/*-------------------------------------------------------------------------
 *
 * verify_heapam.c
 *	  Functions to check postgresql heap relations for corruption
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 *	  contrib/amcheck/verify_heapam.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(verify_heapam);

/* The number of columns in tuples returned by verify_heapam
 *
 * verify_heapam 返回的元组中的列数
 */
#define HEAPCHECK_RELATION_COLS 4

/* The largest valid toast va_rawsize
 *
 * 最大有效toast va_rawsize
 */
#define VARLENA_SIZE_LIMIT 0x3FFFFFFF

/*
 * Despite the name, we use this for reporting problems with both XIDs and
 * MXIDs.
 *
 * 尽管有这个名称，我们还是用它来报告 XID 和 MXID 的问题。
 */
typedef enum XidBoundsViolation
{
	XID_INVALID,
	XID_IN_FUTURE,
	XID_PRECEDES_CLUSTERMIN,
	XID_PRECEDES_RELMIN,
	XID_BOUNDS_OK,
} XidBoundsViolation;

typedef enum XidCommitStatus
{
	XID_COMMITTED,
	XID_IS_CURRENT_XID,
	XID_IN_PROGRESS,
	XID_ABORTED,
} XidCommitStatus;

typedef enum SkipPages
{
	SKIP_PAGES_ALL_FROZEN,
	SKIP_PAGES_ALL_VISIBLE,
	SKIP_PAGES_NONE,
} SkipPages;

/*
 * Struct holding information about a toasted attribute sufficient to both
 * check the toasted attribute and, if found to be corrupt, to report where it
 * was encountered in the main table.
 *
 * 保存有关 toasted 属性的信息的结构足以检查 toasted 属性，并且如果发现损坏，则报告在主表中遇到的位置。
 */
typedef struct ToastedAttribute
{
	struct varatt_external toast_pointer;
	BlockNumber blkno;			/* block in main table */
	OffsetNumber offnum;		/* offset in main table */
	AttrNumber	attnum;			/* attribute in main table */
} ToastedAttribute;

/*
 * Struct holding the running context information during
 * a lifetime of a verify_heapam execution.
 *
 * 在 verify_heapam 执行的生命周期内保存运行上下文信息的结构。
 */
typedef struct HeapCheckContext
{
	/*
	 * Cached copies of values from TransamVariables and computed values from
	 * them.
	 *
	 * TransamVariables 中的值的缓存副本以及从中计算出的值。
	 */
	FullTransactionId next_fxid;	/* TransamVariables->nextXid */
	TransactionId next_xid;		/* 32-bit version of next_fxid */
	TransactionId oldest_xid;	/* TransamVariables->oldestXid */
	FullTransactionId oldest_fxid;	/* 64-bit version of oldest_xid, computed
									 * relative to next_fxid */
	TransactionId safe_xmin;	/* this XID and newer ones can't become
								 * all-visible while we're running */

	/*
	 * Cached copy of value from MultiXactState
	 *
	 * 来自 MultiXactState 的值的缓存副本
	 */
	MultiXactId next_mxact;		/* MultiXactState->nextMXact */
	MultiXactId oldest_mxact;	/* MultiXactState->oldestMultiXactId */

	/*
	 * Cached copies of the most recently checked xid and its status.
	 *
	 * 最近检查的 xid 及其状态的缓存副本。
	 */
	TransactionId cached_xid;
	XidCommitStatus cached_status;

	/* Values concerning the heap relation being checked
	 *
	 * 有关正在检查的堆关系的值
	 */
	Relation	rel;
	TransactionId relfrozenxid;
	FullTransactionId relfrozenfxid;
	TransactionId relminmxid;
	Relation	toast_rel;
	Relation   *toast_indexes;
	Relation	valid_toast_index;
	int			num_toast_indexes;

	/*
	 * Values for iterating over pages in the relation. `blkno` is the most
	 * recent block in the buffer yielded by the read stream API.
	 *
	 * 用于迭代关系中的页面的值。 `blkno` 是读取流 API 生成的缓冲区中的最新块。
	 */
	BlockNumber blkno;
	BufferAccessStrategy bstrategy;
	Buffer		buffer;
	Page		page;

	/* Values for iterating over tuples within a page
	 *
	 * 用于迭代页面内元组的值
	 */
	OffsetNumber offnum;
	ItemId		itemid;
	uint16		lp_len;
	uint16		lp_off;
	HeapTupleHeader tuphdr;
	int			natts;

	/* Values for iterating over attributes within the tuple
	 *
	 * 用于迭代元组内属性的值
	 */
	uint32		offset;			/* offset in tuple data */
	AttrNumber	attnum;

	/* True if tuple's xmax makes it eligible for pruning
	 *
	 * 如果元组的 xmax 使其符合修剪条件，则为 True
	 */
	bool		tuple_could_be_pruned;

	/*
	 * List of ToastedAttribute structs for toasted attributes which are not
	 * eligible for pruning and should be checked
	 *
	 * 不符合修剪条件且应检查的 toasted 属性的 ToastedAttribute 结构列表
	 */
	List	   *toasted_attributes;

	/* Whether verify_heapam has yet encountered any corrupt tuples
	 *
	 * verify_heapam 是否遇到任何损坏的元组
	 */
	bool		is_corrupt;

	/* The descriptor and tuplestore for verify_heapam's result tuples
	 *
	 * verify_heapam 结果元组的描述符和元组存储
	 */
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
} HeapCheckContext;

/*
 * The per-relation data provided to the read stream API for heap amcheck to
 * use in its callback for the SKIP_PAGES_ALL_FROZEN and
 * SKIP_PAGES_ALL_VISIBLE options.
 *
 * 提供给堆 amcheck 的读取流 API 的每个关系数据，以在 SKIP_PAGES_ALL_FROZEN 和 SKIP_PAGES_ALL_VISIBLE 选项的回调中使用。
 */
typedef struct HeapCheckReadStreamData
{
	/*
	 * `range` is used by all SkipPages options. SKIP_PAGES_NONE uses the
	 * default read stream callback, block_range_read_stream_cb(), which takes
	 * a BlockRangeReadStreamPrivate as its callback_private_data. `range`
	 * keeps track of the current block number across
	 * read_stream_next_buffer() invocations.
	 *
	 * 所有 SkipPages 选项都使用“range”。 SKIP_PAGES_NONE 使用默认的读取流回调 block_range_read_stream_cb()，该回调将 BlockRangeReadStreamPrivate 作为其callback_private_data。 `range` 跟踪 read_stream_next_buffer() 调用中的当前块号。
	 */
	BlockRangeReadStreamPrivate range;
	SkipPages	skip_option;
	Relation	rel;
	Buffer	   *vmbuffer;
} HeapCheckReadStreamData;


/* Internal implementation
 *
 * 内部实施
 */
static BlockNumber heapcheck_read_stream_next_unskippable(ReadStream *stream,
														  void *callback_private_data,
														  void *per_buffer_data);

static void check_tuple(HeapCheckContext *ctx,
						bool *xmin_commit_status_ok,
						XidCommitStatus *xmin_commit_status);
static void check_toast_tuple(HeapTuple toasttup, HeapCheckContext *ctx,
							  ToastedAttribute *ta, int32 *expected_chunk_seq,
							  uint32 extsize);

static bool check_tuple_attribute(HeapCheckContext *ctx);
static void check_toasted_attribute(HeapCheckContext *ctx,
									ToastedAttribute *ta);

static bool check_tuple_header(HeapCheckContext *ctx);
static bool check_tuple_visibility(HeapCheckContext *ctx,
								   bool *xmin_commit_status_ok,
								   XidCommitStatus *xmin_commit_status);

static void report_corruption(HeapCheckContext *ctx, char *msg);
static void report_toast_corruption(HeapCheckContext *ctx,
									ToastedAttribute *ta, char *msg);
static FullTransactionId FullTransactionIdFromXidAndCtx(TransactionId xid,
														const HeapCheckContext *ctx);
static void update_cached_xid_range(HeapCheckContext *ctx);
static void update_cached_mxid_range(HeapCheckContext *ctx);
static XidBoundsViolation check_mxid_in_range(MultiXactId mxid,
											  HeapCheckContext *ctx);
static XidBoundsViolation check_mxid_valid_in_rel(MultiXactId mxid,
												  HeapCheckContext *ctx);
static XidBoundsViolation get_xid_status(TransactionId xid,
										 HeapCheckContext *ctx,
										 XidCommitStatus *status);

/*
 * Scan and report corruption in heap pages, optionally reconciling toasted
 * attributes with entries in the associated toast table.  Intended to be
 * called from SQL with the following parameters:
 *
 * 扫描并报告堆页面中的损坏，可以选择将 toast 属性与关联 toast 表中的条目进行协调。  旨在使用以下参数从 SQL 调用：
 *
 *   relation:
 *     The Oid of the heap relation to be checked.
 *
 * 关系：要检查的堆关系的Oid。
 *
 *   on_error_stop:
 *     Whether to stop at the end of the first page for which errors are
 *     detected.  Note that multiple rows may be returned.
 *
 * on_error_stop：是否在检测到错误的第一页末尾停止。  请注意，可能会返回多行。
 *
 *   check_toast:
 *     Whether to check each toasted attribute against the toast table to
 *     verify that it can be found there.
 *
 * check_toast：是否根据 toast 表检查每个 toast 属性，以验证是否可以在那里找到它。
 *
 *   skip:
 *     What kinds of pages in the heap relation should be skipped.  Valid
 *     options are "all-visible", "all-frozen", and "none".
 *
 * 跳过：应该跳过堆关系中的哪些类型的页面。  有效选项为“全部可见”、“全部冻结”和“无”。
 *
 * Returns to the SQL caller a set of tuples, each containing the location
 * and a description of a corruption found in the heap.
 *
 * 返回给 SQL 调用者一组元组，每个元组包含堆中发现的损坏的位置和描述。
 *
 * This code goes to some trouble to avoid crashing the server even if the
 * table pages are badly corrupted, but it's probably not perfect. If
 * check_toast is true, we'll use regular index lookups to try to fetch TOAST
 * tuples, which can certainly cause crashes if the right kind of corruption
 * exists in the toast table or index. No matter what parameters you pass,
 * we can't protect against crashes that might occur trying to look up the
 * commit status of transaction IDs (though we avoid trying to do such lookups
 * for transaction IDs that can't legally appear in the table).
 *
 * 即使表页严重损坏，此代码也会遇到一些麻烦，以避免服务器崩溃，但它可能并不完美。如果 check_toast 为 true，我们将使用常规索引查找来尝试获取 TOAST 元组，如果 toast 表或索引中存在正确类型的损坏，这肯定会导致崩溃。无论您传递什么参数，我们都无法防止尝试查找事务 ID 的提交状态时可能发生的崩溃（尽管我们避免尝试对无法合法出现在表中的事务 ID 进行此类查找）。
 */
Datum
verify_heapam(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HeapCheckContext ctx;
	Buffer		vmbuffer = InvalidBuffer;
	Oid			relid;
	bool		on_error_stop;
	bool		check_toast;
	SkipPages	skip_option = SKIP_PAGES_NONE;
	BlockNumber first_block;
	BlockNumber last_block;
	BlockNumber nblocks;
	const char *skip;
	ReadStream *stream;
	int			stream_flags;
	ReadStreamBlockNumberCB stream_cb;
	void	   *stream_data;
	HeapCheckReadStreamData stream_skip_data;

	/* Check supplied arguments
	 *
	 * 检查提供的参数
	 */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be null")));
	relid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("on_error_stop cannot be null")));
	on_error_stop = PG_GETARG_BOOL(1);

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("check_toast cannot be null")));
	check_toast = PG_GETARG_BOOL(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("skip cannot be null")));
	skip = text_to_cstring(PG_GETARG_TEXT_PP(3));
	if (pg_strcasecmp(skip, "all-visible") == 0)
		skip_option = SKIP_PAGES_ALL_VISIBLE;
	else if (pg_strcasecmp(skip, "all-frozen") == 0)
		skip_option = SKIP_PAGES_ALL_FROZEN;
	else if (pg_strcasecmp(skip, "none") == 0)
		skip_option = SKIP_PAGES_NONE;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid skip option"),
				 errhint("Valid skip options are \"all-visible\", \"all-frozen\", and \"none\".")));

	memset(&ctx, 0, sizeof(HeapCheckContext));
	ctx.cached_xid = InvalidTransactionId;
	ctx.toasted_attributes = NIL;

	/*
	 * Any xmin newer than the xmin of our snapshot can't become all-visible
	 * while we're running.
	 *
	 * 当我们运行时，任何比我们快照的 xmin 新的 xmin 都不能变得完全可见。
	 */
	ctx.safe_xmin = GetTransactionSnapshot()->xmin;

	/*
	 * If we report corruption when not examining some individual attribute,
	 * we need attnum to be reported as NULL.  Set that up before any
	 * corruption reporting might happen.
	 *
	 * 如果我们在不检查某些单独属性时报告损坏，则需要将 attnum 报告为 NULL。  在发生任何腐败报告之前进行设置。
	 */
	ctx.attnum = -1;

	/* Construct the tuplestore and tuple descriptor
	 *
	 * 构造元组存储和元组描述符
	 */
	InitMaterializedSRF(fcinfo, 0);
	ctx.tupdesc = rsinfo->setDesc;
	ctx.tupstore = rsinfo->setResult;

	/* Open relation, check relkind and access method
	 *
	 * 打开关系，检查relkind和访问方法
	 */
	ctx.rel = relation_open(relid, AccessShareLock);

	/*
	 * Check that a relation's relkind and access method are both supported.
	 *
	 * 检查关系的relkind 和访问方法是否都受支持。
	 */
	if (!RELKIND_HAS_TABLE_AM(ctx.rel->rd_rel->relkind) &&
		ctx.rel->rd_rel->relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot check relation \"%s\"",
						RelationGetRelationName(ctx.rel)),
				 errdetail_relkind_not_supported(ctx.rel->rd_rel->relkind)));

	/*
	 * Sequences always use heap AM, but they don't show that in the catalogs.
	 * Other relkinds might be using a different AM, so check.
	 *
	 * 序列始终使用堆 AM，但它们不会在目录中显示。其他亲属可能使用不同的 AM，因此请检查。
	 */
	if (ctx.rel->rd_rel->relkind != RELKIND_SEQUENCE &&
		ctx.rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	/*
	 * Early exit for unlogged relations during recovery.  These will have no
	 * relation fork, so there won't be anything to check.  We behave as if
	 * the relation is empty.
	 *
	 * 在恢复期间提前退出未记录的关系。  这些将没有关系分叉，因此不需要检查任何内容。  我们的行为就好像关系是空的。
	 */
	if (ctx.rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
		RecoveryInProgress())
	{
		ereport(DEBUG1,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot verify unlogged relation \"%s\" during recovery, skipping",
						RelationGetRelationName(ctx.rel))));
		relation_close(ctx.rel, AccessShareLock);
		PG_RETURN_NULL();
	}

	/* Early exit if the relation is empty
	 *
	 * 如果关系为空则提前退出
	 */
	nblocks = RelationGetNumberOfBlocks(ctx.rel);
	if (!nblocks)
	{
		relation_close(ctx.rel, AccessShareLock);
		PG_RETURN_NULL();
	}

	ctx.bstrategy = GetAccessStrategy(BAS_BULKREAD);
	ctx.buffer = InvalidBuffer;
	ctx.page = NULL;

	/* Validate block numbers, or handle nulls.
	 *
	 * 验证块号，或处理空值。
	 */
	if (PG_ARGISNULL(4))
		first_block = 0;
	else
	{
		int64		fb = PG_GETARG_INT64(4);

		if (fb < 0 || fb >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("starting block number must be between 0 and %u",
							nblocks - 1)));
		first_block = (BlockNumber) fb;
	}
	if (PG_ARGISNULL(5))
		last_block = nblocks - 1;
	else
	{
		int64		lb = PG_GETARG_INT64(5);

		if (lb < 0 || lb >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ending block number must be between 0 and %u",
							nblocks - 1)));
		last_block = (BlockNumber) lb;
	}

	/* Optionally open the toast relation, if any.
	 *
	 * 可以选择打开 toast 关系（如果有）。
	 */
	if (ctx.rel->rd_rel->reltoastrelid && check_toast)
	{
		int			offset;

		/* Main relation has associated toast relation
		 *
		 * 主要关系有关联的toast关系
		 */
		ctx.toast_rel = table_open(ctx.rel->rd_rel->reltoastrelid,
								   AccessShareLock);
		offset = toast_open_indexes(ctx.toast_rel,
									AccessShareLock,
									&(ctx.toast_indexes),
									&(ctx.num_toast_indexes));
		ctx.valid_toast_index = ctx.toast_indexes[offset];
	}
	else
	{
		/*
		 * Main relation has no associated toast relation, or we're
		 * intentionally skipping it.
		 *
		 * 主关系没有关联的 toast 关系，或者我们故意跳过它。
		 */
		ctx.toast_rel = NULL;
		ctx.toast_indexes = NULL;
		ctx.num_toast_indexes = 0;
	}

	update_cached_xid_range(&ctx);
	update_cached_mxid_range(&ctx);
	ctx.relfrozenxid = ctx.rel->rd_rel->relfrozenxid;
	ctx.relfrozenfxid = FullTransactionIdFromXidAndCtx(ctx.relfrozenxid, &ctx);
	ctx.relminmxid = ctx.rel->rd_rel->relminmxid;

	if (TransactionIdIsNormal(ctx.relfrozenxid))
		ctx.oldest_xid = ctx.relfrozenxid;

	/* Now that `ctx` is set up, set up the read stream
	 *
	 * 现在 `ctx` 已设置，请设置读取流
	 */
	stream_skip_data.range.current_blocknum = first_block;
	stream_skip_data.range.last_exclusive = last_block + 1;
	stream_skip_data.skip_option = skip_option;
	stream_skip_data.rel = ctx.rel;
	stream_skip_data.vmbuffer = &vmbuffer;

	if (skip_option == SKIP_PAGES_NONE)
	{
		/*
		 * It is safe to use batchmode as block_range_read_stream_cb takes no
		 * locks.
		 *
		 * 使用批处理模式是安全的，因为 block_range_read_stream_cb 不加锁。
		 */
		stream_cb = block_range_read_stream_cb;
		stream_flags = READ_STREAM_SEQUENTIAL |
			READ_STREAM_FULL |
			READ_STREAM_USE_BATCHING;
		stream_data = &stream_skip_data.range;
	}
	else
	{
		/*
		 * It would not be safe to naively use batchmode, as
		 * heapcheck_read_stream_next_unskippable takes locks. It shouldn't be
		 * too hard to convert though.
		 *
		 * 天真地使用批处理模式是不安全的，因为 heapcheck_read_stream_next_unskippable 需要锁。不过转换应该不会太难。
		 */
		stream_cb = heapcheck_read_stream_next_unskippable;
		stream_flags = READ_STREAM_DEFAULT;
		stream_data = &stream_skip_data;
	}

	stream = read_stream_begin_relation(stream_flags,
										ctx.bstrategy,
										ctx.rel,
										MAIN_FORKNUM,
										stream_cb,
										stream_data,
										0);

	while ((ctx.buffer = read_stream_next_buffer(stream, NULL)) != InvalidBuffer)
	{
		OffsetNumber maxoff;
		OffsetNumber predecessor[MaxOffsetNumber];
		OffsetNumber successor[MaxOffsetNumber];
		bool		lp_valid[MaxOffsetNumber];
		bool		xmin_commit_status_ok[MaxOffsetNumber];
		XidCommitStatus xmin_commit_status[MaxOffsetNumber];

		CHECK_FOR_INTERRUPTS();

		memset(predecessor, 0, sizeof(OffsetNumber) * MaxOffsetNumber);

		/* Lock the next page.
		 *
		 * 锁定下一页。
		 */
		Assert(BufferIsValid(ctx.buffer));
		LockBuffer(ctx.buffer, BUFFER_LOCK_SHARE);

		ctx.blkno = BufferGetBlockNumber(ctx.buffer);
		ctx.page = BufferGetPage(ctx.buffer);

		/* Perform tuple checks
		 *
		 * 执行元组检查
		 */
		maxoff = PageGetMaxOffsetNumber(ctx.page);
		for (ctx.offnum = FirstOffsetNumber; ctx.offnum <= maxoff;
			 ctx.offnum = OffsetNumberNext(ctx.offnum))
		{
			BlockNumber nextblkno;
			OffsetNumber nextoffnum;

			successor[ctx.offnum] = InvalidOffsetNumber;
			lp_valid[ctx.offnum] = false;
			xmin_commit_status_ok[ctx.offnum] = false;
			ctx.itemid = PageGetItemId(ctx.page, ctx.offnum);

			/* Skip over unused/dead line pointers
			 *
			 * 跳过未使用/截止线指针
			 */
			if (!ItemIdIsUsed(ctx.itemid) || ItemIdIsDead(ctx.itemid))
				continue;

			/*
			 * If this line pointer has been redirected, check that it
			 * redirects to a valid offset within the line pointer array
			 *
			 * 如果此行指针已被重定向，请检查它是否重定向到行指针数组中的有效偏移量
			 */
			if (ItemIdIsRedirected(ctx.itemid))
			{
				OffsetNumber rdoffnum = ItemIdGetRedirect(ctx.itemid);
				ItemId		rditem;

				if (rdoffnum < FirstOffsetNumber)
				{
					report_corruption(&ctx,
									  psprintf("line pointer redirection to item at offset %u precedes minimum offset %u",
											   (unsigned) rdoffnum,
											   (unsigned) FirstOffsetNumber));
					continue;
				}
				if (rdoffnum > maxoff)
				{
					report_corruption(&ctx,
									  psprintf("line pointer redirection to item at offset %u exceeds maximum offset %u",
											   (unsigned) rdoffnum,
											   (unsigned) maxoff));
					continue;
				}

				/*
				 * Since we've checked that this redirect points to a line
				 * pointer between FirstOffsetNumber and maxoff, it should now
				 * be safe to fetch the referenced line pointer. We expect it
				 * to be LP_NORMAL; if not, that's corruption.
				 *
				 * 由于我们已经检查此重定向是否指向 FirstOffsetNumber 和 maxoff 之间的行指针，因此现在应该可以安全地获取引用的行指针。我们期望它是 LP_NORMAL；如果没有，那就是腐败。
				 */
				rditem = PageGetItemId(ctx.page, rdoffnum);
				if (!ItemIdIsUsed(rditem))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to an unused item at offset %u",
											   (unsigned) rdoffnum));
					continue;
				}
				else if (ItemIdIsDead(rditem))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to a dead item at offset %u",
											   (unsigned) rdoffnum));
					continue;
				}
				else if (ItemIdIsRedirected(rditem))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to another redirected line pointer at offset %u",
											   (unsigned) rdoffnum));
					continue;
				}

				/*
				 * Record the fact that this line pointer has passed basic
				 * sanity checking, and also the offset number to which it
				 * points.
				 *
				 * 记录该行指针已通过基本健全性检查的事实，以及它指向的偏移量。
				 */
				lp_valid[ctx.offnum] = true;
				successor[ctx.offnum] = rdoffnum;
				continue;
			}

			/* Sanity-check the line pointer's offset and length values
			 *
			 * 健全性检查行指针的偏移量和长度值
			 */
			ctx.lp_len = ItemIdGetLength(ctx.itemid);
			ctx.lp_off = ItemIdGetOffset(ctx.itemid);

			if (ctx.lp_off != MAXALIGN(ctx.lp_off))
			{
				report_corruption(&ctx,
								  psprintf("line pointer to page offset %u is not maximally aligned",
										   ctx.lp_off));
				continue;
			}
			if (ctx.lp_len < MAXALIGN(SizeofHeapTupleHeader))
			{
				report_corruption(&ctx,
								  psprintf("line pointer length %u is less than the minimum tuple header size %u",
										   ctx.lp_len,
										   (unsigned) MAXALIGN(SizeofHeapTupleHeader)));
				continue;
			}
			if (ctx.lp_off + ctx.lp_len > BLCKSZ)
			{
				report_corruption(&ctx,
								  psprintf("line pointer to page offset %u with length %u ends beyond maximum page offset %u",
										   ctx.lp_off,
										   ctx.lp_len,
										   (unsigned) BLCKSZ));
				continue;
			}

			/* It should be safe to examine the tuple's header, at least
			 *
			 * 至少检查元组的标头应该是安全的
			 */
			lp_valid[ctx.offnum] = true;
			ctx.tuphdr = (HeapTupleHeader) PageGetItem(ctx.page, ctx.itemid);
			ctx.natts = HeapTupleHeaderGetNatts(ctx.tuphdr);

			/* Ok, ready to check this next tuple
			 *
			 * 好的，准备检查下一个元组
			 */
			check_tuple(&ctx,
						&xmin_commit_status_ok[ctx.offnum],
						&xmin_commit_status[ctx.offnum]);

			/*
			 * If the CTID field of this tuple seems to point to another tuple
			 * on the same page, record that tuple as the successor of this
			 * one.
			 *
			 * 如果该元组的 CTID 字段似乎指向同一页面上的另一个元组，则将该元组记录为该元组的后继。
			 */
			nextblkno = ItemPointerGetBlockNumber(&(ctx.tuphdr)->t_ctid);
			nextoffnum = ItemPointerGetOffsetNumber(&(ctx.tuphdr)->t_ctid);
			if (nextblkno == ctx.blkno && nextoffnum != ctx.offnum &&
				nextoffnum >= FirstOffsetNumber && nextoffnum <= maxoff)
				successor[ctx.offnum] = nextoffnum;
		}

		/*
		 * Update chain validation. Check each line pointer that's got a valid
		 * successor against that successor.
		 *
		 * 更新链验证。检查每个具有有效后继者的行指针与该后继者。
		 */
		ctx.attnum = -1;
		for (ctx.offnum = FirstOffsetNumber; ctx.offnum <= maxoff;
			 ctx.offnum = OffsetNumberNext(ctx.offnum))
		{
			ItemId		curr_lp;
			ItemId		next_lp;
			HeapTupleHeader curr_htup;
			HeapTupleHeader next_htup;
			TransactionId curr_xmin;
			TransactionId curr_xmax;
			TransactionId next_xmin;
			OffsetNumber nextoffnum = successor[ctx.offnum];

			/*
			 * The current line pointer may not have a successor, either
			 * because it's not valid or because it didn't point to anything.
			 * In either case, we have to give up.
			 *
			 * 当前行指针可能没有后继，因为它无效或因为它没有指向任何内容。无论哪种情况，我们都必须放弃。
			 *
			 * If the current line pointer does point to something, it's
			 * possible that the target line pointer isn't valid. We have to
			 * give up in that case, too.
			 *
			 * 如果当前行指针确实指向某些内容，则目标行指针可能无效。在这种情况下我们也必须放弃。
			 */
			if (nextoffnum == InvalidOffsetNumber || !lp_valid[nextoffnum])
				continue;

			/* We have two valid line pointers that we can examine.
			 *
			 * 我们有两个可以检查的有效行指针。
			 */
			curr_lp = PageGetItemId(ctx.page, ctx.offnum);
			next_lp = PageGetItemId(ctx.page, nextoffnum);

			/* Handle the cases where the current line pointer is a redirect.
			 *
			 * 处理当前行指针是重定向的情况。
			 */
			if (ItemIdIsRedirected(curr_lp))
			{
				/*
				 * We should not have set successor[ctx.offnum] to a value
				 * other than InvalidOffsetNumber unless that line pointer is
				 * LP_NORMAL.
				 *
				 * 我们不应该将 successor[ctx.offnum] 设置为 InvalidOffsetNumber 以外的值，除非该行指针是 LP_NORMAL。
				 */
				Assert(ItemIdIsNormal(next_lp));

				/* Can only redirect to a HOT tuple.
				 *
				 * 只能重定向到 HOT 元组。
				 */
				next_htup = (HeapTupleHeader) PageGetItem(ctx.page, next_lp);
				if (!HeapTupleHeaderIsHeapOnly(next_htup))
				{
					report_corruption(&ctx,
									  psprintf("redirected line pointer points to a non-heap-only tuple at offset %u",
											   (unsigned) nextoffnum));
				}

				/* HOT chains should not intersect.
				 *
				 * HOT 链不应交叉。
				 */
				if (predecessor[nextoffnum] != InvalidOffsetNumber)
				{
					report_corruption(&ctx,
									  psprintf("redirect line pointer points to offset %u, but offset %u also points there",
											   (unsigned) nextoffnum, (unsigned) predecessor[nextoffnum]));
					continue;
				}

				/*
				 * This redirect and the tuple to which it points seem to be
				 * part of an update chain.
				 *
				 * 此重定向及其指向的元组似乎是更新链的一部分。
				 */
				predecessor[nextoffnum] = ctx.offnum;
				continue;
			}

			/*
			 * If the next line pointer is a redirect, or if it's a tuple but
			 * the XMAX of this tuple doesn't match the XMIN of the next
			 * tuple, then the two aren't part of the same update chain and
			 * there is nothing more to do.
			 *
			 * 如果下一行指针是重定向，或者它是一个元组，但该元组的 XMAX 与下一个元组的 XMIN 不匹配，则两者不属于同一更新链，并且无需执行更多操作。
			 */
			if (ItemIdIsRedirected(next_lp))
				continue;
			curr_htup = (HeapTupleHeader) PageGetItem(ctx.page, curr_lp);
			curr_xmax = HeapTupleHeaderGetUpdateXid(curr_htup);
			next_htup = (HeapTupleHeader) PageGetItem(ctx.page, next_lp);
			next_xmin = HeapTupleHeaderGetXmin(next_htup);
			if (!TransactionIdIsValid(curr_xmax) ||
				!TransactionIdEquals(curr_xmax, next_xmin))
				continue;

			/* HOT chains should not intersect.
			 *
			 * HOT 链不应交叉。
			 */
			if (predecessor[nextoffnum] != InvalidOffsetNumber)
			{
				report_corruption(&ctx,
								  psprintf("tuple points to new version at offset %u, but offset %u also points there",
										   (unsigned) nextoffnum, (unsigned) predecessor[nextoffnum]));
				continue;
			}

			/*
			 * This tuple and the tuple to which it points seem to be part of
			 * an update chain.
			 *
			 * 该元组及其指向的元组似乎是更新链的一部分。
			 */
			predecessor[nextoffnum] = ctx.offnum;

			/*
			 * If the current tuple is marked as HOT-updated, then the next
			 * tuple should be marked as a heap-only tuple. Conversely, if the
			 * current tuple isn't marked as HOT-updated, then the next tuple
			 * shouldn't be marked as a heap-only tuple.
			 *
			 * 如果当前元组被标记为热更新，则下一个元组应被标记为仅堆元组。相反，如果当前元组未标记为热更新，则下一个元组不应标记为仅堆元组。
			 *
			 * NB: Can't use HeapTupleHeaderIsHotUpdated() as it checks if
			 * hint bits indicate xmin/xmax aborted.
			 *
			 * 注意：不能使用 HeapTupleHeaderIsHotUpdated()，因为它检查提示位是否指示 xmin/xmax 已中止。
			 */
			if (!(curr_htup->t_infomask2 & HEAP_HOT_UPDATED) &&
				HeapTupleHeaderIsHeapOnly(next_htup))
			{
				report_corruption(&ctx,
								  psprintf("non-heap-only update produced a heap-only tuple at offset %u",
										   (unsigned) nextoffnum));
			}
			if ((curr_htup->t_infomask2 & HEAP_HOT_UPDATED) &&
				!HeapTupleHeaderIsHeapOnly(next_htup))
			{
				report_corruption(&ctx,
								  psprintf("heap-only update produced a non-heap only tuple at offset %u",
										   (unsigned) nextoffnum));
			}

			/*
			 * If the current tuple's xmin is still in progress but the
			 * successor tuple's xmin is committed, that's corruption.
			 *
			 * 如果当前元组的 xmin 仍在进行中，但后继元组的 xmin 已提交，则属于损坏。
			 *
			 * NB: We recheck the commit status of the current tuple's xmin
			 * here, because it might have committed after we checked it and
			 * before we checked the commit status of the successor tuple's
			 * xmin. This should be safe because the xmin itself can't have
			 * changed, only its commit status.
			 *
			 * 注意：我们在这里重新检查当前元组的 xmin 的提交状态，因为它可能在我们检查之后和检查后继元组的 xmin 的提交状态之前已经提交。这应该是安全的，因为 xmin 本身不能改变，只能改变它的提交状态。
			 */
			curr_xmin = HeapTupleHeaderGetXmin(curr_htup);
			if (xmin_commit_status_ok[ctx.offnum] &&
				xmin_commit_status[ctx.offnum] == XID_IN_PROGRESS &&
				xmin_commit_status_ok[nextoffnum] &&
				xmin_commit_status[nextoffnum] == XID_COMMITTED &&
				TransactionIdIsInProgress(curr_xmin))
			{
				report_corruption(&ctx,
								  psprintf("tuple with in-progress xmin %u was updated to produce a tuple at offset %u with committed xmin %u",
										   (unsigned) curr_xmin,
										   (unsigned) ctx.offnum,
										   (unsigned) next_xmin));
			}

			/*
			 * If the current tuple's xmin is aborted but the successor
			 * tuple's xmin is in-progress or committed, that's corruption.
			 *
			 * 如果当前元组的 xmin 已中止，但后继元组的 xmin 正在进行或已提交，则属于损坏。
			 */
			if (xmin_commit_status_ok[ctx.offnum] &&
				xmin_commit_status[ctx.offnum] == XID_ABORTED &&
				xmin_commit_status_ok[nextoffnum])
			{
				if (xmin_commit_status[nextoffnum] == XID_IN_PROGRESS)
					report_corruption(&ctx,
									  psprintf("tuple with aborted xmin %u was updated to produce a tuple at offset %u with in-progress xmin %u",
											   (unsigned) curr_xmin,
											   (unsigned) ctx.offnum,
											   (unsigned) next_xmin));
				else if (xmin_commit_status[nextoffnum] == XID_COMMITTED)
					report_corruption(&ctx,
									  psprintf("tuple with aborted xmin %u was updated to produce a tuple at offset %u with committed xmin %u",
											   (unsigned) curr_xmin,
											   (unsigned) ctx.offnum,
											   (unsigned) next_xmin));
			}
		}

		/*
		 * An update chain can start either with a non-heap-only tuple or with
		 * a redirect line pointer, but not with a heap-only tuple.
		 *
		 * 更新链可以从非堆元组或重定向行指针开始，但不能从堆元组开始。
		 *
		 * (This check is in a separate loop because we need the predecessor
		 * array to be fully populated before we can perform it.)
		 *
		 * （此检查位于单独的循环中，因为我们需要先完全填充前驱数组，然后才能执行它。）
		 */
		for (ctx.offnum = FirstOffsetNumber;
			 ctx.offnum <= maxoff;
			 ctx.offnum = OffsetNumberNext(ctx.offnum))
		{
			if (xmin_commit_status_ok[ctx.offnum] &&
				(xmin_commit_status[ctx.offnum] == XID_COMMITTED ||
				 xmin_commit_status[ctx.offnum] == XID_IN_PROGRESS) &&
				predecessor[ctx.offnum] == InvalidOffsetNumber)
			{
				ItemId		curr_lp;

				curr_lp = PageGetItemId(ctx.page, ctx.offnum);
				if (!ItemIdIsRedirected(curr_lp))
				{
					HeapTupleHeader curr_htup;

					curr_htup = (HeapTupleHeader)
						PageGetItem(ctx.page, curr_lp);
					if (HeapTupleHeaderIsHeapOnly(curr_htup))
						report_corruption(&ctx,
										  psprintf("tuple is root of chain but is marked as heap-only tuple"));
				}
			}
		}

		/* clean up
		 *
		 * 清理
		 */
		UnlockReleaseBuffer(ctx.buffer);

		/*
		 * Check any toast pointers from the page whose lock we just released
		 *
		 * 检查我们刚刚释放锁的页面中的任何 toast 指针
		 */
		if (ctx.toasted_attributes != NIL)
		{
			ListCell   *cell;

			foreach(cell, ctx.toasted_attributes)
				check_toasted_attribute(&ctx, lfirst(cell));
			list_free_deep(ctx.toasted_attributes);
			ctx.toasted_attributes = NIL;
		}

		if (on_error_stop && ctx.is_corrupt)
			break;
	}

	read_stream_end(stream);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/* Close the associated toast table and indexes, if any.
	 *
	 * 关闭关联的 Toast 表和索引（如果有）。
	 */
	if (ctx.toast_indexes)
		toast_close_indexes(ctx.toast_indexes, ctx.num_toast_indexes,
							AccessShareLock);
	if (ctx.toast_rel)
		table_close(ctx.toast_rel, AccessShareLock);

	/* Close the main relation
	 *
	 * 关闭主要关系
	 */
	relation_close(ctx.rel, AccessShareLock);

	PG_RETURN_NULL();
}

/*
 * Heap amcheck's read stream callback for getting the next unskippable block.
 * This callback is only used when 'all-visible' or 'all-frozen' is provided
 * as the skip option to verify_heapam(). With the default 'none',
 * block_range_read_stream_cb() is used instead.
 *
 * 堆 amcheck 的读取流回调，用于获取下一个不可跳过的块。仅当提供“all-visible”或“all-frozen”作为 verify_heapam() 的跳过选项时，才会使用此回调。默认为“none”，则使用 block_range_read_stream_cb()。
 */
static BlockNumber
heapcheck_read_stream_next_unskippable(ReadStream *stream,
									   void *callback_private_data,
									   void *per_buffer_data)
{
	HeapCheckReadStreamData *p = callback_private_data;

	/* Loops over [current_blocknum, last_exclusive) blocks
	 *
	 * 循环 [current_blocknum, last_exclusive) 块
	 */
	for (BlockNumber i; (i = p->range.current_blocknum++) < p->range.last_exclusive;)
	{
		uint8		mapbits = visibilitymap_get_status(p->rel, i, p->vmbuffer);

		if (p->skip_option == SKIP_PAGES_ALL_FROZEN)
		{
			if ((mapbits & VISIBILITYMAP_ALL_FROZEN) != 0)
				continue;
		}

		if (p->skip_option == SKIP_PAGES_ALL_VISIBLE)
		{
			if ((mapbits & VISIBILITYMAP_ALL_VISIBLE) != 0)
				continue;
		}

		return i;
	}

	return InvalidBlockNumber;
}

/*
 * Shared internal implementation for report_corruption and
 * report_toast_corruption.
 *
 * report_corruption 和 report_toast_corruption 的共享内部实现。
 */
static void
report_corruption_internal(Tuplestorestate *tupstore, TupleDesc tupdesc,
						   BlockNumber blkno, OffsetNumber offnum,
						   AttrNumber attnum, char *msg)
{
	Datum		values[HEAPCHECK_RELATION_COLS] = {0};
	bool		nulls[HEAPCHECK_RELATION_COLS] = {0};
	HeapTuple	tuple;

	values[0] = Int64GetDatum(blkno);
	values[1] = Int32GetDatum(offnum);
	values[2] = Int32GetDatum(attnum);
	nulls[2] = (attnum < 0);
	values[3] = CStringGetTextDatum(msg);

	/*
	 * In principle, there is nothing to prevent a scan over a large, highly
	 * corrupted table from using work_mem worth of memory building up the
	 * tuplestore.  That's ok, but if we also leak the msg argument memory
	 * until the end of the query, we could exceed work_mem by more than a
	 * trivial amount.  Therefore, free the msg argument each time we are
	 * called rather than waiting for our current memory context to be freed.
	 *
	 * 原则上，没有什么可以阻止对大型、高度损坏的表的扫描使用构建元组存储的 work_mem 内存。  没关系，但如果我们还泄漏 msg 参数内存直到查询结束，我们可能会超出 work_mem 一个微不足道的量。  因此，每次调用时释放 msg 参数，而不是等待当前的内存上下文被释放。
	 */
	pfree(msg);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	tuplestore_puttuple(tupstore, tuple);
}

/*
 * Record a single corruption found in the main table.  The values in ctx should
 * indicate the location of the corruption, and the msg argument should contain
 * a human-readable description of the corruption.
 *
 * 记录主表中发现的单个损坏。  ctx 中的值应指示损坏的位置，并且 msg 参数应包含人类可读的损坏描述。
 *
 * The msg argument is pfree'd by this function.
 *
 * msg 参数被该函数释放。
 */
static void
report_corruption(HeapCheckContext *ctx, char *msg)
{
	report_corruption_internal(ctx->tupstore, ctx->tupdesc, ctx->blkno,
							   ctx->offnum, ctx->attnum, msg);
	ctx->is_corrupt = true;
}

/*
 * Record corruption found in the toast table.  The values in ta should
 * indicate the location in the main table where the toast pointer was
 * encountered, and the msg argument should contain a human-readable
 * description of the toast table corruption.
 *
 * Toast 表中发现记录损坏。  ta 中的值应指示主表中遇到 toast 指针的位置，并且 msg 参数应包含 toast 表损坏的人类可读的描述。
 *
 * As above, the msg argument is pfree'd by this function.
 *
 * 如上所述，msg 参数被该函数释放。
 */
static void
report_toast_corruption(HeapCheckContext *ctx, ToastedAttribute *ta,
						char *msg)
{
	report_corruption_internal(ctx->tupstore, ctx->tupdesc, ta->blkno,
							   ta->offnum, ta->attnum, msg);
	ctx->is_corrupt = true;
}

/*
 * Check for tuple header corruption.
 *
 * 检查元组标头是否损坏。
 *
 * Some kinds of corruption make it unsafe to check the tuple attributes, for
 * example when the line pointer refers to a range of bytes outside the page.
 * In such cases, we return false (not checkable) after recording appropriate
 * corruption messages.
 *
 * 某些类型的损坏使得检查元组属性变得不安全，例如当行指针引用页面外部的字节范围时。在这种情况下，我们在记录适当的损坏消息后返回 false（不可检查）。
 *
 * Some other kinds of tuple header corruption confuse the question of where
 * the tuple attributes begin, or how long the nulls bitmap is, etc., making it
 * unreasonable to attempt to check attributes, even if all candidate answers
 * to those questions would not result in reading past the end of the line
 * pointer or page.  In such cases, like above, we record corruption messages
 * about the header and then return false.
 *
 * 其他一些类型的元组标头损坏混淆了元组属性从哪里开始或空位图有多长等问题，使得尝试检查属性变得不合理，即使这些问题的所有候选答案都不会导致读取超过行指针或页的末尾。  在这种情况下，就像上面一样，我们记录有关标头的损坏消息，然后返回 false。
 *
 * Other kinds of tuple header corruption do not bear on the question of
 * whether the tuple attributes can be checked, so we record corruption
 * messages for them but we do not return false merely because we detected
 * them.
 *
 * 其他类型的元组头损坏与是否可以检查元组属性的问题无关，因此我们为它们记录损坏消息，但我们不会仅仅因为检测到它们而返回 false。
 *
 * Returns whether the tuple is sufficiently sensible to undergo visibility and
 * attribute checks.
 *
 * 返回元组是否足够敏感以进行可见性和属性检查。
 */
static bool
check_tuple_header(HeapCheckContext *ctx)
{
	HeapTupleHeader tuphdr = ctx->tuphdr;
	uint16		infomask = tuphdr->t_infomask;
	TransactionId curr_xmax = HeapTupleHeaderGetUpdateXid(tuphdr);
	bool		result = true;
	unsigned	expected_hoff;

	if (ctx->tuphdr->t_hoff > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("data begins at offset %u beyond the tuple length %u",
								   ctx->tuphdr->t_hoff, ctx->lp_len));
		result = false;
	}

	if ((ctx->tuphdr->t_infomask & HEAP_XMAX_COMMITTED) &&
		(ctx->tuphdr->t_infomask & HEAP_XMAX_IS_MULTI))
	{
		report_corruption(ctx,
						  pstrdup("multixact should not be marked committed"));

		/*
		 * This condition is clearly wrong, but it's not enough to justify
		 * skipping further checks, because we don't rely on this to determine
		 * whether the tuple is visible or to interpret other relevant header
		 * fields.
		 *
		 * 这个条件显然是错误的，但不足以证明跳过进一步的检查是合理的，因为我们不依赖于此来确定元组是否可见或解释其他相关头字段。
		 */
	}

	if (!TransactionIdIsValid(curr_xmax) &&
		HeapTupleHeaderIsHotUpdated(tuphdr))
	{
		report_corruption(ctx,
						  psprintf("tuple has been HOT updated, but xmax is 0"));

		/*
		 * As above, even though this shouldn't happen, it's not sufficient
		 * justification for skipping further checks, we should still be able
		 * to perform sensibly.
		 *
		 * 如上所述，尽管这种情况不应该发生，但这并不足以成为跳过进一步检查的理由，我们仍然应该能够明智地执行。
		 */
	}

	if (HeapTupleHeaderIsHeapOnly(tuphdr) &&
		((tuphdr->t_infomask & HEAP_UPDATED) == 0))
	{
		report_corruption(ctx,
						  psprintf("tuple is heap only, but not the result of an update"));

		/* Here again, we can still perform further checks.
		 *
		 * 在这里，我们仍然可以进行进一步的检查。
		 */
	}

	if (infomask & HEAP_HASNULL)
		expected_hoff = MAXALIGN(SizeofHeapTupleHeader + BITMAPLEN(ctx->natts));
	else
		expected_hoff = MAXALIGN(SizeofHeapTupleHeader);
	if (ctx->tuphdr->t_hoff != expected_hoff)
	{
		if ((infomask & HEAP_HASNULL) && ctx->natts == 1)
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (1 attribute, has nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff));
		else if ((infomask & HEAP_HASNULL))
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (%u attributes, has nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff, ctx->natts));
		else if (ctx->natts == 1)
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (1 attribute, no nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff));
		else
			report_corruption(ctx,
							  psprintf("tuple data should begin at byte %u, but actually begins at byte %u (%u attributes, no nulls)",
									   expected_hoff, ctx->tuphdr->t_hoff, ctx->natts));
		result = false;
	}

	return result;
}

/*
 * Checks tuple visibility so we know which further checks are safe to
 * perform.
 *
 * 检查元组可见性，以便我们知道哪些进一步检查可以安全执行。
 *
 * If a tuple could have been inserted by a transaction that also added a
 * column to the table, but which ultimately did not commit, or which has not
 * yet committed, then the table's current TupleDesc might differ from the one
 * used to construct this tuple, so we must not check it.
 *
 * 如果一个元组可能是由一个事务插入的，该事务也向表中添加了一列，但最终没有提交，或者尚未提交，那么表的当前 TupleDesc 可能与用于构造该元组的不同，因此我们不能检查它。
 *
 * As a special case, if our own transaction inserted the tuple, even if we
 * added a column to the table, our TupleDesc should match.  We could check the
 * tuple, but choose not to do so.
 *
 * 作为一种特殊情况，如果我们自己的事务插入了元组，即使我们向表中添加了一列，我们的 TupleDesc 也应该匹配。  我们可以检查元组，但选择不这样做。
 *
 * If a tuple has been updated or deleted, we can still read the old tuple for
 * corruption checking purposes, as long as we are careful about concurrent
 * vacuums.  The main table tuple itself cannot be vacuumed away because we
 * hold a buffer lock on the page, but if the deleting transaction is older
 * than our transaction snapshot's xmin, then vacuum could remove the toast at
 * any time, so we must not try to follow TOAST pointers.
 *
 * 如果元组已更新或删除，只要我们小心并发真空，我们仍然可以读取旧元组以进行损坏检查。  主表元组本身无法被清理掉，因为我们在页面上持有缓冲区锁，但如果删除事务早于事务快照的 xmin，则清理可以随时删除 toast，因此我们不能尝试遵循 TOAST 指针。
 *
 * If xmin or xmax values are older than can be checked against clog, or appear
 * to be in the future (possibly due to wrap-around), then we cannot make a
 * determination about the visibility of the tuple, so we skip further checks.
 *
 * 如果 xmin 或 xmax 值比可以针对堵塞进行检查的值更旧，或者看起来是将来的值（可能是由于环绕），那么我们无法确定元组的可见性，因此我们跳过进一步的检查。
 *
 * Returns true if the tuple itself should be checked, false otherwise.  Sets
 * ctx->tuple_could_be_pruned if the tuple -- and thus also any associated
 * TOAST tuples -- are eligible for pruning.
 *
 * 如果应该检查元组本身，则返回 true，否则返回 false。  如果元组（以及任何关联的 TOAST 元组）符合修剪条件，则设置 ctx->tuple_could_be_pruned。
 *
 * Sets *xmin_commit_status_ok to true if the commit status of xmin is known
 * and false otherwise. If it's set to true, then also set *xmin_commit_status
 * to the actual commit status.
 *
 * 如果 xmin 的提交状态已知，则将 *xmin_commit_status_ok 设置为 true，否则设置为 false。如果它设置为 true，则还将 *xmin_commit_status 设置为实际提交状态。
 */
static bool
check_tuple_visibility(HeapCheckContext *ctx, bool *xmin_commit_status_ok,
					   XidCommitStatus *xmin_commit_status)
{
	TransactionId xmin;
	TransactionId xvac;
	TransactionId xmax;
	XidCommitStatus xmin_status;
	XidCommitStatus xvac_status;
	XidCommitStatus xmax_status;
	HeapTupleHeader tuphdr = ctx->tuphdr;

	ctx->tuple_could_be_pruned = true;	/* have not yet proven otherwise */
	*xmin_commit_status_ok = false; /* have not yet proven otherwise */

	/* If xmin is normal, it should be within valid range
	 *
	 * 如果xmin正常，应该在有效范围内
	 */
	xmin = HeapTupleHeaderGetXmin(tuphdr);
	switch (get_xid_status(xmin, ctx, &xmin_status))
	{
		case XID_INVALID:
			/* Could be the result of a speculative insertion that aborted.
			 *
			 * 可能是推测性插入中止的结果。
			 */
			return false;
		case XID_BOUNDS_OK:
			*xmin_commit_status_ok = true;
			*xmin_commit_status = xmin_status;
			break;
		case XID_IN_FUTURE:
			report_corruption(ctx,
							  psprintf("xmin %u equals or exceeds next valid transaction ID %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->next_fxid),
									   XidFromFullTransactionId(ctx->next_fxid)));
			return false;
		case XID_PRECEDES_CLUSTERMIN:
			report_corruption(ctx,
							  psprintf("xmin %u precedes oldest valid transaction ID %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->oldest_fxid),
									   XidFromFullTransactionId(ctx->oldest_fxid)));
			return false;
		case XID_PRECEDES_RELMIN:
			report_corruption(ctx,
							  psprintf("xmin %u precedes relation freeze threshold %u:%u",
									   xmin,
									   EpochFromFullTransactionId(ctx->relfrozenfxid),
									   XidFromFullTransactionId(ctx->relfrozenfxid)));
			return false;
	}

	/*
	 * Has inserting transaction committed?
	 *
	 * 插入事务是否已提交？
	 */
	if (!HeapTupleHeaderXminCommitted(tuphdr))
	{
		if (HeapTupleHeaderXminInvalid(tuphdr))
			return false;		/* inserter aborted, don't check */
		/* Used by pre-9.0 binary upgrades
		 *
		 * 由 9.0 之前的二进制升级使用
		 */
		else if (tuphdr->t_infomask & HEAP_MOVED_OFF)
		{
			xvac = HeapTupleHeaderGetXvac(tuphdr);

			switch (get_xid_status(xvac, ctx, &xvac_status))
			{
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("old-style VACUUM FULL transaction ID for moved off tuple is invalid"));
					return false;
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple equals or exceeds next valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple precedes relation freeze threshold %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple precedes oldest valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;
				case XID_BOUNDS_OK:
					break;
			}

			switch (xvac_status)
			{
				case XID_IS_CURRENT_XID:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple matches our current transaction ID",
											   xvac));
					return false;
				case XID_IN_PROGRESS:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved off tuple appears to be in progress",
											   xvac));
					return false;

				case XID_COMMITTED:

					/*
					 * The tuple is dead, because the xvac transaction moved
					 * it off and committed. It's checkable, but also
					 * prunable.
					 *
					 * 该元组已死亡，因为 xvac 事务将其移走并提交。它是可检查的，但也是可修剪的。
					 */
					return true;

				case XID_ABORTED:

					/*
					 * The original xmin must have committed, because the xvac
					 * transaction tried to move it later. Since xvac is
					 * aborted, whether it's still alive now depends on the
					 * status of xmax.
					 *
					 * 原始 xmin 必须已提交，因为 xvac 事务稍后尝试移动它。由于 xvac 已中止，因此它现在是否还活着取决于 xmax 的状态。
					 */
					break;
			}
		}
		/* Used by pre-9.0 binary upgrades
		 *
		 * 由 9.0 之前的二进制升级使用
		 */
		else if (tuphdr->t_infomask & HEAP_MOVED_IN)
		{
			xvac = HeapTupleHeaderGetXvac(tuphdr);

			switch (get_xid_status(xvac, ctx, &xvac_status))
			{
				case XID_INVALID:
					report_corruption(ctx,
									  pstrdup("old-style VACUUM FULL transaction ID for moved in tuple is invalid"));
					return false;
				case XID_IN_FUTURE:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple equals or exceeds next valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->next_fxid),
											   XidFromFullTransactionId(ctx->next_fxid)));
					return false;
				case XID_PRECEDES_RELMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple precedes relation freeze threshold %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->relfrozenfxid),
											   XidFromFullTransactionId(ctx->relfrozenfxid)));
					return false;
				case XID_PRECEDES_CLUSTERMIN:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple precedes oldest valid transaction ID %u:%u",
											   xvac,
											   EpochFromFullTransactionId(ctx->oldest_fxid),
											   XidFromFullTransactionId(ctx->oldest_fxid)));
					return false;
				case XID_BOUNDS_OK:
					break;
			}

			switch (xvac_status)
			{
				case XID_IS_CURRENT_XID:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple matches our current transaction ID",
											   xvac));
					return false;
				case XID_IN_PROGRESS:
					report_corruption(ctx,
									  psprintf("old-style VACUUM FULL transaction ID %u for moved in tuple appears to be in progress",
											   xvac));
					return false;

				case XID_COMMITTED:

					/*
					 * The original xmin must have committed, because the xvac
					 * transaction moved it later. Whether it's still alive
					 * now depends on the status of xmax.
					 *
					 * 原来的 xmin 一定已经提交了，因为 xvac 事务稍后移动了它。现在是否还活着取决于xmax的状态。
					 */
					break;

				case XID_ABORTED:

					/*
					 * The tuple is dead, because the xvac transaction moved
					 * it off and committed. It's checkable, but also
					 * prunable.
					 *
					 * 该元组已死亡，因为 xvac 事务将其移走并提交。它是可检查的，但也是可修剪的。
					 */
					return true;
			}
		}
		else if (xmin_status != XID_COMMITTED)
		{
			/*
			 * Inserting transaction is not in progress, and not committed, so
			 * it might have changed the TupleDesc in ways we don't know
			 * about. Thus, don't try to check the tuple structure.
			 *
			 * 插入事务尚未进行，也未提交，因此它可能以我们不知道的方式更改了 TupleDesc。因此，不要尝试检查元组结构。
			 *
			 * If xmin_status happens to be XID_IS_CURRENT_XID, then in theory
			 * any such DDL changes ought to be visible to us, so perhaps we
			 * could check anyway in that case. But, for now, let's be
			 * conservative and treat this like any other uncommitted insert.
			 *
			 * 如果 xmin_status 恰好是 XID_IS_CURRENT_XID，那么理论上任何此类 DDL 更改都应该对我们可见，因此也许在这种情况下我们可以检查。但是，现在，让我们保守一点，像对待任何其他未提交的插入一样对待它。
			 */
			return false;
		}
	}

	/*
	 * Okay, the inserter committed, so it was good at some point.  Now what
	 * about the deleting transaction?
	 *
	 * 好的，插入器已提交，所以在某些时候效果很好。  现在删除交易怎么样？
	 */

	if (tuphdr->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/*
		 * xmax is a multixact, so sanity-check the MXID. Note that we do this
		 * prior to checking for HEAP_XMAX_INVALID or
		 * HEAP_XMAX_IS_LOCKED_ONLY. This might therefore complain about
		 * things that wouldn't actually be a problem during a normal scan,
		 * but eventually we're going to have to freeze, and that process will
		 * ignore hint bits.
		 *
		 * xmax 是一个 multixact，因此请对 MXID 进行健全性检查。请注意，我们在检查 HEAP_XMAX_INVALID 或 HEAP_XMAX_IS_LOCKED_ONLY 之前执行此操作。因此，这可能会抱怨在正常扫描期间实际上不会出现问题的事情，但最终我们将不得不冻结，并且该过程将忽略提示位。
		 *
		 * Even if the MXID is out of range, we still know that the original
		 * insert committed, so we can check the tuple itself. However, we
		 * can't rule out the possibility that this tuple is dead, so don't
		 * clear ctx->tuple_could_be_pruned. Possibly we should go ahead and
		 * clear that flag anyway if HEAP_XMAX_INVALID is set or if
		 * HEAP_XMAX_IS_LOCKED_ONLY is true, but for now we err on the side of
		 * avoiding possibly-bogus complaints about missing TOAST entries.
		 *
		 * 即使 MXID 超出范围，我们仍然知道原始插入已提交，因此我们可以检查元组本身。不过，我们不能排除这个tuple已经死的可能性，所以不要清除ctx->tuple_could_be_pruned。如果设置了 HEAP_XMAX_INVALID 或者 HEAP_XMAX_IS_LOCKED_ONLY 为 true，我们可能应该继续清除该标志，但现在我们宁愿避免可能伪造的关于缺少 TOAST 条目的投诉。
		 */
		xmax = HeapTupleHeaderGetRawXmax(tuphdr);
		switch (check_mxid_valid_in_rel(xmax, ctx))
		{
			case XID_INVALID:
				report_corruption(ctx,
								  pstrdup("multitransaction ID is invalid"));
				return true;
			case XID_PRECEDES_RELMIN:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u precedes relation minimum multitransaction ID threshold %u",
										   xmax, ctx->relminmxid));
				return true;
			case XID_PRECEDES_CLUSTERMIN:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u precedes oldest valid multitransaction ID threshold %u",
										   xmax, ctx->oldest_mxact));
				return true;
			case XID_IN_FUTURE:
				report_corruption(ctx,
								  psprintf("multitransaction ID %u equals or exceeds next valid multitransaction ID %u",
										   xmax,
										   ctx->next_mxact));
				return true;
			case XID_BOUNDS_OK:
				break;
		}
	}

	if (tuphdr->t_infomask & HEAP_XMAX_INVALID)
	{
		/*
		 * This tuple is live.  A concurrently running transaction could
		 * delete it before we get around to checking the toast, but any such
		 * running transaction is surely not less than our safe_xmin, so the
		 * toast cannot be vacuumed out from under us.
		 *
		 * 该元组是活动的。  并发运行的事务可以在我们检查 toast 之前将其删除，但是任何此类正在运行的事务肯定不小于我们的 safe_xmin，因此无法将 toast 从我们下面清除。
		 */
		ctx->tuple_could_be_pruned = false;
		return true;
	}

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuphdr->t_infomask))
	{
		/*
		 * "Deleting" xact really only locked it, so the tuple is live in any
		 * case.  As above, a concurrently running transaction could delete
		 * it, but it cannot be vacuumed out from under us.
		 *
		 * “删除”xact 实际上只是锁定了它，因此无论如何该元组都是活动的。  如上所述，并发运行的事务可以删除它，但不能将其从我们下面清除。
		 */
		ctx->tuple_could_be_pruned = false;
		return true;
	}

	if (tuphdr->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/*
		 * We already checked above that this multixact is within limits for
		 * this table.  Now check the update xid from this multixact.
		 *
		 * 我们已经在上面检查过该 multixact 是否在该表的限制范围内。  现在检查来自该 multixact 的更新 xid。
		 */
		xmax = HeapTupleGetUpdateXid(tuphdr);
		switch (get_xid_status(xmax, ctx, &xmax_status))
		{
			case XID_INVALID:
				/* not LOCKED_ONLY, so it has to have an xmax
				 *
				 * 不是 LOCKED_ONLY，所以它必须有一个 xmax
				 */
				report_corruption(ctx,
								  pstrdup("update xid is invalid"));
				return true;
			case XID_IN_FUTURE:
				report_corruption(ctx,
								  psprintf("update xid %u equals or exceeds next valid transaction ID %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->next_fxid),
										   XidFromFullTransactionId(ctx->next_fxid)));
				return true;
			case XID_PRECEDES_RELMIN:
				report_corruption(ctx,
								  psprintf("update xid %u precedes relation freeze threshold %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->relfrozenfxid),
										   XidFromFullTransactionId(ctx->relfrozenfxid)));
				return true;
			case XID_PRECEDES_CLUSTERMIN:
				report_corruption(ctx,
								  psprintf("update xid %u precedes oldest valid transaction ID %u:%u",
										   xmax,
										   EpochFromFullTransactionId(ctx->oldest_fxid),
										   XidFromFullTransactionId(ctx->oldest_fxid)));
				return true;
			case XID_BOUNDS_OK:
				break;
		}

		switch (xmax_status)
		{
			case XID_IS_CURRENT_XID:
			case XID_IN_PROGRESS:

				/*
				 * The delete is in progress, so it cannot be visible to our
				 * snapshot.
				 *
				 * 删除正在进行中，因此我们的快照无法看到它。
				 */
				ctx->tuple_could_be_pruned = false;
				break;
			case XID_COMMITTED:

				/*
				 * The delete committed.  Whether the toast can be vacuumed
				 * away depends on how old the deleting transaction is.
				 *
				 * 删除已提交。  toast是否可以被清理掉取决于删除事务的年龄。
				 */
				ctx->tuple_could_be_pruned = TransactionIdPrecedes(xmax,
																   ctx->safe_xmin);
				break;
			case XID_ABORTED:

				/*
				 * The delete aborted or crashed.  The tuple is still live.
				 *
				 * 删除中止或崩溃。  该元组仍然存在。
				 */
				ctx->tuple_could_be_pruned = false;
				break;
		}

		/* Tuple itself is checkable even if it's dead.
		 *
		 * 元组本身是可检查的，即使它已经死了。
		 */
		return true;
	}

	/* xmax is an XID, not a MXID. Sanity check it.
	 *
	 * xmax 是 XID，而不是 MXID。理智检查一下。
	 */
	xmax = HeapTupleHeaderGetRawXmax(tuphdr);
	switch (get_xid_status(xmax, ctx, &xmax_status))
	{
		case XID_INVALID:
			ctx->tuple_could_be_pruned = false;
			return true;
		case XID_IN_FUTURE:
			report_corruption(ctx,
							  psprintf("xmax %u equals or exceeds next valid transaction ID %u:%u",
									   xmax,
									   EpochFromFullTransactionId(ctx->next_fxid),
									   XidFromFullTransactionId(ctx->next_fxid)));
			return false;		/* corrupt */
		case XID_PRECEDES_RELMIN:
			report_corruption(ctx,
							  psprintf("xmax %u precedes relation freeze threshold %u:%u",
									   xmax,
									   EpochFromFullTransactionId(ctx->relfrozenfxid),
									   XidFromFullTransactionId(ctx->relfrozenfxid)));
			return false;		/* corrupt */
		case XID_PRECEDES_CLUSTERMIN:
			report_corruption(ctx,
							  psprintf("xmax %u precedes oldest valid transaction ID %u:%u",
									   xmax,
									   EpochFromFullTransactionId(ctx->oldest_fxid),
									   XidFromFullTransactionId(ctx->oldest_fxid)));
			return false;		/* corrupt */
		case XID_BOUNDS_OK:
			break;
	}

	/*
	 * Whether the toast can be vacuumed away depends on how old the deleting
	 * transaction is.
	 *
	 * toast是否可以被清理掉取决于删除事务的年龄。
	 */
	switch (xmax_status)
	{
		case XID_IS_CURRENT_XID:
		case XID_IN_PROGRESS:

			/*
			 * The delete is in progress, so it cannot be visible to our
			 * snapshot.
			 *
			 * 删除正在进行中，因此我们的快照无法看到它。
			 */
			ctx->tuple_could_be_pruned = false;
			break;

		case XID_COMMITTED:

			/*
			 * The delete committed.  Whether the toast can be vacuumed away
			 * depends on how old the deleting transaction is.
			 *
			 * 删除已提交。  toast是否可以被清理掉取决于删除事务的年龄。
			 */
			ctx->tuple_could_be_pruned = TransactionIdPrecedes(xmax,
															   ctx->safe_xmin);
			break;

		case XID_ABORTED:

			/*
			 * The delete aborted or crashed.  The tuple is still live.
			 *
			 * 删除中止或崩溃。  该元组仍然存在。
			 */
			ctx->tuple_could_be_pruned = false;
			break;
	}

	/* Tuple itself is checkable even if it's dead.
	 *
	 * 元组本身是可检查的，即使它已经死了。
	 */
	return true;
}


/*
 * Check the current toast tuple against the state tracked in ctx, recording
 * any corruption found in ctx->tupstore.
 *
 * 根据 ctx 中跟踪的状态检查当前的 toast 元组，记录 ctx->tupstore 中发现的任何损坏。
 *
 * This is not equivalent to running verify_heapam on the toast table itself,
 * and is not hardened against corruption of the toast table.  Rather, when
 * validating a toasted attribute in the main table, the sequence of toast
 * tuples that store the toasted value are retrieved and checked in order, with
 * each toast tuple being checked against where we are in the sequence, as well
 * as each toast tuple having its varlena structure sanity checked.
 *
 * 这并不等同于在 toast 表本身上运行 verify_heapam，并且没有针对 toast 表的损坏进行强化。  相反，当验证主表中的 toasted 属性时，将按顺序检索和检查存储 toasted 值的 toast 元组序列，根据我们在序列中的位置检查每个 toast 元组，并检查每个 toast 元组的 varlena 结构健全性。
 *
 * On entry, *expected_chunk_seq should be the chunk_seq value that we expect
 * to find in toasttup. On exit, it will be updated to the value the next call
 * to this function should expect to see.
 *
 * 输入时，*expected_chunk_seq 应该是我们期望在 toasttup 中找到的 chunk_seq 值。退出时，它将更新为下次调用该函数时应该看到的值。
 */
static void
check_toast_tuple(HeapTuple toasttup, HeapCheckContext *ctx,
				  ToastedAttribute *ta, int32 *expected_chunk_seq,
				  uint32 extsize)
{
	int32		chunk_seq;
	int32		last_chunk_seq = (extsize - 1) / TOAST_MAX_CHUNK_SIZE;
	Pointer		chunk;
	bool		isnull;
	int32		chunksize;
	int32		expected_size;

	/* Sanity-check the sequence number.
	 *
	 * 健全性检查序列号。
	 */
	chunk_seq = DatumGetInt32(fastgetattr(toasttup, 2,
										  ctx->toast_rel->rd_att, &isnull));
	if (isnull)
	{
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u has toast chunk with null sequence number",
										 ta->toast_pointer.va_valueid));
		return;
	}
	if (chunk_seq != *expected_chunk_seq)
	{
		/* Either the TOAST index is corrupt, or we don't have all chunks.
		 *
		 * 要么 TOAST 索引已损坏，要么我们没有所有块。
		 */
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u index scan returned chunk %d when expecting chunk %d",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, *expected_chunk_seq));
	}
	*expected_chunk_seq = chunk_seq + 1;

	/* Sanity-check the chunk data.
	 *
	 * 健全性检查块数据。
	 */
	chunk = DatumGetPointer(fastgetattr(toasttup, 3,
										ctx->toast_rel->rd_att, &isnull));
	if (isnull)
	{
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d has null data",
										 ta->toast_pointer.va_valueid,
										 chunk_seq));
		return;
	}
	if (!VARATT_IS_EXTENDED(chunk))
		chunksize = VARSIZE(chunk) - VARHDRSZ;
	else if (VARATT_IS_SHORT(chunk))
	{
		/*
		 * could happen due to heap_form_tuple doing its thing
		 *
		 * 可能由于 heap_form_tuple 做它的事情而发生
		 */
		chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
	}
	else
	{
		/* should never happen
		 *
		 * 永远不应该发生
		 */
		uint32		header = ((varattrib_4b *) chunk)->va_4byte.va_header;

		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d has invalid varlena header %0x",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, header));
		return;
	}

	/*
	 * Some checks on the data we've found
	 *
	 * 对我们发现的数据进行一些检查
	 */
	if (chunk_seq > last_chunk_seq)
	{
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d follows last expected chunk %d",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, last_chunk_seq));
		return;
	}

	expected_size = chunk_seq < last_chunk_seq ? TOAST_MAX_CHUNK_SIZE
		: extsize - (last_chunk_seq * TOAST_MAX_CHUNK_SIZE);

	if (chunksize != expected_size)
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u chunk %d has size %u, but expected size %u",
										 ta->toast_pointer.va_valueid,
										 chunk_seq, chunksize, expected_size));
}

/*
 * Check the current attribute as tracked in ctx, recording any corruption
 * found in ctx->tupstore.
 *
 * 检查 ctx 中跟踪的当前属性，记录 ctx->tupstore 中发现的任何损坏。
 *
 * This function follows the logic performed by heap_deform_tuple(), and in the
 * case of a toasted value, optionally stores the toast pointer so later it can
 * be checked following the logic of detoast_external_attr(), checking for any
 * conditions that would result in either of those functions Asserting or
 * crashing the backend.  The checks performed by Asserts present in those two
 * functions are also performed here and in check_toasted_attribute.  In cases
 * where those two functions are a bit cavalier in their assumptions about data
 * being correct, we perform additional checks not present in either of those
 * two functions.  Where some condition is checked in both of those functions,
 * we perform it here twice, as we parallel the logical flow of those two
 * functions.  The presence of duplicate checks seems a reasonable price to pay
 * for keeping this code tightly coupled with the code it protects.
 *
 * 该函数遵循 heap_deform_tuple() 执行的逻辑，并且在 toasted 值的情况下，可以选择存储 toast 指针，以便稍后可以按照 detoast_external_attr() 的逻辑对其进行检查，检查是否有任何可能导致这些函数断言或后端崩溃的条件。  这两个函数中存在的断言执行的检查也在此处和 check_toasted_attribute 中执行。  如果这两个函数对数据正确性的假设有点漫不经心，我​​们会执行这两个函数中都不存在的额外检查。  当在这两个函数中检查某些条件时，我们在这里执行两次，因为我们并行这两个函数的逻辑流程。  为了使该代码与其所保护的代码紧密结合，重复检查的存在似乎是一个合理的代价。
 *
 * Returns true if the tuple attribute is sane enough for processing to
 * continue on to the next attribute, false otherwise.
 *
 * 如果元组属性足够健全，可以继续处理下一个属性，则返回 true，否则返回 false。
 */
static bool
check_tuple_attribute(HeapCheckContext *ctx)
{
	Datum		attdatum;
	struct varlena *attr;
	char	   *tp;				/* pointer to the tuple data */
	uint16		infomask;
	CompactAttribute *thisatt;
	struct varatt_external toast_pointer;

	infomask = ctx->tuphdr->t_infomask;
	thisatt = TupleDescCompactAttr(RelationGetDescr(ctx->rel), ctx->attnum);

	tp = (char *) ctx->tuphdr + ctx->tuphdr->t_hoff;

	if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("attribute with length %u starts at offset %u beyond total tuple length %u",
								   thisatt->attlen,
								   ctx->tuphdr->t_hoff + ctx->offset,
								   ctx->lp_len));
		return false;
	}

	/* Skip null values
	 *
	 * 跳过空值
	 */
	if (infomask & HEAP_HASNULL && att_isnull(ctx->attnum, ctx->tuphdr->t_bits))
		return true;

	/* Skip non-varlena values, but update offset first
	 *
	 * 跳过非 varlena 值，但首先更新偏移量
	 */
	if (thisatt->attlen != -1)
	{
		ctx->offset = att_nominal_alignby(ctx->offset, thisatt->attalignby);
		ctx->offset = att_addlength_pointer(ctx->offset, thisatt->attlen,
											tp + ctx->offset);
		if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
		{
			report_corruption(ctx,
							  psprintf("attribute with length %u ends at offset %u beyond total tuple length %u",
									   thisatt->attlen,
									   ctx->tuphdr->t_hoff + ctx->offset,
									   ctx->lp_len));
			return false;
		}
		return true;
	}

	/* Ok, we're looking at a varlena attribute.
	 *
	 * 好的，我们正在研究 varlena 属性。
	 */
	ctx->offset = att_pointer_alignby(ctx->offset, thisatt->attalignby, -1,
									  tp + ctx->offset);

	/* Get the (possibly corrupt) varlena datum
	 *
	 * 获取（可能已损坏的）varlena 数据
	 */
	attdatum = fetchatt(thisatt, tp + ctx->offset);

	/*
	 * We have the datum, but we cannot decode it carelessly, as it may still
	 * be corrupt.
	 *
	 * 我们有数据，但我们不能粗心地对其进行解码，因为它可能仍然是损坏的。
	 */

	/*
	 * Check that VARTAG_SIZE won't hit an Assert on a corrupt va_tag before
	 * risking a call into att_addlength_pointer
	 *
	 * 在冒险调用 att_addlength_pointer 之前，检查 VARTAG_SIZE 是否不会在损坏的 va_tag 上触发断言
	 */
	if (VARATT_IS_EXTERNAL(tp + ctx->offset))
	{
		uint8		va_tag = VARTAG_EXTERNAL(tp + ctx->offset);

		if (va_tag != VARTAG_ONDISK)
		{
			report_corruption(ctx,
							  psprintf("toasted attribute has unexpected TOAST tag %u",
									   va_tag));
			/* We can't know where the next attribute begins
			 *
			 * 我们无法知道下一个属性从哪里开始
			 */
			return false;
		}
	}

	/* Ok, should be safe now
	 *
	 * 好的，现在应该安全了
	 */
	ctx->offset = att_addlength_pointer(ctx->offset, thisatt->attlen,
										tp + ctx->offset);

	if (ctx->tuphdr->t_hoff + ctx->offset > ctx->lp_len)
	{
		report_corruption(ctx,
						  psprintf("attribute with length %u ends at offset %u beyond total tuple length %u",
								   thisatt->attlen,
								   ctx->tuphdr->t_hoff + ctx->offset,
								   ctx->lp_len));

		return false;
	}

	/*
	 * heap_deform_tuple would be done with this attribute at this point,
	 * having stored it in values[], and would continue to the next attribute.
	 * We go further, because we need to check if the toast datum is corrupt.
	 *
	 * 此时，heap_deform_tuple 将使用此属性完成，并将其存储在 value[] 中，并将继续处理下一个属性。我们更进一步，因为我们需要检查 toast 数据是否损坏。
	 */

	attr = (struct varlena *) DatumGetPointer(attdatum);

	/*
	 * Now we follow the logic of detoast_external_attr(), with the same
	 * caveats about being paranoid about corruption.
	 *
	 * 现在我们遵循 detoast_external_attr() 的逻辑，对于腐败的偏执也有同样的警告。
	 */

	/* Skip values that are not external
	 *
	 * 跳过非外部值
	 */
	if (!VARATT_IS_EXTERNAL(attr))
		return true;

	/* It is external, and we're looking at a page on disk
	 *
	 * 它是外部的，我们正在查看磁盘上的页面
	 */

	/*
	 * Must copy attr into toast_pointer for alignment considerations
	 *
	 * 出于对齐考虑，必须将 attr 复制到 toast_pointer
	 */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/* Toasted attributes too large to be untoasted should never be stored
	 *
	 * 切勿存储太大而无法未烘烤的烘烤属性
	 */
	if (toast_pointer.va_rawsize > VARLENA_SIZE_LIMIT)
		report_corruption(ctx,
						  psprintf("toast value %u rawsize %d exceeds limit %d",
								   toast_pointer.va_valueid,
								   toast_pointer.va_rawsize,
								   VARLENA_SIZE_LIMIT));

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
	{
		ToastCompressionId cmid;
		bool		valid = false;

		/* Compressed attributes should have a valid compression method
		 *
		 * 压缩属性应该有一个有效的压缩方法
		 */
		cmid = TOAST_COMPRESS_METHOD(&toast_pointer);
		switch (cmid)
		{
				/* List of all valid compression method IDs
				 *
				 * 所有有效压缩方法 ID 的列表
				 */
			case TOAST_PGLZ_COMPRESSION_ID:
			case TOAST_LZ4_COMPRESSION_ID:
				valid = true;
				break;

				/* Recognized but invalid compression method ID
				 *
				 * 已识别但无效的压缩方法 ID
				 */
			case TOAST_INVALID_COMPRESSION_ID:
				break;

				/* Intentionally no default here
				 *
				 * 这里故意不设置默认值
				 */
		}
		if (!valid)
			report_corruption(ctx,
							  psprintf("toast value %u has invalid compression method id %d",
									   toast_pointer.va_valueid, cmid));
	}

	/* The tuple header better claim to contain toasted values
	 *
	 * 元组标头更好地声明包含经过烘烤的值
	 */
	if (!(infomask & HEAP_HASEXTERNAL))
	{
		report_corruption(ctx,
						  psprintf("toast value %u is external but tuple header flag HEAP_HASEXTERNAL not set",
								   toast_pointer.va_valueid));
		return true;
	}

	/* The relation better have a toast table
	 *
	 * 关系最好有一个敬酒桌
	 */
	if (!ctx->rel->rd_rel->reltoastrelid)
	{
		report_corruption(ctx,
						  psprintf("toast value %u is external but relation has no toast relation",
								   toast_pointer.va_valueid));
		return true;
	}

	/* If we were told to skip toast checking, then we're done.
	 *
	 * 如果我们被告知跳过 Toast 检查，那么我们就完成了。
	 */
	if (ctx->toast_rel == NULL)
		return true;

	/*
	 * If this tuple is eligible to be pruned, we cannot check the toast.
	 * Otherwise, we push a copy of the toast tuple so we can check it after
	 * releasing the main table buffer lock.
	 *
	 * 如果这个元组有资格被修剪，我们就无法检查 toast。否则，我们将推送 toast 元组的副本，以便在释放主表缓冲区锁后可以检查它。
	 */
	if (!ctx->tuple_could_be_pruned)
	{
		ToastedAttribute *ta;

		ta = (ToastedAttribute *) palloc0(sizeof(ToastedAttribute));

		VARATT_EXTERNAL_GET_POINTER(ta->toast_pointer, attr);
		ta->blkno = ctx->blkno;
		ta->offnum = ctx->offnum;
		ta->attnum = ctx->attnum;
		ctx->toasted_attributes = lappend(ctx->toasted_attributes, ta);
	}

	return true;
}

/*
 * For each attribute collected in ctx->toasted_attributes, look up the value
 * in the toast table and perform checks on it.  This function should only be
 * called on toast pointers which cannot be vacuumed away during our
 * processing.
 *
 * 对于 ctx->toasted_attributes 中收集的每个属性，在 toast 表中查找该值并对其进行检查。  这个函数应该只在 toast 指针上调用，在我们的处理过程中不能被清理掉。
 */
static void
check_toasted_attribute(HeapCheckContext *ctx, ToastedAttribute *ta)
{
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	bool		found_toasttup;
	HeapTuple	toasttup;
	uint32		extsize;
	int32		expected_chunk_seq = 0;
	int32		last_chunk_seq;

	extsize = VARATT_EXTERNAL_GET_EXTSIZE(ta->toast_pointer);
	last_chunk_seq = (extsize - 1) / TOAST_MAX_CHUNK_SIZE;

	/*
	 * Setup a scan key to find chunks in toast table with matching va_valueid
	 *
	 * 设置扫描键以在 toast 表中查找具有匹配 va_valueid 的块
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ta->toast_pointer.va_valueid));

	/*
	 * Check if any chunks for this toasted object exist in the toast table,
	 * accessible via the index.
	 *
	 * 检查 toast 表中是否存在此 toast 对象的任何块，可通过索引访问。
	 */
	toastscan = systable_beginscan_ordered(ctx->toast_rel,
										   ctx->valid_toast_index,
										   get_toast_snapshot(), 1,
										   &toastkey);
	found_toasttup = false;
	while ((toasttup =
			systable_getnext_ordered(toastscan,
									 ForwardScanDirection)) != NULL)
	{
		found_toasttup = true;
		check_toast_tuple(toasttup, ctx, ta, &expected_chunk_seq, extsize);
	}
	systable_endscan_ordered(toastscan);

	if (!found_toasttup)
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u not found in toast table",
										 ta->toast_pointer.va_valueid));
	else if (expected_chunk_seq <= last_chunk_seq)
		report_toast_corruption(ctx, ta,
								psprintf("toast value %u was expected to end at chunk %d, but ended while expecting chunk %d",
										 ta->toast_pointer.va_valueid,
										 last_chunk_seq, expected_chunk_seq));
}

/*
 * Check the current tuple as tracked in ctx, recording any corruption found in
 * ctx->tupstore.
 *
 * 检查 ctx 中跟踪的当前元组，记录 ctx->tupstore 中发现的任何损坏。
 *
 * We return some information about the status of xmin to aid in validating
 * update chains.
 *
 * 我们返回一些有关 xmin 状态的信息以帮助验证更新链。
 */
static void
check_tuple(HeapCheckContext *ctx, bool *xmin_commit_status_ok,
			XidCommitStatus *xmin_commit_status)
{
	/*
	 * Check various forms of tuple header corruption, and if the header is
	 * too corrupt, do not continue with other checks.
	 *
	 * 检查各种形式的元组标头损坏，如果标头损坏太多，则不要继续进行其他检查。
	 */
	if (!check_tuple_header(ctx))
		return;

	/*
	 * Check tuple visibility.  If the inserting transaction aborted, we
	 * cannot assume our relation description matches the tuple structure, and
	 * therefore cannot check it.
	 *
	 * 检查元组的可见性。  如果插入事务中止，我们不能假设我们的关系描述与元组结构匹配，因此无法检查它。
	 */
	if (!check_tuple_visibility(ctx, xmin_commit_status_ok,
								xmin_commit_status))
		return;

	/*
	 * The tuple is visible, so it must be compatible with the current version
	 * of the relation descriptor. It might have fewer columns than are
	 * present in the relation descriptor, but it cannot have more.
	 *
	 * 元组是可见的，因此它必须与关系描述符的当前版本兼容。它的列数可能少于关系描述符中的列数，但不能有更多。
	 */
	if (RelationGetDescr(ctx->rel)->natts < ctx->natts)
	{
		report_corruption(ctx,
						  psprintf("number of attributes %u exceeds maximum expected for table %u",
								   ctx->natts,
								   RelationGetDescr(ctx->rel)->natts));
		return;
	}

	/*
	 * Check each attribute unless we hit corruption that confuses what to do
	 * next, at which point we abort further attribute checks for this tuple.
	 * Note that we don't abort for all types of corruption, only for those
	 * types where we don't know how to continue.  We also don't abort the
	 * checking of toasted attributes collected from the tuple prior to
	 * aborting.  Those will still be checked later along with other toasted
	 * attributes collected from the page.
	 *
	 * 检查每个属性，除非我们遇到损坏而混淆了下一步该做什么，此时我们将中止对该元组的进一步属性检查。请注意，我们不会中止所有类型的损坏，只会中止那些我们不知道如何继续的类型。  我们也不会在中止之前中止对从元组收集的 toasted 属性的检查。  稍后仍将检查这些属性以及从页面收集的其他烘烤属性。
	 */
	ctx->offset = 0;
	for (ctx->attnum = 0; ctx->attnum < ctx->natts; ctx->attnum++)
		if (!check_tuple_attribute(ctx))
			break;				/* cannot continue */

	/* revert attnum to -1 until we again examine individual attributes
	 *
	 * 将 attnum 恢复为 -1 直到我们再次检查各个属性
	 */
	ctx->attnum = -1;
}

/*
 * Convert a TransactionId into a FullTransactionId using our cached values of
 * the valid transaction ID range.  It is the caller's responsibility to have
 * already updated the cached values, if necessary.  This is akin to
 * FullTransactionIdFromAllowableAt(), but it tolerates corruption in the form
 * of an xid before epoch 0.
 *
 * 使用有效事务 ID 范围的缓存值将 TransactionId 转换为 FullTransactionId。  如有必要，调用者有责任更新缓存的值。  这类似于 FullTransactionIdFromAllowableAt()，但它容忍 epoch 0 之前 xid 形式的损坏。
 */
static FullTransactionId
FullTransactionIdFromXidAndCtx(TransactionId xid, const HeapCheckContext *ctx)
{
	uint64		nextfxid_i;
	int32		diff;
	FullTransactionId fxid;

	Assert(TransactionIdIsNormal(ctx->next_xid));
	Assert(FullTransactionIdIsNormal(ctx->next_fxid));
	Assert(XidFromFullTransactionId(ctx->next_fxid) == ctx->next_xid);

	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);

	nextfxid_i = U64FromFullTransactionId(ctx->next_fxid);

	/* compute the 32bit modulo difference
	 *
	 * 计算 32 位模差
	 */
	diff = (int32) (ctx->next_xid - xid);

	/*
	 * In cases of corruption we might see a 32bit xid that is before epoch 0.
	 * We can't represent that as a 64bit xid, due to 64bit xids being
	 * unsigned integers, without the modulo arithmetic of 32bit xid. There's
	 * no really nice way to deal with that, but it works ok enough to use
	 * FirstNormalFullTransactionId in that case, as a freshly initdb'd
	 * cluster already has a newer horizon.
	 *
	 * 在损坏的情况下，我们可能会看到 epoch 0 之前的 32 位 xid。我们无法将其表示为 64 位 xid，因为 64 位 xid 是无符号整数，没有 32 位 xid 的模算术。没有真正好的方法来处理这个问题，但在这种情况下使用 FirstNormalFullTransactionId 就足够了，因为新初始化的集群已经有了更新的视野。
	 */
	if (diff > 0 && (nextfxid_i - FirstNormalTransactionId) < (int64) diff)
	{
		Assert(EpochFromFullTransactionId(ctx->next_fxid) == 0);
		fxid = FirstNormalFullTransactionId;
	}
	else
		fxid = FullTransactionIdFromU64(nextfxid_i - diff);

	Assert(FullTransactionIdIsNormal(fxid));
	return fxid;
}

/*
 * Update our cached range of valid transaction IDs.
 *
 * 更新我们缓存的有效交易 ID 范围。
 */
static void
update_cached_xid_range(HeapCheckContext *ctx)
{
	/* Make cached copies
	 *
	 * 制作缓存副本
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	ctx->next_fxid = TransamVariables->nextXid;
	ctx->oldest_xid = TransamVariables->oldestXid;
	LWLockRelease(XidGenLock);

	/* And compute alternate versions of the same
	 *
	 * 并计算相同的替代版本
	 */
	ctx->next_xid = XidFromFullTransactionId(ctx->next_fxid);
	ctx->oldest_fxid = FullTransactionIdFromXidAndCtx(ctx->oldest_xid, ctx);
}

/*
 * Update our cached range of valid multitransaction IDs.
 *
 * 更新我们缓存的有效多事务 ID 范围。
 */
static void
update_cached_mxid_range(HeapCheckContext *ctx)
{
	ReadMultiXactIdRange(&ctx->oldest_mxact, &ctx->next_mxact);
}

/*
 * Return whether the given FullTransactionId is within our cached valid
 * transaction ID range.
 *
 * 返回给定的 FullTransactionId 是否在我们缓存的有效交易 ID 范围内。
 */
static inline bool
fxid_in_cached_range(FullTransactionId fxid, const HeapCheckContext *ctx)
{
	return (FullTransactionIdPrecedesOrEquals(ctx->oldest_fxid, fxid) &&
			FullTransactionIdPrecedes(fxid, ctx->next_fxid));
}

/*
 * Checks whether a multitransaction ID is in the cached valid range, returning
 * the nature of the range violation, if any.
 *
 * 检查多事务 ID 是否在缓存的有效范围内，如果有，则返回范围违规的性质。
 */
static XidBoundsViolation
check_mxid_in_range(MultiXactId mxid, HeapCheckContext *ctx)
{
	if (!TransactionIdIsValid(mxid))
		return XID_INVALID;
	if (MultiXactIdPrecedes(mxid, ctx->relminmxid))
		return XID_PRECEDES_RELMIN;
	if (MultiXactIdPrecedes(mxid, ctx->oldest_mxact))
		return XID_PRECEDES_CLUSTERMIN;
	if (MultiXactIdPrecedesOrEquals(ctx->next_mxact, mxid))
		return XID_IN_FUTURE;
	return XID_BOUNDS_OK;
}

/*
 * Checks whether the given mxid is valid to appear in the heap being checked,
 * returning the nature of the range violation, if any.
 *
 * 检查给定的 mxid 是否有效出现在正在检查的堆中，返回范围违规的性质（如果有）。
 *
 * This function attempts to return quickly by caching the known valid mxid
 * range in ctx.  Callers should already have performed the initial setup of
 * the cache prior to the first call to this function.
 *
 * 该函数尝试通过在 ctx 中缓存已知的有效 mxid 范围来快速返回。  在第一次调用此函数之前，调用者应该已经执行了缓存的初始设置。
 */
static XidBoundsViolation
check_mxid_valid_in_rel(MultiXactId mxid, HeapCheckContext *ctx)
{
	XidBoundsViolation result;

	result = check_mxid_in_range(mxid, ctx);
	if (result == XID_BOUNDS_OK)
		return XID_BOUNDS_OK;

	/* The range may have advanced.  Recheck.
	 *
	 * 范围可能有所扩大。  重新检查。
	 */
	update_cached_mxid_range(ctx);
	return check_mxid_in_range(mxid, ctx);
}

/*
 * Checks whether the given transaction ID is (or was recently) valid to appear
 * in the heap being checked, or whether it is too old or too new to appear in
 * the relation, returning information about the nature of the bounds violation.
 *
 * 检查给定的事务 ID 是否（或最近）有效地出现在正在检查的堆中，或者它是否太旧或太新而无法出现在关系中，返回有关边界违规性质的信息。
 *
 * We cache the range of valid transaction IDs.  If xid is in that range, we
 * conclude that it is valid, even though concurrent changes to the table might
 * invalidate it under certain corrupt conditions.  (For example, if the table
 * contains corrupt all-frozen bits, a concurrent vacuum might skip the page(s)
 * containing the xid and then truncate clog and advance the relfrozenxid
 * beyond xid.) Reporting the xid as valid under such conditions seems
 * acceptable, since if we had checked it earlier in our scan it would have
 * truly been valid at that time.
 *
 * 我们缓存有效交易 ID 的范围。  如果 xid 在该范围内，我们就得出结论它是有效的，即使对表的并发更改可能在某些损坏条件下使其无效。  （例如，如果表包含损坏的全冻结位，则并发真空可能会跳过包含 xid 的页面，然后截断堵塞并将 relfrozenxid 推进到 xid 之外。）在这种情况下将 xid 报告为有效似乎是可以接受的，因为如果我们在扫描中早些时候检查过它，那么它当时确实是有效的。
 *
 * If the status argument is not NULL, and if and only if the transaction ID
 * appears to be valid in this relation, the status argument will be set with
 * the commit status of the transaction ID.
 *
 * 如果状态参数不为 NULL，并且当且仅当事务 ID 在该关系中显得有效时，状态参数将设置为事务 ID 的提交状态。
 */
static XidBoundsViolation
get_xid_status(TransactionId xid, HeapCheckContext *ctx,
			   XidCommitStatus *status)
{
	FullTransactionId fxid;
	FullTransactionId clog_horizon;

	/* Quick check for special xids
	 *
	 * 快速检查特殊 xids
	 */
	if (!TransactionIdIsValid(xid))
		return XID_INVALID;
	else if (xid == BootstrapTransactionId || xid == FrozenTransactionId)
	{
		if (status != NULL)
			*status = XID_COMMITTED;
		return XID_BOUNDS_OK;
	}

	/* Check if the xid is within bounds
	 *
	 * 检查 xid 是否在范围内
	 */
	fxid = FullTransactionIdFromXidAndCtx(xid, ctx);
	if (!fxid_in_cached_range(fxid, ctx))
	{
		/*
		 * We may have been checking against stale values.  Update the cached
		 * range to be sure, and since we relied on the cached range when we
		 * performed the full xid conversion, reconvert.
		 *
		 * 我们可能一直在检查过时的值。  确保更新缓存范围，并且由于我们在执行完整 xid 转换时依赖于缓存范围，因此请重新转换。
		 */
		update_cached_xid_range(ctx);
		fxid = FullTransactionIdFromXidAndCtx(xid, ctx);
	}

	if (FullTransactionIdPrecedesOrEquals(ctx->next_fxid, fxid))
		return XID_IN_FUTURE;
	if (FullTransactionIdPrecedes(fxid, ctx->oldest_fxid))
		return XID_PRECEDES_CLUSTERMIN;
	if (FullTransactionIdPrecedes(fxid, ctx->relfrozenfxid))
		return XID_PRECEDES_RELMIN;

	/* Early return if the caller does not request clog checking
	 *
	 * 如果调用者不请求阻塞检查，则提前返回
	 */
	if (status == NULL)
		return XID_BOUNDS_OK;

	/* Early return if we just checked this xid in a prior call
	 *
	 * 如果我们刚刚在之前的调用中检查了此 xid，则提前返回
	 */
	if (xid == ctx->cached_xid)
	{
		*status = ctx->cached_status;
		return XID_BOUNDS_OK;
	}

	*status = XID_COMMITTED;
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	clog_horizon =
		FullTransactionIdFromXidAndCtx(TransamVariables->oldestClogXid,
									   ctx);
	if (FullTransactionIdPrecedesOrEquals(clog_horizon, fxid))
	{
		if (TransactionIdIsCurrentTransactionId(xid))
			*status = XID_IS_CURRENT_XID;
		else if (TransactionIdIsInProgress(xid))
			*status = XID_IN_PROGRESS;
		else if (TransactionIdDidCommit(xid))
			*status = XID_COMMITTED;
		else
			*status = XID_ABORTED;
	}
	LWLockRelease(XactTruncationLock);
	ctx->cached_xid = xid;
	ctx->cached_status = *status;
	return XID_BOUNDS_OK;
}
