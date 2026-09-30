/*-------------------------------------------------------------------------
 * slotsync.c
 *	   Functionality for synchronizing slots to a standby server from the
 *         primary server.
 *
 * slotsync.c：把复制槽从主服务器同步到备服务器的功能。
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/slotsync.c
 *
 * This file contains the code for slot synchronization on a physical standby
 * to fetch logical failover slots information from the primary server, create
 * the slots on the standby and synchronize them periodically.
 *
 * 本文件包含物理备库上的槽同步代码：从主服务器获取逻辑故障转移槽的信息，在备库上创建这些槽，并定期同步它们。
 *
 * Slot synchronization can be performed either automatically by enabling slot
 * sync worker or manually by calling SQL function pg_sync_replication_slots().
 *
 * 槽同步可以自动进行，只要启用 slot sync worker；也可以手动调用 SQL 函数 pg_sync_replication_slots()。
 *
 * If the WAL corresponding to the remote's restart_lsn is not available on the
 * physical standby or the remote's catalog_xmin precedes the oldest xid for
 * which it is guaranteed that rows wouldn't have been removed then we cannot
 * create the local standby slot because that would mean moving the local slot
 * backward and decoding won't be possible via such a slot. In this case, the
 * slot will be marked as RS_TEMPORARY. Once the primary server catches up,
 * the slot will be marked as RS_PERSISTENT (which means sync-ready) after
 * which slot sync worker can perform the sync periodically or user can call
 * pg_sync_replication_slots() periodically to perform the syncs.
 *
 * 如果远端 restart_lsn 对应的 WAL 在物理备库上不可用，或者远端的 catalog_xmin 早于能够保证行尚未被删除的最老 xid，就不能创建本地备库槽，因为那意味着把本地槽向后移动，这样的槽无法解码。此时把槽标为 RS_TEMPORARY。等主服务器赶上来之后，槽会被标为 RS_PERSISTENT（表示已可同步），此后 slot sync worker 可以定期同步，用户也可以定期调用 pg_sync_replication_slots() 来同步。
 *
 * If synchronized slots fail to build a consistent snapshot from the
 * restart_lsn before reaching confirmed_flush_lsn, they would become
 * unreliable after promotion due to potential data loss from changes
 * before reaching a consistent point. This can happen because the slots can
 * be synced at some random time and we may not reach the consistent point
 * at the same WAL location as the primary. So, we mark such slots as
 * RS_TEMPORARY. Once the decoding from corresponding LSNs can reach a
 * consistent point, they will be marked as RS_PERSISTENT.
 *
 * 如果已同步的槽在从 restart_lsn 到达 confirmed_flush_lsn 之前建不出一致快照，提升之后就会不可靠，因为到达一致点之前的变更可能丢失。这是因为槽可能在任意时刻被同步，我们到达一致点的 WAL 位置可能和主库不同。因此把这样的槽标为 RS_TEMPORARY。一旦从相应 LSN 解码能够到达一致点，就把它们标为 RS_PERSISTENT。
 *
 * The slot sync worker waits for some time before the next synchronization,
 * with the duration varying based on whether any slots were updated during
 * the last cycle. Refer to the comments above wait_for_slot_activity() for
 * more details.
 *
 * slot sync worker 在下一次同步之前会等待一段时间，时长取决于上一轮是否有槽被更新。详情见 wait_for_slot_activity() 上方的注释。
 *
 * Any standby synchronized slots will be dropped if they no longer need
 * to be synchronized. See comment atop drop_local_obsolete_slots() for more
 * details.
 *
 * 备库上已同步的槽如果不再需要同步，就会被删除。详情见 drop_local_obsolete_slots() 上方的注释。
 *
 *---------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>

#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/logical.h"
#include "replication/slotsync.h"
#include "replication/snapbuild.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

/*
 * 核心流程：
 * 备库可以启动 slot sync worker（ReplSlotSyncWorkerMain），也可以由 SQL 函数
 * pg_sync_replication_slots 调用 SyncReplicationSlots。
 * synchronize_slots 从主库读取故障转移逻辑槽，再对每个槽调用 synchronize_one_slot：
 * 本地不存在则创建；WAL 或 catalog_xmin 不满足时标为 RS_TEMPORARY，否则同步 LSN 并标为 RS_PERSISTENT。
 * drop_local_obsolete_slots 删除主库上已不再需要同步的本地槽。
 * 两次同步之间由 wait_for_slot_activity 按是否有槽更新决定等待时间。
 */

/*
 * Struct for sharing information to control slot synchronization.
 *
 * 用于共享信息、以控制槽同步的结构。
 *
 * The slot sync worker's pid is needed by the startup process to shut it
 * down during promotion. The startup process shuts down the slot sync worker
 * and also sets stopSignaled=true to handle the race condition when the
 * postmaster has not noticed the promotion yet and thus may end up restarting
 * the slot sync worker. If stopSignaled is set, the worker will exit in such a
 * case. The SQL function pg_sync_replication_slots() will also error out if
 * this flag is set. Note that we don't need to reset this variable as after
 * promotion the slot sync worker won't be restarted because the pmState
 * changes to PM_RUN from PM_HOT_STANDBY and we don't support demoting
 * primary without restarting the server. See LaunchMissingBackgroundProcesses.
 *
 * 启动进程在提升期间需要 slot sync worker 的 pid 来关闭它。启动进程会关闭 slot sync worker，并把 stopSignaled 设为真，以处理 postmaster 尚未注意到提升、因而可能再次启动 slot sync worker 的竞态。如果 stopSignaled 已设置，worker 在这种情况下会退出。SQL 函数 pg_sync_replication_slots() 在该标志已设置时也会报错。提升之后不必复位这个变量，因为 pmState 会从 PM_HOT_STANDBY 变为 PM_RUN，而且不支持不重启服务器就把主库降为备库，slot sync worker 不会再被启动。参见 LaunchMissingBackgroundProcesses。
 *
 * The 'syncing' flag is needed to prevent concurrent slot syncs to avoid slot
 * overwrites.
 *
 * syncing 标志用来防止并发的槽同步，以免槽被互相覆盖。
 *
 * The 'last_start_time' is needed by postmaster to start the slot sync worker
 * once per SLOTSYNC_RESTART_INTERVAL_SEC. In cases where an immediate restart
 * is expected (e.g., slot sync GUCs change), slot sync worker will reset
 * last_start_time before exiting, so that postmaster can start the worker
 * without waiting for SLOTSYNC_RESTART_INTERVAL_SEC.
 *
 * postmaster 用 last_start_time 保证每 SLOTSYNC_RESTART_INTERVAL_SEC 才启动一次 slot sync worker。如果期望立即重启（例如槽同步的 GUC 变了），slot sync worker 会在退出前把 last_start_time 复位，这样 postmaster 不必等待 SLOTSYNC_RESTART_INTERVAL_SEC 就能启动 worker。
 */
typedef struct SlotSyncCtxStruct
{
	pid_t		pid;
	bool		stopSignaled;
	bool		syncing;
	time_t		last_start_time;
	slock_t		mutex;
} SlotSyncCtxStruct;

static SlotSyncCtxStruct *SlotSyncCtx = NULL;

/* GUC variable
 *
 * GUC 变量。
 */
bool		sync_replication_slots = false;

/*
 * The sleep time (ms) between slot-sync cycles varies dynamically
 * (within a MIN/MAX range) according to slot activity. See
 * wait_for_slot_activity() for details.
 *
 * 两次槽同步之间的睡眠时间（毫秒）会按槽的活动情况，在最小值和最大值之间动态变化。详情见 wait_for_slot_activity()。
 */
#define MIN_SLOTSYNC_WORKER_NAPTIME_MS  200
#define MAX_SLOTSYNC_WORKER_NAPTIME_MS  30000	/* 30s */
/*
 *
 * 30 秒。
 */

static long sleep_ms = MIN_SLOTSYNC_WORKER_NAPTIME_MS;

/* The restart interval for slot sync work used by postmaster
 *
 * postmaster 用来重启槽同步工作的间隔。
 */
#define SLOTSYNC_RESTART_INTERVAL_SEC 10

/*
 * Flag to tell if we are syncing replication slots. Unlike the 'syncing' flag
 * in SlotSyncCtxStruct, this flag is true only if the current process is
 * performing slot synchronization.
 *
 * 表示当前是否正在同步复制槽的标志。与 SlotSyncCtxStruct 里的 syncing 标志不同，只有当前进程正在执行槽同步时，这个标志才为真。
 */
static bool syncing_slots = false;

/*
 * Structure to hold information fetched from the primary server about a logical
 * replication slot.
 *
 * 保存从主服务器取到的某个逻辑复制槽信息的结构。
 */
typedef struct RemoteSlot
{
	char	   *name;
	char	   *plugin;
	char	   *database;
	bool		two_phase;
	bool		failover;
	XLogRecPtr	restart_lsn;
	XLogRecPtr	confirmed_lsn;
	XLogRecPtr	two_phase_at;
	TransactionId catalog_xmin;

	/* RS_INVAL_NONE if valid, or the reason of invalidation
	 *
	 * 有效时为 RS_INVAL_NONE，否则是失效原因。
	 */
	ReplicationSlotInvalidationCause invalidated;
} RemoteSlot;

static void slotsync_failure_callback(int code, Datum arg);
static void update_synced_slots_inactive_since(void);

/*
 * If necessary, update the local synced slot's metadata based on the data
 * from the remote slot.
 *
 * 必要时根据远端槽的数据，更新本地已同步槽的元数据。
 *
 * If no update was needed (the data of the remote slot is the same as the
 * local slot) return false, otherwise true.
 *
 * 如果不需要更新（远端槽的数据与本地槽相同）则返回假，否则返回真。
 *
 * *found_consistent_snapshot will be true iff the remote slot's LSN or xmin is
 * modified, and decoding from the corresponding LSN's can reach a
 * consistent snapshot.
 *
 * 当且仅当远端槽的 LSN 或 xmin 被修改，并且从相应 LSN 解码能够到达一致快照时，found_consistent_snapshot 所指向的值为真。
 *
 * *remote_slot_precedes will be true if the remote slot's LSN or xmin
 * precedes locally reserved position.
 *
 * 如果远端槽的 LSN 或 xmin 早于本地已保留的位置，则 remote_slot_precedes 所指向的值为真。
 */
static bool
update_local_synced_slot(RemoteSlot *remote_slot, Oid remote_dbid,
						 bool *found_consistent_snapshot,
						 bool *remote_slot_precedes)
{
	ReplicationSlot *slot = MyReplicationSlot;
	bool		updated_xmin_or_lsn = false;
	bool		updated_config = false;

	Assert(slot->data.invalidated == RS_INVAL_NONE);

	if (found_consistent_snapshot)
		*found_consistent_snapshot = false;

	if (remote_slot_precedes)
		*remote_slot_precedes = false;

	/*
	 * Don't overwrite if we already have a newer catalog_xmin and
	 * restart_lsn.
	 *
	 * 如果本地已经有更新的 catalog_xmin 和 restart_lsn，就不要覆盖。
	 */
	if (remote_slot->restart_lsn < slot->data.restart_lsn ||
		TransactionIdPrecedes(remote_slot->catalog_xmin,
							  slot->data.catalog_xmin))
	{
		/*
		 * This can happen in following situations:
		 *
		 * 下列情况可能发生：
		 *
		 * If the slot is temporary, it means either the initial WAL location
		 * reserved for the local slot is ahead of the remote slot's
		 * restart_lsn or the initial xmin_horizon computed for the local slot
		 * is ahead of the remote slot.
		 *
		 * 如果槽是临时的，说明要么为本地槽保留的初始 WAL 位置超前于远端槽的 restart_lsn，要么为本地槽算出的初始 xmin 视界超前于远端槽。
		 *
		 * If the slot is persistent, both restart_lsn and catalog_xmin of the
		 * synced slot could still be ahead of the remote slot. Since we use
		 * slot advance functionality to keep snapbuild/slot updated, it is
		 * possible that the restart_lsn and catalog_xmin are advanced to a
		 * later position than it has on the primary. This can happen when
		 * slot advancing machinery finds running xacts record after reaching
		 * the consistent state at a later point than the primary where it
		 * serializes the snapshot and updates the restart_lsn.
		 *
		 * 如果槽是持久的，已同步槽的 restart_lsn 和 catalog_xmin 仍可能超前于远端槽。因为我们用槽推进功能来保持快照构建器和槽的更新，restart_lsn 和 catalog_xmin 可能被推进到比主库更靠后的位置。当槽推进机制在到达一致状态之后、于比主库序列化快照并更新 restart_lsn 更晚的位置找到 running xacts 记录时，就会这样。
		 *
		 * We LOG the message if the slot is temporary as it can help the user
		 * to understand why the slot is not sync-ready. In the case of a
		 * persistent slot, it would be a more common case and won't directly
		 * impact the users, so we used DEBUG1 level to log the message.
		 *
		 * 槽是临时的时候用 LOG 记这条消息，帮助用户理解为什么槽还不能同步。持久槽则更常见，而且不直接影响用户，所以用 DEBUG1 记录。
		 */
		ereport(slot->data.persistency == RS_TEMPORARY ? LOG : DEBUG1,
				errmsg("could not synchronize replication slot \"%s\"",
					   remote_slot->name),
				errdetail("Synchronization could lead to data loss, because the remote slot needs WAL at LSN %X/%X and catalog xmin %u, but the standby has LSN %X/%X and catalog xmin %u.",
						  LSN_FORMAT_ARGS(remote_slot->restart_lsn),
						  remote_slot->catalog_xmin,
						  LSN_FORMAT_ARGS(slot->data.restart_lsn),
						  slot->data.catalog_xmin));

		if (remote_slot_precedes)
			*remote_slot_precedes = true;

		/*
		 * Skip updating the configuration. This is required to avoid syncing
		 * two_phase_at without syncing confirmed_lsn. Otherwise, the prepared
		 * transaction between old confirmed_lsn and two_phase_at will
		 * unexpectedly get decoded and sent to the downstream after
		 * promotion. See comments in ReorderBufferFinishPrepared.
		 *
		 * 跳过配置更新。这是为了避免在没有同步 confirmed_lsn 的情况下同步 two_phase_at。否则旧 confirmed_lsn 与 two_phase_at 之间的已准备事务，会在提升后被意外解码并发给下游。参见 ReorderBufferFinishPrepared 中的注释。
		 */
		return false;
	}

	/*
	 * Attempt to sync LSNs and xmins only if remote slot is ahead of local
	 * slot.
	 *
	 * 只有远端槽超前于本地槽时，才尝试同步 LSN 和 xmin。
	 */
	if (remote_slot->confirmed_lsn > slot->data.confirmed_flush ||
		remote_slot->restart_lsn > slot->data.restart_lsn ||
		TransactionIdFollows(remote_slot->catalog_xmin,
							 slot->data.catalog_xmin))
	{
		/*
		 * We can't directly copy the remote slot's LSN or xmin unless there
		 * exists a consistent snapshot at that point. Otherwise, after
		 * promotion, the slots may not reach a consistent point before the
		 * confirmed_flush_lsn which can lead to a data loss. To avoid data
		 * loss, we let slot machinery advance the slot which ensures that
		 * snapbuilder/slot statuses are updated properly.
		 *
		 * 不能直接复制远端槽的 LSN 或 xmin，除非那个点存在一致快照。否则提升之后，槽可能在 confirmed_flush_lsn 之前到不了一致点，从而导致数据丢失。为避免数据丢失，让槽机制去推进槽，以保证快照构建器和槽的状态被正确更新。
		 */
		if (SnapBuildSnapshotExists(remote_slot->restart_lsn))
		{
			/*
			 * Update the slot info directly if there is a serialized snapshot
			 * at the restart_lsn, as the slot can quickly reach consistency
			 * at restart_lsn by restoring the snapshot.
			 *
			 * 如果 restart_lsn 处有已序列化的快照，就直接更新槽信息，因为槽可以通过恢复该快照很快在 restart_lsn 达到一致。
			 */
			SpinLockAcquire(&slot->mutex);
			slot->data.restart_lsn = remote_slot->restart_lsn;
			slot->data.confirmed_flush = remote_slot->confirmed_lsn;
			slot->data.catalog_xmin = remote_slot->catalog_xmin;
			SpinLockRelease(&slot->mutex);

			if (found_consistent_snapshot)
				*found_consistent_snapshot = true;
		}
		else
		{
			LogicalSlotAdvanceAndCheckSnapState(remote_slot->confirmed_lsn,
												found_consistent_snapshot);

			/* Sanity check
			 *
			 * 健全性检查。
			 */
			if (slot->data.confirmed_flush != remote_slot->confirmed_lsn)
				ereport(ERROR,
						errmsg_internal("synchronized confirmed_flush for slot \"%s\" differs from remote slot",
										remote_slot->name),
						errdetail_internal("Remote slot has LSN %X/%X but local slot has LSN %X/%X.",
										   LSN_FORMAT_ARGS(remote_slot->confirmed_lsn),
										   LSN_FORMAT_ARGS(slot->data.confirmed_flush)));
		}

		updated_xmin_or_lsn = true;
	}

	if (remote_dbid != slot->data.database ||
		remote_slot->two_phase != slot->data.two_phase ||
		remote_slot->failover != slot->data.failover ||
		strcmp(remote_slot->plugin, NameStr(slot->data.plugin)) != 0 ||
		remote_slot->two_phase_at != slot->data.two_phase_at)
	{
		NameData	plugin_name;

		/* Avoid expensive operations while holding a spinlock.
		 *
		 * 持有自旋锁时避免昂贵操作。
		 */
		namestrcpy(&plugin_name, remote_slot->plugin);

		SpinLockAcquire(&slot->mutex);
		slot->data.plugin = plugin_name;
		slot->data.database = remote_dbid;
		slot->data.two_phase = remote_slot->two_phase;
		slot->data.two_phase_at = remote_slot->two_phase_at;
		slot->data.failover = remote_slot->failover;
		SpinLockRelease(&slot->mutex);

		updated_config = true;

		/*
		 * Ensure that there is no risk of sending prepared transactions
		 * unexpectedly after the promotion.
		 *
		 * 确保提升之后不会意外发送已准备事务。
		 */
		Assert(slot->data.two_phase_at <= slot->data.confirmed_flush);
	}

	/*
	 * We have to write the changed xmin to disk *before* we change the
	 * in-memory value, otherwise after a crash we wouldn't know that some
	 * catalog tuples might have been removed already.
	 *
	 * 必须先把改变后的 xmin 写到磁盘，再修改内存中的值，否则崩溃之后无法知道某些目录元组可能已经被删掉。
	 */
	if (updated_config || updated_xmin_or_lsn)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}

	/*
	 * Now the new xmin is safely on disk, we can let the global value
	 * advance. We do not take ProcArrayLock or similar since we only advance
	 * xmin here and there's not much harm done by a concurrent computation
	 * missing that.
	 *
	 * 新的 xmin 已经安全落盘，可以让全局值向前推进。这里只推进 xmin，不必获取 ProcArrayLock 之类的锁，并发计算漏掉这次推进也没有太大害处。
	 */
	if (updated_xmin_or_lsn)
	{
		SpinLockAcquire(&slot->mutex);
		slot->effective_catalog_xmin = remote_slot->catalog_xmin;
		SpinLockRelease(&slot->mutex);

		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
	}

	return updated_config || updated_xmin_or_lsn;
}

/*
 * Get the list of local logical slots that are synchronized from the
 * primary server.
 *
 * 取得从主服务器同步过来的本地逻辑槽列表。
 */
static List *
get_local_synced_slots(void)
{
	List	   *local_slots = NIL;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		/* Check if it is a synchronized slot
		 *
		 * 检查它是不是已同步的槽。
		 */
		if (s->in_use && s->data.synced)
		{
			Assert(SlotIsLogical(s));
			local_slots = lappend(local_slots, s);
		}
	}

	LWLockRelease(ReplicationSlotControlLock);

	return local_slots;
}

/*
 * Helper function to check if local_slot is required to be retained.
 *
 * 辅助函数：检查 local_slot 是否必须保留。
 *
 * Return false either if local_slot does not exist in the remote_slots list
 * or is invalidated while the corresponding remote slot is still valid,
 * otherwise true.
 *
 * 如果 local_slot 不在 remote_slots 列表中，或者本地槽已失效而对应的远端槽仍然有效，则返回假，否则返回真。
 */
static bool
local_sync_slot_required(ReplicationSlot *local_slot, List *remote_slots)
{
	bool		remote_exists = false;
	bool		locally_invalidated = false;

	foreach_ptr(RemoteSlot, remote_slot, remote_slots)
	{
		if (strcmp(remote_slot->name, NameStr(local_slot->data.name)) == 0)
		{
			remote_exists = true;

			/*
			 * If remote slot is not invalidated but local slot is marked as
			 * invalidated, then set locally_invalidated flag.
			 *
			 * 如果远端槽没有失效，但本地槽被标为已失效，就设置 locally_invalidated 标志。
			 */
			SpinLockAcquire(&local_slot->mutex);
			locally_invalidated =
				(remote_slot->invalidated == RS_INVAL_NONE) &&
				(local_slot->data.invalidated != RS_INVAL_NONE);
			SpinLockRelease(&local_slot->mutex);

			break;
		}
	}

	return (remote_exists && !locally_invalidated);
}

/*
 * Drop local obsolete slots.
 *
 * 删除本地过时的槽。
 *
 * Drop the local slots that no longer need to be synced i.e. these either do
 * not exist on the primary or are no longer enabled for failover.
 *
 * 删除不再需要同步的本地槽，也就是主库上已不存在、或者不再启用故障转移的槽。
 *
 * Additionally, drop any slots that are valid on the primary but got
 * invalidated on the standby. This situation may occur due to the following
 * reasons:
 * - The 'max_slot_wal_keep_size' on the standby is insufficient to retain WAL
 *   records from the restart_lsn of the slot.
 * - 'primary_slot_name' is temporarily reset to null and the physical slot is
 *   removed.
 * These dropped slots will get recreated in next sync-cycle and it is okay to
 * drop and recreate such slots as long as these are not consumable on the
 * standby (which is the case currently).
 *
 * 另外，删除那些在主库上仍然有效、但在备库上已失效的槽。可能的原因：备库上的 max_slot_wal_keep_size 不足以保留从该槽 restart_lsn 起的 WAL；或者 primary_slot_name 被临时置空并且物理槽被删除。这些被删的槽会在下一轮同步中重建。只要这些槽目前在备库上还不能被消费（当前就是这种情况），删掉再重建是可以的。
 *
 * Note: Change of 'wal_level' on the primary server to a level lower than
 * logical may also result in slot invalidation and removal on the standby.
 * This is because such 'wal_level' change is only possible if the logical
 * slots are removed on the primary server, so it's expected to see the
 * slots being invalidated and removed on the standby too (and re-created
 * if they are re-created on the primary server).
 *
 * 注意：主服务器把 wal_level 改成低于 logical 时，也可能导致备库上的槽失效并被删除。因为只有先在主服务器上删除逻辑槽，才能把 wal_level 改到那么低，所以预期备库上的槽也会失效并被删除（如果它们在主服务器上被重建，备库上也会重建）。
 */
static void
drop_local_obsolete_slots(List *remote_slot_list)
{
	List	   *local_slots = get_local_synced_slots();

	foreach_ptr(ReplicationSlot, local_slot, local_slots)
	{
		/* Drop the local slot if it is not required to be retained.
		 *
		 * 如果本地槽不需要保留，就删除它。
		 */
		if (!local_sync_slot_required(local_slot, remote_slot_list))
		{
			bool		synced_slot;

			/*
			 * Use shared lock to prevent a conflict with
			 * ReplicationSlotsDropDBSlots(), trying to drop the same slot
			 * during a drop-database operation.
			 *
			 * 使用共享锁，避免与 ReplicationSlotsDropDBSlots() 冲突，后者在删除数据库时可能试图删除同一个槽。
			 */
			LockSharedObject(DatabaseRelationId, local_slot->data.database,
							 0, AccessShareLock);

			/*
			 * In the small window between getting the slot to drop and
			 * locking the database, there is a possibility of a parallel
			 * database drop by the startup process and the creation of a new
			 * slot by the user. This new user-created slot may end up using
			 * the same shared memory as that of 'local_slot'. Thus check if
			 * local_slot is still the synced one before performing actual
			 * drop.
			 *
			 * 在拿到要删除的槽和锁定数据库之间有一个小窗口，启动进程可能并行删除数据库，用户也可能创建一个新槽。这个新建的用户槽可能占用与 local_slot 相同的共享内存。因此在真正删除之前，要再确认 local_slot 仍然是那个已同步的槽。
			 */
			SpinLockAcquire(&local_slot->mutex);
			synced_slot = local_slot->in_use && local_slot->data.synced;
			SpinLockRelease(&local_slot->mutex);

			if (synced_slot)
			{
				ReplicationSlotAcquire(NameStr(local_slot->data.name), true, false);
				ReplicationSlotDropAcquired();
			}

			UnlockSharedObject(DatabaseRelationId, local_slot->data.database,
							   0, AccessShareLock);

			ereport(LOG,
					errmsg("dropped replication slot \"%s\" of database with OID %u",
						   NameStr(local_slot->data.name),
						   local_slot->data.database));
		}
	}
}

/*
 * Reserve WAL for the currently active local slot using the specified WAL
 * location (restart_lsn).
 *
 * 用指定的 WAL 位置 restart_lsn，为当前活动的本地槽保留 WAL。
 *
 * If the given WAL location has been removed or is at risk of removal,
 * reserve WAL using the oldest segment that is non-removable.
 *
 * 如果给定的 WAL 位置已被删除或有被删除的风险，就用最老的、不可删除的段来保留 WAL。
 */
static void
reserve_wal_for_local_slot(XLogRecPtr restart_lsn)
{
	XLogRecPtr	slot_min_lsn;
	XLogRecPtr	min_safe_lsn;
	XLogSegNo	segno;
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(!XLogRecPtrIsValid(slot->data.restart_lsn));

	/*
	 * Acquire an exclusive lock to prevent the checkpoint process from
	 * concurrently calculating the minimum slot LSN (see
	 * CheckPointReplicationSlots), ensuring that if WAL reservation occurs
	 * first, the checkpoint must wait for the restart_lsn update before
	 * calculating the minimum LSN.
	 *
	 * 获取排他锁，防止检查点进程同时计算槽的最小 LSN（见 CheckPointReplicationSlots）。这样如果先做了 WAL 保留，检查点就必须等 restart_lsn 更新之后再计算最小 LSN。
	 *
	 * Note: Unlike ReplicationSlotReserveWal(), this lock does not protect a
	 * newly synced slot from being invalidated if a concurrent checkpoint has
	 * invoked CheckPointReplicationSlots() before the WAL reservation here.
	 * This can happen because the initial restart_lsn received from the
	 * remote server can precede the redo pointer. Therefore, when selecting
	 * the initial restart_lsn, we consider using the redo pointer or the
	 * minimum slot LSN (if those values are greater than the remote
	 * restart_lsn) instead of relying solely on the remote value.
	 *
	 * 注意：与 ReplicationSlotReserveWal() 不同，这把锁不能保护一个新同步的槽，使其免于在并发检查点于此处保留 WAL 之前调用了 CheckPointReplicationSlots() 时被失效。这是因为从远端收到的初始 restart_lsn 可能早于重做指针。因此选择初始 restart_lsn 时，会考虑使用重做指针或槽的最小 LSN（如果它们大于远端 restart_lsn），而不是只依赖远端的值。
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/*
	 * Determine the minimum non-removable LSN by comparing the redo pointer
	 * with the minimum slot LSN.
	 *
	 * 通过比较重做指针和槽的最小 LSN，确定最小的不可删除 LSN。
	 *
	 * The minimum slot LSN is considered because the redo pointer advances at
	 * every checkpoint, even when replication slots are present on the
	 * standby. In such scenarios, the redo pointer can exceed the remote
	 * restart_lsn, while WALs preceding the remote restart_lsn remain
	 * protected by a local replication slot.
	 *
	 * 要考虑槽的最小 LSN，是因为即使备库上有复制槽，重做指针也会在每次检查点前进。这时重做指针可能超过远端 restart_lsn，而远端 restart_lsn 之前的 WAL 仍由本地复制槽保护着。
	 */
	min_safe_lsn = GetRedoRecPtr();
	slot_min_lsn = XLogGetReplicationSlotMinimumLSN();

	if (XLogRecPtrIsValid(slot_min_lsn) && min_safe_lsn > slot_min_lsn)
		min_safe_lsn = slot_min_lsn;

	/*
	 * If the minimum safe LSN is greater than the given restart_lsn, use it
	 * as the initial restart_lsn for the newly synced slot. Otherwise, use
	 * the given remote restart_lsn.
	 *
	 * 如果最小安全 LSN 大于给定的 restart_lsn，就用它作为新同步槽的初始 restart_lsn；否则使用给定的远端 restart_lsn。
	 */
	SpinLockAcquire(&slot->mutex);
	slot->data.restart_lsn = Max(restart_lsn, min_safe_lsn);
	SpinLockRelease(&slot->mutex);

	ReplicationSlotsComputeRequiredLSN();

	XLByteToSeg(slot->data.restart_lsn, segno, wal_segment_size);
	if (XLogGetLastRemovedSegno() >= segno)
		elog(ERROR, "WAL required by replication slot %s has been removed concurrently",
			 NameStr(slot->data.name));

	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * If the remote restart_lsn and catalog_xmin have caught up with the
 * local ones, then update the LSNs and persist the local synced slot for
 * future synchronization; otherwise, do nothing.
 *
 * 如果远端的 restart_lsn 和 catalog_xmin 已经赶上本地的值，就更新 LSN 并持久化本地已同步槽，供以后同步；否则什么都不做。
 *
 * Return true if the slot is marked as RS_PERSISTENT (sync-ready), otherwise
 * false.
 *
 * 如果槽被标为 RS_PERSISTENT（已可同步）则返回真，否则返回假。
 */
static bool
update_and_persist_local_synced_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot = MyReplicationSlot;
	bool		found_consistent_snapshot = false;
	bool		remote_slot_precedes = false;

	(void) update_local_synced_slot(remote_slot, remote_dbid,
									&found_consistent_snapshot,
									&remote_slot_precedes);

	/*
	 * Check if the primary server has caught up. Refer to the comment atop
	 * the file for details on this check.
	 *
	 * 检查主服务器是否已经赶上来。这项检查的细节见文件开头的注释。
	 */
	if (remote_slot_precedes)
	{
		/*
		 * The remote slot didn't catch up to locally reserved position.
		 *
		 * 远端槽没有赶上本地已保留的位置。
		 *
		 * We do not drop the slot because the restart_lsn can be ahead of the
		 * current location when recreating the slot in the next cycle. It may
		 * take more time to create such a slot. Therefore, we keep this slot
		 * and attempt the synchronization in the next cycle.
		 *
		 * 不删除这个槽，因为下一轮重建槽时，restart_lsn 可能超前于当前位置。创建这样的槽可能需要更多时间。因此保留这个槽，下一轮再尝试同步。
		 */
		return false;
	}

	/*
	 * Don't persist the slot if it cannot reach the consistent point from the
	 * restart_lsn. See comments atop this file.
	 *
	 * 如果从 restart_lsn 到不了一致点，就不要把槽持久化。参见本文件开头的注释。
	 */
	if (!found_consistent_snapshot)
	{
		ereport(LOG,
				errmsg("could not synchronize replication slot \"%s\"", remote_slot->name),
				errdetail("Synchronization could lead to data loss, because the standby could not build a consistent snapshot to decode WALs at LSN %X/%X.",
						  LSN_FORMAT_ARGS(slot->data.restart_lsn)));

		return false;
	}

	ReplicationSlotPersist();

	ereport(LOG,
			errmsg("newly created replication slot \"%s\" is sync-ready now",
				   remote_slot->name));

	return true;
}

/*
 * Synchronize a single slot to the given position.
 *
 * 把单个槽同步到给定位置。
 *
 * This creates a new slot if there is no existing one and updates the
 * metadata of the slot as per the data received from the primary server.
 *
 * 如果本地还没有这个槽就创建一个，并按照从主服务器收到的数据更新槽的元数据。
 *
 * The slot is created as a temporary slot and stays in the same state until the
 * remote_slot catches up with locally reserved position and local slot is
 * updated. The slot is then persisted and is considered as sync-ready for
 * periodic syncs.
 *
 * 槽先创建为临时槽，并保持这个状态，直到远端槽赶上本地已保留的位置并且本地槽已更新。然后把槽持久化，并视为可以定期同步。
 *
 * Returns TRUE if the local slot is updated.
 *
 * 如果本地槽被更新了，则返回真。
 */
static bool
synchronize_one_slot(RemoteSlot *remote_slot, Oid remote_dbid)
{
	ReplicationSlot *slot;
	XLogRecPtr	latestFlushPtr;
	bool		slot_updated = false;

	/*
	 * Make sure that concerned WAL is received and flushed before syncing
	 * slot to target lsn received from the primary server.
	 *
	 * 在把槽同步到从主服务器收到的目标 LSN 之前，确保相关 WAL 已经收到并刷盘。
	 */
	latestFlushPtr = GetStandbyFlushRecPtr(NULL);
	if (remote_slot->confirmed_lsn > latestFlushPtr)
	{
		/*
		 * Can get here only if GUC 'synchronized_standby_slots' on the
		 * primary server was not configured correctly.
		 *
		 * 只有主服务器上的 GUC synchronized_standby_slots 配置不正确时，才会走到这里。
		 */
		ereport(AmLogicalSlotSyncWorkerProcess() ? LOG : ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("skipping slot synchronization because the received slot sync"
					   " LSN %X/%X for slot \"%s\" is ahead of the standby position %X/%X",
					   LSN_FORMAT_ARGS(remote_slot->confirmed_lsn),
					   remote_slot->name,
					   LSN_FORMAT_ARGS(latestFlushPtr)));

		return false;
	}

	/* Search for the named slot
	 *
	 * 查找指定名字的槽。
	 */
	if ((slot = SearchNamedReplicationSlot(remote_slot->name, true)))
	{
		bool		synced;

		SpinLockAcquire(&slot->mutex);
		synced = slot->data.synced;
		SpinLockRelease(&slot->mutex);

		/* User-created slot with the same name exists, raise ERROR.
		 *
		 * 已存在同名的用户创建的槽，报 ERROR。
		 */
		if (!synced)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("exiting from slot synchronization because same"
						   " name slot \"%s\" already exists on the standby",
						   remote_slot->name));

		/*
		 * The slot has been synchronized before.
		 *
		 * 这个槽以前已经被同步过。
		 *
		 * It is important to acquire the slot here before checking
		 * invalidation. If we don't acquire the slot first, there could be a
		 * race condition that the local slot could be invalidated just after
		 * checking the 'invalidated' flag here and we could end up
		 * overwriting 'invalidated' flag to remote_slot's value. See
		 * InvalidatePossiblyObsoleteSlot() where it invalidates slot directly
		 * if the slot is not acquired by other processes.
		 *
		 * 在检查失效之前必须先占用这个槽。如果不先占用，可能出现竞态：刚检查完 invalidated 标志，本地槽就被失效，结果我们会用远端槽的值覆盖 invalidated 标志。参见 InvalidatePossiblyObsoleteSlot()，槽未被其他进程占用时它会直接使槽失效。
		 *
		 * XXX: If it ever turns out that slot acquire/release is costly for
		 * cases when none of the slot properties is changed then we can do a
		 * pre-check to ensure that at least one of the slot properties is
		 * changed before acquiring the slot.
		 *
		 * 待查：如果将来发现在槽的属性都没变时，占用和释放槽的代价太高，可以先做一次预检查，确认至少有一项槽属性变了，再占用槽。
		 */
		ReplicationSlotAcquire(remote_slot->name, true, false);

		Assert(slot == MyReplicationSlot);

		/*
		 * Copy the invalidation cause from remote only if local slot is not
		 * invalidated locally, we don't want to overwrite existing one.
		 *
		 * 只有本地槽不是在本地失效的，才从远端复制失效原因，不想覆盖已有的原因。
		 */
		if (slot->data.invalidated == RS_INVAL_NONE &&
			remote_slot->invalidated != RS_INVAL_NONE)
		{
			SpinLockAcquire(&slot->mutex);
			slot->data.invalidated = remote_slot->invalidated;
			SpinLockRelease(&slot->mutex);

			/* Make sure the invalidated state persists across server restart
			 *
			 * 确保失效状态在服务器重启后仍然保留。
			 */
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();

			slot_updated = true;
		}

		/* Skip the sync of an invalidated slot
		 *
		 * 跳过对已失效槽的同步。
		 */
		if (slot->data.invalidated != RS_INVAL_NONE)
		{
			ReplicationSlotRelease();
			return slot_updated;
		}

		/* Slot not ready yet, let's attempt to make it sync-ready now.
		 *
		 * 槽还没就绪，现在尝试让它变得可以同步。
		 */
		if (slot->data.persistency == RS_TEMPORARY)
		{
			slot_updated = update_and_persist_local_synced_slot(remote_slot,
																remote_dbid);
		}

		/* Slot ready for sync, so sync it.
		 *
		 * 槽已可同步，于是同步它。
		 */
		else
		{
			/*
			 * Sanity check: As long as the invalidations are handled
			 * appropriately as above, this should never happen.
			 *
			 * 健全性检查：只要上面正确处理了失效，这里就绝不应该发生。
			 *
			 * We don't need to check restart_lsn here. See the comments in
			 * update_local_synced_slot() for details.
			 *
			 * 这里不必检查 restart_lsn。详情见 update_local_synced_slot() 中的注释。
			 */
			if (remote_slot->confirmed_lsn < slot->data.confirmed_flush)
				ereport(ERROR,
						errmsg_internal("cannot synchronize local slot \"%s\"",
										remote_slot->name),
						errdetail_internal("Local slot's start streaming location LSN(%X/%X) is ahead of remote slot's LSN(%X/%X).",
										   LSN_FORMAT_ARGS(slot->data.confirmed_flush),
										   LSN_FORMAT_ARGS(remote_slot->confirmed_lsn)));

			slot_updated = update_local_synced_slot(remote_slot, remote_dbid,
													NULL, NULL);
		}
	}
	/* Otherwise create the slot first.
	 *
	 * 否则先创建这个槽。
	 */
	else
	{
		NameData	plugin_name;
		TransactionId xmin_horizon = InvalidTransactionId;

		/* Skip creating the local slot if remote_slot is invalidated already
		 *
		 * 如果远端槽已经失效，就不要创建本地槽。
		 */
		if (remote_slot->invalidated != RS_INVAL_NONE)
			return false;

		/*
		 * We create temporary slots instead of ephemeral slots here because
		 * we want the slots to survive after releasing them. This is done to
		 * avoid dropping and re-creating the slots in each synchronization
		 * cycle if the restart_lsn or catalog_xmin of the remote slot has not
		 * caught up.
		 *
		 * 这里创建临时槽而不是短暂槽，是因为释放之后仍希望槽继续存在。这样如果远端槽的 restart_lsn 或 catalog_xmin 还没赶上来，就不必在每一轮同步里删除再重建。
		 */
		ReplicationSlotCreate(remote_slot->name, true, RS_TEMPORARY,
							  remote_slot->two_phase,
							  remote_slot->failover,
							  true);

		/* For shorter lines.
		 *
		 * 为了缩短代码行。
		 */
		slot = MyReplicationSlot;

		/* Avoid expensive operations while holding a spinlock.
		 *
		 * 持有自旋锁时避免昂贵操作。
		 */
		namestrcpy(&plugin_name, remote_slot->plugin);

		SpinLockAcquire(&slot->mutex);
		slot->data.database = remote_dbid;
		slot->data.plugin = plugin_name;
		SpinLockRelease(&slot->mutex);

		reserve_wal_for_local_slot(remote_slot->restart_lsn);

		LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		xmin_horizon = GetOldestSafeDecodingTransactionId(true);
		SpinLockAcquire(&slot->mutex);
		slot->effective_catalog_xmin = xmin_horizon;
		slot->data.catalog_xmin = xmin_horizon;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotsComputeRequiredXmin(true);
		LWLockRelease(ProcArrayLock);
		LWLockRelease(ReplicationSlotControlLock);

		update_and_persist_local_synced_slot(remote_slot, remote_dbid);

		slot_updated = true;
	}

	ReplicationSlotRelease();

	return slot_updated;
}

/*
 * Synchronize slots.
 *
 * 同步各个槽。
 *
 * Gets the failover logical slots info from the primary server and updates
 * the slots locally. Creates the slots if not present on the standby.
 *
 * 从主服务器取得故障转移逻辑槽的信息，并在本地更新这些槽。备库上没有的槽会被创建。
 *
 * Returns TRUE if any of the slots gets updated in this sync-cycle.
 *
 * 如果本轮同步中有任何槽被更新，则返回真。
 */
static bool
synchronize_slots(WalReceiverConn *wrconn)
{
#define SLOTSYNC_COLUMN_COUNT 10
	Oid			slotRow[SLOTSYNC_COLUMN_COUNT] = {TEXTOID, TEXTOID, LSNOID,
	LSNOID, XIDOID, BOOLOID, LSNOID, BOOLOID, TEXTOID, TEXTOID};

	WalRcvExecResult *res;
	TupleTableSlot *tupslot;
	List	   *remote_slot_list = NIL;
	bool		some_slot_updated = false;
	bool		started_tx = false;
	const char *query = "SELECT slot_name, plugin, confirmed_flush_lsn,"
		" restart_lsn, catalog_xmin, two_phase, two_phase_at, failover,"
		" database, invalidation_reason"
		" FROM pg_catalog.pg_replication_slots"
		" WHERE failover and NOT temporary";

	/* The syscache access in walrcv_exec() needs a transaction env.
	 *
	 * walrcv_exec() 里访问系统缓存需要事务环境。
	 */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/* Execute the query
	 *
	 * 执行查询。
	 */
	res = walrcv_exec(wrconn, query, SLOTSYNC_COLUMN_COUNT, slotRow);
	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not fetch failover logical slots info from the primary server: %s",
					   res->err));

	/* Construct the remote_slot tuple and synchronize each slot locally
	 *
	 * 构造 remote_slot 元组，并在本地逐个同步。
	 */
	tupslot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	while (tuplestore_gettupleslot(res->tuplestore, true, false, tupslot))
	{
		bool		isnull;
		RemoteSlot *remote_slot = palloc0(sizeof(RemoteSlot));
		Datum		d;
		int			col = 0;

		remote_slot->name = TextDatumGetCString(slot_getattr(tupslot, ++col,
															 &isnull));
		Assert(!isnull);

		remote_slot->plugin = TextDatumGetCString(slot_getattr(tupslot, ++col,
															   &isnull));
		Assert(!isnull);

		/*
		 * It is possible to get null values for LSN and Xmin if slot is
		 * invalidated on the primary server, so handle accordingly.
		 *
		 * 如果槽在主服务器上已失效，LSN 和 xmin 可能是空值，要相应处理。
		 */
		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->confirmed_lsn = isnull ? InvalidXLogRecPtr :
			DatumGetLSN(d);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->restart_lsn = isnull ? InvalidXLogRecPtr : DatumGetLSN(d);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->catalog_xmin = isnull ? InvalidTransactionId :
			DatumGetTransactionId(d);

		remote_slot->two_phase = DatumGetBool(slot_getattr(tupslot, ++col,
														   &isnull));
		Assert(!isnull);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->two_phase_at = isnull ? InvalidXLogRecPtr : DatumGetLSN(d);

		remote_slot->failover = DatumGetBool(slot_getattr(tupslot, ++col,
														  &isnull));
		Assert(!isnull);

		remote_slot->database = TextDatumGetCString(slot_getattr(tupslot,
																 ++col, &isnull));
		Assert(!isnull);

		d = slot_getattr(tupslot, ++col, &isnull);
		remote_slot->invalidated = isnull ? RS_INVAL_NONE :
			GetSlotInvalidationCause(TextDatumGetCString(d));

		/* Sanity check
		 *
		 * 健全性检查。
		 */
		Assert(col == SLOTSYNC_COLUMN_COUNT);

		/*
		 * If restart_lsn, confirmed_lsn or catalog_xmin is invalid but the
		 * slot is valid, that means we have fetched the remote_slot in its
		 * RS_EPHEMERAL state. In such a case, don't sync it; we can always
		 * sync it in the next sync cycle when the remote_slot is persisted
		 * and has valid lsn(s) and xmin values.
		 *
		 * 如果 restart_lsn、confirmed_lsn 或 catalog_xmin 无效，但槽本身有效，说明取到的远端槽处于 RS_EPHEMERAL 状态。这时不要同步它；下一轮远端槽被持久化并且 LSN 和 xmin 有效时，总可以再同步。
		 *
		 * XXX: In future, if we plan to expose 'slot->data.persistency' in
		 * pg_replication_slots view, then we can avoid fetching RS_EPHEMERAL
		 * slots in the first place.
		 *
		 * 待查：将来如果打算在 pg_replication_slots 视图中暴露槽数据的 persistency，就可以一开始就不去取 RS_EPHEMERAL 槽。
		 */
		if ((XLogRecPtrIsInvalid(remote_slot->restart_lsn) ||
			 XLogRecPtrIsInvalid(remote_slot->confirmed_lsn) ||
			 !TransactionIdIsValid(remote_slot->catalog_xmin)) &&
			remote_slot->invalidated == RS_INVAL_NONE)
			pfree(remote_slot);
		else
			/* Create list of remote slots
			 *
			 * 建立远端槽的列表。
			 */
			remote_slot_list = lappend(remote_slot_list, remote_slot);

		ExecClearTuple(tupslot);
	}

	/* Drop local slots that no longer need to be synced.
	 *
	 * 删除不再需要同步的本地槽。
	 */
	drop_local_obsolete_slots(remote_slot_list);

	/* Now sync the slots locally
	 *
	 * 现在在本地同步这些槽。
	 */
	foreach_ptr(RemoteSlot, remote_slot, remote_slot_list)
	{
		Oid			remote_dbid = get_database_oid(remote_slot->database, false);

		/*
		 * Use shared lock to prevent a conflict with
		 * ReplicationSlotsDropDBSlots(), trying to drop the same slot during
		 * a drop-database operation.
		 *
		 * 使用共享锁，避免与 ReplicationSlotsDropDBSlots() 冲突，后者在删除数据库时可能试图删除同一个槽。
		 */
		LockSharedObject(DatabaseRelationId, remote_dbid, 0, AccessShareLock);

		some_slot_updated |= synchronize_one_slot(remote_slot, remote_dbid);

		UnlockSharedObject(DatabaseRelationId, remote_dbid, 0, AccessShareLock);
	}

	/* We are done, free remote_slot_list elements
	 *
	 * 已经完成，释放 remote_slot_list 的元素。
	 */
	list_free_deep(remote_slot_list);

	walrcv_clear_result(res);

	if (started_tx)
		CommitTransactionCommand();

	return some_slot_updated;
}

/*
 * Checks the remote server info.
 *
 * 检查远端服务器的信息。
 *
 * We ensure that the 'primary_slot_name' exists on the remote server and the
 * remote server is not a standby node.
 *
 * 确认 primary_slot_name 在远端服务器上存在，并且远端服务器不是备库节点。
 */
static void
validate_remote_info(WalReceiverConn *wrconn)
{
#define PRIMARY_INFO_OUTPUT_COL_COUNT 2
	WalRcvExecResult *res;
	Oid			slotRow[PRIMARY_INFO_OUTPUT_COL_COUNT] = {BOOLOID, BOOLOID};
	StringInfoData cmd;
	bool		isnull;
	TupleTableSlot *tupslot;
	bool		remote_in_recovery;
	bool		primary_slot_valid;
	bool		started_tx = false;

	initStringInfo(&cmd);
	appendStringInfo(&cmd,
					 "SELECT pg_is_in_recovery(), count(*) = 1"
					 " FROM pg_catalog.pg_replication_slots"
					 " WHERE slot_type='physical' AND slot_name=%s",
					 quote_literal_cstr(PrimarySlotName));

	/* The syscache access in walrcv_exec() needs a transaction env.
	 *
	 * walrcv_exec() 里访问系统缓存需要事务环境。
	 */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	res = walrcv_exec(wrconn, cmd.data, PRIMARY_INFO_OUTPUT_COL_COUNT, slotRow);
	pfree(cmd.data);

	if (res->status != WALRCV_OK_TUPLES)
		ereport(ERROR,
				errmsg("could not fetch primary slot name \"%s\" info from the primary server: %s",
					   PrimarySlotName, res->err),
				errhint("Check if \"primary_slot_name\" is configured correctly."));

	tupslot = MakeSingleTupleTableSlot(res->tupledesc, &TTSOpsMinimalTuple);
	if (!tuplestore_gettupleslot(res->tuplestore, true, false, tupslot))
		elog(ERROR,
			 "failed to fetch tuple for the primary server slot specified by \"primary_slot_name\"");

	remote_in_recovery = DatumGetBool(slot_getattr(tupslot, 1, &isnull));
	Assert(!isnull);

	/*
	 * Slot sync is currently not supported on a cascading standby. This is
	 * because if we allow it, the primary server needs to wait for all the
	 * cascading standbys, otherwise, logical subscribers can still be ahead
	 * of one of the cascading standbys which we plan to promote. Thus, to
	 * avoid this additional complexity, we restrict it for the time being.
	 *
	 * 目前不支持在级联备库上做槽同步。如果允许，主服务器就必须等待所有级联备库，否则逻辑订阅者仍可能超前于我们打算提升的某一台级联备库。为避免这种额外的复杂性，暂时加以限制。
	 */
	if (remote_in_recovery)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot synchronize replication slots from a standby server"));

	primary_slot_valid = DatumGetBool(slot_getattr(tupslot, 2, &isnull));
	Assert(!isnull);

	if (!primary_slot_valid)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: second %s is a GUC variable name
		 *
		 * 供翻译者：第二个 %s 是 GUC 变量名。
		 */
				errmsg("replication slot \"%s\" specified by \"%s\" does not exist on primary server",
					   PrimarySlotName, "primary_slot_name"));

	ExecClearTuple(tupslot);
	walrcv_clear_result(res);

	if (started_tx)
		CommitTransactionCommand();
}

/*
 * Checks if dbname is specified in 'primary_conninfo'.
 *
 * 检查 primary_conninfo 里是否指定了 dbname。
 *
 * Error out if not specified otherwise return it.
 *
 * 如果没有指定就报错，否则返回它。
 */
char *
CheckAndGetDbnameFromConninfo(void)
{
	char	   *dbname;

	/*
	 * The slot synchronization needs a database connection for walrcv_exec to
	 * work.
	 *
	 * 槽同步需要数据库连接，walrcv_exec 才能工作。
	 */
	dbname = walrcv_get_dbname_from_conninfo(PrimaryConnInfo);
	if (dbname == NULL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),

		/*
		 * translator: first %s is a connection option; second %s is a GUC
		 * variable name
		 *
		 * 供翻译者：第一个 %s 是连接选项，第二个 %s 是 GUC 变量名。
		 */
				errmsg("replication slot synchronization requires \"%s\" to be specified in \"%s\"",
					   "dbname", "primary_conninfo"));
	return dbname;
}

/*
 * Return true if all necessary GUCs for slot synchronization are set
 * appropriately, otherwise, return false.
 *
 * 如果槽同步所需的 GUC 都已正确设置则返回真，否则返回假。
 */
bool
ValidateSlotSyncParams(int elevel)
{
	/*
	 * Logical slot sync/creation requires wal_level >= logical.
	 *
	 * 同步或创建逻辑槽要求 wal_level 至少为 logical。
	 */
	if (wal_level < WAL_LEVEL_LOGICAL)
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("replication slot synchronization requires \"wal_level\" >= \"logical\""));
		return false;
	}

	/*
	 * A physical replication slot(primary_slot_name) is required on the
	 * primary to ensure that the rows needed by the standby are not removed
	 * after restarting, so that the synchronized slot on the standby will not
	 * be invalidated.
	 *
	 * 主库上必须有一个物理复制槽 primary_slot_name，以保证重启后备库需要的行不会被删除，这样备库上已同步的槽才不会失效。
	 */
	if (PrimarySlotName == NULL || *PrimarySlotName == '\0')
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: %s is a GUC variable name
		 *
		 * 供翻译者：%s 是 GUC 变量名。
		 */
				errmsg("replication slot synchronization requires \"%s\" to be set", "primary_slot_name"));
		return false;
	}

	/*
	 * hot_standby_feedback must be enabled to cooperate with the physical
	 * replication slot, which allows informing the primary about the xmin and
	 * catalog_xmin values on the standby.
	 *
	 * 必须启用 hot_standby_feedback，才能与物理复制槽配合，从而把备库上的 xmin 和 catalog_xmin 告知主库。
	 */
	if (!hot_standby_feedback)
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: %s is a GUC variable name
		 *
		 * 供翻译者：%s 是 GUC 变量名。
		 */
				errmsg("replication slot synchronization requires \"%s\" to be enabled",
					   "hot_standby_feedback"));
		return false;
	}

	/*
	 * The primary_conninfo is required to make connection to primary for
	 * getting slots information.
	 *
	 * 需要 primary_conninfo，才能连接主库并取得槽信息。
	 */
	if (PrimaryConnInfo == NULL || *PrimaryConnInfo == '\0')
	{
		ereport(elevel,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/* translator: %s is a GUC variable name
		 *
		 * 供翻译者：%s 是 GUC 变量名。
		 */
				errmsg("replication slot synchronization requires \"%s\" to be set",
					   "primary_conninfo"));
		return false;
	}

	return true;
}

/*
 * Re-read the config file.
 *
 * 重新读取配置文件。
 *
 * Exit if any of the slot sync GUCs have changed. The postmaster will
 * restart it.
 *
 * 如果任一槽同步 GUC 发生了变化就退出。postmaster 会重新启动它。
 */
static void
slotsync_reread_config(void)
{
	char	   *old_primary_conninfo = pstrdup(PrimaryConnInfo);
	char	   *old_primary_slotname = pstrdup(PrimarySlotName);
	bool		old_sync_replication_slots = sync_replication_slots;
	bool		old_hot_standby_feedback = hot_standby_feedback;
	bool		conninfo_changed;
	bool		primary_slotname_changed;

	Assert(sync_replication_slots);

	ConfigReloadPending = false;
	ProcessConfigFile(PGC_SIGHUP);

	conninfo_changed = strcmp(old_primary_conninfo, PrimaryConnInfo) != 0;
	primary_slotname_changed = strcmp(old_primary_slotname, PrimarySlotName) != 0;
	pfree(old_primary_conninfo);
	pfree(old_primary_slotname);

	if (old_sync_replication_slots != sync_replication_slots)
	{
		ereport(LOG,
		/* translator: %s is a GUC variable name
		 *
		 * 供翻译者：%s 是 GUC 变量名。
		 */
				errmsg("replication slot synchronization worker will shut down because \"%s\" is disabled", "sync_replication_slots"));
		proc_exit(0);
	}

	if (conninfo_changed ||
		primary_slotname_changed ||
		(old_hot_standby_feedback != hot_standby_feedback))
	{
		ereport(LOG,
				errmsg("replication slot synchronization worker will restart because of a parameter change"));

		/*
		 * Reset the last-start time for this worker so that the postmaster
		 * can restart it without waiting for SLOTSYNC_RESTART_INTERVAL_SEC.
		 *
		 * 重置这个 worker 的上次启动时间，这样 postmaster 不必等待 SLOTSYNC_RESTART_INTERVAL_SEC 就能重启它。
		 */
		SlotSyncCtx->last_start_time = 0;

		proc_exit(0);
	}

}

/*
 * Interrupt handler for main loop of slot sync worker.
 *
 * slot sync worker 主循环的中断处理函数。
 */
static void
ProcessSlotSyncInterrupts(WalReceiverConn *wrconn)
{
	CHECK_FOR_INTERRUPTS();

	if (SlotSyncCtx->stopSignaled)
	{
		ereport(LOG,
				errmsg("replication slot synchronization worker is shutting down because promotion is triggered"));

		proc_exit(0);
	}

	if (ConfigReloadPending)
		slotsync_reread_config();
}

/*
 * Connection cleanup function for slotsync worker.
 *
 * slotsync worker 的连接清理函数。
 *
 * Called on slotsync worker exit.
 *
 * 在 slotsync worker 退出时调用。
 */
static void
slotsync_worker_disconnect(int code, Datum arg)
{
	WalReceiverConn *wrconn = (WalReceiverConn *) DatumGetPointer(arg);

	walrcv_disconnect(wrconn);
}

/*
 * Cleanup function for slotsync worker.
 *
 * slotsync worker 的清理函数。
 *
 * Called on slotsync worker exit.
 *
 * 在 slotsync worker 退出时调用。
 */
static void
slotsync_worker_onexit(int code, Datum arg)
{
	/*
	 * We need to do slots cleanup here just like WalSndErrorCleanup() does.
	 *
	 * 这里需要像 WalSndErrorCleanup() 那样清理槽。
	 *
	 * The startup process during promotion invokes ShutDownSlotSync() which
	 * waits for slot sync to finish and it does that by checking the
	 * 'syncing' flag. Thus the slot sync worker must be done with slots'
	 * release and cleanup to avoid any dangling temporary slots or active
	 * slots before it marks itself as finished syncing.
	 *
	 * 提升期间启动进程会调用 ShutDownSlotSync()，它通过检查 syncing 标志等待槽同步结束。因此 slot sync worker 在把自己标为同步结束之前，必须完成槽的释放和清理，以免留下悬空的临时槽或仍被占用的槽。
	 */

	/* Make sure active replication slots are released
	 *
	 * 确保活动的复制槽已被释放。
	 */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* Also cleanup the temporary slots.
	 *
	 * 同时清理临时槽。
	 */
	ReplicationSlotCleanup(false);

	SpinLockAcquire(&SlotSyncCtx->mutex);

	SlotSyncCtx->pid = InvalidPid;

	/*
	 * If syncing_slots is true, it indicates that the process errored out
	 * without resetting the flag. So, we need to clean up shared memory and
	 * reset the flag here.
	 *
	 * 如果 syncing_slots 为真，说明进程出错时没有复位该标志。因此要在这里清理共享内存并复位标志。
	 */
	if (syncing_slots)
	{
		SlotSyncCtx->syncing = false;
		syncing_slots = false;
	}

	SpinLockRelease(&SlotSyncCtx->mutex);
}

/*
 * Sleep for long enough that we believe it's likely that the slots on primary
 * get updated.
 *
 * 睡眠足够长的时间，使我们认为主库上的槽很可能已经更新。
 *
 * If there is no slot activity the wait time between sync-cycles will double
 * (to a maximum of 30s). If there is some slot activity the wait time between
 * sync-cycles is reset to the minimum (200ms).
 *
 * 如果没有槽活动，两次同步之间的等待时间会加倍，最多到 30 秒。如果有槽活动，等待时间会重置为最小值 200 毫秒。
 */
static void
wait_for_slot_activity(bool some_slot_updated)
{
	int			rc;

	if (!some_slot_updated)
	{
		/*
		 * No slots were updated, so double the sleep time, but not beyond the
		 * maximum allowable value.
		 *
		 * 没有槽被更新，因此把睡眠时间加倍，但不超过允许的最大值。
		 */
		sleep_ms = Min(sleep_ms * 2, MAX_SLOTSYNC_WORKER_NAPTIME_MS);
	}
	else
	{
		/*
		 * Some slots were updated since the last sleep, so reset the sleep
		 * time.
		 *
		 * 上次睡眠之后有槽被更新，因此重置睡眠时间。
		 */
		sleep_ms = MIN_SLOTSYNC_WORKER_NAPTIME_MS;
	}

	rc = WaitLatch(MyLatch,
				   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				   sleep_ms,
				   WAIT_EVENT_REPLICATION_SLOTSYNC_MAIN);

	if (rc & WL_LATCH_SET)
		ResetLatch(MyLatch);
}

/*
 * Emit an error if a promotion or a concurrent sync call is in progress.
 * Otherwise, advertise that a sync is in progress.
 *
 * 如果正在提升，或者已有并发的同步调用，就报错。否则宣告一次同步正在进行。
 */
static void
check_and_set_sync_info(pid_t worker_pid)
{
	SpinLockAcquire(&SlotSyncCtx->mutex);

	/* The worker pid must not be already assigned in SlotSyncCtx
	 *
	 * SlotSyncCtx 里不能已经分配了 worker 的 pid。
	 */
	Assert(worker_pid == InvalidPid || SlotSyncCtx->pid == InvalidPid);

	/*
	 * Emit an error if startup process signaled the slot sync machinery to
	 * stop. See comments atop SlotSyncCtxStruct.
	 *
	 * 如果启动进程已经通知槽同步机制停止，就报错。参见 SlotSyncCtxStruct 上方的注释。
	 */
	if (SlotSyncCtx->stopSignaled)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot synchronize replication slots when standby promotion is ongoing"));
	}

	if (SlotSyncCtx->syncing)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot synchronize replication slots concurrently"));
	}

	SlotSyncCtx->syncing = true;

	/*
	 * Advertise the required PID so that the startup process can kill the
	 * slot sync worker on promotion.
	 *
	 * 公布所需的 PID，以便启动进程在提升时杀掉 slot sync worker。
	 */
	SlotSyncCtx->pid = worker_pid;

	SpinLockRelease(&SlotSyncCtx->mutex);

	syncing_slots = true;
}

/*
 * Reset syncing flag.
 *
 * 复位 syncing 标志。
 */
static void
reset_syncing_flag()
{
	SpinLockAcquire(&SlotSyncCtx->mutex);
	SlotSyncCtx->syncing = false;
	SpinLockRelease(&SlotSyncCtx->mutex);

	syncing_slots = false;
}

/*
 * The main loop of our worker process.
 *
 * 本 worker 进程的主循环。
 *
 * It connects to the primary server, fetches logical failover slots
 * information periodically in order to create and sync the slots.
 *
 * 它连接主服务器，定期获取逻辑故障转移槽的信息，以便创建并同步这些槽。
 */
void
ReplSlotSyncWorkerMain(const void *startup_data, size_t startup_data_len)
{
	WalReceiverConn *wrconn = NULL;
	char	   *dbname;
	char	   *err;
	sigjmp_buf	local_sigjmp_buf;
	StringInfoData app_name;

	Assert(startup_data_len == 0);

	MyBackendType = B_SLOTSYNC_WORKER;

	init_ps_display(NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we access any shared memory.
	 *
	 * 在共享内存中为本后端创建 PGPROC 结构。访问任何共享内存之前必须先做这件事。
	 */
	InitProcess();

	/*
	 * Early initialization.
	 *
	 * 早期初始化。
	 */
	BaseInit();

	Assert(SlotSyncCtx != NULL);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * 如果遇到异常，处理从这里继续。
	 *
	 * We just need to clean up, report the error, and go away.
	 *
	 * 只需清理、报告错误，然后离开。
	 *
	 * If we do not have this handling here, then since this worker process
	 * operates at the bottom of the exception stack, ERRORs turn into FATALs.
	 * Therefore, we create our own exception handler to catch ERRORs.
	 *
	 * 如果这里没有这种处理，由于本 worker 进程处在异常栈的底部，ERROR 会变成 FATAL。因此我们建立自己的异常处理来捕获 ERROR。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand
		 *
		 * 因为没有使用 PG_TRY，必须手工重置错误栈。
		 */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up
		 *
		 * 清理期间禁止中断。
		 */
		HOLD_INTERRUPTS();

		/* Report the error to the server log
		 *
		 * 把错误报告到服务器日志。
		 */
		EmitErrorReport();

		/*
		 * We can now go away.  Note that because we called InitProcess, a
		 * callback was registered to do ProcKill, which will clean up
		 * necessary state.
		 *
		 * 现在可以离开了。注意因为调用了 InitProcess，已经注册了执行 ProcKill 的回调，它会清理必要的状态。
		 */
		proc_exit(0);
	}

	/* We can now handle ereport(ERROR)
	 *
	 * 现在可以处理 ereport(ERROR) 了。
	 */
	PG_exception_stack = &local_sigjmp_buf;

	/* Setup signal handling
	 *
	 * 设置信号处理。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, StatementCancelHandler);
	pqsignal(SIGTERM, die);
	pqsignal(SIGFPE, FloatExceptionHandler);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	check_and_set_sync_info(MyProcPid);

	ereport(LOG, errmsg("slot sync worker started"));

	/* Register it as soon as SlotSyncCtx->pid is initialized.
	 *
	 * SlotSyncCtx 的 pid 一初始化就注册它。
	 */
	before_shmem_exit(slotsync_worker_onexit, (Datum) 0);

	/*
	 * Establishes SIGALRM handler and initialize timeout module. It is needed
	 * by InitPostgres to register different timeouts.
	 *
	 * 建立 SIGALRM 处理函数并初始化超时模块。InitPostgres 需要它来登记各种超时。
	 */
	InitializeTimeouts();

	/* Load the libpq-specific functions
	 *
	 * 加载 libpq 专用的函数。
	 */
	load_file("libpqwalreceiver", false);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 *
	 * 解除信号阻塞（postmaster 派生我们时把信号阻塞了）。
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. operators).
	 *
	 * 设置始终安全的搜索路径，以免恶意用户把用户代码（例如操作符）重定向到别处。
	 *
	 * It's not strictly necessary since we won't be scanning or writing to
	 * any user table locally, but it's good to retain it here for added
	 * precaution.
	 *
	 * 严格来说并非必需，因为我们不会在本地扫描或写入任何用户表，但留在这里多一层预防是好的。
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	dbname = CheckAndGetDbnameFromConninfo();

	/*
	 * Connect to the database specified by the user in primary_conninfo. We
	 * need a database connection for walrcv_exec to work which we use to
	 * fetch slot information from the remote node. See comments atop
	 * libpqrcv_exec.
	 *
	 * 连接到用户在 primary_conninfo 中指定的数据库。walrcv_exec 需要数据库连接才能工作，我们用它从远端节点获取槽信息。参见 libpqrcv_exec 上方的注释。
	 *
	 * We do not specify a specific user here since the slot sync worker will
	 * operate as a superuser. This is safe because the slot sync worker does
	 * not interact with user tables, eliminating the risk of executing
	 * arbitrary code within triggers.
	 *
	 * 这里不指定具体用户，因为 slot sync worker 将以超级用户身份运行。这是安全的，因为 slot sync worker 不接触用户表，不存在在触发器里执行任意代码的风险。
	 */
	InitPostgres(dbname, InvalidOid, NULL, InvalidOid, 0, NULL);

	SetProcessingMode(NormalProcessing);

	initStringInfo(&app_name);
	if (cluster_name[0])
		appendStringInfo(&app_name, "%s_%s", cluster_name, "slotsync worker");
	else
		appendStringInfoString(&app_name, "slotsync worker");

	/*
	 * Establish the connection to the primary server for slot
	 * synchronization.
	 *
	 * 建立到主服务器的连接，用于槽同步。
	 */
	wrconn = walrcv_connect(PrimaryConnInfo, false, false, false,
							app_name.data, &err);

	if (!wrconn)
		ereport(ERROR,
				errcode(ERRCODE_CONNECTION_FAILURE),
				errmsg("synchronization worker \"%s\" could not connect to the primary server: %s",
					   app_name.data, err));

	pfree(app_name.data);

	/*
	 * Register the disconnection callback.
	 *
	 * 登记断开连接时的回调。
	 *
	 * XXX: This can be combined with previous cleanup registration of
	 * slotsync_worker_onexit() but that will need the connection to be made
	 * global and we want to avoid introducing global for this purpose.
	 *
	 * 待查：这可以和前面登记的 slotsync_worker_onexit() 清理合在一起，但那样需要把连接做成全局变量，我们希望避免为此引入全局变量。
	 */
	before_shmem_exit(slotsync_worker_disconnect, PointerGetDatum(wrconn));

	/*
	 * Using the specified primary server connection, check that we are not a
	 * cascading standby and slot configured in 'primary_slot_name' exists on
	 * the primary server.
	 *
	 * 使用指定的主服务器连接，检查我们不是级联备库，并且 primary_slot_name 中配置的槽在主服务器上存在。
	 */
	validate_remote_info(wrconn);

	/* Main loop to synchronize slots
	 *
	 * 同步槽的主循环。
	 */
	for (;;)
	{
		bool		some_slot_updated = false;

		ProcessSlotSyncInterrupts(wrconn);

		some_slot_updated = synchronize_slots(wrconn);

		wait_for_slot_activity(some_slot_updated);
	}

	/*
	 * The slot sync worker can't get here because it will only stop when it
	 * receives a stop request from the startup process, or when there is an
	 * error.
	 *
	 * slot sync worker 到不了这里，因为它只会在收到启动进程的停止请求时，或者出错时才停止。
	 */
	Assert(false);
}

/*
 * Update the inactive_since property for synced slots.
 *
 * 更新已同步槽的 inactive_since 属性。
 *
 * Note that this function is currently called when we shutdown the slot
 * sync machinery.
 *
 * 注意：目前在关闭槽同步机制时调用本函数。
 */
static void
update_synced_slots_inactive_since(void)
{
	TimestampTz now = 0;

	/*
	 * We need to update inactive_since only when we are promoting standby to
	 * correctly interpret the inactive_since if the standby gets promoted
	 * without a restart. We don't want the slots to appear inactive for a
	 * long time after promotion if they haven't been synchronized recently.
	 * Whoever acquires the slot, i.e., makes the slot active, will reset it.
	 *
	 * 只有在提升备库时才需要更新 inactive_since，以便在备库不重启就被提升时正确解释 inactive_since。如果槽最近没有被同步，我们不希望提升之后它们长时间看起来处于不活动状态。谁占用了槽、也就是使槽变为活动，谁就会把它复位。
	 */
	if (!StandbyMode)
		return;

	/* The slot sync worker or SQL function mustn't be running by now
	 *
	 * 到这时 slot sync worker 或 SQL 函数都不应该还在运行。
	 */
	Assert((SlotSyncCtx->pid == InvalidPid) && !SlotSyncCtx->syncing);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		/* Check if it is a synchronized slot
		 *
		 * 检查它是不是已同步的槽。
		 */
		if (s->in_use && s->data.synced)
		{
			Assert(SlotIsLogical(s));

			/* The slot must not be acquired by any process
			 *
			 * 槽不能被任何进程占用。
			 */
			Assert(s->active_pid == 0);

			/* Use the same inactive_since time for all the slots.
			 *
			 * 所有槽使用同一个 inactive_since 时间。
			 */
			if (now == 0)
				now = GetCurrentTimestamp();

			ReplicationSlotSetInactiveSince(s, now, true);
		}
	}

	LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Shut down the slot sync worker.
 *
 * 关闭 slot sync worker。
 *
 * This function sends signal to shutdown slot sync worker, if required. It
 * also waits till the slot sync worker has exited or
 * pg_sync_replication_slots() has finished.
 *
 * 本函数在需要时向 slot sync worker 发信号让它关闭。它也会一直等到 slot sync worker 退出，或者 pg_sync_replication_slots() 结束。
 */
void
ShutDownSlotSync(void)
{
	pid_t		worker_pid;

	SpinLockAcquire(&SlotSyncCtx->mutex);

	SlotSyncCtx->stopSignaled = true;

	/*
	 * Return if neither the slot sync worker is running nor the function
	 * pg_sync_replication_slots() is executing.
	 *
	 * 如果 slot sync worker 没在运行，并且函数 pg_sync_replication_slots() 也没在执行，就直接返回。
	 */
	if (!SlotSyncCtx->syncing)
	{
		SpinLockRelease(&SlotSyncCtx->mutex);
		update_synced_slots_inactive_since();
		return;
	}

	worker_pid = SlotSyncCtx->pid;

	SpinLockRelease(&SlotSyncCtx->mutex);

	/*
	 * Signal slotsync worker if it was still running. The worker will stop
	 * upon detecting that the stopSignaled flag is set to true.
	 *
	 * 如果 slotsync worker 仍在运行，就向它发信号。worker 发现 stopSignaled 标志为真后会停止。
	 */
	if (worker_pid != InvalidPid)
		kill(worker_pid, SIGUSR1);

	/* Wait for slot sync to end
	 *
	 * 等待槽同步结束。
	 */
	for (;;)
	{
		int			rc;

		/* Wait a bit, we don't expect to have to wait long
		 *
		 * 稍等一下，预期不会等太久。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   10L, WAIT_EVENT_REPLICATION_SLOTSYNC_SHUTDOWN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		SpinLockAcquire(&SlotSyncCtx->mutex);

		/* Ensure that no process is syncing the slots.
		 *
		 * 确保没有任何进程正在同步这些槽。
		 */
		if (!SlotSyncCtx->syncing)
			break;

		SpinLockRelease(&SlotSyncCtx->mutex);
	}

	SpinLockRelease(&SlotSyncCtx->mutex);

	update_synced_slots_inactive_since();
}

/*
 * SlotSyncWorkerCanRestart
 *
 * SlotSyncWorkerCanRestart：判断 slot sync worker 是否可以重启。
 *
 * Returns true if enough time (SLOTSYNC_RESTART_INTERVAL_SEC) has passed
 * since it was launched last. Otherwise returns false.
 *
 * 如果距离上次启动已经过了足够时间 SLOTSYNC_RESTART_INTERVAL_SEC，则返回真，否则返回假。
 *
 * This is a safety valve to protect against continuous respawn attempts if the
 * worker is dying immediately at launch. Note that since we will retry to
 * launch the worker from the postmaster main loop, we will get another
 * chance later.
 *
 * 这是一道安全阀，防止 worker 一启动就退出时被连续反复拉起。由于我们会在 postmaster 主循环里再次尝试启动 worker，稍后还有机会。
 */
bool
SlotSyncWorkerCanRestart(void)
{
	time_t		curtime = time(NULL);

	/* Return false if too soon since last start.
	 *
	 * 如果距离上次启动太近，则返回假。
	 */
	if ((unsigned int) (curtime - SlotSyncCtx->last_start_time) <
		(unsigned int) SLOTSYNC_RESTART_INTERVAL_SEC)
		return false;

	SlotSyncCtx->last_start_time = curtime;

	return true;
}

/*
 * Is current process syncing replication slots?
 *
 * 当前进程是否正在同步复制槽？
 *
 * Could be either backend executing SQL function or slot sync worker.
 *
 * 可能是正在执行 SQL 函数的后端，也可能是 slot sync worker。
 */
bool
IsSyncingReplicationSlots(void)
{
	return syncing_slots;
}

/*
 * Amount of shared memory required for slot synchronization.
 *
 * 槽同步所需的共享内存数量。
 */
Size
SlotSyncShmemSize(void)
{
	return sizeof(SlotSyncCtxStruct);
}

/*
 * Allocate and initialize the shared memory of slot synchronization.
 *
 * 分配并初始化槽同步的共享内存。
 */
void
SlotSyncShmemInit(void)
{
	Size		size = SlotSyncShmemSize();
	bool		found;

	SlotSyncCtx = (SlotSyncCtxStruct *)
		ShmemInitStruct("Slot Sync Data", size, &found);

	if (!found)
	{
		memset(SlotSyncCtx, 0, size);
		SlotSyncCtx->pid = InvalidPid;
		SpinLockInit(&SlotSyncCtx->mutex);
	}
}

/*
 * Error cleanup callback for slot sync SQL function.
 *
 * 槽同步 SQL 函数的错误清理回调。
 */
static void
slotsync_failure_callback(int code, Datum arg)
{
	WalReceiverConn *wrconn = (WalReceiverConn *) DatumGetPointer(arg);

	/*
	 * We need to do slots cleanup here just like WalSndErrorCleanup() does.
	 *
	 * 这里需要像 WalSndErrorCleanup() 那样清理槽。
	 *
	 * The startup process during promotion invokes ShutDownSlotSync() which
	 * waits for slot sync to finish and it does that by checking the
	 * 'syncing' flag. Thus the SQL function must be done with slots' release
	 * and cleanup to avoid any dangling temporary slots or active slots
	 * before it marks itself as finished syncing.
	 *
	 * 提升期间启动进程会调用 ShutDownSlotSync()，它通过检查 syncing 标志等待槽同步结束。因此 SQL 函数在把自己标为同步结束之前，必须完成槽的释放和清理，以免留下悬空的临时槽或仍被占用的槽。
	 */

	/* Make sure active replication slots are released
	 *
	 * 确保活动的复制槽已被释放。
	 */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* Also cleanup the synced temporary slots.
	 *
	 * 同时清理已同步的临时槽。
	 */
	ReplicationSlotCleanup(true);

	/*
	 * The set syncing_slots indicates that the process errored out without
	 * resetting the flag. So, we need to clean up shared memory and reset the
	 * flag here.
	 *
	 * syncing_slots 被设置，说明进程出错时没有复位该标志。因此要在这里清理共享内存并复位标志。
	 */
	if (syncing_slots)
		reset_syncing_flag();

	walrcv_disconnect(wrconn);
}

/*
 * Synchronize the failover enabled replication slots using the specified
 * primary server connection.
 *
 * 使用指定的主服务器连接，同步启用了故障转移的复制槽。
 */
void
SyncReplicationSlots(WalReceiverConn *wrconn)
{
	PG_ENSURE_ERROR_CLEANUP(slotsync_failure_callback, PointerGetDatum(wrconn));
	{
		check_and_set_sync_info(InvalidPid);

		validate_remote_info(wrconn);

		synchronize_slots(wrconn);

		/* Cleanup the synced temporary slots
		 *
		 * 清理已同步的临时槽。
		 */
		ReplicationSlotCleanup(true);

		/* We are done with sync, so reset sync flag
		 *
		 * 同步已经完成，因此复位同步标志。
		 */
		reset_syncing_flag();
	}
	PG_END_ENSURE_ERROR_CLEANUP(slotsync_failure_callback, PointerGetDatum(wrconn));
}
