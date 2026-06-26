/*-------------------------------------------------------------------------
 *
 * blscan.c
 *		Bloom index scan functions.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "bloom.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"

/*
 * Begin scan of bloom index.
 *
 * 开始扫描布隆索引。
 */
IndexScanDesc
blbeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BloomScanOpaque so;

	scan = RelationGetIndexScan(r, nkeys, norderbys);

	so = (BloomScanOpaque) palloc(sizeof(BloomScanOpaqueData));
	initBloomState(&so->state, scan->indexRelation);
	so->sign = NULL;

	scan->opaque = so;

	return scan;
}

/*
 * Rescan a bloom index.
 *
 * 重新扫描绽放索引。
 */
void
blrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	BloomScanOpaque so = (BloomScanOpaque) scan->opaque;

	if (so->sign)
		pfree(so->sign);
	so->sign = NULL;

	if (scankey && scan->numberOfKeys > 0)
		memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));
}

/*
 * End scan of bloom index.
 *
 * 结束布隆索引扫描。
 */
void
blendscan(IndexScanDesc scan)
{
	BloomScanOpaque so = (BloomScanOpaque) scan->opaque;

	if (so->sign)
		pfree(so->sign);
	so->sign = NULL;
}

/*
 * Insert all matching tuples into a bitmap.
 *
 * 将所有匹配的元组插入位图中。
 */
int64
blgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	int64		ntids = 0;
	BlockNumber blkno = BLOOM_HEAD_BLKNO,
				npages;
	int			i;
	BufferAccessStrategy bas;
	BloomScanOpaque so = (BloomScanOpaque) scan->opaque;

	if (so->sign == NULL)
	{
		/* New search: have to calculate search signature
		 *
		 * 新搜索：必须计算搜索签名
		 */
		ScanKey		skey = scan->keyData;

		so->sign = palloc0(sizeof(BloomSignatureWord) * so->state.opts.bloomLength);

		for (i = 0; i < scan->numberOfKeys; i++)
		{
			/*
			 * Assume bloom-indexable operators to be strict, so nothing could
			 * be found for NULL key.
			 *
			 * 假设布隆可索引运算符是严格的，因此找不到 NULL 键的任何内容。
			 */
			if (skey->sk_flags & SK_ISNULL)
			{
				pfree(so->sign);
				so->sign = NULL;
				return 0;
			}

			/* Add next value to the signature
			 *
			 * 将下一个值添加到签名中
			 */
			signValue(&so->state, so->sign, skey->sk_argument,
					  skey->sk_attno - 1);

			skey++;
		}
	}

	/*
	 * We're going to read the whole index. This is why we use appropriate
	 * buffer access strategy.
	 *
	 * 我们将阅读整个索引。这就是我们使用适当的缓冲区访问策略的原因。
	 */
	bas = GetAccessStrategy(BAS_BULKREAD);
	npages = RelationGetNumberOfBlocks(scan->indexRelation);
	pgstat_count_index_scan(scan->indexRelation);
	if (scan->instrument)
		scan->instrument->nsearches++;

	for (blkno = BLOOM_HEAD_BLKNO; blkno < npages; blkno++)
	{
		Buffer		buffer;
		Page		page;

		buffer = ReadBufferExtended(scan->indexRelation, MAIN_FORKNUM,
									blkno, RBM_NORMAL, bas);

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		if (!PageIsNew(page) && !BloomPageIsDeleted(page))
		{
			OffsetNumber offset,
						maxOffset = BloomPageGetMaxOffset(page);

			for (offset = 1; offset <= maxOffset; offset++)
			{
				BloomTuple *itup = BloomPageGetTuple(&so->state, page, offset);
				bool		res = true;

				/* Check index signature with scan signature
				 *
				 * 使用扫描签名检查索引签名
				 */
				for (i = 0; i < so->state.opts.bloomLength; i++)
				{
					if ((itup->sign[i] & so->sign[i]) != so->sign[i])
					{
						res = false;
						break;
					}
				}

				/* Add matching tuples to bitmap
				 *
				 * 将匹配的元组添加到位图
				 */
				if (res)
				{
					tbm_add_tuples(tbm, &itup->heapPtr, 1, true);
					ntids++;
				}
			}
		}

		UnlockReleaseBuffer(buffer);
		CHECK_FOR_INTERRUPTS();
	}
	FreeAccessStrategy(bas);

	return ntids;
}
