/*
moddatetime.c

contrib/spi/moddatetime.c

What is this?
It is a function to be called from a trigger for the purpose of updating
a modification datetime stamp in a record when that record is UPDATEd.
 *
 * 这是什么？它是从触发器调用的函数，目的是在更新记录时更新记录中的修改日期时间戳。

Credits
This is 95%+ based on autoinc.c, which I used as a starting point as I do
not really know what I am doing.  I also had help from
Jan Wieck <jwieck@debis.com> who told me about the timestamp_in("now") function.
OH, me, I'm Terry Mackintosh <terry@terrym.com>
 *
 * 学分 这是 95%+ 基于 autoinc.c，我用它作为起点，因为我真的不知道我在做什么。  我还得到了 Jan Wieck <jwieck@debis.com> 的帮助，他告诉我有关 timestamp_in("now") 函数的信息。哦，我，我是特里·麦金托什 <terry@terrym.com>
*/
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "moddatetime",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(moddatetime);

Datum
moddatetime(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of arguments */
	int			attnum;			/* positional number of field to change */
	Oid			atttypid;		/* type OID of field to change */
	Datum		newdt;			/* The current datetime. */
	bool		newdtnull;		/* null flag for it */
	char	  **args;			/* arguments */
	char	   *relname;		/* triggered relation name */
	Relation	rel;			/* triggered relation */
	HeapTuple	rettuple = NULL;
	TupleDesc	tupdesc;		/* tuple description */

	if (!CALLED_AS_TRIGGER(fcinfo))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "moddatetime: not fired by trigger manager");

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "moddatetime: must be fired for row");

	if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "moddatetime: must be fired before event");

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "moddatetime: cannot process INSERT events");
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = trigdata->tg_newtuple;
	else
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "moddatetime: cannot process DELETE events");

	rel = trigdata->tg_relation;
	relname = SPI_getrelname(rel);

	trigger = trigdata->tg_trigger;

	nargs = trigger->tgnargs;

	if (nargs != 1)
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "moddatetime (%s): A single argument was expected", relname);

	args = trigger->tgargs;
	/* must be the field layout?
	 *
	 * 一定是场布局吗？
	 */
	tupdesc = rel->rd_att;

	/*
	 * This gets the position in the tuple of the field we want. args[0] being
	 * the name of the field to update, as passed in from the trigger.
	 *
	 * 这得到了我们想要的字段在元组中的位置。 args[0] 是从触发器传入的要更新的字段的名称。
	 */
	attnum = SPI_fnumber(tupdesc, args[0]);

	/*
	 * This is where we check to see if the field we are supposed to update
	 * even exists.
	 *
	 * 这是我们检查要更新的字段是否存在的地方。
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("\"%s\" has no attribute \"%s\"",
						relname, args[0])));

	/*
	 * Check the target field has an allowed type, and get the current
	 * datetime as a value of that type.
	 *
	 * 检查目标字段是否具有允许的类型，并获取当前日期时间作为该类型的值。
	 */
	atttypid = SPI_gettypeid(tupdesc, attnum);
	if (atttypid == TIMESTAMPOID)
		newdt = DirectFunctionCall3(timestamp_in,
									CStringGetDatum("now"),
									ObjectIdGetDatum(InvalidOid),
									Int32GetDatum(-1));
	else if (atttypid == TIMESTAMPTZOID)
		newdt = DirectFunctionCall3(timestamptz_in,
									CStringGetDatum("now"),
									ObjectIdGetDatum(InvalidOid),
									Int32GetDatum(-1));
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_TRIGGERED_ACTION_EXCEPTION),
				 errmsg("attribute \"%s\" of \"%s\" must be type TIMESTAMP or TIMESTAMPTZ",
						args[0], relname)));
		newdt = (Datum) 0;		/* keep compiler quiet */
	}
	newdtnull = false;

	/* Replace the attnum'th column with newdt
	 *
	 * 将第 attnum 列替换为 newdt
	 */
	rettuple = heap_modify_tuple_by_cols(rettuple, tupdesc,
										 1, &attnum, &newdt, &newdtnull);

	/* Clean up
	 *
	 * 清理
	 */
	pfree(relname);

	return PointerGetDatum(rettuple);
}
