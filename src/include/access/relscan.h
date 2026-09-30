/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/relscan.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * relscan.h POSTGRES 关系扫描描述符定义。 src/
 * include/access/relscan.h
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/htup_details.h"
#include "access/itup.h"
#include "nodes/tidbitmap.h"
#include "port/atomics.h"
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "storage/spin.h"
#include "utils/relcache.h"


struct ParallelTableScanDescData;

/*
 * Generic descriptor for table scans. This is the base-class for table scans,
 * which needs to be embedded in the scans of individual AMs.
 *
 * 中文翻译：
 * 表扫描的通用描述符。这是表扫描的基类，需要嵌入到各个 AM 的扫描中
 * 。
 */
typedef struct TableScanDescData
{
	/* scan parameters */

	/* 中文翻译：扫描参数 */
	Relation	rs_rd;			/* heap relation descriptor */

	/* 中文翻译：堆关系描述符 */
	struct SnapshotData *rs_snapshot;	/* snapshot to see */

	/* 中文翻译：快照查看 */
	int			rs_nkeys;		/* number of scan keys */

	/* 中文翻译：扫描键数量 */
	struct ScanKeyData *rs_key; /* array of scan key descriptors */

	/* 中文翻译：扫描键描述符数组 */

	/*
	 * Scan type-specific members
	 *
	 * 中文翻译：
	 * 扫描特定类型的成员
	 */
	union
	{
		/* Iterator for Bitmap Table Scans */

		/* 中文翻译：位图表扫描的迭代器 */
		TBMIterator rs_tbmiterator;

		/*
		 * Range of ItemPointers for table_scan_getnextslot_tidrange() to
		 * scan.
		 *
		 * 中文翻译：
		 * table_scan_getnextslot_tidrange()
		 * 扫描的 ItemPointers 范围。
		 */
		struct
		{
			ItemPointerData rs_mintid;
			ItemPointerData rs_maxtid;
		}			tidrange;
	}			st;

	/*
	 * Information about type and behaviour of the scan, a bitmask of members
	 * of the ScanOptions enum (see tableam.h).
	 *
	 * 中文翻译：
	 * 有关扫描类型和行为的信息，ScanOptions 枚举成员的位掩码（
	 * 请参阅 tableam.h）。
	 */
	uint32		rs_flags;

	struct ParallelTableScanDescData *rs_parallel;	/* parallel scan
													 * information */

	/* 中文翻译：
	 * 并行扫描信息
	 */
} TableScanDescData;
typedef struct TableScanDescData *TableScanDesc;

/*
 * Shared state for parallel table scan.
 *
 * Each backend participating in a parallel table scan has its own
 * TableScanDesc in backend-private memory, and those objects all contain a
 * pointer to this structure.  The information here must be sufficient to
 * properly initialize each new TableScanDesc as workers join the scan, and it
 * must act as a information what to scan for those workers.
 *
 * 中文翻译：
 * 并行表扫描的共享状态。参与并行表扫描的每个后端在后端私有内存中都有自
 * 己的 TableScanDesc，并且这些对象都包含指向该结构的指针
 * 。当工作人员加入扫描时，此处的信息必须足以正确初始化每个新的 Tab
 * leScanDesc，并且它必须充当为这些工作人员扫描内容的信息。
 */
typedef struct ParallelTableScanDescData
{
	RelFileLocator phs_locator; /* physical relation to scan */

	/* 中文翻译：与扫描的物理关系 */
	bool		phs_syncscan;	/* report location to syncscan logic? */

	/* 中文翻译：向同步扫描逻辑报告位置？ */
	bool		phs_snapshot_any;	/* SnapshotAny, not phs_snapshot_data? */

	/* 中文翻译：SnapshotAny，不是 phs_snapshot_data？ */
	Size		phs_snapshot_off;	/* data for snapshot */

	/* 中文翻译：快照数据 */
} ParallelTableScanDescData;
typedef struct ParallelTableScanDescData *ParallelTableScanDesc;

/*
 * Shared state for parallel table scans, for block oriented storage.
 *
 * 中文翻译：
 * 并行表扫描的共享状态，面向块的存储。
 */
typedef struct ParallelBlockTableScanDescData
{
	ParallelTableScanDescData base;

	BlockNumber phs_nblocks;	/* # blocks in relation at start of scan */

	/* 中文翻译：# 扫描开始时相关的块 */
	slock_t		phs_mutex;		/* mutual exclusion for setting startblock */

	/* 中文翻译：设置起始块的互斥 */
	BlockNumber phs_startblock; /* starting block number */

	/* 中文翻译：起始块号 */
	pg_atomic_uint64 phs_nallocated;	/* number of blocks allocated to
										 * workers so far. */

	/* 中文翻译：
	 * 到目前为止分配给工作人员的块数。
	 */
}			ParallelBlockTableScanDescData;
typedef struct ParallelBlockTableScanDescData *ParallelBlockTableScanDesc;

/*
 * Per backend state for parallel table scan, for block-oriented storage.
 *
 * 中文翻译：
 * 每个后端状态用于并行表扫描，用于面向块的存储。
 */
typedef struct ParallelBlockTableScanWorkerData
{
	uint64		phsw_nallocated;	/* Current # of blocks into the scan */

	/* 中文翻译：当前扫描的块数 */
	uint32		phsw_chunk_remaining;	/* # blocks left in this chunk */

	/* 中文翻译：# 该块中剩余的块 */
	uint32		phsw_chunk_size;	/* The number of blocks to allocate in
									 * each I/O chunk for the scan */

	/* 中文翻译：
	 * 每个 I/O 块中为扫描分配的块数
	 */
} ParallelBlockTableScanWorkerData;
typedef struct ParallelBlockTableScanWorkerData *ParallelBlockTableScanWorker;

/*
 * Base class for fetches from a table via an index. This is the base-class
 * for such scans, which needs to be embedded in the respective struct for
 * individual AMs.
 *
 * 中文翻译：
 * 通过索引从表中获取的基类。这是此类扫描的基类，需要嵌入到各个 AM
 * 的相应结构中。
 */
typedef struct IndexFetchTableData
{
	Relation	rel;
} IndexFetchTableData;

struct IndexScanInstrumentation;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.
 *
 * 中文翻译：
 * 我们对基于 amgettuple 和基于 amgetbitmap 的
 * 索引扫描使用相同的 IndexScanDescData 结构。有些字
 * 段仅与基于 amgettuple 的扫描相关。
 */
typedef struct IndexScanDescData
{
	/* scan parameters */

	/* 中文翻译：扫描参数 */
	Relation	heapRelation;	/* heap relation descriptor, or NULL */

	/* 中文翻译：堆关系描述符，或 NULL */
	Relation	indexRelation;	/* index relation descriptor */

	/* 中文翻译：索引关系描述符 */
	struct SnapshotData *xs_snapshot;	/* snapshot to see */

	/* 中文翻译：快照查看 */
	int			numberOfKeys;	/* number of index qualifier conditions */

	/* 中文翻译：索引限定条件的数量 */
	int			numberOfOrderBys;	/* number of ordering operators */

	/* 中文翻译：订购操作员数量 */
	struct ScanKeyData *keyData;	/* array of index qualifier descriptors */

	/* 中文翻译：索引限定符描述符数组 */
	struct ScanKeyData *orderByData;	/* array of ordering op descriptors */

	/* 中文翻译：排序操作描述符数组 */
	bool		xs_want_itup;	/* caller requests index tuples */

	/* 中文翻译：调用者请求索引元组 */
	bool		xs_temp_snap;	/* unregister snapshot at scan end? */

	/* 中文翻译：本注释说明了相关声明、数据结构或访问流程的用途和约束。 */

	/* signaling to index AM about killing index tuples */

	/* 中文翻译：向索引 AM 发出关于杀死索引元组的信号 */
	bool		kill_prior_tuple;	/* last-returned tuple is dead */

	/* 中文翻译：本注释说明了相关声明、数据结构或访问流程的用途和约束。 */
	bool		ignore_killed_tuples;	/* do not return killed entries */

	/* 中文翻译：不返回被杀死的条目 */
	bool		xactStartedInRecovery;	/* prevents killing/seeing killed
										 * tuples */

	/* 中文翻译：
	 * 防止杀死/看到被杀死的元组
	 */

	/* index access method's private state */

	/* 中文翻译：索引访问方法的私有状态 */
	void	   *opaque;			/* access-method-specific info */

	/* 中文翻译：访问方法特定信息 */

	/*
	 * Instrumentation counters maintained by all index AMs during both
	 * amgettuple calls and amgetbitmap calls (unless field remains NULL)
	 *
	 * 中文翻译：
	 * 在 amgettuple 调用和 amgetbitmap 调用期间由
	 * 所有索引 AM 维护的仪器计数器（除非字段保持为 NULL）
	 */
	struct IndexScanInstrumentation *instrument;

	/*
	 * In an index-only scan, a successful amgettuple call must fill either
	 * xs_itup (and xs_itupdesc) or xs_hitup (and xs_hitupdesc) to provide the
	 * data returned by the scan.  It can fill both, in which case the heap
	 * format will be used.
	 *
	 * 中文翻译：
	 * 在仅索引扫描中，成功的 amgettuple 调用必须填充 xs_i
	 * tup（和 xs_itupdesc）或 xs_hitup（和 xs_
	 * hitupdesc）以提供扫描返回的数据。它可以同时填充两者，在这种
	 * 情况下将使用堆格式。
	 */
	IndexTuple	xs_itup;		/* index tuple returned by AM */

	/* 中文翻译：AM 返回的索引元组 */
	struct TupleDescData *xs_itupdesc;	/* rowtype descriptor of xs_itup */

	/* 中文翻译：xs_itup 的行类型描述符 */
	HeapTuple	xs_hitup;		/* index data returned by AM, as HeapTuple */

	/* 中文翻译：AM返回的索引数据，作为HeapTuple */
	struct TupleDescData *xs_hitupdesc; /* rowtype descriptor of xs_hitup */

	/* 中文翻译：xs_hitup 的行类型描述符 */

	ItemPointerData xs_heaptid; /* result */

	/* 中文翻译：结果 */
	bool		xs_heap_continue;	/* T if must keep walking, potential
									 * further results */

	/* 中文翻译：
	 * 如果必须继续行走，可能会产生进一步的结果
	 */
	IndexFetchTableData *xs_heapfetch;

	bool		xs_recheck;		/* T means scan keys must be rechecked */

	/* 中文翻译：T 表示必须重新检查扫描键 */

	/*
	 * When fetching with an ordering operator, the values of the ORDER BY
	 * expressions of the last returned tuple, according to the index.  If
	 * xs_recheckorderby is true, these need to be rechecked just like the
	 * scan keys, and the values returned here are a lower-bound on the actual
	 * values.
	 *
	 * 中文翻译：
	 * 使用排序运算符获取时，根据索引获取最后返回的元组的 ORDER BY
	 *  表达式的值。如果 xs_recheckorderby 为 true
	 * ，则需要像扫描键一样重新检查这些值，并且此处返回的值是实际值的下限。
	 */
	Datum	   *xs_orderbyvals;
	bool	   *xs_orderbynulls;
	bool		xs_recheckorderby;

	/* parallel index scan information, in shared memory */

	/* 中文翻译：共享内存中的并行索引扫描信息 */
	struct ParallelIndexScanDescData *parallel_scan;
}			IndexScanDescData;

/* Generic structure for parallel scans */

/* 中文翻译：并行扫描的通用结构 */
typedef struct ParallelIndexScanDescData
{
	RelFileLocator ps_locator;	/* physical table relation to scan */

	/* 中文翻译：物理表与扫描的关系 */
	RelFileLocator ps_indexlocator; /* physical index relation to scan */

	/* 中文翻译：物理索引与扫描的关系 */
	Size		ps_offset_ins;	/* Offset to SharedIndexScanInstrumentation */

	/* 中文翻译：SharedIndexScanInstrumentation 的偏移量 */
	Size		ps_offset_am;	/* Offset to am-specific structure */

	/* 中文翻译：偏移特定于 am 的结构 */
	char		ps_snapshot_data[FLEXIBLE_ARRAY_MEMBER];
}			ParallelIndexScanDescData;

struct TupleTableSlot;

/* Struct for storage-or-index scans of system tables */

/* 中文翻译：本注释说明了相关声明、数据结构或访问流程的用途和约束。 */
typedef struct SysScanDescData
{
	Relation	heap_rel;		/* catalog being scanned */

	/* 中文翻译：正在扫描目录 */
	Relation	irel;			/* NULL if doing heap scan */

	/* 中文翻译：如果进行堆扫描则为 NULL */
	struct TableScanDescData *scan; /* only valid in storage-scan case */

	/* 中文翻译：仅在存储扫描情况下有效 */
	struct IndexScanDescData *iscan;	/* only valid in index-scan case */

	/* 中文翻译：仅在索引扫描情况下有效 */
	struct SnapshotData *snapshot;	/* snapshot to unregister at end of scan */

	/* 中文翻译：扫描结束时取消注册的快照 */
	struct TupleTableSlot *slot;
}			SysScanDescData;

#endif							/* RELSCAN_H */

/* 中文翻译：RELSCAN_H */
