/*-------------------------------------------------------------------------
 *
 * slot.c
 *	   Replication slot management.
 *
 * 复制槽管理。
 *
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/slot.c
 *
 * 标识
 *
 * NOTES
 *
 * 说明
 *
 * Replication slots are used to keep state about replication streams
 * originating from this cluster.  Their primary purpose is to prevent the
 * premature removal of WAL or of old tuple versions in a manner that would
 * interfere with replication; they are also useful for monitoring purposes.
 * Slots need to be permanent (to allow restarts), crash-safe, and allocatable
 * on standbys (to support cascading setups).  The requirement that slots be
 * usable on standbys precludes storing them in the system catalogs.
 *
 * 复制槽用来保存从本集群发出的复制流状态。其主要目的是避免过早删除
 * WAL 或旧元组版本，以免干扰复制；也可用于监控。槽必须持久，
 * 以便重启后仍在，并且崩溃安全，还能在备机上分配以支持级联。
 * 由于槽要能在备机上使用，因此不能存放在系统目录中。
 *
 * Each replication slot gets its own directory inside the directory
 * $PGDATA / PG_REPLSLOT_DIR.  Inside that directory the state file will
 * contain the slot's own data.  Additional data can be stored alongside that
 * file if required.  While the server is running, the state data is also
 * cached in memory for efficiency.
 *
 * 每个复制槽在 $PGDATA / PG_REPLSLOT_DIR 下各有一个目录。
 * 该目录中的状态文件保存槽自身的数据，必要时可在旁边存放额外数据。
 * 服务器运行期间，状态也会缓存在内存中以提高效率。
 *
 * ReplicationSlotAllocationLock must be taken in exclusive mode to allocate
 * or free a slot. ReplicationSlotControlLock must be taken in shared mode
 * to iterate over the slots, and in exclusive mode to change the in_use flag
 * of a slot.  The remaining data in each slot is protected by its mutex.
 *
 * 分配或释放槽时必须以排他模式持有 ReplicationSlotAllocationLock。
 * 遍历槽时以共享模式持有 ReplicationSlotControlLock，修改槽的 in_use
 * 标志时则以排他模式持有。槽内其余数据由其 mutex 保护。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "common/file_utils.h"
#include "common/string.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/slotsync.h"
#include "replication/slot.h"
#include "replication/walsender_private.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/varlena.h"

/*
 * Replication slot on-disk data structure.
 *
 * 复制槽的磁盘数据结构。
 */
typedef struct ReplicationSlotOnDisk
{
	/* first part of this struct needs to be version independent
	 *
	 * 该结构的前半部分必须与版本无关
	 */

	/* data not covered by checksum
	 *
	 * 不计入校验和的数据
	 */
	uint32		magic;
	pg_crc32c	checksum;

	/* data covered by checksum
	 *
	 * 计入校验和的数据
	 */
	uint32		version;
	uint32		length;

	/*
	 * The actual data in the slot that follows can differ based on the above
	 * 'version'.
	 *
	 * 其后的实际槽数据可随上面的 version 而不同。
	 */

	ReplicationSlotPersistentData slotdata;
} ReplicationSlotOnDisk;

/*
 * Struct for the configuration of synchronized_standby_slots.
 *
 * synchronized_standby_slots 的配置结构。
 *
 * Note: this must be a flat representation that can be held in a single chunk
 * of guc_malloc'd memory, so that it can be stored as the "extra" data for the
 * synchronized_standby_slots GUC.
 *
 * 注意：它必须是能放进一块 guc_malloc 内存的平坦表示，以便作为
 * synchronized_standby_slots 这个 GUC 的 extra 数据存放。
 */
typedef struct
{
	/* Number of slot names in the slot_names[]
	 *
	 * slot_names 数组中的槽名个数
	 */
	int			nslotnames;

	/*
	 * slot_names contains 'nslotnames' consecutive null-terminated C strings.
	 *
	 * slot_names 含有 nslotnames 个连续的、以 NUL 结尾的 C 字符串。
	 */
	char		slot_names[FLEXIBLE_ARRAY_MEMBER];
} SyncStandbySlotsConfigData;

/*
 * Lookup table for slot invalidation causes.
 *
 * 槽作废原因的查找表。
 */
typedef struct SlotInvalidationCauseMap
{
	ReplicationSlotInvalidationCause cause;
	const char *cause_name;
} SlotInvalidationCauseMap;

static const SlotInvalidationCauseMap SlotInvalidationCauses[] = {
	{RS_INVAL_NONE, "none"},
	{RS_INVAL_WAL_REMOVED, "wal_removed"},
	{RS_INVAL_HORIZON, "rows_removed"},
	{RS_INVAL_WAL_LEVEL, "wal_level_insufficient"},
	{RS_INVAL_IDLE_TIMEOUT, "idle_timeout"},
};

/*
 * Ensure that the lookup table is up-to-date with the enums defined in
 * ReplicationSlotInvalidationCause.
 *
 * 确保查找表与 ReplicationSlotInvalidationCause 中定义的枚举保持一致。
 */
StaticAssertDecl(lengthof(SlotInvalidationCauses) == (RS_INVAL_MAX_CAUSES + 1),
				 "array length mismatch");

/* size of version independent data
 *
 * 与版本无关部分的大小
 */
#define ReplicationSlotOnDiskConstantSize \
	offsetof(ReplicationSlotOnDisk, slotdata)
/* size of the part of the slot not covered by the checksum
 *
 * 槽中不计入校验和的部分的大小
 */
#define ReplicationSlotOnDiskNotChecksummedSize  \
	offsetof(ReplicationSlotOnDisk, version)
/* size of the part covered by the checksum
 *
 * 计入校验和的部分的大小
 */
#define ReplicationSlotOnDiskChecksummedSize \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskNotChecksummedSize
/* size of the slot data that is version dependent
 *
 * 随版本变化的槽数据大小
 */
#define ReplicationSlotOnDiskV2Size \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskConstantSize

#define SLOT_MAGIC		0x1051CA1	/* format identifier
 *
 * 格式标识
 */
#define SLOT_VERSION	5		/* version for new files
 *
 * 新文件使用的版本
 */

/* Control array for replication slot management
 *
 * 复制槽管理的控制数组
 */
ReplicationSlotCtlData *ReplicationSlotCtl = NULL;

/* My backend's replication slot in the shared memory array
 *
 * 本后端在共享内存数组中的复制槽
 */
ReplicationSlot *MyReplicationSlot = NULL;

/* GUC variables
 *
 * GUC 变量
 */
int			max_replication_slots = 10; /* the maximum number of replication
										 * slots
 *
 * 复制槽的最大数量
 */

/*
 * Invalidate replication slots that have remained idle longer than this
 * duration; '0' disables it.
 *
 * 空闲时间超过此时长的复制槽将被作废；0 表示关闭该功能。
 */
int			idle_replication_slot_timeout_secs = 0;

/*
 * This GUC lists streaming replication standby server slot names that
 * logical WAL sender processes will wait for.
 *
 * 该 GUC 列出逻辑 WAL sender 进程需要等待的流复制备库槽名。
 */
char	   *synchronized_standby_slots;

/* This is the parsed and cached configuration for synchronized_standby_slots
 *
 * synchronized_standby_slots 解析后的缓存配置
 */
static SyncStandbySlotsConfigData *synchronized_standby_slots_config;

/*
 * Oldest LSN that has been confirmed to be flushed to the standbys
 * corresponding to the physical slots specified in the synchronized_standby_slots GUC.
 *
 * 已确认刷到 synchronized_standby_slots GUC
 * 所指定物理槽对应备库的最老 LSN。
 */
static XLogRecPtr ss_oldest_flush_lsn = InvalidXLogRecPtr;

static void ReplicationSlotShmemExit(int code, Datum arg);
static void ReplicationSlotDropPtr(ReplicationSlot *slot);

/* internal persistency functions
 *
 * 内部持久化函数
 */
static void RestoreSlotFromDisk(const char *name);
static void CreateSlotOnDisk(ReplicationSlot *slot);
static void SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel);

/*
 * 核心流程：
 * 1) ReplicationSlotsShmemInit 建立共享内存槽数组；ReplicationSlotCreate
 *    在 ReplicationSlotAllocationLock 下分配空槽、落盘并标记 in_use。
 * 2) ReplicationSlotAcquire 按名占用槽，ReplicationSlotRelease 释放；
 *    进程退出时 ReplicationSlotCleanup 删除本会话的临时槽。
 * 3) ReplicationSlotSave 与检查点把槽状态刷到 PG_REPLSLOT_DIR；
 *    StartupReplicationSlots 在崩溃恢复前从磁盘装回。
 * 4) ReplicationSlotsComputeRequiredXmin 与 ReplicationSlotsComputeRequiredLSN
 *    汇总需保留的 xmin 与 WAL；InvalidateObsoleteReplicationSlots 在检查点中作废过期槽。
 */

/*
 * Report shared-memory space needed by ReplicationSlotsShmemInit.
 *
 * 报告 ReplicationSlotsShmemInit 所需的共享内存大小。
 */
Size
ReplicationSlotsShmemSize(void)
{
	Size		size = 0;

	if (max_replication_slots == 0)
		return size;

	size = offsetof(ReplicationSlotCtlData, replication_slots);
	size = add_size(size,
					mul_size(max_replication_slots, sizeof(ReplicationSlot)));

	return size;
}

/*
 * Allocate and initialize shared memory for replication slots.
 *
 * 为复制槽分配并初始化共享内存。
 */
void
ReplicationSlotsShmemInit(void)
{
	bool		found;

	if (max_replication_slots == 0)
		return;

	ReplicationSlotCtl = (ReplicationSlotCtlData *)
		ShmemInitStruct("ReplicationSlot Ctl", ReplicationSlotsShmemSize(),
						&found);

	if (!found)
	{
		int			i;

		/* First time through, so initialize
		 *
		 * 首次经过，因此进行初始化
		 */
		MemSet(ReplicationSlotCtl, 0, ReplicationSlotsShmemSize());

		for (i = 0; i < max_replication_slots; i++)
		{
			ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];

			/* everything else is zeroed by the memset above
			 *
			 * 其余字段已由上面的 memset 清零
			 */
			SpinLockInit(&slot->mutex);
			LWLockInitialize(&slot->io_in_progress_lock,
							 LWTRANCHE_REPLICATION_SLOT_IO);
			ConditionVariableInit(&slot->active_cv);
		}
	}
}

/*
 * Register the callback for replication slot cleanup and releasing.
 *
 * 注册复制槽清理与释放的回调。
 */
void
ReplicationSlotInitialize(void)
{
	before_shmem_exit(ReplicationSlotShmemExit, 0);
}

/*
 * Release and cleanup replication slots.
 *
 * 释放并清理复制槽。
 */
static void
ReplicationSlotShmemExit(int code, Datum arg)
{
	/* Make sure active replication slots are released
	 *
	 * 确保活动复制槽已被释放
	 */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* Also cleanup all the temporary slots.
	 *
	 * 同时清理全部临时槽。
	 */
	ReplicationSlotCleanup(false);
}

/*
 * Check whether the passed slot name is valid and report errors at elevel.
 *
 * 检查传入的槽名是否合法，并按 elevel 报告错误。
 *
 * See comments for ReplicationSlotValidateNameInternal().
 *
 * 参见 ReplicationSlotValidateNameInternal 的注释。
 */
bool
ReplicationSlotValidateName(const char *name, int elevel)
{
	int			err_code;
	char	   *err_msg = NULL;
	char	   *err_hint = NULL;

	if (!ReplicationSlotValidateNameInternal(name, &err_code, &err_msg,
											 &err_hint))
	{
		/*
		 * Use errmsg_internal() and errhint_internal() instead of errmsg()
		 * and errhint(), since the messages from
		 * ReplicationSlotValidateNameInternal() are already translated. This
		 * avoids double translation.
		 *
		 * 使用 errmsg_internal 与 errhint_internal，而不是 errmsg 与 errhint，
		 * 因为 ReplicationSlotValidateNameInternal 给出的消息已经翻译过。
		 * 这样可避免二次翻译。
		 */
		ereport(elevel,
				errcode(err_code),
				errmsg_internal("%s", err_msg),
				(err_hint != NULL) ? errhint_internal("%s", err_hint) : 0);

		pfree(err_msg);
		if (err_hint != NULL)
			pfree(err_hint);
		return false;
	}

	return true;
}

/*
 * Check whether the passed slot name is valid.
 *
 * 检查传入的槽名是否合法。
 *
 * Slot names may consist out of [a-z0-9_]{1,NAMEDATALEN-1} which should allow
 * the name to be used as a directory name on every supported OS.
 *
 * 槽名只能由 [a-z0-9_]{1,NAMEDATALEN-1} 组成，
 * 以便在所有受支持的操作系统上都能用作目录名。
 *
 * Returns true if the slot name is valid. Otherwise, returns false and stores
 * the error code, error message, and optional hint in err_code, err_msg, and
 * err_hint, respectively. The caller is responsible for freeing err_msg and
 * err_hint, which are palloc'd.
 *
 * 槽名合法则返回 true。否则返回 false，并把错误码、
 * 错误信息与可选提示分别写入 err_code、err_msg 与 err_hint。
 * 调用方负责释放由 palloc 得到的 err_msg 与 err_hint。
 */
bool
ReplicationSlotValidateNameInternal(const char *name, int *err_code,
									char **err_msg, char **err_hint)
{
	const char *cp;

	if (strlen(name) == 0)
	{
		*err_code = ERRCODE_INVALID_NAME;
		*err_msg = psprintf(_("replication slot name \"%s\" is too short"), name);
		*err_hint = NULL;
		return false;
	}

	if (strlen(name) >= NAMEDATALEN)
	{
		*err_code = ERRCODE_NAME_TOO_LONG;
		*err_msg = psprintf(_("replication slot name \"%s\" is too long"), name);
		*err_hint = NULL;
		return false;
	}

	for (cp = name; *cp; cp++)
	{
		if (!((*cp >= 'a' && *cp <= 'z')
			  || (*cp >= '0' && *cp <= '9')
			  || (*cp == '_')))
		{
			*err_code = ERRCODE_INVALID_NAME;
			*err_msg = psprintf(_("replication slot name \"%s\" contains invalid character"), name);
			*err_hint = psprintf(_("Replication slot names may only contain lower case letters, numbers, and the underscore character."));
			return false;
		}
	}
	return true;
}

/*
 * Create a new replication slot and mark it as used by this backend.
 *
 * 创建新的复制槽，并标记为本后端正在使用。
 *
 * name: Name of the slot
 * db_specific: logical decoding is db specific; if the slot is going to
 *	   be used for that pass true, otherwise false.
 * two_phase: If enabled, allows decoding of prepared transactions.
 * failover: If enabled, allows the slot to be synced to standbys so
 *     that logical replication can be resumed after failover.
 * synced: True if the slot is synchronized from the primary server.
 *
 * name：槽名。db_specific：逻辑解码按数据库区分；
 * 若槽将用于逻辑解码则传 true，否则传 false。two_phase：
 * 启用后允许解码预备事务。failover：启用后允许把槽同步到备库，
 * 以便故障转移后继续逻辑复制。synced：若该槽是从主库同步而来则为
 * true。
 */
void
ReplicationSlotCreate(const char *name, bool db_specific,
					  ReplicationSlotPersistency persistency,
					  bool two_phase, bool failover, bool synced)
{
	ReplicationSlot *slot = NULL;
	int			i;

	Assert(MyReplicationSlot == NULL);

	ReplicationSlotValidateName(name, ERROR);

	if (failover)
	{
		/*
		 * Do not allow users to create the failover enabled slots on the
		 * standby as we do not support sync to the cascading standby.
		 *
		 * 不允许用户在备库上创建启用了 failover 的槽，
		 * 因为不支持同步到级联备库。
		 *
		 * However, failover enabled slots can be created during slot
		 * synchronization because we need to retain the same values as the
		 * remote slot.
		 *
		 * 不过槽同步过程中可以创建启用 failover 的槽，
		 * 因为需要保持与远端槽相同的取值。
		 */
		if (RecoveryInProgress() && !IsSyncingReplicationSlots())
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a replication slot created on the standby"));

		/*
		 * Do not allow users to create failover enabled temporary slots,
		 * because temporary slots will not be synced to the standby.
		 *
		 * 不允许用户创建启用 failover 的临时槽，因为临时槽不会同步到备库。
		 *
		 * However, failover enabled temporary slots can be created during
		 * slot synchronization. See the comments atop slotsync.c for details.
		 *
		 * 不过槽同步过程中可以创建启用 failover 的临时槽。详见 slotsync.c
		 * 顶部的注释。
		 */
		if (persistency == RS_TEMPORARY && !IsSyncingReplicationSlots())
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a temporary replication slot"));
	}

	/*
	 * If some other backend ran this code concurrently with us, we'd likely
	 * both allocate the same slot, and that would be bad.  We'd also be at
	 * risk of missing a name collision.  Also, we don't want to try to create
	 * a new slot while somebody's busy cleaning up an old one, because we
	 * might both be monkeying with the same directory.
	 *
	 * 若另一个后端与我们并发执行这段代码，双方很可能分配到同一个槽，
	 * 这是不允许的，也可能漏掉重名。也不要在别人清理旧槽时创建新槽，
	 * 否则可能同时改动同一个目录。
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/*
	 * Check for name collision, and identify an allocatable slot.  We need to
	 * hold ReplicationSlotControlLock in shared mode for this, so that nobody
	 * else can change the in_use flags while we're looking at them.
	 *
	 * 检查重名，并找出一个可分配的槽。为此必须以共享模式持有
	 * ReplicationSlotControlLock，以免查看期间别人修改 in_use 标志。
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("replication slot \"%s\" already exists", name)));
		if (!s->in_use && slot == NULL)
			slot = s;
	}
	LWLockRelease(ReplicationSlotControlLock);

	/* If all slots are in use, we're out of luck.
	 *
	 * 所有槽都已被占用，无法再分配。
	 */
	if (slot == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("all replication slots are in use"),
				 errhint("Free one or increase \"max_replication_slots\".")));

	/*
	 * Since this slot is not in use, nobody should be looking at any part of
	 * it other than the in_use field unless they're trying to allocate it.
	 * And since we hold ReplicationSlotAllocationLock, nobody except us can
	 * be doing that.  So it's safe to initialize the slot.
	 *
	 * 该槽尚未使用，除了正在分配它的人，别人不应查看 in_use 以外的字段。
	 * 我们持有 ReplicationSlotAllocationLock，因此只有我们在分配。
	 * 此时初始化该槽是安全的。
	 */
	Assert(!slot->in_use);
	Assert(slot->active_pid == 0);

	/* first initialize persistent data
	 *
	 * 先初始化持久化数据
	 */
	memset(&slot->data, 0, sizeof(ReplicationSlotPersistentData));
	namestrcpy(&slot->data.name, name);
	slot->data.database = db_specific ? MyDatabaseId : InvalidOid;
	slot->data.persistency = persistency;
	slot->data.two_phase = two_phase;
	slot->data.two_phase_at = InvalidXLogRecPtr;
	slot->data.failover = failover;
	slot->data.synced = synced;

	/* and then data only present in shared memory
	 *
	 * 然后初始化仅存在于共享内存中的数据
	 */
	slot->just_dirtied = false;
	slot->dirty = false;
	slot->effective_xmin = InvalidTransactionId;
	slot->effective_catalog_xmin = InvalidTransactionId;
	slot->candidate_catalog_xmin = InvalidTransactionId;
	slot->candidate_xmin_lsn = InvalidXLogRecPtr;
	slot->candidate_restart_valid = InvalidXLogRecPtr;
	slot->candidate_restart_lsn = InvalidXLogRecPtr;
	slot->last_saved_confirmed_flush = InvalidXLogRecPtr;
	slot->last_saved_restart_lsn = InvalidXLogRecPtr;
	slot->inactive_since = 0;

	/*
	 * Create the slot on disk.  We haven't actually marked the slot allocated
	 * yet, so no special cleanup is required if this errors out.
	 *
	 * 在磁盘上创建该槽。此时尚未把槽标记为已分配，
	 * 若此处出错则无需特殊清理。
	 */
	CreateSlotOnDisk(slot);

	/*
	 * We need to briefly prevent any other backend from iterating over the
	 * slots while we flip the in_use flag. We also need to set the active
	 * flag while holding the ControlLock as otherwise a concurrent
	 * ReplicationSlotAcquire() could acquire the slot as well.
	 *
	 * 翻转 in_use 标志时，需要短暂阻止其他后端遍历槽。还必须在持有
	 * ControlLock 时设置活动标志，否则并发的 ReplicationSlotAcquire
	 * 也可能占用该槽。
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);

	slot->in_use = true;

	/* We can now mark the slot active, and that makes it our slot.
	 *
	 * 现在可以把槽标为活动，它就成为本后端的槽。
	 */
	SpinLockAcquire(&slot->mutex);
	Assert(slot->active_pid == 0);
	slot->active_pid = MyProcPid;
	SpinLockRelease(&slot->mutex);
	MyReplicationSlot = slot;

	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * Create statistics entry for the new logical slot. We don't collect any
	 * stats for physical slots, so no need to create an entry for the same.
	 * See ReplicationSlotDropPtr for why we need to do this before releasing
	 * ReplicationSlotAllocationLock.
	 *
	 * 为新的逻辑槽创建统计项。物理槽不收集统计，因此不必创建。必须在释放
	 * ReplicationSlotAllocationLock 之前完成，原因见
	 * ReplicationSlotDropPtr。
	 */
	if (SlotIsLogical(slot))
		pgstat_create_replslot(slot);

	/*
	 * Now that the slot has been marked as in_use and active, it's safe to
	 * let somebody else try to allocate a slot.
	 *
	 * 槽已标记为 in_use 且活动，现在可以让别人尝试分配槽。
	 */
	LWLockRelease(ReplicationSlotAllocationLock);

	/* Let everybody know we've modified this slot
	 *
	 * 通知所有人该槽已被修改
	 */
	ConditionVariableBroadcast(&slot->active_cv);
}

/*
 * Search for the named replication slot.
 *
 * 按名称查找复制槽。
 *
 * Return the replication slot if found, otherwise NULL.
 *
 * 找到则返回该复制槽，否则返回 NULL。
 */
ReplicationSlot *
SearchNamedReplicationSlot(const char *name, bool need_lock)
{
	int			i;
	ReplicationSlot *slot = NULL;

	if (need_lock)
		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
		{
			slot = s;
			break;
		}
	}

	if (need_lock)
		LWLockRelease(ReplicationSlotControlLock);

	return slot;
}

/*
 * Return the index of the replication slot in
 * ReplicationSlotCtl->replication_slots.
 *
 * 返回该复制槽在 ReplicationSlotCtl 的 replication_slots
 * 数组中的下标。
 *
 * This is mainly useful to have an efficient key for storing replication slot
 * stats.
 *
 * 这主要用来作为保存复制槽统计信息的高效键。
 */
int
ReplicationSlotIndex(ReplicationSlot *slot)
{
	Assert(slot >= ReplicationSlotCtl->replication_slots &&
		   slot < ReplicationSlotCtl->replication_slots + max_replication_slots);

	return slot - ReplicationSlotCtl->replication_slots;
}

/*
 * If the slot at 'index' is unused, return false. Otherwise 'name' is set to
 * the slot's name and true is returned.
 *
 * 若 index 处的槽未使用则返回 false。否则把 name 设为槽名并返回 true。
 *
 * This likely is only useful for pgstat_replslot.c during shutdown, in other
 * cases there are obvious TOCTOU issues.
 *
 * 这大概只在关闭过程中对 pgstat_replslot.c 有用，其他场合存在明显的
 * TOCTOU 问题。
 */
bool
ReplicationSlotName(int index, Name name)
{
	ReplicationSlot *slot;
	bool		found;

	slot = &ReplicationSlotCtl->replication_slots[index];

	/*
	 * Ensure that the slot cannot be dropped while we copy the name. Don't
	 * need the spinlock as the name of an existing slot cannot change.
	 *
	 * 复制名称期间确保槽不会被删除。已有槽的名称不会改变，
	 * 因此不需要自旋锁。
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	found = slot->in_use;
	if (slot->in_use)
		namestrcpy(name, NameStr(slot->data.name));
	LWLockRelease(ReplicationSlotControlLock);

	return found;
}

/*
 * Find a previously created slot and mark it as used by this process.
 *
 * 找到先前创建的槽，并标记为本进程正在使用。
 *
 * An error is raised if nowait is true and the slot is currently in use. If
 * nowait is false, we sleep until the slot is released by the owning process.
 *
 * 若 nowait 为 true 且槽当前被占用，则报错。若 nowait 为 false，
 * 则一直睡眠到拥有者进程释放该槽。
 *
 * An error is raised if error_if_invalid is true and the slot is found to
 * be invalid. It should always be set to true, except when we are temporarily
 * acquiring the slot and don't intend to change it.
 *
 * 若 error_if_invalid 为 true 且发现槽已作废，则报错。
 * 除了暂时占用且不打算修改槽的情况，该参数应始终为 true。
 */
void
ReplicationSlotAcquire(const char *name, bool nowait, bool error_if_invalid)
{
	ReplicationSlot *s;
	int			active_pid;

	Assert(name != NULL);

retry:
	Assert(MyReplicationSlot == NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/* Check if the slot exits with the given name.
	 *
	 * 检查是否存在给定名称的槽。
	 */
	s = SearchNamedReplicationSlot(name, false);
	if (s == NULL || !s->in_use)
	{
		LWLockRelease(ReplicationSlotControlLock);

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist",
						name)));
	}

	/*
	 * This is the slot we want; check if it's active under some other
	 * process.  In single user mode, we don't need this check.
	 *
	 * 这就是要找的槽；检查它是否被其他进程占用。
	 * 单用户模式下不需要这项检查。
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Get ready to sleep on the slot in case it is active.  (We may end
		 * up not sleeping, but we don't want to do this while holding the
		 * spinlock.)
		 *
		 * 若槽处于活动状态，准备在其上睡眠。最终可能并不睡眠，
		 * 但不想在持有自旋锁时做这件事。
		 */
		if (!nowait)
			ConditionVariablePrepareToSleep(&s->active_cv);

		/*
		 * It is important to reset the inactive_since under spinlock here to
		 * avoid race conditions with slot invalidation. See comments related
		 * to inactive_since in InvalidatePossiblyObsoleteSlot.
		 *
		 * 必须在自旋锁内把 inactive_since 清零，以免与槽作废产生竞争。参见
		 * InvalidatePossiblyObsoleteSlot 中与 inactive_since 相关的注释。
		 */
		SpinLockAcquire(&s->mutex);
		if (s->active_pid == 0)
			s->active_pid = MyProcPid;
		active_pid = s->active_pid;
		ReplicationSlotSetInactiveSince(s, 0, false);
		SpinLockRelease(&s->mutex);
	}
	else
	{
		s->active_pid = active_pid = MyProcPid;
		ReplicationSlotSetInactiveSince(s, 0, true);
	}
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * If we found the slot but it's already active in another process, we
	 * wait until the owning process signals us that it's been released, or
	 * error out.
	 *
	 * 若找到的槽已由其他进程占用，则等待拥有者通知已释放，或者直接报错。
	 */
	if (active_pid != MyProcPid)
	{
		if (!nowait)
		{
			/* Wait here until we get signaled, and then restart
			 *
			 * 在此等待信号，然后重新开始
			 */
			ConditionVariableSleep(&s->active_cv,
								   WAIT_EVENT_REPLICATION_SLOT_DROP);
			ConditionVariableCancelSleep();
			goto retry;
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("replication slot \"%s\" is active for PID %d",
						NameStr(s->data.name), active_pid)));
	}
	else if (!nowait)
		ConditionVariableCancelSleep(); /* no sleep needed after all
		 *
		 * 最终并不需要睡眠
		 */

	/* We made this slot active, so it's ours now.
	 *
	 * 本进程已使该槽活动，因此它现在属于我们。
	 */
	MyReplicationSlot = s;

	/*
	 * We need to check for invalidation after making the slot ours to avoid
	 * the possible race condition with the checkpointer that can otherwise
	 * invalidate the slot immediately after the check.
	 *
	 * 必须在槽归属本进程之后再检查是否作废，以免与 checkpointer 竞争：
	 * 否则检查刚结束槽就可能被作废。
	 */
	if (error_if_invalid && s->data.invalidated != RS_INVAL_NONE)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("can no longer access replication slot \"%s\"",
					   NameStr(s->data.name)),
				errdetail("This replication slot has been invalidated due to \"%s\".",
						  GetSlotInvalidationCauseName(s->data.invalidated)));

	/* Let everybody know we've modified this slot
	 *
	 * 通知所有人该槽已被修改
	 */
	ConditionVariableBroadcast(&s->active_cv);

	/*
	 * The call to pgstat_acquire_replslot() protects against stats for a
	 * different slot, from before a restart or such, being present during
	 * pgstat_report_replslot().
	 *
	 * 调用 pgstat_acquire_replslot 是为了避免 pgstat_report_replslot
	 * 期间仍残留重启前或其他槽的统计。
	 */
	if (SlotIsLogical(s))
		pgstat_acquire_replslot(s);


	if (am_walsender)
	{
		ereport(log_replication_commands ? LOG : DEBUG1,
				SlotIsLogical(s)
				? errmsg("acquired logical replication slot \"%s\"",
						 NameStr(s->data.name))
				: errmsg("acquired physical replication slot \"%s\"",
						 NameStr(s->data.name)));
	}
}

/*
 * Release the replication slot that this backend considers to own.
 *
 * 释放本后端认为自己拥有的复制槽。
 *
 * This or another backend can re-acquire the slot later.
 * Resources this slot requires will be preserved.
 *
 * 本后端或其他后端以后可以再次占用该槽。该槽所需的资源会被保留。
 */
void
ReplicationSlotRelease(void)
{
	ReplicationSlot *slot = MyReplicationSlot;
	char	   *slotname = NULL;	/* keep compiler quiet
	 *
	 * 避免编译器告警
	 */
	bool		is_logical = false; /* keep compiler quiet
	 *
	 * 避免编译器告警
	 */
	TimestampTz now = 0;

	Assert(slot != NULL && slot->active_pid != 0);

	if (am_walsender)
	{
		slotname = pstrdup(NameStr(slot->data.name));
		is_logical = SlotIsLogical(slot);
	}

	if (slot->data.persistency == RS_EPHEMERAL)
	{
		/*
		 * Delete the slot. There is no !PANIC case where this is allowed to
		 * fail, all that may happen is an incomplete cleanup of the on-disk
		 * data.
		 *
		 * 删除该槽。除 PANIC 以外不允许此处失败，
		 * 最多只会发生磁盘数据清理不完整。
		 */
		ReplicationSlotDropAcquired();
	}

	/*
	 * If slot needed to temporarily restrain both data and catalog xmin to
	 * create the catalog snapshot, remove that temporary constraint.
	 * Snapshots can only be exported while the initial snapshot is still
	 * acquired.
	 *
	 * 若该槽为了建立目录快照而临时收紧了数据 xmin 与目录 xmin，
	 * 则去掉这一临时约束。只有在仍持有初始快照时才能导出快照。
	 */
	if (!TransactionIdIsValid(slot->data.xmin) &&
		TransactionIdIsValid(slot->effective_xmin))
	{
		SpinLockAcquire(&slot->mutex);
		slot->effective_xmin = InvalidTransactionId;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotsComputeRequiredXmin(false);
	}

	/*
	 * Set the time since the slot has become inactive. We get the current
	 * time beforehand to avoid system call while holding the spinlock.
	 *
	 * 记录槽变为非活动后的时刻。先取当前时间，以免持有自旋锁时做系统调用。
	 */
	now = GetCurrentTimestamp();

	if (slot->data.persistency == RS_PERSISTENT)
	{
		/*
		 * Mark persistent slot inactive.  We're not freeing it, just
		 * disconnecting, but wake up others that may be waiting for it.
		 *
		 * 把持久槽标为非活动。这里并不释放槽，只是断开使用，
		 * 并唤醒可能正在等待它的进程。
		 */
		SpinLockAcquire(&slot->mutex);
		slot->active_pid = 0;
		ReplicationSlotSetInactiveSince(slot, now, false);
		SpinLockRelease(&slot->mutex);
		ConditionVariableBroadcast(&slot->active_cv);
	}
	else
		ReplicationSlotSetInactiveSince(slot, now, true);

	MyReplicationSlot = NULL;

	/* might not have been set when we've been a plain slot
	 *
	 * 作为普通槽时可能尚未设置
	 */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags &= ~PROC_IN_LOGICAL_DECODING;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);

	if (am_walsender)
	{
		ereport(log_replication_commands ? LOG : DEBUG1,
				is_logical
				? errmsg("released logical replication slot \"%s\"",
						 slotname)
				: errmsg("released physical replication slot \"%s\"",
						 slotname));

		pfree(slotname);
	}
}

/*
 * Cleanup temporary slots created in current session.
 *
 * 清理当前会话创建的临时槽。
 *
 * Cleanup only synced temporary slots if 'synced_only' is true, else
 * cleanup all temporary slots.
 *
 * 若 synced_only 为 true，则只清理已同步的临时槽，否则清理全部临时槽。
 */
void
ReplicationSlotCleanup(bool synced_only)
{
	int			i;

	Assert(MyReplicationSlot == NULL);

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		if ((s->active_pid == MyProcPid &&
			 (!synced_only || s->data.synced)))
		{
			Assert(s->data.persistency == RS_TEMPORARY);
			SpinLockRelease(&s->mutex);
			LWLockRelease(ReplicationSlotControlLock);	/* avoid deadlock
			 *
			 * 避免死锁
			 */

			ReplicationSlotDropPtr(s);

			ConditionVariableBroadcast(&s->active_cv);
			goto restart;
		}
		else
			SpinLockRelease(&s->mutex);
	}

	LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Permanently drop replication slot identified by the passed in name.
 *
 * 按传入的名称永久删除复制槽。
 */
void
ReplicationSlotDrop(const char *name, bool nowait)
{
	Assert(MyReplicationSlot == NULL);

	ReplicationSlotAcquire(name, nowait, false);

	/*
	 * Do not allow users to drop the slots which are currently being synced
	 * from the primary to the standby.
	 *
	 * 不允许用户删除正在从主库同步到备库的槽。
	 */
	if (RecoveryInProgress() && MyReplicationSlot->data.synced)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot drop replication slot \"%s\"", name),
				errdetail("This replication slot is being synchronized from the primary server."));

	ReplicationSlotDropAcquired();
}

/*
 * Change the definition of the slot identified by the specified name.
 *
 * 修改指定名称的槽的定义。
 *
 * Altering the two_phase property of a slot requires caution on the
 * client-side. Enabling it at any random point during decoding has the
 * risk that transactions prepared before this change may be skipped by
 * the decoder, leading to missing prepare records on the client. So, we
 * enable it for subscription related slots only once the initial tablesync
 * is finished. See comments atop worker.c. Disabling it is safe only when
 * there are no pending prepared transaction, otherwise, the changes of
 * already prepared transactions can be replicated again along with their
 * corresponding commit leading to duplicate data or errors.
 *
 * 修改槽的 two_phase 属性时客户端必须谨慎。
 * 在解码过程中任意时刻启用它，
 * 可能导致此变更之前已预备的事务被解码器跳过，客户端因而缺少 prepare
 * 记录。因此与订阅相关的槽只在初始 tablesync 完成后才启用。参见
 * worker.c 顶部的注释。只有在没有未完成的预备事务时，禁用才是安全的；
 * 否则已预备事务的变更可能连同其提交再次被复制，造成重复数据或错误。
 */
void
ReplicationSlotAlter(const char *name, const bool *failover,
					 const bool *two_phase)
{
	bool		update_slot = false;

	Assert(MyReplicationSlot == NULL);
	Assert(failover || two_phase);

	ReplicationSlotAcquire(name, false, true);

	if (SlotIsPhysical(MyReplicationSlot))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot use %s with a physical replication slot",
					   "ALTER_REPLICATION_SLOT"));

	if (RecoveryInProgress())
	{
		/*
		 * Do not allow users to alter the slots which are currently being
		 * synced from the primary to the standby.
		 *
		 * 不允许用户修改正在从主库同步到备库的槽。
		 */
		if (MyReplicationSlot->data.synced)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot alter replication slot \"%s\"", name),
					errdetail("This replication slot is being synchronized from the primary server."));

		/*
		 * Do not allow users to enable failover on the standby as we do not
		 * support sync to the cascading standby.
		 *
		 * 不允许用户在备库上启用 failover，因为不支持同步到级联备库。
		 */
		if (failover && *failover)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a replication slot"
						   " on the standby"));
	}

	if (failover)
	{
		/*
		 * Do not allow users to enable failover for temporary slots as we do
		 * not support syncing temporary slots to the standby.
		 *
		 * 不允许用户为临时槽启用 failover，因为不支持把临时槽同步到备库。
		 */
		if (*failover && MyReplicationSlot->data.persistency == RS_TEMPORARY)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot enable failover for a temporary replication slot"));

		if (MyReplicationSlot->data.failover != *failover)
		{
			SpinLockAcquire(&MyReplicationSlot->mutex);
			MyReplicationSlot->data.failover = *failover;
			SpinLockRelease(&MyReplicationSlot->mutex);

			update_slot = true;
		}
	}

	if (two_phase && MyReplicationSlot->data.two_phase != *two_phase)
	{
		SpinLockAcquire(&MyReplicationSlot->mutex);
		MyReplicationSlot->data.two_phase = *two_phase;
		SpinLockRelease(&MyReplicationSlot->mutex);

		update_slot = true;
	}

	if (update_slot)
	{
		ReplicationSlotMarkDirty();
		ReplicationSlotSave();
	}

	ReplicationSlotRelease();
}

/*
 * Permanently drop the currently acquired replication slot.
 *
 * 永久删除当前已占用的复制槽。
 */
void
ReplicationSlotDropAcquired(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(MyReplicationSlot != NULL);

	/* slot isn't acquired anymore
	 *
	 * 槽已不再被占用
	 */
	MyReplicationSlot = NULL;

	ReplicationSlotDropPtr(slot);
}

/*
 * Permanently drop the replication slot which will be released by the point
 * this function returns.
 *
 * 永久删除复制槽；本函数返回时该槽将被释放。
 */
static void
ReplicationSlotDropPtr(ReplicationSlot *slot)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];

	/*
	 * If some other backend ran this code concurrently with us, we might try
	 * to delete a slot with a certain name while someone else was trying to
	 * create a slot with the same name.
	 *
	 * 若另一个后端与我们并发执行这段代码，
	 * 我们可能在别人用同一名称创建槽时删除该名称的槽。
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/* Generate pathnames.
	 *
	 * 生成路径名。
	 */
	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(slot->data.name));
	sprintf(tmppath, "%s/%s.tmp", PG_REPLSLOT_DIR, NameStr(slot->data.name));

	/*
	 * Rename the slot directory on disk, so that we'll no longer recognize
	 * this as a valid slot.  Note that if this fails, we've got to mark the
	 * slot inactive before bailing out.  If we're dropping an ephemeral or a
	 * temporary slot, we better never fail hard as the caller won't expect
	 * the slot to survive and this might get called during error handling.
	 *
	 * 在磁盘上重命名槽目录，使它不再被认作有效槽。若失败，
	 * 退出前必须把槽标为非活动。删除临时槽或短暂槽时绝不能硬失败，
	 * 因为调用方不期望槽仍然存在，而且此处可能在错误处理过程中被调用。
	 */
	if (rename(path, tmppath) == 0)
	{
		/*
		 * We need to fsync() the directory we just renamed and its parent to
		 * make sure that our changes are on disk in a crash-safe fashion.  If
		 * fsync() fails, we can't be sure whether the changes are on disk or
		 * not.  For now, we handle that by panicking;
		 * StartupReplicationSlots() will try to straighten it out after
		 * restart.
		 *
		 * 需要对刚重命名的目录及其父目录做 fsync，以便崩溃后更改仍在磁盘上。
		 * 若 fsync 失败，无法确定更改是否已落盘。目前的处理是 panic；重启后
		 * StartupReplicationSlots 会尝试纠正。
		 */
		START_CRIT_SECTION();
		fsync_fname(tmppath, true);
		fsync_fname(PG_REPLSLOT_DIR, true);
		END_CRIT_SECTION();
	}
	else
	{
		bool		fail_softly = slot->data.persistency != RS_PERSISTENT;

		SpinLockAcquire(&slot->mutex);
		slot->active_pid = 0;
		SpinLockRelease(&slot->mutex);

		/* wake up anyone waiting on this slot
		 *
		 * 唤醒正在等待该槽的进程
		 */
		ConditionVariableBroadcast(&slot->active_cv);

		ereport(fail_softly ? WARNING : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						path, tmppath)));
	}

	/*
	 * The slot is definitely gone.  Lock out concurrent scans of the array
	 * long enough to kill it.  It's OK to clear the active PID here without
	 * grabbing the mutex because nobody else can be scanning the array here,
	 * and nobody can be attached to this slot and thus access it without
	 * scanning the array.
	 *
	 * 槽确实已经消失。短暂禁止并发扫描数组，以便把它清掉。此处可以不持有
	 * mutex 就清除活动 PID，因为此刻没有别人能扫描数组，
	 * 也没有进程附着在该槽上，而附着必须先扫描数组。
	 *
	 * Also wake up processes waiting for it.
	 *
	 * 同时唤醒正在等待它的进程。
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
	slot->active_pid = 0;
	slot->in_use = false;
	LWLockRelease(ReplicationSlotControlLock);
	ConditionVariableBroadcast(&slot->active_cv);

	/*
	 * Slot is dead and doesn't prevent resource removal anymore, recompute
	 * limits.
	 *
	 * 槽已失效，不再阻止资源回收，重新计算限制。
	 */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();

	/*
	 * If removing the directory fails, the worst thing that will happen is
	 * that the user won't be able to create a new slot with the same name
	 * until the next server restart.  We warn about it, but that's all.
	 *
	 * 若删除目录失败，
	 * 最坏情况是用户在下次服务器重启前无法用同一名称创建新槽。
	 * 对此发出警告即可。
	 */
	if (!rmtree(tmppath, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", tmppath)));

	/*
	 * Drop the statistics entry for the replication slot.  Do this while
	 * holding ReplicationSlotAllocationLock so that we don't drop a
	 * statistics entry for another slot with the same name just created in
	 * another session.
	 *
	 * 删除该复制槽的统计项。删除时持有 ReplicationSlotAllocationLock，
	 * 以免删掉另一个会话刚用同一名称创建的槽的统计项。
	 */
	if (SlotIsLogical(slot))
		pgstat_drop_replslot(slot);

	/*
	 * We release this at the very end, so that nobody starts trying to create
	 * a slot while we're still cleaning up the detritus of the old one.
	 *
	 * 直到最后才释放这把锁，以免我们还在清理旧槽残留时就有人开始创建槽。
	 */
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Serialize the currently acquired slot's state from memory to disk, thereby
 * guaranteeing the current state will survive a crash.
 *
 * 把当前占用槽的内存状态序列化到磁盘，从而保证当前状态能在崩溃后保留。
 */
void
ReplicationSlotSave(void)
{
	char		path[MAXPGPATH];

	Assert(MyReplicationSlot != NULL);

	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(MyReplicationSlot->data.name));
	SaveSlotToPath(MyReplicationSlot, path, ERROR);
}

/*
 * Signal that it would be useful if the currently acquired slot would be
 * flushed out to disk.
 *
 * 提示当前占用的槽值得刷到磁盘。
 *
 * Note that the actual flush to disk can be delayed for a long time, if
 * required for correctness explicitly do a ReplicationSlotSave().
 *
 * 注意：真正刷盘可能推迟很久；若正确性要求立即落盘，应显式调用
 * ReplicationSlotSave。
 */
void
ReplicationSlotMarkDirty(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(MyReplicationSlot != NULL);

	SpinLockAcquire(&slot->mutex);
	MyReplicationSlot->just_dirtied = true;
	MyReplicationSlot->dirty = true;
	SpinLockRelease(&slot->mutex);
}

/*
 * Convert a slot that's marked as RS_EPHEMERAL or RS_TEMPORARY to a
 * RS_PERSISTENT slot, guaranteeing it will be there after an eventual crash.
 *
 * 把标记为 RS_EPHEMERAL 或 RS_TEMPORARY 的槽转为 RS_PERSISTENT，
 * 保证最终崩溃后它仍然存在。
 */
void
ReplicationSlotPersist(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(slot->data.persistency != RS_PERSISTENT);

	SpinLockAcquire(&slot->mutex);
	slot->data.persistency = RS_PERSISTENT;
	SpinLockRelease(&slot->mutex);

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();
}

/*
 * Compute the oldest xmin across all slots and store it in the ProcArray.
 *
 * 计算所有槽中最老的 xmin，并存入 ProcArray。
 *
 * If already_locked is true, both the ReplicationSlotControlLock and the
 * ProcArrayLock have already been acquired exclusively. It is crucial that the
 * caller first acquires the ReplicationSlotControlLock, followed by the
 * ProcArrayLock, to prevent any undetectable deadlocks since this function
 * acquires them in that order.
 *
 * 若 already_locked 为 true，则调用方已排他持有
 * ReplicationSlotControlLock 与 ProcArrayLock。调用方必须先取得
 * ReplicationSlotControlLock 再取得 ProcArrayLock，
 * 因为本函数按此顺序加锁，否则可能产生无法检测的死锁。
 */
void
ReplicationSlotsComputeRequiredXmin(bool already_locked)
{
	int			i;
	TransactionId agg_xmin = InvalidTransactionId;
	TransactionId agg_catalog_xmin = InvalidTransactionId;

	Assert(ReplicationSlotCtl != NULL);
	Assert(!already_locked ||
		   (LWLockHeldByMeInMode(ReplicationSlotControlLock, LW_EXCLUSIVE) &&
			LWLockHeldByMeInMode(ProcArrayLock, LW_EXCLUSIVE)));

	/*
	 * Hold the ReplicationSlotControlLock until after updating the slot xmin
	 * values, so no backend updates the initial xmin for newly created slot
	 * concurrently. A shared lock is used here to minimize lock contention,
	 * especially when many slots exist and advancements occur frequently.
	 * This is safe since an exclusive lock is taken during initial slot xmin
	 * update in slot creation.
	 *
	 * 在更新槽的 xmin 之前一直持有 ReplicationSlotControlLock，
	 * 以免其他后端同时为新建槽更新初始 xmin。这里用共享锁以减少争用，
	 * 尤其是槽很多且推进很频繁时。这是安全的，因为创建槽时更新初始 xmin
	 * 会取得排他锁。
	 *
	 * One might think that we can hold the ProcArrayLock exclusively and
	 * update the slot xmin values, but it could increase lock contention on
	 * the ProcArrayLock, which is not great since this function can be called
	 * at non-negligible frequency.
	 *
	 * 也许可以排他持有 ProcArrayLock 再更新槽的 xmin，但这会加重
	 * ProcArrayLock 的争用。本函数调用频率不低，这样做不合适。
	 *
	 * Concurrent invocation of this function may cause the computed slot xmin
	 * to regress. However, this is harmless because tuples prior to the most
	 * recent xmin are no longer useful once advancement occurs (see
	 * LogicalConfirmReceivedLocation where the slot's xmin value is flushed
	 * before updating the effective_xmin). Thus, such regression merely
	 * prevents VACUUM from prematurely removing tuples without causing the
	 * early deletion of required data.
	 *
	 * 本函数并发调用可能导致算出的槽 xmin 回退。这是无害的：一旦发生推进，
	 * 比最近一次 xmin 更老的元组就不再有用。见
	 * LogicalConfirmReceivedLocation，那里先把槽的 xmin 刷盘再更新
	 * effective_xmin。因此这种回退只是阻止 VACUUM 过早删除元组，
	 * 不会提前删掉仍需要的数据。
	 */
	if (!already_locked)
		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		TransactionId effective_xmin;
		TransactionId effective_catalog_xmin;
		bool		invalidated;

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		effective_xmin = s->effective_xmin;
		effective_catalog_xmin = s->effective_catalog_xmin;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* invalidated slots need not apply
		 *
		 * 已作废的槽不必参与
		 */
		if (invalidated)
			continue;

		/* check the data xmin
		 *
		 * 检查数据 xmin
		 */
		if (TransactionIdIsValid(effective_xmin) &&
			(!TransactionIdIsValid(agg_xmin) ||
			 TransactionIdPrecedes(effective_xmin, agg_xmin)))
			agg_xmin = effective_xmin;

		/* check the catalog xmin
		 *
		 * 检查目录 xmin
		 */
		if (TransactionIdIsValid(effective_catalog_xmin) &&
			(!TransactionIdIsValid(agg_catalog_xmin) ||
			 TransactionIdPrecedes(effective_catalog_xmin, agg_catalog_xmin)))
			agg_catalog_xmin = effective_catalog_xmin;
	}

	ProcArraySetReplicationSlotXmin(agg_xmin, agg_catalog_xmin, already_locked);

	if (!already_locked)
		LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Compute the oldest restart LSN across all slots and inform xlog module.
 *
 * 计算所有槽中最老的 restart LSN，并通知 xlog 模块。
 *
 * Note: while max_slot_wal_keep_size is theoretically relevant for this
 * purpose, we don't try to account for that, because this module doesn't
 * know what to compare against.
 *
 * 注意：max_slot_wal_keep_size 理论上与此有关，但这里不把它算进去，
 * 因为本模块不知道该和什么比较。
 */
void
ReplicationSlotsComputeRequiredLSN(void)
{
	int			i;
	XLogRecPtr	min_required = InvalidXLogRecPtr;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		XLogRecPtr	restart_lsn;
		XLogRecPtr	last_saved_restart_lsn;
		bool		invalidated;
		ReplicationSlotPersistency persistency;

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		persistency = s->data.persistency;
		restart_lsn = s->data.restart_lsn;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		last_saved_restart_lsn = s->last_saved_restart_lsn;
		SpinLockRelease(&s->mutex);

		/* invalidated slots need not apply
		 *
		 * 已作废的槽不必参与
		 */
		if (invalidated)
			continue;

		/*
		 * For persistent slot use last_saved_restart_lsn to compute the
		 * oldest LSN for removal of WAL segments.  The segments between
		 * last_saved_restart_lsn and restart_lsn might be needed by a
		 * persistent slot in the case of database crash.  Non-persistent
		 * slots can't survive the database crash, so we don't care about
		 * last_saved_restart_lsn for them.
		 *
		 * 对持久槽，用 last_saved_restart_lsn 计算可删除 WAL 段的最老 LSN。
		 * 数据库崩溃时，持久槽可能仍需要 last_saved_restart_lsn 与
		 * restart_lsn 之间的段。非持久槽无法在崩溃后保留，因此不必关心它们的
		 * last_saved_restart_lsn。
		 */
		if (persistency == RS_PERSISTENT)
		{
			if (last_saved_restart_lsn != InvalidXLogRecPtr &&
				restart_lsn > last_saved_restart_lsn)
			{
				restart_lsn = last_saved_restart_lsn;
			}
		}

		if (restart_lsn != InvalidXLogRecPtr &&
			(min_required == InvalidXLogRecPtr ||
			 restart_lsn < min_required))
			min_required = restart_lsn;
	}
	LWLockRelease(ReplicationSlotControlLock);

	XLogSetReplicationSlotMinimumLSN(min_required);
}

/*
 * Compute the oldest WAL LSN required by *logical* decoding slots..
 *
 * 计算逻辑解码槽所需的最老 WAL LSN。
 *
 * Returns InvalidXLogRecPtr if logical decoding is disabled or no logical
 * slots exist.
 *
 * 若逻辑解码被关闭或不存在逻辑槽，则返回 InvalidXLogRecPtr。
 *
 * NB: this returns a value >= ReplicationSlotsComputeRequiredLSN(), since it
 * ignores physical replication slots.
 *
 * 注意：返回值不小于 ReplicationSlotsComputeRequiredLSN，
 * 因为这里忽略物理复制槽。
 *
 * The results aren't required frequently, so we don't maintain a precomputed
 * value like we do for ComputeRequiredLSN() and ComputeRequiredXmin().
 *
 * 结果并不经常需要，因此不像 ComputeRequiredLSN 与
 * ComputeRequiredXmin 那样维护预计算值。
 */
XLogRecPtr
ReplicationSlotsComputeLogicalRestartLSN(void)
{
	XLogRecPtr	result = InvalidXLogRecPtr;
	int			i;

	if (max_replication_slots <= 0)
		return InvalidXLogRecPtr;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;
		XLogRecPtr	restart_lsn;
		XLogRecPtr	last_saved_restart_lsn;
		bool		invalidated;
		ReplicationSlotPersistency persistency;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* cannot change while ReplicationSlotCtlLock is held
		 *
		 * 持有 ReplicationSlotCtlLock 期间不会改变
		 */
		if (!s->in_use)
			continue;

		/* we're only interested in logical slots
		 *
		 * 我们只关心逻辑槽
		 */
		if (!SlotIsLogical(s))
			continue;

		/* read once, it's ok if it increases while we're checking
		 *
		 * 只读一次；检查期间它变大也没关系
		 */
		SpinLockAcquire(&s->mutex);
		persistency = s->data.persistency;
		restart_lsn = s->data.restart_lsn;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		last_saved_restart_lsn = s->last_saved_restart_lsn;
		SpinLockRelease(&s->mutex);

		/* invalidated slots need not apply
		 *
		 * 已作废的槽不必参与
		 */
		if (invalidated)
			continue;

		/*
		 * For persistent slot use last_saved_restart_lsn to compute the
		 * oldest LSN for removal of WAL segments.  The segments between
		 * last_saved_restart_lsn and restart_lsn might be needed by a
		 * persistent slot in the case of database crash.  Non-persistent
		 * slots can't survive the database crash, so we don't care about
		 * last_saved_restart_lsn for them.
		 *
		 * 对持久槽，用 last_saved_restart_lsn 计算可删除 WAL 段的最老 LSN。
		 * 数据库崩溃时，持久槽可能仍需要 last_saved_restart_lsn 与
		 * restart_lsn 之间的段。非持久槽无法在崩溃后保留，因此不必关心它们的
		 * last_saved_restart_lsn。
		 */
		if (persistency == RS_PERSISTENT)
		{
			if (last_saved_restart_lsn != InvalidXLogRecPtr &&
				restart_lsn > last_saved_restart_lsn)
			{
				restart_lsn = last_saved_restart_lsn;
			}
		}

		if (restart_lsn == InvalidXLogRecPtr)
			continue;

		if (result == InvalidXLogRecPtr ||
			restart_lsn < result)
			result = restart_lsn;
	}

	LWLockRelease(ReplicationSlotControlLock);

	return result;
}

/*
 * ReplicationSlotsCountDBSlots -- count the number of slots that refer to the
 * passed database oid.
 *
 * ReplicationSlotsCountDBSlots：统计引用给定数据库 oid 的槽数量。
 *
 * Returns true if there are any slots referencing the database. *nslots will
 * be set to the absolute number of slots in the database, *nactive to ones
 * currently active.
 *
 * 若有槽引用该数据库则返回 true。nslots 会被设为该数据库中的槽总数，
 * nactive 为当前活动的数量。
 */
bool
ReplicationSlotsCountDBSlots(Oid dboid, int *nslots, int *nactive)
{
	int			i;

	*nslots = *nactive = 0;

	if (max_replication_slots <= 0)
		return false;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* cannot change while ReplicationSlotCtlLock is held
		 *
		 * 持有 ReplicationSlotCtlLock 期间不会改变
		 */
		if (!s->in_use)
			continue;

		/* only logical slots are database specific, skip
		 *
		 * 只有逻辑槽按数据库区分，跳过
		 */
		if (!SlotIsLogical(s))
			continue;

		/* not our database, skip
		 *
		 * 不是我们的数据库，跳过
		 */
		if (s->data.database != dboid)
			continue;

		/* NB: intentionally counting invalidated slots
		 *
		 * 注意：有意把已作废的槽也算进去
		 */

		/* count slots with spinlock held
		 *
		 * 持有自旋锁时计数
		 */
		SpinLockAcquire(&s->mutex);
		(*nslots)++;
		if (s->active_pid != 0)
			(*nactive)++;
		SpinLockRelease(&s->mutex);
	}
	LWLockRelease(ReplicationSlotControlLock);

	if (*nslots > 0)
		return true;
	return false;
}

/*
 * ReplicationSlotsDropDBSlots -- Drop all db-specific slots relating to the
 * passed database oid. The caller should hold an exclusive lock on the
 * pg_database oid for the database to prevent creation of new slots on the db
 * or replay from existing slots.
 *
 * ReplicationSlotsDropDBSlots：删除与给定数据库 oid
 * 相关的全部数据库专用槽。调用方应对该数据库的 pg_database oid
 * 持有排他锁，以防止在该库上创建新槽或从已有槽重放。
 *
 * Another session that concurrently acquires an existing slot on the target DB
 * (most likely to drop it) may cause this function to ERROR. If that happens
 * it may have dropped some but not all slots.
 *
 * 另一个会话若同时占用目标库上的已有槽，多半是为了删除它，
 * 可能导致本函数 ERROR。一旦发生，可能只删除了部分槽。
 *
 * This routine isn't as efficient as it could be - but we don't drop
 * databases often, especially databases with lots of slots.
 *
 * 这个例程还可以更快，但删除数据库并不常见，尤其是带有很多槽的数据库。
 */
void
ReplicationSlotsDropDBSlots(Oid dboid)
{
	int			i;

	if (max_replication_slots <= 0)
		return;

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;
		char	   *slotname;
		int			active_pid;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* cannot change while ReplicationSlotCtlLock is held
		 *
		 * 持有 ReplicationSlotCtlLock 期间不会改变
		 */
		if (!s->in_use)
			continue;

		/* only logical slots are database specific, skip
		 *
		 * 只有逻辑槽按数据库区分，跳过
		 */
		if (!SlotIsLogical(s))
			continue;

		/* not our database, skip
		 *
		 * 不是我们的数据库，跳过
		 */
		if (s->data.database != dboid)
			continue;

		/* NB: intentionally including invalidated slots
		 *
		 * 注意：有意把已作废的槽也包括进来
		 */

		/* acquire slot, so ReplicationSlotDropAcquired can be reused 
		 *
		 * 占用该槽，以便复用 ReplicationSlotDropAcquired
		 */
		SpinLockAcquire(&s->mutex);
		/* can't change while ReplicationSlotControlLock is held
		 *
		 * 持有 ReplicationSlotControlLock 期间不会改变
		 */
		slotname = NameStr(s->data.name);
		active_pid = s->active_pid;
		if (active_pid == 0)
		{
			MyReplicationSlot = s;
			s->active_pid = MyProcPid;
		}
		SpinLockRelease(&s->mutex);

		/*
		 * Even though we hold an exclusive lock on the database object a
		 * logical slot for that DB can still be active, e.g. if it's
		 * concurrently being dropped by a backend connected to another DB.
		 *
		 * 即使我们对数据库对象持有排他锁，该库的逻辑槽仍可能处于活动状态，
		 * 例如被连到其他数据库的后端正在删除它。
		 *
		 * That's fairly unlikely in practice, so we'll just bail out.
		 *
		 * 实践中这相当少见，因此直接退出。
		 *
		 * The slot sync worker holds a shared lock on the database before
		 * operating on synced logical slots to avoid conflict with the drop
		 * happening here. The persistent synced slots are thus safe but there
		 * is a possibility that the slot sync worker has created a temporary
		 * slot (which stays active even on release) and we are trying to drop
		 * that here. In practice, the chances of hitting this scenario are
		 * less as during slot synchronization, the temporary slot is
		 * immediately converted to persistent and thus is safe due to the
		 * shared lock taken on the database. So, we'll just bail out in such
		 * a case.
		 *
		 * 槽同步工作进程在操作已同步的逻辑槽之前会对数据库持有共享锁，
		 * 以免与此处的删除冲突。因此持久的已同步槽是安全的，
		 * 但仍可能出现这种情况：槽同步工作进程创建了临时槽，释放后仍保持活动，
		 * 而我们正要在这里删除它。实践中碰到的机会较小，
		 * 因为槽同步时临时槽会立刻转为持久槽，并因数据库上的共享锁而安全。
		 * 遇到这种情况就直接退出。
		 *
		 * XXX: We can consider shutting down the slot sync worker before
		 * trying to drop synced temporary slots here.
		 *
		 * XXX：可以考虑在尝试删除已同步的临时槽之前先关掉槽同步工作进程。
		 */
		if (active_pid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication slot \"%s\" is active for PID %d",
							slotname, active_pid)));

		/*
		 * To avoid duplicating ReplicationSlotDropAcquired() and to avoid
		 * holding ReplicationSlotControlLock over filesystem operations,
		 * release ReplicationSlotControlLock and use
		 * ReplicationSlotDropAcquired.
		 *
		 * 为避免重复实现 ReplicationSlotDropAcquired，
		 * 也为避免在文件系统操作期间持有 ReplicationSlotControlLock，先释放
		 * ReplicationSlotControlLock，再调用 ReplicationSlotDropAcquired。
		 *
		 * As that means the set of slots could change, restart scan from the
		 * beginning each time we release the lock.
		 *
		 * 这样做意味着槽集合可能变化，因此每次释放锁后都从头重新扫描。
		 */
		LWLockRelease(ReplicationSlotControlLock);
		ReplicationSlotDropAcquired();
		goto restart;
	}
	LWLockRelease(ReplicationSlotControlLock);
}


/*
 * Check whether the server's configuration supports using replication
 * slots.
 *
 * 检查服务器配置是否支持使用复制槽。
 */
void
CheckSlotRequirements(void)
{
	/*
	 * NB: Adding a new requirement likely means that RestoreSlotFromDisk()
	 * needs the same check.
	 *
	 * 注意：新增一项要求时，RestoreSlotFromDisk 很可能也需要同样的检查。
	 */

	if (max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if \"max_replication_slots\" > 0")));

	if (wal_level < WAL_LEVEL_REPLICA)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if \"wal_level\" >= \"replica\"")));
}

/*
 * Check whether the user has privilege to use replication slots.
 *
 * 检查用户是否有权使用复制槽。
 */
void
CheckSlotPermissions(void)
{
	if (!has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to use replication slots"),
				 errdetail("Only roles with the %s attribute may use replication slots.",
						   "REPLICATION")));
}

/*
 * Reserve WAL for the currently active slot.
 *
 * 为当前活动槽保留 WAL。
 *
 * Compute and set restart_lsn in a manner that's appropriate for the type of
 * the slot and concurrency safe.
 *
 * 按槽的类型以并发安全的方式计算并设置 restart_lsn。
 */
void
ReplicationSlotReserveWal(void)
{
	ReplicationSlot *slot = MyReplicationSlot;
	XLogSegNo	segno;
	XLogRecPtr	restart_lsn;

	Assert(slot != NULL);
	Assert(slot->data.restart_lsn == InvalidXLogRecPtr);
	Assert(slot->last_saved_restart_lsn == InvalidXLogRecPtr);

	/*
	 * The replication slot mechanism is used to prevent the removal of
	 * required WAL.
	 *
	 * 复制槽机制用于阻止删除仍需要的 WAL。
	 *
	 * Acquire an exclusive lock to prevent the checkpoint process from
	 * concurrently computing the minimum slot LSN (see
	 * CheckPointReplicationSlots). This ensures that the WAL reserved for
	 * replication cannot be removed during a checkpoint.
	 *
	 * 取得排他锁，防止检查点进程同时计算最小槽 LSN，见
	 * CheckPointReplicationSlots。这样可保证为复制保留的 WAL
	 * 不会在检查点期间被删除。
	 *
	 * The mechanism is reliable because if WAL reservation occurs first, the
	 * checkpoint must wait for the restart_lsn update before determining the
	 * minimum non-removable LSN. On the other hand, if the checkpoint happens
	 * first, subsequent WAL reservations will select positions at or beyond
	 * the redo pointer of that checkpoint.
	 *
	 * 该机制是可靠的：若先保留 WAL，检查点必须等 restart_lsn
	 * 更新后才能确定不可删除的最小 LSN。若先发生检查点，随后的 WAL
	 * 保留会选择该检查点 redo 指针或其之后的位置。
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/*
	 * For logical slots log a standby snapshot and start logical decoding at
	 * exactly that position. That allows the slot to start up more quickly.
	 * But on a standby we cannot do WAL writes, so just use the replay
	 * pointer; effectively, an attempt to create a logical slot on standby
	 * will cause it to wait for an xl_running_xact record to be logged
	 * independently on the primary, so that a snapshot can be built using the
	 * record.
	 *
	 * 对逻辑槽，先记录一条 standby snapshot，并恰好从该位置开始逻辑解码，
	 * 这样槽能更快启动。但备库上不能写 WAL，因此只用重放指针；实际上，
	 * 在备库上创建逻辑槽会等待主库另行写入一条 xl_running_xact，
	 * 才能用该记录建立快照。
	 *
	 * None of this is needed (or indeed helpful) for physical slots as
	 * they'll start replay at the last logged checkpoint anyway. Instead,
	 * return the location of the last redo LSN, where a base backup has to
	 * start replay at.
	 *
	 * 物理槽不需要也不借助上述做法，
	 * 因为它们本来就会从最后一条已记录的检查点开始重放。
	 * 这里改为返回最后的 redo LSN，基础备份必须从那里开始重放。
	 */
	if (SlotIsPhysical(slot))
		restart_lsn = GetRedoRecPtr();
	else if (RecoveryInProgress())
		restart_lsn = GetXLogReplayRecPtr(NULL);
	else
		restart_lsn = GetXLogInsertRecPtr();

	SpinLockAcquire(&slot->mutex);
	slot->data.restart_lsn = restart_lsn;
	SpinLockRelease(&slot->mutex);

	/* prevent WAL removal as fast as possible
	 *
	 * 尽快阻止 WAL 删除
	 */
	ReplicationSlotsComputeRequiredLSN();

	/* Checkpoint shouldn't remove the required WAL.
	 *
	 * 检查点不应删除所需的 WAL。
	 */
	XLByteToSeg(slot->data.restart_lsn, segno, wal_segment_size);
	if (XLogGetLastRemovedSegno() >= segno)
		elog(ERROR, "WAL required by replication slot %s has been removed concurrently",
			 NameStr(slot->data.name));

	LWLockRelease(ReplicationSlotAllocationLock);

	if (!RecoveryInProgress() && SlotIsLogical(slot))
	{
		XLogRecPtr	flushptr;

		/* make sure we have enough information to start
		 *
		 * 确保有足够信息可以启动
		 */
		flushptr = LogStandbySnapshot();

		/* and make sure it's fsynced to disk
		 *
		 * 并确保它已 fsync 到磁盘
		 */
		XLogFlush(flushptr);
	}
}

/*
 * Report that replication slot needs to be invalidated
 *
 * 报告复制槽需要作废
 */
static void
ReportSlotInvalidation(ReplicationSlotInvalidationCause cause,
					   bool terminating,
					   int pid,
					   NameData slotname,
					   XLogRecPtr restart_lsn,
					   XLogRecPtr oldestLSN,
					   TransactionId snapshotConflictHorizon,
					   long slot_idle_seconds)
{
	StringInfoData err_detail;
	StringInfoData err_hint;

	initStringInfo(&err_detail);
	initStringInfo(&err_hint);

	switch (cause)
	{
		case RS_INVAL_WAL_REMOVED:
			{
				uint64		ex = oldestLSN - restart_lsn;

				appendStringInfo(&err_detail,
								 ngettext("The slot's restart_lsn %X/%X exceeds the limit by %" PRIu64 " byte.",
										  "The slot's restart_lsn %X/%X exceeds the limit by %" PRIu64 " bytes.",
										  ex),
								 LSN_FORMAT_ARGS(restart_lsn),
								 ex);
				/* translator: %s is a GUC variable name
				 *
				 * 翻译者：%s 是 GUC 变量名
				 */
				appendStringInfo(&err_hint, _("You might need to increase \"%s\"."),
								 "max_slot_wal_keep_size");
				break;
			}
		case RS_INVAL_HORIZON:
			appendStringInfo(&err_detail, _("The slot conflicted with xid horizon %u."),
							 snapshotConflictHorizon);
			break;

		case RS_INVAL_WAL_LEVEL:
			appendStringInfoString(&err_detail, _("Logical decoding on standby requires \"wal_level\" >= \"logical\" on the primary server."));
			break;

		case RS_INVAL_IDLE_TIMEOUT:
			{
				/* translator: %s is a GUC variable name
				 *
				 * 翻译者：%s 是 GUC 变量名
				 */
				appendStringInfo(&err_detail, _("The slot's idle time of %lds exceeds the configured \"%s\" duration of %ds."),
								 slot_idle_seconds, "idle_replication_slot_timeout",
								 idle_replication_slot_timeout_secs);
				/* translator: %s is a GUC variable name
				 *
				 * 翻译者：%s 是 GUC 变量名
				 */
				appendStringInfo(&err_hint, _("You might need to increase \"%s\"."),
								 "idle_replication_slot_timeout");
				break;
			}
		case RS_INVAL_NONE:
			pg_unreachable();
	}

	ereport(LOG,
			terminating ?
			errmsg("terminating process %d to release replication slot \"%s\"",
				   pid, NameStr(slotname)) :
			errmsg("invalidating obsolete replication slot \"%s\"",
				   NameStr(slotname)),
			errdetail_internal("%s", err_detail.data),
			err_hint.len ? errhint("%s", err_hint.data) : 0);

	pfree(err_detail.data);
	pfree(err_hint.data);
}

/*
 * Can we invalidate an idle replication slot?
 *
 * 能否作废一个空闲复制槽？
 *
 * Idle timeout invalidation is allowed only when:
 *
 * 仅在以下条件都满足时才允许因空闲超时而作废：
 *
 * 1. Idle timeout is set
 * 2. Slot has reserved WAL
 * 3. Slot is inactive
 * 4. The slot is not being synced from the primary while the server is in
 *	  recovery. This is because synced slots are always considered to be
 *	  inactive because they don't perform logical decoding to produce changes.
 *
 * 1. 已设置空闲超时。2. 槽已保留 WAL。3. 槽处于非活动状态。4.
 * 服务器处于恢复期间时，该槽并非正在从主库同步。
 * 原因是同步槽不执行逻辑解码来产生变更，因此总被视为非活动。
 */
static inline bool
CanInvalidateIdleSlot(ReplicationSlot *s)
{
	return (idle_replication_slot_timeout_secs != 0 &&
			!XLogRecPtrIsInvalid(s->data.restart_lsn) &&
			s->inactive_since > 0 &&
			!(RecoveryInProgress() && s->data.synced));
}

/*
 * DetermineSlotInvalidationCause - Determine the cause for which a slot
 * becomes invalid among the given possible causes.
 *
 * DetermineSlotInvalidationCause：在给定的可能原因中，
 * 确定槽因哪一个而作废。
 *
 * This function sequentially checks all possible invalidation causes and
 * returns the first one for which the slot is eligible for invalidation.
 *
 * 本函数按顺序检查所有可能的作废原因，返回槽符合条件的第一个原因。
 */
static ReplicationSlotInvalidationCause
DetermineSlotInvalidationCause(uint32 possible_causes, ReplicationSlot *s,
							   XLogRecPtr oldestLSN, Oid dboid,
							   TransactionId snapshotConflictHorizon,
							   TimestampTz *inactive_since, TimestampTz now)
{
	Assert(possible_causes != RS_INVAL_NONE);

	if (possible_causes & RS_INVAL_WAL_REMOVED)
	{
		XLogRecPtr	restart_lsn = s->data.restart_lsn;

		if (restart_lsn != InvalidXLogRecPtr &&
			restart_lsn < oldestLSN)
			return RS_INVAL_WAL_REMOVED;
	}

	if (possible_causes & RS_INVAL_HORIZON)
	{
		/* invalid DB oid signals a shared relation
		 *
		 * 无效的数据库 oid 表示共享关系
		 */
		if (SlotIsLogical(s) &&
			(dboid == InvalidOid || dboid == s->data.database))
		{
			TransactionId effective_xmin = s->effective_xmin;
			TransactionId catalog_effective_xmin = s->effective_catalog_xmin;

			if (TransactionIdIsValid(effective_xmin) &&
				TransactionIdPrecedesOrEquals(effective_xmin,
											  snapshotConflictHorizon))
				return RS_INVAL_HORIZON;
			else if (TransactionIdIsValid(catalog_effective_xmin) &&
					 TransactionIdPrecedesOrEquals(catalog_effective_xmin,
												   snapshotConflictHorizon))
				return RS_INVAL_HORIZON;
		}
	}

	if (possible_causes & RS_INVAL_WAL_LEVEL)
	{
		if (SlotIsLogical(s))
			return RS_INVAL_WAL_LEVEL;
	}

	if (possible_causes & RS_INVAL_IDLE_TIMEOUT)
	{
		Assert(now > 0);

		if (CanInvalidateIdleSlot(s))
		{
			/*
			 * Simulate the invalidation due to idle_timeout to test the
			 * timeout behavior promptly, without waiting for it to trigger
			 * naturally.
			 *
			 * 模拟因 idle_timeout 而作废，以便及时测试超时行为，
			 * 而不必等它自然触发。
			 */
#ifdef USE_INJECTION_POINTS
			if (IS_INJECTION_POINT_ATTACHED("slot-timeout-inval"))
			{
				*inactive_since = 0;	/* since the beginning of time
				 *
				 * 自时间起点起
				 */
				return RS_INVAL_IDLE_TIMEOUT;
			}
#endif

			/*
			 * Check if the slot needs to be invalidated due to
			 * idle_replication_slot_timeout GUC.
			 *
			 * 检查槽是否因 idle_replication_slot_timeout 这个 GUC 而需要作废。
			 */
			if (TimestampDifferenceExceedsSeconds(s->inactive_since, now,
												  idle_replication_slot_timeout_secs))
			{
				*inactive_since = s->inactive_since;
				return RS_INVAL_IDLE_TIMEOUT;
			}
		}
	}

	return RS_INVAL_NONE;
}

/*
 * Helper for InvalidateObsoleteReplicationSlots
 *
 * InvalidateObsoleteReplicationSlots 的辅助函数
 *
 * Acquires the given slot and mark it invalid, if necessary and possible.
 *
 * 占用给定槽，并在必要且可能时把它标为作废。
 *
 * Returns whether ReplicationSlotControlLock was released in the interim (and
 * in that case we're not holding the lock at return, otherwise we are).
 *
 * 返回期间是否释放过 ReplicationSlotControlLock。若释放过，
 * 返回时不再持有该锁，否则仍然持有。
 *
 * Sets *invalidated true if the slot was invalidated. (Untouched otherwise.)
 *
 * 若槽被作废，则把 invalidated 设为 true，否则不改动它。
 *
 * This is inherently racy, because we release the LWLock
 * for syscalls, so caller must restart if we return true.
 *
 * 这里天生存在竞争，因为做系统调用时会释放 LWLock，因此若返回 true，
 * 调用方必须重新开始。
 */
static bool
InvalidatePossiblyObsoleteSlot(uint32 possible_causes,
							   ReplicationSlot *s,
							   XLogRecPtr oldestLSN,
							   Oid dboid, TransactionId snapshotConflictHorizon,
							   bool *invalidated)
{
	int			last_signaled_pid = 0;
	bool		released_lock = false;
	TimestampTz inactive_since = 0;

	for (;;)
	{
		XLogRecPtr	restart_lsn;
		NameData	slotname;
		int			active_pid = 0;
		ReplicationSlotInvalidationCause invalidation_cause = RS_INVAL_NONE;
		TimestampTz now = 0;
		long		slot_idle_secs = 0;

		Assert(LWLockHeldByMeInMode(ReplicationSlotControlLock, LW_SHARED));

		if (!s->in_use)
		{
			if (released_lock)
				LWLockRelease(ReplicationSlotControlLock);
			break;
		}

		if (possible_causes & RS_INVAL_IDLE_TIMEOUT)
		{
			/*
			 * Assign the current time here to avoid system call overhead
			 * while holding the spinlock in subsequent code.
			 *
			 * 在这里取当前时间，以免后续代码持有自旋锁时付出系统调用开销。
			 */
			now = GetCurrentTimestamp();
		}

		/*
		 * Check if the slot needs to be invalidated. If it needs to be
		 * invalidated, and is not currently acquired, acquire it and mark it
		 * as having been invalidated.  We do this with the spinlock held to
		 * avoid race conditions -- for example the restart_lsn could move
		 * forward, or the slot could be dropped.
		 *
		 * 检查槽是否需要作废。若需要作废且当前未被占用，则占用它并标为已作废。
		 * 持有自旋锁完成这些操作，以免竞争，例如 restart_lsn
		 * 向前移动或槽被删除。
		 */
		SpinLockAcquire(&s->mutex);

		restart_lsn = s->data.restart_lsn;

		/* we do nothing if the slot is already invalid
		 *
		 * 若槽已经作废，则什么也不做
		 */
		if (s->data.invalidated == RS_INVAL_NONE)
			invalidation_cause = DetermineSlotInvalidationCause(possible_causes,
																s, oldestLSN,
																dboid,
																snapshotConflictHorizon,
																&inactive_since,
																now);

		/* if there's no invalidation, we're done
		 *
		 * 若没有作废原因，则结束
		 */
		if (invalidation_cause == RS_INVAL_NONE)
		{
			SpinLockRelease(&s->mutex);
			if (released_lock)
				LWLockRelease(ReplicationSlotControlLock);
			break;
		}

		slotname = s->data.name;
		active_pid = s->active_pid;

		/*
		 * If the slot can be acquired, do so and mark it invalidated
		 * immediately.  Otherwise we'll signal the owning process, below, and
		 * retry.
		 *
		 * 若能够占用该槽，则立即占用并标为作废。否则向拥有者进程发信号，
		 * 见下文，并重试。
		 *
		 * Note: Unlike other slot attributes, slot's inactive_since can't be
		 * changed until the acquired slot is released or the owning process
		 * is terminated. So, the inactive slot can only be invalidated
		 * immediately without being terminated.
		 *
		 * 注意：与槽的其他属性不同，inactive_since
		 * 在已占用的槽被释放或拥有者进程终止之前不能改变。
		 * 因此非活动槽只能立即作废，而不能通过终止进程来作废。
		 */
		if (active_pid == 0)
		{
			MyReplicationSlot = s;
			s->active_pid = MyProcPid;
			s->data.invalidated = invalidation_cause;

			/*
			 * XXX: We should consider not overwriting restart_lsn and instead
			 * just rely on .invalidated.
			 *
			 * XXX：应考虑不要覆盖 restart_lsn，而只依赖 invalidated 字段。
			 */
			if (invalidation_cause == RS_INVAL_WAL_REMOVED)
			{
				s->data.restart_lsn = InvalidXLogRecPtr;
				s->last_saved_restart_lsn = InvalidXLogRecPtr;
			}

			/* Let caller know
			 *
			 * 通知调用方
			 */
			*invalidated = true;
		}

		SpinLockRelease(&s->mutex);

		/*
		 * Calculate the idle time duration of the slot if slot is marked
		 * invalidated with RS_INVAL_IDLE_TIMEOUT.
		 *
		 * 若槽因 RS_INVAL_IDLE_TIMEOUT 被标为作废，则计算其空闲时长。
		 */
		if (invalidation_cause == RS_INVAL_IDLE_TIMEOUT)
		{
			int			slot_idle_usecs;

			TimestampDifference(inactive_since, now, &slot_idle_secs,
								&slot_idle_usecs);
		}

		if (active_pid != 0)
		{
			/*
			 * Prepare the sleep on the slot's condition variable before
			 * releasing the lock, to close a possible race condition if the
			 * slot is released before the sleep below.
			 *
			 * 在释放锁之前准备在槽的条件变量上睡眠，以堵住一种竞争：
			 * 槽可能在下面的睡眠之前就被释放。
			 */
			ConditionVariablePrepareToSleep(&s->active_cv);

			LWLockRelease(ReplicationSlotControlLock);
			released_lock = true;

			/*
			 * Signal to terminate the process that owns the slot, if we
			 * haven't already signalled it.  (Avoidance of repeated
			 * signalling is the only reason for there to be a loop in this
			 * routine; otherwise we could rely on caller's restart loop.)
			 *
			 * 若尚未向拥有该槽的进程发过信号，则发信号终止它。
			 * 避免重复发信号是本例程存在循环的唯一原因；
			 * 否则可以依赖调用方的重启循环。
			 *
			 * There is the race condition that other process may own the slot
			 * after its current owner process is terminated and before this
			 * process owns it. To handle that, we signal only if the PID of
			 * the owning process has changed from the previous time. (This
			 * logic assumes that the same PID is not reused very quickly.)
			 *
			 * 存在这样一种竞争：当前拥有者进程终止之后、本进程占用之前，
			 * 其他进程可能先占用该槽。为此，仅当拥有者 PID 与上次不同时才发信号。
			 * 该逻辑假定同一 PID 不会很快被复用。
			 */
			if (last_signaled_pid != active_pid)
			{
				ReportSlotInvalidation(invalidation_cause, true, active_pid,
									   slotname, restart_lsn,
									   oldestLSN, snapshotConflictHorizon,
									   slot_idle_secs);

				if (MyBackendType == B_STARTUP)
					(void) SendProcSignal(active_pid,
										  PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT,
										  INVALID_PROC_NUMBER);
				else
					(void) kill(active_pid, SIGTERM);

				last_signaled_pid = active_pid;
			}

			/* Wait until the slot is released.
			 *
			 * 等待槽被释放。
			 */
			ConditionVariableSleep(&s->active_cv,
								   WAIT_EVENT_REPLICATION_SLOT_DROP);

			/*
			 * Re-acquire lock and start over; we expect to invalidate the
			 * slot next time (unless another process acquires the slot in the
			 * meantime).
			 *
			 * 重新获取锁并从头开始；预期下次会作废该槽，
			 * 除非其间另一个进程占用了它。
			 *
			 * Note: It is possible for a slot to advance its restart_lsn or
			 * xmin values sufficiently between when we release the mutex and
			 * when we recheck, moving from a conflicting state to a non
			 * conflicting state.  This is intentional and safe: if the slot
			 * has caught up while we're busy here, the resources we were
			 * concerned about (WAL segments or tuples) have not yet been
			 * removed, and there's no reason to invalidate the slot.
			 *
			 * 注意：在我们释放 mutex 与再次检查之间，槽可能把 restart_lsn 或 xmin
			 * 推进得足够远，从冲突状态变为不冲突。这是有意的，也是安全的：
			 * 若槽在我们忙碌时已经追上，所担心的 WAL 段或元组尚未被删除，
			 * 就没有理由作废该槽。
			 */
			LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
			continue;
		}
		else
		{
			/*
			 * We hold the slot now and have already invalidated it; flush it
			 * to ensure that state persists.
			 *
			 * 我们现在持有该槽并且已经把它作废；刷盘以确保该状态得以保留。
			 *
			 * Don't want to hold ReplicationSlotControlLock across file
			 * system operations, so release it now but be sure to tell caller
			 * to restart from scratch.
			 *
			 * 不想在文件系统操作期间持有 ReplicationSlotControlLock，
			 * 因此现在释放它，但必须告诉调用方从头重新开始。
			 */
			LWLockRelease(ReplicationSlotControlLock);
			released_lock = true;

			/* Make sure the invalidated state persists across server restart
			 *
			 * 确保作废状态在服务器重启后仍然保留
			 */
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			ReplicationSlotRelease();

			ReportSlotInvalidation(invalidation_cause, false, active_pid,
								   slotname, restart_lsn,
								   oldestLSN, snapshotConflictHorizon,
								   slot_idle_secs);

			/* done with this slot for now
			 *
			 * 目前对该槽的处理结束
			 */
			break;
		}
	}

	Assert(released_lock == !LWLockHeldByMe(ReplicationSlotControlLock));

	return released_lock;
}

/*
 * Invalidate slots that require resources about to be removed.
 *
 * 作废那些依赖即将被删除的资源的槽。
 *
 * Returns true when any slot have got invalidated.
 *
 * 若有任何槽被作废则返回 true。
 *
 * Whether a slot needs to be invalidated depends on the invalidation cause.
 * A slot is invalidated if it:
 * - RS_INVAL_WAL_REMOVED: requires a LSN older than the given segment
 * - RS_INVAL_HORIZON: requires a snapshot <= the given horizon in the given
 *   db; dboid may be InvalidOid for shared relations
 * - RS_INVAL_WAL_LEVEL: is logical and wal_level is insufficient
 * - RS_INVAL_IDLE_TIMEOUT: has been idle longer than the configured
 *   "idle_replication_slot_timeout" duration.
 *
 * 槽是否需要作废取决于作废原因。槽会在以下情况作废：
 * RS_INVAL_WAL_REMOVED，需要比给定段更老的 LSN；RS_INVAL_HORIZON，
 * 在给定数据库中需要不晚于给定 horizon 的快照，共享关系的 dboid
 * 可以是 InvalidOid；RS_INVAL_WAL_LEVEL，是逻辑槽且 wal_level 不足；
 * RS_INVAL_IDLE_TIMEOUT，空闲时间超过 idle_replication_slot_timeout
 * 所配置的时长。
 *
 * Note: This function attempts to invalidate the slot for multiple possible
 * causes in a single pass, minimizing redundant iterations. The "cause"
 * parameter can be a MASK representing one or more of the defined causes.
 *
 * 注意：本函数试图在一次遍历中按多个可能原因作废槽，以减少重复扫描。
 * cause 参数可以是表示一种或多种已定义原因的掩码。
 *
 * NB - this runs as part of checkpoint, so avoid raising errors if possible.
 *
 * 注意：本函数作为检查点的一部分运行，因此尽量不要抛出错误。
 */
bool
InvalidateObsoleteReplicationSlots(uint32 possible_causes,
								   XLogSegNo oldestSegno, Oid dboid,
								   TransactionId snapshotConflictHorizon)
{
	XLogRecPtr	oldestLSN;
	bool		invalidated = false;

	Assert(!(possible_causes & RS_INVAL_HORIZON) || TransactionIdIsValid(snapshotConflictHorizon));
	Assert(!(possible_causes & RS_INVAL_WAL_REMOVED) || oldestSegno > 0);
	Assert(possible_causes != RS_INVAL_NONE);

	if (max_replication_slots == 0)
		return invalidated;

	XLogSegNoOffsetToRecPtr(oldestSegno, 0, wal_segment_size, oldestLSN);

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		/* Prevent invalidation of logical slots during binary upgrade
		 *
		 * 二进制升级期间阻止逻辑槽作废
		 */
		if (SlotIsLogical(s) && IsBinaryUpgrade)
			continue;

		if (InvalidatePossiblyObsoleteSlot(possible_causes, s, oldestLSN, dboid,
										   snapshotConflictHorizon,
										   &invalidated))
		{
			/* if the lock was released, start from scratch
			 *
			 * 若锁已释放，则从头开始
			 */
			goto restart;
		}
	}
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * If any slots have been invalidated, recalculate the resource limits.
	 *
	 * 若有槽被作废，重新计算资源限制。
	 */
	if (invalidated)
	{
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
	}

	return invalidated;
}

/*
 * Flush all replication slots to disk.
 *
 * 把所有复制槽刷到磁盘。
 *
 * It is convenient to flush dirty replication slots at the time of checkpoint.
 * Additionally, in case of a shutdown checkpoint, we also identify the slots
 * for which the confirmed_flush LSN has been updated since the last time it
 * was saved and flush them.
 *
 * 在检查点时刷出脏的复制槽比较方便。另外，若是关闭检查点，
 * 还会找出自上次保存以来 confirmed_flush LSN 已更新的槽并刷出它们。
 */
void
CheckPointReplicationSlots(bool is_shutdown)
{
	int			i;
	bool		last_saved_restart_lsn_updated = false;

	elog(DEBUG1, "performing replication slot checkpoint");

	/*
	 * Prevent any slot from being created/dropped while we're active. As we
	 * explicitly do *not* want to block iterating over replication_slots or
	 * acquiring a slot we cannot take the control lock - but that's OK,
	 * because holding ReplicationSlotAllocationLock is strictly stronger, and
	 * enough to guarantee that nobody can change the in_use bits on us.
	 *
	 * 我们活动期间阻止创建或删除任何槽。我们明确不想阻塞对
	 * replication_slots 的遍历或对槽的占用，因此不能拿控制锁。这是可以的，
	 * 因为持有 ReplicationSlotAllocationLock 严格更强，
	 * 足以保证别人不能改动我们看到的 in_use 位。
	 *
	 * Additionally, acquiring the Allocation lock is necessary to serialize
	 * the slot flush process with concurrent slot WAL reservation. This
	 * ensures that the WAL position being reserved is either flushed to disk
	 * or is beyond or equal to the redo pointer of the current checkpoint
	 * (See ReplicationSlotReserveWal for details).
	 *
	 * 另外，取得分配锁是为了把槽刷盘与并发的槽 WAL 保留串行化。
	 * 这样可保证正在保留的 WAL 位置要么已刷到磁盘，要么不早于当前检查点的
	 * redo 指针。详见 ReplicationSlotReserveWal。
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		char		path[MAXPGPATH];

		if (!s->in_use)
			continue;

		/* save the slot to disk, locking is handled in SaveSlotToPath()
		 *
		 * 把槽保存到磁盘，加锁由 SaveSlotToPath 处理
		 */
		sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(s->data.name));

		/*
		 * Slot's data is not flushed each time the confirmed_flush LSN is
		 * updated as that could lead to frequent writes.  However, we decide
		 * to force a flush of all logical slot's data at the time of shutdown
		 * if the confirmed_flush LSN is changed since we last flushed it to
		 * disk.  This helps in avoiding an unnecessary retreat of the
		 * confirmed_flush LSN after restart.
		 *
		 * 并非每次更新 confirmed_flush LSN 都把槽数据刷盘，
		 * 否则写盘会过于频繁。不过在关闭时，若逻辑槽的 confirmed_flush LSN
		 * 自上次刷盘以来有变化，就强制刷出全部逻辑槽数据。这有助于避免重启后
		 * confirmed_flush LSN 不必要地后退。
		 */
		if (is_shutdown && SlotIsLogical(s))
		{
			SpinLockAcquire(&s->mutex);

			if (s->data.invalidated == RS_INVAL_NONE &&
				s->data.confirmed_flush > s->last_saved_confirmed_flush)
			{
				s->just_dirtied = true;
				s->dirty = true;
			}
			SpinLockRelease(&s->mutex);
		}

		/*
		 * Track if we're going to update slot's last_saved_restart_lsn. We
		 * need this to know if we need to recompute the required LSN.
		 *
		 * 记录是否将更新槽的 last_saved_restart_lsn。
		 * 需要据此判断是否要重新计算所需 LSN。
		 */
		if (s->last_saved_restart_lsn != s->data.restart_lsn)
			last_saved_restart_lsn_updated = true;

		SaveSlotToPath(s, path, LOG);
	}
	LWLockRelease(ReplicationSlotAllocationLock);

	/*
	 * Recompute the required LSN if SaveSlotToPath() updated
	 * last_saved_restart_lsn for any slot.
	 *
	 * 若 SaveSlotToPath 更新了任何槽的 last_saved_restart_lsn，
	 * 则重新计算所需 LSN。
	 */
	if (last_saved_restart_lsn_updated)
		ReplicationSlotsComputeRequiredLSN();
}

/*
 * Load all replication slots from disk into memory at server startup. This
 * needs to be run before we start crash recovery.
 *
 * 在服务器启动时把磁盘上的全部复制槽装入内存。
 * 必须在开始崩溃恢复之前运行。
 */
void
StartupReplicationSlots(void)
{
	DIR		   *replication_dir;
	struct dirent *replication_de;

	elog(DEBUG1, "starting up replication slots");

	/* restore all slots by iterating over all on-disk entries
	 *
	 * 遍历磁盘上的全部条目以恢复所有槽
	 */
	replication_dir = AllocateDir(PG_REPLSLOT_DIR);
	while ((replication_de = ReadDir(replication_dir, PG_REPLSLOT_DIR)) != NULL)
	{
		char		path[MAXPGPATH + sizeof(PG_REPLSLOT_DIR)];
		PGFileType	de_type;

		if (strcmp(replication_de->d_name, ".") == 0 ||
			strcmp(replication_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", PG_REPLSLOT_DIR, replication_de->d_name);
		de_type = get_dirent_type(path, replication_de, false, DEBUG1);

		/* we're only creating directories here, skip if it's not our's
		 *
		 * 这里只是在创建目录，不是我们的则跳过
		 */
		if (de_type != PGFILETYPE_ERROR && de_type != PGFILETYPE_DIR)
			continue;

		/* we crashed while a slot was being setup or deleted, clean up
		 *
		 * 在槽正在建立或删除时发生崩溃，进行清理
		 */
		if (pg_str_endswith(replication_de->d_name, ".tmp"))
		{
			if (!rmtree(path, true))
			{
				ereport(WARNING,
						(errmsg("could not remove directory \"%s\"",
								path)));
				continue;
			}
			fsync_fname(PG_REPLSLOT_DIR, true);
			continue;
		}

		/* looks like a slot in a normal state, restore
		 *
		 * 看起来是处于正常状态的槽，进行恢复
		 */
		RestoreSlotFromDisk(replication_de->d_name);
	}
	FreeDir(replication_dir);

	/* currently no slots exist, we're done.
	 *
	 * 当前不存在槽，结束。
	 */
	if (max_replication_slots <= 0)
		return;

	/* Now that we have recovered all the data, compute replication xmin
	 *
	 * 数据都已恢复，现在计算复制 xmin
	 */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();
}

/* ----
 * Manipulation of on-disk state of replication slots
 *
 * 复制槽磁盘状态的操作
 *
 * NB: none of the routines below should take any notice whether a slot is the
 * current one or not, that's all handled a layer above.
 *
 * 注意：下面的例程都不应关心某个槽是不是当前槽，那一层在上面处理。
 * ----
 */
static void
CreateSlotOnDisk(ReplicationSlot *slot)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	struct stat st;

	/*
	 * No need to take out the io_in_progress_lock, nobody else can see this
	 * slot yet, so nobody else will write. We're reusing SaveSlotToPath which
	 * takes out the lock, if we'd take the lock here, we'd deadlock.
	 *
	 * 不必获取 io_in_progress_lock，还没有别人能看见这个槽，
	 * 因此不会有别人写它。我们复用会获取该锁的 SaveSlotToPath；
	 * 若这里再获取，就会死锁。
	 */

	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, NameStr(slot->data.name));
	sprintf(tmppath, "%s/%s.tmp", PG_REPLSLOT_DIR, NameStr(slot->data.name));

	/*
	 * It's just barely possible that some previous effort to create or drop a
	 * slot with this name left a temp directory lying around. If that seems
	 * to be the case, try to remove it.  If the rmtree() fails, we'll error
	 * out at the MakePGDirectory() below, so we don't bother checking
	 * success.
	 *
	 * 极小的可能是，先前创建或删除同名槽时留下了临时目录。
	 * 若看起来是这种情况，尝试删掉它。若 rmtree 失败，下面的
	 * MakePGDirectory 会报错，因此这里不检查是否成功。
	 */
	if (stat(tmppath, &st) == 0 && S_ISDIR(st.st_mode))
		rmtree(tmppath, true);

	/* Create and fsync the temporary slot directory.
	 *
	 * 创建临时槽目录并 fsync。
	 */
	if (MakePGDirectory(tmppath) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						tmppath)));
	fsync_fname(tmppath, true);

	/* Write the actual state file.
	 *
	 * 写入实际的状态文件。
	 */
	slot->dirty = true;			/* signal that we really need to write
	 *
	 * 表明确实需要写入
	 */
	SaveSlotToPath(slot, tmppath, ERROR);

	/* Rename the directory into place.
	 *
	 * 把目录重命名到最终位置。
	 */
	if (rename(tmppath, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));

	/*
	 * If we'd now fail - really unlikely - we wouldn't know whether this slot
	 * would persist after an OS crash or not - so, force a restart. The
	 * restart would try to fsync this again till it works.
	 *
	 * 若此时失败，非常少见，就无法知道操作系统崩溃后该槽是否还会存在，
	 * 因此强制重启。重启后会再次尝试 fsync，直到成功。
	 */
	START_CRIT_SECTION();

	fsync_fname(path, true);
	fsync_fname(PG_REPLSLOT_DIR, true);

	END_CRIT_SECTION();
}

/*
 * Shared functionality between saving and creating a replication slot.
 *
 * 保存与创建复制槽之间的共用功能。
 */
static void
SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	int			fd;
	ReplicationSlotOnDisk cp;
	bool		was_dirty;

	/* first check whether there's something to write out
	 *
	 * 先检查是否有内容需要写出
	 */
	SpinLockAcquire(&slot->mutex);
	was_dirty = slot->dirty;
	slot->just_dirtied = false;
	SpinLockRelease(&slot->mutex);

	/* and don't do anything if there's nothing to write
	 *
	 * 若没有要写的内容则什么也不做
	 */
	if (!was_dirty)
		return;

	LWLockAcquire(&slot->io_in_progress_lock, LW_EXCLUSIVE);

	/* silence valgrind :(
	 *
	 * 让 valgrind 保持安静
	 */
	memset(&cp, 0, sizeof(ReplicationSlotOnDisk));

	sprintf(tmppath, "%s/state.tmp", dir);
	sprintf(path, "%s/state", dir);

	fd = OpenTransientFile(tmppath, O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (fd < 0)
	{
		/*
		 * If not an ERROR, then release the lock before returning.  In case
		 * of an ERROR, the error recovery path automatically releases the
		 * lock, but no harm in explicitly releasing even in that case.  Note
		 * that LWLockRelease() could affect errno.
		 *
		 * 若不是 ERROR，则返回前释放锁。若是 ERROR，错误恢复路径会自动释放锁，
		 * 但即使那样显式释放也无妨。注意 LWLockRelease 可能影响 errno。
		 */
		int			save_errno = errno;

		LWLockRelease(&slot->io_in_progress_lock);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						tmppath)));
		return;
	}

	cp.magic = SLOT_MAGIC;
	INIT_CRC32C(cp.checksum);
	cp.version = SLOT_VERSION;
	cp.length = ReplicationSlotOnDiskV2Size;

	SpinLockAcquire(&slot->mutex);

	memcpy(&cp.slotdata, &slot->data, sizeof(ReplicationSlotPersistentData));

	SpinLockRelease(&slot->mutex);

	COMP_CRC32C(cp.checksum,
				(char *) (&cp) + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(cp.checksum);

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_WRITE);
	if ((write(fd, &cp, sizeof(cp))) != sizeof(cp))
	{
		int			save_errno = errno;

		pgstat_report_wait_end();
		CloseTransientFile(fd);
		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		/* if write didn't set errno, assume problem is no disk space
		 *
		 * 若 write 没有设置 errno，则假定问题是磁盘空间不足
		 */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
		return;
	}
	pgstat_report_wait_end();

	/* fsync the temporary file
	 *
	 * 对临时文件做 fsync
	 */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		pgstat_report_wait_end();
		CloseTransientFile(fd);
		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						tmppath)));
		return;
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
	{
		int			save_errno = errno;

		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						tmppath)));
		return;
	}

	/* rename to permanent file, fsync file and directory
	 *
	 * 重命名为永久文件，并对文件和目录做 fsync
	 */
	if (rename(tmppath, path) != 0)
	{
		int			save_errno = errno;

		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));
		return;
	}

	/*
	 * Check CreateSlotOnDisk() for the reasoning of using a critical section.
	 *
	 * 使用临界区的理由见 CreateSlotOnDisk。
	 */
	START_CRIT_SECTION();

	fsync_fname(path, false);
	fsync_fname(dir, true);
	fsync_fname(PG_REPLSLOT_DIR, true);

	END_CRIT_SECTION();

	/*
	 * Successfully wrote, unset dirty bit, unless somebody dirtied again
	 * already and remember the confirmed_flush LSN value.
	 *
	 * 写入成功后清除 dirty 位，除非已经又被别人标脏，并记住
	 * confirmed_flush LSN。
	 */
	SpinLockAcquire(&slot->mutex);
	if (!slot->just_dirtied)
		slot->dirty = false;
	slot->last_saved_confirmed_flush = cp.slotdata.confirmed_flush;
	slot->last_saved_restart_lsn = cp.slotdata.restart_lsn;
	SpinLockRelease(&slot->mutex);

	LWLockRelease(&slot->io_in_progress_lock);
}

/*
 * Load a single slot from disk into memory.
 *
 * 把单个槽从磁盘装入内存。
 */
static void
RestoreSlotFromDisk(const char *name)
{
	ReplicationSlotOnDisk cp;
	int			i;
	char		slotdir[MAXPGPATH + sizeof(PG_REPLSLOT_DIR)];
	char		path[MAXPGPATH + sizeof(PG_REPLSLOT_DIR) + 10];
	int			fd;
	bool		restored = false;
	int			readBytes;
	pg_crc32c	checksum;
	TimestampTz now = 0;

	/* no need to lock here, no concurrent access allowed yet
	 *
	 * 这里不需要加锁，此时不允许并发访问
	 */

	/* delete temp file if it exists
	 *
	 * 若存在临时文件则删除
	 */
	sprintf(slotdir, "%s/%s", PG_REPLSLOT_DIR, name);
	sprintf(path, "%s/state.tmp", slotdir);
	if (unlink(path) < 0 && errno != ENOENT)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));

	sprintf(path, "%s/state", slotdir);

	elog(DEBUG1, "restoring replication slot from \"%s\"", path);

	/* on some operating systems fsyncing a file requires O_RDWR
	 *
	 * 在某些操作系统上，对文件做 fsync 需要 O_RDWR
	 */
	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);

	/*
	 * We do not need to handle this as we are rename()ing the directory into
	 * place only after we fsync()ed the state file.
	 *
	 * 不需要处理这种情况，因为我们只在对状态文件做完 fsync 之后才用
	 * rename 把目录放到最终位置。
	 */
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/*
	 * Sync state file before we're reading from it. We might have crashed
	 * while it wasn't synced yet and we shouldn't continue on that basis.
	 *
	 * 读取状态文件之前先同步它。可能在它尚未同步时发生了崩溃，
	 * 不应在此基础上继续。
	 */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_RESTORE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						path)));
	pgstat_report_wait_end();

	/* Also sync the parent directory
	 *
	 * 同时同步父目录
	 */
	START_CRIT_SECTION();
	fsync_fname(slotdir, true);
	END_CRIT_SECTION();

	/* read part of statefile that's guaranteed to be version independent
	 *
	 * 读取状态文件中保证与版本无关的部分
	 */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_READ);
	readBytes = read(fd, &cp, ReplicationSlotOnDiskConstantSize);
	pgstat_report_wait_end();
	if (readBytes != ReplicationSlotOnDiskConstantSize)
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes,
							(Size) ReplicationSlotOnDiskConstantSize)));
	}

	/* verify magic
	 *
	 * 校验 magic
	 */
	if (cp.magic != SLOT_MAGIC)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has wrong magic number: %u instead of %u",
						path, cp.magic, SLOT_MAGIC)));

	/* verify version
	 *
	 * 校验版本
	 */
	if (cp.version != SLOT_VERSION)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has unsupported version %u",
						path, cp.version)));

	/* boundary check on length
	 *
	 * 对长度做边界检查
	 */
	if (cp.length != ReplicationSlotOnDiskV2Size)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has corrupted length %u",
						path, cp.length)));

	/* Now that we know the size, read the entire file
	 *
	 * 既然已经知道大小，读入整个文件
	 */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_READ);
	readBytes = read(fd,
					 (char *) &cp + ReplicationSlotOnDiskConstantSize,
					 cp.length);
	pgstat_report_wait_end();
	if (readBytes != cp.length)
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, (Size) cp.length)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/* now verify the CRC
	 *
	 * 现在校验 CRC
	 */
	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				(char *) &cp + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(checksum);

	if (!EQ_CRC32C(checksum, cp.checksum))
		ereport(PANIC,
				(errmsg("checksum mismatch for replication slot file \"%s\": is %u, should be %u",
						path, checksum, cp.checksum)));

	/*
	 * If we crashed with an ephemeral slot active, don't restore but delete
	 * it.
	 *
	 * 若崩溃时有活动的短暂槽，则不恢复而是删除它。
	 */
	if (cp.slotdata.persistency != RS_PERSISTENT)
	{
		if (!rmtree(slotdir, true))
		{
			ereport(WARNING,
					(errmsg("could not remove directory \"%s\"",
							slotdir)));
		}
		fsync_fname(PG_REPLSLOT_DIR, true);
		return;
	}

	/*
	 * Verify that requirements for the specific slot type are met. That's
	 * important because if these aren't met we're not guaranteed to retain
	 * all the necessary resources for the slot.
	 *
	 * 验证该槽类型的要求是否满足。这很重要，因为若不满足，
	 * 就不能保证保留该槽所需的全部资源。
	 *
	 * NB: We have to do so *after* the above checks for ephemeral slots,
	 * because otherwise a slot that shouldn't exist anymore could prevent
	 * restarts.
	 *
	 * 注意：必须在上面针对短暂槽的检查之后再做，
	 * 否则一个本不该存在的槽可能阻止重启。
	 *
	 * NB: Changing the requirements here also requires adapting
	 * CheckSlotRequirements() and CheckLogicalDecodingRequirements().
	 *
	 * 注意：这里改变要求时，也必须相应调整 CheckSlotRequirements 与
	 * CheckLogicalDecodingRequirements。
	 */
	if (cp.slotdata.database != InvalidOid)
	{
		if (wal_level < WAL_LEVEL_LOGICAL)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication slot \"%s\" exists, but \"wal_level\" < \"logical\"",
							NameStr(cp.slotdata.name)),
					 errhint("Change \"wal_level\" to be \"logical\" or higher.")));

		/*
		 * In standby mode, the hot standby must be enabled. This check is
		 * necessary to ensure logical slots are invalidated when they become
		 * incompatible due to insufficient wal_level. Otherwise, if the
		 * primary reduces wal_level < logical while hot standby is disabled,
		 * logical slots would remain valid even after promotion.
		 *
		 * 在备库模式下必须启用热备。这项检查是为了保证逻辑槽在因 wal_level
		 * 不足而不再兼容时被作废。否则，若主库在热备关闭时把 wal_level 降到
		 * logical 以下，逻辑槽在提升之后仍会保持有效。
		 */
		if (StandbyMode && !EnableHotStandby)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication slot \"%s\" exists on the standby, but \"hot_standby\" = \"off\"",
							NameStr(cp.slotdata.name)),
					 errhint("Change \"hot_standby\" to be \"on\".")));
	}
	else if (wal_level < WAL_LEVEL_REPLICA)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("physical replication slot \"%s\" exists, but \"wal_level\" < \"replica\"",
						NameStr(cp.slotdata.name)),
				 errhint("Change \"wal_level\" to be \"replica\" or higher.")));

	/* nothing can be active yet, don't lock anything
	 *
	 * 此时不可能有活动槽，不必加锁
	 */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot;

		slot = &ReplicationSlotCtl->replication_slots[i];

		if (slot->in_use)
			continue;

		/* restore the entire set of persistent data
		 *
		 * 恢复全部持久化数据
		 */
		memcpy(&slot->data, &cp.slotdata,
			   sizeof(ReplicationSlotPersistentData));

		/* initialize in memory state
		 *
		 * 初始化内存状态
		 */
		slot->effective_xmin = cp.slotdata.xmin;
		slot->effective_catalog_xmin = cp.slotdata.catalog_xmin;
		slot->last_saved_confirmed_flush = cp.slotdata.confirmed_flush;
		slot->last_saved_restart_lsn = cp.slotdata.restart_lsn;

		slot->candidate_catalog_xmin = InvalidTransactionId;
		slot->candidate_xmin_lsn = InvalidXLogRecPtr;
		slot->candidate_restart_lsn = InvalidXLogRecPtr;
		slot->candidate_restart_valid = InvalidXLogRecPtr;

		slot->in_use = true;
		slot->active_pid = 0;

		/*
		 * Set the time since the slot has become inactive after loading the
		 * slot from the disk into memory. Whoever acquires the slot i.e.
		 * makes the slot active will reset it. Use the same inactive_since
		 * time for all the slots.
		 *
		 * 把槽从磁盘装入内存后，设置它变为非活动后的时刻。谁占用该槽、
		 * 即使它变为活动，谁就会重置该时刻。所有槽使用同一个 inactive_since
		 * 时间。
		 */
		if (now == 0)
			now = GetCurrentTimestamp();

		ReplicationSlotSetInactiveSince(slot, now, false);

		restored = true;
		break;
	}

	if (!restored)
		ereport(FATAL,
				(errmsg("too many replication slots active before shutdown"),
				 errhint("Increase \"max_replication_slots\" and try again.")));
}

/*
 * Maps an invalidation reason for a replication slot to
 * ReplicationSlotInvalidationCause.
 *
 * 把复制槽的作废原因字符串映射为 ReplicationSlotInvalidationCause。
 */
ReplicationSlotInvalidationCause
GetSlotInvalidationCause(const char *cause_name)
{
	Assert(cause_name);

	/* Search lookup table for the cause having this name
	 *
	 * 在查找表中搜索具有该名称的原因
	 */
	for (int i = 0; i <= RS_INVAL_MAX_CAUSES; i++)
	{
		if (strcmp(SlotInvalidationCauses[i].cause_name, cause_name) == 0)
			return SlotInvalidationCauses[i].cause;
	}

	Assert(false);
	return RS_INVAL_NONE;		/* to keep compiler quiet
	 *
	 * 避免编译器告警
	 */
}

/*
 * Maps an ReplicationSlotInvalidationCause to the invalidation
 * reason for a replication slot.
 *
 * 把 ReplicationSlotInvalidationCause 映射为复制槽的作废原因字符串。
 */
const char *
GetSlotInvalidationCauseName(ReplicationSlotInvalidationCause cause)
{
	/* Search lookup table for the name of this cause
	 *
	 * 在查找表中搜索该原因的名称
	 */
	for (int i = 0; i <= RS_INVAL_MAX_CAUSES; i++)
	{
		if (SlotInvalidationCauses[i].cause == cause)
			return SlotInvalidationCauses[i].cause_name;
	}

	Assert(false);
	return "none";				/* to keep compiler quiet
	 *
	 * 避免编译器告警
	 */
}

/*
 * A helper function to validate slots specified in GUC synchronized_standby_slots.
 *
 * 用于校验 GUC synchronized_standby_slots 中指定槽的辅助函数。
 *
 * The rawname will be parsed, and the result will be saved into *elemlist.
 *
 * rawname 将被解析，结果保存到 elemlist。
 */
static bool
validate_sync_standby_slots(char *rawname, List **elemlist)
{
	/* Verify syntax and parse string into a list of identifiers
	 *
	 * 校验语法，并把字符串解析为标识符列表
	 */
	if (!SplitIdentifierString(rawname, ',', elemlist))
	{
		GUC_check_errdetail("List syntax is invalid.");
		return false;
	}

	/* Iterate the list to validate each slot name
	 *
	 * 遍历列表以校验每个槽名
	 */
	foreach_ptr(char, name, *elemlist)
	{
		int			err_code;
		char	   *err_msg = NULL;
		char	   *err_hint = NULL;

		if (!ReplicationSlotValidateNameInternal(name, &err_code, &err_msg,
												 &err_hint))
		{
			GUC_check_errcode(err_code);
			GUC_check_errdetail("%s", err_msg);
			if (err_hint != NULL)
				GUC_check_errhint("%s", err_hint);
			return false;
		}
	}

	return true;
}

/*
 * GUC check_hook for synchronized_standby_slots
 *
 * synchronized_standby_slots 的 GUC check_hook
 */
bool
check_synchronized_standby_slots(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	char	   *ptr;
	List	   *elemlist;
	int			size;
	bool		ok;
	SyncStandbySlotsConfigData *config;

	if ((*newval)[0] == '\0')
		return true;

	/* Need a modifiable copy of the GUC string
	 *
	 * 需要一份可修改的 GUC 字符串副本
	 */
	rawname = pstrdup(*newval);

	/* Now verify if the specified slots exist and have correct type
	 *
	 * 现在验证指定的槽是否存在且类型正确
	 */
	ok = validate_sync_standby_slots(rawname, &elemlist);

	if (!ok || elemlist == NIL)
	{
		pfree(rawname);
		list_free(elemlist);
		return ok;
	}

	/* Compute the size required for the SyncStandbySlotsConfigData struct
	 *
	 * 计算 SyncStandbySlotsConfigData 结构所需的大小
	 */
	size = offsetof(SyncStandbySlotsConfigData, slot_names);
	foreach_ptr(char, slot_name, elemlist)
		size += strlen(slot_name) + 1;

	/* GUC extra value must be guc_malloc'd, not palloc'd
	 *
	 * GUC 的 extra 值必须用 guc_malloc 分配，不能用 palloc
	 */
	config = (SyncStandbySlotsConfigData *) guc_malloc(LOG, size);
	if (!config)
		return false;

	/* Transform the data into SyncStandbySlotsConfigData
	 *
	 * 把数据转换成 SyncStandbySlotsConfigData
	 */
	config->nslotnames = list_length(elemlist);

	ptr = config->slot_names;
	foreach_ptr(char, slot_name, elemlist)
	{
		strcpy(ptr, slot_name);
		ptr += strlen(slot_name) + 1;
	}

	*extra = config;

	pfree(rawname);
	list_free(elemlist);
	return true;
}

/*
 * GUC assign_hook for synchronized_standby_slots
 *
 * synchronized_standby_slots 的 GUC assign_hook
 */
void
assign_synchronized_standby_slots(const char *newval, void *extra)
{
	/*
	 * The standby slots may have changed, so we must recompute the oldest
	 * LSN.
	 *
	 * 备库槽可能已改变，因此必须重新计算最老 LSN。
	 */
	ss_oldest_flush_lsn = InvalidXLogRecPtr;

	synchronized_standby_slots_config = (SyncStandbySlotsConfigData *) extra;
}

/*
 * Check if the passed slot_name is specified in the synchronized_standby_slots GUC.
 *
 * 检查传入的 slot_name 是否出现在 synchronized_standby_slots 这个 GUC
 * 中。
 */
bool
SlotExistsInSyncStandbySlots(const char *slot_name)
{
	const char *standby_slot_name;

	/* Return false if there is no value in synchronized_standby_slots
	 *
	 * 若 synchronized_standby_slots 没有值，则返回 false
	 */
	if (synchronized_standby_slots_config == NULL)
		return false;

	/*
	 * XXX: We are not expecting this list to be long so a linear search
	 * shouldn't hurt but if that turns out not to be true then we can cache
	 * this information for each WalSender as well.
	 *
	 * XXX：预期这个列表不会很长，线性查找应该可以接受；若事实并非如此，
	 * 也可以为每个 WalSender 缓存该信息。
	 */
	standby_slot_name = synchronized_standby_slots_config->slot_names;
	for (int i = 0; i < synchronized_standby_slots_config->nslotnames; i++)
	{
		if (strcmp(standby_slot_name, slot_name) == 0)
			return true;

		standby_slot_name += strlen(standby_slot_name) + 1;
	}

	return false;
}

/*
 * Return true if the slots specified in synchronized_standby_slots have caught up to
 * the given WAL location, false otherwise.
 *
 * 若 synchronized_standby_slots 中指定的槽都已追上给定 WAL 位置则返回
 * true，否则返回 false。
 *
 * The elevel parameter specifies the error level used for logging messages
 * related to slots that do not exist, are invalidated, or are inactive.
 *
 * elevel 参数指定用于记录不存在、已作废或非活动槽相关消息的错误级别。
 */
bool
StandbySlotsHaveCaughtup(XLogRecPtr wait_for_lsn, int elevel)
{
	const char *name;
	int			caught_up_slot_num = 0;
	XLogRecPtr	min_restart_lsn = InvalidXLogRecPtr;

	/*
	 * Don't need to wait for the standbys to catch up if there is no value in
	 * synchronized_standby_slots.
	 *
	 * 若 synchronized_standby_slots 没有值，则不必等待备库追上。
	 */
	if (synchronized_standby_slots_config == NULL)
		return true;

	/*
	 * Don't need to wait for the standbys to catch up if we are on a standby
	 * server, since we do not support syncing slots to cascading standbys.
	 *
	 * 若我们在备库上，则不必等待备库追上，因为不支持把槽同步到级联备库。
	 */
	if (RecoveryInProgress())
		return true;

	/*
	 * Don't need to wait for the standbys to catch up if they are already
	 * beyond the specified WAL location.
	 *
	 * 若备库已经超过指定的 WAL 位置，则不必等待它们追上。
	 */
	if (!XLogRecPtrIsInvalid(ss_oldest_flush_lsn) &&
		ss_oldest_flush_lsn >= wait_for_lsn)
		return true;

	/*
	 * To prevent concurrent slot dropping and creation while filtering the
	 * slots, take the ReplicationSlotControlLock outside of the loop.
	 *
	 * 为防止过滤槽期间并发删除和创建槽，在循环外获取
	 * ReplicationSlotControlLock。
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	name = synchronized_standby_slots_config->slot_names;
	for (int i = 0; i < synchronized_standby_slots_config->nslotnames; i++)
	{
		XLogRecPtr	restart_lsn;
		bool		invalidated;
		bool		inactive;
		ReplicationSlot *slot;

		slot = SearchNamedReplicationSlot(name, false);

		/*
		 * If a slot name provided in synchronized_standby_slots does not
		 * exist, report a message and exit the loop.
		 *
		 * 若 synchronized_standby_slots 中给出的槽名不存在，
		 * 则报告一条消息并退出循环。
		 */
		if (!slot)
		{
			ereport(elevel,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("replication slot \"%s\" specified in parameter \"%s\" does not exist",
						   name, "synchronized_standby_slots"),
					errdetail("Logical replication is waiting on the standby associated with replication slot \"%s\".",
							  name),
					errhint("Create the replication slot \"%s\" or amend parameter \"%s\".",
							name, "synchronized_standby_slots"));
			break;
		}

		/* Same as above: if a slot is not physical, exit the loop.
		 *
		 * 与上面相同：若槽不是物理槽，则退出循环。
		 */
		if (SlotIsLogical(slot))
		{
			ereport(elevel,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("cannot specify logical replication slot \"%s\" in parameter \"%s\"",
						   name, "synchronized_standby_slots"),
					errdetail("Logical replication is waiting for correction on replication slot \"%s\".",
							  name),
					errhint("Remove the logical replication slot \"%s\" from parameter \"%s\".",
							name, "synchronized_standby_slots"));
			break;
		}

		SpinLockAcquire(&slot->mutex);
		restart_lsn = slot->data.restart_lsn;
		invalidated = slot->data.invalidated != RS_INVAL_NONE;
		inactive = slot->active_pid == 0;
		SpinLockRelease(&slot->mutex);

		if (invalidated)
		{
			/* Specified physical slot has been invalidated
			 *
			 * 指定的物理槽已被作废
			 */
			ereport(elevel,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("physical replication slot \"%s\" specified in parameter \"%s\" has been invalidated",
						   name, "synchronized_standby_slots"),
					errdetail("Logical replication is waiting on the standby associated with replication slot \"%s\".",
							  name),
					errhint("Drop and recreate the replication slot \"%s\", or amend parameter \"%s\".",
							name, "synchronized_standby_slots"));
			break;
		}

		if (XLogRecPtrIsInvalid(restart_lsn) || restart_lsn < wait_for_lsn)
		{
			/* Log a message if no active_pid for this physical slot
			 *
			 * 若该物理槽没有 active_pid，则记一条日志
			 */
			if (inactive)
				ereport(elevel,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("replication slot \"%s\" specified in parameter \"%s\" does not have active_pid",
							   name, "synchronized_standby_slots"),
						errdetail("Logical replication is waiting on the standby associated with replication slot \"%s\".",
								  name),
						errhint("Start the standby associated with the replication slot \"%s\", or amend parameter \"%s\".",
								name, "synchronized_standby_slots"));

			/* Continue if the current slot hasn't caught up.
			 *
			 * 若当前槽尚未追上，则继续。
			 */
			break;
		}

		Assert(restart_lsn >= wait_for_lsn);

		if (XLogRecPtrIsInvalid(min_restart_lsn) ||
			min_restart_lsn > restart_lsn)
			min_restart_lsn = restart_lsn;

		caught_up_slot_num++;

		name += strlen(name) + 1;
	}

	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * Return false if not all the standbys have caught up to the specified
	 * WAL location.
	 *
	 * 若并非所有备库都已追上指定的 WAL 位置，则返回 false。
	 */
	if (caught_up_slot_num != synchronized_standby_slots_config->nslotnames)
		return false;

	/* The ss_oldest_flush_lsn must not retreat.
	 *
	 * ss_oldest_flush_lsn 不得后退。
	 */
	Assert(XLogRecPtrIsInvalid(ss_oldest_flush_lsn) ||
		   min_restart_lsn >= ss_oldest_flush_lsn);

	ss_oldest_flush_lsn = min_restart_lsn;

	return true;
}

/*
 * Wait for physical standbys to confirm receiving the given lsn.
 *
 * 等待物理备库确认已收到给定的 lsn。
 *
 * Used by logical decoding SQL functions. It waits for physical standbys
 * corresponding to the physical slots specified in the synchronized_standby_slots GUC.
 *
 * 供逻辑解码 SQL 函数使用。它等待 synchronized_standby_slots 这个 GUC
 * 所指定的物理槽对应的物理备库。
 */
void
WaitForStandbyConfirmation(XLogRecPtr wait_for_lsn)
{
	/*
	 * Don't need to wait for the standby to catch up if the current acquired
	 * slot is not a logical failover slot, or there is no value in
	 * synchronized_standby_slots.
	 *
	 * 若当前占用的槽不是逻辑 failover 槽，或 synchronized_standby_slots
	 * 没有值，则不必等待备库追上。
	 */
	if (!MyReplicationSlot->data.failover || !synchronized_standby_slots_config)
		return;

	ConditionVariablePrepareToSleep(&WalSndCtl->wal_confirm_rcv_cv);

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Exit if done waiting for every slot.
		 *
		 * 若每个槽都已等待完成，则退出。
		 */
		if (StandbySlotsHaveCaughtup(wait_for_lsn, WARNING))
			break;

		/*
		 * Wait for the slots in the synchronized_standby_slots to catch up,
		 * but use a timeout (1s) so we can also check if the
		 * synchronized_standby_slots has been changed.
		 *
		 * 等待 synchronized_standby_slots 中的槽追上，但使用 1 秒超时，
		 * 以便同时检查 synchronized_standby_slots 是否已改变。
		 */
		ConditionVariableTimedSleep(&WalSndCtl->wal_confirm_rcv_cv, 1000,
									WAIT_EVENT_WAIT_FOR_STANDBY_CONFIRMATION);
	}

	ConditionVariableCancelSleep();
}
