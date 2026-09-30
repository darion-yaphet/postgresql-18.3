/*
 * xlogdefs.h
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 *
 * Postgres write-ahead log manager record pointer and
 * timeline number definitions
 *
 * Postgres 预写式日志管理器的记录指针与时间线编号定义。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogdefs.h
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
#ifndef XLOG_DEFS_H
#define XLOG_DEFS_H

#include <fcntl.h>				/* need open() flags */

/* 需要 open() 标志。 */

/*
 * Pointer to a location in the XLOG.  These pointers are 64 bits wide,
 * because we don't want them ever to overflow.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
typedef uint64 XLogRecPtr;

/*
 * Zero is used indicate an invalid pointer. Bootstrap skips the first possible
 * WAL segment, initializing the first WAL page at WAL segment size, so no XLOG
 * record can begin at zero.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
#define InvalidXLogRecPtr		0
#define XLogRecPtrIsValid(r)	((r) != InvalidXLogRecPtr)
#define XLogRecPtrIsInvalid(r)	((r) == InvalidXLogRecPtr)

/*
 * First LSN to use for "fake" LSNs.
 *
 * 用于“伪”LSN 的第一个 LSN。
 *
 * Values smaller than this can be used for special per-AM purposes.
 *
 * 小于此值的取值可供各访问方法（AM）用于特殊用途。
 */
#define FirstNormalUnloggedLSN	((XLogRecPtr) 1000)

/*
 * Handy macro for printing XLogRecPtr in conventional format, e.g.,
 *
 * 用于以惯用格式打印 XLogRecPtr 的便捷宏，例如：
 *
 * printf("%X/%X", LSN_FORMAT_ARGS(lsn));
 *
 * 示例：printf("%X/%X", LSN_FORMAT_ARGS(lsn));
 */
#define LSN_FORMAT_ARGS(lsn) (AssertVariableIsOfTypeMacro((lsn), XLogRecPtr), (uint32) ((lsn) >> 32)), ((uint32) (lsn))

/*
 * XLogSegNo - physical log file sequence number.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
typedef uint64 XLogSegNo;

/*
 * TimeLineID (TLI) - identifies different database histories to prevent
 * confusion after restoring a prior state of a database installation.
 * TLI does not change in a normal stop/restart of the database (including
 * crash-and-recover cases); but we must assign a new TLI after doing
 * a recovery to a prior state, a/k/a point-in-time recovery.  This makes
 * the new WAL logfile sequence we generate distinguishable from the
 * sequence that was generated in the previous incarnation.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
typedef uint32 TimeLineID;

/*
 * Replication origin id - this is located in this file to avoid having to
 * include origin.h in a bunch of xlog related places.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
typedef uint16 RepOriginId;

/*
 * This chunk of hackery attempts to determine which file sync methods
 * are available on the current platform, and to choose an appropriate
 * default method.
 *
 * 这段技巧性代码尝试判断当前平台上有哪些文件同步方法可用，并选择一个合适的默认方法。
 *
 * Note that we define our own O_DSYNC on Windows, but not O_SYNC.
 *
 * 注意：在 Windows 上我们自行定义了 O_DSYNC，但没有定义 O_SYNC。
 */
#if defined(PLATFORM_DEFAULT_WAL_SYNC_METHOD)
#define DEFAULT_WAL_SYNC_METHOD		PLATFORM_DEFAULT_WAL_SYNC_METHOD
#elif defined(O_DSYNC) && (!defined(O_SYNC) || O_DSYNC != O_SYNC)
#define DEFAULT_WAL_SYNC_METHOD		WAL_SYNC_METHOD_OPEN_DSYNC
#else
#define DEFAULT_WAL_SYNC_METHOD		WAL_SYNC_METHOD_FDATASYNC
#endif

#endif							/* XLOG_DEFS_H */
