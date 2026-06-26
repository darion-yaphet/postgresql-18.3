/*-------------------------------------------------------------------------
 *
 * blutils.c
 *		Bloom index utilities.
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1990-1993, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/bloom/blutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/reloptions.h"
#include "bloom.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "storage/indexfsm.h"
#include "utils/memutils.h"
#include "varatt.h"

/* Signature dealing macros - note i is assumed to be of type int
 *
 * 签名处理宏 - 注意 i 被假定为 int 类型
 */
#define GETWORD(x,i) ( *( (BloomSignatureWord *)(x) + ( (i) / SIGNWORDBITS ) ) )
#define CLRBIT(x,i)   GETWORD(x,i) &= ~( 0x01 << ( (i) % SIGNWORDBITS ) )
#define SETBIT(x,i)   GETWORD(x,i) |=  ( 0x01 << ( (i) % SIGNWORDBITS ) )
#define GETBIT(x,i) ( (GETWORD(x,i) >> ( (i) % SIGNWORDBITS )) & 0x01 )

PG_FUNCTION_INFO_V1(blhandler);

/* Kind of relation options for bloom index
 *
 * 布卢姆索引的关系选项类型
 */
static relopt_kind bl_relopt_kind;

/* parse table for fillRelOptions
 *
 * fillRelOptions 的解析表
 */
static relopt_parse_elt bl_relopt_tab[INDEX_MAX_KEYS + 1];

static int32 myRand(void);
static void mySrand(uint32 seed);

/*
 * Module initialize function: initialize info about Bloom relation options.
 *
 * 模块初始化函数：初始化Bloom关系选项信息。
 *
 * Note: keep this in sync with makeDefaultBloomOptions().
 *
 * 注意：保持与 makeDefaultBloomOptions() 同步。
 */
void
_PG_init(void)
{
	int			i;
	char		buf[16];

	bl_relopt_kind = add_reloption_kind();

	/* Option for length of signature
	 *
	 * 签名长度选项
	 */
	add_int_reloption(bl_relopt_kind, "length",
					  "Length of signature in bits",
					  DEFAULT_BLOOM_LENGTH, 1, MAX_BLOOM_LENGTH,
					  AccessExclusiveLock);
	bl_relopt_tab[0].optname = "length";
	bl_relopt_tab[0].opttype = RELOPT_TYPE_INT;
	bl_relopt_tab[0].offset = offsetof(BloomOptions, bloomLength);

	/* Number of bits for each possible index column: col1, col2, ...
	 *
	 * 每个可能的索引列的位数：col1、col2、...
	 */
	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		snprintf(buf, sizeof(buf), "col%d", i + 1);
		add_int_reloption(bl_relopt_kind, buf,
						  "Number of bits generated for each index column",
						  DEFAULT_BLOOM_BITS, 1, MAX_BLOOM_BITS,
						  AccessExclusiveLock);
		bl_relopt_tab[i + 1].optname = MemoryContextStrdup(TopMemoryContext,
														   buf);
		bl_relopt_tab[i + 1].opttype = RELOPT_TYPE_INT;
		bl_relopt_tab[i + 1].offset = offsetof(BloomOptions, bitSize[0]) + sizeof(int) * i;
	}
}

/*
 * Construct a default set of Bloom options.
 *
 * 构造一组默认的 Bloom 选项。
 */
static BloomOptions *
makeDefaultBloomOptions(void)
{
	BloomOptions *opts;
	int			i;

	opts = (BloomOptions *) palloc0(sizeof(BloomOptions));
	/* Convert DEFAULT_BLOOM_LENGTH from # of bits to # of words
	 *
	 * 将 DEFAULT_BLOOM_LENGTH 从位数转换为字数
	 */
	opts->bloomLength = (DEFAULT_BLOOM_LENGTH + SIGNWORDBITS - 1) / SIGNWORDBITS;
	for (i = 0; i < INDEX_MAX_KEYS; i++)
		opts->bitSize[i] = DEFAULT_BLOOM_BITS;
	SET_VARSIZE(opts, sizeof(BloomOptions));
	return opts;
}

/*
 * Bloom handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 *
 * Bloom 处理函数：返回带有访问方法参数和回调的 IndexAmRoutine。
 */
Datum
blhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BLOOM_NSTRATEGIES;
	amroutine->amsupport = BLOOM_NPROC;
	amroutine->amoptsprocnum = BLOOM_OPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = blbuild;
	amroutine->ambuildempty = blbuildempty;
	amroutine->aminsert = blinsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = blbulkdelete;
	amroutine->amvacuumcleanup = blvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = blcostestimate;
	amroutine->amgettreeheight = NULL;
	amroutine->amoptions = bloptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = blvalidate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = blbeginscan;
	amroutine->amrescan = blrescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = blgetbitmap;
	amroutine->amendscan = blendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;
	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * Fill BloomState structure for particular index.
 *
 * 填充特定索引的 BloomState 结构。
 */
void
initBloomState(BloomState *state, Relation index)
{
	int			i;

	state->nColumns = index->rd_att->natts;

	/* Initialize hash function for each attribute
	 *
	 * 为每个属性初始化哈希函数
	 */
	for (i = 0; i < index->rd_att->natts; i++)
	{
		fmgr_info_copy(&(state->hashFn[i]),
					   index_getprocinfo(index, i + 1, BLOOM_HASH_PROC),
					   CurrentMemoryContext);
		state->collations[i] = index->rd_indcollation[i];
	}

	/* Initialize amcache if needed with options from metapage
	 *
	 * 如果需要，使用元页面中的选项初始化 amcache
	 */
	if (!index->rd_amcache)
	{
		Buffer		buffer;
		Page		page;
		BloomMetaPageData *meta;
		BloomOptions *opts;

		opts = MemoryContextAlloc(index->rd_indexcxt, sizeof(BloomOptions));

		buffer = ReadBuffer(index, BLOOM_METAPAGE_BLKNO);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);

		if (!BloomPageIsMeta(page))
			elog(ERROR, "Relation is not a bloom index");
		meta = BloomPageGetMeta(BufferGetPage(buffer));

		if (meta->magickNumber != BLOOM_MAGICK_NUMBER)
			elog(ERROR, "Relation is not a bloom index");

		*opts = meta->opts;

		UnlockReleaseBuffer(buffer);

		index->rd_amcache = opts;
	}

	memcpy(&state->opts, index->rd_amcache, sizeof(state->opts));
	state->sizeOfBloomTuple = BLOOMTUPLEHDRSZ +
		sizeof(BloomSignatureWord) * state->opts.bloomLength;
}

/*
 * Random generator copied from FreeBSD.  Using own random generator here for
 * two reasons:
 *
 * 从 FreeBSD 复制的随机生成器。  在这里使用自己的随机生成器有两个原因：
 *
 * 1) In this case random numbers are used for on-disk storage.  Usage of
 *	  PostgreSQL number generator would obstruct it from all possible changes.
 * 2) Changing seed of PostgreSQL random generator would be undesirable side
 *	  effect.
 *
 * 1) 在这种情况下，随机数用于磁盘存储。  使用 PostgreSQL 数字生成器会阻止它进行所有可能的更改。 2) 改变 PostgreSQL 随机生成器的种子会产生不良的副作用。
 */
static int32 next;

static int32
myRand(void)
{
	/*----------
	 * Compute x = (7^5 * x) mod (2^31 - 1)
	 * without overflowing 31 bits:
	 *		(2^31 - 1) = 127773 * (7^5) + 2836
	 * From "Random number generators: good ones are hard to find",
	 * Park and Miller, Communications of the ACM, vol. 31, no. 10,
	 * October 1988, p. 1195.
	 *
	 * 计算 x = (7^5 * x) mod (2^31 - 1) 而不溢出 31 位： (2^31 - 1) = 127773 * (7^5) + 2836 来自“随机数生成器：好的生成器很难找到”，Park 和 Miller，《ACM 通信》，卷。 31、没有。 1988 年 10 月，第 10 页。 1195.
	 *----------
	 */
	int32		hi,
				lo,
				x;

	/* Must be in [1, 0x7ffffffe] range at this point.
	 *
	 * 此时必须在 [1, 0x7ffffffe] 范围内。
	 */
	hi = next / 127773;
	lo = next % 127773;
	x = 16807 * lo - 2836 * hi;
	if (x < 0)
		x += 0x7fffffff;
	next = x;
	/* Transform to [0, 0x7ffffffd] range.
	 *
	 * 转换为 [0, 0x7ffffffd] 范围。
	 */
	return (x - 1);
}

static void
mySrand(uint32 seed)
{
	next = seed;
	/* Transform to [1, 0x7ffffffe] range.
	 *
	 * 转换为 [1, 0x7ffffffe] 范围。
	 */
	next = (next % 0x7ffffffe) + 1;
}

/*
 * Add bits of given value to the signature.
 *
 * 将给定值的位添加到签名中。
 */
void
signValue(BloomState *state, BloomSignatureWord *sign, Datum value, int attno)
{
	uint32		hashVal;
	int			nBit,
				j;

	/*
	 * init generator with "column's" number to get "hashed" seed for new
	 * value. We don't want to map the same numbers from different columns
	 * into the same bits!
	 *
	 * 使用“列的”编号初始化生成器以获得新值的“散列”种子。我们不想将不同列中的相同数字映射到相同的位！
	 */
	mySrand(attno);

	/*
	 * Init hash sequence to map our value into bits. the same values in
	 * different columns will be mapped into different bits because of step
	 * above
	 *
	 * 初始化哈希序列以将我们的值映射为位。由于上述步骤，不同列中的相同值将被映射到不同的位
	 */
	hashVal = DatumGetInt32(FunctionCall1Coll(&state->hashFn[attno], state->collations[attno], value));
	mySrand(hashVal ^ myRand());

	for (j = 0; j < state->opts.bitSize[attno]; j++)
	{
		/* prevent multiple evaluation in SETBIT macro
		 *
		 * 防止 SETBIT 宏中的多重计算
		 */
		nBit = myRand() % (state->opts.bloomLength * SIGNWORDBITS);
		SETBIT(sign, nBit);
	}
}

/*
 * Make bloom tuple from values.
 *
 * 根据值制作绽放元组。
 */
BloomTuple *
BloomFormTuple(BloomState *state, ItemPointer iptr, Datum *values, bool *isnull)
{
	int			i;
	BloomTuple *res = (BloomTuple *) palloc0(state->sizeOfBloomTuple);

	res->heapPtr = *iptr;

	/* Blooming each column
	 *
	 * 绽放每一栏
	 */
	for (i = 0; i < state->nColumns; i++)
	{
		/* skip nulls
		 *
		 * 跳过空值
		 */
		if (isnull[i])
			continue;

		signValue(state, res->sign, values[i], i);
	}

	return res;
}

/*
 * Add new bloom tuple to the page.  Returns true if new tuple was successfully
 * added to the page.  Returns false if it doesn't fit on the page.
 *
 * 将新的Bloom元组添加到页面。  如果新元组已成功添加到页面，则返回 true。  如果页面不适合则返回 false。
 */
bool
BloomPageAddItem(BloomState *state, Page page, BloomTuple *tuple)
{
	BloomTuple *itup;
	BloomPageOpaque opaque;
	Pointer		ptr;

	/* We shouldn't be pointed to an invalid page
	 *
	 * 我们不应该被指向无效页面
	 */
	Assert(!PageIsNew(page) && !BloomPageIsDeleted(page));

	/* Does new tuple fit on the page?
	 *
	 * 新元组适合页面吗？
	 */
	if (BloomPageGetFreeSpace(state, page) < state->sizeOfBloomTuple)
		return false;

	/* Copy new tuple to the end of page
	 *
	 * 将新元组复制到页面末尾
	 */
	opaque = BloomPageGetOpaque(page);
	itup = BloomPageGetTuple(state, page, opaque->maxoff + 1);
	memcpy((Pointer) itup, (Pointer) tuple, state->sizeOfBloomTuple);

	/* Adjust maxoff and pd_lower
	 *
	 * 调整 maxoff 和 pd_lower
	 */
	opaque->maxoff++;
	ptr = (Pointer) BloomPageGetTuple(state, page, opaque->maxoff + 1);
	((PageHeader) page)->pd_lower = ptr - page;

	/* Assert we didn't overrun available space
	 *
	 * 断言我们没有超出可用空间
	 */
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);

	return true;
}

/*
 * Allocate a new page (either by recycling, or by extending the index file)
 * The returned buffer is already pinned and exclusive-locked
 * Caller is responsible for initializing the page by calling BloomInitPage
 *
 * 分配新页面（通过回收或扩展索引文件） 返回的缓冲区已被固定并独占锁定 调用者负责通过调用 BloomInitPage 初始化页面
 */
Buffer
BloomNewBuffer(Relation index)
{
	Buffer		buffer;

	/* First, try to get a page from FSM
	 *
	 * 首先，尝试从 FSM 获取页面
	 */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBuffer(index, blkno);

		/*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 *
		 * 我们必须警惕其他人已经回收该页面的可能性；如果是这样，缓冲区可能被锁定。
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			if (PageIsNew(page))
				return buffer;	/* OK to use, if never initialized */

			if (BloomPageIsDeleted(page))
				return buffer;	/* OK to use */

			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		}

		/* Can't use it, so release buffer and try again
		 *
		 * 无法使用，所以释放缓冲区并重试
		 */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file
	 *
	 * 必须扩展文件
	 */
	buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

	return buffer;
}

/*
 * Initialize any page of a bloom index.
 *
 * 初始化Bloom索引的任意页面。
 */
void
BloomInitPage(Page page, uint16 flags)
{
	BloomPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(BloomPageOpaqueData));

	opaque = BloomPageGetOpaque(page);
	opaque->flags = flags;
	opaque->bloom_page_id = BLOOM_PAGE_ID;
}

/*
 * Fill in metapage for bloom index.
 *
 * 填写Bloom索引的元页面。
 */
void
BloomFillMetapage(Relation index, Page metaPage)
{
	BloomOptions *opts;
	BloomMetaPageData *metadata;

	/*
	 * Choose the index's options.  If reloptions have been assigned, use
	 * those, otherwise create default options.
	 *
	 * 选择索引的选项。  如果已分配reloptions，请使用它们，否则创建默认选项。
	 */
	opts = (BloomOptions *) index->rd_options;
	if (!opts)
		opts = makeDefaultBloomOptions();

	/*
	 * Initialize contents of meta page, including a copy of the options,
	 * which are now frozen for the life of the index.
	 *
	 * 初始化元页​​面的内容，包括选项的副本，这些选项现在在索引的生命周期内被冻结。
	 */
	BloomInitPage(metaPage, BLOOM_META);
	metadata = BloomPageGetMeta(metaPage);
	memset(metadata, 0, sizeof(BloomMetaPageData));
	metadata->magickNumber = BLOOM_MAGICK_NUMBER;
	metadata->opts = *opts;
	((PageHeader) metaPage)->pd_lower += sizeof(BloomMetaPageData);

	/* If this fails, probably FreeBlockNumberArray size calc is wrong:
	 *
	 * 如果失败，则 FreeBlockNumberArray 大小计算可能是错误的：
	 */
	Assert(((PageHeader) metaPage)->pd_lower <= ((PageHeader) metaPage)->pd_upper);
}

/*
 * Initialize metapage for bloom index.
 *
 * 初始化Bloom索引的元页面。
 */
void
BloomInitMetapage(Relation index, ForkNumber forknum)
{
	Buffer		metaBuffer;
	Page		metaPage;
	GenericXLogState *state;

	/*
	 * Make a new page; since it is first page it should be associated with
	 * block number 0 (BLOOM_METAPAGE_BLKNO).  No need to hold the extension
	 * lock because there cannot be concurrent inserters yet.
	 *
	 * 新建一个页面；由于它是第一页，因此应与块号 0 (BLOOM_METAPAGE_BLKNO) 相关联。  不需要持有扩展锁，因为还不能有并发的插入器。
	 */
	metaBuffer = ReadBufferExtended(index, forknum, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);
	Assert(BufferGetBlockNumber(metaBuffer) == BLOOM_METAPAGE_BLKNO);

	/* Initialize contents of meta page
	 *
	 * 初始化元页​​面的内容
	 */
	state = GenericXLogStart(index);
	metaPage = GenericXLogRegisterBuffer(state, metaBuffer,
										 GENERIC_XLOG_FULL_IMAGE);
	BloomFillMetapage(index, metaPage);
	GenericXLogFinish(state);

	UnlockReleaseBuffer(metaBuffer);
}

/*
 * Parse reloptions for bloom index, producing a BloomOptions struct.
 *
 * 解析 Bloom 索引的重新选项，生成 BloomOptions 结构。
 */
bytea *
bloptions(Datum reloptions, bool validate)
{
	BloomOptions *rdopts;

	/* Parse the user-given reloptions
	 *
	 * 解析用户给定的重新选项
	 */
	rdopts = (BloomOptions *) build_reloptions(reloptions, validate,
											   bl_relopt_kind,
											   sizeof(BloomOptions),
											   bl_relopt_tab,
											   lengthof(bl_relopt_tab));

	/* Convert signature length from # of bits to # to words, rounding up
	 *
	 * 将签名长度从位数转换为字数，向上舍入
	 */
	if (rdopts)
		rdopts->bloomLength = (rdopts->bloomLength + SIGNWORDBITS - 1) / SIGNWORDBITS;

	return (bytea *) rdopts;
}
