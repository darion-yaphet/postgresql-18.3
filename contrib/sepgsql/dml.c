/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/dml.c
 *
 * Routines to handle DML permission checks
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/tupdesc.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "commands/seclabel.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "nodes/bitmapset.h"
#include "parser/parsetree.h"
#include "sepgsql.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * fixup_whole_row_references
 *
 * When user references a whole-row Var, it is equivalent to referencing
 * all the user columns (not system columns). So, we need to fix up the
 * given bitmapset, if it contains a whole-row reference.
 *
 * 当用户引用整行Var时，相当于引用所有用户列（而不是系统列）。因此，如果给定的位图集包含整行引用，我们需要修复它。
 */
static Bitmapset *
fixup_whole_row_references(Oid relOid, Bitmapset *columns)
{
	Bitmapset  *result;
	HeapTuple	tuple;
	AttrNumber	natts;
	AttrNumber	attno;
	int			index;

	/* if no whole-row references, nothing to do
	 *
	 * 如果没有整行引用，则无需执行任何操作
	 */
	index = InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber;
	if (!bms_is_member(index, columns))
		return columns;

	/* obtain number of attributes
	 *
	 * 获取属性个数
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	natts = ((Form_pg_class) GETSTRUCT(tuple))->relnatts;
	ReleaseSysCache(tuple);

	/* remove bit 0 from column set, add in all the non-dropped columns
	 *
	 * 从列集中删除位 0，添加所有未删除的列
	 */
	result = bms_copy(columns);
	result = bms_del_member(result, index);

	for (attno = 1; attno <= natts; attno++)
	{
		tuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(relOid),
								Int16GetDatum(attno));
		if (!HeapTupleIsValid(tuple))
			continue;			/* unexpected case, should we error? */

		if (!((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
		{
			index = attno - FirstLowInvalidHeapAttributeNumber;
			result = bms_add_member(result, index);
		}

		ReleaseSysCache(tuple);
	}
	return result;
}

/*
 * fixup_inherited_columns
 *
 * When user is querying on a table with children, it implicitly accesses
 * child tables also. So, we also need to check security label of child
 * tables and columns, but there is no guarantee attribute numbers are
 * same between the parent and children.
 * It returns a bitmapset which contains attribute number of the child
 * table based on the given bitmapset of the parent.
 *
 * 当用户查询包含子表的表时，它也会隐式访问子表。因此，我们还需要检查子表和列的安全标签，但不能保证父子表之间的属性号相同。它返回一个位图集，其中包含基于父级给定位图集的子表的属性号。
 */
static Bitmapset *
fixup_inherited_columns(Oid parentId, Oid childId, Bitmapset *columns)
{
	Bitmapset  *result = NULL;
	int			index;

	/*
	 * obviously, no need to do anything here
	 *
	 * 显然，不需要在这里做任何事情
	 */
	if (parentId == childId)
		return columns;

	index = -1;
	while ((index = bms_next_member(columns, index)) >= 0)
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber
		 *
		 * 位编号由 FirstLowInvalidHeapAttributeNumber 偏移
		 */
		AttrNumber	attno = index + FirstLowInvalidHeapAttributeNumber;
		char	   *attname;

		/*
		 * whole-row-reference shall be fixed-up later
		 *
		 * 整行引用将在稍后修复
		 */
		if (attno == InvalidAttrNumber)
		{
			result = bms_add_member(result, index);
			continue;
		}

		attname = get_attname(parentId, attno, false);
		attno = get_attnum(childId, attname);
		if (attno == InvalidAttrNumber)
			elog(ERROR, "cache lookup failed for attribute %s of relation %u",
				 attname, childId);

		result = bms_add_member(result,
								attno - FirstLowInvalidHeapAttributeNumber);

		pfree(attname);
	}

	return result;
}

/*
 * check_relation_privileges
 *
 * It actually checks required permissions on a certain relation
 * and its columns.
 *
 * 它实际上检查特定关系及其列所需的权限。
 */
static bool
check_relation_privileges(Oid relOid,
						  Bitmapset *selected,
						  Bitmapset *inserted,
						  Bitmapset *updated,
						  uint32 required,
						  bool abort_on_violation)
{
	ObjectAddress object;
	char	   *audit_name;
	Bitmapset  *columns;
	int			index;
	char		relkind = get_rel_relkind(relOid);
	bool		result = true;

	/*
	 * Hardwired Policies: SE-PostgreSQL enforces - clients cannot modify
	 * system catalogs using DMLs - clients cannot reference/modify toast
	 * relations using DMLs
	 *
	 * 硬连线策略：SE-PostgreSQL 强制 - 客户端无法使用 DML 修改系统目录 - 客户端无法使用 DML 引用/修改 toast 关系
	 */
	if (sepgsql_getenforce() > 0)
	{
		if ((required & (SEPG_DB_TABLE__UPDATE |
						 SEPG_DB_TABLE__INSERT |
						 SEPG_DB_TABLE__DELETE)) != 0 &&
			IsCatalogRelationOid(relOid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("SELinux: hardwired security policy violation")));

		if (relkind == RELKIND_TOASTVALUE)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("SELinux: hardwired security policy violation")));
	}

	/*
	 * Check permissions on the relation
	 *
	 * 检查关系的权限
	 */
	object.classId = RelationRelationId;
	object.objectId = relOid;
	object.objectSubId = 0;
	audit_name = getObjectIdentity(&object, false);
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			result = sepgsql_avc_check_perms(&object,
											 SEPG_CLASS_DB_TABLE,
											 required,
											 audit_name,
											 abort_on_violation);
			break;

		case RELKIND_SEQUENCE:
			Assert((required & ~SEPG_DB_TABLE__SELECT) == 0);

			if (required & SEPG_DB_TABLE__SELECT)
				result = sepgsql_avc_check_perms(&object,
												 SEPG_CLASS_DB_SEQUENCE,
												 SEPG_DB_SEQUENCE__GET_VALUE,
												 audit_name,
												 abort_on_violation);
			break;

		case RELKIND_VIEW:
			result = sepgsql_avc_check_perms(&object,
											 SEPG_CLASS_DB_VIEW,
											 SEPG_DB_VIEW__EXPAND,
											 audit_name,
											 abort_on_violation);
			break;

		default:
			/* nothing to be checked
			 *
			 * 没有什么需要检查的
			 */
			break;
	}
	pfree(audit_name);

	/*
	 * Only columns owned by relations shall be checked
	 *
	 * 仅应检查关系拥有的列
	 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
		return true;

	/*
	 * Check permissions on the columns
	 *
	 * 检查列的权限
	 */
	selected = fixup_whole_row_references(relOid, selected);
	inserted = fixup_whole_row_references(relOid, inserted);
	updated = fixup_whole_row_references(relOid, updated);
	columns = bms_union(selected, bms_union(inserted, updated));

	index = -1;
	while ((index = bms_next_member(columns, index)) >= 0)
	{
		AttrNumber	attnum;
		uint32		column_perms = 0;

		if (bms_is_member(index, selected))
			column_perms |= SEPG_DB_COLUMN__SELECT;
		if (bms_is_member(index, inserted))
		{
			if (required & SEPG_DB_TABLE__INSERT)
				column_perms |= SEPG_DB_COLUMN__INSERT;
		}
		if (bms_is_member(index, updated))
		{
			if (required & SEPG_DB_TABLE__UPDATE)
				column_perms |= SEPG_DB_COLUMN__UPDATE;
		}
		if (column_perms == 0)
			continue;

		/* obtain column's permission
		 *
		 * 获得专栏许可
		 */
		attnum = index + FirstLowInvalidHeapAttributeNumber;

		object.classId = RelationRelationId;
		object.objectId = relOid;
		object.objectSubId = attnum;
		audit_name = getObjectDescription(&object, false);

		result = sepgsql_avc_check_perms(&object,
										 SEPG_CLASS_DB_COLUMN,
										 column_perms,
										 audit_name,
										 abort_on_violation);
		pfree(audit_name);

		if (!result)
			return result;
	}
	return true;
}

/*
 * sepgsql_dml_privileges
 *
 * Entrypoint of the DML permission checks
 *
 * DML权限检查的入口点
 */
bool
sepgsql_dml_privileges(List *rangeTbls, List *rteperminfos,
					   bool abort_on_violation)
{
	ListCell   *lr;

	foreach(lr, rteperminfos)
	{
		RTEPermissionInfo *perminfo = lfirst_node(RTEPermissionInfo, lr);
		uint32		required = 0;
		List	   *tableIds;
		ListCell   *li;

		/*
		 * Find out required permissions
		 *
		 * 找出所需的权限
		 */
		if (perminfo->requiredPerms & ACL_SELECT)
			required |= SEPG_DB_TABLE__SELECT;
		if (perminfo->requiredPerms & ACL_INSERT)
			required |= SEPG_DB_TABLE__INSERT;
		if (perminfo->requiredPerms & ACL_UPDATE)
		{
			if (!bms_is_empty(perminfo->updatedCols))
				required |= SEPG_DB_TABLE__UPDATE;
			else
				required |= SEPG_DB_TABLE__LOCK;
		}
		if (perminfo->requiredPerms & ACL_DELETE)
			required |= SEPG_DB_TABLE__DELETE;

		/*
		 * Skip, if nothing to be checked
		 *
		 * 如果没有什么要检查的，则跳过
		 */
		if (required == 0)
			continue;

		/*
		 * If this RangeTblEntry is also supposed to reference inherited
		 * tables, we need to check security label of the child tables. So, we
		 * expand rte->relid into list of OIDs of inheritance hierarchy, then
		 * checker routine will be invoked for each relations.
		 *
		 * 如果这个RangeTblEntry也应该引用继承的表，我们需要检查子表的安全标签。因此，我们将 rte->relid 扩展为继承层次结构的 OID 列表，然后将为每个关系调用检查器例程。
		 */
		if (!perminfo->inh)
			tableIds = list_make1_oid(perminfo->relid);
		else
			tableIds = find_all_inheritors(perminfo->relid, NoLock, NULL);

		foreach(li, tableIds)
		{
			Oid			tableOid = lfirst_oid(li);
			Bitmapset  *selectedCols;
			Bitmapset  *insertedCols;
			Bitmapset  *updatedCols;

			/*
			 * child table has different attribute numbers, so we need to fix
			 * up them.
			 *
			 * 子表有不同的属性编号，因此我们需要修复它们。
			 */
			selectedCols = fixup_inherited_columns(perminfo->relid, tableOid,
												   perminfo->selectedCols);
			insertedCols = fixup_inherited_columns(perminfo->relid, tableOid,
												   perminfo->insertedCols);
			updatedCols = fixup_inherited_columns(perminfo->relid, tableOid,
												  perminfo->updatedCols);

			/*
			 * check permissions on individual tables
			 *
			 * 检查各个表的权限
			 */
			if (!check_relation_privileges(tableOid,
										   selectedCols,
										   insertedCols,
										   updatedCols,
										   required, abort_on_violation))
				return false;
		}
		list_free(tableIds);
	}
	return true;
}
