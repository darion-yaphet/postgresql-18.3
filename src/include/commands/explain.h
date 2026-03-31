/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *	  explain.c 的原型
 *
 * 实现核心流程概述：
 * 本文件定义了处理 PostgreSQL EXPLAIN 命令的核心接口。EXPLAIN 用于展示查询计划器（Planner）为特定查询选择的执行路径。
 *
 * 核心流程如下：
 * 1. 命令初筛：ExplainQuery() 负责解析 EXPLAIN 的各种选项（如 ANALYZE, COSTS, VERBOSE, FORMAT 等），并设置 ExplainState。
 * 2. 规划/执行：
 *    - 核心逻辑由 ExplainOneQuery() 或 standard_ExplainOneQuery() 承载，它会调用规划器生成执行计划。
 *    - 若含有 ANALYZE 选项，则会调用执行器（Executor）实际运行查询并收集实时统计信息（如实际行数、耗时）。
 * 3. 格式化输出：支持多种输出格式（文本、XML、JSON、YAML）。ExplainPrintPlan() 及相关函数负责将计划树递归转换为可视化信息。
 * 4. 插件扩展：通过一系列钩子（Hooks），如 ExplainOneQuery_hook，允许加载模块（如 auto_explain）参与或修改解释过程，从而提供诸如索引名称自定义或额外节点统计信息等增强功能。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/explain.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"
#include "parser/parse_node.h"

struct ExplainState;			/* defined in explain_state.h */

/*
 * =========================================================================
 * 1. 插件钩子 (Hooks)
 * 允许外部模块拦截 EXPLAIN 的不同阶段，以实现实时监控或输出定制。
 * =========================================================================
 */

/* Hook for plugins to get control in ExplainOneQuery()
 * 插件在 ExplainOneQuery() 中获得控制权的钩子 */
typedef void (*ExplainOneQuery_hook_type) (Query *query,
										   int cursorOptions,
										   IntoClause *into,
										   struct ExplainState *es,
										   const char *queryString,
										   ParamListInfo params,
										   QueryEnvironment *queryEnv);
extern PGDLLIMPORT ExplainOneQuery_hook_type ExplainOneQuery_hook;

/* Hook for EXPLAIN plugins to print extra information for each plan
 * EXPLAIN 插件为每个计划打印额外信息的钩子 */
typedef void (*explain_per_plan_hook_type) (PlannedStmt *plannedstmt,
											IntoClause *into,
											struct ExplainState *es,
											const char *queryString,
											ParamListInfo params,
											QueryEnvironment *queryEnv);
extern PGDLLIMPORT explain_per_plan_hook_type explain_per_plan_hook;

/* Hook for EXPLAIN plugins to print extra fields on individual plan nodes
 * EXPLAIN 插件在单个计划节点上打印额外字段的钩子 */
typedef void (*explain_per_node_hook_type) (PlanState *planstate,
											List *ancestors,
											const char *relationship,
											const char *plan_name,
											struct ExplainState *es);
extern PGDLLIMPORT explain_per_node_hook_type explain_per_node_hook;

/* Hook for plugins to get control in explain_get_index_name()
 * 插件在 explain_get_index_name() 中获得控制权的钩子 */
typedef const char *(*explain_get_index_name_hook_type) (Oid indexId);
extern PGDLLIMPORT explain_get_index_name_hook_type explain_get_index_name_hook;


/*
 * =========================================================================
 * 2. 核心解释接口
 * 执行查询并生成解释输出的主函数入口。
 * =========================================================================
 */

extern void ExplainQuery(ParseState *pstate, ExplainStmt *stmt,
						 ParamListInfo params, DestReceiver *dest);
extern void standard_ExplainOneQuery(Query *query, int cursorOptions,
									 IntoClause *into, struct ExplainState *es,
									 const char *queryString, ParamListInfo params,
									 QueryEnvironment *queryEnv);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOneUtility(Node *utilityStmt, IntoClause *into,
							  struct ExplainState *es, ParseState *pstate,
							  ParamListInfo params);

/*
 * =========================================================================
 * 3. 计划树遍历与组件打印
 * 递归遍历计划树各节点，并根据选项打印相关统计数据（如触发器、JIT、参数等）。
 * =========================================================================
 */

extern void ExplainOnePlan(PlannedStmt *plannedstmt, IntoClause *into,
						   struct ExplainState *es, const char *queryString,
						   ParamListInfo params, QueryEnvironment *queryEnv,
						   const instr_time *planduration,
						   const BufferUsage *bufusage,
						   const MemoryContextCounters *mem_counters);

extern void ExplainPrintPlan(struct ExplainState *es, QueryDesc *queryDesc);
extern void ExplainPrintTriggers(struct ExplainState *es,
								 QueryDesc *queryDesc);

extern void ExplainPrintJITSummary(struct ExplainState *es,
								   QueryDesc *queryDesc);

extern void ExplainQueryText(struct ExplainState *es, QueryDesc *queryDesc);
extern void ExplainQueryParameters(struct ExplainState *es,
								   ParamListInfo params, int maxlen);

#endif							/* EXPLAIN_H */
