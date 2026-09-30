/*-------------------------------------------------------------------------
 *
 * user.c
 *	  Commands for manipulating roles (formerly called users).
 *
 * 操作角色（旧称用户）的命令。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/commands/user.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/user.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

/*
 * Removing a role grant - or the admin option on it - might recurse to
 * dependent grants. We use these values to reason about what would need to
 * be done in such cases.
 *
 * 撤销角色授权，或仅撤销其上的 admin option，可能递归影响到依赖授权。
 * 下列取值用于描述这类情况下需要执行的操作。
 *
 * RRG_NOOP indicates a grant that would not need to be altered by the
 * operation.
 *
 * RRG_NOOP 表示该授权无需被本操作修改。
 *
 * RRG_REMOVE_ADMIN_OPTION indicates a grant that would need to have
 * admin_option set to false by the operation.
 *
 * RRG_REMOVE_ADMIN_OPTION 表示本操作需要把该授权的 admin_option 设为 false。
 *
 * Similarly, RRG_REMOVE_INHERIT_OPTION and RRG_REMOVE_SET_OPTION indicate
 * grants that would need to have the corresponding options set to false.
 *
 * 类似地，RRG_REMOVE_INHERIT_OPTION 与 RRG_REMOVE_SET_OPTION 表示需要把对应选项设为 false 的授权。
 *
 * RRG_DELETE_GRANT indicates a grant that would need to be removed entirely
 * by the operation.
 *
 * RRG_DELETE_GRANT 表示本操作需要整个删除该授权。
 */
typedef enum
{
	RRG_NOOP,
	RRG_REMOVE_ADMIN_OPTION,
	RRG_REMOVE_INHERIT_OPTION,
	RRG_REMOVE_SET_OPTION,
	RRG_DELETE_GRANT,
} RevokeRoleGrantAction;

/* Potentially set by pg_upgrade_support functions */
/*
 *
 * 可能由 pg_upgrade_support 函数设置
 */
Oid			binary_upgrade_next_pg_authid_oid = InvalidOid;

typedef struct
{
	unsigned	specified;
	bool		admin;
	bool		inherit;
	bool		set;
} GrantRoleOptions;

#define GRANT_ROLE_SPECIFIED_ADMIN			0x0001
#define GRANT_ROLE_SPECIFIED_INHERIT		0x0002
#define GRANT_ROLE_SPECIFIED_SET			0x0004

/* GUC parameters */
/*
 *
 * GUC 参数
 */
int			Password_encryption = PASSWORD_TYPE_SCRAM_SHA_256;
char	   *createrole_self_grant = "";
static bool createrole_self_grant_enabled = false;
static GrantRoleOptions createrole_self_grant_options;

/* Hook to check passwords in CreateRole() and AlterRole() */
/*
 *
 * 在 CreateRole() 与 AlterRole() 中检查口令的钩子
 */
check_password_hook_type check_password_hook = NULL;

static void AddRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
						List *memberSpecs, List *memberIds,
						Oid grantorId, GrantRoleOptions *popt);
static void DelRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
						List *memberSpecs, List *memberIds,
						Oid grantorId, GrantRoleOptions *popt,
						DropBehavior behavior);
static void check_role_membership_authorization(Oid currentUserId, Oid roleid,
												bool is_grant);
static Oid	check_role_grantor(Oid currentUserId, Oid roleid, Oid grantorId,
							   bool is_grant);
static RevokeRoleGrantAction *initialize_revoke_actions(CatCList *memlist);
static bool plan_single_revoke(CatCList *memlist,
							   RevokeRoleGrantAction *actions,
							   Oid member, Oid grantor,
							   GrantRoleOptions *popt,
							   DropBehavior behavior);
static void plan_member_revoke(CatCList *memlist,
							   RevokeRoleGrantAction *actions, Oid member);
static void plan_recursive_revoke(CatCList *memlist,
								  RevokeRoleGrantAction *actions,
								  int index,
								  bool revoke_admin_option_only,
								  DropBehavior behavior);
static void InitGrantRoleOptions(GrantRoleOptions *popt);


/* Check if current user has createrole privileges */
/*
 *
 * 检查当前用户是否拥有 CREATEROLE 权限
 */
static bool
have_createrole_privilege(void)
{
	return has_createrole_privilege(GetUserId());
}


/*
 * 核心流程概览：
 * CreateRole：解析选项、检查权限与保留名，向 pg_authid 插入元组，并处理成员与隐式 ADMIN。
 * AlterRole / AlterRoleSet：修改角色属性、口令、成员，或角色级 GUC。
 * DropRole：检查依赖，清理 pg_auth_members 后删除 pg_authid。
 * GrantRole、AddRoleMems、DelRoleMems：维护 pg_auth_members，处理 ADMIN OPTION 环与级联撤销。
 * RenameRole、DropOwnedObjects、ReassignOwnedObjects：重命名角色，或删除、转移其拥有的对象。
 */

/*
 * CREATE ROLE
 *
 * 创建角色
 */
Oid
CreateRole(ParseState *pstate, CreateRoleStmt *stmt)
{
	Relation	pg_authid_rel;
	TupleDesc	pg_authid_dsc;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_authid] = {0};
	bool		new_record_nulls[Natts_pg_authid] = {0};
	Oid			currentUserId = GetUserId();
	Oid			roleid;
	ListCell   *item;
	ListCell   *option;
	char	   *password = NULL;	/* user password */
	/*
	 *
	 * 用户口令
	 */
	bool		issuper = false;	/* Make the user a superuser? */
	/*
	 *
	 * 是否把该用户设为 superuser？
	 */
	bool		inherit = true; /* Auto inherit privileges? */
	/*
	 *
	 * 是否自动继承权限？
	 */
	bool		createrole = false; /* Can this user create roles? */
	/*
	 *
	 * 该用户能否创建角色？
	 */
	bool		createdb = false;	/* Can the user create databases? */
	/*
	 *
	 * 该用户能否创建数据库？
	 */
	bool		canlogin = false;	/* Can this user login? */
	/*
	 *
	 * 该用户能否登录？
	 */
	bool		isreplication = false;	/* Is this a replication role? */
	/*
	 *
	 * 这是否为复制角色？
	 */
	bool		bypassrls = false;	/* Is this a row security enabled role? */
	/*
	 *
	 * 这是否为启用行级安全的角色？
	 */
	int			connlimit = -1; /* maximum connections allowed */
	/*
	 *
	 * 允许的最大连接数
	 */
	List	   *addroleto = NIL;	/* roles to make this a member of */
	/*
	 *
	 * 要让本角色成为其成员的角色
	 */
	List	   *rolemembers = NIL;	/* roles to be members of this role */
	/*
	 *
	 * 要成为该角色成员的角色
	 */
	List	   *adminmembers = NIL; /* roles to be admins of this role */
	/*
	 *
	 * 要成为该角色管理员的角色
	 */
	char	   *validUntil = NULL;	/* time the login is valid until */
	/*
	 *
	 * 登录的有效截止时间
	 */
	Datum		validUntil_datum;	/* same, as timestamptz Datum */
	/*
	 *
	 * 同上，但是 timestamptz Datum
	 */
	bool		validUntil_null;
	DefElem    *dpassword = NULL;
	DefElem    *dissuper = NULL;
	DefElem    *dinherit = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *disreplication = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *daddroleto = NULL;
	DefElem    *drolemembers = NULL;
	DefElem    *dadminmembers = NULL;
	DefElem    *dvalidUntil = NULL;
	DefElem    *dbypassRLS = NULL;
	GrantRoleOptions popt;

	/* The defaults can vary depending on the original statement type */
	/*
	 *
	 * 默认值可能随原始语句类型而变化
	 */
	switch (stmt->stmt_type)
	{
		case ROLESTMT_ROLE:
			break;
		case ROLESTMT_USER:
			canlogin = true;
			/* may eventually want inherit to default to false here */
			/*
			 *
			 * 将来也许希望这里的 inherit 默认为 false
			 */
			break;
		case ROLESTMT_GROUP:
			break;
	}

	/* Extract options from the statement node tree */
	/*
	 *
	 * 从语句节点树中提取选项
	 */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "password") == 0)
		{
			if (dpassword)
				errorConflictingDefElem(defel, pstate);
			dpassword = defel;
		}
		else if (strcmp(defel->defname, "sysid") == 0)
		{
			ereport(NOTICE,
					(errmsg("SYSID can no longer be specified")));
		}
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (dissuper)
				errorConflictingDefElem(defel, pstate);
			dissuper = defel;
		}
		else if (strcmp(defel->defname, "inherit") == 0)
		{
			if (dinherit)
				errorConflictingDefElem(defel, pstate);
			dinherit = defel;
		}
		else if (strcmp(defel->defname, "createrole") == 0)
		{
			if (dcreaterole)
				errorConflictingDefElem(defel, pstate);
			dcreaterole = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				errorConflictingDefElem(defel, pstate);
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "canlogin") == 0)
		{
			if (dcanlogin)
				errorConflictingDefElem(defel, pstate);
			dcanlogin = defel;
		}
		else if (strcmp(defel->defname, "isreplication") == 0)
		{
			if (disreplication)
				errorConflictingDefElem(defel, pstate);
			disreplication = defel;
		}
		else if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "addroleto") == 0)
		{
			if (daddroleto)
				errorConflictingDefElem(defel, pstate);
			daddroleto = defel;
		}
		else if (strcmp(defel->defname, "rolemembers") == 0)
		{
			if (drolemembers)
				errorConflictingDefElem(defel, pstate);
			drolemembers = defel;
		}
		else if (strcmp(defel->defname, "adminmembers") == 0)
		{
			if (dadminmembers)
				errorConflictingDefElem(defel, pstate);
			dadminmembers = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				errorConflictingDefElem(defel, pstate);
			dvalidUntil = defel;
		}
		else if (strcmp(defel->defname, "bypassrls") == 0)
		{
			if (dbypassRLS)
				errorConflictingDefElem(defel, pstate);
			dbypassRLS = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dpassword && dpassword->arg)
		password = strVal(dpassword->arg);
	if (dissuper)
		issuper = boolVal(dissuper->arg);
	if (dinherit)
		inherit = boolVal(dinherit->arg);
	if (dcreaterole)
		createrole = boolVal(dcreaterole->arg);
	if (dcreatedb)
		createdb = boolVal(dcreatedb->arg);
	if (dcanlogin)
		canlogin = boolVal(dcanlogin->arg);
	if (disreplication)
		isreplication = boolVal(disreplication->arg);
	if (dconnlimit)
	{
		connlimit = intVal(dconnlimit->arg);
		if (connlimit < -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", connlimit)));
	}
	if (daddroleto)
		addroleto = (List *) daddroleto->arg;
	if (drolemembers)
		rolemembers = (List *) drolemembers->arg;
	if (dadminmembers)
		adminmembers = (List *) dadminmembers->arg;
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);
	if (dbypassRLS)
		bypassrls = boolVal(dbypassRLS->arg);

	/* Check some permissions first */
	/*
	 *
	 * 先检查若干权限
	 */
	if (!superuser_arg(currentUserId))
	{
		if (!has_createrole_privilege(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles.",
							   "CREATEROLE")));
		if (issuper)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "SUPERUSER", "SUPERUSER")));
		if (createdb && !have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "CREATEDB", "CREATEDB")));
		if (isreplication && !has_rolreplication(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "REPLICATION", "REPLICATION")));
		if (bypassrls && !has_bypassrls_privilege(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role"),
					 errdetail("Only roles with the %s attribute may create roles with the %s attribute.",
							   "BYPASSRLS", "BYPASSRLS")));
	}

	/*
	 * Check that the user is not trying to create a role in the reserved
	 * "pg_" namespace.
	 *
	 * 检查用户不是在保留的 pg_ 名字空间中创建角色。
	 */
	if (IsReservedName(stmt->role))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						stmt->role),
				 errdetail("Role names starting with \"pg_\" are reserved.")));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for role names are violated.
	 *
	 * 若以相应开关编译，则在角色名违反回归测试约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->role, "regress_", 8) != 0)
		elog(WARNING, "roles created by regression test cases should have names starting with \"regress_\"");
#endif

	/*
	 * Check the pg_authid relation to be certain the role doesn't already
	 * exist.
	 *
	 * 检查 pg_authid，确认该角色尚不存在。
	 */
	pg_authid_rel = table_open(AuthIdRelationId, RowExclusiveLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	if (OidIsValid(get_role_oid(stmt->role, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists",
						stmt->role)));

	/* Convert validuntil to internal form */
	/*
	 *
	 * 把 validuntil 转换成内部形式
	 */
	if (validUntil)
	{
		validUntil_datum = DirectFunctionCall3(timestamptz_in,
											   CStringGetDatum(validUntil),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
		validUntil_null = false;
	}
	else
	{
		validUntil_datum = (Datum) 0;
		validUntil_null = true;
	}

	/*
	 * Call the password checking hook if there is one defined
	 *
	 * 若定义了口令检查钩子，则调用它
	 */
	if (check_password_hook && password)
		(*check_password_hook) (stmt->role,
								password,
								get_password_type(password),
								validUntil_datum,
								validUntil_null);

	/*
	 * Build a tuple to insert
	 *
	 * 构造待插入的元组
	 */
	new_record[Anum_pg_authid_rolname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->role));
	new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(issuper);
	new_record[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(inherit);
	new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(createrole);
	new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(createdb);
	new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(canlogin);
	new_record[Anum_pg_authid_rolreplication - 1] = BoolGetDatum(isreplication);
	new_record[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(connlimit);

	if (password)
	{
		char	   *shadow_pass;
		const char *logdetail = NULL;

		/*
		 * Don't allow an empty password. Libpq treats an empty password the
		 * same as no password at all, and won't even try to authenticate. But
		 * other clients might, so allowing it would be confusing. By clearing
		 * the password when an empty string is specified, the account is
		 * consistently locked for all clients.
		 *
		 * 不允许空口令。libpq 把空口令当成完全没有口令，甚至不会尝试认证。
		 * 但其他客户端可能会，因此允许空口令会造成混淆。
		 * 指定空字符串时清掉口令，这样对所有客户端该账户都处于锁定状态。
		 *
		 * Note that this only covers passwords stored in the database itself.
		 * There are also checks in the authentication code, to forbid an
		 * empty password from being used with authentication methods that
		 * fetch the password from an external system, like LDAP or PAM.
		 *
		 * 注意这只覆盖存放在数据库内部的口令。
		 * 认证代码中还有检查，禁止对从外部系统（如 LDAP 或 PAM）获取口令的认证方法使用空口令。
		 */
		if (password[0] == '\0' ||
			plain_crypt_verify(stmt->role, password, "", &logdetail) == STATUS_OK)
		{
			ereport(NOTICE,
					(errmsg("empty string is not a valid password, clearing password")));
			new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;
		}
		else
		{
			/* Encrypt the password to the requested format. */
			/*
			 *
			 * 按要求的格式加密口令。
			 */
			shadow_pass = encrypt_password(Password_encryption, stmt->role,
										   password);
			new_record[Anum_pg_authid_rolpassword - 1] =
				CStringGetTextDatum(shadow_pass);
		}
	}
	else
		new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;

	new_record[Anum_pg_authid_rolvaliduntil - 1] = validUntil_datum;
	new_record_nulls[Anum_pg_authid_rolvaliduntil - 1] = validUntil_null;

	new_record[Anum_pg_authid_rolbypassrls - 1] = BoolGetDatum(bypassrls);

	/*
	 * pg_largeobject_metadata contains pg_authid.oid's, so we use the
	 * binary-upgrade override.
	 *
	 * pg_largeobject_metadata 包含 pg_authid.oid，因此使用 binary-upgrade 覆盖值。
	 */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_pg_authid_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_authid OID value not set when in binary upgrade mode")));

		roleid = binary_upgrade_next_pg_authid_oid;
		binary_upgrade_next_pg_authid_oid = InvalidOid;
	}
	else
	{
		roleid = GetNewOidWithIndex(pg_authid_rel, AuthIdOidIndexId,
									Anum_pg_authid_oid);
	}

	new_record[Anum_pg_authid_oid - 1] = ObjectIdGetDatum(roleid);

	tuple = heap_form_tuple(pg_authid_dsc, new_record, new_record_nulls);

	/*
	 * Insert new record in the pg_authid table
	 *
	 * 向 pg_authid 表插入新记录
	 */
	CatalogTupleInsert(pg_authid_rel, tuple);

	/*
	 * Advance command counter so we can see new record; else tests in
	 * AddRoleMems may fail.
	 *
	 * 推进命令计数器以便能看到新记录；否则 AddRoleMems 中的检查可能失败。
	 */
	if (addroleto || adminmembers || rolemembers)
		CommandCounterIncrement();

	/* Default grant. */
	/*
	 *
	 * 默认授权。
	 */
	InitGrantRoleOptions(&popt);

	/*
	 * Add the new role to the specified existing roles.
	 *
	 * 把新角色加入指定的现有角色。
	 */
	if (addroleto)
	{
		RoleSpec   *thisrole = makeNode(RoleSpec);
		List	   *thisrole_list = list_make1(thisrole);
		List	   *thisrole_oidlist = list_make1_oid(roleid);

		thisrole->roletype = ROLESPEC_CSTRING;
		thisrole->rolename = stmt->role;
		thisrole->location = -1;

		foreach(item, addroleto)
		{
			RoleSpec   *oldrole = lfirst(item);
			HeapTuple	oldroletup = get_rolespec_tuple(oldrole);
			Form_pg_authid oldroleform = (Form_pg_authid) GETSTRUCT(oldroletup);
			Oid			oldroleid = oldroleform->oid;
			char	   *oldrolename = NameStr(oldroleform->rolname);

			/* can only add this role to roles for which you have rights */
			/*
			 *
			 * 只能把该角色加入你有权操作的角色
			 */
			check_role_membership_authorization(currentUserId, oldroleid, true);
			AddRoleMems(currentUserId, oldrolename, oldroleid,
						thisrole_list,
						thisrole_oidlist,
						InvalidOid, &popt);

			ReleaseSysCache(oldroletup);
		}
	}

	/*
	 * If the current user isn't a superuser, make them an admin of the new
	 * role so that they can administer the new object they just created.
	 * Superusers will be able to do that anyway.
	 *
	 * 若当前用户不是 superuser，则让其成为新角色的 admin，以便管理刚刚创建的对象。
	 * superuser 反正都能这样做。
	 *
	 * The grantor of record for this implicit grant is the bootstrap
	 * superuser, which means that the CREATEROLE user cannot revoke the
	 * grant. They can however grant the created role back to themselves with
	 * different options, since they enjoy ADMIN OPTION on it.
	 *
	 * 这条隐式授权记录的 grantor 是 bootstrap superuser，因此该 CREATEROLE 用户不能撤销它。
	 * 不过他们对该角色拥有 ADMIN OPTION，可以把创建出的角色以不同选项授回给自己。
	 */
	if (!superuser())
	{
		RoleSpec   *current_role = makeNode(RoleSpec);
		GrantRoleOptions poptself;
		List	   *memberSpecs;
		List	   *memberIds = list_make1_oid(currentUserId);

		current_role->roletype = ROLESPEC_CURRENT_ROLE;
		current_role->location = -1;
		memberSpecs = list_make1(current_role);

		poptself.specified = GRANT_ROLE_SPECIFIED_ADMIN
			| GRANT_ROLE_SPECIFIED_INHERIT
			| GRANT_ROLE_SPECIFIED_SET;
		poptself.admin = true;
		poptself.inherit = false;
		poptself.set = false;

		AddRoleMems(BOOTSTRAP_SUPERUSERID, stmt->role, roleid,
					memberSpecs, memberIds,
					BOOTSTRAP_SUPERUSERID, &poptself);

		/*
		 * We must make the implicit grant visible to the code below, else the
		 * additional grants will fail.
		 *
		 * 必须让下面的代码能看到这条隐式授权，否则后续授权会失败。
		 */
		CommandCounterIncrement();

		/*
		 * Because of the implicit grant above, a CREATEROLE user who creates
		 * a role has the ability to grant that role back to themselves with
		 * the INHERIT or SET options, if they wish to inherit the role's
		 * privileges or be able to SET ROLE to it. The createrole_self_grant
		 * GUC can be used to make this happen automatically. This has no
		 * security implications since the same user is able to make the same
		 * grant using an explicit GRANT statement; it's just convenient.
		 *
		 * 由于上面的隐式授权，创建角色的 CREATEROLE 用户可以把该角色再授给自己，并带上 INHERIT 或 SET 选项，
		 * 以便继承该角色的权限或对其执行 SET ROLE。GUC createrole_self_grant 可以让这件事自动发生。
		 * 这没有安全影响，因为同一用户本来就能用显式 GRANT 做出同样的授权；只是更方便。
		 */
		if (createrole_self_grant_enabled)
			AddRoleMems(currentUserId, stmt->role, roleid,
						memberSpecs, memberIds,
						currentUserId, &createrole_self_grant_options);
	}

	/*
	 * Add the specified members to this new role. adminmembers get the admin
	 * option, rolemembers don't.
	 *
	 * 把指定成员加入这个新角色。adminmembers 获得 admin option，rolemembers 则没有。
	 *
	 * NB: No permissions check is required here. If you have enough rights to
	 * create a role, you can add any members you like.
	 *
	 * 注意：这里不需要权限检查。只要有足够权限创建角色，就可以加入任何成员。
	 */
	AddRoleMems(currentUserId, stmt->role, roleid,
				rolemembers, roleSpecsToIds(rolemembers),
				InvalidOid, &popt);
	popt.specified |= GRANT_ROLE_SPECIFIED_ADMIN;
	popt.admin = true;
	AddRoleMems(currentUserId, stmt->role, roleid,
				adminmembers, roleSpecsToIds(adminmembers),
				InvalidOid, &popt);

	/* Post creation hook for new role */
	/*
	 *
	 * 新角色的创建后钩子
	 */
	InvokeObjectPostCreateHook(AuthIdRelationId, roleid, 0);

	/*
	 * Close pg_authid, but keep lock till commit.
	 *
	 * 关闭 pg_authid，但持锁直到提交。
	 */
	table_close(pg_authid_rel, NoLock);

	return roleid;
}


/*
 * ALTER ROLE
 *
 * 修改角色
 *
 * Note: the rolemembers option accepted here is intended to support the
 * backwards-compatible ALTER GROUP syntax.  Although it will work to say
 * "ALTER ROLE role ROLE rolenames", we don't document it.
 *
 * 注意：这里接受 rolemembers 选项是为了支持向后兼容的 ALTER GROUP 语法。
 * 虽然可以说 ALTER ROLE role ROLE rolenames，但我们并不为其写文档。
 */
Oid
AlterRole(ParseState *pstate, AlterRoleStmt *stmt)
{
	Datum		new_record[Natts_pg_authid] = {0};
	bool		new_record_nulls[Natts_pg_authid] = {0};
	bool		new_record_repl[Natts_pg_authid] = {0};
	Relation	pg_authid_rel;
	TupleDesc	pg_authid_dsc;
	HeapTuple	tuple,
				new_tuple;
	Form_pg_authid authform;
	ListCell   *option;
	char	   *rolename;
	char	   *password = NULL;	/* user password */
	/*
	 *
	 * 用户口令
	 */
	int			connlimit = -1; /* maximum connections allowed */
	/*
	 *
	 * 允许的最大连接数
	 */
	char	   *validUntil = NULL;	/* time the login is valid until */
	/*
	 *
	 * 登录的有效截止时间
	 */
	Datum		validUntil_datum;	/* same, as timestamptz Datum */
	/*
	 *
	 * 同上，但是 timestamptz Datum
	 */
	bool		validUntil_null;
	DefElem    *dpassword = NULL;
	DefElem    *dissuper = NULL;
	DefElem    *dinherit = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *disreplication = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *drolemembers = NULL;
	DefElem    *dvalidUntil = NULL;
	DefElem    *dbypassRLS = NULL;
	Oid			roleid;
	Oid			currentUserId = GetUserId();
	GrantRoleOptions popt;

	check_rolespec_name(stmt->role,
						_("Cannot alter reserved roles."));

	/* Extract options from the statement node tree */
	/*
	 *
	 * 从语句节点树中提取选项
	 */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "password") == 0)
		{
			if (dpassword)
				errorConflictingDefElem(defel, pstate);
			dpassword = defel;
		}
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (dissuper)
				errorConflictingDefElem(defel, pstate);
			dissuper = defel;
		}
		else if (strcmp(defel->defname, "inherit") == 0)
		{
			if (dinherit)
				errorConflictingDefElem(defel, pstate);
			dinherit = defel;
		}
		else if (strcmp(defel->defname, "createrole") == 0)
		{
			if (dcreaterole)
				errorConflictingDefElem(defel, pstate);
			dcreaterole = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				errorConflictingDefElem(defel, pstate);
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "canlogin") == 0)
		{
			if (dcanlogin)
				errorConflictingDefElem(defel, pstate);
			dcanlogin = defel;
		}
		else if (strcmp(defel->defname, "isreplication") == 0)
		{
			if (disreplication)
				errorConflictingDefElem(defel, pstate);
			disreplication = defel;
		}
		else if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "rolemembers") == 0 &&
				 stmt->action != 0)
		{
			if (drolemembers)
				errorConflictingDefElem(defel, pstate);
			drolemembers = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				errorConflictingDefElem(defel, pstate);
			dvalidUntil = defel;
		}
		else if (strcmp(defel->defname, "bypassrls") == 0)
		{
			if (dbypassRLS)
				errorConflictingDefElem(defel, pstate);
			dbypassRLS = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dpassword && dpassword->arg)
		password = strVal(dpassword->arg);
	if (dconnlimit)
	{
		connlimit = intVal(dconnlimit->arg);
		if (connlimit < -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", connlimit)));
	}
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);

	/*
	 * Scan the pg_authid relation to be certain the user exists.
	 *
	 * 扫描 pg_authid，确认该用户存在。
	 */
	pg_authid_rel = table_open(AuthIdRelationId, RowExclusiveLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	tuple = get_rolespec_tuple(stmt->role);
	authform = (Form_pg_authid) GETSTRUCT(tuple);
	rolename = pstrdup(NameStr(authform->rolname));
	roleid = authform->oid;

	/* To mess with a superuser in any way you gotta be superuser. */
	/*
	 *
	 * 要以任何方式改动 superuser，自身必须是 superuser。
	 */
	if (!superuser() && authform->rolsuper)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter role"),
				 errdetail("Only roles with the %s attribute may alter roles with the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));
	if (!superuser() && dissuper)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter role"),
				 errdetail("Only roles with the %s attribute may change the %s attribute.",
						   "SUPERUSER", "SUPERUSER")));

	/*
	 * Most changes to a role require that you both have CREATEROLE privileges
	 * and also ADMIN OPTION on the role.
	 *
	 * 对角色的大多数修改都要求同时拥有 CREATEROLE 权限以及对该角色的 ADMIN OPTION。
	 */
	if (!have_createrole_privilege() ||
		!is_admin_of_role(GetUserId(), roleid))
	{
		/* things an unprivileged user certainly can't do */
		/*
		 *
		 * 非特权用户肯定不能做的事
		 */
		if (dinherit || dcreaterole || dcreatedb || dcanlogin || dconnlimit ||
			dvalidUntil || disreplication || dbypassRLS)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may alter this role.",
							   "CREATEROLE", "ADMIN", rolename)));

		/* an unprivileged user can change their own password */
		/*
		 *
		 * 非特权用户可以修改自己的口令
		 */
		if (dpassword && roleid != currentUserId)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("To change another role's password, the current user must have the %s attribute and the %s option on the role.",
							   "CREATEROLE", "ADMIN")));
	}
	else if (!superuser())
	{
		/*
		 * Even if you have both CREATEROLE and ADMIN OPTION on a role, you
		 * can only change the CREATEDB, REPLICATION, or BYPASSRLS attributes
		 * if they are set for your own role (or you are the superuser).
		 *
		 * 即使同时拥有 CREATEROLE 和对该角色的 ADMIN OPTION，
		 * 也只有在自己的角色已设置 CREATEDB、REPLICATION 或 BYPASSRLS（或自己是 superuser）时，才能修改这些属性。
		 */
		if (dcreatedb && !have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute may change the %s attribute.",
							   "CREATEDB", "CREATEDB")));
		if (disreplication && !has_rolreplication(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute may change the %s attribute.",
							   "REPLICATION", "REPLICATION")));
		if (dbypassRLS && !has_bypassrls_privilege(currentUserId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter role"),
					 errdetail("Only roles with the %s attribute may change the %s attribute.",
							   "BYPASSRLS", "BYPASSRLS")));
	}

	/* To add or drop members, you need ADMIN OPTION. */
	/*
	 *
	 * 添加或删除成员需要 ADMIN OPTION。
	 */
	if (drolemembers && !is_admin_of_role(currentUserId, roleid))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter role"),
				 errdetail("Only roles with the %s option on role \"%s\" may add or drop members.",
						   "ADMIN", rolename)));

	/* Convert validuntil to internal form */
	/*
	 *
	 * 把 validuntil 转换成内部形式
	 */
	if (dvalidUntil)
	{
		validUntil_datum = DirectFunctionCall3(timestamptz_in,
											   CStringGetDatum(validUntil),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
		validUntil_null = false;
	}
	else
	{
		/* fetch existing setting in case hook needs it */
		/*
		 *
		 * 取出现有设置，以备钩子需要
		 */
		validUntil_datum = SysCacheGetAttr(AUTHNAME, tuple,
										   Anum_pg_authid_rolvaliduntil,
										   &validUntil_null);
	}

	/*
	 * Call the password checking hook if there is one defined
	 *
	 * 若定义了口令检查钩子，则调用它
	 */
	if (check_password_hook && password)
		(*check_password_hook) (rolename,
								password,
								get_password_type(password),
								validUntil_datum,
								validUntil_null);

	/*
	 * Build an updated tuple, perusing the information just obtained
	 *
	 * 根据刚取得的信息构造更新后的元组
	 */

	/*
	 * issuper/createrole/etc
	 *
	 * issuper、createrole 等属性
	 */
	if (dissuper)
	{
		bool		should_be_super = boolVal(dissuper->arg);

		if (!should_be_super && roleid == BOOTSTRAP_SUPERUSERID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("permission denied to alter role"),
					 errdetail("The bootstrap superuser must have the %s attribute.",
							   "SUPERUSER")));

		new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(should_be_super);
		new_record_repl[Anum_pg_authid_rolsuper - 1] = true;
	}

	if (dinherit)
	{
		new_record[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(boolVal(dinherit->arg));
		new_record_repl[Anum_pg_authid_rolinherit - 1] = true;
	}

	if (dcreaterole)
	{
		new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(boolVal(dcreaterole->arg));
		new_record_repl[Anum_pg_authid_rolcreaterole - 1] = true;
	}

	if (dcreatedb)
	{
		new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(boolVal(dcreatedb->arg));
		new_record_repl[Anum_pg_authid_rolcreatedb - 1] = true;
	}

	if (dcanlogin)
	{
		new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(boolVal(dcanlogin->arg));
		new_record_repl[Anum_pg_authid_rolcanlogin - 1] = true;
	}

	if (disreplication)
	{
		new_record[Anum_pg_authid_rolreplication - 1] = BoolGetDatum(boolVal(disreplication->arg));
		new_record_repl[Anum_pg_authid_rolreplication - 1] = true;
	}

	if (dconnlimit)
	{
		new_record[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(connlimit);
		new_record_repl[Anum_pg_authid_rolconnlimit - 1] = true;
	}

	/* password */
	/*
	 *
	 * 口令
	 */
	if (password)
	{
		char	   *shadow_pass;
		const char *logdetail = NULL;

		/* Like in CREATE USER, don't allow an empty password. */
		/*
		 *
		 * 与 CREATE USER 一样，不允许空口令。
		 */
		if (password[0] == '\0' ||
			plain_crypt_verify(rolename, password, "", &logdetail) == STATUS_OK)
		{
			ereport(NOTICE,
					(errmsg("empty string is not a valid password, clearing password")));
			new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;
		}
		else
		{
			/* Encrypt the password to the requested format. */
			/*
			 *
			 * 按要求的格式加密口令。
			 */
			shadow_pass = encrypt_password(Password_encryption, rolename,
										   password);
			new_record[Anum_pg_authid_rolpassword - 1] =
				CStringGetTextDatum(shadow_pass);
		}
		new_record_repl[Anum_pg_authid_rolpassword - 1] = true;
	}

	/* unset password */
	/*
	 *
	 * 清除口令
	 */
	if (dpassword && dpassword->arg == NULL)
	{
		new_record_repl[Anum_pg_authid_rolpassword - 1] = true;
		new_record_nulls[Anum_pg_authid_rolpassword - 1] = true;
	}

	/* valid until */
	/*
	 *
	 * 有效期
	 */
	new_record[Anum_pg_authid_rolvaliduntil - 1] = validUntil_datum;
	new_record_nulls[Anum_pg_authid_rolvaliduntil - 1] = validUntil_null;
	new_record_repl[Anum_pg_authid_rolvaliduntil - 1] = true;

	if (dbypassRLS)
	{
		new_record[Anum_pg_authid_rolbypassrls - 1] = BoolGetDatum(boolVal(dbypassRLS->arg));
		new_record_repl[Anum_pg_authid_rolbypassrls - 1] = true;
	}

	new_tuple = heap_modify_tuple(tuple, pg_authid_dsc, new_record,
								  new_record_nulls, new_record_repl);
	CatalogTupleUpdate(pg_authid_rel, &tuple->t_self, new_tuple);

	InvokeObjectPostAlterHook(AuthIdRelationId, roleid, 0);

	ReleaseSysCache(tuple);
	heap_freetuple(new_tuple);

	InitGrantRoleOptions(&popt);

	/*
	 * Advance command counter so we can see new record; else tests in
	 * AddRoleMems may fail.
	 *
	 * 推进命令计数器以便能看到新记录；否则 AddRoleMems 中的检查可能失败。
	 */
	if (drolemembers)
	{
		List	   *rolemembers = (List *) drolemembers->arg;

		CommandCounterIncrement();

		if (stmt->action == +1) /* add members to role */
		/*
		 *
		 * 向角色添加成员
		 */
			AddRoleMems(currentUserId, rolename, roleid,
						rolemembers, roleSpecsToIds(rolemembers),
						InvalidOid, &popt);
		else if (stmt->action == -1)	/* drop members from role */
		/*
		 *
		 * 从角色中删除成员
		 */
			DelRoleMems(currentUserId, rolename, roleid,
						rolemembers, roleSpecsToIds(rolemembers),
						InvalidOid, &popt, DROP_RESTRICT);
	}

	/*
	 * Close pg_authid, but keep lock till commit.
	 *
	 * 关闭 pg_authid，但持锁直到提交。
	 */
	table_close(pg_authid_rel, NoLock);

	return roleid;
}


/*
 * ALTER ROLE ... SET
 *
 * 修改角色的配置参数
 */
Oid
AlterRoleSet(AlterRoleSetStmt *stmt)
{
	HeapTuple	roletuple;
	Form_pg_authid roleform;
	Oid			databaseid = InvalidOid;
	Oid			roleid = InvalidOid;

	if (stmt->role)
	{
		check_rolespec_name(stmt->role,
							_("Cannot alter reserved roles."));

		roletuple = get_rolespec_tuple(stmt->role);
		roleform = (Form_pg_authid) GETSTRUCT(roletuple);
		roleid = roleform->oid;

		/*
		 * Obtain a lock on the role and make sure it didn't go away in the
		 * meantime.
		 *
		 * 锁定该角色，并确认它在此期间没有消失。
		 */
		shdepLockAndCheckObject(AuthIdRelationId, roleid);

		/*
		 * To mess with a superuser you gotta be superuser; otherwise you need
		 * CREATEROLE plus admin option on the target role; unless you're just
		 * trying to change your own settings
		 *
		 * 要改动 superuser，自身必须是 superuser；否则需要 CREATEROLE 以及对目标角色的 admin option；
		 * 除非只是在修改自己的设置
		 */
		if (roleform->rolsuper)
		{
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to alter role"),
						 errdetail("Only roles with the %s attribute may alter roles with the %s attribute.",
								   "SUPERUSER", "SUPERUSER")));
		}
		else
		{
			if ((!have_createrole_privilege() ||
				 !is_admin_of_role(GetUserId(), roleid))
				&& roleid != GetUserId())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to alter role"),
						 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may alter this role.",
								   "CREATEROLE", "ADMIN", NameStr(roleform->rolname))));
		}

		ReleaseSysCache(roletuple);
	}

	/* look up and lock the database, if specified */
	/*
	 *
	 * 若指定了数据库，则查找并锁定它
	 */
	if (stmt->database != NULL)
	{
		databaseid = get_database_oid(stmt->database, false);
		shdepLockAndCheckObject(DatabaseRelationId, databaseid);

		if (!stmt->role)
		{
			/*
			 * If no role is specified, then this is effectively the same as
			 * ALTER DATABASE ... SET, so use the same permission check.
			 *
			 * 若未指定角色，则实际上等同于 ALTER DATABASE ... SET，因此使用相同的权限检查。
			 */
			if (!object_ownercheck(DatabaseRelationId, databaseid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
							   stmt->database);
		}
	}

	if (!stmt->role && !stmt->database)
	{
		/* Must be superuser to alter settings globally. */
		/*
		 *
		 * 要全局修改设置，必须是 superuser。
		 */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to alter setting"),
					 errdetail("Only roles with the %s attribute may alter settings globally.",
							   "SUPERUSER")));
	}

	AlterSetting(databaseid, roleid, stmt->setstmt);

	return roleid;
}


/*
 * DROP ROLE
 *
 * 删除角色
 */
void
DropRole(DropRoleStmt *stmt)
{
	Relation	pg_authid_rel,
				pg_auth_members_rel;
	ListCell   *item;
	List	   *role_oids = NIL;

	if (!have_createrole_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to drop role"),
				 errdetail("Only roles with the %s attribute and the %s option on the target roles may drop roles.",
						   "CREATEROLE", "ADMIN")));

	/*
	 * Scan the pg_authid relation to find the Oid of the role(s) to be
	 * deleted and perform preliminary permissions and sanity checks.
	 *
	 * 扫描 pg_authid，找出要删除角色的 Oid，并做初步的权限与健全性检查。
	 */
	pg_authid_rel = table_open(AuthIdRelationId, RowExclusiveLock);
	pg_auth_members_rel = table_open(AuthMemRelationId, RowExclusiveLock);

	foreach(item, stmt->roles)
	{
		RoleSpec   *rolspec = lfirst(item);
		char	   *role;
		HeapTuple	tuple,
					tmp_tuple;
		Form_pg_authid roleform;
		ScanKeyData scankey;
		SysScanDesc sscan;
		Oid			roleid;

		if (rolspec->roletype != ROLESPEC_CSTRING)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot use special role specifier in DROP ROLE")));
		role = rolspec->rolename;

		tuple = SearchSysCache1(AUTHNAME, PointerGetDatum(role));
		if (!HeapTupleIsValid(tuple))
		{
			if (!stmt->missing_ok)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", role)));
			}
			else
			{
				ereport(NOTICE,
						(errmsg("role \"%s\" does not exist, skipping",
								role)));
			}

			continue;
		}

		roleform = (Form_pg_authid) GETSTRUCT(tuple);
		roleid = roleform->oid;

		if (roleid == GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (roleid == GetOuterUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (roleid == GetSessionUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("session user cannot be dropped")));

		/*
		 * For safety's sake, we allow createrole holders to drop ordinary
		 * roles but not superuser roles, and only if they also have ADMIN
		 * OPTION.
		 *
		 * 出于安全，允许持有 createrole 的用户删除普通角色，但不能删除 superuser 角色，并且还必须拥有 ADMIN OPTION。
		 */
		if (roleform->rolsuper && !superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop role"),
					 errdetail("Only roles with the %s attribute may drop roles with the %s attribute.",
							   "SUPERUSER", "SUPERUSER")));
		if (!is_admin_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop role"),
					 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may drop this role.",
							   "CREATEROLE", "ADMIN", NameStr(roleform->rolname))));

		/* DROP hook for the role being removed */
		/*
		 *
		 * 对被删除角色调用 DROP 钩子
		 */
		InvokeObjectDropHook(AuthIdRelationId, roleid, 0);

		/* Don't leak the syscache tuple */
		/*
		 *
		 * 不要泄漏 syscache 元组
		 */
		ReleaseSysCache(tuple);

		/*
		 * Lock the role, so nobody can add dependencies to her while we drop
		 * her.  We keep the lock until the end of transaction.
		 *
		 * 锁定该角色，以免在删除期间有人向其添加依赖。锁保持到事务结束。
		 */
		LockSharedObject(AuthIdRelationId, roleid, 0, AccessExclusiveLock);

		/*
		 * If there is a pg_auth_members entry that has one of the roles to be
		 * dropped as the roleid or member, it should be silently removed, but
		 * if there is a pg_auth_members entry that has one of the roles to be
		 * dropped as the grantor, the operation should fail.
		 *
		 * 若某条 pg_auth_members 把待删除角色当作 roleid 或 member，应静默删除；但若把它当作 grantor，则操作应失败。
		 *
		 * It's possible, however, that a single pg_auth_members entry could
		 * fall into multiple categories - e.g. the user could do "GRANT foo
		 * TO bar GRANTED BY baz" and then "DROP ROLE baz, bar". We want such
		 * an operation to succeed regardless of the order in which the
		 * to-be-dropped roles are passed to DROP ROLE.
		 *
		 * 不过，同一条 pg_auth_members 可能落入多个类别。
		 * 例如用户执行 GRANT foo TO bar GRANTED BY baz，然后 DROP ROLE baz, bar。
		 * 无论待删除角色传给 DROP ROLE 的顺序如何，这种操作都应成功。
		 *
		 * To make that work, we remove all pg_auth_members entries that can
		 * be silently removed in this loop, and then below we'll make a
		 * second pass over the list of roles to be removed and check for any
		 * remaining dependencies.
		 *
		 * 为此，本循环先删除所有可静默删除的 pg_auth_members 条目，
		 * 然后下面再对要删除的角色列表做第二遍，检查是否还有剩余依赖。
		 */
		ScanKeyInit(&scankey,
					Anum_pg_auth_members_roleid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		sscan = systable_beginscan(pg_auth_members_rel, AuthMemRoleMemIndexId,
								   true, NULL, 1, &scankey);

		while (HeapTupleIsValid(tmp_tuple = systable_getnext(sscan)))
		{
			Form_pg_auth_members authmem_form;

			authmem_form = (Form_pg_auth_members) GETSTRUCT(tmp_tuple);
			deleteSharedDependencyRecordsFor(AuthMemRelationId,
											 authmem_form->oid, 0);
			CatalogTupleDelete(pg_auth_members_rel, &tmp_tuple->t_self);
		}

		systable_endscan(sscan);

		ScanKeyInit(&scankey,
					Anum_pg_auth_members_member,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		sscan = systable_beginscan(pg_auth_members_rel, AuthMemMemRoleIndexId,
								   true, NULL, 1, &scankey);

		while (HeapTupleIsValid(tmp_tuple = systable_getnext(sscan)))
		{
			Form_pg_auth_members authmem_form;

			authmem_form = (Form_pg_auth_members) GETSTRUCT(tmp_tuple);
			deleteSharedDependencyRecordsFor(AuthMemRelationId,
											 authmem_form->oid, 0);
			CatalogTupleDelete(pg_auth_members_rel, &tmp_tuple->t_self);
		}

		systable_endscan(sscan);

		/*
		 * Advance command counter so that later iterations of this loop will
		 * see the changes already made.  This is essential if, for example,
		 * we are trying to drop both a role and one of its direct members ---
		 * we'll get an error if we try to delete the linking pg_auth_members
		 * tuple twice.  (We do not need a CCI between the two delete loops
		 * above, because it's not allowed for a role to directly contain
		 * itself.)
		 *
		 * 推进命令计数器，使本循环后续迭代能看到已做的修改。
		 * 例如同时删除一个角色及其某个直接成员时，这一点必不可少，否则把连接它们的 pg_auth_members 元组删除两次就会报错。
		 * 上面两个删除循环之间不需要 CCI，因为不允许角色直接包含自身。
		 */
		CommandCounterIncrement();

		/* Looks tentatively OK, add it to the list if not there yet. */
		/*
		 *
		 * 暂且看起来没问题，若尚未在列表中则加入。
		 */
		role_oids = list_append_unique_oid(role_oids, roleid);
	}

	/*
	 * Second pass over the roles to be removed.
	 *
	 * 对要删除的角色做第二遍处理。
	 */
	foreach(item, role_oids)
	{
		Oid			roleid = lfirst_oid(item);
		HeapTuple	tuple;
		Form_pg_authid roleform;
		char	   *detail;
		char	   *detail_log;

		/*
		 * Re-find the pg_authid tuple.
		 *
		 * 重新查找 pg_authid 元组。
		 *
		 * Since we've taken a lock on the role OID, it shouldn't be possible
		 * for the tuple to have been deleted -- or for that matter updated --
		 * unless the user is manually modifying the system catalogs.
		 *
		 * 既然已经锁定该角色 OID，除非用户手动修改系统目录，否则元组不应已被删除，也不应已被更新。
		 */
		tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "could not find tuple for role %u", roleid);
		roleform = (Form_pg_authid) GETSTRUCT(tuple);

		/*
		 * Check for pg_shdepend entries depending on this role.
		 *
		 * 检查是否有依赖于该角色的 pg_shdepend 条目。
		 *
		 * This needs to happen after we've completed removing any
		 * pg_auth_members entries that can be removed silently, in order to
		 * avoid spurious failures. See notes above for more details.
		 *
		 * 这必须在静默删除所有可静默删除的 pg_auth_members 条目之后进行，以免产生虚假失败。详见上面的说明。
		 */
		if (checkSharedDependencies(AuthIdRelationId, roleid,
									&detail, &detail_log))
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("role \"%s\" cannot be dropped because some objects depend on it",
							NameStr(roleform->rolname)),
					 errdetail_internal("%s", detail),
					 errdetail_log("%s", detail_log)));

		/*
		 * Remove the role from the pg_authid table
		 *
		 * 从 pg_authid 表中删除该角色
		 */
		CatalogTupleDelete(pg_authid_rel, &tuple->t_self);

		ReleaseSysCache(tuple);

		/*
		 * Remove any comments or security labels on this role.
		 *
		 * 删除该角色上的注释或安全标签。
		 */
		DeleteSharedComments(roleid, AuthIdRelationId);
		DeleteSharedSecurityLabel(roleid, AuthIdRelationId);

		/*
		 * Remove settings for this role.
		 *
		 * 删除该角色的设置。
		 */
		DropSetting(InvalidOid, roleid);
	}

	/*
	 * Now we can clean up; but keep locks until commit.
	 *
	 * 现在可以清理；但持锁直到提交。
	 */
	table_close(pg_auth_members_rel, NoLock);
	table_close(pg_authid_rel, NoLock);
}

/*
 * Rename role
 *
 * 重命名角色
 */
ObjectAddress
RenameRole(const char *oldname, const char *newname)
{
	HeapTuple	oldtuple,
				newtuple;
	TupleDesc	dsc;
	Relation	rel;
	Datum		datum;
	bool		isnull;
	Datum		repl_val[Natts_pg_authid];
	bool		repl_null[Natts_pg_authid];
	bool		repl_repl[Natts_pg_authid];
	int			i;
	Oid			roleid;
	ObjectAddress address;
	Form_pg_authid authform;

	rel = table_open(AuthIdRelationId, RowExclusiveLock);
	dsc = RelationGetDescr(rel);

	oldtuple = SearchSysCache1(AUTHNAME, CStringGetDatum(oldname));
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", oldname)));

	/*
	 * XXX Client applications probably store the session user somewhere, so
	 * renaming it could cause confusion.  On the other hand, there may not be
	 * an actual problem besides a little confusion, so think about this and
	 * decide.  Same for SET ROLE ... we don't restrict renaming the current
	 * effective userid, though.
	 *
	 * XXX：客户端应用多半会在某处保存会话用户，重命名可能造成混淆。
	 * 另一方面，除了一点混淆外也许并没有实际问题，需要再考虑后决定。
	 * SET ROLE 同样如此，不过我们并不限制重命名当前的有效 userid。
	 */

	authform = (Form_pg_authid) GETSTRUCT(oldtuple);
	roleid = authform->oid;

	if (roleid == GetSessionUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("session user cannot be renamed")));
	if (roleid == GetOuterUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current user cannot be renamed")));

	/*
	 * Check that the user is not trying to rename a system role and not
	 * trying to rename a role into the reserved "pg_" namespace.
	 *
	 * 检查用户不是在重命名系统角色，也不是把角色重命名进保留的 pg_ 名字空间。
	 */
	if (IsReservedName(NameStr(authform->rolname)))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						NameStr(authform->rolname)),
				 errdetail("Role names starting with \"pg_\" are reserved.")));

	if (IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						newname),
				 errdetail("Role names starting with \"pg_\" are reserved.")));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for role names are violated.
	 *
	 * 若以相应开关编译，则在角色名违反回归测试约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(newname, "regress_", 8) != 0)
		elog(WARNING, "roles created by regression test cases should have names starting with \"regress_\"");
#endif

	/* make sure the new name doesn't exist */
	/*
	 *
	 * 确认新名称尚不存在
	 */
	if (SearchSysCacheExists1(AUTHNAME, CStringGetDatum(newname)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists", newname)));

	/*
	 * Only superusers can mess with superusers. Otherwise, a user with
	 * CREATEROLE can rename a role for which they have ADMIN OPTION.
	 *
	 * 只有 superuser 能改动 superuser。否则，拥有 CREATEROLE 的用户可以重命名自己对其拥有 ADMIN OPTION 的角色。
	 */
	if (authform->rolsuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to rename role"),
					 errdetail("Only roles with the %s attribute may rename roles with the %s attribute.",
							   "SUPERUSER", "SUPERUSER")));
	}
	else
	{
		if (!have_createrole_privilege() ||
			!is_admin_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to rename role"),
					 errdetail("Only roles with the %s attribute and the %s option on role \"%s\" may rename this role.",
							   "CREATEROLE", "ADMIN", NameStr(authform->rolname))));
	}

	/* OK, construct the modified tuple */
	/*
	 *
	 * 可以，构造修改后的元组
	 */
	for (i = 0; i < Natts_pg_authid; i++)
		repl_repl[i] = false;

	repl_repl[Anum_pg_authid_rolname - 1] = true;
	repl_val[Anum_pg_authid_rolname - 1] = DirectFunctionCall1(namein,
															   CStringGetDatum(newname));
	repl_null[Anum_pg_authid_rolname - 1] = false;

	datum = heap_getattr(oldtuple, Anum_pg_authid_rolpassword, dsc, &isnull);

	if (!isnull && get_password_type(TextDatumGetCString(datum)) == PASSWORD_TYPE_MD5)
	{
		/* MD5 uses the username as salt, so just clear it on a rename */
		/*
		 *
		 * MD5 用用户名作盐，因此重命名时直接清掉口令
		 */
		repl_repl[Anum_pg_authid_rolpassword - 1] = true;
		repl_null[Anum_pg_authid_rolpassword - 1] = true;

		ereport(NOTICE,
				(errmsg("MD5 password cleared because of role rename")));
	}

	newtuple = heap_modify_tuple(oldtuple, dsc, repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(rel, &oldtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(AuthIdRelationId, roleid, 0);

	ObjectAddressSet(address, AuthIdRelationId, roleid);

	ReleaseSysCache(oldtuple);

	/*
	 * Close pg_authid, but keep lock till commit.
	 *
	 * 关闭 pg_authid，但持锁直到提交。
	 */
	table_close(rel, NoLock);

	return address;
}

/*
 * GrantRoleStmt
 *
 * 授予或撤销角色成员资格
 *
 * Grant/Revoke roles to/from roles
 *
 * 在角色之间授予或撤销角色
 */
void
GrantRole(ParseState *pstate, GrantRoleStmt *stmt)
{
	Relation	pg_authid_rel;
	Oid			grantor;
	List	   *grantee_ids;
	ListCell   *item;
	GrantRoleOptions popt;
	Oid			currentUserId = GetUserId();

	/* Parse options list. */
	/*
	 *
	 * 解析选项列表。
	 */
	InitGrantRoleOptions(&popt);
	foreach(item, stmt->opt)
	{
		DefElem    *opt = (DefElem *) lfirst(item);
		char	   *optval = defGetString(opt);

		if (strcmp(opt->defname, "admin") == 0)
		{
			popt.specified |= GRANT_ROLE_SPECIFIED_ADMIN;

			if (parse_bool(optval, &popt.admin))
				continue;
		}
		else if (strcmp(opt->defname, "inherit") == 0)
		{
			popt.specified |= GRANT_ROLE_SPECIFIED_INHERIT;
			if (parse_bool(optval, &popt.inherit))
				continue;
		}
		else if (strcmp(opt->defname, "set") == 0)
		{
			popt.specified |= GRANT_ROLE_SPECIFIED_SET;
			if (parse_bool(optval, &popt.set))
				continue;
		}
		else
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("unrecognized role option \"%s\"", opt->defname),
					parser_errposition(pstate, opt->location));

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized value for role option \"%s\": \"%s\"",
						opt->defname, optval),
				 parser_errposition(pstate, opt->location)));
	}

	/* Lookup OID of grantor, if specified. */
	/*
	 *
	 * 若指定了 grantor，查找其 OID。
	 */
	if (stmt->grantor)
		grantor = get_rolespec_oid(stmt->grantor, false);
	else
		grantor = InvalidOid;

	grantee_ids = roleSpecsToIds(stmt->grantee_roles);

	/* AccessShareLock is enough since we aren't modifying pg_authid */
	/*
	 *
	 * 既然不修改 pg_authid，AccessShareLock 就足够了
	 */
	pg_authid_rel = table_open(AuthIdRelationId, AccessShareLock);

	/*
	 * Step through all of the granted roles and add, update, or remove
	 * entries in pg_auth_members as appropriate. If stmt->is_grant is true,
	 * we are adding new grants or, if they already exist, updating options on
	 * those grants. If stmt->is_grant is false, we are revoking grants or
	 * removing options from them.
	 *
	 * 遍历所有被授予的角色，按情况在 pg_auth_members 中添加、更新或删除条目。
	 * 若 stmt->is_grant 为 true，则添加新授权，或在已存在时更新其选项。
	 * 若 stmt->is_grant 为 false，则撤销授权或从中移除选项。
	 */
	foreach(item, stmt->granted_roles)
	{
		AccessPriv *priv = (AccessPriv *) lfirst(item);
		char	   *rolename = priv->priv_name;
		Oid			roleid;

		/* Must reject priv(columns) and ALL PRIVILEGES(columns) */
		/*
		 *
		 * 必须拒绝 priv(columns) 和 ALL PRIVILEGES(columns)
		 */
		if (rolename == NULL || priv->cols != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("column names cannot be included in GRANT/REVOKE ROLE")));

		roleid = get_role_oid(rolename, false);
		check_role_membership_authorization(currentUserId,
											roleid, stmt->is_grant);
		if (stmt->is_grant)
			AddRoleMems(currentUserId, rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						grantor, &popt);
		else
			DelRoleMems(currentUserId, rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						grantor, &popt, stmt->behavior);
	}

	/*
	 * Close pg_authid, but keep lock till commit.
	 *
	 * 关闭 pg_authid，但持锁直到提交。
	 */
	table_close(pg_authid_rel, NoLock);
}

/*
 * DropOwnedObjects
 *
 * 删除这些角色拥有的对象
 *
 * Drop the objects owned by a given list of roles.
 *
 * 删除给定角色列表所拥有的对象。
 */
void
DropOwnedObjects(DropOwnedStmt *stmt)
{
	List	   *role_ids = roleSpecsToIds(stmt->roles);
	ListCell   *cell;

	/* Check privileges */
	/*
	 *
	 * 检查权限
	 */
	foreach(cell, role_ids)
	{
		Oid			roleid = lfirst_oid(cell);

		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop objects"),
					 errdetail("Only roles with privileges of role \"%s\" may drop objects owned by it.",
							   GetUserNameFromId(roleid, false))));
	}

	/* Ok, do it */
	/*
	 *
	 * 可以，执行操作
	 */
	shdepDropOwned(role_ids, stmt->behavior);
}

/*
 * ReassignOwnedObjects
 *
 * 把这些角色拥有的对象转给其他用户
 *
 * Give the objects owned by a given list of roles away to another user.
 *
 * 把给定角色列表所拥有的对象转给另一用户。
 */
void
ReassignOwnedObjects(ReassignOwnedStmt *stmt)
{
	List	   *role_ids = roleSpecsToIds(stmt->roles);
	ListCell   *cell;
	Oid			newrole;

	/* Check privileges */
	/*
	 *
	 * 检查权限
	 */
	foreach(cell, role_ids)
	{
		Oid			roleid = lfirst_oid(cell);

		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to reassign objects"),
					 errdetail("Only roles with privileges of role \"%s\" may reassign objects owned by it.",
							   GetUserNameFromId(roleid, false))));
	}

	/* Must have privileges on the receiving side too */
	/*
	 *
	 * 接收方也必须拥有相应权限
	 */
	newrole = get_rolespec_oid(stmt->newrole, false);

	if (!has_privs_of_role(GetUserId(), newrole))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reassign objects"),
				 errdetail("Only roles with privileges of role \"%s\" may reassign objects to it.",
						   GetUserNameFromId(newrole, false))));

	/* Ok, do it */
	/*
	 *
	 * 可以，执行操作
	 */
	shdepReassignOwned(role_ids, newrole);
}

/*
 * roleSpecsToIds
 *
 * 把 RoleSpec 列表转换成角色 OID
 *
 * Given a list of RoleSpecs, generate a list of role OIDs in the same order.
 *
 * 给定 RoleSpec 列表，按相同顺序生成角色 OID 列表。
 *
 * ROLESPEC_PUBLIC is not allowed.
 *
 * 不允许 ROLESPEC_PUBLIC。
 */
List *
roleSpecsToIds(List *memberNames)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, memberNames)
	{
		RoleSpec   *rolespec = lfirst_node(RoleSpec, l);
		Oid			roleid;

		roleid = get_rolespec_oid(rolespec, false);
		result = lappend_oid(result, roleid);
	}
	return result;
}

/*
 * AddRoleMems -- Add given members to the specified role
 *
 * AddRoleMems -- 把给定成员加入指定角色
 *
 * currentUserId: OID of role performing the operation
 * rolename: name of role to add to (used only for error messages)
 * roleid: OID of role to add to
 * memberSpecs: list of RoleSpec of roles to add (used only for error messages)
 * memberIds: OIDs of roles to add
 * grantorId: OID that should be recorded as having granted the membership
 * (InvalidOid if not set explicitly)
 * popt: information about grant options
 *
 * currentUserId：执行操作的角色 OID
 * rolename：要加入到的角色名（仅用于报错）
 * roleid：要加入到的角色 OID
 * memberSpecs：要加入的角色的 RoleSpec 列表（仅用于报错）
 * memberIds：要加入的角色 OID
 * grantorId：应记为授予该成员资格的 OID（未显式设置时为 InvalidOid）
 * popt：授权选项信息
 */
static void
AddRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
			List *memberSpecs, List *memberIds,
			Oid grantorId, GrantRoleOptions *popt)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell   *specitem;
	ListCell   *iditem;

	Assert(list_length(memberSpecs) == list_length(memberIds));

	/* Validate grantor (and resolve implicit grantor if not specified). */
	/*
	 *
	 * 校验 grantor（若未指定则解析隐式 grantor）。
	 */
	grantorId = check_role_grantor(currentUserId, roleid, grantorId, true);

	pg_authmem_rel = table_open(AuthMemRelationId, RowExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	/*
	 * Only allow changes to this role by one backend at a time, so that we
	 * can check integrity constraints like the lack of circular ADMIN OPTION
	 * grants without fear of race conditions.
	 *
	 * 同一时刻只允许一个后端修改该角色，以便检查不存在循环 ADMIN OPTION 授权之类的完整性约束，而不必担心竞态。
	 */
	LockSharedObject(AuthIdRelationId, roleid, 0,
					 ShareUpdateExclusiveLock);

	/* Preliminary sanity checks. */
	/*
	 *
	 * 初步健全性检查。
	 */
	forboth(specitem, memberSpecs, iditem, memberIds)
	{
		RoleSpec   *memberRole = lfirst_node(RoleSpec, specitem);
		Oid			memberid = lfirst_oid(iditem);

		/*
		 * pg_database_owner is never a role member.  Lifting this restriction
		 * would require a policy decision about membership loops.  One could
		 * prevent loops, which would include making "ALTER DATABASE x OWNER
		 * TO proposed_datdba" fail if is_member_of_role(pg_database_owner,
		 * proposed_datdba).  Hence, gaining a membership could reduce what a
		 * role could do.  Alternately, one could allow these memberships to
		 * complete loops.  A role could then have actual WITH ADMIN OPTION on
		 * itself, prompting a decision about is_admin_of_role() treatment of
		 * the case.
		 *
		 * pg_database_owner 永远不能成为角色成员。取消这一限制需要先对成员环作出策略决定。
		 * 可以禁止成环，那就会让 is_member_of_role(pg_database_owner, proposed_datdba) 为真时
		 * ALTER DATABASE x OWNER TO proposed_datdba 失败。于是获得成员资格反而可能减少角色能做的事。
		 * 也可以允许这些成员关系成环。那样角色就可能真正对自己拥有 WITH ADMIN OPTION，
		 * 从而需要决定 is_admin_of_role() 如何处理该情况。
		 *
		 * Lifting this restriction also has policy implications for ownership
		 * of shared objects (databases and tablespaces).  We allow such
		 * ownership, but we might find cause to ban it in the future.
		 * Designing such a ban would more troublesome if the design had to
		 * address pg_database_owner being a member of role FOO that owns a
		 * shared object.  (The effect of such ownership is that any owner of
		 * another database can act as the owner of affected shared objects.)
		 *
		 * 取消这一限制还会影响共享对象（数据库和表空间）的属主权策略。目前允许这种属主，但将来也许会禁止。
		 * 若设计时还要处理 pg_database_owner 是拥有共享对象的角色 FOO 的成员，这种禁止会更麻烦。
		 * 这种属主关系的效果是：任何其他数据库的属主都能充当受影响共享对象的属主。
		 */
		if (memberid == ROLE_PG_DATABASE_OWNER)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("role \"%s\" cannot be a member of any role",
						   get_rolespec_name(memberRole)));

		/*
		 * Refuse creation of membership loops, including the trivial case
		 * where a role is made a member of itself.  We do this by checking to
		 * see if the target role is already a member of the proposed member
		 * role.  We have to ignore possible superuserness, however, else we
		 * could never grant membership in a superuser-privileged role.
		 *
		 * 拒绝形成成员环，包括角色成为自身成员这种平凡情况。
		 * 做法是检查目标角色是否已经是拟加入成员角色的成员。
		 * 必须忽略可能的 superuser 身份，否则永远无法向拥有 superuser 权限的角色授予成员资格。
		 */
		if (is_member_of_role_nosuper(roleid, memberid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("role \"%s\" is a member of role \"%s\"",
							rolename, get_rolespec_name(memberRole))));
	}

	/*
	 * Disallow attempts to grant ADMIN OPTION back to a user who granted it
	 * to you, similar to what check_circularity does for ACLs. We want the
	 * chains of grants to remain acyclic, so that it's always possible to use
	 * REVOKE .. CASCADE to clean up all grants that depend on the one being
	 * revoked.
	 *
	 * 禁止把 ADMIN OPTION 授回当初把它授给你的用户，类似于 check_circularity 对 ACL 的处理。
	 * 授权链必须保持无环，以便始终可以用 REVOKE .. CASCADE 清掉依赖于被撤销授权的全部授权。
	 *
	 * NB: This check might look redundant with the check for membership loops
	 * above, but it isn't. That's checking for role-member loop (e.g. A is a
	 * member of B and B is a member of A) while this is checking for a
	 * member-grantor loop (e.g. A gave ADMIN OPTION on X to B and now B, who
	 * has no other source of ADMIN OPTION on X, tries to give ADMIN OPTION on
	 * X back to A).
	 *
	 * 注意：这项检查看起来与上面的成员环检查重复，其实不然。
	 * 那是检查角色与成员之间的环（例如 A 是 B 的成员且 B 是 A 的成员），
	 * 而这里检查的是成员与 grantor 之间的环（例如 A 把 X 的 ADMIN OPTION 授给 B，
	 * 而 B 没有 X 的其他 ADMIN OPTION 来源，现在又试图把 X 的 ADMIN OPTION 授回给 A）。
	 */
	if (popt->admin && grantorId != BOOTSTRAP_SUPERUSERID)
	{
		CatCList   *memlist;
		RevokeRoleGrantAction *actions;
		int			i;

		/* Get the list of members for this role. */
		/*
		 *
		 * 取得该角色的成员列表。
		 */
		memlist = SearchSysCacheList1(AUTHMEMROLEMEM,
									  ObjectIdGetDatum(roleid));

		/*
		 * Figure out what would happen if we removed all existing grants to
		 * every role to which we've been asked to make a new grant.
		 *
		 * 设想若删掉我们被要求新授权的每个角色上的全部现存授权，会发生什么。
		 */
		actions = initialize_revoke_actions(memlist);
		foreach(iditem, memberIds)
		{
			Oid			memberid = lfirst_oid(iditem);

			if (memberid == BOOTSTRAP_SUPERUSERID)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("%s option cannot be granted back to your own grantor",
								"ADMIN")));
			plan_member_revoke(memlist, actions, memberid);
		}

		/*
		 * If the result would be that the grantor role would no longer have
		 * the ability to perform the grant, then the proposed grant would
		 * create a circularity.
		 *
		 * 若结果会让 grantor 角色不再有能力执行该授权，则拟议的授权会形成环。
		 */
		for (i = 0; i < memlist->n_members; ++i)
		{
			HeapTuple	authmem_tuple;
			Form_pg_auth_members authmem_form;

			authmem_tuple = &memlist->members[i]->tuple;
			authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

			if (actions[i] == RRG_NOOP &&
				authmem_form->member == grantorId &&
				authmem_form->admin_option)
				break;
		}
		if (i >= memlist->n_members)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("%s option cannot be granted back to your own grantor",
							"ADMIN")));

		ReleaseSysCacheList(memlist);
	}

	/* Now perform the catalog updates. */
	/*
	 *
	 * 现在执行目录更新。
	 */
	forboth(specitem, memberSpecs, iditem, memberIds)
	{
		RoleSpec   *memberRole = lfirst_node(RoleSpec, specitem);
		Oid			memberid = lfirst_oid(iditem);
		HeapTuple	authmem_tuple;
		HeapTuple	tuple;
		Datum		new_record[Natts_pg_auth_members] = {0};
		bool		new_record_nulls[Natts_pg_auth_members] = {0};
		bool		new_record_repl[Natts_pg_auth_members] = {0};

		/* Common initialization for possible insert or update */
		/*
		 *
		 * 可能插入或更新时的公共初始化
		 */
		new_record[Anum_pg_auth_members_roleid - 1] =
			ObjectIdGetDatum(roleid);
		new_record[Anum_pg_auth_members_member - 1] =
			ObjectIdGetDatum(memberid);
		new_record[Anum_pg_auth_members_grantor - 1] =
			ObjectIdGetDatum(grantorId);

		/* Find any existing tuple */
		/*
		 *
		 * 查找是否已有元组
		 */
		authmem_tuple = SearchSysCache3(AUTHMEMROLEMEM,
										ObjectIdGetDatum(roleid),
										ObjectIdGetDatum(memberid),
										ObjectIdGetDatum(grantorId));

		/*
		 * If we found a tuple, update it with new option values, unless there
		 * are no changes, in which case issue a WARNING.
		 *
		 * 若找到元组，则用新选项值更新；若没有任何变化，则发出 WARNING。
		 *
		 * If we didn't find a tuple, just insert one.
		 *
		 * 若未找到元组，直接插入一条。
		 */
		if (HeapTupleIsValid(authmem_tuple))
		{
			Form_pg_auth_members authmem_form;
			bool		at_least_one_change = false;

			authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

			if ((popt->specified & GRANT_ROLE_SPECIFIED_ADMIN) != 0
				&& authmem_form->admin_option != popt->admin)
			{
				new_record[Anum_pg_auth_members_admin_option - 1] =
					BoolGetDatum(popt->admin);
				new_record_repl[Anum_pg_auth_members_admin_option - 1] =
					true;
				at_least_one_change = true;
			}

			if ((popt->specified & GRANT_ROLE_SPECIFIED_INHERIT) != 0
				&& authmem_form->inherit_option != popt->inherit)
			{
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					BoolGetDatum(popt->inherit);
				new_record_repl[Anum_pg_auth_members_inherit_option - 1] =
					true;
				at_least_one_change = true;
			}

			if ((popt->specified & GRANT_ROLE_SPECIFIED_SET) != 0
				&& authmem_form->set_option != popt->set)
			{
				new_record[Anum_pg_auth_members_set_option - 1] =
					BoolGetDatum(popt->set);
				new_record_repl[Anum_pg_auth_members_set_option - 1] =
					true;
				at_least_one_change = true;
			}

			if (!at_least_one_change)
			{
				ereport(NOTICE,
						(errmsg("role \"%s\" has already been granted membership in role \"%s\" by role \"%s\"",
								get_rolespec_name(memberRole), rolename,
								GetUserNameFromId(grantorId, false))));
				ReleaseSysCache(authmem_tuple);
				continue;
			}

			tuple = heap_modify_tuple(authmem_tuple, pg_authmem_dsc,
									  new_record,
									  new_record_nulls, new_record_repl);
			CatalogTupleUpdate(pg_authmem_rel, &tuple->t_self, tuple);

			ReleaseSysCache(authmem_tuple);
		}
		else
		{
			Oid			objectId;
			Oid		   *newmembers = palloc(sizeof(Oid));

			/*
			 * The values for these options can be taken directly from 'popt'.
			 * Either they were specified, or the defaults as set by
			 * InitGrantRoleOptions are correct.
			 *
			 * 这些选项的值可直接取自 popt。
			 * 要么由用户指定，要么 InitGrantRoleOptions 设置的默认值是正确的。
			 */
			new_record[Anum_pg_auth_members_admin_option - 1] =
				BoolGetDatum(popt->admin);
			new_record[Anum_pg_auth_members_set_option - 1] =
				BoolGetDatum(popt->set);

			/*
			 * If the user specified a value for the inherit option, use
			 * whatever was specified. Otherwise, set the default value based
			 * on the role-level property.
			 *
			 * 若用户指定了 inherit 选项，则使用指定值。否则按角色级属性设置默认值。
			 */
			if ((popt->specified & GRANT_ROLE_SPECIFIED_INHERIT) != 0)
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					popt->inherit;
			else
			{
				HeapTuple	mrtup;
				Form_pg_authid mrform;

				mrtup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(memberid));
				if (!HeapTupleIsValid(mrtup))
					elog(ERROR, "cache lookup failed for role %u", memberid);
				mrform = (Form_pg_authid) GETSTRUCT(mrtup);
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					mrform->rolinherit;
				ReleaseSysCache(mrtup);
			}

			/* get an OID for the new row and insert it */
			/*
			 *
			 * 为新行分配 OID 并插入
			 */
			objectId = GetNewOidWithIndex(pg_authmem_rel, AuthMemOidIndexId,
										  Anum_pg_auth_members_oid);
			new_record[Anum_pg_auth_members_oid - 1] = objectId;
			tuple = heap_form_tuple(pg_authmem_dsc,
									new_record, new_record_nulls);
			CatalogTupleInsert(pg_authmem_rel, tuple);

			/* updateAclDependencies wants to pfree array inputs */
			/*
			 *
			 * updateAclDependencies 会 pfree 传入的数组
			 */
			newmembers[0] = grantorId;
			updateAclDependencies(AuthMemRelationId, objectId,
								  0, InvalidOid,
								  0, NULL,
								  1, newmembers);
		}

		/* CCI after each change, in case there are duplicates in list */
		/*
		 *
		 * 每次修改后执行 CCI，以防列表中有重复项
		 */
		CommandCounterIncrement();
	}

	/*
	 * Close pg_authmem, but keep lock till commit.
	 *
	 * 关闭 pg_authmem，但持锁直到提交。
	 */
	table_close(pg_authmem_rel, NoLock);
}

/*
 * DelRoleMems -- Remove given members from the specified role
 *
 * DelRoleMems -- 从指定角色中移除给定成员
 *
 * rolename: name of role to del from (used only for error messages)
 * roleid: OID of role to del from
 * memberSpecs: list of RoleSpec of roles to del (used only for error messages)
 * memberIds: OIDs of roles to del
 * grantorId: who is revoking the membership
 * popt: information about grant options
 * behavior: RESTRICT or CASCADE behavior for recursive removal
 *
 * rolename：要移出的角色名（仅用于报错）
 * roleid：要移出的角色 OID
 * memberSpecs：要删除的角色的 RoleSpec 列表（仅用于报错）
 * memberIds：要删除的角色 OID
 * grantorId：正在撤销成员资格的角色
 * popt：授权选项信息
 * behavior：递归删除时的 RESTRICT 或 CASCADE 行为
 */
static void
DelRoleMems(Oid currentUserId, const char *rolename, Oid roleid,
			List *memberSpecs, List *memberIds,
			Oid grantorId, GrantRoleOptions *popt, DropBehavior behavior)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell   *specitem;
	ListCell   *iditem;
	CatCList   *memlist;
	RevokeRoleGrantAction *actions;
	int			i;

	Assert(list_length(memberSpecs) == list_length(memberIds));

	/* Validate grantor (and resolve implicit grantor if not specified). */
	/*
	 *
	 * 校验 grantor（若未指定则解析隐式 grantor）。
	 */
	grantorId = check_role_grantor(currentUserId, roleid, grantorId, false);

	pg_authmem_rel = table_open(AuthMemRelationId, RowExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	/*
	 * Only allow changes to this role by one backend at a time, so that we
	 * can check for things like dependent privileges without fear of race
	 * conditions.
	 *
	 * 同一时刻只允许一个后端修改该角色，以便检查依赖权限之类的约束，而不必担心竞态。
	 */
	LockSharedObject(AuthIdRelationId, roleid, 0,
					 ShareUpdateExclusiveLock);

	memlist = SearchSysCacheList1(AUTHMEMROLEMEM, ObjectIdGetDatum(roleid));
	actions = initialize_revoke_actions(memlist);

	/*
	 * We may need to recurse to dependent privileges if DROP_CASCADE was
	 * specified, or refuse to perform the operation if dependent privileges
	 * exist and DROP_RESTRICT was specified. plan_single_revoke() will figure
	 * out what to do with each catalog tuple.
	 *
	 * 若指定了 DROP_CASCADE，可能需要递归到依赖权限；
	 * 若指定了 DROP_RESTRICT 且存在依赖权限，则拒绝执行。
	 * plan_single_revoke() 会决定每个目录元组的处理方式。
	 */
	forboth(specitem, memberSpecs, iditem, memberIds)
	{
		RoleSpec   *memberRole = lfirst(specitem);
		Oid			memberid = lfirst_oid(iditem);

		if (!plan_single_revoke(memlist, actions, memberid, grantorId,
								popt, behavior))
		{
			ereport(WARNING,
					(errmsg("role \"%s\" has not been granted membership in role \"%s\" by role \"%s\"",
							get_rolespec_name(memberRole), rolename,
							GetUserNameFromId(grantorId, false))));
			continue;
		}
	}

	/*
	 * We now know what to do with each catalog tuple: it should either be
	 * left alone, deleted, or just have the admin_option flag cleared.
	 * Perform the appropriate action in each case.
	 *
	 * 现在已经知道每个目录元组该如何处理：保持不动、删除，或仅清除 admin_option 标志。
	 * 按情况执行相应动作。
	 */
	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	authmem_tuple;
		Form_pg_auth_members authmem_form;

		if (actions[i] == RRG_NOOP)
			continue;

		authmem_tuple = &memlist->members[i]->tuple;
		authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

		if (actions[i] == RRG_DELETE_GRANT)
		{
			/*
			 * Remove the entry altogether, after first removing its
			 * dependencies
			 *
			 * 先删除其依赖，再整个移除该条目
			 */
			deleteSharedDependencyRecordsFor(AuthMemRelationId,
											 authmem_form->oid, 0);
			CatalogTupleDelete(pg_authmem_rel, &authmem_tuple->t_self);
		}
		else
		{
			/* Just turn off the specified option */
			/*
			 *
			 * 只关闭指定的选项
			 */
			HeapTuple	tuple;
			Datum		new_record[Natts_pg_auth_members] = {0};
			bool		new_record_nulls[Natts_pg_auth_members] = {0};
			bool		new_record_repl[Natts_pg_auth_members] = {0};

			/* Build a tuple to update with */
			/*
			 *
			 * 构造用于更新的元组
			 */
			if (actions[i] == RRG_REMOVE_ADMIN_OPTION)
			{
				new_record[Anum_pg_auth_members_admin_option - 1] =
					BoolGetDatum(false);
				new_record_repl[Anum_pg_auth_members_admin_option - 1] =
					true;
			}
			else if (actions[i] == RRG_REMOVE_INHERIT_OPTION)
			{
				new_record[Anum_pg_auth_members_inherit_option - 1] =
					BoolGetDatum(false);
				new_record_repl[Anum_pg_auth_members_inherit_option - 1] =
					true;
			}
			else if (actions[i] == RRG_REMOVE_SET_OPTION)
			{
				new_record[Anum_pg_auth_members_set_option - 1] =
					BoolGetDatum(false);
				new_record_repl[Anum_pg_auth_members_set_option - 1] =
					true;
			}
			else
				elog(ERROR, "unknown role revoke action");

			tuple = heap_modify_tuple(authmem_tuple, pg_authmem_dsc,
									  new_record,
									  new_record_nulls, new_record_repl);
			CatalogTupleUpdate(pg_authmem_rel, &tuple->t_self, tuple);
		}
	}

	ReleaseSysCacheList(memlist);

	/*
	 * Close pg_authmem, but keep lock till commit.
	 *
	 * 关闭 pg_authmem，但持锁直到提交。
	 */
	table_close(pg_authmem_rel, NoLock);
}

/*
 * Check that currentUserId has permission to modify the membership list for
 * roleid. Throw an error if not.
 *
 * 检查 currentUserId 是否有权修改 roleid 的成员列表。无权则报错。
 */
static void
check_role_membership_authorization(Oid currentUserId, Oid roleid,
									bool is_grant)
{
	/*
	 * The charter of pg_database_owner is to have exactly one, implicit,
	 * situation-dependent member.  There's no technical need for this
	 * restriction.  (One could lift it and take the further step of making
	 * object_ownercheck(DatabaseRelationId, ...) equivalent to
	 * has_privs_of_role(roleid, ROLE_PG_DATABASE_OWNER), in which case
	 * explicit, situation-independent members could act as the owner of any
	 * database.)
	 *
	 * pg_database_owner 的设定是恰好有一个隐式的、随情境变化的成员。这一限制并无技术必要。
	 * 可以取消它，并进一步让 object_ownercheck(DatabaseRelationId, ...) 等价于
	 * has_privs_of_role(roleid, ROLE_PG_DATABASE_OWNER)，这样显式的、与情境无关的成员就能充当任意数据库的属主。
	 */
	if (is_grant && roleid == ROLE_PG_DATABASE_OWNER)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("role \"%s\" cannot have explicit members",
					   GetUserNameFromId(roleid, false)));

	/* To mess with a superuser role, you gotta be superuser. */
	/*
	 *
	 * 要改动 superuser 角色，自身必须是 superuser。
	 */
	if (superuser_arg(roleid))
	{
		if (!superuser_arg(currentUserId))
		{
			if (is_grant)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to grant role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s attribute may grant roles with the %s attribute.",
								   "SUPERUSER", "SUPERUSER")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to revoke role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s attribute may revoke roles with the %s attribute.",
								   "SUPERUSER", "SUPERUSER")));
		}
	}
	else
	{
		/*
		 * Otherwise, must have admin option on the role to be changed.
		 *
		 * 否则，必须对被修改的角色拥有 admin option。
		 */
		if (!is_admin_of_role(currentUserId, roleid))
		{
			if (is_grant)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to grant role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s option on role \"%s\" may grant this role.",
								   "ADMIN", GetUserNameFromId(roleid, false))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to revoke role \"%s\"",
								GetUserNameFromId(roleid, false)),
						 errdetail("Only roles with the %s option on role \"%s\" may revoke this role.",
								   "ADMIN", GetUserNameFromId(roleid, false))));
		}
	}
}

/*
 * Sanity-check, or infer, the grantor for a GRANT or REVOKE statement
 * targeting a role.
 *
 * 对针对角色的 GRANT 或 REVOKE，检查或推断 grantor。
 *
 * The grantor must always be either a role with ADMIN OPTION on the role in
 * which membership is being granted, or the bootstrap superuser. This is
 * similar to the restriction enforced by select_best_grantor, except that
 * roles don't have owners, so we regard the bootstrap superuser as the
 * implicit owner.
 *
 * grantor 必须是对被授予成员资格的角色拥有 ADMIN OPTION 的角色，或者是 bootstrap superuser。
 * 这与 select_best_grantor 的限制类似，但角色没有属主，因此把 bootstrap superuser 视为隐式属主。
 *
 * If the grantor was not explicitly specified by the user, grantorId should
 * be passed as InvalidOid, and this function will infer the user to be
 * recorded as the grantor. In many cases, this will be the current user, but
 * things get more complicated when the current user doesn't possess ADMIN
 * OPTION on the role but rather relies on having SUPERUSER privileges, or
 * on inheriting the privileges of a role which does have ADMIN OPTION. See
 * below for details.
 *
 * 若用户未显式指定 grantor，grantorId 应传入 InvalidOid，由本函数推断应记录的 grantor。
 * 多数情况下是当前用户；若当前用户自己没有该角色的 ADMIN OPTION，
 * 而是依赖 SUPERUSER，或继承了拥有 ADMIN OPTION 的角色的权限，情况会更复杂。详见下文。
 *
 * If the grantor was specified by the user, then it must be a user that
 * can legally be recorded as the grantor, as per the rule stated above.
 * This is an integrity constraint, not a permissions check, and thus even
 * superusers are subject to this restriction. However, there is also a
 * permissions check: to specify a role as the grantor, the current user
 * must possess the privileges of that role. Superusers will always pass
 * this check, but for non-superusers it may lead to an error.
 *
 * 若用户指定了 grantor，则它必须是按上述规则可以合法记为 grantor 的用户。
 * 这是完整性约束，不是权限检查，因此 superuser 也受此限制。
 * 另外还有权限检查：要把某角色指定为 grantor，当前用户必须拥有该角色的权限。
 * superuser 总能通过；非 superuser 则可能报错。
 *
 * The return value is the OID to be regarded as the grantor when executing
 * the operation.
 *
 * 返回值是执行该操作时应视为 grantor 的 OID。
 */
static Oid
check_role_grantor(Oid currentUserId, Oid roleid, Oid grantorId, bool is_grant)
{
	/* If the grantor ID was not specified, pick one to use. */
	/*
	 *
	 * 若未指定 grantor ID，则选取一个使用。
	 */
	if (!OidIsValid(grantorId))
	{
		/*
		 * Grants where the grantor is recorded as the bootstrap superuser do
		 * not depend on any other existing grants, so always default to this
		 * interpretation when possible.
		 *
		 * 把 grantor 记为 bootstrap superuser 的授权不依赖任何其他现存授权，
		 * 因此只要可能就默认采用这种解释。
		 */
		if (superuser_arg(currentUserId))
			return BOOTSTRAP_SUPERUSERID;

		/*
		 * Otherwise, the grantor must either have ADMIN OPTION on the role or
		 * inherit the privileges of a role which does. In the former case,
		 * record the grantor as the current user; in the latter, pick one of
		 * the roles that is "most directly" inherited by the current role
		 * (i.e. fewest "hops").
		 *
		 * 否则，grantor 必须自身拥有该角色的 ADMIN OPTION，或继承了拥有该选项的角色的权限。
		 * 前者把 grantor 记为当前用户；后者选取当前角色最直接继承到的角色（即跳数最少）。
		 *
		 * (We shouldn't fail to find a best grantor, because we've already
		 * established that the current user has permission to perform the
		 * operation.)
		 *
		 * 不应找不到最佳 grantor，因为已经确认当前用户有权执行该操作。
		 */
		grantorId = select_best_admin(currentUserId, roleid);
		if (!OidIsValid(grantorId))
			elog(ERROR, "no possible grantors");
		return grantorId;
	}

	/*
	 * If an explicit grantor is specified, it must be a role whose privileges
	 * the current user possesses.
	 *
	 * 若显式指定了 grantor，它必须是当前用户拥有其权限的角色。
	 *
	 * It should also be a role that has ADMIN OPTION on the target role, but
	 * we check this condition only in case of GRANT. For REVOKE, no matching
	 * grant should exist anyway, but if it somehow does, let the user get rid
	 * of it.
	 *
	 * 它还应是对目标角色拥有 ADMIN OPTION 的角色，但该条件只在 GRANT 时检查。
	 * 对 REVOKE，本来就不该存在匹配授权；若意外存在，则允许用户把它清掉。
	 */
	if (is_grant)
	{
		if (!has_privs_of_role(currentUserId, grantorId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to grant privileges as role \"%s\"",
							GetUserNameFromId(grantorId, false)),
					 errdetail("Only roles with privileges of role \"%s\" may grant privileges as this role.",
							   GetUserNameFromId(grantorId, false))));

		if (grantorId != BOOTSTRAP_SUPERUSERID &&
			select_best_admin(grantorId, roleid) != grantorId)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to grant privileges as role \"%s\"",
							GetUserNameFromId(grantorId, false)),
					 errdetail("The grantor must have the %s option on role \"%s\".",
							   "ADMIN", GetUserNameFromId(roleid, false))));
	}
	else
	{
		if (!has_privs_of_role(currentUserId, grantorId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to revoke privileges granted by role \"%s\"",
							GetUserNameFromId(grantorId, false)),
					 errdetail("Only roles with privileges of role \"%s\" may revoke privileges granted by this role.",
							   GetUserNameFromId(grantorId, false))));
	}

	/*
	 * If a grantor was specified explicitly, always attribute the grant to
	 * that role (unless we error out above).
	 *
	 * 若显式指定了 grantor，则始终把该授权记到该角色名下（除非上面已经报错）。
	 */
	return grantorId;
}

/*
 * Initialize an array of RevokeRoleGrantAction objects.
 *
 * 初始化 RevokeRoleGrantAction 对象数组。
 *
 * 'memlist' should be a list of all grants for the target role.
 *
 * memlist 应为目标角色的全部授权列表。
 *
 * This constructs an array indicating that no actions are to be performed;
 * that is, every element is initially RRG_NOOP.
 *
 * 构造一个表示不执行任何动作的数组，即每个元素初始为 RRG_NOOP。
 */
static RevokeRoleGrantAction *
initialize_revoke_actions(CatCList *memlist)
{
	RevokeRoleGrantAction *result;
	int			i;

	if (memlist->n_members == 0)
		return NULL;

	result = palloc(sizeof(RevokeRoleGrantAction) * memlist->n_members);
	for (i = 0; i < memlist->n_members; i++)
		result[i] = RRG_NOOP;
	return result;
}

/*
 * Figure out what we would need to do in order to revoke a grant, or just the
 * admin option on a grant, given that there might be dependent privileges.
 *
 * 在可能存在依赖权限时，计算撤销一条授权、或仅撤销其 admin option 需要做什么。
 *
 * 'memlist' should be a list of all grants for the target role.
 *
 * memlist 应为目标角色的全部授权列表。
 *
 * Whatever actions prove to be necessary will be signalled by updating
 * 'actions'.
 *
 * 必要的动作通过更新 actions 标出。
 *
 * If behavior is DROP_RESTRICT, an error will occur if there are dependent
 * role membership grants; if DROP_CASCADE, those grants will be scheduled
 * for deletion.
 *
 * 若 behavior 为 DROP_RESTRICT，存在依赖的角色成员授权时报错；
 * 若为 DROP_CASCADE，则把这些授权排入删除计划。
 *
 * The return value is true if the matching grant was found in the list,
 * and false if not.
 *
 * 若在列表中找到匹配授权则返回 true，否则返回 false。
 */
static bool
plan_single_revoke(CatCList *memlist, RevokeRoleGrantAction *actions,
				   Oid member, Oid grantor, GrantRoleOptions *popt,
				   DropBehavior behavior)
{
	int			i;

	/*
	 * If popt.specified == 0, we're revoking the grant entirely; otherwise,
	 * we expect just one bit to be set, and we're revoking the corresponding
	 * option. As of this writing, there's no syntax that would allow for an
	 * attempt to revoke multiple options at once, and the logic below
	 * wouldn't work properly if such syntax were added, so assert that our
	 * caller isn't trying to do that.
	 *
	 * 若 popt.specified == 0，表示完全撤销该授权；否则应只有一个标志位被置位，
	 * 表示撤销对应选项。目前没有一次撤销多个选项的语法，下面的逻辑也无法正确处理，
	 * 因此断言调用方不会这样做。
	 */
	Assert(pg_popcount32(popt->specified) <= 1);

	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	authmem_tuple;
		Form_pg_auth_members authmem_form;

		authmem_tuple = &memlist->members[i]->tuple;
		authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

		if (authmem_form->member == member &&
			authmem_form->grantor == grantor)
		{
			if ((popt->specified & GRANT_ROLE_SPECIFIED_INHERIT) != 0)
			{
				/*
				 * Revoking the INHERIT option doesn't change anything for
				 * dependent privileges, so we don't need to recurse.
				 *
				 * 撤销 INHERIT 选项不会改变依赖权限，因此不必递归。
				 */
				actions[i] = RRG_REMOVE_INHERIT_OPTION;
			}
			else if ((popt->specified & GRANT_ROLE_SPECIFIED_SET) != 0)
			{
				/* Here too, no need to recurse. */
				/*
				 *
				 * 这里同样不必递归。
				 */
				actions[i] = RRG_REMOVE_SET_OPTION;
			}
			else
			{
				bool		revoke_admin_option_only;

				/*
				 * Revoking the grant entirely, or ADMIN option on a grant,
				 * implicates dependent privileges, so we may need to recurse.
				 *
				 * 完全撤销授权，或撤销授权上的 ADMIN OPTION，会牵涉依赖权限，因此可能需要递归。
				 */
				revoke_admin_option_only =
					(popt->specified & GRANT_ROLE_SPECIFIED_ADMIN) != 0;
				plan_recursive_revoke(memlist, actions, i,
									  revoke_admin_option_only, behavior);
			}
			return true;
		}
	}

	return false;
}

/*
 * Figure out what we would need to do in order to revoke all grants to
 * a given member, given that there might be dependent privileges.
 *
 * 在可能存在依赖权限时，计算撤销授予某成员的全部授权需要做什么。
 *
 * 'memlist' should be a list of all grants for the target role.
 *
 * memlist 应为目标角色的全部授权列表。
 *
 * Whatever actions prove to be necessary will be signalled by updating
 * 'actions'.
 *
 * 必要的动作通过更新 actions 标出。
 */
static void
plan_member_revoke(CatCList *memlist, RevokeRoleGrantAction *actions,
				   Oid member)
{
	int			i;

	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	authmem_tuple;
		Form_pg_auth_members authmem_form;

		authmem_tuple = &memlist->members[i]->tuple;
		authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

		if (authmem_form->member == member)
			plan_recursive_revoke(memlist, actions, i, false, DROP_CASCADE);
	}
}

/*
 * Workhorse for figuring out recursive revocation of role grants.
 *
 * 计算角色授权递归撤销的主要逻辑。
 *
 * This is similar to what recursive_revoke() does for ACLs.
 *
 * 这与 ACL 上 recursive_revoke() 的做法类似。
 */
static void
plan_recursive_revoke(CatCList *memlist, RevokeRoleGrantAction *actions,
					  int index,
					  bool revoke_admin_option_only, DropBehavior behavior)
{
	bool		would_still_have_admin_option = false;
	HeapTuple	authmem_tuple;
	Form_pg_auth_members authmem_form;
	int			i;

	/* If it's already been done, we can just return. */
	/*
	 *
	 * 若已经处理过，直接返回。
	 */
	if (actions[index] == RRG_DELETE_GRANT)
		return;
	if (actions[index] == RRG_REMOVE_ADMIN_OPTION &&
		revoke_admin_option_only)
		return;

	/* Locate tuple data. */
	/*
	 *
	 * 定位元组数据。
	 */
	authmem_tuple = &memlist->members[index]->tuple;
	authmem_form = (Form_pg_auth_members) GETSTRUCT(authmem_tuple);

	/*
	 * If the existing tuple does not have admin_option set, then we do not
	 * need to recurse. If we're just supposed to clear that bit we don't need
	 * to do anything at all; if we're supposed to remove the grant, we need
	 * to do something, but only to the tuple, and not any others.
	 *
	 * 若现有元组未设置 admin_option，则不必递归。
	 * 若只是要清除该标志，则完全不必做事；
	 * 若要删除授权，只需处理该元组，不必动其他元组。
	 */
	if (!revoke_admin_option_only)
	{
		actions[index] = RRG_DELETE_GRANT;
		if (!authmem_form->admin_option)
			return;
	}
	else
	{
		if (!authmem_form->admin_option)
			return;
		actions[index] = RRG_REMOVE_ADMIN_OPTION;
	}

	/* Determine whether the member would still have ADMIN OPTION. */
	/*
	 *
	 * 判断该成员是否仍会保有 ADMIN OPTION。
	 */
	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	am_cascade_tuple;
		Form_pg_auth_members am_cascade_form;

		am_cascade_tuple = &memlist->members[i]->tuple;
		am_cascade_form = (Form_pg_auth_members) GETSTRUCT(am_cascade_tuple);

		if (am_cascade_form->member == authmem_form->member &&
			am_cascade_form->admin_option && actions[i] == RRG_NOOP)
		{
			would_still_have_admin_option = true;
			break;
		}
	}

	/* If the member would still have ADMIN OPTION, we need not recurse. */
	/*
	 *
	 * 若该成员仍保有 ADMIN OPTION，则不必递归。
	 */
	if (would_still_have_admin_option)
		return;

	/*
	 * Recurse to grants that are not yet slated for deletion which have this
	 * member as the grantor.
	 *
	 * 递归处理尚未计划删除、且以该成员为 grantor 的授权。
	 */
	for (i = 0; i < memlist->n_members; ++i)
	{
		HeapTuple	am_cascade_tuple;
		Form_pg_auth_members am_cascade_form;

		am_cascade_tuple = &memlist->members[i]->tuple;
		am_cascade_form = (Form_pg_auth_members) GETSTRUCT(am_cascade_tuple);

		if (am_cascade_form->grantor == authmem_form->member &&
			actions[i] != RRG_DELETE_GRANT)
		{
			if (behavior == DROP_RESTRICT)
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("dependent privileges exist"),
						 errhint("Use CASCADE to revoke them too.")));

			plan_recursive_revoke(memlist, actions, i, false, behavior);
		}
	}
}

/*
 * Initialize a GrantRoleOptions object with default values.
 *
 * 用默认值初始化 GrantRoleOptions 对象。
 */
static void
InitGrantRoleOptions(GrantRoleOptions *popt)
{
	popt->specified = 0;
	popt->admin = false;
	popt->inherit = false;
	popt->set = true;
}

/*
 * GUC check_hook for createrole_self_grant
 *
 * createrole_self_grant 的 GUC check_hook
 */
bool
check_createrole_self_grant(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	unsigned	options = 0;
	unsigned   *result;

	/* Need a modifiable copy of string */
	/*
	 *
	 * 需要一份可修改的字符串副本
	 */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		/*
		 *
		 * 列表中存在语法错误
		 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "SET") == 0)
			options |= GRANT_ROLE_SPECIFIED_SET;
		else if (pg_strcasecmp(tok, "INHERIT") == 0)
			options |= GRANT_ROLE_SPECIFIED_INHERIT;
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	result = (unsigned *) guc_malloc(LOG, sizeof(unsigned));
	if (!result)
		return false;
	*result = options;
	*extra = result;

	return true;
}

/*
 * GUC assign_hook for createrole_self_grant
 *
 * createrole_self_grant 的 GUC assign_hook
 */
void
assign_createrole_self_grant(const char *newval, void *extra)
{
	unsigned	options = *(unsigned *) extra;

	createrole_self_grant_enabled = (options != 0);
	createrole_self_grant_options.specified = GRANT_ROLE_SPECIFIED_ADMIN
		| GRANT_ROLE_SPECIFIED_INHERIT
		| GRANT_ROLE_SPECIFIED_SET;
	createrole_self_grant_options.admin = false;
	createrole_self_grant_options.inherit =
		(options & GRANT_ROLE_SPECIFIED_INHERIT) != 0;
	createrole_self_grant_options.set =
		(options & GRANT_ROLE_SPECIFIED_SET) != 0;
}
