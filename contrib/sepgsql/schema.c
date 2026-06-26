/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/schema.c
 *
 * Routines corresponding to schema objects
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/seclabel.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "sepgsql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

/*
 * sepgsql_schema_post_create
 *
 * This routine assigns a default security label on a newly defined
 * schema.
 *
 * 此例程在新定义的模式上分配默认安全标签。
 */
void
sepgsql_schema_post_create(Oid namespaceId)
{
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *tcontext;
	char	   *ncontext;
	const char *nsp_name;
	ObjectAddress object;
	Form_pg_namespace nspForm;
	StringInfoData audit_name;

	/*
	 * Compute a default security label when we create a new schema object
	 * under the working database.
	 *
	 * 当我们在工作数据库下创建新的模式对象时，计算默认的安全标签。
	 *
	 * XXX - upcoming version of libselinux supports to take object name to
	 * handle special treatment on default security label; such as special
	 * label on "pg_temp" schema.
	 *
	 * XXX - 即将推出的 libselinux 版本支持使用对象名称来处理默认安全标签的特殊处理；例如“pg_temp”模式上的特殊标签。
	 */
	rel = table_open(NamespaceRelationId, AccessShareLock);

	ScanKeyInit(&skey,
				Anum_pg_namespace_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(namespaceId));

	sscan = systable_beginscan(rel, NamespaceOidIndexId, true,
							   SnapshotSelf, 1, &skey);
	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for namespace %u", namespaceId);

	nspForm = (Form_pg_namespace) GETSTRUCT(tuple);
	nsp_name = NameStr(nspForm->nspname);
	if (strncmp(nsp_name, "pg_temp_", 8) == 0)
		nsp_name = "pg_temp";
	else if (strncmp(nsp_name, "pg_toast_temp_", 14) == 0)
		nsp_name = "pg_toast_temp";

	tcontext = sepgsql_get_label(DatabaseRelationId, MyDatabaseId, 0);
	ncontext = sepgsql_compute_create(sepgsql_get_client_label(),
									  tcontext,
									  SEPG_CLASS_DB_SCHEMA,
									  nsp_name);

	/*
	 * check db_schema:{create}
	 *
	 * 检查 db_schema:{创建}
	 */
	initStringInfo(&audit_name);
	appendStringInfoString(&audit_name, quote_identifier(nsp_name));
	sepgsql_avc_check_perms_label(ncontext,
								  SEPG_CLASS_DB_SCHEMA,
								  SEPG_DB_SCHEMA__CREATE,
								  audit_name.data,
								  true);
	systable_endscan(sscan);
	table_close(rel, AccessShareLock);

	/*
	 * Assign the default security label on a new procedure
	 *
	 * 为新过程分配默认安全标签
	 */
	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, ncontext);

	pfree(ncontext);
	pfree(tcontext);
}

/*
 * sepgsql_schema_drop
 *
 * It checks privileges to drop the supplied schema object.
 *
 * 它检查删除提供的模式对象的权限。
 */
void
sepgsql_schema_drop(Oid namespaceId)
{
	ObjectAddress object;
	char	   *audit_name;

	/*
	 * check db_schema:{drop} permission
	 *
	 * 检查 db_schema:{drop} 权限
	 */
	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__DROP,
							audit_name,
							true);
	pfree(audit_name);
}

/*
 * sepgsql_schema_relabel
 *
 * It checks privileges to relabel the supplied schema
 * by the `seclabel'.
 *
 * 它检查通过“seclabel”重新标记所提供模式的权限。
 */
void
sepgsql_schema_relabel(Oid namespaceId, const char *seclabel)
{
	ObjectAddress object;
	char	   *audit_name;

	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	/*
	 * check db_schema:{setattr relabelfrom} permission
	 *
	 * 检查 db_schema:{setattr relabelfrom} 权限
	 */
	sepgsql_avc_check_perms(&object,
							SEPG_CLASS_DB_SCHEMA,
							SEPG_DB_SCHEMA__SETATTR |
							SEPG_DB_SCHEMA__RELABELFROM,
							audit_name,
							true);

	/*
	 * check db_schema:{relabelto} permission
	 *
	 * 检查 db_schema:{relabelto} 权限
	 */
	sepgsql_avc_check_perms_label(seclabel,
								  SEPG_CLASS_DB_SCHEMA,
								  SEPG_DB_SCHEMA__RELABELTO,
								  audit_name,
								  true);
	pfree(audit_name);
}

/*
 * sepgsql_schema_check_perms
 *
 * utility routine to check db_schema:{xxx} permissions
 *
 * 检查 db_schema:{xxx} 权限的实用程序例程
 */
static bool
check_schema_perms(Oid namespaceId, uint32 required, bool abort_on_violation)
{
	ObjectAddress object;
	char	   *audit_name;
	bool		result;

	object.classId = NamespaceRelationId;
	object.objectId = namespaceId;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);

	result = sepgsql_avc_check_perms(&object,
									 SEPG_CLASS_DB_SCHEMA,
									 required,
									 audit_name,
									 abort_on_violation);
	pfree(audit_name);

	return result;
}

/* db_schema:{setattr} permission
 *
 * db_schema:{setattr} 权限
 */
void
sepgsql_schema_setattr(Oid namespaceId)
{
	check_schema_perms(namespaceId, SEPG_DB_SCHEMA__SETATTR, true);
}

/* db_schema:{search} permission
 *
 * db_schema:{搜索}权限
 */
bool
sepgsql_schema_search(Oid namespaceId, bool abort_on_violation)
{
	return check_schema_perms(namespaceId,
							  SEPG_DB_SCHEMA__SEARCH,
							  abort_on_violation);
}

void
sepgsql_schema_add_name(Oid namespaceId)
{
	check_schema_perms(namespaceId, SEPG_DB_SCHEMA__ADD_NAME, true);
}

void
sepgsql_schema_remove_name(Oid namespaceId)
{
	check_schema_perms(namespaceId, SEPG_DB_SCHEMA__REMOVE_NAME, true);
}

void
sepgsql_schema_rename(Oid namespaceId)
{
	check_schema_perms(namespaceId,
					   SEPG_DB_SCHEMA__ADD_NAME |
					   SEPG_DB_SCHEMA__REMOVE_NAME,
					   true);
}
