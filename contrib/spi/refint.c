/*
 * contrib/spi/refint.c
 *
 *
 * refint.c --	set of functions to define referential integrity
 *		constraints using general triggers.
 *
 * refint.c——使用通用触发器定义引用完整性约束的函数集。
 */
#include "postgres.h"

#include <ctype.h>

#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "refint",
					.version = PG_VERSION
);

typedef struct
{
	char	   *ident;
	int			nplans;
	SPIPlanPtr *splan;
} EPlan;

static EPlan *FPlans = NULL;
static int	nFPlans = 0;
static EPlan *PPlans = NULL;
static int	nPPlans = 0;

static EPlan *find_plan(char *ident, EPlan **eplan, int *nplans);

/*
 * check_primary_key () -- check that key in tuple being inserted/updated
 *			 references existing tuple in "primary" table.
 * Though it's called without args You have to specify referenced
 * table/keys while creating trigger:  key field names in triggered table,
 * referenced table name, referenced key field names:
 * EXECUTE PROCEDURE
 * check_primary_key ('Fkey1', 'Fkey2', 'Ptable', 'Pkey1', 'Pkey2').
 *
 * check_primary_key () -- 检查正在插入/更新的元组中的键引用“主”表中的现有元组。虽然调用时没有参数，但创建触发器时必须指定引用的表/键：触发表中的关键字段名称、引用的表名称、引用的关键字段名称： EXECUTE PROCEDURE check_primary_key ('Fkey1', 'Fkey2', 'Ptable', 'Pkey1', 'Pkey2')。
 */

PG_FUNCTION_INFO_V1(check_primary_key);

Datum
check_primary_key(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of args specified in CREATE TRIGGER */
	char	  **args;			/* arguments: column names and table name */
	int			nkeys;			/* # of key columns (= nargs / 2) */
	Datum	   *kvals;			/* key values */
	char	   *relname;		/* referenced relation name */
	Relation	rel;			/* triggered relation */
	HeapTuple	tuple = NULL;	/* tuple to return */
	TupleDesc	tupdesc;		/* tuple description */
	EPlan	   *plan;			/* prepared plan */
	Oid		   *argtypes = NULL;	/* key types to prepare execution plan */
	bool		isnull;			/* to know is some column NULL or not */
	char		ident[2 * NAMEDATALEN]; /* to identify myself */
	int			ret;
	int			i;

#ifdef	DEBUG_QUERY
	elog(DEBUG4, "check_primary_key: Enter Function");
#endif

	/*
	 * Some checks first...
	 *
	 * 首先进行一些检查...
	 */

	/* Called by trigger manager ?
	 *
	 * 由触发器管理器调用？
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_primary_key: not fired by trigger manager");

	/* Should be called for ROW trigger
	 *
	 * 应该调用 ROW 触发器
	 */
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_primary_key: must be fired for row");

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_primary_key: must be fired by AFTER trigger");

	/* If INSERTion then must check Tuple to being inserted
	 *
	 * 如果 INSERTion 则必须检查要插入的 Tuple
	 */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		tuple = trigdata->tg_trigtuple;

	/* Not should be called for DELETE
	 *
	 * 不应调用 DELETE
	 */
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_primary_key: cannot process DELETE events");

	/* If UPDATE, then must check new Tuple, not old one
	 *
	 * 如果更新，则必须检查新元组，而不是旧元组
	 */
	else
		tuple = trigdata->tg_newtuple;

	trigger = trigdata->tg_trigger;
	nargs = trigger->tgnargs;
	args = trigger->tgargs;

	if (nargs % 2 != 1)			/* odd number of arguments! */
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_primary_key: odd number of arguments should be specified");

	nkeys = nargs / 2;
	relname = args[nkeys];
	rel = trigdata->tg_relation;
	tupdesc = rel->rd_att;

	/* Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	/*
	 * We use SPI plan preparation feature, so allocate space to place key
	 * values.
	 *
	 * 我们使用SPI计划准备功能，因此分配空间来放置键值。
	 */
	kvals = (Datum *) palloc(nkeys * sizeof(Datum));

	/*
	 * Construct ident string as TriggerName $ TriggeredRelationId and try to
	 * find prepared execution plan.
	 *
	 * 构造 ident 字符串为 TriggerName $TriggeredRelationId 并尝试查找准备好的执行计划。
	 */
	snprintf(ident, sizeof(ident), "%s$%u", trigger->tgname, rel->rd_id);
	plan = find_plan(ident, &PPlans, &nPPlans);

	/* if there is no plan then allocate argtypes for preparation
	 *
	 * 如果没有计划则分配argtypes进行准备
	 */
	if (plan->nplans <= 0)
		argtypes = (Oid *) palloc(nkeys * sizeof(Oid));

	/* For each column in key ...
	 *
	 * 对于键中的每一列...
	 */
	for (i = 0; i < nkeys; i++)
	{
		/* get index of column in tuple
		 *
		 * 获取元组中列的索引
		 */
		int			fnumber = SPI_fnumber(tupdesc, args[i]);

		/* Bad guys may give us un-existing column in CREATE TRIGGER
		 *
		 * 坏人可能会在 CREATE TRIGGER 中给我们不存在的列
		 */
		if (fnumber <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("there is no attribute \"%s\" in relation \"%s\"",
							args[i], SPI_getrelname(rel))));

		/* Well, get binary (in internal format) value of column
		 *
		 * 好吧，获取列的二进制（内部格式）值
		 */
		kvals[i] = SPI_getbinval(tuple, tupdesc, fnumber, &isnull);

		/*
		 * If it's NULL then nothing to do! DON'T FORGET call SPI_finish ()!
		 * DON'T FORGET return tuple! Executor inserts tuple you're returning!
		 * If you return NULL then nothing will be inserted!
		 *
		 * 如果它是 NULL 那么什么也不做！不要忘记调用 SPI_finish ()！不要忘记返回元组！执行器插入您要返回的元组！如果返回 NULL，则不会插入任何内容！
		 */
		if (isnull)
		{
			SPI_finish();
			return PointerGetDatum(tuple);
		}

		if (plan->nplans <= 0)	/* Get typeId of column */
			argtypes[i] = SPI_gettypeid(tupdesc, fnumber);
	}

	/*
	 * If we have to prepare plan ...
	 *
	 * 如果我们必须准备计划...
	 */
	if (plan->nplans <= 0)
	{
		SPIPlanPtr	pplan;
		char		sql[8192];

		/*
		 * Construct query: SELECT 1 FROM _referenced_relation_ WHERE Pkey1 =
		 * $1 [AND Pkey2 = $2 [...]]
		 *
		 * 构造查询： SELECT 1 FROM _referenced_relation_ WHERE Pkey1 = $1 [AND Pkey2 = $2 [...]]
		 */
		snprintf(sql, sizeof(sql), "select 1 from %s where ", relname);
		for (i = 0; i < nkeys; i++)
		{
			snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "%s = $%d %s",
					 args[i + nkeys + 1], i + 1, (i < nkeys - 1) ? "and " : "");
		}

		/* Prepare plan for query
		 *
		 * 准备查询计划
		 */
		pplan = SPI_prepare(sql, nkeys, argtypes);
		if (pplan == NULL)
			/* internal error
			 *
			 * 内部错误
			 */
			elog(ERROR, "check_primary_key: SPI_prepare returned %s", SPI_result_code_string(SPI_result));

		/*
		 * Remember that SPI_prepare places plan in current memory context -
		 * so, we have to save plan in TopMemoryContext for later use.
		 *
		 * 请记住，SPI_prepare 将计划放置在当前内存上下文中 - 因此，我们必须将计划保存在 TopMemoryContext 中以供以后使用。
		 */
		if (SPI_keepplan(pplan))
			/* internal error
			 *
			 * 内部错误
			 */
			elog(ERROR, "check_primary_key: SPI_keepplan failed");
		plan->splan = (SPIPlanPtr *) MemoryContextAlloc(TopMemoryContext,
														sizeof(SPIPlanPtr));
		*(plan->splan) = pplan;
		plan->nplans = 1;
	}

	/*
	 * Ok, execute prepared plan.
	 *
	 * 好的，执行准备好的计划。
	 */
	ret = SPI_execp(*(plan->splan), kvals, NULL, 1);
	/* we have no NULLs - so we pass   ^^^^   here
	 *
	 * 我们没有 NULL - 所以我们在这里传递 ^^^^
	 */

	if (ret < 0)
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_primary_key: SPI_execp returned %d", ret);

	/*
	 * If there are no tuples returned by SELECT then ...
	 *
	 * 如果 SELECT 没有返回元组，则...
	 */
	if (SPI_processed == 0)
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("tuple references non-existent key"),
				 errdetail("Trigger \"%s\" found tuple referencing non-existent key in \"%s\".", trigger->tgname, relname)));

	SPI_finish();

	return PointerGetDatum(tuple);
}

/*
 * check_foreign_key () -- check that key in tuple being deleted/updated
 *			 is not referenced by tuples in "foreign" table(s).
 * Though it's called without args You have to specify (while creating trigger):
 * number of references, action to do if key referenced
 * ('restrict' | 'setnull' | 'cascade'), key field names in triggered
 * ("primary") table and referencing table(s)/keys:
 * EXECUTE PROCEDURE
 * check_foreign_key (2, 'restrict', 'Pkey1', 'Pkey2',
 * 'Ftable1', 'Fkey11', 'Fkey12', 'Ftable2', 'Fkey21', 'Fkey22').
 *
 * check_foreign_key () -- 检查正在删除/更新的元组中的键是否未被“外部”表中的元组引用。虽然调用时没有参数，但您必须指定（在创建触发器时）：引用数量、引用键时要执行的操作（'restrict' | 'setnull' | 'cascade'）、触发的（“主”）表中的关键字段名称和引用表/键： EXECUTE PROCEDURE check_foreign_key (2, 'restrict', 'Pkey1', 'Pkey2', 'Ftable1', “Fkey11”、“Fkey12”、“Ftable2”、“Fkey21”、“Fkey22”）。
 */

PG_FUNCTION_INFO_V1(check_foreign_key);

Datum
check_foreign_key(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of args specified in CREATE TRIGGER */
	char	  **args;			/* arguments: as described above */
	char	  **args_temp;
	int			nrefs;			/* number of references (== # of plans) */
	char		action;			/* 'R'estrict | 'S'etnull | 'C'ascade */
	int			nkeys;			/* # of key columns */
	Datum	   *kvals;			/* key values */
	char	   *relname;		/* referencing relation name */
	Relation	rel;			/* triggered relation */
	HeapTuple	trigtuple = NULL;	/* tuple to being changed */
	HeapTuple	newtuple = NULL;	/* tuple to return */
	TupleDesc	tupdesc;		/* tuple description */
	EPlan	   *plan;			/* prepared plan(s) */
	Oid		   *argtypes = NULL;	/* key types to prepare execution plan */
	bool		isnull;			/* to know is some column NULL or not */
	bool		isequal = true; /* are keys in both tuples equal (in UPDATE) */
	char		ident[2 * NAMEDATALEN]; /* to identify myself */
	int			is_update = 0;
	int			ret;
	int			i,
				r;

#ifdef DEBUG_QUERY
	elog(DEBUG4, "check_foreign_key: Enter Function");
#endif

	/*
	 * Some checks first...
	 *
	 * 首先进行一些检查...
	 */

	/* Called by trigger manager ?
	 *
	 * 由触发器管理器调用？
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: not fired by trigger manager");

	/* Should be called for ROW trigger
	 *
	 * 应该调用 ROW 触发器
	 */
	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: must be fired for row");

	/* Not should be called for INSERT
	 *
	 * 不应调用 INSERT
	 */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: cannot process INSERT events");

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: must be fired by AFTER trigger");

	/* Have to check tg_trigtuple - tuple being deleted
	 *
	 * 必须检查 tg_trigtuple - 元组被删除
	 */
	trigtuple = trigdata->tg_trigtuple;

	/*
	 * But if this is UPDATE then we have to return tg_newtuple. Also, if key
	 * in tg_newtuple is the same as in tg_trigtuple then nothing to do.
	 *
	 * 但如果这是更新，那么我们必须返回 tg_newtuple。另外，如果 tg_newtuple 中的键与 tg_trigtuple 中的键相同，则无需执行任何操作。
	 */
	is_update = 0;
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		newtuple = trigdata->tg_newtuple;
		is_update = 1;
	}
	trigger = trigdata->tg_trigger;
	nargs = trigger->tgnargs;
	args = trigger->tgargs;

	if (nargs < 5)				/* nrefs, action, key, Relation, key - at
								 * least */
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: too short %d (< 5) list of arguments", nargs);

	nrefs = pg_strtoint32(args[0]);
	if (nrefs < 1)
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: %d (< 1) number of references specified", nrefs);
	action = tolower((unsigned char) *(args[1]));
	if (action != 'r' && action != 'c' && action != 's')
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: invalid action %s", args[1]);
	nargs -= 2;
	args += 2;
	nkeys = (nargs - nrefs) / (nrefs + 1);
	if (nkeys <= 0 || nargs != (nrefs + nkeys * (nrefs + 1)))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "check_foreign_key: invalid number of arguments %d for %d references",
			 nargs + 2, nrefs);

	rel = trigdata->tg_relation;
	tupdesc = rel->rd_att;

	/* Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	/*
	 * We use SPI plan preparation feature, so allocate space to place key
	 * values.
	 *
	 * 我们使用SPI计划准备功能，因此分配空间来放置键值。
	 */
	kvals = (Datum *) palloc(nkeys * sizeof(Datum));

	/*
	 * Construct ident string as TriggerName $ TriggeredRelationId $
	 * OperationType and try to find prepared execution plan(s).
	 *
	 * 将 ident 字符串构造为 TriggerName $ TriggeredRelationId $ OperationType 并尝试查找准备好的执行计划。
	 */
	snprintf(ident, sizeof(ident), "%s$%u$%c", trigger->tgname, rel->rd_id, is_update ? 'U' : 'D');
	plan = find_plan(ident, &FPlans, &nFPlans);

	/* if there is no plan(s) then allocate argtypes for preparation
	 *
	 * 如果没有计划，则分配 argtypes 进行准备
	 */
	if (plan->nplans <= 0)
		argtypes = (Oid *) palloc(nkeys * sizeof(Oid));

	/*
	 * else - check that we have exactly nrefs plan(s) ready
	 *
	 * 否则 - 检查我们是否已准备好 nrefs 计划
	 */
	else if (plan->nplans != nrefs)
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "%s: check_foreign_key: # of plans changed in meantime",
			 trigger->tgname);

	/* For each column in key ...
	 *
	 * 对于键中的每一列...
	 */
	for (i = 0; i < nkeys; i++)
	{
		/* get index of column in tuple
		 *
		 * 获取元组中列的索引
		 */
		int			fnumber = SPI_fnumber(tupdesc, args[i]);

		/* Bad guys may give us un-existing column in CREATE TRIGGER
		 *
		 * 坏人可能会在 CREATE TRIGGER 中给我们不存在的列
		 */
		if (fnumber <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("there is no attribute \"%s\" in relation \"%s\"",
							args[i], SPI_getrelname(rel))));

		/* Well, get binary (in internal format) value of column
		 *
		 * 好吧，获取列的二进制（内部格式）值
		 */
		kvals[i] = SPI_getbinval(trigtuple, tupdesc, fnumber, &isnull);

		/*
		 * If it's NULL then nothing to do! DON'T FORGET call SPI_finish ()!
		 * DON'T FORGET return tuple! Executor inserts tuple you're returning!
		 * If you return NULL then nothing will be inserted!
		 *
		 * 如果它是 NULL 那么什么也不做！不要忘记调用 SPI_finish ()！不要忘记返回元组！执行器插入您要返回的元组！如果返回 NULL，则不会插入任何内容！
		 */
		if (isnull)
		{
			SPI_finish();
			return PointerGetDatum((newtuple == NULL) ? trigtuple : newtuple);
		}

		/*
		 * If UPDATE then get column value from new tuple being inserted and
		 * compare is this the same as old one. For the moment we use string
		 * presentation of values...
		 *
		 * 如果更新，则从插入的新元组中获取列值，并比较这是否与旧元组相同。目前我们使用字符串表示值......
		 */
		if (newtuple != NULL)
		{
			char	   *oldval = SPI_getvalue(trigtuple, tupdesc, fnumber);
			char	   *newval;

			/* this shouldn't happen! SPI_ERROR_NOOUTFUNC ?
			 *
			 * 这不应该发生！ SPI_ERROR_NOOUTFUNC ？
			 */
			if (oldval == NULL)
				/* internal error
				 *
				 * 内部错误
				 */
				elog(ERROR, "check_foreign_key: SPI_getvalue returned %s", SPI_result_code_string(SPI_result));
			newval = SPI_getvalue(newtuple, tupdesc, fnumber);
			if (newval == NULL || strcmp(oldval, newval) != 0)
				isequal = false;
		}

		if (plan->nplans <= 0)	/* Get typeId of column */
			argtypes[i] = SPI_gettypeid(tupdesc, fnumber);
	}
	args_temp = args;
	nargs -= nkeys;
	args += nkeys;

	/*
	 * If we have to prepare plans ...
	 *
	 * 如果我们必须准备计划...
	 */
	if (plan->nplans <= 0)
	{
		SPIPlanPtr	pplan;
		char		sql[8192];
		char	  **args2 = args;

		plan->splan = (SPIPlanPtr *) MemoryContextAlloc(TopMemoryContext,
														nrefs * sizeof(SPIPlanPtr));

		for (r = 0; r < nrefs; r++)
		{
			relname = args2[0];

			/*---------
			 * For 'R'estrict action we construct SELECT query:
			 *
			 * 对于“R”限制操作，我们构建 SELECT 查询：
			 *
			 *	SELECT 1
			 *	FROM _referencing_relation_
			 *	WHERE Fkey1 = $1 [AND Fkey2 = $2 [...]]
			 *
			 * SELECT 1 FROM _referencing_relation_ WHERE Fkey1 = $1 [AND Fkey2 = $2 [...]]
			 *
			 *	to check is tuple referenced or not.
			 *
			 * 检查元组是否被引用。
			 *---------
			 */
			if (action == 'r')

				snprintf(sql, sizeof(sql), "select 1 from %s where ", relname);

			/*---------
			 * For 'C'ascade action we construct DELETE query
			 *
			 * 对于“C”级联操作，我们构建 DELETE 查询
			 *
			 *	DELETE
			 *	FROM _referencing_relation_
			 *	WHERE Fkey1 = $1 [AND Fkey2 = $2 [...]]
			 *
			 * 从 _referencing_relation_ 删除，其中 Fkey1 = $1 [AND Fkey2 = $2 [...]]
			 *
			 * to delete all referencing tuples.
			 *
			 * 删除所有引用元组。
			 *---------
			 */

			/*
			 * Max : Cascade with UPDATE query i create update query that
			 * updates new key values in referenced tables
			 *
			 * Max：与 UPDATE 查询级联我创建更新查询来更新引用表中的新键值
			 */


			else if (action == 'c')
			{
				if (is_update == 1)
				{
					int			fn;
					char	   *nv;
					int			k;

					snprintf(sql, sizeof(sql), "update %s set ", relname);
					for (k = 1; k <= nkeys; k++)
					{
						int			is_char_type = 0;
						char	   *type;

						fn = SPI_fnumber(tupdesc, args_temp[k - 1]);
						Assert(fn > 0); /* already checked above */
						nv = SPI_getvalue(newtuple, tupdesc, fn);
						type = SPI_gettype(tupdesc, fn);

						if (strcmp(type, "text") == 0 ||
							strcmp(type, "varchar") == 0 ||
							strcmp(type, "char") == 0 ||
							strcmp(type, "bpchar") == 0 ||
							strcmp(type, "date") == 0 ||
							strcmp(type, "timestamp") == 0)
							is_char_type = 1;
#ifdef	DEBUG_QUERY
						elog(DEBUG4, "check_foreign_key Debug value %s type %s %d",
							 nv, type, is_char_type);
#endif

						/*
						 * is_char_type =1 i set ' ' for define a new value
						 *
						 * is_char_type =1 我设置 ' ' 来定义新值
						 */
						snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql),
								 " %s = %s%s%s %s ",
								 args2[k], (is_char_type > 0) ? "'" : "",
								 nv, (is_char_type > 0) ? "'" : "", (k < nkeys) ? ", " : "");
					}
					strcat(sql, " where ");
				}
				else
					/* DELETE */
					snprintf(sql, sizeof(sql), "delete from %s where ", relname);
			}

			/*
			 * For 'S'etnull action we construct UPDATE query - UPDATE
			 * _referencing_relation_ SET Fkey1 null [, Fkey2 null [...]]
			 * WHERE Fkey1 = $1 [AND Fkey2 = $2 [...]] - to set key columns in
			 * all referencing tuples to NULL.
			 *
			 * 对于“S”etnull 操作，我们构造 UPDATE 查询 - UPDATE _referencing_relation_ SET Fkey1 null [, Fkey2 null [...]] WHERE Fkey1 = $1 [AND Fkey2 = $2 [...]] - 将所有引用元组中的键列设置为 NULL。
			 */
			else if (action == 's')
			{
				snprintf(sql, sizeof(sql), "update %s set ", relname);
				for (i = 1; i <= nkeys; i++)
				{
					snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql),
							 "%s = null%s",
							 args2[i], (i < nkeys) ? ", " : "");
				}
				strcat(sql, " where ");
			}

			/* Construct WHERE qual
			 *
			 * 构造 WHERE 质量
			 */
			for (i = 1; i <= nkeys; i++)
			{
				snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), "%s = $%d %s",
						 args2[i], i, (i < nkeys) ? "and " : "");
			}

			/* Prepare plan for query
			 *
			 * 准备查询计划
			 */
			pplan = SPI_prepare(sql, nkeys, argtypes);
			if (pplan == NULL)
				/* internal error
				 *
				 * 内部错误
				 */
				elog(ERROR, "check_foreign_key: SPI_prepare returned %s", SPI_result_code_string(SPI_result));

			/*
			 * Remember that SPI_prepare places plan in current memory context
			 * - so, we have to save plan in Top memory context for later use.
			 *
			 * 请记住，SPI_prepare 将计划放置在当前内存上下文中 - 因此，我们必须将计划保存在顶部内存上下文中以供以后使用。
			 */
			if (SPI_keepplan(pplan))
				/* internal error
				 *
				 * 内部错误
				 */
				elog(ERROR, "check_foreign_key: SPI_keepplan failed");

			plan->splan[r] = pplan;

			args2 += nkeys + 1; /* to the next relation */
		}
		plan->nplans = nrefs;
#ifdef	DEBUG_QUERY
		elog(DEBUG4, "check_foreign_key Debug Query is :  %s ", sql);
#endif
	}

	/*
	 * If UPDATE and key is not changed ...
	 *
	 * 如果更新且密钥未更改...
	 */
	if (newtuple != NULL && isequal)
	{
		SPI_finish();
		return PointerGetDatum(newtuple);
	}

	/*
	 * Ok, execute prepared plan(s).
	 *
	 * 好的，执行准备好的计划。
	 */
	for (r = 0; r < nrefs; r++)
	{
		/*
		 * For 'R'estrict we may to execute plan for one tuple only, for other
		 * actions - for all tuples.
		 *
		 * 对于“R”限制，我们可以仅针对一个元组执行计划，对于其他操作 - 对于所有元组。
		 */
		int			tcount = (action == 'r') ? 1 : 0;

		relname = args[0];

		ret = SPI_execp(plan->splan[r], kvals, NULL, tcount);
		/* we have no NULLs - so we pass   ^^^^  here
		 *
		 * 我们没有 NULL - 所以我们在这里传递 ^^^^
		 */

		if (ret < 0)
			ereport(ERROR,
					(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
					 errmsg("SPI_execp returned %d", ret)));

		/* If action is 'R'estrict ...
		 *
		 * 如果操作是“R”限制...
		 */
		if (action == 'r')
		{
			/* If there is tuple returned by SELECT then ...
			 *
			 * 如果 SELECT 返回元组则...
			 */
			if (SPI_processed > 0)
				ereport(ERROR,
						(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
						 errmsg("\"%s\": tuple is referenced in \"%s\"",
								trigger->tgname, relname)));
		}
		else
		{
#ifdef REFINT_VERBOSE
			const char *operation;

			if (action == 'c')
				operation = is_update ? "updated" : "deleted";
			else
				operation = "set to null";

			elog(NOTICE, "%s: " UINT64_FORMAT " tuple(s) of %s are %s",
				 trigger->tgname, SPI_processed, relname, operation);
#endif
		}
		args += nkeys + 1;		/* to the next relation */
	}

	SPI_finish();

	return PointerGetDatum((newtuple == NULL) ? trigtuple : newtuple);
}

static EPlan *
find_plan(char *ident, EPlan **eplan, int *nplans)
{
	EPlan	   *newp;
	int			i;
	MemoryContext oldcontext;

	/*
	 * All allocations done for the plans need to happen in a session-safe
	 * context.
	 *
	 * 为计划完成的所有分配都需要在会话安全的上下文中进行。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (*nplans > 0)
	{
		for (i = 0; i < *nplans; i++)
		{
			if (strcmp((*eplan)[i].ident, ident) == 0)
				break;
		}
		if (i != *nplans)
		{
			MemoryContextSwitchTo(oldcontext);
			return (*eplan + i);
		}
		*eplan = (EPlan *) repalloc(*eplan, (i + 1) * sizeof(EPlan));
		newp = *eplan + i;
	}
	else
	{
		newp = *eplan = (EPlan *) palloc(sizeof(EPlan));
		(*nplans) = i = 0;
	}

	newp->ident = pstrdup(ident);
	newp->nplans = 0;
	newp->splan = NULL;
	(*nplans)++;

	MemoryContextSwitchTo(oldcontext);
	return newp;
}
