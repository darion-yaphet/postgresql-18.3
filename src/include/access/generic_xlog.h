/*-------------------------------------------------------------------------
 *
 * generic_xlog.h
 *	  Generic xlog API definition.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/generic_xlog.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * generic_xlog.h 通用 xlog API 定义。 src
 * /include/access/generic_xlog.h
 */
#ifndef GENERIC_XLOG_H
#define GENERIC_XLOG_H

#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/xloginsert.h"
#include "storage/bufpage.h"
#include "utils/rel.h"

#define MAX_GENERIC_XLOG_PAGES	XLR_NORMAL_MAX_BLOCK_ID

/* Flag bits for GenericXLogRegisterBuffer */

/* 中文翻译：GenericXLogRegisterBuffer 的标志位 */
#define GENERIC_XLOG_FULL_IMAGE 0x0001	/* write full-page image */

/* 中文翻译：写整页图像 */

/* state of generic xlog record construction */

/* 中文翻译：通用 xlog 记录构造的状态 */
struct GenericXLogState;
typedef struct GenericXLogState GenericXLogState;

/* API for construction of generic xlog records */

/* 中文翻译：用于构建通用 xlog 记录的 API */
/*
 * Function GenericXLogStart initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 GenericXLogStart通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern GenericXLogState *GenericXLogStart(Relation relation);
/*
 * Function GenericXLogRegisterBuffer updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 GenericXLogRegisterBuffer通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern Page GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buffer,
									  int flags);
/*
 * Function GenericXLogFinish completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 GenericXLogFinish在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern XLogRecPtr GenericXLogFinish(GenericXLogState *state);
/*
 * Function GenericXLogAbort completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 GenericXLogAbort在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void GenericXLogAbort(GenericXLogState *state);

/* functions defined for rmgr */

/* 中文翻译：为 rmgr 定义的函数 */
/*
 * Function generic_redo carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 generic_redo通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void generic_redo(XLogReaderState *record);
/*
 * Function generic_identify retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 generic_identify通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern const char *generic_identify(uint8 info);
/*
 * Function generic_desc retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 generic_desc通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void generic_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function generic_mask carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 generic_mask通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void generic_mask(char *page, BlockNumber blkno);

#endif							/* GENERIC_XLOG_H */

/* 中文翻译：GENERIC_XLOG_H */
