/*-------------------------------------------------------------------------
 *
 * bulk_write.h
 *	  Efficiently and reliably populate a new relation
 *
 *	  高效且可靠地填充一个新关系。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bulk_write.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BULK_WRITE_H
#define BULK_WRITE_H

#include "storage/smgr.h"
#include "utils/rel.h"

/* Bulk writer state, contents are private to bulk_write.c */

/* 批量写入器状态，其内容由 bulk_write.c 私有。 */
typedef struct BulkWriteState BulkWriteState;

/*
 * Temporary buffer to hold a page to until it's written out. Use
 * smgr_bulk_get_buf() to reserve one of these.  This is a separate typedef to
 * distinguish it from other block-sized buffers passed around in the system.
 *
 * 在页面写出前临时保存它的缓冲区。使用 smgr_bulk_get_buf() 预留一个此类缓冲区。
 * 这是单独的 typedef，用于将它与系统中传递的其他块大小缓冲区区分开来。
 */
typedef PGIOAlignedBlock *BulkWriteBuffer;

/* forward declared from smgr.h */

/* 在 smgr.h 中前向声明。 */
struct SMgrRelationData;

/*
 * Start bulk writing for a relation and return its write state.
 * The function derives storage-manager context from the relation and initializes buffering.
 *
 * 为关系启动批量写入并返回其写入状态。
 * 该函数从关系派生存储管理器上下文并初始化缓冲。
 */
extern BulkWriteState *smgr_bulk_start_rel(Relation rel, ForkNumber forknum);

/*
 * Start bulk writing for an existing storage-manager relation.
 * The function initializes write state with the supplied WAL policy.
 *
 * 为已有的存储管理器关系启动批量写入。
 * 该函数使用给定的 WAL 策略初始化写入状态。
 */
extern BulkWriteState *smgr_bulk_start_smgr(struct SMgrRelationData *smgr, ForkNumber forknum, bool use_wal);

/*
 * Reserve and return a temporary bulk-write buffer.
 * The function obtains a page-sized buffer from the writer's managed pool.
 *
 * 预留并返回一个临时批量写入缓冲区。
 * 该函数从写入器管理的池中取得一个页面大小的缓冲区。
 */
extern BulkWriteBuffer smgr_bulk_get_buf(BulkWriteState *bulkstate);

/*
 * Write one buffered page at the specified block number.
 * The function persists the buffer and applies the requested standard-page handling.
 *
 * 将一个缓冲页面写入指定块号。
 * 该函数持久化缓冲区，并应用请求的标准页面处理。
 */
extern void smgr_bulk_write(BulkWriteState *bulkstate, BlockNumber blocknum, BulkWriteBuffer buf, bool page_std);

/*
 * Finish bulk writing and release the writer state.
 * The function flushes remaining work and performs final storage cleanup.
 *
 * 完成批量写入并释放写入器状态。
 * 该函数刷出剩余工作并执行最终的存储清理。
 */
extern void smgr_bulk_finish(BulkWriteState *bulkstate);

#endif							/* BULK_WRITE_H */
