/*-------------------------------------------------------------------------
 *
 * sync.h
 *	  File synchronization management code.
 *
 *
 *	  文件同步管理代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sync.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYNC_H
#define SYNC_H

#include "storage/relfilelocator.h"

/*
 * Type of sync request.  These are used to manage the set of pending
 * requests to call a sync handler's sync or unlink functions at the next
 * checkpoint.
 */

/*
 * 同步请求的类型。这些类型用于管理在下一个检查点调用同步处理程序的 sync
 * 或 unlink 函数的待处理请求集合。
 */
typedef enum SyncRequestType
{
	SYNC_REQUEST,				/* schedule a call of sync function */

								/* 安排调用同步函数。 */
	SYNC_UNLINK_REQUEST,		/* schedule a call of unlink function */

								/* 安排调用取消链接函数。 */
	SYNC_FORGET_REQUEST,		/* forget all calls for a tag */

								/* 忘记某个标记的所有调用。 */
	SYNC_FILTER_REQUEST,		/* forget all calls satisfying match fn */

								/* 忘记所有满足匹配函数的调用。 */
} SyncRequestType;

/*
 * Which set of functions to use to handle a given request.  The values of
 * the enumerators must match the indexes of the function table in sync.c.
 */

/*
 * 用于处理给定请求的函数集合。枚举值必须与 sync.c 中函数表的索引匹配。
 */
typedef enum SyncRequestHandler
{
	SYNC_HANDLER_MD = 0,
	SYNC_HANDLER_CLOG,
	SYNC_HANDLER_COMMIT_TS,
	SYNC_HANDLER_MULTIXACT_OFFSET,
	SYNC_HANDLER_MULTIXACT_MEMBER,
	SYNC_HANDLER_NONE,
} SyncRequestHandler;

/*
 * A tag identifying a file.  Currently it has the members required for md.c's
 * usage, but sync.c has no knowledge of the internal structure, and it is
 * liable to change as required by future handlers.
 */

/*
 * 标识文件的标记。目前它拥有 md.c 使用所需的成员，但 sync.c 不了解内部结构，
 * 并且它可能随未来处理程序的需要而变化。
 */
typedef struct FileTag
{
	int16		handler;		/* SyncRequestHandler value, saving space */

								/* SyncRequestHandler 值，以节省空间。 */
	int16		forknum;		/* ForkNumber, saving space */

								/* ForkNumber，以节省空间。 */
	RelFileLocator rlocator;
	uint64		segno;
} FileTag;

/* Initialize the file synchronization manager.
 * Startup creates request queues and handler state used by checkpoint work.
 *
 * 初始化文件同步管理器。
 * 启动过程创建检查点工作使用的请求队列和处理程序状态。
 */
extern void InitSync(void);

/* Prepare pending synchronization work before a checkpoint.
 * The routine establishes the pre-checkpoint state needed to collect and
 * order file requests.
 *
 * 在检查点前准备待处理的同步工作。
 * 此例程建立收集和排序文件请求所需的检查点前状态。
 */
extern void SyncPreCheckpoint(void);

/* Complete synchronization bookkeeping after a checkpoint.
 * The manager advances or clears request state once checkpoint processing has
 * finished.
 *
 * 在检查点后完成同步记账。
 * 一旦检查点处理完成，管理器会推进或清除请求状态。
 */
extern void SyncPostCheckpoint(void);

/* Process queued file synchronization requests.
 * The dispatcher selects each request handler and invokes its sync, unlink,
 * forget, or filter action.
 *
 * 处理排队的文件同步请求。
 * 分派器选择每个请求处理程序，并调用其同步、取消链接、忘记或筛选操作。
 */
extern void ProcessSyncRequests(void);

/* Record a synchronization request without returning status.
 * The request is placed in the pending set for later checkpoint processing.
 *
 * 记录同步请求而不返回状态。
 * 该请求被放入待处理集合，以供稍后的检查点处理。
 */
extern void RememberSyncRequest(const FileTag *ftag, SyncRequestType type);

/* Register a synchronization request and report the result.
 * The routine adds the tagged action to the pending set and can retry after a
 * registration error when requested.
 *
 * 注册同步请求并报告结果。
 * 此例程将带标记的操作加入待处理集合，并可在请求时于注册错误后重试。
 */
extern bool RegisterSyncRequest(const FileTag *ftag, SyncRequestType type,
								bool retryOnError);

#endif							/* SYNC_H */
