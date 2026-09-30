/*-------------------------------------------------------------------------
 *
 * syncscan.h
 *    POSTGRES synchronous scan support functions.
 *
 *
 *    POSTGRES 同步扫描支持函数。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/syncscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYNCSCAN_H
#define SYNCSCAN_H

#include "storage/block.h"
#include "utils/relcache.h"

/* GUC variables */

/* GUC 变量。 */
#ifdef TRACE_SYNCSCAN
extern PGDLLIMPORT bool trace_syncscan;
#endif

/* Report a scan position for a relation.
 * The routine publishes the current block so concurrent scans can select a
 * nearby start point.
 *
 * 报告一个关系的扫描位置。
 * 此例程发布当前块，使并发扫描可选择附近的起始点。
 */
extern void ss_report_location(Relation rel, BlockNumber location);

/* Get a synchronized scan start position for a relation.
 * The routine consults shared scan state and returns a valid block within the
 * supplied relation size.
 *
 * 获取一个关系的同步扫描起始位置。
 * 此例程查询共享扫描状态，并在给定关系大小内返回有效块。
 */
extern BlockNumber ss_get_location(Relation rel, BlockNumber relnblocks);

/* Initialize shared state for synchronized scans.
 * Startup creates or attaches the location-tracking structures used by
 * concurrent scans.
 *
 * 初始化同步扫描的共享状态。
 * 启动过程创建或附接并发扫描使用的位置跟踪结构。
 */
extern void SyncScanShmemInit(void);

/* Return the shared-memory size needed for synchronized scans.
 * The sizing pass accounts for shared relation-location tracking entries.
 *
 * 返回同步扫描所需的共享内存大小。
 * 大小计算阶段会计入共享关系位置跟踪条目。
 */
extern Size SyncScanShmemSize(void);

#endif
