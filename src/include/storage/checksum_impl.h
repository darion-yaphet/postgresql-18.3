/*-------------------------------------------------------------------------
 *
 * checksum_impl.h
 *	  Checksum implementation for data pages.
 *
 *	  数据页的校验和实现。
 *
 * This file exists for the benefit of external programs that may wish to
 * check Postgres page checksums.  They can #include this to get the code
 * referenced by storage/checksum.h.  (Note: you may need to redefine
 * Assert() as empty to compile this successfully externally.)
 *
 * 此文件供可能希望检查 Postgres 页面校验和的外部程序使用。它们可以 #include
 * 此文件以取得 storage/checksum.h 引用的代码。（注意：要在外部成功编译，
 * 可能需要将 Assert() 重定义为空。）
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum_impl.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * The algorithm used to checksum pages is chosen for very fast calculation.
 * Workloads where the database working set fits into OS file cache but not
 * into shared buffers can read in pages at a very fast pace and the checksum
 * algorithm itself can become the largest bottleneck.
 *
 * 用于页面校验和的算法经过选择，以实现非常快速的计算。对于数据库工作集可放入
 * 操作系统文件缓存但无法放入共享缓冲区的工作负载，页面读取速度可能非常快，
 * 校验和算法本身可能成为最大的瓶颈。
 *
 * The checksum algorithm itself is based on the FNV-1a hash (FNV is shorthand
 * for Fowler/Noll/Vo).  The primitive of a plain FNV-1a hash folds in data 1
 * byte at a time according to the formula:
 *
 *	   hash = (hash ^ value) * FNV_PRIME
 *
 * FNV-1a algorithm is described at http://www.isthe.com/chongo/tech/comp/fnv/
 *
 * 校验和算法本身基于 FNV-1a 哈希（FNV 是 Fowler/Noll/Vo 的缩写）。普通
 * FNV-1a 哈希的基本操作按照下列公式每次合入 1 个字节的数据：
 *
 *	   hash = (hash ^ value) * FNV_PRIME
 *
 * FNV-1a 算法见 http://www.isthe.com/chongo/tech/comp/fnv/。
 *
 * PostgreSQL doesn't use FNV-1a hash directly because it has bad mixing of
 * high bits - high order bits in input data only affect high order bits in
 * output data. To resolve this we xor in the value prior to multiplication
 * shifted right by 17 bits. The number 17 was chosen because it doesn't
 * have common denominator with set bit positions in FNV_PRIME and empirically
 * provides the fastest mixing for high order bits of final iterations quickly
 * avalanche into lower positions. For performance reasons we choose to combine
 * 4 bytes at a time. The actual hash formula used as the basis is:
 *
 *	   hash = (hash ^ value) * FNV_PRIME ^ ((hash ^ value) >> 17)
 *
 * PostgreSQL 不直接使用 FNV-1a 哈希，因为它对高位的混合效果较差：输入数据的
 * 高位只会影响输出数据的高位。为解决这一问题，我们在乘法前将右移 17 位的值
 * 异或进去。选择 17 是因为它与 FNV_PRIME 中置位的位置没有公因数，并且经验上
 * 能让最终迭代的高位迅速雪崩到较低位置，从而最快地混合高位。出于性能考虑，
 * 我们一次合并 4 个字节。作为基础实际使用的哈希公式是：
 *
 *	   hash = (hash ^ value) * FNV_PRIME ^ ((hash ^ value) >> 17)
 *
 * The main bottleneck in this calculation is the multiplication latency. To
 * hide the latency and to make use of SIMD parallelism multiple hash values
 * are calculated in parallel. The page is treated as a 32 column two
 * dimensional array of 32 bit values. Each column is aggregated separately
 * into a partial checksum. Each partial checksum uses a different initial
 * value (offset basis in FNV terminology). The initial values actually used
 * were chosen randomly, as the values themselves don't matter as much as that
 * they are different and don't match anything in real data. After initializing
 * partial checksums each value in the column is aggregated according to the
 * above formula. Finally two more iterations of the formula are performed with
 * value 0 to mix the bits of the last value added.
 *
 * 此计算的主要瓶颈是乘法延迟。为隐藏该延迟并利用 SIMD 并行性，会并行计算多个
 * 哈希值。页面被视为一个有 32 列、元素为 32 位值的二维数组。每一列单独聚合为
 * 一个部分校验和。每个部分校验和使用不同的初始值（FNV 术语中的偏移基值）。
 * 实际使用的初始值是随机选定的，值本身不如它们彼此不同且不匹配真实数据重要。
 * 初始化部分校验和后，列中的每个值按上述公式聚合。最后以值 0 再执行两轮公式，
 * 以混合最后加入值的位。
 *
 * The partial checksums are then folded together using xor to form a single
 * 32-bit checksum. The caller can safely reduce the value to 16 bits
 * using modulo 2^16-1. That will cause a very slight bias towards lower
 * values but this is not significant for the performance of the
 * checksum.
 *
 * 随后使用异或将部分校验和折叠成一个 32 位校验和。调用者可以安全地用模
 * 2^16-1 的方式将该值缩减为 16 位。这会使较低数值略微偏多，但对校验和的
 * 性能没有显著影响。
 *
 * The algorithm choice was based on what instructions are available in SIMD
 * instruction sets. This meant that a fast and good algorithm needed to use
 * multiplication as the main mixing operator. The simplest multiplication
 * based checksum primitive is the one used by FNV. The prime used is chosen
 * for good dispersion of values. It has no known simple patterns that result
 * in collisions. Test of 5-bit differentials of the primitive over 64bit keys
 * reveals no differentials with 3 or more values out of 100000 random keys
 * colliding. Avalanche test shows that only high order bits of the last word
 * have a bias. Tests of 1-4 uncorrelated bit errors, stray 0 and 0xFF bytes,
 * overwriting page from random position to end with 0 bytes, and overwriting
 * random segments of page with 0x00, 0xFF and random data all show optimal
 * 2e-16 false positive rate within margin of error.
 *
 * 算法选择基于 SIMD 指令集可用的指令。这意味着快速且良好的算法需要以乘法作为
 * 主要混合操作。最简单的基于乘法的校验和基本操作就是 FNV 所使用的操作。所选
 * 质数可以良好地分散值，且没有已知会导致碰撞的简单模式。对 64 位键的基本操作
 * 进行 5 位差分测试显示，在 100000 个随机键中，没有三个或更多值发生碰撞的
 * 差分。雪崩测试显示，只有最后一个字的高位存在偏差。对 1–4 个不相关位错误、
 * 游离的 0 和 0xFF 字节、从随机位置到页面结尾写入 0 字节，以及用 0x00、0xFF
 * 和随机数据覆盖页面随机片段的测试，都在误差范围内显示出最优的 2e-16 假阳性率。
 *
 * Vectorization of the algorithm requires 32bit x 32bit -> 32bit integer
 * multiplication instruction. As of 2013 the corresponding instruction is
 * available on x86 SSE4.1 extensions (pmulld) and ARM NEON (vmul.i32).
 * Vectorization requires a compiler to do the vectorization for us. For recent
 * GCC versions the flags -msse4.1 -funroll-loops -ftree-vectorize are enough
 * to achieve vectorization.
 *
 * 算法向量化需要 32 位乘 32 位得到 32 位整数乘法指令。截至 2013 年，相应指令
 * 可用于 x86 SSE4.1 扩展（pmulld）和 ARM NEON（vmul.i32）。向量化要求编译器
 * 为我们完成向量化。对于较新的 GCC 版本，-msse4.1 -funroll-loops
 * -ftree-vectorize 标志足以实现向量化。
 *
 * The optimal amount of parallelism to use depends on CPU specific instruction
 * latency, SIMD instruction width, throughput and the amount of registers
 * available to hold intermediate state. Generally, more parallelism is better
 * up to the point that state doesn't fit in registers and extra load-store
 * instructions are needed to swap values in/out. The number chosen is a fixed
 * part of the algorithm because changing the parallelism changes the checksum
 * result.
 *
 * 可使用的最佳并行度取决于 CPU 特有的指令延迟、SIMD 指令宽度、吞吐量以及可用于
 * 保存中间状态的寄存器数量。通常更多并行度更好，直到状态无法装入寄存器、必须用
 * 额外的加载和存储指令交换值为止。所选数量是算法的固定组成部分，因为改变并行度
 * 会改变校验和结果。
 *
 * The parallelism number 32 was chosen based on the fact that it is the
 * largest state that fits into architecturally visible x86 SSE registers while
 * leaving some free registers for intermediate values. For future processors
 * with 256bit vector registers this will leave some performance on the table.
 * When vectorization is not available it might be beneficial to restructure
 * the computation to calculate a subset of the columns at a time and perform
 * multiple passes to avoid register spilling. This optimization opportunity
 * is not used. Current coding also assumes that the compiler has the ability
 * to unroll the inner loop to avoid loop overhead and minimize register
 * spilling. For less sophisticated compilers it might be beneficial to
 * manually unroll the inner loop.
 *
 * 并行度 32 的选择基于这样一个事实：它是在保留一些自由寄存器给中间值的同时，
 * 能装入体系结构可见 x86 SSE 寄存器的最大状态。对于拥有 256 位向量寄存器的未来
 * 处理器，这会损失一些性能。向量化不可用时，重组计算以一次计算部分列并执行多轮
 * 处理、从而避免寄存器溢出，可能是有益的；本实现未使用这一优化机会。当前代码还
 * 假定编译器能够展开内层循环，以避免循环开销并最小化寄存器溢出。对于不那么复杂的
 * 编译器，手动展开内层循环可能有益。
 */

#include "storage/bufpage.h"

/* number of checksums to calculate in parallel */

/* 并行计算的校验和数量。 */
#define N_SUMS 32
/* prime multiplier of FNV-1a hash */

/* FNV-1a 哈希的质数乘数。 */
#define FNV_PRIME 16777619

/* Use a union so that this code is valid under strict aliasing */

/* 使用联合体，使此代码在严格别名规则下仍然有效。 */
typedef union
{
	PageHeaderData phdr;
	uint32		data[BLCKSZ / (sizeof(uint32) * N_SUMS)][N_SUMS];
} PGChecksummablePage;

/*
 * Base offsets to initialize each of the parallel FNV hashes into a
 * different initial state.
 *
 * 用于将每个并行 FNV 哈希初始化为不同初始状态的基准偏移量。
 */
static const uint32 checksumBaseOffsets[N_SUMS] = {
	0x5B1F36E9, 0xB8525960, 0x02AB50AA, 0x1DE66D2A,
	0x79FF467A, 0x9BB9F8A3, 0x217E7CD2, 0x83E13D2C,
	0xF8D4474F, 0xE39EB970, 0x42C6AE16, 0x993216FA,
	0x7B093B5D, 0x98DAFF3C, 0xF718902A, 0x0B1C9CDB,
	0xE58F764B, 0x187636BC, 0x5D7B3BB1, 0xE73DE7DE,
	0x92BEC979, 0xCCA6C0B2, 0x304A0979, 0x85AA43D4,
	0x783125BB, 0x6CA8EAA2, 0xE407EAC6, 0x4B5CFC3E,
	0x9FBF8C76, 0x15CA20BE, 0xF2CA9FD3, 0x959BD756
};

/*
 * Calculate one round of the checksum.
 *
 * 计算校验和的一轮处理。
 */
#define CHECKSUM_COMP(checksum, value) \
do { \
	uint32 __tmp = (checksum) ^ (value); \
	(checksum) = __tmp * FNV_PRIME ^ (__tmp >> 17); \
} while (0)

/*
 * Block checksum algorithm.  The page must be adequately aligned
 * (at least on 4-byte boundary).
 *
 * 块校验和算法。页面必须充分对齐（至少按 4 字节边界对齐）。
 *
 * The function initializes parallel lanes, mixes all page words, and xor-folds the partial checksums.
 *
 * 该函数初始化并行通道、混合全部页面字，并通过异或折叠部分校验和。
 */
static uint32
pg_checksum_block(const PGChecksummablePage *page)
{
	uint32		sums[N_SUMS];
	uint32		result = 0;
	uint32		i,
				j;

	/* ensure that the size is compatible with the algorithm */

	/* 确保大小与算法兼容。 */
	Assert(sizeof(PGChecksummablePage) == BLCKSZ);

	/* initialize partial checksums to their corresponding offsets */

	/* 将部分校验和初始化为相应的偏移量。 */
	memcpy(sums, checksumBaseOffsets, sizeof(checksumBaseOffsets));

	/* main checksum calculation */

	/* 主校验和计算。 */
	for (i = 0; i < (uint32) (BLCKSZ / (sizeof(uint32) * N_SUMS)); i++)
		for (j = 0; j < N_SUMS; j++)
			CHECKSUM_COMP(sums[j], page->data[i][j]);

	/* finally add in two rounds of zeroes for additional mixing */

	/* 最后加入两轮零值以进行额外混合。 */
	for (i = 0; i < 2; i++)
		for (j = 0; j < N_SUMS; j++)
			CHECKSUM_COMP(sums[j], 0);

	/* xor fold partial checksums together */

	/* 通过异或将部分校验和折叠在一起。 */
	for (i = 0; i < N_SUMS; i++)
		result ^= sums[i];

	return result;
}

/*
 * Compute the checksum for a Postgres page.
 *
 * The page must be adequately aligned (at least on a 4-byte boundary).
 * Beware also that the checksum field of the page is transiently zeroed.
 *
 * 页面必须充分对齐（至少按 4 字节边界对齐）。还应注意，页面的校验和字段会被
 * 暂时置零。
 *
 * The checksum includes the block number (to detect the case where a page is
 * somehow moved to a different location), the page header (excluding the
 * checksum itself), and the page data.
 *
 * 校验和包括块号（用于检测页面以某种方式移动到不同位置的情况）、页面头部
 * （不包括校验和自身）以及页面数据。
 *
 * The function temporarily clears the checksum field, calculates the block hash, mixes the block number, and restores the field.
 *
 * 该函数暂时清除校验和字段、计算块哈希、混入块号，然后恢复该字段。
 */
uint16
pg_checksum_page(char *page, BlockNumber blkno)
{
	PGChecksummablePage *cpage = (PGChecksummablePage *) page;
	uint16		save_checksum;
	uint32		checksum;

	/* We only calculate the checksum for properly-initialized pages */

	/* 我们只为正确初始化的页面计算校验和。 */
	Assert(!PageIsNew((Page) page));

	/*
	 * Save pd_checksum and temporarily set it to zero, so that the checksum
	 * calculation isn't affected by the old checksum stored on the page.
	 * Restore it after, because actually updating the checksum is NOT part of
	 * the API of this function.
	 *
	 * 保存 pd_checksum 并暂时将其置零，以免计算受到页面上原有校验和的影响。
	 * 随后恢复它，因为实际更新校验和不是此函数 API 的一部分。
	 */
	save_checksum = cpage->phdr.pd_checksum;
	cpage->phdr.pd_checksum = 0;
	checksum = pg_checksum_block(cpage);
	cpage->phdr.pd_checksum = save_checksum;

	/* Mix in the block number to detect transposed pages */

	/* 混入块号以检测被调换位置的页面。 */
	checksum ^= blkno;

	/*
	 * Reduce to a uint16 (to fit in the pd_checksum field) with an offset of
	 * one. That avoids checksums of zero, which seems like a good idea.
	 *
	 * 以偏移量一缩减为 uint16（以适配 pd_checksum 字段）。这避免出现零校验和，
	 * 看起来是一个好主意。
	 */
	return (uint16) ((checksum % 65535) + 1);
}
