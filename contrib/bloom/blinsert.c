/*-------------------------------------------------------------------------
 *
 * blinsert.c
 *		Bloom index build and insert functions.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blinsert.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/tableam.h"
#include "bloom.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "bloom",
					.version = PG_VERSION
);

/*
 * State of bloom index build.  We accumulate one page data here before
 * flushing it to buffer manager.
 *
 * 绽放状态指数构建。  在将一页数据刷新到缓冲区管理器之前，我们在这里累积一页数据。
 */
typedef struct
{
	BloomState	blstate;		/* bloom index state */
	int64		indtuples;		/* total number of tuples indexed */
	MemoryContext tmpCtx;		/* temporary memory context reset after each
								 * tuple */
	PGAlignedBlock data;		/* cached page */
	int			count;			/* number of tuples in cached page */
} BloomBuildState;

/*
 * Flush page cached in BloomBuildState.
 *
 * 刷新 BloomBuildState 中缓存的页面。
 */
static void
flushCachedPage(Relation index, BloomBuildState *buildstate)
{
	Page		page;
	Buffer		buffer = BloomNewBuffer(index);
	GenericXLogState *state;

	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
	memcpy(page, buildstate->data.data, BLCKSZ);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buffer);
}

/*
 * (Re)initialize cached page in BloomBuildState.
 *
 * （重新）初始化 BloomBuildState 中的缓存页面。
 */
static void
initCachedPage(BloomBuildState *buildstate)
{
	BloomInitPage(buildstate->data.data, 0);
	buildstate->count = 0;
}

/*
 * Per-tuple callback for table_index_build_scan.
 *
 * table_index_build_scan 的每元组回调。
 */
static void
bloomBuildCallback(Relation index, ItemPointer tid, Datum *values,
				   bool *isnull, bool tupleIsAlive, void *state)
{
	BloomBuildState *buildstate = (BloomBuildState *) state;
	MemoryContext oldCtx;
	BloomTuple *itup;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	itup = BloomFormTuple(&buildstate->blstate, tid, values, isnull);

	/* Try to add next item to cached page
	 *
	 * 尝试将下一个项目添加到缓存页面
	 */
	if (BloomPageAddItem(&buildstate->blstate, buildstate->data.data, itup))
	{
		/* Next item was added successfully
		 *
		 * 下一项已成功添加
		 */
		buildstate->count++;
	}
	else
	{
		/* Cached page is full, flush it out and make a new one
		 *
		 * 缓存页面已满，将其刷新并创建新页面
		 */
		flushCachedPage(index, buildstate);

		CHECK_FOR_INTERRUPTS();

		initCachedPage(buildstate);

		if (!BloomPageAddItem(&buildstate->blstate, buildstate->data.data, itup))
		{
			/* We shouldn't be here since we're inserting to the empty page
			 *
			 * 我们不应该在这里，因为我们要插入到空页面
			 */
			elog(ERROR, "could not add new bloom tuple to empty page");
		}

		/* Next item was added successfully
		 *
		 * 下一项已成功添加
		 */
		buildstate->count++;
	}

	/* Update total tuple count
	 *
	 * 更新元组总数
	 */
	buildstate->indtuples += 1;

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
}

/*
 * Build a new bloom index.
 *
 * 建立一个新的绽放指数。
 */
IndexBuildResult *
blbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	double		reltuples;
	BloomBuildState buildstate;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* Initialize the meta page
	 *
	 * 初始化元页​​面
	 */
	BloomInitMetapage(index, MAIN_FORKNUM);

	/* Initialize the bloom build state
	 *
	 * 初始化Bloom构建状态
	 */
	memset(&buildstate, 0, sizeof(buildstate));
	initBloomState(&buildstate.blstate, index);
	buildstate.tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
											  "Bloom build temporary context",
											  ALLOCSET_DEFAULT_SIZES);
	initCachedPage(&buildstate);

	/* Do the heap scan
	 *
	 * 进行堆扫描
	 */
	reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
									   bloomBuildCallback, &buildstate,
									   NULL);

	/* Flush last page if needed (it will be, unless heap was empty)
	 *
	 * 如果需要的话刷新最后一页（除非堆为空）
	 */
	if (buildstate.count > 0)
		flushCachedPage(index, &buildstate);

	MemoryContextDelete(buildstate.tmpCtx);

	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = buildstate.indtuples;

	return result;
}

/*
 * Build an empty bloom index in the initialization fork.
 *
 * 在初始化分支中构建一个空的Bloom索引。
 */
void
blbuildempty(Relation index)
{
	/* Initialize the meta page
	 *
	 * 初始化元页​​面
	 */
	BloomInitMetapage(index, INIT_FORKNUM);
}

/*
 * Insert new tuple to the bloom index.
 *
 * 将新元组插入到布隆索引中。
 */
bool
blinsert(Relation index, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 bool indexUnchanged,
		 IndexInfo *indexInfo)
{
	BloomState	blstate;
	BloomTuple *itup;
	MemoryContext oldCtx;
	MemoryContext insertCtx;
	BloomMetaPageData *metaData;
	Buffer		buffer,
				metaBuffer;
	Page		page,
				metaPage;
	BlockNumber blkno = InvalidBlockNumber;
	OffsetNumber nStart;
	GenericXLogState *state;

	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
									  "Bloom insert temporary context",
									  ALLOCSET_DEFAULT_SIZES);

	oldCtx = MemoryContextSwitchTo(insertCtx);

	initBloomState(&blstate, index);
	itup = BloomFormTuple(&blstate, ht_ctid, values, isnull);

	/*
	 * At first, try to insert new tuple to the first page in notFullPage
	 * array.  If successful, we don't need to modify the meta page.
	 *
	 * 首先，尝试将新元组插入到 notFullPage 数组的第一页。  如果成功，我们不需要修改元页面。
	 */
	metaBuffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
	LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
	metaData = BloomPageGetMeta(BufferGetPage(metaBuffer));

	if (metaData->nEnd > metaData->nStart)
	{
		blkno = metaData->notFullPage[metaData->nStart];
		Assert(blkno != InvalidBlockNumber);

		/* Don't hold metabuffer lock while doing insert
		 *
		 * 执行插入时不要持有元缓冲区锁
		 */
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page = GenericXLogRegisterBuffer(state, buffer, 0);

		/*
		 * We might have found a page that was recently deleted by VACUUM.  If
		 * so, we can reuse it, but we must reinitialize it.
		 *
		 * 我们可能发现了最近被 VACUUM 删除的页面。  如果是这样，我们可以重用它，但我们必须重新初始化它。
		 */
		if (PageIsNew(page) || BloomPageIsDeleted(page))
			BloomInitPage(page, 0);

		if (BloomPageAddItem(&blstate, page, itup))
		{
			/* Success!  Apply the change, clean up, and exit
			 *
			 * 成功！  应用更改、清理并退出
			 */
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
			ReleaseBuffer(metaBuffer);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextDelete(insertCtx);
			return false;
		}

		/* Didn't fit, must try other pages
		 *
		 * 不适合，必须尝试其他页面
		 */
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buffer);
	}
	else
	{
		/* No entries in notFullPage
		 *
		 * notFullPage 中没有条目
		 */
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);
	}

	/*
	 * Try other pages in notFullPage array.  We will have to change nStart in
	 * metapage.  Thus, grab exclusive lock on metapage.
	 *
	 * 尝试 notFullPage 数组中的其他页面。  我们必须更改元页面中的 nStart。  因此，获取元页上的独占锁。
	 */
	LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);

	/* nStart might have changed while we didn't have lock
	 *
	 * 当我们没有锁时 nStart 可能已经改变
	 */
	nStart = metaData->nStart;

	/* Skip first page if we already tried it above
	 *
	 * 如果我们已经在上面尝试过，请跳过第一页
	 */
	if (nStart < metaData->nEnd &&
		blkno == metaData->notFullPage[nStart])
		nStart++;

	/*
	 * This loop iterates for each page we try from the notFullPage array, and
	 * will also initialize a GenericXLogState for the fallback case of having
	 * to allocate a new page.
	 *
	 * 此循环对我们从 notFullPage 数组中尝试的每个页面进行迭代，并且还将初始化 GenericXLogState 以应对必须分配新页面的后备情况。
	 */
	for (;;)
	{
		state = GenericXLogStart(index);

		/* get modifiable copy of metapage
		 *
		 * 获取元页面的可修改副本
		 */
		metaPage = GenericXLogRegisterBuffer(state, metaBuffer, 0);
		metaData = BloomPageGetMeta(metaPage);

		if (nStart >= metaData->nEnd)
			break;				/* no more entries in notFullPage array */

		blkno = metaData->notFullPage[nStart];
		Assert(blkno != InvalidBlockNumber);

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buffer, 0);

		/* Basically same logic as above
		 *
		 * 与上面的逻辑基本相同
		 */
		if (PageIsNew(page) || BloomPageIsDeleted(page))
			BloomInitPage(page, 0);

		if (BloomPageAddItem(&blstate, page, itup))
		{
			/* Success!  Apply the changes, clean up, and exit
			 *
			 * 成功！  应用更改、清理并退出
			 */
			metaData->nStart = nStart;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buffer);
			UnlockReleaseBuffer(metaBuffer);
			MemoryContextSwitchTo(oldCtx);
			MemoryContextDelete(insertCtx);
			return false;
		}

		/* Didn't fit, must try other pages
		 *
		 * 不适合，必须尝试其他页面
		 */
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buffer);
		nStart++;
	}

	/*
	 * Didn't find place to insert in notFullPage array.  Allocate new page.
	 * (XXX is it good to do this while holding ex-lock on the metapage??)
	 *
	 * Didn't find place to insert in notFullPage array.  分配新页面。 (XXX is it good to do this while holding ex-lock on the metapage??)
	 */
	buffer = BloomNewBuffer(index);

	page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
	BloomInitPage(page, 0);

	if (!BloomPageAddItem(&blstate, page, itup))
	{
		/* We shouldn't be here since we're inserting to an empty page
		 *
		 * 我们不应该在这里，因为我们要插入到一个空页面
		 */
		elog(ERROR, "could not add new bloom tuple to empty page");
	}

	/* Reset notFullPage array to contain just this new page
	 *
	 * 重置 notFullPage 数组以仅包含这个新页面
	 */
	metaData->nStart = 0;
	metaData->nEnd = 1;
	metaData->notFullPage[0] = BufferGetBlockNumber(buffer);

	/* Apply the changes, clean up, and exit
	 *
	 * 应用更改、清理并退出
	 */
	GenericXLogFinish(state);

	UnlockReleaseBuffer(buffer);
	UnlockReleaseBuffer(metaBuffer);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	return false;
}
