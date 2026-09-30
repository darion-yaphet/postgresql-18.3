/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL EVENT TRIGGER support code.
 *
 * PostgreSQL 的 EVENT TRIGGER 支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_database.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * CreateEventTrigger：校验事件名、过滤条件与函数，写入 pg_event_trigger 并登记依赖。
 * EventTriggerDDLCommandStart / EventTriggerDDLCommandEnd：单用户模式或 GUC 关闭时直接返回，否则按 command tag 触发。
 * EventTriggerSQLDrop / EventTriggerTableRewrite：把已收集的删除对象或表重写信息交给对应事件的触发器函数。
 * EventTriggerOnLogin：若 pg_database.dathasloginevt 为真则执行登录事件触发器，没有触发器时清除该标志。
 * EventTriggerBeginCompleteQuery：为 sql_drop、table_rewrite、ddl_command_end 准备当前命令状态。
 * EventTriggerCollect*：在效用命令路径上收集 DDL，供 pg_event_trigger_ddl_commands() 反解析。
 */

typedef struct EventTriggerQueryState
{
	/* memory context for this state's objects */
	/*
	 *
	 * 本状态对象使用的内存上下文
	 */
	MemoryContext cxt;

	/* sql_drop */
	/*
	 *
	 * 事件 sql_drop
	 */
	slist_head	SQLDropList;
	bool		in_sql_drop;

	/* table_rewrite */
	/*
	 *
	 * 事件 table_rewrite
	 */
	Oid			table_rewrite_oid;	/* InvalidOid, or set for table_rewrite
									 * event */
	int			table_rewrite_reason;	/* AT_REWRITE reason */
	/*
	 *
	 * AT_REWRITE 的原因
	 */

	/* Support for command collection */
	/*
	 *
	 * 命令收集支持
	 */
	bool		commandCollectionInhibited;
	CollectedCommand *currentCommand;
	List	   *commandList;	/* list of CollectedCommand; see
								 * deparse_utility.h */
	struct EventTriggerQueryState *previous;
} EventTriggerQueryState;

static EventTriggerQueryState *currentEventTriggerState = NULL;

/* GUC parameter */
/*
 *
 * GUC 参数
 */
bool		event_triggers = true;

/* Support for dropped objects */
/*
 *
 * 对被删除对象的支持
 */
typedef struct SQLDropObject
{
	ObjectAddress address;
	const char *schemaname;
	const char *objname;
	const char *objidentity;
	const char *objecttype;
	List	   *addrnames;
	List	   *addrargs;
	bool		original;
	bool		normal;
	bool		istemp;
	slist_node	next;
} SQLDropObject;

static void AlterEventTriggerOwner_internal(Relation rel,
											HeapTuple tup,
											Oid newOwnerId);
static void error_duplicate_filter_variable(const char *defname);
static Datum filter_list_to_array(List *filterlist);
static Oid	insert_event_trigger_tuple(const char *trigname, const char *eventname,
									   Oid evtOwner, Oid funcoid, List *taglist);
static void validate_ddl_tags(const char *filtervar, List *taglist);
static void validate_table_rewrite_tags(const char *filtervar, List *taglist);
static void EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata);
static bool obtain_object_name_namespace(const ObjectAddress *object,
										 SQLDropObject *obj);
static const char *stringify_grant_objtype(ObjectType objtype);
static const char *stringify_adefprivs_objtype(ObjectType objtype);
static void SetDatabaseHasLoginEventTriggers(void);

/*
 * Create an event trigger.
 *
 * 创建一个事件触发器。
 */
Oid
CreateEventTrigger(CreateEventTrigStmt *stmt)
{
	HeapTuple	tuple;
	Oid			funcoid;
	Oid			funcrettype;
	Oid			evtowner = GetUserId();
	ListCell   *lc;
	List	   *tags = NULL;

	/*
	 * It would be nice to allow database owners or even regular users to do
	 * this, but there are obvious privilege escalation risks which would have
	 * to somehow be plugged first.
	 *
	 * 最好也能让数据库属主甚至普通用户做这件事，但存在明显的权限提升风险，必须先堵住。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create event trigger \"%s\"",
						stmt->trigname),
				 errhint("Must be superuser to create an event trigger.")));

	/* Validate event name. */
	/*
	 *
	 * 校验事件名。
	 */
	if (strcmp(stmt->eventname, "ddl_command_start") != 0 &&
		strcmp(stmt->eventname, "ddl_command_end") != 0 &&
		strcmp(stmt->eventname, "sql_drop") != 0 &&
		strcmp(stmt->eventname, "login") != 0 &&
		strcmp(stmt->eventname, "table_rewrite") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unrecognized event name \"%s\"",
						stmt->eventname)));

	/* Validate filter conditions. */
	/*
	 *
	 * 校验过滤条件。
	 */
	foreach(lc, stmt->whenclause)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "tag") == 0)
		{
			if (tags != NULL)
				error_duplicate_filter_variable(def->defname);
			tags = (List *) def->arg;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized filter variable \"%s\"", def->defname)));
	}

	/* Validate tag list, if any. */
	/*
	 *
	 * 若有标签列表，则校验它。
	 */
	if ((strcmp(stmt->eventname, "ddl_command_start") == 0 ||
		 strcmp(stmt->eventname, "ddl_command_end") == 0 ||
		 strcmp(stmt->eventname, "sql_drop") == 0)
		&& tags != NULL)
		validate_ddl_tags("tag", tags);
	else if (strcmp(stmt->eventname, "table_rewrite") == 0
			 && tags != NULL)
		validate_table_rewrite_tags("tag", tags);
	else if (strcmp(stmt->eventname, "login") == 0 && tags != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("tag filtering is not supported for login event triggers")));

	/*
	 * Give user a nice error message if an event trigger of the same name
	 * already exists.
	 *
	 * 若已存在同名事件触发器，给用户一条清晰的错误信息。
	 */
	tuple = SearchSysCache1(EVENTTRIGGERNAME, CStringGetDatum(stmt->trigname));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists",
						stmt->trigname)));

	/* Find and validate the trigger function. */
	/*
	 *
	 * 查找并校验触发器函数。
	 */
	funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != EVENT_TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->funcname), "event_trigger")));

	/* Insert catalog entries. */
	/*
	 *
	 * 插入目录项。
	 */
	return insert_event_trigger_tuple(stmt->trigname, stmt->eventname,
									  evtowner, funcoid, tags);
}

/*
 * Validate DDL command tags.
 *
 * 校验 DDL 命令标签。
 */
static void
validate_ddl_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tagstr = strVal(lfirst(lc));
		CommandTag	commandTag = GetCommandTagEnum(tagstr);

		if (commandTag == CMDTAG_UNKNOWN)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("filter value \"%s\" not recognized for filter variable \"%s\"",
							tagstr, filtervar)));
		if (!command_tag_event_trigger_ok(commandTag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
			/*
			 *
			 * 供翻译者：%s 表示一条 SQL 语句名
			 */
					 errmsg("event triggers are not supported for %s",
							tagstr)));
	}
}

/*
 * Validate DDL command tags for event table_rewrite.
 *
 * 校验事件 table_rewrite 的 DDL 命令标签。
 */
static void
validate_table_rewrite_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tagstr = strVal(lfirst(lc));
		CommandTag	commandTag = GetCommandTagEnum(tagstr);

		if (!command_tag_table_rewrite_ok(commandTag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
			/*
			 *
			 * 供翻译者：%s 表示一条 SQL 语句名
			 */
					 errmsg("event triggers are not supported for %s",
							tagstr)));
	}
}

/*
 * Complain about a duplicate filter variable.
 *
 * 过滤变量重复时发出抱怨。
 */
static void
error_duplicate_filter_variable(const char *defname)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("filter variable \"%s\" specified more than once",
					defname)));
}

/*
 * Insert the new pg_event_trigger row and record dependencies.
 *
 * 插入新的 pg_event_trigger 行并记录依赖。
 */
static Oid
insert_event_trigger_tuple(const char *trigname, const char *eventname, Oid evtOwner,
						   Oid funcoid, List *taglist)
{
	Relation	tgrel;
	Oid			trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_event_trigger];
	bool		nulls[Natts_pg_event_trigger];
	NameData	evtnamedata,
				evteventdata;
	ObjectAddress myself,
				referenced;

	/* Open pg_event_trigger. */
	/*
	 *
	 * 打开 pg_event_trigger。
	 */
	tgrel = table_open(EventTriggerRelationId, RowExclusiveLock);

	/* Build the new pg_trigger tuple. */
	/*
	 *
	 * 构造新的 pg_trigger 元组。
	 */
	trigoid = GetNewOidWithIndex(tgrel, EventTriggerOidIndexId,
								 Anum_pg_event_trigger_oid);
	values[Anum_pg_event_trigger_oid - 1] = ObjectIdGetDatum(trigoid);
	memset(nulls, false, sizeof(nulls));
	namestrcpy(&evtnamedata, trigname);
	values[Anum_pg_event_trigger_evtname - 1] = NameGetDatum(&evtnamedata);
	namestrcpy(&evteventdata, eventname);
	values[Anum_pg_event_trigger_evtevent - 1] = NameGetDatum(&evteventdata);
	values[Anum_pg_event_trigger_evtowner - 1] = ObjectIdGetDatum(evtOwner);
	values[Anum_pg_event_trigger_evtfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_event_trigger_evtenabled - 1] =
		CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);
	if (taglist == NIL)
		nulls[Anum_pg_event_trigger_evttags - 1] = true;
	else
		values[Anum_pg_event_trigger_evttags - 1] =
			filter_list_to_array(taglist);

	/* Insert heap tuple. */
	/*
	 *
	 * 插入堆元组。
	 */
	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);
	CatalogTupleInsert(tgrel, tuple);
	heap_freetuple(tuple);

	/*
	 * Login event triggers have an additional flag in pg_database to enable
	 * faster lookups in hot codepaths. Set the flag unless already True.
	 *
	 * 登录事件触发器在 pg_database 中多一个标志，以便热路径更快查找。除非已经为真，否则设置该标志。
	 */
	if (strcmp(eventname, "login") == 0)
		SetDatabaseHasLoginEventTriggers();

	/* Depend on owner. */
	/*
	 *
	 * 依赖于属主。
	 */
	recordDependencyOnOwner(EventTriggerRelationId, trigoid, evtOwner);

	/* Depend on event trigger function. */
	/*
	 *
	 * 依赖于事件触发器函数。
	 */
	myself.classId = EventTriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;
	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Depend on extension, if any. */
	/*
	 *
	 * 若属于扩展，则依赖于该扩展。
	 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new event trigger */
	/*
	 *
	 * 新事件触发器的创建后钩子
	 */
	InvokeObjectPostCreateHook(EventTriggerRelationId, trigoid, 0);

	/* Close pg_event_trigger. */
	/*
	 *
	 * 关闭 pg_event_trigger。
	 */
	table_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
 * In the parser, a clause like WHEN tag IN ('cmd1', 'cmd2') is represented
 * by a DefElem whose value is a List of String nodes; in the catalog, we
 * store the list of strings as a text array.  This function transforms the
 * former representation into the latter one.
 *
 * 在解析器中，WHEN tag IN ('cmd1', 'cmd2') 这类子句表示为值是 String 节点 List 的 DefElem；
 * 在目录中，我们把字符串列表存成 text 数组。本函数把前一种表示转换成后一种。
 *
 * For cleanliness, we store command tags in the catalog as text.  It's
 * possible (although not currently anticipated) that we might have
 * a case-sensitive filter variable in the future, in which case this would
 * need some further adjustment.
 *
 * 为了干净，命令标签在目录中以 text 存放。将来也许（目前没有这种打算）会有区分大小写的过滤变量，
 * 那时这里还需要进一步调整。
 */
static Datum
filter_list_to_array(List *filterlist)
{
	ListCell   *lc;
	Datum	   *data;
	int			i = 0,
				l = list_length(filterlist);

	data = (Datum *) palloc(l * sizeof(Datum));

	foreach(lc, filterlist)
	{
		const char *value = strVal(lfirst(lc));
		char	   *result,
				   *p;

		result = pstrdup(value);
		for (p = result; *p; p++)
			*p = pg_ascii_toupper((unsigned char) *p);
		data[i++] = PointerGetDatum(cstring_to_text(result));
		pfree(result);
	}

	return PointerGetDatum(construct_array_builtin(data, l, TEXTOID));
}

/*
 * Set pg_database.dathasloginevt flag for current database indicating that
 * current database has on login event triggers.
 *
 * 设置当前数据库的 pg_database.dathasloginevt 标志，表示本库有登录事件触发器。
 */
void
SetDatabaseHasLoginEventTriggers(void)
{
	/* Set dathasloginevt flag in pg_database */
	/*
	 *
	 * 在 pg_database 中设置 dathasloginevt 标志
	 */
	Form_pg_database db;
	Relation	pg_db = table_open(DatabaseRelationId, RowExclusiveLock);
	ItemPointerData otid;
	HeapTuple	tuple;

	/*
	 * Use shared lock to prevent a conflict with EventTriggerOnLogin() trying
	 * to reset pg_database.dathasloginevt flag.  Note, this lock doesn't
	 * effectively blocks database or other objection.  It's just custom lock
	 * tag used to prevent multiple backends changing
	 * pg_database.dathasloginevt flag.
	 *
	 * 使用共享锁，避免与试图重置 pg_database.dathasloginevt 的 EventTriggerOnLogin() 冲突。
	 * 注意，这把锁并不真正阻塞数据库或其他对象，只是用来防止多个后端同时修改该标志的自定义锁标签。
	 */
	LockSharedObject(DatabaseRelationId, MyDatabaseId, 0, AccessExclusiveLock);

	tuple = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	otid = tuple->t_self;
	db = (Form_pg_database) GETSTRUCT(tuple);
	if (!db->dathasloginevt)
	{
		db->dathasloginevt = true;
		CatalogTupleUpdate(pg_db, &otid, tuple);
		CommandCounterIncrement();
	}
	UnlockTuple(pg_db, &otid, InplaceUpdateTupleLock);
	table_close(pg_db, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER EVENT TRIGGER foo ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 *
 * ALTER EVENT TRIGGER foo ENABLE、DISABLE、ENABLE ALWAYS 或 ENABLE REPLICA
 */
Oid
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
	Oid			trigoid;
	Form_pg_event_trigger evtForm;
	char		tgenabled = stmt->tgenabled;

	tgrel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME,
							  CStringGetDatum(stmt->trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist",
						stmt->trigname)));

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	trigoid = evtForm->oid;

	if (!object_ownercheck(EventTriggerRelationId, trigoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EVENT_TRIGGER,
					   stmt->trigname);

	/* tuple is a copy, so we can modify it below */
	/*
	 *
	 * 元组是副本，因此下面可以修改它
	 */
	evtForm->evtenabled = tgenabled;

	CatalogTupleUpdate(tgrel, &tup->t_self, tup);

	/*
	 * Login event triggers have an additional flag in pg_database to enable
	 * faster lookups in hot codepaths. Set the flag unless already True.
	 *
	 * 登录事件触发器在 pg_database 中多一个标志，以便热路径更快查找。除非已经为真，否则设置该标志。
	 */
	if (namestrcmp(&evtForm->evtevent, "login") == 0 &&
		tgenabled != TRIGGER_DISABLED)
		SetDatabaseHasLoginEventTriggers();

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  trigoid, 0);

	/* clean up */
	/*
	 *
	 * 清理
	 */
	heap_freetuple(tup);
	table_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
 * Change event trigger's owner -- by name
 *
 * 按名称更改事件触发器的属主
 */
ObjectAddress
AlterEventTriggerOwner(const char *name, Oid newOwnerId)
{
	Oid			evtOid;
	HeapTuple	tup;
	Form_pg_event_trigger evtForm;
	Relation	rel;
	ObjectAddress address;

	rel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	evtOid = evtForm->oid;

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, EventTriggerRelationId, evtOid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change event trigger owner, by OID
 *
 * 按 OID 更改事件触发器的属主
 */
void
AlterEventTriggerOwner_oid(Oid trigOid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger with OID %u does not exist", trigOid)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Internal workhorse for changing an event trigger's owner
 *
 * 更改事件触发器属主的内部主力函数
 */
static void
AlterEventTriggerOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_event_trigger form;

	form = (Form_pg_event_trigger) GETSTRUCT(tup);

	if (form->evtowner == newOwnerId)
		return;

	if (!object_ownercheck(EventTriggerRelationId, form->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EVENT_TRIGGER,
					   NameStr(form->evtname));

	/* New owner must be a superuser */
	/*
	 *
	 * 新属主必须是超级用户
	 */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of event trigger \"%s\"",
						NameStr(form->evtname)),
				 errhint("The owner of an event trigger must be a superuser.")));

	form->evtowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Update owner dependency reference */
	/*
	 *
	 * 更新属主依赖引用
	 */
	changeDependencyOnOwner(EventTriggerRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  form->oid, 0);
}

/*
 * get_event_trigger_oid - Look up an event trigger by name to find its OID.
 *
 * get_event_trigger_oid：按名称查找事件触发器并得到其 OID。
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 *
 * 若 missing_ok 为 false，找不到触发器时抛错。若为 true，则只返回 InvalidOid。
 */
Oid
get_event_trigger_oid(const char *trigname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(EVENTTRIGGERNAME, Anum_pg_event_trigger_oid,
						  CStringGetDatum(trigname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", trigname)));
	return oid;
}

/*
 * Return true when we want to fire given Event Trigger and false otherwise,
 * filtering on the session replication role and the event trigger registered
 * tags matching.
 *
 * 若应触发给定的事件触发器则返回 true，否则返回 false；
 * 按会话复制角色以及已登记标签是否匹配来过滤。
 */
static bool
filter_event_trigger(CommandTag tag, EventTriggerCacheItem *item)
{
	/*
	 * Filter by session replication role, knowing that we never see disabled
	 * items down here.
	 *
	 * 按会话复制角色过滤；走到这里时不会再见到被禁用的项。
	 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (item->enabled == TRIGGER_FIRES_ON_ORIGIN)
			return false;
	}
	else
	{
		if (item->enabled == TRIGGER_FIRES_ON_REPLICA)
			return false;
	}

	/* Filter by tags, if any were specified. */
	/*
	 *
	 * 若指定了标签，则按标签过滤。
	 */
	if (!bms_is_empty(item->tagset) && !bms_is_member(tag, item->tagset))
		return false;

	/* if we reach that point, we're not filtering out this item */
	/*
	 *
	 * 若走到这里，就不过滤掉此项
	 */
	return true;
}

/*
 * 从解析树取出本次事件的 CommandTag；登录事件固定返回 CMDTAG_LOGIN。
 */
static CommandTag
EventTriggerGetTag(Node *parsetree, EventTriggerEvent event)
{
	if (event == EVT_Login)
		return CMDTAG_LOGIN;
	else
		return CreateCommandTag(parsetree);
}

/*
 * Setup for running triggers for the given event.  Return value is an OID list
 * of functions to run; if there are any, trigdata is filled with an
 * appropriate EventTriggerData for them to receive.
 *
 * 为运行给定事件的触发器做准备。返回值是要运行的函数 OID 列表；
 * 若有函数，则用合适的 EventTriggerData 填充 trigdata，供它们接收。
 */
static List *
EventTriggerCommonSetup(Node *parsetree,
						EventTriggerEvent event, const char *eventstr,
						EventTriggerData *trigdata, bool unfiltered)
{
	CommandTag	tag;
	List	   *cachelist;
	ListCell   *lc;
	List	   *runlist = NIL;

	/*
	 * We want the list of command tags for which this procedure is actually
	 * invoked to match up exactly with the list that CREATE EVENT TRIGGER
	 * accepts.  This debugging cross-check will throw an error if this
	 * function is invoked for a command tag that CREATE EVENT TRIGGER won't
	 * accept.  (Unfortunately, there doesn't seem to be any simple, automated
	 * way to verify that CREATE EVENT TRIGGER doesn't accept extra stuff that
	 * never reaches this control point.)
	 *
	 * 我们希望实际调用此过程的命令标签列表，与 CREATE EVENT TRIGGER 接受的列表完全一致。
	 * 若本函数被一个 CREATE EVENT TRIGGER 不接受的命令标签调用，这个调试交叉检查会抛错。
	 * （可惜似乎没有简单的自动办法，验证 CREATE EVENT TRIGGER 不会接受永远到不了这个控制点的额外内容。）
	 *
	 * If this cross-check fails for you, you probably need to either adjust
	 * standard_ProcessUtility() not to invoke event triggers for the command
	 * type in question, or you need to adjust event_trigger_ok to accept the
	 * relevant command tag.
	 *
	 * 若这项交叉检查对你失败，可能需要调整 standard_ProcessUtility()，不要为该命令类型调用事件触发器，
	 * 或者调整 event_trigger_ok 以接受相应的命令标签。
	 */
#ifdef USE_ASSERT_CHECKING
	{
		CommandTag	dbgtag;

		dbgtag = EventTriggerGetTag(parsetree, event);

		if (event == EVT_DDLCommandStart ||
			event == EVT_DDLCommandEnd ||
			event == EVT_SQLDrop ||
			event == EVT_Login)
		{
			if (!command_tag_event_trigger_ok(dbgtag))
				elog(ERROR, "unexpected command tag \"%s\"", GetCommandTagName(dbgtag));
		}
		else if (event == EVT_TableRewrite)
		{
			if (!command_tag_table_rewrite_ok(dbgtag))
				elog(ERROR, "unexpected command tag \"%s\"", GetCommandTagName(dbgtag));
		}
	}
#endif

	/* Use cache to find triggers for this event; fast exit if none. */
	/*
	 *
	 * 用缓存查找此事件的触发器；若没有则快速退出。
	 */
	cachelist = EventCacheLookup(event);
	if (cachelist == NIL)
		return NIL;

	/* Get the command tag. */
	/*
	 *
	 * 取得命令标签。
	 */
	tag = EventTriggerGetTag(parsetree, event);

	/*
	 * Filter list of event triggers by command tag, and copy them into our
	 * memory context.  Once we start running the command triggers, or indeed
	 * once we do anything at all that touches the catalogs, an invalidation
	 * might leave cachelist pointing at garbage, so we must do this before we
	 * can do much else.
	 *
	 * 按命令标签过滤事件触发器列表，并复制到我们的内存上下文。
	 * 一旦开始运行命令触发器，或者做任何触及目录的事，失效可能让 cachelist 指向垃圾，
	 * 因此必须在做太多其他事情之前完成复制。
	 */
	foreach(lc, cachelist)
	{
		EventTriggerCacheItem *item = lfirst(lc);

		if (unfiltered || filter_event_trigger(tag, item))
		{
			/* We must plan to fire this trigger. */
			/*
			 *
			 * 必须计划触发这个触发器。
			 */
			runlist = lappend_oid(runlist, item->fnoid);
		}
	}

	/* Don't spend any more time on this if no functions to run */
	/*
	 *
	 * 若没有要运行的函数，就不要再花时间
	 */
	if (runlist == NIL)
		return NIL;

	trigdata->type = T_EventTriggerData;
	trigdata->event = eventstr;
	trigdata->parsetree = parsetree;
	trigdata->tag = tag;

	return runlist;
}

/*
 * Fire ddl_command_start triggers.
 *
 * 触发 ddl_command_start 触发器。
 */
void
EventTriggerDDLCommandStart(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * Event Triggers are completely disabled in standalone mode.  There are
	 * (at least) two reasons for this:
	 *
	 * 独立模式下事件触发器完全禁用。至少有两个原因：
	 *
	 * 1. A sufficiently broken event trigger might not only render the
	 * database unusable, but prevent disabling itself to fix the situation.
	 * In this scenario, restarting in standalone mode provides an escape
	 * hatch.
	 *
	 * 1. 损坏得足够严重的事件触发器不仅会让数据库无法使用，还会让人无法禁用它来修复。
	 * 这时以独立模式重启提供一条退路。
	 *
	 * 2. BuildEventTriggerCache relies on systable_beginscan_ordered, and
	 * therefore will malfunction if pg_event_trigger's indexes are damaged.
	 * To allow recovery from a damaged index, we need some operating mode
	 * wherein event triggers are disabled.  (Or we could implement
	 * heapscan-and-sort logic for that case, but having disaster recovery
	 * scenarios depend on code that's otherwise untested isn't appetizing.)
	 *
	 * 2. BuildEventTriggerCache 依赖 systable_beginscan_ordered，因此 pg_event_trigger 的索引损坏时会失常。
	 * 为了能从损坏的索引中恢复，需要一种禁用事件触发器的运行模式。
	 * （也可以为那种情况实现堆扫描加排序，但让灾难恢复依赖平时不测试的代码并不吸引人。）
	 *
	 * Additionally, event triggers can be disabled with a superuser-only GUC
	 * to make fixing database easier as per 1 above.
	 *
	 * 此外，可以用仅超级用户可设的 GUC 禁用事件触发器，以便按上面第 1 点更容易修复数据库。
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandStart,
									  "ddl_command_start",
									  &trigdata, false);
	if (runlist == NIL)
		return;

	/* Run the triggers. */
	/*
	 *
	 * 运行这些触发器。
	 */
	EventTriggerInvoke(runlist, &trigdata);

	/* Cleanup. */
	/*
	 *
	 * 清理。
	 */
	list_free(runlist);

	/*
	 * Make sure anything the event triggers did will be visible to the main
	 * command.
	 *
	 * 确保事件触发器所做的修改对主命令可见。
	 */
	CommandCounterIncrement();
}

/*
 * Fire ddl_command_end triggers.
 *
 * 触发 ddl_command_end 触发器。
 */
void
EventTriggerDDLCommandEnd(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via GUC.
	 *
	 * 关于为何在单用户模式或通过 GUC 禁用事件触发器，见 EventTriggerDDLCommandStart 的讨论。
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	/*
	 * Also do nothing if our state isn't set up, which it won't be if there
	 * weren't any relevant event triggers at the start of the current DDL
	 * command.  This test might therefore seem optional, but it's important
	 * because EventTriggerCommonSetup might find triggers that didn't exist
	 * at the time the command started.  Although this function itself
	 * wouldn't crash, the event trigger functions would presumably call
	 * pg_event_trigger_ddl_commands which would fail.  Better to do nothing
	 * until the next command.
	 *
	 * 若状态尚未建立，也什么都不做；当前 DDL 命令开始时若没有相关事件触发器，状态就不会建立。
	 * 这项测试看起来可选，但很重要，因为 EventTriggerCommonSetup 可能找到命令开始时还不存在的触发器。
	 * 本函数本身不会崩溃，但事件触发器函数大概会调用 pg_event_trigger_ddl_commands 并失败。
	 * 最好什么都不做，直到下一条命令。
	 */
	if (!currentEventTriggerState)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandEnd, "ddl_command_end",
									  &trigdata, false);
	if (runlist == NIL)
		return;

	/*
	 * Make sure anything the main command did will be visible to the event
	 * triggers.
	 *
	 * 确保主命令所做的修改对事件触发器可见。
	 */
	CommandCounterIncrement();

	/* Run the triggers. */
	/*
	 *
	 * 运行这些触发器。
	 */
	EventTriggerInvoke(runlist, &trigdata);

	/* Cleanup. */
	/*
	 *
	 * 清理。
	 */
	list_free(runlist);
}

/*
 * Fire sql_drop triggers.
 *
 * 触发 sql_drop 触发器。
 */
void
EventTriggerSQLDrop(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via a GUC.
	 *
	 * 关于为何在单用户模式或通过 GUC 禁用事件触发器，见 EventTriggerDDLCommandStart 的讨论。
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	/*
	 * Use current state to determine whether this event fires at all.  If
	 * there are no triggers for the sql_drop event, then we don't have
	 * anything to do here.  Note that dropped object collection is disabled
	 * if this is the case, so even if we were to try to run, the list would
	 * be empty.
	 *
	 * 用当前状态判断此事件是否触发。若没有 sql_drop 事件的触发器，这里就无事可做。
	 * 注意这种情况下不会收集被删除对象，因此即便尝试运行，列表也是空的。
	 */
	if (!currentEventTriggerState ||
		slist_is_empty(&currentEventTriggerState->SQLDropList))
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_SQLDrop, "sql_drop",
									  &trigdata, false);

	/*
	 * Nothing to do if run list is empty.  Note this typically can't happen,
	 * because if there are no sql_drop events, then objects-to-drop wouldn't
	 * have been collected in the first place and we would have quit above.
	 * But it could occur if event triggers were dropped partway through.
	 *
	 * 若运行列表为空则无事可做。通常不会发生，因为若没有 sql_drop 事件，
	 * 一开始就不会收集待删除对象，上面就已经退出。但事件触发器在中途被删除时可能发生。
	 */
	if (runlist == NIL)
		return;

	/*
	 * Make sure anything the main command did will be visible to the event
	 * triggers.
	 *
	 * 确保主命令所做的修改对事件触发器可见。
	 */
	CommandCounterIncrement();

	/*
	 * Make sure pg_event_trigger_dropped_objects only works when running
	 * these triggers.  Use PG_TRY to ensure in_sql_drop is reset even when
	 * one trigger fails.  (This is perhaps not necessary, as the currentState
	 * variable will be removed shortly by our caller, but it seems better to
	 * play safe.)
	 *
	 * 确保 pg_event_trigger_dropped_objects 只在运行这些触发器时可用。
	 * 用 PG_TRY 保证即使某个触发器失败也会重置 in_sql_drop。
	 * （也许不必如此，因为调用方很快会去掉 currentState，但稳妥一些更好。）
	 */
	currentEventTriggerState->in_sql_drop = true;

	/* Run the triggers. */
	/*
	 *
	 * 运行这些触发器。
	 */
	PG_TRY();
	{
		EventTriggerInvoke(runlist, &trigdata);
	}
	PG_FINALLY();
	{
		currentEventTriggerState->in_sql_drop = false;
	}
	PG_END_TRY();

	/* Cleanup. */
	/*
	 *
	 * 清理。
	 */
	list_free(runlist);
}

/*
 * Fire login event triggers if any are present.  The dathasloginevt
 * pg_database flag is left unchanged when an event trigger is dropped to avoid
 * complicating the codepath in the case of multiple event triggers.  This
 * function will instead unset the flag if no trigger is defined.
 *
 * 若存在登录事件触发器则触发它们。删除事件触发器时不改 dathasloginevt 标志，
 * 以免多个事件触发器时把代码路径搞复杂。若没有定义触发器，本函数会改为清除该标志。
 */
void
EventTriggerOnLogin(void)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via a GUC.  We also need a
	 * database connection (some background workers don't have it).
	 *
	 * 关于为何在单用户模式或通过 GUC 禁用事件触发器，见 EventTriggerDDLCommandStart 的讨论。
	 * 我们还需要一条数据库连接（有些后台工作进程没有）。
	 */
	if (!IsUnderPostmaster || !event_triggers ||
		!OidIsValid(MyDatabaseId) || !MyDatabaseHasLoginEventTriggers)
		return;

	StartTransactionCommand();
	runlist = EventTriggerCommonSetup(NULL,
									  EVT_Login, "login",
									  &trigdata, false);

	if (runlist != NIL)
	{
		/*
		 * Event trigger execution may require an active snapshot.
		 *
		 * 执行事件触发器可能需要一个活动快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/* Run the triggers. */
		/*
		 *
		 * 运行这些触发器。
		 */
		EventTriggerInvoke(runlist, &trigdata);

		/* Cleanup. */
		/*
		 *
		 * 清理。
		 */
		list_free(runlist);

		PopActiveSnapshot();
	}

	/*
	 * There is no active login event trigger, but our
	 * pg_database.dathasloginevt is set. Try to unset this flag.  We use the
	 * lock to prevent concurrent SetDatabaseHasLoginEventTriggers(), but we
	 * don't want to hang the connection waiting on the lock.  Thus, we are
	 * just trying to acquire the lock conditionally.
	 *
	 * 没有活动的登录事件触发器，但 pg_database.dathasloginevt 已设置。尝试清除该标志。
	 * 用锁防止与 SetDatabaseHasLoginEventTriggers() 并发，但不想让连接因等锁而挂起。因此只是有条件地尝试获取锁。
	 */
	else if (ConditionalLockSharedObject(DatabaseRelationId, MyDatabaseId,
										 0, AccessExclusiveLock))
	{
		/*
		 * The lock is held.  Now we need to recheck that login event triggers
		 * list is still empty.  Once the list is empty, we know that even if
		 * there is a backend which concurrently inserts/enables a login event
		 * trigger, it will update pg_database.dathasloginevt *afterwards*.
		 *
		 * 锁已持有。现在需要重新检查登录事件触发器列表是否仍为空。
		 * 一旦列表为空，即便有后端并发插入或启用登录事件触发器，它也会在之后才更新 pg_database.dathasloginevt。
		 */
		runlist = EventTriggerCommonSetup(NULL,
										  EVT_Login, "login",
										  &trigdata, true);

		if (runlist == NIL)
		{
			Relation	pg_db = table_open(DatabaseRelationId, RowExclusiveLock);
			HeapTuple	tuple;
			void	   *state;
			Form_pg_database db;
			ScanKeyData key[1];

			/* Fetch a copy of the tuple to scribble on */
			/*
			 *
			 * 取一份元组副本以便涂改
			 */
			ScanKeyInit(&key[0],
						Anum_pg_database_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(MyDatabaseId));

			systable_inplace_update_begin(pg_db, DatabaseOidIndexId, true,
										  NULL, 1, key, &tuple, &state);

			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "could not find tuple for database %u", MyDatabaseId);

			db = (Form_pg_database) GETSTRUCT(tuple);
			if (db->dathasloginevt)
			{
				db->dathasloginevt = false;

				/*
				 * Do an "in place" update of the pg_database tuple.  Doing
				 * this instead of regular updates serves two purposes. First,
				 * that avoids possible waiting on the row-level lock. Second,
				 * that avoids dealing with TOAST.
				 *
				 * 对 pg_database 元组做原地更新。这样做而不是普通更新有两个目的：
				 * 第一，避免可能等待行级锁；第二，避免处理 TOAST。
				 */
				systable_inplace_update_finish(state, tuple);
			}
			else
				systable_inplace_update_cancel(state);
			table_close(pg_db, RowExclusiveLock);
			heap_freetuple(tuple);
		}
		else
		{
			list_free(runlist);
		}
	}
	CommitTransactionCommand();
}


/*
 * Fire table_rewrite triggers.
 *
 * 触发表 table_rewrite 触发器。
 */
void
EventTriggerTableRewrite(Node *parsetree, Oid tableOid, int reason)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * See EventTriggerDDLCommandStart for a discussion about why event
	 * triggers are disabled in single user mode or via a GUC.
	 *
	 * 关于为何在单用户模式或通过 GUC 禁用事件触发器，见 EventTriggerDDLCommandStart 的讨论。
	 */
	if (!IsUnderPostmaster || !event_triggers)
		return;

	/*
	 * Also do nothing if our state isn't set up, which it won't be if there
	 * weren't any relevant event triggers at the start of the current DDL
	 * command.  This test might therefore seem optional, but it's
	 * *necessary*, because EventTriggerCommonSetup might find triggers that
	 * didn't exist at the time the command started.
	 *
	 * 若状态尚未建立，也什么都不做；当前 DDL 命令开始时若没有相关事件触发器，状态就不会建立。
	 * 这项测试看起来可选，但有必要，因为 EventTriggerCommonSetup 可能找到命令开始时还不存在的触发器。
	 */
	if (!currentEventTriggerState)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_TableRewrite,
									  "table_rewrite",
									  &trigdata, false);
	if (runlist == NIL)
		return;

	/*
	 * Make sure pg_event_trigger_table_rewrite_oid only works when running
	 * these triggers. Use PG_TRY to ensure table_rewrite_oid is reset even
	 * when one trigger fails. (This is perhaps not necessary, as the
	 * currentState variable will be removed shortly by our caller, but it
	 * seems better to play safe.)
	 *
	 * 确保 pg_event_trigger_table_rewrite_oid 只在运行这些触发器时可用。
	 * 用 PG_TRY 保证即使某个触发器失败也会重置 table_rewrite_oid。
	 * （也许不必如此，因为调用方很快会去掉 currentState，但稳妥一些更好。）
	 */
	currentEventTriggerState->table_rewrite_oid = tableOid;
	currentEventTriggerState->table_rewrite_reason = reason;

	/* Run the triggers. */
	/*
	 *
	 * 运行这些触发器。
	 */
	PG_TRY();
	{
		EventTriggerInvoke(runlist, &trigdata);
	}
	PG_FINALLY();
	{
		currentEventTriggerState->table_rewrite_oid = InvalidOid;
		currentEventTriggerState->table_rewrite_reason = 0;
	}
	PG_END_TRY();

	/* Cleanup. */
	/*
	 *
	 * 清理。
	 */
	list_free(runlist);

	/*
	 * Make sure anything the event triggers did will be visible to the main
	 * command.
	 *
	 * 确保事件触发器所做的修改对主命令可见。
	 */
	CommandCounterIncrement();
}

/*
 * Invoke each event trigger in a list of event triggers.
 *
 * 依次调用事件触发器列表中的每一个。
 */
static void
EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata)
{
	MemoryContext context;
	MemoryContext oldcontext;
	ListCell   *lc;
	bool		first = true;

	/* Guard against stack overflow due to recursive event trigger */
	/*
	 *
	 * 防止递归事件触发器导致栈溢出
	 */
	check_stack_depth();

	/*
	 * Let's evaluate event triggers in their own memory context, so that any
	 * leaks get cleaned up promptly.
	 *
	 * 在事件触发器自己的内存上下文中执行它们，以便泄漏能被及时清理。
	 */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"event trigger context",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);

	/* Call each event trigger. */
	/*
	 *
	 * 调用每一个事件触发器。
	 */
	foreach(lc, fn_oid_list)
	{
		LOCAL_FCINFO(fcinfo, 0);
		Oid			fnoid = lfirst_oid(lc);
		FmgrInfo	flinfo;
		PgStat_FunctionCallUsage fcusage;

		elog(DEBUG1, "EventTriggerInvoke %u", fnoid);

		/*
		 * We want each event trigger to be able to see the results of the
		 * previous event trigger's action.  Caller is responsible for any
		 * command-counter increment that is needed between the event trigger
		 * and anything else in the transaction.
		 *
		 * 我们希望每个事件触发器都能看到前一个事件触发器动作的结果。
		 * 事件触发器与事务中其他操作之间所需的命令计数器递增，由调用方负责。
		 */
		if (first)
			first = false;
		else
			CommandCounterIncrement();

		/* Look up the function */
		/*
		 *
		 * 查找该函数
		 */
		fmgr_info(fnoid, &flinfo);

		/* Call the function, passing no arguments but setting a context. */
		/*
		 *
		 * 调用该函数：不传参数，但设置一个上下文。
		 */
		InitFunctionCallInfoData(*fcinfo, &flinfo, 0,
								 InvalidOid, (Node *) trigdata, NULL);
		pgstat_init_function_usage(fcinfo, &fcusage);
		FunctionCallInvoke(fcinfo);
		pgstat_end_function_usage(&fcusage, true);

		/* Reclaim memory. */
		/*
		 *
		 * 回收内存。
		 */
		MemoryContextReset(context);
	}

	/* Restore old memory context and delete the temporary one. */
	/*
	 *
	 * 恢复旧的内存上下文并删除临时上下文。
	 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
}

/*
 * Do event triggers support this object type?
 *
 * 事件触发器是否支持这种对象类型？
 *
 * See also event trigger documentation in event-trigger.sgml.
 *
 * 另见 event-trigger.sgml 中的事件触发器文档。
 */
bool
EventTriggerSupportsObjectType(ObjectType obtype)
{
	switch (obtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_TABLESPACE:
		case OBJECT_ROLE:
		case OBJECT_PARAMETER_ACL:
			/* no support for global objects (except subscriptions) */
			/*
			 *
			 * 不支持全局对象（订阅除外）
			 */
			return false;
		case OBJECT_EVENT_TRIGGER:
			/* no support for event triggers on event triggers */
			/*
			 *
			 * 不支持在事件触发器上再挂事件触发器
			 */
			return false;
		default:
			return true;
	}
}

/*
 * Do event triggers support this object class?
 *
 * 事件触发器是否支持这种对象类？
 *
 * See also event trigger documentation in event-trigger.sgml.
 *
 * 另见 event-trigger.sgml 中的事件触发器文档。
 */
bool
EventTriggerSupportsObject(const ObjectAddress *object)
{
	switch (object->classId)
	{
		case DatabaseRelationId:
		case TableSpaceRelationId:
		case AuthIdRelationId:
		case AuthMemRelationId:
		case ParameterAclRelationId:
			/* no support for global objects (except subscriptions) */
			/*
			 *
			 * 不支持全局对象（订阅除外）
			 */
			return false;
		case EventTriggerRelationId:
			/* no support for event triggers on event triggers */
			/*
			 *
			 * 不支持在事件触发器上再挂事件触发器
			 */
			return false;
		default:
			return true;
	}
}

/*
 * Prepare event trigger state for a new complete query to run, if necessary;
 * returns whether this was done.  If it was, EventTriggerEndCompleteQuery must
 * be called when the query is done, regardless of whether it succeeds or fails
 * -- so use of a PG_TRY block is mandatory.
 *
 * 若有必要，为即将运行的一条完整查询准备事件触发器状态；返回是否做了准备。
 * 若做了，无论查询成功还是失败，结束时都必须调用 EventTriggerEndCompleteQuery，因此必须使用 PG_TRY 块。
 */
bool
EventTriggerBeginCompleteQuery(void)
{
	EventTriggerQueryState *state;
	MemoryContext cxt;

	/*
	 * Currently, sql_drop, table_rewrite, ddl_command_end events are the only
	 * reason to have event trigger state at all; so if there are none, don't
	 * install one.
	 *
	 * 目前，只有 sql_drop、table_rewrite、ddl_command_end 事件才需要事件触发器状态；若都没有，就不要安装。
	 */
	if (!trackDroppedObjectsNeeded())
		return false;

	cxt = AllocSetContextCreate(TopMemoryContext,
								"event trigger state",
								ALLOCSET_DEFAULT_SIZES);
	state = MemoryContextAlloc(cxt, sizeof(EventTriggerQueryState));
	state->cxt = cxt;
	slist_init(&(state->SQLDropList));
	state->in_sql_drop = false;
	state->table_rewrite_oid = InvalidOid;

	state->commandCollectionInhibited = currentEventTriggerState ?
		currentEventTriggerState->commandCollectionInhibited : false;
	state->currentCommand = NULL;
	state->commandList = NIL;
	state->previous = currentEventTriggerState;
	currentEventTriggerState = state;

	return true;
}

/*
 * Query completed (or errored out) -- clean up local state, return to previous
 * one.
 *
 * 查询已完成（或出错）：清理本地状态，回到前一个状态。
 *
 * Note: it's an error to call this routine if EventTriggerBeginCompleteQuery
 * returned false previously.
 *
 * 注意：若此前 EventTriggerBeginCompleteQuery 返回了 false，再调用本例程就是错误。
 *
 * Note: this might be called in the PG_CATCH block of a failing transaction,
 * so be wary of running anything unnecessary.  (In particular, it's probably
 * unwise to try to allocate memory.)
 *
 * 注意：这可能在失败事务的 PG_CATCH 块中调用，因此不要运行任何不必要的东西。
 * （尤其是，试图分配内存大概是不明智的。）
 */
void
EventTriggerEndCompleteQuery(void)
{
	EventTriggerQueryState *prevstate;

	prevstate = currentEventTriggerState->previous;

	/* this avoids the need for retail pfree of SQLDropList items: */
	/*
	 *
	 * 这样就不需要逐个 pfree SQLDropList 中的项：
	 */
	MemoryContextDelete(currentEventTriggerState->cxt);

	currentEventTriggerState = prevstate;
}

/*
 * Do we need to keep close track of objects being dropped?
 *
 * 是否需要密切跟踪正在被删除的对象？
 *
 * This is useful because there is a cost to running with them enabled.
 *
 * 这是有用的，因为启用它们是有代价的。
 */
bool
trackDroppedObjectsNeeded(void)
{
	/*
	 * true if any sql_drop, table_rewrite, ddl_command_end event trigger
	 * exists
	 *
	 * 若存在任何 sql_drop、table_rewrite、ddl_command_end 事件触发器，则为真
	 */
	return (EventCacheLookup(EVT_SQLDrop) != NIL) ||
		(EventCacheLookup(EVT_TableRewrite) != NIL) ||
		(EventCacheLookup(EVT_DDLCommandEnd) != NIL);
}

/*
 * Support for dropped objects information on event trigger functions.
 *
 * 事件触发器函数上被删除对象信息的支持。
 *
 * We keep the list of objects dropped by the current command in current
 * state's SQLDropList (comprising SQLDropObject items).  Each time a new
 * command is to start, a clean EventTriggerQueryState is created; commands
 * that drop objects do the dependency.c dance to drop objects, which
 * populates the current state's SQLDropList; when the event triggers are
 * invoked they can consume the list via pg_event_trigger_dropped_objects().
 * When the command finishes, the EventTriggerQueryState is cleared, and
 * the one from the previous command is restored (when no command is in
 * execution, the current state is NULL).
 *
 * 当前命令删除的对象列表保存在当前状态的 SQLDropList 中（由 SQLDropObject 项组成）。
 * 每次新命令开始时，会创建一个干净的 EventTriggerQueryState；
 * 删除对象的命令通过 dependency.c 删除对象，从而填充当前状态的 SQLDropList；
 * 事件触发器被调用时可通过 pg_event_trigger_dropped_objects() 消费该列表。
 * 命令结束时清除 EventTriggerQueryState，并恢复上一条命令的状态（没有命令在执行时，当前状态为 NULL）。
 *
 * All this lets us support the case that an event trigger function drops
 * objects "reentrantly".
 *
 * 这一切使我们能支持事件触发器函数“重入地”删除对象的情况。
 */

/*
 * Register one object as being dropped by the current command.
 *
 * 把一个对象登记为被当前命令删除。
 */
void
EventTriggerSQLDropAddObject(const ObjectAddress *object, bool original, bool normal)
{
	SQLDropObject *obj;
	MemoryContext oldcxt;

	if (!currentEventTriggerState)
		return;

	Assert(EventTriggerSupportsObject(object));

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	obj = palloc0(sizeof(SQLDropObject));
	obj->address = *object;
	obj->original = original;
	obj->normal = normal;

	if (object->classId == NamespaceRelationId)
	{
		/* Special handling is needed for temp namespaces */
		/*
		 *
		 * 临时命名空间需要特殊处理
		 */
		if (isTempNamespace(object->objectId))
			obj->istemp = true;
		else if (isAnyTempNamespace(object->objectId))
		{
			/* don't report temp schemas except my own */
			/*
			 *
			 * 除了自己的临时模式，不报告其他临时模式
			 */
			pfree(obj);
			MemoryContextSwitchTo(oldcxt);
			return;
		}
		obj->objname = get_namespace_name(object->objectId);
	}
	else if (object->classId == AttrDefaultRelationId)
	{
		/* We treat a column default as temp if its table is temp */
		/*
		 *
		 * 若列默认值所属的表是临时的，则把该默认值也视为临时的
		 */
		ObjectAddress colobject;

		colobject = GetAttrDefaultColumnAddress(object->objectId);
		if (OidIsValid(colobject.objectId))
		{
			if (!obtain_object_name_namespace(&colobject, obj))
			{
				pfree(obj);
				MemoryContextSwitchTo(oldcxt);
				return;
			}
		}
	}
	else if (object->classId == TriggerRelationId)
	{
		/* Similarly, a trigger is temp if its table is temp */
		/*
		 *
		 * 同样，若触发器所属的表是临时的，则该触发器也是临时的
		 */
		/* Sadly, there's no lsyscache.c support for trigger objects */
		/*
		 *
		 * 可惜 lsyscache.c 不支持触发器对象
		 */
		Relation	pg_trigger_rel;
		ScanKeyData skey[1];
		SysScanDesc sscan;
		HeapTuple	tuple;
		Oid			relid;

		/* Fetch the trigger's table OID the hard way */
		/*
		 *
		 * 用较笨的办法取得触发器所属表的 OID
		 */
		pg_trigger_rel = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&skey[0],
					Anum_pg_trigger_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(object->objectId));
		sscan = systable_beginscan(pg_trigger_rel, TriggerOidIndexId, true,
								   NULL, 1, skey);
		tuple = systable_getnext(sscan);
		if (HeapTupleIsValid(tuple))
			relid = ((Form_pg_trigger) GETSTRUCT(tuple))->tgrelid;
		else
			relid = InvalidOid; /* shouldn't happen */
			/*
			 *
			 * 不应该发生
			 */
		systable_endscan(sscan);
		table_close(pg_trigger_rel, AccessShareLock);
		/* Do nothing if we didn't find the trigger */
		/*
		 *
		 * 若没有找到该触发器，则什么都不做
		 */
		if (OidIsValid(relid))
		{
			ObjectAddress relobject;

			relobject.classId = RelationRelationId;
			relobject.objectId = relid;
			/* Arbitrarily set objectSubId nonzero so as not to fill objname */
			/*
			 *
			 * 任意把 objectSubId 设为非零，以免填充 objname
			 */
			relobject.objectSubId = 1;
			if (!obtain_object_name_namespace(&relobject, obj))
			{
				pfree(obj);
				MemoryContextSwitchTo(oldcxt);
				return;
			}
		}
	}
	else if (object->classId == PolicyRelationId)
	{
		/* Similarly, a policy is temp if its table is temp */
		/*
		 *
		 * 同样，若策略所属的表是临时的，则该策略也是临时的
		 */
		/* Sadly, there's no lsyscache.c support for policy objects */
		/*
		 *
		 * 可惜 lsyscache.c 不支持策略对象
		 */
		Relation	pg_policy_rel;
		ScanKeyData skey[1];
		SysScanDesc sscan;
		HeapTuple	tuple;
		Oid			relid;

		/* Fetch the policy's table OID the hard way */
		/*
		 *
		 * 用较笨的办法取得策略所属表的 OID
		 */
		pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);
		ScanKeyInit(&skey[0],
					Anum_pg_policy_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(object->objectId));
		sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
								   NULL, 1, skey);
		tuple = systable_getnext(sscan);
		if (HeapTupleIsValid(tuple))
			relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;
		else
			relid = InvalidOid; /* shouldn't happen */
			/*
			 *
			 * 不应该发生
			 */
		systable_endscan(sscan);
		table_close(pg_policy_rel, AccessShareLock);
		/* Do nothing if we didn't find the policy */
		/*
		 *
		 * 若没有找到该策略，则什么都不做
		 */
		if (OidIsValid(relid))
		{
			ObjectAddress relobject;

			relobject.classId = RelationRelationId;
			relobject.objectId = relid;
			/* Arbitrarily set objectSubId nonzero so as not to fill objname */
			/*
			 *
			 * 任意把 objectSubId 设为非零，以免填充 objname
			 */
			relobject.objectSubId = 1;
			if (!obtain_object_name_namespace(&relobject, obj))
			{
				pfree(obj);
				MemoryContextSwitchTo(oldcxt);
				return;
			}
		}
	}
	else
	{
		/* Generic handling for all other object classes */
		/*
		 *
		 * 对所有其他对象类的一般处理
		 */
		if (!obtain_object_name_namespace(object, obj))
		{
			/* don't report temp objects except my own */
			/*
			 *
			 * 除了自己的临时对象，不报告其他临时对象
			 */
			pfree(obj);
			MemoryContextSwitchTo(oldcxt);
			return;
		}
	}

	/* object identity, objname and objargs */
	/*
	 *
	 * 对象标识、objname 与 objargs
	 */
	obj->objidentity =
		getObjectIdentityParts(&obj->address, &obj->addrnames, &obj->addrargs,
							   false);

	/* object type */
	/*
	 *
	 * 对象类型
	 */
	obj->objecttype = getObjectTypeDescription(&obj->address, false);

	slist_push_head(&(currentEventTriggerState->SQLDropList), &obj->next);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Fill obj->objname, obj->schemaname, and obj->istemp based on object.
 *
 * 根据对象填充 obj->objname、obj->schemaname 和 obj->istemp。
 *
 * Returns true if this object should be reported, false if it should
 * be ignored because it is a temporary object of another session.
 *
 * 若应报告此对象则返回 true；若它是其他会话的临时对象、应被忽略，则返回 false。
 */
static bool
obtain_object_name_namespace(const ObjectAddress *object, SQLDropObject *obj)
{
	/*
	 * Obtain schema names from the object's catalog tuple, if one exists;
	 * this lets us skip objects in temp schemas.  We trust that
	 * ObjectProperty contains all object classes that can be
	 * schema-qualified.
	 *
	 * 若对象有目录元组，则从中取得模式名；这样可以跳过临时模式中的对象。
	 * 我们相信 ObjectProperty 包含了所有可以按模式限定的对象类。
	 *
	 * Currently, this function does nothing for object classes that are not
	 * in ObjectProperty, but we might sometime add special cases for that.
	 *
	 * 目前，对不在 ObjectProperty 中的对象类，此函数什么都不做，但将来也许会为那种情况增加特例。
	 */
	if (is_objectclass_supported(object->classId))
	{
		Relation	catalog;
		HeapTuple	tuple;

		catalog = table_open(object->classId, AccessShareLock);
		tuple = get_catalog_object_by_oid(catalog,
										  get_object_attnum_oid(object->classId),
										  object->objectId);

		if (tuple)
		{
			AttrNumber	attnum;
			Datum		datum;
			bool		isnull;

			attnum = get_object_attnum_namespace(object->classId);
			if (attnum != InvalidAttrNumber)
			{
				datum = heap_getattr(tuple, attnum,
									 RelationGetDescr(catalog), &isnull);
				if (!isnull)
				{
					Oid			namespaceId;

					namespaceId = DatumGetObjectId(datum);
					/* temp objects are only reported if they are my own */
					/*
					 *
					 * 只有属于自己的临时对象才会被报告
					 */
					if (isTempNamespace(namespaceId))
					{
						obj->schemaname = "pg_temp";
						obj->istemp = true;
					}
					else if (isAnyTempNamespace(namespaceId))
					{
						/* no need to fill any fields of *obj */
						/*
						 *
						 * 不必填充 *obj 的任何字段
						 */
						table_close(catalog, AccessShareLock);
						return false;
					}
					else
					{
						obj->schemaname = get_namespace_name(namespaceId);
						obj->istemp = false;
					}
				}
			}

			if (get_object_namensp_unique(object->classId) &&
				object->objectSubId == 0)
			{
				attnum = get_object_attnum_name(object->classId);
				if (attnum != InvalidAttrNumber)
				{
					datum = heap_getattr(tuple, attnum,
										 RelationGetDescr(catalog), &isnull);
					if (!isnull)
						obj->objname = pstrdup(NameStr(*DatumGetName(datum)));
				}
			}
		}

		table_close(catalog, AccessShareLock);
	}

	return true;
}

/*
 * pg_event_trigger_dropped_objects
 *
 * 函数 pg_event_trigger_dropped_objects。
 *
 * Make the list of dropped objects available to the user function run by the
 * Event Trigger.
 *
 * 把被删除对象的列表提供给事件触发器所运行的用户函数。
 */
Datum
pg_event_trigger_dropped_objects(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	slist_iter	iter;

	/*
	 * Protect this function from being called out of context
	 *
	 * 防止此函数在错误的上下文中被调用
	 */
	if (!currentEventTriggerState ||
		!currentEventTriggerState->in_sql_drop)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a sql_drop event trigger function",
						"pg_event_trigger_dropped_objects()")));

	/* Build tuplestore to hold the result rows */
	/*
	 *
	 * 建立用于保存结果行的 tuplestore
	 */
	InitMaterializedSRF(fcinfo, 0);

	slist_foreach(iter, &(currentEventTriggerState->SQLDropList))
	{
		SQLDropObject *obj;
		int			i = 0;
		Datum		values[12] = {0};
		bool		nulls[12] = {0};

		obj = slist_container(SQLDropObject, next, iter.cur);

		/* classid */
		/*
		 *
		 * classid
		 */
		values[i++] = ObjectIdGetDatum(obj->address.classId);

		/* objid */
		/*
		 *
		 * objid
		 */
		values[i++] = ObjectIdGetDatum(obj->address.objectId);

		/* objsubid */
		/*
		 *
		 * objsubid
		 */
		values[i++] = Int32GetDatum(obj->address.objectSubId);

		/* original */
		/*
		 *
		 * original
		 */
		values[i++] = BoolGetDatum(obj->original);

		/* normal */
		/*
		 *
		 * normal
		 */
		values[i++] = BoolGetDatum(obj->normal);

		/* is_temporary */
		/*
		 *
		 * is_temporary
		 */
		values[i++] = BoolGetDatum(obj->istemp);

		/* object_type */
		/*
		 *
		 * object_type
		 */
		values[i++] = CStringGetTextDatum(obj->objecttype);

		/* schema_name */
		/*
		 *
		 * schema_name
		 */
		if (obj->schemaname)
			values[i++] = CStringGetTextDatum(obj->schemaname);
		else
			nulls[i++] = true;

		/* object_name */
		/*
		 *
		 * object_name
		 */
		if (obj->objname)
			values[i++] = CStringGetTextDatum(obj->objname);
		else
			nulls[i++] = true;

		/* object_identity */
		/*
		 *
		 * object_identity
		 */
		if (obj->objidentity)
			values[i++] = CStringGetTextDatum(obj->objidentity);
		else
			nulls[i++] = true;

		/* address_names and address_args */
		/*
		 *
		 * address_names 与 address_args
		 */
		if (obj->addrnames)
		{
			values[i++] = PointerGetDatum(strlist_to_textarray(obj->addrnames));

			if (obj->addrargs)
				values[i++] = PointerGetDatum(strlist_to_textarray(obj->addrargs));
			else
				values[i++] = PointerGetDatum(construct_empty_array(TEXTOID));
		}
		else
		{
			nulls[i++] = true;
			nulls[i++] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * pg_event_trigger_table_rewrite_oid
 *
 * 函数 pg_event_trigger_table_rewrite_oid。
 *
 * Make the Oid of the table going to be rewritten available to the user
 * function run by the Event Trigger.
 *
 * 把即将被重写的表的 OID 提供给事件触发器所运行的用户函数。
 */
Datum
pg_event_trigger_table_rewrite_oid(PG_FUNCTION_ARGS)
{
	/*
	 * Protect this function from being called out of context
	 *
	 * 防止此函数在错误的上下文中被调用
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->table_rewrite_oid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a table_rewrite event trigger function",
						"pg_event_trigger_table_rewrite_oid()")));

	PG_RETURN_OID(currentEventTriggerState->table_rewrite_oid);
}

/*
 * pg_event_trigger_table_rewrite_reason
 *
 * 函数 pg_event_trigger_table_rewrite_reason。
 *
 * Make the rewrite reason available to the user.
 *
 * 把重写原因提供给用户。
 */
Datum
pg_event_trigger_table_rewrite_reason(PG_FUNCTION_ARGS)
{
	/*
	 * Protect this function from being called out of context
	 *
	 * 防止此函数在错误的上下文中被调用
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->table_rewrite_reason == 0)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a table_rewrite event trigger function",
						"pg_event_trigger_table_rewrite_reason()")));

	PG_RETURN_INT32(currentEventTriggerState->table_rewrite_reason);
}

/*-------------------------------------------------------------------------
 * Support for DDL command deparsing
 *
 * DDL 命令反解析支持
 *
 * The routines below enable an event trigger function to obtain a list of
 * DDL commands as they are executed.  There are three main pieces to this
 * feature:
 *
 * 下面的例程让事件触发器函数能取得正在执行的 DDL 命令列表。这个特性主要有三部分：
 *
 * 1) Within ProcessUtilitySlow, or some sub-routine thereof, each DDL command
 * adds a struct CollectedCommand representation of itself to the command list,
 * using the routines below.
 *
 * 1) 在 ProcessUtilitySlow 或其某个子程序中，每条 DDL 命令用下面的例程
 * 把自身的 CollectedCommand 结构表示加入命令列表。
 *
 * 2) Some time after that, ddl_command_end fires and the command list is made
 * available to the event trigger function via pg_event_trigger_ddl_commands();
 * the complete command details are exposed as a column of type pg_ddl_command.
 *
 * 2) 此后一段时间，ddl_command_end 触发，命令列表通过 pg_event_trigger_ddl_commands() 提供给事件触发器函数；
 * 完整的命令细节作为类型 pg_ddl_command 的一列暴露出来。
 *
 * 3) An extension can install a function capable of taking a value of type
 * pg_ddl_command and transform it into some external, user-visible and/or
 * -modifiable representation.
 *
 * 3) 扩展可以安装一个函数，接收 pg_ddl_command 类型的值，并把它转换成某种外部的、用户可见和/或可修改的表示。
 *-------------------------------------------------------------------------
 */

/*
 * Inhibit DDL command collection.
 *
 * 禁止收集 DDL 命令。
 */
void
EventTriggerInhibitCommandCollection(void)
{
	if (!currentEventTriggerState)
		return;

	currentEventTriggerState->commandCollectionInhibited = true;
}

/*
 * Re-establish DDL command collection.
 *
 * 重新启用 DDL 命令收集。
 */
void
EventTriggerUndoInhibitCommandCollection(void)
{
	if (!currentEventTriggerState)
		return;

	currentEventTriggerState->commandCollectionInhibited = false;
}

/*
 * EventTriggerCollectSimpleCommand
 *		Save data about a simple DDL command that was just executed
 *
 * EventTriggerCollectSimpleCommand：保存刚刚执行的一条简单 DDL 命令的数据
 *
 * address identifies the object being operated on.  secondaryObject is an
 * object address that was related in some way to the executed command; its
 * meaning is command-specific.
 *
 * address 标识正在操作的对象。secondaryObject 是与所执行命令以某种方式相关的对象地址；其含义因命令而异。
 *
 * For instance, for an ALTER obj SET SCHEMA command, objtype is the type of
 * object being moved, objectId is its OID, and secondaryOid is the OID of the
 * old schema.  (The destination schema OID can be obtained by catalog lookup
 * of the object.)
 *
 * 例如，对于 ALTER obj SET SCHEMA 命令，objtype 是被移动对象的类型，objectId 是它的 OID，
 * secondaryOid 是旧模式的 OID。（目标模式的 OID 可通过查找该对象的目录得到。）
 */
void
EventTriggerCollectSimpleCommand(ObjectAddress address,
								 ObjectAddress secondaryObject,
								 Node *parsetree)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));

	command->type = SCT_Simple;
	command->in_extension = creating_extension;

	command->d.simple.address = address;
	command->d.simple.secondaryObject = secondaryObject;
	command->parsetree = copyObject(parsetree);

	currentEventTriggerState->commandList = lappend(currentEventTriggerState->commandList,
													command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerAlterTableStart
 *		Prepare to receive data on an ALTER TABLE command about to be executed
 *
 * EventTriggerAlterTableStart：准备接收即将执行的 ALTER TABLE 命令的数据
 *
 * Note we don't collect the command immediately; instead we keep it in
 * currentCommand, and only when we're done processing the subcommands we will
 * add it to the command list.
 *
 * 注意我们不立刻收集该命令；而是把它留在 currentCommand 中，处理完子命令后才加入命令列表。
 */
void
EventTriggerAlterTableStart(Node *parsetree)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));

	command->type = SCT_AlterTable;
	command->in_extension = creating_extension;

	command->d.alterTable.classId = RelationRelationId;
	command->d.alterTable.objectId = InvalidOid;
	command->d.alterTable.subcmds = NIL;
	command->parsetree = copyObject(parsetree);

	command->parent = currentEventTriggerState->currentCommand;
	currentEventTriggerState->currentCommand = command;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Remember the OID of the object being affected by an ALTER TABLE.
 *
 * 记住受 ALTER TABLE 影响的对象的 OID。
 *
 * This is needed because in some cases we don't know the OID until later.
 *
 * 这是需要的，因为有些情况下要到稍后才知道 OID。
 */
void
EventTriggerAlterTableRelid(Oid objectId)
{
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	currentEventTriggerState->currentCommand->d.alterTable.objectId = objectId;
}

/*
 * EventTriggerCollectAlterTableSubcmd
 *		Save data about a single part of an ALTER TABLE.
 *
 * EventTriggerCollectAlterTableSubcmd：保存 ALTER TABLE 中单独一部分的数据。
 *
 * Several different commands go through this path, but apart from ALTER TABLE
 * itself, they are all concerned with AlterTableCmd nodes that are generated
 * internally, so that's all that this code needs to handle at the moment.
 *
 * 有几条不同的命令走这条路径，但除 ALTER TABLE 本身外，它们都与内部生成的 AlterTableCmd 节点有关，
 * 因此目前这段代码只需要处理这些。
 */
void
EventTriggerCollectAlterTableSubcmd(Node *subcmd, ObjectAddress address)
{
	MemoryContext oldcxt;
	CollectedATSubcmd *newsub;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	Assert(IsA(subcmd, AlterTableCmd));
	Assert(currentEventTriggerState->currentCommand != NULL);
	Assert(OidIsValid(currentEventTriggerState->currentCommand->d.alterTable.objectId));

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	newsub = palloc(sizeof(CollectedATSubcmd));
	newsub->address = address;
	newsub->parsetree = copyObject(subcmd);

	currentEventTriggerState->currentCommand->d.alterTable.subcmds =
		lappend(currentEventTriggerState->currentCommand->d.alterTable.subcmds, newsub);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerAlterTableEnd
 *		Finish up saving an ALTER TABLE command, and add it to command list.
 *
 * EventTriggerAlterTableEnd：结束对一条 ALTER TABLE 命令的保存，并把它加入命令列表。
 *
 * FIXME this API isn't considering the possibility that an xact/subxact is
 * aborted partway through.  Probably it's best to add an
 * AtEOSubXact_EventTriggers() to fix this.
 *
 * FIXME：这个 API 没有考虑事务或子事务中途中止的可能。
 * 也许最好增加 AtEOSubXact_EventTriggers() 来修复。
 */
void
EventTriggerAlterTableEnd(void)
{
	CollectedCommand *parent;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	parent = currentEventTriggerState->currentCommand->parent;

	/* If no subcommands, don't collect */
	/*
	 *
	 * 若没有子命令，则不收集
	 */
	if (currentEventTriggerState->currentCommand->d.alterTable.subcmds != NIL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

		currentEventTriggerState->commandList =
			lappend(currentEventTriggerState->commandList,
					currentEventTriggerState->currentCommand);

		MemoryContextSwitchTo(oldcxt);
	}
	else
		pfree(currentEventTriggerState->currentCommand);

	currentEventTriggerState->currentCommand = parent;
}

/*
 * EventTriggerCollectGrant
 *		Save data about a GRANT/REVOKE command being executed
 *
 * EventTriggerCollectGrant：保存正在执行的 GRANT/REVOKE 命令的数据
 *
 * This function creates a copy of the InternalGrant, as the original might
 * not have the right lifetime.
 *
 * 此函数复制一份 InternalGrant，因为原来的那份寿命可能不对。
 */
void
EventTriggerCollectGrant(InternalGrant *istmt)
{
	MemoryContext oldcxt;
	CollectedCommand *command;
	InternalGrant *icopy;
	ListCell   *cell;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	/*
	 * This is tedious, but necessary.
	 *
	 * 这很繁琐，但是必要的。
	 */
	icopy = palloc(sizeof(InternalGrant));
	memcpy(icopy, istmt, sizeof(InternalGrant));
	icopy->objects = list_copy(istmt->objects);
	icopy->grantees = list_copy(istmt->grantees);
	icopy->col_privs = NIL;
	foreach(cell, istmt->col_privs)
		icopy->col_privs = lappend(icopy->col_privs, copyObject(lfirst(cell)));

	/* Now collect it, using the copied InternalGrant */
	/*
	 *
	 * 现在用复制后的 InternalGrant 收集它
	 */
	command = palloc(sizeof(CollectedCommand));
	command->type = SCT_Grant;
	command->in_extension = creating_extension;
	command->d.grant.istmt = icopy;
	command->parsetree = NULL;

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterOpFam
 *		Save data about an ALTER OPERATOR FAMILY ADD/DROP command being
 *		executed
 *
 * EventTriggerCollectAlterOpFam：保存正在执行的 ALTER OPERATOR FAMILY ADD/DROP 命令的数据
 */
void
EventTriggerCollectAlterOpFam(AlterOpFamilyStmt *stmt, Oid opfamoid,
							  List *operators, List *procedures)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));
	command->type = SCT_AlterOpFamily;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.opfam.address,
					 OperatorFamilyRelationId, opfamoid);
	command->d.opfam.operators = operators;
	command->d.opfam.procedures = procedures;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectCreateOpClass
 *		Save data about a CREATE OPERATOR CLASS command being executed
 *
 * EventTriggerCollectCreateOpClass：保存正在执行的 CREATE OPERATOR CLASS 命令的数据
 */
void
EventTriggerCollectCreateOpClass(CreateOpClassStmt *stmt, Oid opcoid,
								 List *operators, List *procedures)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_CreateOpClass;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.createopc.address,
					 OperatorClassRelationId, opcoid);
	command->d.createopc.operators = operators;
	command->d.createopc.procedures = procedures;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterTSConfig
 *		Save data about an ALTER TEXT SEARCH CONFIGURATION command being
 *		executed
 *
 * EventTriggerCollectAlterTSConfig：保存正在执行的 ALTER TEXT SEARCH CONFIGURATION 命令的数据
 */
void
EventTriggerCollectAlterTSConfig(AlterTSConfigurationStmt *stmt, Oid cfgId,
								 Oid *dictIds, int ndicts)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_AlterTSConfig;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.atscfg.address,
					 TSConfigRelationId, cfgId);
	command->d.atscfg.dictIds = palloc(sizeof(Oid) * ndicts);
	memcpy(command->d.atscfg.dictIds, dictIds, sizeof(Oid) * ndicts);
	command->d.atscfg.ndicts = ndicts;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterDefPrivs
 *		Save data about an ALTER DEFAULT PRIVILEGES command being
 *		executed
 *
 * EventTriggerCollectAlterDefPrivs：保存正在执行的 ALTER DEFAULT PRIVILEGES 命令的数据
 */
void
EventTriggerCollectAlterDefPrivs(AlterDefaultPrivilegesStmt *stmt)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* ignore if event trigger context not set, or collection disabled */
	/*
	 *
	 * 若未设置事件触发器上下文，或收集已禁用，则忽略
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_AlterDefaultPrivileges;
	command->d.defprivs.objtype = stmt->action->objtype;
	command->in_extension = creating_extension;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * In a ddl_command_end event trigger, this function reports the DDL commands
 * being run.
 *
 * 在 ddl_command_end 事件触发器中，此函数报告正在运行的 DDL 命令。
 */
Datum
pg_event_trigger_ddl_commands(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	/*
	 * Protect this function from being called out of context
	 *
	 * 防止此函数在错误的上下文中被调用
	 */
	if (!currentEventTriggerState)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in an event trigger function",
						"pg_event_trigger_ddl_commands()")));

	/* Build tuplestore to hold the result rows */
	/*
	 *
	 * 建立用于保存结果行的 tuplestore
	 */
	InitMaterializedSRF(fcinfo, 0);

	foreach(lc, currentEventTriggerState->commandList)
	{
		CollectedCommand *cmd = lfirst(lc);
		Datum		values[9];
		bool		nulls[9] = {0};
		ObjectAddress addr;
		int			i = 0;

		/*
		 * For IF NOT EXISTS commands that attempt to create an existing
		 * object, the returned OID is Invalid.  Don't return anything.
		 *
		 * 对于试图创建已存在对象的 IF NOT EXISTS 命令，返回的 OID 是 Invalid。不要返回任何东西。
		 *
		 * One might think that a viable alternative would be to look up the
		 * Oid of the existing object and run the deparse with that.  But
		 * since the parse tree might be different from the one that created
		 * the object in the first place, we might not end up in a consistent
		 * state anyway.
		 *
		 * 也许有人认为可以查找已存在对象的 OID 并用它做反解析。
		 * 但解析树可能与当初创建该对象时不同，结果仍可能处于不一致状态。
		 */
		if (cmd->type == SCT_Simple &&
			!OidIsValid(cmd->d.simple.address.objectId))
			continue;

		switch (cmd->type)
		{
			case SCT_Simple:
			case SCT_AlterTable:
			case SCT_AlterOpFamily:
			case SCT_CreateOpClass:
			case SCT_AlterTSConfig:
				{
					char	   *identity;
					char	   *type;
					char	   *schema = NULL;

					if (cmd->type == SCT_Simple)
						addr = cmd->d.simple.address;
					else if (cmd->type == SCT_AlterTable)
						ObjectAddressSet(addr,
										 cmd->d.alterTable.classId,
										 cmd->d.alterTable.objectId);
					else if (cmd->type == SCT_AlterOpFamily)
						addr = cmd->d.opfam.address;
					else if (cmd->type == SCT_CreateOpClass)
						addr = cmd->d.createopc.address;
					else if (cmd->type == SCT_AlterTSConfig)
						addr = cmd->d.atscfg.address;

					/*
					 * If an object was dropped in the same command we may end
					 * up in a situation where we generated a message but can
					 * no longer look for the object information, so skip it
					 * rather than failing.  This can happen for example with
					 * some subcommand combinations of ALTER TABLE.
					 *
					 * 若同一条命令中删除了某个对象，可能出现已经生成了消息却再也查不到对象信息的情况，
					 * 这时跳过它而不是失败。例如 ALTER TABLE 的某些子命令组合就会这样。
					 */
					identity = getObjectIdentity(&addr, true);
					if (identity == NULL)
						continue;

					/* The type can never be NULL. */
					/*
					 *
					 * 类型永远不可能是 NULL。
					 */
					type = getObjectTypeDescription(&addr, true);

					/*
					 * Obtain schema name, if any ("pg_temp" if a temp
					 * object). If the object class is not in the supported
					 * list here, we assume it's a schema-less object type,
					 * and thus "schema" remains set to NULL.
					 *
					 * 若有模式名则取得它（临时对象为 pg_temp）。
					 * 若对象类不在这里支持的列表中，则假定它是无模式的对象类型，因此 schema 保持为 NULL。
					 */
					if (is_objectclass_supported(addr.classId))
					{
						AttrNumber	nspAttnum;

						nspAttnum = get_object_attnum_namespace(addr.classId);
						if (nspAttnum != InvalidAttrNumber)
						{
							Relation	catalog;
							HeapTuple	objtup;
							Oid			schema_oid;
							bool		isnull;

							catalog = table_open(addr.classId, AccessShareLock);
							objtup = get_catalog_object_by_oid(catalog,
															   get_object_attnum_oid(addr.classId),
															   addr.objectId);
							if (!HeapTupleIsValid(objtup))
								elog(ERROR, "cache lookup failed for object %u/%u",
									 addr.classId, addr.objectId);
							schema_oid =
								heap_getattr(objtup, nspAttnum,
											 RelationGetDescr(catalog), &isnull);
							if (isnull)
								elog(ERROR,
									 "invalid null namespace in object %u/%u/%d",
									 addr.classId, addr.objectId, addr.objectSubId);
							schema = get_namespace_name_or_temp(schema_oid);

							table_close(catalog, AccessShareLock);
						}
					}

					/* classid */
					/*
					 *
					 * classid
					 */
					values[i++] = ObjectIdGetDatum(addr.classId);
					/* objid */
					/*
					 *
					 * objid
					 */
					values[i++] = ObjectIdGetDatum(addr.objectId);
					/* objsubid */
					/*
					 *
					 * objsubid
					 */
					values[i++] = Int32GetDatum(addr.objectSubId);
					/* command tag */
					/*
					 *
					 * 命令标签
					 */
					values[i++] = CStringGetTextDatum(CreateCommandName(cmd->parsetree));
					/* object_type */
					/*
					 *
					 * object_type
					 */
					values[i++] = CStringGetTextDatum(type);
					/* schema */
					/*
					 *
					 * 模式
					 */
					if (schema == NULL)
						nulls[i++] = true;
					else
						values[i++] = CStringGetTextDatum(schema);
					/* identity */
					/*
					 *
					 * 标识
					 */
					values[i++] = CStringGetTextDatum(identity);
					/* in_extension */
					/*
					 *
					 * in_extension
					 */
					values[i++] = BoolGetDatum(cmd->in_extension);
					/* command */
					/*
					 *
					 * 命令
					 */
					values[i++] = PointerGetDatum(cmd);
				}
				break;

			case SCT_AlterDefaultPrivileges:
				/* classid */
				/*
				 *
				 * classid
				 */
				nulls[i++] = true;
				/* objid */
				/*
				 *
				 * objid
				 */
				nulls[i++] = true;
				/* objsubid */
				/*
				 *
				 * objsubid
				 */
				nulls[i++] = true;
				/* command tag */
				/*
				 *
				 * 命令标签
				 */
				values[i++] = CStringGetTextDatum(CreateCommandName(cmd->parsetree));
				/* object_type */
				/*
				 *
				 * object_type
				 */
				values[i++] = CStringGetTextDatum(stringify_adefprivs_objtype(cmd->d.defprivs.objtype));
				/* schema */
				/*
				 *
				 * 模式
				 */
				nulls[i++] = true;
				/* identity */
				/*
				 *
				 * 标识
				 */
				nulls[i++] = true;
				/* in_extension */
				/*
				 *
				 * in_extension
				 */
				values[i++] = BoolGetDatum(cmd->in_extension);
				/* command */
				/*
				 *
				 * 命令
				 */
				values[i++] = PointerGetDatum(cmd);
				break;

			case SCT_Grant:
				/* classid */
				/*
				 *
				 * classid
				 */
				nulls[i++] = true;
				/* objid */
				/*
				 *
				 * objid
				 */
				nulls[i++] = true;
				/* objsubid */
				/*
				 *
				 * objsubid
				 */
				nulls[i++] = true;
				/* command tag */
				/*
				 *
				 * 命令标签
				 */
				values[i++] = CStringGetTextDatum(cmd->d.grant.istmt->is_grant ?
												  "GRANT" : "REVOKE");
				/* object_type */
				/*
				 *
				 * object_type
				 */
				values[i++] = CStringGetTextDatum(stringify_grant_objtype(cmd->d.grant.istmt->objtype));
				/* schema */
				/*
				 *
				 * 模式
				 */
				nulls[i++] = true;
				/* identity */
				/*
				 *
				 * 标识
				 */
				nulls[i++] = true;
				/* in_extension */
				/*
				 *
				 * in_extension
				 */
				values[i++] = BoolGetDatum(cmd->in_extension);
				/* command */
				/*
				 *
				 * 命令
				 */
				values[i++] = PointerGetDatum(cmd);
				break;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	PG_RETURN_VOID();
}

/*
 * Return the ObjectType as a string, as it would appear in GRANT and
 * REVOKE commands.
 *
 * 把 ObjectType 返回为字符串，形式与它在 GRANT 和 REVOKE 命令中出现时一样。
 */
static const char *
stringify_grant_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return "COLUMN";
		case OBJECT_TABLE:
			return "TABLE";
		case OBJECT_SEQUENCE:
			return "SEQUENCE";
		case OBJECT_DATABASE:
			return "DATABASE";
		case OBJECT_DOMAIN:
			return "DOMAIN";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPER";
		case OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVER";
		case OBJECT_FUNCTION:
			return "FUNCTION";
		case OBJECT_LANGUAGE:
			return "LANGUAGE";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECT";
		case OBJECT_SCHEMA:
			return "SCHEMA";
		case OBJECT_PARAMETER_ACL:
			return "PARAMETER";
		case OBJECT_PROCEDURE:
			return "PROCEDURE";
		case OBJECT_ROUTINE:
			return "ROUTINE";
		case OBJECT_TABLESPACE:
			return "TABLESPACE";
		case OBJECT_TYPE:
			return "TYPE";
			/* these currently aren't used */
			/*
			 *
			 * 这些目前未使用
			 */
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROLE:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			elog(ERROR, "unsupported object type: %d", (int) objtype);
	}

	return "???";				/* keep compiler quiet */
	/*
	 *
	 * 让编译器保持安静
	 */
}

/*
 * Return the ObjectType as a string; as above, but use the spelling
 * in ALTER DEFAULT PRIVILEGES commands instead.  Generally this is just
 * the plural.
 *
 * 把 ObjectType 返回为字符串；与上面相同，但采用 ALTER DEFAULT PRIVILEGES 命令中的拼写。通常就是复数形式。
 */
static const char *
stringify_adefprivs_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return "COLUMNS";
		case OBJECT_TABLE:
			return "TABLES";
		case OBJECT_SEQUENCE:
			return "SEQUENCES";
		case OBJECT_DATABASE:
			return "DATABASES";
		case OBJECT_DOMAIN:
			return "DOMAINS";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPERS";
		case OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVERS";
		case OBJECT_FUNCTION:
			return "FUNCTIONS";
		case OBJECT_LANGUAGE:
			return "LANGUAGES";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECTS";
		case OBJECT_SCHEMA:
			return "SCHEMAS";
		case OBJECT_PROCEDURE:
			return "PROCEDURES";
		case OBJECT_ROUTINE:
			return "ROUTINES";
		case OBJECT_TABLESPACE:
			return "TABLESPACES";
		case OBJECT_TYPE:
			return "TYPES";
			/* these currently aren't used */
			/*
			 *
			 * 这些目前未使用
			 */
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROLE:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			elog(ERROR, "unsupported object type: %d", (int) objtype);
	}

	return "???";				/* keep compiler quiet */
	/*
	 *
	 * 让编译器保持安静
	 */
}
