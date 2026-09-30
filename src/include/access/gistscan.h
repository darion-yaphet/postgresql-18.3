/*-------------------------------------------------------------------------
 *
 * gistscan.h
 *	  routines defined in access/gist/gistscan.c
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gistscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GISTSCAN_H
#define GISTSCAN_H

#include "access/amapi.h"

/*
 * Begin a scan of a GiST index: allocate and initialize the IndexScanDesc
 * and its GiST-specific opaque state, preparing the search queue and support
 * function information for the given number of scan and order-by keys.
 */

/*
 * 开始扫描 GiST 索引：分配并初始化 IndexScanDesc 及其 GiST 专用的不透明状态，
 * 根据给定数量的扫描键和 order-by 键，准备搜索队列以及支持函数信息。
 */
extern IndexScanDesc gistbeginscan(Relation r, int nkeys, int norderbys);

/*
 * Restart an existing GiST index scan with a new set of scan keys and
 * order-by keys: reset the scan's queue and per-scan state, then copy in the
 * new keys so the scan can be re-executed without reallocating the descriptor.
 */

/*
 * 使用一组新的扫描键和 order-by 键重新启动一个已存在的 GiST 索引扫描：
 * 重置扫描的队列和每次扫描的状态，然后拷贝进新的键，使得扫描无需重新分配
 * 描述符即可再次执行。
 */
extern void gistrescan(IndexScanDesc scan, ScanKey key, int nkeys,
					   ScanKey orderbys, int norderbys);

/*
 * End a GiST index scan: release resources associated with the scan,
 * including the search queue and any memory contexts held by the opaque state.
 */

/*
 * 结束一个 GiST 索引扫描：释放与该扫描相关联的资源，
 * 包括搜索队列以及不透明状态所持有的任何内存上下文。
 */
extern void gistendscan(IndexScanDesc scan);

#endif							/* GISTSCAN_H */
