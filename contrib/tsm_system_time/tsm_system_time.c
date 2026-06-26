/*-------------------------------------------------------------------------
 *
 * tsm_system_time.c
 *	  support routines for SYSTEM_TIME tablesample method
 *
 * The desire here is to produce a random sample with as many rows as possible
 * in no more than the specified amount of time.  We use a block-sampling
 * approach.  To ensure that the whole relation will be visited if necessary,
 * we start at a randomly chosen block and then advance with a stride that
 * is randomly chosen but is relatively prime to the relation's nblocks.
 *
 * Because of the time dependence, this method is necessarily unrepeatable.
 * However, we do what we can to reduce surprising behavior by selecting
 * the sampling pattern just once per query, much as in tsm_system_rows.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/tsm_system_time/tsm_system_time.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/tsmapi.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "utils/sampling.h"
#include "utils/spccache.h"

PG_MODULE_MAGIC_EXT(
					.name = "tsm_system_time",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(tsm_system_time_handler);


/* Private state
 *
 * 私人国家
 */
typedef struct
{
	uint32		seed;			/* random seed */
	double		millis;			/* time limit for sampling */
	instr_time	start_time;		/* scan start time */
	OffsetNumber lt;			/* last tuple returned from current block */
	BlockNumber doneblocks;		/* number of already-scanned blocks */
	BlockNumber lb;				/* last block visited */
	/* these three values are not changed during a rescan:
	 *
	 * 这三个值在重新扫描期间不会更改：
	 */
	BlockNumber nblocks;		/* number of blocks in relation */
	BlockNumber firstblock;		/* first block to sample from */
	BlockNumber step;			/* step size, or 0 if not set yet */
} SystemTimeSamplerData;

static void system_time_samplescangetsamplesize(PlannerInfo *root,
												RelOptInfo *baserel,
												List *paramexprs,
												BlockNumber *pages,
												double *tuples);
static void system_time_initsamplescan(SampleScanState *node,
									   int eflags);
static void system_time_beginsamplescan(SampleScanState *node,
										Datum *params,
										int nparams,
										uint32 seed);
static BlockNumber system_time_nextsampleblock(SampleScanState *node, BlockNumber nblocks);
static OffsetNumber system_time_nextsampletuple(SampleScanState *node,
												BlockNumber blockno,
												OffsetNumber maxoffset);
static uint32 random_relative_prime(uint32 n, pg_prng_state *randstate);


/*
 * Create a TsmRoutine descriptor for the SYSTEM_TIME method.
 *
 * 为 SYSTEM_TIME 方法创建 TsmRoutine 描述符。
 */
Datum
tsm_system_time_handler(PG_FUNCTION_ARGS)
{
	TsmRoutine *tsm = makeNode(TsmRoutine);

	tsm->parameterTypes = list_make1_oid(FLOAT8OID);

	/* See notes at head of file
	 *
	 * 请参阅文件头的注释
	 */
	tsm->repeatable_across_queries = false;
	tsm->repeatable_across_scans = false;

	tsm->SampleScanGetSampleSize = system_time_samplescangetsamplesize;
	tsm->InitSampleScan = system_time_initsamplescan;
	tsm->BeginSampleScan = system_time_beginsamplescan;
	tsm->NextSampleBlock = system_time_nextsampleblock;
	tsm->NextSampleTuple = system_time_nextsampletuple;
	tsm->EndSampleScan = NULL;

	PG_RETURN_POINTER(tsm);
}

/*
 * Sample size estimation.
 *
 * 样本量估计。
 */
static void
system_time_samplescangetsamplesize(PlannerInfo *root,
									RelOptInfo *baserel,
									List *paramexprs,
									BlockNumber *pages,
									double *tuples)
{
	Node	   *limitnode;
	double		millis;
	double		spc_random_page_cost;
	double		npages;
	double		ntuples;

	/* Try to extract an estimate for the limit time spec
	 *
	 * 尝试提取限制时间规范的估计值
	 */
	limitnode = (Node *) linitial(paramexprs);
	limitnode = estimate_expression_value(root, limitnode);

	if (IsA(limitnode, Const) &&
		!((Const *) limitnode)->constisnull)
	{
		millis = DatumGetFloat8(((Const *) limitnode)->constvalue);
		if (millis < 0 || isnan(millis))
		{
			/* Default millis if the value is bogus
			 *
			 * 如果该值是假的，则默认毫秒
			 */
			millis = 1000;
		}
	}
	else
	{
		/* Default millis if we didn't obtain a non-null Const
		 *
		 * 如果我们没有获得非空 Const，则默认毫秒
		 */
		millis = 1000;
	}

	/* Get the planner's idea of cost per page read
	 *
	 * 了解规划者对每页阅读成本的想法
	 */
	get_tablespace_page_costs(baserel->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Estimate the number of pages we can read by assuming that the cost
	 * figure is expressed in milliseconds.  This is completely, unmistakably
	 * bogus, but we have to do something to produce an estimate and there's
	 * no better answer.
	 *
	 * 通过假设成本数字以毫秒表示来估计我们可以读取的页数。  这完全是、毫无疑问是假的，但我们必须做一些事情来产生一个估计，而且没有更好的答案。
	 */
	if (spc_random_page_cost > 0)
		npages = millis / spc_random_page_cost;
	else
		npages = millis;		/* even more bogus, but whatcha gonna do? */

	/* Clamp to sane value
	 *
	 * 钳位到合理值
	 */
	npages = clamp_row_est(Min((double) baserel->pages, npages));

	if (baserel->tuples > 0 && baserel->pages > 0)
	{
		/* Estimate number of tuples returned based on tuple density
		 *
		 * 根据元组密度估计返回的元组数量
		 */
		double		density = baserel->tuples / (double) baserel->pages;

		ntuples = npages * density;
	}
	else
	{
		/* For lack of data, assume one tuple per page
		 *
		 * 由于缺乏数据，假设每页一个元组
		 */
		ntuples = npages;
	}

	/* Clamp to the estimated relation size
	 *
	 * 钳位到估计的关系尺寸
	 */
	ntuples = clamp_row_est(Min(baserel->tuples, ntuples));

	*pages = npages;
	*tuples = ntuples;
}

/*
 * Initialize during executor setup.
 *
 * 在执行器设置期间初始化。
 */
static void
system_time_initsamplescan(SampleScanState *node, int eflags)
{
	node->tsm_state = palloc0(sizeof(SystemTimeSamplerData));
	/* Note the above leaves tsm_state->step equal to zero
	 *
	 * 注意上面的 tsm_state->step 等于 0
	 */
}

/*
 * Examine parameters and prepare for a sample scan.
 *
 * 检查参数并准备样本扫描。
 */
static void
system_time_beginsamplescan(SampleScanState *node,
							Datum *params,
							int nparams,
							uint32 seed)
{
	SystemTimeSamplerData *sampler = (SystemTimeSamplerData *) node->tsm_state;
	double		millis = DatumGetFloat8(params[0]);

	if (millis < 0 || isnan(millis))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
				 errmsg("sample collection time must not be negative")));

	sampler->seed = seed;
	sampler->millis = millis;
	sampler->lt = InvalidOffsetNumber;
	sampler->doneblocks = 0;
	/* start_time, lb will be initialized during first NextSampleBlock call
	 *
	 * start_time, lb 将在第一次 NextSampleBlock 调用期间初始化
	 */
	/* we intentionally do not change nblocks/firstblock/step here
	 *
	 * 我们故意不在这里更改 nblocks/firstblock/step
	 */
}

/*
 * Select next block to sample.
 *
 * 选择下一个要采样的块。
 *
 * Uses linear probing algorithm for picking next block.
 *
 * 使用线性探测算法来挑选下一个块。
 */
static BlockNumber
system_time_nextsampleblock(SampleScanState *node, BlockNumber nblocks)
{
	SystemTimeSamplerData *sampler = (SystemTimeSamplerData *) node->tsm_state;
	instr_time	cur_time;

	/* First call within scan?
	 *
	 * 扫描内第一次调用？
	 */
	if (sampler->doneblocks == 0)
	{
		/* First scan within query?
		 *
		 * 查询中的第一次扫描？
		 */
		if (sampler->step == 0)
		{
			/* Initialize now that we have scan descriptor
			 *
			 * 现在我们有了扫描描述符，进行初始化
			 */
			pg_prng_state randstate;

			/* If relation is empty, there's nothing to scan
			 *
			 * 如果关系为空，则没有可扫描的内容
			 */
			if (nblocks == 0)
				return InvalidBlockNumber;

			/* We only need an RNG during this setup step
			 *
			 * 在此设置步骤中我们只需要一个 RNG
			 */
			sampler_random_init_state(sampler->seed, &randstate);

			/* Compute nblocks/firstblock/step only once per query
			 *
			 * 每个查询仅计算一次 nblocks/firstblock/step
			 */
			sampler->nblocks = nblocks;

			/* Choose random starting block within the relation
			 *
			 * 在关系中选择随机起始块
			 */
			/* (Actually this is the predecessor of the first block visited)
			 *
			 * （实际上这是访问的第一个块的前身）
			 */
			sampler->firstblock = sampler_random_fract(&randstate) *
				sampler->nblocks;

			/* Find relative prime as step size for linear probing
			 *
			 * 查找相对素数作为线性探测的步长
			 */
			sampler->step = random_relative_prime(sampler->nblocks, &randstate);
		}

		/* Reinitialize lb and start_time
		 *
		 * 重新初始化 lb 和 start_time
		 */
		sampler->lb = sampler->firstblock;
		INSTR_TIME_SET_CURRENT(sampler->start_time);
	}

	/* If we've read all blocks in relation, we're done
	 *
	 * 如果我们已经阅读了所有相关的块，我们就完成了
	 */
	if (++sampler->doneblocks > sampler->nblocks)
		return InvalidBlockNumber;

	/* If we've used up all the allotted time, we're done
	 *
	 * 如果我们用完了所有分配的时间，我们就完成了
	 */
	INSTR_TIME_SET_CURRENT(cur_time);
	INSTR_TIME_SUBTRACT(cur_time, sampler->start_time);
	if (INSTR_TIME_GET_MILLISEC(cur_time) >= sampler->millis)
		return InvalidBlockNumber;

	/*
	 * It's probably impossible for scan->rs_nblocks to decrease between scans
	 * within a query; but just in case, loop until we select a block number
	 * less than scan->rs_nblocks.  We don't care if scan->rs_nblocks has
	 * increased since the first scan.
	 *
	 * scan->rs_nblocks 可能不可能在查询内的扫描之间减少；但为了以防万一，循环直到我们选择一个小于 scan->rs_nblocks 的块号。  我们不关心自第一次扫描以来 scan->rs_nblocks 是否增加。
	 */
	do
	{
		/* Advance lb, using uint64 arithmetic to forestall overflow
		 *
		 * Advance lb，使用uint64算法来防止溢出
		 */
		sampler->lb = ((uint64) sampler->lb + sampler->step) % sampler->nblocks;
	} while (sampler->lb >= nblocks);

	return sampler->lb;
}

/*
 * Select next sampled tuple in current block.
 *
 * 选择当前块中的下一个采样元组。
 *
 * In block sampling, we just want to sample all the tuples in each selected
 * block.
 *
 * 在块采样中，我们只想对每个选定块中的所有元组进行采样。
 *
 * When we reach end of the block, return InvalidOffsetNumber which tells
 * SampleScan to go to next block.
 *
 * 当我们到达块的末尾时，返回 InvalidOffsetNumber 告诉 SampleScan 转到下一个块。
 */
static OffsetNumber
system_time_nextsampletuple(SampleScanState *node,
							BlockNumber blockno,
							OffsetNumber maxoffset)
{
	SystemTimeSamplerData *sampler = (SystemTimeSamplerData *) node->tsm_state;
	OffsetNumber tupoffset = sampler->lt;

	/* Advance to next possible offset on page
	 *
	 * 前进到页面上下一个可能的偏移量
	 */
	if (tupoffset == InvalidOffsetNumber)
		tupoffset = FirstOffsetNumber;
	else
		tupoffset++;

	/* Done?
	 *
	 * 完毕？
	 */
	if (tupoffset > maxoffset)
		tupoffset = InvalidOffsetNumber;

	sampler->lt = tupoffset;

	return tupoffset;
}

/*
 * Compute greatest common divisor of two uint32's.
 *
 * 计算两个 uint32 的最大公约数。
 */
static uint32
gcd(uint32 a, uint32 b)
{
	uint32		c;

	while (a != 0)
	{
		c = a;
		a = b % a;
		b = c;
	}

	return b;
}

/*
 * Pick a random value less than and relatively prime to n, if possible
 * (else return 1).
 *
 * 如果可能的话，选择一个小于 n 且与​​ n 互质的随机值（否则返回 1）。
 */
static uint32
random_relative_prime(uint32 n, pg_prng_state *randstate)
{
	uint32		r;

	/* Safety check to avoid infinite loop or zero result for small n.
	 *
	 * 安全检查以避免小 n 的无限循环或零结果。
	 */
	if (n <= 1)
		return 1;

	/*
	 * This should only take 2 or 3 iterations as the probability of 2 numbers
	 * being relatively prime is ~61%; but just in case, we'll include a
	 * CHECK_FOR_INTERRUPTS in the loop.
	 *
	 * 这应该只需要 2 或 3 次迭代，因为 2 个数字互质的概率约为 61%；但为了以防万一，我们将在循环中包含 CHECK_FOR_INTERRUPTS。
	 */
	do
	{
		CHECK_FOR_INTERRUPTS();
		r = (uint32) (sampler_random_fract(randstate) * n);
	} while (r == 0 || gcd(r, n) > 1);

	return r;
}
