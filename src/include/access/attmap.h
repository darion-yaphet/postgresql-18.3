/*-------------------------------------------------------------------------
 *
 * attmap.h
 *	  Definitions for PostgreSQL attribute mappings
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/attmap.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * attmap.h PostgreSQL 属性映射的定义 src/in
 * clude/access/attmap.h
 */
#ifndef ATTMAP_H
#define ATTMAP_H

#include "access/attnum.h"
#include "access/tupdesc.h"

/*
 * Attribute mapping structure
 *
 * This maps attribute numbers between a pair of relations, designated
 * 'input' and 'output' (most typically inheritance parent and child
 * relations), whose common columns may have different attribute numbers.
 * Such difference may arise due to the columns being ordered differently
 * in the two relations or the two relations having dropped columns at
 * different positions.
 *
 * 'maplen' is set to the number of attributes of the 'output' relation,
 * taking into account any of its dropped attributes, with the corresponding
 * elements of the 'attnums' array set to 0.
 *
 * 中文翻译：
 * 属性映射结构 这在指定为“输入”和“输出”的一对关系之间映射属性编号
 * （最典型的是继承父关系和子关系），其公共列可以具有不同的属性编号。这
 * 种差异可能是由于两个关系中列的排序不同或者两个关系在不同位置删除了列
 * 而引起的。 'maplen' 设置为 'output' 关系的属性数
 * 量，考虑到其删除的任何属性，并将 'attnums' 数组的相应元素
 * 设置为 0。
 */
typedef struct AttrMap
{
	AttrNumber *attnums;
	int			maplen;
} AttrMap;

/*
 * Function make_attrmap converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 make_attrmap通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern AttrMap *make_attrmap(int maplen);
/*
 * Function free_attrmap completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 free_attrmap在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void free_attrmap(AttrMap *map);

/* Conversion routines to build mappings */

/* 中文翻译：用于构建映射的转换例程 */
/*
 * Function build_attrmap_by_name constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 build_attrmap_by_name通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern AttrMap *build_attrmap_by_name(TupleDesc indesc,
									  TupleDesc outdesc,
									  bool missing_ok);
/*
 * Function build_attrmap_by_name_if_req constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 build_attrmap_by_name_if_req通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern AttrMap *build_attrmap_by_name_if_req(TupleDesc indesc,
											 TupleDesc outdesc,
											 bool missing_ok);
/*
 * Function build_attrmap_by_position constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 build_attrmap_by_position通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern AttrMap *build_attrmap_by_position(TupleDesc indesc,
										  TupleDesc outdesc,
										  const char *msg);

#endif							/* ATTMAP_H */

/* 中文翻译：ATTMAP_H */
