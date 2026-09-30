/*-------------------------------------------------------------------------
 *
 * dropcmds.c
 *	  handle various "DROP" operations
 *
 * 处理各类 DROP 操作。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dropcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"


/*
 * 核心流程概览：
 * RemoveObjects 先逐个解析对象地址并检查权限，再一次性 performMultipleDeletions。
 * IF EXISTS 且对象不存在时，does_not_exist_skipping 及其子程序区分
 * “对象本身缺失”和“模式、所属关系或数据类型缺失”，并发出 NOTICE。
 */
static void does_not_exist_skipping(ObjectType objtype,
									Node *object);
static bool owningrel_does_not_exist_skipping(List *object,
											  const char **msg, char **name);
static bool schema_does_not_exist_skipping(List *object,
										   const char **msg, char **name);
static bool type_in_list_does_not_exist_skipping(List *typenames,
												 const char **msg, char **name);


/*
 * Drop one or more objects.
 *
 * 删除一个或多个对象。
 *
 * We don't currently handle all object types here.  Relations, for example,
 * require special handling, because (for example) indexes have additional
 * locking requirements.
 *
 * 这里并不处理全部对象类型。例如关系需要特殊处理，
 * 索引还有额外的加锁要求。
 *
 * We look up all the objects first, and then delete them in a single
 * performMultipleDeletions() call.  This avoids unnecessary DROP RESTRICT
 * errors if there are dependencies between them.
 *
 * 先查找全部对象，再一次 performMultipleDeletions() 删除。
 * 这样它们之间有依赖时不会多余地报 DROP RESTRICT 错误。
 */
void
RemoveObjects(DropStmt *stmt)
{
	ObjectAddresses *objects;
	ListCell   *cell1;

	objects = new_object_addresses();

	foreach(cell1, stmt->objects)
	{
		ObjectAddress address;
		Node	   *object = lfirst(cell1);
		Relation	relation = NULL;
		Oid			namespaceId;

		/* Get an ObjectAddress for the object. */
		/*
		 *
		 * 取得对象的 ObjectAddress。
		 */
		address = get_object_address(stmt->removeType,
									 object,
									 &relation,
									 AccessExclusiveLock,
									 stmt->missing_ok);

		/*
		 * Issue NOTICE if supplied object was not found.  Note this is only
		 * relevant in the missing_ok case, because otherwise
		 * get_object_address would have thrown an error.
		 *
		 * 找不到对象时发 NOTICE。这只在 missing_ok 时有意义，
		 * 否则 get_object_address 已经报错。
		 */
		if (!OidIsValid(address.objectId))
		{
			Assert(stmt->missing_ok);
			does_not_exist_skipping(stmt->removeType, object);
			continue;
		}

		/*
		 * Although COMMENT ON FUNCTION, SECURITY LABEL ON FUNCTION, etc. are
		 * happy to operate on an aggregate as on any other function, we have
		 * historically not allowed this for DROP FUNCTION.
		 *
		 * COMMENT ON FUNCTION、SECURITY LABEL ON FUNCTION 等可以把聚集当作普通函数，
		 * 但 DROP FUNCTION 历史上不允许这样。
		 */
		if (stmt->removeType == OBJECT_FUNCTION)
		{
			if (get_func_prokind(address.objectId) == PROKIND_AGGREGATE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is an aggregate function",
								NameListToString(castNode(ObjectWithArgs, object)->objname)),
						 errhint("Use DROP AGGREGATE to drop aggregate functions.")));
		}

		/* Check permissions. */
		/*
		 *
		 * 检查权限。
		 */
		namespaceId = get_object_namespace(&address);
		if (!OidIsValid(namespaceId) ||
			!object_ownercheck(NamespaceRelationId, namespaceId, GetUserId()))
			check_object_ownership(GetUserId(), stmt->removeType, address,
								   object, relation);

		/*
		 * Make note if a temporary namespace has been accessed in this
		 * transaction.
		 *
		 * 若本事务访问了临时命名空间，记下来。
		 */
		if (OidIsValid(namespaceId) && isTempNamespace(namespaceId))
			MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

		/* Release any relcache reference count, but keep lock until commit. */
		/*
		 *
		 * 释放 relcache 引用计数，但把锁保留到提交。
		 */
		if (relation)
			table_close(relation, NoLock);

		add_exact_object_address(&address, objects);
	}

	/* Here we really delete them. */
	/*
	 *
	 * 这里真正删除它们。
	 */
	performMultipleDeletions(objects, stmt->behavior, 0);

	free_object_addresses(objects);
}

/*
 * owningrel_does_not_exist_skipping
 *		Subroutine for RemoveObjects
 *
 * owningrel_does_not_exist_skipping：RemoveObjects 的子程序。
 *
 * After determining that a specification for a rule or trigger returns that
 * the specified object does not exist, test whether its owning relation, and
 * its schema, exist or not; if they do, return false --- the trigger or rule
 * itself is missing instead.  If the owning relation or its schema do not
 * exist, fill the error message format string and name, and return true.
 *
 * 规则或触发器不存在时，再查其所属关系和模式是否存在。
 * 若存在，返回 false，表示缺的是规则或触发器本身。
 * 若所属关系或模式不存在，填好错误格式串和名字并返回 true。
 */
static bool
owningrel_does_not_exist_skipping(List *object, const char **msg, char **name)
{
	List	   *parent_object;
	RangeVar   *parent_rel;

	parent_object = list_copy_head(object, list_length(object) - 1);

	if (schema_does_not_exist_skipping(parent_object, msg, name))
		return true;

	parent_rel = makeRangeVarFromNameList(parent_object);

	if (!OidIsValid(RangeVarGetRelid(parent_rel, NoLock, true)))
	{
		*msg = gettext_noop("relation \"%s\" does not exist, skipping");
		*name = NameListToString(parent_object);

		return true;
	}

	return false;
}

/*
 * schema_does_not_exist_skipping
 *		Subroutine for RemoveObjects
 *
 * schema_does_not_exist_skipping：RemoveObjects 的子程序。
 *
 * After determining that a specification for a schema-qualifiable object
 * refers to an object that does not exist, test whether the specified schema
 * exists or not.  If no schema was specified, or if the schema does exist,
 * return false -- the object itself is missing instead.  If the specified
 * schema does not exist, fill the error message format string and the
 * specified schema name, and return true.
 *
 * 可带模式名的对象不存在时，检查指定模式是否存在。
 * 未指定模式或模式存在则返回 false，表示缺的是对象本身。
 * 模式不存在则填好错误格式串和模式名并返回 true。
 */
static bool
schema_does_not_exist_skipping(List *object, const char **msg, char **name)
{
	RangeVar   *rel;

	rel = makeRangeVarFromNameList(object);

	if (rel->schemaname != NULL &&
		!OidIsValid(LookupNamespaceNoError(rel->schemaname)))
	{
		*msg = gettext_noop("schema \"%s\" does not exist, skipping");
		*name = rel->schemaname;

		return true;
	}

	return false;
}

/*
 * type_in_list_does_not_exist_skipping
 *		Subroutine for RemoveObjects
 *
 * type_in_list_does_not_exist_skipping：RemoveObjects 的子程序。
 *
 * After determining that a specification for a function, cast, aggregate or
 * operator returns that the specified object does not exist, test whether the
 * involved datatypes, and their schemas, exist or not; if they do, return
 * false --- the original object itself is missing instead.  If the datatypes
 * or schemas do not exist, fill the error message format string and the
 * missing name, and return true.
 *
 * 函数、转换、聚集或操作符不存在时，检查相关数据类型及其模式。
 * 若都存在，返回 false，表示缺的是原对象本身。
 * 类型或模式不存在则填好错误格式串和缺失的名字并返回 true。
 *
 * First parameter is a list of TypeNames.
 *
 * 第一个参数是 TypeName 的列表。
 */
static bool
type_in_list_does_not_exist_skipping(List *typenames, const char **msg,
									 char **name)
{
	ListCell   *l;

	foreach(l, typenames)
	{
		TypeName   *typeName = lfirst_node(TypeName, l);

		if (typeName != NULL)
		{
			if (!OidIsValid(LookupTypeNameOid(NULL, typeName, true)))
			{
				/* type doesn't exist, try to find why */
				/*
				 *
				 * 类型不存在，试着找出原因。
				 */
				if (schema_does_not_exist_skipping(typeName->names, msg, name))
					return true;

				*msg = gettext_noop("type \"%s\" does not exist, skipping");
				*name = TypeNameToString(typeName);

				return true;
			}
		}
	}

	return false;
}

/*
 * does_not_exist_skipping
 *		Subroutine for RemoveObjects
 *
 * does_not_exist_skipping：RemoveObjects 的子程序。
 *
 * Generate a NOTICE stating that the named object was not found, and is
 * being skipped.  This is only relevant when "IF EXISTS" is used; otherwise,
 * get_object_address() in RemoveObjects would have thrown an ERROR.
 *
 * 发出 NOTICE，说明未找到该对象并已跳过。这只在使用 "IF EXISTS" 时有意义，
 * 否则 RemoveObjects 里的 get_object_address() 已经报 ERROR。
 */
static void
does_not_exist_skipping(ObjectType objtype, Node *object)
{
	const char *msg = NULL;
	char	   *name = NULL;
	char	   *args = NULL;

	switch (objtype)
	{
		case OBJECT_ACCESS_METHOD:
			msg = gettext_noop("access method \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			{
				TypeName   *typ = castNode(TypeName, object);

				if (!schema_does_not_exist_skipping(typ->names, &msg, &name))
				{
					msg = gettext_noop("type \"%s\" does not exist, skipping");
					name = TypeNameToString(typ);
				}
			}
			break;
		case OBJECT_COLLATION:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("collation \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_CONVERSION:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("conversion \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_SCHEMA:
			msg = gettext_noop("schema \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_STATISTIC_EXT:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("statistics object \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSPARSER:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search parser \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSDICTIONARY:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search dictionary \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSTEMPLATE:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search template \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_TSCONFIGURATION:
			if (!schema_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("text search configuration \"%s\" does not exist, skipping");
				name = NameListToString(castNode(List, object));
			}
			break;
		case OBJECT_EXTENSION:
			msg = gettext_noop("extension \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_FUNCTION:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("function %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_PROCEDURE:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("procedure %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_ROUTINE:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("routine %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_AGGREGATE:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("aggregate %s(%s) does not exist, skipping");
					name = NameListToString(owa->objname);
					args = TypeNameListToString(owa->objargs);
				}
				break;
			}
		case OBJECT_OPERATOR:
			{
				ObjectWithArgs *owa = castNode(ObjectWithArgs, object);

				if (!schema_does_not_exist_skipping(owa->objname, &msg, &name) &&
					!type_in_list_does_not_exist_skipping(owa->objargs, &msg, &name))
				{
					msg = gettext_noop("operator %s does not exist, skipping");
					name = NameListToString(owa->objname);
				}
				break;
			}
		case OBJECT_LANGUAGE:
			msg = gettext_noop("language \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_CAST:
			{
				if (!type_in_list_does_not_exist_skipping(list_make1(linitial(castNode(List, object))), &msg, &name) &&
					!type_in_list_does_not_exist_skipping(list_make1(lsecond(castNode(List, object))), &msg, &name))
				{
					/* XXX quote or no quote? */
					/*
					 *
					 * XXX：名字要不要加引号？
					 */
					msg = gettext_noop("cast from type %s to type %s does not exist, skipping");
					name = TypeNameToString(linitial_node(TypeName, castNode(List, object)));
					args = TypeNameToString(lsecond_node(TypeName, castNode(List, object)));
				}
			}
			break;
		case OBJECT_TRANSFORM:
			if (!type_in_list_does_not_exist_skipping(list_make1(linitial(castNode(List, object))), &msg, &name))
			{
				msg = gettext_noop("transform for type %s language \"%s\" does not exist, skipping");
				name = TypeNameToString(linitial_node(TypeName, castNode(List, object)));
				args = strVal(lsecond(castNode(List, object)));
			}
			break;
		case OBJECT_TRIGGER:
			if (!owningrel_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("trigger \"%s\" for relation \"%s\" does not exist, skipping");
				name = strVal(llast(castNode(List, object)));
				args = NameListToString(list_copy_head(castNode(List, object),
													   list_length(castNode(List, object)) - 1));
			}
			break;
		case OBJECT_POLICY:
			if (!owningrel_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("policy \"%s\" for relation \"%s\" does not exist, skipping");
				name = strVal(llast(castNode(List, object)));
				args = NameListToString(list_copy_head(castNode(List, object),
													   list_length(castNode(List, object)) - 1));
			}
			break;
		case OBJECT_EVENT_TRIGGER:
			msg = gettext_noop("event trigger \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_RULE:
			if (!owningrel_does_not_exist_skipping(castNode(List, object), &msg, &name))
			{
				msg = gettext_noop("rule \"%s\" for relation \"%s\" does not exist, skipping");
				name = strVal(llast(castNode(List, object)));
				args = NameListToString(list_copy_head(castNode(List, object),
													   list_length(castNode(List, object)) - 1));
			}
			break;
		case OBJECT_FDW:
			msg = gettext_noop("foreign-data wrapper \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_FOREIGN_SERVER:
			msg = gettext_noop("server \"%s\" does not exist, skipping");
			name = strVal(object);
			break;
		case OBJECT_OPCLASS:
			{
				List	   *opcname = list_copy_tail(castNode(List, object), 1);

				if (!schema_does_not_exist_skipping(opcname, &msg, &name))
				{
					msg = gettext_noop("operator class \"%s\" does not exist for access method \"%s\", skipping");
					name = NameListToString(opcname);
					args = strVal(linitial(castNode(List, object)));
				}
			}
			break;
		case OBJECT_OPFAMILY:
			{
				List	   *opfname = list_copy_tail(castNode(List, object), 1);

				if (!schema_does_not_exist_skipping(opfname, &msg, &name))
				{
					msg = gettext_noop("operator family \"%s\" does not exist for access method \"%s\", skipping");
					name = NameListToString(opfname);
					args = strVal(linitial(castNode(List, object)));
				}
			}
			break;
		case OBJECT_PUBLICATION:
			msg = gettext_noop("publication \"%s\" does not exist, skipping");
			name = strVal(object);
			break;

		case OBJECT_COLUMN:
		case OBJECT_DATABASE:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_ROLE:
		case OBJECT_SEQUENCE:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLE:
		case OBJECT_TABLESPACE:
		case OBJECT_VIEW:

			/*
			 * These are handled elsewhere, so if someone gets here the code
			 * is probably wrong or should be revisited.
			 *
			 * 这些情况在别处处理；若执行到这里，代码可能有误或需要重看。
			 */
			elog(ERROR, "unsupported object type: %d", (int) objtype);
			break;

		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_LARGEOBJECT:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_USER_MAPPING:
			/* These are currently not used or needed. */
			/*
			 *
			 * 这些目前未使用，也不需要。
			 */
			elog(ERROR, "unsupported object type: %d", (int) objtype);
			break;

			/* no default, to let compiler warn about missing case */
			/*
			 *
			 * 不写 default，以便编译器对遗漏的 case 告警。
			 */
	}
	if (!msg)
		elog(ERROR, "unrecognized object type: %d", (int) objtype);

	if (!args)
		ereport(NOTICE, (errmsg(msg, name)));
	else
		ereport(NOTICE, (errmsg(msg, name, args)));
}
