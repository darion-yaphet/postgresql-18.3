/*
 * contrib/pgstattuple/pgstatindex.c
 *
 *
 * pgstatindex
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

#include "access/gin_private.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/varlena.h"


/*
 * Because of backward-compatibility issue, we have decided to have
 * two types of interfaces, with regclass-type input arg and text-type
 * input arg, for each function.
 *
 * 由于向后兼容性问题，我们决定为每个函数提供两种类型的接口，即 regclass 类型输入 arg 和文本类型输入 arg。
 *
 * Those functions which have text-type input arg will be deprecated
 * in the future release.
 *
 * 那些具有文本类型输入参数的函数将在未来版本中被弃用。
 */
PG_FUNCTION_INFO_V1(pgstatindex);
PG_FUNCTION_INFO_V1(pgstatindexbyid);
PG_FUNCTION_INFO_V1(pg_relpages);
PG_FUNCTION_INFO_V1(pg_relpagesbyid);
PG_FUNCTION_INFO_V1(pgstatginindex);
PG_FUNCTION_INFO_V1(pgstathashindex);

PG_FUNCTION_INFO_V1(pgstatindex_v1_5);
PG_FUNCTION_INFO_V1(pgstatindexbyid_v1_5);
PG_FUNCTION_INFO_V1(pg_relpages_v1_5);
PG_FUNCTION_INFO_V1(pg_relpagesbyid_v1_5);
PG_FUNCTION_INFO_V1(pgstatginindex_v1_5);

Datum		pgstatginindex_internal(Oid relid, FunctionCallInfo fcinfo);

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_BTREE(r) ((r)->rd_rel->relam == BTREE_AM_OID)
#define IS_GIN(r) ((r)->rd_rel->relam == GIN_AM_OID)
#define IS_HASH(r) ((r)->rd_rel->relam == HASH_AM_OID)

/* ------------------------------------------------
 * A structure for a whole btree index statistics
 * used by pgstatindex().
 *
 * pgstatindex() 使用的整个 btree 索引统计信息的结构。
 * ------------------------------------------------
 */
typedef struct BTIndexStat
{
	uint32		version;
	uint32		level;
	BlockNumber root_blkno;

	uint64		internal_pages;
	uint64		leaf_pages;
	uint64		empty_pages;
	uint64		deleted_pages;

	uint64		max_avail;
	uint64		free_space;

	uint64		fragments;
} BTIndexStat;

/* ------------------------------------------------
 * A structure for a whole GIN index statistics
 * used by pgstatginindex().
 *
 * pgstatginindex() 使用的整个 GIN 索引统计信息的结构。
 * ------------------------------------------------
 */
typedef struct GinIndexStat
{
	int32		version;

	BlockNumber pending_pages;
	int64		pending_tuples;
} GinIndexStat;

/* ------------------------------------------------
 * A structure for a whole HASH index statistics
 * used by pgstathashindex().
 *
 * pgstathashindex() 使用的整个 HASH 索引统计信息的结构。
 * ------------------------------------------------
 */
typedef struct HashIndexStat
{
	int32		version;
	int32		space_per_page;

	BlockNumber bucket_pages;
	BlockNumber overflow_pages;
	BlockNumber bitmap_pages;
	BlockNumber unused_pages;

	int64		live_items;
	int64		dead_items;
	uint64		free_space;
} HashIndexStat;

static Datum pgstatindex_impl(Relation rel, FunctionCallInfo fcinfo);
static int64 pg_relpages_impl(Relation rel);
static void GetHashPageStats(Page page, HashIndexStat *stats);

/* ------------------------------------------------------
 * pgstatindex()
 *
 * Usage: SELECT * FROM pgstatindex('t1_pkey');
 *
 * 用法： SELECT * FROM pgstatindex('t1_pkey');
 *
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.5 installations
 * these functions could be called by any user.
 *
 * 必须保留此处的 superuser() 检查，因为库可能会在扩展未升级的情况下升级，这意味着在 1.5 之前的安装中，任何用户都可以调用这些函数。
 * ------------------------------------------------------
 */
Datum
pgstatindex(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Relation	rel;
	RangeVar   *relrv;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_DATUM(pgstatindex_impl(rel, fcinfo));
}

/*
 * As of pgstattuple version 1.5, we no longer need to check if the user
 * is a superuser because we REVOKE EXECUTE on the function from PUBLIC.
 * Users can then grant access to it based on their policies.
 *
 * 从 pgstattuple 版本 1.5 开始，我们不再需要检查用户是否是超级用户，因为我们从 PUBLIC 中撤销了函数上的 EXECUTE。然后，用户可以根据自己的策略授予对其的访问权限。
 *
 * Otherwise identical to pgstatindex (above).
 *
 * 其他方面与 pgstatindex （上面）相同。
 */
Datum
pgstatindex_v1_5(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Relation	rel;
	RangeVar   *relrv;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_DATUM(pgstatindex_impl(rel, fcinfo));
}

/*
 * The superuser() check here must be kept as the library might be upgraded
 * without the extension being upgraded, meaning that in pre-1.5 installations
 * these functions could be called by any user.
 *
 * 必须保留此处的 superuser() 检查，因为库可能会在扩展未升级的情况下升级，这意味着在 1.5 之前的安装中，任何用户都可以调用这些函数。
 */
Datum
pgstatindexbyid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_DATUM(pgstatindex_impl(rel, fcinfo));
}

/* No need for superuser checks in v1.5, see above
 *
 * v1.5 中不需要超级用户检查，见上文
 */
Datum
pgstatindexbyid_v1_5(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_DATUM(pgstatindex_impl(rel, fcinfo));
}

static Datum
pgstatindex_impl(Relation rel, FunctionCallInfo fcinfo)
{
	Datum		result;
	BlockNumber nblocks;
	BlockNumber blkno;
	BTIndexStat indexStat;
	BufferAccessStrategy bstrategy = GetAccessStrategy(BAS_BULKREAD);

	if (!IS_INDEX(rel) || !IS_BTREE(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a btree index",
						RelationGetRelationName(rel))));

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

	/*
	 * A !indisready index could lead to ERRCODE_DATA_CORRUPTED later, so exit
	 * early.  We're capable of assessing an indisready&&!indisvalid index,
	 * but the results could be confusing.  For example, the index's size
	 * could be too low for a valid index of the table.
	 *
	 * !indisready 索引可能会导致稍后出现 ERRCODE_DATA_CORRUPTED，因此请尽早退出。  我们能够评估 indisready&&!indisvalid 索引，但结果可能会令人困惑。  例如，索引的大小对于表的有效索引来说可能太小。
	 */
	if (!rel->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(rel))));

	/*
	 * Read metapage
	 *
	 * 阅读元页面
	 */
	{
		Buffer		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, 0, RBM_NORMAL, bstrategy);
		Page		page = BufferGetPage(buffer);
		BTMetaPageData *metad = BTPageGetMeta(page);

		indexStat.version = metad->btm_version;
		indexStat.level = metad->btm_level;
		indexStat.root_blkno = metad->btm_root;

		ReleaseBuffer(buffer);
	}

	/* -- init counters --
	 *
	 * -- 初始化计数器 --
	 */
	indexStat.internal_pages = 0;
	indexStat.leaf_pages = 0;
	indexStat.empty_pages = 0;
	indexStat.deleted_pages = 0;

	indexStat.max_avail = 0;
	indexStat.free_space = 0;

	indexStat.fragments = 0;

	/*
	 * Scan all blocks except the metapage
	 *
	 * 扫描除元页之外的所有块
	 */
	nblocks = RelationGetNumberOfBlocks(rel);

	for (blkno = 1; blkno < nblocks; blkno++)
	{
		Buffer		buffer;
		Page		page;
		BTPageOpaque opaque;

		CHECK_FOR_INTERRUPTS();

		/* Read and lock buffer
		 *
		 * 读取并锁定缓冲区
		 */
		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, bstrategy);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		opaque = BTPageGetOpaque(page);

		/*
		 * Determine page type, and update totals.
		 *
		 * 确定页面类型并更新总计。
		 *
		 * Note that we arbitrarily bucket deleted pages together without
		 * considering if they're leaf pages or internal pages.
		 *
		 * 请注意，我们任意将删除的页面存储在一起，而不考虑它们是叶页面还是内部页面。
		 */
		if (P_ISDELETED(opaque))
			indexStat.deleted_pages++;
		else if (P_IGNORE(opaque))
			indexStat.empty_pages++;	/* this is the "half dead" state */
		else if (P_ISLEAF(opaque))
		{
			int			max_avail;

			max_avail = BLCKSZ - (BLCKSZ - ((PageHeader) page)->pd_special + SizeOfPageHeaderData);
			indexStat.max_avail += max_avail;
			indexStat.free_space += PageGetExactFreeSpace(page);

			indexStat.leaf_pages++;

			/*
			 * If the next leaf is on an earlier block, it means a
			 * fragmentation.
			 *
			 * 如果下一个叶子位于较早的块上，则意味着碎片。
			 */
			if (opaque->btpo_next != P_NONE && opaque->btpo_next < blkno)
				indexStat.fragments++;
		}
		else
			indexStat.internal_pages++;

		/* Unlock and release buffer
		 *
		 * 解锁并释放缓冲区
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	relation_close(rel, AccessShareLock);

	/*----------------------------
	 * Build a result tuple
	 *
	 * 构建结果元组
	 *----------------------------
	 */
	{
		TupleDesc	tupleDesc;
		int			j;
		char	   *values[10];
		HeapTuple	tuple;

		/* Build a tuple descriptor for our result type
		 *
		 * 为我们的结果类型构建一个元组描述符
		 */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		j = 0;
		values[j++] = psprintf("%d", indexStat.version);
		values[j++] = psprintf("%d", indexStat.level);
		values[j++] = psprintf(INT64_FORMAT,
							   (1 + /* include the metapage in index_size */
								indexStat.leaf_pages +
								indexStat.internal_pages +
								indexStat.deleted_pages +
								indexStat.empty_pages) * BLCKSZ);
		values[j++] = psprintf("%u", indexStat.root_blkno);
		values[j++] = psprintf(INT64_FORMAT, indexStat.internal_pages);
		values[j++] = psprintf(INT64_FORMAT, indexStat.leaf_pages);
		values[j++] = psprintf(INT64_FORMAT, indexStat.empty_pages);
		values[j++] = psprintf(INT64_FORMAT, indexStat.deleted_pages);
		if (indexStat.max_avail > 0)
			values[j++] = psprintf("%.2f",
								   100.0 - (double) indexStat.free_space / (double) indexStat.max_avail * 100.0);
		else
			values[j++] = pstrdup("NaN");
		if (indexStat.leaf_pages > 0)
			values[j++] = psprintf("%.2f",
								   (double) indexStat.fragments / (double) indexStat.leaf_pages * 100.0);
		else
			values[j++] = pstrdup("NaN");

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
									   values);

		result = HeapTupleGetDatum(tuple);
	}

	return result;
}

/* --------------------------------------------------------
 * pg_relpages()
 *
 * Get the number of pages of the table/index.
 *
 * 获取表/索引的页数。
 *
 * Usage: SELECT pg_relpages('t1');
 *		  SELECT pg_relpages('t1_pkey');
 *
 * 用法： SELECT pg_relpages('t1');选择 pg_relpages('t1_pkey');
 *
 * Must keep superuser() check, see above.
 *
 * 必须保持 superuser() 检查，见上文。
 * --------------------------------------------------------
 */
Datum
pg_relpages(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Relation	rel;
	RangeVar   *relrv;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_INT64(pg_relpages_impl(rel));
}

/* No need for superuser checks in v1.5, see above
 *
 * v1.5 中不需要超级用户检查，见上文
 */
Datum
pg_relpages_v1_5(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	Relation	rel;
	RangeVar   *relrv;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	PG_RETURN_INT64(pg_relpages_impl(rel));
}

/* Must keep superuser() check, see above.
 *
 * 必须保持 superuser() 检查，见上文。
 */
Datum
pg_relpagesbyid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_INT64(pg_relpages_impl(rel));
}

/* No need for superuser checks in v1.5, see above
 *
 * v1.5 中不需要超级用户检查，见上文
 */
Datum
pg_relpagesbyid_v1_5(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;

	rel = relation_open(relid, AccessShareLock);

	PG_RETURN_INT64(pg_relpages_impl(rel));
}

static int64
pg_relpages_impl(Relation rel)
{
	int64		relpages;

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get page count of relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	/* note: this will work OK on non-local temp tables
	 *
	 * 注意：这在非本地临时表上可以正常工作
	 */

	relpages = RelationGetNumberOfBlocks(rel);

	relation_close(rel, AccessShareLock);

	return relpages;
}

/* ------------------------------------------------------
 * pgstatginindex()
 *
 * Usage: SELECT * FROM pgstatginindex('ginindex');
 *
 * 用法： SELECT * FROM pgstatginindex('ginindex');
 *
 * Must keep superuser() check, see above.
 *
 * 必须保持 superuser() 检查，见上文。
 * ------------------------------------------------------
 */
Datum
pgstatginindex(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pgstattuple functions")));

	PG_RETURN_DATUM(pgstatginindex_internal(relid, fcinfo));
}

/* No need for superuser checks in v1.5, see above
 *
 * v1.5 中不需要超级用户检查，见上文
 */
Datum
pgstatginindex_v1_5(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	PG_RETURN_DATUM(pgstatginindex_internal(relid, fcinfo));
}

Datum
pgstatginindex_internal(Oid relid, FunctionCallInfo fcinfo)
{
	Relation	rel;
	Buffer		buffer;
	Page		page;
	GinMetaPageData *metadata;
	GinIndexStat stats;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	Datum		result;

	rel = relation_open(relid, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_GIN(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a GIN index",
						RelationGetRelationName(rel))));

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
				 errmsg("cannot access temporary indexes of other sessions")));

	/* see pgstatindex_impl
	 *
	 * 参见 pgstatindex_impl
	 */
	if (!rel->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(rel))));

	/*
	 * Read metapage
	 *
	 * 阅读元页面
	 */
	buffer = ReadBuffer(rel, GIN_METAPAGE_BLKNO);
	LockBuffer(buffer, GIN_SHARE);
	page = BufferGetPage(buffer);
	metadata = GinPageGetMeta(page);

	stats.version = metadata->ginVersion;
	stats.pending_pages = metadata->nPendingPages;
	stats.pending_tuples = metadata->nPendingHeapTuples;

	UnlockReleaseBuffer(buffer);
	relation_close(rel, AccessShareLock);

	/*
	 * Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values[0] = Int32GetDatum(stats.version);
	values[1] = UInt32GetDatum(stats.pending_pages);
	values[2] = Int64GetDatum(stats.pending_tuples);

	/*
	 * Build and return the tuple
	 *
	 * 构建并返回元组
	 */
	tuple = heap_form_tuple(tupleDesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	return result;
}

/* ------------------------------------------------------
 * pgstathashindex()
 *
 * Usage: SELECT * FROM pgstathashindex('hashindex');
 *
 * 用法： SELECT * FROM pgstathashindex('hashindex');
 * ------------------------------------------------------
 */
Datum
pgstathashindex(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	BlockNumber nblocks;
	BlockNumber blkno;
	Relation	rel;
	HashIndexStat stats;
	BufferAccessStrategy bstrategy;
	HeapTuple	tuple;
	TupleDesc	tupleDesc;
	Datum		values[8];
	bool		nulls[8] = {0};
	Buffer		metabuf;
	HashMetaPage metap;
	float8		free_percent;
	uint64		total_space;

	rel = relation_open(relid, AccessShareLock);

	if (!IS_INDEX(rel) || !IS_HASH(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a hash index",
						RelationGetRelationName(rel))));

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
				 errmsg("cannot access temporary indexes of other sessions")));

	/* see pgstatindex_impl
	 *
	 * 参见 pgstatindex_impl
	 */
	if (!rel->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid",
						RelationGetRelationName(rel))));

	/* Get the information we need from the metapage.
	 *
	 * 从元页面获取我们需要的信息。
	 */
	memset(&stats, 0, sizeof(stats));
	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));
	stats.version = metap->hashm_version;
	stats.space_per_page = metap->hashm_bsize;
	_hash_relbuf(rel, metabuf);

	/* Get the current relation length
	 *
	 * 获取当前关系长度
	 */
	nblocks = RelationGetNumberOfBlocks(rel);

	/* prepare access strategy for this index
	 *
	 * 准备该索引的访问策略
	 */
	bstrategy = GetAccessStrategy(BAS_BULKREAD);

	/* Start from blkno 1 as 0th block is metapage
	 *
	 * 从 blkno 1 开始，因为第 0 个块是元页
	 */
	for (blkno = 1; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 bstrategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = (Page) BufferGetPage(buf);

		if (PageIsNew(page))
			stats.unused_pages++;
		else if (PageGetSpecialSize(page) !=
				 MAXALIGN(sizeof(HashPageOpaqueData)))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" contains corrupted page at block %u",
							RelationGetRelationName(rel),
							BufferGetBlockNumber(buf))));
		else
		{
			HashPageOpaque opaque;
			int			pagetype;

			opaque = HashPageGetOpaque(page);
			pagetype = opaque->hasho_flag & LH_PAGE_TYPE;

			if (pagetype == LH_BUCKET_PAGE)
			{
				stats.bucket_pages++;
				GetHashPageStats(page, &stats);
			}
			else if (pagetype == LH_OVERFLOW_PAGE)
			{
				stats.overflow_pages++;
				GetHashPageStats(page, &stats);
			}
			else if (pagetype == LH_BITMAP_PAGE)
				stats.bitmap_pages++;
			else if (pagetype == LH_UNUSED_PAGE)
				stats.unused_pages++;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("unexpected page type 0x%04X in HASH index \"%s\" block %u",
								opaque->hasho_flag, RelationGetRelationName(rel),
								BufferGetBlockNumber(buf))));
		}
		UnlockReleaseBuffer(buf);
	}

	/* Done accessing the index
	 *
	 * 完成索引访问
	 */
	index_close(rel, AccessShareLock);

	/* Count unused pages as free space.
	 *
	 * 将未使用的页面计为可用空间。
	 */
	stats.free_space += (uint64) stats.unused_pages * stats.space_per_page;

	/*
	 * Total space available for tuples excludes the metapage and the bitmap
	 * pages.
	 *
	 * 元组可用的总空间不包括元页和位图页。
	 */
	total_space = (uint64) (nblocks - (stats.bitmap_pages + 1)) *
		stats.space_per_page;

	if (total_space == 0)
		free_percent = 0.0;
	else
		free_percent = 100.0 * stats.free_space / total_space;

	/*
	 * Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupleDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupleDesc = BlessTupleDesc(tupleDesc);

	/*
	 * Build and return the tuple
	 *
	 * 构建并返回元组
	 */
	values[0] = Int32GetDatum(stats.version);
	values[1] = Int64GetDatum((int64) stats.bucket_pages);
	values[2] = Int64GetDatum((int64) stats.overflow_pages);
	values[3] = Int64GetDatum((int64) stats.bitmap_pages);
	values[4] = Int64GetDatum((int64) stats.unused_pages);
	values[5] = Int64GetDatum(stats.live_items);
	values[6] = Int64GetDatum(stats.dead_items);
	values[7] = Float8GetDatum(free_percent);
	tuple = heap_form_tuple(tupleDesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* -------------------------------------------------
 * GetHashPageStats()
 *
 * Collect statistics of single hash page
 *
 * 统计单个hash页
 * -------------------------------------------------
 */
static void
GetHashPageStats(Page page, HashIndexStat *stats)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	int			off;

	/* count live and dead tuples, and free space
	 *
	 * 计算活元组和死元组以及可用空间
	 */
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		id = PageGetItemId(page, off);

		if (!ItemIdIsDead(id))
			stats->live_items++;
		else
			stats->dead_items++;
	}
	stats->free_space += PageGetExactFreeSpace(page);
}
