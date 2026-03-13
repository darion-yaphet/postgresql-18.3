/*-------------------------------------------------------------------------
 *
 * pgarch.h
 *	  Exports from postmaster/pgarch.c.
 *	  来自 postmaster/pgarch.c 的导出内容。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/pgarch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGARCH_H
#define _PGARCH_H

/* ----------
 * Archiver control info.
 *
 * We expect that archivable files within pg_wal will have names between
 * MIN_XFN_CHARS and MAX_XFN_CHARS in length, consisting only of characters
 * appearing in VALID_XFN_CHARS.  The status files in archive_status have
 * corresponding names with ".ready" or ".done" appended.
 *
 * 我们期望 pg_wal 中的可归档文件名称长度在 MIN_XFN_CHARS 和 MAX_XFN_CHARS
 * 之间，并且只包含 VALID_XFN_CHARS 中的字符。archive_status 中的状态
 * 文件具有相应的名称，并附加了 ".ready" 或 ".done"。
 * ----------
 */
#define MIN_XFN_CHARS	16
#define MAX_XFN_CHARS	40
#define VALID_XFN_CHARS "0123456789ABCDEF.history.backup.partial"

extern Size PgArchShmemSize(void);
extern void PgArchShmemInit(void);
extern bool PgArchCanRestart(void);
pg_noreturn extern void PgArchiverMain(const void *startup_data, size_t startup_data_len);
extern void PgArchWakeup(void);
extern void PgArchForceDirScan(void);

#endif							/* _PGARCH_H */
