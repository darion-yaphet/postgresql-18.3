/*-------------------------------------------------------------------------
 *
 * nodeLimit.h
 * 节点限制处理头文件
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * 部分版权 (c) 1996-2025，PostgreSQL 全球开发组
 * Portions Copyright (c) 1994, Regents of the University of California
 * 部分版权 (c) 1994，加州大学董事会
 *
 * src/include/executor/nodeLimit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODELIMIT_H
#define NODELIMIT_H

#include "nodes/execnodes.h"

/*
 * Limit 节点的核心流程说明：
 * Limit 节点通过一个简单的状态机来实现 SQL 中的 OFFSET 和 LIMIT 功能。
 *
 * 核心流程如下：
 * 1. 初始化：计算 OFFSET 和 LIMIT 表达式的值（这些值在初始化时可能不可用，因此在首次执行时计算）。
 * 2. 跳过：从子计划中获取元组，直到跳过了 OFFSET 指定的数量。
 * 3. 获取：继续从子计划获取元组并返回给上层节点，直到达到 LIMIT 指定的数量。
 * 4. 结束：如果指定了 LIMIT 且已达到，或者子计划已耗尽，则停止。
 * 5. Ties处理：如果使用了 WITH TIES，在达到 LIMIT 后会继续获取并返回与最后一条记录排序列值相同的元组。
 */

/*
 * ExecInitLimit
 * 初始化 Limit 节点。
 * 负责设置执行状态（LimitState），初始化其子计划节点，并准备执行。
 */
extern LimitState *ExecInitLimit(Limit *node, EState *estate, int eflags);

/*
 * ExecEndLimit
 * 结束 Limit 节点。
 * 释放节点分配的资源，并递归地关闭子计划。
 */
extern void ExecEndLimit(LimitState *node);

/*
 * ExecReScanLimit
 * 重新扫描 Limit 节点。
 * 重置内部位置计数器和状态机，并要求子计划重新开始。
 */
extern void ExecReScanLimit(LimitState *node);

#endif							/* NODELIMIT_H */
                                /* NODELIMIT_H 结束 */
