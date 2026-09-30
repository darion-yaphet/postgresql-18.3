/*-------------------------------------------------------------------------
 *
 * rmgrdesc_utils.h
 *	  Support functions for rmgrdesc routines
 *
  * rmgrdesc_utils.h rmgrdesc 例程的支持函数
 *
 * Copyright (c) 2023-2025, PostgreSQL Global Development Group
 *
 * src/include/access/rmgrdesc_utils.h
 *
  * src/include/access/rmgrdesc_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RMGRDESC_UTILS_H_
#define RMGRDESC_UTILS_H_

extern void array_desc(StringInfo buf, void *array, size_t elem_size, int count,
					   void (*elem_desc) (StringInfo buf, void *elem, void *data),
					   void *data);
/*
 * Function: offset_elem_desc.
 * Purpose: Performs the operation represented by offset elem desc.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：offset_elem_desc。
 * 作用：执行 offset elem desc 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void offset_elem_desc(StringInfo buf, void *offset, void *data);
/*
 * Function: redirect_elem_desc.
 * Purpose: Performs the operation represented by redirect elem desc.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：redirect_elem_desc。
 * 作用：执行 redirect elem desc 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void redirect_elem_desc(StringInfo buf, void *offset, void *data);
/*
 * Function: oid_elem_desc.
 * Purpose: Performs the operation represented by oid elem desc.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：oid_elem_desc。
 * 作用：执行 oid elem desc 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void oid_elem_desc(StringInfo buf, void *relid, void *data);

#endif							/* RMGRDESC_UTILS_H_ */
