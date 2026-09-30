/*
 * brin_tuple.h
 *		Declarations for dealing with BRIN-specific tuples.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/brin_tuple.h
 */
#ifndef BRIN_TUPLE_H
#define BRIN_TUPLE_H

#include "access/brin_internal.h"
#include "access/tupdesc.h"

/*
 * The BRIN opclasses may register serialization callback, in case the on-disk
 * and in-memory representations differ (e.g. for performance reasons).
 */

/*
 * BRIN 操作符类（opclass）可以注册序列化回调，以应对磁盘表示与内存
 * 表示不同的情况（例如出于性能方面的原因）。
 */
typedef void (*brin_serialize_callback_type) (BrinDesc *bdesc, Datum src, Datum *dst);

/*
 * A BRIN index stores one index tuple per page range.  Each index tuple
 * has one BrinValues struct for each indexed column; in turn, each BrinValues
 * has (besides the null flags) an array of Datum whose size is determined by
 * the opclass.
 */

/*
 * BRIN 索引为每个页范围存储一个索引元组。每个索引元组为每个被索引的
 * 列包含一个 BrinValues 结构；进而，每个 BrinValues（除了空值标志之外）
 * 拥有一个 Datum 数组，其大小由操作符类决定。
 */
typedef struct BrinValues
{
	AttrNumber	bv_attno;		/* index attribute number */

	/* 索引属性编号 */
	bool		bv_hasnulls;	/* are there any nulls in the page range? */

	/* 该页范围内是否存在任何空值？ */
	bool		bv_allnulls;	/* are all values nulls in the page range? */

	/* 该页范围内的所有值是否都是空值？ */
	Datum	   *bv_values;		/* current accumulated values */

	/* 当前累积的值 */
	Datum		bv_mem_value;	/* expanded accumulated values */

	/* 展开后的累积值 */
	MemoryContext bv_context;
	brin_serialize_callback_type bv_serialize;
} BrinValues;

/*
 * This struct is used to represent an in-memory index tuple.  The values can
 * only be meaningfully decoded with an appropriate BrinDesc.
 */

/*
 * 此结构用于表示一个内存中的索引元组。其中的值只有借助一个合适的
 * BrinDesc 才能被有意义地解码。
 */
typedef struct BrinMemTuple
{
	bool		bt_placeholder; /* this is a placeholder tuple */

	/* 这是一个占位符（placeholder）元组 */
	bool		bt_empty_range; /* range represents no tuples */

	/* 该范围不代表任何元组 */
	BlockNumber bt_blkno;		/* heap blkno that the tuple is for */

	/* 该元组所对应的堆块号 */
	MemoryContext bt_context;	/* memcxt holding the bt_columns values */

	/* 持有 bt_columns 各值的内存上下文 */
	/* output arrays for brin_deform_tuple: */

	/* 供 brin_deform_tuple 使用的输出数组： */
	Datum	   *bt_values;		/* values array */

	/* 值数组 */
	bool	   *bt_allnulls;	/* allnulls array */

	/* allnulls（全空值）数组 */
	bool	   *bt_hasnulls;	/* hasnulls array */

	/* hasnulls（含空值）数组 */
	/* not an output array, but must be last */

	/* 不是输出数组，但必须放在最后 */
	BrinValues	bt_columns[FLEXIBLE_ARRAY_MEMBER];
} BrinMemTuple;

/*
 * An on-disk BRIN tuple.  This is possibly followed by a nulls bitmask, with
 * room for 2 null bits (two bits for each indexed column); an opclass-defined
 * number of Datum values for each column follow.
 */

/*
 * 磁盘上的 BRIN 元组。其后可能跟随一个空值位掩码，为每个被索引的列
 * 预留 2 个空值位（每列两位）；再之后是每列若干个由操作符类定义数量
 * 的 Datum 值。
 */
typedef struct BrinTuple
{
	/* heap block number that the tuple is for */

	/* 该元组所对应的堆块号 */
	BlockNumber bt_blkno;

	/* ---------------
	 * bt_info is laid out in the following fashion:
	 *
	 * 7th (high) bit: has nulls
	 * 6th bit: is placeholder tuple
	 * 5th bit: range is empty
	 * 4-0 bit: offset of data
	 * ---------------
	 */

	/* ---------------
	 * bt_info 按如下方式布局：
	 *
	 * 第 7 位（最高位）：是否含有空值
	 * 第 6 位：是否为占位符元组
	 * 第 5 位：范围是否为空
	 * 第 4-0 位：数据的偏移量
	 * ---------------
	 */
	uint8		bt_info;
} BrinTuple;

#define SizeOfBrinTuple (offsetof(BrinTuple, bt_info) + sizeof(uint8))

/*
 * bt_info manipulation macros
 */

/*
 * 用于操作 bt_info 的宏
 */
#define BRIN_OFFSET_MASK		0x1F
#define BRIN_EMPTY_RANGE_MASK	0x20
#define BRIN_PLACEHOLDER_MASK	0x40
#define BRIN_NULLS_MASK			0x80

#define BrinTupleDataOffset(tup)	((Size) (((BrinTuple *) (tup))->bt_info & BRIN_OFFSET_MASK))
#define BrinTupleHasNulls(tup)	(((((BrinTuple *) (tup))->bt_info & BRIN_NULLS_MASK)) != 0)
#define BrinTupleIsPlaceholder(tup) (((((BrinTuple *) (tup))->bt_info & BRIN_PLACEHOLDER_MASK)) != 0)
#define BrinTupleIsEmptyRange(tup) (((((BrinTuple *) (tup))->bt_info & BRIN_EMPTY_RANGE_MASK)) != 0)


/*
 * Serialize an in-memory BrinMemTuple for the given heap block into a newly
 * allocated on-disk BrinTuple, encoding the per-column values, null flags and
 * bt_info bits, and returning the resulting tuple along with its size.
 *
 * 将给定堆块对应的内存中 BrinMemTuple 序列化为一个新分配的磁盘上
 * BrinTuple，编码各列的值、空值标志以及 bt_info 位，并返回生成的元组
 * 及其大小。
 */
extern BrinTuple *brin_form_tuple(BrinDesc *brdesc, BlockNumber blkno,
								  BrinMemTuple *tuple, Size *size);

/*
 * Build a placeholder BRIN tuple for the given heap block, used to reserve a
 * revmap entry for a range that has not yet been summarized; returns the new
 * tuple and its size via the output parameter.
 *
 * 为给定堆块构建一个占位符 BRIN 元组，用于为尚未被汇总的范围预留一个
 * revmap 条目；通过输出参数返回新元组及其大小。
 */
extern BrinTuple *brin_form_placeholder_tuple(BrinDesc *brdesc,
											  BlockNumber blkno, Size *size);

/*
 * Free the memory allocated for an on-disk BRIN tuple previously produced by
 * one of the brin_form_* routines.
 *
 * 释放此前由某个 brin_form_* 例程生成的磁盘上 BRIN 元组所分配的内存。
 */
extern void brin_free_tuple(BrinTuple *tuple);

/*
 * Make a copy of a BRIN tuple, reusing the provided destination buffer if it
 * is large enough (growing it otherwise) and updating its size, so tuples can
 * be retained beyond the lifetime of their source buffer.
 *
 * 复制一个 BRIN 元组，若提供的目标缓冲区足够大则复用它（否则将其扩大），
 * 并更新其大小，从而使元组能够在其源缓冲区的生命周期之外继续保留。
 */
extern BrinTuple *brin_copy_tuple(BrinTuple *tuple, Size len,
								  BrinTuple *dest, Size *destsz);

/*
 * Compare two on-disk BRIN tuples byte-for-byte (given their respective
 * lengths) and return true if they are identical.
 *
 * 逐字节比较两个磁盘上的 BRIN 元组（依据各自给定的长度），若二者完全
 * 相同则返回 true。
 */
extern bool brin_tuples_equal(const BrinTuple *a, Size alen,
							  const BrinTuple *b, Size blen);

/*
 * Allocate and return a new, uninitialized in-memory BrinMemTuple sized
 * according to the index descriptor, ready to be initialized and populated.
 *
 * 根据索引描述符分配并返回一个新的、未初始化的内存中 BrinMemTuple，
 * 使其准备好被初始化并填充。
 */
extern BrinMemTuple *brin_new_memtuple(BrinDesc *brdesc);

/*
 * Reset an existing in-memory BrinMemTuple to its initial empty state, wiring
 * up the per-column BrinValues arrays according to the index descriptor so it
 * can be reused for another page range.
 *
 * 将一个已存在的内存中 BrinMemTuple 重置为其初始的空状态，根据索引
 * 描述符重新组织各列的 BrinValues 数组，使其可以被复用于另一个页范围。
 */
extern BrinMemTuple *brin_memtuple_initialize(BrinMemTuple *dtuple,
											  BrinDesc *brdesc);

/*
 * Decode an on-disk BRIN tuple into an in-memory BrinMemTuple, extracting the
 * per-column values and null flags; if dMemtuple is provided it is reused as
 * the destination, otherwise a new one is allocated.
 *
 * 将一个磁盘上的 BRIN 元组解码为内存中的 BrinMemTuple，提取各列的值
 * 和空值标志；若提供了 dMemtuple 则将其复用为目标，否则分配一个新的。
 */
extern BrinMemTuple *brin_deform_tuple(BrinDesc *brdesc,
									   BrinTuple *tuple, BrinMemTuple *dMemtuple);

#endif							/* BRIN_TUPLE_H */
