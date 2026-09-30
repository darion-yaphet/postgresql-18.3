/*-------------------------------------------------------------------------
 *
 * standby.h
 *	  Definitions for hot standby mode.
 *
 *
 *	  热备模式的定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/standby.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STANDBY_H
#define STANDBY_H

#include "datatype/timestamp.h"
#include "storage/lock.h"
#include "storage/procsignal.h"
#include "storage/relfilelocator.h"
#include "storage/standbydefs.h"

/* User-settable GUC parameters */

/* 用户可设置的 GUC 参数。 */
extern PGDLLIMPORT int max_standby_archive_delay;
extern PGDLLIMPORT int max_standby_streaming_delay;
extern PGDLLIMPORT bool log_recovery_conflict_waits;

/* Initialize transaction state used during recovery.
 * Startup establishes conflict-tracking and recovery transaction resources
 * before replay permits standby queries.
 *
 * 初始化恢复期间使用的事务状态。
 * 启动过程会在重放允许备库查询前建立冲突跟踪和恢复事务资源。
 */
extern void InitRecoveryTransactionEnvironment(void);

/* Shut down recovery transaction state.
 * Cleanup releases conflict-tracking and transaction resources when recovery
 * processing ends.
 *
 * 关闭恢复事务状态。
 * 清理过程在恢复处理结束时释放冲突跟踪和事务资源。
 */
extern void ShutdownRecoveryTransactionEnvironment(void);

/* Resolve a recovery conflict caused by a snapshot horizon.
 * The routine identifies conflicting standby backends for the relation and
 * makes them release the snapshot or cancels them as required.
 *
 * 解决由快照范围引起的恢复冲突。
 * 此例程识别该关系上发生冲突的备库后端，并按需要使其释放快照或取消它们。
 */
extern void ResolveRecoveryConflictWithSnapshot(TransactionId snapshotConflictHorizon,
												bool isCatalogRel,
												RelFileLocator locator);
/* Resolve a snapshot conflict using a full transaction ID horizon.
 * The full-XID form preserves epoch information while locating and resolving
 * conflicting standby snapshots.
 *
 * 使用完整事务 ID 范围解决快照冲突。
 * 完整 XID 形式在定位和解决冲突的备库快照时保留纪元信息。
 */
extern void ResolveRecoveryConflictWithSnapshotFullXid(FullTransactionId snapshotConflictHorizon,
													   bool isCatalogRel,
													   RelFileLocator locator);
/* Resolve conflicts that prevent removal of a tablespace.
 * Backends using the target tablespace are signaled so recovery can continue.
 *
 * 解决阻止删除表空间的冲突。
 * 使用目标表空间的后端会收到信号，使恢复能够继续。
 */
extern void ResolveRecoveryConflictWithTablespace(Oid tsid);

/* Resolve conflicts that prevent removal of a database.
 * Backends connected to the target database are signaled before recovery
 * applies the database-level change.
 *
 * 解决阻止删除数据库的冲突。
 * 在恢复应用数据库级变更前，会向连接到目标数据库的后端发送信号。
 */
extern void ResolveRecoveryConflictWithDatabase(Oid dbid);

/* Resolve a recovery conflict with a lock held by a standby backend.
 * The lock tag locates conflicting holders, and logging_conflict controls the
 * conflict accounting path.
 *
 * 解决与备库后端持有锁的恢复冲突。
 * 锁标记定位冲突持有者，logging_conflict 控制冲突记账路径。
 */
extern void ResolveRecoveryConflictWithLock(LOCKTAG locktag, bool logging_conflict);

/* Resolve a recovery conflict caused by a pinned buffer.
 * Recovery signals the backend retaining the pin so WAL replay can proceed.
 *
 * 解决由被固定缓冲区引起的恢复冲突。
 * 恢复过程向保留该固定引用的后端发信号，使 WAL 重放可以继续。
 */
extern void ResolveRecoveryConflictWithBufferPin(void);

/* Check whether recovery conflict waiting has deadlocked.
 * The check detects an unsafe wait cycle and triggers the appropriate
 * conflict-resolution action.
 *
 * 检查恢复冲突等待是否已死锁。
 * 此检查检测不安全的等待环，并触发相应的冲突解决操作。
 */
extern void CheckRecoveryConflictDeadlock(void);

/* Handle a standby deadlock signal.
 * The signal handler records or processes deadlock state for recovery conflict
 * management.
 *
 * 处理备库死锁信号。
 * 信号处理程序为恢复冲突管理记录或处理死锁状态。
 */
extern void StandbyDeadLockHandler(void);

/* Handle a standby conflict-delay timeout.
 * The handler advances timeout processing so delayed recovery conflicts are
 * resolved.
 *
 * 处理备库冲突延迟超时。
 * 该处理程序推进超时处理，使延迟的恢复冲突得以解决。
 */
extern void StandbyTimeoutHandler(void);

/* Handle a standby lock-wait timeout.
 * The handler marks the pending lock conflict for resolution by normal
 * recovery processing.
 *
 * 处理备库锁等待超时。
 * 该处理程序将待处理的锁冲突标记为由常规恢复处理解决。
 */
extern void StandbyLockTimeoutHandler(void);

/* Log details of a recovery conflict.
 * The routine formats the reason, timing, and waiting transactions to make
 * conflict handling observable.
 *
 * 记录恢复冲突的详细信息。
 * 此例程格式化原因、时间和等待事务，使冲突处理可被观察。
 */
extern void LogRecoveryConflict(ProcSignalReason reason, TimestampTz wait_start,
								TimestampTz now, VirtualTransactionId *wait_list,
								bool still_waiting);

/*
 * Standby Rmgr (RM_STANDBY_ID)
 *
 * Standby recovery manager exists to perform actions that are required
 * to make hot standby work. That includes logging AccessExclusiveLocks taken
 * by transactions and running-xacts snapshots.
 */

/*
 * 备库恢复管理器（RM_STANDBY_ID）。
 *
 * 备库恢复管理器执行使热备可用所需的动作，其中包括记录事务取得的
 * AccessExclusiveLock 以及运行中事务快照。
 */

/* Record acquisition of an access-exclusive lock for standby replay.
 * The lock is emitted so a standby can reproduce conflict behavior.
 *
 * 记录访问排他锁的获取以供备库重放。
 * 该锁会被写出，使备库能够复现冲突行为。
 */
extern void StandbyAcquireAccessExclusiveLock(TransactionId xid, Oid dbOid, Oid relOid);

/* Release a transaction lock tree during standby processing.
 * The routine releases the parent and listed subtransaction locks in their
 * recovery bookkeeping path.
 *
 * 在备库处理中释放事务锁树。
 * 此例程在恢复记账路径中释放父事务和列出的子事务锁。
 */
extern void StandbyReleaseLockTree(TransactionId xid,
								   int nsubxids, TransactionId *subxids);
/* Release all standby-tracked access-exclusive locks.
 * The manager clears the current recovery lock set during transaction cleanup.
 *
 * 释放所有由备库跟踪的访问排他锁。
 * 管理器会在事务清理期间清除当前恢复锁集合。
 */
extern void StandbyReleaseAllLocks(void);

/* Release standby locks older than the supplied transaction ID.
 * The routine prunes obsolete tracking entries as recovery advances.
 *
 * 释放早于给定事务 ID 的备库锁。
 * 此例程会随着恢复推进修剪过时的跟踪条目。
 */
extern void StandbyReleaseOldLocks(TransactionId oldxid);

#define MinSizeOfXactRunningXacts offsetof(xl_running_xacts, xids)


/*
 * Declarations for GetRunningTransactionData(). Similar to Snapshots, but
 * not quite. This has nothing at all to do with visibility on this server,
 * so this is completely separate from snapmgr.c and snapmgr.h.
 * This data is important for creating the initial snapshot state on a
 * standby server. We need lots more information than a normal snapshot,
 * hence we use a specific data structure for our needs. This data
 * is written to WAL as a separate record immediately after each
 * checkpoint. That means that wherever we start a standby from we will
 * almost immediately see the data we need to begin executing queries.
 */

/*
 * GetRunningTransactionData() 的声明。它与快照类似，但并不完全相同。
 * 这与此服务器上的可见性完全无关，因此完全独立于 snapmgr.c 和 snapmgr.h。
 * 这些数据对在备库服务器上创建初始快照状态很重要。我们需要的信息远多于
 * 普通快照，因此使用满足需求的特定数据结构。这些数据会在每次检查点后立即
 * 作为单独的 WAL 记录写入。这意味着无论从何处启动备库，几乎都会立即看到
 * 开始执行查询所需的数据。
 */

typedef enum
{
	SUBXIDS_IN_ARRAY,			/* xids array includes all running subxids */

									/* xids 数组包含所有运行中的子事务 ID。 */
	SUBXIDS_MISSING,			/* snapshot overflowed, subxids are missing */

									/* 快照已溢出，子事务 ID 缺失。 */
	SUBXIDS_IN_SUBTRANS,		/* subxids are not included in 'xids', but
								 * pg_subtrans is fully up-to-date */

									/* 子事务 ID 未包含在“xids”中，但 pg_subtrans 已完全更新。 */
} subxids_array_status;

typedef struct RunningTransactionsData
{
	int			xcnt;			/* # of xact ids in xids[] */

									/* xids[] 中的事务 ID 数量。 */
	int			subxcnt;		/* # of subxact ids in xids[] */

									/* xids[] 中的子事务 ID 数量。 */
	subxids_array_status subxid_status;
	TransactionId nextXid;		/* xid from TransamVariables->nextXid */

									/* 来自 TransamVariables->nextXid 的 xid。 */
	TransactionId oldestRunningXid; /* *not* oldestXmin */

									/* 不是 oldestXmin。 */
	TransactionId oldestDatabaseRunningXid; /* same as above, but within the
											 * current database */

									/* 与上面相同，但限于当前数据库内。 */
	TransactionId latestCompletedXid;	/* so we can set xmax */

									/* 因此我们可以设置 xmax。 */

	TransactionId *xids;		/* array of (sub)xids still running */

									/* 仍在运行的（子）事务 ID 数组。 */
} RunningTransactionsData;

typedef RunningTransactionsData *RunningTransactions;

/* Write an access-exclusive lock record to WAL.
 * The record lets standby replay install equivalent conflict protection.
 *
 * 将访问排他锁记录写入 WAL。
 * 该记录使备库重放能够安装等效的冲突保护。
 */
extern void LogAccessExclusiveLock(Oid dbOid, Oid relOid);

/* Begin a WAL record sequence for access-exclusive locks.
 * The preparation step groups subsequent lock records under standby recovery
 * processing.
 *
 * 为访问排他锁开始 WAL 记录序列。
 * 准备步骤将后续锁记录归入备库恢复处理。
 */
extern void LogAccessExclusiveLockPrepare(void);

/* Write a running-transactions snapshot to WAL.
 * The routine gathers recovery snapshot state and returns the record's WAL
 * location.
 *
 * 将运行中事务快照写入 WAL。
 * 此例程收集恢复快照状态并返回记录的 WAL 位置。
 */
extern XLogRecPtr LogStandbySnapshot(void);

/* Write shared-cache invalidations to a standby WAL record.
 * The message batch and relcache-init-file flag are serialized for replay.
 *
 * 将共享缓存失效写入备库 WAL 记录。
 * 消息批次和 relcache 初始化文件标志会被序列化以供重放。
 */
extern void LogStandbyInvalidations(int nmsgs, SharedInvalidationMessage *msgs,
									bool relcacheInitFileInval);

#endif							/* STANDBY_H */
