/*-------------------------------------------------------------------------
 *
 * toast_internals.h
 *	  Internal definitions for the TOAST system.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/toast_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOAST_INTERNALS_H
#define TOAST_INTERNALS_H

#include "access/toast_compression.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/*
 *	The information at the start of the compressed toast data.
 *
 * 中文翻译：
 * 压缩 Toast 数据开头的信息。
 */
typedef struct toast_compress_header
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */

	/* 中文翻译：varlena header（不要直接触摸！） */
	uint32		tcinfo;			/* 2 bits for compression method and 30 bits
								 * external size; see va_extinfo */

	/* 中文翻译：
	 * 2 位压缩方法和 30 位外部大小；参见 va_extinfo
	 */
} toast_compress_header;

/*
 * Utilities for manipulation of header information for compressed
 * toast entries.
 *
 * 中文翻译：
 * 用于操作压缩 Toast 条目的标头信息的实用程序。
 */
#define TOAST_COMPRESS_EXTSIZE(ptr) \
	(((toast_compress_header *) (ptr))->tcinfo & VARLENA_EXTSIZE_MASK)
#define TOAST_COMPRESS_METHOD(ptr) \
	(((toast_compress_header *) (ptr))->tcinfo >> VARLENA_EXTSIZE_BITS)

#define TOAST_COMPRESS_SET_SIZE_AND_COMPRESS_METHOD(ptr, len, cm_method) \
	do { \
		Assert((len) > 0 && (len) <= VARLENA_EXTSIZE_MASK); \
		Assert((cm_method) == TOAST_PGLZ_COMPRESSION_ID || \
			   (cm_method) == TOAST_LZ4_COMPRESSION_ID); \
		((toast_compress_header *) (ptr))->tcinfo = \
			(len) | ((uint32) (cm_method) << VARLENA_EXTSIZE_BITS); \
	} while (0)

/*
 * Function toast_compress_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_compress_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern Datum toast_compress_datum(Datum value, char cmethod);
/*
 * Function toast_get_valid_index retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 toast_get_valid_index通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Oid	toast_get_valid_index(Oid toastoid, LOCKMODE lock);

/*
 * Function toast_delete_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_delete_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void toast_delete_datum(Relation rel, Datum value, bool is_speculative);
/*
 * Function toast_save_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_save_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern Datum toast_save_datum(Relation rel, Datum value,
							  struct varlena *oldexternal, int options);

/*
 * Function toast_open_indexes carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 toast_open_indexes通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern int	toast_open_indexes(Relation toastrel,
							   LOCKMODE lock,
							   Relation **toastidxs,
							   int *num_indexes);
/*
 * Function toast_close_indexes completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 toast_close_indexes在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void toast_close_indexes(Relation *toastidxs, int num_indexes,
								LOCKMODE lock);
/*
 * Function get_toast_snapshot retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 get_toast_snapshot通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Snapshot get_toast_snapshot(void);

#endif							/* TOAST_INTERNALS_H */

/* 中文翻译：TOAST_INTERNALS_H */
