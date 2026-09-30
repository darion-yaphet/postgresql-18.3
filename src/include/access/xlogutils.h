/*
 * xlogutils.h
 *
 * 说明预写式日志（WAL）的读取、记录、恢复或状态约束。
 *
 * Utilities for replaying WAL records.
 *
 * 说明 WAL 记录的头部、块数据或解码格式。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogutils.h
 *
 * 说明预写式日志（WAL）的读取、记录、恢复或状态约束。
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "access/xlogreader.h"
#include "storage/bufmgr.h"

/* GUC variable */

/* GUC 变量。 */
extern PGDLLIMPORT bool ignore_invalid_pages;

/*
 * Prior to 8.4, all activity during recovery was carried out by the startup
 * process. This local variable continues to be used in many parts of the
 * code to indicate actions taken by RecoveryManagers. Other processes that
 * potentially perform work during recovery should check RecoveryInProgress().
 * See XLogCtl notes in xlog.c.
 *
 * 说明 WAL 恢复、暂停、提升或结束状态。
 */
extern PGDLLIMPORT bool InRecovery;

/*
 * Like InRecovery, standbyState is only valid in the startup process.
 * In all other processes it will have the value STANDBY_DISABLED (so
 * InHotStandby will read as false).
 *
 * 说明 WAL 恢复、暂停、提升或结束状态。
 *
 * In DISABLED state, we're performing crash recovery or hot standby was
 * disabled in postgresql.conf.
 *
 * 说明 WAL 恢复、暂停、提升或结束状态。
 *
 * In INITIALIZED state, we've run InitRecoveryTransactionEnvironment, but
 * we haven't yet processed a RUNNING_XACTS or shutdown-checkpoint WAL record
 * to initialize our primary-transaction tracking system.
 *
 * 说明 WAL 记录的头部、块数据或解码格式。
 *
 * When the transaction tracking is initialized, we enter the SNAPSHOT_PENDING
 * state. The tracked information might still be incomplete, so we can't allow
 * connections yet, but redo functions must update the in-memory state when
 * appropriate.
 *
 * 说明 WAL 重做时的页面读取、关系操作或错误处理。
 *
 * In SNAPSHOT_READY mode, we have full knowledge of transactions that are
 * (or were) running on the primary at the current WAL location. Snapshots
 * can be taken, and read-only queries can be run.
 *
 * 说明预写式日志（WAL）的读取、记录、恢复或状态约束。
 */
typedef enum
{
	STANDBY_DISABLED,
	STANDBY_INITIALIZED,
	STANDBY_SNAPSHOT_PENDING,
	STANDBY_SNAPSHOT_READY,
} HotStandbyState;

extern PGDLLIMPORT HotStandbyState standbyState;

#define InHotStandby (standbyState >= STANDBY_SNAPSHOT_PENDING)


/*
 * Function: XLogHaveInvalidPages.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog have invalid pages.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogHaveInvalidPages。
 * 作用：执行 xlog have invalid pages 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern bool XLogHaveInvalidPages(void);
/*
 * Function: XLogCheckInvalidPages.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog check invalid pages.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogCheckInvalidPages。
 * 作用：执行 xlog check invalid pages 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogCheckInvalidPages(void);

/*
 * Function: XLogDropRelation.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog drop relation.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogDropRelation。
 * 作用：执行 xlog drop relation 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogDropRelation(RelFileLocator rlocator, ForkNumber forknum);
/*
 * Function: XLogDropDatabase.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog drop database.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogDropDatabase。
 * 作用：执行 xlog drop database 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogDropDatabase(Oid dbid);
/*
 * Function: XLogTruncateRelation.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog truncate relation.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogTruncateRelation。
 * 作用：执行 xlog truncate relation 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogTruncateRelation(RelFileLocator rlocator, ForkNumber forkNum,
								 BlockNumber nblocks);

/* Result codes for XLogReadBufferForRedo[Extended] */

/* XLogReadBufferForRedo[Extended] 的结果代码。 */
typedef enum
{
	BLK_NEEDS_REDO,				/* changes from WAL record need to be applied */

	/* 需要应用 WAL 记录中的更改。 */
	BLK_DONE,					/* block is already up-to-date */

	/* 块已经是最新状态。 */
	BLK_RESTORED,				/* block was restored from a full-page image */

	/* 块已从完整页面镜像恢复。 */
	BLK_NOTFOUND,				/* block was not found (and hence does not
								 * need to be replayed) */
} XLogRedoAction;

/* Private data of the read_local_xlog_page_no_wait callback. */

/* read_local_xlog_page_no_wait 回调的私有数据。 */
typedef struct ReadLocalXLogPageNoWaitPrivate
{
	bool		end_of_wal;		/* true, when end of WAL is reached */

	/* 到达 WAL 末尾时为 true。 */
} ReadLocalXLogPageNoWaitPrivate;

/*
 * Function: XLogReadBufferForRedo.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog read buffer for redo.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReadBufferForRedo。
 * 作用：执行 xlog read buffer for redo 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern XLogRedoAction XLogReadBufferForRedo(XLogReaderState *record,
											uint8 block_id, Buffer *buf);
/*
 * Function: XLogInitBufferForRedo.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog init buffer for redo.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogInitBufferForRedo。
 * 作用：执行 xlog init buffer for redo 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern Buffer XLogInitBufferForRedo(XLogReaderState *record, uint8 block_id);
/*
 * Function: XLogReadBufferForRedoExtended.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog read buffer for redo extended.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReadBufferForRedoExtended。
 * 作用：执行 xlog read buffer for redo extended 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern XLogRedoAction XLogReadBufferForRedoExtended(XLogReaderState *record,
													uint8 block_id,
													ReadBufferMode mode, bool get_cleanup_lock,
													Buffer *buf);

/*
 * Function: XLogReadBufferExtended.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog read buffer extended.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReadBufferExtended。
 * 作用：执行 xlog read buffer extended 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern Buffer XLogReadBufferExtended(RelFileLocator rlocator, ForkNumber forknum,
									 BlockNumber blkno, ReadBufferMode mode,
									 Buffer recent_buffer);

/*
 * Function: CreateFakeRelcacheEntry.
 * Purpose: Performs the WAL operation represented by create fake relcache entry.
 * Core flow: It uses the supplied context to process WAL data and returns or records the result.
 *
 * 函数：CreateFakeRelcacheEntry。
 * 作用：执行 create fake relcache entry 所表示的 WAL 操作。
 * 核心流程：它使用给定上下文处理 WAL 数据，并返回或记录结果。
 */
extern Relation CreateFakeRelcacheEntry(RelFileLocator rlocator);
/*
 * Function: FreeFakeRelcacheEntry.
 * Purpose: Completes or releases the WAL work represented by free fake relcache entry.
 * Core flow: It finalizes processing and releases or preserves the state required by subsequent recovery work.
 *
 * 函数：FreeFakeRelcacheEntry。
 * 作用：完成或释放 free fake relcache entry 所表示的 WAL 工作。
 * 核心流程：它结束处理，并释放或保留后续恢复工作所需的状态。
 */
extern void FreeFakeRelcacheEntry(Relation fakerel);

/*
 * Function: read_local_xlog_page.
 * Purpose: Performs the WAL reading or recovery operation represented by read local xlog page.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：read_local_xlog_page。
 * 作用：执行 read local xlog page 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern int	read_local_xlog_page(XLogReaderState *state,
								 XLogRecPtr targetPagePtr, int reqLen,
								 XLogRecPtr targetRecPtr, char *cur_page);
/*
 * Function: read_local_xlog_page_no_wait.
 * Purpose: Performs the WAL reading or recovery operation represented by read local xlog page no wait.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：read_local_xlog_page_no_wait。
 * 作用：执行 read local xlog page no wait 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern int	read_local_xlog_page_no_wait(XLogReaderState *state,
										 XLogRecPtr targetPagePtr, int reqLen,
										 XLogRecPtr targetRecPtr,
										 char *cur_page);
/*
 * Function: wal_segment_open.
 * Purpose: Performs the WAL reading or recovery operation represented by wal segment open.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：wal_segment_open。
 * 作用：执行 wal segment open 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void wal_segment_open(XLogReaderState *state,
							 XLogSegNo nextSegNo,
							 TimeLineID *tli_p);
/*
 * Function: wal_segment_close.
 * Purpose: Performs the WAL reading or recovery operation represented by wal segment close.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：wal_segment_close。
 * 作用：执行 wal segment close 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void wal_segment_close(XLogReaderState *state);

/*
 * Function: XLogReadDetermineTimeline.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog read determine timeline.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogReadDetermineTimeline。
 * 作用：执行 xlog read determine timeline 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogReadDetermineTimeline(XLogReaderState *state,
									  XLogRecPtr wantPage,
									  uint32 wantLength,
									  TimeLineID currTLI);

/*
 * Function: WALReadRaiseError.
 * Purpose: Performs the WAL reading or recovery operation represented by walread raise error.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：WALReadRaiseError。
 * 作用：执行 walread raise error 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void WALReadRaiseError(WALReadError *errinfo);

#endif
