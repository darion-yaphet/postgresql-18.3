/*-------------------------------------------------------------------------
 *
 * heap_surgery.c
 *	  Functions to perform surgery on the damaged heap table.
 *
 * Copyright (c) 2020-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_surgery/heap_surgery.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/visibilitymap.h"
#include "access/xloginsert.h"
#include "catalog/pg_am_d.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_surgery",
					.version = PG_VERSION
);

/* Options to forcefully change the state of a heap tuple.
 *
 * 强制更改堆元组状态的选项。
 */
typedef enum HeapTupleForceOption
{
	HEAP_FORCE_KILL,
	HEAP_FORCE_FREEZE,
} HeapTupleForceOption;

PG_FUNCTION_INFO_V1(heap_force_kill);
PG_FUNCTION_INFO_V1(heap_force_freeze);

static int32 tidcmp(const void *a, const void *b);
static Datum heap_force_common(FunctionCallInfo fcinfo,
							   HeapTupleForceOption heap_force_opt);
static void sanity_check_tid_array(ArrayType *ta, int *ntids);
static BlockNumber find_tids_one_page(ItemPointer tids, int ntids,
									  OffsetNumber *next_start_ptr);

/*-------------------------------------------------------------------------
 * heap_force_kill()
 *
 * Force kill the tuple(s) pointed to by the item pointer(s) stored in the
 * given TID array.
 *
 * 强制终止存储在给定 TID 数组中的项指针所指向的元组。
 *
 * Usage: SELECT heap_force_kill(regclass, tid[]);
 *
 * 用法： SELECT heap_force_kill(regclass, tid[]);
 *-------------------------------------------------------------------------
 */
Datum
heap_force_kill(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(heap_force_common(fcinfo, HEAP_FORCE_KILL));
}

/*-------------------------------------------------------------------------
 * heap_force_freeze()
 *
 * Force freeze the tuple(s) pointed to by the item pointer(s) stored in the
 * given TID array.
 *
 * 强制冻结给定 TID 数组中存储的项指针所指向的元组。
 *
 * Usage: SELECT heap_force_freeze(regclass, tid[]);
 *
 * 用法： SELECT heap_force_freeze(regclass, tid[]);
 *-------------------------------------------------------------------------
 */
Datum
heap_force_freeze(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(heap_force_common(fcinfo, HEAP_FORCE_FREEZE));
}

/*-------------------------------------------------------------------------
 * heap_force_common()
 *
 * Common code for heap_force_kill and heap_force_freeze
 *
 * heap_force_kill 和 heap_force_freeze 的通用代码
 *-------------------------------------------------------------------------
 */
static Datum
heap_force_common(FunctionCallInfo fcinfo, HeapTupleForceOption heap_force_opt)
{
	Oid			relid = PG_GETARG_OID(0);
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P_COPY(1);
	ItemPointer tids;
	int			ntids,
				nblocks;
	Relation	rel;
	OffsetNumber curr_start_ptr,
				next_start_ptr;
	bool		include_this_tid[MaxHeapTuplesPerPage];

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("Heap surgery functions cannot be executed during recovery.")));

	/* Check inputs.
	 *
	 * 检查输入。
	 */
	sanity_check_tid_array(ta, &ntids);

	rel = relation_open(relid, RowExclusiveLock);

	/*
	 * Check target relation.
	 *
	 * 检查目标关系。
	 */
	if (!RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot operate on relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	/* Must be owner of the table or superuser.
	 *
	 * 必须是表的所有者或超级用户。
	 */
	if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	tids = ((ItemPointer) ARR_DATA_PTR(ta));

	/*
	 * If there is more than one TID in the array, sort them so that we can
	 * easily fetch all the TIDs belonging to one particular page from the
	 * array.
	 *
	 * 如果数组中有多个TID，请将它们排序，以便我们可以轻松地从数组中获取属于某一特定页面的所有TID。
	 */
	if (ntids > 1)
		qsort(tids, ntids, sizeof(ItemPointerData), tidcmp);

	curr_start_ptr = next_start_ptr = 0;
	nblocks = RelationGetNumberOfBlocks(rel);

	/*
	 * Loop, performing the necessary actions for each block.
	 *
	 * 循环，为每个块执行必要的操作。
	 */
	while (next_start_ptr != ntids)
	{
		Buffer		buf;
		Buffer		vmbuf = InvalidBuffer;
		Page		page;
		BlockNumber blkno;
		OffsetNumber curoff;
		OffsetNumber maxoffset;
		int			i;
		bool		did_modify_page = false;
		bool		did_modify_vm = false;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Find all the TIDs belonging to one particular page starting from
		 * next_start_ptr and process them one by one.
		 *
		 * 从next_start_ptr开始查找属于某一特定页面的所有TID并一一处理。
		 */
		blkno = find_tids_one_page(tids, ntids, &next_start_ptr);

		/* Check whether the block number is valid.
		 *
		 * 检查区块号是否有效。
		 */
		if (blkno >= nblocks)
		{
			/* Update the current_start_ptr before moving to the next page.
			 *
			 * 在移动到下一页之前更新 current_start_ptr。
			 */
			curr_start_ptr = next_start_ptr;

			ereport(NOTICE,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("skipping block %u for relation \"%s\" because the block number is out of range",
							blkno, RelationGetRelationName(rel))));
			continue;
		}

		buf = ReadBuffer(rel, blkno);
		LockBufferForCleanup(buf);

		page = BufferGetPage(buf);

		maxoffset = PageGetMaxOffsetNumber(page);

		/*
		 * Figure out which TIDs we are going to process and which ones we are
		 * going to skip.
		 *
		 * 找出我们要处理哪些 TID 以及要跳过哪些 TID。
		 */
		memset(include_this_tid, 0, sizeof(include_this_tid));
		for (i = curr_start_ptr; i < next_start_ptr; i++)
		{
			OffsetNumber offno = ItemPointerGetOffsetNumberNoCheck(&tids[i]);
			ItemId		itemid;

			/* Check whether the offset number is valid.
			 *
			 * 检查偏移号是否有效。
			 */
			if (offno == InvalidOffsetNumber || offno > maxoffset)
			{
				ereport(NOTICE,
						errmsg("skipping tid (%u, %u) for relation \"%s\" because the item number is out of range",
							   blkno, offno, RelationGetRelationName(rel)));
				continue;
			}

			itemid = PageGetItemId(page, offno);

			/* Only accept an item ID that is used.
			 *
			 * 仅接受已使用的项目 ID。
			 */
			if (ItemIdIsRedirected(itemid))
			{
				ereport(NOTICE,
						errmsg("skipping tid (%u, %u) for relation \"%s\" because it redirects to item %u",
							   blkno, offno, RelationGetRelationName(rel),
							   ItemIdGetRedirect(itemid)));
				continue;
			}
			else if (ItemIdIsDead(itemid))
			{
				ereport(NOTICE,
						(errmsg("skipping tid (%u, %u) for relation \"%s\" because it is marked dead",
								blkno, offno, RelationGetRelationName(rel))));
				continue;
			}
			else if (!ItemIdIsUsed(itemid))
			{
				ereport(NOTICE,
						(errmsg("skipping tid (%u, %u) for relation \"%s\" because it is marked unused",
								blkno, offno, RelationGetRelationName(rel))));
				continue;
			}

			/* Mark it for processing.
			 *
			 * 将其标记为待处理。
			 */
			Assert(offno < MaxHeapTuplesPerPage);
			include_this_tid[offno] = true;
		}

		/*
		 * Before entering the critical section, pin the visibility map page
		 * if it appears to be necessary.
		 *
		 * 在进入关键部分之前，如果有必要，请固定可见性地图页面。
		 */
		if (heap_force_opt == HEAP_FORCE_KILL && PageIsAllVisible(page))
			visibilitymap_pin(rel, blkno, &vmbuf);

		/* No ereport(ERROR) from here until all the changes are logged.
		 *
		 * 在记录所有更改之前，不会从这里报告（错误）。
		 */
		START_CRIT_SECTION();

		for (curoff = FirstOffsetNumber; curoff <= maxoffset;
			 curoff = OffsetNumberNext(curoff))
		{
			ItemId		itemid;

			if (!include_this_tid[curoff])
				continue;

			itemid = PageGetItemId(page, curoff);
			Assert(ItemIdIsNormal(itemid));

			did_modify_page = true;

			if (heap_force_opt == HEAP_FORCE_KILL)
			{
				ItemIdSetDead(itemid);

				/*
				 * If the page is marked all-visible, we must clear
				 * PD_ALL_VISIBLE flag on the page header and an all-visible
				 * bit on the visibility map corresponding to the page.
				 *
				 * 如果页面被标记为所有可见，我们必须清除页面标题上的 PD_ALL_VISIBLE 标志以及与该页面对应的可见性映射上的所有可见位。
				 */
				if (PageIsAllVisible(page))
				{
					PageClearAllVisible(page);
					visibilitymap_clear(rel, blkno, vmbuf,
										VISIBILITYMAP_VALID_BITS);
					did_modify_vm = true;
				}
			}
			else
			{
				HeapTupleHeader htup;

				Assert(heap_force_opt == HEAP_FORCE_FREEZE);

				htup = (HeapTupleHeader) PageGetItem(page, itemid);

				/*
				 * Reset all visibility-related fields of the tuple. This
				 * logic should mimic heap_execute_freeze_tuple(), but we
				 * choose to reset xmin and ctid just to be sure that no
				 * potentially-garbled data is left behind.
				 *
				 * 重置元组中所有与可见性相关的字段。这个逻辑应该模仿 heap_execute_freeze_tuple()，但我们选择重置 xmin 和 ctid 只是为了确保不会留下潜在的乱码数据。
				 */
				ItemPointerSet(&htup->t_ctid, blkno, curoff);
				HeapTupleHeaderSetXmin(htup, FrozenTransactionId);
				HeapTupleHeaderSetXmax(htup, InvalidTransactionId);
				if (htup->t_infomask & HEAP_MOVED)
				{
					if (htup->t_infomask & HEAP_MOVED_OFF)
						HeapTupleHeaderSetXvac(htup, InvalidTransactionId);
					else
						HeapTupleHeaderSetXvac(htup, FrozenTransactionId);
				}

				/*
				 * Clear all the visibility-related bits of this tuple and
				 * mark it as frozen. Also, get rid of HOT_UPDATED and
				 * KEYS_UPDATES bits.
				 *
				 * 清除该元组中所有与可见性相关的位并将其标记为冻结。另外，去掉 HOT_UPDATED 和 KEYS_UPDATES 位。
				 */
				htup->t_infomask &= ~HEAP_XACT_MASK;
				htup->t_infomask |= (HEAP_XMIN_FROZEN | HEAP_XMAX_INVALID);
				htup->t_infomask2 &= ~HEAP_HOT_UPDATED;
				htup->t_infomask2 &= ~HEAP_KEYS_UPDATED;
			}
		}

		/*
		 * If the page was modified, only then, we mark the buffer dirty or do
		 * the WAL logging.
		 *
		 * 如果页面被修改，只有那时我们才将缓冲区标记为脏或进行 WAL 日志记录。
		 */
		if (did_modify_page)
		{
			/* Mark buffer dirty before we write WAL.
			 *
			 * 在写入 WAL 之前将缓冲区标记为脏。
			 */
			MarkBufferDirty(buf);

			/* XLOG stuff
			 *
			 * XLOG 的东西
			 */
			if (RelationNeedsWAL(rel))
				log_newpage_buffer(buf, true);
		}

		/* WAL log the VM page if it was modified.
		 *
		 * WAL 记录 VM 页面（如果已修改）。
		 */
		if (did_modify_vm && RelationNeedsWAL(rel))
			log_newpage_buffer(vmbuf, false);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		if (vmbuf != InvalidBuffer)
			ReleaseBuffer(vmbuf);

		/* Update the current_start_ptr before moving to the next page.
		 *
		 * 在移动到下一页之前更新 current_start_ptr。
		 */
		curr_start_ptr = next_start_ptr;
	}

	relation_close(rel, RowExclusiveLock);

	pfree(ta);

	PG_RETURN_VOID();
}

/*-------------------------------------------------------------------------
 * tidcmp()
 *
 * Compare two item pointers, return -1, 0, or +1.
 *
 * 比较两个项指针，返回 -1、0 或 +1。
 *
 * See ItemPointerCompare for details.
 *
 * 有关详细信息，请参阅 ItemPointerCompare。
 * ------------------------------------------------------------------------
 */
static int32
tidcmp(const void *a, const void *b)
{
	ItemPointer iptr1 = ((const ItemPointer) a);
	ItemPointer iptr2 = ((const ItemPointer) b);

	return ItemPointerCompare(iptr1, iptr2);
}

/*-------------------------------------------------------------------------
 * sanity_check_tid_array()
 *
 * Perform sanity checks on the given tid array, and set *ntids to the
 * number of items in the array.
 *
 * 对给定的 tid 数组执行健全性检查，并将 *ntids 设置为数组中的项目数。
 * ------------------------------------------------------------------------
 */
static void
sanity_check_tid_array(ArrayType *ta, int *ntids)
{
	if (ARR_HASNULL(ta) && array_contains_nulls(ta))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	if (ARR_NDIM(ta) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("argument must be empty or one-dimensional array")));

	*ntids = ArrayGetNItems(ARR_NDIM(ta), ARR_DIMS(ta));
}

/*-------------------------------------------------------------------------
 * find_tids_one_page()
 *
 * Find all the tids residing in the same page as tids[next_start_ptr], and
 * update next_start_ptr so that it points to the first tid in the next page.
 *
 * 查找与tids[next_start_ptr]位于同一页面中的所有tids，并更新next_start_ptr，使其指向下一页中的第一个tid。
 *
 * NOTE: The input tids[] array must be sorted.
 *
 * 注意：输入的tids[]数组必须经过排序。
 * ------------------------------------------------------------------------
 */
static BlockNumber
find_tids_one_page(ItemPointer tids, int ntids, OffsetNumber *next_start_ptr)
{
	int			i;
	BlockNumber prev_blkno,
				blkno;

	prev_blkno = blkno = InvalidBlockNumber;

	for (i = *next_start_ptr; i < ntids; i++)
	{
		ItemPointerData tid = tids[i];

		blkno = ItemPointerGetBlockNumberNoCheck(&tid);

		if (i == *next_start_ptr)
			prev_blkno = blkno;

		if (prev_blkno != blkno)
			break;
	}

	*next_start_ptr = i;
	return prev_blkno;
}
