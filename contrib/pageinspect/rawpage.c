/*-------------------------------------------------------------------------
 *
 * rawpage.c
 *	  Functions to extract a raw page as bytea and inspect it
 *
 * Access-method specific inspection functions are in separate files.
 *
 * Copyright (c) 2007-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pageinspect/rawpage.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pageinspect.h"
#include "storage/bufmgr.h"
#include "storage/checksum.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC_EXT(
					.name = "pageinspect",
					.version = PG_VERSION
);

static bytea *get_raw_page_internal(text *relname, ForkNumber forknum,
									BlockNumber blkno);


/*
 * get_raw_page
 *
 * Returns a copy of a page from shared buffers as a bytea
 *
 * 从共享缓冲区返回页面的副本作为字节
 */
PG_FUNCTION_INFO_V1(get_raw_page_1_9);

Datum
get_raw_page_1_9(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	int64		blkno = PG_GETARG_INT64(1);
	bytea	   *raw_page;

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	raw_page = get_raw_page_internal(relname, MAIN_FORKNUM, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * entry point for old extension version
 *
 * 旧扩展版本的入口点
 */
PG_FUNCTION_INFO_V1(get_raw_page);

Datum
get_raw_page(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	uint32		blkno = PG_GETARG_UINT32(1);
	bytea	   *raw_page;

	/*
	 * We don't normally bother to check the number of arguments to a C
	 * function, but here it's needed for safety because early 8.4 beta
	 * releases mistakenly redefined get_raw_page() as taking three arguments.
	 *
	 * 我们通常不会费心检查 C 函数的参数数量，但出于安全考虑，这里需要这样做，因为早期的 8.4 beta 版本错误地将 get_raw_page() 重新定义为采用三个参数。
	 */
	if (PG_NARGS() != 2)
		ereport(ERROR,
				(errmsg("wrong number of arguments to get_raw_page()"),
				 errhint("Run the updated pageinspect.sql script.")));

	raw_page = get_raw_page_internal(relname, MAIN_FORKNUM, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * get_raw_page_fork
 *
 * Same, for any fork
 *
 * 同样，对于任何叉子
 */
PG_FUNCTION_INFO_V1(get_raw_page_fork_1_9);

Datum
get_raw_page_fork_1_9(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	text	   *forkname = PG_GETARG_TEXT_PP(1);
	int64		blkno = PG_GETARG_INT64(2);
	bytea	   *raw_page;
	ForkNumber	forknum;

	forknum = forkname_to_number(text_to_cstring(forkname));

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	raw_page = get_raw_page_internal(relname, forknum, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * Entry point for old extension version
 *
 * 旧扩展版本的入口点
 */
PG_FUNCTION_INFO_V1(get_raw_page_fork);

Datum
get_raw_page_fork(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	text	   *forkname = PG_GETARG_TEXT_PP(1);
	uint32		blkno = PG_GETARG_UINT32(2);
	bytea	   *raw_page;
	ForkNumber	forknum;

	forknum = forkname_to_number(text_to_cstring(forkname));

	raw_page = get_raw_page_internal(relname, forknum, blkno);

	PG_RETURN_BYTEA_P(raw_page);
}

/*
 * workhorse
 */
static bytea *
get_raw_page_internal(text *relname, ForkNumber forknum, BlockNumber blkno)
{
	bytea	   *raw_page;
	RangeVar   *relrv;
	Relation	rel;
	char	   *raw_page_data;
	Buffer		buf;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from relation \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

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

	if (blkno >= RelationGetNumberOfBlocksInFork(rel, forknum))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(rel))));

	/* Initialize buffer to copy to
	 *
	 * 初始化要复制到的缓冲区
	 */
	raw_page = (bytea *) palloc(BLCKSZ + VARHDRSZ);
	SET_VARSIZE(raw_page, BLCKSZ + VARHDRSZ);
	raw_page_data = VARDATA(raw_page);

	/* Take a verbatim copy of the page
	 *
	 * 逐字复制该页面
	 */

	buf = ReadBufferExtended(rel, forknum, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	memcpy(raw_page_data, BufferGetPage(buf), BLCKSZ);

	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buf);

	relation_close(rel, AccessShareLock);

	return raw_page;
}


/*
 * get_page_from_raw
 *
 * Get a palloc'd, maxalign'ed page image from the result of get_raw_page()
 *
 * 从 get_raw_page() 的结果中获取 palloc'd、maxalign'ed 页面图像
 *
 * On machines with MAXALIGN = 8, the payload of a bytea is not maxaligned,
 * since it will start 4 bytes into a palloc'd value.  On alignment-picky
 * machines, this will cause failures in accesses to 8-byte-wide values
 * within the page.  We don't need to worry if accessing only 4-byte or
 * smaller fields, but when examining a struct that contains 8-byte fields,
 * use this function for safety.
 *
 * 在 MAXALIGN = 8 的机器上，bytea 的有效负载不是最大对齐的，因为它将从 4 个字节开始进入 palloc'd 值。  在对齐挑剔的机器上，这将导致页面内 8 字节宽值的访问失败。  如果仅访问 4 字节或更小的字段，我们无需担心，但在检查包含 8 字节字段的结构时，为了安全起见，请使用此函数。
 */
Page
get_page_from_raw(bytea *raw_page)
{
	Page		page;
	int			raw_page_size;

	raw_page_size = VARSIZE_ANY_EXHDR(raw_page);

	if (raw_page_size != BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid page size"),
				 errdetail("Expected %d bytes, got %d.",
						   BLCKSZ, raw_page_size)));

	page = palloc(raw_page_size);

	memcpy(page, VARDATA_ANY(raw_page), raw_page_size);

	return page;
}


/*
 * page_header
 *
 * Allows inspection of page header fields of a raw page
 *
 * 允许检查原始页面的页眉字段
 */

PG_FUNCTION_INFO_V1(page_header);

Datum
page_header(PG_FUNCTION_ARGS)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);

	TupleDesc	tupdesc;

	Datum		result;
	HeapTuple	tuple;
	Datum		values[9];
	bool		nulls[9];

	Page		page;
	PageHeader	pageheader;
	XLogRecPtr	lsn;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	page = get_page_from_raw(raw_page);
	pageheader = (PageHeader) page;

	/* Build a tuple descriptor for our result type
	 *
	 * 为我们的结果类型构建一个元组描述符
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Extract information from the page header
	 *
	 * 从页眉中提取信息
	 */

	lsn = PageGetLSN(page);

	/* pageinspect >= 1.2 uses pg_lsn instead of text for the LSN field.
	 *
	 * pageinspect >= 1.2 使用 pg_lsn 而不是 LSN 字段的文本。
	 */
	if (TupleDescAttr(tupdesc, 0)->atttypid == TEXTOID)
	{
		char		lsnchar[64];

		snprintf(lsnchar, sizeof(lsnchar), "%X/%X", LSN_FORMAT_ARGS(lsn));
		values[0] = CStringGetTextDatum(lsnchar);
	}
	else
		values[0] = LSNGetDatum(lsn);
	values[1] = UInt16GetDatum(pageheader->pd_checksum);
	values[2] = UInt16GetDatum(pageheader->pd_flags);

	/* pageinspect >= 1.10 uses int4 instead of int2 for those fields
	 *
	 * pageinspect >= 1.10 对这些字段使用 int4 而不是 int2
	 */
	switch (TupleDescAttr(tupdesc, 3)->atttypid)
	{
		case INT2OID:
			Assert(TupleDescAttr(tupdesc, 4)->atttypid == INT2OID &&
				   TupleDescAttr(tupdesc, 5)->atttypid == INT2OID &&
				   TupleDescAttr(tupdesc, 6)->atttypid == INT2OID);
			values[3] = UInt16GetDatum(pageheader->pd_lower);
			values[4] = UInt16GetDatum(pageheader->pd_upper);
			values[5] = UInt16GetDatum(pageheader->pd_special);
			values[6] = UInt16GetDatum(PageGetPageSize(page));
			break;
		case INT4OID:
			Assert(TupleDescAttr(tupdesc, 4)->atttypid == INT4OID &&
				   TupleDescAttr(tupdesc, 5)->atttypid == INT4OID &&
				   TupleDescAttr(tupdesc, 6)->atttypid == INT4OID);
			values[3] = Int32GetDatum(pageheader->pd_lower);
			values[4] = Int32GetDatum(pageheader->pd_upper);
			values[5] = Int32GetDatum(pageheader->pd_special);
			values[6] = Int32GetDatum(PageGetPageSize(page));
			break;
		default:
			elog(ERROR, "incorrect output types");
			break;
	}

	values[7] = UInt16GetDatum(PageGetPageLayoutVersion(page));
	values[8] = TransactionIdGetDatum(pageheader->pd_prune_xid);

	/* Build and return the tuple.
	 *
	 * 构建并返回元组。
	 */

	memset(nulls, 0, sizeof(nulls));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * page_checksum
 *
 * Compute checksum of a raw page
 *
 * 计算原始页面的校验和
 */

PG_FUNCTION_INFO_V1(page_checksum_1_9);
PG_FUNCTION_INFO_V1(page_checksum);

static Datum
page_checksum_internal(PG_FUNCTION_ARGS, enum pageinspect_version ext_version)
{
	bytea	   *raw_page = PG_GETARG_BYTEA_P(0);
	int64		blkno = (ext_version == PAGEINSPECT_V1_8 ? PG_GETARG_UINT32(1) : PG_GETARG_INT64(1));
	Page		page;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use raw page functions")));

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	page = get_page_from_raw(raw_page);

	if (PageIsNew(page))
		PG_RETURN_NULL();

	PG_RETURN_INT16(pg_checksum_page(page, blkno));
}

Datum
page_checksum_1_9(PG_FUNCTION_ARGS)
{
	return page_checksum_internal(fcinfo, PAGEINSPECT_V1_9);
}

/*
 * Entry point for old extension version
 *
 * 旧扩展版本的入口点
 */
Datum
page_checksum(PG_FUNCTION_ARGS)
{
	return page_checksum_internal(fcinfo, PAGEINSPECT_V1_8);
}
