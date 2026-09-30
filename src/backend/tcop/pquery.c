/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/pquery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "commands/prepare.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * ActivePortal is the currently executing Portal (the most closely nested,
 * if there are several).
 *
 * ActivePortal 是当前正在执行的 Portal；如果存在多个嵌套 Portal，
 * 它指向嵌套最深的那个。
 */
Portal		ActivePortal = NULL;


static void ProcessQuery(PlannedStmt *plan,
						 const char *sourceText,
						 ParamListInfo params,
						 QueryEnvironment *queryEnv,
						 DestReceiver *dest,
						 QueryCompletion *qc);
static void FillPortalStore(Portal portal, bool isTopLevel);
static uint64 RunFromStore(Portal portal, ScanDirection direction, uint64 count,
						   DestReceiver *dest);
static uint64 PortalRunSelect(Portal portal, bool forward, long count,
							  DestReceiver *dest);
static void PortalRunUtility(Portal portal, PlannedStmt *pstmt,
							 bool isTopLevel, bool setHoldSnapshot,
							 DestReceiver *dest, QueryCompletion *qc);
static void PortalRunMulti(Portal portal,
						   bool isTopLevel, bool setHoldSnapshot,
						   DestReceiver *dest, DestReceiver *altdest,
						   QueryCompletion *qc);
static uint64 DoPortalRunFetch(Portal portal,
							   FetchDirection fdirection,
							   long count,
							   DestReceiver *dest);
static void DoPortalRewind(Portal portal);


/*
 * CreateQueryDesc
 *
 * 创建 QueryDesc，记录执行计划、快照、目标接收器和参数等执行所需状态。
 * 主要流程是分配结构体、注册快照、填充字段，并把执行器稍后设置的字段清空。
 */
QueryDesc *
CreateQueryDesc(PlannedStmt *plannedstmt,
				const char *sourceText,
				Snapshot snapshot,
				Snapshot crosscheck_snapshot,
				DestReceiver *dest,
				ParamListInfo params,
				QueryEnvironment *queryEnv,
				int instrument_options)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = plannedstmt->commandType;	/* operation
												 *
												 * 操作类型 */
	qd->plannedstmt = plannedstmt;	/* plan
									 *
									 * 执行计划 */
	qd->sourceText = sourceText;	/* query text
								 *
								 * 查询文本 */
	qd->snapshot = RegisterSnapshot(snapshot);	/* snapshot
												 *
												 * 快照 */
	/*
	 * RI check snapshot
	 *
	 * RI 检查快照
	 */
	qd->crosscheck_snapshot = RegisterSnapshot(crosscheck_snapshot);
	qd->dest = dest;			/* output dest
								 *
								 * 输出目标 */
	qd->params = params;		/* parameter values passed into query
								 *
								 * 传入查询的参数值 */
	qd->queryEnv = queryEnv;
	qd->instrument_options = instrument_options;	/* instrumentation wanted?
												 *
												 * 是否需要性能检测 */

	/*
	 * null these fields until set by ExecutorStart
	 *
	 * 在 ExecutorStart 设置这些字段之前，将它们置为空。
	 */
	qd->tupDesc = NULL;
	qd->estate = NULL;
	qd->planstate = NULL;
	qd->totaltime = NULL;

	/*
	 * not yet executed
	 *
	 * 尚未执行。
	 */
	qd->already_executed = false;

	return qd;
}

/*
 * FreeQueryDesc
 *
 * 释放 QueryDesc 持有的资源。主要流程是确认查询已不再执行、
 * 注销快照，然后释放 QueryDesc 本身。
 */
void
FreeQueryDesc(QueryDesc *qdesc)
{
	/*
	 * Can't be a live query
	 *
	 * 不能是仍在执行的查询。
	 */
	Assert(qdesc->estate == NULL);

	/*
	 * forget our snapshots
	 *
	 * 注销本对象持有的快照。
	 */
	UnregisterSnapshot(qdesc->snapshot);
	UnregisterSnapshot(qdesc->crosscheck_snapshot);

	/*
	 * Only the QueryDesc itself need be freed
	 *
	 * 只需要释放 QueryDesc 本身。
	 */
	pfree(qdesc);
}


/*
 * ProcessQuery
 *		Execute a single plannable query within a PORTAL_MULTI_QUERY,
 *		PORTAL_ONE_RETURNING, or PORTAL_ONE_MOD_WITH portal
 *
 *	plan: the plan tree for the query
 *	sourceText: the source text of the query
 *	params: any parameters needed
 *	dest: where to send results
 *	qc: where to store the command completion status data.
 *
 * qc may be NULL if caller doesn't want a status string.
 *
 * 如果调用者不需要状态字符串，qc 可以为 NULL。
 *
 * Must be called in a memory context that will be reset or deleted on
 * error; otherwise the executor's memory usage will be leaked.
 *
 * 必须在出错时会被重置或删除的内存上下文中调用；
 * 否则执行器使用的内存会泄漏。
 */
static void
ProcessQuery(PlannedStmt *plan,
			 const char *sourceText,
			 ParamListInfo params,
			 QueryEnvironment *queryEnv,
			 DestReceiver *dest,
			 QueryCompletion *qc)
{
	QueryDesc  *queryDesc;

	/*
	 * Create the QueryDesc object
	 *
	 * 创建 QueryDesc 对象。
	 */
	queryDesc = CreateQueryDesc(plan, sourceText,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, queryEnv, 0);

	/*
	 * Call ExecutorStart to prepare the plan for execution
	 *
	 * 调用 ExecutorStart，为执行计划做准备。
	 */
	ExecutorStart(queryDesc, 0);

	/*
	 * Run the plan to completion.
	 *
	 * 将计划执行到完成。
	 */
	ExecutorRun(queryDesc, ForwardScanDirection, 0);

	/*
	 * Build command completion status data, if caller wants one.
	 *
	 * 如果调用者需要，则构造命令完成状态数据。
	 */
	if (qc)
	{
		switch (queryDesc->operation)
		{
			case CMD_SELECT:
				SetQueryCompletion(qc, CMDTAG_SELECT, queryDesc->estate->es_processed);
				break;
			case CMD_INSERT:
				SetQueryCompletion(qc, CMDTAG_INSERT, queryDesc->estate->es_processed);
				break;
			case CMD_UPDATE:
				SetQueryCompletion(qc, CMDTAG_UPDATE, queryDesc->estate->es_processed);
				break;
			case CMD_DELETE:
				SetQueryCompletion(qc, CMDTAG_DELETE, queryDesc->estate->es_processed);
				break;
			case CMD_MERGE:
				SetQueryCompletion(qc, CMDTAG_MERGE, queryDesc->estate->es_processed);
				break;
			default:
				SetQueryCompletion(qc, CMDTAG_UNKNOWN, queryDesc->estate->es_processed);
				break;
		}
	}

	/*
	 * Now, we close down all the scans and free allocated resources.
	 *
	 * 现在关闭所有扫描并释放已分配的资源。
	 */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);
}

/*
 * ChoosePortalStrategy
 *		Select portal execution strategy given the intended statement list.
 *
 * The list elements can be Querys or PlannedStmts.
 * That's more general than portals need, but plancache.c uses this too.
 *
 * 列表元素可以是 Query 或 PlannedStmt。
 * 这比 Portal 所需的更通用，但 plancache.c 也会使用它。
 *
 * See the comments in portal.h.
 *
 * 参见 portal.h 中的注释。
 */
PortalStrategy
ChoosePortalStrategy(List *stmts)
{
	int			nSetTag;
	ListCell   *lc;

	/*
	 * PORTAL_ONE_SELECT and PORTAL_UTIL_SELECT need only consider the
	 * single-statement case, since there are no rewrite rules that can add
	 * auxiliary queries to a SELECT or a utility command. PORTAL_ONE_MOD_WITH
	 * likewise allows only one top-level statement.
	 *
	 * PORTAL_ONE_SELECT 和 PORTAL_UTIL_SELECT 只需考虑单语句情形，
	 * 因为没有重写规则会给 SELECT 或 utility 命令添加辅助查询。
	 * PORTAL_ONE_MOD_WITH 同样只允许一个顶层语句。
	 */
	if (list_length(stmts) == 1)
	{
		Node	   *stmt = (Node *) linitial(stmts);

		if (IsA(stmt, Query))
		{
			Query	   *query = (Query *) stmt;

			if (query->canSetTag)
			{
				if (query->commandType == CMD_SELECT)
				{
					if (query->hasModifyingCTE)
						return PORTAL_ONE_MOD_WITH;
					else
						return PORTAL_ONE_SELECT;
				}
				if (query->commandType == CMD_UTILITY)
				{
					if (UtilityReturnsTuples(query->utilityStmt))
						return PORTAL_UTIL_SELECT;
					/* it can't be ONE_RETURNING, so give up
					 *
					 * 它不可能是 ONE_RETURNING，因此放弃。 */
					return PORTAL_MULTI_QUERY;
				}
			}
		}
		else if (IsA(stmt, PlannedStmt))
		{
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			if (pstmt->canSetTag)
			{
				if (pstmt->commandType == CMD_SELECT)
				{
					if (pstmt->hasModifyingCTE)
						return PORTAL_ONE_MOD_WITH;
					else
						return PORTAL_ONE_SELECT;
				}
				if (pstmt->commandType == CMD_UTILITY)
				{
					if (UtilityReturnsTuples(pstmt->utilityStmt))
						return PORTAL_UTIL_SELECT;
					/* it can't be ONE_RETURNING, so give up
					 *
					 * 它不可能是 ONE_RETURNING，因此放弃。 */
					return PORTAL_MULTI_QUERY;
				}
			}
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
	}

	/*
	 * PORTAL_ONE_RETURNING has to allow auxiliary queries added by rewrite.
	 * Choose PORTAL_ONE_RETURNING if there is exactly one canSetTag query and
	 * it has a RETURNING list.
	 *
	 * PORTAL_ONE_RETURNING 必须允许由重写添加的辅助查询。
	 * 如果恰好有一个 canSetTag 查询并且它带有 RETURNING 列表，
	 * 则选择 PORTAL_ONE_RETURNING。
	 */
	nSetTag = 0;
	foreach(lc, stmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		if (IsA(stmt, Query))
		{
			Query	   *query = (Query *) stmt;

			if (query->canSetTag)
			{
				if (++nSetTag > 1)
					return PORTAL_MULTI_QUERY;	/* no need to look further
												 *
												 * 无需继续查看。 */
				if (query->commandType == CMD_UTILITY ||
					query->returningList == NIL)
					return PORTAL_MULTI_QUERY;	/* no need to look further
												 *
												 * 无需继续查看。 */
			}
		}
		else if (IsA(stmt, PlannedStmt))
		{
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			if (pstmt->canSetTag)
			{
				if (++nSetTag > 1)
					return PORTAL_MULTI_QUERY;	/* no need to look further
												 *
												 * 无需继续查看。 */
				if (pstmt->commandType == CMD_UTILITY ||
					!pstmt->hasReturning)
					return PORTAL_MULTI_QUERY;	/* no need to look further
												 *
												 * 无需继续查看。 */
			}
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
	}
	if (nSetTag == 1)
		return PORTAL_ONE_RETURNING;

	/*
	 * Else, it's the general case...
	 *
	 * 否则就是通用情形。
	 */
	return PORTAL_MULTI_QUERY;
}

/*
 * FetchPortalTargetList
 *		Given a portal that returns tuples, extract the query targetlist.
 *		Returns NIL if the portal doesn't have a determinable targetlist.
 *
 *		给定一个会返回元组的 Portal，提取查询的目标列表。
 *		如果该 Portal 没有可确定的目标列表，则返回 NIL。
 *
 * Note: do not modify the result.
 *
 * 注意：不要修改返回结果。
 */
List *
FetchPortalTargetList(Portal portal)
{
	/*
	 * no point in looking if we determined it doesn't return tuples
	 *
	 * 如果已确定它不会返回元组，就没有必要继续查看。
	 */
	if (portal->strategy == PORTAL_MULTI_QUERY)
		return NIL;
	/*
	 * get the primary statement and find out what it returns
	 *
	 * 取得主语句并确定它返回什么。
	 */
	return FetchStatementTargetList((Node *) PortalGetPrimaryStmt(portal));
}

/*
 * FetchStatementTargetList
 *		Given a statement that returns tuples, extract the query targetlist.
 *		Returns NIL if the statement doesn't have a determinable targetlist.
 *
 *		给定一个会返回元组的语句，提取查询的目标列表。
 *		如果该语句没有可确定的目标列表，则返回 NIL。
 *
 * This can be applied to a Query or a PlannedStmt.
 * That's more general than portals need, but plancache.c uses this too.
 *
 * 这可应用于 Query 或 PlannedStmt。
 * 这比 Portal 所需的更通用，但 plancache.c 也会使用它。
 *
 * Note: do not modify the result.
 *
 * 注意：不要修改返回结果。
 *
 * XXX be careful to keep this in sync with UtilityReturnsTuples.
 *
 * XXX 注意保持它与 UtilityReturnsTuples 同步。
 */
List *
FetchStatementTargetList(Node *stmt)
{
	if (stmt == NULL)
		return NIL;
	if (IsA(stmt, Query))
	{
		Query	   *query = (Query *) stmt;

		if (query->commandType == CMD_UTILITY)
		{
			/*
			 * transfer attention to utility statement
			 *
			 * 将关注点转到 utility 语句。
			 */
			stmt = query->utilityStmt;
		}
		else
		{
			if (query->commandType == CMD_SELECT)
				return query->targetList;
			if (query->returningList)
				return query->returningList;
			return NIL;
		}
	}
	if (IsA(stmt, PlannedStmt))
	{
		PlannedStmt *pstmt = (PlannedStmt *) stmt;

		if (pstmt->commandType == CMD_UTILITY)
		{
			/*
			 * transfer attention to utility statement
			 *
			 * 将关注点转到 utility 语句。
			 */
			stmt = pstmt->utilityStmt;
		}
		else
		{
			if (pstmt->commandType == CMD_SELECT)
				return pstmt->planTree->targetlist;
			if (pstmt->hasReturning)
				return pstmt->planTree->targetlist;
			return NIL;
		}
	}
	if (IsA(stmt, FetchStmt))
	{
		FetchStmt  *fstmt = (FetchStmt *) stmt;
		Portal		subportal;

		Assert(!fstmt->ismove);
		subportal = GetPortalByName(fstmt->portalname);
		Assert(PortalIsValid(subportal));
		return FetchPortalTargetList(subportal);
	}
	if (IsA(stmt, ExecuteStmt))
	{
		ExecuteStmt *estmt = (ExecuteStmt *) stmt;
		PreparedStatement *entry;

		entry = FetchPreparedStatement(estmt->name, true);
		return FetchPreparedStatementTargetList(entry);
	}
	return NIL;
}

/*
 * PortalStart
 *		Prepare a portal for execution.
 *
 *		准备一个 Portal 以便执行。
 *
 * Caller must already have created the portal, done PortalDefineQuery(),
 * and adjusted portal options if needed.
 *
 * 调用者必须已经创建 Portal、完成 PortalDefineQuery()，
 * 并在需要时调整 Portal 选项。
 *
 * If parameters are needed by the query, they must be passed in "params"
 * (caller is responsible for giving them appropriate lifetime).
 *
 * 如果查询需要参数，必须通过 "params" 传入
 * （调用者负责保证这些参数有合适的生命周期）。
 *
 * The caller can also provide an initial set of "eflags" to be passed to
 * ExecutorStart (but note these can be modified internally, and they are
 * currently only honored for PORTAL_ONE_SELECT portals).  Most callers
 * should simply pass zero.
 *
 * 调用者也可以提供一组初始 "eflags" 传给 ExecutorStart
 * （但注意这些标志可能在内部被修改，并且目前只对
 * PORTAL_ONE_SELECT Portal 生效）。大多数调用者应直接传入零。
 *
 * The caller can optionally pass a snapshot to be used; pass InvalidSnapshot
 * for the normal behavior of setting a new snapshot.  This parameter is
 * presently ignored for non-PORTAL_ONE_SELECT portals (it's only intended
 * to be used for cursors).
 *
 * 调用者可以选择传入要使用的快照；传入 InvalidSnapshot 表示采用
 * 设置新快照的常规行为。目前该参数会被非 PORTAL_ONE_SELECT Portal
 * 忽略（它只打算用于游标）。
 *
 * On return, portal is ready to accept PortalRun() calls, and the result
 * tupdesc (if any) is known.
 *
 * 返回时，Portal 已准备好接受 PortalRun() 调用，并且结果 tupdesc
 * （如果有）已经确定。
 */
void
PortalStart(Portal portal, ParamListInfo params,
			int eflags, Snapshot snapshot)
{
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldContext;
	QueryDesc  *queryDesc;
	int			myeflags;

	Assert(PortalIsValid(portal));
	Assert(portal->status == PORTAL_DEFINED);

	/*
	 * Set up global portal context pointers.
	 *
	 * 设置全局 Portal 上下文指针。
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		oldContext = MemoryContextSwitchTo(PortalContext);

		/*
		 * Must remember portal param list, if any
		 *
		 * 如果存在 Portal 参数列表，必须记住它。
		 */
		portal->portalParams = params;

		/*
		 * Determine the portal execution strategy
		 *
		 * 确定 Portal 的执行策略。
		 */
		portal->strategy = ChoosePortalStrategy(portal->stmts);

		/*
		 * Fire her up according to the strategy
		 *
		 * 按照策略启动执行。
		 */
		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:

				/*
				 * Must set snapshot before starting executor.
				 *
				 * 启动执行器之前必须设置快照。
				 */
				if (snapshot)
					PushActiveSnapshot(snapshot);
				else
					PushActiveSnapshot(GetTransactionSnapshot());

				/*
				 * We could remember the snapshot in portal->portalSnapshot,
				 * but presently there seems no need to, as this code path
				 * cannot be used for non-atomic execution.  Hence there can't
				 * be any commit/abort that might destroy the snapshot.  Since
				 * we don't do that, there's also no need to force a
				 * non-default nesting level for the snapshot.
				 *
				 * 我们可以把快照记在 portal->portalSnapshot 中，
				 * 但目前似乎没有必要，因为这条代码路径不能用于非原子执行。
				 * 因此不会有可能销毁该快照的提交/中止。既然不这么做，
				 * 也就没有必要为该快照强制使用非默认嵌套层级。
				 */

				/*
				 * Create QueryDesc in portal's context; for the moment, set
				 * the destination to DestNone.
				 *
				 * 在 Portal 的上下文中创建 QueryDesc；暂时把目标设置为
				 * DestNone。
				 */
				queryDesc = CreateQueryDesc(linitial_node(PlannedStmt, portal->stmts),
											portal->sourceText,
											GetActiveSnapshot(),
											InvalidSnapshot,
											None_Receiver,
											params,
											portal->queryEnv,
											0);

				/*
				 * If it's a scrollable cursor, executor needs to support
				 * REWIND and backwards scan, as well as whatever the caller
				 * might've asked for.
				 *
				 * 如果这是可滚动游标，执行器需要支持 REWIND 和反向扫描，
				 * 同时还要支持调用者可能请求的其他能力。
				 */
				if (portal->cursorOptions & CURSOR_OPT_SCROLL)
					myeflags = eflags | EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD;
				else
					myeflags = eflags;

				/*
				 * Call ExecutorStart to prepare the plan for execution
				 *
				 * 调用 ExecutorStart，为执行计划做准备。
				 */
				ExecutorStart(queryDesc, myeflags);

				/*
				 * This tells PortalCleanup to shut down the executor
				 *
				 * 这会告知 PortalCleanup 关闭执行器。
				 */
				portal->queryDesc = queryDesc;

				/*
				 * Remember tuple descriptor (computed by ExecutorStart)
				 *
				 * 记住元组描述符（由 ExecutorStart 计算）。
				 */
				portal->tupDesc = queryDesc->tupDesc;

				/*
				 * Reset cursor position data to "start of query"
				 *
				 * 将游标位置数据重置为“查询起点”。
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* allow fetches
										 *
										 * 允许抓取。 */
				portal->portalPos = 0;

				PopActiveSnapshot();
				break;

			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:

				/*
				 * We don't start the executor until we are told to run the
				 * portal.  We do need to set up the result tupdesc.
				 *
				 * 在被要求运行 Portal 之前，我们不会启动执行器。
				 * 但确实需要设置结果 tupdesc。
				 */
				{
					PlannedStmt *pstmt;

					pstmt = PortalGetPrimaryStmt(portal);
					portal->tupDesc =
						ExecCleanTypeFromTL(pstmt->planTree->targetlist);
				}

				/*
				 * Reset cursor position data to "start of query"
				 *
				 * 将游标位置数据重置为“查询起点”。
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* allow fetches
										 *
										 * 允许抓取。 */
				portal->portalPos = 0;
				break;

			case PORTAL_UTIL_SELECT:

				/*
				 * We don't set snapshot here, because PortalRunUtility will
				 * take care of it if needed.
				 *
				 * 这里不设置快照，因为如果需要，PortalRunUtility 会处理它。
				 */
				{
					PlannedStmt *pstmt = PortalGetPrimaryStmt(portal);

					Assert(pstmt->commandType == CMD_UTILITY);
					portal->tupDesc = UtilityTupleDescriptor(pstmt->utilityStmt);
				}

				/*
				 * Reset cursor position data to "start of query"
				 *
				 * 将游标位置数据重置为“查询起点”。
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* allow fetches
										 *
										 * 允许抓取。 */
				portal->portalPos = 0;
				break;

			case PORTAL_MULTI_QUERY:
				/*
				 * Need do nothing now
				 *
				 * 现在无需做任何事。
				 */
				portal->tupDesc = NULL;
				break;
		}
	}
	PG_CATCH();
	{
		/*
		 * Uncaught error while executing portal: mark it dead
		 *
		 * 执行 Portal 时出现未捕获错误：将其标记为失效。
		 */
		MarkPortalFailed(portal);

		/*
		 * Restore global vars and propagate error
		 *
		 * 恢复全局变量并继续抛出错误。
		 */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldContext);

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	portal->status = PORTAL_READY;
}

/*
 * PortalSetResultFormat
 *		Select the format codes for a portal's output.
 *
 *		为 Portal 的输出选择格式代码。
 *
 * This must be run after PortalStart for a portal that will be read by
 * a DestRemote or DestRemoteExecute destination.  It is not presently needed
 * for other destination types.
 *
 * 对于将由 DestRemote 或 DestRemoteExecute 目标读取的 Portal，
 * 必须在 PortalStart 之后运行它。目前其他目标类型不需要它。
 *
 * formats[] is the client format request, as per Bind message conventions.
 *
 * formats[] 是客户端格式请求，遵循 Bind 消息约定。
 */
void
PortalSetResultFormat(Portal portal, int nFormats, int16 *formats)
{
	int			natts;
	int			i;

	/*
	 * Do nothing if portal won't return tuples
	 *
	 * 如果 Portal 不会返回元组，则什么也不做。
	 */
	if (portal->tupDesc == NULL)
		return;
	natts = portal->tupDesc->natts;
	portal->formats = (int16 *)
		MemoryContextAlloc(portal->portalContext,
						   natts * sizeof(int16));
	if (nFormats > 1)
	{
		/*
		 * format specified for each column
		 *
		 * 为每一列分别指定了格式。
		 */
		if (nFormats != natts)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("bind message has %d result formats but query has %d columns",
							nFormats, natts)));
		memcpy(portal->formats, formats, natts * sizeof(int16));
	}
	else if (nFormats > 0)
	{
		/*
		 * single format specified, use for all columns
		 *
		 * 指定了单一格式，将其用于所有列。
		 */
		int16		format1 = formats[0];

		for (i = 0; i < natts; i++)
			portal->formats[i] = format1;
	}
	else
	{
		/*
		 * use default format for all columns
		 *
		 * 对所有列使用默认格式。
		 */
		for (i = 0; i < natts; i++)
			portal->formats[i] = 0;
	}
}

/*
 * PortalRun
 *		Run a portal's query or queries.
 *
 *		运行 Portal 中的一个或多个查询。
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".  Note that count is ignored in multi-query
 * situations, where we always run the portal to completion.
 *
 * count <= 0 会解释为空操作：目标会被启动并关闭，但不会发生其他事情。
 * 同时，count == FETCH_ALL 会解释为“所有行”。注意，在多查询情形下
 * count 会被忽略，因为我们总是把 Portal 运行到完成。
 *
 * isTopLevel: true if query is being executed at backend "top level"
 * (that is, directly from a client command message)
 *
 * isTopLevel：如果查询正在后端“顶层”执行（也就是直接来自客户端命令消息），
 * 则为 true。
 *
 * dest: where to send output of primary (canSetTag) query
 *
 * dest：主查询（canSetTag）的输出发送位置。
 *
 * altdest: where to send output of non-primary queries
 *
 * altdest：非主查询的输出发送位置。
 *
 * qc: where to store command completion status data.
 *		May be NULL if caller doesn't want status data.
 *
 * qc：存放命令完成状态数据的位置。
 *		如果调用者不需要状态数据，可以为 NULL。
 *
 * Returns true if the portal's execution is complete, false if it was
 * suspended due to exhaustion of the count parameter.
 *
 * 如果 Portal 执行已完成则返回 true；如果因为 count 参数耗尽而暂停，
 * 则返回 false。
 */
bool
PortalRun(Portal portal, long count, bool isTopLevel,
		  DestReceiver *dest, DestReceiver *altdest,
		  QueryCompletion *qc)
{
	bool		result;
	uint64		nprocessed;
	ResourceOwner saveTopTransactionResourceOwner;
	MemoryContext saveTopTransactionContext;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext saveMemoryContext;

	Assert(PortalIsValid(portal));

	TRACE_POSTGRESQL_QUERY_EXECUTE_START();

	/*
	 * Initialize empty completion data
	 *
	 * 初始化空的完成数据。
	 */
	if (qc)
		InitializeQueryCompletion(qc);

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
	{
		elog(DEBUG3, "PortalRun");
		/* PORTAL_MULTI_QUERY logs its own stats per query
		 *
		 * PORTAL_MULTI_QUERY 会为每个查询记录自己的统计信息。 */
		ResetUsage();
	}

	/*
	 * Check for improper portal use, and mark portal active.
	 *
	 * 检查 Portal 使用是否不当，并将 Portal 标记为活动状态。
	 */
	MarkPortalActive(portal);

	/*
	 * Set up global portal context pointers.
	 *
	 * We have to play a special game here to support utility commands like
	 * VACUUM and CLUSTER, which internally start and commit transactions.
	 * When we are called to execute such a command, CurrentResourceOwner will
	 * be pointing to the TopTransactionResourceOwner --- which will be
	 * destroyed and replaced in the course of the internal commit and
	 * restart.  So we need to be prepared to restore it as pointing to the
	 * exit-time TopTransactionResourceOwner.  (Ain't that ugly?  This idea of
	 * internally starting whole new transactions is not good.)
	 * CurrentMemoryContext has a similar problem, but the other pointers we
	 * save here will be NULL or pointing to longer-lived objects.
	 *
	 * 设置全局 Portal 上下文指针。
	 *
	 * 为了支持 VACUUM 和 CLUSTER 这类会在内部启动并提交事务的 utility 命令，
	 * 我们必须在这里做特殊处理。当我们被调用来执行这类命令时，
	 * CurrentResourceOwner 会指向 TopTransactionResourceOwner，
	 * 而它会在内部提交和重启过程中被销毁并替换。因此我们必须准备好把它恢复为
	 * 指向退出时的 TopTransactionResourceOwner。（这很难看；这种在内部启动
	 * 全新事务的想法并不好。）CurrentMemoryContext 也有类似问题，
	 * 但这里保存的其他指针要么是 NULL，要么指向生命周期更长的对象。
	 */
	saveTopTransactionResourceOwner = TopTransactionResourceOwner;
	saveTopTransactionContext = TopTransactionContext;
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	saveMemoryContext = CurrentMemoryContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		MemoryContextSwitchTo(PortalContext);

		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:
			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:
			case PORTAL_UTIL_SELECT:

				/*
				 * If we have not yet run the command, do so, storing its
				 * results in the portal's tuplestore.  But we don't do that
				 * for the PORTAL_ONE_SELECT case.
				 *
				 * 如果尚未运行该命令，就运行它，并把结果存入 Portal 的
				 * tuplestore。但对于 PORTAL_ONE_SELECT 情形不这样做。
				 */
				if (portal->strategy != PORTAL_ONE_SELECT && !portal->holdStore)
					FillPortalStore(portal, isTopLevel);

				/*
				 * Now fetch desired portion of results.
				 *
				 * 现在抓取所需的结果部分。
				 */
				nprocessed = PortalRunSelect(portal, true, count, dest);

				/*
				 * If the portal result contains a command tag and the caller
				 * gave us a pointer to store it, copy it and update the
				 * rowcount.
				 *
				 * 如果 Portal 结果包含命令标签，并且调用者给了用于存放它的指针，
				 * 则复制该标签并更新行数。
				 */
				if (qc && portal->qc.commandTag != CMDTAG_UNKNOWN)
				{
					CopyQueryCompletion(qc, &portal->qc);
					qc->nprocessed = nprocessed;
				}

				/*
				 * Mark portal not active
				 *
				 * 将 Portal 标记为非活动状态。
				 */
				portal->status = PORTAL_READY;

				/*
				 * Since it's a forward fetch, say DONE iff atEnd is now true.
				 *
				 * 因为这是正向抓取，只有当前 atEnd 为 true 时才报告完成。
				 */
				result = portal->atEnd;
				break;

			case PORTAL_MULTI_QUERY:
				PortalRunMulti(portal, isTopLevel, false,
							   dest, altdest, qc);

				/*
				 * Prevent portal's commands from being re-executed
				 *
				 * 防止 Portal 的命令被重新执行。
				 */
				MarkPortalDone(portal);

				/*
				 * Always complete at end of RunMulti
				 *
				 * RunMulti 结束时总是已完成。
				 */
				result = true;
				break;

			default:
				elog(ERROR, "unrecognized portal strategy: %d",
					 (int) portal->strategy);
				result = false; /* keep compiler quiet
								 *
								 * 让编译器保持安静。 */
				break;
		}
	}
	PG_CATCH();
	{
		/*
		 * Uncaught error while executing portal: mark it dead
		 *
		 * 执行 Portal 时出现未捕获错误：将其标记为失效。
		 */
		MarkPortalFailed(portal);

		/*
		 * Restore global vars and propagate error
		 *
		 * 恢复全局变量并继续抛出错误。
		 */
		if (saveMemoryContext == saveTopTransactionContext)
			MemoryContextSwitchTo(TopTransactionContext);
		else
			MemoryContextSwitchTo(saveMemoryContext);
		ActivePortal = saveActivePortal;
		if (saveResourceOwner == saveTopTransactionResourceOwner)
			CurrentResourceOwner = TopTransactionResourceOwner;
		else
			CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (saveMemoryContext == saveTopTransactionContext)
		MemoryContextSwitchTo(TopTransactionContext);
	else
		MemoryContextSwitchTo(saveMemoryContext);
	ActivePortal = saveActivePortal;
	if (saveResourceOwner == saveTopTransactionResourceOwner)
		CurrentResourceOwner = TopTransactionResourceOwner;
	else
		CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
		ShowUsage("EXECUTOR STATISTICS");

	TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();

	return result;
}

/*
 * PortalRunSelect
 *		Execute a portal's query in PORTAL_ONE_SELECT mode, and also
 *		when fetching from a completed holdStore in PORTAL_ONE_RETURNING,
 *		PORTAL_ONE_MOD_WITH, and PORTAL_UTIL_SELECT cases.
 *
 *		在 PORTAL_ONE_SELECT 模式下执行 Portal 的查询；也用于
 *		PORTAL_ONE_RETURNING、PORTAL_ONE_MOD_WITH 和 PORTAL_UTIL_SELECT
 *		情形中从已完成的 holdStore 抓取结果。
 *
 * This handles simple N-rows-forward-or-backward cases.  For more complex
 * nonsequential access to a portal, see PortalRunFetch.
 *
 * 它处理简单的向前或向后 N 行抓取情形。对于更复杂的 Portal 非顺序访问，
 * 请参见 PortalRunFetch。
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".  (cf FetchStmt.howMany)
 *
 * count <= 0 会解释为空操作：目标会被启动并关闭，但不会发生其他事情。
 * 同时，count == FETCH_ALL 会解释为“所有行”。（参见 FetchStmt.howMany）
 *
 * Caller must already have validated the Portal and done appropriate
 * setup (cf. PortalRun).
 *
 * 调用者必须已经验证 Portal 并完成适当设置（参见 PortalRun）。
 *
 * Returns number of rows processed (suitable for use in result tag)
 *
 * 返回已处理的行数（适合用于结果标签）。
 */
static uint64
PortalRunSelect(Portal portal,
				bool forward,
				long count,
				DestReceiver *dest)
{
	QueryDesc  *queryDesc;
	ScanDirection direction;
	uint64		nprocessed;

	/*
	 * NB: queryDesc will be NULL if we are fetching from a held cursor or a
	 * completed utility query; can't use it in that path.
	 *
	 * 注意：如果我们从保持游标或已完成的 utility 查询中抓取，
	 * queryDesc 将为 NULL；这条路径不能使用它。
	 */
	queryDesc = portal->queryDesc;

	/*
	 * Caller messed up if we have neither a ready query nor held data.
	 *
	 * 如果既没有就绪查询也没有保持的数据，则调用者出错了。
	 */
	Assert(queryDesc || portal->holdStore);

	/*
	 * Force the queryDesc destination to the right thing.  This supports
	 * MOVE, for example, which will pass in dest = DestNone.  This is okay to
	 * change as long as we do it on every fetch.  (The Executor must not
	 * assume that dest never changes.)
	 *
	 * 强制把 queryDesc 的目标设置为正确对象。例如 MOVE 会传入
	 * dest = DestNone。只要每次抓取都这样做，修改它就是可以的。
	 * （执行器不能假定 dest 永远不变。）
	 */
	if (queryDesc)
		queryDesc->dest = dest;

	/*
	 * Determine which direction to go in, and check to see if we're already
	 * at the end of the available tuples in that direction.  If so, set the
	 * direction to NoMovement to avoid trying to fetch any tuples.  (This
	 * check exists because not all plan node types are robust about being
	 * called again if they've already returned NULL once.)  Then call the
	 * executor (we must not skip this, because the destination needs to see a
	 * setup and shutdown even if no tuples are available).  Finally, update
	 * the portal position state depending on the number of tuples that were
	 * retrieved.
	 *
	 * 确定前进方向，并检查在该方向上是否已经位于可用元组的末端。
	 * 如果是，则把方向设置为 NoMovement，以避免尝试抓取任何元组。
	 * （存在此检查是因为并非所有计划节点类型都能稳健地处理已经返回过
	 * NULL 后再次被调用。）然后调用执行器（不能跳过此步骤，因为即使
	 * 没有可用元组，目标也需要看到启动和关闭过程）。最后，根据实际取得的
	 * 元组数量更新 Portal 位置状态。
	 */
	if (forward)
	{
		if (portal->atEnd || count <= 0)
		{
			direction = NoMovementScanDirection;
			count = 0;			/* don't pass negative count to executor
								 *
								 * 不要把负数 count 传给执行器。 */
		}
		else
			direction = ForwardScanDirection;

		/*
		 * In the executor, zero count processes all rows
		 *
		 * 在执行器中，count 为零表示处理所有行。
		 */
		if (count == FETCH_ALL)
			count = 0;

		if (portal->holdStore)
			nprocessed = RunFromStore(portal, direction, (uint64) count, dest);
		else
		{
			PushActiveSnapshot(queryDesc->snapshot);
			ExecutorRun(queryDesc, direction, (uint64) count);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
		}

		if (!ScanDirectionIsNoMovement(direction))
		{
			if (nprocessed > 0)
				portal->atStart = false;	/* OK to go backward now
											 *
											 * 现在可以向后移动。 */
			if (count == 0 || nprocessed < (uint64) count)
				portal->atEnd = true;	/* we retrieved 'em all
										 *
										 * 我们已经抓取了全部结果。 */
			portal->portalPos += nprocessed;
		}
	}
	else
	{
		if (portal->cursorOptions & CURSOR_OPT_NO_SCROLL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cursor can only scan forward"),
					 errhint("Declare it with SCROLL option to enable backward scan.")));

		if (portal->atStart || count <= 0)
		{
			direction = NoMovementScanDirection;
			count = 0;			/* don't pass negative count to executor
								 *
								 * 不要把负数 count 传给执行器。 */
		}
		else
			direction = BackwardScanDirection;

		/*
		 * In the executor, zero count processes all rows
		 *
		 * 在执行器中，count 为零表示处理所有行。
		 */
		if (count == FETCH_ALL)
			count = 0;

		if (portal->holdStore)
			nprocessed = RunFromStore(portal, direction, (uint64) count, dest);
		else
		{
			PushActiveSnapshot(queryDesc->snapshot);
			ExecutorRun(queryDesc, direction, (uint64) count);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
		}

		if (!ScanDirectionIsNoMovement(direction))
		{
			if (nprocessed > 0 && portal->atEnd)
			{
				portal->atEnd = false;	/* OK to go forward now
										 *
										 * 现在可以向前移动。 */
				portal->portalPos++;	/* adjust for endpoint case
										 *
										 * 针对端点情形进行调整。 */
			}
			if (count == 0 || nprocessed < (uint64) count)
			{
				portal->atStart = true; /* we retrieved 'em all
										 *
										 * 我们已经抓取了全部结果。 */
				portal->portalPos = 0;
			}
			else
			{
				portal->portalPos -= nprocessed;
			}
		}
	}

	return nprocessed;
}

/*
 * FillPortalStore
 *		Run the query and load result tuples into the portal's tuple store.
 *
 *		运行查询，并把结果元组装入 Portal 的元组存储。
 *
 * This is used for PORTAL_ONE_RETURNING, PORTAL_ONE_MOD_WITH, and
 * PORTAL_UTIL_SELECT cases only.
 *
 * 这只用于 PORTAL_ONE_RETURNING、PORTAL_ONE_MOD_WITH 和
 * PORTAL_UTIL_SELECT 情形。
 */
static void
FillPortalStore(Portal portal, bool isTopLevel)
{
	DestReceiver *treceiver;
	QueryCompletion qc;

	InitializeQueryCompletion(&qc);
	PortalCreateHoldStore(portal);
	treceiver = CreateDestReceiver(DestTuplestore);
	SetTuplestoreDestReceiverParams(treceiver,
									portal->holdStore,
									portal->holdContext,
									false,
									NULL,
									NULL);

	switch (portal->strategy)
	{
		case PORTAL_ONE_RETURNING:
		case PORTAL_ONE_MOD_WITH:

			/*
			 * Run the portal to completion just as for the default
			 * PORTAL_MULTI_QUERY case, but send the primary query's output to
			 * the tuplestore.  Auxiliary query outputs are discarded. Set the
			 * portal's holdSnapshot to the snapshot used (or a copy of it).
			 *
			 * 像默认的 PORTAL_MULTI_QUERY 情形一样把 Portal 运行到完成，
			 * 但把主查询的输出发送到 tuplestore。辅助查询的输出会被丢弃。
			 * 将 Portal 的 holdSnapshot 设置为所用快照（或它的副本）。
			 */
			PortalRunMulti(portal, isTopLevel, true,
						   treceiver, None_Receiver, &qc);
			break;

		case PORTAL_UTIL_SELECT:
			PortalRunUtility(portal, linitial_node(PlannedStmt, portal->stmts),
							 isTopLevel, true, treceiver, &qc);
			break;

		default:
			elog(ERROR, "unsupported portal strategy: %d",
				 (int) portal->strategy);
			break;
	}

	/*
	 * Override portal completion data with actual command results
	 *
	 * 用实际命令结果覆盖 Portal 完成数据。
	 */
	if (qc.commandTag != CMDTAG_UNKNOWN)
		CopyQueryCompletion(&portal->qc, &qc);

	treceiver->rDestroy(treceiver);
}

/*
 * RunFromStore
 *		Fetch tuples from the portal's tuple store.
 *
 *		从 Portal 的元组存储中抓取元组。
 *
 * Calling conventions are similar to ExecutorRun, except that we
 * do not depend on having a queryDesc or estate.  Therefore we return the
 * number of tuples processed as the result, not in estate->es_processed.
 *
 * 调用约定类似 ExecutorRun，只是我们不依赖 queryDesc 或 estate。
 * 因此我们把已处理元组数作为结果返回，而不是放在 estate->es_processed 中。
 *
 * One difference from ExecutorRun is that the destination receiver functions
 * are run in the caller's memory context (since we have no estate).  Watch
 * out for memory leaks.
 *
 * 与 ExecutorRun 的一个差异是，目标接收器函数会在调用者的内存上下文中运行
 * （因为我们没有 estate）。注意避免内存泄漏。
 */
static uint64
RunFromStore(Portal portal, ScanDirection direction, uint64 count,
			 DestReceiver *dest)
{
	uint64		current_tuple_count = 0;
	TupleTableSlot *slot;

	slot = MakeSingleTupleTableSlot(portal->tupDesc, &TTSOpsMinimalTuple);

	dest->rStartup(dest, CMD_SELECT, portal->tupDesc);

	if (ScanDirectionIsNoMovement(direction))
	{
		/*
		 * do nothing except start/stop the destination
		 *
		 * 除了启动/关闭目标外什么也不做。
		 */
	}
	else
	{
		bool		forward = ScanDirectionIsForward(direction);

		for (;;)
		{
			MemoryContext oldcontext;
			bool		ok;

			oldcontext = MemoryContextSwitchTo(portal->holdContext);

			ok = tuplestore_gettupleslot(portal->holdStore, forward, false,
										 slot);

			MemoryContextSwitchTo(oldcontext);

			if (!ok)
				break;

			/*
			 * If we are not able to send the tuple, we assume the destination
			 * has closed and no more tuples can be sent. If that's the case,
			 * end the loop.
			 *
			 * 如果无法发送元组，则假定目标已经关闭，不能再发送更多元组。
			 * 如果是这种情况，就结束循环。
			 */
			if (!dest->receiveSlot(slot, dest))
				break;

			ExecClearTuple(slot);

			/*
			 * check our tuple count.. if we've processed the proper number
			 * then quit, else loop again and process more tuples. Zero count
			 * means no limit.
			 *
			 * 检查元组计数；如果已处理了正确数量，就退出；否则继续循环处理更多元组。
			 * count 为零表示没有限制。
			 */
			current_tuple_count++;
			if (count && count == current_tuple_count)
				break;
		}
	}

	dest->rShutdown(dest);

	ExecDropSingleTupleTableSlot(slot);

	return current_tuple_count;
}

/*
 * PortalRunUtility
 *		Execute a utility statement inside a portal.
 *
 *		在 Portal 内执行 utility 语句。
 *		主要流程是按需建立快照，调用 ProcessUtility，然后恢复内存上下文并弹出快照。
 */
static void
PortalRunUtility(Portal portal, PlannedStmt *pstmt,
				 bool isTopLevel, bool setHoldSnapshot,
				 DestReceiver *dest, QueryCompletion *qc)
{
	/*
	 * Set snapshot if utility stmt needs one.
	 *
	 * 如果 utility 语句需要快照，则设置快照。
	 */
	if (PlannedStmtRequiresSnapshot(pstmt))
	{
		Snapshot	snapshot = GetTransactionSnapshot();

		/*
		 * If told to, register the snapshot we're using and save in portal
		 *
		 * 如果被要求这样做，注册正在使用的快照并保存到 Portal 中。
		 */
		if (setHoldSnapshot)
		{
			snapshot = RegisterSnapshot(snapshot);
			portal->holdSnapshot = snapshot;
		}

		/*
		 * In any case, make the snapshot active and remember it in portal.
		 * Because the portal now references the snapshot, we must tell
		 * snapmgr.c that the snapshot belongs to the portal's transaction
		 * level, else we risk portalSnapshot becoming a dangling pointer.
		 *
		 * 无论如何，都要使该快照成为活动快照并记入 Portal。
		 * 因为 Portal 现在引用该快照，所以必须告诉 snapmgr.c
		 * 该快照属于 Portal 的事务层级，否则 portalSnapshot 有变成悬垂指针的风险。
		 */
		PushActiveSnapshotWithLevel(snapshot, portal->createLevel);
		/*
		 * PushActiveSnapshotWithLevel might have copied the snapshot
		 *
		 * PushActiveSnapshotWithLevel 可能已经复制了快照。
		 */
		portal->portalSnapshot = GetActiveSnapshot();
	}
	else
		portal->portalSnapshot = NULL;

	ProcessUtility(pstmt,
				   portal->sourceText,
				   (portal->cplan != NULL), /* protect tree if in plancache
											 *
											 * 如果在计划缓存中，则保护树。 */
				   isTopLevel ? PROCESS_UTILITY_TOPLEVEL : PROCESS_UTILITY_QUERY,
				   portal->portalParams,
				   portal->queryEnv,
				   dest,
				   qc);

	/*
	 * Some utility statements may change context on us
	 *
	 * 有些 utility 语句可能会改变当前上下文。
	 */
	MemoryContextSwitchTo(portal->portalContext);

	/*
	 * Some utility commands (e.g., VACUUM) pop the ActiveSnapshot stack from
	 * under us, so don't complain if it's now empty.  Otherwise, our snapshot
	 * should be the top one; pop it.  Note that this could be a different
	 * snapshot from the one we made above; see EnsurePortalSnapshotExists.
	 *
	 * 一些 utility 命令（例如 VACUUM）可能会从我们下面弹出 ActiveSnapshot 栈，
	 * 所以如果它现在为空，不要报错。否则，我们的快照应当位于栈顶；弹出它。
	 * 注意，这可能与上面创建的快照不同；参见 EnsurePortalSnapshotExists。
	 */
	if (portal->portalSnapshot != NULL && ActiveSnapshotSet())
	{
		Assert(portal->portalSnapshot == GetActiveSnapshot());
		PopActiveSnapshot();
	}
	portal->portalSnapshot = NULL;
}

/*
 * PortalRunMulti
 *		Execute a portal's queries in the general case (multi queries
 *		or non-SELECT-like queries)
 *
 *		在通用情形下执行 Portal 的查询（多查询或非 SELECT 类查询）。
 *		主要流程是逐个执行 PlannedStmt，按需维护活动快照和命令计数器，
 *		并把主查询或辅助查询的输出发送到相应目标。
 */
static void
PortalRunMulti(Portal portal,
			   bool isTopLevel, bool setHoldSnapshot,
			   DestReceiver *dest, DestReceiver *altdest,
			   QueryCompletion *qc)
{
	bool		active_snapshot_set = false;
	ListCell   *stmtlist_item;

	/*
	 * If the destination is DestRemoteExecute, change to DestNone.  The
	 * reason is that the client won't be expecting any tuples, and indeed has
	 * no way to know what they are, since there is no provision for Describe
	 * to send a RowDescription message when this portal execution strategy is
	 * in effect.  This presently will only affect SELECT commands added to
	 * non-SELECT queries by rewrite rules: such commands will be executed,
	 * but the results will be discarded unless you use "simple Query"
	 * protocol.
	 *
	 * 如果目标是 DestRemoteExecute，则改为 DestNone。原因是客户端不会期待任何元组，
	 * 而且也无法知道它们是什么，因为当此 Portal 执行策略生效时，
	 * Describe 没有机制发送 RowDescription 消息。目前这只会影响由重写规则添加到
	 * 非 SELECT 查询中的 SELECT 命令：这些命令会被执行，但除非使用
	 * "simple Query" 协议，否则结果会被丢弃。
	 */
	if (dest->mydest == DestRemoteExecute)
		dest = None_Receiver;
	if (altdest->mydest == DestRemoteExecute)
		altdest = None_Receiver;

	/*
	 * Loop to handle the individual queries generated from a single parsetree
	 * by analysis and rewrite.
	 *
	 * 循环处理由分析和重写从单个解析树生成的各个查询。
	 */
	foreach(stmtlist_item, portal->stmts)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, stmtlist_item);

		/*
		 * If we got a cancel signal in prior command, quit
		 *
		 * 如果前一个命令收到了取消信号，则退出。
		 */
		CHECK_FOR_INTERRUPTS();

		if (pstmt->utilityStmt == NULL)
		{
			/*
			 * process a plannable query.
			 *
			 * 处理可规划查询。
			 */
			TRACE_POSTGRESQL_QUERY_EXECUTE_START();

			if (log_executor_stats)
				ResetUsage();

			/*
			 * Must always have a snapshot for plannable queries.  First time
			 * through, take a new snapshot; for subsequent queries in the
			 * same portal, just update the snapshot's copy of the command
			 * counter.
			 *
			 * 可规划查询必须始终有快照。第一次经过时取得新快照；
			 * 对同一 Portal 中的后续查询，只更新该快照副本中的命令计数器。
			 */
			if (!active_snapshot_set)
			{
				Snapshot	snapshot = GetTransactionSnapshot();

				/*
				 * If told to, register the snapshot and save in portal
				 *
				 * 如果被要求这样做，注册快照并保存到 Portal 中。
				 */
				if (setHoldSnapshot)
				{
					snapshot = RegisterSnapshot(snapshot);
					portal->holdSnapshot = snapshot;
				}

				/*
				 * We can't have the holdSnapshot also be the active one,
				 * because UpdateActiveSnapshotCommandId would complain.  So
				 * force an extra snapshot copy.  Plain PushActiveSnapshot
				 * would have copied the transaction snapshot anyway, so this
				 * only adds a copy step when setHoldSnapshot is true.  (It's
				 * okay for the command ID of the active snapshot to diverge
				 * from what holdSnapshot has.)
				 *
				 * holdSnapshot 不能同时作为活动快照，因为
				 * UpdateActiveSnapshotCommandId 会报错。因此强制额外复制一份快照。
				 * 普通 PushActiveSnapshot 本来也会复制事务快照，所以只有在
				 * setHoldSnapshot 为 true 时，这才会额外增加一个复制步骤。
				 * （活动快照的命令 ID 与 holdSnapshot 的命令 ID 不同是可以的。）
				 */
				PushCopiedSnapshot(snapshot);

				/*
				 * As for PORTAL_ONE_SELECT portals, it does not seem
				 * necessary to maintain portal->portalSnapshot here.
				 *
				 * 与 PORTAL_ONE_SELECT Portal 一样，这里似乎没有必要维护
				 * portal->portalSnapshot。
				 */

				active_snapshot_set = true;
			}
			else
				UpdateActiveSnapshotCommandId();

			if (pstmt->canSetTag)
			{
				/* statement can set tag string
				 *
				 * 语句可以设置标签字符串。 */
				ProcessQuery(pstmt,
							 portal->sourceText,
							 portal->portalParams,
							 portal->queryEnv,
							 dest, qc);
			}
			else
			{
				/* stmt added by rewrite cannot set tag
				 *
				 * 由重写添加的语句不能设置标签。 */
				ProcessQuery(pstmt,
							 portal->sourceText,
							 portal->portalParams,
							 portal->queryEnv,
							 altdest, NULL);
			}

			if (log_executor_stats)
				ShowUsage("EXECUTOR STATISTICS");

			TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();
		}
		else
		{
			/*
			 * process utility functions (create, destroy, etc..)
			 *
			 * We must not set a snapshot here for utility commands (if one is
			 * needed, PortalRunUtility will do it).  If a utility command is
			 * alone in a portal then everything's fine.  The only case where
			 * a utility command can be part of a longer list is that rules
			 * are allowed to include NotifyStmt.  NotifyStmt doesn't care
			 * whether it has a snapshot or not, so we just leave the current
			 * snapshot alone if we have one.
			 *
			 * 处理 utility 函数（创建、销毁等）。
			 *
			 * 这里不能为 utility 命令设置快照（如果需要快照，
			 * PortalRunUtility 会处理）。如果一个 utility 命令单独位于 Portal 中，
			 * 一切都没有问题。utility 命令可能成为更长列表一部分的唯一情况是，
			 * 规则允许包含 NotifyStmt。NotifyStmt 不关心是否有快照，
			 * 所以如果当前已有快照，我们就保持不变。
			 */
			if (pstmt->canSetTag)
			{
				Assert(!active_snapshot_set);
				/* statement can set tag string
				 *
				 * 语句可以设置标签字符串。 */
				PortalRunUtility(portal, pstmt, isTopLevel, false,
								 dest, qc);
			}
			else
			{
				Assert(IsA(pstmt->utilityStmt, NotifyStmt));
				/* stmt added by rewrite cannot set tag
				 *
				 * 由重写添加的语句不能设置标签。 */
				PortalRunUtility(portal, pstmt, isTopLevel, false,
								 altdest, NULL);
			}
		}

		/*
		 * Clear subsidiary contexts to recover temporary memory.
		 *
		 * 清理子上下文以回收临时内存。
		 */
		Assert(portal->portalContext == CurrentMemoryContext);

		MemoryContextDeleteChildren(portal->portalContext);

		/*
		 * Avoid crashing if portal->stmts has been reset.  This can only
		 * occur if a CALL or DO utility statement executed an internal
		 * COMMIT/ROLLBACK (cf PortalReleaseCachedPlan).  The CALL or DO must
		 * have been the only statement in the portal, so there's nothing left
		 * for us to do; but we don't want to dereference a now-dangling list
		 * pointer.
		 *
		 * 避免在 portal->stmts 被重置时崩溃。这只能发生在 CALL 或 DO utility
		 * 语句执行了内部 COMMIT/ROLLBACK 时（参见 PortalReleaseCachedPlan）。
		 * CALL 或 DO 必须是 Portal 中唯一的语句，所以已经没有剩余工作；
		 * 但我们不想解引用现在已经悬垂的列表指针。
		 */
		if (portal->stmts == NIL)
			break;

		/*
		 * Increment command counter between queries, but not after the last
		 * one.
		 *
		 * 在查询之间递增命令计数器，但不要在最后一个查询之后递增。
		 */
		if (lnext(portal->stmts, stmtlist_item) != NULL)
			CommandCounterIncrement();
	}

	/*
	 * Pop the snapshot if we pushed one.
	 *
	 * 如果压入过快照，则将其弹出。
	 */
	if (active_snapshot_set)
		PopActiveSnapshot();

	/*
	 * If a command tag was requested and we did not fill in a run-time-
	 * determined tag above, copy the parse-time tag from the Portal.  (There
	 * might not be any tag there either, in edge cases such as empty prepared
	 * statements.  That's OK.)
	 *
	 * 如果请求了命令标签，而上面没有填入运行时确定的标签，
	 * 则从 Portal 复制解析时标签。（在空预备语句等边缘情况下，
	 * 那里也可能没有任何标签。这没问题。）
	 */
	if (qc &&
		qc->commandTag == CMDTAG_UNKNOWN &&
		portal->qc.commandTag != CMDTAG_UNKNOWN)
		CopyQueryCompletion(qc, &portal->qc);
}

/*
 * PortalRunFetch
 *		Variant form of PortalRun that supports SQL FETCH directions.
 *
 *		PortalRun 的变体形式，支持 SQL FETCH 方向。
 *
 * Note: we presently assume that no callers of this want isTopLevel = true.
 *
 * 注意：目前我们假定它的调用者都不需要 isTopLevel = true。
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".  (cf FetchStmt.howMany)
 *
 * count <= 0 会解释为空操作：目标会被启动并关闭，但不会发生其他事情。
 * 同时，count == FETCH_ALL 会解释为“所有行”。（参见 FetchStmt.howMany）
 *
 * Returns number of rows processed (suitable for use in result tag)
 *
 * 返回已处理的行数（适合用于结果标签）。
 */
uint64
PortalRunFetch(Portal portal,
			   FetchDirection fdirection,
			   long count,
			   DestReceiver *dest)
{
	uint64		result;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldContext;

	Assert(PortalIsValid(portal));

	/*
	 * Check for improper portal use, and mark portal active.
	 *
	 * 检查 Portal 使用是否不当，并将 Portal 标记为活动状态。
	 */
	MarkPortalActive(portal);

	/*
	 * Set up global portal context pointers.
	 *
	 * 设置全局 Portal 上下文指针。
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		oldContext = MemoryContextSwitchTo(PortalContext);

		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:
				result = DoPortalRunFetch(portal, fdirection, count, dest);
				break;

			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:
			case PORTAL_UTIL_SELECT:

				/*
				 * If we have not yet run the command, do so, storing its
				 * results in the portal's tuplestore.
				 *
				 * 如果尚未运行该命令，就运行它，并把结果存入 Portal 的 tuplestore。
				 */
				if (!portal->holdStore)
					FillPortalStore(portal, false /* isTopLevel
												   *
												   * 是否为顶层执行。 */ );

				/*
				 * Now fetch desired portion of results.
				 *
				 * 现在抓取所需的结果部分。
				 */
				result = DoPortalRunFetch(portal, fdirection, count, dest);
				break;

			default:
				elog(ERROR, "unsupported portal strategy");
				result = 0;		/* keep compiler quiet
								 *
								 * 让编译器保持安静。 */
				break;
		}
	}
	PG_CATCH();
	{
		/*
		 * Uncaught error while executing portal: mark it dead
		 *
		 * 执行 Portal 时出现未捕获错误：将其标记为失效。
		 */
		MarkPortalFailed(portal);

		/*
		 * Restore global vars and propagate error
		 *
		 * 恢复全局变量并继续抛出错误。
		 */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldContext);

	/*
	 * Mark portal not active
	 *
	 * 将 Portal 标记为非活动状态。
	 */
	portal->status = PORTAL_READY;

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	return result;
}

/*
 * DoPortalRunFetch
 *		Guts of PortalRunFetch --- the portal context is already set up
 *
 *		PortalRunFetch 的核心实现，调用时 Portal 上下文已经设置好。
 *
 * Here, count < 0 typically reverses the direction.  Also, count == FETCH_ALL
 * is interpreted as "all rows".  (cf FetchStmt.howMany)
 *
 * 在这里，count < 0 通常会反转方向。同时，count == FETCH_ALL 会解释为
 * “所有行”。（参见 FetchStmt.howMany）
 *
 * Returns number of rows processed (suitable for use in result tag)
 *
 * 返回已处理的行数（适合用于结果标签）。
 */
static uint64
DoPortalRunFetch(Portal portal,
				 FetchDirection fdirection,
				 long count,
				 DestReceiver *dest)
{
	bool		forward;

	Assert(portal->strategy == PORTAL_ONE_SELECT ||
		   portal->strategy == PORTAL_ONE_RETURNING ||
		   portal->strategy == PORTAL_ONE_MOD_WITH ||
		   portal->strategy == PORTAL_UTIL_SELECT);

	/*
	 * Note: we disallow backwards fetch (including re-fetch of current row)
	 * for NO SCROLL cursors, but we interpret that very loosely: you can use
	 * any of the FetchDirection options, so long as the end result is to move
	 * forwards by at least one row.  Currently it's sufficient to check for
	 * NO SCROLL in DoPortalRewind() and in the forward == false path in
	 * PortalRunSelect(); but someday we might prefer to account for that
	 * restriction explicitly here.
	 *
	 * 注意：我们禁止 NO SCROLL 游标进行向后抓取（包括重新抓取当前行），
	 * 但对此的解释很宽松：可以使用任意 FetchDirection 选项，
	 * 只要最终结果是至少向前移动一行即可。目前在 DoPortalRewind()
	 * 和 PortalRunSelect() 中 forward == false 的路径里检查 NO SCROLL 就足够了；
	 * 但将来我们可能更愿意在这里显式考虑该限制。
	 */
	switch (fdirection)
	{
		case FETCH_FORWARD:
			if (count < 0)
			{
				fdirection = FETCH_BACKWARD;
				count = -count;
			}
			/* fall out of switch to share code with FETCH_BACKWARD
			 *
			 * 跳出 switch，以便与 FETCH_BACKWARD 共享代码。 */
			break;
		case FETCH_BACKWARD:
			if (count < 0)
			{
				fdirection = FETCH_FORWARD;
				count = -count;
			}
			/* fall out of switch to share code with FETCH_FORWARD
			 *
			 * 跳出 switch，以便与 FETCH_FORWARD 共享代码。 */
			break;
		case FETCH_ABSOLUTE:
			if (count > 0)
			{
				/*
				 * Definition: Rewind to start, advance count-1 rows, return
				 * next row (if any).
				 *
				 * In practice, if the goal is less than halfway back to the
				 * start, it's better to scan from where we are.
				 *
				 * Also, if current portalPos is outside the range of "long",
				 * do it the hard way to avoid possible overflow of the count
				 * argument to PortalRunSelect.  We must exclude exactly
				 * LONG_MAX, as well, lest the count look like FETCH_ALL.
				 *
				 * In any case, we arrange to fetch the target row going
				 * forwards.
				 *
				 * 定义：倒回到起点，前进 count-1 行，返回下一行（如果有）。
				 *
				 * 实际上，如果目标位置距离起点不到当前位置的一半，
				 * 从当前位置扫描更好。
				 *
				 * 此外，如果当前 portalPos 超出了 "long" 的范围，
				 * 则用较慢的方法执行，以避免 PortalRunSelect 的 count 参数可能溢出。
				 * 我们还必须排除恰好等于 LONG_MAX 的情况，以免 count 看起来像
				 * FETCH_ALL。
				 *
				 * 无论如何，我们都会安排以向前方向抓取目标行。
				 */
				if ((uint64) (count - 1) <= portal->portalPos / 2 ||
					portal->portalPos >= (uint64) LONG_MAX)
				{
					DoPortalRewind(portal);
					if (count > 1)
						PortalRunSelect(portal, true, count - 1,
										None_Receiver);
				}
				else
				{
					long		pos = (long) portal->portalPos;

					if (portal->atEnd)
						pos++;	/* need one extra fetch if off end
								 *
								 * 如果已经越过末端，需要额外抓取一次。 */
					if (count <= pos)
						PortalRunSelect(portal, false, pos - count + 1,
										None_Receiver);
					else if (count > pos + 1)
						PortalRunSelect(portal, true, count - pos - 1,
										None_Receiver);
				}
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: Advance to end, back up abs(count)-1 rows,
				 * return prior row (if any).  We could optimize this if we
				 * knew in advance where the end was, but typically we won't.
				 * (Is it worth considering case where count > half of size of
				 * query?  We could rewind once we know the size ...)
				 *
				 * 定义：前进到末端，回退 abs(count)-1 行，返回前一行（如果有）。
				 * 如果预先知道末端位置，可以优化这一点，但通常我们不知道。
				 * （是否值得考虑 count 大于查询大小一半的情况？知道大小后可以倒回……）
				 */
				PortalRunSelect(portal, true, FETCH_ALL, None_Receiver);
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
			{
				/* count == 0
				 *
				 * count 等于 0。 */
				/* Rewind to start, return zero rows
				 *
				 * 倒回到起点，返回零行。 */
				DoPortalRewind(portal);
				return PortalRunSelect(portal, true, 0L, dest);
			}
			break;
		case FETCH_RELATIVE:
			if (count > 0)
			{
				/*
				 * Definition: advance count-1 rows, return next row (if any).
				 *
				 * 定义：前进 count-1 行，返回下一行（如果有）。
				 */
				if (count > 1)
					PortalRunSelect(portal, true, count - 1, None_Receiver);
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: back up abs(count)-1 rows, return prior row (if
				 * any).
				 *
				 * 定义：回退 abs(count)-1 行，返回前一行（如果有）。
				 */
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
			{
				/* count == 0
				 *
				 * count 等于 0。 */
				/* Same as FETCH FORWARD 0, so fall out of switch
				 *
				 * 与 FETCH FORWARD 0 相同，因此跳出 switch。 */
				fdirection = FETCH_FORWARD;
			}
			break;
		default:
			elog(ERROR, "bogus direction");
			break;
	}

	/*
	 * Get here with fdirection == FETCH_FORWARD or FETCH_BACKWARD, and count
	 * >= 0.
	 *
	 * 到达这里时，fdirection == FETCH_FORWARD 或 FETCH_BACKWARD，
	 * 并且 count >= 0。
	 */
	forward = (fdirection == FETCH_FORWARD);

	/*
	 * Zero count means to re-fetch the current row, if any (per SQL)
	 *
	 * count 为零表示重新抓取当前行（如果有，按 SQL 语义）。
	 */
	if (count == 0)
	{
		bool		on_row;

		/*
		 * Are we sitting on a row?
		 *
		 * 我们当前是否停在某一行上？
		 */
		on_row = (!portal->atStart && !portal->atEnd);

		if (dest->mydest == DestNone)
		{
			/* MOVE 0 returns 0/1 based on if FETCH 0 would return a row
			 *
			 * MOVE 0 根据 FETCH 0 是否会返回一行而返回 0/1。 */
			return on_row ? 1 : 0;
		}
		else
		{
			/*
			 * If we are sitting on a row, back up one so we can re-fetch it.
			 * If we are not sitting on a row, we still have to start up and
			 * shut down the executor so that the destination is initialized
			 * and shut down correctly; so keep going.  To PortalRunSelect,
			 * count == 0 means we will retrieve no row.
			 *
			 * 如果当前停在某一行上，则先回退一行，以便重新抓取它。
			 * 如果当前没有停在某一行上，仍然必须启动并关闭执行器，
			 * 以便目标能被正确初始化和关闭；因此继续执行。对于 PortalRunSelect，
			 * count == 0 表示不会取回任何行。
			 */
			if (on_row)
			{
				PortalRunSelect(portal, false, 1L, None_Receiver);
				/* Set up to fetch one row forward
				 *
				 * 设置为向前抓取一行。 */
				count = 1;
				forward = true;
			}
		}
	}

	/*
	 * Optimize MOVE BACKWARD ALL into a Rewind.
	 *
	 * 将 MOVE BACKWARD ALL 优化为 Rewind。
	 */
	if (!forward && count == FETCH_ALL && dest->mydest == DestNone)
	{
		uint64		result = portal->portalPos;

		if (result > 0 && !portal->atEnd)
			result--;
		DoPortalRewind(portal);
		return result;
	}

	return PortalRunSelect(portal, forward, count, dest);
}

/*
 * DoPortalRewind - rewind a Portal to starting point
 *
 * DoPortalRewind - 将 Portal 倒回到起点。
 * 主要流程是检查是否需要移动、确认游标允许滚动、重扫 holdStore 和执行器，
 * 最后重置 Portal 位置状态。
 */
static void
DoPortalRewind(Portal portal)
{
	QueryDesc  *queryDesc;

	/*
	 * No work is needed if we've not advanced nor attempted to advance the
	 * cursor (and we don't want to throw a NO SCROLL error in this case).
	 *
	 * 如果还没有前进，也没有尝试前进游标，就不需要做任何工作
	 * （并且这种情况下不希望抛出 NO SCROLL 错误）。
	 */
	if (portal->atStart && !portal->atEnd)
		return;

	/*
	 * Otherwise, cursor must allow scrolling
	 *
	 * 否则，游标必须允许滚动。
	 */
	if (portal->cursorOptions & CURSOR_OPT_NO_SCROLL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cursor can only scan forward"),
				 errhint("Declare it with SCROLL option to enable backward scan.")));

	/*
	 * Rewind holdStore, if we have one
	 *
	 * 如果存在 holdStore，则将其倒回。
	 */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_rescan(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Rewind executor, if active
	 *
	 * 如果执行器处于活动状态，则将其倒回。
	 */
	queryDesc = portal->queryDesc;
	if (queryDesc)
	{
		PushActiveSnapshot(queryDesc->snapshot);
		ExecutorRewind(queryDesc);
		PopActiveSnapshot();
	}

	portal->atStart = true;
	portal->atEnd = false;
	portal->portalPos = 0;
}

/*
 * PlannedStmtRequiresSnapshot - what it says on the tin
 *
 * PlannedStmtRequiresSnapshot - 顾名思义，判断 PlannedStmt 是否需要快照。
 * 主要流程是非 utility 语句直接需要快照；utility 语句只枚举那些不需要快照的例外。
 */
bool
PlannedStmtRequiresSnapshot(PlannedStmt *pstmt)
{
	Node	   *utilityStmt = pstmt->utilityStmt;

	/*
	 * If it's not a utility statement, it definitely needs a snapshot
	 *
	 * 如果它不是 utility 语句，就一定需要快照。
	 */
	if (utilityStmt == NULL)
		return true;

	/*
	 * Most utility statements need a snapshot, and the default presumption
	 * about new ones should be that they do too.  Hence, enumerate those that
	 * do not need one.
	 *
	 * Transaction control, LOCK, and SET must *not* set a snapshot, since
	 * they need to be executable at the start of a transaction-snapshot-mode
	 * transaction without freezing a snapshot.  By extension we allow SHOW
	 * not to set a snapshot.  The other stmts listed are just efficiency
	 * hacks.  Beware of listing anything that can modify the database --- if,
	 * say, it has to update an index with expressions that invoke
	 * user-defined functions, then it had better have a snapshot.
	 *
	 * 大多数 utility 语句需要快照，对于新增语句，默认也应假定它们需要快照。
	 * 因此，这里枚举那些不需要快照的语句。
	 *
	 * 事务控制、LOCK 和 SET 绝不能设置快照，因为它们需要能在事务快照模式事务的
	 * 开头执行，而不冻结快照。扩展来说，我们也允许 SHOW 不设置快照。
	 * 列出的其他语句只是出于效率的技巧。注意不要列入任何可能修改数据库的语句；
	 * 比如，如果它必须更新一个带有表达式且会调用用户定义函数的索引，
	 * 那它最好有快照。
	 */
	if (IsA(utilityStmt, TransactionStmt) ||
		IsA(utilityStmt, LockStmt) ||
		IsA(utilityStmt, VariableSetStmt) ||
		IsA(utilityStmt, VariableShowStmt) ||
		IsA(utilityStmt, ConstraintsSetStmt) ||
	/* efficiency hacks from here down
	 *
	 * 从这里往下是效率技巧。 */
		IsA(utilityStmt, FetchStmt) ||
		IsA(utilityStmt, ListenStmt) ||
		IsA(utilityStmt, NotifyStmt) ||
		IsA(utilityStmt, UnlistenStmt) ||
		IsA(utilityStmt, CheckPointStmt))
		return false;

	return true;
}

/*
 * EnsurePortalSnapshotExists - recreate Portal-level snapshot, if needed
 *
 * EnsurePortalSnapshotExists - 如有需要，重新创建 Portal 级快照。
 *
 * Generally, we will have an active snapshot whenever we are executing
 * inside a Portal, unless the Portal's query is one of the utility
 * statements exempted from that rule (see PlannedStmtRequiresSnapshot).
 * However, procedures and DO blocks can commit or abort the transaction,
 * and thereby destroy all snapshots.  This function can be called to
 * re-establish the Portal-level snapshot when none exists.
 *
 * 通常，在 Portal 内执行时都会有一个活动快照，除非该 Portal 的查询属于
 * 此规则豁免的 utility 语句之一（参见 PlannedStmtRequiresSnapshot）。
 * 但是，过程和 DO 块可以提交或中止事务，从而销毁所有快照。
 * 当不存在 Portal 级快照时，可以调用此函数重新建立它。
 */
void
EnsurePortalSnapshotExists(void)
{
	Portal		portal;

	/*
	 * Nothing to do if a snapshot is set.  (We take it on faith that the
	 * outermost active snapshot belongs to some Portal; or if there is no
	 * Portal, it's somebody else's responsibility to manage things.)
	 *
	 * 如果已经设置了快照，就无需做任何事。（我们相信最外层活动快照属于某个
	 * Portal；或者如果没有 Portal，管理这些内容就是其他人的责任。）
	 */
	if (ActiveSnapshotSet())
		return;

	/*
	 * Otherwise, we'd better have an active Portal
	 *
	 * 否则，最好确实存在活动 Portal。
	 */
	portal = ActivePortal;
	if (unlikely(portal == NULL))
		elog(ERROR, "cannot execute SQL without an outer snapshot or portal");
	Assert(portal->portalSnapshot == NULL);

	/*
	 * Create a new snapshot, make it active, and remember it in portal.
	 * Because the portal now references the snapshot, we must tell snapmgr.c
	 * that the snapshot belongs to the portal's transaction level, else we
	 * risk portalSnapshot becoming a dangling pointer.
	 *
	 * 创建新快照，使其成为活动快照，并记入 Portal。
	 * 因为 Portal 现在引用该快照，所以必须告诉 snapmgr.c
	 * 该快照属于 Portal 的事务层级，否则 portalSnapshot 有变成悬垂指针的风险。
	 */
	PushActiveSnapshotWithLevel(GetTransactionSnapshot(), portal->createLevel);
	/*
	 * PushActiveSnapshotWithLevel might have copied the snapshot
	 *
	 * PushActiveSnapshotWithLevel 可能已经复制了快照。
	 */
	portal->portalSnapshot = GetActiveSnapshot();
}
