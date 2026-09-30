/*-------------------------------------------------------------------------
 *
 * standbydefs.h
 *	   Frontend exposed definitions for hot standby mode.
 *
 *
 *	   面向前端公开的热备模式定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/standbydefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STANDBYDEFS_H
#define STANDBYDEFS_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/lockdefs.h"
#include "storage/sinval.h"

/* Recovery handlers for the Standby Rmgr (RM_STANDBY_ID) */

/* 备库恢复管理器（RM_STANDBY_ID）的恢复处理程序。 */

/* Replay a standby WAL record.
 * The handler decodes the record type and applies the corresponding standby
 * lock, snapshot, or invalidation action.
 *
 * 重放一条备库 WAL 记录。
 * 处理程序解码记录类型，并应用相应的备库锁、快照或失效操作。
 */
extern void standby_redo(XLogReaderState *record);

/* Describe a standby WAL record for diagnostics.
 * The routine decodes record data and appends a human-readable description to
 * the supplied buffer.
 *
 * 为诊断描述一条备库 WAL 记录。
 * 此例程解码记录数据，并将人类可读的描述追加到给定缓冲区。
 */
extern void standby_desc(StringInfo buf, XLogReaderState *record);

/* Return the symbolic name for a standby WAL record subtype.
 * The info code is mapped to its stable textual identifier for display.
 *
 * 返回备库 WAL 记录子类型的符号名称。
 * info 代码会映射为稳定的文本标识符以供显示。
 */
extern const char *standby_identify(uint8 info);

/* Describe invalidation payloads in a standby WAL record.
 * The routine formats each message using its database, tablespace, and
 * relcache-init-file context.
 *
 * 描述备库 WAL 记录中的失效负载。
 * 此例程使用数据库、表空间和 relcache 初始化文件上下文格式化每条消息。
 */
extern void standby_desc_invalidations(StringInfo buf,
									   int nmsgs, SharedInvalidationMessage *msgs,
									   Oid dbId, Oid tsId,
									   bool relcacheInitFileInval);

/*
 * XLOG message types
 */

/*
 * XLOG 消息类型。
 */
#define XLOG_STANDBY_LOCK			0x00
#define XLOG_RUNNING_XACTS			0x10
#define XLOG_INVALIDATIONS			0x20

typedef struct xl_standby_locks
{
	int			nlocks;			/* number of entries in locks array */

									/* locks 数组中的条目数。 */
	xl_standby_lock locks[FLEXIBLE_ARRAY_MEMBER];
} xl_standby_locks;

/*
 * When we write running xact data to WAL, we use this structure.
 */

/*
 * 将运行中事务数据写入 WAL 时使用此结构。
 */
typedef struct xl_running_xacts
{
	int			xcnt;			/* # of xact ids in xids[] */

									/* xids[] 中的事务 ID 数量。 */
	int			subxcnt;		/* # of subxact ids in xids[] */

									/* xids[] 中的子事务 ID 数量。 */
	bool		subxid_overflow;	/* snapshot overflowed, subxids missing */

									/* 快照已溢出，子事务 ID 缺失。 */
	TransactionId nextXid;		/* xid from TransamVariables->nextXid */

									/* 来自 TransamVariables->nextXid 的 xid。 */
	TransactionId oldestRunningXid; /* *not* oldestXmin */

									/* 不是 oldestXmin。 */
	TransactionId latestCompletedXid;	/* so we can set xmax */

									/* 因此我们可以设置 xmax。 */

	TransactionId xids[FLEXIBLE_ARRAY_MEMBER];
} xl_running_xacts;

/*
 * Invalidations for standby, currently only when transactions without an
 * assigned xid commit.
 */

/*
 * 备库的失效信息，目前仅在未分配 xid 的事务提交时使用。
 */
typedef struct xl_invalidations
{
	Oid			dbId;			/* MyDatabaseId */

									/* MyDatabaseId。 */
	Oid			tsId;			/* MyDatabaseTableSpace */

									/* MyDatabaseTableSpace。 */
	bool		relcacheInitFileInval;	/* invalidate relcache init files */

									/* 使 relcache 初始化文件失效。 */
	int			nmsgs;			/* number of shared inval msgs */

									/* 共享失效消息数量。 */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_invalidations;

#define MinSizeOfInvalidations offsetof(xl_invalidations, msgs)

#endif							/* STANDBYDEFS_H */
