/*-------------------------------------------------------------------------
 *
 * htup.h
 *	  POSTGRES heap tuple definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * htup.h POSTGRES 堆元组定义。 src/include
 * /access/htup.h
 */
#ifndef HTUP_H
#define HTUP_H

#include "storage/itemptr.h"

/* typedefs and forward declarations for structs defined in htup_details.h */

/* 中文翻译：htup_details.h 中定义的结构的 typedef 和前向声明 */

typedef struct HeapTupleHeaderData HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

typedef struct MinimalTupleData MinimalTupleData;

typedef MinimalTupleData *MinimalTuple;


/*
 * HeapTupleData is an in-memory data structure that points to a tuple.
 *
 * There are several ways in which this data structure is used:
 *
 * * Pointer to a tuple in a disk buffer: t_data points directly into the
 *	 buffer (which the code had better be holding a pin on, but this is not
 *	 reflected in HeapTupleData itself).
 *
 * * Pointer to nothing: t_data is NULL.  This is used as a failure indication
 *	 in some functions.
 *
 * * Part of a palloc'd tuple: the HeapTupleData itself and the tuple
 *	 form a single palloc'd chunk.  t_data points to the memory location
 *	 immediately following the HeapTupleData struct (at offset HEAPTUPLESIZE).
 *	 This is the output format of heap_form_tuple and related routines.
 *
 * * Separately allocated tuple: t_data points to a palloc'd chunk that
 *	 is not adjacent to the HeapTupleData.  (This case is deprecated since
 *	 it's difficult to tell apart from case #1.  It should be used only in
 *	 limited contexts where the code knows that case #1 will never apply.)
 *
 * * Separately allocated minimal tuple: t_data points MINIMAL_TUPLE_OFFSET
 *	 bytes before the start of a MinimalTuple.  As with the previous case,
 *	 this can't be told apart from case #1 by inspection; code setting up
 *	 or destroying this representation has to know what it's doing.
 *
 * t_len should always be valid, except in the pointer-to-nothing case.
 * t_self and t_tableOid should be valid if the HeapTupleData points to
 * a disk buffer, or if it represents a copy of a tuple on disk.  They
 * should be explicitly set invalid in manufactured tuples.
 *
 * 中文翻译：
 * HeapTupleData 是一个指向元组的内存数据结构。该数据结构
 * 有多种使用方式： * 指向磁盘缓冲区中元组的指针：t_data 直接
 * 指向缓冲区（代码最好将其固定在缓冲区上，但这不会反映在 HeapTu
 * pleData 本身中）。 * 没有指向任何内容的指针：t_data
 *  为 NULL。这在某些功能中用作故障指示。 * palloc'd
 * 元组的一部分：HeapTupleData 本身和元组形成单个 pal
 * loc'd 块。 t_data 指向紧跟在 HeapTupleDat
 * a 结构之后的内存位置（位于偏移量 HEAPTUPLESIZE 处）
 * 。这是heap_form_tuple及相关例程的输出格式。 * 单独
 * 分配的元组：t_data 指向与 HeapTupleData 不相邻
 * 的 palloc'd chunk。 （这种情况已被弃用，因为很难将其
 * 与情况 #1 区分开。它应该仅在代码知道情况 #1 永远不会适用的有
 * 限上下文中使用。） * 单独分配的最小元组：t_data 在 Min
 * imalTuple 开始之前指向 MINIMAL_TUPLE_OFF
 * SET 字节。与前一个案例一样，通过检查无法将其与案例 1 区分开来
 * ；设置或销毁此表示的代码必须知道它在做什么。 t_len 应始终有效
 * ，除了指向无指针的情况。如果 HeapTupleData 指向磁盘缓
 * 冲区，或者它表示磁盘上元组的副本，则 t_self 和 t_tabl
 * eOid 应该有效。它们应该在制造的元组中明确设置为无效。
 */
typedef struct HeapTupleData
{
	uint32		t_len;			/* length of *t_data */

	/* 中文翻译：*t_data 的长度 */
	ItemPointerData t_self;		/* SelfItemPointer */

	/* 中文翻译：自项目指针 */
	Oid			t_tableOid;		/* table the tuple came from */

	/* 中文翻译：元组来自的表 */
#define FIELDNO_HEAPTUPLEDATA_DATA 3
	HeapTupleHeader t_data;		/* -> tuple header and data */

	/* 中文翻译：-> 元组头和数据 */
} HeapTupleData;

typedef HeapTupleData *HeapTuple;

#define HEAPTUPLESIZE	MAXALIGN(sizeof(HeapTupleData))

/*
 * Accessor macros to be used with HeapTuple pointers.
 *
 * 中文翻译：
 * 与 HeapTuple 指针一起使用的访问器宏。
 */
#define HeapTupleIsValid(tuple) PointerIsValid(tuple)

/* HeapTupleHeader functions implemented in utils/time/combocid.c */

/* 中文翻译：HeapTupleHeader 函数在 utils/time/combocid.c 中实现 */
/*
 * Function HeapTupleHeaderGetCmin retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetCmin通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern CommandId HeapTupleHeaderGetCmin(const HeapTupleHeaderData *tup);
/*
 * Function HeapTupleHeaderGetCmax retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetCmax通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern CommandId HeapTupleHeaderGetCmax(const HeapTupleHeaderData *tup);
/*
 * Function HeapTupleHeaderAdjustCmax updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderAdjustCmax通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void HeapTupleHeaderAdjustCmax(const HeapTupleHeaderData *tup,
									  CommandId *cmax, bool *iscombo);

/* Prototype for HeapTupleHeader accessors in heapam.c */

/* 中文翻译：heapam.c 中 HeapTupleHeader 访问器的原型 */
/*
 * Function HeapTupleGetUpdateXid retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleGetUpdateXid通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern TransactionId HeapTupleGetUpdateXid(const HeapTupleHeaderData *tup);

#endif							/* HTUP_H */

/* 中文翻译：HTUP_H */
