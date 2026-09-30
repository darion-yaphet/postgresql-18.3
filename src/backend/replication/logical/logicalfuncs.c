/*-------------------------------------------------------------------------
 *
 * logicalfuncs.c
 *
 *	   Support functions for using logical decoding and management of
 *	   logical replication slots via SQL.
 *
 * 通过 SQL 使用逻辑解码和管理逻辑复制槽的支持函数。
 *
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/logicalfuncs.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/regproc.h"
#include "utils/resowner.h"

/* Private data for writing out data
 *
 * 写出数据时使用的私有状态。
 */
typedef struct DecodingOutputState
{
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	bool		binary_output;
	int64		returned_rows;
} DecodingOutputState;

/*
 * Prepare for an output plugin write.
 *
 * 为输出插件的一次写出做准备。
 */
static void
LogicalOutputPrepareWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
						  bool last_write)
{
	resetStringInfo(ctx->out);
}

/*
 * Perform output plugin write into tuplestore.
 *
 * 把输出插件写出的内容放入 tuplestore。
 */
static void
LogicalOutputWrite(LogicalDecodingContext *ctx, XLogRecPtr lsn, TransactionId xid,
				   bool last_write)
{
	Datum		values[3];
	bool		nulls[3];
	DecodingOutputState *p;

	/* SQL Datums can only be of a limited length...
	 *
	 * SQL Datum 的长度有上限。
	 */
	if (ctx->out->len > MaxAllocSize - VARHDRSZ)
		elog(ERROR, "too much output for sql interface");

	p = (DecodingOutputState *) ctx->output_writer_private;

	memset(nulls, 0, sizeof(nulls));
	values[0] = LSNGetDatum(lsn);
	values[1] = TransactionIdGetDatum(xid);

	/*
	 * Assert ctx->out is in database encoding when we're writing textual
	 * output.
	 *
	 * 写出文本时，断言 ctx 的 out 使用数据库编码。
	 */
	if (!p->binary_output)
		Assert(pg_verify_mbstr(GetDatabaseEncoding(),
							   ctx->out->data, ctx->out->len,
							   false));

	/* ick, but cstring_to_text_with_len works for bytea perfectly fine
	 *
	 * 虽不雅观，但 cstring_to_text_with_len 对 bytea 完全适用。
	 */
	values[2] = PointerGetDatum(cstring_to_text_with_len(ctx->out->data, ctx->out->len));

	tuplestore_putvalues(p->tupstore, p->tupdesc, values, nulls);
	p->returned_rows++;
}

/*
 * 核心流程：SQL 入口进入 pg_logical_slot_get_changes_guts，取得复制槽、解码 WAL，再把变更写入 tuplestore。
 */

/*
 * Helper function for the various SQL callable logical decoding functions.
 *
 * 各个可从 SQL 调用的逻辑解码函数的共用实现。
 */
static Datum
pg_logical_slot_get_changes_guts(FunctionCallInfo fcinfo, bool confirm, bool binary)
{
	Name		name;
	XLogRecPtr	upto_lsn;
	int32		upto_nchanges;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	XLogRecPtr	end_of_wal;
	XLogRecPtr	wait_for_wal_lsn;
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	ArrayType  *arr;
	Size		ndim;
	List	   *options = NIL;
	DecodingOutputState *p;

	CheckSlotPermissions();

	CheckLogicalDecodingRequirements();

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("slot name must not be null")));
	name = PG_GETARG_NAME(0);

	if (PG_ARGISNULL(1))
		upto_lsn = InvalidXLogRecPtr;
	else
		upto_lsn = PG_GETARG_LSN(1);

	if (PG_ARGISNULL(2))
		upto_nchanges = InvalidXLogRecPtr;
	else
		upto_nchanges = PG_GETARG_INT32(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("options array must not be null")));
	arr = PG_GETARG_ARRAYTYPE_P(3);

	/* state to write output to
	 *
	 * 写出结果用的状态。
	 */
	p = palloc0(sizeof(DecodingOutputState));

	p->binary_output = binary;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Deconstruct options array
	 *
	 * 拆开选项数组。
	 */
	ndim = ARR_NDIM(arr);
	if (ndim > 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("array must be one-dimensional")));
	}
	else if (array_contains_nulls(arr))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("array must not contain nulls")));
	}
	else if (ndim == 1)
	{
		int			nelems;
		Datum	   *datum_opts;
		int			i;

		Assert(ARR_ELEMTYPE(arr) == TEXTOID);

		deconstruct_array_builtin(arr, TEXTOID, &datum_opts, NULL, &nelems);

		if (nelems % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array must have even number of elements")));

		for (i = 0; i < nelems; i += 2)
		{
			char	   *optname = TextDatumGetCString(datum_opts[i]);
			char	   *opt = TextDatumGetCString(datum_opts[i + 1]);

			options = lappend(options, makeDefElem(optname, (Node *) makeString(opt), -1));
		}
	}

	InitMaterializedSRF(fcinfo, 0);
	p->tupstore = rsinfo->setResult;
	p->tupdesc = rsinfo->setDesc;

	/*
	 * Compute the current end-of-wal.
	 *
	 * 计算当前的 WAL 末端。
	 */
	if (!RecoveryInProgress())
		end_of_wal = GetFlushRecPtr(NULL);
	else
		end_of_wal = GetXLogReplayRecPtr(NULL);

	ReplicationSlotAcquire(NameStr(*name), true, true);

	PG_TRY();
	{
		/* restart at slot's confirmed_flush
		 *
		 * 从复制槽的 confirmed_flush 处重新开始。
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									options,
									false,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									LogicalOutputPrepareWrite,
									LogicalOutputWrite, NULL);

		MemoryContextSwitchTo(oldcontext);

		/*
		 * Check whether the output plugin writes textual output if that's
		 * what we need.
		 *
		 * 若调用方需要文本输出，则检查输出插件是否真的写文本。
		 */
		if (!binary &&
			ctx->options.output_type !=OUTPUT_PLUGIN_TEXTUAL_OUTPUT)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("logical decoding output plugin \"%s\" produces binary output, but function \"%s\" expects textual data",
							NameStr(MyReplicationSlot->data.plugin),
							format_procedure(fcinfo->flinfo->fn_oid))));

		/*
		 * Wait for specified streaming replication standby servers (if any)
		 * to confirm receipt of WAL up to wait_for_wal_lsn.
		 *
		 * 若指定了流复制备库，则等待它们确认已收到直到 wait_for_wal_lsn 的 WAL。
		 */
		if (XLogRecPtrIsInvalid(upto_lsn))
			wait_for_wal_lsn = end_of_wal;
		else
			wait_for_wal_lsn = Min(upto_lsn, end_of_wal);

		WaitForStandbyConfirmation(wait_for_wal_lsn);

		ctx->output_writer_private = p;

		/*
		 * Decoding of WAL must start at restart_lsn so that the entirety of
		 * xacts that committed after the slot's confirmed_flush can be
		 * accumulated into reorder buffers.
		 *
		 * 解码必须从 restart_lsn 开始，这样在复制槽 confirmed_flush 之后提交的事务才能完整进入重排缓冲区。
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* invalidate non-timetravel entries
		 *
		 * 使非时间旅行的缓存项失效。
		 */
		InvalidateSystemCaches();

		/* Decode until we run out of records
		 *
		 * 一直解码到没有更多记录。
		 */
		while (ctx->reader->EndRecPtr < end_of_wal)
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "could not find record for logical decoding: %s", errm);

			/*
			 * The {begin_txn,change,commit_txn}_wrapper callbacks above will
			 * store the description into our tuplestore.
			 *
			 * 上面的 begin_txn、change、commit_txn 包装回调会把描述写入 tuplestore。
			 */
			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			/* check limits
			 *
			 * 检查条数等限制。
			 */
			if (upto_lsn != InvalidXLogRecPtr &&
				upto_lsn <= ctx->reader->EndRecPtr)
				break;
			if (upto_nchanges != 0 &&
				upto_nchanges <= p->returned_rows)
				break;
			CHECK_FOR_INTERRUPTS();
		}

		/*
		 * Logical decoding could have clobbered CurrentResourceOwner during
		 * transaction management, so restore the executor's value.  (This is
		 * a kluge, but it's not worth cleaning up right now.)
		 *
		 * 逻辑解码在事务管理期间可能改写 CurrentResourceOwner，因此恢复执行器原来的值。
		 * 这是权宜之计，眼下不值得专门清理。
		 */
		CurrentResourceOwner = old_resowner;

		/*
		 * Next time, start where we left off. (Hunting things, the family
		 * business..)
		 *
		 * 下次从上次停下的位置继续。
		 */
		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr && confirm)
		{
			LogicalConfirmReceivedLocation(ctx->reader->EndRecPtr);

			/*
			 * If only the confirmed_flush_lsn has changed the slot won't get
			 * marked as dirty by the above. Callers on the walsender
			 * interface are expected to keep track of their own progress and
			 * don't need it written out. But SQL-interface users cannot
			 * specify their own start positions and it's harder for them to
			 * keep track of their progress, so we should make more of an
			 * effort to save it for them.
			 *
			 * 若只改了 confirmed_flush_lsn，上面的操作不会把复制槽标为脏。
			 * walsender 接口的调用者应自己跟踪进度，不必写出该位置。
			 * SQL 接口的用户不能指定自己的起点，也更难跟踪进度，因此更应帮他们保存。
			 *
			 * Dirty the slot so it's written out at the next checkpoint.
			 * We'll still lose its position on crash, as documented, but it's
			 * better than always losing the position even on clean restart.
			 *
			 * 把复制槽标脏，以便下次检查点写出。崩溃时仍会丢掉位置，文档已说明；
			 * 但这好过即使干净重启也总是丢掉位置。
			 */
			ReplicationSlotMarkDirty();
		}

		/* free context, call shutdown callback
		 *
		 * 释放解码上下文，并调用关闭回调。
		 */
		FreeDecodingContext(ctx);

		ReplicationSlotRelease();
		InvalidateSystemCaches();
	}
	PG_CATCH();
	{
		/* clear all timetravel entries
		 *
		 * 清除全部时间旅行缓存项。
		 */
		InvalidateSystemCaches();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return (Datum) 0;
}

/*
 * SQL function returning the changestream as text, consuming the data.
 *
 * SQL 函数：以文本返回变更流，并消费这些数据。
 */
Datum
pg_logical_slot_get_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, true, false);
}

/*
 * SQL function returning the changestream as text, only peeking ahead.
 *
 * SQL 函数：以文本返回变更流，只向前查看，不消费。
 */
Datum
pg_logical_slot_peek_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, false, false);
}

/*
 * SQL function returning the changestream in binary, consuming the data.
 *
 * SQL 函数：以二进制返回变更流，并消费这些数据。
 */
Datum
pg_logical_slot_get_binary_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, true, true);
}

/*
 * SQL function returning the changestream in binary, only peeking ahead.
 *
 * SQL 函数：以二进制返回变更流，只向前查看，不消费。
 */
Datum
pg_logical_slot_peek_binary_changes(PG_FUNCTION_ARGS)
{
	return pg_logical_slot_get_changes_guts(fcinfo, false, true);
}


/*
 * SQL function for writing logical decoding message into WAL.
 *
 * SQL 函数：把逻辑解码消息写入 WAL。
 */
Datum
pg_logical_emit_message_bytea(PG_FUNCTION_ARGS)
{
	bool		transactional = PG_GETARG_BOOL(0);
	char	   *prefix = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bytea	   *data = PG_GETARG_BYTEA_PP(2);
	bool		flush = PG_GETARG_BOOL(3);
	XLogRecPtr	lsn;

	lsn = LogLogicalMessage(prefix, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data),
							transactional, flush);
	PG_RETURN_LSN(lsn);
}

/*
 * 把 text 负载交给 pg_logical_emit_message_bytea，写入 WAL。
 */
Datum
pg_logical_emit_message_text(PG_FUNCTION_ARGS)
{
	/* bytea and text are compatible
	 *
	 * bytea 与 text 在这里兼容。
	 */
	return pg_logical_emit_message_bytea(fcinfo);
}
