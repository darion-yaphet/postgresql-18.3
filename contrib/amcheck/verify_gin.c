/*-------------------------------------------------------------------------
 *
 * verify_gin.c
 *		Verifies the integrity of GIN indexes based on invariants.
 *
 *
 * GIN index verification checks a number of invariants:
 *
 * - consistency: Paths in GIN graph have to contain consistent keys: tuples
 *   on parent pages consistently include tuples from children pages.
 *
 * - graph invariants: Each internal page must have at least one downlink, and
 *   can reference either only leaf pages or only internal pages.
 *
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_gin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin_private.h"
#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "verify_common.h"
#include "string.h"

/*
 * GinScanItem represents one item of depth-first scan of the index.
 *
 * GinScanItem 表示索引的深度优先扫描的一项。
 */
typedef struct GinScanItem
{
	int			depth;
	IndexTuple	parenttup;
	BlockNumber parentblk;
	BlockNumber blkno;
	struct GinScanItem *next;
} GinScanItem;

/*
 * GinPostingTreeScanItem represents one item of a depth-first posting tree scan.
 *
 * GinPostingTreeScanItem 表示深度优先发布树扫描的一项。
 */
typedef struct GinPostingTreeScanItem
{
	int			depth;
	ItemPointerData parentkey;
	BlockNumber parentblk;
	BlockNumber blkno;
	struct GinPostingTreeScanItem *next;
} GinPostingTreeScanItem;


PG_FUNCTION_INFO_V1(gin_index_check);

static void gin_check_parent_keys_consistency(Relation rel,
											  Relation heaprel,
											  void *callback_state, bool readonly);
static void check_index_page(Relation rel, Buffer buffer, BlockNumber blockNo);
static IndexTuple gin_refind_parent(Relation rel,
									BlockNumber parentblkno,
									BlockNumber childblkno,
									BufferAccessStrategy strategy);
static ItemId PageGetItemIdCareful(Relation rel, BlockNumber block, Page page,
								   OffsetNumber offset);

/*
 * gin_index_check(index regclass)
 *
 * gin_index_check(索引regclass)
 *
 * Verify integrity of GIN index.
 *
 * 验证 GIN 索引的完整性。
 *
 * Acquires AccessShareLock on heap & index relations.
 *
 * 获取堆和索引关系上的 AccessShareLock。
 */
Datum
gin_index_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);

	amcheck_lock_relation_and_check(indrelid,
									GIN_AM_OID,
									gin_check_parent_keys_consistency,
									AccessShareLock,
									NULL);

	PG_RETURN_VOID();
}

/*
 * Read item pointers from leaf entry tuple.
 *
 * 从叶条目元组读取项目指针。
 *
 * Returns a palloc'd array of ItemPointers. The number of items is returned
 * in *nitems.
 *
 * 返回一个 palloc 的 ItemPointers 数组。项目数以 *nitems 形式返回。
 */
static ItemPointer
ginReadTupleWithoutState(IndexTuple itup, int *nitems)
{
	Pointer		ptr = GinGetPosting(itup);
	int			nipd = GinGetNPosting(itup);
	ItemPointer ipd;
	int			ndecoded;

	if (GinItupIsCompressed(itup))
	{
		if (nipd > 0)
		{
			ipd = ginPostingListDecode((GinPostingList *) ptr, &ndecoded);
			if (nipd != ndecoded)
				elog(ERROR, "number of items mismatch in GIN entry tuple, %d in tuple header, %d decoded",
					 nipd, ndecoded);
		}
		else
			ipd = palloc(0);
	}
	else
	{
		ipd = (ItemPointer) palloc(sizeof(ItemPointerData) * nipd);
		memcpy(ipd, ptr, sizeof(ItemPointerData) * nipd);
	}
	*nitems = nipd;
	return ipd;
}

/*
 * Scans through a posting tree (given by the root), and verifies that the keys
 * on a child keys are consistent with the parent.
 *
 * 扫描发布树（由根给出），并验证子密钥上的密钥与父密钥一致。
 *
 * Allocates a separate memory context and scans through posting tree graph.
 *
 * 分配单独的内存上下文并扫描发布树图。
 */
static void
gin_check_posting_tree_parent_keys_consistency(Relation rel, BlockNumber posting_tree_root)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	GinPostingTreeScanItem *stack;
	MemoryContext mctx;
	MemoryContext oldcontext;

	int			leafdepth;

	mctx = AllocSetContextCreate(CurrentMemoryContext,
								 "posting tree check context",
								 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(mctx);

	/*
	 * We don't know the height of the tree yet, but as soon as we encounter a
	 * leaf page, we will set 'leafdepth' to its depth.
	 *
	 * 我们还不知道树的高度，但是一旦遇到叶子页面，我们就会将 'leafdepth' 设置为它的深度。
	 */
	leafdepth = -1;

	/* Start the scan at the root page
	 *
	 * 从根页面开始扫描
	 */
	stack = (GinPostingTreeScanItem *) palloc0(sizeof(GinPostingTreeScanItem));
	stack->depth = 0;
	ItemPointerSetInvalid(&stack->parentkey);
	stack->parentblk = InvalidBlockNumber;
	stack->blkno = posting_tree_root;

	elog(DEBUG3, "processing posting tree at blk %u", posting_tree_root);

	while (stack)
	{
		GinPostingTreeScanItem *stack_next;
		Buffer		buffer;
		Page		page;
		OffsetNumber i,
					maxoff;
		BlockNumber rightlink;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, stack->blkno,
									RBM_NORMAL, strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);

		Assert(GinPageIsData(page));

		/* Check that the tree has the same height in all branches
		 *
		 * 检查树的所有分支的高度是否相同
		 */
		if (GinPageIsLeaf(page))
		{
			ItemPointerData minItem;
			int			nlist;
			ItemPointerData *list;
			char		tidrange_buf[MAXPGPATH];

			ItemPointerSetMin(&minItem);

			elog(DEBUG1, "page blk: %u, type leaf", stack->blkno);

			if (leafdepth == -1)
				leafdepth = stack->depth;
			else if (stack->depth != leafdepth)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": internal pages traversal encountered leaf page unexpectedly on block %u",
								RelationGetRelationName(rel), stack->blkno)));
			list = GinDataLeafPageGetItems(page, &nlist, minItem);

			if (nlist > 0)
				snprintf(tidrange_buf, sizeof(tidrange_buf),
						 "%d tids (%u, %u) - (%u, %u)",
						 nlist,
						 ItemPointerGetBlockNumberNoCheck(&list[0]),
						 ItemPointerGetOffsetNumberNoCheck(&list[0]),
						 ItemPointerGetBlockNumberNoCheck(&list[nlist - 1]),
						 ItemPointerGetOffsetNumberNoCheck(&list[nlist - 1]));
			else
				snprintf(tidrange_buf, sizeof(tidrange_buf), "0 tids");

			if (stack->parentblk != InvalidBlockNumber)
				elog(DEBUG3, "blk %u: parent %u highkey (%u, %u), %s",
					 stack->blkno,
					 stack->parentblk,
					 ItemPointerGetBlockNumberNoCheck(&stack->parentkey),
					 ItemPointerGetOffsetNumberNoCheck(&stack->parentkey),
					 tidrange_buf);
			else
				elog(DEBUG3, "blk %u: root leaf, %s",
					 stack->blkno,
					 tidrange_buf);

			if (stack->parentblk != InvalidBlockNumber &&
				ItemPointerGetOffsetNumberNoCheck(&stack->parentkey) != InvalidOffsetNumber &&
				nlist > 0 && ItemPointerCompare(&stack->parentkey, &list[nlist - 1]) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": tid exceeds parent's high key in postingTree leaf on block %u",
								RelationGetRelationName(rel), stack->blkno)));
		}
		else
		{
			LocationIndex pd_lower;
			ItemPointerData bound;
			int			lowersize;

			/*
			 * Check that tuples in each page are properly ordered and
			 * consistent with parent high key
			 *
			 * 检查每个页面中的元组是否正确排序并与父高键一致
			 */
			maxoff = GinPageGetOpaque(page)->maxoff;
			rightlink = GinPageGetOpaque(page)->rightlink;

			elog(DEBUG1, "page blk: %u, type data, maxoff %d", stack->blkno, maxoff);

			if (stack->parentblk != InvalidBlockNumber)
				elog(DEBUG3, "blk %u: internal posting tree page with %u items, parent %u highkey (%u, %u)",
					 stack->blkno, maxoff, stack->parentblk,
					 ItemPointerGetBlockNumberNoCheck(&stack->parentkey),
					 ItemPointerGetOffsetNumberNoCheck(&stack->parentkey));
			else
				elog(DEBUG3, "blk %u: root internal posting tree page with %u items",
					 stack->blkno, maxoff);

			/*
			 * A GIN posting tree internal page stores PostingItems in the
			 * 'lower' part of the page. The 'upper' part is unused. The
			 * number of elements is stored in the opaque area (maxoff). Make
			 * sure the size of the 'lower' part agrees with 'maxoff'
			 *
			 * GIN 发布树内部页面将 PostingItems 存储在页面的“下部”部分。 “上部”部分未使用。元素的数量存储在不透明区域（maxoff）中。确保“lower”部分的尺寸与“maxoff”一致
			 *
			 * We didn't set pd_lower until PostgreSQL version 9.4, so if this
			 * check fails, it could also be because the index was
			 * binary-upgraded from an earlier version. That was a long time
			 * ago, though, so let's warn if it doesn't match.
			 *
			 * 我们直到 PostgreSQL 9.4 版本才设置 pd_lower，因此如果此检查失败，也可能是因为索引是从早期版本进行二进制升级的。不过那是很久以前的事了，所以如果不匹配的话我们会发出警告。
			 */
			pd_lower = ((PageHeader) page)->pd_lower;
			lowersize = pd_lower - MAXALIGN(SizeOfPageHeaderData);
			if ((lowersize - MAXALIGN(sizeof(ItemPointerData))) / sizeof(PostingItem) != maxoff)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" has unexpected pd_lower %u in posting tree block %u with maxoff %u)",
								RelationGetRelationName(rel), pd_lower, stack->blkno, maxoff)));

			/*
			 * Before the PostingItems, there's one ItemPointerData in the
			 * 'lower' part that stores the page's high key.
			 *
			 * 在 PostingItems 之前，“下部”部分有一个 ItemPointerData，用于存储页面的高键。
			 */
			bound = *GinDataPageGetRightBound(page);

			/*
			 * Gin page right bound has a sane value only when not a highkey
			 * on the rightmost page (at a given level). For the rightmost
			 * page does not store the highkey explicitly, and the value is
			 * infinity.
			 *
			 * 仅当最右侧页面（在给定级别）上不是高键时，Gin 页面右边界才具有正常值。对于最右边的页面没有显式存储 highkey，并且该值是无穷大。
			 */
			if (ItemPointerIsValid(&stack->parentkey) &&
				rightlink != InvalidBlockNumber &&
				!ItemPointerEquals(&stack->parentkey, &bound))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": posting tree page's high key (%u, %u) doesn't match the downlink on block %u (parent blk %u, key (%u, %u))",
								RelationGetRelationName(rel),
								ItemPointerGetBlockNumberNoCheck(&bound),
								ItemPointerGetOffsetNumberNoCheck(&bound),
								stack->blkno, stack->parentblk,
								ItemPointerGetBlockNumberNoCheck(&stack->parentkey),
								ItemPointerGetOffsetNumberNoCheck(&stack->parentkey))));

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				GinPostingTreeScanItem *ptr;
				PostingItem *posting_item = GinDataPageGetPostingItem(page, i);

				/* ItemPointerGetOffsetNumber expects a valid pointer
				 *
				 * ItemPointerGetOffsetNumber 需要一个有效的指针
				 */
				if (!(i == maxoff &&
					  rightlink == InvalidBlockNumber))
					elog(DEBUG3, "key (%u, %u) -> %u",
						 ItemPointerGetBlockNumber(&posting_item->key),
						 ItemPointerGetOffsetNumber(&posting_item->key),
						 BlockIdGetBlockNumber(&posting_item->child_blkno));
				else
					elog(DEBUG3, "key (%u, %u) -> %u",
						 0, 0, BlockIdGetBlockNumber(&posting_item->child_blkno));

				if (i == maxoff && rightlink == InvalidBlockNumber)
				{
					/*
					 * The rightmost item in the tree level has (0, 0) as the
					 * key
					 *
					 * 树级别中最右边的项以 (0, 0) 作为键
					 */
					if (ItemPointerGetBlockNumberNoCheck(&posting_item->key) != 0 ||
						ItemPointerGetOffsetNumberNoCheck(&posting_item->key) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("index \"%s\": rightmost posting tree page (blk %u) has unexpected last key (%u, %u)",
										RelationGetRelationName(rel),
										stack->blkno,
										ItemPointerGetBlockNumberNoCheck(&posting_item->key),
										ItemPointerGetOffsetNumberNoCheck(&posting_item->key))));
				}
				else if (i != FirstOffsetNumber)
				{
					PostingItem *previous_posting_item = GinDataPageGetPostingItem(page, i - 1);

					if (ItemPointerCompare(&posting_item->key, &previous_posting_item->key) < 0)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("index \"%s\" has wrong tuple order in posting tree, block %u, offset %u",
										RelationGetRelationName(rel), stack->blkno, i)));
				}

				/*
				 * Check if this tuple is consistent with the downlink in the
				 * parent.
				 *
				 * 检查该元组与父元组中的下行链路是否一致。
				 */
				if (i == maxoff && ItemPointerIsValid(&stack->parentkey) &&
					ItemPointerCompare(&stack->parentkey, &posting_item->key) < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("index \"%s\": posting item exceeds parent's high key in postingTree internal page on block %u offset %u",
									RelationGetRelationName(rel),
									stack->blkno, i)));

				/* This is an internal page, recurse into the child.
				 *
				 * 这是一个内部页面，递归到子页面。
				 */
				ptr = (GinPostingTreeScanItem *) palloc(sizeof(GinPostingTreeScanItem));
				ptr->depth = stack->depth + 1;

				/*
				 * The rightmost parent key is always invalid item pointer.
				 * Its value is 'Infinity' and not explicitly stored.
				 *
				 * 最右边的父键始终是无效的项目指针。它的值为“Infinity”并且未显式存储。
				 */
				ptr->parentkey = posting_item->key;
				ptr->parentblk = stack->blkno;
				ptr->blkno = BlockIdGetBlockNumber(&posting_item->child_blkno);
				ptr->next = stack->next;
				stack->next = ptr;
			}
		}
		LockBuffer(buffer, GIN_UNLOCK);
		ReleaseBuffer(buffer);

		/* Step to next item in the queue
		 *
		 * 进入队列中的下一个项目
		 */
		stack_next = stack->next;
		pfree(stack);
		stack = stack_next;
	}

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(mctx);
}

/*
 * Main entry point for GIN checks.
 *
 * GIN 检查的主要入口点。
 *
 * Allocates memory context and scans through the whole GIN graph.
 *
 * 分配内存上下文并扫描整个 GIN 图。
 */
static void
gin_check_parent_keys_consistency(Relation rel,
								  Relation heaprel,
								  void *callback_state,
								  bool readonly)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	GinScanItem *stack;
	MemoryContext mctx;
	MemoryContext oldcontext;
	GinState	state;
	int			leafdepth;

	mctx = AllocSetContextCreate(CurrentMemoryContext,
								 "amcheck consistency check context",
								 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(mctx);
	initGinState(&state, rel);

	/*
	 * We don't know the height of the tree yet, but as soon as we encounter a
	 * leaf page, we will set 'leafdepth' to its depth.
	 *
	 * 我们还不知道树的高度，但是一旦遇到叶子页面，我们就会将 'leafdepth' 设置为它的深度。
	 */
	leafdepth = -1;

	/* Start the scan at the root page
	 *
	 * 从根页面开始扫描
	 */
	stack = (GinScanItem *) palloc0(sizeof(GinScanItem));
	stack->depth = 0;
	stack->parenttup = NULL;
	stack->parentblk = InvalidBlockNumber;
	stack->blkno = GIN_ROOT_BLKNO;

	while (stack)
	{
		GinScanItem *stack_next;
		Buffer		buffer;
		Page		page;
		OffsetNumber i,
					maxoff,
					prev_attnum;
		IndexTuple	prev_tuple;
		BlockNumber rightlink;

		CHECK_FOR_INTERRUPTS();

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, stack->blkno,
									RBM_NORMAL, strategy);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);
		maxoff = PageGetMaxOffsetNumber(page);
		rightlink = GinPageGetOpaque(page)->rightlink;

		/* Do basic sanity checks on the page headers
		 *
		 * 对页眉进行基本的健全性检查
		 */
		check_index_page(rel, buffer, stack->blkno);

		elog(DEBUG3, "processing entry tree page at blk %u, maxoff: %u", stack->blkno, maxoff);

		/*
		 * It's possible that the page was split since we looked at the
		 * parent, so that we didn't missed the downlink of the right sibling
		 * when we scanned the parent.  If so, add the right sibling to the
		 * stack now.
		 *
		 * 可能是因为我们查看了父级，所以页面被分割了，这样我们在扫描父级时就不会错过右兄弟的下行链路。  如果是这样，请立即将右侧同级添加到堆栈中。
		 */
		if (stack->parenttup != NULL)
		{
			GinNullCategory parent_key_category;
			Datum		parent_key = gintuple_get_key(&state,
													  stack->parenttup,
													  &parent_key_category);
			OffsetNumber parent_key_attnum = gintuple_get_attrnum(&state, stack->parenttup);
			ItemId		iid = PageGetItemIdCareful(rel, stack->blkno,
												   page, maxoff);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
			OffsetNumber page_max_key_attnum = gintuple_get_attrnum(&state, idxtuple);
			GinNullCategory page_max_key_category;
			Datum		page_max_key = gintuple_get_key(&state, idxtuple, &page_max_key_category);

			if (rightlink != InvalidBlockNumber &&
				ginCompareAttEntries(&state, page_max_key_attnum, page_max_key,
									 page_max_key_category, parent_key_attnum,
									 parent_key, parent_key_category) < 0)
			{
				/* split page detected, install right link to the stack
				 *
				 * 检测到拆分页面，安装堆栈的正确链接
				 */
				GinScanItem *ptr;

				elog(DEBUG3, "split detected for blk: %u, parent blk: %u", stack->blkno, stack->parentblk);

				ptr = (GinScanItem *) palloc(sizeof(GinScanItem));
				ptr->depth = stack->depth;
				ptr->parenttup = CopyIndexTuple(stack->parenttup);
				ptr->parentblk = stack->parentblk;
				ptr->blkno = rightlink;
				ptr->next = stack->next;
				stack->next = ptr;
			}
		}

		/* Check that the tree has the same height in all branches
		 *
		 * 检查树的所有分支的高度是否相同
		 */
		if (GinPageIsLeaf(page))
		{
			if (leafdepth == -1)
				leafdepth = stack->depth;
			else if (stack->depth != leafdepth)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\": internal pages traversal encountered leaf page unexpectedly on block %u",
								RelationGetRelationName(rel), stack->blkno)));
		}

		/*
		 * Check that tuples in each page are properly ordered and consistent
		 * with parent high key
		 *
		 * 检查每个页面中的元组是否正确排序并与父高键一致
		 */
		prev_tuple = NULL;
		prev_attnum = InvalidAttrNumber;
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			ItemId		iid = PageGetItemIdCareful(rel, stack->blkno, page, i);
			IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
			OffsetNumber current_attnum = gintuple_get_attrnum(&state, idxtuple);
			GinNullCategory current_key_category;
			Datum		current_key;

			if (MAXALIGN(ItemIdGetLength(iid)) != MAXALIGN(IndexTupleSize(idxtuple)))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" has inconsistent tuple sizes, block %u, offset %u",
								RelationGetRelationName(rel), stack->blkno, i)));

			current_key = gintuple_get_key(&state, idxtuple, &current_key_category);

			/*
			 * Compare the entry to the preceding one.
			 *
			 * 将条目与前一个条目进行比较。
			 *
			 * Don't check for high key on the rightmost inner page, as this
			 * key is not really stored explicitly.
			 *
			 * 不要检查最右侧内页上的高键，因为该键并未真正显式存储。
			 *
			 * The entries may be for different attributes, so make sure to
			 * use ginCompareAttEntries for comparison.
			 *
			 * 这些条目可能用于不同的属性，因此请确保使用 ginCompareAttEntries 进行比较。
			 */
			if ((i != FirstOffsetNumber) &&
				!(i == maxoff && rightlink == InvalidBlockNumber && !GinPageIsLeaf(page)))
			{
				Datum		prev_key;
				GinNullCategory prev_key_category;

				prev_key = gintuple_get_key(&state, prev_tuple, &prev_key_category);
				if (ginCompareAttEntries(&state, prev_attnum, prev_key,
										 prev_key_category, current_attnum,
										 current_key, current_key_category) >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("index \"%s\" has wrong tuple order on entry tree page, block %u, offset %u, rightlink %u",
									RelationGetRelationName(rel), stack->blkno, i, rightlink)));
			}

			/*
			 * Check if this tuple is consistent with the downlink in the
			 * parent.
			 *
			 * 检查该元组与父元组中的下行链路是否一致。
			 */
			if (stack->parenttup &&
				i == maxoff)
			{
				GinNullCategory parent_key_category;
				OffsetNumber parent_key_attnum = gintuple_get_attrnum(&state, stack->parenttup);
				Datum		parent_key = gintuple_get_key(&state,
														  stack->parenttup,
														  &parent_key_category);

				if (ginCompareAttEntries(&state, current_attnum, current_key,
										 current_key_category, parent_key_attnum,
										 parent_key, parent_key_category) > 0)
				{
					/*
					 * There was a discrepancy between parent and child
					 * tuples. We need to verify it is not a result of
					 * concurrent call of gistplacetopage(). So, lock parent
					 * and try to find downlink for current page. It may be
					 * missing due to concurrent page split, this is OK.
					 *
					 * 父元组和子元组之间存在差异。我们需要验证它不是并发调用 gistplacetopage() 的结果。因此，锁定父级并尝试查找当前页面的下行链接。可能是由于并发页面分割而丢失，这是可以的。
					 */
					pfree(stack->parenttup);
					stack->parenttup = gin_refind_parent(rel, stack->parentblk,
														 stack->blkno, strategy);

					/* We found it - make a final check before failing
					 *
					 * 我们找到了 - 在失败之前进行最后检查
					 */
					if (!stack->parenttup)
						elog(NOTICE, "Unable to find parent tuple for block %u on block %u due to concurrent split",
							 stack->blkno, stack->parentblk);
					else
					{
						parent_key_attnum = gintuple_get_attrnum(&state, stack->parenttup);
						parent_key = gintuple_get_key(&state,
													  stack->parenttup,
													  &parent_key_category);

						/*
						 * Check if it is properly adjusted. If succeed,
						 * proceed to the next key.
						 *
						 * 检查是否调整正确。如果成功，则继续执行下一个键。
						 */
						if (ginCompareAttEntries(&state, current_attnum, current_key,
												 current_key_category, parent_key_attnum,
												 parent_key, parent_key_category) > 0)
							ereport(ERROR,
									(errcode(ERRCODE_INDEX_CORRUPTED),
									 errmsg("index \"%s\" has inconsistent records on page %u offset %u",
											RelationGetRelationName(rel), stack->blkno, i)));
					}
				}
			}

			/* If this is an internal page, recurse into the child
			 *
			 * 如果这是一个内部页面，则递归到子页面
			 */
			if (!GinPageIsLeaf(page))
			{
				GinScanItem *ptr;

				ptr = (GinScanItem *) palloc(sizeof(GinScanItem));
				ptr->depth = stack->depth + 1;
				/* last tuple in layer has no high key
				 *
				 * 层中的最后一个元组没有高调
				 */
				if (i == maxoff && rightlink == InvalidBlockNumber)
					ptr->parenttup = NULL;
				else
					ptr->parenttup = CopyIndexTuple(idxtuple);
				ptr->parentblk = stack->blkno;
				ptr->blkno = GinGetDownlink(idxtuple);
				ptr->next = stack->next;
				stack->next = ptr;
			}
			/* If this item is a pointer to a posting tree, recurse into it
			 *
			 * 如果此项是指向发布树的指针，则递归到它
			 */
			else if (GinIsPostingTree(idxtuple))
			{
				BlockNumber rootPostingTree = GinGetPostingTree(idxtuple);

				gin_check_posting_tree_parent_keys_consistency(rel, rootPostingTree);
			}
			else
			{
				ItemPointer ipd;
				int			nipd;

				ipd = ginReadTupleWithoutState(idxtuple, &nipd);

				for (int j = 0; j < nipd; j++)
				{
					if (!OffsetNumberIsValid(ItemPointerGetOffsetNumber(&ipd[j])))
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("index \"%s\": posting list contains invalid heap pointer on block %u",
										RelationGetRelationName(rel), stack->blkno)));
				}
				pfree(ipd);
			}

			prev_tuple = CopyIndexTuple(idxtuple);
			prev_attnum = current_attnum;
		}

		LockBuffer(buffer, GIN_UNLOCK);
		ReleaseBuffer(buffer);

		/* Step to next item in the queue
		 *
		 * 进入队列中的下一个项目
		 */
		stack_next = stack->next;
		if (stack->parenttup)
			pfree(stack->parenttup);
		pfree(stack);
		stack = stack_next;
	}

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(mctx);
}

/*
 * Verify that a freshly-read page looks sane.
 *
 * 验证新阅读的页面看起来是否正常。
 */
static void
check_index_page(Relation rel, Buffer buffer, BlockNumber blockNo)
{
	Page		page = BufferGetPage(buffer);

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.  We have to defend against the all-zero
	 * case, however.
	 *
	 * ReadBuffer 验证每个新读取的页面是否通过 PageHeaderIsValid，这意味着它要么包含合理的页面标题，要么全为零。  然而，我们必须防范全零的情况。
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buffer)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 *
	 * 另外检查特殊区域是否看起来正常。
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buffer)),
				 errhint("Please REINDEX it.")));

	if (GinPageIsDeleted(page))
	{
		if (!GinPageIsLeaf(page))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has deleted internal page %u",
							RelationGetRelationName(rel), blockNo)));
		if (PageGetMaxOffsetNumber(page) > InvalidOffsetNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has deleted page %u with tuples",
							RelationGetRelationName(rel), blockNo)));
	}
	else if (PageGetMaxOffsetNumber(page) > MaxIndexTuplesPerPage)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has page %u with exceeding count of tuples",
						RelationGetRelationName(rel), blockNo)));
}

/*
 * Try to re-find downlink pointing to 'blkno', in 'parentblkno'.
 *
 * 尝试在“parentblkno”中重新查找指向“blkno”的下行链路。
 *
 * If found, returns a palloc'd copy of the downlink tuple. Otherwise,
 * returns NULL.
 *
 * 如果找到，则返回下行链路元组的 palloc 副本。否则，返回 NULL。
 */
static IndexTuple
gin_refind_parent(Relation rel, BlockNumber parentblkno,
				  BlockNumber childblkno, BufferAccessStrategy strategy)
{
	Buffer		parentbuf;
	Page		parentpage;
	OffsetNumber o,
				parent_maxoff;
	IndexTuple	result = NULL;

	parentbuf = ReadBufferExtended(rel, MAIN_FORKNUM, parentblkno, RBM_NORMAL,
								   strategy);

	LockBuffer(parentbuf, GIN_SHARE);
	parentpage = BufferGetPage(parentbuf);

	if (GinPageIsLeaf(parentpage))
	{
		UnlockReleaseBuffer(parentbuf);
		return result;
	}

	parent_maxoff = PageGetMaxOffsetNumber(parentpage);
	for (o = FirstOffsetNumber; o <= parent_maxoff; o = OffsetNumberNext(o))
	{
		ItemId		p_iid = PageGetItemIdCareful(rel, parentblkno, parentpage, o);
		IndexTuple	itup = (IndexTuple) PageGetItem(parentpage, p_iid);

		if (GinGetDownlink(itup) == childblkno)
		{
			/* Found it! Make copy and return it
			 *
			 * 找到了！复印并退回
			 */
			result = CopyIndexTuple(itup);
			break;
		}
	}

	UnlockReleaseBuffer(parentbuf);

	return result;
}

static ItemId
PageGetItemIdCareful(Relation rel, BlockNumber block, Page page,
					 OffsetNumber offset)
{
	ItemId		itemid = PageGetItemId(page, offset);

	if (ItemIdGetOffset(itemid) + ItemIdGetLength(itemid) >
		BLCKSZ - MAXALIGN(sizeof(GinPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("line pointer points past end of tuple space in index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	/*
	 * Verify that line pointer isn't LP_REDIRECT or LP_UNUSED or LP_DEAD,
	 * since GIN never uses all three.  Verify that line pointer has storage,
	 * too.
	 *
	 * 验证行指针不是 LP_REDIRECT 或 LP_UNUSED 或 LP_DEAD，因为 GIN 从未使用这三个指针。  验证行指针也有存储空间。
	 */
	if (ItemIdIsRedirected(itemid) || !ItemIdIsUsed(itemid) ||
		ItemIdIsDead(itemid) || ItemIdGetLength(itemid) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid line pointer storage in index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	return itemid;
}
