/*-------------------------------------------------------------------------
 *
 * gistxlog.h
 *	  gist xlog routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gistxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_XLOG_H
#define GIST_XLOG_H

#include "access/gist.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"

#define XLOG_GIST_PAGE_UPDATE		0x00
#define XLOG_GIST_DELETE			0x10	/* delete leaf index tuples for a
											 * page */

/* 删除某个页面的叶子索引元组 */
#define XLOG_GIST_PAGE_REUSE		0x20	/* old page is about to be reused
											 * from FSM */

/* 旧页面即将从 FSM 中被重新利用 */
#define XLOG_GIST_PAGE_SPLIT		0x30
 /* #define XLOG_GIST_INSERT_COMPLETE	 0x40 */	/* not used anymore */

 /* 不再使用 */
 /* #define XLOG_GIST_CREATE_INDEX		 0x50 */	/* not used anymore */

 /* 不再使用 */
#define XLOG_GIST_PAGE_DELETE		0x60
#define XLOG_GIST_ASSIGN_LSN		0x70	/* nop, assign new LSN */

/* 空操作，分配新的 LSN */

/*
 * Backup Blk 0: updated page.
 * Backup Blk 1: If this operation completes a page split, by inserting a
 *				 downlink for the split page, the left half of the split
 */

/*
 * 备份块 0：被更新的页面。
 * 备份块 1：如果此操作通过为已分裂的页面插入 downlink 从而完成一次页面分裂，
 *				 则为该次分裂的左半部分。
 */
typedef struct gistxlogPageUpdate
{
	/* number of deleted offsets */

	/* 被删除的偏移量的数量 */
	uint16		ntodelete;
	uint16		ntoinsert;

	/*
	 * In payload of blk 0 : 1. todelete OffsetNumbers 2. tuples to insert
	 */

	/*
	 * 在块 0 的负载（payload）中：1. 待删除的 OffsetNumber 2. 待插入的元组
	 */
} gistxlogPageUpdate;

/*
 * Backup Blk 0: Leaf page, whose index tuples are deleted.
 */

/*
 * 备份块 0：叶子页面，其索引元组将被删除。
 */
typedef struct gistxlogDelete
{
	TransactionId snapshotConflictHorizon;
	uint16		ntodelete;		/* number of deleted offsets */

	/* 被删除的偏移量的数量 */
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* 用于处理在备库上进行逻辑解码期间的恢复冲突 */

	/* TODELETE OFFSET NUMBERS */

	/* 待删除的偏移量编号 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} gistxlogDelete;

#define SizeOfGistxlogDelete	offsetof(gistxlogDelete, offsets)

/*
 * Backup Blk 0: If this operation completes a page split, by inserting a
 *				 downlink for the split page, the left half of the split
 * Backup Blk 1 - npage: split pages (1 is the original page)
 */

/*
 * 备份块 0：如果此操作通过为已分裂的页面插入 downlink 从而完成一次页面分裂，
 *				 则为该次分裂的左半部分。
 * 备份块 1 - npage：分裂产生的各个页面（其中第 1 个是原始页面）。
 */
typedef struct gistxlogPageSplit
{
	BlockNumber origrlink;		/* rightlink of the page before split */

	/* 分裂前页面的 rightlink（右链接） */
	GistNSN		orignsn;		/* NSN of the page before split */

	/* 分裂前页面的 NSN */
	bool		origleaf;		/* was split page a leaf page? */

	/* 被分裂的页面曾经是叶子页面吗？ */

	uint16		npage;			/* # of pages in the split */

	/* 本次分裂中的页面数量 */
	bool		markfollowright;	/* set F_FOLLOW_RIGHT flags */

	/* 设置 F_FOLLOW_RIGHT 标志 */

	/*
	 * follow: 1. gistxlogPage and array of IndexTupleData per page
	 */

	/*
	 * 紧随其后：1. 每个页面对应的 gistxlogPage 以及 IndexTupleData 数组
	 */
} gistxlogPageSplit;

/*
 * Backup Blk 0: page that was deleted.
 * Backup Blk 1: parent page, containing the downlink to the deleted page.
 */

/*
 * 备份块 0：被删除的页面。
 * 备份块 1：父页面，包含指向被删除页面的 downlink。
 */
typedef struct gistxlogPageDelete
{
	FullTransactionId deleteXid;	/* last Xid which could see page in scan */

	/* 在扫描中仍有可能看到该页面的最后一个 Xid */
	OffsetNumber downlinkOffset;	/* Offset of downlink referencing this
									 * page */

	/* 引用此页面的 downlink 的偏移量 */
} gistxlogPageDelete;

#define SizeOfGistxlogPageDelete	(offsetof(gistxlogPageDelete, downlinkOffset) + sizeof(OffsetNumber))


/*
 * This is what we need to know about page reuse, for hot standby.
 */

/*
 * 这是为了热备（hot standby）而言，我们需要了解的关于页面重用的信息。
 */
typedef struct gistxlogPageReuse
{
	RelFileLocator locator;
	BlockNumber block;
	FullTransactionId snapshotConflictHorizon;
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* 用于处理在备库上进行逻辑解码期间的恢复冲突 */
} gistxlogPageReuse;

#define SizeOfGistxlogPageReuse	(offsetof(gistxlogPageReuse, isCatalogRel) + sizeof(bool))

/*
 * WAL redo handler for GiST index records: dispatch on the record's info
 * byte and replay the corresponding operation (page update, delete, split,
 * page delete, page reuse, or LSN assignment) during crash recovery or on a
 * standby.
 */

/*
 * GiST 索引记录的 WAL 重做（redo）处理程序：根据记录的 info 字节进行分派，
 * 并在崩溃恢复期间或在备库上重放相应的操作（页面更新、删除、分裂、页面删除、
 * 页面重用或 LSN 分配）。
 */
extern void gist_redo(XLogReaderState *record);

/*
 * Append a human-readable description of a GiST WAL record to the given
 * StringInfo, used by tools such as pg_waldump to display record contents.
 */

/*
 * 将某条 GiST WAL 记录的可读描述追加到给定的 StringInfo 中，供 pg_waldump 之类的
 * 工具用于显示记录内容。
 */
extern void gist_desc(StringInfo buf, XLogReaderState *record);

/*
 * Return the textual name of a GiST WAL record type given its info byte,
 * used for identifying record kinds when dumping or describing WAL.
 */

/*
 * 根据给定的 info 字节返回某种 GiST WAL 记录类型的文本名称，
 * 用于在转储或描述 WAL 时识别记录种类。
 */
extern const char *gist_identify(uint8 info);

/*
 * Startup hook for the GiST WAL resource manager, invoked at the beginning of
 * recovery to initialize any state needed while replaying GiST records.
 */

/*
 * GiST WAL 资源管理器的启动钩子，在恢复开始时被调用，用于初始化重放 GiST 记录
 * 期间所需的任何状态。
 */
extern void gist_xlog_startup(void);

/*
 * Cleanup hook for the GiST WAL resource manager, invoked at the end of
 * recovery to release any state allocated during GiST record replay.
 */

/*
 * GiST WAL 资源管理器的清理钩子，在恢复结束时被调用，用于释放在 GiST 记录重放
 * 期间分配的任何状态。
 */
extern void gist_xlog_cleanup(void);

/*
 * Mask out non-deterministic fields of a GiST page before comparison, so that
 * pages can be checked for consistency (e.g. by wal_consistency_checking)
 * without spurious differences caused by hint bits or similar volatile data.
 */

/*
 * 在比较之前掩去 GiST 页面中不确定的字段，以便可以对页面进行一致性检查
 * （例如通过 wal_consistency_checking），而不会因提示位（hint bit）或类似的
 * 易变数据而产生虚假的差异。
 */
extern void gist_mask(char *pagedata, BlockNumber blkno);

#endif
