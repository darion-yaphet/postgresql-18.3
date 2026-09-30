/*
 * xlogrecord.h
 *
 * 中文翻译：xlogrecord.h
 *
 * Definitions for the WAL record format.
 *
 * 中文翻译：WAL 记录格式的定义。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecord.h
 *
 * 中文翻译：src/include/access/xlogrecord.h
 */
#ifndef XLOGRECORD_H
#define XLOGRECORD_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "port/pg_crc32c.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * The overall layout of an XLOG record is:
 *		Fixed-size header (XLogRecord struct)
 *		XLogRecordBlockHeader struct
 *		XLogRecordBlockHeader struct
 *		...
 *		XLogRecordDataHeader[Short|Long] struct
 *		block data
 *		block data
 *		...
 *		main data
 *
 * 中文翻译：主要数据
 *
 * There can be zero or more XLogRecordBlockHeaders, and 0 or more bytes of
 * rmgr-specific data not associated with a block.  XLogRecord structs
 * always start on MAXALIGN boundaries in the WAL files, but the rest of
 * the fields are not aligned.
 *
 * 中文翻译：可以有零个或多个 XLogRecordBlockHeader，以及 0 个或多个字节的与块不关联的 rmgr 特定数据。 XLogRecord 结构始终从 WAL 文件中的 MAXALIGN 边界开始，但其余字段未对齐。
 *
 * The XLogRecordBlockHeader, XLogRecordDataHeaderShort and
 * XLogRecordDataHeaderLong structs all begin with a single 'id' byte. It's
 * used to distinguish between block references, and the main data structs.
 *
 * 中文翻译：XLogRecordBlockHeader、XLogRecordDataHeaderShort 和 XLogRecordDataHeaderLong 结构均以单个“id”字节开头。它用于区分块引用和主要数据结构。
 */
typedef struct XLogRecord
{
	uint32		xl_tot_len;		/* total len of entire record */

	/* 整个记录的总长度。 */
	TransactionId xl_xid;		/* xact id */

	/* 事务 ID。 */
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */

	/* 日志中前一条记录的指针。 */
	uint8		xl_info;		/* flag bits, see below */

	/* 标志位，见下文。 */
	RmgrId		xl_rmid;		/* resource manager for this record */

	/* 该记录的资源管理器。 */
	/* 2 bytes of padding here, initialize to zero */

	/* 中文翻译：这里填充2个字节，初始化为零 */
	pg_crc32c	xl_crc;			/* CRC for this record */

	/* 该记录的 CRC。 */

	/* XLogRecordBlockHeaders and XLogRecordDataHeader follow, no padding */

	/* 中文翻译：XLogRecordBlockHeaders 和 XLogRecordDataHeader 跟随，无填充 */

} XLogRecord;

#define SizeOfXLogRecord	(offsetof(XLogRecord, xl_crc) + sizeof(pg_crc32c))

/*
 * The high 4 bits in xl_info may be used freely by rmgr. The
 * XLR_SPECIAL_REL_UPDATE and XLR_CHECK_CONSISTENCY bits can be passed by
 * XLogInsert caller. The rest are set internally by XLogInsert.
 *
 * 中文翻译：xl_info 中的高 4 位可以由 rmgr 自由使用。 XLR_SPECIAL_REL_UPDATE 和 XLR_CHECK_CONSISTENCY 位可由 XLogInsert 调用方传递。其余的由 XLogInsert 在内部设置。
 */
#define XLR_INFO_MASK			0x0F
#define XLR_RMGR_INFO_MASK		0xF0

/*
 * XLogReader needs to allocate all the data of a WAL record in a single
 * chunk.  This means that a single XLogRecord cannot exceed MaxAllocSize
 * in length if we ignore any allocation overhead of the XLogReader.
 *
 * 中文翻译：XLogReader需要将WAL记录的所有数据分配在单个块中。这意味着如果我们忽略 XLogReader 的任何分配开销，单个 XLogRecord 的长度不能超过 MaxAllocSize。
 *
 * To accommodate some overhead, this value allows for 4M of allocation
 * overhead, that should be plenty enough for what the XLogReader
 * infrastructure expects as extra.
 *
 * 中文翻译：为了容纳一些开销，该值允许 4M 的分配开销，这对于 XLogReader 基础设施所期望的额外开销来说应该足够了。
 */
#define XLogRecordMaxSize	(1020 * 1024 * 1024)

/*
 * If a WAL record modifies any relation files, in ways not covered by the
 * usual block references, this flag is set. This is not used for anything
 * by PostgreSQL itself, but it allows external tools that read WAL and keep
 * track of modified blocks to recognize such special record types.
 *
 * 中文翻译：如果 WAL 记录以通常块引用未涵盖的方式修改任何关系文件，则设置此标志。 PostgreSQL 本身不使用它，但它允许读取 WAL 并跟踪修改块的外部工具来识别此类特殊记录类型。
 */
#define XLR_SPECIAL_REL_UPDATE	0x01

/*
 * Enforces consistency checks of replayed WAL at recovery. If enabled,
 * each record will log a full-page write for each block modified by the
 * record and will reuse it afterwards for consistency checks. The caller
 * of XLogInsert can use this value if necessary, but if
 * wal_consistency_checking is enabled for a rmgr this is set unconditionally.
 *
 * 中文翻译：在恢复时强制重放 WAL 的一致性检查。如果启用，每个记录将记录该记录修改的每个块的整页写入，并随后重用它进行一致性检查。如果需要，XLogInsert 的调用者可以使用此值，但如果为 rmgr 启用了 wal_consistency_checking，则将无条件设置此值。
 */
#define XLR_CHECK_CONSISTENCY	0x02

/*
 * Header info for block data appended to an XLOG record.
 *
 * 中文翻译：附加到 XLOG 记录的块数据的标头信息。
 *
 * 'data_length' is the length of the rmgr-specific payload data associated
 * with this block. It does not include the possible full page image, nor
 * XLogRecordBlockHeader struct itself.
 *
 * 中文翻译：“data_length”是与该块关联的特定于 RMGR 的有效负载数据的长度。它不包括可能的整页图像，也不包括 XLogRecordBlockHeader 结构本身。
 *
 * Note that we don't attempt to align the XLogRecordBlockHeader struct!
 * So, the struct must be copied to aligned local storage before use.
 *
 * 中文翻译：请注意，我们不会尝试对齐 XLogRecordBlockHeader 结构！因此，在使用之前必须将结构复制到对齐的本地存储。
 */
typedef struct XLogRecordBlockHeader
{
	uint8		id;				/* block reference ID */

	/* 中文翻译：块参考 ID */
	uint8		fork_flags;		/* fork within the relation, and flags */

	/* 中文翻译：关系内的分叉和标志 */
	uint16		data_length;	/* number of payload bytes (not including page
								 * image) */

	/* If BKPBLOCK_HAS_IMAGE, an XLogRecordBlockImageHeader struct follows */

	/* 中文翻译：如果 BKPBLOCK_HAS_IMAGE，则后面是 XLogRecordBlockImageHeader 结构 */
	/* If BKPBLOCK_SAME_REL is not set, a RelFileLocator follows */

	/* 中文翻译：如果未设置 BKPBLOCK_SAME_REL，则后面跟着一个 RelFileLocator */
	/* BlockNumber follows */

	/* 中文翻译：区块编号如下 */
} XLogRecordBlockHeader;

#define SizeOfXLogRecordBlockHeader (offsetof(XLogRecordBlockHeader, data_length) + sizeof(uint16))

/*
 * Additional header information when a full-page image is included
 * (i.e. when BKPBLOCK_HAS_IMAGE is set).
 *
 * 中文翻译：包含整页图像时的附加标头信息（即设置 BKPBLOCK_HAS_IMAGE 时）。
 *
 * The XLOG code is aware that PG data pages usually contain an unused "hole"
 * in the middle, which contains only zero bytes.  Since we know that the
 * "hole" is all zeros, we remove it from the stored data (and it's not counted
 * in the XLOG record's CRC, either).  Hence, the amount of block data actually
 * present is (BLCKSZ - <length of "hole" bytes>).
 *
 * 中文翻译：XLOG代码知道PG数据页通常在中间包含一个未使用的“洞”，其中仅包含零字节。由于我们知道“漏洞”全为零，因此我们将其从存储的数据中删除（并且它也不计入 XLOG 记录的 CRC 中）。因此，实际存在的块数据量是（BLCKSZ - <“洞”字节的长度>）。
 *
 * Additionally, when wal_compression is enabled, we will try to compress full
 * page images using one of the supported algorithms, after removing the
 * "hole". This can reduce the WAL volume, but at some extra cost of CPU spent
 * on the compression during WAL logging. In this case, since the "hole"
 * length cannot be calculated by subtracting the number of page image bytes
 * from BLCKSZ, basically it needs to be stored as an extra information.
 * But when no "hole" exists, we can assume that the "hole" length is zero
 * and no such an extra information needs to be stored. Note that
 * the original version of page image is stored in WAL instead of the
 * compressed one if the number of bytes saved by compression is less than
 * the length of extra information. Hence, when a page image is successfully
 * compressed, the amount of block data actually present is less than
 * BLCKSZ - the length of "hole" bytes - the length of extra information.
 *
 * 中文翻译：此外，当启用 wal_compression 时，我们将在删除“洞”后尝试使用支持的算法之一来压缩整页图像。这可以减少 WAL 体积，但会在 WAL 日志记录期间花费一些额外的 CPU 成本来进行压缩。在这种情况下，由于无法通过从 BLCKSZ 中减去页面图像字节数来计算“洞”长度，因此基本上需要将其存储为额外信息。但是当不存在“洞”时，我们可以假设“洞”长度为零，并且不需要存储这样的额外信息。请注意，如果压缩节省的字节数小于额外信息的长度，则页面图像的原始版本将存储在 WAL 中，而不是压缩后的版本。因此，当页面图像被成功压缩时，实际存在的块数据量小于BLCKSZ——“空洞”字节的长度——额外信息的长度。
 */
typedef struct XLogRecordBlockImageHeader
{
	uint16		length;			/* number of page image bytes */

	/* 中文翻译：页面图像字节数 */
	uint16		hole_offset;	/* number of bytes before "hole" */

	/* 中文翻译：“hole”之前的字节数 */
	uint8		bimg_info;		/* flag bits, see below */

	/* 标志位，见下文。 */

	/*
	 * If BKPIMAGE_HAS_HOLE and BKPIMAGE_COMPRESSED(), an
	 * XLogRecordBlockCompressHeader struct follows.
 *
 * 中文翻译：如果 BKPIMAGE_HAS_HOLE 和 BKPIMAGE_COMPRESSED()，则后面跟着一个 XLogRecordBlockcompressHeader 结构。
	 */
} XLogRecordBlockImageHeader;

#define SizeOfXLogRecordBlockImageHeader	\
	(offsetof(XLogRecordBlockImageHeader, bimg_info) + sizeof(uint8))

/* Information stored in bimg_info */

/* 中文翻译：bimg_info中存储的信息 */
#define BKPIMAGE_HAS_HOLE		0x01	/* page image has "hole" */

/* 中文翻译：页面图像有“洞” */
#define BKPIMAGE_APPLY			0x02	/* page image should be restored
										 * during replay */
/* compression methods supported */

/* 中文翻译：支持的压缩方法 */
#define BKPIMAGE_COMPRESS_PGLZ	0x04
#define BKPIMAGE_COMPRESS_LZ4	0x08
#define BKPIMAGE_COMPRESS_ZSTD	0x10

#define	BKPIMAGE_COMPRESSED(info) \
	((info & (BKPIMAGE_COMPRESS_PGLZ | BKPIMAGE_COMPRESS_LZ4 | \
			  BKPIMAGE_COMPRESS_ZSTD)) != 0)

/*
 * Extra header information used when page image has "hole" and
 * is compressed.
 *
 * 中文翻译：当页面图像有“洞”并且被压缩时使用额外的标头信息。
 */
typedef struct XLogRecordBlockCompressHeader
{
	uint16		hole_length;	/* number of bytes in "hole" */

	/* 中文翻译：“洞”中的字节数 */
} XLogRecordBlockCompressHeader;

#define SizeOfXLogRecordBlockCompressHeader \
	sizeof(XLogRecordBlockCompressHeader)

/*
 * Maximum size of the header for a block reference. This is used to size a
 * temporary buffer for constructing the header.
 *
 * 中文翻译：块引用的标头的最大大小。这用于确定用于构建标头的临时缓冲区的大小。
 */
#define MaxSizeOfXLogRecordBlockHeader \
	(SizeOfXLogRecordBlockHeader + \
	 SizeOfXLogRecordBlockImageHeader + \
	 SizeOfXLogRecordBlockCompressHeader + \
	 sizeof(RelFileLocator) + \
	 sizeof(BlockNumber))

/*
 * The fork number fits in the lower 4 bits in the fork_flags field. The upper
 * bits are used for flags.
 *
 * 中文翻译：分叉号位于 fork_flags 字段的低 4 位中。高位用于标志。
 */
#define BKPBLOCK_FORK_MASK	0x0F
#define BKPBLOCK_FLAG_MASK	0xF0
#define BKPBLOCK_HAS_IMAGE	0x10	/* block data is an XLogRecordBlockImage */

/* 中文翻译：块数据是 XLogRecordBlockImage */
#define BKPBLOCK_HAS_DATA	0x20
#define BKPBLOCK_WILL_INIT	0x40	/* redo will re-init the page */

/* 说明 WAL 重做时的页面读取、关系操作或错误处理。 */
#define BKPBLOCK_SAME_REL	0x80	/* RelFileLocator omitted, same as
									 * previous */

/*
 * XLogRecordDataHeaderShort/Long are used for the "main data" portion of
 * the record. If the length of the data is less than 256 bytes, the short
 * form is used, with a single byte to hold the length. Otherwise the long
 * form is used.
 *
 * 中文翻译：XLogRecordDataHeaderShort/Long 用于记录的“主要数据”部分。如果数据长度小于256字节，则使用短格式，用单个字节来保存长度。否则使用长形式。
 *
 * (These structs are currently not used in the code, they are here just for
 * documentation purposes).
 *
 * 中文翻译：（这些结构目前未在代码中使用，它们在这里仅用于文档目的）。
 */
typedef struct XLogRecordDataHeaderShort
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_SHORT */
	uint8		data_length;	/* number of payload bytes */

	/* 中文翻译：有效负载字节数 */
}			XLogRecordDataHeaderShort;

#define SizeOfXLogRecordDataHeaderShort (sizeof(uint8) * 2)

typedef struct XLogRecordDataHeaderLong
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_LONG */
	/* followed by uint32 data_length, unaligned */

	/* 中文翻译：后面是 uint32 data_length，未对齐 */
}			XLogRecordDataHeaderLong;

#define SizeOfXLogRecordDataHeaderLong (sizeof(uint8) + sizeof(uint32))

/*
 * Block IDs used to distinguish different kinds of record fragments. Block
 * references are numbered from 0 to XLR_MAX_BLOCK_ID. A rmgr is free to use
 * any ID number in that range (although you should stick to small numbers,
 * because the WAL machinery is optimized for that case). A few ID
 * numbers are reserved to denote the "main" data portion of the record,
 * as well as replication-supporting transaction metadata.
 *
 * 中文翻译：块ID用于区分不同类型的记录片段。块引用的编号范围为 0 到 XLR_MAX_BLOCK_ID。 rmgr 可以自由使用该范围内的任何 ID 号（尽管您应该坚持使用较小的数字，因为 WAL 机制针对这种情况进行了优化）。保留一些 ID 号来表示记录的“主要”数据部分，以及支持复制的事务元数据。
 *
 * The maximum is currently set at 32, quite arbitrarily. Most records only
 * need a handful of block references, but there are a few exceptions that
 * need more.
 *
 * 中文翻译：目前最大值设置为 32，相当随意。大多数记录只需要少量的块引用，但也有一些例外需要更多。
 */
#define XLR_MAX_BLOCK_ID			32

#define XLR_BLOCK_ID_DATA_SHORT		255
#define XLR_BLOCK_ID_DATA_LONG		254
#define XLR_BLOCK_ID_ORIGIN			253
#define XLR_BLOCK_ID_TOPLEVEL_XID	252

#endif							/* XLOGRECORD_H */
