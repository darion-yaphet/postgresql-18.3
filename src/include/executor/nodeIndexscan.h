/*-------------------------------------------------------------------------
 *
 * nodeIndexscan.h
 * 索引扫描节点处理头文件
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * 部分版权 (c) 1996-2025，PostgreSQL 全球开发组
 * Portions Copyright (c) 1994, Regents of the University of California
 * 部分版权 (c) 1994，加州大学董事会
 *
 * src/include/executor/nodeIndexscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINDEXSCAN_H
#define NODEINDEXSCAN_H

#include "access/genam.h"
#include "access/parallel.h"
#include "nodes/execnodes.h"

/*
 * IndexScan 节点的核心流程说明：
 * IndexScan 节点负责执行对表（Relation）的索引扫描。
 *
 * 核心流程如下：
 * 1. 初始化 (ExecInitIndexScan)：打开基础表和索引，初始化扫描状态。构建扫描键 (ScanKeys)，
 *    如果有运行时确定的键（Runtime Keys），也会进行准备。
 * 2. 执行 (ExecIndexScan)：
 *    - 如果有 ORDER BY 子句且索引支持，会调用 IndexNextWithReorder 处理排序（KNN 扫描）。
 *    - 否则调用 IndexNext 进行普通索引扫描。
 *    - 获取索引项，根据索引项找到对应的基础表元组，验证过滤条件。
 * 3. 重新扫描 (ExecReScanIndexScan)：当需要重复执行扫描时（如在嵌套循环连接的内侧），
 *    重置扫描位置，重新评估运行时参数。
 * 4. 结束 (ExecEndIndexScan)：关闭索引和表，释放相关资源。
 */

/* 初始化 IndexScan 节点 */
extern IndexScanState *ExecInitIndexScan(IndexScan *node, EState *estate, int eflags);

/* 结束 IndexScan 节点 */
extern void ExecEndIndexScan(IndexScanState *node);

/* 标记当前的扫描位置 */
extern void ExecIndexMarkPos(IndexScanState *node);

/* 恢复到之前标记的扫描位置 */
extern void ExecIndexRestrPos(IndexScanState *node);

/* 重新开始索引扫描 */
extern void ExecReScanIndexScan(IndexScanState *node);

/* 估计并行索引扫描所需的 DSM 空间 */
extern void ExecIndexScanEstimate(IndexScanState *node, ParallelContext *pcxt);

/* 为并行索引扫描初始化 DSM */
extern void ExecIndexScanInitializeDSM(IndexScanState *node, ParallelContext *pcxt);

/* 为新的扫描重新初始化 DSM */
extern void ExecIndexScanReInitializeDSM(IndexScanState *node, ParallelContext *pcxt);

/* 在并行工作进程上附加到 DSM 信息 */
extern void ExecIndexScanInitializeWorker(IndexScanState *node,
										  ParallelWorkerContext *pwcxt);

/* 获取扫描的检测信息 */
extern void ExecIndexScanRetrieveInstrumentation(IndexScanState *node);

/*
 * These routines are exported to share code with nodeIndexonlyscan.c and
 * nodeBitmapIndexscan.c
 * 这些例程被导出以供 nodeIndexonlyscan.c 和 nodeBitmapIndexscan.c 共享代码。
 */
/* 构建扫描键。将过滤条件和 ORDER BY 子句转化为索引访问方法所需的扫描键。 */
extern void ExecIndexBuildScanKeys(PlanState *planstate, Relation index,
								   List *quals, bool isorderby,
								   ScanKey *scanKeys, int *numScanKeys,
								   IndexRuntimeKeyInfo **runtimeKeys, int *numRuntimeKeys,
								   IndexArrayKeyInfo **arrayKeys, int *numArrayKeys);

/* 评估运行时键的值并更新扫描键。 */
extern void ExecIndexEvalRuntimeKeys(ExprContext *econtext,
									 IndexRuntimeKeyInfo *runtimeKeys, int numRuntimeKeys);

/* 评估数组类型的键值（处理 ANY 数组表达式）。 */
extern bool ExecIndexEvalArrayKeys(ExprContext *econtext,
								   IndexArrayKeyInfo *arrayKeys, int numArrayKeys);

/* 迭代到数组键值的下一个组合。 */
extern bool ExecIndexAdvanceArrayKeys(IndexArrayKeyInfo *arrayKeys, int numArrayKeys);

#endif							/* NODEINDEXSCAN_H */
                                /* NODEINDEXSCAN_H 结束 */
