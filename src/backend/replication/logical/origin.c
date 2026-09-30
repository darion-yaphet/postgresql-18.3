/*-------------------------------------------------------------------------
 *
 * origin.c
 *	  Logical replication progress tracking support.
 *
 * 逻辑复制进度跟踪支持。
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/origin.c
 *
 * NOTES
 *
 * 说明
 *
 * This file provides the following:
 * * An infrastructure to name nodes in a replication setup
 * * A facility to efficiently store and persist replication progress in an
 *	 efficient and durable manner.
 *
 * 本文件提供：一套为复制拓扑中的节点命名的基础设施；以及一种高效、持久
 * 地保存复制进度的机制。
 *
 * Replication origin consist out of a descriptive, user defined, external
 * name and a short, thus space efficient, internal 2 byte one. This split
 * exists because replication origin have to be stored in WAL and shared
 * memory and long descriptors would be inefficient.  For now only use 2 bytes
 * for the internal id of a replication origin as it seems unlikely that there
 * soon will be more than 65k nodes in one replication setup; and using only
 * two bytes allow us to be more space efficient.
 *
 * 复制源由用户定义的外部描述名，以及一个短的、因此更省空间的内部 2 字
 * 节标识组成。之所以拆开，是因为复制源要写入 WAL 和共享内存，长描述名
 * 效率太低。目前内部 id 只用 2 字节，因为一个复制拓扑里短期内不太可能
 * 超过 65k 个节点；只用两字节也更省空间。
 *
 * Replication progress is tracked in a shared memory table
 * (ReplicationState) that's dumped to disk every checkpoint. Entries
 * ('slots') in this table are identified by the internal id. That's the case
 * because it allows to increase replication progress during crash
 * recovery. To allow doing so we store the original LSN (from the originating
 * system) of a transaction in the commit record. That allows to recover the
 * precise replayed state after crash recovery; without requiring synchronous
 * commits. Allowing logical replication to use asynchronous commit is
 * generally good for performance, but especially important as it allows a
 * single threaded replay process to keep up with a source that has multiple
 * backends generating changes concurrently.  For efficiency and simplicity
 * reasons a backend can setup one replication origin that's from then used as
 * the source of changes produced by the backend, until reset again.
 *
 * 复制进度记录在共享内存表 ReplicationState 中，每次检查点都转储到磁盘。
 * 表中的项（slot）用内部 id 标识，这样才能在崩溃恢复期间推进复制进度。
 * 为此，把事务在源系统上的原始 LSN 记在提交记录里。于是崩溃恢复后可以
 * 精确恢复已重放的状态，而不必要求同步提交。允许逻辑复制使用异步提交通
 * 常有利于性能，尤其重要的是，这样单线程重放进程才能跟上有多个后端并发
 * 生成变更的源端。出于效率和简单，一个后端可以设置一个复制源，此后它产
 * 生的变更都算作来自该源，直到再次重置。
 *
 * This infrastructure is intended to be used in cooperation with logical
 * decoding. When replaying from a remote system the configured origin is
 * provided to output plugins, allowing prevention of replication loops and
 * other filtering.
 *
 * 这套机制打算与逻辑解码配合使用。从远程系统重放时，把配置的 origin 交
 * 给输出插件，从而可以防止复制回环并做其他过滤。
 *
 * There are several levels of locking at work:
 *
 * 这里有好几层锁：
 *
 * * To create and drop replication origins an exclusive lock on
 *	 pg_replication_slot is required for the duration. That allows us to
 *	 safely and conflict free assign new origins using a dirty snapshot.
 *
 * 创建和删除复制源期间，需要对 pg_replication_slot 持有排他锁。这样就
 * 能用脏快照安全、无冲突地分配新的 origin。
 *
 * * When creating an in-memory replication progress slot the ReplicationOrigin
 *	 LWLock has to be held exclusively; when iterating over the replication
 *	 progress a shared lock has to be held, the same when advancing the
 *	 replication progress of an individual backend that has not setup as the
 *	 session's replication origin.
 *
 * 创建内存中的复制进度 slot 时，必须排他持有 ReplicationOrigin 的
 * LWLock；遍历复制进度时持有共享锁。推进某个尚未把该 origin 设为会话复
 * 制源的单个后端的进度时，也同样持有共享锁。
 *
 * * When manipulating or looking at the remote_lsn and local_lsn fields of a
 *	 replication progress slot that slot's lwlock has to be held. That's
 *	 primarily because we do not assume 8 byte writes (the LSN) is atomic on
 *	 all our platforms, but it also simplifies memory ordering concerns
 *	 between the remote and local lsn. We use a lwlock instead of a spinlock
 *	 so it's less harmful to hold the lock over a WAL write
 *	 (cf. AdvanceReplicationProgress).
 *
 * 查看或修改某个复制进度 slot 的 remote_lsn 和 local_lsn 时，必须持有
 * 该 slot 的 lwlock。主要是因为不能假定在所有平台上 8 字节的 LSN 写入
 * 都是原子的，同时这也简化了 remote 与 local lsn 之间的内存序问题。这
 * 里用 lwlock 而不是 spinlock，这样在写 WAL 期间持锁代价更小（参见
 * AdvanceReplicationProgress）。
 *
 * ---------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 核心流程：
 * 复制源登记在 pg_replication_origin 中，外部名称对应 16 位 roident。
 * 重放进度放在共享内存 ReplicationState：replorigin_advance 与
 * replorigin_session_advance 推进 remote_lsn 和 local_lsn。
 * CheckPointReplicationOrigin 在检查点时把进度落盘，启动时再读回。
 * 崩溃恢复由 replorigin_redo 重放 XLOG_REPLORIGIN_SET 与 XLOG_REPLORIGIN_DROP。
 * 会话经 replorigin_session_setup 绑定 origin，退出时由 replorigin_session_reset 拆除。
 */

/* paths for replication origin checkpoint files
 *
 * 复制源检查点文件的路径
 */
#define PG_REPLORIGIN_CHECKPOINT_FILENAME PG_LOGICAL_DIR "/replorigin_checkpoint"
#define PG_REPLORIGIN_CHECKPOINT_TMPFILE PG_REPLORIGIN_CHECKPOINT_FILENAME ".tmp"

/* GUC variables
 *
 * GUC 变量
 */
int			max_active_replication_origins = 10;

/*
 * Replay progress of a single remote node.
 *
 * 单个远程节点的重放进度。
 */
typedef struct ReplicationState
{
	/*
	 * Local identifier for the remote node.
	 *
	 * 远程节点的本地标识。
	 */
	RepOriginId roident;

	/*
	 * Location of the latest commit from the remote side.
	 *
	 * 远程侧最近一次提交的位置。
	 */
	XLogRecPtr	remote_lsn;

	/*
	 * Remember the local lsn of the commit record so we can XLogFlush() to it
	 * during a checkpoint so we know the commit record actually is safe on
	 * disk.
	 *
	 * 记住提交记录的本地 LSN，以便在检查点时对它做 XLogFlush()，从而确认提
	 * 交记录确实已经安全落盘。
	 */
	XLogRecPtr	local_lsn;

	/*
	 * PID of backend that's acquired slot, or 0 if none.
	 *
	 * 已占用该 slot 的后端 PID；无人占用则为 0。
	 */
	int			acquired_by;

	/*
	 * Condition variable that's signaled when acquired_by changes.
	 *
	 * acquired_by 变化时被唤醒的条件变量。
	 */
	ConditionVariable origin_cv;

	/*
	 * Lock protecting remote_lsn and local_lsn.
	 *
	 * 保护 remote_lsn 和 local_lsn 的锁。
	 */
	LWLock		lock;
} ReplicationState;

/*
 * On disk version of ReplicationState.
 *
 * ReplicationState 的磁盘版本。
 */
typedef struct ReplicationStateOnDisk
{
	RepOriginId roident;
	XLogRecPtr	remote_lsn;
} ReplicationStateOnDisk;


typedef struct ReplicationStateCtl
{
	/* Tranche to use for per-origin LWLocks
	 *
	 * 供每个 origin 的 LWLock 使用的 tranche
	 */
	int			tranche_id;
	/* Array of length max_active_replication_origins
	 *
	 * 长度为 max_active_replication_origins 的数组
	 */
	ReplicationState states[FLEXIBLE_ARRAY_MEMBER];
} ReplicationStateCtl;

/* external variables
 *
 * 外部变量
 */
RepOriginId replorigin_session_origin = InvalidRepOriginId; /* assumed identity
															 *
															 * 假定的身份
															 */
XLogRecPtr	replorigin_session_origin_lsn = InvalidXLogRecPtr;
TimestampTz replorigin_session_origin_timestamp = 0;

/*
 * Base address into a shared memory array of replication states of size
 * max_active_replication_origins.
 *
 * 指向共享内存中复制状态数组的基地址，数组大小为
 * max_active_replication_origins。
 */
static ReplicationState *replication_states;

/*
 * Actual shared memory block (replication_states[] is now part of this).
 *
 * 实际的共享内存块（replication_states[] 现在是它的一部分）。
 */
static ReplicationStateCtl *replication_states_ctl;

/*
 * We keep a pointer to this backend's ReplicationState to avoid having to
 * search the replication_states array in replorigin_session_advance for each
 * remote commit.  (Ownership of a backend's own entry can only be changed by
 * that backend.)
 *
 * 保存指向本后端 ReplicationState 的指针，以免在
 * replorigin_session_advance 里为每次远程提交都去搜索
 * replication_states 数组。（某个后端对自己那一项的所有权只能由该后端
 * 自己改变。）
 */
static ReplicationState *session_replication_state = NULL;

/* Magic for on disk files.
 *
 * 磁盘文件使用的魔数。
 */
#define REPLICATION_STATE_MAGIC ((uint32) 0x1257DADE)

/*
 * 检查复制源操作的前提：max_active_replication_origins 不为 0，并且在
 * 不允许恢复的情况下当前不处于恢复中。
 */
static void
replorigin_check_prerequisites(bool check_origins, bool recoveryOK)
{
	if (check_origins && max_active_replication_origins == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot query or manipulate replication origin when \"max_active_replication_origins\" is 0")));

	if (!recoveryOK && RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot manipulate replication origins during recovery")));
}


/*
 * IsReservedOriginName
 *		True iff name is either "none" or "any".
 *
 * IsReservedOriginName：当名称是 none 或 any 时为真。
 */
static bool
IsReservedOriginName(const char *name)
{
	return ((pg_strcasecmp(name, LOGICALREP_ORIGIN_NONE) == 0) ||
			(pg_strcasecmp(name, LOGICALREP_ORIGIN_ANY) == 0));
}

/* ---------------------------------------------------------------------------
 * Functions for working with replication origins themselves.
 *
 * 操作复制源本身的函数。
 * ---------------------------------------------------------------------------
 */

/*
 * Check for a persistent replication origin identified by name.
 *
 * 按名称查找持久化的复制源。
 *
 * Returns InvalidOid if the node isn't known yet and missing_ok is true.
 *
 * 若节点尚不存在且 missing_ok 为真，则返回 InvalidOid。
 */
RepOriginId
replorigin_by_name(const char *roname, bool missing_ok)
{
	Form_pg_replication_origin ident;
	Oid			roident = InvalidOid;
	HeapTuple	tuple;
	Datum		roname_d;

	roname_d = CStringGetTextDatum(roname);

	tuple = SearchSysCache1(REPLORIGNAME, roname_d);
	if (HeapTupleIsValid(tuple))
	{
		ident = (Form_pg_replication_origin) GETSTRUCT(tuple);
		roident = ident->roident;
		ReleaseSysCache(tuple);
	}
	else if (!missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication origin \"%s\" does not exist",
						roname)));

	return roident;
}

/*
 * Create a replication origin.
 *
 * 创建一个复制源。
 *
 * Needs to be called in a transaction.
 *
 * 必须在事务中调用。
 */
RepOriginId
replorigin_create(const char *roname)
{
	Oid			roident;
	HeapTuple	tuple = NULL;
	Relation	rel;
	Datum		roname_d;
	SnapshotData SnapshotDirty;
	SysScanDesc scan;
	ScanKeyData key;

	/*
	 * To avoid needing a TOAST table for pg_replication_origin, we limit
	 * replication origin names to 512 bytes.  This should be more than enough
	 * for all practical use.
	 *
	 * 为了让 pg_replication_origin 不需要 TOAST 表，把复制源名称限制在 512
	 * 字节以内。对所有实际用途来说这已经足够。
	 */
	if (strlen(roname) > MAX_RONAME_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("replication origin name is too long"),
				 errdetail("Replication origin names must be no longer than %d bytes.",
						   MAX_RONAME_LEN)));

	roname_d = CStringGetTextDatum(roname);

	Assert(IsTransactionState());

	/*
	 * We need the numeric replication origin to be 16bit wide, so we cannot
	 * rely on the normal oid allocation. Instead we simply scan
	 * pg_replication_origin for the first unused id. That's not particularly
	 * efficient, but this should be a fairly infrequent operation - we can
	 * easily spend a bit more code on this when it turns out it needs to be
	 * faster.
	 *
	 * 数值形式的复制源必须是 16 位宽，因此不能依赖普通的 oid 分配。这里改
	 * 为扫描 pg_replication_origin，找第一个未使用的 id。这不算特别高效，
	 * 但这个操作应该很少发生；如果以后证明需要更快，可以再多写一些代码。
	 *
	 * We handle concurrency by taking an exclusive lock (allowing reads!)
	 * over the table for the duration of the search. Because we use a "dirty
	 * snapshot" we can read rows that other in-progress sessions have
	 * written, even though they would be invisible with normal snapshots. Due
	 * to the exclusive lock there's no danger that new rows can appear while
	 * we're checking.
	 *
	 * 并发方面，搜索期间对表加排他锁（仍允许读）。因为使用脏快照，可以读到
	 * 其他进行中的会话已写入、但用普通快照还看不见的行。由于持有排他锁，检
	 * 查过程中不会出现新行。
	 */
	InitDirtySnapshot(SnapshotDirty);

	rel = table_open(ReplicationOriginRelationId, ExclusiveLock);

	/*
	 * We want to be able to access pg_replication_origin without setting up a
	 * snapshot.  To make that safe, it needs to not have a TOAST table, since
	 * TOASTed data cannot be fetched without a snapshot.  As of this writing,
	 * its only varlena column is roname, which we limit to 512 bytes to avoid
	 * needing out-of-line storage.  If you add a TOAST table to this catalog,
	 * be sure to set up a snapshot everywhere it might be needed.  For more
	 * information, see https://postgr.es/m/ZvMSUPOqUU-VNADN%40nathan.
	 *
	 * 希望在不建立快照的情况下访问 pg_replication_origin。为了安全，它不能
	 * 有 TOAST 表，因为没有快照就取不到 TOAST 数据。截至目前，它唯一的
	 * varlena 列是 roname，我们把它限制在 512 字节，以免需要行外存储。如果
	 * 给这个目录加了 TOAST 表，务必在所有可能用到的地方建立快照。更多说明
	 * 见 https://postgr.es/m/ZvMSUPOqUU-VNADN%40nathan。
	 */
	Assert(!OidIsValid(rel->rd_rel->reltoastrelid));

	for (roident = InvalidOid + 1; roident < PG_UINT16_MAX; roident++)
	{
		bool		nulls[Natts_pg_replication_origin];
		Datum		values[Natts_pg_replication_origin];
		bool		collides;

		CHECK_FOR_INTERRUPTS();

		ScanKeyInit(&key,
					Anum_pg_replication_origin_roident,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roident));

		scan = systable_beginscan(rel, ReplicationOriginIdentIndex,
								  true /* indexOK
									    *
									    * 可以使用索引
									    */ ,
								  &SnapshotDirty,
								  1, &key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		if (!collides)
		{
			/*
			 * Ok, found an unused roident, insert the new row and do a CCI,
			 * so our callers can look it up if they want to.
			 *
			 * 找到了未使用的 roident，插入新行并做一次 CCI，这样调用方如果需要就可
			 * 以查到它。
			 */
			memset(&nulls, 0, sizeof(nulls));

			values[Anum_pg_replication_origin_roident - 1] = ObjectIdGetDatum(roident);
			values[Anum_pg_replication_origin_roname - 1] = roname_d;

			tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
			CatalogTupleInsert(rel, tuple);
			CommandCounterIncrement();
			break;
		}
	}

	/* now release lock again,
	 *
	 * 现在再次释放锁
	 */
	table_close(rel, ExclusiveLock);

	if (tuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("could not find free replication origin ID")));

	heap_freetuple(tuple);
	return roident;
}

/*
 * Helper function to drop a replication origin.
 *
 * 删除复制源的辅助函数。
 */
static void
replorigin_state_clear(RepOriginId roident, bool nowait)
{
	int			i;

	/*
	 * Clean up the slot state info, if there is any matching slot.
	 *
	 * 若有匹配的 slot，则清理其状态信息。
	 */
restart:
	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	for (i = 0; i < max_active_replication_origins; i++)
	{
		ReplicationState *state = &replication_states[i];

		if (state->roident == roident)
		{
			/* found our slot, is it busy?
			 *
			 * 找到了我们的 slot，它是否正忙？
			 */
			if (state->acquired_by != 0)
			{
				ConditionVariable *cv;

				if (nowait)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_IN_USE),
							 errmsg("could not drop replication origin with ID %d, in use by PID %d",
									state->roident,
									state->acquired_by)));

				/*
				 * We must wait and then retry.  Since we don't know which CV
				 * to wait on until here, we can't readily use
				 * ConditionVariablePrepareToSleep (calling it here would be
				 * wrong, since we could miss the signal if we did so); just
				 * use ConditionVariableSleep directly.
				 *
				 * 必须等待然后重试。因为直到这里才知道该等哪一个条件变量，所以不能方便
				 * 地使用 ConditionVariablePrepareToSleep（在这里调用是错的，可能错过信
				 * 号）；直接使用 ConditionVariableSleep。
				 */
				cv = &state->origin_cv;

				LWLockRelease(ReplicationOriginLock);

				ConditionVariableSleep(cv, WAIT_EVENT_REPLICATION_ORIGIN_DROP);
				goto restart;
			}

			/* first make a WAL log entry
			 *
			 * 先写一条 WAL 记录
			 */
			{
				xl_replorigin_drop xlrec;

				xlrec.node_id = roident;
				XLogBeginInsert();
				XLogRegisterData(&xlrec, sizeof(xlrec));
				XLogInsert(RM_REPLORIGIN_ID, XLOG_REPLORIGIN_DROP);
			}

			/* then clear the in-memory slot
			 *
			 * 然后清空内存中的 slot
			 */
			state->roident = InvalidRepOriginId;
			state->remote_lsn = InvalidXLogRecPtr;
			state->local_lsn = InvalidXLogRecPtr;
			break;
		}
	}
	LWLockRelease(ReplicationOriginLock);
	ConditionVariableCancelSleep();
}

/*
 * Drop replication origin (by name).
 *
 * 按名称删除复制源。
 *
 * Needs to be called in a transaction.
 *
 * 必须在事务中调用。
 */
void
replorigin_drop_by_name(const char *name, bool missing_ok, bool nowait)
{
	RepOriginId roident;
	Relation	rel;
	HeapTuple	tuple;

	Assert(IsTransactionState());

	rel = table_open(ReplicationOriginRelationId, RowExclusiveLock);

	roident = replorigin_by_name(name, missing_ok);

	/* Lock the origin to prevent concurrent drops.
	 *
	 * 锁住该 origin，防止并发删除。
	 */
	LockSharedObject(ReplicationOriginRelationId, roident, 0,
					 AccessExclusiveLock);

	tuple = SearchSysCache1(REPLORIGIDENT, ObjectIdGetDatum(roident));
	if (!HeapTupleIsValid(tuple))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for replication origin with ID %d",
				 roident);

		/*
		 * We don't need to retain the locks if the origin is already dropped.
		 *
		 * 若 origin 已经被删掉，就不必继续持有这些锁。
		 */
		UnlockSharedObject(ReplicationOriginRelationId, roident, 0,
						   AccessExclusiveLock);
		table_close(rel, RowExclusiveLock);
		return;
	}

	replorigin_state_clear(roident, nowait);

	/*
	 * Now, we can delete the catalog entry.
	 *
	 * 现在可以删除目录项。
	 */
	CatalogTupleDelete(rel, &tuple->t_self);
	ReleaseSysCache(tuple);

	CommandCounterIncrement();

	/* We keep the lock on pg_replication_origin until commit
	 *
	 * 对 pg_replication_origin 的锁一直保持到提交
	 */
	table_close(rel, NoLock);
}

/*
 * Lookup replication origin via its oid and return the name.
 *
 * 按 oid 查找复制源并返回名称。
 *
 * The external name is palloc'd in the calling context.
 *
 * 外部名称用 palloc 分配在调用方的内存上下文中。
 *
 * Returns true if the origin is known, false otherwise.
 *
 * origin 已知则返回 true，否则返回 false。
 */
bool
replorigin_by_oid(RepOriginId roident, bool missing_ok, char **roname)
{
	HeapTuple	tuple;
	Form_pg_replication_origin ric;

	Assert(OidIsValid((Oid) roident));
	Assert(roident != InvalidRepOriginId);
	Assert(roident != DoNotReplicateId);

	tuple = SearchSysCache1(REPLORIGIDENT,
							ObjectIdGetDatum((Oid) roident));

	if (HeapTupleIsValid(tuple))
	{
		ric = (Form_pg_replication_origin) GETSTRUCT(tuple);
		*roname = text_to_cstring(&ric->roname);
		ReleaseSysCache(tuple);

		return true;
	}
	else
	{
		*roname = NULL;

		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("replication origin with ID %d does not exist",
							roident)));

		return false;
	}
}


/* ---------------------------------------------------------------------------
 * Functions for handling replication progress.
 *
 * 处理复制进度的函数。
 * ---------------------------------------------------------------------------
 */

Size
ReplicationOriginShmemSize(void)
{
	Size		size = 0;

	if (max_active_replication_origins == 0)
		return size;

	size = add_size(size, offsetof(ReplicationStateCtl, states));

	size = add_size(size,
					mul_size(max_active_replication_origins, sizeof(ReplicationState)));
	return size;
}

/*
 * 初始化复制源进度使用的共享内存，并在首次创建时设置每个 slot 的
 * LWLock。
 */
void
ReplicationOriginShmemInit(void)
{
	bool		found;

	if (max_active_replication_origins == 0)
		return;

	replication_states_ctl = (ReplicationStateCtl *)
		ShmemInitStruct("ReplicationOriginState",
						ReplicationOriginShmemSize(),
						&found);
	replication_states = replication_states_ctl->states;

	if (!found)
	{
		int			i;

		MemSet(replication_states_ctl, 0, ReplicationOriginShmemSize());

		replication_states_ctl->tranche_id = LWTRANCHE_REPLICATION_ORIGIN_STATE;

		for (i = 0; i < max_active_replication_origins; i++)
		{
			LWLockInitialize(&replication_states[i].lock,
							 replication_states_ctl->tranche_id);
			ConditionVariableInit(&replication_states[i].origin_cv);
		}
	}
}

/* ---------------------------------------------------------------------------
 * Perform a checkpoint of each replication origin's progress with respect to
 * the replayed remote_lsn. Make sure that all transactions we refer to in the
 * checkpoint (local_lsn) are actually on-disk. This might not yet be the case
 * if the transactions were originally committed asynchronously.
 *
 * 为每个复制源的重放进度做检查点，进度相对于已重放的 remote_lsn。确保
 * 检查点中引用的所有事务（local_lsn）确实已经在磁盘上。若这些事务最初
 * 是异步提交的，此时可能还没落盘。
 *
 * We store checkpoints in the following format:
 * +-------+------------------------+------------------+-----+--------+
 * | MAGIC | ReplicationStateOnDisk | struct Replic... | ... | CRC32C | EOF
 *
 * 检查点按如下格式存放：MAGIC，随后是若干 ReplicationStateOnDisk（图中
 * 为 struct Replic...），最后是 CRC32C，直到 EOF。
 * +-------+------------------------+------------------+-----+--------+
 *
 * So its just the magic, followed by the statically sized
 * ReplicationStateOnDisk structs. Note that the maximum number of
 * ReplicationState is determined by max_active_replication_origins.
 *
 * 也就是先写魔数，后面跟着固定大小的 ReplicationStateOnDisk 结构。
 * ReplicationState 的最大个数由 max_active_replication_origins 决定。
 * ---------------------------------------------------------------------------
 */
void
CheckPointReplicationOrigin(void)
{
	const char *tmppath = PG_REPLORIGIN_CHECKPOINT_TMPFILE;
	const char *path = PG_REPLORIGIN_CHECKPOINT_FILENAME;
	int			tmpfd;
	int			i;
	uint32		magic = REPLICATION_STATE_MAGIC;
	pg_crc32c	crc;

	if (max_active_replication_origins == 0)
		return;

	INIT_CRC32C(crc);

	/* make sure no old temp file is remaining
	 *
	 * 确保没有残留的旧临时文件
	 */
	if (unlink(tmppath) < 0 && errno != ENOENT)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						tmppath)));

	/*
	 * no other backend can perform this at the same time; only one checkpoint
	 * can happen at a time.
	 *
	 * 同一时刻不能有其他后端做这件事；一次只能有一个检查点。
	 */
	tmpfd = OpenTransientFile(tmppath,
							  O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (tmpfd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						tmppath)));

	/* write magic
	 *
	 * 写入魔数
	 */
	errno = 0;
	if ((write(tmpfd, &magic, sizeof(magic))) != sizeof(magic))
	{
		/* if write didn't set errno, assume problem is no disk space
		 *
		 * 若写操作没有设置 errno，就假定是磁盘空间不足
		 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
	}
	COMP_CRC32C(crc, &magic, sizeof(magic));

	/* prevent concurrent creations/drops
	 *
	 * 防止并发创建或删除
	 */
	LWLockAcquire(ReplicationOriginLock, LW_SHARED);

	/* write actual data
	 *
	 * 写入实际数据
	 */
	for (i = 0; i < max_active_replication_origins; i++)
	{
		ReplicationStateOnDisk disk_state;
		ReplicationState *curstate = &replication_states[i];
		XLogRecPtr	local_lsn;

		if (curstate->roident == InvalidRepOriginId)
			continue;

		/* zero, to avoid uninitialized padding bytes
		 *
		 * 清零，以免写出未初始化的填充字节
		 */
		memset(&disk_state, 0, sizeof(disk_state));

		LWLockAcquire(&curstate->lock, LW_SHARED);

		disk_state.roident = curstate->roident;

		disk_state.remote_lsn = curstate->remote_lsn;
		local_lsn = curstate->local_lsn;

		LWLockRelease(&curstate->lock);

		/* make sure we only write out a commit that's persistent
		 *
		 * 确保只写出已经持久化的提交
		 */
		XLogFlush(local_lsn);

		errno = 0;
		if ((write(tmpfd, &disk_state, sizeof(disk_state))) !=
			sizeof(disk_state))
		{
			/* if write didn't set errno, assume problem is no disk space
			 *
			 * 若写操作没有设置 errno，就假定是磁盘空间不足
			 */
			if (errno == 0)
				errno = ENOSPC;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							tmppath)));
		}

		COMP_CRC32C(crc, &disk_state, sizeof(disk_state));
	}

	LWLockRelease(ReplicationOriginLock);

	/* write out the CRC
	 *
	 * 写出 CRC
	 */
	FIN_CRC32C(crc);
	errno = 0;
	if ((write(tmpfd, &crc, sizeof(crc))) != sizeof(crc))
	{
		/* if write didn't set errno, assume problem is no disk space
		 *
		 * 若写操作没有设置 errno，就假定是磁盘空间不足
		 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
	}

	if (CloseTransientFile(tmpfd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						tmppath)));

	/* fsync, rename to permanent file, fsync file and directory
	 *
	 * fsync，重命名为永久文件，再对文件和目录做 fsync
	 */
	durable_rename(tmppath, path, PANIC);
}

/*
 * Recover replication replay status from checkpoint data saved earlier by
 * CheckPointReplicationOrigin.
 *
 * 从先前由 CheckPointReplicationOrigin 保存的检查点数据中恢复复制重放
 * 状态。
 *
 * This only needs to be called at startup and *not* during every checkpoint
 * read during recovery (e.g. in HS or PITR from a base backup) afterwards. All
 * state thereafter can be recovered by looking at commit records.
 *
 * 只需要在启动时调用，不必在恢复期间每次读检查点时都调用（例如热备，或
 * 从基础备份做 PITR）。此后的全部状态都可以通过查看提交记录来恢复。
 */
void
StartupReplicationOrigin(void)
{
	const char *path = PG_REPLORIGIN_CHECKPOINT_FILENAME;
	int			fd;
	int			readBytes;
	uint32		magic = REPLICATION_STATE_MAGIC;
	int			last_state = 0;
	pg_crc32c	file_crc;
	pg_crc32c	crc;

	/* don't want to overwrite already existing state
	 *
	 * 不要覆盖已经存在的状态
	 */
#ifdef USE_ASSERT_CHECKING
	static bool already_started = false;

	Assert(!already_started);
	already_started = true;
#endif

	if (max_active_replication_origins == 0)
		return;

	INIT_CRC32C(crc);

	elog(DEBUG2, "starting up replication origin progress state");

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);

	/*
	 * might have had max_active_replication_origins == 0 last run, or we just
	 * brought up a standby.
	 *
	 * 上次运行时 max_active_replication_origins 可能为 0，或者我们刚刚拉起
	 * 了一个备库。
	 */
	if (fd < 0 && errno == ENOENT)
		return;
	else if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));

	/* verify magic, that is written even if nothing was active
	 *
	 * 校验魔数；即使当时没有任何活动 origin，魔数也会被写入
	 */
	readBytes = read(fd, &magic, sizeof(magic));
	if (readBytes != sizeof(magic))
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, sizeof(magic))));
	}
	COMP_CRC32C(crc, &magic, sizeof(magic));

	if (magic != REPLICATION_STATE_MAGIC)
		ereport(PANIC,
				(errmsg("replication checkpoint has wrong magic %u instead of %u",
						magic, REPLICATION_STATE_MAGIC)));

	/* we can skip locking here, no other access is possible
	 *
	 * 这里可以不加锁，不可能有其他访问
	 */

	/* recover individual states, until there are no more to be found
	 *
	 * 逐个恢复状态，直到再也读不到为止
	 */
	while (true)
	{
		ReplicationStateOnDisk disk_state;

		readBytes = read(fd, &disk_state, sizeof(disk_state));

		/* no further data
		 *
		 * 没有更多数据
		 */
		if (readBytes == sizeof(crc))
		{
			/* not pretty, but simple ...
			 *
			 * 不算漂亮，但很简单
			 */
			file_crc = *(pg_crc32c *) &disk_state;
			break;
		}

		if (readBytes < 0)
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		}

		if (readBytes != sizeof(disk_state))
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, sizeof(disk_state))));
		}

		COMP_CRC32C(crc, &disk_state, sizeof(disk_state));

		if (last_state == max_active_replication_origins)
			ereport(PANIC,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("could not find free replication state, increase \"max_active_replication_origins\"")));

		/* copy data to shared memory
		 *
		 * 把数据复制到共享内存
		 */
		replication_states[last_state].roident = disk_state.roident;
		replication_states[last_state].remote_lsn = disk_state.remote_lsn;
		last_state++;

		ereport(LOG,
				(errmsg("recovered replication state of node %d to %X/%X",
						disk_state.roident,
						LSN_FORMAT_ARGS(disk_state.remote_lsn))));
	}

	/* now check checksum
	 *
	 * 现在检查校验和
	 */
	FIN_CRC32C(crc);
	if (file_crc != crc)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot checkpoint has wrong checksum %u, expected %u",
						crc, file_crc)));

	if (CloseTransientFile(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						path)));
}

/*
 * 重放复制源相关的 WAL：推进进度或删除 origin。
 */
void
replorigin_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_REPLORIGIN_SET:
			{
				xl_replorigin_set *xlrec =
					(xl_replorigin_set *) XLogRecGetData(record);

				replorigin_advance(xlrec->node_id,
								   xlrec->remote_lsn, record->EndRecPtr,
								   xlrec->force /* backward
												 *
												 * 允许回退
												 */ ,
								   false /* WAL log
										  *
										  * 写 WAL
										  */ );
				break;
			}
		case XLOG_REPLORIGIN_DROP:
			{
				xl_replorigin_drop *xlrec;
				int			i;

				xlrec = (xl_replorigin_drop *) XLogRecGetData(record);

				for (i = 0; i < max_active_replication_origins; i++)
				{
					ReplicationState *state = &replication_states[i];

					/* found our slot
					 *
					 * 找到了我们的 slot
					 */
					if (state->roident == xlrec->node_id)
					{
						/* reset entry
						 *
						 * 重置该项
						 */
						state->roident = InvalidRepOriginId;
						state->remote_lsn = InvalidXLogRecPtr;
						state->local_lsn = InvalidXLogRecPtr;
						break;
					}
				}
				break;
			}
		default:
			elog(PANIC, "replorigin_redo: unknown op code %u", info);
	}
}


/*
 * Tell the replication origin progress machinery that a commit from 'node'
 * that originated at the LSN remote_commit on the remote node was replayed
 * successfully and that we don't need to do so again. In combination with
 * setting up replorigin_session_origin_lsn and replorigin_session_origin
 * that ensures we won't lose knowledge about that after a crash if the
 * transaction had a persistent effect (think of asynchronous commits).
 *
 * 告知复制源进度机制：来自 node、在远程节点上位于 LSN remote_commit 的
 * 提交已经成功重放，不必再重放一次。再配合设置
 * replorigin_session_origin_lsn 和 replorigin_session_origin，就能保证
 * 若该事务有持久效果（例如异步提交），崩溃后也不会丢掉这一信息。
 *
 * local_commit needs to be a local LSN of the commit so that we can make sure
 * upon a checkpoint that enough WAL has been persisted to disk.
 *
 * local_commit 必须是这次提交的本地 LSN，这样检查点时才能确认已经有足
 * 够的 WAL 持久化到磁盘。
 *
 * Needs to be called with a RowExclusiveLock on pg_replication_origin,
 * unless running in recovery.
 *
 * 除非正在恢复，否则调用时必须对 pg_replication_origin 持有
 * RowExclusiveLock。
 */
void
replorigin_advance(RepOriginId node,
				   XLogRecPtr remote_commit, XLogRecPtr local_commit,
				   bool go_backward, bool wal_log)
{
	int			i;
	ReplicationState *replication_state = NULL;
	ReplicationState *free_state = NULL;

	Assert(node != InvalidRepOriginId);

	/* we don't track DoNotReplicateId
	 *
	 * 不跟踪 DoNotReplicateId
	 */
	if (node == DoNotReplicateId)
		return;

	/*
	 * XXX: For the case where this is called by WAL replay, it'd be more
	 * efficient to restore into a backend local hashtable and only dump into
	 * shmem after recovery is finished. Let's wait with implementing that
	 * till it's shown to be a measurable expense
	 *
	 * XXX：若由 WAL 重放调用，更高效的做法是先恢复到后端本地哈希表，等恢复
	 * 结束后再一次性倒进共享内存。等证明这是可测的开销之后再实现。
	 */

	/* Lock exclusively, as we may have to create a new table entry.
	 *
	 * 排他加锁，因为可能要新建一个表项。
	 */
	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	/*
	 * Search for either an existing slot for the origin, or a free one we can
	 * use.
	 *
	 * 查找该 origin 已有的 slot，或者一个可以占用的空闲 slot。
	 */
	for (i = 0; i < max_active_replication_origins; i++)
	{
		ReplicationState *curstate = &replication_states[i];

		/* remember where to insert if necessary
		 *
		 * 如有必要，记住插入位置
		 */
		if (curstate->roident == InvalidRepOriginId &&
			free_state == NULL)
		{
			free_state = curstate;
			continue;
		}

		/* not our slot
		 *
		 * 不是我们的 slot
		 */
		if (curstate->roident != node)
		{
			continue;
		}

		/* ok, found slot
		 *
		 * 找到了 slot
		 */
		replication_state = curstate;

		LWLockAcquire(&replication_state->lock, LW_EXCLUSIVE);

		/* Make sure it's not used by somebody else
		 *
		 * 确保没有被别人占用
		 */
		if (replication_state->acquired_by != 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication origin with ID %d is already active for PID %d",
							replication_state->roident,
							replication_state->acquired_by)));
		}

		break;
	}

	if (replication_state == NULL && free_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("could not find free replication state slot for replication origin with ID %d",
						node),
				 errhint("Increase \"max_active_replication_origins\" and try again.")));

	if (replication_state == NULL)
	{
		/* initialize new slot
		 *
		 * 初始化新 slot
		 */
		LWLockAcquire(&free_state->lock, LW_EXCLUSIVE);
		replication_state = free_state;
		Assert(replication_state->remote_lsn == InvalidXLogRecPtr);
		Assert(replication_state->local_lsn == InvalidXLogRecPtr);
		replication_state->roident = node;
	}

	Assert(replication_state->roident != InvalidRepOriginId);

	/*
	 * If somebody "forcefully" sets this slot, WAL log it, so it's durable
	 * and the standby gets the message. Primarily this will be called during
	 * WAL replay (of commit records) where no WAL logging is necessary.
	 *
	 * 若有人强制设置这个 slot，就记入 WAL，使其持久，备库也能收到。这主要
	 * 会在 WAL 重放（提交记录）期间被调用，那时不需要再写 WAL。
	 */
	if (wal_log)
	{
		xl_replorigin_set xlrec;

		xlrec.remote_lsn = remote_commit;
		xlrec.node_id = node;
		xlrec.force = go_backward;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xlrec));

		XLogInsert(RM_REPLORIGIN_ID, XLOG_REPLORIGIN_SET);
	}

	/*
	 * Due to - harmless - race conditions during a checkpoint we could see
	 * values here that are older than the ones we already have in memory. We
	 * could also see older values for prepared transactions when the prepare
	 * is sent at a later point of time along with commit prepared and there
	 * are other transactions commits between prepare and commit prepared. See
	 * ReorderBufferFinishPrepared. Don't overwrite those.
	 *
	 * 由于检查点期间无害的竞态，这里可能看到比内存中已有值更旧的值。在
	 * prepare 较晚才和 commit prepared 一起发送、且 prepare 与 commit
	 * prepared 之间还有其他事务提交时，预备事务也可能看到更旧的值。见
	 * ReorderBufferFinishPrepared。不要覆盖那些更新的值。
	 */
	if (go_backward || replication_state->remote_lsn < remote_commit)
		replication_state->remote_lsn = remote_commit;
	if (local_commit != InvalidXLogRecPtr &&
		(go_backward || replication_state->local_lsn < local_commit))
		replication_state->local_lsn = local_commit;
	LWLockRelease(&replication_state->lock);

	/*
	 * Release *after* changing the LSNs, slot isn't acquired and thus could
	 * otherwise be dropped anytime.
	 *
	 * 在改完 LSN 之后再释放。slot 并未被占用，否则随时可能被删掉。
	 */
	LWLockRelease(ReplicationOriginLock);
}


/*
 * 读取指定复制源的重放进度；flush 为真时先把对应本地 LSN 刷盘。
 */
XLogRecPtr
replorigin_get_progress(RepOriginId node, bool flush)
{
	int			i;
	XLogRecPtr	local_lsn = InvalidXLogRecPtr;
	XLogRecPtr	remote_lsn = InvalidXLogRecPtr;

	/* prevent slots from being concurrently dropped
	 *
	 * 防止 slot 被并发删除
	 */
	LWLockAcquire(ReplicationOriginLock, LW_SHARED);

	for (i = 0; i < max_active_replication_origins; i++)
	{
		ReplicationState *state;

		state = &replication_states[i];

		if (state->roident == node)
		{
			LWLockAcquire(&state->lock, LW_SHARED);

			remote_lsn = state->remote_lsn;
			local_lsn = state->local_lsn;

			LWLockRelease(&state->lock);

			break;
		}
	}

	LWLockRelease(ReplicationOriginLock);

	if (flush && local_lsn != InvalidXLogRecPtr)
		XLogFlush(local_lsn);

	return remote_lsn;
}

/*
 * Tear down a (possibly) configured session replication origin during process
 * exit.
 *
 * 进程退出时，拆除本会话可能已经配置的复制源。
 */
static void
ReplicationOriginExitCleanup(int code, Datum arg)
{
	ConditionVariable *cv = NULL;

	if (session_replication_state == NULL)
		return;

	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	if (session_replication_state->acquired_by == MyProcPid)
	{
		cv = &session_replication_state->origin_cv;

		session_replication_state->acquired_by = 0;
		session_replication_state = NULL;
	}

	LWLockRelease(ReplicationOriginLock);

	if (cv)
		ConditionVariableBroadcast(cv);
}

/*
 * Setup a replication origin in the shared memory struct if it doesn't
 * already exist and cache access to the specific ReplicationSlot so the
 * array doesn't have to be searched when calling
 * replorigin_session_advance().
 *
 * 若共享内存结构里还没有该复制源，就建立它，并缓存对特定
 * ReplicationSlot 的访问，这样调用 replorigin_session_advance() 时不必
 * 再搜索数组。
 *
 * Normally only one such cached origin can exist per process so the cached
 * value can only be set again after the previous value is torn down with
 * replorigin_session_reset(). For this normal case pass acquired_by = 0
 * (meaning the slot is not allowed to be already acquired by another process).
 *
 * 通常每个进程只能有一个这样的缓存 origin，因此必须先用
 * replorigin_session_reset() 拆掉前一个值，才能再次设置。这种普通情况
 * 传入 acquired_by = 0（表示该 slot 不允许已被其他进程占用）。
 *
 * However, sometimes multiple processes can safely re-use the same origin slot
 * (for example, multiple parallel apply processes can safely use the same
 * origin, provided they maintain commit order by allowing only one process to
 * commit at a time). For this case the first process must pass acquired_by =
 * 0, and then the other processes sharing that same origin can pass
 * acquired_by = PID of the first process.
 *
 * 不过有时多个进程可以安全地共用同一个 origin slot（例如多个并行 apply
 * 进程可以共用同一个 origin，只要它们通过同一时刻只允许一个进程提交来
 * 保持提交顺序）。这种情况下，第一个进程必须传入 acquired_by = 0，随后
 * 共用该 origin 的其他进程传入的 acquired_by 为第一个进程的 PID。
 */
void
replorigin_session_setup(RepOriginId node, int acquired_by)
{
	static bool registered_cleanup;
	int			i;
	int			free_slot = -1;

	if (!registered_cleanup)
	{
		on_shmem_exit(ReplicationOriginExitCleanup, 0);
		registered_cleanup = true;
	}

	Assert(max_active_replication_origins > 0);

	if (session_replication_state != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot setup replication origin when one is already setup")));

	/* Lock exclusively, as we may have to create a new table entry.
	 *
	 * 排他加锁，因为可能要新建一个表项。
	 */
	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	/*
	 * Search for either an existing slot for the origin, or a free one we can
	 * use.
	 *
	 * 查找该 origin 已有的 slot，或者一个可以占用的空闲 slot。
	 */
	for (i = 0; i < max_active_replication_origins; i++)
	{
		ReplicationState *curstate = &replication_states[i];

		/* remember where to insert if necessary
		 *
		 * 如有必要，记住插入位置
		 */
		if (curstate->roident == InvalidRepOriginId &&
			free_slot == -1)
		{
			free_slot = i;
			continue;
		}

		/* not our slot
		 *
		 * 不是我们的 slot
		 */
		if (curstate->roident != node)
			continue;

		else if (curstate->acquired_by != 0 && acquired_by == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication origin with ID %d is already active for PID %d",
							curstate->roident, curstate->acquired_by)));
		}

		/* ok, found slot
		 *
		 * 找到了 slot
		 */
		session_replication_state = curstate;
		break;
	}


	if (session_replication_state == NULL && free_slot == -1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("could not find free replication state slot for replication origin with ID %d",
						node),
				 errhint("Increase \"max_active_replication_origins\" and try again.")));
	else if (session_replication_state == NULL)
	{
		/* initialize new slot
		 *
		 * 初始化新 slot
		 */
		session_replication_state = &replication_states[free_slot];
		Assert(session_replication_state->remote_lsn == InvalidXLogRecPtr);
		Assert(session_replication_state->local_lsn == InvalidXLogRecPtr);
		session_replication_state->roident = node;
	}


	Assert(session_replication_state->roident != InvalidRepOriginId);

	if (acquired_by == 0)
		session_replication_state->acquired_by = MyProcPid;
	else if (session_replication_state->acquired_by != acquired_by)
		elog(ERROR, "could not find replication state slot for replication origin with OID %u which was acquired by %d",
			 node, acquired_by);

	LWLockRelease(ReplicationOriginLock);

	/* probably this one is pointless
	 *
	 * 这一项大概没有意义
	 */
	ConditionVariableBroadcast(&session_replication_state->origin_cv);
}

/*
 * Reset replay state previously setup in this session.
 *
 * 重置本会话先前设置的重放状态。
 *
 * This function may only be called if an origin was setup with
 * replorigin_session_setup().
 *
 * 只有在已经用 replorigin_session_setup() 设置过 origin 时才能调用本函数。
 */
void
replorigin_session_reset(void)
{
	ConditionVariable *cv;

	Assert(max_active_replication_origins != 0);

	if (session_replication_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no replication origin is configured")));

	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	session_replication_state->acquired_by = 0;
	cv = &session_replication_state->origin_cv;
	session_replication_state = NULL;

	LWLockRelease(ReplicationOriginLock);

	ConditionVariableBroadcast(cv);
}

/*
 * Do the same work replorigin_advance() does, just on the session's
 * configured origin.
 *
 * 做与 replorigin_advance() 相同的事，但针对本会话已配置的 origin。
 *
 * This is noticeably cheaper than using replorigin_advance().
 *
 * 这比使用 replorigin_advance() 明显更便宜。
 */
void
replorigin_session_advance(XLogRecPtr remote_commit, XLogRecPtr local_commit)
{
	Assert(session_replication_state != NULL);
	Assert(session_replication_state->roident != InvalidRepOriginId);

	LWLockAcquire(&session_replication_state->lock, LW_EXCLUSIVE);
	if (session_replication_state->local_lsn < local_commit)
		session_replication_state->local_lsn = local_commit;
	if (session_replication_state->remote_lsn < remote_commit)
		session_replication_state->remote_lsn = remote_commit;
	LWLockRelease(&session_replication_state->lock);
}

/*
 * Ask the machinery about the point up to which we successfully replayed
 * changes from an already setup replication origin.
 *
 * 向该机制查询：对于已经设置好的复制源，我们成功重放到了哪一点。
 */
XLogRecPtr
replorigin_session_get_progress(bool flush)
{
	XLogRecPtr	remote_lsn;
	XLogRecPtr	local_lsn;

	Assert(session_replication_state != NULL);

	LWLockAcquire(&session_replication_state->lock, LW_SHARED);
	remote_lsn = session_replication_state->remote_lsn;
	local_lsn = session_replication_state->local_lsn;
	LWLockRelease(&session_replication_state->lock);

	if (flush && local_lsn != InvalidXLogRecPtr)
		XLogFlush(local_lsn);

	return remote_lsn;
}



/* ---------------------------------------------------------------------------
 * SQL functions for working with replication origin.
 *
 * 操作复制源的 SQL 函数。
 *
 * These mostly should be fairly short wrappers around more generic functions.
 *
 * 它们大多应是更通用函数的简短包装。
 * ---------------------------------------------------------------------------
 */

/*
 * Create replication origin for the passed in name, and return the assigned
 * oid.
 *
 * 为传入的名称创建复制源，并返回分配到的 oid。
 */
Datum
pg_replication_origin_create(PG_FUNCTION_ARGS)
{
	char	   *name;
	RepOriginId roident;

	replorigin_check_prerequisites(false, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));

	/*
	 * Replication origins "any and "none" are reserved for system options.
	 * The origins "pg_xxx" are reserved for internal use.
	 *
	 * 复制源名称 any 和 none 保留给系统选项。名称 pg_xxx 保留给内部使用。
	 */
	if (IsReservedName(name) || IsReservedOriginName(name))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("replication origin name \"%s\" is reserved",
						name),
				 errdetail("Origin names \"%s\", \"%s\", and names starting with \"pg_\" are reserved.",
						   LOGICALREP_ORIGIN_ANY, LOGICALREP_ORIGIN_NONE)));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for replication origin names are violated.
	 *
	 * 若以相应开关编译，当复制源名称违反回归测试约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(name, "regress_", 8) != 0)
		elog(WARNING, "replication origins created by regression test cases should have names starting with \"regress_\"");
#endif

	roident = replorigin_create(name);

	pfree(name);

	PG_RETURN_OID(roident);
}

/*
 * Drop replication origin.
 *
 * 删除复制源。
 */
Datum
pg_replication_origin_drop(PG_FUNCTION_ARGS)
{
	char	   *name;

	replorigin_check_prerequisites(false, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));

	replorigin_drop_by_name(name, false, true);

	pfree(name);

	PG_RETURN_VOID();
}

/*
 * Return oid of a replication origin.
 *
 * 返回某个复制源的 oid。
 */
Datum
pg_replication_origin_oid(PG_FUNCTION_ARGS)
{
	char	   *name;
	RepOriginId roident;

	replorigin_check_prerequisites(false, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));
	roident = replorigin_by_name(name, true);

	pfree(name);

	if (OidIsValid(roident))
		PG_RETURN_OID(roident);
	PG_RETURN_NULL();
}

/*
 * Setup a replication origin for this session.
 *
 * 为本会话设置一个复制源。
 */
Datum
pg_replication_origin_session_setup(PG_FUNCTION_ARGS)
{
	char	   *name;
	RepOriginId origin;

	replorigin_check_prerequisites(true, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));
	origin = replorigin_by_name(name, false);
	replorigin_session_setup(origin, 0);

	replorigin_session_origin = origin;

	pfree(name);

	PG_RETURN_VOID();
}

/*
 * Reset previously setup origin in this session
 *
 * 重置本会话先前设置的 origin
 */
Datum
pg_replication_origin_session_reset(PG_FUNCTION_ARGS)
{
	replorigin_check_prerequisites(true, false);

	replorigin_session_reset();

	replorigin_session_origin = InvalidRepOriginId;
	replorigin_session_origin_lsn = InvalidXLogRecPtr;
	replorigin_session_origin_timestamp = 0;

	PG_RETURN_VOID();
}

/*
 * Has a replication origin been setup for this session.
 *
 * 本会话是否已经设置了复制源。
 */
Datum
pg_replication_origin_session_is_setup(PG_FUNCTION_ARGS)
{
	replorigin_check_prerequisites(false, false);

	PG_RETURN_BOOL(replorigin_session_origin != InvalidRepOriginId);
}


/*
 * Return the replication progress for origin setup in the current session.
 *
 * 返回当前会话所设置 origin 的复制进度。
 *
 * If 'flush' is set to true it is ensured that the returned value corresponds
 * to a local transaction that has been flushed. This is useful if asynchronous
 * commits are used when replaying replicated transactions.
 *
 * 若 flush 为真，则保证返回值对应一个已经刷盘的本地事务。在重放复制事
 * 务时使用了异步提交的话，这很有用。
 */
Datum
pg_replication_origin_session_progress(PG_FUNCTION_ARGS)
{
	XLogRecPtr	remote_lsn = InvalidXLogRecPtr;
	bool		flush = PG_GETARG_BOOL(0);

	replorigin_check_prerequisites(true, false);

	if (session_replication_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no replication origin is configured")));

	remote_lsn = replorigin_session_get_progress(flush);

	if (remote_lsn == InvalidXLogRecPtr)
		PG_RETURN_NULL();

	PG_RETURN_LSN(remote_lsn);
}

/*
 * 为当前事务设置复制源的远程 LSN 和时间戳。
 */
Datum
pg_replication_origin_xact_setup(PG_FUNCTION_ARGS)
{
	XLogRecPtr	location = PG_GETARG_LSN(0);

	replorigin_check_prerequisites(true, false);

	if (session_replication_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no replication origin is configured")));

	replorigin_session_origin_lsn = location;
	replorigin_session_origin_timestamp = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_VOID();
}

/*
 * 清除当前事务上设置的复制源 LSN 和时间戳。
 */
Datum
pg_replication_origin_xact_reset(PG_FUNCTION_ARGS)
{
	replorigin_check_prerequisites(true, false);

	replorigin_session_origin_lsn = InvalidXLogRecPtr;
	replorigin_session_origin_timestamp = 0;

	PG_RETURN_VOID();
}


/*
 * 把指定复制源的远程进度推进到给定 LSN，并写入 WAL。
 */
Datum
pg_replication_origin_advance(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	XLogRecPtr	remote_commit = PG_GETARG_LSN(1);
	RepOriginId node;

	replorigin_check_prerequisites(true, false);

	/* lock to prevent the replication origin from vanishing
	 *
	 * 加锁，防止复制源消失
	 */
	LockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

	node = replorigin_by_name(text_to_cstring(name), false);

	/*
	 * Can't sensibly pass a local commit to be flushed at checkpoint - this
	 * xact hasn't committed yet. This is why this function should be used to
	 * set up the initial replication state, but not for replay.
	 *
	 * 没法合理地传入一个要在检查点时刷盘的本地提交，因为这个事务还没提交。
	 * 因此本函数应用于建立初始复制状态，而不是用于重放。
	 */
	replorigin_advance(node, remote_commit, InvalidXLogRecPtr,
					   true /* go backward
							 *
							 * 允许回退
							 */ , true /* WAL log
							 *
							 * 写 WAL
							 */ );

	UnlockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

	PG_RETURN_VOID();
}


/*
 * Return the replication progress for an individual replication origin.
 *
 * 返回单个复制源的复制进度。
 *
 * If 'flush' is set to true it is ensured that the returned value corresponds
 * to a local transaction that has been flushed. This is useful if asynchronous
 * commits are used when replaying replicated transactions.
 *
 * 若 flush 为真，则保证返回值对应一个已经刷盘的本地事务。在重放复制事
 * 务时使用了异步提交的话，这很有用。
 */
Datum
pg_replication_origin_progress(PG_FUNCTION_ARGS)
{
	char	   *name;
	bool		flush;
	RepOriginId roident;
	XLogRecPtr	remote_lsn = InvalidXLogRecPtr;

	replorigin_check_prerequisites(true, true);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));
	flush = PG_GETARG_BOOL(1);

	roident = replorigin_by_name(name, false);
	Assert(OidIsValid(roident));

	remote_lsn = replorigin_get_progress(roident, flush);

	if (remote_lsn == InvalidXLogRecPtr)
		PG_RETURN_NULL();

	PG_RETURN_LSN(remote_lsn);
}


/*
 * 返回所有复制源进度 slot 的状态。
 */
Datum
pg_show_replication_origin_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			i;
#define REPLICATION_ORIGIN_PROGRESS_COLS 4

	/* we want to return 0 rows if slot is set to zero
	 *
	 * 若 slot 数量被设为 0，则返回 0 行
	 */
	replorigin_check_prerequisites(false, true);

	InitMaterializedSRF(fcinfo, 0);

	/* prevent slots from being concurrently dropped
	 *
	 * 防止 slot 被并发删除
	 */
	LWLockAcquire(ReplicationOriginLock, LW_SHARED);

	/*
	 * Iterate through all possible replication_states, display if they are
	 * filled. Note that we do not take any locks, so slightly corrupted/out
	 * of date values are a possibility.
	 *
	 * 遍历所有可能的 replication_states，有内容的就显示出来。注意这里不加
	 * 任何锁，因此值可能略有损坏或过期。
	 */
	for (i = 0; i < max_active_replication_origins; i++)
	{
		ReplicationState *state;
		Datum		values[REPLICATION_ORIGIN_PROGRESS_COLS];
		bool		nulls[REPLICATION_ORIGIN_PROGRESS_COLS];
		char	   *roname;

		state = &replication_states[i];

		/* unused slot, nothing to display
		 *
		 * 未使用的 slot，没有可显示的内容
		 */
		if (state->roident == InvalidRepOriginId)
			continue;

		memset(values, 0, sizeof(values));
		memset(nulls, 1, sizeof(nulls));

		values[0] = ObjectIdGetDatum(state->roident);
		nulls[0] = false;

		/*
		 * We're not preventing the origin to be dropped concurrently, so
		 * silently accept that it might be gone.
		 *
		 * 我们并不阻止 origin 被并发删除，因此若它已经不在，就静默接受。
		 */
		if (replorigin_by_oid(state->roident, true,
							  &roname))
		{
			values[1] = CStringGetTextDatum(roname);
			nulls[1] = false;
		}

		LWLockAcquire(&state->lock, LW_SHARED);

		values[2] = LSNGetDatum(state->remote_lsn);
		nulls[2] = false;

		values[3] = LSNGetDatum(state->local_lsn);
		nulls[3] = false;

		LWLockRelease(&state->lock);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	LWLockRelease(ReplicationOriginLock);

#undef REPLICATION_ORIGIN_PROGRESS_COLS

	return (Datum) 0;
}
