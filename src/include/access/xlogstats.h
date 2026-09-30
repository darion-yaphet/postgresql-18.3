/*-------------------------------------------------------------------------
 *
 * xlogstats.h
 *		Definitions for WAL Statistics
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/xlogstats.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XLOGSTATS_H
#define XLOGSTATS_H

#include "access/rmgr.h"
#include "access/xlogreader.h"

#define MAX_XLINFO_TYPES 16

typedef struct XLogRecStats
{
	uint64		count;
	uint64		rec_len;
	uint64		fpi_len;
} XLogRecStats;

typedef struct XLogStats
{
	uint64		count;
#ifdef FRONTEND
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
#endif
	XLogRecStats rmgr_stats[RM_MAX_ID + 1];
	XLogRecStats record_stats[RM_MAX_ID + 1][MAX_XLINFO_TYPES];
} XLogStats;

/*
 * Function: XLogRecGetLen.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog rec get len.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecGetLen。
 * 作用：执行 xlog rec get len 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogRecGetLen(XLogReaderState *record, uint32 *rec_len,
						  uint32 *fpi_len);
/*
 * Function: XLogRecStoreStats.
 * Purpose: Performs the WAL reading or recovery operation represented by xlog rec store stats.
 * Core flow: It prepares the reader or recovery context, processes the requested WAL state, and exposes the result.
 *
 * 函数：XLogRecStoreStats。
 * 作用：执行 xlog rec store stats 所表示的 WAL 读取或恢复操作。
 * 核心流程：它准备读取器或恢复上下文，处理请求的 WAL 状态，并提供结果。
 */
extern void XLogRecStoreStats(XLogStats *stats, XLogReaderState *record);

#endif							/* XLOGSTATS_H */
