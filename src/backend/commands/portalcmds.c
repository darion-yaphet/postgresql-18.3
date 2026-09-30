/*-------------------------------------------------------------------------
 *
 * portalcmds.c
 *	  Utility commands affecting portals (that is, SQL cursor commands)
 *
 * 影响 portal 的实用命令，也就是 SQL 游标命令。
 *
 * Note: see also tcop/pquery.c, which implements portal operations for
 * the FE/BE protocol.  This module uses pquery.c for some operations.
 * And both modules depend on utils/mmgr/portalmem.c, which controls
 * storage management for portals (but doesn't run any queries in them).
 *
 * 另见 tcop/pquery.c，它实现前端/后端协议中的 portal 操作。本模块的部分操作会用到 pquery.c。
 * 两者都依赖 utils/mmgr/portalmem.c，后者管理 portal 的存储，但不在其中运行查询。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/portalcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "commands/portalcmds.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "parser/analyze.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * 核心流程概览：
 * PerformCursorOpen 执行 DECLARE CURSOR：重写、规划，创建 portal 并启动执行。
 * PerformPortalFetch 执行 FETCH 或 MOVE。PerformPortalClose 关闭游标。
 * PortalCleanup 在 portal 丢弃时收尾；PersistHoldablePortal 把可保持游标的
 * 结果写入 tuplestore，以便事务结束后仍能访问。
 */
/*
 * PerformCursorOpen
 *		Execute SQL DECLARE CURSOR command.
 *
 * PerformCursorOpen：执行 SQL DECLARE CURSOR。
 */
void
PerformCursorOpen(ParseState *pstate, DeclareCursorStmt *cstmt, ParamListInfo params,
				  bool isTopLevel)
{
	Query	   *query = castNode(Query, cstmt->query);
	JumbleState *jstate = NULL;
	List	   *rewritten;
	PlannedStmt *plan;
	Portal		portal;
	MemoryContext oldContext;
	char	   *queryString;

	/*
	 * Disallow empty-string cursor name (conflicts with protocol-level
	 * unnamed portal).
	 *
	 * 不允许空字符串游标名，它会与协议层的未命名 portal 冲突。
	 */
	if (!cstmt->portalname || cstmt->portalname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/*
	 * If this is a non-holdable cursor, we require that this statement has
	 * been executed inside a transaction block (or else, it would have no
	 * user-visible effect).
	 *
	 * 非可保持游标必须在事务块内执行，否则对用户没有可见效果。
	 */
	if (!(cstmt->options & CURSOR_OPT_HOLD))
		RequireTransactionBlock(isTopLevel, "DECLARE CURSOR");
	else if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create a cursor WITH HOLD within security-restricted operation")));

	/* Query contained by DeclareCursor needs to be jumbled if requested */
	/*
	 *
	 * 若有请求，DeclareCursor 所含的 Query 需要做 jumble。
	 */
	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);

	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);

	/*
	 * Parse analysis was done already, but we still have to run the rule
	 * rewriter.  We do not do AcquireRewriteLocks: we assume the query either
	 * came straight from the parser, or suitable locks were acquired by
	 * plancache.c.
	 *
	 * 语法分析已经做过，但仍要跑规则重写器。这里不做 AcquireRewriteLocks：
	 * 假定查询直接来自解析器，或 plancache.c 已经加上了合适的锁。
	 */
	rewritten = QueryRewrite(query);

	/* SELECT should never rewrite to more or less than one query */
	/*
	 *
	 * SELECT 重写后不应变成多于或少于一条查询。
	 */
	if (list_length(rewritten) != 1)
		elog(ERROR, "non-SELECT statement in DECLARE CURSOR");

	query = linitial_node(Query, rewritten);

	if (query->commandType != CMD_SELECT)
		elog(ERROR, "non-SELECT statement in DECLARE CURSOR");

	/* Plan the query, applying the specified options */
	/*
	 *
	 * 规划查询，并应用指定选项。
	 */
	plan = pg_plan_query(query, pstate->p_sourcetext, cstmt->options, params);

	/*
	 * Create a portal and copy the plan and query string into its memory.
	 *
	 * 创建 portal，并把计划和查询字符串复制到它的内存中。
	 */
	portal = CreatePortal(cstmt->portalname, false, false);

	oldContext = MemoryContextSwitchTo(portal->portalContext);

	plan = copyObject(plan);

	queryString = pstrdup(pstate->p_sourcetext);

	PortalDefineQuery(portal,
					  NULL,
					  queryString,
					  CMDTAG_SELECT,	/* cursor's query is always a SELECT */
					  /*
					   *
					   * 游标的查询始终是 SELECT。
					   */
					  list_make1(plan),
					  NULL);

	/*----------
	 * Also copy the outer portal's parameter list into the inner portal's
	 * memory context.  We want to pass down the parameter values in case we
	 * had a command like
	 *		DECLARE c CURSOR FOR SELECT ... WHERE foo = $1
	 * This will have been parsed using the outer parameter set and the
	 * parameter value needs to be preserved for use when the cursor is
	 * executed.
	 *
	 * 把外层 portal 的参数列表复制到内层 portal 的内存上下文。
	 * 这样 DECLARE c CURSOR FOR SELECT ... WHERE foo = $1 这类命令
	 * 在外层参数集下分析后，执行游标时仍能用到参数值。
	 *----------
	 */
	params = copyParamList(params);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Set up options for portal.
	 *
	 * 设置 portal 的选项。
	 *
	 * If the user didn't specify a SCROLL type, allow or disallow scrolling
	 * based on whether it would require any additional runtime overhead to do
	 * so.  Also, we disallow scrolling for FOR UPDATE cursors.
	 *
	 * 用户未指定 SCROLL 时，按滚动是否带来额外运行时开销来允许或禁止。
	 * FOR UPDATE 游标不允许滚动。
	 */
	portal->cursorOptions = cstmt->options;
	if (!(portal->cursorOptions & (CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL)))
	{
		if (plan->rowMarks == NIL &&
			ExecSupportsBackwardScan(plan->planTree))
			portal->cursorOptions |= CURSOR_OPT_SCROLL;
		else
			portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;
	}

	/*
	 * Start execution, inserting parameters if any.
	 *
	 * 开始执行，如有参数则代入。
	 */
	PortalStart(portal, params, 0, GetActiveSnapshot());

	Assert(portal->strategy == PORTAL_ONE_SELECT);

	/*
	 * We're done; the query won't actually be run until PerformPortalFetch is
	 * called.
	 *
	 * 到此结束；查询要等到调用 PerformPortalFetch 才会真正运行。
	 */
}

/*
 * PerformPortalFetch
 *		Execute SQL FETCH or MOVE command.
 *
 * PerformPortalFetch：执行 SQL FETCH 或 MOVE。
 *
 *	stmt: parsetree node for command
 *	dest: where to send results
 *	qc: where to store a command completion status data.
 *
 * stmt：命令的分析树节点。
 * dest：结果发送到哪里。
 * qc：存放命令完成状态的位置。
 *
 * qc may be NULL if caller doesn't want status data.
 *
 * 调用者不需要状态数据时 qc 可以为 NULL。
 */
void
PerformPortalFetch(FetchStmt *stmt,
				   DestReceiver *dest,
				   QueryCompletion *qc)
{
	Portal		portal;
	uint64		nprocessed;

	/*
	 * Disallow empty-string cursor name (conflicts with protocol-level
	 * unnamed portal).
	 *
	 * 不允许空字符串游标名，它会与协议层的未命名 portal 冲突。
	 */
	if (!stmt->portalname || stmt->portalname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/* get the portal from the portal name */
	/*
	 *
	 * 按 portal 名取得 portal。
	 */
	portal = GetPortalByName(stmt->portalname);
	if (!PortalIsValid(portal))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", stmt->portalname)));
		return;					/* keep compiler happy */
		/*
		 *
		 * 避免编译器告警。
		 */
	}

	/* Adjust dest if needed.  MOVE wants destination DestNone */
	/*
	 *
	 * 必要时调整 dest。MOVE 的目标是 DestNone。
	 */
	if (stmt->ismove)
		dest = None_Receiver;

	/* Do it */
	/*
	 *
	 * 执行。
	 */
	nprocessed = PortalRunFetch(portal,
								stmt->direction,
								stmt->howMany,
								dest);

	/* Return command status if wanted */
	/*
	 *
	 * 若需要则返回命令状态。
	 */
	if (qc)
		SetQueryCompletion(qc, stmt->ismove ? CMDTAG_MOVE : CMDTAG_FETCH,
						   nprocessed);
}

/*
 * PerformPortalClose
 *		Close a cursor.
 *
 * PerformPortalClose：关闭游标。
 */
void
PerformPortalClose(const char *name)
{
	Portal		portal;

	/* NULL means CLOSE ALL */
	/*
	 *
	 * NULL 表示 CLOSE ALL。
	 */
	if (name == NULL)
	{
		PortalHashTableDeleteAll();
		return;
	}

	/*
	 * Disallow empty-string cursor name (conflicts with protocol-level
	 * unnamed portal).
	 *
	 * 不允许空字符串游标名，它会与协议层的未命名 portal 冲突。
	 */
	if (name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/*
	 * get the portal from the portal name
	 *
	 * 按 portal 名取得 portal。
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));
		return;					/* keep compiler happy */
		/*
		 *
		 * 避免编译器告警。
		 */
	}

	/*
	 * Note: PortalCleanup is called as a side-effect, if not already done.
	 *
	 * 若尚未清理，PortalCleanup 会作为副作用被调用。
	 */
	PortalDrop(portal, false);
}

/*
 * PortalCleanup
 *
 * PortalCleanup：
 *
 * Clean up a portal when it's dropped.  This is the standard cleanup hook
 * for portals.
 *
 * portal 被丢弃时做清理。这是 portal 的标准清理钩子。
 *
 * Note: if portal->status is PORTAL_FAILED, we are probably being called
 * during error abort, and must be careful to avoid doing anything that
 * is likely to fail again.
 *
 * 若 portal->status 为 PORTAL_FAILED，多半是在错误中止期间被调用，
 * 必须避免再做可能再次失败的事。
 */
void
PortalCleanup(Portal portal)
{
	QueryDesc  *queryDesc;

	/*
	 * sanity checks
	 *
	 * 健全性检查。
	 */
	Assert(PortalIsValid(portal));
	Assert(portal->cleanup == PortalCleanup);

	/*
	 * Shut down executor, if still running.  We skip this during error abort,
	 * since other mechanisms will take care of releasing executor resources,
	 * and we can't be sure that ExecutorEnd itself wouldn't fail.
	 *
	 * 若执行器仍在运行则关闭它。错误中止期间跳过，因为另有机制释放执行器资源，
	 * 而且不能保证 ExecutorEnd 本身不会失败。
	 */
	queryDesc = portal->queryDesc;
	if (queryDesc)
	{
		/*
		 * Reset the queryDesc before anything else.  This prevents us from
		 * trying to shut down the executor twice, in case of an error below.
		 * The transaction abort mechanisms will take care of resource cleanup
		 * in such a case.
		 *
		 * 先重置 queryDesc，以免下面出错时把执行器关闭两次。
		 * 那种情况下由事务中止机制负责清理资源。
		 */
		portal->queryDesc = NULL;

		if (portal->status != PORTAL_FAILED)
		{
			ResourceOwner saveResourceOwner;

			/* We must make the portal's resource owner current */
			/*
			 *
			 * 必须把 portal 的资源所有者设为当前。
			 */
			saveResourceOwner = CurrentResourceOwner;
			if (portal->resowner)
				CurrentResourceOwner = portal->resowner;

			ExecutorFinish(queryDesc);
			ExecutorEnd(queryDesc);
			FreeQueryDesc(queryDesc);

			CurrentResourceOwner = saveResourceOwner;
		}
	}
}

/*
 * PersistHoldablePortal
 *
 * PersistHoldablePortal：
 *
 * Prepare the specified Portal for access outside of the current
 * transaction. When this function returns, all future accesses to the
 * portal must be done via the Tuplestore (not by invoking the
 * executor).
 *
 * 让指定 Portal 能在当前事务之外访问。本函数返回后，以后访问该 portal
 * 必须通过 Tuplestore，而不能再调用执行器。
 */
void
PersistHoldablePortal(Portal portal)
{
	QueryDesc  *queryDesc = portal->queryDesc;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldcxt;

	/*
	 * If we're preserving a holdable portal, we had better be inside the
	 * transaction that originally created it.
	 *
	 * 若要保留可保持 portal，当前必须仍在最初创建它的事务里。
	 */
	Assert(portal->createSubid != InvalidSubTransactionId);
	Assert(queryDesc != NULL);

	/*
	 * Caller must have created the tuplestore already ... but not a snapshot.
	 *
	 * 调用者必须已经创建 tuplestore，但还没有快照。
	 */
	Assert(portal->holdContext != NULL);
	Assert(portal->holdStore != NULL);
	Assert(portal->holdSnapshot == NULL);

	/*
	 * Before closing down the executor, we must copy the tupdesc into
	 * long-term memory, since it was created in executor memory.
	 *
	 * 关闭执行器之前，必须把 tupdesc 复制到长期内存，因为它是在执行器内存中创建的。
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	portal->tupDesc = CreateTupleDescCopy(portal->tupDesc);

	MemoryContextSwitchTo(oldcxt);

	/*
	 * Check for improper portal use, and mark portal active.
	 *
	 * 检查 portal 是否被不当使用，并标记为活动。
	 */
	MarkPortalActive(portal);

	/*
	 * Set up global portal context pointers.
	 *
	 * 设置全局 portal 上下文指针。
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ScanDirection direction = ForwardScanDirection;

		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = portal->portalContext;

		MemoryContextSwitchTo(PortalContext);

		PushActiveSnapshot(queryDesc->snapshot);

		/*
		 * If the portal is marked scrollable, we need to store the entire
		 * result set in the tuplestore, so that subsequent backward FETCHs
		 * can be processed.  Otherwise, store only the not-yet-fetched rows.
		 * (The latter is not only more efficient, but avoids semantic
		 * problems if the query's output isn't stable.)
		 *
		 * 若 portal 可滚动，必须把整个结果集存进 tuplestore，以便随后反向 FETCH。
		 * 否则只保存尚未取出的行。后者更高效，也能避免查询输出不稳定时的语义问题。
		 *
		 * In the no-scroll case, tuple indexes in the tuplestore will not
		 * match the cursor's nominal position (portalPos).  Currently this
		 * causes no difficulty because we only navigate in the tuplestore by
		 * relative position, except for the tuplestore_skiptuples call below
		 * and the tuplestore_rescan call in DoPortalRewind, both of which are
		 * disabled for no-scroll cursors.  But someday we might need to track
		 * the offset between the holdStore and the cursor's nominal position
		 * explicitly.
		 *
		 * 不可滚动时，tuplestore 中的元组下标与游标名义位置 portalPos 不一致。
		 * 目前只按相对位置在 tuplestore 中移动，因此没有问题；下面的 tuplestore_skiptuples
		 * 和 DoPortalRewind 中的 tuplestore_rescan 对不可滚动游标是禁用的。
		 * 将来也许需要显式记录 holdStore 与游标名义位置的偏移。
		 */
		if (portal->cursorOptions & CURSOR_OPT_SCROLL)
		{
			ExecutorRewind(queryDesc);
		}
		else
		{
			/*
			 * If we already reached end-of-query, set the direction to
			 * NoMovement to avoid trying to fetch any tuples.  (This check
			 * exists because not all plan node types are robust about being
			 * called again if they've already returned NULL once.)  We'll
			 * still set up an empty tuplestore, though, to keep this from
			 * being a special case later.
			 *
			 * 若已经到达查询末尾，把方向设为 NoMovement，避免再取元组。
			 * 有些计划节点在返回过一次 NULL 后再被调用并不稳健。
			 * 仍会建立一个空 tuplestore，以免后面把它当成特例。
			 */
			if (portal->atEnd)
				direction = NoMovementScanDirection;
		}

		/*
		 * Change the destination to output to the tuplestore.  Note we tell
		 * the tuplestore receiver to detoast all data passed through it; this
		 * makes it safe to not keep a snapshot associated with the data.
		 *
		 * 把目标改成输出到 tuplestore。并让接收器对经过的数据全部 detoast，
		 * 这样数据就不必再关联快照。
		 */
		queryDesc->dest = CreateDestReceiver(DestTuplestore);
		SetTuplestoreDestReceiverParams(queryDesc->dest,
										portal->holdStore,
										portal->holdContext,
										true,
										NULL,
										NULL);

		/* Fetch the result set into the tuplestore */
		/*
		 *
		 * 把结果集取进 tuplestore。
		 */
		ExecutorRun(queryDesc, direction, 0);

		queryDesc->dest->rDestroy(queryDesc->dest);
		queryDesc->dest = NULL;

		/*
		 * Now shut down the inner executor.
		 *
		 * 现在关闭内部执行器。
		 */
		portal->queryDesc = NULL;	/* prevent double shutdown */
		/*
		 *
		 * 防止关闭两次。
		 */
		ExecutorFinish(queryDesc);
		ExecutorEnd(queryDesc);
		FreeQueryDesc(queryDesc);

		/*
		 * Set the position in the result set.
		 *
		 * 设置在结果集中的位置。
		 */
		MemoryContextSwitchTo(portal->holdContext);

		if (portal->atEnd)
		{
			/*
			 * Just force the tuplestore forward to its end.  The size of the
			 * skip request here is arbitrary.
			 *
			 * 直接把 tuplestore 快进到末尾。这里请求跳过的数量是任意的。
			 */
			while (tuplestore_skiptuples(portal->holdStore, 1000000, true))
				 /* continue */ ;
		}
		else
		{
			tuplestore_rescan(portal->holdStore);

			/*
			 * In the no-scroll case, the start of the tuplestore is exactly
			 * where we want to be, so no repositioning is wanted.
			 *
			 * 不可滚动时，tuplestore 的起点就是我们要的位置，不必重新定位。
			 */
			if (portal->cursorOptions & CURSOR_OPT_SCROLL)
			{
				if (!tuplestore_skiptuples(portal->holdStore,
										   portal->portalPos,
										   true))
					elog(ERROR, "unexpected end of tuple stream");
			}
		}
	}
	PG_CATCH();
	{
		/* Uncaught error while executing portal: mark it dead */
		/*
		 *
		 * 执行 portal 时出现未捕获错误：把它标为失效。
		 */
		MarkPortalFailed(portal);

		/* Restore global vars and propagate error */
		/*
		 *
		 * 恢复全局变量并传播错误。
		 */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);

	/* Mark portal not active */
	/*
	 *
	 * 把 portal 标为非活动。
	 */
	portal->status = PORTAL_READY;

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	PopActiveSnapshot();

	/*
	 * We can now release any subsidiary memory of the portal's context; we'll
	 * never use it again.  The executor already dropped its context, but this
	 * will clean up anything that glommed onto the portal's context via
	 * PortalContext.
	 *
	 * 现在可以释放 portal 上下文中的附属内存，以后不会再用。
	 * 执行器已经丢掉自己的上下文，这里再清掉通过 PortalContext 挂到 portal 上下文上的东西。
	 */
	MemoryContextDeleteChildren(portal->portalContext);
}
