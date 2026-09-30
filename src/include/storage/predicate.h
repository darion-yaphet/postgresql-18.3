/*-------------------------------------------------------------------------
 *
 * predicate.h
 *	  POSTGRES public predicate locking definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/predicate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICATE_H
#define PREDICATE_H

#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/*
 * GUC variables
 */

/*
 * GUC 变量。
 */
extern PGDLLIMPORT int max_predicate_locks_per_xact;
extern PGDLLIMPORT int max_predicate_locks_per_relation;
extern PGDLLIMPORT int max_predicate_locks_per_page;

/*
 * A handle used for sharing SERIALIZABLEXACT objects between the participants
 * in a parallel query.
 */

/*
 * 用于在并行查询参与者之间共享 SERIALIZABLEXACT 对象的句柄。
 */
typedef void *SerializableXactHandle;

/*
 * function prototypes
 */

/*
 * 函数原型。
 */

/* housekeeping for shared memory predicate lock structures */

/* 共享内存谓词锁结构的维护。 */
/*
 * Initializes shared-memory predicate-lock data structures.
 */

/*
 * 初始化共享内存谓词锁数据结构。
 */
extern void PredicateLockShmemInit(void);
/*
 * Returns the shared-memory space required for predicate locks.
 */

/*
 * 返回谓词锁所需的共享内存空间。
 */
extern Size PredicateLockShmemSize(void);

/*
 * Checkpoints predicate-lock state for durability and cleanup.
 */

/*
 * 对谓词锁状态执行检查点，以保证持久性并进行清理。
 */
extern void CheckPointPredicate(void);

/* predicate lock reporting */

/* 谓词锁报告。 */
/*
 * Reports whether a relation page has a predicate lock.
 */

/*
 * 报告关系页面是否具有谓词锁。
 */
extern bool PageIsPredicateLocked(Relation relation, BlockNumber blkno);

/* predicate lock maintenance */

/* 谓词锁维护。 */
/*
 * Obtains a serializable transaction snapshot from the supplied snapshot.
 */

/*
 * 从给定快照获取可串行化事务快照。
 */
extern Snapshot GetSerializableTransactionSnapshot(Snapshot snapshot);
/*
 * Installs a serializable transaction snapshot from another backend.
 */

/*
 * 安装来自另一个后端的可串行化事务快照。
 */
extern void SetSerializableTransactionSnapshot(Snapshot snapshot,
											   VirtualTransactionId *sourcevxid,
											   int sourcepid);
/*
 * Registers a transaction ID for predicate locking.
 */

/*
 * 为谓词锁注册事务 ID。
 */
extern void RegisterPredicateLockingXid(TransactionId xid);
/*
 * Acquires a predicate lock on a relation.
 */

/*
 * 在关系上获取谓词锁。
 */
extern void PredicateLockRelation(Relation relation, Snapshot snapshot);
/*
 * Acquires a predicate lock on one relation page.
 */

/*
 * 在一个关系页面上获取谓词锁。
 */
extern void PredicateLockPage(Relation relation, BlockNumber blkno, Snapshot snapshot);
/*
 * Acquires a predicate lock on a tuple.
 */

/*
 * 在元组上获取谓词锁。
 */
extern void PredicateLockTID(Relation relation, ItemPointer tid, Snapshot snapshot,
							 TransactionId tuple_xid);
/*
 * Transfers predicate locks when a page is split.
 */

/*
 * 在页面分裂时转移谓词锁。
 */
extern void PredicateLockPageSplit(Relation relation, BlockNumber oldblkno, BlockNumber newblkno);
/*
 * Combines predicate locks when pages are merged.
 */

/*
 * 在页面合并时组合谓词锁。
 */
extern void PredicateLockPageCombine(Relation relation, BlockNumber oldblkno, BlockNumber newblkno);
/*
 * Moves predicate locks to the corresponding heap relation.
 */

/*
 * 将谓词锁移至对应的堆关系。
 */
extern void TransferPredicateLocksToHeapRelation(Relation relation);
/*
 * Releases predicate locks at transaction completion.
 */

/*
 * 在事务结束时释放谓词锁。
 */
extern void ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe);

/* conflict detection (may also trigger rollback) */

/* 冲突检测（也可能触发回滚）。 */
/*
 * Checks whether an outgoing serializable conflict must be recorded.
 */

/*
 * 检查是否必须记录向外的可串行化冲突。
 */
extern bool CheckForSerializableConflictOutNeeded(Relation relation, Snapshot snapshot);
/*
 * Records an outgoing serializable conflict, possibly causing rollback.
 */

/*
 * 记录向外的可串行化冲突，并可能导致回滚。
 */
extern void CheckForSerializableConflictOut(Relation relation, TransactionId xid, Snapshot snapshot);
/*
 * Detects a serializable conflict caused by an incoming write.
 */

/*
 * 检测由传入写操作造成的可串行化冲突。
 */
extern void CheckForSerializableConflictIn(Relation relation, ItemPointer tid, BlockNumber blkno);
/*
 * Checks a relation for incoming serializable conflicts.
 */

/*
 * 检查关系中是否存在传入的可串行化冲突。
 */
extern void CheckTableForSerializableConflictIn(Relation relation);

/* final rollback checking */

/* 最终回滚检查。 */
/*
 * Raises a serialization failure if commit cannot safely proceed.
 */

/*
 * 若提交无法安全进行，则抛出可串行化失败。
 */
extern void PreCommit_CheckForSerializationFailure(void);

/* two-phase commit support */

/* 两阶段提交支持。 */
/*
 * Prepares predicate locks for two-phase commit.
 */

/*
 * 为两阶段提交准备谓词锁。
 */
extern void AtPrepare_PredicateLocks(void);
/*
 * Restores predicate-lock state after preparing a transaction.
 */

/*
 * 在事务准备后恢复谓词锁状态。
 */
extern void PostPrepare_PredicateLocks(TransactionId xid);
/*
 * Finishes predicate-lock processing for a two-phase transaction.
 */

/*
 * 完成两阶段事务的谓词锁处理。
 */
extern void PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit);
/*
 * Reconstructs predicate-lock state during two-phase recovery.
 */

/*
 * 在两阶段恢复期间重建谓词锁状态。
 */
extern void predicatelock_twophase_recover(TransactionId xid, uint16 info,
										   void *recdata, uint32 len);

/* parallel query support */

/* 并行查询支持。 */
/*
 * Shares the current serializable transaction with parallel workers.
 */

/*
 * 与并行工作进程共享当前可串行化事务。
 */
extern SerializableXactHandle ShareSerializableXact(void);
/*
 * Attaches a parallel worker to a shared serializable transaction.
 */

/*
 * 将并行工作进程附加到共享的可串行化事务。
 */
extern void AttachSerializableXact(SerializableXactHandle handle);

#endif							/* PREDICATE_H */
