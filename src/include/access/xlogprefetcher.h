/*-------------------------------------------------------------------------
 *
 * xlogprefetcher.h
 *		Declarations for the recovery prefetching module.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 *
 * Portions Copyright (c) 2022-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogprefetcher.h
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 *-------------------------------------------------------------------------
 */
#ifndef XLOGPREFETCHER_H
#define XLOGPREFETCHER_H

#include "access/xlogdefs.h"
#include "access/xlogreader.h"
#include "access/xlogrecord.h"

/* GUCs */

/* GUC 参数。 */
extern PGDLLIMPORT int recovery_prefetch;

/* Possible values for recovery_prefetch */

/* recovery_prefetch 的可能取值。 */
typedef enum
{
	RECOVERY_PREFETCH_OFF,
	RECOVERY_PREFETCH_ON,
	RECOVERY_PREFETCH_TRY,
}			RecoveryPrefetchValue;

struct XLogPrefetcher;
typedef struct XLogPrefetcher XLogPrefetcher;


/*
 * Function: XLogPrefetchReconfigure.
 * Purpose: Performs the WAL operation represented by xlog prefetch reconfigure.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetchReconfigure。
 * 作用：执行 xlog prefetch reconfigure 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogPrefetchReconfigure(void);

/*
 * Function: XLogPrefetchShmemSize.
 * Purpose: Performs the WAL operation represented by xlog prefetch shmem size.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetchShmemSize。
 * 作用：执行 xlog prefetch shmem size 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern size_t XLogPrefetchShmemSize(void);
/*
 * Function: XLogPrefetchShmemInit.
 * Purpose: Performs the WAL operation represented by xlog prefetch shmem init.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetchShmemInit。
 * 作用：执行 xlog prefetch shmem init 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogPrefetchShmemInit(void);

/*
 * Function: XLogPrefetchResetStats.
 * Purpose: Performs the WAL operation represented by xlog prefetch reset stats.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetchResetStats。
 * 作用：执行 xlog prefetch reset stats 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogPrefetchResetStats(void);

/*
 * Function: XLogPrefetcherAllocate.
 * Purpose: Performs the WAL operation represented by xlog prefetcher allocate.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetcherAllocate。
 * 作用：执行 xlog prefetcher allocate 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogPrefetcher *XLogPrefetcherAllocate(XLogReaderState *reader);
/*
 * Function: XLogPrefetcherFree.
 * Purpose: Performs the WAL operation represented by xlog prefetcher free.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetcherFree。
 * 作用：执行 xlog prefetcher free 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogPrefetcherFree(XLogPrefetcher *prefetcher);

/*
 * Function: XLogPrefetcherGetReader.
 * Purpose: Performs the WAL operation represented by xlog prefetcher get reader.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetcherGetReader。
 * 作用：执行 xlog prefetcher get reader 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogReaderState *XLogPrefetcherGetReader(XLogPrefetcher *prefetcher);

/*
 * Function: XLogPrefetcherBeginRead.
 * Purpose: Performs the WAL operation represented by xlog prefetcher begin read.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetcherBeginRead。
 * 作用：执行 xlog prefetcher begin read 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogPrefetcherBeginRead(XLogPrefetcher *prefetcher,
									XLogRecPtr recPtr);

/*
 * Function: XLogPrefetcherReadRecord.
 * Purpose: Performs the WAL operation represented by xlog prefetcher read record.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetcherReadRecord。
 * 作用：执行 xlog prefetcher read record 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogRecord *XLogPrefetcherReadRecord(XLogPrefetcher *prefetcher,
											char **errmsg);

/*
 * Function: XLogPrefetcherComputeStats.
 * Purpose: Performs the WAL operation represented by xlog prefetcher compute stats.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogPrefetcherComputeStats。
 * 作用：执行 xlog prefetcher compute stats 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogPrefetcherComputeStats(XLogPrefetcher *prefetcher);

#endif
