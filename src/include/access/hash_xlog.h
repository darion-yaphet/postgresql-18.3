/*-------------------------------------------------------------------------
 *
 * hash_xlog.h
 *	  header file for Postgres hash AM implementation
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_XLOG_H
#define HASH_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/* Number of buffers required for XLOG_HASH_SQUEEZE_PAGE operation */

/* XLOG_HASH_SQUEEZE_PAGE 操作所需的缓冲区数量 */
#define HASH_XLOG_FREE_OVFL_BUFS	6

/*
 * XLOG records for hash operations
 */

/*
 * 哈希操作的 XLOG 记录
 */
#define XLOG_HASH_INIT_META_PAGE	0x00	/* initialize the meta page */

/* 初始化元页 */
#define XLOG_HASH_INIT_BITMAP_PAGE	0x10	/* initialize the bitmap page */

/* 初始化位图页 */
#define XLOG_HASH_INSERT		0x20	/* add index tuple without split */

/* 在不分裂的情况下添加索引元组 */
#define XLOG_HASH_ADD_OVFL_PAGE 0x30	/* add overflow page */

/* 添加溢出页 */
#define XLOG_HASH_SPLIT_ALLOCATE_PAGE	0x40	/* allocate new page for split */

/* 为分裂分配新页 */
#define XLOG_HASH_SPLIT_PAGE	0x50	/* split page */

/* 分裂页 */
#define XLOG_HASH_SPLIT_COMPLETE	0x60	/* completion of split operation */

/* 分裂操作完成 */
#define XLOG_HASH_MOVE_PAGE_CONTENTS	0x70	/* remove tuples from one page
												 * and add to another page */

/* 从一个页移除元组并添加到另一个页 */
#define XLOG_HASH_SQUEEZE_PAGE	0x80	/* add tuples to one of the previous
										 * pages in chain and free the ovfl
										 * page */

/* 将元组添加到链中前面的某个页并释放该溢出页 */
#define XLOG_HASH_DELETE		0x90	/* delete index tuples from a page */

/* 从一个页删除索引元组 */
#define XLOG_HASH_SPLIT_CLEANUP 0xA0	/* clear split-cleanup flag in primary
										 * bucket page after deleting tuples
										 * that are moved due to split	*/

/* 在删除因分裂而移动的元组后，清除主桶页中的 split-cleanup 标志 */
#define XLOG_HASH_UPDATE_META_PAGE	0xB0	/* update meta page after vacuum */

/* 在 vacuum 之后更新元页 */

#define XLOG_HASH_VACUUM_ONE_PAGE	0xC0	/* remove dead tuples from index
											 * page */

/* 从索引页移除 dead 元组 */

/*
 * xl_hash_split_allocate_page flag values, 8 bits are available.
 */

/*
 * xl_hash_split_allocate_page 的标志值，共有 8 位可用。
 */
#define XLH_SPLIT_META_UPDATE_MASKS		(1<<0)
#define XLH_SPLIT_META_UPDATE_SPLITPOINT		(1<<1)

/*
 * This is what we need to know about simple (without split) insert.
 *
 * This data record is used for XLOG_HASH_INSERT
 *
 * Backup Blk 0: original page (data contains the inserted tuple)
 * Backup Blk 1: metapage (HashMetaPageData)
 */

/*
 * 这是关于简单（不涉及分裂）插入我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_INSERT
 *
 * 备份块 0：原始页（数据中包含被插入的元组）
 * 备份块 1：元页（HashMetaPageData）
 */
typedef struct xl_hash_insert
{
	OffsetNumber offnum;
} xl_hash_insert;

#define SizeOfHashInsert	(offsetof(xl_hash_insert, offnum) + sizeof(OffsetNumber))

/*
 * This is what we need to know about addition of overflow page.
 *
 * This data record is used for XLOG_HASH_ADD_OVFL_PAGE
 *
 * Backup Blk 0: newly allocated overflow page
 * Backup Blk 1: page before new overflow page in the bucket chain
 * Backup Blk 2: bitmap page
 * Backup Blk 3: new bitmap page
 * Backup Blk 4: metapage
 */

/*
 * 这是关于添加溢出页我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_ADD_OVFL_PAGE
 *
 * 备份块 0：新分配的溢出页
 * 备份块 1：桶链中位于新溢出页之前的页
 * 备份块 2：位图页
 * 备份块 3：新位图页
 * 备份块 4：元页
 */
typedef struct xl_hash_add_ovfl_page
{
	uint16		bmsize;
	bool		bmpage_found;
} xl_hash_add_ovfl_page;

#define SizeOfHashAddOvflPage	\
	(offsetof(xl_hash_add_ovfl_page, bmpage_found) + sizeof(bool))

/*
 * This is what we need to know about allocating a page for split.
 *
 * This data record is used for XLOG_HASH_SPLIT_ALLOCATE_PAGE
 *
 * Backup Blk 0: page for old bucket
 * Backup Blk 1: page for new bucket
 * Backup Blk 2: metapage
 */

/*
 * 这是关于为分裂分配一个页我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_SPLIT_ALLOCATE_PAGE
 *
 * 备份块 0：旧桶的页
 * 备份块 1：新桶的页
 * 备份块 2：元页
 */
typedef struct xl_hash_split_allocate_page
{
	uint32		new_bucket;
	uint16		old_bucket_flag;
	uint16		new_bucket_flag;
	uint8		flags;
} xl_hash_split_allocate_page;

#define SizeOfHashSplitAllocPage	\
	(offsetof(xl_hash_split_allocate_page, flags) + sizeof(uint8))

/*
 * This is what we need to know about completing the split operation.
 *
 * This data record is used for XLOG_HASH_SPLIT_COMPLETE
 *
 * Backup Blk 0: page for old bucket
 * Backup Blk 1: page for new bucket
 */

/*
 * 这是关于完成分裂操作我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_SPLIT_COMPLETE
 *
 * 备份块 0：旧桶的页
 * 备份块 1：新桶的页
 */
typedef struct xl_hash_split_complete
{
	uint16		old_bucket_flag;
	uint16		new_bucket_flag;
} xl_hash_split_complete;

#define SizeOfHashSplitComplete \
	(offsetof(xl_hash_split_complete, new_bucket_flag) + sizeof(uint16))

/*
 * This is what we need to know about move page contents required during
 * squeeze operation.
 *
 * This data record is used for XLOG_HASH_MOVE_PAGE_CONTENTS
 *
 * Backup Blk 0: primary bucket page
 * Backup Blk 1: page containing moved tuples
 * Backup Blk 2: page from which tuples will be removed
 */

/*
 * 这是关于 squeeze 操作期间所需的移动页内容我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_MOVE_PAGE_CONTENTS
 *
 * 备份块 0：主桶页
 * 备份块 1：包含被移动元组的页
 * 备份块 2：将从中移除元组的页
 */
typedef struct xl_hash_move_page_contents
{
	uint16		ntups;
	bool		is_prim_bucket_same_wrt;	/* true if the page to which
											 * tuples are moved is same as
											 * primary bucket page */

	/* 若元组被移动到的页与主桶页相同则为 true */
} xl_hash_move_page_contents;

#define SizeOfHashMovePageContents	\
	(offsetof(xl_hash_move_page_contents, is_prim_bucket_same_wrt) + sizeof(bool))

/*
 * This is what we need to know about the squeeze page operation.
 *
 * This data record is used for XLOG_HASH_SQUEEZE_PAGE
 *
 * Backup Blk 0: primary bucket page
 * Backup Blk 1: page containing tuples moved from freed overflow page
 * Backup Blk 2: freed overflow page
 * Backup Blk 3: page previous to the freed overflow page
 * Backup Blk 4: page next to the freed overflow page
 * Backup Blk 5: bitmap page containing info of freed overflow page
 * Backup Blk 6: meta page
 */

/*
 * 这是关于 squeeze 页操作我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_SQUEEZE_PAGE
 *
 * 备份块 0：主桶页
 * 备份块 1：包含从已释放溢出页移入元组的页
 * 备份块 2：已释放的溢出页
 * 备份块 3：位于已释放溢出页之前的页
 * 备份块 4：位于已释放溢出页之后的页
 * 备份块 5：包含已释放溢出页信息的位图页
 * 备份块 6：元页
 */
typedef struct xl_hash_squeeze_page
{
	BlockNumber prevblkno;
	BlockNumber nextblkno;
	uint16		ntups;
	bool		is_prim_bucket_same_wrt;	/* true if the page to which
											 * tuples are moved is same as
											 * primary bucket page */

	/* 若元组被移动到的页与主桶页相同则为 true */
	bool		is_prev_bucket_same_wrt;	/* true if the page to which
											 * tuples are moved is the page
											 * previous to the freed overflow
											 * page */

	/* 若元组被移动到的页是位于已释放溢出页之前的页则为 true */
} xl_hash_squeeze_page;

#define SizeOfHashSqueezePage	\
	(offsetof(xl_hash_squeeze_page, is_prev_bucket_same_wrt) + sizeof(bool))

/*
 * This is what we need to know about the deletion of index tuples from a page.
 *
 * This data record is used for XLOG_HASH_DELETE
 *
 * Backup Blk 0: primary bucket page
 * Backup Blk 1: page from which tuples are deleted
 */

/*
 * 这是关于从一个页删除索引元组我们需要知道的信息。
 *
 * 该数据记录用于 XLOG_HASH_DELETE
 *
 * 备份块 0：主桶页
 * 备份块 1：将从中删除元组的页
 */
typedef struct xl_hash_delete
{
	bool		clear_dead_marking; /* true if this operation clears
									 * LH_PAGE_HAS_DEAD_TUPLES flag */

	/* 若本操作清除 LH_PAGE_HAS_DEAD_TUPLES 标志则为 true */
	bool		is_primary_bucket_page; /* true if the operation is for
										 * primary bucket page */

	/* 若本操作针对主桶页则为 true */
} xl_hash_delete;

#define SizeOfHashDelete	(offsetof(xl_hash_delete, is_primary_bucket_page) + sizeof(bool))

/*
 * This is what we need for metapage update operation.
 *
 * This data record is used for XLOG_HASH_UPDATE_META_PAGE
 *
 * Backup Blk 0: meta page
 */

/*
 * 这是元页更新操作我们所需的信息。
 *
 * 该数据记录用于 XLOG_HASH_UPDATE_META_PAGE
 *
 * 备份块 0：元页
 */
typedef struct xl_hash_update_meta_page
{
	double		ntuples;
} xl_hash_update_meta_page;

#define SizeOfHashUpdateMetaPage	\
	(offsetof(xl_hash_update_meta_page, ntuples) + sizeof(double))

/*
 * This is what we need to initialize metapage.
 *
 * This data record is used for XLOG_HASH_INIT_META_PAGE
 *
 * Backup Blk 0: meta page
 */

/*
 * 这是初始化元页我们所需的信息。
 *
 * 该数据记录用于 XLOG_HASH_INIT_META_PAGE
 *
 * 备份块 0：元页
 */
typedef struct xl_hash_init_meta_page
{
	double		num_tuples;
	RegProcedure procid;
	uint16		ffactor;
} xl_hash_init_meta_page;

#define SizeOfHashInitMetaPage		\
	(offsetof(xl_hash_init_meta_page, ffactor) + sizeof(uint16))

/*
 * This is what we need to initialize bitmap page.
 *
 * This data record is used for XLOG_HASH_INIT_BITMAP_PAGE
 *
 * Backup Blk 0: bitmap page
 * Backup Blk 1: meta page
 */

/*
 * 这是初始化位图页我们所需的信息。
 *
 * 该数据记录用于 XLOG_HASH_INIT_BITMAP_PAGE
 *
 * 备份块 0：位图页
 * 备份块 1：元页
 */
typedef struct xl_hash_init_bitmap_page
{
	uint16		bmsize;
} xl_hash_init_bitmap_page;

#define SizeOfHashInitBitmapPage	\
	(offsetof(xl_hash_init_bitmap_page, bmsize) + sizeof(uint16))

/*
 * This is what we need for index tuple deletion and to
 * update the meta page.
 *
 * This data record is used for XLOG_HASH_VACUUM_ONE_PAGE
 *
 * Backup Blk 0: primary bucket page
 * Backup Blk 1: meta page
 */

/*
 * 这是索引元组删除以及更新元页我们所需的信息。
 *
 * 该数据记录用于 XLOG_HASH_VACUUM_ONE_PAGE
 *
 * 备份块 0：主桶页
 * 备份块 1：元页
 */
typedef struct xl_hash_vacuum_one_page
{
	TransactionId snapshotConflictHorizon;
	uint16		ntuples;
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* 用于处理备库上逻辑解码期间的恢复冲突 */

	/* TARGET OFFSET NUMBERS */

	/* 目标偏移号 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_hash_vacuum_one_page;

#define SizeOfHashVacuumOnePage offsetof(xl_hash_vacuum_one_page, offsets)

/*
 * Replay a hash-index WAL record during recovery, dispatching on the record's
 * info byte to apply the corresponding page changes.
 */

/*
 * 在恢复期间重放一条哈希索引 WAL 记录，根据记录的 info 字节进行分派，
 * 以应用相应的页更改。
 */
extern void hash_redo(XLogReaderState *record);

/*
 * Describe a hash-index WAL record for debugging tools, appending a
 * human-readable summary of its contents to the given buffer.
 */

/*
 * 为调试工具描述一条哈希索引 WAL 记录，将其内容的
 * 可读摘要追加到给定的缓冲区。
 */
extern void hash_desc(StringInfo buf, XLogReaderState *record);

/*
 * Return a human-readable name for a hash-index WAL record type given its info
 * byte.
 */

/*
 * 根据给定的 info 字节，返回哈希索引 WAL 记录类型的可读名称。
 */
extern const char *hash_identify(uint8 info);

/*
 * Mask out volatile, non-deterministic parts of a hash-index page so that WAL
 * consistency checking can compare pages meaningfully.
 */

/*
 * 屏蔽哈希索引页中易变的、不确定的部分，以便 WAL 一致性检查
 * 能够有意义地比较页。
 */
extern void hash_mask(char *pagedata, BlockNumber blkno);

#endif							/* HASH_XLOG_H */
