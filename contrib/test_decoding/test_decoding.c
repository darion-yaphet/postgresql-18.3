/*-------------------------------------------------------------------------
 *
 * test_decoding.c
 *		  example logical decoding output plugin
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/test_decoding/test_decoding.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"

#include "replication/logical.h"
#include "replication/origin.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_decoding",
					.version = PG_VERSION
);

typedef struct
{
	MemoryContext context;
	bool		include_xids;
	bool		include_timestamp;
	bool		skip_empty_xacts;
	bool		only_local;
} TestDecodingData;

/*
 * Maintain the per-transaction level variables to track whether the
 * transaction and or streams have written any changes. In streaming mode the
 * transaction can be decoded in streams so along with maintaining whether the
 * transaction has written any changes, we also need to track whether the
 * current stream has written any changes. This is required so that if user
 * has requested to skip the empty transactions we can skip the empty streams
 * even though the transaction has written some changes.
 *
 * 维护每个事务级别的变量以跟踪事务和/或流是否写入了任何更改。在流模式下，事务可以在流中解码，因此除了维护事务是否写入任何更改之外，我们还需要跟踪当前流是否写入任何更改。这是必需的，以便如果用户请求跳过空事务，即使事务已写入一些更改，我们也可以跳过空流。
 */
typedef struct
{
	bool		xact_wrote_changes;
	bool		stream_wrote_changes;
} TestDecodingTxnData;

static void pg_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
							  bool is_init);
static void pg_decode_shutdown(LogicalDecodingContext *ctx);
static void pg_decode_begin_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn);
static void pg_output_begin(LogicalDecodingContext *ctx,
							TestDecodingData *data,
							ReorderBufferTXN *txn,
							bool last_write);
static void pg_decode_commit_txn(LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pg_decode_change(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn, Relation relation,
							 ReorderBufferChange *change);
static void pg_decode_truncate(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn,
							   int nrelations, Relation relations[],
							   ReorderBufferChange *change);
static bool pg_decode_filter(LogicalDecodingContext *ctx,
							 RepOriginId origin_id);
static void pg_decode_message(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, XLogRecPtr lsn,
							  bool transactional, const char *prefix,
							  Size sz, const char *message);
static bool pg_decode_filter_prepare(LogicalDecodingContext *ctx,
									 TransactionId xid,
									 const char *gid);
static void pg_decode_begin_prepare_txn(LogicalDecodingContext *ctx,
										ReorderBufferTXN *txn);
static void pg_decode_prepare_txn(LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn,
								  XLogRecPtr prepare_lsn);
static void pg_decode_commit_prepared_txn(LogicalDecodingContext *ctx,
										  ReorderBufferTXN *txn,
										  XLogRecPtr commit_lsn);
static void pg_decode_rollback_prepared_txn(LogicalDecodingContext *ctx,
											ReorderBufferTXN *txn,
											XLogRecPtr prepare_end_lsn,
											TimestampTz prepare_time);
static void pg_decode_stream_start(LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn);
static void pg_output_stream_start(LogicalDecodingContext *ctx,
								   TestDecodingData *data,
								   ReorderBufferTXN *txn,
								   bool last_write);
static void pg_decode_stream_stop(LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn);
static void pg_decode_stream_abort(LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn,
								   XLogRecPtr abort_lsn);
static void pg_decode_stream_prepare(LogicalDecodingContext *ctx,
									 ReorderBufferTXN *txn,
									 XLogRecPtr prepare_lsn);
static void pg_decode_stream_commit(LogicalDecodingContext *ctx,
									ReorderBufferTXN *txn,
									XLogRecPtr commit_lsn);
static void pg_decode_stream_change(LogicalDecodingContext *ctx,
									ReorderBufferTXN *txn,
									Relation relation,
									ReorderBufferChange *change);
static void pg_decode_stream_message(LogicalDecodingContext *ctx,
									 ReorderBufferTXN *txn, XLogRecPtr lsn,
									 bool transactional, const char *prefix,
									 Size sz, const char *message);
static void pg_decode_stream_truncate(LogicalDecodingContext *ctx,
									  ReorderBufferTXN *txn,
									  int nrelations, Relation relations[],
									  ReorderBufferChange *change);

void
_PG_init(void)
{
	/* other plugins can perform things here
	 *
	 * 其他插件可以在这里执行操作
	 */
}

/* specify output plugin callbacks
 *
 * 指定输出插件回调
 */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	cb->startup_cb = pg_decode_startup;
	cb->begin_cb = pg_decode_begin_txn;
	cb->change_cb = pg_decode_change;
	cb->truncate_cb = pg_decode_truncate;
	cb->commit_cb = pg_decode_commit_txn;
	cb->filter_by_origin_cb = pg_decode_filter;
	cb->shutdown_cb = pg_decode_shutdown;
	cb->message_cb = pg_decode_message;
	cb->filter_prepare_cb = pg_decode_filter_prepare;
	cb->begin_prepare_cb = pg_decode_begin_prepare_txn;
	cb->prepare_cb = pg_decode_prepare_txn;
	cb->commit_prepared_cb = pg_decode_commit_prepared_txn;
	cb->rollback_prepared_cb = pg_decode_rollback_prepared_txn;
	cb->stream_start_cb = pg_decode_stream_start;
	cb->stream_stop_cb = pg_decode_stream_stop;
	cb->stream_abort_cb = pg_decode_stream_abort;
	cb->stream_prepare_cb = pg_decode_stream_prepare;
	cb->stream_commit_cb = pg_decode_stream_commit;
	cb->stream_change_cb = pg_decode_stream_change;
	cb->stream_message_cb = pg_decode_stream_message;
	cb->stream_truncate_cb = pg_decode_stream_truncate;
}


/* initialize this plugin
 *
 * 初始化这个插件
 */
static void
pg_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				  bool is_init)
{
	ListCell   *option;
	TestDecodingData *data;
	bool		enable_streaming = false;

	data = palloc0(sizeof(TestDecodingData));
	data->context = AllocSetContextCreate(ctx->context,
										  "text conversion context",
										  ALLOCSET_DEFAULT_SIZES);
	data->include_xids = true;
	data->include_timestamp = false;
	data->skip_empty_xacts = false;
	data->only_local = false;

	ctx->output_plugin_private = data;

	opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
	opt->receive_rewrites = false;

	foreach(option, ctx->output_plugin_options)
	{
		DefElem    *elem = lfirst(option);

		Assert(elem->arg == NULL || IsA(elem->arg, String));

		if (strcmp(elem->defname, "include-xids") == 0)
		{
			/* if option does not provide a value, it means its value is true
			 *
			 * 如果选项没有提供值，则表示其值为 true
			 */
			if (elem->arg == NULL)
				data->include_xids = true;
			else if (!parse_bool(strVal(elem->arg), &data->include_xids))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-timestamp") == 0)
		{
			if (elem->arg == NULL)
				data->include_timestamp = true;
			else if (!parse_bool(strVal(elem->arg), &data->include_timestamp))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "force-binary") == 0)
		{
			bool		force_binary;

			if (elem->arg == NULL)
				continue;
			else if (!parse_bool(strVal(elem->arg), &force_binary))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));

			if (force_binary)
				opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;
		}
		else if (strcmp(elem->defname, "skip-empty-xacts") == 0)
		{

			if (elem->arg == NULL)
				data->skip_empty_xacts = true;
			else if (!parse_bool(strVal(elem->arg), &data->skip_empty_xacts))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "only-local") == 0)
		{

			if (elem->arg == NULL)
				data->only_local = true;
			else if (!parse_bool(strVal(elem->arg), &data->only_local))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "include-rewrites") == 0)
		{

			if (elem->arg == NULL)
				continue;
			else if (!parse_bool(strVal(elem->arg), &opt->receive_rewrites))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else if (strcmp(elem->defname, "stream-changes") == 0)
		{
			if (elem->arg == NULL)
				continue;
			else if (!parse_bool(strVal(elem->arg), &enable_streaming))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("could not parse value \"%s\" for parameter \"%s\"",
								strVal(elem->arg), elem->defname)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" = \"%s\" is unknown",
							elem->defname,
							elem->arg ? strVal(elem->arg) : "(null)")));
		}
	}

	ctx->streaming &= enable_streaming;
}

/* cleanup this plugin's resources
 *
 * 清理这个插件的资源
 */
static void
pg_decode_shutdown(LogicalDecodingContext *ctx)
{
	TestDecodingData *data = ctx->output_plugin_private;

	/* cleanup our own resources via memory context reset
	 *
	 * 通过内存上下文重置来清理我们自己的资源
	 */
	MemoryContextDelete(data->context);
}

/* BEGIN callback
 *
 * 开始回调
 */
static void
pg_decode_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata =
		MemoryContextAllocZero(ctx->context, sizeof(TestDecodingTxnData));

	txndata->xact_wrote_changes = false;
	txn->output_plugin_private = txndata;

	/*
	 * If asked to skip empty transactions, we'll emit BEGIN at the point
	 * where the first operation is received for this transaction.
	 *
	 * 如果要求跳过空事务，我们将在收到该事务的第一个操作时发出 BEGIN 信号。
	 */
	if (data->skip_empty_xacts)
		return;

	pg_output_begin(ctx, data, txn, true);
}

static void
pg_output_begin(LogicalDecodingContext *ctx, TestDecodingData *data, ReorderBufferTXN *txn, bool last_write)
{
	OutputPluginPrepareWrite(ctx, last_write);
	if (data->include_xids)
		appendStringInfo(ctx->out, "BEGIN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "BEGIN");
	OutputPluginWrite(ctx, last_write);
}

/* COMMIT callback
 *
 * 提交回调
 */
static void
pg_decode_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr commit_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;
	bool		xact_wrote_changes = txndata->xact_wrote_changes;

	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (data->skip_empty_xacts && !xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "COMMIT %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "COMMIT");

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->xact_time.commit_time));

	OutputPluginWrite(ctx, true);
}

/* BEGIN PREPARE callback
 *
 * 开始准备回调
 */
static void
pg_decode_begin_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata =
		MemoryContextAllocZero(ctx->context, sizeof(TestDecodingTxnData));

	txndata->xact_wrote_changes = false;
	txn->output_plugin_private = txndata;

	/*
	 * If asked to skip empty transactions, we'll emit BEGIN at the point
	 * where the first operation is received for this transaction.
	 *
	 * 如果要求跳过空事务，我们将在收到该事务的第一个操作时发出 BEGIN 信号。
	 */
	if (data->skip_empty_xacts)
		return;

	pg_output_begin(ctx, data, txn, true);
}

/* PREPARE callback
 *
 * 准备回调
 */
static void
pg_decode_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					  XLogRecPtr prepare_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	/*
	 * If asked to skip empty transactions, we'll emit PREPARE at the point
	 * where the first operation is received for this transaction.
	 *
	 * 如果要求跳过空事务，我们将在收到该事务的第一个操作时发出 PREPARE。
	 */
	if (data->skip_empty_xacts && !txndata->xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfo(ctx->out, "PREPARE TRANSACTION %s",
					 quote_literal_cstr(txn->gid));

	if (data->include_xids)
		appendStringInfo(ctx->out, ", txid %u", txn->xid);

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->xact_time.prepare_time));

	OutputPluginWrite(ctx, true);
}

/* COMMIT PREPARED callback
 *
 * COMMIT PREPARED 回调
 */
static void
pg_decode_commit_prepared_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
							  XLogRecPtr commit_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfo(ctx->out, "COMMIT PREPARED %s",
					 quote_literal_cstr(txn->gid));

	if (data->include_xids)
		appendStringInfo(ctx->out, ", txid %u", txn->xid);

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->xact_time.commit_time));

	OutputPluginWrite(ctx, true);
}

/* ROLLBACK PREPARED callback
 *
 * ROLLBACK PREPARED 回调
 */
static void
pg_decode_rollback_prepared_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn,
								XLogRecPtr prepare_end_lsn,
								TimestampTz prepare_time)
{
	TestDecodingData *data = ctx->output_plugin_private;

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfo(ctx->out, "ROLLBACK PREPARED %s",
					 quote_literal_cstr(txn->gid));

	if (data->include_xids)
		appendStringInfo(ctx->out, ", txid %u", txn->xid);

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->xact_time.commit_time));

	OutputPluginWrite(ctx, true);
}

/*
 * Filter out two-phase transactions.
 *
 * 过滤掉两阶段交易。
 *
 * Each plugin can implement its own filtering logic. Here we demonstrate a
 * simple logic by checking the GID. If the GID contains the "_nodecode"
 * substring, then we filter it out.
 *
 * 每个插件都可以实现自己的过滤逻辑。这里我们通过检查GID来演示一个简单的逻辑。如果 GID 包含“_nodecode”子字符串，那么我们将其过滤掉。
 */
static bool
pg_decode_filter_prepare(LogicalDecodingContext *ctx, TransactionId xid,
						 const char *gid)
{
	if (strstr(gid, "_nodecode") != NULL)
		return true;

	return false;
}

static bool
pg_decode_filter(LogicalDecodingContext *ctx,
				 RepOriginId origin_id)
{
	TestDecodingData *data = ctx->output_plugin_private;

	if (data->only_local && origin_id != InvalidRepOriginId)
		return true;
	return false;
}

/*
 * Print literal `outputstr' already represented as string of type `typid'
 * into stringbuf `s'.
 *
 * 将已经表示为“typid”类型字符串的文字“outputstr”打印到 stringbuf“s”中。
 *
 * Some builtin types aren't quoted, the rest is quoted. Escaping is done as
 * if standard_conforming_strings were enabled.
 *
 * 一些内置类型没有被引用，其余的被引用。转义的完成就像启用了 standard_conforming_strings 一样。
 */
static void
print_literal(StringInfo s, Oid typid, char *outputstr)
{
	const char *valptr;

	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			/* NB: We don't care about Inf, NaN et al.
			 *
			 * 注意：我们不关心 Inf、NaN 等。
			 */
			appendStringInfoString(s, outputstr);
			break;

		case BITOID:
		case VARBITOID:
			appendStringInfo(s, "B'%s'", outputstr);
			break;

		case BOOLOID:
			if (strcmp(outputstr, "t") == 0)
				appendStringInfoString(s, "true");
			else
				appendStringInfoString(s, "false");
			break;

		default:
			appendStringInfoChar(s, '\'');
			for (valptr = outputstr; *valptr; valptr++)
			{
				char		ch = *valptr;

				if (SQL_STR_DOUBLE(ch, false))
					appendStringInfoChar(s, ch);
				appendStringInfoChar(s, ch);
			}
			appendStringInfoChar(s, '\'');
			break;
	}
}

/* print the tuple 'tuple' into the StringInfo s
 *
 * 将元组 'tuple' 打印到 StringInfo 中
 */
static void
tuple_to_stringinfo(StringInfo s, TupleDesc tupdesc, HeapTuple tuple, bool skip_nulls)
{
	int			natt;

	/* print all columns individually
	 *
	 * 单独打印所有列
	 */
	for (natt = 0; natt < tupdesc->natts; natt++)
	{
		Form_pg_attribute attr; /* the attribute itself */
		Oid			typid;		/* type of current attribute */
		Oid			typoutput;	/* output function */
		bool		typisvarlena;
		Datum		origval;	/* possibly toasted Datum */
		bool		isnull;		/* column is null? */

		attr = TupleDescAttr(tupdesc, natt);

		/*
		 * don't print dropped columns, we can't be sure everything is
		 * available for them
		 *
		 * 不要打印删除的列，我们无法确定所有内容都可供他们使用
		 */
		if (attr->attisdropped)
			continue;

		/*
		 * Don't print system columns, oid will already have been printed if
		 * present.
		 *
		 * 不要打印系统列，oid 如果存在的话已经被打印了。
		 */
		if (attr->attnum < 0)
			continue;

		typid = attr->atttypid;

		/* get Datum from tuple
		 *
		 * 从元组中获取数据
		 */
		origval = heap_getattr(tuple, natt + 1, tupdesc, &isnull);

		if (isnull && skip_nulls)
			continue;

		/* print attribute name
		 *
		 * 打印属性名称
		 */
		appendStringInfoChar(s, ' ');
		appendStringInfoString(s, quote_identifier(NameStr(attr->attname)));

		/* print attribute type
		 *
		 * 打印属性类型
		 */
		appendStringInfoChar(s, '[');
		appendStringInfoString(s, format_type_be(typid));
		appendStringInfoChar(s, ']');

		/* query output function
		 *
		 * 查询输出功能
		 */
		getTypeOutputInfo(typid,
						  &typoutput, &typisvarlena);

		/* print separator
		 *
		 * 打印分隔符
		 */
		appendStringInfoChar(s, ':');

		/* print data
		 *
		 * 打印数据
		 */
		if (isnull)
			appendStringInfoString(s, "null");
		else if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval))
			appendStringInfoString(s, "unchanged-toast-datum");
		else if (!typisvarlena)
			print_literal(s, typid,
						  OidOutputFunctionCall(typoutput, origval));
		else
		{
			Datum		val;	/* definitely detoasted Datum */

			val = PointerGetDatum(PG_DETOAST_DATUM(origval));
			print_literal(s, typid, OidOutputFunctionCall(typoutput, val));
		}
	}
}

/*
 * callback for individual changed tuples
 *
 * 个别更改元组的回调
 */
static void
pg_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 Relation relation, ReorderBufferChange *change)
{
	TestDecodingData *data;
	TestDecodingTxnData *txndata;
	Form_pg_class class_form;
	TupleDesc	tupdesc;
	MemoryContext old;

	data = ctx->output_plugin_private;
	txndata = txn->output_plugin_private;

	/* output BEGIN if we haven't yet
	 *
	 * 如果我们还没有输出 BEGIN
	 */
	if (data->skip_empty_xacts && !txndata->xact_wrote_changes)
	{
		pg_output_begin(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = true;

	class_form = RelationGetForm(relation);
	tupdesc = RelationGetDescr(relation);

	/* Avoid leaking memory by using and resetting our own context
	 *
	 * 通过使用和重置我们自己的上下文来避免内存泄漏
	 */
	old = MemoryContextSwitchTo(data->context);

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfoString(ctx->out, "table ");
	appendStringInfoString(ctx->out,
						   quote_qualified_identifier(get_namespace_name(get_rel_namespace(RelationGetRelid(relation))),
													  class_form->relrewrite ?
													  get_rel_name(class_form->relrewrite) :
													  NameStr(class_form->relname)));
	appendStringInfoChar(ctx->out, ':');

	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			appendStringInfoString(ctx->out, " INSERT:");
			if (change->data.tp.newtuple == NULL)
				appendStringInfoString(ctx->out, " (no-tuple-data)");
			else
				tuple_to_stringinfo(ctx->out, tupdesc,
									change->data.tp.newtuple,
									false);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			appendStringInfoString(ctx->out, " UPDATE:");
			if (change->data.tp.oldtuple != NULL)
			{
				appendStringInfoString(ctx->out, " old-key:");
				tuple_to_stringinfo(ctx->out, tupdesc,
									change->data.tp.oldtuple,
									true);
				appendStringInfoString(ctx->out, " new-tuple:");
			}

			if (change->data.tp.newtuple == NULL)
				appendStringInfoString(ctx->out, " (no-tuple-data)");
			else
				tuple_to_stringinfo(ctx->out, tupdesc,
									change->data.tp.newtuple,
									false);
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			appendStringInfoString(ctx->out, " DELETE:");

			/* if there was no PK, we only know that a delete happened
			 *
			 * 如果没有PK，我们只知道发生了删除
			 */
			if (change->data.tp.oldtuple == NULL)
				appendStringInfoString(ctx->out, " (no-tuple-data)");
			/* In DELETE, only the replica identity is present; display that
			 *
			 * 在 DELETE 中，仅存在副本身份；显示那个
			 */
			else
				tuple_to_stringinfo(ctx->out, tupdesc,
									change->data.tp.oldtuple,
									true);
			break;
		default:
			Assert(false);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}

static void
pg_decode_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				   int nrelations, Relation relations[], ReorderBufferChange *change)
{
	TestDecodingData *data;
	TestDecodingTxnData *txndata;
	MemoryContext old;
	int			i;

	data = ctx->output_plugin_private;
	txndata = txn->output_plugin_private;

	/* output BEGIN if we haven't yet
	 *
	 * 如果我们还没有输出 BEGIN
	 */
	if (data->skip_empty_xacts && !txndata->xact_wrote_changes)
	{
		pg_output_begin(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = true;

	/* Avoid leaking memory by using and resetting our own context
	 *
	 * 通过使用和重置我们自己的上下文来避免内存泄漏
	 */
	old = MemoryContextSwitchTo(data->context);

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfoString(ctx->out, "table ");

	for (i = 0; i < nrelations; i++)
	{
		if (i > 0)
			appendStringInfoString(ctx->out, ", ");

		appendStringInfoString(ctx->out,
							   quote_qualified_identifier(get_namespace_name(relations[i]->rd_rel->relnamespace),
														  NameStr(relations[i]->rd_rel->relname)));
	}

	appendStringInfoString(ctx->out, ": TRUNCATE:");

	if (change->data.truncate.restart_seqs
		|| change->data.truncate.cascade)
	{
		if (change->data.truncate.restart_seqs)
			appendStringInfoString(ctx->out, " restart_seqs");
		if (change->data.truncate.cascade)
			appendStringInfoString(ctx->out, " cascade");
	}
	else
		appendStringInfoString(ctx->out, " (no-flags)");

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}

static void
pg_decode_message(LogicalDecodingContext *ctx,
				  ReorderBufferTXN *txn, XLogRecPtr lsn, bool transactional,
				  const char *prefix, Size sz, const char *message)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata;

	txndata = transactional ? txn->output_plugin_private : NULL;

	/* output BEGIN if we haven't yet for transactional messages
	 *
	 * 如果我们还没有收到事务消息，则输出 BEGIN
	 */
	if (transactional && data->skip_empty_xacts && !txndata->xact_wrote_changes)
		pg_output_begin(ctx, data, txn, false);

	if (transactional)
		txndata->xact_wrote_changes = true;

	OutputPluginPrepareWrite(ctx, true);
	appendStringInfo(ctx->out, "message: transactional: %d prefix: %s, sz: %zu content:",
					 transactional, prefix, sz);
	appendBinaryStringInfo(ctx->out, message, sz);
	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_start(LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	/*
	 * Allocate the txn plugin data for the first stream in the transaction.
	 *
	 * 为事务中的第一个流分配 txn 插件数据。
	 */
	if (txndata == NULL)
	{
		txndata =
			MemoryContextAllocZero(ctx->context, sizeof(TestDecodingTxnData));
		txndata->xact_wrote_changes = false;
		txn->output_plugin_private = txndata;
	}

	txndata->stream_wrote_changes = false;
	if (data->skip_empty_xacts)
		return;
	pg_output_stream_start(ctx, data, txn, true);
}

static void
pg_output_stream_start(LogicalDecodingContext *ctx, TestDecodingData *data, ReorderBufferTXN *txn, bool last_write)
{
	OutputPluginPrepareWrite(ctx, last_write);
	if (data->include_xids)
		appendStringInfo(ctx->out, "opening a streamed block for transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "opening a streamed block for transaction");
	OutputPluginWrite(ctx, last_write);
}

static void
pg_decode_stream_stop(LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "closing a streamed block for transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "closing a streamed block for transaction");
	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_abort(LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn,
					   XLogRecPtr abort_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;

	/*
	 * stream abort can be sent for an individual subtransaction but we
	 * maintain the output_plugin_private only under the toptxn so if this is
	 * not the toptxn then fetch the toptxn.
	 *
	 * 可以为单个子事务发送流中止，但我们仅在toptxn下维护output_plugin_private，因此如果这不是toptxn，则获取toptxn。
	 */
	ReorderBufferTXN *toptxn = rbtxn_get_toptxn(txn);
	TestDecodingTxnData *txndata = toptxn->output_plugin_private;
	bool		xact_wrote_changes = txndata->xact_wrote_changes;

	if (rbtxn_is_toptxn(txn))
	{
		Assert(txn->output_plugin_private != NULL);
		pfree(txndata);
		txn->output_plugin_private = NULL;
	}

	if (data->skip_empty_xacts && !xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "aborting streamed (sub)transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "aborting streamed (sub)transaction");
	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_prepare(LogicalDecodingContext *ctx,
						 ReorderBufferTXN *txn,
						 XLogRecPtr prepare_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	if (data->skip_empty_xacts && !txndata->xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);

	if (data->include_xids)
		appendStringInfo(ctx->out, "preparing streamed transaction TXN %s, txid %u",
						 quote_literal_cstr(txn->gid), txn->xid);
	else
		appendStringInfo(ctx->out, "preparing streamed transaction %s",
						 quote_literal_cstr(txn->gid));

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->xact_time.prepare_time));

	OutputPluginWrite(ctx, true);
}

static void
pg_decode_stream_commit(LogicalDecodingContext *ctx,
						ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;
	bool		xact_wrote_changes = txndata->xact_wrote_changes;

	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (data->skip_empty_xacts && !xact_wrote_changes)
		return;

	OutputPluginPrepareWrite(ctx, true);

	if (data->include_xids)
		appendStringInfo(ctx->out, "committing streamed transaction TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "committing streamed transaction");

	if (data->include_timestamp)
		appendStringInfo(ctx->out, " (at %s)",
						 timestamptz_to_str(txn->xact_time.commit_time));

	OutputPluginWrite(ctx, true);
}

/*
 * In streaming mode, we don't display the changes as the transaction can abort
 * at a later point in time.  We don't want users to see the changes until the
 * transaction is committed.
 *
 * 在流模式下，我们不会显示更改，因为事务可能会在稍后的时间点中止。  我们不希望用户在提交事务之前看到更改。
 */
static void
pg_decode_stream_change(LogicalDecodingContext *ctx,
						ReorderBufferTXN *txn,
						Relation relation,
						ReorderBufferChange *change)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	/* output stream start if we haven't yet
	 *
	 * 如果我们还没有开始输出流
	 */
	if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
	{
		pg_output_stream_start(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = txndata->stream_wrote_changes = true;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "streaming change for TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "streaming change for transaction");
	OutputPluginWrite(ctx, true);
}

/*
 * In streaming mode, we don't display the contents for transactional messages
 * as the transaction can abort at a later point in time.  We don't want users to
 * see the message contents until the transaction is committed.
 *
 * 在流模式下，我们不显示事务消息的内容，因为事务可能会在稍后的时间点中止。  我们不希望用户在提交事务之前看到消息内容。
 */
static void
pg_decode_stream_message(LogicalDecodingContext *ctx,
						 ReorderBufferTXN *txn, XLogRecPtr lsn, bool transactional,
						 const char *prefix, Size sz, const char *message)
{
	/* Output stream start if we haven't yet for transactional messages.
	 *
	 * 如果我们还没有处理事务消息，则输出流启动。
	 */
	if (transactional)
	{
		TestDecodingData *data = ctx->output_plugin_private;
		TestDecodingTxnData *txndata = txn->output_plugin_private;

		if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
		{
			pg_output_stream_start(ctx, data, txn, false);
		}
		txndata->xact_wrote_changes = txndata->stream_wrote_changes = true;
	}

	OutputPluginPrepareWrite(ctx, true);

	if (transactional)
	{
		appendStringInfo(ctx->out, "streaming message: transactional: %d prefix: %s, sz: %zu",
						 transactional, prefix, sz);
	}
	else
	{
		appendStringInfo(ctx->out, "streaming message: transactional: %d prefix: %s, sz: %zu content:",
						 transactional, prefix, sz);
		appendBinaryStringInfo(ctx->out, message, sz);
	}

	OutputPluginWrite(ctx, true);
}

/*
 * In streaming mode, we don't display the detailed information of Truncate.
 * See pg_decode_stream_change.
 *
 * 在流模式下，我们不显示Truncate的详细信息。请参阅 pg_decode_stream_change。
 */
static void
pg_decode_stream_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
						  int nrelations, Relation relations[],
						  ReorderBufferChange *change)
{
	TestDecodingData *data = ctx->output_plugin_private;
	TestDecodingTxnData *txndata = txn->output_plugin_private;

	if (data->skip_empty_xacts && !txndata->stream_wrote_changes)
	{
		pg_output_stream_start(ctx, data, txn, false);
	}
	txndata->xact_wrote_changes = txndata->stream_wrote_changes = true;

	OutputPluginPrepareWrite(ctx, true);
	if (data->include_xids)
		appendStringInfo(ctx->out, "streaming truncate for TXN %u", txn->xid);
	else
		appendStringInfoString(ctx->out, "streaming truncate for transaction");
	OutputPluginWrite(ctx, true);
}
