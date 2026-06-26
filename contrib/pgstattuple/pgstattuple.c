/*
 * contrib/pgstattuple/pgstattuple.c
 *
 * Copyright (c) 2001,2002	Tatsuo Ishii
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

#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC_EXT(
					.name = "pgstattuple",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(pgstattuple);
PG_FUNCTION_INFO_V1(pgstattuple_v1_5);
PG_FUNCTION_INFO_V1(pgstattuplebyid);
PG_FUNCTION_INFO_V1(pgstattuplebyid_v1_5);

/*
 * struct pgstattuple_type
 *
 * struct pgstattuple_type
 *
 * tuple_percent, dead_tuple_percent and free_percent are computable,
 * so not defined here.
 *
 * tuple_percent、dead_tuple_percent 和 free_percent 是可计算的，因此此处未定义。
 */
typedef struct pgstattuple_type
{
	uint64		table_len;
	uint64		tuple_count;
	uint64		tuple_len;
	uint64		dead_tuple_count;
	uint64		dead_tuple_len;
	uint64		free_space;		/* free/reusable space in bytes */
} pgstattuple_type;

typedef void (*pgstat_page) (pgstattuple_type *, Relation, BlockNumber,
							 BufferAccessStrategy);

static Datum build_pgstattuple_type(pgstattuple_type *stat,
									FunctionCallInfo fcinfo);
static Datum pgstat_relation(Relation rel, FunctionCallInfo fcinfo);
static Datum pgstat_heap(Relation rel, FunctionCallInfo fcinfo);
static void pgstat_btree_page(pgstattuple_type *stat,
							  Relation rel, BlockNumber blkno,
							  BufferAccessStrategy bstrategy);
static void pgstat_hash_page(pgstattuple_type *stat,
							 Relation rel, BlockNumber blkno,
							 BufferAccessStrategy bstrategy);
static void pgstat_gist_page(pgstattuple_type *stat,
							 Relation rel, BlockNumber blkno,
							 BufferAccessStrategy bstrategy);
static Datum pgstat_index(Relation rel, BlockNumber start,
						  pgstat_page pagefn, FunctionCallInfo fcinfo);
static void pgstat_index_page(pgstattuple_type *stat, Page page,
							  OffsetNumber minoff, OffsetNumber maxoff);

/*
 * build_pgstattuple_type -- build a pgstattuple_type tuple
 *
 * build_pgstattuple_type -- 构建一个 pgstattuple_type 元组
 */
static Datum
build_pgstattuple_type(pgstattuple_type *stat, FunctionCallInfo fcinfo)
{
#define NCOLUMNS	9
#define NCHARS		314

	HeapTuple	tuple;
	char	   *values[NCOLUMNS];
	char		values_buf[NCOLUMNS][NCHARS];
	int			i;
	double		tuple_percent;
	double		dead_tuple_percent;
	double		free_percent;	/* free/reusable space in % */
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;

	/* Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * Generate attribute metadata needed later to produce tuples from raw C
	 * strings
	 *
	 * 生成稍后需要的属性元数据，以从原始 C 字符串生成元组
	 */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	if (stat->table_len == 0)
	{
		tuple_percent = 0.0;
		dead_tuple_percent = 0.0;
		free_percent = 0.0;
	}
	else
	{
		tuple_percent = 100.0 * stat->tuple_len / stat->table_len;
		dead_tuple_percent = 100.0 * stat->dead_tuple_len / stat->table_len;
		free_percent = 100.0 * stat->free_space / stat->table_len;
	}

	/*
	 * Prepare a values array for constructing the tuple. This should be an
	 * array of C strings which will be processed later by the appropriate
	 * "in" functions.
	 *
	 * 准备一个用于构造元组的值数组。这应该是一个 C 字符串数组，稍后将由适当的“in”函数进行处理。
	 */
	for (i = 0; i < NCOLUMNS; i++)
		values[i] = values_buf[i];
	i = 0;
	snprintf(values[i++], NCHARS, INT64_FORMAT, stat->table_len);
	snprintf(values[i++], NCHARS, INT64_FORMAT, stat->tuple_count);
	snprintf(values[i++], NCHARS, INT64_FORMAT, stat->tuple_len);
	snprintf(values[i++], NCHARS, "%.2f", tuple_percent);
	snprintf(values[i++], NCHARS, INT64_FORMAT, stat->dead_tuple_count);
	snprintf(values[i++], NCHARS, INT64_FORMAT, stat->dead_tuple_len);
	snprintf(values[i++], NCHARS, "%.2f", dead_tuple_percent);
	snprintf(values[i++], NCHARS, INT64_FORMAT, stat->free_space);
	snprintf(values[i++], NCHARS, "%.2f", free_percent);

	/* build a tuple
	 *
	 * 构建一个元组
	 */
	tuple = BuildTupleFromCStrings(attinmeta, values);

	/* make the tuple into a datum
	 *
	 * 使元组成为数据
	 */
	return HeapTupleGetDatum(tuple);
}

/* ----------
 * pgstattuple:
 * returns live/dead tuples info
 *
 * pgstattuple：返回活/死元组信息
 *
 * C FUNCTION definition
 * pgstattuple(text) returns pgstattuple_type
 *
 * C FUNCTION 定义 pgstattuple(text) 返回 pgstattuple_type
 *
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.5 installations
 * these functions could be called by any user.
 *
 * 必须保留此处的 superuser() 检查，因为库可能会在扩展未升级的情况下升级，这意味着在 1.5 之前的安装中，任何用户都可以调用这些函数。
 * ----------
 */

Datum
pgstattuple(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	RangeVar   *relrv;
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	/* open relation
	 *
	 * 开放关系
	 */
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_DATUM(pgstat_relation(rel, fcinfo));
}

/*
 * As of pgstattuple version 1.5, we no longer need to check if the user
 * is a superuser because we REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 *
 * 从 pgstattuple 版本 1.5 开始，我们不再需要检查用户是否是超级用户，因为我们从 PUBLIC 中撤销了函数上的 EXECUTE。然后，用户可以根据自己的策略授予对其的访问权限。
 *
 * Otherwise identical to pgstattuple (above).
 *
 * 其他方面与 pgstattuple （上面）相同。
 */
Datum
pgstattuple_v1_5(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	RangeVar   *relrv;
	Relation	rel;

	/* open relation
	 *
	 * 开放关系
	 */
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_DATUM(pgstat_relation(rel, fcinfo));
}

/* Must keep superuser() check, see above.
 *
 * 必须保持 superuser() 检查，见上文。
 */
Datum
pgstattuplebyid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	/* open relation
	 *
	 * 开放关系
	 */
	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_DATUM(pgstat_relation(rel, fcinfo));
}

/* Remove superuser() check for 1.5 version, see above
 *
 * 删除 1.5 版本的 superuser() 检查，见上文
 */
Datum
pgstattuplebyid_v1_5(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	/* open relation
	 *
	 * 开放关系
	 */
	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_DATUM(pgstat_relation(rel, fcinfo));
}

/*
 * pgstat_relation
 */
static Datum
pgstat_relation(Relation rel, FunctionCallInfo fcinfo)
{
	const char *err;

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

	if (RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind) ||
		rel->rd_rel->relkind == RELKIND_SEQUENCE)
	{
		return pgstat_heap(rel, fcinfo);
	}
	else if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		/* see pgstatindex_impl
		 *
		 * 参见 pgstatindex_impl
		 */
		if (!rel->rd_index->indisvalid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" is not valid",
							RelationGetRelationName(rel))));

		switch (rel->rd_rel->relam)
		{
			case BTREE_AM_OID:
				return pgstat_index(rel, BTREE_METAPAGE + 1,
									pgstat_btree_page, fcinfo);
			case HASH_AM_OID:
				return pgstat_index(rel, HASH_METAPAGE + 1,
									pgstat_hash_page, fcinfo);
			case GIST_AM_OID:
				return pgstat_index(rel, GIST_ROOT_BLKNO + 1,
									pgstat_gist_page, fcinfo);
			case GIN_AM_OID:
				err = "gin index";
				break;
			case SPGIST_AM_OID:
				err = "spgist index";
				break;
			case BRIN_AM_OID:
				err = "brin index";
				break;
			default:
				err = "unknown index";
				break;
		}
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index \"%s\" (%s) is not supported",
						RelationGetRelationName(rel), err)));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot get tuple-level statistics for relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
	}

	return 0;					/* should not happen */
}

/*
 * pgstat_heap -- returns live/dead tuples info in a heap
 *
 * pgstat_heap -- 返回堆中的活/死元组信息
 */
static Datum
pgstat_heap(Relation rel, FunctionCallInfo fcinfo)
{
	TableScanDesc scan;
	HeapScanDesc hscan;
	HeapTuple	tuple;
	BlockNumber nblocks;
	BlockNumber block = 0;		/* next block to count free space in */
	BlockNumber tupblock;
	Buffer		buffer;
	pgstattuple_type stat = {0};
	SnapshotData SnapshotDirty;

	/*
	 * Sequences always use heap AM, but they don't show that in the catalogs.
	 *
	 * 序列始终使用堆 AM，但它们不会在目录中显示。
	 */
	if (rel->rd_rel->relkind != RELKIND_SEQUENCE &&
		rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	/* Disable syncscan because we assume we scan from block zero upwards
	 *
	 * 禁用同步扫描，因为我们假设我们从块零向上扫描
	 */
	scan = table_beginscan_strat(rel, SnapshotAny, 0, NULL, true, false);
	hscan = (HeapScanDesc) scan;

	InitDirtySnapshot(SnapshotDirty);

	nblocks = hscan->rs_nblocks;	/* # blocks to be scanned */

	/* scan the relation
	 *
	 * 扫描关系
	 */
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		/* must hold a buffer lock to call HeapTupleSatisfiesVisibility
		 *
		 * 必须持有缓冲区锁才能调用 HeapTupleSatisfiesVisibility
		 */
		LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

		if (HeapTupleSatisfiesVisibility(tuple, &SnapshotDirty, hscan->rs_cbuf))
		{
			stat.tuple_len += tuple->t_len;
			stat.tuple_count++;
		}
		else
		{
			stat.dead_tuple_len += tuple->t_len;
			stat.dead_tuple_count++;
		}

		LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

		/*
		 * To avoid physically reading the table twice, try to do the
		 * free-space scan in parallel with the heap scan.  However,
		 * heap_getnext may find no tuples on a given page, so we cannot
		 * simply examine the pages returned by the heap scan.
		 *
		 * 为了避免对表进行两次物理读取，请尝试与堆扫描并行地进行可用空间扫描。  但是，heap_getnext 可能在给定页面上找不到元组，因此我们不能简单地检查堆扫描返回的页面。
		 */
		tupblock = ItemPointerGetBlockNumber(&tuple->t_self);

		while (block <= tupblock)
		{
			CHECK_FOR_INTERRUPTS();

			buffer = ReadBufferExtended(rel, MAIN_FORKNUM, block,
										RBM_NORMAL, hscan->rs_strategy);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			stat.free_space += PageGetExactFreeSpace((Page) BufferGetPage(buffer));
			UnlockReleaseBuffer(buffer);
			block++;
		}
	}

	while (block < nblocks)
	{
		CHECK_FOR_INTERRUPTS();

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, block,
									RBM_NORMAL, hscan->rs_strategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		stat.free_space += PageGetExactFreeSpace((Page) BufferGetPage(buffer));
		UnlockReleaseBuffer(buffer);
		block++;
	}

	table_endscan(scan);
	relation_close(rel, AccessShareLock);

	stat.table_len = (uint64) nblocks * BLCKSZ;

	return build_pgstattuple_type(&stat, fcinfo);
}

/*
 * pgstat_btree_page -- check tuples in a btree page
 *
 * pgstat_btree_page -- 检查 btree 页面中的元组
 */
static void
pgstat_btree_page(pgstattuple_type *stat, Relation rel, BlockNumber blkno,
				  BufferAccessStrategy bstrategy)
{
	Buffer		buf;
	Page		page;

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);
	LockBuffer(buf, BT_READ);
	page = BufferGetPage(buf);

	/* Page is valid, see what to do with it
	 *
	 * 页面有效，看看如何处理它
	 */
	if (PageIsNew(page))
	{
		/* fully empty page
		 *
		 * 完全空白的页面
		 */
		stat->free_space += BLCKSZ;
	}
	else if (PageGetSpecialSize(page) == MAXALIGN(sizeof(BTPageOpaqueData)))
	{
		BTPageOpaque opaque;

		opaque = BTPageGetOpaque(page);
		if (P_IGNORE(opaque))
		{
			/* deleted or half-dead page
			 *
			 * 已删除或半死页
			 */
			stat->free_space += BLCKSZ;
		}
		else if (P_ISLEAF(opaque))
		{
			pgstat_index_page(stat, page, P_FIRSTDATAKEY(opaque),
							  PageGetMaxOffsetNumber(page));
		}
		else
		{
			/* internal page
			 *
			 * 内部页面
			 */
		}
	}

	_bt_relbuf(rel, buf);
}

/*
 * pgstat_hash_page -- check tuples in a hash page
 *
 * pgstat_hash_page -- 检查哈希页中的元组
 */
static void
pgstat_hash_page(pgstattuple_type *stat, Relation rel, BlockNumber blkno,
				 BufferAccessStrategy bstrategy)
{
	Buffer		buf;
	Page		page;

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);
	LockBuffer(buf, HASH_READ);
	page = BufferGetPage(buf);

	if (PageIsNew(page))
	{
		/* fully empty page
		 *
		 * 完全空白的页面
		 */
		stat->free_space += BLCKSZ;
	}
	else if (PageGetSpecialSize(page) == MAXALIGN(sizeof(HashPageOpaqueData)))
	{
		HashPageOpaque opaque;

		opaque = HashPageGetOpaque(page);
		switch (opaque->hasho_flag & LH_PAGE_TYPE)
		{
			case LH_UNUSED_PAGE:
				stat->free_space += BLCKSZ;
				break;
			case LH_BUCKET_PAGE:
			case LH_OVERFLOW_PAGE:
				pgstat_index_page(stat, page, FirstOffsetNumber,
								  PageGetMaxOffsetNumber(page));
				break;
			case LH_BITMAP_PAGE:
			case LH_META_PAGE:
			default:
				break;
		}
	}
	else
	{
		/* maybe corrupted
		 *
		 * 可能已损坏
		 */
	}

	_hash_relbuf(rel, buf);
}

/*
 * pgstat_gist_page -- check tuples in a gist page
 *
 * pgstat_gist_page -- 检查要点页面中的元组
 */
static void
pgstat_gist_page(pgstattuple_type *stat, Relation rel, BlockNumber blkno,
				 BufferAccessStrategy bstrategy)
{
	Buffer		buf;
	Page		page;

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);
	LockBuffer(buf, GIST_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page))
	{
		/* fully empty page
		 *
		 * 完全空白的页面
		 */
		stat->free_space += BLCKSZ;
	}
	else if (PageGetSpecialSize(page) == MAXALIGN(sizeof(GISTPageOpaqueData)))
	{
		if (GistPageIsLeaf(page))
		{
			pgstat_index_page(stat, page, FirstOffsetNumber,
							  PageGetMaxOffsetNumber(page));
		}
		else
		{
			/* root or node
			 *
			 * 根或节点
			 */
		}
	}

	UnlockReleaseBuffer(buf);
}

/*
 * pgstat_index -- returns live/dead tuples info in a generic index
 *
 * pgstat_index -- 返回通用索引中的活/死元组信息
 */
static Datum
pgstat_index(Relation rel, BlockNumber start, pgstat_page pagefn,
			 FunctionCallInfo fcinfo)
{
	BlockNumber nblocks;
	BlockNumber blkno;
	BufferAccessStrategy bstrategy;
	pgstattuple_type stat = {0};

	/* prepare access strategy for this index
	 *
	 * 准备该索引的访问策略
	 */
	bstrategy = GetAccessStrategy(BAS_BULKREAD);

	blkno = start;
	for (;;)
	{
		/* Get the current relation length
		 *
		 * 获取当前关系长度
		 */
		LockRelationForExtension(rel, ExclusiveLock);
		nblocks = RelationGetNumberOfBlocks(rel);
		UnlockRelationForExtension(rel, ExclusiveLock);

		/* Quit if we've scanned the whole relation
		 *
		 * 如果我们扫描了整个关系就退出
		 */
		if (blkno >= nblocks)
		{
			stat.table_len = (uint64) nblocks * BLCKSZ;

			break;
		}

		for (; blkno < nblocks; blkno++)
		{
			CHECK_FOR_INTERRUPTS();

			pagefn(&stat, rel, blkno, bstrategy);
		}
	}

	relation_close(rel, AccessShareLock);

	return build_pgstattuple_type(&stat, fcinfo);
}

/*
 * pgstat_index_page -- for generic index page
 *
 * pgstat_index_page -- 用于通用索引页
 */
static void
pgstat_index_page(pgstattuple_type *stat, Page page,
				  OffsetNumber minoff, OffsetNumber maxoff)
{
	OffsetNumber i;

	stat->free_space += PageGetExactFreeSpace(page);

	for (i = minoff; i <= maxoff; i = OffsetNumberNext(i))
	{
		ItemId		itemid = PageGetItemId(page, i);

		if (ItemIdIsDead(itemid))
		{
			stat->dead_tuple_count++;
			stat->dead_tuple_len += ItemIdGetLength(itemid);
		}
		else
		{
			stat->tuple_count++;
			stat->tuple_len += ItemIdGetLength(itemid);
		}
	}
}
