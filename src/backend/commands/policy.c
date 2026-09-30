/*-------------------------------------------------------------------------
 *
 * policy.c
 *	  Commands for manipulating policies.
 *
 * 行安全策略维护命令。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/commands/policy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_type.h"
#include "commands/policy.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rowsecurity.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * CreatePolicy 在表上创建行安全策略，写入 pg_policy 并记录角色依赖。
 * AlterPolicy 更新 USING / WITH CHECK、角色与命令类型；rename_policy 改名。
 * RemovePolicyById 按 OID 删除策略；RemoveRoleFromObjectPolicy 从策略中移除角色，
 * 若角色列表会变空则改为删除整个策略。
 * RelationBuildRowSecurity 从 pg_policy 装载策略并挂到 relcache。
 */
static void RangeVarCallbackForPolicy(const RangeVar *rv,
									  Oid relid, Oid oldrelid, void *arg);
static char parse_policy_command(const char *cmd_name);
static Datum *policy_role_list_to_array(List *roles, int *num_roles);

/*
 * Callback to RangeVarGetRelidExtended().
 *
 * RangeVarGetRelidExtended() 的回调。
 *
 * Checks the following:
 *	- the relation specified is a table.
 *	- current user owns the table.
 *	- the table is not a system table.
 *
 * 检查以下各项：
 * - 指定的关系是表。
 * - 当前用户拥有该表。
 * - 该表不是系统表。
 *
 * If any of these checks fails then an error is raised.
 *
 * 任一项检查失败则报错。
 */
static void
RangeVarCallbackForPolicy(const RangeVar *rv, Oid relid, Oid oldrelid,
						  void *arg)
{
	HeapTuple	tuple;
	Form_pg_class classform;
	char		relkind;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;

	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/* Must own relation. */
	/*
	 *
	 * 必须拥有该关系。
	 */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);

	/* No system table modifications unless explicitly allowed. */
	/*
	 *
	 * 除非显式允许，否则不能修改系统表。
	 */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/* Relation type MUST be a table. */
	/*
	 *
	 * 关系类型必须是表。
	 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 * parse_policy_command -
 *	 helper function to convert full command strings to their char
 *	 representation.
 *
 * parse_policy_command：把完整命令字符串转换成对应的字符表示。
 *
 * cmd_name - full string command name. Valid values are 'all', 'select',
 *			  'insert', 'update' and 'delete'.
 *
 * cmd_name 是完整的命令名。合法值为 all、select、insert、update 和 delete。
 *
 */
static char
parse_policy_command(const char *cmd_name)
{
	char		polcmd;

	if (!cmd_name)
		elog(ERROR, "unrecognized policy command");

	if (strcmp(cmd_name, "all") == 0)
		polcmd = '*';
	else if (strcmp(cmd_name, "select") == 0)
		polcmd = ACL_SELECT_CHR;
	else if (strcmp(cmd_name, "insert") == 0)
		polcmd = ACL_INSERT_CHR;
	else if (strcmp(cmd_name, "update") == 0)
		polcmd = ACL_UPDATE_CHR;
	else if (strcmp(cmd_name, "delete") == 0)
		polcmd = ACL_DELETE_CHR;
	else
		elog(ERROR, "unrecognized policy command");

	return polcmd;
}

/*
 * policy_role_list_to_array
 *	 helper function to convert a list of RoleSpecs to an array of
 *	 role id Datums.
 *
 * policy_role_list_to_array：把 RoleSpec 列表转换成角色 id Datum 数组。
 */
static Datum *
policy_role_list_to_array(List *roles, int *num_roles)
{
	Datum	   *role_oids;
	ListCell   *cell;
	int			i = 0;

	/* Handle no roles being passed in as being for public */
	/*
	 *
	 * 未传入角色时视为 public。
	 */
	if (roles == NIL)
	{
		*num_roles = 1;
		role_oids = (Datum *) palloc(*num_roles * sizeof(Datum));
		role_oids[0] = ObjectIdGetDatum(ACL_ID_PUBLIC);

		return role_oids;
	}

	*num_roles = list_length(roles);
	role_oids = (Datum *) palloc(*num_roles * sizeof(Datum));

	foreach(cell, roles)
	{
		RoleSpec   *spec = lfirst(cell);

		/*
		 * PUBLIC covers all roles, so it only makes sense alone.
		 *
		 * PUBLIC 覆盖所有角色，因此只能单独出现。
		 */
		if (spec->roletype == ROLESPEC_PUBLIC)
		{
			if (*num_roles != 1)
			{
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("ignoring specified roles other than PUBLIC"),
						 errhint("All roles are members of the PUBLIC role.")));
				*num_roles = 1;
			}
			role_oids[0] = ObjectIdGetDatum(ACL_ID_PUBLIC);

			return role_oids;
		}
		else
			role_oids[i++] =
				ObjectIdGetDatum(get_rolespec_oid(spec, false));
	}

	return role_oids;
}

/*
 * Load row security policy from the catalog, and store it in
 * the relation's relcache entry.
 *
 * 从目录装载行安全策略，并存入该关系的 relcache 项。
 *
 * Note that caller should have verified that pg_class.relrowsecurity
 * is true for this relation.
 *
 * 调用方应已确认该关系的 pg_class.relrowsecurity 为真。
 */
void
RelationBuildRowSecurity(Relation relation)
{
	MemoryContext rscxt;
	MemoryContext oldcxt = CurrentMemoryContext;
	RowSecurityDesc *rsdesc;
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;

	/*
	 * Create a memory context to hold everything associated with this
	 * relation's row security policy.  This makes it easy to clean up during
	 * a relcache flush.  However, to cover the possibility of an error
	 * partway through, we don't make the context long-lived till we're done.
	 *
	 * 创建内存上下文，存放该关系行安全策略的全部数据，便于 relcache 刷新时清理。
	 * 为防中途出错，做完之前不把该上下文变成长期存活的。
	 */
	rscxt = AllocSetContextCreate(CurrentMemoryContext,
								  "row security descriptor",
								  ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(rscxt,
									  RelationGetRelationName(relation));

	rsdesc = MemoryContextAllocZero(rscxt, sizeof(RowSecurityDesc));
	rsdesc->rscxt = rscxt;

	/*
	 * Now scan pg_policy for RLS policies associated with this relation.
	 * Because we use the index on (polrelid, polname), we should consistently
	 * visit the rel's policies in name order, at least when system indexes
	 * aren't disabled.  This simplifies equalRSDesc().
	 *
	 * 扫描 pg_policy 中与该关系关联的 RLS 策略。由于使用 (polrelid, polname) 索引，
	 * 在系统索引未被禁用时会按名称顺序访问，这简化了 equalRSDesc()。
	 */
	catalog = table_open(PolicyRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	sscan = systable_beginscan(catalog, PolicyPolrelidPolnameIndexId, true,
							   NULL, 1, &skey);

	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		Form_pg_policy policy_form = (Form_pg_policy) GETSTRUCT(tuple);
		RowSecurityPolicy *policy;
		Datum		datum;
		bool		isnull;
		char	   *str_value;

		policy = MemoryContextAllocZero(rscxt, sizeof(RowSecurityPolicy));

		/*
		 * Note: we must be sure that pass-by-reference data gets copied into
		 * rscxt.  We avoid making that context current over wider spans than
		 * we have to, though.
		 *
		 * 注意：必须把按引用传递的数据复制进 rscxt。但不会让该上下文在不必要的范围内成为当前上下文。
		 */

		/* Get policy command */
		/*
		 *
		 * 取得策略命令。
		 */
		policy->polcmd = policy_form->polcmd;

		/* Get policy, permissive or restrictive */
		/*
		 *
		 * 取得策略是 permissive 还是 restrictive。
		 */
		policy->permissive = policy_form->polpermissive;

		/* Get policy name */
		/*
		 *
		 * 取得策略名。
		 */
		policy->policy_name =
			MemoryContextStrdup(rscxt, NameStr(policy_form->polname));

		/* Get policy roles */
		/*
		 *
		 * 取得策略角色。
		 */
		datum = heap_getattr(tuple, Anum_pg_policy_polroles,
							 RelationGetDescr(catalog), &isnull);
		/* shouldn't be null, but let's check for luck */
		/*
		 *
		 * 不应为 null，但还是检查一下。
		 */
		if (isnull)
			elog(ERROR, "unexpected null value in pg_policy.polroles");
		MemoryContextSwitchTo(rscxt);
		policy->roles = DatumGetArrayTypePCopy(datum);
		MemoryContextSwitchTo(oldcxt);

		/* Get policy qual */
		/*
		 *
		 * 取得策略的 qual。
		 */
		datum = heap_getattr(tuple, Anum_pg_policy_polqual,
							 RelationGetDescr(catalog), &isnull);
		if (!isnull)
		{
			str_value = TextDatumGetCString(datum);
			MemoryContextSwitchTo(rscxt);
			policy->qual = (Expr *) stringToNode(str_value);
			MemoryContextSwitchTo(oldcxt);
			pfree(str_value);
		}
		else
			policy->qual = NULL;

		/* Get WITH CHECK qual */
		/*
		 *
		 * 取得 WITH CHECK 的 qual。
		 */
		datum = heap_getattr(tuple, Anum_pg_policy_polwithcheck,
							 RelationGetDescr(catalog), &isnull);
		if (!isnull)
		{
			str_value = TextDatumGetCString(datum);
			MemoryContextSwitchTo(rscxt);
			policy->with_check_qual = (Expr *) stringToNode(str_value);
			MemoryContextSwitchTo(oldcxt);
			pfree(str_value);
		}
		else
			policy->with_check_qual = NULL;

		/* We want to cache whether there are SubLinks in these expressions */
		/*
		 *
		 * 要缓存这些表达式中是否有 SubLink。
		 */
		policy->hassublinks = checkExprHasSubLink((Node *) policy->qual) ||
			checkExprHasSubLink((Node *) policy->with_check_qual);

		/*
		 * Add this object to list.  For historical reasons, the list is built
		 * in reverse order.
		 *
		 * 把该对象加入列表。由于历史原因，列表按相反顺序构建。
		 */
		MemoryContextSwitchTo(rscxt);
		rsdesc->policies = lcons(policy, rsdesc->policies);
		MemoryContextSwitchTo(oldcxt);
	}

	systable_endscan(sscan);
	table_close(catalog, AccessShareLock);

	/*
	 * Success.  Reparent the descriptor's memory context under
	 * CacheMemoryContext so that it will live indefinitely, then attach the
	 * policy descriptor to the relcache entry.
	 *
	 * 成功。把描述符的内存上下文改挂到 CacheMemoryContext 下使其长期存活，
	 * 再把策略描述符挂到 relcache 项上。
	 */
	MemoryContextSetParent(rscxt, CacheMemoryContext);

	relation->rd_rsdesc = rsdesc;
}

/*
 * RemovePolicyById -
 *	 remove a policy by its OID.  If a policy does not exist with the provided
 *	 oid, then an error is raised.
 *
 * RemovePolicyById：按 OID 删除策略。若不存在该 OID 的策略则报错。
 *
 * policy_id - the oid of the policy.
 *
 * policy_id 是策略的 OID。
 */
void
RemovePolicyById(Oid policy_id)
{
	Relation	pg_policy_rel;
	SysScanDesc sscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;
	Oid			relid;
	Relation	rel;

	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/*
	 * Find the policy to delete.
	 *
	 * 查找要删除的策略。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
							   NULL, 1, skey);

	tuple = systable_getnext(sscan);

	/* If the policy exists, then remove it, otherwise raise an error. */
	/*
	 *
	 * 若策略存在则删除，否则报错。
	 */
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for policy %u", policy_id);

	/*
	 * Open and exclusive-lock the relation the policy belongs to.  (We need
	 * exclusive lock to lock out queries that might otherwise depend on the
	 * set of policies the rel has; furthermore we've got to hold the lock
	 * till commit.)
	 *
	 * 打开策略所属关系并加排他锁。需要排他锁以挡住可能依赖该关系策略集合的查询，
	 * 并且锁必须保持到提交。
	 */
	relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;

	rel = table_open(relid, AccessExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	CatalogTupleDelete(pg_policy_rel, &tuple->t_self);

	systable_endscan(sscan);

	/*
	 * Note that, unlike some of the other flags in pg_class, relrowsecurity
	 * is not just an indication of if policies exist.  When relrowsecurity is
	 * set by a user, then all access to the relation must be through a
	 * policy.  If no policy is defined for the relation then a default-deny
	 * policy is created and all records are filtered (except for queries from
	 * the owner).
	 *
	 * 与 pg_class 中的某些标志不同，relrowsecurity 不只表示是否存在策略。
	 * 用户一旦设置它，对该关系的所有访问都必须经过策略。
	 * 若没有定义策略，则采用默认拒绝，除属主的查询外所有记录都被过滤。
	 */
	CacheInvalidateRelcache(rel);

	table_close(rel, NoLock);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(pg_policy_rel, RowExclusiveLock);
}

/*
 * RemoveRoleFromObjectPolicy -
 *	 remove a role from a policy's applicable-roles list.
 *
 * RemoveRoleFromObjectPolicy：从策略的适用角色列表中移除一个角色。
 *
 * Returns true if the role was successfully removed from the policy.
 * Returns false if the role was not removed because it would have left
 * polroles empty (which is disallowed, though perhaps it should not be).
 * On false return, the caller should instead drop the policy altogether.
 *
 * 成功从策略中移除角色则返回 true。若移除后 polroles 会变空（这是不允许的，尽管也许不该禁止），
 * 则不移除并返回 false。返回 false 时调用方应改为整个删除该策略。
 *
 * roleid - the oid of the role to remove
 * classid - should always be PolicyRelationId
 * policy_id - the oid of the policy.
 *
 * roleid 是要移除的角色 OID。
 * classid 应当始终是 PolicyRelationId。
 * policy_id 是策略的 OID。
 */
bool
RemoveRoleFromObjectPolicy(Oid roleid, Oid classid, Oid policy_id)
{
	Relation	pg_policy_rel;
	SysScanDesc sscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;
	Oid			relid;
	ArrayType  *policy_roles;
	Datum		roles_datum;
	Oid		   *roles;
	int			num_roles;
	Datum	   *role_oids;
	bool		attr_isnull;
	bool		keep_policy = true;
	int			i,
				j;

	Assert(classid == PolicyRelationId);

	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/*
	 * Find the policy to update.
	 *
	 * 查找要更新的策略。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
							   NULL, 1, skey);

	tuple = systable_getnext(sscan);

	/* Raise an error if we don't find the policy. */
	/*
	 *
	 * 找不到策略则报错。
	 */
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for policy %u", policy_id);

	/* Identify rel the policy belongs to */
	/*
	 *
	 * 确定策略所属的关系。
	 */
	relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;

	/* Get the current set of roles */
	/*
	 *
	 * 取得当前的角色集合。
	 */
	roles_datum = heap_getattr(tuple,
							   Anum_pg_policy_polroles,
							   RelationGetDescr(pg_policy_rel),
							   &attr_isnull);

	Assert(!attr_isnull);

	policy_roles = DatumGetArrayTypePCopy(roles_datum);
	roles = (Oid *) ARR_DATA_PTR(policy_roles);
	num_roles = ARR_DIMS(policy_roles)[0];

	/*
	 * Rebuild the polroles array, without any mentions of the target role.
	 * Ordinarily there'd be exactly one, but we must cope with duplicate
	 * mentions, since CREATE/ALTER POLICY historically have allowed that.
	 *
	 * 重建 polroles 数组，去掉目标角色的所有出现。通常恰好一个，
	 * 但 CREATE/ALTER POLICY 历史上允许重复，因此必须处理重复项。
	 */
	role_oids = (Datum *) palloc(num_roles * sizeof(Datum));
	for (i = 0, j = 0; i < num_roles; i++)
	{
		if (roles[i] != roleid)
			role_oids[j++] = ObjectIdGetDatum(roles[i]);
	}
	num_roles = j;

	/* If any roles remain, update the policy entry. */
	/*
	 *
	 * 若仍有角色留下，则更新策略项。
	 */
	if (num_roles > 0)
	{
		ArrayType  *role_ids;
		Datum		values[Natts_pg_policy];
		bool		isnull[Natts_pg_policy];
		bool		replaces[Natts_pg_policy];
		HeapTuple	new_tuple;
		HeapTuple	reltup;
		ObjectAddress target;
		ObjectAddress myself;

		/* zero-clear */
		/*
		 *
		 * 清零。
		 */
		memset(values, 0, sizeof(values));
		memset(replaces, 0, sizeof(replaces));
		memset(isnull, 0, sizeof(isnull));

		/* This is the array for the new tuple */
		/*
		 *
		 * 这是新元组使用的数组。
		 */
		role_ids = construct_array_builtin(role_oids, num_roles, OIDOID);

		replaces[Anum_pg_policy_polroles - 1] = true;
		values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);

		new_tuple = heap_modify_tuple(tuple,
									  RelationGetDescr(pg_policy_rel),
									  values, isnull, replaces);
		CatalogTupleUpdate(pg_policy_rel, &new_tuple->t_self, new_tuple);

		/* Remove all the old shared dependencies (roles) */
		/*
		 *
		 * 删除所有旧的共享依赖（角色）。
		 */
		deleteSharedDependencyRecordsFor(PolicyRelationId, policy_id, 0);

		/* Record the new shared dependencies (roles) */
		/*
		 *
		 * 记录新的共享依赖（角色）。
		 */
		myself.classId = PolicyRelationId;
		myself.objectId = policy_id;
		myself.objectSubId = 0;

		target.classId = AuthIdRelationId;
		target.objectSubId = 0;
		for (i = 0; i < num_roles; i++)
		{
			target.objectId = DatumGetObjectId(role_oids[i]);
			/* no need for dependency on the public role */
			/*
			 *
			 * 不需要对 public 角色建立依赖。
			 */
			if (target.objectId != ACL_ID_PUBLIC)
				recordSharedDependencyOn(&myself, &target,
										 SHARED_DEPENDENCY_POLICY);
		}

		InvokeObjectPostAlterHook(PolicyRelationId, policy_id, 0);

		heap_freetuple(new_tuple);

		/* Make updates visible */
		/*
		 *
		 * 使更新可见。
		 */
		CommandCounterIncrement();

		/*
		 * Invalidate relcache entry for rel the policy belongs to, to force
		 * redoing any dependent plans.  In case of a race condition where the
		 * rel was just dropped, we need do nothing.
		 *
		 * 使策略所属关系的 relcache 项失效，以强制重做依赖它的计划。
		 * 若竞争中该关系刚被删除，则无需处理。
		 */
		reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (HeapTupleIsValid(reltup))
		{
			CacheInvalidateRelcacheByTuple(reltup);
			ReleaseSysCache(reltup);
		}
	}
	else
	{
		/* No roles would remain, so drop the policy instead. */
		/*
		 *
		 * 不会剩下任何角色，因此改为删除该策略。
		 */
		keep_policy = false;
	}

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	systable_endscan(sscan);

	table_close(pg_policy_rel, RowExclusiveLock);

	return keep_policy;
}

/*
 * CreatePolicy -
 *	 handles the execution of the CREATE POLICY command.
 *
 * CreatePolicy：执行 CREATE POLICY 命令。
 *
 * stmt - the CreatePolicyStmt that describes the policy to create.
 *
 * stmt 是描述要创建的策略的 CreatePolicyStmt。
 */
ObjectAddress
CreatePolicy(CreatePolicyStmt *stmt)
{
	Relation	pg_policy_rel;
	Oid			policy_id;
	Relation	target_table;
	Oid			table_id;
	char		polcmd;
	Datum	   *role_oids;
	int			nitems = 0;
	ArrayType  *role_ids;
	ParseState *qual_pstate;
	ParseState *with_check_pstate;
	ParseNamespaceItem *nsitem;
	Node	   *qual;
	Node	   *with_check_qual;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	Datum		values[Natts_pg_policy];
	bool		isnull[Natts_pg_policy];
	ObjectAddress target;
	ObjectAddress myself;
	int			i;

	/* Parse command */
	/*
	 *
	 * 解析命令。
	 */
	polcmd = parse_policy_command(stmt->cmd_name);

	/*
	 * If the command is SELECT or DELETE then WITH CHECK should be NULL.
	 *
	 * 若命令是 SELECT 或 DELETE，则 WITH CHECK 应为 NULL。
	 */
	if ((polcmd == ACL_SELECT_CHR || polcmd == ACL_DELETE_CHR)
		&& stmt->with_check != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("WITH CHECK cannot be applied to SELECT or DELETE")));

	/*
	 * If the command is INSERT then WITH CHECK should be the only expression
	 * provided.
	 *
	 * 若命令是 INSERT，则只应提供 WITH CHECK 表达式。
	 */
	if (polcmd == ACL_INSERT_CHR && stmt->qual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only WITH CHECK expression allowed for INSERT")));

	/* Collect role ids */
	/*
	 *
	 * 收集角色 id。
	 */
	role_oids = policy_role_list_to_array(stmt->roles, &nitems);
	role_ids = construct_array_builtin(role_oids, nitems, OIDOID);

	/* Parse the supplied clause */
	/*
	 *
	 * 解析给出的子句。
	 */
	qual_pstate = make_parsestate(NULL);
	with_check_pstate = make_parsestate(NULL);

	/* zero-clear */
	/*
	 *
	 * 清零。
	 */
	memset(values, 0, sizeof(values));
	memset(isnull, 0, sizeof(isnull));

	/* Get id of table.  Also handles permissions checks. */
	/*
	 *
	 * 取得表的 id，同时做权限检查。
	 */
	table_id = RangeVarGetRelidExtended(stmt->table, AccessExclusiveLock,
										0,
										RangeVarCallbackForPolicy,
										stmt);

	/* Open target_table to build quals. No additional lock is necessary. */
	/*
	 *
	 * 打开 target_table 以构造 qual。不必再加锁。
	 */
	target_table = relation_open(table_id, NoLock);

	/* Add for the regular security quals */
	/*
	 *
	 * 加入普通的安全 qual。
	 */
	nsitem = addRangeTableEntryForRelation(qual_pstate, target_table,
										   AccessShareLock,
										   NULL, false, false);
	addNSItemToQuery(qual_pstate, nsitem, false, true, true);

	/* Add for the with-check quals */
	/*
	 *
	 * 加入 WITH CHECK 的 qual。
	 */
	nsitem = addRangeTableEntryForRelation(with_check_pstate, target_table,
										   AccessShareLock,
										   NULL, false, false);
	addNSItemToQuery(with_check_pstate, nsitem, false, true, true);

	qual = transformWhereClause(qual_pstate,
								stmt->qual,
								EXPR_KIND_POLICY,
								"POLICY");

	with_check_qual = transformWhereClause(with_check_pstate,
										   stmt->with_check,
										   EXPR_KIND_POLICY,
										   "POLICY");

	/* Fix up collation information */
	/*
	 *
	 * 修正排序规则信息。
	 */
	assign_expr_collations(qual_pstate, qual);
	assign_expr_collations(with_check_pstate, with_check_qual);

	/* Open pg_policy catalog */
	/*
	 *
	 * 打开 pg_policy 目录。
	 */
	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/* Set key - policy's relation id. */
	/*
	 *
	 * 设置键：策略的关系 id。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Set key - policy's name. */
	/*
	 *
	 * 设置键：策略名。
	 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* Complain if the policy name already exists for the table */
	/*
	 *
	 * 若该表上已有同名策略则报错。
	 */
	if (HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" already exists",
						stmt->policy_name, RelationGetRelationName(target_table))));

	policy_id = GetNewOidWithIndex(pg_policy_rel, PolicyOidIndexId,
								   Anum_pg_policy_oid);
	values[Anum_pg_policy_oid - 1] = ObjectIdGetDatum(policy_id);
	values[Anum_pg_policy_polrelid - 1] = ObjectIdGetDatum(table_id);
	values[Anum_pg_policy_polname - 1] = DirectFunctionCall1(namein,
															 CStringGetDatum(stmt->policy_name));
	values[Anum_pg_policy_polcmd - 1] = CharGetDatum(polcmd);
	values[Anum_pg_policy_polpermissive - 1] = BoolGetDatum(stmt->permissive);
	values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);

	/* Add qual if present. */
	/*
	 *
	 * 若有 qual 则加入。
	 */
	if (qual)
		values[Anum_pg_policy_polqual - 1] = CStringGetTextDatum(nodeToString(qual));
	else
		isnull[Anum_pg_policy_polqual - 1] = true;

	/* Add WITH CHECK qual if present */
	/*
	 *
	 * 若有 WITH CHECK qual 则加入。
	 */
	if (with_check_qual)
		values[Anum_pg_policy_polwithcheck - 1] = CStringGetTextDatum(nodeToString(with_check_qual));
	else
		isnull[Anum_pg_policy_polwithcheck - 1] = true;

	policy_tuple = heap_form_tuple(RelationGetDescr(pg_policy_rel), values,
								   isnull);

	CatalogTupleInsert(pg_policy_rel, policy_tuple);

	/* Record Dependencies */
	/*
	 *
	 * 记录依赖。
	 */
	target.classId = RelationRelationId;
	target.objectId = table_id;
	target.objectSubId = 0;

	myself.classId = PolicyRelationId;
	myself.objectId = policy_id;
	myself.objectSubId = 0;

	recordDependencyOn(&myself, &target, DEPENDENCY_AUTO);

	recordDependencyOnExpr(&myself, qual, qual_pstate->p_rtable,
						   DEPENDENCY_NORMAL);

	recordDependencyOnExpr(&myself, with_check_qual,
						   with_check_pstate->p_rtable, DEPENDENCY_NORMAL);

	/* Register role dependencies */
	/*
	 *
	 * 登记角色依赖。
	 */
	target.classId = AuthIdRelationId;
	target.objectSubId = 0;
	for (i = 0; i < nitems; i++)
	{
		target.objectId = DatumGetObjectId(role_oids[i]);
		/* no dependency if public */
		/*
		 *
		 * 若是 public 则不建立依赖。
		 */
		if (target.objectId != ACL_ID_PUBLIC)
			recordSharedDependencyOn(&myself, &target,
									 SHARED_DEPENDENCY_POLICY);
	}

	InvokeObjectPostCreateHook(PolicyRelationId, policy_id, 0);

	/* Invalidate Relation Cache */
	/*
	 *
	 * 使关系缓存失效。
	 */
	CacheInvalidateRelcache(target_table);

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	heap_freetuple(policy_tuple);
	free_parsestate(qual_pstate);
	free_parsestate(with_check_pstate);
	systable_endscan(sscan);
	relation_close(target_table, NoLock);
	table_close(pg_policy_rel, RowExclusiveLock);

	return myself;
}

/*
 * AlterPolicy -
 *	 handles the execution of the ALTER POLICY command.
 *
 * AlterPolicy：执行 ALTER POLICY 命令。
 *
 * stmt - the AlterPolicyStmt that describes the policy and how to alter it.
 *
 * stmt 是描述策略及如何修改它的 AlterPolicyStmt。
 */
ObjectAddress
AlterPolicy(AlterPolicyStmt *stmt)
{
	Relation	pg_policy_rel;
	Oid			policy_id;
	Relation	target_table;
	Oid			table_id;
	Datum	   *role_oids = NULL;
	int			nitems = 0;
	ArrayType  *role_ids = NULL;
	List	   *qual_parse_rtable = NIL;
	List	   *with_check_parse_rtable = NIL;
	Node	   *qual = NULL;
	Node	   *with_check_qual = NULL;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	HeapTuple	new_tuple;
	Datum		values[Natts_pg_policy];
	bool		isnull[Natts_pg_policy];
	bool		replaces[Natts_pg_policy];
	ObjectAddress target;
	ObjectAddress myself;
	Datum		polcmd_datum;
	char		polcmd;
	bool		polcmd_isnull;
	int			i;

	/* Parse role_ids */
	/*
	 *
	 * 解析 role_ids。
	 */
	if (stmt->roles != NULL)
	{
		role_oids = policy_role_list_to_array(stmt->roles, &nitems);
		role_ids = construct_array_builtin(role_oids, nitems, OIDOID);
	}

	/* Get id of table.  Also handles permissions checks. */
	/*
	 *
	 * 取得表的 id，同时做权限检查。
	 */
	table_id = RangeVarGetRelidExtended(stmt->table, AccessExclusiveLock,
										0,
										RangeVarCallbackForPolicy,
										stmt);

	target_table = relation_open(table_id, NoLock);

	/* Parse the using policy clause */
	/*
	 *
	 * 解析 USING 策略子句。
	 */
	if (stmt->qual)
	{
		ParseNamespaceItem *nsitem;
		ParseState *qual_pstate = make_parsestate(NULL);

		nsitem = addRangeTableEntryForRelation(qual_pstate, target_table,
											   AccessShareLock,
											   NULL, false, false);

		addNSItemToQuery(qual_pstate, nsitem, false, true, true);

		qual = transformWhereClause(qual_pstate, stmt->qual,
									EXPR_KIND_POLICY,
									"POLICY");

		/* Fix up collation information */
		/*
		 *
		 * 修正排序规则信息。
		 */
		assign_expr_collations(qual_pstate, qual);

		qual_parse_rtable = qual_pstate->p_rtable;
		free_parsestate(qual_pstate);
	}

	/* Parse the with-check policy clause */
	/*
	 *
	 * 解析 WITH CHECK 策略子句。
	 */
	if (stmt->with_check)
	{
		ParseNamespaceItem *nsitem;
		ParseState *with_check_pstate = make_parsestate(NULL);

		nsitem = addRangeTableEntryForRelation(with_check_pstate, target_table,
											   AccessShareLock,
											   NULL, false, false);

		addNSItemToQuery(with_check_pstate, nsitem, false, true, true);

		with_check_qual = transformWhereClause(with_check_pstate,
											   stmt->with_check,
											   EXPR_KIND_POLICY,
											   "POLICY");

		/* Fix up collation information */
		/*
		 *
		 * 修正排序规则信息。
		 */
		assign_expr_collations(with_check_pstate, with_check_qual);

		with_check_parse_rtable = with_check_pstate->p_rtable;
		free_parsestate(with_check_pstate);
	}

	/* zero-clear */
	/*
	 *
	 * 清零。
	 */
	memset(values, 0, sizeof(values));
	memset(replaces, 0, sizeof(replaces));
	memset(isnull, 0, sizeof(isnull));

	/* Find policy to update. */
	/*
	 *
	 * 查找要更新的策略。
	 */
	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/* Set key - policy's relation id. */
	/*
	 *
	 * 设置键：策略的关系 id。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Set key - policy's name. */
	/*
	 *
	 * 设置键：策略名。
	 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* Check that the policy is found, raise an error if not. */
	/*
	 *
	 * 确认找到策略，否则报错。
	 */
	if (!HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" does not exist",
						stmt->policy_name,
						RelationGetRelationName(target_table))));

	/* Get policy command */
	/*
	 *
	 * 取得策略命令。
	 */
	polcmd_datum = heap_getattr(policy_tuple, Anum_pg_policy_polcmd,
								RelationGetDescr(pg_policy_rel),
								&polcmd_isnull);
	Assert(!polcmd_isnull);
	polcmd = DatumGetChar(polcmd_datum);

	/*
	 * If the command is SELECT or DELETE then WITH CHECK should be NULL.
	 *
	 * 若命令是 SELECT 或 DELETE，则 WITH CHECK 应为 NULL。
	 */
	if ((polcmd == ACL_SELECT_CHR || polcmd == ACL_DELETE_CHR)
		&& stmt->with_check != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only USING expression allowed for SELECT, DELETE")));

	/*
	 * If the command is INSERT then WITH CHECK should be the only expression
	 * provided.
	 *
	 * 若命令是 INSERT，则只应提供 WITH CHECK 表达式。
	 */
	if ((polcmd == ACL_INSERT_CHR)
		&& stmt->qual != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only WITH CHECK expression allowed for INSERT")));

	policy_id = ((Form_pg_policy) GETSTRUCT(policy_tuple))->oid;

	if (role_ids != NULL)
	{
		replaces[Anum_pg_policy_polroles - 1] = true;
		values[Anum_pg_policy_polroles - 1] = PointerGetDatum(role_ids);
	}
	else
	{
		Oid		   *roles;
		Datum		roles_datum;
		bool		attr_isnull;
		ArrayType  *policy_roles;

		/*
		 * We need to pull the set of roles this policy applies to from what's
		 * in the catalog, so that we can recreate the dependencies correctly
		 * for the policy.
		 *
		 * 需要从目录中取出该策略适用的角色集合，以便正确地重建策略的依赖。
		 */

		roles_datum = heap_getattr(policy_tuple, Anum_pg_policy_polroles,
								   RelationGetDescr(pg_policy_rel),
								   &attr_isnull);
		Assert(!attr_isnull);

		policy_roles = DatumGetArrayTypePCopy(roles_datum);

		roles = (Oid *) ARR_DATA_PTR(policy_roles);

		nitems = ARR_DIMS(policy_roles)[0];

		role_oids = (Datum *) palloc(nitems * sizeof(Datum));

		for (i = 0; i < nitems; i++)
			role_oids[i] = ObjectIdGetDatum(roles[i]);
	}

	if (qual != NULL)
	{
		replaces[Anum_pg_policy_polqual - 1] = true;
		values[Anum_pg_policy_polqual - 1]
			= CStringGetTextDatum(nodeToString(qual));
	}
	else
	{
		Datum		value_datum;
		bool		attr_isnull;

		/*
		 * We need to pull the USING expression and build the range table for
		 * the policy from what's in the catalog, so that we can recreate the
		 * dependencies correctly for the policy.
		 *
		 * 需要从目录中取出 USING 表达式并为策略建立范围表，以便正确地重建依赖。
		 */

		/* Check if the policy has a USING expr */
		/*
		 *
		 * 检查策略是否有 USING 表达式。
		 */
		value_datum = heap_getattr(policy_tuple, Anum_pg_policy_polqual,
								   RelationGetDescr(pg_policy_rel),
								   &attr_isnull);
		if (!attr_isnull)
		{
			char	   *qual_value;
			ParseState *qual_pstate;

			/* parsestate is built just to build the range table */
			/*
			 *
			 * 构建 parsestate 只是为了建立范围表。
			 */
			qual_pstate = make_parsestate(NULL);

			qual_value = TextDatumGetCString(value_datum);
			qual = stringToNode(qual_value);

			/* Add this rel to the parsestate's rangetable, for dependencies */
			/*
			 *
			 * 把该关系加入 parsestate 的范围表，以便建立依赖。
			 */
			(void) addRangeTableEntryForRelation(qual_pstate, target_table,
												 AccessShareLock,
												 NULL, false, false);

			qual_parse_rtable = qual_pstate->p_rtable;
			free_parsestate(qual_pstate);
		}
	}

	if (with_check_qual != NULL)
	{
		replaces[Anum_pg_policy_polwithcheck - 1] = true;
		values[Anum_pg_policy_polwithcheck - 1]
			= CStringGetTextDatum(nodeToString(with_check_qual));
	}
	else
	{
		Datum		value_datum;
		bool		attr_isnull;

		/*
		 * We need to pull the WITH CHECK expression and build the range table
		 * for the policy from what's in the catalog, so that we can recreate
		 * the dependencies correctly for the policy.
		 *
		 * 需要从目录中取出 WITH CHECK 表达式并为策略建立范围表，以便正确地重建依赖。
		 */

		/* Check if the policy has a WITH CHECK expr */
		/*
		 *
		 * 检查策略是否有 WITH CHECK 表达式。
		 */
		value_datum = heap_getattr(policy_tuple, Anum_pg_policy_polwithcheck,
								   RelationGetDescr(pg_policy_rel),
								   &attr_isnull);
		if (!attr_isnull)
		{
			char	   *with_check_value;
			ParseState *with_check_pstate;

			/* parsestate is built just to build the range table */
			/*
			 *
			 * 构建 parsestate 只是为了建立范围表。
			 */
			with_check_pstate = make_parsestate(NULL);

			with_check_value = TextDatumGetCString(value_datum);
			with_check_qual = stringToNode(with_check_value);

			/* Add this rel to the parsestate's rangetable, for dependencies */
			/*
			 *
			 * 把该关系加入 parsestate 的范围表，以便建立依赖。
			 */
			(void) addRangeTableEntryForRelation(with_check_pstate,
												 target_table,
												 AccessShareLock,
												 NULL, false, false);

			with_check_parse_rtable = with_check_pstate->p_rtable;
			free_parsestate(with_check_pstate);
		}
	}

	new_tuple = heap_modify_tuple(policy_tuple,
								  RelationGetDescr(pg_policy_rel),
								  values, isnull, replaces);
	CatalogTupleUpdate(pg_policy_rel, &new_tuple->t_self, new_tuple);

	/* Update Dependencies. */
	/*
	 *
	 * 更新依赖。
	 */
	deleteDependencyRecordsFor(PolicyRelationId, policy_id, false);

	/* Record Dependencies */
	/*
	 *
	 * 记录依赖。
	 */
	target.classId = RelationRelationId;
	target.objectId = table_id;
	target.objectSubId = 0;

	myself.classId = PolicyRelationId;
	myself.objectId = policy_id;
	myself.objectSubId = 0;

	recordDependencyOn(&myself, &target, DEPENDENCY_AUTO);

	recordDependencyOnExpr(&myself, qual, qual_parse_rtable, DEPENDENCY_NORMAL);

	recordDependencyOnExpr(&myself, with_check_qual, with_check_parse_rtable,
						   DEPENDENCY_NORMAL);

	/* Register role dependencies */
	/*
	 *
	 * 登记角色依赖。
	 */
	deleteSharedDependencyRecordsFor(PolicyRelationId, policy_id, 0);
	target.classId = AuthIdRelationId;
	target.objectSubId = 0;
	for (i = 0; i < nitems; i++)
	{
		target.objectId = DatumGetObjectId(role_oids[i]);
		/* no dependency if public */
		/*
		 *
		 * 若是 public 则不建立依赖。
		 */
		if (target.objectId != ACL_ID_PUBLIC)
			recordSharedDependencyOn(&myself, &target,
									 SHARED_DEPENDENCY_POLICY);
	}

	InvokeObjectPostAlterHook(PolicyRelationId, policy_id, 0);

	heap_freetuple(new_tuple);

	/* Invalidate Relation Cache */
	/*
	 *
	 * 使关系缓存失效。
	 */
	CacheInvalidateRelcache(target_table);

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	systable_endscan(sscan);
	relation_close(target_table, NoLock);
	table_close(pg_policy_rel, RowExclusiveLock);

	return myself;
}

/*
 * rename_policy -
 *	 change the name of a policy on a relation
 *
 * rename_policy：更改关系上某个策略的名称。
 */
ObjectAddress
rename_policy(RenameStmt *stmt)
{
	Relation	pg_policy_rel;
	Relation	target_table;
	Oid			table_id;
	Oid			opoloid;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	ObjectAddress address;

	/* Get id of table.  Also handles permissions checks. */
	/*
	 *
	 * 取得表的 id，同时做权限检查。
	 */
	table_id = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
										0,
										RangeVarCallbackForPolicy,
										stmt);

	target_table = relation_open(table_id, NoLock);

	pg_policy_rel = table_open(PolicyRelationId, RowExclusiveLock);

	/* First pass -- check for conflict */
	/*
	 *
	 * 第一遍：检查冲突。
	 */

	/* Add key - policy's relation id. */
	/*
	 *
	 * 添加键：策略的关系 id。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Add key - policy's name. */
	/*
	 *
	 * 添加键：策略名。
	 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->newname));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	if (HeapTupleIsValid(systable_getnext(sscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" already exists",
						stmt->newname, RelationGetRelationName(target_table))));

	systable_endscan(sscan);

	/* Second pass -- find existing policy and update */
	/*
	 *
	 * 第二遍：找到现有策略并更新。
	 */
	/* Add key - policy's relation id. */
	/*
	 *
	 * 添加键：策略的关系 id。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(table_id));

	/* Add key - policy's name. */
	/*
	 *
	 * 添加键：策略名。
	 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->subname));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	/* Complain if we did not find the policy */
	/*
	 *
	 * 找不到策略则报错。
	 */
	if (!HeapTupleIsValid(policy_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(target_table))));

	opoloid = ((Form_pg_policy) GETSTRUCT(policy_tuple))->oid;

	policy_tuple = heap_copytuple(policy_tuple);

	namestrcpy(&((Form_pg_policy) GETSTRUCT(policy_tuple))->polname,
			   stmt->newname);

	CatalogTupleUpdate(pg_policy_rel, &policy_tuple->t_self, policy_tuple);

	InvokeObjectPostAlterHook(PolicyRelationId, opoloid, 0);

	ObjectAddressSet(address, PolicyRelationId, opoloid);

	/*
	 * Invalidate relation's relcache entry so that other backends (and this
	 * one too!) are sent SI message to make them rebuild relcache entries.
	 * (Ideally this should happen automatically...)
	 *
	 * 使关系的 relcache 项失效，以便向其他后端（以及本后端）发送 SI 消息，让它们重建 relcache。
	 * （理想情况下这应自动发生……）
	 */
	CacheInvalidateRelcache(target_table);

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	systable_endscan(sscan);
	table_close(pg_policy_rel, RowExclusiveLock);
	relation_close(target_table, NoLock);

	return address;
}

/*
 * get_relation_policy_oid - Look up a policy by name to find its OID
 *
 * get_relation_policy_oid：按名称查找策略并取得其 OID。
 *
 * If missing_ok is false, throw an error if policy not found.  If
 * true, just return InvalidOid.
 *
 * missing_ok 为 false 时，找不到策略就报错；为 true 时返回 InvalidOid。
 */
Oid
get_relation_policy_oid(Oid relid, const char *policy_name, bool missing_ok)
{
	Relation	pg_policy_rel;
	ScanKeyData skey[2];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	Oid			policy_oid;

	pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);

	/* Add key - policy's relation id. */
	/*
	 *
	 * 添加键：策略的关系 id。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	/* Add key - policy's name. */
	/*
	 *
	 * 添加键：策略名。
	 */
	ScanKeyInit(&skey[1],
				Anum_pg_policy_polname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(policy_name));

	sscan = systable_beginscan(pg_policy_rel,
							   PolicyPolrelidPolnameIndexId, true, NULL, 2,
							   skey);

	policy_tuple = systable_getnext(sscan);

	if (!HeapTupleIsValid(policy_tuple))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("policy \"%s\" for table \"%s\" does not exist",
							policy_name, get_rel_name(relid))));

		policy_oid = InvalidOid;
	}
	else
		policy_oid = ((Form_pg_policy) GETSTRUCT(policy_tuple))->oid;

	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	systable_endscan(sscan);
	table_close(pg_policy_rel, AccessShareLock);

	return policy_oid;
}

/*
 * relation_has_policies - Determine if relation has any policies
 *
 * relation_has_policies：判断关系是否有任何策略。
 */
bool
relation_has_policies(Relation rel)
{
	Relation	catalog;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	bool		ret = false;

	catalog = table_open(PolicyRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_policy_polrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	sscan = systable_beginscan(catalog, PolicyPolrelidPolnameIndexId, true,
							   NULL, 1, &skey);
	policy_tuple = systable_getnext(sscan);
	if (HeapTupleIsValid(policy_tuple))
		ret = true;

	systable_endscan(sscan);
	table_close(catalog, AccessShareLock);

	return ret;
}
