/*-------------------------------------------------------------------------
 *
 * spgxlog.h
 *	  xlog declarations for SP-GiST access method.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGXLOG_H
#define SPGXLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/* XLOG record types for SPGiST */

/* SP-GiST 的 XLOG 记录类型 */
 /* #define XLOG_SPGIST_CREATE_INDEX       0x00 */	/* not used anymore */

 /* 该记录类型已废弃，不再使用 */
#define XLOG_SPGIST_ADD_LEAF		0x10
#define XLOG_SPGIST_MOVE_LEAFS		0x20
#define XLOG_SPGIST_ADD_NODE		0x30
#define XLOG_SPGIST_SPLIT_TUPLE		0x40
#define XLOG_SPGIST_PICKSPLIT		0x50
#define XLOG_SPGIST_VACUUM_LEAF		0x60
#define XLOG_SPGIST_VACUUM_ROOT		0x70
#define XLOG_SPGIST_VACUUM_REDIRECT 0x80

/*
 * Some redo functions need an SpGistState, although only a few of its fields
 * need to be valid.  spgxlogState carries the required info in xlog records.
 * (See fillFakeState in spgxlog.c for more comments.)
 */

/*
 * 某些 redo 函数需要一个 SpGistState，尽管其中只有少数字段需要是有效的。
 * spgxlogState 在 xlog 记录中携带这些所需的信息。
 * （更多说明请参见 spgxlog.c 中的 fillFakeState。）
 */
typedef struct spgxlogState
{
	TransactionId redirectXid;
	bool		isBuild;
} spgxlogState;

/*
 * Backup Blk 0: destination page for leaf tuple
 * Backup Blk 1: parent page (if any)
 */

/*
 * 备份块 0：叶子元组的目标页面
 * 备份块 1：父页面（如果存在）
 */
typedef struct spgxlogAddLeaf
{
	bool		newPage;		/* init dest page? */

	/* 是否初始化目标页面？ */
	bool		storesNulls;	/* page is in the nulls tree? */

	/* 页面是否位于 nulls 树中？ */
	OffsetNumber offnumLeaf;	/* offset where leaf tuple gets placed */

	/* 叶子元组被放置的偏移位置 */
	OffsetNumber offnumHeadLeaf;	/* offset of head tuple in chain, if any */

	/* 链中头部元组的偏移位置（如果存在） */

	OffsetNumber offnumParent;	/* where the parent downlink is, if any */

	/* 父级下行链接所在的位置（如果存在） */
	uint16		nodeI;

	/* new leaf tuple follows (unaligned!) */

	/* 随后跟着新的叶子元组（未对齐！） */
} spgxlogAddLeaf;

/*
 * Backup Blk 0: source leaf page
 * Backup Blk 1: destination leaf page
 * Backup Blk 2: parent page
 */

/*
 * 备份块 0：源叶子页面
 * 备份块 1：目标叶子页面
 * 备份块 2：父页面
 */
typedef struct spgxlogMoveLeafs
{
	uint16		nMoves;			/* number of tuples moved from source page */

	/* 从源页面移动的元组数量 */
	bool		newPage;		/* init dest page? */

	/* 是否初始化目标页面？ */
	bool		replaceDead;	/* are we replacing a DEAD source tuple? */

	/* 我们是否正在替换一个 DEAD 状态的源元组？ */
	bool		storesNulls;	/* pages are in the nulls tree? */

	/* 这些页面是否位于 nulls 树中？ */

	/* where the parent downlink is */

	/* 父级下行链接所在的位置 */
	OffsetNumber offnumParent;
	uint16		nodeI;

	spgxlogState stateSrc;

	/*----------
	 * data follows:
	 *		array of deleted tuple numbers, length nMoves
	 *		array of inserted tuple numbers, length nMoves + 1 or 1
	 *		list of leaf tuples, length nMoves + 1 or 1 (unaligned!)
	 *
	 * Note: if replaceDead is true then there is only one inserted tuple
	 * number and only one leaf tuple in the data, because we are not copying
	 * the dead tuple from the source
	 *----------
	 */

	/*----------
	 * 随后跟着的数据：
	 *		被删除元组编号的数组，长度为 nMoves
	 *		被插入元组编号的数组，长度为 nMoves + 1 或 1
	 *		叶子元组列表，长度为 nMoves + 1 或 1（未对齐！）
	 *
	 * 注意：如果 replaceDead 为 true，则数据中只有一个被插入的元组
	 * 编号和一个叶子元组，因为我们不会从源页面复制那个 dead 元组
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogMoveLeafs;

#define SizeOfSpgxlogMoveLeafs	offsetof(spgxlogMoveLeafs, offsets)

/*
 * Backup Blk 0: original page
 * Backup Blk 1: where new tuple goes, if not same place
 * Backup Blk 2: where parent downlink is, if updated and different from
 *				 the old and new
 */

/*
 * 备份块 0：原始页面
 * 备份块 1：新元组的存放位置（如果不在同一位置）
 * 备份块 2：父级下行链接所在的位置（如果它被更新且不同于
 *				 旧页面和新页面）
 */
typedef struct spgxlogAddNode
{
	/*
	 * Offset of the original inner tuple, in the original page (on backup
	 * block 0).
	 */

	/*
	 * 原始内部元组在原始页面（备份块 0）中的偏移位置。
	 */
	OffsetNumber offnum;

	/*
	 * Offset of the new tuple, on the new page (on backup block 1). Invalid,
	 * if we overwrote the old tuple in the original page).
	 */

	/*
	 * 新元组在新页面（备份块 1）中的偏移位置。如果我们在原始页面中
	 * 覆盖了旧元组，则此值无效。
	 */
	OffsetNumber offnumNew;
	bool		newPage;		/* init new page? */

	/* 是否初始化新页面？ */

	/*----
	 * Where is the parent downlink? parentBlk indicates which page it's on,
	 * and offnumParent is the offset within the page. The possible values for
	 * parentBlk are:
	 *
	 * 0: parent == original page
	 * 1: parent == new page
	 * 2: parent == different page (blk ref 2)
	 * -1: parent not updated
	 *----
	 */

	/*----
	 * 父级下行链接在哪里？parentBlk 指示它位于哪个页面上，
	 * 而 offnumParent 是页面内的偏移。parentBlk 可能的取值为：
	 *
	 * 0：父级 == 原始页面
	 * 1：父级 == 新页面
	 * 2：父级 == 不同的页面（块引用 2）
	 * -1：父级未被更新
	 *----
	 */
	int8		parentBlk;
	OffsetNumber offnumParent;	/* offset within the parent page */

	/* 父页面内的偏移位置 */

	uint16		nodeI;

	spgxlogState stateSrc;

	/*
	 * updated inner tuple follows (unaligned!)
	 */

	/*
	 * 随后跟着更新后的内部元组（未对齐！）
	 */
} spgxlogAddNode;

/*
 * Backup Blk 0: where the prefix tuple goes
 * Backup Blk 1: where the postfix tuple goes (if different page)
 */

/*
 * 备份块 0：前缀元组的存放位置
 * 备份块 1：后缀元组的存放位置（如果在不同页面）
 */
typedef struct spgxlogSplitTuple
{
	/* where the prefix tuple goes */

	/* 前缀元组的存放位置 */
	OffsetNumber offnumPrefix;

	/* where the postfix tuple goes */

	/* 后缀元组的存放位置 */
	OffsetNumber offnumPostfix;
	bool		newPage;		/* need to init that page? */

	/* 是否需要初始化该页面？ */
	bool		postfixBlkSame; /* was postfix tuple put on same page as
								 * prefix? */

	/* 后缀元组是否被放在与前缀相同的页面上？ */

	/*
	 * new prefix inner tuple follows, then new postfix inner tuple (both are
	 * unaligned!)
	 */

	/*
	 * 随后跟着新的前缀内部元组，然后是新的后缀内部元组（两者都是
	 * 未对齐的！）
	 */
} spgxlogSplitTuple;

/*
 * Buffer references in the rdata array are:
 * Backup Blk 0: Src page (only if not root)
 * Backup Blk 1: Dest page (if used)
 * Backup Blk 2: Inner page
 * Backup Blk 3: Parent page (if any, and different from Inner)
 */

/*
 * rdata 数组中的缓冲区引用为：
 * 备份块 0：源页面（仅当不是根页面时）
 * 备份块 1：目标页面（如果使用）
 * 备份块 2：内部页面
 * 备份块 3：父页面（如果存在，且不同于内部页面）
 */
typedef struct spgxlogPickSplit
{
	bool		isRootSplit;

	uint16		nDelete;		/* n to delete from Src */

	/* 要从源页面删除的数量 */
	uint16		nInsert;		/* n to insert on Src and/or Dest */

	/* 要在源页面和/或目标页面插入的数量 */
	bool		initSrc;		/* re-init the Src page? */

	/* 是否重新初始化源页面？ */
	bool		initDest;		/* re-init the Dest page? */

	/* 是否重新初始化目标页面？ */

	/* where to put new inner tuple */

	/* 新内部元组的存放位置 */
	OffsetNumber offnumInner;
	bool		initInner;		/* re-init the Inner page? */

	/* 是否重新初始化内部页面？ */

	bool		storesNulls;	/* pages are in the nulls tree? */

	/* 这些页面是否位于 nulls 树中？ */

	/* where the parent downlink is, if any */

	/* 父级下行链接所在的位置（如果存在） */
	bool		innerIsParent;	/* is parent the same as inner page? */

	/* 父级是否与内部页面相同？ */
	OffsetNumber offnumParent;
	uint16		nodeI;

	spgxlogState stateSrc;

	/*----------
	 * data follows:
	 *		array of deleted tuple numbers, length nDelete
	 *		array of inserted tuple numbers, length nInsert
	 *		array of page selector bytes for inserted tuples, length nInsert
	 *		new inner tuple (unaligned!)
	 *		list of leaf tuples, length nInsert (unaligned!)
	 *----------
	 */

	/*----------
	 * 随后跟着的数据：
	 *		被删除元组编号的数组，长度为 nDelete
	 *		被插入元组编号的数组，长度为 nInsert
	 *		被插入元组的页面选择字节数组，长度为 nInsert
	 *		新的内部元组（未对齐！）
	 *		叶子元组列表，长度为 nInsert（未对齐！）
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogPickSplit;

#define SizeOfSpgxlogPickSplit offsetof(spgxlogPickSplit, offsets)

typedef struct spgxlogVacuumLeaf
{
	uint16		nDead;			/* number of tuples to become DEAD */

	/* 将要变为 DEAD 状态的元组数量 */
	uint16		nPlaceholder;	/* number of tuples to become PLACEHOLDER */

	/* 将要变为 PLACEHOLDER 状态的元组数量 */
	uint16		nMove;			/* number of tuples to move */

	/* 需要移动的元组数量 */
	uint16		nChain;			/* number of tuples to re-chain */

	/* 需要重新链接的元组数量 */

	spgxlogState stateSrc;

	/*----------
	 * data follows:
	 *		tuple numbers to become DEAD
	 *		tuple numbers to become PLACEHOLDER
	 *		tuple numbers to move from (and replace with PLACEHOLDER)
	 *		tuple numbers to move to (replacing what is there)
	 *		tuple numbers to update nextOffset links of
	 *		tuple numbers to insert in nextOffset links
	 *----------
	 */

	/*----------
	 * 随后跟着的数据：
	 *		将要变为 DEAD 状态的元组编号
	 *		将要变为 PLACEHOLDER 状态的元组编号
	 *		作为移动来源的元组编号（并用 PLACEHOLDER 替换）
	 *		作为移动目标的元组编号（替换原本所在处的内容）
	 *		需要更新其 nextOffset 链接的元组编号
	 *		需要插入到 nextOffset 链接中的元组编号
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumLeaf;

#define SizeOfSpgxlogVacuumLeaf offsetof(spgxlogVacuumLeaf, offsets)

typedef struct spgxlogVacuumRoot
{
	/* vacuum a root page when it is also a leaf */

	/* 当根页面同时也是叶子页面时对其进行 vacuum */
	uint16		nDelete;		/* number of tuples to delete */

	/* 需要删除的元组数量 */

	spgxlogState stateSrc;

	/* offsets of tuples to delete follow */

	/* 随后跟着需要删除的元组的偏移位置 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumRoot;

#define SizeOfSpgxlogVacuumRoot offsetof(spgxlogVacuumRoot, offsets)

typedef struct spgxlogVacuumRedirect
{
	uint16		nToPlaceholder; /* number of redirects to make placeholders */

	/* 需要转变为 placeholder 的重定向元组数量 */
	OffsetNumber firstPlaceholder;	/* first placeholder tuple to remove */

	/* 要移除的第一个 placeholder 元组 */
	TransactionId snapshotConflictHorizon;	/* newest XID of removed redirects */

	/* 被移除的重定向元组中最新的 XID */
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* 用于在备库上进行逻辑解码期间处理恢复冲突 */

	/* offsets of redirect tuples to make placeholders follow */

	/* 随后跟着需要转变为 placeholder 的重定向元组的偏移位置 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumRedirect;

#define SizeOfSpgxlogVacuumRedirect offsetof(spgxlogVacuumRedirect, offsets)

/*
 * spg_redo: WAL replay entry point for SP-GiST.  Dispatches on the record's
 * info byte to the specific redo routine that reapplies the logged operation
 * (add leaf, move leafs, add node, split tuple, pick split, or vacuum) to the
 * appropriate index pages during crash recovery or standby replay.
 */

/*
 * spg_redo：SP-GiST 的 WAL 重放入口点。它根据记录的 info 字节进行分派，
 * 调用具体的 redo 例程，在崩溃恢复或备库重放期间将已记录的操作
 *（添加叶子、移动叶子、添加节点、分裂元组、pick split 或 vacuum）
 * 重新应用到相应的索引页面上。
 */
extern void spg_redo(XLogReaderState *record);

/*
 * spg_desc: format a human-readable description of an SP-GiST WAL record into
 * the given buffer.  Used by tools such as pg_waldump to explain the contents
 * of each record type.
 */

/*
 * spg_desc：将一条 SP-GiST WAL 记录格式化为人类可读的描述，写入给定的
 * 缓冲区。被诸如 pg_waldump 之类的工具用来解释每种记录类型的内容。
 */
extern void spg_desc(StringInfo buf, XLogReaderState *record);

/*
 * spg_identify: given a WAL record info byte, return the constant string name
 * of the corresponding SP-GiST record type (or NULL if unrecognized).
 */

/*
 * spg_identify：给定一个 WAL 记录的 info 字节，返回对应 SP-GiST 记录类型
 * 的常量字符串名称（若无法识别则返回 NULL）。
 */
extern const char *spg_identify(uint8 info);

/*
 * spg_xlog_startup: resource-manager startup callback invoked before SP-GiST
 * WAL replay begins, giving the AM a chance to initialize any redo-time state.
 */

/*
 * spg_xlog_startup：资源管理器的启动回调，在 SP-GiST WAL 重放开始之前被
 * 调用，让该访问方法有机会初始化任何 redo 阶段所需的状态。
 */
extern void spg_xlog_startup(void);

/*
 * spg_xlog_cleanup: resource-manager cleanup callback invoked after SP-GiST
 * WAL replay finishes, releasing any state allocated during redo.
 */

/*
 * spg_xlog_cleanup：资源管理器的清理回调，在 SP-GiST WAL 重放结束之后被
 * 调用，释放在 redo 期间分配的任何状态。
 */
extern void spg_xlog_cleanup(void);

/*
 * spg_mask: mask out non-deterministic parts of an SP-GiST page so that
 * pages produced on a primary and on a standby can be compared for
 * consistency checking (wal_consistency_checking).
 */

/*
 * spg_mask：屏蔽 SP-GiST 页面中具有非确定性的部分，以便对主库和备库上
 * 生成的页面进行比较，用于一致性检查（wal_consistency_checking）。
 */
extern void spg_mask(char *pagedata, BlockNumber blkno);

#endif							/* SPGXLOG_H */
