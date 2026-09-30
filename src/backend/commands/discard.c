/*-------------------------------------------------------------------------
 *
 * discard.c
 *	  The implementation of the DISCARD command
 *
 * DISCARD 命令的实现。
 *
 * Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/discard.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/async.h"
#include "commands/discard.h"
#include "commands/prepare.h"
#include "commands/sequence.h"
#include "utils/guc.h"
#include "utils/portal.h"

/*
 * 核心流程概览：
 * DiscardCommand 按 stmt->target 分发 DISCARD ALL、PLANS、SEQUENCES、TEMP。
 * DiscardAll 拒绝在事务块中执行，并依次关闭 portal、重置会话授权、GUC、
 * 预备语句、LISTEN、咨询锁、计划缓存、临时表命名空间和序列缓存。
 */
static void DiscardAll(bool isTopLevel);

/*
 * DISCARD { ALL | SEQUENCES | TEMP | PLANS }
 *
 * DISCARD 的目标：ALL、SEQUENCES、TEMP 或 PLANS。
 */
void
DiscardCommand(DiscardStmt *stmt, bool isTopLevel)
{
	switch (stmt->target)
	{
		case DISCARD_ALL:
			DiscardAll(isTopLevel);
			break;

		case DISCARD_PLANS:
			ResetPlanCache();
			break;

		case DISCARD_SEQUENCES:
			ResetSequenceCaches();
			break;

		case DISCARD_TEMP:
			ResetTempTableNamespace();
			break;

		default:
			elog(ERROR, "unrecognized DISCARD target: %d", stmt->target);
	}
}

/*
 * 丢弃当前会话的全部状态。
 * 必须在顶层调用，不能位于事务块内。
 */
static void
DiscardAll(bool isTopLevel)
{
	/*
	 * Disallow DISCARD ALL in a transaction block. This is arguably
	 * inconsistent (we don't make a similar check in the command sequence
	 * that DISCARD ALL is equivalent to), but the idea is to catch mistakes:
	 * DISCARD ALL inside a transaction block would leave the transaction
	 * still uncommitted.
	 *
	 * 禁止在事务块中执行 DISCARD ALL。这与它所等价的命令序列并不一致，
	 * 但目的是抓住误用：事务块内的 DISCARD ALL 会让事务仍未提交。
	 */
	PreventInTransactionBlock(isTopLevel, "DISCARD ALL");

	/* Closing portals might run user-defined code, so do that first. */
	/*
	 *
	 * 关闭 portal 可能运行用户定义代码，因此先做这一步。
	 */
	PortalHashTableDeleteAll();
	SetPGVariable("session_authorization", NIL, false);
	ResetAllOptions();
	DropAllPreparedStatements();
	Async_UnlistenAll();
	LockReleaseAll(USER_LOCKMETHOD, true);
	ResetPlanCache();
	ResetTempTableNamespace();
	ResetSequenceCaches();
}
