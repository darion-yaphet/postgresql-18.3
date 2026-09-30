/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * 物化视图支持。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "commands/cluster.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * 核心流程概览：
 * ExecRefreshMatView 刷新物化视图：WITH NO DATA 时清空并标记未填充；
 * 否则经 RefreshMatViewByOid 建临时堆，refresh_matview_datafill 执行查询写入。
 * 指定 CONCURRENTLY 时 refresh_by_match_merge 按唯一索引做差异合并；
 * 否则 refresh_by_heap_swap 交换文件并重建索引。
 * SetMatViewPopulatedState 更新 pg_class 的填充状态。
 * OpenMatViewIncrementalMaintenance / CloseMatViewIncrementalMaintenance 维护允许内部 DML 的深度计数。
 */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	/*
	 *
	 * 对外已知的函数指针。
	 */
	Oid			transientoid;	/* OID of new heap into which to store */
	/*
	 *
	 * 用于存放数据的新堆的 OID。
	 */
	/* These fields are filled by transientrel_startup: */
	/*
	 *
	 * 这些字段由 transientrel_startup 填充：
	 */
	Relation	transientrel;	/* relation to write to */
	/*
	 *
	 * 要写入的关系。
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
} DR_transientrel;

static int	matview_maintenance_depth = 0;

static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static uint64 refresh_matview_datafill(DestReceiver *dest, Query *query,
									   const char *queryString, bool is_create);
static char *make_temptable_name_n(char *tempname, int n);
static void refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
								   int save_sec_context);
static void refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence);
static bool is_usable_unique_index(Relation indexRel);
static void OpenMatViewIncrementalMaintenance(void);
static void CloseMatViewIncrementalMaintenance(void);

/*
 * SetMatViewPopulatedState
 *		Mark a materialized view as populated, or not.
 *
 * SetMatViewPopulatedState：把物化视图标为已填充或未填充。
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 *
 * 注意：调用方必须已对关系持有合适的锁。
 */
void
SetMatViewPopulatedState(Relation relation, bool newstate)
{
	Relation	pgrel;
	HeapTuple	tuple;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 *
	 * 更新关系的 pg_class 项。关键副作用：向其他后端（以及本后端）发送 SI 消息，使其重建 relcache。
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(relation));

	((Form_pg_class) GETSTRUCT(tuple))->relispopulated = newstate;

	CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * Advance command counter to make the updated pg_class row locally
	 * visible.
	 *
	 * 推进命令计数器，使更新后的 pg_class 行在本地可见。
	 */
	CommandCounterIncrement();
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * ExecRefreshMatView：执行 REFRESH MATERIALIZED VIEW。
 *
 * If WITH NO DATA was specified, this is effectively like a TRUNCATE;
 * otherwise it is like a TRUNCATE followed by an INSERT using the SELECT
 * statement associated with the materialized view.  The statement node's
 * skipData field shows whether the clause was used.
 *
 * 若指定了 WITH NO DATA，效果类似 TRUNCATE；否则类似 TRUNCATE 后再用物化视图的 SELECT 做 INSERT。
 * 语句节点的 skipData 表示是否使用了该子句。
 */
ObjectAddress
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				   QueryCompletion *qc)
{
	Oid			matviewOid;
	LOCKMODE	lockmode;

	/* Determine strength of lock needed. */
	/*
	 *
	 * 确定所需的锁强度。
	 */
	lockmode = stmt->concurrent ? ExclusiveLock : AccessExclusiveLock;

	/*
	 * Get a lock until end of transaction.
	 *
	 * 取得锁并保持到事务结束。
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										  lockmode, 0,
										  RangeVarCallbackMaintainsTable,
										  NULL);

	return RefreshMatViewByOid(matviewOid, false, stmt->skipData,
							   stmt->concurrent, queryString, qc);
}

/*
 * RefreshMatViewByOid -- refresh materialized view by OID
 *
 * RefreshMatViewByOid：按 OID 刷新物化视图。
 *
 * This refreshes the materialized view by creating a new table and swapping
 * the relfilenumbers of the new table and the old materialized view, so the OID
 * of the original materialized view is preserved. Thus we do not lose GRANT
 * nor references to this materialized view.
 *
 * 通过创建新表并交换新旧 relfilenumber 来刷新物化视图，从而保留原 OID。
 * 因此不会丢失 GRANT 以及对该物化视图的引用。
 *
 * If skipData is true, this is effectively like a TRUNCATE; otherwise it is
 * like a TRUNCATE followed by an INSERT using the SELECT statement associated
 * with the materialized view.
 *
 * skipData 为真时效果类似 TRUNCATE；否则类似 TRUNCATE 后再用物化视图关联的 SELECT 做 INSERT。
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new heap, it's better to create the indexes afterwards than to fill them
 * incrementally while we load.
 *
 * 索引也通过 REINDEX 重建。相当于在批量装载新堆，应在装载之后再建索引，而不是边装载边填充。
 *
 * The matview's "populated" state is changed based on whether the contents
 * reflect the result set of the materialized view's query.
 *
 * 物化视图的 populated 状态取决于其内容是否反映查询的结果集。
 *
 * This is also used to populate the materialized view created by CREATE
 * MATERIALIZED VIEW command.
 *
 * CREATE MATERIALIZED VIEW 创建后的填充也走这条路径。
 */
ObjectAddress
RefreshMatViewByOid(Oid matviewOid, bool is_create, bool skipData,
					bool concurrent, const char *queryString,
					QueryCompletion *qc)
{
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			tableSpace;
	Oid			relowner;
	Oid			OIDNewHeap;
	uint64		processed = 0;
	char		relpersistence;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	ObjectAddress address;

	matviewRel = table_open(matviewOid, NoLock);
	relowner = matviewRel->rd_rel->relowner;

	/*
	 * Switch to the owner's userid, so that any functions are run as that
	 * user.  Also lock down security-restricted operations and arrange to
	 * make GUC variable changes local to this command.
	 *
	 * 切换到属主的用户 ID，使函数以该用户身份运行。
	 * 同时限制安全敏感操作，并让 GUC 变更仅在本命令内有效。
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Make sure it is a materialized view. */
	/*
	 *
	 * 确认它是物化视图。
	 */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/* Check that CONCURRENTLY is not specified if not populated. */
	/*
	 *
	 * 若尚未填充，则检查没有指定 CONCURRENTLY。
	 */
	if (concurrent && !RelationIsPopulated(matviewRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY cannot be used when the materialized view is not populated")));

	/* Check that conflicting options have not been specified. */
	/*
	 *
	 * 检查没有指定互相冲突的选项。
	 */
	if (concurrent && skipData)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s and %s options cannot be used together",
						"CONCURRENTLY", "WITH NO DATA")));

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 *
	 * 检查刷新所需的条件都正确。此时的问题属于内部错误，elog 就够了。
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * Check that there is a unique index with no WHERE clause on one or more
	 * columns of the materialized view if CONCURRENTLY is specified.
	 *
	 * 若指定了 CONCURRENTLY，则检查物化视图上存在覆盖一列或多列、且没有 WHERE 子句的唯一索引。
	 */
	if (concurrent)
	{
		List	   *indexoidlist = RelationGetIndexList(matviewRel);
		ListCell   *indexoidscan;
		bool		hasUniqueIndex = false;

		Assert(!is_create);

		foreach(indexoidscan, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(indexoidscan);
			Relation	indexRel;

			indexRel = index_open(indexoid, AccessShareLock);
			hasUniqueIndex = is_usable_unique_index(indexRel);
			index_close(indexRel, AccessShareLock);
			if (hasUniqueIndex)
				break;
		}

		list_free(indexoidlist);

		if (!hasUniqueIndex)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot refresh materialized view \"%s\" concurrently",
							quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
													   RelationGetRelationName(matviewRel))),
					 errhint("Create a unique index with no WHERE clause on one or more columns of the materialized view.")));
	}

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 *
	 * 存储的查询在物化视图定义时已重写，但还没有被规划器改写过。
	 */
	dataQuery = linitial_node(Query, actions);

	/*
	 * Check for active uses of the relation in the current transaction, such
	 * as open scans.
	 *
	 * 检查当前事务中是否仍在使用该关系，例如打开的扫描。
	 *
	 * NB: We count on this to protect us against problems with refreshing the
	 * data using TABLE_INSERT_FROZEN.
	 *
	 * 注意：我们靠这一点来避免用 TABLE_INSERT_FROZEN 刷新数据时出问题。
	 */
	CheckTableNotInUse(matviewRel,
					   is_create ? "CREATE MATERIALIZED VIEW" :
					   "REFRESH MATERIALIZED VIEW");

	/*
	 * Tentatively mark the matview as populated or not (this will roll back
	 * if we fail later).
	 *
	 * 先把物化视图标为已填充或未填充（若后面失败会回滚）。
	 */
	SetMatViewPopulatedState(matviewRel, !skipData);

	/* Concurrent refresh builds new data in temp tablespace, and does diff. */
	/*
	 *
	 * 并发刷新在临时表空间中生成新数据，并做差异比较。
	 */
	if (concurrent)
	{
		tableSpace = GetDefaultTablespace(RELPERSISTENCE_TEMP, false);
		relpersistence = RELPERSISTENCE_TEMP;
	}
	else
	{
		tableSpace = matviewRel->rd_rel->reltablespace;
		relpersistence = matviewRel->rd_rel->relpersistence;
	}

	/*
	 * Create the transient table that will receive the regenerated data. Lock
	 * it against access by any other process until commit (by which time it
	 * will be gone).
	 *
	 * 创建承接重新生成数据的临时表。锁住它直到提交，期间其他进程不能访问（提交时它已不存在）。
	 */
	OIDNewHeap = make_new_heap(matviewOid, tableSpace,
							   matviewRel->rd_rel->relam,
							   relpersistence, ExclusiveLock);
	Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock, false));

	/* Generate the data, if wanted. */
	/*
	 *
	 * 若需要则生成数据。
	 */
	if (!skipData)
	{
		DestReceiver *dest;

		dest = CreateTransientRelDestReceiver(OIDNewHeap);
		processed = refresh_matview_datafill(dest, dataQuery, queryString,
											 is_create);
	}

	/* Make the matview match the newly generated data. */
	/*
	 *
	 * 使物化视图与新生成的数据一致。
	 */
	if (concurrent)
	{
		int			old_depth = matview_maintenance_depth;

		PG_TRY();
		{
			refresh_by_match_merge(matviewOid, OIDNewHeap, relowner,
								   save_sec_context);
		}
		PG_CATCH();
		{
			matview_maintenance_depth = old_depth;
			PG_RE_THROW();
		}
		PG_END_TRY();
		Assert(matview_maintenance_depth == old_depth);
	}
	else
	{
		refresh_by_heap_swap(matviewOid, OIDNewHeap, relpersistence);

		/*
		 * Inform cumulative stats system about our activity: basically, we
		 * truncated the matview and inserted some new data.  (The concurrent
		 * code path above doesn't need to worry about this because the
		 * inserts and deletes it issues get counted by lower-level code.)
		 *
		 * 把本次活动告诉累积统计：基本上是截断物化视图并插入新数据。
		 * 上面的并发路径不必操心，因为它发出的插入和删除会由下层代码计数。
		 */
		pgstat_count_truncate(matviewRel);
		if (!skipData)
			pgstat_count_heap_insert(matviewRel, processed);
	}

	table_close(matviewRel, NoLock);

	/* Roll back any GUC changes */
	/*
	 *
	 * 回滚任何 GUC 变更。
	 */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	/*
	 *
	 * 恢复用户 ID 和安全上下文。
	 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	ObjectAddressSet(address, RelationRelationId, matviewOid);

	/*
	 * Save the rowcount so that pg_stat_statements can track the total number
	 * of rows processed by REFRESH MATERIALIZED VIEW command. Note that we
	 * still don't display the rowcount in the command completion tag output,
	 * i.e., the display_rowcount flag of CMDTAG_REFRESH_MATERIALIZED_VIEW
	 * command tag is left false in cmdtaglist.h. Otherwise, the change of
	 * completion tag output might break applications using it.
	 *
	 * 保存行数，以便 pg_stat_statements 统计 REFRESH MATERIALIZED VIEW 处理的总行数。
	 * 命令完成标签仍不显示行数，即 cmdtaglist.h 里 CMDTAG_REFRESH_MATERIALIZED_VIEW 的 display_rowcount 保持为 false。
	 * 否则完成标签输出的变化可能破坏依赖它的应用。
	 *
	 * When called from CREATE MATERIALIZED VIEW command, the rowcount is
	 * displayed with the command tag CMDTAG_SELECT.
	 *
	 * 从 CREATE MATERIALIZED VIEW 调用时，行数随命令标签 CMDTAG_SELECT 显示。
	 */
	if (qc)
		SetQueryCompletion(qc,
						   is_create ? CMDTAG_SELECT : CMDTAG_REFRESH_MATERIALIZED_VIEW,
						   processed);

	return address;
}

/*
 * refresh_matview_datafill
 *
 * refresh_matview_datafill。
 *
 * Execute the given query, sending result rows to "dest" (which will
 * insert them into the target matview).
 *
 * 执行给定查询，把结果行送给 dest（由它插入目标物化视图）。
 *
 * Returns number of rows inserted.
 *
 * 返回插入的行数。
 */
static uint64
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString, bool is_create)
{
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Query	   *copied_query;
	uint64		processed;

	/* Lock and rewrite, using a copy to preserve the original query. */
	/*
	 *
	 * 加锁并重写，使用副本以保留原始查询。
	 */
	copied_query = copyObject(query);
	AcquireRewriteLocks(copied_query, true, false);
	rewritten = QueryRewrite(copied_query);

	/* SELECT should never rewrite to more or less than one SELECT query */
	/*
	 *
	 * SELECT 重写后不应变成多于或少于一条 SELECT。
	 */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for %s",
			 is_create ? "CREATE MATERIALIZED VIEW " : "REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* Check for user-requested abort. */
	/*
	 *
	 * 检查用户是否请求中止。
	 */
	CHECK_FOR_INTERRUPTS();

	/* Plan the query which will generate data for the refresh. */
	/*
	 *
	 * 为生成刷新数据的查询做规划。
	 */
	plan = pg_plan_query(query, queryString, CURSOR_OPT_PARALLEL_OK, NULL);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.  (This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be safe.)
	 *
	 * 使用命令 ID 已更新的快照，使本查询能看到先前已执行查询的结果。
	 * 只有规划器执行了声称稳定却改了数据库的函数时才要紧，但为安全仍然这样做。
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	/*
	 *
	 * 创建 QueryDesc，把输出重定向到我们的元组接收器。
	 */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, NULL, 0);

	/* call ExecutorStart to prepare the plan for execution */
	/*
	 *
	 * 调用 ExecutorStart，准备执行计划。
	 */
	ExecutorStart(queryDesc, 0);

	/* run the plan */
	/*
	 *
	 * 运行计划。
	 */
	ExecutorRun(queryDesc, ForwardScanDirection, 0);

	processed = queryDesc->estate->es_processed;

	/* and clean up */
	/*
	 *
	 * 然后清理。
	 */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	return processed;
}

/*
 * 创建把查询结果写入临时堆的 DestReceiver。
 */
DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid)
{
	DR_transientrel *self = (DR_transientrel *) palloc0(sizeof(DR_transientrel));

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- executor startup
 *
 * transientrel_startup：执行器启动。
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation	transientrel;

	transientrel = table_open(myState->transientoid, NoLock);

	/*
	 * Fill private fields of myState for use by later routines
	 *
	 * 填充 myState 的私有字段，供后续例程使用。
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM | TABLE_INSERT_FROZEN;
	myState->bistate = GetBulkInsertState();

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 *
	 * 有效的 smgr_targblock 表示已有东西写过该关系。这也许无害，但本函数没有为此做准备。
	 */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 *
 * transientrel_receive：接收一个元组。
 */
static bool
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	/*
	 * Note that the input slot might not be of the type of the target
	 * relation. That's supported by table_tuple_insert(), but slightly less
	 * efficient than inserting with the right slot - but the alternative
	 * would be to copy into a slot of the right type, which would not be
	 * cheap either. This also doesn't allow accessing per-AM data (say a
	 * tuple's xmin), but since we don't do that here...
	 *
	 * 输入 slot 的类型可能不是目标关系的类型。table_tuple_insert() 支持这种情况，
	 * 但比用正确类型的 slot 插入稍慢；另一种办法是复制到正确类型的 slot，同样不便宜。
	 * 这样也无法访问访问方法私有数据（例如元组的 xmin），但这里并不需要。
	 */

	table_tuple_insert(myState->transientrel,
					   slot,
					   myState->output_cid,
					   myState->ti_options,
					   myState->bistate);

	/* We know this is a newly created relation, so there are no indexes */
	/*
	 *
	 * 已知这是新建的关系，因此没有索引。
	 */

	return true;
}

/*
 * transientrel_shutdown --- executor end
 *
 * transientrel_shutdown：执行器结束。
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	table_finish_bulk_insert(myState->transientrel, myState->ti_options);

	/* close transientrel, but keep lock until commit */
	/*
	 *
	 * 关闭 transientrel，但把锁保持到提交。
	 */
	table_close(myState->transientrel, NoLock);
	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 *
 * transientrel_destroy：释放 DestReceiver 对象。
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}


/*
 * Given a qualified temporary table name, append an underscore followed by
 * the given integer, to make a new table name based on the old one.
 * The result is a palloc'd string.
 *
 * 给定限定的临时表名，追加下划线和给定整数，据此生成新表名。结果是 palloc 分配的字符串。
 *
 * As coded, this would fail to make a valid SQL name if the given name were,
 * say, "FOO"."BAR".  Currently, the table name portion of the input will
 * never be double-quoted because it's of the form "pg_temp_NNN", cf
 * make_new_heap().  But we might have to work harder someday.
 *
 * 按当前写法，若给定名称是 "FOO"."BAR" 这样，就无法生成合法 SQL 名。
 * 目前输入的表名部分不会被双引号括起，因为它形如 pg_temp_NNN，见 make_new_heap()。
 * 将来也许要处理得更仔细。
 */
static char *
make_temptable_name_n(char *tempname, int n)
{
	StringInfoData namebuf;

	initStringInfo(&namebuf);
	appendStringInfoString(&namebuf, tempname);
	appendStringInfo(&namebuf, "_%d", n);
	return namebuf.data;
}

/*
 * refresh_by_match_merge
 *
 * refresh_by_match_merge。
 *
 * Refresh a materialized view with transactional semantics, while allowing
 * concurrent reads.
 *
 * 以事务语义刷新物化视图，同时允许并发读。
 *
 * This is called after a new version of the data has been created in a
 * temporary table.  It performs a full outer join against the old version of
 * the data, producing "diff" results.  This join cannot work if there are any
 * duplicated rows in either the old or new versions, in the sense that every
 * column would compare as equal between the two rows.  It does work correctly
 * in the face of rows which have at least one NULL value, with all non-NULL
 * columns equal.  The behavior of NULLs on equality tests and on UNIQUE
 * indexes turns out to be quite convenient here; the tests we need to make
 * are consistent with default behavior.  If there is at least one UNIQUE
 * index on the materialized view, we have exactly the guarantee we need.
 *
 * 在临时表中生成新版本数据后调用。它对旧数据做全外连接，得到 diff。
 * 若新旧版本中存在所有列都相等的重复行，该连接无法工作。
 * 对至少有一个 NULL、其余非 NULL 列都相等的行则能正确处理。
 * NULL 在等值测试和 UNIQUE 索引上的行为恰好符合这里需要的默认语义。
 * 物化视图上只要有至少一个 UNIQUE 索引，就有所需的保证。
 *
 * The temporary table used to hold the diff results contains just the TID of
 * the old record (if matched) and the ROW from the new table as a single
 * column of complex record type (if matched).
 *
 * 存放 diff 的临时表只包含旧记录的 TID（若匹配）以及新表的 ROW，后者是单个复杂记录类型列（若匹配）。
 *
 * Once we have the diff table, we perform set-based DELETE and INSERT
 * operations against the materialized view, and discard both temporary
 * tables.
 *
 * 有了 diff 表之后，对物化视图做基于集合的 DELETE 和 INSERT，并丢弃两张临时表。
 *
 * Everything from the generation of the new data to applying the differences
 * takes place under cover of an ExclusiveLock, since it seems as though we
 * would want to prohibit not only concurrent REFRESH operations, but also
 * incremental maintenance.  It also doesn't seem reasonable or safe to allow
 * SELECT FOR UPDATE or SELECT FOR SHARE on rows being updated or deleted by
 * this command.
 *
 * 从生成新数据到应用差异，全程持有 ExclusiveLock。这样不仅禁止并发 REFRESH，也禁止增量维护。
 * 对本命令正在更新或删除的行允许 SELECT FOR UPDATE 或 SELECT FOR SHARE 也不合理、不安全。
 */
static void
refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
					   int save_sec_context)
{
	StringInfoData querybuf;
	Relation	matviewRel;
	Relation	tempRel;
	char	   *matviewname;
	char	   *tempname;
	char	   *diffname;
	TupleDesc	tupdesc;
	bool		foundUniqueIndex;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int16		relnatts;
	Oid		   *opUsedForQual;

	initStringInfo(&querybuf);
	matviewRel = table_open(matviewOid, NoLock);
	matviewname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
											 RelationGetRelationName(matviewRel));
	tempRel = table_open(tempOid, NoLock);
	tempname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(tempRel)),
										  RelationGetRelationName(tempRel));
	diffname = make_temptable_name_n(tempname, 2);

	relnatts = RelationGetNumberOfAttributes(matviewRel);

	/* Open SPI context. */
	/*
	 *
	 * 打开 SPI 上下文。
	 */
	SPI_connect();

	/* Analyze the temp table with the new contents. */
	/*
	 *
	 * 对装有新内容的临时表做 ANALYZE。
	 */
	appendStringInfo(&querybuf, "ANALYZE %s", tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We need to ensure that there are not duplicate rows without NULLs in
	 * the new data set before we can count on the "diff" results.  Check for
	 * that in a way that allows showing the first duplicated row found.  Even
	 * after we pass this test, a unique index on the materialized view may
	 * find a duplicate key problem.
	 *
	 * 在依赖 diff 结果之前，必须确认新数据集中没有不含 NULL 的重复行，并以能展示第一行重复数据的方式检查。
	 * 即使通过该检查，物化视图上的唯一索引仍可能发现重复键。
	 *
	 * Note: here and below, we use "tablename.*::tablerowtype" as a hack to
	 * keep ".*" from being expanded into multiple columns in a SELECT list.
	 * Compare ruleutils.c's get_variable().
	 *
	 * 注意：这里及下面用 tablename.*::tablerowtype 这种写法，避免 SELECT 列表里的 .* 被展开成多列。
	 * 对照 ruleutils.c 的 get_variable()。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "SELECT newdata.*::%s FROM %s newdata "
					 "WHERE newdata.* IS NOT NULL AND EXISTS "
					 "(SELECT 1 FROM %s newdata2 WHERE newdata2.* IS NOT NULL "
					 "AND newdata2.* OPERATOR(pg_catalog.*=) newdata.* "
					 "AND newdata2.ctid OPERATOR(pg_catalog.<>) "
					 "newdata.ctid)",
					 tempname, tempname, tempname);
	if (SPI_execute(querybuf.data, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	if (SPI_processed > 0)
	{
		/*
		 * Note that this ereport() is returning data to the user.  Generally,
		 * we would want to make sure that the user has been granted access to
		 * this data.  However, REFRESH MAT VIEW is only able to be run by the
		 * owner of the mat view (or a superuser) and therefore there is no
		 * need to check for access to data in the mat view.
		 *
		 * 这个 ereport() 会把数据返回给用户。通常应确认用户已被授予访问权。
		 * 但 REFRESH MATERIALIZED VIEW 只能由物化视图属主或超级用户执行，因此不必再检查对视图数据的访问权。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("new data for materialized view \"%s\" contains duplicate rows without any null columns",
						RelationGetRelationName(matviewRel)),
				 errdetail("Row: %s",
						   SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1))));
	}

	/*
	 * Create the temporary "diff" table.
	 *
	 * 创建临时 diff 表。
	 *
	 * Temporarily switch out of the SECURITY_RESTRICTED_OPERATION context,
	 * because you cannot create temp tables in SRO context.  For extra
	 * paranoia, add the composite type column only after switching back to
	 * SRO context.
	 *
	 * 暂时离开 SECURITY_RESTRICTED_OPERATION 上下文，因为该上下文中不能创建临时表。
	 * 为更谨慎，回到该上下文之后才添加复合类型列。
	 */
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "CREATE TEMP TABLE %s (tid pg_catalog.tid)",
					 diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "ALTER TABLE %s ADD COLUMN newdata %s",
					 diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Start building the query for populating the diff table. */
	/*
	 *
	 * 开始构造填充 diff 表的查询。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s "
					 "SELECT mv.ctid AS tid, newdata.*::%s AS newdata "
					 "FROM %s mv FULL JOIN %s newdata ON (",
					 diffname, tempname, matviewname, tempname);

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache.  We will test for equality on all
	 * columns present in all unique indexes which only reference columns and
	 * include all rows.
	 *
	 * 从 relcache 取得表的索引 OID 列表，并在 pg_index 的 syscache 中逐个查找。
	 * 将对所有只引用列且包含全部行的唯一索引中的列做等值测试。
	 */
	tupdesc = matviewRel->rd_att;
	opUsedForQual = (Oid *) palloc0(sizeof(Oid) * relnatts);
	foundUniqueIndex = false;

	indexoidlist = RelationGetIndexList(matviewRel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;

		indexRel = index_open(indexoid, RowExclusiveLock);
		if (is_usable_unique_index(indexRel))
		{
			Form_pg_index indexStruct = indexRel->rd_index;
			int			indnkeyatts = indexStruct->indnkeyatts;
			oidvector  *indclass;
			Datum		indclassDatum;
			int			i;

			/* Must get indclass the hard way. */
			/*
			 *
			 * 必须用较麻烦的方式取得 indclass。
			 */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID,
												   indexRel->rd_indextuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/* Add quals for all columns from this index. */
			/*
			 *
			 * 为该索引的所有列添加 qual。
			 */
			for (i = 0; i < indnkeyatts; i++)
			{
				int			attnum = indexStruct->indkey.values[i];
				Oid			opclass = indclass->values[i];
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
				Oid			attrtype = attr->atttypid;
				HeapTuple	cla_ht;
				Form_pg_opclass cla_tup;
				Oid			opfamily;
				Oid			opcintype;
				Oid			op;
				const char *leftop;
				const char *rightop;

				/*
				 * Identify the equality operator associated with this index
				 * column.  First we need to look up the column's opclass.
				 *
				 * 确定与该索引列关联的等值运算符。首先要查找该列的 opclass。
				 */
				cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
				if (!HeapTupleIsValid(cla_ht))
					elog(ERROR, "cache lookup failed for opclass %u", opclass);
				cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
				opfamily = cla_tup->opcfamily;
				opcintype = cla_tup->opcintype;
				ReleaseSysCache(cla_ht);

				op = get_opfamily_member_for_cmptype(opfamily, opcintype, opcintype, COMPARE_EQ);
				if (!OidIsValid(op))
					elog(ERROR, "missing equality operator for (%u,%u) in opfamily %u",
						 opcintype, opcintype, opfamily);

				/*
				 * If we find the same column with the same equality semantics
				 * in more than one index, we only need to emit the equality
				 * clause once.
				 *
				 * 若多个索引中同一列具有相同的等值语义，等值子句只生成一次。
				 *
				 * Since we only remember the last equality operator, this
				 * code could be fooled into emitting duplicate clauses given
				 * multiple indexes with several different opclasses ... but
				 * that's so unlikely it doesn't seem worth spending extra
				 * code to avoid.
				 *
				 * 因为只记住最后一个等值运算符，多个索引使用不同 opclass 时可能生成重复子句。
				 * 这种情况极不可能，不值得另写代码去避免。
				 */
				if (opUsedForQual[attnum - 1] == op)
					continue;
				opUsedForQual[attnum - 1] = op;

				/*
				 * Actually add the qual, ANDed with any others.
				 *
				 * 真正加入该 qual，并与其他 qual 做 AND。
				 */
				if (foundUniqueIndex)
					appendStringInfoString(&querybuf, " AND ");

				leftop = quote_qualified_identifier("newdata",
													NameStr(attr->attname));
				rightop = quote_qualified_identifier("mv",
													 NameStr(attr->attname));

				generate_operator_clause(&querybuf,
										 leftop, attrtype,
										 op,
										 rightop, attrtype);

				foundUniqueIndex = true;
			}
		}

		/* Keep the locks, since we're about to run DML which needs them. */
		/*
		 *
		 * 保留这些锁，因为接下来的 DML 需要它们。
		 */
		index_close(indexRel, NoLock);
	}

	list_free(indexoidlist);

	/*
	 * There must be at least one usable unique index on the matview.
	 *
	 * 物化视图上必须至少有一个可用的唯一索引。
	 *
	 * ExecRefreshMatView() checks that after taking the exclusive lock on the
	 * matview. So at least one unique index is guaranteed to exist here
	 * because the lock is still being held.  (One known exception is if a
	 * function called as part of refreshing the matview drops the index.
	 * That's a pretty silly thing to do.)
	 *
	 * ExecRefreshMatView() 在取得物化视图排他锁之后做了该检查。锁仍持有，因此这里保证至少有一个唯一索引。
	 * 已知例外是刷新过程中调用的函数删掉了索引，那样做很荒唐。
	 */
	if (!foundUniqueIndex)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("could not find suitable unique index on materialized view \"%s\"",
					   RelationGetRelationName(matviewRel)));

	appendStringInfoString(&querybuf,
						   " AND newdata.* OPERATOR(pg_catalog.*=) mv.*) "
						   "WHERE newdata.* IS NULL OR mv.* IS NULL "
						   "ORDER BY tid");

	/* Populate the temporary "diff" table. */
	/*
	 *
	 * 填充临时 diff 表。
	 */
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We have no further use for data from the "full-data" temp table, but we
	 * must keep it around because its type is referenced from the diff table.
	 *
	 * 不再需要全量临时表里的数据，但必须留着它，因为 diff 表引用了它的类型。
	 */

	/* Analyze the diff table. */
	/*
	 *
	 * 对 diff 表做 ANALYZE。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "ANALYZE %s", diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	OpenMatViewIncrementalMaintenance();

	/* Deletes must come before inserts; do them first. */
	/*
	 *
	 * 删除必须在插入之前，先做删除。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "DELETE FROM %s mv WHERE ctid OPERATOR(pg_catalog.=) ANY "
					 "(SELECT diff.tid FROM %s diff "
					 "WHERE diff.tid IS NOT NULL "
					 "AND diff.newdata IS NULL)",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_DELETE)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Inserts go last. */
	/*
	 *
	 * 插入放在最后。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s SELECT (diff.newdata).* "
					 "FROM %s diff WHERE tid IS NULL",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* We're done maintaining the materialized view. */
	/*
	 *
	 * 物化视图维护到此结束。
	 */
	CloseMatViewIncrementalMaintenance();
	table_close(tempRel, NoLock);
	table_close(matviewRel, NoLock);

	/* Clean up temp tables. */
	/*
	 *
	 * 清理临时表。
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "DROP TABLE %s, %s", diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Close SPI context. */
	/*
	 *
	 * 关闭 SPI 上下文。
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

/*
 * Swap the physical files of the target and transient tables, then rebuild
 * the target's indexes and throw away the transient table.  Security context
 * swapping is handled by the called function, so it is not needed here.
 *
 * 交换目标表与临时表的物理文件，然后重建目标索引并丢弃临时表。
 * 安全上下文的切换由被调函数处理，这里不需要。
 */
static void
refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence)
{
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true,
					 RecentXmin, ReadNextMultiXactId(), relpersistence);
}

/*
 * Check whether specified index is usable for match merge.
 *
 * 检查指定索引能否用于匹配合并。
 */
static bool
is_usable_unique_index(Relation indexRel)
{
	Form_pg_index indexStruct = indexRel->rd_index;

	/*
	 * Must be unique, valid, immediate, non-partial, and be defined over
	 * plain user columns (not expressions).
	 *
	 * 必须是唯一、有效、immediate、非部分索引，并且定义在普通用户列上（不能是表达式）。
	 */
	if (indexStruct->indisunique &&
		indexStruct->indimmediate &&
		indexStruct->indisvalid &&
		RelationGetIndexPredicate(indexRel) == NIL &&
		indexStruct->indnatts > 0)
	{
		/*
		 * The point of groveling through the index columns individually is to
		 * reject both index expressions and system columns.  Currently,
		 * matviews couldn't have OID columns so there's no way to create an
		 * index on a system column; but maybe someday that wouldn't be true,
		 * so let's be safe.
		 *
		 * 逐列检查索引是为了同时拒绝索引表达式和系统列。目前物化视图不能有 OID 列，因此无法在系统列上建索引；
		 * 但将来未必如此，所以仍然谨慎处理。
		 */
		int			numatts = indexStruct->indnatts;
		int			i;

		for (i = 0; i < numatts; i++)
		{
			int			attnum = indexStruct->indkey.values[i];

			if (attnum <= 0)
				return false;
		}
		return true;
	}
	return false;
}


/*
 * This should be used to test whether the backend is in a context where it is
 * OK to allow DML statements to modify materialized views.  We only want to
 * allow that for internal code driven by the materialized view definition,
 * not for arbitrary user-supplied code.
 *
 * 用于判断后端是否处于允许 DML 修改物化视图的上下文。只允许由物化视图定义驱动的内部代码这样做，不允许任意用户代码。
 *
 * While the function names reflect the fact that their main intended use is
 * incremental maintenance of materialized views (in response to changes to
 * the data in referenced relations), they are initially used to allow REFRESH
 * without blocking concurrent reads.
 *
 * 函数名反映其主要用途是物化视图的增量维护（响应被引用关系的数据变化），
 * 但最初用来允许 REFRESH 时不阻塞并发读。
 */
bool
MatViewIncrementalMaintenanceIsEnabled(void)
{
	return matview_maintenance_depth > 0;
}

/*
 * 进入物化视图维护上下文，递增 matview_maintenance_depth。
 */
static void
OpenMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth++;
}

/*
 * 离开物化视图维护上下文，递减 matview_maintenance_depth。
 */
static void
CloseMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth--;
	Assert(matview_maintenance_depth >= 0);
}
