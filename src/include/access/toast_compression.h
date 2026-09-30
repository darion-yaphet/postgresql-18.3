/*-------------------------------------------------------------------------
 *
 * toast_compression.h
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021-2025, PostgreSQL Global Development Group
 *
 * src/include/access/toast_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_COMPRESSION_H
#define TOAST_COMPRESSION_H

/*
 * GUC support.
 *
 * default_toast_compression is an integer for purposes of the GUC machinery,
 * but the value is one of the char values defined below, as they appear in
 * pg_attribute.attcompression, e.g. TOAST_PGLZ_COMPRESSION.
 *
 * 中文翻译：
 * GUC 支持。 default_toast_compression
 * 是一个用于 GUC 机制的整数，但该值是下面定义的 char 值之一
 * ，正如它们出现在 pg_attribute.attcompressi
 * on 中一样，例如TOAST_PGLZ_COMPRESSION。
 */
/*
 * Global declaration default_toast_compression exposes shared access-layer state. Callers read or update it to coordinate the related behavior.
 *
 * 全局声明 default_toast_compression 暴露共享的访问层状态。调用方读取或更新它，以协调相关行为。
 */
extern PGDLLIMPORT int default_toast_compression;

/*
 * Built-in compression method ID.  The toast compression header will store
 * this in the first 2 bits of the raw length.  These built-in compression
 * method IDs are directly mapped to the built-in compression methods.
 *
 * Don't use these values for anything other than understanding the meaning
 * of the raw bits from a varlena; in particular, if the goal is to identify
 * a compression method, use the constants TOAST_PGLZ_COMPRESSION, etc.
 * below. We might someday support more than 4 compression methods, but
 * we can never have more than 4 values in this enum, because there are
 * only 2 bits available in the places where this is stored.
 *
 * 中文翻译：
 * 内置压缩方法ID。 Toast 压缩标头会将其存储在原始长度的前 2
 *  位中。这些内置压缩方法ID直接映射到内置压缩方法。除了理解 var
 * lena 原始位的含义之外，不要将这些值用于任何其他目的；特别是，如
 * 果目标是确定压缩方法，请使用下面的常量 TOAST_PGLZ_COM
 * PRESSION 等。有一天我们可能会支持超过 4 种压缩方法，但是
 * 这个枚举中的值永远不会超过 4 个，因为存储该值的地方只有 2 位可
 * 用。
 */
typedef enum ToastCompressionId
{
	TOAST_PGLZ_COMPRESSION_ID = 0,
	TOAST_LZ4_COMPRESSION_ID = 1,
	TOAST_INVALID_COMPRESSION_ID = 2,
} ToastCompressionId;

/*
 * Built-in compression methods.  pg_attribute will store these in the
 * attcompression column.  In attcompression, InvalidCompressionMethod
 * denotes the default behavior.
 *
 * 中文翻译：
 * 内置压缩方法。 pg_attribute 将把它们存储在 attco
 * mpression 列中。在 attcompression 中，In
 * validCompressionMethod 表示默认行为。
 */
#define TOAST_PGLZ_COMPRESSION			'p'
#define TOAST_LZ4_COMPRESSION			'l'
#define InvalidCompressionMethod		'\0'

#define CompressionMethodIsValid(cm)  ((cm) != InvalidCompressionMethod)


/* pglz compression/decompression routines */

/* 中文翻译：pglz 压缩/解压例程 */
/*
 * Function pglz_compress_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 pglz_compress_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern struct varlena *pglz_compress_datum(const struct varlena *value);
/*
 * Function pglz_decompress_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 pglz_decompress_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern struct varlena *pglz_decompress_datum(const struct varlena *value);
/*
 * Function pglz_decompress_datum_slice constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 pglz_decompress_datum_slice通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern struct varlena *pglz_decompress_datum_slice(const struct varlena *value,
												   int32 slicelength);

/* lz4 compression/decompression routines */

/* 中文翻译：lz4 压缩/解压例程 */
/*
 * Function lz4_compress_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 lz4_compress_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern struct varlena *lz4_compress_datum(const struct varlena *value);
/*
 * Function lz4_decompress_datum constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 lz4_decompress_datum通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern struct varlena *lz4_decompress_datum(const struct varlena *value);
/*
 * Function lz4_decompress_datum_slice constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 lz4_decompress_datum_slice通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern struct varlena *lz4_decompress_datum_slice(const struct varlena *value,
												  int32 slicelength);

/* other stuff */

/* 中文翻译：其他东西 */
/*
 * Function toast_get_compression_id retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 toast_get_compression_id通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern ToastCompressionId toast_get_compression_id(struct varlena *attr);
/*
 * Function CompressionNameToMethod constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 CompressionNameToMethod通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern char CompressionNameToMethod(const char *compression);
/*
 * Function GetCompressionMethodName retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 GetCompressionMethodName通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern const char *GetCompressionMethodName(char method);

#endif							/* TOAST_COMPRESSION_H */

/* 中文翻译：TOAST_COMPRESSION_H */
