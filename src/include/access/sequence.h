/*-------------------------------------------------------------------------
 *
 * sequence.h
 *	  Generic routines for sequence-related code.
 *
 *
 *	  与序列相关代码的通用例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sequence.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ACCESS_SEQUENCE_H
#define ACCESS_SEQUENCE_H

#include "storage/lockdefs.h"
#include "utils/relcache.h"

/* Open a sequence relation with the requested lock.
 * The routine resolves the sequence OID, acquires the lock, and returns the
 * relation descriptor.
 *
 * 使用请求的锁打开一个序列关系。
 * 此例程解析序列 OID、获取锁，并返回关系描述符。
 */
extern Relation sequence_open(Oid relationId, LOCKMODE lockmode);

/* Close a sequence relation and release its lock.
 * The routine releases the descriptor and applies the specified lock-release
 * mode.
 *
 * 关闭一个序列关系并释放其锁。
 * 此例程释放描述符，并应用指定的锁释放模式。
 */
extern void sequence_close(Relation relation, LOCKMODE lockmode);

#endif							/* ACCESS_SEQUENCE_H */
