/*-------------------------------------------------------------------------
 *
 * itup.h
 *	  POSTGRES index tuple definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/itup.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * itup.h POSTGRES 索引元组定义。 src/includ
 * e/access/itup.h
 */
#ifndef ITUP_H
#define ITUP_H

#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"

/*
 * Index tuple header structure
 *
 * All index tuples start with IndexTupleData.  If the HasNulls bit is set,
 * this is followed by an IndexAttributeBitMapData.  The index attribute
 * values follow, beginning at a MAXALIGN boundary.
 *
 * Note that the space allocated for the bitmap does not vary with the number
 * of attributes; that is because we don't have room to store the number of
 * attributes in the header.  Given the MAXALIGN constraint there's no space
 * savings to be had anyway, for usual values of INDEX_MAX_KEYS.
 *
 * 中文翻译：
 * 索引元组头结构 所有索引元组都以 IndexTupleData 开头
 * 。如果设置了 HasNulls 位，则后面跟着 IndexAttri
 * buteBitMapData。接下来是索引属性值，从 MAXALIG
 * N 边界开始。注意，为位图分配的空间不随属性数量的变化而变化；这是因
 * 为我们没有足够的空间来存储标头中的属性数量。考虑到 MAXALIGN
 *  约束，对于 INDEX_MAX_KEYS 的通常值来说，无论如何都
 * 不会节省空间。
 */

typedef struct IndexTupleData
{
	ItemPointerData t_tid;		/* reference TID to heap tuple */

	/* 中文翻译：引用 TID 到堆元组 */

	/* ---------------
	 * t_info is laid out in the following fashion:
	 *
	 * 15th (high) bit: has nulls
	 * 14th bit: has var-width attributes
	 * 13th bit: AM-defined meaning
	 * 12-0 bit: size of tuple
	 * ---------------
	 *
	 * 中文翻译：
	 * t_info 按以下方式布局： 第 15 位（高位）：具有空值 第
	 * 14 位：具有 var-width 属性 第 13 位：AM 定义的
	 * 含义 第 12-0 位：元组的大小
	 */

	unsigned short t_info;		/* various info about tuple */

	/* 中文翻译：有关元组的各种信息 */

} IndexTupleData;				/* MORE DATA FOLLOWS AT END OF STRUCT */

/* 中文翻译：结构体末尾有更多数据 */

typedef IndexTupleData *IndexTuple;

typedef struct IndexAttributeBitMapData
{
	bits8		bits[(INDEX_MAX_KEYS + 8 - 1) / 8];
}			IndexAttributeBitMapData;

typedef IndexAttributeBitMapData * IndexAttributeBitMap;

/*
 * t_info manipulation macros
 *
 * 中文翻译：
 * t_info 操作宏
 */
#define INDEX_SIZE_MASK 0x1FFF
#define INDEX_AM_RESERVED_BIT 0x2000	/* reserved for index-AM specific
										 * usage */

/* 中文翻译：
 * 保留用于索引 AM 特定用途
 */
#define INDEX_VAR_MASK	0x4000
#define INDEX_NULL_MASK 0x8000

/*
 * Function IndexTupleSize carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 IndexTupleSize通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline Size
IndexTupleSize(const IndexTupleData *itup)
{
	return (itup->t_info & INDEX_SIZE_MASK);
}

/*
 * Function IndexTupleHasNulls evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 IndexTupleHasNulls通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
IndexTupleHasNulls(const IndexTupleData *itup)
{
	return itup->t_info & INDEX_NULL_MASK;
}

/*
 * Function IndexTupleHasVarwidths evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 IndexTupleHasVarwidths通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
IndexTupleHasVarwidths(const IndexTupleData *itup)
{
	return itup->t_info & INDEX_VAR_MASK;
}


/* routines in indextuple.c */

/* 中文翻译：Indextuple.c 中的例程 */
/*
 * Function index_form_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 index_form_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern IndexTuple index_form_tuple(TupleDesc tupleDescriptor,
								   const Datum *values, const bool *isnull);
/*
 * Function index_form_tuple_context constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 index_form_tuple_context通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern IndexTuple index_form_tuple_context(TupleDesc tupleDescriptor,
										   const Datum *values, const bool *isnull,
										   MemoryContext context);
/*
 * Function nocache_index_getattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 nocache_index_getattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Datum nocache_index_getattr(IndexTuple tup, int attnum,
								   TupleDesc tupleDesc);
/*
 * Function index_deform_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 index_deform_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void index_deform_tuple(IndexTuple tup, TupleDesc tupleDescriptor,
							   Datum *values, bool *isnull);
/*
 * Function index_deform_tuple_internal constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 index_deform_tuple_internal通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void index_deform_tuple_internal(TupleDesc tupleDescriptor,
										Datum *values, bool *isnull,
										char *tp, bits8 *bp, int hasnulls);
/*
 * Function CopyIndexTuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 CopyIndexTuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern IndexTuple CopyIndexTuple(IndexTuple source);
/*
 * Function index_truncate_tuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 index_truncate_tuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern IndexTuple index_truncate_tuple(TupleDesc sourceDescriptor,
									   IndexTuple source, int leavenatts);


/*
 * Takes an infomask as argument (primarily because this needs to be usable
 * at index_form_tuple time so enough space is allocated).
 *
 * 中文翻译：
 * 采用 infomask 作为参数（主要是因为这需要在 index_f
 * orm_tuple 时间可用，以便分配足够的空间）。
 */
/*
 * Function IndexInfoFindDataOffset retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 IndexInfoFindDataOffset通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline Size
IndexInfoFindDataOffset(unsigned short t_info)
{
	if (!(t_info & INDEX_NULL_MASK))
		return MAXALIGN(sizeof(IndexTupleData));
	else
		return MAXALIGN(sizeof(IndexTupleData) + sizeof(IndexAttributeBitMapData));
}

#ifndef FRONTEND

/* ----------------
 *		index_getattr
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call nocache_index_getattr() for the rest.
 *
 * ----------------
 *
 * 中文翻译：
 * index_getattr 这会被调用很多次，因此我们宏化可缓存和
 * NULL 查找，并调用 nocache_index_getattr(
 * ) 来完成其余的操作。
 */
/*
 * Function index_getattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 index_getattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline Datum
index_getattr(IndexTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(PointerIsValid(isnull));
	Assert(attnum > 0);

	*isnull = false;

	if (!IndexTupleHasNulls(tup))
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupleDesc, attnum - 1);

		if (attr->attcacheoff >= 0)
		{
			return fetchatt(attr,
							(char *) tup + IndexInfoFindDataOffset(tup->t_info) +
							attr->attcacheoff);
		}
		else
			return nocache_index_getattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, (bits8 *) tup + sizeof(IndexTupleData)))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocache_index_getattr(tup, attnum, tupleDesc);
	}
}

#endif

/*
 * MaxIndexTuplesPerPage is an upper bound on the number of tuples that can
 * fit on one index page.  An index tuple must have either data or a null
 * bitmap, so we can safely assume it's at least 1 byte bigger than a bare
 * IndexTupleData struct.  We arrive at the divisor because each tuple
 * must be maxaligned, and it must have an associated line pointer.
 *
 * To be index-type-independent, this does not account for any special space
 * on the page, and is thus conservative.
 *
 * Note: in btree non-leaf pages, the first tuple has no key (it's implicitly
 * minus infinity), thus breaking the "at least 1 byte bigger" assumption.
 * On such a page, N tuples could take one MAXALIGN quantum less space than
 * estimated here, seemingly allowing one more tuple than estimated here.
 * But such a page always has at least MAXALIGN special space, so we're safe.
 *
 * 中文翻译：
 * MaxIndexTuplesPerPage 是一个索引页上可以容纳的
 * 元组数量的上限。索引元组必须具有数据或空位图，因此我们可以安全地假设
 * 它至少比裸 IndexTupleData 结构大 1 个字节。我们得
 * 到除数是因为每个元组必须是最大对齐的，并且它必须有一个关联的行指针。
 * 为了与索引类型无关，这不会占用页面上的任何特殊空间，因此是保守的。注
 * 意：在 btree 非叶页中，第一个元组没有键（它隐式地负无穷大），
 * 从而打破了“至少大 1 个字节”的假设。在这样的页面上，N 个元组占
 * 用的空间可能比此处估计的少 1 个 MAXALIGN 量子，看起来比
 * 此处估计的多一个元组。但这样的页面总是至少有 MAXALIGN 特殊
 * 空间，所以我们是安全的。
 */
#define MaxIndexTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(sizeof(IndexTupleData) + 1) + sizeof(ItemIdData))))

#endif							/* ITUP_H */

/* 中文翻译：ITU_H */
