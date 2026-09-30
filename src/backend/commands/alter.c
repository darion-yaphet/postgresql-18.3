/*-------------------------------------------------------------------------
 *
 * alter.c
 *	  Drivers for generic alter commands
 *
 * 通用 ALTER 命令的分发例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/alter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/policy.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/subscriptioncmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "miscadmin.h"
#include "replication/logicalworker.h"
#include "rewrite/rewriteDefine.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * ExecRenameStmt、ExecAlterObjectDependsStmt、ExecAlterOwnerStmt 和
 * SET SCHEMA 路径按对象类型分发。简单对象走 AlterObjectRename_internal、
 * AlterObjectNamespace_internal、AlterObjectOwner_internal：检查属主与
 * CREATE 权限、避免重名，更新目录元组、依赖，并调用对象访问钩子。
 */
static Oid	AlterObjectNamespace_internal(Relation rel, Oid objid, Oid nspOid);

/*
 * Raise an error to the effect that an object of the given name is already
 * present in the given namespace.
 *
 * 报错：给定命名空间中已经有同名对象。
 */
static void
report_name_conflict(Oid classId, const char *name)
{
	char	   *msgfmt;

	switch (classId)
	{
		case EventTriggerRelationId:
			msgfmt = gettext_noop("event trigger \"%s\" already exists");
			break;
		case ForeignDataWrapperRelationId:
			msgfmt = gettext_noop("foreign-data wrapper \"%s\" already exists");
			break;
		case ForeignServerRelationId:
			msgfmt = gettext_noop("server \"%s\" already exists");
			break;
		case LanguageRelationId:
			msgfmt = gettext_noop("language \"%s\" already exists");
			break;
		case PublicationRelationId:
			msgfmt = gettext_noop("publication \"%s\" already exists");
			break;
		case SubscriptionRelationId:
			msgfmt = gettext_noop("subscription \"%s\" already exists");
			break;
		default:
			elog(ERROR, "unsupported object class: %u", classId);
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DUPLICATE_OBJECT),
			 errmsg(msgfmt, name)));
}

/*
 * 报告命名空间中已存在同名对象。
 */
static void
report_namespace_conflict(Oid classId, const char *name, Oid nspOid)
{
	char	   *msgfmt;

	Assert(OidIsValid(nspOid));

	switch (classId)
	{
		case ConversionRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("conversion \"%s\" already exists in schema \"%s\"");
			break;
		case StatisticExtRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("statistics object \"%s\" already exists in schema \"%s\"");
			break;
		case TSParserRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search parser \"%s\" already exists in schema \"%s\"");
			break;
		case TSDictionaryRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search dictionary \"%s\" already exists in schema \"%s\"");
			break;
		case TSTemplateRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search template \"%s\" already exists in schema \"%s\"");
			break;
		case TSConfigRelationId:
			Assert(OidIsValid(nspOid));
			msgfmt = gettext_noop("text search configuration \"%s\" already exists in schema \"%s\"");
			break;
		default:
			elog(ERROR, "unsupported object class: %u", classId);
			break;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DUPLICATE_OBJECT),
			 errmsg(msgfmt, name, get_namespace_name(nspOid))));
}

/*
 * AlterObjectRename_internal
 *
 * AlterObjectRename_internal：
 *
 * Generic function to rename the given object, for simple cases (won't
 * work for tables, nor other cases where we need to do more than change
 * the name column of a single catalog entry).
 *
 * 简单情况下重命名对象的通用函数。不适用于表，也不适用于除了改一条目录记录的名字列之外还要做更多事的情况。
 *
 * rel: catalog relation containing object (RowExclusiveLock'd by caller)
 * objectId: OID of object to be renamed
 * new_name: CString representation of new name
 *
 * rel：存放该对象的目录关系（调用者已加 RowExclusiveLock）。
 * objectId：待重命名对象的 OID。
 * new_name：新名字的 C 字符串。
 */
static void
AlterObjectRename_internal(Relation rel, Oid objectId, const char *new_name)
{
	Oid			classId = RelationGetRelid(rel);
	int			oidCacheId = get_object_catcache_oid(classId);
	int			nameCacheId = get_object_catcache_name(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Datum		datum;
	bool		isnull;
	Oid			namespaceId;
	Oid			ownerId;
	char	   *old_name;
	AclResult	aclresult;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;
	NameData	nameattrdata;

	oldtup = SearchSysCache1(oidCacheId, ObjectIdGetDatum(objectId));
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(rel));

	datum = heap_getattr(oldtup, Anum_name,
						 RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	old_name = NameStr(*(DatumGetName(datum)));

	/* Get OID of namespace */
	/*
	 *
	 * 取得命名空间 OID。
	 */
	if (Anum_namespace > 0)
	{
		datum = heap_getattr(oldtup, Anum_namespace,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		namespaceId = DatumGetObjectId(datum);
	}
	else
		namespaceId = InvalidOid;

	/* Permission checks ... superusers can always do it */
	/*
	 *
	 * 权限检查。超级用户始终可以执行。
	 */
	if (!superuser())
	{
		/* Fail if object does not have an explicit owner */
		/*
		 *
		 * 对象没有显式属主则失败。
		 */
		if (Anum_owner <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to rename %s",
							getObjectDescriptionOids(classId, objectId))));

		/* Otherwise, must be owner of the existing object */
		/*
		 *
		 * 否则必须是现有对象的属主。
		 */
		datum = heap_getattr(oldtup, Anum_owner,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		ownerId = DatumGetObjectId(datum);

		if (!has_privs_of_role(GetUserId(), DatumGetObjectId(ownerId)))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_object_type(classId, objectId),
						   old_name);

		/* User must have CREATE privilege on the namespace */
		/*
		 *
		 * 用户必须对命名空间有 CREATE 权限。
		 */
		if (OidIsValid(namespaceId))
		{
			aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(),
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_SCHEMA,
							   get_namespace_name(namespaceId));
		}

		if (classId == SubscriptionRelationId)
		{
			Form_pg_subscription form;

			/* must have CREATE privilege on database */
			/*
			 *
			 * 必须对数据库有 CREATE 权限。
			 */
			aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId,
										GetUserId(), ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_DATABASE,
							   get_database_name(MyDatabaseId));

			/*
			 * Don't allow non-superuser modification of a subscription with
			 * password_required=false.
			 *
			 * 不允许非超级用户修改 password_required=false 的订阅。
			 */
			form = (Form_pg_subscription) GETSTRUCT(oldtup);
			if (!form->subpasswordrequired && !superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("password_required=false is superuser-only"),
						 errhint("Subscriptions with the password_required option set to false may only be created or modified by the superuser.")));
		}
	}

	/*
	 * Check for duplicate name (more friendly than unique-index failure).
	 * Since this is just a friendliness check, we can just skip it in cases
	 * where there isn't suitable support.
	 *
	 * 检查重名（比唯一索引失败更友好）。这只是友好检查，没有合适支持时可以跳过。
	 */
	if (classId == ProcedureRelationId)
	{
		Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(oldtup);

		IsThereFunctionInNamespace(new_name, proc->pronargs,
								   &proc->proargtypes, proc->pronamespace);
	}
	else if (classId == CollationRelationId)
	{
		Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(oldtup);

		IsThereCollationInNamespace(new_name, coll->collnamespace);
	}
	else if (classId == OperatorClassRelationId)
	{
		Form_pg_opclass opc = (Form_pg_opclass) GETSTRUCT(oldtup);

		IsThereOpClassInNamespace(new_name, opc->opcmethod,
								  opc->opcnamespace);
	}
	else if (classId == OperatorFamilyRelationId)
	{
		Form_pg_opfamily opf = (Form_pg_opfamily) GETSTRUCT(oldtup);

		IsThereOpFamilyInNamespace(new_name, opf->opfmethod,
								   opf->opfnamespace);
	}
	else if (classId == SubscriptionRelationId)
	{
		if (SearchSysCacheExists2(SUBSCRIPTIONNAME,
								  ObjectIdGetDatum(MyDatabaseId),
								  CStringGetDatum(new_name)))
			report_name_conflict(classId, new_name);

		/* Also enforce regression testing naming rules, if enabled */
		/*
		 *
		 * 若已启用，同时执行回归测试的命名规则。
		 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
		if (strncmp(new_name, "regress_", 8) != 0)
			elog(WARNING, "subscriptions created by regression test cases should have names starting with \"regress_\"");
#endif

		/* Wake up related replication workers to handle this change quickly */
		/*
		 *
		 * 唤醒相关复制进程，以便尽快处理这次改动。
		 */
		LogicalRepWorkersWakeupAtCommit(objectId);
	}
	else if (nameCacheId >= 0)
	{
		if (OidIsValid(namespaceId))
		{
			if (SearchSysCacheExists2(nameCacheId,
									  CStringGetDatum(new_name),
									  ObjectIdGetDatum(namespaceId)))
				report_namespace_conflict(classId, new_name, namespaceId);
		}
		else
		{
			if (SearchSysCacheExists1(nameCacheId,
									  CStringGetDatum(new_name)))
				report_name_conflict(classId, new_name);
		}
	}

	/* Build modified tuple */
	/*
	 *
	 * 构造修改后的元组。
	 */
	values = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(Datum));
	nulls = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	replaces = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	namestrcpy(&nameattrdata, new_name);
	values[Anum_name - 1] = NameGetDatum(&nameattrdata);
	replaces[Anum_name - 1] = true;
	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   values, nulls, replaces);

	/* Perform actual update */
	/*
	 *
	 * 执行实际更新。
	 */
	CatalogTupleUpdate(rel, &oldtup->t_self, newtup);

	InvokeObjectPostAlterHook(classId, objectId, 0);

	/* Do post catalog-update tasks */
	/*
	 *
	 * 做目录更新之后的收尾。
	 */
	if (classId == PublicationRelationId)
	{
		Form_pg_publication pub = (Form_pg_publication) GETSTRUCT(oldtup);

		/*
		 * Invalidate relsynccache entries.
		 *
		 * 使 relsynccache 项失效。
		 *
		 * Unlike ALTER PUBLICATION ADD/SET/DROP commands, renaming a
		 * publication does not impact the publication status of tables. So,
		 * we don't need to invalidate relcache to rebuild the rd_pubdesc.
		 * Instead, we invalidate only the relsyncache.
		 *
		 * 与 ALTER PUBLICATION ADD/SET/DROP 不同，重命名发布不影响表的发布状态。
		 * 因此不必让 relcache 失效来重建 rd_pubdesc，只需使 relsynccache 失效。
		 */
		InvalidatePubRelSyncCache(pub->oid, pub->puballtables);
	}

	/* Release memory */
	/*
	 *
	 * 释放内存。
	 */
	pfree(values);
	pfree(nulls);
	pfree(replaces);
	heap_freetuple(newtup);

	ReleaseSysCache(oldtup);
}

/*
 * Executes an ALTER OBJECT / RENAME TO statement.  Based on the object
 * type, the function appropriate to that type is executed.
 *
 * 执行 ALTER OBJECT / RENAME TO。按对象类型调用相应函数。
 *
 * Return value is the address of the renamed object.
 *
 * 返回值是被重命名对象的地址。
 */
ObjectAddress
ExecRenameStmt(RenameStmt *stmt)
{
	switch (stmt->renameType)
	{
		case OBJECT_TABCONSTRAINT:
		case OBJECT_DOMCONSTRAINT:
			return RenameConstraint(stmt);

		case OBJECT_DATABASE:
			return RenameDatabase(stmt->subname, stmt->newname);

		case OBJECT_ROLE:
			return RenameRole(stmt->subname, stmt->newname);

		case OBJECT_SCHEMA:
			return RenameSchema(stmt->subname, stmt->newname);

		case OBJECT_TABLESPACE:
			return RenameTableSpace(stmt->subname, stmt->newname);

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_INDEX:
		case OBJECT_FOREIGN_TABLE:
			return RenameRelation(stmt);

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
			return renameatt(stmt);

		case OBJECT_RULE:
			return RenameRewriteRule(stmt->relation, stmt->subname,
									 stmt->newname);

		case OBJECT_TRIGGER:
			return renametrig(stmt);

		case OBJECT_POLICY:
			return rename_policy(stmt);

		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			return RenameType(stmt);

		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_FUNCTION:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_LANGUAGE:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_PUBLICATION:
		case OBJECT_SUBSCRIPTION:
			{
				ObjectAddress address;
				Relation	catalog;

				address = get_object_address(stmt->renameType,
											 stmt->object,
											 NULL,
											 AccessExclusiveLock, false);

				catalog = table_open(address.classId, RowExclusiveLock);
				AlterObjectRename_internal(catalog,
										   address.objectId,
										   stmt->newname);
				table_close(catalog, RowExclusiveLock);

				return address;
			}

		default:
			elog(ERROR, "unrecognized rename stmt type: %d",
				 (int) stmt->renameType);
			return InvalidObjectAddress;	/* keep compiler happy */
			/*
			 *
			 * 避免编译器告警。
			 */
	}
}

/*
 * Executes an ALTER OBJECT / [NO] DEPENDS ON EXTENSION statement.
 *
 * 执行 ALTER OBJECT / [NO] DEPENDS ON EXTENSION。
 *
 * Return value is the address of the altered object.  refAddress is an output
 * argument which, if not null, receives the address of the object that the
 * altered object now depends on.
 *
 * 返回值是被修改对象的地址。refAddress 若非空，则接收该对象现在所依赖对象的地址。
 */
ObjectAddress
ExecAlterObjectDependsStmt(AlterObjectDependsStmt *stmt, ObjectAddress *refAddress)
{
	ObjectAddress address;
	ObjectAddress refAddr;
	Relation	rel;

	address =
		get_object_address_rv(stmt->objectType, stmt->relation, (List *) stmt->object,
							  &rel, AccessExclusiveLock, false);

	/*
	 * Verify that the user is entitled to run the command.
	 *
	 * 确认用户有权执行该命令。
	 *
	 * We don't check any privileges on the extension, because that's not
	 * needed.  The object owner is stipulating, by running this command, that
	 * the extension owner can drop the object whenever they feel like it,
	 * which is not considered a problem.
	 *
	 * 不检查扩展上的权限，因为不需要。对象属主执行此命令，即表示允许扩展属主随时删除该对象，这不视为问题。
	 */
	check_object_ownership(GetUserId(),
						   stmt->objectType, address, stmt->object, rel);

	/*
	 * If a relation was involved, it would have been opened and locked. We
	 * don't need the relation here, but we'll retain the lock until commit.
	 *
	 * 若涉及关系，它已经被打开并加锁。这里不需要该关系，但锁保留到提交。
	 */
	if (rel)
		table_close(rel, NoLock);

	refAddr = get_object_address(OBJECT_EXTENSION, (Node *) stmt->extname,
								 NULL, AccessExclusiveLock, false);
	if (refAddress)
		*refAddress = refAddr;

	if (stmt->remove)
	{
		deleteDependencyRecordsForSpecific(address.classId, address.objectId,
										   DEPENDENCY_AUTO_EXTENSION,
										   refAddr.classId, refAddr.objectId);
	}
	else
	{
		List	   *currexts;

		/* Avoid duplicates */
		/*
		 *
		 * 避免重复。
		 */
		currexts = getAutoExtensionsOfObject(address.classId,
											 address.objectId);
		if (!list_member_oid(currexts, refAddr.objectId))
			recordDependencyOn(&address, &refAddr, DEPENDENCY_AUTO_EXTENSION);
	}

	return address;
}

/*
 * Executes an ALTER OBJECT / SET SCHEMA statement.  Based on the object
 * type, the function appropriate to that type is executed.
 *
 * 执行 ALTER OBJECT / SET SCHEMA。按对象类型调用相应函数。
 *
 * Return value is that of the altered object.
 *
 * 返回值是被修改对象的地址。
 *
 * oldSchemaAddr is an output argument which, if not NULL, is set to the object
 * address of the original schema.
 *
 * oldSchemaAddr 是输出参数；非 NULL 时被设为原模式的对象地址。
 */
ObjectAddress
ExecAlterObjectSchemaStmt(AlterObjectSchemaStmt *stmt,
						  ObjectAddress *oldSchemaAddr)
{
	ObjectAddress address;
	Oid			oldNspOid;

	switch (stmt->objectType)
	{
		case OBJECT_EXTENSION:
			address = AlterExtensionNamespace(strVal(stmt->object), stmt->newschema,
											  oldSchemaAddr ? &oldNspOid : NULL);
			break;

		case OBJECT_FOREIGN_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
			address = AlterTableNamespace(stmt,
										  oldSchemaAddr ? &oldNspOid : NULL);
			break;

		case OBJECT_DOMAIN:
		case OBJECT_TYPE:
			address = AlterTypeNamespace(castNode(List, stmt->object), stmt->newschema,
										 stmt->objectType,
										 oldSchemaAddr ? &oldNspOid : NULL);
			break;

			/* generic code path */
			/*
			 *
			 * 通用代码路径。
			 */
		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_FUNCTION:
		case OBJECT_OPERATOR:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
			{
				Relation	catalog;
				Oid			classId;
				Oid			nspOid;

				address = get_object_address(stmt->objectType,
											 stmt->object,
											 NULL,
											 AccessExclusiveLock,
											 false);
				classId = address.classId;
				catalog = table_open(classId, RowExclusiveLock);
				nspOid = LookupCreationNamespace(stmt->newschema);

				oldNspOid = AlterObjectNamespace_internal(catalog, address.objectId,
														  nspOid);
				table_close(catalog, RowExclusiveLock);
			}
			break;

		default:
			elog(ERROR, "unrecognized AlterObjectSchemaStmt type: %d",
				 (int) stmt->objectType);
			return InvalidObjectAddress;	/* keep compiler happy */
			/*
			 *
			 * 避免编译器告警。
			 */
	}

	if (oldSchemaAddr)
		ObjectAddressSet(*oldSchemaAddr, NamespaceRelationId, oldNspOid);

	return address;
}

/*
 * Change an object's namespace given its classOid and object Oid.
 *
 * 按 classOid 和对象 OID 更改对象的命名空间。
 *
 * Objects that don't have a namespace should be ignored, as should
 * dependent types such as array types.
 *
 * 没有命名空间的对象应忽略，数组类型这类依赖类型也应忽略。
 *
 * This function is currently used only by ALTER EXTENSION SET SCHEMA,
 * so it only needs to cover object kinds that can be members of an
 * extension, and it can silently ignore dependent types --- we assume
 * those will be moved when their parent object is moved.
 *
 * 本函数目前只给 ALTER EXTENSION SET SCHEMA 使用，因此只需覆盖能成为扩展成员的对象种类，
 * 并可以静默忽略依赖类型——假定它们会随父对象一起移动。
 *
 * Returns the OID of the object's previous namespace, or InvalidOid if
 * object doesn't have a schema or was ignored due to being a dependent type.
 *
 * 返回对象原来的命名空间 OID；若对象没有模式，或因是依赖类型而被忽略，则返回 InvalidOid。
 */
Oid
AlterObjectNamespace_oid(Oid classId, Oid objid, Oid nspOid,
						 ObjectAddresses *objsMoved)
{
	Oid			oldNspOid = InvalidOid;

	switch (classId)
	{
		case RelationRelationId:
			{
				Relation	rel;

				rel = relation_open(objid, AccessExclusiveLock);
				oldNspOid = RelationGetNamespace(rel);

				AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);

				relation_close(rel, NoLock);
				break;
			}

		case TypeRelationId:
			oldNspOid = AlterTypeNamespace_oid(objid, nspOid, true, objsMoved);
			break;

		case ProcedureRelationId:
		case CollationRelationId:
		case ConversionRelationId:
		case OperatorRelationId:
		case OperatorClassRelationId:
		case OperatorFamilyRelationId:
		case StatisticExtRelationId:
		case TSParserRelationId:
		case TSDictionaryRelationId:
		case TSTemplateRelationId:
		case TSConfigRelationId:
			{
				Relation	catalog;

				catalog = table_open(classId, RowExclusiveLock);

				oldNspOid = AlterObjectNamespace_internal(catalog, objid,
														  nspOid);

				table_close(catalog, RowExclusiveLock);
			}
			break;

		default:
			/* ignore object types that don't have schema-qualified names */
			/*
			 *
			 * 忽略没有模式限定名的对象类型。
			 */
			Assert(get_object_attnum_namespace(classId) == InvalidAttrNumber);
	}

	return oldNspOid;
}

/*
 * Generic function to change the namespace of a given object, for simple
 * cases (won't work for tables, nor other cases where we need to do more
 * than change the namespace column of a single catalog entry).
 *
 * 简单情况下更改对象命名空间的通用函数。不适用于表，也不适用于除了改一条目录记录的命名空间列之外还要做更多事的情况。
 *
 * rel: catalog relation containing object (RowExclusiveLock'd by caller)
 * objid: OID of object to change the namespace of
 * nspOid: OID of new namespace
 *
 * rel：存放该对象的目录关系（调用者已加 RowExclusiveLock）。
 * objid：要改命名空间的对象 OID。
 * nspOid：新命名空间的 OID。
 *
 * Returns the OID of the object's previous namespace.
 *
 * 返回对象原来的命名空间 OID。
 */
static Oid
AlterObjectNamespace_internal(Relation rel, Oid objid, Oid nspOid)
{
	Oid			classId = RelationGetRelid(rel);
	int			oidCacheId = get_object_catcache_oid(classId);
	int			nameCacheId = get_object_catcache_name(classId);
	AttrNumber	Anum_name = get_object_attnum_name(classId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(classId);
	AttrNumber	Anum_owner = get_object_attnum_owner(classId);
	Oid			oldNspOid;
	Datum		name,
				namespace;
	bool		isnull;
	HeapTuple	tup,
				newtup;
	Datum	   *values;
	bool	   *nulls;
	bool	   *replaces;

	tup = SearchSysCacheCopy1(oidCacheId, ObjectIdGetDatum(objid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
	/*
	 *
	 * 不应发生。
	 */
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objid, RelationGetRelationName(rel));

	name = heap_getattr(tup, Anum_name, RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	namespace = heap_getattr(tup, Anum_namespace, RelationGetDescr(rel),
							 &isnull);
	Assert(!isnull);
	oldNspOid = DatumGetObjectId(namespace);

	/*
	 * If the object is already in the correct namespace, we don't need to do
	 * anything except fire the object access hook.
	 *
	 * 对象已在正确的命名空间中时，除了触发对象访问钩子外不必做其它事。
	 */
	if (oldNspOid == nspOid)
	{
		InvokeObjectPostAlterHook(classId, objid, 0);
		return oldNspOid;
	}

	/* Check basic namespace related issues */
	/*
	 *
	 * 检查与命名空间相关的基本问题。
	 */
	CheckSetNamespace(oldNspOid, nspOid);

	/* Permission checks ... superusers can always do it */
	/*
	 *
	 * 权限检查。超级用户始终可以执行。
	 */
	if (!superuser())
	{
		Datum		owner;
		Oid			ownerId;
		AclResult	aclresult;

		/* Fail if object does not have an explicit owner */
		/*
		 *
		 * 对象没有显式属主则失败。
		 */
		if (Anum_owner <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to set schema of %s",
							getObjectDescriptionOids(classId, objid))));

		/* Otherwise, must be owner of the existing object */
		/*
		 *
		 * 否则必须是现有对象的属主。
		 */
		owner = heap_getattr(tup, Anum_owner, RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		ownerId = DatumGetObjectId(owner);

		if (!has_privs_of_role(GetUserId(), ownerId))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_object_type(classId, objid),
						   NameStr(*(DatumGetName(name))));

		/* User must have CREATE privilege on new namespace */
		/*
		 *
		 * 用户必须对新命名空间有 CREATE 权限。
		 */
		aclresult = object_aclcheck(NamespaceRelationId, nspOid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(nspOid));
	}

	/*
	 * Check for duplicate name (more friendly than unique-index failure).
	 * Since this is just a friendliness check, we can just skip it in cases
	 * where there isn't suitable support.
	 *
	 * 检查重名（比唯一索引失败更友好）。这只是友好检查，没有合适支持时可以跳过。
	 */
	if (classId == ProcedureRelationId)
	{
		Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(tup);

		IsThereFunctionInNamespace(NameStr(proc->proname), proc->pronargs,
								   &proc->proargtypes, nspOid);
	}
	else if (classId == CollationRelationId)
	{
		Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(tup);

		IsThereCollationInNamespace(NameStr(coll->collname), nspOid);
	}
	else if (classId == OperatorClassRelationId)
	{
		Form_pg_opclass opc = (Form_pg_opclass) GETSTRUCT(tup);

		IsThereOpClassInNamespace(NameStr(opc->opcname),
								  opc->opcmethod, nspOid);
	}
	else if (classId == OperatorFamilyRelationId)
	{
		Form_pg_opfamily opf = (Form_pg_opfamily) GETSTRUCT(tup);

		IsThereOpFamilyInNamespace(NameStr(opf->opfname),
								   opf->opfmethod, nspOid);
	}
	else if (nameCacheId >= 0 &&
			 SearchSysCacheExists2(nameCacheId, name,
								   ObjectIdGetDatum(nspOid)))
		report_namespace_conflict(classId,
								  NameStr(*(DatumGetName(name))),
								  nspOid);

	/* Build modified tuple */
	/*
	 *
	 * 构造修改后的元组。
	 */
	values = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(Datum));
	nulls = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	replaces = palloc0(RelationGetNumberOfAttributes(rel) * sizeof(bool));
	values[Anum_namespace - 1] = ObjectIdGetDatum(nspOid);
	replaces[Anum_namespace - 1] = true;
	newtup = heap_modify_tuple(tup, RelationGetDescr(rel),
							   values, nulls, replaces);

	/* Perform actual update */
	/*
	 *
	 * 执行实际更新。
	 */
	CatalogTupleUpdate(rel, &tup->t_self, newtup);

	/* Release memory */
	/*
	 *
	 * 释放内存。
	 */
	pfree(values);
	pfree(nulls);
	pfree(replaces);

	/* update dependency to point to the new schema */
	/*
	 *
	 * 把依赖改指向新模式。
	 */
	if (changeDependencyFor(classId, objid,
							NamespaceRelationId, oldNspOid, nspOid) != 1)
		elog(ERROR, "could not change schema dependency for object %u",
			 objid);

	InvokeObjectPostAlterHook(classId, objid, 0);

	return oldNspOid;
}

/*
 * Executes an ALTER OBJECT / OWNER TO statement.  Based on the object
 * type, the function appropriate to that type is executed.
 *
 * 执行 ALTER OBJECT / OWNER TO。按对象类型调用相应函数。
 */
ObjectAddress
ExecAlterOwnerStmt(AlterOwnerStmt *stmt)
{
	Oid			newowner = get_rolespec_oid(stmt->newowner, false);

	switch (stmt->objectType)
	{
		case OBJECT_DATABASE:
			return AlterDatabaseOwner(strVal(stmt->object), newowner);

		case OBJECT_SCHEMA:
			return AlterSchemaOwner(strVal(stmt->object), newowner);

		case OBJECT_TYPE:
		case OBJECT_DOMAIN:		/* same as TYPE */
		/*
		 *
		 * 与 TYPE 相同。
		 */
			return AlterTypeOwner(castNode(List, stmt->object), newowner, stmt->objectType);
			break;

		case OBJECT_FDW:
			return AlterForeignDataWrapperOwner(strVal(stmt->object),
												newowner);

		case OBJECT_FOREIGN_SERVER:
			return AlterForeignServerOwner(strVal(stmt->object),
										   newowner);

		case OBJECT_EVENT_TRIGGER:
			return AlterEventTriggerOwner(strVal(stmt->object),
										  newowner);

		case OBJECT_PUBLICATION:
			return AlterPublicationOwner(strVal(stmt->object),
										 newowner);

		case OBJECT_SUBSCRIPTION:
			return AlterSubscriptionOwner(strVal(stmt->object),
										  newowner);

			/* Generic cases */
			/*
			 *
			 * 通用情况。
			 */
		case OBJECT_AGGREGATE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_FUNCTION:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_OPERATOR:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TABLESPACE:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSCONFIGURATION:
			{
				ObjectAddress address;

				address = get_object_address(stmt->objectType,
											 stmt->object,
											 NULL,
											 AccessExclusiveLock,
											 false);

				AlterObjectOwner_internal(address.classId, address.objectId,
										  newowner);

				return address;
			}
			break;

		default:
			elog(ERROR, "unrecognized AlterOwnerStmt type: %d",
				 (int) stmt->objectType);
			return InvalidObjectAddress;	/* keep compiler happy */
			/*
			 *
			 * 避免编译器告警。
			 */
	}
}

/*
 * Generic function to change the ownership of a given object, for simple
 * cases (won't work for tables, nor other cases where we need to do more than
 * change the ownership column of a single catalog entry).
 *
 * 简单情况下更改对象属主的通用函数。不适用于表，也不适用于除了改一条目录记录的属主列之外还要做更多事的情况。
 *
 * classId: OID of catalog containing object
 * objectId: OID of object to change the ownership of
 * new_ownerId: OID of new object owner
 *
 * classId：存放该对象的目录 OID。
 * objectId：要改属主的对象 OID。
 * new_ownerId：新属主的 OID。
 *
 * This will work on large objects, but we have to beware of the fact that
 * classId isn't the OID of the catalog to modify in that case.
 *
 * 这也适用于大对象，但此时 classId 并不是要修改的那个目录的 OID。
 */
void
AlterObjectOwner_internal(Oid classId, Oid objectId, Oid new_ownerId)
{
	/* For large objects, the catalog to modify is pg_largeobject_metadata */
	/*
	 *
	 * 对大对象，要修改的目录是 pg_largeobject_metadata。
	 */
	Oid			catalogId = (classId == LargeObjectRelationId) ? LargeObjectMetadataRelationId : classId;
	AttrNumber	Anum_oid = get_object_attnum_oid(catalogId);
	AttrNumber	Anum_owner = get_object_attnum_owner(catalogId);
	AttrNumber	Anum_namespace = get_object_attnum_namespace(catalogId);
	AttrNumber	Anum_acl = get_object_attnum_acl(catalogId);
	AttrNumber	Anum_name = get_object_attnum_name(catalogId);
	Relation	rel;
	HeapTuple	oldtup;
	Datum		datum;
	bool		isnull;
	Oid			old_ownerId;
	Oid			namespaceId = InvalidOid;

	rel = table_open(catalogId, RowExclusiveLock);

	/* Search tuple and lock it. */
	/*
	 *
	 * 查找元组并加锁。
	 */
	oldtup =
		get_catalog_object_by_oid_extended(rel, Anum_oid, objectId, true);
	if (oldtup == NULL)
		elog(ERROR, "cache lookup failed for object %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(rel));

	datum = heap_getattr(oldtup, Anum_owner,
						 RelationGetDescr(rel), &isnull);
	Assert(!isnull);
	old_ownerId = DatumGetObjectId(datum);

	if (Anum_namespace != InvalidAttrNumber)
	{
		datum = heap_getattr(oldtup, Anum_namespace,
							 RelationGetDescr(rel), &isnull);
		Assert(!isnull);
		namespaceId = DatumGetObjectId(datum);
	}

	if (old_ownerId != new_ownerId)
	{
		AttrNumber	nattrs;
		HeapTuple	newtup;
		Datum	   *values;
		bool	   *nulls;
		bool	   *replaces;

		/* Superusers can bypass permission checks */
		/*
		 *
		 * 超级用户可以跳过权限检查。
		 */
		if (!superuser())
		{
			/* must be owner */
			/*
			 *
			 * 必须是属主。
			 */
			if (!has_privs_of_role(GetUserId(), old_ownerId))
			{
				char	   *objname;
				char		namebuf[NAMEDATALEN];

				if (Anum_name != InvalidAttrNumber)
				{
					datum = heap_getattr(oldtup, Anum_name,
										 RelationGetDescr(rel), &isnull);
					Assert(!isnull);
					objname = NameStr(*DatumGetName(datum));
				}
				else
				{
					snprintf(namebuf, sizeof(namebuf), "%u", objectId);
					objname = namebuf;
				}
				aclcheck_error(ACLCHECK_NOT_OWNER,
							   get_object_type(catalogId, objectId),
							   objname);
			}
			/* Must be able to become new owner */
			/*
			 *
			 * 必须能够成为新属主。
			 */
			check_can_set_role(GetUserId(), new_ownerId);

			/* New owner must have CREATE privilege on namespace */
			/*
			 *
			 * 新属主必须对命名空间有 CREATE 权限。
			 */
			if (OidIsValid(namespaceId))
			{
				AclResult	aclresult;

				aclresult = object_aclcheck(NamespaceRelationId, namespaceId, new_ownerId,
											ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_SCHEMA,
								   get_namespace_name(namespaceId));
			}
		}

		/* Build a modified tuple */
		/*
		 *
		 * 构造修改后的元组。
		 */
		nattrs = RelationGetNumberOfAttributes(rel);
		values = palloc0(nattrs * sizeof(Datum));
		nulls = palloc0(nattrs * sizeof(bool));
		replaces = palloc0(nattrs * sizeof(bool));
		values[Anum_owner - 1] = ObjectIdGetDatum(new_ownerId);
		replaces[Anum_owner - 1] = true;

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 *
		 * 计算新属主对应的 ACL。仅当 ACL 非空时需要。
		 */
		if (Anum_acl != InvalidAttrNumber)
		{
			datum = heap_getattr(oldtup,
								 Anum_acl, RelationGetDescr(rel), &isnull);
			if (!isnull)
			{
				Acl		   *newAcl;

				newAcl = aclnewowner(DatumGetAclP(datum),
									 old_ownerId, new_ownerId);
				values[Anum_acl - 1] = PointerGetDatum(newAcl);
				replaces[Anum_acl - 1] = true;
			}
		}

		newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
								   values, nulls, replaces);

		/* Perform actual update */
		/*
		 *
		 * 执行实际更新。
		 */
		CatalogTupleUpdate(rel, &newtup->t_self, newtup);

		UnlockTuple(rel, &oldtup->t_self, InplaceUpdateTupleLock);

		/* Update owner dependency reference */
		/*
		 *
		 * 更新属主依赖引用。
		 */
		changeDependencyOnOwner(classId, objectId, new_ownerId);

		/* Release memory */
		/*
		 *
		 * 释放内存。
		 */
		pfree(values);
		pfree(nulls);
		pfree(replaces);
	}
	else
		UnlockTuple(rel, &oldtup->t_self, InplaceUpdateTupleLock);

	/* Note the post-alter hook gets classId not catalogId */
	/*
	 *
	 * 注意：alter 之后的钩子收到的是 classId，不是 catalogId。
	 */
	InvokeObjectPostAlterHook(classId, objectId, 0);

	table_close(rel, RowExclusiveLock);
}
