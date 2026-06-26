/*
 * contrib/pageinspect/btreefuncs.c
 *
 *
 * btreefuncs.c
 *
 * Copyright (c) 2006 Satoshi Nagayasu <nagayasus@nttdata.co.jp>
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(bt_metap);
PG_FUNCTION_INFO_V1(bt_page_items_1_9);
PG_FUNCTION_INFO_V1(bt_page_items);
PG_FUNCTION_INFO_V1(bt_page_items_bytea);
PG_FUNCTION_INFO_V1(bt_page_stats_1_9);
PG_FUNCTION_INFO_V1(bt_page_stats);
PG_FUNCTION_INFO_V1(bt_multi_page_stats);

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_BTREE(r) ((r)->rd_rel->relam == BTREE_AM_OID)

/* ------------------------------------------------
 * structure for single btree page statistics
 *
 * 单btree页面统计的结构
 * ------------------------------------------------
 */
typedef struct BTPageStat
{
	uint32		blkno;
	uint32		live_items;
	uint32		dead_items;
	uint32		page_size;
	uint32		max_avail;
	uint32		free_size;
	uint32		avg_item_size;
	char		type;

	/* opaque data
	 *
	 * 不透明的数据
	 */
	BlockNumber btpo_prev;
	BlockNumber btpo_next;
	uint32		btpo_level;
	uint16		btpo_flags;
	BTCycleId	btpo_cycleid;
} BTPageStat;

/*
 * cross-call data structure for SRF for page stats
 *
 * 用于页面统计的 SRF 的交叉调用数据结构
 */
typedef struct ua_page_stats
{
	Oid			relid;
	int64		blkno;
	int64		blk_count;
	bool		allpages;
} ua_page_stats;

/*
 * cross-call data structure for SRF for page items
 *
 * 页面项 SRF 的交叉调用数据结构
 */
typedef struct ua_page_items
{
	Page		page;
	OffsetNumber offset;
	bool		leafpage;
	bool		rightmost;
	TupleDesc	tupd;
} ua_page_items;


/* -------------------------------------------------
 * GetBTPageStatistics()
 *
 * Collect statistics of single b-tree page
 *
 * 收集单个b树页面的统计信息
 * -------------------------------------------------
 */
static void
GetBTPageStatistics(BlockNumber blkno, Buffer buffer, BTPageStat *stat)
{
	Page		page = BufferGetPage(buffer);
	PageHeader	phdr = (PageHeader) page;
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	BTPageOpaque opaque = BTPageGetOpaque(page);
	int			item_size = 0;
	int			off;

	stat->blkno = blkno;

	stat->max_avail = BLCKSZ - (BLCKSZ - phdr->pd_special + SizeOfPageHeaderData);

	stat->dead_items = stat->live_items = 0;

	stat->page_size = PageGetPageSize(page);

	/* page type (flags)
	 *
	 * 页面类型（标志）
	 */
	if (P_ISDELETED(opaque))
	{
		/* We divide deleted pages into leaf ('d') or internal ('D')
		 *
		 * 我们将删除的页面分为叶（'d'）或内部（'D'）
		 */
		if (P_ISLEAF(opaque) || !P_HAS_FULLXID(opaque))
			stat->type = 'd';
		else
			stat->type = 'D';

		/*
		 * Report safexid in a deleted page.
		 *
		 * 在已删除的页面中报告 safexid。
		 *
		 * Handle pg_upgrade'd deleted pages that used the previous safexid
		 * representation in btpo_level field (this used to be a union type
		 * called "bpto").
		 *
		 * 处理 pg_upgrade 删除的页面，这些页面在 btpo_level 字段中使用了先前的 safexid 表示（这曾经是一个名为“bpto”的联合类型）。
		 */
		if (P_HAS_FULLXID(opaque))
		{
			FullTransactionId safexid = BTPageGetDeleteXid(page);

			elog(DEBUG2, "deleted page from block %u has safexid %u:%u",
				 blkno, EpochFromFullTransactionId(safexid),
				 XidFromFullTransactionId(safexid));
		}
		else
			elog(DEBUG2, "deleted page from block %u has safexid %u",
				 blkno, opaque->btpo_level);

		/* Don't interpret BTDeletedPageData as index tuples
		 *
		 * 不要将 BTDeletedPageData 解释为索引元组
		 */
		maxoff = InvalidOffsetNumber;
	}
	else if (P_IGNORE(opaque))
		stat->type = 'e';
	else if (P_ISLEAF(opaque))
		stat->type = 'l';
	else if (P_ISROOT(opaque))
		stat->type = 'r';
	else
		stat->type = 'i';

	/* btpage opaque data
	 *
	 * btpage 不透明数据
	 */
	stat->btpo_prev = opaque->btpo_prev;
	stat->btpo_next = opaque->btpo_next;
	stat->btpo_level = opaque->btpo_level;
	stat->btpo_flags = opaque->btpo_flags;
	stat->btpo_cycleid = opaque->btpo_cycleid;

	/* count live and dead tuples, and free space
	 *
	 * 计算活元组和死元组以及可用空间
	 */
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		IndexTuple	itup;

		ItemId		id = PageGetItemId(page, off);

		itup = (IndexTuple) PageGetItem(page, id);

		item_size += IndexTupleSize(itup);

		if (!ItemIdIsDead(id))
			stat->live_items++;
		else
			stat->dead_items++;
	}
	stat->free_size = PageGetFreeSpace(page);

	if ((stat->live_items + stat->dead_items) > 0)
		stat->avg_item_size = item_size / (stat->live_items + stat->dead_items);
	else
		stat->avg_item_size = 0;
}

/* -----------------------------------------------
 * check_relation_block_range()
 *
 * Verify that a block number (given as int64) is valid for the relation.
 *
 * 验证块号（以 int64 形式给出）对于关系是否有效。
 * -----------------------------------------------
 */
static void
check_relation_block_range(Relation rel, int64 blkno)
{
	/* Ensure we can cast to BlockNumber
	 *
	 * 确保我们可以转换为 BlockNumber
	 */
	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number %" PRId64, blkno)));

	if ((BlockNumber) (blkno) >= RelationGetNumberOfBlocks(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %" PRId64 " is out of range", blkno)));
}

/* -----------------------------------------------
 * bt_index_block_validate()
 *
 * Validate index type is btree and block number
 * is valid (and not the metapage).
 *
 * 验证索引类型是 btree 并且块号有效（而不是元页）。
 * -----------------------------------------------
 */
static void
bt_index_block_validate(Relation rel, int64 blkno)
{
	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index",
						RelationGetRelationName(rel), "btree")));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 *
	 * 拒绝读取非本地临时关系的尝试；我们可能会得到错误的数据，因为我们看不到拥有会话的本地缓冲区。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (blkno == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block 0 is a meta page")));

	check_relation_block_range(rel, blkno);
}

/* -----------------------------------------------
 * bt_page_stats()
 *
 * Usage: SELECT * FROM bt_page_stats('t1_pkey', 1);
 * Arguments are index relation name and block number
 *
 * 用法： SELECT * FROM bt_page_stats('t1_pkey', 1);参数是索引关系名称和块号
 * -----------------------------------------------
 */
static Datum
bt_page_stats_internal(PG_FUNCTION_ARGS, enum pageinspect_version ext_version)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	int64		blkno = (ext_version == PAGEINSPECT_V1_8 ? PG_GETARG_UINT32(1) : PG_GETARG_INT64(1));
	Buffer		buffer;
	Relation	rel;
	RangeVar   *relrv;
	Datum		result;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[11];
	BTPageStat	stat;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pageinspect functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	bt_index_block_validate(rel, blkno);

	buffer = ReadBuffer(rel, blkno);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/* keep compiler quiet
	 *
	 * 保持编译器安静
	 */
	stat.btpo_prev = stat.btpo_next = InvalidBlockNumber;
	stat.btpo_flags = stat.free_size = stat.avg_item_size = 0;

	GetBTPageStatistics(blkno, buffer, &stat);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	/* Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	j = 0;
	values[j++] = psprintf("%u", stat.blkno);
	values[j++] = psprintf("%c", stat.type);
	values[j++] = psprintf("%u", stat.live_items);
	values[j++] = psprintf("%u", stat.dead_items);
	values[j++] = psprintf("%u", stat.avg_item_size);
	values[j++] = psprintf("%u", stat.page_size);
	values[j++] = psprintf("%u", stat.free_size);
	values[j++] = psprintf("%u", stat.btpo_prev);
	values[j++] = psprintf("%u", stat.btpo_next);
	values[j++] = psprintf("%u", stat.btpo_level);
	values[j++] = psprintf("%d", stat.btpo_flags);

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

Datum
bt_page_stats_1_9(PG_FUNCTION_ARGS)
{
	return bt_page_stats_internal(fcinfo, PAGEINSPECT_V1_9);
}

/* entry point for old extension version
 *
 * 旧扩展版本的入口点
 */
Datum
bt_page_stats(PG_FUNCTION_ARGS)
{
	return bt_page_stats_internal(fcinfo, PAGEINSPECT_V1_8);
}


/* -----------------------------------------------
 * bt_multi_page_stats()
 *
 * Usage: SELECT * FROM bt_page_stats('t1_pkey', 1, 2);
 * Arguments are index relation name, first block number, number of blocks
 * (but number of blocks can be negative to mean "read all the rest")
 *
 * 用法： SELECT * FROM bt_page_stats('t1_pkey', 1, 2);参数是索引关系名称、第一个块编号、块数（但块数可以为负数，表示“读取其余所有内容”）
 * -----------------------------------------------
 */
Datum
bt_multi_page_stats(PG_FUNCTION_ARGS)
{
	Relation	rel;
	ua_page_stats *uargs;
	FuncCallContext *fctx;
	MemoryContext mctx;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pageinspect functions")));

	if (SRF_IS_FIRSTCALL())
	{
		text	   *relname = PG_GETARG_TEXT_PP(0);
		int64		blkno = PG_GETARG_INT64(1);
		int64		blk_count = PG_GETARG_INT64(2);
		RangeVar   *relrv;

		fctx = SRF_FIRSTCALL_INIT();

		relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
		rel = relation_openrv(relrv, AccessShareLock);

		/* Check that rel is a valid btree index and 1st block number is OK
		 *
		 * 检查 rel 是否是有效的 btree 索引并且第一个块号是否正确
		 */
		bt_index_block_validate(rel, blkno);

		/*
		 * Check if upper bound of the specified range is valid. If only one
		 * page is requested, skip as we've already validated the page. (Also,
		 * it's important to skip this if blk_count is negative.)
		 *
		 * 检查指定范围的上限是否有效。如果仅请求一页，请跳过，因为我们已经验证了该页面。 （此外，如果 blk_count 为负数，请务必跳过此操作。）
		 */
		if (blk_count > 1)
			check_relation_block_range(rel, blkno + blk_count - 1);

		/* Save arguments for reuse
		 *
		 * 保存参数以供重用
		 */
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(ua_page_stats));

		uargs->relid = RelationGetRelid(rel);
		uargs->blkno = blkno;
		uargs->blk_count = blk_count;
		uargs->allpages = (blk_count < 0);

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);

		/*
		 * To avoid possibly leaking a relcache reference if the SRF isn't run
		 * to completion, we close and re-open the index rel each time
		 * through, using the index's OID for re-opens to ensure we get the
		 * same rel.  Keep the AccessShareLock though, to ensure it doesn't go
		 * away underneath us.
		 *
		 * 为了避免在 SRF 未运行完成时可能泄漏 relcache 引用，我们每次都会关闭并重新打开索引 rel，使用索引的 OID 重新打开以确保我们获得相同的 rel。  不过，请保留 AccessShareLock，以确保它不会在我们脚下消失。
		 */
		relation_close(rel, NoLock);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	/* We should have lock already
	 *
	 * 我们应该已经有锁了
	 */
	rel = relation_open(uargs->relid, NoLock);

	/* In all-pages mode, recheck the index length each time
	 *
	 * 全页模式下，每次重新检查索引长度
	 */
	if (uargs->allpages)
		uargs->blk_count = RelationGetNumberOfBlocks(rel) - uargs->blkno;

	if (uargs->blk_count > 0)
	{
		/* We need to fetch next block statistics
		 *
		 * 我们需要获取下一个块的统计信息
		 */
		Buffer		buffer;
		Datum		result;
		HeapTuple	tuple;
		int			j;
		char	   *values[11];
		BTPageStat	stat;
		TupleDesc	tupleDesc;

		buffer = ReadBuffer(rel, uargs->blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		/* keep compiler quiet
		 *
		 * 保持编译器安静
		 */
		stat.btpo_prev = stat.btpo_next = InvalidBlockNumber;
		stat.btpo_flags = stat.free_size = stat.avg_item_size = 0;

		GetBTPageStatistics(uargs->blkno, buffer, &stat);

		UnlockReleaseBuffer(buffer);
		relation_close(rel, NoLock);

		/* Build a tuple descriptor for our result type
		 *
		 * 为我们的结果类型构建一个元组描述符
		 */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		j = 0;
		values[j++] = psprintf("%u", stat.blkno);
		values[j++] = psprintf("%c", stat.type);
		values[j++] = psprintf("%u", stat.live_items);
		values[j++] = psprintf("%u", stat.dead_items);
		values[j++] = psprintf("%u", stat.avg_item_size);
		values[j++] = psprintf("%u", stat.page_size);
		values[j++] = psprintf("%u", stat.free_size);
		values[j++] = psprintf("%u", stat.btpo_prev);
		values[j++] = psprintf("%u", stat.btpo_next);
		values[j++] = psprintf("%u", stat.btpo_level);
		values[j++] = psprintf("%d", stat.btpo_flags);

		/* Construct tuple to be returned
		 *
		 * 构造要返回的元组
		 */
		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
									   values);

		result = HeapTupleGetDatum(tuple);

		/*
		 * Move to the next block number and decrement the number of blocks
		 * still to be fetched
		 *
		 * 移动到下一个块号并减少仍要获取的块数
		 */
		uargs->blkno++;
		uargs->blk_count--;

		SRF_RETURN_NEXT(fctx, result);
	}

	/* Done, so finally we can release the index lock
	 *
	 * 完成了，终于可以释放索引锁了
	 */
	relation_close(rel, AccessShareLock);
	SRF_RETURN_DONE(fctx);
}

/*-------------------------------------------------------
 * bt_page_print_tuples()
 *
 * Form a tuple describing index tuple at a given offset
 *
 * 形成一个描述给定偏移处索引元组的元组
 * ------------------------------------------------------
 */
static Datum
bt_page_print_tuples(ua_page_items *uargs)
{
	Page		page = uargs->page;
	OffsetNumber offset = uargs->offset;
	bool		leafpage = uargs->leafpage;
	bool		rightmost = uargs->rightmost;
	bool		ispivottuple;
	Datum		values[9];
	bool		nulls[9];
	HeapTuple	tuple;
	ItemId		id;
	IndexTuple	itup;
	int			j;
	int			off;
	int			dlen;
	char	   *dump,
			   *datacstring;
	char	   *ptr;
	ItemPointer htid;

	id = PageGetItemId(page, offset);

	if (!ItemIdIsValid(id))
		elog(ERROR, "invalid ItemId");

	itup = (IndexTuple) PageGetItem(page, id);

	j = 0;
	memset(nulls, 0, sizeof(nulls));
	values[j++] = DatumGetInt16(offset);
	values[j++] = ItemPointerGetDatum(&itup->t_tid);
	values[j++] = Int32GetDatum((int) IndexTupleSize(itup));
	values[j++] = BoolGetDatum(IndexTupleHasNulls(itup));
	values[j++] = BoolGetDatum(IndexTupleHasVarwidths(itup));

	ptr = (char *) itup + IndexInfoFindDataOffset(itup->t_info);
	dlen = IndexTupleSize(itup) - IndexInfoFindDataOffset(itup->t_info);

	/*
	 * Make sure that "data" column does not include posting list or pivot
	 * tuple representation of heap TID(s).
	 *
	 * 确保“数据”列不包括堆 TID 的发布列表或枢轴元组表示。
	 *
	 * Note: BTreeTupleIsPivot() won't work reliably on !heapkeyspace indexes
	 * (those built before BTREE_VERSION 4), but we have no way of determining
	 * if this page came from a !heapkeyspace index.  We may only have a bytea
	 * nbtree page image to go on, so in general there is no metapage that we
	 * can check.
	 *
	 * 注意：BTreeTupleIsPivot() 无法在 !heapkeyspace 索引（在 BTREE_VERSION 4 之前构建的索引）上可靠地工作，但我们无法确定此页面是否来自 !heapkeyspace 索引。  我们可能只有一个bytea nbtree页面图像可以继续，所以通常没有我们可以检查的元页面。
	 *
	 * That's okay here because BTreeTupleIsPivot() can only return false for
	 * a !heapkeyspace pivot, never true for a !heapkeyspace non-pivot.  Since
	 * heap TID isn't part of the keyspace in a !heapkeyspace index anyway,
	 * there cannot possibly be a pivot tuple heap TID representation that we
	 * fail to make an adjustment for.  A !heapkeyspace index can have
	 * BTreeTupleIsPivot() return true (due to things like suffix truncation
	 * for INCLUDE indexes in Postgres v11), but when that happens
	 * BTreeTupleGetHeapTID() can be trusted to work reliably (i.e. return
	 * NULL).
	 *
	 * 这里没关系，因为 BTreeTupleIsPivot() 只能对 !heapkeyspace 枢轴返回 false，而对 !heapkeyspace 非枢轴永远不会返回 true。  由于堆 TID 无论如何都不是 !heapkeyspace 索引中键空间的一部分，因此不可能存在我们未能对其进行调整的主元组堆 TID 表示形式。  !heapkeyspace 索引可以让 BTreeTupleIsPivot() 返回 true （由于 Postgres v11 中 INCLUDE 索引的后缀截断等原因），但当发生这种情况时，可以相信 BTreeTupleGetHeapTID() 可以可靠地工作（即返回 NULL）。
	 *
	 * Note: BTreeTupleIsPosting() always works reliably, even with
	 * !heapkeyspace indexes.
	 *
	 * 注意：BTreeTupleIsPosting() 始终可靠地工作，即使使用 !heapkeyspace 索引也是如此。
	 */
	if (BTreeTupleIsPosting(itup))
		dlen -= IndexTupleSize(itup) - BTreeTupleGetPostingOffset(itup);
	else if (BTreeTupleIsPivot(itup) && BTreeTupleGetHeapTID(itup) != NULL)
		dlen -= MAXALIGN(sizeof(ItemPointerData));

	if (dlen < 0 || dlen > INDEX_SIZE_MASK)
		elog(ERROR, "invalid tuple length %d for tuple at offset number %u",
			 dlen, offset);
	dump = palloc0(dlen * 3 + 1);
	datacstring = dump;
	for (off = 0; off < dlen; off++)
	{
		if (off > 0)
			*dump++ = ' ';
		sprintf(dump, "%02x", *(ptr + off) & 0xff);
		dump += 2;
	}
	values[j++] = CStringGetTextDatum(datacstring);
	pfree(datacstring);

	/*
	 * We need to work around the BTreeTupleIsPivot() !heapkeyspace limitation
	 * again.  Deduce whether or not tuple must be a pivot tuple based on
	 * whether or not the page is a leaf page, as well as the page offset
	 * number of the tuple.
	 *
	 * 我们需要再次解决 BTreeTupleIsPivot() !heapkeyspace 限制。  根据该页是否是叶子页以及该元组的页偏移量来推断该元组是否必须是主元组。
	 */
	ispivottuple = (!leafpage || (!rightmost && offset == P_HIKEY));

	/* LP_DEAD bit can never be set for pivot tuples, so show a NULL there
	 *
	 * 永远不能为枢轴元组设置 LP_DEAD 位，因此在那里显示 NULL
	 */
	if (!ispivottuple)
		values[j++] = BoolGetDatum(ItemIdIsDead(id));
	else
	{
		Assert(!ItemIdIsDead(id));
		nulls[j++] = true;
	}

	htid = BTreeTupleGetHeapTID(itup);
	if (ispivottuple && !BTreeTupleIsPivot(itup))
	{
		/* Don't show bogus heap TID in !heapkeyspace pivot tuple
		 *
		 * 不要在 !heapkeyspace 枢轴元组中显示伪造的堆 TID
		 */
		htid = NULL;
	}

	if (htid)
		values[j++] = ItemPointerGetDatum(htid);
	else
		nulls[j++] = true;

	if (BTreeTupleIsPosting(itup))
	{
		/* Build an array of item pointers
		 *
		 * 构建项目指针数组
		 */
		ItemPointer tids;
		Datum	   *tids_datum;
		int			nposting;

		tids = BTreeTupleGetPosting(itup);
		nposting = BTreeTupleGetNPosting(itup);
		tids_datum = (Datum *) palloc(nposting * sizeof(Datum));
		for (int i = 0; i < nposting; i++)
			tids_datum[i] = ItemPointerGetDatum(&tids[i]);
		values[j++] = PointerGetDatum(construct_array_builtin(tids_datum, nposting, TIDOID));
		pfree(tids_datum);
	}
	else
		nulls[j++] = true;

	/* Build and return the result tuple
	 *
	 * 构建并返回结果元组
	 */
	tuple = heap_form_tuple(uargs->tupd, values, nulls);

	return HeapTupleGetDatum(tuple);
}

/*-------------------------------------------------------
 * bt_page_items()
 *
 * Get IndexTupleData set in a btree page
 *
 * 获取 btree 页面中的 IndexTupleData 集
 *
 * Usage: SELECT * FROM bt_page_items('t1_pkey', 1);
 *
 * 用法： SELECT * FROM bt_page_items('t1_pkey', 1);
 *-------------------------------------------------------
 */
static Datum
bt_page_items_internal(PG_FUNCTION_ARGS, enum pageinspect_version ext_version)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	int64		blkno = (ext_version == PAGEINSPECT_V1_8 ? PG_GETARG_UINT32(1) : PG_GETARG_INT64(1));
	Datum		result;
	FuncCallContext *fctx;
	MemoryContext mctx;
	ua_page_items *uargs;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pageinspect functions")));

	if (SRF_IS_FIRSTCALL())
	{
		RangeVar   *relrv;
		Relation	rel;
		Buffer		buffer;
		BTPageOpaque opaque;
		TupleDesc	tupleDesc;

		fctx = SRF_FIRSTCALL_INIT();

		relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
		rel = relation_openrv(relrv, AccessShareLock);

		bt_index_block_validate(rel, blkno);

		buffer = ReadBuffer(rel, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		/*
		 * We copy the page into local storage to avoid holding pin on the
		 * buffer longer than we must, and possibly failing to release it at
		 * all if the calling query doesn't fetch all rows.
		 *
		 * 我们将页面复制到本地存储中，以避免在缓冲区上保持 pin 的时间超过我们必须的时间，并且如果调用查询未获取所有行，则可能根本无法释放它。
		 */
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(ua_page_items));

		uargs->page = palloc(BLCKSZ);
		memcpy(uargs->page, BufferGetPage(buffer), BLCKSZ);

		UnlockReleaseBuffer(buffer);
		relation_close(rel, AccessShareLock);

		uargs->offset = FirstOffsetNumber;

		opaque = BTPageGetOpaque(uargs->page);

		if (!P_ISDELETED(opaque))
			fctx->max_calls = PageGetMaxOffsetNumber(uargs->page);
		else
		{
			/* Don't interpret BTDeletedPageData as index tuples
			 *
			 * 不要将 BTDeletedPageData 解释为索引元组
			 */
			elog(NOTICE, "page from block " INT64_FORMAT " is deleted", blkno);
			fctx->max_calls = 0;
		}
		uargs->leafpage = P_ISLEAF(opaque);
		uargs->rightmost = P_RIGHTMOST(opaque);

		/* Build a tuple descriptor for our result type
		 *
		 * 为我们的结果类型构建一个元组描述符
		 */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		tupleDesc = BlessTupleDesc(tupleDesc);

		uargs->tupd = tupleDesc;

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		result = bt_page_print_tuples(uargs);
		uargs->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}

Datum
bt_page_items_1_9(PG_FUNCTION_ARGS)
{
	return bt_page_items_internal(fcinfo, PAGEINSPECT_V1_9);
}

/* entry point for old extension version
 *
 * 旧扩展版本的入口点
 */
Datum
bt_page_items(PG_FUNCTION_ARGS)
{
	return bt_page_items_internal(fcinfo, PAGEINSPECT_V1_8);
}

/*-------------------------------------------------------
 * bt_page_items_bytea()
 *
 * Get IndexTupleData set in a btree page
 *
 * 获取 btree 页面中的 IndexTupleData 集
 *
 * Usage: SELECT * FROM bt_page_items(get_raw_page('t1_pkey', 1));
 *
 * 用法： SELECT * FROM bt_page_items(get_raw_page('t1_pkey', 1));
 *-------------------------------------------------------
 */

Datum
bt_page_items_bytea(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	Datum		result;
	FuncCallContext *fctx;
	ua_page_items *uargs;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (SRF_IS_FIRSTCALL())
	{
		BTPageOpaque opaque;
		MemoryContext mctx;
		TupleDesc	tupleDesc;

		fctx = SRF_FIRSTCALL_INIT();
		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		uargs = palloc(sizeof(ua_page_items));

		uargs->page = get_page_from_raw(raw_page);

		if (PageIsNew(uargs->page))
		{
			MemoryContextSwitchTo(mctx);
			PG_RETURN_NULL();
		}

		uargs->offset = FirstOffsetNumber;

		/* verify the special space has the expected size
		 *
		 * 验证特殊空间是否具有预期的大小
		 */
		if (PageGetSpecialSize(uargs->page) != MAXALIGN(sizeof(BTPageOpaqueData)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("input page is not a valid %s page", "btree"),
					 errdetail("Expected special size %d, got %d.",
							   (int) MAXALIGN(sizeof(BTPageOpaqueData)),
							   (int) PageGetSpecialSize(uargs->page))));

		opaque = BTPageGetOpaque(uargs->page);

		if (P_ISMETA(opaque))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block is a meta page")));

		if (P_ISLEAF(opaque) && opaque->btpo_level != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("block is not a valid btree leaf page")));

		if (P_ISDELETED(opaque))
			elog(NOTICE, "page is deleted");

		if (!P_ISDELETED(opaque))
			fctx->max_calls = PageGetMaxOffsetNumber(uargs->page);
		else
		{
			/* Don't interpret BTDeletedPageData as index tuples
			 *
			 * 不要将 BTDeletedPageData 解释为索引元组
			 */
			elog(NOTICE, "page from block is deleted");
			fctx->max_calls = 0;
		}
		uargs->leafpage = P_ISLEAF(opaque);
		uargs->rightmost = P_RIGHTMOST(opaque);

		/* Build a tuple descriptor for our result type
		 *
		 * 为我们的结果类型构建一个元组描述符
		 */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		tupleDesc = BlessTupleDesc(tupleDesc);

		uargs->tupd = tupleDesc;

		fctx->user_fctx = uargs;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();
	uargs = fctx->user_fctx;

	if (fctx->call_cntr < fctx->max_calls)
	{
		result = bt_page_print_tuples(uargs);
		uargs->offset++;
		SRF_RETURN_NEXT(fctx, result);
	}

	SRF_RETURN_DONE(fctx);
}

/* Number of output arguments (columns) for bt_metap()
 *
 * bt_metap() 的输出参数（列）数
 */
#define BT_METAP_COLS_V1_8		9

/* ------------------------------------------------
 * bt_metap()
 *
 * Get a btree's meta-page information
 *
 * 获取btree的元页面信息
 *
 * Usage: SELECT * FROM bt_metap('t1_pkey')
 *
 * 用法： SELECT * FROM bt_metap('t1_pkey')
 * ------------------------------------------------
 */
Datum
bt_metap(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Datum		result;
	Relation	rel;
	RangeVar   *relrv;
	BTMetaPageData *metad;
	TupleDesc	tupleDesc;
	int			j;
	char	   *values[9];
	Buffer		buffer;
	Page		page;
	HeapTuple	tuple;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pageinspect functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index",
						RelationGetRelationName(rel), "btree")));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 *
	 * 拒绝读取非本地临时关系的尝试；我们可能会得到错误的数据，因为我们看不到拥有会话的本地缓冲区。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	buffer = ReadBuffer(rel, 0);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	page = BufferGetPage(buffer);
	metad = BTPageGetMeta(page);

	/* Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * We need a kluge here to detect API versions prior to 1.8.  Earlier
	 * versions incorrectly used int4 for certain columns.
	 *
	 * 我们需要一个 kluge 来检测 1.8 之前的 API 版本。  早期版本错误地对某些列使用了 int4。
	 *
	 * There is no way to reliably avoid the problems created by the old
	 * function definition at this point, so insist that the user update the
	 * extension.
	 *
	 * 目前没有办法可靠地避免旧函数定义造成的问题，因此坚持要求用户更新扩展。
	 */
	if (tupleDesc->natts < BT_METAP_COLS_V1_8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function has wrong number of declared columns"),
				 errhint("To resolve the problem, update the \"pageinspect\" extension to the latest version.")));

	j = 0;
	values[j++] = psprintf("%d", metad->btm_magic);
	values[j++] = psprintf("%d", metad->btm_version);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_root);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_level);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_fastroot);
	values[j++] = psprintf(INT64_FORMAT, (int64) metad->btm_fastlevel);

	/*
	 * Get values of extended metadata if available, use default values
	 * otherwise.  Note that we rely on the assumption that btm_allequalimage
	 * is initialized to zero with indexes that were built on versions prior
	 * to Postgres 13 (just like _bt_metaversion()).
	 *
	 * 获取扩展元数据的值（如果可用），否则使用默认值。  请注意，我们依赖于这样的假设：btm_allequalimage 使用在 Postgres 13 之前的版本上构建的索引（就像 _bt_metaversion() 一样）初始化为零。
	 */
	if (metad->btm_version >= BTREE_NOVAC_VERSION)
	{
		values[j++] = psprintf(INT64_FORMAT,
							   (int64) metad->btm_last_cleanup_num_delpages);
		values[j++] = psprintf("%f", metad->btm_last_cleanup_num_heap_tuples);
		values[j++] = metad->btm_allequalimage ? "t" : "f";
	}
	else
	{
		values[j++] = "0";
		values[j++] = "-1";
		values[j++] = "f";
	}

	tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
								   values);

	result = HeapTupleGetDatum(tuple);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	PG_RETURN_DATUM(result);
}
