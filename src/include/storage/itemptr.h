/*-------------------------------------------------------------------------
 *
 * itemptr.h
 *	  POSTGRES disk item pointer definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemptr.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * PostgreSQL 磁盘项指针定义。
 */
#ifndef ITEMPTR_H
#define ITEMPTR_H

#include "storage/block.h"
#include "storage/off.h"

/*
 * ItemPointer:
 *
 * This is a pointer to an item within a disk page of a known file
 * (for example, a cross-link from an index to its parent table).
 * ip_blkid tells us which block, ip_posid tells us which entry in
 * the linp (ItemIdData) array we want.
 *
 * Note: because there is an item pointer in each tuple header and index
 * tuple header on disk, it's very important not to waste space with
 * structure padding bytes.  The struct is designed to be six bytes long
 * (it contains three int16 fields) but a few compilers will pad it to
 * eight bytes unless coerced.  We apply appropriate persuasion where
 * possible.  If your compiler can't be made to play along, you'll waste
 * lots of space.
 */

/*
 * ItemPointer 是已知文件中磁盘页内某项的指针，例如从索引到其父表的交叉链接。ip_blkid 指明块，
 * ip_posid 指明所需 linp（ItemIdData）数组条目。
 * 由于每个元组头和磁盘索引元组头中都有项指针，因此不得浪费结构填充字节。该结构设计为六字节长
 * （包含三个 int16 字段），但少数编译器会将其填充为八字节，除非施加适当约束。如编译器无法配合，
 * 将浪费大量空间。
 */
typedef struct ItemPointerData
{
	BlockIdData ip_blkid;
	OffsetNumber ip_posid;
}

/* If compiler understands packed and aligned pragmas, use those */

/*
 * 如果编译器理解 packed 和 aligned pragma，则使用它们。
 */
#if defined(pg_attribute_packed) && defined(pg_attribute_aligned)
			pg_attribute_packed()
			pg_attribute_aligned(2)
#endif
ItemPointerData;

typedef ItemPointerData *ItemPointer;

/* ----------------
 *		special values used in heap tuples (t_ctid)
 * ----------------
 */

/*
 * If a heap tuple holds a speculative insertion token rather than a real
 * TID, ip_posid is set to SpecTokenOffsetNumber, and the token is stored in
 * ip_blkid. SpecTokenOffsetNumber must be higher than MaxOffsetNumber, so
 * that it can be distinguished from a valid offset number in a regular item
 * pointer.
 */

/*
 * 如果堆元组持有的是推测插入令牌而非真实 TID，则 ip_posid 设为 SpecTokenOffsetNumber，令牌存储在
 * ip_blkid 中。SpecTokenOffsetNumber 必须大于 MaxOffsetNumber，以便与常规项指针中的有效偏移量区分。
 */
#define SpecTokenOffsetNumber		0xfffe

/*
 * When a tuple is moved to a different partition by UPDATE, the t_ctid of
 * the old tuple version is set to this magic value.
 */

/*
 * UPDATE 将元组移到不同分区时，旧元组版本的 t_ctid 会设为此魔数。
 */
#define MovedPartitionsOffsetNumber 0xfffd
#define MovedPartitionsBlockNumber	InvalidBlockNumber


/* ----------------
 *		support functions
 * ----------------
 */

/*
 * ItemPointerIsValid
 *		True iff the disk item pointer is not NULL.
 */

/*
 * 当且仅当磁盘项指针非 NULL 且偏移号非零时为真。
 */
static inline bool
ItemPointerIsValid(const ItemPointerData *pointer)
{
	return PointerIsValid(pointer) && pointer->ip_posid != 0;
}

/*
 * ItemPointerGetBlockNumberNoCheck
 *		Returns the block number of a disk item pointer.
 */

/*
 * 返回磁盘项指针的块号，不执行有效性检查。
 */
static inline BlockNumber
ItemPointerGetBlockNumberNoCheck(const ItemPointerData *pointer)
{
	return BlockIdGetBlockNumber(&pointer->ip_blkid);
}

/*
 * ItemPointerGetBlockNumber
 *		As above, but verifies that the item pointer looks valid.
 */

/*
 * 与上例相同，但会断言项指针看起来有效。
 */
static inline BlockNumber
ItemPointerGetBlockNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetBlockNumberNoCheck(pointer);
}

/*
 * ItemPointerGetOffsetNumberNoCheck
 *		Returns the offset number of a disk item pointer.
 */

/*
 * 返回磁盘项指针的偏移号，不执行有效性检查。
 */
static inline OffsetNumber
ItemPointerGetOffsetNumberNoCheck(const ItemPointerData *pointer)
{
	return pointer->ip_posid;
}

/*
 * ItemPointerGetOffsetNumber
 *		As above, but verifies that the item pointer looks valid.
 */

/*
 * 与上例相同，但会断言项指针看起来有效。
 */
static inline OffsetNumber
ItemPointerGetOffsetNumber(const ItemPointerData *pointer)
{
	Assert(ItemPointerIsValid(pointer));
	return ItemPointerGetOffsetNumberNoCheck(pointer);
}

/*
 * ItemPointerSet
 *		Sets a disk item pointer to the specified block and offset.
 */

/*
 * 将磁盘项指针设置为指定的块和偏移量。
 */
static inline void
ItemPointerSet(ItemPointerData *pointer, BlockNumber blockNumber, OffsetNumber offNum)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
	pointer->ip_posid = offNum;
}

/*
 * ItemPointerSetBlockNumber
 *		Sets a disk item pointer to the specified block.
 */

/*
 * 将磁盘项指针设置为指定的块。
 */
static inline void
ItemPointerSetBlockNumber(ItemPointerData *pointer, BlockNumber blockNumber)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, blockNumber);
}

/*
 * ItemPointerSetOffsetNumber
 *		Sets a disk item pointer to the specified offset.
 */

/*
 * 将磁盘项指针设置为指定的偏移量。
 */
static inline void
ItemPointerSetOffsetNumber(ItemPointerData *pointer, OffsetNumber offsetNumber)
{
	Assert(PointerIsValid(pointer));
	pointer->ip_posid = offsetNumber;
}

/*
 * ItemPointerCopy
 *		Copies the contents of one disk item pointer to another.
 *
 * Should there ever be padding in an ItemPointer this would need to be handled
 * differently as it's used as hash key.
 */

/*
 * 将一个磁盘项指针的内容复制到另一个。若 ItemPointer 中曾出现填充字节，则由于它被用作哈希键，
 * 此处需要采用不同的处理方式。
 */
static inline void
ItemPointerCopy(const ItemPointerData *fromPointer, ItemPointerData *toPointer)
{
	Assert(PointerIsValid(toPointer));
	Assert(PointerIsValid(fromPointer));
	*toPointer = *fromPointer;
}

/*
 * ItemPointerSetInvalid
 *		Sets a disk item pointer to be invalid.
 */

/*
 * 将磁盘项指针设为无效。
 */
static inline void
ItemPointerSetInvalid(ItemPointerData *pointer)
{
	Assert(PointerIsValid(pointer));
	BlockIdSet(&pointer->ip_blkid, InvalidBlockNumber);
	pointer->ip_posid = InvalidOffsetNumber;
}

/*
 * ItemPointerIndicatesMovedPartitions
 *		True iff the block number indicates the tuple has moved to another
 *		partition.
 */

/*
 * 当且仅当块号表示元组已移至另一分区时为真。
 */
static inline bool
ItemPointerIndicatesMovedPartitions(const ItemPointerData *pointer)
{
	return
		ItemPointerGetOffsetNumber(pointer) == MovedPartitionsOffsetNumber &&
		ItemPointerGetBlockNumberNoCheck(pointer) == MovedPartitionsBlockNumber;
}

/*
 * ItemPointerSetMovedPartitions
 *		Indicate that the item referenced by the itempointer has moved into a
 *		different partition.
 */

/*
 * 标示 itempointer 引用的项已经移至不同分区。
 */
static inline void
ItemPointerSetMovedPartitions(ItemPointerData *pointer)
{
	ItemPointerSet(pointer, MovedPartitionsBlockNumber, MovedPartitionsOffsetNumber);
}

/* ----------------
 *		externs
 * ----------------
 */

/*
 * 外部函数。
 */

/*
 * ItemPointerSet
 *		Sets a disk item pointer to the specified block and offset.
 */

/*
 * 测试两个项指针是否标识同一块和偏移量。
 */
extern bool ItemPointerEquals(ItemPointer pointer1, ItemPointer pointer2);
/*
 * Orders two item pointers by block and then offset.
 */

/*
 * 按块号再按偏移量比较两个项指针。
 */
extern int32 ItemPointerCompare(ItemPointer arg1, ItemPointer arg2);
/*
 * Advances an item pointer to the next offset position.
 */

/*
 * 将项指针推进到下一个偏移位置。
 */
extern void ItemPointerInc(ItemPointer pointer);
/*
 * Moves an item pointer back to the preceding offset position.
 */

/*
 * 将项指针移回前一个偏移位置。
 */
extern void ItemPointerDec(ItemPointer pointer);

/* ----------------
 *		Datum conversion functions
 * ----------------
 */

/*
 * Datum 转换函数。
 */

/*
 * Converts a Datum containing a pointer into an ItemPointer.
 */

/*
 * 将包含指针的 Datum 转换为 ItemPointer。
 */
static inline ItemPointer
DatumGetItemPointer(Datum X)
{
	return (ItemPointer) DatumGetPointer(X);
}

/*
 * Converts an ItemPointer address into a Datum.
 */

/*
 * 将 ItemPointer 地址转换为 Datum。
 */
static inline Datum
ItemPointerGetDatum(const ItemPointerData *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_ITEMPOINTER(n) DatumGetItemPointer(PG_GETARG_DATUM(n))
#define PG_RETURN_ITEMPOINTER(x) return ItemPointerGetDatum(x)

#endif							/* ITEMPTR_H */
