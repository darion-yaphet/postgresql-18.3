/*-------------------------------------------------------------------------
 *
 * pg_visibility.c
 *	  display visibility map information and page-level visibility bits
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 *	  contrib/pg_visibility/pg_visibility.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_type.h"
#include "catalog/storage_xlog.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_visibility",
					.version = PG_VERSION
);

typedef struct vbits
{
	BlockNumber next;
	BlockNumber count;
	uint8		bits[FLEXIBLE_ARRAY_MEMBER];
} vbits;

typedef struct corrupt_items
{
	BlockNumber next;
	BlockNumber count;
	ItemPointer tids;
} corrupt_items;

/* for collect_corrupt_items_read_stream_next_block
 *
 * 对于collect_corrupt_items_read_stream_next_block
 */
struct collect_corrupt_items_read_stream_private
{
	bool		all_frozen;
	bool		all_visible;
	BlockNumber current_blocknum;
	BlockNumber last_exclusive;
	Relation	rel;
	Buffer		vmbuffer;
};

PG_FUNCTION_INFO_V1(pg_visibility_map);
PG_FUNCTION_INFO_V1(pg_visibility_map_rel);
PG_FUNCTION_INFO_V1(pg_visibility);
PG_FUNCTION_INFO_V1(pg_visibility_rel);
PG_FUNCTION_INFO_V1(pg_visibility_map_summary);
PG_FUNCTION_INFO_V1(pg_check_frozen);
PG_FUNCTION_INFO_V1(pg_check_visible);
PG_FUNCTION_INFO_V1(pg_truncate_visibility_map);

static TupleDesc pg_visibility_tupdesc(bool include_blkno, bool include_pd);
static vbits *collect_visibility_data(Oid relid, bool include_pd);
static corrupt_items *collect_corrupt_items(Oid relid, bool all_visible,
											bool all_frozen);
static void record_corrupt_item(corrupt_items *items, ItemPointer tid);
static bool tuple_all_visible(HeapTuple tup, TransactionId OldestXmin,
							  Buffer buffer);
static void check_relation_relkind(Relation rel);

/*
 * Visibility map information for a single block of a relation.
 *
 * 关系的单个块的可见性映射信息。
 *
 * Note: the VM code will silently return zeroes for pages past the end
 * of the map, so we allow probes up to MaxBlockNumber regardless of the
 * actual relation size.
 *
 * 注意：VM 代码将默默地为超出映射末尾的页面返回零，因此无论实际关系大小如何，我们都允许探测最多 MaxBlockNumber。
 */
Datum
pg_visibility_map(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	int32		mapbits;
	Relation	rel;
	Buffer		vmbuffer = InvalidBuffer;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map
	 *
	 * 只有部分亲属拥有可见性地图
	 */
	check_relation_relkind(rel);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	tupdesc = pg_visibility_tupdesc(false, false);

	mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	values[0] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0);
	values[1] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0);

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Visibility map information for a single block of a relation, plus the
 * page-level information for the same block.
 *
 * 关系的单个块的可见性映射信息，加上同一块的页面级信息。
 */
Datum
pg_visibility(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	int32		mapbits;
	Relation	rel;
	Buffer		vmbuffer = InvalidBuffer;
	Buffer		buffer;
	Page		page;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {0};

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map
	 *
	 * 只有部分亲属拥有可见性地图
	 */
	check_relation_relkind(rel);

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	tupdesc = pg_visibility_tupdesc(false, true);

	mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	values[0] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0);
	values[1] = BoolGetDatum((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0);

	/* Here we have to explicitly check rel size ...
	 *
	 * 这里我们必须明确检查 rel 大小...
	 */
	if (blkno < RelationGetNumberOfBlocks(rel))
	{
		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		values[2] = BoolGetDatum(PageIsAllVisible(page));

		UnlockReleaseBuffer(buffer);
	}
	else
	{
		/* As with the vismap, silently return 0 for pages past EOF
		 *
		 * 与 vismap 一样，对于 EOF 之后的页面，默默地返回 0
		 */
		values[2] = BoolGetDatum(false);
	}

	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Visibility map information for every block in a relation.
 *
 * 关系中每个块的可见性映射信息。
 */
Datum
pg_visibility_map_rel(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	vbits	   *info;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		funcctx->tuple_desc = pg_visibility_tupdesc(true, false);
		/* collect_visibility_data will verify the relkind
		 *
		 * collect_visibility_data 将验证relkind
		 */
		funcctx->user_fctx = collect_visibility_data(relid, false);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	info = (vbits *) funcctx->user_fctx;

	if (info->next < info->count)
	{
		Datum		values[3];
		bool		nulls[3] = {0};
		HeapTuple	tuple;

		values[0] = Int64GetDatum(info->next);
		values[1] = BoolGetDatum((info->bits[info->next] & (1 << 0)) != 0);
		values[2] = BoolGetDatum((info->bits[info->next] & (1 << 1)) != 0);
		info->next++;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Visibility map information for every block in a relation, plus the page
 * level information for each block.
 *
 * 关系中每个块的可见性映射信息，以及每个块的页面级别信息。
 */
Datum
pg_visibility_rel(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	vbits	   *info;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		funcctx->tuple_desc = pg_visibility_tupdesc(true, true);
		/* collect_visibility_data will verify the relkind
		 *
		 * collect_visibility_data 将验证relkind
		 */
		funcctx->user_fctx = collect_visibility_data(relid, true);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	info = (vbits *) funcctx->user_fctx;

	if (info->next < info->count)
	{
		Datum		values[4];
		bool		nulls[4] = {0};
		HeapTuple	tuple;

		values[0] = Int64GetDatum(info->next);
		values[1] = BoolGetDatum((info->bits[info->next] & (1 << 0)) != 0);
		values[2] = BoolGetDatum((info->bits[info->next] & (1 << 1)) != 0);
		values[3] = BoolGetDatum((info->bits[info->next] & (1 << 2)) != 0);
		info->next++;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Count the number of all-visible and all-frozen pages in the visibility
 * map for a particular relation.
 *
 * 计算特定关系的可见性映射中所有可见和所有冻结页面的数量。
 */
Datum
pg_visibility_map_summary(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	BlockNumber nblocks;
	BlockNumber blkno;
	Buffer		vmbuffer = InvalidBuffer;
	int64		all_visible = 0;
	int64		all_frozen = 0;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map
	 *
	 * 只有部分亲属拥有可见性地图
	 */
	check_relation_relkind(rel);

	nblocks = RelationGetNumberOfBlocks(rel);

	for (blkno = 0; blkno < nblocks; ++blkno)
	{
		int32		mapbits;

		/* Make sure we are interruptible.
		 *
		 * 确保我们不会被打扰。
		 */
		CHECK_FOR_INTERRUPTS();

		/* Get map info.
		 *
		 * 获取地图信息。
		 */
		mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
		if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0)
			++all_visible;
		if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
			++all_frozen;
	}

	/* Clean up.
	 *
	 * 清理。
	 */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	relation_close(rel, AccessShareLock);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values[0] = Int64GetDatum(all_visible);
	values[1] = Int64GetDatum(all_frozen);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Return the TIDs of non-frozen tuples present in pages marked all-frozen
 * in the visibility map.  We hope no one will ever find any, but there could
 * be bugs, database corruption, etc.
 *
 * 返回可见性图中标记为全部冻结的页面中存在的非冻结元组的 TID。  我们希望没有人会发现任何问题，但可能会出现错误、数据库损坏等。
 */
Datum
pg_check_frozen(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	corrupt_items *items;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		/* collect_corrupt_items will verify the relkind
		 *
		 * collect_corrupt_items 将验证relkind
		 */
		funcctx->user_fctx = collect_corrupt_items(relid, false, true);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	items = (corrupt_items *) funcctx->user_fctx;

	if (items->next < items->count)
		SRF_RETURN_NEXT(funcctx, PointerGetDatum(&items->tids[items->next++]));

	SRF_RETURN_DONE(funcctx);
}

/*
 * Return the TIDs of not-all-visible tuples in pages marked all-visible
 * in the visibility map.  We hope no one will ever find any, but there could
 * be bugs, database corruption, etc.
 *
 * 返回可见性图中标记为全部可见的页面中非全部可见元组的 TID。  我们希望没有人会发现任何问题，但可能会出现错误、数据库损坏等。
 */
Datum
pg_check_visible(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	corrupt_items *items;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		/* collect_corrupt_items will verify the relkind
		 *
		 * collect_corrupt_items 将验证relkind
		 */
		funcctx->user_fctx = collect_corrupt_items(relid, true, false);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	items = (corrupt_items *) funcctx->user_fctx;

	if (items->next < items->count)
		SRF_RETURN_NEXT(funcctx, PointerGetDatum(&items->tids[items->next++]));

	SRF_RETURN_DONE(funcctx);
}

/*
 * Remove the visibility map fork for a relation.  If there turn out to be
 * any bugs in the visibility map code that require rebuilding the VM, this
 * provides users with a way to do it that is cleaner than shutting down the
 * server and removing files by hand.
 *
 * 删除关系的可见性映射分支。  如果可见性地图代码中存在任何需要重建虚拟机的错误，这为用户提供了一种比关闭服务器并手动删除文件更干净的方法。
 *
 * This is a cut-down version of RelationTruncate.
 *
 * 这是 RelationTruncate 的精简版本。
 */
Datum
pg_truncate_visibility_map(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	ForkNumber	fork;
	BlockNumber block;
	BlockNumber old_block;

	rel = relation_open(relid, AccessExclusiveLock);

	/* Only some relkinds have a visibility map
	 *
	 * 只有部分亲属拥有可见性地图
	 */
	check_relation_relkind(rel);

	/* Forcibly reset cached file size
	 *
	 * 强制重置缓存文件大小
	 */
	RelationGetSmgr(rel)->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = InvalidBlockNumber;

	/* Compute new and old size before entering critical section.
	 *
	 * 在进入临界区之前计算新旧大小。
	 */
	fork = VISIBILITYMAP_FORKNUM;
	block = visibilitymap_prepare_truncate(rel, 0);
	old_block = BlockNumberIsValid(block) ? smgrnblocks(RelationGetSmgr(rel), fork) : 0;

	/*
	 * WAL-logging, buffer dropping, file truncation must be atomic and all on
	 * one side of a checkpoint.  See RelationTruncate() for discussion.
	 *
	 * WAL 日志记录、缓冲区删除、文件截断必须是原子的，并且全部位于检查点的一侧。  请参阅 RelationTruncate() 进行讨论。
	 */
	Assert((MyProc->delayChkptFlags & (DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE)) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE;
	START_CRIT_SECTION();

	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	lsn;
		xl_smgr_truncate xlrec;

		xlrec.blkno = 0;
		xlrec.rlocator = rel->rd_locator;
		xlrec.flags = SMGR_TRUNCATE_VM;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));

		lsn = XLogInsert(RM_SMGR_ID,
						 XLOG_SMGR_TRUNCATE | XLR_SPECIAL_REL_UPDATE);
		XLogFlush(lsn);
	}

	if (BlockNumberIsValid(block))
		smgrtruncate(RelationGetSmgr(rel), &fork, 1, &old_block, &block);

	END_CRIT_SECTION();
	MyProc->delayChkptFlags &= ~(DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE);

	/*
	 * Release the lock right away, not at commit time.
	 *
	 * 立即释放锁，而不是在提交时释放锁。
	 *
	 * It would be a problem to release the lock prior to commit if this
	 * truncate operation sends any transactional invalidation messages. Other
	 * backends would potentially be able to lock the relation without
	 * processing them in the window of time between when we release the lock
	 * here and when we sent the messages at our eventual commit.  However,
	 * we're currently only sending a non-transactional smgr invalidation,
	 * which will have been posted to shared memory immediately from within
	 * smgr_truncate.  Therefore, there should be no race here.
	 *
	 * 如果此截断操作发送任何事务无效消息，那么在提交之前释放锁将会出现问题。其他后端可能能够锁定关系，而无需在我们释放锁和最终提交时发送消息之间的时间窗口内处理它们。  但是，我们目前仅发送非事务性 smgr 失效，该失效将立即从 smgr_truncate 内发布到共享内存。  因此，这里不应该有比赛。
	 *
	 * The reason why it's desirable to release the lock early here is because
	 * of the possibility that someone will need to use this to blow away many
	 * visibility map forks at once.  If we can't release the lock until
	 * commit time, the transaction doing this will accumulate
	 * AccessExclusiveLocks on all of those relations at the same time, which
	 * is undesirable. However, if this turns out to be unsafe we may have no
	 * choice...
	 *
	 * 之所以希望尽早释放锁，是因为有人可能需要使用它来一次消除许多可见性地图分叉。  如果我们在提交之前无法释放锁，那么执行此操作的事务将同时在所有这些关系上累积 AccessExclusiveLock，这是不希望的。然而，如果事实证明这不安全，我们可能别无选择……
	 */
	relation_close(rel, AccessExclusiveLock);

	/* Nothing to return.
	 *
	 * 没有什么可返回的。
	 */
	PG_RETURN_VOID();
}

/*
 * Helper function to construct whichever TupleDesc we need for a particular
 * call.
 *
 * 用于构造特定调用所需的 TupleDesc 的辅助函数。
 */
static TupleDesc
pg_visibility_tupdesc(bool include_blkno, bool include_pd)
{
	TupleDesc	tupdesc;
	AttrNumber	maxattr = 2;
	AttrNumber	a = 0;

	if (include_blkno)
		++maxattr;
	if (include_pd)
		++maxattr;
	tupdesc = CreateTemplateTupleDesc(maxattr);
	if (include_blkno)
		TupleDescInitEntry(tupdesc, ++a, "blkno", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "all_visible", BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "all_frozen", BOOLOID, -1, 0);
	if (include_pd)
		TupleDescInitEntry(tupdesc, ++a, "pd_all_visible", BOOLOID, -1, 0);
	Assert(a == maxattr);

	return BlessTupleDesc(tupdesc);
}

/*
 * Collect visibility data about a relation.
 *
 * 收集有关关系的可见性数据。
 *
 * Checks relkind of relid and will throw an error if the relation does not
 * have a VM.
 *
 * 检查 relkind 的 relkind，如果关系没有 VM，则会抛出错误。
 */
static vbits *
collect_visibility_data(Oid relid, bool include_pd)
{
	Relation	rel;
	BlockNumber nblocks;
	vbits	   *info;
	BlockNumber blkno;
	Buffer		vmbuffer = InvalidBuffer;
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);
	BlockRangeReadStreamPrivate p;
	ReadStream *stream = NULL;

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map
	 *
	 * 只有部分亲属拥有可见性地图
	 */
	check_relation_relkind(rel);

	nblocks = RelationGetNumberOfBlocks(rel);
	info = palloc0(offsetof(vbits, bits) + nblocks);
	info->next = 0;
	info->count = nblocks;

	/* Create a stream if reading main fork.
	 *
	 * 如果读取主分支，则创建一个流。
	 */
	if (include_pd)
	{
		p.current_blocknum = 0;
		p.last_exclusive = nblocks;

		/*
		 * It is safe to use batchmode as block_range_read_stream_cb takes no
		 * locks.
		 *
		 * 使用批处理模式是安全的，因为 block_range_read_stream_cb 不加锁。
		 */
		stream = read_stream_begin_relation(READ_STREAM_FULL |
											READ_STREAM_USE_BATCHING,
											bstrategy,
											rel,
											MAIN_FORKNUM,
											block_range_read_stream_cb,
											&p,
											0);
	}

	for (blkno = 0; blkno < nblocks; ++blkno)
	{
		int32		mapbits;

		/* Make sure we are interruptible.
		 *
		 * 确保我们不会被打扰。
		 */
		CHECK_FOR_INTERRUPTS();

		/* Get map info.
		 *
		 * 获取地图信息。
		 */
		mapbits = (int32) visibilitymap_get_status(rel, blkno, &vmbuffer);
		if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0)
			info->bits[blkno] |= (1 << 0);
		if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
			info->bits[blkno] |= (1 << 1);

		/*
		 * Page-level data requires reading every block, so only get it if the
		 * caller needs it.  Use a buffer access strategy, too, to prevent
		 * cache-trashing.
		 *
		 * 页级数据需要读取每个块，因此仅在调用者需要时才获取。  还可以使用缓冲区访问策略来防止缓存损坏。
		 */
		if (include_pd)
		{
			Buffer		buffer;
			Page		page;

			buffer = read_stream_next_buffer(stream, NULL);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);

			page = BufferGetPage(buffer);
			if (PageIsAllVisible(page))
				info->bits[blkno] |= (1 << 2);

			UnlockReleaseBuffer(buffer);
		}
	}

	if (include_pd)
	{
		Assert(read_stream_next_buffer(stream, NULL) == InvalidBuffer);
		read_stream_end(stream);
	}

	/* Clean up.
	 *
	 * 清理。
	 */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	relation_close(rel, AccessShareLock);

	return info;
}

/*
 * The "strict" version of GetOldestNonRemovableTransactionId().  The
 * pg_visibility check can tolerate false positives (don't report some of the
 * errors), but can't tolerate false negatives (report false errors). Normally,
 * horizons move forwards, but there are cases when it could move backward
 * (see comment for ComputeXidHorizons()).
 *
 * GetOldestNonRemovableTransactionId() 的“严格”版本。  pg_visibility 检查可以容忍误报（不报告某些错误），但不能容忍误报（报告错误错误）。通常，地平线向前移动，但在某些情况下它可能会向后移动（请参阅 ComputeXidHorizo​​ns() 的注释）。
 *
 * This is why we have to implement our own function for xid horizon, which
 * would be guaranteed to be newer or equal to any xid horizon computed before.
 * We have to do the following to achieve this.
 *
 * 这就是为什么我们必须为 xid Horizo​​n 实现我们自己的函数，这将保证更新或等于之前计算的任何 xid Horizo​​n。为了实现这一目标，我们必须做到以下几点。
 *
 * 1. Ignore processes xmin's, because they consider connection to other
 *    databases that were ignored before.
 * 2. Ignore KnownAssignedXids, as they are not database-aware. Although we
 *    now perform minimal checking on a standby by always using nextXid, this
 *    approach is better than nothing and will at least catch extremely broken
 *    cases where a xid is in the future.
 * 3. Ignore walsender xmin, because it could go backward if some replication
 *    connections don't use replication slots.
 *
 * 1. 忽略xmin的进程，因为它们考虑与之前被忽略的其他数据库的连接。 2. 忽略 KnownAssignedXids，因为它们不支持数据库。尽管我们现在总是使用 nextXid 对备用数据库执行最少的检查，但这种方法总比没有好，并且至少会捕获将来 xid 出现的极其损坏的情况。 3. 忽略 walsender xmin，因为如果某些复制连接不使用复制槽，它可能会向后移动。
 *
 * While it might seem like we could use KnownAssignedXids for shared
 * catalogs, since shared catalogs rely on a global horizon rather than a
 * database-specific one - there are potential edge cases.  For example, a
 * transaction may crash on the primary without writing a commit/abort record.
 * This would lead to a situation where it appears to still be running on the
 * standby, even though it has already ended on the primary.  For this reason,
 * it's safer to ignore KnownAssignedXids, even for shared catalogs.
 *
 * 虽然我们似乎可以将 KnownAssignedXids 用于共享目录，但由于共享目录依赖于全局范围而不是特定于数据库的范围，因此存在潜在的边缘情况。  例如，事务可能在未写入提交/中止记录的情况下在主服务器上崩溃。这将导致这样的情况：尽管它已经在主数据库上结束，但它似乎仍在备用数据库上运行。  因此，忽略 KnownAssignedXids 更安全，即使对于共享目录也是如此。
 *
 * As a result, we're using only currently running xids to compute the horizon.
 * Surely these would significantly sacrifice accuracy.  But we have to do so
 * to avoid reporting false errors.
 *
 * 因此，我们仅使用当前运行的 xids 来计算范围。当然，这些会大大牺牲准确性。  但我们必须这样做以避免报告虚假错误。
 */
static TransactionId
GetStrictOldestNonRemovableTransactionId(Relation rel)
{
	RunningTransactions runningTransactions;

	if (RecoveryInProgress())
	{
		TransactionId result;

		/* As we ignore KnownAssignedXids on standby, just pick nextXid
		 *
		 * 由于我们在待机时忽略 KnownAssignedXids，因此只需选择 nextXid
		 */
		LWLockAcquire(XidGenLock, LW_SHARED);
		result = XidFromFullTransactionId(TransamVariables->nextXid);
		LWLockRelease(XidGenLock);
		return result;
	}
	else if (rel == NULL || rel->rd_rel->relisshared)
	{
		/* Shared relation: take into account all running xids
		 *
		 * 共享关系：考虑所有正在运行的 xids
		 */
		runningTransactions = GetRunningTransactionData();
		LWLockRelease(ProcArrayLock);
		LWLockRelease(XidGenLock);
		return runningTransactions->oldestRunningXid;
	}
	else if (!RELATION_IS_LOCAL(rel))
	{
		/*
		 * Normal relation: take into account xids running within the current
		 * database
		 *
		 * 正常关系：考虑当前数据库中运行的 xids
		 */
		runningTransactions = GetRunningTransactionData();
		LWLockRelease(ProcArrayLock);
		LWLockRelease(XidGenLock);
		return runningTransactions->oldestDatabaseRunningXid;
	}
	else
	{
		/*
		 * For temporary relations, ComputeXidHorizons() uses only
		 * TransamVariables->latestCompletedXid and MyProc->xid.  These two
		 * shouldn't go backwards.  So we're fine with this horizon.
		 *
		 * 对于临时关系，ComputeXidHorizo​​ns() 仅使用 TransamVariables->latestCompletedXid 和 MyProc->xid。  这两个人不该走回头路。  所以我们对这个地平线很满意。
		 */
		return GetOldestNonRemovableTransactionId(rel);
	}
}

/*
 * Callback function to get next block for read stream object used in
 * collect_corrupt_items() function.
 *
 * 用于获取在collect_corrupt_items()函数中使用的读取流对象的下一个块的回调函数。
 */
static BlockNumber
collect_corrupt_items_read_stream_next_block(ReadStream *stream,
											 void *callback_private_data,
											 void *per_buffer_data)
{
	struct collect_corrupt_items_read_stream_private *p = callback_private_data;

	for (; p->current_blocknum < p->last_exclusive; p->current_blocknum++)
	{
		bool		check_frozen = false;
		bool		check_visible = false;

		/* Make sure we are interruptible.
		 *
		 * 确保我们不会被打扰。
		 */
		CHECK_FOR_INTERRUPTS();

		if (p->all_frozen && VM_ALL_FROZEN(p->rel, p->current_blocknum, &p->vmbuffer))
			check_frozen = true;
		if (p->all_visible && VM_ALL_VISIBLE(p->rel, p->current_blocknum, &p->vmbuffer))
			check_visible = true;
		if (!check_visible && !check_frozen)
			continue;

		return p->current_blocknum++;
	}

	return InvalidBlockNumber;
}

/*
 * Returns a list of items whose visibility map information does not match
 * the status of the tuples on the page.
 *
 * 返回可见性图信息与页面上元组的状态不匹配的项目列表。
 *
 * If all_visible is passed as true, this will include all items which are
 * on pages marked as all-visible in the visibility map but which do not
 * seem to in fact be all-visible.
 *
 * 如果将 all_visible 传递为 true，则这将包括在可见性映射中标记为全部可见但实际上似乎并非全部可见的页面上的所有项目。
 *
 * If all_frozen is passed as true, this will include all items which are
 * on pages marked as all-frozen but which do not seem to in fact be frozen.
 *
 * 如果将 all_frozen 传递为 true，则这将包括标记为全冻结但实际上似乎并未冻结的页面上的所有项目。
 *
 * Checks relkind of relid and will throw an error if the relation does not
 * have a VM.
 *
 * 检查 relkind 的 relkind，如果关系没有 VM，则会抛出错误。
 */
static corrupt_items *
collect_corrupt_items(Oid relid, bool all_visible, bool all_frozen)
{
	Relation	rel;
	corrupt_items *items;
	Buffer		vmbuffer = InvalidBuffer;
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);
	TransactionId OldestXmin = InvalidTransactionId;
	struct collect_corrupt_items_read_stream_private p;
	ReadStream *stream;
	Buffer		buffer;

	rel = relation_open(relid, AccessShareLock);

	/* Only some relkinds have a visibility map
	 *
	 * 只有部分亲属拥有可见性地图
	 */
	check_relation_relkind(rel);

	if (all_visible)
		OldestXmin = GetStrictOldestNonRemovableTransactionId(rel);

	/*
	 * Guess an initial array size. We don't expect many corrupted tuples, so
	 * start with a small array.  This function uses the "next" field to track
	 * the next offset where we can store an item (which is the same thing as
	 * the number of items found so far) and the "count" field to track the
	 * number of entries allocated.  We'll repurpose these fields before
	 * returning.
	 *
	 * 猜测初始数组大小。我们预计不会有很多损坏的元组，因此从一个小数组开始。  该函数使用“next”字段来跟踪我们可以存储项目的下一个偏移量（与到目前为止找到的项目数相同），并使用“count”字段来跟踪分配的条目数。  我们将在返回之前重新调整这些字段的用途。
	 */
	items = palloc0(sizeof(corrupt_items));
	items->next = 0;
	items->count = 64;
	items->tids = palloc(items->count * sizeof(ItemPointerData));

	p.current_blocknum = 0;
	p.last_exclusive = RelationGetNumberOfBlocks(rel);
	p.rel = rel;
	p.vmbuffer = InvalidBuffer;
	p.all_frozen = all_frozen;
	p.all_visible = all_visible;
	stream = read_stream_begin_relation(READ_STREAM_FULL,
										bstrategy,
										rel,
										MAIN_FORKNUM,
										collect_corrupt_items_read_stream_next_block,
										&p,
										0);

	/* Loop over every block in the relation.
	 *
	 * 循环关系中的每个块。
	 */
	while ((buffer = read_stream_next_buffer(stream, NULL)) != InvalidBuffer)
	{
		bool		check_frozen = all_frozen;
		bool		check_visible = all_visible;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		BlockNumber blkno;

		/* Make sure we are interruptible.
		 *
		 * 确保我们不会被打扰。
		 */
		CHECK_FOR_INTERRUPTS();

		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		maxoff = PageGetMaxOffsetNumber(page);
		blkno = BufferGetBlockNumber(buffer);

		/*
		 * The visibility map bits might have changed while we were acquiring
		 * the page lock.  Recheck to avoid returning spurious results.
		 *
		 * 当我们获取页面锁定时，可见性映射位可能已更改。  重新检查以避免返回虚假结果。
		 */
		if (check_frozen && !VM_ALL_FROZEN(rel, blkno, &vmbuffer))
			check_frozen = false;
		if (check_visible && !VM_ALL_VISIBLE(rel, blkno, &vmbuffer))
			check_visible = false;
		if (!check_visible && !check_frozen)
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}

		/* Iterate over each tuple on the page.
		 *
		 * 迭代页面上的每个元组。
		 */
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			HeapTupleData tuple;
			ItemId		itemid;

			itemid = PageGetItemId(page, offnum);

			/* Unused or redirect line pointers are of no interest.
			 *
			 * 未使用或重定向的行指针没有意义。
			 */
			if (!ItemIdIsUsed(itemid) || ItemIdIsRedirected(itemid))
				continue;

			/* Dead line pointers are neither all-visible nor frozen.
			 *
			 * 截止线指针既不是全部可见，也不是冻结的。
			 */
			if (ItemIdIsDead(itemid))
			{
				ItemPointerSet(&(tuple.t_self), blkno, offnum);
				record_corrupt_item(items, &tuple.t_self);
				continue;
			}

			/* Initialize a HeapTupleData structure for checks below.
			 *
			 * 初始化 HeapTupleData 结构以进行下面的检查。
			 */
			ItemPointerSet(&(tuple.t_self), blkno, offnum);
			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			tuple.t_tableOid = relid;

			/*
			 * If we're checking whether the page is all-visible, we expect
			 * the tuple to be all-visible.
			 *
			 * 如果我们检查页面是否是所有可见的，我们期望元组是所有可见的。
			 */
			if (check_visible &&
				!tuple_all_visible(&tuple, OldestXmin, buffer))
			{
				TransactionId RecomputedOldestXmin;

				/*
				 * Time has passed since we computed OldestXmin, so it's
				 * possible that this tuple is all-visible in reality even
				 * though it doesn't appear so based on our
				 * previously-computed value.  Let's compute a new value so we
				 * can be certain whether there is a problem.
				 *
				 * 自从我们计算 OldestXmin 以来，时间已经过去了，所以这个元组在现实中可能是完全可见的，尽管根据我们之前计算的值它看起来并不那么明显。  让我们计算一个新值，以便确定是否存在问题。
				 *
				 * From a concurrency point of view, it sort of sucks to
				 * retake ProcArrayLock here while we're holding the buffer
				 * exclusively locked, but it should be safe against
				 * deadlocks, because surely
				 * GetStrictOldestNonRemovableTransactionId() should never
				 * take a buffer lock. And this shouldn't happen often, so
				 * it's worth being careful so as to avoid false positives.
				 *
				 * 从并发的角度来看，当我们以独占方式锁定缓冲区时，在这里重新获取 ProcArrayLock 有点糟糕，但它应该可以安全地防止死锁，因为 GetStrictOldestNonRemovableTransactionId() 肯定永远不应该获取缓冲区锁。这种情况不应该经常发生，因此值得小心以避免误报。
				 */
				RecomputedOldestXmin = GetStrictOldestNonRemovableTransactionId(rel);

				if (!TransactionIdPrecedes(OldestXmin, RecomputedOldestXmin))
					record_corrupt_item(items, &tuple.t_self);
				else
				{
					OldestXmin = RecomputedOldestXmin;
					if (!tuple_all_visible(&tuple, OldestXmin, buffer))
						record_corrupt_item(items, &tuple.t_self);
				}
			}

			/*
			 * If we're checking whether the page is all-frozen, we expect the
			 * tuple to be in a state where it will never need freezing.
			 *
			 * 如果我们检查页面是否全部冻结，我们期望元组处于永远不需要冻结的状态。
			 */
			if (check_frozen)
			{
				if (heap_tuple_needs_eventual_freeze(tuple.t_data))
					record_corrupt_item(items, &tuple.t_self);
			}
		}

		UnlockReleaseBuffer(buffer);
	}
	read_stream_end(stream);

	/* Clean up.
	 *
	 * 清理。
	 */
	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	if (p.vmbuffer != InvalidBuffer)
		ReleaseBuffer(p.vmbuffer);
	relation_close(rel, AccessShareLock);

	/*
	 * Before returning, repurpose the fields to match caller's expectations.
	 * next is now the next item that should be read (rather than written) and
	 * count is now the number of items we wrote (rather than the number we
	 * allocated).
	 *
	 * 返回之前，重新调整字段的用途以满足调用者的期望。 next 现在是应该读取（而不是写入）的下一个项目，而 count 现在是我们写入的项目数（而不是我们分配的数量）。
	 */
	items->count = items->next;
	items->next = 0;

	return items;
}

/*
 * Remember one corrupt item.
 *
 * 记住一件损坏的物品。
 */
static void
record_corrupt_item(corrupt_items *items, ItemPointer tid)
{
	/* enlarge output array if needed.
	 *
	 * 如果需要，扩大输出数组。
	 */
	if (items->next >= items->count)
	{
		items->count *= 2;
		items->tids = repalloc(items->tids,
							   items->count * sizeof(ItemPointerData));
	}
	/* and add the new item
	 *
	 * 并添加新项目
	 */
	items->tids[items->next++] = *tid;
}

/*
 * Check whether a tuple is all-visible relative to a given OldestXmin value.
 * The buffer should contain the tuple and should be locked and pinned.
 *
 * 检查元组相对于给定的 OldestXmin 值是否全部可见。缓冲区应该包含元组并且应该被锁定和固定。
 */
static bool
tuple_all_visible(HeapTuple tup, TransactionId OldestXmin, Buffer buffer)
{
	HTSV_Result state;
	TransactionId xmin;

	state = HeapTupleSatisfiesVacuum(tup, OldestXmin, buffer);
	if (state != HEAPTUPLE_LIVE)
		return false;			/* all-visible implies live */

	/*
	 * Neither lazy_scan_heap nor heap_page_is_all_visible will mark a page
	 * all-visible unless every tuple is hinted committed. However, those hint
	 * bits could be lost after a crash, so we can't be certain that they'll
	 * be set here.  So just check the xmin.
	 *
	 * 无论是lazy_scan_heap还是heap_page_is_all_visible都不会将页面标记为全部可见，除非每个元组都被提示提交。但是，这些提示位可能会在崩溃后丢失，因此我们无法确定它们是否会在此处设置。  所以只需检查 xmin 即可。
	 */

	xmin = HeapTupleHeaderGetXmin(tup->t_data);
	if (!TransactionIdPrecedes(xmin, OldestXmin))
		return false;			/* xmin not old enough for all to see */

	return true;
}

/*
 * check_relation_relkind - convenience routine to check that relation
 * is of the relkind supported by the callers
 *
 * check_relation_relkind - 检查关系是否属于调用者支持的relkind的便捷例程
 */
static void
check_relation_relkind(Relation rel)
{
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is of wrong relation kind",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
}
