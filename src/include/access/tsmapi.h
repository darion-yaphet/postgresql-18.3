/*-------------------------------------------------------------------------
 *
 * tsmapi.h
 *	  API for tablesample methods
 *
 * Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 * src/include/access/tsmapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TSMAPI_H
#define TSMAPI_H

#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"


/*
 * Callback function signatures --- see tablesample-method.sgml for more info.
 *
 * 中文翻译：
 * 回调函数签名 --- 请参阅 tablesample-method.
 * sgml 了解更多信息。
 */

typedef void (*SampleScanGetSampleSize_function) (PlannerInfo *root,
												  RelOptInfo *baserel,
												  List *paramexprs,
												  BlockNumber *pages,
												  double *tuples);

typedef void (*InitSampleScan_function) (SampleScanState *node,
										 int eflags);

typedef void (*BeginSampleScan_function) (SampleScanState *node,
										  Datum *params,
										  int nparams,
										  uint32 seed);

typedef BlockNumber (*NextSampleBlock_function) (SampleScanState *node,
												 BlockNumber nblocks);

typedef OffsetNumber (*NextSampleTuple_function) (SampleScanState *node,
												  BlockNumber blockno,
												  OffsetNumber maxoffset);

typedef void (*EndSampleScan_function) (SampleScanState *node);

/*
 * TsmRoutine is the struct returned by a tablesample method's handler
 * function.  It provides pointers to the callback functions needed by the
 * planner and executor, as well as additional information about the method.
 *
 * More function pointers are likely to be added in the future.
 * Therefore it's recommended that the handler initialize the struct with
 * makeNode(TsmRoutine) so that all fields are set to NULL.  This will
 * ensure that no fields are accidentally left undefined.
 *
 * 中文翻译：
 * TsmRoutine 是 tablesample 方法的处理函数返回
 * 的结构。它提供了计划器和执行器所需的回调函数的指针，以及有关该方法的
 * 附加信息。将来可能会添加更多函数指针。因此，建议处理程序使用 mak
 * eNode(TsmRoutine) 初始化结构，以便将所有字段设置为
 *  NULL。这将确保没有字段意外地未定义。
 */
typedef struct TsmRoutine
{
	NodeTag		type;

	/* List of datatype OIDs for the arguments of the TABLESAMPLE clause */

	/* 中文翻译：TABLESAMPLE 子句的参数的数据类型 OID 列表 */
	List	   *parameterTypes;

	/* Can method produce repeatable samples across, or even within, queries? */

	/* 中文翻译：方法可以跨查询甚至在查询内生成可重复的样本吗？ */
	bool		repeatable_across_queries;
	bool		repeatable_across_scans;

	/* Functions for planning a SampleScan on a physical table */

	/* 中文翻译：用于在物理表上规划 SampleScan 的函数 */
	SampleScanGetSampleSize_function SampleScanGetSampleSize;

	/* Functions for executing a SampleScan on a physical table */

	/* 中文翻译：用于在物理表上执行 SampleScan 的函数 */
	InitSampleScan_function InitSampleScan; /* can be NULL */

	/* 中文翻译：可以为 NULL */
	BeginSampleScan_function BeginSampleScan;
	NextSampleBlock_function NextSampleBlock;	/* can be NULL */

	/* 中文翻译：可以为 NULL */
	NextSampleTuple_function NextSampleTuple;
	EndSampleScan_function EndSampleScan;	/* can be NULL */

	/* 中文翻译：可以为 NULL */
} TsmRoutine;


/* Functions in access/tablesample/tablesample.c */

/* 中文翻译：access/tablesample/tablesample.c 中的函数 */
/*
 * Function GetTsmRoutine retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 GetTsmRoutine通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern TsmRoutine *GetTsmRoutine(Oid tsmhandler);

#endif							/* TSMAPI_H */

/* 中文翻译：TSMAPI_H */
