/*-------------------------------------------------------------------------
 *
 * brin_xlog.h
 *	  POSTGRES BRIN access XLOG definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/brin_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BRIN_XLOG_H
#define BRIN_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"


/*
 * WAL record definitions for BRIN's WAL operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field.
 */

/*
 * BRIN 各类 WAL 操作对应的 WAL 记录定义。
 *
 * XLOG 允许在日志记录的 xl_info 字段的高 4 位中存储一些信息。
 */
#define XLOG_BRIN_CREATE_INDEX		0x00
#define XLOG_BRIN_INSERT			0x10
#define XLOG_BRIN_UPDATE			0x20
#define XLOG_BRIN_SAMEPAGE_UPDATE	0x30
#define XLOG_BRIN_REVMAP_EXTEND		0x40
#define XLOG_BRIN_DESUMMARIZE		0x50

#define XLOG_BRIN_OPMASK			0x70
/*
 * When we insert the first item on a new page, we restore the entire page in
 * redo.
 */

/*
 * 当我们在一个新页面上插入第一个项（item）时，会在 redo（重做）阶段恢复整个页面。
 */
#define XLOG_BRIN_INIT_PAGE		0x80

/*
 * This is what we need to know about a BRIN index create.
 *
 * Backup block 0: metapage
 */

/*
 * 这是我们在创建 BRIN 索引时需要记录的信息。
 *
 * 备份块 0：元页面（metapage）
 */
typedef struct xl_brin_createidx
{
	BlockNumber pagesPerRange;
	uint16		version;
} xl_brin_createidx;
#define SizeOfBrinCreateIdx (offsetof(xl_brin_createidx, version) + sizeof(uint16))

/*
 * This is what we need to know about a BRIN tuple insert
 *
 * Backup block 0: main page, block data is the new BrinTuple.
 * Backup block 1: revmap page
 */

/*
 * 这是我们在插入一个 BRIN 元组时需要记录的信息。
 *
 * 备份块 0：主页面（main page），块数据是新的 BrinTuple。
 * 备份块 1：反向映射页面（revmap page）
 */
typedef struct xl_brin_insert
{
	BlockNumber heapBlk;

	/* extra information needed to update the revmap */

	/* 更新反向映射（revmap）所需的额外信息 */
	BlockNumber pagesPerRange;

	/* offset number in the main page to insert the tuple to. */

	/* 元组要插入到主页面中的偏移量（offset number）。 */
	OffsetNumber offnum;
} xl_brin_insert;

#define SizeOfBrinInsert	(offsetof(xl_brin_insert, offnum) + sizeof(OffsetNumber))

/*
 * A cross-page update is the same as an insert, but also stores information
 * about the old tuple.
 *
 * Like in xl_brin_insert:
 * Backup block 0: new page, block data includes the new BrinTuple.
 * Backup block 1: revmap page
 *
 * And in addition:
 * Backup block 2: old page
 */

/*
 * 跨页面更新（cross-page update）与插入相同，但同时还存储了关于旧元组的信息。
 *
 * 与 xl_brin_insert 中一样：
 * 备份块 0：新页面（new page），块数据包含新的 BrinTuple。
 * 备份块 1：反向映射页面（revmap page）
 *
 * 此外还有：
 * 备份块 2：旧页面（old page）
 */
typedef struct xl_brin_update
{
	/* offset number of old tuple on old page */

	/* 旧元组在旧页面上的偏移量（offset number） */
	OffsetNumber oldOffnum;

	xl_brin_insert insert;
} xl_brin_update;

#define SizeOfBrinUpdate	(offsetof(xl_brin_update, insert) + SizeOfBrinInsert)

/*
 * This is what we need to know about a BRIN tuple samepage update
 *
 * Backup block 0: updated page, with new BrinTuple as block data
 */

/*
 * 这是我们在进行 BRIN 元组同页更新（samepage update）时需要记录的信息。
 *
 * 备份块 0：被更新的页面，以新的 BrinTuple 作为块数据
 */
typedef struct xl_brin_samepage_update
{
	OffsetNumber offnum;
} xl_brin_samepage_update;

#define SizeOfBrinSamepageUpdate		(sizeof(OffsetNumber))

/*
 * This is what we need to know about a revmap extension
 *
 * Backup block 0: metapage
 * Backup block 1: new revmap page
 */

/*
 * 这是我们在进行反向映射扩展（revmap extension）时需要记录的信息。
 *
 * 备份块 0：元页面（metapage）
 * 备份块 1：新的反向映射页面（revmap page）
 */
typedef struct xl_brin_revmap_extend
{
	/*
	 * XXX: This is actually redundant - the block number is stored as part of
	 * backup block 1.
	 */

	/*
	 * XXX：这其实是冗余的 —— 块号已经作为备份块 1 的一部分被存储了。
	 */
	BlockNumber targetBlk;
} xl_brin_revmap_extend;

#define SizeOfBrinRevmapExtend	(offsetof(xl_brin_revmap_extend, targetBlk) + \
								 sizeof(BlockNumber))

/*
 * This is what we need to know about a range de-summarization
 *
 * Backup block 0: revmap page
 * Backup block 1: regular page
 */

/*
 * 这是我们在进行范围反汇总（de-summarization）时需要记录的信息。
 *
 * 备份块 0：反向映射页面（revmap page）
 * 备份块 1：常规页面（regular page）
 */
typedef struct xl_brin_desummarize
{
	BlockNumber pagesPerRange;
	/* page number location to set to invalid */

	/* 要设置为无效的页号位置 */
	BlockNumber heapBlk;
	/* offset of item to delete in regular index page */

	/* 在常规索引页面中要删除的项的偏移量 */
	OffsetNumber regOffset;
} xl_brin_desummarize;

#define SizeOfBrinDesummarize	(offsetof(xl_brin_desummarize, regOffset) + \
								 sizeof(OffsetNumber))


/*
 * brin_redo
 *		Replay a BRIN WAL record during crash recovery or standby replay.
 *		Dispatches on the record's info field to the proper handler
 *		(index create, insert, update, samepage update, revmap extend or
 *		desummarize) so that the on-disk BRIN pages are restored to a
 *		consistent state.
 */

/*
 * brin_redo
 *		在崩溃恢复或备库回放期间重放一条 BRIN WAL 记录。
 *		它根据记录的 info 字段分派到相应的处理函数（索引创建、插入、更新、
 *		同页更新、反向映射扩展或反汇总），从而将磁盘上的 BRIN 页面恢复到
 *		一致的状态。
 */
extern void brin_redo(XLogReaderState *record);

/*
 * brin_desc
 *		Format the contents of a BRIN WAL record into the given StringInfo
 *		for human-readable output, used by tools such as pg_waldump.
 */

/*
 * brin_desc
 *		将一条 BRIN WAL 记录的内容格式化输出到给定的 StringInfo 中，
 *		生成便于阅读的文本，供 pg_waldump 等工具使用。
 */
extern void brin_desc(StringInfo buf, XLogReaderState *record);

/*
 * brin_identify
 *		Return a human-readable name for the given BRIN WAL record info
 *		(operation) code, or NULL if the code is not recognized.
 */

/*
 * brin_identify
 *		返回给定 BRIN WAL 记录 info（操作）码对应的可读名称；若该编码无法
 *		识别则返回 NULL。
 */
extern const char *brin_identify(uint8 info);

/*
 * brin_mask
 *		Mask out the parts of a BRIN page that are allowed to differ between
 *		the original and the replayed copy (e.g. hint bits, unused space),
 *		so that WAL consistency checking does not report spurious mismatches.
 */

/*
 * brin_mask
 *		屏蔽 BRIN 页面中允许在原始副本与回放副本之间存在差异的部分
 *		（例如提示位、未使用空间），从而避免 WAL 一致性检查报告出虚假的
 *		不匹配。
 */
extern void brin_mask(char *pagedata, BlockNumber blkno);

#endif							/* BRIN_XLOG_H */
