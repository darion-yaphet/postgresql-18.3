/*-------------------------------------------------------------------------
 *
 * blvacuum.c
 *		Bloom VACUUM functions.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blvacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "bloom.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"


/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * 批量删除指向一组堆元组的所有索引条目。目标元组集是通过回调例程指定的，该例程告知是否正在删除任何给定的堆元组（由 ItemPointer 标识）。
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 *
 * 结果：包含 VACUUM 显示统计信息的 palloc'd 结构。
 */
IndexBulkDeleteResult *
blbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno,
				npages;
	FreeBlockNumberArray notFullPage;
	int			countPage = 0;
	BloomState	state;
	Buffer		buffer;
	Page		page;
	BloomMetaPageData *metaData;
	GenericXLogState *gxlogState;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	initBloomState(&state, index);

	/*
	 * Iterate over the pages. We don't care about concurrently added pages,
	 * they can't contain tuples to delete.
	 *
	 * 迭代页面。我们不关心同时添加的页面，它们不能包含要删除的元组。
	 */
	npages = RelationGetNumberOfBlocks(index);
	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		BloomTuple *itup,
				   *itupPtr,
				   *itupEnd;

		vacuum_delay_point(false);

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		gxlogState = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(gxlogState, buffer, 0);

		/* Ignore empty/deleted pages until blvacuumcleanup()
		 *
		 * 忽略空/删除的页面，直到 blvacuumcleanup()
		 */
		if (PageIsNew(page) || BloomPageIsDeleted(page))
		{
			UnlockReleaseBuffer(buffer);
			GenericXLogAbort(gxlogState);
			continue;
		}

		/*
		 * Iterate over the tuples.  itup points to current tuple being
		 * scanned, itupPtr points to where to save next non-deleted tuple.
		 *
		 * 迭代元组。  itup指向当前正在扫描的元组，itupPtr指向保存下一个未删除元组的位置。
		 */
		itup = itupPtr = BloomPageGetTuple(&state, page, FirstOffsetNumber);
		itupEnd = BloomPageGetTuple(&state, page,
									OffsetNumberNext(BloomPageGetMaxOffset(page)));
		while (itup < itupEnd)
		{
			/* Do we have to delete this tuple?
			 *
			 * 我们必须删除这个元组吗？
			 */
			if (callback(&itup->heapPtr, callback_state))
			{
				/* Yes; adjust count of tuples that will be left on page
				 *
				 * 是的;调整将留在页面上的元组的数量
				 */
				BloomPageGetOpaque(page)->maxoff--;
				stats->tuples_removed += 1;
			}
			else
			{
				/* No; copy it to itupPtr++, but skip copy if not needed
				 *
				 * 不;将其复制到 itupPtr++，但如果不需要则跳过复制
				 */
				if (itupPtr != itup)
					memmove((Pointer) itupPtr, (Pointer) itup,
							state.sizeOfBloomTuple);
				itupPtr = BloomPageGetNextTuple(&state, itupPtr);
			}

			itup = BloomPageGetNextTuple(&state, itup);
		}

		/* Assert that we counted correctly
		 *
		 * 断言我们计数正确
		 */
		Assert(itupPtr == BloomPageGetTuple(&state, page,
											OffsetNumberNext(BloomPageGetMaxOffset(page))));

		/*
		 * Add page to new notFullPage list if we will not mark page as
		 * deleted and there is free space on it
		 *
		 * 如果我们不将页面标记为已删除并且页面上有可用空间，则将页面添加到新的 notFullPage 列表中
		 */
		if (BloomPageGetMaxOffset(page) != 0 &&
			BloomPageGetFreeSpace(&state, page) >= state.sizeOfBloomTuple &&
			countPage < BloomMetaBlockN)
			notFullPage[countPage++] = blkno;

		/* Did we delete something?
		 *
		 * 我们删除了什么吗？
		 */
		if (itupPtr != itup)
		{
			/* Is it empty page now?
			 *
			 * 现在是空页吗？
			 */
			if (BloomPageGetMaxOffset(page) == 0)
				BloomPageSetDeleted(page);
			/* Adjust pd_lower
			 *
			 * 调整 pd_lower
			 */
			((PageHeader) page)->pd_lower = (Pointer) itupPtr - page;
			/* Finish WAL-logging
			 *
			 * 完成 WAL 日志记录
			 */
			GenericXLogFinish(gxlogState);
		}
		else
		{
			/* Didn't change anything: abort WAL-logging
			 *
			 * 没有改变任何东西：中止 WAL 日志记录
			 */
			GenericXLogAbort(gxlogState);
		}
		UnlockReleaseBuffer(buffer);
	}

	/*
	 * Update the metapage's notFullPage list with whatever we found.  Our
	 * info could already be out of date at this point, but blinsert() will
	 * cope if so.
	 *
	 * 使用我们发现的内容更新元页面的 notFullPage 列表。  此时我们的信息可能已经过时，但 blinsert() 会处理这种情况。
	 */
	buffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	gxlogState = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(gxlogState, buffer, 0);

	metaData = BloomPageGetMeta(page);
	memcpy(metaData->notFullPage, notFullPage, sizeof(BlockNumber) * countPage);
	metaData->nStart = 0;
	metaData->nEnd = countPage;

	GenericXLogFinish(gxlogState);
	UnlockReleaseBuffer(buffer);

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * 真空后清理。
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 *
 * 结果：包含 VACUUM 显示统计信息的 palloc'd 结构。
 */
IndexBulkDeleteResult *
blvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	BlockNumber npages,
				blkno;

	if (info->analyze_only)
		return stats;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/*
	 * Iterate over the pages: insert deleted pages into FSM and collect
	 * statistics.
	 *
	 * 迭代页面：将已删除的页面插入 FSM 并收集统计信息。
	 */
	npages = RelationGetNumberOfBlocks(index);
	stats->num_pages = npages;
	stats->pages_free = 0;
	stats->num_index_tuples = 0;
	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		vacuum_delay_point(false);

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (PageIsNew(page) || BloomPageIsDeleted(page))
		{
			RecordFreeIndexPage(index, blkno);
			stats->pages_free++;
		}
		else
		{
			stats->num_index_tuples += BloomPageGetMaxOffset(page);
		}

		UnlockReleaseBuffer(buffer);
	}

	IndexFreeSpaceMapVacuum(info->index);

	return stats;
}
