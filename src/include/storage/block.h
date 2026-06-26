/*-------------------------------------------------------------------------
 *
 * block.h
 *	  POSTGRES disk block definitions.
 *	  POSTGRES 磁盘块定义。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/block.h
 *
 * Core Flow:
 * 核心流程：
 * 1. Addressing: PostgreSQL views files as a linear sequence of blocks (usually 8KB).
 *    编址：PostgreSQL 将文件视为块（通常为 8KB）的线性序列。
 * 2. Representation: Use 'BlockNumber' (uint32) for in-memory operations and 'BlockIdData' (2x uint16)
 *    for compact on-disk storage to save space and allow alignment.
 *    表示：使用 'BlockNumber' (uint32) 进行内存操作，使用 'BlockIdData' (2x uint16) 进行紧凑的磁盘存储，以节省空间并允许对齐。
 * 3. Conversion: Macros/functions are provided to seamlessly convert between the two representations.
 *    转换：提供了宏/函数，以便在两种表示形式之间无缝转换。
 *
 *-------------------------------------------------------------------------
 */
#ifndef BLOCK_H
#define BLOCK_H

/*
 * BlockNumber:
 *
 * each data file (heap or index) is divided into postgres disk blocks
 * (which may be thought of as the unit of i/o -- a postgres buffer
 * contains exactly one disk block).  the blocks are numbered
 * sequentially, 0 to 0xFFFFFFFE.
 * 每个数据文件（堆或索引）都分为 postgres 磁盘块（可以将其视为 I/O 的基本单位 —— 一个 postgres 缓冲区
 * 正好包含一个磁盘块）。这些块按顺序编号，从 0 到 0xFFFFFFFE。
 *
 * InvalidBlockNumber is the same thing as P_NEW in bufmgr.h.
 * InvalidBlockNumber 与 bufmgr.h 中的 P_NEW 是相同的。
 *
 * the access methods, the buffer manager and the storage manager are
 * more or less the only pieces of code that should be accessing disk
 * blocks directly.
 * 访问方法、缓冲区管理器和存储管理器或多或少是仅有的应该直接访问磁盘块的代码片段。
 */
typedef uint32 BlockNumber;

#define InvalidBlockNumber		((BlockNumber) 0xFFFFFFFF)

#define MaxBlockNumber			((BlockNumber) 0xFFFFFFFE)

/*
 * BlockId:
 *
 * this is a storage type for BlockNumber.  in other words, this type
 * is used for on-disk structures (e.g., in HeapTupleData) whereas
 * BlockNumber is the type on which calculations are performed (e.g.,
 * in access method code).
 * 这是 BlockNumber 的一种存储类型。换句话说，此类型用于磁盘上的结构（例如，在 HeapTupleData 中），
 * 而 BlockNumber 是执行计算的类型（例如，在访问方法代码中）。
 *
 * there doesn't appear to be any reason to have separate types except
 * for the fact that BlockIds can be SHORTALIGN'd (and therefore any
 * structures that contains them, such as ItemPointerData, can also be
 * SHORTALIGN'd).  this is an important consideration for reducing the
 * space requirements of the line pointer (ItemIdData) array on each
 * page and the header of each heap or index tuple, so it doesn't seem
 * wise to change this without good reason.
 * 似乎没有任何理由拥有单独的类型，除了 BlockIds 可以进行 SHORTALIGN（短对齐）（因此任何
 * 包含它们的结构，例如 ItemPointerData，也可以进行 SHORTALIGN）。
 * 这是减少每个页面上的行指针（ItemIdData）数组以及每个堆或索引元组头部的空间要求的重要考虑因素，
 * 因此在没有充分理由的情况下更改这一点似乎不明智。
 */
typedef struct BlockIdData
{
	uint16		bi_hi;
	uint16		bi_lo;
} BlockIdData;

typedef BlockIdData *BlockId;	/* block identifier */

/* ----------------
 *		support functions
 * ----------------
 */

/*
 * BlockNumberIsValid
 *		True iff blockNumber is valid.
 *		如果 blockNumber 有效则返回 true。
 */
static inline bool
BlockNumberIsValid(BlockNumber blockNumber)
{
	return blockNumber != InvalidBlockNumber;
}

/*
 * BlockIdSet
 *		Sets a block identifier to the specified value.
 *		将块标识符设置为指定值。
 */
static inline void
BlockIdSet(BlockIdData *blockId, BlockNumber blockNumber)
{
	blockId->bi_hi = blockNumber >> 16;
	blockId->bi_lo = blockNumber & 0xffff;
}

/*
 * BlockIdEquals
 *		Check for block number equality.
 *		检查两个块标识符对应的块号是否相等。
 */
static inline bool
BlockIdEquals(const BlockIdData *blockId1, const BlockIdData *blockId2)
{
	return (blockId1->bi_hi == blockId2->bi_hi &&
			blockId1->bi_lo == blockId2->bi_lo);
}

/*
 * BlockIdGetBlockNumber
 *		Retrieve the block number from a block identifier.
 *		从块标识符中检索块号。
 */
static inline BlockNumber
BlockIdGetBlockNumber(const BlockIdData *blockId)
{
	return (((BlockNumber) blockId->bi_hi) << 16) | ((BlockNumber) blockId->bi_lo);
}

#endif							/* BLOCK_H */
