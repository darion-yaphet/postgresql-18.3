/*-------------------------------------------------------------------------
 * logical.c
 *	   PostgreSQL logical decoding coordination
 *
 * logical.c：PostgreSQL 逻辑解码的协调模块。
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/logical.c
 *
 * NOTES
 *	  This file coordinates interaction between the various modules that
 *	  together provide logical decoding, primarily by providing so
 *	  called LogicalDecodingContexts. The goal is to encapsulate most of the
 *	  internal complexity for consumers of logical decoding, so they can
 *	  create and consume a changestream with a low amount of code. Builtin
 *	  consumers are the walsender and SQL SRF interface, but it's possible to
 *	  add further ones without changing core code, e.g. to consume changes in
 *	  a bgworker.
 *
 * 说明：本文件协调各模块共同完成逻辑解码，主要靠提供 LogicalDecodingContext。目标是把大部分内部复杂性封装起来，让调用方用很少的代码就能创建并消费变更流。内建消费方是 walsender 和 SQL SRF 接口，也可以不改核心代码再增加消费方，例如在 bgworker 里消费变更。
 *
 *	  The idea is that a consumer provides three callbacks, one to read WAL,
 *	  one to prepare a data write, and a final one for actually writing since
 *	  their implementation depends on the type of consumer.  Check
 *	  logicalfuncs.c for an example implementation of a fairly simple consumer
 *	  and an implementation of a WAL reading callback that's suitable for
 *	  simple consumers.
 *
 *	  消费方提供三个回调：一个读 WAL，一个准备写出数据，一个真正写出，因为具体实现取决于消费方类型。logicalfuncs.c 中有一个较简单的消费方示例，以及适合简单消费方的 WAL 读取回调。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/slotsync.h"
#include "replication/snapbuild.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/inval.h"
#include "utils/memutils.h"

/*
 * 核心流程：
 * CheckLogicalDecodingRequirements 检查 wal_level、数据库连接，以及备库上主库的 wal_level。
 * CreateInitDecodingContext 为新建逻辑复制槽建立解码上下文并钉住安全的 xmin；
 * CreateDecodingContext 为已有槽从 confirmed_flush 继续解码。
 * DecodingContextFindStartpoint 读取 WAL，直到构建出一致的初始快照。
 * 之后由各类回调包装函数把 ReorderBuffer 的事务、行变更、流式事件和两阶段提交交给输出插件。
 * 消费端确认收到变更后，LogicalConfirmReceivedLocation 推进 confirmed_flush、目录 xmin 与 restart_lsn。
 */

/* data for errcontext callback
 *
 * errcontext 回调使用的数据。
 */
typedef struct LogicalErrorCallbackState
{
	LogicalDecodingContext *ctx;
	const char *callback_name;
	XLogRecPtr	report_location;
} LogicalErrorCallbackState;

/* wrappers around output plugin callbacks
 *
 * 输出插件回调的包装函数。
 */
static void output_plugin_error_callback(void *arg);
static void startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
							   bool is_init);
static void shutdown_cb_wrapper(LogicalDecodingContext *ctx);
static void begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							  XLogRecPtr commit_lsn);
static void begin_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn);
static void prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr prepare_lsn);
static void commit_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									   XLogRecPtr commit_lsn);
static void rollback_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
										 XLogRecPtr prepare_end_lsn, TimestampTz prepare_time);
static void change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							  Relation relation, ReorderBufferChange *change);
static void truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
								int nrelations, Relation relations[], ReorderBufferChange *change);
static void message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr message_lsn, bool transactional,
							   const char *prefix, Size message_size, const char *message);

/* streaming callbacks
 *
 * 流式解码回调。
 */
static void stream_start_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									XLogRecPtr first_lsn);
static void stream_stop_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
								   XLogRecPtr last_lsn);
static void stream_abort_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									XLogRecPtr abort_lsn);
static void stream_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									  XLogRecPtr prepare_lsn);
static void stream_commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									 XLogRecPtr commit_lsn);
static void stream_change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									 Relation relation, ReorderBufferChange *change);
static void stream_message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									  XLogRecPtr message_lsn, bool transactional,
									  const char *prefix, Size message_size, const char *message);
static void stream_truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
									   int nrelations, Relation relations[], ReorderBufferChange *change);

/* callback to update txn's progress
 *
 * 更新事务进度的回调。
 */
static void update_progress_txn_cb_wrapper(ReorderBuffer *cache,
										   ReorderBufferTXN *txn,
										   XLogRecPtr lsn);

static void LoadOutputPlugin(OutputPluginCallbacks *callbacks, const char *plugin);

/*
 * Make sure the current settings & environment are capable of doing logical
 * decoding.
 *
 * 确认当前设置与运行环境能够进行逻辑解码。
 */
void
CheckLogicalDecodingRequirements(void)
{
	CheckSlotRequirements();

	/*
	 * NB: Adding a new requirement likely means that RestoreSlotFromDisk()
	 * needs the same check.
	 *
	 * 注意：每增加一条新的前提条件，RestoreSlotFromDisk() 很可能也要做同样的检查。
	 */

	if (wal_level < WAL_LEVEL_LOGICAL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires \"wal_level\" >= \"logical\"")));

	if (MyDatabaseId == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical decoding requires a database connection")));

	if (RecoveryInProgress())
	{
		/*
		 * This check may have race conditions, but whenever
		 * XLOG_PARAMETER_CHANGE indicates that wal_level has changed, we
		 * verify that there are no existing logical replication slots. And to
		 * avoid races around creating a new slot,
		 * CheckLogicalDecodingRequirements() is called once before creating
		 * the slot, and once when logical decoding is initially starting up.
		 *
		 * 这项检查可能有竞态。每当 XLOG_PARAMETER_CHANGE 表明 wal_level 已改变，就会确认当前没有逻辑复制槽。为避免创建新槽时的竞态，CheckLogicalDecodingRequirements() 会在创建槽之前调用一次，并在逻辑解码真正启动时再调用一次。
		 */
		if (GetActiveWalLevelOnStandby() < WAL_LEVEL_LOGICAL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical decoding on standby requires \"wal_level\" >= \"logical\" on the primary")));
	}
}

/*
 * Helper function for CreateInitDecodingContext() and
 * CreateDecodingContext() performing common tasks.
 *
 * CreateInitDecodingContext() 与 CreateDecodingContext() 的公共辅助函数。
 */
static LogicalDecodingContext *
StartupDecodingContext(List *output_plugin_options,
					   XLogRecPtr start_lsn,
					   TransactionId xmin_horizon,
					   bool need_full_snapshot,
					   bool fast_forward,
					   bool in_create,
					   XLogReaderRoutine *xl_routine,
					   LogicalOutputPluginWriterPrepareWrite prepare_write,
					   LogicalOutputPluginWriterWrite do_write,
					   LogicalOutputPluginWriterUpdateProgress update_progress)
{
	ReplicationSlot *slot;
	MemoryContext context,
				old_context;
	LogicalDecodingContext *ctx;

	/* shorter lines...
	 *
	 * 为了缩短后面的代码行。
	 */
	slot = MyReplicationSlot;

	context = AllocSetContextCreate(CurrentMemoryContext,
									"Logical decoding context",
									ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(context);
	ctx = palloc0(sizeof(LogicalDecodingContext));

	ctx->context = context;

	/*
	 * (re-)load output plugins, so we detect a bad (removed) output plugin
	 * now.
	 *
	 * 重新加载输出插件，以便现在就发现已被移除的插件。
	 */
	if (!fast_forward)
		LoadOutputPlugin(&ctx->callbacks, NameStr(slot->data.plugin));

	/*
	 * Now that the slot's xmin has been set, we can announce ourselves as a
	 * logical decoding backend which doesn't need to be checked individually
	 * when computing the xmin horizon because the xmin is enforced via
	 * replication slots.
	 *
	 * 槽的 xmin 已经设定，可以把本进程登记为逻辑解码后端。计算 xmin 视界时不必再单独检查它，因为 xmin 由复制槽来保证。
	 *
	 * We can only do so if we're outside of a transaction (i.e. the case when
	 * streaming changes via walsender), otherwise an already setup
	 * snapshot/xid would end up being ignored. That's not a particularly
	 * bothersome restriction since the SQL interface can't be used for
	 * streaming anyway.
	 *
	 * 只有处于事务之外才能这样做（通过 walsender 流式发送变更时就是这种情况），否则已经建好的 snapshot 或 xid 会被忽略。SQL 接口本来就不能用于流式发送，所以这个限制影响不大。
	 */
	if (!IsTransactionOrTransactionBlock())
	{
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyProc->statusFlags |= PROC_IN_LOGICAL_DECODING;
		ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
		LWLockRelease(ProcArrayLock);
	}

	ctx->slot = slot;

	ctx->reader = XLogReaderAllocate(wal_segment_size, NULL, xl_routine, ctx);
	if (!ctx->reader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	ctx->reorder = ReorderBufferAllocate();
	ctx->snapshot_builder =
		AllocateSnapshotBuilder(ctx->reorder, xmin_horizon, start_lsn,
								need_full_snapshot, in_create, slot->data.two_phase_at);

	ctx->reorder->private_data = ctx;

	/* wrap output plugin callbacks, so we can add error context information
	 *
	 * 包装输出插件回调，以便附上错误上下文信息。
	 */
	ctx->reorder->begin = begin_cb_wrapper;
	ctx->reorder->apply_change = change_cb_wrapper;
	ctx->reorder->apply_truncate = truncate_cb_wrapper;
	ctx->reorder->commit = commit_cb_wrapper;
	ctx->reorder->message = message_cb_wrapper;

	/*
	 * To support streaming, we require start/stop/abort/commit/change
	 * callbacks. The message and truncate callbacks are optional, similar to
	 * regular output plugins. We however enable streaming when at least one
	 * of the methods is enabled so that we can easily identify missing
	 * methods.
	 *
	 * 要支持流式解码，必须有 start、stop、abort、commit、change 回调。message 和 truncate 回调是可选的，与普通输出插件一样。只要其中任一方法存在就启用流式解码，这样以后容易发现缺了哪些方法。
	 *
	 * We decide it here, but only check it later in the wrappers.
	 *
	 * 这里只作出决定，真正的检查留到包装函数里。
	 */
	ctx->streaming = (ctx->callbacks.stream_start_cb != NULL) ||
		(ctx->callbacks.stream_stop_cb != NULL) ||
		(ctx->callbacks.stream_abort_cb != NULL) ||
		(ctx->callbacks.stream_commit_cb != NULL) ||
		(ctx->callbacks.stream_change_cb != NULL) ||
		(ctx->callbacks.stream_message_cb != NULL) ||
		(ctx->callbacks.stream_truncate_cb != NULL);

	/*
	 * streaming callbacks
	 *
	 * 流式解码回调。
	 *
	 * stream_message and stream_truncate callbacks are optional, so we do not
	 * fail with ERROR when missing, but the wrappers simply do nothing. We
	 * must set the ReorderBuffer callbacks to something, otherwise the calls
	 * from there will crash (we don't want to move the checks there).
	 *
	 * stream_message 和 stream_truncate 回调是可选的，缺失时不报 ERROR，包装函数直接什么都不做。但仍必须给 ReorderBuffer 的回调赋上函数，否则那里的调用会崩溃（不想把检查挪到那边）。
	 */
	ctx->reorder->stream_start = stream_start_cb_wrapper;
	ctx->reorder->stream_stop = stream_stop_cb_wrapper;
	ctx->reorder->stream_abort = stream_abort_cb_wrapper;
	ctx->reorder->stream_prepare = stream_prepare_cb_wrapper;
	ctx->reorder->stream_commit = stream_commit_cb_wrapper;
	ctx->reorder->stream_change = stream_change_cb_wrapper;
	ctx->reorder->stream_message = stream_message_cb_wrapper;
	ctx->reorder->stream_truncate = stream_truncate_cb_wrapper;


	/*
	 * To support two-phase logical decoding, we require
	 * begin_prepare/prepare/commit-prepare/abort-prepare callbacks. The
	 * filter_prepare callback is optional. We however enable two-phase
	 * logical decoding when at least one of the methods is enabled so that we
	 * can easily identify missing methods.
	 *
	 * 要支持两阶段逻辑解码，必须有 begin_prepare、prepare、commit_prepared、abort_prepared 回调。filter_prepare 回调可选。只要其中任一方法存在就启用两阶段逻辑解码，这样以后容易发现缺了哪些方法。
	 *
	 * We decide it here, but only check it later in the wrappers.
	 *
	 * 这里只作出决定，真正的检查留到包装函数里。
	 */
	ctx->twophase = (ctx->callbacks.begin_prepare_cb != NULL) ||
		(ctx->callbacks.prepare_cb != NULL) ||
		(ctx->callbacks.commit_prepared_cb != NULL) ||
		(ctx->callbacks.rollback_prepared_cb != NULL) ||
		(ctx->callbacks.stream_prepare_cb != NULL) ||
		(ctx->callbacks.filter_prepare_cb != NULL);

	/*
	 * Callback to support decoding at prepare time.
	 *
	 * 用于在 PREPARE 时刻进行解码的回调。
	 */
	ctx->reorder->begin_prepare = begin_prepare_cb_wrapper;
	ctx->reorder->prepare = prepare_cb_wrapper;
	ctx->reorder->commit_prepared = commit_prepared_cb_wrapper;
	ctx->reorder->rollback_prepared = rollback_prepared_cb_wrapper;

	/*
	 * Callback to support updating progress during sending data of a
	 * transaction (and its subtransactions) to the output plugin.
	 *
	 * 在把一个事务及其子事务的数据发给输出插件期间，更新进度的回调。
	 */
	ctx->reorder->update_progress_txn = update_progress_txn_cb_wrapper;

	ctx->out = makeStringInfo();
	ctx->prepare_write = prepare_write;
	ctx->write = do_write;
	ctx->update_progress = update_progress;

	ctx->output_plugin_options = output_plugin_options;

	ctx->fast_forward = fast_forward;

	MemoryContextSwitchTo(old_context);

	return ctx;
}

/*
 * Create a new decoding context, for a new logical slot.
 *
 * 为一个新建的逻辑复制槽创建解码上下文。
 *
 * plugin -- contains the name of the output plugin
 * output_plugin_options -- contains options passed to the output plugin
 * need_full_snapshot -- if true, must obtain a snapshot able to read all
 *		tables; if false, one that can read only catalogs is acceptable.
 * restart_lsn -- if given as invalid, it's this routine's responsibility to
 *		mark WAL as reserved by setting a convenient restart_lsn for the slot.
 *		Otherwise, we set for decoding to start from the given LSN without
 *		marking WAL reserved beforehand.  In that scenario, it's up to the
 *		caller to guarantee that WAL remains available.
 * xl_routine -- XLogReaderRoutine for underlying XLogReader
 * prepare_write, do_write, update_progress --
 *		callbacks that perform the use-case dependent, actual, work.
 *
 * plugin 是输出插件的名字。output_plugin_options 是传给输出插件的选项。need_full_snapshot 为真时，必须取得能读取所有表的快照；为假时，只能读系统目录的快照也可以接受。restart_lsn 若传入无效值，则由本函数设置一个合适的 restart_lsn，把 WAL 标为保留；否则从给定 LSN 开始解码，事先不保留 WAL，此时由调用方保证 WAL 仍然可用。xl_routine 是底层 XLogReader 使用的 XLogReaderRoutine。prepare_write、do_write、update_progress 是随使用场景而不同的实际工作回调。
 *
 * Needs to be called while in a memory context that's at least as long lived
 * as the decoding context because further memory contexts will be created
 * inside it.
 *
 * 调用时所处的内存上下文至少要和解码上下文一样长寿，因为还会在其中创建更多内存上下文。
 *
 * Returns an initialized decoding context after calling the output plugin's
 * startup function.
 *
 * 调用输出插件的 startup 函数之后，返回已初始化的解码上下文。
 */
LogicalDecodingContext *
CreateInitDecodingContext(const char *plugin,
						  List *output_plugin_options,
						  bool need_full_snapshot,
						  XLogRecPtr restart_lsn,
						  XLogReaderRoutine *xl_routine,
						  LogicalOutputPluginWriterPrepareWrite prepare_write,
						  LogicalOutputPluginWriterWrite do_write,
						  LogicalOutputPluginWriterUpdateProgress update_progress)
{
	TransactionId xmin_horizon = InvalidTransactionId;
	ReplicationSlot *slot;
	NameData	plugin_name;
	LogicalDecodingContext *ctx;
	MemoryContext old_context;

	/*
	 * On a standby, this check is also required while creating the slot.
	 * Check the comments in the function.
	 *
	 * 在备库上创建槽时同样需要这项检查。参见该函数里的注释。
	 */
	CheckLogicalDecodingRequirements();

	/* shorter lines...
	 *
	 * 为了缩短后面的代码行。
	 */
	slot = MyReplicationSlot;

	/* first some sanity checks that are unlikely to be violated
	 *
	 * 先做一些通常不会被违反的健全性检查。
	 */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	if (plugin == NULL)
		elog(ERROR, "cannot initialize logical decoding without a specified plugin");

	/* Make sure the passed slot is suitable. These are user facing errors.
	 *
	 * 确认传入的槽适合使用。这些是面向用户的错误。
	 */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot use physical replication slot for logical decoding")));

	if (slot->data.database != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" was not created in this database",
						NameStr(slot->data.name))));

	if (IsTransactionState() &&
		GetTopTransactionIdIfAny() != InvalidTransactionId)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot create logical replication slot in transaction that has performed writes")));

	/*
	 * Register output plugin name with slot.  We need the mutex to avoid
	 * concurrent reading of a partially copied string.  But we don't want any
	 * complicated code while holding a spinlock, so do namestrcpy() outside.
	 *
	 * 把输出插件名登记到槽上。需要互斥锁，以免别人读到只拷贝了一半的字符串。持有自旋锁时不想执行复杂代码，所以 namestrcpy() 放在锁外面做。
	 */
	namestrcpy(&plugin_name, plugin);
	SpinLockAcquire(&slot->mutex);
	slot->data.plugin = plugin_name;
	SpinLockRelease(&slot->mutex);

	if (XLogRecPtrIsInvalid(restart_lsn))
		ReplicationSlotReserveWal();
	else
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);
	}

	/* ----
	 * This is a bit tricky: We need to determine a safe xmin horizon to start
	 * decoding from, to avoid starting from a running xacts record referring
	 * to xids whose rows have been vacuumed or pruned
	 * already. GetOldestSafeDecodingTransactionId() returns such a value, but
	 * without further interlock its return value might immediately be out of
	 * date.
	 *
	 * 这里比较微妙：必须确定一个安全的 xmin 视界作为解码起点，避免从一条 running xacts 记录开始，而它所引用的 xid 对应的行已经被 vacuum 或剪枝掉。GetOldestSafeDecodingTransactionId() 能返回这样的值，但若不进一步加锁互斥，返回值可能立刻就过时。
	 *
	 * So we have to acquire both the ReplicationSlotControlLock and the
	 * ProcArrayLock to prevent concurrent computation and update of new xmin
	 * horizons by other backends, get the safe decoding xid, and inform the
	 * slot machinery about the new limit. Once that's done both locks can be
	 * released as the slot machinery now is protecting against vacuum.
	 *
	 * 因此必须同时拿到 ReplicationSlotControlLock 和 ProcArrayLock，防止其他后端同时计算并更新新的 xmin 视界，然后取得安全的解码 xid，并告知槽机制这个新限制。做完之后两把锁都可以释放，此后由槽机制来防止 vacuum。
	 *
	 * Note that, temporarily, the data, not just the catalog, xmin has to be
	 * reserved if a data snapshot is to be exported.  Otherwise the initial
	 * data snapshot created here is not guaranteed to be valid. After that
	 * the data xmin doesn't need to be managed anymore and the global xmin
	 * should be recomputed. As we are fine with losing the pegged data xmin
	 * after crash - no chance a snapshot would get exported anymore - we can
	 * get away with just setting the slot's
	 * effective_xmin. ReplicationSlotRelease will reset it again.
	 *
	 * 注意：如果要导出数据快照，暂时不仅要保留目录 xmin，还要保留数据 xmin，否则这里创建的初始数据快照不能保证有效。之后数据 xmin 就不必再管理，全局 xmin 应当重新计算。崩溃后丢掉这个被钉住的数据 xmin 是可以接受的，因为快照不可能再被导出，所以只设置槽的 effective_xmin 即可。ReplicationSlotRelease 会再次把它复位。
	 *
	 * ----
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);

	xmin_horizon = GetOldestSafeDecodingTransactionId(!need_full_snapshot);

	SpinLockAcquire(&slot->mutex);
	slot->effective_catalog_xmin = xmin_horizon;
	slot->data.catalog_xmin = xmin_horizon;
	if (need_full_snapshot)
		slot->effective_xmin = xmin_horizon;
	SpinLockRelease(&slot->mutex);

	ReplicationSlotsComputeRequiredXmin(true);

	LWLockRelease(ProcArrayLock);
	LWLockRelease(ReplicationSlotControlLock);

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();

	ctx = StartupDecodingContext(NIL, restart_lsn, xmin_horizon,
								 need_full_snapshot, false, true,
								 xl_routine, prepare_write, do_write,
								 update_progress);

	/* call output plugin initialization callback
	 *
	 * 调用输出插件的初始化回调。
	 */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, true);
	MemoryContextSwitchTo(old_context);

	/*
	 * We allow decoding of prepared transactions when the two_phase is
	 * enabled at the time of slot creation, or when the two_phase option is
	 * given at the streaming start, provided the plugin supports all the
	 * callbacks for two-phase.
	 *
	 * 创建槽时若启用了 two_phase，或者开始流式传输时给出了 two_phase 选项，并且插件支持全部两阶段回调，则允许解码已准备事务。
	 */
	ctx->twophase &= slot->data.two_phase;

	ctx->reorder->output_rewrites = ctx->options.receive_rewrites;

	return ctx;
}

/*
 * Create a new decoding context, for a logical slot that has previously been
 * used already.
 *
 * 为一个先前已经使用过的逻辑复制槽创建新的解码上下文。
 *
 * start_lsn
 *		The LSN at which to start decoding.  If InvalidXLogRecPtr, restart
 *		from the slot's confirmed_flush; otherwise, start from the specified
 *		location (but move it forwards to confirmed_flush if it's older than
 *		that, see below).
 *
 * start_lsn 是开始解码的 LSN。若为 InvalidXLogRecPtr，则从槽的 confirmed_flush 重新开始；否则从指定位置开始（若它比 confirmed_flush 更旧，则向前移到 confirmed_flush，见下文）。
 *
 * output_plugin_options
 *		options passed to the output plugin.
 *
 * output_plugin_options 是传给输出插件的选项。
 *
 * fast_forward
 *		bypass the generation of logical changes.
 *
 * fast_forward 表示跳过逻辑变更的生成。
 *
 * xl_routine
 *		XLogReaderRoutine used by underlying xlogreader
 *
 * xl_routine 是底层 xlogreader 使用的 XLogReaderRoutine。
 *
 * prepare_write, do_write, update_progress
 *		callbacks that have to be filled to perform the use-case dependent,
 *		actual work.
 *
 * prepare_write、do_write、update_progress 是必须填上的、随使用场景而不同的实际工作回调。
 *
 * Needs to be called while in a memory context that's at least as long lived
 * as the decoding context because further memory contexts will be created
 * inside it.
 *
 * 调用时所处的内存上下文至少要和解码上下文一样长寿，因为还会在其中创建更多内存上下文。
 *
 * Returns an initialized decoding context after calling the output plugin's
 * startup function.
 *
 * 调用输出插件的 startup 函数之后，返回已初始化的解码上下文。
 */
LogicalDecodingContext *
CreateDecodingContext(XLogRecPtr start_lsn,
					  List *output_plugin_options,
					  bool fast_forward,
					  XLogReaderRoutine *xl_routine,
					  LogicalOutputPluginWriterPrepareWrite prepare_write,
					  LogicalOutputPluginWriterWrite do_write,
					  LogicalOutputPluginWriterUpdateProgress update_progress)
{
	LogicalDecodingContext *ctx;
	ReplicationSlot *slot;
	MemoryContext old_context;

	/* shorter lines...
	 *
	 * 为了缩短后面的代码行。
	 */
	slot = MyReplicationSlot;

	/* first some sanity checks that are unlikely to be violated
	 *
	 * 先做一些通常不会被违反的健全性检查。
	 */
	if (slot == NULL)
		elog(ERROR, "cannot perform logical decoding without an acquired slot");

	/* make sure the passed slot is suitable, these are user facing errors
	 *
	 * 确认传入的槽适合使用，这些是面向用户的错误。
	 */
	if (SlotIsPhysical(slot))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot use physical replication slot for logical decoding")));

	/*
	 * We need to access the system tables during decoding to build the
	 * logical changes unless we are in fast_forward mode where no changes are
	 * generated.
	 *
	 * 解码时需要访问系统表来构建逻辑变更，除非处于 fast_forward 模式，该模式下不会生成变更。
	 */
	if (slot->data.database != MyDatabaseId && !fast_forward)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slot \"%s\" was not created in this database",
						NameStr(slot->data.name))));

	/*
	 * The slots being synced from the primary can't be used for decoding as
	 * they are used after failover. However, we do allow advancing the LSNs
	 * during the synchronization of slots. See update_local_synced_slot.
	 *
	 * 从主库同步过来的槽不能用于解码，它们要等故障转移之后才使用。不过在同步槽的过程中允许推进 LSN。参见 update_local_synced_slot。
	 */
	if (RecoveryInProgress() && slot->data.synced && !IsSyncingReplicationSlots())
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot use replication slot \"%s\" for logical decoding",
					   NameStr(slot->data.name)),
				errdetail("This replication slot is being synchronized from the primary server."),
				errhint("Specify another replication slot."));

	/* slot must be valid to allow decoding
	 *
	 * 槽必须有效，才允许解码。
	 */
	Assert(slot->data.invalidated == RS_INVAL_NONE);
	Assert(slot->data.restart_lsn != InvalidXLogRecPtr);

	if (start_lsn == InvalidXLogRecPtr)
	{
		/* continue from last position
		 *
		 * 从上次的位置继续。
		 */
		start_lsn = slot->data.confirmed_flush;
	}
	else if (start_lsn < slot->data.confirmed_flush)
	{
		/*
		 * It might seem like we should error out in this case, but it's
		 * pretty common for a client to acknowledge a LSN it doesn't have to
		 * do anything for, and thus didn't store persistently, because the
		 * xlog records didn't result in anything relevant for logical
		 * decoding. Clients have to be able to do that to support synchronous
		 * replication.
		 *
		 * 这种情况看起来应该报错，但客户端确认一个自己不必处理、因而没有持久保存的 LSN 十分常见，因为那些 xlog 记录对逻辑解码没有产生相关结果。客户端必须能这样做，才能支持同步复制。
		 *
		 * Starting at a different LSN than requested might not catch certain
		 * kinds of client errors; so the client may wish to check that
		 * confirmed_flush_lsn matches its expectations.
		 *
		 * 从与所请求不同的 LSN 开始，可能发现不了某些客户端错误；因此客户端也许希望检查 confirmed_flush_lsn 是否符合自己的预期。
		 */
		elog(LOG, "%X/%X has been already streamed, forwarding to %X/%X",
			 LSN_FORMAT_ARGS(start_lsn),
			 LSN_FORMAT_ARGS(slot->data.confirmed_flush));

		start_lsn = slot->data.confirmed_flush;
	}

	ctx = StartupDecodingContext(output_plugin_options,
								 start_lsn, InvalidTransactionId, false,
								 fast_forward, false, xl_routine, prepare_write,
								 do_write, update_progress);

	/* call output plugin initialization callback
	 *
	 * 调用输出插件的初始化回调。
	 */
	old_context = MemoryContextSwitchTo(ctx->context);
	if (ctx->callbacks.startup_cb != NULL)
		startup_cb_wrapper(ctx, &ctx->options, false);
	MemoryContextSwitchTo(old_context);

	/*
	 * We allow decoding of prepared transactions when the two_phase is
	 * enabled at the time of slot creation, or when the two_phase option is
	 * given at the streaming start, provided the plugin supports all the
	 * callbacks for two-phase.
	 *
	 * 创建槽时若启用了 two_phase，或者开始流式传输时给出了 two_phase 选项，并且插件支持全部两阶段回调，则允许解码已准备事务。
	 */
	ctx->twophase &= (slot->data.two_phase || ctx->twophase_opt_given);

	/* Mark slot to allow two_phase decoding if not already marked
	 *
	 * 若尚未标记，则把槽标为允许 two_phase 解码。
	 */
	if (ctx->twophase && !slot->data.two_phase)
	{
		SpinLockAcquire(&slot->mutex);
		slot->data.two_phase = true;
		slot->data.two_phase_at = start_lsn;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
		SnapBuildSetTwoPhaseAt(ctx->snapshot_builder, start_lsn);
	}

	ctx->reorder->output_rewrites = ctx->options.receive_rewrites;

	ereport(LOG,
			(errmsg("starting logical decoding for slot \"%s\"",
					NameStr(slot->data.name)),
			 errdetail("Streaming transactions committing after %X/%X, reading WAL from %X/%X.",
					   LSN_FORMAT_ARGS(slot->data.confirmed_flush),
					   LSN_FORMAT_ARGS(slot->data.restart_lsn))));

	return ctx;
}

/*
 * Returns true if a consistent initial decoding snapshot has been built.
 *
 * 若已经构建出一致的初始解码快照，则返回真。
 */
bool
DecodingContextReady(LogicalDecodingContext *ctx)
{
	return SnapBuildCurrentState(ctx->snapshot_builder) == SNAPBUILD_CONSISTENT;
}

/*
 * Read from the decoding slot, until it is ready to start extracting changes.
 *
 * 从解码槽中读取，直到可以开始提取变更。
 */
void
DecodingContextFindStartpoint(LogicalDecodingContext *ctx)
{
	ReplicationSlot *slot = ctx->slot;

	/* Initialize from where to start reading WAL.
	 *
	 * 初始化从哪里开始读取 WAL。
	 */
	XLogBeginRead(ctx->reader, slot->data.restart_lsn);

	elog(DEBUG1, "searching for logical decoding starting point, starting at %X/%X",
		 LSN_FORMAT_ARGS(slot->data.restart_lsn));

	/* Wait for a consistent starting point
	 *
	 * 等待一个一致的起始点。
	 */
	for (;;)
	{
		XLogRecord *record;
		char	   *err = NULL;

		/* the read_page callback waits for new WAL
		 *
		 * read_page 回调会等待新的 WAL。
		 */
		record = XLogReadRecord(ctx->reader, &err);
		if (err)
			elog(ERROR, "could not find logical decoding starting point: %s", err);
		if (!record)
			elog(ERROR, "could not find logical decoding starting point");

		LogicalDecodingProcessRecord(ctx, ctx->reader);

		/* only continue till we found a consistent spot
		 *
		 * 只继续到找到一致点为止。
		 */
		if (DecodingContextReady(ctx))
			break;

		CHECK_FOR_INTERRUPTS();
	}

	SpinLockAcquire(&slot->mutex);
	slot->data.confirmed_flush = ctx->reader->EndRecPtr;
	if (slot->data.two_phase)
		slot->data.two_phase_at = ctx->reader->EndRecPtr;
	SpinLockRelease(&slot->mutex);
}

/*
 * Free a previously allocated decoding context, invoking the shutdown
 * callback if necessary.
 *
 * 释放先前分配的解码上下文，必要时调用 shutdown 回调。
 */
void
FreeDecodingContext(LogicalDecodingContext *ctx)
{
	if (ctx->callbacks.shutdown_cb != NULL)
		shutdown_cb_wrapper(ctx);

	ReorderBufferFree(ctx->reorder);
	FreeSnapshotBuilder(ctx->snapshot_builder);
	XLogReaderFree(ctx->reader);
	MemoryContextDelete(ctx->context);
}

/*
 * Prepare a write using the context's output routine.
 *
 * 使用上下文的输出例程准备一次写出。
 */
void
OutputPluginPrepareWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->accept_writes)
		elog(ERROR, "writes are only accepted in commit, begin and change callbacks");

	ctx->prepare_write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = true;
}

/*
 * Perform a write using the context's output routine.
 *
 * 使用上下文的输出例程执行一次写出。
 */
void
OutputPluginWrite(struct LogicalDecodingContext *ctx, bool last_write)
{
	if (!ctx->prepared_write)
		elog(ERROR, "OutputPluginPrepareWrite needs to be called before OutputPluginWrite");

	ctx->write(ctx, ctx->write_location, ctx->write_xid, last_write);
	ctx->prepared_write = false;
}

/*
 * Update progress tracking (if supported).
 *
 * 更新进度跟踪（若支持）。
 */
void
OutputPluginUpdateProgress(struct LogicalDecodingContext *ctx,
						   bool skipped_xact)
{
	if (!ctx->update_progress)
		return;

	ctx->update_progress(ctx, ctx->write_location, ctx->write_xid,
						 skipped_xact);
}

/*
 * Load the output plugin, lookup its output plugin init function, and check
 * that it provides the required callbacks.
 *
 * 加载输出插件，查找其初始化函数，并检查它是否提供了必需的回调。
 */
static void
LoadOutputPlugin(OutputPluginCallbacks *callbacks, const char *plugin)
{
	LogicalOutputPluginInit plugin_init;

	plugin_init = (LogicalOutputPluginInit)
		load_external_function(plugin, "_PG_output_plugin_init", false, NULL);

	if (plugin_init == NULL)
		elog(ERROR, "output plugins have to declare the _PG_output_plugin_init symbol");

	/* ask the output plugin to fill the callback struct
	 *
	 * 让输出插件填充回调结构体。
	 */
	plugin_init(callbacks);

	if (callbacks->begin_cb == NULL)
		elog(ERROR, "output plugins have to register a begin callback");
	if (callbacks->change_cb == NULL)
		elog(ERROR, "output plugins have to register a change callback");
	if (callbacks->commit_cb == NULL)
		elog(ERROR, "output plugins have to register a commit callback");
}

/*
 * 输出插件回调出错时使用的 errcontext 回调，把当前回调名和 LSN 写入错误上下文。
 */
static void
output_plugin_error_callback(void *arg)
{
	LogicalErrorCallbackState *state = (LogicalErrorCallbackState *) arg;

	/* not all callbacks have an associated LSN
	 *
	 * 并非所有回调都带有关联的 LSN。
	 */
	if (state->report_location != InvalidXLogRecPtr)
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback, associated LSN %X/%X",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name,
				   LSN_FORMAT_ARGS(state->report_location));
	else
		errcontext("slot \"%s\", output plugin \"%s\", in the %s callback",
				   NameStr(state->ctx->slot->data.name),
				   NameStr(state->ctx->slot->data.plugin),
				   state->callback_name);
}

/*
 * 包装输出插件的 startup 回调，压入错误上下文后调用插件。
 */
static void
startup_cb_wrapper(LogicalDecodingContext *ctx, OutputPluginOptions *opt, bool is_init)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "startup";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.startup_cb(ctx, opt, is_init);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装输出插件的 shutdown 回调，压入错误上下文后调用插件。
 */
static void
shutdown_cb_wrapper(LogicalDecodingContext *ctx)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "shutdown";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.shutdown_cb(ctx);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}


/*
 * Callbacks for ReorderBuffer which add in some more information and then call
 * output_plugin.h plugins.
 *
 * 供 ReorderBuffer 使用的回调：补充一些信息后，再调用 output_plugin.h 中的插件。
 */
static void
begin_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "begin";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.begin_cb(ctx, txn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 commit 回调，向输出插件报告事务提交。
 */
static void
commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "commit";
	state.report_location = txn->final_lsn; /* beginning of commit record
	 *
	 * 提交记录的起始处。
	 */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record
	 *
	 * 指向该记录的末尾。
	 */
	ctx->end_xact = true;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.commit_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * The functionality of begin_prepare is quite similar to begin with the
 * exception that this will have gid (global transaction id) information which
 * can be used by plugin. Now, we thought about extending the existing begin
 * but that would break the replication protocol and additionally this looks
 * cleaner.
 *
 * begin_prepare 的功能与 begin 很相似，区别是它带有 gid（全局事务标识），插件可以使用。曾经考虑扩展已有的 begin，但那会破坏复制协议，而且单独的回调更清晰。
 */
static void
begin_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported
	 *
	 * 只有在支持两阶段提交时才应该调用这里。
	 */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "begin_prepare";
	state.report_location = txn->first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->first_lsn;
	ctx->end_xact = false;

	/*
	 * If the plugin supports two-phase commits then begin prepare callback is
	 * mandatory
	 *
	 * 若插件支持两阶段提交，则 begin prepare 回调是必需的。
	 */
	if (ctx->callbacks.begin_prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"begin_prepare_cb")));

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.begin_prepare_cb(ctx, txn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 prepare 回调，向输出插件报告两阶段事务的 PREPARE。
 */
static void
prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr prepare_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported
	 *
	 * 只有在支持两阶段提交时才应该调用这里。
	 */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "prepare";
	state.report_location = txn->final_lsn; /* beginning of prepare record
	 *
	 * PREPARE 记录的起始处。
	 */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record
	 *
	 * 指向该记录的末尾。
	 */
	ctx->end_xact = true;

	/*
	 * If the plugin supports two-phase commits then prepare callback is
	 * mandatory
	 *
	 * 若插件支持两阶段提交，则 prepare 回调是必需的。
	 */
	if (ctx->callbacks.prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"prepare_cb")));

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.prepare_cb(ctx, txn, prepare_lsn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 commit_prepared 回调，向输出插件报告已准备事务的提交。
 */
static void
commit_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						   XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported
	 *
	 * 只有在支持两阶段提交时才应该调用这里。
	 */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "commit_prepared";
	state.report_location = txn->final_lsn; /* beginning of commit record
	 *
	 * 提交记录的起始处。
	 */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record
	 *
	 * 指向该记录的末尾。
	 */
	ctx->end_xact = true;

	/*
	 * If the plugin support two-phase commits then commit prepared callback
	 * is mandatory
	 *
	 * 若插件支持两阶段提交，则 commit prepared 回调是必需的。
	 */
	if (ctx->callbacks.commit_prepared_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"commit_prepared_cb")));

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.commit_prepared_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 rollback_prepared 回调，向输出插件报告已准备事务的回滚。
 */
static void
rollback_prepared_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							 XLogRecPtr prepare_end_lsn,
							 TimestampTz prepare_time)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when two-phase commits are supported
	 *
	 * 只有在支持两阶段提交时才应该调用这里。
	 */
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "rollback_prepared";
	state.report_location = txn->final_lsn; /* beginning of commit record
	 *
	 * 提交记录的起始处。
	 */
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn; /* points to the end of the record
	 *
	 * 指向该记录的末尾。
	 */
	ctx->end_xact = true;

	/*
	 * If the plugin support two-phase commits then rollback prepared callback
	 * is mandatory
	 *
	 * 若插件支持两阶段提交，则 rollback prepared 回调是必需的。
	 */
	if (ctx->callbacks.rollback_prepared_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication at prepare time requires a %s callback",
						"rollback_prepared_cb")));

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.rollback_prepared_cb(ctx, txn, prepare_end_lsn,
										prepare_time);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 change 回调，把一行变更交给输出插件。
 */
static void
change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				  Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 *
	 * 报告这条变更的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.change_cb(ctx, txn, relation, change);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 truncate 回调，把截断操作交给输出插件。
 */
static void
truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
					int nrelations, Relation relations[], ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	if (!ctx->callbacks.truncate_cb)
		return;

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "truncate";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 *
	 * 报告这条变更的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.truncate_cb(ctx, txn, nrelations, relations, change);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 filter_prepare 回调，由插件决定是否输出该准备事务。
 */
bool
filter_prepare_cb_wrapper(LogicalDecodingContext *ctx, TransactionId xid,
						  const char *gid)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "filter_prepare";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ret = ctx->callbacks.filter_prepare_cb(ctx, xid, gid);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;

	return ret;
}

/*
 * 包装 filter_by_origin 回调，按复制源过滤变更。
 */
bool
filter_by_origin_cb_wrapper(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;
	bool		ret;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "filter_by_origin";
	state.report_location = InvalidXLogRecPtr;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = false;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ret = ctx->callbacks.filter_by_origin_cb(ctx, origin_id);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;

	return ret;
}

/*
 * 包装 message 回调，把逻辑解码消息交给输出插件。
 */
static void
message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
				   XLogRecPtr message_lsn, bool transactional,
				   const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	if (ctx->callbacks.message_cb == NULL)
		return;

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.message_cb(ctx, txn, message_lsn, transactional, prefix,
							  message_size, message);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_start 回调，通知插件开始流式发送某个事务。
 */
static void
stream_start_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						XLogRecPtr first_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_start";
	state.report_location = first_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this message's lsn so replies from clients can give an
	 * up-to-date answer. This won't ever be enough (and shouldn't be!) to
	 * confirm receipt of this transaction, but it might allow another
	 * transaction's commit to be confirmed with one message.
	 *
	 * 报告这条消息的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = first_lsn;

	ctx->end_xact = false;

	/* in streaming mode, stream_start_cb is required
	 *
	 * 流式模式下必须提供 stream_start_cb。
	 */
	if (ctx->callbacks.stream_start_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_start_cb")));

	ctx->callbacks.stream_start_cb(ctx, txn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_stop 回调，通知插件暂停流式发送。
 */
static void
stream_stop_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
					   XLogRecPtr last_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_stop";
	state.report_location = last_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this message's lsn so replies from clients can give an
	 * up-to-date answer. This won't ever be enough (and shouldn't be!) to
	 * confirm receipt of this transaction, but it might allow another
	 * transaction's commit to be confirmed with one message.
	 *
	 * 报告这条消息的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = last_lsn;

	ctx->end_xact = false;

	/* in streaming mode, stream_stop_cb is required
	 *
	 * 流式模式下必须提供 stream_stop_cb。
	 */
	if (ctx->callbacks.stream_stop_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_stop_cb")));

	ctx->callbacks.stream_stop_cb(ctx, txn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_abort 回调，通知插件流式事务已中止。
 */
static void
stream_abort_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						XLogRecPtr abort_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_abort";
	state.report_location = abort_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = abort_lsn;
	ctx->end_xact = true;

	/* in streaming mode, stream_abort_cb is required
	 *
	 * 流式模式下必须提供 stream_abort_cb。
	 */
	if (ctx->callbacks.stream_abort_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_abort_cb")));

	ctx->callbacks.stream_abort_cb(ctx, txn, abort_lsn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_prepare 回调，通知插件流式事务已经 PREPARE。
 */
static void
stream_prepare_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						  XLogRecPtr prepare_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/*
	 * We're only supposed to call this when streaming and two-phase commits
	 * are supported.
	 *
	 * 只有在同时支持流式解码和两阶段提交时才应该调用这里。
	 */
	Assert(ctx->streaming);
	Assert(ctx->twophase);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_prepare";
	state.report_location = txn->final_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn;
	ctx->end_xact = true;

	/* in streaming mode with two-phase commits, stream_prepare_cb is required
	 *
	 * 在流式模式并且支持两阶段提交时，必须提供 stream_prepare_cb。
	 */
	if (ctx->callbacks.stream_prepare_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming at prepare time requires a %s callback",
						"stream_prepare_cb")));

	ctx->callbacks.stream_prepare_cb(ctx, txn, prepare_lsn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_commit 回调，通知插件流式事务已经提交。
 */
static void
stream_commit_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						 XLogRecPtr commit_lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_commit";
	state.report_location = txn->final_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;
	ctx->write_location = txn->end_lsn;
	ctx->end_xact = true;

	/* in streaming mode, stream_commit_cb is required
	 *
	 * 流式模式下必须提供 stream_commit_cb。
	 */
	if (ctx->callbacks.stream_commit_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_commit_cb")));

	ctx->callbacks.stream_commit_cb(ctx, txn, commit_lsn);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_change 回调，在流式模式下把行变更交给插件。
 */
static void
stream_change_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						 Relation relation, ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_change";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 *
	 * 报告这条变更的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	/* in streaming mode, stream_change_cb is required
	 *
	 * 流式模式下必须提供 stream_change_cb。
	 */
	if (ctx->callbacks.stream_change_cb == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical streaming requires a %s callback",
						"stream_change_cb")));

	ctx->callbacks.stream_change_cb(ctx, txn, relation, change);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_message 回调，在流式模式下把逻辑消息交给插件。
 */
static void
stream_message_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						  XLogRecPtr message_lsn, bool transactional,
						  const char *prefix, Size message_size, const char *message)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* this callback is optional
	 *
	 * 此回调是可选的。
	 */
	if (ctx->callbacks.stream_message_cb == NULL)
		return;

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_message";
	state.report_location = message_lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn != NULL ? txn->xid : InvalidTransactionId;
	ctx->write_location = message_lsn;
	ctx->end_xact = false;

	/* do the actual work: call callback
	 *
	 * 做实际工作：调用回调。
	 */
	ctx->callbacks.stream_message_cb(ctx, txn, message_lsn, transactional, prefix,
									 message_size, message);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装 stream_truncate 回调，在流式模式下把截断交给插件。
 */
static void
stream_truncate_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
						   int nrelations, Relation relations[],
						   ReorderBufferChange *change)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* We're only supposed to call this when streaming is supported.
	 *
	 * 只有在支持流式解码时才应该调用这里。
	 */
	Assert(ctx->streaming);

	/* this callback is optional
	 *
	 * 此回调是可选的。
	 */
	if (!ctx->callbacks.stream_truncate_cb)
		return;

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "stream_truncate";
	state.report_location = change->lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = true;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 *
	 * 报告这条变更的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = change->lsn;

	ctx->end_xact = false;

	ctx->callbacks.stream_truncate_cb(ctx, txn, nrelations, relations, change);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * 包装事务进度回调，在向插件发送事务数据时更新解码进度。
 */
static void
update_progress_txn_cb_wrapper(ReorderBuffer *cache, ReorderBufferTXN *txn,
							   XLogRecPtr lsn)
{
	LogicalDecodingContext *ctx = cache->private_data;
	LogicalErrorCallbackState state;
	ErrorContextCallback errcallback;

	Assert(!ctx->fast_forward);

	/* Push callback + info on the error context stack
	 *
	 * 把回调及其信息压入错误上下文栈。
	 */
	state.ctx = ctx;
	state.callback_name = "update_progress_txn";
	state.report_location = lsn;
	errcallback.callback = output_plugin_error_callback;
	errcallback.arg = &state;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* set output state
	 *
	 * 设置输出状态。
	 */
	ctx->accept_writes = false;
	ctx->write_xid = txn->xid;

	/*
	 * Report this change's lsn so replies from clients can give an up-to-date
	 * answer. This won't ever be enough (and shouldn't be!) to confirm
	 * receipt of this transaction, but it might allow another transaction's
	 * commit to be confirmed with one message.
	 *
	 * 报告这条变更的 LSN，使客户端的回复能反映最新位置。这永远不足以（也不应当用来）确认已经收到本事务，但也许能让另一个事务的提交用一条消息得到确认。
	 */
	ctx->write_location = lsn;

	ctx->end_xact = false;

	OutputPluginUpdateProgress(ctx, false);

	/* Pop the error context stack
	 *
	 * 从错误上下文栈弹出。
	 */
	error_context_stack = errcallback.previous;
}

/*
 * Set the required catalog xmin horizon for historic snapshots in the current
 * replication slot.
 *
 * 为当前复制槽设置历史快照所需的目录 xmin 视界。
 *
 * Note that in the most cases, we won't be able to immediately use the xmin
 * to increase the xmin horizon: we need to wait till the client has confirmed
 * receiving current_lsn with LogicalConfirmReceivedLocation().
 *
 * 多数情况下不能立刻用这个 xmin 来抬高 xmin 视界：必须等到客户端用 LogicalConfirmReceivedLocation() 确认已经收到 current_lsn。
 */
void
LogicalIncreaseXminForSlot(XLogRecPtr current_lsn, TransactionId xmin)
{
	bool		updated_xmin = false;
	ReplicationSlot *slot;
	bool		got_new_xmin = false;

	slot = MyReplicationSlot;

	Assert(slot != NULL);

	SpinLockAcquire(&slot->mutex);

	/*
	 * don't overwrite if we already have a newer xmin. This can happen if we
	 * restart decoding in a slot.
	 *
	 * 如果已经有更新的 xmin，就不要覆盖。在一个槽里重新开始解码时可能出现这种情况。
	 */
	if (TransactionIdPrecedesOrEquals(xmin, slot->data.catalog_xmin))
	{
	}

	/*
	 * If the client has already confirmed up to this lsn, we directly can
	 * mark this as accepted. This can happen if we restart decoding in a
	 * slot.
	 *
	 * 如果客户端已经确认到这个 LSN，可以直接把它标为已接受。在一个槽里重新开始解码时可能出现这种情况。
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/* our candidate can directly be used
		 *
		 * 候选值可以直接使用。
		 */
		updated_xmin = true;
	}

	/*
	 * Only increase if the previous values have been applied, otherwise we
	 * might never end up updating if the receiver acks too slowly.
	 *
	 * 只有先前的值已经生效才继续抬高，否则接收方确认太慢时，可能永远更新不了。
	 */
	else if (slot->candidate_xmin_lsn == InvalidXLogRecPtr)
	{
		slot->candidate_catalog_xmin = xmin;
		slot->candidate_xmin_lsn = current_lsn;

		/*
		 * Log new xmin at an appropriate log level after releasing the
		 * spinlock.
		 *
		 * 释放自旋锁之后，以合适的日志级别记下新的 xmin。
		 */
		got_new_xmin = true;
	}
	SpinLockRelease(&slot->mutex);

	if (got_new_xmin)
		elog(DEBUG1, "got new catalog xmin %u at %X/%X", xmin,
			 LSN_FORMAT_ARGS(current_lsn));

	/* candidate already valid with the current flush position, apply
	 *
	 * 候选值相对于当前刷盘位置已经有效，直接应用。
	 */
	if (updated_xmin)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * Mark the minimal LSN (restart_lsn) we need to read to replay all
 * transactions that have not yet committed at current_lsn.
 *
 * 标记为了重放 current_lsn 处尚未提交的全部事务而必须读取的最小 LSN，即 restart_lsn。
 *
 * Just like LogicalIncreaseXminForSlot this only takes effect when the
 * client has confirmed to have received current_lsn.
 *
 * 和 LogicalIncreaseXminForSlot 一样，只有客户端确认已经收到 current_lsn 之后才会生效。
 */
void
LogicalIncreaseRestartDecodingForSlot(XLogRecPtr current_lsn, XLogRecPtr restart_lsn)
{
	bool		updated_lsn = false;
	ReplicationSlot *slot;

	slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(restart_lsn != InvalidXLogRecPtr);
	Assert(current_lsn != InvalidXLogRecPtr);

	SpinLockAcquire(&slot->mutex);

	/* don't overwrite if have a newer restart lsn
	 *
	 * 如果已经有更新的 restart_lsn，就不要覆盖。
	 */
	if (restart_lsn <= slot->data.restart_lsn)
	{
		SpinLockRelease(&slot->mutex);
	}

	/*
	 * We might have already flushed far enough to directly accept this lsn,
	 * in this case there is no need to check for existing candidate LSNs
	 *
	 * 也许已经刷盘到足够远，可以直接接受这个 LSN，这时不必再检查已有的候选 LSN。
	 */
	else if (current_lsn <= slot->data.confirmed_flush)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		/* our candidate can directly be used
		 *
		 * 候选值可以直接使用。
		 */
		updated_lsn = true;
	}

	/*
	 * Only increase if the previous values have been applied, otherwise we
	 * might never end up updating if the receiver acks too slowly. A missed
	 * value here will just cause some extra effort after reconnecting.
	 *
	 * 只有先前的值已经生效才继续抬高，否则接收方确认太慢时可能永远更新不了。这里漏掉一个值，重连之后只是多做一点工作。
	 */
	else if (slot->candidate_restart_valid == InvalidXLogRecPtr)
	{
		slot->candidate_restart_valid = current_lsn;
		slot->candidate_restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		elog(DEBUG1, "got new restart lsn %X/%X at %X/%X",
			 LSN_FORMAT_ARGS(restart_lsn),
			 LSN_FORMAT_ARGS(current_lsn));
	}
	else
	{
		XLogRecPtr	candidate_restart_lsn;
		XLogRecPtr	candidate_restart_valid;
		XLogRecPtr	confirmed_flush;

		candidate_restart_lsn = slot->candidate_restart_lsn;
		candidate_restart_valid = slot->candidate_restart_valid;
		confirmed_flush = slot->data.confirmed_flush;
		SpinLockRelease(&slot->mutex);

		elog(DEBUG1, "failed to increase restart lsn: proposed %X/%X, after %X/%X, current candidate %X/%X, current after %X/%X, flushed up to %X/%X",
			 LSN_FORMAT_ARGS(restart_lsn),
			 LSN_FORMAT_ARGS(current_lsn),
			 LSN_FORMAT_ARGS(candidate_restart_lsn),
			 LSN_FORMAT_ARGS(candidate_restart_valid),
			 LSN_FORMAT_ARGS(confirmed_flush));
	}

	/* candidates are already valid with the current flush position, apply
	 *
	 * 这些候选值相对于当前刷盘位置已经有效，直接应用。
	 */
	if (updated_lsn)
		LogicalConfirmReceivedLocation(slot->data.confirmed_flush);
}

/*
 * Handle a consumer's confirmation having received all changes up to lsn.
 *
 * 处理消费方发出的确认：已经收到直到该 LSN 的全部变更。
 */
void
LogicalConfirmReceivedLocation(XLogRecPtr lsn)
{
	Assert(lsn != InvalidXLogRecPtr);

	/* Do an unlocked check for candidate_lsn first.
	 *
	 * 先不加锁检查 candidate_lsn。
	 */
	if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr ||
		MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr)
	{
		bool		updated_xmin = false;
		bool		updated_restart = false;
		XLogRecPtr	restart_lsn pg_attribute_unused();

		SpinLockAcquire(&MyReplicationSlot->mutex);

		/* remember the old restart lsn
		 *
		 * 记住原来的 restart_lsn。
		 */
		restart_lsn = MyReplicationSlot->data.restart_lsn;

		/*
		 * Prevent moving the confirmed_flush backwards, as this could lead to
		 * data duplication issues caused by replicating already replicated
		 * changes.
		 *
		 * 禁止把 confirmed_flush 向后移动，否则会把已经复制过的变更再复制一次，造成数据重复。
		 *
		 * This can happen when a client acknowledges an LSN it doesn't have
		 * to do anything for, and thus didn't store persistently. After a
		 * restart, the client can send the prior LSN that it stored
		 * persistently as an acknowledgement, but we need to ignore such an
		 * LSN. See similar case handling in CreateDecodingContext.
		 *
		 * 当客户端确认一个自己不必处理、因而没有持久保存的 LSN 时就会这样。重启之后，客户端可能把先前持久保存的那个更旧的 LSN 当作确认发来，这种 LSN 必须忽略。类似处理见 CreateDecodingContext。
		 */
		if (lsn > MyReplicationSlot->data.confirmed_flush)
			MyReplicationSlot->data.confirmed_flush = lsn;

		/* if we're past the location required for bumping xmin, do so
		 *
		 * 如果已经越过抬高 xmin 所需的位置，就抬高它。
		 */
		if (MyReplicationSlot->candidate_xmin_lsn != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_xmin_lsn <= lsn)
		{
			/*
			 * We have to write the changed xmin to disk *before* we change
			 * the in-memory value, otherwise after a crash we wouldn't know
			 * that some catalog tuples might have been removed already.
			 *
			 * 必须先把改变后的 xmin 写到磁盘，再修改内存中的值，否则崩溃之后无法知道某些目录元组可能已经被删掉。
			 *
			 * Ensure that by first writing to ->xmin and only update
			 * ->effective_xmin once the new state is synced to disk. After a
			 * crash ->effective_xmin is set to ->xmin.
			 *
			 * 做法是先写入 xmin，等新状态同步到磁盘之后再更新 effective_xmin。崩溃之后 effective_xmin 会被设成 xmin。
			 */
			if (TransactionIdIsValid(MyReplicationSlot->candidate_catalog_xmin) &&
				MyReplicationSlot->data.catalog_xmin != MyReplicationSlot->candidate_catalog_xmin)
			{
				MyReplicationSlot->data.catalog_xmin = MyReplicationSlot->candidate_catalog_xmin;
				MyReplicationSlot->candidate_catalog_xmin = InvalidTransactionId;
				MyReplicationSlot->candidate_xmin_lsn = InvalidXLogRecPtr;
				updated_xmin = true;
			}
		}

		if (MyReplicationSlot->candidate_restart_valid != InvalidXLogRecPtr &&
			MyReplicationSlot->candidate_restart_valid <= lsn)
		{
			Assert(MyReplicationSlot->candidate_restart_lsn != InvalidXLogRecPtr);

			MyReplicationSlot->data.restart_lsn = MyReplicationSlot->candidate_restart_lsn;
			MyReplicationSlot->candidate_restart_lsn = InvalidXLogRecPtr;
			MyReplicationSlot->candidate_restart_valid = InvalidXLogRecPtr;
			updated_restart = true;
		}

		SpinLockRelease(&MyReplicationSlot->mutex);

		/* first write new xmin to disk, so we know what's up after a crash
		 *
		 * 先把新的 xmin 写到磁盘，以便崩溃之后知道当时的状态。
		 */
		if (updated_xmin || updated_restart)
		{
#ifdef USE_INJECTION_POINTS
			XLogSegNo	seg1,
						seg2;

			XLByteToSeg(restart_lsn, seg1, wal_segment_size);
			XLByteToSeg(MyReplicationSlot->data.restart_lsn, seg2, wal_segment_size);

			/* trigger injection point, but only if segment changes
			 *
			 * 触发注入点，但仅当 WAL 段发生变化时。
			 */
			if (seg1 != seg2)
				INJECTION_POINT("logical-replication-slot-advance-segment", NULL);
#endif

			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			elog(DEBUG1, "updated xmin: %u restart: %u", updated_xmin, updated_restart);
		}

		/*
		 * Now the new xmin is safely on disk, we can let the global value
		 * advance. We do not take ProcArrayLock or similar since we only
		 * advance xmin here and there's not much harm done by a concurrent
		 * computation missing that.
		 *
		 * 新的 xmin 已经安全落盘，可以让全局值向前推进。这里只推进 xmin，不必获取 ProcArrayLock 之类的锁，并发计算漏掉这次推进也没有太大害处。
		 */
		if (updated_xmin)
		{
			SpinLockAcquire(&MyReplicationSlot->mutex);
			MyReplicationSlot->effective_catalog_xmin = MyReplicationSlot->data.catalog_xmin;
			SpinLockRelease(&MyReplicationSlot->mutex);

			ReplicationSlotsComputeRequiredXmin(false);
			ReplicationSlotsComputeRequiredLSN();
		}
	}
	else
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);

		/*
		 * Prevent moving the confirmed_flush backwards. See comments above
		 * for the details.
		 *
		 * 禁止把 confirmed_flush 向后移动。详情见上面的注释。
		 */
		if (lsn > MyReplicationSlot->data.confirmed_flush)
			MyReplicationSlot->data.confirmed_flush = lsn;

		SpinLockRelease(&MyReplicationSlot->mutex);
	}
}

/*
 * Clear logical streaming state during (sub)transaction abort.
 *
 * 在子事务或事务中止时，清除逻辑流式发送的状态。
 */
void
ResetLogicalStreamingState(void)
{
	CheckXidAlive = InvalidTransactionId;
	bsysscan = false;
}

/*
 * Report stats for a slot.
 *
 * 报告一个复制槽的统计信息。
 */
void
UpdateDecodingStats(LogicalDecodingContext *ctx)
{
	ReorderBuffer *rb = ctx->reorder;
	PgStat_StatReplSlotEntry repSlotStat;

	/* Nothing to do if we don't have any replication stats to be sent.
	 *
	 * 如果没有待发送的复制统计信息，就什么都不做。
	 */
	if (rb->spillBytes <= 0 && rb->streamBytes <= 0 && rb->totalBytes <= 0)
		return;

	elog(DEBUG2, "UpdateDecodingStats: updating stats %p %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64,
		 rb,
		 rb->spillTxns,
		 rb->spillCount,
		 rb->spillBytes,
		 rb->streamTxns,
		 rb->streamCount,
		 rb->streamBytes,
		 rb->totalTxns,
		 rb->totalBytes);

	repSlotStat.spill_txns = rb->spillTxns;
	repSlotStat.spill_count = rb->spillCount;
	repSlotStat.spill_bytes = rb->spillBytes;
	repSlotStat.stream_txns = rb->streamTxns;
	repSlotStat.stream_count = rb->streamCount;
	repSlotStat.stream_bytes = rb->streamBytes;
	repSlotStat.total_txns = rb->totalTxns;
	repSlotStat.total_bytes = rb->totalBytes;

	pgstat_report_replslot(ctx->slot, &repSlotStat);

	rb->spillTxns = 0;
	rb->spillCount = 0;
	rb->spillBytes = 0;
	rb->streamTxns = 0;
	rb->streamCount = 0;
	rb->streamBytes = 0;
	rb->totalTxns = 0;
	rb->totalBytes = 0;
}

/*
 * Read up to the end of WAL starting from the decoding slot's restart_lsn.
 * Return true if any meaningful/decodable WAL records are encountered,
 * otherwise false.
 *
 * 从解码槽的 restart_lsn 一直读到 WAL 末尾。若遇到任何有意义、可解码的 WAL 记录则返回真，否则返回假。
 */
bool
LogicalReplicationSlotHasPendingWal(XLogRecPtr end_of_wal)
{
	bool		has_pending_wal = false;

	Assert(MyReplicationSlot);

	PG_TRY();
	{
		LogicalDecodingContext *ctx;

		/*
		 * Create our decoding context in fast_forward mode, passing start_lsn
		 * as InvalidXLogRecPtr, so that we start processing from the slot's
		 * confirmed_flush.
		 *
		 * 以 fast_forward 模式创建解码上下文，并把 start_lsn 传为 InvalidXLogRecPtr，从而从槽的 confirmed_flush 开始处理。
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,	/* fast_forward
									 *
									 * 快进模式。
									 */
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

		/*
		 * Start reading at the slot's restart_lsn, which we know points to a
		 * valid record.
		 *
		 * 从槽的 restart_lsn 开始读取，已知它指向一条有效记录。
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* Invalidate non-timetravel entries
		 *
		 * 使非时间旅行缓存项失效。
		 */
		InvalidateSystemCaches();

		/* Loop until the end of WAL or some changes are processed
		 *
		 * 循环，直到 WAL 结束，或者已经处理了一些变更。
		 */
		while (!has_pending_wal && ctx->reader->EndRecPtr < end_of_wal)
		{
			XLogRecord *record;
			char	   *errm = NULL;

			record = XLogReadRecord(ctx->reader, &errm);

			if (errm)
				elog(ERROR, "could not find record for logical decoding: %s", errm);

			if (record != NULL)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			has_pending_wal = ctx->processing_required;

			CHECK_FOR_INTERRUPTS();
		}

		/* Clean up
		 *
		 * 清理。
		 */
		FreeDecodingContext(ctx);
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

	return has_pending_wal;
}

/*
 * Helper function for advancing our logical replication slot forward.
 *
 * 把逻辑复制槽向前推进的辅助函数。
 *
 * The slot's restart_lsn is used as start point for reading records, while
 * confirmed_flush is used as base point for the decoding context.
 *
 * 读取记录时以槽的 restart_lsn 为起点，解码上下文则以 confirmed_flush 为基准点。
 *
 * We cannot just do LogicalConfirmReceivedLocation to update confirmed_flush,
 * because we need to digest WAL to advance restart_lsn allowing to recycle
 * WAL and removal of old catalog tuples.  As decoding is done in fast_forward
 * mode, no changes are generated anyway.
 *
 * 不能只调用 LogicalConfirmReceivedLocation 来更新 confirmed_flush，因为必须消化 WAL 才能推进 restart_lsn，从而回收 WAL 并删除旧的目录元组。解码在 fast_forward 模式下进行，反正不会生成变更。
 *
 * *found_consistent_snapshot will be true if the initial decoding snapshot has
 * been built; Otherwise, it will be false.
 *
 * 若已经构建出初始解码快照，则 found_consistent_snapshot 所指向的值为真，否则为假。
 */
XLogRecPtr
LogicalSlotAdvanceAndCheckSnapState(XLogRecPtr moveto,
									bool *found_consistent_snapshot)
{
	LogicalDecodingContext *ctx;
	ResourceOwner old_resowner = CurrentResourceOwner;
	XLogRecPtr	retlsn;

	Assert(moveto != InvalidXLogRecPtr);

	if (found_consistent_snapshot)
		*found_consistent_snapshot = false;

	PG_TRY();
	{
		/*
		 * Create our decoding context in fast_forward mode, passing start_lsn
		 * as InvalidXLogRecPtr, so that we start processing from my slot's
		 * confirmed_flush.
		 *
		 * 以 fast_forward 模式创建解码上下文，并把 start_lsn 传为 InvalidXLogRecPtr，从而从本槽的 confirmed_flush 开始处理。
		 */
		ctx = CreateDecodingContext(InvalidXLogRecPtr,
									NIL,
									true,	/* fast_forward
									 *
									 * 快进模式。
									 */
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

		/*
		 * Wait for specified streaming replication standby servers (if any)
		 * to confirm receipt of WAL up to moveto lsn.
		 *
		 * 等待指定的流式复制备库（如果有）确认已经收到直到 moveto 的 WAL。
		 */
		WaitForStandbyConfirmation(moveto);

		/*
		 * Start reading at the slot's restart_lsn, which we know to point to
		 * a valid record.
		 *
		 * 从槽的 restart_lsn 开始读取，已知它指向一条有效记录。
		 */
		XLogBeginRead(ctx->reader, MyReplicationSlot->data.restart_lsn);

		/* invalidate non-timetravel entries
		 *
		 * 使非时间旅行缓存项失效。
		 */
		InvalidateSystemCaches();

		/* Decode records until we reach the requested target
		 *
		 * 解码记录，直到到达所请求的目标位置。
		 */
		while (ctx->reader->EndRecPtr < moveto)
		{
			char	   *errm = NULL;
			XLogRecord *record;

			/*
			 * Read records.  No changes are generated in fast_forward mode,
			 * but snapbuilder/slot statuses are updated properly.
			 *
			 * 读取记录。fast_forward 模式不会生成变更，但快照构建器和槽的状态会得到正确更新。
			 */
			record = XLogReadRecord(ctx->reader, &errm);
			if (errm)
				elog(ERROR, "could not find record while advancing replication slot: %s",
					 errm);

			/*
			 * Process the record.  Storage-level changes are ignored in
			 * fast_forward mode, but other modules (such as snapbuilder)
			 * might still have critical updates to do.
			 *
			 * 处理这条记录。fast_forward 模式会忽略存储层的变更，但其他模块（例如快照构建器）可能仍有必须完成的关键更新。
			 */
			if (record)
				LogicalDecodingProcessRecord(ctx, ctx->reader);

			CHECK_FOR_INTERRUPTS();
		}

		if (found_consistent_snapshot && DecodingContextReady(ctx))
			*found_consistent_snapshot = true;

		/*
		 * Logical decoding could have clobbered CurrentResourceOwner during
		 * transaction management, so restore the executor's value.  (This is
		 * a kluge, but it's not worth cleaning up right now.)
		 *
		 * 逻辑解码在事务管理过程中可能改掉 CurrentResourceOwner，因此把执行器原来的值恢复回来。（这是权宜做法，眼下不值得专门清理。）
		 */
		CurrentResourceOwner = old_resowner;

		if (ctx->reader->EndRecPtr != InvalidXLogRecPtr)
		{
			LogicalConfirmReceivedLocation(moveto);

			/*
			 * If only the confirmed_flush LSN has changed the slot won't get
			 * marked as dirty by the above. Callers on the walsender
			 * interface are expected to keep track of their own progress and
			 * don't need it written out. But SQL-interface users cannot
			 * specify their own start positions and it's harder for them to
			 * keep track of their progress, so we should make more of an
			 * effort to save it for them.
			 *
			 * 如果只是 confirmed_flush 的 LSN 变了，上面的逻辑不会把槽标为脏。walsender 接口的调用方应当自己跟踪进度，不必把这个位置写出去。SQL 接口的用户不能指定自己的起始位置，也更难跟踪进度，所以应该更努力地为他们保存。
			 *
			 * Dirty the slot so it is written out at the next checkpoint. The
			 * LSN position advanced to may still be lost on a crash but this
			 * makes the data consistent after a clean shutdown.
			 *
			 * 把槽标为脏，以便下一次检查点把它写出去。推进到的 LSN 在崩溃时仍可能丢失，但这样可以在干净关闭之后保持数据一致。
			 */
			ReplicationSlotMarkDirty();
		}

		retlsn = MyReplicationSlot->data.confirmed_flush;

		/* free context, call shutdown callback
		 *
		 * 释放上下文，并调用 shutdown 回调。
		 */
		FreeDecodingContext(ctx);

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

	return retlsn;
}
