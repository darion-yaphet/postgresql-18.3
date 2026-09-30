/*-------------------------------------------------------------------------
 *
 * nbtxlog.h
 *	  header file for postgres btree xlog routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTXLOG_H
#define NBTXLOG_H

#include "access/transam.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/*
 * XLOG records for btree operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */

/*
 * 用于 btree 操作的 XLOG 记录
 *
 * XLOG 允许在日志记录 xl_info 字段的高 4 位中
 * 存储一些信息
 */
#define XLOG_BTREE_INSERT_LEAF	0x00	/* add index tuple without split */

/* XLOG_BTREE_INSERT_LEAF：不进行分裂地添加索引元组 */
#define XLOG_BTREE_INSERT_UPPER 0x10	/* same, on a non-leaf page */

/* XLOG_BTREE_INSERT_UPPER：同上，但作用于非叶子页 */
#define XLOG_BTREE_INSERT_META	0x20	/* same, plus update metapage */

/* XLOG_BTREE_INSERT_META：同上，并且更新元页 */
#define XLOG_BTREE_SPLIT_L		0x30	/* add index tuple with split */

/* XLOG_BTREE_SPLIT_L：添加索引元组并进行分裂 */
#define XLOG_BTREE_SPLIT_R		0x40	/* as above, new item on right */

/* XLOG_BTREE_SPLIT_R：同上，新项位于右侧页 */
#define XLOG_BTREE_INSERT_POST	0x50	/* add index tuple with posting split */

/* XLOG_BTREE_INSERT_POST：添加索引元组并进行倒排列表分裂 */
#define XLOG_BTREE_DEDUP		0x60	/* deduplicate tuples for a page */

/* XLOG_BTREE_DEDUP：对某个页面的元组进行去重 */
#define XLOG_BTREE_DELETE		0x70	/* delete leaf index tuples for a page */

/* XLOG_BTREE_DELETE：删除某个页面的叶子索引元组 */
#define XLOG_BTREE_UNLINK_PAGE	0x80	/* delete a half-dead page */

/* XLOG_BTREE_UNLINK_PAGE：删除一个半死（half-dead）页 */
#define XLOG_BTREE_UNLINK_PAGE_META 0x90	/* same, and update metapage */

/* XLOG_BTREE_UNLINK_PAGE_META：同上，并且更新元页 */
#define XLOG_BTREE_NEWROOT		0xA0	/* new root page */

/* XLOG_BTREE_NEWROOT：新的根页 */
#define XLOG_BTREE_MARK_PAGE_HALFDEAD 0xB0	/* mark a leaf as half-dead */

/* XLOG_BTREE_MARK_PAGE_HALFDEAD：将某个叶子页标记为半死 */
#define XLOG_BTREE_VACUUM		0xC0	/* delete entries on a page during
										 * vacuum */

/* XLOG_BTREE_VACUUM：在 vacuum 期间删除某个页面上的条目 */
#define XLOG_BTREE_REUSE_PAGE	0xD0	/* old page is about to be reused from
										 * FSM */

/* XLOG_BTREE_REUSE_PAGE：旧页即将从 FSM 中被重新使用 */
#define XLOG_BTREE_META_CLEANUP	0xE0	/* update cleanup-related data in the
										 * metapage */

/* XLOG_BTREE_META_CLEANUP：更新元页中与清理相关的数据 */

/*
 * All that we need to regenerate the meta-data page
 */

/*
 * 重新生成元数据页所需的全部内容
 */
typedef struct xl_btree_metadata
{
	uint32		version;
	BlockNumber root;
	uint32		level;
	BlockNumber fastroot;
	uint32		fastlevel;
	uint32		last_cleanup_num_delpages;
	bool		allequalimage;
} xl_btree_metadata;

/*
 * This is what we need to know about simple (without split) insert.
 *
 * This data record is used for INSERT_LEAF, INSERT_UPPER, INSERT_META, and
 * INSERT_POST.  Note that INSERT_META and INSERT_UPPER implies it's not a
 * leaf page, while INSERT_POST and INSERT_LEAF imply that it must be a leaf
 * page.
 *
 * Backup Blk 0: original page
 * Backup Blk 1: child's left sibling, if INSERT_UPPER or INSERT_META
 * Backup Blk 2: xl_btree_metadata, if INSERT_META
 *
 * Note: The new tuple is actually the "original" new item in the posting
 * list split insert case (i.e. the INSERT_POST case).  A split offset for
 * the posting list is logged before the original new item.  Recovery needs
 * both, since it must do an in-place update of the existing posting list
 * that was split as an extra step.  Also, recovery generates a "final"
 * newitem.  See _bt_swap_posting() for details on posting list splits.
 */

/*
 * 这是我们需要了解的关于简单（不进行分裂）插入的信息。
 *
 * 该数据记录用于 INSERT_LEAF、INSERT_UPPER、INSERT_META 和
 * INSERT_POST。注意 INSERT_META 和 INSERT_UPPER 意味着它不是
 * 叶子页，而 INSERT_POST 和 INSERT_LEAF 则意味着它一定是叶子
 * 页。
 *
 * 备份块 0：原始页
 * 备份块 1：子节点的左兄弟页，若为 INSERT_UPPER 或 INSERT_META
 * 备份块 2：xl_btree_metadata，若为 INSERT_META
 *
 * 注意：在倒排列表分裂插入的情况下（即 INSERT_POST 情况），新元组
 * 实际上是“原始”新项。倒排列表的分裂偏移量会在原始新项之前被记录。
 * 恢复过程需要两者，因为它必须作为额外步骤对被分裂的现有倒排列表
 * 进行就地更新。此外，恢复过程会生成一个“最终”的 newitem。关于倒排
 * 列表分裂的细节，请参见 _bt_swap_posting()。
 */
typedef struct xl_btree_insert
{
	OffsetNumber offnum;

	/* POSTING SPLIT OFFSET FOLLOWS (INSERT_POST case) */

	/* 倒排列表分裂偏移量紧随其后（INSERT_POST 情况） */
	/* NEW TUPLE ALWAYS FOLLOWS AT THE END */

	/* 新元组总是紧随在末尾 */
} xl_btree_insert;

#define SizeOfBtreeInsert	(offsetof(xl_btree_insert, offnum) + sizeof(OffsetNumber))

/*
 * On insert with split, we save all the items going into the right sibling
 * so that we can restore it completely from the log record.  This way takes
 * less xlog space than the normal approach, because if we did it standardly,
 * XLogInsert would almost always think the right page is new and store its
 * whole page image.  The left page, however, is handled in the normal
 * incremental-update fashion.
 *
 * Note: XLOG_BTREE_SPLIT_L and XLOG_BTREE_SPLIT_R share this data record.
 * There are two variants to indicate whether the inserted tuple went into the
 * left or right split page (and thus, whether the new item is stored or not).
 * We always log the left page high key because suffix truncation can generate
 * a new leaf high key using user-defined code.  This is also necessary on
 * internal pages, since the firstright item that the left page's high key was
 * based on will have been truncated to zero attributes in the right page (the
 * separator key is unavailable from the right page).
 *
 * Backup Blk 0: original page / new left page
 *
 * The left page's data portion contains the new item, if it's the _L variant.
 * _R variant split records generally do not have a newitem (_R variant leaf
 * page split records that must deal with a posting list split will include an
 * explicit newitem, though it is never used on the right page -- it is
 * actually an orignewitem needed to update existing posting list).  The new
 * high key of the left/original page appears last of all (and must always be
 * present).
 *
 * Page split records that need the REDO routine to deal with a posting list
 * split directly will have an explicit newitem, which is actually an
 * orignewitem (the newitem as it was before the posting list split, not
 * after).  A posting list split always has a newitem that comes immediately
 * after the posting list being split (which would have overlapped with
 * orignewitem prior to split).  Usually REDO must deal with posting list
 * splits with an _L variant page split record, and usually both the new
 * posting list and the final newitem go on the left page (the existing
 * posting list will be inserted instead of the old, and the final newitem
 * will be inserted next to that).  However, _R variant split records will
 * include an orignewitem when the split point for the page happens to have a
 * lastleft tuple that is also the posting list being split (leaving newitem
 * as the page split's firstright tuple).  The existence of this corner case
 * does not change the basic fact about newitem/orignewitem for the REDO
 * routine: it is always state used for the left page alone.  (This is why the
 * record's postingoff field isn't a reliable indicator of whether or not a
 * posting list split occurred during the page split; a non-zero value merely
 * indicates that the REDO routine must reconstruct a new posting list tuple
 * that is needed for the left page.)
 *
 * This posting list split handling is equivalent to the xl_btree_insert REDO
 * routine's INSERT_POST handling.  While the details are more complicated
 * here, the concept and goals are exactly the same.  See _bt_swap_posting()
 * for details on posting list splits.
 *
 * Backup Blk 1: new right page
 *
 * The right page's data portion contains the right page's tuples in the form
 * used by _bt_restore_page.  This includes the new item, if it's the _R
 * variant.  The right page's tuples also include the right page's high key
 * with either variant (moved from the left/original page during the split),
 * unless the split happened to be of the rightmost page on its level, where
 * there is no high key for new right page.
 *
 * Backup Blk 2: next block (orig page's rightlink), if any
 * Backup Blk 3: child's left sibling, if non-leaf split
 */

/*
 * 在带分裂的插入中，我们保存所有进入右兄弟页的项，以便能够仅从日志
 * 记录中就完整地恢复它。这种方式比标准做法占用更少的 xlog 空间，因为
 * 如果按标准方式处理，XLogInsert 几乎总是会认为右页是新页并存储其
 * 整页镜像。而左页仍以常规的增量更新方式处理。
 *
 * 注意：XLOG_BTREE_SPLIT_L 和 XLOG_BTREE_SPLIT_R 共用此数据记录。
 * 存在两种变体，用于指示被插入的元组是进入左侧还是右侧分裂页（从而
 * 决定是否存储新项）。我们总是记录左页的高键（high key），因为后缀
 * 截断可能会使用用户定义的代码生成新的叶子高键。在内部页上这同样是
 * 必要的，因为左页高键所基于的 firstright 项在右页中已被截断为零个
 * 属性（分隔键在右页中不可用）。
 *
 * 备份块 0：原始页 / 新的左页
 *
 * 若为 _L 变体，则左页的数据部分包含新项。_R 变体的分裂记录通常没有
 * newitem（但需要处理倒排列表分裂的 _R 变体叶子页分裂记录会包含一个
 * 显式的 newitem，尽管它从不在右页上使用——它实际上是用于更新现有
 * 倒排列表的 orignewitem）。左/原始页的新高键出现在最后（且必须始终
 * 存在）。
 *
 * 需要 REDO 例程直接处理倒排列表分裂的页分裂记录会带有一个显式的
 * newitem，它实际上是一个 orignewitem（即倒排列表分裂之前而非之后的
 * newitem）。倒排列表分裂总是有一个 newitem 紧跟在被分裂的倒排列表
 * 之后（在分裂之前它会与 orignewitem 重叠）。通常 REDO 必须以 _L 变体
 * 页分裂记录来处理倒排列表分裂，并且通常新倒排列表和最终 newitem 都
 * 进入左页（现有倒排列表将被插入以替换旧的，最终 newitem 将被插入到
 * 其旁边）。然而，当页面分裂点恰好使 lastleft 元组同时也是被分裂的
 * 倒排列表时，_R 变体分裂记录也会包含一个 orignewitem（此时 newitem
 * 成为该页分裂的 firstright 元组）。这个极端情况的存在并不改变关于
 * REDO 例程中 newitem/orignewitem 的基本事实：它始终是仅用于左页的
 * 状态。（这就是为什么记录的 postingoff 字段并不能可靠地指示页分裂
 * 期间是否发生了倒排列表分裂；非零值仅表示 REDO 例程必须为左页重建
 * 一个所需的新倒排列表元组。）
 *
 * 这种倒排列表分裂处理等价于 xl_btree_insert REDO 例程的 INSERT_POST
 * 处理。尽管这里的细节更复杂，但其概念与目标完全相同。关于倒排列表
 * 分裂的细节，请参见 _bt_swap_posting()。
 *
 * 备份块 1：新的右页
 *
 * 右页的数据部分以 _bt_restore_page 使用的形式包含右页的元组。若为 _R
 * 变体，则其中包含新项。无论哪种变体，右页的元组还包含右页的高键
 * （在分裂期间从左/原始页移动而来），除非该分裂恰好发生在其层级最右
 * 的页上，此时新的右页没有高键。
 *
 * 备份块 2：下一个块（原始页的 rightlink），如果有的话
 * 备份块 3：子节点的左兄弟页，若为非叶子分裂
 */
typedef struct xl_btree_split
{
	uint32		level;			/* tree level of page being split */

	/* level：被分裂页所处的树层级 */
	OffsetNumber firstrightoff; /* first origpage item on rightpage */

	/* firstrightoff：进入右页的第一个原始页项 */
	OffsetNumber newitemoff;	/* new item's offset */

	/* newitemoff：新项的偏移量 */
	uint16		postingoff;		/* offset inside orig posting tuple */

	/* postingoff：原始倒排列表元组内部的偏移量 */
} xl_btree_split;

#define SizeOfBtreeSplit	(offsetof(xl_btree_split, postingoff) + sizeof(uint16))

/*
 * When page is deduplicated, consecutive groups of tuples with equal keys are
 * merged together into posting list tuples.
 *
 * The WAL record represents a deduplication pass for a leaf page.  An array
 * of BTDedupInterval structs follows.
 */

/*
 * 当页面被去重时，具有相等键的连续元组组会被合并为倒排列表元组。
 *
 * 该 WAL 记录表示对某个叶子页的一次去重遍历。其后紧跟一个
 * BTDedupInterval 结构体数组。
 */
typedef struct xl_btree_dedup
{
	uint16		nintervals;

	/* DEDUPLICATION INTERVALS FOLLOW */

	/* 去重区间紧随其后 */
} xl_btree_dedup;

#define SizeOfBtreeDedup 	(offsetof(xl_btree_dedup, nintervals) + sizeof(uint16))

/*
 * This is what we need to know about page reuse within btree.  This record
 * only exists to generate a conflict point for Hot Standby.
 *
 * Note that we must include a RelFileLocator in the record because we don't
 * actually register the buffer with the record.
 */

/*
 * 这是我们需要了解的关于 btree 内部页面重用的信息。该记录仅用于为
 * 热备（Hot Standby）生成一个冲突点。
 *
 * 注意，我们必须在记录中包含一个 RelFileLocator，因为我们实际上并没有
 * 将缓冲区注册到该记录中。
 */
typedef struct xl_btree_reuse_page
{
	RelFileLocator locator;
	BlockNumber block;
	FullTransactionId snapshotConflictHorizon;
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* isCatalogRel：用于处理备库上逻辑解码期间的恢复冲突 */
} xl_btree_reuse_page;

#define SizeOfBtreeReusePage	(offsetof(xl_btree_reuse_page, isCatalogRel) + sizeof(bool))

/*
 * xl_btree_vacuum and xl_btree_delete records describe deletion of index
 * tuples on a leaf page.  The former variant is used by VACUUM, while the
 * latter variant is used by the ad-hoc deletions that sometimes take place
 * when btinsert() is called.
 *
 * The records are very similar.  The only difference is that xl_btree_delete
 * have snapshotConflictHorizon/isCatalogRel fields for recovery conflicts.
 * (VACUUM operations can just rely on earlier conflicts generated during
 * pruning of the table whose TIDs the to-be-deleted index tuples point to.
 * There are also small differences between each REDO routine that we don't go
 * into here.)
 *
 * xl_btree_vacuum and xl_btree_delete both represent deletion of any number
 * of index tuples on a single leaf page using page offset numbers.  Both also
 * support "updates" of index tuples, which is how deletes of a subset of TIDs
 * contained in an existing posting list tuple are implemented.
 *
 * Updated posting list tuples are represented using xl_btree_update metadata.
 * The REDO routines each use the xl_btree_update entries (plus each
 * corresponding original index tuple from the target leaf page) to generate
 * the final updated tuple.
 *
 * Updates are only used when there will be some remaining TIDs left by the
 * REDO routine.  Otherwise the posting list tuple just gets deleted outright.
 */

/*
 * xl_btree_vacuum 和 xl_btree_delete 记录描述了对某个叶子页上索引元组的
 * 删除。前一种变体由 VACUUM 使用，而后一种变体则用于调用 btinsert()
 * 时有时会发生的临时（ad-hoc）删除。
 *
 * 这两种记录非常相似。唯一的区别在于 xl_btree_delete 拥有用于恢复冲突
 * 的 snapshotConflictHorizon/isCatalogRel 字段。（VACUUM 操作可以直接
 * 依赖于对表进行剪枝时更早生成的冲突，而待删除的索引元组的 TID 正指向
 * 该表。每个 REDO 例程之间也存在一些细微差别，此处不再赘述。）
 *
 * xl_btree_vacuum 和 xl_btree_delete 都表示使用页偏移量对单个叶子页上
 * 任意数量索引元组的删除。两者也都支持索引元组的“更新”，这正是删除
 * 现有倒排列表元组中所包含 TID 子集的实现方式。
 *
 * 被更新的倒排列表元组使用 xl_btree_update 元数据来表示。各个 REDO
 * 例程都会使用 xl_btree_update 条目（外加来自目标叶子页的每个相应原始
 * 索引元组）来生成最终更新后的元组。
 *
 * 仅当 REDO 例程执行后仍会留下一些 TID 时才使用更新。否则，倒排列表
 * 元组会被直接彻底删除。
 */
typedef struct xl_btree_vacuum
{
	uint16		ndeleted;
	uint16		nupdated;

	/*----
	 * In payload of blk 0 :
	 * - DELETED TARGET OFFSET NUMBERS
	 * - UPDATED TARGET OFFSET NUMBERS
	 * - UPDATED TUPLES METADATA (xl_btree_update) ITEMS
	 *----
	 */

	/*----
	 * 在块 0 的负载中：
	 * - 被删除的目标偏移量
	 * - 被更新的目标偏移量
	 * - 被更新元组的元数据（xl_btree_update）项
	 *----
	 */
} xl_btree_vacuum;

#define SizeOfBtreeVacuum	(offsetof(xl_btree_vacuum, nupdated) + sizeof(uint16))

typedef struct xl_btree_delete
{
	TransactionId snapshotConflictHorizon;
	uint16		ndeleted;
	uint16		nupdated;
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* isCatalogRel：用于处理备库上逻辑解码期间的恢复冲突 */

	/*----
	 * In payload of blk 0 :
	 * - DELETED TARGET OFFSET NUMBERS
	 * - UPDATED TARGET OFFSET NUMBERS
	 * - UPDATED TUPLES METADATA (xl_btree_update) ITEMS
	 *----
	 */

	/*----
	 * 在块 0 的负载中：
	 * - 被删除的目标偏移量
	 * - 被更新的目标偏移量
	 * - 被更新元组的元数据（xl_btree_update）项
	 *----
	 */
} xl_btree_delete;

#define SizeOfBtreeDelete	(offsetof(xl_btree_delete, isCatalogRel) + sizeof(bool))

/*
 * The offsets that appear in xl_btree_update metadata are offsets into the
 * original posting list from tuple, not page offset numbers.  These are
 * 0-based.  The page offset number for the original posting list tuple comes
 * from the main xl_btree_vacuum/xl_btree_delete record.
 */

/*
 * 出现在 xl_btree_update 元数据中的偏移量是相对于元组中原始倒排列表的
 * 偏移量，而非页偏移量。它们从 0 开始计数。原始倒排列表元组的页偏移量
 * 来自主 xl_btree_vacuum/xl_btree_delete 记录。
 */
typedef struct xl_btree_update
{
	uint16		ndeletedtids;

	/* POSTING LIST uint16 OFFSETS TO A DELETED TID FOLLOW */

	/* 指向被删除 TID 的倒排列表 uint16 偏移量紧随其后 */
} xl_btree_update;

#define SizeOfBtreeUpdate	(offsetof(xl_btree_update, ndeletedtids) + sizeof(uint16))

/*
 * This is what we need to know about marking an empty subtree for deletion.
 * The target identifies the tuple removed from the parent page (note that we
 * remove this tuple's downlink and the *following* tuple's key).  Note that
 * the leaf page is empty, so we don't need to store its content --- it is
 * just reinitialized during recovery using the rest of the fields.
 *
 * Backup Blk 0: leaf block
 * Backup Blk 1: top parent
 */

/*
 * 这是我们需要了解的关于将空子树标记为待删除的信息。target 标识了从
 * 父页中移除的元组（注意我们会移除该元组的 downlink 以及*紧随其后*那个
 * 元组的键）。注意叶子页是空的，因此我们不需要存储其内容——恢复期间
 * 只需使用其余字段对其进行重新初始化即可。
 *
 * 备份块 0：叶子块
 * 备份块 1：顶层父节点
 */
typedef struct xl_btree_mark_page_halfdead
{
	OffsetNumber poffset;		/* deleted tuple id in parent page */

	/* poffset：父页中被删除元组的 id */

	/* information needed to recreate the leaf page: */

	/* 重建叶子页所需的信息： */
	BlockNumber leafblk;		/* leaf block ultimately being deleted */

	/* leafblk：最终被删除的叶子块 */
	BlockNumber leftblk;		/* leaf block's left sibling, if any */

	/* leftblk：叶子块的左兄弟块，如果有的话 */
	BlockNumber rightblk;		/* leaf block's right sibling */

	/* rightblk：叶子块的右兄弟块 */
	BlockNumber topparent;		/* topmost internal page in the subtree */

	/* topparent：子树中最顶层的内部页 */
} xl_btree_mark_page_halfdead;

#define SizeOfBtreeMarkPageHalfDead (offsetof(xl_btree_mark_page_halfdead, topparent) + sizeof(BlockNumber))

/*
 * This is what we need to know about deletion of a btree page.  Note that we
 * only leave behind a small amount of bookkeeping information in deleted
 * pages (deleted pages must be kept around as tombstones for a while).  It is
 * convenient for the REDO routine to regenerate its target page from scratch.
 * This is why WAL record describes certain details that are actually directly
 * available from the target page.
 *
 * Backup Blk 0: target block being deleted
 * Backup Blk 1: target block's left sibling, if any
 * Backup Blk 2: target block's right sibling
 * Backup Blk 3: leaf block (if different from target)
 * Backup Blk 4: metapage (if rightsib becomes new fast root)
 */

/*
 * 这是我们需要了解的关于删除某个 btree 页的信息。注意我们只在被删除的
 * 页中留下少量记账信息（被删除的页必须作为墓碑（tombstone）保留一段
 * 时间）。让 REDO 例程从头重新生成其目标页会比较方便。这就是为什么
 * WAL 记录会描述某些实际上可以直接从目标页获取的细节。
 *
 * 备份块 0：正被删除的目标块
 * 备份块 1：目标块的左兄弟块，如果有的话
 * 备份块 2：目标块的右兄弟块
 * 备份块 3：叶子块（如果与目标块不同）
 * 备份块 4：元页（如果右兄弟成为新的快速根）
 */
typedef struct xl_btree_unlink_page
{
	BlockNumber leftsib;		/* target block's left sibling, if any */

	/* leftsib：目标块的左兄弟块，如果有的话 */
	BlockNumber rightsib;		/* target block's right sibling */

	/* rightsib：目标块的右兄弟块 */
	uint32		level;			/* target block's level */

	/* level：目标块的层级 */
	FullTransactionId safexid;	/* target block's BTPageSetDeleted() XID */

	/* safexid：目标块的 BTPageSetDeleted() XID */

	/*
	 * Information needed to recreate a half-dead leaf page with correct
	 * topparent link.  The fields are only used when deletion operation's
	 * target page is an internal page.  REDO routine creates half-dead page
	 * from scratch to keep things simple (this is the same convenient
	 * approach used for the target page itself).
	 */

	/*
	 * 用于以正确的 topparent 链接重建半死叶子页所需的信息。这些字段仅在
	 * 删除操作的目标页是内部页时使用。为了简化处理，REDO 例程会从头创建
	 * 半死页（这与目标页本身所采用的便捷方法相同）。
	 */
	BlockNumber leafleftsib;
	BlockNumber leafrightsib;
	BlockNumber leaftopparent;	/* next child down in the subtree */

	/* leaftopparent：子树中向下的下一个子节点 */

	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_UNLINK_PAGE_META */

	/* 若为 XLOG_BTREE_UNLINK_PAGE_META，则 xl_btree_metadata 紧随其后 */
} xl_btree_unlink_page;

#define SizeOfBtreeUnlinkPage	(offsetof(xl_btree_unlink_page, leaftopparent) + sizeof(BlockNumber))

/*
 * New root log record.  There are zero tuples if this is to establish an
 * empty root, or two if it is the result of splitting an old root.
 *
 * Note that although this implies rewriting the metadata page, we don't need
 * an xl_btree_metadata record --- the rootblk and level are sufficient.
 *
 * Backup Blk 0: new root page (2 tuples as payload, if splitting old root)
 * Backup Blk 1: left child (if splitting an old root)
 * Backup Blk 2: metapage
 */

/*
 * 新根日志记录。如果这是用于建立一个空根，则元组数为零；如果它是分裂
 * 旧根的结果，则为两个元组。
 *
 * 注意，尽管这意味着要重写元数据页，但我们并不需要 xl_btree_metadata
 * 记录——rootblk 和 level 就已足够。
 *
 * 备份块 0：新的根页（如果是分裂旧根，则包含 2 个元组作为负载）
 * 备份块 1：左子节点（如果是分裂旧根）
 * 备份块 2：元页
 */
typedef struct xl_btree_newroot
{
	BlockNumber rootblk;		/* location of new root (redundant with blk 0) */

	/* rootblk：新根的位置（与块 0 冗余） */
	uint32		level;			/* its tree level */

	/* level：其树层级 */
} xl_btree_newroot;

#define SizeOfBtreeNewroot	(offsetof(xl_btree_newroot, level) + sizeof(uint32))


/*
 * prototypes for functions in nbtxlog.c
 */

/*
 * nbtxlog.c 中函数的原型声明
 */

/*
 * btree_redo: main WAL replay dispatcher for btree operations.  Reads the
 * info bits from the given xlog record and routes to the appropriate REDO
 * handler (insert, split, dedup, delete, vacuum, page unlink, new root, etc.)
 * to reapply the logged change to the relevant pages during recovery.
 *
 * btree_redo：btree 操作的主 WAL 重放分发器。它从给定的 xlog 记录中读取
 * info 位，并将其路由到相应的 REDO 处理程序（插入、分裂、去重、删除、
 * vacuum、页面解除链接、新根等），以便在恢复期间将所记录的更改重新应用
 * 到相关页面上。
 */
extern void btree_redo(XLogReaderState *record);

/*
 * btree_xlog_startup: called at the start of WAL replay for btree.  Sets up
 * any transient state (such as the temporary work area) that the REDO
 * routines need while replaying btree records.
 *
 * btree_xlog_startup：在 btree 的 WAL 重放开始时调用。它会建立 REDO
 * 例程在重放 btree 记录期间所需的任何临时状态（例如临时工作区）。
 */
extern void btree_xlog_startup(void);

/*
 * btree_xlog_cleanup: called at the end of WAL replay for btree.  Releases
 * the transient state allocated by btree_xlog_startup.
 *
 * btree_xlog_cleanup：在 btree 的 WAL 重放结束时调用。它会释放由
 * btree_xlog_startup 分配的临时状态。
 */
extern void btree_xlog_cleanup(void);

/*
 * btree_mask: masks out non-deterministic parts of a btree page image so that
 * WAL consistency checking can compare a replayed page against the original
 * without spurious mismatches (e.g. hint bits and unused space).
 *
 * btree_mask：屏蔽 btree 页镜像中不确定的部分，以便 WAL 一致性检查能够
 * 将重放后的页与原始页进行比较而不产生虚假的不匹配（例如提示位和未使用
 * 的空间）。
 */
extern void btree_mask(char *pagedata, BlockNumber blkno);

/*
 * prototypes for functions in nbtdesc.c
 */

/*
 * nbtdesc.c 中函数的原型声明
 */

/*
 * btree_desc: formats a human-readable description of a btree WAL record into
 * the given StringInfo buffer.  Used by tools such as pg_waldump to render the
 * record-specific fields for each btree xlog record type.
 *
 * btree_desc：将某条 btree WAL 记录的可读描述格式化输出到给定的
 * StringInfo 缓冲区中。诸如 pg_waldump 之类的工具使用它来呈现每种 btree
 * xlog 记录类型特有的字段。
 */
extern void btree_desc(StringInfo buf, XLogReaderState *record);

/*
 * btree_identify: returns the human-readable name of a btree WAL record type
 * corresponding to the given info bits.  Used together with btree_desc by WAL
 * inspection tools.
 *
 * btree_identify：根据给定的 info 位返回相应 btree WAL 记录类型的可读
 * 名称。WAL 检查工具会将它与 btree_desc 配合使用。
 */
extern const char *btree_identify(uint8 info);

#endif							/* NBTXLOG_H */
