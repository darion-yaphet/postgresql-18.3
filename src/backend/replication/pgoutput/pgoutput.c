/*-------------------------------------------------------------------------
 *
 * pgoutput.c
 *		Logical Replication output plugin
 *
 * 逻辑复制输出插件
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/backend/replication/pgoutput/pgoutput.c
 *
 * 标识
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupconvert.h"
#include "catalog/partition.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_subscription.h"
#include "commands/defrem.h"
#include "commands/subscriptioncmds.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "replication/logical.h"
#include "replication/logicalproto.h"
#include "replication/origin.h"
#include "replication/pgoutput.h"
#include "rewrite/rewriteHandler.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC_EXT(
					.name = "pgoutput",
					.version = PG_VERSION
);

static void pgoutput_startup(LogicalDecodingContext *ctx,
							 OutputPluginOptions *opt, bool is_init);
static void pgoutput_shutdown(LogicalDecodingContext *ctx);
static void pgoutput_begin_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn);
static void pgoutput_commit_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_change(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn, Relation relation,
							ReorderBufferChange *change);
static void pgoutput_truncate(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, int nrelations, Relation relations[],
							  ReorderBufferChange *change);
static void pgoutput_message(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn, XLogRecPtr message_lsn,
							 bool transactional, const char *prefix,
							 Size sz, const char *message);
static bool pgoutput_origin_filter(LogicalDecodingContext *ctx,
								   RepOriginId origin_id);
static void pgoutput_begin_prepare_txn(LogicalDecodingContext *ctx,
									   ReorderBufferTXN *txn);
static void pgoutput_prepare_txn(LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn, XLogRecPtr prepare_lsn);
static void pgoutput_commit_prepared_txn(LogicalDecodingContext *ctx,
										 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);
static void pgoutput_rollback_prepared_txn(LogicalDecodingContext *ctx,
										   ReorderBufferTXN *txn,
										   XLogRecPtr prepare_end_lsn,
										   TimestampTz prepare_time);
static void pgoutput_stream_start(struct LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn);
static void pgoutput_stream_stop(struct LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn);
static void pgoutput_stream_abort(struct LogicalDecodingContext *ctx,
								  ReorderBufferTXN *txn,
								  XLogRecPtr abort_lsn);
static void pgoutput_stream_commit(struct LogicalDecodingContext *ctx,
								   ReorderBufferTXN *txn,
								   XLogRecPtr commit_lsn);
static void pgoutput_stream_prepare_txn(LogicalDecodingContext *ctx,
										ReorderBufferTXN *txn, XLogRecPtr prepare_lsn);

static bool publications_valid;

static List *LoadPublications(List *pubnames);
static void publication_invalidation_cb(Datum arg, int cacheid,
										uint32 hashvalue);
static void send_repl_origin(LogicalDecodingContext *ctx,
							 RepOriginId origin_id, XLogRecPtr origin_lsn,
							 bool send_origin);

/*
 * Only 3 publication actions are used for row filtering ("insert", "update",
 * "delete"). See RelationSyncEntry.exprstate[].
 *
 * 行过滤只用到 3 种 publication 动作，即 insert、update 与 delete。
 * 参见 RelationSyncEntry 的 exprstate 数组。
 */
enum RowFilterPubAction
{
	PUBACTION_INSERT,
	PUBACTION_UPDATE,
	PUBACTION_DELETE,
};

#define NUM_ROWFILTER_PUBACTIONS (PUBACTION_DELETE+1)

/*
 * Entry in the map used to remember which relation schemas we sent.
 *
 * 用于记住已发送过哪些关系 schema 的映射项。
 *
 * The schema_sent flag determines if the current schema record for the
 * relation (and for its ancestor if publish_as_relid is set) was already
 * sent to the subscriber (in which case we don't need to send it again).
 *
 * schema_sent 标志表示该关系（以及在设置了 publish_as_relid
 * 时其祖先）的当前 schema 记录是否已经发给订阅端；若已发送则不必再发。
 *
 * The schema cache on downstream is however updated only at commit time,
 * and with streamed transactions the commit order may be different from
 * the order the transactions are sent in. Also, the (sub) transactions
 * might get aborted so we need to send the schema for each (sub) transaction
 * so that we don't lose the schema information on abort. For handling this,
 * we maintain the list of xids (streamed_txns) for those we have already sent
 * the schema.
 *
 * 下游的 schema 缓存只在提交时更新，
 * 而流式事务的提交顺序可能与发送顺序不同。子事务也可能中止，
 * 因此必须为每个事务或子事务发送 schema，以免中止时丢失 schema 信息。
 * 为此用 streamed_txns 记录已经发送过 schema 的事务 xid。
 *
 * For partitions, 'pubactions' considers not only the table's own
 * publications, but also those of all of its ancestors.
 *
 * 对分区而言，pubactions 不仅考虑表自身的 publication，
 * 还包括其所有祖先的 publication。
 */
typedef struct RelationSyncEntry
{
	Oid			relid;			/* relation oid
	 *
	 * 关系 oid
	 */

	bool		replicate_valid;	/* overall validity flag for entry
	 *
	 * 该项的总体有效标志
	 */

	bool		schema_sent;

	/*
	 * This will be PUBLISH_GENCOLS_STORED if the relation contains generated
	 * columns and the 'publish_generated_columns' parameter is set to
	 * PUBLISH_GENCOLS_STORED. Otherwise, it will be PUBLISH_GENCOLS_NONE,
	 * indicating that no generated columns should be published, unless
	 * explicitly specified in the column list.
	 *
	 * 若关系含有生成列，且 publish_generated_columns 参数为
	 * PUBLISH_GENCOLS_STORED，则这里为 PUBLISH_GENCOLS_STORED。否则为
	 * PUBLISH_GENCOLS_NONE，表示除非列清单明确指定，否则不发布生成列。
	 */
	PublishGencolsType include_gencols_type;
	List	   *streamed_txns;	/* streamed toplevel transactions with this
								 * schema
	 *
	 * 已随此 schema 发送过的流式顶层事务
	 */

	/* are we publishing this rel?
	 *
	 * 是否正在发布该关系？
	 */
	PublicationActions pubactions;

	/*
	 * ExprState array for row filter. Different publication actions don't
	 * allow multiple expressions to always be combined into one, because
	 * updates or deletes restrict the column in expression to be part of the
	 * replica identity index whereas inserts do not have this restriction, so
	 * there is one ExprState per publication action.
	 *
	 * 行过滤用的 ExprState 数组。不同 publication
	 * 动作不能总把多个表达式合成一个，因为 update 与 delete
	 * 要求表达式中的列属于副本标识索引，而 insert 没有这个限制，所以每种
	 * publication 动作各有一个 ExprState。
	 */
	ExprState  *exprstate[NUM_ROWFILTER_PUBACTIONS];
	EState	   *estate;			/* executor state used for row filter
	 *
	 * 行过滤使用的执行器状态
	 */
	TupleTableSlot *new_slot;	/* slot for storing new tuple
	 *
	 * 存放新元组的 slot
	 */
	TupleTableSlot *old_slot;	/* slot for storing old tuple
	 *
	 * 存放旧元组的 slot
	 */

	/*
	 * OID of the relation to publish changes as.  For a partition, this may
	 * be set to one of its ancestors whose schema will be used when
	 * replicating changes, if publish_via_partition_root is set for the
	 * publication.
	 *
	 * 作为发布变更时所使用的关系 OID。对分区而言，若 publication 设置了
	 * publish_via_partition_root，这里可能是某个祖先，
	 * 复制变更时使用该祖先的 schema。
	 */
	Oid			publish_as_relid;

	/*
	 * Map used when replicating using an ancestor's schema to convert tuples
	 * from partition's type to the ancestor's; NULL if publish_as_relid is
	 * same as 'relid' or if unnecessary due to partition and the ancestor
	 * having identical TupleDesc.
	 *
	 * 用祖先 schema 复制时，把元组从分区类型转换到祖先类型的映射；若
	 * publish_as_relid 与 relid 相同，或分区与祖先的 TupleDesc
	 * 相同因而不需要转换，则为 NULL。
	 */
	AttrMap    *attrmap;

	/*
	 * Columns included in the publication, or NULL if all columns are
	 * included implicitly.  Note that the attnums in this bitmap are not
	 * shifted by FirstLowInvalidHeapAttributeNumber.
	 *
	 * publication 包含的列；若为 NULL，则隐式包含全部列。注意此位图中的
	 * attnum 没有按 FirstLowInvalidHeapAttributeNumber 做偏移。
	 */
	Bitmapset  *columns;

	/*
	 * Private context to store additional data for this entry - state for the
	 * row filter expressions, column list, etc.
	 *
	 * 存放该项额外数据的私有内存上下文，例如行过滤表达式状态、列清单等。
	 */
	MemoryContext entry_cxt;
} RelationSyncEntry;

/*
 * Maintain a per-transaction level variable to track whether the transaction
 * has sent BEGIN. BEGIN is only sent when the first change in a transaction
 * is processed. This makes it possible to skip sending a pair of BEGIN/COMMIT
 * messages for empty transactions which saves network bandwidth.
 *
 * 为每个事务维护一个变量，记录是否已发送 BEGIN。BEGIN
 * 只在处理该事务的第一处变更时发送。这样可以跳过空事务的 BEGIN/COMMIT
 * 对，节省网络带宽。
 *
 * This optimization is not used for prepared transactions because if the
 * WALSender restarts after prepare of a transaction and before commit prepared
 * of the same transaction then we won't be able to figure out if we have
 * skipped sending BEGIN/PREPARE of a transaction as it was empty. This is
 * because we would have lost the in-memory txndata information that was
 * present prior to the restart. This will result in sending a spurious
 * COMMIT PREPARED without a corresponding prepared transaction at the
 * downstream which would lead to an error when it tries to process it.
 *
 * 预备事务不使用这项优化。若 WALSender 在事务 prepare 之后、
 * 同一事务的 commit prepared 之前重启，
 * 就无法判断是否因为事务为空而跳过了 BEGIN/PREPARE。重启前内存中的
 * txndata 已经丢失。结果会向下游发送一条没有对应预备事务的 COMMIT
 * PREPARED，下游处理时会出错。
 *
 * XXX We could achieve this optimization by changing protocol to send
 * additional information so that downstream can detect that the corresponding
 * prepare has not been sent. However, adding such a check for every
 * transaction in the downstream could be costly so we might want to do it
 * optionally.
 *
 * XXX：可以通过改协议、发送额外信息来做这项优化，让下游发现对应的
 * prepare 没有发送。但下游对每个事务都做这种检查可能代价较高，
 * 因此也许应做成可选项。
 *
 * We also don't have this optimization for streamed transactions because
 * they can contain prepared transactions.
 *
 * 流式事务也不使用这项优化，因为它们可以包含预备事务。
 */
typedef struct PGOutputTxnData
{
	bool		sent_begin_txn; /* flag indicating whether BEGIN has been sent
	 *
	 * 表示是否已发送 BEGIN 的标志
	 */
} PGOutputTxnData;

/* Map used to remember which relation schemas we sent.
 *
 * 用于记住已发送过哪些关系 schema 的映射。
 */
static HTAB *RelationSyncCache = NULL;

static void init_rel_sync_cache(MemoryContext cachectx);
static void cleanup_rel_sync_cache(TransactionId xid, bool is_commit);
static RelationSyncEntry *get_rel_sync_entry(PGOutputData *data,
											 Relation relation);
static void send_relation_and_attrs(Relation relation, TransactionId xid,
									LogicalDecodingContext *ctx,
									RelationSyncEntry *relentry);
static void rel_sync_cache_relation_cb(Datum arg, Oid relid);
static void rel_sync_cache_publication_cb(Datum arg, int cacheid,
										  uint32 hashvalue);
static void set_schema_sent_in_streamed_txn(RelationSyncEntry *entry,
											TransactionId xid);
static bool get_schema_sent_in_streamed_txn(RelationSyncEntry *entry,
											TransactionId xid);
static void init_tuple_slot(PGOutputData *data, Relation relation,
							RelationSyncEntry *entry);
static void pgoutput_memory_context_reset(void *arg);

/* row filter routines
 *
 * 行过滤例程
 */
static EState *create_estate_for_relation(Relation rel);
static void pgoutput_row_filter_init(PGOutputData *data,
									 List *publications,
									 RelationSyncEntry *entry);
static bool pgoutput_row_filter_exec_expr(ExprState *state,
										  ExprContext *econtext);
static bool pgoutput_row_filter(Relation relation, TupleTableSlot *old_slot,
								TupleTableSlot **new_slot_ptr,
								RelationSyncEntry *entry,
								ReorderBufferChangeType *action);

/* column list routines
 *
 * 列清单例程
 */
static void pgoutput_column_list_init(PGOutputData *data,
									  List *publications,
									  RelationSyncEntry *entry);

/*
 * 核心流程：
 * 1) _PG_output_plugin_init 注册逻辑解码回调。
 * 2) pgoutput_startup 解析 proto_version、publication_names 等选项并装载 publication。
 * 3) pgoutput_change、pgoutput_truncate、pgoutput_message 按 publication 的行过滤与列清单发送变更。
 * 4) 流式事务与两阶段提交走 stream 与 prepare 系列回调；关系 schema 经 RelationSyncCache 去重后发给订阅端。
 */

/*
 * Specify output plugin callbacks
 *
 * 指定输出插件回调
 */
void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	cb->startup_cb = pgoutput_startup;
	cb->begin_cb = pgoutput_begin_txn;
	cb->change_cb = pgoutput_change;
	cb->truncate_cb = pgoutput_truncate;
	cb->message_cb = pgoutput_message;
	cb->commit_cb = pgoutput_commit_txn;

	cb->begin_prepare_cb = pgoutput_begin_prepare_txn;
	cb->prepare_cb = pgoutput_prepare_txn;
	cb->commit_prepared_cb = pgoutput_commit_prepared_txn;
	cb->rollback_prepared_cb = pgoutput_rollback_prepared_txn;
	cb->filter_by_origin_cb = pgoutput_origin_filter;
	cb->shutdown_cb = pgoutput_shutdown;

	/* transaction streaming
	 *
	 * 事务流式传输
	 */
	cb->stream_start_cb = pgoutput_stream_start;
	cb->stream_stop_cb = pgoutput_stream_stop;
	cb->stream_abort_cb = pgoutput_stream_abort;
	cb->stream_commit_cb = pgoutput_stream_commit;
	cb->stream_change_cb = pgoutput_change;
	cb->stream_message_cb = pgoutput_message;
	cb->stream_truncate_cb = pgoutput_truncate;
	/* transaction streaming - two-phase commit
	 *
	 * 事务流式传输，两阶段提交
	 */
	cb->stream_prepare_cb = pgoutput_stream_prepare_txn;
}

/*
 * 解析输出插件启动参数，填入 PGOutputData。
 */
static void
parse_output_parameters(List *options, PGOutputData *data)
{
	ListCell   *lc;
	bool		protocol_version_given = false;
	bool		publication_names_given = false;
	bool		binary_option_given = false;
	bool		messages_option_given = false;
	bool		streaming_given = false;
	bool		two_phase_option_given = false;
	bool		origin_option_given = false;

	data->binary = false;
	data->streaming = LOGICALREP_STREAM_OFF;
	data->messages = false;
	data->two_phase = false;

	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		Assert(defel->arg == NULL || IsA(defel->arg, String));

		/* Check each param, whether or not we recognize it
		 *
		 * 检查每个参数，无论我们是否认识它
		 */
		if (strcmp(defel->defname, "proto_version") == 0)
		{
			unsigned long parsed;
			char	   *endptr;

			if (protocol_version_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			protocol_version_given = true;

			errno = 0;
			parsed = strtoul(strVal(defel->arg), &endptr, 10);
			if (errno != 0 || *endptr != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid proto_version")));

			if (parsed > PG_UINT32_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("proto_version \"%s\" out of range",
								strVal(defel->arg))));

			data->protocol_version = (uint32) parsed;
		}
		else if (strcmp(defel->defname, "publication_names") == 0)
		{
			if (publication_names_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			publication_names_given = true;

			/*
			 * Pass a copy of the DefElem->arg since SplitIdentifierString
			 * modifies its input.
			 *
			 * 传入 DefElem 的 arg 的副本，因为 SplitIdentifierString
			 * 会修改其输入。
			 */
			if (!SplitIdentifierString(pstrdup(strVal(defel->arg)), ',',
									   &data->publication_names))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
						 errmsg("invalid publication_names syntax")));
		}
		else if (strcmp(defel->defname, "binary") == 0)
		{
			if (binary_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			binary_option_given = true;

			data->binary = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "messages") == 0)
		{
			if (messages_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			messages_option_given = true;

			data->messages = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "streaming") == 0)
		{
			if (streaming_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			streaming_given = true;

			data->streaming = defGetStreamingMode(defel);
		}
		else if (strcmp(defel->defname, "two_phase") == 0)
		{
			if (two_phase_option_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			two_phase_option_given = true;

			data->two_phase = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "origin") == 0)
		{
			char	   *origin;

			if (origin_option_given)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("conflicting or redundant options"));
			origin_option_given = true;

			origin = defGetString(defel);
			if (pg_strcasecmp(origin, LOGICALREP_ORIGIN_NONE) == 0)
				data->publish_no_origin = true;
			else if (pg_strcasecmp(origin, LOGICALREP_ORIGIN_ANY) == 0)
				data->publish_no_origin = false;
			else
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("unrecognized origin value: \"%s\"", origin));
		}
		else
			elog(ERROR, "unrecognized pgoutput option: %s", defel->defname);
	}

	/* Check required options
	 *
	 * 检查必需选项
	 */
	if (!protocol_version_given)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("option \"%s\" missing", "proto_version"));
	if (!publication_names_given)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("option \"%s\" missing", "publication_names"));
}

/*
 * Memory context reset callback of PGOutputData->context.
 *
 * PGOutputData 的 context 的内存上下文重置回调。
 */
static void
pgoutput_memory_context_reset(void *arg)
{
	if (RelationSyncCache)
	{
		hash_destroy(RelationSyncCache);
		RelationSyncCache = NULL;
	}
}

/*
 * Initialize this plugin
 *
 * 初始化本插件
 */
static void
pgoutput_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				 bool is_init)
{
	PGOutputData *data = palloc0(sizeof(PGOutputData));
	static bool publication_callback_registered = false;
	MemoryContextCallback *mcallback;

	/* Create our memory context for private allocations.
	 *
	 * 创建用于私有分配的内存上下文。
	 */
	data->context = AllocSetContextCreate(ctx->context,
										  "logical replication output context",
										  ALLOCSET_DEFAULT_SIZES);

	data->cachectx = AllocSetContextCreate(ctx->context,
										   "logical replication cache context",
										   ALLOCSET_DEFAULT_SIZES);

	data->pubctx = AllocSetContextCreate(ctx->context,
										 "logical replication publication list context",
										 ALLOCSET_SMALL_SIZES);

	/*
	 * Ensure to cleanup RelationSyncCache even when logical decoding invoked
	 * via SQL interface ends up with an error.
	 *
	 * 即使通过 SQL 接口调用的逻辑解码以错误结束，也要清理
	 * RelationSyncCache。
	 */
	mcallback = palloc0(sizeof(MemoryContextCallback));
	mcallback->func = pgoutput_memory_context_reset;
	MemoryContextRegisterResetCallback(ctx->context, mcallback);

	ctx->output_plugin_private = data;

	/* This plugin uses binary protocol.
	 *
	 * 本插件使用二进制协议。
	 */
	opt->output_type = OUTPUT_PLUGIN_BINARY_OUTPUT;

	/*
	 * This is replication start and not slot initialization.
	 *
	 * 这是复制启动，而不是槽初始化。
	 *
	 * Parse and validate options passed by the client.
	 *
	 * 解析并校验客户端传入的选项。
	 */
	if (!is_init)
	{
		/* Parse the params and ERROR if we see any we don't recognize
		 *
		 * 解析参数，若遇到不认识的参数则 ERROR
		 */
		parse_output_parameters(ctx->output_plugin_options, data);

		/* Check if we support requested protocol
		 *
		 * 检查是否支持所请求的协议
		 */
		if (data->protocol_version > LOGICALREP_PROTO_MAX_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but server only supports protocol %d or lower",
							data->protocol_version, LOGICALREP_PROTO_MAX_VERSION_NUM)));

		if (data->protocol_version < LOGICALREP_PROTO_MIN_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("client sent proto_version=%d but server only supports protocol %d or higher",
							data->protocol_version, LOGICALREP_PROTO_MIN_VERSION_NUM)));

		/*
		 * Decide whether to enable streaming. It is disabled by default, in
		 * which case we just update the flag in decoding context. Otherwise
		 * we only allow it with sufficient version of the protocol, and when
		 * the output plugin supports it.
		 *
		 * 决定是否启用流式传输。默认关闭，此时只更新解码上下文中的标志。
		 * 否则仅在协议版本足够且输出插件支持时才允许启用。
		 */
		if (data->streaming == LOGICALREP_STREAM_OFF)
			ctx->streaming = false;
		else if (data->streaming == LOGICALREP_STREAM_ON &&
				 data->protocol_version < LOGICALREP_PROTO_STREAM_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support streaming, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_STREAM_VERSION_NUM)));
		else if (data->streaming == LOGICALREP_STREAM_PARALLEL &&
				 data->protocol_version < LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support parallel streaming, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM)));
		else if (!ctx->streaming)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("streaming requested, but not supported by output plugin")));

		/*
		 * Here, we just check whether the two-phase option is passed by
		 * plugin and decide whether to enable it at later point of time. It
		 * remains enabled if the previous start-up has done so. But we only
		 * allow the option to be passed in with sufficient version of the
		 * protocol, and when the output plugin supports it.
		 *
		 * 这里只检查插件是否传入了 two-phase 选项，并决定稍后是否启用。
		 * 若上次启动已经启用，则保持启用。但只有协议版本足够且输出插件支持时，
		 * 才允许传入该选项。
		 */
		if (!data->two_phase)
			ctx->twophase_opt_given = false;
		else if (data->protocol_version < LOGICALREP_PROTO_TWOPHASE_VERSION_NUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested proto_version=%d does not support two-phase commit, need %d or higher",
							data->protocol_version, LOGICALREP_PROTO_TWOPHASE_VERSION_NUM)));
		else if (!ctx->twophase)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("two-phase commit requested, but not supported by output plugin")));
		else
			ctx->twophase_opt_given = true;

		/* Init publication state.
		 *
		 * 初始化 publication 状态。
		 */
		data->publications = NIL;
		publications_valid = false;

		/*
		 * Register callback for pg_publication if we didn't already do that
		 * during some previous call in this process.
		 *
		 * 若本进程此前的调用尚未注册，则为 pg_publication 注册回调。
		 */
		if (!publication_callback_registered)
		{
			CacheRegisterSyscacheCallback(PUBLICATIONOID,
										  publication_invalidation_cb,
										  (Datum) 0);
			CacheRegisterRelSyncCallback(rel_sync_cache_relation_cb,
										 (Datum) 0);
			publication_callback_registered = true;
		}

		/* Initialize relation schema cache.
		 *
		 * 初始化关系 schema 缓存。
		 */
		init_rel_sync_cache(CacheMemoryContext);
	}
	else
	{
		/*
		 * Disable the streaming and prepared transactions during the slot
		 * initialization mode.
		 *
		 * 在槽初始化模式下关闭流式传输与预备事务。
		 */
		ctx->streaming = false;
		ctx->twophase = false;
	}
}

/*
 * BEGIN callback.
 *
 * BEGIN 回调。
 *
 * Don't send the BEGIN message here instead postpone it until the first
 * change. In logical replication, a common scenario is to replicate a set of
 * tables (instead of all tables) and transactions whose changes were on
 * the table(s) that are not published will produce empty transactions. These
 * empty transactions will send BEGIN and COMMIT messages to subscribers,
 * using bandwidth on something with little/no use for logical replication.
 *
 * 不要在这里发送 BEGIN，推迟到第一处变更。
 * 逻辑复制常见的是只复制一部分表，
 * 变更落在未发布表上的事务会产生空事务。这些空事务若发送 BEGIN 与
 * COMMIT，只会占用带宽，对逻辑复制几乎没有用处。
 */
static void
pgoutput_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	PGOutputTxnData *txndata = MemoryContextAllocZero(ctx->context,
													  sizeof(PGOutputTxnData));

	txn->output_plugin_private = txndata;
}

/*
 * Send BEGIN.
 *
 * 发送 BEGIN。
 *
 * This is called while processing the first change of the transaction.
 *
 * 处理该事务的第一处变更时调用。
 */
static void
pgoutput_send_begin(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;

	Assert(txndata);
	Assert(!txndata->sent_begin_txn);

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin(ctx->out, txn);
	txndata->sent_begin_txn = true;

	send_repl_origin(ctx, txn->origin_id, txn->origin_lsn,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT callback
 *
 * COMMIT 回调
 */
static void
pgoutput_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					XLogRecPtr commit_lsn)
{
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	bool		sent_begin_txn;

	Assert(txndata);

	/*
	 * We don't need to send the commit message unless some relevant change
	 * from this transaction has been sent to the downstream.
	 *
	 * 除非该事务已有相关变更发给下游，否则不必发送提交消息。
	 */
	sent_begin_txn = txndata->sent_begin_txn;
	OutputPluginUpdateProgress(ctx, !sent_begin_txn);
	pfree(txndata);
	txn->output_plugin_private = NULL;

	if (!sent_begin_txn)
	{
		elog(DEBUG1, "skipped replication of an empty transaction with XID: %u", txn->xid);
		return;
	}

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * BEGIN PREPARE callback
 *
 * BEGIN PREPARE 回调
 */
static void
pgoutput_begin_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_begin_prepare(ctx->out, txn);

	send_repl_origin(ctx, txn->origin_id, txn->origin_lsn,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);
}

/*
 * PREPARE callback
 *
 * PREPARE 回调
 */
static void
pgoutput_prepare_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr prepare_lsn)
{
	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_prepare(ctx->out, txn, prepare_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * COMMIT PREPARED callback
 *
 * COMMIT PREPARED 回调
 */
static void
pgoutput_commit_prepared_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
							 XLogRecPtr commit_lsn)
{
	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_commit_prepared(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * ROLLBACK PREPARED callback
 *
 * ROLLBACK PREPARED 回调
 */
static void
pgoutput_rollback_prepared_txn(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn,
							   XLogRecPtr prepare_end_lsn,
							   TimestampTz prepare_time)
{
	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_rollback_prepared(ctx->out, txn, prepare_end_lsn,
									   prepare_time);
	OutputPluginWrite(ctx, true);
}

/*
 * Write the current schema of the relation and its ancestor (if any) if not
 * done yet.
 *
 * 若尚未发送，则写出该关系及其祖先（若有）的当前 schema。
 */
static void
maybe_send_schema(LogicalDecodingContext *ctx,
				  ReorderBufferChange *change,
				  Relation relation, RelationSyncEntry *relentry)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	bool		schema_sent;
	TransactionId xid = InvalidTransactionId;
	TransactionId topxid = InvalidTransactionId;

	/*
	 * Remember XID of the (sub)transaction for the change. We don't care if
	 * it's top-level transaction or not (we have already sent that XID in
	 * start of the current streaming block).
	 *
	 * 记住该变更所属事务或子事务的 XID。不必区分是否为顶层事务，
	 * 当前流式块开始时已经发送过该 XID。
	 *
	 * If we're not in a streaming block, just use InvalidTransactionId and
	 * the write methods will not include it.
	 *
	 * 若不在流式块中，则使用 InvalidTransactionId，写入方法不会带上它。
	 */
	if (data->in_streaming)
		xid = change->txn->xid;

	if (rbtxn_is_subtxn(change->txn))
		topxid = rbtxn_get_toptxn(change->txn)->xid;
	else
		topxid = xid;

	/*
	 * Do we need to send the schema? We do track streamed transactions
	 * separately, because those may be applied later (and the regular
	 * transactions won't see their effects until then) and in an order that
	 * we don't know at this point.
	 *
	 * 是否需要发送 schema？流式事务要单独跟踪，因为它们可能稍后才应用，
	 * 常规事务在此之前看不到其效果，而且此时还不知道应用顺序。
	 *
	 * XXX There is a scope of optimization here. Currently, we always send
	 * the schema first time in a streaming transaction but we can probably
	 * avoid that by checking 'relentry->schema_sent' flag. However, before
	 * doing that we need to study its impact on the case where we have a mix
	 * of streaming and non-streaming transactions.
	 *
	 * XXX：这里还有优化余地。目前流式事务第一次总会发送 schema，
	 * 也许可以通过检查 relentry 的 schema_sent 标志来避免。
	 * 但在此之前需要研究它与流式、非流式事务混合时的影响。
	 */
	if (data->in_streaming)
		schema_sent = get_schema_sent_in_streamed_txn(relentry, topxid);
	else
		schema_sent = relentry->schema_sent;

	/* Nothing to do if we already sent the schema.
	 *
	 * 若已经发送过 schema，则无需再做。
	 */
	if (schema_sent)
		return;

	/*
	 * Send the schema.  If the changes will be published using an ancestor's
	 * schema, not the relation's own, send that ancestor's schema before
	 * sending relation's own (XXX - maybe sending only the former suffices?).
	 *
	 * 发送 schema。若变更将按某个祖先的 schema 而不是关系自身的 schema
	 * 发布，则先发送该祖先的 schema，再发送关系自身的 schema。XXX：
	 * 也许只发送前者就够了。
	 */
	if (relentry->publish_as_relid != RelationGetRelid(relation))
	{
		Relation	ancestor = RelationIdGetRelation(relentry->publish_as_relid);

		send_relation_and_attrs(ancestor, xid, ctx, relentry);
		RelationClose(ancestor);
	}

	send_relation_and_attrs(relation, xid, ctx, relentry);

	if (data->in_streaming)
		set_schema_sent_in_streamed_txn(relentry, topxid);
	else
		relentry->schema_sent = true;
}

/*
 * Sends a relation
 *
 * 发送一个关系
 */
static void
send_relation_and_attrs(Relation relation, TransactionId xid,
						LogicalDecodingContext *ctx,
						RelationSyncEntry *relentry)
{
	TupleDesc	desc = RelationGetDescr(relation);
	Bitmapset  *columns = relentry->columns;
	PublishGencolsType include_gencols_type = relentry->include_gencols_type;
	int			i;

	/*
	 * Write out type info if needed.  We do that only for user-created types.
	 * We use FirstGenbkiObjectId as the cutoff, so that we only consider
	 * objects with hand-assigned OIDs to be "built in", not for instance any
	 * function or type defined in the information_schema. This is important
	 * because only hand-assigned OIDs can be expected to remain stable across
	 * major versions.
	 *
	 * 需要时写出类型信息。只对用户创建的类型这样做。以
	 * FirstGenbkiObjectId 为界，只把手工分配 OID 的对象视为内建对象，
	 * 而不是例如 information_schema 中定义的函数或类型。这很重要，
	 * 因为只有手工分配的 OID 才能期望在大版本之间保持稳定。
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!logicalrep_should_publish_column(att, columns,
											  include_gencols_type))
			continue;

		if (att->atttypid < FirstGenbkiObjectId)
			continue;

		OutputPluginPrepareWrite(ctx, false);
		logicalrep_write_typ(ctx->out, xid, att->atttypid);
		OutputPluginWrite(ctx, false);
	}

	OutputPluginPrepareWrite(ctx, false);
	logicalrep_write_rel(ctx->out, xid, relation, columns,
						 include_gencols_type);
	OutputPluginWrite(ctx, false);
}

/*
 * Executor state preparation for evaluation of row filter expressions for the
 * specified relation.
 *
 * 为指定关系准备执行器状态，以便计算行过滤表达式。
 */
static EState *
create_estate_for_relation(Relation rel)
{
	EState	   *estate;
	RangeTblEntry *rte;
	List	   *perminfos = NIL;

	estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel);
	rte->relkind = rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;

	addRTEPermissionInfo(&perminfos, rte);

	ExecInitRangeTable(estate, list_make1(rte), perminfos,
					   bms_make_singleton(1));

	estate->es_output_cid = GetCurrentCommandId(false);

	return estate;
}

/*
 * Evaluates row filter.
 *
 * 计算行过滤。
 *
 * If the row filter evaluates to NULL, it is taken as false i.e. the change
 * isn't replicated.
 *
 * 若行过滤结果为 NULL，则视为 false，即不复制该变更。
 */
static bool
pgoutput_row_filter_exec_expr(ExprState *state, ExprContext *econtext)
{
	Datum		ret;
	bool		isnull;

	Assert(state != NULL);

	ret = ExecEvalExprSwitchContext(state, econtext, &isnull);

	elog(DEBUG3, "row filter evaluates to %s (isnull: %s)",
		 isnull ? "false" : DatumGetBool(ret) ? "true" : "false",
		 isnull ? "true" : "false");

	if (isnull)
		return false;

	return DatumGetBool(ret);
}

/*
 * Make sure the per-entry memory context exists.
 *
 * 确保该项自己的内存上下文存在。
 */
static void
pgoutput_ensure_entry_cxt(PGOutputData *data, RelationSyncEntry *entry)
{
	Relation	relation;

	/* The context may already exist, in which case bail out.
	 *
	 * 上下文可能已经存在，若如此则直接返回。
	 */
	if (entry->entry_cxt)
		return;

	relation = RelationIdGetRelation(entry->publish_as_relid);

	entry->entry_cxt = AllocSetContextCreate(data->cachectx,
											 "entry private context",
											 ALLOCSET_SMALL_SIZES);

	MemoryContextCopyAndSetIdentifier(entry->entry_cxt,
									  RelationGetRelationName(relation));
}

/*
 * Initialize the row filter.
 *
 * 初始化行过滤。
 */
static void
pgoutput_row_filter_init(PGOutputData *data, List *publications,
						 RelationSyncEntry *entry)
{
	ListCell   *lc;
	List	   *rfnodes[] = {NIL, NIL, NIL};	/* One per pubaction
	 *
	 * 每种 pubaction 一个
	 */
	bool		no_filter[] = {false, false, false};	/* One per pubaction
	 *
	 * 每种 pubaction 一个
	 */
	MemoryContext oldctx;
	int			idx;
	bool		has_filter = true;
	Oid			schemaid = get_rel_namespace(entry->publish_as_relid);

	/*
	 * Find if there are any row filters for this relation. If there are, then
	 * prepare the necessary ExprState and cache it in entry->exprstate. To
	 * build an expression state, we need to ensure the following:
	 *
	 * 查找该关系是否有行过滤。若有，则准备所需的 ExprState 并缓存在 entry
	 * 的 exprstate 中。要建立表达式状态，需要保证以下几点：
	 *
	 * All the given publication-table mappings must be checked.
	 *
	 * 必须检查所有给定的 publication 与表的映射。
	 *
	 * Multiple publications might have multiple row filters for this
	 * relation. Since row filter usage depends on the DML operation, there
	 * are multiple lists (one for each operation) to which row filters will
	 * be appended.
	 *
	 * 多个 publication 可能对该关系有多个行过滤。行过滤的使用取决于 DML
	 * 操作，因此每种操作各有一个列表，行过滤会追加到对应列表。
	 *
	 * FOR ALL TABLES and FOR TABLES IN SCHEMA implies "don't use row filter
	 * expression" so it takes precedence.
	 *
	 * FOR ALL TABLES 与 FOR TABLES IN SCHEMA 意味着不使用行过滤表达式，
	 * 因此它们优先。
	 */
	foreach(lc, publications)
	{
		Publication *pub = lfirst(lc);
		HeapTuple	rftuple = NULL;
		Datum		rfdatum = 0;
		bool		pub_no_filter = true;

		/*
		 * If the publication is FOR ALL TABLES, or the publication includes a
		 * FOR TABLES IN SCHEMA where the table belongs to the referred
		 * schema, then it is treated the same as if there are no row filters
		 * (even if other publications have a row filter).
		 *
		 * 若 publication 是 FOR ALL TABLES，或 publication 包含 FOR TABLES IN
		 * SCHEMA 且表属于所指 schema，则视为没有行过滤，即使其他 publication
		 * 有行过滤也一样。
		 */
		if (!pub->alltables &&
			!SearchSysCacheExists2(PUBLICATIONNAMESPACEMAP,
								   ObjectIdGetDatum(schemaid),
								   ObjectIdGetDatum(pub->oid)))
		{
			/*
			 * Check for the presence of a row filter in this publication.
			 *
			 * 检查该 publication 中是否存在行过滤。
			 */
			rftuple = SearchSysCache2(PUBLICATIONRELMAP,
									  ObjectIdGetDatum(entry->publish_as_relid),
									  ObjectIdGetDatum(pub->oid));

			if (HeapTupleIsValid(rftuple))
			{
				/* Null indicates no filter.
				 *
				 * 空值表示没有过滤。
				 */
				rfdatum = SysCacheGetAttr(PUBLICATIONRELMAP, rftuple,
										  Anum_pg_publication_rel_prqual,
										  &pub_no_filter);
			}
		}

		if (pub_no_filter)
		{
			if (rftuple)
				ReleaseSysCache(rftuple);

			no_filter[PUBACTION_INSERT] |= pub->pubactions.pubinsert;
			no_filter[PUBACTION_UPDATE] |= pub->pubactions.pubupdate;
			no_filter[PUBACTION_DELETE] |= pub->pubactions.pubdelete;

			/*
			 * Quick exit if all the DML actions are publicized via this
			 * publication.
			 *
			 * 若该 publication 已公开全部 DML 动作，则快速退出。
			 */
			if (no_filter[PUBACTION_INSERT] &&
				no_filter[PUBACTION_UPDATE] &&
				no_filter[PUBACTION_DELETE])
			{
				has_filter = false;
				break;
			}

			/* No additional work for this publication. Next one.
			 *
			 * 对该 publication 无需额外工作。处理下一个。
			 */
			continue;
		}

		/* Form the per pubaction row filter lists.
		 *
		 * 按每种 pubaction 组成行过滤列表。
		 */
		if (pub->pubactions.pubinsert && !no_filter[PUBACTION_INSERT])
			rfnodes[PUBACTION_INSERT] = lappend(rfnodes[PUBACTION_INSERT],
												TextDatumGetCString(rfdatum));
		if (pub->pubactions.pubupdate && !no_filter[PUBACTION_UPDATE])
			rfnodes[PUBACTION_UPDATE] = lappend(rfnodes[PUBACTION_UPDATE],
												TextDatumGetCString(rfdatum));
		if (pub->pubactions.pubdelete && !no_filter[PUBACTION_DELETE])
			rfnodes[PUBACTION_DELETE] = lappend(rfnodes[PUBACTION_DELETE],
												TextDatumGetCString(rfdatum));

		ReleaseSysCache(rftuple);
	}							/* loop all subscribed publications
	 *
	 * 遍历所有已订阅的 publication
	 */

	/* Clean the row filter
	 *
	 * 清理行过滤
	 */
	for (idx = 0; idx < NUM_ROWFILTER_PUBACTIONS; idx++)
	{
		if (no_filter[idx])
		{
			list_free_deep(rfnodes[idx]);
			rfnodes[idx] = NIL;
		}
	}

	if (has_filter)
	{
		Relation	relation = RelationIdGetRelation(entry->publish_as_relid);

		pgoutput_ensure_entry_cxt(data, entry);

		/*
		 * Now all the filters for all pubactions are known. Combine them when
		 * their pubactions are the same.
		 *
		 * 现在已经知道所有 pubaction 的全部过滤条件。当 pubaction
		 * 相同时把它们合并。
		 */
		oldctx = MemoryContextSwitchTo(entry->entry_cxt);
		entry->estate = create_estate_for_relation(relation);
		for (idx = 0; idx < NUM_ROWFILTER_PUBACTIONS; idx++)
		{
			List	   *filters = NIL;
			Expr	   *rfnode;

			if (rfnodes[idx] == NIL)
				continue;

			foreach(lc, rfnodes[idx])
				filters = lappend(filters, expand_generated_columns_in_expr(stringToNode((char *) lfirst(lc)), relation, 1));

			/* combine the row filter and cache the ExprState
			 *
			 * 合并行过滤并缓存 ExprState
			 */
			rfnode = make_orclause(filters);
			entry->exprstate[idx] = ExecPrepareExpr(rfnode, entry->estate);
		}						/* for each pubaction
		 *
		 * 对每种 pubaction
		 */
		MemoryContextSwitchTo(oldctx);

		RelationClose(relation);
	}
}

/*
 * If the table contains a generated column, check for any conflicting
 * values of 'publish_generated_columns' parameter in the publications.
 *
 * 若表含有生成列，检查各 publication 的 publish_generated_columns
 * 参数是否有冲突取值。
 */
static void
check_and_init_gencol(PGOutputData *data, List *publications,
					  RelationSyncEntry *entry)
{
	Relation	relation = RelationIdGetRelation(entry->publish_as_relid);
	TupleDesc	desc = RelationGetDescr(relation);
	bool		gencolpresent = false;
	bool		first = true;

	/* Check if there is any generated column present.
	 *
	 * 检查是否存在生成列。
	 */
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attgenerated)
		{
			gencolpresent = true;
			break;
		}
	}

	/* There are no generated columns to be published.
	 *
	 * 没有需要发布的生成列。
	 */
	if (!gencolpresent)
	{
		entry->include_gencols_type = PUBLISH_GENCOLS_NONE;
		return;
	}

	/*
	 * There may be a conflicting value for 'publish_generated_columns'
	 * parameter in the publications.
	 *
	 * 各 publication 的 publish_generated_columns 参数可能取值冲突。
	 */
	foreach_ptr(Publication, pub, publications)
	{
		/*
		 * The column list takes precedence over the
		 * 'publish_generated_columns' parameter. Those will be checked later,
		 * see pgoutput_column_list_init.
		 *
		 * 列清单优先于 publish_generated_columns 参数。这些稍后检查，见
		 * pgoutput_column_list_init。
		 */
		if (check_and_fetch_column_list(pub, entry->publish_as_relid, NULL, NULL))
			continue;

		if (first)
		{
			entry->include_gencols_type = pub->pubgencols_type;
			first = false;
		}
		else if (entry->include_gencols_type != pub->pubgencols_type)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different values of publish_generated_columns for table \"%s.%s\" in different publications",
						   get_namespace_name(RelationGetNamespace(relation)),
						   RelationGetRelationName(relation)));
	}
}

/*
 * Initialize the column list.
 *
 * 初始化列清单。
 */
static void
pgoutput_column_list_init(PGOutputData *data, List *publications,
						  RelationSyncEntry *entry)
{
	ListCell   *lc;
	bool		first = true;
	Relation	relation = RelationIdGetRelation(entry->publish_as_relid);
	bool		found_pub_collist = false;
	Bitmapset  *relcols = NULL;

	pgoutput_ensure_entry_cxt(data, entry);

	/*
	 * Find if there are any column lists for this relation. If there are,
	 * build a bitmap using the column lists.
	 *
	 * 查找该关系是否有列清单。若有，则用列清单建立位图。
	 *
	 * Multiple publications might have multiple column lists for this
	 * relation.
	 *
	 * 多个 publication 可能对该关系有多个列清单。
	 *
	 * Note that we don't support the case where the column list is different
	 * for the same table when combining publications. See comments atop
	 * fetch_table_list. But one can later change the publication so we still
	 * need to check all the given publication-table mappings and report an
	 * error if any publications have a different column list.
	 *
	 * 不支持合并 publication 时同一张表的列清单不同。参见
	 * fetch_table_list 顶部的注释。但以后仍可能修改 publication，
	 * 因此仍须检查所有给定的 publication 与表的映射，若任何 publication
	 * 的列清单不同则报错。
	 */
	foreach(lc, publications)
	{
		Publication *pub = lfirst(lc);
		Bitmapset  *cols = NULL;

		/* Retrieve the bitmap of columns for a column list publication.
		 *
		 * 取得列清单 publication 的列位图。
		 */
		found_pub_collist |= check_and_fetch_column_list(pub,
														 entry->publish_as_relid,
														 entry->entry_cxt, &cols);

		/*
		 * For non-column list publications — e.g. TABLE (without a column
		 * list), ALL TABLES, or ALL TABLES IN SCHEMA, we consider all columns
		 * of the table (including generated columns when
		 * 'publish_generated_columns' parameter is true).
		 *
		 * 对于非列清单 publication，例如不带列清单的 TABLE、ALL TABLES 或 ALL
		 * TABLES IN SCHEMA，视为包含表的全部列；当 publish_generated_columns
		 * 参数为真时也包括生成列。
		 */
		if (!cols)
		{
			/*
			 * Cache the table columns for the first publication with no
			 * specified column list to detect publication with a different
			 * column list.
			 *
			 * 缓存第一个未指定列清单的 publication 的表列，以便发现列清单不同的
			 * publication。
			 */
			if (!relcols && (list_length(publications) > 1))
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(entry->entry_cxt);

				relcols = pub_form_cols_map(relation,
											entry->include_gencols_type);
				MemoryContextSwitchTo(oldcxt);
			}

			cols = relcols;
		}

		if (first)
		{
			entry->columns = cols;
			first = false;
		}
		else if (!bms_equal(entry->columns, cols))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   get_namespace_name(RelationGetNamespace(relation)),
						   RelationGetRelationName(relation)));
	}							/* loop all subscribed publications
	 *
	 * 遍历所有已订阅的 publication
	 */

	/*
	 * If no column list publications exist, columns to be published will be
	 * computed later according to the 'publish_generated_columns' parameter.
	 *
	 * 若不存在列清单 publication，稍后将按 publish_generated_columns
	 * 参数计算要发布的列。
	 */
	if (!found_pub_collist)
		entry->columns = NULL;

	RelationClose(relation);
}

/*
 * Initialize the slot for storing new and old tuples, and build the map that
 * will be used to convert the relation's tuples into the ancestor's format.
 *
 * 初始化存放新旧元组的 slot，
 * 并建立把关系元组转换成祖先格式所用的映射。
 */
static void
init_tuple_slot(PGOutputData *data, Relation relation,
				RelationSyncEntry *entry)
{
	MemoryContext oldctx;
	TupleDesc	oldtupdesc;
	TupleDesc	newtupdesc;

	oldctx = MemoryContextSwitchTo(data->cachectx);

	/*
	 * Create tuple table slots. Create a copy of the TupleDesc as it needs to
	 * live as long as the cache remains.
	 *
	 * 创建元组表 slot。复制一份 TupleDesc，因为它的寿命必须与缓存一样长。
	 */
	oldtupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));
	newtupdesc = CreateTupleDescCopyConstr(RelationGetDescr(relation));

	entry->old_slot = MakeSingleTupleTableSlot(oldtupdesc, &TTSOpsHeapTuple);
	entry->new_slot = MakeSingleTupleTableSlot(newtupdesc, &TTSOpsHeapTuple);

	MemoryContextSwitchTo(oldctx);

	/*
	 * Cache the map that will be used to convert the relation's tuples into
	 * the ancestor's format, if needed.
	 *
	 * 若需要，缓存把关系元组转换成祖先格式所用的映射。
	 */
	if (entry->publish_as_relid != RelationGetRelid(relation))
	{
		Relation	ancestor = RelationIdGetRelation(entry->publish_as_relid);
		TupleDesc	indesc = RelationGetDescr(relation);
		TupleDesc	outdesc = RelationGetDescr(ancestor);

		/* Map must live as long as the logical decoding context.
		 *
		 * 映射的寿命必须与逻辑解码上下文一样长。
		 */
		oldctx = MemoryContextSwitchTo(data->cachectx);

		entry->attrmap = build_attrmap_by_name_if_req(indesc, outdesc, false);

		MemoryContextSwitchTo(oldctx);
		RelationClose(ancestor);
	}
}

/*
 * Change is checked against the row filter if any.
 *
 * 若有行过滤，则用它检查变更。
 *
 * Returns true if the change is to be replicated, else false.
 *
 * 若应复制该变更则返回 true，否则返回 false。
 *
 * For inserts, evaluate the row filter for new tuple.
 * For deletes, evaluate the row filter for old tuple.
 * For updates, evaluate the row filter for old and new tuple.
 *
 * 对 insert，用新元组计算行过滤。对 delete，用旧元组计算。对 update，
 * 同时用旧元组和新元组计算。
 *
 * For updates, if both evaluations are true, we allow sending the UPDATE and
 * if both the evaluations are false, it doesn't replicate the UPDATE. Now, if
 * only one of the tuples matches the row filter expression, we transform
 * UPDATE to DELETE or INSERT to avoid any data inconsistency based on the
 * following rules:
 *
 * 对 update，若两次计算都为真，则允许发送 UPDATE；若都为假，
 * 则不复制该 UPDATE。若只有其中一个元组匹配行过滤表达式，
 * 则按下述规则把 UPDATE 变成 DELETE 或 INSERT，以免数据不一致：
 *
 * Case 1: old-row (no match)    new-row (no match)  -> (drop change)
 * Case 2: old-row (no match)    new row (match)     -> INSERT
 * Case 3: old-row (match)       new-row (no match)  -> DELETE
 * Case 4: old-row (match)       new row (match)     -> UPDATE
 *
 * 情形 1：旧行不匹配且新行不匹配，则丢弃该变更。情形 2：
 * 旧行不匹配而新行匹配，则变为 INSERT。情形 3：旧行匹配而新行不匹配，
 * 则变为 DELETE。情形 4：旧行与新行都匹配，则仍为 UPDATE。
 *
 * The new action is updated in the action parameter.
 *
 * 新的动作写回 action 参数。
 *
 * The new slot could be updated when transforming the UPDATE into INSERT,
 * because the original new tuple might not have column values from the replica
 * identity.
 *
 * 把 UPDATE 变成 INSERT 时，新 slot 可能被更新，
 * 因为原来的新元组可能没有副本标识列的值。
 *
 * Examples:
 * Let's say the old tuple satisfies the row filter but the new tuple doesn't.
 * Since the old tuple satisfies, the initial table synchronization copied this
 * row (or another method was used to guarantee that there is data
 * consistency).  However, after the UPDATE the new tuple doesn't satisfy the
 * row filter, so from a data consistency perspective, that row should be
 * removed on the subscriber. The UPDATE should be transformed into a DELETE
 * statement and be sent to the subscriber. Keeping this row on the subscriber
 * is undesirable because it doesn't reflect what was defined in the row filter
 * expression on the publisher. This row on the subscriber would likely not be
 * modified by replication again. If someone inserted a new row with the same
 * old identifier, replication could stop due to a constraint violation.
 *
 * 例如：旧元组满足行过滤而新元组不满足。由于旧元组满足，
 * 初始表同步已经复制了该行，或其他方法保证了数据一致。但 UPDATE
 * 之后新元组不再满足行过滤，从数据一致性看，订阅端应删除该行。
 * 因此应把 UPDATE 变成 DELETE 发给订阅端。把该行留在订阅端并不合适，
 * 因为它不再符合发布端行过滤表达式的定义。
 * 订阅端上的这一行此后很可能不会再被复制修改。
 * 若有人插入具有相同旧标识的新行，复制可能因约束违反而停止。
 *
 * Let's say the old tuple doesn't match the row filter but the new tuple does.
 * Since the old tuple doesn't satisfy, the initial table synchronization
 * probably didn't copy this row. However, after the UPDATE the new tuple does
 * satisfy the row filter, so from a data consistency perspective, that row
 * should be inserted on the subscriber. Otherwise, subsequent UPDATE or DELETE
 * statements have no effect (it matches no row -- see
 * apply_handle_update_internal()). So, the UPDATE should be transformed into a
 * INSERT statement and be sent to the subscriber. However, this might surprise
 * someone who expects the data set to satisfy the row filter expression on the
 * provider.
 *
 * 再如：旧元组不匹配行过滤而新元组匹配。由于旧元组不满足，
 * 初始表同步大概没有复制该行。但 UPDATE 之后新元组满足行过滤，
 * 从数据一致性看，订阅端应插入该行。否则后续 UPDATE 或 DELETE
 * 没有效果，因为匹配不到行，见 apply_handle_update_internal。因此应把
 * UPDATE 变成 INSERT 发给订阅端。
 * 不过这可能让期望数据集满足提供端行过滤表达式的人感到意外。
 */
static bool
pgoutput_row_filter(Relation relation, TupleTableSlot *old_slot,
					TupleTableSlot **new_slot_ptr, RelationSyncEntry *entry,
					ReorderBufferChangeType *action)
{
	TupleDesc	desc;
	int			i;
	bool		old_matched,
				new_matched,
				result;
	TupleTableSlot *tmp_new_slot;
	TupleTableSlot *new_slot = *new_slot_ptr;
	ExprContext *ecxt;
	ExprState  *filter_exprstate;

	/*
	 * We need this map to avoid relying on ReorderBufferChangeType enums
	 * having specific values.
	 *
	 * 需要这张映射，以免依赖 ReorderBufferChangeType 枚举的具体数值。
	 */
	static const int map_changetype_pubaction[] = {
		[REORDER_BUFFER_CHANGE_INSERT] = PUBACTION_INSERT,
		[REORDER_BUFFER_CHANGE_UPDATE] = PUBACTION_UPDATE,
		[REORDER_BUFFER_CHANGE_DELETE] = PUBACTION_DELETE
	};

	Assert(*action == REORDER_BUFFER_CHANGE_INSERT ||
		   *action == REORDER_BUFFER_CHANGE_UPDATE ||
		   *action == REORDER_BUFFER_CHANGE_DELETE);

	Assert(new_slot || old_slot);

	/* Get the corresponding row filter
	 *
	 * 取得对应的行过滤
	 */
	filter_exprstate = entry->exprstate[map_changetype_pubaction[*action]];

	/* Bail out if there is no row filter
	 *
	 * 若没有行过滤则返回
	 */
	if (!filter_exprstate)
		return true;

	elog(DEBUG3, "table \"%s.%s\" has row filter",
		 get_namespace_name(RelationGetNamespace(relation)),
		 RelationGetRelationName(relation));

	ResetPerTupleExprContext(entry->estate);

	ecxt = GetPerTupleExprContext(entry->estate);

	/*
	 * For the following occasions where there is only one tuple, we can
	 * evaluate the row filter for that tuple and return.
	 *
	 * 在下列只有一个元组的场合，可以对该元组计算行过滤后返回。
	 *
	 * For inserts, we only have the new tuple.
	 *
	 * 对 insert，只有新元组。
	 *
	 * For updates, we can have only a new tuple when none of the replica
	 * identity columns changed and none of those columns have external data
	 * but we still need to evaluate the row filter for the new tuple as the
	 * existing values of those columns might not match the filter. Also,
	 * users can use constant expressions in the row filter, so we anyway need
	 * to evaluate it for the new tuple.
	 *
	 * 对 update，当副本标识列都没有变化且这些列都没有外部数据时，
	 * 可能只有新元组，但仍须用新元组计算行过滤，
	 * 因为这些列的现有值可能不匹配过滤条件。
	 * 用户也可以在行过滤中使用常量表达式，因此无论如何都要为新元组计算。
	 *
	 * For deletes, we only have the old tuple.
	 *
	 * 对 delete，只有旧元组。
	 */
	if (!new_slot || !old_slot)
	{
		ecxt->ecxt_scantuple = new_slot ? new_slot : old_slot;
		result = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

		return result;
	}

	/*
	 * Both the old and new tuples must be valid only for updates and need to
	 * be checked against the row filter.
	 *
	 * 只有 update 才会同时拥有有效的旧元组和新元组，
	 * 并且两者都要对照行过滤检查。
	 */
	Assert(map_changetype_pubaction[*action] == PUBACTION_UPDATE);

	slot_getallattrs(new_slot);
	slot_getallattrs(old_slot);

	tmp_new_slot = NULL;
	desc = RelationGetDescr(relation);

	/*
	 * The new tuple might not have all the replica identity columns, in which
	 * case it needs to be copied over from the old tuple.
	 *
	 * 新元组可能缺少部分副本标识列，此时需要从旧元组复制过来。
	 */
	for (i = 0; i < desc->natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);

		/*
		 * if the column in the new tuple or old tuple is null, nothing to do
		 *
		 * 若新元组或旧元组中的该列为 null，则无需处理
		 */
		if (new_slot->tts_isnull[i] || old_slot->tts_isnull[i])
			continue;

		/*
		 * Unchanged toasted replica identity columns are only logged in the
		 * old tuple. Copy this over to the new tuple. The changed (or WAL
		 * Logged) toast values are always assembled in memory and set as
		 * VARTAG_INDIRECT. See ReorderBufferToastReplace.
		 *
		 * 未改变的已 toast 副本标识列只记录在旧元组中。把它复制到新元组。
		 * 已改变或已写入 WAL 的 toast 值总会在内存中组装，并设为
		 * VARTAG_INDIRECT。参见 ReorderBufferToastReplace。
		 */
		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_ONDISK(new_slot->tts_values[i]) &&
			!VARATT_IS_EXTERNAL_ONDISK(old_slot->tts_values[i]))
		{
			if (!tmp_new_slot)
			{
				tmp_new_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
				ExecClearTuple(tmp_new_slot);

				memcpy(tmp_new_slot->tts_values, new_slot->tts_values,
					   desc->natts * sizeof(Datum));
				memcpy(tmp_new_slot->tts_isnull, new_slot->tts_isnull,
					   desc->natts * sizeof(bool));
			}

			tmp_new_slot->tts_values[i] = old_slot->tts_values[i];
			tmp_new_slot->tts_isnull[i] = old_slot->tts_isnull[i];
		}
	}

	ecxt->ecxt_scantuple = old_slot;
	old_matched = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

	if (tmp_new_slot)
	{
		ExecStoreVirtualTuple(tmp_new_slot);
		ecxt->ecxt_scantuple = tmp_new_slot;
	}
	else
		ecxt->ecxt_scantuple = new_slot;

	new_matched = pgoutput_row_filter_exec_expr(filter_exprstate, ecxt);

	/*
	 * Case 1: if both tuples don't match the row filter, bailout. Send
	 * nothing.
	 *
	 * 情形 1：两个元组都不匹配行过滤，则退出，什么也不发送。
	 */
	if (!old_matched && !new_matched)
		return false;

	/*
	 * Case 2: if the old tuple doesn't satisfy the row filter but the new
	 * tuple does, transform the UPDATE into INSERT.
	 *
	 * 情形 2：旧元组不满足行过滤而新元组满足，则把 UPDATE 变成 INSERT。
	 *
	 * Use the newly transformed tuple that must contain the column values for
	 * all the replica identity columns. This is required to ensure that the
	 * while inserting the tuple in the downstream node, we have all the
	 * required column values.
	 *
	 * 使用转换后的新元组，它必须包含全部副本标识列的值。
	 * 这样才能保证在下游节点插入元组时具备所有必需的列值。
	 */
	if (!old_matched && new_matched)
	{
		*action = REORDER_BUFFER_CHANGE_INSERT;

		if (tmp_new_slot)
			*new_slot_ptr = tmp_new_slot;
	}

	/*
	 * Case 3: if the old tuple satisfies the row filter but the new tuple
	 * doesn't, transform the UPDATE into DELETE.
	 *
	 * 情形 3：旧元组满足行过滤而新元组不满足，则把 UPDATE 变成 DELETE。
	 *
	 * This transformation does not require another tuple. The Old tuple will
	 * be used for DELETE.
	 *
	 * 这种转换不需要另一个元组。DELETE 将使用旧元组。
	 */
	else if (old_matched && !new_matched)
		*action = REORDER_BUFFER_CHANGE_DELETE;

	/*
	 * Case 4: if both tuples match the row filter, transformation isn't
	 * required. (*action is default UPDATE).
	 *
	 * 情形 4：两个元组都匹配行过滤，则不必转换。action 默认为 UPDATE。
	 */

	return true;
}

/*
 * Sends the decoded DML over wire.
 *
 * 把解码后的 DML 通过线路发送。
 *
 * This is called both in streaming and non-streaming modes.
 *
 * 流式与非流式模式都会调用。
 */
static void
pgoutput_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				Relation relation, ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	TransactionId xid = InvalidTransactionId;
	Relation	ancestor = NULL;
	Relation	targetrel = relation;
	ReorderBufferChangeType action = change->action;
	TupleTableSlot *old_slot = NULL;
	TupleTableSlot *new_slot = NULL;

	if (!is_publishable_relation(relation))
		return;

	/*
	 * Remember the xid for the change in streaming mode. We need to send xid
	 * with each change in the streaming mode so that subscriber can make
	 * their association and on aborts, it can discard the corresponding
	 * changes.
	 *
	 * 在流式模式下记住该变更的 xid。流式模式下每条变更都要带上 xid，
	 * 以便订阅端建立关联，并在中止时丢弃对应变更。
	 */
	if (data->in_streaming)
		xid = change->txn->xid;

	relentry = get_rel_sync_entry(data, relation);

	/* First check the table filter
	 *
	 * 先检查表过滤
	 */
	switch (action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			if (!relentry->pubactions.pubinsert)
				return;
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			if (!relentry->pubactions.pubupdate)
				return;
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			if (!relentry->pubactions.pubdelete)
				return;

			/*
			 * This is only possible if deletes are allowed even when replica
			 * identity is not defined for a table. Since the DELETE action
			 * can't be published, we simply return.
			 *
			 * 只有在表未定义副本标识也允许删除时才会出现这种情况。由于 DELETE
			 * 动作不能发布，直接返回。
			 */
			if (!change->data.tp.oldtuple)
			{
				elog(DEBUG1, "didn't send DELETE change because of missing oldtuple");
				return;
			}
			break;
		default:
			Assert(false);
	}

	/* Avoid leaking memory by using and resetting our own context
	 *
	 * 使用并重置自己的上下文，以免泄漏内存
	 */
	old = MemoryContextSwitchTo(data->context);

	/* Switch relation if publishing via root.
	 *
	 * 若通过根表发布，则切换关系。
	 */
	if (relentry->publish_as_relid != RelationGetRelid(relation))
	{
		Assert(relation->rd_rel->relispartition);
		ancestor = RelationIdGetRelation(relentry->publish_as_relid);
		targetrel = ancestor;
	}

	if (change->data.tp.oldtuple)
	{
		old_slot = relentry->old_slot;
		ExecStoreHeapTuple(change->data.tp.oldtuple, old_slot, false);

		/* Convert tuple if needed.
		 *
		 * 需要时转换元组。
		 */
		if (relentry->attrmap)
		{
			TupleTableSlot *slot = MakeTupleTableSlot(RelationGetDescr(targetrel),
													  &TTSOpsVirtual);

			old_slot = execute_attr_map_slot(relentry->attrmap, old_slot, slot);
		}
	}

	if (change->data.tp.newtuple)
	{
		new_slot = relentry->new_slot;
		ExecStoreHeapTuple(change->data.tp.newtuple, new_slot, false);

		/* Convert tuple if needed.
		 *
		 * 需要时转换元组。
		 */
		if (relentry->attrmap)
		{
			TupleTableSlot *slot = MakeTupleTableSlot(RelationGetDescr(targetrel),
													  &TTSOpsVirtual);

			new_slot = execute_attr_map_slot(relentry->attrmap, new_slot, slot);
		}
	}

	/*
	 * Check row filter.
	 *
	 * 检查行过滤。
	 *
	 * Updates could be transformed to inserts or deletes based on the results
	 * of the row filter for old and new tuple.
	 *
	 * 根据新旧元组的行过滤结果，update 可能被变成 insert 或 delete。
	 */
	if (!pgoutput_row_filter(targetrel, old_slot, &new_slot, relentry, &action))
		goto cleanup;

	/*
	 * Send BEGIN if we haven't yet.
	 *
	 * 若尚未发送 BEGIN，则发送。
	 *
	 * We send the BEGIN message after ensuring that we will actually send the
	 * change. This avoids sending a pair of BEGIN/COMMIT messages for empty
	 * transactions.
	 *
	 * 确认确实要发送该变更之后才发送 BEGIN。这样可避免为空事务发送一对
	 * BEGIN/COMMIT。
	 */
	if (txndata && !txndata->sent_begin_txn)
		pgoutput_send_begin(ctx, txn);

	/*
	 * Schema should be sent using the original relation because it also sends
	 * the ancestor's relation.
	 *
	 * 应使用原始关系发送 schema，因为同时也会发送祖先关系。
	 */
	maybe_send_schema(ctx, change, relation, relentry);

	OutputPluginPrepareWrite(ctx, true);

	/* Send the data
	 *
	 * 发送数据
	 */
	switch (action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
			logicalrep_write_insert(ctx->out, xid, targetrel, new_slot,
									data->binary, relentry->columns,
									relentry->include_gencols_type);
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			logicalrep_write_update(ctx->out, xid, targetrel, old_slot,
									new_slot, data->binary, relentry->columns,
									relentry->include_gencols_type);
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			logicalrep_write_delete(ctx->out, xid, targetrel, old_slot,
									data->binary, relentry->columns,
									relentry->include_gencols_type);
			break;
		default:
			Assert(false);
	}

	OutputPluginWrite(ctx, true);

cleanup:
	if (RelationIsValid(ancestor))
	{
		RelationClose(ancestor);
		ancestor = NULL;
	}

	/* Drop the new slots that were used to store the converted tuples.
	 *
	 * 丢弃用于存放转换后元组的新 slot。
	 */
	if (relentry->attrmap)
	{
		if (old_slot)
			ExecDropSingleTupleTableSlot(old_slot);

		if (new_slot)
			ExecDropSingleTupleTableSlot(new_slot);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

/*
 * TRUNCATE 回调：按 publication 过滤后，向订阅端发送截断。
 */
static void
pgoutput_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				  int nrelations, Relation relations[], ReorderBufferChange *change)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;
	MemoryContext old;
	RelationSyncEntry *relentry;
	int			i;
	int			nrelids;
	Oid		   *relids;
	TransactionId xid = InvalidTransactionId;

	/* Remember the xid for the change in streaming mode. See pgoutput_change.
	 *
	 * 在流式模式下记住该变更的 xid。参见 pgoutput_change。
	 */
	if (data->in_streaming)
		xid = change->txn->xid;

	old = MemoryContextSwitchTo(data->context);

	relids = palloc0(nrelations * sizeof(Oid));
	nrelids = 0;

	for (i = 0; i < nrelations; i++)
	{
		Relation	relation = relations[i];
		Oid			relid = RelationGetRelid(relation);

		if (!is_publishable_relation(relation))
			continue;

		relentry = get_rel_sync_entry(data, relation);

		if (!relentry->pubactions.pubtruncate)
			continue;

		/*
		 * Don't send partitions if the publication wants to send only the
		 * root tables through it.
		 *
		 * 若 publication 只想通过它发送根表，则不要发送分区。
		 */
		if (relation->rd_rel->relispartition &&
			relentry->publish_as_relid != relid)
			continue;

		relids[nrelids++] = relid;

		/* Send BEGIN if we haven't yet
		 *
		 * 若尚未发送 BEGIN，则发送
		 */
		if (txndata && !txndata->sent_begin_txn)
			pgoutput_send_begin(ctx, txn);

		maybe_send_schema(ctx, change, relation, relentry);
	}

	if (nrelids > 0)
	{
		OutputPluginPrepareWrite(ctx, true);
		logicalrep_write_truncate(ctx->out,
								  xid,
								  nrelids,
								  relids,
								  change->data.truncate.cascade,
								  change->data.truncate.restart_seqs);
		OutputPluginWrite(ctx, true);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);
}

/*
 * 逻辑消息回调：开启 messages 时把解码消息发给订阅端。
 */
static void
pgoutput_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 XLogRecPtr message_lsn, bool transactional, const char *prefix, Size sz,
				 const char *message)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	TransactionId xid = InvalidTransactionId;

	if (!data->messages)
		return;

	/*
	 * Remember the xid for the message in streaming mode. See
	 * pgoutput_change.
	 *
	 * 在流式模式下记住该消息的 xid。参见 pgoutput_change。
	 */
	if (data->in_streaming)
		xid = txn->xid;

	/*
	 * Output BEGIN if we haven't yet. Avoid for non-transactional messages.
	 *
	 * 若尚未输出 BEGIN 则输出。非事务性消息则避免这样做。
	 */
	if (transactional)
	{
		PGOutputTxnData *txndata = (PGOutputTxnData *) txn->output_plugin_private;

		/* Send BEGIN if we haven't yet
		 *
		 * 若尚未发送 BEGIN，则发送
		 */
		if (txndata && !txndata->sent_begin_txn)
			pgoutput_send_begin(ctx, txn);
	}

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_message(ctx->out,
							 xid,
							 message_lsn,
							 transactional,
							 prefix,
							 sz,
							 message);
	OutputPluginWrite(ctx, true);
}

/*
 * Return true if the data is associated with an origin and the user has
 * requested the changes that don't have an origin, false otherwise.
 *
 * 若数据关联了复制源，且用户要求的是没有复制源的变更，则返回 true，
 * 否则返回 false。
 */
static bool
pgoutput_origin_filter(LogicalDecodingContext *ctx,
					   RepOriginId origin_id)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;

	if (data->publish_no_origin && origin_id != InvalidRepOriginId)
		return true;

	return false;
}

/*
 * Shutdown the output plugin.
 *
 * 关闭输出插件。
 *
 * Note, we don't need to clean the data->context, data->cachectx, and
 * data->pubctx as they are child contexts of the ctx->context so they
 * will be cleaned up by logical decoding machinery.
 *
 * 注意不必清理 data 的 context、cachectx 与 pubctx，它们是 ctx 的
 * context 的子上下文，会由逻辑解码机制清理。
 */
static void
pgoutput_shutdown(LogicalDecodingContext *ctx)
{
	pgoutput_memory_context_reset(NULL);
}

/*
 * Load publications from the list of publication names.
 *
 * 按 publication 名称列表装载 publication。
 *
 * Here, we skip the publications that don't exist yet. This will allow us
 * to silently continue the replication in the absence of a missing publication.
 * This is required because we allow the users to create publications after they
 * have specified the required publications at the time of replication start.
 *
 * 这里跳过尚不存在的 publication。这样在缺少某个 publication
 * 时仍可静默继续复制。这是必需的，因为允许用户在复制启动时指定所需
 * publication 之后再创建它们。
 */
static List *
LoadPublications(List *pubnames)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, pubnames)
	{
		char	   *pubname = (char *) lfirst(lc);
		Publication *pub = GetPublicationByName(pubname, true);

		if (pub)
			result = lappend(result, pub);
		else
			ereport(WARNING,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("skipped loading publication \"%s\"", pubname),
					errdetail("The publication does not exist at this point in the WAL."),
					errhint("Create the publication if it does not exist."));
	}

	return result;
}

/*
 * Publication syscache invalidation callback.
 *
 * publication 系统缓存失效回调。
 *
 * Called for invalidations on pg_publication.
 *
 * 在 pg_publication 失效时调用。
 */
static void
publication_invalidation_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	publications_valid = false;
}

/*
 * START STREAM callback
 *
 * START STREAM 回调
 */
static void
pgoutput_stream_start(struct LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	bool		send_replication_origin = txn->origin_id != InvalidRepOriginId;

	/* we can't nest streaming of transactions
	 *
	 * 不能嵌套事务的流式传输
	 */
	Assert(!data->in_streaming);

	/*
	 * If we already sent the first stream for this transaction then don't
	 * send the origin id in the subsequent streams.
	 *
	 * 若已经为该事务发送过第一个流，则后续流不再发送 origin id。
	 */
	if (rbtxn_is_streamed(txn))
		send_replication_origin = false;

	OutputPluginPrepareWrite(ctx, !send_replication_origin);
	logicalrep_write_stream_start(ctx->out, txn->xid, !rbtxn_is_streamed(txn));

	send_repl_origin(ctx, txn->origin_id, InvalidXLogRecPtr,
					 send_replication_origin);

	OutputPluginWrite(ctx, true);

	/* we're streaming a chunk of transaction now
	 *
	 * 现在正在流式发送事务的一块
	 */
	data->in_streaming = true;
}

/*
 * STOP STREAM callback
 *
 * STOP STREAM 回调
 */
static void
pgoutput_stream_stop(struct LogicalDecodingContext *ctx,
					 ReorderBufferTXN *txn)
{
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;

	/* we should be streaming a transaction
	 *
	 * 此时应当正在流式传输一个事务
	 */
	Assert(data->in_streaming);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_stop(ctx->out);
	OutputPluginWrite(ctx, true);

	/* we've stopped streaming a transaction
	 *
	 * 已经停止流式传输一个事务
	 */
	data->in_streaming = false;
}

/*
 * Notify downstream to discard the streamed transaction (along with all
 * its subtransactions, if it's a toplevel transaction).
 *
 * 通知下游丢弃该流式事务；若它是顶层事务，则连同其全部子事务一起丢弃。
 */
static void
pgoutput_stream_abort(struct LogicalDecodingContext *ctx,
					  ReorderBufferTXN *txn,
					  XLogRecPtr abort_lsn)
{
	ReorderBufferTXN *toptxn;
	PGOutputData *data = (PGOutputData *) ctx->output_plugin_private;
	bool		write_abort_info = (data->streaming == LOGICALREP_STREAM_PARALLEL);

	/*
	 * The abort should happen outside streaming block, even for streamed
	 * transactions. The transaction has to be marked as streamed, though.
	 *
	 * 即使是流式事务，中止也应发生在流式块之外。
	 * 不过该事务必须被标记为已流式传输。
	 */
	Assert(!data->in_streaming);

	/* determine the toplevel transaction
	 *
	 * 确定顶层事务
	 */
	toptxn = rbtxn_get_toptxn(txn);

	Assert(rbtxn_is_streamed(toptxn));

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_abort(ctx->out, toptxn->xid, txn->xid, abort_lsn,
								  txn->xact_time.abort_time, write_abort_info);

	OutputPluginWrite(ctx, true);

	cleanup_rel_sync_cache(toptxn->xid, false);
}

/*
 * Notify downstream to apply the streamed transaction (along with all
 * its subtransactions).
 *
 * 通知下游应用该流式事务及其全部子事务。
 */
static void
pgoutput_stream_commit(struct LogicalDecodingContext *ctx,
					   ReorderBufferTXN *txn,
					   XLogRecPtr commit_lsn)
{
	PGOutputData *data PG_USED_FOR_ASSERTS_ONLY = (PGOutputData *) ctx->output_plugin_private;

	/*
	 * The commit should happen outside streaming block, even for streamed
	 * transactions. The transaction has to be marked as streamed, though.
	 *
	 * 即使是流式事务，提交也应发生在流式块之外。
	 * 不过该事务必须被标记为已流式传输。
	 */
	Assert(!data->in_streaming);
	Assert(rbtxn_is_streamed(txn));

	OutputPluginUpdateProgress(ctx, false);

	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_commit(ctx->out, txn, commit_lsn);
	OutputPluginWrite(ctx, true);

	cleanup_rel_sync_cache(txn->xid, true);
}

/*
 * PREPARE callback (for streaming two-phase commit).
 *
 * PREPARE 回调，用于流式两阶段提交。
 *
 * Notify the downstream to prepare the transaction.
 *
 * 通知下游预备该事务。
 */
static void
pgoutput_stream_prepare_txn(LogicalDecodingContext *ctx,
							ReorderBufferTXN *txn,
							XLogRecPtr prepare_lsn)
{
	Assert(rbtxn_is_streamed(txn));

	OutputPluginUpdateProgress(ctx, false);
	OutputPluginPrepareWrite(ctx, true);
	logicalrep_write_stream_prepare(ctx->out, txn, prepare_lsn);
	OutputPluginWrite(ctx, true);
}

/*
 * Initialize the relation schema sync cache for a decoding session.
 *
 * 为一次解码会话初始化关系 schema 同步缓存。
 *
 * The hash table is destroyed at the end of a decoding session. While
 * relcache invalidations still exist and will still be invoked, they
 * will just see the null hash table global and take no action.
 *
 * 哈希表在解码会话结束时销毁。relcache 失效仍然存在并仍会被调用，
 * 但它们只会看到空的全局哈希表，因而不做任何事。
 */
static void
init_rel_sync_cache(MemoryContext cachectx)
{
	HASHCTL		ctl;
	static bool relation_callbacks_registered = false;

	/* Nothing to do if hash table already exists
	 *
	 * 若哈希表已经存在，则无需处理
	 */
	if (RelationSyncCache != NULL)
		return;

	/* Make a new hash table for the cache
	 *
	 * 为缓存建立新的哈希表
	 */
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelationSyncEntry);
	ctl.hcxt = cachectx;

	RelationSyncCache = hash_create("logical replication output relation cache",
									128, &ctl,
									HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);

	Assert(RelationSyncCache != NULL);

	/* No more to do if we already registered callbacks
	 *
	 * 若已经注册过回调，则不必再做
	 */
	if (relation_callbacks_registered)
		return;

	/* We must update the cache entry for a relation after a relcache flush
	 *
	 * relcache 刷新之后必须更新该关系的缓存项
	 */
	CacheRegisterRelcacheCallback(rel_sync_cache_relation_cb, (Datum) 0);

	/*
	 * Flush all cache entries after a pg_namespace change, in case it was a
	 * schema rename affecting a relation being replicated.
	 *
	 * pg_namespace 变化后刷新全部缓存项，以防被复制的关系受到 schema
	 * 重命名的影响。
	 *
	 * XXX: It is not a good idea to invalidate all the relation entries in
	 * RelationSyncCache on schema rename. We can optimize it to invalidate
	 * only the required relations by either having a specific invalidation
	 * message containing impacted relations or by having schema information
	 * in each RelationSyncCache entry and using hashvalue of pg_namespace.oid
	 * passed to the callback.
	 *
	 * XXX：schema 重命名时使 RelationSyncCache
	 * 中的全部关系项失效并不理想。可以优化为只失效受影响的关系，
	 * 办法是使用包含受影响关系的专门失效消息，或在每个 RelationSyncCache
	 * 项中保存 schema 信息，并使用传给回调的 pg_namespace.oid 的
	 * hashvalue。
	 */
	CacheRegisterSyscacheCallback(NAMESPACEOID,
								  rel_sync_cache_publication_cb,
								  (Datum) 0);

	relation_callbacks_registered = true;
}

/*
 * We expect relatively small number of streamed transactions.
 *
 * 预期流式事务的数量相对较少。
 */
static bool
get_schema_sent_in_streamed_txn(RelationSyncEntry *entry, TransactionId xid)
{
	return list_member_xid(entry->streamed_txns, xid);
}

/*
 * Add the xid in the rel sync entry for which we have already sent the schema
 * of the relation.
 *
 * 把已经发送过该关系 schema 的 xid 加入关系同步项。
 */
static void
set_schema_sent_in_streamed_txn(RelationSyncEntry *entry, TransactionId xid)
{
	MemoryContext oldctx;

	oldctx = MemoryContextSwitchTo(CacheMemoryContext);

	entry->streamed_txns = lappend_xid(entry->streamed_txns, xid);

	MemoryContextSwitchTo(oldctx);
}

/*
 * Find or create entry in the relation schema cache.
 *
 * 在关系 schema 缓存中查找或创建项。
 *
 * This looks up publications that the given relation is directly or
 * indirectly part of (the latter if it's really the relation's ancestor that
 * is part of a publication) and fills up the found entry with the information
 * about which operations to publish and whether to use an ancestor's schema
 * when publishing.
 *
 * 查找给定关系直接或间接所属的 publication。
 * 间接是指其实是该关系的祖先属于某个 publication。
 * 然后把找到的项填上要发布哪些操作，以及发布时是否使用祖先 schema。
 */
static RelationSyncEntry *
get_rel_sync_entry(PGOutputData *data, Relation relation)
{
	RelationSyncEntry *entry;
	bool		found;
	MemoryContext oldctx;
	Oid			relid = RelationGetRelid(relation);

	Assert(RelationSyncCache != NULL);

	/* Find cached relation info, creating if not found
	 *
	 * 查找缓存的关系信息，若没有则创建
	 */
	entry = (RelationSyncEntry *) hash_search(RelationSyncCache,
											  &relid,
											  HASH_ENTER, &found);
	Assert(entry != NULL);

	/* initialize entry, if it's new
	 *
	 * 若是新项则初始化
	 */
	if (!found)
	{
		entry->replicate_valid = false;
		entry->schema_sent = false;
		entry->include_gencols_type = PUBLISH_GENCOLS_NONE;
		entry->streamed_txns = NIL;
		entry->pubactions.pubinsert = entry->pubactions.pubupdate =
			entry->pubactions.pubdelete = entry->pubactions.pubtruncate = false;
		entry->new_slot = NULL;
		entry->old_slot = NULL;
		memset(entry->exprstate, 0, sizeof(entry->exprstate));
		entry->entry_cxt = NULL;
		entry->publish_as_relid = InvalidOid;
		entry->columns = NULL;
		entry->attrmap = NULL;
	}

	/* Validate the entry
	 *
	 * 校验该项
	 */
	if (!entry->replicate_valid)
	{
		Oid			schemaId = get_rel_namespace(relid);
		List	   *pubids = GetRelationPublications(relid);

		/*
		 * We don't acquire a lock on the namespace system table as we build
		 * the cache entry using a historic snapshot and all the later changes
		 * are absorbed while decoding WAL.
		 *
		 * 建立缓存项时使用历史快照，之后的变更都在解码 WAL 时吸收，
		 * 因此不对命名空间系统表加锁。
		 */
		List	   *schemaPubids = GetSchemaPublications(schemaId);
		ListCell   *lc;
		Oid			publish_as_relid = relid;
		int			publish_ancestor_level = 0;
		bool		am_partition = get_rel_relispartition(relid);
		char		relkind = get_rel_relkind(relid);
		List	   *rel_publications = NIL;

		/* Reload publications if needed before use.
		 *
		 * 使用前若需要则重新装载 publication。
		 */
		if (!publications_valid)
		{
			MemoryContextReset(data->pubctx);

			oldctx = MemoryContextSwitchTo(data->pubctx);
			data->publications = LoadPublications(data->publication_names);
			MemoryContextSwitchTo(oldctx);
			publications_valid = true;
		}

		/*
		 * Reset schema_sent status as the relation definition may have
		 * changed.  Also reset pubactions to empty in case rel was dropped
		 * from a publication.  Also free any objects that depended on the
		 * earlier definition.
		 *
		 * 重置 schema_sent 状态，因为关系定义可能已改变。若关系已从
		 * publication 中去掉，也把 pubactions 清空。并释放依赖先前定义的对象。
		 */
		entry->schema_sent = false;
		entry->include_gencols_type = PUBLISH_GENCOLS_NONE;
		list_free(entry->streamed_txns);
		entry->streamed_txns = NIL;
		bms_free(entry->columns);
		entry->columns = NULL;
		entry->pubactions.pubinsert = false;
		entry->pubactions.pubupdate = false;
		entry->pubactions.pubdelete = false;
		entry->pubactions.pubtruncate = false;

		/*
		 * Tuple slots cleanups. (Will be rebuilt later if needed).
		 *
		 * 清理元组 slot。若以后需要会重建。
		 */
		if (entry->old_slot)
		{
			TupleDesc	desc = entry->old_slot->tts_tupleDescriptor;

			Assert(desc->tdrefcount == -1);

			ExecDropSingleTupleTableSlot(entry->old_slot);

			/*
			 * ExecDropSingleTupleTableSlot() would not free the TupleDesc, so
			 * do it now to avoid any leaks.
			 *
			 * ExecDropSingleTupleTableSlot 不会释放 TupleDesc，因此现在释放，
			 * 以免泄漏。
			 */
			FreeTupleDesc(desc);
		}
		if (entry->new_slot)
		{
			TupleDesc	desc = entry->new_slot->tts_tupleDescriptor;

			Assert(desc->tdrefcount == -1);

			ExecDropSingleTupleTableSlot(entry->new_slot);

			/*
			 * ExecDropSingleTupleTableSlot() would not free the TupleDesc, so
			 * do it now to avoid any leaks.
			 *
			 * ExecDropSingleTupleTableSlot 不会释放 TupleDesc，因此现在释放，
			 * 以免泄漏。
			 */
			FreeTupleDesc(desc);
		}

		entry->old_slot = NULL;
		entry->new_slot = NULL;

		if (entry->attrmap)
			free_attrmap(entry->attrmap);
		entry->attrmap = NULL;

		/*
		 * Row filter cache cleanups.
		 *
		 * 清理行过滤缓存。
		 */
		if (entry->entry_cxt)
			MemoryContextDelete(entry->entry_cxt);

		entry->entry_cxt = NULL;
		entry->estate = NULL;
		memset(entry->exprstate, 0, sizeof(entry->exprstate));

		/*
		 * Build publication cache. We can't use one provided by relcache as
		 * relcache considers all publications that the given relation is in,
		 * but here we only need to consider ones that the subscriber
		 * requested.
		 *
		 * 建立 publication 缓存。不能使用 relcache 提供的那一份，因为
		 * relcache 考虑该关系所属的全部 publication，
		 * 而这里只需要订阅端请求的那些。
		 */
		foreach(lc, data->publications)
		{
			Publication *pub = lfirst(lc);
			bool		publish = false;

			/*
			 * Under what relid should we publish changes in this publication?
			 * We'll use the top-most relid across all publications. Also
			 * track the ancestor level for this publication.
			 *
			 * 本 publication 应以哪个 relid 发布变更？将使用所有 publication
			 * 中最顶层的 relid。同时记录该 publication 的祖先层级。
			 */
			Oid			pub_relid = relid;
			int			ancestor_level = 0;

			/*
			 * If this is a FOR ALL TABLES publication, pick the partition
			 * root and set the ancestor level accordingly.
			 *
			 * 若这是 FOR ALL TABLES publication，则选取分区根并相应设置祖先层级。
			 */
			if (pub->alltables)
			{
				publish = true;
				if (pub->pubviaroot && am_partition)
				{
					List	   *ancestors = get_partition_ancestors(relid);

					pub_relid = llast_oid(ancestors);
					ancestor_level = list_length(ancestors);
				}
			}

			if (!publish)
			{
				bool		ancestor_published = false;

				/*
				 * For a partition, check if any of the ancestors are
				 * published.  If so, note down the topmost ancestor that is
				 * published via this publication, which will be used as the
				 * relation via which to publish the partition's changes.
				 *
				 * 对分区，检查是否有祖先被发布。若有，记下通过该 publication
				 * 发布的最顶层祖先，分区的变更将经由该关系发布。
				 */
				if (am_partition)
				{
					Oid			ancestor;
					int			level;
					List	   *ancestors = get_partition_ancestors(relid);

					ancestor = GetTopMostAncestorInPublication(pub->oid,
															   ancestors,
															   &level);

					if (ancestor != InvalidOid)
					{
						ancestor_published = true;
						if (pub->pubviaroot)
						{
							pub_relid = ancestor;
							ancestor_level = level;
						}
					}
				}

				if (list_member_oid(pubids, pub->oid) ||
					list_member_oid(schemaPubids, pub->oid) ||
					ancestor_published)
					publish = true;
			}

			/*
			 * If the relation is to be published, determine actions to
			 * publish, and list of columns, if appropriate.
			 *
			 * 若要发布该关系，则确定要发布的动作，并在适当时确定列清单。
			 *
			 * Don't publish changes for partitioned tables, because
			 * publishing those of its partitions suffices, unless partition
			 * changes won't be published due to pubviaroot being set.
			 *
			 * 不要发布分区表本身的变更，因为发布其分区的变更就够了，除非因设置了
			 * pubviaroot 而不会发布分区变更。
			 */
			if (publish &&
				(relkind != RELKIND_PARTITIONED_TABLE || pub->pubviaroot))
			{
				entry->pubactions.pubinsert |= pub->pubactions.pubinsert;
				entry->pubactions.pubupdate |= pub->pubactions.pubupdate;
				entry->pubactions.pubdelete |= pub->pubactions.pubdelete;
				entry->pubactions.pubtruncate |= pub->pubactions.pubtruncate;

				/*
				 * We want to publish the changes as the top-most ancestor
				 * across all publications. So we need to check if the already
				 * calculated level is higher than the new one. If yes, we can
				 * ignore the new value (as it's a child). Otherwise the new
				 * value is an ancestor, so we keep it.
				 *
				 * 希望按所有 publication 中最顶层的祖先来发布变更。
				 * 因此需要检查已经算出的层级是否高于新值。若是，则可以忽略新值，
				 * 因为它是子级。否则新值是祖先，予以保留。
				 */
				if (publish_ancestor_level > ancestor_level)
					continue;

				/*
				 * If we found an ancestor higher up in the tree, discard the
				 * list of publications through which we replicate it, and use
				 * the new ancestor.
				 *
				 * 若在树的更高处找到了祖先，则丢弃此前用来复制它的 publication 列表，
				 * 改用新的祖先。
				 */
				if (publish_ancestor_level < ancestor_level)
				{
					publish_as_relid = pub_relid;
					publish_ancestor_level = ancestor_level;

					/* reset the publication list for this relation
					 *
					 * 重置该关系的 publication 列表
					 */
					rel_publications = NIL;
				}
				else
				{
					/* Same ancestor level, has to be the same OID.
					 *
					 * 祖先层级相同，则 OID 也必须相同。
					 */
					Assert(publish_as_relid == pub_relid);
				}

				/* Track publications for this ancestor.
				 *
				 * 记录该祖先的 publication。
				 */
				rel_publications = lappend(rel_publications, pub);
			}
		}

		entry->publish_as_relid = publish_as_relid;

		/*
		 * Initialize the tuple slot, map, and row filter. These are only used
		 * when publishing inserts, updates, or deletes.
		 *
		 * 初始化元组 slot、映射与行过滤。它们只在发布 insert、update 或
		 * delete 时使用。
		 */
		if (entry->pubactions.pubinsert || entry->pubactions.pubupdate ||
			entry->pubactions.pubdelete)
		{
			/* Initialize the tuple slot and map
			 *
			 * 初始化元组 slot 与映射
			 */
			init_tuple_slot(data, relation, entry);

			/* Initialize the row filter
			 *
			 * 初始化行过滤
			 */
			pgoutput_row_filter_init(data, rel_publications, entry);

			/* Check whether to publish generated columns.
			 *
			 * 检查是否发布生成列。
			 */
			check_and_init_gencol(data, rel_publications, entry);

			/* Initialize the column list
			 *
			 * 初始化列清单
			 */
			pgoutput_column_list_init(data, rel_publications, entry);
		}

		list_free(pubids);
		list_free(schemaPubids);
		list_free(rel_publications);

		entry->replicate_valid = true;
	}

	return entry;
}

/*
 * Cleanup list of streamed transactions and update the schema_sent flag.
 *
 * 清理流式事务列表，并更新 schema_sent 标志。
 *
 * When a streamed transaction commits or aborts, we need to remove the
 * toplevel XID from the schema cache. If the transaction aborted, the
 * subscriber will simply throw away the schema records we streamed, so
 * we don't need to do anything else.
 *
 * 流式事务提交或中止时，需要从 schema 缓存中去掉顶层 XID。若事务中止，
 * 订阅端会直接丢掉我们流式发送的 schema 记录，因此不必再做其他事。
 *
 * If the transaction is committed, the subscriber will update the relation
 * cache - so tweak the schema_sent flag accordingly.
 *
 * 若事务已提交，订阅端会更新关系缓存，因此相应调整 schema_sent 标志。
 */
static void
cleanup_rel_sync_cache(TransactionId xid, bool is_commit)
{
	HASH_SEQ_STATUS hash_seq;
	RelationSyncEntry *entry;

	Assert(RelationSyncCache != NULL);

	hash_seq_init(&hash_seq, RelationSyncCache);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		/*
		 * We can set the schema_sent flag for an entry that has committed xid
		 * in the list as that ensures that the subscriber would have the
		 * corresponding schema and we don't need to send it unless there is
		 * any invalidation for that relation.
		 *
		 * 若某项的列表中有已提交的 xid，就可以设置其 schema_sent 标志。
		 * 这保证订阅端已有对应 schema，除非该关系发生失效，否则不必再发送。
		 */
		foreach_xid(streamed_txn, entry->streamed_txns)
		{
			if (xid == streamed_txn)
			{
				if (is_commit)
					entry->schema_sent = true;

				entry->streamed_txns =
					foreach_delete_current(entry->streamed_txns, streamed_txn);
				break;
			}
		}
	}
}

/*
 * Relcache invalidation callback
 *
 * relcache 失效回调
 */
static void
rel_sync_cache_relation_cb(Datum arg, Oid relid)
{
	RelationSyncEntry *entry;

	/*
	 * We can get here if the plugin was used in SQL interface as the
	 * RelationSyncCache is destroyed when the decoding finishes, but there is
	 * no way to unregister the relcache invalidation callback.
	 *
	 * 若插件通过 SQL 接口使用，解码结束时会销毁 RelationSyncCache，
	 * 但无法注销 relcache 失效回调，因此仍可能进入这里。
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * Nobody keeps pointers to entries in this hash table around outside
	 * logical decoding callback calls - but invalidation events can come in
	 * *during* a callback if we do any syscache access in the callback.
	 * Because of that we must mark the cache entry as invalid but not damage
	 * any of its substructure here.  The next get_rel_sync_entry() call will
	 * rebuild it all.
	 *
	 * 逻辑解码回调之外没有人保存该哈希表项的指针，
	 * 但若回调中访问了系统缓存，失效事件可能在回调期间到来。
	 * 因此必须把缓存项标为无效，但不要在这里破坏它的子结构。下次调用
	 * get_rel_sync_entry 时会全部重建。
	 */
	if (OidIsValid(relid))
	{
		/*
		 * Getting invalidations for relations that aren't in the table is
		 * entirely normal.  So we don't care if it's found or not.
		 *
		 * 收到不在表中的关系的失效是完全正常的。因此无论是否找到都不必在意。
		 */
		entry = (RelationSyncEntry *) hash_search(RelationSyncCache, &relid,
												  HASH_FIND, NULL);
		if (entry != NULL)
			entry->replicate_valid = false;
	}
	else
	{
		/* Whole cache must be flushed.
		 *
		 * 必须刷新整个缓存。
		 */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, RelationSyncCache);
		while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
		{
			entry->replicate_valid = false;
		}
	}
}

/*
 * Publication relation/schema map syscache invalidation callback
 *
 * publication 关系与 schema 映射的系统缓存失效回调
 *
 * Called for invalidations on pg_namespace.
 *
 * 在 pg_namespace 失效时调用。
 */
static void
rel_sync_cache_publication_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	RelationSyncEntry *entry;

	/*
	 * We can get here if the plugin was used in SQL interface as the
	 * RelationSyncCache is destroyed when the decoding finishes, but there is
	 * no way to unregister the invalidation callbacks.
	 *
	 * 若插件通过 SQL 接口使用，解码结束时会销毁 RelationSyncCache，
	 * 但无法注销失效回调，因此仍可能进入这里。
	 */
	if (RelationSyncCache == NULL)
		return;

	/*
	 * We have no easy way to identify which cache entries this invalidation
	 * event might have affected, so just mark them all invalid.
	 *
	 * 无法方便地判断该失效事件可能影响哪些缓存项，因此把它们全部标为无效。
	 */
	hash_seq_init(&status, RelationSyncCache);
	while ((entry = (RelationSyncEntry *) hash_seq_search(&status)) != NULL)
	{
		entry->replicate_valid = false;
	}
}

/* Send Replication origin
 *
 * 发送复制源
 */
static void
send_repl_origin(LogicalDecodingContext *ctx, RepOriginId origin_id,
				 XLogRecPtr origin_lsn, bool send_origin)
{
	if (send_origin)
	{
		char	   *origin;

		/*----------
		 * XXX: which behaviour do we want here?
		 *
		 * XXX：这里希望采用哪种行为？
		 *
		 * Alternatives:
		 *  - don't send origin message if origin name not found
		 *    (that's what we do now)
		 *  - throw error - that will break replication, not good
		 *  - send some special "unknown" origin
		 *
		 * 可选做法：找不到源名称时不发送 origin 消息，这是目前的做法；
		 * 或者抛出错误，那会中断复制，并不合适；或者发送某种特殊的未知源。
		 *----------
		 */
		if (replorigin_by_oid(origin_id, true, &origin))
		{
			/* Message boundary
			 *
			 * 消息边界
			 */
			OutputPluginWrite(ctx, false);
			OutputPluginPrepareWrite(ctx, true);

			logicalrep_write_origin(ctx->out, origin, origin_lsn);
		}
	}
}
