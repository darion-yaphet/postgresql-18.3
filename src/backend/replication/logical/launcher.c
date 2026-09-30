/*-------------------------------------------------------------------------
 * launcher.c
 *	   PostgreSQL logical replication worker launcher process
 *
 * PostgreSQL 逻辑复制 worker 的启动进程。
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/launcher.c
 *
 * NOTES
 *	  This module contains the logical replication worker launcher which
 *	  uses the background worker infrastructure to start the logical
 *	  replication workers for every enabled subscription.
 *
 * 说明。本模块包含逻辑复制 worker 启动器，它用后台 worker 机制为每个已
 * 启用的订阅启动逻辑复制 worker。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/snapmgr.h"

/*
 * 核心流程：
 * postmaster 经 ApplyLauncherRegister 启动 launcher。ApplyLauncherMain 循环读取
 * pg_subscription，对已启用且缺少 worker 的订阅调用 logicalrep_worker_launch。
 * 同一订阅两次启动的间隔受 wal_retrieve_retry_interval 限制，时间记在 last-start 共享哈希表。
 * 订阅目录变化在事务提交时由 ApplyLauncherWakeupAtCommit 唤醒 launcher。
 * 停止 worker 时 logicalrep_worker_stop 发信号并等待其脱离槽位。
 */

/* max sleep time between cycles (3min)
 *
 * 两轮之间的最长睡眠时间（3 分钟）
 */
#define DEFAULT_NAPTIME_PER_CYCLE 180000L

/* GUC variables
 *
 * GUC 变量
 */
int			max_logical_replication_workers = 4;
int			max_sync_workers_per_subscription = 2;
int			max_parallel_apply_workers_per_subscription = 2;

LogicalRepWorker *MyLogicalRepWorker = NULL;

typedef struct LogicalRepCtxStruct
{
	/* Supervisor process.
	 *
	 * 监督进程。
	 */
	pid_t		launcher_pid;

	/* Hash table holding last start times of subscriptions' apply workers.
	 *
	 * 保存各订阅 apply worker 最近一次启动时间的哈希表。
	 */
	dsa_handle	last_start_dsa;
	dshash_table_handle last_start_dsh;

	/* Background workers.
	 *
	 * 后台 worker。
	 */
	LogicalRepWorker workers[FLEXIBLE_ARRAY_MEMBER];
} LogicalRepCtxStruct;

static LogicalRepCtxStruct *LogicalRepCtx;

/* an entry in the last-start-times shared hash table
 *
 * 最近启动时间共享哈希表中的一项
 */
typedef struct LauncherLastStartTimesEntry
{
	Oid			subid;			/* OID of logrep subscription (hash key)
								 *
								 * 逻辑复制订阅的 OID（哈希键）
								 */
	TimestampTz last_start_time;	/* last time its apply worker was started
									 *
									 * 其 apply worker 上次启动的时间
									 */
} LauncherLastStartTimesEntry;

/* parameters for the last-start-times shared hash table
 *
 * 最近启动时间共享哈希表的参数
 */
static const dshash_parameters dsh_params = {
	sizeof(Oid),
	sizeof(LauncherLastStartTimesEntry),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	LWTRANCHE_LAUNCHER_HASH
};

static dsa_area *last_start_times_dsa = NULL;
static dshash_table *last_start_times = NULL;

static bool on_commit_launcher_wakeup = false;


static void ApplyLauncherWakeup(void);
static void logicalrep_launcher_onexit(int code, Datum arg);
static void logicalrep_worker_onexit(int code, Datum arg);
static void logicalrep_worker_detach(void);
static void logicalrep_worker_cleanup(LogicalRepWorker *worker);
static int	logicalrep_pa_worker_count(Oid subid);
static void logicalrep_launcher_attach_dshmem(void);
static void ApplyLauncherSetWorkerStartTime(Oid subid, TimestampTz start_time);
static TimestampTz ApplyLauncherGetWorkerStartTime(Oid subid);


/*
 * Load the list of subscriptions.
 *
 * 装入订阅列表。
 *
 * Only the fields interesting for worker start/stop functions are filled for
 * each subscription.
 *
 * 每个订阅只填写 worker 启动和停止函数关心的字段。
 */
static List *
get_subscription_list(void)
{
	List	   *res = NIL;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext resultcxt;

	/* This is the context that we will allocate our output data in
	 *
	 * 将在这个上下文中分配输出数据
	 */
	resultcxt = CurrentMemoryContext;

	/*
	 * Start a transaction so we can access pg_subscription.
	 *
	 * 开始一个事务，以便访问 pg_subscription。
	 */
	StartTransactionCommand();

	rel = table_open(SubscriptionRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_subscription subform = (Form_pg_subscription) GETSTRUCT(tup);
		Subscription *sub;
		MemoryContext oldcxt;

		/*
		 * Allocate our results in the caller's context, not the
		 * transaction's. We do this inside the loop, and restore the original
		 * context at the end, so that leaky things like heap_getnext() are
		 * not called in a potentially long-lived context.
		 *
		 * 把结果分配在调用方的上下文里，而不是事务的上下文里。在循环内部这样做，
		 * 并在结束时恢复原来的上下文，这样 heap_getnext() 之类会泄漏的调用就不
		 * 会发生在可能长寿的上下文中。
		 */
		oldcxt = MemoryContextSwitchTo(resultcxt);

		sub = (Subscription *) palloc0(sizeof(Subscription));
		sub->oid = subform->oid;
		sub->dbid = subform->subdbid;
		sub->owner = subform->subowner;
		sub->enabled = subform->subenabled;
		sub->name = pstrdup(NameStr(subform->subname));
		/* We don't fill fields we are not interested in.
		 *
		 * 不填写我们不关心的字段。
		 */

		res = lappend(res, sub);
		MemoryContextSwitchTo(oldcxt);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return res;
}

/*
 * Wait for a background worker to start up and attach to the shmem context.
 *
 * 等待后台 worker 启动并挂上共享内存上下文。
 *
 * This is only needed for cleaning up the shared memory in case the worker
 * fails to attach.
 *
 * 仅用于 worker 未能挂上时清理共享内存。
 *
 * Returns whether the attach was successful.
 *
 * 返回挂接是否成功。
 */
static bool
WaitForReplicationWorkerAttach(LogicalRepWorker *worker,
							   uint16 generation,
							   BackgroundWorkerHandle *handle)
{
	bool		result = false;
	bool		dropped_latch = false;

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;
		int			rc;

		CHECK_FOR_INTERRUPTS();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/* Worker either died or has started. Return false if died.
		 *
		 * worker 要么已经退出，要么已经启动。若已退出则返回 false。
		 */
		if (!worker->in_use || worker->proc)
		{
			result = worker->in_use;
			LWLockRelease(LogicalRepWorkerLock);
			break;
		}

		LWLockRelease(LogicalRepWorkerLock);

		/* Check if worker has died before attaching, and clean up after it.
		 *
		 * 在挂接之前检查 worker 是否已退出，并在它退出后做清理。
		 */
		status = GetBackgroundWorkerPid(handle, &pid);

		if (status == BGWH_STOPPED)
		{
			LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);
			/* Ensure that this was indeed the worker we waited for.
			 *
			 * 确认这确实是我们等待的那个 worker。
			 */
			if (generation == worker->generation)
				logicalrep_worker_cleanup(worker);
			LWLockRelease(LogicalRepWorkerLock);
			break;				/* result is already false
								 *
								 * 结果已经是 false
								 */
		}

		/*
		 * We need timeout because we generally don't get notified via latch
		 * about the worker attach.  But we don't expect to have to wait long.
		 *
		 * 需要超时，因为 worker 挂上时通常不会通过 latch 通知我们。不过预计不
		 * 会等很久。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
			dropped_latch = true;
		}
	}

	/*
	 * If we had to clear a latch event in order to wait, be sure to restore
	 * it before exiting.  Otherwise caller may miss events.
	 *
	 * 若为了等待而清掉了 latch 事件，退出前务必把它恢复。否则调用方可能错
	 * 过事件。
	 */
	if (dropped_latch)
		SetLatch(MyLatch);

	return result;
}

/*
 * Walks the workers array and searches for one that matches given
 * subscription id and relid.
 *
 * 遍历 worker 数组，查找与给定订阅 id 和 relid 匹配的那一个。
 *
 * We are only interested in the leader apply worker or table sync worker.
 *
 * 我们只关心 leader apply worker 或表同步 worker。
 */
LogicalRepWorker *
logicalrep_worker_find(Oid subid, Oid relid, bool only_running)
{
	int			i;
	LogicalRepWorker *res = NULL;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id.
	 *
	 * 按给定的订阅 id 查找已挂上的 worker。
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		/* Skip parallel apply workers.
		 *
		 * 跳过并行 apply worker。
		 */
		if (isParallelApplyWorker(w))
			continue;

		if (w->in_use && w->subid == subid && w->relid == relid &&
			(!only_running || w->proc))
		{
			res = w;
			break;
		}
	}

	return res;
}

/*
 * Similar to logicalrep_worker_find(), but returns a list of all workers for
 * the subscription, instead of just one.
 *
 * 与 logicalrep_worker_find() 类似，但返回该订阅的全部 worker 列表，而
 * 不是只返回一个。
 */
List *
logicalrep_workers_find(Oid subid, bool only_running, bool acquire_lock)
{
	int			i;
	List	   *res = NIL;

	if (acquire_lock)
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id.
	 *
	 * 按给定的订阅 id 查找已挂上的 worker。
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (w->in_use && w->subid == subid && (!only_running || w->proc))
			res = lappend(res, w);
	}

	if (acquire_lock)
		LWLockRelease(LogicalRepWorkerLock);

	return res;
}

/*
 * Start new logical replication background worker, if possible.
 *
 * 如果可能，启动新的逻辑复制后台 worker。
 *
 * Returns true on success, false on failure.
 *
 * 成功返回 true，失败返回 false。
 */
bool
logicalrep_worker_launch(LogicalRepWorkerType wtype,
						 Oid dbid, Oid subid, const char *subname, Oid userid,
						 Oid relid, dsm_handle subworker_dsm)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	uint16		generation;
	int			i;
	int			slot = 0;
	LogicalRepWorker *worker = NULL;
	int			nsyncworkers;
	int			nparallelapplyworkers;
	TimestampTz now;
	bool		is_tablesync_worker = (wtype == WORKERTYPE_TABLESYNC);
	bool		is_parallel_apply_worker = (wtype == WORKERTYPE_PARALLEL_APPLY);

	/*----------
	 * Sanity checks:
	 * - must be valid worker type
	 * - tablesync workers are only ones to have relid
	 * - parallel apply worker is the only kind of subworker
	 *
	 * 完整性检查：worker 类型必须有效；只有表同步 worker 才有 relid；并行
	 * apply worker 是唯一的子 worker 种类。
	 */
	Assert(wtype != WORKERTYPE_UNKNOWN);
	Assert(is_tablesync_worker == OidIsValid(relid));
	Assert(is_parallel_apply_worker == (subworker_dsm != DSM_HANDLE_INVALID));

	ereport(DEBUG1,
			(errmsg_internal("starting logical replication worker for subscription \"%s\"",
							 subname)));

	/* Report this after the initial starting message for consistency.
	 *
	 * 为了前后一致，在最初的启动消息之后再报告这个。
	 */
	if (max_active_replication_origins == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("cannot start logical replication workers when \"max_active_replication_origins\" is 0")));

	/*
	 * We need to do the modification of the shared memory under lock so that
	 * we have consistent view.
	 *
	 * 必须在持锁的情况下修改共享内存，才能看到一致的视图。
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

retry:
	/* Find unused worker slot.
	 *
	 * 找一个未使用的 worker 槽位。
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (!w->in_use)
		{
			worker = w;
			slot = i;
			break;
		}
	}

	nsyncworkers = logicalrep_sync_worker_count(subid);

	now = GetCurrentTimestamp();

	/*
	 * If we didn't find a free slot, try to do garbage collection.  The
	 * reason we do this is because if some worker failed to start up and its
	 * parent has crashed while waiting, the in_use state was never cleared.
	 *
	 * 若没找到空闲槽位，就尝试做垃圾回收。这样做是因为：若某个 worker 启动
	 * 失败，且它的父进程在等待时崩溃，in_use 状态就永远不会被清掉。
	 */
	if (worker == NULL || nsyncworkers >= max_sync_workers_per_subscription)
	{
		bool		did_cleanup = false;

		for (i = 0; i < max_logical_replication_workers; i++)
		{
			LogicalRepWorker *w = &LogicalRepCtx->workers[i];

			/*
			 * If the worker was marked in use but didn't manage to attach in
			 * time, clean it up.
			 *
			 * 若 worker 被标为正在使用，但没能及时挂上，就把它清理掉。
			 */
			if (w->in_use && !w->proc &&
				TimestampDifferenceExceeds(w->launch_time, now,
										   wal_receiver_timeout))
			{
				elog(WARNING,
					 "logical replication worker for subscription %u took too long to start; canceled",
					 w->subid);

				logicalrep_worker_cleanup(w);
				did_cleanup = true;
			}
		}

		if (did_cleanup)
			goto retry;
	}

	/*
	 * We don't allow to invoke more sync workers once we have reached the
	 * sync worker limit per subscription. So, just return silently as we
	 * might get here because of an otherwise harmless race condition.
	 *
	 * 每个订阅的同步 worker 达到上限后，就不再启动更多同步 worker。这里直
	 * 接静默返回，因为可能只是一次无害的竞态才走到这里。
	 */
	if (is_tablesync_worker && nsyncworkers >= max_sync_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return false;
	}

	nparallelapplyworkers = logicalrep_pa_worker_count(subid);

	/*
	 * Return false if the number of parallel apply workers reached the limit
	 * per subscription.
	 *
	 * 若该订阅的并行 apply worker 数量已达上限，则返回 false。
	 */
	if (is_parallel_apply_worker &&
		nparallelapplyworkers >= max_parallel_apply_workers_per_subscription)
	{
		LWLockRelease(LogicalRepWorkerLock);
		return false;
	}

	/*
	 * However if there are no more free worker slots, inform user about it
	 * before exiting.
	 *
	 * 不过若已经没有空闲的 worker 槽位，退出前要告知用户。
	 */
	if (worker == NULL)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of logical replication worker slots"),
				 errhint("You might need to increase \"%s\".", "max_logical_replication_workers")));
		return false;
	}

	/* Prepare the worker slot.
	 *
	 * 准备该 worker 槽位。
	 */
	worker->type = wtype;
	worker->launch_time = now;
	worker->in_use = true;
	worker->generation++;
	worker->proc = NULL;
	worker->dbid = dbid;
	worker->userid = userid;
	worker->subid = subid;
	worker->relid = relid;
	worker->relstate = SUBREL_STATE_UNKNOWN;
	worker->relstate_lsn = InvalidXLogRecPtr;
	worker->stream_fileset = NULL;
	worker->leader_pid = is_parallel_apply_worker ? MyProcPid : InvalidPid;
	worker->parallel_apply = is_parallel_apply_worker;
	worker->last_lsn = InvalidXLogRecPtr;
	TIMESTAMP_NOBEGIN(worker->last_send_time);
	TIMESTAMP_NOBEGIN(worker->last_recv_time);
	worker->reply_lsn = InvalidXLogRecPtr;
	TIMESTAMP_NOBEGIN(worker->reply_time);

	/* Before releasing lock, remember generation for future identification.
	 *
	 * 释放锁之前，记住 generation，以便以后识别。
	 */
	generation = worker->generation;

	LWLockRelease(LogicalRepWorkerLock);

	/* Register the new dynamic worker.
	 *
	 * 注册新的动态 worker。
	 */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");

	switch (worker->type)
	{
		case WORKERTYPE_APPLY:
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN,
					 "logical replication apply worker for subscription %u",
					 subid);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication apply worker");
			break;

		case WORKERTYPE_PARALLEL_APPLY:
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ParallelApplyWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN,
					 "logical replication parallel apply worker for subscription %u",
					 subid);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication parallel worker");

			memcpy(bgw.bgw_extra, &subworker_dsm, sizeof(dsm_handle));
			break;

		case WORKERTYPE_TABLESYNC:
			snprintf(bgw.bgw_function_name, BGW_MAXLEN, "TablesyncWorkerMain");
			snprintf(bgw.bgw_name, BGW_MAXLEN,
					 "logical replication tablesync worker for subscription %u sync %u",
					 subid,
					 relid);
			snprintf(bgw.bgw_type, BGW_MAXLEN, "logical replication tablesync worker");
			break;

		case WORKERTYPE_UNKNOWN:
			/* Should never happen.
			 *
			 * 不应该发生。
			 */
			elog(ERROR, "unknown worker type");
	}

	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = Int32GetDatum(slot);

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		/* Failed to start worker, so clean up the worker slot.
		 *
		 * 启动 worker 失败，因此清理该 worker 槽位。
		 */
		LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);
		Assert(generation == worker->generation);
		logicalrep_worker_cleanup(worker);
		LWLockRelease(LogicalRepWorkerLock);

		ereport(WARNING,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("out of background worker slots"),
				 errhint("You might need to increase \"%s\".", "max_worker_processes")));
		return false;
	}

	/* Now wait until it attaches.
	 *
	 * 现在等到它挂上。
	 */
	return WaitForReplicationWorkerAttach(worker, generation, bgw_handle);
}

/*
 * Internal function to stop the worker and wait until it detaches from the
 * slot.
 *
 * 停止该 worker 并等待它脱离槽位的内部函数。
 */
static void
logicalrep_worker_stop_internal(LogicalRepWorker *worker, int signo)
{
	uint16		generation;

	Assert(LWLockHeldByMeInMode(LogicalRepWorkerLock, LW_SHARED));

	/*
	 * Remember which generation was our worker so we can check if what we see
	 * is still the same one.
	 *
	 * 记住我们这个 worker 的 generation，以便检查现在看到的是否仍是同一个。
	 */
	generation = worker->generation;

	/*
	 * If we found a worker but it does not have proc set then it is still
	 * starting up; wait for it to finish starting and then kill it.
	 *
	 * 若找到了 worker 但还没有设置 proc，说明它仍在启动；等它启动完成后再
	 * 杀掉它。
	 */
	while (worker->in_use && !worker->proc)
	{
		int			rc;

		LWLockRelease(LogicalRepWorkerLock);

		/* Wait a bit --- we don't expect to have to wait long.
		 *
		 * 稍等一下。预计不会等很久。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		/* Recheck worker status.
		 *
		 * 再次检查 worker 状态。
		 */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		/*
		 * Check whether the worker slot is no longer used, which would mean
		 * that the worker has exited, or whether the worker generation is
		 * different, meaning that a different worker has taken the slot.
		 *
		 * 检查 worker 槽位是否已不再使用（表示 worker 已退出），或者
		 * generation 是否已经不同（表示另一个 worker 占用了该槽位）。
		 */
		if (!worker->in_use || worker->generation != generation)
			return;

		/* Worker has assigned proc, so it has started.
		 *
		 * worker 已经分配了 proc，说明它已经启动。
		 */
		if (worker->proc)
			break;
	}

	/* Now terminate the worker ...
	 *
	 * 现在终止该 worker……
	 */
	kill(worker->proc->pid, signo);

	/* ... and wait for it to die.
	 *
	 * ……并等待它退出。
	 */
	for (;;)
	{
		int			rc;

		/* is it gone?
		 *
		 * 它已经不在了吗？
		 */
		if (!worker->proc || worker->generation != generation)
			break;

		LWLockRelease(LogicalRepWorkerLock);

		/* Wait a bit --- we don't expect to have to wait long.
		 *
		 * 稍等一下。预计不会等很久。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_BGWORKER_SHUTDOWN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
	}
}

/*
 * Stop the logical replication worker for subid/relid, if any.
 *
 * 若有的话，停止 subid/relid 对应的逻辑复制 worker。
 */
void
logicalrep_worker_stop(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, false);

	if (worker)
	{
		Assert(!isParallelApplyWorker(worker));
		logicalrep_worker_stop_internal(worker, SIGTERM);
	}

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Stop the given logical replication parallel apply worker.
 *
 * 停止指定的逻辑复制并行 apply worker。
 *
 * Node that the function sends SIGUSR2 instead of SIGTERM to the parallel apply
 * worker so that the worker exits cleanly.
 *
 * 注意：本函数向并行 apply worker 发送的是 SIGUSR2 而不是 SIGTERM，以
 * 便它干净地退出。
 */
void
logicalrep_pa_worker_stop(ParallelApplyWorkerInfo *winfo)
{
	int			slot_no;
	uint16		generation;
	LogicalRepWorker *worker;

	SpinLockAcquire(&winfo->shared->mutex);
	generation = winfo->shared->logicalrep_worker_generation;
	slot_no = winfo->shared->logicalrep_worker_slot_no;
	SpinLockRelease(&winfo->shared->mutex);

	Assert(slot_no >= 0 && slot_no < max_logical_replication_workers);

	/*
	 * Detach from the error_mq_handle for the parallel apply worker before
	 * stopping it. This prevents the leader apply worker from trying to
	 * receive the message from the error queue that might already be detached
	 * by the parallel apply worker.
	 *
	 * 在停止并行 apply worker 之前，先脱离它的 error_mq_handle。这样
	 * leader apply worker 就不会再试图从可能已被并行 apply worker 拆掉的错
	 * 误队列里接收消息。
	 */
	if (winfo->error_mq_handle)
	{
		shm_mq_detach(winfo->error_mq_handle);
		winfo->error_mq_handle = NULL;
	}

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = &LogicalRepCtx->workers[slot_no];
	Assert(isParallelApplyWorker(worker));

	/*
	 * Only stop the worker if the generation matches and the worker is alive.
	 *
	 * 只有 generation 匹配并且 worker 仍然存活时才停止它。
	 */
	if (worker->generation == generation && worker->proc)
		logicalrep_worker_stop_internal(worker, SIGUSR2);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Wake up (using latch) any logical replication worker for specified sub/rel.
 *
 * 用 latch 唤醒指定 sub/rel 的任意逻辑复制 worker。
 */
void
logicalrep_worker_wakeup(Oid subid, Oid relid)
{
	LogicalRepWorker *worker;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	worker = logicalrep_worker_find(subid, relid, true);

	if (worker)
		logicalrep_worker_wakeup_ptr(worker);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Wake up (using latch) the specified logical replication worker.
 *
 * 用 latch 唤醒指定的逻辑复制 worker。
 *
 * Caller must hold lock, else worker->proc could change under us.
 *
 * 调用方必须持有锁，否则 worker 的 proc 可能在我们使用期间被改掉。
 */
void
logicalrep_worker_wakeup_ptr(LogicalRepWorker *worker)
{
	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	SetLatch(&worker->proc->procLatch);
}

/*
 * Attach to a slot.
 *
 * 挂上一个槽位。
 */
void
logicalrep_worker_attach(int slot)
{
	/* Block concurrent access.
	 *
	 * 阻止并发访问。
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	Assert(slot >= 0 && slot < max_logical_replication_workers);
	MyLogicalRepWorker = &LogicalRepCtx->workers[slot];

	if (!MyLogicalRepWorker->in_use)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication worker slot %d is empty, cannot attach",
						slot)));
	}

	if (MyLogicalRepWorker->proc)
	{
		LWLockRelease(LogicalRepWorkerLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("logical replication worker slot %d is already used by "
						"another worker, cannot attach", slot)));
	}

	MyLogicalRepWorker->proc = MyProc;
	before_shmem_exit(logicalrep_worker_onexit, (Datum) 0);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Stop the parallel apply workers if any, and detach the leader apply worker
 * (cleans up the worker info).
 *
 * 若有并行 apply worker 则停掉它们，并让 leader apply worker 脱离（清
 * 理 worker 信息）。
 */
static void
logicalrep_worker_detach(void)
{
	/* Stop the parallel apply workers.
	 *
	 * 停止并行 apply worker。
	 */
	if (am_leader_apply_worker())
	{
		List	   *workers;
		ListCell   *lc;

		/*
		 * Detach from the error_mq_handle for all parallel apply workers
		 * before terminating them. This prevents the leader apply worker from
		 * receiving the worker termination message and sending it to logs
		 * when the same is already done by the parallel worker.
		 *
		 * 在终止所有并行 apply worker 之前，先脱离它们的 error_mq_handle。这样
		 * leader apply worker 就不会收到 worker 终止消息并写到日志里，因为并行
		 * worker 自己已经做过同样的事。
		 */
		pa_detach_all_error_mq();

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

		workers = logicalrep_workers_find(MyLogicalRepWorker->subid, true, false);
		foreach(lc, workers)
		{
			LogicalRepWorker *w = (LogicalRepWorker *) lfirst(lc);

			if (isParallelApplyWorker(w))
				logicalrep_worker_stop_internal(w, SIGTERM);
		}

		LWLockRelease(LogicalRepWorkerLock);
	}

	/* Block concurrent access.
	 *
	 * 阻止并发访问。
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	logicalrep_worker_cleanup(MyLogicalRepWorker);

	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Clean up worker info.
 *
 * 清理 worker 信息。
 */
static void
logicalrep_worker_cleanup(LogicalRepWorker *worker)
{
	Assert(LWLockHeldByMeInMode(LogicalRepWorkerLock, LW_EXCLUSIVE));

	worker->type = WORKERTYPE_UNKNOWN;
	worker->in_use = false;
	worker->proc = NULL;
	worker->dbid = InvalidOid;
	worker->userid = InvalidOid;
	worker->subid = InvalidOid;
	worker->relid = InvalidOid;
	worker->leader_pid = InvalidPid;
	worker->parallel_apply = false;
}

/*
 * Cleanup function for logical replication launcher.
 *
 * 逻辑复制 launcher 的清理函数。
 *
 * Called on logical replication launcher exit.
 *
 * 在逻辑复制 launcher 退出时调用。
 */
static void
logicalrep_launcher_onexit(int code, Datum arg)
{
	LogicalRepCtx->launcher_pid = 0;
}

/*
 * Cleanup function.
 *
 * 清理函数。
 *
 * Called on logical replication worker exit.
 *
 * 在逻辑复制 worker 退出时调用。
 */
static void
logicalrep_worker_onexit(int code, Datum arg)
{
	/* Disconnect gracefully from the remote side.
	 *
	 * 优雅地断开与远程端的连接。
	 */
	if (LogRepWorkerWalRcvConn)
		walrcv_disconnect(LogRepWorkerWalRcvConn);

	logicalrep_worker_detach();

	/* Cleanup fileset used for streaming transactions.
	 *
	 * 清理流式事务使用的文件集。
	 */
	if (MyLogicalRepWorker->stream_fileset != NULL)
		FileSetDeleteAll(MyLogicalRepWorker->stream_fileset);

	/*
	 * Session level locks may be acquired outside of a transaction in
	 * parallel apply mode and will not be released when the worker
	 * terminates, so manually release all locks before the worker exits.
	 *
	 * 并行 apply 模式下，会话级锁可能在事务之外获取，worker 终止时不会自动
	 * 释放，因此退出前要手动释放全部锁。
	 *
	 * The locks will be acquired once the worker is initialized.
	 *
	 * worker 初始化之后才会获取这些锁。
	 */
	if (!InitializingApplyWorker)
		LockReleaseAll(DEFAULT_LOCKMETHOD, true);

	ApplyLauncherWakeup();
}

/*
 * Count the number of registered (not necessarily running) sync workers
 * for a subscription.
 *
 * 统计某个订阅已注册（未必正在运行）的同步 worker 数量。
 */
int
logicalrep_sync_worker_count(Oid subid)
{
	int			i;
	int			res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/* Search for attached worker for a given subscription id.
	 *
	 * 按给定的订阅 id 查找已挂上的 worker。
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (isTablesyncWorker(w) && w->subid == subid)
			res++;
	}

	return res;
}

/*
 * Count the number of registered (but not necessarily running) parallel apply
 * workers for a subscription.
 *
 * 统计某个订阅已注册（未必正在运行）的并行 apply worker 数量。
 */
static int
logicalrep_pa_worker_count(Oid subid)
{
	int			i;
	int			res = 0;

	Assert(LWLockHeldByMe(LogicalRepWorkerLock));

	/*
	 * Scan all attached parallel apply workers, only counting those which
	 * have the given subscription id.
	 *
	 * 扫描所有已挂上的并行 apply worker，只统计具有给定订阅 id 的那些。
	 */
	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (isParallelApplyWorker(w) && w->subid == subid)
			res++;
	}

	return res;
}

/*
 * ApplyLauncherShmemSize
 *		Compute space needed for replication launcher shared memory
 *
 * ApplyLauncherShmemSize：计算复制 launcher 所需的共享内存空间
 */
Size
ApplyLauncherShmemSize(void)
{
	Size		size;

	/*
	 * Need the fixed struct and the array of LogicalRepWorker.
	 *
	 * 需要固定结构体以及 LogicalRepWorker 数组。
	 */
	size = sizeof(LogicalRepCtxStruct);
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_logical_replication_workers,
								   sizeof(LogicalRepWorker)));
	return size;
}

/*
 * ApplyLauncherRegister
 *		Register a background worker running the logical replication launcher.
 *
 * ApplyLauncherRegister：注册一个运行逻辑复制 launcher 的后台 worker。
 */
void
ApplyLauncherRegister(void)
{
	BackgroundWorker bgw;

	/*
	 * The logical replication launcher is disabled during binary upgrades, to
	 * prevent logical replication workers from running on the source cluster.
	 * That could cause replication origins to move forward after having been
	 * copied to the target cluster, potentially creating conflicts with the
	 * copied data files.
	 *
	 * 二进制升级期间会禁用逻辑复制 launcher，以免逻辑复制 worker 在源集群
	 * 上运行。否则复制源可能在被复制到目标集群之后继续向前推进，从而与已复
	 * 制的数据文件产生冲突。
	 */
	if (max_logical_replication_workers == 0 || IsBinaryUpgrade)
		return;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, MAXPGPATH, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ApplyLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN,
			 "logical replication launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN,
			 "logical replication launcher");
	bgw.bgw_restart_time = 5;
	bgw.bgw_notify_pid = 0;
	bgw.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&bgw);
}

/*
 * ApplyLauncherShmemInit
 *		Allocate and initialize replication launcher shared memory
 *
 * ApplyLauncherShmemInit：分配并初始化复制 launcher 的共享内存
 */
void
ApplyLauncherShmemInit(void)
{
	bool		found;

	LogicalRepCtx = (LogicalRepCtxStruct *)
		ShmemInitStruct("Logical Replication Launcher Data",
						ApplyLauncherShmemSize(),
						&found);

	if (!found)
	{
		int			slot;

		memset(LogicalRepCtx, 0, ApplyLauncherShmemSize());

		LogicalRepCtx->last_start_dsa = DSA_HANDLE_INVALID;
		LogicalRepCtx->last_start_dsh = DSHASH_HANDLE_INVALID;

		/* Initialize memory and spin locks for each worker slot.
		 *
		 * 为每个 worker 槽位初始化内存和自旋锁。
		 */
		for (slot = 0; slot < max_logical_replication_workers; slot++)
		{
			LogicalRepWorker *worker = &LogicalRepCtx->workers[slot];

			memset(worker, 0, sizeof(LogicalRepWorker));
			SpinLockInit(&worker->relmutex);
		}
	}
}

/*
 * Initialize or attach to the dynamic shared hash table that stores the
 * last-start times, if not already done.
 * This must be called before accessing the table.
 *
 * 若尚未完成，则初始化或挂上保存最近启动时间的动态共享哈希表。访问该表
 * 之前必须先调用。
 */
static void
logicalrep_launcher_attach_dshmem(void)
{
	MemoryContext oldcontext;

	/* Quick exit if we already did this.
	 *
	 * 若已经做过，就直接返回。
	 */
	if (LogicalRepCtx->last_start_dsh != DSHASH_HANDLE_INVALID &&
		last_start_times != NULL)
		return;

	/* Otherwise, use a lock to ensure only one process creates the table.
	 *
	 * 否则加锁，确保只有一个进程创建该表。
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_EXCLUSIVE);

	/* Be sure any local memory allocated by DSA routines is persistent.
	 *
	 * 确保 DSA 例程分配的本地内存是持久的。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (LogicalRepCtx->last_start_dsh == DSHASH_HANDLE_INVALID)
	{
		/* Initialize dynamic shared hash table for last-start times.
		 *
		 * 初始化用于最近启动时间的动态共享哈希表。
		 */
		last_start_times_dsa = dsa_create(LWTRANCHE_LAUNCHER_DSA);
		dsa_pin(last_start_times_dsa);
		dsa_pin_mapping(last_start_times_dsa);
		last_start_times = dshash_create(last_start_times_dsa, &dsh_params, NULL);

		/* Store handles in shared memory for other backends to use.
		 *
		 * 把句柄存入共享内存，供其他后端使用。
		 */
		LogicalRepCtx->last_start_dsa = dsa_get_handle(last_start_times_dsa);
		LogicalRepCtx->last_start_dsh = dshash_get_hash_table_handle(last_start_times);
	}
	else if (!last_start_times)
	{
		/* Attach to existing dynamic shared hash table.
		 *
		 * 挂上已有的动态共享哈希表。
		 */
		last_start_times_dsa = dsa_attach(LogicalRepCtx->last_start_dsa);
		dsa_pin_mapping(last_start_times_dsa);
		last_start_times = dshash_attach(last_start_times_dsa, &dsh_params,
										 LogicalRepCtx->last_start_dsh, NULL);
	}

	MemoryContextSwitchTo(oldcontext);
	LWLockRelease(LogicalRepWorkerLock);
}

/*
 * Set the last-start time for the subscription.
 *
 * 设置该订阅的最近启动时间。
 */
static void
ApplyLauncherSetWorkerStartTime(Oid subid, TimestampTz start_time)
{
	LauncherLastStartTimesEntry *entry;
	bool		found;

	logicalrep_launcher_attach_dshmem();

	entry = dshash_find_or_insert(last_start_times, &subid, &found);
	entry->last_start_time = start_time;
	dshash_release_lock(last_start_times, entry);
}

/*
 * Return the last-start time for the subscription, or 0 if there isn't one.
 *
 * 返回该订阅的最近启动时间；若没有则返回 0。
 */
static TimestampTz
ApplyLauncherGetWorkerStartTime(Oid subid)
{
	LauncherLastStartTimesEntry *entry;
	TimestampTz ret;

	logicalrep_launcher_attach_dshmem();

	entry = dshash_find(last_start_times, &subid, false);
	if (entry == NULL)
		return 0;

	ret = entry->last_start_time;
	dshash_release_lock(last_start_times, entry);

	return ret;
}

/*
 * Remove the last-start-time entry for the subscription, if one exists.
 *
 * 若存在，则删除该订阅的最近启动时间项。
 *
 * This has two use-cases: to remove the entry related to a subscription
 * that's been deleted or disabled (just to avoid leaking shared memory),
 * and to allow immediate restart of an apply worker that has exited
 * due to subscription parameter changes.
 *
 * 有两种用途：删除已删除或已禁用订阅的项（避免共享内存泄漏）；以及让因
 * 订阅参数变化而退出的 apply worker 可以立即重启。
 */
void
ApplyLauncherForgetWorkerStartTime(Oid subid)
{
	logicalrep_launcher_attach_dshmem();

	(void) dshash_delete_key(last_start_times, &subid);
}

/*
 * Wakeup the launcher on commit if requested.
 *
 * 若已请求，则在提交时唤醒 launcher。
 */
void
AtEOXact_ApplyLauncher(bool isCommit)
{
	if (isCommit)
	{
		if (on_commit_launcher_wakeup)
			ApplyLauncherWakeup();
	}

	on_commit_launcher_wakeup = false;
}

/*
 * Request wakeup of the launcher on commit of the transaction.
 *
 * 请求在本事务提交时唤醒 launcher。
 *
 * This is used to send launcher signal to stop sleeping and process the
 * subscriptions when current transaction commits. Should be used when new
 * tuple was added to the pg_subscription catalog.
 *
 * 当前事务提交时，用它通知 launcher 停止睡眠并处理订阅。应在
 * pg_subscription 目录新增元组时使用。
*/
void
ApplyLauncherWakeupAtCommit(void)
{
	if (!on_commit_launcher_wakeup)
		on_commit_launcher_wakeup = true;
}

/*
 * 向 launcher 进程发送 SIGUSR1，把它从睡眠中唤醒。
 */
static void
ApplyLauncherWakeup(void)
{
	if (LogicalRepCtx->launcher_pid != 0)
		kill(LogicalRepCtx->launcher_pid, SIGUSR1);
}

/*
 * Main loop for the apply launcher process.
 *
 * apply launcher 进程的主循环。
 */
void
ApplyLauncherMain(Datum main_arg)
{
	ereport(DEBUG1,
			(errmsg_internal("logical replication launcher started")));

	before_shmem_exit(logicalrep_launcher_onexit, (Datum) 0);

	Assert(LogicalRepCtx->launcher_pid == 0);
	LogicalRepCtx->launcher_pid = MyProcPid;

	/* Establish signal handlers.
	 *
	 * 建立信号处理函数。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * Establish connection to nailed catalogs (we only ever access
	 * pg_subscription).
	 *
	 * 建立到钉住的系统目录的连接（我们只会访问 pg_subscription）。
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/* Enter main loop
	 *
	 * 进入主循环
	 */
	for (;;)
	{
		int			rc;
		List	   *sublist;
		ListCell   *lc;
		MemoryContext subctx;
		MemoryContext oldctx;
		long		wait_time = DEFAULT_NAPTIME_PER_CYCLE;

		CHECK_FOR_INTERRUPTS();

		/* Use temporary context to avoid leaking memory across cycles.
		 *
		 * 使用临时上下文，避免跨循环泄漏内存。
		 */
		subctx = AllocSetContextCreate(TopMemoryContext,
									   "Logical Replication Launcher sublist",
									   ALLOCSET_DEFAULT_SIZES);
		oldctx = MemoryContextSwitchTo(subctx);

		/* Start any missing workers for enabled subscriptions.
		 *
		 * 为已启用的订阅启动尚缺的 worker。
		 */
		sublist = get_subscription_list();
		foreach(lc, sublist)
		{
			Subscription *sub = (Subscription *) lfirst(lc);
			LogicalRepWorker *w;
			TimestampTz last_start;
			TimestampTz now;
			long		elapsed;

			if (!sub->enabled)
				continue;

			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
			w = logicalrep_worker_find(sub->oid, InvalidOid, false);
			LWLockRelease(LogicalRepWorkerLock);

			if (w != NULL)
				continue;		/* worker is running already
								 *
								 * worker 已经在运行
								 */

			/*
			 * If the worker is eligible to start now, launch it.  Otherwise,
			 * adjust wait_time so that we'll wake up as soon as it can be
			 * started.
			 *
			 * 若该 worker 现在可以启动，就启动它。否则调整 wait_time，以便它一旦可
			 * 以启动我们就醒来。
			 *
			 * Each subscription's apply worker can only be restarted once per
			 * wal_retrieve_retry_interval, so that errors do not cause us to
			 * repeatedly restart the worker as fast as possible.  In cases
			 * where a restart is expected (e.g., subscription parameter
			 * changes), another process should remove the last-start entry
			 * for the subscription so that the worker can be restarted
			 * without waiting for wal_retrieve_retry_interval to elapse.
			 *
			 * 每个订阅的 apply worker 在每个 wal_retrieve_retry_interval 内只能重
			 * 启一次，以免出错时我们以最快速度反复重启。若预期会重启（例如订阅参数
			 * 变化），另一个进程应删掉该订阅的最近启动时间项，这样不必等
			 * wal_retrieve_retry_interval 过去就能重启 worker。
			 */
			last_start = ApplyLauncherGetWorkerStartTime(sub->oid);
			now = GetCurrentTimestamp();
			if (last_start == 0 ||
				(elapsed = TimestampDifferenceMilliseconds(last_start, now)) >= wal_retrieve_retry_interval)
			{
				ApplyLauncherSetWorkerStartTime(sub->oid, now);
				if (!logicalrep_worker_launch(WORKERTYPE_APPLY,
											  sub->dbid, sub->oid, sub->name,
											  sub->owner, InvalidOid,
											  DSM_HANDLE_INVALID))
				{
					/*
					 * We get here either if we failed to launch a worker
					 * (perhaps for resource-exhaustion reasons) or if we
					 * launched one but it immediately quit.  Either way, it
					 * seems appropriate to try again after
					 * wal_retrieve_retry_interval.
					 *
					 * 走到这里，要么是启动 worker 失败（也许是资源耗尽），要么是启动后它立
					 * 刻退出了。无论哪种，都适合在 wal_retrieve_retry_interval 之后再试。
					 */
					wait_time = Min(wait_time,
									wal_retrieve_retry_interval);
				}
			}
			else
			{
				wait_time = Min(wait_time,
								wal_retrieve_retry_interval - elapsed);
			}
		}

		/* Switch back to original memory context.
		 *
		 * 切换回原来的内存上下文。
		 */
		MemoryContextSwitchTo(oldctx);
		/* Clean the temporary memory.
		 *
		 * 清理临时内存。
		 */
		MemoryContextDelete(subctx);

		/* Wait for more work.
		 *
		 * 等待更多工作。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   wait_time,
					   WAIT_EVENT_LOGICAL_LAUNCHER_MAIN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
	}

	/* Not reachable
	 *
	 * 不可到达
	 */
}

/*
 * Is current process the logical replication launcher?
 *
 * 当前进程是不是逻辑复制 launcher？
 */
bool
IsLogicalLauncher(void)
{
	return LogicalRepCtx->launcher_pid == MyProcPid;
}

/*
 * Return the pid of the leader apply worker if the given pid is the pid of a
 * parallel apply worker, otherwise, return InvalidPid.
 *
 * 若给定 pid 是某个并行 apply worker 的 pid，则返回其 leader apply
 * worker 的 pid，否则返回 InvalidPid。
 */
pid_t
GetLeaderApplyWorkerPid(pid_t pid)
{
	int			leader_pid = InvalidPid;
	int			i;

	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		LogicalRepWorker *w = &LogicalRepCtx->workers[i];

		if (isParallelApplyWorker(w) && w->proc && pid == w->proc->pid)
		{
			leader_pid = w->leader_pid;
			break;
		}
	}

	LWLockRelease(LogicalRepWorkerLock);

	return leader_pid;
}

/*
 * Returns state of the subscriptions.
 *
 * 返回各订阅的状态。
 */
Datum
pg_stat_get_subscription(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBSCRIPTION_COLS	10
	Oid			subid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	int			i;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	/* Make sure we get consistent view of the workers.
	 *
	 * 确保看到一致的 worker 视图。
	 */
	LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

	for (i = 0; i < max_logical_replication_workers; i++)
	{
		/* for each row
		 *
		 * 对每一行
		 */
		Datum		values[PG_STAT_GET_SUBSCRIPTION_COLS] = {0};
		bool		nulls[PG_STAT_GET_SUBSCRIPTION_COLS] = {0};
		int			worker_pid;
		LogicalRepWorker worker;

		memcpy(&worker, &LogicalRepCtx->workers[i],
			   sizeof(LogicalRepWorker));
		if (!worker.proc || !IsBackendPid(worker.proc->pid))
			continue;

		if (OidIsValid(subid) && worker.subid != subid)
			continue;

		worker_pid = worker.proc->pid;

		values[0] = ObjectIdGetDatum(worker.subid);
		if (isTablesyncWorker(&worker))
			values[1] = ObjectIdGetDatum(worker.relid);
		else
			nulls[1] = true;
		values[2] = Int32GetDatum(worker_pid);

		if (isParallelApplyWorker(&worker))
			values[3] = Int32GetDatum(worker.leader_pid);
		else
			nulls[3] = true;

		if (XLogRecPtrIsInvalid(worker.last_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(worker.last_lsn);
		if (worker.last_send_time == 0)
			nulls[5] = true;
		else
			values[5] = TimestampTzGetDatum(worker.last_send_time);
		if (worker.last_recv_time == 0)
			nulls[6] = true;
		else
			values[6] = TimestampTzGetDatum(worker.last_recv_time);
		if (XLogRecPtrIsInvalid(worker.reply_lsn))
			nulls[7] = true;
		else
			values[7] = LSNGetDatum(worker.reply_lsn);
		if (worker.reply_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(worker.reply_time);

		switch (worker.type)
		{
			case WORKERTYPE_APPLY:
				values[9] = CStringGetTextDatum("apply");
				break;
			case WORKERTYPE_PARALLEL_APPLY:
				values[9] = CStringGetTextDatum("parallel apply");
				break;
			case WORKERTYPE_TABLESYNC:
				values[9] = CStringGetTextDatum("table synchronization");
				break;
			case WORKERTYPE_UNKNOWN:
				/* Should never happen.
				 *
				 * 不应该发生。
				 */
				elog(ERROR, "unknown worker type");
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);

		/*
		 * If only a single subscription was requested, and we found it,
		 * break.
		 *
		 * 若只请求了一个订阅并且已经找到，就跳出循环。
		 */
		if (OidIsValid(subid))
			break;
	}

	LWLockRelease(LogicalRepWorkerLock);

	return (Datum) 0;
}
