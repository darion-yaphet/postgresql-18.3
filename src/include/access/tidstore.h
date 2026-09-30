/*-------------------------------------------------------------------------
 *
 * tidstore.h
 *	  TidStore interface.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tidstore.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * tidstore.h TidStore 接口。 src/includ
 * e/access/tidstore.h
 */
#ifndef TIDSTORE_H
#define TIDSTORE_H

#include "storage/itemptr.h"
#include "utils/dsa.h"

typedef struct TidStore TidStore;
typedef struct TidStoreIter TidStoreIter;

/*
 * Result struct for TidStoreIterateNext.  This is copyable, but should be
 * treated as opaque.  Call TidStoreGetBlockOffsets() to obtain the offsets.
 *
 * 中文翻译：
 * TidStoreIterateNext 的结果结构。这是可复制的，但
 * 应被视为不透明。调用 TidStoreGetBlockOffsets
 * () 获取偏移量。
 */
typedef struct TidStoreIterResult
{
	BlockNumber blkno;
	void	   *internal_page;
} TidStoreIterResult;

/*
 * Function TidStoreCreateLocal initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 TidStoreCreateLocal通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TidStore *TidStoreCreateLocal(size_t max_bytes, bool insert_only);
/*
 * Function TidStoreCreateShared initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 TidStoreCreateShared通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TidStore *TidStoreCreateShared(size_t max_bytes, int tranche_id);
/*
 * Function TidStoreAttach carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 TidStoreAttach通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern TidStore *TidStoreAttach(dsa_handle area_handle, dsa_pointer handle);
/*
 * Function TidStoreDetach completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 TidStoreDetach在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void TidStoreDetach(TidStore *ts);
/*
 * Function TidStoreLockExclusive updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 TidStoreLockExclusive通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void TidStoreLockExclusive(TidStore *ts);
/*
 * Function TidStoreLockShare updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 TidStoreLockShare通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void TidStoreLockShare(TidStore *ts);
/*
 * Function TidStoreUnlock updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 TidStoreUnlock通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void TidStoreUnlock(TidStore *ts);
/*
 * Function TidStoreDestroy completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 TidStoreDestroy在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void TidStoreDestroy(TidStore *ts);
/*
 * Function TidStoreSetBlockOffsets updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 TidStoreSetBlockOffsets通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void TidStoreSetBlockOffsets(TidStore *ts, BlockNumber blkno, OffsetNumber *offsets,
									int num_offsets);
/*
 * Function TidStoreIsMember evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 TidStoreIsMember通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern bool TidStoreIsMember(TidStore *ts, ItemPointer tid);
/*
 * Function TidStoreBeginIterate initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 TidStoreBeginIterate通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TidStoreIter *TidStoreBeginIterate(TidStore *ts);
/*
 * Function TidStoreIterateNext retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TidStoreIterateNext通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern TidStoreIterResult *TidStoreIterateNext(TidStoreIter *iter);
/*
 * Function TidStoreGetBlockOffsets retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TidStoreGetBlockOffsets通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern int	TidStoreGetBlockOffsets(TidStoreIterResult *result,
									OffsetNumber *offsets,
									int max_offsets);
/*
 * Function TidStoreEndIterate completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 TidStoreEndIterate在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void TidStoreEndIterate(TidStoreIter *iter);
/*
 * Function TidStoreMemoryUsage carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 TidStoreMemoryUsage通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern size_t TidStoreMemoryUsage(TidStore *ts);
/*
 * Function TidStoreGetHandle retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TidStoreGetHandle通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern dsa_pointer TidStoreGetHandle(TidStore *ts);
/*
 * Function TidStoreGetDSA retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TidStoreGetDSA通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern dsa_area *TidStoreGetDSA(TidStore *ts);

#endif							/* TIDSTORE_H */

/* 中文翻译：TIDSTORE_H */
