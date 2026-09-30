/*-------------------------------------------------------------------------
 *
 * lmgr.h
 *	  POSTGRES lock manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lmgr.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * PostgreSQL 锁管理器定义。
 */
#ifndef LMGR_H
#define LMGR_H

#include "lib/stringinfo.h"
#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/rel.h"


/* XactLockTableWait operations */

/*
 * XactLockTableWait 操作。
 */
typedef enum XLTW_Oper
{
	XLTW_None,
	XLTW_Update,
	XLTW_Delete,
	XLTW_Lock,
	XLTW_LockUpdated,
	XLTW_InsertIndex,
	XLTW_InsertIndexUnique,
	XLTW_FetchUpdated,
	XLTW_RecheckExclusionConstr,
} XLTW_Oper;

/*
 * Initializes the lock information cached in a relation descriptor.
 */

/*
 * 初始化关系描述符中缓存的锁信息。
 */
extern void RelationInitLockInfo(Relation relation);

/* Lock a relation */

/*
 * 锁定关系。
 */
/*
 * Acquires a lock on a relation identified by OID, waiting if necessary.
 */

/*
 * 获取由 OID 标识的关系锁，必要时等待。
 */
extern void LockRelationOid(Oid relid, LOCKMODE lockmode);
/*
 * Acquires a lock on a relation identified by LockRelId.
 */

/*
 * 获取由 LockRelId 标识的关系锁。
 */
extern void LockRelationId(LockRelId *relid, LOCKMODE lockmode);
/*
 * Attempts to acquire an OID-identified relation lock without waiting.
 */

/*
 * 尝试不等待地获取由 OID 标识的关系锁。
 */
extern bool ConditionalLockRelationOid(Oid relid, LOCKMODE lockmode);
/*
 * Releases a LockRelId-identified relation lock.
 */

/*
 * 释放由 LockRelId 标识的关系锁。
 */
extern void UnlockRelationId(LockRelId *relid, LOCKMODE lockmode);
/*
 * Releases an OID-identified relation lock.
 */

/*
 * 释放由 OID 标识的关系锁。
 */
extern void UnlockRelationOid(Oid relid, LOCKMODE lockmode);

/*
 * Acquires a lock on a relation descriptor.
 */

/*
 * 获取关系描述符上的锁。
 */
extern void LockRelation(Relation relation, LOCKMODE lockmode);
/*
 * Attempts to acquire a relation-descriptor lock without waiting.
 */

/*
 * 尝试不等待地获取关系描述符上的锁。
 */
extern bool ConditionalLockRelation(Relation relation, LOCKMODE lockmode);
/*
 * Releases a lock held on a relation descriptor.
 */

/*
 * 释放关系描述符上持有的锁。
 */
extern void UnlockRelation(Relation relation, LOCKMODE lockmode);
/*
 * Tests whether this backend holds the requested or stronger relation lock.
 */

/*
 * 测试当前后端是否持有所需或更强的关系锁。
 */
extern bool CheckRelationLockedByMe(Relation relation, LOCKMODE lockmode,
									bool orstronger);
/*
 * Tests whether this backend holds the requested or stronger OID relation lock.
 */

/*
 * 测试当前后端是否持有所需或更强的 OID 关系锁。
 */
extern bool CheckRelationOidLockedByMe(Oid relid, LOCKMODE lockmode,
									   bool orstronger);
/*
 * Reports whether another backend waits for a relation lock mode.
 */

/*
 * 报告是否有其他后端正在等待该关系锁模式。
 */
extern bool LockHasWaitersRelation(Relation relation, LOCKMODE lockmode);

/*
 * Acquires a session-level lock on a relation identifier.
 */

/*
 * 获取关系标识符上的会话级锁。
 */
extern void LockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode);
/*
 * Releases a session-level lock on a relation identifier.
 */

/*
 * 释放关系标识符上的会话级锁。
 */
extern void UnlockRelationIdForSession(LockRelId *relid, LOCKMODE lockmode);

/* Lock a relation for extension */

/*
 * 为扩展关系而锁定它。
 */
/*
 * Acquires the lock needed to extend a relation.
 */

/*
 * 获取扩展关系所需的锁。
 */
extern void LockRelationForExtension(Relation relation, LOCKMODE lockmode);
/* Lock a relation for extension */

/*
 * 释放关系扩展锁。
 */
extern void UnlockRelationForExtension(Relation relation, LOCKMODE lockmode);
/*
 * Attempts to acquire a relation-extension lock without waiting.
 */

/*
 * 尝试不等待地获取关系扩展锁。
 */
extern bool ConditionalLockRelationForExtension(Relation relation,
												LOCKMODE lockmode);
/*
 * Counts waiters for a relation-extension lock.
 */

/*
 * 统计关系扩展锁的等待者。
 */
extern int	RelationExtensionLockWaiterCount(Relation relation);

/* Lock to recompute pg_database.datfrozenxid in the current database */

/*
 * 锁定当前数据库中 pg_database.datfrozenxid 的重新计算。
 */
/*
 * Acquires the database frozen-ID lock in the specified mode.
 */

/*
 * 以指定模式获取数据库冻结 ID 锁。
 */
extern void LockDatabaseFrozenIds(LOCKMODE lockmode);

/* Lock a page (currently only used within indexes) */

/*
 * 锁定页面（目前仅在索引内部使用）。
 */
/*
 * Acquires a lock on one relation page.
 */

/*
 * 获取一个关系页上的锁。
 */
extern void LockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
/*
 * Attempts to acquire a page lock without waiting.
 */

/*
 * 尝试不等待地获取页面锁。
 */
extern bool ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);
/*
 * Releases a lock on one relation page.
 */

/*
 * 释放一个关系页上的锁。
 */
extern void UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode);

/* Lock a tuple (see heap_lock_tuple before assuming you understand this) */

/*
 * 锁定元组（请先参阅 heap_lock_tuple，再假定自己理解此操作）。
 */
/*
 * Acquires a heavyweight lock on a tuple.
 */

/*
 * 获取元组上的重量级锁。
 */
extern void LockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode);
/*
 * Attempts to acquire a tuple lock without waiting and may log failure.
 */

/*
 * 尝试不等待地获取元组锁，并可记录失败。
 */
extern bool ConditionalLockTuple(Relation relation, ItemPointer tid,
								 LOCKMODE lockmode, bool logLockFailure);
/*
 * Releases a heavyweight lock on a tuple.
 */

/*
 * 释放元组上的重量级锁。
 */
extern void UnlockTuple(Relation relation, ItemPointer tid, LOCKMODE lockmode);

/* Lock an XID (used to wait for a transaction to finish) */

/*
 * 锁定 XID（用于等待事务结束）。
 */
/*
 * Inserts a transaction ID into the transaction lock table.
 */

/*
 * 将事务 ID 插入事务锁表。
 */
extern void XactLockTableInsert(TransactionId xid);
/*
 * Removes a transaction ID from the transaction lock table.
 */

/*
 * 从事务锁表中移除事务 ID。
 */
extern void XactLockTableDelete(TransactionId xid);
/*
 * Waits for a transaction lock and records the waiting operation context.
 */

/*
 * 等待事务锁，并记录等待操作上下文。
 */
extern void XactLockTableWait(TransactionId xid, Relation rel,
							  ItemPointer ctid, XLTW_Oper oper);
/*
 * Attempts to wait for a transaction lock without blocking.
 */

/*
 * 尝试不阻塞地等待事务锁。
 */
extern bool ConditionalXactLockTableWait(TransactionId xid,
										 bool logLockFailure);

/* Lock VXIDs, specified by conflicting locktags */

/*
 * 锁定由冲突锁标签指定的 VXID。
 */
/*
 * Waits for lockers conflicting with one heap lock tag.
 */

/*
 * 等待与一个堆锁标签冲突的持锁者。
 */
extern void WaitForLockers(LOCKTAG heaplocktag, LOCKMODE lockmode, bool progress);
/*
 * Waits for lockers conflicting with any tag in a list.
 */

/*
 * 等待与列表中任一标签冲突的持锁者。
 */
extern void WaitForLockersMultiple(List *locktags, LOCKMODE lockmode, bool progress);

/* Lock an XID for tuple insertion (used to wait for an insertion to finish) */

/*
 * 为元组插入锁定 XID（用于等待插入完成）。
 */
/*
 * Acquires a speculative-insertion lock and returns its token.
 */

/*
 * 获取推测插入锁并返回其令牌。
 */
extern uint32 SpeculativeInsertionLockAcquire(TransactionId xid);
/*
 * Releases a speculative-insertion lock.
 */

/*
 * 释放推测插入锁。
 */
extern void SpeculativeInsertionLockRelease(TransactionId xid);
/*
 * Waits for a speculative insertion identified by transaction and token.
 */

/*
 * 等待由事务和令牌标识的推测插入。
 */
extern void SpeculativeInsertionWait(TransactionId xid, uint32 token);

/* Lock a general object (other than a relation) of the current database */

/*
 * 锁定当前数据库中的一般对象（关系除外）。
 */
/*
 * Acquires a lock on a database-local object.
 */

/*
 * 获取数据库本地对象上的锁。
 */
extern void LockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
							   LOCKMODE lockmode);
/*
 * Attempts to acquire a database-local object lock without waiting.
 */

/*
 * 尝试不等待地获取数据库本地对象锁。
 */
extern bool ConditionalLockDatabaseObject(Oid classid, Oid objid,
										  uint16 objsubid, LOCKMODE lockmode);
/*
 * Releases a lock on a database-local object.
 */

/*
 * 释放数据库本地对象上的锁。
 */
extern void UnlockDatabaseObject(Oid classid, Oid objid, uint16 objsubid,
								 LOCKMODE lockmode);

/* Lock a shared-across-databases object (other than a relation) */

/*
 * 锁定跨数据库共享的对象（关系除外）。
 */
/*
 * Acquires a lock on a shared database object.
 */

/*
 * 获取共享数据库对象上的锁。
 */
extern void LockSharedObject(Oid classid, Oid objid, uint16 objsubid,
							 LOCKMODE lockmode);
/*
 * Attempts to acquire a shared-object lock without waiting.
 */

/*
 * 尝试不等待地获取共享对象锁。
 */
extern bool ConditionalLockSharedObject(Oid classid, Oid objid, uint16 objsubid,
										LOCKMODE lockmode);
/*
 * Releases a lock on a shared database object.
 */

/*
 * 释放共享数据库对象上的锁。
 */
extern void UnlockSharedObject(Oid classid, Oid objid, uint16 objsubid,
							   LOCKMODE lockmode);

/*
 * Acquires a session-level lock on a shared database object.
 */

/*
 * 获取共享数据库对象上的会话级锁。
 */
extern void LockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
									   LOCKMODE lockmode);
/*
 * Releases a session-level lock on a shared database object.
 */

/*
 * 释放共享数据库对象上的会话级锁。
 */
extern void UnlockSharedObjectForSession(Oid classid, Oid objid, uint16 objsubid,
										 LOCKMODE lockmode);

/*
 * Acquires a session-level lock for a logical-replication apply transaction.
 */

/*
 * 获取逻辑复制应用事务的会话级锁。
 */
extern void LockApplyTransactionForSession(Oid suboid, TransactionId xid, uint16 objid,
										   LOCKMODE lockmode);
/*
 * Releases a session-level lock for a logical-replication apply transaction.
 */

/*
 * 释放逻辑复制应用事务的会话级锁。
 */
extern void UnlockApplyTransactionForSession(Oid suboid, TransactionId xid, uint16 objid,
											 LOCKMODE lockmode);

/* Describe a locktag for error messages */

/*
 * 为错误消息描述锁标签。
 */
/*
 * Appends a human-readable lock-tag description to a string buffer.
 */

/*
 * 将人类可读的锁标签描述追加到字符串缓冲区。
 */
extern void DescribeLockTag(StringInfo buf, const LOCKTAG *tag);

/*
 * Returns the display name for a lock-tag type.
 */

/*
 * 返回锁标签类型的显示名称。
 */
extern const char *GetLockNameFromTagType(uint16 locktag_type);

#endif							/* LMGR_H */
