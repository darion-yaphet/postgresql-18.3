/*-------------------------------------------------------------------------
 *
 * createas.c
 *	  Execution of CREATE TABLE ... AS, a/k/a SELECT INTO.
 *	  Since CREATE MATERIALIZED VIEW shares syntax and most behaviors,
 *	  we implement that here, too.
 *
 * 执行 CREATE TABLE AS（即 SELECT INTO）。
 * CREATE MATERIALIZED VIEW 语法和行为大多相同，也在这里实现。
 *
 * We implement this by diverting the query's normal output to a
 * specialized DestReceiver type.
 *
 * 实现方式是把查询的正常输出转到专用的 DestReceiver。
 *
 * Formerly, CTAS was implemented as a variant of SELECT, which led
 * to assorted legacy behaviors that we still try to preserve, notably that
 * we must return a tuples-processed count in the QueryCompletion.  (We no
 * longer do that for CTAS ... WITH NO DATA, however.)
 *
 * CTAS 以前是 SELECT 的变体，因此留下一些仍要保持的旧行为，
 * 尤其是必须在 QueryCompletion 里返回处理的元组数。CTAS ... WITH NO DATA 则不再这样做。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/createas.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/createas.h"
#include "commands/matview.h"
#include "commands/prepare.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "parser/analyze.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"

/*
 * 核心流程概览：
 * ExecCreateTableAs 处理 CREATE TABLE AS 和 CREATE MATERIALIZED VIEW。
 * WITH NO DATA 走 create_ctas_nodata，类似 CREATE VIEW；否则规划并执行查询，
 * 由 IntoRel DestReceiver 在 intorel_startup 中建表，intorel_receive 批量插入，
 * intorel_shutdown 收尾。物化视图的填数复用 REFRESH。
 */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	/*
	 *
	 * 对外可见的函数指针。
	 */
	IntoClause *into;			/* target relation specification */
	/*
	 *
	 * 目标关系说明。
	 */
	/* These fields are filled by intorel_startup: */
	/*
	 *
	 * 这些字段由 intorel_startup 填写：
	 */
	Relation	rel;			/* relation to write to */
	/*
	 *
	 * 要写入的关系。
	 */
	ObjectAddress reladdr;		/* address of rel, for ExecCreateTableAs */
	/*
	 *
	 * 关系地址，供 ExecCreateTableAs 使用。
	 */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	/*
	 *
	 * 写入输出元组的 cmin。
	 */
	int			ti_options;		/* table_tuple_insert performance options */
	/*
	 *
	 * table_tuple_insert 的性能选项。
	 */
	BulkInsertState bistate;	/* bulk insert state */
	/*
	 *
	 * 批量插入状态。
	 */
} DR_intorel;

/* utility functions for CTAS definition creation */
/*
 *
 * 创建 CTAS 定义的实用函数。
 */
static ObjectAddress create_ctas_internal(List *attrList, IntoClause *into);
static ObjectAddress create_ctas_nodata(List *tlist, IntoClause *into);

/* DestReceiver routines for collecting data */
/*
 *
 * 收集数据的 DestReceiver 例程。
 */
static void intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool intorel_receive(TupleTableSlot *slot, DestReceiver *self);
static void intorel_shutdown(DestReceiver *self);
static void intorel_destroy(DestReceiver *self);


/*
 * create_ctas_internal
 *
 * create_ctas_internal：
 *
 * Internal utility used for the creation of the definition of a relation
 * created via CREATE TABLE AS or a materialized view.  Caller needs to
 * provide a list of attributes (ColumnDef nodes).
 *
 * 为 CREATE TABLE AS 或物化视图创建关系定义的内部工具。调用者需提供属性列表（ColumnDef 节点）。
 */
static ObjectAddress
create_ctas_internal(List *attrList, IntoClause *into)
{
	CreateStmt *create = makeNode(CreateStmt);
	bool		is_matview;
	char		relkind;
	Datum		toast_options;
	const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;
	ObjectAddress intoRelationAddr;

	/* This code supports both CREATE TABLE AS and CREATE MATERIALIZED VIEW */
	/*
	 *
	 * 这段代码同时支持 CREATE TABLE AS 和 CREATE MATERIALIZED VIEW。
	 */
	is_matview = (into->viewQuery != NULL);
	relkind = is_matview ? RELKIND_MATVIEW : RELKIND_RELATION;

	/*
	 * Create the target relation by faking up a CREATE TABLE parsetree and
	 * passing it to DefineRelation.
	 *
	 * 伪造一棵 CREATE TABLE 分析树，交给 DefineRelation 来创建目标关系。
	 */
	create->relation = into->rel;
	create->tableElts = attrList;
	create->inhRelations = NIL;
	create->ofTypename = NULL;
	create->constraints = NIL;
	create->options = into->options;
	create->oncommit = into->onCommit;
	create->tablespacename = into->tableSpaceName;
	create->if_not_exists = false;
	create->accessMethod = into->accessMethod;

	/*
	 * Create the relation.  (This will error out if there's an existing view,
	 * so we don't need more code to complain if "replace" is false.)
	 *
	 * 创建关系。若已有视图会在这里报错，因此 replace 为 false 时不必再额外报错。
	 */
	intoRelationAddr = DefineRelation(create, relkind, InvalidOid, NULL, NULL);

	/*
	 * If necessary, create a TOAST table for the target table.  Note that
	 * NewRelationCreateToastTable ends with CommandCounterIncrement(), so
	 * that the TOAST table will be visible for insertion.
	 *
	 * 必要时为目标表创建 TOAST 表。NewRelationCreateToastTable 结束时会 CommandCounterIncrement()，
	 * 使 TOAST 表对后续插入可见。
	 */
	CommandCounterIncrement();

	/* parse and validate reloptions for the toast table */
	/*
	 *
	 * 解析并校验 TOAST 表的 reloptions。
	 */
	toast_options = transformRelOptions((Datum) 0,
										create->options,
										"toast",
										validnsps,
										true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	NewRelationCreateToastTable(intoRelationAddr.objectId, toast_options);

	/* Create the "view" part of a materialized view. */
	/*
	 *
	 * 创建物化视图的“视图”部分。
	 */
	if (is_matview)
	{
		/* StoreViewQuery scribbles on tree, so make a copy */
		/*
		 *
		 * StoreViewQuery 会改写树，因此先复制一份。
		 */
		Query	   *query = copyObject(into->viewQuery);

		StoreViewQuery(intoRelationAddr.objectId, query, false);
		CommandCounterIncrement();
	}

	return intoRelationAddr;
}


/*
 * create_ctas_nodata
 *
 * create_ctas_nodata：
 *
 * Create CTAS or materialized view when WITH NO DATA is used, starting from
 * the targetlist of the SELECT or view definition.
 *
 * WITH NO DATA 时，从 SELECT 或视图定义的目标列表出发创建 CTAS 或物化视图。
 */
static ObjectAddress
create_ctas_nodata(List *tlist, IntoClause *into)
{
	List	   *attrList;
	ListCell   *t,
			   *lc;

	/*
	 * Build list of ColumnDefs from non-junk elements of the tlist.  If a
	 * column name list was specified in CREATE TABLE AS, override the column
	 * names in the query.  (Too few column names are OK, too many are not.)
	 *
	 * 从目标列表的非 junk 项构造 ColumnDef。若 CREATE TABLE AS 指定了列名，则覆盖查询中的列名。
	 * 列名过少可以，过多不行。
	 */
	attrList = NIL;
	lc = list_head(into->colNames);
	foreach(t, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(t);

		if (!tle->resjunk)
		{
			ColumnDef  *col;
			char	   *colname;

			if (lc)
			{
				colname = strVal(lfirst(lc));
				lc = lnext(into->colNames, lc);
			}
			else
				colname = tle->resname;

			col = makeColumnDef(colname,
								exprType((Node *) tle->expr),
								exprTypmod((Node *) tle->expr),
								exprCollation((Node *) tle->expr));

			/*
			 * It's possible that the column is of a collatable type but the
			 * collation could not be resolved, so double-check.  (We must
			 * check this here because DefineRelation would adopt the type's
			 * default collation rather than complaining.)
			 *
			 * 列可能是可排序类型，但排序规则未能解析，这里再检查一次。
			 * 必须在这里检查，否则 DefineRelation 会采用类型的默认排序规则而不是报错。
			 */
			if (!OidIsValid(col->collOid) &&
				type_is_collatable(col->typeName->typeOid))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("no collation was derived for column \"%s\" with collatable type %s",
								col->colname,
								format_type_be(col->typeName->typeOid)),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));

			attrList = lappend(attrList, col);
		}
	}

	if (lc != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("too many column names were specified")));

	/* Create the relation definition using the ColumnDef list */
	/*
	 *
	 * 用 ColumnDef 列表创建关系定义。
	 */
	return create_ctas_internal(attrList, into);
}


/*
 * ExecCreateTableAs -- execute a CREATE TABLE AS command
 *
 * ExecCreateTableAs：执行 CREATE TABLE AS。
 */
ObjectAddress
ExecCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
				  ParamListInfo params, QueryEnvironment *queryEnv,
				  QueryCompletion *qc)
{
	Query	   *query = castNode(Query, stmt->query);
	IntoClause *into = stmt->into;
	JumbleState *jstate = NULL;
	bool		is_matview = (into->viewQuery != NULL);
	bool		do_refresh = false;
	DestReceiver *dest;
	ObjectAddress address;

	/* Check if the relation exists or not */
	/*
	 *
	 * 检查关系是否已存在。
	 */
	if (CreateTableAsRelExists(stmt))
		return InvalidObjectAddress;

	/*
	 * Create the tuple receiver object and insert info it will need
	 *
	 * 创建元组接收器，并填入它需要的信息。
	 */
	dest = CreateIntoRelDestReceiver(into);

	/* Query contained by CTAS needs to be jumbled if requested */
	/*
	 *
	 * 若有请求，CTAS 所含的 Query 需要做 jumble。
	 */
	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	/*
	 * The contained Query could be a SELECT, or an EXECUTE utility command.
	 * If the latter, we just pass it off to ExecuteQuery.
	 *
	 * 所含 Query 可能是 SELECT，也可能是 EXECUTE 实用命令。后者直接交给 ExecuteQuery。
	 */
	if (query->commandType == CMD_UTILITY &&
		IsA(query->utilityStmt, ExecuteStmt))
	{
		ExecuteStmt *estmt = castNode(ExecuteStmt, query->utilityStmt);

		Assert(!is_matview);	/* excluded by syntax */
		/*
		 *
		 * 语法已排除。
		 */
		ExecuteQuery(pstate, estmt, into, params, dest, qc);

		/* get object address that intorel_startup saved for us */
		/*
		 *
		 * 取 intorel_startup 为我们保存的对象地址。
		 */
		address = ((DR_intorel *) dest)->reladdr;

		return address;
	}
	Assert(query->commandType == CMD_SELECT);

	/*
	 * For materialized views, always skip data during table creation, and use
	 * REFRESH instead (see below).
	 *
	 * 物化视图在建表时总是跳过数据，改用 REFRESH（见下）。
	 */
	if (is_matview)
	{
		do_refresh = !into->skipData;
		into->skipData = true;
	}

	if (into->skipData)
	{
		/*
		 * If WITH NO DATA was specified, do not go through the rewriter,
		 * planner and executor.  Just define the relation using a code path
		 * similar to CREATE VIEW.  This avoids dump/restore problems stemming
		 * from running the planner before all dependencies are set up.
		 *
		 * 若指定了 WITH NO DATA，不经过重写器、规划器和执行器，
		 * 用类似 CREATE VIEW 的路径定义关系。这样可避免在依赖尚未建好时运行规划器导致的转储/恢复问题。
		 */
		address = create_ctas_nodata(query->targetList, into);

		/*
		 * For materialized views, reuse the REFRESH logic, which locks down
		 * security-restricted operations and restricts the search_path.  This
		 * reduces the chance that a subsequent refresh will fail.
		 *
		 * 物化视图复用 REFRESH 逻辑，它会锁住安全受限操作并限制 search_path，
		 * 以降低随后刷新失败的可能。
		 */
		if (do_refresh)
			RefreshMatViewByOid(address.objectId, true, false, false,
								pstate->p_sourcetext, qc);

	}
	else
	{
		List	   *rewritten;
		PlannedStmt *plan;
		QueryDesc  *queryDesc;

		Assert(!is_matview);

		/*
		 * Parse analysis was done already, but we still have to run the rule
		 * rewriter.  We do not do AcquireRewriteLocks: we assume the query
		 * either came straight from the parser, or suitable locks were
		 * acquired by plancache.c.
		 *
		 * 语法分析已经做过，但仍要跑规则重写器。这里不做 AcquireRewriteLocks：
		 * 假定查询直接来自解析器，或 plancache.c 已经加上了合适的锁。
		 */
		rewritten = QueryRewrite(query);

		/* SELECT should never rewrite to more or less than one SELECT query */
		/*
		 *
		 * SELECT 重写后不应变成多于或少于一条 SELECT。
		 */
		if (list_length(rewritten) != 1)
			elog(ERROR, "unexpected rewrite result for CREATE TABLE AS SELECT");
		query = linitial_node(Query, rewritten);
		Assert(query->commandType == CMD_SELECT);

		/* plan the query */
		/*
		 *
		 * 规划查询。
		 */
		plan = pg_plan_query(query, pstate->p_sourcetext,
							 CURSOR_OPT_PARALLEL_OK, params);

		/*
		 * Use a snapshot with an updated command ID to ensure this query sees
		 * results of any previously executed queries.  (This could only
		 * matter if the planner executed an allegedly-stable function that
		 * changed the database contents, but let's do it anyway to be
		 * parallel to the EXPLAIN code path.)
		 *
		 * 使用命令 ID 已更新的快照，使本查询能看见先前已执行查询的结果。
		 * 这只在规划器执行了声称 stable 却修改了数据库的函数时才有影响，但为了与 EXPLAIN 路径一致仍然这样做。
		 */
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();

		/* Create a QueryDesc, redirecting output to our tuple receiver */
		/*
		 *
		 * 创建 QueryDesc，把输出重定向到我们的元组接收器。
		 */
		queryDesc = CreateQueryDesc(plan, pstate->p_sourcetext,
									GetActiveSnapshot(), InvalidSnapshot,
									dest, params, queryEnv, 0);

		/* call ExecutorStart to prepare the plan for execution */
		/*
		 *
		 * 调用 ExecutorStart，准备执行计划。
		 */
		ExecutorStart(queryDesc, GetIntoRelEFlags(into));

		/* run the plan to completion */
		/*
		 *
		 * 把计划执行到结束。
		 */
		ExecutorRun(queryDesc, ForwardScanDirection, 0);

		/* save the rowcount if we're given a qc to fill */
		/*
		 *
		 * 若给出了要填充的 qc，则保存行数。
		 */
		if (qc)
			SetQueryCompletion(qc, CMDTAG_SELECT, queryDesc->estate->es_processed);

		/* get object address that intorel_startup saved for us */
		/*
		 *
		 * 取 intorel_startup 为我们保存的对象地址。
		 */
		address = ((DR_intorel *) dest)->reladdr;

		/* and clean up */
		/*
		 *
		 * 然后清理。
		 */
		ExecutorFinish(queryDesc);
		ExecutorEnd(queryDesc);

		FreeQueryDesc(queryDesc);

		PopActiveSnapshot();
	}

	return address;
}

/*
 * GetIntoRelEFlags --- compute executor flags needed for CREATE TABLE AS
 *
 * GetIntoRelEFlags：计算 CREATE TABLE AS 所需的执行器标志。
 *
 * This is exported because EXPLAIN and PREPARE need it too.  (Note: those
 * callers still need to deal explicitly with the skipData flag; since they
 * use different methods for suppressing execution, it doesn't seem worth
 * trying to encapsulate that part.)
 *
 * 导出此函数是因为 EXPLAIN 和 PREPARE 也需要它。这些调用者仍须自行处理 skipData，
 * 它们抑制执行的方式不同，不值得把那一部分也封装进来。
 */
int
GetIntoRelEFlags(IntoClause *intoClause)
{
	int			flags = 0;

	if (intoClause->skipData)
		flags |= EXEC_FLAG_WITH_NO_DATA;

	return flags;
}

/*
 * CreateTableAsRelExists --- check existence of relation for CreateTableAsStmt
 *
 * CreateTableAsRelExists：检查 CreateTableAsStmt 的目标关系是否已存在。
 *
 * Utility wrapper checking if the relation pending for creation in this
 * CreateTableAsStmt query already exists or not.  Returns true if the
 * relation exists, otherwise false.
 *
 * 检查这条 CreateTableAsStmt 即将创建的关系是否已存在。存在返回 true，否则返回 false。
 */
bool
CreateTableAsRelExists(CreateTableAsStmt *ctas)
{
	Oid			nspid;
	Oid			oldrelid;
	ObjectAddress address;
	IntoClause *into = ctas->into;

	nspid = RangeVarGetCreationNamespace(into->rel);

	oldrelid = get_relname_relid(into->rel->relname, nspid);
	if (OidIsValid(oldrelid))
	{
		if (!ctas->if_not_exists)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists",
							into->rel->relname)));

		/*
		 * The relation exists and IF NOT EXISTS has been specified.
		 *
		 * 关系已存在，并且指定了 IF NOT EXISTS。
		 *
		 * If we are in an extension script, insist that the pre-existing
		 * object be a member of the extension, to avoid security risks.
		 *
		 * 若处于扩展脚本中，已存在的对象必须属于该扩展，以避免安全风险。
		 */
		ObjectAddressSet(address, RelationRelationId, oldrelid);
		checkMembershipInCurrentExtension(&address);

		/* OK to skip */
		/*
		 *
		 * 可以跳过。
		 */
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists, skipping",
						into->rel->relname)));
		return true;
	}

	/* Relation does not exist, it can be created */
	/*
	 *
	 * 关系不存在，可以创建。
	 */
	return false;
}

/*
 * CreateIntoRelDestReceiver -- create a suitable DestReceiver object
 *
 * CreateIntoRelDestReceiver：创建合适的 DestReceiver。
 *
 * intoClause will be NULL if called from CreateDestReceiver(), in which
 * case it has to be provided later.  However, it is convenient to allow
 * self->into to be filled in immediately for other callers.
 *
 * 从 CreateDestReceiver() 调用时 intoClause 为 NULL，必须稍后补上。
 * 对其它调用者，可以立刻填好 self->into。
 */
DestReceiver *
CreateIntoRelDestReceiver(IntoClause *intoClause)
{
	DR_intorel *self = (DR_intorel *) palloc0(sizeof(DR_intorel));

	self->pub.receiveSlot = intorel_receive;
	self->pub.rStartup = intorel_startup;
	self->pub.rShutdown = intorel_shutdown;
	self->pub.rDestroy = intorel_destroy;
	self->pub.mydest = DestIntoRel;
	self->into = intoClause;
	/* other private fields will be set during intorel_startup */
	/*
	 *
	 * 其它私有字段在 intorel_startup 中设置。
	 */

	return (DestReceiver *) self;
}

/*
 * intorel_startup --- executor startup
 *
 * intorel_startup：执行器启动。
 */
static void
intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_intorel *myState = (DR_intorel *) self;
	IntoClause *into = myState->into;
	bool		is_matview;
	List	   *attrList;
	ObjectAddress intoRelationAddr;
	Relation	intoRelationDesc;
	ListCell   *lc;
	int			attnum;

	Assert(into != NULL);		/* else somebody forgot to set it */
	/*
	 *
	 * 否则就是有人忘了设置它。
	 */

	/* This code supports both CREATE TABLE AS and CREATE MATERIALIZED VIEW */
	/*
	 *
	 * 这段代码同时支持 CREATE TABLE AS 和 CREATE MATERIALIZED VIEW。
	 */
	is_matview = (into->viewQuery != NULL);

	/*
	 * Build column definitions using "pre-cooked" type and collation info. If
	 * a column name list was specified in CREATE TABLE AS, override the
	 * column names derived from the query.  (Too few column names are OK, too
	 * many are not.)
	 *
	 * 用已经准备好的类型和排序规则信息构造列定义。若 CREATE TABLE AS 指定了列名，则覆盖查询推导出的列名。
	 * 列名过少可以，过多不行。
	 */
	attrList = NIL;
	lc = list_head(into->colNames);
	for (attnum = 0; attnum < typeinfo->natts; attnum++)
	{
		Form_pg_attribute attribute = TupleDescAttr(typeinfo, attnum);
		ColumnDef  *col;
		char	   *colname;

		if (lc)
		{
			colname = strVal(lfirst(lc));
			lc = lnext(into->colNames, lc);
		}
		else
			colname = NameStr(attribute->attname);

		col = makeColumnDef(colname,
							attribute->atttypid,
							attribute->atttypmod,
							attribute->attcollation);

		/*
		 * It's possible that the column is of a collatable type but the
		 * collation could not be resolved, so double-check.  (We must check
		 * this here because DefineRelation would adopt the type's default
		 * collation rather than complaining.)
		 *
		 * 列可能是可排序类型，但排序规则未能解析，这里再检查一次。
		 * 必须在这里检查，否则 DefineRelation 会采用类型的默认排序规则而不是报错。
		 */
		if (!OidIsValid(col->collOid) &&
			type_is_collatable(col->typeName->typeOid))
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_COLLATION),
					 errmsg("no collation was derived for column \"%s\" with collatable type %s",
							col->colname,
							format_type_be(col->typeName->typeOid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));

		attrList = lappend(attrList, col);
	}

	if (lc != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("too many column names were specified")));

	/*
	 * Actually create the target table
	 *
	 * 真正创建目标表。
	 */
	intoRelationAddr = create_ctas_internal(attrList, into);

	/*
	 * Finally we can open the target table
	 *
	 * 最后可以打开目标表。
	 */
	intoRelationDesc = table_open(intoRelationAddr.objectId, AccessExclusiveLock);

	/*
	 * Make sure the constructed table does not have RLS enabled.
	 *
	 * 确认构造出的表没有启用 RLS。
	 *
	 * check_enable_rls() will ereport(ERROR) itself if the user has requested
	 * something invalid, and otherwise will return RLS_ENABLED if RLS should
	 * be enabled here.  We don't actually support that currently, so throw
	 * our own ereport(ERROR) if that happens.
	 *
	 * 用户请求非法时 check_enable_rls() 自己会 ereport(ERROR)；
	 * 若这里应启用 RLS 则返回 RLS_ENABLED。目前并不支持这种情况，因此再自行 ereport(ERROR)。
	 */
	if (check_enable_rls(intoRelationAddr.objectId, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("policies not yet implemented for this command")));

	/*
	 * Tentatively mark the target as populated, if it's a matview and we're
	 * going to fill it; otherwise, no change needed.
	 *
	 * 若是物化视图并且即将填充数据，先暂标为已填充；否则不必改。
	 */
	if (is_matview && !into->skipData)
		SetMatViewPopulatedState(intoRelationDesc, true);

	/*
	 * Fill private fields of myState for use by later routines
	 *
	 * 填写 myState 的私有字段，供后续例程使用。
	 */
	myState->rel = intoRelationDesc;
	myState->reladdr = intoRelationAddr;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM;

	/*
	 * If WITH NO DATA is specified, there is no need to set up the state for
	 * bulk inserts as there are no tuples to insert.
	 *
	 * 指定了 WITH NO DATA 时没有元组要插入，不必建立批量插入状态。
	 */
	if (!into->skipData)
		myState->bistate = GetBulkInsertState();
	else
		myState->bistate = NULL;

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 *
	 * 有效的 smgr_targblock 意味着已有东西写过该关系。这也许无害，但本函数没有为此做准备。
	 */
	Assert(RelationGetTargetBlock(intoRelationDesc) == InvalidBlockNumber);
}

/*
 * intorel_receive --- receive one tuple
 *
 * intorel_receive：接收一个元组。
 */
static bool
intorel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;

	/* Nothing to insert if WITH NO DATA is specified. */
	/*
	 *
	 * 指定了 WITH NO DATA 时无内容可插入。
	 */
	if (!myState->into->skipData)
	{
		/*
		 * Note that the input slot might not be of the type of the target
		 * relation. That's supported by table_tuple_insert(), but slightly
		 * less efficient than inserting with the right slot - but the
		 * alternative would be to copy into a slot of the right type, which
		 * would not be cheap either. This also doesn't allow accessing per-AM
		 * data (say a tuple's xmin), but since we don't do that here...
		 *
		 * 输入 slot 的类型可能不是目标关系的类型。table_tuple_insert() 支持这种情况，
		 * 但比用正确类型的 slot 稍慢；复制到正确类型的 slot 也不便宜。
		 * 这样也不能访问访问方法私有数据（例如元组的 xmin），而这里并不需要。
		 */
		table_tuple_insert(myState->rel,
						   slot,
						   myState->output_cid,
						   myState->ti_options,
						   myState->bistate);
	}

	/* We know this is a newly created relation, so there are no indexes */
	/*
	 *
	 * 这是新建的关系，因此没有索引。
	 */

	return true;
}

/*
 * intorel_shutdown --- executor end
 *
 * intorel_shutdown：执行器结束。
 */
static void
intorel_shutdown(DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;
	IntoClause *into = myState->into;

	if (!into->skipData)
	{
		FreeBulkInsertState(myState->bistate);
		table_finish_bulk_insert(myState->rel, myState->ti_options);
	}

	/* close rel, but keep lock until commit */
	/*
	 *
	 * 关闭关系，但把锁保留到提交。
	 */
	table_close(myState->rel, NoLock);
	myState->rel = NULL;
}

/*
 * intorel_destroy --- release DestReceiver object
 *
 * intorel_destroy：释放 DestReceiver 对象。
 */
static void
intorel_destroy(DestReceiver *self)
{
	pfree(self);
}
