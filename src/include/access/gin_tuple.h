/*--------------------------------------------------------------------------
 * gin.h
 *	  Public header file for Generalized Inverted Index access method.
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/gin.h
 *--------------------------------------------------------------------------
 */
#ifndef GIN_TUPLE_H
#define GIN_TUPLE_H

#include "access/ginblock.h"
#include "storage/itemptr.h"
#include "utils/sortsupport.h"

/*
 * Data for one key in a GIN index.
 */

/*
 * GIN 索引中一个键（key）所对应的数据。
 */
typedef struct GinTuple
{
	int			tuplen;			/* length of the whole tuple */

	/* 整个元组的长度 */
	OffsetNumber attrnum;		/* attnum of index key */

	/* 索引键的属性编号（attnum） */
	uint16		keylen;			/* bytes in data for key value */

	/* data 中键值所占的字节数 */
	int16		typlen;			/* typlen for key */

	/* 键的类型长度（typlen） */
	bool		typbyval;		/* typbyval for key */

	/* 键的按值传递标志（typbyval） */
	signed char category;		/* category: normal or NULL? */

	/* 类别：普通值还是 NULL？ */
	int			nitems;			/* number of TIDs in the data */

	/* data 中 TID 的数量 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} GinTuple;

/*
 * Return a pointer to the first item pointer stored in the compressed posting
 * list of a GinTuple.  It locates the posting list by skipping past the key
 * value and applying short alignment, then yields the address of its first TID.
 *
 * 返回指向 GinTuple 压缩倒排列表中第一个项指针的指针。
 * 它通过跳过键值并进行短对齐来定位倒排列表，
 * 然后给出其第一个 TID 的地址。
 */
static inline ItemPointer
GinTupleGetFirst(GinTuple *tup)
{
	GinPostingList *list;

	list = (GinPostingList *) SHORTALIGN(tup->data + tup->keylen);

	return &list->first;
}

/*
 * Compare two GinTuples for sorting during parallel GIN index build, using the
 * provided sort support to order first by attribute number and then by key
 * value, so that entries for the same key become adjacent and can be merged.
 *
 * 在并行 GIN 索引构建期间比较两个 GinTuple 以进行排序，
 * 使用所提供的排序支持先按属性编号、再按键值排序，
 * 从而使同一键的条目相邻并可被合并。
 */
extern int	_gin_compare_tuples(GinTuple *a, GinTuple *b, SortSupport ssup);

#endif							/* GIN_TUPLE_H */
