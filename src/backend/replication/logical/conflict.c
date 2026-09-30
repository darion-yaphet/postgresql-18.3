/*-------------------------------------------------------------------------
 * conflict.c
 *	   Support routines for logging conflicts.
 *
 * 记录冲突的支持例程。
 *
 * Copyright (c) 2024-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/conflict.c
 *
 * This file contains the code for logging conflicts on the subscriber during
 * logical replication.
 *
 * 本文件在逻辑复制期间，于订阅端记录冲突。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/commit_ts.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "pgstat.h"
#include "replication/conflict.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"

static const char *const ConflictTypeNames[] = {
	[CT_INSERT_EXISTS] = "insert_exists",
	[CT_UPDATE_ORIGIN_DIFFERS] = "update_origin_differs",
	[CT_UPDATE_EXISTS] = "update_exists",
	[CT_UPDATE_MISSING] = "update_missing",
	[CT_DELETE_ORIGIN_DIFFERS] = "delete_origin_differs",
	[CT_DELETE_MISSING] = "delete_missing",
	[CT_MULTIPLE_UNIQUE_CONFLICTS] = "multiple_unique_conflicts"
};

static int	errcode_apply_conflict(ConflictType type);
static void errdetail_apply_conflict(EState *estate,
									 ResultRelInfo *relinfo,
									 ConflictType type,
									 TupleTableSlot *searchslot,
									 TupleTableSlot *localslot,
									 TupleTableSlot *remoteslot,
									 Oid indexoid, TransactionId localxmin,
									 RepOriginId localorigin,
									 TimestampTz localts, StringInfo err_msg);
static char *build_tuple_value_details(EState *estate, ResultRelInfo *relinfo,
									   ConflictType type,
									   TupleTableSlot *searchslot,
									   TupleTableSlot *localslot,
									   TupleTableSlot *remoteslot,
									   Oid indexoid);
static char *build_index_value_desc(EState *estate, Relation localrel,
									TupleTableSlot *slot, Oid indexoid);

/*
 * Get the xmin and commit timestamp data (origin and timestamp) associated
 * with the provided local row.
 *
 * 取给定本地行的 xmin，以及提交时间戳数据（复制源与时间戳）。
 *
 * Return true if the commit timestamp data was found, false otherwise.
 *
 * 找到提交时间戳数据则返回 true，否则返回 false。
 */
bool
GetTupleTransactionInfo(TupleTableSlot *localslot, TransactionId *xmin,
						RepOriginId *localorigin, TimestampTz *localts)
{
	Datum		xminDatum;
	bool		isnull;

	xminDatum = slot_getsysattr(localslot, MinTransactionIdAttributeNumber,
								&isnull);
	*xmin = DatumGetTransactionId(xminDatum);
	Assert(!isnull);

	/*
	 * The commit timestamp data is not available if track_commit_timestamp is
	 * disabled.
	 *
	 * 若关闭了 track_commit_timestamp，就没有提交时间戳数据。
	 */
	if (!track_commit_timestamp)
	{
		*localorigin = InvalidRepOriginId;
		*localts = 0;
		return false;
	}

	return TransactionIdGetCommitTsData(*xmin, localts, localorigin);
}

/*
 * 核心流程：订阅端应用冲突时，ReportApplyConflict 汇总本地冲突行并 ereport。
 */

/*
 * This function is used to report a conflict while applying replication
 * changes.
 *
 * 在应用复制变更时报告冲突。
 *
 * 'searchslot' should contain the tuple used to search the local row to be
 * updated or deleted.
 *
 * searchslot 应包含用来查找待更新或待删除本地行的元组。
 *
 * 'remoteslot' should contain the remote new tuple, if any.
 *
 * remoteslot 应包含远端新元组；没有则为空。
 *
 * conflicttuples is a list of local rows that caused the conflict and the
 * conflict related information. See ConflictTupleInfo.
 *
 * conflicttuples 是引发冲突的本地行及其相关信息，见 ConflictTupleInfo。
 *
 * The caller must ensure that all the indexes passed in ConflictTupleInfo are
 * locked so that we can fetch and display the conflicting key values.
 *
 * 调用者必须锁住 ConflictTupleInfo 中的全部索引，以便取出并显示冲突键值。
 */
void
ReportApplyConflict(EState *estate, ResultRelInfo *relinfo, int elevel,
					ConflictType type, TupleTableSlot *searchslot,
					TupleTableSlot *remoteslot, List *conflicttuples)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	StringInfoData err_detail;

	initStringInfo(&err_detail);

	/* Form errdetail message by combining conflicting tuples information.
	 *
	 * 把各冲突元组的信息拼成 errdetail 消息。
	 */
	foreach_ptr(ConflictTupleInfo, conflicttuple, conflicttuples)
		errdetail_apply_conflict(estate, relinfo, type, searchslot,
								 conflicttuple->slot, remoteslot,
								 conflicttuple->indexoid,
								 conflicttuple->xmin,
								 conflicttuple->origin,
								 conflicttuple->ts,
								 &err_detail);

	pgstat_report_subscription_conflict(MySubscription->oid, type);

	ereport(elevel,
			errcode_apply_conflict(type),
			errmsg("conflict detected on relation \"%s.%s\": conflict=%s",
				   get_namespace_name(RelationGetNamespace(localrel)),
				   RelationGetRelationName(localrel),
				   ConflictTypeNames[type]),
			errdetail_internal("%s", err_detail.data));
}

/*
 * Find all unique indexes to check for a conflict and store them into
 * ResultRelInfo.
 *
 * 找出所有用于冲突检查的唯一索引，并存入 ResultRelInfo。
 */
void
InitConflictIndexes(ResultRelInfo *relInfo)
{
	List	   *uniqueIndexes = NIL;

	for (int i = 0; i < relInfo->ri_NumIndices; i++)
	{
		Relation	indexRelation = relInfo->ri_IndexRelationDescs[i];

		if (indexRelation == NULL)
			continue;

		/* Detect conflict only for unique indexes
		 *
		 * 只对唯一索引做冲突检测。
		 */
		if (!relInfo->ri_IndexRelationInfo[i]->ii_Unique)
			continue;

		/* Don't support conflict detection for deferrable index
		 *
		 * 可延迟索引不支持冲突检测。
		 */
		if (!indexRelation->rd_index->indimmediate)
			continue;

		uniqueIndexes = lappend_oid(uniqueIndexes,
									RelationGetRelid(indexRelation));
	}

	relInfo->ri_onConflictArbiterIndexes = uniqueIndexes;
}

/*
 * Add SQLSTATE error code to the current conflict report.
 *
 * 给当前冲突报告加上 SQLSTATE 错误码。
 */
static int
errcode_apply_conflict(ConflictType type)
{
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			return errcode(ERRCODE_UNIQUE_VIOLATION);
		case CT_UPDATE_ORIGIN_DIFFERS:
		case CT_UPDATE_MISSING:
		case CT_DELETE_ORIGIN_DIFFERS:
		case CT_DELETE_MISSING:
			return errcode(ERRCODE_T_R_SERIALIZATION_FAILURE);
	}

	Assert(false);
	return 0;					/* silence compiler warning
								 *
								 * 消除编译器警告。
								 */
}

/*
 * Add an errdetail() line showing conflict detail.
 *
 * 增加一行 errdetail()，展示冲突细节。
 *
 * The DETAIL line comprises of two parts:
 * 1. Explanation of the conflict type, including the origin and commit
 *    timestamp of the existing local row.
 * 2. Display of conflicting key, existing local row, remote new row, and
 *    replica identity columns, if any. The remote old row is excluded as its
 *    information is covered in the replica identity columns.
 *
 * DETAIL 行分两部分：1. 解释冲突类型，包括已有本地行的复制源和提交时间戳。
 * 2. 显示冲突键、已有本地行、远端新行，以及副本标识列（若有）。
 * 不显示远端旧行，因为副本标识列已经覆盖那些信息。
 */
static void
errdetail_apply_conflict(EState *estate, ResultRelInfo *relinfo,
						 ConflictType type, TupleTableSlot *searchslot,
						 TupleTableSlot *localslot, TupleTableSlot *remoteslot,
						 Oid indexoid, TransactionId localxmin,
						 RepOriginId localorigin, TimestampTz localts,
						 StringInfo err_msg)
{
	StringInfoData err_detail;
	char	   *val_desc;
	char	   *origin_name;

	initStringInfo(&err_detail);

	/* First, construct a detailed message describing the type of conflict
	 *
	 * 先构造说明冲突类型的详细消息。
	 */
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			Assert(OidIsValid(indexoid) &&
				   CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

			if (localts)
			{
				if (localorigin == InvalidRepOriginId)
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified locally in transaction %u at %s."),
									 get_rel_name(indexoid),
									 localxmin, timestamptz_to_str(localts));
				else if (replorigin_by_oid(localorigin, true, &origin_name))
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by origin \"%s\" in transaction %u at %s."),
									 get_rel_name(indexoid), origin_name,
									 localxmin, timestamptz_to_str(localts));

				/*
				 * The origin that modified this row has been removed. This
				 * can happen if the origin was created by a different apply
				 * worker and its associated subscription and origin were
				 * dropped after updating the row, or if the origin was
				 * manually dropped by the user.
				 *
				 * 修改该行的复制源已被删除。可能是另一个应用工作进程创建了该复制源，
				 * 更新行之后其订阅和复制源被删除，也可能是用户手工删除了复制源。
				 */
				else
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by a non-existent origin in transaction %u at %s."),
									 get_rel_name(indexoid),
									 localxmin, timestamptz_to_str(localts));
			}
			else
				appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified in transaction %u."),
								 get_rel_name(indexoid), localxmin);

			break;

		case CT_UPDATE_ORIGIN_DIFFERS:
			if (localorigin == InvalidRepOriginId)
				appendStringInfo(&err_detail, _("Updating the row that was modified locally in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));
			else if (replorigin_by_oid(localorigin, true, &origin_name))
				appendStringInfo(&err_detail, _("Updating the row that was modified by a different origin \"%s\" in transaction %u at %s."),
								 origin_name, localxmin, timestamptz_to_str(localts));

			/* The origin that modified this row has been removed.
			 *
			 * 修改该行的复制源已被删除。
			 */
			else
				appendStringInfo(&err_detail, _("Updating the row that was modified by a non-existent origin in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));

			break;

		case CT_UPDATE_MISSING:
			appendStringInfoString(&err_detail, _("Could not find the row to be updated."));
			break;

		case CT_DELETE_ORIGIN_DIFFERS:
			if (localorigin == InvalidRepOriginId)
				appendStringInfo(&err_detail, _("Deleting the row that was modified locally in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));
			else if (replorigin_by_oid(localorigin, true, &origin_name))
				appendStringInfo(&err_detail, _("Deleting the row that was modified by a different origin \"%s\" in transaction %u at %s."),
								 origin_name, localxmin, timestamptz_to_str(localts));

			/* The origin that modified this row has been removed.
			 *
			 * 修改该行的复制源已被删除。
			 */
			else
				appendStringInfo(&err_detail, _("Deleting the row that was modified by a non-existent origin in transaction %u at %s."),
								 localxmin, timestamptz_to_str(localts));

			break;

		case CT_DELETE_MISSING:
			appendStringInfoString(&err_detail, _("Could not find the row to be deleted."));
			break;
	}

	Assert(err_detail.len > 0);

	val_desc = build_tuple_value_details(estate, relinfo, type, searchslot,
										 localslot, remoteslot, indexoid);

	/*
	 * Next, append the key values, existing local row, remote row, and
	 * replica identity columns after the message.
	 *
	 * 接着在消息后面附上键值、已有本地行、远端行和副本标识列。
	 */
	if (val_desc)
		appendStringInfo(&err_detail, "\n%s", val_desc);

	/*
	 * Insert a blank line to visually separate the new detail line from the
	 * existing ones.
	 *
	 * 插入一个空行，把新的细节行和已有内容视觉上分开。
	 */
	if (err_msg->len > 0)
		appendStringInfoChar(err_msg, '\n');

	appendStringInfoString(err_msg, err_detail.data);
}

/*
 * Helper function to build the additional details for conflicting key,
 * existing local row, remote row, and replica identity columns.
 *
 * 辅助函数：拼出冲突键、已有本地行、远端行和副本标识列的附加细节。
 *
 * If the return value is NULL, it indicates that the current user lacks
 * permissions to view the columns involved.
 *
 * 返回 NULL 表示当前用户无权查看所涉及的列。
 */
static char *
build_tuple_value_details(EState *estate, ResultRelInfo *relinfo,
						  ConflictType type,
						  TupleTableSlot *searchslot,
						  TupleTableSlot *localslot,
						  TupleTableSlot *remoteslot,
						  Oid indexoid)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	Oid			relid = RelationGetRelid(localrel);
	TupleDesc	tupdesc = RelationGetDescr(localrel);
	StringInfoData tuple_value;
	char	   *desc = NULL;

	Assert(searchslot || localslot || remoteslot);

	initStringInfo(&tuple_value);

	/*
	 * Report the conflicting key values in the case of a unique constraint
	 * violation.
	 *
	 * 唯一约束冲突时，报告冲突的键值。
	 */
	if (type == CT_INSERT_EXISTS || type == CT_UPDATE_EXISTS ||
		type == CT_MULTIPLE_UNIQUE_CONFLICTS)
	{
		Assert(OidIsValid(indexoid) && localslot);

		desc = build_index_value_desc(estate, localrel, localslot, indexoid);

		if (desc)
			appendStringInfo(&tuple_value, _("Key %s"), desc);
	}

	if (localslot)
	{
		/*
		 * The 'modifiedCols' only applies to the new tuple, hence we pass
		 * NULL for the existing local row.
		 *
		 * modifiedCols 只适用于新元组，因此对已有本地行传入 NULL。
		 */
		desc = ExecBuildSlotValueDescription(relid, localslot, tupdesc,
											 NULL, 64);

		if (desc)
		{
			if (tuple_value.len > 0)
			{
				appendStringInfoString(&tuple_value, "; ");
				appendStringInfo(&tuple_value, _("existing local row %s"),
								 desc);
			}
			else
			{
				appendStringInfo(&tuple_value, _("Existing local row %s"),
								 desc);
			}
		}
	}

	if (remoteslot)
	{
		Bitmapset  *modifiedCols;

		/*
		 * Although logical replication doesn't maintain the bitmap for the
		 * columns being inserted, we still use it to create 'modifiedCols'
		 * for consistency with other calls to ExecBuildSlotValueDescription.
		 *
		 * 逻辑复制并不维护正在插入的列位图，这里仍用它生成 modifiedCols，
		 * 以便与其他 ExecBuildSlotValueDescription 调用保持一致。
		 *
		 * Note that generated columns are formed locally on the subscriber.
		 *
		 * 生成列是在订阅端本地形成的。
		 */
		modifiedCols = bms_union(ExecGetInsertedCols(relinfo, estate),
								 ExecGetUpdatedCols(relinfo, estate));
		desc = ExecBuildSlotValueDescription(relid, remoteslot, tupdesc,
											 modifiedCols, 64);

		if (desc)
		{
			if (tuple_value.len > 0)
			{
				appendStringInfoString(&tuple_value, "; ");
				appendStringInfo(&tuple_value, _("remote row %s"), desc);
			}
			else
			{
				appendStringInfo(&tuple_value, _("Remote row %s"), desc);
			}
		}
	}

	if (searchslot)
	{
		/*
		 * Note that while index other than replica identity may be used (see
		 * IsIndexUsableForReplicaIdentityFull for details) to find the tuple
		 * when applying update or delete, such an index scan may not result
		 * in a unique tuple and we still compare the complete tuple in such
		 * cases, thus such indexes are not used here.
		 *
		 * 应用 UPDATE 或 DELETE 时，除副本标识外的索引也可能用来找元组，见 IsIndexUsableForReplicaIdentityFull。
		 * 这种索引扫描不一定得到唯一元组，此时仍会比较完整元组，因此这里不用那些索引。
		 */
		Oid			replica_index = GetRelationIdentityOrPK(localrel);

		Assert(type != CT_INSERT_EXISTS);

		/*
		 * If the table has a valid replica identity index, build the index
		 * key value string. Otherwise, construct the full tuple value for
		 * REPLICA IDENTITY FULL cases.
		 *
		 * 若表有有效的副本标识索引，则构造索引键字符串。
		 * 否则在 REPLICA IDENTITY FULL 时构造完整元组的值。
		 */
		if (OidIsValid(replica_index))
			desc = build_index_value_desc(estate, localrel, searchslot, replica_index);
		else
			desc = ExecBuildSlotValueDescription(relid, searchslot, tupdesc, NULL, 64);

		if (desc)
		{
			if (tuple_value.len > 0)
			{
				appendStringInfoString(&tuple_value, "; ");
				appendStringInfo(&tuple_value, OidIsValid(replica_index)
								 ? _("replica identity %s")
								 : _("replica identity full %s"), desc);
			}
			else
			{
				appendStringInfo(&tuple_value, OidIsValid(replica_index)
								 ? _("Replica identity %s")
								 : _("Replica identity full %s"), desc);
			}
		}
	}

	if (tuple_value.len == 0)
		return NULL;

	appendStringInfoChar(&tuple_value, '.');
	return tuple_value.data;
}

/*
 * Helper functions to construct a string describing the contents of an index
 * entry. See BuildIndexValueDescription for details.
 *
 * 辅助函数：构造描述索引项内容的字符串。细节见 BuildIndexValueDescription。
 *
 * The caller must ensure that the index with the OID 'indexoid' is locked so
 * that we can fetch and display the conflicting key value.
 *
 * 调用者必须锁住 OID 为 indexoid 的索引，以便取出并显示冲突键值。
 */
static char *
build_index_value_desc(EState *estate, Relation localrel, TupleTableSlot *slot,
					   Oid indexoid)
{
	char	   *index_value;
	Relation	indexDesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	TupleTableSlot *tableslot = slot;

	if (!tableslot)
		return NULL;

	Assert(CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

	indexDesc = index_open(indexoid, NoLock);

	/*
	 * If the slot is a virtual slot, copy it into a heap tuple slot as
	 * FormIndexDatum only works with heap tuple slots.
	 *
	 * 若 slot 是虚拟槽，先复制到堆元组槽，因为 FormIndexDatum 只接受堆元组槽。
	 */
	if (TTS_IS_VIRTUAL(slot))
	{
		tableslot = table_slot_create(localrel, &estate->es_tupleTable);
		tableslot = ExecCopySlot(tableslot, slot);
	}

	/*
	 * Initialize ecxt_scantuple for potential use in FormIndexDatum when
	 * index expressions are present.
	 *
	 * 若索引含表达式，为 FormIndexDatum 可能用到的 ecxt_scantuple 做初始化。
	 */
	GetPerTupleExprContext(estate)->ecxt_scantuple = tableslot;

	/*
	 * The values/nulls arrays passed to BuildIndexValueDescription should be
	 * the results of FormIndexDatum, which are the "raw" input to the index
	 * AM.
	 *
	 * 传给 BuildIndexValueDescription 的 values 与 nulls 数组应是 FormIndexDatum 的结果，
	 * 也就是索引访问方法的原始输入。
	 */
	FormIndexDatum(BuildIndexInfo(indexDesc), tableslot, estate, values, isnull);

	index_value = BuildIndexValueDescription(indexDesc, values, isnull);

	index_close(indexDesc, NoLock);

	return index_value;
}
