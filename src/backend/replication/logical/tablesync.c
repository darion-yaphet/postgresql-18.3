/*-------------------------------------------------------------------------
 * tablesync.c
 *	  PostgreSQL logical replication: initial table data synchronization
 *
 * tablesync.c：PostgreSQL 逻辑复制中的表初始数据同步。
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/tablesync.c
 *
 * NOTES
 *	  This file contains code for initial table data synchronization for
 *	  logical replication.
 *
 * 说明：本文件包含逻辑复制的表初始数据同步代码。
 *
 *	  The initial data synchronization is done separately for each table,
 *	  in a separate apply worker that only fetches the initial snapshot data
 *	  from the publisher and then synchronizes the position in the stream with
 *	  the leader apply worker.
 *
 *	  每张表的初始数据同步单独进行，由一个单独的 apply worker 完成。它只从发布端拉取初始快照数据，然后与负责该流的主 apply worker 对齐流位置。
 *
 *	  There are several reasons for doing the synchronization this way:
 *	   - It allows us to parallelize the initial data synchronization
 *		 which lowers the time needed for it to happen.
 *	   - The initial synchronization does not have to hold the xid and LSN
 *		 for the time it takes to copy data of all tables, causing less
 *		 bloat and lower disk consumption compared to doing the
 *		 synchronization in a single process for the whole database.
 *	   - It allows us to synchronize any tables added after the initial
 *		 synchronization has finished.
 *
 *	  这样做有几个原因。可以并行做初始数据同步，缩短所需时间。初始同步不必在复制所有表的数据期间一直占着 xid 和 LSN，因此比用单个进程同步整个数据库产生的膨胀更少、磁盘占用更低。初始同步完成之后新增的表也可以再同步。
 *
 *	  The stream position synchronization works in multiple steps:
 *	   - Apply worker requests a tablesync worker to start, setting the new
 *		 table state to INIT.
 *	   - Tablesync worker starts; changes table state from INIT to DATASYNC while
 *		 copying.
 *	   - Tablesync worker does initial table copy; there is a FINISHEDCOPY (sync
 *		 worker specific) state to indicate when the copy phase has completed, so
 *		 if the worker crashes with this (non-memory) state then the copy will not
 *		 be re-attempted.
 *	   - Tablesync worker then sets table state to SYNCWAIT; waits for state change.
 *	   - Apply worker periodically checks for tables in SYNCWAIT state.  When
 *		 any appear, it sets the table state to CATCHUP and starts loop-waiting
 *		 until either the table state is set to SYNCDONE or the sync worker
 *		 exits.
 *	   - After the sync worker has seen the state change to CATCHUP, it will
 *		 read the stream and apply changes (acting like an apply worker) until
 *		 it catches up to the specified stream position.  Then it sets the
 *		 state to SYNCDONE.  There might be zero changes applied between
 *		 CATCHUP and SYNCDONE, because the sync worker might be ahead of the
 *		 apply worker.
 *	   - Once the state is set to SYNCDONE, the apply will continue tracking
 *		 the table until it reaches the SYNCDONE stream position, at which
 *		 point it sets state to READY and stops tracking.  Again, there might
 *		 be zero changes in between.
 *
 *	  流位置的同步分多步。apply worker 请求启动 tablesync worker，并把新表状态设为 INIT。tablesync worker 启动后，在拷贝期间把状态从 INIT 改为 DATASYNC。它做完初始表拷贝；FINISHEDCOPY 是同步 worker 专用状态，表示拷贝阶段已完成，这样如果 worker 带着这个已落盘的状态崩溃，就不会重新尝试拷贝。随后 tablesync worker 把表状态设为 SYNCWAIT，并等待状态变化。apply worker 定期检查处于 SYNCWAIT 的表。一旦出现，就把表状态设为 CATCHUP，并循环等待，直到表状态变成 SYNCDONE，或者同步 worker 退出。同步 worker 看到状态变为 CATCHUP 后，会读取流并应用变更（行为类似 apply worker），直到追上指定的流位置，然后把状态设为 SYNCDONE。CATCHUP 和 SYNCDONE 之间可能一条变更都没有应用，因为同步 worker 可能已经跑在 apply worker 前面。状态变为 SYNCDONE 之后，apply worker 会继续跟踪这张表，直到到达 SYNCDONE 对应的流位置，然后把状态设为 READY 并停止跟踪。这中间同样可能没有变更。
 *
 *	  So the state progression is always: INIT -> DATASYNC -> FINISHEDCOPY
 *	  -> SYNCWAIT -> CATCHUP -> SYNCDONE -> READY.
 *
 *	  因此状态始终按这个顺序前进：INIT、DATASYNC、FINISHEDCOPY、SYNCWAIT、CATCHUP、SYNCDONE、READY。
 *
 *	  The catalog pg_subscription_rel is used to keep information about
 *	  subscribed tables and their state.  The catalog holds all states
 *	  except SYNCWAIT and CATCHUP which are only in shared memory.
 *
 *	  系统表 pg_subscription_rel 用来保存被订阅的表及其状态。除了 SYNCWAIT 和 CATCHUP 只放在共享内存里，其余状态都记在这个系统表中。
 *
 *	  Example flows look like this:
 *	   - Apply is in front:
 *		  sync:8
 *			-> set in catalog FINISHEDCOPY
 *			-> set in memory SYNCWAIT
 *		  apply:10
 *			-> set in memory CATCHUP
 *			-> enter wait-loop
 *		  sync:10
 *			-> set in catalog SYNCDONE
 *			-> exit
 *		  apply:10
 *			-> exit wait-loop
 *			-> continue rep
 *		  apply:11
 *			-> set in catalog READY
 *
 *	  流程示例：apply 在前面时，同步 worker 在 LSN 8 把目录状态设为 FINISHEDCOPY，并在内存中设为 SYNCWAIT；apply 在 LSN 10 把内存状态设为 CATCHUP 并进入等待循环；同步 worker 在 LSN 10 把目录状态设为 SYNCDONE 后退出；apply 在 LSN 10 退出等待循环并继续复制；apply 在 LSN 11 把目录状态设为 READY。
 *
 *	   - Sync is in front:
 *		  sync:10
 *			-> set in catalog FINISHEDCOPY
 *			-> set in memory SYNCWAIT
 *		  apply:8
 *			-> set in memory CATCHUP
 *			-> continue per-table filtering
 *		  sync:10
 *			-> set in catalog SYNCDONE
 *			-> exit
 *		  apply:10
 *			-> set in catalog READY
 *			-> stop per-table filtering
 *			-> continue rep
 *
 *	   同步 worker 在前面时：同步 worker 在 LSN 10 把目录状态设为 FINISHEDCOPY，并在内存中设为 SYNCWAIT；apply 在 LSN 8 把内存状态设为 CATCHUP，并继续按表过滤；同步 worker 在 LSN 10 把目录状态设为 SYNCDONE 后退出；apply 在 LSN 10 把目录状态设为 READY，停止按表过滤，并继续复制。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_type.h"
#include "commands/copy.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "replication/logicallauncher.h"
#include "replication/logicalrelation.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/usercontext.h"

/*
 * 核心流程：
 * apply worker 发现待同步的表后调用 start_table_sync，把表状态设为 INIT 并启动 tablesync worker。
 * TablesyncWorkerMain 进入 LogicalRepSyncTableStart：创建临时复制槽，用 COPY 拉取初始数据，
 * 状态经过 DATASYNC、FINISHEDCOPY 进入 SYNCWAIT。
 * apply worker 在 process_syncing_tables_for_apply 里把 SYNCWAIT 改为 CATCHUP，并等待 SYNCDONE。
 * tablesync worker 在 CATCHUP 下继续应用变更，追上指定 LSN 后把状态设为 SYNCDONE 并退出。
 * apply worker 自己也应用到该 LSN 之后，表进入 READY。
 */

typedef enum
{
	SYNC_TABLE_STATE_NEEDS_REBUILD,
	SYNC_TABLE_STATE_REBUILD_STARTED,
	SYNC_TABLE_STATE_VALID,
} SyncingTablesState;

static SyncingTablesState table_states_validity = SYNC_TABLE_STATE_NEEDS_REBUILD;
static List *table_states_not_ready = NIL;
static bool FetchTableStates(bool *started_tx);

static StringInfo copybuf = NULL;

/*
 * Exit routine for synchronization worker.
 *
 * 同步 worker 的退出例程。
 */
pg_noreturn static void
finish_sync_worker(void)
{
	/*
	 * Commit any outstanding transaction. This is the usual case, unless
	 * there was nothing to do for the table.
	 *
	 * 提交尚未结束的事务。这是通常情况，除非这张表无事可做。
	 */
	if (IsTransactionState())
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/* And flush all writes.
	 *
	 * 并刷出全部写入。
	 */
	XLogFlush(GetXLogWriteRecPtr());

	StartTransactionCommand();
	ereport(LOG,
			(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has finished",
					MySubscription->name,
					get_rel_name(MyLogicalRepWorker->relid))));
	CommitTransactionCommand();

	/* Find the leader apply worker and signal it.
	 *
	 * 找到主 apply worker 并向它发信号。
	 */
	logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);

	/* Stop gracefully
	 *
	 * 优雅地停止。
	 */
	proc_exit(0);
}

/*
 * Wait until the relation sync state is set in the catalog to the expected
 * one; return true when it happens.
 *
 * 一直等到目录里的关系同步状态被设成期望值；发生时返回真。
 *
 * Returns false if the table sync worker or the table itself have
 * disappeared, or the table state has been reset.
 *
 * 如果表同步 worker 或表本身已经消失，或者表状态已被重置，则返回假。
 *
 * Currently, this is used in the apply worker when transitioning from
 * CATCHUP state to SYNCDONE.
 *
 * 目前 apply worker 从 CATCHUP 转到 SYNCDONE 时会用到它。
 */
static bool
wait_for_relation_state_change(Oid relid, char expected_state)
{
	char		state;

	for (;;)
	{
		LogicalRepWorker *worker;
		XLogRecPtr	statelsn;

		CHECK_FOR_INTERRUPTS();

		InvalidateCatalogSnapshot();
		state = GetSubscriptionRelState(MyLogicalRepWorker->subid,
										relid, &statelsn);

		if (state == SUBREL_STATE_UNKNOWN)
			break;

		if (state == expected_state)
			return true;

		/* Check if the sync worker is still running and bail if not.
		 *
		 * 检查同步 worker 是否仍在运行，否则放弃。
		 */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid, relid,
										false);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			break;

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 1000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		ResetLatch(MyLatch);
	}

	return false;
}

/*
 * Wait until the apply worker changes the state of our synchronization
 * worker to the expected one.
 *
 * 一直等到 apply worker 把我们这个同步 worker 的状态改成期望值。
 *
 * Used when transitioning from SYNCWAIT state to CATCHUP.
 *
 * 从 SYNCWAIT 转到 CATCHUP 时使用。
 *
 * Returns false if the apply worker has disappeared.
 *
 * 如果 apply worker 已经消失，则返回假。
 */
static bool
wait_for_worker_state_change(char expected_state)
{
	int			rc;

	for (;;)
	{
		LogicalRepWorker *worker;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Done if already in correct state.  (We assume this fetch is atomic
		 * enough to not give a misleading answer if we do it with no lock.)
		 *
		 * 如果已经是正确状态就结束。（假定这次读取足够原子，不加锁也不会得到误导性的结果。）
		 */
		if (MyLogicalRepWorker->relstate == expected_state)
			return true;

		/*
		 * Bail out if the apply worker has died, else signal it we're
		 * waiting.
		 *
		 * 如果 apply worker 已经死了就退出，否则通知它我们正在等待。
		 */
		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		worker = logicalrep_worker_find(MyLogicalRepWorker->subid,
										InvalidOid, false);
		if (worker && worker->proc)
			logicalrep_worker_wakeup_ptr(worker);
		LWLockRelease(LogicalRepWorkerLock);
		if (!worker)
			break;

		/*
		 * Wait.  We expect to get a latch signal back from the apply worker,
		 * but use a timeout in case it dies without sending one.
		 *
		 * 等待。预期会从 apply worker 收到闩锁信号，但仍然使用超时，以防它死掉时没有发信号。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   1000L, WAIT_EVENT_LOGICAL_SYNC_STATE_CHANGE);

		if (rc & WL_LATCH_SET)
			ResetLatch(MyLatch);
	}

	return false;
}

/*
 * Callback from syscache invalidation.
 *
 * 来自系统缓存失效的回调。
 */
void
invalidate_syncing_table_states(Datum arg, int cacheid, uint32 hashvalue)
{
	table_states_validity = SYNC_TABLE_STATE_NEEDS_REBUILD;
}

/*
 * Handle table synchronization cooperation from the synchronization
 * worker.
 *
 * 在同步 worker 一侧处理表同步协作。
 *
 * If the sync worker is in CATCHUP state and reached (or passed) the
 * predetermined synchronization point in the WAL stream, mark the table as
 * SYNCDONE and finish.
 *
 * 如果同步 worker 处于 CATCHUP，并且已经到达或超过 WAL 流中预定的同步点，就把表标为 SYNCDONE 并结束。
 */
static void
process_syncing_tables_for_sync(XLogRecPtr current_lsn)
{
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_CATCHUP &&
		current_lsn >= MyLogicalRepWorker->relstate_lsn)
	{
		TimeLineID	tli;
		char		syncslotname[NAMEDATALEN] = {0};
		char		originname[NAMEDATALEN] = {0};

		MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCDONE;
		MyLogicalRepWorker->relstate_lsn = current_lsn;

		SpinLockRelease(&MyLogicalRepWorker->relmutex);

		/*
		 * UpdateSubscriptionRelState must be called within a transaction.
		 *
		 * UpdateSubscriptionRelState 必须在事务中调用。
		 */
		if (!IsTransactionState())
			StartTransactionCommand();

		UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
								   MyLogicalRepWorker->relid,
								   MyLogicalRepWorker->relstate,
								   MyLogicalRepWorker->relstate_lsn,
								   false);

		/*
		 * End streaming so that LogRepWorkerWalRcvConn can be used to drop
		 * the slot.
		 *
		 * 结束流式传输，以便用 LogRepWorkerWalRcvConn 删除复制槽。
		 */
		walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);

		/*
		 * Cleanup the tablesync slot.
		 *
		 * 清理 tablesync 复制槽。
		 *
		 * This has to be done after updating the state because otherwise if
		 * there is an error while doing the database operations we won't be
		 * able to rollback dropped slot.
		 *
		 * 必须在更新状态之后做这件事。否则如果做数据库操作时出错，已经删除的槽就无法回滚。
		 */
		ReplicationSlotNameForTablesync(MyLogicalRepWorker->subid,
										MyLogicalRepWorker->relid,
										syncslotname,
										sizeof(syncslotname));

		/*
		 * It is important to give an error if we are unable to drop the slot,
		 * otherwise, it won't be dropped till the corresponding subscription
		 * is dropped. So passing missing_ok = false.
		 *
		 * 如果删不掉槽，必须报错，否则这个槽要一直留到对应订阅被删除。因此 missing_ok 传假。
		 */
		ReplicationSlotDropAtPubNode(LogRepWorkerWalRcvConn, syncslotname, false);

		CommitTransactionCommand();
		pgstat_report_stat(false);

		/*
		 * Start a new transaction to clean up the tablesync origin tracking.
		 * This transaction will be ended within the finish_sync_worker().
		 * Now, even, if we fail to remove this here, the apply worker will
		 * ensure to clean it up afterward.
		 *
		 * 开启一个新事务来清理 tablesync 的复制源跟踪。这个事务会在 finish_sync_worker() 里结束。即使这里删除失败，apply worker 之后也会负责清理。
		 *
		 * We need to do this after the table state is set to SYNCDONE.
		 * Otherwise, if an error occurs while performing the database
		 * operation, the worker will be restarted and the in-memory state of
		 * replication progress (remote_lsn) won't be rolled-back which would
		 * have been cleared before restart. So, the restarted worker will use
		 * invalid replication progress state resulting in replay of
		 * transactions that have already been applied.
		 *
		 * 必须在表状态设为 SYNCDONE 之后做这件事。否则如果执行数据库操作时出错，worker 会重启，而重启前已经被清掉的内存中复制进度 remote_lsn 不会回滚。重启后的 worker 会使用无效的复制进度，把已经应用过的事务再重放一遍。
		 */
		StartTransactionCommand();

		ReplicationOriginNameForLogicalRep(MyLogicalRepWorker->subid,
										   MyLogicalRepWorker->relid,
										   originname,
										   sizeof(originname));

		/*
		 * Resetting the origin session removes the ownership of the slot.
		 * This is needed to allow the origin to be dropped.
		 *
		 * 重置复制源会话会去掉对槽的占用。这样才能删除该复制源。
		 */
		replorigin_session_reset();
		replorigin_session_origin = InvalidRepOriginId;
		replorigin_session_origin_lsn = InvalidXLogRecPtr;
		replorigin_session_origin_timestamp = 0;

		/*
		 * Drop the tablesync's origin tracking if exists.
		 *
		 * 如果存在 tablesync 的复制源跟踪，就把它删掉。
		 *
		 * There is a chance that the user is concurrently performing refresh
		 * for the subscription where we remove the table state and its origin
		 * or the apply worker would have removed this origin. So passing
		 * missing_ok = true.
		 *
		 * 用户可能正在并发刷新订阅，那时会删掉表状态及其复制源，或者 apply worker 已经删过这个复制源。因此 missing_ok 传真。
		 */
		replorigin_drop_by_name(originname, true, false);

		finish_sync_worker();
	}
	else
		SpinLockRelease(&MyLogicalRepWorker->relmutex);
}

/*
 * Handle table synchronization cooperation from the apply worker.
 *
 * 在 apply worker 一侧处理表同步协作。
 *
 * Walk over all subscription tables that are individually tracked by the
 * apply process (currently, all that have state other than
 * SUBREL_STATE_READY) and manage synchronization for them.
 *
 * 遍历 apply 进程单独跟踪的全部订阅表（目前是所有状态不是 SUBREL_STATE_READY 的表），并管理它们的同步。
 *
 * If there are tables that need synchronizing and are not being synchronized
 * yet, start sync workers for them (if there are free slots for sync
 * workers).  To prevent starting the sync worker for the same relation at a
 * high frequency after a failure, we store its last start time with each sync
 * state info.  We start the sync worker for the same relation after waiting
 * at least wal_retrieve_retry_interval.
 *
 * 如果有表需要同步但还没开始同步，就为它们启动同步 worker（在还有空闲同步 worker 槽位时）。为了避免失败后对同一张关系过于频繁地启动同步 worker，我们在每个同步状态信息里记下上次启动时间。至少等待 wal_retrieve_retry_interval 之后，才会再次为同一张关系启动同步 worker。
 *
 * For tables that are being synchronized already, check if sync workers
 * either need action from the apply worker or have finished.  This is the
 * SYNCWAIT to CATCHUP transition.
 *
 * 对已经在同步的表，检查同步 worker 是需要 apply worker 采取行动，还是已经结束。这就是从 SYNCWAIT 到 CATCHUP 的转换。
 *
 * If the synchronization position is reached (SYNCDONE), then the table can
 * be marked as READY and is no longer tracked.
 *
 * 如果已经到达同步位置（SYNCDONE），就可以把表标为 READY，不再跟踪它。
 */
static void
process_syncing_tables_for_apply(XLogRecPtr current_lsn)
{
	struct tablesync_start_time_mapping
	{
		Oid			relid;
		TimestampTz last_start_time;
	};
	static HTAB *last_start_times = NULL;
	ListCell   *lc;
	bool		started_tx = false;
	bool		should_exit = false;
	Relation	rel = NULL;

	Assert(!IsTransactionState());

	/* We need up-to-date sync state info for subscription tables here.
	 *
	 * 这里需要订阅表的最新同步状态信息。
	 */
	FetchTableStates(&started_tx);

	/*
	 * Prepare a hash table for tracking last start times of workers, to avoid
	 * immediate restarts.  We don't need it if there are no tables that need
	 * syncing.
	 *
	 * 准备一张哈希表，记录 worker 的上次启动时间，避免立刻重启。如果没有需要同步的表，就不需要它。
	 */
	if (table_states_not_ready != NIL && !last_start_times)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(struct tablesync_start_time_mapping);
		last_start_times = hash_create("Logical replication table sync worker start times",
									   256, &ctl, HASH_ELEM | HASH_BLOBS);
	}

	/*
	 * Clean up the hash table when we're done with all tables (just to
	 * release the bit of memory).
	 *
	 * 所有表都处理完后清理这张哈希表，只是为了释放这一点内存。
	 */
	else if (table_states_not_ready == NIL && last_start_times)
	{
		hash_destroy(last_start_times);
		last_start_times = NULL;
	}

	/*
	 * Process all tables that are being synchronized.
	 *
	 * 处理所有正在同步的表。
	 */
	foreach(lc, table_states_not_ready)
	{
		SubscriptionRelState *rstate = (SubscriptionRelState *) lfirst(lc);

		if (rstate->state == SUBREL_STATE_SYNCDONE)
		{
			/*
			 * Apply has caught up to the position where the table sync has
			 * finished.  Mark the table as ready so that the apply will just
			 * continue to replicate it normally.
			 *
			 * apply 已经追上表同步结束的位置。把表标为就绪，此后 apply 就按正常方式继续复制它。
			 */
			if (current_lsn >= rstate->lsn)
			{
				char		originname[NAMEDATALEN];

				rstate->state = SUBREL_STATE_READY;
				rstate->lsn = current_lsn;
				if (!started_tx)
				{
					StartTransactionCommand();
					started_tx = true;
				}

				/*
				 * Remove the tablesync origin tracking if exists.
				 *
				 * 如果存在 tablesync 的复制源跟踪，就把它去掉。
				 *
				 * There is a chance that the user is concurrently performing
				 * refresh for the subscription where we remove the table
				 * state and its origin or the tablesync worker would have
				 * already removed this origin. We can't rely on tablesync
				 * worker to remove the origin tracking as if there is any
				 * error while dropping we won't restart it to drop the
				 * origin. So passing missing_ok = true.
				 *
				 * 用户可能正在并发刷新订阅，那时会删掉表状态及其复制源，或者 tablesync worker 已经去掉了这个复制源。不能依赖 tablesync worker 来删除复制源跟踪，因为删除出错时不会再重启它来删。因此 missing_ok 传真。
				 *
				 * Lock the subscription and origin in the same order as we
				 * are doing during DDL commands to avoid deadlocks. See
				 * AlterSubscription_refresh.
				 *
				 * 按 DDL 命令中的同样顺序锁定订阅和复制源，以避免死锁。参见 AlterSubscription_refresh。
				 */
				LockSharedObject(SubscriptionRelationId, MyLogicalRepWorker->subid,
								 0, AccessShareLock);

				if (!rel)
					rel = table_open(SubscriptionRelRelationId, RowExclusiveLock);

				ReplicationOriginNameForLogicalRep(MyLogicalRepWorker->subid,
												   rstate->relid,
												   originname,
												   sizeof(originname));
				replorigin_drop_by_name(originname, true, false);

				/*
				 * Update the state to READY only after the origin cleanup.
				 *
				 * 只有在清理复制源之后，才把状态更新为 READY。
				 */
				UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
										   rstate->relid, rstate->state,
										   rstate->lsn, true);
			}
		}
		else
		{
			LogicalRepWorker *syncworker;

			/*
			 * Look for a sync worker for this relation.
			 *
			 * 寻找这张关系的同步 worker。
			 */
			LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);

			syncworker = logicalrep_worker_find(MyLogicalRepWorker->subid,
												rstate->relid, false);

			if (syncworker)
			{
				/* Found one, update our copy of its state
				 *
				 * 找到了，更新我们这边保存的它的状态。
				 */
				SpinLockAcquire(&syncworker->relmutex);
				rstate->state = syncworker->relstate;
				rstate->lsn = syncworker->relstate_lsn;
				if (rstate->state == SUBREL_STATE_SYNCWAIT)
				{
					/*
					 * Sync worker is waiting for apply.  Tell sync worker it
					 * can catchup now.
					 *
					 * 同步 worker 正在等待 apply。告诉它可以开始追赶了。
					 */
					syncworker->relstate = SUBREL_STATE_CATCHUP;
					syncworker->relstate_lsn =
						Max(syncworker->relstate_lsn, current_lsn);
				}
				SpinLockRelease(&syncworker->relmutex);

				/* If we told worker to catch up, wait for it.
				 *
				 * 如果已经告诉 worker 去追赶，就等待它。
				 */
				if (rstate->state == SUBREL_STATE_SYNCWAIT)
				{
					/* Signal the sync worker, as it may be waiting for us.
					 *
					 * 向同步 worker 发信号，因为它可能正在等我们。
					 */
					if (syncworker->proc)
						logicalrep_worker_wakeup_ptr(syncworker);

					/* Now safe to release the LWLock
					 *
					 * 现在可以安全地释放 LWLock。
					 */
					LWLockRelease(LogicalRepWorkerLock);

					if (started_tx)
					{
						/*
						 * We must commit the existing transaction to release
						 * the existing locks before entering a busy loop.
						 * This is required to avoid any undetected deadlocks
						 * due to any existing lock as deadlock detector won't
						 * be able to detect the waits on the latch.
						 *
						 * 进入忙等之前必须提交现有事务，以释放已有的锁。否则已有的锁可能导致检测不到的死锁，死锁检测器无法发现在闩锁上的等待。
						 *
						 * Also close any tables prior to the commit.
						 *
						 * 提交之前还要关闭所有已打开的表。
						 */
						if (rel)
						{
							table_close(rel, NoLock);
							rel = NULL;
						}
						CommitTransactionCommand();
						pgstat_report_stat(false);
					}

					/*
					 * Enter busy loop and wait for synchronization worker to
					 * reach expected state (or die trying).
					 *
					 * 进入忙等，等待同步 worker 到达期望状态，或者在尝试中死去。
					 */
					StartTransactionCommand();
					started_tx = true;

					wait_for_relation_state_change(rstate->relid,
												   SUBREL_STATE_SYNCDONE);
				}
				else
					LWLockRelease(LogicalRepWorkerLock);
			}
			else
			{
				/*
				 * If there is no sync worker for this table yet, count
				 * running sync workers for this subscription, while we have
				 * the lock.
				 *
				 * 如果这张表还没有同步 worker，就在持有锁时统计本订阅正在运行的同步 worker 数量。
				 */
				int			nsyncworkers =
					logicalrep_sync_worker_count(MyLogicalRepWorker->subid);

				/* Now safe to release the LWLock
				 *
				 * 现在可以安全地释放 LWLock。
				 */
				LWLockRelease(LogicalRepWorkerLock);

				/*
				 * If there are free sync worker slot(s), start a new sync
				 * worker for the table.
				 *
				 * 如果还有空闲的同步 worker 槽位，就为这张表启动一个新的同步 worker。
				 */
				if (nsyncworkers < max_sync_workers_per_subscription)
				{
					TimestampTz now = GetCurrentTimestamp();
					struct tablesync_start_time_mapping *hentry;
					bool		found;

					hentry = hash_search(last_start_times, &rstate->relid,
										 HASH_ENTER, &found);

					if (!found ||
						TimestampDifferenceExceeds(hentry->last_start_time, now,
												   wal_retrieve_retry_interval))
					{
						/*
						 * Set the last_start_time even if we fail to start
						 * the worker, so that we won't retry until
						 * wal_retrieve_retry_interval has elapsed.
						 *
						 * 即使启动 worker 失败，也设置 last_start_time，这样在 wal_retrieve_retry_interval 过去之前不会重试。
						 */
						hentry->last_start_time = now;
						(void) logicalrep_worker_launch(WORKERTYPE_TABLESYNC,
														MyLogicalRepWorker->dbid,
														MySubscription->oid,
														MySubscription->name,
														MyLogicalRepWorker->userid,
														rstate->relid,
														DSM_HANDLE_INVALID);
					}
				}
			}
		}
	}

	/* Close table if opened
	 *
	 * 如果表已打开，就关闭它。
	 */
	if (rel)
		table_close(rel, NoLock);


	if (started_tx)
	{
		/*
		 * Even when the two_phase mode is requested by the user, it remains
		 * as 'pending' until all tablesyncs have reached READY state.
		 *
		 * 即使用户请求了 two_phase 模式，在所有 tablesync 都到达 READY 之前，它仍然保持 pending。
		 *
		 * When this happens, we restart the apply worker and (if the
		 * conditions are still ok) then the two_phase tri-state will become
		 * 'enabled' at that time.
		 *
		 * 发生这种情况时，我们重启 apply worker；如果条件仍然满足，那时 two_phase 的三态会变成 enabled。
		 *
		 * Note: If the subscription has no tables then leave the state as
		 * PENDING, which allows ALTER SUBSCRIPTION ... REFRESH PUBLICATION to
		 * work.
		 *
		 * 注意：如果订阅没有表，就把状态留在 PENDING，这样 ALTER SUBSCRIPTION ... REFRESH PUBLICATION 仍然可以工作。
		 */
		if (MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING)
		{
			CommandCounterIncrement();	/* make updates visible
			 *
			 * 让更新对别人可见。
			 */
			if (AllTablesyncsReady())
			{
				ereport(LOG,
						(errmsg("logical replication apply worker for subscription \"%s\" will restart so that two_phase can be enabled",
								MySubscription->name)));
				should_exit = true;
			}
		}

		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	if (should_exit)
	{
		/*
		 * Reset the last-start time for this worker so that the launcher will
		 * restart it without waiting for wal_retrieve_retry_interval.
		 *
		 * 重置这个 worker 的上次启动时间，这样启动器不必等待 wal_retrieve_retry_interval 就会重启它。
		 */
		ApplyLauncherForgetWorkerStartTime(MySubscription->oid);

		proc_exit(0);
	}
}

/*
 * Process possible state change(s) of tables that are being synchronized.
 *
 * 处理正在同步的表可能发生的状态变化。
 */
void
process_syncing_tables(XLogRecPtr current_lsn)
{
	switch (MyLogicalRepWorker->type)
	{
		case WORKERTYPE_PARALLEL_APPLY:

			/*
			 * Skip for parallel apply workers because they only operate on
			 * tables that are in a READY state. See pa_can_start() and
			 * should_apply_changes_for_rel().
			 *
			 * 并行 apply worker 要跳过，因为它们只处理处于 READY 状态的表。参见 pa_can_start() 和 should_apply_changes_for_rel()。
			 */
			break;

		case WORKERTYPE_TABLESYNC:
			process_syncing_tables_for_sync(current_lsn);
			break;

		case WORKERTYPE_APPLY:
			process_syncing_tables_for_apply(current_lsn);
			break;

		case WORKERTYPE_UNKNOWN:
			/* Should never happen.
			 *
			 * 绝不应该发生。
			 */
			elog(ERROR, "Unknown worker type");
	}
}

/*
 * Create list of columns for COPY based on logical relation mapping.
 *
 * 根据逻辑关系映射，为 COPY 建立列名单。
 */
static List *
make_copy_attnamelist(LogicalRepRelMapEntry *rel)
{
	List	   *attnamelist = NIL;
	int			i;

	for (i = 0; i < rel->remoterel.natts; i++)
	{
		attnamelist = lappend(attnamelist,
							  makeString(rel->remoterel.attnames[i]));
	}


	return attnamelist;
}

/*
 * Data source callback for the COPY FROM, which reads from the remote
 * connection and passes the data back to our local COPY.
 *
 * COPY FROM 的数据源回调：从远程连接读取数据，再交回本地的 COPY。
 */
static int
copy_read_data(void *outbuf, int minread, int maxread)
{
	int			bytesread = 0;
	int			avail;

	/* If there are some leftover data from previous read, use it.
	 *
	 * 如果上次读取还有剩余数据，就先用它。
	 */
	avail = copybuf->len - copybuf->cursor;
	if (avail)
	{
		if (avail > maxread)
			avail = maxread;
		memcpy(outbuf, &copybuf->data[copybuf->cursor], avail);
		copybuf->cursor += avail;
		maxread -= avail;
		bytesread += avail;
	}

	while (maxread > 0 && bytesread < minread)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			len;
		char	   *buf = NULL;

		for (;;)
		{
			/* Try read the data.
			 *
			 * 尝试读取数据。
			 */
			len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);

			CHECK_FOR_INTERRUPTS();

			if (len == 0)
				break;
			else if (len < 0)
				return bytesread;
			else
			{
				/* Process the data
				 *
				 * 处理这些数据。
				 */
				copybuf->data = buf;
				copybuf->len = len;
				copybuf->cursor = 0;

				avail = copybuf->len - copybuf->cursor;
				if (avail > maxread)
					avail = maxread;
				memcpy(outbuf, &copybuf->data[copybuf->cursor], avail);
				outbuf = (char *) outbuf + avail;
				copybuf->cursor += avail;
				maxread -= avail;
				bytesread += avail;
			}

			if (maxread <= 0 || bytesread >= minread)
				return bytesread;
		}

		/*
		 * Wait for more data or latch.
		 *
		 * 等待更多数据或闩锁。
		 */
		(void) WaitLatchOrSocket(MyLatch,
								 WL_SOCKET_READABLE | WL_LATCH_SET |
								 WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 fd, 1000L, WAIT_EVENT_LOGICAL_SYNC_DATA);

		ResetLatch(MyLatch);
	}

	return bytesread;
}


/*
 * Get information about remote relation in similar fashion the RELATION
 * message provides during replication.
 *
 * 获取远程关系的信息，方式与复制期间 RELATION 消息所提供的类似。
 *
 * This function also returns (a) the relation qualifications to be used in
 * the COPY command, and (b) whether the remote relation has published any
 * generated column.
 *
 * 本函数还返回两项内容：COPY 命令要使用的关系限定条件，以及远程关系是否发布了任何生成列。
 */
static void
fetch_remote_table_info(char *nspname, char *relname, LogicalRepRelation *lrel,
						List **qual, bool *gencol_published)
{
	WalRcvExecResult *res;
	StringInfoData cmd;
	TupleTableSlot *slot;
	Oid			tableRow[] = {OIDOID, CHAROID, CHAROID};
	Oid			attrRow[] = {INT2OID, TEXTOID, OIDOID, BOOLOID, BOOLOID};
	Oid			qualRow[] = {TEXTOID};
	bool		isnull;
	int			natt;
	StringInfo	pub_names = NULL;
	Bitmapset  *included_cols = NULL;
	int			server_version = walrcv_server_version(LogRepWorkerWalRcvConn);

	lrel->nspname = nspname;
	lrel->relname = relname;

	/* First fetch Oid and replica identity.
	 *
	 * 首先取得 Oid 和副本标识。
	 */
	initStringInfo(&cmd);
	appendStringInfo(&cmd, "SELECT c.oid, c.relreplident, c.relkind"
					 "  FROM pg_catalog.pg_class c"
					 "  INNER JOIN pg_catalog.pg_namespace n"
					 "        ON (c.relnamespace = n.oid)"
					 " WHERE n.nspname = %s"
					 "   AND c.relname = %s",
					 quote_literal_cstr(nspname),
					 quote_literal_cstr(relname));
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  lengthof(tableRow), tableRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("table \"%s.%s\" not found on publisher",
						nspname, relname)));

	lrel->remoteid = DatumGetObjectId(slot_getattr(slot, 1, &isnull));
	Assert(!isnull);
	lrel->replident = DatumGetChar(slot_getattr(slot, 2, &isnull));
	Assert(!isnull);
	lrel->relkind = DatumGetChar(slot_getattr(slot, 3, &isnull));
	Assert(!isnull);

	ExecDropSingleTupleTableSlot(slot);
	walrcv_clear_result(res);


	/*
	 * Get column lists for each relation.
	 *
	 * 取得每张关系的列名单。
	 *
	 * We need to do this before fetching info about column names and types,
	 * so that we can skip columns that should not be replicated.
	 *
	 * 必须在获取列名和类型信息之前做这件事，这样才能跳过不应复制的列。
	 */
	if (server_version >= 150000)
	{
		WalRcvExecResult *pubres;
		TupleTableSlot *tslot;
		Oid			attrsRow[] = {INT2VECTOROID};

		/* Build the pub_names comma-separated string.
		 *
		 * 把 pub_names 拼成逗号分隔的字符串。
		 */
		pub_names = makeStringInfo();
		GetPublicationsStr(MySubscription->publications, pub_names, true);

		/*
		 * Fetch info about column lists for the relation (from all the
		 * publications).
		 *
		 * 取得该关系的列名单信息（来自所有发布）。
		 */
		resetStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "SELECT DISTINCT"
						 "  (CASE WHEN (array_length(gpt.attrs, 1) = c.relnatts)"
						 "   THEN NULL ELSE gpt.attrs END)"
						 "  FROM pg_publication p,"
						 "  LATERAL pg_get_publication_tables(p.pubname) gpt,"
						 "  pg_class c"
						 " WHERE gpt.relid = %u AND c.oid = gpt.relid"
						 "   AND p.pubname IN ( %s )",
						 lrel->remoteid,
						 pub_names->data);

		pubres = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
							 lengthof(attrsRow), attrsRow);

		if (pubres->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not fetch column list info for table \"%s.%s\" from publisher: %s",
							nspname, relname, pubres->err)));

		/*
		 * We don't support the case where the column list is different for
		 * the same table when combining publications. See comments atop
		 * fetch_table_list. So there should be only one row returned.
		 * Although we already checked this when creating the subscription, we
		 * still need to check here in case the column list was changed after
		 * creating the subscription and before the sync worker is started.
		 *
		 * 合并发布时，不支持同一张表的列名单不一致。参见 fetch_table_list 上方的注释。因此应该只返回一行。创建订阅时已经检查过，但同步 worker 启动前列名单可能被改过，所以这里仍要检查。
		 */
		if (tuplestore_tuple_count(pubres->tuplestore) > 1)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use different column lists for table \"%s.%s\" in different publications",
						   nspname, relname));

		/*
		 * Get the column list and build a single bitmap with the attnums.
		 *
		 * 取得列名单，并用属性号建成一张位图。
		 *
		 * If we find a NULL value, it means all the columns should be
		 * replicated.
		 *
		 * 如果发现 NULL，表示所有列都应该复制。
		 */
		tslot = MakeSingleTupleTableSlot(pubres->tupledesc, &TTSOpsMinimalTuple);
		if (tuplestore_gettupleslot(pubres->tuplestore, true, false, tslot))
		{
			Datum		cfval = slot_getattr(tslot, 1, &isnull);

			if (!isnull)
			{
				ArrayType  *arr;
				int			nelems;
				int16	   *elems;

				arr = DatumGetArrayTypeP(cfval);
				nelems = ARR_DIMS(arr)[0];
				elems = (int16 *) ARR_DATA_PTR(arr);

				for (natt = 0; natt < nelems; natt++)
					included_cols = bms_add_member(included_cols, elems[natt]);
			}

			ExecClearTuple(tslot);
		}
		ExecDropSingleTupleTableSlot(tslot);

		walrcv_clear_result(pubres);
	}

	/*
	 * Now fetch column names and types.
	 *
	 * 现在获取列名和类型。
	 */
	resetStringInfo(&cmd);
	appendStringInfoString(&cmd,
						   "SELECT a.attnum,"
						   "       a.attname,"
						   "       a.atttypid,"
						   "       a.attnum = ANY(i.indkey)");

	/* Generated columns can be replicated since version 18.
	 *
	 * 从版本 18 起可以复制生成列。
	 */
	if (server_version >= 180000)
		appendStringInfoString(&cmd, ", a.attgenerated != ''");

	appendStringInfo(&cmd,
					 "  FROM pg_catalog.pg_attribute a"
					 "  LEFT JOIN pg_catalog.pg_index i"
					 "       ON (i.indexrelid = pg_get_replica_identity_index(%u))"
					 " WHERE a.attnum > 0::pg_catalog.int2"
					 "   AND NOT a.attisdropped %s"
					 "   AND a.attrelid = %u"
					 " ORDER BY a.attnum",
					 lrel->remoteid,
					 (server_version >= 120000 && server_version < 180000 ?
					  "AND a.attgenerated = ''" : ""),
					 lrel->remoteid);
	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data,
					  server_version >= 180000 ? lengthof(attrRow) : lengthof(attrRow) - 1, attrRow);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not fetch table info for table \"%s.%s\" from publisher: %s",
						nspname, relname, res->err)));

	/* We don't know the number of rows coming, so allocate enough space.
	 *
	 * 不知道会来多少行，所以分配足够的空间。
	 */
	lrel->attnames = palloc0(MaxTupleAttributeNumber * sizeof(char *));
	lrel->atttyps = palloc0(MaxTupleAttributeNumber * sizeof(Oid));
	lrel->attkeys = NULL;

	/*
	 * Store the columns as a list of names.  Ignore those that are not
	 * present in the column list, if there is one.
	 *
	 * 把列存成名字列表。如果有列名单，就忽略不在名单里的列。
	 */
	natt = 0;
	slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
	{
		char	   *rel_colname;
		AttrNumber	attnum;

		attnum = DatumGetInt16(slot_getattr(slot, 1, &isnull));
		Assert(!isnull);

		/* If the column is not in the column list, skip it.
		 *
		 * 如果该列不在列名单中，就跳过。
		 */
		if (included_cols != NULL && !bms_is_member(attnum, included_cols))
		{
			ExecClearTuple(slot);
			continue;
		}

		rel_colname = TextDatumGetCString(slot_getattr(slot, 2, &isnull));
		Assert(!isnull);

		lrel->attnames[natt] = rel_colname;
		lrel->atttyps[natt] = DatumGetObjectId(slot_getattr(slot, 3, &isnull));
		Assert(!isnull);

		if (DatumGetBool(slot_getattr(slot, 4, &isnull)))
			lrel->attkeys = bms_add_member(lrel->attkeys, natt);

		/* Remember if the remote table has published any generated column.
		 *
		 * 记住远程表是否发布了任何生成列。
		 */
		if (server_version >= 180000 && !(*gencol_published))
		{
			*gencol_published = DatumGetBool(slot_getattr(slot, 5, &isnull));
			Assert(!isnull);
		}

		/* Should never happen.
		 *
		 * 绝不应该发生。
		 */
		if (++natt >= MaxTupleAttributeNumber)
			elog(ERROR, "too many columns in remote table \"%s.%s\"",
				 nspname, relname);

		ExecClearTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);

	lrel->natts = natt;

	walrcv_clear_result(res);

	/*
	 * Get relation's row filter expressions. DISTINCT avoids the same
	 * expression of a table in multiple publications from being included
	 * multiple times in the final expression.
	 *
	 * 取得关系的行过滤表达式。DISTINCT 避免同一张表在多个发布里的相同表达式被重复放进最终表达式。
	 *
	 * We need to copy the row even if it matches just one of the
	 * publications, so we later combine all the quals with OR.
	 *
	 * 只要行匹配其中一个发布就要复制，所以稍后用 OR 把所有限定条件合起来。
	 *
	 * For initial synchronization, row filtering can be ignored in following
	 * cases:
	 *
	 * 初始同步时，下列情况可以忽略行过滤：
	 *
	 * 1) one of the subscribed publications for the table hasn't specified
	 * any row filter
	 *
	 * 1) 该表所订阅的某个发布没有指定任何行过滤
	 *
	 * 2) one of the subscribed publications has puballtables set to true
	 *
	 * 2) 所订阅的某个发布把 puballtables 设为真
	 *
	 * 3) one of the subscribed publications is declared as TABLES IN SCHEMA
	 * that includes this relation
	 *
	 * 3) 所订阅的某个发布声明为 TABLES IN SCHEMA，并且包含这张关系
	 */
	if (server_version >= 150000)
	{
		/* Reuse the already-built pub_names.
		 *
		 * 复用已经拼好的 pub_names。
		 */
		Assert(pub_names != NULL);

		/* Check for row filters.
		 *
		 * 检查行过滤。
		 */
		resetStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "SELECT DISTINCT pg_get_expr(gpt.qual, gpt.relid)"
						 "  FROM pg_publication p,"
						 "  LATERAL pg_get_publication_tables(p.pubname) gpt"
						 " WHERE gpt.relid = %u"
						 "   AND p.pubname IN ( %s )",
						 lrel->remoteid,
						 pub_names->data);

		res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 1, qualRow);

		if (res->status != WALRCV_OK_TUPLES)
			ereport(ERROR,
					(errmsg("could not fetch table WHERE clause info for table \"%s.%s\" from publisher: %s",
							nspname, relname, res->err)));

		/*
		 * Multiple row filter expressions for the same table will be combined
		 * by COPY using OR. If any of the filter expressions for this table
		 * are null, it means the whole table will be copied. In this case it
		 * is not necessary to construct a unified row filter expression at
		 * all.
		 *
		 * 同一张表的多个行过滤表达式会由 COPY 用 OR 合并。如果该表的任一过滤表达式为空，就表示整张表都要拷贝。这时完全不必再构造统一的行过滤表达式。
		 */
		slot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
		while (tuplestore_gettupleslot(res->tuplestore, true, false, slot))
		{
			Datum		rf = slot_getattr(slot, 1, &isnull);

			if (!isnull)
				*qual = lappend(*qual, makeString(TextDatumGetCString(rf)));
			else
			{
				/* Ignore filters and cleanup as necessary.
				 *
				 * 忽略过滤器，并按需要进行清理。
				 */
				if (*qual)
				{
					list_free_deep(*qual);
					*qual = NIL;
				}
				break;
			}

			ExecClearTuple(slot);
		}
		ExecDropSingleTupleTableSlot(slot);

		walrcv_clear_result(res);
		destroyStringInfo(pub_names);
	}

	pfree(cmd.data);
}

/*
 * Copy existing data of a table from publisher.
 *
 * 从发布端拷贝一张表的已有数据。
 *
 * Caller is responsible for locking the local relation.
 *
 * 调用方负责锁定本地关系。
 */
static void
copy_table(Relation rel)
{
	LogicalRepRelMapEntry *relmapentry;
	LogicalRepRelation lrel;
	List	   *qual = NIL;
	WalRcvExecResult *res;
	StringInfoData cmd;
	CopyFromState cstate;
	List	   *attnamelist;
	ParseState *pstate;
	List	   *options = NIL;
	bool		gencol_published = false;

	/* Get the publisher relation info.
	 *
	 * 取得发布端关系的信息。
	 */
	fetch_remote_table_info(get_namespace_name(RelationGetNamespace(rel)),
							RelationGetRelationName(rel), &lrel, &qual,
							&gencol_published);

	/* Put the relation into relmap.
	 *
	 * 把该关系放进 relmap。
	 */
	logicalrep_relmap_update(&lrel);

	/* Map the publisher relation to local one.
	 *
	 * 把发布端关系映射到本地关系。
	 */
	relmapentry = logicalrep_rel_open(lrel.remoteid, NoLock);
	Assert(rel == relmapentry->localrel);

	/* Start copy on the publisher.
	 *
	 * 在发布端开始拷贝。
	 */
	initStringInfo(&cmd);

	/* Regular table with no row filter or generated columns
	 *
	 * 没有行过滤、也没有生成列的普通表。
	 */
	if (lrel.relkind == RELKIND_RELATION && qual == NIL && !gencol_published)
	{
		appendStringInfo(&cmd, "COPY %s",
						 quote_qualified_identifier(lrel.nspname, lrel.relname));

		/* If the table has columns, then specify the columns
		 *
		 * 如果表有列，就指定这些列。
		 */
		if (lrel.natts)
		{
			appendStringInfoString(&cmd, " (");

			/*
			 * XXX Do we need to list the columns in all cases? Maybe we're
			 * replicating all columns?
			 *
			 * 待查：是否在所有情况下都要列出列？也许我们正在复制全部列？
			 */
			for (int i = 0; i < lrel.natts; i++)
			{
				if (i > 0)
					appendStringInfoString(&cmd, ", ");

				appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			}

			appendStringInfoChar(&cmd, ')');
		}

		appendStringInfoString(&cmd, " TO STDOUT");
	}
	else
	{
		/*
		 * For non-tables and tables with row filters, we need to do COPY
		 * (SELECT ...), but we can't just do SELECT * because we may need to
		 * copy only subset of columns including generated columns. For tables
		 * with any row filters, build a SELECT query with OR'ed row filters
		 * for COPY.
		 *
		 * 对于非表对象以及带行过滤的表，需要使用 COPY (SELECT ...)，但不能直接 SELECT *，因为可能只拷贝列的一个子集，并且要包含生成列。对带有任何行过滤的表，为 COPY 构造一条用 OR 连接行过滤的 SELECT 查询。
		 *
		 * We also need to use this same COPY (SELECT ...) syntax when
		 * generated columns are published, because copy of generated columns
		 * is not supported by the normal COPY.
		 *
		 * 发布了生成列时也必须使用同样的 COPY (SELECT ...) 语法，因为普通 COPY 不支持拷贝生成列。
		 */
		appendStringInfoString(&cmd, "COPY (SELECT ");
		for (int i = 0; i < lrel.natts; i++)
		{
			appendStringInfoString(&cmd, quote_identifier(lrel.attnames[i]));
			if (i < lrel.natts - 1)
				appendStringInfoString(&cmd, ", ");
		}

		appendStringInfoString(&cmd, " FROM ");

		/*
		 * For regular tables, make sure we don't copy data from a child that
		 * inherits the named table as those will be copied separately.
		 *
		 * 对普通表，不要从继承了该具名表的子表拷贝数据，那些子表会单独拷贝。
		 */
		if (lrel.relkind == RELKIND_RELATION)
			appendStringInfoString(&cmd, "ONLY ");

		appendStringInfoString(&cmd, quote_qualified_identifier(lrel.nspname, lrel.relname));
		/* list of OR'ed filters
		 *
		 * 用 OR 连接起来的过滤条件列表。
		 */
		if (qual != NIL)
		{
			ListCell   *lc;
			char	   *q = strVal(linitial(qual));

			appendStringInfo(&cmd, " WHERE %s", q);
			for_each_from(lc, qual, 1)
			{
				q = strVal(lfirst(lc));
				appendStringInfo(&cmd, " OR %s", q);
			}
			list_free_deep(qual);
		}

		appendStringInfoString(&cmd, ") TO STDOUT");
	}

	/*
	 * Prior to v16, initial table synchronization will use text format even
	 * if the binary option is enabled for a subscription.
	 *
	 * 在 v16 之前，即使订阅启用了 binary 选项，初始表同步也使用文本格式。
	 */
	if (walrcv_server_version(LogRepWorkerWalRcvConn) >= 160000 &&
		MySubscription->binary)
	{
		appendStringInfoString(&cmd, " WITH (FORMAT binary)");
		options = list_make1(makeDefElem("format",
										 (Node *) makeString("binary"), -1));
	}

	res = walrcv_exec(LogRepWorkerWalRcvConn, cmd.data, 0, NULL);
	pfree(cmd.data);
	if (res->status != WALRCV_OK_COPY_OUT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not start initial contents copy for table \"%s.%s\": %s",
						lrel.nspname, lrel.relname, res->err)));
	walrcv_clear_result(res);

	copybuf = makeStringInfo();

	pstate = make_parsestate(NULL);
	(void) addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										 NULL, false, false);

	attnamelist = make_copy_attnamelist(relmapentry);
	cstate = BeginCopyFrom(pstate, rel, NULL, NULL, false, copy_read_data, attnamelist, options);

	/* Do the copy
	 *
	 * 执行拷贝。
	 */
	(void) CopyFrom(cstate);

	logicalrep_rel_close(relmapentry, NoLock);
}

/*
 * Determine the tablesync slot name.
 *
 * 确定 tablesync 复制槽的名字。
 *
 * The name must not exceed NAMEDATALEN - 1 because of remote node constraints
 * on slot name length. We append system_identifier to avoid slot_name
 * collision with subscriptions in other clusters. With the current scheme
 * pg_%u_sync_%u_UINT64_FORMAT (3 + 10 + 6 + 10 + 20 + '\0'), the maximum
 * length of slot_name will be 50.
 *
 * 由于远端对槽名长度的限制，名字不得超过 NAMEDATALEN 减 1。我们附上 system_identifier，避免与其他集群中的订阅发生槽名冲突。按当前格式 pg_%u_sync_%u_UINT64_FORMAT（3 + 10 + 6 + 10 + 20 + 结尾的空字符），槽名最大长度为 50。
 *
 * The returned slot name is stored in the supplied buffer (syncslotname) with
 * the given size.
 *
 * 返回的槽名存放在调用方提供的缓冲区 syncslotname 中，长度为给定大小。
 *
 * Note: We don't use the subscription slot name as part of tablesync slot name
 * because we are responsible for cleaning up these slots and it could become
 * impossible to recalculate what name to cleanup if the subscription slot name
 * had changed.
 *
 * 注意：tablesync 槽名不使用订阅槽名，因为这些槽要由我们负责清理；如果订阅槽名改了，就可能无法再算出该清理哪个名字。
 */
void
ReplicationSlotNameForTablesync(Oid suboid, Oid relid,
								char *syncslotname, Size szslot)
{
	snprintf(syncslotname, szslot, "pg_%u_sync_%u_" UINT64_FORMAT, suboid,
			 relid, GetSystemIdentifier());
}

/*
 * Start syncing the table in the sync worker.
 *
 * 在同步 worker 中开始同步这张表。
 *
 * If nothing needs to be done to sync the table, we exit the worker without
 * any further action.
 *
 * 如果这张表没有需要做的同步工作，worker 不再做其他动作，直接退出。
 *
 * The returned slot name is palloc'ed in current memory context.
 *
 * 返回的槽名用 palloc 分配在当前内存上下文中。
 */
static char *
LogicalRepSyncTableStart(XLogRecPtr *origin_startpos)
{
	char	   *slotname;
	char	   *err;
	char		relstate;
	XLogRecPtr	relstate_lsn;
	Relation	rel;
	AclResult	aclresult;
	WalRcvExecResult *res;
	char		originname[NAMEDATALEN];
	RepOriginId originid;
	UserContext ucxt;
	bool		must_use_password;
	bool		run_as_owner;

	/* Check the state of the table synchronization.
	 *
	 * 检查表同步的状态。
	 */
	StartTransactionCommand();
	relstate = GetSubscriptionRelState(MyLogicalRepWorker->subid,
									   MyLogicalRepWorker->relid,
									   &relstate_lsn);
	CommitTransactionCommand();

	/* Is the use of a password mandatory?
	 *
	 * 是否必须使用密码？
	 */
	must_use_password = MySubscription->passwordrequired &&
		!MySubscription->ownersuperuser;

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = relstate;
	MyLogicalRepWorker->relstate_lsn = relstate_lsn;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * If synchronization is already done or no longer necessary, exit now
	 * that we've updated shared memory state.
	 *
	 * 如果同步已经完成或不再需要，既然共享内存状态已经更新，现在就退出。
	 */
	switch (relstate)
	{
		case SUBREL_STATE_SYNCDONE:
		case SUBREL_STATE_READY:
		case SUBREL_STATE_UNKNOWN:
			finish_sync_worker();	/* doesn't return
			 *
			 * 不会返回。
			 */
	}

	/* Calculate the name of the tablesync slot.
	 *
	 * 计算 tablesync 槽的名字。
	 */
	slotname = (char *) palloc(NAMEDATALEN);
	ReplicationSlotNameForTablesync(MySubscription->oid,
									MyLogicalRepWorker->relid,
									slotname,
									NAMEDATALEN);

	/*
	 * Here we use the slot name instead of the subscription name as the
	 * application_name, so that it is different from the leader apply worker,
	 * so that synchronous replication can distinguish them.
	 *
	 * 这里用槽名而不是订阅名作为 application_name，使它与主 apply worker 不同，从而让同步复制能够区分它们。
	 */
	LogRepWorkerWalRcvConn =
		walrcv_connect(MySubscription->conninfo, true, true,
					   must_use_password,
					   slotname, &err);
	if (LogRepWorkerWalRcvConn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table synchronization worker for subscription \"%s\" could not connect to the publisher: %s",
						MySubscription->name, err)));

	Assert(MyLogicalRepWorker->relstate == SUBREL_STATE_INIT ||
		   MyLogicalRepWorker->relstate == SUBREL_STATE_DATASYNC ||
		   MyLogicalRepWorker->relstate == SUBREL_STATE_FINISHEDCOPY);

	/* Assign the origin tracking record name.
	 *
	 * 指定复制源跟踪记录的名字。
	 */
	ReplicationOriginNameForLogicalRep(MySubscription->oid,
									   MyLogicalRepWorker->relid,
									   originname,
									   sizeof(originname));

	if (MyLogicalRepWorker->relstate == SUBREL_STATE_DATASYNC)
	{
		/*
		 * We have previously errored out before finishing the copy so the
		 * replication slot might exist. We want to remove the slot if it
		 * already exists and proceed.
		 *
		 * 先前在拷贝完成前就报过错，复制槽可能还在。如果槽已经存在，我们希望删掉它再继续。
		 *
		 * XXX We could also instead try to drop the slot, last time we failed
		 * but for that, we might need to clean up the copy state as it might
		 * be in the middle of fetching the rows. Also, if there is a network
		 * breakdown then it wouldn't have succeeded so trying it next time
		 * seems like a better bet.
		 *
		 * 待查：也可以改为尝试删除槽。上次失败了，但那样做可能还要清理拷贝状态，因为它可能正取到一半。另外如果是网络中断，当时也不会成功，所以下次再试似乎更妥当。
		 */
		ReplicationSlotDropAtPubNode(LogRepWorkerWalRcvConn, slotname, true);
	}
	else if (MyLogicalRepWorker->relstate == SUBREL_STATE_FINISHEDCOPY)
	{
		/*
		 * The COPY phase was previously done, but tablesync then crashed
		 * before it was able to finish normally.
		 *
		 * COPY 阶段先前已经完成，但 tablesync 在能够正常结束之前崩溃了。
		 */
		StartTransactionCommand();

		/*
		 * The origin tracking name must already exist. It was created first
		 * time this tablesync was launched.
		 *
		 * 复制源跟踪名必须已经存在。它是在这个 tablesync 第一次启动时创建的。
		 */
		originid = replorigin_by_name(originname, false);
		replorigin_session_setup(originid, 0);
		replorigin_session_origin = originid;
		*origin_startpos = replorigin_session_get_progress(false);

		CommitTransactionCommand();

		goto copy_table_done;
	}

	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = SUBREL_STATE_DATASYNC;
	MyLogicalRepWorker->relstate_lsn = InvalidXLogRecPtr;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * Update the state, create the replication origin, and make them visible
	 * to others.
	 *
	 * 更新状态，创建复制源，并使它们对别人可见。
	 */
	StartTransactionCommand();
	UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
							   MyLogicalRepWorker->relid,
							   MyLogicalRepWorker->relstate,
							   MyLogicalRepWorker->relstate_lsn,
							   false);

	/*
	 * Create the replication origin in a separate transaction from the one
	 * that sets up the origin in shared memory. This prevents the risk that
	 * changes to the origin in shared memory cannot be rolled back if the
	 * transaction aborts.
	 *
	 * 在与把复制源装入共享内存的那个事务分开的事务里创建复制源。这样如果事务中止，共享内存中对复制源的修改就不会面临无法回滚的风险。
	 */
	originid = replorigin_by_name(originname, true);
	if (!OidIsValid(originid))
		originid = replorigin_create(originname);

	CommitTransactionCommand();
	pgstat_report_stat(true);

	StartTransactionCommand();

	/*
	 * Use a standard write lock here. It might be better to disallow access
	 * to the table while it's being synchronized. But we don't want to block
	 * the main apply process from working and it has to open the relation in
	 * RowExclusiveLock when remapping remote relation id to local one.
	 *
	 * 这里使用普通的写锁。同步期间禁止访问这张表也许更好。但我们不想挡住主 apply 进程，它在把远程关系标识映射到本地关系时必须以 RowExclusiveLock 打开关系。
	 */
	rel = table_open(MyLogicalRepWorker->relid, RowExclusiveLock);

	/*
	 * Start a transaction in the remote node in REPEATABLE READ mode.  This
	 * ensures that both the replication slot we create (see below) and the
	 * COPY are consistent with each other.
	 *
	 * 在远端以 REPEATABLE READ 开启一个事务。这样下面创建的复制槽和 COPY 彼此一致。
	 */
	res = walrcv_exec(LogRepWorkerWalRcvConn,
					  "BEGIN READ ONLY ISOLATION LEVEL REPEATABLE READ",
					  0, NULL);
	if (res->status != WALRCV_OK_COMMAND)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table copy could not start transaction on publisher: %s",
						res->err)));
	walrcv_clear_result(res);

	/*
	 * Create a new permanent logical decoding slot. This slot will be used
	 * for the catchup phase after COPY is done, so tell it to use the
	 * snapshot to make the final data consistent.
	 *
	 * 创建一个新的永久逻辑解码槽。COPY 完成后的追赶阶段会使用这个槽，因此告诉它使用该快照，使最终数据保持一致。
	 */
	walrcv_create_slot(LogRepWorkerWalRcvConn,
					   slotname, false /* permanent
					    *
					    * 永久槽。
					    */
					   MySubscription->failover,
					   CRS_USE_SNAPSHOT, origin_startpos);

	/*
	 * Advance the origin to the LSN got from walrcv_create_slot and then set
	 * up the origin. The advancement is WAL logged for the purpose of
	 * recovery. Locks are to prevent the replication origin from vanishing
	 * while advancing.
	 *
	 * 把复制源推进到 walrcv_create_slot 返回的 LSN，然后设置复制源。这次推进会记入 WAL，以便恢复。加锁是为了防止推进期间复制源消失。
	 *
	 * The purpose of doing these before the copy is to avoid doing the copy
	 * again due to any error in advancing or setting up origin tracking.
	 *
	 * 在拷贝之前做这些，是为了避免推进或设置复制源跟踪出错时不得不再拷贝一次。
	 */
	LockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);
	replorigin_advance(originid, *origin_startpos, InvalidXLogRecPtr,
					   true /* go backward
					    *
					    * 向后移动。
					    */
	UnlockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

	replorigin_session_setup(originid, 0);
	replorigin_session_origin = originid;

	/*
	 * Make sure that the copy command runs as the table owner, unless the
	 * user has opted out of that behaviour.
	 *
	 * 除非用户明确选择不这样做，否则确保 COPY 命令以表所有者的身份运行。
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->rd_rel->relowner, &ucxt);

	/*
	 * Check that our table sync worker has permission to insert into the
	 * target table.
	 *
	 * 检查表同步 worker 是否有权向目标表插入。
	 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_INSERT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	/*
	 * COPY FROM does not honor RLS policies.  That is not a problem for
	 * subscriptions owned by roles with BYPASSRLS privilege (or superuser,
	 * who has it implicitly), but other roles should not be able to
	 * circumvent RLS.  Disallow logical replication into RLS enabled
	 * relations for such roles.
	 *
	 * COPY FROM 不遵守 RLS 策略。对拥有 BYPASSRLS 权限的角色（或隐式拥有该权限的超级用户）所拥有的订阅，这不是问题；其他角色则不应能绕过 RLS。对这类角色，禁止向启用了 RLS 的关系做逻辑复制。
	 */
	if (check_enable_rls(RelationGetRelid(rel), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot replicate into relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						RelationGetRelationName(rel))));

	/* Now do the initial data copy
	 *
	 * 现在做初始数据拷贝。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());
	copy_table(rel);
	PopActiveSnapshot();

	res = walrcv_exec(LogRepWorkerWalRcvConn, "COMMIT", 0, NULL);
	if (res->status != WALRCV_OK_COMMAND)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("table copy could not finish transaction on publisher: %s",
						res->err)));
	walrcv_clear_result(res);

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	table_close(rel, NoLock);

	/* Make the copy visible.
	 *
	 * 让这次拷贝可见。
	 */
	CommandCounterIncrement();

	/*
	 * Update the persisted state to indicate the COPY phase is done; make it
	 * visible to others.
	 *
	 * 把持久化状态更新为表示 COPY 阶段已完成，并使它对别人可见。
	 */
	UpdateSubscriptionRelState(MyLogicalRepWorker->subid,
							   MyLogicalRepWorker->relid,
							   SUBREL_STATE_FINISHEDCOPY,
							   MyLogicalRepWorker->relstate_lsn,
							   false);

	CommitTransactionCommand();

copy_table_done:

	elog(DEBUG1,
		 "LogicalRepSyncTableStart: '%s' origin_startpos lsn %X/%X",
		 originname, LSN_FORMAT_ARGS(*origin_startpos));

	/*
	 * We are done with the initial data synchronization, update the state.
	 *
	 * 初始数据同步已经完成，更新状态。
	 */
	SpinLockAcquire(&MyLogicalRepWorker->relmutex);
	MyLogicalRepWorker->relstate = SUBREL_STATE_SYNCWAIT;
	MyLogicalRepWorker->relstate_lsn = *origin_startpos;
	SpinLockRelease(&MyLogicalRepWorker->relmutex);

	/*
	 * Finally, wait until the leader apply worker tells us to catch up and
	 * then return to let LogicalRepApplyLoop do it.
	 *
	 * 最后，等到主 apply worker 通知我们去追赶，然后返回，让 LogicalRepApplyLoop 来做这件事。
	 */
	wait_for_worker_state_change(SUBREL_STATE_CATCHUP);
	return slotname;
}

/*
 * Common code to fetch the up-to-date sync state info into the static lists.
 *
 * 把最新的同步状态信息取进静态列表的公共代码。
 *
 * Returns true if subscription has 1 or more tables, else false.
 *
 * 如果订阅有一张或更多表，返回真，否则返回假。
 *
 * Note: If this function started the transaction (indicated by the parameter)
 * then it is the caller's responsibility to commit it.
 *
 * 注意：如果本函数开启了事务（由参数指出），则由调用方负责提交。
 */
static bool
FetchTableStates(bool *started_tx)
{
	static bool has_subrels = false;

	*started_tx = false;

	if (table_states_validity != SYNC_TABLE_STATE_VALID)
	{
		MemoryContext oldctx;
		List	   *rstates;
		ListCell   *lc;
		SubscriptionRelState *rstate;

		table_states_validity = SYNC_TABLE_STATE_REBUILD_STARTED;

		/* Clean the old lists.
		 *
		 * 清掉旧列表。
		 */
		list_free_deep(table_states_not_ready);
		table_states_not_ready = NIL;

		if (!IsTransactionState())
		{
			StartTransactionCommand();
			*started_tx = true;
		}

		/* Fetch all non-ready tables.
		 *
		 * 取出所有尚未就绪的表。
		 */
		rstates = GetSubscriptionRelations(MySubscription->oid, true);

		/* Allocate the tracking info in a permanent memory context.
		 *
		 * 在永久内存上下文中分配跟踪信息。
		 */
		oldctx = MemoryContextSwitchTo(CacheMemoryContext);
		foreach(lc, rstates)
		{
			rstate = palloc(sizeof(SubscriptionRelState));
			memcpy(rstate, lfirst(lc), sizeof(SubscriptionRelState));
			table_states_not_ready = lappend(table_states_not_ready, rstate);
		}
		MemoryContextSwitchTo(oldctx);

		/*
		 * Does the subscription have tables?
		 *
		 * 这个订阅有表吗？
		 *
		 * If there were not-READY relations found then we know it does. But
		 * if table_states_not_ready was empty we still need to check again to
		 * see if there are 0 tables.
		 *
		 * 如果找到了非 READY 的关系，就知道它有表。但如果 table_states_not_ready 是空的，仍要再检查一次，看是不是一张表都没有。
		 */
		has_subrels = (table_states_not_ready != NIL) ||
			HasSubscriptionRelations(MySubscription->oid);

		/*
		 * If the subscription relation cache has been invalidated since we
		 * entered this routine, we still use and return the relations we just
		 * finished constructing, to avoid infinite loops, but we leave the
		 * table states marked as stale so that we'll rebuild it again on next
		 * access. Otherwise, we mark the table states as valid.
		 *
		 * 如果进入本函数之后订阅关系缓存已失效，我们仍然使用并返回刚刚建好的关系，以避免无限循环，但把表状态标为过期，下次访问时再重建。否则把表状态标为有效。
		 */
		if (table_states_validity == SYNC_TABLE_STATE_REBUILD_STARTED)
			table_states_validity = SYNC_TABLE_STATE_VALID;
	}

	return has_subrels;
}

/*
 * Execute the initial sync with error handling. Disable the subscription,
 * if it's required.
 *
 * 带错误处理地执行初始同步。如果需要，就禁用该订阅。
 *
 * Allocate the slot name in long-lived context on return. Note that we don't
 * handle FATAL errors which are probably because of system resource error and
 * are not repeatable.
 *
 * 返回时在长寿命上下文中分配槽名。注意这里不处理 FATAL 错误，那些多半是系统资源错误，而且不可重复。
 */
static void
start_table_sync(XLogRecPtr *origin_startpos, char **slotname)
{
	char	   *sync_slotname = NULL;

	Assert(am_tablesync_worker());

	PG_TRY();
	{
		/* Call initial sync.
		 *
		 * 调用初始同步。
		 */
		sync_slotname = LogicalRepSyncTableStart(origin_startpos);
	}
	PG_CATCH();
	{
		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * Report the worker failed during table synchronization. Abort
			 * the current transaction so that the stats message is sent in an
			 * idle state.
			 *
			 * 报告 worker 在表同步期间失败。中止当前事务，以便在空闲状态下发送统计消息。
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, false);

			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	/* allocate slot name in long-lived context
	 *
	 * 在长寿命上下文中分配槽名。
	 */
	*slotname = MemoryContextStrdup(ApplyContext, sync_slotname);
	pfree(sync_slotname);
}

/*
 * Runs the tablesync worker.
 *
 * 运行 tablesync worker。
 *
 * It starts syncing tables. After a successful sync, sets streaming options
 * and starts streaming to catchup with apply worker.
 *
 * 它开始同步表。成功同步之后，设置流式选项，并开始流式传输，以追上 apply worker。
 */
static void
run_tablesync_worker()
{
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos = InvalidXLogRecPtr;
	char	   *slotname = NULL;
	WalRcvStreamOptions options;

	start_table_sync(&origin_startpos, &slotname);

	ReplicationOriginNameForLogicalRep(MySubscription->oid,
									   MyLogicalRepWorker->relid,
									   originname,
									   sizeof(originname));

	set_apply_error_context_origin(originname);

	set_stream_options(&options, slotname, &origin_startpos);

	walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);

	/* Apply the changes till we catchup with the apply worker.
	 *
	 * 应用变更，直到追上 apply worker。
	 */
	start_apply(origin_startpos);
}

/* Logical Replication Tablesync worker entry point
 *
 * 逻辑复制表同步 worker 的入口。
 */
void
TablesyncWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);

	SetupApplyOrSyncWorker(worker_slot);

	run_tablesync_worker();

	finish_sync_worker();
}

/*
 * If the subscription has no tables then return false.
 *
 * 如果订阅没有表，则返回假。
 *
 * Otherwise, are all tablesyncs READY?
 *
 * 否则，是否所有 tablesync 都已 READY？
 *
 * Note: This function is not suitable to be called from outside of apply or
 * tablesync workers because MySubscription needs to be already initialized.
 *
 * 注意：本函数不适合在 apply 或 tablesync worker 之外调用，因为 MySubscription 必须已经初始化。
 */
bool
AllTablesyncsReady(void)
{
	bool		started_tx = false;
	bool		has_subrels = false;

	/* We need up-to-date sync state info for subscription tables here.
	 *
	 * 这里需要订阅表的最新同步状态信息。
	 */
	has_subrels = FetchTableStates(&started_tx);

	if (started_tx)
	{
		CommitTransactionCommand();
		pgstat_report_stat(true);
	}

	/*
	 * Return false when there are no tables in subscription or not all tables
	 * are in ready state; true otherwise.
	 *
	 * 当订阅中没有表，或者并非所有表都处于就绪状态时返回假；否则返回真。
	 */
	return has_subrels && (table_states_not_ready == NIL);
}

/*
 * Update the two_phase state of the specified subscription in pg_subscription.
 *
 * 更新 pg_subscription 中指定订阅的 two_phase 状态。
 */
void
UpdateTwoPhaseState(Oid suboid, char new_state)
{
	Relation	rel;
	HeapTuple	tup;
	bool		nulls[Natts_pg_subscription];
	bool		replaces[Natts_pg_subscription];
	Datum		values[Natts_pg_subscription];

	Assert(new_state == LOGICALREP_TWOPHASE_STATE_DISABLED ||
		   new_state == LOGICALREP_TWOPHASE_STATE_PENDING ||
		   new_state == LOGICALREP_TWOPHASE_STATE_ENABLED);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID, ObjectIdGetDatum(suboid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR,
			 "cache lookup failed for subscription oid %u",
			 suboid);

	/* Form a new tuple.
	 *
	 * 构造一个新元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	/* And update/set two_phase state
	 *
	 * 并更新、设置 two_phase 状态。
	 */
	values[Anum_pg_subscription_subtwophasestate - 1] = CharGetDatum(new_state);
	replaces[Anum_pg_subscription_subtwophasestate - 1] = true;

	tup = heap_modify_tuple(tup, RelationGetDescr(rel),
							values, nulls, replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}
