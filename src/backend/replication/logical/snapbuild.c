/*-------------------------------------------------------------------------
 *
 * snapbuild.c
 *
 * 源文件 snapbuild.c。
 *
 *	  Infrastructure for building historic catalog snapshots based on contents
 *	  of the WAL, for the purpose of decoding heapam.c style values in the
 *	  WAL.
 *
 *	  基于 WAL 内容构建历史目录快照的基础设施，用来解码 WAL 中 heapam.c 风格的值。
 *
 * NOTES:
 *
 * 说明：
 *
 * We build snapshots which can *only* be used to read catalog contents and we
 * do so by reading and interpreting the WAL stream. The aim is to build a
 * snapshot that behaves the same as a freshly taken MVCC snapshot would have
 * at the time the XLogRecord was generated.
 *
 * 我们构建的快照只能用来读取目录内容，做法是读取并解释 WAL 流。目标是让这份快照的行为，与生成该 XLogRecord 当时新取的一份 MVCC 快照相同。
 *
 * To build the snapshots we reuse the infrastructure built for Hot
 * Standby. The in-memory snapshots we build look different than HS' because
 * we have different needs. To successfully decode data from the WAL we only
 * need to access catalog tables and (sys|rel|cat)cache, not the actual user
 * tables since the data we decode is wholly contained in the WAL
 * records. Also, our snapshots need to be different in comparison to normal
 * MVCC ones because in contrast to those we cannot fully rely on the clog and
 * pg_subtrans for information about committed transactions because they might
 * commit in the future from the POV of the WAL entry we're currently
 * decoding. This definition has the advantage that we only need to prevent
 * removal of catalog rows, while normal table's rows can still be
 * removed. This is achieved by using the replication slot mechanism.
 *
 * 构建快照时复用为 Hot Standby 准备的基础设施。内存中的快照和 HS 的不一样，因为需求不同。要从 WAL 成功解码，只需要访问目录表以及 syscache、relcache 和 catcache，不需要访问真正的用户表，因为要解码的数据全部包含在 WAL 记录里。我们的快照也必须和普通 MVCC 快照不同：不能完全依赖 clog 和 pg_subtrans 来判断事务是否已提交，因为从当前正在解码的 WAL 条目来看，这些事务可能在将来才提交。这样定义的好处是，只需要阻止目录行被删除，普通表的行仍然可以删除。这是通过复制槽机制实现的。
 *
 * As the percentage of transactions modifying the catalog normally is fairly
 * small in comparisons to ones only manipulating user data, we keep track of
 * the committed catalog modifying ones inside [xmin, xmax) instead of keeping
 * track of all running transactions like it's done in a normal snapshot. Note
 * that we're generally only looking at transactions that have acquired an
 * xid. That is we keep a list of transactions between snapshot->(xmin, xmax)
 * that we consider committed, everything else is considered aborted/in
 * progress. That also allows us not to care about subtransactions before they
 * have committed which means this module, in contrast to HS, doesn't have to
 * care about suboverflowed subtransactions and similar.
 *
 * 修改目录的事务比例通常远小于只改用户数据的事务，因此我们只在 xmin 到 xmax 这个区间内跟踪已提交且修改了目录的事务，而不是像普通快照那样跟踪所有正在运行的事务。一般来说只关心已经获得 xid 的事务。也就是在 snapshot 的 xmin 与 xmax 之间保存一份我们认为已提交的事务列表，其余都视为已中止或仍在进行。这样在子事务提交之前不必关心它们，因此本模块不必像 HS 那样处理溢出的子事务等情况。
 *
 * One complexity of doing this is that to e.g. handle mixed DDL/DML
 * transactions we need Snapshots that see intermediate versions of the
 * catalog in a transaction. During normal operation this is achieved by using
 * CommandIds/cmin/cmax. The problem with that however is that for space
 * efficiency reasons, the cmin and cmax are not included in WAL records. We
 * cannot read the cmin/cmax from the tuple itself, either, because it is
 * reset on crash recovery. Even if we could, we could not decode combocids
 * which are only tracked in the original backend's memory. To work around
 * that, heapam writes an extra WAL record (XLOG_HEAP2_NEW_CID) every time a
 * catalog row is modified, which includes the cmin and cmax of the
 * tuple. During decoding, we insert the ctid->(cmin,cmax) mappings into the
 * reorder buffer, and use them at visibility checks instead of the cmin/cmax
 * on the tuple itself. Check the reorderbuffer.c's comment above
 * ResolveCminCmaxDuringDecoding() for details.
 *
 * 这样做的一个复杂点是：要处理 DDL 与 DML 混合的事务，就需要能看到事务中目录中间版本的 Snapshot。正常运行时靠 CommandId、cmin、cmax 实现。但为了节省空间，WAL 记录里不包含 cmin 和 cmax。也不能从元组本身读取 cmin 和 cmax，因为崩溃恢复时它们会被重置。即便能读到，也无法解码 combocid，它们只记录在原来那个后端的内存里。变通办法是：heapam 每次修改目录行都额外写一条 WAL 记录 XLOG_HEAP2_NEW_CID，其中包含该元组的 cmin 和 cmax。解码时把 ctid 到 cmin、cmax 的映射放进 reorder buffer，可见性检查时用它们，而不用元组上的 cmin 和 cmax。详情见 reorderbuffer.c 里 ResolveCminCmaxDuringDecoding() 上方的注释。
 *
 * To facilitate all this we need our own visibility routine, as the normal
 * ones are optimized for different usecases.
 *
 * 为此需要自己的可见性例程，因为普通例程是为别的用途优化的。
 *
 * To replace the normal catalog snapshots with decoding ones use the
 * SetupHistoricSnapshot() and TeardownHistoricSnapshot() functions.
 *
 * 要用解码快照替换普通目录快照时，调用 SetupHistoricSnapshot() 和 TeardownHistoricSnapshot()。
 *
 *
 * The snapbuild machinery is starting up in several stages, as illustrated
 * by the following graph describing the SnapBuild->state transitions:
 *
 * 快照构建器分若干阶段启动。下面的图描述 SnapBuild 的 state 迁移。
 *
 *		   +-------------------------+
 *	  +----|		 START			 |-------------+
 *	  |    +-------------------------+			   |
 *	  |					|						   |
 *	  |					|						   |
 *	  |		   running_xacts #1					   |
 *	  |					|						   |
 *	  |					|						   |
 *	  |					v						   |
 *	  |    +-------------------------+			   v
 *	  |    |   BUILDING_SNAPSHOT	 |------------>|
 *	  |    +-------------------------+			   |
 *	  |					|						   |
 *	  |					|						   |
 *	  | running_xacts #2, xacts from #1 finished   |
 *	  |					|						   |
 *	  |					|						   |
 *	  |					v						   |
 *	  |    +-------------------------+			   v
 *	  |    |	   FULL_SNAPSHOT	 |------------>|
 *	  |    +-------------------------+			   |
 *	  |					|						   |
 * running_xacts		|					   saved snapshot
 * with zero xacts		|				  at running_xacts's lsn
 *	  |					|						   |
 *	  | running_xacts with xacts from #2 finished  |
 *	  |					|						   |
 *	  |					v						   |
 *	  |    +-------------------------+			   |
 *	  +--->|SNAPBUILD_CONSISTENT	 |<------------+
 *		   +-------------------------+
 *
 * Initially the machinery is in the START stage. When an xl_running_xacts
 * record is read that is sufficiently new (above the safe xmin horizon),
 * there's a state transition. If there were no running xacts when the
 * xl_running_xacts record was generated, we'll directly go into CONSISTENT
 * state, otherwise we'll switch to the BUILDING_SNAPSHOT state. Having a full
 * snapshot means that all transactions that start henceforth can be decoded
 * in their entirety, but transactions that started previously can't. In
 * FULL_SNAPSHOT we'll switch into CONSISTENT once all those previously
 * running transactions have committed or aborted.
 *
 * 机器最初处于 START 阶段。读到一条足够新（高于安全 xmin 视界）的 xl_running_xacts 记录时发生状态迁移。若生成该记录时没有正在运行的事务，就直接进入 CONSISTENT；否则进入 BUILDING_SNAPSHOT。拥有完整快照意味着此后开始的事务可以完整解码，但此前已经开始的事务不行。在 FULL_SNAPSHOT 中，等那些先前正在运行的事务全部提交或中止后，才转入 CONSISTENT。
 *
 * Only transactions that commit after CONSISTENT state has been reached will
 * be replayed, even though they might have started while still in
 * FULL_SNAPSHOT. That ensures that we'll reach a point where no previous
 * changes has been exported, but all the following ones will be. That point
 * is a convenient point to initialize replication from, which is why we
 * export a snapshot at that point, which *can* be used to read normal data.
 *
 * 只有在到达 CONSISTENT 之后提交的事务才会被重放，即使它们可能在仍处于 FULL_SNAPSHOT 时就已经开始。这样能到达一个点：在此之前的变更都不会被导出，此后的变更都会被导出。这个点适合作为复制的初始化位置，因此在这里导出一份快照，这份快照可以用来读取普通数据。
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/snapbuild.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "replication/snapbuild_internal.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/standby.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"

/*
 * 核心流程：
 * AllocateSnapshotBuilder 建立快照构建器，初始状态为 START。
 * 读到足够新的 xl_running_xacts 后，SnapBuildFindSnapshot 把状态推进到
 * BUILDING_SNAPSHOT、FULL_SNAPSHOT，直至 SNAPBUILD_CONSISTENT。
 * SnapBuildCommitTxn 记录修改了目录的已提交事务，并在一致点之后分发快照与缓存失效。
 * 目录变更产生的 XLOG_HEAP2_NEW_CID 由 SnapBuildProcessNewCid 记入 reorder buffer，供可见性判断使用。
 * 到达序列化点后 SnapBuildSerialize 把快照写入磁盘，重启时由 SnapBuildRestore 恢复。
 */

/*
 * Starting a transaction -- which we need to do while exporting a snapshot --
 * removes knowledge about the previously used resowner, so we save it here.
 *
 * 导出快照时需要开启事务，而这会丢掉先前使用的 resowner，所以把它保存在这里。
 */
static ResourceOwner SavedResourceOwnerDuringExport = NULL;
static bool ExportInProgress = false;

/* ->committed and ->catchange manipulation
 *
 * 操作 committed 与 catchange。
 */
static void SnapBuildPurgeOlderTxn(SnapBuild *builder);

/* snapshot building/manipulation/distribution functions
 *
 * 快照的构建、操作与分发函数。
 */
static Snapshot SnapBuildBuildSnapshot(SnapBuild *builder);

static void SnapBuildFreeSnapshot(Snapshot snap);

static void SnapBuildSnapIncRefcount(Snapshot snap);

static void SnapBuildDistributeSnapshotAndInval(SnapBuild *builder, XLogRecPtr lsn, TransactionId xid);

static inline bool SnapBuildXidHasCatalogChanges(SnapBuild *builder, TransactionId xid,
												 uint32 xinfo);

/* xlog reading helper functions for SnapBuildProcessRunningXacts
 *
 * 供 SnapBuildProcessRunningXacts 使用的 WAL 读取辅助函数。
 */
static bool SnapBuildFindSnapshot(SnapBuild *builder, XLogRecPtr lsn, xl_running_xacts *running);
static void SnapBuildWaitSnapshot(xl_running_xacts *running, TransactionId cutoff);

/* serialization functions
 *
 * 序列化函数。
 */
static void SnapBuildSerialize(SnapBuild *builder, XLogRecPtr lsn);
static bool SnapBuildRestore(SnapBuild *builder, XLogRecPtr lsn);
static void SnapBuildRestoreContents(int fd, void *dest, Size size, const char *path);

/*
 * Allocate a new snapshot builder.
 *
 * 分配一个新的快照构建器。
 *
 * xmin_horizon is the xid >= which we can be sure no catalog rows have been
 * removed, start_lsn is the LSN >= we want to replay commits.
 *
 * xmin_horizon 是可以确信没有目录行被删除的最小 xid；start_lsn 是希望从那里开始重放提交的 LSN。
 */
SnapBuild *
AllocateSnapshotBuilder(ReorderBuffer *reorder,
						TransactionId xmin_horizon,
						XLogRecPtr start_lsn,
						bool need_full_snapshot,
						bool in_slot_creation,
						XLogRecPtr two_phase_at)
{
	MemoryContext context;
	MemoryContext oldcontext;
	SnapBuild  *builder;

	/* allocate memory in own context, to have better accountability
	 *
	 * 在自己的内存上下文中分配，便于追查内存归属。
	 */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"snapshot builder context",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);

	builder = palloc0(sizeof(SnapBuild));

	builder->state = SNAPBUILD_START;
	builder->context = context;
	builder->reorder = reorder;
	/* Other struct members initialized by zeroing via palloc0 above
	 *
	 * 其余结构成员已由上面的 palloc0 清零完成初始化。
	 */

	builder->committed.xcnt = 0;
	builder->committed.xcnt_space = 128;	/* arbitrary number
	 *
	 * 一个任意选取的数值。
	 */
	builder->committed.xip =
		palloc0(builder->committed.xcnt_space * sizeof(TransactionId));
	builder->committed.includes_all_transactions = true;

	builder->catchange.xcnt = 0;
	builder->catchange.xip = NULL;

	builder->initial_xmin_horizon = xmin_horizon;
	builder->start_decoding_at = start_lsn;
	builder->in_slot_creation = in_slot_creation;
	builder->building_full_snapshot = need_full_snapshot;
	builder->two_phase_at = two_phase_at;

	MemoryContextSwitchTo(oldcontext);

	return builder;
}

/*
 * Free a snapshot builder.
 *
 * 释放快照构建器。
 */
void
FreeSnapshotBuilder(SnapBuild *builder)
{
	MemoryContext context = builder->context;

	/* free snapshot explicitly, that contains some error checking
	 *
	 * 显式释放快照，其中包含一些错误检查。
	 */
	if (builder->snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(builder->snapshot);
		builder->snapshot = NULL;
	}

	/* other resources are deallocated via memory context reset
	 *
	 * 其他资源通过重置内存上下文来释放。
	 */
	MemoryContextDelete(context);
}

/*
 * Free an unreferenced snapshot that has previously been built by us.
 *
 * 释放一份由我们构建、且已经没有引用的快照。
 */
static void
SnapBuildFreeSnapshot(Snapshot snap)
{
	/* make sure we don't get passed an external snapshot
	 *
	 * 确认传入的不是外部快照。
	 */
	Assert(snap->snapshot_type == SNAPSHOT_HISTORIC_MVCC);

	/* make sure nobody modified our snapshot
	 *
	 * 确认没有人改过我们的快照。
	 */
	Assert(snap->curcid == FirstCommandId);
	Assert(!snap->suboverflowed);
	Assert(!snap->takenDuringRecovery);
	Assert(snap->regd_count == 0);

	/* slightly more likely, so it's checked even without c-asserts
	 *
	 * 这种情况稍更可能发生，所以即使没有断言也会检查。
	 */
	if (snap->copied)
		elog(ERROR, "cannot free a copied snapshot");

	if (snap->active_count)
		elog(ERROR, "cannot free an active snapshot");

	pfree(snap);
}

/*
 * In which state of snapshot building are we?
 *
 * 当前快照构建处于哪个状态？
 */
SnapBuildState
SnapBuildCurrentState(SnapBuild *builder)
{
	return builder->state;
}

/*
 * Return the LSN at which the two-phase decoding was first enabled.
 *
 * 返回首次启用两阶段解码时的 LSN。
 */
XLogRecPtr
SnapBuildGetTwoPhaseAt(SnapBuild *builder)
{
	return builder->two_phase_at;
}

/*
 * Set the LSN at which two-phase decoding is enabled.
 *
 * 设置启用两阶段解码的 LSN。
 */
void
SnapBuildSetTwoPhaseAt(SnapBuild *builder, XLogRecPtr ptr)
{
	builder->two_phase_at = ptr;
}

/*
 * Should the contents of transaction ending at 'ptr' be decoded?
 *
 * 在 ptr 处结束的事务，其内容是否应当被解码？
 */
bool
SnapBuildXactNeedsSkip(SnapBuild *builder, XLogRecPtr ptr)
{
	return ptr < builder->start_decoding_at;
}

/*
 * Increase refcount of a snapshot.
 *
 * 增加快照的引用计数。
 *
 * This is used when handing out a snapshot to some external resource or when
 * adding a Snapshot as builder->snapshot.
 *
 * 把快照交给外部资源，或把一份 Snapshot 放进 builder 的 snapshot 时使用。
 */
static void
SnapBuildSnapIncRefcount(Snapshot snap)
{
	snap->active_count++;
}

/*
 * Decrease refcount of a snapshot and free if the refcount reaches zero.
 *
 * 减少快照的引用计数，降到零时释放它。
 *
 * Externally visible, so that external resources that have been handed an
 * IncRef'ed Snapshot can adjust its refcount easily.
 *
 * 对外部可见，这样拿到已经 IncRef 过的 Snapshot 的外部资源可以方便地调整引用计数。
 */
void
SnapBuildSnapDecRefcount(Snapshot snap)
{
	/* make sure we don't get passed an external snapshot
	 *
	 * 确认传入的不是外部快照。
	 */
	Assert(snap->snapshot_type == SNAPSHOT_HISTORIC_MVCC);

	/* make sure nobody modified our snapshot
	 *
	 * 确认没有人改过我们的快照。
	 */
	Assert(snap->curcid == FirstCommandId);
	Assert(!snap->suboverflowed);
	Assert(!snap->takenDuringRecovery);

	Assert(snap->regd_count == 0);

	Assert(snap->active_count > 0);

	/* slightly more likely, so it's checked even without casserts
	 *
	 * 这种情况稍更可能发生，所以即使没有断言也会检查。
	 */
	if (snap->copied)
		elog(ERROR, "cannot free a copied snapshot");

	snap->active_count--;
	if (snap->active_count == 0)
		SnapBuildFreeSnapshot(snap);
}

/*
 * Build a new snapshot, based on currently committed catalog-modifying
 * transactions.
 *
 * 根据当前已提交且修改了目录的事务，构建一份新快照。
 *
 * In-progress transactions with catalog access are *not* allowed to modify
 * these snapshots; they have to copy them and fill in appropriate ->curcid
 * and ->subxip/subxcnt values.
 *
 * 正在进行且访问了目录的事务不允许修改这些快照；它们必须复制一份，并填上合适的 curcid 以及 subxip、subxcnt。
 */
static Snapshot
SnapBuildBuildSnapshot(SnapBuild *builder)
{
	Snapshot	snapshot;
	Size		ssize;

	Assert(builder->state >= SNAPBUILD_FULL_SNAPSHOT);

	ssize = sizeof(SnapshotData)
		+ sizeof(TransactionId) * builder->committed.xcnt
		+ sizeof(TransactionId) * 1 /* toplevel xid
		 *
		 * 顶层 xid。
		 */

	snapshot = MemoryContextAllocZero(builder->context, ssize);

	snapshot->snapshot_type = SNAPSHOT_HISTORIC_MVCC;

	/*
	 * We misuse the original meaning of SnapshotData's xip and subxip fields
	 * to make the more fitting for our needs.
	 *
	 * 我们挪用了 SnapshotData 里 xip 和 subxip 字段原来的含义，使它们更符合这里的需要。
	 *
	 * In the 'xip' array we store transactions that have to be treated as
	 * committed. Since we will only ever look at tuples from transactions
	 * that have modified the catalog it's more efficient to store those few
	 * that exist between xmin and xmax (frequently there are none).
	 *
	 * 在 xip 数组里存放必须视为已提交的事务。因为我们只会查看修改过目录的事务产生的元组，所以只保存 xmin 与 xmax 之间那少数几个事务效率更高（经常一个都没有）。
	 *
	 * Snapshots that are used in transactions that have modified the catalog
	 * also use the 'subxip' array to store their toplevel xid and all the
	 * subtransaction xids so we can recognize when we need to treat rows as
	 * visible that are not in xip but still need to be visible. Subxip only
	 * gets filled when the transaction is copied into the context of a
	 * catalog modifying transaction since we otherwise share a snapshot
	 * between transactions. As long as a txn hasn't modified the catalog it
	 * doesn't need to treat any uncommitted rows as visible, so there is no
	 * need for those xids.
	 *
	 * 被修改过目录的事务所使用的快照，还会用 subxip 数组存放其顶层 xid 和全部子事务 xid，以便识别那些不在 xip 里、但仍然必须可见的行。只有把事务复制进一个修改目录的事务的上下文时才会填充 subxip，否则多个事务共享同一份快照。只要事务还没修改目录，就不需要把任何未提交的行视为可见，因此不需要那些 xid。
	 *
	 * Both arrays are qsort'ed so that we can use bsearch() on them.
	 *
	 * 两个数组都经过 qsort，以便对其使用 bsearch()。
	 */
	Assert(TransactionIdIsNormal(builder->xmin));
	Assert(TransactionIdIsNormal(builder->xmax));

	snapshot->xmin = builder->xmin;
	snapshot->xmax = builder->xmax;

	/* store all transactions to be treated as committed by this snapshot
	 *
	 * 保存本快照应视为已提交的全部事务。
	 */
	snapshot->xip =
		(TransactionId *) ((char *) snapshot + sizeof(SnapshotData));
	snapshot->xcnt = builder->committed.xcnt;
	memcpy(snapshot->xip,
		   builder->committed.xip,
		   builder->committed.xcnt * sizeof(TransactionId));

	/* sort so we can bsearch()
	 *
	 * 排序，以便使用 bsearch()。
	 */
	qsort(snapshot->xip, snapshot->xcnt, sizeof(TransactionId), xidComparator);

	/*
	 * Initially, subxip is empty, i.e. it's a snapshot to be used by
	 * transactions that don't modify the catalog. Will be filled by
	 * ReorderBufferCopySnap() if necessary.
	 *
	 * 最初 subxip 为空，也就是给不修改目录的事务使用的快照。必要时由 ReorderBufferCopySnap() 填充。
	 */
	snapshot->subxcnt = 0;
	snapshot->subxip = NULL;

	snapshot->suboverflowed = false;
	snapshot->takenDuringRecovery = false;
	snapshot->copied = false;
	snapshot->curcid = FirstCommandId;
	snapshot->active_count = 0;
	snapshot->regd_count = 0;
	snapshot->snapXactCompletionCount = 0;

	return snapshot;
}

/*
 * Build the initial slot snapshot and convert it to a normal snapshot that
 * is understood by HeapTupleSatisfiesMVCC.
 *
 * 构建槽的初始快照，并把它转换成 HeapTupleSatisfiesMVCC 能理解的普通快照。
 *
 * The snapshot will be usable directly in current transaction or exported
 * for loading in different transaction.
 *
 * 这份快照可以直接在当前事务中使用，也可以导出后在另一个事务中加载。
 */
Snapshot
SnapBuildInitialSnapshot(SnapBuild *builder)
{
	Snapshot	snap;
	TransactionId xid;
	TransactionId safeXid;
	TransactionId *newxip;
	int			newxcnt = 0;

	Assert(XactIsoLevel == XACT_REPEATABLE_READ);
	Assert(builder->building_full_snapshot);

	/* don't allow older snapshots
	 *
	 * 不允许更旧的快照。
	 */
	InvalidateCatalogSnapshot();	/* about to overwrite MyProc->xmin
	 *
	 * 即将覆盖 MyProc 的 xmin。
	 */
	if (HaveRegisteredOrActiveSnapshot())
		elog(ERROR, "cannot build an initial slot snapshot when snapshots exist");
	Assert(!HistoricSnapshotActive());

	if (builder->state != SNAPBUILD_CONSISTENT)
		elog(ERROR, "cannot build an initial slot snapshot before reaching a consistent state");

	if (!builder->committed.includes_all_transactions)
		elog(ERROR, "cannot build an initial slot snapshot, not all transactions are monitored anymore");

	/* so we don't overwrite the existing value
	 *
	 * 这样就不会覆盖已有的值。
	 */
	if (TransactionIdIsValid(MyProc->xmin))
		elog(ERROR, "cannot build an initial slot snapshot when MyProc->xmin already is valid");

	snap = SnapBuildBuildSnapshot(builder);

	/*
	 * We know that snap->xmin is alive, enforced by the logical xmin
	 * mechanism. Due to that we can do this without locks, we're only
	 * changing our own value.
	 *
	 * 我们知道 snap 的 xmin 仍然存活，这由逻辑 xmin 机制保证。因此可以不加锁，我们只改自己的值。
	 *
	 * Building an initial snapshot is expensive and an unenforced xmin
	 * horizon would have bad consequences, therefore always double-check that
	 * the horizon is enforced.
	 *
	 * 构建初始快照代价很高，若 xmin 视界没有被强制保住，后果会很严重，所以始终要再检查一次视界确实被保住了。
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	safeXid = GetOldestSafeDecodingTransactionId(false);
	LWLockRelease(ProcArrayLock);

	if (TransactionIdFollows(safeXid, snap->xmin))
		elog(ERROR, "cannot build an initial slot snapshot as oldest safe xid %u follows snapshot's xmin %u",
			 safeXid, snap->xmin);

	MyProc->xmin = snap->xmin;

	/* allocate in transaction context
	 *
	 * 在事务上下文中分配。
	 */
	newxip = (TransactionId *)
		palloc(sizeof(TransactionId) * GetMaxSnapshotXidCount());

	/*
	 * snapbuild.c builds transactions in an "inverted" manner, which means it
	 * stores committed transactions in ->xip, not ones in progress. Build a
	 * classical snapshot by marking all non-committed transactions as
	 * in-progress. This can be expensive.
	 *
	 * snapbuild.c 以一种颠倒的方式构建事务：它在 xip 里保存已提交的事务，而不是正在进行的事务。这里把所有未提交的事务标成进行中，从而建成一份经典快照。这一步可能比较贵。
	 */
	for (xid = snap->xmin; NormalTransactionIdPrecedes(xid, snap->xmax);)
	{
		void	   *test;

		/*
		 * Check whether transaction committed using the decoding snapshot
		 * meaning of ->xip.
		 *
		 * 按解码快照对 xip 的含义，检查事务是否已提交。
		 */
		test = bsearch(&xid, snap->xip, snap->xcnt,
					   sizeof(TransactionId), xidComparator);

		if (test == NULL)
		{
			if (newxcnt >= GetMaxSnapshotXidCount())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("initial slot snapshot too large")));

			newxip[newxcnt++] = xid;
		}

		TransactionIdAdvance(xid);
	}

	/* adjust remaining snapshot fields as needed
	 *
	 * 按需要调整快照的其余字段。
	 */
	snap->snapshot_type = SNAPSHOT_MVCC;
	snap->xcnt = newxcnt;
	snap->xip = newxip;

	return snap;
}

/*
 * Export a snapshot so it can be set in another session with SET TRANSACTION
 * SNAPSHOT.
 *
 * 导出一份快照，以便另一个会话用 SET TRANSACTION SNAPSHOT 来设置它。
 *
 * For that we need to start a transaction in the current backend as the
 * importing side checks whether the source transaction is still open to make
 * sure the xmin horizon hasn't advanced since then.
 *
 * 为此必须在当前后端开启一个事务，因为导入方会检查源事务是否仍开着，以确认 xmin 视界此后没有前进。
 */
const char *
SnapBuildExportSnapshot(SnapBuild *builder)
{
	Snapshot	snap;
	char	   *snapname;

	if (IsTransactionOrTransactionBlock())
		elog(ERROR, "cannot export a snapshot from within a transaction");

	if (SavedResourceOwnerDuringExport)
		elog(ERROR, "can only export one snapshot at a time");

	SavedResourceOwnerDuringExport = CurrentResourceOwner;
	ExportInProgress = true;

	StartTransactionCommand();

	/* There doesn't seem to a nice API to set these
	 *
	 * 似乎没有一个合适的 API 来设置这些值。
	 */
	XactIsoLevel = XACT_REPEATABLE_READ;
	XactReadOnly = true;

	snap = SnapBuildInitialSnapshot(builder);

	/*
	 * now that we've built a plain snapshot, make it active and use the
	 * normal mechanisms for exporting it
	 *
	 * 普通快照已经建好，现在把它设为活动快照，并用正常机制导出。
	 */
	snapname = ExportSnapshot(snap);

	ereport(LOG,
			(errmsg_plural("exported logical decoding snapshot: \"%s\" with %u transaction ID",
						   "exported logical decoding snapshot: \"%s\" with %u transaction IDs",
						   snap->xcnt,
						   snapname, snap->xcnt)));
	return snapname;
}

/*
 * Ensure there is a snapshot and if not build one for current transaction.
 *
 * 确保存在一份快照；如果没有，就为当前事务建一份。
 */
Snapshot
SnapBuildGetOrBuildSnapshot(SnapBuild *builder)
{
	Assert(builder->state == SNAPBUILD_CONSISTENT);

	/* only build a new snapshot if we don't have a prebuilt one
	 *
	 * 只有在没有预先建好的快照时才新建。
	 */
	if (builder->snapshot == NULL)
	{
		builder->snapshot = SnapBuildBuildSnapshot(builder);
		/* increase refcount for the snapshot builder
		 *
		 * 为快照构建器增加引用计数。
		 */
		SnapBuildSnapIncRefcount(builder->snapshot);
	}

	return builder->snapshot;
}

/*
 * Reset a previously SnapBuildExportSnapshot()'ed snapshot if there is
 * any. Aborts the previously started transaction and resets the resource
 * owner back to its original value.
 *
 * 如果先前用 SnapBuildExportSnapshot() 导出过快照，就把它复位。中止先前开启的事务，并把资源所有者恢复为原来的值。
 */
void
SnapBuildClearExportedSnapshot(void)
{
	ResourceOwner tmpResOwner;

	/* nothing exported, that is the usual case
	 *
	 * 没有导出任何东西，这是通常情况。
	 */
	if (!ExportInProgress)
		return;

	if (!IsTransactionState())
		elog(ERROR, "clearing exported snapshot in wrong transaction state");

	/*
	 * AbortCurrentTransaction() takes care of resetting the snapshot state,
	 * so remember SavedResourceOwnerDuringExport.
	 *
	 * AbortCurrentTransaction() 会负责重置快照状态，所以要记住 SavedResourceOwnerDuringExport。
	 */
	tmpResOwner = SavedResourceOwnerDuringExport;

	/* make sure nothing could have ever happened
	 *
	 * 确认不可能发生过任何事情。
	 */
	AbortCurrentTransaction();

	CurrentResourceOwner = tmpResOwner;
}

/*
 * Clear snapshot export state during transaction abort.
 *
 * 在事务中止时清除快照导出状态。
 */
void
SnapBuildResetExportedSnapshotState(void)
{
	SavedResourceOwnerDuringExport = NULL;
	ExportInProgress = false;
}

/*
 * Handle the effects of a single heap change, appropriate to the current state
 * of the snapshot builder and returns whether changes made at (xid, lsn) can
 * be decoded.
 *
 * 按快照构建器的当前状态，处理单条堆变更的影响，并返回位于该 xid 与 lsn 的变更能否被解码。
 */
bool
SnapBuildProcessChange(SnapBuild *builder, TransactionId xid, XLogRecPtr lsn)
{
	/*
	 * We can't handle data in transactions if we haven't built a snapshot
	 * yet, so don't store them.
	 *
	 * 如果还没有建成快照，就无法处理事务中的数据，所以不要保存它们。
	 */
	if (builder->state < SNAPBUILD_FULL_SNAPSHOT)
		return false;

	/*
	 * No point in keeping track of changes in transactions that we don't have
	 * enough information about to decode. This means that they started before
	 * we got into the SNAPBUILD_FULL_SNAPSHOT state.
	 *
	 * 对那些信息不足以解码的事务，跟踪其变更没有意义。这些事务在我们进入 SNAPBUILD_FULL_SNAPSHOT 之前就已经开始了。
	 */
	if (builder->state < SNAPBUILD_CONSISTENT &&
		TransactionIdPrecedes(xid, builder->next_phase_at))
		return false;

	/*
	 * If the reorderbuffer doesn't yet have a snapshot, add one now, it will
	 * be needed to decode the change we're currently processing.
	 *
	 * 如果 reorderbuffer 还没有快照，现在就加一份，解码当前正在处理的变更时会用到。
	 */
	if (!ReorderBufferXidHasBaseSnapshot(builder->reorder, xid))
	{
		/* only build a new snapshot if we don't have a prebuilt one
		 *
		 * 只有在没有预先建好的快照时才新建。
		 */
		if (builder->snapshot == NULL)
		{
			builder->snapshot = SnapBuildBuildSnapshot(builder);
			/* increase refcount for the snapshot builder
			 *
			 * 为快照构建器增加引用计数。
			 */
			SnapBuildSnapIncRefcount(builder->snapshot);
		}

		/*
		 * Increase refcount for the transaction we're handing the snapshot
		 * out to.
		 *
		 * 把快照交给该事务时，增加引用计数。
		 */
		SnapBuildSnapIncRefcount(builder->snapshot);
		ReorderBufferSetBaseSnapshot(builder->reorder, xid, lsn,
									 builder->snapshot);
	}

	return true;
}

/*
 * Do CommandId/combo CID handling after reading an xl_heap_new_cid record.
 * This implies that a transaction has done some form of write to system
 * catalogs.
 *
 * 读到 xl_heap_new_cid 记录后处理 CommandId 和 combo CID。这意味着某个事务对系统目录做了某种写入。
 */
void
SnapBuildProcessNewCid(SnapBuild *builder, TransactionId xid,
					   XLogRecPtr lsn, xl_heap_new_cid *xlrec)
{
	CommandId	cid;

	/*
	 * we only log new_cid's if a catalog tuple was modified, so mark the
	 * transaction as containing catalog modifications
	 *
	 * 只有目录元组被修改时才会记录 new_cid，因此把该事务标为包含目录修改。
	 */
	ReorderBufferXidSetCatalogChanges(builder->reorder, xid, lsn);

	ReorderBufferAddNewTupleCids(builder->reorder, xlrec->top_xid, lsn,
								 xlrec->target_locator, xlrec->target_tid,
								 xlrec->cmin, xlrec->cmax,
								 xlrec->combocid);

	/* figure out new command id
	 *
	 * 算出新的命令标识。
	 */
	if (xlrec->cmin != InvalidCommandId &&
		xlrec->cmax != InvalidCommandId)
		cid = Max(xlrec->cmin, xlrec->cmax);
	else if (xlrec->cmax != InvalidCommandId)
		cid = xlrec->cmax;
	else if (xlrec->cmin != InvalidCommandId)
		cid = xlrec->cmin;
	else
	{
		cid = InvalidCommandId; /* silence compiler
		 *
		 * 让编译器不再报警。
		 */
		elog(ERROR, "xl_heap_new_cid record without a valid CommandId");
	}

	ReorderBufferAddNewCommandId(builder->reorder, xid, lsn, cid + 1);
}

/*
 * Add a new Snapshot and invalidation messages to all transactions we're
 * decoding that currently are in-progress so they can see new catalog contents
 * made by the transaction that just committed. This is necessary because those
 * in-progress transactions will use the new catalog's contents from here on
 * (at the very least everything they do needs to be compatible with newer
 * catalog contents).
 *
 * 把一份新的 Snapshot 和失效消息加给所有正在解码、且当前仍在进行的事务，使它们能看到刚刚提交的事务所产生的新目录内容。这是必需的，因为这些进行中的事务此后会使用新的目录内容（至少它们所做的一切都必须与更新后的目录兼容）。
 */
static void
SnapBuildDistributeSnapshotAndInval(SnapBuild *builder, XLogRecPtr lsn, TransactionId xid)
{
	dlist_iter	txn_i;
	ReorderBufferTXN *txn;

	/*
	 * Iterate through all toplevel transactions. This can include
	 * subtransactions which we just don't yet know to be that, but that's
	 * fine, they will just get an unnecessary snapshot and invalidations
	 * queued.
	 *
	 * 遍历所有顶层事务。其中可能包含我们还不知道是子事务的子事务，这没有关系，它们只是会多拿到一份不必要的快照和失效消息。
	 */
	dlist_foreach(txn_i, &builder->reorder->toplevel_by_lsn)
	{
		txn = dlist_container(ReorderBufferTXN, node, txn_i.cur);

		Assert(TransactionIdIsValid(txn->xid));

		/*
		 * If we don't have a base snapshot yet, there are no changes in this
		 * transaction which in turn implies we don't yet need a snapshot at
		 * all. We'll add a snapshot when the first change gets queued.
		 *
		 * 如果还没有基础快照，说明这个事务里还没有变更，因而现在根本不需要快照。第一条变更入队时会再加快照。
		 *
		 * Similarly, we don't need to add invalidations to a transaction
		 * whose base snapshot is not yet set. Once a base snapshot is built,
		 * it will include the xids of committed transactions that have
		 * modified the catalog, thus reflecting the new catalog contents. The
		 * existing catalog cache will have already been invalidated after
		 * processing the invalidations in the transaction that modified
		 * catalogs, ensuring that a fresh cache is constructed during
		 * decoding.
		 *
		 * 同样，基础快照尚未设置的事务也不需要加入失效消息。一旦建成基础快照，它会包含已提交且修改过目录的事务 xid，从而反映新的目录内容。处理完修改目录的事务中的失效消息后，现有目录缓存已经被失效，解码时会重建新的缓存。
		 *
		 * NB: This works correctly even for subtransactions because
		 * ReorderBufferAssignChild() takes care to transfer the base snapshot
		 * to the top-level transaction, and while iterating the changequeue
		 * we'll get the change from the subtxn.
		 *
		 * 注意：这对子事务也正确，因为 ReorderBufferAssignChild() 会把基础快照转移到顶层事务，遍历变更队列时会从子事务拿到该变更。
		 */
		if (!ReorderBufferXidHasBaseSnapshot(builder->reorder, txn->xid))
			continue;

		/*
		 * We don't need to add snapshot or invalidations to prepared
		 * transactions as they should not see the new catalog contents.
		 *
		 * 不必给已准备事务添加快照或失效消息，它们不应该看到新的目录内容。
		 */
		if (rbtxn_is_prepared(txn))
			continue;

		elog(DEBUG2, "adding a new snapshot and invalidations to %u at %X/%X",
			 txn->xid, LSN_FORMAT_ARGS(lsn));

		/*
		 * increase the snapshot's refcount for the transaction we are handing
		 * it out to
		 *
		 * 把快照交给该事务时，增加快照的引用计数。
		 */
		SnapBuildSnapIncRefcount(builder->snapshot);
		ReorderBufferAddSnapshot(builder->reorder, txn->xid, lsn,
								 builder->snapshot);

		/*
		 * Add invalidation messages to the reorder buffer of in-progress
		 * transactions except the current committed transaction, for which we
		 * will execute invalidations at the end.
		 *
		 * 把失效消息加入进行中事务的 reorder buffer，当前这个已提交的事务除外，它的失效消息会在结束时执行。
		 *
		 * It is required, otherwise, we will end up using the stale catcache
		 * contents built by the current transaction even after its decoding,
		 * which should have been invalidated due to concurrent catalog
		 * changing transaction.
		 *
		 * 必须这样做，否则即使当前事务解码结束，仍会使用它建起来的过期 catcache 内容，而这些内容本应因并发的目录修改事务而失效。
		 *
		 * Distribute only the invalidation messages generated by the current
		 * committed transaction. Invalidation messages received from other
		 * transactions would have already been propagated to the relevant
		 * in-progress transactions. This transaction would have processed
		 * those invalidations, ensuring that subsequent transactions observe
		 * a consistent cache state.
		 *
		 * 只分发当前已提交事务产生的失效消息。从其他事务收到的失效消息已经传播给相关的进行中事务。本事务会处理那些失效，从而让后续事务看到一致的缓存状态。
		 */
		if (txn->xid != xid)
		{
			uint32		ninvalidations;
			SharedInvalidationMessage *msgs = NULL;

			ninvalidations = ReorderBufferGetInvalidations(builder->reorder,
														   xid, &msgs);

			if (ninvalidations > 0)
			{
				Assert(msgs != NULL);

				ReorderBufferAddDistributedInvalidations(builder->reorder,
														 txn->xid, lsn,
														 ninvalidations, msgs);
			}
		}
	}
}

/*
 * Keep track of a new catalog changing transaction that has committed.
 *
 * 记录一个新的、已提交且修改了目录的事务。
 */
static void
SnapBuildAddCommittedTxn(SnapBuild *builder, TransactionId xid)
{
	Assert(TransactionIdIsValid(xid));

	if (builder->committed.xcnt == builder->committed.xcnt_space)
	{
		builder->committed.xcnt_space = builder->committed.xcnt_space * 2 + 1;

		elog(DEBUG1, "increasing space for committed transactions to %u",
			 (uint32) builder->committed.xcnt_space);

		builder->committed.xip = repalloc(builder->committed.xip,
										  builder->committed.xcnt_space * sizeof(TransactionId));
	}

	/*
	 * TODO: It might make sense to keep the array sorted here instead of
	 * doing it every time we build a new snapshot. On the other hand this
	 * gets called repeatedly when a transaction with subtransactions commits.
	 *
	 * 待办：也许应该在这里就保持数组有序，而不是每次建新快照时再排序。另一方面，带有子事务的事务提交时这里会被反复调用。
	 */
	builder->committed.xip[builder->committed.xcnt++] = xid;
}

/*
 * Remove knowledge about transactions we treat as committed or containing catalog
 * changes that are smaller than ->xmin. Those won't ever get checked via
 * the ->committed or ->catchange array, respectively. The committed xids will
 * get checked via the clog machinery.
 *
 * 丢掉那些我们视为已提交、或包含目录变更、但小于 xmin 的事务。它们再也不会通过 committed 或 catchange 数组被检查。已提交的 xid 会通过 clog 机制来检查。
 *
 * We can ideally remove the transaction from catchange array once it is
 * finished (committed/aborted) but that could be costly as we need to maintain
 * the xids order in the array.
 *
 * 理想情况下，事务结束（提交或中止）后就可以把它从 catchange 数组去掉，但维持数组中 xid 的顺序可能代价较高。
 */
static void
SnapBuildPurgeOlderTxn(SnapBuild *builder)
{
	int			off;
	TransactionId *workspace;
	int			surviving_xids = 0;

	/* not ready yet
	 *
	 * 尚未就绪。
	 */
	if (!TransactionIdIsNormal(builder->xmin))
		return;

	/* TODO: Neater algorithm than just copying and iterating?
	 *
	 * 待办：有没有比复制再遍历更整齐的算法？
	 */
	workspace =
		MemoryContextAlloc(builder->context,
						   builder->committed.xcnt * sizeof(TransactionId));

	/* copy xids that still are interesting to workspace
	 *
	 * 把仍然有用的 xid 复制到工作区。
	 */
	for (off = 0; off < builder->committed.xcnt; off++)
	{
		if (NormalTransactionIdPrecedes(builder->committed.xip[off],
										builder->xmin))
			;					/* remove
			 *
			 * 删除。
			 */
		else
			workspace[surviving_xids++] = builder->committed.xip[off];
	}

	/* copy workspace back to persistent state
	 *
	 * 把工作区复制回持久状态。
	 */
	memcpy(builder->committed.xip, workspace,
		   surviving_xids * sizeof(TransactionId));

	elog(DEBUG3, "purged committed transactions from %u to %u, xmin: %u, xmax: %u",
		 (uint32) builder->committed.xcnt, (uint32) surviving_xids,
		 builder->xmin, builder->xmax);
	builder->committed.xcnt = surviving_xids;

	pfree(workspace);

	/*
	 * Purge xids in ->catchange as well. The purged array must also be sorted
	 * in xidComparator order.
	 *
	 * 同时清理 catchange 中的 xid。清理后的数组仍必须按 xidComparator 的顺序排序。
	 */
	if (builder->catchange.xcnt > 0)
	{
		/*
		 * Since catchange.xip is sorted, we find the lower bound of xids that
		 * are still interesting.
		 *
		 * 因为 catchange.xip 是有序的，所以找出仍然有用的 xid 的下界。
		 */
		for (off = 0; off < builder->catchange.xcnt; off++)
		{
			if (TransactionIdFollowsOrEquals(builder->catchange.xip[off],
											 builder->xmin))
				break;
		}

		surviving_xids = builder->catchange.xcnt - off;

		if (surviving_xids > 0)
		{
			memmove(builder->catchange.xip, &(builder->catchange.xip[off]),
					surviving_xids * sizeof(TransactionId));
		}
		else
		{
			pfree(builder->catchange.xip);
			builder->catchange.xip = NULL;
		}

		elog(DEBUG3, "purged catalog modifying transactions from %u to %u, xmin: %u, xmax: %u",
			 (uint32) builder->catchange.xcnt, (uint32) surviving_xids,
			 builder->xmin, builder->xmax);
		builder->catchange.xcnt = surviving_xids;
	}
}

/*
 * Handle everything that needs to be done when a transaction commits
 *
 * 处理事务提交时需要做的全部事情。
 */
void
SnapBuildCommitTxn(SnapBuild *builder, XLogRecPtr lsn, TransactionId xid,
				   int nsubxacts, TransactionId *subxacts, uint32 xinfo)
{
	int			nxact;

	bool		needs_snapshot = false;
	bool		needs_timetravel = false;
	bool		sub_needs_timetravel = false;

	TransactionId xmax = xid;

	/*
	 * Transactions preceding BUILDING_SNAPSHOT will neither be decoded, nor
	 * will they be part of a snapshot.  So we don't need to record anything.
	 *
	 * BUILDING_SNAPSHOT 之前的事务既不会被解码，也不会进入快照，所以不必记录任何东西。
	 */
	if (builder->state == SNAPBUILD_START ||
		(builder->state == SNAPBUILD_BUILDING_SNAPSHOT &&
		 TransactionIdPrecedes(xid, builder->next_phase_at)))
	{
		/* ensure that only commits after this are getting replayed
		 *
		 * 确保只有此后的提交才会被重放。
		 */
		if (builder->start_decoding_at <= lsn)
			builder->start_decoding_at = lsn + 1;
		return;
	}

	if (builder->state < SNAPBUILD_CONSISTENT)
	{
		/* ensure that only commits after this are getting replayed
		 *
		 * 确保只有此后的提交才会被重放。
		 */
		if (builder->start_decoding_at <= lsn)
			builder->start_decoding_at = lsn + 1;

		/*
		 * If building an exportable snapshot, force xid to be tracked, even
		 * if the transaction didn't modify the catalog.
		 *
		 * 如果正在构建可导出的快照，即使该事务没有修改目录，也强制跟踪这个 xid。
		 */
		if (builder->building_full_snapshot)
		{
			needs_timetravel = true;
		}
	}

	for (nxact = 0; nxact < nsubxacts; nxact++)
	{
		TransactionId subxid = subxacts[nxact];

		/*
		 * Add subtransaction to base snapshot if catalog modifying, we don't
		 * distinguish to toplevel transactions there.
		 *
		 * 若子事务修改了目录，就把它加入基础快照；在那里我们不区分它和顶层事务。
		 */
		if (SnapBuildXidHasCatalogChanges(builder, subxid, xinfo))
		{
			sub_needs_timetravel = true;
			needs_snapshot = true;

			elog(DEBUG1, "found subtransaction %u:%u with catalog changes",
				 xid, subxid);

			SnapBuildAddCommittedTxn(builder, subxid);

			if (NormalTransactionIdFollows(subxid, xmax))
				xmax = subxid;
		}

		/*
		 * If we're forcing timetravel we also need visibility information
		 * about subtransaction, so keep track of subtransaction's state, even
		 * if not catalog modifying.  Don't need to distribute a snapshot in
		 * that case.
		 *
		 * 如果正在强制时间旅行，也需要子事务的可见性信息，因此即使子事务没有修改目录，也跟踪它的状态。这种情况下不必分发快照。
		 */
		else if (needs_timetravel)
		{
			SnapBuildAddCommittedTxn(builder, subxid);
			if (NormalTransactionIdFollows(subxid, xmax))
				xmax = subxid;
		}
	}

	/* if top-level modified catalog, it'll need a snapshot
	 *
	 * 如果顶层事务修改了目录，它就需要一份快照。
	 */
	if (SnapBuildXidHasCatalogChanges(builder, xid, xinfo))
	{
		elog(DEBUG2, "found top level transaction %u, with catalog changes",
			 xid);
		needs_snapshot = true;
		needs_timetravel = true;
		SnapBuildAddCommittedTxn(builder, xid);
	}
	else if (sub_needs_timetravel)
	{
		/* track toplevel txn as well, subxact alone isn't meaningful
		 *
		 * 同时跟踪顶层事务，单独的子事务没有意义。
		 */
		elog(DEBUG2, "forced transaction %u to do timetravel due to one of its subtransactions",
			 xid);
		needs_timetravel = true;
		SnapBuildAddCommittedTxn(builder, xid);
	}
	else if (needs_timetravel)
	{
		elog(DEBUG2, "forced transaction %u to do timetravel", xid);

		SnapBuildAddCommittedTxn(builder, xid);
	}

	if (!needs_timetravel)
	{
		/* record that we cannot export a general snapshot anymore
		 *
		 * 记录我们已经不能再导出通用快照。
		 */
		builder->committed.includes_all_transactions = false;
	}

	Assert(!needs_snapshot || needs_timetravel);

	/*
	 * Adjust xmax of the snapshot builder, we only do that for committed,
	 * catalog modifying, transactions, everything else isn't interesting for
	 * us since we'll never look at the respective rows.
	 *
	 * 调整快照构建器的 xmax。只对已提交且修改了目录的事务这样做，其他事务对我们没有意义，因为我们永远不会查看那些行。
	 */
	if (needs_timetravel &&
		(!TransactionIdIsValid(builder->xmax) ||
		 TransactionIdFollowsOrEquals(xmax, builder->xmax)))
	{
		builder->xmax = xmax;
		TransactionIdAdvance(builder->xmax);
	}

	/* if there's any reason to build a historic snapshot, do so now
	 *
	 * 如果有任何理由要构建历史快照，现在就构建。
	 */
	if (needs_snapshot)
	{
		/*
		 * If we haven't built a complete snapshot yet there's no need to hand
		 * it out, it wouldn't (and couldn't) be used anyway.
		 *
		 * 如果还没有建成完整快照，就不必把它交出去，它反正不会、也不能被使用。
		 */
		if (builder->state < SNAPBUILD_FULL_SNAPSHOT)
			return;

		/*
		 * Decrease the snapshot builder's refcount of the old snapshot, note
		 * that it still will be used if it has been handed out to the
		 * reorderbuffer earlier.
		 *
		 * 减少快照构建器对旧快照的引用计数。注意如果先前已经把它交给 reorderbuffer，那份引用仍会继续使用它。
		 */
		if (builder->snapshot)
			SnapBuildSnapDecRefcount(builder->snapshot);

		builder->snapshot = SnapBuildBuildSnapshot(builder);

		/* we might need to execute invalidations, add snapshot
		 *
		 * 可能需要执行失效，并添加快照。
		 */
		if (!ReorderBufferXidHasBaseSnapshot(builder->reorder, xid))
		{
			SnapBuildSnapIncRefcount(builder->snapshot);
			ReorderBufferSetBaseSnapshot(builder->reorder, xid, lsn,
										 builder->snapshot);
		}

		/* refcount of the snapshot builder for the new snapshot
		 *
		 * 快照构建器对新快照持有的引用计数。
		 */
		SnapBuildSnapIncRefcount(builder->snapshot);

		/*
		 * Add a new catalog snapshot and invalidations messages to all
		 * currently running transactions.
		 *
		 * 给所有当前正在运行的事务添加一份新的目录快照和失效消息。
		 */
		SnapBuildDistributeSnapshotAndInval(builder, lsn, xid);
	}
}

/*
 * Check the reorder buffer and the snapshot to see if the given transaction has
 * modified catalogs.
 *
 * 检查 reorder buffer 和快照，看给定事务是否修改过目录。
 */
static inline bool
SnapBuildXidHasCatalogChanges(SnapBuild *builder, TransactionId xid,
							  uint32 xinfo)
{
	if (ReorderBufferXidHasCatalogChanges(builder->reorder, xid))
		return true;

	/*
	 * The transactions that have changed catalogs must have invalidation
	 * info.
	 *
	 * 修改过目录的事务必须带有失效信息。
	 */
	if (!(xinfo & XACT_XINFO_HAS_INVALS))
		return false;

	/* Check the catchange XID array
	 *
	 * 检查 catchange 的 XID 数组。
	 */
	return ((builder->catchange.xcnt > 0) &&
			(bsearch(&xid, builder->catchange.xip, builder->catchange.xcnt,
					 sizeof(TransactionId), xidComparator) != NULL));
}

/* -----------------------------------
 * Snapshot building functions dealing with xlog records
 *
 * 处理 xlog 记录的快照构建函数。
 *
 * -----------------------------------
 */

/*
 * Process a running xacts record, and use its information to first build a
 * historic snapshot and later to release resources that aren't needed
 * anymore.
 *
 * 处理一条 running xacts 记录，先用它的信息构建历史快照，之后再释放不再需要的资源。
 */
void
SnapBuildProcessRunningXacts(SnapBuild *builder, XLogRecPtr lsn, xl_running_xacts *running)
{
	ReorderBufferTXN *txn;
	TransactionId xmin;

	/*
	 * If we're not consistent yet, inspect the record to see whether it
	 * allows to get closer to being consistent. If we are consistent, dump
	 * our snapshot so others or we, after a restart, can use it.
	 *
	 * 如果还没到达一致状态，就检查这条记录能否让我们更接近一致。如果已经一致，就把快照转储出去，供别人或我们自己在重启后使用。
	 */
	if (builder->state < SNAPBUILD_CONSISTENT)
	{
		/* returns false if there's no point in performing cleanup just yet
		 *
		 * 若现在做清理还没有意义，则返回假。
		 */
		if (!SnapBuildFindSnapshot(builder, lsn, running))
			return;
	}
	else
		SnapBuildSerialize(builder, lsn);

	/*
	 * Update range of interesting xids based on the running xacts
	 * information. We don't increase ->xmax using it, because once we are in
	 * a consistent state we can do that ourselves and much more efficiently
	 * so, because we only need to do it for catalog transactions since we
	 * only ever look at those.
	 *
	 * 根据 running xacts 信息更新感兴趣的 xid 范围。不要用它来增大 xmax，因为一旦进入一致状态，我们自己就能更高效地做这件事：只需对目录事务做，而我们只会查看那些事务。
	 *
	 * NB: We only increase xmax when a catalog modifying transaction commits
	 * (see SnapBuildCommitTxn).  Because of this, xmax can be lower than
	 * xmin, which looks odd but is correct and actually more efficient, since
	 * we hit fast paths in heapam_visibility.c.
	 *
	 * 注意：只有修改目录的事务提交时才增大 xmax（见 SnapBuildCommitTxn）。因此 xmax 可以小于 xmin，看起来奇怪，但其实正确，而且更高效，因为能走到 heapam_visibility.c 里的快路径。
	 */
	builder->xmin = running->oldestRunningXid;

	/* Remove transactions we don't need to keep track off anymore
	 *
	 * 去掉那些不必再跟踪的事务。
	 */
	SnapBuildPurgeOlderTxn(builder);

	/*
	 * Advance the xmin limit for the current replication slot, to allow
	 * vacuum to clean up the tuples this slot has been protecting.
	 *
	 * 推进当前复制槽的 xmin 限制，让 vacuum 可以清理这个槽一直保护着的元组。
	 *
	 * The reorderbuffer might have an xmin among the currently running
	 * snapshots; use it if so.  If not, we need only consider the snapshots
	 * we'll produce later, which can't be less than the oldest running xid in
	 * the record we're reading now.
	 *
	 * reorderbuffer 的当前运行快照里可能有一个 xmin；如果有就用它。如果没有，只需考虑以后会产生的快照，它们不会小于当前这条记录里最老的正在运行的 xid。
	 */
	xmin = ReorderBufferGetOldestXmin(builder->reorder);
	if (xmin == InvalidTransactionId)
		xmin = running->oldestRunningXid;
	elog(DEBUG3, "xmin: %u, xmax: %u, oldest running: %u, oldest xmin: %u",
		 builder->xmin, builder->xmax, running->oldestRunningXid, xmin);
	LogicalIncreaseXminForSlot(lsn, xmin);

	/*
	 * Also tell the slot where we can restart decoding from. We don't want to
	 * do that after every commit because changing that implies an fsync of
	 * the logical slot's state file, so we only do it every time we see a
	 * running xacts record.
	 *
	 * 同时告诉槽可以从哪里重新开始解码。不想在每次提交后都做这件事，因为改这个位置意味着要对逻辑槽的状态文件做 fsync，所以只在看到 running xacts 记录时才做。
	 *
	 * Do so by looking for the oldest in progress transaction (determined by
	 * the first LSN of any of its relevant records). Every transaction
	 * remembers the last location we stored the snapshot to disk before its
	 * beginning. That point is where we can restart from.
	 *
	 * 做法是寻找最老的进行中事务（由其任一相关记录的第一个 LSN 决定）。每个事务都记住自己开始之前、我们最后一次把快照写到磁盘的位置。那个点就是可以重新开始的位置。
	 */

	/*
	 * Can't know about a serialized snapshot's location if we're not
	 * consistent.
	 *
	 * 如果还没到达一致状态，就无法知道已序列化快照的位置。
	 */
	if (builder->state < SNAPBUILD_CONSISTENT)
		return;

	txn = ReorderBufferGetOldestTXN(builder->reorder);

	/*
	 * oldest ongoing txn might have started when we didn't yet serialize
	 * anything because we hadn't reached a consistent state yet.
	 *
	 * 最老的进行中事务可能在我们还没序列化任何东西时就开始了，因为当时还没到达一致状态。
	 */
	if (txn != NULL && txn->restart_decoding_lsn != InvalidXLogRecPtr)
		LogicalIncreaseRestartDecodingForSlot(lsn, txn->restart_decoding_lsn);

	/*
	 * No in-progress transaction, can reuse the last serialized snapshot if
	 * we have one.
	 *
	 * 没有进行中的事务时，如果已有最后一份序列化快照，可以复用它。
	 */
	else if (txn == NULL &&
			 builder->reorder->current_restart_decoding_lsn != InvalidXLogRecPtr &&
			 builder->last_serialized_snapshot != InvalidXLogRecPtr)
		LogicalIncreaseRestartDecodingForSlot(lsn,
											  builder->last_serialized_snapshot);
}


/*
 * Build the start of a snapshot that's capable of decoding the catalog.
 *
 * 开始构建一份能够解码目录的快照。
 *
 * Helper function for SnapBuildProcessRunningXacts() while we're not yet
 * consistent.
 *
 * 在尚未一致时，供 SnapBuildProcessRunningXacts() 使用的辅助函数。
 *
 * Returns true if there is a point in performing internal maintenance/cleanup
 * using the xl_running_xacts record.
 *
 * 若值得用这条 xl_running_xacts 记录做内部维护或清理，则返回真。
 */
static bool
SnapBuildFindSnapshot(SnapBuild *builder, XLogRecPtr lsn, xl_running_xacts *running)
{
	/* ---
	 * Build catalog decoding snapshot incrementally using information about
	 * the currently running transactions. There are several ways to do that:
	 *
	 * 利用当前正在运行的事务的信息，逐步构建目录解码快照。有几种做法：
	 *
	 * a) There were no running transactions when the xl_running_xacts record
	 *	  was inserted, jump to CONSISTENT immediately. We might find such a
	 *	  state while waiting on c)'s sub-states.
	 *
	 * a) 插入 xl_running_xacts 记录时没有正在运行的事务，立即跳到 CONSISTENT。在等待下面 c) 的子状态时也可能遇到这种状态。
	 *
	 * b) This (in a previous run) or another decoding slot serialized a
	 *	  snapshot to disk that we can use. Can't use this method while finding
	 *	  the start point for decoding changes as the restart LSN would be an
	 *	  arbitrary LSN but we need to find the start point to extract changes
	 *	  where we won't see the data for partial transactions. Also, we cannot
	 *	  use this method when a slot needs a full snapshot for export or direct
	 *	  use, as that snapshot will only contain catalog modifying transactions.
	 *
	 * b) 本次（先前的一次运行）或另一个解码槽已经把快照序列化到磁盘，我们可以使用它。寻找解码变更的起点时不能用这个方法，因为 restart LSN 会是任意 LSN，而我们需要找到一个起点，使得提取变更时不会看到不完整事务的数据。当槽需要一份完整快照用于导出或直接使用时也不能用这个方法，因为那份快照只会包含修改目录的事务。
	 *
	 * c) First incrementally build a snapshot for catalog tuples
	 *	  (BUILDING_SNAPSHOT), that requires all, already in-progress,
	 *	  transactions to finish.  Every transaction starting after that
	 *	  (FULL_SNAPSHOT state), has enough information to be decoded.  But
	 *	  for older running transactions no viable snapshot exists yet, so
	 *	  CONSISTENT will only be reached once all of those have finished.
	 *
	 * c) 先为目录元组逐步构建快照（BUILDING_SNAPSHOT），这要求所有已经在进行的事务都结束。此后开始的每个事务（FULL_SNAPSHOT 状态）都有足够信息可以被解码。但对更老的正在运行的事务，还没有可用的快照，所以只有它们全部结束后才会到达 CONSISTENT。
	 *
	 * ---
	 */

	/*
	 * xl_running_xacts record is older than what we can use, we might not
	 * have all necessary catalog rows anymore.
	 *
	 * xl_running_xacts 记录比我们能用的还旧，可能已经不再拥有全部必需的目录行。
	 */
	if (TransactionIdIsNormal(builder->initial_xmin_horizon) &&
		NormalTransactionIdPrecedes(running->oldestRunningXid,
									builder->initial_xmin_horizon))
	{
		ereport(DEBUG1,
				(errmsg_internal("skipping snapshot at %X/%X while building logical decoding snapshot, xmin horizon too low",
								 LSN_FORMAT_ARGS(lsn)),
				 errdetail_internal("initial xmin horizon of %u vs the snapshot's %u",
									builder->initial_xmin_horizon, running->oldestRunningXid)));


		SnapBuildWaitSnapshot(running, builder->initial_xmin_horizon);

		return true;
	}

	/*
	 * a) No transaction were running, we can jump to consistent.
	 *
	 * a) 当时没有事务在运行，可以直接跳到一致状态。
	 *
	 * This is not affected by races around xl_running_xacts, because we can
	 * miss transaction commits, but currently not transactions starting.
	 *
	 * 这不受 xl_running_xacts 周围竞态的影响，因为我们可能漏掉事务提交，但目前不会漏掉事务开始。
	 *
	 * NB: We might have already started to incrementally assemble a snapshot,
	 * so we need to be careful to deal with that.
	 *
	 * 注意：我们可能已经开始逐步拼装快照，所以要小心地处理这种情况。
	 */
	if (running->oldestRunningXid == running->nextXid)
	{
		if (builder->start_decoding_at == InvalidXLogRecPtr ||
			builder->start_decoding_at <= lsn)
			/* can decode everything after this
			 *
			 * 此后的一切都可以解码。
			 */
			builder->start_decoding_at = lsn + 1;

		/* As no transactions were running xmin/xmax can be trivially set.
		 *
		 * 因为没有事务在运行，xmin 和 xmax 可以简单地设置。
		 */
		builder->xmin = running->nextXid;	/* < are finished
		 *
		 * 小于该值的事务已经结束。
		 */
		builder->xmax = running->nextXid;	/* >= are running
		 *
		 * 大于等于该值的事务仍在运行。
		 */

		/* so we can safely use the faster comparisons
		 *
		 * 因此可以安全地使用更快的比较。
		 */
		Assert(TransactionIdIsNormal(builder->xmin));
		Assert(TransactionIdIsNormal(builder->xmax));

		builder->state = SNAPBUILD_CONSISTENT;
		builder->next_phase_at = InvalidTransactionId;

		ereport(LOG,
				(errmsg("logical decoding found consistent point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("There are no running transactions.")));

		return false;
	}

	/*
	 * b) valid on disk state and while neither building full snapshot nor
	 * creating a slot.
	 *
	 * b) 磁盘上的状态有效，并且既不是在构建完整快照，也不是在创建槽。
	 */
	else if (!builder->building_full_snapshot &&
			 !builder->in_slot_creation &&
			 SnapBuildRestore(builder, lsn))
	{
		/* there won't be any state to cleanup
		 *
		 * 不会有需要清理的状态。
		 */
		return false;
	}

	/*
	 * c) transition from START to BUILDING_SNAPSHOT.
	 *
	 * c) 从 START 迁移到 BUILDING_SNAPSHOT。
	 *
	 * In START state, and a xl_running_xacts record with running xacts is
	 * encountered.  In that case, switch to BUILDING_SNAPSHOT state, and
	 * record xl_running_xacts->nextXid.  Once all running xacts have finished
	 * (i.e. they're all >= nextXid), we have a complete catalog snapshot.  It
	 * might look that we could use xl_running_xacts's ->xids information to
	 * get there quicker, but that is problematic because transactions marked
	 * as running, might already have inserted their commit record - it's
	 * infeasible to change that with locking.
	 *
	 * 处于 START 状态，并且遇到一条带有正在运行事务的 xl_running_xacts 记录。此时切换到 BUILDING_SNAPSHOT，并记下 xl_running_xacts 的 nextXid。一旦所有正在运行的事务都结束（也就是它们的 xid 都大于等于 nextXid），就有了完整的目录快照。看起来也许可以用 xl_running_xacts 的 xids 更快到达那里，但这有问题：被标为正在运行的事务可能已经写入了提交记录，用加锁来改变这一点并不可行。
	 */
	else if (builder->state == SNAPBUILD_START)
	{
		builder->state = SNAPBUILD_BUILDING_SNAPSHOT;
		builder->next_phase_at = running->nextXid;

		/*
		 * Start with an xmin/xmax that's correct for future, when all the
		 * currently running transactions have finished. We'll update both
		 * while waiting for the pending transactions to finish.
		 *
		 * 先设置一个对未来正确的 xmin 和 xmax，即当前正在运行的事务全部结束后的值。等待这些未完成事务结束的过程中，会再更新这两个值。
		 */
		builder->xmin = running->nextXid;	/* < are finished
		 *
		 * 小于该值的事务已经结束。
		 */
		builder->xmax = running->nextXid;	/* >= are running
		 *
		 * 大于等于该值的事务仍在运行。
		 */

		/* so we can safely use the faster comparisons
		 *
		 * 因此可以安全地使用更快的比较。
		 */
		Assert(TransactionIdIsNormal(builder->xmin));
		Assert(TransactionIdIsNormal(builder->xmax));

		ereport(LOG,
				(errmsg("logical decoding found initial starting point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("Waiting for transactions (approximately %d) older than %u to end.",
						   running->xcnt, running->nextXid)));

		SnapBuildWaitSnapshot(running, running->nextXid);
	}

	/*
	 * c) transition from BUILDING_SNAPSHOT to FULL_SNAPSHOT.
	 *
	 * c) 从 BUILDING_SNAPSHOT 迁移到 FULL_SNAPSHOT。
	 *
	 * In BUILDING_SNAPSHOT state, and this xl_running_xacts' oldestRunningXid
	 * is >= than nextXid from when we switched to BUILDING_SNAPSHOT.  This
	 * means all transactions starting afterwards have enough information to
	 * be decoded.  Switch to FULL_SNAPSHOT.
	 *
	 * 处于 BUILDING_SNAPSHOT，并且这条 xl_running_xacts 的 oldestRunningXid 大于等于我们切换到 BUILDING_SNAPSHOT 时的 nextXid。这意味着此后开始的事务都有足够信息可以被解码。切换到 FULL_SNAPSHOT。
	 */
	else if (builder->state == SNAPBUILD_BUILDING_SNAPSHOT &&
			 TransactionIdPrecedesOrEquals(builder->next_phase_at,
										   running->oldestRunningXid))
	{
		builder->state = SNAPBUILD_FULL_SNAPSHOT;
		builder->next_phase_at = running->nextXid;

		ereport(LOG,
				(errmsg("logical decoding found initial consistent point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("Waiting for transactions (approximately %d) older than %u to end.",
						   running->xcnt, running->nextXid)));

		SnapBuildWaitSnapshot(running, running->nextXid);
	}

	/*
	 * c) transition from FULL_SNAPSHOT to CONSISTENT.
	 *
	 * c) 从 FULL_SNAPSHOT 迁移到 CONSISTENT。
	 *
	 * In FULL_SNAPSHOT state, and this xl_running_xacts' oldestRunningXid is
	 * >= than nextXid from when we switched to FULL_SNAPSHOT.  This means all
	 * transactions that are currently in progress have a catalog snapshot,
	 * and all their changes have been collected.  Switch to CONSISTENT.
	 *
	 * 处于 FULL_SNAPSHOT，并且这条 xl_running_xacts 的 oldestRunningXid 大于等于我们切换到 FULL_SNAPSHOT 时的 nextXid。这意味着当前进行中的事务都有目录快照，它们的变更也都已收集。切换到 CONSISTENT。
	 */
	else if (builder->state == SNAPBUILD_FULL_SNAPSHOT &&
			 TransactionIdPrecedesOrEquals(builder->next_phase_at,
										   running->oldestRunningXid))
	{
		builder->state = SNAPBUILD_CONSISTENT;
		builder->next_phase_at = InvalidTransactionId;

		ereport(LOG,
				(errmsg("logical decoding found consistent point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("There are no old transactions anymore.")));
	}

	/*
	 * We already started to track running xacts and need to wait for all
	 * in-progress ones to finish. We fall through to the normal processing of
	 * records so incremental cleanup can be performed.
	 *
	 * 我们已经开始跟踪正在运行的事务，需要等待所有进行中的事务结束。然后落到记录的正常处理上，以便做增量清理。
	 */
	return true;
}

/* ---
 * Iterate through xids in record, wait for all older than the cutoff to
 * finish.  Then, if possible, log a new xl_running_xacts record.
 *
 * 遍历记录中的 xid，等待所有比截止点更老的事务结束。然后如果可能，记一条新的 xl_running_xacts 记录。
 *
 * This isn't required for the correctness of decoding, but to:
 * a) allow isolationtester to notice that we're currently waiting for
 *	  something.
 * b) log a new xl_running_xacts record where it'd be helpful, without having
 *	  to wait for bgwriter or checkpointer.
 *
 * 这对解码的正确性不是必需的，目的是：a) 让 isolationtester 注意到我们正在等待某件事；b) 在有帮助的地方记一条新的 xl_running_xacts，而不必等待 bgwriter 或 checkpointer。
 *
 * ---
 */
static void
SnapBuildWaitSnapshot(xl_running_xacts *running, TransactionId cutoff)
{
	int			off;

	for (off = 0; off < running->xcnt; off++)
	{
		TransactionId xid = running->xids[off];

		/*
		 * Upper layers should prevent that we ever need to wait on ourselves.
		 * Check anyway, since failing to do so would either result in an
		 * endless wait or an Assert() failure.
		 *
		 * 上层应当保证我们永远不必等待自己。这里仍然检查一下，否则要么无限等待，要么触发断言失败。
		 */
		if (TransactionIdIsCurrentTransactionId(xid))
			elog(ERROR, "waiting for ourselves");

		if (TransactionIdFollows(xid, cutoff))
			continue;

		XactLockTableWait(xid, NULL, NULL, XLTW_None);
	}

	/*
	 * All transactions we needed to finish finished - try to ensure there is
	 * another xl_running_xacts record in a timely manner, without having to
	 * wait for bgwriter or checkpointer to log one.  During recovery we can't
	 * enforce that, so we'll have to wait.
	 *
	 * 所有需要结束的事务都已结束。尽量及时再产生一条 xl_running_xacts 记录，而不必等待 bgwriter 或 checkpointer 去记。恢复期间无法强制做到这一点，所以只好等待。
	 */
	if (!RecoveryInProgress())
	{
		LogStandbySnapshot();
	}
}

#define SnapBuildOnDiskConstantSize \
	offsetof(SnapBuildOnDisk, builder)
#define SnapBuildOnDiskNotChecksummedSize \
	offsetof(SnapBuildOnDisk, version)

#define SNAPBUILD_MAGIC 0x51A1E001
#define SNAPBUILD_VERSION 6

/*
 * Store/Load a snapshot from disk, depending on the snapshot builder's state.
 *
 * 根据快照构建器的状态，把快照存到磁盘或从磁盘加载。
 *
 * Supposed to be used by external (i.e. not snapbuild.c) code that just read
 * a record that's a potential location for a serialized snapshot.
 *
 * 供 snapbuild.c 以外的代码使用：它们刚刚读到一条记录，而该位置可能适合存放序列化快照。
 */
void
SnapBuildSerializationPoint(SnapBuild *builder, XLogRecPtr lsn)
{
	if (builder->state < SNAPBUILD_CONSISTENT)
		SnapBuildRestore(builder, lsn);
	else
		SnapBuildSerialize(builder, lsn);
}

/*
 * Serialize the snapshot 'builder' at the location 'lsn' if it hasn't already
 * been done by another decoding process.
 *
 * 若还没有别的解码进程做过，就在 lsn 这个位置序列化快照 builder。
 */
static void
SnapBuildSerialize(SnapBuild *builder, XLogRecPtr lsn)
{
	Size		needed_length;
	SnapBuildOnDisk *ondisk = NULL;
	TransactionId *catchange_xip = NULL;
	MemoryContext old_ctx;
	size_t		catchange_xcnt;
	char	   *ondisk_c;
	int			fd;
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	int			ret;
	struct stat stat_buf;
	Size		sz;

	Assert(lsn != InvalidXLogRecPtr);
	Assert(builder->last_serialized_snapshot == InvalidXLogRecPtr ||
		   builder->last_serialized_snapshot <= lsn);

	/*
	 * no point in serializing if we cannot continue to work immediately after
	 * restoring the snapshot
	 *
	 * 如果恢复快照后不能立刻继续工作，序列化就没有意义。
	 */
	if (builder->state < SNAPBUILD_CONSISTENT)
		return;

	/* consistent snapshots have no next phase
	 *
	 * 一致快照没有下一个阶段。
	 */
	Assert(builder->next_phase_at == InvalidTransactionId);

	/*
	 * We identify snapshots by the LSN they are valid for. We don't need to
	 * include timelines in the name as each LSN maps to exactly one timeline
	 * unless the user used pg_resetwal or similar. If a user did so, there's
	 * no hope continuing to decode anyway.
	 *
	 * 用快照有效的那个 LSN 来标识它们。名字里不必包含时间线，因为除非用户用了 pg_resetwal 之类的工具，每个 LSN 只对应一条时间线。如果用户这样做了，继续解码反正也没有希望。
	 */
	sprintf(path, "%s/%X-%X.snap",
			PG_LOGICAL_SNAPSHOTS_DIR,
			LSN_FORMAT_ARGS(lsn));

	/*
	 * first check whether some other backend already has written the snapshot
	 * for this LSN. It's perfectly fine if there's none, so we accept ENOENT
	 * as a valid state. Everything else is an unexpected error.
	 *
	 * 先检查是否已有其他后端为这个 LSN 写过快照。没有也完全正常，因此把 ENOENT 视为有效状态。其他情况都是意外错误。
	 */
	ret = stat(path, &stat_buf);

	if (ret != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));

	else if (ret == 0)
	{
		/*
		 * somebody else has already serialized to this point, don't overwrite
		 * but remember location, so we don't need to read old data again.
		 *
		 * 别人已经序列化到这个点，不要覆盖，但记住这个位置，这样不必再读旧数据。
		 *
		 * To be sure it has been synced to disk after the rename() from the
		 * tempfile filename to the real filename, we just repeat the fsync.
		 * That ought to be cheap because in most scenarios it should already
		 * be safely on disk.
		 *
		 * 为了确认从临时文件名 rename() 到正式文件名之后确实同步到了磁盘，这里再做一次 fsync。这应当很便宜，因为多数情况下它已经安全落盘。
		 */
		fsync_fname(path, false);
		fsync_fname(PG_LOGICAL_SNAPSHOTS_DIR, true);

		builder->last_serialized_snapshot = lsn;
		goto out;
	}

	/*
	 * there is an obvious race condition here between the time we stat(2) the
	 * file and us writing the file. But we rename the file into place
	 * atomically and all files created need to contain the same data anyway,
	 * so this is perfectly fine, although a bit of a resource waste. Locking
	 * seems like pointless complication.
	 *
	 * 这里在 stat(2) 文件和我们写文件之间有一个明显的竞态。但我们是原子地把文件改名到位的，而且所有创建出来的文件都需要包含相同的数据，所以这完全没问题，只是有点浪费资源。加锁看起来是不必要的复杂化。
	 */
	elog(DEBUG1, "serializing snapshot to %s", path);

	/* to make sure only we will write to this tempfile, include pid
	 *
	 * 为了确保只有我们会写这个临时文件，把 pid 包含进去。
	 */
	sprintf(tmppath, "%s/%X-%X.snap.%d.tmp",
			PG_LOGICAL_SNAPSHOTS_DIR,
			LSN_FORMAT_ARGS(lsn), MyProcPid);

	/*
	 * Unlink temporary file if it already exists, needs to have been before a
	 * crash/error since we won't enter this function twice from within a
	 * single decoding slot/backend and the temporary file contains the pid of
	 * the current process.
	 *
	 * 如果临时文件已经存在就删掉它。它只能是崩溃或出错之前留下的，因为同一个解码槽或后端不会两次进入这个函数，而且临时文件名里包含当前进程的 pid。
	 */
	if (unlink(tmppath) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", tmppath)));

	old_ctx = MemoryContextSwitchTo(builder->context);

	/* Get the catalog modifying transactions that are yet not committed
	 *
	 * 取得尚未提交的、修改了目录的事务。
	 */
	catchange_xip = ReorderBufferGetCatalogChangesXacts(builder->reorder);
	catchange_xcnt = dclist_count(&builder->reorder->catchange_txns);

	needed_length = sizeof(SnapBuildOnDisk) +
		sizeof(TransactionId) * (builder->committed.xcnt + catchange_xcnt);

	ondisk_c = palloc0(needed_length);
	ondisk = (SnapBuildOnDisk *) ondisk_c;
	ondisk->magic = SNAPBUILD_MAGIC;
	ondisk->version = SNAPBUILD_VERSION;
	ondisk->length = needed_length;
	INIT_CRC32C(ondisk->checksum);
	COMP_CRC32C(ondisk->checksum,
				((char *) ondisk) + SnapBuildOnDiskNotChecksummedSize,
				SnapBuildOnDiskConstantSize - SnapBuildOnDiskNotChecksummedSize);
	ondisk_c += sizeof(SnapBuildOnDisk);

	memcpy(&ondisk->builder, builder, sizeof(SnapBuild));
	/* NULL-ify memory-only data
	 *
	 * 把仅存在于内存中的数据置空。
	 */
	ondisk->builder.context = NULL;
	ondisk->builder.snapshot = NULL;
	ondisk->builder.reorder = NULL;
	ondisk->builder.committed.xip = NULL;
	ondisk->builder.catchange.xip = NULL;
	/* update catchange only on disk data
	 *
	 * 只更新 catchange 的磁盘数据。
	 */
	ondisk->builder.catchange.xcnt = catchange_xcnt;

	COMP_CRC32C(ondisk->checksum,
				&ondisk->builder,
				sizeof(SnapBuild));

	/* copy committed xacts
	 *
	 * 复制已提交的事务。
	 */
	if (builder->committed.xcnt > 0)
	{
		sz = sizeof(TransactionId) * builder->committed.xcnt;
		memcpy(ondisk_c, builder->committed.xip, sz);
		COMP_CRC32C(ondisk->checksum, ondisk_c, sz);
		ondisk_c += sz;
	}

	/* copy catalog modifying xacts
	 *
	 * 复制修改了目录的事务。
	 */
	if (catchange_xcnt > 0)
	{
		sz = sizeof(TransactionId) * catchange_xcnt;
		memcpy(ondisk_c, catchange_xip, sz);
		COMP_CRC32C(ondisk->checksum, ondisk_c, sz);
		ondisk_c += sz;
	}

	FIN_CRC32C(ondisk->checksum);

	/* we have valid data now, open tempfile and write it there
	 *
	 * 现在数据有效，打开临时文件并写进去。
	 */
	fd = OpenTransientFile(tmppath,
						   O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", tmppath)));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SNAPBUILD_WRITE);
	if ((write(fd, ondisk, needed_length)) != needed_length)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		/* if write didn't set errno, assume problem is no disk space
		 *
		 * 如果写操作没有设置 errno，就假定问题是磁盘空间不足。
		 */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	/*
	 * fsync the file before renaming so that even if we crash after this we
	 * have either a fully valid file or nothing.
	 *
	 * 改名之前先 fsync 文件，这样即使此后崩溃，要么留下一份完全有效的文件，要么什么都没有。
	 *
	 * It's safe to just ERROR on fsync() here because we'll retry the whole
	 * operation including the writes.
	 *
	 * 在这里对 fsync() 直接 ERROR 是安全的，因为整个操作包括写入都会重试。
	 *
	 * TODO: Do the fsync() via checkpoints/restartpoints, doing it here has
	 * some noticeable overhead since it's performed synchronously during
	 * decoding?
	 *
	 * 待办：是否改由检查点或重启点来做 fsync()？在这里做会有可察觉的开销，因为解码期间是同步执行的。
	 */
	pgstat_report_wait_start(WAIT_EVENT_SNAPBUILD_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	fsync_fname(PG_LOGICAL_SNAPSHOTS_DIR, true);

	/*
	 * We may overwrite the work from some other backend, but that's ok, our
	 * snapshot is valid as well, we'll just have done some superfluous work.
	 *
	 * 我们可能会覆盖其他后端的成果，这没关系，我们的快照同样有效，只是多做了一些多余的工作。
	 */
	if (rename(tmppath, path) != 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));
	}

	/* make sure we persist
	 *
	 * 确保数据被持久化。
	 */
	fsync_fname(path, false);
	fsync_fname(PG_LOGICAL_SNAPSHOTS_DIR, true);

	/*
	 * Now there's no way we can lose the dumped state anymore, remember this
	 * as a serialization point.
	 *
	 * 现在转储出去的状态不可能再丢失了，把这里记为一个序列化点。
	 */
	builder->last_serialized_snapshot = lsn;

	MemoryContextSwitchTo(old_ctx);

out:
	ReorderBufferSetRestartPoint(builder->reorder,
								 builder->last_serialized_snapshot);
	/* be tidy
	 *
	 * 保持整洁。
	 */
	if (ondisk)
		pfree(ondisk);
	if (catchange_xip)
		pfree(catchange_xip);
}

/*
 * Restore the logical snapshot file contents to 'ondisk'.
 *
 * 把逻辑快照文件的内容恢复到 ondisk。
 *
 * 'context' is the memory context where the catalog modifying/committed xid
 * will live.
 * If 'missing_ok' is true, will not throw an error if the file is not found.
 *
 * context 是存放修改目录或已提交 xid 的内存上下文。若 missing_ok 为真，找不到文件时不报错。
 */
bool
SnapBuildRestoreSnapshot(SnapBuildOnDisk *ondisk, XLogRecPtr lsn,
						 MemoryContext context, bool missing_ok)
{
	int			fd;
	pg_crc32c	checksum;
	Size		sz;
	char		path[MAXPGPATH];

	sprintf(path, "%s/%X-%X.snap",
			PG_LOGICAL_SNAPSHOTS_DIR,
			LSN_FORMAT_ARGS(lsn));

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		if (missing_ok && errno == ENOENT)
			return false;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

	/* ----
	 * Make sure the snapshot had been stored safely to disk, that's normally
	 * cheap.
	 * Note that we do not need PANIC here, nobody will be able to use the
	 * slot without fsyncing, and saving it won't succeed without an fsync()
	 * either...
	 *
	 * 确认快照已经安全存到磁盘，这通常很便宜。注意这里不需要 PANIC：没有 fsync，没人能使用这个槽，保存它时如果没有 fsync() 也不会成功。
	 *
	 * ----
	 */
	fsync_fname(path, false);
	fsync_fname(PG_LOGICAL_SNAPSHOTS_DIR, true);

	/* read statically sized portion of snapshot
	 *
	 * 读取快照中大小固定的那一部分。
	 */
	SnapBuildRestoreContents(fd, ondisk, SnapBuildOnDiskConstantSize, path);

	if (ondisk->magic != SNAPBUILD_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("snapbuild state file \"%s\" has wrong magic number: %u instead of %u",
						path, ondisk->magic, SNAPBUILD_MAGIC)));

	if (ondisk->version != SNAPBUILD_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("snapbuild state file \"%s\" has unsupported version: %u instead of %u",
						path, ondisk->version, SNAPBUILD_VERSION)));

	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				((char *) ondisk) + SnapBuildOnDiskNotChecksummedSize,
				SnapBuildOnDiskConstantSize - SnapBuildOnDiskNotChecksummedSize);

	/* read SnapBuild
	 *
	 * 读取 SnapBuild。
	 */
	SnapBuildRestoreContents(fd, &ondisk->builder, sizeof(SnapBuild), path);
	COMP_CRC32C(checksum, &ondisk->builder, sizeof(SnapBuild));

	/* restore committed xacts information
	 *
	 * 恢复已提交事务的信息。
	 */
	if (ondisk->builder.committed.xcnt > 0)
	{
		sz = sizeof(TransactionId) * ondisk->builder.committed.xcnt;
		ondisk->builder.committed.xip = MemoryContextAllocZero(context, sz);
		SnapBuildRestoreContents(fd, ondisk->builder.committed.xip, sz, path);
		COMP_CRC32C(checksum, ondisk->builder.committed.xip, sz);
	}

	/* restore catalog modifying xacts information
	 *
	 * 恢复修改目录的事务的信息。
	 */
	if (ondisk->builder.catchange.xcnt > 0)
	{
		sz = sizeof(TransactionId) * ondisk->builder.catchange.xcnt;
		ondisk->builder.catchange.xip = MemoryContextAllocZero(context, sz);
		SnapBuildRestoreContents(fd, ondisk->builder.catchange.xip, sz, path);
		COMP_CRC32C(checksum, ondisk->builder.catchange.xip, sz);
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	FIN_CRC32C(checksum);

	/* verify checksum of what we've read
	 *
	 * 校验刚刚读到的内容的校验和。
	 */
	if (!EQ_CRC32C(checksum, ondisk->checksum))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("checksum mismatch for snapbuild state file \"%s\": is %u, should be %u",
						path, checksum, ondisk->checksum)));

	return true;
}

/*
 * Restore a snapshot into 'builder' if previously one has been stored at the
 * location indicated by 'lsn'. Returns true if successful, false otherwise.
 *
 * 如果先前在 lsn 指示的位置存过快照，就把它恢复进 builder。成功返回真，否则返回假。
 */
static bool
SnapBuildRestore(SnapBuild *builder, XLogRecPtr lsn)
{
	SnapBuildOnDisk ondisk;

	/* no point in loading a snapshot if we're already there
	 *
	 * 如果已经在那个位置，就没有必要再加载快照。
	 */
	if (builder->state == SNAPBUILD_CONSISTENT)
		return false;

	/* validate and restore the snapshot to 'ondisk'
	 *
	 * 校验并把快照恢复到 ondisk。
	 */
	if (!SnapBuildRestoreSnapshot(&ondisk, lsn, builder->context, true))
		return false;

	/*
	 * ok, we now have a sensible snapshot here, figure out if it has more
	 * information than we have.
	 *
	 * 现在这里有一份合理的快照，判断它是否比我们已有的信息更多。
	 */

	/*
	 * We are only interested in consistent snapshots for now, comparing
	 * whether one incomplete snapshot is more "advanced" seems to be
	 * unnecessarily complex.
	 *
	 * 目前只对一致快照感兴趣。比较两份不完整快照哪一份更靠前，似乎没有必要那么复杂。
	 */
	if (ondisk.builder.state < SNAPBUILD_CONSISTENT)
		goto snapshot_not_interesting;

	/*
	 * Don't use a snapshot that requires an xmin that we cannot guarantee to
	 * be available.
	 *
	 * 不要使用一份所要求的 xmin 我们无法保证仍然可用的快照。
	 */
	if (TransactionIdPrecedes(ondisk.builder.xmin, builder->initial_xmin_horizon))
		goto snapshot_not_interesting;

	/*
	 * Consistent snapshots have no next phase. Reset next_phase_at as it is
	 * possible that an old value may remain.
	 *
	 * 一致快照没有下一个阶段。把 next_phase_at 复位，因为可能残留旧值。
	 */
	Assert(ondisk.builder.next_phase_at == InvalidTransactionId);
	builder->next_phase_at = InvalidTransactionId;

	/* ok, we think the snapshot is sensible, copy over everything important
	 *
	 * 我们认为这份快照是合理的，把所有重要内容复制过来。
	 */
	builder->xmin = ondisk.builder.xmin;
	builder->xmax = ondisk.builder.xmax;
	builder->state = ondisk.builder.state;

	builder->committed.xcnt = ondisk.builder.committed.xcnt;
	/* We only allocated/stored xcnt, not xcnt_space xids !
	 *
	 * 我们只分配并保存了 xcnt 个 xid，不是 xcnt_space 个。
	 */
	/* don't overwrite preallocated xip, if we don't have anything here
	 *
	 * 如果这里没有任何内容，就不要覆盖预先分配的 xip。
	 */
	if (builder->committed.xcnt > 0)
	{
		pfree(builder->committed.xip);
		builder->committed.xcnt_space = ondisk.builder.committed.xcnt;
		builder->committed.xip = ondisk.builder.committed.xip;
	}
	ondisk.builder.committed.xip = NULL;

	/* set catalog modifying transactions
	 *
	 * 设置修改目录的事务。
	 */
	if (builder->catchange.xip)
		pfree(builder->catchange.xip);
	builder->catchange.xcnt = ondisk.builder.catchange.xcnt;
	builder->catchange.xip = ondisk.builder.catchange.xip;
	ondisk.builder.catchange.xip = NULL;

	/* our snapshot is not interesting anymore, build a new one
	 *
	 * 我们的快照已经不再有用，重新建一份。
	 */
	if (builder->snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(builder->snapshot);
	}
	builder->snapshot = SnapBuildBuildSnapshot(builder);
	SnapBuildSnapIncRefcount(builder->snapshot);

	ReorderBufferSetRestartPoint(builder->reorder, lsn);

	Assert(builder->state == SNAPBUILD_CONSISTENT);

	ereport(LOG,
			(errmsg("logical decoding found consistent point at %X/%X",
					LSN_FORMAT_ARGS(lsn)),
			 errdetail("Logical decoding will begin using saved snapshot.")));
	return true;

snapshot_not_interesting:
	if (ondisk.builder.committed.xip != NULL)
		pfree(ondisk.builder.committed.xip);
	if (ondisk.builder.catchange.xip != NULL)
		pfree(ondisk.builder.catchange.xip);
	return false;
}

/*
 * Read the contents of the serialized snapshot to 'dest'.
 *
 * 把序列化快照的内容读到 dest。
 */
static void
SnapBuildRestoreContents(int fd, void *dest, Size size, const char *path)
{
	int			readBytes;

	pgstat_report_wait_start(WAIT_EVENT_SNAPBUILD_READ);
	readBytes = read(fd, dest, size);
	pgstat_report_wait_end();
	if (readBytes != size)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		if (readBytes < 0)
		{
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, size)));
	}
}

/*
 * Remove all serialized snapshots that are not required anymore because no
 * slot can need them. This doesn't actually have to run during a checkpoint,
 * but it's a convenient point to schedule this.
 *
 * 删除所有不再需要的序列化快照，因为没有任何槽还会用到它们。这并不一定要在检查点期间运行，但检查点是安排这件事的方便时机。
 *
 * NB: We run this during checkpoints even if logical decoding is disabled so
 * we cleanup old slots at some point after it got disabled.
 *
 * 注意：即使逻辑解码已关闭，我们仍在检查点期间运行它，以便在关闭之后的某个时刻清理旧槽。
 */
void
CheckPointSnapBuild(void)
{
	XLogRecPtr	cutoff;
	XLogRecPtr	redo;
	DIR		   *snap_dir;
	struct dirent *snap_de;
	char		path[MAXPGPATH + sizeof(PG_LOGICAL_SNAPSHOTS_DIR)];

	/*
	 * We start off with a minimum of the last redo pointer. No new
	 * replication slot will start before that, so that's a safe upper bound
	 * for removal.
	 *
	 * 起点取最后一次重做指针中的较小者。新的复制槽不会在此之前开始，所以这是删除时安全的上界。
	 */
	redo = GetRedoRecPtr();

	/* now check for the restart ptrs from existing slots
	 *
	 * 现在检查现有槽的重启指针。
	 */
	cutoff = ReplicationSlotsComputeLogicalRestartLSN();

	/* don't start earlier than the restart lsn
	 *
	 * 不要早于 restart_lsn 开始。
	 */
	if (redo < cutoff)
		cutoff = redo;

	snap_dir = AllocateDir(PG_LOGICAL_SNAPSHOTS_DIR);
	while ((snap_de = ReadDir(snap_dir, PG_LOGICAL_SNAPSHOTS_DIR)) != NULL)
	{
		uint32		hi;
		uint32		lo;
		XLogRecPtr	lsn;
		PGFileType	de_type;

		if (strcmp(snap_de->d_name, ".") == 0 ||
			strcmp(snap_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", PG_LOGICAL_SNAPSHOTS_DIR, snap_de->d_name);
		de_type = get_dirent_type(path, snap_de, false, DEBUG1);

		if (de_type != PGFILETYPE_ERROR && de_type != PGFILETYPE_REG)
		{
			elog(DEBUG1, "only regular files expected: %s", path);
			continue;
		}

		/*
		 * temporary filenames from SnapBuildSerialize() include the LSN and
		 * everything but are postfixed by .$pid.tmp. We can just remove them
		 * the same as other files because there can be none that are
		 * currently being written that are older than cutoff.
		 *
		 * SnapBuildSerialize() 的临时文件名包含 LSN 等内容，并以 .pid.tmp 结尾。可以像其他文件一样删除它们，因为不可能有正在写入、且比截止点更旧的临时文件。
		 *
		 * We just log a message if a file doesn't fit the pattern, it's
		 * probably some editors lock/state file or similar...
		 *
		 * 如果文件名不符合这个模式，只记一条消息，它多半是编辑器的锁文件或状态文件之类。
		 */
		if (sscanf(snap_de->d_name, "%X-%X.snap", &hi, &lo) != 2)
		{
			ereport(LOG,
					(errmsg("could not parse file name \"%s\"", path)));
			continue;
		}

		lsn = ((uint64) hi) << 32 | lo;

		/* check whether we still need it
		 *
		 * 检查是否仍然需要它。
		 */
		if (lsn < cutoff || cutoff == InvalidXLogRecPtr)
		{
			elog(DEBUG1, "removing snapbuild snapshot %s", path);

			/*
			 * It's not particularly harmful, though strange, if we can't
			 * remove the file here. Don't prevent the checkpoint from
			 * completing, that'd be a cure worse than the disease.
			 *
			 * 如果在这里删不掉文件，虽然奇怪，但危害并不特别大。不要因此阻止检查点完成，那会是比问题本身更糟的办法。
			 */
			if (unlink(path) < 0)
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m",
								path)));
				continue;
			}
		}
	}
	FreeDir(snap_dir);
}

/*
 * Check if a logical snapshot at the specified point has been serialized.
 *
 * 检查指定位置的逻辑快照是否已经序列化。
 */
bool
SnapBuildSnapshotExists(XLogRecPtr lsn)
{
	char		path[MAXPGPATH];
	int			ret;
	struct stat stat_buf;

	sprintf(path, "%s/%X-%X.snap",
			PG_LOGICAL_SNAPSHOTS_DIR,
			LSN_FORMAT_ARGS(lsn));

	ret = stat(path, &stat_buf);

	if (ret != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));

	return ret == 0;
}
