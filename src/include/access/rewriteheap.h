/*-------------------------------------------------------------------------
 *
 * rewriteheap.h
 *	  Declarations for heap rewrite support functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/access/rewriteheap.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * rewriteheap.h 堆重写支持函数的声明 src/inclu
 * de/access/rewriteheap.h
 */
#ifndef REWRITE_HEAP_H
#define REWRITE_HEAP_H

#include "access/htup.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* struct definition is private to rewriteheap.c */

/* 中文翻译：结构体定义是 rewriteheap.c 私有的 */
typedef struct RewriteStateData *RewriteState;

/*
 * Function begin_heap_rewrite constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 begin_heap_rewrite通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern RewriteState begin_heap_rewrite(Relation old_heap, Relation new_heap,
									   TransactionId oldest_xmin, TransactionId freeze_xid,
									   MultiXactId cutoff_multi);
/*
 * Function end_heap_rewrite completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 end_heap_rewrite在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void end_heap_rewrite(RewriteState state);
/*
 * Function rewrite_heap_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 rewrite_heap_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void rewrite_heap_tuple(RewriteState state, HeapTuple old_tuple,
							   HeapTuple new_tuple);
/*
 * Function rewrite_heap_dead_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 rewrite_heap_dead_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern bool rewrite_heap_dead_tuple(RewriteState state, HeapTuple old_tuple);

/*
 * On-Disk data format for an individual logical rewrite mapping.
 *
 * 中文翻译：
 * 用于单独逻辑重写映射的磁盘数据格式。
 */
typedef struct LogicalRewriteMappingData
{
	RelFileLocator old_locator;
	RelFileLocator new_locator;
	ItemPointerData old_tid;
	ItemPointerData new_tid;
} LogicalRewriteMappingData;

/* ---
 * The filename consists of the following, dash separated,
 * components:
 * 1) database oid or InvalidOid for shared relations
 * 2) the oid of the relation
 * 3) upper 32bit of the LSN at which a rewrite started
 * 4) lower 32bit of the LSN at which a rewrite started
 * 5) xid we are mapping for
 * 6) xid of the xact performing the mapping
 * ---
 *
 * 中文翻译：
 * 文件名由以下部分组成，用破折号分隔： 1) 共享关系的数据库 oid
 *  或 InvalidOid 2) 关系的 oid 3) 开始重写的
 * LSN 的高 32 位 4) 开始重写的 LSN 的低 32 位 5
 * ) 我们正在映射的 xid 6) 执行映射的 xact 的 xid
 */
#define LOGICAL_REWRITE_FORMAT "map-%x-%x-%X_%X-%x-%x"
/*
 * Function CheckPointLogicalRewriteHeap constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 CheckPointLogicalRewriteHeap通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void CheckPointLogicalRewriteHeap(void);

#endif							/* REWRITE_HEAP_H */

/* 中文翻译：重写_HEAP_H */
