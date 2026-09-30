/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * 通过 PREPARE、EXECUTE 和 DEALLOCATE 使用可预备的 SQL 语句。
 *
 * This module also implements storage of prepared statements that are
 * accessed via the extended FE/BE query protocol.
 *
 * 本模块也实现经扩展前端/后端查询协议访问的预备语句存储。
 *
 *
 * Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/prepare.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "commands/prepare.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"


/*
 * 核心流程概览：
 * PrepareQuery 分析语句、按参数类型重写，并把 CachedPlanSource 存入每后端哈希表。
 * ExecuteQuery 查找预备语句、求值参数，在 portal 中运行；也支持 CREATE TABLE AS EXECUTE。
 * DeallocateQuery / DropPreparedStatement 删除一项或全部缓存语句。
 * ExplainExecuteQuery 对预备语句做 EXPLAIN。
 */
/*
 * The hash table in which prepared queries are stored. This is
 * per-backend: query plans are not shared between backends.
 * The keys for this hash table are the arguments to PREPARE and EXECUTE
 * (statement names); the entries are PreparedStatement structs.
 *
 * 存放预备查询的哈希表。它属于单个后端，查询计划不在后端之间共享。
 * 键是 PREPARE 和 EXECUTE 的语句名，项是 PreparedStatement 结构。
 */
static HTAB *prepared_queries = NULL;

static void InitQueryHashTable(void);
static ParamListInfo EvaluateParams(ParseState *pstate,
									PreparedStatement *pstmt, List *params,
									EState *estate);
static Datum build_regtype_array(Oid *param_types, int num_params);

/*
 * Implements the 'PREPARE' utility statement.
 *
 * 实现 PREPARE 实用语句。
 */
void
PrepareQuery(ParseState *pstate, PrepareStmt *stmt,
			 int stmt_location, int stmt_len)
{
	RawStmt    *rawstmt;
	CachedPlanSource *plansource;
	Oid		   *argtypes = NULL;
	int			nargs;
	List	   *query_list;

	/*
	 * Disallow empty-string statement name (conflicts with protocol-level
	 * unnamed statement).
	 *
	 * 不允许空字符串语句名，它会与协议层的未命名语句冲突。
	 */
	if (!stmt->name || stmt->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
				 errmsg("invalid statement name: must not be empty")));

	/*
	 * Need to wrap the contained statement in a RawStmt node to pass it to
	 * parse analysis.
	 *
	 * 需要把所含语句包进 RawStmt 节点再做语法分析。
	 */
	rawstmt = makeNode(RawStmt);
	rawstmt->stmt = stmt->query;
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	/*
	 * Create the CachedPlanSource before we do parse analysis, since it needs
	 * to see the unmodified raw parse tree.
	 *
	 * 在语法分析之前创建 CachedPlanSource，因为它需要看到未被修改的原始分析树。
	 */
	plansource = CreateCachedPlan(rawstmt, pstate->p_sourcetext,
								  CreateCommandTag(stmt->query));

	/* Transform list of TypeNames to array of type OIDs */
	/*
	 *
	 * 把 TypeName 列表转换成类型 OID 数组。
	 */
	nargs = list_length(stmt->argtypes);

	if (nargs)
	{
		int			i;
		ListCell   *l;

		argtypes = palloc_array(Oid, nargs);
		i = 0;

		foreach(l, stmt->argtypes)
		{
			TypeName   *tn = lfirst(l);
			Oid			toid = typenameTypeId(pstate, tn);

			argtypes[i++] = toid;
		}
	}

	/*
	 * Analyze the statement using these parameter types (any parameters
	 * passed in from above us will not be visible to it), allowing
	 * information about unknown parameters to be deduced from context.
	 * Rewrite the query. The result could be 0, 1, or many queries.
	 *
	 * 用这些参数类型分析语句（上层传入的参数对它不可见），并允许从上下文推断未知参数。
	 * 然后重写查询。结果可能是 0 条、1 条或多条查询。
	 */
	query_list = pg_analyze_and_rewrite_varparams(rawstmt, pstate->p_sourcetext,
												  &argtypes, &nargs, NULL);

	/* Finish filling in the CachedPlanSource */
	/*
	 *
	 * 填完 CachedPlanSource。
	 */
	CompleteCachedPlan(plansource,
					   query_list,
					   NULL,
					   argtypes,
					   nargs,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* allow parallel mode */
					   /*
					    *
					    * 允许并行模式。
					    */
					   true);	/* fixed result */
					   /*
					    *
					    * 结果描述固定。
					    */

	/*
	 * Save the results.
	 *
	 * 保存结果。
	 */
	StorePreparedStatement(stmt->name,
						   plansource,
						   true);
}

/*
 * ExecuteQuery --- implement the 'EXECUTE' utility statement.
 *
 * ExecuteQuery：实现 EXECUTE 实用语句。
 *
 * This code also supports CREATE TABLE ... AS EXECUTE.  That case is
 * indicated by passing a non-null intoClause.  The DestReceiver is already
 * set up correctly for CREATE TABLE AS, but we still have to make a few
 * other adjustments here.
 *
 * 这里也支持 CREATE TABLE ... AS EXECUTE，由非空的 intoClause 表示。
 * DestReceiver 已按 CREATE TABLE AS 设好，但这里仍要做几处调整。
 */
void
ExecuteQuery(ParseState *pstate,
			 ExecuteStmt *stmt, IntoClause *intoClause,
			 ParamListInfo params,
			 DestReceiver *dest, QueryCompletion *qc)
{
	PreparedStatement *entry;
	CachedPlan *cplan;
	List	   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	Portal		portal;
	char	   *query_string;
	int			eflags;
	long		count;

	/* Look it up in the hash table */
	/*
	 *
	 * 在哈希表中查找。
	 */
	entry = FetchPreparedStatement(stmt->name, true);

	/* Shouldn't find a non-fixed-result cached plan */
	/*
	 *
	 * 不应找到结果不固定的缓存计划。
	 */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXECUTE does not support variable-result cached plans");

	/* Evaluate parameters, if any */
	/*
	 *
	 * 若有参数则求值。
	 */
	if (entry->plansource->num_params > 0)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.  Note that the
		 * passed-in "params" could possibly be referenced in the parameter
		 * expressions.
		 *
		 * 求值参数需要 EState，并且必须留到查询结束，因为参数可能是传引用的。
		 * 传入的 params 也可能被参数表达式引用。
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;
		paramLI = EvaluateParams(pstate, entry, stmt->params, estate);
	}

	/* Create a new portal to run the query in */
	/*
	 *
	 * 创建一个新 portal 来运行查询。
	 */
	portal = CreateNewPortal();
	/* Don't display the portal in pg_cursors, it is for internal use only */
	/*
	 *
	 * 不要把该 portal 显示在 pg_cursors 中，它只供内部使用。
	 */
	portal->visible = false;

	/* Copy the plan's saved query string into the portal's memory */
	/*
	 *
	 * 把计划保存的查询字符串复制到 portal 的内存中。
	 */
	query_string = MemoryContextStrdup(portal->portalContext,
									   entry->plansource->query_string);

	/* Replan if needed, and increment plan refcount for portal */
	/*
	 *
	 * 必要时重新规划，并为 portal 增加计划引用计数。
	 */
	cplan = GetCachedPlan(entry->plansource, paramLI, NULL, NULL);
	plan_list = cplan->stmt_list;

	/*
	 * DO NOT add any logic that could possibly throw an error between
	 * GetCachedPlan and PortalDefineQuery, or you'll leak the plan refcount.
	 *
	 * 不要在 GetCachedPlan 和 PortalDefineQuery 之间加入可能报错的逻辑，否则会泄漏计划引用计数。
	 */
	PortalDefineQuery(portal,
					  NULL,
					  query_string,
					  entry->plansource->commandTag,
					  plan_list,
					  cplan);

	/*
	 * For CREATE TABLE ... AS EXECUTE, we must verify that the prepared
	 * statement is one that produces tuples.  Currently we insist that it be
	 * a plain old SELECT.  In future we might consider supporting other
	 * things such as INSERT ... RETURNING, but there are a couple of issues
	 * to be settled first, notably how WITH NO DATA should be handled in such
	 * a case (do we really want to suppress execution?) and how to pass down
	 * the OID-determining eflags (PortalStart won't handle them in such a
	 * case, and for that matter it's not clear the executor will either).
	 *
	 * 对 CREATE TABLE ... AS EXECUTE，必须确认预备语句会产生元组。目前只接受普通 SELECT。
	 * 将来也许支持 INSERT ... RETURNING，但要先解决 WITH NO DATA 是否真要抑制执行，
	 * 以及如何传递决定 OID 的 eflags（这种情况下 PortalStart 不会处理，执行器是否处理也不清楚）。
	 *
	 * For CREATE TABLE ... AS EXECUTE, we also have to ensure that the proper
	 * eflags and fetch count are passed to PortalStart/PortalRun.
	 *
	 * 对 CREATE TABLE ... AS EXECUTE，还须把正确的 eflags 和 fetch 计数传给 PortalStart/PortalRun。
	 */
	if (intoClause)
	{
		PlannedStmt *pstmt;

		if (list_length(plan_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		pstmt = linitial_node(PlannedStmt, plan_list);
		if (pstmt->commandType != CMD_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));

		/* Set appropriate eflags */
		/*
		 *
		 * 设置合适的 eflags。
		 */
		eflags = GetIntoRelEFlags(intoClause);

		/* And tell PortalRun whether to run to completion or not */
		/*
		 *
		 * 并告诉 PortalRun 是否运行到结束。
		 */
		if (intoClause->skipData)
			count = 0;
		else
			count = FETCH_ALL;
	}
	else
	{
		/* Plain old EXECUTE */
		/*
		 *
		 * 普通的 EXECUTE。
		 */
		eflags = 0;
		count = FETCH_ALL;
	}

	/*
	 * Run the portal as appropriate.
	 *
	 * 按情况运行 portal。
	 */
	PortalStart(portal, paramLI, eflags, GetActiveSnapshot());

	(void) PortalRun(portal, count, false, dest, dest, qc);

	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	/* No need to pfree other memory, MemoryContext will be reset */
	/*
	 *
	 * 不必 pfree 其它内存，MemoryContext 会被重置。
	 */
}

/*
 * EvaluateParams: evaluate a list of parameters.
 *
 * EvaluateParams：对参数列表求值。
 *
 * pstate: parse state
 * pstmt: statement we are getting parameters for.
 * params: list of given parameter expressions (raw parser output!)
 * estate: executor state to use.
 *
 * pstate：分析状态。
 * pstmt：要取参数的语句。
 * params：给定的参数表达式列表（原始解析器输出）。
 * estate：要使用的执行器状态。
 *
 * Returns a filled-in ParamListInfo -- this can later be passed to
 * CreateQueryDesc(), which allows the executor to make use of the parameters
 * during query execution.
 *
 * 返回填好的 ParamListInfo，稍后可传给 CreateQueryDesc()，让执行器在执行时使用这些参数。
 */
static ParamListInfo
EvaluateParams(ParseState *pstate, PreparedStatement *pstmt, List *params,
			   EState *estate)
{
	Oid		   *param_types = pstmt->plansource->param_types;
	int			num_params = pstmt->plansource->num_params;
	int			nparams = list_length(params);
	ParamListInfo paramLI;
	List	   *exprstates;
	ListCell   *l;
	int			i;

	if (nparams != num_params)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("wrong number of parameters for prepared statement \"%s\"",
						pstmt->stmt_name),
				 errdetail("Expected %d parameters but got %d.",
						   num_params, nparams)));

	/* Quick exit if no parameters */
	/*
	 *
	 * 没有参数则立刻返回。
	 */
	if (num_params == 0)
		return NULL;

	/*
	 * We have to run parse analysis for the expressions.  Since the parser is
	 * not cool about scribbling on its input, copy first.
	 *
	 * 必须对表达式做语法分析。解析器会改写输入，因此先复制。
	 */
	params = copyObject(params);

	i = 0;
	foreach(l, params)
	{
		Node	   *expr = lfirst(l);
		Oid			expected_type_id = param_types[i];
		Oid			given_type_id;

		expr = transformExpr(pstate, expr, EXPR_KIND_EXECUTE_PARAMETER);

		given_type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, given_type_id,
									 expected_type_id, -1,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST,
									 -1);

		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("parameter $%d of type %s cannot be coerced to the expected type %s",
							i + 1,
							format_type_be(given_type_id),
							format_type_be(expected_type_id)),
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(lfirst(l)))));

		/* Take care of collations in the finished expression. */
		/*
		 *
		 * 处理完成表达式中的排序规则。
		 */
		assign_expr_collations(pstate, expr);

		lfirst(l) = expr;
		i++;
	}

	/* Prepare the expressions for execution */
	/*
	 *
	 * 为执行准备这些表达式。
	 */
	exprstates = ExecPrepareExprList(params, estate);

	paramLI = makeParamList(num_params);

	i = 0;
	foreach(l, exprstates)
	{
		ExprState  *n = (ExprState *) lfirst(l);
		ParamExternData *prm = &paramLI->params[i];

		prm->ptype = param_types[i];
		prm->pflags = PARAM_FLAG_CONST;
		prm->value = ExecEvalExprSwitchContext(n,
											   GetPerTupleExprContext(estate),
											   &prm->isnull);

		i++;
	}

	return paramLI;
}


/*
 * Initialize query hash table upon first use.
 *
 * 第一次使用时初始化查询哈希表。
 */
static void
InitQueryHashTable(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(PreparedStatement);

	prepared_queries = hash_create("Prepared Queries",
								   32,
								   &hash_ctl,
								   HASH_ELEM | HASH_STRINGS);
}

/*
 * Store all the data pertaining to a query in the hash table using
 * the specified key.  The passed CachedPlanSource should be "unsaved"
 * in case we get an error here; we'll save it once we've created the hash
 * table entry.
 *
 * 用指定键把查询的全部数据存入哈希表。传入的 CachedPlanSource 应尚未保存，
 * 以免这里出错；哈希表项创建后再保存它。
 */
void
StorePreparedStatement(const char *stmt_name,
					   CachedPlanSource *plansource,
					   bool from_sql)
{
	PreparedStatement *entry;
	TimestampTz cur_ts = GetCurrentStatementStartTimestamp();
	bool		found;

	/* Initialize the hash table, if necessary */
	/*
	 *
	 * 必要时初始化哈希表。
	 */
	if (!prepared_queries)
		InitQueryHashTable();

	/* Add entry to hash table */
	/*
	 *
	 * 向哈希表加入一项。
	 */
	entry = (PreparedStatement *) hash_search(prepared_queries,
											  stmt_name,
											  HASH_ENTER,
											  &found);

	/* Shouldn't get a duplicate entry */
	/*
	 *
	 * 不应得到重复项。
	 */
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_PSTATEMENT),
				 errmsg("prepared statement \"%s\" already exists",
						stmt_name)));

	/* Fill in the hash table entry */
	/*
	 *
	 * 填写哈希表项。
	 */
	entry->plansource = plansource;
	entry->from_sql = from_sql;
	entry->prepare_time = cur_ts;

	/* Now it's safe to move the CachedPlanSource to permanent memory */
	/*
	 *
	 * 现在可以把 CachedPlanSource 移到永久内存。
	 */
	SaveCachedPlan(plansource);
}

/*
 * Lookup an existing query in the hash table. If the query does not
 * actually exist, throw ereport(ERROR) or return NULL per second parameter.
 *
 * 在哈希表中查找已有查询。若不存在，按第二个参数决定 ereport(ERROR) 或返回 NULL。
 *
 * Note: this does not force the referenced plancache entry to be valid,
 * since not all callers care.
 *
 * 这里不强制所引用的计划缓存项有效，因为并非所有调用者都在意。
 */
PreparedStatement *
FetchPreparedStatement(const char *stmt_name, bool throwError)
{
	PreparedStatement *entry;

	/*
	 * If the hash table hasn't been initialized, it can't be storing
	 * anything, therefore it couldn't possibly store our plan.
	 *
	 * 哈希表尚未初始化就不可能存有任何东西，因此也不可能存有我们的计划。
	 */
	if (prepared_queries)
		entry = (PreparedStatement *) hash_search(prepared_queries,
												  stmt_name,
												  HASH_FIND,
												  NULL);
	else
		entry = NULL;

	if (!entry && throwError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("prepared statement \"%s\" does not exist",
						stmt_name)));

	return entry;
}

/*
 * Given a prepared statement, determine the result tupledesc it will
 * produce.  Returns NULL if the execution will not return tuples.
 *
 * 对给定预备语句，确定它将产生的结果 tupledesc。若执行不返回元组则返回 NULL。
 *
 * Note: the result is created or copied into current memory context.
 *
 * 结果在当前内存上下文中创建或复制。
 */
TupleDesc
FetchPreparedStatementResultDesc(PreparedStatement *stmt)
{
	/*
	 * Since we don't allow prepared statements' result tupdescs to change,
	 * there's no need to worry about revalidating the cached plan here.
	 *
	 * 预备语句的结果 tupledesc 不允许改变，因此这里不必重新验证缓存计划。
	 */
	Assert(stmt->plansource->fixed_result);
	if (stmt->plansource->resultDesc)
		return CreateTupleDescCopy(stmt->plansource->resultDesc);
	else
		return NULL;
}

/*
 * Given a prepared statement that returns tuples, extract the query
 * targetlist.  Returns NIL if the statement doesn't have a determinable
 * targetlist.
 *
 * 对返回元组的预备语句，取出查询目标列表。若无法确定目标列表则返回 NIL。
 *
 * Note: this is pretty ugly, but since it's only used in corner cases like
 * Describe Statement on an EXECUTE command, we don't worry too much about
 * efficiency.
 *
 * 这比较难看，但只用于对 EXECUTE 做 Describe Statement 这类边角情况，不必太在意效率。
 */
List *
FetchPreparedStatementTargetList(PreparedStatement *stmt)
{
	List	   *tlist;

	/* Get the plan's primary targetlist */
	/*
	 *
	 * 取得计划的主目标列表。
	 */
	tlist = CachedPlanGetTargetList(stmt->plansource, NULL);

	/* Copy into caller's context in case plan gets invalidated */
	/*
	 *
	 * 复制到调用者的上下文，以防计划失效。
	 */
	return copyObject(tlist);
}

/*
 * Implements the 'DEALLOCATE' utility statement: deletes the
 * specified plan from storage.
 *
 * 实现 DEALLOCATE 实用语句：从存储中删除指定计划。
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
	if (stmt->name)
		DropPreparedStatement(stmt->name, true);
	else
		DropAllPreparedStatements();
}

/*
 * Internal version of DEALLOCATE
 *
 * DEALLOCATE 的内部版本。
 *
 * If showError is false, dropping a nonexistent statement is a no-op.
 *
 * showError 为 false 时，删除不存在的语句是空操作。
 */
void
DropPreparedStatement(const char *stmt_name, bool showError)
{
	PreparedStatement *entry;

	/* Find the query's hash table entry; raise error if wanted */
	/*
	 *
	 * 查找查询的哈希表项；需要时则报错。
	 */
	entry = FetchPreparedStatement(stmt_name, showError);

	if (entry)
	{
		/* Release the plancache entry */
		/*
		 *
		 * 释放计划缓存项。
		 */
		DropCachedPlan(entry->plansource);

		/* Now we can remove the hash table entry */
		/*
		 *
		 * 现在可以删除哈希表项。
		 */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * Drop all cached statements.
 *
 * 丢掉全部缓存语句。
 */
void
DropAllPreparedStatements(void)
{
	HASH_SEQ_STATUS seq;
	PreparedStatement *entry;

	/* nothing cached */
	/*
	 *
	 * 没有缓存内容。
	 */
	if (!prepared_queries)
		return;

	/* walk over cache */
	/*
	 *
	 * 遍历缓存。
	 */
	hash_seq_init(&seq, prepared_queries);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		/* Release the plancache entry */
		/*
		 *
		 * 释放计划缓存项。
		 */
		DropCachedPlan(entry->plansource);

		/* Now we can remove the hash table entry */
		/*
		 *
		 * 现在可以删除哈希表项。
		 */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * Implements the 'EXPLAIN EXECUTE' utility statement.
 *
 * 实现 EXPLAIN EXECUTE 实用语句。
 *
 * "into" is NULL unless we are doing EXPLAIN CREATE TABLE AS EXECUTE,
 * in which case executing the query should result in creating that table.
 *
 * 除非是 EXPLAIN CREATE TABLE AS EXECUTE，否则 into 为 NULL；
 * 那种情况下执行查询应创建该表。
 *
 * Note: the passed-in pstate's queryString is that of the EXPLAIN EXECUTE,
 * not the original PREPARE; we get the latter string from the plancache.
 *
 * 传入 pstate 的 queryString 属于 EXPLAIN EXECUTE，不是原来的 PREPARE；后者从计划缓存取得。
 */
void
ExplainExecuteQuery(ExecuteStmt *execstmt, IntoClause *into, ExplainState *es,
					ParseState *pstate, ParamListInfo params)
{
	PreparedStatement *entry;
	const char *query_string;
	CachedPlan *cplan;
	List	   *plan_list;
	ListCell   *p;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	instr_time	planstart;
	instr_time	planduration;
	BufferUsage bufusage_start,
				bufusage;
	MemoryContextCounters mem_counters;
	MemoryContext planner_ctx = NULL;
	MemoryContext saved_ctx = NULL;

	if (es->memory)
	{
		/* See ExplainOneQuery about this */
		/*
		 *
		 * 参见 ExplainOneQuery。
		 */
		Assert(IsA(CurrentMemoryContext, AllocSetContext));
		planner_ctx = AllocSetContextCreate(CurrentMemoryContext,
											"explain analyze planner context",
											ALLOCSET_DEFAULT_SIZES);
		saved_ctx = MemoryContextSwitchTo(planner_ctx);
	}

	if (es->buffers)
		bufusage_start = pgBufferUsage;
	INSTR_TIME_SET_CURRENT(planstart);

	/* Look it up in the hash table */
	/*
	 *
	 * 在哈希表中查找。
	 */
	entry = FetchPreparedStatement(execstmt->name, true);

	/* Shouldn't find a non-fixed-result cached plan */
	/*
	 *
	 * 不应找到结果不固定的缓存计划。
	 */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXPLAIN EXECUTE does not support variable-result cached plans");

	query_string = entry->plansource->query_string;

	/* Evaluate parameters, if any */
	/*
	 *
	 * 若有参数则求值。
	 */
	if (entry->plansource->num_params)
	{
		ParseState *pstate_params;

		pstate_params = make_parsestate(NULL);
		pstate_params->p_sourcetext = pstate->p_sourcetext;

		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.  Note that the
		 * passed-in "params" could possibly be referenced in the parameter
		 * expressions.
		 *
		 * 求值参数需要 EState，并且必须留到查询结束，因为参数可能是传引用的。
		 * 传入的 params 也可能被参数表达式引用。
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;

		paramLI = EvaluateParams(pstate_params, entry, execstmt->params, estate);
	}

	/* Replan if needed, and acquire a transient refcount */
	/*
	 *
	 * 必要时重新规划，并取得一个临时引用计数。
	 */
	cplan = GetCachedPlan(entry->plansource, paramLI,
						  CurrentResourceOwner, pstate->p_queryEnv);

	INSTR_TIME_SET_CURRENT(planduration);
	INSTR_TIME_SUBTRACT(planduration, planstart);

	if (es->memory)
	{
		MemoryContextSwitchTo(saved_ctx);
		MemoryContextMemConsumed(planner_ctx, &mem_counters);
	}

	/* calc differences of buffer counters. */
	/*
	 *
	 * 计算缓冲区计数器的差值。
	 */
	if (es->buffers)
	{
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
	}

	plan_list = cplan->stmt_list;

	/* Explain each query */
	/*
	 *
	 * 解释每条查询。
	 */
	foreach(p, plan_list)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, p);

		if (pstmt->commandType != CMD_UTILITY)
			ExplainOnePlan(pstmt, into, es, query_string, paramLI, pstate->p_queryEnv,
						   &planduration, (es->buffers ? &bufusage : NULL),
						   es->memory ? &mem_counters : NULL);
		else
			ExplainOneUtility(pstmt->utilityStmt, into, es, pstate, paramLI);

		/* No need for CommandCounterIncrement, as ExplainOnePlan did it */
		/*
		 *
		 * 不必 CommandCounterIncrement，ExplainOnePlan 已经做过。
		 */

		/* Separate plans with an appropriate separator */
		/*
		 *
		 * 用适当的分隔符隔开各个计划。
		 */
		if (lnext(plan_list, p) != NULL)
			ExplainSeparatePlans(es);
	}

	if (estate)
		FreeExecutorState(estate);

	ReleaseCachedPlan(cplan, CurrentResourceOwner);
}

/*
 * This set returning function reads all the prepared statements and
 * returns a set of (name, statement, prepare_time, param_types, from_sql,
 * generic_plans, custom_plans).
 *
 * 这个集合返回函数读出全部预备语句，返回 (name, statement, prepare_time, param_types, from_sql, generic_plans, custom_plans)。
 */
Datum
pg_prepared_statement(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/*
	 * We put all the tuples into a tuplestore in one scan of the hashtable.
	 * This avoids any issue of the hashtable possibly changing between calls.
	 *
	 * 一次扫描哈希表就把全部元组放进 tuplestore，避免调用之间哈希表发生变化。
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* hash table might be uninitialized */
	/*
	 *
	 * 哈希表可能尚未初始化。
	 */
	if (prepared_queries)
	{
		HASH_SEQ_STATUS hash_seq;
		PreparedStatement *prep_stmt;

		hash_seq_init(&hash_seq, prepared_queries);
		while ((prep_stmt = hash_seq_search(&hash_seq)) != NULL)
		{
			TupleDesc	result_desc;
			Datum		values[8];
			bool		nulls[8] = {0};

			result_desc = prep_stmt->plansource->resultDesc;

			values[0] = CStringGetTextDatum(prep_stmt->stmt_name);
			values[1] = CStringGetTextDatum(prep_stmt->plansource->query_string);
			values[2] = TimestampTzGetDatum(prep_stmt->prepare_time);
			values[3] = build_regtype_array(prep_stmt->plansource->param_types,
											prep_stmt->plansource->num_params);
			if (result_desc)
			{
				Oid		   *result_types;

				result_types = palloc_array(Oid, result_desc->natts);
				for (int i = 0; i < result_desc->natts; i++)
					result_types[i] = TupleDescAttr(result_desc, i)->atttypid;
				values[4] = build_regtype_array(result_types, result_desc->natts);
			}
			else
			{
				/* no result descriptor (for example, DML statement) */
				/*
				 *
				 * 没有结果描述符（例如 DML 语句）。
				 */
				nulls[4] = true;
			}
			values[5] = BoolGetDatum(prep_stmt->from_sql);
			values[6] = Int64GetDatumFast(prep_stmt->plansource->num_generic_plans);
			values[7] = Int64GetDatumFast(prep_stmt->plansource->num_custom_plans);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * This utility function takes a C array of Oids, and returns a Datum
 * pointing to a one-dimensional Postgres array of regtypes. An empty
 * array is returned as a zero-element array, not NULL.
 *
 * 该实用函数接收 Oid 的 C 数组，返回指向一维 regtype 数组的 Datum。空数组返回零元素数组，而不是 NULL。
 */
static Datum
build_regtype_array(Oid *param_types, int num_params)
{
	Datum	   *tmp_ary;
	ArrayType  *result;
	int			i;

	tmp_ary = palloc_array(Datum, num_params);

	for (i = 0; i < num_params; i++)
		tmp_ary[i] = ObjectIdGetDatum(param_types[i]);

	result = construct_array_builtin(tmp_ary, num_params, REGTYPEOID);
	return PointerGetDatum(result);
}
