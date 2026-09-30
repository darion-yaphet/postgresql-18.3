/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  header file for postgres btree access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/tableam.h"
#include "access/xlogreader.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/shm_toc.h"
#include "utils/skipsupport.h"

/* There's room for a 16-bit vacuum cycle ID in BTPageOpaqueData */

/* BTPageOpaqueData 中有容纳一个 16 位 vacuum 周期 ID 的空间 */
typedef uint16 BTCycleId;

/*
 *	BTPageOpaqueData -- At the end of every page, we store a pointer
 *	to both siblings in the tree.  This is used to do forward/backward
 *	index scans.  The next-page link is also critical for recovery when
 *	a search has navigated to the wrong page due to concurrent page splits
 *	or deletions; see src/backend/access/nbtree/README for more info.
 *
 *	In addition, we store the page's btree level (counting upwards from
 *	zero at a leaf page) as well as some flag bits indicating the page type
 *	and status.  If the page is deleted, a BTDeletedPageData struct is stored
 *	in the page's tuple area, while a standard BTPageOpaqueData struct is
 *	stored in the page special area.
 *
 *	We also store a "vacuum cycle ID".  When a page is split while VACUUM is
 *	processing the index, a nonzero value associated with the VACUUM run is
 *	stored into both halves of the split page.  (If VACUUM is not running,
 *	both pages receive zero cycleids.)	This allows VACUUM to detect whether
 *	a page was split since it started, with a small probability of false match
 *	if the page was last split some exact multiple of MAX_BT_CYCLE_ID VACUUMs
 *	ago.  Also, during a split, the BTP_SPLIT_END flag is cleared in the left
 *	(original) page, and set in the right page, but only if the next page
 *	to its right has a different cycleid.
 *
 *	NOTE: the BTP_LEAF flag bit is redundant since level==0 could be tested
 *	instead.
 *
 *	NOTE: the btpo_level field used to be a union type in order to allow
 *	deleted pages to store a 32-bit safexid in the same field.  We now store
 *	64-bit/full safexid values using BTDeletedPageData instead.
 */

/*
 *	BTPageOpaqueData —— 在每一页的末尾，我们存储指向树中两个兄弟页的
 *	指针。它用于执行向前/向后的索引扫描。当搜索由于并发的页面分裂或删除
 *	而导航到错误的页时，指向下一页的链接对恢复也至关重要；更多信息请
 *	参见 src/backend/access/nbtree/README。
 *
 *	此外，我们还存储该页的 btree 层级（从叶子页的零开始向上计数）以及
 *	一些指示页面类型和状态的标志位。如果该页被删除，则在该页的元组区域
 *	中存储一个 BTDeletedPageData 结构体，而在页的特殊区域中存储一个标准
 *	的 BTPageOpaqueData 结构体。
 *
 *	我们还存储一个“vacuum 周期 ID”。当 VACUUM 正在处理索引时某个页发生
 *	分裂，则会将一个与该 VACUUM 运行相关联的非零值存入分裂页的两个部分中。
 *	（如果 VACUUM 未运行，则两个页都得到零 cycleid。）这使 VACUUM 能够
 *	检测某个页是否在其开始之后发生过分裂，只有当该页上次分裂恰好发生在
 *	MAX_BT_CYCLE_ID 的整数倍次 VACUUM 之前时，才会有很小的误匹配概率。
 *	此外，在分裂期间，BTP_SPLIT_END 标志会在左（原始）页中被清除，并在
 *	右页中被设置，但仅当其右侧的下一页具有不同的 cycleid 时才如此。
 *
 *	注意：BTP_LEAF 标志位是冗余的，因为可以改用 level==0 来测试。
 *
 *	注意：btpo_level 字段过去是一个联合类型，以便被删除的页能在同一字段
 *	中存储一个 32 位的 safexid。现在我们改用 BTDeletedPageData 来存储
 *	64 位/完整的 safexid 值。
 */

typedef struct BTPageOpaqueData
{
	BlockNumber btpo_prev;		/* left sibling, or P_NONE if leftmost */

	/* btpo_prev：左兄弟页，若为最左页则为 P_NONE */
	BlockNumber btpo_next;		/* right sibling, or P_NONE if rightmost */

	/* btpo_next：右兄弟页，若为最右页则为 P_NONE */
	uint32		btpo_level;		/* tree level --- zero for leaf pages */

	/* btpo_level：树层级——叶子页为零 */
	uint16		btpo_flags;		/* flag bits, see below */

	/* btpo_flags：标志位，见下文 */
	BTCycleId	btpo_cycleid;	/* vacuum cycle ID of latest split */

	/* btpo_cycleid：最近一次分裂的 vacuum 周期 ID */
} BTPageOpaqueData;

typedef BTPageOpaqueData *BTPageOpaque;

#define BTPageGetOpaque(page) ((BTPageOpaque) PageGetSpecialPointer(page))

/* Bits defined in btpo_flags */

/* btpo_flags 中定义的位 */
#define BTP_LEAF		(1 << 0)	/* leaf page, i.e. not internal page */

/* BTP_LEAF：叶子页，即非内部页 */
#define BTP_ROOT		(1 << 1)	/* root page (has no parent) */

/* BTP_ROOT：根页（没有父节点） */
#define BTP_DELETED		(1 << 2)	/* page has been deleted from tree */

/* BTP_DELETED：该页已从树中删除 */
#define BTP_META		(1 << 3)	/* meta-page */

/* BTP_META：元页 */
#define BTP_HALF_DEAD	(1 << 4)	/* empty, but still in tree */

/* BTP_HALF_DEAD：为空，但仍在树中 */
#define BTP_SPLIT_END	(1 << 5)	/* rightmost page of split group */

/* BTP_SPLIT_END：分裂组中最右的页 */
#define BTP_HAS_GARBAGE (1 << 6)	/* page has LP_DEAD tuples (deprecated) */

/* BTP_HAS_GARBAGE：该页包含 LP_DEAD 元组（已弃用） */
#define BTP_INCOMPLETE_SPLIT (1 << 7)	/* right sibling's downlink is missing */

/* BTP_INCOMPLETE_SPLIT：右兄弟页的 downlink 缺失 */
#define BTP_HAS_FULLXID	(1 << 8)	/* contains BTDeletedPageData */

/* BTP_HAS_FULLXID：包含 BTDeletedPageData */

/*
 * The max allowed value of a cycle ID is a bit less than 64K.  This is
 * for convenience of pg_filedump and similar utilities: we want to use
 * the last 2 bytes of special space as an index type indicator, and
 * restricting cycle ID lets btree use that space for vacuum cycle IDs
 * while still allowing index type to be identified.
 */

/*
 * 周期 ID 允许的最大值略小于 64K。这是为了方便 pg_filedump 及类似的
 * 工具：我们希望将特殊空间的最后 2 个字节用作索引类型指示符，而限制
 * 周期 ID 可以让 btree 使用该空间存储 vacuum 周期 ID，同时仍允许识别
 * 索引类型。
 */
#define MAX_BT_CYCLE_ID		0xFF7F


/*
 * The Meta page is always the first page in the btree index.
 * Its primary purpose is to point to the location of the btree root page.
 * We also point to the "fast" root, which is the current effective root;
 * see README for discussion.
 */

/*
 * 元页始终是 btree 索引中的第一页。其主要目的是指向 btree 根页的位置。
 * 我们还指向“快速”根，即当前的有效根；相关讨论请参见 README。
 */

typedef struct BTMetaPageData
{
	uint32		btm_magic;		/* should contain BTREE_MAGIC */

	/* btm_magic：应包含 BTREE_MAGIC */
	uint32		btm_version;	/* nbtree version (always <= BTREE_VERSION) */

	/* btm_version：nbtree 版本（始终 <= BTREE_VERSION） */
	BlockNumber btm_root;		/* current root location */

	/* btm_root：当前根的位置 */
	uint32		btm_level;		/* tree level of the root page */

	/* btm_level：根页的树层级 */
	BlockNumber btm_fastroot;	/* current "fast" root location */

	/* btm_fastroot：当前“快速”根的位置 */
	uint32		btm_fastlevel;	/* tree level of the "fast" root page */

	/* btm_fastlevel：“快速”根页的树层级 */
	/* remaining fields only valid when btm_version >= BTREE_NOVAC_VERSION */

	/* 其余字段仅在 btm_version >= BTREE_NOVAC_VERSION 时有效 */

	/* number of deleted, non-recyclable pages during last cleanup */

	/* 上次清理期间被删除但不可回收的页数 */
	uint32		btm_last_cleanup_num_delpages;
	/* number of heap tuples during last cleanup (deprecated) */

	/* 上次清理期间的堆元组数（已弃用） */
	float8		btm_last_cleanup_num_heap_tuples;

	bool		btm_allequalimage;	/* are all columns "equalimage"? */

	/* btm_allequalimage：是否所有列都是“equalimage”？ */
} BTMetaPageData;

#define BTPageGetMeta(p) \
	((BTMetaPageData *) PageGetContents(p))

/*
 * The current Btree version is 4.  That's what you'll get when you create
 * a new index.
 *
 * Btree version 3 was used in PostgreSQL v11.  It is mostly the same as
 * version 4, but heap TIDs were not part of the keyspace.  Index tuples
 * with duplicate keys could be stored in any order.  We continue to
 * support reading and writing Btree versions 2 and 3, so that they don't
 * need to be immediately re-indexed at pg_upgrade.  In order to get the
 * new heapkeyspace semantics, however, a REINDEX is needed.
 *
 * Deduplication is safe to use when the btm_allequalimage field is set to
 * true.  It's safe to read the btm_allequalimage field on version 3, but
 * only version 4 indexes make use of deduplication.  Even version 4
 * indexes created on PostgreSQL v12 will need a REINDEX to make use of
 * deduplication, though, since there is no other way to set
 * btm_allequalimage to true (pg_upgrade hasn't been taught to set the
 * metapage field).
 *
 * Btree version 2 is mostly the same as version 3.  There are two new
 * fields in the metapage that were introduced in version 3.  A version 2
 * metapage will be automatically upgraded to version 3 on the first
 * insert to it.  INCLUDE indexes cannot use version 2.
 */

/*
 * 当前的 Btree 版本是 4。创建新索引时得到的就是该版本。
 *
 * Btree 版本 3 用于 PostgreSQL v11。它与版本 4 大体相同，但堆 TID 不是
 * 键空间的一部分。具有重复键的索引元组可以按任意顺序存储。我们继续
 * 支持读写 Btree 版本 2 和 3，以便它们在 pg_upgrade 时不需要立即重建
 * 索引。然而，要获得新的 heapkeyspace 语义，则需要执行 REINDEX。
 *
 * 当 btm_allequalimage 字段被设置为 true 时，去重可以安全使用。在版本 3
 * 上读取 btm_allequalimage 字段是安全的，但只有版本 4 的索引才会使用
 * 去重。不过，即便是在 PostgreSQL v12 上创建的版本 4 索引也需要执行
 * REINDEX 才能使用去重，因为没有其他方法将 btm_allequalimage 设置为
 * true（pg_upgrade 尚未被教会设置该元页字段）。
 *
 * Btree 版本 2 与版本 3 大体相同。元页中有两个在版本 3 引入的新字段。
 * 版本 2 的元页会在对其进行首次插入时自动升级为版本 3。INCLUDE 索引
 * 不能使用版本 2。
 */
#define BTREE_METAPAGE	0		/* first page is meta */

/* BTREE_METAPAGE：第一页是元页 */
#define BTREE_MAGIC		0x053162	/* magic number in metapage */

/* BTREE_MAGIC：元页中的魔数 */
#define BTREE_VERSION	4		/* current version number */

/* BTREE_VERSION：当前版本号 */
#define BTREE_MIN_VERSION	2	/* minimum supported version */

/* BTREE_MIN_VERSION：支持的最低版本 */
#define BTREE_NOVAC_VERSION	3	/* version with all meta fields set */

/* BTREE_NOVAC_VERSION：所有元字段均已设置的版本 */

/*
 * Maximum size of a btree index entry, including its tuple header.
 *
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 *
 * There are rare cases where _bt_truncate() will need to enlarge
 * a heap index tuple to make space for a tiebreaker heap TID
 * attribute, which we account for here.
 */

/*
 * btree 索引条目的最大大小，包括其元组头。
 *
 * 我们实际上需要能够在每一页上容纳三个项，因此将任何单个项限制为每页
 * 可用空间的 1/3。
 *
 * 在少数情况下，_bt_truncate() 需要扩大一个堆索引元组以便为作为决胜键
 * 的堆 TID 属性腾出空间，我们在此对此加以考虑。
 */
#define BTMaxItemSize \
	(MAXALIGN_DOWN((BLCKSZ - \
					MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
					MAXALIGN(sizeof(BTPageOpaqueData))) / 3) - \
					MAXALIGN(sizeof(ItemPointerData)))
#define BTMaxItemSizeNoHeapTid \
	MAXALIGN_DOWN((BLCKSZ - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(BTPageOpaqueData))) / 3)

/*
 * MaxTIDsPerBTreePage is an upper bound on the number of heap TIDs tuples
 * that may be stored on a btree leaf page.  It is used to size the
 * per-page temporary buffers.
 *
 * Note: we don't bother considering per-tuple overheads here to keep
 * things simple (value is based on how many elements a single array of
 * heap TIDs must have to fill the space between the page header and
 * special area).  The value is slightly higher (i.e. more conservative)
 * than necessary as a result, which is considered acceptable.
 */

/*
 * MaxTIDsPerBTreePage 是可存储在一个 btree 叶子页上的堆 TID 元组数量的
 * 上界。它用于确定每页临时缓冲区的大小。
 *
 * 注意：为保持简单，我们在此不去考虑每个元组的开销（该值基于单个堆 TID
 * 数组必须有多少个元素才能填满页头与特殊区域之间的空间）。因此该值比
 * 实际所需略高（即更保守），这被认为是可接受的。
 */
#define MaxTIDsPerBTreePage \
	(int) ((BLCKSZ - SizeOfPageHeaderData - sizeof(BTPageOpaqueData)) / \
		   sizeof(ItemPointerData))

/*
 * The leaf-page fillfactor defaults to 90% but is user-adjustable.
 * For pages above the leaf level, we use a fixed 70% fillfactor.
 * The fillfactor is applied during index build and when splitting
 * a rightmost page; when splitting non-rightmost pages we try to
 * divide the data equally.  When splitting a page that's entirely
 * filled with a single value (duplicates), the effective leaf-page
 * fillfactor is 96%, regardless of whether the page is a rightmost
 * page.
 */

/*
 * 叶子页的填充因子默认为 90%，但用户可调整。对于叶子层之上的页，我们
 * 使用固定的 70% 填充因子。填充因子在索引构建期间以及分裂最右页时应用；
 * 在分裂非最右页时，我们尝试将数据平均分配。当分裂一个完全由单一值
 * （重复值）填满的页时，无论该页是否为最右页，其有效叶子页填充因子
 * 均为 96%。
 */
#define BTREE_MIN_FILLFACTOR		10
#define BTREE_DEFAULT_FILLFACTOR	90
#define BTREE_NONLEAF_FILLFACTOR	70
#define BTREE_SINGLEVAL_FILLFACTOR	96

/*
 *	In general, the btree code tries to localize its knowledge about
 *	page layout to a couple of routines.  However, we need a special
 *	value to indicate "no page number" in those places where we expect
 *	page numbers.  We can use zero for this because we never need to
 *	make a pointer to the metadata page.
 */

/*
 *	一般来说，btree 代码会尝试将其关于页面布局的知识局限在少数几个例程
 *	中。然而，在那些我们期望出现页号的地方，我们需要一个特殊值来表示
 *	“无页号”。我们可以用零来表示这一点，因为我们从不需要构造一个指向
 *	元数据页的指针。
 */

#define P_NONE			0

/*
 * Macros to test whether a page is leftmost or rightmost on its tree level,
 * as well as other state info kept in the opaque data.
 */

/*
 * 用于测试某个页在其树层级上是否为最左或最右页，以及测试保存在 opaque
 * 数据中的其他状态信息的宏。
 */
#define P_LEFTMOST(opaque)		((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->btpo_next == P_NONE)
#define P_ISLEAF(opaque)		(((opaque)->btpo_flags & BTP_LEAF) != 0)
#define P_ISROOT(opaque)		(((opaque)->btpo_flags & BTP_ROOT) != 0)
#define P_ISDELETED(opaque)		(((opaque)->btpo_flags & BTP_DELETED) != 0)
#define P_ISMETA(opaque)		(((opaque)->btpo_flags & BTP_META) != 0)
#define P_ISHALFDEAD(opaque)	(((opaque)->btpo_flags & BTP_HALF_DEAD) != 0)
#define P_IGNORE(opaque)		(((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque)	(((opaque)->btpo_flags & BTP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque)	(((opaque)->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0)
#define P_HAS_FULLXID(opaque)	(((opaque)->btpo_flags & BTP_HAS_FULLXID) != 0)

/*
 * BTDeletedPageData is the page contents of a deleted page
 */

/*
 * BTDeletedPageData 是被删除页的页内容
 */
typedef struct BTDeletedPageData
{
	FullTransactionId safexid;	/* See BTPageIsRecyclable() */

	/* safexid：参见 BTPageIsRecyclable() */
} BTDeletedPageData;

/*
 * BTPageSetDeleted: mark the given page as deleted.  Clears the half-dead
 * flag, sets the deleted and full-xid flags, shrinks the page's data area to
 * hold a BTDeletedPageData struct, and records the supplied safexid there so
 * later code can decide when the page becomes recyclable.
 *
 * BTPageSetDeleted：将给定页标记为已删除。它清除 half-dead 标志，设置
 * deleted 与 full-xid 标志，将页的数据区收缩为可容纳一个 BTDeletedPageData
 * 结构体，并在其中记录传入的 safexid，以便后续代码能判断该页何时可回收。
 */
static inline void
BTPageSetDeleted(Page page, FullTransactionId safexid)
{
	BTPageOpaque opaque;
	PageHeader	header;
	BTDeletedPageData *contents;

	opaque = BTPageGetOpaque(page);
	header = ((PageHeader) page);

	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED | BTP_HAS_FULLXID;
	header->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
		sizeof(BTDeletedPageData);
	header->pd_upper = header->pd_special;

	/* Set safexid in deleted page */

	/* 在被删除的页中设置 safexid */
	contents = ((BTDeletedPageData *) PageGetContents(page));
	contents->safexid = safexid;
}

/*
 * BTPageGetDeleteXid: return the safexid recorded in a deleted page, which
 * tells when the page can be recycled.  Pages left over from pg_upgrade lack
 * a stored full xid, so this returns FirstNormalFullTransactionId for them to
 * signal that they are already safe to recycle.
 *
 * BTPageGetDeleteXid：返回记录在被删除页中的 safexid，它指示该页何时可被
 * 回收。由 pg_upgrade 遗留下来的页没有存储完整的 xid，因此对它们返回
 * FirstNormalFullTransactionId，以表示它们已经可以安全回收。
 */
static inline FullTransactionId
BTPageGetDeleteXid(Page page)
{
	BTPageOpaque opaque;
	BTDeletedPageData *contents;

	/* We only expect to be called with a deleted page */

	/* 我们只期望在被删除的页上被调用 */
	Assert(!PageIsNew(page));
	opaque = BTPageGetOpaque(page);
	Assert(P_ISDELETED(opaque));

	/* pg_upgrade'd deleted page -- must be safe to recycle now */

	/* 经 pg_upgrade 处理的被删除页——现在必定可以安全回收 */
	if (!P_HAS_FULLXID(opaque))
		return FirstNormalFullTransactionId;

	/* Get safexid from deleted page */

	/* 从被删除的页中获取 safexid */
	contents = ((BTDeletedPageData *) PageGetContents(page));
	return contents->safexid;
}

/*
 * Is an existing page recyclable?
 *
 * This exists to centralize the policy on which deleted pages are now safe to
 * re-use.  However, _bt_pendingfsm_finalize() duplicates some of the same
 * logic because it doesn't work directly with pages -- keep the two in sync.
 *
 * Note: PageIsNew() pages are always safe to recycle, but we can't deal with
 * them here (caller is responsible for that case themselves).  Caller might
 * well need special handling for new pages anyway.
 */

/*
 * 某个现有页是否可回收？
 *
 * 该函数用于集中管理关于哪些被删除的页现在可以安全重用的策略。然而，
 * _bt_pendingfsm_finalize() 复制了其中一些相同的逻辑，因为它并不直接
 * 处理页——请保持两者同步。
 *
 * 注意：PageIsNew() 的页总是可以安全回收，但我们无法在此处理它们
 * （调用者需自行负责该情况）。无论如何，调用者很可能都需要对新页进行
 * 特殊处理。
 */
static inline bool
BTPageIsRecyclable(Page page, Relation heaprel)
{
	BTPageOpaque opaque;

	Assert(!PageIsNew(page));
	Assert(heaprel != NULL);

	/* Recycling okay iff page is deleted and safexid is old enough */

	/* 当且仅当页已被删除且 safexid 足够旧时，回收才是允许的 */
	opaque = BTPageGetOpaque(page);
	if (P_ISDELETED(opaque))
	{
		FullTransactionId safexid = BTPageGetDeleteXid(page);

		/*
		 * The page was deleted, but when? If it was just deleted, a scan
		 * might have seen the downlink to it, and will read the page later.
		 * As long as that can happen, we must keep the deleted page around as
		 * a tombstone.
		 *
		 * For that check if the deletion XID could still be visible to
		 * anyone. If not, then no scan that's still in progress could have
		 * seen its downlink, and we can recycle it.
		 */

		/*
		 * 该页已被删除，但是何时删除的？如果它刚刚被删除，某个扫描可能
		 * 已经看到指向它的 downlink，并将在稍后读取该页。只要这种情况
		 * 有可能发生，我们就必须将被删除的页作为墓碑保留下来。
		 *
		 * 为此，检查删除 XID 是否仍可能对任何人可见。如果不可见，那么
		 * 任何仍在进行中的扫描都不可能看到它的 downlink，于是我们就可以
		 * 回收它。
		 */
		return GlobalVisCheckRemovableFullXid(heaprel, safexid);
	}

	return false;
}

/*
 * BTVacState and BTPendingFSM are private nbtree.c state used during VACUUM.
 * They are exported for use by page deletion related code in nbtpage.c.
 */

/*
 * BTVacState 和 BTPendingFSM 是 VACUUM 期间使用的 nbtree.c 私有状态。
 * 它们被导出以供 nbtpage.c 中与页面删除相关的代码使用。
 */
typedef struct BTPendingFSM
{
	BlockNumber target;			/* Page deleted by current VACUUM */

	/* target：被当前 VACUUM 删除的页 */
	FullTransactionId safexid;	/* Page's BTDeletedPageData.safexid */

	/* safexid：该页的 BTDeletedPageData.safexid */
} BTPendingFSM;

typedef struct BTVacState
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	BTCycleId	cycleid;
	MemoryContext pagedelcontext;

	/*
	 * _bt_pendingfsm_finalize() state
	 */

	/*
	 * _bt_pendingfsm_finalize() 的状态
	 */
	int			bufsize;		/* pendingpages space (in # elements) */

	/* bufsize：pendingpages 的空间（以元素个数计） */
	int			maxbufsize;		/* max bufsize that respects work_mem */

	/* maxbufsize：在遵守 work_mem 前提下的最大 bufsize */
	BTPendingFSM *pendingpages; /* One entry per newly deleted page */

	/* pendingpages：每个新删除的页对应一个条目 */
	int			npendingpages;	/* current # valid pendingpages */

	/* npendingpages：当前有效的 pendingpages 数量 */
} BTVacState;

/*
 *	Lehman and Yao's algorithm requires a ``high key'' on every non-rightmost
 *	page.  The high key is not a tuple that is used to visit the heap.  It is
 *	a pivot tuple (see "Notes on B-Tree tuple format" below for definition).
 *	The high key on a page is required to be greater than or equal to any
 *	other key that appears on the page.  If we find ourselves trying to
 *	insert a key that is strictly > high key, we know we need to move right
 *	(this should only happen if the page was split since we examined the
 *	parent page).
 *
 *	Our insertion algorithm guarantees that we can use the initial least key
 *	on our right sibling as the high key.  Once a page is created, its high
 *	key changes only if the page is split.
 *
 *	On a non-rightmost page, the high key lives in item 1 and data items
 *	start in item 2.  Rightmost pages have no high key, so we store data
 *	items beginning in item 1.
 */

/*
 *	Lehman 和 Yao 的算法要求每个非最右页都有一个“高键”（high key）。高键
 *	不是用于访问堆的元组，而是一个 pivot 元组（其定义参见下文“B-Tree 元组
 *	格式说明”）。要求页上的高键大于或等于该页上出现的任何其他键。如果我们
 *	发现自己试图插入一个严格大于高键的键，就知道需要向右移动（这只应在
 *	我们检查父页之后该页发生了分裂时才发生）。
 *
 *	我们的插入算法保证可以将右兄弟页上最初的最小键用作高键。一个页一旦
 *	被创建，其高键只有在该页发生分裂时才会改变。
 *
 *	在非最右页上，高键位于第 1 项，数据项从第 2 项开始。最右页没有高键，
 *	因此我们从第 1 项开始存储数据项。
 */

#define P_HIKEY				((OffsetNumber) 1)
#define P_FIRSTKEY			((OffsetNumber) 2)
#define P_FIRSTDATAKEY(opaque)	(P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)

/*
 * Notes on B-Tree tuple format, and key and non-key attributes:
 *
 * INCLUDE B-Tree indexes have non-key attributes.  These are extra
 * attributes that may be returned by index-only scans, but do not influence
 * the order of items in the index (formally, non-key attributes are not
 * considered to be part of the key space).  Non-key attributes are only
 * present in leaf index tuples whose item pointers actually point to heap
 * tuples (non-pivot tuples).  _bt_check_natts() enforces the rules
 * described here.
 *
 * Non-pivot tuple format (plain/non-posting variant):
 *
 *  t_tid | t_info | key values | INCLUDE columns, if any
 *
 * t_tid points to the heap TID, which is a tiebreaker key column as of
 * BTREE_VERSION 4.
 *
 * Non-pivot tuples complement pivot tuples, which only have key columns.
 * The sole purpose of pivot tuples is to represent how the key space is
 * separated.  In general, any B-Tree index that has more than one level
 * (i.e. any index that does not just consist of a metapage and a single
 * leaf root page) must have some number of pivot tuples, since pivot
 * tuples are used for traversing the tree.  Suffix truncation can omit
 * trailing key columns when a new pivot is formed, which makes minus
 * infinity their logical value.  Since BTREE_VERSION 4 indexes treat heap
 * TID as a trailing key column that ensures that all index tuples are
 * physically unique, it is necessary to represent heap TID as a trailing
 * key column in pivot tuples, though very often this can be truncated
 * away, just like any other key column. (Actually, the heap TID is
 * omitted rather than truncated, since its representation is different to
 * the non-pivot representation.)
 *
 * Pivot tuple format:
 *
 *  t_tid | t_info | key values | [heap TID]
 *
 * We store the number of columns present inside pivot tuples by abusing
 * their t_tid offset field, since pivot tuples never need to store a real
 * offset (pivot tuples generally store a downlink in t_tid, though).  The
 * offset field only stores the number of columns/attributes when the
 * INDEX_ALT_TID_MASK bit is set, which doesn't count the trailing heap
 * TID column sometimes stored in pivot tuples -- that's represented by
 * the presence of BT_PIVOT_HEAP_TID_ATTR.  The INDEX_ALT_TID_MASK bit in
 * t_info is always set on BTREE_VERSION 4 pivot tuples, since
 * BTreeTupleIsPivot() must work reliably on heapkeyspace versions.
 *
 * In version 2 or version 3 (!heapkeyspace) indexes, INDEX_ALT_TID_MASK
 * might not be set in pivot tuples.  BTreeTupleIsPivot() won't work
 * reliably as a result.  The number of columns stored is implicitly the
 * same as the number of columns in the index, just like any non-pivot
 * tuple. (The number of columns stored should not vary, since suffix
 * truncation of key columns is unsafe within any !heapkeyspace index.)
 *
 * The 12 least significant bits from t_tid's offset number are used to
 * represent the number of key columns within a pivot tuple.  This leaves 4
 * status bits (BT_STATUS_OFFSET_MASK bits), which are shared by all tuples
 * that have the INDEX_ALT_TID_MASK bit set (set in t_info) to store basic
 * tuple metadata.  BTreeTupleIsPivot() and BTreeTupleIsPosting() use the
 * BT_STATUS_OFFSET_MASK bits.
 *
 * Sometimes non-pivot tuples also use a representation that repurposes
 * t_tid to store metadata rather than a TID.  PostgreSQL v13 introduced a
 * new non-pivot tuple format to support deduplication: posting list
 * tuples.  Deduplication merges together multiple equal non-pivot tuples
 * into a logically equivalent, space efficient representation.  A posting
 * list is an array of ItemPointerData elements.  Non-pivot tuples are
 * merged together to form posting list tuples lazily, at the point where
 * we'd otherwise have to split a leaf page.
 *
 * Posting tuple format (alternative non-pivot tuple representation):
 *
 *  t_tid | t_info | key values | posting list (TID array)
 *
 * Posting list tuples are recognized as such by having the
 * INDEX_ALT_TID_MASK status bit set in t_info and the BT_IS_POSTING status
 * bit set in t_tid's offset number.  These flags redefine the content of
 * the posting tuple's t_tid to store the location of the posting list
 * (instead of a block number), as well as the total number of heap TIDs
 * present in the tuple (instead of a real offset number).
 *
 * The 12 least significant bits from t_tid's offset number are used to
 * represent the number of heap TIDs present in the tuple, leaving 4 status
 * bits (the BT_STATUS_OFFSET_MASK bits).  Like any non-pivot tuple, the
 * number of columns stored is always implicitly the total number in the
 * index (in practice there can never be non-key columns stored, since
 * deduplication is not supported with INCLUDE indexes).
 */

/*
 * B-Tree 元组格式以及键属性与非键属性的说明：
 *
 * INCLUDE B-Tree 索引带有非键属性。它们是一些额外的属性，可以由 index-only
 * 扫描返回，但不影响索引中项的顺序（形式上，非键属性不被视为键空间的
 * 一部分）。非键属性只存在于其项指针实际指向堆元组的叶子索引元组（即
 * 非 pivot 元组）中。_bt_check_natts() 强制执行此处描述的规则。
 *
 * 非 pivot 元组格式（普通/非倒排列表变体）：
 *
 *  t_tid | t_info | 键值 | INCLUDE 列（如有）
 *
 * t_tid 指向堆 TID，自 BTREE_VERSION 4 起它是一个作为决胜键的键列。
 *
 * 非 pivot 元组与 pivot 元组互为补充，后者只有键列。pivot 元组的唯一
 * 目的是表示键空间是如何被划分的。一般来说，任何具有多于一层的 B-Tree
 * 索引（即任何不仅仅由一个元页和单个叶子根页构成的索引）都必须有一定
 * 数量的 pivot 元组，因为 pivot 元组用于遍历树。当形成新的 pivot 时，
 * 后缀截断可以省略末尾的键列，这使得它们的逻辑值为负无穷。由于
 * BTREE_VERSION 4 索引将堆 TID 视为一个末尾键列以确保所有索引元组在
 * 物理上唯一，因此有必要在 pivot 元组中将堆 TID 表示为一个末尾键列，
 * 尽管很多时候它可以被截断掉，就像任何其他键列一样。（实际上，堆 TID
 * 是被省略而非被截断，因为其表示方式不同于非 pivot 表示。）
 *
 * pivot 元组格式：
 *
 *  t_tid | t_info | 键值 | [堆 TID]
 *
 * 我们通过滥用 pivot 元组的 t_tid 偏移字段来存储其内部所含的列数，因为
 * pivot 元组从不需要存储真实的偏移量（不过 pivot 元组通常在 t_tid 中
 * 存储一个 downlink）。仅当 INDEX_ALT_TID_MASK 位被设置时，该偏移字段
 * 才存储列/属性的数量，而这不计入有时存储在 pivot 元组中的末尾堆 TID
 * 列——那由 BT_PIVOT_HEAP_TID_ATTR 的存在来表示。在 BTREE_VERSION 4
 * 的 pivot 元组中，t_info 中的 INDEX_ALT_TID_MASK 位始终被设置，因为
 * BTreeTupleIsPivot() 必须在 heapkeyspace 版本上可靠地工作。
 *
 * 在版本 2 或版本 3（!heapkeyspace）索引中，pivot 元组里可能未设置
 * INDEX_ALT_TID_MASK。因此 BTreeTupleIsPivot() 将无法可靠工作。所存储
 * 的列数隐式地等于索引中的列数，就像任何非 pivot 元组一样。（所存储的
 * 列数不应变化，因为在任何 !heapkeyspace 索引中对键列进行后缀截断都是
 * 不安全的。）
 *
 * t_tid 偏移量的低 12 位用于表示 pivot 元组内的键列数。这样就剩下 4 个
 * 状态位（BT_STATUS_OFFSET_MASK 位），它们由所有设置了 INDEX_ALT_TID_MASK
 * 位（在 t_info 中设置）的元组共享，用于存储基本的元组元数据。
 * BTreeTupleIsPivot() 和 BTreeTupleIsPosting() 使用 BT_STATUS_OFFSET_MASK
 * 位。
 *
 * 有时非 pivot 元组也会使用一种重新利用 t_tid 来存储元数据而非 TID 的
 * 表示方式。PostgreSQL v13 引入了一种新的非 pivot 元组格式以支持去重：
 * 倒排列表元组。去重将多个相等的非 pivot 元组合并为一种逻辑上等价、
 * 空间高效的表示。倒排列表是一个 ItemPointerData 元素数组。非 pivot 元组
 * 会被惰性地合并为倒排列表元组，即在我们本来不得不分裂某个叶子页的
 * 时刻进行合并。
 *
 * 倒排列表元组格式（非 pivot 元组的另一种表示）：
 *
 *  t_tid | t_info | 键值 | 倒排列表（TID 数组）
 *
 * 倒排列表元组通过在 t_info 中设置 INDEX_ALT_TID_MASK 状态位以及在
 * t_tid 的偏移量中设置 BT_IS_POSTING 状态位来被识别为倒排列表元组。这些
 * 标志重新定义了倒排元组 t_tid 的内容，用于存储倒排列表的位置（而非块
 * 号），以及元组中所含堆 TID 的总数（而非真实的偏移量）。
 *
 * t_tid 偏移量的低 12 位用于表示元组中所含堆 TID 的数量，从而留下 4 个
 * 状态位（即 BT_STATUS_OFFSET_MASK 位）。与任何非 pivot 元组一样，所
 * 存储的列数总是隐式地等于索引中的总列数（实际上永远不会存储非键列，
 * 因为去重不支持 INCLUDE 索引）。
 */
#define INDEX_ALT_TID_MASK			INDEX_AM_RESERVED_BIT

/* Item pointer offset bit masks */

/* 项指针偏移量的位掩码 */
#define BT_OFFSET_MASK				0x0FFF
#define BT_STATUS_OFFSET_MASK		0xF000
/* BT_STATUS_OFFSET_MASK status bits */

/* BT_STATUS_OFFSET_MASK 状态位 */
#define BT_PIVOT_HEAP_TID_ATTR		0x1000
#define BT_IS_POSTING				0x2000

/*
 * Mask allocated for number of keys in index tuple must be able to fit
 * maximum possible number of index attributes
 */

/*
 * 为索引元组中键的数量所分配的掩码，必须能够容纳可能的最大索引属性数
 */
StaticAssertDecl(BT_OFFSET_MASK >= INDEX_MAX_KEYS,
				 "BT_OFFSET_MASK can't fit INDEX_MAX_KEYS");

/*
 * Note: BTreeTupleIsPivot() can have false negatives (but not false
 * positives) when used with !heapkeyspace indexes
 */

/*
 * 注意：当用于 !heapkeyspace 索引时，BTreeTupleIsPivot() 可能出现假阴性
 * （但不会出现假阳性）
 */

/*
 * BTreeTupleIsPivot: report whether the given index tuple uses the pivot
 * tuple representation.  It checks that the alternative-TID bit is set in
 * t_info and that the posting-list bit is absent from the t_tid offset, which
 * together identify a pivot tuple as opposed to a posting-list tuple.
 *
 * BTreeTupleIsPivot：报告给定的索引元组是否使用 pivot 元组表示。它检查
 * t_info 中是否设置了 alternative-TID 位，以及 t_tid 偏移量中是否不含
 * 倒排列表位，二者共同将 pivot 元组与倒排列表元组区分开来。
 */
static inline bool
BTreeTupleIsPivot(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* absence of BT_IS_POSTING in offset number indicates pivot tuple */

	/* 偏移量中不含 BT_IS_POSTING 表示这是一个 pivot 元组 */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & BT_IS_POSTING) != 0)
		return false;

	return true;
}

/*
 * BTreeTupleIsPosting: report whether the given index tuple is a posting-list
 * tuple.  It requires the alternative-TID bit in t_info and the posting-list
 * bit in the t_tid offset to both be set, which distinguishes posting-list
 * tuples from pivot tuples and plain non-pivot tuples.
 *
 * BTreeTupleIsPosting：报告给定的索引元组是否为倒排列表元组。它要求
 * t_info 中的 alternative-TID 位和 t_tid 偏移量中的倒排列表位都被设置，
 * 从而将倒排列表元组与 pivot 元组以及普通非 pivot 元组区分开来。
 */
static inline bool
BTreeTupleIsPosting(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* presence of BT_IS_POSTING in offset number indicates posting tuple */

	/* 偏移量中含有 BT_IS_POSTING 表示这是一个倒排列表元组 */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & BT_IS_POSTING) == 0)
		return false;

	return true;
}

/*
 * BTreeTupleSetPosting: turn a tuple into a posting-list tuple by recording
 * the number of heap TIDs and the byte offset of the posting list.  It sets
 * the alternative-TID bit, stores nhtids together with the BT_IS_POSTING flag
 * in the t_tid offset, and stashes the posting offset in the block field.
 *
 * BTreeTupleSetPosting：通过记录堆 TID 的数量以及倒排列表的字节偏移量，
 * 将一个元组转变为倒排列表元组。它设置 alternative-TID 位，将 nhtids 连同
 * BT_IS_POSTING 标志一起存入 t_tid 偏移量中，并把倒排偏移量藏入块字段中。
 */
static inline void
BTreeTupleSetPosting(IndexTuple itup, uint16 nhtids, int postingoffset)
{
	Assert(nhtids > 1);
	Assert((nhtids & BT_STATUS_OFFSET_MASK) == 0);
	Assert((size_t) postingoffset == MAXALIGN(postingoffset));
	Assert(postingoffset < INDEX_SIZE_MASK);
	Assert(!BTreeTupleIsPivot(itup));

	itup->t_info |= INDEX_ALT_TID_MASK;
	ItemPointerSetOffsetNumber(&itup->t_tid, (nhtids | BT_IS_POSTING));
	ItemPointerSetBlockNumber(&itup->t_tid, postingoffset);
}

/*
 * BTreeTupleGetNPosting: return the number of heap TIDs stored in a
 * posting-list tuple.  It reads the t_tid offset number and masks off the
 * status bits, leaving just the count that was encoded by
 * BTreeTupleSetPosting().
 *
 * BTreeTupleGetNPosting：返回倒排列表元组中存储的堆 TID 数量。它读取
 * t_tid 偏移量并屏蔽掉状态位，只留下由 BTreeTupleSetPosting() 编码的
 * 计数值。
 */
static inline uint16
BTreeTupleGetNPosting(IndexTuple posting)
{
	OffsetNumber existing;

	Assert(BTreeTupleIsPosting(posting));

	existing = ItemPointerGetOffsetNumberNoCheck(&posting->t_tid);
	return (existing & BT_OFFSET_MASK);
}

/*
 * BTreeTupleGetPostingOffset: return the byte offset within a posting-list
 * tuple at which its posting list (the TID array) begins.  The value is read
 * from the block-number field of t_tid, where BTreeTupleSetPosting() stored
 * it.
 *
 * BTreeTupleGetPostingOffset：返回倒排列表元组内其倒排列表（TID 数组）
 * 起始处的字节偏移量。该值从 t_tid 的块号字段中读取，BTreeTupleSetPosting()
 * 正是将其存储在那里。
 */
static inline uint32
BTreeTupleGetPostingOffset(IndexTuple posting)
{
	Assert(BTreeTupleIsPosting(posting));

	return ItemPointerGetBlockNumberNoCheck(&posting->t_tid);
}

/*
 * BTreeTupleGetPosting: return a pointer to the first heap TID in a
 * posting-list tuple by advancing from the tuple start by the stored posting
 * offset.
 *
 * BTreeTupleGetPosting：通过从元组起始处前移所存储的倒排偏移量，返回
 * 指向倒排列表元组中第一个堆 TID 的指针。
 */
static inline ItemPointer
BTreeTupleGetPosting(IndexTuple posting)
{
	return (ItemPointer) ((char *) posting +
						  BTreeTupleGetPostingOffset(posting));
}

/*
 * BTreeTupleGetPostingN: return a pointer to the n-th (0-based) heap TID in a
 * posting-list tuple, computed as an offset from the start of the posting
 * list.
 *
 * BTreeTupleGetPostingN：返回指向倒排列表元组中第 n 个（从 0 开始计数）
 * 堆 TID 的指针，其计算方式是相对于倒排列表起始处的偏移。
 */
static inline ItemPointer
BTreeTupleGetPostingN(IndexTuple posting, int n)
{
	return BTreeTupleGetPosting(posting) + n;
}

/*
 * Get/set downlink block number in pivot tuple.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */

/*
 * 获取/设置 pivot 元组中的 downlink 块号。
 *
 * 注意：不能断言该元组为 pivot 元组。如果那样做，!heapkeyspace 索引将会
 * 出现假阳性的断言失败。
 */

/*
 * BTreeTupleGetDownLink: return the child block number (downlink) stored in a
 * pivot tuple's t_tid, used to descend one level while searching the tree.
 *
 * BTreeTupleGetDownLink：返回存储在 pivot 元组 t_tid 中的子块号
 * （downlink），用于在搜索树时向下深入一层。
 */
static inline BlockNumber
BTreeTupleGetDownLink(IndexTuple pivot)
{
	return ItemPointerGetBlockNumberNoCheck(&pivot->t_tid);
}

/*
 * BTreeTupleSetDownLink: store the given child block number as the downlink
 * in a pivot tuple's t_tid.
 *
 * BTreeTupleSetDownLink：将给定的子块号作为 downlink 存入 pivot 元组的
 * t_tid 中。
 */
static inline void
BTreeTupleSetDownLink(IndexTuple pivot, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&pivot->t_tid, blkno);
}

/*
 * Get number of attributes within tuple.
 *
 * Note that this does not include an implicit tiebreaker heap TID
 * attribute, if any.  Note also that the number of key attributes must be
 * explicitly represented in all heapkeyspace pivot tuples.
 *
 * Note: This is defined as a macro rather than an inline function to
 * avoid including rel.h.
 */

/*
 * 获取元组内的属性数量。
 *
 * 注意这不包括隐式的、作为决胜键的堆 TID 属性（如果有的话）。还要注意，
 * 在所有 heapkeyspace 的 pivot 元组中，键属性的数量必须被显式表示。
 *
 * 注意：这里将其定义为宏而非内联函数，是为了避免包含 rel.h。
 */
#define BTreeTupleGetNAtts(itup, rel)	\
	( \
		(BTreeTupleIsPivot(itup)) ? \
		( \
			ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & BT_OFFSET_MASK \
		) \
		: \
		IndexRelationGetNumberOfAttributes(rel) \
	)

/*
 * Set number of key attributes in tuple.
 *
 * The heap TID tiebreaker attribute bit may also be set here, indicating that
 * a heap TID value will be stored at the end of the tuple (i.e. using the
 * special pivot tuple representation).
 */

/*
 * 设置元组中键属性的数量。
 *
 * 作为决胜键的堆 TID 属性位也可能在此被设置，表示会在元组末尾存储一个
 * 堆 TID 值（即使用特殊的 pivot 元组表示）。
 */

/*
 * BTreeTupleSetNAtts: record the key-attribute count for a pivot tuple.  It
 * sets the alternative-TID bit, optionally OR-s in the heap-TID tiebreaker
 * flag, and writes the count (never the posting bit) into the t_tid offset so
 * the tuple is recognized as a pivot tuple.
 *
 * BTreeTupleSetNAtts：为 pivot 元组记录键属性的数量。它设置 alternative-TID
 * 位，可选地按位或上作为决胜键的堆 TID 标志，并将计数值（绝不含倒排位）
 * 写入 t_tid 偏移量，以便该元组被识别为 pivot 元组。
 */
static inline void
BTreeTupleSetNAtts(IndexTuple itup, uint16 nkeyatts, bool heaptid)
{
	Assert(nkeyatts <= INDEX_MAX_KEYS);
	Assert((nkeyatts & BT_STATUS_OFFSET_MASK) == 0);
	Assert(!heaptid || nkeyatts > 0);
	Assert(!BTreeTupleIsPivot(itup) || nkeyatts == 0);

	itup->t_info |= INDEX_ALT_TID_MASK;

	if (heaptid)
		nkeyatts |= BT_PIVOT_HEAP_TID_ATTR;

	/* BT_IS_POSTING bit is deliberately unset here */

	/* 此处刻意不设置 BT_IS_POSTING 位 */
	ItemPointerSetOffsetNumber(&itup->t_tid, nkeyatts);
	Assert(BTreeTupleIsPivot(itup));
}

/*
 * Get/set leaf page's "top parent" link from its high key.  Used during page
 * deletion.
 *
 * Note: Cannot assert that tuple is a pivot tuple.  If we did so then
 * !heapkeyspace indexes would exhibit false positive assertion failures.
 */

/*
 * 从叶子页的高键中获取/设置该页的“顶层父节点”（top parent）链接。用于
 * 页面删除期间。
 *
 * 注意：不能断言该元组为 pivot 元组。如果那样做，!heapkeyspace 索引将会
 * 出现假阳性的断言失败。
 */

/*
 * BTreeTupleGetTopParent: return the top-parent block number stored in a
 * half-dead leaf page's high key, read from its t_tid block field.  Page
 * deletion uses this link to walk up the subtree being removed.
 *
 * BTreeTupleGetTopParent：返回存储在半死叶子页高键中的顶层父节点块号，
 * 从其 t_tid 块字段读取。页面删除利用该链接向上遍历正被移除的子树。
 */
static inline BlockNumber
BTreeTupleGetTopParent(IndexTuple leafhikey)
{
	return ItemPointerGetBlockNumberNoCheck(&leafhikey->t_tid);
}

/*
 * BTreeTupleSetTopParent: store the given block number as the top-parent link
 * in a leaf page's high key, then reset the tuple's attribute count to zero
 * (a truncated pivot) so the high key carries only the link.
 *
 * BTreeTupleSetTopParent：将给定块号作为顶层父节点链接存入叶子页的高键
 * 中，然后将该元组的属性数量重置为零（即一个被截断的 pivot），使该高键
 * 仅承载此链接。
 */
static inline void
BTreeTupleSetTopParent(IndexTuple leafhikey, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&leafhikey->t_tid, blkno);
	BTreeTupleSetNAtts(leafhikey, 0, false);
}

/*
 * Get tiebreaker heap TID attribute, if any.
 *
 * This returns the first/lowest heap TID in the case of a posting list tuple.
 */

/*
 * 获取作为决胜键的堆 TID 属性（如果有的话）。
 *
 * 对于倒排列表元组，此函数返回第一个/最小的堆 TID。
 */

/*
 * BTreeTupleGetHeapTID: return a pointer to a tuple's tiebreaker heap TID.
 * For a pivot tuple it reads the trailing heap TID when present (else NULL);
 * for a posting-list tuple it returns the first (lowest) TID; for a plain
 * non-pivot tuple it simply returns &t_tid.
 *
 * BTreeTupleGetHeapTID：返回指向元组中作为决胜键的堆 TID 的指针。对于
 * pivot 元组，当末尾存在堆 TID 时读取之（否则返回 NULL）；对于倒排列表
 * 元组，返回第一个（最小的）TID；对于普通非 pivot 元组，则直接返回
 * &t_tid。
 */
static inline ItemPointer
BTreeTupleGetHeapTID(IndexTuple itup)
{
	if (BTreeTupleIsPivot(itup))
	{
		/* Pivot tuple heap TID representation? */

		/* pivot 元组是否采用堆 TID 表示？ */
		if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
			 BT_PIVOT_HEAP_TID_ATTR) != 0)
			return (ItemPointer) ((char *) itup + IndexTupleSize(itup) -
								  sizeof(ItemPointerData));

		/* Heap TID attribute was truncated */

		/* 堆 TID 属性已被截断 */
		return NULL;
	}
	else if (BTreeTupleIsPosting(itup))
		return BTreeTupleGetPosting(itup);

	return &itup->t_tid;
}

/*
 * Get maximum heap TID attribute, which could be the only TID in the case of
 * a non-pivot tuple that does not have a posting list.
 *
 * Works with non-pivot tuples only.
 */

/*
 * 获取最大的堆 TID 属性；对于没有倒排列表的非 pivot 元组而言，它可能是
 * 唯一的 TID。
 *
 * 仅适用于非 pivot 元组。
 */

/*
 * BTreeTupleGetMaxHeapTID: return a pointer to the highest heap TID in a
 * non-pivot tuple.  For a posting-list tuple it returns the last element of
 * the posting list; otherwise the tuple's single t_tid is the maximum.
 *
 * BTreeTupleGetMaxHeapTID：返回指向非 pivot 元组中最大堆 TID 的指针。对于
 * 倒排列表元组，返回倒排列表的最后一个元素；否则元组唯一的 t_tid 即为
 * 最大值。
 */
static inline ItemPointer
BTreeTupleGetMaxHeapTID(IndexTuple itup)
{
	Assert(!BTreeTupleIsPivot(itup));

	if (BTreeTupleIsPosting(itup))
	{
		uint16		nposting = BTreeTupleGetNPosting(itup);

		return BTreeTupleGetPostingN(itup, nposting - 1);
	}

	return &itup->t_tid;
}

/*
 *	Operator strategy numbers for B-tree have been moved to access/stratnum.h,
 *	because many places need to use them in ScanKeyInit() calls.
 *
 *	The strategy numbers are chosen so that we can commute them by
 *	subtraction, thus:
 */

/*
 *	B-tree 的操作符策略号已被移至 access/stratnum.h，因为许多地方需要在
 *	ScanKeyInit() 调用中使用它们。
 *
 *	选择这些策略号是为了能够通过减法来交换它们，如下：
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (BTORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.  This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (BTSORTSUPPORT_PROC).  For full details, see
 *	src/include/utils/sortsupport.h.
 *
 *	To support window frames defined by "RANGE offset PRECEDING/FOLLOWING",
 *	an operator class may choose to offer a third amproc procedure
 *	(BTINRANGE_PROC), independently of whether it offers sortsupport.
 *	For full details, see doc/src/sgml/btree.sgml.
 *
 *	To facilitate B-Tree deduplication, an operator class may choose to
 *	offer a forth amproc procedure (BTEQUALIMAGE_PROC).  For full details,
 *	see doc/src/sgml/btree.sgml.
 *
 *	An operator class may choose to offer a fifth amproc procedure
 *	(BTOPTIONS_PROC).  These procedures define a set of user-visible
 *	parameters that can be used to control operator class behavior.  None of
 *	the built-in B-Tree operator classes currently register an "options" proc.
 *
 *	To facilitate more efficient B-Tree skip scans, an operator class may
 *	choose to offer a sixth amproc procedure (BTSKIPSUPPORT_PROC).  For full
 *	details, see src/include/utils/skipsupport.h.
 */

/*
 *	当声明一个新的操作符类时，我们要求用户为我们提供一个 amproc 过程
 *	（BTORDER_PROC），用于判断对于两个键 a 和 b，是 a < b、a = b 还是
 *	a > b。该例程必须在这三种情况下分别返回 < 0、0、> 0。
 *
 *	为便于加速排序，操作符类可以选择提供第二个过程（BTSORTSUPPORT_PROC）。
 *	完整细节请参见 src/include/utils/sortsupport.h。
 *
 *	为支持由“RANGE offset PRECEDING/FOLLOWING”定义的窗口帧，操作符类可以
 *	选择提供第三个 amproc 过程（BTINRANGE_PROC），这与它是否提供 sortsupport
 *	无关。完整细节请参见 doc/src/sgml/btree.sgml。
 *
 *	为便于 B-Tree 去重，操作符类可以选择提供第四个 amproc 过程
 *	（BTEQUALIMAGE_PROC）。完整细节请参见 doc/src/sgml/btree.sgml。
 *
 *	操作符类可以选择提供第五个 amproc 过程（BTOPTIONS_PROC）。这些过程
 *	定义了一组可用于控制操作符类行为的用户可见参数。目前所有内置的 B-Tree
 *	操作符类都未注册“options”过程。
 *
 *	为便于实现更高效的 B-Tree 跳跃扫描，操作符类可以选择提供第六个 amproc
 *	过程（BTSKIPSUPPORT_PROC）。完整细节请参见 src/include/utils/skipsupport.h。
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2
#define BTINRANGE_PROC		3
#define BTEQUALIMAGE_PROC	4
#define BTOPTIONS_PROC		5
#define BTSKIPSUPPORT_PROC	6
#define BTNProcs			6

/*
 *	We need to be able to tell the difference between read and write
 *	requests for pages, in order to do locking correctly.
 */

/*
 *	我们需要能够区分对页面的读请求与写请求，以便正确地进行加锁。
 */

#define BT_READ			BUFFER_LOCK_SHARE
#define BT_WRITE		BUFFER_LOCK_EXCLUSIVE

/*
 * BTStackData -- As we descend a tree, we push the location of pivot
 * tuples whose downlink we are about to follow onto a private stack.  If
 * we split a leaf, we use this stack to walk back up the tree and insert
 * data into its parent page at the correct location.  We also have to
 * recursively insert into the grandparent page if and when the parent page
 * splits.  Our private stack can become stale due to concurrent page
 * splits and page deletions, but it should never give us an irredeemably
 * bad picture.
 */

/*
 * BTStackData —— 当我们向下遍历树时，会把即将沿其 downlink 前进的 pivot
 * 元组的位置压入一个私有栈。如果我们分裂了某个叶子页，就使用该栈沿树
 * 向上回溯，并在其父页的正确位置插入数据。如果父页发生分裂，我们还必须
 * 递归地向祖父页插入。由于并发的页面分裂和页面删除，我们的私有栈可能
 * 变得陈旧，但它绝不应给出一幅无可挽回的错误图景。
 */
typedef struct BTStackData
{
	BlockNumber bts_blkno;
	OffsetNumber bts_offset;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

/*
 * BTScanInsertData is the btree-private state needed to find an initial
 * position for an indexscan, or to insert new tuples -- an "insertion
 * scankey" (not to be confused with a search scankey).  It's used to descend
 * a B-Tree using _bt_search.
 *
 * heapkeyspace indicates if we expect all keys in the index to be physically
 * unique because heap TID is used as a tiebreaker attribute, and if index may
 * have truncated key attributes in pivot tuples.  This is actually a property
 * of the index relation itself (not an indexscan).  heapkeyspace indexes are
 * indexes whose version is >= version 4.  It's convenient to keep this close
 * by, rather than accessing the metapage repeatedly.
 *
 * allequalimage is set to indicate that deduplication is safe for the index.
 * This is also a property of the index relation rather than an indexscan.
 *
 * anynullkeys indicates if any of the keys had NULL value when scankey was
 * built from index tuple (note that already-truncated tuple key attributes
 * set NULL as a placeholder key value, which also affects value of
 * anynullkeys).  This is a convenience for unique index non-pivot tuple
 * insertion, which usually temporarily unsets scantid, but shouldn't iff
 * anynullkeys is true.  Value generally matches non-pivot tuple's HasNulls
 * bit, but may not when inserting into an INCLUDE index (tuple header value
 * is affected by the NULL-ness of both key and non-key attributes).
 *
 * See comments in _bt_first for an explanation of the nextkey and backward
 * fields.
 *
 * scantid is the heap TID that is used as a final tiebreaker attribute.  It
 * is set to NULL when index scan doesn't need to find a position for a
 * specific physical tuple.  Must be set when inserting new tuples into
 * heapkeyspace indexes, since every tuple in the tree unambiguously belongs
 * in one exact position (it's never set with !heapkeyspace indexes, though).
 * Despite the representational difference, nbtree search code considers
 * scantid to be just another insertion scankey attribute.
 *
 * scankeys is an array of scan key entries for attributes that are compared
 * before scantid (user-visible attributes).  keysz is the size of the array.
 * During insertion, there must be a scan key for every attribute, but when
 * starting a regular index scan some can be omitted.  The array is used as a
 * flexible array member, though it's sized in a way that makes it possible to
 * use stack allocations.  See nbtree/README for full details.
 */

/*
 * BTScanInsertData 是查找索引扫描初始位置或插入新元组时所需的 btree 私有
 * 状态——即一个“插入扫描键”（不要与搜索扫描键混淆）。它用于借助
 * _bt_search 向下遍历 B-Tree。
 *
 * heapkeyspace 指示我们是否期望索引中的所有键在物理上都是唯一的（因为
 * 堆 TID 被用作决胜键属性），以及索引在 pivot 元组中是否可能带有被截断的
 * 键属性。这实际上是索引关系本身（而非某次索引扫描）的属性。heapkeyspace
 * 索引是版本 >= 4 的索引。将其就近保存会比较方便，而不必反复访问元页。
 *
 * allequalimage 被设置以表示对该索引进行去重是安全的。这同样是索引关系
 * 而非某次索引扫描的属性。
 *
 * anynullkeys 指示在从索引元组构建扫描键时是否有任何键具有 NULL 值（注意
 * 已被截断的元组键属性会将 NULL 设置为占位键值，这也会影响 anynullkeys
 * 的值）。这为唯一索引的非 pivot 元组插入提供了便利，该插入通常会临时
 * 取消设置 scantid，但当且仅当 anynullkeys 为 true 时则不应如此。该值一般
 * 与非 pivot 元组的 HasNulls 位相匹配，但在向 INCLUDE 索引插入时可能不
 * 匹配（元组头的值同时受键属性和非键属性是否为 NULL 的影响）。
 *
 * 关于 nextkey 与 backward 字段的解释，请参见 _bt_first 中的注释。
 *
 * scantid 是用作最终决胜键属性的堆 TID。当索引扫描不需要为某个特定物理
 * 元组查找位置时，它被设为 NULL。向 heapkeyspace 索引插入新元组时必须
 * 设置它，因为树中的每个元组都无歧义地属于某个确切位置（不过它在
 * !heapkeyspace 索引中从不被设置）。尽管表示方式不同，nbtree 搜索代码将
 * scantid 视为又一个插入扫描键属性。
 *
 * scankeys 是一个扫描键条目数组，对应在 scantid 之前比较的属性（用户可见
 * 属性）。keysz 是该数组的大小。在插入期间，每个属性都必须有一个扫描键，
 * 但在启动常规索引扫描时可以省略其中一些。该数组用作柔性数组成员，不过
 * 其大小的设定方式使得可以使用栈分配。完整细节请参见 nbtree/README。
 */
typedef struct BTScanInsertData
{
	bool		heapkeyspace;
	bool		allequalimage;
	bool		anynullkeys;
	bool		nextkey;
	bool		backward;		/* backward index scan? */

	/* backward：是否为向后的索引扫描？ */
	ItemPointer scantid;		/* tiebreaker for scankeys */

	/* scantid：scankeys 的决胜键 */
	int			keysz;			/* Size of scankeys array */

	/* keysz：scankeys 数组的大小 */
	ScanKeyData scankeys[INDEX_MAX_KEYS];	/* Must appear last */

	/* scankeys：必须出现在最后 */
} BTScanInsertData;

typedef BTScanInsertData *BTScanInsert;

/*
 * BTInsertStateData is a working area used during insertion.
 *
 * This is filled in after descending the tree to the first leaf page the new
 * tuple might belong on.  Tracks the current position while performing
 * uniqueness check, before we have determined which exact page to insert
 * to.
 *
 * (This should be private to nbtinsert.c, but it's also used by
 * _bt_binsrch_insert)
 */

/*
 * BTInsertStateData 是插入期间使用的工作区。
 *
 * 它在向下遍历树到达新元组可能所属的第一个叶子页之后被填充。在我们尚未
 * 确定要插入到哪个确切页之前，它会在执行唯一性检查时跟踪当前位置。
 *
 * （它本应是 nbtinsert.c 私有的，但 _bt_binsrch_insert 也会使用它）
 */
typedef struct BTInsertStateData
{
	IndexTuple	itup;			/* Item we're inserting */

	/* itup：我们正在插入的项 */
	Size		itemsz;			/* Size of itup -- should be MAXALIGN()'d */

	/* itemsz：itup 的大小——应经过 MAXALIGN() 对齐 */
	BTScanInsert itup_key;		/* Insertion scankey */

	/* itup_key：插入扫描键 */

	/* Buffer containing leaf page we're likely to insert itup on */

	/* 包含我们很可能要在其上插入 itup 的叶子页的缓冲区 */
	Buffer		buf;

	/*
	 * Cache of bounds within the current buffer.  Only used for insertions
	 * where _bt_check_unique is called.  See _bt_binsrch_insert and
	 * _bt_findinsertloc for details.
	 */

	/*
	 * 当前缓冲区内边界的缓存。仅用于调用了 _bt_check_unique 的插入。细节
	 * 请参见 _bt_binsrch_insert 和 _bt_findinsertloc。
	 */
	bool		bounds_valid;
	OffsetNumber low;
	OffsetNumber stricthigh;

	/*
	 * if _bt_binsrch_insert found the location inside existing posting list,
	 * save the position inside the list.  -1 sentinel value indicates overlap
	 * with an existing posting list tuple that has its LP_DEAD bit set.
	 */

	/*
	 * 如果 _bt_binsrch_insert 在现有倒排列表内部找到了位置，则保存在该
	 * 列表内部的位置。-1 这个哨兵值表示与某个已设置 LP_DEAD 位的现有倒排
	 * 列表元组发生了重叠。
	 */
	int			postingoff;
} BTInsertStateData;

typedef BTInsertStateData *BTInsertState;

/*
 * State used to representing an individual pending tuple during
 * deduplication.
 */

/*
 * 去重期间用于表示单个待处理元组的状态。
 */
typedef struct BTDedupInterval
{
	OffsetNumber baseoff;
	uint16		nitems;
} BTDedupInterval;

/*
 * BTDedupStateData is a working area used during deduplication.
 *
 * The status info fields track the state of a whole-page deduplication pass.
 * State about the current pending posting list is also tracked.
 *
 * A pending posting list is comprised of a contiguous group of equal items
 * from the page, starting from page offset number 'baseoff'.  This is the
 * offset number of the "base" tuple for new posting list.  'nitems' is the
 * current total number of existing items from the page that will be merged to
 * make a new posting list tuple, including the base tuple item.  (Existing
 * items may themselves be posting list tuples, or regular non-pivot tuples.)
 *
 * The total size of the existing tuples to be freed when pending posting list
 * is processed gets tracked by 'phystupsize'.  This information allows
 * deduplication to calculate the space saving for each new posting list
 * tuple, and for the entire pass over the page as a whole.
 */

/*
 * BTDedupStateData 是去重期间使用的工作区。
 *
 * 状态信息字段跟踪整页去重遍历的状态。关于当前待处理倒排列表的状态也会
 * 被跟踪。
 *
 * 一个待处理倒排列表由页面中一组连续的相等项组成，从页偏移量 'baseoff'
 * 开始。这是新倒排列表“基”元组的偏移量。'nitems' 是当前将被合并以生成
 * 一个新倒排列表元组的、来自该页的现有项的总数，包括基元组项在内。
 * （现有项本身可能是倒排列表元组，也可能是常规的非 pivot 元组。）
 *
 * 当处理待处理倒排列表时将要释放的现有元组的总大小由 'phystupsize'
 * 跟踪。该信息使去重能够计算每个新倒排列表元组以及整页遍历整体所节省的
 * 空间。
 */
typedef struct BTDedupStateData
{
	/* Deduplication status info for entire pass over page */

	/* 整页遍历的去重状态信息 */
	bool		deduplicate;	/* Still deduplicating page? */

	/* deduplicate：是否仍在对页面进行去重？ */
	int			nmaxitems;		/* Number of max-sized tuples so far */

	/* nmaxitems：迄今为止最大尺寸元组的数量 */
	Size		maxpostingsize; /* Limit on size of final tuple */

	/* maxpostingsize：最终元组大小的上限 */

	/* Metadata about base tuple of current pending posting list */

	/* 关于当前待处理倒排列表基元组的元数据 */
	IndexTuple	base;			/* Use to form new posting list */

	/* base：用于形成新的倒排列表 */
	OffsetNumber baseoff;		/* page offset of base */

	/* baseoff：base 的页偏移量 */
	Size		basetupsize;	/* base size without original posting list */

	/* basetupsize：不含原始倒排列表的 base 大小 */

	/* Other metadata about pending posting list */

	/* 关于待处理倒排列表的其他元数据 */
	ItemPointer htids;			/* Heap TIDs in pending posting list */

	/* htids：待处理倒排列表中的堆 TID */
	int			nhtids;			/* Number of heap TIDs in htids array */

	/* nhtids：htids 数组中堆 TID 的数量 */
	int			nitems;			/* Number of existing tuples/line pointers */

	/* nitems：现有元组/行指针的数量 */
	Size		phystupsize;	/* Includes line pointer overhead */

	/* phystupsize：包含行指针开销 */

	/*
	 * Array of tuples to go on new version of the page.  Contains one entry
	 * for each group of consecutive items.  Note that existing tuples that
	 * will not become posting list tuples do not appear in the array (they
	 * are implicitly unchanged by deduplication pass).
	 */

	/*
	 * 将进入页面新版本的元组数组。每组连续项对应一个条目。注意，那些不会
	 * 成为倒排列表元组的现有元组不会出现在该数组中（它们在去重遍历中被
	 * 隐式地保持不变）。
	 */
	int			nintervals;		/* current number of intervals in array */

	/* nintervals：数组中当前的区间数 */
	BTDedupInterval intervals[MaxIndexTuplesPerPage];
} BTDedupStateData;

typedef BTDedupStateData *BTDedupState;

/*
 * BTVacuumPostingData is state that represents how to VACUUM (or delete) a
 * posting list tuple when some (though not all) of its TIDs are to be
 * deleted.
 *
 * Convention is that itup field is the original posting list tuple on input,
 * and palloc()'d final tuple used to overwrite existing tuple on output.
 */

/*
 * BTVacuumPostingData 是一种状态，用于表示当某个倒排列表元组的部分（但
 * 并非全部）TID 将被删除时，如何对其进行 VACUUM（或删除）。
 *
 * 约定是：输入时 itup 字段为原始倒排列表元组，输出时则为 palloc() 分配的、
 * 用于覆盖现有元组的最终元组。
 */
typedef struct BTVacuumPostingData
{
	/* Tuple that will be/was updated */

	/* 将被/已被更新的元组 */
	IndexTuple	itup;
	OffsetNumber updatedoffset;

	/* State needed to describe final itup in WAL */

	/* 在 WAL 中描述最终 itup 所需的状态 */
	uint16		ndeletedtids;
	uint16		deletetids[FLEXIBLE_ARRAY_MEMBER];
} BTVacuumPostingData;

typedef BTVacuumPostingData *BTVacuumPosting;

/*
 * BTScanOpaqueData is the btree-private state needed for an indexscan.
 * This consists of preprocessed scan keys (see _bt_preprocess_keys() for
 * details of the preprocessing), information about the current location
 * of the scan, and information about the marked location, if any.  (We use
 * BTScanPosData to represent the data needed for each of current and marked
 * locations.)	In addition we can remember some known-killed index entries
 * that must be marked before we can move off the current page.
 *
 * Index scans work a page at a time: we pin and read-lock the page, identify
 * all the matching items on the page and save them in BTScanPosData, then
 * release the read-lock while returning the items to the caller for
 * processing.  This approach minimizes lock/unlock traffic.  We must always
 * drop the lock to make it okay for caller to process the returned items.
 * Whether or not we can also release the pin during this window will vary.
 * We drop the pin (when so->dropPin) to avoid blocking progress by VACUUM
 * (see nbtree/README section about making concurrent TID recycling safe).
 * We'll always release both the lock and the pin on the current page before
 * moving on to its sibling page.
 *
 * If we are doing an index-only scan, we save the entire IndexTuple for each
 * matched item, otherwise only its heap TID and offset.  The IndexTuples go
 * into a separate workspace array; each BTScanPosItem stores its tuple's
 * offset within that array.  Posting list tuples store a "base" tuple once,
 * allowing the same key to be returned for each TID in the posting list
 * tuple.
 */

/*
 * BTScanOpaqueData 是索引扫描所需的 btree 私有状态。它由预处理后的扫描键
 * （关于预处理的细节参见 _bt_preprocess_keys()）、关于扫描当前位置的信息，
 * 以及关于标记位置（如果有的话）的信息组成。（我们使用 BTScanPosData 来
 * 表示当前位置和标记位置各自所需的数据。）此外，我们还可以记住一些已知被
 * 杀死的索引条目，它们必须在我们移出当前页之前被标记。
 *
 * 索引扫描一次处理一页：我们 pin 住并读锁该页，识别页面上所有匹配的项并将
 * 它们保存在 BTScanPosData 中，然后在把这些项返回给调用者处理时释放读锁。
 * 这种方式将加锁/解锁的开销降到最低。我们必须始终释放锁，以便调用者能够
 * 处理返回的项。在这一时间窗口内我们是否也能释放 pin 则视情况而定。我们
 * 在（so->dropPin 时）释放 pin，以避免阻塞 VACUUM 的进展（参见 nbtree/README
 * 中关于使并发 TID 回收安全的章节）。在移动到兄弟页之前，我们总是会释放
 * 当前页上的锁和 pin。
 *
 * 如果我们正在执行 index-only 扫描，则为每个匹配项保存整个 IndexTuple，
 * 否则只保存其堆 TID 和偏移量。这些 IndexTuple 进入一个单独的工作区数组；
 * 每个 BTScanPosItem 存储其元组在该数组中的偏移量。倒排列表元组只存储一次
 * “基”元组，从而允许为倒排列表元组中的每个 TID 返回相同的键。
 */

typedef struct BTScanPosItem	/* what we remember about each match */

/* BTScanPosItem：我们对每个匹配项所记住的内容 */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */

	/* heapTid：所引用堆项的 TID */
	OffsetNumber indexOffset;	/* index item's location within page */

	/* indexOffset：索引项在页内的位置 */
	LocationIndex tupleOffset;	/* IndexTuple's offset in workspace, if any */

	/* tupleOffset：IndexTuple 在工作区中的偏移量（如果有的话） */
} BTScanPosItem;

typedef struct BTScanPosData
{
	Buffer		buf;			/* currPage buf (invalid means unpinned) */

	/* buf：currPage 的缓冲区（无效表示未 pin 住） */

	/* page details as of the saved position's call to _bt_readpage */

	/* 截至所保存位置调用 _bt_readpage 时的页面详情 */
	BlockNumber currPage;		/* page referenced by items array */

	/* currPage：items 数组所引用的页 */
	BlockNumber prevPage;		/* currPage's left link */

	/* prevPage：currPage 的左链接 */
	BlockNumber nextPage;		/* currPage's right link */

	/* nextPage：currPage 的右链接 */
	XLogRecPtr	lsn;			/* currPage's LSN (when so->dropPin) */

	/* lsn：currPage 的 LSN（当 so->dropPin 时） */

	/* scan direction for the saved position's call to _bt_readpage */

	/* 所保存位置调用 _bt_readpage 时的扫描方向 */
	ScanDirection dir;

	/*
	 * If we are doing an index-only scan, nextTupleOffset is the first free
	 * location in the associated tuple storage workspace.
	 */

	/*
	 * 如果我们正在执行 index-only 扫描，nextTupleOffset 是相关联的元组存储
	 * 工作区中第一个空闲位置。
	 */
	int			nextTupleOffset;

	/*
	 * moreLeft and moreRight track whether we think there may be matching
	 * index entries to the left and right of the current page, respectively.
	 */

	/*
	 * moreLeft 和 moreRight 分别跟踪我们是否认为当前页的左侧和右侧可能还有
	 * 匹配的索引条目。
	 */
	bool		moreLeft;
	bool		moreRight;

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */

	/*
	 * items 数组始终按索引顺序排列（即 indexoffset 递增）。当向后扫描时，
	 * 从后往前填充该数组会更方便，因此我们从最后一个槽位开始向下填充。
	 * 因此我们同时需要一个“首个有效条目”计数器和一个“最后有效条目”
	 * 计数器。itemIndex 是一个游标，指示最后返回给调用者的是哪个条目。
	 */
	int			firstItem;		/* first valid index in items[] */

	/* firstItem：items[] 中首个有效的索引 */
	int			lastItem;		/* last valid index in items[] */

	/* lastItem：items[] 中最后一个有效的索引 */
	int			itemIndex;		/* current index in items[] */

	/* itemIndex：items[] 中当前的索引 */

	BTScanPosItem items[MaxTIDsPerBTreePage];	/* MUST BE LAST */

	/* items：必须放在最后 */
} BTScanPosData;

typedef BTScanPosData *BTScanPos;

#define BTScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)
#define BTScanPosUnpin(scanpos) \
	do { \
		ReleaseBuffer((scanpos).buf); \
		(scanpos).buf = InvalidBuffer; \
	} while (0)
#define BTScanPosUnpinIfPinned(scanpos) \
	do { \
		if (BTScanPosIsPinned(scanpos)) \
			BTScanPosUnpin(scanpos); \
	} while (0)

#define BTScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)
#define BTScanPosInvalidate(scanpos) \
	do { \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).currPage = InvalidBlockNumber; \
	} while (0)

/* We need one of these for each equality-type SK_SEARCHARRAY scan key */

/* 每个相等类型的 SK_SEARCHARRAY 扫描键都需要一个这样的结构 */
typedef struct BTArrayKeyInfo
{
	/* fields set for both kinds of array (SAOP arrays and skip arrays) */

	/* 为两种数组（SAOP 数组和 skip 数组）都设置的字段 */
	int			scan_key;		/* index of associated key in keyData */

	/* scan_key：所关联键在 keyData 中的索引 */
	int			num_elems;		/* number of elems (-1 means skip array) */

	/* num_elems：元素数量（-1 表示 skip 数组） */

	/* fields set for ScalarArrayOpExpr arrays only */

	/* 仅为 ScalarArrayOpExpr 数组设置的字段 */
	Datum	   *elem_values;	/* array of num_elems Datums */

	/* elem_values：由 num_elems 个 Datum 组成的数组 */
	int			cur_elem;		/* index of current element in elem_values */

	/* cur_elem：当前元素在 elem_values 中的索引 */

	/* fields set for skip arrays only */

	/* 仅为 skip 数组设置的字段 */
	int16		attlen;			/* attr's length, in bytes */

	/* attlen：属性的长度，以字节计 */
	bool		attbyval;		/* attr's FormData_pg_attribute.attbyval */

	/* attbyval：属性的 FormData_pg_attribute.attbyval */
	bool		null_elem;		/* NULL is lowest/highest element? */

	/* null_elem：NULL 是最小/最大元素吗？ */
	SkipSupport sksup;			/* skip support (NULL if opclass lacks it) */

	/* sksup：skip support（若操作符类缺少它则为 NULL） */
	ScanKey		low_compare;	/* array's > or >= lower bound */

	/* low_compare：数组的 > 或 >= 下界 */
	ScanKey		high_compare;	/* array's < or <= upper bound */

	/* high_compare：数组的 < 或 <= 上界 */
} BTArrayKeyInfo;

typedef struct BTScanOpaqueData
{
	/* these fields are set by _bt_preprocess_keys(): */

	/* 以下字段由 _bt_preprocess_keys() 设置： */
	bool		qual_ok;		/* false if qual can never be satisfied */

	/* qual_ok：若条件永远无法满足则为 false */
	int			numberOfKeys;	/* number of preprocessed scan keys */

	/* numberOfKeys：预处理后的扫描键数量 */
	ScanKey		keyData;		/* array of preprocessed scan keys */

	/* keyData：预处理后的扫描键数组 */

	/* workspace for SK_SEARCHARRAY support */

	/* 用于支持 SK_SEARCHARRAY 的工作区 */
	int			numArrayKeys;	/* number of equality-type array keys */

	/* numArrayKeys：相等类型数组键的数量 */
	bool		skipScan;		/* At least one skip array in arrayKeys[]? */

	/* skipScan：arrayKeys[] 中是否至少有一个 skip 数组？ */
	bool		needPrimScan;	/* New prim scan to continue in current dir? */

	/* needPrimScan：是否需要新的原始扫描以在当前方向上继续？ */
	bool		scanBehind;		/* Check scan not still behind on next page? */

	/* scanBehind：检查扫描在下一页上是否不再落后？ */
	bool		oppositeDirCheck;	/* scanBehind opposite-scan-dir check? */

	/* oppositeDirCheck：scanBehind 的反向扫描方向检查？ */
	BTArrayKeyInfo *arrayKeys;	/* info about each equality-type array key */

	/* arrayKeys：关于每个相等类型数组键的信息 */
	FmgrInfo   *orderProcs;		/* ORDER procs for required equality keys */

	/* orderProcs：用于必需相等键的 ORDER 过程 */
	MemoryContext arrayContext; /* scan-lifespan context for array data */

	/* arrayContext：用于数组数据的、生命周期与扫描相同的内存上下文 */

	/* info about killed items if any (killedItems is NULL if never used) */

	/* 关于被杀死项的信息（如果有的话；若从未使用则 killedItems 为 NULL） */
	int		   *killedItems;	/* currPos.items indexes of killed items */

	/* killedItems：被杀死项在 currPos.items 中的索引 */
	int			numKilled;		/* number of currently stored items */

	/* numKilled：当前存储的项数 */
	bool		dropPin;		/* drop leaf pin before btgettuple returns? */

	/* dropPin：在 btgettuple 返回前释放叶子 pin？ */

	/*
	 * If we are doing an index-only scan, these are the tuple storage
	 * workspaces for the currPos and markPos respectively.  Each is of size
	 * BLCKSZ, so it can hold as much as a full page's worth of tuples.
	 */

	/*
	 * 如果我们正在执行 index-only 扫描，这些分别是 currPos 和 markPos 的元组
	 * 存储工作区。每个大小为 BLCKSZ，因此可容纳多达整页的元组。
	 */
	char	   *currTuples;		/* tuple storage for currPos */

	/* currTuples：currPos 的元组存储 */
	char	   *markTuples;		/* tuple storage for markPos */

	/* markTuples：markPos 的元组存储 */

	/*
	 * If the marked position is on the same page as current position, we
	 * don't use markPos, but just keep the marked itemIndex in markItemIndex
	 * (all the rest of currPos is valid for the mark position). Hence, to
	 * determine if there is a mark, first look at markItemIndex, then at
	 * markPos.
	 */

	/*
	 * 如果标记位置与当前位置在同一页上，我们不使用 markPos，而只是将被标记
	 * 的 itemIndex 保存在 markItemIndex 中（currPos 的其余部分对标记位置仍然
	 * 有效）。因此，要确定是否存在标记，先看 markItemIndex，再看 markPos。
	 */
	int			markItemIndex;	/* itemIndex, or -1 if not valid */

	/* markItemIndex：itemIndex，若无效则为 -1 */

	/* keep these last in struct for efficiency */

	/* 出于效率考虑，将这些字段放在结构体的最后 */
	BTScanPosData currPos;		/* current position data */

	/* currPos：当前位置数据 */
	BTScanPosData markPos;		/* marked position, if any */

	/* markPos：标记位置（如果有的话） */
} BTScanOpaqueData;

typedef BTScanOpaqueData *BTScanOpaque;

/*
 * _bt_readpage state used across _bt_checkkeys calls for a page
 */

/*
 * _bt_readpage 的状态，在针对一个页面的多次 _bt_checkkeys 调用之间共享
 */
typedef struct BTReadPageState
{
	/* Input parameters, set by _bt_readpage for _bt_checkkeys */

	/* 输入参数，由 _bt_readpage 为 _bt_checkkeys 设置 */
	OffsetNumber minoff;		/* Lowest non-pivot tuple's offset */

	/* minoff：最低的非 pivot 元组的偏移量 */
	OffsetNumber maxoff;		/* Highest non-pivot tuple's offset */

	/* maxoff：最高的非 pivot 元组的偏移量 */
	IndexTuple	finaltup;		/* Needed by scans with array keys */

	/* finaltup：带数组键的扫描需要它 */
	Page		page;			/* Page being read */

	/* page：正被读取的页 */
	bool		firstpage;		/* page is first for primitive scan? */

	/* firstpage：该页是否为原始扫描的第一页？ */
	bool		forcenonrequired;	/* treat all keys as nonrequired? */

	/* forcenonrequired：是否将所有键都视为非必需？ */
	int			startikey;		/* start comparisons from this scan key */

	/* startikey：从该扫描键开始进行比较 */

	/* Per-tuple input parameters, set by _bt_readpage for _bt_checkkeys */

	/* 每元组的输入参数，由 _bt_readpage 为 _bt_checkkeys 设置 */
	OffsetNumber offnum;		/* current tuple's page offset number */

	/* offnum：当前元组的页偏移量 */

	/* Output parameters, set by _bt_checkkeys for _bt_readpage */

	/* 输出参数，由 _bt_checkkeys 为 _bt_readpage 设置 */
	OffsetNumber skip;			/* Array keys "look ahead" skip offnum */

	/* skip：数组键“向前看”所跳过的 offnum */
	bool		continuescan;	/* Terminate ongoing (primitive) index scan? */

	/* continuescan：是否终止正在进行的（原始）索引扫描？ */

	/*
	 * Private _bt_checkkeys state used to manage "look ahead" optimization
	 * and primscan scheduling (only used during scans with array keys)
	 */

	/*
	 * 私有的 _bt_checkkeys 状态，用于管理“向前看”优化以及原始扫描调度
	 * （仅在带数组键的扫描期间使用）
	 */
	int16		rechecks;
	int16		targetdistance;
	int16		nskipadvances;

} BTReadPageState;

/*
 * We use some private sk_flags bits in preprocessed scan keys.  We're allowed
 * to use bits 16-31 (see skey.h).  The uppermost bits are copied from the
 * index's indoption[] array entry for the index attribute.
 */

/*
 * 我们在预处理后的扫描键中使用一些私有的 sk_flags 位。我们被允许使用第
 * 16-31 位（参见 skey.h）。最高的那些位是从索引针对该索引属性的
 * indoption[] 数组条目复制而来。
 */
#define SK_BT_REQFWD	0x00010000	/* required to continue forward scan */

/* SK_BT_REQFWD：继续向前扫描所必需 */
#define SK_BT_REQBKWD	0x00020000	/* required to continue backward scan */

/* SK_BT_REQBKWD：继续向后扫描所必需 */
#define SK_BT_SKIP		0x00040000	/* skip array on column without input = */

/* SK_BT_SKIP：位于没有输入 = 的列上的 skip 数组 */

/* SK_BT_SKIP-only flags (set and unset by array advancement) */

/* 仅 SK_BT_SKIP 使用的标志（由数组推进来设置和清除） */
#define SK_BT_MINVAL	0x00080000	/* invalid sk_argument, use low_compare */

/* SK_BT_MINVAL：sk_argument 无效，改用 low_compare */
#define SK_BT_MAXVAL	0x00100000	/* invalid sk_argument, use high_compare */

/* SK_BT_MAXVAL：sk_argument 无效，改用 high_compare */
#define SK_BT_NEXT		0x00200000	/* positions the scan > sk_argument */

/* SK_BT_NEXT：将扫描定位到 > sk_argument */
#define SK_BT_PRIOR		0x00400000	/* positions the scan < sk_argument */

/* SK_BT_PRIOR：将扫描定位到 < sk_argument */

/* Remaps pg_index flag bits to uppermost SK_BT_* byte */

/* 将 pg_index 的标志位重新映射到最高的 SK_BT_* 字节 */
#define SK_BT_INDOPTION_SHIFT  24	/* must clear the above bits */

/* SK_BT_INDOPTION_SHIFT：必须清除上述各位 */
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

typedef struct BTOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */

	/* varlena_header_：varlena 头（切勿直接操作！） */
	int			fillfactor;		/* page fill factor in percent (0..100) */

	/* fillfactor：页填充因子，以百分比计（0..100） */
	float8		vacuum_cleanup_index_scale_factor;	/* deprecated */

	/* vacuum_cleanup_index_scale_factor：已弃用 */
	bool		deduplicate_items;	/* Try to deduplicate items? */

	/* deduplicate_items：是否尝试对项进行去重？ */
} BTOptions;

#define BTGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BTREE_AM_OID), \
	 (relation)->rd_options ? \
	 ((BTOptions *) (relation)->rd_options)->fillfactor : \
	 BTREE_DEFAULT_FILLFACTOR)
#define BTGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - BTGetFillFactor(relation)) / 100)
#define BTGetDeduplicateItems(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BTREE_AM_OID), \
	((relation)->rd_options ? \
	 ((BTOptions *) (relation)->rd_options)->deduplicate_items : true))

/*
 * Constant definition for progress reporting.  Phase numbers must match
 * btbuildphasename.
 */

/*
 * 用于进度报告的常量定义。阶段编号必须与 btbuildphasename 相匹配。
 */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 (see progress.h) */

/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE 为 1（参见 progress.h） */
#define PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN		2
#define PROGRESS_BTREE_PHASE_PERFORMSORT_1				3
#define PROGRESS_BTREE_PHASE_PERFORMSORT_2				4
#define PROGRESS_BTREE_PHASE_LEAF_LOAD					5

/*
 * external entry points for btree, in nbtree.c
 */

/*
 * btree 的外部入口点，位于 nbtree.c 中
 */

/*
 * btbuildempty: build an empty btree index in the init fork, used to
 * initialize an unlogged index's persistent state.  Creates and writes out a
 * metapage describing an empty tree.
 *
 * btbuildempty：在 init fork 中构建一个空的 btree 索引，用于初始化无日志
 * 索引的持久化状态。它会创建并写出一个描述空树的元页。
 */
extern void btbuildempty(Relation index);

/*
 * btinsert: the btree access method's index-insertion entry point.  Forms an
 * index tuple from the supplied values and inserts it into the tree via
 * _bt_doinsert, enforcing uniqueness when requested.  Returns whether the
 * insertion was actually performed.
 *
 * btinsert：btree 访问方法的索引插入入口点。它根据提供的值构造一个索引
 * 元组，并通过 _bt_doinsert 将其插入树中，并在被要求时强制唯一性。返回
 * 是否确实执行了插入。
 */
extern bool btinsert(Relation rel, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo);

/*
 * btbeginscan: begin a btree index scan.  Allocates and initializes the
 * BTScanOpaqueData private state that the other scan routines will use to
 * track position and preprocessed keys.
 *
 * btbeginscan：开始一次 btree 索引扫描。它分配并初始化 BTScanOpaqueData
 * 私有状态，其他扫描例程将用它来跟踪位置和预处理后的键。
 */
extern IndexScanDesc btbeginscan(Relation rel, int nkeys, int norderbys);

/*
 * btestimateparallelscan: return the amount of shared memory needed to hold
 * the btree-specific parallel scan descriptor, used when setting up a
 * parallel index scan.
 *
 * btestimateparallelscan：返回容纳 btree 专用并行扫描描述符所需的共享内存
 * 大小，在设置并行索引扫描时使用。
 */
extern Size btestimateparallelscan(Relation rel, int nkeys, int norderbys);

/*
 * btinitparallelscan: initialize the shared-memory parallel scan descriptor
 * (a BTParallelScanDesc) so that cooperating workers can coordinate which
 * pages to read.
 *
 * btinitparallelscan：初始化共享内存中的并行扫描描述符（一个
 * BTParallelScanDesc），以便协作的工作进程能够协调读取哪些页面。
 */
extern void btinitparallelscan(void *target);

/*
 * btgettuple: fetch the next matching tuple in the given scan direction.
 * Advances the scan position, applying kill-tuple bookkeeping, and returns
 * whether another matching tuple was found.
 *
 * btgettuple：按给定扫描方向获取下一个匹配元组。它推进扫描位置，执行
 * kill-tuple 记账，并返回是否找到了另一个匹配元组。
 */
extern bool btgettuple(IndexScanDesc scan, ScanDirection dir);

/*
 * btgetbitmap: collect all tuples matching the scan keys into a TID bitmap in
 * one pass, returning the number of tuples added.  Used for bitmap index
 * scans.
 *
 * btgetbitmap：一次性将所有匹配扫描键的元组收集到一个 TID 位图中，返回
 * 添加的元组数量。用于位图索引扫描。
 */
extern int64 btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/*
 * btrescan: (re)start a btree scan with a new set of scan keys, resetting any
 * existing position and array-key state so the scan can run again.
 *
 * btrescan：以一组新的扫描键（重新）启动一次 btree 扫描，重置任何现有的
 * 位置和数组键状态，以便扫描能够再次运行。
 */
extern void btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);

/*
 * btparallelrescan: reset the shared-memory parallel scan descriptor so that
 * a parallel btree scan can be restarted by cooperating workers.
 *
 * btparallelrescan：重置共享内存中的并行扫描描述符，以便协作的工作进程
 * 能够重新启动一次并行 btree 扫描。
 */
extern void btparallelrescan(IndexScanDesc scan);

/*
 * btendscan: finish a btree index scan, releasing any pinned buffers and the
 * BTScanOpaqueData private state allocated by btbeginscan.
 *
 * btendscan：结束一次 btree 索引扫描，释放所有已 pin 住的缓冲区以及由
 * btbeginscan 分配的 BTScanOpaqueData 私有状态。
 */
extern void btendscan(IndexScanDesc scan);

/*
 * btmarkpos: remember the scan's current position so it can later be restored
 * with btrestrpos.
 *
 * btmarkpos：记住扫描的当前位置，以便之后可用 btrestrpos 恢复。
 */
extern void btmarkpos(IndexScanDesc scan);

/*
 * btrestrpos: restore the scan position previously saved by btmarkpos.
 *
 * btrestrpos：恢复先前由 btmarkpos 保存的扫描位置。
 */
extern void btrestrpos(IndexScanDesc scan);

/*
 * btbulkdelete: the VACUUM bulk-delete callback for btree.  Scans the whole
 * index, invoking the supplied callback to decide which index tuples to
 * remove, and returns updated statistics about the deletion.
 *
 * btbulkdelete：btree 的 VACUUM 批量删除回调。它扫描整个索引，调用提供的
 * 回调来决定删除哪些索引元组，并返回关于本次删除的更新统计信息。
 */
extern IndexBulkDeleteResult *btbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats,
										   IndexBulkDeleteCallback callback,
										   void *callback_state);

/*
 * btvacuumcleanup: the VACUUM cleanup callback for btree.  Performs a final
 * pass to recycle newly deleted pages and update the metapage's bookkeeping,
 * returning the final index statistics.
 *
 * btvacuumcleanup：btree 的 VACUUM 清理回调。它执行最后一遍处理以回收
 * 新删除的页并更新元页的记账信息，返回最终的索引统计信息。
 */
extern IndexBulkDeleteResult *btvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);

/*
 * btcanreturn: report whether the index can return the indexed value for the
 * given attribute (i.e. whether index-only scans are possible for it).
 *
 * btcanreturn：报告索引是否能够返回给定属性的被索引值（即对该属性而言
 * 是否可以进行 index-only 扫描）。
 */
extern bool btcanreturn(Relation index, int attno);

/*
 * btgettreeheight: return the current height of the btree, computed from the
 * fast root's level in the metapage.  Used by the planner for cost
 * estimation.
 *
 * btgettreeheight：返回 btree 的当前高度，根据元页中快速根的层级计算得出。
 * 供规划器用于代价估算。
 */
extern int	btgettreeheight(Relation rel);

/*
 * bttranslatestrategy: map a btree strategy number for the given operator
 * family to the corresponding CompareType, translating from the AM-specific
 * encoding to a generic comparison kind.
 *
 * bttranslatestrategy：将给定操作符族的 btree 策略号映射为相应的
 * CompareType，即从访问方法特有的编码转换为通用的比较类别。
 */
extern CompareType bttranslatestrategy(StrategyNumber strategy, Oid opfamily);

/*
 * bttranslatecmptype: the inverse of bttranslatestrategy, mapping a generic
 * CompareType back to the btree strategy number for the given operator
 * family.
 *
 * bttranslatecmptype：bttranslatestrategy 的逆操作，将通用的 CompareType
 * 映射回给定操作符族的 btree 策略号。
 */
extern StrategyNumber bttranslatecmptype(CompareType cmptype, Oid opfamily);

/*
 * prototypes for internal functions in nbtree.c
 */

/*
 * nbtree.c 中内部函数的原型声明
 */

/*
 * _bt_parallel_seize: claim the next page (or work item) for the calling
 * worker in a parallel scan.  Coordinates via the shared descriptor and
 * returns whether the caller obtained a page to process.
 *
 * _bt_parallel_seize：在并行扫描中为调用的工作进程认领下一个页（或工作
 * 项）。它通过共享描述符进行协调，并返回调用者是否获得了要处理的页。
 */
extern bool _bt_parallel_seize(IndexScanDesc scan, BlockNumber *next_scan_page,
							   BlockNumber *last_curr_page, bool first);

/*
 * _bt_parallel_release: publish the next page to be scanned so that other
 * parallel workers can proceed, releasing the coordination lock held after a
 * seize.
 *
 * _bt_parallel_release：发布下一个将被扫描的页，以便其他并行工作进程能够
 * 继续，并释放认领之后所持有的协调锁。
 */
extern void _bt_parallel_release(IndexScanDesc scan,
								 BlockNumber next_scan_page,
								 BlockNumber curr_page);

/*
 * _bt_parallel_done: mark the parallel scan as finished for the calling
 * worker and wake any peers waiting on the shared descriptor.
 *
 * _bt_parallel_done：为调用的工作进程将并行扫描标记为已完成，并唤醒任何
 * 在共享描述符上等待的对等进程。
 */
extern void _bt_parallel_done(IndexScanDesc scan);

/*
 * _bt_parallel_primscan_schedule: schedule the start of a new primitive scan
 * within a parallel scan, recording the current page so other workers know
 * where the next primitive scan should begin.
 *
 * _bt_parallel_primscan_schedule：在并行扫描内部安排一次新原始扫描的开始，
 * 记录当前页以便其他工作进程知道下一次原始扫描应从何处开始。
 */
extern void _bt_parallel_primscan_schedule(IndexScanDesc scan,
										   BlockNumber curr_page);

/*
 * prototypes for functions in nbtdedup.c
 */

/*
 * nbtdedup.c 中函数的原型声明
 */

/*
 * _bt_dedup_pass: perform one deduplication pass over a leaf page, merging
 * groups of equal tuples into posting-list tuples to reclaim space (typically
 * to avoid an imminent page split).
 *
 * _bt_dedup_pass：对一个叶子页执行一次去重遍历，将相等元组组合并为倒排
 * 列表元组以回收空间（通常是为了避免一次迫在眉睫的页分裂）。
 */
extern void _bt_dedup_pass(Relation rel, Buffer buf, IndexTuple newitem,
						   Size newitemsz, bool bottomupdedup);

/*
 * _bt_bottomupdel_pass: attempt bottom-up index deletion on a leaf page,
 * consulting the heap to physically remove index tuples pointing to
 * dead-to-all rows, returning whether enough space was freed.
 *
 * _bt_bottomupdel_pass：尝试对一个叶子页进行自底向上的索引删除，通过查询
 * 堆来物理移除指向对所有事务均已死亡行的索引元组，返回是否释放了足够的
 * 空间。
 */
extern bool _bt_bottomupdel_pass(Relation rel, Buffer buf, Relation heapRel,
								 Size newitemsz);

/*
 * _bt_dedup_start_pending: begin a new pending posting list in the dedup
 * state, using the given base tuple (at baseoff) as the first member of the
 * group being merged.
 *
 * _bt_dedup_start_pending：在去重状态中开始一个新的待处理倒排列表，以给定
 * 的基元组（位于 baseoff 处）作为被合并组的第一个成员。
 */
extern void _bt_dedup_start_pending(BTDedupState state, IndexTuple base,
									OffsetNumber baseoff);
/*
 * _bt_dedup_save_htid: try to add the given tuple's heap TIDs to the current
 * pending posting list, returning whether they fit within the size limit.
 *
 * _bt_dedup_save_htid：尝试将给定元组的堆 TID 添加到当前待处理倒排列表中，
 * 返回它们是否在大小限制内能够容纳。
 */
extern bool _bt_dedup_save_htid(BTDedupState state, IndexTuple itup);

/*
 * _bt_dedup_finish_pending: emit the accumulated pending posting list onto
 * the new page image, returning the space (in bytes) that this group
 * occupies.
 *
 * _bt_dedup_finish_pending：将累积的待处理倒排列表写入新的页镜像，返回该组
 * 所占用的空间（以字节计）。
 */
extern Size _bt_dedup_finish_pending(Page newpage, BTDedupState state);

/*
 * _bt_form_posting: build a new posting-list tuple from a base tuple and an
 * array of heap TIDs, producing the compact deduplicated representation.
 *
 * _bt_form_posting：根据一个基元组和一个堆 TID 数组构建一个新的倒排列表
 * 元组，生成紧凑的去重表示。
 */
extern IndexTuple _bt_form_posting(IndexTuple base, ItemPointer htids,
								   int nhtids);

/*
 * _bt_update_posting: given VACUUM posting state describing which TIDs to
 * remove, build the final smaller posting-list (or plain) tuple that will
 * replace the original on the page.
 *
 * _bt_update_posting：给定描述要移除哪些 TID 的 VACUUM 倒排状态，构建最终
 * 更小的倒排列表（或普通）元组，用以替换页面上的原始元组。
 */
extern void _bt_update_posting(BTVacuumPosting vacposting);

/*
 * _bt_swap_posting: handle a posting-list split during insertion by swapping
 * a new item into an existing posting list, returning the rebuilt posting
 * tuple with the item inserted at postingoff.
 *
 * _bt_swap_posting：在插入期间处理倒排列表分裂，将一个新项换入现有的倒排
 * 列表中，返回在 postingoff 处插入该项后重建的倒排元组。
 */
extern IndexTuple _bt_swap_posting(IndexTuple newitem, IndexTuple oposting,
								   int postingoff);

/*
 * prototypes for functions in nbtinsert.c
 */

/*
 * nbtinsert.c 中函数的原型声明
 */

/*
 * _bt_doinsert: the core routine that inserts a single index tuple.  Descends
 * the tree to the correct leaf page, optionally performs a uniqueness check,
 * and inserts the tuple, splitting the page if necessary.
 *
 * _bt_doinsert：插入单个索引元组的核心例程。它向下遍历树到达正确的叶子页，
 * 可选地执行唯一性检查，并插入该元组，必要时分裂页面。
 */
extern bool _bt_doinsert(Relation rel, IndexTuple itup,
						 IndexUniqueCheck checkUnique, bool indexUnchanged,
						 Relation heapRel);

/*
 * _bt_finish_split: complete an incomplete page split by inserting the
 * missing downlink for the right sibling into the parent, using the saved
 * stack to locate the parent page.
 *
 * _bt_finish_split：通过将右兄弟页缺失的 downlink 插入父页来完成一次未完成
 * 的页分裂，使用保存的栈来定位父页。
 */
extern void _bt_finish_split(Relation rel, Relation heaprel, Buffer lbuf,
							 BTStack stack);

/*
 * _bt_getstackbuf: walk the parent page (via the stack) to find and lock the
 * buffer holding the downlink to the given child block, coping with
 * concurrent splits by moving right as needed.
 *
 * _bt_getstackbuf：沿父页（借助栈）查找并锁定持有指向给定子块 downlink 的
 * 缓冲区，并按需向右移动以应对并发分裂。
 */
extern Buffer _bt_getstackbuf(Relation rel, Relation heaprel, BTStack stack,
							  BlockNumber child);

/*
 * prototypes for functions in nbtsplitloc.c
 */

/*
 * nbtsplitloc.c 中函数的原型声明
 */

/*
 * _bt_findsplitloc: choose the optimal split point for a page that is about
 * to overflow.  Weighs fillfactor and suffix-truncation considerations, and
 * reports through newitemonleft which side the incoming item belongs on.
 *
 * _bt_findsplitloc：为即将溢出的页选择最优的分裂点。它权衡填充因子和后缀
 * 截断方面的考虑，并通过 newitemonleft 报告新到来的项应属于哪一侧。
 */
extern OffsetNumber _bt_findsplitloc(Relation rel, Page origpage,
									 OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
									 bool *newitemonleft);

/*
 * prototypes for functions in nbtpage.c
 */

/*
 * nbtpage.c 中函数的原型声明
 */

/*
 * _bt_initmetapage: format an in-memory page image as a btree metapage that
 * points at the given root block and level, recording whether the index is
 * all-equalimage (deduplication-safe).
 *
 * _bt_initmetapage：将一个内存中的页镜像格式化为指向给定根块和层级的 btree
 * 元页，并记录该索引是否为 all-equalimage（可安全去重）。
 */
extern void _bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level,
							 bool allequalimage);

/*
 * _bt_vacuum_needs_cleanup: decide whether a btree needs a cleanup-only
 * VACUUM pass based on metapage bookkeeping about deleted pages.
 *
 * _bt_vacuum_needs_cleanup：根据元页中关于被删除页的记账信息，判断某个
 * btree 是否需要一次仅清理的 VACUUM 遍历。
 */
extern bool _bt_vacuum_needs_cleanup(Relation rel);

/*
 * _bt_set_cleanup_info: update the metapage's record of how many deleted but
 * not-yet-recyclable pages remain, so future VACUUMs can decide on cleanup.
 *
 * _bt_set_cleanup_info：更新元页中关于还剩多少已删除但尚不可回收页的记录，
 * 以便未来的 VACUUM 能据此决定是否清理。
 */
extern void _bt_set_cleanup_info(Relation rel, BlockNumber num_delpages);

/*
 * _bt_upgrademetapage: upgrade an old-version metapage in place to the
 * current metapage format, populating fields added in newer versions.
 *
 * _bt_upgrademetapage：就地将旧版本的元页升级为当前的元页格式，填充在更新
 * 版本中新增的字段。
 */
extern void _bt_upgrademetapage(Page page);

/*
 * _bt_getroot: locate and lock the current fast root page of the index for
 * the given access mode, creating a new root if the index is still empty.
 *
 * _bt_getroot：为给定的访问模式定位并锁定索引当前的快速根页，如果索引仍为
 * 空则创建一个新根。
 */
extern Buffer _bt_getroot(Relation rel, Relation heaprel, int access);

/*
 * _bt_gettrueroot: locate and lock the true (topmost) root of the index,
 * bypassing the fast-root shortcut; used where the real tree top is required.
 *
 * _bt_gettrueroot：定位并锁定索引真正的（最顶层的）根，绕过快速根这一
 * 捷径；用于需要真实树顶的场合。
 */
extern Buffer _bt_gettrueroot(Relation rel);

/*
 * _bt_getrootheight: return the height of the tree measured from the fast
 * root, reading the metapage without locking the root itself.
 *
 * _bt_getrootheight：返回从快速根量得的树的高度，读取元页而不锁定根本身。
 */
extern int	_bt_getrootheight(Relation rel);

/*
 * _bt_metaversion: read the metapage and report the index's heapkeyspace and
 * allequalimage properties, which govern key semantics and deduplication.
 *
 * _bt_metaversion：读取元页并报告索引的 heapkeyspace 与 allequalimage 属性，
 * 它们决定了键语义和是否可去重。
 */
extern void _bt_metaversion(Relation rel, bool *heapkeyspace,
							bool *allequalimage);

/*
 * _bt_checkpage: sanity-check a freshly read btree page (special size, not
 * new/corrupt) before it is used, raising an error on inconsistency.
 *
 * _bt_checkpage：在使用一个刚读入的 btree 页之前对其进行合理性检查（特殊
 * 空间大小、非新页/未损坏），若发现不一致则报错。
 */
extern void _bt_checkpage(Relation rel, Buffer buf);

/*
 * _bt_getbuf: read and lock the existing block blkno of the index in the
 * requested access mode, returning the pinned buffer.
 *
 * _bt_getbuf：以请求的访问模式读取并锁定索引中已有的块 blkno，返回已 pin
 * 住的缓冲区。
 */
extern Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access);

/*
 * _bt_allocbuf: obtain a new writable btree page, either by recycling a
 * previously deleted page that is now safe to reuse or by extending the
 * relation, returning it locked.
 *
 * _bt_allocbuf：获取一个新的可写 btree 页，方式为回收一个此前删除且现在
 * 可安全重用的页，或者扩展关系，返回已锁定的该页。
 */
extern Buffer _bt_allocbuf(Relation rel, Relation heaprel);

/*
 * _bt_relandgetbuf: atomically release the currently held buffer obuf and
 * acquire/lock block blkno, saving a lock-release/acquire cycle when walking
 * between pages.
 *
 * _bt_relandgetbuf：原子地释放当前持有的缓冲区 obuf 并获取/锁定块 blkno，
 * 在页面之间移动时省去一次解锁/加锁的循环。
 */
extern Buffer _bt_relandgetbuf(Relation rel, Buffer obuf,
							   BlockNumber blkno, int access);

/*
 * _bt_relbuf: unlock and unpin the given btree buffer.
 *
 * _bt_relbuf：解锁并 unpin 给定的 btree 缓冲区。
 */
extern void _bt_relbuf(Relation rel, Buffer buf);

/*
 * _bt_lockbuf: acquire the requested buffer lock on a btree page, with the
 * appropriate lock accounting and instrumentation.
 *
 * _bt_lockbuf：在一个 btree 页上获取请求的缓冲区锁，并进行相应的锁记账与
 * 监测。
 */
extern void _bt_lockbuf(Relation rel, Buffer buf, int access);

/*
 * _bt_unlockbuf: release the buffer lock previously taken on a btree page
 * (while keeping the pin).
 *
 * _bt_unlockbuf：释放此前在某个 btree 页上获取的缓冲区锁（同时保留 pin）。
 */
extern void _bt_unlockbuf(Relation rel, Buffer buf);

/*
 * _bt_conditionallockbuf: try to acquire the buffer lock without blocking,
 * returning whether the lock was obtained.
 *
 * _bt_conditionallockbuf：尝试在不阻塞的情况下获取缓冲区锁，返回是否成功
 * 获得该锁。
 */
extern bool _bt_conditionallockbuf(Relation rel, Buffer buf);

/*
 * _bt_upgradelockbufcleanup: upgrade an already-held buffer lock to a
 * cleanup lock, waiting until no other backend pins the page.
 *
 * _bt_upgradelockbufcleanup：将已持有的缓冲区锁升级为清理锁，等待直到没有
 * 其他后端 pin 住该页。
 */
extern void _bt_upgradelockbufcleanup(Relation rel, Buffer buf);

/*
 * _bt_pageinit: initialize a raw page as an empty btree page of the given
 * size, laying out the header and special area.
 *
 * _bt_pageinit：将一个原始页初始化为给定大小的空 btree 页，布置页头和特殊
 * 区域。
 */
extern void _bt_pageinit(Page page, Size size);

/*
 * _bt_delitems_vacuum: physically delete and/or update the specified index
 * tuples on a leaf page on behalf of VACUUM, WAL-logging the change.
 *
 * _bt_delitems_vacuum：代表 VACUUM 在一个叶子页上物理删除和/或更新指定的
 * 索引元组，并对该更改进行 WAL 记录。
 */
extern void _bt_delitems_vacuum(Relation rel, Buffer buf,
								OffsetNumber *deletable, int ndeletable,
								BTVacuumPosting *updatable, int nupdatable);

/*
 * _bt_delitems_delete_check: perform ad-hoc index tuple deletion during
 * inserts, consulting the heap via delstate to confirm which TIDs are safe to
 * remove before deleting them.
 *
 * _bt_delitems_delete_check：在插入期间执行临时的索引元组删除，通过 delstate
 * 查询堆以确认哪些 TID 可安全移除，然后再删除它们。
 */
extern void _bt_delitems_delete_check(Relation rel, Buffer buf,
									  Relation heapRel,
									  TM_IndexDeleteOp *delstate);

/*
 * _bt_pagedel: delete an empty (half-dead) leaf page and any resulting empty
 * parents from the tree, using VACUUM state to track newly deleted pages.
 *
 * _bt_pagedel：从树中删除一个空的（半死的）叶子页以及由此产生的任何空父
 * 页，使用 VACUUM 状态来跟踪新删除的页。
 */
extern void _bt_pagedel(Relation rel, Buffer leafbuf, BTVacState *vstate);

/*
 * _bt_pendingfsm_init: initialize the VACUUM state's array that tracks
 * newly-deleted pages awaiting recording in the free space map.
 *
 * _bt_pendingfsm_init：初始化 VACUUM 状态中用于跟踪等待记入空闲空间映射的
 * 新删除页的数组。
 */
extern void _bt_pendingfsm_init(Relation rel, BTVacState *vstate,
								bool cleanuponly);

/*
 * _bt_pendingfsm_finalize: at the end of VACUUM, record now-recyclable
 * deleted pages into the free space map, mirroring BTPageIsRecyclable's
 * policy.
 *
 * _bt_pendingfsm_finalize：在 VACUUM 结束时，将现在可回收的已删除页记入
 * 空闲空间映射，其策略与 BTPageIsRecyclable 保持一致。
 */
extern void _bt_pendingfsm_finalize(Relation rel, BTVacState *vstate);

/*
 * prototypes for functions in nbtpreprocesskeys.c
 */

/*
 * nbtpreprocesskeys.c 中函数的原型声明
 */

/*
 * _bt_preprocess_keys: transform the raw scan keys of an index scan into the
 * canonical, redundancy-eliminated form used during the scan, detecting
 * unsatisfiable qualifications along the way.
 *
 * _bt_preprocess_keys：将一次索引扫描的原始扫描键转换为扫描期间使用的
 * 规范化、消除冗余后的形式，并在此过程中检测无法满足的条件。
 */
extern void _bt_preprocess_keys(IndexScanDesc scan);

/*
 * prototypes for functions in nbtsearch.c
 */

/*
 * nbtsearch.c 中函数的原型声明
 */

/*
 * _bt_search: descend the tree from the root to the leaf page where the given
 * insertion scankey belongs, returning the leaf buffer and a BTStack
 * recording the path of parent downlinks followed.
 *
 * _bt_search：从根向下遍历树，到达给定插入扫描键所属的叶子页，返回该叶子
 * 缓冲区以及一个记录所经过父节点 downlink 路径的 BTStack。
 */
extern BTStack _bt_search(Relation rel, Relation heaprel, BTScanInsert key,
						  Buffer *bufP, int access);

/*
 * _bt_binsrch_insert: binary-search a leaf page for the exact insertion point
 * of the tuple described by insertstate, caching bounds to speed up
 * uniqueness checking.
 *
 * _bt_binsrch_insert：在一个叶子页上进行二分查找，找到 insertstate 所描述
 * 元组的确切插入点，并缓存边界以加速唯一性检查。
 */
extern OffsetNumber _bt_binsrch_insert(Relation rel, BTInsertState insertstate);

/*
 * _bt_compare: compare an insertion scankey against the tuple at the given
 * offset on a page, returning <0, 0, or >0 to indicate their key ordering.
 *
 * _bt_compare：将一个插入扫描键与页面上给定偏移处的元组进行比较，返回
 * <0、0 或 >0 以指示它们的键顺序。
 */
extern int32 _bt_compare(Relation rel, BTScanInsert key, Page page, OffsetNumber offnum);

/*
 * _bt_first: position a new index scan on its first matching tuple, using the
 * scan keys to descend to a starting leaf page, and return whether any match
 * was found.
 *
 * _bt_first：将一次新的索引扫描定位到其第一个匹配元组上，利用扫描键向下
 * 遍历到起始叶子页，并返回是否找到任何匹配。
 */
extern bool _bt_first(IndexScanDesc scan, ScanDirection dir);

/*
 * _bt_next: advance an already-positioned scan to the next matching tuple,
 * stepping to sibling pages as needed, and return whether one was found.
 *
 * _bt_next：将一次已定位的扫描推进到下一个匹配元组，按需跨到兄弟页，并
 * 返回是否找到。
 */
extern bool _bt_next(IndexScanDesc scan, ScanDirection dir);

/*
 * _bt_get_endpoint: locate and pin the leftmost or rightmost page at the
 * given tree level, used to start scans that begin at one end of the index.
 *
 * _bt_get_endpoint：定位并 pin 住给定树层级上最左或最右的页，用于启动从
 * 索引某一端开始的扫描。
 */
extern Buffer _bt_get_endpoint(Relation rel, uint32 level, bool rightmost);

/*
 * prototypes for functions in nbtutils.c
 */

/*
 * nbtutils.c 中函数的原型声明
 */

/*
 * _bt_mkscankey: build an insertion scankey (BTScanInsert) from an index
 * tuple, extracting each key attribute and the heap-TID tiebreaker so the
 * tree can be searched for that tuple's position.
 *
 * _bt_mkscankey：根据一个索引元组构建一个插入扫描键（BTScanInsert），提取
 * 每个键属性以及作为决胜键的堆 TID，以便在树中搜索该元组的位置。
 */
extern BTScanInsert _bt_mkscankey(Relation rel, IndexTuple itup);

/*
 * _bt_freestack: free a BTStack chain returned by _bt_search once the caller
 * no longer needs the recorded descent path.
 *
 * _bt_freestack：一旦调用者不再需要所记录的下降路径，就释放由 _bt_search
 * 返回的 BTStack 链。
 */
extern void _bt_freestack(BTStack stack);

/*
 * _bt_start_prim_scan: begin the next primitive index scan when array keys
 * require multiple descents, advancing the array key state and reporting
 * whether another primitive scan is needed.
 *
 * _bt_start_prim_scan：当数组键需要多次下降时，开始下一次原始索引扫描，
 * 推进数组键状态并报告是否还需要另一次原始扫描。
 */
extern bool _bt_start_prim_scan(IndexScanDesc scan, ScanDirection dir);

/*
 * _bt_binsrch_array_skey: binary-search within an array scan key's element
 * list for the element matching the tuple datum, reporting the comparison
 * result and used to advance array keys during a scan.
 *
 * _bt_binsrch_array_skey：在某个数组扫描键的元素列表内二分查找与元组 datum
 * 匹配的元素，报告比较结果，用于在扫描期间推进数组键。
 */
extern int	_bt_binsrch_array_skey(FmgrInfo *orderproc,
								   bool cur_elem_trig, ScanDirection dir,
								   Datum tupdatum, bool tupnull,
								   BTArrayKeyInfo *array, ScanKey cur,
								   int32 *set_elem_result);

/*
 * _bt_start_array_keys: reset all array scan keys to their first element in
 * the current scan direction, preparing for the initial primitive scan.
 *
 * _bt_start_array_keys：将所有数组扫描键按当前扫描方向重置到它们的第一个
 * 元素，为初次原始扫描做准备。
 */
extern void _bt_start_array_keys(IndexScanDesc scan, ScanDirection dir);

/*
 * _bt_checkkeys: test a single index tuple against the scan keys, deciding
 * whether it qualifies and whether the scan should continue; also drives
 * array-key advancement and look-ahead via pstate.
 *
 * _bt_checkkeys：将单个索引元组与扫描键进行检验，判断它是否符合条件以及
 * 扫描是否应继续；同时通过 pstate 驱动数组键推进和向前看。
 */
extern bool _bt_checkkeys(IndexScanDesc scan, BTReadPageState *pstate, bool arrayKeys,
						  IndexTuple tuple, int tupnatts);

/*
 * _bt_scanbehind_checkkeys: check, using a page's final tuple, whether an
 * array-key scan has fallen behind and needs a new primitive scan before
 * advancing further.
 *
 * _bt_scanbehind_checkkeys：借助一个页的最后一个元组，检查数组键扫描是否
 * 已落后并在继续推进之前需要一次新的原始扫描。
 */
extern bool _bt_scanbehind_checkkeys(IndexScanDesc scan, ScanDirection dir,
									 IndexTuple finaltup);

/*
 * _bt_set_startikey: determine the first scan key from which per-tuple
 * comparisons must begin on the current page, an optimization that skips keys
 * already known to be satisfied.
 *
 * _bt_set_startikey：确定在当前页上每元组比较必须从哪个扫描键开始，这是一
 * 项跳过已知已满足键的优化。
 */
extern void _bt_set_startikey(IndexScanDesc scan, BTReadPageState *pstate);

/*
 * _bt_killitems: mark the index tuples recorded as known-dead during a scan
 * with LP_DEAD hint bits on their leaf page, so future scans and VACUUM can
 * skip or reclaim them.
 *
 * _bt_killitems：将扫描期间记录为已知死亡的索引元组在其叶子页上标记
 * LP_DEAD 提示位，以便未来的扫描和 VACUUM 能跳过或回收它们。
 */
extern void _bt_killitems(IndexScanDesc scan);

/*
 * _bt_vacuum_cycleid: return the current vacuum cycle ID for the index, used
 * to stamp pages split during a VACUUM run.
 *
 * _bt_vacuum_cycleid：返回索引当前的 vacuum 周期 ID，用于给在一次 VACUUM
 * 运行期间分裂的页打上标记。
 */
extern BTCycleId _bt_vacuum_cycleid(Relation rel);

/*
 * _bt_start_vacuum: register the start of a VACUUM on the index in shared
 * memory and return a freshly assigned vacuum cycle ID.
 *
 * _bt_start_vacuum：在共享内存中登记索引上一次 VACUUM 的开始，并返回一个
 * 新分配的 vacuum 周期 ID。
 */
extern BTCycleId _bt_start_vacuum(Relation rel);

/*
 * _bt_end_vacuum: clear the shared-memory record of an in-progress VACUUM on
 * the index once it completes.
 *
 * _bt_end_vacuum：当索引上正在进行的 VACUUM 完成后，清除其共享内存记录。
 */
extern void _bt_end_vacuum(Relation rel);

/*
 * _bt_end_vacuum_callback: on-error cleanup callback wrapper that calls
 * _bt_end_vacuum so an aborted VACUUM does not leave stale shared state.
 *
 * _bt_end_vacuum_callback：出错时的清理回调包装器，它调用 _bt_end_vacuum，
 * 使得被中止的 VACUUM 不会遗留陈旧的共享状态。
 */
extern void _bt_end_vacuum_callback(int code, Datum arg);

/*
 * BTreeShmemSize: report the amount of shared memory the btree subsystem
 * needs for its VACUUM cycle-ID tracking area.
 *
 * BTreeShmemSize：报告 btree 子系统为其 VACUUM 周期 ID 跟踪区所需的共享
 * 内存大小。
 */
extern Size BTreeShmemSize(void);

/*
 * BTreeShmemInit: allocate and initialize the btree subsystem's shared memory
 * during postmaster startup.
 *
 * BTreeShmemInit：在 postmaster 启动期间分配并初始化 btree 子系统的共享
 * 内存。
 */
extern void BTreeShmemInit(void);

/*
 * btoptions: parse and validate the reloptions for a btree index, returning
 * the filled-in BTOptions bytea used to configure fillfactor and
 * deduplication.
 *
 * btoptions：解析并校验 btree 索引的 reloptions，返回填充好的 BTOptions
 * bytea，用于配置填充因子和去重。
 */
extern bytea *btoptions(Datum reloptions, bool validate);

/*
 * btproperty: answer AM property inquiries (e.g. whether ordering or
 * distance-ordering is supported) for a btree index, returning whether the
 * property was handled here.
 *
 * btproperty：回答关于 btree 索引的访问方法属性询问（例如是否支持排序或
 * 按距离排序），返回该属性是否在此被处理。
 */
extern bool btproperty(Oid index_oid, int attno,
					   IndexAMProperty prop, const char *propname,
					   bool *res, bool *isnull);

/*
 * btbuildphasename: return the human-readable name of an index-build progress
 * phase given its number, for progress reporting views.
 *
 * btbuildphasename：给定编号，返回某个索引构建进度阶段的可读名称，供进度
 * 报告视图使用。
 */
extern char *btbuildphasename(int64 phasenum);

/*
 * _bt_truncate: form a new pivot tuple by suffix-truncating the separator key
 * between the last-left and first-right tuples of a split, keeping only as
 * many attributes as needed to distinguish the two pages.
 *
 * _bt_truncate：通过对一次分裂中 last-left 与 first-right 元组之间的分隔键
 * 进行后缀截断来构造一个新的 pivot 元组，只保留区分这两个页所需的那么多
 * 属性。
 */
extern IndexTuple _bt_truncate(Relation rel, IndexTuple lastleft,
							   IndexTuple firstright, BTScanInsert itup_key);

/*
 * _bt_keep_natts_fast: quickly compute how many leading key attributes two
 * tuples share, using equal-image opclass semantics, to guide suffix
 * truncation.
 *
 * _bt_keep_natts_fast：使用 equal-image 操作符类语义快速计算两个元组共享
 * 多少个前导键属性，用以指导后缀截断。
 */
extern int	_bt_keep_natts_fast(Relation rel, IndexTuple lastleft,
								IndexTuple firstright);

/*
 * _bt_check_natts: verify that the tuple at the given offset has the expected
 * number of attributes for its page and index version, used by amcheck-style
 * validation.
 *
 * _bt_check_natts：验证给定偏移处的元组对于其页面和索引版本而言具有预期的
 * 属性数量，供 amcheck 风格的校验使用。
 */
extern bool _bt_check_natts(Relation rel, bool heapkeyspace, Page page,
							OffsetNumber offnum);

/*
 * _bt_check_third_page: raise an error when a tuple is too large to fit the
 * "three items per page" rule, reporting the offending index and tuple.
 *
 * _bt_check_third_page：当某个元组过大以致违反“每页三项”规则时报错，报告
 * 出问题的索引和元组。
 */
extern void _bt_check_third_page(Relation rel, Relation heap,
								 bool needheaptidspace, Page page, IndexTuple newtup);

/*
 * _bt_allequalimage: determine whether every indexed column uses an
 * equal-image opclass, meaning deduplication is safe, optionally emitting a
 * debug message.
 *
 * _bt_allequalimage：判断是否每个被索引列都使用 equal-image 操作符类（这
 * 意味着去重是安全的），并可选地输出一条调试信息。
 */
extern bool _bt_allequalimage(Relation rel, bool debugmessage);

/*
 * prototypes for functions in nbtvalidate.c
 */

/*
 * nbtvalidate.c 中函数的原型声明
 */

/*
 * btvalidate: validate the definition of a btree operator class, checking
 * that it provides the required support procedures and a consistent set of
 * operators.
 *
 * btvalidate：校验一个 btree 操作符类的定义，检查它是否提供了必需的支持
 * 过程以及一组一致的操作符。
 */
extern bool btvalidate(Oid opclassoid);

/*
 * btadjustmembers: adjust dependency links for the operators and support
 * functions of a btree operator family as it is being modified.
 *
 * btadjustmembers：在某个 btree 操作符族被修改时，调整其操作符和支持函数
 * 的依赖链接。
 */
extern void btadjustmembers(Oid opfamilyoid,
							Oid opclassoid,
							List *operators,
							List *functions);

/*
 * prototypes for functions in nbtsort.c
 */

/*
 * nbtsort.c 中函数的原型声明
 */

/*
 * btbuild: build a btree index from scratch over an existing heap by sorting
 * all index tuples (possibly in parallel) and loading them into leaf pages,
 * returning build statistics.
 *
 * btbuild：通过对所有索引元组进行排序（可能是并行的）并将它们装入叶子页，
 * 在一个已有堆之上从头构建一个 btree 索引，返回构建统计信息。
 */
extern IndexBuildResult *btbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);

/*
 * _bt_parallel_build_main: the entry point run by each parallel worker during
 * a parallel index build; it joins the shared sort and contributes tuples to
 * the build.
 *
 * _bt_parallel_build_main：并行索引构建期间每个并行工作进程所运行的入口点；
 * 它加入共享排序并向构建贡献元组。
 */
extern void _bt_parallel_build_main(dsm_segment *seg, shm_toc *toc);

#endif							/* NBTREE_H */
