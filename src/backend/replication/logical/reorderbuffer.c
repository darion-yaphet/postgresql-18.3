/*-------------------------------------------------------------------------
 *
 * reorderbuffer.c
 *	  PostgreSQL logical replay/reorder buffer management
 *
 *	  PostgreSQL 逻辑重放与 reorder buffer 的管理。
 *
 *
 * Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/reorderbuffer.c
 *
 * NOTES
 *	  This module gets handed individual pieces of transactions in the order
 *	  they are written to the WAL and is responsible to reassemble them into
 *	  toplevel transaction sized pieces. When a transaction is completely
 *	  reassembled - signaled by reading the transaction commit record - it
 *	  will then call the output plugin (cf. ReorderBufferCommit()) with the
 *	  individual changes. The output plugins rely on snapshots built by
 *	  snapbuild.c which hands them to us.
 *
 *	  本模块按写入 WAL 的顺序接收事务的各个片段，并把它们重新组装成顶层事务规模的片段。事务完整重组后（
 *	  以读到该事务的 commit 记录为信号），再调用输出插件（参见 ReorderBufferCommit()）逐条交出变更。输出
 *	  插件依赖 snapbuild.c 构建并交给本模块的快照。
 *
 *	  Transactions and subtransactions/savepoints in postgres are not
 *	  immediately linked to each other from outside the performing
 *	  backend. Only at commit/abort (or special xact_assignment records) they
 *	  are linked together. Which means that we will have to splice together a
 *	  toplevel transaction from its subtransactions. To do that efficiently we
 *	  build a binary heap indexed by the smallest current lsn of the individual
 *	  subtransactions' changestreams. As the individual streams are inherently
 *	  ordered by LSN - since that is where we build them from - the transaction
 *	  can easily be reassembled by always using the subtransaction with the
 *	  smallest current LSN from the heap.
 *
 *	  在执行该事务的后端之外，postgres 中的事务与子事务或 savepoint 并不会立刻彼此关联。只在 commit、
 *	  abort，或特殊的 xact_assignment 记录处才会连在一起。因此必须把子事务拼接成顶层事务。为提高效率，按
 *	  各子事务变更流当前最小的 LSN 建立二叉堆。各流本身已按 LSN 有序（因为就是据此建立的），所以总是取堆
 *	  中当前 LSN 最小的子事务，即可重组出该事务。
 *
 *	  In order to cope with large transactions - which can be several times as
 *	  big as the available memory - this module supports spooling the contents
 *	  of large transactions to disk. When the transaction is replayed the
 *	  contents of individual (sub-)transactions will be read from disk in
 *	  chunks.
 *
 *	  为应对可能数倍于可用内存的大事务，本模块支持把大事务内容溢出到磁盘。重放时再按块读回各个（子）事务
 *	  的内容。
 *
 *	  This module also has to deal with reassembling toast records from the
 *	  individual chunks stored in WAL. When a new (or initial) version of a
 *	  tuple is stored in WAL it will always be preceded by the toast chunks
 *	  emitted for the columns stored out of line. Within a single toplevel
 *	  transaction there will be no other data carrying records between a row's
 *	  toast chunks and the row data itself. See ReorderBufferToast* for
 *	  details.
 *
 *	  本模块还要把 WAL 中的各个 toast 块重新拼起来。元组的新版本（或初始版本）写入 WAL 时，行外列对应的
 *	  toast 块总会先出现。在同一个顶层事务内，一行的 toast 块与该行数据之间不会再插入其他携带数据的记录。
 *	  详见 ReorderBufferToast 系列函数。
 *
 *	  ReorderBuffer uses two special memory context types - SlabContext for
 *	  allocations of fixed-length structures (changes and transactions), and
 *	  GenerationContext for the variable-length transaction data (allocated
 *	  and freed in groups with similar lifespans).
 *
 *	  ReorderBuffer 使用两种特殊内存上下文：SlabContext 分配定长结构（change 与 transaction），
 *	  GenerationContext 分配变长事务数据（生命周期相近的对象成组分配、成组释放）。
 *
 *	  To limit the amount of memory used by decoded changes, we track memory
 *	  used at the reorder buffer level (i.e. total amount of memory), and for
 *	  each transaction. When the total amount of used memory exceeds the
 *	  limit, the transaction consuming the most memory is then serialized to
 *	  disk.
 *
 *	  为限制已解码变更占用的内存，在 reorder buffer 层（即总内存）以及每个事务上分别记账。总用量超过上限
 *	  时，把占用内存最多的事务序列化到磁盘。
 *
 *	  Only decoded changes are evicted from memory (spilled to disk), not the
 *	  transaction records. The number of toplevel transactions is limited,
 *	  but a transaction with many subtransactions may still consume significant
 *	  amounts of memory. However, the transaction records are fairly small and
 *	  are not included in the memory limit.
 *
 *	  只有已解码的变更会从内存逐出（溢出到磁盘），事务记录本身不会。顶层事务数量有限，但含子事务很多的事
 *	  务仍可能占用大量内存。不过事务记录本身较小，并且不计入内存上限。
 *
 *	  The current eviction algorithm is very simple - the transaction is
 *	  picked merely by size, while it might be useful to also consider age
 *	  (LSN) of the changes for example. With the new Generational memory
 *	  allocator, evicting the oldest changes would make it more likely the
 *	  memory gets actually freed.
 *
 *	  当前逐出算法很简单，只按大小挑选事务；有时把变更的年龄（LSN）也考虑进去会更有用。换用分代内存分配
 *	  器后，逐出最老的变更更有可能让内存真正被释放。
 *
 *	  We use a max-heap with transaction size as the key to efficiently find
 *	  the largest transaction. We update the max-heap whenever the memory
 *	  counter is updated; however transactions with size 0 are not stored in
 *	  the heap, because they have no changes to evict.
 *
 *	  以事务大小为关键字维护最大堆，以便高效找到最大的事务。内存计数更新时同步更新该堆；大小为 0 的事务
 *	  不进堆，因为它们没有可逐出的变更。
 *
 *	  We still rely on max_changes_in_memory when loading serialized changes
 *	  back into memory. At that point we can't use the memory limit directly
 *	  as we load the subxacts independently. One option to deal with this
 *	  would be to count the subxacts, and allow each to allocate 1/N of the
 *	  memory limit. That however does not seem very appealing, because with
 *	  many subtransactions it may easily cause thrashing (short cycles of
 *	  deserializing and applying very few changes). We probably should give
 *	  a bit more memory to the oldest subtransactions, because it's likely
 *	  they are the source for the next sequence of changes.
 *
 *	  把已序列化的变更装回内存时，仍依赖 max_changes_in_memory。此时不能直接套用内存上限，因为各个子事务
 *	  是独立装入的。一种做法是先数出子事务个数，让每个子事务分到内存上限的 N 分之一。这并不理想：子事务
 *	  很多时很容易抖动（短时间内反复反序列化并应用极少变更）。也许应给最老的子事务多一点内存，因为下一串
 *	  变更很可能来自它们。
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/detoast.h"
#include "access/heapam.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "common/int.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"	/* just for SnapBuildSnapDecRefcount
					 *
					 * 仅供 SnapBuildSnapDecRefcount 使用。
					 */
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relfilenumbermap.h"

/*
 * Each transaction has an 8MB limit for invalidation messages distributed from
 * other transactions. This limit is set considering scenarios with many
 * concurrent logical decoding operations. When the distributed invalidation
 * messages reach this threshold, the transaction is marked as
 * RBTXN_DISTR_INVAL_OVERFLOWED to invalidate the complete cache as we have lost
 * some inval messages and hence don't know what needs to be invalidated.
 *
 * 每个事务对来自其他事务分发的失效消息设有 8MB 上限。该限制考虑到可能同时进行大量逻辑解码。分发的失效消息达
 * 到阈值时，把事务标为 RBTXN_DISTR_INVAL_OVERFLOWED，并作废整个缓存：此时已丢失部分失效消息，无法知道究竟该
 * 失效哪些内容。
 */
#define MAX_DISTR_INVAL_MSG_PER_TXN \
	((8 * 1024 * 1024) / sizeof(SharedInvalidationMessage))

/* entry for a hash table we use to map from xid to our transaction state
 *
 * 哈希表项，用于从 xid 映射到本模块的事务状态。
 */
typedef struct ReorderBufferTXNByIdEnt
{
	TransactionId xid;
	ReorderBufferTXN *txn;
} ReorderBufferTXNByIdEnt;

/* data structures for (relfilelocator, ctid) => (cmin, cmax) mapping
 *
 * (relfilelocator, ctid) 到 (cmin, cmax) 映射所用的数据结构。
 */
typedef struct ReorderBufferTupleCidKey
{
	RelFileLocator rlocator;
	ItemPointerData tid;
} ReorderBufferTupleCidKey;

typedef struct ReorderBufferTupleCidEnt
{
	ReorderBufferTupleCidKey key;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* just for debugging
						 *
						 * 仅用于调试。
						 */
} ReorderBufferTupleCidEnt;

/* Virtual file descriptor with file offset tracking
 *
 * 带文件偏移跟踪的虚拟文件描述符。
 */
typedef struct TXNEntryFile
{
	File		vfd;			/* -1 when the file is closed
						 *
						 * 文件关闭时为 -1。
						 */
	off_t		curOffset;		/* offset for next write or read. Reset to 0
								 * when vfd is opened.
						 *
						 * 下一次写或读的偏移。打开 vfd 时重置为 0。
						 */
} TXNEntryFile;

/* k-way in-order change iteration support structures
 *
 * 按序做 k 路变更迭代所用的支持结构。
 */
typedef struct ReorderBufferIterTXNEntry
{
	XLogRecPtr	lsn;
	ReorderBufferChange *change;
	ReorderBufferTXN *txn;
	TXNEntryFile file;
	XLogSegNo	segno;
} ReorderBufferIterTXNEntry;

typedef struct ReorderBufferIterTXNState
{
	binaryheap *heap;
	Size		nr_txns;
	dlist_head	old_change;
	ReorderBufferIterTXNEntry entries[FLEXIBLE_ARRAY_MEMBER];
} ReorderBufferIterTXNState;

/* toast datastructures
 *
 * toast 数据结构。
 */
typedef struct ReorderBufferToastEnt
{
	Oid			chunk_id;		/* toast_table.chunk_id
							 *
							 * toast 表的 chunk_id。
							 */
	int32		last_chunk_seq; /* toast_table.chunk_seq of the last chunk we
								 * have seen
					 *
					 * 已经见过的最后一个块在 toast 表中的 chunk_seq。
					 */
	Size		num_chunks;		/* number of chunks we've already seen
						 *
						 * 已经见过的块数。
						 */
	Size		size;			/* combined size of chunks seen
						 *
						 * 已见块的合计大小。
						 */
	dlist_head	chunks;			/* linked list of chunks
						 *
						 * 块的链表。
						 */
	struct varlena *reconstructed;	/* reconstructed varlena now pointed to in
									 * main tup
					 *
					 * 已重建的 varlena，现由主元组指向它。
					 */
} ReorderBufferToastEnt;

/* Disk serialization support datastructures
 *
 * 磁盘序列化所用的数据结构。
 */
typedef struct ReorderBufferDiskChange
{
	Size		size;
	ReorderBufferChange change;
	/* data follows
	 *
	 * 其后紧跟数据。
	 */
} ReorderBufferDiskChange;

#define IsSpecInsert(action) \
( \
	((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT) \
)
#define IsSpecConfirmOrAbort(action) \
( \
	(((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM) || \
	((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT)) \
)
#define IsInsertOrUpdate(action) \
( \
	(((action) == REORDER_BUFFER_CHANGE_INSERT) || \
	((action) == REORDER_BUFFER_CHANGE_UPDATE) || \
	((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT)) \
)

/*
 * Maximum number of changes kept in memory, per transaction. After that,
 * changes are spooled to disk.
 *
 * 每个事务在内存中保留的变更数量上限，超过后变更溢出到磁盘。
 *
 * The current value should be sufficient to decode the entire transaction
 * without hitting disk in OLTP workloads, while starting to spool to disk in
 * other workloads reasonably fast.
 *
 * 当前取值应足以在 OLTP 负载下完整解码事务而不访问磁盘，同时在其他负载下较快开始溢出到磁盘。
 *
 * At some point in the future it probably makes sense to have a more elaborate
 * resource management here, but it's not entirely clear what that would look
 * like.
 *
 * 将来这里也许应做更精细的资源管理，但具体形态尚不清楚。
 */
int			logical_decoding_work_mem;
static const Size max_changes_in_memory = 4096; /* XXX for restore only
						 *
						 * XXX：仅用于从磁盘恢复。
						 */

/* GUC variable
 *
 * GUC 变量。
 */
int			debug_logical_replication_streaming = DEBUG_LOGICAL_REP_STREAMING_BUFFERED;

/* ---------------------------------------
 * primary reorderbuffer support routines
 *
 * reorderbuffer 的主要支持例程。
 * ---------------------------------------
 */
static ReorderBufferTXN *ReorderBufferAllocTXN(ReorderBuffer *rb);
static void ReorderBufferFreeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static ReorderBufferTXN *ReorderBufferTXNByXid(ReorderBuffer *rb,
											   TransactionId xid, bool create, bool *is_new,
											   XLogRecPtr lsn, bool create_as_top);
static void ReorderBufferTransferSnapToParent(ReorderBufferTXN *txn,
											  ReorderBufferTXN *subtxn);

static void AssertTXNLsnOrder(ReorderBuffer *rb);

/* ---------------------------------------
 * support functions for lsn-order iterating over the ->changes of a
 * transaction and its subtransactions
 *
 * 按 LSN 顺序遍历事务及其子事务的 ->changes 的支持函数。
 *
 * used for iteration over the k-way heap merge of a transaction and its
 * subtransactions
 *
 * 用于对事务及其子事务做 k 路堆归并迭代。
 * ---------------------------------------
 */
static void ReorderBufferIterTXNInit(ReorderBuffer *rb, ReorderBufferTXN *txn,
									 ReorderBufferIterTXNState *volatile *iter_state);
static ReorderBufferChange *ReorderBufferIterTXNNext(ReorderBuffer *rb, ReorderBufferIterTXNState *state);
static void ReorderBufferIterTXNFinish(ReorderBuffer *rb,
									   ReorderBufferIterTXNState *state);
static void ReorderBufferExecuteInvalidations(uint32 nmsgs, SharedInvalidationMessage *msgs);

/*
 * ---------------------------------------
 * Disk serialization support functions
 *
 * 磁盘序列化支持函数。
 * ---------------------------------------
 */
static void ReorderBufferCheckMemoryLimit(ReorderBuffer *rb);
static void ReorderBufferSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferSerializeChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
										 int fd, ReorderBufferChange *change);
static Size ReorderBufferRestoreChanges(ReorderBuffer *rb, ReorderBufferTXN *txn,
										TXNEntryFile *file, XLogSegNo *segno);
static void ReorderBufferRestoreChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
									   char *data);
static void ReorderBufferRestoreCleanup(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferTruncateTXN(ReorderBuffer *rb, ReorderBufferTXN *txn,
									 bool txn_prepared);
static void ReorderBufferMaybeMarkTXNStreamed(ReorderBuffer *rb, ReorderBufferTXN *txn);
static bool ReorderBufferCheckAndTruncateAbortedTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferCleanupSerializedTXNs(const char *slotname);
static void ReorderBufferSerializedPath(char *path, ReplicationSlot *slot,
										TransactionId xid, XLogSegNo segno);
static int	ReorderBufferTXNSizeCompare(const pairingheap_node *a, const pairingheap_node *b, void *arg);

static void ReorderBufferFreeSnap(ReorderBuffer *rb, Snapshot snap);
static Snapshot ReorderBufferCopySnap(ReorderBuffer *rb, Snapshot orig_snap,
									  ReorderBufferTXN *txn, CommandId cid);

/*
 * ---------------------------------------
 * Streaming support functions
 *
 * 流式传输支持函数。
 * ---------------------------------------
 */
static inline bool ReorderBufferCanStream(ReorderBuffer *rb);
static inline bool ReorderBufferCanStartStreaming(ReorderBuffer *rb);
static void ReorderBufferStreamTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferStreamCommit(ReorderBuffer *rb, ReorderBufferTXN *txn);

/* ---------------------------------------
 * toast reassembly support
 *
 * toast 重组支持。
 * ---------------------------------------
 */
static void ReorderBufferToastInitHash(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferToastReset(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferToastReplace(ReorderBuffer *rb, ReorderBufferTXN *txn,
									  Relation relation, ReorderBufferChange *change);
static void ReorderBufferToastAppendChunk(ReorderBuffer *rb, ReorderBufferTXN *txn,
										  Relation relation, ReorderBufferChange *change);

/*
 * ---------------------------------------
 * memory accounting
 *
 * 内存记账。
 * ---------------------------------------
 */
static Size ReorderBufferChangeSize(ReorderBufferChange *change);
static void ReorderBufferChangeMemoryUpdate(ReorderBuffer *rb,
											ReorderBufferChange *change,
											ReorderBufferTXN *txn,
											bool addition, Size sz);

/*
 * 核心流程：
 * 解码侧按 WAL 顺序把变更交给本文件。ReorderBufferQueueChange 按 XID 归入
 * ReorderBufferTXN；子事务要到提交、中止或 xact_assignment 时，才由
 * ReorderBufferAssignChild 挂到顶层事务，并用按当前最小 LSN 的二叉堆把子事务
 * 变更流归并回去。内存超过 logical_decoding_work_mem 时，
 * ReorderBufferCheckMemoryLimit 选出最大事务：可以流式发送则走
 * ReorderBufferStreamTXN，否则 ReorderBufferSerializeTXN 溢出到磁盘。
 * 读到提交或 PREPARE 后，ReorderBufferCommit / ReorderBufferPrepare 经
 * ReorderBufferReplay 调用 ReorderBufferProcessTXN，按 LSN 重放，重组 toast
 * 后交给输出插件。中止走 ReorderBufferAbort；崩溃留下的 spill 文件由
 * StartupReorderBuffer 清理。
 */

/*
 * Allocate a new ReorderBuffer and clean out any old serialized state from
 * prior ReorderBuffer instances for the same slot.
 *
 * 分配一个新的 ReorderBuffer，并清掉同一复制槽上先前 ReorderBuffer 实例留下的序列化状态。
 */
ReorderBuffer *
ReorderBufferAllocate(void)
{
	ReorderBuffer *buffer;
	HASHCTL		hash_ctl;
	MemoryContext new_ctx;

	Assert(MyReplicationSlot != NULL);

	/* allocate memory in own context, to have better accountability
	 *
	 * 在独立内存上下文中分配，便于核算内存。
	 */
	new_ctx = AllocSetContextCreate(CurrentMemoryContext,
									"ReorderBuffer",
									ALLOCSET_DEFAULT_SIZES);

	buffer =
		(ReorderBuffer *) MemoryContextAlloc(new_ctx, sizeof(ReorderBuffer));

	memset(&hash_ctl, 0, sizeof(hash_ctl));

	buffer->context = new_ctx;

	buffer->change_context = SlabContextCreate(new_ctx,
											   "Change",
											   SLAB_DEFAULT_BLOCK_SIZE,
											   sizeof(ReorderBufferChange));

	buffer->txn_context = SlabContextCreate(new_ctx,
											"TXN",
											SLAB_DEFAULT_BLOCK_SIZE,
											sizeof(ReorderBufferTXN));

	/*
	 * To minimize memory fragmentation caused by long-running transactions
	 * with changes spanning multiple memory blocks, we use a single
	 * fixed-size memory block for decoded tuple storage. The performance
	 * testing showed that the default memory block size maintains logical
	 * decoding performance without causing fragmentation due to concurrent
	 * transactions. One might think that we can use the max size as
	 * SLAB_LARGE_BLOCK_SIZE but the test also showed it doesn't help resolve
	 * the memory fragmentation.
	 *
	 * 为减少长事务的变更跨多个内存块造成的碎片，解码后的元组存放在单一固定大小的内存块中。性能测试表明，
	 * 默认块大小能维持逻辑解码性能，且不会因并发事务产生碎片。也许会想到把最大尺寸设为
	 * SLAB_LARGE_BLOCK_SIZE，但测试表明那并不能消除碎片。
	 */
	buffer->tup_context = GenerationContextCreate(new_ctx,
												  "Tuples",
												  SLAB_DEFAULT_BLOCK_SIZE,
												  SLAB_DEFAULT_BLOCK_SIZE,
												  SLAB_DEFAULT_BLOCK_SIZE);

	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(ReorderBufferTXNByIdEnt);
	hash_ctl.hcxt = buffer->context;

	buffer->by_txn = hash_create("ReorderBufferByXid", 1000, &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	buffer->by_txn_last_xid = InvalidTransactionId;
	buffer->by_txn_last_txn = NULL;

	buffer->outbuf = NULL;
	buffer->outbufsize = 0;
	buffer->size = 0;

	/* txn_heap is ordered by transaction size
	 *
	 * txn_heap 按事务大小排序。
	 */
	buffer->txn_heap = pairingheap_allocate(ReorderBufferTXNSizeCompare, NULL);

	buffer->spillTxns = 0;
	buffer->spillCount = 0;
	buffer->spillBytes = 0;
	buffer->streamTxns = 0;
	buffer->streamCount = 0;
	buffer->streamBytes = 0;
	buffer->totalTxns = 0;
	buffer->totalBytes = 0;

	buffer->current_restart_decoding_lsn = InvalidXLogRecPtr;

	dlist_init(&buffer->toplevel_by_lsn);
	dlist_init(&buffer->txns_by_base_snapshot_lsn);
	dclist_init(&buffer->catchange_txns);

	/*
	 * Ensure there's no stale data from prior uses of this slot, in case some
	 * prior exit avoided calling ReorderBufferFree. Failure to do this can
	 * produce duplicated txns, and it's very cheap if there's nothing there.
	 *
	 * 确保该复制槽没有上次使用留下的过期数据，以防先前退出时没有调用 ReorderBufferFree。否则可能产生重
	 * 复事务；若本来就没有残留，这一步开销很小。
	 */
	ReorderBufferCleanupSerializedTXNs(NameStr(MyReplicationSlot->data.name));

	return buffer;
}

/*
 * Free a ReorderBuffer
 *
 * 释放 ReorderBuffer。
 */
void
ReorderBufferFree(ReorderBuffer *rb)
{
	MemoryContext context = rb->context;

	/*
	 * We free separately allocated data by entirely scrapping reorderbuffer's
	 * memory context.
	 *
	 * 通过整体销毁 reorderbuffer 的内存上下文，释放单独分配的数据。
	 */
	MemoryContextDelete(context);

	/* Free disk space used by unconsumed reorder buffers
	 *
	 * 释放尚未消费的 reorder buffer 占用的磁盘空间。
	 */
	ReorderBufferCleanupSerializedTXNs(NameStr(MyReplicationSlot->data.name));
}

/*
 * Allocate a new ReorderBufferTXN.
 *
 * 分配一个新的 ReorderBufferTXN。
 */
static ReorderBufferTXN *
ReorderBufferAllocTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	txn = (ReorderBufferTXN *)
		MemoryContextAlloc(rb->txn_context, sizeof(ReorderBufferTXN));

	memset(txn, 0, sizeof(ReorderBufferTXN));

	dlist_init(&txn->changes);
	dlist_init(&txn->tuplecids);
	dlist_init(&txn->subtxns);

	/* InvalidCommandId is not zero, so set it explicitly
	 *
	 * InvalidCommandId 不是零，因此显式设置。
	 */
	txn->command_id = InvalidCommandId;
	txn->output_plugin_private = NULL;

	return txn;
}

/*
 * Free a ReorderBufferTXN.
 *
 * 释放 ReorderBufferTXN。
 */
static void
ReorderBufferFreeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/* clean the lookup cache if we were cached (quite likely)
	 *
	 * 若本事务正在查找缓存中（很常见），则清掉该缓存。
	 */
	if (rb->by_txn_last_xid == txn->xid)
	{
		rb->by_txn_last_xid = InvalidTransactionId;
		rb->by_txn_last_txn = NULL;
	}

	/* free data that's contained
	 *
	 * 释放其中包含的数据。
	 */

	if (txn->gid != NULL)
	{
		pfree(txn->gid);
		txn->gid = NULL;
	}

	if (txn->tuplecid_hash != NULL)
	{
		hash_destroy(txn->tuplecid_hash);
		txn->tuplecid_hash = NULL;
	}

	if (txn->invalidations)
	{
		pfree(txn->invalidations);
		txn->invalidations = NULL;
	}

	if (txn->invalidations_distributed)
	{
		pfree(txn->invalidations_distributed);
		txn->invalidations_distributed = NULL;
	}

	/* Reset the toast hash
	 *
	 * 重置 toast 哈希表。
	 */
	ReorderBufferToastReset(rb, txn);

	/* All changes must be deallocated
	 *
	 * 所有变更必须已经释放。
	 */
	Assert(txn->size == 0);

	pfree(txn);
}

/*
 * Allocate a ReorderBufferChange.
 *
 * 分配一个 ReorderBufferChange。
 */
ReorderBufferChange *
ReorderBufferAllocChange(ReorderBuffer *rb)
{
	ReorderBufferChange *change;

	change = (ReorderBufferChange *)
		MemoryContextAlloc(rb->change_context, sizeof(ReorderBufferChange));

	memset(change, 0, sizeof(ReorderBufferChange));
	return change;
}

/*
 * Free a ReorderBufferChange and update memory accounting, if requested.
 *
 * 释放 ReorderBufferChange，并在要求时更新内存记账。
 */
void
ReorderBufferFreeChange(ReorderBuffer *rb, ReorderBufferChange *change,
						bool upd_mem)
{
	/* update memory accounting info
	 *
	 * 更新内存记账信息。
	 */
	if (upd_mem)
		ReorderBufferChangeMemoryUpdate(rb, change, NULL, false,
										ReorderBufferChangeSize(change));

	/* free contained data
	 *
	 * 释放其中包含的数据。
	 */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			if (change->data.tp.newtuple)
			{
				ReorderBufferFreeTupleBuf(change->data.tp.newtuple);
				change->data.tp.newtuple = NULL;
			}

			if (change->data.tp.oldtuple)
			{
				ReorderBufferFreeTupleBuf(change->data.tp.oldtuple);
				change->data.tp.oldtuple = NULL;
			}
			break;
		case REORDER_BUFFER_CHANGE_MESSAGE:
			if (change->data.msg.prefix != NULL)
				pfree(change->data.msg.prefix);
			change->data.msg.prefix = NULL;
			if (change->data.msg.message != NULL)
				pfree(change->data.msg.message);
			change->data.msg.message = NULL;
			break;
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			if (change->data.inval.invalidations)
				pfree(change->data.inval.invalidations);
			change->data.inval.invalidations = NULL;
			break;
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			if (change->data.snapshot)
			{
				ReorderBufferFreeSnap(rb, change->data.snapshot);
				change->data.snapshot = NULL;
			}
			break;
			/* no data in addition to the struct itself
			 *
			 * 除结构体本身外没有额外数据。
			 */
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			if (change->data.truncate.relids != NULL)
			{
				ReorderBufferFreeRelids(rb, change->data.truncate.relids);
				change->data.truncate.relids = NULL;
			}
			break;
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			break;
	}

	pfree(change);
}

/*
 * Allocate a HeapTuple fitting a tuple of size tuple_len (excluding header
 * overhead).
 *
 * 分配能容纳 tuple_len 字节元组的 HeapTuple（不含头部开销）。
 */
HeapTuple
ReorderBufferAllocTupleBuf(ReorderBuffer *rb, Size tuple_len)
{
	HeapTuple	tuple;
	Size		alloc_len;

	alloc_len = tuple_len + SizeofHeapTupleHeader;

	tuple = (HeapTuple) MemoryContextAlloc(rb->tup_context,
										   HEAPTUPLESIZE + alloc_len);
	tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	return tuple;
}

/*
 * Free a HeapTuple returned by ReorderBufferAllocTupleBuf().
 *
 * 释放由 ReorderBufferAllocTupleBuf() 返回的 HeapTuple。
 */
void
ReorderBufferFreeTupleBuf(HeapTuple tuple)
{
	pfree(tuple);
}

/*
 * Allocate an array for relids of truncated relations.
 *
 * 为被截断关系的 relid 分配数组。
 *
 * We use the global memory context (for the whole reorder buffer), because
 * none of the existing ones seems like a good match (some are SLAB, so we
 * can't use those, and tup_context is meant for tuple data, not relids). We
 * could add yet another context, but it seems like an overkill - TRUNCATE is
 * not particularly common operation, so it does not seem worth it.
 *
 * 使用整个 reorder buffer 的全局内存上下文，因为现有上下文都不合适：有的是 SLAB，不能用来分配变长数组；
 * tup_context 用于元组数据，不是 relid。可以再加一个上下文，但显得多余。TRUNCATE 并不常见，不值得为此增加上
 * 下文。
 */
Oid *
ReorderBufferAllocRelids(ReorderBuffer *rb, int nrelids)
{
	Oid		   *relids;
	Size		alloc_len;

	alloc_len = sizeof(Oid) * nrelids;

	relids = (Oid *) MemoryContextAlloc(rb->context, alloc_len);

	return relids;
}

/*
 * Free an array of relids.
 *
 * 释放 relid 数组。
 */
void
ReorderBufferFreeRelids(ReorderBuffer *rb, Oid *relids)
{
	pfree(relids);
}

/*
 * Return the ReorderBufferTXN from the given buffer, specified by Xid.
 * If create is true, and a transaction doesn't already exist, create it
 * (with the given LSN, and as top transaction if that's specified);
 * when this happens, is_new is set to true.
 *
 * 按 Xid 返回给定缓冲区中的 ReorderBufferTXN。若 create 为真且事务尚不存在，则创建它（使用给定 LSN；若如此
 * 指定则作为顶层事务）。发生创建时把 is_new 设为真。
 */
static ReorderBufferTXN *
ReorderBufferTXNByXid(ReorderBuffer *rb, TransactionId xid, bool create,
					  bool *is_new, XLogRecPtr lsn, bool create_as_top)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXNByIdEnt *ent;
	bool		found;

	Assert(TransactionIdIsValid(xid));

	/*
	 * Check the one-entry lookup cache first
	 *
	 * 先查只有一项的查找缓存。
	 */
	if (TransactionIdIsValid(rb->by_txn_last_xid) &&
		rb->by_txn_last_xid == xid)
	{
		txn = rb->by_txn_last_txn;

		if (txn != NULL)
		{
			/* found it, and it's valid
			 *
			 * 已经找到，且有效。
			 */
			if (is_new)
				*is_new = false;
			return txn;
		}

		/*
		 * cached as non-existent, and asked not to create? Then nothing else
		 * to do.
		 *
		 * 缓存中记为不存在，且调用方不要求创建，则无需再做别的事。
		 */
		if (!create)
			return NULL;
		/* otherwise fall through to create it
		 *
		 * 否则继续往下创建。
		 */
	}

	/*
	 * If the cache wasn't hit or it yielded a "does-not-exist" and we want to
	 * create an entry.
	 *
	 * 缓存未命中，或命中的是“不存在”，并且我们要创建一项。
	 */

	/* search the lookup table
	 *
	 * 查找哈希表。
	 */
	ent = (ReorderBufferTXNByIdEnt *)
		hash_search(rb->by_txn,
					&xid,
					create ? HASH_ENTER : HASH_FIND,
					&found);
	if (found)
		txn = ent->txn;
	else if (create)
	{
		/* initialize the new entry, if creation was requested
		 *
		 * 若请求了创建，则初始化新项。
		 */
		Assert(ent != NULL);
		Assert(lsn != InvalidXLogRecPtr);

		ent->txn = ReorderBufferAllocTXN(rb);
		ent->txn->xid = xid;
		txn = ent->txn;
		txn->first_lsn = lsn;
		txn->restart_decoding_lsn = rb->current_restart_decoding_lsn;

		if (create_as_top)
		{
			dlist_push_tail(&rb->toplevel_by_lsn, &txn->node);
			AssertTXNLsnOrder(rb);
		}
	}
	else
		txn = NULL;				/* not found and not asked to create
							 *
							 * 未找到，且未要求创建。
							 */

	/* update cache
	 *
	 * 更新缓存。
	 */
	rb->by_txn_last_xid = xid;
	rb->by_txn_last_txn = txn;

	if (is_new)
		*is_new = !found;

	Assert(!create || txn != NULL);
	return txn;
}

/*
 * Record the partial change for the streaming of in-progress transactions.  We
 * can stream only complete changes so if we have a partial change like toast
 * table insert or speculative insert then we mark such a 'txn' so that it
 * can't be streamed.  We also ensure that if the changes in such a 'txn' can
 * be streamed and are above logical_decoding_work_mem threshold then we stream
 * them as soon as we have a complete change.
 *
 * 记录进行中事务流式传输时的不完整变更。只能流式发送完整变更，因此若遇到 toast 表插入或推测性插入这类不完整
 * 变更，就把该 txn 标为不能流式发送。同时，若这类 txn 中的变更已经可以流式发送，并且超过
 * logical_decoding_work_mem，则一旦变更完整就立刻流式发送。
 */
static void
ReorderBufferProcessPartialChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
								  ReorderBufferChange *change,
								  bool toast_insert)
{
	ReorderBufferTXN *toptxn;

	/*
	 * The partial changes need to be processed only while streaming
	 * in-progress transactions.
	 *
	 * 只有在对流式传输进行中的事务时，才需要处理不完整变更。
	 */
	if (!ReorderBufferCanStream(rb))
		return;

	/* Get the top transaction.
	 *
	 * 取得顶层事务。
	 */
	toptxn = rbtxn_get_toptxn(txn);

	/*
	 * Indicate a partial change for toast inserts.  The change will be
	 * considered as complete once we get the insert or update on the main
	 * table and we are sure that the pending toast chunks are not required
	 * anymore.
	 *
	 * 为 toast 插入标出不完整变更。等到主表上的插入或更新到来，并且确认不再需要尚未处理的 toast 块时，
	 * 该变更才算完整。
	 *
	 * If we allow streaming when there are pending toast chunks then such
	 * chunks won't be released till the insert (multi_insert) is complete and
	 * we expect the txn to have streamed all changes after streaming.  This
	 * restriction is mainly to ensure the correctness of streamed
	 * transactions and it doesn't seem worth uplifting such a restriction
	 * just to allow this case because anyway we will stream the transaction
	 * once such an insert is complete.
	 *
	 * 若在仍有未完成 toast 块时允许流式发送，这些块要等到插入（multi_insert）完成才会释放，而我们期望事
	 * 务在流式发送之后已经把全部变更送出。此限制主要是为了流式事务的正确性；仅为支持这种情况而放宽限制
	 * 并不值得，因为这类插入一旦完成，事务反正会被流式发送。
	 */
	if (toast_insert)
		toptxn->txn_flags |= RBTXN_HAS_PARTIAL_CHANGE;
	else if (rbtxn_has_partial_change(toptxn) &&
			 IsInsertOrUpdate(change->action) &&
			 change->data.tp.clear_toast_afterwards)
		toptxn->txn_flags &= ~RBTXN_HAS_PARTIAL_CHANGE;

	/*
	 * Indicate a partial change for speculative inserts.  The change will be
	 * considered as complete once we get the speculative confirm or abort
	 * token.
	 *
	 * 为推测性插入标出不完整变更。收到推测性确认或中止标记后，该变更才算完整。
	 */
	if (IsSpecInsert(change->action))
		toptxn->txn_flags |= RBTXN_HAS_PARTIAL_CHANGE;
	else if (rbtxn_has_partial_change(toptxn) &&
			 IsSpecConfirmOrAbort(change->action))
		toptxn->txn_flags &= ~RBTXN_HAS_PARTIAL_CHANGE;

	/*
	 * Stream the transaction if it is serialized before and the changes are
	 * now complete in the top-level transaction.
	 *
	 * 若事务此前已经序列化，且顶层事务中的变更现已完整，则流式发送该事务。
	 *
	 * The reason for doing the streaming of such a transaction as soon as we
	 * get the complete change for it is that previously it would have reached
	 * the memory threshold and wouldn't get streamed because of incomplete
	 * changes.  Delaying such transactions would increase apply lag for them.
	 *
	 * 一旦变更完整就立刻流式发送，是因为该事务先前已达到内存阈值，却因变更不完整而未能发送。再推迟会加
	 * 大这些事务的应用延迟。
	 */
	if (ReorderBufferCanStartStreaming(rb) &&
		!(rbtxn_has_partial_change(toptxn)) &&
		rbtxn_is_serialized(txn) &&
		rbtxn_has_streamable_change(toptxn))
		ReorderBufferStreamTXN(rb, toptxn);
}

/*
 * Queue a change into a transaction so it can be replayed upon commit or will be
 * streamed when we reach logical_decoding_work_mem threshold.
 *
 * 把一条变更排入事务，以便在提交时重放，或在达到 logical_decoding_work_mem 时流式发送。
 */
void
ReorderBufferQueueChange(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
						 ReorderBufferChange *change, bool toast_insert)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	/*
	 * If we have detected that the transaction is aborted while streaming the
	 * previous changes or by checking its CLOG, there is no point in
	 * collecting further changes for it.
	 *
	 * 若在流式发送先前变更时，或通过查看 CLOG，已经发现该事务已中止，就没有必要再收集后续变更。
	 */
	if (rbtxn_is_aborted(txn))
	{
		/*
		 * We don't need to update memory accounting for this change as we
		 * have not added it to the queue yet.
		 *
		 * 这条变更尚未入队，因此不必为它更新内存记账。
		 */
		ReorderBufferFreeChange(rb, change, false);
		return;
	}

	/*
	 * The changes that are sent downstream are considered streamable.  We
	 * remember such transactions so that only those will later be considered
	 * for streaming.
	 *
	 * 会发往下游的变更视为可流式发送。记住这类事务，稍后只有它们才会被考虑流式发送。
	 */
	if (change->action == REORDER_BUFFER_CHANGE_INSERT ||
		change->action == REORDER_BUFFER_CHANGE_UPDATE ||
		change->action == REORDER_BUFFER_CHANGE_DELETE ||
		change->action == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT ||
		change->action == REORDER_BUFFER_CHANGE_TRUNCATE ||
		change->action == REORDER_BUFFER_CHANGE_MESSAGE)
	{
		ReorderBufferTXN *toptxn = rbtxn_get_toptxn(txn);

		toptxn->txn_flags |= RBTXN_HAS_STREAMABLE_CHANGE;
	}

	change->lsn = lsn;
	change->txn = txn;

	Assert(InvalidXLogRecPtr != lsn);
	dlist_push_tail(&txn->changes, &change->node);
	txn->nentries++;
	txn->nentries_mem++;

	/* update memory accounting information
	 *
	 * 更新内存记账信息。
	 */
	ReorderBufferChangeMemoryUpdate(rb, change, NULL, true,
									ReorderBufferChangeSize(change));

	/* process partial change
	 *
	 * 处理不完整变更。
	 */
	ReorderBufferProcessPartialChange(rb, txn, change, toast_insert);

	/* check the memory limits and evict something if needed
	 *
	 * 检查内存上限，必要时逐出一部分数据。
	 */
	ReorderBufferCheckMemoryLimit(rb);
}

/*
 * A transactional message is queued to be processed upon commit and a
 * non-transactional message gets processed immediately.
 *
 * 事务性消息先排队，到提交时再处理；非事务性消息立即处理。
 */
void
ReorderBufferQueueMessage(ReorderBuffer *rb, TransactionId xid,
						  Snapshot snap, XLogRecPtr lsn,
						  bool transactional, const char *prefix,
						  Size message_size, const char *message)
{
	if (transactional)
	{
		MemoryContext oldcontext;
		ReorderBufferChange *change;

		Assert(xid != InvalidTransactionId);

		/*
		 * We don't expect snapshots for transactional changes - we'll use the
		 * snapshot derived later during apply (unless the change gets
		 * skipped).
		 *
		 * 事务性变更不带快照；稍后应用时再使用推导出的快照（除非该变更被跳过）。
		 */
		Assert(!snap);

		oldcontext = MemoryContextSwitchTo(rb->context);

		change = ReorderBufferAllocChange(rb);
		change->action = REORDER_BUFFER_CHANGE_MESSAGE;
		change->data.msg.prefix = pstrdup(prefix);
		change->data.msg.message_size = message_size;
		change->data.msg.message = palloc(message_size);
		memcpy(change->data.msg.message, message, message_size);

		ReorderBufferQueueChange(rb, xid, lsn, change, false);

		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		ReorderBufferTXN *txn = NULL;
		volatile Snapshot snapshot_now = snap;

		/* Non-transactional changes require a valid snapshot.
		 *
		 * 非事务性变更需要有效快照。
		 */
		Assert(snapshot_now);

		if (xid != InvalidTransactionId)
			txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

		/* setup snapshot to allow catalog access
		 *
		 * 设置快照，以便访问系统目录。
		 */
		SetupHistoricSnapshot(snapshot_now, NULL);
		PG_TRY();
		{
			rb->message(rb, txn, lsn, false, prefix, message_size, message);

			TeardownHistoricSnapshot(false);
		}
		PG_CATCH();
		{
			TeardownHistoricSnapshot(true);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
}

/*
 * AssertTXNLsnOrder
 *		Verify LSN ordering of transaction lists in the reorderbuffer
 *
 *		AssertTXNLsnOrder：校验 reorderbuffer 中事务链表的 LSN 顺序。
 *
 * Other LSN-related invariants are checked too.
 *
 * 同时检查其他与 LSN 相关的不变量。
 *
 * No-op if assertions are not in use.
 *
 * 未启用断言时为空操作。
 */
static void
AssertTXNLsnOrder(ReorderBuffer *rb)
{
#ifdef USE_ASSERT_CHECKING
	LogicalDecodingContext *ctx = rb->private_data;
	dlist_iter	iter;
	XLogRecPtr	prev_first_lsn = InvalidXLogRecPtr;
	XLogRecPtr	prev_base_snap_lsn = InvalidXLogRecPtr;

	/*
	 * Skip the verification if we don't reach the LSN at which we start
	 * decoding the contents of transactions yet because until we reach the
	 * LSN, we could have transactions that don't have the association between
	 * the top-level transaction and subtransaction yet and consequently have
	 * the same LSN.  We don't guarantee this association until we try to
	 * decode the actual contents of transaction. The ordering of the records
	 * prior to the start_decoding_at LSN should have been checked before the
	 * restart.
	 *
	 * 若尚未到达开始解码事务内容的 LSN，则跳过校验。在那之前，顶层事务与子事务可能还没有关联，因而可能
	 * 具有相同的 LSN。直到真正解码事务内容时才保证这一关联。start_decoding_at 之前记录的顺序，应已在重
	 * 启前检查过。
	 */
	if (SnapBuildXactNeedsSkip(ctx->snapshot_builder, ctx->reader->EndRecPtr))
		return;

	dlist_foreach(iter, &rb->toplevel_by_lsn)
	{
		ReorderBufferTXN *cur_txn = dlist_container(ReorderBufferTXN, node,
													iter.cur);

		/* start LSN must be set
		 *
		 * 起始 LSN 必须已经设置。
		 */
		Assert(cur_txn->first_lsn != InvalidXLogRecPtr);

		/* If there is an end LSN, it must be higher than start LSN
		 *
		 * 若有结束 LSN，它必须不低于起始 LSN。
		 */
		if (cur_txn->end_lsn != InvalidXLogRecPtr)
			Assert(cur_txn->first_lsn <= cur_txn->end_lsn);

		/* Current initial LSN must be strictly higher than previous
		 *
		 * 当前起始 LSN 必须严格大于前一个。
		 */
		if (prev_first_lsn != InvalidXLogRecPtr)
			Assert(prev_first_lsn < cur_txn->first_lsn);

		/* known-as-subtxn txns must not be listed
		 *
		 * 已确认为子事务的事务不得出现在此链表中。
		 */
		Assert(!rbtxn_is_known_subxact(cur_txn));

		prev_first_lsn = cur_txn->first_lsn;
	}

	dlist_foreach(iter, &rb->txns_by_base_snapshot_lsn)
	{
		ReorderBufferTXN *cur_txn = dlist_container(ReorderBufferTXN,
													base_snapshot_node,
													iter.cur);

		/* base snapshot (and its LSN) must be set
		 *
		 * 基础快照及其 LSN 必须已经设置。
		 */
		Assert(cur_txn->base_snapshot != NULL);
		Assert(cur_txn->base_snapshot_lsn != InvalidXLogRecPtr);

		/* current LSN must be strictly higher than previous
		 *
		 * 当前 LSN 必须严格大于前一个。
		 */
		if (prev_base_snap_lsn != InvalidXLogRecPtr)
			Assert(prev_base_snap_lsn < cur_txn->base_snapshot_lsn);

		/* known-as-subtxn txns must not be listed
		 *
		 * 已确认为子事务的事务不得出现在此链表中。
		 */
		Assert(!rbtxn_is_known_subxact(cur_txn));

		prev_base_snap_lsn = cur_txn->base_snapshot_lsn;
	}
#endif
}

/*
 * AssertChangeLsnOrder
 *
 * 函数 AssertChangeLsnOrder。
 *
 * Check ordering of changes in the (sub)transaction.
 *
 * 检查（子）事务中变更的顺序。
 */
static void
AssertChangeLsnOrder(ReorderBufferTXN *txn)
{
#ifdef USE_ASSERT_CHECKING
	dlist_iter	iter;
	XLogRecPtr	prev_lsn = txn->first_lsn;

	dlist_foreach(iter, &txn->changes)
	{
		ReorderBufferChange *cur_change;

		cur_change = dlist_container(ReorderBufferChange, node, iter.cur);

		Assert(txn->first_lsn != InvalidXLogRecPtr);
		Assert(cur_change->lsn != InvalidXLogRecPtr);
		Assert(txn->first_lsn <= cur_change->lsn);

		if (txn->end_lsn != InvalidXLogRecPtr)
			Assert(cur_change->lsn <= txn->end_lsn);

		Assert(prev_lsn <= cur_change->lsn);

		prev_lsn = cur_change->lsn;
	}
#endif
}

/*
 * ReorderBufferGetOldestTXN
 *		Return oldest transaction in reorderbuffer
 *
 *		ReorderBufferGetOldestTXN：返回 reorderbuffer 中最老的事务。
 */
ReorderBufferTXN *
ReorderBufferGetOldestTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	AssertTXNLsnOrder(rb);

	if (dlist_is_empty(&rb->toplevel_by_lsn))
		return NULL;

	txn = dlist_head_element(ReorderBufferTXN, node, &rb->toplevel_by_lsn);

	Assert(!rbtxn_is_known_subxact(txn));
	Assert(txn->first_lsn != InvalidXLogRecPtr);
	return txn;
}

/*
 * ReorderBufferGetOldestXmin
 *		Return oldest Xmin in reorderbuffer
 *
 *		ReorderBufferGetOldestXmin：返回 reorderbuffer 中最老的 Xmin。
 *
 * Returns oldest possibly running Xid from the point of view of snapshots
 * used in the transactions kept by reorderbuffer, or InvalidTransactionId if
 * there are none.
 *
 * 从 reorderbuffer 所保存事务的快照来看，返回可能仍在运行的最老 Xid；若没有则返回 InvalidTransactionId。
 *
 * Since snapshots are assigned monotonically, this equals the Xmin of the
 * base snapshot with minimal base_snapshot_lsn.
 *
 * 快照是单调分配的，因此这等于 base_snapshot_lsn 最小的那个基础快照的 Xmin。
 */
TransactionId
ReorderBufferGetOldestXmin(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	AssertTXNLsnOrder(rb);

	if (dlist_is_empty(&rb->txns_by_base_snapshot_lsn))
		return InvalidTransactionId;

	txn = dlist_head_element(ReorderBufferTXN, base_snapshot_node,
							 &rb->txns_by_base_snapshot_lsn);
	return txn->base_snapshot->xmin;
}

/*
 * 记录当前逻辑解码重启点 LSN，供此后创建的事务保存 restart_decoding_lsn。
 */
void
ReorderBufferSetRestartPoint(ReorderBuffer *rb, XLogRecPtr ptr)
{
	rb->current_restart_decoding_lsn = ptr;
}

/*
 * ReorderBufferAssignChild
 *
 * 函数 ReorderBufferAssignChild。
 *
 * Make note that we know that subxid is a subtransaction of xid, seen as of
 * the given lsn.
 *
 * 记录 subxid 是 xid 的子事务，该事实在给定 lsn 处可见。
 */
void
ReorderBufferAssignChild(ReorderBuffer *rb, TransactionId xid,
						 TransactionId subxid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXN *subtxn;
	bool		new_top;
	bool		new_sub;

	txn = ReorderBufferTXNByXid(rb, xid, true, &new_top, lsn, true);
	subtxn = ReorderBufferTXNByXid(rb, subxid, true, &new_sub, lsn, false);

	if (!new_sub)
	{
		if (rbtxn_is_known_subxact(subtxn))
		{
			/* already associated, nothing to do
			 *
			 * 已经关联过，无需再做。
			 */
			return;
		}
		else
		{
			/*
			 * We already saw this transaction, but initially added it to the
			 * list of top-level txns.  Now that we know it's not top-level,
			 * remove it from there.
			 *
			 * 先前已经见过该事务，并把它放进了顶层事务链表。现在知道它不是顶层事务，于是从那里
			 * 移除。
			 */
			dlist_delete(&subtxn->node);
		}
	}

	subtxn->txn_flags |= RBTXN_IS_SUBXACT;
	subtxn->toplevel_xid = xid;
	Assert(subtxn->nsubtxns == 0);

	/* set the reference to top-level transaction
	 *
	 * 设置指向顶层事务的引用。
	 */
	subtxn->toptxn = txn;

	/* add to subtransaction list
	 *
	 * 加入子事务链表。
	 */
	dlist_push_tail(&txn->subtxns, &subtxn->node);
	txn->nsubtxns++;

	/* Possibly transfer the subtxn's snapshot to its top-level txn.
	 *
	 * 如有需要，把子事务的快照转移给它的顶层事务。
	 */
	ReorderBufferTransferSnapToParent(txn, subtxn);

	/* Verify LSN-ordering invariant
	 *
	 * 校验 LSN 顺序不变量。
	 */
	AssertTXNLsnOrder(rb);
}

/*
 * ReorderBufferTransferSnapToParent
 *		Transfer base snapshot from subtxn to top-level txn, if needed
 *
 *		ReorderBufferTransferSnapToParent：在需要时把子事务的基础快照转移给顶层事务。
 *
 * This is done if the top-level txn doesn't have a base snapshot, or if the
 * subtxn's base snapshot has an earlier LSN than the top-level txn's base
 * snapshot's LSN.  This can happen if there are no changes in the toplevel
 * txn but there are some in the subtxn, or the first change in subtxn has
 * earlier LSN than first change in the top-level txn and we learned about
 * their kinship only now.
 *
 * 在顶层事务还没有基础快照，或者子事务基础快照的 LSN 早于顶层事务基础快照的 LSN 时进行转移。可能的情况是：
 * 顶层事务没有变更而子事务有，或者子事务的第一条变更 LSN 更早，而我们直到现在才知道它们的亲缘关系。
 *
 * The subtransaction's snapshot is cleared regardless of the transfer
 * happening, since it's not needed anymore in either case.
 *
 * 无论是否发生转移，都清掉子事务的快照，因为两种情况下都不再需要它。
 *
 * We do this as soon as we become aware of their kinship, to avoid queueing
 * extra snapshots to txns known-as-subtxns -- only top-level txns will
 * receive further snapshots.
 *
 * 一旦知道亲缘关系就立刻做这件事，以免再把额外快照排给已知的子事务。之后只有顶层事务才会继续收到快照。
 */
static void
ReorderBufferTransferSnapToParent(ReorderBufferTXN *txn,
								  ReorderBufferTXN *subtxn)
{
	Assert(subtxn->toplevel_xid == txn->xid);

	if (subtxn->base_snapshot != NULL)
	{
		if (txn->base_snapshot == NULL ||
			subtxn->base_snapshot_lsn < txn->base_snapshot_lsn)
		{
			/*
			 * If the toplevel transaction already has a base snapshot but
			 * it's newer than the subxact's, purge it.
			 *
			 * 若顶层事务已有基础快照，但比子事务的更新，则丢弃它。
			 */
			if (txn->base_snapshot != NULL)
			{
				SnapBuildSnapDecRefcount(txn->base_snapshot);
				dlist_delete(&txn->base_snapshot_node);
			}

			/*
			 * The snapshot is now the top transaction's; transfer it, and
			 * adjust the list position of the top transaction in the list by
			 * moving it to where the subtransaction is.
			 *
			 * 该快照现在属于顶层事务：转移它，并把顶层事务在链表中的位置调到子事务所在之处。
			 */
			txn->base_snapshot = subtxn->base_snapshot;
			txn->base_snapshot_lsn = subtxn->base_snapshot_lsn;
			dlist_insert_before(&subtxn->base_snapshot_node,
								&txn->base_snapshot_node);

			/*
			 * The subtransaction doesn't have a snapshot anymore (so it
			 * mustn't be in the list.)
			 *
			 * 子事务不再持有快照（因此它不能再留在该链表中）。
			 */
			subtxn->base_snapshot = NULL;
			subtxn->base_snapshot_lsn = InvalidXLogRecPtr;
			dlist_delete(&subtxn->base_snapshot_node);
		}
		else
		{
			/* Base snap of toplevel is fine, so subxact's is not needed
			 *
			 * 顶层事务的基础快照已经合适，因此不再需要子事务的快照。
			 */
			SnapBuildSnapDecRefcount(subtxn->base_snapshot);
			dlist_delete(&subtxn->base_snapshot_node);
			subtxn->base_snapshot = NULL;
			subtxn->base_snapshot_lsn = InvalidXLogRecPtr;
		}
	}
}

/*
 * Associate a subtransaction with its toplevel transaction at commit
 * time. There may be no further changes added after this.
 *
 * 在提交时把子事务关联到它的顶层事务。此后不应再追加变更。
 */
void
ReorderBufferCommitChild(ReorderBuffer *rb, TransactionId xid,
						 TransactionId subxid, XLogRecPtr commit_lsn,
						 XLogRecPtr end_lsn)
{
	ReorderBufferTXN *subtxn;

	subtxn = ReorderBufferTXNByXid(rb, subxid, false, NULL,
								   InvalidXLogRecPtr, false);

	/*
	 * No need to do anything if that subtxn didn't contain any changes
	 *
	 * 若该子事务没有任何变更，则无需处理。
	 */
	if (!subtxn)
		return;

	subtxn->final_lsn = commit_lsn;
	subtxn->end_lsn = end_lsn;

	/*
	 * Assign this subxact as a child of the toplevel xact (no-op if already
	 * done.)
	 *
	 * 把该子事务登记为顶层事务的子事务（若已经登记则无操作）。
	 */
	ReorderBufferAssignChild(rb, xid, subxid, InvalidXLogRecPtr);
}


/*
 * Support for efficiently iterating over a transaction's and its
 * subtransactions' changes.
 *
 * 高效遍历一个事务及其子事务变更的支持代码。
 *
 * We do by doing a k-way merge between transactions/subtransactions. For that
 * we model the current heads of the different transactions as a binary heap
 * so we easily know which (sub-)transaction has the change with the smallest
 * lsn next.
 *
 * 做法是在事务与子事务之间做 k 路归并。把各事务当前的头部放进二叉堆，从而容易知道下一个最小 LSN 的变更属于
 * 哪个（子）事务。
 *
 * We assume the changes in individual transactions are already sorted by LSN.
 *
 * 假定单个事务内的变更已经按 LSN 排好序。
 */

/*
 * Binary heap comparison function.
 *
 * 二叉堆的比较函数。
 */
static int
ReorderBufferIterCompare(Datum a, Datum b, void *arg)
{
	ReorderBufferIterTXNState *state = (ReorderBufferIterTXNState *) arg;
	XLogRecPtr	pos_a = state->entries[DatumGetInt32(a)].lsn;
	XLogRecPtr	pos_b = state->entries[DatumGetInt32(b)].lsn;

	if (pos_a < pos_b)
		return 1;
	else if (pos_a == pos_b)
		return 0;
	return -1;
}

/*
 * Allocate & initialize an iterator which iterates in lsn order over a
 * transaction and all its subtransactions.
 *
 * 分配并初始化一个迭代器，按 LSN 顺序遍历一个事务及其全部子事务。
 *
 * Note: The iterator state is returned through iter_state parameter rather
 * than the function's return value.  This is because the state gets cleaned up
 * in a PG_CATCH block in the caller, so we want to make sure the caller gets
 * back the state even if this function throws an exception.
 *
 * 迭代器状态通过 iter_state 参数返回，而不是作为函数返回值。因为调用方会在 PG_CATCH 块里清理该状态，所以即
 * 使本函数抛出异常，也要保证调用方已经拿到状态。
 */
static void
ReorderBufferIterTXNInit(ReorderBuffer *rb, ReorderBufferTXN *txn,
						 ReorderBufferIterTXNState *volatile *iter_state)
{
	Size		nr_txns = 0;
	ReorderBufferIterTXNState *state;
	dlist_iter	cur_txn_i;
	int32		off;

	*iter_state = NULL;

	/* Check ordering of changes in the toplevel transaction.
	 *
	 * 检查顶层事务中变更的顺序。
	 */
	AssertChangeLsnOrder(txn);

	/*
	 * Calculate the size of our heap: one element for every transaction that
	 * contains changes.  (Besides the transactions already in the reorder
	 * buffer, we count the one we were directly passed.)
	 *
	 * 计算堆的大小：每个含有变更的事务占一个元素。（除了 reorder buffer 里已有的事务，还计入直接传入的
	 * 那一个。）
	 */
	if (txn->nentries > 0)
		nr_txns++;

	dlist_foreach(cur_txn_i, &txn->subtxns)
	{
		ReorderBufferTXN *cur_txn;

		cur_txn = dlist_container(ReorderBufferTXN, node, cur_txn_i.cur);

		/* Check ordering of changes in this subtransaction.
		 *
		 * 检查该子事务中变更的顺序。
		 */
		AssertChangeLsnOrder(cur_txn);

		if (cur_txn->nentries > 0)
			nr_txns++;
	}

	/* allocate iteration state
	 *
	 * 分配迭代状态。
	 */
	state = (ReorderBufferIterTXNState *)
		MemoryContextAllocZero(rb->context,
							   sizeof(ReorderBufferIterTXNState) +
							   sizeof(ReorderBufferIterTXNEntry) * nr_txns);

	state->nr_txns = nr_txns;
	dlist_init(&state->old_change);

	for (off = 0; off < state->nr_txns; off++)
	{
		state->entries[off].file.vfd = -1;
		state->entries[off].segno = 0;
	}

	/* allocate heap
	 *
	 * 分配堆。
	 */
	state->heap = binaryheap_allocate(state->nr_txns,
									  ReorderBufferIterCompare,
									  state);

	/* Now that the state fields are initialized, it is safe to return it.
	 *
	 * 状态字段已经初始化，现在可以把它返回给调用方。
	 */
	*iter_state = state;

	/*
	 * Now insert items into the binary heap, in an unordered fashion.  (We
	 * will run a heap assembly step at the end; this is more efficient.)
	 *
	 * 现在以无序方式把元素插入二叉堆。（最后再做一次建堆，这样更高效。）
	 */

	off = 0;

	/* add toplevel transaction if it contains changes
	 *
	 * 若顶层事务含有变更，则把它加入。
	 */
	if (txn->nentries > 0)
	{
		ReorderBufferChange *cur_change;

		if (rbtxn_is_serialized(txn))
		{
			/* serialize remaining changes
			 *
			 * 把剩余变更序列化出去。
			 */
			ReorderBufferSerializeTXN(rb, txn);
			ReorderBufferRestoreChanges(rb, txn, &state->entries[off].file,
										&state->entries[off].segno);
		}

		cur_change = dlist_head_element(ReorderBufferChange, node,
										&txn->changes);

		state->entries[off].lsn = cur_change->lsn;
		state->entries[off].change = cur_change;
		state->entries[off].txn = txn;

		binaryheap_add_unordered(state->heap, Int32GetDatum(off++));
	}

	/* add subtransactions if they contain changes
	 *
	 * 若子事务含有变更，则把它们加入。
	 */
	dlist_foreach(cur_txn_i, &txn->subtxns)
	{
		ReorderBufferTXN *cur_txn;

		cur_txn = dlist_container(ReorderBufferTXN, node, cur_txn_i.cur);

		if (cur_txn->nentries > 0)
		{
			ReorderBufferChange *cur_change;

			if (rbtxn_is_serialized(cur_txn))
			{
				/* serialize remaining changes
				 *
				 * 把剩余变更序列化出去。
				 */
				ReorderBufferSerializeTXN(rb, cur_txn);
				ReorderBufferRestoreChanges(rb, cur_txn,
											&state->entries[off].file,
											&state->entries[off].segno);
			}
			cur_change = dlist_head_element(ReorderBufferChange, node,
											&cur_txn->changes);

			state->entries[off].lsn = cur_change->lsn;
			state->entries[off].change = cur_change;
			state->entries[off].txn = cur_txn;

			binaryheap_add_unordered(state->heap, Int32GetDatum(off++));
		}
	}

	/* assemble a valid binary heap
	 *
	 * 组装成一个有效的二叉堆。
	 */
	binaryheap_build(state->heap);
}

/*
 * Return the next change when iterating over a transaction and its
 * subtransactions.
 *
 * 在遍历事务及其子事务时返回下一条变更。
 *
 * Returns NULL when no further changes exist.
 *
 * 没有更多变更时返回 NULL。
 */
static ReorderBufferChange *
ReorderBufferIterTXNNext(ReorderBuffer *rb, ReorderBufferIterTXNState *state)
{
	ReorderBufferChange *change;
	ReorderBufferIterTXNEntry *entry;
	int32		off;

	/* nothing there anymore
	 *
	 * 已经没有内容了。
	 */
	if (state->heap->bh_size == 0)
		return NULL;

	off = DatumGetInt32(binaryheap_first(state->heap));
	entry = &state->entries[off];

	/* free memory we might have "leaked" in the previous *Next call
	 *
	 * 释放上一次 *Next 调用中可能“泄漏”的内存。
	 */
	if (!dlist_is_empty(&state->old_change))
	{
		change = dlist_container(ReorderBufferChange, node,
								 dlist_pop_head_node(&state->old_change));
		ReorderBufferFreeChange(rb, change, true);
		Assert(dlist_is_empty(&state->old_change));
	}

	change = entry->change;

	/*
	 * update heap with information about which transaction has the next
	 * relevant change in LSN order
	 *
	 * 根据哪个事务拥有按 LSN 顺序的下一条相关变更，更新堆。
	 */

	/* there are in-memory changes
	 *
	 * 内存中还有变更。
	 */
	if (dlist_has_next(&entry->txn->changes, &entry->change->node))
	{
		dlist_node *next = dlist_next_node(&entry->txn->changes, &change->node);
		ReorderBufferChange *next_change =
			dlist_container(ReorderBufferChange, node, next);

		/* txn stays the same
		 *
		 * 事务保持不变。
		 */
		state->entries[off].lsn = next_change->lsn;
		state->entries[off].change = next_change;

		binaryheap_replace_first(state->heap, Int32GetDatum(off));
		return change;
	}

	/* try to load changes from disk
	 *
	 * 尝试从磁盘装入变更。
	 */
	if (entry->txn->nentries != entry->txn->nentries_mem)
	{
		/*
		 * Ugly: restoring changes will reuse *Change records, thus delete the
		 * current one from the per-tx list and only free in the next call.
		 *
		 * 不够优雅：恢复变更会复用 Change 记录，因此先把当前记录从该事务链表中摘下，留到下一次调用
		 * 再释放。
		 */
		dlist_delete(&change->node);
		dlist_push_tail(&state->old_change, &change->node);

		/*
		 * Update the total bytes processed by the txn for which we are
		 * releasing the current set of changes and restoring the new set of
		 * changes.
		 *
		 * 更新该事务已处理的总字节数：我们正在释放当前这一批变更，并恢复下一批。
		 */
		rb->totalBytes += entry->txn->size;
		if (ReorderBufferRestoreChanges(rb, entry->txn, &entry->file,
										&state->entries[off].segno))
		{
			/* successfully restored changes from disk
			 *
			 * 已成功从磁盘恢复变更。
			 */
			ReorderBufferChange *next_change =
				dlist_head_element(ReorderBufferChange, node,
								   &entry->txn->changes);

			elog(DEBUG2, "restored %u/%u changes from disk",
				 (uint32) entry->txn->nentries_mem,
				 (uint32) entry->txn->nentries);

			Assert(entry->txn->nentries_mem);
			/* txn stays the same
			 *
			 * 事务保持不变。
			 */
			state->entries[off].lsn = next_change->lsn;
			state->entries[off].change = next_change;
			binaryheap_replace_first(state->heap, Int32GetDatum(off));

			return change;
		}
	}

	/* ok, no changes there anymore, remove
	 *
	 * 这里已经没有更多变更，移除它。
	 */
	binaryheap_remove_first(state->heap);

	return change;
}

/*
 * Deallocate the iterator
 *
 * 释放迭代器。
 */
static void
ReorderBufferIterTXNFinish(ReorderBuffer *rb,
						   ReorderBufferIterTXNState *state)
{
	int32		off;

	for (off = 0; off < state->nr_txns; off++)
	{
		if (state->entries[off].file.vfd != -1)
			FileClose(state->entries[off].file.vfd);
	}

	/* free memory we might have "leaked" in the last *Next call
	 *
	 * 释放最后一次 *Next 调用中可能“泄漏”的内存。
	 */
	if (!dlist_is_empty(&state->old_change))
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node,
								 dlist_pop_head_node(&state->old_change));
		ReorderBufferFreeChange(rb, change, true);
		Assert(dlist_is_empty(&state->old_change));
	}

	binaryheap_free(state->heap);
	pfree(state);
}

/*
 * Cleanup the contents of a transaction, usually after the transaction
 * committed or aborted.
 *
 * 清理事务内容，通常在事务提交或中止之后。
 */
static void
ReorderBufferCleanupTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	bool		found;
	dlist_mutable_iter iter;
	Size		mem_freed = 0;

	/* cleanup subtransactions & their changes
	 *
	 * 清理子事务及其变更。
	 */
	dlist_foreach_modify(iter, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, iter.cur);

		/*
		 * Subtransactions are always associated to the toplevel TXN, even if
		 * they originally were happening inside another subtxn, so we won't
		 * ever recurse more than one level deep here.
		 *
		 * 子事务总是直接挂在顶层 TXN 上，即使它们最初发生在另一个子事务内部，因此这里递归不会超过一
		 * 层。
		 */
		Assert(rbtxn_is_known_subxact(subtxn));
		Assert(subtxn->nsubtxns == 0);

		ReorderBufferCleanupTXN(rb, subtxn);
	}

	/* cleanup changes in the txn
	 *
	 * 清理该事务中的变更。
	 */
	dlist_foreach_modify(iter, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		/* Check we're not mixing changes from different transactions.
		 *
		 * 确认没有把不同事务的变更混在一起。
		 */
		Assert(change->txn == txn);

		/*
		 * Instead of updating the memory counter for individual changes, we
		 * sum up the size of memory to free so we can update the memory
		 * counter all together below. This saves costs of maintaining the
		 * max-heap.
		 *
		 * 不逐条更新内存计数，而是先把要释放的内存大小加总，再在下面一次性更新计数。这样可以省去维
		 * 护最大堆的开销。
		 */
		mem_freed += ReorderBufferChangeSize(change);

		ReorderBufferFreeChange(rb, change, false);
	}

	/* Update the memory counter
	 *
	 * 更新内存计数。
	 */
	ReorderBufferChangeMemoryUpdate(rb, NULL, txn, false, mem_freed);

	/*
	 * Cleanup the tuplecids we stored for decoding catalog snapshot access.
	 * They are always stored in the toplevel transaction.
	 *
	 * 清理为解码目录快照访问而保存的 tuplecid。它们总是存放在顶层事务中。
	 */
	dlist_foreach_modify(iter, &txn->tuplecids)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		/* Check we're not mixing changes from different transactions.
		 *
		 * 确认没有把不同事务的变更混在一起。
		 */
		Assert(change->txn == txn);
		Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

		ReorderBufferFreeChange(rb, change, true);
	}

	/*
	 * Cleanup the base snapshot, if set.
	 *
	 * 若设置了基础快照，则清理它。
	 */
	if (txn->base_snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(txn->base_snapshot);
		dlist_delete(&txn->base_snapshot_node);
	}

	/*
	 * Cleanup the snapshot for the last streamed run.
	 *
	 * 清理上一次流式传输所用的快照。
	 */
	if (txn->snapshot_now != NULL)
	{
		Assert(rbtxn_is_streamed(txn));
		ReorderBufferFreeSnap(rb, txn->snapshot_now);
	}

	/*
	 * Remove TXN from its containing lists.
	 *
	 * 把 TXN 从包含它的链表中移除。
	 *
	 * Note: if txn is known as subxact, we are deleting the TXN from its
	 * parent's list of known subxacts; this leaves the parent's nsubxacts
	 * count too high, but we don't care.  Otherwise, we are deleting the TXN
	 * from the LSN-ordered list of toplevel TXNs. We remove the TXN from the
	 * list of catalog modifying transactions as well.
	 *
	 * 若 txn 已知是子事务，这里是从父事务的已知子事务链表中删除它；这会使父事务的 nsubtxns 偏大，但我们
	 * 不在意。否则，是从按 LSN 排序的顶层 TXN 链表中删除。同时也会把它从修改了系统目录的事务链表中移除。
	 */
	dlist_delete(&txn->node);
	if (rbtxn_has_catalog_changes(txn))
		dclist_delete_from(&rb->catchange_txns, &txn->catchange_node);

	/* now remove reference from buffer
	 *
	 * 现在从缓冲区中去掉对该事务的引用。
	 */
	hash_search(rb->by_txn, &txn->xid, HASH_REMOVE, &found);
	Assert(found);

	/* remove entries spilled to disk
	 *
	 * 删除溢出到磁盘的内容。
	 */
	if (rbtxn_is_serialized(txn))
		ReorderBufferRestoreCleanup(rb, txn);

	/* deallocate
	 *
	 * 释放该事务。
	 */
	ReorderBufferFreeTXN(rb, txn);
}

/*
 * Discard changes from a transaction (and subtransactions), either after
 * streaming, decoding them at PREPARE, or detecting the transaction abort.
 * Keep the remaining info - transactions, tuplecids, invalidations and
 * snapshots.
 *
 * 在流式发送之后、在 PREPARE 处解码之后，或发现事务中止时，丢弃事务（及子事务）的变更。保留其余信息：事务本
 * 身、tuplecid、失效消息和快照。
 *
 * We additionally remove tuplecids after decoding the transaction at prepare
 * time as we only need to perform invalidation at rollback or commit prepared.
 *
 * 在 prepare 时解码完事务后，还要额外去掉 tuplecid，因为回滚或 commit prepared 时只需要执行失效。
 *
 * 'txn_prepared' indicates that we have decoded the transaction at prepare
 * time.
 *
 * 'txn_prepared' 表示我们已经在 prepare 时解码了该事务。
 */
static void
ReorderBufferTruncateTXN(ReorderBuffer *rb, ReorderBufferTXN *txn, bool txn_prepared)
{
	dlist_mutable_iter iter;
	Size		mem_freed = 0;

	/* cleanup subtransactions & their changes
	 *
	 * 清理子事务及其变更。
	 */
	dlist_foreach_modify(iter, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, iter.cur);

		/*
		 * Subtransactions are always associated to the toplevel TXN, even if
		 * they originally were happening inside another subtxn, so we won't
		 * ever recurse more than one level deep here.
		 *
		 * 子事务总是直接挂在顶层 TXN 上，即使它们最初发生在另一个子事务内部，因此这里递归不会超过一
		 * 层。
		 */
		Assert(rbtxn_is_known_subxact(subtxn));
		Assert(subtxn->nsubtxns == 0);

		ReorderBufferMaybeMarkTXNStreamed(rb, subtxn);
		ReorderBufferTruncateTXN(rb, subtxn, txn_prepared);
	}

	/* cleanup changes in the txn
	 *
	 * 清理该事务中的变更。
	 */
	dlist_foreach_modify(iter, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		/* Check we're not mixing changes from different transactions.
		 *
		 * 确认没有把不同事务的变更混在一起。
		 */
		Assert(change->txn == txn);

		/* remove the change from its containing list
		 *
		 * 把该变更从包含它的链表中移除。
		 */
		dlist_delete(&change->node);

		/*
		 * Instead of updating the memory counter for individual changes, we
		 * sum up the size of memory to free so we can update the memory
		 * counter all together below. This saves costs of maintaining the
		 * max-heap.
		 *
		 * 不逐条更新内存计数，而是先把要释放的内存大小加总，再在下面一次性更新计数。这样可以省去维
		 * 护最大堆的开销。
		 */
		mem_freed += ReorderBufferChangeSize(change);

		ReorderBufferFreeChange(rb, change, false);
	}

	/* Update the memory counter
	 *
	 * 更新内存计数。
	 */
	ReorderBufferChangeMemoryUpdate(rb, NULL, txn, false, mem_freed);

	if (txn_prepared)
	{
		/*
		 * If this is a prepared txn, cleanup the tuplecids we stored for
		 * decoding catalog snapshot access. They are always stored in the
		 * toplevel transaction.
		 *
		 * 若这是已准备的事务，清掉为解码目录快照访问而保存的 tuplecid。它们总是存放在顶层事务中。
		 */
		dlist_foreach_modify(iter, &txn->tuplecids)
		{
			ReorderBufferChange *change;

			change = dlist_container(ReorderBufferChange, node, iter.cur);

			/* Check we're not mixing changes from different transactions.
			 *
			 * 确认没有把不同事务的变更混在一起。
			 */
			Assert(change->txn == txn);
			Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

			/* Remove the change from its containing list.
			 *
			 * 把该变更从包含它的链表中移除。
			 */
			dlist_delete(&change->node);

			ReorderBufferFreeChange(rb, change, true);
		}
	}

	/*
	 * Destroy the (relfilelocator, ctid) hashtable, so that we don't leak any
	 * memory. We could also keep the hash table and update it with new ctid
	 * values, but this seems simpler and good enough for now.
	 *
	 * 销毁 (relfilelocator, ctid) 哈希表，以免泄漏内存。也可以保留哈希表并用新的 ctid 更新它，但目前这
	 * 样更简单，也够用。
	 */
	if (txn->tuplecid_hash != NULL)
	{
		hash_destroy(txn->tuplecid_hash);
		txn->tuplecid_hash = NULL;
	}

	/* If this txn is serialized then clean the disk space.
	 *
	 * 若该事务已序列化，则清理其磁盘空间。
	 */
	if (rbtxn_is_serialized(txn))
	{
		ReorderBufferRestoreCleanup(rb, txn);
		txn->txn_flags &= ~RBTXN_IS_SERIALIZED;

		/*
		 * We set this flag to indicate if the transaction is ever serialized.
		 * We need this to accurately update the stats as otherwise the same
		 * transaction can be counted as serialized multiple times.
		 *
		 * 用这个标志记录该事务是否曾经被序列化。否则同一事务可能被多次计入序列化统计。
		 */
		txn->txn_flags |= RBTXN_IS_SERIALIZED_CLEAR;
	}

	/* also reset the number of entries in the transaction
	 *
	 * 同时重置事务中的条目数量。
	 */
	txn->nentries_mem = 0;
	txn->nentries = 0;
}

/*
 * Check the transaction status by CLOG lookup and discard all changes if
 * the transaction is aborted. The transaction status is cached in
 * txn->txn_flags so we can skip future changes and avoid CLOG lookups on the
 * next call.
 *
 * 通过查询 CLOG 检查事务状态；若事务已中止，则丢弃全部变更。状态缓存在 txn->txn_flags 中，这样可以跳过后续
 * 变更，并避免下次再查 CLOG。
 *
 * Return true if the transaction is aborted, otherwise return false.
 *
 * 事务已中止则返回真，否则返回假。
 *
 * When the 'debug_logical_replication_streaming' is set to "immediate", we
 * don't check the transaction status, meaning the caller will always process
 * this transaction.
 *
 * 当 debug_logical_replication_streaming 设为 immediate 时，不检查事务状态，调用方总会处理该事务。
 */
static bool
ReorderBufferCheckAndTruncateAbortedTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/* Quick return for regression tests
	 *
	 * 供回归测试快速返回。
	 */
	if (unlikely(debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE))
		return false;

	/*
	 * Quick return if the transaction status is already known.
	 *
	 * 若事务状态已知，则快速返回。
	 */

	if (rbtxn_is_committed(txn))
		return false;
	if (rbtxn_is_aborted(txn))
	{
		/* Already-aborted transactions should not have any changes
		 *
		 * 已经中止的事务不应再有任何变更。
		 */
		Assert(txn->size == 0);

		return true;
	}

	/* Otherwise, check the transaction status using CLOG lookup
	 *
	 * 否则通过查询 CLOG 检查事务状态。
	 */

	if (TransactionIdIsInProgress(txn->xid))
		return false;

	if (TransactionIdDidCommit(txn->xid))
	{
		/*
		 * Remember the transaction is committed so that we can skip CLOG
		 * check next time, avoiding the pressure on CLOG lookup.
		 *
		 * 记住该事务已提交，下次就可以跳过 CLOG 检查，减轻 CLOG 查找压力。
		 */
		Assert(!rbtxn_is_aborted(txn));
		txn->txn_flags |= RBTXN_IS_COMMITTED;
		return false;
	}

	/*
	 * The transaction aborted. We discard both the changes collected so far
	 * and the toast reconstruction data. The full cleanup will happen as part
	 * of decoding ABORT record of this transaction.
	 *
	 * 事务已中止。丢弃目前收集的变更以及 toast 重建数据。完整清理会在解码该事务的 ABORT 记录时进行。
	 */
	ReorderBufferTruncateTXN(rb, txn, rbtxn_is_prepared(txn));
	ReorderBufferToastReset(rb, txn);

	/* All changes should be discarded
	 *
	 * 所有变更都应当已经丢弃。
	 */
	Assert(txn->size == 0);

	/*
	 * Mark the transaction as aborted so we can ignore future changes of this
	 * transaction.
	 *
	 * 把事务标为已中止，以便忽略它以后的变更。
	 */
	Assert(!rbtxn_is_committed(txn));
	txn->txn_flags |= RBTXN_IS_ABORTED;

	return true;
}

/*
 * Build a hash with a (relfilelocator, ctid) -> (cmin, cmax) mapping for use by
 * HeapTupleSatisfiesHistoricMVCC.
 *
 * 建立 (relfilelocator, ctid) 到 (cmin, cmax) 的哈希，供 HeapTupleSatisfiesHistoricMVCC 使用。
 */
static void
ReorderBufferBuildTupleCidHash(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	dlist_iter	iter;
	HASHCTL		hash_ctl;

	if (!rbtxn_has_catalog_changes(txn) || dlist_is_empty(&txn->tuplecids))
		return;

	hash_ctl.keysize = sizeof(ReorderBufferTupleCidKey);
	hash_ctl.entrysize = sizeof(ReorderBufferTupleCidEnt);
	hash_ctl.hcxt = rb->context;

	/*
	 * create the hash with the exact number of to-be-stored tuplecids from
	 * the start
	 *
	 * 一开始就按即将存入的 tuplecid 精确数量创建哈希表。
	 */
	txn->tuplecid_hash =
		hash_create("ReorderBufferTupleCid", txn->ntuplecids, &hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	dlist_foreach(iter, &txn->tuplecids)
	{
		ReorderBufferTupleCidKey key;
		ReorderBufferTupleCidEnt *ent;
		bool		found;
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

		/* be careful about padding
		 *
		 * 注意填充字节。
		 */
		memset(&key, 0, sizeof(ReorderBufferTupleCidKey));

		key.rlocator = change->data.tuplecid.locator;

		ItemPointerCopy(&change->data.tuplecid.tid,
						&key.tid);

		ent = (ReorderBufferTupleCidEnt *)
			hash_search(txn->tuplecid_hash, &key, HASH_ENTER, &found);
		if (!found)
		{
			ent->cmin = change->data.tuplecid.cmin;
			ent->cmax = change->data.tuplecid.cmax;
			ent->combocid = change->data.tuplecid.combocid;
		}
		else
		{
			/*
			 * Maybe we already saw this tuple before in this transaction, but
			 * if so it must have the same cmin.
			 *
			 * 也许本事务中已经见过这个元组，若是如此，cmin 必须相同。
			 */
			Assert(ent->cmin == change->data.tuplecid.cmin);

			/*
			 * cmax may be initially invalid, but once set it can only grow,
			 * and never become invalid again.
			 *
			 * cmax 起初可以无效，但一旦设置就只能增大，并且不会再变回无效。
			 */
			Assert((ent->cmax == InvalidCommandId) ||
				   ((change->data.tuplecid.cmax != InvalidCommandId) &&
					(change->data.tuplecid.cmax > ent->cmax)));
			ent->cmax = change->data.tuplecid.cmax;
		}
	}
}

/*
 * Copy a provided snapshot so we can modify it privately. This is needed so
 * that catalog modifying transactions can look into intermediate catalog
 * states.
 *
 * 复制给定快照，以便私下修改。修改系统目录的事务需要借此查看中间的目录状态。
 */
static Snapshot
ReorderBufferCopySnap(ReorderBuffer *rb, Snapshot orig_snap,
					  ReorderBufferTXN *txn, CommandId cid)
{
	Snapshot	snap;
	dlist_iter	iter;
	int			i = 0;
	Size		size;

	size = sizeof(SnapshotData) +
		sizeof(TransactionId) * orig_snap->xcnt +
		sizeof(TransactionId) * (txn->nsubtxns + 1);

	snap = MemoryContextAllocZero(rb->context, size);
	memcpy(snap, orig_snap, sizeof(SnapshotData));

	snap->copied = true;
	snap->active_count = 1;		/* mark as active so nobody frees it
					 *
					 * 标为活动，以免被别人释放。
					 */
	snap->regd_count = 0;
	snap->xip = (TransactionId *) (snap + 1);

	memcpy(snap->xip, orig_snap->xip, sizeof(TransactionId) * snap->xcnt);

	/*
	 * snap->subxip contains all txids that belong to our transaction which we
	 * need to check via cmin/cmax. That's why we store the toplevel
	 * transaction in there as well.
	 *
	 * snap->subxip 包含属于本事务、需要用 cmin/cmax 检查的全部事务号，因此把顶层事务也放进去。
	 */
	snap->subxip = snap->xip + snap->xcnt;
	snap->subxip[i++] = txn->xid;

	/*
	 * txn->nsubtxns isn't decreased when subtransactions abort, so count
	 * manually. Since it's an upper boundary it is safe to use it for the
	 * allocation above.
	 *
	 * 子事务中止时 txn->nsubtxns 不会减少，所以这里手工计数。它是上界，用来做上面的分配是安全的。
	 */
	snap->subxcnt = 1;

	dlist_foreach(iter, &txn->subtxns)
	{
		ReorderBufferTXN *sub_txn;

		sub_txn = dlist_container(ReorderBufferTXN, node, iter.cur);
		snap->subxip[i++] = sub_txn->xid;
		snap->subxcnt++;
	}

	/* sort so we can bsearch() later
	 *
	 * 排序，以便稍后用 bsearch()。
	 */
	qsort(snap->subxip, snap->subxcnt, sizeof(TransactionId), xidComparator);

	/* store the specified current CommandId
	 *
	 * 保存指定的当前 CommandId。
	 */
	snap->curcid = cid;

	return snap;
}

/*
 * Free a previously ReorderBufferCopySnap'ed snapshot
 *
 * 释放先前由 ReorderBufferCopySnap 复制的快照。
 */
static void
ReorderBufferFreeSnap(ReorderBuffer *rb, Snapshot snap)
{
	if (snap->copied)
		pfree(snap);
	else
		SnapBuildSnapDecRefcount(snap);
}

/*
 * If the transaction was (partially) streamed, we need to prepare or commit
 * it in a 'streamed' way.  That is, we first stream the remaining part of the
 * transaction, and then invoke stream_prepare or stream_commit message as per
 * the case.
 *
 * 若事务已经（部分）流式发送，就需要以流式方式准备或提交：先把剩余部分流式送出，再按情况调用 stream_prepare
 * 或 stream_commit。
 */
static void
ReorderBufferStreamCommit(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/* we should only call this for previously streamed transactions
	 *
	 * 只应对先前已经流式发送过的事务调用此函数。
	 */
	Assert(rbtxn_is_streamed(txn));

	ReorderBufferStreamTXN(rb, txn);

	if (rbtxn_is_prepared(txn))
	{
		/*
		 * Note, we send stream prepare even if a concurrent abort is
		 * detected. See DecodePrepare for more information.
		 *
		 * 即使检测到并发中止，也会发送 stream prepare。详见 DecodePrepare。
		 */
		Assert(!rbtxn_sent_prepare(txn));
		rb->stream_prepare(rb, txn, txn->final_lsn);
		txn->txn_flags |= RBTXN_SENT_PREPARE;

		/*
		 * This is a PREPARED transaction, part of a two-phase commit. The
		 * full cleanup will happen as part of the COMMIT PREPAREDs, so now
		 * just truncate txn by removing changes and tuplecids.
		 *
		 * 这是两阶段提交中的 PREPARED 事务。完整清理会在 COMMIT PREPARED 时进行，所以现在只截断事务，
		 * 去掉变更和 tuplecid。
		 */
		ReorderBufferTruncateTXN(rb, txn, true);
		/* Reset the CheckXidAlive
		 *
		 * 重置 CheckXidAlive。
		 */
		CheckXidAlive = InvalidTransactionId;
	}
	else
	{
		rb->stream_commit(rb, txn, txn->final_lsn);
		ReorderBufferCleanupTXN(rb, txn);
	}
}

/*
 * Set xid to detect concurrent aborts.
 *
 * 设置 xid，用来检测并发中止。
 *
 * While streaming an in-progress transaction or decoding a prepared
 * transaction there is a possibility that the (sub)transaction might get
 * aborted concurrently.  In such case if the (sub)transaction has catalog
 * update then we might decode the tuple using wrong catalog version.  For
 * example, suppose there is one catalog tuple with (xmin: 500, xmax: 0).  Now,
 * the transaction 501 updates the catalog tuple and after that we will have
 * two tuples (xmin: 500, xmax: 501) and (xmin: 501, xmax: 0).  Now, if 501 is
 * aborted and some other transaction say 502 updates the same catalog tuple
 * then the first tuple will be changed to (xmin: 500, xmax: 502).  So, the
 * problem is that when we try to decode the tuple inserted/updated in 501
 * after the catalog update, we will see the catalog tuple with (xmin: 500,
 * xmax: 502) as visible because it will consider that the tuple is deleted by
 * xid 502 which is not visible to our snapshot.  And when we will try to
 * decode with that catalog tuple, it can lead to a wrong result or a crash.
 * So, it is necessary to detect concurrent aborts to allow streaming of
 * in-progress transactions or decoding of prepared transactions.
 *
 * 流式发送进行中的事务，或解码已准备事务时，（子）事务可能被并发中止。若该（子）事务更新过系统目录，就可能
 * 用错误的目录版本去解码元组。例如目录元组原为 (xmin: 500, xmax: 0)。事务 501 更新它之后，会有两条元组 (
 * xmin: 500, xmax: 501) 和 (xmin: 501, xmax: 0)。若 501 中止，另一个事务 502 再更新同一目录元组，第一条会变
 * 成 (xmin: 500, xmax: 502)。此后解码 501 在目录更新之后插入或更新的元组时，会看到 (xmin: 500, xmax: 502)
 * 为可见，因为它认为删除者 xid 502 对我们的快照不可见。用那条目录元组解码可能导致错误结果或崩溃。因此必须检
 * 测并发中止，才能安全地流式发送进行中事务或解码已准备事务。
 *
 * For detecting the concurrent abort we set CheckXidAlive to the current
 * (sub)transaction's xid for which this change belongs to.  And, during
 * catalog scan we can check the status of the xid and if it is aborted we will
 * report a specific error so that we can stop streaming current transaction
 * and discard the already streamed changes on such an error.  We might have
 * already streamed some of the changes for the aborted (sub)transaction, but
 * that is fine because when we decode the abort we will stream abort message
 * to truncate the changes in the subscriber. Similarly, for prepared
 * transactions, we stop decoding if concurrent abort is detected and then
 * rollback the changes when rollback prepared is encountered. See
 * DecodePrepare.
 *
 * 为检测并发中止，把 CheckXidAlive 设为当前变更所属（子）事务的 xid。目录扫描时可以检查该 xid 的状态；若已
 * 中止，就报告一个特定错误，从而停止当前事务的流式发送，并在该错误上丢弃已经送出的变更。中止的（子）事务可
 * 能已经送出一部分变更，这没有关系：解码到 abort 时会流式发送 abort 消息，让订阅端截断这些变更。对已准备事
 * 务也类似：检测到并发中止就停止解码，等到 rollback prepared 时再回滚变更。参见 DecodePrepare。
 */
static inline void
SetupCheckXidLive(TransactionId xid)
{
	/*
	 * If the input transaction id is already set as a CheckXidAlive then
	 * nothing to do.
	 *
	 * 若传入的事务号已经是 CheckXidAlive，则无需再做。
	 */
	if (TransactionIdEquals(CheckXidAlive, xid))
		return;

	/*
	 * setup CheckXidAlive if it's not committed yet.  We don't check if the
	 * xid is aborted.  That will happen during catalog access.
	 *
	 * 若 xid 尚未提交，则设置 CheckXidAlive。这里不检查它是否已中止，那会在访问目录时进行。
	 */
	if (!TransactionIdDidCommit(xid))
		CheckXidAlive = xid;
	else
		CheckXidAlive = InvalidTransactionId;
}

/*
 * Helper function for ReorderBufferProcessTXN for applying change.
 *
 * ReorderBufferProcessTXN 用来应用变更的辅助函数。
 */
static inline void
ReorderBufferApplyChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
						 Relation relation, ReorderBufferChange *change,
						 bool streaming)
{
	if (streaming)
		rb->stream_change(rb, txn, relation, change);
	else
		rb->apply_change(rb, txn, relation, change);
}

/*
 * Helper function for ReorderBufferProcessTXN for applying the truncate.
 *
 * ReorderBufferProcessTXN 用来应用截断的辅助函数。
 */
static inline void
ReorderBufferApplyTruncate(ReorderBuffer *rb, ReorderBufferTXN *txn,
						   int nrelations, Relation *relations,
						   ReorderBufferChange *change, bool streaming)
{
	if (streaming)
		rb->stream_truncate(rb, txn, nrelations, relations, change);
	else
		rb->apply_truncate(rb, txn, nrelations, relations, change);
}

/*
 * Helper function for ReorderBufferProcessTXN for applying the message.
 *
 * ReorderBufferProcessTXN 用来应用消息的辅助函数。
 */
static inline void
ReorderBufferApplyMessage(ReorderBuffer *rb, ReorderBufferTXN *txn,
						  ReorderBufferChange *change, bool streaming)
{
	if (streaming)
		rb->stream_message(rb, txn, change->lsn, true,
						   change->data.msg.prefix,
						   change->data.msg.message_size,
						   change->data.msg.message);
	else
		rb->message(rb, txn, change->lsn, true,
					change->data.msg.prefix,
					change->data.msg.message_size,
					change->data.msg.message);
}

/*
 * Function to store the command id and snapshot at the end of the current
 * stream so that we can reuse the same while sending the next stream.
 *
 * 在当前流结束时保存 command id 和快照，以便发送下一流时复用。
 */
static inline void
ReorderBufferSaveTXNSnapshot(ReorderBuffer *rb, ReorderBufferTXN *txn,
							 Snapshot snapshot_now, CommandId command_id)
{
	txn->command_id = command_id;

	/* Avoid copying if it's already copied.
	 *
	 * 若已经复制过，则避免再次复制。
	 */
	if (snapshot_now->copied)
		txn->snapshot_now = snapshot_now;
	else
		txn->snapshot_now = ReorderBufferCopySnap(rb, snapshot_now,
												  txn, command_id);
}

/*
 * Mark the given transaction as streamed if it's a top-level transaction
 * or has changes.
 *
 * 若给定事务是顶层事务，或者它含有变更，则把它标为已流式发送。
 */
static void
ReorderBufferMaybeMarkTXNStreamed(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/*
	 * The top-level transaction, is marked as streamed always, even if it
	 * does not contain any changes (that is, when all the changes are in
	 * subtransactions).
	 *
	 * 顶层事务总是被标为已流式发送，即使它自己没有任何变更（全部变更都在子事务里）。
	 *
	 * For subtransactions, we only mark them as streamed when there are
	 * changes in them.
	 *
	 * 对子事务，只有其中确有变更时才标为已流式发送。
	 *
	 * We do it this way because of aborts - we don't want to send aborts for
	 * XIDs the downstream is not aware of. And of course, it always knows
	 * about the top-level xact (we send the XID in all messages), but we
	 * never stream XIDs of empty subxacts.
	 *
	 * 这样做是因为中止：我们不想为下游还不知道的 XID 发送中止。下游当然知道顶层事务（每条消息都会带上该
	 * XID），但我们从不流式发送空子事务的 XID。
	 */
	if (rbtxn_is_toptxn(txn) || (txn->nentries_mem != 0))
		txn->txn_flags |= RBTXN_IS_STREAMED;
}

/*
 * Helper function for ReorderBufferProcessTXN to handle the concurrent
 * abort of the streaming transaction.  This resets the TXN such that it
 * can be used to stream the remaining data of transaction being processed.
 * This can happen when the subtransaction is aborted and we still want to
 * continue processing the main or other subtransactions data.
 *
 * ReorderBufferProcessTXN 用来处理流式事务并发中止的辅助函数。它重置 TXN，使之仍可流式发送正在处理的事务的
 * 剩余数据。子事务中止后，我们仍可能要继续处理主事务或其他子事务的数据。
 */
static void
ReorderBufferResetTXN(ReorderBuffer *rb, ReorderBufferTXN *txn,
					  Snapshot snapshot_now,
					  CommandId command_id,
					  XLogRecPtr last_lsn,
					  ReorderBufferChange *specinsert)
{
	/* Discard the changes that we just streamed
	 *
	 * 丢弃刚刚流式发送过的变更。
	 */
	ReorderBufferTruncateTXN(rb, txn, rbtxn_is_prepared(txn));

	/* Free all resources allocated for toast reconstruction
	 *
	 * 释放为 toast 重建分配的全部资源。
	 */
	ReorderBufferToastReset(rb, txn);

	/* Return the spec insert change if it is not NULL
	 *
	 * 若推测性插入变更不是 NULL，则归还它。
	 */
	if (specinsert != NULL)
	{
		ReorderBufferFreeChange(rb, specinsert, true);
		specinsert = NULL;
	}

	/*
	 * For the streaming case, stop the stream and remember the command ID and
	 * snapshot for the streaming run.
	 *
	 * 对流式情况，停止当前流，并记住这次流式传输的 command ID 和快照。
	 */
	if (rbtxn_is_streamed(txn))
	{
		rb->stream_stop(rb, txn, last_lsn);
		ReorderBufferSaveTXNSnapshot(rb, txn, snapshot_now, command_id);
	}

	/* All changes must be deallocated
	 *
	 * 所有变更必须已经释放。
	 */
	Assert(txn->size == 0);
}

/*
 * Helper function for ReorderBufferReplay and ReorderBufferStreamTXN.
 *
 * 供 ReorderBufferReplay 和 ReorderBufferStreamTXN 使用的辅助函数。
 *
 * Send data of a transaction (and its subtransactions) to the
 * output plugin. We iterate over the top and subtransactions (using a k-way
 * merge) and replay the changes in lsn order.
 *
 * 把事务（及其子事务）的数据发给输出插件。用 k 路归并遍历顶层事务和子事务，按 LSN 顺序重放变更。
 *
 * If streaming is true then data will be sent using stream API.
 *
 * 若 streaming 为真，则通过流式 API 发送数据。
 *
 * Note: "volatile" markers on some parameters are to avoid trouble with
 * PG_TRY inside the function.
 *
 * 部分参数标为 volatile，是为了避免函数内部 PG_TRY 带来的问题。
 */
static void
ReorderBufferProcessTXN(ReorderBuffer *rb, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn,
						volatile Snapshot snapshot_now,
						volatile CommandId command_id,
						bool streaming)
{
	bool		using_subtxn;
	MemoryContext ccxt = CurrentMemoryContext;
	ReorderBufferIterTXNState *volatile iterstate = NULL;
	volatile XLogRecPtr prev_lsn = InvalidXLogRecPtr;
	ReorderBufferChange *volatile specinsert = NULL;
	volatile bool stream_started = false;
	ReorderBufferTXN *volatile curtxn = NULL;

	/* build data to be able to lookup the CommandIds of catalog tuples
	 *
	 * 建立数据，以便查找目录元组的 CommandId。
	 */
	ReorderBufferBuildTupleCidHash(rb, txn);

	/* setup the initial snapshot
	 *
	 * 设置初始快照。
	 */
	SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);

	/*
	 * Decoding needs access to syscaches et al., which in turn use
	 * heavyweight locks and such. Thus we need to have enough state around to
	 * keep track of those.  The easiest way is to simply use a transaction
	 * internally.  That also allows us to easily enforce that nothing writes
	 * to the database by checking for xid assignments.
	 *
	 * 解码需要访问系统缓存等，而这些会使用重量级锁之类的资源。因此必须有足够的状态来跟踪它们。最简单的
	 * 办法是在内部使用一个事务。这样也可以通过检查是否分配了 xid，轻易保证没有向数据库写入。
	 *
	 * When we're called via the SQL SRF there's already a transaction
	 * started, so start an explicit subtransaction there.
	 *
	 * 若通过 SQL 的 SRF 调用，外面已经有事务，因此在那里显式开始一个子事务。
	 */
	using_subtxn = IsTransactionOrTransactionBlock();

	PG_TRY();
	{
		ReorderBufferChange *change;
		int			changes_count = 0;	/* used to accumulate the number of
										 * changes
								 *
								 * 用来累计变更条数。
								 */

		if (using_subtxn)
			BeginInternalSubTransaction(streaming ? "stream" : "replay");
		else
			StartTransactionCommand();

		/*
		 * We only need to send begin/begin-prepare for non-streamed
		 * transactions.
		 *
		 * 只有非流式事务才需要发送 begin 或 begin-prepare。
		 */
		if (!streaming)
		{
			if (rbtxn_is_prepared(txn))
				rb->begin_prepare(rb, txn);
			else
				rb->begin(rb, txn);
		}

		ReorderBufferIterTXNInit(rb, txn, &iterstate);
		while ((change = ReorderBufferIterTXNNext(rb, iterstate)) != NULL)
		{
			Relation	relation = NULL;
			Oid			reloid;

			CHECK_FOR_INTERRUPTS();

			/*
			 * We can't call start stream callback before processing first
			 * change.
			 *
			 * 在处理第一条变更之前，不能调用开始流的回调。
			 */
			if (prev_lsn == InvalidXLogRecPtr)
			{
				if (streaming)
				{
					txn->origin_id = change->origin_id;
					rb->stream_start(rb, txn, change->lsn);
					stream_started = true;
				}
			}

			/*
			 * Enforce correct ordering of changes, merged from multiple
			 * subtransactions. The changes may have the same LSN due to
			 * MULTI_INSERT xlog records.
			 *
			 * 强制从多个子事务归并来的变更保持正确顺序。由于 MULTI_INSERT 的 xlog 记录，这些变
			 * 更的 LSN 可能相同。
			 */
			Assert(prev_lsn == InvalidXLogRecPtr || prev_lsn <= change->lsn);

			prev_lsn = change->lsn;

			/*
			 * Set the current xid to detect concurrent aborts. This is
			 * required for the cases when we decode the changes before the
			 * COMMIT record is processed.
			 *
			 * 设置当前 xid 以检测并发中止。在处理 COMMIT 记录之前就解码变更时必须这样做。
			 */
			if (streaming || rbtxn_is_prepared(change->txn))
			{
				curtxn = change->txn;
				SetupCheckXidLive(curtxn->xid);
			}

			switch (change->action)
			{
				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:

					/*
					 * Confirmation for speculative insertion arrived. Simply
					 * use as a normal record. It'll be cleaned up at the end
					 * of INSERT processing.
					 *
					 * 推测性插入的确认已经到达。把它当作普通记录使用，并在 INSERT 处理
					 * 结束时清理。
					 */
					if (specinsert == NULL)
						elog(ERROR, "invalid ordering of speculative insertion changes");
					Assert(specinsert->data.tp.oldtuple == NULL);
					change = specinsert;
					change->action = REORDER_BUFFER_CHANGE_INSERT;

					/* intentionally fall through
					 *
					 * 有意落入下面的分支。
					 */
				case REORDER_BUFFER_CHANGE_INSERT:
				case REORDER_BUFFER_CHANGE_UPDATE:
				case REORDER_BUFFER_CHANGE_DELETE:
					Assert(snapshot_now);

					reloid = RelidByRelfilenumber(change->data.tp.rlocator.spcOid,
												  change->data.tp.rlocator.relNumber);

					/*
					 * Mapped catalog tuple without data, emitted while
					 * catalog table was in the process of being rewritten. We
					 * can fail to look up the relfilenumber, because the
					 * relmapper has no "historic" view, in contrast to the
					 * normal catalog during decoding. Thus repeated rewrites
					 * can cause a lookup failure. That's OK because we do not
					 * decode catalog changes anyway. Normally such tuples
					 * would be skipped over below, but we can't identify
					 * whether the table should be logically logged without
					 * mapping the relfilenumber to the oid.
					 *
					 * 这是重写目录表期间发出的、没有数据的已映射目录元组。可能查不到
					 * relfilenumber，因为与解码期间的普通目录不同，relmapper 没有历史视
					 * 图。反复重写因此可能导致查找失败。这是可以接受的，因为我们本来就
					 * 不解码目录变更。正常情况下这种元组会在下面被跳过，但不把
					 * relfilenumber 映射到 oid，就无法判断该表是否应当做逻辑日志。
					 */
					if (reloid == InvalidOid &&
						change->data.tp.newtuple == NULL &&
						change->data.tp.oldtuple == NULL)
						goto change_done;
					else if (reloid == InvalidOid)
						elog(ERROR, "could not map filenumber \"%s\" to relation OID",
							 relpathperm(change->data.tp.rlocator,
										 MAIN_FORKNUM).str);

					relation = RelationIdGetRelation(reloid);

					if (!RelationIsValid(relation))
						elog(ERROR, "could not open relation with OID %u (for filenumber \"%s\")",
							 reloid,
							 relpathperm(change->data.tp.rlocator,
										 MAIN_FORKNUM).str);

					if (!RelationIsLogicallyLogged(relation))
						goto change_done;

					/*
					 * Ignore temporary heaps created during DDL unless the
					 * plugin has asked for them.
					 *
					 * 除非插件要求，否则忽略 DDL 期间创建的临时堆。
					 */
					if (relation->rd_rel->relrewrite && !rb->output_rewrites)
						goto change_done;

					/*
					 * For now ignore sequence changes entirely. Most of the
					 * time they don't log changes using records we
					 * understand, so it doesn't make sense to handle the few
					 * cases we do.
					 *
					 * 目前完全忽略序列变更。大多数时候它们不用我们能理解的记录来记日志，
					 * 因此没有必要处理我们能看懂的那少数几种。
					 */
					if (relation->rd_rel->relkind == RELKIND_SEQUENCE)
						goto change_done;

					/* user-triggered change
					 *
					 * 用户触发的变更。
					 */
					if (!IsToastRelation(relation))
					{
						ReorderBufferToastReplace(rb, txn, relation, change);
						ReorderBufferApplyChange(rb, txn, relation, change,
												 streaming);

						/*
						 * Only clear reassembled toast chunks if we're sure
						 * they're not required anymore. The creator of the
						 * tuple tells us.
						 *
						 * 只有确定不再需要已经重组的 toast 块时才清除它们。由元组的
						 * 创建者告知我们。
						 */
						if (change->data.tp.clear_toast_afterwards)
							ReorderBufferToastReset(rb, txn);
					}
					/* we're not interested in toast deletions
					 *
					 * 我们不关心 toast 删除。
					 */
					else if (change->action == REORDER_BUFFER_CHANGE_INSERT)
					{
						/*
						 * Need to reassemble the full toasted Datum in
						 * memory, to ensure the chunks don't get reused till
						 * we're done remove it from the list of this
						 * transaction's changes. Otherwise it will get
						 * freed/reused while restoring spooled data from
						 * disk.
						 *
						 * 需要在内存中重组完整的 toast Datum，以确保在我们用完之前
						 * 这些块不会被复用。把它从本事务的变更链表中摘下，否则从磁
						 * 盘恢复溢出数据时它会被释放或复用。
						 */
						Assert(change->data.tp.newtuple != NULL);

						dlist_delete(&change->node);
						ReorderBufferToastAppendChunk(rb, txn, relation,
													  change);
					}

			change_done:

					/*
					 * If speculative insertion was confirmed, the record
					 * isn't needed anymore.
					 *
					 * 若推测性插入已确认，这条记录就不再需要。
					 */
					if (specinsert != NULL)
					{
						ReorderBufferFreeChange(rb, specinsert, true);
						specinsert = NULL;
					}

					if (RelationIsValid(relation))
					{
						RelationClose(relation);
						relation = NULL;
					}
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:

					/*
					 * Speculative insertions are dealt with by delaying the
					 * processing of the insert until the confirmation record
					 * arrives. For that we simply unlink the record from the
					 * chain, so it does not get freed/reused while restoring
					 * spooled data from disk.
					 *
					 * 推测性插入要推迟到确认记录到达后再处理。为此把该记录从链表上摘下，
					 * 这样从磁盘恢复溢出数据时它不会被释放或复用。
					 *
					 * This is safe in the face of concurrent catalog changes
					 * because the relevant relation can't be changed between
					 * speculative insertion and confirmation due to
					 * CheckTableNotInUse() and locking.
					 *
					 * 即使目录并发变化这也是安全的：由于 CheckTableNotInUse() 和锁，相
					 * 关关系在推测性插入和确认之间不能被修改。
					 */

					/* clear out a pending (and thus failed) speculation
					 *
					 * 清掉一条尚未完成（因而失败）的推测性插入。
					 */
					if (specinsert != NULL)
					{
						ReorderBufferFreeChange(rb, specinsert, true);
						specinsert = NULL;
					}

					/* and memorize the pending insertion
					 *
					 * 并记住这条待处理的插入。
					 */
					dlist_delete(&change->node);
					specinsert = change;
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:

					/*
					 * Abort for speculative insertion arrived. So cleanup the
					 * specinsert tuple and toast hash.
					 *
					 * 推测性插入的中止已经到达。清理 specinsert 元组和 toast 哈希。
					 *
					 * Note that we get the spec abort change for each toast
					 * entry but we need to perform the cleanup only the first
					 * time we get it for the main table.
					 *
					 * 每个 toast 项都会收到一次推测性中止变更，但只需要在第一次针对主表
					 * 收到它时做清理。
					 */
					if (specinsert != NULL)
					{
						/*
						 * We must clean the toast hash before processing a
						 * completely new tuple to avoid confusion about the
						 * previous tuple's toast chunks.
						 *
						 * 处理一条全新元组之前必须清掉 toast 哈希，以免和前一条元组
						 * 的 toast 块混淆。
						 */
						Assert(change->data.tp.clear_toast_afterwards);
						ReorderBufferToastReset(rb, txn);

						/* We don't need this record anymore.
						 *
						 * 这条记录不再需要。
						 */
						ReorderBufferFreeChange(rb, specinsert, true);
						specinsert = NULL;
					}
					break;

				case REORDER_BUFFER_CHANGE_TRUNCATE:
					{
						int			i;
						int			nrelids = change->data.truncate.nrelids;
						int			nrelations = 0;
						Relation   *relations;

						relations = palloc0(nrelids * sizeof(Relation));
						for (i = 0; i < nrelids; i++)
						{
							Oid			relid = change->data.truncate.relids[i];
							Relation	rel;

							rel = RelationIdGetRelation(relid);

							if (!RelationIsValid(rel))
								elog(ERROR, "could not open relation with OID %u", relid);

							if (!RelationIsLogicallyLogged(rel))
								continue;

							relations[nrelations++] = rel;
						}

						/* Apply the truncate.
						 *
						 * 应用截断。
						 */
						ReorderBufferApplyTruncate(rb, txn, nrelations,
												   relations, change,
												   streaming);

						for (i = 0; i < nrelations; i++)
							RelationClose(relations[i]);

						break;
					}

				case REORDER_BUFFER_CHANGE_MESSAGE:
					ReorderBufferApplyMessage(rb, txn, change, streaming);
					break;

				case REORDER_BUFFER_CHANGE_INVALIDATION:
					/* Execute the invalidation messages locally
					 *
					 * 在本地执行失效消息。
					 */
					ReorderBufferExecuteInvalidations(change->data.inval.ninvalidations,
													  change->data.inval.invalidations);
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
					/* get rid of the old
					 *
					 * 丢掉旧的。
					 */
					TeardownHistoricSnapshot(false);

					if (snapshot_now->copied)
					{
						ReorderBufferFreeSnap(rb, snapshot_now);
						snapshot_now =
							ReorderBufferCopySnap(rb, change->data.snapshot,
												  txn, command_id);
					}

					/*
					 * Restored from disk, need to be careful not to double
					 * free. We could introduce refcounting for that, but for
					 * now this seems infrequent enough not to care.
					 *
					 * 这是从磁盘恢复的，注意不要重复释放。可以为此引入引用计数，但目前
					 * 这种情况很少，不必在意。
					 */
					else if (change->data.snapshot->copied)
					{
						snapshot_now =
							ReorderBufferCopySnap(rb, change->data.snapshot,
												  txn, command_id);
					}
					else
					{
						snapshot_now = change->data.snapshot;
					}

					/* and continue with the new one
					 *
					 * 然后继续使用新的快照。
					 */
					SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
					Assert(change->data.command_id != InvalidCommandId);

					if (command_id < change->data.command_id)
					{
						command_id = change->data.command_id;

						if (!snapshot_now->copied)
						{
							/* we don't use the global one anymore
							 *
							 * 不再使用全局的那一份。
							 */
							snapshot_now = ReorderBufferCopySnap(rb, snapshot_now,
																 txn, command_id);
						}

						snapshot_now->curcid = command_id;

						TeardownHistoricSnapshot(false);
						SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);
					}

					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
					elog(ERROR, "tuplecid value in changequeue");
					break;
			}

			/*
			 * It is possible that the data is not sent to downstream for a
			 * long time either because the output plugin filtered it or there
			 * is a DDL that generates a lot of data that is not processed by
			 * the plugin. So, in such cases, the downstream can timeout. To
			 * avoid that we try to send a keepalive message if required.
			 * Trying to send a keepalive message after every change has some
			 * overhead, but testing showed there is no noticeable overhead if
			 * we do it after every ~100 changes.
			 *
			 * 数据可能长时间不发往下游，要么因为输出插件过滤了它，要么因为 DDL 产生了大量插件不
			 * 处理的数据。这时下游可能超时。为避免这种情况，在需要时尝试发送 keepalive。每条变
			 * 更后都尝试发送会有一些开销，但测试表明大约每 100 条变更发送一次没有明显开销。
			 */
#define CHANGES_THRESHOLD 100

			if (++changes_count >= CHANGES_THRESHOLD)
			{
				rb->update_progress_txn(rb, txn, prev_lsn);
				changes_count = 0;
			}
		}

		/* speculative insertion record must be freed by now
		 *
		 * 推测性插入记录此时必须已经释放。
		 */
		Assert(!specinsert);

		/* clean up the iterator
		 *
		 * 清理迭代器。
		 */
		ReorderBufferIterTXNFinish(rb, iterstate);
		iterstate = NULL;

		/*
		 * Update total transaction count and total bytes processed by the
		 * transaction and its subtransactions. Ensure to not count the
		 * streamed transaction multiple times.
		 *
		 * 更新事务总数以及该事务及其子事务已处理的总字节数。不要把已流式发送的事务重复计数。
		 *
		 * Note that the statistics computation has to be done after
		 * ReorderBufferIterTXNFinish as it releases the serialized change
		 * which we have already accounted in ReorderBufferIterTXNNext.
		 *
		 * 统计必须在 ReorderBufferIterTXNFinish 之后计算，因为它会释放序列化变更，而那些字节已在
		 * ReorderBufferIterTXNNext 中计入。
		 */
		if (!rbtxn_is_streamed(txn))
			rb->totalTxns++;

		rb->totalBytes += txn->total_size;

		/*
		 * Done with current changes, send the last message for this set of
		 * changes depending upon streaming mode.
		 *
		 * 当前这批变更已经处理完，按是否流式发送，为这批变更发出最后一条消息。
		 */
		if (streaming)
		{
			if (stream_started)
			{
				rb->stream_stop(rb, txn, prev_lsn);
				stream_started = false;
			}
		}
		else
		{
			/*
			 * Call either PREPARE (for two-phase transactions) or COMMIT (for
			 * regular ones).
			 *
			 * 两阶段事务调用 PREPARE，普通事务调用 COMMIT。
			 */
			if (rbtxn_is_prepared(txn))
			{
				Assert(!rbtxn_sent_prepare(txn));
				rb->prepare(rb, txn, commit_lsn);
				txn->txn_flags |= RBTXN_SENT_PREPARE;
			}
			else
				rb->commit(rb, txn, commit_lsn);
		}

		/* this is just a sanity check against bad output plugin behaviour
		 *
		 * 这只是用来防范行为不当的输出插件的健全性检查。
		 */
		if (GetCurrentTransactionIdIfAny() != InvalidTransactionId)
			elog(ERROR, "output plugin used XID %u",
				 GetCurrentTransactionId());

		/*
		 * Remember the command ID and snapshot for the next set of changes in
		 * streaming mode.
		 *
		 * 在流式模式下记住 command ID 和快照，供下一批变更使用。
		 */
		if (streaming)
			ReorderBufferSaveTXNSnapshot(rb, txn, snapshot_now, command_id);
		else if (snapshot_now->copied)
			ReorderBufferFreeSnap(rb, snapshot_now);

		/* cleanup
		 *
		 * 清理。
		 */
		TeardownHistoricSnapshot(false);

		/*
		 * Aborting the current (sub-)transaction as a whole has the right
		 * semantics. We want all locks acquired in here to be released, not
		 * reassigned to the parent and we do not want any database access
		 * have persistent effects.
		 *
		 * 把当前（子）事务整个中止，语义才正确。这里获得的锁都应当释放，而不是转给父事务，并且不希
		 * 望任何数据库访问留下持久效果。
		 */
		AbortCurrentTransaction();

		/* make sure there's no cache pollution
		 *
		 * 确保缓存没有被污染。
		 */
		if (rbtxn_distr_inval_overflowed(txn))
		{
			Assert(txn->ninvalidations_distributed == 0);
			InvalidateSystemCaches();
		}
		else
		{
			ReorderBufferExecuteInvalidations(txn->ninvalidations, txn->invalidations);
			ReorderBufferExecuteInvalidations(txn->ninvalidations_distributed,
											  txn->invalidations_distributed);
		}

		if (using_subtxn)
			RollbackAndReleaseCurrentSubTransaction();

		/*
		 * We are here due to one of the four reasons: 1. Decoding an
		 * in-progress txn. 2. Decoding a prepared txn. 3. Decoding of a
		 * prepared txn that was (partially) streamed. 4. Decoding a committed
		 * txn.
		 *
		 * 执行到这里是四种原因之一：1. 解码进行中的事务。2. 解码已准备事务。3. 解码曾经（部分）流式
		 * 发送的已准备事务。4. 解码已提交事务。
		 *
		 * For 1, we allow truncation of txn data by removing the changes
		 * already streamed but still keeping other things like invalidations,
		 * snapshot, and tuplecids. For 2 and 3, we indicate
		 * ReorderBufferTruncateTXN to do more elaborate truncation of txn
		 * data as the entire transaction has been decoded except for commit.
		 * For 4, as the entire txn has been decoded, we can fully clean up
		 * the TXN reorder buffer.
		 *
		 * 对第 1 种，允许截断事务数据：去掉已经流式发送的变更，但仍保留失效消息、快照和 tuplecid。
		 * 对第 2、3 种，让 ReorderBufferTruncateTXN 做更彻底的截断，因为除了提交之外整个事务都已解
		 * 码。对第 4 种，整个事务已经解码，可以完全清理该 TXN 的 reorder buffer。
		 */
		if (streaming || rbtxn_is_prepared(txn))
		{
			if (streaming)
				ReorderBufferMaybeMarkTXNStreamed(rb, txn);

			ReorderBufferTruncateTXN(rb, txn, rbtxn_is_prepared(txn));
			/* Reset the CheckXidAlive
			 *
			 * 重置 CheckXidAlive。
			 */
			CheckXidAlive = InvalidTransactionId;
		}
		else
			ReorderBufferCleanupTXN(rb, txn);
	}
	PG_CATCH();
	{
		MemoryContext ecxt = MemoryContextSwitchTo(ccxt);
		ErrorData  *errdata = CopyErrorData();

		/* TODO: Encapsulate cleanup from the PG_TRY and PG_CATCH blocks
		 *
		 * TODO：把 PG_TRY 与 PG_CATCH 块中的清理封装起来。
		 */
		if (iterstate)
			ReorderBufferIterTXNFinish(rb, iterstate);

		TeardownHistoricSnapshot(true);

		/*
		 * Force cache invalidation to happen outside of a valid transaction
		 * to prevent catalog access as we just caught an error.
		 *
		 * 强制在有效事务之外做缓存失效，以免刚刚捕获错误之后还去访问目录。
		 */
		AbortCurrentTransaction();

		/* make sure there's no cache pollution
		 *
		 * 确保缓存没有被污染。
		 */
		if (rbtxn_distr_inval_overflowed(txn))
		{
			Assert(txn->ninvalidations_distributed == 0);
			InvalidateSystemCaches();
		}
		else
		{
			ReorderBufferExecuteInvalidations(txn->ninvalidations, txn->invalidations);
			ReorderBufferExecuteInvalidations(txn->ninvalidations_distributed,
											  txn->invalidations_distributed);
		}

		if (using_subtxn)
			RollbackAndReleaseCurrentSubTransaction();

		/*
		 * The error code ERRCODE_TRANSACTION_ROLLBACK indicates a concurrent
		 * abort of the (sub)transaction we are streaming or preparing. We
		 * need to do the cleanup and return gracefully on this error, see
		 * SetupCheckXidLive.
		 *
		 * 错误码 ERRCODE_TRANSACTION_ROLLBACK 表示正在流式发送或准备的（子）事务被并发中止。需要做
		 * 清理并从这个错误正常返回，参见 SetupCheckXidLive。
		 *
		 * This error code can be thrown by one of the callbacks we call
		 * during decoding so we need to ensure that we return gracefully only
		 * when we are sending the data in streaming mode and the streaming is
		 * not finished yet or when we are sending the data out on a PREPARE
		 * during a two-phase commit.
		 *
		 * 这个错误码可能由解码期间调用的某个回调抛出，因此只有在流式发送数据且流尚未结束，或者在两
		 * 阶段提交的 PREPARE 上向外发送数据时，才正常返回。
		 */
		if (errdata->sqlerrcode == ERRCODE_TRANSACTION_ROLLBACK &&
			(stream_started || rbtxn_is_prepared(txn)))
		{
			/* curtxn must be set for streaming or prepared transactions
			 *
			 * 对流式或已准备事务，curtxn 必须已经设置。
			 */
			Assert(curtxn);

			/* Cleanup the temporary error state.
			 *
			 * 清理临时错误状态。
			 */
			FlushErrorState();
			FreeErrorData(errdata);
			errdata = NULL;

			/* Remember the transaction is aborted.
			 *
			 * 记住该事务已中止。
			 */
			Assert(!rbtxn_is_committed(curtxn));
			curtxn->txn_flags |= RBTXN_IS_ABORTED;

			/* Mark the transaction is streamed if appropriate
			 *
			 * 在适当的情况下把事务标为已流式发送。
			 */
			if (stream_started)
				ReorderBufferMaybeMarkTXNStreamed(rb, txn);

			/* Reset the TXN so that it is allowed to stream remaining data.
			 *
			 * 重置 TXN，使它仍可以流式发送剩余数据。
			 */
			ReorderBufferResetTXN(rb, txn, snapshot_now,
								  command_id, prev_lsn,
								  specinsert);
		}
		else
		{
			ReorderBufferCleanupTXN(rb, txn);
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}
	}
	PG_END_TRY();
}

/*
 * Perform the replay of a transaction and its non-aborted subtransactions.
 *
 * 重放一个事务及其未中止的子事务。
 *
 * Subtransactions previously have to be processed by
 * ReorderBufferCommitChild(), even if previously assigned to the toplevel
 * transaction with ReorderBufferAssignChild.
 *
 * 子事务必须事先由 ReorderBufferCommitChild() 处理过，即使之前已经用 ReorderBufferAssignChild 挂到顶层事务
 * 上。
 *
 * This interface is called once a prepare or toplevel commit is read for both
 * streamed as well as non-streamed transactions.
 *
 * 读到 prepare 或顶层提交时调用此接口，对流式和非流式事务都是如此。
 */
static void
ReorderBufferReplay(ReorderBufferTXN *txn,
					ReorderBuffer *rb, TransactionId xid,
					XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
					TimestampTz commit_time,
					RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	Snapshot	snapshot_now;
	CommandId	command_id = FirstCommandId;

	txn->final_lsn = commit_lsn;
	txn->end_lsn = end_lsn;
	txn->xact_time.commit_time = commit_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	/*
	 * If the transaction was (partially) streamed, we need to commit it in a
	 * 'streamed' way. That is, we first stream the remaining part of the
	 * transaction, and then invoke stream_commit message.
	 *
	 * 若事务已经（部分）流式发送，就需要以流式方式提交：先把剩余部分流式送出，再调用 stream_commit。
	 *
	 * Called after everything (origin ID, LSN, ...) is stored in the
	 * transaction to avoid passing that information directly.
	 *
	 * 在 origin ID、LSN 等都已写入事务之后才调用，以免直接传递这些信息。
	 */
	if (rbtxn_is_streamed(txn))
	{
		ReorderBufferStreamCommit(rb, txn);
		return;
	}

	/*
	 * If this transaction has no snapshot, it didn't make any changes to the
	 * database, so there's nothing to decode.  Note that
	 * ReorderBufferCommitChild will have transferred any snapshots from
	 * subtransactions if there were any.
	 *
	 * 若该事务没有快照，说明它没有对数据库做任何修改，因此没有什么可解码的。若子事务有快照，
	 * ReorderBufferCommitChild 会已经把它们转移过来。
	 */
	if (txn->base_snapshot == NULL)
	{
		Assert(txn->ninvalidations == 0);

		/*
		 * Removing this txn before a commit might result in the computation
		 * of an incorrect restart_lsn. See SnapBuildProcessRunningXacts.
		 *
		 * 在提交之前移除该事务，可能导致算出错误的 restart_lsn。参见 SnapBuildProcessRunningXacts。
		 */
		if (!rbtxn_is_prepared(txn))
			ReorderBufferCleanupTXN(rb, txn);
		return;
	}

	snapshot_now = txn->base_snapshot;

	/* Process and send the changes to output plugin.
	 *
	 * 处理变更并发送给输出插件。
	 */
	ReorderBufferProcessTXN(rb, txn, commit_lsn, snapshot_now,
							command_id, false);
}

/*
 * Commit a transaction.
 *
 * 提交一个事务。
 *
 * See comments for ReorderBufferReplay().
 *
 * 参见 ReorderBufferReplay() 的注释。
 */
void
ReorderBufferCommit(ReorderBuffer *rb, TransactionId xid,
					XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
					TimestampTz commit_time,
					RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown transaction, nothing to replay
	 *
	 * 未知事务，没有什么可重放的。
	 */
	if (txn == NULL)
		return;

	ReorderBufferReplay(txn, rb, xid, commit_lsn, end_lsn, commit_time,
						origin_id, origin_lsn);
}

/*
 * Record the prepare information for a transaction. Also, mark the transaction
 * as a prepared transaction.
 *
 * 记录事务的 prepare 信息，并把该事务标为已准备事务。
 */
bool
ReorderBufferRememberPrepareInfo(ReorderBuffer *rb, TransactionId xid,
								 XLogRecPtr prepare_lsn, XLogRecPtr end_lsn,
								 TimestampTz prepare_time,
								 RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr, false);

	/* unknown transaction, nothing to do
	 *
	 * 未知事务，无需处理。
	 */
	if (txn == NULL)
		return false;

	/*
	 * Remember the prepare information to be later used by commit prepared in
	 * case we skip doing prepare.
	 *
	 * 记住 prepare 信息，以便在跳过 prepare 时供 commit prepared 稍后使用。
	 */
	txn->final_lsn = prepare_lsn;
	txn->end_lsn = end_lsn;
	txn->xact_time.prepare_time = prepare_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	/* Mark this transaction as a prepared transaction
	 *
	 * 把该事务标为已准备事务。
	 */
	Assert((txn->txn_flags & RBTXN_PREPARE_STATUS_MASK) == 0);
	txn->txn_flags |= RBTXN_IS_PREPARED;

	return true;
}

/* Remember that we have skipped prepare
 *
 * 记住我们已经跳过了 prepare。
 */
void
ReorderBufferSkipPrepare(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr, false);

	/* unknown transaction, nothing to do
	 *
	 * 未知事务，无需处理。
	 */
	if (txn == NULL)
		return;

	/* txn must have been marked as a prepared transaction
	 *
	 * txn 必须已经被标为已准备事务。
	 */
	Assert((txn->txn_flags & RBTXN_PREPARE_STATUS_MASK) == RBTXN_IS_PREPARED);
	txn->txn_flags |= RBTXN_SKIPPED_PREPARE;
}

/*
 * Prepare a two-phase transaction.
 *
 * 准备一个两阶段事务。
 *
 * See comments for ReorderBufferReplay().
 *
 * 参见 ReorderBufferReplay() 的注释。
 */
void
ReorderBufferPrepare(ReorderBuffer *rb, TransactionId xid,
					 char *gid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown transaction, nothing to replay
	 *
	 * 未知事务，没有什么可重放的。
	 */
	if (txn == NULL)
		return;

	/*
	 * txn must have been marked as a prepared transaction and must have
	 * neither been skipped nor sent a prepare. Also, the prepare info must
	 * have been updated in it by now.
	 *
	 * txn 必须已被标为已准备事务，并且既没有被跳过，也没有发送过 prepare。此时它里面的 prepare 信息也必
	 * 须已经更新。
	 */
	Assert((txn->txn_flags & RBTXN_PREPARE_STATUS_MASK) == RBTXN_IS_PREPARED);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	txn->gid = pstrdup(gid);

	ReorderBufferReplay(txn, rb, xid, txn->final_lsn, txn->end_lsn,
						txn->xact_time.prepare_time, txn->origin_id, txn->origin_lsn);

	/*
	 * Send a prepare if not already done so. This might occur if we have
	 * detected a concurrent abort while replaying the non-streaming
	 * transaction.
	 *
	 * 若尚未发送 prepare，则补发一次。这可能发生在重放非流式事务时检测到并发中止的情况。
	 */
	if (!rbtxn_sent_prepare(txn))
	{
		rb->prepare(rb, txn, txn->final_lsn);
		txn->txn_flags |= RBTXN_SENT_PREPARE;
	}
}

/*
 * This is used to handle COMMIT/ROLLBACK PREPARED.
 *
 * 用于处理 COMMIT PREPARED 或 ROLLBACK PREPARED。
 */
void
ReorderBufferFinishPrepared(ReorderBuffer *rb, TransactionId xid,
							XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
							XLogRecPtr two_phase_at,
							TimestampTz commit_time, RepOriginId origin_id,
							XLogRecPtr origin_lsn, char *gid, bool is_commit)
{
	ReorderBufferTXN *txn;
	XLogRecPtr	prepare_end_lsn;
	TimestampTz prepare_time;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, commit_lsn, false);

	/* unknown transaction, nothing to do
	 *
	 * 未知事务，无需处理。
	 */
	if (txn == NULL)
		return;

	/*
	 * By this time the txn has the prepare record information, remember it to
	 * be later used for rollback.
	 *
	 * 此时 txn 已有 prepare 记录的信息，记住它，供稍后回滚使用。
	 */
	prepare_end_lsn = txn->end_lsn;
	prepare_time = txn->xact_time.prepare_time;

	/* add the gid in the txn
	 *
	 * 把 gid 写入该事务。
	 */
	txn->gid = pstrdup(gid);

	/*
	 * It is possible that this transaction is not decoded at prepare time
	 * either because by that time we didn't have a consistent snapshot, or
	 * two_phase was not enabled, or it was decoded earlier but we have
	 * restarted. We only need to send the prepare if it was not decoded
	 * earlier. We don't need to decode the xact for aborts if it is not done
	 * already.
	 *
	 * 该事务可能没有在 prepare 时被解码：当时也许还没有一致性快照，或者没有启用 two_phase，或者先前解码
	 * 过但我们已经重启。只有先前没有解码过时才需要发送 prepare。若中止时尚未解码，也不需要再解码该事务。
	 */
	if ((txn->final_lsn < two_phase_at) && is_commit)
	{
		/*
		 * txn must have been marked as a prepared transaction and skipped but
		 * not sent a prepare. Also, the prepare info must have been updated
		 * in txn even if we skip prepare.
		 *
		 * txn 必须已被标为已准备事务并且被跳过，但还没有发送 prepare。即使跳过 prepare，txn 中的
		 * prepare 信息也必须已经更新。
		 */
		Assert((txn->txn_flags & RBTXN_PREPARE_STATUS_MASK) ==
			   (RBTXN_IS_PREPARED | RBTXN_SKIPPED_PREPARE));
		Assert(txn->final_lsn != InvalidXLogRecPtr);

		/*
		 * By this time the txn has the prepare record information and it is
		 * important to use that so that downstream gets the accurate
		 * information. If instead, we have passed commit information here
		 * then downstream can behave as it has already replayed commit
		 * prepared after the restart.
		 *
		 * 此时 txn 已有 prepare 记录的信息，必须使用它，下游才能得到准确信息。若这里改传提交信息，
		 * 重启之后下游会表现得好像已经重放过 commit prepared。
		 */
		ReorderBufferReplay(txn, rb, xid, txn->final_lsn, txn->end_lsn,
							txn->xact_time.prepare_time, txn->origin_id, txn->origin_lsn);
	}

	txn->final_lsn = commit_lsn;
	txn->end_lsn = end_lsn;
	txn->xact_time.commit_time = commit_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	if (is_commit)
		rb->commit_prepared(rb, txn, commit_lsn);
	else
		rb->rollback_prepared(rb, txn, prepare_end_lsn, prepare_time);

	/* cleanup: make sure there's no cache pollution
	 *
	 * 清理：确保缓存没有被污染。
	 */
	ReorderBufferExecuteInvalidations(txn->ninvalidations,
									  txn->invalidations);
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * Abort a transaction that possibly has previous changes. Needs to be first
 * called for subtransactions and then for the toplevel xid.
 *
 * 中止一个可能已有先前变更的事务。必须先对子事务调用，再对顶层 xid 调用。
 *
 * NB: Transactions handled here have to have actively aborted (i.e. have
 * produced an abort record). Implicitly aborted transactions are handled via
 * ReorderBufferAbortOld(); transactions we're just not interested in, but
 * which have committed are handled in ReorderBufferForget().
 *
 * 注意：这里处理的事务必须是主动中止的（即产生了 abort 记录）。隐式中止的事务由 ReorderBufferAbortOld() 处
 * 理；我们不感兴趣但已经提交的事务由 ReorderBufferForget() 处理。
 *
 * This function purges this transaction and its contents from memory and
 * disk.
 *
 * 本函数从内存和磁盘上清除该事务及其内容。
 */
void
ReorderBufferAbort(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
				   TimestampTz abort_time)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown, nothing to remove
	 *
	 * 未知事务，没有什么可移除的。
	 */
	if (txn == NULL)
		return;

	txn->xact_time.abort_time = abort_time;

	/* For streamed transactions notify the remote node about the abort.
	 *
	 * 对已流式发送的事务，通知远端节点该事务中止。
	 */
	if (rbtxn_is_streamed(txn))
	{
		rb->stream_abort(rb, txn, lsn);

		/*
		 * We might have decoded changes for this transaction that could load
		 * the cache as per the current transaction's view (consider DDL's
		 * happened in this transaction). We don't want the decoding of future
		 * transactions to use those cache entries so execute only the inval
		 * messages in this transaction.
		 *
		 * 我们可能已经按当前事务的视图解码过变更，从而装入缓存（例如本事务中发生的 DDL）。不希望以
		 * 后的事务解码使用这些缓存项，因此只执行本事务中的失效消息。
		 */
		if (txn->ninvalidations > 0)
			ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
											   txn->invalidations);
	}

	/* cosmetic...
	 *
	 * 仅作记录，无功能影响。
	 */
	txn->final_lsn = lsn;

	/* remove potential on-disk data, and deallocate
	 *
	 * 删除可能存在的磁盘数据，并释放该事务。
	 */
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * Abort all transactions that aren't actually running anymore because the
 * server restarted.
 *
 * 中止所有因服务器重启而实际上不再运行的事务。
 *
 * NB: These really have to be transactions that have aborted due to a server
 * crash/immediate restart, as we don't deal with invalidations here.
 *
 * 注意：这些必须是因服务器崩溃或立即重启而中止的事务，因为这里不处理失效消息。
 */
void
ReorderBufferAbortOld(ReorderBuffer *rb, TransactionId oldestRunningXid)
{
	dlist_mutable_iter it;

	/*
	 * Iterate through all (potential) toplevel TXNs and abort all that are
	 * older than what possibly can be running. Once we've found the first
	 * that is alive we stop, there might be some that acquired an xid earlier
	 * but started writing later, but it's unlikely and they will be cleaned
	 * up in a later call to this function.
	 *
	 * 遍历所有（可能的）顶层 TXN，中止所有比仍可能在运行的事务更老的事务。一旦遇到第一个仍然存活的就停
	 * 止。有些事务可能更早获得 xid，但更晚才开始写，这种情况少见，会在以后再次调用本函数时清理。
	 */
	dlist_foreach_modify(it, &rb->toplevel_by_lsn)
	{
		ReorderBufferTXN *txn;

		txn = dlist_container(ReorderBufferTXN, node, it.cur);

		if (TransactionIdPrecedes(txn->xid, oldestRunningXid))
		{
			elog(DEBUG2, "aborting old transaction %u", txn->xid);

			/* Notify the remote node about the crash/immediate restart.
			 *
			 * 通知远端节点发生了崩溃或立即重启。
			 */
			if (rbtxn_is_streamed(txn))
				rb->stream_abort(rb, txn, InvalidXLogRecPtr);

			/* remove potential on-disk data, and deallocate this tx
			 *
			 * 删除可能存在的磁盘数据，并释放该事务。
			 */
			ReorderBufferCleanupTXN(rb, txn);
		}
		else
			return;
	}
}

/*
 * Forget the contents of a transaction if we aren't interested in its
 * contents. Needs to be first called for subtransactions and then for the
 * toplevel xid.
 *
 * 若我们对事务内容不感兴趣，就忘掉它。必须先对子事务调用，再对顶层 xid 调用。
 *
 * This is significantly different to ReorderBufferAbort() because
 * transactions that have committed need to be treated differently from aborted
 * ones since they may have modified the catalog.
 *
 * 这与 ReorderBufferAbort() 有重要区别：已提交的事务必须与已中止的事务区别对待，因为它们可能修改过系统目录。
 *
 * Note that this is only allowed to be called in the moment a transaction
 * commit has just been read, not earlier; otherwise later records referring
 * to this xid might re-create the transaction incompletely.
 *
 * 只允许在刚刚读到事务提交的那一刻调用，不能更早；否则此后引用该 xid 的记录可能不完整地重建该事务。
 */
void
ReorderBufferForget(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown, nothing to forget
	 *
	 * 未知事务，没有什么可忘掉的。
	 */
	if (txn == NULL)
		return;

	/* this transaction mustn't be streamed
	 *
	 * 该事务绝不能是已流式发送的。
	 */
	Assert(!rbtxn_is_streamed(txn));

	/* cosmetic...
	 *
	 * 仅作记录，无功能影响。
	 */
	txn->final_lsn = lsn;

	/*
	 * Process only cache invalidation messages in this transaction if there
	 * are any. Even if we're not interested in the transaction's contents, it
	 * could have manipulated the catalog and we need to update the caches
	 * according to that.
	 *
	 * 即使我们对事务内容不感兴趣，也只处理其中的缓存失效消息（若有）。它可能改过系统目录，缓存必须据此
	 * 更新。
	 */
	if (txn->base_snapshot != NULL && txn->ninvalidations > 0)
		ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
										   txn->invalidations);
	else
		Assert(txn->ninvalidations == 0);

	/* remove potential on-disk data, and deallocate
	 *
	 * 删除可能存在的磁盘数据，并释放该事务。
	 */
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * Invalidate cache for those transactions that need to be skipped just in case
 * catalogs were manipulated as part of the transaction.
 *
 * 对那些需要跳过的事务作废缓存，以防事务中操纵过系统目录。
 *
 * Note that this is a special-purpose function for prepared transactions where
 * we don't want to clean up the TXN even when we decide to skip it. See
 * DecodePrepare.
 *
 * 这是给已准备事务用的专用函数：即使决定跳过它，也不想清理该 TXN。参见 DecodePrepare。
 */
void
ReorderBufferInvalidate(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* unknown, nothing to do
	 *
	 * 未知事务，无需处理。
	 */
	if (txn == NULL)
		return;

	/*
	 * Process cache invalidation messages if there are any. Even if we're not
	 * interested in the transaction's contents, it could have manipulated the
	 * catalog and we need to update the caches according to that.
	 *
	 * 若有缓存失效消息就处理它们。即使我们对事务内容不感兴趣，它也可能改过系统目录，缓存必须据此更新。
	 */
	if (txn->base_snapshot != NULL && txn->ninvalidations > 0)
		ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
										   txn->invalidations);
	else
		Assert(txn->ninvalidations == 0);
}


/*
 * Execute invalidations happening outside the context of a decoded
 * transaction. That currently happens either for xid-less commits
 * (cf. RecordTransactionCommit()) or for invalidations in uninteresting
 * transactions (via ReorderBufferForget()).
 *
 * 执行发生在已解码事务上下文之外的失效。目前出现在两种情况：没有 xid 的提交（参见 RecordTransactionCommit()），
 * 或不感兴趣的事务中的失效（经由 ReorderBufferForget()）。
 */
void
ReorderBufferImmediateInvalidation(ReorderBuffer *rb, uint32 ninvalidations,
								   SharedInvalidationMessage *invalidations)
{
	bool		use_subtxn = IsTransactionOrTransactionBlock();
	int			i;

	if (use_subtxn)
		BeginInternalSubTransaction("replay");

	/*
	 * Force invalidations to happen outside of a valid transaction - that way
	 * entries will just be marked as invalid without accessing the catalog.
	 * That's advantageous because we don't need to setup the full state
	 * necessary for catalog access.
	 *
	 * 强制在有效事务之外执行失效，这样条目只会被标为无效，而不会访问目录。好处是不必为访问目录建立完整
	 * 状态。
	 */
	if (use_subtxn)
		AbortCurrentTransaction();

	for (i = 0; i < ninvalidations; i++)
		LocalExecuteInvalidationMessage(&invalidations[i]);

	if (use_subtxn)
		RollbackAndReleaseCurrentSubTransaction();
}

/*
 * Tell reorderbuffer about an xid seen in the WAL stream. Has to be called at
 * least once for every xid in XLogRecord->xl_xid (other places in records
 * may, but do not have to be passed through here).
 *
 * 把 WAL 流中见到的 xid 告诉 reorderbuffer。对 XLogRecord 的 xl_xid 中的每个 xid 至少要调用一次（记录中其他
 * 地方的 xid 可以传来，但不是必须）。
 *
 * Reorderbuffer keeps some data structures about transactions in LSN order,
 * for efficiency. To do that it has to know about when transactions are seen
 * first in the WAL. As many types of records are not actually interesting for
 * logical decoding, they do not necessarily pass through here.
 *
 * 为了效率，reorderbuffer 按 LSN 顺序保存一些事务数据结构。为此它必须知道事务在 WAL 中第一次出现的时刻。许
 * 多记录对逻辑解码其实没兴趣，因此不一定会经过这里。
 */
void
ReorderBufferProcessXid(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	/* many records won't have an xid assigned, centralize check here
	 *
	 * 许多记录没有分配 xid，把这个检查集中在这里。
	 */
	if (xid != InvalidTransactionId)
		ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);
}

/*
 * Add a new snapshot to this transaction that may only used after lsn 'lsn'
 * because the previous snapshot doesn't describe the catalog correctly for
 * following rows.
 *
 * 给该事务增加一个新快照。它只能在该 lsn 之后使用，因为之前的快照不能正确描述后续行的目录状态。
 */
void
ReorderBufferAddSnapshot(ReorderBuffer *rb, TransactionId xid,
						 XLogRecPtr lsn, Snapshot snap)
{
	ReorderBufferChange *change = ReorderBufferAllocChange(rb);

	change->data.snapshot = snap;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT;

	ReorderBufferQueueChange(rb, xid, lsn, change, false);
}

/*
 * Set up the transaction's base snapshot.
 *
 * 设置事务的基础快照。
 *
 * If we know that xid is a subtransaction, set the base snapshot on the
 * top-level transaction instead.
 *
 * 若已知 xid 是子事务，则改为在顶层事务上设置基础快照。
 */
void
ReorderBufferSetBaseSnapshot(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, Snapshot snap)
{
	ReorderBufferTXN *txn;
	bool		is_new;

	Assert(snap != NULL);

	/*
	 * Fetch the transaction to operate on.  If we know it's a subtransaction,
	 * operate on its top-level transaction instead.
	 *
	 * 取出要操作的事务。若已知它是子事务，则改为操作它的顶层事务。
	 */
	txn = ReorderBufferTXNByXid(rb, xid, true, &is_new, lsn, true);
	if (rbtxn_is_known_subxact(txn))
		txn = ReorderBufferTXNByXid(rb, txn->toplevel_xid, false,
									NULL, InvalidXLogRecPtr, false);
	Assert(txn->base_snapshot == NULL);

	txn->base_snapshot = snap;
	txn->base_snapshot_lsn = lsn;
	dlist_push_tail(&rb->txns_by_base_snapshot_lsn, &txn->base_snapshot_node);

	AssertTXNLsnOrder(rb);
}

/*
 * Access the catalog with this CommandId at this point in the changestream.
 *
 * 在变更流的这一点上，用这个 CommandId 访问目录。
 *
 * May only be called for command ids > 1
 *
 * 只能用于大于 1 的 command id。
 */
void
ReorderBufferAddNewCommandId(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, CommandId cid)
{
	ReorderBufferChange *change = ReorderBufferAllocChange(rb);

	change->data.command_id = cid;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID;

	ReorderBufferQueueChange(rb, xid, lsn, change, false);
}

/*
 * Update memory counters to account for the new or removed change.
 *
 * 更新内存计数，以计入新增或移除的变更。
 *
 * We update two counters - in the reorder buffer, and in the transaction
 * containing the change. The reorder buffer counter allows us to quickly
 * decide if we reached the memory limit, the transaction counter allows
 * us to quickly pick the largest transaction for eviction.
 *
 * 更新两个计数：reorder buffer 上的，以及包含该变更的事务上的。前者用来快速判断是否达到内存上限，后者用来快
 * 速选出最大的事务以便逐出。
 *
 * Either txn or change must be non-NULL at least. We update the memory
 * counter of txn if it's non-NULL, otherwise change->txn.
 *
 * txn 和 change 至少有一个非空。若 txn 非空，就更新它的内存计数，否则更新 change->txn。
 *
 * When streaming is enabled, we need to update the toplevel transaction
 * counters instead - we don't really care about subtransactions as we
 * can't stream them individually anyway, and we only pick toplevel
 * transactions for eviction. So only toplevel transactions matter.
 *
 * 启用流式传输时，需要改去更新顶层事务的计数。子事务反正不能单独流式发送，我们也只挑选顶层事务来逐出，所以
 * 只有顶层事务要紧。
 */
static void
ReorderBufferChangeMemoryUpdate(ReorderBuffer *rb,
								ReorderBufferChange *change,
								ReorderBufferTXN *txn,
								bool addition, Size sz)
{
	ReorderBufferTXN *toptxn;

	Assert(txn || change);

	/*
	 * Ignore tuple CID changes, because those are not evicted when reaching
	 * memory limit. So we just don't count them, because it might easily
	 * trigger a pointless attempt to spill.
	 *
	 * 忽略 tuple CID 变更，因为达到内存上限时不会逐出它们。干脆不把它们计入，否则很容易触发一次无意义的
	 * 溢出尝试。
	 */
	if (change && change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID)
		return;

	if (sz == 0)
		return;

	if (txn == NULL)
		txn = change->txn;
	Assert(txn != NULL);

	/*
	 * Update the total size in top level as well. This is later used to
	 * compute the decoding stats.
	 *
	 * 同时更新顶层的总大小。稍后用它计算解码统计。
	 */
	toptxn = rbtxn_get_toptxn(txn);

	if (addition)
	{
		Size		oldsize = txn->size;

		txn->size += sz;
		rb->size += sz;

		/* Update the total size in the top transaction.
		 *
		 * 更新顶层事务中的总大小。
		 */
		toptxn->total_size += sz;

		/* Update the max-heap
		 *
		 * 更新最大堆。
		 */
		if (oldsize != 0)
			pairingheap_remove(rb->txn_heap, &txn->txn_node);
		pairingheap_add(rb->txn_heap, &txn->txn_node);
	}
	else
	{
		Assert((rb->size >= sz) && (txn->size >= sz));
		txn->size -= sz;
		rb->size -= sz;

		/* Update the total size in the top transaction.
		 *
		 * 更新顶层事务中的总大小。
		 */
		toptxn->total_size -= sz;

		/* Update the max-heap
		 *
		 * 更新最大堆。
		 */
		pairingheap_remove(rb->txn_heap, &txn->txn_node);
		if (txn->size != 0)
			pairingheap_add(rb->txn_heap, &txn->txn_node);
	}

	Assert(txn->size <= rb->size);
}

/*
 * Add new (relfilelocator, tid) -> (cmin, cmax) mappings.
 *
 * 增加新的 (relfilelocator, tid) 到 (cmin, cmax) 映射。
 *
 * We do not include this change type in memory accounting, because we
 * keep CIDs in a separate list and do not evict them when reaching
 * the memory limit.
 *
 * 这种变更不计入内存，因为 CID 单独放在一条链表里，达到内存上限时也不会逐出它们。
 */
void
ReorderBufferAddNewTupleCids(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, RelFileLocator locator,
							 ItemPointerData tid, CommandId cmin,
							 CommandId cmax, CommandId combocid)
{
	ReorderBufferChange *change = ReorderBufferAllocChange(rb);
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	change->data.tuplecid.locator = locator;
	change->data.tuplecid.tid = tid;
	change->data.tuplecid.cmin = cmin;
	change->data.tuplecid.cmax = cmax;
	change->data.tuplecid.combocid = combocid;
	change->lsn = lsn;
	change->txn = txn;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID;

	dlist_push_tail(&txn->tuplecids, &change->node);
	txn->ntuplecids++;
}

/*
 * Add new invalidation messages to the reorder buffer queue.
 *
 * 把新的失效消息加入 reorder buffer 队列。
 */
static void
ReorderBufferQueueInvalidations(ReorderBuffer *rb, TransactionId xid,
								XLogRecPtr lsn, Size nmsgs,
								SharedInvalidationMessage *msgs)
{
	ReorderBufferChange *change;

	change = ReorderBufferAllocChange(rb);
	change->action = REORDER_BUFFER_CHANGE_INVALIDATION;
	change->data.inval.ninvalidations = nmsgs;
	change->data.inval.invalidations = (SharedInvalidationMessage *)
		palloc(sizeof(SharedInvalidationMessage) * nmsgs);
	memcpy(change->data.inval.invalidations, msgs,
		   sizeof(SharedInvalidationMessage) * nmsgs);

	ReorderBufferQueueChange(rb, xid, lsn, change, false);
}

/*
 * A helper function for ReorderBufferAddInvalidations() and
 * ReorderBufferAddDistributedInvalidations() to accumulate the invalidation
 * messages to the **invals_out.
 *
 * 供 ReorderBufferAddInvalidations() 和 ReorderBufferAddDistributedInvalidations() 使用的辅助函数，把失效消
 * 息累加到 invals_out。
 */
static void
ReorderBufferAccumulateInvalidations(SharedInvalidationMessage **invals_out,
									 uint32 *ninvals_out,
									 SharedInvalidationMessage *msgs_new,
									 Size nmsgs_new)
{
	if (*ninvals_out == 0)
	{
		*ninvals_out = nmsgs_new;
		*invals_out = (SharedInvalidationMessage *)
			palloc(sizeof(SharedInvalidationMessage) * nmsgs_new);
		memcpy(*invals_out, msgs_new, sizeof(SharedInvalidationMessage) * nmsgs_new);
	}
	else
	{
		/* Enlarge the array of inval messages
		 *
		 * 扩大失效消息数组。
		 */
		*invals_out = (SharedInvalidationMessage *)
			repalloc(*invals_out, sizeof(SharedInvalidationMessage) *
					 (*ninvals_out + nmsgs_new));
		memcpy(*invals_out + *ninvals_out, msgs_new,
			   nmsgs_new * sizeof(SharedInvalidationMessage));
		*ninvals_out += nmsgs_new;
	}
}

/*
 * Accumulate the invalidations for executing them later.
 *
 * 累加失效消息，留待以后执行。
 *
 * This needs to be called for each XLOG_XACT_INVALIDATIONS message and
 * accumulates all the invalidation messages in the toplevel transaction, if
 * available, otherwise in the current transaction, as well as in the form of
 * change in reorder buffer.  We require to record it in form of the change
 * so that we can execute only the required invalidations instead of executing
 * all the invalidations on each CommandId increment.  We also need to
 * accumulate these in the txn buffer because in some cases where we skip
 * processing the transaction (see ReorderBufferForget), we need to execute
 * all the invalidations together.
 *
 * 每条 XLOG_XACT_INVALIDATIONS 消息都要调用它，把全部失效消息累加到顶层事务（若有），否则累加到当前事务，同
 * 时也作为 reorder buffer 中的一条变更。必须记成变更，才能只执行需要的失效，而不是在每次 CommandId 增加时执
 * 行全部失效。也要在事务缓冲区里累加，因为有时会跳过该事务（参见 ReorderBufferForget），那时需要把全部失效
 * 一起执行。
 */
void
ReorderBufferAddInvalidations(ReorderBuffer *rb, TransactionId xid,
							  XLogRecPtr lsn, Size nmsgs,
							  SharedInvalidationMessage *msgs)
{
	ReorderBufferTXN *txn;
	MemoryContext oldcontext;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	oldcontext = MemoryContextSwitchTo(rb->context);

	/*
	 * Collect all the invalidations under the top transaction, if available,
	 * so that we can execute them all together.  See comments atop this
	 * function.
	 *
	 * 若有顶层事务，就把全部失效收集到它下面，以便一起执行。参见本函数顶部的注释。
	 */
	txn = rbtxn_get_toptxn(txn);

	Assert(nmsgs > 0);

	ReorderBufferAccumulateInvalidations(&txn->invalidations,
										 &txn->ninvalidations,
										 msgs, nmsgs);

	ReorderBufferQueueInvalidations(rb, xid, lsn, nmsgs, msgs);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accumulate the invalidations distributed by other committed transactions
 * for executing them later.
 *
 * 累加其他已提交事务分发来的失效消息，留待以后执行。
 *
 * This function is similar to ReorderBufferAddInvalidations() but stores
 * the given inval messages to the txn->invalidations_distributed with the
 * overflow check.
 *
 * 本函数类似 ReorderBufferAddInvalidations()，但把给定的失效消息存入 txn->invalidations_distributed，并做溢
 * 出检查。
 *
 * This needs to be called by committed transactions to distribute their
 * inval messages to in-progress transactions.
 *
 * 已提交的事务需要调用它，把自己的失效消息分发给进行中的事务。
 */
void
ReorderBufferAddDistributedInvalidations(ReorderBuffer *rb, TransactionId xid,
										 XLogRecPtr lsn, Size nmsgs,
										 SharedInvalidationMessage *msgs)
{
	ReorderBufferTXN *txn;
	MemoryContext oldcontext;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	oldcontext = MemoryContextSwitchTo(rb->context);

	/*
	 * Collect all the invalidations under the top transaction, if available,
	 * so that we can execute them all together.  See comments
	 * ReorderBufferAddInvalidations.
	 *
	 * 若有顶层事务，就把全部失效收集到它下面，以便一起执行。参见 ReorderBufferAddInvalidations 的注释。
	 */
	txn = rbtxn_get_toptxn(txn);

	Assert(nmsgs > 0);

	if (!rbtxn_distr_inval_overflowed(txn))
	{
		/*
		 * Check the transaction has enough space for storing distributed
		 * invalidation messages.
		 *
		 * 检查该事务是否还有足够空间存放分发来的失效消息。
		 */
		if (txn->ninvalidations_distributed + nmsgs >= MAX_DISTR_INVAL_MSG_PER_TXN)
		{
			/*
			 * Mark the invalidation message as overflowed and free up the
			 * messages accumulated so far.
			 *
			 * 把失效消息标为已溢出，并释放目前已经累加的消息。
			 */
			txn->txn_flags |= RBTXN_DISTR_INVAL_OVERFLOWED;

			if (txn->invalidations_distributed)
			{
				pfree(txn->invalidations_distributed);
				txn->invalidations_distributed = NULL;
				txn->ninvalidations_distributed = 0;
			}
		}
		else
			ReorderBufferAccumulateInvalidations(&txn->invalidations_distributed,
												 &txn->ninvalidations_distributed,
												 msgs, nmsgs);
	}

	/* Queue the invalidation messages into the transaction
	 *
	 * 把失效消息排入该事务。
	 */
	ReorderBufferQueueInvalidations(rb, xid, lsn, nmsgs, msgs);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Apply all invalidations we know. Possibly we only need parts at this point
 * in the changestream but we don't know which those are.
 *
 * 应用目前已知的全部失效。在变更流的这一点上也许只需要其中一部分，但我们不知道是哪一部分。
 */
static void
ReorderBufferExecuteInvalidations(uint32 nmsgs, SharedInvalidationMessage *msgs)
{
	int			i;

	for (i = 0; i < nmsgs; i++)
		LocalExecuteInvalidationMessage(&msgs[i]);
}

/*
 * Mark a transaction as containing catalog changes
 *
 * 把事务标为含有目录变更。
 */
void
ReorderBufferXidSetCatalogChanges(ReorderBuffer *rb, TransactionId xid,
								  XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	if (!rbtxn_has_catalog_changes(txn))
	{
		txn->txn_flags |= RBTXN_HAS_CATALOG_CHANGES;
		dclist_push_tail(&rb->catchange_txns, &txn->catchange_node);
	}

	/*
	 * Mark top-level transaction as having catalog changes too if one of its
	 * children has so that the ReorderBufferBuildTupleCidHash can
	 * conveniently check just top-level transaction and decide whether to
	 * build the hash table or not.
	 *
	 * 若某个子事务有目录变更，也把顶层事务标为有目录变更，这样 ReorderBufferBuildTupleCidHash 只需检查
	 * 顶层事务，就能决定要不要建哈希表。
	 */
	if (rbtxn_is_subtxn(txn))
	{
		ReorderBufferTXN *toptxn = rbtxn_get_toptxn(txn);

		if (!rbtxn_has_catalog_changes(toptxn))
		{
			toptxn->txn_flags |= RBTXN_HAS_CATALOG_CHANGES;
			dclist_push_tail(&rb->catchange_txns, &toptxn->catchange_node);
		}
	}
}

/*
 * Return palloc'ed array of the transactions that have changed catalogs.
 * The returned array is sorted in xidComparator order.
 *
 * 返回用 palloc 分配的、修改过系统目录的事务数组。数组按 xidComparator 排序。
 *
 * The caller must free the returned array when done with it.
 *
 * 调用方用完后必须释放返回的数组。
 */
TransactionId *
ReorderBufferGetCatalogChangesXacts(ReorderBuffer *rb)
{
	dlist_iter	iter;
	TransactionId *xids = NULL;
	size_t		xcnt = 0;

	/* Quick return if the list is empty
	 *
	 * 链表为空时快速返回。
	 */
	if (dclist_count(&rb->catchange_txns) == 0)
		return NULL;

	/* Initialize XID array
	 *
	 * 初始化 XID 数组。
	 */
	xids = (TransactionId *) palloc(sizeof(TransactionId) *
									dclist_count(&rb->catchange_txns));
	dclist_foreach(iter, &rb->catchange_txns)
	{
		ReorderBufferTXN *txn = dclist_container(ReorderBufferTXN,
												 catchange_node,
												 iter.cur);

		Assert(rbtxn_has_catalog_changes(txn));

		xids[xcnt++] = txn->xid;
	}

	qsort(xids, xcnt, sizeof(TransactionId), xidComparator);

	Assert(xcnt == dclist_count(&rb->catchange_txns));
	return xids;
}

/*
 * Query whether a transaction is already *known* to contain catalog
 * changes. This can be wrong until directly before the commit!
 *
 * 查询一个事务是否已经已知含有目录变更。在紧挨提交之前，这个答案仍可能是错的。
 */
bool
ReorderBufferXidHasCatalogChanges(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);
	if (txn == NULL)
		return false;

	return rbtxn_has_catalog_changes(txn);
}

/*
 * ReorderBufferXidHasBaseSnapshot
 *		Have we already set the base snapshot for the given txn/subtxn?
 *
 *		ReorderBufferXidHasBaseSnapshot：是否已经为给定事务或子事务设置了基础快照？
 */
bool
ReorderBufferXidHasBaseSnapshot(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false,
								NULL, InvalidXLogRecPtr, false);

	/* transaction isn't known yet, ergo no snapshot
	 *
	 * 事务尚不可知，因此没有快照。
	 */
	if (txn == NULL)
		return false;

	/* a known subtxn? operate on top-level txn instead
	 *
	 * 已知是子事务？改为操作顶层事务。
	 */
	if (rbtxn_is_known_subxact(txn))
		txn = ReorderBufferTXNByXid(rb, txn->toplevel_xid, false,
									NULL, InvalidXLogRecPtr, false);

	return txn->base_snapshot != NULL;
}


/*
 * ---------------------------------------
 * Disk serialization support
 *
 * 磁盘序列化支持。
 * ---------------------------------------
 */

/*
 * Ensure the IO buffer is >= sz.
 *
 * 确保 IO 缓冲区不小于 sz。
 */
static void
ReorderBufferSerializeReserve(ReorderBuffer *rb, Size sz)
{
	if (!rb->outbufsize)
	{
		rb->outbuf = MemoryContextAlloc(rb->context, sz);
		rb->outbufsize = sz;
	}
	else if (rb->outbufsize < sz)
	{
		rb->outbuf = repalloc(rb->outbuf, sz);
		rb->outbufsize = sz;
	}
}


/* Compare two transactions by size
 *
 * 按大小比较两个事务。
 */
static int
ReorderBufferTXNSizeCompare(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const ReorderBufferTXN *ta = pairingheap_const_container(ReorderBufferTXN, txn_node, a);
	const ReorderBufferTXN *tb = pairingheap_const_container(ReorderBufferTXN, txn_node, b);

	if (ta->size < tb->size)
		return -1;
	if (ta->size > tb->size)
		return 1;
	return 0;
}

/*
 * Find the largest transaction (toplevel or subxact) to evict (spill to disk).
 *
 * 找出最大的事务（顶层或子事务）以便逐出（溢出到磁盘）。
 */
static ReorderBufferTXN *
ReorderBufferLargestTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *largest;

	/* Get the largest transaction from the max-heap
	 *
	 * 从最大堆中取出最大的事务。
	 */
	largest = pairingheap_container(ReorderBufferTXN, txn_node,
									pairingheap_first(rb->txn_heap));

	Assert(largest);
	Assert(largest->size > 0);
	Assert(largest->size <= rb->size);

	return largest;
}

/*
 * Find the largest streamable (and non-aborted) toplevel transaction to evict
 * (by streaming).
 *
 * 找出最大的、可流式发送且未中止的顶层事务，以便通过流式发送将其逐出。
 *
 * This can be seen as an optimized version of ReorderBufferLargestTXN, which
 * should give us the same transaction (because we don't update memory account
 * for subtransaction with streaming, so it's always 0). But we can simply
 * iterate over the limited number of toplevel transactions that have a base
 * snapshot. There is no use of selecting a transaction that doesn't have base
 * snapshot because we don't decode such transactions.  Also, we do not select
 * the transaction which doesn't have any streamable change.
 *
 * 这可以看作 ReorderBufferLargestTXN 的优化版本，结果应当是同一个事务（启用流式传输时不为子事务更新内存记账，
 * 所以子事务大小总是 0）。但这里只需遍历数量有限的、拥有基础快照的顶层事务。没有基础快照的事务不会被解码，
 * 选它没有用。也不选择没有任何可流式变更的事务。
 *
 * Note that, we skip transactions that contain incomplete changes. There
 * is a scope of optimization here such that we can select the largest
 * transaction which has incomplete changes.  But that will make the code and
 * design quite complex and that might not be worth the benefit.  If we plan to
 * stream the transactions that contain incomplete changes then we need to
 * find a way to partially stream/truncate the transaction changes in-memory
 * and build a mechanism to partially truncate the spilled files.
 * Additionally, whenever we partially stream the transaction we need to
 * maintain the last streamed lsn and next time we need to restore from that
 * segment and the offset in WAL.  As we stream the changes from the top
 * transaction and restore them subtransaction wise, we need to even remember
 * the subxact from where we streamed the last change.
 *
 * 注意，我们跳过含有不完整变更的事务。这里还有优化余地：可以选择含有不完整变更的最大事务。但那会使代码和设
 * 计相当复杂，收益未必值得。若打算流式发送含有不完整变更的事务，就需要能在内存中部分流式发送或截断事务变更，
 * 并部分截断溢出文件。此外，每次部分流式发送时都要记住最后送出的 LSN，下次从该 WAL 段和偏移恢复。由于变更从
 * 顶层事务送出、却按子事务恢复，甚至还要记住最后一条变更来自哪个子事务。
 */
static ReorderBufferTXN *
ReorderBufferLargestStreamableTopTXN(ReorderBuffer *rb)
{
	dlist_iter	iter;
	Size		largest_size = 0;
	ReorderBufferTXN *largest = NULL;

	/* Find the largest top-level transaction having a base snapshot.
	 *
	 * 找出拥有基础快照的最大顶层事务。
	 */
	dlist_foreach(iter, &rb->txns_by_base_snapshot_lsn)
	{
		ReorderBufferTXN *txn;

		txn = dlist_container(ReorderBufferTXN, base_snapshot_node, iter.cur);

		/* must not be a subtxn
		 *
		 * 绝不能是子事务。
		 */
		Assert(!rbtxn_is_known_subxact(txn));
		/* base_snapshot must be set
		 *
		 * base_snapshot 必须已经设置。
		 */
		Assert(txn->base_snapshot != NULL);

		/* Don't consider these kinds of transactions for eviction.
		 *
		 * 不考虑这些类型的事务作为逐出对象。
		 */
		if (rbtxn_has_partial_change(txn) ||
			!rbtxn_has_streamable_change(txn) ||
			rbtxn_is_aborted(txn))
			continue;

		/* Find the largest of the eviction candidates.
		 *
		 * 在逐出候选中找出最大的一个。
		 */
		if ((largest == NULL || txn->total_size > largest_size) &&
			(txn->total_size > 0))
		{
			largest = txn;
			largest_size = txn->total_size;
		}
	}

	return largest;
}

/*
 * Check whether the logical_decoding_work_mem limit was reached, and if yes
 * pick the largest (sub)transaction at-a-time to evict and spill its changes to
 * disk or send to the output plugin until we reach under the memory limit.
 *
 * 检查是否达到 logical_decoding_work_mem 上限。若是，则每次挑出最大的（子）事务逐出，把它的变更溢出到磁盘或
 * 发给输出插件，直到降到内存上限以下。
 *
 * If debug_logical_replication_streaming is set to "immediate", stream or
 * serialize the changes immediately.
 *
 * 若 debug_logical_replication_streaming 设为 immediate，则立即流式发送或序列化变更。
 *
 * XXX At this point we select the transactions until we reach under the memory
 * limit, but we might also adapt a more elaborate eviction strategy - for example
 * evicting enough transactions to free certain fraction (e.g. 50%) of the memory
 * limit.
 *
 * XXX：目前只逐出事务直到降到内存上限以下，以后也可以采用更精细的策略，例如逐出足够多的事务，以释放内存上限
 * 的一定比例（例如百分之五十）。
 */
static void
ReorderBufferCheckMemoryLimit(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	/*
	 * Bail out if debug_logical_replication_streaming is buffered and we
	 * haven't exceeded the memory limit.
	 *
	 * 若 debug_logical_replication_streaming 为 buffered，且尚未超过内存上限，则直接返回。
	 */
	if (debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_BUFFERED &&
		rb->size < logical_decoding_work_mem * (Size) 1024)
		return;

	/*
	 * If debug_logical_replication_streaming is immediate, loop until there's
	 * no change. Otherwise, loop until we reach under the memory limit. One
	 * might think that just by evicting the largest (sub)transaction we will
	 * come under the memory limit based on assumption that the selected
	 * transaction is at least as large as the most recent change (which
	 * caused us to go over the memory limit). However, that is not true
	 * because a user can reduce the logical_decoding_work_mem to a smaller
	 * value before the most recent change.
	 *
	 * 若 debug_logical_replication_streaming 为 immediate，则循环直到没有任何变更。否则循环直到降到内存
	 * 上限以下。也许会以为只逐出最大的（子）事务就能降到上限以下，因为被选中的事务至少和最近那条导致超
	 * 限的变更一样大。这并不成立，因为用户可以在最近这条变更之前把 logical_decoding_work_mem 调小。
	 */
	while (rb->size >= logical_decoding_work_mem * (Size) 1024 ||
		   (debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE &&
			rb->size > 0))
	{
		/*
		 * Pick the largest non-aborted transaction and evict it from memory
		 * by streaming, if possible.  Otherwise, spill to disk.
		 *
		 * 尽可能选出最大的未中止事务，通过流式发送把它从内存逐出；否则溢出到磁盘。
		 */
		if (ReorderBufferCanStartStreaming(rb) &&
			(txn = ReorderBufferLargestStreamableTopTXN(rb)) != NULL)
		{
			/* we know there has to be one, because the size is not zero
			 *
			 * 大小不是零，因此必然存在这样一个事务。
			 */
			Assert(txn && rbtxn_is_toptxn(txn));
			Assert(txn->total_size > 0);
			Assert(rb->size >= txn->total_size);

			/* skip the transaction if aborted
			 *
			 * 若事务已中止则跳过。
			 */
			if (ReorderBufferCheckAndTruncateAbortedTXN(rb, txn))
				continue;

			ReorderBufferStreamTXN(rb, txn);
		}
		else
		{
			/*
			 * Pick the largest transaction (or subtransaction) and evict it
			 * from memory by serializing it to disk.
			 *
			 * 选出最大的事务（或子事务），通过序列化到磁盘把它从内存逐出。
			 */
			txn = ReorderBufferLargestTXN(rb);

			/* we know there has to be one, because the size is not zero
			 *
			 * 大小不是零，因此必然存在这样一个事务。
			 */
			Assert(txn);
			Assert(txn->size > 0);
			Assert(rb->size >= txn->size);

			/* skip the transaction if aborted
			 *
			 * 若事务已中止则跳过。
			 */
			if (ReorderBufferCheckAndTruncateAbortedTXN(rb, txn))
				continue;

			ReorderBufferSerializeTXN(rb, txn);
		}

		/*
		 * After eviction, the transaction should have no entries in memory,
		 * and should use 0 bytes for changes.
		 *
		 * 逐出之后，该事务在内存中不应再有条目，变更占用的字节数应为 0。
		 */
		Assert(txn->size == 0);
		Assert(txn->nentries_mem == 0);
	}

	/* We must be under the memory limit now.
	 *
	 * 此时必须已经低于内存上限。
	 */
	Assert(rb->size < logical_decoding_work_mem * (Size) 1024);
}

/*
 * Spill data of a large transaction (and its subtransactions) to disk.
 *
 * 把大事务（及其子事务）的数据溢出到磁盘。
 */
static void
ReorderBufferSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	dlist_iter	subtxn_i;
	dlist_mutable_iter change_i;
	int			fd = -1;
	XLogSegNo	curOpenSegNo = 0;
	Size		spilled = 0;
	Size		size = txn->size;

	elog(DEBUG2, "spill %u changes in XID %u to disk",
		 (uint32) txn->nentries_mem, txn->xid);

	/* do the same to all child TXs
	 *
	 * 对所有子事务做同样的事。
	 */
	dlist_foreach(subtxn_i, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, subtxn_i.cur);
		ReorderBufferSerializeTXN(rb, subtxn);
	}

	/* serialize changestream
	 *
	 * 序列化变更流。
	 */
	dlist_foreach_modify(change_i, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, change_i.cur);

		/*
		 * store in segment in which it belongs by start lsn, don't split over
		 * multiple segments tho
		 *
		 * 按起始 LSN 存入所属的段，但不要拆到多个段里。
		 */
		if (fd == -1 ||
			!XLByteInSeg(change->lsn, curOpenSegNo, wal_segment_size))
		{
			char		path[MAXPGPATH];

			if (fd != -1)
				CloseTransientFile(fd);

			XLByteToSeg(change->lsn, curOpenSegNo, wal_segment_size);

			/*
			 * No need to care about TLIs here, only used during a single run,
			 * so each LSN only maps to a specific WAL record.
			 *
			 * 这里不必关心时间线。只在单次运行中使用，因此每个 LSN 只对应一条特定的 WAL 记录。
			 */
			ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid,
										curOpenSegNo);

			/* open segment, create it if necessary
			 *
			 * 打开段文件，必要时创建它。
			 */
			fd = OpenTransientFile(path,
								   O_CREAT | O_WRONLY | O_APPEND | PG_BINARY);

			if (fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));
		}

		ReorderBufferSerializeChange(rb, txn, fd, change);
		dlist_delete(&change->node);
		ReorderBufferFreeChange(rb, change, false);

		spilled++;
	}

	/* Update the memory counter
	 *
	 * 更新内存计数。
	 */
	ReorderBufferChangeMemoryUpdate(rb, NULL, txn, false, size);

	/* update the statistics iff we have spilled anything
	 *
	 * 只有确实溢出了内容时才更新统计。
	 */
	if (spilled)
	{
		rb->spillCount += 1;
		rb->spillBytes += size;

		/* don't consider already serialized transactions
		 *
		 * 不要把已经序列化过的事务再算进去。
		 */
		rb->spillTxns += (rbtxn_is_serialized(txn) || rbtxn_is_serialized_clear(txn)) ? 0 : 1;

		/* update the decoding stats
		 *
		 * 更新解码统计。
		 */
		UpdateDecodingStats((LogicalDecodingContext *) rb->private_data);
	}

	Assert(spilled == txn->nentries_mem);
	Assert(dlist_is_empty(&txn->changes));
	txn->nentries_mem = 0;
	txn->txn_flags |= RBTXN_IS_SERIALIZED;

	if (fd != -1)
		CloseTransientFile(fd);
}

/*
 * Serialize individual change to disk.
 *
 * 把单条变更序列化到磁盘。
 */
static void
ReorderBufferSerializeChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
							 int fd, ReorderBufferChange *change)
{
	ReorderBufferDiskChange *ondisk;
	Size		sz = sizeof(ReorderBufferDiskChange);

	ReorderBufferSerializeReserve(rb, sz);

	ondisk = (ReorderBufferDiskChange *) rb->outbuf;
	memcpy(&ondisk->change, change, sizeof(ReorderBufferChange));

	switch (change->action)
	{
			/* fall through these, they're all similar enough
			 *
			 * 这些分支足够相似，一并处理。
			 */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			{
				char	   *data;
				HeapTuple	oldtup,
							newtup;
				Size		oldlen = 0;
				Size		newlen = 0;

				oldtup = change->data.tp.oldtuple;
				newtup = change->data.tp.newtuple;

				if (oldtup)
				{
					sz += sizeof(HeapTupleData);
					oldlen = oldtup->t_len;
					sz += oldlen;
				}

				if (newtup)
				{
					sz += sizeof(HeapTupleData);
					newlen = newtup->t_len;
					sz += newlen;
				}

				/* make sure we have enough space
				 *
				 * 确保有足够空间。
				 */
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* might have been reallocated above
				 *
				 * 上面可能已经重新分配过。
				 */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				if (oldlen)
				{
					memcpy(data, oldtup, sizeof(HeapTupleData));
					data += sizeof(HeapTupleData);

					memcpy(data, oldtup->t_data, oldlen);
					data += oldlen;
				}

				if (newlen)
				{
					memcpy(data, newtup, sizeof(HeapTupleData));
					data += sizeof(HeapTupleData);

					memcpy(data, newtup->t_data, newlen);
					data += newlen;
				}
				break;
			}
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				char	   *data;
				Size		prefix_size = strlen(change->data.msg.prefix) + 1;

				sz += prefix_size + change->data.msg.message_size +
					sizeof(Size) + sizeof(Size);
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);

				/* might have been reallocated above
				 *
				 * 上面可能已经重新分配过。
				 */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				/* write the prefix including the size
				 *
				 * 写入前缀，包括其长度。
				 */
				memcpy(data, &prefix_size, sizeof(Size));
				data += sizeof(Size);
				memcpy(data, change->data.msg.prefix,
					   prefix_size);
				data += prefix_size;

				/* write the message including the size
				 *
				 * 写入消息，包括其长度。
				 */
				memcpy(data, &change->data.msg.message_size, sizeof(Size));
				data += sizeof(Size);
				memcpy(data, change->data.msg.message,
					   change->data.msg.message_size);
				data += change->data.msg.message_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			{
				char	   *data;
				Size		inval_size = sizeof(SharedInvalidationMessage) *
					change->data.inval.ninvalidations;

				sz += inval_size;

				ReorderBufferSerializeReserve(rb, sz);
				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);

				/* might have been reallocated above
				 *
				 * 上面可能已经重新分配过。
				 */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;
				memcpy(data, change->data.inval.invalidations, inval_size);
				data += inval_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	snap;
				char	   *data;

				snap = change->data.snapshot;

				sz += sizeof(SnapshotData) +
					sizeof(TransactionId) * snap->xcnt +
					sizeof(TransactionId) * snap->subxcnt;

				/* make sure we have enough space
				 *
				 * 确保有足够空间。
				 */
				ReorderBufferSerializeReserve(rb, sz);
				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* might have been reallocated above
				 *
				 * 上面可能已经重新分配过。
				 */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				memcpy(data, snap, sizeof(SnapshotData));
				data += sizeof(SnapshotData);

				if (snap->xcnt)
				{
					memcpy(data, snap->xip,
						   sizeof(TransactionId) * snap->xcnt);
					data += sizeof(TransactionId) * snap->xcnt;
				}

				if (snap->subxcnt)
				{
					memcpy(data, snap->subxip,
						   sizeof(TransactionId) * snap->subxcnt);
					data += sizeof(TransactionId) * snap->subxcnt;
				}
				break;
			}
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				Size		size;
				char	   *data;

				/* account for the OIDs of truncated relations
				 *
				 * 计入被截断关系的 OID。
				 */
				size = sizeof(Oid) * change->data.truncate.nrelids;
				sz += size;

				/* make sure we have enough space
				 *
				 * 确保有足够空间。
				 */
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* might have been reallocated above
				 *
				 * 上面可能已经重新分配过。
				 */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				memcpy(data, change->data.truncate.relids, size);
				data += size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			/* ReorderBufferChange contains everything important
			 *
			 * ReorderBufferChange 已包含所有重要内容。
			 */
			break;
	}

	ondisk->size = sz;

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_REORDER_BUFFER_WRITE);
	if (write(fd, rb->outbuf, ondisk->size) != ondisk->size)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		/* if write didn't set errno, assume problem is no disk space
		 *
		 * 若 write 没有设置 errno，就假定问题是磁盘空间不足。
		 */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to data file for XID %u: %m",
						txn->xid)));
	}
	pgstat_report_wait_end();

	/*
	 * Keep the transaction's final_lsn up to date with each change we send to
	 * disk, so that ReorderBufferRestoreCleanup works correctly.  (We used to
	 * only do this on commit and abort records, but that doesn't work if a
	 * system crash leaves a transaction without its abort record).
	 *
	 * 每把一条变更写到磁盘，就更新事务的 final_lsn，这样 ReorderBufferRestoreCleanup 才能正确工作。（以
	 * 前只在 commit 和 abort 记录上做这件事，但若系统崩溃留下一个没有 abort 记录的事务，那样就行不通。）
	 *
	 * Make sure not to move it backwards.
	 *
	 * 确保不要把它往回移动。
	 */
	if (txn->final_lsn < change->lsn)
		txn->final_lsn = change->lsn;

	Assert(ondisk->change.action == change->action);
}

/* Returns true, if the output plugin supports streaming, false, otherwise.
 *
 * 若输出插件支持流式传输则返回真，否则返回假。
 */
static inline bool
ReorderBufferCanStream(ReorderBuffer *rb)
{
	LogicalDecodingContext *ctx = rb->private_data;

	return ctx->streaming;
}

/* Returns true, if the streaming can be started now, false, otherwise.
 *
 * 若现在可以开始流式传输则返回真，否则返回假。
 */
static inline bool
ReorderBufferCanStartStreaming(ReorderBuffer *rb)
{
	LogicalDecodingContext *ctx = rb->private_data;
	SnapBuild  *builder = ctx->snapshot_builder;

	/* We can't start streaming unless a consistent state is reached.
	 *
	 * 未达到一致性状态就不能开始流式传输。
	 */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_CONSISTENT)
		return false;

	/*
	 * We can't start streaming immediately even if the streaming is enabled
	 * because we previously decoded this transaction and now just are
	 * restarting.
	 *
	 * 即使启用了流式传输也不能立刻开始，因为我们先前已经解码过该事务，现在只是在重启。
	 */
	if (ReorderBufferCanStream(rb) &&
		!SnapBuildXactNeedsSkip(builder, ctx->reader->ReadRecPtr))
		return true;

	return false;
}

/*
 * Send data of a large transaction (and its subtransactions) to the
 * output plugin, but using the stream API.
 *
 * 把大事务（及其子事务）的数据发给输出插件，但使用流式 API。
 */
static void
ReorderBufferStreamTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	Snapshot	snapshot_now;
	CommandId	command_id;
	Size		stream_bytes;
	bool		txn_is_streamed;

	/* We can never reach here for a subtransaction.
	 *
	 * 子事务绝不可能执行到这里。
	 */
	Assert(rbtxn_is_toptxn(txn));

	/*
	 * We can't make any assumptions about base snapshot here, similar to what
	 * ReorderBufferCommit() does. That relies on base_snapshot getting
	 * transferred from subxact in ReorderBufferCommitChild(), but that was
	 * not yet called as the transaction is in-progress.
	 *
	 * 这里不能对基础快照作任何假定，这与 ReorderBufferCommit() 类似。后者依赖
	 * ReorderBufferCommitChild() 把 base_snapshot 从子事务转移过来，但事务仍在进行，那个函数还没有被调
	 * 用。
	 *
	 * So just walk the subxacts and use the same logic here. But we only need
	 * to do that once, when the transaction is streamed for the first time.
	 * After that we need to reuse the snapshot from the previous run.
	 *
	 * 因此在这里遍历子事务并使用同样的逻辑。但只需要在事务第一次流式发送时做一次。之后要复用上一轮的快
	 * 照。
	 *
	 * Unlike DecodeCommit which adds xids of all the subtransactions in
	 * snapshot's xip array via SnapBuildCommitTxn, we can't do that here but
	 * we do add them to subxip array instead via ReorderBufferCopySnap. This
	 * allows the catalog changes made in subtransactions decoded till now to
	 * be visible.
	 *
	 * DecodeCommit 会通过 SnapBuildCommitTxn 把所有子事务的 xid 加入快照的 xip 数组，这里做不到。我们改
	 * 为通过 ReorderBufferCopySnap 把它们加入 subxip 数组。这样，到目前为止在子事务中解码出的目录变更仍
	 * 然可见。
	 */
	if (txn->snapshot_now == NULL)
	{
		dlist_iter	subxact_i;

		/* make sure this transaction is streamed for the first time
		 *
		 * 确认该事务是第一次被流式发送。
		 */
		Assert(!rbtxn_is_streamed(txn));

		/* at the beginning we should have invalid command ID
		 *
		 * 一开始 command ID 应当是无效的。
		 */
		Assert(txn->command_id == InvalidCommandId);

		dlist_foreach(subxact_i, &txn->subtxns)
		{
			ReorderBufferTXN *subtxn;

			subtxn = dlist_container(ReorderBufferTXN, node, subxact_i.cur);
			ReorderBufferTransferSnapToParent(txn, subtxn);
		}

		/*
		 * If this transaction has no snapshot, it didn't make any changes to
		 * the database till now, so there's nothing to decode.
		 *
		 * 若该事务没有快照，说明它到目前为止没有修改数据库，因此没有什么可解码的。
		 */
		if (txn->base_snapshot == NULL)
		{
			Assert(txn->ninvalidations == 0);
			return;
		}

		command_id = FirstCommandId;
		snapshot_now = ReorderBufferCopySnap(rb, txn->base_snapshot,
											 txn, command_id);
	}
	else
	{
		/* the transaction must have been already streamed
		 *
		 * 该事务必须已经流式发送过。
		 */
		Assert(rbtxn_is_streamed(txn));

		/*
		 * Nah, we already have snapshot from the previous streaming run. We
		 * assume new subxacts can't move the LSN backwards, and so can't beat
		 * the LSN condition in the previous branch (so no need to walk
		 * through subxacts again). In fact, we must not do that as we may be
		 * using snapshot half-way through the subxact.
		 *
		 * 上一轮流式传输已经留下快照。假定新的子事务不能把 LSN 往回移，因此也不会打破上一分支的 LSN
		 * 条件（不必再遍历子事务）。事实上也不能再遍历，因为我们可能正在使用子事务进行到一半时的快
		 * 照。
		 */
		command_id = txn->command_id;

		/*
		 * We can't use txn->snapshot_now directly because after the last
		 * streaming run, we might have got some new sub-transactions. So we
		 * need to add them to the snapshot.
		 *
		 * 不能直接使用 txn->snapshot_now，因为上一轮流式传输之后可能出现了新的子事务，需要把它们加
		 * 进快照。
		 */
		snapshot_now = ReorderBufferCopySnap(rb, txn->snapshot_now,
											 txn, command_id);

		/* Free the previously copied snapshot.
		 *
		 * 释放先前复制的快照。
		 */
		Assert(txn->snapshot_now->copied);
		ReorderBufferFreeSnap(rb, txn->snapshot_now);
		txn->snapshot_now = NULL;
	}

	/*
	 * Remember this information to be used later to update stats. We can't
	 * update the stats here as an error while processing the changes would
	 * lead to the accumulation of stats even though we haven't streamed all
	 * the changes.
	 *
	 * 记住这些信息，稍后用来更新统计。不能在这里更新：处理变更时若出错，统计会被累加，但变更其实还没有
	 * 全部流式送出。
	 */
	txn_is_streamed = rbtxn_is_streamed(txn);
	stream_bytes = txn->total_size;

	/* Process and send the changes to output plugin.
	 *
	 * 处理变更并发送给输出插件。
	 */
	ReorderBufferProcessTXN(rb, txn, InvalidXLogRecPtr, snapshot_now,
							command_id, true);

	rb->streamCount += 1;
	rb->streamBytes += stream_bytes;

	/* Don't consider already streamed transaction.
	 *
	 * 不要把已经流式发送过的事务再算进去。
	 */
	rb->streamTxns += (txn_is_streamed) ? 0 : 1;

	/* update the decoding stats
	 *
	 * 更新解码统计。
	 */
	UpdateDecodingStats((LogicalDecodingContext *) rb->private_data);

	Assert(dlist_is_empty(&txn->changes));
	Assert(txn->nentries == 0);
	Assert(txn->nentries_mem == 0);
}

/*
 * Size of a change in memory.
 *
 * 一条变更在内存中的大小。
 */
static Size
ReorderBufferChangeSize(ReorderBufferChange *change)
{
	Size		sz = sizeof(ReorderBufferChange);

	switch (change->action)
	{
			/* fall through these, they're all similar enough
			 *
			 * 这些分支足够相似，一并处理。
			 */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			{
				HeapTuple	oldtup,
							newtup;
				Size		oldlen = 0;
				Size		newlen = 0;

				oldtup = change->data.tp.oldtuple;
				newtup = change->data.tp.newtuple;

				if (oldtup)
				{
					sz += sizeof(HeapTupleData);
					oldlen = oldtup->t_len;
					sz += oldlen;
				}

				if (newtup)
				{
					sz += sizeof(HeapTupleData);
					newlen = newtup->t_len;
					sz += newlen;
				}

				break;
			}
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				Size		prefix_size = strlen(change->data.msg.prefix) + 1;

				sz += prefix_size + change->data.msg.message_size +
					sizeof(Size) + sizeof(Size);

				break;
			}
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			{
				sz += sizeof(SharedInvalidationMessage) *
					change->data.inval.ninvalidations;
				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	snap;

				snap = change->data.snapshot;

				sz += sizeof(SnapshotData) +
					sizeof(TransactionId) * snap->xcnt +
					sizeof(TransactionId) * snap->subxcnt;

				break;
			}
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				sz += sizeof(Oid) * change->data.truncate.nrelids;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			/* ReorderBufferChange contains everything important
			 *
			 * ReorderBufferChange 已包含所有重要内容。
			 */
			break;
	}

	return sz;
}


/*
 * Restore a number of changes spilled to disk back into memory.
 *
 * 把若干条溢出到磁盘的变更恢复回内存。
 */
static Size
ReorderBufferRestoreChanges(ReorderBuffer *rb, ReorderBufferTXN *txn,
							TXNEntryFile *file, XLogSegNo *segno)
{
	Size		restored = 0;
	XLogSegNo	last_segno;
	dlist_mutable_iter cleanup_iter;
	File	   *fd = &file->vfd;

	Assert(txn->first_lsn != InvalidXLogRecPtr);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	/* free current entries, so we have memory for more
	 *
	 * 释放当前条目，以便为更多内容腾出内存。
	 */
	dlist_foreach_modify(cleanup_iter, &txn->changes)
	{
		ReorderBufferChange *cleanup =
			dlist_container(ReorderBufferChange, node, cleanup_iter.cur);

		dlist_delete(&cleanup->node);
		ReorderBufferFreeChange(rb, cleanup, true);
	}
	txn->nentries_mem = 0;
	Assert(dlist_is_empty(&txn->changes));

	XLByteToSeg(txn->final_lsn, last_segno, wal_segment_size);

	while (restored < max_changes_in_memory && *segno <= last_segno)
	{
		int			readBytes;
		ReorderBufferDiskChange *ondisk;

		CHECK_FOR_INTERRUPTS();

		if (*fd == -1)
		{
			char		path[MAXPGPATH];

			/* first time in
			 *
			 * 第一次进入。
			 */
			if (*segno == 0)
				XLByteToSeg(txn->first_lsn, *segno, wal_segment_size);

			Assert(*segno != 0 || dlist_is_empty(&txn->changes));

			/*
			 * No need to care about TLIs here, only used during a single run,
			 * so each LSN only maps to a specific WAL record.
			 *
			 * 这里不必关心时间线。只在单次运行中使用，因此每个 LSN 只对应一条特定的 WAL 记录。
			 */
			ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid,
										*segno);

			*fd = PathNameOpenFile(path, O_RDONLY | PG_BINARY);

			/* No harm in resetting the offset even in case of failure
			 *
			 * 即使失败，把偏移重置掉也没有害处。
			 */
			file->curOffset = 0;

			if (*fd < 0 && errno == ENOENT)
			{
				*fd = -1;
				(*segno)++;
				continue;
			}
			else if (*fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								path)));
		}

		/*
		 * Read the statically sized part of a change which has information
		 * about the total size. If we couldn't read a record, we're at the
		 * end of this file.
		 *
		 * 读取变更中长度固定的部分，其中含有总大小。若读不到一条记录，说明到了该文件末尾。
		 */
		ReorderBufferSerializeReserve(rb, sizeof(ReorderBufferDiskChange));
		readBytes = FileRead(file->vfd, rb->outbuf,
							 sizeof(ReorderBufferDiskChange),
							 file->curOffset, WAIT_EVENT_REORDER_BUFFER_READ);

		/* eof */
		/*
		 *
		 * 文件结束。
		 */
		if (readBytes == 0)
		{
			FileClose(*fd);
			*fd = -1;
			(*segno)++;
			continue;
		}
		else if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: %m")));
		else if (readBytes != sizeof(ReorderBufferDiskChange))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: read %d instead of %u bytes",
							readBytes,
							(uint32) sizeof(ReorderBufferDiskChange))));

		file->curOffset += readBytes;

		ondisk = (ReorderBufferDiskChange *) rb->outbuf;

		ReorderBufferSerializeReserve(rb,
									  sizeof(ReorderBufferDiskChange) + ondisk->size);
		ondisk = (ReorderBufferDiskChange *) rb->outbuf;

		readBytes = FileRead(file->vfd,
							 rb->outbuf + sizeof(ReorderBufferDiskChange),
							 ondisk->size - sizeof(ReorderBufferDiskChange),
							 file->curOffset,
							 WAIT_EVENT_REORDER_BUFFER_READ);

		if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: %m")));
		else if (readBytes != ondisk->size - sizeof(ReorderBufferDiskChange))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: read %d instead of %u bytes",
							readBytes,
							(uint32) (ondisk->size - sizeof(ReorderBufferDiskChange)))));

		file->curOffset += readBytes;

		/*
		 * ok, read a full change from disk, now restore it into proper
		 * in-memory format
		 *
		 * 已经从磁盘读到一条完整变更，现在把它恢复成正确的内存格式。
		 */
		ReorderBufferRestoreChange(rb, txn, rb->outbuf);
		restored++;
	}

	return restored;
}

/*
 * Convert change from its on-disk format to in-memory format and queue it onto
 * the TXN's ->changes list.
 *
 * 把变更从磁盘格式转换成内存格式，并挂到该 TXN 的 ->changes 链表上。
 *
 * Note: although "data" is declared char*, at entry it points to a
 * maxalign'd buffer, making it safe in most of this function to assume
 * that the pointed-to data is suitably aligned for direct access.
 *
 * 虽然 data 声明为 char 指针，但进入函数时它指向按最大对齐分配的缓冲区，因此在本函数的大部分地方，可以安全
 * 地假定所指数据已适当对齐，可以直接访问。
 */
static void
ReorderBufferRestoreChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
						   char *data)
{
	ReorderBufferDiskChange *ondisk;
	ReorderBufferChange *change;

	ondisk = (ReorderBufferDiskChange *) data;

	change = ReorderBufferAllocChange(rb);

	/* copy static part
	 *
	 * 复制静态部分。
	 */
	memcpy(change, &ondisk->change, sizeof(ReorderBufferChange));

	data += sizeof(ReorderBufferDiskChange);

	/* restore individual stuff
	 *
	 * 恢复各自的附加数据。
	 */
	switch (change->action)
	{
			/* fall through these, they're all similar enough
			 *
			 * 这些分支足够相似，一并处理。
			 */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			if (change->data.tp.oldtuple)
			{
				uint32		tuplelen = ((HeapTuple) data)->t_len;

				change->data.tp.oldtuple =
					ReorderBufferAllocTupleBuf(rb, tuplelen - SizeofHeapTupleHeader);

				/* restore ->tuple
				 *
				 * 恢复 tuple。
				 */
				memcpy(change->data.tp.oldtuple, data,
					   sizeof(HeapTupleData));
				data += sizeof(HeapTupleData);

				/* reset t_data pointer into the new tuplebuf
				 *
				 * 把 t_data 指针重置到新的元组缓冲区中。
				 */
				change->data.tp.oldtuple->t_data =
					(HeapTupleHeader) ((char *) change->data.tp.oldtuple + HEAPTUPLESIZE);

				/* restore tuple data itself
				 *
				 * 恢复元组数据本身。
				 */
				memcpy(change->data.tp.oldtuple->t_data, data, tuplelen);
				data += tuplelen;
			}

			if (change->data.tp.newtuple)
			{
				/* here, data might not be suitably aligned!
				 *
				 * 此处 data 可能没有适当对齐。
				 */
				uint32		tuplelen;

				memcpy(&tuplelen, data + offsetof(HeapTupleData, t_len),
					   sizeof(uint32));

				change->data.tp.newtuple =
					ReorderBufferAllocTupleBuf(rb, tuplelen - SizeofHeapTupleHeader);

				/* restore ->tuple
				 *
				 * 恢复 tuple。
				 */
				memcpy(change->data.tp.newtuple, data,
					   sizeof(HeapTupleData));
				data += sizeof(HeapTupleData);

				/* reset t_data pointer into the new tuplebuf
				 *
				 * 把 t_data 指针重置到新的元组缓冲区中。
				 */
				change->data.tp.newtuple->t_data =
					(HeapTupleHeader) ((char *) change->data.tp.newtuple + HEAPTUPLESIZE);

				/* restore tuple data itself
				 *
				 * 恢复元组数据本身。
				 */
				memcpy(change->data.tp.newtuple->t_data, data, tuplelen);
				data += tuplelen;
			}

			break;
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				Size		prefix_size;

				/* read prefix
				 *
				 * 读取前缀。
				 */
				memcpy(&prefix_size, data, sizeof(Size));
				data += sizeof(Size);
				change->data.msg.prefix = MemoryContextAlloc(rb->context,
															 prefix_size);
				memcpy(change->data.msg.prefix, data, prefix_size);
				Assert(change->data.msg.prefix[prefix_size - 1] == '\0');
				data += prefix_size;

				/* read the message
				 *
				 * 读取消息。
				 */
				memcpy(&change->data.msg.message_size, data, sizeof(Size));
				data += sizeof(Size);
				change->data.msg.message = MemoryContextAlloc(rb->context,
															  change->data.msg.message_size);
				memcpy(change->data.msg.message, data,
					   change->data.msg.message_size);
				data += change->data.msg.message_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			{
				Size		inval_size = sizeof(SharedInvalidationMessage) *
					change->data.inval.ninvalidations;

				change->data.inval.invalidations =
					MemoryContextAlloc(rb->context, inval_size);

				/* read the message
				 *
				 * 读取消息。
				 */
				memcpy(change->data.inval.invalidations, data, inval_size);

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	oldsnap;
				Snapshot	newsnap;
				Size		size;

				oldsnap = (Snapshot) data;

				size = sizeof(SnapshotData) +
					sizeof(TransactionId) * oldsnap->xcnt +
					sizeof(TransactionId) * (oldsnap->subxcnt + 0);

				change->data.snapshot = MemoryContextAllocZero(rb->context, size);

				newsnap = change->data.snapshot;

				memcpy(newsnap, data, size);
				newsnap->xip = (TransactionId *)
					(((char *) newsnap) + sizeof(SnapshotData));
				newsnap->subxip = newsnap->xip + newsnap->xcnt;
				newsnap->copied = true;
				break;
			}
			/* the base struct contains all the data, easy peasy
			 *
			 * 基结构已经包含全部数据，处理很直接。
			 */
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				Oid		   *relids;

				relids = ReorderBufferAllocRelids(rb, change->data.truncate.nrelids);
				memcpy(relids, data, change->data.truncate.nrelids * sizeof(Oid));
				change->data.truncate.relids = relids;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			break;
	}

	dlist_push_tail(&txn->changes, &change->node);
	txn->nentries_mem++;

	/*
	 * Update memory accounting for the restored change.  We need to do this
	 * although we don't check the memory limit when restoring the changes in
	 * this branch (we only do that when initially queueing the changes after
	 * decoding), because we will release the changes later, and that will
	 * update the accounting too (subtracting the size from the counters). And
	 * we don't want to underflow there.
	 *
	 * 为恢复出来的变更更新内存记账。在这条路径上恢复变更时并不检查内存上限（只在解码后初次入队时检查），
	 * 但仍要记账，因为稍后释放这些变更时也会更新记账（从计数中减去大小）。不希望那时发生下溢。
	 */
	ReorderBufferChangeMemoryUpdate(rb, change, NULL, true,
									ReorderBufferChangeSize(change));
}

/*
 * Remove all on-disk stored for the passed in transaction.
 *
 * 删除传入事务在磁盘上保存的全部内容。
 */
static void
ReorderBufferRestoreCleanup(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	XLogSegNo	first;
	XLogSegNo	cur;
	XLogSegNo	last;

	Assert(txn->first_lsn != InvalidXLogRecPtr);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	XLByteToSeg(txn->first_lsn, first, wal_segment_size);
	XLByteToSeg(txn->final_lsn, last, wal_segment_size);

	/* iterate over all possible filenames, and delete them
	 *
	 * 遍历所有可能的文件名并删除它们。
	 */
	for (cur = first; cur <= last; cur++)
	{
		char		path[MAXPGPATH];

		ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid, cur);
		if (unlink(path) != 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
	}
}

/*
 * Remove any leftover serialized reorder buffers from a slot directory after a
 * prior crash or decoding session exit.
 *
 * 在先前崩溃或解码会话退出之后，从复制槽目录中删除残留的序列化 reorder buffer。
 */
static void
ReorderBufferCleanupSerializedTXNs(const char *slotname)
{
	DIR		   *spill_dir;
	struct dirent *spill_de;
	struct stat statbuf;
	char		path[MAXPGPATH * 2 + sizeof(PG_REPLSLOT_DIR)];

	sprintf(path, "%s/%s", PG_REPLSLOT_DIR, slotname);

	/* we're only handling directories here, skip if it's not ours
	 *
	 * 这里只处理目录；若不是我们的目录则跳过。
	 */
	if (lstat(path, &statbuf) == 0 && !S_ISDIR(statbuf.st_mode))
		return;

	spill_dir = AllocateDir(path);
	while ((spill_de = ReadDirExtended(spill_dir, path, INFO)) != NULL)
	{
		/* only look at names that can be ours
		 *
		 * 只查看可能属于我们的名字。
		 */
		if (strncmp(spill_de->d_name, "xid", 3) == 0)
		{
			snprintf(path, sizeof(path),
					 "%s/%s/%s", PG_REPLSLOT_DIR, slotname,
					 spill_de->d_name);

			if (unlink(path) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\" during removal of %s/%s/xid*: %m",
								path, PG_REPLSLOT_DIR, slotname)));
		}
	}
	FreeDir(spill_dir);
}

/*
 * Given a replication slot, transaction ID and segment number, fill in the
 * corresponding spill file into 'path', which is a caller-owned buffer of size
 * at least MAXPGPATH.
 *
 * 给定复制槽、事务号和段号，把对应的溢出文件路径填入 path。path 由调用方提供，大小至少为 MAXPGPATH。
 */
static void
ReorderBufferSerializedPath(char *path, ReplicationSlot *slot, TransactionId xid,
							XLogSegNo segno)
{
	XLogRecPtr	recptr;

	XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, recptr);

	snprintf(path, MAXPGPATH, "%s/%s/xid-%u-lsn-%X-%X.spill",
			 PG_REPLSLOT_DIR,
			 NameStr(MyReplicationSlot->data.name),
			 xid, LSN_FORMAT_ARGS(recptr));
}

/*
 * Delete all data spilled to disk after we've restarted/crashed. It will be
 * recreated when the respective slots are reused.
 *
 * 在重启或崩溃之后删除所有溢出到磁盘的数据。相应复制槽被再次使用时会重新创建。
 */
void
StartupReorderBuffer(void)
{
	DIR		   *logical_dir;
	struct dirent *logical_de;

	logical_dir = AllocateDir(PG_REPLSLOT_DIR);
	while ((logical_de = ReadDir(logical_dir, PG_REPLSLOT_DIR)) != NULL)
	{
		if (strcmp(logical_de->d_name, ".") == 0 ||
			strcmp(logical_de->d_name, "..") == 0)
			continue;

		/* if it cannot be a slot, skip the directory
		 *
		 * 若它不可能是复制槽，则跳过该目录。
		 */
		if (!ReplicationSlotValidateName(logical_de->d_name, DEBUG2))
			continue;

		/*
		 * ok, has to be a surviving logical slot, iterate and delete
		 * everything starting with xid-*
		 *
		 * 这必然是一个幸存的逻辑复制槽，遍历并删除所有以 xid- 开头的文件。
		 */
		ReorderBufferCleanupSerializedTXNs(logical_de->d_name);
	}
	FreeDir(logical_dir);
}

/* ---------------------------------------
 * toast reassembly support
 *
 * toast 重组支持。
 * ---------------------------------------
 */

/*
 * Initialize per tuple toast reconstruction support.
 *
 * 初始化按元组重建 toast 的支持结构。
 */
static void
ReorderBufferToastInitHash(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	HASHCTL		hash_ctl;

	Assert(txn->toast_hash == NULL);

	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(ReorderBufferToastEnt);
	hash_ctl.hcxt = rb->context;
	txn->toast_hash = hash_create("ReorderBufferToastHash", 5, &hash_ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Per toast-chunk handling for toast reconstruction
 *
 * toast 重组时对每个 toast 块的处理。
 *
 * Appends a toast chunk so we can reconstruct it when the tuple "owning" the
 * toasted Datum comes along.
 *
 * 追加一个 toast 块，以便拥有该 toast Datum 的元组到来时能够重建它。
 */
static void
ReorderBufferToastAppendChunk(ReorderBuffer *rb, ReorderBufferTXN *txn,
							  Relation relation, ReorderBufferChange *change)
{
	ReorderBufferToastEnt *ent;
	HeapTuple	newtup;
	bool		found;
	int32		chunksize;
	bool		isnull;
	Pointer		chunk;
	TupleDesc	desc = RelationGetDescr(relation);
	Oid			chunk_id;
	int32		chunk_seq;

	if (txn->toast_hash == NULL)
		ReorderBufferToastInitHash(rb, txn);

	Assert(IsToastRelation(relation));

	newtup = change->data.tp.newtuple;
	chunk_id = DatumGetObjectId(fastgetattr(newtup, 1, desc, &isnull));
	Assert(!isnull);
	chunk_seq = DatumGetInt32(fastgetattr(newtup, 2, desc, &isnull));
	Assert(!isnull);

	ent = (ReorderBufferToastEnt *)
		hash_search(txn->toast_hash, &chunk_id, HASH_ENTER, &found);

	if (!found)
	{
		Assert(ent->chunk_id == chunk_id);
		ent->num_chunks = 0;
		ent->last_chunk_seq = 0;
		ent->size = 0;
		ent->reconstructed = NULL;
		dlist_init(&ent->chunks);

		if (chunk_seq != 0)
			elog(ERROR, "got sequence entry %d for toast chunk %u instead of seq 0",
				 chunk_seq, chunk_id);
	}
	else if (found && chunk_seq != ent->last_chunk_seq + 1)
		elog(ERROR, "got sequence entry %d for toast chunk %u instead of seq %d",
			 chunk_seq, chunk_id, ent->last_chunk_seq + 1);

	chunk = DatumGetPointer(fastgetattr(newtup, 3, desc, &isnull));
	Assert(!isnull);

	/* calculate size so we can allocate the right size at once later
	 *
	 * 计算大小，以便稍后一次分配出正确的长度。
	 */
	if (!VARATT_IS_EXTENDED(chunk))
		chunksize = VARSIZE(chunk) - VARHDRSZ;
	else if (VARATT_IS_SHORT(chunk))
		/* could happen due to heap_form_tuple doing its thing
		 *
		 * heap_form_tuple 做它该做的事时可能出现这种情况。
		 */
		chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
	else
		elog(ERROR, "unexpected type of toast chunk");

	ent->size += chunksize;
	ent->last_chunk_seq = chunk_seq;
	ent->num_chunks++;
	dlist_push_tail(&ent->chunks, &change->node);
}

/*
 * Rejigger change->newtuple to point to in-memory toast tuples instead of
 * on-disk toast tuples that may no longer exist (think DROP TABLE or VACUUM).
 *
 * 改写 change 的 newtuple，使它指向内存中的 toast 元组，而不是磁盘上可能已经不存在的 toast 元组（例如 DROP
 * TABLE 或 VACUUM 之后）。
 *
 * We cannot replace unchanged toast tuples though, so those will still point
 * to on-disk toast data.
 *
 * 但不能替换未改变的 toast 元组，那些仍会指向磁盘上的 toast 数据。
 *
 * While updating the existing change with detoasted tuple data, we need to
 * update the memory accounting info, because the change size will differ.
 * Otherwise the accounting may get out of sync, triggering serialization
 * at unexpected times.
 *
 * 用已 detoast 的元组数据更新现有变更时，必须更新内存记账，因为变更大小会变。否则记账会失去同步，在意料之外
 * 的时刻触发序列化。
 *
 * We simply subtract size of the change before rejiggering the tuple, and
 * then add the new size. This makes it look like the change was removed
 * and then added back, except it only tweaks the accounting info.
 *
 * 做法很简单：在改写元组之前减去变更的大小，然后再加上新的大小。这看起来像是先移除变更再加回来，但实际只调
 * 整记账信息。
 *
 * In particular it can't trigger serialization, which would be pointless
 * anyway as it happens during commit processing right before handing
 * the change to the output plugin.
 *
 * 特别是它不会触发序列化。那也没有意义，因为这发生在提交处理中，即将把变更交给输出插件之前。
 */
static void
ReorderBufferToastReplace(ReorderBuffer *rb, ReorderBufferTXN *txn,
						  Relation relation, ReorderBufferChange *change)
{
	TupleDesc	desc;
	int			natt;
	Datum	   *attrs;
	bool	   *isnull;
	bool	   *free;
	HeapTuple	tmphtup;
	Relation	toast_rel;
	TupleDesc	toast_desc;
	MemoryContext oldcontext;
	HeapTuple	newtup;
	Size		old_size;

	/* no toast tuples changed
	 *
	 * 没有 toast 元组发生变化。
	 */
	if (txn->toast_hash == NULL)
		return;

	/*
	 * We're going to modify the size of the change. So, to make sure the
	 * accounting is correct we record the current change size and then after
	 * re-computing the change we'll subtract the recorded size and then
	 * re-add the new change size at the end. We don't immediately subtract
	 * the old size because if there is any error before we add the new size,
	 * we will release the changes and that will update the accounting info
	 * (subtracting the size from the counters). And we don't want to
	 * underflow there.
	 *
	 * 即将修改变更的大小。为了让记账正确，先记下当前大小；重新计算变更后，再减去记下的大小，并在末尾重
	 * 新加上新的大小。不要立刻减去旧大小：若在加上新大小之前出错，释放变更时也会更新记账（从计数中减去
	 * 大小），那时不希望发生下溢。
	 */
	old_size = ReorderBufferChangeSize(change);

	oldcontext = MemoryContextSwitchTo(rb->context);

	/* we should only have toast tuples in an INSERT or UPDATE
	 *
	 * 只应在 INSERT 或 UPDATE 中出现 toast 元组。
	 */
	Assert(change->data.tp.newtuple);

	desc = RelationGetDescr(relation);

	toast_rel = RelationIdGetRelation(relation->rd_rel->reltoastrelid);
	if (!RelationIsValid(toast_rel))
		elog(ERROR, "could not open toast relation with OID %u (base relation \"%s\")",
			 relation->rd_rel->reltoastrelid, RelationGetRelationName(relation));

	toast_desc = RelationGetDescr(toast_rel);

	/* should we allocate from stack instead?
	 *
	 * 是否改为从栈上分配？
	 */
	attrs = palloc0(sizeof(Datum) * desc->natts);
	isnull = palloc0(sizeof(bool) * desc->natts);
	free = palloc0(sizeof(bool) * desc->natts);

	newtup = change->data.tp.newtuple;

	heap_deform_tuple(newtup, desc, attrs, isnull);

	for (natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, natt);
		ReorderBufferToastEnt *ent;
		struct varlena *varlena;

		/* va_rawsize is the size of the original datum -- including header
		 *
		 * va_rawsize 是原始 datum 的大小，含头部。
		 */
		struct varatt_external toast_pointer;
		struct varatt_indirect redirect_pointer;
		struct varlena *new_datum = NULL;
		struct varlena *reconstructed;
		dlist_iter	it;
		Size		data_done = 0;

		/* system columns aren't toasted
		 *
		 * 系统列不会被 toast。
		 */
		if (attr->attnum < 0)
			continue;

		if (attr->attisdropped)
			continue;

		/* not a varlena datatype
		 *
		 * 不是 varlena 数据类型。
		 */
		if (attr->attlen != -1)
			continue;

		/* no data
		 *
		 * 没有数据。
		 */
		if (isnull[natt])
			continue;

		/* ok, we know we have a toast datum
		 *
		 * 可以确定这是一个 toast datum。
		 */
		varlena = (struct varlena *) DatumGetPointer(attrs[natt]);

		/* no need to do anything if the tuple isn't external
		 *
		 * 若元组不是外部存储的，则无需处理。
		 */
		if (!VARATT_IS_EXTERNAL(varlena))
			continue;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, varlena);

		/*
		 * Check whether the toast tuple changed, replace if so.
		 *
		 * 检查 toast 元组是否改变，若改变则替换。
		 */
		ent = (ReorderBufferToastEnt *)
			hash_search(txn->toast_hash,
						&toast_pointer.va_valueid,
						HASH_FIND,
						NULL);
		if (ent == NULL)
			continue;

		new_datum =
			(struct varlena *) palloc0(INDIRECT_POINTER_SIZE);

		free[natt] = true;

		reconstructed = palloc0(toast_pointer.va_rawsize);

		ent->reconstructed = reconstructed;

		/* stitch toast tuple back together from its parts
		 *
		 * 把各部分重新缝合成 toast 元组。
		 */
		dlist_foreach(it, &ent->chunks)
		{
			bool		cisnull;
			ReorderBufferChange *cchange;
			HeapTuple	ctup;
			Pointer		chunk;

			cchange = dlist_container(ReorderBufferChange, node, it.cur);
			ctup = cchange->data.tp.newtuple;
			chunk = DatumGetPointer(fastgetattr(ctup, 3, toast_desc, &cisnull));

			Assert(!cisnull);
			Assert(!VARATT_IS_EXTERNAL(chunk));
			Assert(!VARATT_IS_SHORT(chunk));

			memcpy(VARDATA(reconstructed) + data_done,
				   VARDATA(chunk),
				   VARSIZE(chunk) - VARHDRSZ);
			data_done += VARSIZE(chunk) - VARHDRSZ;
		}
		Assert(data_done == VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer));

		/* make sure its marked as compressed or not
		 *
		 * 确保它是否被标记为已压缩是正确的。
		 */
		if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			SET_VARSIZE_COMPRESSED(reconstructed, data_done + VARHDRSZ);
		else
			SET_VARSIZE(reconstructed, data_done + VARHDRSZ);

		memset(&redirect_pointer, 0, sizeof(redirect_pointer));
		redirect_pointer.pointer = reconstructed;

		SET_VARTAG_EXTERNAL(new_datum, VARTAG_INDIRECT);
		memcpy(VARDATA_EXTERNAL(new_datum), &redirect_pointer,
			   sizeof(redirect_pointer));

		attrs[natt] = PointerGetDatum(new_datum);
	}

	/*
	 * Build tuple in separate memory & copy tuple back into the tuplebuf
	 * passed to the output plugin. We can't directly heap_fill_tuple() into
	 * the tuplebuf because attrs[] will point back into the current content.
	 *
	 * 在单独的内存中构造元组，再复制回交给输出插件的元组缓冲区。不能直接对元组缓冲区调用
	 * heap_fill_tuple()，因为 attrs 数组会指回当前内容。
	 */
	tmphtup = heap_form_tuple(desc, attrs, isnull);
	Assert(newtup->t_len <= MaxHeapTupleSize);
	Assert(newtup->t_data == (HeapTupleHeader) ((char *) newtup + HEAPTUPLESIZE));

	memcpy(newtup->t_data, tmphtup->t_data, tmphtup->t_len);
	newtup->t_len = tmphtup->t_len;

	/*
	 * free resources we won't further need, more persistent stuff will be
	 * free'd in ReorderBufferToastReset().
	 *
	 * 释放后面不再需要的资源；更持久的部分会在 ReorderBufferToastReset() 中释放。
	 */
	RelationClose(toast_rel);
	pfree(tmphtup);
	for (natt = 0; natt < desc->natts; natt++)
	{
		if (free[natt])
			pfree(DatumGetPointer(attrs[natt]));
	}
	pfree(attrs);
	pfree(free);
	pfree(isnull);

	MemoryContextSwitchTo(oldcontext);

	/* subtract the old change size
	 *
	 * 减去旧的变更大小。
	 */
	ReorderBufferChangeMemoryUpdate(rb, change, NULL, false, old_size);
	/* now add the change back, with the correct size
	 *
	 * 再把变更加回去，并使用正确的大小。
	 */
	ReorderBufferChangeMemoryUpdate(rb, change, NULL, true,
									ReorderBufferChangeSize(change));
}

/*
 * Free all resources allocated for toast reconstruction.
 *
 * 释放为 toast 重建分配的全部资源。
 */
static void
ReorderBufferToastReset(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	HASH_SEQ_STATUS hstat;
	ReorderBufferToastEnt *ent;

	if (txn->toast_hash == NULL)
		return;

	/* sequentially walk over the hash and free everything
	 *
	 * 顺序遍历哈希表并释放所有内容。
	 */
	hash_seq_init(&hstat, txn->toast_hash);
	while ((ent = (ReorderBufferToastEnt *) hash_seq_search(&hstat)) != NULL)
	{
		dlist_mutable_iter it;

		if (ent->reconstructed != NULL)
			pfree(ent->reconstructed);

		dlist_foreach_modify(it, &ent->chunks)
		{
			ReorderBufferChange *change =
				dlist_container(ReorderBufferChange, node, it.cur);

			dlist_delete(&change->node);
			ReorderBufferFreeChange(rb, change, true);
		}
	}

	hash_destroy(txn->toast_hash);
	txn->toast_hash = NULL;
}


/* ---------------------------------------
 * Visibility support for logical decoding
 *
 * 逻辑解码的可见性支持。
 *
 *
 * Lookup actual cmin/cmax values when using decoding snapshot. We can't
 * always rely on stored cmin/cmax values because of two scenarios:
 *
 * 使用解码快照时查找真正的 cmin 和 cmax。不能总是依赖存储的 cmin/cmax，因为有两种情况：
 *
 * * A tuple got changed multiple times during a single transaction and thus
 *	 has got a combo CID. Combo CIDs are only valid for the duration of a
 *	 single transaction.
 * * A tuple with a cmin but no cmax (and thus no combo CID) got
 *	 deleted/updated in another transaction than the one which created it
 *	 which we are looking at right now. As only one of cmin, cmax or combo CID
 *	 is actually stored in the heap we don't have access to the value we
 *	 need anymore.
 *
 *	 元组在单个事务中被修改多次，于是得到 combo CID。combo CID 只在该事务期间有效。另一种情况是：元组有
 *	 cmin 但没有 cmax（因此也没有 combo CID），却在创建它的事务之外的另一个事务中被删除或更新，而我们当
 *	 前正在看的正是创建它的那个事务。堆中实际只存储 cmin、cmax 或 combo CID 之一，因此我们不再能拿到需要
 *	 的那个值。
 *
 * To resolve those problems we have a per-transaction hash of (cmin,
 * cmax) tuples keyed by (relfilelocator, ctid) which contains the actual
 * (cmin, cmax) values. That also takes care of combo CIDs by simply
 * not caring about them at all. As we have the real cmin/cmax values
 * combo CIDs aren't interesting.
 *
 * 为解决这些问题，每个事务有一个以 (relfilelocator, ctid) 为键的 (cmin, cmax) 哈希，其中存放真正的 cmin 和
 * cmax。这也顺便处理了 combo CID：干脆完全不管它们。既然已有真正的 cmin/cmax，combo CID 就不重要了。
 *
 * As we only care about catalog tuples here the overhead of this
 * hashtable should be acceptable.
 *
 * 这里只关心目录元组，这个哈希表的开销应当可以接受。
 *
 * Heap rewrites complicate this a bit, check rewriteheap.c for
 * details.
 *
 * 堆重写会使这件事更复杂一些，详见 rewriteheap.c。
 * -------------------------------------------------------------------------
 */

/* struct for sorting mapping files by LSN efficiently
 *
 * 用于按 LSN 高效排序映射文件的结构。
 */
typedef struct RewriteMappingFile
{
	XLogRecPtr	lsn;
	char		fname[MAXPGPATH];
} RewriteMappingFile;

#ifdef NOT_USED
/*
 * 调试用：遍历 tuplecid 哈希，打印 relfilelocator、ctid 与 cmin、cmax。
 */
static void
DisplayMapping(HTAB *tuplecid_data)
{
	HASH_SEQ_STATUS hstat;
	ReorderBufferTupleCidEnt *ent;

	hash_seq_init(&hstat, tuplecid_data);
	while ((ent = (ReorderBufferTupleCidEnt *) hash_seq_search(&hstat)) != NULL)
	{
		elog(DEBUG3, "mapping: node: %u/%u/%u tid: %u/%u cmin: %u, cmax: %u",
			 ent->key.rlocator.dbOid,
			 ent->key.rlocator.spcOid,
			 ent->key.rlocator.relNumber,
			 ItemPointerGetBlockNumber(&ent->key.tid),
			 ItemPointerGetOffsetNumber(&ent->key.tid),
			 ent->cmin,
			 ent->cmax
			);
	}
}
#endif

/*
 * Apply a single mapping file to tuplecid_data.
 *
 * 把单个映射文件应用到 tuplecid_data。
 *
 * The mapping file has to have been verified to be a) committed b) for our
 * transaction c) applied in LSN order.
 *
 * 该映射文件必须已经核实：已经提交、属于我们的事务，并且按 LSN 顺序应用。
 */
static void
ApplyLogicalMappingFile(HTAB *tuplecid_data, Oid relid, const char *fname)
{
	char		path[MAXPGPATH];
	int			fd;
	int			readBytes;
	LogicalRewriteMappingData map;

	sprintf(path, "%s/%s", PG_LOGICAL_MAPPINGS_DIR, fname);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	while (true)
	{
		ReorderBufferTupleCidKey key;
		ReorderBufferTupleCidEnt *ent;
		ReorderBufferTupleCidEnt *new_ent;
		bool		found;

		/* be careful about padding
		 *
		 * 注意填充字节。
		 */
		memset(&key, 0, sizeof(ReorderBufferTupleCidKey));

		/* read all mappings till the end of the file
		 *
		 * 一直读到文件末尾的全部映射。
		 */
		pgstat_report_wait_start(WAIT_EVENT_REORDER_LOGICAL_MAPPING_READ);
		readBytes = read(fd, &map, sizeof(LogicalRewriteMappingData));
		pgstat_report_wait_end();

		if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else if (readBytes == 0)	/* EOF */
						/*
						 *
						 * 文件结束。
						 */
			break;
		else if (readBytes != sizeof(LogicalRewriteMappingData))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": read %d instead of %d bytes",
							path, readBytes,
							(int32) sizeof(LogicalRewriteMappingData))));

		key.rlocator = map.old_locator;
		ItemPointerCopy(&map.old_tid,
						&key.tid);


		ent = (ReorderBufferTupleCidEnt *)
			hash_search(tuplecid_data, &key, HASH_FIND, NULL);

		/* no existing mapping, no need to update
		 *
		 * 没有已有映射，无需更新。
		 */
		if (!ent)
			continue;

		key.rlocator = map.new_locator;
		ItemPointerCopy(&map.new_tid,
						&key.tid);

		new_ent = (ReorderBufferTupleCidEnt *)
			hash_search(tuplecid_data, &key, HASH_ENTER, &found);

		if (found)
		{
			/*
			 * Make sure the existing mapping makes sense. We sometime update
			 * old records that did not yet have a cmax (e.g. pg_class' own
			 * entry while rewriting it) during rewrites, so allow that.
			 *
			 * 确认已有映射是合理的。重写期间有时会更新尚未设置 cmax 的旧记录（例如重写 pg_class
			 * 时它自己的那一项），因此允许这种情况。
			 */
			Assert(ent->cmin == InvalidCommandId || ent->cmin == new_ent->cmin);
			Assert(ent->cmax == InvalidCommandId || ent->cmax == new_ent->cmax);
		}
		else
		{
			/* update mapping
			 *
			 * 更新映射。
			 */
			new_ent->cmin = ent->cmin;
			new_ent->cmax = ent->cmax;
			new_ent->combocid = ent->combocid;
		}
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}


/*
 * Check whether the TransactionId 'xid' is in the pre-sorted array 'xip'.
 *
 * 检查事务号 xid 是否在已预先排序的数组 xip 中。
 */
static bool
TransactionIdInArray(TransactionId xid, TransactionId *xip, Size num)
{
	return bsearch(&xid, xip, num,
				   sizeof(TransactionId), xidComparator) != NULL;
}

/*
 * list_sort() comparator for sorting RewriteMappingFiles in LSN order.
 *
 * list_sort() 用来按 LSN 顺序排序 RewriteMappingFile 的比较函数。
 */
static int
file_sort_by_lsn(const ListCell *a_p, const ListCell *b_p)
{
	RewriteMappingFile *a = (RewriteMappingFile *) lfirst(a_p);
	RewriteMappingFile *b = (RewriteMappingFile *) lfirst(b_p);

	return pg_cmp_u64(a->lsn, b->lsn);
}

/*
 * Apply any existing logical remapping files if there are any targeted at our
 * transaction for relid.
 *
 * 若存在针对本事务、且目标关系为 relid 的逻辑重映射文件，则应用它们。
 */
static void
UpdateLogicalMappings(HTAB *tuplecid_data, Oid relid, Snapshot snapshot)
{
	DIR		   *mapping_dir;
	struct dirent *mapping_de;
	List	   *files = NIL;
	ListCell   *file;
	Oid			dboid = IsSharedRelation(relid) ? InvalidOid : MyDatabaseId;

	mapping_dir = AllocateDir(PG_LOGICAL_MAPPINGS_DIR);
	while ((mapping_de = ReadDir(mapping_dir, PG_LOGICAL_MAPPINGS_DIR)) != NULL)
	{
		Oid			f_dboid;
		Oid			f_relid;
		TransactionId f_mapped_xid;
		TransactionId f_create_xid;
		XLogRecPtr	f_lsn;
		uint32		f_hi,
					f_lo;
		RewriteMappingFile *f;

		if (strcmp(mapping_de->d_name, ".") == 0 ||
			strcmp(mapping_de->d_name, "..") == 0)
			continue;

		/* Ignore files that aren't ours
		 *
		 * 忽略不属于我们的文件。
		 */
		if (strncmp(mapping_de->d_name, "map-", 4) != 0)
			continue;

		if (sscanf(mapping_de->d_name, LOGICAL_REWRITE_FORMAT,
				   &f_dboid, &f_relid, &f_hi, &f_lo,
				   &f_mapped_xid, &f_create_xid) != 6)
			elog(ERROR, "could not parse filename \"%s\"", mapping_de->d_name);

		f_lsn = ((uint64) f_hi) << 32 | f_lo;

		/* mapping for another database
		 *
		 * 属于另一个数据库的映射。
		 */
		if (f_dboid != dboid)
			continue;

		/* mapping for another relation
		 *
		 * 属于另一个关系的映射。
		 */
		if (f_relid != relid)
			continue;

		/* did the creating transaction abort?
		 *
		 * 创建该映射的事务是否已中止？
		 */
		if (!TransactionIdDidCommit(f_create_xid))
			continue;

		/* not for our transaction
		 *
		 * 不属于我们的事务。
		 */
		if (!TransactionIdInArray(f_mapped_xid, snapshot->subxip, snapshot->subxcnt))
			continue;

		/* ok, relevant, queue for apply
		 *
		 * 相关，排队等待应用。
		 */
		f = palloc(sizeof(RewriteMappingFile));
		f->lsn = f_lsn;
		strcpy(f->fname, mapping_de->d_name);
		files = lappend(files, f);
	}
	FreeDir(mapping_dir);

	/* sort files so we apply them in LSN order
	 *
	 * 对文件排序，以便按 LSN 顺序应用。
	 */
	list_sort(files, file_sort_by_lsn);

	foreach(file, files)
	{
		RewriteMappingFile *f = (RewriteMappingFile *) lfirst(file);

		elog(DEBUG1, "applying mapping: \"%s\" in %u", f->fname,
			 snapshot->subxip[0]);
		ApplyLogicalMappingFile(tuplecid_data, relid, f->fname);
		pfree(f);
	}
}

/*
 * Lookup cmin/cmax of a tuple, during logical decoding where we can't rely on
 * combo CIDs.
 *
 * 在逻辑解码期间查找元组的 cmin/cmax，此时不能依赖 combo CID。
 */
bool
ResolveCminCmaxDuringDecoding(HTAB *tuplecid_data,
							  Snapshot snapshot,
							  HeapTuple htup, Buffer buffer,
							  CommandId *cmin, CommandId *cmax)
{
	ReorderBufferTupleCidKey key;
	ReorderBufferTupleCidEnt *ent;
	ForkNumber	forkno;
	BlockNumber blockno;
	bool		updated_mapping = false;

	/*
	 * Return unresolved if tuplecid_data is not valid.  That's because when
	 * streaming in-progress transactions we may run into tuples with the CID
	 * before actually decoding them.  Think e.g. about INSERT followed by
	 * TRUNCATE, where the TRUNCATE may not be decoded yet when applying the
	 * INSERT.  So in such cases, we assume the CID is from the future
	 * command.
	 *
	 * 若 tuplecid_data 无效则返回未解析。流式发送进行中事务时，可能在真正解码到该 CID 之前就遇到带有该
	 * CID 的元组。例如先 INSERT 再 TRUNCATE，应用 INSERT 时 TRUNCATE 可能还没解码。这时假定该 CID 来自
	 * 未来的命令。
	 */
	if (tuplecid_data == NULL)
		return false;

	/* be careful about padding
	 *
	 * 注意填充字节。
	 */
	memset(&key, 0, sizeof(key));

	Assert(!BufferIsLocal(buffer));

	/*
	 * get relfilelocator from the buffer, no convenient way to access it
	 * other than that.
	 *
	 * 从缓冲区取得 relfilelocator，除此之外没有方便的访问方式。
	 */
	BufferGetTag(buffer, &key.rlocator, &forkno, &blockno);

	/* tuples can only be in the main fork
	 *
	 * 元组只能位于主分叉。
	 */
	Assert(forkno == MAIN_FORKNUM);
	Assert(blockno == ItemPointerGetBlockNumber(&htup->t_self));

	ItemPointerCopy(&htup->t_self,
					&key.tid);

restart:
	ent = (ReorderBufferTupleCidEnt *)
		hash_search(tuplecid_data, &key, HASH_FIND, NULL);

	/*
	 * failed to find a mapping, check whether the table was rewritten and
	 * apply mapping if so, but only do that once - there can be no new
	 * mappings while we are in here since we have to hold a lock on the
	 * relation.
	 *
	 * 没有找到映射。检查该表是否被重写过，若是则应用映射，但只做一次。我们在这里必须持有关系上的锁，因
	 * 此不会再出现新的映射。
	 */
	if (ent == NULL && !updated_mapping)
	{
		UpdateLogicalMappings(tuplecid_data, htup->t_tableOid, snapshot);
		/* now check but don't update for a mapping again
		 *
		 * 现在再查一次，但不要再次更新映射。
		 */
		updated_mapping = true;
		goto restart;
	}
	else if (ent == NULL)
		return false;

	if (cmin)
		*cmin = ent->cmin;
	if (cmax)
		*cmax = ent->cmax;
	return true;
}

/*
 * Count invalidation messages of specified transaction.
 *
 * 统计指定事务的失效消息数量。
 *
 * Returns number of messages, and msgs is set to the pointer of the linked
 * list for the messages.
 *
 * 返回消息数量，并把 msgs 设为这些消息链表的指针。
 */
uint32
ReorderBufferGetInvalidations(ReorderBuffer *rb, TransactionId xid,
							  SharedInvalidationMessage **msgs)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	if (txn == NULL)
		return 0;

	*msgs = txn->invalidations;

	return txn->ninvalidations;
}
