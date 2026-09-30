/*-------------------------------------------------------------------------
 *
 * visibilitymap.h
 *		visibility map interface
 *
 *
 * Portions Copyright (c) 2007-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/visibilitymap.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * visibilitymap.h 可见性地图接口 src/includ
 * e/access/visibilitymap.h
 */
#ifndef VISIBILITYMAP_H
#define VISIBILITYMAP_H

#include "access/visibilitymapdefs.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "utils/relcache.h"

/* Macros for visibilitymap test */

/* 中文翻译：用于可见性图测试的宏 */
#define VM_ALL_VISIBLE(r, b, v) \
	((visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_VISIBLE) != 0)
#define VM_ALL_FROZEN(r, b, v) \
	((visibilitymap_get_status((r), (b), (v)) & VISIBILITYMAP_ALL_FROZEN) != 0)

/*
 * Function visibilitymap_clear updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 visibilitymap_clear通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern bool visibilitymap_clear(Relation rel, BlockNumber heapBlk,
								Buffer vmbuf, uint8 flags);
/*
 * Function visibilitymap_pin updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 visibilitymap_pin通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void visibilitymap_pin(Relation rel, BlockNumber heapBlk,
							  Buffer *vmbuf);
/*
 * Function visibilitymap_pin_ok updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 visibilitymap_pin_ok通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern bool visibilitymap_pin_ok(BlockNumber heapBlk, Buffer vmbuf);
/*
 * Function visibilitymap_set updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 visibilitymap_set通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern uint8 visibilitymap_set(Relation rel,
							   BlockNumber heapBlk, Buffer heapBuf,
							   XLogRecPtr recptr,
							   Buffer vmBuf,
							   TransactionId cutoff_xid,
							   uint8 flags);
/*
 * Function visibilitymap_get_status retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 visibilitymap_get_status通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern uint8 visibilitymap_get_status(Relation rel, BlockNumber heapBlk, Buffer *vmbuf);
/*
 * Function visibilitymap_count evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 visibilitymap_count通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern void visibilitymap_count(Relation rel, BlockNumber *all_visible, BlockNumber *all_frozen);
/*
 * Function visibilitymap_prepare_truncate updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 visibilitymap_prepare_truncate通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern BlockNumber visibilitymap_prepare_truncate(Relation rel,
												  BlockNumber nheapblocks);

#endif							/* VISIBILITYMAP_H */

/* 中文翻译：VISIBILITYMAP_H */
