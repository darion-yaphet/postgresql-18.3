/*--------------------------------------------------------------------------
 * ginxlog.h
 *	  header file for postgres inverted index xlog implementation.
 *
 *	Copyright (c) 2006-2025, PostgreSQL Global Development Group
 *
 *	src/include/access/ginxlog.h
 *--------------------------------------------------------------------------
 */
#ifndef GINXLOG_H
#define GINXLOG_H

#include "access/ginblock.h"
#include "access/itup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

#define XLOG_GIN_CREATE_PTREE  0x10

typedef struct ginxlogCreatePostingTree
{
	uint32		size;
	/* A compressed posting list follows */

	/* 其后跟随一个压缩的倒排列表 */
} ginxlogCreatePostingTree;

/*
 * The format of the insertion record varies depending on the page type.
 * ginxlogInsert is the common part between all variants.
 *
 * Backup Blk 0: target page
 * Backup Blk 1: left child, if this insertion finishes an incomplete split
 */

/*
 * 插入记录的格式因页面类型而异。
 * ginxlogInsert 是所有变体之间的公共部分。
 *
 * 备份块 0：目标页
 * 备份块 1：左子页（如果此次插入完成了一个未完成的分裂）
 */

#define XLOG_GIN_INSERT  0x20

typedef struct
{
	uint16		flags;			/* GIN_INSERT_ISLEAF and/or GIN_INSERT_ISDATA */

	/* GIN_INSERT_ISLEAF 和/或 GIN_INSERT_ISDATA */

	/*
	 * FOLLOWS:
	 *
	 * 1. if not leaf page, block numbers of the left and right child pages
	 * whose split this insertion finishes, as BlockIdData[2] (beware of
	 * adding fields in this struct that would make them not 16-bit aligned)
	 *
	 * 2. a ginxlogInsertEntry or ginxlogRecompressDataLeaf struct, depending
	 * on tree type.
	 *
	 * NB: the below structs are only 16-bit aligned when appended to a
	 * ginxlogInsert struct! Beware of adding fields to them that require
	 * stricter alignment.
	 */

	/*
	 * 其后跟随：
	 *
	 * 1. 如果不是叶子页，则为此次插入所完成分裂的左、右子页的块号，
	 * 形式为 BlockIdData[2]（当心：在本结构体中添加字段可能会使它们不再 16 位对齐）
	 *
	 * 2. 一个 ginxlogInsertEntry 或 ginxlogRecompressDataLeaf 结构体，
	 * 具体取决于树的类型。
	 *
	 * 注意：只有当下面这些结构体被追加到 ginxlogInsert 结构体之后时，
	 * 它们才是 16 位对齐的！当心向它们中添加需要更严格对齐的字段。
	 */
} ginxlogInsert;

typedef struct
{
	OffsetNumber offset;
	bool		isDelete;
	IndexTupleData tuple;		/* variable length */

	/* 变长 */
} ginxlogInsertEntry;


typedef struct
{
	uint16		nactions;

	/* Variable number of 'actions' follow */

	/* 其后跟随可变数量的“动作”（actions） */
} ginxlogRecompressDataLeaf;

/*
 * Note: this struct is currently not used in code, and only acts as
 * documentation. The WAL record format is as specified here, but the code
 * uses straight access through a Pointer and memcpy to read/write these.
 */

/*
 * 注意：此结构体目前在代码中并未使用，仅作为文档说明。
 * WAL 记录的格式如这里所指定，但代码是通过 Pointer 直接访问并使用 memcpy
 * 来读写这些数据的。
 */
typedef struct
{
	uint8		segno;			/* segment this action applies to */

	/* 此动作所作用的段（segment） */
	char		type;			/* action type (see below) */

	/* 动作类型（见下文） */

	/*
	 * Action-specific data follows. For INSERT and REPLACE actions that is a
	 * GinPostingList struct. For ADDITEMS, a uint16 for the number of items
	 * added, followed by the items themselves as ItemPointers. DELETE actions
	 * have no further data.
	 */

	/*
	 * 其后跟随与动作相关的数据。对于 INSERT 和 REPLACE 动作，
	 * 它是一个 GinPostingList 结构体。对于 ADDITEMS，先是一个表示所添加项数量的
	 * uint16，随后是这些项本身（以 ItemPointer 形式）。DELETE 动作没有更多数据。
	 */
}			ginxlogSegmentAction;

/* Action types */

/* 动作类型 */
#define GIN_SEGMENT_UNMODIFIED	0	/* no action (not used in WAL records) */

/* 无动作（不在 WAL 记录中使用） */
#define GIN_SEGMENT_DELETE		1	/* a whole segment is removed */

/* 移除整个段 */
#define GIN_SEGMENT_INSERT		2	/* a whole segment is added */

/* 添加整个段 */
#define GIN_SEGMENT_REPLACE		3	/* a segment is replaced */

/* 替换某个段 */
#define GIN_SEGMENT_ADDITEMS	4	/* items are added to existing segment */

/* 向已有段中添加项 */

typedef struct
{
	OffsetNumber offset;
	PostingItem newitem;
} ginxlogInsertDataInternal;

/*
 * Backup Blk 0: new left page (= original page, if not root split)
 * Backup Blk 1: new right page
 * Backup Blk 2: original page / new root page, if root split
 * Backup Blk 3: left child, if this insertion completes an earlier split
 */

/*
 * 备份块 0：新的左页（如果不是根分裂，则等于原页）
 * 备份块 1：新的右页
 * 备份块 2：原页 / 新的根页（如果发生根分裂）
 * 备份块 3：左子页（如果此次插入完成了先前的一次分裂）
 */
#define XLOG_GIN_SPLIT	0x30

typedef struct ginxlogSplit
{
	RelFileLocator locator;
	BlockNumber rrlink;			/* right link, or root's blocknumber if root
								 * split */

	/* 右链接；如果是根分裂，则为根的块号 */
	BlockNumber leftChildBlkno; /* valid on a non-leaf split */

	/* 在非叶子分裂时有效 */
	BlockNumber rightChildBlkno;
	uint16		flags;			/* see below */

	/* 见下文 */
} ginxlogSplit;

/*
 * Flags used in ginxlogInsert and ginxlogSplit records
 */

/*
 * 在 ginxlogInsert 和 ginxlogSplit 记录中使用的标志。
 */
#define GIN_INSERT_ISDATA	0x01	/* for both insert and split records */

/* 同时用于插入记录和分裂记录 */
#define GIN_INSERT_ISLEAF	0x02	/* ditto */

/* 同上 */
#define GIN_SPLIT_ROOT		0x04	/* only for split records */

/* 仅用于分裂记录 */

/*
 * Vacuum simply WAL-logs the whole page, when anything is modified. This
 * is functionally identical to XLOG_FPI records, but is kept separate for
 * debugging purposes. (When inspecting the WAL stream, it's easier to see
 * what's going on when GIN vacuum records are marked as such, not as heap
 * records.) This is currently only used for entry tree leaf pages.
 */

/*
 * 当有任何内容被修改时，vacuum 只是简单地对整个页面记录 WAL。
 * 这在功能上与 XLOG_FPI 记录完全相同，但为了便于调试而单独保留。
 *（在检查 WAL 流时，如果 GIN vacuum 记录被标记为其本身、而非堆记录，
 * 会更容易看清正在发生什么。）目前它仅用于条目树的叶子页。
 */
#define XLOG_GIN_VACUUM_PAGE	0x40

/*
 * Vacuuming posting tree leaf page is WAL-logged like recompression caused
 * by insertion.
 */

/*
 * 对倒排树叶子页进行 vacuum 时，其 WAL 记录方式与插入引起的重新压缩相同。
 */
#define XLOG_GIN_VACUUM_DATA_LEAF_PAGE	0x90

typedef struct ginxlogVacuumDataLeafPage
{
	ginxlogRecompressDataLeaf data;
} ginxlogVacuumDataLeafPage;

/*
 * Backup Blk 0: deleted page
 * Backup Blk 1: parent
 * Backup Blk 2: left sibling
 */

/*
 * 备份块 0：被删除的页
 * 备份块 1：父页
 * 备份块 2：左兄弟页
 */
#define XLOG_GIN_DELETE_PAGE	0x50

typedef struct ginxlogDeletePage
{
	OffsetNumber parentOffset;
	BlockNumber rightLink;
	TransactionId deleteXid;	/* last Xid which could see this page in scan */

	/* 在扫描中仍可能看到此页的最后一个 Xid */
} ginxlogDeletePage;

#define XLOG_GIN_UPDATE_META_PAGE 0x60

/*
 * Backup Blk 0: metapage
 * Backup Blk 1: tail page
 */

/*
 * 备份块 0：元页
 * 备份块 1：尾页
 */
typedef struct ginxlogUpdateMeta
{
	RelFileLocator locator;
	GinMetaPageData metadata;
	BlockNumber prevTail;
	BlockNumber newRightlink;
	int32		ntuples;		/* if ntuples > 0 then metadata.tail was
								 * updated with that many tuples; else new sub
								 * list was inserted */

	/*
	 * 如果 ntuples > 0，则表示 metadata.tail 被更新了这么多个元组；
	 * 否则表示插入了一个新的子列表。
	 */
	/* array of inserted tuples follows */

	/* 其后跟随被插入元组的数组 */
} ginxlogUpdateMeta;

#define XLOG_GIN_INSERT_LISTPAGE  0x70

typedef struct ginxlogInsertListPage
{
	BlockNumber rightlink;
	int32		ntuples;
	/* array of inserted tuples follows */

	/* 其后跟随被插入元组的数组 */
} ginxlogInsertListPage;

/*
 * Backup Blk 0: metapage
 * Backup Blk 1 to (ndeleted + 1): deleted pages
 */

/*
 * 备份块 0：元页
 * 备份块 1 到 (ndeleted + 1)：被删除的页
 */

#define XLOG_GIN_DELETE_LISTPAGE  0x80

/*
 * The WAL record for deleting list pages must contain a block reference to
 * all the deleted pages, so the number of pages that can be deleted in one
 * record is limited by XLR_MAX_BLOCK_ID. (block_id 0 is used for the
 * metapage.)
 */

/*
 * 删除列表页的 WAL 记录必须包含对所有被删除页的块引用，
 * 因此一条记录中可删除的页数受 XLR_MAX_BLOCK_ID 限制。
 *（block_id 0 用于元页。）
 */
#define GIN_NDELETE_AT_ONCE Min(16, XLR_MAX_BLOCK_ID - 1)
typedef struct ginxlogDeleteListPages
{
	GinMetaPageData metadata;
	int32		ndeleted;
} ginxlogDeleteListPages;

/*
 * Main WAL redo dispatcher for GIN: given a decoded WAL record, inspect its
 * info byte and replay the corresponding GIN operation to bring index pages
 * up to date during crash recovery or replication.
 *
 * GIN 的主 WAL 重做（redo）分发器：给定一条已解码的 WAL 记录，
 * 检查其 info 字节并重放相应的 GIN 操作，
 * 以便在崩溃恢复或复制期间使索引页面保持最新。
 */
extern void gin_redo(XLogReaderState *record);

/*
 * Produce a human-readable textual description of a GIN WAL record's contents
 * into the given StringInfo, used by tools such as pg_waldump.
 *
 * 将一条 GIN WAL 记录内容的可读文本描述输出到给定的 StringInfo 中，
 * 供 pg_waldump 等工具使用。
 */
extern void gin_desc(StringInfo buf, XLogReaderState *record);

/*
 * Map a GIN WAL record's info byte to the short name of its record type,
 * used when identifying records for display.
 *
 * 将一条 GIN WAL 记录的 info 字节映射为其记录类型的简短名称，
 * 用于在显示时标识记录。
 */
extern const char *gin_identify(uint8 info);

/*
 * Startup hook for GIN WAL replay: initialize any state (such as the list of
 * incomplete splits) needed before GIN redo processing begins.
 *
 * GIN WAL 重放的启动钩子：在 GIN 重做处理开始之前，
 * 初始化所需的任何状态（例如未完成分裂的列表）。
 *
 */
extern void gin_xlog_startup(void);

/*
 * Cleanup hook for GIN WAL replay: finish any pending work and release state
 * set up by gin_xlog_startup once redo processing has completed.
 *
 * GIN WAL 重放的清理钩子：在重做处理完成后，
 * 完成任何未决工作并释放由 gin_xlog_startup 建立的状态。
 */
extern void gin_xlog_cleanup(void);

/*
 * Mask out non-deterministic parts of a GIN page (such as unused space and
 * hint bits) so that pages can be compared for consistency checking, e.g. by
 * wal_consistency_checking.
 *
 * 屏蔽 GIN 页面中不确定的部分（例如未使用空间和提示位），
 * 以便可以对页面进行一致性检查比较，例如由 wal_consistency_checking 使用。
 */
extern void gin_mask(char *pagedata, BlockNumber blkno);

#endif							/* GINXLOG_H */
