/*-------------------------------------------------------------------------
 *
 * nodeProjectSet.h
 *	  Prototypes for ProjectSet nodes.
 *    ProjectSet 节点的原型。
 *
 * ProjectSet nodes are used to evaluate set-returning functions (SRFs)
 * that appear in the targetlist.
 * ProjectSet 节点用于评估出现在目标列中的集合返回函数 (SRF)。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeProjectSet.h
 *
 * Core Flow:
 * 核心流程：
 * 1. Initialization: ExecInitProjectSet prepares the state for the node and its subplan.
 *    初始化：ExecInitProjectSet 准备该节点及其子计划的状态。
 * 2. Execution: For each tuple from the subplan, the node expands any SRFs in the targetlist,
 *    potentially emitting multiple rows per input row. This is handled by ExecProjectSet.
 *    执行：对于来自子计划的每个元组，该节点展开目标列中的任何集合返回函数 (SRF)，
 *    可能会为每个输入行输出多行。这由 ExecProjectSet 处理。
 * 3. Re-scanning: ExecReScanProjectSet resets the node and its child plan to start over.
 *    重新扫描：ExecReScanProjectSet 重置该节点及其子计划以从头开始。
 * 4. Termination: ExecEndProjectSet cleans up the state and resources.
 *    终止：ExecEndProjectSet 清理状态和资源。
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEPROJECTSET_H
#define NODEPROJECTSET_H

#include "nodes/execnodes.h"

/* Initialize ProjectSet state / 初始化 ProjectSet 节点状态 */
extern ProjectSetState *ExecInitProjectSet(ProjectSet *node, EState *estate, int eflags);
/* Cleanup ProjectSet / 清理 ProjectSet 节点资源 */
extern void ExecEndProjectSet(ProjectSetState *node);
/* Restart ProjectSet / 重新启动 ProjectSet 扫描 */
extern void ExecReScanProjectSet(ProjectSetState *node);

#endif							/* NODEPROJECTSET_H */
