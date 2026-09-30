/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * 本文件实现 PostgreSQL 的 TRIGGER 支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"


/* GUC variables */
/*
 *
 * GUC 变量
 */
int			SessionReplicationRole = SESSION_REPLICATION_ROLE_ORIGIN;

/* How many levels deep into trigger execution are we? */
/*
 *
 * 当前触发器执行嵌套了多少层？
 */
static int	MyTriggerDepth = 0;

/* Local function prototypes */
/*
 *
 * 本文件内部函数原型
 */
static void renametrig_internal(Relation tgrel, Relation targetrel,
								HeapTuple trigtup, const char *newname,
								const char *expected_name);
static void renametrig_partition(Relation tgrel, Oid partitionId,
								 Oid parentTriggerOid, const char *newname,
								 const char *expected_name);
static void SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger);
static bool GetTupleForTrigger(EState *estate,
							   EPQState *epqstate,
							   ResultRelInfo *relinfo,
							   ItemPointer tid,
							   LockTupleMode lockmode,
							   TupleTableSlot *oldslot,
							   bool do_epq_recheck,
							   TupleTableSlot **epqslot,
							   TM_Result *tmresultp,
							   TM_FailureData *tmfdp);
static bool TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
						   Trigger *trigger, TriggerEvent event,
						   Bitmapset *modifiedCols,
						   TupleTableSlot *oldslot, TupleTableSlot *newslot);
static HeapTuple ExecCallTriggerFunc(TriggerData *trigdata,
									 int tgindx,
									 FmgrInfo *finfo,
									 Instrumentation *instr,
									 MemoryContext per_tuple_context);
static void AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
								  ResultRelInfo *src_partinfo,
								  ResultRelInfo *dst_partinfo,
								  int event, bool row_trigger,
								  TupleTableSlot *oldslot, TupleTableSlot *newslot,
								  List *recheckIndexes, Bitmapset *modifiedCols,
								  TransitionCaptureState *transition_capture,
								  bool is_crosspart_update);
static void AfterTriggerEnlargeQueryState(void);
static bool before_stmt_triggers_fired(Oid relid, CmdType cmdType);
static HeapTuple check_modified_virtual_generated(TupleDesc tupdesc, HeapTuple tuple);


/*
 * 核心流程概览：
 * CreateTrigger / CreateTriggerFiringOn：校验关系种类、权限、WHEN 与分区限制，
 * 写入 pg_trigger 并记录依赖，必要时递归为各分区创建触发器。
 * RelationBuildTriggers：从系统目录装载 TriggerDesc 到 relcache。
 * ExecBS/BR/AS/AR/IR*：按 INSERT、UPDATE、DELETE、TRUNCATE 以及
 * BEFORE、AFTER、INSTEAD OF 和语句级或行级执行触发器。
 * AfterTriggerSaveEvent 将 AFTER 事件入队；AfterTriggerEndQuery 与
 * AfterTriggerFireDeferred 在语句结束或事务结束时按约束延迟状态触发，
 * 并维护 transition table。
 */

/*
 * Create a trigger.  Returns the address of the created trigger.
 *
 * 创建一个触发器，并返回所创建触发器的地址。
 *
 * queryString is the source text of the CREATE TRIGGER command.
 * This must be supplied if a whenClause is specified, else it can be NULL.
 *
 * queryString 是 CREATE TRIGGER 命令的源文本。
 * 若指定了 whenClause 则必须提供，否则可为 NULL。
 *
 * relOid, if nonzero, is the relation on which the trigger should be
 * created.  If zero, the name provided in the statement will be looked up.
 *
 * relOid 非 0 时，是应在其上创建触发器的关系。
 * 为 0 时，按语句中给出的名字查找。
 *
 * refRelOid, if nonzero, is the relation to which the constraint trigger
 * refers.  If zero, the constraint relation name provided in the statement
 * will be looked up as needed.
 *
 * refRelOid 非 0 时，是约束触发器所引用的关系。
 * 为 0 时，按需查找语句中给出的约束关系名。
 *
 * constraintOid, if nonzero, says that this trigger is being created
 * internally to implement that constraint.  A suitable pg_depend entry will
 * be made to link the trigger to that constraint.  constraintOid is zero when
 * executing a user-entered CREATE TRIGGER command.  (For CREATE CONSTRAINT
 * TRIGGER, we build a pg_constraint entry internally.)
 *
 * constraintOid 非 0 表示此触发器是为了实现该约束而在内部创建的。
 * 将建立合适的 pg_depend 项，把触发器连到该约束。
 * 执行用户输入的 CREATE TRIGGER 时 constraintOid 为 0。
 * （对于 CREATE CONSTRAINT TRIGGER，
 * 我们会在内部建立 pg_constraint 项。）
 *
 * indexOid, if nonzero, is the OID of an index associated with the constraint.
 * We do nothing with this except store it into pg_trigger.tgconstrindid;
 * but when creating a trigger for a deferrable unique constraint on a
 * partitioned table, its children are looked up.  Note we don't cope with
 * invalid indexes in that case.
 *
 * indexOid 非 0 时，是与该约束关联的索引 OID。
 * 除了把它写入 pg_trigger.tgconstrindid 之外不做别的事；
 * 但在分区表上为可延迟唯一约束创建触发器时，会查找其子索引。
 * 注意该情况下我们不处理无效索引。
 *
 * funcoid, if nonzero, is the OID of the function to invoke.  When this is
 * given, stmt->funcname is ignored.
 *
 * funcoid 非 0 时，是要调用的函数 OID。
 * 给出该值后忽略 stmt->funcname。
 *
 * parentTriggerOid, if nonzero, is a trigger that begets this one; so that
 * if that trigger is dropped, this one should be too.  There are two cases
 * when a nonzero value is passed for this: 1) when this function recurses to
 * create the trigger on partitions, 2) when creating child foreign key
 * triggers; see CreateFKCheckTrigger() and createForeignKeyActionTriggers().
 *
 * parentTriggerOid 非 0 时，
 * 表示本触发器由其派生；父触发器被删除时本触发器也应删除。
 * 非 0 有两种情形：1) 本函数递归到分区上创建触发器；
 * 2) 创建子表外键触发器，见 CreateFKCheckTrigger() 与
 * createForeignKeyActionTriggers()。
 *
 * If whenClause is passed, it is an already-transformed expression for
 * WHEN.  In this case, we ignore any that may come in stmt->whenClause.
 *
 * 若传入 whenClause，它已是变换后的 WHEN 表达式。
 * 此时忽略 stmt->whenClause 中可能带来的内容。
 *
 * If isInternal is true then this is an internally-generated trigger.
 * This argument sets the tgisinternal field of the pg_trigger entry, and
 * if true causes us to modify the given trigger name to ensure uniqueness.
 *
 * isInternal 为 true 表示这是内部生成的触发器。
 * 该参数设置 pg_trigger 项的 tgisinternal 字段；
 * 为 true 时还会改写给定触发器名以保证唯一。
 *
 * When isInternal is not true we require ACL_TRIGGER permissions on the
 * relation, as well as ACL_EXECUTE on the trigger function.  For internal
 * triggers the caller must apply any required permission checks.
 *
 * isInternal 不为 true 时，要求对关系有 ACL_TRIGGER 权限，
 * 并对触发器函数有 ACL_EXECUTE 权限。
 * 内部触发器由调用者自行完成所需权限检查。
 *
 * When called on partitioned tables, this function recurses to create the
 * trigger on all the partitions, except if isInternal is true, in which
 * case caller is expected to execute recursion on its own.  in_partition
 * indicates such a recursive call; outside callers should pass "false"
 * (but see CloneRowTriggersToPartition).
 *
 * 在分区表上调用时，本函数会递归到所有分区创建触发器；
 * 但 isInternal 为 true 时，由调用者自己执行递归。
 * in_partition 表示这次就是这样的递归调用；外部调用者应传 false
 * （另见 CloneRowTriggersToPartition）。
 */
ObjectAddress
CreateTrigger(CreateTrigStmt *stmt, const char *queryString,
			  Oid relOid, Oid refRelOid, Oid constraintOid, Oid indexOid,
			  Oid funcoid, Oid parentTriggerOid, Node *whenClause,
			  bool isInternal, bool in_partition)
{
	return
		CreateTriggerFiringOn(stmt, queryString, relOid, refRelOid,
							  constraintOid, indexOid, funcoid,
							  parentTriggerOid, whenClause, isInternal,
							  in_partition, TRIGGER_FIRES_ON_ORIGIN);
}

/*
 * Like the above; additionally the firing condition
 * (always/origin/replica/disabled) can be specified.
 *
 * 与上一函数类似；另外可以指定触发时机
 * （always/origin/replica/disabled）。
 */
ObjectAddress
CreateTriggerFiringOn(CreateTrigStmt *stmt, const char *queryString,
					  Oid relOid, Oid refRelOid, Oid constraintOid,
					  Oid indexOid, Oid funcoid, Oid parentTriggerOid,
					  Node *whenClause, bool isInternal, bool in_partition,
					  char trigger_fires_when)
{
	int16		tgtype;
	int			ncolumns;
	int16	   *columns;
	int2vector *tgattr;
	List	   *whenRtable;
	char	   *qual;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	Relation	rel;
	AclResult	aclresult;
	Relation	tgrel;
	Relation	pgrel;
	HeapTuple	tuple = NULL;
	Oid			funcrettype;
	Oid			trigoid = InvalidOid;
	char		internaltrigname[NAMEDATALEN];
	char	   *trigname;
	Oid			constrrelid = InvalidOid;
	ObjectAddress myself,
				referenced;
	char	   *oldtablename = NULL;
	char	   *newtablename = NULL;
	bool		partition_recurse;
	bool		trigger_exists = false;
	Oid			existing_constraint_oid = InvalidOid;
	bool		existing_isInternal = false;
	bool		existing_isClone = false;

	if (OidIsValid(relOid))
		rel = table_open(relOid, ShareRowExclusiveLock);
	else
		rel = table_openrv(stmt->relation, ShareRowExclusiveLock);

	/*
	 * Triggers must be on tables or views, and there are additional
	 * relation-type-specific restrictions.
	 *
	 * 触发器必须建在表或视图上，并且还有与关系类型相关的额外限制。
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		/* Tables can't have INSTEAD OF triggers */
		/*
		 *
		 * 表不能有 INSTEAD OF 触发器
		 */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/* Partitioned tables can't have INSTEAD OF triggers */
		/*
		 *
		 * 分区表不能有 INSTEAD OF 触发器
		 */
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a table",
							RelationGetRelationName(rel)),
					 errdetail("Tables cannot have INSTEAD OF triggers.")));

		/*
		 * FOR EACH ROW triggers have further restrictions
		 *
		 * FOR EACH ROW 触发器还有进一步限制
		 */
		if (stmt->row)
		{
			/*
			 * Disallow use of transition tables.
			 *
			 * 不允许使用 transition table。
			 *
			 * Note that we have another restriction about transition tables
			 * in partitions; search for 'has_superclass' below for an
			 * explanation.  The check here is just to protect from the fact
			 * that if we allowed it here, the creation would succeed for a
			 * partitioned table with no partitions, but would be blocked by
			 * the other restriction when the first partition was created,
			 * which is very unfriendly behavior.
			 *
			 * 分区上的 transition table 另有限制；
			 * 可在下文搜索 has_superclass 查看说明。
			 * 这里的检查只是为了避免：若此处放行，没有分区的分区表能创建成功，
			 * 但创建第一个分区时会被另一条限制拦住，这种行为很不友好。
			 */
			if (stmt->transitionRels != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" is a partitioned table",
								RelationGetRelationName(rel)),
						 errdetail("ROW triggers with transition tables are not supported on partitioned tables.")));
		}
	}
	else if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		/*
		 * Views can have INSTEAD OF triggers (which we check below are
		 * row-level), or statement-level BEFORE/AFTER triggers.
		 *
		 * 视图可以有 INSTEAD OF 触发器（下面会检查必须是行级），
		 * 或语句级 BEFORE/AFTER 触发器。
		 */
		if (stmt->timing != TRIGGER_TYPE_INSTEAD && stmt->row)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have row-level BEFORE or AFTER triggers.")));
		/* Disallow TRUNCATE triggers on VIEWs */
		/*
		 *
		 * 不允许在 VIEW 上建立 TRUNCATE 触发器
		 */
		if (TRIGGER_FOR_TRUNCATE(stmt->events))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a view",
							RelationGetRelationName(rel)),
					 errdetail("Views cannot have TRUNCATE triggers.")));
	}
	else if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		if (stmt->timing != TRIGGER_TYPE_BEFORE &&
			stmt->timing != TRIGGER_TYPE_AFTER)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
					 errdetail("Foreign tables cannot have INSTEAD OF triggers.")));

		/*
		 * We disallow constraint triggers to protect the assumption that
		 * triggers on FKs can't be deferred.  See notes with AfterTriggers
		 * data structures, below.
		 *
		 * 不允许约束触发器，以维持“外键上的触发器不能被延迟”这一假设。
		 * 见下文 AfterTriggers 数据结构处的说明。
		 */
		if (stmt->isconstraint)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a foreign table",
							RelationGetRelationName(rel)),
					 errdetail("Foreign tables cannot have constraint triggers.")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	if (stmt->isconstraint)
	{
		/*
		 * We must take a lock on the target relation to protect against
		 * concurrent drop.  It's not clear that AccessShareLock is strong
		 * enough, but we certainly need at least that much... otherwise, we
		 * might end up creating a pg_constraint entry referencing a
		 * nonexistent table.
		 *
		 * 必须对目标关系加锁，以防并发删除。
		 * AccessShareLock 是否足够并不明确，但至少需要这么强；
		 * 否则可能写出引用不存在表的 pg_constraint 项。
		 */
		if (OidIsValid(refRelOid))
		{
			LockRelationOid(refRelOid, AccessShareLock);
			constrrelid = refRelOid;
		}
		else if (stmt->constrrel != NULL)
			constrrelid = RangeVarGetRelid(stmt->constrrel, AccessShareLock,
										   false);
	}

	/* permission checks */
	/*
	 *
	 * 权限检查
	 */
	if (!isInternal)
	{
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_TRIGGER);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		if (OidIsValid(constrrelid))
		{
			aclresult = pg_class_aclcheck(constrrelid, GetUserId(),
										  ACL_TRIGGER);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, get_relkind_objtype(get_rel_relkind(constrrelid)),
							   get_rel_name(constrrelid));
		}
	}

	/*
	 * When called on a partitioned table to create a FOR EACH ROW trigger
	 * that's not internal, we create one trigger for each partition, too.
	 *
	 * 在分区表上创建非内部的 FOR EACH ROW 触发器时，
	 * 也为每个分区各创建一个触发器。
	 *
	 * For that, we'd better hold lock on all of them ahead of time.
	 *
	 * 为此最好事先锁住所有这些分区。
	 */
	partition_recurse = !isInternal && stmt->row &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (partition_recurse)
		list_free(find_all_inheritors(RelationGetRelid(rel),
									  ShareRowExclusiveLock, NULL));

	/* Compute tgtype */
	/*
	 *
	 * 计算 tgtype
	 */
	TRIGGER_CLEAR_TYPE(tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(tgtype);
	tgtype |= stmt->timing;
	tgtype |= stmt->events;

	/* Disallow ROW-level TRUNCATE triggers */
	/*
	 *
	 * 不允许行级 TRUNCATE 触发器
	 */
	if (TRIGGER_FOR_ROW(tgtype) && TRIGGER_FOR_TRUNCATE(tgtype))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("TRUNCATE FOR EACH ROW triggers are not supported")));

	/* INSTEAD triggers must be row-level, and can't have WHEN or columns */
	/*
	 *
	 * INSTEAD OF 触发器必须是行级，且不能带 WHEN 或列列表
	 */
	if (TRIGGER_FOR_INSTEAD(tgtype))
	{
		if (!TRIGGER_FOR_ROW(tgtype))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers must be FOR EACH ROW")));
		if (stmt->whenClause)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have WHEN conditions")));
		if (stmt->columns != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSTEAD OF triggers cannot have column lists")));
	}

	/*
	 * We don't yet support naming ROW transition variables, but the parser
	 * recognizes the syntax so we can give a nicer message here.
	 *
	 * 尚不支持为行级 transition 变量命名，但解析器认识该语法，
	 * 以便在这里给出更明确的错误信息。
	 *
	 * Per standard, REFERENCING TABLE names are only allowed on AFTER
	 * triggers.  Per standard, REFERENCING ROW names are not allowed with FOR
	 * EACH STATEMENT.  Per standard, each OLD/NEW, ROW/TABLE permutation is
	 * only allowed once.  Per standard, OLD may not be specified when
	 * creating a trigger only for INSERT, and NEW may not be specified when
	 * creating a trigger only for DELETE.
	 *
	 * 按标准，REFERENCING TABLE 名称只允许出现在 AFTER 触发器上。
	 * 按标准，FOR EACH STATEMENT 不允许 REFERENCING ROW
	 * 名称。
	 * 按标准，OLD/NEW 与 ROW/TABLE 的每种组合只能出现一次。
	 * 按标准，仅为 INSERT 创建触发器时不能指定 OLD，
	 * 仅为 DELETE 创建触发器时不能指定 NEW。
	 *
	 * Notice that the standard allows an AFTER ... FOR EACH ROW trigger to
	 * reference both ROW and TABLE transition data.
	 *
	 * 注意：标准允许 AFTER ... FOR EACH ROW 触发器同时引用
	 * ROW 与 TABLE 两类 transition 数据。
	 */
	if (stmt->transitionRels != NIL)
	{
		List	   *varList = stmt->transitionRels;
		ListCell   *lc;

		foreach(lc, varList)
		{
			TriggerTransition *tt = lfirst_node(TriggerTransition, lc);

			if (!(tt->isTable))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ROW variable naming in the REFERENCING clause is not supported"),
						 errhint("Use OLD TABLE or NEW TABLE for naming transition tables.")));

			/*
			 * Because of the above test, we omit further ROW-related testing
			 * below.  If we later allow naming OLD and NEW ROW variables,
			 * adjustments will be needed below.
			 *
			 * 由于上面的检查，下面不再做与 ROW 相关的进一步测试。
			 * 若以后允许为 OLD 与 NEW 行变量命名，下面的逻辑需要相应调整。
			 */

			if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a foreign table",
								RelationGetRelationName(rel)),
						 errdetail("Triggers on foreign tables cannot have transition tables.")));

			if (rel->rd_rel->relkind == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is a view",
								RelationGetRelationName(rel)),
						 errdetail("Triggers on views cannot have transition tables.")));

			/*
			 * We currently don't allow row-level triggers with transition
			 * tables on partition or inheritance children.  Such triggers
			 * would somehow need to see tuples converted to the format of the
			 * table they're attached to, and it's not clear which subset of
			 * tuples each child should see.  See also the prohibitions in
			 * ATExecAttachPartition() and ATExecAddInherit().
			 *
			 * 目前不允许分区或继承子表上的行级触发器使用 transition table。
			 * 这类触发器需要看到已转换到所附关系格式的元组，
			 * 而且每个子表应看到哪一部分元组并不明确。
			 * 另见 ATExecAttachPartition() 与
			 * ATExecAddInherit() 中的禁止项。
			 */
			if (TRIGGER_FOR_ROW(tgtype) && has_superclass(rel->rd_id))
			{
				/* Use appropriate error message. */
				/*
				 *
				 * 使用合适的错误信息。
				 */
				if (rel->rd_rel->relispartition)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ROW triggers with transition tables are not supported on partitions")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("ROW triggers with transition tables are not supported on inheritance children")));
			}

			if (stmt->timing != TRIGGER_TYPE_AFTER)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("transition table name can only be specified for an AFTER trigger")));

			if (TRIGGER_FOR_TRUNCATE(tgtype))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("TRUNCATE triggers with transition tables are not supported")));

			/*
			 * We currently don't allow multi-event triggers ("INSERT OR
			 * UPDATE") with transition tables, because it's not clear how to
			 * handle INSERT ... ON CONFLICT statements which can fire both
			 * INSERT and UPDATE triggers.  We show the inserted tuples to
			 * INSERT triggers and the updated tuples to UPDATE triggers, but
			 * it's not yet clear what INSERT OR UPDATE trigger should see.
			 * This restriction could be lifted if we can decide on the right
			 * semantics in a later release.
			 *
			 * 目前不允许多事件触发器（INSERT OR UPDATE）使用 transition
			 * table，
			 * 因为还不清楚如何处理会同时触发 INSERT 与 UPDATE 触发器的
			 * INSERT ... ON CONFLICT。插入的元组交给 INSERT 触发器，
			 * 更新的元组交给 UPDATE 触发器，但 INSERT OR UPDATE
			 * 触发器应看到什么尚不明确。
			 * 若以后能确定语义，可以取消该限制。
			 */
			if (((TRIGGER_FOR_INSERT(tgtype) ? 1 : 0) +
				 (TRIGGER_FOR_UPDATE(tgtype) ? 1 : 0) +
				 (TRIGGER_FOR_DELETE(tgtype) ? 1 : 0)) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transition tables cannot be specified for triggers with more than one event")));

			/*
			 * We currently don't allow column-specific triggers with
			 * transition tables.  Per spec, that seems to require
			 * accumulating separate transition tables for each combination of
			 * columns, which is a lot of work for a rather marginal feature.
			 *
			 * 目前不允许列级触发器使用 transition table。
			 * 按规范，这似乎需要为每种列组合分别累积 transition table，
			 * 对一项相当边缘的功能来说工作量太大。
			 */
			if (stmt->columns != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("transition tables cannot be specified for triggers with column lists")));

			/*
			 * We disallow constraint triggers with transition tables, to
			 * protect the assumption that such triggers can't be deferred.
			 * See notes with AfterTriggers data structures, below.
			 *
			 * 不允许约束触发器使用 transition table，
			 * 以维持这类触发器不能被延迟的假设。
			 * 见下文 AfterTriggers 数据结构处的说明。
			 *
			 * Currently this is enforced by the grammar, so just Assert here.
			 *
			 * 目前由语法保证，这里只需 Assert。
			 */
			Assert(!stmt->isconstraint);

			if (tt->isNew)
			{
				if (!(TRIGGER_FOR_INSERT(tgtype) ||
					  TRIGGER_FOR_UPDATE(tgtype)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("NEW TABLE can only be specified for an INSERT or UPDATE trigger")));

				if (newtablename != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("NEW TABLE cannot be specified multiple times")));

				newtablename = tt->name;
			}
			else
			{
				if (!(TRIGGER_FOR_DELETE(tgtype) ||
					  TRIGGER_FOR_UPDATE(tgtype)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("OLD TABLE can only be specified for a DELETE or UPDATE trigger")));

				if (oldtablename != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("OLD TABLE cannot be specified multiple times")));

				oldtablename = tt->name;
			}
		}

		if (newtablename != NULL && oldtablename != NULL &&
			strcmp(newtablename, oldtablename) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("OLD TABLE name and NEW TABLE name cannot be the same")));
	}

	/*
	 * Parse the WHEN clause, if any and we weren't passed an already
	 * transformed one.
	 *
	 * 若有 WHEN 子句，且调用者没有传入已变换好的表达式，则解析它。
	 *
	 * Note that as a side effect, we fill whenRtable when parsing.  If we got
	 * an already parsed clause, this does not occur, which is what we want --
	 * no point in adding redundant dependencies below.
	 *
	 * 副作用：解析时会填充 whenRtable。若拿到的已是解析过的子句，则不会填充，
	 * 这正是我们想要的，以免下面重复记录依赖。
	 */
	if (!whenClause && stmt->whenClause)
	{
		ParseState *pstate;
		ParseNamespaceItem *nsitem;
		List	   *varList;
		ListCell   *lc;

		/* Set up a pstate to parse with */
		/*
		 *
		 * 准备用于解析的 pstate
		 */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * Set up nsitems for OLD and NEW references.
		 *
		 * 为 OLD 与 NEW 引用设置 nsitems。
		 *
		 * 'OLD' must always have varno equal to 1 and 'NEW' equal to 2.
		 *
		 * OLD 的 varno 必须始终为 1，NEW 必须为 2。
		 */
		nsitem = addRangeTableEntryForRelation(pstate, rel,
											   AccessShareLock,
											   makeAlias("old", NIL),
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);
		nsitem = addRangeTableEntryForRelation(pstate, rel,
											   AccessShareLock,
											   makeAlias("new", NIL),
											   false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		/* Transform expression.  Copy to be sure we don't modify original */
		/*
		 *
		 * 变换表达式。先复制一份，以免修改原始节点。
		 */
		whenClause = transformWhereClause(pstate,
										  copyObject(stmt->whenClause),
										  EXPR_KIND_TRIGGER_WHEN,
										  "WHEN");
		/* we have to fix its collations too */
		/*
		 *
		 * 还要修正其排序规则
		 */
		assign_expr_collations(pstate, whenClause);

		/*
		 * Check for disallowed references to OLD/NEW.
		 *
		 * 检查对 OLD/NEW 的非法引用。
		 *
		 * NB: pull_var_clause is okay here only because we don't allow
		 * subselects in WHEN clauses; it would fail to examine the contents
		 * of subselects.
		 *
		 * 注意：这里可以用 pull_var_clause，
		 * 只是因为 WHEN 子句不允许子查询；
		 * 否则它不会检查子查询内部。
		 */
		varList = pull_var_clause(whenClause, 0);
		foreach(lc, varList)
		{
			Var		   *var = (Var *) lfirst(lc);

			switch (var->varno)
			{
				case PRS2_OLD_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_INSERT(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("INSERT trigger's WHEN condition cannot reference OLD values"),
								 parser_errposition(pstate, var->location)));
					/* system columns are okay here */
					/*
					 *
					 * 这里允许系统列
					 */
					break;
				case PRS2_NEW_VARNO:
					if (!TRIGGER_FOR_ROW(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("statement trigger's WHEN condition cannot reference column values"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_DELETE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("DELETE trigger's WHEN condition cannot reference NEW values"),
								 parser_errposition(pstate, var->location)));
					if (var->varattno < 0 && TRIGGER_FOR_BEFORE(tgtype))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW system columns"),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_BEFORE(tgtype) &&
						var->varattno == 0 &&
						RelationGetDescr(rel)->constr &&
						(RelationGetDescr(rel)->constr->has_generated_stored ||
						 RelationGetDescr(rel)->constr->has_generated_virtual))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW generated columns"),
								 errdetail("A whole-row reference is used and the table contains generated columns."),
								 parser_errposition(pstate, var->location)));
					if (TRIGGER_FOR_BEFORE(tgtype) &&
						var->varattno > 0 &&
						TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attgenerated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("BEFORE trigger's WHEN condition cannot reference NEW generated columns"),
								 errdetail("Column \"%s\" is a generated column.",
										   NameStr(TupleDescAttr(RelationGetDescr(rel), var->varattno - 1)->attname)),
								 parser_errposition(pstate, var->location)));
					break;
				default:
					/* can't happen without add_missing_from, so just elog */
					/*
					 *
					 * 没有 add_missing_from 时不可能发生，因此直接 elog
					 */
					elog(ERROR, "trigger WHEN condition cannot contain references to other relations");
					break;
			}
		}

		/* we'll need the rtable for recordDependencyOnExpr */
		/*
		 *
		 * recordDependencyOnExpr 需要这份 rtable
		 */
		whenRtable = pstate->p_rtable;

		qual = nodeToString(whenClause);

		free_parsestate(pstate);
	}
	else if (!whenClause)
	{
		whenClause = NULL;
		whenRtable = NIL;
		qual = NULL;
	}
	else
	{
		qual = nodeToString(whenClause);
		whenRtable = NIL;
	}

	/*
	 * Find and validate the trigger function.
	 *
	 * 查找并校验触发器函数。
	 */
	if (!OidIsValid(funcoid))
		funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	if (!isInternal)
	{
		aclresult = object_aclcheck(ProcedureRelationId, funcoid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION,
						   NameListToString(stmt->funcname));
	}
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->funcname), "trigger")));

	/*
	 * Scan pg_trigger to see if there is already a trigger of the same name.
	 * Skip this for internally generated triggers, since we'll modify the
	 * name to be unique below.
	 *
	 * 扫描 pg_trigger，看是否已有同名触发器。
	 * 内部生成的触发器跳过此步，因为下面会把名字改成唯一。
	 *
	 * NOTE that this is cool only because we have ShareRowExclusiveLock on
	 * the relation, so the trigger set won't be changing underneath us.
	 *
	 * 注意：这样做是安全的，因为我们对关系持有 ShareRowExclusiveLock，
	 * 触发器集合不会在我们脚下被改变。
	 */
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	if (!isInternal)
	{
		ScanKeyData skeys[2];
		SysScanDesc tgscan;

		ScanKeyInit(&skeys[0],
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(rel)));

		ScanKeyInit(&skeys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(stmt->trigname));

		tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
									NULL, 2, skeys);

		/* There should be at most one matching tuple */
		/*
		 *
		 * 最多只应有一条匹配元组
		 */
		if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		{
			Form_pg_trigger oldtrigger = (Form_pg_trigger) GETSTRUCT(tuple);

			trigoid = oldtrigger->oid;
			existing_constraint_oid = oldtrigger->tgconstraint;
			existing_isInternal = oldtrigger->tgisinternal;
			existing_isClone = OidIsValid(oldtrigger->tgparentid);
			trigger_exists = true;
			/* copy the tuple to use in CatalogTupleUpdate() */
			/*
			 *
			 * 复制该元组，供 CatalogTupleUpdate() 使用
			 */
			tuple = heap_copytuple(tuple);
		}
		systable_endscan(tgscan);
	}

	if (!trigger_exists)
	{
		/* Generate the OID for the new trigger. */
		/*
		 *
		 * 为新触发器生成 OID。
		 */
		trigoid = GetNewOidWithIndex(tgrel, TriggerOidIndexId,
									 Anum_pg_trigger_oid);
	}
	else
	{
		/*
		 * If OR REPLACE was specified, we'll replace the old trigger;
		 * otherwise complain about the duplicate name.
		 *
		 * 若指定了 OR REPLACE，则替换旧触发器；否则对重名报错。
		 */
		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" already exists",
							stmt->trigname, RelationGetRelationName(rel))));

		/*
		 * An internal trigger or a child trigger (isClone) cannot be replaced
		 * by a user-defined trigger.  However, skip this test when
		 * in_partition, because then we're recursing from a partitioned table
		 * and the check was made at the parent level.
		 *
		 * 内部触发器或子触发器（isClone）不能被用户定义的触发器替换。
		 * 但 in_partition 时跳过此检查，因为那是从分区表递归下来的，
		 * 检查已在父表完成。
		 */
		if ((existing_isInternal || existing_isClone) &&
			!isInternal && !in_partition)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" is an internal or a child trigger",
							stmt->trigname, RelationGetRelationName(rel))));

		/*
		 * It is not allowed to replace with a constraint trigger; gram.y
		 * should have enforced this already.
		 *
		 * 不允许替换成约束触发器；gram.y 应当已经强制了这一点。
		 */
		Assert(!stmt->isconstraint);

		/*
		 * It is not allowed to replace an existing constraint trigger,
		 * either.  (The reason for these restrictions is partly that it seems
		 * difficult to deal with pending trigger events in such cases, and
		 * partly that the command might imply changing the constraint's
		 * properties as well, which doesn't seem nice.)
		 *
		 * 也不允许替换已有的约束触发器。
		 * （限制的原因部分是这类情况下很难处理尚未触发的事件，
		 * 部分是该命令可能同时改变约束属性，这并不合适。）
		 */
		if (OidIsValid(existing_constraint_oid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for relation \"%s\" is a constraint trigger",
							stmt->trigname, RelationGetRelationName(rel))));
	}

	/*
	 * If it's a user-entered CREATE CONSTRAINT TRIGGER command, make a
	 * corresponding pg_constraint entry.
	 *
	 * 若是用户输入的 CREATE CONSTRAINT TRIGGER，
	 * 则建立对应的 pg_constraint 项。
	 */
	if (stmt->isconstraint && !OidIsValid(constraintOid))
	{
		/* Internal callers should have made their own constraints */
		/*
		 *
		 * 内部调用者应已自行建立约束
		 */
		Assert(!isInternal);
		constraintOid = CreateConstraintEntry(stmt->trigname,
											  RelationGetNamespace(rel),
											  CONSTRAINT_TRIGGER,
											  stmt->deferrable,
											  stmt->initdeferred,
											  true, /* Is Enforced */
											  /*
											   *
											   * 是否强制执行
											   */
											  true,
											  InvalidOid,	/* no parent */
											  /*
											   *
											   * 无父约束
											   */
											  RelationGetRelid(rel),
											  NULL, /* no conkey */
											  /*
											   *
											   * 无 conkey
											   */
											  0,
											  0,
											  InvalidOid,	/* no domain */
											  /*
											   *
											   * 无域
											   */
											  InvalidOid,	/* no index */
											  /*
											   *
											   * 无索引
											   */
											  InvalidOid,	/* no foreign key */
											  /*
											   *
											   * 无外键
											   */
											  NULL,
											  NULL,
											  NULL,
											  NULL,
											  0,
											  ' ',
											  ' ',
											  NULL,
											  0,
											  ' ',
											  NULL, /* no exclusion */
											  /*
											   *
											   * 无 exclusion 约束
											   */
											  NULL, /* no check constraint */
											  /*
											   *
											   * 无 CHECK 约束
											   */
											  NULL,
											  true, /* islocal */
											  /*
											   *
											   * islocal
											   */
											  0,	/* inhcount */
											  /*
											   *
											   * inhcount
											   */
											  true, /* noinherit */
											  /*
											   *
											   * noinherit
											   */
											  false,	/* conperiod */
											  /*
											   *
											   * conperiod
											   */
											  isInternal);	/* is_internal */
											  /*
											   *
											   * is_internal
											   */
	}

	/*
	 * If trigger is internally generated, modify the provided trigger name to
	 * ensure uniqueness by appending the trigger OID.  (Callers will usually
	 * supply a simple constant trigger name in these cases.)
	 *
	 * 若触发器是内部生成的，在给定名字后追加触发器 OID 以保证唯一。
	 * （这种情况下调用者通常传入一个简单的常量名。）
	 */
	if (isInternal)
	{
		snprintf(internaltrigname, sizeof(internaltrigname),
				 "%s_%u", stmt->trigname, trigoid);
		trigname = internaltrigname;
	}
	else
	{
		/* user-defined trigger; use the specified trigger name as-is */
		/*
		 *
		 * 用户定义的触发器：按指定名称原样使用
		 */
		trigname = stmt->trigname;
	}

	/*
	 * Build the new pg_trigger tuple.
	 *
	 * 构造新的 pg_trigger 元组。
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_trigger_oid - 1] = ObjectIdGetDatum(trigoid);
	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgparentid - 1] = ObjectIdGetDatum(parentTriggerOid);
	values[Anum_pg_trigger_tgname - 1] = DirectFunctionCall1(namein,
															 CStringGetDatum(trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);
	values[Anum_pg_trigger_tgenabled - 1] = trigger_fires_when;
	values[Anum_pg_trigger_tgisinternal - 1] = BoolGetDatum(isInternal);
	values[Anum_pg_trigger_tgconstrrelid - 1] = ObjectIdGetDatum(constrrelid);
	values[Anum_pg_trigger_tgconstrindid - 1] = ObjectIdGetDatum(indexOid);
	values[Anum_pg_trigger_tgconstraint - 1] = ObjectIdGetDatum(constraintOid);
	values[Anum_pg_trigger_tgdeferrable - 1] = BoolGetDatum(stmt->deferrable);
	values[Anum_pg_trigger_tginitdeferred - 1] = BoolGetDatum(stmt->initdeferred);

	if (stmt->args)
	{
		ListCell   *le;
		char	   *args;
		int16		nargs = list_length(stmt->args);
		int			len = 0;

		foreach(le, stmt->args)
		{
			char	   *ar = strVal(lfirst(le));

			len += strlen(ar) + 4;
			for (; *ar; ar++)
			{
				if (*ar == '\\')
					len++;
			}
		}
		args = (char *) palloc(len + 1);
		args[0] = '\0';
		foreach(le, stmt->args)
		{
			char	   *s = strVal(lfirst(le));
			char	   *d = args + strlen(args);

			while (*s)
			{
				if (*s == '\\')
					*d++ = '\\';
				*d++ = *s++;
			}
			strcpy(d, "\\000");
		}
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(nargs);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
																 CStringGetDatum(args));
	}
	else
	{
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
																 CStringGetDatum(""));
	}

	/* build column number array if it's a column-specific trigger */
	/*
	 *
	 * 若是列级触发器，则建立列号数组
	 */
	ncolumns = list_length(stmt->columns);
	if (ncolumns == 0)
		columns = NULL;
	else
	{
		ListCell   *cell;
		int			i = 0;

		columns = (int16 *) palloc(ncolumns * sizeof(int16));
		foreach(cell, stmt->columns)
		{
			char	   *name = strVal(lfirst(cell));
			int16		attnum;
			int			j;

			/* Lookup column name.  System columns are not allowed */
			/*
			 *
			 * 查找列名。不允许系统列
			 */
			attnum = attnameAttNum(rel, name, false);
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								name, RelationGetRelationName(rel))));

			/* Check for duplicates */
			/*
			 *
			 * 检查重复
			 */
			for (j = i - 1; j >= 0; j--)
			{
				if (columns[j] == attnum)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									name)));
			}

			columns[i++] = attnum;
		}
	}
	tgattr = buildint2vector(columns, ncolumns);
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	/* set tgqual if trigger has WHEN clause */
	/*
	 *
	 * 若触发器有 WHEN 子句，则设置 tgqual
	 */
	if (qual)
		values[Anum_pg_trigger_tgqual - 1] = CStringGetTextDatum(qual);
	else
		nulls[Anum_pg_trigger_tgqual - 1] = true;

	if (oldtablename)
		values[Anum_pg_trigger_tgoldtable - 1] = DirectFunctionCall1(namein,
																	 CStringGetDatum(oldtablename));
	else
		nulls[Anum_pg_trigger_tgoldtable - 1] = true;
	if (newtablename)
		values[Anum_pg_trigger_tgnewtable - 1] = DirectFunctionCall1(namein,
																	 CStringGetDatum(newtablename));
	else
		nulls[Anum_pg_trigger_tgnewtable - 1] = true;

	/*
	 * Insert or replace tuple in pg_trigger.
	 *
	 * 向 pg_trigger 插入或替换元组。
	 */
	if (!trigger_exists)
	{
		tuple = heap_form_tuple(tgrel->rd_att, values, nulls);
		CatalogTupleInsert(tgrel, tuple);
	}
	else
	{
		HeapTuple	newtup;

		newtup = heap_form_tuple(tgrel->rd_att, values, nulls);
		CatalogTupleUpdate(tgrel, &tuple->t_self, newtup);
		heap_freetuple(newtup);
	}

	heap_freetuple(tuple);		/* free either original or new tuple */
	/*
	 *
	 * 释放原始元组或新元组
	 */
	table_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgattr - 1]));
	if (oldtablename)
		pfree(DatumGetPointer(values[Anum_pg_trigger_tgoldtable - 1]));
	if (newtablename)
		pfree(DatumGetPointer(values[Anum_pg_trigger_tgnewtable - 1]));

	/*
	 * Update relation's pg_class entry; if necessary; and if not, send an SI
	 * message to make other backends (and this one) rebuild relcache entries.
	 *
	 * 必要时更新关系的 pg_class 项；若不需要更新，则发送 SI 消息，
	 * 让其他后端（以及本后端）重建 relcache 项。
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	if (!((Form_pg_class) GETSTRUCT(tuple))->relhastriggers)
	{
		((Form_pg_class) GETSTRUCT(tuple))->relhastriggers = true;

		CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

		CommandCounterIncrement();
	}
	else
		CacheInvalidateRelcacheByTuple(tuple);

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * If we're replacing a trigger, flush all the old dependencies before
	 * recording new ones.
	 *
	 * 若正在替换触发器，先清除全部旧依赖，再记录新依赖。
	 */
	if (trigger_exists)
		deleteDependencyRecordsFor(TriggerRelationId, trigoid, true);

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.
	 *
	 * 记录触发器的依赖。始终对函数建立普通依赖。
	 */
	myself.classId = TriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (isInternal && OidIsValid(constraintOid))
	{
		/*
		 * Internally-generated trigger for a constraint, so make it an
		 * internal dependency of the constraint.  We can skip depending on
		 * the relation(s), as there'll be an indirect dependency via the
		 * constraint.
		 *
		 * 这是为约束内部生成的触发器，因此把它做成该约束的内部依赖。
		 * 可以不再依赖关系本身，因为会经由约束形成间接依赖。
		 */
		referenced.classId = ConstraintRelationId;
		referenced.objectId = constraintOid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
	else
	{
		/*
		 * User CREATE TRIGGER, so place dependencies.  We make trigger be
		 * auto-dropped if its relation is dropped or if the FK relation is
		 * dropped.  (Auto drop is compatible with our pre-7.3 behavior.)
		 *
		 * 用户的 CREATE TRIGGER，因此建立依赖。
		 * 关系被删除或外键关系被删除时，触发器自动删除。
		 * （自动删除与 7.3 之前的行为兼容。）
		 */
		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

		if (OidIsValid(constrrelid))
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = constrrelid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		}
		/* Not possible to have an index dependency in this case */
		/*
		 *
		 * 这种情况下不可能有索引依赖
		 */
		Assert(!OidIsValid(indexOid));

		/*
		 * If it's a user-specified constraint trigger, make the constraint
		 * internally dependent on the trigger instead of vice versa.
		 *
		 * 若是用户指定的约束触发器，让约束内部依赖于触发器，而不是反过来。
		 */
		if (OidIsValid(constraintOid))
		{
			referenced.classId = ConstraintRelationId;
			referenced.objectId = constraintOid;
			referenced.objectSubId = 0;
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
		}

		/*
		 * If it's a partition trigger, create the partition dependencies.
		 *
		 * 若是分区触发器，则建立分区依赖。
		 */
		if (OidIsValid(parentTriggerOid))
		{
			ObjectAddressSet(referenced, TriggerRelationId, parentTriggerOid);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_PRI);
			ObjectAddressSet(referenced, RelationRelationId, RelationGetRelid(rel));
			recordDependencyOn(&myself, &referenced, DEPENDENCY_PARTITION_SEC);
		}
	}

	/* If column-specific trigger, add normal dependencies on columns */
	/*
	 *
	 * 若是列级触发器，对列建立普通依赖
	 */
	if (columns != NULL)
	{
		int			i;

		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		for (i = 0; i < ncolumns; i++)
		{
			referenced.objectSubId = columns[i];
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * If it has a WHEN clause, add dependencies on objects mentioned in the
	 * expression (eg, functions, as well as any columns used).
	 *
	 * 若有 WHEN 子句，对表达式中提到的对象建立依赖
	 * （例如函数，以及用到的列）。
	 */
	if (whenRtable != NIL)
		recordDependencyOnExpr(&myself, whenClause, whenRtable,
							   DEPENDENCY_NORMAL);

	/* Post creation hook for new trigger */
	/*
	 *
	 * 新触发器的创建后钩子
	 */
	InvokeObjectPostCreateHookArg(TriggerRelationId, trigoid, 0,
								  isInternal);

	/*
	 * Lastly, create the trigger on child relations, if needed.
	 *
	 * 最后，如有需要，在子关系上创建触发器。
	 */
	if (partition_recurse)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);
		int			i;
		MemoryContext oldcxt,
					perChildCxt;

		perChildCxt = AllocSetContextCreate(CurrentMemoryContext,
											"part trig clone",
											ALLOCSET_SMALL_SIZES);

		/*
		 * We don't currently expect to be called with a valid indexOid.  If
		 * that ever changes then we'll need to write code here to find the
		 * corresponding child index.
		 *
		 * 目前不期望带着有效的 indexOid 被调用。
		 * 若以后会这样，这里需要写出查找对应子索引的代码。
		 */
		Assert(!OidIsValid(indexOid));

		oldcxt = MemoryContextSwitchTo(perChildCxt);

		/* Iterate to create the trigger on each existing partition */
		/*
		 *
		 * 遍历现有分区，在每个分区上创建触发器
		 */
		for (i = 0; i < partdesc->nparts; i++)
		{
			CreateTrigStmt *childStmt;
			Relation	childTbl;
			Node	   *qual;

			childTbl = table_open(partdesc->oids[i], ShareRowExclusiveLock);

			/*
			 * Initialize our fabricated parse node by copying the original
			 * one, then resetting fields that we pass separately.
			 *
			 * 复制原始解析节点来初始化我们构造的节点，
			 * 然后重置那些单独传入的字段。
			 */
			childStmt = copyObject(stmt);
			childStmt->funcname = NIL;
			childStmt->whenClause = NULL;

			/* If there is a WHEN clause, create a modified copy of it */
			/*
			 *
			 * 若有 WHEN 子句，则建立它的一份修改后副本
			 */
			qual = copyObject(whenClause);
			qual = (Node *)
				map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
										childTbl, rel);
			qual = (Node *)
				map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
										childTbl, rel);

			CreateTriggerFiringOn(childStmt, queryString,
								  partdesc->oids[i], refRelOid,
								  InvalidOid, InvalidOid,
								  funcoid, trigoid, qual,
								  isInternal, true, trigger_fires_when);

			table_close(childTbl, NoLock);

			MemoryContextReset(perChildCxt);
		}

		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(perChildCxt);
	}

	/* Keep lock on target rel until end of xact */
	/*
	 *
	 * 对目标关系保持锁直到事务结束
	 */
	table_close(rel, NoLock);

	return myself;
}

/*
 * TriggerSetParentTrigger
 *		Set a partition's trigger as child of its parent trigger,
 *		or remove the linkage if parentTrigId is InvalidOid.
 *
 * TriggerSetParentTrigger：把分区上的触发器设为其父触发器的子触
 * 发器，
 * 或在 parentTrigId 为 InvalidOid 时解除这一联系。
 *
 * This updates the constraint's pg_trigger row to show it as inherited, and
 * adds PARTITION dependencies to prevent the trigger from being deleted
 * on its own.  Alternatively, reverse that.
 *
 * 这会更新约束的 pg_trigger 行，把它标为继承而来，
 * 并添加 PARTITION 依赖，防止该触发器被单独删除。也可以反过来撤销这些。
 */
void
TriggerSetParentTrigger(Relation trigRel,
						Oid childTrigId,
						Oid parentTrigId,
						Oid childTableId)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	Form_pg_trigger trigForm;
	HeapTuple	tuple,
				newtup;
	ObjectAddress depender;
	ObjectAddress referenced;

	/*
	 * Find the trigger to delete.
	 *
	 * 查找要删除的触发器。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(childTrigId));

	tgscan = systable_beginscan(trigRel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tuple = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for trigger %u", childTrigId);
	newtup = heap_copytuple(tuple);
	trigForm = (Form_pg_trigger) GETSTRUCT(newtup);
	if (OidIsValid(parentTrigId))
	{
		/* don't allow setting parent for a constraint that already has one */
		/*
		 *
		 * 不允许为已经有父约束的约束再设置父项
		 */
		if (OidIsValid(trigForm->tgparentid))
			elog(ERROR, "trigger %u already has a parent trigger",
				 childTrigId);

		trigForm->tgparentid = parentTrigId;

		CatalogTupleUpdate(trigRel, &tuple->t_self, newtup);

		ObjectAddressSet(depender, TriggerRelationId, childTrigId);

		ObjectAddressSet(referenced, TriggerRelationId, parentTrigId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_PRI);

		ObjectAddressSet(referenced, RelationRelationId, childTableId);
		recordDependencyOn(&depender, &referenced, DEPENDENCY_PARTITION_SEC);
	}
	else
	{
		trigForm->tgparentid = InvalidOid;

		CatalogTupleUpdate(trigRel, &tuple->t_self, newtup);

		deleteDependencyRecordsForClass(TriggerRelationId, childTrigId,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, childTrigId,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);
	}

	heap_freetuple(newtup);
	systable_endscan(tgscan);
}


/*
 * Guts of trigger deletion.
 *
 * 触发器删除的核心实现。
 */
void
RemoveTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			relid;
	Relation	rel;

	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 *
	 * 查找要删除的触发器。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								NULL, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for trigger %u", trigOid);

	/*
	 * Open and exclusive-lock the relation the trigger belongs to.
	 *
	 * 打开触发器所属关系并加排他锁。
	 */
	relid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

	rel = table_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_VIEW &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * Delete the pg_trigger tuple.
	 *
	 * 删除 pg_trigger 元组。
	 */
	CatalogTupleDelete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	table_close(tgrel, RowExclusiveLock);

	/*
	 * We do not bother to try to determine whether any other triggers remain,
	 * which would be needed in order to decide whether it's safe to clear the
	 * relation's relhastriggers.  (In any case, there might be a concurrent
	 * process adding new triggers.)  Instead, just force a relcache inval to
	 * make other backends (and this one too!) rebuild their relcache entries.
	 * There's no great harm in leaving relhastriggers true even if there are
	 * no triggers left.
	 *
	 * 我们不费力去判断是否还有其他触发器，
	 * 那本可用于决定能否清除关系的 relhastriggers。
	 * （无论如何，可能有并发进程正在添加新触发器。）
	 * 改为强制 relcache 失效，让其他后端（以及本后端）重建 relcache 项。
	 * 即使已经没有触发器，让 relhastriggers 保持为 true 也没有大害。
	 */
	CacheInvalidateRelcache(rel);

	/* Keep lock on trigger's rel until end of xact */
	/*
	 *
	 * 对触发器所属关系保持锁直到事务结束
	 */
	table_close(rel, NoLock);
}

/*
 * get_trigger_oid - Look up a trigger by name to find its OID.
 *
 * get_trigger_oid：按名称查找触发器并返回其 OID。
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 *
 * missing_ok 为 false 时，找不到触发器就报错。
 * 为 true 时只返回 InvalidOid。
 */
Oid
get_trigger_oid(Oid relid, const char *trigname, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * Find the trigger, verify permissions, set up object address
	 *
	 * 查找触发器，校验权限，并设置对象地址
	 */
	tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&skey[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("trigger \"%s\" for table \"%s\" does not exist",
							trigname, get_rel_name(relid))));
		oid = InvalidOid;
	}
	else
	{
		oid = ((Form_pg_trigger) GETSTRUCT(tup))->oid;
	}

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);
	return oid;
}

/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 *
 * 在获取关系锁之前做权限与完整性检查。
 */
static void
RangeVarCallbackForRenameTrigger(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
		/*
		 *
		 * 已被并发删除
		 */
	form = (Form_pg_class) GETSTRUCT(tuple);

	/* only tables and views can have triggers */
	/*
	 *
	 * 只有表和视图可以有触发器
	 */
	if (form->relkind != RELKIND_RELATION && form->relkind != RELKIND_VIEW &&
		form->relkind != RELKIND_FOREIGN_TABLE &&
		form->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" cannot have triggers",
						rv->relname),
				 errdetail_relkind_not_supported(form->relkind)));

	/* you must own the table to rename one of its triggers */
	/*
	 *
	 * 必须拥有该表才能重命名其上的触发器
	 */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);
	if (!allowSystemTableMods && IsSystemClass(relid, form))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	ReleaseSysCache(tuple);
}

/*
 *		renametrig		- changes the name of a trigger on a relation
 *
 * renametrig：修改某关系上一个触发器的名称
 *
 *		trigger name is changed in trigger catalog.
 *		No record of the previous name is kept.
 *
 * 在触发器目录中更改触发器名。
 * 不保留旧名称的记录。
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan trigger catalog
 *				for name conflict (within rel)
 *				for original trigger (if not arg)
 *		modify tgname in trigger tuple
 *		update row in catalog
 *
 * 从关系目录取得正确的 relrelation（若参数未给出）；
 * 扫描触发器目录，查找（关系内的）名称冲突，以及原始触发器（若参数未给出）；
 * 修改触发器元组中的 tgname；
 * 更新目录中的行。
 */
ObjectAddress
renametrig(RenameStmt *stmt)
{
	Oid			tgoid;
	Relation	targetrel;
	Relation	tgrel;
	HeapTuple	tuple;
	SysScanDesc tgscan;
	ScanKeyData key[2];
	Oid			relid;
	ObjectAddress address;

	/*
	 * Look up name, check permissions, and acquire lock (which we will NOT
	 * release until end of transaction).
	 *
	 * 查找名称、检查权限并获取锁（该锁直到事务结束才释放）。
	 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 0,
									 RangeVarCallbackForRenameTrigger,
									 NULL);

	/* Have lock already, so just need to build relcache entry. */
	/*
	 *
	 * 已经持有锁，因此只需建立 relcache 项。
	 */
	targetrel = relation_open(relid, NoLock);

	/*
	 * On partitioned tables, this operation recurses to partitions.  Lock all
	 * tables upfront.
	 *
	 * 在分区表上，此操作会递归到各分区。事先锁住所有表。
	 */
	if (targetrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		(void) find_all_inheritors(relid, AccessExclusiveLock, NULL);

	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Search for the trigger to modify.
	 *
	 * 查找要修改的触发器。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(stmt->subname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger trigform;

		trigform = (Form_pg_trigger) GETSTRUCT(tuple);
		tgoid = trigform->oid;

		/*
		 * If the trigger descends from a trigger on a parent partitioned
		 * table, reject the rename.  We don't allow a trigger in a partition
		 * to differ in name from that of its parent: that would lead to an
		 * inconsistency that pg_dump would not reproduce.
		 *
		 * 若该触发器派生自分区父表上的触发器，则拒绝重命名。
		 * 不允许分区上的触发器与父触发器名称不同，否则 pg_dump 无法再现这种不一致。
		 */
		if (OidIsValid(trigform->tgparentid))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot rename trigger \"%s\" on table \"%s\"",
						   stmt->subname, RelationGetRelationName(targetrel)),
					errhint("Rename the trigger on the partitioned table \"%s\" instead.",
							get_rel_name(get_partition_parent(relid, false))));


		/* Rename the trigger on this relation ... */
		/*
		 *
		 * 重命名此关系上的触发器……
		 */
		renametrig_internal(tgrel, targetrel, tuple, stmt->newname,
							stmt->subname);

		/* ... and if it is partitioned, recurse to its partitions */
		/*
		 *
		 * ……若它是分区表，则递归到其分区
		 */
		if (targetrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(targetrel, true);

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Oid			partitionId = partdesc->oids[i];

				renametrig_partition(tgrel, partitionId, trigform->oid,
									 stmt->newname, stmt->subname);
			}
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						stmt->subname, RelationGetRelationName(targetrel))));
	}

	ObjectAddressSet(address, TriggerRelationId, tgoid);

	systable_endscan(tgscan);

	table_close(tgrel, RowExclusiveLock);

	/*
	 * Close rel, but keep exclusive lock!
	 *
	 * 关闭关系，但保持排他锁！
	 */
	relation_close(targetrel, NoLock);

	return address;
}

/*
 * Subroutine for renametrig -- perform the actual work of renaming one
 * trigger on one table.
 *
 * renametrig 的子程序：在一张表上实际完成一个触发器的重命名。
 *
 * If the trigger has a name different from the expected one, raise a
 * NOTICE about it.
 *
 * 若触发器当前名称与预期不同，则发出 NOTICE。
 */
static void
renametrig_internal(Relation tgrel, Relation targetrel, HeapTuple trigtup,
					const char *newname, const char *expected_name)
{
	HeapTuple	tuple;
	Form_pg_trigger tgform;
	ScanKeyData key[2];
	SysScanDesc tgscan;

	/* If the trigger already has the new name, nothing to do. */
	/*
	 *
	 * 若触发器已经是新名称，则无需处理。
	 */
	tgform = (Form_pg_trigger) GETSTRUCT(trigtup);
	if (strcmp(NameStr(tgform->tgname), newname) == 0)
		return;

	/*
	 * Before actually trying the rename, search for triggers with the same
	 * name.  The update would fail with an ugly message in that case, and it
	 * is better to throw a nicer error.
	 *
	 * 真正重命名之前，先查找同名触发器。
	 * 否则更新会以难看的信息失败，不如抛出更清楚的错误。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(targetrel)));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(newname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("trigger \"%s\" for relation \"%s\" already exists",
						newname, RelationGetRelationName(targetrel))));
	systable_endscan(tgscan);

	/*
	 * The target name is free; update the existing pg_trigger tuple with it.
	 *
	 * 目标名称可用；用它更新现有的 pg_trigger 元组。
	 */
	tuple = heap_copytuple(trigtup);	/* need a modifiable copy */
	/*
	 *
	 * 需要一份可修改的副本
	 */
	tgform = (Form_pg_trigger) GETSTRUCT(tuple);

	/*
	 * If the trigger has a name different from what we expected, let the user
	 * know. (We can proceed anyway, since we must have reached here following
	 * a tgparentid link.)
	 *
	 * 若触发器名称与预期不同，告知用户。
	 * （仍可继续，因为我们是沿着 tgparentid 链接到达这里的。）
	 */
	if (strcmp(NameStr(tgform->tgname), expected_name) != 0)
		ereport(NOTICE,
				errmsg("renamed trigger \"%s\" on relation \"%s\"",
					   NameStr(tgform->tgname),
					   RelationGetRelationName(targetrel)));

	namestrcpy(&tgform->tgname, newname);

	CatalogTupleUpdate(tgrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(TriggerRelationId, tgform->oid, 0);

	/*
	 * Invalidate relation's relcache entry so that other backends (and this
	 * one too!) are sent SI message to make them rebuild relcache entries.
	 * (Ideally this should happen automatically...)
	 *
	 * 使关系的 relcache 项失效，以便向其他后端（以及本后端）发送 SI 消息，
	 * 让它们重建 relcache 项。（理想情况下这应自动发生……）
	 */
	CacheInvalidateRelcache(targetrel);
}

/*
 * Subroutine for renametrig -- Helper for recursing to partitions when
 * renaming triggers on a partitioned table.
 *
 * renametrig 的子程序：在分区表上重命名触发器时，递归到各分区的辅助函数。
 */
static void
renametrig_partition(Relation tgrel, Oid partitionId, Oid parentTriggerOid,
					 const char *newname, const char *expected_name)
{
	SysScanDesc tgscan;
	ScanKeyData key;
	HeapTuple	tuple;

	/*
	 * Given a relation and the OID of a trigger on parent relation, find the
	 * corresponding trigger in the child and rename that trigger to the given
	 * name.
	 *
	 * 给定一个关系以及父关系上某触发器的 OID，
	 * 在子关系中找到对应触发器，并将其重命名为给定名称。
	 */
	ScanKeyInit(&key,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(partitionId));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger tgform = (Form_pg_trigger) GETSTRUCT(tuple);
		Relation	partitionRel;

		if (tgform->tgparentid != parentTriggerOid)
			continue;			/* not our trigger */
			/*
			 *
			 * 不是我们要的触发器
			 */

		partitionRel = table_open(partitionId, NoLock);

		/* Rename the trigger on this partition */
		/*
		 *
		 * 重命名此分区上的触发器
		 */
		renametrig_internal(tgrel, partitionRel, tuple, newname, expected_name);

		/* And if this relation is partitioned, recurse to its partitions */
		/*
		 *
		 * 若此关系仍是分区表，则继续递归到其分区
		 */
		if (partitionRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(partitionRel,
															  true);

			for (int i = 0; i < partdesc->nparts; i++)
			{
				Oid			partoid = partdesc->oids[i];

				renametrig_partition(tgrel, partoid, tgform->oid, newname,
									 NameStr(tgform->tgname));
			}
		}
		table_close(partitionRel, NoLock);

		/* There should be at most one matching tuple */
		/*
		 *
		 * 最多只应有一条匹配元组
		 */
		break;
	}
	systable_endscan(tgscan);
}

/*
 * EnableDisableTrigger()
 *
 * EnableDisableTrigger()
 *
 *	Called by ALTER TABLE ENABLE/DISABLE [ REPLICA | ALWAYS ] TRIGGER
 *	to change 'tgenabled' field for the specified trigger(s)
 *
 * 由 ALTER TABLE ENABLE/DISABLE [ REPLICA |
 * ALWAYS ] TRIGGER 调用，
 * 以修改指定触发器的 tgenabled 字段。
 *
 * rel: relation to process (caller must hold suitable lock on it)
 * tgname: name of trigger to process, or NULL to scan all triggers
 * tgparent: if not zero, process only triggers with this tgparentid
 * fires_when: new value for tgenabled field. In addition to generic
 *			   enablement/disablement, this also defines when the trigger
 *			   should be fired in session replication roles.
 * skip_system: if true, skip "system" triggers (constraint triggers)
 * recurse: if true, recurse to partitions
 *
 * rel：要处理的关系（调用者必须持有合适的锁）
 * tgname：要处理的触发器名，或 NULL 表示扫描全部触发器
 * tgparent：非 0 时只处理具有该 tgparentid 的触发器
 * fires_when：tgenabled 的新值。除了一般的启用/禁用，
 * 还定义在会话复制角色下何时触发。
 * skip_system：为 true 时跳过系统触发器（约束触发器）
 * recurse：为 true 时递归到分区
 *
 * Caller should have checked permissions for the table; here we also
 * enforce that superuser privilege is required to alter the state of
 * system triggers
 *
 * 调用者应已检查表的权限；这里还强制要求：
 * 改变系统触发器的状态必须具有超级用户权限。
 */
void
EnableDisableTrigger(Relation rel, const char *tgname, Oid tgparent,
					 char fires_when, bool skip_system, bool recurse,
					 LOCKMODE lockmode)
{
	Relation	tgrel;
	int			nkeys;
	ScanKeyData keys[2];
	SysScanDesc tgscan;
	HeapTuple	tuple;
	bool		found;
	bool		changed;

	/* Scan the relevant entries in pg_triggers */
	/*
	 *
	 * 扫描 pg_trigger 中的相关项
	 */
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	ScanKeyInit(&keys[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	if (tgname)
	{
		ScanKeyInit(&keys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(tgname));
		nkeys = 2;
	}
	else
		nkeys = 1;

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, nkeys, keys);

	found = changed = false;

	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger oldtrig = (Form_pg_trigger) GETSTRUCT(tuple);

		if (OidIsValid(tgparent) && tgparent != oldtrig->tgparentid)
			continue;

		if (oldtrig->tgisinternal)
		{
			/* system trigger ... ok to process? */
			/*
			 *
			 * 系统触发器……可以处理吗？
			 */
			if (skip_system)
				continue;
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied: \"%s\" is a system trigger",
								NameStr(oldtrig->tgname))));
		}

		found = true;

		if (oldtrig->tgenabled != fires_when)
		{
			/* need to change this one ... make a copy to scribble on */
			/*
			 *
			 * 需要修改这一条……复制一份再改
			 */
			HeapTuple	newtup = heap_copytuple(tuple);
			Form_pg_trigger newtrig = (Form_pg_trigger) GETSTRUCT(newtup);

			newtrig->tgenabled = fires_when;

			CatalogTupleUpdate(tgrel, &newtup->t_self, newtup);

			heap_freetuple(newtup);

			changed = true;
		}

		/*
		 * When altering FOR EACH ROW triggers on a partitioned table, do the
		 * same on the partitions as well, unless ONLY is specified.
		 *
		 * 修改分区表上的 FOR EACH ROW 触发器时，除非指定了 ONLY，
		 * 也对各分区做同样的修改。
		 *
		 * Note that we recurse even if we didn't change the trigger above,
		 * because the partitions' copy of the trigger may have a different
		 * value of tgenabled than the parent's trigger and thus might need to
		 * be changed.
		 *
		 * 注意：即使上面没有改动父触发器，也会递归。
		 * 因为分区上的副本的 tgenabled 可能与父触发器不同，因而仍需要修改。
		 */
		if (recurse &&
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
			(TRIGGER_FOR_ROW(oldtrig->tgtype)))
		{
			PartitionDesc partdesc = RelationGetPartitionDesc(rel, true);
			int			i;

			for (i = 0; i < partdesc->nparts; i++)
			{
				Relation	part;

				part = relation_open(partdesc->oids[i], lockmode);
				/* Match on child triggers' tgparentid, not their name */
				/*
				 *
				 * 按子触发器的 tgparentid 匹配，而不是按名称
				 */
				EnableDisableTrigger(part, NULL, oldtrig->oid,
									 fires_when, skip_system, recurse,
									 lockmode);
				table_close(part, NoLock);	/* keep lock till commit */
				/*
				 *
				 * 保持锁直到提交
				 */
			}
		}

		InvokeObjectPostAlterHook(TriggerRelationId,
								  oldtrig->oid, 0);
	}

	systable_endscan(tgscan);

	table_close(tgrel, RowExclusiveLock);

	if (tgname && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						tgname, RelationGetRelationName(rel))));

	/*
	 * If we changed anything, broadcast a SI inval message to force each
	 * backend (including our own!) to rebuild relation's relcache entry.
	 * Otherwise they will fail to apply the change promptly.
	 *
	 * 若有任何改动，广播 SI 失效消息，强制每个后端（包括我们自己）
	 * 重建该关系的 relcache 项。否则它们不能及时应用这次变更。
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * Build trigger data to attach to the given relcache entry.
 *
 * 构造要挂到给定 relcache 项上的触发器数据。
 *
 * Note that trigger data attached to a relcache entry must be stored in
 * CacheMemoryContext to ensure it survives as long as the relcache entry.
 * But we should be running in a less long-lived working context.  To avoid
 * leaking cache memory if this routine fails partway through, we build a
 * temporary TriggerDesc in working memory and then copy the completed
 * structure into cache memory.
 *
 * 挂在 relcache 项上的触发器数据必须放在
 * CacheMemoryContext 中，
 * 才能与 relcache 项活得一样久。但本函数应运行在更短命的工作上下文中。
 * 为避免中途失败时泄漏缓存内存，先在工作内存中建立临时 TriggerDesc，
 * 完成后再复制到缓存内存。
 */
void
RelationBuildTriggers(Relation relation)
{
	TriggerDesc *trigdesc;
	int			numtrigs;
	int			maxtrigs;
	Trigger    *triggers;
	Relation	tgrel;
	ScanKeyData skey;
	SysScanDesc tgscan;
	HeapTuple	htup;
	MemoryContext oldContext;
	int			i;

	/*
	 * Allocate a working array to hold the triggers (the array is extended if
	 * necessary)
	 *
	 * 分配用于存放触发器的工作数组（必要时会扩展）
	 */
	maxtrigs = 16;
	triggers = (Trigger *) palloc(maxtrigs * sizeof(Trigger));
	numtrigs = 0;

	/*
	 * Note: since we scan the triggers using TriggerRelidNameIndexId, we will
	 * be reading the triggers in name order, except possibly during
	 * emergency-recovery operations (ie, IgnoreSystemIndexes). This in turn
	 * ensures that triggers will be fired in name order.
	 *
	 * 注意：因为用 TriggerRelidNameIndexId 扫描触发器，
	 * 读取顺序是名称顺序，
	 * 紧急恢复（即 IgnoreSystemIndexes）时可能例外。
	 * 这进而保证触发器按名称顺序触发。
	 */
	ScanKeyInit(&skey,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = table_open(TriggerRelationId, AccessShareLock);
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
		Trigger    *build;
		Datum		datum;
		bool		isnull;

		if (numtrigs >= maxtrigs)
		{
			maxtrigs *= 2;
			triggers = (Trigger *) repalloc(triggers, maxtrigs * sizeof(Trigger));
		}
		build = &(triggers[numtrigs]);

		build->tgoid = pg_trigger->oid;
		build->tgname = DatumGetCString(DirectFunctionCall1(nameout,
															NameGetDatum(&pg_trigger->tgname)));
		build->tgfoid = pg_trigger->tgfoid;
		build->tgtype = pg_trigger->tgtype;
		build->tgenabled = pg_trigger->tgenabled;
		build->tgisinternal = pg_trigger->tgisinternal;
		build->tgisclone = OidIsValid(pg_trigger->tgparentid);
		build->tgconstrrelid = pg_trigger->tgconstrrelid;
		build->tgconstrindid = pg_trigger->tgconstrindid;
		build->tgconstraint = pg_trigger->tgconstraint;
		build->tgdeferrable = pg_trigger->tgdeferrable;
		build->tginitdeferred = pg_trigger->tginitdeferred;
		build->tgnargs = pg_trigger->tgnargs;
		/* tgattr is first var-width field, so OK to access directly */
		/*
		 *
		 * tgattr 是第一个变宽字段，因此可以直接访问
		 */
		build->tgnattr = pg_trigger->tgattr.dim1;
		if (build->tgnattr > 0)
		{
			build->tgattr = (int16 *) palloc(build->tgnattr * sizeof(int16));
			memcpy(build->tgattr, &(pg_trigger->tgattr.values),
				   build->tgnattr * sizeof(int16));
		}
		else
			build->tgattr = NULL;
		if (build->tgnargs > 0)
		{
			bytea	   *val;
			char	   *p;

			val = DatumGetByteaPP(fastgetattr(htup,
											  Anum_pg_trigger_tgargs,
											  tgrel->rd_att, &isnull));
			if (isnull)
				elog(ERROR, "tgargs is null in trigger for relation \"%s\"",
					 RelationGetRelationName(relation));
			p = (char *) VARDATA_ANY(val);
			build->tgargs = (char **) palloc(build->tgnargs * sizeof(char *));
			for (i = 0; i < build->tgnargs; i++)
			{
				build->tgargs[i] = pstrdup(p);
				p += strlen(p) + 1;
			}
		}
		else
			build->tgargs = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgoldtable,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgoldtable =
				DatumGetCString(DirectFunctionCall1(nameout, datum));
		else
			build->tgoldtable = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgnewtable,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgnewtable =
				DatumGetCString(DirectFunctionCall1(nameout, datum));
		else
			build->tgnewtable = NULL;

		datum = fastgetattr(htup, Anum_pg_trigger_tgqual,
							tgrel->rd_att, &isnull);
		if (!isnull)
			build->tgqual = TextDatumGetCString(datum);
		else
			build->tgqual = NULL;

		numtrigs++;
	}

	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);

	/* There might not be any triggers */
	/*
	 *
	 * 可能没有任何触发器
	 */
	if (numtrigs == 0)
	{
		pfree(triggers);
		return;
	}

	/* Build trigdesc */
	/*
	 *
	 * 构造 trigdesc
	 */
	trigdesc = (TriggerDesc *) palloc0(sizeof(TriggerDesc));
	trigdesc->triggers = triggers;
	trigdesc->numtriggers = numtrigs;
	for (i = 0; i < numtrigs; i++)
		SetTriggerFlags(trigdesc, &(triggers[i]));

	/* Copy completed trigdesc into cache storage */
	/*
	 *
	 * 把完成的 trigdesc 复制到缓存存储中
	 */
	oldContext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->trigdesc = CopyTriggerDesc(trigdesc);
	MemoryContextSwitchTo(oldContext);

	/* Release working memory */
	/*
	 *
	 * 释放工作内存
	 */
	FreeTriggerDesc(trigdesc);
}

/*
 * Update the TriggerDesc's hint flags to include the specified trigger
 *
 * 更新 TriggerDesc 的提示标志，使之包含指定触发器
 */
static void
SetTriggerFlags(TriggerDesc *trigdesc, Trigger *trigger)
{
	int16		tgtype = trigger->tgtype;

	trigdesc->trig_insert_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_INSERT);
	trigdesc->trig_insert_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_INSERT);
	trigdesc->trig_update_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_update_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_UPDATE);
	trigdesc->trig_delete_before_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_instead_row |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_ROW,
							 TRIGGER_TYPE_INSTEAD, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_DELETE);
	trigdesc->trig_delete_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_DELETE);
	/* there are no row-level truncate triggers */
	/*
	 *
	 * 不存在行级 TRUNCATE 触发器
	 */
	trigdesc->trig_truncate_before_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_TRUNCATE);
	trigdesc->trig_truncate_after_statement |=
		TRIGGER_TYPE_MATCHES(tgtype, TRIGGER_TYPE_STATEMENT,
							 TRIGGER_TYPE_AFTER, TRIGGER_TYPE_TRUNCATE);

	trigdesc->trig_insert_new_table |=
		(TRIGGER_FOR_INSERT(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgnewtable));
	trigdesc->trig_update_old_table |=
		(TRIGGER_FOR_UPDATE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgoldtable));
	trigdesc->trig_update_new_table |=
		(TRIGGER_FOR_UPDATE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgnewtable));
	trigdesc->trig_delete_old_table |=
		(TRIGGER_FOR_DELETE(tgtype) &&
		 TRIGGER_USES_TRANSITION_TABLE(trigger->tgoldtable));
}

/*
 * Copy a TriggerDesc data structure.
 *
 * 复制一份 TriggerDesc 数据结构。
 *
 * The copy is allocated in the current memory context.
 *
 * 副本分配在当前内存上下文中。
 */
TriggerDesc *
CopyTriggerDesc(TriggerDesc *trigdesc)
{
	TriggerDesc *newdesc;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL || trigdesc->numtriggers <= 0)
		return NULL;

	newdesc = (TriggerDesc *) palloc(sizeof(TriggerDesc));
	memcpy(newdesc, trigdesc, sizeof(TriggerDesc));

	trigger = (Trigger *) palloc(trigdesc->numtriggers * sizeof(Trigger));
	memcpy(trigger, trigdesc->triggers,
		   trigdesc->numtriggers * sizeof(Trigger));
	newdesc->triggers = trigger;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		trigger->tgname = pstrdup(trigger->tgname);
		if (trigger->tgnattr > 0)
		{
			int16	   *newattr;

			newattr = (int16 *) palloc(trigger->tgnattr * sizeof(int16));
			memcpy(newattr, trigger->tgattr,
				   trigger->tgnattr * sizeof(int16));
			trigger->tgattr = newattr;
		}
		if (trigger->tgnargs > 0)
		{
			char	  **newargs;
			int16		j;

			newargs = (char **) palloc(trigger->tgnargs * sizeof(char *));
			for (j = 0; j < trigger->tgnargs; j++)
				newargs[j] = pstrdup(trigger->tgargs[j]);
			trigger->tgargs = newargs;
		}
		if (trigger->tgqual)
			trigger->tgqual = pstrdup(trigger->tgqual);
		if (trigger->tgoldtable)
			trigger->tgoldtable = pstrdup(trigger->tgoldtable);
		if (trigger->tgnewtable)
			trigger->tgnewtable = pstrdup(trigger->tgnewtable);
		trigger++;
	}

	return newdesc;
}

/*
 * Free a TriggerDesc data structure.
 *
 * 释放 TriggerDesc 数据结构。
 */
void
FreeTriggerDesc(TriggerDesc *trigdesc)
{
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL)
		return;

	trigger = trigdesc->triggers;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		pfree(trigger->tgname);
		if (trigger->tgnattr > 0)
			pfree(trigger->tgattr);
		if (trigger->tgnargs > 0)
		{
			while (--(trigger->tgnargs) >= 0)
				pfree(trigger->tgargs[trigger->tgnargs]);
			pfree(trigger->tgargs);
		}
		if (trigger->tgqual)
			pfree(trigger->tgqual);
		if (trigger->tgoldtable)
			pfree(trigger->tgoldtable);
		if (trigger->tgnewtable)
			pfree(trigger->tgnewtable);
		trigger++;
	}
	pfree(trigdesc->triggers);
	pfree(trigdesc);
}

/*
 * Compare two TriggerDesc structures for logical equality.
 *
 * 比较两个 TriggerDesc 是否逻辑相等。
 */
#ifdef NOT_USED
bool
equalTriggerDescs(TriggerDesc *trigdesc1, TriggerDesc *trigdesc2)
{
	int			i,
				j;

	/*
	 * We need not examine the hint flags, just the trigger array itself; if
	 * we have the same triggers with the same types, the flags should match.
	 *
	 * 不必检查提示标志，只比较触发器数组本身；
	 * 若触发器及其类型相同，标志也应一致。
	 *
	 * As of 7.3 we assume trigger set ordering is significant in the
	 * comparison; so we just compare corresponding slots of the two sets.
	 *
	 * 自 7.3 起，比较时认为触发器集合的顺序有意义；
	 * 因此只逐槽比较两个集合的对应项。
	 *
	 * Note: comparing the stringToNode forms of the WHEN clauses means that
	 * parse column locations will affect the result.  This is okay as long as
	 * this function is only used for detecting exact equality, as for example
	 * in checking for staleness of a cache entry.
	 *
	 * 注意：比较 WHEN 子句的 stringToNode 形式时，
	 * 解析列位置会影响结果。
	 * 只要本函数只用于检测完全相等（例如判断缓存项是否过期），这是可以接受的。
	 */
	if (trigdesc1 != NULL)
	{
		if (trigdesc2 == NULL)
			return false;
		if (trigdesc1->numtriggers != trigdesc2->numtriggers)
			return false;
		for (i = 0; i < trigdesc1->numtriggers; i++)
		{
			Trigger    *trig1 = trigdesc1->triggers + i;
			Trigger    *trig2 = trigdesc2->triggers + i;

			if (trig1->tgoid != trig2->tgoid)
				return false;
			if (strcmp(trig1->tgname, trig2->tgname) != 0)
				return false;
			if (trig1->tgfoid != trig2->tgfoid)
				return false;
			if (trig1->tgtype != trig2->tgtype)
				return false;
			if (trig1->tgenabled != trig2->tgenabled)
				return false;
			if (trig1->tgisinternal != trig2->tgisinternal)
				return false;
			if (trig1->tgisclone != trig2->tgisclone)
				return false;
			if (trig1->tgconstrrelid != trig2->tgconstrrelid)
				return false;
			if (trig1->tgconstrindid != trig2->tgconstrindid)
				return false;
			if (trig1->tgconstraint != trig2->tgconstraint)
				return false;
			if (trig1->tgdeferrable != trig2->tgdeferrable)
				return false;
			if (trig1->tginitdeferred != trig2->tginitdeferred)
				return false;
			if (trig1->tgnargs != trig2->tgnargs)
				return false;
			if (trig1->tgnattr != trig2->tgnattr)
				return false;
			if (trig1->tgnattr > 0 &&
				memcmp(trig1->tgattr, trig2->tgattr,
					   trig1->tgnattr * sizeof(int16)) != 0)
				return false;
			for (j = 0; j < trig1->tgnargs; j++)
				if (strcmp(trig1->tgargs[j], trig2->tgargs[j]) != 0)
					return false;
			if (trig1->tgqual == NULL && trig2->tgqual == NULL)
				 /* ok */ ;
				 /*
				  *
				  * 一致
				  */
			else if (trig1->tgqual == NULL || trig2->tgqual == NULL)
				return false;
			else if (strcmp(trig1->tgqual, trig2->tgqual) != 0)
				return false;
			if (trig1->tgoldtable == NULL && trig2->tgoldtable == NULL)
				 /* ok */ ;
				 /*
				  *
				  * 一致
				  */
			else if (trig1->tgoldtable == NULL || trig2->tgoldtable == NULL)
				return false;
			else if (strcmp(trig1->tgoldtable, trig2->tgoldtable) != 0)
				return false;
			if (trig1->tgnewtable == NULL && trig2->tgnewtable == NULL)
				 /* ok */ ;
				 /*
				  *
				  * 一致
				  */
			else if (trig1->tgnewtable == NULL || trig2->tgnewtable == NULL)
				return false;
			else if (strcmp(trig1->tgnewtable, trig2->tgnewtable) != 0)
				return false;
		}
	}
	else if (trigdesc2 != NULL)
		return false;
	return true;
}
#endif							/* NOT_USED */
/*
 *
 * 未使用
 */

/*
 * Check if there is a row-level trigger with transition tables that prevents
 * a table from becoming an inheritance child or partition.  Return the name
 * of the first such incompatible trigger, or NULL if there is none.
 *
 * 检查是否存在带 transition table 的行级触发器，
 * 因而阻止该表成为继承子表或分区。
 * 返回第一个不兼容触发器的名称；若没有则返回 NULL。
 */
const char *
FindTriggerIncompatibleWithInheritance(TriggerDesc *trigdesc)
{
	if (trigdesc != NULL)
	{
		int			i;

		for (i = 0; i < trigdesc->numtriggers; ++i)
		{
			Trigger    *trigger = &trigdesc->triggers[i];

			if (!TRIGGER_FOR_ROW(trigger->tgtype))
				continue;
			if (trigger->tgoldtable != NULL || trigger->tgnewtable != NULL)
				return trigger->tgname;
		}
	}

	return NULL;
}

/*
 * Call a trigger function.
 *
 * 调用一个触发器函数。
 *
 *		trigdata: trigger descriptor.
 *		tgindx: trigger's index in finfo and instr arrays.
 *		finfo: array of cached trigger function call information.
 *		instr: optional array of EXPLAIN ANALYZE instrumentation state.
 *		per_tuple_context: memory context to execute the function in.
 *
 * trigdata：触发器描述符。
 * tgindx：该触发器在 finfo 与 instr 数组中的下标。
 * finfo：缓存的触发器函数调用信息数组。
 * instr：可选的 EXPLAIN ANALYZE 插桩状态数组。
 * per_tuple_context：执行该函数所用的内存上下文。
 *
 * Returns the tuple (or NULL) as returned by the function.
 *
 * 返回函数所返回的元组（或 NULL）。
 */
static HeapTuple
ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context)
{
	LOCAL_FCINFO(fcinfo, 0);
	PgStat_FunctionCallUsage fcusage;
	Datum		result;
	MemoryContext oldContext;

	/*
	 * Protect against code paths that may fail to initialize transition table
	 * info.
	 *
	 * 防止某些代码路径未能初始化 transition table 信息。
	 */
	Assert(((TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
			 TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event) ||
			 TRIGGER_FIRED_BY_DELETE(trigdata->tg_event)) &&
			TRIGGER_FIRED_AFTER(trigdata->tg_event) &&
			!(trigdata->tg_event & AFTER_TRIGGER_DEFERRABLE) &&
			!(trigdata->tg_event & AFTER_TRIGGER_INITDEFERRED)) ||
		   (trigdata->tg_oldtable == NULL && trigdata->tg_newtable == NULL));

	finfo += tgindx;

	/*
	 * We cache fmgr lookup info, to avoid making the lookup again on each
	 * call.
	 *
	 * 缓存 fmgr 查找信息，避免每次调用都重新查找。
	 */
	if (finfo->fn_oid == InvalidOid)
		fmgr_info(trigdata->tg_trigger->tgfoid, finfo);

	Assert(finfo->fn_oid == trigdata->tg_trigger->tgfoid);

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger.
	 *
	 * 若正在做 EXPLAIN ANALYZE，开始把时间记到这个触发器上。
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Do the function evaluation in the per-tuple memory context, so that
	 * leaked memory will be reclaimed once per tuple. Note in particular that
	 * any new tuple created by the trigger function will live till the end of
	 * the tuple cycle.
	 *
	 * 在每元组内存上下文中执行函数，使泄漏的内存按元组回收。
	 * 特别是触发器函数新建的元组会活到本轮元组处理结束。
	 */
	oldContext = MemoryContextSwitchTo(per_tuple_context);

	/*
	 * Call the function, passing no arguments but setting a context.
	 *
	 * 调用函数：不传参数，但设置上下文。
	 */
	InitFunctionCallInfoData(*fcinfo, finfo, 0,
							 InvalidOid, (Node *) trigdata, NULL);

	pgstat_init_function_usage(fcinfo, &fcusage);

	MyTriggerDepth++;
	PG_TRY();
	{
		result = FunctionCallInvoke(fcinfo);
	}
	PG_FINALLY();
	{
		MyTriggerDepth--;
	}
	PG_END_TRY();

	pgstat_end_function_usage(&fcusage, true);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Trigger protocol allows function to return a null pointer, but NOT to
	 * set the isnull result flag.
	 *
	 * 触发器协议允许函数返回空指针，但不允许设置 isnull 结果标志。
	 */
	if (fcinfo->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("trigger function %u returned null value",
						fcinfo->flinfo->fn_oid)));

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 *
	 * 若正在做 EXPLAIN ANALYZE，停止把时间记到这个触发器上，
	 * 并计一次“返回的元组”（实际上是触发次数）。
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);

	return (HeapTuple) DatumGetPointer(result);
}

/*
 *
 * 执行 INSERT 的 BEFORE STATEMENT 触发器。
 */

void
ExecBSInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_insert_before_statement)
		return;

	/* no-op if we already fired BS triggers in this context */
	/*
	 *
	 * 若本上下文中已经触发过 BEFORE STATEMENT 触发器，则什么也不做
	 */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_INSERT))
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

/*
 *
 * 执行 INSERT 的 AFTER STATEMENT 触发器。
 */

void
ExecASInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_insert_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_INSERT,
							  false, NULL, NULL, NIL, NULL, transition_capture,
							  false);
}

/*
 *
 * 执行 INSERT 的 BEFORE ROW 触发器；可改写新元组或取消插入。
 */

bool
ExecBRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, slot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			if (should_free)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
			/*
			 *
			 * 什么也不做
			 */
		}
		else if (newtuple != oldtuple)
		{
			newtuple = check_modified_virtual_generated(RelationGetDescr(relinfo->ri_RelationDesc), newtuple);

			ExecForceStoreHeapTuple(newtuple, slot, false);

			/*
			 * After a tuple in a partition goes through a trigger, the user
			 * could have changed the partition key enough that the tuple no
			 * longer fits the partition.  Verify that.
			 *
			 * 分区中的元组经过触发器后，用户可能已把分区键改到不再属于该分区。需要验证这一点。
			 */
			if (trigger->tgisclone &&
				!ExecPartitionCheck(relinfo, slot, estate, false))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("moving row to another partition during a BEFORE FOR EACH ROW trigger is not supported"),
						 errdetail("Before executing trigger \"%s\", the row was to be in partition \"%s.%s\".",
								   trigger->tgname,
								   get_namespace_name(RelationGetNamespace(relinfo->ri_RelationDesc)),
								   RelationGetRelationName(relinfo->ri_RelationDesc))));

			if (should_free)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			/*
			 *
			 * 若还会使用该元组，则发出需要重新读取的信号
			 */
			newtuple = NULL;
		}
	}

	return true;
}

/*
 *
 * 执行 INSERT 的 AFTER ROW 触发器，并把事件交给 AFTER 队列。
 */

void
ExecARInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot, List *recheckIndexes,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (relinfo->ri_FdwRoutine && transition_capture &&
		transition_capture->tcs_insert_new_table)
	{
		Assert(relinfo->ri_RootResultRelInfo);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot collect transition tuples from child foreign tables")));
	}

	if ((trigdesc && trigdesc->trig_insert_after_row) ||
		(transition_capture && transition_capture->tcs_insert_new_table))
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_INSERT,
							  true, NULL, slot,
							  recheckIndexes, NULL,
							  transition_capture,
							  false);
}

/*
 *
 * 执行 INSERT 的 INSTEAD OF ROW 触发器。
 */

bool
ExecIRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 TupleTableSlot *slot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_INSERT))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, slot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			if (should_free)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
			/*
			 *
			 * 什么也不做
			 */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, slot, false);

			if (should_free)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			/*
			 *
			 * 若还会使用该元组，则发出需要重新读取的信号
			 */
			newtuple = NULL;
		}
	}

	return true;
}

/*
 *
 * 执行 DELETE 的 BEFORE STATEMENT 触发器。
 */

void
ExecBSDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_delete_before_statement)
		return;

	/* no-op if we already fired BS triggers in this context */
	/*
	 *
	 * 若本上下文中已经触发过 BEFORE STATEMENT 触发器，则什么也不做
	 */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_DELETE))
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

/*
 *
 * 执行 DELETE 的 AFTER STATEMENT 触发器。
 */

void
ExecASDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_delete_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_DELETE,
							  false, NULL, NULL, NIL, NULL, transition_capture,
							  false);
}

/*
 * Execute BEFORE ROW DELETE triggers.
 *
 * 执行 BEFORE ROW DELETE 触发器。
 *
 * True indicates caller can proceed with the delete.  False indicates caller
 * need to suppress the delete and additionally if requested, we need to pass
 * back the concurrently updated tuple if any.
 *
 * 返回 true 表示调用者可以继续删除。返回 false 表示调用者需要取消删除；
 * 并且若调用者要求，还要把并发更新后的元组传回去。
 */
bool
ExecBRDeleteTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot **epqslot,
					 TM_Result *tmresult,
					 TM_FailureData *tmfd,
					 bool is_merge_delete)
{
	TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	bool		result = true;
	TriggerData LocTriggerData = {0};
	HeapTuple	trigtuple;
	bool		should_free = false;
	int			i;

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		TupleTableSlot *epqslot_candidate = NULL;

		/*
		 * Get a copy of the on-disk tuple we are planning to delete.  In
		 * general, if the tuple has been concurrently updated, we should
		 * recheck it using EPQ.  However, if this is a MERGE DELETE action,
		 * we skip this EPQ recheck and leave it to the caller (it must do
		 * additional rechecking, and might end up executing a different
		 * action entirely).
		 *
		 * 取得准备删除的磁盘元组副本。一般而言，若元组已被并发更新，应用 EPQ 重新检查。
		 * 但若这是 MERGE DELETE 动作，则跳过这次 EPQ 复查并留给调用者
		 * （它必须做额外复查，并且最终可能执行完全不同的动作）。
		 */
		if (!GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
								LockTupleExclusive, slot, !is_merge_delete,
								&epqslot_candidate, tmresult, tmfd))
			return false;

		/*
		 * If the tuple was concurrently updated and the caller of this
		 * function requested for the updated tuple, skip the trigger
		 * execution.
		 *
		 * 若元组已被并发更新，且本函数的调用者要求得到更新后的元组，则跳过触发器执行。
		 */
		if (epqslot_candidate != NULL && epqslot != NULL)
		{
			*epqslot = epqslot_candidate;
			return false;
		}

		trigtuple = ExecFetchSlotHeapTuple(slot, true, &should_free);
	}
	else
	{
		trigtuple = fdw_trigtuple;
		ExecForceStoreHeapTuple(trigtuple, slot, false);
	}

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		HeapTuple	newtuple;
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, slot, NULL))
			continue;

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			result = false;		/* tell caller to suppress delete */
			/*
			 *
			 * 告知调用者取消删除
			 */
			break;
		}
		if (newtuple != trigtuple)
			heap_freetuple(newtuple);
	}
	if (should_free)
		heap_freetuple(trigtuple);

	return result;
}

/*
 * Note: is_crosspart_update must be true if the DELETE is being performed
 * as part of a cross-partition update.
 *
 * 注意：若 DELETE 是跨分区更新的一部分，
 * is_crosspart_update 必须为 true。
 */
void
ExecARDeleteTriggers(EState *estate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TransitionCaptureState *transition_capture,
					 bool is_crosspart_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (relinfo->ri_FdwRoutine && transition_capture &&
		transition_capture->tcs_delete_old_table)
	{
		Assert(relinfo->ri_RootResultRelInfo);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot collect transition tuples from child foreign tables")));
	}

	if ((trigdesc && trigdesc->trig_delete_after_row) ||
		(transition_capture && transition_capture->tcs_delete_old_table))
	{
		TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);

		Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
		if (fdw_trigtuple == NULL)
			GetTupleForTrigger(estate,
							   NULL,
							   relinfo,
							   tupleid,
							   LockTupleExclusive,
							   slot,
							   false,
							   NULL,
							   NULL,
							   NULL);
		else
			ExecForceStoreHeapTuple(fdw_trigtuple, slot, false);

		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_DELETE,
							  true, slot, NULL, NIL, NULL,
							  transition_capture,
							  is_crosspart_update);
	}
}

/*
 *
 * 执行 DELETE 的 INSTEAD OF ROW 触发器。
 */

bool
ExecIRDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *slot = ExecGetTriggerOldSlot(estate, relinfo);
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	ExecForceStoreHeapTuple(trigtuple, slot, false);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		HeapTuple	rettuple;
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_DELETE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, slot, NULL))
			continue;

		LocTriggerData.tg_trigslot = slot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		rettuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (rettuple == NULL)
			return false;		/* Delete was suppressed */
			/*
			 *
			 * 删除已被抑制
			 */
		if (rettuple != trigtuple)
			heap_freetuple(rettuple);
	}
	return true;
}

/*
 *
 * 执行 UPDATE 的 BEFORE STATEMENT 触发器。
 */

void
ExecBSUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};
	Bitmapset  *updatedCols;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_update_before_statement)
		return;

	/* no-op if we already fired BS triggers in this context */
	/*
	 *
	 * 若本上下文中已经触发过 BEFORE STATEMENT 触发器，则什么也不做
	 */
	if (before_stmt_triggers_fired(RelationGetRelid(relinfo->ri_RelationDesc),
								   CMD_UPDATE))
		return;

	/* statement-level triggers operate on the parent table */
	/*
	 *
	 * 语句级触发器作用于父表
	 */
	Assert(relinfo->ri_RootResultRelInfo == NULL);

	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_updatedcols = updatedCols;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							updatedCols, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

/*
 *
 * 执行 UPDATE 的 AFTER STATEMENT 触发器。
 */

void
ExecASUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 TransitionCaptureState *transition_capture)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	/* statement-level triggers operate on the parent table */
	/*
	 *
	 * 语句级触发器作用于父表
	 */
	Assert(relinfo->ri_RootResultRelInfo == NULL);

	if (trigdesc && trigdesc->trig_update_after_statement)
		AfterTriggerSaveEvent(estate, relinfo, NULL, NULL,
							  TRIGGER_EVENT_UPDATE,
							  false, NULL, NULL, NIL,
							  ExecGetAllUpdatedCols(relinfo, estate),
							  transition_capture,
							  false);
}

/*
 *
 * 执行 UPDATE 的 BEFORE ROW 触发器；可改写新元组或取消更新。
 */

bool
ExecBRUpdateTriggers(EState *estate, EPQState *epqstate,
					 ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *newslot,
					 TM_Result *tmresult,
					 TM_FailureData *tmfd,
					 bool is_merge_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *oldslot = ExecGetTriggerOldSlot(estate, relinfo);
	HeapTuple	newtuple = NULL;
	HeapTuple	trigtuple;
	bool		should_free_trig = false;
	bool		should_free_new = false;
	TriggerData LocTriggerData = {0};
	int			i;
	Bitmapset  *updatedCols;
	LockTupleMode lockmode;

	/* Determine lock mode to use */
	/*
	 *
	 * 确定要使用的锁模式
	 */
	lockmode = ExecUpdateLockMode(estate, relinfo);

	Assert(HeapTupleIsValid(fdw_trigtuple) ^ ItemPointerIsValid(tupleid));
	if (fdw_trigtuple == NULL)
	{
		TupleTableSlot *epqslot_candidate = NULL;

		/*
		 * Get a copy of the on-disk tuple we are planning to update.  In
		 * general, if the tuple has been concurrently updated, we should
		 * recheck it using EPQ.  However, if this is a MERGE UPDATE action,
		 * we skip this EPQ recheck and leave it to the caller (it must do
		 * additional rechecking, and might end up executing a different
		 * action entirely).
		 *
		 * 取得准备更新的磁盘元组副本。一般而言，若元组已被并发更新，应用 EPQ 重新检查。
		 * 但若这是 MERGE UPDATE 动作，则跳过这次 EPQ 复查并留给调用者
		 * （它必须做额外复查，并且最终可能执行完全不同的动作）。
		 */
		if (!GetTupleForTrigger(estate, epqstate, relinfo, tupleid,
								lockmode, oldslot, !is_merge_update,
								&epqslot_candidate, tmresult, tmfd))
			return false;		/* cancel the update action */
			/*
			 *
			 * 取消这次更新动作
			 */

		/*
		 * In READ COMMITTED isolation level it's possible that target tuple
		 * was changed due to concurrent update.  In that case we have a raw
		 * subplan output tuple in epqslot_candidate, and need to form a new
		 * insertable tuple using ExecGetUpdateNewTuple to replace the one we
		 * received in newslot.  Neither we nor our callers have any further
		 * interest in the passed-in tuple, so it's okay to overwrite newslot
		 * with the newer data.
		 *
		 * 在 READ COMMITTED 隔离级别下，目标元组可能因并发更新而改变。
		 * 此时 epqslot_candidate 中有子计划输出的原始元组，
		 * 需要用 ExecGetUpdateNewTuple
		 * 形成新的可插入元组，替换 newslot 中收到的那个。
		 * 我们和调用者都不再关心传入的元组，因此可以用更新后的数据覆盖 newslot。
		 */
		if (epqslot_candidate != NULL)
		{
			TupleTableSlot *epqslot_clean;

			epqslot_clean = ExecGetUpdateNewTuple(relinfo, epqslot_candidate,
												  oldslot);

			/*
			 * Typically, the caller's newslot was also generated by
			 * ExecGetUpdateNewTuple, so that epqslot_clean will be the same
			 * slot and copying is not needed.  But do the right thing if it
			 * isn't.
			 *
			 * 通常调用者的 newslot 也是由 ExecGetUpdateNewTuple
			 * 生成的，
			 * 因此 epqslot_clean 就是同一个槽，不必复制。若不是，则按正确方式处理。
			 */
			if (unlikely(newslot != epqslot_clean))
				ExecCopySlot(newslot, epqslot_clean);

			/*
			 * At this point newslot contains a virtual tuple that may
			 * reference some fields of oldslot's tuple in some disk buffer.
			 * If that tuple is in a different page than the original target
			 * tuple, then our only pin on that buffer is oldslot's, and we're
			 * about to release it.  Hence we'd better materialize newslot to
			 * ensure it doesn't contain references into an unpinned buffer.
			 * (We'd materialize it below anyway, but too late for safety.)
			 *
			 * 此时 newslot 含有一个虚元组，可能引用 oldslot
			 * 元组在某个磁盘缓冲区中的字段。
			 * 若该元组与原始目标元组不在同一页，则我们对那个缓冲区的唯一 pin 来自
			 * oldslot，
			 * 而我们即将释放它。因此最好先物化 newslot，避免它引用未 pin 的缓冲区。
			 * （下面本来也会物化，但为了安全已经太晚。）
			 */
			ExecMaterializeSlot(newslot);
		}

		/*
		 * Here we convert oldslot to a materialized slot holding trigtuple.
		 * Neither slot passed to the triggers will hold any buffer pin.
		 *
		 * 这里把 oldslot 转成持有 trigtuple 的物化槽。
		 * 传给触发器的两个槽都不会持有缓冲区 pin。
		 */
		trigtuple = ExecFetchSlotHeapTuple(oldslot, true, &should_free_trig);
	}
	else
	{
		/* Put the FDW-supplied tuple into oldslot to unify the cases */
		/*
		 *
		 * 把 FDW 提供的元组放入 oldslot，以统一各种情形
		 */
		ExecForceStoreHeapTuple(fdw_trigtuple, oldslot, false);
		trigtuple = fdw_trigtuple;
	}

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	updatedCols = ExecGetAllUpdatedCols(relinfo, estate);
	LocTriggerData.tg_updatedcols = updatedCols;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							updatedCols, oldslot, newslot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(newslot, true, &should_free_new);

		LocTriggerData.tg_trigslot = oldslot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;
		LocTriggerData.tg_newslot = newslot;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple == NULL)
		{
			if (should_free_trig)
				heap_freetuple(trigtuple);
			if (should_free_new)
				heap_freetuple(oldtuple);
			return false;		/* "do nothing" */
			/*
			 *
			 * 什么也不做
			 */
		}
		else if (newtuple != oldtuple)
		{
			newtuple = check_modified_virtual_generated(RelationGetDescr(relinfo->ri_RelationDesc), newtuple);

			ExecForceStoreHeapTuple(newtuple, newslot, false);

			/*
			 * If the tuple returned by the trigger / being stored, is the old
			 * row version, and the heap tuple passed to the trigger was
			 * allocated locally, materialize the slot. Otherwise we might
			 * free it while still referenced by the slot.
			 *
			 * 若触发器返回的、即将存储的元组就是旧行版本，
			 * 且传给触发器的堆元组是本地分配的，则物化该槽。
			 * 否则可能在槽仍引用它时就把它释放。
			 */
			if (should_free_trig && newtuple == trigtuple)
				ExecMaterializeSlot(newslot);

			if (should_free_new)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			/*
			 *
			 * 若还会使用该元组，则发出需要重新读取的信号
			 */
			newtuple = NULL;
		}
	}
	if (should_free_trig)
		heap_freetuple(trigtuple);

	return true;
}

/*
 * Note: 'src_partinfo' and 'dst_partinfo', when non-NULL, refer to the source
 * and destination partitions, respectively, of a cross-partition update of
 * the root partitioned table mentioned in the query, given by 'relinfo'.
 * 'tupleid' in that case refers to the ctid of the "old" tuple in the source
 * partition, and 'newslot' contains the "new" tuple in the destination
 * partition.  This interface allows to support the requirements of
 * ExecCrossPartitionUpdateForeignKey(); is_crosspart_update must be true in
 * that case.
 *
 * 注意：src_partinfo 与 dst_partinfo 非 NULL 时，
 * 分别是查询中根分区表
 * （由 relinfo 给出）一次跨分区更新的源分区与目标分区。
 * 此时 tupleid 是源分区中“旧”元组的 ctid，
 * newslot 含有目标分区中的“新”元组。
 * 该接口用于满足 ExecCrossPartitionUpdateForeignKey
 * () 的需要；此时 is_crosspart_update 必须为 true。
 */
void
ExecARUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 ResultRelInfo *src_partinfo,
					 ResultRelInfo *dst_partinfo,
					 ItemPointer tupleid,
					 HeapTuple fdw_trigtuple,
					 TupleTableSlot *newslot,
					 List *recheckIndexes,
					 TransitionCaptureState *transition_capture,
					 bool is_crosspart_update)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (relinfo->ri_FdwRoutine && transition_capture &&
		(transition_capture->tcs_update_old_table ||
		 transition_capture->tcs_update_new_table))
	{
		Assert(relinfo->ri_RootResultRelInfo);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot collect transition tuples from child foreign tables")));
	}

	if ((trigdesc && trigdesc->trig_update_after_row) ||
		(transition_capture &&
		 (transition_capture->tcs_update_old_table ||
		  transition_capture->tcs_update_new_table)))
	{
		/*
		 * Note: if the UPDATE is converted into a DELETE+INSERT as part of
		 * update-partition-key operation, then this function is also called
		 * separately for DELETE and INSERT to capture transition table rows.
		 * In such case, either old tuple or new tuple can be NULL.
		 *
		 * 注意：若 UPDATE 因更新分区键而被改写成 DELETE+INSERT，
		 * 则本函数也会分别为 DELETE 与 INSERT 调用，
		 * 以捕获 transition table 行。
		 * 此时旧元组或新元组之一可以为 NULL。
		 */
		TupleTableSlot *oldslot;
		ResultRelInfo *tupsrc;

		Assert((src_partinfo != NULL && dst_partinfo != NULL) ||
			   !is_crosspart_update);

		tupsrc = src_partinfo ? src_partinfo : relinfo;
		oldslot = ExecGetTriggerOldSlot(estate, tupsrc);

		if (fdw_trigtuple == NULL && ItemPointerIsValid(tupleid))
			GetTupleForTrigger(estate,
							   NULL,
							   tupsrc,
							   tupleid,
							   LockTupleExclusive,
							   oldslot,
							   false,
							   NULL,
							   NULL,
							   NULL);
		else if (fdw_trigtuple != NULL)
			ExecForceStoreHeapTuple(fdw_trigtuple, oldslot, false);
		else
			ExecClearTuple(oldslot);

		AfterTriggerSaveEvent(estate, relinfo,
							  src_partinfo, dst_partinfo,
							  TRIGGER_EVENT_UPDATE,
							  true,
							  oldslot, newslot, recheckIndexes,
							  ExecGetAllUpdatedCols(relinfo, estate),
							  transition_capture,
							  is_crosspart_update);
	}
}

/*
 *
 * 执行 UPDATE 的 INSTEAD OF ROW 触发器。
 */

bool
ExecIRUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple, TupleTableSlot *newslot)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	TupleTableSlot *oldslot = ExecGetTriggerOldSlot(estate, relinfo);
	HeapTuple	newtuple = NULL;
	bool		should_free;
	TriggerData LocTriggerData = {0};
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_INSTEAD;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	ExecForceStoreHeapTuple(trigtuple, oldslot, false);

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	oldtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_INSTEAD,
								  TRIGGER_TYPE_UPDATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, oldslot, newslot))
			continue;

		if (!newtuple)
			newtuple = ExecFetchSlotHeapTuple(newslot, true, &should_free);

		LocTriggerData.tg_trigslot = oldslot;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newslot = newslot;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			return false;		/* "do nothing" */
			/*
			 *
			 * 什么也不做
			 */
		}
		else if (newtuple != oldtuple)
		{
			ExecForceStoreHeapTuple(newtuple, newslot, false);

			if (should_free)
				heap_freetuple(oldtuple);

			/* signal tuple should be re-fetched if used */
			/*
			 *
			 * 若还会使用该元组，则发出需要重新读取的信号
			 */
			newtuple = NULL;
		}
	}

	return true;
}

/*
 *
 * 执行 TRUNCATE 的 BEFORE STATEMENT 触发器。
 */

void
ExecBSTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			i;
	TriggerData LocTriggerData = {0};

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;
	if (!trigdesc->trig_truncate_before_statement)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_TRUNCATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];
		HeapTuple	newtuple;

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  TRIGGER_TYPE_STATEMENT,
								  TRIGGER_TYPE_BEFORE,
								  TRIGGER_TYPE_TRUNCATE))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, LocTriggerData.tg_event,
							NULL, NULL, NULL))
			continue;

		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   i,
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
					 errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

/*
 *
 * 执行 TRUNCATE 的 AFTER STATEMENT 触发器。
 */

void
ExecASTruncateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->trig_truncate_after_statement)
		AfterTriggerSaveEvent(estate, relinfo,
							  NULL, NULL,
							  TRIGGER_EVENT_TRUNCATE,
							  false, NULL, NULL, NIL, NULL, NULL,
							  false);
}


/*
 * Fetch tuple into "oldslot", dealing with locking and EPQ if necessary
 *
 * 把元组取入 oldslot，并在需要时处理加锁与 EPQ
 */
static bool
GetTupleForTrigger(EState *estate,
				   EPQState *epqstate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   LockTupleMode lockmode,
				   TupleTableSlot *oldslot,
				   bool do_epq_recheck,
				   TupleTableSlot **epqslot,
				   TM_Result *tmresultp,
				   TM_FailureData *tmfdp)
{
	Relation	relation = relinfo->ri_RelationDesc;

	if (epqslot != NULL)
	{
		TM_Result	test;
		TM_FailureData tmfd;
		int			lockflags = 0;

		*epqslot = NULL;

		/* caller must pass an epqstate if EvalPlanQual is possible */
		/*
		 *
		 * 若可能进行 EvalPlanQual，调用者必须传入 epqstate
		 */
		Assert(epqstate != NULL);

		/*
		 * lock tuple for update
		 *
		 * 为更新而锁定元组
		 */
		if (!IsolationUsesXactSnapshot())
			lockflags |= TUPLE_LOCK_FLAG_FIND_LAST_VERSION;
		test = table_tuple_lock(relation, tid, estate->es_snapshot, oldslot,
								estate->es_output_cid,
								lockmode, LockWaitBlock,
								lockflags,
								&tmfd);

		/* Let the caller know about the status of this operation */
		/*
		 *
		 * 把本次操作的状态告知调用者
		 */
		if (tmresultp)
			*tmresultp = test;
		if (tmfdp)
			*tmfdp = tmfd;

		switch (test)
		{
			case TM_SelfModified:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  We ignore the tuple in the former case, and
				 * throw error in the latter case, for the same reasons
				 * enumerated in ExecUpdate and ExecDelete in
				 * nodeModifyTable.c.
				 *
				 * 目标元组已被当前命令，或当前事务中更晚的命令更新或删除。
				 * 前一种情况忽略该元组，后一种情况报错，原因与 nodeModifyTable.c 中
				 * ExecUpdate 和 ExecDelete 所列举的相同。
				 */
				if (tmfd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* treat it as deleted; do not process */
				/*
				 *
				 * 当作已删除处理；不再处理
				 */
				return false;

			case TM_Ok:
				if (tmfd.traversed)
				{
					/*
					 * Recheck the tuple using EPQ, if requested.  Otherwise,
					 * just return that it was concurrently updated.
					 *
					 * 若要求，则用 EPQ 重新检查该元组。否则只返回它已被并发更新。
					 */
					if (do_epq_recheck)
					{
						*epqslot = EvalPlanQual(epqstate,
												relation,
												relinfo->ri_RangeTableIndex,
												oldslot);

						/*
						 * If PlanQual failed for updated tuple - we must not
						 * process this tuple!
						 *
						 * 若对已更新元组的 PlanQual 失败，则不得处理该元组！
						 */
						if (TupIsNull(*epqslot))
						{
							*epqslot = NULL;
							return false;
						}
					}
					else
					{
						if (tmresultp)
							*tmresultp = TM_Updated;
						return false;
					}
				}
				break;

			case TM_Updated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				elog(ERROR, "unexpected table_tuple_lock status: %u", test);
				break;

			case TM_Deleted:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent delete")));
				/* tuple was deleted */
				/*
				 *
				 * 元组已被删除
				 */
				return false;

			case TM_Invisible:
				elog(ERROR, "attempted to lock invisible tuple");
				break;

			default:
				elog(ERROR, "unrecognized table_tuple_lock status: %u", test);
				return false;	/* keep compiler quiet */
				/*
				 *
				 * 避免编译器告警
				 */
		}
	}
	else
	{
		/*
		 * We expect the tuple to be present, thus very simple error handling
		 * suffices.
		 *
		 * 我们预期元组存在，因此非常简单的错误处理就够了。
		 */
		if (!table_tuple_fetch_row_version(relation, tid, SnapshotAny,
										   oldslot))
			elog(ERROR, "failed to fetch tuple for trigger");
	}

	return true;
}

/*
 * Is trigger enabled to fire?
 *
 * 该触发器是否允许触发？
 */
static bool
TriggerEnabled(EState *estate, ResultRelInfo *relinfo,
			   Trigger *trigger, TriggerEvent event,
			   Bitmapset *modifiedCols,
			   TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	/* Check replication-role-dependent enable state */
	/*
	 *
	 * 检查依赖于复制角色的启用状态
	 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_ORIGIN ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}
	else						/* ORIGIN or LOCAL role */
	/*
	 *
	 * ORIGIN 或 LOCAL 角色
	 */
	{
		if (trigger->tgenabled == TRIGGER_FIRES_ON_REPLICA ||
			trigger->tgenabled == TRIGGER_DISABLED)
			return false;
	}

	/*
	 * Check for column-specific trigger (only possible for UPDATE, and in
	 * fact we *must* ignore tgattr for other event types)
	 *
	 * 检查列级触发器（只可能出现在 UPDATE 上；
	 * 对其他事件类型实际上必须忽略 tgattr）
	 */
	if (trigger->tgnattr > 0 && TRIGGER_FIRED_BY_UPDATE(event))
	{
		int			i;
		bool		modified;

		modified = false;
		for (i = 0; i < trigger->tgnattr; i++)
		{
			if (bms_is_member(trigger->tgattr[i] - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols))
			{
				modified = true;
				break;
			}
		}
		if (!modified)
			return false;
	}

	/* Check for WHEN clause */
	/*
	 *
	 * 检查 WHEN 子句
	 */
	if (trigger->tgqual)
	{
		ExprState **predicate;
		ExprContext *econtext;
		MemoryContext oldContext;
		int			i;

		Assert(estate != NULL);

		/*
		 * trigger is an element of relinfo->ri_TrigDesc->triggers[]; find the
		 * matching element of relinfo->ri_TrigWhenExprs[]
		 *
		 * 该触发器是 relinfo->ri_TrigDesc->triggers[]
		 * 的一个元素；
		 * 找到 relinfo->ri_TrigWhenExprs[] 中对应的元素
		 */
		i = trigger - relinfo->ri_TrigDesc->triggers;
		predicate = &relinfo->ri_TrigWhenExprs[i];

		/*
		 * If first time through for this WHEN expression, build expression
		 * nodetrees for it.  Keep them in the per-query memory context so
		 * they'll survive throughout the query.
		 *
		 * 若是第一次处理这个 WHEN 表达式，则为它建立表达式节点树。
		 * 把它们放在每查询内存上下文中，以便在整个查询期间存活。
		 */
		if (*predicate == NULL)
		{
			Node	   *tgqual;

			oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
			tgqual = stringToNode(trigger->tgqual);
			tgqual = expand_generated_columns_in_expr(tgqual, relinfo->ri_RelationDesc, PRS2_OLD_VARNO);
			tgqual = expand_generated_columns_in_expr(tgqual, relinfo->ri_RelationDesc, PRS2_NEW_VARNO);
			/* Change references to OLD and NEW to INNER_VAR and OUTER_VAR */
			/*
			 *
			 * 把对 OLD 和 NEW 的引用改成 INNER_VAR 与 OUTER_VAR
			 */
			ChangeVarNodes(tgqual, PRS2_OLD_VARNO, INNER_VAR, 0);
			ChangeVarNodes(tgqual, PRS2_NEW_VARNO, OUTER_VAR, 0);
			/* ExecPrepareQual wants implicit-AND form */
			/*
			 *
			 * ExecPrepareQual 需要隐式 AND 形式
			 */
			tgqual = (Node *) make_ands_implicit((Expr *) tgqual);
			*predicate = ExecPrepareQual((List *) tgqual, estate);
			MemoryContextSwitchTo(oldContext);
		}

		/*
		 * We will use the EState's per-tuple context for evaluating WHEN
		 * expressions (creating it if it's not already there).
		 *
		 * 用 EState 的每元组上下文来计算 WHEN 表达式（若还没有则创建）。
		 */
		econtext = GetPerTupleExprContext(estate);

		/*
		 * Finally evaluate the expression, making the old and/or new tuples
		 * available as INNER_VAR/OUTER_VAR respectively.
		 *
		 * 最后计算表达式，使旧元组和/或新元组分别作为 INNER_VAR/OUTER_VAR
		 * 可用。
		 */
		econtext->ecxt_innertuple = oldslot;
		econtext->ecxt_outertuple = newslot;
		if (!ExecQual(*predicate, econtext))
			return false;
	}

	return true;
}


/* ----------
 * After-trigger stuff
 *
 * AFTER 触发器相关逻辑
 *
 * The AfterTriggersData struct holds data about pending AFTER trigger events
 * during the current transaction tree.  (BEFORE triggers are fired
 * immediately so we don't need any persistent state about them.)  The struct
 * and most of its subsidiary data are kept in TopTransactionContext; however
 * some data that can be discarded sooner appears in the CurTransactionContext
 * of the relevant subtransaction.  Also, the individual event records are
 * kept in a separate sub-context of TopTransactionContext.  This is done
 * mainly so that it's easy to tell from a memory context dump how much space
 * is being eaten by trigger events.
 *
 * AfterTriggersData 保存当前事务树中尚未处理的 AFTER
 * 触发器事件。
 * （BEFORE 触发器立即执行，因此不需要持久状态。）
 * 该结构及其大部分附属数据放在 TopTransactionContext 中；
 * 可以更早丢弃的数据则放在相应子事务的 CurTransactionContext 中。
 * 单个事件记录放在 TopTransactionContext 的单独子上下文中，
 * 主要是为了从内存上下文转储中容易看出触发器事件占用了多少空间。
 *
 * Because the list of pending events can grow large, we go to some
 * considerable effort to minimize per-event memory consumption.  The event
 * records are grouped into chunks and common data for similar events in the
 * same chunk is only stored once.
 *
 * 待处理事件列表可能变得很大，因此我们相当努力地减少每个事件的内存消耗。
 * 事件记录按块分组，同一块中相似事件的公共数据只存一份。
 *
 * XXX We need to be able to save the per-event data in a file if it grows too
 * large.
 * ----------
 *
 * XXX 若每事件数据增长过大，需要能够把它保存到文件中。
 */

/* Per-trigger SET CONSTRAINT status */
/*
 *
 * 每个触发器的 SET CONSTRAINT 状态
 */
typedef struct SetConstraintTriggerData
{
	Oid			sct_tgoid;
	bool		sct_tgisdeferred;
} SetConstraintTriggerData;

typedef struct SetConstraintTriggerData *SetConstraintTrigger;

/*
 * SET CONSTRAINT intra-transaction status.
 *
 * SET CONSTRAINT 的事务内状态。
 *
 * We make this a single palloc'd object so it can be copied and freed easily.
 *
 * 把它做成一次 palloc 的对象，以便容易复制和释放。
 *
 * all_isset and all_isdeferred are used to keep track
 * of SET CONSTRAINTS ALL {DEFERRED, IMMEDIATE}.
 *
 * all_isset 与 all_isdeferred 用于跟踪 SET
 * CONSTRAINTS ALL {DEFERRED, IMMEDIATE}。
 *
 * trigstates[] stores per-trigger tgisdeferred settings.
 *
 * trigstates[] 保存每个触发器的 tgisdeferred 设置。
 */
typedef struct SetConstraintStateData
{
	bool		all_isset;
	bool		all_isdeferred;
	int			numstates;		/* number of trigstates[] entries in use */
	/*
	 *
	 * 正在使用的 trigstates[] 项数
	 */
	int			numalloc;		/* allocated size of trigstates[] */
	/*
	 *
	 * trigstates[] 的分配大小
	 */
	SetConstraintTriggerData trigstates[FLEXIBLE_ARRAY_MEMBER];
} SetConstraintStateData;

typedef SetConstraintStateData *SetConstraintState;


/*
 * Per-trigger-event data
 *
 * 每个触发事件的数据
 *
 * The actual per-event data, AfterTriggerEventData, includes DONE/IN_PROGRESS
 * status bits, up to two tuple CTIDs, and optionally two OIDs of partitions.
 * Each event record also has an associated AfterTriggerSharedData that is
 * shared across all instances of similar events within a "chunk".
 *
 * 实际的每事件数据 AfterTriggerEventData 包含
 * DONE/IN_PROGRESS 状态位、
 * 最多两个元组 CTID，以及可选的两个分区 OID。
 * 每条事件记录还关联一份 AfterTriggerSharedData，
 * 在一个块内的所有相似事件实例之间共享。
 *
 * For row-level triggers, we arrange not to waste storage on unneeded ctid
 * fields.  Updates of regular tables use two; inserts and deletes of regular
 * tables use one; foreign tables always use zero and save the tuple(s) to a
 * tuplestore.  AFTER_TRIGGER_FDW_FETCH directs AfterTriggerExecute() to
 * retrieve a fresh tuple or pair of tuples from that tuplestore, while
 * AFTER_TRIGGER_FDW_REUSE directs it to use the most-recently-retrieved
 * tuple(s).  This permits storing tuples once regardless of the number of
 * row-level triggers on a foreign table.
 *
 * 对行级触发器，我们避免为不需要的 ctid 字段浪费存储。
 * 普通表的更新用两个；普通表的插入和删除用一个；外部表始终用零个，
 * 并把元组保存到 tuplestore。AFTER_TRIGGER_FDW_FETCH
 * 让 AfterTriggerExecute()
 * 从该 tuplestore 取一个或一对新元组，
 * AFTER_TRIGGER_FDW_REUSE 则让它使用最近取到的元组。
 * 这样无论外部表上有多少行级触发器，元组都只存一次。
 *
 * When updates on partitioned tables cause rows to move between partitions,
 * the OIDs of both partitions are stored too, so that the tuples can be
 * fetched; such entries are marked AFTER_TRIGGER_CP_UPDATE (for "cross-
 * partition update").
 *
 * 分区表更新导致行在分区间移动时，也会保存两个分区的 OID，以便取回元组；
 * 这类项标记为 AFTER_TRIGGER_CP_UPDATE（表示跨分区更新）。
 *
 * Note that we need triggers on foreign tables to be fired in exactly the
 * order they were queued, so that the tuples come out of the tuplestore in
 * the right order.  To ensure that, we forbid deferrable (constraint)
 * triggers on foreign tables.  This also ensures that such triggers do not
 * get deferred into outer trigger query levels, meaning that it's okay to
 * destroy the tuplestore at the end of the query level.
 *
 * 注意：外部表上的触发器必须严格按入队顺序触发，
 * 元组才会按正确顺序从 tuplestore 出来。
 * 为此禁止外部表上的可延迟（约束）触发器。
 * 这也保证它们不会被推迟到外层触发器查询级别，
 * 因此可以在查询级别结束时销毁 tuplestore。
 *
 * Statement-level triggers always bear AFTER_TRIGGER_1CTID, though they
 * require no ctid field.  We lack the flag bit space to neatly represent that
 * distinct case, and it seems unlikely to be worth much trouble.
 *
 * 语句级触发器始终带 AFTER_TRIGGER_1CTID，
 * 尽管它们不需要 ctid 字段。
 * 我们没有足够的标志位来整齐表示这种单独情形，而且似乎不值得为此大费周章。
 *
 * Note: ats_firing_id is initially zero and is set to something else when
 * AFTER_TRIGGER_IN_PROGRESS is set.  It indicates which trigger firing
 * cycle the trigger will be fired in (or was fired in, if DONE is set).
 * Although this is mutable state, we can keep it in AfterTriggerSharedData
 * because all instances of the same type of event in a given event list will
 * be fired at the same time, if they were queued between the same firing
 * cycles.  So we need only ensure that ats_firing_id is zero when attaching
 * a new event to an existing AfterTriggerSharedData record.
 *
 * 注意：ats_firing_id 初始为 0，
 * 在设置 AFTER_TRIGGER_IN_PROGRESS 时改为其他值。
 * 它表示该触发器将在（或若已设 DONE，则曾经在）哪一轮触发周期中触发。
 * 尽管这是可变状态，仍可放在 AfterTriggerSharedData 中，
 * 因为给定事件列表里
 * 同一类型事件的所有实例若在相同触发周期之间入队，就会同时触发。
 * 因此只需保证把新事件挂到已有 AfterTriggerSharedData 记录时
 * ats_firing_id 为 0。
 */
typedef uint32 TriggerFlags;

#define AFTER_TRIGGER_OFFSET			0x07FFFFFF	/* must be low-order bits */
/*
 *
 * 必须是低位
 */
#define AFTER_TRIGGER_DONE				0x80000000
#define AFTER_TRIGGER_IN_PROGRESS		0x40000000
/* bits describing the size and tuple sources of this event */
/*
 *
 * 描述本事件大小与元组来源的位
 */
#define AFTER_TRIGGER_FDW_REUSE			0x00000000
#define AFTER_TRIGGER_FDW_FETCH			0x20000000
#define AFTER_TRIGGER_1CTID				0x10000000
#define AFTER_TRIGGER_2CTID				0x30000000
#define AFTER_TRIGGER_CP_UPDATE			0x08000000
#define AFTER_TRIGGER_TUP_BITS			0x38000000
typedef struct AfterTriggerSharedData *AfterTriggerShared;

typedef struct AfterTriggerSharedData
{
	TriggerEvent ats_event;		/* event type indicator, see trigger.h */
	/*
	 *
	 * 事件类型指示，见 trigger.h
	 */
	Oid			ats_tgoid;		/* the trigger's ID */
	/*
	 *
	 * 触发器的 ID
	 */
	Oid			ats_relid;		/* the relation it's on */
	/*
	 *
	 * 它所在的关系
	 */
	Oid			ats_rolid;		/* role to execute the trigger */
	/*
	 *
	 * 执行该触发器的角色
	 */
	CommandId	ats_firing_id;	/* ID for firing cycle */
	/*
	 *
	 * 触发周期的 ID
	 */
	struct AfterTriggersTableData *ats_table;	/* transition table access */
	/*
	 *
	 * transition table 访问
	 */
	Bitmapset  *ats_modifiedcols;	/* modified columns */
	/*
	 *
	 * 被修改的列
	 */
} AfterTriggerSharedData;

typedef struct AfterTriggerEventData *AfterTriggerEvent;

typedef struct AfterTriggerEventData
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	/*
	 *
	 * 状态位以及指向共享数据的偏移
	 */
	ItemPointerData ate_ctid1;	/* inserted, deleted, or old updated tuple */
	/*
	 *
	 * 插入的、删除的或更新前的旧元组
	 */
	ItemPointerData ate_ctid2;	/* new updated tuple */
	/*
	 *
	 * 更新后的新元组
	 */

	/*
	 * During a cross-partition update of a partitioned table, we also store
	 * the OIDs of source and destination partitions that are needed to fetch
	 * the old (ctid1) and the new tuple (ctid2) from, respectively.
	 *
	 * 分区表跨分区更新期间，还保存源分区与目标分区的 OID，
	 * 以便分别取出旧元组（ctid1）和新元组（ctid2）。
	 */
	Oid			ate_src_part;
	Oid			ate_dst_part;
} AfterTriggerEventData;

/* AfterTriggerEventData, minus ate_src_part, ate_dst_part */
/*
 *
 * AfterTriggerEventData，
 * 去掉 ate_src_part 与 ate_dst_part
 */
typedef struct AfterTriggerEventDataNoOids
{
	TriggerFlags ate_flags;
	ItemPointerData ate_ctid1;
	ItemPointerData ate_ctid2;
}			AfterTriggerEventDataNoOids;

/* AfterTriggerEventData, minus ate_*_part and ate_ctid2 */
/*
 *
 * AfterTriggerEventData，
 * 去掉 ate_*_part 与 ate_ctid2
 */
typedef struct AfterTriggerEventDataOneCtid
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	/*
	 *
	 * 状态位以及指向共享数据的偏移
	 */
	ItemPointerData ate_ctid1;	/* inserted, deleted, or old updated tuple */
	/*
	 *
	 * 插入的、删除的或更新前的旧元组
	 */
}			AfterTriggerEventDataOneCtid;

/* AfterTriggerEventData, minus ate_*_part, ate_ctid1 and ate_ctid2 */
/*
 *
 * AfterTriggerEventData，
 * 去掉 ate_*_part、ate_ctid1 与 ate_ctid2
 */
typedef struct AfterTriggerEventDataZeroCtids
{
	TriggerFlags ate_flags;		/* status bits and offset to shared data */
	/*
	 *
	 * 状态位以及指向共享数据的偏移
	 */
}			AfterTriggerEventDataZeroCtids;

#define SizeofTriggerEvent(evt) \
	(((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_CP_UPDATE ? \
	 sizeof(AfterTriggerEventData) : \
	 (((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ? \
	  sizeof(AfterTriggerEventDataNoOids) : \
	  (((evt)->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_1CTID ? \
	   sizeof(AfterTriggerEventDataOneCtid) : \
	   sizeof(AfterTriggerEventDataZeroCtids))))

#define GetTriggerSharedData(evt) \
	((AfterTriggerShared) ((char *) (evt) + ((evt)->ate_flags & AFTER_TRIGGER_OFFSET)))

/*
 * To avoid palloc overhead, we keep trigger events in arrays in successively-
 * larger chunks (a slightly more sophisticated version of an expansible
 * array).  The space between CHUNK_DATA_START and freeptr is occupied by
 * AfterTriggerEventData records; the space between endfree and endptr is
 * occupied by AfterTriggerSharedData records.
 *
 * 为避免 palloc 开销，把触发器事件放在逐级变大的数组块中
 * （一种稍复杂的可扩展数组）。CHUNK_DATA_START 与 freeptr
 * 之间是
 * AfterTriggerEventData 记录；
 * endfree 与 endptr 之间是 AfterTriggerSharedDat
 * a 记录。
 */
typedef struct AfterTriggerEventChunk
{
	struct AfterTriggerEventChunk *next;	/* list link */
	/*
	 *
	 * 链表链接
	 */
	char	   *freeptr;		/* start of free space in chunk */
	/*
	 *
	 * 块中空闲空间的起点
	 */
	char	   *endfree;		/* end of free space in chunk */
	/*
	 *
	 * 块中空闲空间的终点
	 */
	char	   *endptr;			/* end of chunk */
	/*
	 *
	 * 块的末尾
	 */
	/* event data follows here */
	/*
	 *
	 * 事件数据接在后面
	 */
} AfterTriggerEventChunk;

#define CHUNK_DATA_START(cptr) ((char *) (cptr) + MAXALIGN(sizeof(AfterTriggerEventChunk)))

/* A list of events */
/*
 *
 * 事件列表
 */
typedef struct AfterTriggerEventList
{
	AfterTriggerEventChunk *head;
	AfterTriggerEventChunk *tail;
	char	   *tailfree;		/* freeptr of tail chunk */
	/*
	 *
	 * 尾块的 freeptr
	 */
} AfterTriggerEventList;

/* Macros to help in iterating over a list of events */
/*
 *
 * 帮助遍历事件列表的宏
 */
#define for_each_chunk(cptr, evtlist) \
	for (cptr = (evtlist).head; cptr != NULL; cptr = cptr->next)
#define for_each_event(eptr, cptr) \
	for (eptr = (AfterTriggerEvent) CHUNK_DATA_START(cptr); \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))
/* Use this if no special per-chunk processing is needed */
/*
 *
 * 若不需要对每个块做特殊处理，使用这个
 */
#define for_each_event_chunk(eptr, cptr, evtlist) \
	for_each_chunk(cptr, evtlist) for_each_event(eptr, cptr)

/* Macros for iterating from a start point that might not be list start */
/*
 *
 * 从可能不是列表开头的位置开始迭代的宏
 */
#define for_each_chunk_from(cptr) \
	for (; cptr != NULL; cptr = cptr->next)
#define for_each_event_from(eptr, cptr) \
	for (; \
		 (char *) eptr < (cptr)->freeptr; \
		 eptr = (AfterTriggerEvent) (((char *) eptr) + SizeofTriggerEvent(eptr)))


/*
 * All per-transaction data for the AFTER TRIGGERS module.
 *
 * AFTER TRIGGERS 模块的全部每事务数据。
 *
 * AfterTriggersData has the following fields:
 *
 * AfterTriggersData 有下列字段：
 *
 * firing_counter is incremented for each call of afterTriggerInvokeEvents.
 * We mark firable events with the current firing cycle's ID so that we can
 * tell which ones to work on.  This ensures sane behavior if a trigger
 * function chooses to do SET CONSTRAINTS: the inner SET CONSTRAINTS will
 * only fire those events that weren't already scheduled for firing.
 *
 * 每次调用 afterTriggerInvokeEvents 时
 * firing_counter 递增。
 * 我们用当前触发周期的 ID 标记可触发的事件，以便知道要处理哪些。
 * 这样当触发器函数执行 SET CONSTRAINTS 时行为仍然合理：
 * 内层 SET CONSTRAINTS 只会触发那些尚未被安排触发的事件。
 *
 * state keeps track of the transaction-local effects of SET CONSTRAINTS.
 * This is saved and restored across failed subtransactions.
 *
 * state 跟踪 SET CONSTRAINTS 的事务局部效果。
 * 失败的子事务会保存并恢复它。
 *
 * events is the current list of deferred events.  This is global across
 * all subtransactions of the current transaction.  In a subtransaction
 * abort, we know that the events added by the subtransaction are at the
 * end of the list, so it is relatively easy to discard them.  The event
 * list chunks themselves are stored in event_cxt.
 *
 * events 是当前的延迟事件列表。它在当前事务的所有子事务之间全局共享。
 * 子事务中止时，我们知道该子事务加入的事件在列表末尾，因此相对容易丢弃。
 * 事件列表的块本身存放在 event_cxt 中。
 *
 * query_depth is the current depth of nested AfterTriggerBeginQuery calls
 * (-1 when the stack is empty).
 *
 * query_depth 是嵌套 AfterTriggerBeginQuery
 * 调用的当前深度（栈空时为 -1）。
 *
 * query_stack[query_depth] is the per-query-level data, including these fields:
 *
 * query_stack[query_depth] 是每查询级别的数据，包含下列字段：
 *
 * events is a list of AFTER trigger events queued by the current query.
 * None of these are valid until the matching AfterTriggerEndQuery call
 * occurs.  At that point we fire immediate-mode triggers, and append any
 * deferred events to the main events list.
 *
 * events 是当前查询入队的 AFTER 触发器事件列表。
 * 在对应的 AfterTriggerEndQuery 调用发生之前，它们都无效。
 * 届时触发立即模式的触发器，并把延迟事件追加到主事件列表。
 *
 * fdw_tuplestore is a tuplestore containing the foreign-table tuples
 * needed by events queued by the current query.  (Note: we use just one
 * tuplestore even though more than one foreign table might be involved.
 * This is okay because tuplestores don't really care what's in the tuples
 * they store; but it's possible that someday it'd break.)
 *
 * fdw_tuplestore 是存放当前查询所入队事件需要的外部表元组的
 * tuplestore。
 * （注意：即使涉及多张外部表，也只用一个 tuplestore。
 * 这是可以的，因为 tuplestore 并不真正关心所存元组的内容；
 * 但将来有可能因此出问题。）
 *
 * tables is a List of AfterTriggersTableData structs for target tables
 * of the current query (see below).
 *
 * tables 是当前查询目标表的 AfterTriggersTableData 结构
 * List（见下文）。
 *
 * maxquerydepth is just the allocated length of query_stack.
 *
 * maxquerydepth 只是 query_stack 的已分配长度。
 *
 * trans_stack holds per-subtransaction data, including these fields:
 *
 * trans_stack 保存每子事务数据，包含下列字段：
 *
 * state is NULL or a pointer to a saved copy of the SET CONSTRAINTS
 * state data.  Each subtransaction level that modifies that state first
 * saves a copy, which we use to restore the state if we abort.
 *
 * state 为 NULL，或指向已保存的 SET CONSTRAINTS 状态副本。
 * 修改该状态的每个子事务级别会先保存一份，中止时用它恢复。
 *
 * events is a copy of the events head/tail pointers,
 * which we use to restore those values during subtransaction abort.
 *
 * events 是事件头/尾指针的副本，子事务中止时用来恢复这些值。
 *
 * query_depth is the subtransaction-start-time value of query_depth,
 * which we similarly use to clean up at subtransaction abort.
 *
 * query_depth 是子事务开始时的 query_depth，
 * 同样用于子事务中止时的清理。
 *
 * firing_counter is the subtransaction-start-time value of firing_counter.
 * We use this to recognize which deferred triggers were fired (or marked
 * for firing) within an aborted subtransaction.
 *
 * firing_counter 是子事务开始时的 firing_counter。
 * 用来识别哪些延迟触发器是在被中止的子事务内触发（或被标记为触发）的。
 *
 * We use GetCurrentTransactionNestLevel() to determine the correct array
 * index in trans_stack.  maxtransdepth is the number of allocated entries in
 * trans_stack.  (By not keeping our own stack pointer, we can avoid trouble
 * in cases where errors during subxact abort cause multiple invocations
 * of AfterTriggerEndSubXact() at the same nesting depth.)
 *
 * 用 GetCurrentTransactionNestLevel() 确定
 * trans_stack 中的正确下标。
 * maxtransdepth 是 trans_stack 已分配的项数。
 * （不自己维护栈指针，可以避免子事务中止期间出错导致
 * 同一嵌套深度多次调用 AfterTriggerEndSubXact() 时出问题。）
 *
 * We create an AfterTriggersTableData struct for each target table of the
 * current query, and each operation mode (INSERT/UPDATE/DELETE), that has
 * either transition tables or statement-level triggers.  This is used to
 * hold the relevant transition tables, as well as info tracking whether
 * we already queued the statement triggers.  (We use that info to prevent
 * firing the same statement triggers more than once per statement, or really
 * once per transition table set.)  These structs, along with the transition
 * table tuplestores, live in the (sub)transaction's CurTransactionContext.
 * That's sufficient lifespan because we don't allow transition tables to be
 * used by deferrable triggers, so they only need to survive until
 * AfterTriggerEndQuery.
 *
 * 对当前查询中每个具有 transition table
 * 或语句级触发器的目标表及每种操作
 * （INSERT/UPDATE/DELETE），
 * 建立一个 AfterTriggersTableData。
 * 它保存相关的 transition table，
 * 以及是否已经把语句级触发器入队的信息。
 * （用该信息防止同一语句，或实际上同一组 transition table，
 * 多次触发相同的语句级触发器。）
 * 这些结构连同 transition table 的 tuplestore
 * 活在（子）事务的 CurTransactionContext 中。
 * 寿命足够，因为不允许可延迟触发器使用 transition table，
 * 它们只需存活到 AfterTriggerEndQuery。
 */
typedef struct AfterTriggersQueryData AfterTriggersQueryData;
typedef struct AfterTriggersTransData AfterTriggersTransData;
typedef struct AfterTriggersTableData AfterTriggersTableData;

typedef struct AfterTriggersData
{
	CommandId	firing_counter; /* next firing ID to assign */
	/*
	 *
	 * 下一个要分配的触发 ID
	 */
	SetConstraintState state;	/* the active S C state */
	/*
	 *
	 * 当前活动的 SET CONSTRAINT 状态
	 */
	AfterTriggerEventList events;	/* deferred-event list */
	/*
	 *
	 * 延迟事件列表
	 */
	MemoryContext event_cxt;	/* memory context for events, if any */
	/*
	 *
	 * 事件所用的内存上下文，若有
	 */

	/* per-query-level data: */
	/*
	 *
	 * 每查询级别的数据：
	 */
	AfterTriggersQueryData *query_stack;	/* array of structs shown below */
	/*
	 *
	 * 下面所示结构的数组
	 */
	int			query_depth;	/* current index in above array */
	/*
	 *
	 * 上述数组中的当前下标
	 */
	int			maxquerydepth;	/* allocated len of above array */
	/*
	 *
	 * 上述数组的已分配长度
	 */

	/* per-subtransaction-level data: */
	/*
	 *
	 * 每子事务级别的数据：
	 */
	AfterTriggersTransData *trans_stack;	/* array of structs shown below */
	/*
	 *
	 * 下面所示结构的数组
	 */
	int			maxtransdepth;	/* allocated len of above array */
	/*
	 *
	 * 上述数组的已分配长度
	 */
} AfterTriggersData;

struct AfterTriggersQueryData
{
	AfterTriggerEventList events;	/* events pending from this query */
	/*
	 *
	 * 本查询尚未处理的事件
	 */
	Tuplestorestate *fdw_tuplestore;	/* foreign tuples for said events */
	/*
	 *
	 * 上述事件所用的外部元组
	 */
	List	   *tables;			/* list of AfterTriggersTableData, see below */
	/*
	 *
	 * AfterTriggersTableData 的列表，见下文
	 */
};

struct AfterTriggersTransData
{
	/* these fields are just for resetting at subtrans abort: */
	/*
	 *
	 * 这些字段只用于在子事务中止时复位：
	 */
	SetConstraintState state;	/* saved S C state, or NULL if not yet saved */
	/*
	 *
	 * 已保存的 SET CONSTRAINT 状态，若尚未保存则为 NULL
	 */
	AfterTriggerEventList events;	/* saved list pointer */
	/*
	 *
	 * 已保存的列表指针
	 */
	int			query_depth;	/* saved query_depth */
	/*
	 *
	 * 已保存的 query_depth
	 */
	CommandId	firing_counter; /* saved firing_counter */
	/*
	 *
	 * 已保存的 firing_counter
	 */
};

struct AfterTriggersTableData
{
	/* relid + cmdType form the lookup key for these structs: */
	/*
	 *
	 * relid 与 cmdType 构成这些结构的查找键：
	 */
	Oid			relid;			/* target table's OID */
	/*
	 *
	 * 目标表的 OID
	 */
	CmdType		cmdType;		/* event type, CMD_INSERT/UPDATE/DELETE */
	/*
	 *
	 * 事件类型，CMD_INSERT/UPDATE/DELETE
	 */
	bool		closed;			/* true when no longer OK to add tuples */
	/*
	 *
	 * 为 true 时已不能再添加元组
	 */
	bool		before_trig_done;	/* did we already queue BS triggers? */
	/*
	 *
	 * 是否已经把 BEFORE STATEMENT 触发器入队？
	 */
	bool		after_trig_done;	/* did we already queue AS triggers? */
	/*
	 *
	 * 是否已经把 AFTER STATEMENT 触发器入队？
	 */
	AfterTriggerEventList after_trig_events;	/* if so, saved list pointer */
	/*
	 *
	 * 若是，则保存的列表指针
	 */

	/* "old" transition table for UPDATE/DELETE, if any */
	/*
	 *
	 * UPDATE/DELETE 的“旧”transition table，若有
	 */
	Tuplestorestate *old_tuplestore;
	/* "new" transition table for INSERT/UPDATE, if any */
	/*
	 *
	 * INSERT/UPDATE 的“新”transition table，若有
	 */
	Tuplestorestate *new_tuplestore;

	TupleTableSlot *storeslot;	/* for converting to tuplestore's format */
	/*
	 *
	 * 用于转换到 tuplestore 的格式
	 */
};

static AfterTriggersData afterTriggers;

static void AfterTriggerExecute(EState *estate,
								AfterTriggerEvent event,
								ResultRelInfo *relInfo,
								ResultRelInfo *src_relInfo,
								ResultRelInfo *dst_relInfo,
								TriggerDesc *trigdesc,
								FmgrInfo *finfo,
								Instrumentation *instr,
								MemoryContext per_tuple_context,
								TupleTableSlot *trig_tuple_slot1,
								TupleTableSlot *trig_tuple_slot2);
static AfterTriggersTableData *GetAfterTriggersTableData(Oid relid,
														 CmdType cmdType);
static TupleTableSlot *GetAfterTriggersStoreSlot(AfterTriggersTableData *table,
												 TupleDesc tupdesc);
static Tuplestorestate *GetAfterTriggersTransitionTable(int event,
														TupleTableSlot *oldslot,
														TupleTableSlot *newslot,
														TransitionCaptureState *transition_capture);
static void TransitionTableAddTuple(EState *estate,
									int event,
									TransitionCaptureState *transition_capture,
									ResultRelInfo *relinfo,
									TupleTableSlot *slot,
									TupleTableSlot *original_insert_tuple,
									Tuplestorestate *tuplestore);
static void AfterTriggerFreeQuery(AfterTriggersQueryData *qs);
static SetConstraintState SetConstraintStateCreate(int numalloc);
static SetConstraintState SetConstraintStateCopy(SetConstraintState origstate);
static SetConstraintState SetConstraintStateAddItem(SetConstraintState state,
													Oid tgoid, bool tgisdeferred);
static void cancel_prior_stmt_triggers(Oid relid, CmdType cmdType, int tgevent);


/*
 * Get the FDW tuplestore for the current trigger query level, creating it
 * if necessary.
 *
 * 取得当前触发器查询级别的 FDW tuplestore，必要时创建它。
 */
static Tuplestorestate *
GetCurrentFDWTuplestore(void)
{
	Tuplestorestate *ret;

	ret = afterTriggers.query_stack[afterTriggers.query_depth].fdw_tuplestore;
	if (ret == NULL)
	{
		MemoryContext oldcxt;
		ResourceOwner saveResourceOwner;

		/*
		 * Make the tuplestore valid until end of subtransaction.  We really
		 * only need it until AfterTriggerEndQuery().
		 *
		 * 使 tuplestore 一直有效到子事务结束。
		 * 实际上只需要到 AfterTriggerEndQuery()。
		 */
		oldcxt = MemoryContextSwitchTo(CurTransactionContext);
		saveResourceOwner = CurrentResourceOwner;
		CurrentResourceOwner = CurTransactionResourceOwner;

		ret = tuplestore_begin_heap(false, false, work_mem);

		CurrentResourceOwner = saveResourceOwner;
		MemoryContextSwitchTo(oldcxt);

		afterTriggers.query_stack[afterTriggers.query_depth].fdw_tuplestore = ret;
	}

	return ret;
}

/* ----------
 * afterTriggerCheckState()
 *
 * afterTriggerCheckState()
 *
 *	Returns true if the trigger event is actually in state DEFERRED.
 * ----------
 *
 * 若该触发事件实际上处于 DEFERRED 状态，则返回 true。
 */
static bool
afterTriggerCheckState(AfterTriggerShared evtshared)
{
	Oid			tgoid = evtshared->ats_tgoid;
	SetConstraintState state = afterTriggers.state;
	int			i;

	/*
	 * For not-deferrable triggers (i.e. normal AFTER ROW triggers and
	 * constraints declared NOT DEFERRABLE), the state is always false.
	 *
	 * 对不可延迟的触发器（即普通 AFTER ROW 触发器以及声明为 NOT
	 * DEFERRABLE 的约束），
	 * 状态始终为 false。
	 */
	if ((evtshared->ats_event & AFTER_TRIGGER_DEFERRABLE) == 0)
		return false;

	/*
	 * If constraint state exists, SET CONSTRAINTS might have been executed
	 * either for this trigger or for all triggers.
	 *
	 * 若存在约束状态，则可能已对本触发器或全部触发器执行过 SET CONSTRAINTS。
	 */
	if (state != NULL)
	{
		/* Check for SET CONSTRAINTS for this specific trigger. */
		/*
		 *
		 * 检查是否对本触发器执行了 SET CONSTRAINTS。
		 */
		for (i = 0; i < state->numstates; i++)
		{
			if (state->trigstates[i].sct_tgoid == tgoid)
				return state->trigstates[i].sct_tgisdeferred;
		}

		/* Check for SET CONSTRAINTS ALL. */
		/*
		 *
		 * 检查 SET CONSTRAINTS ALL。
		 */
		if (state->all_isset)
			return state->all_isdeferred;
	}

	/*
	 * Otherwise return the default state for the trigger.
	 *
	 * 否则返回该触发器的默认状态。
	 */
	return ((evtshared->ats_event & AFTER_TRIGGER_INITDEFERRED) != 0);
}

/* ----------
 * afterTriggerCopyBitmap()
 *
 * afterTriggerCopyBitmap()
 *
 * Copy bitmap into AfterTriggerEvents memory context, which is where the after
 * trigger events are kept.
 * ----------
 *
 * 把位图复制到 AfterTriggerEvents 内存上下文，
 * AFTER 触发器事件就保存在那里。
 */
static Bitmapset *
afterTriggerCopyBitmap(Bitmapset *src)
{
	Bitmapset  *dst;
	MemoryContext oldcxt;

	if (src == NULL)
		return NULL;

	oldcxt = MemoryContextSwitchTo(afterTriggers.event_cxt);

	dst = bms_copy(src);

	MemoryContextSwitchTo(oldcxt);

	return dst;
}

/* ----------
 * afterTriggerAddEvent()
 *
 * afterTriggerAddEvent()
 *
 *	Add a new trigger event to the specified queue.
 *	The passed-in event data is copied.
 * ----------
 *
 * 把一个新的触发器事件加入指定队列。传入的事件数据会被复制。
 */
static void
afterTriggerAddEvent(AfterTriggerEventList *events,
					 AfterTriggerEvent event, AfterTriggerShared evtshared)
{
	Size		eventsize = SizeofTriggerEvent(event);
	Size		needed = eventsize + sizeof(AfterTriggerSharedData);
	AfterTriggerEventChunk *chunk;
	AfterTriggerShared newshared;
	AfterTriggerEvent newevent;

	/*
	 * If empty list or not enough room in the tail chunk, make a new chunk.
	 * We assume here that a new shared record will always be needed.
	 *
	 * 若列表为空或尾块空间不足，则新建一块。这里假定总是需要一条新的共享记录。
	 */
	chunk = events->tail;
	if (chunk == NULL ||
		chunk->endfree - chunk->freeptr < needed)
	{
		Size		chunksize;

		/* Create event context if we didn't already */
		/*
		 *
		 * 若还没有事件上下文，则创建它
		 */
		if (afterTriggers.event_cxt == NULL)
			afterTriggers.event_cxt =
				AllocSetContextCreate(TopTransactionContext,
									  "AfterTriggerEvents",
									  ALLOCSET_DEFAULT_SIZES);

		/*
		 * Chunk size starts at 1KB and is allowed to increase up to 1MB.
		 * These numbers are fairly arbitrary, though there is a hard limit at
		 * AFTER_TRIGGER_OFFSET; else we couldn't link event records to their
		 * shared records using the available space in ate_flags.  Another
		 * constraint is that if the chunk size gets too huge, the search loop
		 * below would get slow given a (not too common) usage pattern with
		 * many distinct event types in a chunk.  Therefore, we double the
		 * preceding chunk size only if there weren't too many shared records
		 * in the preceding chunk; otherwise we halve it.  This gives us some
		 * ability to adapt to the actual usage pattern of the current query
		 * while still having large chunk sizes in typical usage.  All chunk
		 * sizes used should be MAXALIGN multiples, to ensure that the shared
		 * records will be aligned safely.
		 *
		 * 块大小从 1KB 起，最多可增到 1MB。这些数字相当任意，
		 * 但有硬上限 AFTER_TRIGGER_OFFSET；
		 * 否则无法用 ate_flags 中的可用空间把事件记录链接到共享记录。
		 * 另一约束是块过大时，若一块里有很多不同事件类型（不太常见），下面的搜索循环会变慢。
		 * 因此只有前一块的共享记录不太多时才把块大小加倍，否则减半。
		 * 这样既能适应当前查询的实际使用方式，又能在典型情况下使用较大的块。
		 * 所用块大小都应是 MAXALIGN 的倍数，以保证共享记录安全对齐。
		 */
#define MIN_CHUNK_SIZE 1024
#define MAX_CHUNK_SIZE (1024*1024)

#if MAX_CHUNK_SIZE > (AFTER_TRIGGER_OFFSET+1)
#error MAX_CHUNK_SIZE must not exceed AFTER_TRIGGER_OFFSET
#endif

		if (chunk == NULL)
			chunksize = MIN_CHUNK_SIZE;
		else
		{
			/* preceding chunk size... */
			/*
			 *
			 * 前一块的大小……
			 */
			chunksize = chunk->endptr - (char *) chunk;
			/* check number of shared records in preceding chunk */
			/*
			 *
			 * 检查前一块中的共享记录数
			 */
			if ((chunk->endptr - chunk->endfree) <=
				(100 * sizeof(AfterTriggerSharedData)))
				chunksize *= 2; /* okay, double it */
				/*
				 *
				 * 可以，将其加倍
				 */
			else
				chunksize /= 2; /* too many shared records */
				/*
				 *
				 * 共享记录太多
				 */
			chunksize = Min(chunksize, MAX_CHUNK_SIZE);
		}
		chunk = MemoryContextAlloc(afterTriggers.event_cxt, chunksize);
		chunk->next = NULL;
		chunk->freeptr = CHUNK_DATA_START(chunk);
		chunk->endptr = chunk->endfree = (char *) chunk + chunksize;
		Assert(chunk->endfree - chunk->freeptr >= needed);

		if (events->tail == NULL)
		{
			Assert(events->head == NULL);
			events->head = chunk;
		}
		else
			events->tail->next = chunk;
		events->tail = chunk;
		/* events->tailfree is now out of sync, but we'll fix it below */
		/*
		 *
		 * events->tailfree 现在不同步，下面会修正
		 */
	}

	/*
	 * Try to locate a matching shared-data record already in the chunk. If
	 * none, make a new one. The search begins with the most recently added
	 * record, since newer ones are most likely to match.
	 *
	 * 尝试在块中找到已有的匹配共享数据记录。若没有则新建一条。
	 * 搜索从最近加入的记录开始，因为较新的更可能匹配。
	 */
	for (newshared = (AfterTriggerShared) chunk->endfree;
		 (char *) newshared < chunk->endptr;
		 newshared++)
	{
		/* compare fields roughly by probability of them being different */
		/*
		 *
		 * 大致按字段不同的概率来比较
		 */
		if (newshared->ats_tgoid == evtshared->ats_tgoid &&
			newshared->ats_event == evtshared->ats_event &&
			newshared->ats_firing_id == 0 &&
			newshared->ats_table == evtshared->ats_table &&
			newshared->ats_relid == evtshared->ats_relid &&
			newshared->ats_rolid == evtshared->ats_rolid &&
			bms_equal(newshared->ats_modifiedcols,
					  evtshared->ats_modifiedcols))
			break;
	}
	if ((char *) newshared >= chunk->endptr)
	{
		newshared = ((AfterTriggerShared) chunk->endfree) - 1;
		*newshared = *evtshared;
		/* now we must make a suitably-long-lived copy of the bitmap */
		/*
		 *
		 * 现在必须把位图复制成寿命足够长的副本
		 */
		newshared->ats_modifiedcols = afterTriggerCopyBitmap(evtshared->ats_modifiedcols);
		newshared->ats_firing_id = 0;	/* just to be sure */
		/*
		 *
		 * 只是为了确认
		 */
		chunk->endfree = (char *) newshared;
	}

	/* Insert the data */
	/*
	 *
	 * 插入数据
	 */
	newevent = (AfterTriggerEvent) chunk->freeptr;
	memcpy(newevent, event, eventsize);
	/* ... and link the new event to its shared record */
	/*
	 *
	 * ……并把新事件链接到它的共享记录
	 */
	newevent->ate_flags &= ~AFTER_TRIGGER_OFFSET;
	newevent->ate_flags |= (char *) newshared - (char *) newevent;

	chunk->freeptr += eventsize;
	events->tailfree = chunk->freeptr;
}

/* ----------
 * afterTriggerFreeEventList()
 *
 * afterTriggerFreeEventList()
 *
 *	Free all the event storage in the given list.
 * ----------
 *
 * 释放给定列表中的全部事件存储。
 */
static void
afterTriggerFreeEventList(AfterTriggerEventList *events)
{
	AfterTriggerEventChunk *chunk;

	while ((chunk = events->head) != NULL)
	{
		events->head = chunk->next;
		pfree(chunk);
	}
	events->tail = NULL;
	events->tailfree = NULL;
}

/* ----------
 * afterTriggerRestoreEventList()
 *
 * afterTriggerRestoreEventList()
 *
 *	Restore an event list to its prior length, removing all the events
 *	added since it had the value old_events.
 * ----------
 *
 * 把事件列表恢复到先前长度，删除自它具有 old_events 值以来加入的全部事件。
 */
static void
afterTriggerRestoreEventList(AfterTriggerEventList *events,
							 const AfterTriggerEventList *old_events)
{
	AfterTriggerEventChunk *chunk;
	AfterTriggerEventChunk *next_chunk;

	if (old_events->tail == NULL)
	{
		/* restoring to a completely empty state, so free everything */
		/*
		 *
		 * 恢复到完全空的状态，因此全部释放
		 */
		afterTriggerFreeEventList(events);
	}
	else
	{
		*events = *old_events;
		/* free any chunks after the last one we want to keep */
		/*
		 *
		 * 释放我们想保留的最后一块之后的所有块
		 */
		for (chunk = events->tail->next; chunk != NULL; chunk = next_chunk)
		{
			next_chunk = chunk->next;
			pfree(chunk);
		}
		/* and clean up the tail chunk to be the right length */
		/*
		 *
		 * 并把尾块清理到正确长度
		 */
		events->tail->next = NULL;
		events->tail->freeptr = events->tailfree;

		/*
		 * We don't make any effort to remove now-unused shared data records.
		 * They might still be useful, anyway.
		 *
		 * 我们不努力删除现在未使用的共享数据记录。它们可能仍然有用。
		 */
	}
}

/* ----------
 * afterTriggerDeleteHeadEventChunk()
 *
 * afterTriggerDeleteHeadEventChunk()
 *
 *	Remove the first chunk of events from the query level's event list.
 *	Keep any event list pointers elsewhere in the query level's data
 *	structures in sync.
 * ----------
 *
 * 从查询级别的事件列表中移除第一块事件。
 * 使查询级别数据结构中其他地方的事件列表指针保持同步。
 */
static void
afterTriggerDeleteHeadEventChunk(AfterTriggersQueryData *qs)
{
	AfterTriggerEventChunk *target = qs->events.head;
	ListCell   *lc;

	Assert(target && target->next);

	/*
	 * First, update any pointers in the per-table data, so that they won't be
	 * dangling.  Resetting obsoleted pointers to NULL will make
	 * cancel_prior_stmt_triggers start from the list head, which is fine.
	 *
	 * 首先更新每表数据中的指针，以免它们悬空。
	 * 把过时指针重置为 NULL 会让 cancel_prior_stmt_trigger
	 * s 从列表头开始，这没问题。
	 */
	foreach(lc, qs->tables)
	{
		AfterTriggersTableData *table = (AfterTriggersTableData *) lfirst(lc);

		if (table->after_trig_done &&
			table->after_trig_events.tail == target)
		{
			table->after_trig_events.head = NULL;
			table->after_trig_events.tail = NULL;
			table->after_trig_events.tailfree = NULL;
		}
	}

	/* Now we can flush the head chunk */
	/*
	 *
	 * 现在可以冲掉头块
	 */
	qs->events.head = target->next;
	pfree(target);
}


/* ----------
 * AfterTriggerExecute()
 *
 * AfterTriggerExecute()
 *
 *	Fetch the required tuples back from the heap and fire one
 *	single trigger function.
 *
 * 从堆中取回所需元组，并触发一个触发器函数。
 *
 *	Frequently, this will be fired many times in a row for triggers of
 *	a single relation.  Therefore, we cache the open relation and provide
 *	fmgr lookup cache space at the caller level.  (For triggers fired at
 *	the end of a query, we can even piggyback on the executor's state.)
 *
 * 常常会针对同一关系的触发器连续触发很多次。因此在调用者一级缓存打开的关系，
 * 并提供 fmgr 查找缓存空间。（对查询结束时触发的触发器，
 * 甚至可以借用执行器的状态。）
 *
 *	When fired for a cross-partition update of a partitioned table, the old
 *	tuple is fetched using 'src_relInfo' (the source leaf partition) and
 *	the new tuple using 'dst_relInfo' (the destination leaf partition), though
 *	both are converted into the root partitioned table's format before passing
 *	to the trigger function.
 *
 * 为分区表的跨分区更新触发时，旧元组用 src_relInfo（源叶子分区）取出，
 * 新元组用 dst_relInfo（目标叶子分区）取出，
 * 但传给触发器函数之前都会转换成根分区表的格式。
 *
 *	event: event currently being fired.
 *	relInfo: result relation for event.
 *	src_relInfo: source partition of a cross-partition update
 *	dst_relInfo: its destination partition
 *	trigdesc: working copy of rel's trigger info.
 *	finfo: array of fmgr lookup cache entries (one per trigger in trigdesc).
 *	instr: array of EXPLAIN ANALYZE instrumentation nodes (one per trigger),
 *		or NULL if no instrumentation is wanted.
 *	per_tuple_context: memory context to call trigger function in.
 *	trig_tuple_slot1: scratch slot for tg_trigtuple (foreign tables only)
 *	trig_tuple_slot2: scratch slot for tg_newtuple (foreign tables only)
 * ----------
 *
 * event：当前正在触发的事件。
 * relInfo：事件的结果关系。
 * src_relInfo：跨分区更新的源分区
 * dst_relInfo：其目标分区
 * trigdesc：关系触发器信息的工作副本。
 * finfo：fmgr 查找缓存项数组（trigdesc 中每个触发器一项）。
 * instr：EXPLAIN ANALYZE 插桩节点数组（每个触发器一项），
 * 若不需要插桩则为 NULL。
 * per_tuple_context：调用触发器函数的内存上下文。
 * trig_tuple_slot1：tg_trigtuple 的临时槽（仅外部表）
 * trig_tuple_slot2：tg_newtuple 的临时槽（仅外部表）
 */
static void
AfterTriggerExecute(EState *estate,
					AfterTriggerEvent event,
					ResultRelInfo *relInfo,
					ResultRelInfo *src_relInfo,
					ResultRelInfo *dst_relInfo,
					TriggerDesc *trigdesc,
					FmgrInfo *finfo, Instrumentation *instr,
					MemoryContext per_tuple_context,
					TupleTableSlot *trig_tuple_slot1,
					TupleTableSlot *trig_tuple_slot2)
{
	Relation	rel = relInfo->ri_RelationDesc;
	Relation	src_rel = src_relInfo->ri_RelationDesc;
	Relation	dst_rel = dst_relInfo->ri_RelationDesc;
	AfterTriggerShared evtshared = GetTriggerSharedData(event);
	Oid			tgoid = evtshared->ats_tgoid;
	TriggerData LocTriggerData = {0};
	Oid			save_rolid;
	int			save_sec_context;
	HeapTuple	rettuple;
	int			tgindx;
	bool		should_free_trig = false;
	bool		should_free_new = false;

	/*
	 * Locate trigger in trigdesc.  It might not be present, and in fact the
	 * trigdesc could be NULL, if the trigger was dropped since the event was
	 * queued.  In that case, silently do nothing.
	 *
	 * 在 trigdesc 中定位触发器。它可能已经不在，
	 * trigdesc 甚至可能为 NULL，
	 * 如果事件入队后触发器被删除了。此时静默地什么也不做。
	 */
	if (trigdesc == NULL)
		return;
	for (tgindx = 0; tgindx < trigdesc->numtriggers; tgindx++)
	{
		if (trigdesc->triggers[tgindx].tgoid == tgoid)
		{
			LocTriggerData.tg_trigger = &(trigdesc->triggers[tgindx]);
			break;
		}
	}
	if (LocTriggerData.tg_trigger == NULL)
		return;

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger. We want
	 * to include time spent re-fetching tuples in the trigger cost.
	 *
	 * 若正在做 EXPLAIN ANALYZE，开始把时间记到这个触发器上。
	 * 我们希望把重新取元组的时间也计入触发器代价。
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Fetch the required tuple(s).
	 *
	 * 取出所需的元组。
	 */
	switch (event->ate_flags & AFTER_TRIGGER_TUP_BITS)
	{
		case AFTER_TRIGGER_FDW_FETCH:
			{
				Tuplestorestate *fdw_tuplestore = GetCurrentFDWTuplestore();

				if (!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot1))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
					TRIGGER_EVENT_UPDATE &&
					!tuplestore_gettupleslot(fdw_tuplestore, true, false,
											 trig_tuple_slot2))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");
			}
			/* fall through */
			/*
			 *
			 * 落入下面的处理
			 */
		case AFTER_TRIGGER_FDW_REUSE:

			/*
			 * Store tuple in the slot so that tg_trigtuple does not reference
			 * tuplestore memory.  (It is formally possible for the trigger
			 * function to queue trigger events that add to the same
			 * tuplestore, which can push other tuples out of memory.)  The
			 * distinction is academic, because we start with a minimal tuple
			 * that is stored as a heap tuple, constructed in different memory
			 * context, in the slot anyway.
			 *
			 * 把元组存入槽，使 tg_trigtuple 不引用 tuplestore 内存。
			 * （形式上触发器函数可能再入队事件并写入同一个 tuplestore，
			 * 从而把其他元组挤出内存。）
			 * 这种区别其实是学术性的，因为我们一开始放入槽中的就是在不同内存上下文里
			 * 构造的、以堆元组形式存储的最小元组。
			 */
			LocTriggerData.tg_trigslot = trig_tuple_slot1;
			LocTriggerData.tg_trigtuple =
				ExecFetchSlotHeapTuple(trig_tuple_slot1, true, &should_free_trig);

			if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) ==
				TRIGGER_EVENT_UPDATE)
			{
				LocTriggerData.tg_newslot = trig_tuple_slot2;
				LocTriggerData.tg_newtuple =
					ExecFetchSlotHeapTuple(trig_tuple_slot2, true, &should_free_new);
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
			}
			break;

		default:
			if (ItemPointerIsValid(&(event->ate_ctid1)))
			{
				TupleTableSlot *src_slot = ExecGetTriggerOldSlot(estate,
																 src_relInfo);

				if (!table_tuple_fetch_row_version(src_rel,
												   &(event->ate_ctid1),
												   SnapshotAny,
												   src_slot))
					elog(ERROR, "failed to fetch tuple1 for AFTER trigger");

				/*
				 * Store the tuple fetched from the source partition into the
				 * target (root partitioned) table slot, converting if needed.
				 *
				 * 把从源分区取出的元组存入目标（根分区）表的槽，必要时做转换。
				 */
				if (src_relInfo != relInfo)
				{
					TupleConversionMap *map = ExecGetChildToRootMap(src_relInfo);

					LocTriggerData.tg_trigslot = ExecGetTriggerOldSlot(estate, relInfo);
					if (map)
					{
						execute_attr_map_slot(map->attrMap,
											  src_slot,
											  LocTriggerData.tg_trigslot);
					}
					else
						ExecCopySlot(LocTriggerData.tg_trigslot, src_slot);
				}
				else
					LocTriggerData.tg_trigslot = src_slot;
				LocTriggerData.tg_trigtuple =
					ExecFetchSlotHeapTuple(LocTriggerData.tg_trigslot, false, &should_free_trig);
			}
			else
			{
				LocTriggerData.tg_trigtuple = NULL;
			}

			/* don't touch ctid2 if not there */
			/*
			 *
			 * 若不存在 ctid2 则不要动它
			 */
			if (((event->ate_flags & AFTER_TRIGGER_TUP_BITS) == AFTER_TRIGGER_2CTID ||
				 (event->ate_flags & AFTER_TRIGGER_CP_UPDATE)) &&
				ItemPointerIsValid(&(event->ate_ctid2)))
			{
				TupleTableSlot *dst_slot = ExecGetTriggerNewSlot(estate,
																 dst_relInfo);

				if (!table_tuple_fetch_row_version(dst_rel,
												   &(event->ate_ctid2),
												   SnapshotAny,
												   dst_slot))
					elog(ERROR, "failed to fetch tuple2 for AFTER trigger");

				/*
				 * Store the tuple fetched from the destination partition into
				 * the target (root partitioned) table slot, converting if
				 * needed.
				 *
				 * 把从目标分区取出的元组存入目标（根分区）表的槽，必要时做转换。
				 */
				if (dst_relInfo != relInfo)
				{
					TupleConversionMap *map = ExecGetChildToRootMap(dst_relInfo);

					LocTriggerData.tg_newslot = ExecGetTriggerNewSlot(estate, relInfo);
					if (map)
					{
						execute_attr_map_slot(map->attrMap,
											  dst_slot,
											  LocTriggerData.tg_newslot);
					}
					else
						ExecCopySlot(LocTriggerData.tg_newslot, dst_slot);
				}
				else
					LocTriggerData.tg_newslot = dst_slot;
				LocTriggerData.tg_newtuple =
					ExecFetchSlotHeapTuple(LocTriggerData.tg_newslot, false, &should_free_new);
			}
			else
			{
				LocTriggerData.tg_newtuple = NULL;
			}
	}

	/*
	 * Set up the tuplestore information to let the trigger have access to
	 * transition tables.  When we first make a transition table available to
	 * a trigger, mark it "closed" so that it cannot change anymore.  If any
	 * additional events of the same type get queued in the current trigger
	 * query level, they'll go into new transition tables.
	 *
	 * 设置 tuplestore 信息，让触发器能访问 transition table。
	 * 第一次把某个 transition table 提供给触发器时，
	 * 把它标为 closed，使其不能再变。
	 * 若当前触发器查询级别又入队了同类型的其他事件，
	 * 它们会进入新的 transition table。
	 */
	LocTriggerData.tg_oldtable = LocTriggerData.tg_newtable = NULL;
	if (evtshared->ats_table)
	{
		if (LocTriggerData.tg_trigger->tgoldtable)
		{
			LocTriggerData.tg_oldtable = evtshared->ats_table->old_tuplestore;
			evtshared->ats_table->closed = true;
		}

		if (LocTriggerData.tg_trigger->tgnewtable)
		{
			LocTriggerData.tg_newtable = evtshared->ats_table->new_tuplestore;
			evtshared->ats_table->closed = true;
		}
	}

	/*
	 * Setup the remaining trigger information
	 *
	 * 设置其余的触发器信息
	 */
	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event =
		evtshared->ats_event & (TRIGGER_EVENT_OPMASK | TRIGGER_EVENT_ROW);
	LocTriggerData.tg_relation = rel;
	if (TRIGGER_FOR_UPDATE(LocTriggerData.tg_trigger->tgtype))
		LocTriggerData.tg_updatedcols = evtshared->ats_modifiedcols;

	MemoryContextReset(per_tuple_context);

	/*
	 * If necessary, become the role that was active when the trigger got
	 * queued.  Note that the role might have been dropped since the trigger
	 * was queued, but if that is a problem, we will get an error later.
	 * Checking here would still leave a race condition.
	 *
	 * 如有必要，切换为触发器入队时处于活动状态的角色。
	 * 注意该角色可能在入队之后已被删除，若这会出问题，稍后会报错。
	 * 在这里检查仍然会留下竞态。
	 */
	GetUserIdAndSecContext(&save_rolid, &save_sec_context);
	if (save_rolid != evtshared->ats_rolid)
		SetUserIdAndSecContext(evtshared->ats_rolid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	/*
	 * Call the trigger and throw away any possibly returned updated tuple.
	 * (Don't let ExecCallTriggerFunc measure EXPLAIN time.)
	 *
	 * 调用触发器并丢弃可能返回的更新后元组。
	 * （不要让 ExecCallTriggerFunc 计量 EXPLAIN 时间。）
	 */
	rettuple = ExecCallTriggerFunc(&LocTriggerData,
								   tgindx,
								   finfo,
								   NULL,
								   per_tuple_context);
	if (rettuple != NULL &&
		rettuple != LocTriggerData.tg_trigtuple &&
		rettuple != LocTriggerData.tg_newtuple)
		heap_freetuple(rettuple);

	/* Restore the current role if necessary */
	/*
	 *
	 * 如有必要，恢复当前角色
	 */
	if (save_rolid != evtshared->ats_rolid)
		SetUserIdAndSecContext(save_rolid, save_sec_context);

	/*
	 * Release resources
	 *
	 * 释放资源
	 */
	if (should_free_trig)
		heap_freetuple(LocTriggerData.tg_trigtuple);
	if (should_free_new)
		heap_freetuple(LocTriggerData.tg_newtuple);

	/* don't clear slots' contents if foreign table */
	/*
	 *
	 * 若是外部表，不要清除槽的内容
	 */
	if (trig_tuple_slot1 == NULL)
	{
		if (LocTriggerData.tg_trigslot)
			ExecClearTuple(LocTriggerData.tg_trigslot);
		if (LocTriggerData.tg_newslot)
			ExecClearTuple(LocTriggerData.tg_newslot);
	}

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 *
	 * 若正在做 EXPLAIN ANALYZE，停止把时间记到这个触发器上，
	 * 并计一次“返回的元组”（实际上是触发次数）。
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);
}


/*
 * afterTriggerMarkEvents()
 *
 * afterTriggerMarkEvents()
 *
 *	Scan the given event list for not yet invoked events.  Mark the ones
 *	that can be invoked now with the current firing ID.
 *
 * 扫描给定事件列表中尚未调用的事件。把现在可以调用的那些标上当前触发 ID。
 *
 *	If move_list isn't NULL, events that are not to be invoked now are
 *	transferred to move_list.
 *
 * 若 move_list 不是 NULL，把现在不应调用的事件转移到
 * move_list。
 *
 *	When immediate_only is true, do not invoke currently-deferred triggers.
 *	(This will be false only at main transaction exit.)
 *
 * immediate_only 为 true 时，不调用当前已延迟的触发器。
 * （只有在主事务退出时才会为 false。）
 *
 *	Returns true if any invokable events were found.
 *
 * 若找到任何可调用的事件，则返回 true。
 */
static bool
afterTriggerMarkEvents(AfterTriggerEventList *events,
					   AfterTriggerEventList *move_list,
					   bool immediate_only)
{
	bool		found = false;
	bool		deferred_found = false;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;

	for_each_event_chunk(event, chunk, *events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);
		bool		defer_it = false;

		if (!(event->ate_flags &
			  (AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS)))
		{
			/*
			 * This trigger hasn't been called or scheduled yet. Check if we
			 * should call it now.
			 *
			 * 这个触发器尚未被调用或安排。检查现在是否应该调用它。
			 */
			if (immediate_only && afterTriggerCheckState(evtshared))
			{
				defer_it = true;
			}
			else
			{
				/*
				 * Mark it as to be fired in this firing cycle.
				 *
				 * 把它标记为在本触发周期中触发。
				 */
				evtshared->ats_firing_id = afterTriggers.firing_counter;
				event->ate_flags |= AFTER_TRIGGER_IN_PROGRESS;
				found = true;
			}
		}

		/*
		 * If it's deferred, move it to move_list, if requested.
		 *
		 * 若它是延迟的，则按要求把它移到 move_list。
		 */
		if (defer_it && move_list != NULL)
		{
			deferred_found = true;
			/* add it to move_list */
			/*
			 *
			 * 把它加入 move_list
			 */
			afterTriggerAddEvent(move_list, event, evtshared);
			/* mark original copy "done" so we don't do it again */
			/*
			 *
			 * 把原始副本标为 done，以免再次处理
			 */
			event->ate_flags |= AFTER_TRIGGER_DONE;
		}
	}

	/*
	 * We could allow deferred triggers if, before the end of the
	 * security-restricted operation, we were to verify that a SET CONSTRAINTS
	 * ... IMMEDIATE has fired all such triggers.  For now, don't bother.
	 *
	 * 若在安全受限操作结束前，能验证 SET CONSTRAINTS ...
	 * IMMEDIATE 已触发全部这类触发器，
	 * 则可以允许延迟触发器。目前先不这么做。
	 */
	if (deferred_found && InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot fire deferred trigger within security-restricted operation")));

	return found;
}

/*
 * afterTriggerInvokeEvents()
 *
 * afterTriggerInvokeEvents()
 *
 *	Scan the given event list for events that are marked as to be fired
 *	in the current firing cycle, and fire them.
 *
 * 扫描给定事件列表中被标记为在当前触发周期触发的事件，并触发它们。
 *
 *	If estate isn't NULL, we use its result relation info to avoid repeated
 *	openings and closing of trigger target relations.  If it is NULL, we
 *	make one locally to cache the info in case there are multiple trigger
 *	events per rel.
 *
 * 若 estate 不是 NULL，用它的结果关系信息，
 * 避免反复打开和关闭触发器目标关系。
 * 若为 NULL，则在本地建一个，以便同一关系有多个触发事件时缓存该信息。
 *
 *	When delete_ok is true, it's safe to delete fully-processed events.
 *	(We are not very tense about that: we simply reset a chunk to be empty
 *	if all its events got fired.  The objective here is just to avoid useless
 *	rescanning of events when a trigger queues new events during transaction
 *	end, so it's not necessary to worry much about the case where only
 *	some events are fired.)
 *
 * delete_ok 为 true 时，可以删除已完全处理的事件。
 * （我们对此并不严格：只是在一块里的事件都已触发时把该块重置为空。
 * 目的只是避免触发器在事务结束时又入队新事件而导致无用的重复扫描，
 * 因此不必太担心只有部分事件被触发的情形。）
 *
 *	Returns true if no unfired events remain in the list (this allows us
 *	to avoid repeating afterTriggerMarkEvents).
 *
 * 若列表中已没有未触发的事件则返回 true（这样就可以避免重复
 * afterTriggerMarkEvents）。
 */
static bool
afterTriggerInvokeEvents(AfterTriggerEventList *events,
						 CommandId firing_id,
						 EState *estate,
						 bool delete_ok)
{
	bool		all_fired = true;
	AfterTriggerEventChunk *chunk;
	MemoryContext per_tuple_context;
	bool		local_estate = false;
	ResultRelInfo *rInfo = NULL;
	Relation	rel = NULL;
	TriggerDesc *trigdesc = NULL;
	FmgrInfo   *finfo = NULL;
	Instrumentation *instr = NULL;
	TupleTableSlot *slot1 = NULL,
			   *slot2 = NULL;

	/* Make a local EState if need be */
	/*
	 *
	 * 如有需要，建立一个本地 EState
	 */
	if (estate == NULL)
	{
		estate = CreateExecutorState();
		local_estate = true;
	}

	/* Make a per-tuple memory context for trigger function calls */
	/*
	 *
	 * 为触发器函数调用建立一个每元组内存上下文
	 */
	per_tuple_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AfterTriggerTupleContext",
							  ALLOCSET_DEFAULT_SIZES);

	for_each_chunk(chunk, *events)
	{
		AfterTriggerEvent event;
		bool		all_fired_in_chunk = true;

		for_each_event(event, chunk)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			/*
			 * Is it one for me to fire?
			 *
			 * 这是要由我来触发的吗？
			 */
			if ((event->ate_flags & AFTER_TRIGGER_IN_PROGRESS) &&
				evtshared->ats_firing_id == firing_id)
			{
				ResultRelInfo *src_rInfo,
						   *dst_rInfo;

				/*
				 * So let's fire it... but first, find the correct relation if
				 * this is not the same relation as before.
				 *
				 * 那就触发它……但若这不是先前同一个关系，先找到正确的关系。
				 */
				if (rel == NULL || RelationGetRelid(rel) != evtshared->ats_relid)
				{
					rInfo = ExecGetTriggerResultRel(estate, evtshared->ats_relid,
													NULL);
					rel = rInfo->ri_RelationDesc;
					/* Catch calls with insufficient relcache refcounting */
					/*
					 *
					 * 捕获 relcache 引用计数不足的调用
					 */
					Assert(!RelationHasReferenceCountZero(rel));
					trigdesc = rInfo->ri_TrigDesc;
					/* caution: trigdesc could be NULL here */
					/*
					 *
					 * 注意：这里 trigdesc 可能为 NULL
					 */
					finfo = rInfo->ri_TrigFunctions;
					instr = rInfo->ri_TrigInstrument;
					if (slot1 != NULL)
					{
						ExecDropSingleTupleTableSlot(slot1);
						ExecDropSingleTupleTableSlot(slot2);
						slot1 = slot2 = NULL;
					}
					if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
					{
						slot1 = MakeSingleTupleTableSlot(rel->rd_att,
														 &TTSOpsMinimalTuple);
						slot2 = MakeSingleTupleTableSlot(rel->rd_att,
														 &TTSOpsMinimalTuple);
					}
				}

				/*
				 * Look up source and destination partition result rels of a
				 * cross-partition update event.
				 *
				 * 查找跨分区更新事件的源分区与目标分区结果关系。
				 */
				if ((event->ate_flags & AFTER_TRIGGER_TUP_BITS) ==
					AFTER_TRIGGER_CP_UPDATE)
				{
					Assert(OidIsValid(event->ate_src_part) &&
						   OidIsValid(event->ate_dst_part));
					src_rInfo = ExecGetTriggerResultRel(estate,
														event->ate_src_part,
														rInfo);
					dst_rInfo = ExecGetTriggerResultRel(estate,
														event->ate_dst_part,
														rInfo);
				}
				else
					src_rInfo = dst_rInfo = rInfo;

				/*
				 * Fire it.  Note that the AFTER_TRIGGER_IN_PROGRESS flag is
				 * still set, so recursive examinations of the event list
				 * won't try to re-fire it.
				 *
				 * 触发它。注意 AFTER_TRIGGER_IN_PROGRESS 标志仍然设置着，
				 * 因此递归检查事件列表时不会试图再次触发它。
				 */
				AfterTriggerExecute(estate, event, rInfo,
									src_rInfo, dst_rInfo,
									trigdesc, finfo, instr,
									per_tuple_context, slot1, slot2);

				/*
				 * Mark the event as done.
				 *
				 * 把该事件标为已完成。
				 */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			else if (!(event->ate_flags & AFTER_TRIGGER_DONE))
			{
				/* something remains to be done */
				/*
				 *
				 * 还有事情要做
				 */
				all_fired = all_fired_in_chunk = false;
			}
		}

		/* Clear the chunk if delete_ok and nothing left of interest */
		/*
		 *
		 * 若 delete_ok 且已没有值得关注的内容，则清空该块
		 */
		if (delete_ok && all_fired_in_chunk)
		{
			chunk->freeptr = CHUNK_DATA_START(chunk);
			chunk->endfree = chunk->endptr;

			/*
			 * If it's last chunk, must sync event list's tailfree too.  Note
			 * that delete_ok must NOT be passed as true if there could be
			 * additional AfterTriggerEventList values pointing at this event
			 * list, since we'd fail to fix their copies of tailfree.
			 *
			 * 若是最后一块，还必须同步事件列表的 tailfree。注意：若还可能有其他
			 * AfterTriggerEventList 指向这个事件列表，
			 * 则不得把 delete_ok 传为 true，
			 * 否则无法修正它们自己的 tailfree 副本。
			 */
			if (chunk == events->tail)
				events->tailfree = chunk->freeptr;
		}
	}
	if (slot1 != NULL)
	{
		ExecDropSingleTupleTableSlot(slot1);
		ExecDropSingleTupleTableSlot(slot2);
	}

	/* Release working resources */
	/*
	 *
	 * 释放工作资源
	 */
	MemoryContextDelete(per_tuple_context);

	if (local_estate)
	{
		ExecCloseResultRelations(estate);
		ExecResetTupleTable(estate->es_tupleTable, false);
		FreeExecutorState(estate);
	}

	return all_fired;
}


/*
 * GetAfterTriggersTableData
 *
 * GetAfterTriggersTableData
 *
 * Find or create an AfterTriggersTableData struct for the specified
 * trigger event (relation + operation type).  Ignore existing structs
 * marked "closed"; we don't want to put any additional tuples into them,
 * nor change their stmt-triggers-fired state.
 *
 * 为指定的触发事件（关系 + 操作类型）查找或创建
 * AfterTriggersTableData。
 * 忽略已标为 closed 的现有结构；我们不想再往里面放元组，
 * 也不想改变其语句触发器已触发状态。
 *
 * Note: the AfterTriggersTableData list is allocated in the current
 * (sub)transaction's CurTransactionContext.  This is OK because
 * we don't need it to live past AfterTriggerEndQuery.
 *
 * 注意：AfterTriggersTableData 列表分配在当前（子）事务的
 * CurTransactionContext 中。
 * 这可以，因为我们不需要它活过 AfterTriggerEndQuery。
 */
static AfterTriggersTableData *
GetAfterTriggersTableData(Oid relid, CmdType cmdType)
{
	AfterTriggersTableData *table;
	AfterTriggersQueryData *qs;
	MemoryContext oldcxt;
	ListCell   *lc;

	/* At this level, cmdType should not be, eg, CMD_MERGE */
	/*
	 *
	 * 在这一层，cmdType 不应是例如 CMD_MERGE
	 */
	Assert(cmdType == CMD_INSERT ||
		   cmdType == CMD_UPDATE ||
		   cmdType == CMD_DELETE);

	/* Caller should have ensured query_depth is OK. */
	/*
	 *
	 * 调用者应已保证 query_depth 正确。
	 */
	Assert(afterTriggers.query_depth >= 0 &&
		   afterTriggers.query_depth < afterTriggers.maxquerydepth);
	qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	foreach(lc, qs->tables)
	{
		table = (AfterTriggersTableData *) lfirst(lc);
		if (table->relid == relid && table->cmdType == cmdType &&
			!table->closed)
			return table;
	}

	oldcxt = MemoryContextSwitchTo(CurTransactionContext);

	table = (AfterTriggersTableData *) palloc0(sizeof(AfterTriggersTableData));
	table->relid = relid;
	table->cmdType = cmdType;
	qs->tables = lappend(qs->tables, table);

	MemoryContextSwitchTo(oldcxt);

	return table;
}

/*
 * Returns a TupleTableSlot suitable for holding the tuples to be put
 * into AfterTriggersTableData's transition table tuplestores.
 *
 * 返回一个适合存放将放入 AfterTriggersTableData 的
 * transition table tuplestore 的
 * TupleTableSlot。
 */
static TupleTableSlot *
GetAfterTriggersStoreSlot(AfterTriggersTableData *table,
						  TupleDesc tupdesc)
{
	/* Create it if not already done. */
	/*
	 *
	 * 若尚未创建则创建它。
	 */
	if (!table->storeslot)
	{
		MemoryContext oldcxt;

		/*
		 * We need this slot only until AfterTriggerEndQuery, but making it
		 * last till end-of-subxact is good enough.  It'll be freed by
		 * AfterTriggerFreeQuery().  However, the passed-in tupdesc might have
		 * a different lifespan, so we'd better make a copy of that.
		 *
		 * 这个槽只需要用到 AfterTriggerEndQuery，但活到子事务结束就够了。
		 * 它会由 AfterTriggerFreeQuery() 释放。
		 * 不过传入的 tupdesc 寿命可能不同，所以最好复制一份。
		 */
		oldcxt = MemoryContextSwitchTo(CurTransactionContext);
		tupdesc = CreateTupleDescCopy(tupdesc);
		table->storeslot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);
		MemoryContextSwitchTo(oldcxt);
	}

	return table->storeslot;
}

/*
 * MakeTransitionCaptureState
 *
 * MakeTransitionCaptureState
 *
 * Make a TransitionCaptureState object for the given TriggerDesc, target
 * relation, and operation type.  The TCS object holds all the state needed
 * to decide whether to capture tuples in transition tables.
 *
 * 为给定的 TriggerDesc、目标关系和操作类型建立一个
 * TransitionCaptureState。
 * 该对象保存决定是否把元组捕获进 transition table 所需的全部状态。
 *
 * If there are no triggers in 'trigdesc' that request relevant transition
 * tables, then return NULL.
 *
 * 若 trigdesc 中没有触发器请求相关的 transition table，
 * 则返回 NULL。
 *
 * The resulting object can be passed to the ExecAR* functions.  When
 * dealing with child tables, the caller can set tcs_original_insert_tuple
 * to avoid having to reconstruct the original tuple in the root table's
 * format.
 *
 * 得到的对象可以传给 ExecAR* 函数。处理子表时，调用者可以设置
 * tcs_original_insert_tuple，以免再把原始元组重建成根表格式。
 *
 * Note that we copy the flags from a parent table into this struct (rather
 * than subsequently using the relation's TriggerDesc directly) so that we can
 * use it to control collection of transition tuples from child tables.
 *
 * 注意：我们把父表的标志复制进这个结构（而不是随后直接用关系的
 * TriggerDesc），
 * 以便用它控制从子表收集 transition 元组。
 *
 * Per SQL spec, all operations of the same kind (INSERT/UPDATE/DELETE)
 * on the same table during one query should share one transition table.
 * Therefore, the Tuplestores are owned by an AfterTriggersTableData struct
 * looked up using the table OID + CmdType, and are merely referenced by
 * the TransitionCaptureState objects we hand out to callers.
 *
 * 按 SQL 规范，一次查询中对同一表的同类操作（INSERT/UPDATE/DELE
 * TE）应共享一个 transition table。
 * 因此 Tuplestore 由按表 OID + CmdType 查到的
 * AfterTriggersTableData 拥有，
 * 我们交给调用者的 TransitionCaptureState 只是引用它们。
 */
TransitionCaptureState *
MakeTransitionCaptureState(TriggerDesc *trigdesc, Oid relid, CmdType cmdType)
{
	TransitionCaptureState *state;
	bool		need_old_upd,
				need_new_upd,
				need_old_del,
				need_new_ins;
	AfterTriggersTableData *ins_table;
	AfterTriggersTableData *upd_table;
	AfterTriggersTableData *del_table;
	MemoryContext oldcxt;
	ResourceOwner saveResourceOwner;

	if (trigdesc == NULL)
		return NULL;

	/* Detect which table(s) we need. */
	/*
	 *
	 * 判断需要哪些表。
	 */
	switch (cmdType)
	{
		case CMD_INSERT:
			need_old_upd = need_old_del = need_new_upd = false;
			need_new_ins = trigdesc->trig_insert_new_table;
			break;
		case CMD_UPDATE:
			need_old_upd = trigdesc->trig_update_old_table;
			need_new_upd = trigdesc->trig_update_new_table;
			need_old_del = need_new_ins = false;
			break;
		case CMD_DELETE:
			need_old_del = trigdesc->trig_delete_old_table;
			need_old_upd = need_new_upd = need_new_ins = false;
			break;
		case CMD_MERGE:
			need_old_upd = trigdesc->trig_update_old_table;
			need_new_upd = trigdesc->trig_update_new_table;
			need_old_del = trigdesc->trig_delete_old_table;
			need_new_ins = trigdesc->trig_insert_new_table;
			break;
		default:
			elog(ERROR, "unexpected CmdType: %d", (int) cmdType);
			/* keep compiler quiet */
			/*
			 *
			 * 避免编译器告警
			 */
			need_old_upd = need_new_upd = need_old_del = need_new_ins = false;
			break;
	}
	if (!need_old_upd && !need_new_upd && !need_new_ins && !need_old_del)
		return NULL;

	/* Check state, like AfterTriggerSaveEvent. */
	/*
	 *
	 * 检查状态，与 AfterTriggerSaveEvent 相同。
	 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "MakeTransitionCaptureState() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	/*
	 *
	 * 确保在此查询深度有足够空间记录事件。
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * Find or create AfterTriggersTableData struct(s) to hold the
	 * tuplestore(s).  If there's a matching struct but it's marked closed,
	 * ignore it; we need a newer one.
	 *
	 * 查找或创建用于存放 tuplestore 的
	 * AfterTriggersTableData。
	 * 若有匹配结构但已标为 closed，则忽略它；我们需要一个更新的。
	 *
	 * Note: MERGE must use the same AfterTriggersTableData structs as INSERT,
	 * UPDATE, and DELETE, so that any MERGE'd tuples are added to the same
	 * tuplestores as tuples from any INSERT, UPDATE, or DELETE commands
	 * running in the same top-level command (e.g., in a writable CTE).
	 *
	 * 注意：MERGE 必须与 INSERT、UPDATE、
	 * DELETE 使用相同的 AfterTriggersTableData，
	 * 这样 MERGE 的元组会进入与同一顶层命令中（例如可写 CTE 里）
	 * 任何 INSERT、UPDATE 或 DELETE 相同的 tuplestore。
	 *
	 * Note: the AfterTriggersTableData list, as well as the tuplestores, are
	 * allocated in the current (sub)transaction's CurTransactionContext, and
	 * the tuplestores are managed by the (sub)transaction's resource owner.
	 * This is sufficient lifespan because we do not allow triggers using
	 * transition tables to be deferrable; they will be fired during
	 * AfterTriggerEndQuery, after which it's okay to delete the data.
	 *
	 * 注意：AfterTriggersTableData 列表以及 tuplestore
	 * 都分配在当前（子）事务的
	 * CurTransactionContext 中，
	 * tuplestore 由该（子）事务的资源所有者管理。
	 * 寿命足够，因为不允许使用 transition table 的触发器被延迟；
	 * 它们会在 AfterTriggerEndQuery 期间触发，
	 * 之后就可以删除这些数据。
	 */
	if (need_new_ins)
		ins_table = GetAfterTriggersTableData(relid, CMD_INSERT);
	else
		ins_table = NULL;

	if (need_old_upd || need_new_upd)
		upd_table = GetAfterTriggersTableData(relid, CMD_UPDATE);
	else
		upd_table = NULL;

	if (need_old_del)
		del_table = GetAfterTriggersTableData(relid, CMD_DELETE);
	else
		del_table = NULL;

	/* Now create required tuplestore(s), if we don't have them already. */
	/*
	 *
	 * 若还没有所需的 tuplestore，现在创建它们。
	 */
	oldcxt = MemoryContextSwitchTo(CurTransactionContext);
	saveResourceOwner = CurrentResourceOwner;
	CurrentResourceOwner = CurTransactionResourceOwner;

	if (need_old_upd && upd_table->old_tuplestore == NULL)
		upd_table->old_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_new_upd && upd_table->new_tuplestore == NULL)
		upd_table->new_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_old_del && del_table->old_tuplestore == NULL)
		del_table->old_tuplestore = tuplestore_begin_heap(false, false, work_mem);
	if (need_new_ins && ins_table->new_tuplestore == NULL)
		ins_table->new_tuplestore = tuplestore_begin_heap(false, false, work_mem);

	CurrentResourceOwner = saveResourceOwner;
	MemoryContextSwitchTo(oldcxt);

	/* Now build the TransitionCaptureState struct, in caller's context */
	/*
	 *
	 * 现在在调用者的上下文中建立 TransitionCaptureState 结构
	 */
	state = (TransitionCaptureState *) palloc0(sizeof(TransitionCaptureState));
	state->tcs_delete_old_table = need_old_del;
	state->tcs_update_old_table = need_old_upd;
	state->tcs_update_new_table = need_new_upd;
	state->tcs_insert_new_table = need_new_ins;
	state->tcs_insert_private = ins_table;
	state->tcs_update_private = upd_table;
	state->tcs_delete_private = del_table;

	return state;
}


/* ----------
 * AfterTriggerBeginXact()
 *
 * AfterTriggerBeginXact()
 *
 *	Called at transaction start (either BEGIN or implicit for single
 *	statement outside of transaction block).
 * ----------
 *
 * 在事务开始时调用（BEGIN，或事务块之外单条语句的隐式开始）。
 */
void
AfterTriggerBeginXact(void)
{
	/*
	 * Initialize after-trigger state structure to empty
	 *
	 * 把 AFTER 触发器状态结构初始化为空
	 */
	afterTriggers.firing_counter = (CommandId) 1;	/* mustn't be 0 */
	/*
	 *
	 * 不得为 0
	 */
	afterTriggers.query_depth = -1;

	/*
	 * Verify that there is no leftover state remaining.  If these assertions
	 * trip, it means that AfterTriggerEndXact wasn't called or didn't clean
	 * up properly.
	 *
	 * 确认没有残留状态。若这些断言失败，说明 AfterTriggerEndXact
	 * 没被调用或没有正确清理。
	 */
	Assert(afterTriggers.state == NULL);
	Assert(afterTriggers.query_stack == NULL);
	Assert(afterTriggers.maxquerydepth == 0);
	Assert(afterTriggers.event_cxt == NULL);
	Assert(afterTriggers.events.head == NULL);
	Assert(afterTriggers.trans_stack == NULL);
	Assert(afterTriggers.maxtransdepth == 0);
}


/* ----------
 * AfterTriggerBeginQuery()
 *
 * AfterTriggerBeginQuery()
 *
 *	Called just before we start processing a single query within a
 *	transaction (or subtransaction).  Most of the real work gets deferred
 *	until somebody actually tries to queue a trigger event.
 * ----------
 *
 * 在事务（或子事务）中开始处理单条查询之前调用。
 * 大部分实际工作会推迟到真的有人试图把触发器事件入队时。
 */
void
AfterTriggerBeginQuery(void)
{
	/* Increase the query stack depth */
	/*
	 *
	 * 增加查询栈深度
	 */
	afterTriggers.query_depth++;
}


/* ----------
 * AfterTriggerEndQuery()
 *
 * AfterTriggerEndQuery()
 *
 *	Called after one query has been completely processed. At this time
 *	we invoke all AFTER IMMEDIATE trigger events queued by the query, and
 *	transfer deferred trigger events to the global deferred-trigger list.
 *
 * 一条查询完全处理完后调用。此时调用该查询入队的全部 AFTER IMMEDIATE
 * 触发器事件，
 * 并把延迟的触发器事件转移到全局延迟触发器列表。
 *
 *	Note that this must be called BEFORE closing down the executor
 *	with ExecutorEnd, because we make use of the EState's info about
 *	target relations.  Normally it is called from ExecutorFinish.
 * ----------
 *
 * 注意：必须在用 ExecutorEnd 关闭执行器之前调用，
 * 因为我们要使用 EState 中关于目标关系的信息。
 * 通常由 ExecutorFinish 调用。
 */
void
AfterTriggerEndQuery(EState *estate)
{
	AfterTriggersQueryData *qs;

	/* Must be inside a query, too */
	/*
	 *
	 * 也必须处在一条查询之内
	 */
	Assert(afterTriggers.query_depth >= 0);

	/*
	 * If we never even got as far as initializing the event stack, there
	 * certainly won't be any events, so exit quickly.
	 *
	 * 若甚至还没初始化事件栈，肯定不会有事件，因此快速退出。
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
	{
		afterTriggers.query_depth--;
		return;
	}

	/*
	 * Process all immediate-mode triggers queued by the query, and move the
	 * deferred ones to the main list of deferred events.
	 *
	 * 处理该查询入队的全部立即模式触发器，并把延迟的那些移到延迟事件主列表。
	 *
	 * Notice that we decide which ones will be fired, and put the deferred
	 * ones on the main list, before anything is actually fired.  This ensures
	 * reasonably sane behavior if a trigger function does SET CONSTRAINTS ...
	 * IMMEDIATE: all events we have decided to defer will be available for it
	 * to fire.
	 *
	 * 注意：我们先决定哪些要触发，并把延迟的放到主列表上，然后才真正触发任何东西。
	 * 这样若触发器函数执行 SET CONSTRAINTS ... IMMEDIATE，
	 * 我们已决定延迟的事件都能供它触发。
	 *
	 * We loop in case a trigger queues more events at the same query level.
	 * Ordinary trigger functions, including all PL/pgSQL trigger functions,
	 * will instead fire any triggers in a dedicated query level.  Foreign key
	 * enforcement triggers do add to the current query level, thanks to their
	 * passing fire_triggers = false to SPI_execute_snapshot().  Other
	 * C-language triggers might do likewise.
	 *
	 * 循环是因为触发器可能在同一查询级别再入队更多事件。
	 * 普通触发器函数，包括全部 PL/pgSQL 触发器函数，
	 * 会在专用查询级别触发任何触发器。
	 * 外键强制触发器会加入当前查询级别，因为它们向
	 * SPI_execute_snapshot() 传入 fire_triggers =
	 * false。
	 * 其他 C 语言触发器也可能如此。
	 *
	 * If we find no firable events, we don't have to increment
	 * firing_counter.
	 *
	 * 若找不到可触发的事件，就不必增加 firing_counter。
	 */
	qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	for (;;)
	{
		if (afterTriggerMarkEvents(&qs->events, &afterTriggers.events, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;
			AfterTriggerEventChunk *oldtail = qs->events.tail;

			if (afterTriggerInvokeEvents(&qs->events, firing_id, estate, false))
				break;			/* all fired */
				/*
				 *
				 * 已全部触发
				 */

			/*
			 * Firing a trigger could result in query_stack being repalloc'd,
			 * so we must recalculate qs after each afterTriggerInvokeEvents
			 * call.  Furthermore, it's unsafe to pass delete_ok = true here,
			 * because that could cause afterTriggerInvokeEvents to try to
			 * access qs->events after the stack has been repalloc'd.
			 *
			 * 触发一个触发器可能导致 query_stack 被 repalloc，
			 * 因此每次 afterTriggerInvokeEvents 之后必须重新计算 qs。
			 * 而且这里把 delete_ok 传为 true 是不安全的，
			 * 因为那可能让 afterTriggerInvokeEvents
			 * 在栈被 repalloc 之后还去访问 qs->events。
			 */
			qs = &afterTriggers.query_stack[afterTriggers.query_depth];

			/*
			 * We'll need to scan the events list again.  To reduce the cost
			 * of doing so, get rid of completely-fired chunks.  We know that
			 * all events were marked IN_PROGRESS or DONE at the conclusion of
			 * afterTriggerMarkEvents, so any still-interesting events must
			 * have been added after that, and so must be in the chunk that
			 * was then the tail chunk, or in later chunks.  So, zap all
			 * chunks before oldtail.  This is approximately the same set of
			 * events we would have gotten rid of by passing delete_ok = true.
			 *
			 * 需要再次扫描事件列表。为降低开销，去掉已完全触发的块。
			 * 我们知道 afterTriggerMarkEvents 结束时所有事件都被标为
			 * IN_PROGRESS 或 DONE，
			 * 因此仍然值得关注的事件一定是在那之后加入的，从而位于当时的尾块或更后面的块中。
			 * 所以清掉 oldtail 之前的所有块。这大约就是传入 delete_ok =
			 * true 时会丢掉的那组事件。
			 */
			Assert(oldtail != NULL);
			while (qs->events.head != oldtail)
				afterTriggerDeleteHeadEventChunk(qs);
		}
		else
			break;
	}

	/* Release query-level-local storage, including tuplestores if any */
	/*
	 *
	 * 释放查询级别的局部存储，包括 tuplestore（若有）
	 */
	AfterTriggerFreeQuery(&afterTriggers.query_stack[afterTriggers.query_depth]);

	afterTriggers.query_depth--;
}


/*
 * AfterTriggerFreeQuery
 *	Release subsidiary storage for a trigger query level.
 *	This includes closing down tuplestores.
 *	Note: it's important for this to be safe if interrupted by an error
 *	and then called again for the same query level.
 *
 * AfterTriggerFreeQuery：释放一个触发器查询级别的附属存储。
 * 这包括关闭 tuplestore。
 * 注意：若被错误打断后对同一查询级别再次调用，必须仍然安全。
 */
static void
AfterTriggerFreeQuery(AfterTriggersQueryData *qs)
{
	Tuplestorestate *ts;
	List	   *tables;
	ListCell   *lc;

	/* Drop the trigger events */
	/*
	 *
	 * 丢弃触发器事件
	 */
	afterTriggerFreeEventList(&qs->events);

	/* Drop FDW tuplestore if any */
	/*
	 *
	 * 若有 FDW tuplestore 则丢弃
	 */
	ts = qs->fdw_tuplestore;
	qs->fdw_tuplestore = NULL;
	if (ts)
		tuplestore_end(ts);

	/* Release per-table subsidiary storage */
	/*
	 *
	 * 释放每表的附属存储
	 */
	tables = qs->tables;
	foreach(lc, tables)
	{
		AfterTriggersTableData *table = (AfterTriggersTableData *) lfirst(lc);

		ts = table->old_tuplestore;
		table->old_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		ts = table->new_tuplestore;
		table->new_tuplestore = NULL;
		if (ts)
			tuplestore_end(ts);
		if (table->storeslot)
		{
			TupleTableSlot *slot = table->storeslot;

			table->storeslot = NULL;
			ExecDropSingleTupleTableSlot(slot);
		}
	}

	/*
	 * Now free the AfterTriggersTableData structs and list cells.  Reset list
	 * pointer first; if list_free_deep somehow gets an error, better to leak
	 * that storage than have an infinite loop.
	 *
	 * 现在释放 AfterTriggersTableData 结构和列表单元。
	 * 先把列表指针重置；
	 * 若 list_free_deep 不知为何出错，宁可泄漏这块存储，也不要无限循环。
	 */
	qs->tables = NIL;
	list_free_deep(tables);
}


/* ----------
 * AfterTriggerFireDeferred()
 *
 * AfterTriggerFireDeferred()
 *
 *	Called just before the current transaction is committed. At this
 *	time we invoke all pending DEFERRED triggers.
 *
 * 在当前事务即将提交前调用。此时调用所有尚未处理的 DEFERRED 触发器。
 *
 *	It is possible for other modules to queue additional deferred triggers
 *	during pre-commit processing; therefore xact.c may have to call this
 *	multiple times.
 * ----------
 *
 * 预提交处理期间其他模块可能再入队额外的延迟触发器；
 * 因此 xact.c 可能要多次调用本函数。
 */
void
AfterTriggerFireDeferred(void)
{
	AfterTriggerEventList *events;
	bool		snap_pushed = false;

	/* Must not be inside a query */
	/*
	 *
	 * 不得处在一条查询之内
	 */
	Assert(afterTriggers.query_depth == -1);

	/*
	 * If there are any triggers to fire, make sure we have set a snapshot for
	 * them to use.  (Since PortalRunUtility doesn't set a snap for COMMIT, we
	 * can't assume ActiveSnapshot is valid on entry.)
	 *
	 * 若有要触发的触发器，确保已为它们设置快照。
	 * （PortalRunUtility 不为 COMMIT 设置快照，
	 * 因此不能假定进入时 ActiveSnapshot 有效。）
	 */
	events = &afterTriggers.events;
	if (events->head != NULL)
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snap_pushed = true;
	}

	/*
	 * Run all the remaining triggers.  Loop until they are all gone, in case
	 * some trigger queues more for us to do.
	 *
	 * 运行所有剩余的触发器。循环直到它们全部消失，以防某个触发器又为我们排入更多工作。
	 */
	while (afterTriggerMarkEvents(events, NULL, false))
	{
		CommandId	firing_id = afterTriggers.firing_counter++;

		if (afterTriggerInvokeEvents(events, firing_id, NULL, true))
			break;				/* all fired */
			/*
			 *
			 * 已全部触发
			 */
	}

	/*
	 * We don't bother freeing the event list, since it will go away anyway
	 * (and more efficiently than via pfree) in AfterTriggerEndXact.
	 *
	 * 我们不费力释放事件列表，因为它反正会在 AfterTriggerEndXact
	 * 中消失
	 * （而且比 pfree 更高效）。
	 */

	if (snap_pushed)
		PopActiveSnapshot();
}


/* ----------
 * AfterTriggerEndXact()
 *
 * AfterTriggerEndXact()
 *
 *	The current transaction is finishing.
 *
 * 当前事务正在结束。
 *
 *	Any unfired triggers are canceled so we simply throw
 *	away anything we know.
 *
 * 任何未触发的触发器都被取消，因此我们直接丢掉所知的一切。
 *
 *	Note: it is possible for this to be called repeatedly in case of
 *	error during transaction abort; therefore, do not complain if
 *	already closed down.
 * ----------
 *
 * 注意：事务中止期间若出错，本函数可能被重复调用；因此已经关闭时不要抱怨。
 */
void
AfterTriggerEndXact(bool isCommit)
{
	/*
	 * Forget the pending-events list.
	 *
	 * 忘掉待处理事件列表。
	 *
	 * Since all the info is in TopTransactionContext or children thereof, we
	 * don't really need to do anything to reclaim memory.  However, the
	 * pending-events list could be large, and so it's useful to discard it as
	 * soon as possible --- especially if we are aborting because we ran out
	 * of memory for the list!
	 *
	 * 由于全部信息都在 TopTransactionContext 或其子上下文中，
	 * 其实不必做任何事来回收内存。
	 * 但待处理事件列表可能很大，因此尽早丢掉它是有用的，尤其是因为列表耗尽内存而中止时。
	 */
	if (afterTriggers.event_cxt)
	{
		MemoryContextDelete(afterTriggers.event_cxt);
		afterTriggers.event_cxt = NULL;
		afterTriggers.events.head = NULL;
		afterTriggers.events.tail = NULL;
		afterTriggers.events.tailfree = NULL;
	}

	/*
	 * Forget any subtransaction state as well.  Since this can't be very
	 * large, we let the eventual reset of TopTransactionContext free the
	 * memory instead of doing it here.
	 *
	 * 也忘掉任何子事务状态。它不会很大，我们让最终重置
	 * TopTransactionContext 来释放内存，而不是在这里做。
	 */
	afterTriggers.trans_stack = NULL;
	afterTriggers.maxtransdepth = 0;


	/*
	 * Forget the query stack and constraint-related state information.  As
	 * with the subtransaction state information, we don't bother freeing the
	 * memory here.
	 *
	 * 忘掉查询栈和与约束相关的状态信息。与子事务状态信息一样，这里不费力释放内存。
	 */
	afterTriggers.query_stack = NULL;
	afterTriggers.maxquerydepth = 0;
	afterTriggers.state = NULL;

	/* No more afterTriggers manipulation until next transaction starts. */
	/*
	 *
	 * 在下一个事务开始之前，不再操作 afterTriggers。
	 */
	afterTriggers.query_depth = -1;
}

/*
 * AfterTriggerBeginSubXact()
 *
 * AfterTriggerBeginSubXact()
 *
 *	Start a subtransaction.
 *
 * 开始一个子事务。
 */
void
AfterTriggerBeginSubXact(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Allocate more space in the trans_stack if needed.  (Note: because the
	 * minimum nest level of a subtransaction is 2, we waste the first couple
	 * entries of the array; not worth the notational effort to avoid it.)
	 *
	 * 如有需要，为 trans_stack 分配更多空间。
	 * （注意：子事务的最小嵌套级别是 2，因此数组开头几项会被浪费；
	 * 不值得为避免它而增加记号上的麻烦。）
	 */
	while (my_level >= afterTriggers.maxtransdepth)
	{
		if (afterTriggers.maxtransdepth == 0)
		{
			/* Arbitrarily initialize for max of 8 subtransaction levels */
			/*
			 *
			 * 任意地按最多 8 层子事务来初始化
			 */
			afterTriggers.trans_stack = (AfterTriggersTransData *)
				MemoryContextAlloc(TopTransactionContext,
								   8 * sizeof(AfterTriggersTransData));
			afterTriggers.maxtransdepth = 8;
		}
		else
		{
			/* repalloc will keep the stack in the same context */
			/*
			 *
			 * repalloc 会把栈保持在同一上下文中
			 */
			int			new_alloc = afterTriggers.maxtransdepth * 2;

			afterTriggers.trans_stack = (AfterTriggersTransData *)
				repalloc(afterTriggers.trans_stack,
						 new_alloc * sizeof(AfterTriggersTransData));
			afterTriggers.maxtransdepth = new_alloc;
		}
	}

	/*
	 * Push the current information into the stack.  The SET CONSTRAINTS state
	 * is not saved until/unless changed.  Likewise, we don't make a
	 * per-subtransaction event context until needed.
	 *
	 * 把当前信息压入栈。SET CONSTRAINTS 状态要到被改变时才保存。
	 * 同样，每子事务的事件上下文也到需要时才建立。
	 */
	afterTriggers.trans_stack[my_level].state = NULL;
	afterTriggers.trans_stack[my_level].events = afterTriggers.events;
	afterTriggers.trans_stack[my_level].query_depth = afterTriggers.query_depth;
	afterTriggers.trans_stack[my_level].firing_counter = afterTriggers.firing_counter;
}

/*
 * AfterTriggerEndSubXact()
 *
 * AfterTriggerEndSubXact()
 *
 *	The current subtransaction is ending.
 *
 * 当前子事务正在结束。
 */
void
AfterTriggerEndSubXact(bool isCommit)
{
	int			my_level = GetCurrentTransactionNestLevel();
	SetConstraintState state;
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	CommandId	subxact_firing_id;

	/*
	 * Pop the prior state if needed.
	 *
	 * 如有需要，弹出先前的状态。
	 */
	if (isCommit)
	{
		Assert(my_level < afterTriggers.maxtransdepth);
		/* If we saved a prior state, we don't need it anymore */
		/*
		 *
		 * 若保存过先前状态，现在不再需要它
		 */
		state = afterTriggers.trans_stack[my_level].state;
		if (state != NULL)
			pfree(state);
		/* this avoids double pfree if error later: */
		/*
		 *
		 * 这样若稍后出错可避免双重 pfree：
		 */
		afterTriggers.trans_stack[my_level].state = NULL;
		Assert(afterTriggers.query_depth ==
			   afterTriggers.trans_stack[my_level].query_depth);
	}
	else
	{
		/*
		 * Aborting.  It is possible subxact start failed before calling
		 * AfterTriggerBeginSubXact, in which case we mustn't risk touching
		 * trans_stack levels that aren't there.
		 *
		 * 正在中止。子事务开始可能在调用 AfterTriggerBeginSubXact
		 * 之前就失败了，
		 * 此时绝不能去碰 trans_stack 中并不存在的层级。
		 */
		if (my_level >= afterTriggers.maxtransdepth)
			return;

		/*
		 * Release query-level storage for queries being aborted, and restore
		 * query_depth to its pre-subxact value.  This assumes that a
		 * subtransaction will not add events to query levels started in a
		 * earlier transaction state.
		 *
		 * 释放正在中止的查询的查询级存储，并把 query_depth 恢复到子事务之前的值。
		 * 这里假定子事务不会向更早事务状态中开始的查询级别添加事件。
		 */
		while (afterTriggers.query_depth > afterTriggers.trans_stack[my_level].query_depth)
		{
			if (afterTriggers.query_depth < afterTriggers.maxquerydepth)
				AfterTriggerFreeQuery(&afterTriggers.query_stack[afterTriggers.query_depth]);
			afterTriggers.query_depth--;
		}
		Assert(afterTriggers.query_depth ==
			   afterTriggers.trans_stack[my_level].query_depth);

		/*
		 * Restore the global deferred-event list to its former length,
		 * discarding any events queued by the subxact.
		 *
		 * 把全局延迟事件列表恢复到原来的长度，丢弃该子事务入队的任何事件。
		 */
		afterTriggerRestoreEventList(&afterTriggers.events,
									 &afterTriggers.trans_stack[my_level].events);

		/*
		 * Restore the trigger state.  If the saved state is NULL, then this
		 * subxact didn't save it, so it doesn't need restoring.
		 *
		 * 恢复触发器状态。若保存的状态为 NULL，说明这个子事务没有保存它，因此不需要恢复。
		 */
		state = afterTriggers.trans_stack[my_level].state;
		if (state != NULL)
		{
			pfree(afterTriggers.state);
			afterTriggers.state = state;
		}
		/* this avoids double pfree if error later: */
		/*
		 *
		 * 这样若稍后出错可避免双重 pfree：
		 */
		afterTriggers.trans_stack[my_level].state = NULL;

		/*
		 * Scan for any remaining deferred events that were marked DONE or IN
		 * PROGRESS by this subxact or a child, and un-mark them. We can
		 * recognize such events because they have a firing ID greater than or
		 * equal to the firing_counter value we saved at subtransaction start.
		 * (This essentially assumes that the current subxact includes all
		 * subxacts started after it.)
		 *
		 * 扫描剩余的延迟事件，把本子事务或其子级标为 DONE 或 IN PROGRESS
		 * 的那些取消标记。
		 * 可以认出它们，因为它们的触发 ID 大于或等于子事务开始时保存的
		 * firing_counter。
		 * （这实质上假定当前子事务包含在它之后开始的所有子事务。）
		 */
		subxact_firing_id = afterTriggers.trans_stack[my_level].firing_counter;
		for_each_event_chunk(event, chunk, afterTriggers.events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags &
				(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS))
			{
				if (evtshared->ats_firing_id >= subxact_firing_id)
					event->ate_flags &=
						~(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS);
			}
		}
	}
}

/*
 * Get the transition table for the given event and depending on whether we are
 * processing the old or the new tuple.
 *
 * 按给定事件以及正在处理旧元组还是新元组，取得 transition table。
 */
static Tuplestorestate *
GetAfterTriggersTransitionTable(int event,
								TupleTableSlot *oldslot,
								TupleTableSlot *newslot,
								TransitionCaptureState *transition_capture)
{
	Tuplestorestate *tuplestore = NULL;
	bool		delete_old_table = transition_capture->tcs_delete_old_table;
	bool		update_old_table = transition_capture->tcs_update_old_table;
	bool		update_new_table = transition_capture->tcs_update_new_table;
	bool		insert_new_table = transition_capture->tcs_insert_new_table;

	/*
	 * For INSERT events NEW should be non-NULL, for DELETE events OLD should
	 * be non-NULL, whereas for UPDATE events normally both OLD and NEW are
	 * non-NULL.  But for UPDATE events fired for capturing transition tuples
	 * during UPDATE partition-key row movement, OLD is NULL when the event is
	 * for a row being inserted, whereas NEW is NULL when the event is for a
	 * row being deleted.
	 *
	 * 对 INSERT 事件 NEW 应非 NULL，
	 * 对 DELETE 事件 OLD 应非 NULL，
	 * 对 UPDATE 事件通常 OLD 与 NEW 都非 NULL。
	 * 但为在 UPDATE 分区键导致行移动时捕获 transition 元组而触发的
	 * UPDATE 事件中，
	 * 事件对应正在插入的行时 OLD 为 NULL，
	 * 对应正在删除的行时 NEW 为 NULL。
	 */
	Assert(!(event == TRIGGER_EVENT_DELETE && delete_old_table &&
			 TupIsNull(oldslot)));
	Assert(!(event == TRIGGER_EVENT_INSERT && insert_new_table &&
			 TupIsNull(newslot)));

	if (!TupIsNull(oldslot))
	{
		Assert(TupIsNull(newslot));
		if (event == TRIGGER_EVENT_DELETE && delete_old_table)
			tuplestore = transition_capture->tcs_delete_private->old_tuplestore;
		else if (event == TRIGGER_EVENT_UPDATE && update_old_table)
			tuplestore = transition_capture->tcs_update_private->old_tuplestore;
	}
	else if (!TupIsNull(newslot))
	{
		Assert(TupIsNull(oldslot));
		if (event == TRIGGER_EVENT_INSERT && insert_new_table)
			tuplestore = transition_capture->tcs_insert_private->new_tuplestore;
		else if (event == TRIGGER_EVENT_UPDATE && update_new_table)
			tuplestore = transition_capture->tcs_update_private->new_tuplestore;
	}

	return tuplestore;
}

/*
 * Add the given heap tuple to the given tuplestore, applying the conversion
 * map if necessary.
 *
 * 把给定的堆元组加入给定的 tuplestore，必要时应用转换映射。
 *
 * If original_insert_tuple is given, we can add that tuple without conversion.
 *
 * 若给出了 original_insert_tuple，可以不加转换地加入该元组。
 */
static void
TransitionTableAddTuple(EState *estate,
						int event,
						TransitionCaptureState *transition_capture,
						ResultRelInfo *relinfo,
						TupleTableSlot *slot,
						TupleTableSlot *original_insert_tuple,
						Tuplestorestate *tuplestore)
{
	TupleConversionMap *map;

	/*
	 * Nothing needs to be done if we don't have a tuplestore.
	 *
	 * 若没有 tuplestore，则无需做任何事。
	 */
	if (tuplestore == NULL)
		return;

	if (original_insert_tuple)
		tuplestore_puttupleslot(tuplestore, original_insert_tuple);
	else if ((map = ExecGetChildToRootMap(relinfo)) != NULL)
	{
		AfterTriggersTableData *table;
		TupleTableSlot *storeslot;

		switch (event)
		{
			case TRIGGER_EVENT_INSERT:
				table = transition_capture->tcs_insert_private;
				break;
			case TRIGGER_EVENT_UPDATE:
				table = transition_capture->tcs_update_private;
				break;
			case TRIGGER_EVENT_DELETE:
				table = transition_capture->tcs_delete_private;
				break;
			default:
				elog(ERROR, "invalid after-trigger event code: %d", event);
				table = NULL;	/* keep compiler quiet */
				/*
				 *
				 * 避免编译器告警
				 */
				break;
		}

		storeslot = GetAfterTriggersStoreSlot(table, map->outdesc);
		execute_attr_map_slot(map->attrMap, slot, storeslot);
		tuplestore_puttupleslot(tuplestore, storeslot);
	}
	else
		tuplestore_puttupleslot(tuplestore, slot);
}

/* ----------
 * AfterTriggerEnlargeQueryState()
 *
 * AfterTriggerEnlargeQueryState()
 *
 *	Prepare the necessary state so that we can record AFTER trigger events
 *	queued by a query.  It is allowed to have nested queries within a
 *	(sub)transaction, so we need to have separate state for each query
 *	nesting level.
 * ----------
 *
 * 准备必要的状态，以便记录一条查询入队的 AFTER 触发器事件。
 * 允许在（子）事务内嵌套查询，因此每个查询嵌套级别需要单独的状态。
 */
static void
AfterTriggerEnlargeQueryState(void)
{
	int			init_depth = afterTriggers.maxquerydepth;

	Assert(afterTriggers.query_depth >= afterTriggers.maxquerydepth);

	if (afterTriggers.maxquerydepth == 0)
	{
		int			new_alloc = Max(afterTriggers.query_depth + 1, 8);

		afterTriggers.query_stack = (AfterTriggersQueryData *)
			MemoryContextAlloc(TopTransactionContext,
							   new_alloc * sizeof(AfterTriggersQueryData));
		afterTriggers.maxquerydepth = new_alloc;
	}
	else
	{
		/* repalloc will keep the stack in the same context */
		/*
		 *
		 * repalloc 会把栈保持在同一上下文中
		 */
		int			old_alloc = afterTriggers.maxquerydepth;
		int			new_alloc = Max(afterTriggers.query_depth + 1,
									old_alloc * 2);

		afterTriggers.query_stack = (AfterTriggersQueryData *)
			repalloc(afterTriggers.query_stack,
					 new_alloc * sizeof(AfterTriggersQueryData));
		afterTriggers.maxquerydepth = new_alloc;
	}

	/* Initialize new array entries to empty */
	/*
	 *
	 * 把新的数组项初始化为空
	 */
	while (init_depth < afterTriggers.maxquerydepth)
	{
		AfterTriggersQueryData *qs = &afterTriggers.query_stack[init_depth];

		qs->events.head = NULL;
		qs->events.tail = NULL;
		qs->events.tailfree = NULL;
		qs->fdw_tuplestore = NULL;
		qs->tables = NIL;

		++init_depth;
	}
}

/*
 * Create an empty SetConstraintState with room for numalloc trigstates
 *
 * 建立一个空的 SetConstraintState，
 * 其中有容纳 numalloc 个 trigstates 的空间
 */
static SetConstraintState
SetConstraintStateCreate(int numalloc)
{
	SetConstraintState state;

	/* Behave sanely with numalloc == 0 */
	/*
	 *
	 * 在 numalloc == 0 时也行为正常
	 */
	if (numalloc <= 0)
		numalloc = 1;

	/*
	 * We assume that zeroing will correctly initialize the state values.
	 *
	 * 假定清零能正确初始化这些状态值。
	 */
	state = (SetConstraintState)
		MemoryContextAllocZero(TopTransactionContext,
							   offsetof(SetConstraintStateData, trigstates) +
							   numalloc * sizeof(SetConstraintTriggerData));

	state->numalloc = numalloc;

	return state;
}

/*
 * Copy a SetConstraintState
 *
 * 复制一份 SetConstraintState
 */
static SetConstraintState
SetConstraintStateCopy(SetConstraintState origstate)
{
	SetConstraintState state;

	state = SetConstraintStateCreate(origstate->numstates);

	state->all_isset = origstate->all_isset;
	state->all_isdeferred = origstate->all_isdeferred;
	state->numstates = origstate->numstates;
	memcpy(state->trigstates, origstate->trigstates,
		   origstate->numstates * sizeof(SetConstraintTriggerData));

	return state;
}

/*
 * Add a per-trigger item to a SetConstraintState.  Returns possibly-changed
 * pointer to the state object (it will change if we have to repalloc).
 *
 * 向 SetConstraintState 添加一个每触发器项。
 * 返回可能已改变的状态对象指针
 * （若必须 repalloc 则会改变）。
 */
static SetConstraintState
SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred)
{
	if (state->numstates >= state->numalloc)
	{
		int			newalloc = state->numalloc * 2;

		newalloc = Max(newalloc, 8);	/* in case original has size 0 */
		/*
		 *
		 * 以防原始对象大小为 0
		 */
		state = (SetConstraintState)
			repalloc(state,
					 offsetof(SetConstraintStateData, trigstates) +
					 newalloc * sizeof(SetConstraintTriggerData));
		state->numalloc = newalloc;
		Assert(state->numstates < state->numalloc);
	}

	state->trigstates[state->numstates].sct_tgoid = tgoid;
	state->trigstates[state->numstates].sct_tgisdeferred = tgisdeferred;
	state->numstates++;

	return state;
}

/* ----------
 * AfterTriggerSetState()
 *
 * AfterTriggerSetState()
 *
 *	Execute the SET CONSTRAINTS ... utility command.
 * ----------
 *
 * 执行 SET CONSTRAINTS ... 实用命令。
 */
void
AfterTriggerSetState(ConstraintsSetStmt *stmt)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* If we haven't already done so, initialize our state. */
	/*
	 *
	 * 若尚未初始化状态，则初始化。
	 */
	if (afterTriggers.state == NULL)
		afterTriggers.state = SetConstraintStateCreate(8);

	/*
	 * If in a subtransaction, and we didn't save the current state already,
	 * save it so it can be restored if the subtransaction aborts.
	 *
	 * 若在子事务中，且还没保存当前状态，则保存它，以便子事务中止时恢复。
	 */
	if (my_level > 1 &&
		afterTriggers.trans_stack[my_level].state == NULL)
	{
		afterTriggers.trans_stack[my_level].state =
			SetConstraintStateCopy(afterTriggers.state);
	}

	/*
	 * Handle SET CONSTRAINTS ALL ...
	 *
	 * 处理 SET CONSTRAINTS ALL ...
	 */
	if (stmt->constraints == NIL)
	{
		/*
		 * Forget any previous SET CONSTRAINTS commands in this transaction.
		 *
		 * 忘掉本事务中先前的 SET CONSTRAINTS 命令。
		 */
		afterTriggers.state->numstates = 0;

		/*
		 * Set the per-transaction ALL state to known.
		 *
		 * 把每事务的 ALL 状态设为已知。
		 */
		afterTriggers.state->all_isset = true;
		afterTriggers.state->all_isdeferred = stmt->deferred;
	}
	else
	{
		Relation	conrel;
		Relation	tgrel;
		List	   *conoidlist = NIL;
		List	   *tgoidlist = NIL;
		ListCell   *lc;

		/*
		 * Handle SET CONSTRAINTS constraint-name [, ...]
		 *
		 * 处理 SET CONSTRAINTS constraint-name [, ...]
		 *
		 * First, identify all the named constraints and make a list of their
		 * OIDs.  Since, unlike the SQL spec, we allow multiple constraints of
		 * the same name within a schema, the specifications are not
		 * necessarily unique.  Our strategy is to target all matching
		 * constraints within the first search-path schema that has any
		 * matches, but disregard matches in schemas beyond the first match.
		 * (This is a bit odd but it's the historical behavior.)
		 *
		 * 首先识别所有被点名的约束并列出它们的 OID。与 SQL 规范不同，
		 * 我们允许同一模式中
		 * 有多个同名约束，因此指定并不一定唯一。策略是：针对搜索路径中第一个有任何匹配的
		 * 模式里的全部匹配约束，而忽略第一个匹配之后的模式中的匹配。
		 * （这有点古怪，但是历史行为。）
		 *
		 * A constraint in a partitioned table may have corresponding
		 * constraints in the partitions.  Grab those too.
		 *
		 * 分区表中的约束可能在分区中有对应约束。把那些也抓进来。
		 */
		conrel = table_open(ConstraintRelationId, AccessShareLock);

		foreach(lc, stmt->constraints)
		{
			RangeVar   *constraint = lfirst(lc);
			bool		found;
			List	   *namespacelist;
			ListCell   *nslc;

			if (constraint->catalogname)
			{
				if (strcmp(constraint->catalogname, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
									constraint->catalogname, constraint->schemaname,
									constraint->relname)));
			}

			/*
			 * If we're given the schema name with the constraint, look only
			 * in that schema.  If given a bare constraint name, use the
			 * search path to find the first matching constraint.
			 *
			 * 若给出了约束的模式名，则只在该模式中查找。
			 * 若只给出约束名，则用搜索路径找到第一个匹配的约束。
			 */
			if (constraint->schemaname)
			{
				Oid			namespaceId = LookupExplicitNamespace(constraint->schemaname,
																  false);

				namespacelist = list_make1_oid(namespaceId);
			}
			else
			{
				namespacelist = fetch_search_path(true);
			}

			found = false;
			foreach(nslc, namespacelist)
			{
				Oid			namespaceId = lfirst_oid(nslc);
				SysScanDesc conscan;
				ScanKeyData skey[2];
				HeapTuple	tup;

				ScanKeyInit(&skey[0],
							Anum_pg_constraint_conname,
							BTEqualStrategyNumber, F_NAMEEQ,
							CStringGetDatum(constraint->relname));
				ScanKeyInit(&skey[1],
							Anum_pg_constraint_connamespace,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(namespaceId));

				conscan = systable_beginscan(conrel, ConstraintNameNspIndexId,
											 true, NULL, 2, skey);

				while (HeapTupleIsValid(tup = systable_getnext(conscan)))
				{
					Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

					if (con->condeferrable)
						conoidlist = lappend_oid(conoidlist, con->oid);
					else if (stmt->deferred)
						ereport(ERROR,
								(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								 errmsg("constraint \"%s\" is not deferrable",
										constraint->relname)));
					found = true;
				}

				systable_endscan(conscan);

				/*
				 * Once we've found a matching constraint we do not search
				 * later parts of the search path.
				 *
				 * 一旦找到匹配的约束，就不再搜索搜索路径的后面部分。
				 */
				if (found)
					break;
			}

			list_free(namespacelist);

			/*
			 * Not found ?
			 *
			 * 没找到？
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("constraint \"%s\" does not exist",
								constraint->relname)));
		}

		/*
		 * Scan for any possible descendants of the constraints.  We append
		 * whatever we find to the same list that we're scanning; this has the
		 * effect that we create new scans for those, too, so if there are
		 * further descendents, we'll also catch them.
		 *
		 * 扫描这些约束可能的后代。把找到的追加到正在扫描的同一列表；
		 * 这样也会为它们创建新的扫描，若还有更深层的后代，同样能抓住。
		 */
		foreach(lc, conoidlist)
		{
			Oid			parent = lfirst_oid(lc);
			ScanKeyData key;
			SysScanDesc scan;
			HeapTuple	tuple;

			ScanKeyInit(&key,
						Anum_pg_constraint_conparentid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(parent));

			scan = systable_beginscan(conrel, ConstraintParentIndexId, true, NULL, 1, &key);

			while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

				conoidlist = lappend_oid(conoidlist, con->oid);
			}

			systable_endscan(scan);
		}

		table_close(conrel, AccessShareLock);

		/*
		 * Now, locate the trigger(s) implementing each of these constraints,
		 * and make a list of their OIDs.
		 *
		 * 现在定位实现这些约束的触发器，并列出它们的 OID。
		 */
		tgrel = table_open(TriggerRelationId, AccessShareLock);

		foreach(lc, conoidlist)
		{
			Oid			conoid = lfirst_oid(lc);
			ScanKeyData skey;
			SysScanDesc tgscan;
			HeapTuple	htup;

			ScanKeyInit(&skey,
						Anum_pg_trigger_tgconstraint,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(conoid));

			tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
										NULL, 1, &skey);

			while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
			{
				Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);

				/*
				 * Silently skip triggers that are marked as non-deferrable in
				 * pg_trigger.  This is not an error condition, since a
				 * deferrable RI constraint may have some non-deferrable
				 * actions.
				 *
				 * 静默跳过在 pg_trigger 中标为不可延迟的触发器。这不是错误，
				 * 因为一个可延迟的 RI 约束可能有一些不可延迟的动作。
				 */
				if (pg_trigger->tgdeferrable)
					tgoidlist = lappend_oid(tgoidlist, pg_trigger->oid);
			}

			systable_endscan(tgscan);
		}

		table_close(tgrel, AccessShareLock);

		/*
		 * Now we can set the trigger states of individual triggers for this
		 * xact.
		 *
		 * 现在可以为本事务设置各个触发器的触发器状态。
		 */
		foreach(lc, tgoidlist)
		{
			Oid			tgoid = lfirst_oid(lc);
			SetConstraintState state = afterTriggers.state;
			bool		found = false;
			int			i;

			for (i = 0; i < state->numstates; i++)
			{
				if (state->trigstates[i].sct_tgoid == tgoid)
				{
					state->trigstates[i].sct_tgisdeferred = stmt->deferred;
					found = true;
					break;
				}
			}
			if (!found)
			{
				afterTriggers.state =
					SetConstraintStateAddItem(state, tgoid, stmt->deferred);
			}
		}
	}

	/*
	 * SQL99 requires that when a constraint is set to IMMEDIATE, any deferred
	 * checks against that constraint must be made when the SET CONSTRAINTS
	 * command is executed -- i.e. the effects of the SET CONSTRAINTS command
	 * apply retroactively.  We've updated the constraints state, so scan the
	 * list of previously deferred events to fire any that have now become
	 * immediate.
	 *
	 * SQL99 要求：当约束被设为 IMMEDIATE 时，
	 * 对该约束的任何延迟检查必须在执行
	 * SET CONSTRAINTS 命令时进行，也就是该命令的效果追溯生效。
	 * 我们已经更新了约束状态，因此扫描先前延迟的事件列表，触发现在已变为立即的那些。
	 *
	 * Obviously, if this was SET ... DEFERRED then it can't have converted
	 * any unfired events to immediate, so we need do nothing in that case.
	 *
	 * 显然，若这是 SET ... DEFERRED，它不可能把任何未触发事件变成立即的，
	 * 因此那种情况无需做任何事。
	 */
	if (!stmt->deferred)
	{
		AfterTriggerEventList *events = &afterTriggers.events;
		bool		snapshot_set = false;

		while (afterTriggerMarkEvents(events, NULL, true))
		{
			CommandId	firing_id = afterTriggers.firing_counter++;

			/*
			 * Make sure a snapshot has been established in case trigger
			 * functions need one.  Note that we avoid setting a snapshot if
			 * we don't find at least one trigger that has to be fired now.
			 * This is so that BEGIN; SET CONSTRAINTS ...; SET TRANSACTION
			 * ISOLATION LEVEL SERIALIZABLE; ... works properly.  (If we are
			 * at the start of a transaction it's not possible for any trigger
			 * events to be queued yet.)
			 *
			 * 确保已建立快照，以防触发器函数需要。注意：若找不到至少一个现在必须触发的触发器，
			 * 就避免设置快照。这样 BEGIN; SET CONSTRAINTS ...; SET
			 * TRANSACTION ISOLATION LEVEL SERIALIZABLE;
			 * ...
			 * 才能正常工作。（若正处于事务开始，还不可能有任何已入队的触发器事件。）
			 */
			if (!snapshot_set)
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				snapshot_set = true;
			}

			/*
			 * We can delete fired events if we are at top transaction level,
			 * but we'd better not if inside a subtransaction, since the
			 * subtransaction could later get rolled back.
			 *
			 * 若在顶层事务，可以删除已触发的事件；若在子事务内则最好不要，因为子事务以后可能回滚。
			 */
			if (afterTriggerInvokeEvents(events, firing_id, NULL,
										 !IsSubTransaction()))
				break;			/* all fired */
				/*
				 *
				 * 已全部触发
				 */
		}

		if (snapshot_set)
			PopActiveSnapshot();
	}
}

/* ----------
 * AfterTriggerPendingOnRel()
 *		Test to see if there are any pending after-trigger events for rel.
 *
 * AfterTriggerPendingOnRel()：测试该关系是否有任何尚未处理的
 * AFTER 触发器事件。
 *
 * This is used by TRUNCATE, CLUSTER, ALTER TABLE, etc to detect whether
 * it is unsafe to perform major surgery on a relation.  Note that only
 * local pending events are examined.  We assume that having exclusive lock
 * on a rel guarantees there are no unserviced events in other backends ---
 * but having a lock does not prevent there being such events in our own.
 *
 * TRUNCATE、CLUSTER、ALTER TABLE
 * 等用它来检测对关系做大手术是否不安全。
 * 注意只检查本地尚未处理的事件。我们假定对关系持有排他锁就能保证其他后端没有未处理事件，
 * 但持有锁并不能防止我们自己这边有这类事件。
 *
 * In some scenarios it'd be reasonable to remove pending events (more
 * specifically, mark them DONE by the current subxact) but without a lot
 * of knowledge of the trigger semantics we can't do this in general.
 * ----------
 *
 * 某些情况下去掉待处理事件是合理的（更具体地说，由当前子事务把它们标为 DONE），
 * 但没有大量触发器语义知识就无法一般地这样做。
 */
bool
AfterTriggerPendingOnRel(Oid relid)
{
	AfterTriggerEvent event;
	AfterTriggerEventChunk *chunk;
	int			depth;

	/* Scan queued events */
	/*
	 *
	 * 扫描已入队的事件
	 */
	for_each_event_chunk(event, chunk, afterTriggers.events)
	{
		AfterTriggerShared evtshared = GetTriggerSharedData(event);

		/*
		 * We can ignore completed events.  (Even if a DONE flag is rolled
		 * back by subxact abort, it's OK because the effects of the TRUNCATE
		 * or whatever must get rolled back too.)
		 *
		 * 可以忽略已完成的事件。（即使 DONE 标志因子事务中止而回滚也没问题，
		 * 因为 TRUNCATE 或其他操作的效果也必须一起回滚。）
		 */
		if (event->ate_flags & AFTER_TRIGGER_DONE)
			continue;

		if (evtshared->ats_relid == relid)
			return true;
	}

	/*
	 * Also scan events queued by incomplete queries.  This could only matter
	 * if TRUNCATE/etc is executed by a function or trigger within an updating
	 * query on the same relation, which is pretty perverse, but let's check.
	 *
	 * 也扫描未完成查询入队的事件。这只有在 TRUNCATE 等由函数或触发器在同一关系的
	 * 更新查询内部执行时才重要，这相当反常，但还是检查一下。
	 */
	for (depth = 0; depth <= afterTriggers.query_depth && depth < afterTriggers.maxquerydepth; depth++)
	{
		for_each_event_chunk(event, chunk, afterTriggers.query_stack[depth].events)
		{
			AfterTriggerShared evtshared = GetTriggerSharedData(event);

			if (event->ate_flags & AFTER_TRIGGER_DONE)
				continue;

			if (evtshared->ats_relid == relid)
				return true;
		}
	}

	return false;
}

/* ----------
 * AfterTriggerSaveEvent()
 *
 * AfterTriggerSaveEvent()
 *
 *	Called by ExecA[RS]...Triggers() to queue up the triggers that should
 *	be fired for an event.
 *
 * 由 ExecA[RS]...Triggers() 调用，
 * 把应为某事件触发的触发器入队。
 *
 *	NOTE: this is called whenever there are any triggers associated with
 *	the event (even if they are disabled).  This function decides which
 *	triggers actually need to be queued.  It is also called after each row,
 *	even if there are no triggers for that event, if there are any AFTER
 *	STATEMENT triggers for the statement which use transition tables, so that
 *	the transition tuplestores can be built.  Furthermore, if the transition
 *	capture is happening for UPDATEd rows being moved to another partition due
 *	to the partition-key being changed, then this function is called once when
 *	the row is deleted (to capture OLD row), and once when the row is inserted
 *	into another partition (to capture NEW row).  This is done separately because
 *	DELETE and INSERT happen on different tables.
 *
 * 注意：只要该事件关联了任何触发器（即使已禁用）就会调用本函数。
 * 本函数决定哪些触发器真正需要入队。
 * 即使该事件没有触发器，只要语句有使用 transition table 的
 * AFTER STATEMENT 触发器，
 * 也会在每一行之后调用，以便建立 transition tuplestore。
 * 另外，若因分区键改变而把被 UPDATE 的行移到另一分区时正在捕获
 * transition，
 * 则删除该行时调用一次（捕获 OLD 行），插入另一分区时再调用一次（捕获 NEW
 * 行）。
 * 分开做是因为 DELETE 与 INSERT 发生在不同的表上。
 *
 *	Transition tuplestores are built now, rather than when events are pulled
 *	off of the queue because AFTER ROW triggers are allowed to select from the
 *	transition tables for the statement.
 *
 * 现在就建立 transition tuplestore，而不是在事件出队时才建，
 * 因为 AFTER ROW 触发器允许从本语句的 transition table
 * 中查询。
 *
 *	This contains special support to queue the update events for the case where
 *	a partitioned table undergoing a cross-partition update may have foreign
 *	keys pointing into it.  Normally, a partitioned table's row triggers are
 *	not fired because the leaf partition(s) which are modified as a result of
 *	the operation on the partitioned table contain the same triggers which are
 *	fired instead. But that general scheme can cause problematic behavior with
 *	foreign key triggers during cross-partition updates, which are implemented
 *	as DELETE on the source partition followed by INSERT into the destination
 *	partition.  Specifically, firing DELETE triggers would lead to the wrong
 *	foreign key action to be enforced considering that the original command is
 *	UPDATE; in this case, this function is called with relinfo as the
 *	partitioned table, and src_partinfo and dst_partinfo referring to the
 *	source and target leaf partitions, respectively.
 *
 * 这里有特殊支持：分区表进行跨分区更新且可能有外键指向它时，为更新事件入队。
 * 通常不触发分区表的行级触发器，因为该操作实际修改的叶子分区含有相同触发器并改为触发那
 * 些。
 * 但这一通用方案在跨分区更新期间对外键触发器会造成问题。
 * 跨分区更新实现为源分区上的 DELETE
 * 加上目标分区上的 INSERT。具体地说，触发 DELETE
 * 触发器会按错误的外键动作去强制，
 * 因为原始命令是 UPDATE；此时本函数以 relinfo 为分区表被调用，
 * src_partinfo 与 dst_partinfo
 * 分别指向源叶子分区和目标叶子分区。
 *
 *	is_crosspart_update is true either when a DELETE event is fired on the
 *	source partition (which is to be ignored) or an UPDATE event is fired on
 *	the root partitioned table.
 * ----------
 *
 * is_crosspart_update 为 true 的情况，
 * 要么是在源分区上触发了应被忽略的 DELETE 事件，
 * 要么是在根分区表上触发了 UPDATE 事件。
 */
static void
AfterTriggerSaveEvent(EState *estate, ResultRelInfo *relinfo,
					  ResultRelInfo *src_partinfo,
					  ResultRelInfo *dst_partinfo,
					  int event, bool row_trigger,
					  TupleTableSlot *oldslot, TupleTableSlot *newslot,
					  List *recheckIndexes, Bitmapset *modifiedCols,
					  TransitionCaptureState *transition_capture,
					  bool is_crosspart_update)
{
	Relation	rel = relinfo->ri_RelationDesc;
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	AfterTriggerEventData new_event;
	AfterTriggerSharedData new_shared;
	char		relkind = rel->rd_rel->relkind;
	int			tgtype_event;
	int			tgtype_level;
	int			i;
	Tuplestorestate *fdw_tuplestore = NULL;

	/*
	 * Check state.  We use a normal test not Assert because it is possible to
	 * reach here in the wrong state given misconfigured RI triggers, in
	 * particular deferring a cascade action trigger.
	 *
	 * 检查状态。这里用普通测试而不是 Assert，
	 * 因为 RI 触发器配置错误时可能以错误状态到达这里，
	 * 特别是把级联动作触发器延迟了。
	 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "AfterTriggerSaveEvent() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	/*
	 *
	 * 确保在此查询深度有足够空间记录事件。
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * If the directly named relation has any triggers with transition tables,
	 * then we need to capture transition tuples.
	 *
	 * 若直接点名的关系有任何带 transition table 的触发器，
	 * 则需要捕获 transition 元组。
	 */
	if (row_trigger && transition_capture != NULL)
	{
		TupleTableSlot *original_insert_tuple = transition_capture->tcs_original_insert_tuple;

		/*
		 * Capture the old tuple in the appropriate transition table based on
		 * the event.
		 *
		 * 根据事件把旧元组捕获到合适的 transition table 中。
		 */
		if (!TupIsNull(oldslot))
		{
			Tuplestorestate *old_tuplestore;

			old_tuplestore = GetAfterTriggersTransitionTable(event,
															 oldslot,
															 NULL,
															 transition_capture);
			TransitionTableAddTuple(estate, event, transition_capture, relinfo,
									oldslot, NULL, old_tuplestore);
		}

		/*
		 * Capture the new tuple in the appropriate transition table based on
		 * the event.
		 *
		 * 根据事件把新元组捕获到合适的 transition table 中。
		 */
		if (!TupIsNull(newslot))
		{
			Tuplestorestate *new_tuplestore;

			new_tuplestore = GetAfterTriggersTransitionTable(event,
															 NULL,
															 newslot,
															 transition_capture);
			TransitionTableAddTuple(estate, event, transition_capture, relinfo,
									newslot, original_insert_tuple, new_tuplestore);
		}

		/*
		 * If transition tables are the only reason we're here, return. As
		 * mentioned above, we can also be here during update tuple routing in
		 * presence of transition tables, in which case this function is
		 * called separately for OLD and NEW, so we expect exactly one of them
		 * to be NULL.
		 *
		 * 若来这里的唯一原因是 transition table，则返回。如上所述，
		 * 在有 transition table 时的
		 * 更新元组路由期间也可能来到这里，此时本函数会分别为 OLD 和 NEW 调用，
		 * 因此预期其中恰好一个为 NULL。
		 */
		if (trigdesc == NULL ||
			(event == TRIGGER_EVENT_DELETE && !trigdesc->trig_delete_after_row) ||
			(event == TRIGGER_EVENT_INSERT && !trigdesc->trig_insert_after_row) ||
			(event == TRIGGER_EVENT_UPDATE && !trigdesc->trig_update_after_row) ||
			(event == TRIGGER_EVENT_UPDATE && (TupIsNull(oldslot) ^ TupIsNull(newslot))))
			return;
	}

	/*
	 * We normally don't see partitioned tables here for row level triggers
	 * except in the special case of a cross-partition update.  In that case,
	 * nodeModifyTable.c:ExecCrossPartitionUpdateForeignKey() calls here to
	 * queue an update event on the root target partitioned table, also
	 * passing the source and destination partitions and their tuples.
	 *
	 * 行级触发器这里通常看不到分区表，除非是跨分区更新这一特殊情况。
	 * 此时 nodeModifyTable.c:ExecCrossPartitionUpd
	 * ateForeignKey() 调用这里，
	 * 在根目标分区表上入队一个更新事件，并传入源分区、目标分区及其元组。
	 */
	Assert(!row_trigger ||
		   rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE ||
		   (is_crosspart_update &&
			TRIGGER_FIRED_BY_UPDATE(event) &&
			src_partinfo != NULL && dst_partinfo != NULL));

	/*
	 * Validate the event code and collect the associated tuple CTIDs.
	 *
	 * 校验事件码并收集相关的元组 CTID。
	 *
	 * The event code will be used both as a bitmask and an array offset, so
	 * validation is important to make sure we don't walk off the edge of our
	 * arrays.
	 *
	 * 事件码既会用作位掩码，也会用作数组下标，因此校验很重要，以免走出数组边界。
	 *
	 * Also, if we're considering statement-level triggers, check whether we
	 * already queued a set of them for this event, and cancel the prior set
	 * if so.  This preserves the behavior that statement-level triggers fire
	 * just once per statement and fire after row-level triggers.
	 *
	 * 另外，若正在考虑语句级触发器，检查是否已经为该事件入队过一组，若是则取消先前那组。
	 * 这保持语句级触发器每条语句只触发一次，并且在行级触发器之后触发。
	 */
	switch (event)
	{
		case TRIGGER_EVENT_INSERT:
			tgtype_event = TRIGGER_TYPE_INSERT;
			if (row_trigger)
			{
				Assert(oldslot == NULL);
				Assert(newslot != NULL);
				ItemPointerCopy(&(newslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_INSERT, event);
			}
			break;
		case TRIGGER_EVENT_DELETE:
			tgtype_event = TRIGGER_TYPE_DELETE;
			if (row_trigger)
			{
				Assert(oldslot != NULL);
				Assert(newslot == NULL);
				ItemPointerCopy(&(oldslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_DELETE, event);
			}
			break;
		case TRIGGER_EVENT_UPDATE:
			tgtype_event = TRIGGER_TYPE_UPDATE;
			if (row_trigger)
			{
				Assert(oldslot != NULL);
				Assert(newslot != NULL);
				ItemPointerCopy(&(oldslot->tts_tid), &(new_event.ate_ctid1));
				ItemPointerCopy(&(newslot->tts_tid), &(new_event.ate_ctid2));

				/*
				 * Also remember the OIDs of partitions to fetch these tuples
				 * out of later in AfterTriggerExecute().
				 *
				 * 同时记住分区的 OID，以便稍后在 AfterTriggerExecute()
				 * 中把这些元组取出来。
				 */
				if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				{
					Assert(src_partinfo != NULL && dst_partinfo != NULL);
					new_event.ate_src_part =
						RelationGetRelid(src_partinfo->ri_RelationDesc);
					new_event.ate_dst_part =
						RelationGetRelid(dst_partinfo->ri_RelationDesc);
				}
			}
			else
			{
				Assert(oldslot == NULL);
				Assert(newslot == NULL);
				ItemPointerSetInvalid(&(new_event.ate_ctid1));
				ItemPointerSetInvalid(&(new_event.ate_ctid2));
				cancel_prior_stmt_triggers(RelationGetRelid(rel),
										   CMD_UPDATE, event);
			}
			break;
		case TRIGGER_EVENT_TRUNCATE:
			tgtype_event = TRIGGER_TYPE_TRUNCATE;
			Assert(oldslot == NULL);
			Assert(newslot == NULL);
			ItemPointerSetInvalid(&(new_event.ate_ctid1));
			ItemPointerSetInvalid(&(new_event.ate_ctid2));
			break;
		default:
			elog(ERROR, "invalid after-trigger event code: %d", event);
			tgtype_event = 0;	/* keep compiler quiet */
			/*
			 *
			 * 避免编译器告警
			 */
			break;
	}

	/* Determine flags */
	/*
	 *
	 * 确定标志
	 */
	if (!(relkind == RELKIND_FOREIGN_TABLE && row_trigger))
	{
		if (row_trigger && event == TRIGGER_EVENT_UPDATE)
		{
			if (relkind == RELKIND_PARTITIONED_TABLE)
				new_event.ate_flags = AFTER_TRIGGER_CP_UPDATE;
			else
				new_event.ate_flags = AFTER_TRIGGER_2CTID;
		}
		else
			new_event.ate_flags = AFTER_TRIGGER_1CTID;
	}

	/* else, we'll initialize ate_flags for each trigger */
	/*
	 *
	 * 否则，我们将为每个触发器初始化 ate_flags
	 */

	tgtype_level = (row_trigger ? TRIGGER_TYPE_ROW : TRIGGER_TYPE_STATEMENT);

	/*
	 * Must convert/copy the source and destination partition tuples into the
	 * root partitioned table's format/slot, because the processing in the
	 * loop below expects both oldslot and newslot tuples to be in that form.
	 *
	 * 必须把源分区和目标分区的元组转换/复制到根分区表的格式/槽中，
	 * 因为下面循环中的处理期望 oldslot 与 newslot 中的元组都是那种形式。
	 */
	if (row_trigger && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		TupleTableSlot *rootslot;
		TupleConversionMap *map;

		rootslot = ExecGetTriggerOldSlot(estate, relinfo);
		map = ExecGetChildToRootMap(src_partinfo);
		if (map)
			oldslot = execute_attr_map_slot(map->attrMap,
											oldslot,
											rootslot);
		else
			oldslot = ExecCopySlot(rootslot, oldslot);

		rootslot = ExecGetTriggerNewSlot(estate, relinfo);
		map = ExecGetChildToRootMap(dst_partinfo);
		if (map)
			newslot = execute_attr_map_slot(map->attrMap,
											newslot,
											rootslot);
		else
			newslot = ExecCopySlot(rootslot, newslot);
	}

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[i];

		if (!TRIGGER_TYPE_MATCHES(trigger->tgtype,
								  tgtype_level,
								  TRIGGER_TYPE_AFTER,
								  tgtype_event))
			continue;
		if (!TriggerEnabled(estate, relinfo, trigger, event,
							modifiedCols, oldslot, newslot))
			continue;

		if (relkind == RELKIND_FOREIGN_TABLE && row_trigger)
		{
			if (fdw_tuplestore == NULL)
			{
				fdw_tuplestore = GetCurrentFDWTuplestore();
				new_event.ate_flags = AFTER_TRIGGER_FDW_FETCH;
			}
			else
				/* subsequent event for the same tuple */
				/*
				 *
				 * 同一元组的后续事件
				 */
				new_event.ate_flags = AFTER_TRIGGER_FDW_REUSE;
		}

		/*
		 * If the trigger is a foreign key enforcement trigger, there are
		 * certain cases where we can skip queueing the event because we can
		 * tell by inspection that the FK constraint will still pass. There
		 * are also some cases during cross-partition updates of a partitioned
		 * table where queuing the event can be skipped.
		 *
		 * 若该触发器是外键强制触发器，某些情况下可以通过检查判断外键约束仍然成立，
		 * 从而跳过入队。
		 * 分区表跨分区更新期间也有一些可以跳过入队的情况。
		 */
		if (TRIGGER_FIRED_BY_UPDATE(event) || TRIGGER_FIRED_BY_DELETE(event))
		{
			switch (RI_FKey_trigger_type(trigger->tgfoid))
			{
				case RI_TRIGGER_PK:

					/*
					 * For cross-partitioned updates of partitioned PK table,
					 * skip the event fired by the component delete on the
					 * source leaf partition unless the constraint originates
					 * in the partition itself (!tgisclone), because the
					 * update event that will be fired on the root
					 * (partitioned) target table will be used to perform the
					 * necessary foreign key enforcement action.
					 *
					 * 对分区主键表的跨分区更新，跳过源叶子分区上由组成删除所触发的事件，
					 * 除非约束起源于分区自身（!tgisclone），
					 * 因为将在根（分区）目标表上触发的更新事件
					 * 会被用来执行必要的外键强制动作。
					 */
					if (is_crosspart_update &&
						TRIGGER_FIRED_BY_DELETE(event) &&
						trigger->tgisclone)
						continue;

					/* Update or delete on trigger's PK table */
					/*
					 *
					 * 对触发器主键表的更新或删除
					 */
					if (!RI_FKey_pk_upd_check_required(trigger, rel,
													   oldslot, newslot))
					{
						/* skip queuing this event */
						/*
						 *
						 * 跳过把该事件入队
						 */
						continue;
					}
					break;

				case RI_TRIGGER_FK:

					/*
					 * Update on trigger's FK table.  We can skip the update
					 * event fired on a partitioned table during a
					 * cross-partition of that table, because the insert event
					 * that is fired on the destination leaf partition would
					 * suffice to perform the necessary foreign key check.
					 * Moreover, RI_FKey_fk_upd_check_required() expects to be
					 * passed a tuple that contains system attributes, most of
					 * which are not present in the virtual slot belonging to
					 * a partitioned table.
					 *
					 * 对触发器外键表的更新。可以跳过该表跨分区期间在分区表上触发的更新事件，
					 * 因为在目标叶子分区上触发的插入事件足以做必要的外键检查。
					 * 而且 RI_FKey_fk_upd_check_required()
					 * 期望收到含有系统属性的元组，
					 * 分区表所属的虚槽中大部分系统属性并不存在。
					 */
					if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
						!RI_FKey_fk_upd_check_required(trigger, rel,
													   oldslot, newslot))
					{
						/* skip queuing this event */
						/*
						 *
						 * 跳过把该事件入队
						 */
						continue;
					}
					break;

				case RI_TRIGGER_NONE:

					/*
					 * Not an FK trigger.  No need to queue the update event
					 * fired during a cross-partitioned update of a
					 * partitioned table, because the same row trigger must be
					 * present in the leaf partition(s) that are affected as
					 * part of this update and the events fired on them are
					 * queued instead.
					 *
					 * 不是外键触发器。不必把分区表跨分区更新期间触发的更新事件入队，
					 * 因为作为本次更新一部分而受影响的叶子分区上必定有相同的行触发器，
					 * 改为把它们的事件入队。
					 */
					if (row_trigger &&
						rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
						continue;
					break;
			}
		}

		/*
		 * If the trigger is a deferred unique constraint check trigger, only
		 * queue it if the unique constraint was potentially violated, which
		 * we know from index insertion time.
		 *
		 * 若该触发器是延迟的唯一约束检查触发器，仅当唯一约束可能被违反时才入队，
		 * 这在索引插入时就已经知道。
		 */
		if (trigger->tgfoid == F_UNIQUE_KEY_RECHECK)
		{
			if (!list_member_oid(recheckIndexes, trigger->tgconstrindid))
				continue;		/* Uniqueness definitely not violated */
				/*
				 *
				 * 唯一性肯定没有被违反
				 */
		}

		/*
		 * Fill in event structure and add it to the current query's queue.
		 * Note we set ats_table to NULL whenever this trigger doesn't use
		 * transition tables, to improve sharability of the shared event data.
		 *
		 * 填好事件结构并加入当前查询的队列。注意：只要该触发器不使用 transition
		 * table，
		 * 就把 ats_table 设为 NULL，以提高共享事件数据的可共享性。
		 */
		new_shared.ats_event =
			(event & TRIGGER_EVENT_OPMASK) |
			(row_trigger ? TRIGGER_EVENT_ROW : 0) |
			(trigger->tgdeferrable ? AFTER_TRIGGER_DEFERRABLE : 0) |
			(trigger->tginitdeferred ? AFTER_TRIGGER_INITDEFERRED : 0);
		new_shared.ats_tgoid = trigger->tgoid;
		new_shared.ats_relid = RelationGetRelid(rel);
		new_shared.ats_rolid = GetUserId();
		new_shared.ats_firing_id = 0;
		if ((trigger->tgoldtable || trigger->tgnewtable) &&
			transition_capture != NULL)
		{
			switch (event)
			{
				case TRIGGER_EVENT_INSERT:
					new_shared.ats_table = transition_capture->tcs_insert_private;
					break;
				case TRIGGER_EVENT_UPDATE:
					new_shared.ats_table = transition_capture->tcs_update_private;
					break;
				case TRIGGER_EVENT_DELETE:
					new_shared.ats_table = transition_capture->tcs_delete_private;
					break;
				default:
					/* Must be TRUNCATE, see switch above */
					/*
					 *
					 * 必须是 TRUNCATE，见上面的 switch
					 */
					new_shared.ats_table = NULL;
					break;
			}
		}
		else
			new_shared.ats_table = NULL;
		new_shared.ats_modifiedcols = modifiedCols;

		afterTriggerAddEvent(&afterTriggers.query_stack[afterTriggers.query_depth].events,
							 &new_event, &new_shared);
	}

	/*
	 * Finally, spool any foreign tuple(s).  The tuplestore squashes them to
	 * minimal tuples, so this loses any system columns.  The executor lost
	 * those columns before us, for an unrelated reason, so this is fine.
	 *
	 * 最后，把任何外部元组暂存起来。tuplestore 会把它们压成最小元组，
	 * 因此会丢失系统列。
	 * 执行器在我们之前已经因为无关原因丢掉了那些列，所以这没问题。
	 */
	if (fdw_tuplestore)
	{
		if (oldslot != NULL)
			tuplestore_puttupleslot(fdw_tuplestore, oldslot);
		if (newslot != NULL)
			tuplestore_puttupleslot(fdw_tuplestore, newslot);
	}
}

/*
 * Detect whether we already queued BEFORE STATEMENT triggers for the given
 * relation + operation, and set the flag so the next call will report "true".
 *
 * 检测是否已经为给定关系 + 操作把 BEFORE STATEMENT 触发器入队，
 * 并设置标志，使下次调用报告 true。
 */
static bool
before_stmt_triggers_fired(Oid relid, CmdType cmdType)
{
	bool		result;
	AfterTriggersTableData *table;

	/* Check state, like AfterTriggerSaveEvent. */
	/*
	 *
	 * 检查状态，与 AfterTriggerSaveEvent 相同。
	 */
	if (afterTriggers.query_depth < 0)
		elog(ERROR, "before_stmt_triggers_fired() called outside of query");

	/* Be sure we have enough space to record events at this query depth. */
	/*
	 *
	 * 确保在此查询深度有足够空间记录事件。
	 */
	if (afterTriggers.query_depth >= afterTriggers.maxquerydepth)
		AfterTriggerEnlargeQueryState();

	/*
	 * We keep this state in the AfterTriggersTableData that also holds
	 * transition tables for the relation + operation.  In this way, if we are
	 * forced to make a new set of transition tables because more tuples get
	 * entered after we've already fired triggers, we will allow a new set of
	 * statement triggers to get queued.
	 *
	 * 把该状态保存在同时为该关系 + 操作保存 transition table 的
	 * AfterTriggersTableData 中。
	 * 这样，若在已经触发过触发器之后又进入更多元组、
	 * 被迫建立新的一组 transition table 时，
	 * 就会允许再入队一组新的语句级触发器。
	 */
	table = GetAfterTriggersTableData(relid, cmdType);
	result = table->before_trig_done;
	table->before_trig_done = true;
	return result;
}

/*
 * If we previously queued a set of AFTER STATEMENT triggers for the given
 * relation + operation, and they've not been fired yet, cancel them.  The
 * caller will queue a fresh set that's after any row-level triggers that may
 * have been queued by the current sub-statement, preserving (as much as
 * possible) the property that AFTER ROW triggers fire before AFTER STATEMENT
 * triggers, and that the latter only fire once.  This deals with the
 * situation where several FK enforcement triggers sequentially queue triggers
 * for the same table into the same trigger query level.  We can't fully
 * prevent odd behavior though: if there are AFTER ROW triggers taking
 * transition tables, we don't want to change the transition tables once the
 * first such trigger has seen them.  In such a case, any additional events
 * will result in creating new transition tables and allowing new firings of
 * statement triggers.
 *
 * 若先前已为给定关系 + 操作入队过一组 AFTER STATEMENT
 * 触发器且尚未触发，则取消它们。
 * 调用者会入队一组新的，位于当前子语句可能入队的任何行级触发器之后，
 * 从而尽可能保持 AFTER ROW 触发器先于 AFTER STATEMENT
 * 触发器，且后者只触发一次。
 * 这处理多个外键强制触发器依次把同一表的触发器排入同一触发器查询级别的情况。
 * 不过无法完全避免古怪行为：若有使用 transition table 的 AFTER
 * ROW 触发器，
 * 我们不想在第一个这样的触发器已经看过之后再改变 transition table。
 * 此时任何额外事件都会建立新的 transition table，
 * 并允许语句级触发器再次触发。
 *
 * This also saves the current event list location so that a later invocation
 * of this function can cheaply find the triggers we're about to queue and
 * cancel them.
 *
 * 这也会保存当前事件列表位置，使本函数稍后能廉价地找到即将入队并要取消的触发器。
 */
static void
cancel_prior_stmt_triggers(Oid relid, CmdType cmdType, int tgevent)
{
	AfterTriggersTableData *table;
	AfterTriggersQueryData *qs = &afterTriggers.query_stack[afterTriggers.query_depth];

	/*
	 * We keep this state in the AfterTriggersTableData that also holds
	 * transition tables for the relation + operation.  In this way, if we are
	 * forced to make a new set of transition tables because more tuples get
	 * entered after we've already fired triggers, we will allow a new set of
	 * statement triggers to get queued without canceling the old ones.
	 *
	 * 把该状态保存在同时为该关系 + 操作保存 transition table 的
	 * AfterTriggersTableData 中。
	 * 这样，若在已经触发过触发器之后又进入更多元组、
	 * 被迫建立新的一组 transition table 时，
	 * 就会允许再入队一组新的语句级触发器，而不取消旧的那些。
	 */
	table = GetAfterTriggersTableData(relid, cmdType);

	if (table->after_trig_done)
	{
		/*
		 * We want to start scanning from the tail location that existed just
		 * before we inserted any statement triggers.  But the events list
		 * might've been entirely empty then, in which case scan from the
		 * current head.
		 *
		 * 我们希望从插入任何语句级触发器之前的尾部位置开始扫描。
		 * 但当时事件列表可能完全为空，那种情况下从当前头开始扫描。
		 */
		AfterTriggerEvent event;
		AfterTriggerEventChunk *chunk;

		if (table->after_trig_events.tail)
		{
			chunk = table->after_trig_events.tail;
			event = (AfterTriggerEvent) table->after_trig_events.tailfree;
		}
		else
		{
			chunk = qs->events.head;
			event = NULL;
		}

		for_each_chunk_from(chunk)
		{
			if (event == NULL)
				event = (AfterTriggerEvent) CHUNK_DATA_START(chunk);
			for_each_event_from(event, chunk)
			{
				AfterTriggerShared evtshared = GetTriggerSharedData(event);

				/*
				 * Exit loop when we reach events that aren't AS triggers for
				 * the target relation.
				 *
				 * 到达不是目标关系的 AFTER STATEMENT 触发器的事件时退出循环。
				 */
				if (evtshared->ats_relid != relid)
					goto done;
				if ((evtshared->ats_event & TRIGGER_EVENT_OPMASK) != tgevent)
					goto done;
				if (!TRIGGER_FIRED_FOR_STATEMENT(evtshared->ats_event))
					goto done;
				if (!TRIGGER_FIRED_AFTER(evtshared->ats_event))
					goto done;
				/* OK, mark it DONE */
				/*
				 *
				 * 好，把它标为 DONE
				 */
				event->ate_flags &= ~AFTER_TRIGGER_IN_PROGRESS;
				event->ate_flags |= AFTER_TRIGGER_DONE;
			}
			/* signal we must reinitialize event ptr for next chunk */
			/*
			 *
			 * 表示下一块必须重新初始化事件指针
			 */
			event = NULL;
		}
	}
done:

	/* In any case, save current insertion point for next time */
	/*
	 *
	 * 无论如何，为下次保存当前插入点
	 */
	table->after_trig_done = true;
	table->after_trig_events = qs->events;
}

/*
 * GUC assign_hook for session_replication_role
 *
 * session_replication_role 的 GUC assign_hook
 */
void
assign_session_replication_role(int newval, void *extra)
{
	/*
	 * Must flush the plan cache when changing replication role; but don't
	 * flush unnecessarily.
	 *
	 * 改变复制角色时必须刷新计划缓存；但不要做不必要的刷新。
	 */
	if (SessionReplicationRole != newval)
		ResetPlanCache();
}

/*
 * SQL function pg_trigger_depth()
 *
 * SQL 函数 pg_trigger_depth()
 */
Datum
pg_trigger_depth(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyTriggerDepth);
}

/*
 * Check whether a trigger modified a virtual generated column and replace the
 * value with null if so.
 *
 * 检查触发器是否修改了虚拟生成列，若是则把该值替换为 null。
 *
 * We need to check this so that we don't end up storing a non-null value in a
 * virtual generated column.
 *
 * 需要做此检查，以免在虚拟生成列中存入非空值。
 *
 * We don't need to check for stored generated columns, since those will be
 * overwritten later anyway.
 *
 * 不必检查已存储的生成列，因为那些稍后无论如何都会被覆盖。
 */
static HeapTuple
check_modified_virtual_generated(TupleDesc tupdesc, HeapTuple tuple)
{
	if (!(tupdesc->constr && tupdesc->constr->has_generated_virtual))
		return tuple;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
		{
			if (!heap_attisnull(tuple, i + 1, tupdesc))
			{
				int			replCol = i + 1;
				Datum		replValue = 0;
				bool		replIsnull = true;

				tuple = heap_modify_tuple_by_cols(tuple, tupdesc, 1, &replCol, &replValue, &replIsnull);
			}
		}
	}

	return tuple;
}
