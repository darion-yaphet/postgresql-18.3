/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	  definitions for query plan nodes
 *    查询计划节点的定义
 *
 * This file defines the data structures used by the PostgreSQL planner to
 * represent the final execution plan. The executor receives a tree of these
 * nodes and processes them to produce query results.
 * 该文件定义了 PostgreSQL 查询优化器（planner）用于表示最终执行计划的数据结构。
 * 执行器（executor）接收这些节点组成的树并处理它们以产生查询结果。
 *
 * Core Flow / 核心流程:
 * 1. Planner Output: The planner transforms a Query tree into a PlannedStmt.
 *    优化器输出：优化器将查询树（Query tree）转换为 PlannedStmt。
 * 2. PlannedStmt: Root node containing global information for the executor.
 *    PlannedStmt：包含执行器所需全局信息的根节点。
 * 3. Plan Tree: A recursive structure of Plan nodes (Scan, Join, Agg, etc.).
 *    计划树：由 Plan 节点（扫描、连接、聚合等）组成的递归结构。
 * 4. Demand-driven Execution: Each node pulls tuples from its children (lefttree/righttree).
 *    驱动执行：每个节点从其子节点（左树/右树）拉取元组。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/plannodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include "access/sdir.h"
#include "access/stratnum.h"
#include "common/relpath.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		PlannedStmt node
 *
 * The output of the planner is a Plan tree headed by a PlannedStmt node.
 * PlannedStmt holds the "one time" information needed by the executor.
 * 优化器的输出是一个以 PlannedStmt 节点开头的计划树。
 * PlannedStmt 保存了执行器需要的“一次性”信息。
 *
 * For simplicity in APIs, we also wrap utility statements in PlannedStmt
 * nodes; in such cases, commandType == CMD_UTILITY, the statement itself
 * is in the utilityStmt field, and the rest of the struct is mostly dummy.
 * (We do use canSetTag, stmt_location, stmt_len, and possibly queryId.)
 * 为了简化 API，我们还将实用语句封装在 PlannedStmt 节点中；
 * 在这种情况下，commandType == CMD_UTILITY，语句本身在 utilityStmt 字段中，
 * 结构体的其余部分大多是虚拟的。（我们确实使用了 canSetTag、stmt_location、stmt_len，可能还有 queryId。）
 *
 * PlannedStmt, as well as all varieties of Plan, do not support equal(),
 * not because it's not sensible but because we currently have no need.
 * PlannedStmt 以及所有各种版本的 Plan 都不支持 equal()，
 * 不是因为这不合理，而是因为我们目前没有这个需求。
 * ----------------
 */
typedef struct PlannedStmt
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;

	/* select|insert|update|delete|merge|utility */
	/* 选择|插入|更新|删除|合并|实用程序 */
	CmdType		commandType;

	/* query identifier (copied from Query) */
	/* 查询标识符（从 Query 复制） */
	int64		queryId;

	/* plan identifier (can be set by plugins) */
	/* 计划标识符（可由插件设置） */
	int64		planId;

	/* is it insert|update|delete|merge RETURNING? */
	/* 是否为 insert|update|delete|merge RETURNING？ */
	bool		hasReturning;

	/* has insert|update|delete|merge in WITH? */
	/* WITH 中是否有 insert|update|delete|merge？ */
	bool		hasModifyingCTE;

	/* do I set the command result tag? */
	/* 是否设置命令结果标签？ */
	bool		canSetTag;

	/* redo plan when TransactionXmin changes? */
	/* 当 TransactionXmin 更改时是否重新执行计划？ */
	bool		transientPlan;

	/* is plan specific to current role? */
	/* 计划是否特定于当前角色？ */
	bool		dependsOnRole;

	/* parallel mode required to execute? */
	/* 执行是否需要并行模式？ */
	bool		parallelModeNeeded;

	/* which forms of JIT should be performed */
	/* 应执行哪些形式的 JIT */
	int			jitFlags;

	/* tree of Plan nodes */
	/* Plan 节点树 */
	struct Plan *planTree;

	/*
	 * List of PartitionPruneInfo contained in the plan
	 */
	/*
	 * 计划中包含的 PartitionPruneInfo 列表
	 */
	List	   *partPruneInfos;

	/* list of RangeTblEntry nodes */
	/* RangeTblEntry 节点列表 */
	List	   *rtable;

	/*
	 * RT indexes of relations that are not subject to runtime pruning or are
	 * needed to perform runtime pruning
	 */
	/*
	 * 不受运行时剪枝影响或执行运行时剪枝所需的关系的 RT 索引
	 */
	Bitmapset  *unprunableRelids;

	/*
	 * list of RTEPermissionInfo nodes for rtable entries needing one
	 */
	/*
	 * 需要 RTEPermissionInfo 节点的 rtable 条目的列表
	 */
	List	   *permInfos;

	/* rtable indexes of target relations for INSERT/UPDATE/DELETE/MERGE */
	/* integer list of RT indexes, or NIL */
	/* INSERT/UPDATE/DELETE/MERGE 的目标关系的 rtable 索引；整数 RT 索引列表，或 NIL */
	List	   *resultRelations;

	/* list of AppendRelInfo nodes */
	/* AppendRelInfo 节点列表 */
	List	   *appendRelations;

	/*
	 * Plan trees for SubPlan expressions; note that some could be NULL
	 */
	/*
	 * SubPlan 表达式的计划树；注意有些可能为 NULL
	 */
	List	   *subplans;

	/* indices of subplans that require REWIND */
	/* 需要 REWIND（倒回）的子计划索引 */
	Bitmapset  *rewindPlanIDs;

	/* a list of PlanRowMark's */
	/* PlanRowMark 列表 */
	List	   *rowMarks;

	/* OIDs of relations the plan depends on */
	/* 计划依赖的关系 OID */
	List	   *relationOids;

	/* other dependencies, as PlanInvalItems */
	/* 其他依赖项，作为 PlanInvalItems */
	List	   *invalItems;

	/* type OIDs for PARAM_EXEC Params */
	/* PARAM_EXEC 参数的类型 OID */
	List	   *paramExecTypes;

	/* non-null if this is utility stmt */
	/* 如果这是实用语句（utility stmt），则为非空 */
	Node	   *utilityStmt;

	/* statement location in source string (copied from Query) */
	/* start location, or -1 if unknown */
	/* 源字符串中的语句位置（从 Query 复制）；起始位置，如果未知则为 -1 */
	ParseLoc	stmt_location;
	/* length in bytes; 0 means "rest of string" */
	/* 长度（字节）；0 表示“字符串的其余部分” */
	ParseLoc	stmt_len;
} PlannedStmt;

/* macro for fetching the Plan associated with a SubPlan node */
/* 用于获取与 SubPlan 节点关联的 Plan 的宏 */
#define exec_subplan_get_plan(plannedstmt, subplan) \
	((Plan *) list_nth((plannedstmt)->subplans, (subplan)->plan_id - 1))


/* ----------------
 *		Plan node
 *
 * All plan nodes "derive" from the Plan structure by having the
 * Plan structure as the first field.  This ensures that everything works
 * when nodes are cast to Plan's.  (node pointers are frequently cast to Plan*
 * when passed around generically in the executor)
 * 所有计划节点通过将 Plan 结构作为第一个字段来从 Plan 结构“派生”。
 * 这确保了在将节点转换为 Plan 时一切正常（节点指针在执行器中通用传递时经常被转换为 Plan*）。
 *
 * We never actually instantiate any Plan nodes; this is just the common
 * abstract superclass for all Plan-type nodes.
 * 我们从未实际实例化任何 Plan 节点；这只是所有 Plan 类型节点的公共抽象超类。
 * ----------------
 */
typedef struct Plan
{
	pg_node_attr(abstract, no_equal, no_query_jumble)

	NodeTag		type;

	/*
	 * estimated execution costs for plan (see costsize.c for more info)
	 */
	/*
	 * 计划的估计执行成本（有关更多信息，请参见 costsize.c）
	 */
	/* count of disabled nodes */
	/* 已禁用节点的数量 */
	int			disabled_nodes;
	/* cost expended before fetching any tuples */
	/* 在提取任何元组之前花费的成本 */
	Cost		startup_cost;
	/* total cost (assuming all tuples fetched) */
	/* 总成本（假设提取了所有元组） */
	Cost		total_cost;

	/*
	 * planner's estimate of result size of this plan step
	 */
	/*
	 * 优化器对该计划步骤结果大小的估计
	 */
	/* number of rows plan is expected to emit */
	/* 计划预期发出的行数 */
	Cardinality plan_rows;
	/* average row width in bytes */
	/* 平均行宽（以字节为单位） */
	int			plan_width;

	/*
	 * information needed for parallel query
	 */
	/*
	 * 并行查询所需的信息
	 */
	/* engage parallel-aware logic? */
	/* 启用并行感知逻辑？ */
	bool		parallel_aware;
	/* OK to use as part of parallel plan? */
	/* 是否可以作为并行计划的一部分？ */
	bool		parallel_safe;

	/*
	 * information needed for asynchronous execution
	 */
	/*
	 * 异步执行所需的信息
	 */
	/* engage asynchronous-capable logic? */
	/* 启用具有异步能力的逻辑？ */
	bool		async_capable;

	/*
	 * Common structural data for all Plan types.
	 */
	/*
	 * 所有 Plan 类型的通用结构数据。
	 */
	/* unique across entire final plan tree */
	/* 在整个最终计划树中唯一 */
	int			plan_node_id;
	/* target list to be computed at this node */
	/* 在此节点计算的目标列表 */
	List	   *targetlist;
	/* implicitly-ANDed qual conditions */
	/* 隐式 AND 连接的限定条件 */
	List	   *qual;
	/* input plan tree(s) */
	/* 输入计划树 */
	struct Plan *lefttree;
	struct Plan *righttree;
	/* Init Plan nodes (un-correlated expr subselects) */
	/* Init Plan 节点（不相关的表达式子查询） */
	List	   *initPlan;

	/*
	 * Information for management of parameter-change-driven rescanning
	 *
	 * extParam includes the paramIDs of all external PARAM_EXEC params
	 * affecting this plan node or its children.  setParam params from the
	 * node's initPlans are not included, but their extParams are.
	 *
	 * allParam includes all the extParam paramIDs, plus the IDs of local
	 * params that affect the node (i.e., the setParams of its initplans).
	 * These are _all_ the PARAM_EXEC params that affect this node.
	 */
	/*
	 * 用于管理参数更改驱动的重新扫描的信息
	 *
	 * extParam 包含影响此计划节点及其子节点的所有外部 PARAM_EXEC 参数的 paramID。
	 * 来自节点 initPlans 的 setParam 参数不包括在内，但它们的 extParams 包括在内。
	 *
	 * allParam 包括所有 extParam paramID，加上影响节点的本地参数 ID（即其 initplans 的 setParams）。
	 * 这些是影响此节点的所有 PARAM_EXEC 参数。
	 */
	Bitmapset  *extParam;
	Bitmapset  *allParam;
} Plan;

/* ----------------
 *	these are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * 定义这些是为了避免“左”与“右”、“内”与“外”产生的混淆问题。
 * 约定是“左”计划是“外”计划，“右”计划是“内”计划，
 * 但这些定义使代码更具可读性。
 * ----------------
 */
#define innerPlan(node)			(((Plan *)(node))->righttree)
#define outerPlan(node)			(((Plan *)(node))->lefttree)


/* ----------------
 *	 Result node -
 *		If no outer plan, evaluate a variable-free targetlist.
 *		If outer plan, return tuples from outer plan (after a level of
 *		projection as shown by targetlist).
 *   Result 节点 -
 *     如果没有外部计划，则评估一个无变量的目标列表。
 *     如果有外部计划，则从外部计划返回元组（在经过目标列表所示的一层投影之后）。
 *
 * If resconstantqual isn't NULL, it represents a one-time qualification
 * test (i.e., one that doesn't depend on any variables from the outer plan,
 * so needs to be evaluated only once).
 * 如果 resconstantqual 不为 NULL，它表示一次性限定测试（即，不依赖于外部计划任何变量的测试，因此只需评估一次）。
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
} Result;

/* ----------------
 *	 ProjectSet node -
 *		Apply a projection that includes set-returning functions to the
 *		output tuples of the outer plan.
 *   ProjectSet 节点 -
 *     对外部计划的输出元组应用包含集合返回函数（SRF）的投影。
 * ----------------
 */
typedef struct ProjectSet
{
	Plan		plan;
} ProjectSet;

/* ----------------
 *	 ModifyTable node -
 *		Apply rows produced by outer plan to result table(s),
 *		by inserting, updating, or deleting.
 *   ModifyTable 节点 -
 *     通过插入、更新或删除，将外部计划产生的行应用到结果表。
 *
 * If the originally named target table is a partitioned table or inheritance
 * tree, both nominalRelation and rootRelation contain the RT index of the
 * partition root or appendrel RTE, which is not otherwise mentioned in the
 * plan.  Otherwise rootRelation is zero.  However, nominalRelation will
 * always be set, as it's the rel that EXPLAIN should claim is the
 * INSERT/UPDATE/DELETE/MERGE target.
 * 如果最初命名的目标表是分区表或继承树，nominalRelation 和 rootRelation
 * 都包含分区根节点或 appendrel RTE 的 RT 索引，而在计划的其他地方不会提到这一点。
 * 否则 rootRelation 为零。但是，nominalRelation 总是会被设置，
 * 因为它是 EXPLAIN 应该声称的 INSERT/UPDATE/DELETE/MERGE 目标表。
 *
 * Note that rowMarks and epqParam are presumed to be valid for all the
 * table(s); they can't contain any info that varies across tables.
 * 注意，rowMarks 和 epqParam 被假定对所有表都有效；
 * 它们不能包含随表而变化的信息。
 * ----------------
 */
typedef struct ModifyTable
{
	Plan		plan;
	/* INSERT, UPDATE, DELETE, or MERGE */
	/* INSERT, UPDATE, DELETE 或 MERGE */
	CmdType		operation;
	/* do we set the command tag/es_processed? */
	/* 是否设置命令标签/es_processed？ */
	bool		canSetTag;
	/* Parent RT index for use of EXPLAIN */
	/* 用于 EXPLAIN 的父级 RT 索引 */
	Index		nominalRelation;
	/* Root RT index, if partitioned/inherited */
	/* 如果是分区/继承表，则为根 RT 索引 */
	Index		rootRelation;
	/* some part key in hierarchy updated? */
	/* 层次结构中的某些分区键是否已更新？ */
	bool		partColsUpdated;
	/* integer list of RT indexes */
	/* RT 索引的整数列表 */
	List	   *resultRelations;
	/* per-target-table update_colnos lists */
	/* 每个目标表的 update_colnos 列表 */
	List	   *updateColnosLists;
	/* per-target-table WCO lists */
	/* 每个目标表的 WCO 列表 */
	List	   *withCheckOptionLists;
	/* alias for OLD in RETURNING lists */
	/* RETURNING 列表中 OLD 的别名 */
	char	   *returningOldAlias;
	/* alias for NEW in RETURNING lists */
	/* RETURNING 列表中 NEW 的别名 */
	char	   *returningNewAlias;
	/* per-target-table RETURNING tlists */
	/* 每个目标表的 RETURNING 目标列表 */
	List	   *returningLists;
	/* per-target-table FDW private data lists */
	/* 每个目标表的 FDW 私有数据列表 */
	List	   *fdwPrivLists;
	/* indices of FDW DM plans */
	/* FDW 直接修改计划的索引 */
	Bitmapset  *fdwDirectModifyPlans;
	/* PlanRowMarks (non-locking only) */
	/* PlanRowMarks（仅非锁定） */
	List	   *rowMarks;
	/* ID of Param for EvalPlanQual re-eval */
	/* 用于 EvalPlanQual 重新评估的参数 ID */
	int			epqParam;
	/* ON CONFLICT action */
	/* ON CONFLICT 操作 */
	OnConflictAction onConflictAction;
	/* List of ON CONFLICT arbiter index OIDs  */
	/* ON CONFLICT 仲裁索引 OID 列表 */
	List	   *arbiterIndexes;
	/* INSERT ON CONFLICT DO UPDATE targetlist */
	/* INSERT ON CONFLICT DO UPDATE 目标列表 */
	List	   *onConflictSet;
	/* target column numbers for onConflictSet */
	/* onConflictSet 的目标列号 */
	List	   *onConflictCols;
	/* WHERE for ON CONFLICT UPDATE */
	/* ON CONFLICT UPDATE 的 WHERE 条件 */
	Node	   *onConflictWhere;
	/* RTI of the EXCLUDED pseudo relation */
	/* EXCLUDED 伪关系的 RTI */
	Index		exclRelRTI;
	/* tlist of the EXCLUDED pseudo relation */
	/* EXCLUDED 伪关系的目标列表 */
	List	   *exclRelTlist;
	/* per-target-table lists of actions for MERGE */
	/* 每个目标表的 MERGE 操作列表 */
	List	   *mergeActionLists;
	/* per-target-table join conditions for MERGE */
	/* 每个目标表的 MERGE 连接条件 */
	List	   *mergeJoinConditions;
} ModifyTable;

struct PartitionPruneInfo;		/* forward reference to struct below */

/* ----------------
 *	 Append node -
 *		Generate the concatenation of the results of sub-plans.
 *   Append 节点 -
 *     生成子计划结果的串联。
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	/* RTIs of appendrel(s) formed by this node */
	/* 由此节点生成的 appendrel 的 RTI */
	Bitmapset  *apprelids;
	List	   *appendplans;
	/* # of asynchronous plans */
	/* 异步计划的数量 */
	int			nasyncplans;

	/*
	 * All 'appendplans' preceding this index are non-partial plans. All
	 * 'appendplans' from this index onwards are partial plans.
	 */
	/*
	 * 此索引之前的所有 'appendplans' 都是非部分计划。从该索引往后的所有 'appendplans' 都是部分计划。
	 */
	int			first_partial_plan;

	/*
	 * Index into PlannedStmt.partPruneInfos and parallel lists in EState:
	 * es_part_prune_states and es_part_prune_results. Set to -1 if no
	 * run-time pruning is used.
	 */
	/*
	 * 进入 PlannedStmt.partPruneInfos 以及 EState 中的并行列表（es_part_prune_states 和 es_part_prune_results）的索引。
	 * 如果不使用运行时剪枝，则设置为 -1。
	 */
	int			part_prune_index;
} Append;

/* ----------------
 *	 MergeAppend node -
 *		Merge the results of pre-sorted sub-plans to preserve the ordering.
 *   MergeAppend 节点 -
 *     合并预先排序好的子计划结果以保持顺序。
 * ----------------
 */
typedef struct MergeAppend
{
	Plan		plan;

	/* RTIs of appendrel(s) formed by this node */
	/* 由此节点生成的 appendrel 的 RTI */
	Bitmapset  *apprelids;

	List	   *mergeplans;

	/* these fields are just like the sort-key info in struct Sort: */
	/* 这些字段就像 Sort 结构中的排序键信息： */

	/* number of sort-key columns */
	/* 排序键列的数量 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* OIDs of operators to sort them by */
	/* 用于对它们进行排序的操作符 OID */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* OIDs of collations */
	/* 排序规则 OID */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST directions */
	/* NULLS FIRST/LAST 方向 */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));

	/*
	 * Index into PlannedStmt.partPruneInfos and parallel lists in EState:
	 * es_part_prune_states and es_part_prune_results. Set to -1 if no
	 * run-time pruning is used.
	 */
	/*
	 * 进入 PlannedStmt.partPruneInfos 以及 EState 中的并行列表（es_part_prune_states 和 es_part_prune_results）的索引。
	 * 如果不使用运行时剪枝，则设置为 -1。
	 */
	int			part_prune_index;
} MergeAppend;

/* ----------------
 *	RecursiveUnion node -
 *		Generate a recursive union of two subplans.
 *   RecursiveUnion 节点 -
 *     生成两个子计划的递归联合。
 *
 * The "outer" subplan is always the non-recursive term, and the "inner"
 * subplan is the recursive term.
 * “外”子计划始终是非递归项，“内”子计划是递归项。
 * ----------------
 */
typedef struct RecursiveUnion
{
	Plan		plan;

	/* ID of Param representing work table */
	/* 代表工作表的参数 ID */
	int			wtParam;

	/* Remaining fields are zero/null in UNION ALL case */
	/* 在 UNION ALL 的情况下，其余字段为零/空 */

	/* number of columns to check for duplicate-ness */
	/* 要检查重复性的列数 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *dupColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	/* 用于比较的等值操作符 */
	Oid		   *dupOperators pg_node_attr(array_size(numCols));
	Oid		   *dupCollations pg_node_attr(array_size(numCols));

	/* estimated number of groups in input */
	/* 输入中的估计组数 */
	long		numGroups;
} RecursiveUnion;

/* ----------------
 *	 BitmapAnd node -
 *		Generate the intersection of the results of sub-plans.
 *   BitmapAnd 节点 -
 *     生成子计划结果的交集。
 *
 * The subplans must be of types that yield tuple bitmaps.  The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * 子计划必须是产生元组位图的类型。计划的 targetlist 和 qual 字段未使用，且始终为 NIL。
 * ----------------
 */
typedef struct BitmapAnd
{
	Plan		plan;
	List	   *bitmapplans;
} BitmapAnd;

/* ----------------
 *	 BitmapOr node -
 *		Generate the union of the results of sub-plans.
 *   BitmapOr 节点 -
 *     生成子计划结果的并集。
 *
 * The subplans must be of types that yield tuple bitmaps.  The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * 子计划必须是产生元组位图的类型。计划的 targetlist 和 qual 字段未使用，且始终为 NIL。
 * ----------------
 */
typedef struct BitmapOr
{
	Plan		plan;
	bool		isshared;
	List	   *bitmapplans;
} BitmapOr;

/*
 * ==========
 * Scan nodes
 *   扫描节点
 *
 * Scan is an abstract type that all relation scan plan types inherit from.
 * Scan 是所有关系扫描计划类型继承的抽象类型。
 * ==========
 */
typedef struct Scan
{
	pg_node_attr(abstract)

	Plan		plan;
	/* relid is index into the range table */
	Index		scanrelid;
} Scan;

/* ----------------
 *		sequential scan node
 *      顺序扫描节点
 * ----------------
 */
typedef struct SeqScan
{
	Scan		scan;
} SeqScan;

/* ----------------
 *		table sample scan node
 *      表采样扫描节点
 * ----------------
 */
typedef struct SampleScan
{
	Scan		scan;
	/* use struct pointer to avoid including parsenodes.h here */
	struct TableSampleClause *tablesample;
} SampleScan;

/* ----------------
 *		index scan node
 *      索引扫描节点
 *
 * indexqualorig is an implicitly-ANDed list of index qual expressions, each
 * in the same form it appeared in the query WHERE condition.  Each should
 * be of the form (indexkey OP comparisonval) or (comparisonval OP indexkey).
 * The indexkey is a Var or expression referencing column(s) of the index's
 * base table.  The comparisonval might be any expression, but it won't use
 * any columns of the base table.  The expressions are ordered by index
 * column position (but items referencing the same index column can appear
 * in any order).  indexqualorig is used at runtime only if we have to recheck
 * a lossy indexqual.
 * indexqualorig 是一个隐式 AND 连接的索引限定表达式列表，
 * 每个表达式的形式与其在查询 WHERE 条件中出现的形式相同。
 * 每个表达式应具有 (indexkey OP comparisonval) 或 (comparisonval OP indexkey) 的形式。
 * indexkey 是引用索引基表列的 Var 或表达式。
 * comparisonval 可以是任何表达式，但它不会使用基表的任何列。
 * 表达式按索引列位置排序（但引用相同索引列的项可以以任何顺序出现）。
 * 仅当我们必须重新检查有损 indexqual 时，才在运行时使用 indexqualorig。
 *
 * indexqual has the same form, but the expressions have been commuted if
 * necessary to put the indexkeys on the left, and the indexkeys are replaced
 * by Var nodes identifying the index columns (their varno is INDEX_VAR and
 * their varattno is the index column number).
 * indexqual 具有相同的形式，但如果需要，表达式已被置换为将 indexkey 放在左侧，
 * 并且 indexkey 被标识索引列的 Var 节点替换（它们的 varno 是 INDEX_VAR，
 * 它们的 varattno 是索引列号）。
 *
 * indexorderbyorig is similarly the original form of any ORDER BY expressions
 * that are being implemented by the index, while indexorderby is modified to
 * have index column Vars on the left-hand side.  Here, multiple expressions
 * must appear in exactly the ORDER BY order, and this is not necessarily the
 * index column order.  Only the expressions are provided, not the auxiliary
 * sort-order information from the ORDER BY SortGroupClauses; it's assumed
 * that the sort ordering is fully determinable from the top-level operators.
 * indexorderbyorig is used at runtime to recheck the ordering, if the index
 * cannot calculate an accurate ordering.  It is also needed for EXPLAIN.
 * indexorderbyorig 类似地是正在由索引实现的任何 ORDER BY 表达式的原始形式，
 * 而 indexorderby 经过修改以将索引列 Var 放在左侧。
 * 在这里，多个表达式必须完全按照 ORDER BY 的顺序出现，这不一定是索引列的顺序。
 * 仅提供表达式，不提供 ORDER BY SortGroupClauses 中的辅助排序列信息；
 * 假定排序顺序完全可以从顶级运算符确定。
 * 如果索引无法计算准确的顺序，则在运行时使用 indexorderbyorig 重新检查顺序。
 * EXPLAIN 也需要它。
 *
 * indexorderbyops is a list of the OIDs of the operators used to sort the
 * ORDER BY expressions.  This is used together with indexorderbyorig to
 * recheck ordering at run time.  (Note that indexorderby, indexorderbyorig,
 * and indexorderbyops are used for amcanorderbyop cases, not amcanorder.)
 * indexorderbyops 是用于对 ORDER BY 表达式进行排序的运算符 OID 列表。
 * 这与 indexorderbyorig 一起用于在运行时重新检查排序。（请注意，indexorderby、
 * indexorderbyorig 和 indexorderbyops 用于 amcanorderbyop 情况，而不是 amcanorder。）
 *
 * indexorderdir specifies the scan ordering, for indexscans on amcanorder
 * indexes (for other indexes it should be "don't care").
 * indexorderdir 指定扫描顺序，用于 amcanorder 索引上的索引扫描（对于其他索引，它应为“无所谓”）。
 * ----------------
 */
typedef struct IndexScan
{
	Scan		scan;
	/* OID of index to scan */
	/* 要扫描的索引的 OID */
	Oid			indexid;
	/* list of index quals (usually OpExprs) */
	/* 索引限定条件的列表（通常是 OpExprs） */
	List	   *indexqual;
	/* the same in original form */
	/* 原始形式的相同内容 */
	List	   *indexqualorig;
	/* list of index ORDER BY exprs */
	/* 索引 ORDER BY 表达式的列表 */
	List	   *indexorderby;
	/* the same in original form */
	/* 原始形式的相同内容 */
	List	   *indexorderbyorig;
	/* OIDs of sort ops for ORDER BY exprs */
	/* ORDER BY 表达式的排序操作符 OID */
	List	   *indexorderbyops;
	/* forward or backward or don't care */
	/* 向前、向后或无所谓 */
	ScanDirection indexorderdir;
} IndexScan;

/* ----------------
 *		index-only scan node
 *      仅索引扫描节点
 *
 * IndexOnlyScan is very similar to IndexScan, but it specifies an
 * index-only scan, in which the data comes from the index not the heap.
 * Because of this, *all* Vars in the plan node's targetlist, qual, and
 * index expressions reference index columns and have varno = INDEX_VAR.
 * IndexOnlyScan 与 IndexScan 非常相似，但它指定了仅索引扫描，
 * 其中数据来自索引而非堆（heap）。
 * 因此，计划节点 targetlist、qual 和索引表达式中的 *所有* Var 引用索引列，
 * 并且具有 varno = INDEX_VAR。
 *
 * We could almost use indexqual directly against the index's output tuple
 * when rechecking lossy index operators, but that won't work for quals on
 * index columns that are not retrievable.  Hence, recheckqual is needed
 * for rechecks: it expresses the same condition as indexqual, but using
 * only index columns that are retrievable.  (We will not generate an
 * index-only scan if this is not possible.  An example is that if an
 * index has table column "x" in a retrievable index column "ind1", plus
 * an expression f(x) in a non-retrievable column "ind2", an indexable
 * query on f(x) will use "ind2" in indexqual and f(ind1) in recheckqual.
 * Without the "ind1" column, an index-only scan would be disallowed.)
 * 在重新检查有损索引运算符时，我们几乎可以针对索引的输出元组直接使用 indexqual，
 * 但这对于不可检索的索引列上的限定（qual）不起作用。
 * 因此，重新检查需要 recheckqual：它表达与 indexqual 相同的条件，
 * 但仅使用可以检索的索引列。（如果不可能，我们将不会生成仅索引扫描。
 * 例如，如果索引在可检索列 "ind1" 中具有表列 "x"，
 * 并且在不可检索列 "ind2" 中具有表达式 f(x)，
 * 则针对 f(x) 的可索引查询将在 indexqual 中使用 "ind2"，并在 recheckqual 中使用 f(ind1)。
 * 如果没有 "ind1" 列，将不允许进行仅索引扫描。）
 *
 * We don't currently need a recheckable equivalent of indexorderby,
 * because we don't support lossy operators in index ORDER BY.
 * 我们目前不需要 indexorderby 的可重新检查等效物，因为我们不支持索引 ORDER BY 中的有损运算符。
 *
 * To help EXPLAIN interpret the index Vars for display, we provide
 * indextlist, which represents the contents of the index as a targetlist
 * with one TLE per index column.  Vars appearing in this list reference
 * the base table, and this is the only field in the plan node that may
 * contain such Vars.  Also, for the convenience of setrefs.c, TLEs in
 * indextlist are marked as resjunk if they correspond to columns that
 * the index AM cannot reconstruct.
 * 为了帮助 EXPLAIN 解释用于显示的索引 Var，我们提供了 indextlist，
 * 它将索引内容表示为一个目标列表，每个索引列有一个 TLE。
 * 此列表中出现的 Var 引用基表，这是计划节点中唯一可以包含此类 Var 的字段。
 * 此外，为了 setrefs.c 的方便，如果 indextlist 中的 TLE 对应于索引 AM 无法重建的列，
 * 则将其标记为 resjunk。
 * ----------------
 */
typedef struct IndexOnlyScan
{
	Scan		scan;
	/* OID of index to scan */
	/* 要扫描的索引的 OID */
	Oid			indexid;
	/* list of index quals (usually OpExprs) */
	/* 索引限定条件的列表（通常是 OpExprs） */
	List	   *indexqual;
	/* index quals in recheckable form */
	/* 可重新检查形式的索引限定条件 */
	List	   *recheckqual;
	/* list of index ORDER BY exprs */
	/* 索引 ORDER BY 表达式的列表 */
	List	   *indexorderby;
	/* TargetEntry list describing index's cols */
	/* 描述索引列的 TargetEntry 列表 */
	List	   *indextlist;
	/* forward or backward or don't care */
	/* 向前、向后或无所谓 */
	ScanDirection indexorderdir;
} IndexOnlyScan;

/* ----------------
 *		bitmap index scan node
 *      位图索引扫描节点
 *
 * BitmapIndexScan delivers a bitmap of potential tuple locations;
 * it does not access the heap itself.  The bitmap is used by an
 * ancestor BitmapHeapScan node, possibly after passing through
 * intermediate BitmapAnd and/or BitmapOr nodes to combine it with
 * the results of other BitmapIndexScans.
 * BitmapIndexScan 提供潜在元组位置的位图；它本身不访问堆。
 * 该位图由祖先 BitmapHeapScan 节点使用，
 * 可能会经过中间的 BitmapAnd 和/或 BitmapOr 节点，将其与其他 BitmapIndexScan 的结果合并。
 *
 * The fields have the same meanings as for IndexScan, except we don't
 * store a direction flag because direction is uninteresting.
 * 字段的含义与 IndexScan 相同，但我们不存储方向标志，因为方向在这里不重要。
 *
 * In a BitmapIndexScan plan node, the targetlist and qual fields are
 * not used and are always NIL.  The indexqualorig field is unused at
 * run time too, but is saved for the benefit of EXPLAIN.
 * 在 BitmapIndexScan 计划节点中，targetlist 和 qual 字段未使用且始终为 NIL。
 * indexqualorig 字段在运行时也不使用，但保留以利于 EXPLAIN。
 * ----------------
 */
typedef struct BitmapIndexScan
{
	Scan		scan;
	/* OID of index to scan */
	/* 要扫描的索引的 OID */
	Oid			indexid;
	/* Create shared bitmap if set */
	/* 如果设置，则创建共享位图 */
	bool		isshared;
	/* list of index quals (OpExprs) */
	/* 索引限定条件列表 (OpExprs) */
	List	   *indexqual;
	/* the same in original form */
	/* 原始形式的相同内容 */
	List	   *indexqualorig;
} BitmapIndexScan;

/* ----------------
 *		bitmap sequential scan node
 *      位图顺序扫描节点
 *
 * This needs a copy of the qual conditions being used by the input index
 * scans because there are various cases where we need to recheck the quals;
 * for example, when the bitmap is lossy about the specific rows on a page
 * that meet the index condition.
 * 这需要一份由输入索引扫描使用的限定条件的副本，
 * 因为在各种情况下我们需要重新检查限定条件；例如，
 * 当位图对于页面上满足索引条件的特定行是有损的时。
 * ----------------
 */
typedef struct BitmapHeapScan
{
	Scan		scan;
	/* index quals, in standard expr form */
	/* 标准表达式形式的索引限定条件 */
	List	   *bitmapqualorig;
} BitmapHeapScan;

/* ----------------
 *		tid scan node
 *      TID 扫描节点
 *
 * tidquals is an implicitly OR'ed list of qual expressions of the form
 * "CTID = pseudoconstant", or "CTID = ANY(pseudoconstant_array)",
 * or a CurrentOfExpr for the relation.
 * tidquals 是一个隐式 OR 连接的限定表达式列表，形式为 "CTID = pseudoconstant"
 * 或 "CTID = ANY(pseudoconstant_array)"，或者是关系的 CurrentOfExpr。
 * ----------------
 */
typedef struct TidScan
{
	Scan		scan;
	/* qual(s) involving CTID = something */
	/* 涉及 CTID = 某内容的限定条件 */
	List	   *tidquals;
} TidScan;

/* ----------------
 *		tid range scan node
 *      TID 范围扫描节点
 *
 * tidrangequals is an implicitly AND'ed list of qual expressions of the form
 * "CTID relop pseudoconstant", where relop is one of >,>=,<,<=.
 * tidrangequals 是一个隐式 AND 连接的限定表达式列表，
 * 形式为 "CTID relop pseudoconstant"，其中 relop 是 >,>=,<,<= 之一。
 * ----------------
 */
typedef struct TidRangeScan
{
	Scan		scan;
	/* qual(s) involving CTID op something */
	/* 涉及 CTID op 某内容的限定条件 */
	List	   *tidrangequals;
} TidRangeScan;

/* ----------------
 *		subquery scan node
 *      子查询扫描节点
 *
 * SubqueryScan is for scanning the output of a sub-query in the range table.
 * We often need an extra plan node above the sub-query's plan to perform
 * expression evaluations (which we can't push into the sub-query without
 * risking changing its semantics).  Although we are not scanning a physical
 * relation, we make this a descendant of Scan anyway for code-sharing
 * purposes.
 * SubqueryScan 用于扫描 range table 中子查询的输出。
 * 我们通常需要在子查询计划之上添加一个额外的计划节点来执行表达式评估
 * （我们不能将其推入子查询，以免改变其语义）。
 * 尽管我们不是在扫描物理关系，但出于代码共享的目的，我们仍然使其成为 Scan 的派生类。
 *
 * SubqueryScanStatus caches the trivial_subqueryscan property of the node.
 * SUBQUERY_SCAN_UNKNOWN means not yet determined.  This is only used during
 * planning.
 * SubqueryScanStatus 缓存节点的 trivial_subqueryscan 属性。
 * SUBQUERY_SCAN_UNKNOWN 表示尚未确定。这仅在规划期间使用。
 *
 * Note: we store the sub-plan in the type-specific subplan field, not in
 * the generic lefttree field as you might expect.  This is because we do
 * not want plan-tree-traversal routines to recurse into the subplan without
 * knowing that they are changing Query contexts.
 * 注意：我们将子计划存储在特定于类型的 subplan 字段中，而不是像预期的那样存储在通用的 lefttree 字段中。
 * 这是因为我们不希望计划树遍历例程在不知道它们正在更改 Query 上下文的情况下递归进入子计划。
 * ----------------
 */
typedef enum SubqueryScanStatus
{
	SUBQUERY_SCAN_UNKNOWN,
	SUBQUERY_SCAN_TRIVIAL,
	SUBQUERY_SCAN_NONTRIVIAL,
} SubqueryScanStatus;

typedef struct SubqueryScan
{
	Scan		scan;
	Plan	   *subplan;
	SubqueryScanStatus scanstatus;
} SubqueryScan;

/* ----------------
 *		FunctionScan node
 *      函数扫描节点
 * ----------------
 */
typedef struct FunctionScan
{
	Scan		scan;
	/* list of RangeTblFunction nodes */
	/* RangeTblFunction 节点列表 */
	List	   *functions;
	/* WITH ORDINALITY */
	/* 带有 ORDINALITY 子句 */
	bool		funcordinality;
} FunctionScan;

/* ----------------
 *		ValuesScan node
 *      Values 扫描节点
 * ----------------
 */
typedef struct ValuesScan
{
	Scan		scan;
	/* list of expression lists */
	/* 表达式列表的列表 */
	List	   *values_lists;
} ValuesScan;

/* ----------------
 *		TableFunc scan node
 *      表函数扫描节点
 * ----------------
 */
typedef struct TableFuncScan
{
	Scan		scan;
	/* table function node */
	/* 表函数节点 */
	TableFunc  *tablefunc;
} TableFuncScan;

/* ----------------
 *		CteScan node
 *      CTE 扫描节点
 * ----------------
 */
typedef struct CteScan
{
	Scan		scan;
	/* ID of init SubPlan for CTE */
	/* CTE 的 init SubPlan 的 ID */
	int			ctePlanId;
	/* ID of Param representing CTE output */
	/* 代表 CTE 输出的参数 ID */
	int			cteParam;
} CteScan;

/* ----------------
 *		NamedTuplestoreScan node
 *      命名元组存储扫描节点
 * ----------------
 */
typedef struct NamedTuplestoreScan
{
	Scan		scan;
	/* Name given to Ephemeral Named Relation */
	/* 给临时命名关系（ENR）指定的名称 */
	char	   *enrname;
} NamedTuplestoreScan;

/* ----------------
 *		WorkTableScan node
 *      工作表扫描节点
 * ----------------
 */
typedef struct WorkTableScan
{
	Scan		scan;
	/* ID of Param representing work table */
	/* 代表工作表的参数 ID */
	int			wtParam;
} WorkTableScan;

/* ----------------
 *		ForeignScan node
 *
 * fdw_exprs and fdw_private are both under the control of the foreign-data
 * wrapper, but fdw_exprs is presumed to contain expression trees and will
 * be post-processed accordingly by the planner; fdw_private won't be.
 * Note that everything in both lists must be copiable by copyObject().
 * One way to store an arbitrary blob of bytes is to represent it as a bytea
 * Const.  Usually, though, you'll be better off choosing a representation
 * that can be dumped usefully by nodeToString().
 * fdw_exprs 和 fdw_private 都受外部数据包装器（FDW）控制，但 fdw_exprs 被假定包含表达式树，
 * 并且优化器将对其进行相应的后处理；fdw_private 则不会。注意两个列表中的所有内容都必须能由 copyObject() 复制。
 * 存储任意字节块的一种方法是将其表示为 bytea Const。不过通常情况下，选择一个能被 nodeToString() 有效转储的表示形式会更好。
 *
 * fdw_scan_tlist is a targetlist describing the contents of the scan tuple
 * returned by the FDW; it can be NIL if the scan tuple matches the declared
 * rowtype of the foreign table, which is the normal case for a simple foreign
 * table scan.  (If the plan node represents a foreign join, fdw_scan_tlist
 * is required since there is no rowtype available from the system catalogs.)
 * When fdw_scan_tlist is provided, Vars in the node's tlist and quals must
 * have varno INDEX_VAR, and their varattnos correspond to resnos in the
 * fdw_scan_tlist (which are also column numbers in the actual scan tuple).
 * fdw_scan_tlist is never actually executed; it just holds expression trees
 * describing what is in the scan tuple's columns.
 * fdw_scan_tlist 是一个目标列表，描述由 FDW 返回的扫描元组的内容；
 * 如果扫描元组与外部表声明的行类型匹配（这是简单外部表扫描的正常情况），它可以为 NIL。
 * （如果计划节点表示外部连接，则需要 fdw_scan_tlist，因为系统目录中没有可用的行类型。）
 * 当提供 fdw_scan_tlist 时，节点的目标列表（tlist）和限定条件（quals）中的 Var 必须具有 varno INDEX_VAR，
 * 并且它们的 varattno 对应于 fdw_scan_tlist 中的 resno（即实际扫描元组中的列号）。
 * fdw_scan_tlist 实际上从不执行；它只是持有描述扫描元组各列内容的表达式树。
 *
 * fdw_recheck_quals should contain any quals which the core system passed to
 * the FDW but which were not added to scan.plan.qual; that is, it should
 * contain the quals being checked remotely.  This is needed for correct
 * behavior during EvalPlanQual rechecks.
 * fdw_recheck_quals 应当包含核心系统传递给 FDW 但未添加到 scan.plan.qual 中的任何限定条件；
 * 也就是说，它应当包含正在远程检查的限定条件。这对于 EvalPlanQual 重新检查期间的正确行为是必需的。
 *
 * When the plan node represents a foreign join, scan.scanrelid is zero and
 * fs_relids must be consulted to identify the join relation.  (fs_relids
 * is valid for simple scans as well, but will always match scan.scanrelid.)
 * fs_relids includes outer joins; fs_base_relids does not.
 * 当计划节点表示外部连接时，scan.scanrelid 为零，必须参考 fs_relids 来标识连接关系。
 * （fs_relids 对简单扫描同样有效，但始终与 scan.scanrelid 匹配。）
 * fs_relids 包含外部连接；fs_base_relids 不包含。
 *
 * If the FDW's PlanDirectModify() callback decides to repurpose a ForeignScan
 * node to perform the UPDATE or DELETE operation directly in the remote
 * server, it sets 'operation' and 'resultRelation' to identify the operation
 * type and target relation.  Note that these fields are only set if the
 * modification is performed *fully* remotely; otherwise, the modification is
 * driven by a local ModifyTable node and 'operation' is left to CMD_SELECT.
 * 如果 FDW 的 PlanDirectModify() 回调决定重用 ForeignScan 节点以直接在远程服务器中执行 UPDATE 或 DELETE 操作，
 * 它将设置 'operation' 和 'resultRelation' 来标识操作类型和目标关系。
 * 注意，只有在修改完全在远程执行时才设置这些字段；否则，修改由本地 ModifyTable 节点驱动，且 'operation' 保持为 CMD_SELECT。
 * ----------------
 */
typedef struct ForeignScan
{
	Scan		scan;
	/* SELECT/INSERT/UPDATE/DELETE */
	/* SELECT/INSERT/UPDATE/DELETE */
	CmdType		operation;
	/* direct modification target's RT index */
	/* 直接修改目标的 RT 索引 */
	Index		resultRelation;
	/* user to perform the scan as; 0 means to check as current user */
	/* 执行扫描时使用的用户；0 表示作为当前用户检查 */
	Oid			checkAsUser;
	/* OID of foreign server */
	/* 外部服务器的 OID */
	Oid			fs_server;
	/* expressions that FDW may evaluate */
	/* FDW 可能评估的表达式 */
	List	   *fdw_exprs;
	/* private data for FDW */
	/* FDW 的私有数据 */
	List	   *fdw_private;
	/* optional tlist describing scan tuple */
	/* 描述扫描元组的可选目标列表 */
	List	   *fdw_scan_tlist;
	/* original quals not in scan.plan.qual */
	/* 不在 scan.plan.qual 中的原始限定条件 */
	List	   *fdw_recheck_quals;
	/* base+OJ RTIs generated by this scan */
	/* 由此扫描生成的基表+外部连接 RTI */
	Bitmapset  *fs_relids;
	/* base RTIs generated by this scan */
	/* 由此扫描生成的基表 RTI */
	Bitmapset  *fs_base_relids;
	/* true if any "system column" is needed */
	/* 如果需要任何“系统列”，则为 true */
	bool		fsSystemCol;
} ForeignScan;

/* ----------------
 *	   CustomScan node
 *     自定义扫描节点
 *
 * The comments for ForeignScan's fdw_exprs, fdw_private, fdw_scan_tlist,
 * and fs_relids fields apply equally to CustomScan's custom_exprs,
 * custom_private, custom_scan_tlist, and custom_relids fields.  The
 * convention of setting scan.scanrelid to zero for joins applies as well.
 * ForeignScan 的 fdw_exprs、fdw_private、fdw_scan_tlist 和 fs_relids 字段的注释
 * 同样适用于 CustomScan 的 custom_exprs、custom_private、custom_scan_tlist 和 custom_relids 字段。
 * 将连接的 scan.scanrelid 设置为零的约定也同样适用。
 *
 * Note that since Plan trees can be copied, custom scan providers *must*
 * fit all plan data they need into those fields; embedding CustomScan in
 * a larger struct will not work.
 * 请注意，由于计划树可以被复制，自定义扫描提供者 *必须* 将其所需的所有计划数据放入这些字段中；
 * 将 CustomScan 嵌入更大的结构体中将不起作用。
 * ----------------
 */
struct CustomScanMethods;

typedef struct CustomScan
{
	Scan		scan;
	/* mask of CUSTOMPATH_* flags, see nodes/extensible.h */
	/* CUSTOMPATH_* 标志的掩码，见 nodes/extensible.h */
	uint32		flags;
	/* list of Plan nodes, if any */
	/* 计划节点列表（如果有） */
	List	   *custom_plans;
	/* expressions that custom code may evaluate */
	/* 自定义代码可能评估的表达式 */
	List	   *custom_exprs;
	/* private data for custom code */
	/* 自定义代码的私有数据 */
	List	   *custom_private;
	/* optional tlist describing scan tuple */
	/* 描述扫描元组的可选目标列表 */
	List	   *custom_scan_tlist;
	/* RTIs generated by this scan */
	/* 由此扫描生成的 RTI */
	Bitmapset  *custom_relids;

	/*
	 * NOTE: The method field of CustomScan is required to be a pointer to a
	 * static table of callback functions.  So we don't copy the table itself,
	 * just reference the original one.
	 */
	const struct CustomScanMethods *methods;
} CustomScan;

/*
 * ==========
 * Join nodes
 *   连接节点
 * ==========
 */

/* ----------------
 *		Join node
 *      连接节点
 *
 * jointype:	rule for joining tuples from left and right subtrees
 * inner_unique each outer tuple can match to no more than one inner tuple
 * joinqual:	qual conditions that came from JOIN/ON or JOIN/USING
 *				(plan.qual contains conditions that came from WHERE)
 * jointype: 从左右子树连接元组的规则
 * inner_unique: 每个外部元组最多只能匹配一个内部元组
 * joinqual: 来自 JOIN/ON 或 JOIN/USING 的限定条件（plan.qual 包含来自 WHERE 的条件）
 *
 * When jointype is INNER, joinqual and plan.qual are semantically
 * interchangeable.  For OUTER jointypes, the two are *not* interchangeable;
 * only joinqual is used to determine whether a match has been found for
 * the purpose of deciding whether to generate null-extended tuples.
 * (But plan.qual is still applied before actually returning a tuple.)
 * For an outer join, only joinquals are allowed to be used as the merge
 * or hash condition of a merge or hash join.
 * 当 jointype 为 INNER 时，joinqual 和 plan.qual 在语义上是可以互换的。
 * 对于 OUTER 类型的连接，两者 *不可* 互换；只有 joinqual 用于确定是否找到了匹配项，
 * 以便决定是否生成空值扩展元组。（但在实际返回元组之前，仍会应用 plan.qual。）
 * 对于外部连接，只有 joinqual 允许被用作归并或哈希连接的归并或哈希条件。
 *
 * inner_unique is set if the joinquals are such that no more than one inner
 * tuple could match any given outer tuple.  This allows the executor to
 * skip searching for additional matches.  (This must be provable from just
 * the joinquals, ignoring plan.qual, due to where the executor tests it.)
 * 如果 joinqual 使得对于任何给定的外部元组，最多只有一个内部元组可以匹配，则设置 inner_unique。
 * 这允许执行器跳过搜索其他匹配项。（这必须仅从 joinqual 即可证明，忽略 plan.qual，
 * 因为执行器测试它的位置决定了这一点。）
 * ----------------
 */
typedef struct Join
{
	pg_node_attr(abstract)

	Plan		plan;
	JoinType	jointype;
	bool		inner_unique;
	/* JOIN quals (in addition to plan.qual) */
	List	   *joinqual;
} Join;

/* ----------------
 *		nest loop join node
 *      嵌套循环连接节点
 *
 * The nestParams list identifies any executor Params that must be passed
 * into execution of the inner subplan carrying values from the current row
 * of the outer subplan.  Currently we restrict these values to be simple
 * Vars, but perhaps someday that'd be worth relaxing.  (Note: during plan
 * creation, the paramval can actually be a PlaceHolderVar expression; but it
 * must be a Var with varno OUTER_VAR by the time it gets to the executor.)
 * nestParams 列表标识了必须传递给内部子计划执行的任何执行器 Param，
 * 这些 Param 携带来自外部子计划当前行的值。
 * 目前我们将这些值限制为简单的 Var，但也许有一天放宽这一限制是值得的。
 * （注意：在计划创建期间，paramval 实际上可以是一个 PlaceHolderVar 表达式；
 * 但到它到达执行器时，它必须是一个 varno 为 OUTER_VAR 的 Var。）
 * ----------------
 */
typedef struct NestLoop
{
	Join		join;
	/* list of NestLoopParam nodes */
	List	   *nestParams;
} NestLoop;

typedef struct NestLoopParam
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	/* number of the PARAM_EXEC Param to set */
	/* 要设置的 PARAM_EXEC 参数编号 */
	int			paramno;
	/* outer-relation Var to assign to Param */
	/* 要分配给参数的外部关系变量（Var） */
	Var		   *paramval;
} NestLoopParam;

/* ----------------
 *		merge join node
 *      归并连接节点
 *
 * The expected ordering of each mergeable column is described by a btree
 * opfamily OID, a collation OID, a direction (BTLessStrategyNumber or
 * BTGreaterStrategyNumber) and a nulls-first flag.  Note that the two sides
 * of each mergeclause may be of different datatypes, but they are ordered the
 * same way according to the common opfamily and collation.  The operator in
 * each mergeclause must be an equality operator of the indicated opfamily.
 * 每个可归并列的预期顺序由 btree opfamily OID、排序规则 OID、
 * 方向（BTLessStrategyNumber 或 BTGreaterStrategyNumber）和 nulls-first 标志描述。
 * 请注意，每个 mergeclause 的两侧可能是不同数据类型，但根据通用的 opfamily 和排序规则，
 * 它们以相同的方式排序。每个 mergeclause 中的运算符必须是指定 opfamily 的等值运算符。
 * ----------------
 */
typedef struct MergeJoin
{
	Join		join;

	/* Can we skip mark/restore calls? */
	/* 我们是否可以跳过标记/恢复（mark/restore）调用？ */
	bool		skip_mark_restore;

	/* mergeclauses as expression trees */
	/* 归并子句，作为表达式树 */
	List	   *mergeclauses;

	/* these are arrays, but have the same length as the mergeclauses list: */
	/* 这些是数组，但长度与 mergeclauses 列表相同： */

	/* per-clause OIDs of btree opfamilies */
	/* 每个子句的 btree 操作符族 OID */
	Oid		   *mergeFamilies pg_node_attr(array_size(mergeclauses));

	/* per-clause OIDs of collations */
	/* 每个子句的排序规则 OID */
	Oid		   *mergeCollations pg_node_attr(array_size(mergeclauses));

	/* per-clause ordering (ASC or DESC) */
	/* 每个子句的排序方向 (升序或降序) */
	bool	   *mergeReversals pg_node_attr(array_size(mergeclauses));

	/* per-clause nulls ordering */
	/* 每个子句的空值排序 */
	bool	   *mergeNullsFirst pg_node_attr(array_size(mergeclauses));
} MergeJoin;

/* ----------------
 *		hash join node
 *      哈希连接节点
 * ----------------
 */
typedef struct HashJoin
{
	Join		join;
	/* join clauses for hashjoin */
	/* 哈希连接的连接子句 */
	List	   *hashclauses;
	/* hash operators */
	/* 哈希操作符 */
	List	   *hashoperators;
	/* hash collations */
	/* 哈希排序规则 */
	List	   *hashcollations;

	/*
	 * List of expressions to be hashed for tuples from the outer plan, to
	 * perform lookups in the hashtable over the inner plan.
	 */
	/*
	 * 为来自外部计划的元组进行哈希处理的表达式列表，以便在内部计划生成的哈希表中执行查找。
	 */
	List	   *hashkeys;
} HashJoin;

/* ----------------
 *		materialization node
 *      物化节点
 * ----------------
 */
typedef struct Material
{
	Plan		plan;
} Material;

/* ----------------
 *		memoize node
 *      记忆化（缓存）节点
 * ----------------
 */
typedef struct Memoize
{
	Plan		plan;

	/* size of the two arrays below */
	/* 以下两个数组的大小 */
	int			numKeys;

	/* hash operators for each key */
	/* 每个键的哈希操作符 */
	Oid		   *hashOperators pg_node_attr(array_size(numKeys));

	/* collations for each key */
	/* 每个键的排序规则 */
	Oid		   *collations pg_node_attr(array_size(numKeys));

	/* cache keys in the form of exprs containing parameters */
	/* 含有参数的表达式形式的缓存键 */
	List	   *param_exprs;

	/*
	 * true if the cache entry should be marked as complete after we store the
	 * first tuple in it.
	 */
	/*
	 * 如果在存入第一个元组后应将缓存项标记为完整，则为 true。
	 */
	bool		singlerow;

	/*
	 * true when cache key should be compared bit by bit, false when using
	 * hash equality ops
	 */
	/*
	 * 当缓存键应逐位比较时为 true，使用哈希等值操作符时为 false。
	 */
	bool		binary_mode;

	/*
	 * The maximum number of entries that the planner expects will fit in the
	 * cache, or 0 if unknown
	 */
	/*
	 * 优化器预期缓存中能容纳的最大条目数，如果未知则为 0。
	 */
	uint32		est_entries;

	/* paramids from param_exprs */
	/* 来自 param_exprs 的参数 ID */
	Bitmapset  *keyparamids;
} Memoize;

/* ----------------
 *		sort node
 *      排序节点
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;

	/* number of sort-key columns */
	/* 排序键列的数量 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* OIDs of operators to sort them by */
	/* 用于对它们进行排序的操作符 OID */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* OIDs of collations */
	/* 排序规则 OID */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST directions */
	/* NULLS FIRST/LAST 方向 */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));
} Sort;

/* ----------------
 *		incremental sort node
 *      增量排序节点
 * ----------------
 */
typedef struct IncrementalSort
{
	Sort		sort;
	/* number of presorted columns */
	int			nPresortedCols;
} IncrementalSort;

/* ---------------
 *	 group node -
 *		Used for queries with GROUP BY (but no aggregates) specified.
 *		The input must be presorted according to the grouping columns.
 *   group 节点 -
 *     用于指定了 GROUP BY（但没有聚合函数）的查询。输入必须按照分组列进行预排序。
 * ---------------
 */
typedef struct Group
{
	Plan		plan;

	/* number of grouping columns */
	/* 分组列的数量 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *grpColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	/* 用于比较的等值操作符 */
	Oid		   *grpOperators pg_node_attr(array_size(numCols));
	Oid		   *grpCollations pg_node_attr(array_size(numCols));
} Group;

/* ---------------
 *		aggregate node
 *      聚合节点
 *
 * An Agg node implements plain or grouped aggregation.  For grouped
 * aggregation, we can work with presorted input or unsorted input;
 * the latter strategy uses an internal hashtable.
 * Agg 节点实现普通聚合或分组聚合。对于分组聚合，我们可以处理预排序输入或未排序输入；
 * 后一种策略使用内部哈希表。
 *
 * Notice the lack of any direct info about the aggregate functions to be
 * computed.  They are found by scanning the node's tlist and quals during
 * executor startup.  (It is possible that there are no aggregate functions;
 * this could happen if they get optimized away by constant-folding, or if
 * we are using the Agg node to implement hash-based grouping.)
 * 注意缺少关于要计算的聚合函数的任何直接信息。
 * 它们是在执行器启动期间通过扫描节点的 tlist 和 qual 找到的。
 * （可能没有聚合函数；如果它们被常量折叠优化掉了，或者如果我们使用 Agg 节点来实现基于哈希的分组，就会发生这种情况。）
 * ---------------
 */
typedef struct Agg
{
	Plan		plan;

	/* basic strategy, see nodes.h */
	/* 基本策略，见 nodes.h */
	AggStrategy aggstrategy;

	/* agg-splitting mode, see nodes.h */
	/* 聚合拆分模式，见 nodes.h */
	AggSplit	aggsplit;

	/* number of grouping columns */
	/* 分组列的数量 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *grpColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	/* 用于比较的等值操作符 */
	Oid		   *grpOperators pg_node_attr(array_size(numCols));
	Oid		   *grpCollations pg_node_attr(array_size(numCols));

	/* estimated number of groups in input */
	/* 输入中估计的组数 */
	long		numGroups;

	/* for pass-by-ref transition data */
	/* 用于按引用传递的转换数据 */
	uint64		transitionSpace;

	/* IDs of Params used in Aggref inputs */
	/* Aggref 输入中使用的参数 ID */
	Bitmapset  *aggParams;

	/* Note: planner provides numGroups & aggParams only in HASHED/MIXED case */
	/* 注意：优化器仅在 HASHED/MIXED 情况下提供 numGroups 和 aggParams */

	/* grouping sets to use */
	/* 要使用的分组集 */
	List	   *groupingSets;

	/* chained Agg/Sort nodes */
	/* 链接的 Agg/Sort 节点 */
	List	   *chain;
} Agg;

/* ----------------
 *		window aggregate node
 *      窗口聚合节点
 * ----------------
 */
typedef struct WindowAgg
{
	Plan		plan;

	/* name of WindowClause implemented by this node */
	/* 该节点实现的 WindowClause 的名称 */
	char	   *winname;

	/* ID referenced by window functions */
	/* 窗口函数引用的 ID */
	Index		winref;

	/* number of columns in partition clause */
	/* 分区子句中的列数 */
	int			partNumCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *partColIdx pg_node_attr(array_size(partNumCols));

	/* equality operators for partition columns */
	/* 分区列的等值操作符 */
	Oid		   *partOperators pg_node_attr(array_size(partNumCols));

	/* collations for partition columns */
	/* 分区列的排序规则 */
	Oid		   *partCollations pg_node_attr(array_size(partNumCols));

	/* number of columns in ordering clause */
	/* 排序子句中的列数 */
	int			ordNumCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *ordColIdx pg_node_attr(array_size(ordNumCols));

	/* equality operators for ordering columns */
	/* 排序列的等值操作符 */
	Oid		   *ordOperators pg_node_attr(array_size(ordNumCols));

	/* collations for ordering columns */
	/* 排序列的排序规则 */
	Oid		   *ordCollations pg_node_attr(array_size(ordNumCols));

	/* frame_clause options, see WindowDef */
	/* 帧子句选项，见 WindowDef */
	int			frameOptions;

	/* expression for starting bound, if any */
	/* 起始边界的表达式（如果有） */
	Node	   *startOffset;

	/* expression for ending bound, if any */
	/* 结束边界的表达式（如果有） */
	Node	   *endOffset;

	/* qual to help short-circuit execution */
	/* 帮助短路执行的限定条件 */
	List	   *runCondition;

	/* runCondition for display in EXPLAIN */
	/* 用于 EXPLAIN 显示的运行条件 */
	List	   *runConditionOrig;

	/* these fields are used with RANGE offset PRECEDING/FOLLOWING: */
	/* 这些字段用于带有偏移量的 RANGE PRECEDING/FOLLOWING： */

	/* in_range function for startOffset */
	/* 用于 startOffset 的 in_range 函数 */
	Oid			startInRangeFunc;

	/* in_range function for endOffset */
	/* 用于 endOffset 的 in_range 函数 */
	Oid			endInRangeFunc;

	/* collation for in_range tests */
	/* 用于 in_range 测试的排序规则 */
	Oid			inRangeColl;

	/* use ASC sort order for in_range tests? */
	/* 在 in_range 测试中使用升序排列？ */
	bool		inRangeAsc;

	/* nulls sort first for in_range tests? */
	/* 在 in_range 测试中空值排在前面？ */
	bool		inRangeNullsFirst;

	/*
	 * false for all apart from the WindowAgg that's closest to the root of
	 * the plan
	 */
	/*
	 * 除了最靠近计划根节点的 WindowAgg 之外，其余均为 false
	 */
	bool		topWindow;
} WindowAgg;

/* ----------------
 *		unique node
 *      唯一（去重）节点
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;

	/* number of columns to check for uniqueness */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *uniqColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	Oid		   *uniqOperators pg_node_attr(array_size(numCols));

	/* collations for equality comparisons */
	Oid		   *uniqCollations pg_node_attr(array_size(numCols));
} Unique;

/* ------------
 *		gather node
 *      收集（并行执行）节点
 *
 * Note: rescan_param is the ID of a PARAM_EXEC parameter slot.  That slot
 * will never actually contain a value, but the Gather node must flag it as
 * having changed whenever it is rescanned.  The child parallel-aware scan
 * nodes are marked as depending on that parameter, so that the rescan
 * machinery is aware that their output is likely to change across rescans.
 * In some cases we don't need a rescan Param, so rescan_param is set to -1.
 * 注意：rescan_param 是 PARAM_EXEC 参数槽的 ID。该槽位实际上永远不会包含值，
 * 但每当重新扫描时，Gather 节点必须将其标记为已更改。
 * 并行感知的子扫描节点被标记为依赖于该参数，以便重新扫描机制意识到它们的输出可能在重新扫描期间发生变化。
 * 在某些情况下，我们不需要 rescan Param，因此 rescan_param 设置为 -1。
 * ------------
 */
typedef struct Gather
{
	Plan		plan;
	/* planned number of worker processes */
	/* 计划的工作进程数量 */
	int			num_workers;
	/* ID of Param that signals a rescan, or -1 */
	/* 发出重新扫描信号的参数 ID，或 -1 */
	int			rescan_param;
	/* don't execute plan more than once */
	/* 该计划执行次数不超过一次 */
	bool		single_copy;
	/* suppress EXPLAIN display (for testing)? */
	/* 禁止 EXPLAIN 显示（用于测试）？ */
	bool		invisible;

	/*
	 * param id's of initplans which are referred at gather or one of its
	 * child nodes
	 */
	/*
	 * 在 gather 或其子节点中引用的 initplans 的参数 ID
	 */
	Bitmapset  *initParam;
} Gather;

/* ------------
 *		gather merge node
 *      收集归并节点
 * ------------
 */
typedef struct GatherMerge
{
	Plan		plan;

	/* planned number of worker processes */
	/* 计划的工作进程数量 */
	int			num_workers;

	/* ID of Param that signals a rescan, or -1 */
	/* 发出重新扫描信号的参数 ID，或 -1 */
	int			rescan_param;

	/* remaining fields are just like the sort-key info in struct Sort */
	/* 其余字段就像 Sort 结构中的排序键信息 */

	/* number of sort-key columns */
	/* 排序键列的数量 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* OIDs of operators to sort them by */
	/* 用于对它们进行排序的操作符 OID */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* OIDs of collations */
	/* 排序规则 OID */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST directions */
	/* NULLS FIRST/LAST 方向 */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));

	/*
	 * param id's of initplans which are referred at gather merge or one of
	 * its child nodes
	 */
	/*
	 * 在 gather merge 或其子节点中引用的 initplans 的参数 ID
	 */
	Bitmapset  *initParam;
} GatherMerge;

/* ----------------
 *		hash build node
 *      哈希构建节点
 *
 * If the executor is supposed to try to apply skew join optimization, then
 * skewTable/skewColumn/skewInherit identify the outer relation's join key
 * column, from which the relevant MCV statistics can be fetched.
 * 如果执行器应该尝试应用倾斜连接优化（skew join optimization），则
 * skewTable/skewColumn/skewInherit 标识外部关系的连接键列，
 * 从而可以从中获取相关的 MCV 统计信息。
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;

	/*
	 * List of expressions to be hashed for tuples from Hash's outer plan,
	 * needed to put them into the hashtable.
	 */
	/* hash keys for the hashjoin condition */
	/* 用于哈希连接条件的哈希键 */
	List	   *hashkeys;
	/* outer join key's table OID, or InvalidOid */
	/* 外部连接键的表 OID，或 InvalidOid */
	Oid			skewTable;
	/* outer join key's column #, or zero */
	/* 外部连接键的列号，或零 */
	AttrNumber	skewColumn;
	/* is outer join rel an inheritance tree? */
	/* 外部连接关系是否为继承树？ */
	bool		skewInherit;
	/* all other info is in the parent HashJoin node */
	/* 所有其他信息都在父 HashJoin 节点中 */
	/* estimate total rows if parallel_aware */
	/* 如果是并行感知的，估算总行数 */
	Cardinality rows_total;
} Hash;

/* ----------------
 *		setop node
 *      集合操作节点
 * ----------------
 */
typedef struct SetOp
{
	Plan		plan;

	/* what to do, see nodes.h */
	/* 要执行的操作，见 nodes.h */
	SetOpCmd	cmd;

	/* how to do it, see nodes.h */
	/* 执行策略，见 nodes.h */
	SetOpStrategy strategy;

	/* number of columns to compare */
	/* 要比较的列数 */
	int			numCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *cmpColIdx pg_node_attr(array_size(numCols));

	/* comparison operators (either equality operators or sort operators) */
	/* 比较操作符（等值操作符或排序操作符） */
	Oid		   *cmpOperators pg_node_attr(array_size(numCols));
	Oid		   *cmpCollations pg_node_attr(array_size(numCols));

	/* nulls-first flags if sorting, otherwise not interesting */
	/* 如果是排序，则为 nulls-first 标志，否则无所谓 */
	bool	   *cmpNullsFirst pg_node_attr(array_size(numCols));

	/* estimated number of groups in left input */
	/* 左输入中估计的组数 */
	long		numGroups;
} SetOp;

/* ----------------
 *		lock-rows node
 *      行锁定节点
 *
 * rowMarks identifies the rels to be locked by this node; it should be
 * a subset of the rowMarks listed in the top-level PlannedStmt.
 * epqParam is a Param that all scan nodes below this one must depend on.
 * It is used to force re-evaluation of the plan during EvalPlanQual.
 * rowMarks 标识了此节点要锁定的关系；它应该是顶级 PlannedStmt 中列出的 rowMarks 的子集。
 * epqParam 是一个 Param，其下的所有扫描节点都必须依赖于该 Param。
 * 它用于在 EvalPlanQual 期间强制重新评估计划。
 * ----------------
 */
typedef struct LockRows
{
	Plan		plan;
	/* a list of PlanRowMark's */
	List	   *rowMarks;
	/* ID of Param for EvalPlanQual re-eval */
	int			epqParam;
} LockRows;

/* ----------------
 *		limit node
 *      限制（Limit/Offset）节点
 *
 * Note: as of Postgres 8.2, the offset and count expressions are expected
 * to yield int8, rather than int4 as before.
 * 注意：从 Postgres 8.2 开始，offset 和 count 表达式预期产生 int8，而不是之前的 int4。
 * ----------------
 */
typedef struct Limit
{
	Plan		plan;

	/* OFFSET parameter, or NULL if none */
	/* OFFSET 参数，如果没有则为 NULL */
	Node	   *limitOffset;

	/* COUNT parameter, or NULL if none */
	/* COUNT 参数，如果没有则为 NULL */
	Node	   *limitCount;

	/* limit type */
	/* 限制类型 */
	LimitOption limitOption;

	/* number of columns to check for similarity  */
	/* 要检查相似性的列数 */
	int			uniqNumCols;

	/* their indexes in the target list */
	/* 它们在目标列表中的索引 */
	AttrNumber *uniqColIdx pg_node_attr(array_size(uniqNumCols));

	/* equality operators to compare with */
	/* 用于比较的等值操作符 */
	Oid		   *uniqOperators pg_node_attr(array_size(uniqNumCols));

	/* collations for equality comparisons */
	/* 用于等值比较的排序规则 */
	Oid		   *uniqCollations pg_node_attr(array_size(uniqNumCols));
} Limit;


/*
 * RowMarkType -
 *	  enums for types of row-marking operations
 *    行标记操作类型的枚举
 *
 * The first four of these values represent different lock strengths that
 * we can take on tuples according to SELECT FOR [KEY] UPDATE/SHARE requests.
 * We support these on regular tables, as well as on foreign tables whose FDWs
 * report support for late locking.  For other foreign tables, any locking
 * that might be done for such requests must happen during the initial row
 * fetch; their FDWs provide no mechanism for going back to lock a row later.
 * These means that the semantics will be a bit different than for a local
 * table; in particular we are likely to lock more rows than would be locked
 * locally, since remote rows will be locked even if they then fail
 * locally-checked restriction or join quals.  However, the prospect of
 * doing a separate remote query to lock each selected row is usually pretty
 * unappealing, so early locking remains a credible design choice for FDWs.
 * 前四个值代表根据 SELECT FOR [KEY] UPDATE/SHARE 请求，我们可以对元组采取的不同锁定强度。
 * 我们在常规表以及报告支持延迟锁定（late locking）的 FDW 的外部表上支持这些。
 * 对于其他外部表，为此类请求可能进行的任何锁定都必须在初始行提取期间发生；
 * 它们的 FDW 没提供稍后返回以锁定行的机制。这意味着语义将与本地表略有不同；
 * 特别是，我们可能会锁定比本地锁定更多的行，因为即使远程行随后未能通过本地检查的限制或连接条件，
 * 它们也会被锁定。然而，进行单独的远程查询来锁定每个选定的行通常是非常乏味的，
 * 因此早期锁定对于 FDW 来说仍然是一个可靠的设计选择。
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE, we have to uniquely
 * identify all the source rows, not only those from the target relations, so
 * that we can perform EvalPlanQual rechecking at need.  For plain tables we
 * can just fetch the TID, much as for a target relation; this case is
 * represented by ROW_MARK_REFERENCE.  Otherwise (for example for VALUES or
 * FUNCTION scans) we have to copy the whole row value.  ROW_MARK_COPY is
 * pretty inefficient, since most of the time we'll never need the data; but
 * fortunately the overhead is usually not performance-critical in practice.
 * By default we use ROW_MARK_COPY for foreign tables, but if the FDW has
 * a concept of rowid it can request to use ROW_MARK_REFERENCE instead.
 * (Again, this probably doesn't make sense if a physical remote fetch is
 * needed, but for FDWs that map to local storage it might be credible.)
 * 在执行 UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE 时，我们必须唯一标识所有源行，
 * 而不仅仅是目标关系中的行，以便在需要时执行 EvalPlanQual 重新检查。
 * 对于普通表，我们可以只获取 TID，就像目标关系一样；这种情况由 ROW_MARK_REFERENCE 表示。
 * 否则（例如对于 VALUES 或 FUNCTION 扫描），我们必须复制整个行值。
 * ROW_MARK_COPY 效率很低，因为大多数时候我们永远不需要这些数据；
 * 但幸运的是，在实践中这种开销通常对性能并不关键。
 * 默认情况下，我们对外部表使用 ROW_MARK_COPY，但如果 FDW 有 rowid 的概念，
 * 它可以请求改用 ROW_MARK_REFERENCE。（同样，如果需要物理远程提取，这可能没有意义，
 * 但对于映射到本地存储的 FDW，这可能是可靠的。）
 */
typedef enum RowMarkType
{
	ROW_MARK_EXCLUSIVE,			/* obtain exclusive tuple lock */
	ROW_MARK_NOKEYEXCLUSIVE,	/* obtain no-key exclusive tuple lock */
	ROW_MARK_SHARE,				/* obtain shared tuple lock */
	ROW_MARK_KEYSHARE,			/* obtain keyshare tuple lock */
	ROW_MARK_REFERENCE,			/* just fetch the TID, don't lock it */
	ROW_MARK_COPY,				/* physically copy the row value */
} RowMarkType;

#define RowMarkRequiresRowShareLock(marktype)  ((marktype) <= ROW_MARK_KEYSHARE)

/*
 * PlanRowMark -
 *	   plan-time representation of FOR [KEY] UPDATE/SHARE clauses
 *     FOR [KEY] UPDATE/SHARE 子句的计划时表示
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE, we create a separate
 * PlanRowMark node for each non-target relation in the query.  Relations that
 * are not specified as FOR UPDATE/SHARE are marked ROW_MARK_REFERENCE (if
 * regular tables or supported foreign tables) or ROW_MARK_COPY (if not).
 * 在执行 UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE 时，
 * 我们会为查询中的每个非目标关系创建一个单独的 PlanRowMark 节点。
 * 未指定为 FOR UPDATE/SHARE 的关系被标记为 ROW_MARK_REFERENCE（如果是常规表或支持的外部表）
 * 或 ROW_MARK_COPY（如果不是）。
 *
 * Initially all PlanRowMarks have rti == prti and isParent == false.
 * When the planner discovers that a relation is the root of an inheritance
 * tree, it sets isParent true, and adds an additional PlanRowMark to the
 * list for each child relation (including the target rel itself in its role
 * as a child, if it is not a partitioned table).  Any non-leaf partitioned
 * child relations will also have entries with isParent = true.  The child
 * entries have rti == child rel's RT index and prti == top parent's RT index,
 * and can therefore be recognized as children by the fact that prti != rti.
 * The parent's allMarkTypes field gets the OR of (1<<markType) across all
 * its children (this definition allows children to use different markTypes).
 * 最初，所有 PlanRowMark 的 rti == prti 且 isParent == false。
 * 当优化器发现一个关系是继承树的根时，它将 isParent 设置为 true，
 * 并且为每个子关系（包括处于子关系角色下的目标关系本身，如果它不是分区表）在列表中添加一个额外的 PlanRowMark。
 * 任何非叶子分区子关系也将具有 isParent = true 的条目。
 * 子条目的 rti == 子关系的 RT 索引，且 prti == 顶级父关系的 RT 索引，
 * 因此可以通过 prti != rti 这一事实将其识别为子条目。
 * 父条目的 allMarkTypes 字段获得所有子条目 (1<<markType) 的按位或结果（这个定义允许子条目使用不同的 markType）。
 *
 * The planner also adds resjunk output columns to the plan that carry
 * information sufficient to identify the locked or fetched rows.  When
 * markType != ROW_MARK_COPY, these columns are named
 *		tableoid%u			OID of table
 *		ctid%u				TID of row
 * The tableoid column is only present for an inheritance hierarchy.
 * When markType == ROW_MARK_COPY, there is instead a single column named
 *		wholerow%u			whole-row value of relation
 * (An inheritance hierarchy could have all three resjunk output columns,
 * if some children use a different markType than others.)
 * In all three cases, %u represents the rowmark ID number (rowmarkId).
 * This number is unique within a plan tree, except that child relation
 * entries copy their parent's rowmarkId.  (Assigning unique numbers
 * means we needn't renumber rowmarkIds when flattening subqueries, which
 * would require finding and renaming the resjunk columns as well.)
 * Note this means that all tables in an inheritance hierarchy share the
 * same resjunk column names.
 * 优化器还在计划中添加了 resjunk 输出列，这些列携带足以标识已锁定或已提取行的数据。
 * 当 markType != ROW_MARK_COPY 时，这些列被命名为 tableoid%u（表 OID）和 ctid%u（行 TID）。
 * tableoid 列仅在继承层次结构中存在。当 markType == ROW_MARK_COPY 时，
 * 替换为一个名为 wholerow%u 的单列，包含关系的整行值。
 * （如果一些子关系使用与其他子关系不同的 markType，继承层次结构可能拥有所有三个 resjunk 输出列。）
 * 在这三种情况下，%u 代表行标记 ID 号（rowmarkId）。该编号在计划树中是唯一的，
 * 除非子关系条目复制其父关系的 rowmarkId。（分配唯一编号意味着我们在展平子查询时不需要重新编号 rowmarkId，
 * 否则还需要查找并重命名 resjunk 列。）注意这意味着继承层次结构中的所有表共享相同的 resjunk 列名。
 */
typedef struct PlanRowMark
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	/* range table index of markable relation */
	/* 可标记关系的范围表索引 */
	Index		rti;
	/* range table index of parent relation */
	/* 父关系的范围表索引 */
	Index		prti;
	/* unique identifier for resjunk columns */
	/* resjunk 列的唯一标识符 */
	Index		rowmarkId;
	/* see enum above */
	/* 见上方的枚举 */
	RowMarkType markType;
	/* OR of (1<<markType) for all children */
	/* 所有子关系的 (1<<markType) 的按位或结果 */
	int			allMarkTypes;
	/* LockingClause's strength, or LCS_NONE */
	/* LockingClause 的强度，或 LCS_NONE */
	LockClauseStrength strength;
	/* NOWAIT and SKIP LOCKED options */
	/* NOWAIT 和 SKIP LOCKED 选项 */
	LockWaitPolicy waitPolicy;
	/* true if this is a "dummy" parent entry */
	/* 如果这是“虚拟”父条目，则为 true */
	bool		isParent;
} PlanRowMark;


/*
 * Node types to represent partition pruning information.
 */

/*
 * PartitionPruneInfo - Details required to allow the executor to prune
 * partitions.
 * PartitionPruneInfo - 允许执行器剪枝分区的必需详情。
 *
 * Here we store mapping details to allow translation of a partitioned table's
 * index as returned by the partition pruning code into subplan indexes for
 * plan types which support arbitrary numbers of subplans, such as Append.
 * We also store various details to tell the executor when it should be
 * performing partition pruning.
 * 在这里，我们存储映射详情，以便将分区剪枝代码返回的分区表索引转换，为支持任意数量子计划的计划类型
 * （如 Append）的子计划索引。我们还存储各种细节，以告诉执行器何时应执行分区剪枝。
 *
 * Each PartitionedRelPruneInfo describes the partitioning rules for a single
 * partitioned table (a/k/a level of partitioning).  Since a partitioning
 * hierarchy could contain multiple levels, we represent it by a List of
 * PartitionedRelPruneInfos, where the first entry represents the topmost
 * partitioned table and additional entries represent non-leaf child
 * partitions, ordered such that parents appear before their children.
 * Then, since an Append-type node could have multiple partitioning
 * hierarchies among its children, we have an unordered List of those Lists.
 * 每个 PartitionedRelPruneInfo 描述单个分区表（也称为分区级别）的分区规则。
 * 由于分区层次结构可能包含多个级别，我们用 PartitionedRelPruneInfo 列表来表示它，
 * 其中第一个条目代表最顶层的分区表，其他条目代表非叶子子分区，按父级出现在子级之前的顺序排列。
 * 然后，由于 Append 类型的节点在其子节点中可能具有多个分区层次结构，因此我们拥有这些列表的一个无序列表。
 *
 * relids				RelOptInfo.relids of the parent plan node (e.g. Append
 *						or MergeAppend) to which this PartitionPruneInfo node
 *						belongs.  The pruning logic ensures that this matches
 *						the parent plan node's apprelids.
 * prune_infos			List of Lists containing PartitionedRelPruneInfo nodes,
 *						one sublist per run-time-prunable partition hierarchy
 *						appearing in the parent plan node's subplans.
 * other_subplans		Indexes of any subplans that are not accounted for
 *						by any of the PartitionedRelPruneInfo nodes in
 *						"prune_infos".  These subplans must not be pruned.
 * relids: 此 PartitionPruneInfo 节点所属的父计划节点（例如 Append 或 MergeAppend）的 RelOptInfo.relids。
 *         剪枝逻辑确保这与父计划节点的 apprelids 匹配。
 * prune_infos: 包含 PartitionedRelPruneInfo 节点的列表的列表，每个运行时可剪枝的分区层次结构一个子列表，
 *              这些层次结构出现在父计划节点的子计划中。
 * other_subplans: 任何未由 "prune_infos" 中的 PartitionedRelPruneInfo 节点说明的子计划索引。
 *                 这些子计划不得被剪枝。
 */
typedef struct PartitionPruneInfo
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	Bitmapset  *relids;
	List	   *prune_infos;
	Bitmapset  *other_subplans;
} PartitionPruneInfo;

/*
 * PartitionedRelPruneInfo - Details required to allow the executor to prune
 * partitions for a single partitioned table.
 * PartitionedRelPruneInfo - 允许执行器剪枝单个分区表的必需详情。
 *
 * subplan_map[], subpart_map[], and leafpart_rti_map[] are indexed by partition
 * index of the partitioned table referenced by 'rtindex', the partition index
 * being the order that the partitions are defined in the table's
 * PartitionDesc.  For a leaf partition p, subplan_map[p] contains the
 * zero-based index of the partition's subplan in the parent plan's subplan
 * list; it is -1 if the partition is non-leaf or has been pruned.  For a
 * non-leaf partition p, subpart_map[p] contains the zero-based index of that
 * sub-partition's PartitionedRelPruneInfo in the hierarchy's
 * PartitionedRelPruneInfo list; it is -1 if the partition is a leaf or has
 * been pruned.  leafpart_rti_map[p] contains the RT index of a leaf partition
 * if its subplan is in the parent plan' subplan list; it is 0 either if the
 * partition is non-leaf or it is leaf but has been pruned during planning.
 * Note that subplan indexes, as stored in 'subplan_map', are global across the
 * parent plan node, but partition indexes are valid only within a particular
 * hierarchy.  relid_map[p] contains the partition's OID, or 0 if the partition
 * was pruned.
 * subplan_map[]、subpart_map[] 和 leafpart_rti_map[] 由 'rtindex' 引用的分区表的分区索引索引，
 * 该分区索引是分区在表的 PartitionDesc 中定义的顺序。
 * 对于叶子分区 p，subplan_map[p] 包含该分区子计划在父计划子计划列表中的从零开始的索引；
 * 如果分区是非叶子分区或已被剪枝，则为 -1。
 * 对于非叶子分区 p，subpart_map[p] 包含该子分区的 PartitionedRelPruneInfo 在层次结构的
 * PartitionedRelPruneInfo 列表中的从零开始的索引；如果分区是叶子分区或已被剪枝，则为 -1。
 * leafpart_rti_map[p] 包含叶子分区的 RT 索引，如果其子计划在父计划的子计划列表中；
 * 如果分区是非叶子分区或它是叶子分区但在规划期间被剪枝，则为 0。
 * 注意，存储在 'subplan_map' 中的子计划索引在父计划节点范围内是全局的，
 * 但分区索引仅在特定层次结构内有效。relid_map[p] 包含分区的 OID，如果分区被剪枝则为 0。
 */
typedef struct PartitionedRelPruneInfo
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;

	/* RT index of partition rel for this level */
	/* 当前级别分区关系的 RT 索引 */
	Index		rtindex;

	/* Indexes of all partitions which subplans or subparts are present for */
	/* 存在子计划或子分区的多有分区的索引 */
	Bitmapset  *present_parts;

	/* Length of the following arrays: */
	/* 以下数组的长度： */
	int			nparts;

	/* subplan index by partition index, or -1 */
	/* 按分区索引排列的子计划索引，或 -1 */
	int		   *subplan_map pg_node_attr(array_size(nparts));

	/* subpart index by partition index, or -1 */
	/* 按分区索引排列的子分区索引，或 -1 */
	int		   *subpart_map pg_node_attr(array_size(nparts));

	/* RT index by partition index, or 0 */
	/* 按分区索引排列的 RT 索引，或 0 */
	int		   *leafpart_rti_map pg_node_attr(array_size(nparts));

	/* relation OID by partition index, or 0 */
	/* 按分区索引排列的关系 OID，或 0 */
	Oid		   *relid_map pg_node_attr(array_size(nparts));

	/*
	 * initial_pruning_steps shows how to prune during executor startup (i.e.,
	 * without use of any PARAM_EXEC Params); it is NIL if no startup pruning
	 * is required.  exec_pruning_steps shows how to prune with PARAM_EXEC
	 * Params; it is NIL if no per-scan pruning is required.
	 */
	/*
	 * initial_pruning_steps 显示了如何在执行器启动期间进行剪枝（即不使用任何 PARAM_EXEC 参数）；
	 * 如果不需要启动时剪枝，则为 NIL。
	 * exec_pruning_steps 显示了如何使用 PARAM_EXEC 参数进行剪枝；如果不需要每次扫描剪枝，则为 NIL。
	 */
	/* List of PartitionPruneStep */
	/* PartitionPruneStep 列表 */
	List	   *initial_pruning_steps;
	/* List of PartitionPruneStep */
	/* PartitionPruneStep 列表 */
	List	   *exec_pruning_steps;

	/* All PARAM_EXEC Param IDs in exec_pruning_steps */
	/* exec_pruning_steps 中的所有 PARAM_EXEC 参数 ID */
	Bitmapset  *execparamids;
} PartitionedRelPruneInfo;

/*
 * Abstract Node type for partition pruning steps (there are no concrete
 * Nodes of this type).
 * 用于分区剪枝步骤的抽象节点类型（没有此类型的具体节点）。
 *
 * step_id is the global identifier of the step within its pruning context.
 * step_id 是该步骤在其剪枝上下文中的全局标识符。
 */
typedef struct PartitionPruneStep
{
	pg_node_attr(abstract, no_equal, no_query_jumble)

	NodeTag		type;
	int			step_id;
} PartitionPruneStep;
/* PartitionPruneStep: 抽象节点类型，用于表示分区剪枝的一个步骤。 */

/*
 * PartitionPruneStepOp - Information to prune using a set of mutually ANDed
 *							OpExpr clauses
 * PartitionPruneStepOp - 使用一组互相关联（ANDed）的 OpExpr 子句进行剪枝的信息
 *
 * This contains information extracted from up to partnatts OpExpr clauses,
 * where partnatts is the number of partition key columns.  'opstrategy' is the
 * strategy of the operator in the clause matched to the last partition key.
 * 'exprs' contains expressions which comprise the lookup key to be passed to
 * the partition bound search function.  'cmpfns' contains the OIDs of
 * comparison functions used to compare aforementioned expressions with
 * partition bounds.  Both 'exprs' and 'cmpfns' contain the same number of
 * items, up to partnatts items.
 * 这包含从最多 partnatts 个 OpExpr 子句中提取的信息，其中 partnatts 是分区键列的数量。
 * 'opstrategy' 是与最后一个分区键匹配的子句中操作符的策略。
 * 'exprs' 包含构成要传递给分区边界搜索函数的查找键的表达式。
 * 'cmpfns' 包含用于将上述表达式与分区边界进行比较的比较操作符的 OID。
 * 'exprs' 和 'cmpfns' 都包含相同数量的条目，最多 partnatts 个。
 *
 * Once we find the offset of a partition bound using the lookup key, we
 * determine which partitions to include in the result based on the value of
 * 'opstrategy'.  For example, if it were equality, we'd return just the
 * partition that would contain that key or a set of partitions if the key
 * didn't consist of all partitioning columns.  For non-equality strategies,
 * we'd need to include other partitions as appropriate.
 * 一旦我们使用查找键找到分区边界的偏移量，我们就根据 'opstrategy' 的值确定结果中应包含哪些分区。
 * 例如，如果是等值（equality），我们就只返回包含该键的分区，如果该键不包含所有分区列，则返回一组分区。
 * 对于非等值策略，我们需要根据需要包含其他分区。
 *
 * 'nullkeys' is the set containing the offset of the partition keys (0 to
 * partnatts - 1) that were matched to an IS NULL clause.  This is only
 * considered for hash partitioning as we need to pass which keys are null
 * to the hash partition bound search function.  It is never possible to
 * have an expression be present in 'exprs' for a given partition key and
 * the corresponding bit set in 'nullkeys'.
 * 'nullkeys' 是包含匹配 IS NULL 子句的分区键偏移量（0 到 partnatts - 1）的集合。
 * 仅哈希分区考虑这一点，因为我们需要将哪些键为 null 传递给哈希分区边界搜索函数。
 * 对于给定的分区键，绝不可能在 'exprs' 中存在表达式的同时在 'nullkeys' 中设置相应的位。
 */
typedef struct PartitionPruneStepOp
{
	PartitionPruneStep step;

	StrategyNumber opstrategy;
	List	   *exprs;
	List	   *cmpfns;
	Bitmapset  *nullkeys;
} PartitionPruneStepOp;

/*
 * PartitionPruneStepCombine - Information to prune using a BoolExpr clause
 * PartitionPruneStepCombine - 使用 BoolExpr 子句进行剪枝的信息
 *
 * For BoolExpr clauses, we combine the set of partitions determined for each
 * of the argument clauses.
 * 对于 BoolExpr 子句，我们合并为每个参数子句确定的分区集。
 */
typedef enum PartitionPruneCombineOp
{
	PARTPRUNE_COMBINE_UNION,
	PARTPRUNE_COMBINE_INTERSECT,
} PartitionPruneCombineOp;

typedef struct PartitionPruneStepCombine
{
	PartitionPruneStep step;

	PartitionPruneCombineOp combineOp;
	List	   *source_stepids;
} PartitionPruneStepCombine;
/* PartitionPruneStepCombine: 合并多个剪枝步骤结果的节点。 */


/*
 * Plan invalidation info
 * 计划失效信息
 *
 * We track the objects on which a PlannedStmt depends in two ways:
 * relations are recorded as a simple list of OIDs, and everything else
 * is represented as a list of PlanInvalItems.  A PlanInvalItem is designed
 * to be used with the syscache invalidation mechanism, so it identifies a
 * system catalog entry by cache ID and hash value.
 * 我们通过两种方式跟踪 PlannedStmt 所依赖的对象：
 * 关系被记录为 OID 的简单列表，其他所有内容表示为 PlanInvalItems 列表。
 * PlanInvalItem 旨在与 syscache 失效机制配合使用，因此它通过缓存 ID 和哈希值来标识系统目录条目。
 */
typedef struct PlanInvalItem
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	/* a syscache ID, see utils/syscache.h */
	/* syscache ID，见 utils/syscache.h */
	int			cacheId;
	/* hash value of object's cache lookup key */
	/* 对象缓存查找键的哈希值 */
	uint32		hashValue;
} PlanInvalItem;

/*
 * MonotonicFunction
 * 单调函数
 *
 * Allows the planner to track monotonic properties of functions.  A function
 * is monotonically increasing if a subsequent call cannot yield a lower value
 * than the previous call.  A monotonically decreasing function cannot yield a
 * higher value on subsequent calls, and a function which is both must return
 * the same value on each call.
 * 允许优化器跟踪函数的单调性质。
 * 如果后续调用产生的数值不低于之前调用的数值，则该函数是单调递增的。
 * 单调递减函数后续产生的数值不能高于之前的调用，而两者皆是的函数每次调用必须返回相同的值。
 */
typedef enum MonotonicFunction
{
	MONOTONICFUNC_NONE = 0,
	MONOTONICFUNC_INCREASING = (1 << 0),
	MONOTONICFUNC_DECREASING = (1 << 1),
	MONOTONICFUNC_BOTH = MONOTONICFUNC_INCREASING | MONOTONICFUNC_DECREASING,
} MonotonicFunction;

/*
 * Functions and Flow Summary / 函数与流程总结:
 * 1. exec_subplan_get_plan: Retrieves the Plan node for a SubPlan expressions.
 *    获取 SubPlan 表达式对应的 Plan 节点。
 * 2. innerPlan / outerPlan: Macros for accessing child nodes (right/left tree).
 *    访问子节点（左右树）的宏。
 * 3. Execution Flow: The executor processes PlannedStmt, traverses the PlanTree,
 *    and uses individual nodes (Scan, Join, Agg) to pump data through the system.
 *    执行流程：执行器处理 PlannedStmt，遍历计划树（PlanTree），并利用各个节点（扫描、连接、聚合）在系统中驱动数据。
 */

#endif							/* PLANNODES_H */
