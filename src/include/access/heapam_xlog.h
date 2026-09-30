/*-------------------------------------------------------------------------
 *
 * heapam_xlog.h
 *	  POSTGRES heap access XLOG definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam_xlog.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
 */
#ifndef HEAPAM_XLOG_H
#define HEAPAM_XLOG_H

#include "access/htup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"
#include "utils/relcache.h"


/*
 * WAL record definitions for heapam.c's WAL operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field.  We use 3 for opcode and one for init bit.
 *
 * 中文翻译：
 * heapam.c 的 WAL 操作的 WAL 记录定义 XLOG 允
 * 许在日志记录 xl_info 字段的高 4 位中存储一些信息。我们使
 * 用 3 作为操作码，使用 1 作为初始化位。
 */
#define XLOG_HEAP_INSERT		0x00
#define XLOG_HEAP_DELETE		0x10
#define XLOG_HEAP_UPDATE		0x20
#define XLOG_HEAP_TRUNCATE		0x30
#define XLOG_HEAP_HOT_UPDATE	0x40
#define XLOG_HEAP_CONFIRM		0x50
#define XLOG_HEAP_LOCK			0x60
#define XLOG_HEAP_INPLACE		0x70

#define XLOG_HEAP_OPMASK		0x70
/*
 * When we insert 1st item on new page in INSERT, UPDATE, HOT_UPDATE,
 * or MULTI_INSERT, we can (and we do) restore entire page in redo
 *
 * 中文翻译：
 * 当我们在 INSERT、UPDATE、HOT_UPDATE 或 MU
 * LTI_INSERT 中插入新页面上的第一项时，我们可以（而且我们确
 * 实）在重做中恢复整个页面
 */
#define XLOG_HEAP_INIT_PAGE		0x80
/*
 * We ran out of opcodes, so heapam.c now has a second RmgrId.  These opcodes
 * are associated with RM_HEAP2_ID, but are not logically different from
 * the ones above associated with RM_HEAP_ID.  XLOG_HEAP_OPMASK applies to
 * these, too.
 *
 * There's no difference between XLOG_HEAP2_PRUNE_ON_ACCESS,
 * XLOG_HEAP2_PRUNE_VACUUM_SCAN and XLOG_HEAP2_PRUNE_VACUUM_CLEANUP records.
 * They have separate opcodes just for debugging and analysis purposes, to
 * indicate why the WAL record was emitted.
 *
 * 中文翻译：
 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
 */
#define XLOG_HEAP2_REWRITE		0x00
#define XLOG_HEAP2_PRUNE_ON_ACCESS		0x10
#define XLOG_HEAP2_PRUNE_VACUUM_SCAN	0x20
#define XLOG_HEAP2_PRUNE_VACUUM_CLEANUP	0x30
#define XLOG_HEAP2_VISIBLE		0x40
#define XLOG_HEAP2_MULTI_INSERT 0x50
#define XLOG_HEAP2_LOCK_UPDATED 0x60
#define XLOG_HEAP2_NEW_CID		0x70

/*
 * xl_heap_insert/xl_heap_multi_insert flag values, 8 bits are available.
 *
 * 中文翻译：
 * xl_heap_insert/xl_heap_multi_inser
 * t标志值，8位可用。
 */
/* PD_ALL_VISIBLE was cleared */

/* 中文翻译：PD_ALL_VISIBLE 已清除 */
#define XLH_INSERT_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_INSERT_LAST_IN_MULTI				(1<<1)
#define XLH_INSERT_IS_SPECULATIVE				(1<<2)
#define XLH_INSERT_CONTAINS_NEW_TUPLE			(1<<3)
#define XLH_INSERT_ON_TOAST_RELATION			(1<<4)

/* all_frozen_set always implies all_visible_set */

/* 中文翻译：all_frozen_set 始终暗示 all_visible_set */
#define XLH_INSERT_ALL_FROZEN_SET				(1<<5)

/*
 * xl_heap_update flag values, 8 bits are available.
 *
 * 中文翻译：
 * xl_heap_update标志值，8位可用。
 */
/* PD_ALL_VISIBLE was cleared */

/* 中文翻译：PD_ALL_VISIBLE 已清除 */
#define XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED		(1<<0)
/* PD_ALL_VISIBLE was cleared in the 2nd page */

/* 中文翻译：PD_ALL_VISIBLE 在第 2 页被清除 */
#define XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED		(1<<1)
#define XLH_UPDATE_CONTAINS_OLD_TUPLE			(1<<2)
#define XLH_UPDATE_CONTAINS_OLD_KEY				(1<<3)
#define XLH_UPDATE_CONTAINS_NEW_TUPLE			(1<<4)
#define XLH_UPDATE_PREFIX_FROM_OLD				(1<<5)
#define XLH_UPDATE_SUFFIX_FROM_OLD				(1<<6)

/* convenience macro for checking whether any form of old tuple was logged */

/* 中文翻译：用于检查是否记录了任何形式的旧元组的便捷宏 */
#define XLH_UPDATE_CONTAINS_OLD						\
	(XLH_UPDATE_CONTAINS_OLD_TUPLE | XLH_UPDATE_CONTAINS_OLD_KEY)

/*
 * xl_heap_delete flag values, 8 bits are available.
 *
 * 中文翻译：
 * xl_heap_delete标志值，8位可用。
 */
/* PD_ALL_VISIBLE was cleared */

/* 中文翻译：PD_ALL_VISIBLE 已清除 */
#define XLH_DELETE_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_DELETE_CONTAINS_OLD_TUPLE			(1<<1)
#define XLH_DELETE_CONTAINS_OLD_KEY				(1<<2)
#define XLH_DELETE_IS_SUPER						(1<<3)
#define XLH_DELETE_IS_PARTITION_MOVE			(1<<4)

/* convenience macro for checking whether any form of old tuple was logged */

/* 中文翻译：用于检查是否记录了任何形式的旧元组的便捷宏 */
#define XLH_DELETE_CONTAINS_OLD						\
	(XLH_DELETE_CONTAINS_OLD_TUPLE | XLH_DELETE_CONTAINS_OLD_KEY)

/* This is what we need to know about delete */

/* 中文翻译：这是我们需要了解的关于删除的知识 */
typedef struct xl_heap_delete
{
	TransactionId xmax;			/* xmax of the deleted tuple */

	/* 中文翻译：已删除元组的 xmax */
	OffsetNumber offnum;		/* deleted tuple's offset */

	/* 中文翻译：已删除元组的偏移量 */
	uint8		infobits_set;	/* infomask bits */

	/* 中文翻译：信息掩码位 */
	uint8		flags;
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, flags) + sizeof(uint8))

/*
 * xl_heap_truncate flag values, 8 bits are available.
 *
 * 中文翻译：
 * xl_heap_truncate 标志值，8 位可用。
 */
#define XLH_TRUNCATE_CASCADE					(1<<0)
#define XLH_TRUNCATE_RESTART_SEQS				(1<<1)

/*
 * For truncate we list all truncated relids in an array, followed by all
 * sequence relids that need to be restarted, if any.
 * All rels are always within the same database, so we just list dbid once.
 *
 * 中文翻译：
 * 对于截断，我们在数组中列出所有截断的 relids，后跟所有需要重新
 * 启动的序列 relids（如果有）。所有rels总是在同一个数据库中
 * ，所以我们只列出dbid一次。
 */
typedef struct xl_heap_truncate
{
	Oid			dbId;
	uint32		nrelids;
	uint8		flags;
	Oid			relids[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_truncate;

#define SizeOfHeapTruncate	(offsetof(xl_heap_truncate, relids))

/*
 * We don't store the whole fixed part (HeapTupleHeaderData) of an inserted
 * or updated tuple in WAL; we can save a few bytes by reconstructing the
 * fields that are available elsewhere in the WAL record, or perhaps just
 * plain needn't be reconstructed.  These are the fields we must store.
 *
 * 中文翻译：
 * 我们不会将插入或更新的元组的整个固定部分（HeapTupleHead
 * erData）存储在 WAL 中；我们可以通过重建 WAL 记录中其
 * 他地方可用的字段来节省一些字节，或者也许只是不需要重建。这些是我们必
 * 须存储的字段。
 */
typedef struct xl_heap_header
{
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
} xl_heap_header;

#define SizeOfHeapHeader	(offsetof(xl_heap_header, t_hoff) + sizeof(uint8))

/* This is what we need to know about insert */

/* 中文翻译：这是我们需要了解的关于插入的知识 */
typedef struct xl_heap_insert
{
	OffsetNumber offnum;		/* inserted tuple's offset */

	/* 中文翻译：插入元组的偏移量 */
	uint8		flags;

	/* xl_heap_header & TUPLE DATA in backup block 0 */

	/* 中文翻译：xl_heap_header 和备份块 0 中的元组数据 */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, flags) + sizeof(uint8))

/*
 * This is what we need to know about a multi-insert.
 *
 * The main data of the record consists of this xl_heap_multi_insert header.
 * 'offsets' array is omitted if the whole page is reinitialized
 * (XLOG_HEAP_INIT_PAGE).
 *
 * In block 0's data portion, there is an xl_multi_insert_tuple struct,
 * followed by the tuple data for each tuple. There is padding to align
 * each xl_multi_insert_tuple struct.
 *
 * 中文翻译：
 * 这是我们需要了解的关于多插入件的知识。记录的主要数据由 xl_hea
 * p_multi_insert 标头组成。如果重新初始化整个页 (XL
 * OG_HEAP_INIT_PAGE)，则省略“offsets”数组。
 * 在块 0 的数据部分中，有一个 xl_multi_insert_tu
 * ple 结构，后面是每个元组的元组数据。有填充来对齐每个 xl_mu
 * lti_insert_tuple 结构。
 */
typedef struct xl_heap_multi_insert
{
	uint8		flags;
	uint16		ntuples;
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_multi_insert;

#define SizeOfHeapMultiInsert	offsetof(xl_heap_multi_insert, offsets)

typedef struct xl_multi_insert_tuple
{
	uint16		datalen;		/* size of tuple data that follows */

	/* 中文翻译：后面的元组数据的大小 */
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
	/* TUPLE DATA FOLLOWS AT END OF STRUCT */

	/* 中文翻译：元组数据位于结构末尾 */
} xl_multi_insert_tuple;

#define SizeOfMultiInsertTuple	(offsetof(xl_multi_insert_tuple, t_hoff) + sizeof(uint8))

/*
 * This is what we need to know about update|hot_update
 *
 * Backup blk 0: new page
 *
 * If XLH_UPDATE_PREFIX_FROM_OLD or XLH_UPDATE_SUFFIX_FROM_OLD flags are set,
 * the prefix and/or suffix come first, as one or two uint16s.
 *
 * After that, xl_heap_header and new tuple data follow.  The new tuple
 * data doesn't include the prefix and suffix, which are copied from the
 * old tuple on replay.
 *
 * If XLH_UPDATE_CONTAINS_NEW_TUPLE flag is given, the tuple data is
 * included even if a full-page image was taken.
 *
 * Backup blk 1: old page, if different. (no data, just a reference to the blk)
 *
 * 中文翻译：
 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
 */
typedef struct xl_heap_update
{
	TransactionId old_xmax;		/* xmax of the old tuple */

	/* 中文翻译：旧元组的 xmax */
	OffsetNumber old_offnum;	/* old tuple's offset */

	/* 中文翻译：旧元组的偏移量 */
	uint8		old_infobits_set;	/* infomask bits to set on old tuple */

	/* 中文翻译：在旧元组上设置的信息掩码位 */
	uint8		flags;
	TransactionId new_xmax;		/* xmax of the new tuple */

	/* 中文翻译：新元组的 xmax */
	OffsetNumber new_offnum;	/* new tuple's offset */

	/* 中文翻译：新元组的偏移量 */

	/*
	 * If XLH_UPDATE_CONTAINS_OLD_TUPLE or XLH_UPDATE_CONTAINS_OLD_KEY flags
	 * are set, xl_heap_header and tuple data for the old tuple follow.
	 *
	 * 中文翻译：
	 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
	 */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, new_offnum) + sizeof(OffsetNumber))

/*
 * These structures and flags encode VACUUM pruning and freezing and on-access
 * pruning page modifications.
 *
 * xl_heap_prune is the main record.  The XLHP_HAS_* flags indicate which
 * "sub-records" are included and the other XLHP_* flags provide additional
 * information about the conditions for replay.
 *
 * The data for block reference 0 contains "sub-records" depending on which of
 * the XLHP_HAS_* flags are set.  See xlhp_* struct definitions below.  The
 * sub-records appear in the same order as the XLHP_* flags.  An example
 * record with every sub-record included:
 *
 *-----------------------------------------------------------------------------
 * Main data section:
 *
 *	xl_heap_prune
 *		uint8				flags
 *	TransactionId			snapshot_conflict_horizon
 *
 * Block 0 data section:
 *
 *	xlhp_freeze_plans
 *		uint16				nplans
 *		[2 bytes of padding]
 *		xlhp_freeze_plan	plans[nplans]
 *
 *	xlhp_prune_items
 *		uint16				nredirected
 *		OffsetNumber		redirected[2 * nredirected]
 *
 *	xlhp_prune_items
 *		uint16				ndead
 *		OffsetNumber		nowdead[ndead]
 *
 *	xlhp_prune_items
 *		uint16				nunused
 *		OffsetNumber		nowunused[nunused]
 *
 *	OffsetNumber			frz_offsets[sum([plan.ntuples for plan in plans])]
 *-----------------------------------------------------------------------------
 *
 * NOTE: because the record data is assembled from many optional parts, we
 * have to pay close attention to alignment.  In the main data section,
 * 'snapshot_conflict_horizon' is stored unaligned after 'flags', to save
 * space.  In the block 0 data section, the freeze plans appear first, because
 * they contain TransactionId fields that require 4-byte alignment.  All the
 * other fields require only 2-byte alignment.  This is also the reason that
 * 'frz_offsets' is stored separately from the xlhp_freeze_plan structs.
 *
 * 中文翻译：
 * 这些结构和标志对 VACUUM 修剪和冻结以及按访问修剪页面修改进行
 * 编码。 xl_heap_prune 是主要记录。 XLHP_HAS_
 * * 标志指示包含哪些“子记录”，其他 XLHP_* 标志提供有关重播
 * 条件的附加信息。块引用 0 的数据包含“子记录”，具体取决于设置了哪
 * 个 XLHP_HAS_* 标志。请参阅下面的 xlhp_* 结构定义
 * 。子记录的出现顺序与 XLHP_* 标志相同。包含每个子记录的示例记
 * 录： 主数据部分：xl_heap_prune uint8 flags
 *  TransactionId snapshot_conflict_h
 * orizon 块 0 数据部分：xlhp_freeze_plans
 * uint16 nplans [2 个字节的填充] xlhp_free
 * ze_planplans[nplans] xlhp_prune_it
 * ems uint16 nredirected OffsetNumbe
 * r 重定向[2 * nredirected] xlhp_prune_
 * items uint16 ndead OffsetNumber no
 * wdead[ndead] xlhp_prune_items uint
 * 16 nunused OffsetNumber nowunused[
 * nunused] OffsetNumber frz_offsets[
 * sum([plan.ntuples for plan inplans
 * ])] 注意：因为记录数据是由许多可选部分组装而成，所以我们必须密切
 * 注意对齐。在主数据部分中，“snapshot_conflict_ho
 * rizo​​n”在“flags”之后未对齐存储，以节省空间。在块 0
 *  数据部分中，冻结计划首先出现，因为它们包含需要 4 字节对齐的 T
 * ransactionId 字段。所有其他字段仅需要 2 字节对齐。这
 * 也是“frz_offsets”与 xlhp_freeze_plan
 * 结构分开存储的原因。
 */
typedef struct xl_heap_prune
{
	uint8		reason;
	uint8		flags;

	/*
	 * If XLHP_HAS_CONFLICT_HORIZON is set, the conflict horizon XID follows,
	 * unaligned
	 *
	 * 中文翻译：
	 * 如果设置了 XLHP_HAS_CONFLICT_HORIZON，则冲
	 * 突范围 XID 跟随，未对齐
	 */
} xl_heap_prune;

#define SizeOfHeapPrune (offsetof(xl_heap_prune, flags) + sizeof(uint8))

/* to handle recovery conflict during logical decoding on standby */

/* 中文翻译：处理待机逻辑解码期间的恢复冲突 */
#define		XLHP_IS_CATALOG_REL			(1 << 1)

/*
 * Does replaying the record require a cleanup-lock?
 *
 * Pruning, in VACUUM's first pass or when otherwise accessing a page,
 * requires a cleanup lock.  For freezing, and VACUUM's second pass which
 * marks LP_DEAD line pointers as unused without moving any tuple data, an
 * ordinary exclusive lock is sufficient.
 *
 * 中文翻译：
 * 重放记录是否需要清理锁？在 VACUUM 的第一次传递中或以其他方式
 * 访问页面时，修剪需要清理锁。对于冻结，以及 VACUUM 的第二遍将
 *  LP_DEAD 行指针标记为未使用而不移动任何元组数据，普通的独占
 * 锁就足够了。
 */
#define		XLHP_CLEANUP_LOCK	       (1 << 2)

/*
 * If we remove or freeze any entries that contain xids, we need to include a
 * snapshot conflict horizon.  It's used in Hot Standby mode to ensure that
 * there are no queries running for which the removed tuples are still
 * visible, or which still consider the frozen XIDs as running.
 *
 * 中文翻译：
 * 如果我们删除或冻结任何包含 xids 的条目，则需要包含快照冲突范围
 * 。它在热备模式下使用，以确保没有正在运行的查询，其中已删除的元组仍然
 * 可见，或者仍然将冻结的 XID 视为正在运行。
 */
#define		XLHP_HAS_CONFLICT_HORIZON   (1 << 3)

/*
 * Indicates that an xlhp_freeze_plans sub-record and one or more
 * xlhp_freeze_plan sub-records are present.
 *
 * 中文翻译：
 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
 */
#define		XLHP_HAS_FREEZE_PLANS		(1 << 4)

/*
 * XLHP_HAS_REDIRECTIONS, XLHP_HAS_DEAD_ITEMS, and XLHP_HAS_NOW_UNUSED_ITEMS
 * indicate that xlhp_prune_items sub-records with redirected, dead, and
 * unused item offsets are present.
 *
 * 中文翻译：
 * XLHP_HAS_REDIRECTIONS、XLHP_HAS_DEA
 * D_ITEMS 和 XLHP_HAS_NOW_UNUSED_ITEM
 * S 指示存在具有重定向、死和未使用项目偏移量的 xlhp_prune
 * _items 子记录。
 */
#define		XLHP_HAS_REDIRECTIONS		(1 << 5)
#define		XLHP_HAS_DEAD_ITEMS	        (1 << 6)
#define		XLHP_HAS_NOW_UNUSED_ITEMS   (1 << 7)

/*
 * xlhp_freeze_plan describes how to freeze a group of one or more heap tuples
 * (appears in xl_heap_prune's xlhp_freeze_plans sub-record)
 *
 * 中文翻译：
 * xlhp_freeze_plan 描述如何冻结一组一个或多个堆元组（
 * 出现在 xl_heap_prune 的 xlhp_freeze_pl
 * ans 子记录中）
 */
/* 0x01 was XLH_FREEZE_XMIN */

/* 中文翻译：0x01 是 XLH_FREEZE_XMIN */
#define		XLH_FREEZE_XVAC		0x02
#define		XLH_INVALID_XVAC	0x04

typedef struct xlhp_freeze_plan
{
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* Length of individual page offset numbers array for this plan */

	/* 中文翻译：该计划的各个页面偏移量数组的长度 */
	uint16		ntuples;
} xlhp_freeze_plan;

/*
 * This is what we need to know about a block being frozen during vacuum
 *
 * The backup block's data contains an array of xlhp_freeze_plan structs (with
 * nplans elements).  The individual item offsets are located in an array at
 * the end of the entire record with nplans * (each plan's ntuples) members
 * Those offsets are in the same order as the plans.  The REDO routine uses
 * the offsets to freeze the corresponding heap tuples.
 *
 * (As of PostgreSQL 17, XLOG_HEAP2_PRUNE_VACUUM_SCAN records replace the
 * separate XLOG_HEAP2_FREEZE_PAGE records.)
 *
 * 中文翻译：
 * 这是我们需要了解在真空期间冻结块的信息备份块的数据包含 xlhp_f
 * reeze_plan 结构数组（带有 nplans 元素）。各个项目
 * 的偏移量位于整个记录末尾的数组中，其中包含 nplans *（每个计
 * 划的 ntuples）成员。这些偏移量与计划的顺序相同。 REDO
 * 例程使用偏移量来冻结相应的堆元组。 （从 PostgreSQL 17
 *  开始，XLOG_HEAP2_PRUNE_VACUUM_SCAN 记
 * 录取代了单独的 XLOG_HEAP2_FREEZE_PAGE 记录。
 * ）
 */
typedef struct xlhp_freeze_plans
{
	uint16		nplans;
	xlhp_freeze_plan plans[FLEXIBLE_ARRAY_MEMBER];
} xlhp_freeze_plans;

/*
 * Generic sub-record type contained in block reference 0 of an xl_heap_prune
 * record and used for redirect, dead, and unused items if any of
 * XLHP_HAS_REDIRECTIONS/XLHP_HAS_DEAD_ITEMS/XLHP_HAS_NOW_UNUSED_ITEMS are
 * set.  Note that in the XLHP_HAS_REDIRECTIONS variant, there are actually 2
 * * length number of OffsetNumbers in the data.
 *
 * 中文翻译：
 * xl_heap_prune 记录的块引用 0 中包含的通用子记录类型
 * ，如果设置了 XLHP_HAS_REDIRECTIONS/XLHP_
 * HAS_DEAD_ITEMS/XLHP_HAS_NOW_UNUSED
 * _ITEMS 中的任何一个，则用于重定向、死和未使用的项目。请注意，
 * 在 XLHP_HAS_REDIRECTIONS 变体中，数据中实际上
 * 有 2 * 长度的 OffsetNumbers。
 */
typedef struct xlhp_prune_items
{
	uint16		ntargets;
	OffsetNumber data[FLEXIBLE_ARRAY_MEMBER];
} xlhp_prune_items;


/* flags for infobits_set */

/* 中文翻译：infobits_set 的标志 */
#define XLHL_XMAX_IS_MULTI		0x01
#define XLHL_XMAX_LOCK_ONLY		0x02
#define XLHL_XMAX_EXCL_LOCK		0x04
#define XLHL_XMAX_KEYSHR_LOCK	0x08
#define XLHL_KEYS_UPDATED		0x10

/* flag bits for xl_heap_lock / xl_heap_lock_updated's flag field */

/* 中文翻译：xl_heap_lock / xl_heap_lock_updated 的标志字段的标志位 */
#define XLH_LOCK_ALL_FROZEN_CLEARED		0x01

/* This is what we need to know about lock */

/* 中文翻译：这是我们需要了解的关于锁的知识 */
typedef struct xl_heap_lock
{
	TransactionId xmax;			/* might be a MultiXactId */

	/* 中文翻译：可能是 MultiXactId */
	OffsetNumber offnum;		/* locked tuple's offset on page */

	/* 中文翻译：锁定元组在页面上的偏移量 */
	uint8		infobits_set;	/* infomask and infomask2 bits to set */

	/* 中文翻译：要设置的 infomask 和 infomask2 位 */
	uint8		flags;			/* XLH_LOCK_* flag bits */

	/* 中文翻译：XLH_LOCK_* 标志位 */
} xl_heap_lock;

#define SizeOfHeapLock	(offsetof(xl_heap_lock, flags) + sizeof(uint8))

/* This is what we need to know about locking an updated version of a row */

/* 中文翻译：这是我们需要了解的有关锁定行的更新版本的信息 */
typedef struct xl_heap_lock_updated
{
	TransactionId xmax;
	OffsetNumber offnum;
	uint8		infobits_set;
	uint8		flags;
} xl_heap_lock_updated;

#define SizeOfHeapLockUpdated	(offsetof(xl_heap_lock_updated, flags) + sizeof(uint8))

/* This is what we need to know about confirmation of speculative insertion */

/* 中文翻译：这是我们需要了解的关于确认投机插入的信息 */
typedef struct xl_heap_confirm
{
	OffsetNumber offnum;		/* confirmed tuple's offset on page */

	/* 中文翻译：确认元组在页面上的偏移量 */
} xl_heap_confirm;

#define SizeOfHeapConfirm	(offsetof(xl_heap_confirm, offnum) + sizeof(OffsetNumber))

/* This is what we need to know about in-place update */

/* 中文翻译：这是我们需要了解的关于就地更新的知识 */
typedef struct xl_heap_inplace
{
	OffsetNumber offnum;		/* updated tuple's offset on page */

	/* 中文翻译：更新了元组在页面上的偏移量 */
	Oid			dbId;			/* MyDatabaseId */

	/* 中文翻译：我的数据库ID */
	Oid			tsId;			/* MyDatabaseTableSpace */

	/* 中文翻译：本注释说明了相关声明、数据结构或访问流程的用途和约束。 */
	bool		relcacheInitFileInval;	/* invalidate relcache init files */

	/* 中文翻译：使 relcache 初始化文件无效 */
	int			nmsgs;			/* number of shared inval msgs */

	/* 中文翻译：本注释说明了相关声明、数据结构或访问流程的用途和约束。 */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_inplace;

#define MinSizeOfHeapInplace	(offsetof(xl_heap_inplace, nmsgs) + sizeof(int))

/*
 * This is what we need to know about setting a visibility map bit
 *
 * Backup blk 0: visibility map buffer
 * Backup blk 1: heap buffer
 *
 * 中文翻译：
 * 这是我们需要了解的有关设置可见性映射位的信息 Backup blk
 * 0：可见性映射缓冲区 Backup blk 1：堆缓冲区
 */
typedef struct xl_heap_visible
{
	TransactionId snapshotConflictHorizon;
	uint8		flags;
} xl_heap_visible;

#define SizeOfHeapVisible (offsetof(xl_heap_visible, flags) + sizeof(uint8))

typedef struct xl_heap_new_cid
{
	/*
	 * store toplevel xid so we don't have to merge cids from different
	 * transactions
	 *
	 * 中文翻译：
	 * 存储顶级 xid，这样我们就不必合并来自不同事务的 cid
	 */
	TransactionId top_xid;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* just for debugging */

	/* 中文翻译：只是为了调试 */

	/*
	 * Store the relfilelocator/ctid pair to facilitate lookups.
	 *
	 * 中文翻译：
	 * 存储 relfilelocator/ctid 对以方便查找。
	 */
	RelFileLocator target_locator;
	ItemPointerData target_tid;
} xl_heap_new_cid;

#define SizeOfHeapNewCid (offsetof(xl_heap_new_cid, target_tid) + sizeof(ItemPointerData))

/* logical rewrite xlog record header */

/* 中文翻译：逻辑重写xlog记录头 */
typedef struct xl_heap_rewrite_mapping
{
	TransactionId mapped_xid;	/* xid that might need to see the row */

	/* 中文翻译：xid 可能需要查看该行 */
	Oid			mapped_db;		/* DbOid or InvalidOid for shared rels */

	/* 中文翻译：共享 rels 的 DbOid 或 InvalidOid */
	Oid			mapped_rel;		/* Oid of the mapped relation */

	/* 中文翻译：本注释说明了相关声明、数据结构或访问流程的用途和约束。 */
	off_t		offset;			/* How far have we written so far */

	/* 中文翻译：到目前为止我们已经写了多少了 */
	uint32		num_mappings;	/* Number of in-memory mappings */

	/* 中文翻译：内存中映射的数量 */
	XLogRecPtr	start_lsn;		/* Insert LSN at begin of rewrite */

	/* 中文翻译：在重写开始时插入LSN */
} xl_heap_rewrite_mapping;

/*
 * Function HeapTupleHeaderAdvanceConflictHorizon carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapTupleHeaderAdvanceConflictHorizon通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
												  TransactionId *snapshotConflictHorizon);

/*
 * Function heap_redo carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_redo通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap_redo(XLogReaderState *record);
/*
 * Function heap_desc retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_desc通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void heap_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function heap_identify retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_identify通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern const char *heap_identify(uint8 info);
/*
 * Function heap_mask carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_mask通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap_mask(char *pagedata, BlockNumber blkno);
/*
 * Function heap2_redo carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap2_redo通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void heap2_redo(XLogReaderState *record);
/*
 * Function heap2_desc retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap2_desc通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void heap2_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function heap2_identify retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap2_identify通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern const char *heap2_identify(uint8 info);
/*
 * Function heap_xlog_logical_rewrite constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_xlog_logical_rewrite通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void heap_xlog_logical_rewrite(XLogReaderState *r);

/*
 * Function log_heap_visible evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 log_heap_visible通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern XLogRecPtr log_heap_visible(Relation rel, Buffer heap_buffer,
								   Buffer vm_buffer,
								   TransactionId snapshotConflictHorizon,
								   uint8 vmflags);

/* in heapdesc.c, so it can be shared between frontend/backend code */

/* 中文翻译：在heapdesc.c中，因此可以在前端/后端代码之间共享 */
/*
 * Function heap_xlog_deserialize_prune_and_freeze completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_xlog_deserialize_prune_and_freeze在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_xlog_deserialize_prune_and_freeze(char *cursor, uint8 flags,
												   int *nplans, xlhp_freeze_plan **plans,
												   OffsetNumber **frz_offsets,
												   int *nredirected, OffsetNumber **redirected,
												   int *ndead, OffsetNumber **nowdead,
												   int *nunused, OffsetNumber **nowunused);

#endif							/* HEAPAM_XLOG_H */

/* 中文翻译：HEAPAM_XLOG_H */
