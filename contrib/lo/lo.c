/*
 *	PostgreSQL definitions for managed Large Objects.
 *
 * 托管大对象的 PostgreSQL 定义。
 *
 *	contrib/lo/lo.c
 *
 */

#include "postgres.h"

#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "lo",
					.version = PG_VERSION
);


/*
 * This is the trigger that protects us from orphaned large objects
 *
 * 这是保护我们免受孤立大对象侵害的触发器
 */
PG_FUNCTION_INFO_V1(lo_manage);

Datum
lo_manage(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	int			attnum;			/* attribute number to monitor	*/
	char	  **args;			/* Args containing attr name	*/
	TupleDesc	tupdesc;		/* Tuple Descriptor				*/
	HeapTuple	rettuple;		/* Tuple to be returned			*/
	bool		isdelete;		/* are we deleting?				*/
	HeapTuple	newtuple;		/* The new value for tuple		*/
	HeapTuple	trigtuple;		/* The original value of tuple	*/

	if (!CALLED_AS_TRIGGER(fcinfo)) /* internal error */
		elog(ERROR, "lo_manage: not fired by trigger manager");

	if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)) /* internal error */
		elog(ERROR, "%s: must be fired for row",
			 trigdata->tg_trigger->tgname);

	/*
	 * Fetch some values from trigdata
	 *
	 * 从 trigdata 中获取一些值
	 */
	newtuple = trigdata->tg_newtuple;
	trigtuple = trigdata->tg_trigtuple;
	tupdesc = trigdata->tg_relation->rd_att;
	args = trigdata->tg_trigger->tgargs;

	if (args == NULL)			/* internal error */
		elog(ERROR, "%s: no column name provided in the trigger definition",
			 trigdata->tg_trigger->tgname);

	/* tuple to return to Executor
	 *
	 * 元组返回到Executor
	 */
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		rettuple = newtuple;
	else
		rettuple = trigtuple;

	/* Are we deleting the row?
	 *
	 * 我们要删除该行吗？
	 */
	isdelete = TRIGGER_FIRED_BY_DELETE(trigdata->tg_event);

	/* Get the column we're interested in
	 *
	 * 获取我们感兴趣的专栏
	 */
	attnum = SPI_fnumber(tupdesc, args[0]);

	if (attnum <= 0)
		elog(ERROR, "%s: column \"%s\" does not exist",
			 trigdata->tg_trigger->tgname, args[0]);

	/*
	 * Handle updates
	 *
	 * 处理更新
	 *
	 * Here, if the value of the monitored attribute changes, then the large
	 * object associated with the original value is unlinked.
	 *
	 * 这里，如果所监视的属性的值发生变化，则与原始值关联的大对象被取消链接。
	 */
	if (newtuple != NULL &&
		bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber, trigdata->tg_updatedcols))
	{
		char	   *orig = SPI_getvalue(trigtuple, tupdesc, attnum);
		char	   *newv = SPI_getvalue(newtuple, tupdesc, attnum);

		if (orig != NULL && (newv == NULL || strcmp(orig, newv) != 0))
			DirectFunctionCall1(be_lo_unlink,
								ObjectIdGetDatum(atooid(orig)));

		if (newv)
			pfree(newv);
		if (orig)
			pfree(orig);
	}

	/*
	 * Handle deleting of rows
	 *
	 * 处理行的删除
	 *
	 * Here, we unlink the large object associated with the managed attribute
	 *
	 * 在这里，我们取消与托管属性关联的大对象的链接
	 */
	if (isdelete)
	{
		char	   *orig = SPI_getvalue(trigtuple, tupdesc, attnum);

		if (orig != NULL)
		{
			DirectFunctionCall1(be_lo_unlink,
								ObjectIdGetDatum(atooid(orig)));

			pfree(orig);
		}
	}

	return PointerGetDatum(rettuple);
}
