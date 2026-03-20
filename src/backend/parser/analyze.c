/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  transform the raw parse tree into a query tree
 *
 * 将原始解析树转换为查询树
 *
 * For optimizable statements, we are careful to obtain a suitable lock on
 * each referenced table, and other modules of the backend preserve or
 * re-obtain these locks before depending on the results.  It is therefore
 * okay to do significant semantic analysis of these statements.  For
 * utility commands, no locks are obtained here (and if they were, we could
 * not be sure we'd still have them at execution).  Hence the general rule
 * for utility commands is to just dump them into a Query node untransformed.
 * DECLARE CURSOR, EXPLAIN, and CREATE TABLE AS are exceptions because they
 * contain optimizable statements, which we should transform.
 *
 * 对于可优化的语句，我们小心地在每个引用的表上获取合适的锁，而后端的其他模块在依赖结果之前
 * 会保留或重新获取这些锁。因此，对这些语句进行显著的语义分析是可以的。对于实用程序命令，
 * 此处不获取锁（如果获取了，我们也无法确定在执行时仍然拥有它们）。因此，实用程序命令的
 * 通用规则是直接将它们转换成 Query 节点而不进行转换。DECLARE CURSOR、EXPLAIN 和
 * CREATE TABLE AS 是例外，因为它们包含可优化的语句，我们应该对其进行转换。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/parser/analyze.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_merge.h"
#include "parser/parse_oper.h"
#include "parser/parse_param.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/* Hook for plugins to get control at end of parse analysis */
/* 供插件在解析分析结束时获得控制权的钩子 */
post_parse_analyze_hook_type post_parse_analyze_hook = NULL;

static Query *transformOptionalSelectInto(ParseState *pstate, Node *parseTree);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, InsertStmt *stmt);
static OnConflictExpr *transformOnConflictClause(ParseState *pstate,
												 OnConflictClause *onConflictClause);
static int	count_rowexpr_columns(ParseState *pstate, Node *expr);
static Query *transformSelectStmt(ParseState *pstate, SelectStmt *stmt);
static Query *transformValuesClause(ParseState *pstate, SelectStmt *stmt);
static Query *transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt);
static Node *transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
									   bool isTopLevel, List **targetlist);
static void determineRecursiveColTypes(ParseState *pstate,
									   Node *larg, List *nrtargetlist);
static Query *transformReturnStmt(ParseState *pstate, ReturnStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt);
static Query *transformPLAssignStmt(ParseState *pstate,
									PLAssignStmt *stmt);
static Query *transformDeclareCursorStmt(ParseState *pstate,
										 DeclareCursorStmt *stmt);
static Query *transformExplainStmt(ParseState *pstate,
								   ExplainStmt *stmt);
static Query *transformCreateTableAsStmt(ParseState *pstate,
										 CreateTableAsStmt *stmt);
static Query *transformCallStmt(ParseState *pstate,
								CallStmt *stmt);
static void transformLockingClause(ParseState *pstate, Query *qry,
								   LockingClause *lc, bool pushedDown);
#ifdef DEBUG_NODE_TESTS_ENABLED
static bool test_raw_expression_coverage(Node *node, void *context);
#endif


/*
 * parse_analyze_fixedparams
 *		Analyze a raw parse tree and transform it to Query form.
 *
 * 分析原始解析树并将其转换为 Query 形式。
 *
 * Optionally, information about $n parameter types can be supplied.
 * References to $n indexes not defined by paramTypes[] are disallowed.
 *
 * 可选地，可以提供有关 $n 参数类型的信息。不允许引用未由 paramTypes[] 定义的 $n 索引。
 *
 * The result is a Query node.  Optimizable statements require considerable
 * transformation, while utility-type statements are simply hung off
 * a dummy CMD_UTILITY Query node.
 *
 * 结果是一个 Query 节点。可优化的语句需要相当大的转换，而实用程序类型的语句只是
 * 挂在一个虚拟的 CMD_UTILITY Query 节点下。
 */
Query *
parse_analyze_fixedparams(RawStmt *parseTree, const char *sourceText,
						  const Oid *paramTypes, int numParams,
						  QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	if (numParams > 0)
		setup_parse_fixed_parameters(pstate, paramTypes, numParams);

	pstate->p_queryEnv = queryEnv;

	query = transformTopLevelStmt(pstate, parseTree);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}

/*
 * parse_analyze_varparams
 *
 * This variant is used when it's okay to deduce information about $n
 * symbol datatypes from context.  The passed-in paramTypes[] array can
 * be modified or enlarged (via repalloc).
 *
 * 当可以从上下文中推断有关 $n 符号数据类型的信息时，使用此变体。
 * 传入的 paramTypes[] 数组可以被修改或扩大（通过 repalloc）。
 */
Query *
parse_analyze_varparams(RawStmt *parseTree, const char *sourceText,
						Oid **paramTypes, int *numParams,
						QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;

	setup_parse_variable_parameters(pstate, paramTypes, numParams);

	pstate->p_queryEnv = queryEnv;

	query = transformTopLevelStmt(pstate, parseTree);

	/* make sure all is well with parameter types */
	check_variable_parameters(pstate, query);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}

/*
 * parse_analyze_withcb
 *
 * This variant is used when the caller supplies their own parser callback to
 * resolve parameters and possibly other things.
 *
 * 当调用者提供他们自己的解析器回调来解析参数以及可能存在的其他内容时，使用此变体。
 */
Query *
parse_analyze_withcb(RawStmt *parseTree, const char *sourceText,
					 ParserSetupHook parserSetup,
					 void *parserSetupArg,
					 QueryEnvironment *queryEnv)
{
	ParseState *pstate = make_parsestate(NULL);
	Query	   *query;
	JumbleState *jstate = NULL;

	Assert(sourceText != NULL); /* required as of 8.4 */

	pstate->p_sourcetext = sourceText;
	pstate->p_queryEnv = queryEnv;
	(*parserSetup) (pstate, parserSetupArg);

	query = transformTopLevelStmt(pstate, parseTree);

	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	free_parsestate(pstate);

	pgstat_report_query_id(query->queryId, false);

	return query;
}


/*
 * parse_sub_analyze
 *		Entry point for recursively analyzing a sub-statement.
 *
 * 递归分析子语句的入口点。
 */
Query *
parse_sub_analyze(Node *parseTree, ParseState *parentParseState,
				  CommonTableExpr *parentCTE,
				  bool locked_from_parent,
				  bool resolve_unknowns)
{
	ParseState *pstate = make_parsestate(parentParseState);
	Query	   *query;

	pstate->p_parent_cte = parentCTE;
	pstate->p_locked_from_parent = locked_from_parent;
	pstate->p_resolve_unknowns = resolve_unknowns;

	query = transformStmt(pstate, parseTree);

	free_parsestate(pstate);

	return query;
}

/*
 * transformTopLevelStmt -
 *	  transform a Parse tree into a Query tree.
 *
 * 将解析树转换为查询树。
 *
 * This function is just responsible for transferring statement location data
 * from the RawStmt into the finished Query.
 *
 * 此函数仅负责将语句位置数据从 RawStmt 传输到完成的 Query 中。
 */
Query *
transformTopLevelStmt(ParseState *pstate, RawStmt *parseTree)
{
	Query	   *result;

	/* We're at top level, so allow SELECT INTO */
	result = transformOptionalSelectInto(pstate, parseTree->stmt);

	result->stmt_location = parseTree->stmt_location;
	result->stmt_len = parseTree->stmt_len;

	return result;
}

/*
 * transformOptionalSelectInto -
 *	  If SELECT has INTO, convert it to CREATE TABLE AS.
 *
 * 如果 SELECT 带有 INTO，则将其转换为 CREATE TABLE AS。
 *
 * The only thing we do here that we don't do in transformStmt() is to
 * convert SELECT ... INTO into CREATE TABLE AS.  Since utility statements
 * aren't allowed within larger statements, this is only allowed at the top
 * of the parse tree, and so we only try it before entering the recursive
 * transformStmt() processing.
 *
 * 我们在这里做的唯一一件在 transformStmt() 中不做的操作是将 SELECT ... INTO 转换为
 * CREATE TABLE AS。由于实用程序语句不允许在更大的语句中使用，因此这仅允许在解析树的
 * 顶层使用，所以我们仅在进入递归 transformStmt() 处理之前尝试它。
 */
static Query *
transformOptionalSelectInto(ParseState *pstate, Node *parseTree)
{
	if (IsA(parseTree, SelectStmt))
	{
		SelectStmt *stmt = (SelectStmt *) parseTree;

		/* If it's a set-operation tree, drill down to leftmost SelectStmt */
		while (stmt && stmt->op != SETOP_NONE)
			stmt = stmt->larg;
		Assert(stmt && IsA(stmt, SelectStmt) && stmt->larg == NULL);

		if (stmt->intoClause)
		{
			CreateTableAsStmt *ctas = makeNode(CreateTableAsStmt);

			ctas->query = parseTree;
			ctas->into = stmt->intoClause;
			ctas->objtype = OBJECT_TABLE;
			ctas->is_select_into = true;

			/*
			 * Remove the intoClause from the SelectStmt.  This makes it safe
			 * for transformSelectStmt to complain if it finds intoClause set
			 * (implying that the INTO appeared in a disallowed place).
			 */
			stmt->intoClause = NULL;

			parseTree = (Node *) ctas;
		}
	}

	return transformStmt(pstate, parseTree);
}

/*
 * transformStmt -
 *	  recursively transform a Parse tree into a Query tree.
 *
 * 递归地将解析树转换为查询树。
 */
Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result;

#ifdef DEBUG_NODE_TESTS_ENABLED

	/*
	 * We apply debug_raw_expression_coverage_test testing to basic DML
	 * statements; we can't just run it on everything because
	 * raw_expression_tree_walker() doesn't claim to handle utility
	 * statements.
	 *
	 * 我们将 debug_raw_expression_coverage_test 测试应用于基本的 DML 语句；
	 * 我们不能在所有内容上运行它，因为 raw_expression_tree_walker()
	 * 并不声称可以处理实用程序语句。
	 */
	if (Debug_raw_expression_coverage_test)
	{
		switch (nodeTag(parseTree))
		{
			case T_SelectStmt:
			case T_InsertStmt:
			case T_UpdateStmt:
			case T_DeleteStmt:
			case T_MergeStmt:
				(void) test_raw_expression_coverage(parseTree, NULL);
				break;
			default:
				break;
		}
	}
#endif							/* DEBUG_NODE_TESTS_ENABLED */

	/*
	 * Caution: when changing the set of statement types that have non-default
	 * processing here, see also stmt_requires_parse_analysis() and
	 * analyze_requires_snapshot().
	 *
	 * 注意：在此处更改具有非默认处理的语句类型集时，另请参见 
	 * stmt_requires_parse_analysis() 和 analyze_requires_snapshot()。
	 */
	switch (nodeTag(parseTree))
	{
			/*
			 * Optimizable statements
			 *
			 * 可优化语句
			 */
		case T_InsertStmt:
			result = transformInsertStmt(pstate, (InsertStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_UpdateStmt:
			result = transformUpdateStmt(pstate, (UpdateStmt *) parseTree);
			break;

		case T_MergeStmt:
			result = transformMergeStmt(pstate, (MergeStmt *) parseTree);
			break;

		case T_SelectStmt:
			{
				SelectStmt *n = (SelectStmt *) parseTree;

				if (n->valuesLists)
					result = transformValuesClause(pstate, n);
				else if (n->op == SETOP_NONE)
					result = transformSelectStmt(pstate, n);
				else
					result = transformSetOperationStmt(pstate, n);
			}
			break;

		case T_ReturnStmt:
			result = transformReturnStmt(pstate, (ReturnStmt *) parseTree);
			break;

		case T_PLAssignStmt:
			result = transformPLAssignStmt(pstate,
										   (PLAssignStmt *) parseTree);
			break;

			/*
			 * Special cases
			 *
			 * 特殊情况
			 */
		case T_DeclareCursorStmt:
			result = transformDeclareCursorStmt(pstate,
												(DeclareCursorStmt *) parseTree);
			break;

		case T_ExplainStmt:
			result = transformExplainStmt(pstate,
										  (ExplainStmt *) parseTree);
			break;

		case T_CreateTableAsStmt:
			result = transformCreateTableAsStmt(pstate,
												(CreateTableAsStmt *) parseTree);
			break;

		case T_CallStmt:
			result = transformCallStmt(pstate,
									   (CallStmt *) parseTree);
			break;

		default:

			/*
			 * other statements don't require any transformation; just return
			 * the original parsetree with a Query node plastered on top.
			 *
			 * 其他语句不需要任何转换；只需返回原始解析树，并在顶部贴上一个 Query 节点。
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}

	/* Mark as original query until we learn differently */
	result->querySource = QSRC_ORIGINAL;
	result->canSetTag = true;

	return result;
}

/*
 * stmt_requires_parse_analysis
 *		Returns true if parse analysis will do anything non-trivial
 *		with the given raw parse tree.
 *
 * 如果解析分析将对给定的原始解析树执行任何非平凡的操作，则返回 true。
 *
 * Generally, this should return true for any statement type for which
 * transformStmt() does more than wrap a CMD_UTILITY Query around it.
 * When it returns false, the caller can assume that there is no situation
 * in which parse analysis of the raw statement could need to be re-done.
 *
 * 通常，对于 transformStmt() 不仅仅是在其周围包裹一个 CMD_UTILITY Query 的任何
 * 语句类型，这都应该返回 true。当它返回 false 时，调用者可以假设不存在原始语句的
 * 解析分析需要重新执行的情况。
 *
 * Currently, since the rewriter and planner do nothing for CMD_UTILITY
 * Queries, a false result means that the entire parse analysis/rewrite/plan
 * pipeline will never need to be re-done.  If that ever changes, callers
 * will likely need adjustment.
 *
 * 目前，由于重写器和规划器对 CMD_UTILITY 查询不执行任何操作，因此 false 结果意味着
 * 整个解析分析/重写/规划流水线永远不需要重新执行。如果这种情况发生变化，
 * 调用者可能需要进行调整。
 */
bool
stmt_requires_parse_analysis(RawStmt *parseTree)
{
	bool		result;

	switch (nodeTag(parseTree->stmt))
	{
			/*
			 * Optimizable statements
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
		case T_SelectStmt:
		case T_ReturnStmt:
		case T_PLAssignStmt:
			result = true;
			break;

			/*
			 * Special cases
			 */
		case T_DeclareCursorStmt:
		case T_ExplainStmt:
		case T_CreateTableAsStmt:
		case T_CallStmt:
			result = true;
			break;

		default:
			/* all other statements just get wrapped in a CMD_UTILITY Query */
			result = false;
			break;
	}

	return result;
}

/*
 * analyze_requires_snapshot
 *		Returns true if a snapshot must be set before doing parse analysis
 *		on the given raw parse tree.
 *
 * 如果在对给定的原始解析树进行解析分析之前必须设置快照，则返回 true。
 */
bool
analyze_requires_snapshot(RawStmt *parseTree)
{
	/*
	 * Currently, this should return true in exactly the same cases that
	 * stmt_requires_parse_analysis() does, so we just invoke that function
	 * rather than duplicating it.  We keep the two entry points separate for
	 * clarity of callers, since from the callers' standpoint these are
	 * different conditions.
	 *
	 * 目前，这在与 stmt_requires_parse_analysis() 完全相同的情况下应返回 true，
	 * 因此我们只需调用该函数而不是重复它。为了使调用者清晰，我们将两个入口点分开，
	 * 因为从调用者的立场来看，这些是不同的条件。
	 *
	 * While there may someday be a statement type for which transformStmt()
	 * does something nontrivial and yet no snapshot is needed for that
	 * processing, it seems likely that making such a choice would be fragile.
	 * If you want to install an exception, document the reasoning for it in a
	 * comment.
	 *
	 * 虽然将来可能会有一种语句类型，其 transformStmt() 执行某些非平凡的操作，
	 * 但该处理却不需要快照，但做出这样的选择似乎是脆弱的。如果您想安装一个例外，
	 * 请在注释中说明其原因。
	 */
	return stmt_requires_parse_analysis(parseTree);
}

/*
 * query_requires_rewrite_plan()
 *		Returns true if rewriting or planning is non-trivial for this Query.
 *
 * 如果重写或规划对于此 Query 是非平凡的，则返回 true。
 *
 * This is much like stmt_requires_parse_analysis(), but applies one step
 * further down the pipeline.
 *
 * 这非常类似于 stmt_requires_parse_analysis()，但适用于流水线中的更进一步。
 *
 * We do not provide an equivalent of analyze_requires_snapshot(): callers
 * can assume that any rewriting or planning activity needs a snapshot.
 *
 * 我们不提供 analyze_requires_snapshot() 的等效项：调用者可以假设
 * 任何重写或规划活动都需要快照。
 */
bool
query_requires_rewrite_plan(Query *query)
{
	bool		result;

	if (query->commandType != CMD_UTILITY)
	{
		/* All optimizable statements require rewriting/planning */
		result = true;
	}
	else
	{
		/* This list should match stmt_requires_parse_analysis() */
		switch (nodeTag(query->utilityStmt))
		{
			case T_DeclareCursorStmt:
			case T_ExplainStmt:
			case T_CreateTableAsStmt:
			case T_CallStmt:
				result = true;
				break;
			default:
				result = false;
				break;
		}
	}
	return result;
}

/*
 * transformDeleteStmt -
 *	  transforms a Delete Statement
 *
 * 转换 Delete 语句
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_DELETE;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* set up range table with just the result rel */
	/* 仅使用结果关系设置范围表 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_DELETE);
	nsitem = pstate->p_target_nsitem;

	/* there's no DISTINCT in DELETE */
	/* DELETE 中没有 DISTINCT */
	qry->distinctClause = NIL;

	/* subqueries in USING cannot access the result relation */
	/* USING 中的子查询不能访问结果关系 */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * The USING clause is non-standard SQL syntax, and is equivalent in
	 * functionality to the FROM list that can be specified for UPDATE. The
	 * USING keyword is used rather than FROM because FROM is already a
	 * keyword in the DELETE syntax.
	 *
	 * USING 子句是非标准 SQL 语法，在功能上等同于可以为 UPDATE 指定的 FROM 列表。
	 * 使用 USING 关键字而不是 FROM 是因为 FROM 已经是 DELETE 语法中的关键字。
	 */
	transformFromClause(pstate, stmt->usingClause);

	/* remaining clauses can reference the result relation normally */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	transformReturningClause(pstate, qry, stmt->returningClause,
							 EXPR_KIND_RETURNING);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformInsertStmt -
 *	  transform an Insert Statement
 *
 * 转换一个 Insert 语句
 */
static Query *
transformInsertStmt(ParseState *pstate, InsertStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *selectStmt = (SelectStmt *) stmt->selectStmt;
	List	   *exprList = NIL;
	bool		isGeneralSelect;
	List	   *sub_rtable;
	List	   *sub_rteperminfos;
	List	   *sub_namespace;
	List	   *icolumns;
	List	   *attrnos;
	ParseNamespaceItem *nsitem;
	RTEPermissionInfo *perminfo;
	ListCell   *icols;
	ListCell   *attnos;
	ListCell   *lc;
	bool		isOnConflictUpdate;
	AclMode		targetPerms;

	/* There can't be any outer WITH to worry about */
	/* 不需要担心任何外部的 WITH */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->override = stmt->override;

	isOnConflictUpdate = (stmt->onConflictClause &&
						  stmt->onConflictClause->action == ONCONFLICT_UPDATE);

	/*
	 * We have three cases to deal with: DEFAULT VALUES (selectStmt == NULL),
	 * VALUES list, or general SELECT input.  We special-case VALUES, both for
	 * efficiency and so we can handle DEFAULT specifications.
	 *
	 * 我们需要处理三种情况：DEFAULT VALUES (selectStmt == NULL)、VALUES 列表
	 * 或通用的 SELECT 输入。我们对 VALUES 进行特殊处理，既是为了提高效率，
	 * 也是为了我们可以处理 DEFAULT 规范。
	 *
	 * The grammar allows attaching ORDER BY, LIMIT, FOR UPDATE, or WITH to a
	 * VALUES clause.  If we have any of those, treat it as a general SELECT;
	 * so it will work, but you can't use DEFAULT items together with those.
	 *
	 * 语法允许将 ORDER BY、LIMIT、FOR UPDATE 或 WITH 附加到 VALUES 子句。
	 * 如果我们有其中任何一个，则将其视为通用的 SELECT；因此它可以工作，
	 * 但您不能将 DEFAULT 项目与这些项目一起使用。
	 */
	isGeneralSelect = (selectStmt && (selectStmt->valuesLists == NIL ||
									  selectStmt->sortClause != NIL ||
									  selectStmt->limitOffset != NULL ||
									  selectStmt->limitCount != NULL ||
									  selectStmt->lockingClause != NIL ||
									  selectStmt->withClause != NULL));

	/*
	 * If a non-nil rangetable/namespace was passed in, and we are doing
	 * INSERT/SELECT, arrange to pass the rangetable/rteperminfos/namespace
	 * down to the SELECT.  This can only happen if we are inside a CREATE
	 * RULE, and in that case we want the rule's OLD and NEW rtable entries to
	 * appear as part of the SELECT's rtable, not as outer references for it.
	 * (Kluge!) The SELECT's joinlist is not affected however.  We must do
	 * this before adding the target table to the INSERT's rtable.
	 */
	if (isGeneralSelect)
	{
		sub_rtable = pstate->p_rtable;
		pstate->p_rtable = NIL;
		sub_rteperminfos = pstate->p_rteperminfos;
		pstate->p_rteperminfos = NIL;
		sub_namespace = pstate->p_namespace;
		pstate->p_namespace = NIL;
	}
	else
	{
		sub_rtable = NIL;		/* not used, but keep compiler quiet */
		sub_rteperminfos = NIL;
		sub_namespace = NIL;
	}

	/*
	 * Must get write lock on INSERT target table before scanning SELECT, else
	 * we will grab the wrong kind of initial lock if the target table is also
	 * mentioned in the SELECT part.  Note that the target table is not added
	 * to the joinlist or namespace.
	 */
	targetPerms = ACL_INSERT;
	if (isOnConflictUpdate)
		targetPerms |= ACL_UPDATE;
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 false, false, targetPerms);

	/* Validate stmt->cols list, or build default list if no list given */
	/* 验证 stmt->cols 列表，或在未给出列表时构建默认列表 */
	icolumns = checkInsertTargets(pstate, stmt->cols, &attrnos);
	Assert(list_length(icolumns) == list_length(attrnos));

	/*
	 * Determine which variant of INSERT we have.
	 *
	 * 确定我们拥有哪种 INSERT 变体。
	 */
	if (selectStmt == NULL)
	{
		/*
		 * We have INSERT ... DEFAULT VALUES.  We can handle this case by
		 * emitting an empty targetlist --- all columns will be defaulted when
		 * the planner expands the targetlist.
		 *
		 * 我们有 INSERT ... DEFAULT VALUES。我们可以通过发出一个空的目标列表来处理
		 * 此情况 —— 当规划器扩展目标列表时，所有列都将使用默认值。
		 */
		exprList = NIL;
	}
	else if (isGeneralSelect)
	{
		/*
		 * We make the sub-pstate a child of the outer pstate so that it can
		 * see any Param definitions supplied from above.  Since the outer
		 * pstate's rtable and namespace are presently empty, there are no
		 * side-effects of exposing names the sub-SELECT shouldn't be able to
		 * see.
		 *
		 * 我们将 sub-pstate 设置为外部 pstate 的子级，以便它可以查看从上方提供的任何
		 * Param 定义。由于外部 pstate 的 rtable 和命名空间目前为空，因此公开
		 * 子 SELECT 不应该能看到的名称没有副作用。
		 */
		ParseState *sub_pstate = make_parsestate(pstate);
		Query	   *selectQuery;

		/*
		 * Process the source SELECT.
		 *
		 * 处理源 SELECT。
		 *
		 * It is important that this be handled just like a standalone SELECT;
		 * otherwise the behavior of SELECT within INSERT might be different
		 * from a stand-alone SELECT. (Indeed, Postgres up through 6.5 had
		 * bugs of just that nature...)
		 *
		 * 重要的是，这要像独立的 SELECT 一样处理；否则 INSERT 内部 SELECT 的行为可能
		 * 与独立的 SELECT 不同。（事实上，Postgres 直到 6.5 版本都存在这类性质的 bug...）
		 *
		 * The sole exception is that we prevent resolving unknown-type
		 * outputs as TEXT.  This does not change the semantics since if the
		 * column type matters semantically, it would have been resolved to
		 * something else anyway.  Doing this lets us resolve such outputs as
		 * the target column's type, which we handle below.
		 *
		 * 唯一的例外是我们阻止将未知类型的输出解析为 TEXT。这不会改变语义，因为如果
		 * 列类型在语义上很重要，它无论如何都会被解析为其他类型。这样做让我们可以将
		 * 此类输出解析为目标列的类型，我们将在下面处理。
		 */
		sub_pstate->p_rtable = sub_rtable;
		sub_pstate->p_rteperminfos = sub_rteperminfos;
		sub_pstate->p_joinexprs = NIL;	/* sub_rtable has no joins */
		sub_pstate->p_nullingrels = NIL;
		sub_pstate->p_namespace = sub_namespace;
		sub_pstate->p_resolve_unknowns = false;

		selectQuery = transformStmt(sub_pstate, stmt->selectStmt);

		free_parsestate(sub_pstate);

		/* The grammar should have produced a SELECT */
		/* 语法应该生成一个 SELECT */
		if (!IsA(selectQuery, Query) ||
			selectQuery->commandType != CMD_SELECT)
			elog(ERROR, "unexpected non-SELECT command in INSERT ... SELECT");

		/*
		 * Make the source be a subquery in the INSERT's rangetable, and add
		 * it to the INSERT's joinlist (but not the namespace).
		 *
		 * 使源成为 INSERT 范围表中的子查询，并将其添加到 INSERT 的 joinlist
		 *（但不是命名空间）中。
		 */
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias("*SELECT*", NIL),
											   false,
											   false);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*----------
		 * Generate an expression list for the INSERT that selects all the
		 * non-resjunk columns from the subquery.  (INSERT's tlist must be
		 * separate from the subquery's tlist because we may add columns,
		 * insert datatype coercions, etc.)
		 *
		 * 为 INSERT 生成一个表达式列表，该列表从子查询中选择所有非 resjunk 列。
		 *（INSERT 的 tlist 必须与子查询的 tlist 分开，因为我们可能会添加列、
		 * 插入数据类型强制转换等。）
		 *
		 * HACK: unknown-type constants and params in the SELECT's targetlist
		 * are copied up as-is rather than being referenced as subquery
		 * outputs.  This is to ensure that when we try to coerce them to
		 * the target column's datatype, the right things happen (see
		 * special cases in coerce_type).  Otherwise, this fails:
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *
		 * 技巧：SELECT 目标列表中的未知类型常量和参数将按原样复制，而不是作为子查询输出被引用。
		 * 这是为了确保当我们尝试将它们强制转换为目标列的数据类型时，会发生正确的情况
		 *（请参阅 coerce_type 中的特殊情况）。否则，这将失败：
		 *		INSERT INTO foo SELECT 'bar', ... FROM baz
		 *----------
		 */
		exprList = NIL;
		foreach(lc, selectQuery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Expr	   *expr;

			if (tle->resjunk)
				continue;
			if (tle->expr &&
				(IsA(tle->expr, Const) || IsA(tle->expr, Param)) &&
				exprType((Node *) tle->expr) == UNKNOWNOID)
				expr = tle->expr;
			else
			{
				Var		   *var = makeVarFromTargetEntry(nsitem->p_rtindex, tle);

				var->location = exprLocation((Node *) tle->expr);
				expr = (Expr *) var;
			}
			exprList = lappend(exprList, expr);
		}

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else if (list_length(selectStmt->valuesLists) > 1)
	{
		/*
		 * Process INSERT ... VALUES with multiple VALUES sublists. We
		 * generate a VALUES RTE holding the transformed expression lists, and
		 * build up a targetlist containing Vars that reference the VALUES
		 * RTE.
		 *
		 * 处理带有多个 VALUES 子列表的 INSERT ... VALUES。我们生成一个持有转换后的
		 * 表达式列表的 VALUES RTE，并构建一个包含引用该 VALUES RTE 的 Var 的目标列表。
		 */
		List	   *exprsLists = NIL;
		List	   *coltypes = NIL;
		List	   *coltypmods = NIL;
		List	   *colcollations = NIL;
		int			sublist_length = -1;
		bool		lateral = false;

		Assert(selectStmt->intoClause == NULL);

		foreach(lc, selectStmt->valuesLists)
		{
			List	   *sublist = (List *) lfirst(lc);

			/*
			 * Do basic expression transformation (same as a ROW() expr, but
			 * allow SetToDefault at top level)
			 */
			sublist = transformExpressionList(pstate, sublist,
											  EXPR_KIND_VALUES, true);

			/*
			 * All the sublists must be the same length, *after*
			 * transformation (which might expand '*' into multiple items).
			 * The VALUES RTE can't handle anything different.
			 */
			if (sublist_length < 0)
			{
				/* Remember post-transformation length of first sublist */
				/* 记住第一个子列表转换后的长度 */
				sublist_length = list_length(sublist);
			}
			else if (sublist_length != list_length(sublist))
			{
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("VALUES lists must all be the same length"),
						 parser_errposition(pstate,
											exprLocation((Node *) sublist))));
			}

			/*
			 * Prepare row for assignment to target table.  We process any
			 * indirection on the target column specs normally but then strip
			 * off the resulting field/array assignment nodes, since we don't
			 * want the parsed statement to contain copies of those in each
			 * VALUES row.  (It's annoying to have to transform the
			 * indirection specs over and over like this, but avoiding it
			 * would take some really messy refactoring of
			 * transformAssignmentIndirection.)
			 */
			sublist = transformInsertRow(pstate, sublist,
										 stmt->cols,
										 icolumns, attrnos,
										 true);

			/*
			 * We must assign collations now because assign_query_collations
			 * doesn't process rangetable entries.  We just assign all the
			 * collations independently in each row, and don't worry about
			 * whether they are consistent vertically.  The outer INSERT query
			 * isn't going to care about the collations of the VALUES columns,
			 * so it's not worth the effort to identify a common collation for
			 * each one here.  (But note this does have one user-visible
			 * consequence: INSERT ... VALUES won't complain about conflicting
			 * explicit COLLATEs in a column, whereas the same VALUES
			 * construct in another context would complain.)
			 */
			assign_list_collations(pstate, sublist);

			exprsLists = lappend(exprsLists, sublist);
		}

		/*
		 * Construct column type/typmod/collation lists for the VALUES RTE.
		 * Every expression in each column has been coerced to the type/typmod
		 * of the corresponding target column or subfield, so it's sufficient
		 * to look at the exprType/exprTypmod of the first row.  We don't care
		 * about the collation labeling, so just fill in InvalidOid for that.
		 */
		foreach(lc, (List *) linitial(exprsLists))
		{
			Node	   *val = (Node *) lfirst(lc);

			coltypes = lappend_oid(coltypes, exprType(val));
			coltypmods = lappend_int(coltypmods, exprTypmod(val));
			colcollations = lappend_oid(colcollations, InvalidOid);
		}

		/*
		 * Ordinarily there can't be any current-level Vars in the expression
		 * lists, because the namespace was empty ... but if we're inside
		 * CREATE RULE, then NEW/OLD references might appear.  In that case we
		 * have to mark the VALUES RTE as LATERAL.
		 */
		if (list_length(pstate->p_rtable) != 1 &&
			contain_vars_of_level((Node *) exprsLists, 0))
			lateral = true;

		/*
		 * Generate the VALUES RTE
		 */
		nsitem = addRangeTableEntryForValues(pstate, exprsLists,
											 coltypes, coltypmods, colcollations,
											 NULL, lateral, true);
		addNSItemToQuery(pstate, nsitem, true, false, false);

		/*
		 * Generate list of Vars referencing the RTE
		 */
		exprList = expandNSItemVars(pstate, nsitem, 0, -1, NULL);

		/*
		 * Re-apply any indirection on the target column specs to the Vars
		 */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}
	else
	{
		/*
		 * Process INSERT ... VALUES with a single VALUES sublist.  We treat
		 * this case separately for efficiency.  The sublist is just computed
		 * directly as the Query's targetlist, with no VALUES RTE.  So it
		 * works just like a SELECT without any FROM.
		 */
		List	   *valuesLists = selectStmt->valuesLists;

		Assert(list_length(valuesLists) == 1);
		Assert(selectStmt->intoClause == NULL);

		/*
		 * Do basic expression transformation (same as a ROW() expr, but allow
		 * SetToDefault at top level)
		 */
		exprList = transformExpressionList(pstate,
										   (List *) linitial(valuesLists),
										   EXPR_KIND_VALUES_SINGLE,
										   true);

		/* Prepare row for assignment to target table */
		exprList = transformInsertRow(pstate, exprList,
									  stmt->cols,
									  icolumns, attrnos,
									  false);
	}

	/*
	 * Generate query's target list using the computed list of expressions.
	 * Also, mark all the target columns as needing insert permissions.
	 */
	perminfo = pstate->p_target_nsitem->p_perminfo;
	qry->targetList = NIL;
	Assert(list_length(exprList) <= list_length(icolumns));
	forthree(lc, exprList, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
		TargetEntry *tle;

		tle = makeTargetEntry(expr,
							  attr_num,
							  col->name,
							  false);
		qry->targetList = lappend(qry->targetList, tle);

		perminfo->insertedCols = bms_add_member(perminfo->insertedCols,
												attr_num - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * If we have any clauses yet to process, set the query namespace to
	 * contain only the target relation, removing any entries added in a
	 * sub-SELECT or VALUES list.
	 */
	if (stmt->onConflictClause || stmt->returningClause)
	{
		pstate->p_namespace = NIL;
		addNSItemToQuery(pstate, pstate->p_target_nsitem,
						 false, true, true);
	}

	/* Process ON CONFLICT, if any. */
	/* 处理 ON CONFLICT（如果有）。 */
	if (stmt->onConflictClause)
		qry->onConflict = transformOnConflictClause(pstate,
													stmt->onConflictClause);

	/* Process RETURNING, if any. */
	/* 处理 RETURNING（如果有）。 */
	if (stmt->returningClause)
		transformReturningClause(pstate, qry, stmt->returningClause,
								 EXPR_KIND_RETURNING);

	/* done building the range table and jointree */
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * Prepare an INSERT row for assignment to the target table.
 *
 * 为分配给目标表准备 INSERT 行。
 *
 * exprlist: transformed expressions for source values; these might come from
 * a VALUES row, or be Vars referencing a sub-SELECT or VALUES RTE output.
 * stmtcols: original target-columns spec for INSERT (we just test for NIL)
 * icolumns: effective target-columns spec (list of ResTarget)
 * attrnos: integer column numbers (must be same length as icolumns)
 * strip_indirection: if true, remove any field/array assignment nodes
 *
 * exprlist: 源值的转换后表达式；这些可能来自 VALUES 行，或者是引用子 SELECT 或 
 * VALUES RTE 输出的 Var。
 * stmtcols: INSERT 的原始目标列规范（我们仅测试是否为 NIL）
 * icolumns: 有效的目标列规范（ResTarget 列表）
 * attrnos: 整数列编号（必须与 icolumns 长度相同）
 * strip_indirection: 如果为 true，则移除任何字段/数组赋值节点
 */
List *
transformInsertRow(ParseState *pstate, List *exprlist,
				   List *stmtcols, List *icolumns, List *attrnos,
				   bool strip_indirection)
{
	List	   *result;
	ListCell   *lc;
	ListCell   *icols;
	ListCell   *attnos;

	/*
	 * Check length of expr list.  It must not have more expressions than
	 * there are target columns.  We allow fewer, but only if no explicit
	 * columns list was given (the remaining columns are implicitly
	 * defaulted).  Note we must check this *after* transformation because
	 * that could expand '*' into multiple items.
	 */
	if (list_length(exprlist) > list_length(icolumns))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more expressions than target columns"),
				 parser_errposition(pstate,
									exprLocation(list_nth(exprlist,
														  list_length(icolumns))))));
	if (stmtcols != NIL &&
		list_length(exprlist) < list_length(icolumns))
	{
		/*
		 * We can get here for cases like INSERT ... SELECT (a,b,c) FROM ...
		 * where the user accidentally created a RowExpr instead of separate
		 * columns.  Add a suitable hint if that seems to be the problem,
		 * because the main error message is quite misleading for this case.
		 * (If there's no stmtcols, you'll get something about data type
		 * mismatch, which is less misleading so we don't worry about giving a
		 * hint in that case.)
		 */
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INSERT has more target columns than expressions"),
				 ((list_length(exprlist) == 1 &&
				   count_rowexpr_columns(pstate, linitial(exprlist)) ==
				   list_length(icolumns)) ?
				  errhint("The insertion source is a row expression containing the same number of columns expected by the INSERT. Did you accidentally use extra parentheses?") : 0),
				 parser_errposition(pstate,
									exprLocation(list_nth(icolumns,
														  list_length(exprlist))))));
	}

	/*
	 * Prepare columns for assignment to target table.
	 */
	result = NIL;
	forthree(lc, exprlist, icols, icolumns, attnos, attrnos)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		ResTarget  *col = lfirst_node(ResTarget, icols);
		int			attno = lfirst_int(attnos);

		expr = transformAssignedExpr(pstate, expr,
									 EXPR_KIND_INSERT_TARGET,
									 col->name,
									 attno,
									 col->indirection,
									 col->location);

		if (strip_indirection)
		{
			/*
			 * We need to remove top-level FieldStores and SubscriptingRefs,
			 * as well as any CoerceToDomain appearing above one of those ---
			 * but not a CoerceToDomain that isn't above one of those.
			 */
			while (expr)
			{
				Expr	   *subexpr = expr;

				while (IsA(subexpr, CoerceToDomain))
				{
					subexpr = ((CoerceToDomain *) subexpr)->arg;
				}
				if (IsA(subexpr, FieldStore))
				{
					FieldStore *fstore = (FieldStore *) subexpr;

					expr = (Expr *) linitial(fstore->newvals);
				}
				else if (IsA(subexpr, SubscriptingRef))
				{
					SubscriptingRef *sbsref = (SubscriptingRef *) subexpr;

					if (sbsref->refassgnexpr == NULL)
						break;

					expr = sbsref->refassgnexpr;
				}
				else
					break;
			}
		}

		result = lappend(result, expr);
	}

	return result;
}

/*
 * transformOnConflictClause -
 *	  transforms an OnConflictClause in an INSERT
 *
 * 转换 INSERT 中的 OnConflictClause
 */
static OnConflictExpr *
transformOnConflictClause(ParseState *pstate,
						  OnConflictClause *onConflictClause)
{
	ParseNamespaceItem *exclNSItem = NULL;
	List	   *arbiterElems;
	Node	   *arbiterWhere;
	Oid			arbiterConstraint;
	List	   *onConflictSet = NIL;
	Node	   *onConflictWhere = NULL;
	int			exclRelIndex = 0;
	List	   *exclRelTlist = NIL;
	OnConflictExpr *result;

	/*
	 * If this is ON CONFLICT ... UPDATE, first create the range table entry
	 * for the EXCLUDED pseudo relation, so that that will be present while
	 * processing arbiter expressions.  (You can't actually reference it from
	 * there, but this provides a useful error message if you try.)
	 */
	if (onConflictClause->action == ONCONFLICT_UPDATE)
	{
		Relation	targetrel = pstate->p_target_relation;
		RangeTblEntry *exclRte;

		exclNSItem = addRangeTableEntryForRelation(pstate,
												   targetrel,
												   RowExclusiveLock,
												   makeAlias("excluded", NIL),
												   false, false);
		exclRte = exclNSItem->p_rte;
		exclRelIndex = exclNSItem->p_rtindex;

		/*
		 * relkind is set to composite to signal that we're not dealing with
		 * an actual relation, and no permission checks are required on it.
		 * (We'll check the actual target relation, instead.)
		 */
		exclRte->relkind = RELKIND_COMPOSITE_TYPE;

		/* Create EXCLUDED rel's targetlist for use by EXPLAIN */
		/* 创建 EXCLUDED 关系的 targetlist 以供 EXPLAIN 使用 */
		exclRelTlist = BuildOnConflictExcludedTargetlist(targetrel,
														 exclRelIndex);
	}

	/* Process the arbiter clause, ON CONFLICT ON (...) */
	/* 处理仲裁者子句，ON CONFLICT ON (...) */
	transformOnConflictArbiter(pstate, onConflictClause, &arbiterElems,
							   &arbiterWhere, &arbiterConstraint);

	/* Process DO UPDATE */
	/* 处理 DO UPDATE */
	if (onConflictClause->action == ONCONFLICT_UPDATE)
	{
		/*
		 * Expressions in the UPDATE targetlist need to be handled like UPDATE
		 * not INSERT.  We don't need to save/restore this because all INSERT
		 * expressions have been parsed already.
		 */
		pstate->p_is_insert = false;

		/*
		 * Add the EXCLUDED pseudo relation to the query namespace, making it
		 * available in the UPDATE subexpressions.
		 */
		addNSItemToQuery(pstate, exclNSItem, false, true, true);

		/*
		 * Now transform the UPDATE subexpressions.
		 */
		onConflictSet =
			transformUpdateTargetList(pstate, onConflictClause->targetList);

		onConflictWhere = transformWhereClause(pstate,
											   onConflictClause->whereClause,
											   EXPR_KIND_WHERE, "WHERE");

		/*
		 * Remove the EXCLUDED pseudo relation from the query namespace, since
		 * it's not supposed to be available in RETURNING.  (Maybe someday we
		 * could allow that, and drop this step.)
		 */
		Assert((ParseNamespaceItem *) llast(pstate->p_namespace) == exclNSItem);
		pstate->p_namespace = list_delete_last(pstate->p_namespace);
	}

	/* Finally, build ON CONFLICT DO [NOTHING | UPDATE] expression */
	/* 最后，构建 ON CONFLICT DO [NOTHING | UPDATE] 表达式 */
	result = makeNode(OnConflictExpr);

	result->action = onConflictClause->action;
	result->arbiterElems = arbiterElems;
	result->arbiterWhere = arbiterWhere;
	result->constraint = arbiterConstraint;
	result->onConflictSet = onConflictSet;
	result->onConflictWhere = onConflictWhere;
	result->exclRelIndex = exclRelIndex;
	result->exclRelTlist = exclRelTlist;

	return result;
}


/*
 * BuildOnConflictExcludedTargetlist
 *		Create target list for the EXCLUDED pseudo-relation of ON CONFLICT,
 *		representing the columns of targetrel with varno exclRelIndex.
 *
 * 为 ON CONFLICT 的 EXCLUDED 伪关系创建目标列表，表示 varno 为 exclRelIndex 
 * 的 targetrel 的列。
 *
 * Note: Exported for use in the rewriter.
 *
 * 注意：导出以供重写器使用。
 */
List *
BuildOnConflictExcludedTargetlist(Relation targetrel,
								  Index exclRelIndex)
{
	List	   *result = NIL;
	int			attno;
	Var		   *var;
	TargetEntry *te;

	/*
	 * Note that resnos of the tlist must correspond to attnos of the
	 * underlying relation, hence we need entries for dropped columns too.
	 */
	for (attno = 0; attno < RelationGetNumberOfAttributes(targetrel); attno++)
	{
		Form_pg_attribute attr = TupleDescAttr(targetrel->rd_att, attno);
		char	   *name;

		if (attr->attisdropped)
		{
			/*
			 * can't use atttypid here, but it doesn't really matter what type
			 * the Const claims to be.
			 */
			var = (Var *) makeNullConst(INT4OID, -1, InvalidOid);
			name = NULL;
		}
		else
		{
			var = makeVar(exclRelIndex, attno + 1,
						  attr->atttypid, attr->atttypmod,
						  attr->attcollation,
						  0);
			name = pstrdup(NameStr(attr->attname));
		}

		te = makeTargetEntry((Expr *) var,
							 attno + 1,
							 name,
							 false);

		result = lappend(result, te);
	}

	/*
	 * Add a whole-row-Var entry to support references to "EXCLUDED.*".  Like
	 * the other entries in the EXCLUDED tlist, its resno must match the Var's
	 * varattno, else the wrong things happen while resolving references in
	 * setrefs.c.  This is against normal conventions for targetlists, but
	 * it's okay since we don't use this as a real tlist.
	 */
	var = makeVar(exclRelIndex, InvalidAttrNumber,
				  targetrel->rd_rel->reltype,
				  -1, InvalidOid, 0);
	te = makeTargetEntry((Expr *) var, InvalidAttrNumber, NULL, true);
	result = lappend(result, te);

	return result;
}


/*
 * count_rowexpr_columns -
 *	  get number of columns contained in a ROW() expression;
 *	  return -1 if expression isn't a RowExpr or a Var referencing one.
 *
 * 获取 ROW() 表达式中包含的列数；如果表达式不是 RowExpr 或引用 RowExpr 的 Var，
 * 则返回 -1。
 *
 * This is currently used only for hint purposes, so we aren't terribly
 * tense about recognizing all possible cases.  The Var case is interesting
 * because that's what we'll get in the INSERT ... SELECT (...) case.
 *
 * 目前这仅用于提示目的，因此我们并不十分紧张于识别所有可能的情况。
 * Var 的情况很有趣，因为这正是我们在 INSERT ... SELECT (...) 情况下会得到的。
 */
static int
count_rowexpr_columns(ParseState *pstate, Node *expr)
{
	if (expr == NULL)
		return -1;
	if (IsA(expr, RowExpr))
		return list_length(((RowExpr *) expr)->args);
	if (IsA(expr, Var))
	{
		Var		   *var = (Var *) expr;
		AttrNumber	attnum = var->varattno;

		if (attnum > 0 && var->vartype == RECORDOID)
		{
			RangeTblEntry *rte;

			rte = GetRTEByRangeTablePosn(pstate, var->varno, var->varlevelsup);
			if (rte->rtekind == RTE_SUBQUERY)
			{
				/* Subselect-in-FROM: examine sub-select's output expr */
				/* FROM 中的子查询：检查子查询的输出表达式 */
				TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
													attnum);

				if (ste == NULL || ste->resjunk)
					return -1;
				expr = (Node *) ste->expr;
				if (IsA(expr, RowExpr))
					return list_length(((RowExpr *) expr)->args);
			}
		}
	}
	return -1;
}


/*
 * transformSelectStmt -
 *	  transforms a Select Statement
 *
 * 转换 Select 语句
 *
 * Note: this covers only cases with no set operations and no VALUES lists;
 * see below for the other cases.
 *
 * 注意：这仅涵盖没有集合操作且没有 VALUES 列表的情况；其他情况见下文。
 */
static Query *
transformSelectStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	Node	   *qual;
	ListCell   *l;

	qry->commandType = CMD_SELECT;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/* Complain if we get called from someplace where INTO is not allowed */
	/* 如果从不允许 INTO 的地方调用，则报错 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	pstate->p_locking_clause = stmt->lockingClause;

	/* make WINDOW info available for window functions, too */
	pstate->p_windowdefs = stmt->windowClause;

	/* process the FROM clause */
	transformFromClause(pstate, stmt->fromClause);

	/* transform targetlist */
	/* 转换目标列表 */
	qry->targetList = transformTargetList(pstate, stmt->targetList,
										  EXPR_KIND_SELECT_TARGET);

	/* mark column origins */
	markTargetListOrigins(pstate, qry->targetList);

	/* transform WHERE */
	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* initial processing of HAVING clause is much like WHERE clause */
	qry->havingQual = transformWhereClause(pstate, stmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* allow SQL92 rules */ );
	qry->groupDistinct = stmt->groupDistinct;

	if (stmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(stmt->distinctClause) == NULL)
	{
		/* We had SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		/* 我们使用了 SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														stmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* transform LIMIT */
	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	/* resolve any still-unresolved output columns as being type text */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, stmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * transformValuesClause -
 *	  transforms a VALUES clause that's being used as a standalone SELECT
 *
 * 转换作为独立 SELECT 使用的 VALUES 子句
 *
 * We build a Query containing a VALUES RTE, rather as if one had written
 *			SELECT * FROM (VALUES ...) AS "*VALUES*"
 *
 * 我们构建一个包含 VALUES RTE 的 Query，就像写成了 SELECT * FROM (VALUES ...) AS "*VALUES*" 一样
 */
static Query *
transformValuesClause(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	List	   *exprsLists = NIL;
	List	   *coltypes = NIL;
	List	   *coltypmods = NIL;
	List	   *colcollations = NIL;
	List	  **colexprs = NULL;
	int			sublist_length = -1;
	bool		lateral = false;
	ParseNamespaceItem *nsitem;
	ListCell   *lc;
	ListCell   *lc2;
	int			i;

	qry->commandType = CMD_SELECT;

	/* Most SELECT stuff doesn't apply in a VALUES clause */
	/* 大多数 SELECT 的内容不适用于 VALUES 子句 */
	Assert(stmt->distinctClause == NIL);
	Assert(stmt->intoClause == NULL);
	Assert(stmt->targetList == NIL);
	Assert(stmt->fromClause == NIL);
	Assert(stmt->whereClause == NULL);
	Assert(stmt->groupClause == NIL);
	Assert(stmt->havingClause == NULL);
	Assert(stmt->windowClause == NIL);
	Assert(stmt->op == SETOP_NONE);

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * For each row of VALUES, transform the raw expressions.
	 *
	 * 对于 VALUES 的每一行，转换原始表达式。
	 *
	 * Note that the intermediate representation we build is column-organized
	 * not row-organized.  That simplifies the type and collation processing
	 * below.
	 *
	 * 请注意，我们构建的中间表示是按列组织而非按行组织的。这简化了下面的类型和排序规则处理。
	 */
	foreach(lc, stmt->valuesLists)
	{
		List	   *sublist = (List *) lfirst(lc);

		/*
		 * Do basic expression transformation (same as a ROW() expr, but here
		 * we disallow SetToDefault)
		 *
		 * 执行基础表达式转换（与 ROW() 表达式相同，但在此处我们不允许使用 SetToDefault）
		 */
		sublist = transformExpressionList(pstate, sublist,
										  EXPR_KIND_VALUES, false);

		/*
		 * All the sublists must be the same length, *after* transformation
		 * (which might expand '*' into multiple items).  The VALUES RTE can't
		 * handle anything different.
		 *
		 * 在转换后（可能会将 '*' 扩展为多个项目），所有子列表必须具有相同的长度。
		 * VALUES RTE 无法处理任何不同的情况。
		 */
		if (sublist_length < 0)
		{
			/* Remember post-transformation length of first sublist */
			sublist_length = list_length(sublist);
			/* and allocate array for per-column lists */
			/* 并为每列列表分配数组 */
			colexprs = (List **) palloc0(sublist_length * sizeof(List *));
		}
		else if (sublist_length != list_length(sublist))
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("VALUES lists must all be the same length"),
					 parser_errposition(pstate,
										exprLocation((Node *) sublist))));
		}

		/* Build per-column expression lists */
		/* 构建每列的表达式列表 */
		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			colexprs[i] = lappend(colexprs[i], col);
			i++;
		}

		/* Release sub-list's cells to save memory */
		/* 释放子列表的元胞以节省内存 */
		list_free(sublist);

		/* Prepare an exprsLists element for this row */
		/* 为这一行准备一个 exprsLists 元素 */
		exprsLists = lappend(exprsLists, NIL);
	}

	/*
	 * Now resolve the common types of the columns, and coerce everything to
	 * those types.  Then identify the common typmod and common collation, if
	 * any, of each column.
	 *
	 * 现在解析列的通用类型，并将所有内容强制转换为这些类型。
	 * 然后识别每列的通用类型修饰符 (typmod) 和通用排序规则（如果有）。
	 *
	 * We must do collation processing now because (1) assign_query_collations
	 * doesn't process rangetable entries, and (2) we need to label the VALUES
	 * RTE with column collations for use in the outer query.  We don't
	 * consider conflict of implicit collations to be an error here; instead
	 * the column will just show InvalidOid as its collation, and you'll get a
	 * failure later if that results in failure to resolve a collation.
	 *
	 * 我们现在必须进行排序规则处理，因为 (1) assign_query_collations 不处理范围表条目，
	 * 并且 (2) 我们需要为 VALUES RTE 标记列排序规则，以便在外部查询中使用。
	 * 我们在这里不认为隐式排序规则的冲突是错误；相反，该列将仅显示 InvalidOid 作为其排序规则，
	 * 如果这导致解析排序规则失败，您稍后将收到失败消息。
	 *
	 * Note we modify the per-column expression lists in-place.
	 *
	 * 注意我们会在原位修改每列的表达式列表。
	 */
	for (i = 0; i < sublist_length; i++)
	{
		Oid			coltype;
		int32		coltypmod;
		Oid			colcoll;

		coltype = select_common_type(pstate, colexprs[i], "VALUES", NULL);

		foreach(lc, colexprs[i])
		{
			Node	   *col = (Node *) lfirst(lc);

			col = coerce_to_common_type(pstate, col, coltype, "VALUES");
			lfirst(lc) = col;
		}

		coltypmod = select_common_typmod(pstate, colexprs[i], coltype);
		colcoll = select_common_collation(pstate, colexprs[i], true);

		coltypes = lappend_oid(coltypes, coltype);
		coltypmods = lappend_int(coltypmods, coltypmod);
		colcollations = lappend_oid(colcollations, colcoll);
	}

	/*
	 * Finally, rearrange the coerced expressions into row-organized lists.
	 *
	 * 最后，将强制转换后的表达式重新排列为按行组织的列表。
	 */
	for (i = 0; i < sublist_length; i++)
	{
		forboth(lc, colexprs[i], lc2, exprsLists)
		{
			Node	   *col = (Node *) lfirst(lc);
			List	   *sublist = lfirst(lc2);

			sublist = lappend(sublist, col);
			lfirst(lc2) = sublist;
		}
		list_free(colexprs[i]);
	}

	/*
	 * Ordinarily there can't be any current-level Vars in the expression
	 * lists, because the namespace was empty ... but if we're inside CREATE
	 * RULE, then NEW/OLD references might appear.  In that case we have to
	 * mark the VALUES RTE as LATERAL.
	 *
	 * 通常表达式列表中不能有任何当前层级的 Var，因为命名空间是空的...
	 * 但如果我们在 CREATE RULE 内部，则可能会出现 NEW/OLD 引用。
	 * 在这种情况下，我们必须将 VALUES RTE 标记为 LATERAL。
	 */
	if (pstate->p_rtable != NIL &&
		contain_vars_of_level((Node *) exprsLists, 0))
		lateral = true;

	/*
	 * Generate the VALUES RTE
	 *
	 * 生成 VALUES RTE
	 */
	nsitem = addRangeTableEntryForValues(pstate, exprsLists,
										 coltypes, coltypmods, colcollations,
										 NULL, lateral, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*
	 * Generate a targetlist as though expanding "*"
	 *
	 * 生成一个目标列表，就像展开 "*" 一样
	 */
	Assert(pstate->p_next_resno == 1);
	qry->targetList = expandNSItemAttrs(pstate, nsitem, 0, true, -1);

	/*
	 * The grammar allows attaching ORDER BY, LIMIT, and FOR UPDATE to a
	 * VALUES, so cope.
	 *
	 * 语法允许将 ORDER BY、LIMIT 和 FOR UPDATE 附加到 VALUES，因此要进行处理。
	 */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->limitOffset = transformLimitClause(pstate, stmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, stmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s cannot be applied to VALUES",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformSetOperationStmt -
 *	  transforms a set-operations tree
 *
 * 转换集合操作树
 *
 * A set-operation tree is just a SELECT, but with UNION/INTERSECT/EXCEPT
 * structure to it.  We must transform each leaf SELECT and build up a top-
 * level Query that contains the leaf SELECTs as subqueries in its rangetable.
 * The tree of set operations is converted into the setOperations field of
 * the top-level Query.
 *
 * 集合操作树只是一个 SELECT，但具有 UNION/INTERSECT/EXCEPT 结构。
 * 我们必须转换每个叶子 SELECT 并构建一个顶层 Query，该 Query 在其范围表中包含
 * 作为子查询的叶子 SELECT。集合操作树被转换为顶层 Query 的 setOperations 字段。
 */
static Query *
transformSetOperationStmt(ParseState *pstate, SelectStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	SelectStmt *leftmostSelect;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	SetOperationStmt *sostmt;
	List	   *sortClause;
	Node	   *limitOffset;
	Node	   *limitCount;
	List	   *lockingClause;
	WithClause *withClause;
	Node	   *node;
	ListCell   *left_tlist,
			   *lct,
			   *lcm,
			   *lcc,
			   *l;
	List	   *targetvars,
			   *targetnames,
			   *sv_namespace;
	int			sv_rtable_length;
	ParseNamespaceItem *jnsitem;
	ParseNamespaceColumn *sortnscolumns;
	int			sortcolindex;
	int			tllen;

	qry->commandType = CMD_SELECT;

	/*
	 * Find leftmost leaf SelectStmt.  We currently only need to do this in
	 * order to deliver a suitable error message if there's an INTO clause
	 * there, implying the set-op tree is in a context that doesn't allow
	 * INTO.  (transformSetOperationTree would throw error anyway, but it
	 * seems worth the trouble to throw a different error for non-leftmost
	 * INTO, so we produce that error in transformSetOperationTree.)
	 *
	 * 找到最左边的叶子 SelectStmt。我们目前只需要这样做，以便在存在 INTO 子句时
	 * 提供合适的错误消息，这暗示集合操作树处于一个不允许 INTO 的上下文中。
	 *（transformSetOperationTree 无论如何都会抛出错误，但对于非最左边的 INTO
	 * 抛出不同的错误似乎是值得的，所以我们在 transformSetOperationTree
	 * 中产生该错误。）
	 */
	leftmostSelect = stmt->larg;
	while (leftmostSelect && leftmostSelect->op != SETOP_NONE)
		leftmostSelect = leftmostSelect->larg;
	Assert(leftmostSelect && IsA(leftmostSelect, SelectStmt) &&
		   leftmostSelect->larg == NULL);
	if (leftmostSelect->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("SELECT ... INTO is not allowed here"),
				 parser_errposition(pstate,
									exprLocation((Node *) leftmostSelect->intoClause))));

	/*
	 * We need to extract ORDER BY and other top-level clauses here and not
	 * let transformSetOperationTree() see them --- else it'll just recurse
	 * right back here!
	 *
	 * 我们需要在此处提取 ORDER BY 和其他顶层子句，而不让 transformSetOperationTree()
	 * 看到它们 —— 否则它只会直接递归回到这里！
	 */
	sortClause = stmt->sortClause;
	limitOffset = stmt->limitOffset;
	limitCount = stmt->limitCount;
	lockingClause = stmt->lockingClause;
	withClause = stmt->withClause;

	stmt->sortClause = NIL;
	stmt->limitOffset = NULL;
	stmt->limitCount = NULL;
	stmt->lockingClause = NIL;
	stmt->withClause = NULL;

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(lockingClause))->strength))));

	/* Process the WITH clause independently of all else */
	/* 独立于其他所有内容处理 WITH 子句 */
	if (withClause)
	{
		qry->hasRecursive = withClause->recursive;
		qry->cteList = transformWithClause(pstate, withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * Recursively transform the components of the tree.
	 *
	 * 递归转换树的组件。
	 */
	sostmt = castNode(SetOperationStmt,
					  transformSetOperationTree(pstate, stmt, true, NULL));
	Assert(sostmt);
	qry->setOperations = (Node *) sostmt;

	/*
	 * Re-find leftmost SELECT (now it's a sub-query in rangetable)
	 *
	 * 重新找到最左边的 SELECT（现在它是范围表中的子查询）
	 */
	node = sostmt->larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist for outer query using column names of
	 * leftmost select and common datatypes/collations of topmost set
	 * operation.  Also make lists of the dummy vars and their names for use
	 * in parsing ORDER BY.
	 *
	 * 使用最左侧 select 的列名和顶层集合操作的通用数据类型/排序规则，
	 * 为外部查询生成虚拟目标列表。还生成虚拟变量及其名称的列表，以便在解析 ORDER BY 时使用。
	 *
	 * Note: we use leftmostRTI as the varno of the dummy variables. It
	 * shouldn't matter too much which RT index they have, as long as they
	 * have one that corresponds to a real RT entry; else funny things may
	 * happen when the tree is mashed by rule rewriting.
	 *
	 * 注意：我们使用 leftmostRTI 作为虚拟变量的 varno。它们具有哪个 RT 索引并不重要，
	 * 只要它们具有与实际 RT 条目相对应的索引即可；否则，当规则重写捣碎树时，可能会发生有趣的事情。
	 */
	qry->targetList = NIL;
	targetvars = NIL;
	targetnames = NIL;
	sortnscolumns = (ParseNamespaceColumn *)
		palloc0(list_length(sostmt->colTypes) * sizeof(ParseNamespaceColumn));
	sortcolindex = 0;

	forfour(lct, sostmt->colTypes,
			lcm, sostmt->colTypmods,
			lcc, sostmt->colCollations,
			left_tlist, leftmostQuery->targetList)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		var = makeVar(leftmostRTI,
					  lefttle->resno,
					  colType,
					  colTypmod,
					  colCollation,
					  0);
		var->location = exprLocation((Node *) lefttle->expr);
		tle = makeTargetEntry((Expr *) var,
							  (AttrNumber) pstate->p_next_resno++,
							  colName,
							  false);
		qry->targetList = lappend(qry->targetList, tle);
		targetvars = lappend(targetvars, var);
		targetnames = lappend(targetnames, makeString(colName));
		sortnscolumns[sortcolindex].p_varno = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattno = lefttle->resno;
		sortnscolumns[sortcolindex].p_vartype = colType;
		sortnscolumns[sortcolindex].p_vartypmod = colTypmod;
		sortnscolumns[sortcolindex].p_varcollid = colCollation;
		sortnscolumns[sortcolindex].p_varnosyn = leftmostRTI;
		sortnscolumns[sortcolindex].p_varattnosyn = lefttle->resno;
		sortcolindex++;
	}

	/*
	 * As a first step towards supporting sort clauses that are expressions
	 * using the output columns, generate a namespace entry that makes the
	 * output columns visible.  A Join RTE node is handy for this, since we
	 * can easily control the Vars generated upon matches.
	 *
	 * 作为支持使用输出列作为表达式的排序子句的第一步，生成一个使输出列可见的命名空间条目。
	 * Join RTE 节点对此很方便，因为我们可以轻松控制匹配时生成的 Var。
	 *
	 * Note: we don't yet do anything useful with such cases, but at least
	 * "ORDER BY upper(foo)" will draw the right error message rather than
	 * "foo not found".
	 *
	 * 注意：我们目前还没有对这种情况做任何有用的处理，但至少 "ORDER BY upper(foo)"
	 * 将引出正确的错误消息，而不是 "foo not found"。
	 */
	sv_rtable_length = list_length(pstate->p_rtable);

	jnsitem = addRangeTableEntryForJoin(pstate,
										targetnames,
										sortnscolumns,
										JOIN_INNER,
										0,
										targetvars,
										NIL,
										NIL,
										NULL,
										NULL,
										false);

	sv_namespace = pstate->p_namespace;
	pstate->p_namespace = NIL;

	/* add jnsitem to column namespace only */
	/* 仅将 jnsitem 添加到列命名空间 */
	addNSItemToQuery(pstate, jnsitem, false, false, true);

	/*
	 * For now, we don't support resjunk sort clauses on the output of a
	 * setOperation tree --- you can only use the SQL92-spec options of
	 * selecting an output column by name or number.  Enforce by checking that
	 * transformSortClause doesn't add any items to tlist.  Note, if changing
	 * this, add_setop_child_rel_equivalences() will need to be updated.
	 *
	 * 目前，我们不支持在集合操作树的输出上使用 resjunk 排序子句 —— 您只能使用
	 * SQL92 规范选项，即通过名称或编号选择输出列。通过检查 transformSortClause
	 * 不会向 tlist 添加任何项来强制执行此操作。注意，如果更改此设置，
	 * 需要更新 add_setop_child_rel_equivalences()。
	 */
	tllen = list_length(qry->targetList);

	qry->sortClause = transformSortClause(pstate,
										  sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	/* restore namespace, remove join RTE from rtable */
	/* 恢复命名空间，从 rtable 中移除 join RTE */
	pstate->p_namespace = sv_namespace;
	pstate->p_rtable = list_truncate(pstate->p_rtable, sv_rtable_length);

	if (tllen != list_length(qry->targetList))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid UNION/INTERSECT/EXCEPT ORDER BY clause"),
				 errdetail("Only result column names can be used, not expressions or functions."),
				 errhint("Add the expression/function to every SELECT, or move the UNION into a FROM clause."),
				 parser_errposition(pstate,
									exprLocation(list_nth(qry->targetList, tllen)))));

	qry->limitOffset = transformLimitClause(pstate, limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											stmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   stmt->limitOption);
	qry->limitOption = stmt->limitOption;

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}

/*
 * Make a SortGroupClause node for a SetOperationStmt's groupClauses
 *
 * 为 SetOperationStmt 的 groupClauses 创建一个 SortGroupClause 节点
 *
 * If require_hash is true, the caller is indicating that they need hash
 * support or they will fail.  So look extra hard for hash support.
 *
 * 如果 require_hash 为 true，则调用者表明他们需要哈希支持，否则将失败。
 * 因此要特别努力地寻找哈希支持。
 */
SortGroupClause *
makeSortGroupClauseForSetOp(Oid rescoltype, bool require_hash)
{
	SortGroupClause *grpcl = makeNode(SortGroupClause);
	Oid			sortop;
	Oid			eqop;
	bool		hashable;

	/* determine the eqop and optional sortop */
	get_sort_group_operators(rescoltype,
							 false, true, false,
							 &sortop, &eqop, NULL,
							 &hashable);

	/*
	 * The type cache doesn't believe that record is hashable (see
	 * cache_record_field_properties()), but if the caller really needs hash
	 * support, we can assume it does.  Worst case, if any components of the
	 * record don't support hashing, we will fail at execution.
	 *
	 * 类型缓存不相信 record 是可哈希的（参见 cache_record_field_properties()），
	 * 但如果调用者确实需要哈希支持，我们可以假设它是可哈希的。最坏的情况下，
	 * 如果记录的任何组件不支持哈希，我们将在执行时失败。
	 */
	if (require_hash && (rescoltype == RECORDOID || rescoltype == RECORDARRAYOID))
		hashable = true;

	/* we don't have a tlist yet, so can't assign sortgrouprefs */
	/* 我们还没有 tlist，所以无法分配 sortgrouprefs */
	grpcl->tleSortGroupRef = 0;
	grpcl->eqop = eqop;
	grpcl->sortop = sortop;
	grpcl->reverse_sort = false;	/* Sort-op is "less than", or InvalidOid */
	/* 排序操作是“小于”，或者是 InvalidOid */
	grpcl->nulls_first = false; /* OK with or without sortop */
	/* 无论有没有排序操作都可以 */
	grpcl->hashable = hashable;

	return grpcl;
}

/*
 * transformSetOperationTree
 *		Recursively transform leaves and internal nodes of a set-op tree
 *
 * 递归转换集合操作树的叶子和内部节点
 *
 * In addition to returning the transformed node, if targetlist isn't NULL
 * then we return a list of its non-resjunk TargetEntry nodes.  For a leaf
 * set-op node these are the actual targetlist entries; otherwise they are
 * dummy entries created to carry the type, typmod, collation, and location
 * (for error messages) of each output column of the set-op node.  This info
 * is needed only during the internal recursion of this function, so outside
 * callers pass NULL for targetlist.  Note: the reason for passing the
 * actual targetlist entries of a leaf node is so that upper levels can
 * replace UNKNOWN Consts with properly-coerced constants.
 *
 * 除了返回转换后的节点外，如果 targetlist 不为 NULL，则返回其非 resjunk TargetEntry
 * 节点的列表。对于叶子集合操作节点，这些是实际的目标列表条目；否则它们是创建的虚拟条目，
 * 用于承载集合操作节点每个输出列的类型、类型修饰符、排序规则和位置（用于错误消息）。
 * 仅在此函数的内部递归期间需要此信息，因此外部调用者为 targetlist 传递 NULL。
 * 注意：传递叶子节点的实际目标列表条目的原因是，上层可以将被强制转换后的常量替换
 * UNKNOWN Const。
 */
static Node *
transformSetOperationTree(ParseState *pstate, SelectStmt *stmt,
						  bool isTopLevel, List **targetlist)
{
	bool		isLeaf;

	Assert(stmt && IsA(stmt, SelectStmt));

	/* Guard against stack overflow due to overly complex set-expressions */
	/* 防止由于过于复杂的集合表达式导致栈溢出 */
	check_stack_depth();

	/*
	 * Validity-check both leaf and internal SELECTs for disallowed ops.
	 *
	 * 对叶子和内部 SELECT 进行 disallowed 操作的有效性检查。
	 */
	if (stmt->intoClause)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("INTO is only allowed on first SELECT of UNION/INTERSECT/EXCEPT"),
				 parser_errposition(pstate,
									exprLocation((Node *) stmt->intoClause))));

	/* We don't support FOR UPDATE/SHARE with set ops at the moment. */
	if (stmt->lockingClause)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(((LockingClause *)
									  linitial(stmt->lockingClause))->strength))));

	/*
	 * If an internal node of a set-op tree has ORDER BY, LIMIT, FOR UPDATE,
	 * or WITH clauses attached, we need to treat it like a leaf node to
	 * generate an independent sub-Query tree.  Otherwise, it can be
	 * represented by a SetOperationStmt node underneath the parent Query.
	 */
	if (stmt->op == SETOP_NONE)
	{
		Assert(stmt->larg == NULL && stmt->rarg == NULL);
		isLeaf = true;
	}
	else
	{
		Assert(stmt->larg != NULL && stmt->rarg != NULL);
		if (stmt->sortClause || stmt->limitOffset || stmt->limitCount ||
			stmt->lockingClause || stmt->withClause)
			isLeaf = true;
		else
			isLeaf = false;
	}

	if (isLeaf)
	{
		/* Process leaf SELECT */
		/* 处理叶子 SELECT */
		Query	   *selectQuery;
		char		selectName[32];
		ParseNamespaceItem *nsitem;
		RangeTblRef *rtr;
		ListCell   *tl;

		/*
		 * Transform SelectStmt into a Query.
		 *
		 * 将 SelectStmt 转换为 Query。
		 *
		 * This works the same as SELECT transformation normally would, except
		 * that we prevent resolving unknown-type outputs as TEXT.  This does
		 * not change the subquery's semantics since if the column type
		 * matters semantically, it would have been resolved to something else
		 * anyway.  Doing this lets us resolve such outputs using
		 * select_common_type(), below.
		 *
		 * 这与正常的 SELECT 转换工作方式相同，除了我们阻止将未知类型的输出解析为 TEXT。
		 * 这不会改变子查询的语义，因为如果列类型在语义上很重要，
		 * 它无论如何都会被解析为其他类型。这样做让我们可以使用下面的 
		 * select_common_type() 解析此类输出。
		 *
		 * Note: previously transformed sub-queries don't affect the parsing
		 * of this sub-query, because they are not in the toplevel pstate's
		 * namespace list.
		 *
		 * 注意：先前转换的子查询不会影响此子查询的解析，因为它们不在顶级 pstate 的
		 * 命名空间列表中。
		 */
		selectQuery = parse_sub_analyze((Node *) stmt, pstate,
										NULL, false, false);

		/*
		 * Check for bogus references to Vars on the current query level (but
		 * upper-level references are okay). Normally this can't happen
		 * because the namespace will be empty, but it could happen if we are
		 * inside a rule.
		 */
		/*
		 * 检查当前查询级别上是否存在伪造的 Var 引用（但上层引用是可以的）。
		 * 通常这不会发生，因为命名空间将为空，但如果我们处于规则内部，则可能会发生。
		 */
		if (pstate->p_namespace)
		{
			if (contain_vars_of_level((Node *) selectQuery, 1))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("UNION/INTERSECT/EXCEPT member statement cannot refer to other relations of same query level"),
						 parser_errposition(pstate,
											locate_var_of_level((Node *) selectQuery, 1))));
		}

		/*
		 * Extract a list of the non-junk TLEs for upper-level processing.
		 *
		 * 提取非 junk TLE 的列表以供上层处理。
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach(tl, selectQuery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tl);

				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}

		/*
		 * Make the leaf query be a subquery in the top-level rangetable.
		 *
		 * 使叶子查询成为顶层范围表中的子查询。
		 */
		snprintf(selectName, sizeof(selectName), "*SELECT* %d",
				 list_length(pstate->p_rtable) + 1);
		nsitem = addRangeTableEntryForSubquery(pstate,
											   selectQuery,
											   makeAlias(selectName, NIL),
											   false,
											   false);

		/*
		 * Return a RangeTblRef to replace the SelectStmt in the set-op tree.
		 *
		 * 返回一个 RangeTblRef 以替换集合操作树中的 SelectStmt。
		 */
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = nsitem->p_rtindex;
		return (Node *) rtr;
	}
	else
	{
		/* Process an internal node (set operation node) */
		/* 处理内部节点（集合操作节点） */
		SetOperationStmt *op = makeNode(SetOperationStmt);
		List	   *ltargetlist;
		List	   *rtargetlist;
		ListCell   *ltl;
		ListCell   *rtl;
		const char *context;
		bool		recursive = (pstate->p_parent_cte &&
								 pstate->p_parent_cte->cterecursive);
		/* 是否为递归查询 */

		context = (stmt->op == SETOP_UNION ? "UNION" :
				   (stmt->op == SETOP_INTERSECT ? "INTERSECT" :
					"EXCEPT"));

		op->op = stmt->op;
		op->all = stmt->all;

		/*
		 * Recursively transform the left child node.
		 *
		 * 递归转换左子节点。
		 */
		op->larg = transformSetOperationTree(pstate, stmt->larg,
											 false,
											 &ltargetlist);

		/*
		 * If we are processing a recursive union query, now is the time to
		 * examine the non-recursive term's output columns and mark the
		 * containing CTE as having those result columns.  We should do this
		 * only at the topmost setop of the CTE, of course.
		 *
		 * 如果我们正在处理递归并集查询，现在是时候检查非递归项的输出列，并标记包含的
		 * CTE 具有这些结果列。当然，我们应该仅在 CTE 的顶层集合操作处执行此操作。
		 */
		if (isTopLevel && recursive)
			determineRecursiveColTypes(pstate, op->larg, ltargetlist);

		/*
		 * Recursively transform the right child node.
		 *
		 * 递归转换右子节点。
		 */
		op->rarg = transformSetOperationTree(pstate, stmt->rarg,
											 false,
											 &rtargetlist);

		/*
		 * Verify that the two children have the same number of non-junk
		 * columns, and determine the types of the merged output columns.
		 *
		 * 验证两个子查询是否具有相同数量的非 junk 列，并确定合并后输出列的类型。
		 */
		if (list_length(ltargetlist) != list_length(rtargetlist))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("each %s query must have the same number of columns",
							context),
					 parser_errposition(pstate,
										exprLocation((Node *) rtargetlist))));

		if (targetlist)
			*targetlist = NIL;
		op->colTypes = NIL;
		op->colTypmods = NIL;
		op->colCollations = NIL;
		op->groupClauses = NIL;
		forboth(ltl, ltargetlist, rtl, rtargetlist)
		{
			TargetEntry *ltle = (TargetEntry *) lfirst(ltl);
			TargetEntry *rtle = (TargetEntry *) lfirst(rtl);
			Node	   *lcolnode = (Node *) ltle->expr;
			Node	   *rcolnode = (Node *) rtle->expr;
			Oid			lcoltype = exprType(lcolnode);
			Oid			rcoltype = exprType(rcolnode);
			Node	   *bestexpr;
			int			bestlocation;
			Oid			rescoltype;
			int32		rescoltypmod;
			Oid			rescolcoll;

			/* select common type, same as CASE et al */
			/* 选择公共类型，与 CASE 等相同 */
			rescoltype = select_common_type(pstate,
											list_make2(lcolnode, rcolnode),
											context,
											&bestexpr);
			bestlocation = exprLocation(bestexpr);

			/*
			 * Verify the coercions are actually possible.  If not, we'd fail
			 * later anyway, but we want to fail now while we have sufficient
			 * context to produce an error cursor position.
			 *
			 * 验证强制转换实际上是否可行。如果不可行，我们无论如何都会在稍后失败，但我们希望
			 * 现在失败，因为我们有足够的上下文来生成错误游标位置。
			 *
			 * For all non-UNKNOWN-type cases, we verify coercibility but we
			 * don't modify the child's expression, for fear of changing the
			 * child query's semantics.
			 *
			 * 对于所有非 UNKNOWN 类型的情况，我们验证可强制性，但我们不修改子查询的表达式，
			 * 以免改变子查询的语义。
			 *
			 * If a child expression is an UNKNOWN-type Const or Param, we
			 * want to replace it with the coerced expression.  This can only
			 * happen when the child is a leaf set-op node.  It's safe to
			 * replace the expression because if the child query's semantics
			 * depended on the type of this output column, it'd have already
			 * coerced the UNKNOWN to something else.  We want to do this
			 * because (a) we want to verify that a Const is valid for the
			 * target type, or resolve the actual type of an UNKNOWN Param,
			 * and (b) we want to avoid unnecessary discrepancies between the
			 * output type of the child query and the resolved target type.
			 * Such a discrepancy would disable optimization in the planner.
			 *
			 * 如果子表达式是一个 UNKNOWN 类型的 Const 或 Param，我们希望将其替换为强制转换后的表达式。
			 * 这仅在子查询是一个叶子集合操作节点时发生。替换表达式是安全的，因为如果子查询的语义
			 * 依赖于此输出列的类型，它肯定已经将 UNKNOWN 强制转换为其他类型了。我们希望这样做，
			 * 因为 (a) 我们希望验证 Const 对于目标类型是否有效，或者解析 UNKNOWN Param 
			 * 的实际类型，以及 (b) 我们希望避免子查询的输出类型与解析后的目标类型之间出现
			 * 不必要的差异。这种差异会禁用规划器中的优化。
			 *
			 * If it's some other UNKNOWN-type node, eg a Var, we do nothing
			 * (knowing that coerce_to_common_type would fail).  The planner
			 * is sometimes able to fold an UNKNOWN Var to a constant before
			 * it has to coerce the type, so failing now would just break
			 * cases that might work.
			 *
			 * 如果是某些其他 UNKNOWN 类型的节点，例如 Var，我们什么都不做
			 *（知道 coerce_to_common_type 会失败）。规划器有时能够在必须强制转换类型之前
			 * 将 UNKNOWN Var 折叠为常量，因此现在失败只会破坏可能有效的情况。
			 */
			if (lcoltype != UNKNOWNOID)
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
			else if (IsA(lcolnode, Const) ||
					 IsA(lcolnode, Param))
			{
				lcolnode = coerce_to_common_type(pstate, lcolnode,
												 rescoltype, context);
				ltle->expr = (Expr *) lcolnode;
			}

			if (rcoltype != UNKNOWNOID)
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
			else if (IsA(rcolnode, Const) ||
					 IsA(rcolnode, Param))
			{
				rcolnode = coerce_to_common_type(pstate, rcolnode,
												 rescoltype, context);
				rtle->expr = (Expr *) rcolnode;
			}

			rescoltypmod = select_common_typmod(pstate,
												list_make2(lcolnode, rcolnode),
												rescoltype);

			/*
			 * Select common collation.  A common collation is required for
			 * all set operators except UNION ALL; see SQL:2008 7.13 <query
			 * expression> Syntax Rule 15c.  (If we fail to identify a common
			 * collation for a UNION ALL column, the colCollations element
			 * will be set to InvalidOid, which may result in a runtime error
			 * if something at a higher query level wants to use the column's
			 * collation.)
			 *
			 * 选择通用排序规则。除 UNION ALL 外，所有集合运算符都需要通用排序规则；
			 * 请参阅 SQL:2008 7.13 <query expression> 语法规则 15c。
			 *（如果我们未能识别 UNION ALL 列的通用排序规则，则 colCollations 元素
			 * 将设置为 InvalidOid，如果更高查询级别的某些内容想要使用该列的排序规则，
			 * 可能会导致运行时错误。）
			 */
			rescolcoll = select_common_collation(pstate,
												 list_make2(lcolnode, rcolnode),
												 (op->op == SETOP_UNION && op->all));

			/* emit results */
			op->colTypes = lappend_oid(op->colTypes, rescoltype);
			op->colTypmods = lappend_int(op->colTypmods, rescoltypmod);
			op->colCollations = lappend_oid(op->colCollations, rescolcoll);

			/*
			 * For all cases except UNION ALL, identify the grouping operators
			 * (and, if available, sorting operators) that will be used to
			 * eliminate duplicates.
			 */
			if (op->op != SETOP_UNION || !op->all)
			{
				ParseCallbackState pcbstate;

				setup_parser_errposition_callback(&pcbstate, pstate,
												  bestlocation);

				/*
				 * If it's a recursive union, we need to require hashing
				 * support.
				 */
				op->groupClauses = lappend(op->groupClauses,
										   makeSortGroupClauseForSetOp(rescoltype, recursive));

				cancel_parser_errposition_callback(&pcbstate);
			}

			/*
			 * Construct a dummy tlist entry to return.  We use a SetToDefault
			 * node for the expression, since it carries exactly the fields
			 * needed, but any other expression node type would do as well.
			 */
			if (targetlist)
			{
				SetToDefault *rescolnode = makeNode(SetToDefault);
				TargetEntry *restle;

				rescolnode->typeId = rescoltype;
				rescolnode->typeMod = rescoltypmod;
				rescolnode->collation = rescolcoll;
				rescolnode->location = bestlocation;
				restle = makeTargetEntry((Expr *) rescolnode,
										 0, /* no need to set resno */
										 NULL,
										 false);
				*targetlist = lappend(*targetlist, restle);
			}
		}

		return (Node *) op;
	}
}

/*
 * Process the outputs of the non-recursive term of a recursive union
 * to set up the parent CTE's columns
 *
 * 处理递归并集的非递归项的输出，以设置父 CTE 的列
 */
static void
determineRecursiveColTypes(ParseState *pstate, Node *larg, List *nrtargetlist)
{
	Node	   *node;
	int			leftmostRTI;
	Query	   *leftmostQuery;
	List	   *targetList;
	ListCell   *left_tlist;
	ListCell   *nrtl;
	int			next_resno;

	/*
	 * Find leftmost leaf SELECT
	 */
	/* 查找最左侧的叶子 SELECT */
	node = larg;
	while (node && IsA(node, SetOperationStmt))
		node = ((SetOperationStmt *) node)->larg;
	Assert(node && IsA(node, RangeTblRef));
	leftmostRTI = ((RangeTblRef *) node)->rtindex;
	leftmostQuery = rt_fetch(leftmostRTI, pstate->p_rtable)->subquery;
	Assert(leftmostQuery != NULL);

	/*
	 * Generate dummy targetlist using column names of leftmost select and
	 * dummy result expressions of the non-recursive term.
	 */
	/* 
	 * 使用最左侧 select 的列名和非递归项的虚拟结果表达式生成虚拟目标列表。
	 */
	targetList = NIL;
	next_resno = 1;

	forboth(nrtl, nrtargetlist, left_tlist, leftmostQuery->targetList)
	{
		TargetEntry *nrtle = (TargetEntry *) lfirst(nrtl);
		TargetEntry *lefttle = (TargetEntry *) lfirst(left_tlist);
		char	   *colName;
		TargetEntry *tle;

		Assert(!lefttle->resjunk);
		colName = pstrdup(lefttle->resname);
		tle = makeTargetEntry(nrtle->expr,
							  next_resno++,
							  colName,
							  false);
		targetList = lappend(targetList, tle);
	}

	/* Now build CTE's output column info using dummy targetlist */
	/* 现在使用虚拟目标列表构建 CTE 的输出列信息 */
	analyzeCTETargetList(pstate, pstate->p_parent_cte, targetList);
}


/*
 * transformReturnStmt -
 *	  transforms a return statement
 *
 * 转换 return 语句
 */
static Query *
transformReturnStmt(ParseState *pstate, ReturnStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_SELECT;
	qry->isReturn = true;
	/* 将 return 标记为 true */

	qry->targetList = list_make1(makeTargetEntry((Expr *) transformExpr(pstate, stmt->returnval, EXPR_KIND_SELECT_TARGET),
												 1, NULL, false));

	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->targetList);
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);
	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;
	/* 记录查询特性标记 */

	assign_query_collations(pstate, qry);

	return qry;
}


/*
 * transformUpdateStmt -
 *	  transforms an update statement
 *
 * 转换 update 语句
 */
static Query *
transformUpdateStmt(ParseState *pstate, UpdateStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ParseNamespaceItem *nsitem;
	Node	   *qual;

	qry->commandType = CMD_UPDATE;
	pstate->p_is_insert = false;
	/* 标记为 UPDATE 操作而非 INSERT */

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		qry->hasRecursive = stmt->withClause->recursive;
		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 true,
										 ACL_UPDATE);
	nsitem = pstate->p_target_nsitem;

	/* subqueries in FROM cannot access the result relation */
	/* FROM 中的子查询不能访问结果关系 */
	nsitem->p_lateral_only = true;
	nsitem->p_lateral_ok = false;

	/*
	 * the FROM clause is non-standard SQL syntax. We used to be able to do
	 * this with REPLACE in POSTQUEL so we keep the feature.
	 */
	/*
	 * FROM 子句是非标准 SQL 语法。我们以前可以在 POSTQUEL 中使用 REPLACE 
	 * 来实现此目的，因此我们保留了此功能。
	 */
	transformFromClause(pstate, stmt->fromClause);

	/* remaining clauses can reference the result relation normally */
	nsitem->p_lateral_only = false;
	nsitem->p_lateral_ok = true;

	qual = transformWhereClause(pstate, stmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	transformReturningClause(pstate, qry, stmt->returningClause,
							 EXPR_KIND_RETURNING);
	/* 处理 RETURNING 子句 */

	/*
	 * Now we are done with SELECT-like processing, and can get on with
	 * transforming the target list to match the UPDATE target columns.
	 */
	qry->targetList = transformUpdateTargetList(pstate, stmt->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasSubLinks = pstate->p_hasSubLinks;
	/* 记录查询特性标记 */

	assign_query_collations(pstate, qry);

	return qry;
}

/*
 * transformUpdateTargetList -
 *	handle SET clause in UPDATE/MERGE/INSERT ... ON CONFLICT UPDATE
 *
 * 处理 UPDATE/MERGE/INSERT ... ON CONFLICT UPDATE 中的 SET 子句
 */
List *
transformUpdateTargetList(ParseState *pstate, List *origTlist)
{
	List	   *tlist = NIL;
	RTEPermissionInfo *target_perminfo;
	ListCell   *orig_tl;
	ListCell   *tl;

	tlist = transformTargetList(pstate, origTlist,
								EXPR_KIND_UPDATE_SOURCE);

	/* Prepare to assign non-conflicting resnos to resjunk attributes */
	if (pstate->p_next_resno <= RelationGetNumberOfAttributes(pstate->p_target_relation))
		pstate->p_next_resno = RelationGetNumberOfAttributes(pstate->p_target_relation) + 1;

	/* Prepare non-junk columns for assignment to target table */
	target_perminfo = pstate->p_target_nsitem->p_perminfo;
	orig_tl = list_head(origTlist);

	foreach(tl, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(tl);
		ResTarget  *origTarget;
		int			attrno;

		if (tle->resjunk)
		{
			/*
			 * Resjunk nodes need no additional processing, but be sure they
			 * have resnos that do not match any target columns; else rewriter
			 * or planner might get confused.  They don't need a resname
			 * either.
			 */
			tle->resno = (AttrNumber) pstate->p_next_resno++;
			tle->resname = NULL;
			continue;
		}
		if (orig_tl == NULL)
			elog(ERROR, "UPDATE target count mismatch --- internal error");
		origTarget = lfirst_node(ResTarget, orig_tl);

		attrno = attnameAttNum(pstate->p_target_relation,
							   origTarget->name, true);
		if (attrno == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							origTarget->name,
							RelationGetRelationName(pstate->p_target_relation)),
					 (origTarget->indirection != NIL &&
					  strcmp(origTarget->name, pstate->p_target_nsitem->p_names->aliasname) == 0) ?
					 errhint("SET target columns cannot be qualified with the relation name.") : 0,
					 parser_errposition(pstate, origTarget->location)));

		updateTargetListEntry(pstate, tle, origTarget->name,
							  attrno,
							  origTarget->indirection,
							  origTarget->location);

		/* Mark the target column as requiring update permissions */
		target_perminfo->updatedCols = bms_add_member(target_perminfo->updatedCols,
													  attrno - FirstLowInvalidHeapAttributeNumber);

		orig_tl = lnext(origTlist, orig_tl);
	}
	if (orig_tl != NULL)
		elog(ERROR, "UPDATE target count mismatch --- internal error");

	return tlist;
}

/*
 * addNSItemForReturning -
 *	add a ParseNamespaceItem for the OLD or NEW alias in RETURNING.
 */
/*
 * addNSItemForReturning -
 *	为 RETURNING 中的 OLD 或 NEW 别名添加 ParseNamespaceItem。
 */
static void
addNSItemForReturning(ParseState *pstate, const char *aliasname,
					  VarReturningType returning_type)
{
	List	   *colnames;
	int			numattrs;
	ParseNamespaceColumn *nscolumns;
	ParseNamespaceItem *nsitem;

	/* copy per-column data from the target relation */
	/* 从目标关系中复制每列数据 */
	colnames = pstate->p_target_nsitem->p_rte->eref->colnames;
	numattrs = list_length(colnames);

	nscolumns = (ParseNamespaceColumn *)
		palloc(numattrs * sizeof(ParseNamespaceColumn));

	memcpy(nscolumns, pstate->p_target_nsitem->p_nscolumns,
		   numattrs * sizeof(ParseNamespaceColumn));

	/* mark all columns as returning OLD/NEW */
	/* 将所有列标记为返回 OLD/NEW */
	for (int i = 0; i < numattrs; i++)
		nscolumns[i].p_varreturningtype = returning_type;

	/* build the nsitem, copying most fields from the target relation */
	/* 构建 nsitem，从目标关系复制大多数字段 */
	nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
	nsitem->p_names = makeAlias(aliasname, colnames);
	nsitem->p_rte = pstate->p_target_nsitem->p_rte;
	nsitem->p_rtindex = pstate->p_target_nsitem->p_rtindex;
	nsitem->p_perminfo = pstate->p_target_nsitem->p_perminfo;
	nsitem->p_nscolumns = nscolumns;
	nsitem->p_returning_type = returning_type;

	/* add it to the query namespace as a table-only item */
	/* 将其作为仅表项目添加到查询命名空间中 */
	addNSItemToQuery(pstate, nsitem, false, true, false);
}

/*
 * transformReturningClause -
 *	handle a RETURNING clause in INSERT/UPDATE/DELETE/MERGE
 */
/*
 * transformReturningClause -
 *	处理 INSERT/UPDATE/DELETE/MERGE 中的 RETURNING 子句
 */
void
transformReturningClause(ParseState *pstate, Query *qry,
						 ReturningClause *returningClause,
						 ParseExprKind exprKind)
{
	int			save_nslen = list_length(pstate->p_namespace);
	int			save_next_resno;

	if (returningClause == NULL)
		return;					/* nothing to do */

	/*
	 * Scan RETURNING WITH(...) options for OLD/NEW alias names.  Complain if
	 * there is any conflict with existing relations.
	 */
	/*
	 * 扫描 RETURNING WITH(...) 选项以获取 OLD/NEW 别名。
	 * 如果与现有关系有任何冲突，则进行投诉。
	 */
	foreach_node(ReturningOption, option, returningClause->options)
	{
		switch (option->option)
		{
			case RETURNING_OPTION_OLD:
				if (qry->returningOldAlias != NULL)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
					/* translator: %s is OLD or NEW */
							errmsg("%s cannot be specified multiple times", "OLD"),
							parser_errposition(pstate, option->location));
				qry->returningOldAlias = option->value;
				break;

			case RETURNING_OPTION_NEW:
				if (qry->returningNewAlias != NULL)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
					/* translator: %s is OLD or NEW */
							errmsg("%s cannot be specified multiple times", "NEW"),
							parser_errposition(pstate, option->location));
				qry->returningNewAlias = option->value;
				break;

			default:
				elog(ERROR, "unrecognized returning option: %d", option->option);
		}

		if (refnameNamespaceItem(pstate, NULL, option->value, -1, NULL) != NULL)
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_ALIAS),
					errmsg("table name \"%s\" specified more than once",
						   option->value),
					parser_errposition(pstate, option->location));

		addNSItemForReturning(pstate, option->value,
							  option->option == RETURNING_OPTION_OLD ?
							  VAR_RETURNING_OLD : VAR_RETURNING_NEW);
	}

	/*
	 * If OLD/NEW alias names weren't explicitly specified, use "old"/"new"
	 * unless masked by existing relations.
	 */
	/*
	 * 如果未显式指定 OLD/NEW 别名，则使用 "old"/"new"，除非被现有关系掩盖。
	 */
	if (qry->returningOldAlias == NULL &&
		refnameNamespaceItem(pstate, NULL, "old", -1, NULL) == NULL)
	{
		qry->returningOldAlias = "old";
		addNSItemForReturning(pstate, "old", VAR_RETURNING_OLD);
	}
	if (qry->returningNewAlias == NULL &&
		refnameNamespaceItem(pstate, NULL, "new", -1, NULL) == NULL)
	{
		qry->returningNewAlias = "new";
		addNSItemForReturning(pstate, "new", VAR_RETURNING_NEW);
	}

	/*
	 * We need to assign resnos starting at one in the RETURNING list. Save
	 * and restore the main tlist's value of p_next_resno, just in case
	 * someone looks at it later (probably won't happen).
	 */
	/*
	 * 我们需要从 1 开始在 RETURNING 列表中分配 resno。保存并恢复主目标列表
	 * 的 p_next_resno 值，以防万一稍后有人查看它（可能不会发生）。
	 */
	save_next_resno = pstate->p_next_resno;
	pstate->p_next_resno = 1;

	/* transform RETURNING expressions identically to a SELECT targetlist */
	/* 转换 RETURNING 表达式，方式与 SELECT 目标列表相同 */
	qry->returningList = transformTargetList(pstate,
											 returningClause->exprs,
											 exprKind);

	/*
	 * Complain if the nonempty tlist expanded to nothing (which is possible
	 * if it contains only a star-expansion of a zero-column table).  If we
	 * allow this, the parsed Query will look like it didn't have RETURNING,
	 * with results that would probably surprise the user.
	 */
	/*
	 * 如果非空目标列表扩展为空（如果它仅包含零列关系的星号扩展，这是可能的），
	 * 则报错。如果我们允许这种情况，解析后的 Query 将看起来没有 RETURNING，
	 * 其结果可能会让用户感到惊讶。
	 */
	if (qry->returningList == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("RETURNING must have at least one column"),
				 parser_errposition(pstate,
									exprLocation(linitial(returningClause->exprs)))));

	/* mark column origins */
	/* 标记列来源 */
	markTargetListOrigins(pstate, qry->returningList);

	/* resolve any still-unresolved output columns as being type text */
	if (pstate->p_resolve_unknowns)
		resolveTargetListUnknowns(pstate, qry->returningList);

	/* restore state */
	/* 恢复状态 */
	pstate->p_namespace = list_truncate(pstate->p_namespace, save_nslen);
	pstate->p_next_resno = save_next_resno;
}


/*
 * transformPLAssignStmt -
 *	  transform a PL/pgSQL assignment statement
 *
 * If there is no opt_indirection, the transformed statement looks like
 * "SELECT a_expr ...", except the expression has been cast to the type of
 * the target.  With indirection, it's still a SELECT, but the expression will
 * incorporate FieldStore and/or assignment SubscriptingRef nodes to compute a
 * new value for a container-type variable represented by the target.  The
 * expression references the target as the container source.
 */
/*
 * transformPLAssignStmt -
 *	  转换 PL/pgSQL 赋值语句
 *
 * 如果没有 opt_indirection（可选间接引用），转换后的语句看起来像 "SELECT a_expr ..."，
 * 除非表达式已被强制转换为目标的类型。对于间接引用，它仍然是一个 SELECT，
 * 但表达式将包含 FieldStore 和/或赋值 SubscriptingRef 节点，以计算目标表示的
 * 容器类型变量的新值。该表达式将目标作为容器源引用。
 */
static Query *
transformPLAssignStmt(ParseState *pstate, PLAssignStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ColumnRef  *cref = makeNode(ColumnRef);
	List	   *indirection = stmt->indirection;
	int			nnames = stmt->nnames;
	SelectStmt *sstmt = stmt->val;
	Node	   *target;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollation;
	List	   *tlist;
	TargetEntry *tle;
	Oid			type_id;
	Node	   *qual;
	ListCell   *l;

	/*
	 * First, construct a ColumnRef for the target variable.  If the target
	 * has more than one dotted name, we have to pull the extra names out of
	 * the indirection list.
	 */
	/*
	 * 首先，为目标变量构造一个 ColumnRef。如果目标具有多个带点号的名称，
	 * 我们必须从间接引用列表中提取额外的名称。
	 */
	cref->fields = list_make1(makeString(stmt->name));
	cref->location = stmt->location;
	if (nnames > 1)
	{
		/* avoid munging the raw parsetree */
		indirection = list_copy(indirection);
		while (--nnames > 0 && indirection != NIL)
		{
			Node	   *ind = (Node *) linitial(indirection);

			if (!IsA(ind, String))
				elog(ERROR, "invalid name count in PLAssignStmt");
			cref->fields = lappend(cref->fields, ind);
			indirection = list_delete_first(indirection);
		}
	}

	/*
	 * Transform the target reference.  Typically we will get back a Param
	 * node, but there's no reason to be too picky about its type.
	 */
	/*
	 * 转换目标引用。通常我们会得到一个 Param 节点，但没有理由对其类型过于挑剔。
	 */
	target = transformExpr(pstate, (Node *) cref,
						   EXPR_KIND_UPDATE_TARGET);
	targettype = exprType(target);
	targettypmod = exprTypmod(target);
	targetcollation = exprCollation(target);

	/*
	 * The rest mostly matches transformSelectStmt, except that we needn't
	 * consider WITH or INTO, and we build a targetlist our own way.
	 */
	/*
	 * 其余部分大部分与 transformSelectStmt 匹配，不同之处在于我们不需要考虑 WITH 或 INTO，
	 * 并且我们以自己的方式构建目标列表。
	 */
	qry->commandType = CMD_SELECT;
	pstate->p_is_insert = false;

	/* make FOR UPDATE/FOR SHARE info available to addRangeTableEntry */
	/* 使 FOR UPDATE/FOR SHARE 信息可用于 addRangeTableEntry */
	pstate->p_locking_clause = sstmt->lockingClause;

	/* make WINDOW info available for window functions, too */
	/* 也使 WINDOW 信息可用于窗口函数 */
	pstate->p_windowdefs = sstmt->windowClause;

	/* process the FROM clause */
	/* 处理 FROM 子句 */
	transformFromClause(pstate, sstmt->fromClause);

	/* initially transform the targetlist as if in SELECT */
	/* 最初按 SELECT 中的方式转换目标列表 */
	tlist = transformTargetList(pstate, sstmt->targetList,
								EXPR_KIND_SELECT_TARGET);

	/* we should have exactly one targetlist item */
	/* 我们应该正好有一个目标列表项目 */
	if (list_length(tlist) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg_plural("assignment source returned %d column",
							   "assignment source returned %d columns",
							   list_length(tlist),
							   list_length(tlist))));

	tle = linitial_node(TargetEntry, tlist);

	/*
	 * This next bit is similar to transformAssignedExpr; the key difference
	 * is we use COERCION_PLPGSQL not COERCION_ASSIGNMENT.
	 */
	/*
	 * 下一部分类似于 transformAssignedExpr；关键区别在于我们使用
	 * COERCION_PLPGSQL 而不是 COERCION_ASSIGNMENT。
	 */
	type_id = exprType((Node *) tle->expr);

	pstate->p_expr_kind = EXPR_KIND_UPDATE_TARGET;

	if (indirection)
	{
		tle->expr = (Expr *)
			transformAssignmentIndirection(pstate,
										   target,
										   stmt->name,
										   false,
										   targettype,
										   targettypmod,
										   targetcollation,
										   indirection,
										   list_head(indirection),
										   (Node *) tle->expr,
										   COERCION_PLPGSQL,
										   exprLocation(target));
	}
	else if (targettype != type_id &&
			 (targettype == RECORDOID || ISCOMPLEX(targettype)) &&
			 (type_id == RECORDOID || ISCOMPLEX(type_id)))
	{
		/*
		 * Hack: do not let coerce_to_target_type() deal with inconsistent
		 * composite types.  Just pass the expression result through as-is,
		 * and let the PL/pgSQL executor do the conversion its way.  This is
		 * rather bogus, but it's needed for backwards compatibility.
		 */
		/*
		 * 技巧：不要让 coerce_to_target_type() 处理不一致的复合类型。
		 * 只需完整地传递表达式结果，让 PL/pgSQL 执行器以其自己的方式进行转换。
		 * 这相当虚假，但为了向后兼容是必要的。
		 */
	}
	else
	{
		/*
		 * For normal non-qualified target column, do type checking and
		 * coercion.
		 */
		/*
		 * 对于正常的非限定目标列，执行类型检查和强制转换。
		 */
		Node	   *orig_expr = (Node *) tle->expr;

		tle->expr = (Expr *)
			coerce_to_target_type(pstate,
								  orig_expr, type_id,
								  targettype, targettypmod,
								  COERCION_PLPGSQL,
								  COERCE_IMPLICIT_CAST,
								  -1);
		/* With COERCION_PLPGSQL, this error is probably unreachable */
		/* 使用 COERCION_PLPGSQL，此错误可能是无法到达的 */
		if (tle->expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("variable \"%s\" is of type %s"
							" but expression is of type %s",
							stmt->name,
							format_type_be(targettype),
							format_type_be(type_id)),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(orig_expr))));
	}

	pstate->p_expr_kind = EXPR_KIND_NONE;

	qry->targetList = list_make1(tle);

	/* transform WHERE */
	qual = transformWhereClause(pstate, sstmt->whereClause,
								EXPR_KIND_WHERE, "WHERE");

	/* initial processing of HAVING clause is much like WHERE clause */
	qry->havingQual = transformWhereClause(pstate, sstmt->havingClause,
										   EXPR_KIND_HAVING, "HAVING");

	/*
	 * Transform sorting/grouping stuff.  Do ORDER BY first because both
	 * transformGroupClause and transformDistinctClause need the results. Note
	 * that these functions can also change the targetList, so it's passed to
	 * them by reference.
	 */
	qry->sortClause = transformSortClause(pstate,
										  sstmt->sortClause,
										  &qry->targetList,
										  EXPR_KIND_ORDER_BY,
										  false /* allow SQL92 rules */ );

	qry->groupClause = transformGroupClause(pstate,
											sstmt->groupClause,
											&qry->groupingSets,
											&qry->targetList,
											qry->sortClause,
											EXPR_KIND_GROUP_BY,
											false /* allow SQL92 rules */ );
	qry->groupDistinct = sstmt->groupDistinct;

	if (sstmt->distinctClause == NIL)
	{
		qry->distinctClause = NIL;
		qry->hasDistinctOn = false;
	}
	else if (linitial(sstmt->distinctClause) == NULL)
	{
		/* We had SELECT DISTINCT */
		qry->distinctClause = transformDistinctClause(pstate,
													  &qry->targetList,
													  qry->sortClause,
													  false);
		qry->hasDistinctOn = false;
	}
	else
	{
		/* We had SELECT DISTINCT ON */
		qry->distinctClause = transformDistinctOnClause(pstate,
														sstmt->distinctClause,
														&qry->targetList,
														qry->sortClause);
		qry->hasDistinctOn = true;
	}

	/* transform LIMIT */
	qry->limitOffset = transformLimitClause(pstate, sstmt->limitOffset,
											EXPR_KIND_OFFSET, "OFFSET",
											sstmt->limitOption);
	qry->limitCount = transformLimitClause(pstate, sstmt->limitCount,
										   EXPR_KIND_LIMIT, "LIMIT",
										   sstmt->limitOption);
	qry->limitOption = sstmt->limitOption;

	/* transform window clauses after we have seen all window functions */
	qry->windowClause = transformWindowDefinitions(pstate,
												   pstate->p_windowdefs,
												   &qry->targetList);

	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;
	qry->jointree = makeFromExpr(pstate->p_joinlist, qual);

	qry->hasSubLinks = pstate->p_hasSubLinks;
	qry->hasWindowFuncs = pstate->p_hasWindowFuncs;
	qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
	qry->hasAggs = pstate->p_hasAggs;

	foreach(l, sstmt->lockingClause)
	{
		transformLockingClause(pstate, qry,
							   (LockingClause *) lfirst(l), false);
	}

	assign_query_collations(pstate, qry);

	/* this must be done after collations, for reliable comparison of exprs */
	if (pstate->p_hasAggs || qry->groupClause || qry->groupingSets || qry->havingQual)
		parseCheckAggregates(pstate, qry);

	return qry;
}


/*
 * transformDeclareCursorStmt -
 *	transform a DECLARE CURSOR Statement
 *
 * DECLARE CURSOR is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
/*
 * transformDeclareCursorStmt -
 *	转换 DECLARE CURSOR 语句
 *
 * DECLARE CURSOR 与其他实用程序语句相似，我们将它作为一个 CMD_UTILITY Query 节点发出；
 * 但是，我们必须首先转换包含的查询。我们过去常将其推迟到执行时，
 * 但在正常的解析分析阶段执行它确实是必要的，以确保解析器钩子的副作用在预期时间发生。
 */
static Query *
transformDeclareCursorStmt(ParseState *pstate, DeclareCursorStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	if ((stmt->options & CURSOR_OPT_SCROLL) &&
		(stmt->options & CURSOR_OPT_NO_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is a SQL keyword */
				 errmsg("cannot specify both %s and %s",
						"SCROLL", "NO SCROLL")));

	if ((stmt->options & CURSOR_OPT_ASENSITIVE) &&
		(stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is a SQL keyword */
				 errmsg("cannot specify both %s and %s",
						"ASENSITIVE", "INSENSITIVE")));

	/* Transform contained query, not allowing SELECT INTO */
	/* 转换包含的查询，不允许 SELECT INTO */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* Grammar should not have allowed anything but SELECT */
	/* 语法不应允许除 SELECT 以外的任何内容 */
	if (!IsA(query, Query) ||
		query->commandType != CMD_SELECT)
		elog(ERROR, "unexpected non-SELECT command in DECLARE CURSOR");

	/*
	 * We also disallow data-modifying WITH in a cursor.  (This could be
	 * allowed, but the semantics of when the updates occur might be
	 * surprising.)
	 */
	/*
	 * 我们还禁止在游标中使用修改数据的 WITH 子句。（这可以被允许，
	 * 但更新发生的语义可能会令人惊讶。）
	 */
	if (query->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DECLARE CURSOR must not contain data-modifying statements in WITH")));

	/* FOR UPDATE and WITH HOLD are not compatible */
	/* FOR UPDATE 和 WITH HOLD 不兼容 */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_HOLD))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE CURSOR WITH HOLD ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Holdable cursors must be READ ONLY.")));

	/* FOR UPDATE and SCROLL are not compatible */
	/* FOR UPDATE 和 SCROLL 不兼容 */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_SCROLL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE SCROLL CURSOR ... %s is not supported",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Scrollable cursors must be READ ONLY.")));

	/* FOR UPDATE and INSENSITIVE are not compatible */
	/* FOR UPDATE 和 INSENSITIVE 不兼容 */
	if (query->rowMarks != NIL && (stmt->options & CURSOR_OPT_INSENSITIVE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("DECLARE INSENSITIVE CURSOR ... %s is not valid",
						LCS_asString(((RowMarkClause *)
									  linitial(query->rowMarks))->strength)),
				 errdetail("Insensitive cursors must be READ ONLY.")));

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformExplainStmt -
 *	transform an EXPLAIN Statement
 *
 * EXPLAIN is like other utility statements in that we emit it as a
 * CMD_UTILITY Query node; however, we must first transform the contained
 * query.  We used to postpone that until execution, but it's really necessary
 * to do it during the normal parse analysis phase to ensure that side effects
 * of parser hooks happen at the expected time.
 */
/*
 * transformExplainStmt -
 *	转换 EXPLAIN 语句
 *
 * EXPLAIN 与其他实用程序语句类似，我们将它作为一个 CMD_UTILITY Query 节点发出；
 * 但是，我们必须首先转换包含的查询。我们过去常将其推迟到执行时，
 * 但在正常的解析分析阶段执行它确实是必要的，以确保解析器钩子的副作用在预期时间发生。
 */
static Query *
transformExplainStmt(ParseState *pstate, ExplainStmt *stmt)
{
	Query	   *result;
	bool		generic_plan = false;
	Oid		   *paramTypes = NULL;
	int			numParams = 0;

	/*
	 * If we have no external source of parameter definitions, and the
	 * GENERIC_PLAN option is specified, then accept variable parameter
	 * definitions (similarly to PREPARE, for example).
	 */
	/*
	 * 如果我们没有参数定义的外部源，并且指定了 GENERIC_PLAN 选项，
	 * 那么接受可变参数定义（例如，类似于 PREPARE）。
	 */
	if (pstate->p_paramref_hook == NULL)
	{
		ListCell   *lc;

		foreach(lc, stmt->options)
		{
			DefElem    *opt = (DefElem *) lfirst(lc);

			if (strcmp(opt->defname, "generic_plan") == 0)
				generic_plan = defGetBoolean(opt);
			/* don't "break", as we want the last value */
		}
		if (generic_plan)
			setup_parse_variable_parameters(pstate, &paramTypes, &numParams);
	}

	/* transform contained query, allowing SELECT INTO */
	/* 转换包含的查询，允许 SELECT INTO */
	stmt->query = (Node *) transformOptionalSelectInto(pstate, stmt->query);

	/* make sure all is well with parameter types */
	/* 确保参数类型一切正常 */
	if (generic_plan)
		check_variable_parameters(pstate, (Query *) stmt->query);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}


/*
 * transformCreateTableAsStmt -
 *	transform a CREATE TABLE AS, SELECT ... INTO, or CREATE MATERIALIZED VIEW
 *	Statement
 *
 * As with DECLARE CURSOR and EXPLAIN, transform the contained statement now.
 */
/*
 * transformCreateTableAsStmt -
 *	转换 CREATE TABLE AS、SELECT ... INTO 或 CREATE MATERIALIZED VIEW 语句
 *
 * 与 DECLARE CURSOR 和 EXPLAIN 一样，现在转换包含的语句。
 */
/*
 * transformCreateTableAsStmt -
 *	转换 CREATE TABLE AS、SELECT ... INTO 或 CREATE MATERIALIZED VIEW 语句
 *
 * 与 DECLARE CURSOR 和 EXPLAIN 一样，现在转换包含的语句。
 */
static Query *
transformCreateTableAsStmt(ParseState *pstate, CreateTableAsStmt *stmt)
{
	Query	   *result;
	Query	   *query;

	/* transform contained query, not allowing SELECT INTO */
	/* 转换包含的查询，不允许 SELECT INTO */
	query = transformStmt(pstate, stmt->query);
	stmt->query = (Node *) query;

	/* additional work needed for CREATE MATERIALIZED VIEW */
	/* CREATE MATERIALIZED VIEW 需要的额外工作 */
	if (stmt->objtype == OBJECT_MATVIEW)
	{
		/*
		 * Prohibit a data-modifying CTE in the query used to create a
		 * materialized view. It's not sufficiently clear what the user would
		 * want to happen if the MV is refreshed or incrementally maintained.
		 */
		if (query->hasModifyingCTE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use data-modifying statements in WITH")));

		/*
		 * Check whether any temporary database objects are used in the
		 * creation query. It would be hard to refresh data or incrementally
		 * maintain it if a source disappeared.
		 */
		if (isQueryUsingTempRelation(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views must not use temporary tables or views")));

		/*
		 * A materialized view would either need to save parameters for use in
		 * maintaining/loading the data or prohibit them entirely.  The latter
		 * seems safer and more sane.
		 */
		if (query_contains_extern_params(query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views may not be defined using bound parameters")));

		/*
		 * For now, we disallow unlogged materialized views, because it seems
		 * like a bad idea for them to just go to empty after a crash. (If we
		 * could mark them as unpopulated, that would be better, but that
		 * requires catalog changes which crash recovery can't presently
		 * handle.)
		 */
		if (stmt->into->rel->relpersistence == RELPERSISTENCE_UNLOGGED)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialized views cannot be unlogged")));

		/*
		 * At runtime, we'll need a copy of the parsed-but-not-rewritten Query
		 * for purposes of creating the view's ON SELECT rule.  We stash that
		 * in the IntoClause because that's where intorel_startup() can
		 * conveniently get it from.
		 */
		stmt->into->viewQuery = copyObject(query);
	}

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * transform a CallStmt
 */
static Query *
transformCallStmt(ParseState *pstate, CallStmt *stmt)
{
	List	   *targs;
	ListCell   *lc;
	Node	   *node;
	FuncExpr   *fexpr;
	HeapTuple	proctup;
	Datum		proargmodes;
	bool		isNull;
	List	   *outargs = NIL;
	Query	   *result;

	/*
	 * First, do standard parse analysis on the procedure call and its
	 * arguments, allowing us to identify the called procedure.
	 */
	targs = NIL;
	foreach(lc, stmt->funccall->args)
	{
		targs = lappend(targs, transformExpr(pstate,
											 (Node *) lfirst(lc),
											 EXPR_KIND_CALL_ARGUMENT));
	}

	node = ParseFuncOrColumn(pstate,
							 stmt->funccall->funcname,
							 targs,
							 pstate->p_last_srf,
							 stmt->funccall,
							 true,
							 stmt->funccall->location);

	assign_expr_collations(pstate, node);

	fexpr = castNode(FuncExpr, node);

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", fexpr->funcid);

	/*
	 * Expand the argument list to deal with named-argument notation and
	 * default arguments.  For ordinary FuncExprs this'd be done during
	 * planning, but a CallStmt doesn't go through planning, and there seems
	 * no good reason not to do it here.
	 */
	fexpr->args = expand_function_arguments(fexpr->args,
											true,
											fexpr->funcresulttype,
											proctup);

	/* Fetch proargmodes; if it's null, there are no output args */
	proargmodes = SysCacheGetAttr(PROCOID, proctup,
								  Anum_pg_proc_proargmodes,
								  &isNull);
	if (!isNull)
	{
		/*
		 * Split the list into input arguments in fexpr->args and output
		 * arguments in stmt->outargs.  INOUT arguments appear in both lists.
		 */
		ArrayType  *arr;
		int			numargs;
		char	   *argmodes;
		List	   *inargs;
		int			i;

		arr = DatumGetArrayTypeP(proargmodes);	/* ensure not toasted */
		numargs = list_length(fexpr->args);
		if (ARR_NDIM(arr) != 1 ||
			ARR_DIMS(arr)[0] != numargs ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "proargmodes is not a 1-D char array of length %d or it contains nulls",
				 numargs);
		argmodes = (char *) ARR_DATA_PTR(arr);

		inargs = NIL;
		i = 0;
		foreach(lc, fexpr->args)
		{
			Node	   *n = lfirst(lc);

			switch (argmodes[i])
			{
				case PROARGMODE_IN:
				case PROARGMODE_VARIADIC:
					inargs = lappend(inargs, n);
					break;
				case PROARGMODE_OUT:
					outargs = lappend(outargs, n);
					break;
				case PROARGMODE_INOUT:
					inargs = lappend(inargs, n);
					outargs = lappend(outargs, copyObject(n));
					break;
				default:
					/* note we don't support PROARGMODE_TABLE */
					elog(ERROR, "invalid argmode %c for procedure",
						 argmodes[i]);
					break;
			}
			i++;
		}
		fexpr->args = inargs;
	}

	stmt->funcexpr = fexpr;
	stmt->outargs = outargs;

	ReleaseSysCache(proctup);

	/* represent the command as a utility Query */
	result = makeNode(Query);
	result->commandType = CMD_UTILITY;
	result->utilityStmt = (Node *) stmt;

	return result;
}

/*
 * Produce a string representation of a LockClauseStrength value.
 * This should only be applied to valid values (not LCS_NONE).
 */
const char *
LCS_asString(LockClauseStrength strength)
{
	switch (strength)
	{
		case LCS_NONE:
			Assert(false);
			break;
		case LCS_FORKEYSHARE:
			return "FOR KEY SHARE";
		case LCS_FORSHARE:
			return "FOR SHARE";
		case LCS_FORNOKEYUPDATE:
			return "FOR NO KEY UPDATE";
		case LCS_FORUPDATE:
			return "FOR UPDATE";
	}
	return "FOR some";			/* shouldn't happen */
}

/*
 * Check for features that are not supported with FOR [KEY] UPDATE/SHARE.
 *
 * exported so planner can check again after rewriting, query pullup, etc
 */
void
CheckSelectLocking(Query *qry, LockClauseStrength strength)
{
	Assert(strength != LCS_NONE);	/* else caller error */

	if (qry->setOperations)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with UNION/INTERSECT/EXCEPT",
						LCS_asString(strength))));
	if (qry->distinctClause != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with DISTINCT clause",
						LCS_asString(strength))));
	if (qry->groupClause != NIL || qry->groupingSets != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with GROUP BY clause",
						LCS_asString(strength))));
	if (qry->havingQual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with HAVING clause",
						LCS_asString(strength))));
	if (qry->hasAggs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with aggregate functions",
						LCS_asString(strength))));
	if (qry->hasWindowFuncs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with window functions",
						LCS_asString(strength))));
	if (qry->hasTargetSRFs)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*------
		  translator: %s is a SQL row locking clause such as FOR UPDATE */
				 errmsg("%s is not allowed with set-returning functions in the target list",
						LCS_asString(strength))));
}

/*
 * Transform a FOR [KEY] UPDATE/SHARE clause
 *
 * This basically involves replacing names by integer relids.
 *
 * NB: if you need to change this, see also markQueryForLocking()
 * in rewriteHandler.c, and isLockedRefname() in parse_relation.c.
 */
static void
transformLockingClause(ParseState *pstate, Query *qry, LockingClause *lc,
					   bool pushedDown)
{
	List	   *lockedRels = lc->lockedRels;
	ListCell   *l;
	ListCell   *rt;
	Index		i;
	LockingClause *allrels;

	CheckSelectLocking(qry, lc->strength);

	/* make a clause we can pass down to subqueries to select all rels */
	allrels = makeNode(LockingClause);
	allrels->lockedRels = NIL;	/* indicates all rels */
	allrels->strength = lc->strength;
	allrels->waitPolicy = lc->waitPolicy;

	if (lockedRels == NIL)
	{
		/*
		 * Lock all regular tables used in query and its subqueries.  We
		 * examine inFromCl to exclude auto-added RTEs, particularly NEW/OLD
		 * in rules.  This is a bit of an abuse of a mostly-obsolete flag, but
		 * it's convenient.  We can't rely on the namespace mechanism that has
		 * largely replaced inFromCl, since for example we need to lock
		 * base-relation RTEs even if they are masked by upper joins.
		 */
		i = 0;
		foreach(rt, qry->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

			++i;
			if (!rte->inFromCl)
				continue;
			switch (rte->rtekind)
			{
				case RTE_RELATION:
					{
						RTEPermissionInfo *perminfo;

						applyLockingClause(qry, i,
										   lc->strength,
										   lc->waitPolicy,
										   pushedDown);
						perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
						perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
					}
					break;
				case RTE_SUBQUERY:
					applyLockingClause(qry, i, lc->strength, lc->waitPolicy,
									   pushedDown);

					/*
					 * FOR UPDATE/SHARE of subquery is propagated to all of
					 * subquery's rels, too.  We could do this later (based on
					 * the marking of the subquery RTE) but it is convenient
					 * to have local knowledge in each query level about which
					 * rels need to be opened with RowShareLock.
					 */
					transformLockingClause(pstate, rte->subquery,
										   allrels, true);
					break;
				default:
					/* ignore JOIN, SPECIAL, FUNCTION, VALUES, CTE RTEs */
					break;
			}
		}
	}
	else
	{
		/*
		 * Lock just the named tables.  As above, we allow locking any base
		 * relation regardless of alias-visibility rules, so we need to
		 * examine inFromCl to exclude OLD/NEW.
		 */
		foreach(l, lockedRels)
		{
			RangeVar   *thisrel = (RangeVar *) lfirst(l);

			/* For simplicity we insist on unqualified alias names here */
			if (thisrel->catalogname || thisrel->schemaname)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("%s must specify unqualified relation names",
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));

			i = 0;
			foreach(rt, qry->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);
				char	   *rtename = rte->eref->aliasname;

				++i;
				if (!rte->inFromCl)
					continue;

				/*
				 * A join RTE without an alias is not visible as a relation
				 * name and needs to be skipped (otherwise it might hide a
				 * base relation with the same name), except if it has a USING
				 * alias, which *is* visible.
				 *
				 * Subquery and values RTEs without aliases are never visible
				 * as relation names and must always be skipped.
				 */
				if (rte->alias == NULL)
				{
					if (rte->rtekind == RTE_JOIN)
					{
						if (rte->join_using_alias == NULL)
							continue;
						rtename = rte->join_using_alias->aliasname;
					}
					else if (rte->rtekind == RTE_SUBQUERY ||
							 rte->rtekind == RTE_VALUES)
						continue;
				}

				if (strcmp(rtename, thisrel->relname) == 0)
				{
					switch (rte->rtekind)
					{
						case RTE_RELATION:
							{
								RTEPermissionInfo *perminfo;

								applyLockingClause(qry, i,
												   lc->strength,
												   lc->waitPolicy,
												   pushedDown);
								perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
								perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
							}
							break;
						case RTE_SUBQUERY:
							applyLockingClause(qry, i, lc->strength,
											   lc->waitPolicy, pushedDown);
							/* see comment above */
							transformLockingClause(pstate, rte->subquery,
												   allrels, true);
							break;
						case RTE_JOIN:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a join",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_FUNCTION:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_TABLEFUNC:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a table function",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_VALUES:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to VALUES",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_CTE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a WITH query",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;
						case RTE_NAMEDTUPLESTORE:
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							/*------
							  translator: %s is a SQL row locking clause such as FOR UPDATE */
									 errmsg("%s cannot be applied to a named tuplestore",
											LCS_asString(lc->strength)),
									 parser_errposition(pstate, thisrel->location)));
							break;

							/* Shouldn't be possible to see RTE_RESULT here */

						default:
							elog(ERROR, "unrecognized RTE type: %d",
								 (int) rte->rtekind);
							break;
					}
					break;		/* out of foreach loop */
				}
			}
			if (rt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
				/*------
				  translator: %s is a SQL row locking clause such as FOR UPDATE */
						 errmsg("relation \"%s\" in %s clause not found in FROM clause",
								thisrel->relname,
								LCS_asString(lc->strength)),
						 parser_errposition(pstate, thisrel->location)));
		}
	}
}

/*
 * Record locking info for a single rangetable item
 */
void
applyLockingClause(Query *qry, Index rtindex,
				   LockClauseStrength strength, LockWaitPolicy waitPolicy,
				   bool pushedDown)
{
	RowMarkClause *rc;

	Assert(strength != LCS_NONE);	/* else caller error */

	/* If it's an explicit clause, make sure hasForUpdate gets set */
	if (!pushedDown)
		qry->hasForUpdate = true;

	/* Check for pre-existing entry for same rtindex */
	if ((rc = get_parse_rowmark(qry, rtindex)) != NULL)
	{
		/*
		 * If the same RTE is specified with more than one locking strength,
		 * use the strongest.  (Reasonable, since you can't take both a shared
		 * and exclusive lock at the same time; it'll end up being exclusive
		 * anyway.)
		 *
		 * Similarly, if the same RTE is specified with more than one lock
		 * wait policy, consider that NOWAIT wins over SKIP LOCKED, which in
		 * turn wins over waiting for the lock (the default).  This is a bit
		 * more debatable but raising an error doesn't seem helpful. (Consider
		 * for instance SELECT FOR UPDATE NOWAIT from a view that internally
		 * contains a plain FOR UPDATE spec.)  Having NOWAIT win over SKIP
		 * LOCKED is reasonable since the former throws an error in case of
		 * coming across a locked tuple, which may be undesirable in some
		 * cases but it seems better than silently returning inconsistent
		 * results.
		 *
		 * And of course pushedDown becomes false if any clause is explicit.
		 */
		rc->strength = Max(rc->strength, strength);
		rc->waitPolicy = Max(rc->waitPolicy, waitPolicy);
		rc->pushedDown &= pushedDown;
		return;
	}

	/* Make a new RowMarkClause */
	rc = makeNode(RowMarkClause);
	rc->rti = rtindex;
	rc->strength = strength;
	rc->waitPolicy = waitPolicy;
	rc->pushedDown = pushedDown;
	qry->rowMarks = lappend(qry->rowMarks, rc);
}

#ifdef DEBUG_NODE_TESTS_ENABLED
/*
 * Coverage testing for raw_expression_tree_walker().
 *
 * When enabled, we run raw_expression_tree_walker() over every DML statement
 * submitted to parse analysis.  Without this provision, that function is only
 * applied in limited cases involving CTEs, and we don't really want to have
 * to test everything inside as well as outside a CTE.
 */
static bool
test_raw_expression_coverage(Node *node, void *context)
{
	if (node == NULL)
		return false;
	return raw_expression_tree_walker(node,
									  test_raw_expression_coverage,
									  context);
}
#endif							/* DEBUG_NODE_TESTS_ENABLED */
