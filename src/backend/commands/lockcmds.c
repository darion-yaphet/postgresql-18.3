/*-------------------------------------------------------------------------
 *
 * lockcmds.c
 *	  LOCK command support code
 *
 * LOCK 命令的支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/lockcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "commands/lockcmds.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * LockTableCommand 逐个关系检查权限并加锁。普通表沿继承树递归；
 * 视图由 LockViewRecurse 遍历查询树，对其中的表和视图以同一锁模式加锁。
 * LockTableAclCheck 按权限决定允许的锁模式。
 */
static void LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait);
static AclResult LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid);
static void RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid,
										 Oid oldrelid, void *arg);
static void LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
							List *ancestor_views);

/*
 * LOCK TABLE
 *
 * 执行 LOCK TABLE。
 */
void
LockTableCommand(LockStmt *lockstmt)
{
	ListCell   *p;

	/*
	 * Iterate over the list and process the named relations one at a time
	 *
	 * 遍历列表，逐个处理指定的关系。
	 */
	foreach(p, lockstmt->relations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(p);
		bool		recurse = rv->inh;
		Oid			reloid;

		reloid = RangeVarGetRelidExtended(rv, lockstmt->mode,
										  lockstmt->nowait ? RVR_NOWAIT : 0,
										  RangeVarCallbackForLockTable,
										  &lockstmt->mode);

		if (get_rel_relkind(reloid) == RELKIND_VIEW)
			LockViewRecurse(reloid, lockstmt->mode, lockstmt->nowait, NIL);
		else if (recurse)
			LockTableRecurse(reloid, lockstmt->mode, lockstmt->nowait);
	}
}

/*
 * Before acquiring a table lock on the named table, check whether we have
 * permission to do so.
 *
 * 对指定表加锁之前，先检查是否有权限。
 */
static void
RangeVarCallbackForLockTable(const RangeVar *rv, Oid relid, Oid oldrelid,
							 void *arg)
{
	LOCKMODE	lockmode = *(LOCKMODE *) arg;
	char		relkind;
	char		relpersistence;
	AclResult	aclresult;

	if (!OidIsValid(relid))
		return;					/* doesn't exist, so no permissions check */
		/*
		 *
		 * 关系不存在，因此不做权限检查。
		 */
	relkind = get_rel_relkind(relid);
	if (!relkind)
		return;					/* woops, concurrently dropped; no permissions
								 * check */
		/*
		 *
		 * 关系被并发删除，无法做权限检查。
		 */


	/* Currently, we only allow plain tables or views to be locked */
	/*
	 *
	 * 目前只允许锁定普通表或视图。
	 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
		relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot lock relation \"%s\"",
						rv->relname),
				 errdetail_relkind_not_supported(relkind)));

	/*
	 * Make note if a temporary relation has been accessed in this
	 * transaction.
	 *
	 * 若本事务访问了临时关系，记下来。
	 */
	relpersistence = get_rel_persistence(relid);
	if (relpersistence == RELPERSISTENCE_TEMP)
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/* Check permissions. */
	/*
	 *
	 * 检查权限。
	 */
	aclresult = LockTableAclCheck(relid, lockmode, GetUserId());
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
}

/*
 * Apply LOCK TABLE recursively over an inheritance tree
 *
 * 沿继承树递归执行 LOCK TABLE。
 *
 * This doesn't check permission to perform LOCK TABLE on the child tables,
 * because getting here means that the user has permission to lock the
 * parent which is enough.
 *
 * 不再检查对子表执行 LOCK TABLE 的权限。能走到这里说明用户有权锁定父表，这就够了。
 */
static void
LockTableRecurse(Oid reloid, LOCKMODE lockmode, bool nowait)
{
	List	   *children;
	ListCell   *lc;

	children = find_all_inheritors(reloid, NoLock, NULL);

	foreach(lc, children)
	{
		Oid			childreloid = lfirst_oid(lc);

		/* Parent already locked. */
		/*
		 *
		 * 父表已经锁住。
		 */
		if (childreloid == reloid)
			continue;

		if (!nowait)
			LockRelationOid(childreloid, lockmode);
		else if (!ConditionalLockRelationOid(childreloid, lockmode))
		{
			/* try to throw error by name; relation could be deleted... */
			/*
			 *
			 * 尽量按名字报错；关系可能已被删除。
			 */
			char	   *relname = get_rel_name(childreloid);

			if (!relname)
				continue;		/* child concurrently dropped, just skip it */
				/*
				 *
				 * 子表被并发删除，直接跳过。
				 */
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not obtain lock on relation \"%s\"",
							relname)));
		}

		/*
		 * Even if we got the lock, child might have been concurrently
		 * dropped. If so, we can skip it.
		 *
		 * 即使拿到了锁，子表仍可能被并发删除。若是这样，可以跳过。
		 */
		if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(childreloid)))
		{
			/* Release useless lock */
			/*
			 *
			 * 释放无用的锁。
			 */
			UnlockRelationOid(childreloid, lockmode);
			continue;
		}
	}
}

/*
 * Apply LOCK TABLE recursively over a view
 *
 * 对视图递归执行 LOCK TABLE。
 *
 * All tables and views appearing in the view definition query are locked
 * recursively with the same lock mode.
 *
 * 视图定义查询中出现的表和视图都以同一锁模式递归加锁。
 */

typedef struct
{
	LOCKMODE	lockmode;		/* lock mode to use */
	/*
	 *
	 * 要使用的锁模式。
	 */
	bool		nowait;			/* no wait mode */
	/*
	 *
	 * 是否不等待。
	 */
	Oid			check_as_user;	/* user for checking the privilege */
	/*
	 *
	 * 用于检查权限的用户。
	 */
	Oid			viewoid;		/* OID of the view to be locked */
	/*
	 *
	 * 待锁定视图的 OID。
	 */
	List	   *ancestor_views; /* OIDs of ancestor views */
	/*
	 *
	 * 祖先视图的 OID。
	 */
} LockViewRecurse_context;

/*
 * 遍历视图查询树，对其中的表和视图递归加锁。
 */
static bool
LockViewRecurse_walker(Node *node, LockViewRecurse_context *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *query = (Query *) node;
		ListCell   *rtable;

		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = lfirst(rtable);
			AclResult	aclresult;

			Oid			relid = rte->relid;
			char		relkind = rte->relkind;
			char	   *relname = get_rel_name(relid);

			/* Currently, we only allow plain tables or views to be locked. */
			/*
			 *
			 * 目前只允许锁定普通表或视图。
			 */
			if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
				relkind != RELKIND_VIEW)
				continue;

			/*
			 * We might be dealing with a self-referential view.  If so, we
			 * can just stop recursing, since we already locked it.
			 *
			 * 可能遇到自引用视图。此时停止递归，因为已经锁过它。
			 */
			if (list_member_oid(context->ancestor_views, relid))
				continue;

			/*
			 * Check permissions as the specified user.  This will either be
			 * the view owner or the current user.
			 *
			 * 以指定用户检查权限。该用户要么是视图属主，要么是当前用户。
			 */
			aclresult = LockTableAclCheck(relid, context->lockmode,
										  context->check_as_user);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(relkind), relname);

			/* We have enough rights to lock the relation; do so. */
			/*
			 *
			 * 已有足够权限锁定该关系，现在加锁。
			 */
			if (!context->nowait)
				LockRelationOid(relid, context->lockmode);
			else if (!ConditionalLockRelationOid(relid, context->lockmode))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relname)));

			if (relkind == RELKIND_VIEW)
				LockViewRecurse(relid, context->lockmode, context->nowait,
								context->ancestor_views);
			else if (rte->inh)
				LockTableRecurse(relid, context->lockmode, context->nowait);
		}

		return query_tree_walker(query,
								 LockViewRecurse_walker,
								 context,
								 QTW_IGNORE_JOINALIASES);
	}

	return expression_tree_walker(node,
								  LockViewRecurse_walker,
								  context);
}

/*
 * 对视图定义中的关系递归执行 LOCK TABLE。
 */
static void
LockViewRecurse(Oid reloid, LOCKMODE lockmode, bool nowait,
				List *ancestor_views)
{
	LockViewRecurse_context context;
	Relation	view;
	Query	   *viewquery;

	/* caller has already locked the view */
	/*
	 *
	 * 调用者已经锁住该视图。
	 */
	view = table_open(reloid, NoLock);
	viewquery = get_view_query(view);

	/*
	 * If the view has the security_invoker property set, check permissions as
	 * the current user.  Otherwise, check permissions as the view owner.
	 *
	 * 若视图设置了 security_invoker，以当前用户检查权限；否则以视图属主检查。
	 */
	context.lockmode = lockmode;
	context.nowait = nowait;
	if (RelationHasSecurityInvoker(view))
		context.check_as_user = GetUserId();
	else
		context.check_as_user = view->rd_rel->relowner;
	context.viewoid = reloid;
	context.ancestor_views = lappend_oid(ancestor_views, reloid);

	LockViewRecurse_walker((Node *) viewquery, &context);

	context.ancestor_views = list_delete_last(context.ancestor_views);

	table_close(view, NoLock);
}

/*
 * Check whether the current user is permitted to lock this relation.
 *
 * 检查当前用户是否可以锁定该关系。
 */
static AclResult
LockTableAclCheck(Oid reloid, LOCKMODE lockmode, Oid userid)
{
	AclResult	aclresult;
	AclMode		aclmask;

	/* any of these privileges permit any lock mode */
	/*
	 *
	 * 拥有其中任一权限即可使用任意锁模式。
	 */
	aclmask = ACL_MAINTAIN | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE;

	/* SELECT privileges also permit ACCESS SHARE and below */
	/*
	 *
	 * SELECT 权限还允许 ACCESS SHARE 及更弱的锁。
	 */
	if (lockmode <= AccessShareLock)
		aclmask |= ACL_SELECT;

	/* INSERT privileges also permit ROW EXCLUSIVE and below */
	/*
	 *
	 * INSERT 权限还允许 ROW EXCLUSIVE 及更弱的锁。
	 */
	if (lockmode <= RowExclusiveLock)
		aclmask |= ACL_INSERT;

	aclresult = pg_class_aclcheck(reloid, userid, aclmask);

	return aclresult;
}
