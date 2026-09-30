/*-------------------------------------------------------------------------
 *
 * table.h
 *	  Generic routines for table related code.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/table.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * table.h 表相关代码的通用例程。 src/include/ac
 * cess/table.h
 */
#ifndef TABLE_H
#define TABLE_H

#include "nodes/primnodes.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/*
 * Function table_open carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 table_open通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Relation table_open(Oid relationId, LOCKMODE lockmode);
/*
 * Function table_openrv carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 table_openrv通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Relation table_openrv(const RangeVar *relation, LOCKMODE lockmode);
/*
 * Function table_openrv_extended completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 table_openrv_extended在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern Relation table_openrv_extended(const RangeVar *relation,
									  LOCKMODE lockmode, bool missing_ok);
/*
 * Function try_table_open carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 try_table_open通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Relation try_table_open(Oid relationId, LOCKMODE lockmode);
/*
 * Function table_close completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 table_close在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void table_close(Relation relation, LOCKMODE lockmode);

#endif							/* TABLE_H */

/* 中文翻译：表_H */
