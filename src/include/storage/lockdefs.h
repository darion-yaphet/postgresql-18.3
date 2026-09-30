/*-------------------------------------------------------------------------
 *
 * lockdefs.h
 *	   Frontend exposed parts of postgres' low level lock mechanism
 *
 * The split between lockdefs.h and lock.h is not very principled. This file
 * contains definition that have to (indirectly) be available when included by
 * FRONTEND code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lockdefs.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * lockdefs.h 与 lock.h 之间的划分并不十分严格。本文件包含在 FRONTEND 代码间接包含时必须可用的定义。
 */
#ifndef LOCKDEFS_H_
#define LOCKDEFS_H_

/*
 * LOCKMODE is an integer (1..N) indicating a lock type.  LOCKMASK is a bit
 * mask indicating a set of held or requested lock types (the bit 1<<mode
 * corresponds to a particular lock mode).
 */

/*
 * LOCKMODE 是表示锁类型的整数（1..N）。LOCKMASK 是表示已持有或已请求锁类型集合的位掩码（位
 * 1<<mode 对应特定锁模式）。
 */
typedef int LOCKMASK;
typedef int LOCKMODE;

/*
 * These are the valid values of type LOCKMODE for all the standard lock
 * methods (both DEFAULT and USER).
 */

/*
 * 这些是所有标准锁方法（DEFAULT 和 USER）的有效 LOCKMODE 值。
 */

/* NoLock is not a lock mode, but a flag value meaning "don't get a lock" */

/*
 * NoLock 不是锁模式，而是表示“不获取锁”的标志值。
 */
#define NoLock					0

#define AccessShareLock			1	/* SELECT */

/* SELECT。 */
#define RowShareLock			2	/* SELECT FOR UPDATE/FOR SHARE */

/* SELECT FOR UPDATE/FOR SHARE。 */
#define RowExclusiveLock		3	/* INSERT, UPDATE, DELETE */

/* INSERT、UPDATE、DELETE。 */
#define ShareUpdateExclusiveLock 4	/* VACUUM (non-FULL), ANALYZE, CREATE
									 * INDEX CONCURRENTLY */

/*
 * 非 FULL VACUUM、ANALYZE、CREATE INDEX CONCURRENTLY。
 */
#define ShareLock				5	/* CREATE INDEX (WITHOUT CONCURRENTLY) */

/*
 * CREATE INDEX（不使用 CONCURRENTLY）。
 */
#define ShareRowExclusiveLock	6	/* like EXCLUSIVE MODE, but allows ROW
									 * SHARE */

/*
 * 类似 EXCLUSIVE MODE，但允许 ROW SHARE。
 */
#define ExclusiveLock			7	/* blocks ROW SHARE/SELECT...FOR UPDATE */

/*
 * 阻塞 ROW SHARE/SELECT...FOR UPDATE。
 */
#define AccessExclusiveLock		8	/* ALTER TABLE, DROP TABLE, VACUUM FULL,
									 * and unqualified LOCK TABLE */

/*
 * ALTER TABLE、DROP TABLE、VACUUM FULL 和不带模式的 LOCK TABLE。
 */

#define MaxLockMode				8	/* highest standard lock mode */

/*
 * 最高标准锁模式。
 */

/* See README.tuplock section "Locking to write inplace-updated tables" */

/*
 * 参见 README.tuplock 中“Locking to write inplace-updated tables”一节。
 */
#define InplaceUpdateTupleLock ExclusiveLock

/* WAL representation of an AccessExclusiveLock on a table */

/*
 * 表上 AccessExclusiveLock 的 WAL 表示。
 */
typedef struct xl_standby_lock
{
	TransactionId xid;			/* xid of holder of AccessExclusiveLock */

/*
 * AccessExclusiveLock 持有者的 xid。
 */
	Oid			dbOid;			/* DB containing table */

/*
 * 包含该表的数据库。
 */
	Oid			relOid;			/* OID of table */

/*
 * 表的 OID。
 */
} xl_standby_lock;

#endif							/* LOCKDEFS_H_ */
