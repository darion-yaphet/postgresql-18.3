/*-------------------------------------------------------------------------
 *
 * heaptoast.h
 *	  Heap-specific definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/heaptoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPTOAST_H
#define HEAPTOAST_H

#include "access/htup_details.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/*
 * Find the maximum size of a tuple if there are to be N tuples per page.
 *
 * 中文翻译：
 * 如果每页有 N 个元组，请查找元组的最大大小。
 */
#define MaximumBytesPerTuple(tuplesPerPage) \
	MAXALIGN_DOWN((BLCKSZ - \
				   MAXALIGN(SizeOfPageHeaderData + (tuplesPerPage) * sizeof(ItemIdData))) \
				  / (tuplesPerPage))

/*
 * These symbols control toaster activation.  If a tuple is larger than
 * TOAST_TUPLE_THRESHOLD, we will try to toast it down to no more than
 * TOAST_TUPLE_TARGET bytes through compressing compressible fields and
 * moving EXTENDED and EXTERNAL data out-of-line.
 *
 * The numbers need not be the same, though they currently are.  It doesn't
 * make sense for TARGET to exceed THRESHOLD, but it could be useful to make
 * it be smaller.
 *
 * Currently we choose both values to match the largest tuple size for which
 * TOAST_TUPLES_PER_PAGE tuples can fit on a heap page.
 *
 * XXX while these can be modified without initdb, some thought needs to be
 * given to needs_toast_table() in toasting.c before unleashing random
 * changes.  Also see LOBLKSIZE in large_object.h, which can *not* be
 * changed without initdb.
 *
 * 中文翻译：
 * 这些符号控制烤面包机的启动。如果元组大于 TOAST_TUPLE_T
 * HRESHOLD，我们将尝试通过压缩可压缩字段并将 EXTENDED
 *  和 EXTERNAL 数据移出线外，将其降至不超过 TOAST_T
 * UPLE_TARGET 字节。这些数字不必相同，尽管目前是相同的。
 * TARGET 超过 THRESHOLD 没有意义，但使其更小可能会很
 * 有用。目前，我们选择这两个值来匹配 TOAST_TUPLES_PER
 * _PAGE 元组可以容纳在堆页上的最大元组大小。虽然这些可以在没有
 * initdb 的情况下进行修改，但在释放随机更改之前，需要对 toa
 * sting.c 中的 need_toast_table() 进行一些
 * 思考。另请参阅large_object.h 中的LOBLKSIZE，
 * 如果没有initdb，则无法更改该值。
 */
#define TOAST_TUPLES_PER_PAGE	4

#define TOAST_TUPLE_THRESHOLD	MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE)

#define TOAST_TUPLE_TARGET		TOAST_TUPLE_THRESHOLD

/*
 * The code will also consider moving MAIN data out-of-line, but only as a
 * last resort if the previous steps haven't reached the target tuple size.
 * In this phase we use a different target size, currently equal to the
 * largest tuple that will fit on a heap page.  This is reasonable since
 * the user has told us to keep the data in-line if at all possible.
 *
 * 中文翻译：
 * 该代码还将考虑将 MAIN 数据移出线外，但只有在前面的步骤尚未达到
 * 目标元组大小时才作为最后的手段。在此阶段，我们使用不同的目标大小，当
 * 前等于堆页面上适合的最大元组。这是合理的，因为用户告诉我们尽可能保持
 * 数据在线。
 */
#define TOAST_TUPLES_PER_PAGE_MAIN	1

#define TOAST_TUPLE_TARGET_MAIN MaximumBytesPerTuple(TOAST_TUPLES_PER_PAGE_MAIN)

/*
 * If an index value is larger than TOAST_INDEX_TARGET, we will try to
 * compress it (we can't move it out-of-line, however).  Note that this
 * number is per-datum, not per-tuple, for simplicity in index_form_tuple().
 *
 * 中文翻译：
 * 如果索引值大于 TOAST_INDEX_TARGET，我们将尝试压缩
 * 它（但是我们不能将其移出线外）。请注意，为了简单起见，该数字是每个数
 * 据，而不是每个元组，在index_form_tuple()中。
 */
#define TOAST_INDEX_TARGET		(MaxHeapTupleSize / 16)

/*
 * When we store an oversize datum externally, we divide it into chunks
 * containing at most TOAST_MAX_CHUNK_SIZE data bytes.  This number *must*
 * be small enough that the completed toast-table tuple (including the
 * ID and sequence fields and all overhead) will fit on a page.
 * The coding here sets the size on the theory that we want to fit
 * EXTERN_TUPLES_PER_PAGE tuples of maximum size onto a page.
 *
 * NB: Changing TOAST_MAX_CHUNK_SIZE requires an initdb.
 *
 * 中文翻译：
 * 当我们在外部存储超大数据时，我们将其分成最多包含 TOAST_MAX
 * _CHUNK_SIZE 数据字节的块。这个数字*必须*足够小，以便完
 * 整的 toast 表元组（包括 ID 和序列字段以及所有开销）能够容
 * 纳在一个页面上。这里的编码根据我们希望将最大大小的 EXTERN_T
 * UPLES_PER_PAGE 元组放入页面的理论来设置大小。注意：更
 * 改 TOAST_MAX_CHUNK_SIZE 需要 initdb。
 */
#define EXTERN_TUPLES_PER_PAGE	4	/* tweak only this */

/* 中文翻译：只调整这个 */

#define EXTERN_TUPLE_MAX_SIZE	MaximumBytesPerTuple(EXTERN_TUPLES_PER_PAGE)

#define TOAST_MAX_CHUNK_SIZE	\
	(EXTERN_TUPLE_MAX_SIZE -							\
	 MAXALIGN(SizeofHeapTupleHeader) -					\
	 sizeof(Oid) -										\
	 sizeof(int32) -									\
	 VARHDRSZ)

/* ----------
 * heap_toast_insert_or_update -
 *
 *	Called by heap_insert() and heap_update().
 * ----------
 *
 * 中文翻译：
 * heap_toast_insert_or_update - 由 he
 * ap_insert() 和 heap_update() 调用。
 */
/*
 * Function heap_toast_insert_or_update constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_toast_insert_or_update通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern HeapTuple heap_toast_insert_or_update(Relation rel, HeapTuple newtup,
											 HeapTuple oldtup, int options);

/* ----------
 * heap_toast_delete -
 *
 *	Called by heap_delete().
 * ----------
 *
 * 中文翻译：
 * heap_toast_delete - 由 heap_delete(
 * ) 调用。
 */
/*
 * Function heap_toast_delete constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_toast_delete通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void heap_toast_delete(Relation rel, HeapTuple oldtup,
							  bool is_speculative);

/* ----------
 * toast_flatten_tuple -
 *
 *	"Flatten" a tuple to contain no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 * ----------
 *
 * 中文翻译：
 * toast_flatten_tuple - “压平”元组以包含不外线
 * 的烤字段。 （这不会消除压缩或短标头数据。）
 */
/*
 * Function toast_flatten_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_flatten_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern HeapTuple toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc);

/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	"Flatten" a tuple containing out-of-line toasted fields into a Datum.
 * ----------
 *
 * 中文翻译：
 * toast_flatten_tuple_to_datum - 将包含
 * 外线烤字段的元组“扁平化”为数据。
 */
/*
 * Function toast_flatten_tuple_to_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_flatten_tuple_to_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern Datum toast_flatten_tuple_to_datum(HeapTupleHeader tup,
										  uint32 tup_len,
										  TupleDesc tupleDesc);

/* ----------
 * toast_build_flattened_tuple -
 *
 *	Build a tuple containing no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 * ----------
 *
 * 中文翻译：
 * toast_build_flattened_tuple - 构建一个
 * 不包含任何外线 toasted 字段的元组。 （这不会消除压缩或短标
 * 头数据。）
 */
/*
 * Function toast_build_flattened_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_build_flattened_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern HeapTuple toast_build_flattened_tuple(TupleDesc tupleDesc,
											 Datum *values,
											 bool *isnull);

/* ----------
 * heap_fetch_toast_slice
 *
 *	Fetch a slice from a toast value stored in a heap table.
 * ----------
 *
 * 中文翻译：
 * heap_fetch_toast_slice 从存储在堆表中的 to
 * ast 值中获取切片。
 */
/*
 * Function heap_fetch_toast_slice retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_fetch_toast_slice通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void heap_fetch_toast_slice(Relation toastrel, Oid valueid,
								   int32 attrsize, int32 sliceoffset,
								   int32 slicelength, struct varlena *result);

#endif							/* HEAPTOAST_H */

/* 中文翻译：HEAPTOAST_H */
