/* -------------------------------------------------------------------------
 *
 * decode.c
 *		This module decodes WAL records read using xlogreader.h's APIs for the
 *		purpose of logical decoding by passing information to the
 *		reorderbuffer module (containing the actual changes) and to the
 *		snapbuild module to build a fitting catalog snapshot (to be able to
 *		properly decode the changes in the reorderbuffer).
 *
 * 本模块用 xlogreader.h 的接口读取 WAL 记录并解码，供逻辑解码使用：把
 * 实际变更交给 reorderbuffer 模块，并把信息交给 snapbuild 模块以建立合
 * 适的目录快照，从而能在 reorderbuffer 里正确解码这些变更。
 *
 * NOTE:
 *		This basically tries to handle all low level xlog stuff for
 *		reorderbuffer.c and snapbuild.c. There's some minor leakage where a
 *		specific record's struct is used to pass data along, but those just
 *		happen to contain the right amount of data in a convenient
 *		format. There isn't and shouldn't be much intelligence about the
 *		contents of records in here except turning them into a more usable
 *		format.
 *
 * 注意：这里基本上是为 reorderbuffer.c 和 snapbuild.c 处理所有底层
 * xlog 事务。有少量泄漏，会用某条记录自己的结构来传递数据，但那些结构
 * 恰好以方便的格式带有适量数据。这里不该、也不应该对记录内容有太多理解，
 * 只是把它们转成更好用的格式。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/decode.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"
#include "catalog/pg_control.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/message.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "storage/standbydefs.h"

/*
 * 核心流程：
 * LogicalDecodingProcessRecord 取出每条 WAL，先按需把子事务归到顶层事务，
 * 再按资源管理器分发到 xact_decode、heap_decode、heap2_decode、logicalmsg_decode 等。
 * 堆变更解析后进入 reorderbuffer；提交、prepare 和中止交给 snapbuild 维护快照，
 * 快照一致之后再回调输出插件。快照未就绪或 fast_forward 时跳过数据变更。
 */

/* individual record(group)'s handlers
 *
 * 各条记录（分组）的处理函数
 */
static void DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeTruncate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);
static void DecodeSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf);

static void DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						 xl_xact_parsed_commit *parsed, TransactionId xid,
						 bool two_phase);
static void DecodeAbort(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						xl_xact_parsed_abort *parsed, TransactionId xid,
						bool two_phase);
static void DecodePrepare(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
						  xl_xact_parsed_prepare *parsed);


/* common function to decode tuples
 *
 * 解码元组的公共函数
 */
static void DecodeXLogTuple(char *data, Size len, HeapTuple tuple);

/* helper functions for decoding transactions
 *
 * 解码事务的辅助函数
 */
static inline bool FilterPrepare(LogicalDecodingContext *ctx,
								 TransactionId xid, const char *gid);
static bool DecodeTXNNeedSkip(LogicalDecodingContext *ctx,
							  XLogRecordBuffer *buf, Oid txn_dbid,
							  RepOriginId origin_id);

/*
 * Take every XLogReadRecord()ed record and perform the actions required to
 * decode it using the output plugin already setup in the logical decoding
 * context.
 *
 * 取出每一条由 XLogReadRecord() 读到的记录，并用逻辑解码上下文里已经设
 * 置好的输出插件执行解码所需的动作。
 *
 * NB: Note that every record's xid needs to be processed by reorderbuffer
 * (xids contained in the content of records are not relevant for this rule).
 * That means that for records which'd otherwise not go through the
 * reorderbuffer ReorderBufferProcessXid() has to be called. We don't want to
 * call ReorderBufferProcessXid for each record type by default, because
 * e.g. empty xacts can be handled more efficiently if there's no previous
 * state for them.
 *
 * 注意：每条记录的 xid 都要交给 reorderbuffer 处理（记录内容里包含的
 * xid 不在此列）。因此，那些本来不会经过 reorderbuffer 的记录必须调用
 * ReorderBufferProcessXid()。默认情况下不想对每种记录类型都调用
 * ReorderBufferProcessXid，因为例如空事务在没有先前状态时可以处理得更
 * 高效。
 *
 * We also support the ability to fast forward thru records, skipping some
 * record types completely - see individual record types for details.
 *
 * 也支持在记录中快进，完全跳过某些记录类型。详见各记录类型。
 */
void
LogicalDecodingProcessRecord(LogicalDecodingContext *ctx, XLogReaderState *record)
{
	XLogRecordBuffer buf;
	TransactionId txid;
	RmgrData	rmgr;

	buf.origptr = ctx->reader->ReadRecPtr;
	buf.endptr = ctx->reader->EndRecPtr;
	buf.record = record;

	txid = XLogRecGetTopXid(record);

	/*
	 * If the top-level xid is valid, we need to assign the subxact to the
	 * top-level xact. We need to do this for all records, hence we do it
	 * before the switch.
	 *
	 * 若顶层 xid 有效，需要把子事务归到顶层事务。所有记录都要这样做，因此
	 * 放在 switch 之前。
	 */
	if (TransactionIdIsValid(txid))
	{
		ReorderBufferAssignChild(ctx->reorder,
								 txid,
								 XLogRecGetXid(record),
								 buf.origptr);
	}

	rmgr = GetRmgr(XLogRecGetRmid(record));

	if (rmgr.rm_decode != NULL)
		rmgr.rm_decode(ctx, &buf);
	else
	{
		/* just deal with xid, and done
		 *
		 * 只处理 xid，然后结束
		 */
		ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(record),
								buf.origptr);
	}
}

/*
 * Handle rmgr XLOG_ID records for LogicalDecodingProcessRecord().
 *
 * 为 LogicalDecodingProcessRecord() 处理资源管理器 XLOG_ID 的记录。
 */
void
xlog_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	uint8		info = XLogRecGetInfo(buf->record) & ~XLR_INFO_MASK;

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(buf->record),
							buf->origptr);

	switch (info)
	{
			/* this is also used in END_OF_RECOVERY checkpoints
			 *
			 * END_OF_RECOVERY 检查点也会用到这个
			 */
		case XLOG_CHECKPOINT_SHUTDOWN:
		case XLOG_END_OF_RECOVERY:
			SnapBuildSerializationPoint(builder, buf->origptr);

			break;
		case XLOG_CHECKPOINT_ONLINE:

			/*
			 * a RUNNING_XACTS record will have been logged near to this, we
			 * can restart from there.
			 *
			 * 附近会记有一条 RUNNING_XACTS 记录，可以从那里重新开始。
			 */
			break;
		case XLOG_PARAMETER_CHANGE:
			{
				xl_parameter_change *xlrec =
					(xl_parameter_change *) XLogRecGetData(buf->record);

				/*
				 * If wal_level on the primary is reduced to less than
				 * logical, we want to prevent existing logical slots from
				 * being used.  Existing logical slots on the standby get
				 * invalidated when this WAL record is replayed; and further,
				 * slot creation fails when wal_level is not sufficient; but
				 * all these operations are not synchronized, so a logical
				 * slot may creep in while the wal_level is being reduced.
				 * Hence this extra check.
				 *
				 * 若主库上的 wal_level 被降到低于 logical，要防止已有的逻辑槽继续被使
				 * 用。这条 WAL 记录在备库重放时会使已有逻辑槽失效；而且 wal_level 不够
				 * 时创建槽会失败。但这些操作并不同步，因此在降低 wal_level 的过程中仍
				 * 可能溜进一个逻辑槽。所以要多做这一次检查。
				 */
				if (xlrec->wal_level < WAL_LEVEL_LOGICAL)
				{
					/*
					 * This can occur only on a standby, as a primary would
					 * not allow to restart after changing wal_level < logical
					 * if there is pre-existing logical slot.
					 *
					 * 这只可能发生在备库上。主库在存在已有逻辑槽时，不会允许在把 wal_level
					 * 改到低于 logical 之后再重启。
					 */
					Assert(RecoveryInProgress());
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("logical decoding on standby requires \"wal_level\" >= \"logical\" on the primary")));
				}
				break;
			}
		case XLOG_NOOP:
		case XLOG_NEXTOID:
		case XLOG_SWITCH:
		case XLOG_BACKUP_END:
		case XLOG_RESTORE_POINT:
		case XLOG_FPW_CHANGE:
		case XLOG_FPI_FOR_HINT:
		case XLOG_FPI:
		case XLOG_OVERWRITE_CONTRECORD:
		case XLOG_CHECKPOINT_REDO:
			break;
		default:
			elog(ERROR, "unexpected RM_XLOG_ID record type: %u", info);
	}
}

/*
 * Handle rmgr XACT_ID records for LogicalDecodingProcessRecord().
 *
 * 为 LogicalDecodingProcessRecord() 处理资源管理器 XACT_ID 的记录。
 */
void
xact_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	ReorderBuffer *reorder = ctx->reorder;
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & XLOG_XACT_OPMASK;

	/*
	 * If the snapshot isn't yet fully built, we cannot decode anything, so
	 * bail out.
	 *
	 * 若快照尚未完全建好，就什么都不能解码，因此直接返回。
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_XACT_COMMIT:
		case XLOG_XACT_COMMIT_PREPARED:
			{
				xl_xact_commit *xlrec;
				xl_xact_parsed_commit parsed;
				TransactionId xid;
				bool		two_phase = false;

				xlrec = (xl_xact_commit *) XLogRecGetData(r);
				ParseCommitRecord(XLogRecGetInfo(buf->record), xlrec, &parsed);

				if (!TransactionIdIsValid(parsed.twophase_xid))
					xid = XLogRecGetXid(r);
				else
					xid = parsed.twophase_xid;

				/*
				 * We would like to process the transaction in a two-phase
				 * manner iff output plugin supports two-phase commits and
				 * doesn't filter the transaction at prepare time.
				 *
				 * 我们希望以两阶段方式处理该事务，当且仅当输出插件支持两阶段提交，并且
				 * 在 prepare 时没有过滤掉该事务。
				 */
				if (info == XLOG_XACT_COMMIT_PREPARED)
					two_phase = !(FilterPrepare(ctx, xid,
												parsed.twophase_gid));

				DecodeCommit(ctx, buf, &parsed, xid, two_phase);
				break;
			}
		case XLOG_XACT_ABORT:
		case XLOG_XACT_ABORT_PREPARED:
			{
				xl_xact_abort *xlrec;
				xl_xact_parsed_abort parsed;
				TransactionId xid;
				bool		two_phase = false;

				xlrec = (xl_xact_abort *) XLogRecGetData(r);
				ParseAbortRecord(XLogRecGetInfo(buf->record), xlrec, &parsed);

				if (!TransactionIdIsValid(parsed.twophase_xid))
					xid = XLogRecGetXid(r);
				else
					xid = parsed.twophase_xid;

				/*
				 * We would like to process the transaction in a two-phase
				 * manner iff output plugin supports two-phase commits and
				 * doesn't filter the transaction at prepare time.
				 *
				 * 我们希望以两阶段方式处理该事务，当且仅当输出插件支持两阶段提交，并且
				 * 在 prepare 时没有过滤掉该事务。
				 */
				if (info == XLOG_XACT_ABORT_PREPARED)
					two_phase = !(FilterPrepare(ctx, xid,
												parsed.twophase_gid));

				DecodeAbort(ctx, buf, &parsed, xid, two_phase);
				break;
			}
		case XLOG_XACT_ASSIGNMENT:

			/*
			 * We assign subxact to the toplevel xact while processing each
			 * record if required.  So, we don't need to do anything here. See
			 * LogicalDecodingProcessRecord.
			 *
			 * 处理每条记录时，如有需要已经把子事务归到顶层事务。因此这里不必再做什
			 * 么。见 LogicalDecodingProcessRecord。
			 */
			break;
		case XLOG_XACT_INVALIDATIONS:
			{
				TransactionId xid;
				xl_xact_invals *invals;

				xid = XLogRecGetXid(r);
				invals = (xl_xact_invals *) XLogRecGetData(r);

				/*
				 * Execute the invalidations for xid-less transactions,
				 * otherwise, accumulate them so that they can be processed at
				 * the commit time.
				 *
				 * 对没有 xid 的事务立即执行失效；否则先累积起来，到提交时再处理。
				 */
				if (TransactionIdIsValid(xid))
				{
					if (!ctx->fast_forward)
						ReorderBufferAddInvalidations(reorder, xid,
													  buf->origptr,
													  invals->nmsgs,
													  invals->msgs);
					ReorderBufferXidSetCatalogChanges(ctx->reorder, xid,
													  buf->origptr);
				}
				else if (!ctx->fast_forward)
					ReorderBufferImmediateInvalidation(ctx->reorder,
													   invals->nmsgs,
													   invals->msgs);

				break;
			}
		case XLOG_XACT_PREPARE:
			{
				xl_xact_parsed_prepare parsed;
				xl_xact_prepare *xlrec;

				/* ok, parse it
				 *
				 * 好，解析它
				 */
				xlrec = (xl_xact_prepare *) XLogRecGetData(r);
				ParsePrepareRecord(XLogRecGetInfo(buf->record),
								   xlrec, &parsed);

				/*
				 * We would like to process the transaction in a two-phase
				 * manner iff output plugin supports two-phase commits and
				 * doesn't filter the transaction at prepare time.
				 *
				 * 我们希望以两阶段方式处理该事务，当且仅当输出插件支持两阶段提交，并且
				 * 在 prepare 时没有过滤掉该事务。
				 */
				if (FilterPrepare(ctx, parsed.twophase_xid,
								  parsed.twophase_gid))
				{
					ReorderBufferProcessXid(reorder, parsed.twophase_xid,
											buf->origptr);
					break;
				}

				/*
				 * Note that if the prepared transaction has locked [user]
				 * catalog tables exclusively then decoding prepare can block
				 * till the main transaction is committed because it needs to
				 * lock the catalog tables.
				 *
				 * 注意：若预备事务以排他方式锁住了用户目录表，解码 prepare 可能一直阻
				 * 塞到主事务提交，因为它需要锁住这些目录表。
				 *
				 * XXX Now, this can even lead to a deadlock if the prepare
				 * transaction is waiting to get it logically replicated for
				 * distributed 2PC. This can be avoided by disallowing
				 * preparing transactions that have locked [user] catalog
				 * tables exclusively but as of now, we ask users not to do
				 * such an operation.
				 *
				 * XXX：如果 prepare 事务正在等待分布式两阶段提交把它逻辑复制出去，这甚
				 * 至可能导致死锁。可以通过禁止那些排他锁住用户目录表的事务做 prepare
				 * 来避免，但目前我们只是要求用户不要做这种操作。
				 */
				DecodePrepare(ctx, buf, &parsed);
				break;
			}
		default:
			elog(ERROR, "unexpected RM_XACT_ID record type: %u", info);
	}
}

/*
 * Handle rmgr STANDBY_ID records for LogicalDecodingProcessRecord().
 *
 * 为 LogicalDecodingProcessRecord() 处理资源管理器 STANDBY_ID 的记录。
 */
void
standby_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogReaderState *r = buf->record;
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(r), buf->origptr);

	switch (info)
	{
		case XLOG_RUNNING_XACTS:
			{
				xl_running_xacts *running = (xl_running_xacts *) XLogRecGetData(r);

				SnapBuildProcessRunningXacts(builder, buf->origptr, running);

				/*
				 * Abort all transactions that we keep track of, that are
				 * older than the record's oldestRunningXid. This is the most
				 * convenient spot for doing so since, in contrast to shutdown
				 * or end-of-recovery checkpoints, we have information about
				 * all running transactions which includes prepared ones,
				 * while shutdown checkpoints just know that no non-prepared
				 * transactions are in progress.
				 *
				 * 中止我们正在跟踪的、比该记录的 oldestRunningXid 更老的所有事务。这里
				 * 是最方便的位置：与关闭检查点或恢复结束检查点不同，我们掌握包括预备事
				 * 务在内的全部运行中事务的信息，而关闭检查点只知道没有未预备的事务在进行。
				 */
				ReorderBufferAbortOld(ctx->reorder, running->oldestRunningXid);
			}
			break;
		case XLOG_STANDBY_LOCK:
			break;
		case XLOG_INVALIDATIONS:

			/*
			 * We are processing the invalidations at the command level via
			 * XLOG_XACT_INVALIDATIONS.  So we don't need to do anything here.
			 *
			 * 失效是在命令级别通过 XLOG_XACT_INVALIDATIONS 处理的。因此这里不必做
			 * 任何事。
			 */
			break;
		default:
			elog(ERROR, "unexpected RM_STANDBY_ID record type: %u", info);
	}
}

/*
 * Handle rmgr HEAP2_ID records for LogicalDecodingProcessRecord().
 *
 * 为 LogicalDecodingProcessRecord() 处理资源管理器 HEAP2_ID 的记录。
 */
void
heap2_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding data changes. However, it's crucial to build the base
	 * snapshot during fast-forward mode (as is done in
	 * SnapBuildProcessChange()) because we require the snapshot's xmin when
	 * determining the candidate catalog_xmin for the replication slot. See
	 * SnapBuildProcessRunningXacts().
	 *
	 * 若还没有快照，或者只是在快进，就没有必要解码数据变更。不过在快进模式
	 * 下建立基础快照仍然至关重要（SnapBuildProcessChange() 会做这件事），
	 * 因为在确定复制槽的候选 catalog_xmin 时需要快照的 xmin。见
	 * SnapBuildProcessRunningXacts()。
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_HEAP2_MULTI_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeMultiInsert(ctx, buf);
			break;
		case XLOG_HEAP2_NEW_CID:
			if (!ctx->fast_forward)
			{
				xl_heap_new_cid *xlrec;

				xlrec = (xl_heap_new_cid *) XLogRecGetData(buf->record);
				SnapBuildProcessNewCid(builder, xid, buf->origptr, xlrec);

				break;
			}
		case XLOG_HEAP2_REWRITE:

			/*
			 * Although these records only exist to serve the needs of logical
			 * decoding, all the work happens as part of crash or archive
			 * recovery, so we don't need to do anything here.
			 *
			 * 这些记录虽然只为逻辑解码的需要而存在，但所有工作都在崩溃恢复或归档恢
			 * 复中完成，因此这里不必做任何事。
			 */
			break;

			/*
			 * Everything else here is just low level physical stuff we're not
			 * interested in.
			 *
			 * 这里其余的都是我们不关心的底层物理操作。
			 */
		case XLOG_HEAP2_PRUNE_ON_ACCESS:
		case XLOG_HEAP2_PRUNE_VACUUM_SCAN:
		case XLOG_HEAP2_PRUNE_VACUUM_CLEANUP:
		case XLOG_HEAP2_VISIBLE:
		case XLOG_HEAP2_LOCK_UPDATED:
			break;
		default:
			elog(ERROR, "unexpected RM_HEAP2_ID record type: %u", info);
	}
}

/*
 * Handle rmgr HEAP_ID records for LogicalDecodingProcessRecord().
 *
 * 为 LogicalDecodingProcessRecord() 处理资源管理器 HEAP_ID 的记录。
 */
void
heap_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	uint8		info = XLogRecGetInfo(buf->record) & XLOG_HEAP_OPMASK;
	TransactionId xid = XLogRecGetXid(buf->record);
	SnapBuild  *builder = ctx->snapshot_builder;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);

	/*
	 * If we don't have snapshot or we are just fast-forwarding, there is no
	 * point in decoding data changes. However, it's crucial to build the base
	 * snapshot during fast-forward mode (as is done in
	 * SnapBuildProcessChange()) because we require the snapshot's xmin when
	 * determining the candidate catalog_xmin for the replication slot. See
	 * SnapBuildProcessRunningXacts().
	 *
	 * 若还没有快照，或者只是在快进，就没有必要解码数据变更。不过在快进模式
	 * 下建立基础快照仍然至关重要（SnapBuildProcessChange() 会做这件事），
	 * 因为在确定复制槽的候选 catalog_xmin 时需要快照的 xmin。见
	 * SnapBuildProcessRunningXacts()。
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	switch (info)
	{
		case XLOG_HEAP_INSERT:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeInsert(ctx, buf);
			break;

			/*
			 * Treat HOT update as normal updates. There is no useful
			 * information in the fact that we could make it a HOT update
			 * locally and the WAL layout is compatible.
			 *
			 * 把 HOT 更新当作普通更新。本地能做成 HOT 更新这一事实没有有用信息，而
			 * 且 WAL 布局是兼容的。
			 */
		case XLOG_HEAP_HOT_UPDATE:
		case XLOG_HEAP_UPDATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeUpdate(ctx, buf);
			break;

		case XLOG_HEAP_DELETE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeDelete(ctx, buf);
			break;

		case XLOG_HEAP_TRUNCATE:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeTruncate(ctx, buf);
			break;

		case XLOG_HEAP_INPLACE:

			/*
			 * Inplace updates are only ever performed on catalog tuples and
			 * can, per definition, not change tuple visibility.  Since we
			 * also don't decode catalog tuples, we're not interested in the
			 * record's contents.
			 *
			 * 原地更新只会对目录元组进行，并且按定义不会改变元组可见性。既然我们也
			 * 不解码目录元组，就不关心这条记录的内容。
			 */
			break;

		case XLOG_HEAP_CONFIRM:
			if (SnapBuildProcessChange(builder, xid, buf->origptr) &&
				!ctx->fast_forward)
				DecodeSpecConfirm(ctx, buf);
			break;

		case XLOG_HEAP_LOCK:
			/* we don't care about row level locks for now
			 *
			 * 目前不关心行级锁
			 */
			break;

		default:
			elog(ERROR, "unexpected RM_HEAP_ID record type: %u", info);
			break;
	}
}

/*
 * Ask output plugin whether we want to skip this PREPARE and send
 * this transaction as a regular commit later.
 *
 * 询问输出插件是否要跳过这次 PREPARE，以后再把该事务当作普通提交发送。
 */
static inline bool
FilterPrepare(LogicalDecodingContext *ctx, TransactionId xid,
			  const char *gid)
{
	/*
	 * Skip if decoding of two-phase transactions at PREPARE time is not
	 * enabled. In that case, all two-phase transactions are considered
	 * filtered out and will be applied as regular transactions at COMMIT
	 * PREPARED.
	 *
	 * 若没有启用在 PREPARE 时解码两阶段事务，就跳过。这种情况下，所有两阶
	 * 段事务都被视为已过滤，将在 COMMIT PREPARED 时作为普通事务应用。
	 */
	if (!ctx->twophase)
		return true;

	/*
	 * The filter_prepare callback is optional. When not supplied, all
	 * prepared transactions should go through.
	 *
	 * filter_prepare 回调是可选的。未提供时，所有预备事务都应通过。
	 */
	if (ctx->callbacks.filter_prepare_cb == NULL)
		return false;

	return filter_prepare_cb_wrapper(ctx, xid, gid);
}

/*
 * 询问输出插件是否按复制源过滤该变更。未注册回调时不过滤。
 */
static inline bool
FilterByOrigin(LogicalDecodingContext *ctx, RepOriginId origin_id)
{
	if (ctx->callbacks.filter_by_origin_cb == NULL)
		return false;

	return filter_by_origin_cb_wrapper(ctx, origin_id);
}

/*
 * Handle rmgr LOGICALMSG_ID records for LogicalDecodingProcessRecord().
 *
 * 为 LogicalDecodingProcessRecord() 处理资源管理器 LOGICALMSG_ID 的记录。
 */
void
logicalmsg_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogReaderState *r = buf->record;
	TransactionId xid = XLogRecGetXid(r);
	uint8		info = XLogRecGetInfo(r) & ~XLR_INFO_MASK;
	RepOriginId origin_id = XLogRecGetOrigin(r);
	Snapshot	snapshot = NULL;
	xl_logical_message *message;

	if (info != XLOG_LOGICAL_MESSAGE)
		elog(ERROR, "unexpected RM_LOGICALMSG_ID record type: %u", info);

	ReorderBufferProcessXid(ctx->reorder, XLogRecGetXid(r), buf->origptr);

	/* If we don't have snapshot, there is no point in decoding messages
	 *
	 * 若还没有快照，就没有必要解码消息
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT)
		return;

	message = (xl_logical_message *) XLogRecGetData(r);

	if (message->dbId != ctx->slot->data.database ||
		FilterByOrigin(ctx, origin_id))
		return;

	if (message->transactional &&
		!SnapBuildProcessChange(builder, xid, buf->origptr))
		return;
	else if (!message->transactional &&
			 (SnapBuildCurrentState(builder) != SNAPBUILD_CONSISTENT ||
			  SnapBuildXactNeedsSkip(builder, buf->origptr)))
		return;

	/*
	 * We also skip decoding in fast_forward mode. This check must be last
	 * because we don't want to set the processing_required flag unless we
	 * have a decodable message.
	 *
	 * 在 fast_forward 模式下也跳过解码。这个检查必须放在最后，因为除非消息
	 * 可以解码，否则不想设置 processing_required 标志。
	 */
	if (ctx->fast_forward)
	{
		/*
		 * We need to set processing_required flag to notify the message's
		 * existence to the caller. Usually, the flag is set when either the
		 * COMMIT or ABORT records are decoded, but this must be turned on
		 * here because the non-transactional logical message is decoded
		 * without waiting for these records.
		 *
		 * 需要设置 processing_required 标志，以便把这条消息的存在通知调用方。
		 * 通常该标志在解码 COMMIT 或 ABORT 记录时设置，但这里必须打开，因为非
		 * 事务性逻辑消息不等这些记录就会被解码。
		 */
		if (!message->transactional)
			ctx->processing_required = true;

		return;
	}

	/*
	 * If this is a non-transactional change, get the snapshot we're expected
	 * to use. We only get here when the snapshot is consistent, and the
	 * change is not meant to be skipped.
	 *
	 * 若这是非事务性变更，取得我们应当使用的快照。只有快照已经一致、且该变
	 * 更不应被跳过时才会走到这里。
	 *
	 * For transactional changes we don't need a snapshot, we'll use the
	 * regular snapshot maintained by ReorderBuffer. We just leave it NULL.
	 *
	 * 事务性变更不需要快照，将使用 ReorderBuffer 维护的常规快照。这里就把
	 * 它留为 NULL。
	 */
	if (!message->transactional)
		snapshot = SnapBuildGetOrBuildSnapshot(builder);

	ReorderBufferQueueMessage(ctx->reorder, xid, snapshot, buf->endptr,
							  message->transactional,
							  message->message, /* first part of message is
												 * prefix
												 *
												 * 消息的前一部分是 prefix
												 */
							  message->message_size,
							  message->message + message->prefix_size);
}

/*
 * Consolidated commit record handling between the different form of commit
 * records.
 *
 * 对不同形式的提交记录做统一处理。
 *
 * 'two_phase' indicates that caller wants to process the transaction in two
 * phases, first process prepare if not already done and then process
 * commit_prepared.
 *
 * two_phase 表示调用方希望分两阶段处理该事务：若尚未处理 prepare，先处
 * 理它，然后再处理 commit_prepared。
 */
static void
DecodeCommit(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			 xl_xact_parsed_commit *parsed, TransactionId xid,
			 bool two_phase)
{
	XLogRecPtr	origin_lsn = InvalidXLogRecPtr;
	TimestampTz commit_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	int			i;

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		origin_lsn = parsed->origin_lsn;
		commit_time = parsed->origin_timestamp;
	}

	SnapBuildCommitTxn(ctx->snapshot_builder, buf->origptr, xid,
					   parsed->nsubxacts, parsed->subxacts,
					   parsed->xinfo);

	/* ----
	 * Check whether we are interested in this specific transaction, and tell
	 * the reorderbuffer to forget the content of the (sub-)transactions
	 * if not.
	 *
	 * 检查我们是否关心这个特定事务；若不关心，就让 reorderbuffer 忘掉这些
	 * 子事务的内容。
	 *
	 * We can't just use ReorderBufferAbort() here, because we need to execute
	 * the transaction's invalidations.  This currently won't be needed if
	 * we're just skipping over the transaction because currently we only do
	 * so during startup, to get to the first transaction the client needs. As
	 * we have reset the catalog caches before starting to read WAL, and we
	 * haven't yet touched any catalogs, there can't be anything to invalidate.
	 * But if we're "forgetting" this commit because it happened in another
	 * database, the invalidations might be important, because they could be
	 * for shared catalogs and we might have loaded data into the relevant
	 * syscaches.
	 *
	 * 这里不能直接用 ReorderBufferAbort()，因为需要执行该事务的失效。如果
	 * 只是跳过该事务，目前还不需要这样做，因为目前只在启动期间这样做，以便
	 * 到达客户端需要的第一个事务。我们在开始读 WAL 之前已经重置了目录缓存，
	 * 而且尚未碰过任何目录，所以没有什么可失效的。但如果是因为该提交发生在
	 * 另一个数据库而忘掉它，失效可能很重要，因为它们可能针对共享目录，而我
	 * 们可能已经把数据装进了相关的系统缓存。
	 * ---
	 */
	if (DecodeTXNNeedSkip(ctx, buf, parsed->dbId, origin_id))
	{
		for (i = 0; i < parsed->nsubxacts; i++)
		{
			ReorderBufferForget(ctx->reorder, parsed->subxacts[i], buf->origptr);
		}
		ReorderBufferForget(ctx->reorder, xid, buf->origptr);

		return;
	}

	/* tell the reorderbuffer about the surviving subtransactions
	 *
	 * 把仍然存在的子事务告诉 reorderbuffer
	 */
	for (i = 0; i < parsed->nsubxacts; i++)
	{
		ReorderBufferCommitChild(ctx->reorder, xid, parsed->subxacts[i],
								 buf->origptr, buf->endptr);
	}

	/*
	 * Send the final commit record if the transaction data is already
	 * decoded, otherwise, process the entire transaction.
	 *
	 * 若事务数据已经解码，就发送最终的提交记录；否则处理整个事务。
	 */
	if (two_phase)
	{
		ReorderBufferFinishPrepared(ctx->reorder, xid, buf->origptr, buf->endptr,
									SnapBuildGetTwoPhaseAt(ctx->snapshot_builder),
									commit_time, origin_id, origin_lsn,
									parsed->twophase_gid, true);
	}
	else
	{
		ReorderBufferCommit(ctx->reorder, xid, buf->origptr, buf->endptr,
							commit_time, origin_id, origin_lsn);
	}

	/*
	 * Update the decoding stats at transaction prepare/commit/abort.
	 * Additionally we send the stats when we spill or stream the changes to
	 * avoid losing them in case the decoding is interrupted. It is not clear
	 * that sending more or less frequently than this would be better.
	 *
	 * 在事务 prepare、commit 或 abort 时更新解码统计。另外在把变更溢写或流
	 * 式发送时也发送统计，以免解码被打断时丢掉它们。目前还不清楚比这更频繁
	 * 或更少发送是否更好。
	 */
	UpdateDecodingStats(ctx);
}

/*
 * Decode PREPARE record. Similar logic as in DecodeCommit.
 *
 * 解码 PREPARE 记录。逻辑与 DecodeCommit 类似。
 *
 * Note that we don't skip prepare even if have detected concurrent abort
 * because it is quite possible that we had already sent some changes before we
 * detect abort in which case we need to abort those changes in the subscriber.
 * To abort such changes, we do send the prepare and then the rollback prepared
 * which is what happened on the publisher-side as well. Now, we can invent a
 * new abort API wherein in such cases we send abort and skip sending prepared
 * and rollback prepared but then it is not that straightforward because we
 * might have streamed this transaction by that time in which case it is
 * handled when the rollback is encountered. It is not impossible to optimize
 * the concurrent abort case but it can introduce design complexity w.r.t
 * handling different cases so leaving it for now as it doesn't seem worth it.
 *
 * 注意：即使已经发现并发中止，也不跳过 prepare。因为很可能在发现中止之
 * 前已经发出了一些变更，这时需要在订阅端中止那些变更。为了中止它们，我
 * 们会发送 prepare，然后再发送 rollback prepared，这与发布端发生的情况
 * 一致。也可以另做一套中止接口，在这种情况下发送 abort 并跳过 prepared
 * 和 rollback prepared，但并不那么直接，因为那时可能已经把该事务流式发
 * 送出去了，那种情况要等遇到 rollback 时再处理。优化并发中止并非不可能，
 * 但会在不同情形的处理上增加设计复杂度，目前看起来不值得，因此先这样。
 */
static void
DecodePrepare(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			  xl_xact_parsed_prepare *parsed)
{
	SnapBuild  *builder = ctx->snapshot_builder;
	XLogRecPtr	origin_lsn = parsed->origin_lsn;
	TimestampTz prepare_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	int			i;
	TransactionId xid = parsed->twophase_xid;

	if (parsed->origin_timestamp != 0)
		prepare_time = parsed->origin_timestamp;

	/*
	 * Remember the prepare info for a txn so that it can be used later in
	 * commit prepared if required. See ReorderBufferFinishPrepared.
	 *
	 * 记住事务的 prepare 信息，以便以后在 commit prepared 需要时使用。见
	 * ReorderBufferFinishPrepared。
	 */
	if (!ReorderBufferRememberPrepareInfo(ctx->reorder, xid, buf->origptr,
										  buf->endptr, prepare_time, origin_id,
										  origin_lsn))
		return;

	/* We can't start streaming unless a consistent state is reached.
	 *
	 * 在达到一致状态之前不能开始流式发送。
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_CONSISTENT)
	{
		ReorderBufferSkipPrepare(ctx->reorder, xid);
		return;
	}

	/*
	 * Check whether we need to process this transaction. See
	 * DecodeTXNNeedSkip for the reasons why we sometimes want to skip the
	 * transaction.
	 *
	 * 检查是否需要处理该事务。有时要跳过事务的原因见 DecodeTXNNeedSkip。
	 *
	 * We can't call ReorderBufferForget as we did in DecodeCommit as the txn
	 * hasn't yet been committed, removing this txn before a commit might
	 * result in the computation of an incorrect restart_lsn. See
	 * SnapBuildProcessRunningXacts. But we need to process cache
	 * invalidations if there are any for the reasons mentioned in
	 * DecodeCommit.
	 *
	 * 不能像 DecodeCommit 那样调用 ReorderBufferForget，因为该事务尚未提交。
	 * 在提交前删掉它可能导致算出错误的 restart_lsn。见
	 * SnapBuildProcessRunningXacts。但如果有缓存失效，仍需要处理，原因见
	 * DecodeCommit。
	 */
	if (DecodeTXNNeedSkip(ctx, buf, parsed->dbId, origin_id))
	{
		ReorderBufferSkipPrepare(ctx->reorder, xid);
		ReorderBufferInvalidate(ctx->reorder, xid, buf->origptr);
		return;
	}

	/* Tell the reorderbuffer about the surviving subtransactions.
	 *
	 * 把仍然存在的子事务告诉 reorderbuffer。
	 */
	for (i = 0; i < parsed->nsubxacts; i++)
	{
		ReorderBufferCommitChild(ctx->reorder, xid, parsed->subxacts[i],
								 buf->origptr, buf->endptr);
	}

	/* replay actions of all transaction + subtransactions in order
	 *
	 * 按顺序重放该事务及所有子事务的动作
	 */
	ReorderBufferPrepare(ctx->reorder, xid, parsed->twophase_gid);

	/*
	 * Update the decoding stats at transaction prepare/commit/abort.
	 * Additionally we send the stats when we spill or stream the changes to
	 * avoid losing them in case the decoding is interrupted. It is not clear
	 * that sending more or less frequently than this would be better.
	 *
	 * 在事务 prepare、commit 或 abort 时更新解码统计。另外在把变更溢写或流
	 * 式发送时也发送统计，以免解码被打断时丢掉它们。目前还不清楚比这更频繁
	 * 或更少发送是否更好。
	 */
	UpdateDecodingStats(ctx);
}


/*
 * Get the data from the various forms of abort records and pass it on to
 * snapbuild.c and reorderbuffer.c.
 *
 * 从各种形式的中止记录中取出数据，传给 snapbuild.c 和 reorderbuffer.c。
 *
 * 'two_phase' indicates to finish prepared transaction.
 *
 * two_phase 表示要结束预备事务。
 */
static void
DecodeAbort(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
			xl_xact_parsed_abort *parsed, TransactionId xid,
			bool two_phase)
{
	int			i;
	XLogRecPtr	origin_lsn = InvalidXLogRecPtr;
	TimestampTz abort_time = parsed->xact_time;
	RepOriginId origin_id = XLogRecGetOrigin(buf->record);
	bool		skip_xact;

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		origin_lsn = parsed->origin_lsn;
		abort_time = parsed->origin_timestamp;
	}

	/*
	 * Check whether we need to process this transaction. See
	 * DecodeTXNNeedSkip for the reasons why we sometimes want to skip the
	 * transaction.
	 *
	 * 检查是否需要处理该事务。有时要跳过事务的原因见 DecodeTXNNeedSkip。
	 */
	skip_xact = DecodeTXNNeedSkip(ctx, buf, parsed->dbId, origin_id);

	/*
	 * Send the final rollback record for a prepared transaction unless we
	 * need to skip it. For non-two-phase xacts, simply forget the xact.
	 *
	 * 除非需要跳过，否则为预备事务发送最终的回滚记录。对非两阶段事务，直接
	 * 忘掉该事务。
	 */
	if (two_phase && !skip_xact)
	{
		ReorderBufferFinishPrepared(ctx->reorder, xid, buf->origptr, buf->endptr,
									InvalidXLogRecPtr,
									abort_time, origin_id, origin_lsn,
									parsed->twophase_gid, false);
	}
	else
	{
		for (i = 0; i < parsed->nsubxacts; i++)
		{
			ReorderBufferAbort(ctx->reorder, parsed->subxacts[i],
							   buf->record->EndRecPtr, abort_time);
		}

		ReorderBufferAbort(ctx->reorder, xid, buf->record->EndRecPtr,
						   abort_time);
	}

	/* update the decoding stats
	 *
	 * 更新解码统计
	 */
	UpdateDecodingStats(ctx);
}

/*
 * Parse XLOG_HEAP_INSERT (not MULTI_INSERT!) records into tuplebufs.
 *
 * 把 XLOG_HEAP_INSERT（不是 MULTI_INSERT）记录解析进 tuplebuf。
 *
 * Inserts can contain the new tuple.
 *
 * 插入记录里可以带有新元组。
 */
static void
DecodeInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	Size		datalen;
	char	   *tupledata;
	Size		tuplelen;
	XLogReaderState *r = buf->record;
	xl_heap_insert *xlrec;
	ReorderBufferChange *change;
	RelFileLocator target_locator;

	xlrec = (xl_heap_insert *) XLogRecGetData(r);

	/*
	 * Ignore insert records without new tuples (this does happen when
	 * raw_heap_insert marks the TOAST record as HEAP_INSERT_NO_LOGICAL).
	 *
	 * 忽略没有新元组的插入记录（raw_heap_insert 把 TOAST 记录标为
	 * HEAP_INSERT_NO_LOGICAL 时就会这样）。
	 */
	if (!(xlrec->flags & XLH_INSERT_CONTAINS_NEW_TUPLE))
		return;

	/* only interested in our database
	 *
	 * 只关心我们自己的数据库
	 */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue
	 *
	 * 输出插件不关注这个 origin，不必入队
	 */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferAllocChange(ctx->reorder);
	if (!(xlrec->flags & XLH_INSERT_IS_SPECULATIVE))
		change->action = REORDER_BUFFER_CHANGE_INSERT;
	else
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	tupledata = XLogRecGetBlockData(r, 0, &datalen);
	tuplelen = datalen - SizeOfHeapHeader;

	change->data.tp.newtuple =
		ReorderBufferAllocTupleBuf(ctx->reorder, tuplelen);

	DecodeXLogTuple(tupledata, datalen, change->data.tp.newtuple);

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change,
							 xlrec->flags & XLH_INSERT_ON_TOAST_RELATION);
}

/*
 * Parse XLOG_HEAP_UPDATE and XLOG_HEAP_HOT_UPDATE, which have the same layout
 * in the record, from wal into proper tuplebufs.
 *
 * 把 XLOG_HEAP_UPDATE 和 XLOG_HEAP_HOT_UPDATE 从 WAL 解析成合适的
 * tuplebuf，二者在记录中的布局相同。
 *
 * Updates can possibly contain a new tuple and the old primary key.
 *
 * 更新记录里可能带有新元组和旧主键。
 */
static void
DecodeUpdate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_update *xlrec;
	ReorderBufferChange *change;
	char	   *data;
	RelFileLocator target_locator;

	xlrec = (xl_heap_update *) XLogRecGetData(r);

	/* only interested in our database
	 *
	 * 只关心我们自己的数据库
	 */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue
	 *
	 * 输出插件不关注这个 origin，不必入队
	 */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferAllocChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_UPDATE;
	change->origin_id = XLogRecGetOrigin(r);
	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	if (xlrec->flags & XLH_UPDATE_CONTAINS_NEW_TUPLE)
	{
		Size		datalen;
		Size		tuplelen;

		data = XLogRecGetBlockData(r, 0, &datalen);

		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.newtuple =
			ReorderBufferAllocTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple(data, datalen, change->data.tp.newtuple);
	}

	if (xlrec->flags & XLH_UPDATE_CONTAINS_OLD)
	{
		Size		datalen;
		Size		tuplelen;

		/* caution, remaining data in record is not aligned
		 *
		 * 注意：记录中剩余的数据没有对齐
		 */
		data = XLogRecGetData(r) + SizeOfHeapUpdate;
		datalen = XLogRecGetDataLen(r) - SizeOfHeapUpdate;
		tuplelen = datalen - SizeOfHeapHeader;

		change->data.tp.oldtuple =
			ReorderBufferAllocTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple(data, datalen, change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change, false);
}

/*
 * Parse XLOG_HEAP_DELETE from wal into proper tuplebufs.
 *
 * 把 XLOG_HEAP_DELETE 从 WAL 解析成合适的 tuplebuf。
 *
 * Deletes can possibly contain the old primary key.
 *
 * 删除记录里可能带有旧主键。
 */
static void
DecodeDelete(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_delete *xlrec;
	ReorderBufferChange *change;
	RelFileLocator target_locator;

	xlrec = (xl_heap_delete *) XLogRecGetData(r);

	/* only interested in our database
	 *
	 * 只关心我们自己的数据库
	 */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue
	 *
	 * 输出插件不关注这个 origin，不必入队
	 */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferAllocChange(ctx->reorder);

	if (xlrec->flags & XLH_DELETE_IS_SUPER)
		change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT;
	else
		change->action = REORDER_BUFFER_CHANGE_DELETE;

	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	/* old primary key stored
	 *
	 * 存有旧主键
	 */
	if (xlrec->flags & XLH_DELETE_CONTAINS_OLD)
	{
		Size		datalen = XLogRecGetDataLen(r) - SizeOfHeapDelete;
		Size		tuplelen = datalen - SizeOfHeapHeader;

		Assert(XLogRecGetDataLen(r) > (SizeOfHeapDelete + SizeOfHeapHeader));

		change->data.tp.oldtuple =
			ReorderBufferAllocTupleBuf(ctx->reorder, tuplelen);

		DecodeXLogTuple((char *) xlrec + SizeOfHeapDelete,
						datalen, change->data.tp.oldtuple);
	}

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change, false);
}

/*
 * Parse XLOG_HEAP_TRUNCATE from wal
 *
 * 从 WAL 解析 XLOG_HEAP_TRUNCATE
 */
static void
DecodeTruncate(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_truncate *xlrec;
	ReorderBufferChange *change;

	xlrec = (xl_heap_truncate *) XLogRecGetData(r);

	/* only interested in our database
	 *
	 * 只关心我们自己的数据库
	 */
	if (xlrec->dbId != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue
	 *
	 * 输出插件不关注这个 origin，不必入队
	 */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferAllocChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_TRUNCATE;
	change->origin_id = XLogRecGetOrigin(r);
	if (xlrec->flags & XLH_TRUNCATE_CASCADE)
		change->data.truncate.cascade = true;
	if (xlrec->flags & XLH_TRUNCATE_RESTART_SEQS)
		change->data.truncate.restart_seqs = true;
	change->data.truncate.nrelids = xlrec->nrelids;
	change->data.truncate.relids = ReorderBufferAllocRelids(ctx->reorder,
															xlrec->nrelids);
	memcpy(change->data.truncate.relids, xlrec->relids,
		   xlrec->nrelids * sizeof(Oid));
	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
							 buf->origptr, change, false);
}

/*
 * Decode XLOG_HEAP2_MULTI_INSERT record into multiple tuplebufs.
 *
 * 把 XLOG_HEAP2_MULTI_INSERT 记录解码成多个 tuplebuf。
 *
 * Currently MULTI_INSERT will always contain the full tuples.
 *
 * 目前 MULTI_INSERT 总会包含完整元组。
 */
static void
DecodeMultiInsert(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	xl_heap_multi_insert *xlrec;
	int			i;
	char	   *data;
	char	   *tupledata;
	Size		tuplelen;
	RelFileLocator rlocator;

	xlrec = (xl_heap_multi_insert *) XLogRecGetData(r);

	/*
	 * Ignore insert records without new tuples.  This happens when a
	 * multi_insert is done on a catalog or on a non-persistent relation.
	 *
	 * 忽略没有新元组的插入记录。对目录或非持久关系做 multi_insert 时就会这样。
	 */
	if (!(xlrec->flags & XLH_INSERT_CONTAINS_NEW_TUPLE))
		return;

	/* only interested in our database
	 *
	 * 只关心我们自己的数据库
	 */
	XLogRecGetBlockTag(r, 0, &rlocator, NULL, NULL);
	if (rlocator.dbOid != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue
	 *
	 * 输出插件不关注这个 origin，不必入队
	 */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	/*
	 * We know that this multi_insert isn't for a catalog, so the block should
	 * always have data even if a full-page write of it is taken.
	 *
	 * 已知这次 multi_insert 不是针对目录的，因此即使对该块做了全页写，块里
	 * 也应该有数据。
	 */
	tupledata = XLogRecGetBlockData(r, 0, &tuplelen);
	Assert(tupledata != NULL);

	data = tupledata;
	for (i = 0; i < xlrec->ntuples; i++)
	{
		ReorderBufferChange *change;
		xl_multi_insert_tuple *xlhdr;
		int			datalen;
		HeapTuple	tuple;
		HeapTupleHeader header;

		change = ReorderBufferAllocChange(ctx->reorder);
		change->action = REORDER_BUFFER_CHANGE_INSERT;
		change->origin_id = XLogRecGetOrigin(r);

		memcpy(&change->data.tp.rlocator, &rlocator, sizeof(RelFileLocator));

		xlhdr = (xl_multi_insert_tuple *) SHORTALIGN(data);
		data = ((char *) xlhdr) + SizeOfMultiInsertTuple;
		datalen = xlhdr->datalen;

		change->data.tp.newtuple =
			ReorderBufferAllocTupleBuf(ctx->reorder, datalen);

		tuple = change->data.tp.newtuple;
		header = tuple->t_data;

		/* not a disk based tuple
		 *
		 * 不是基于磁盘的元组
		 */
		ItemPointerSetInvalid(&tuple->t_self);

		/*
		 * We can only figure this out after reassembling the transactions.
		 *
		 * 只有在重新组装事务之后才能判断这一点。
		 */
		tuple->t_tableOid = InvalidOid;

		tuple->t_len = datalen + SizeofHeapTupleHeader;

		memset(header, 0, SizeofHeapTupleHeader);

		memcpy((char *) tuple->t_data + SizeofHeapTupleHeader, data, datalen);
		header->t_infomask = xlhdr->t_infomask;
		header->t_infomask2 = xlhdr->t_infomask2;
		header->t_hoff = xlhdr->t_hoff;

		/*
		 * Reset toast reassembly state only after the last row in the last
		 * xl_multi_insert_tuple record emitted by one heap_multi_insert()
		 * call.
		 *
		 * 只有在一次 heap_multi_insert() 调用发出的最后一条
		 * xl_multi_insert_tuple 记录中的最后一行之后，才重置 TOAST 重组状态。
		 */
		if (xlrec->flags & XLH_INSERT_LAST_IN_MULTI &&
			(i + 1) == xlrec->ntuples)
			change->data.tp.clear_toast_afterwards = true;
		else
			change->data.tp.clear_toast_afterwards = false;

		ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r),
								 buf->origptr, change, false);

		/* move to the next xl_multi_insert_tuple entry
		 *
		 * 移到下一条 xl_multi_insert_tuple 项
		 */
		data += datalen;
	}
	Assert(data == tupledata + tuplelen);
}

/*
 * Parse XLOG_HEAP_CONFIRM from wal into a confirmation change.
 *
 * 把 XLOG_HEAP_CONFIRM 从 WAL 解析成一条确认变更。
 *
 * This is pretty trivial, all the state essentially already setup by the
 * speculative insertion.
 *
 * 这很简单，状态本质上已经由推测性插入准备好了。
 */
static void
DecodeSpecConfirm(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	ReorderBufferChange *change;
	RelFileLocator target_locator;

	/* only interested in our database
	 *
	 * 只关心我们自己的数据库
	 */
	XLogRecGetBlockTag(r, 0, &target_locator, NULL, NULL);
	if (target_locator.dbOid != ctx->slot->data.database)
		return;

	/* output plugin doesn't look for this origin, no need to queue
	 *
	 * 输出插件不关注这个 origin，不必入队
	 */
	if (FilterByOrigin(ctx, XLogRecGetOrigin(r)))
		return;

	change = ReorderBufferAllocChange(ctx->reorder);
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM;
	change->origin_id = XLogRecGetOrigin(r);

	memcpy(&change->data.tp.rlocator, &target_locator, sizeof(RelFileLocator));

	change->data.tp.clear_toast_afterwards = true;

	ReorderBufferQueueChange(ctx->reorder, XLogRecGetXid(r), buf->origptr,
							 change, false);
}


/*
 * Read a HeapTuple as WAL logged by heap_insert, heap_update and heap_delete
 * (but not by heap_multi_insert) into a tuplebuf.
 *
 * 把 heap_insert、heap_update 和 heap_delete（但不包括
 * heap_multi_insert）记入 WAL 的 HeapTuple 读进 tuplebuf。
 *
 * The size 'len' and the pointer 'data' in the record need to be
 * computed outside as they are record specific.
 *
 * 记录中的长度 len 和指针 data 需要在外面算好，因为它们随记录类型而不同。
 */
static void
DecodeXLogTuple(char *data, Size len, HeapTuple tuple)
{
	xl_heap_header xlhdr;
	int			datalen = len - SizeOfHeapHeader;
	HeapTupleHeader header;

	Assert(datalen >= 0);

	tuple->t_len = datalen + SizeofHeapTupleHeader;
	header = tuple->t_data;

	/* not a disk based tuple
	 *
	 * 不是基于磁盘的元组
	 */
	ItemPointerSetInvalid(&tuple->t_self);

	/* we can only figure this out after reassembling the transactions
	 *
	 * 只有在重新组装事务之后才能判断这一点
	 */
	tuple->t_tableOid = InvalidOid;

	/* data is not stored aligned, copy to aligned storage
	 *
	 * 数据存放时没有对齐，复制到对齐的存储中
	 */
	memcpy(&xlhdr, data, SizeOfHeapHeader);

	memset(header, 0, SizeofHeapTupleHeader);

	memcpy(((char *) tuple->t_data) + SizeofHeapTupleHeader,
		   data + SizeOfHeapHeader,
		   datalen);

	header->t_infomask = xlhdr.t_infomask;
	header->t_infomask2 = xlhdr.t_infomask2;
	header->t_hoff = xlhdr.t_hoff;
}

/*
 * Check whether we are interested in this specific transaction.
 *
 * 检查我们是否关心这个特定事务。
 *
 * There can be several reasons we might not be interested in this
 * transaction:
 * 1) We might not be interested in decoding transactions up to this
 *	  LSN. This can happen because we previously decoded it and now just
 *	  are restarting or if we haven't assembled a consistent snapshot yet.
 * 2) The transaction happened in another database.
 * 3) The output plugin is not interested in the origin.
 * 4) We are doing fast-forwarding
 *
 * 可能有几种原因使我们不关心该事务：1) 我们可能不想解码直到这个 LSN 的
 * 事务。这可能是因为先前已经解码过、现在只是重新开始，或者还没有组装出
 * 一致快照。2) 该事务发生在另一个数据库。3) 输出插件对该 origin 不感兴
 * 趣。4) 我们正在快进。
 */
static bool
DecodeTXNNeedSkip(LogicalDecodingContext *ctx, XLogRecordBuffer *buf,
				  Oid txn_dbid, RepOriginId origin_id)
{
	if (SnapBuildXactNeedsSkip(ctx->snapshot_builder, buf->origptr) ||
		(txn_dbid != InvalidOid && txn_dbid != ctx->slot->data.database) ||
		FilterByOrigin(ctx, origin_id))
		return true;

	/*
	 * We also skip decoding in fast_forward mode. In passing set the
	 * processing_required flag to indicate that if it were not for
	 * fast_forward mode, processing would have been required.
	 *
	 * 在 fast_forward 模式下也跳过解码。顺便设置 processing_required 标志，
	 * 表示若不是 fast_forward 模式，本来是需要处理的。
	 */
	if (ctx->fast_forward)
	{
		ctx->processing_required = true;
		return true;
	}

	return false;
}
