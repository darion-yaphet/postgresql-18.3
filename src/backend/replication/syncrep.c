/*-------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * Synchronous replication is new as of PostgreSQL 9.1.
 *
 * 同步复制自 PostgreSQL 9.1 起引入。
 *
 * If requested, transaction commits wait until their commit LSN are
 * acknowledged by the synchronous standbys.
 *
 * 若用户要求同步复制，事务提交会等到同步备库确认其提交 LSN。
 *
 * This module contains the code for waiting and release of backends.
 * All code in this module executes on the primary. The core streaming
 * replication transport remains within WALreceiver/WALsender modules.
 *
 * 本模块包含后端等待与放行的代码，全部在主库上执行。流复制传输本身仍在 walreceiver 和 walsender 模块中。
 *
 * The essence of this design is that it isolates all logic about
 * waiting/releasing onto the primary. The primary defines which standbys
 * it wishes to wait for. The standbys are completely unaware of the
 * durability requirements of transactions on the primary, reducing the
 * complexity of the code and streamlining both standby operations and
 * network bandwidth because there is no requirement to ship
 * per-transaction state information.
 *
 * 这个设计把等待和放行的逻辑都放在主库。主库决定要等哪些备库。
 * 备库完全不知道主库事务的持久性要求，从而简化代码，也减轻备库操作和网络带宽，因为不必传送每个事务的状态。
 *
 * Replication is either synchronous or not synchronous (async). If it is
 * async, we just fastpath out of here. If it is sync, then we wait for
 * the write, flush or apply location on the standby before releasing
 * the waiting backend. Further complexity in that interaction is
 * expected in later releases.
 *
 * 复制要么同步，要么异步。异步时直接快速返回。
 * 同步时，要等到备库的写入、刷盘或应用位置后再放行等待中的后端。这种交互以后还可能更复杂。
 *
 * The best performing way to manage the waiting backends is to have a
 * single ordered queue of waiting backends, so that we can avoid
 * searching the through all waiters each time we receive a reply.
 *
 * 管理等待后端的高效做法是维护一条有序队列，这样每次收到回复时不必遍历全部等待者。
 *
 * In 9.5 or before only a single standby could be considered as
 * synchronous. In 9.6 we support a priority-based multiple synchronous
 * standbys. In 10.0 a quorum-based multiple synchronous standbys is also
 * supported. The number of synchronous standbys that transactions
 * must wait for replies from is specified in synchronous_standby_names.
 * This parameter also specifies a list of standby names and the method
 * (FIRST and ANY) to choose synchronous standbys from the listed ones.
 *
 * 9.5 及更早只能有一个同步备库。9.6 支持按优先级的多个同步备库。10.0 起还支持基于 quorum 的多个同步备库。
 * 事务必须等待回复的同步备库个数由 synchronous_standby_names 指定。该参数还给出备库名单，以及从中选择的方法 FIRST 或 ANY。
 *
 * The method FIRST specifies a priority-based synchronous replication
 * and makes transaction commits wait until their WAL records are
 * replicated to the requested number of synchronous standbys chosen based
 * on their priorities. The standbys whose names appear earlier in the list
 * are given higher priority and will be considered as synchronous.
 * Other standby servers appearing later in this list represent potential
 * synchronous standbys. If any of the current synchronous standbys
 * disconnects for whatever reason, it will be replaced immediately with
 * the next-highest-priority standby.
 *
 * 方法 FIRST 表示按优先级的同步复制。事务提交会等到 WAL 已复制到按优先级选出的指定个数的同步备库。
 * 名单中靠前的备库优先级更高，会被当作同步备库。靠后的是潜在同步备库。
 * 当前同步备库无论因何断开，都会立刻由下一优先级的备库顶上。
 *
 * The method ANY specifies a quorum-based synchronous replication
 * and makes transaction commits wait until their WAL records are
 * replicated to at least the requested number of synchronous standbys
 * in the list. All the standbys appearing in the list are considered as
 * candidates for quorum synchronous standbys.
 *
 * 方法 ANY 表示基于 quorum 的同步复制。事务提交会等到 WAL 至少复制到名单中指定个数的同步备库。名单里的备库都是 quorum 候选。
 *
 * If neither FIRST nor ANY is specified, FIRST is used as the method.
 * This is for backward compatibility with 9.6 or before where only a
 * priority-based sync replication was supported.
 *
 * 若既没有指定 FIRST 也没有指定 ANY，则使用 FIRST。这是为了兼容 9.6 及更早只支持优先级同步复制的行为。
 *
 * Before the standbys chosen from synchronous_standby_names can
 * become the synchronous standbys they must have caught up with
 * the primary; that may take some time. Once caught up,
 * the standbys which are considered as synchronous at that moment
 * will release waiters from the queue.
 *
 * 从 synchronous_standby_names 选出的备库要先追上主库，才能成为同步备库，这可能要一些时间。追上之后，当时被视为同步的备库会把队列里的等待者放行。
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "common/int.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc_hooks.h"
#include "utils/ps_status.h"

/* User-settable parameters for sync rep
 *
 * 同步复制可由用户设置的参数。
 */
char	   *SyncRepStandbyNames;

#define SyncStandbysDefined() \
	(SyncRepStandbyNames != NULL && SyncRepStandbyNames[0] != '\0')

static bool announce_next_takeover = true;

SyncRepConfigData *SyncRepConfig = NULL;
static int	SyncRepWaitMode = SYNC_REP_NO_WAIT;

static void SyncRepQueueInsert(int mode);
static void SyncRepCancelWait(void);
static int	SyncRepWakeQueue(bool all, int mode);

static bool SyncRepGetSyncRecPtr(XLogRecPtr *writePtr,
								 XLogRecPtr *flushPtr,
								 XLogRecPtr *applyPtr,
								 bool *am_sync);
static void SyncRepGetOldestSyncRecPtr(XLogRecPtr *writePtr,
									   XLogRecPtr *flushPtr,
									   XLogRecPtr *applyPtr,
									   SyncRepStandbyData *sync_standbys,
									   int num_standbys);
static void SyncRepGetNthLatestSyncRecPtr(XLogRecPtr *writePtr,
										  XLogRecPtr *flushPtr,
										  XLogRecPtr *applyPtr,
										  SyncRepStandbyData *sync_standbys,
										  int num_standbys,
										  uint8 nth);
static int	SyncRepGetStandbyPriority(void);
static int	standby_priority_comparator(const void *a, const void *b);
static int	cmp_lsn(const void *a, const void *b);

#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(int mode);
#endif

/*
 * 核心流程：提交时 SyncRepWaitForLSN 按 synchronous_standby_names 入队等待；walsender 在 SyncRepReleaseWaiters 里按 FIRST 或 ANY 计算同步位置并唤醒等待者。
 */

/*
 * ===========================================================
 * Synchronous Replication functions for normal user backends
 * ===========================================================
 *
 * 普通用户后端使用的同步复制函数。
 */

/*
 * Wait for synchronous replication, if requested by user.
 *
 * 若用户要求，则等待同步复制。
 *
 * Initially backends start in state SYNC_REP_NOT_WAITING and then
 * change that state to SYNC_REP_WAITING before adding ourselves
 * to the wait queue. During SyncRepWakeQueue() a WALSender changes
 * the state to SYNC_REP_WAIT_COMPLETE once replication is confirmed.
 * This backend then resets its state to SYNC_REP_NOT_WAITING.
 *
 * 后端最初处于 SYNC_REP_NOT_WAITING，加入等待队列前改为 SYNC_REP_WAITING。
 * 复制得到确认后，walsender 在 SyncRepWakeQueue() 里把状态改为 SYNC_REP_WAIT_COMPLETE。本后端再把状态重置为 SYNC_REP_NOT_WAITING。
 *
 * 'lsn' represents the LSN to wait for.  'commit' indicates whether this LSN
 * represents a commit record.  If it doesn't, then we wait only for the WAL
 * to be flushed if synchronous_commit is set to the higher level of
 * remote_apply, because only commit records provide apply feedback.
 *
 * lsn 是要等待的 LSN。commit 表示这个 LSN 是否为提交记录。
 * 若不是，且 synchronous_commit 设到更高的 remote_apply，则只等到 WAL 刷盘，因为只有提交记录才提供应用反馈。
 */
void
SyncRepWaitForLSN(XLogRecPtr lsn, bool commit)
{
	int			mode;

	/*
	 * This should be called while holding interrupts during a transaction
	 * commit to prevent the follow-up shared memory queue cleanups to be
	 * influenced by external interruptions.
	 *
	 * 应在事务提交期间持有中断屏蔽时调用，以免随后的共享内存队列清理被外部中断影响。
	 */
	Assert(InterruptHoldoffCount > 0);

	/*
	 * Fast exit if user has not requested sync replication, or there are no
	 * sync replication standby names defined.
	 *
	 * 若用户没有要求同步复制，或没有定义同步备库名，则快速返回。
	 *
	 * Since this routine gets called every commit time, it's important to
	 * exit quickly if sync replication is not requested.
	 *
	 * 本函数每次提交都会调用，因此未要求同步复制时必须尽快返回。
	 *
	 * We check WalSndCtl->sync_standbys_status flag without the lock and exit
	 * immediately if SYNC_STANDBY_INIT is set (the checkpointer has
	 * initialized this data) but SYNC_STANDBY_DEFINED is missing (no sync
	 * replication requested).
	 *
	 * 不加锁检查 WalSndCtl 的 sync_standbys_status。若已设置 SYNC_STANDBY_INIT（checkpointer 已初始化该数据）但没有 SYNC_STANDBY_DEFINED（未要求同步复制），则立刻返回。
	 *
	 * If SYNC_STANDBY_DEFINED is set, we need to check the status again later
	 * while holding the lock, to check the flag and operate the sync rep
	 * queue atomically.  This is necessary to avoid the race condition
	 * described in SyncRepUpdateSyncStandbysDefined().  On the other hand, if
	 * SYNC_STANDBY_DEFINED is not set, the lock is not necessary because we
	 * don't touch the queue.
	 *
	 * 若设置了 SYNC_STANDBY_DEFINED，稍后必须持锁再查一次状态，以便原子地检查标志并操作同步复制队列。
	 * 这是为了避免 SyncRepUpdateSyncStandbysDefined() 里描述的竞争。若没有设置该标志，我们不碰队列，也就不必加锁。
	 */
	if (!SyncRepRequested() ||
		((((volatile WalSndCtlData *) WalSndCtl)->sync_standbys_status) &
		 (SYNC_STANDBY_INIT | SYNC_STANDBY_DEFINED)) == SYNC_STANDBY_INIT)
		return;

	/* Cap the level for anything other than commit to remote flush only.
	 *
	 * 除提交之外，等待级别最高只到远端刷盘。
	 */
	if (commit)
		mode = SyncRepWaitMode;
	else
		mode = Min(SyncRepWaitMode, SYNC_REP_WAIT_FLUSH);

	Assert(dlist_node_is_detached(&MyProc->syncRepLinks));
	Assert(WalSndCtl != NULL);

	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	Assert(MyProc->syncRepState == SYNC_REP_NOT_WAITING);

	/*
	 * We don't wait for sync rep if SYNC_STANDBY_DEFINED is not set.  See
	 * SyncRepUpdateSyncStandbysDefined().
	 *
	 * 未设置 SYNC_STANDBY_DEFINED 时不等待同步复制。见 SyncRepUpdateSyncStandbysDefined()。
	 *
	 * Also check that the standby hasn't already replied. Unlikely race
	 * condition but we'll be fetching that cache line anyway so it's likely
	 * to be a low cost check.
	 *
	 * 同时检查备库是否已经回复。这种竞争很少见，但反正要读那条缓存线，检查的代价很低。
	 *
	 * If the sync standby data has not been initialized yet
	 * (SYNC_STANDBY_INIT is not set), fall back to a check based on the LSN,
	 * then do a direct GUC check.
	 *
	 * 若同步备库数据尚未初始化（没有 SYNC_STANDBY_INIT），则退回到按 LSN 检查，然后再直接检查 GUC。
	 */
	if (WalSndCtl->sync_standbys_status & SYNC_STANDBY_INIT)
	{
		if ((WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) == 0 ||
			lsn <= WalSndCtl->lsn[mode])
		{
			LWLockRelease(SyncRepLock);
			return;
		}
	}
	else if (lsn <= WalSndCtl->lsn[mode])
	{
		/*
		 * The LSN is older than what we need to wait for.  The sync standby
		 * data has not been initialized yet, but we are OK to not wait
		 * because we know that there is no point in doing so based on the
		 * LSN.
		 *
		 * 这个 LSN 比需要等待的位置更老。同步备库数据尚未初始化，但根据 LSN 可以判断没有等待的必要。
		 */
		LWLockRelease(SyncRepLock);
		return;
	}
	else if (!SyncStandbysDefined())
	{
		/*
		 * If we are here, the sync standby data has not been initialized yet,
		 * and the LSN is newer than what need to wait for, so we have fallen
		 * back to the best thing we could do in this case: a check on
		 * SyncStandbysDefined() to see if the GUC is set or not.
		 *
		 * 走到这里说明同步备库数据尚未初始化，而且 LSN 新于需要等待的位置，因此退而检查 SyncStandbysDefined()，看 GUC 是否已设置。
		 *
		 * When the GUC has a value, we wait until the checkpointer updates
		 * the status data because we cannot be sure yet if we should wait or
		 * not. Here, the GUC has *no* value, we are sure that there is no
		 * point to wait; this matters for example when initializing a
		 * cluster, where we should never wait, and no sync standbys is the
		 * default behavior.
		 *
		 * GUC 有值时，要等到 checkpointer 更新状态数据，因为还不能确定该不该等。
		 * 这里 GUC 没有值，可以确定不必等待。初始化集群时尤其如此：默认没有同步备库，永远不该等。
		 */
		LWLockRelease(SyncRepLock);
		return;
	}

	/*
	 * Set our waitLSN so WALSender will know when to wake us, and add
	 * ourselves to the queue.
	 *
	 * 设置 waitLSN，让 walsender 知道何时唤醒我们，并把自己加入队列。
	 */
	MyProc->waitLSN = lsn;
	MyProc->syncRepState = SYNC_REP_WAITING;
	SyncRepQueueInsert(mode);
	Assert(SyncRepQueueIsOrderedByLSN(mode));
	LWLockRelease(SyncRepLock);

	/* Alter ps display to show waiting for sync rep.
	 *
	 * 修改进程状态显示，表明正在等待同步复制。
	 */
	if (update_process_title)
	{
		char		buffer[32];

		sprintf(buffer, "waiting for %X/%X", LSN_FORMAT_ARGS(lsn));
		set_ps_display_suffix(buffer);
	}

	/*
	 * Wait for specified LSN to be confirmed.
	 *
	 * 等待指定的 LSN 被确认。
	 *
	 * Each proc has its own wait latch, so we perform a normal latch
	 * check/wait loop here.
	 *
	 * 每个进程有自己的等待闩锁，这里做普通的闩锁检查与等待循环。
	 */
	for (;;)
	{
		int			rc;

		/* Must reset the latch before testing state.
		 *
		 * 检查状态之前必须先重置闩锁。
		 */
		ResetLatch(MyLatch);

		/*
		 * Acquiring the lock is not needed, the latch ensures proper
		 * barriers. If it looks like we're done, we must really be done,
		 * because once walsender changes the state to SYNC_REP_WAIT_COMPLETE,
		 * it will never update it again, so we can't be seeing a stale value
		 * in that case.
		 *
		 * 不必加锁，闩锁已经提供了正确的内存屏障。若看起来已经完成，那就确实完成了：
		 * walsender 一旦把状态改成 SYNC_REP_WAIT_COMPLETE，就不会再改，因此不会看到过期值。
		 */
		if (MyProc->syncRepState == SYNC_REP_WAIT_COMPLETE)
			break;

		/*
		 * If a wait for synchronous replication is pending, we can neither
		 * acknowledge the commit nor raise ERROR or FATAL.  The latter would
		 * lead the client to believe that the transaction aborted, which is
		 * not true: it's already committed locally. The former is no good
		 * either: the client has requested synchronous replication, and is
		 * entitled to assume that an acknowledged commit is also replicated,
		 * which might not be true. So in this case we issue a WARNING (which
		 * some clients may be able to interpret) and shut off further output.
		 * We do NOT reset ProcDiePending, so that the process will die after
		 * the commit is cleaned up.
		 *
		 * 同步复制等待尚未结束时，既不能确认提交，也不能抛 ERROR 或 FATAL。
		 * 后者会让客户端以为事务已中止，但事务其实已在本地提交。前者也不行：客户端要求了同步复制，有权认为已确认的提交也已复制，而这可能并不成立。
		 * 因此这里发出 WARNING（有的客户端能识别），并停止继续输出。不清 ProcDiePending，以便提交清理之后进程再退出。
		 */
		if (ProcDiePending)
		{
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("canceling the wait for synchronous replication and terminating connection due to administrator command"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby.")));
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}

		/*
		 * It's unclear what to do if a query cancel interrupt arrives.  We
		 * can't actually abort at this point, but ignoring the interrupt
		 * altogether is not helpful, so we just terminate the wait with a
		 * suitable warning.
		 *
		 * 查询取消中断到来时该怎么做并不明确。此时不能真正中止，但完全忽略也没有帮助，因此用一条适当的警告结束等待。
		 */
		if (QueryCancelPending)
		{
			QueryCancelPending = false;
			ereport(WARNING,
					(errmsg("canceling wait for synchronous replication due to user request"),
					 errdetail("The transaction has already committed locally, but might not have been replicated to the standby.")));
			SyncRepCancelWait();
			break;
		}

		/*
		 * Wait on latch.  Any condition that should wake us up will set the
		 * latch, so no need for timeout.
		 *
		 * 在闩锁上等待。任何该唤醒我们的条件都会设置闩锁，因此不需要超时。
		 */
		rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1,
					   WAIT_EVENT_SYNC_REP);

		/*
		 * If the postmaster dies, we'll probably never get an acknowledgment,
		 * because all the wal sender processes will exit. So just bail out.
		 *
		 * 若 postmaster 已死，多半永远等不到确认，因为所有 walsender 都会退出。因此直接放弃。
		 */
		if (rc & WL_POSTMASTER_DEATH)
		{
			ProcDiePending = true;
			whereToSendOutput = DestNone;
			SyncRepCancelWait();
			break;
		}
	}

	/*
	 * WalSender has checked our LSN and has removed us from queue. Clean up
	 * state and leave.  It's OK to reset these shared memory fields without
	 * holding SyncRepLock, because any walsenders will ignore us anyway when
	 * we're not on the queue.  We need a read barrier to make sure we see the
	 * changes to the queue link (this might be unnecessary without
	 * assertions, but better safe than sorry).
	 *
	 * walsender 已检查我们的 LSN 并把我们移出队列。清理状态后离开。
	 * 不持有 SyncRepLock 也可以重置这些共享内存字段，因为不在队列上时 walsender 会忽略我们。
	 * 需要读屏障才能看到队列链接的变化；没有断言时也许不必，但保险起见还是加上。
	 */
	pg_read_barrier();
	Assert(dlist_node_is_detached(&MyProc->syncRepLinks));
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	MyProc->waitLSN = 0;

	/* reset ps display to remove the suffix
	 *
	 * 重置进程状态显示，去掉后缀。
	 */
	if (update_process_title)
		set_ps_display_remove_suffix();
}

/*
 * Insert MyProc into the specified SyncRepQueue, maintaining sorted invariant.
 *
 * 把 MyProc 插入指定的 SyncRepQueue，并保持有序。
 *
 * Usually we will go at tail of queue, though it's possible that we arrive
 * here out of order, so start at tail and work back to insertion point.
 *
 * 通常插在队尾，但也可能乱序到达，因此从队尾往前找到插入点。
 */
static void
SyncRepQueueInsert(int mode)
{
	dlist_head *queue;
	dlist_iter	iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	queue = &WalSndCtl->SyncRepQueue[mode];

	dlist_reverse_foreach(iter, queue)
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * Stop at the queue element that we should insert after to ensure the
		 * queue is ordered by LSN.
		 *
		 * 停在应当插在其后的那个队列元素上，以保证队列按 LSN 有序。
		 */
		if (proc->waitLSN < MyProc->waitLSN)
		{
			dlist_insert_after(&proc->syncRepLinks, &MyProc->syncRepLinks);
			return;
		}
	}

	/*
	 * If we get here, the list was either empty, or this process needs to be
	 * at the head.
	 *
	 * 走到这里说明队列是空的，或者本进程应当排在队头。
	 */
	dlist_push_head(queue, &MyProc->syncRepLinks);
}

/*
 * Acquire SyncRepLock and cancel any wait currently in progress.
 *
 * 获取 SyncRepLock，并取消当前正在进行的等待。
 */
static void
SyncRepCancelWait(void)
{
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
	if (!dlist_node_is_detached(&MyProc->syncRepLinks))
		dlist_delete_thoroughly(&MyProc->syncRepLinks);
	MyProc->syncRepState = SYNC_REP_NOT_WAITING;
	LWLockRelease(SyncRepLock);
}

/*
 * 进程退出时，从同步复制等待队列中摘除本进程。
 */
void
SyncRepCleanupAtProcExit(void)
{
	/*
	 * First check if we are removed from the queue without the lock to not
	 * slow down backend exit.
	 *
	 * 先不加锁检查自己是否已离开队列，以免拖慢后端退出。
	 */
	if (!dlist_node_is_detached(&MyProc->syncRepLinks))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/* maybe we have just been removed, so recheck
		 *
		 * 可能刚刚被移出，因此再查一次。
		 */
		if (!dlist_node_is_detached(&MyProc->syncRepLinks))
			dlist_delete_thoroughly(&MyProc->syncRepLinks);

		LWLockRelease(SyncRepLock);
	}
}

/*
 * ===========================================================
 * Synchronous Replication functions for wal sender processes
 * ===========================================================
 *
 * walsender 进程使用的同步复制函数。
 */

/*
 * Take any action required to initialise sync rep state from config
 * data. Called at WALSender startup and after each SIGHUP.
 *
 * 根据配置数据初始化同步复制状态。在 walsender 启动时以及每次 SIGHUP 之后调用。
 */
void
SyncRepInitConfig(void)
{
	int			priority;

	/*
	 * Determine if we are a potential sync standby and remember the result
	 * for handling replies from standby.
	 *
	 * 判断自己是否可能成为同步备库，并记住结果，供处理备库回复时使用。
	 */
	priority = SyncRepGetStandbyPriority();
	if (MyWalSnd->sync_standby_priority != priority)
	{
		SpinLockAcquire(&MyWalSnd->mutex);
		MyWalSnd->sync_standby_priority = priority;
		SpinLockRelease(&MyWalSnd->mutex);

		ereport(DEBUG1,
				(errmsg_internal("standby \"%s\" now has synchronous standby priority %d",
								 application_name, priority)));
	}
}

/*
 * Update the LSNs on each queue based upon our latest state. This
 * implements a simple policy of first-valid-sync-standby-releases-waiter.
 *
 * 按最新状态更新各队列上的 LSN。这里实现的简单策略是：第一个有效的同步备库放行等待者。
 *
 * Other policies are possible, which would change what we do here and
 * perhaps also which information we store as well.
 *
 * 也可以采用其他策略，那会改变这里的做法，也许还会改变要保存的信息。
 */
void
SyncRepReleaseWaiters(void)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	XLogRecPtr	writePtr;
	XLogRecPtr	flushPtr;
	XLogRecPtr	applyPtr;
	bool		got_recptr;
	bool		am_sync;
	int			numwrite = 0;
	int			numflush = 0;
	int			numapply = 0;

	/*
	 * If this WALSender is serving a standby that is not on the list of
	 * potential sync standbys then we have nothing to do. If we are still
	 * starting up, still running base backup or the current flush position is
	 * still invalid, then leave quickly also.  Streaming or stopping WAL
	 * senders are allowed to release waiters.
	 *
	 * 若这个 walsender 服务的备库不在潜在同步备库名单上，则无事可做。
	 * 若仍在启动、仍在做基础备份，或当前刷盘位置无效，也尽快离开。正在流式发送或正在停止的 walsender 可以放行等待者。
	 */
	if (MyWalSnd->sync_standby_priority == 0 ||
		(MyWalSnd->state != WALSNDSTATE_STREAMING &&
		 MyWalSnd->state != WALSNDSTATE_STOPPING) ||
		XLogRecPtrIsInvalid(MyWalSnd->flush))
	{
		announce_next_takeover = true;
		return;
	}

	/*
	 * We're a potential sync standby. Release waiters if there are enough
	 * sync standbys and we are considered as sync.
	 *
	 * 我们是潜在同步备库。若同步备库数量足够且我们被视为同步备库，则放行等待者。
	 */
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

	/*
	 * Check whether we are a sync standby or not, and calculate the synced
	 * positions among all sync standbys.  (Note: although this step does not
	 * of itself require holding SyncRepLock, it seems like a good idea to do
	 * it after acquiring the lock.  This ensures that the WAL pointers we use
	 * to release waiters are newer than any previous execution of this
	 * routine used.)
	 *
	 * 判断自己是不是同步备库，并计算所有同步备库之间已同步的位置。
	 * 这一步本身不必持有 SyncRepLock，但在加锁之后做更好，这样用来放行等待者的 WAL 指针会新于本函数先前任何一次执行所用的指针。
	 */
	got_recptr = SyncRepGetSyncRecPtr(&writePtr, &flushPtr, &applyPtr, &am_sync);

	/*
	 * If we are managing a sync standby, though we weren't prior to this,
	 * then announce we are now a sync standby.
	 *
	 * 若我们正在管理一个同步备库，而先前并不是，则宣告自己现在是同步备库。
	 */
	if (announce_next_takeover && am_sync)
	{
		announce_next_takeover = false;

		if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY)
			ereport(LOG,
					(errmsg("standby \"%s\" is now a synchronous standby with priority %d",
							application_name, MyWalSnd->sync_standby_priority)));
		else
			ereport(LOG,
					(errmsg("standby \"%s\" is now a candidate for quorum synchronous standby",
							application_name)));
	}

	/*
	 * If the number of sync standbys is less than requested or we aren't
	 * managing a sync standby then just leave.
	 *
	 * 若同步备库数量少于要求，或者我们并没有在管理同步备库，则直接离开。
	 */
	if (!got_recptr || !am_sync)
	{
		LWLockRelease(SyncRepLock);
		announce_next_takeover = !am_sync;
		return;
	}

	/*
	 * Set the lsn first so that when we wake backends they will release up to
	 * this location.
	 *
	 * 先设置 LSN，这样唤醒后端时它们会放行到这个位置。
	 */
	if (walsndctl->lsn[SYNC_REP_WAIT_WRITE] < writePtr)
	{
		walsndctl->lsn[SYNC_REP_WAIT_WRITE] = writePtr;
		numwrite = SyncRepWakeQueue(false, SYNC_REP_WAIT_WRITE);
	}
	if (walsndctl->lsn[SYNC_REP_WAIT_FLUSH] < flushPtr)
	{
		walsndctl->lsn[SYNC_REP_WAIT_FLUSH] = flushPtr;
		numflush = SyncRepWakeQueue(false, SYNC_REP_WAIT_FLUSH);
	}
	if (walsndctl->lsn[SYNC_REP_WAIT_APPLY] < applyPtr)
	{
		walsndctl->lsn[SYNC_REP_WAIT_APPLY] = applyPtr;
		numapply = SyncRepWakeQueue(false, SYNC_REP_WAIT_APPLY);
	}

	LWLockRelease(SyncRepLock);

	elog(DEBUG3, "released %d procs up to write %X/%X, %d procs up to flush %X/%X, %d procs up to apply %X/%X",
		 numwrite, LSN_FORMAT_ARGS(writePtr),
		 numflush, LSN_FORMAT_ARGS(flushPtr),
		 numapply, LSN_FORMAT_ARGS(applyPtr));
}

/*
 * Calculate the synced Write, Flush and Apply positions among sync standbys.
 *
 * 计算各同步备库之间已同步的 Write、Flush 和 Apply 位置。
 *
 * Return false if the number of sync standbys is less than
 * synchronous_standby_names specifies. Otherwise return true and
 * store the positions into *writePtr, *flushPtr and *applyPtr.
 *
 * 若同步备库数量少于 synchronous_standby_names 的要求，则返回 false。否则返回 true，并把位置写入 writePtr、flushPtr 和 applyPtr。
 *
 * On return, *am_sync is set to true if this walsender is connecting to
 * sync standby. Otherwise it's set to false.
 *
 * 返回时，若这个 walsender 连的是同步备库，则 am_sync 为 true，否则为 false。
 */
static bool
SyncRepGetSyncRecPtr(XLogRecPtr *writePtr, XLogRecPtr *flushPtr,
					 XLogRecPtr *applyPtr, bool *am_sync)
{
	SyncRepStandbyData *sync_standbys;
	int			num_standbys;
	int			i;

	/* Initialize default results
	 *
	 * 初始化默认结果。
	 */
	*writePtr = InvalidXLogRecPtr;
	*flushPtr = InvalidXLogRecPtr;
	*applyPtr = InvalidXLogRecPtr;
	*am_sync = false;

	/* Quick out if not even configured to be synchronous
	 *
	 * 若根本没有配置成同步，则快速返回。
	 */
	if (SyncRepConfig == NULL)
		return false;

	/* Get standbys that are considered as synchronous at this moment
	 *
	 * 取得当前被视为同步的备库。
	 */
	num_standbys = SyncRepGetCandidateStandbys(&sync_standbys);

	/* Am I among the candidate sync standbys?
	 *
	 * 我是否在候选同步备库之中？
	 */
	for (i = 0; i < num_standbys; i++)
	{
		if (sync_standbys[i].is_me)
		{
			*am_sync = true;
			break;
		}
	}

	/*
	 * Nothing more to do if we are not managing a sync standby or there are
	 * not enough synchronous standbys.
	 *
	 * 若我们没有在管理同步备库，或者同步备库数量不够，则不必再做。
	 */
	if (!(*am_sync) ||
		num_standbys < SyncRepConfig->num_sync)
	{
		pfree(sync_standbys);
		return false;
	}

	/*
	 * In a priority-based sync replication, the synced positions are the
	 * oldest ones among sync standbys. In a quorum-based, they are the Nth
	 * latest ones.
	 *
	 * 优先级同步复制中，已同步位置是各同步备库里最老的那个。quorum 方式下则是第 N 新的那个。
	 *
	 * SyncRepGetNthLatestSyncRecPtr() also can calculate the oldest
	 * positions. But we use SyncRepGetOldestSyncRecPtr() for that calculation
	 * because it's a bit more efficient.
	 *
	 * SyncRepGetNthLatestSyncRecPtr() 也能算出最老的位置。但算最老位置时用 SyncRepGetOldestSyncRecPtr()，因为它稍快一些。
	 *
	 * XXX If the numbers of current and requested sync standbys are the same,
	 * we can use SyncRepGetOldestSyncRecPtr() to calculate the synced
	 * positions even in a quorum-based sync replication.
	 *
	 * 待办：若当前同步备库数与要求的数量相同，即使是 quorum 同步复制，也可以用 SyncRepGetOldestSyncRecPtr() 计算已同步位置。
	 */
	if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY)
	{
		SyncRepGetOldestSyncRecPtr(writePtr, flushPtr, applyPtr,
								   sync_standbys, num_standbys);
	}
	else
	{
		SyncRepGetNthLatestSyncRecPtr(writePtr, flushPtr, applyPtr,
									  sync_standbys, num_standbys,
									  SyncRepConfig->num_sync);
	}

	pfree(sync_standbys);
	return true;
}

/*
 * Calculate the oldest Write, Flush and Apply positions among sync standbys.
 *
 * 计算各同步备库中最老的 Write、Flush 和 Apply 位置。
 */
static void
SyncRepGetOldestSyncRecPtr(XLogRecPtr *writePtr,
						   XLogRecPtr *flushPtr,
						   XLogRecPtr *applyPtr,
						   SyncRepStandbyData *sync_standbys,
						   int num_standbys)
{
	int			i;

	/*
	 * Scan through all sync standbys and calculate the oldest Write, Flush
	 * and Apply positions.  We assume *writePtr et al were initialized to
	 * InvalidXLogRecPtr.
	 *
	 * 扫描全部同步备库，计算最老的 Write、Flush 和 Apply 位置。假定 writePtr 等已初始化为 InvalidXLogRecPtr。
	 */
	for (i = 0; i < num_standbys; i++)
	{
		XLogRecPtr	write = sync_standbys[i].write;
		XLogRecPtr	flush = sync_standbys[i].flush;
		XLogRecPtr	apply = sync_standbys[i].apply;

		if (XLogRecPtrIsInvalid(*writePtr) || *writePtr > write)
			*writePtr = write;
		if (XLogRecPtrIsInvalid(*flushPtr) || *flushPtr > flush)
			*flushPtr = flush;
		if (XLogRecPtrIsInvalid(*applyPtr) || *applyPtr > apply)
			*applyPtr = apply;
	}
}

/*
 * Calculate the Nth latest Write, Flush and Apply positions among sync
 * standbys.
 *
 * 计算各同步备库中第 N 新的 Write、Flush 和 Apply 位置。
 */
static void
SyncRepGetNthLatestSyncRecPtr(XLogRecPtr *writePtr,
							  XLogRecPtr *flushPtr,
							  XLogRecPtr *applyPtr,
							  SyncRepStandbyData *sync_standbys,
							  int num_standbys,
							  uint8 nth)
{
	XLogRecPtr *write_array;
	XLogRecPtr *flush_array;
	XLogRecPtr *apply_array;
	int			i;

	/* Should have enough candidates, or somebody messed up
	 *
	 * 候选数量应当足够，否则就是前面搞错了。
	 */
	Assert(nth > 0 && nth <= num_standbys);

	write_array = (XLogRecPtr *) palloc(sizeof(XLogRecPtr) * num_standbys);
	flush_array = (XLogRecPtr *) palloc(sizeof(XLogRecPtr) * num_standbys);
	apply_array = (XLogRecPtr *) palloc(sizeof(XLogRecPtr) * num_standbys);

	for (i = 0; i < num_standbys; i++)
	{
		write_array[i] = sync_standbys[i].write;
		flush_array[i] = sync_standbys[i].flush;
		apply_array[i] = sync_standbys[i].apply;
	}

	/* Sort each array in descending order
	 *
	 * 把每个数组按降序排序。
	 */
	qsort(write_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);
	qsort(flush_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);
	qsort(apply_array, num_standbys, sizeof(XLogRecPtr), cmp_lsn);

	/* Get Nth latest Write, Flush, Apply positions
	 *
	 * 取第 N 新的 Write、Flush、Apply 位置。
	 */
	*writePtr = write_array[nth - 1];
	*flushPtr = flush_array[nth - 1];
	*applyPtr = apply_array[nth - 1];

	pfree(write_array);
	pfree(flush_array);
	pfree(apply_array);
}

/*
 * Compare lsn in order to sort array in descending order.
 *
 * 比较 LSN，以便把数组排成降序。
 */
static int
cmp_lsn(const void *a, const void *b)
{
	XLogRecPtr	lsn1 = *((const XLogRecPtr *) a);
	XLogRecPtr	lsn2 = *((const XLogRecPtr *) b);

	return pg_cmp_u64(lsn2, lsn1);
}

/*
 * Return data about walsenders that are candidates to be sync standbys.
 *
 * 返回有可能成为同步备库的 walsender 的数据。
 *
 * *standbys is set to a palloc'd array of structs of per-walsender data,
 * and the number of valid entries (candidate sync senders) is returned.
 * (This might be more or fewer than num_sync; caller must check.)
 *
 * standbys 被设为 palloc 出来的、每个 walsender 一份数据的数组，返回值是有效项个数，即候选同步发送端的数量。它可能多于或少于 num_sync，调用者必须检查。
 */
int
SyncRepGetCandidateStandbys(SyncRepStandbyData **standbys)
{
	int			i;
	int			n;

	/* Create result array
	 *
	 * 创建结果数组。
	 */
	*standbys = (SyncRepStandbyData *)
		palloc(max_wal_senders * sizeof(SyncRepStandbyData));

	/* Quick exit if sync replication is not requested
	 *
	 * 若未要求同步复制，则快速返回。
	 */
	if (SyncRepConfig == NULL)
		return 0;

	/* Collect raw data from shared memory
	 *
	 * 从共享内存收集原始数据。
	 */
	n = 0;
	for (i = 0; i < max_wal_senders; i++)
	{
		volatile WalSnd *walsnd;	/* Use volatile pointer to prevent code
									 * rearrangement
									 *
									 * 使用 volatile 指针，防止编译器重排代码。
									 */
		SyncRepStandbyData *stby;
		WalSndState state;		/* not included in SyncRepStandbyData
								 *
								 * 不放进 SyncRepStandbyData。
								 */

		walsnd = &WalSndCtl->walsnds[i];
		stby = *standbys + n;

		SpinLockAcquire(&walsnd->mutex);
		stby->pid = walsnd->pid;
		state = walsnd->state;
		stby->write = walsnd->write;
		stby->flush = walsnd->flush;
		stby->apply = walsnd->apply;
		stby->sync_standby_priority = walsnd->sync_standby_priority;
		SpinLockRelease(&walsnd->mutex);

		/* Must be active
		 *
		 * 必须处于活动状态。
		 */
		if (stby->pid == 0)
			continue;

		/* Must be streaming or stopping
		 *
		 * 必须正在流式发送或正在停止。
		 */
		if (state != WALSNDSTATE_STREAMING &&
			state != WALSNDSTATE_STOPPING)
			continue;

		/* Must be synchronous
		 *
		 * 必须是同步的。
		 */
		if (stby->sync_standby_priority == 0)
			continue;

		/* Must have a valid flush position
		 *
		 * 必须有有效的刷盘位置。
		 */
		if (XLogRecPtrIsInvalid(stby->flush))
			continue;

		/* OK, it's a candidate
		 *
		 * 可以，它是候选。
		 */
		stby->walsnd_index = i;
		stby->is_me = (walsnd == MyWalSnd);
		n++;
	}

	/*
	 * In quorum mode, we return all the candidates.  In priority mode, if we
	 * have too many candidates then return only the num_sync ones of highest
	 * priority.
	 *
	 * quorum 模式下返回全部候选。优先级模式下，若候选过多，则只返回优先级最高的 num_sync 个。
	 */
	if (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY &&
		n > SyncRepConfig->num_sync)
	{
		/* Sort by priority ...
		 *
		 * 按优先级排序。
		 */
		qsort(*standbys, n, sizeof(SyncRepStandbyData),
			  standby_priority_comparator);
		/* ... then report just the first num_sync ones
		 *
		 * 然后只报告前 num_sync 个。
		 */
		n = SyncRepConfig->num_sync;
	}

	return n;
}

/*
 * qsort comparator to sort SyncRepStandbyData entries by priority
 *
 * 按优先级排序 SyncRepStandbyData 的 qsort 比较函数。
 */
static int
standby_priority_comparator(const void *a, const void *b)
{
	const SyncRepStandbyData *sa = (const SyncRepStandbyData *) a;
	const SyncRepStandbyData *sb = (const SyncRepStandbyData *) b;

	/* First, sort by increasing priority value
	 *
	 * 先按优先级数值从小到大排序。
	 */
	if (sa->sync_standby_priority != sb->sync_standby_priority)
		return sa->sync_standby_priority - sb->sync_standby_priority;

	/*
	 * We might have equal priority values; arbitrarily break ties by position
	 * in the WalSnd array.  (This is utterly bogus, since that is arrival
	 * order dependent, but there are regression tests that rely on it.)
	 *
	 * 优先级可能相同；这时按 WalSnd 数组中的位置任意打破平局。这并不合理，因为它取决于到达顺序，但有回归测试依赖这个行为。
	 */
	return sa->walsnd_index - sb->walsnd_index;
}


/*
 * Check if we are in the list of sync standbys, and if so, determine
 * priority sequence. Return priority if set, or zero to indicate that
 * we are not a potential sync standby.
 *
 * 检查自己是否在同步备库名单中；若在，则确定优先级序号。已设置则返回优先级，否则返回 0，表示不是潜在同步备库。
 *
 * Compare the parameter SyncRepStandbyNames against the application_name
 * for this WALSender, or allow any name if we find a wildcard "*".
 *
 * 把参数 SyncRepStandbyNames 与这个 walsender 的 application_name 比较；若遇到通配符星号，则允许任意名字。
 */
static int
SyncRepGetStandbyPriority(void)
{
	const char *standby_name;
	int			priority;
	bool		found = false;

	/*
	 * Since synchronous cascade replication is not allowed, we always set the
	 * priority of cascading walsender to zero.
	 *
	 * 不允许同步级联复制，因此级联 walsender 的优先级始终设为 0。
	 */
	if (am_cascading_walsender)
		return 0;

	if (!SyncStandbysDefined() || SyncRepConfig == NULL)
		return 0;

	standby_name = SyncRepConfig->member_names;
	for (priority = 1; priority <= SyncRepConfig->nmembers; priority++)
	{
		if (pg_strcasecmp(standby_name, application_name) == 0 ||
			strcmp(standby_name, "*") == 0)
		{
			found = true;
			break;
		}
		standby_name += strlen(standby_name) + 1;
	}

	if (!found)
		return 0;

	/*
	 * In quorum-based sync replication, all the standbys in the list have the
	 * same priority, one.
	 *
	 * 在基于 quorum 的同步复制中，名单里所有备库的优先级都是 1。
	 */
	return (SyncRepConfig->syncrep_method == SYNC_REP_PRIORITY) ? priority : 1;
}

/*
 * Walk the specified queue from head.  Set the state of any backends that
 * need to be woken, remove them from the queue, and then wake them.
 * Pass all = true to wake whole queue; otherwise, just wake up to
 * the walsender's LSN.
 *
 * 从队头遍历指定队列。给需要唤醒的后端设置状态，把它们移出队列，然后唤醒。
 * all 为 true 时唤醒整条队列，否则只唤醒到该 walsender 的 LSN。
 *
 * The caller must hold SyncRepLock in exclusive mode.
 *
 * 调用者必须以排他模式持有 SyncRepLock。
 */
static int
SyncRepWakeQueue(bool all, int mode)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	int			numprocs = 0;
	dlist_mutable_iter iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);
	Assert(LWLockHeldByMeInMode(SyncRepLock, LW_EXCLUSIVE));
	Assert(SyncRepQueueIsOrderedByLSN(mode));

	dlist_foreach_modify(iter, &WalSndCtl->SyncRepQueue[mode])
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * Assume the queue is ordered by LSN
		 *
		 * 假定队列按 LSN 有序。
		 */
		if (!all && walsndctl->lsn[mode] < proc->waitLSN)
			return numprocs;

		/*
		 * Remove from queue.
		 *
		 * 从队列中移除。
		 */
		dlist_delete_thoroughly(&proc->syncRepLinks);

		/*
		 * SyncRepWaitForLSN() reads syncRepState without holding the lock, so
		 * make sure that it sees the queue link being removed before the
		 * syncRepState change.
		 *
		 * SyncRepWaitForLSN() 不加锁读取 syncRepState，因此必须让它先看到队列链接被移除，再看到 syncRepState 的变化。
		 */
		pg_write_barrier();

		/*
		 * Set state to complete; see SyncRepWaitForLSN() for discussion of
		 * the various states.
		 *
		 * 把状态设为完成。各种状态的讨论见 SyncRepWaitForLSN()。
		 */
		proc->syncRepState = SYNC_REP_WAIT_COMPLETE;

		/*
		 * Wake only when we have set state and removed from queue.
		 *
		 * 只有在设置了状态并且移出队列之后才唤醒。
		 */
		SetLatch(&(proc->procLatch));

		numprocs++;
	}

	return numprocs;
}

/*
 * The checkpointer calls this as needed to update the shared
 * sync_standbys_status flag, so that backends don't remain permanently wedged
 * if synchronous_standby_names is unset.  It's safe to check the current value
 * without the lock, because it's only ever updated by one process.  But we
 * must take the lock to change it.
 *
 * checkpointer 在需要时调用本函数，更新共享的 sync_standbys_status 标志，以免 synchronous_standby_names 被清空后后端永远卡住。
 * 当前值可以不加锁读取，因为只有一个进程会更新它。但修改时必须加锁。
 */
void
SyncRepUpdateSyncStandbysDefined(void)
{
	bool		sync_standbys_defined = SyncStandbysDefined();

	if (sync_standbys_defined !=
		((WalSndCtl->sync_standbys_status & SYNC_STANDBY_DEFINED) != 0))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * If synchronous_standby_names has been reset to empty, it's futile
		 * for backends to continue waiting.  Since the user no longer wants
		 * synchronous replication, we'd better wake them up.
		 *
		 * 若 synchronous_standby_names 已被清空，后端继续等待没有意义。用户已经不再要同步复制，应当把它们唤醒。
		 */
		if (!sync_standbys_defined)
		{
			int			i;

			for (i = 0; i < NUM_SYNC_REP_WAIT_MODE; i++)
				SyncRepWakeQueue(true, i);
		}

		/*
		 * Only allow people to join the queue when there are synchronous
		 * standbys defined.  Without this interlock, there's a race
		 * condition: we might wake up all the current waiters; then, some
		 * backend that hasn't yet reloaded its config might go to sleep on
		 * the queue (and never wake up).  This prevents that.
		 *
		 * 只有定义了同步备库时才允许加入队列。否则会有竞争：我们可能先唤醒当前全部等待者，然后某个还没重载配置的后端又睡到队列上，并且永远醒不过来。这个互锁就是为了防止这种情况。
		 */
		WalSndCtl->sync_standbys_status = SYNC_STANDBY_INIT |
			(sync_standbys_defined ? SYNC_STANDBY_DEFINED : 0);

		LWLockRelease(SyncRepLock);
	}
	else if ((WalSndCtl->sync_standbys_status & SYNC_STANDBY_INIT) == 0)
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

		/*
		 * Note that there is no need to wake up the queues here.  We would
		 * reach this path only if SyncStandbysDefined() returns false, or it
		 * would mean that some backends are waiting with the GUC set.  See
		 * SyncRepWaitForLSN().
		 *
		 * 这里不必唤醒队列。只有 SyncStandbysDefined() 返回 false 才会走到这条路径，否则就意味着有后端在 GUC 已设置的情况下等待。见 SyncRepWaitForLSN()。
		 */
		Assert(!SyncStandbysDefined());

		/*
		 * Even if there is no sync standby defined, let the readers of this
		 * information know that the sync standby data has been initialized.
		 * This can just be done once, hence the previous check on
		 * SYNC_STANDBY_INIT to avoid useless work.
		 *
		 * 即使没有定义同步备库，也要让读取者知道同步备库数据已经初始化。这件事做一次即可，因此前面检查了 SYNC_STANDBY_INIT，避免重复劳动。
		 */
		WalSndCtl->sync_standbys_status |= SYNC_STANDBY_INIT;

		LWLockRelease(SyncRepLock);
	}
}

#ifdef USE_ASSERT_CHECKING
static bool
SyncRepQueueIsOrderedByLSN(int mode)
{
	XLogRecPtr	lastLSN;
	dlist_iter	iter;

	Assert(mode >= 0 && mode < NUM_SYNC_REP_WAIT_MODE);

	lastLSN = 0;

	dlist_foreach(iter, &WalSndCtl->SyncRepQueue[mode])
	{
		PGPROC	   *proc = dlist_container(PGPROC, syncRepLinks, iter.cur);

		/*
		 * Check the queue is ordered by LSN and that multiple procs don't
		 * have matching LSNs
		 *
		 * 检查队列按 LSN 有序，并且没有多个进程具有相同的 LSN。
		 */
		if (proc->waitLSN <= lastLSN)
			return false;

		lastLSN = proc->waitLSN;
	}

	return true;
}
#endif

/*
 * ===========================================================
 * Synchronous Replication functions executed by any process
 * ===========================================================
 *
 * 任何进程都可执行的同步复制函数。
 */

/*
 * 校验 GUC synchronous_standby_names，解析成功则把配置放进 extra。
 */
bool
check_synchronous_standby_names(char **newval, void **extra, GucSource source)
{
	if (*newval != NULL && (*newval)[0] != '\0')
	{
		yyscan_t	scanner;
		int			parse_rc;
		SyncRepConfigData *pconf;

		/* Result of parsing is returned in one of these two variables
		 *
		 * 解析结果放在这两个变量之一中。
		 */
		SyncRepConfigData *syncrep_parse_result = NULL;
		char	   *syncrep_parse_error_msg = NULL;

		/* Parse the synchronous_standby_names string
		 *
		 * 解析 synchronous_standby_names 字符串。
		 */
		syncrep_scanner_init(*newval, &scanner);
		parse_rc = syncrep_yyparse(&syncrep_parse_result, &syncrep_parse_error_msg, scanner);
		syncrep_scanner_finish(scanner);

		if (parse_rc != 0 || syncrep_parse_result == NULL)
		{
			GUC_check_errcode(ERRCODE_SYNTAX_ERROR);
			if (syncrep_parse_error_msg)
				GUC_check_errdetail("%s", syncrep_parse_error_msg);
			else
				/* translator: %s is a GUC name
				 *
				 * 翻译标记：%s 是 GUC 名。
				 */
				GUC_check_errdetail("\"%s\" parser failed.",
									"synchronous_standby_names");
			return false;
		}

		if (syncrep_parse_result->num_sync <= 0)
		{
			GUC_check_errmsg("number of synchronous standbys (%d) must be greater than zero",
							 syncrep_parse_result->num_sync);
			return false;
		}

		/* GUC extra value must be guc_malloc'd, not palloc'd
		 *
		 * GUC 的 extra 值必须用 guc_malloc 分配，不能用 palloc。
		 */
		pconf = (SyncRepConfigData *)
			guc_malloc(LOG, syncrep_parse_result->config_size);
		if (pconf == NULL)
			return false;
		memcpy(pconf, syncrep_parse_result, syncrep_parse_result->config_size);

		*extra = pconf;

		/*
		 * We need not explicitly clean up syncrep_parse_result.  It, and any
		 * other cruft generated during parsing, will be freed when the
		 * current memory context is deleted.  (This code is generally run in
		 * a short-lived context used for config file processing, so that will
		 * not be very long.)
		 *
		 * 不必显式清理 syncrep_parse_result。它以及解析中产生的其他杂物会在当前内存上下文删除时释放。
		 * 这段代码通常跑在处理配置文件的短命上下文里，所以不会拖很久。
		 */
	}
	else
		*extra = NULL;

	return true;
}

/*
 * 把解析好的同步备库配置安装为当前 SyncRepConfig。
 */
void
assign_synchronous_standby_names(const char *newval, void *extra)
{
	SyncRepConfig = (SyncRepConfigData *) extra;
}

/*
 * 按 synchronous_commit 设置本后端的 SyncRepWaitMode。
 */
void
assign_synchronous_commit(int newval, void *extra)
{
	switch (newval)
	{
		case SYNCHRONOUS_COMMIT_REMOTE_WRITE:
			SyncRepWaitMode = SYNC_REP_WAIT_WRITE;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_FLUSH:
			SyncRepWaitMode = SYNC_REP_WAIT_FLUSH;
			break;
		case SYNCHRONOUS_COMMIT_REMOTE_APPLY:
			SyncRepWaitMode = SYNC_REP_WAIT_APPLY;
			break;
		default:
			SyncRepWaitMode = SYNC_REP_NO_WAIT;
			break;
	}
}
