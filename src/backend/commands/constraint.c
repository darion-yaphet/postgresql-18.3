/*-------------------------------------------------------------------------
 *
 * constraint.c
 *	  PostgreSQL CONSTRAINT support code.
 *
 * PostgreSQL CONSTRAINT 支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/constraint.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "utils/fmgrprotos.h"
#include "utils/snapmgr.h"


/*
 * 核心流程概览：
 * unique_key_recheck 作为 AFTER ROW 触发器，对可延迟的唯一约束和排除约束
 * 做语句结束、提交或 SET CONSTRAINTS 时的复查：跳过已死行，打开索引，
 * 构造索引值后调用 index_insert 或 check_exclusion_constraint。
 */
/*
 * unique_key_recheck - trigger function to do a deferred uniqueness check.
 *
 * unique_key_recheck：做延迟唯一性检查的触发器函数。
 *
 * This now also does deferred exclusion-constraint checks, so the name is
 * somewhat historical.
 *
 * 现在也做延迟的排除约束检查，因此函数名只是历史遗留。
 *
 * This is invoked as an AFTER ROW trigger for both INSERT and UPDATE,
 * for any rows recorded as potentially violating a deferrable unique
 * or exclusion constraint.
 *
 * 对可能违反可延迟唯一约束或排除约束的行，
 * 在 INSERT 和 UPDATE 上作为 AFTER ROW 触发器调用。
 *
 * This may be an end-of-statement check, a commit-time check, or a
 * check triggered by a SET CONSTRAINTS command.
 *
 * 可能在语句结束、提交时，或由 SET CONSTRAINTS 触发。
 */
Datum
unique_key_recheck(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const char *funcname = "unique_key_recheck";
	ItemPointerData checktid;
	ItemPointerData tmptid;
	Relation	indexRel;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	/*
	 * Make sure this is being called as an AFTER ROW trigger.  Note:
	 * translatable error strings are shared with ri_triggers.c, so resist the
	 * temptation to fold the function name into them.
	 *
	 * 确认这是作为 AFTER ROW 触发器被调用的。可翻译的错误字符串与
	 * ri_triggers.c 共用，不要把函数名折进这些字符串。
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager",
						funcname)));

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER ROW",
						funcname)));

	/*
	 * Get the new data that was inserted/updated.
	 *
	 * 取得插入或更新后的新数据。
	 */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		checktid = trigdata->tg_trigslot->tts_tid;
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		checktid = trigdata->tg_newslot->tts_tid;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired for INSERT or UPDATE",
						funcname)));
		ItemPointerSetInvalid(&checktid);	/* keep compiler quiet */
		/*
		 *
		 * 避免编译器因未使用的返回值告警。
		 */
	}

	slot = table_slot_create(trigdata->tg_relation, NULL);

	/*
	 * If the row pointed at by checktid is now dead (ie, inserted and then
	 * deleted within our transaction), we can skip the check.  However, we
	 * have to be careful, because this trigger gets queued only in response
	 * to index insertions; which means it does not get queued e.g. for HOT
	 * updates.  The row we are called for might now be dead, but have a live
	 * HOT child, in which case we still need to make the check ---
	 * effectively, we're applying the check against the live child row,
	 * although we can use the values from this row since by definition all
	 * columns of interest to us are the same.
	 *
	 * 若 checktid 指向的行已死（在本事务中插入后又删除），可以跳过检查。
	 * 但本触发器只在索引插入时入队，HOT 更新不会入队。当前行可能已死，
	 * 却仍有存活的 HOT 子行，此时仍须检查：实质上是对存活子行检查，
	 * 而相关列按定义相同，因此可以使用本行的值。
	 *
	 * This might look like just an optimization, because the index AM will
	 * make this identical test before throwing an error.  But it's actually
	 * needed for correctness, because the index AM will also throw an error
	 * if it doesn't find the index entry for the row.  If the row's dead then
	 * it's possible the index entry has also been marked dead, and even
	 * removed.
	 *
	 * 这看起来像优化，因为索引 AM 在报错前也会做同样的测试。
	 * 但它是正确性所必需的：找不到该行的索引项时 AM 也会报错。
	 * 行已死后，索引项可能同样被标死甚至删除。
	 */
	tmptid = checktid;
	{
		IndexFetchTableData *scan = table_index_fetch_begin(trigdata->tg_relation);
		bool		call_again = false;

		if (!table_index_fetch_tuple(scan, &tmptid, SnapshotSelf, slot,
									 &call_again, NULL))
		{
			/*
			 * All rows referenced by the index entry are dead, so skip the
			 * check.
			 *
			 * 索引项引用的行都已死，跳过检查。
			 */
			ExecDropSingleTupleTableSlot(slot);
			table_index_fetch_end(scan);
			return PointerGetDatum(NULL);
		}
		table_index_fetch_end(scan);
	}

	/*
	 * Open the index, acquiring a RowExclusiveLock, just as if we were going
	 * to update it.  (This protects against possible changes of the index
	 * schema, not against concurrent updates.)
	 *
	 * 以 RowExclusiveLock 打开索引，如同即将更新它。
	 * 这防止索引模式被改变，并不防止并发更新。
	 */
	indexRel = index_open(trigdata->tg_trigger->tgconstrindid,
						  RowExclusiveLock);
	indexInfo = BuildIndexInfo(indexRel);

	/*
	 * Typically the index won't have expressions, but if it does we need an
	 * EState to evaluate them.  We need it for exclusion constraints too,
	 * even if they are just on simple columns.
	 *
	 * 索引通常没有表达式；若有，则需要 EState 来求值。
	 * 排除约束即使只在简单列上，也需要 EState。
	 */
	if (indexInfo->ii_Expressions != NIL ||
		indexInfo->ii_ExclusionOps != NULL)
	{
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);
		econtext->ecxt_scantuple = slot;
	}
	else
		estate = NULL;

	/*
	 * Form the index values and isnull flags for the index entry that we need
	 * to check.
	 *
	 * 为待检查的索引项构造索引值和 isnull 标志。
	 *
	 * Note: if the index uses functions that are not as immutable as they are
	 * supposed to be, this could produce an index tuple different from the
	 * original.  The index AM can catch such errors by verifying that it
	 * finds a matching index entry with the tuple's TID.  For exclusion
	 * constraints we check this in check_exclusion_constraint().
	 *
	 * 若索引函数的不变性弱于声明，这里可能生成与原来不同的索引元组。
	 * 索引 AM 会核对能否按元组 TID 找到匹配项来捕获这种错误。
	 * 排除约束则在 check_exclusion_constraint() 中检查。
	 */
	FormIndexDatum(indexInfo, slot, estate, values, isnull);

	/*
	 * Now do the appropriate check.
	 *
	 * 现在执行相应的检查。
	 */
	if (indexInfo->ii_ExclusionOps == NULL)
	{
		/*
		 * Note: this is not a real insert; it is a check that the index entry
		 * that has already been inserted is unique.  Passing the tuple's tid
		 * (i.e. unmodified by table_index_fetch_tuple()) is correct even if
		 * the row is now dead, because that is the TID the index will know
		 * about.
		 *
		 * 这不是真正的插入，而是检查已经插入的索引项是否唯一。
		 * 传入元组自己的 tid（不被 table_index_fetch_tuple() 改写）即使行已死也是对的，
		 * 因为索引认识的就是这个 TID。
		 */
		index_insert(indexRel, values, isnull, &checktid,
					 trigdata->tg_relation, UNIQUE_CHECK_EXISTING,
					 false, indexInfo);

		/* Cleanup cache possibly initialized by index_insert. */
		/*
		 *
		 * 清理 index_insert 可能初始化的缓存。
		 */
		index_insert_cleanup(indexRel, indexInfo);
	}
	else
	{
		/*
		 * For exclusion constraints we just do the normal check, but now it's
		 * okay to throw error.  In the HOT-update case, we must use the live
		 * HOT child's TID here, else check_exclusion_constraint will think
		 * the child is a conflict.
		 *
		 * 排除约束走正常检查，此时允许报错。HOT 更新时必须使用存活
		 * HOT 子行的 TID，否则 check_exclusion_constraint 会把子行当成冲突。
		 */
		check_exclusion_constraint(trigdata->tg_relation, indexRel, indexInfo,
								   &tmptid, values, isnull,
								   estate, false);
	}

	/*
	 * If that worked, then this index entry is unique or non-excluded, and we
	 * are done.
	 *
	 * 若通过，则该索引项唯一或未被排除，检查结束。
	 */
	if (estate != NULL)
		FreeExecutorState(estate);

	ExecDropSingleTupleTableSlot(slot);

	index_close(indexRel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}
