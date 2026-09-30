/*-------------------------------------------------------------------------
 *
 * relation.h
 *	  Generic relation related routines.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relation.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * relation.h 通用关系相关例程。 src/include/a
 * ccess/relation.h
 */
#ifndef ACCESS_RELATION_H
#define ACCESS_RELATION_H

#include "nodes/primnodes.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"

/*
 * Function relation_open carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 relation_open通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Relation relation_open(Oid relationId, LOCKMODE lockmode);
/*
 * Function try_relation_open carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 try_relation_open通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Relation try_relation_open(Oid relationId, LOCKMODE lockmode);
/*
 * Function relation_openrv carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 relation_openrv通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Relation relation_openrv(const RangeVar *relation, LOCKMODE lockmode);
/*
 * Function relation_openrv_extended completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 relation_openrv_extended在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern Relation relation_openrv_extended(const RangeVar *relation,
										 LOCKMODE lockmode, bool missing_ok);
/*
 * Function relation_close completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 relation_close在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void relation_close(Relation relation, LOCKMODE lockmode);

#endif							/* ACCESS_RELATION_H */

/* 中文翻译：ACCESS_RELATION_H */
