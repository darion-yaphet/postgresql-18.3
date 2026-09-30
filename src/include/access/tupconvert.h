/*-------------------------------------------------------------------------
 *
 * tupconvert.h
 *	  Tuple conversion support.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupconvert.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * tupconvert.h 元组转换支持。 src/include/a
 * ccess/tupconvert.h
 */
#ifndef TUPCONVERT_H
#define TUPCONVERT_H

#include "access/attmap.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"


typedef struct TupleConversionMap
{
	TupleDesc	indesc;			/* tupdesc for source rowtype */

	/* 中文翻译：tupdesc 用于源行类型 */
	TupleDesc	outdesc;		/* tupdesc for result rowtype */

	/* 中文翻译：tupdesc 用于结果行类型 */
	AttrMap    *attrMap;		/* indexes of input fields, or 0 for null */

	/* 中文翻译：输入字段的索引，或 0 表示 null */
	Datum	   *invalues;		/* workspace for deconstructing source */

	/* 中文翻译：解构源代码的工作区 */
	bool	   *inisnull;
	Datum	   *outvalues;		/* workspace for constructing result */

	/* 中文翻译：构建结果的工作区 */
	bool	   *outisnull;
} TupleConversionMap;


/*
 * Function convert_tuples_by_position converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 convert_tuples_by_position通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern TupleConversionMap *convert_tuples_by_position(TupleDesc indesc,
													  TupleDesc outdesc,
													  const char *msg);

/*
 * Function convert_tuples_by_name converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 convert_tuples_by_name通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern TupleConversionMap *convert_tuples_by_name(TupleDesc indesc,
												  TupleDesc outdesc);
/*
 * Function convert_tuples_by_name_attrmap converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 convert_tuples_by_name_attrmap通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern TupleConversionMap *convert_tuples_by_name_attrmap(TupleDesc indesc,
														  TupleDesc outdesc,
														  AttrMap *attrMap);

/*
 * Function execute_attr_map_tuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 execute_attr_map_tuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern HeapTuple execute_attr_map_tuple(HeapTuple tuple, TupleConversionMap *map);
/*
 * Function execute_attr_map_slot converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 execute_attr_map_slot通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern TupleTableSlot *execute_attr_map_slot(AttrMap *attrMap,
											 TupleTableSlot *in_slot,
											 TupleTableSlot *out_slot);
/*
 * Function execute_attr_map_cols converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 execute_attr_map_cols通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern Bitmapset *execute_attr_map_cols(AttrMap *attrMap, Bitmapset *in_cols);

/*
 * Function free_conversion_map completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 free_conversion_map在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void free_conversion_map(TupleConversionMap *map);

#endif							/* TUPCONVERT_H */

/* 中文翻译：TUPCONVERT_H */
