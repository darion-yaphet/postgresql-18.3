/*------------------------------------------------------------------------
 *
 * xlogarchive.h
 *		Prototypes for WAL archives in the backend
 *
 * 说明 WAL 归档文件的恢复、状态通知或清理处理。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogarchive.h
 *
 * 说明 WAL 归档文件的恢复、状态通知或清理处理。
 *
 *------------------------------------------------------------------------
 */

#ifndef XLOG_ARCHIVE_H
#define XLOG_ARCHIVE_H

#include "access/xlogdefs.h"

/*
 * Function: RestoreArchivedFile.
 * Purpose: Obtains, checks, or restores the WAL state represented by restore archived file.
 * Core flow: It examines the requested WAL files or state, performs the requested read or restore work, and returns the result.
 *
 * 函数：RestoreArchivedFile。
 * 作用：获取、检查或恢复 restore archived file 所表示的 WAL 状态。
 * 核心流程：它检查请求的 WAL 文件或状态，完成读取或恢复工作，并返回结果。
 */
extern bool RestoreArchivedFile(char *path, const char *xlogfname,
								const char *recovername, off_t expectedSize,
								bool cleanupEnabled);
/*
 * Function: ExecuteRecoveryCommand.
 * Purpose: Performs the WAL operation represented by execute recovery command.
 * Core flow: It uses the supplied context to process the requested WAL work and reports or returns the result.
 *
 * 函数：ExecuteRecoveryCommand。
 * 作用：执行 execute recovery command 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理请求的 WAL 工作，并报告或返回结果。
 */
extern void ExecuteRecoveryCommand(const char *command, const char *commandName,
								   bool failOnSignal, uint32 wait_event_info);
/*
 * Function: KeepFileRestoredFromArchive.
 * Purpose: Performs the WAL operation represented by keep file restored from archive.
 * Core flow: It uses the supplied context to process the requested WAL work and reports or returns the result.
 *
 * 函数：KeepFileRestoredFromArchive。
 * 作用：执行 keep file restored from archive 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理请求的 WAL 工作，并报告或返回结果。
 */
extern void KeepFileRestoredFromArchive(const char *path, const char *xlogfname);
/*
 * Function: XLogArchiveNotify.
 * Purpose: Performs the WAL operation represented by xlog archive notify.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveNotify。
 * 作用：执行 xlog archive notify 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogArchiveNotify(const char *xlog);
/*
 * Function: XLogArchiveNotifySeg.
 * Purpose: Performs the WAL operation represented by xlog archive notify seg.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveNotifySeg。
 * 作用：执行 xlog archive notify seg 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogArchiveNotifySeg(XLogSegNo segno, TimeLineID tli);
/*
 * Function: XLogArchiveForceDone.
 * Purpose: Performs the WAL operation represented by xlog archive force done.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveForceDone。
 * 作用：执行 xlog archive force done 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogArchiveForceDone(const char *xlog);
/*
 * Function: XLogArchiveCheckDone.
 * Purpose: Performs the WAL operation represented by xlog archive check done.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveCheckDone。
 * 作用：执行 xlog archive check done 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern bool XLogArchiveCheckDone(const char *xlog);
/*
 * Function: XLogArchiveIsBusy.
 * Purpose: Performs the WAL operation represented by xlog archive is busy.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveIsBusy。
 * 作用：执行 xlog archive is busy 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern bool XLogArchiveIsBusy(const char *xlog);
/*
 * Function: XLogArchiveIsReady.
 * Purpose: Performs the WAL operation represented by xlog archive is ready.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveIsReady。
 * 作用：执行 xlog archive is ready 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern bool XLogArchiveIsReady(const char *xlog);
/*
 * Function: XLogArchiveIsReadyOrDone.
 * Purpose: Performs the WAL operation represented by xlog archive is ready or done.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveIsReadyOrDone。
 * 作用：执行 xlog archive is ready or done 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern bool XLogArchiveIsReadyOrDone(const char *xlog);
/*
 * Function: XLogArchiveCleanup.
 * Purpose: Performs the WAL operation represented by xlog archive cleanup.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogArchiveCleanup。
 * 作用：执行 xlog archive cleanup 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogArchiveCleanup(const char *xlog);

#endif							/* XLOG_ARCHIVE_H */
