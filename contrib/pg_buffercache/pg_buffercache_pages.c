/*-------------------------------------------------------------------------
 *
 * pg_buffercache_pages.c
 *	  display some contents of the buffer cache
 *	  显示缓冲区缓存的一些内容
 *
 * 实现核心流程概述：
 * 本模块（pg_buffercache）是一个扩展，允许从 SQL 界面实时检视 PostgreSQL 的共享缓冲区（Shared Buffer Cache）状态。
 *
 * 核心流程如下：
 * 1. 缓冲区扫描：遍历整个 NBuffers 数组，这是系统共享内存中存储缓冲区描述符的核心区域。
 * 2. 状态获取：
 *    - 对于详细视图，通过 LockBufHdr 获取每个缓冲区的实时状态（buf_state）。
 *    - 对于摘要视图，通过原子方式读取状态以减少加锁开销。
 * 3. 属性映射：提取缓冲区关联的 RelFileNumber、表空间 OID、数据库 OID、分叉号（ForkNum）、块号（BlockNum）以及脏页标记和使用计数。
 * 4. NUMA 检视：利用特定平台的 NUMA 查询接口，映射数据库缓冲区到操作系统的物理内存节点分布。
 * 5. 结果返回：利用 SRF（Set Returning Functions）机制，将内存快照以单行元组的形式逐步返回给 SQL 层。
 * 6. 维护工具：提供手动淘汰（Evict）缓冲区的接口，便于进行冷启动性能分析或诊断特定页。
 *
 *	  contrib/pg_buffercache/pg_buffercache_pages.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "port/pg_numa.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


#define NUM_BUFFERCACHE_PAGES_MIN_ELEM	8
#define NUM_BUFFERCACHE_PAGES_ELEM	9
#define NUM_BUFFERCACHE_SUMMARY_ELEM 5
#define NUM_BUFFERCACHE_USAGE_COUNTS_ELEM 4
#define NUM_BUFFERCACHE_EVICT_ELEM 2
#define NUM_BUFFERCACHE_EVICT_RELATION_ELEM 3
#define NUM_BUFFERCACHE_EVICT_ALL_ELEM 3

#define NUM_BUFFERCACHE_NUMA_ELEM	3

PG_MODULE_MAGIC_EXT(
					.name = "pg_buffercache",
					.version = PG_VERSION
);

/*
 * =========================================================================
 * 1. 数据结构定义
 * 定义用于在 SQL 调用之间保持快照数据和映射结果的内部结构。
 * =========================================================================
 */

/*
 * Record structure holding the to be exposed cache data.
 * 保存待公开的缓存数据的记录结构。
 */
typedef struct
{
	uint32		bufferid;
	RelFileNumber relfilenumber;
	Oid			reltablespace;
	Oid			reldatabase;
	ForkNumber	forknum;
	BlockNumber blocknum;
	bool		isvalid;
	bool		isdirty;
	uint16		usagecount;

	/*
	 * An int32 is sufficiently large, as MAX_BACKENDS prevents a buffer from
	 * being pinned by too many backends and each backend will only pin once
	 * because of bufmgr.c's PrivateRefCount infrastructure.
	 * int32 足够大，因为 MAX_BACKENDS 防止了一个缓冲区被过多的后台进程固定（pin），
	 * 并且由于 bufmgr.c 的 PrivateRefCount 基础架构，每个后台进程只会固定一次。
	 */
	int32		pinning_backends;
} BufferCachePagesRec;


/*
 * Function context for data persisting over repeated calls.
 * 跨多次调用持久化数据的函数上下文。
 */
typedef struct
{
	TupleDesc	tupdesc;
	BufferCachePagesRec *record;
} BufferCachePagesContext;

/*
 * Record structure holding the to be exposed cache data.
 * 保存待公开的缓存数据的记录结构。
 */
typedef struct
{
	uint32		bufferid;
	int64		page_num;
	int32		numa_node;
} BufferCacheNumaRec;

/*
 * Function context for data persisting over repeated calls.
 * 跨多次调用持久化数据的函数上下文。
 */
typedef struct
{
	TupleDesc	tupdesc;
	int			buffers_per_page;
	int			pages_per_buffer;
	int			os_page_size;
	BufferCacheNumaRec *record;
} BufferCacheNumaContext;


/*
 * =========================================================================
 * 2. 导出函数注册与初始化
 * =========================================================================
 */

/*
 * Function returning data from the shared buffer cache - buffer number,
 * relation node/tablespace/database/blocknum and dirty indicator.
 * 返回共享缓冲区数据的功能 —— 缓冲区号、关系节点/表空间/数据库/块号和脏页指标。
 */
PG_FUNCTION_INFO_V1(pg_buffercache_pages);
PG_FUNCTION_INFO_V1(pg_buffercache_numa_pages);
PG_FUNCTION_INFO_V1(pg_buffercache_summary);
PG_FUNCTION_INFO_V1(pg_buffercache_usage_counts);
PG_FUNCTION_INFO_V1(pg_buffercache_evict);
PG_FUNCTION_INFO_V1(pg_buffercache_evict_relation);
PG_FUNCTION_INFO_V1(pg_buffercache_evict_all);


/* Only need to touch memory once per backend process lifetime
 * 在每个后台进程生命周期内只需触碰（touch）一次内存 */
static bool firstNumaTouch = true;


/*
 * =========================================================================
 * 3. 缓冲区详细快照接口 (pg_buffercache_pages)
 * =========================================================================
 */

Datum
pg_buffercache_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Datum		result;
	MemoryContext oldcontext;
	BufferCachePagesContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;

	if (SRF_IS_FIRSTCALL())
	{
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls
		 * 在分配用于后续调用的内容时切换上下文 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence
		 * 创建用于跨调用持久化的用户函数上下文 */
		fctx = (BufferCachePagesContext *) palloc(sizeof(BufferCachePagesContext));

		/*
		 * To smoothly support upgrades from version 1.0 of this extension
		 * transparently handle the (non-)existence of the pinning_backends
		 * column. We unfortunately have to get the result type for that... -
		 * we can't use the result type determined by the function definition
		 * without potentially crashing when somebody uses the old (or even
		 * wrong) function definition though.
		 * 为了平滑支持此扩展从 1.0 版本的升级，透明地处理 pinning_backends 列的存在与否。
		 * 不幸的是，我们必须为此获取结果类型... 因为如果不这样做，
		 * 当有人使用旧的（甚至错误的）函数定义时，我们不能直接使用由函数定义确定的结果类型，否则可能会崩溃。
		 */
		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts < NUM_BUFFERCACHE_PAGES_MIN_ELEM ||
			expected_tupledesc->natts > NUM_BUFFERCACHE_PAGES_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows.
		 * 为结果行构造元组描述符。 */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "relfilenode",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "reltablespace",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 4, "reldatabase",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 5, "relforknumber",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 6, "relblocknumber",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 7, "isdirty",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 8, "usage_count",
						   INT2OID, -1, 0);

		if (expected_tupledesc->natts == NUM_BUFFERCACHE_PAGES_ELEM)
			TupleDescInitEntry(tupledesc, (AttrNumber) 9, "pinning_backends",
							   INT4OID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/* Allocate NBuffers worth of BufferCachePagesRec records.
		 * 分配价值 NBuffers 个 BufferCachePagesRec 的记录空间。 */
		fctx->record = (BufferCachePagesRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(BufferCachePagesRec) * NBuffers);

		/* Set max calls and remember the user function context.
		 * 设置最大通话次数并记录用户功能上下文。 */
		funcctx->max_calls = NBuffers;
		funcctx->user_fctx = fctx;

		/* Return to original context when allocating transient memory
		 * 在分配瞬态内存时返回到原始上下文 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 * 扫描所有缓冲区，将相关字段保存在 fctx->record 结构中。
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 * 我们不持有分区锁，因此无法在所有缓冲区之间获得一致的快照，但我们会抓取缓冲区头锁，因此每个缓冲区的信息是自洽的。
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *bufHdr;
			uint32		buf_state;

			CHECK_FOR_INTERRUPTS();

			bufHdr = GetBufferDescriptor(i);
			/* Lock each buffer header before inspecting.
			 * 在检查前锁定每个缓冲区头。 */
			buf_state = LockBufHdr(bufHdr);

			fctx->record[i].bufferid = BufferDescriptorGetBuffer(bufHdr);
			fctx->record[i].relfilenumber = BufTagGetRelNumber(&bufHdr->tag);
			fctx->record[i].reltablespace = bufHdr->tag.spcOid;
			fctx->record[i].reldatabase = bufHdr->tag.dbOid;
			fctx->record[i].forknum = BufTagGetForkNum(&bufHdr->tag);
			fctx->record[i].blocknum = bufHdr->tag.blockNum;
			fctx->record[i].usagecount = BUF_STATE_GET_USAGECOUNT(buf_state);
			fctx->record[i].pinning_backends = BUF_STATE_GET_REFCOUNT(buf_state);

			if (buf_state & BM_DIRTY)
				fctx->record[i].isdirty = true;
			else
				fctx->record[i].isdirty = false;

			/* Note if the buffer is valid, and has storage created
			 * 记录缓冲区是否有效，并且是否已创建存储空间 */
			if ((buf_state & BM_VALID) && (buf_state & BM_TAG_VALID))
				fctx->record[i].isvalid = true;
			else
				fctx->record[i].isvalid = false;

			UnlockBufHdr(bufHdr, buf_state);
		}
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state
	 * 获取保存的状态 */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_BUFFERCACHE_PAGES_ELEM];
		bool		nulls[NUM_BUFFERCACHE_PAGES_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		nulls[0] = false;

		/*
		 * Set all fields except the bufferid to null if the buffer is unused
		 * or not valid.
		 * 如果缓冲区未使用或无效，则将除 bufferid 之外的所有字段都设置为 null。
		 */
		if (fctx->record[i].blocknum == InvalidBlockNumber ||
			fctx->record[i].isvalid == false)
		{
			nulls[1] = true;
			nulls[2] = true;
			nulls[3] = true;
			nulls[4] = true;
			nulls[5] = true;
			nulls[6] = true;
			nulls[7] = true;
			/* unused for v1.0 callers, but the array is always long enough
			 * 对于 v1.0 调用者未使用，但数组长度始终足够 */
			nulls[8] = true;
		}
		else
		{
			values[1] = ObjectIdGetDatum(fctx->record[i].relfilenumber);
			nulls[1] = false;
			values[2] = ObjectIdGetDatum(fctx->record[i].reltablespace);
			nulls[2] = false;
			values[3] = ObjectIdGetDatum(fctx->record[i].reldatabase);
			nulls[3] = false;
			values[4] = ObjectIdGetDatum(fctx->record[i].forknum);
			nulls[4] = false;
			values[5] = Int64GetDatum((int64) fctx->record[i].blocknum);
			nulls[5] = false;
			values[6] = BoolGetDatum(fctx->record[i].isdirty);
			nulls[6] = false;
			values[7] = Int16GetDatum(fctx->record[i].usagecount);
			nulls[7] = false;
			/* unused for v1.0 callers, but the array is always long enough
			 * 对于 v1.0 调用者未使用，但数组长度始终足够 */
			values[8] = Int32GetDatum(fctx->record[i].pinning_backends);
			nulls[8] = false;
		}

		/* Build and return the tuple.
		 * 构造并返回元组。 */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

/*
 * Inquire about NUMA memory mappings for shared buffers.
 * 查询共享缓冲区的 NUMA 内存映射。
 *
 * Returns NUMA node ID for each memory page used by the buffer. Buffers may
 * be smaller or larger than OS memory pages. For each buffer we return one
 * entry for each memory page used by the buffer (if the buffer is smaller,
 * it only uses a part of one memory page).
 * 返回缓冲区使用的每个内存页的 NUMA 节点 ID。缓冲区可能比操作系统内存页更小或更大。
 * 对于每个缓冲区，其使用的每个内存页我们都返回一个条目（如果缓冲区较小，它仅使用一个内存页的一部分）。
 *
 * We expect both sizes (for buffers and memory pages) to be a power-of-2, so
 * one is always a multiple of the other.
 * 我们期望这两个大小（缓冲区和内存页）都是 2 的幂，因此一个总是另一个的倍数。
 *
 * In order to get reliable results we also need to touch memory pages, so
 * that the inquiry about NUMA memory node doesn't return -2 (which indicates
 * unmapped/unallocated pages).
 * 为了获得可靠的结果，我们还需要触碰内存页，以便关于 NUMA 内存节点的查询不会返回 -2（表示未映射/未分配的页）。
 */
Datum
pg_buffercache_numa_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	MemoryContext oldcontext;
	BufferCacheNumaContext *fctx;	/* User function context. */
	TupleDesc	tupledesc;
	TupleDesc	expected_tupledesc;
	HeapTuple	tuple;
	Datum		result;

	if (SRF_IS_FIRSTCALL())
	{
		int			i,
					idx;
		Size		os_page_size;
		void	  **os_page_ptrs;
		int		   *os_page_status;
		uint64		os_page_count;
		int			pages_per_buffer;
		int			max_entries;
		char	   *startptr,
				   *endptr;

		if (pg_numa_init() == -1)
			elog(ERROR, "libnuma initialization failed or NUMA is not supported on this platform");

		/*
		 * The database block size and OS memory page size are unlikely to be
		 * the same. The block size is 1-32KB, the memory page size depends on
		 * platform. On x86 it's usually 4KB, on ARM it's 4KB or 64KB, but
		 * there are also features like THP etc. Moreover, we don't quite know
		 * how the pages and buffers "align" in memory - the buffers may be
		 * shifted in some way, using more memory pages than necessary.
		 * 数据库块大小和操作系统内存页大小不太可能相同。块大小为 1-32KB，内存页大小取决于平台。
		 * 在 x86 上通常是 4KB，在 ARM 上是 4KB 或 64KB，但也有 THP 等特性。
		 * 此外，我们不太清楚页面和缓冲区在内存中是如何“对齐”的 —— 缓冲区可能会以某种方式偏移，
		 * 使用比必要更多的内存页。
		 *
		 * So we need to be careful about mapping buffers to memory pages. We
		 * calculate the maximum number of pages a buffer might use, so that
		 * we allocate enough space for the entries. And then we count the
		 * actual number of entries as we scan the buffers.
		 * 因此我们需要小心地将缓冲区映射到内存页。我们计算一个缓冲区可能使用的最大页数，
		 * 以便为这些条目分配足够的空间。然后在扫描缓冲区时计算实际的条目数量。
		 *
		 * This information is needed before calling move_pages() for NUMA
		 * node id inquiry.
		 * 在调用 move_pages() 进行 NUMA 节点 ID 查询之前，需要此信息。
		 */
		os_page_size = pg_get_shmem_pagesize();

		/*
		 * The pages and block size is expected to be 2^k, so one divides the
		 * other (we don't know in which direction). This does not say
		 * anything about relative alignment of pages/buffers.
		 * 页面和块大小预期为 2^k，因此一个可以整除另一个（我们不知道方向）。
		 * 这没有说明任何关于页面/缓冲区的相对对齐信息。
		 */
		Assert((os_page_size % BLCKSZ == 0) || (BLCKSZ % os_page_size == 0));

		/*
		 * How many addresses we are going to query? Simply get the page for
		 * the first buffer, and first page after the last buffer, and count
		 * the pages from that.
		 * 我们要查询多少个地址？简单地获取第一个缓冲区的页面，以及最后一个缓冲区之后的第一个页面，
		 * 并据此计算页面数量。
		 */
		startptr = (char *) TYPEALIGN_DOWN(os_page_size,
										   BufferGetBlock(1));
		endptr = (char *) TYPEALIGN(os_page_size,
									(char *) BufferGetBlock(NBuffers) + BLCKSZ);
		os_page_count = (endptr - startptr) / os_page_size;

		/* Used to determine the NUMA node for all OS pages at once
		 * 用于一次性确定所有操作系统页面的 NUMA 节点 */
		os_page_ptrs = palloc0(sizeof(void *) * os_page_count);
		os_page_status = palloc(sizeof(int) * os_page_count);

		/* Fill pointers for all the memory pages.
		 * 填充所有内存页面的指针。 */
		idx = 0;
		for (char *ptr = startptr; ptr < endptr; ptr += os_page_size)
		{
			os_page_ptrs[idx++] = ptr;

			/* Only need to touch memory once per backend process lifetime
			 * 每个后台进程生命周期只需触碰一次内存 */
			if (firstNumaTouch)
				pg_numa_touch_mem_if_required(ptr);
		}

		Assert(idx == os_page_count);

		elog(DEBUG1, "NUMA: NBuffers=%d os_page_count=" UINT64_FORMAT " "
			 "os_page_size=%zu", NBuffers, os_page_count, os_page_size);

		/*
		 * If we ever get 0xff back from kernel inquiry, then we probably have
		 * bug in our buffers to OS page mapping code here.
		 * 如果我们从内核查询中得到 0xff，那么这里的缓冲区到操作系统页面映射代码可能存在 bug。
		 */
		memset(os_page_status, 0xff, sizeof(int) * os_page_count);

		/* Query NUMA status for all the pointers
		 * 查询所有指针的 NUMA 状态 */
		if (pg_numa_query_pages(0, os_page_count, os_page_ptrs, os_page_status) == -1)
			elog(ERROR, "failed NUMA pages inquiry: %m");

		/* Initialize the multi-call context, load entries about buffers
		 * 初始化多调用上下文，加载关于缓冲区的条目 */

		funcctx = SRF_FIRSTCALL_INIT();

		/* Switch context when allocating stuff to be used in later calls
		 * 分配后续调用的内容时切换上下文 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence
		 * 创建用于跨调用持久化的用户函数上下文 */
		fctx = (BufferCacheNumaContext *) palloc(sizeof(BufferCacheNumaContext));

		if (get_call_result_type(fcinfo, NULL, &expected_tupledesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		if (expected_tupledesc->natts != NUM_BUFFERCACHE_NUMA_ELEM)
			elog(ERROR, "incorrect number of output arguments");

		/* Construct a tuple descriptor for the result rows.
		 * 为结果行构造元组描述符。 */
		tupledesc = CreateTemplateTupleDesc(expected_tupledesc->natts);
		TupleDescInitEntry(tupledesc, (AttrNumber) 1, "bufferid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 2, "os_page_num",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupledesc, (AttrNumber) 3, "numa_node",
						   INT4OID, -1, 0);

		fctx->tupdesc = BlessTupleDesc(tupledesc);

		/*
		 * Each buffer needs at least one entry, but it might be offset in
		 * some way, and use one extra entry. So we allocate space for the
		 * maximum number of entries we might need, and then count the exact
		 * number as we're walking buffers. That way we can do it in one pass,
		 * without reallocating memory.
		 * 每个缓冲区至少需要一个条目，但它可能会以某种方式偏移并额外使用一个条目。
		 * 因此，我们为可能需要的最大条目数分配空间，然后在遍历缓冲区时计算准确的数量。
		 * 这样我们就可以在一次遍历中完成，无需重新分配内存。
		 */
		pages_per_buffer = Max(1, BLCKSZ / os_page_size) + 1;
		max_entries = NBuffers * pages_per_buffer;

		/* Allocate entries for BufferCachePagesRec records.
		 * 为 BufferCachePagesRec 记录分配条目。 */
		fctx->record = (BufferCacheNumaRec *)
			MemoryContextAllocHuge(CurrentMemoryContext,
								   sizeof(BufferCacheNumaRec) * max_entries);

		/* Return to original context when allocating transient memory
		 * 分配瞬态内存时返回到原始上下文 */
		MemoryContextSwitchTo(oldcontext);

		if (firstNumaTouch)
			elog(DEBUG1, "NUMA: page-faulting the buffercache for proper NUMA readouts");

		/*
		 * Scan through all the buffers, saving the relevant fields in the
		 * fctx->record structure.
		 * 扫描所有缓冲区，将相关字段保存在 fctx->record 结构中。
		 *
		 * We don't hold the partition locks, so we don't get a consistent
		 * snapshot across all buffers, but we do grab the buffer header
		 * locks, so the information of each buffer is self-consistent.
		 * 我们不持有分区锁，因此无法在所有缓冲区之间获得一致的快照，但我们确实抓取了缓冲区头锁，因此每个缓冲区的信息是自洽的。
		 *
		 * This loop touches and stores addresses into os_page_ptrs[] as input
		 * to one big move_pages(2) inquiry system call. Basically we ask for
		 * all memory pages for NBuffers.
		 * 此循环触碰并将地址存储到 os_page_ptrs[] 中，作为大型 move_pages(2) 查询系统调用的输入。
		 * 基本上我们请求 NBuffers 的所有内存页。
		 */
		startptr = (char *) TYPEALIGN_DOWN(os_page_size, (char *) BufferGetBlock(1));
		idx = 0;
		for (i = 0; i < NBuffers; i++)
		{
			char	   *buffptr = (char *) BufferGetBlock(i + 1);
			BufferDesc *bufHdr;
			uint32		buf_state;
			uint32		bufferid;
			int32		page_num;
			char	   *startptr_buff,
					   *endptr_buff;

			CHECK_FOR_INTERRUPTS();

			bufHdr = GetBufferDescriptor(i);

			/* Lock each buffer header before inspecting.
			 * 在检查前锁定每个缓冲区头。 */
			buf_state = LockBufHdr(bufHdr);
			bufferid = BufferDescriptorGetBuffer(bufHdr);
			UnlockBufHdr(bufHdr, buf_state);

			/* start of the first page of this buffer
			 * 此缓冲区第一页的开始 */
			startptr_buff = (char *) TYPEALIGN_DOWN(os_page_size, buffptr);

			/* end of the buffer (no need to align to memory page)
			 * 缓冲区的末尾（无需对齐到内存页） */
			endptr_buff = buffptr + BLCKSZ;

			Assert(startptr_buff < endptr_buff);

			/* calculate ID of the first page for this buffer
			 * 计算此缓冲区第一页的 ID */
			page_num = (startptr_buff - startptr) / os_page_size;

			/* Add an entry for each OS page overlapping with this buffer.
			 * 为与此缓冲区重叠的每个操作系统页面添加条目。 */
			for (char *ptr = startptr_buff; ptr < endptr_buff; ptr += os_page_size)
			{
				fctx->record[idx].bufferid = bufferid;
				fctx->record[idx].page_num = page_num;
				fctx->record[idx].numa_node = os_page_status[page_num];

				/* advance to the next entry/page
				 * 前进到下一个条目/页面 */
				++idx;
				++page_num;
			}
		}

		Assert((idx >= os_page_count) && (idx <= max_entries));

		/* Set max calls and remember the user function context.
		 * 设置最大通话次数并记录用户功能上下文。 */
		funcctx->max_calls = idx;
		funcctx->user_fctx = fctx;

		/* Remember this backend touched the pages
		 * 记住此后台进程已触及这些页面 */
		firstNumaTouch = false;
	}

	funcctx = SRF_PERCALL_SETUP();

	/* Get the saved state */
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		uint32		i = funcctx->call_cntr;
		Datum		values[NUM_BUFFERCACHE_NUMA_ELEM];
		bool		nulls[NUM_BUFFERCACHE_NUMA_ELEM];

		values[0] = Int32GetDatum(fctx->record[i].bufferid);
		nulls[0] = false;

		values[1] = Int64GetDatum(fctx->record[i].page_num);
		nulls[1] = false;

		/* status is valid node number
		 * 状态为有效的节点编号 */
		if (fctx->record[i].numa_node >= 0)
		{
			values[2] = Int32GetDatum(fctx->record[i].numa_node);
			nulls[2] = false;
		}
		else
		{
			/* some kind of error (e.g. pages moved to swap)
			 * 某种错误（例如页面被移至交换区） */
			values[2] = (Datum) 0;
			nulls[2] = true;
		}

		/* Build and return the tuple.
		 * 构造并返回元组。 */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		SRF_RETURN_DONE(funcctx);
}

/*
 * =========================================================================
 * 4. 缓冲区缓存摘要报告 (pg_buffercache_summary)
 * =========================================================================
 */

Datum
pg_buffercache_summary(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_SUMMARY_ELEM];
	bool		nulls[NUM_BUFFERCACHE_SUMMARY_ELEM];

	int32		buffers_used = 0;
	int32		buffers_unused = 0;
	int32		buffers_dirty = 0;
	int32		buffers_pinned = 0;
	int64		usagecount_total = 0;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr;
		uint32		buf_state;

		CHECK_FOR_INTERRUPTS();

		/*
		 * This function summarizes the state of all headers. Locking the
		 * buffer headers wouldn't provide an improved result as the state of
		 * the buffer can still change after we release the lock and it'd
		 * noticeably increase the cost of the function.
		 * 此函数总结所有头的状态。锁定缓冲区头不会提供更好的结果，因为在释放锁之后缓冲区的状态仍然可能发生变化，
		 * 且会显著增加该函数的成本。
		 */
		bufHdr = GetBufferDescriptor(i);
		buf_state = pg_atomic_read_u32(&bufHdr->state);

		if (buf_state & BM_VALID)
		{
			buffers_used++;
			usagecount_total += BUF_STATE_GET_USAGECOUNT(buf_state);

			if (buf_state & BM_DIRTY)
				buffers_dirty++;
		}
		else
			buffers_unused++;

		if (BUF_STATE_GET_REFCOUNT(buf_state) > 0)
			buffers_pinned++;
	}

	memset(nulls, 0, sizeof(nulls));
	values[0] = Int32GetDatum(buffers_used);
	values[1] = Int32GetDatum(buffers_unused);
	values[2] = Int32GetDatum(buffers_dirty);
	values[3] = Int32GetDatum(buffers_pinned);

	if (buffers_used != 0)
		values[4] = Float8GetDatum((double) usagecount_total / buffers_used);
	else
		nulls[4] = true;

	/* Build and return the tuple.
	 * 构造并返回元组。 */
	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * =========================================================================
 * 5. 使用计数统计报告 (pg_buffercache_usage_counts)
 * =========================================================================
 */

Datum
pg_buffercache_usage_counts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			usage_counts[BM_MAX_USAGE_COUNT + 1] = {0};
	int			dirty[BM_MAX_USAGE_COUNT + 1] = {0};
	int			pinned[BM_MAX_USAGE_COUNT + 1] = {0};
	Datum		values[NUM_BUFFERCACHE_USAGE_COUNTS_ELEM];
	bool		nulls[NUM_BUFFERCACHE_USAGE_COUNTS_ELEM] = {0};

	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < NBuffers; i++)
	{
		BufferDesc *bufHdr = GetBufferDescriptor(i);
		uint32		buf_state = pg_atomic_read_u32(&bufHdr->state);
		int			usage_count;

		CHECK_FOR_INTERRUPTS();

		usage_count = BUF_STATE_GET_USAGECOUNT(buf_state);
		usage_counts[usage_count]++;

		if (buf_state & BM_DIRTY)
			dirty[usage_count]++;

		if (BUF_STATE_GET_REFCOUNT(buf_state) > 0)
			pinned[usage_count]++;
	}

	for (int i = 0; i < BM_MAX_USAGE_COUNT + 1; i++)
	{
		values[0] = Int32GetDatum(i);
		values[1] = Int32GetDatum(usage_counts[i]);
		values[2] = Int32GetDatum(dirty[i]);
		values[3] = Int32GetDatum(pinned[i]);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * =========================================================================
 * 6. 缓冲区维护与淘汰接口 (Eviction Tools)
 * =========================================================================
 */

/*
 * Helper function to check if the user has superuser privileges.
 * 检查用户是否具有超级用户权限的辅助函数。
 */
static void
pg_buffercache_superuser_check(char *func_name)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use %s()",
						func_name)));
}

/*
 * Try to evict a shared buffer.
 * 尝试淘汰一个共享缓冲区。
 */
Datum
pg_buffercache_evict(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_EVICT_ELEM];
	bool		nulls[NUM_BUFFERCACHE_EVICT_ELEM] = {0};

	Buffer		buf = PG_GETARG_INT32(0);
	bool		buffer_flushed;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pg_buffercache_superuser_check("pg_buffercache_evict");

	if (buf < 1 || buf > NBuffers)
		elog(ERROR, "bad buffer ID: %d", buf);

	values[0] = BoolGetDatum(EvictUnpinnedBuffer(buf, &buffer_flushed));
	values[1] = BoolGetDatum(buffer_flushed);

	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * Try to evict specified relation.
 * 尝试淘汰指定关系的缓冲区。
 */
Datum
pg_buffercache_evict_relation(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_EVICT_RELATION_ELEM];
	bool		nulls[NUM_BUFFERCACHE_EVICT_RELATION_ELEM] = {0};

	Oid			relOid;
	Relation	rel;

	int32		buffers_evicted = 0;
	int32		buffers_flushed = 0;
	int32		buffers_skipped = 0;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pg_buffercache_superuser_check("pg_buffercache_evict_relation");

	relOid = PG_GETARG_OID(0);

	rel = relation_open(relOid, AccessShareLock);

	if (RelationUsesLocalBuffers(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation uses local buffers, %s() is intended to be used for shared buffers only",
						"pg_buffercache_evict_relation")));

	EvictRelUnpinnedBuffers(rel, &buffers_evicted, &buffers_flushed,
							&buffers_skipped);

	relation_close(rel, AccessShareLock);

	values[0] = Int32GetDatum(buffers_evicted);
	values[1] = Int32GetDatum(buffers_flushed);
	values[2] = Int32GetDatum(buffers_skipped);

	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}


/*
 * Try to evict all shared buffers.
 * 尝试淘汰所有共享缓冲区。
 */
Datum
pg_buffercache_evict_all(PG_FUNCTION_ARGS)
{
	Datum		result;
	TupleDesc	tupledesc;
	HeapTuple	tuple;
	Datum		values[NUM_BUFFERCACHE_EVICT_ALL_ELEM];
	bool		nulls[NUM_BUFFERCACHE_EVICT_ALL_ELEM] = {0};

	int32		buffers_evicted = 0;
	int32		buffers_flushed = 0;
	int32		buffers_skipped = 0;

	if (get_call_result_type(fcinfo, NULL, &tupledesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	pg_buffercache_superuser_check("pg_buffercache_evict_all");

	EvictAllUnpinnedBuffers(&buffers_evicted, &buffers_flushed,
							&buffers_skipped);

	values[0] = Int32GetDatum(buffers_evicted);
	values[1] = Int32GetDatum(buffers_flushed);
	values[2] = Int32GetDatum(buffers_skipped);

	tuple = heap_form_tuple(tupledesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}
