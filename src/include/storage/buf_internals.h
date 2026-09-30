/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager and the buffer replacement
 *	  strategy.
 *
 *	  缓冲区管理器和缓冲区替换策略的内部定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/aio_types.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/relcache.h"
#include "utils/resowner.h"

/*
 * Buffer state is a single 32-bit variable where following data is combined.
 *
 * - 18 bits refcount
 * - 4 bits usage count
 * - 10 bits of flags
 *
 * Combining these values allows to perform some operations without locking
 * the buffer header, by modifying them together with a CAS loop.
 *
 * The definition of buffer state components is below.
 *
 * 缓冲区状态是一个 32 位变量，其中组合了以下数据：
 *
 * - 18 位引用计数
 * - 4 位使用计数
 * - 10 位标志
 *
 * 组合这些值使得某些操作无需锁定缓冲区头部即可完成，只需通过 CAS 循环同时
 * 修改它们。
 *
 * 缓冲区状态组件的定义如下。
 */
#define BUF_REFCOUNT_BITS 18
#define BUF_USAGECOUNT_BITS 4
#define BUF_FLAG_BITS 10

StaticAssertDecl(BUF_REFCOUNT_BITS + BUF_USAGECOUNT_BITS + BUF_FLAG_BITS == 32,
				 "parts of buffer state space need to equal 32");

#define BUF_REFCOUNT_ONE 1
#define BUF_REFCOUNT_MASK ((1U << BUF_REFCOUNT_BITS) - 1)
#define BUF_USAGECOUNT_MASK (((1U << BUF_USAGECOUNT_BITS) - 1) << (BUF_REFCOUNT_BITS))
#define BUF_USAGECOUNT_ONE (1U << BUF_REFCOUNT_BITS)
#define BUF_USAGECOUNT_SHIFT BUF_REFCOUNT_BITS
#define BUF_FLAG_MASK (((1U << BUF_FLAG_BITS) - 1) << (BUF_REFCOUNT_BITS + BUF_USAGECOUNT_BITS))

/* Get refcount and usagecount from buffer state */

/* 从缓冲区状态获取引用计数和使用计数。 */
#define BUF_STATE_GET_REFCOUNT(state) ((state) & BUF_REFCOUNT_MASK)
#define BUF_STATE_GET_USAGECOUNT(state) (((state) & BUF_USAGECOUNT_MASK) >> BUF_USAGECOUNT_SHIFT)

/*
 * Flags for buffer descriptors
 *
 * Note: BM_TAG_VALID essentially means that there is a buffer hashtable
 * entry associated with the buffer's tag.
 *
 * 缓冲区描述符的标志。
 *
 * 注意：BM_TAG_VALID 实质上表示存在与缓冲区标签关联的缓冲区哈希表条目。
 */
#define BM_LOCKED				(1U << 22)	/* buffer header is locked */

											/* 缓冲区头部已锁定。 */
#define BM_DIRTY				(1U << 23)	/* data needs writing */

											/* 数据需要写出。 */
#define BM_VALID				(1U << 24)	/* data is valid */

											/* 数据有效。 */
#define BM_TAG_VALID			(1U << 25)	/* tag is assigned */

											/* 标签已分配。 */
#define BM_IO_IN_PROGRESS		(1U << 26)	/* read or write in progress */

											/* 正在读取或写入。 */
#define BM_IO_ERROR				(1U << 27)	/* previous I/O failed */

											/* 先前的 I/O 已失败。 */
#define BM_JUST_DIRTIED			(1U << 28)	/* dirtied since write started */

											/* 自写入开始以来变脏。 */
#define BM_PIN_COUNT_WAITER		(1U << 29)	/* have waiter for sole pin */

											/* 有等待独占引脚的等待者。 */
#define BM_CHECKPOINT_NEEDED	(1U << 30)	/* must write for checkpoint */

											/* 必须为检查点写出。 */
#define BM_PERMANENT			(1U << 31)	/* permanent buffer (not unlogged,
											 * or init fork) */

											/* 永久缓冲区（不是无日志关系或初始化 fork）。 */
/*
 * The maximum allowed value of usage_count represents a tradeoff between
 * accuracy and speed of the clock-sweep buffer management algorithm.  A
 * large value (comparable to NBuffers) would approximate LRU semantics.
 * But it can take as many as BM_MAX_USAGE_COUNT+1 complete cycles of
 * clock sweeps to find a free buffer, so in practice we don't want the
 * value to be very large.
 *
 * usage_count 的最大允许值代表时钟扫描缓冲区管理算法准确度与速度之间的权衡。
 * 较大的值（可与 NBuffers 相比）将近似 LRU 语义。但找到一个空闲缓冲区可能需要
 * 多达 BM_MAX_USAGE_COUNT+1 个完整的时钟扫描周期，因此实践中我们不希望该值过大。
 */
#define BM_MAX_USAGE_COUNT	5

StaticAssertDecl(BM_MAX_USAGE_COUNT < (1 << BUF_USAGECOUNT_BITS),
				 "BM_MAX_USAGE_COUNT doesn't fit in BUF_USAGECOUNT_BITS bits");
StaticAssertDecl(MAX_BACKENDS_BITS <= BUF_REFCOUNT_BITS,
				 "MAX_BACKENDS_BITS needs to be <= BUF_REFCOUNT_BITS");

/*
 * Buffer tag identifies which disk block the buffer contains.
 *
 * Note: the BufferTag data must be sufficient to determine where to write the
 * block, without reference to pg_class or pg_tablespace entries.  It's
 * possible that the backend flushing the buffer doesn't even believe the
 * relation is visible yet (its xact may have started before the xact that
 * created the rel).  The storage manager must be able to cope anyway.
 *
 * Note: if there's any pad bytes in the struct, InitBufferTag will have
 * to be fixed to zero them, since this struct is used as a hash key.
 *
 * 缓冲区标签标识缓冲区包含的磁盘块。
 *
 * 注意：BufferTag 数据必须足以确定写出该块的位置，而无需引用 pg_class 或
 * pg_tablespace 条目。刷出缓冲区的后端甚至可能尚不认为关系可见（其事务可能早于
 * 创建该关系的事务开始），但存储管理器仍必须能够处理这种情况。
 *
 * 注意：如果结构体中存在填充字节，必须修正 InitBufferTag 以将其置零，因为此结构体
 * 用作哈希键。
 */
typedef struct buftag
{
	Oid			spcOid;			/* tablespace oid */

								/* 表空间 OID。 */
	Oid			dbOid;			/* database oid */

								/* 数据库 OID。 */
	RelFileNumber relNumber;	/* relation file number */

								/* 关系文件编号。 */
	ForkNumber	forkNum;		/* fork number */

								/* fork 编号。 */
	BlockNumber blockNum;		/* blknum relative to begin of reln */

								/* 相对于关系起始位置的块号。 */
} BufferTag;

/*
 * Return the relation file number from a buffer tag.
 * The function extracts the tag component used to identify the relation file.
 *
 * 从缓冲区标签返回关系文件编号。
 * 该函数提取用于标识关系文件的标签组成部分。
 */
static inline RelFileNumber
BufTagGetRelNumber(const BufferTag *tag)
{
	return tag->relNumber;
}

/*
 * Return the fork number from a buffer tag.
 * The function extracts the tag component identifying the relation fork.
 *
 * 从缓冲区标签返回 fork 编号。
 * 该函数提取标识关系 fork 的标签组成部分。
 */
static inline ForkNumber
BufTagGetForkNum(const BufferTag *tag)
{
	return tag->forkNum;
}

/*
 * Set relation-file and fork details in a buffer tag.
 * The function updates the two tag fields together for tag construction.
 *
 * 在缓冲区标签中设置关系文件和 fork 详情。
 * 该函数在构造标签时一起更新这两个标签字段。
 */
static inline void
BufTagSetRelForkDetails(BufferTag *tag, RelFileNumber relnumber,
						ForkNumber forknum)
{
	tag->relNumber = relnumber;
	tag->forkNum = forknum;
}

/*
 * Construct a relation file locator from a buffer tag.
 * The function copies the tablespace, database, and relation-file fields.
 *
 * 从缓冲区标签构造关系文件定位符。
 * 该函数复制表空间、数据库和关系文件字段。
 */
static inline RelFileLocator
BufTagGetRelFileLocator(const BufferTag *tag)
{
	RelFileLocator rlocator;

	rlocator.spcOid = tag->spcOid;
	rlocator.dbOid = tag->dbOid;
	rlocator.relNumber = BufTagGetRelNumber(tag);

	return rlocator;
}

/*
 * Clear a buffer tag to invalid identifiers.
 * The function resets every location component so the tag cannot match a page.
 *
 * 将缓冲区标签清除为无效标识符。
 * 该函数重置每个位置组成部分，使标签无法匹配页面。
 */
static inline void
ClearBufferTag(BufferTag *tag)
{
	tag->spcOid = InvalidOid;
	tag->dbOid = InvalidOid;
	BufTagSetRelForkDetails(tag, InvalidRelFileNumber, InvalidForkNumber);
	tag->blockNum = InvalidBlockNumber;
}

/*
 * Initialize a buffer tag for a relation fork and block.
 * The function copies locator details and sets the requested fork and block.
 *
 * 为关系 fork 和块初始化缓冲区标签。
 * 该函数复制定位符详情并设置请求的 fork 和块。
 */
static inline void
InitBufferTag(BufferTag *tag, const RelFileLocator *rlocator,
			  ForkNumber forkNum, BlockNumber blockNum)
{
	tag->spcOid = rlocator->spcOid;
	tag->dbOid = rlocator->dbOid;
	BufTagSetRelForkDetails(tag, rlocator->relNumber, forkNum);
	tag->blockNum = blockNum;
}

/*
 * Compare two buffer tags for exact equality.
 * The function compares all relation, fork, and block identity fields.
 *
 * 比较两个缓冲区标签是否完全相等。
 * 该函数比较所有关系、fork 和块标识字段。
 */
static inline bool
BufferTagsEqual(const BufferTag *tag1, const BufferTag *tag2)
{
	return (tag1->spcOid == tag2->spcOid) &&
		(tag1->dbOid == tag2->dbOid) &&
		(tag1->relNumber == tag2->relNumber) &&
		(tag1->blockNum == tag2->blockNum) &&
		(tag1->forkNum == tag2->forkNum);
}

/*
 * Test whether a buffer tag belongs to a relation file locator.
 * The function compares the shared relation identity fields, ignoring fork and block.
 *
 * 测试缓冲区标签是否属于某个关系文件定位符。
 * 该函数比较共享的关系标识字段，忽略 fork 和块。
 */
static inline bool
BufTagMatchesRelFileLocator(const BufferTag *tag,
							const RelFileLocator *rlocator)
{
	return (tag->spcOid == rlocator->spcOid) &&
		(tag->dbOid == rlocator->dbOid) &&
		(BufTagGetRelNumber(tag) == rlocator->relNumber);
}


/*
 * The shared buffer mapping table is partitioned to reduce contention.
 * To determine which partition lock a given tag requires, compute the tag's
 * hash code with BufTableHashCode(), then apply BufMappingPartitionLock().
 * NB: NUM_BUFFER_PARTITIONS must be a power of 2!
 *
 * 共享缓冲区映射表被分区以降低竞争。要确定给定标签所需的分区锁，先使用
 * BufTableHashCode() 计算标签的哈希码，再应用 BufMappingPartitionLock()。
 * 注意：NUM_BUFFER_PARTITIONS 必须是 2 的幂！
 */
/*
 * Select the mapping-table partition for a hash code.
 * The function reduces the hash code to a configured buffer partition index.
 *
 * 为哈希码选择映射表分区。
 * 该函数将哈希码缩减为配置的缓冲区分区索引。
 */
static inline uint32
BufTableHashPartition(uint32 hashcode)
{
	return hashcode % NUM_BUFFER_PARTITIONS;
}

/*
 * Return the mapping partition lock for a hash code.
 * The function derives the partition and returns its main LWLock.
 *
 * 返回哈希码对应的映射分区锁。
 * 该函数派生分区并返回其主 LWLock。
 */
static inline LWLock *
BufMappingPartitionLock(uint32 hashcode)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET +
							BufTableHashPartition(hashcode)].lock;
}

/*
 * Return a mapping partition lock by its index.
 * The function indexes the mapping-lock range directly.
 *
 * 按索引返回映射分区锁。
 * 该函数直接索引映射锁范围。
 */
static inline LWLock *
BufMappingPartitionLockByIndex(uint32 index)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET + index].lock;
}

/*
 *	BufferDesc -- shared descriptor/state data for a single shared buffer.
 *
 * Note: Buffer header lock (BM_LOCKED flag) must be held to examine or change
 * tag, state or wait_backend_pgprocno fields.  In general, buffer header lock
 * is a spinlock which is combined with flags, refcount and usagecount into
 * single atomic variable.  This layout allow us to do some operations in a
 * single atomic operation, without actually acquiring and releasing spinlock;
 * for instance, increase or decrease refcount.  buf_id field never changes
 * after initialization, so does not need locking.  freeNext is protected by
 * the buffer_strategy_lock not buffer header lock.  The LWLock can take care
 * of itself.  The buffer header lock is *not* used to control access to the
 * data in the buffer!
 *
 * It's assumed that nobody changes the state field while buffer header lock
 * is held.  Thus buffer header lock holder can do complex updates of the
 * state variable in single write, simultaneously with lock release (cleaning
 * BM_LOCKED flag).  On the other hand, updating of state without holding
 * buffer header lock is restricted to CAS, which ensures that BM_LOCKED flag
 * is not set.  Atomic increment/decrement, OR/AND etc. are not allowed.
 *
 * An exception is that if we have the buffer pinned, its tag can't change
 * underneath us, so we can examine the tag without locking the buffer header.
 * Also, in places we do one-time reads of the flags without bothering to
 * lock the buffer header; this is generally for situations where we don't
 * expect the flag bit being tested to be changing.
 *
 * We can't physically remove items from a disk page if another backend has
 * the buffer pinned.  Hence, a backend may need to wait for all other pins
 * to go away.  This is signaled by storing its own pgprocno into
 * wait_backend_pgprocno and setting flag bit BM_PIN_COUNT_WAITER.  At present,
 * there can be only one such waiter per buffer.
 *
 * We use this same struct for local buffer headers, but the locks are not
 * used and not all of the flag bits are useful either. To avoid unnecessary
 * overhead, manipulations of the state field should be done without actual
 * atomic operations (i.e. only pg_atomic_read_u32() and
 * pg_atomic_unlocked_write_u32()).
 *
 * Be careful to avoid increasing the size of the struct when adding or
 * reordering members.  Keeping it below 64 bytes (the most common CPU
 * cache line size) is fairly important for performance.
 *
 * Per-buffer I/O condition variables are currently kept outside this struct in
 * a separate array.  They could be moved in here and still fit within that
 * limit on common systems, but for now that is not done.
 *
 * BufferDesc —— 单个共享缓冲区的共享描述符／状态数据。
 *
 * 注意：检查或变更 tag、state 或 wait_backend_pgprocno 字段时必须持有缓冲区头部锁
 * （BM_LOCKED 标志）。通常，缓冲区头部锁是自旋锁，并与标志、引用计数和使用计数
 * 组合进单个原子变量。该布局使我们可以在单个原子操作中执行一些操作，而不实际获取和
 * 释放自旋锁，例如增加或减少引用计数。buf_id 字段在初始化后从不改变，因此不需要锁。
 * freeNext 受 buffer_strategy_lock 而非缓冲区头部锁保护。LWLock 可自行处理。缓冲区
 * 头部锁不用于控制对缓冲区数据的访问！
 *
 * 假设在持有缓冲区头部锁时无人变更 state 字段。因此，缓冲区头部锁持有者可以在一次
 * 写入中完成 state 变量的复杂更新，同时释放锁（清除 BM_LOCKED 标志）。另一方面，
 * 未持有缓冲区头部锁时，状态更新仅限于 CAS，以确保未设置 BM_LOCKED 标志。不允许
 * 原子递增／递减、OR／AND 等操作。
 *
 * 一个例外是：若我们已固定缓冲区，它的标签不会在我们底下改变，因此可以不锁定缓冲区
 * 头部而检查标签。此外，有些位置会一次性读取标志而不费心锁定缓冲区头部；通常是在不
 * 预期被检查的标志位发生变化的情形。
 *
 * 如果另一个后端固定了缓冲区，我们无法从磁盘页面物理移除项目。因此，一个后端可能需要
 * 等待所有其他固定消失。通过将其自身的 pgprocno 保存到 wait_backend_pgprocno 并设置
 * 标志位 BM_PIN_COUNT_WAITER 来发出此信号。目前每个缓冲区只能有一个此类等待者。
 *
 * 本地缓冲区头部也使用此结构体，但不会使用锁，且并非所有标志位都有用。为避免不必要的
 * 开销，对 state 字段的操作应不使用实际原子操作（即只使用 pg_atomic_read_u32() 和
 * pg_atomic_unlocked_write_u32()）。
 *
 * 添加或重排成员时请小心避免增加结构体大小。将其保持在 64 字节（最常见的 CPU 缓存行
 * 大小）以下，对性能相当重要。
 *
 * 每个缓冲区的 I/O 条件变量目前保存在此结构体外部的独立数组中。它们可以移至此处且在
 * 常见系统中仍符合该限制，但目前未这样做。
 */
typedef struct BufferDesc
{
	BufferTag	tag;			/* ID of page contained in buffer */

								/* 缓冲区包含页面的 ID。 */
	int			buf_id;			/* buffer's index number (from 0) */

								/* 缓冲区的索引编号（从 0 开始）。 */

	/* state of the tag, containing flags, refcount and usagecount */

	/* 标签状态，包含标志、引用计数和使用计数。 */
	pg_atomic_uint32 state;

	int			wait_backend_pgprocno;	/* backend of pin-count waiter */

								/* 固定计数等待者所在的后端。 */
	int			freeNext;		/* link in freelist chain */

								/* 空闲列表链中的链接。 */

	PgAioWaitRef io_wref;		/* set iff AIO is in progress */

								/* 当且仅当 AIO 正在进行时设置。 */
	LWLock		content_lock;	/* to lock access to buffer contents */

								/* 用于锁定对缓冲区内容的访问。 */
} BufferDesc;

/*
 * Concurrent access to buffer headers has proven to be more efficient if
 * they're cache line aligned. So we force the start of the BufferDescriptors
 * array to be on a cache line boundary and force the elements to be cache
 * line sized.
 *
 * XXX: As this is primarily matters in highly concurrent workloads which
 * probably all are 64bit these days, and the space wastage would be a bit
 * more noticeable on 32bit systems, we don't force the stride to be cache
 * line sized on those. If somebody does actual performance testing, we can
 * reevaluate.
 *
 * Note that local buffer descriptors aren't forced to be aligned - as there's
 * no concurrent access to those it's unlikely to be beneficial.
 *
 * We use a 64-byte cache line size here, because that's the most common
 * size. Making it bigger would be a waste of memory. Even if running on a
 * platform with either 32 or 128 byte line sizes, it's good to align to
 * boundaries and avoid false sharing.
 *
 * 如果缓冲区头部按缓存行对齐，并发访问已证明更高效。因此我们强制 BufferDescriptors
 * 数组的起始位置位于缓存行边界，并强制元素大小为缓存行大小。
 *
 * XXX：这主要影响高度并发的工作负载，如今可能都使用 64 位系统；而空间浪费在 32 位
 * 系统上会更明显，因此我们不强制这些系统上的步幅为缓存行大小。若有人进行了实际性能
 * 测试，我们可以重新评估。
 *
 * 注意，本地缓冲区描述符不会被强制对齐——由于不存在并发访问，这不太可能有益。
 *
 * 此处使用 64 字节缓存行大小，因为这是最常见的大小。加大它会浪费内存。即使运行在
 * 具有 32 或 128 字节行大小的平台上，按边界对齐并避免伪共享也是有益的。
 */
#define BUFFERDESC_PAD_TO_SIZE	(SIZEOF_VOID_P == 8 ? 64 : 1)

typedef union BufferDescPadded
{
	BufferDesc	bufferdesc;
	char		pad[BUFFERDESC_PAD_TO_SIZE];
} BufferDescPadded;

/*
 * The PendingWriteback & WritebackContext structure are used to keep
 * information about pending flush requests to be issued to the OS.
 *
 * PendingWriteback 和 WritebackContext 结构体用于保存将要发给操作系统的待处理刷写
 * 请求信息。
 */
typedef struct PendingWriteback
{
	/* could store different types of pending flushes here */

	/* 此处可存储不同类型的待处理刷写。 */
	BufferTag	tag;
} PendingWriteback;

/* struct forward declared in bufmgr.h */

/* 结构体在 bufmgr.h 中前向声明。 */
typedef struct WritebackContext
{
	/* pointer to the max number of writeback requests to coalesce */

	/* 指向可合并写回请求最大数量的指针。 */
	int		   *max_pending;

	/* current number of pending writeback requests */

	/* 当前待处理写回请求的数量。 */
	int			nr_pending;

	/* pending requests */

	/* 待处理请求。 */
	PendingWriteback pending_writebacks[WRITEBACK_MAX_PENDING_FLUSHES];
} WritebackContext;

/* in buf_init.c */

/* 位于 buf_init.c 中。 */
extern PGDLLIMPORT BufferDescPadded *BufferDescriptors;
extern PGDLLIMPORT ConditionVariableMinimallyPadded *BufferIOCVArray;
extern PGDLLIMPORT WritebackContext BackendWritebackContext;

/* in localbuf.c */

/* 位于 localbuf.c 中。 */
extern PGDLLIMPORT BufferDesc *LocalBufferDescriptors;


/*
 * Return a shared-buffer descriptor by buffer ID.
 * The function indexes the padded shared descriptor array.
 *
 * 按缓冲区 ID 返回共享缓冲区描述符。
 * 该函数索引填充后的共享描述符数组。
 */
static inline BufferDesc *
GetBufferDescriptor(uint32 id)
{
	return &(BufferDescriptors[id]).bufferdesc;
}

/*
 * Return a local-buffer descriptor by buffer ID.
 * The function indexes the backend-local descriptor array.
 *
 * 按缓冲区 ID 返回本地缓冲区描述符。
 * 该函数索引后端本地描述符数组。
 */
static inline BufferDesc *
GetLocalBufferDescriptor(uint32 id)
{
	return &LocalBufferDescriptors[id];
}

/*
 * Convert a buffer descriptor to its public Buffer value.
 * The function translates the zero-based descriptor ID to the one-based handle.
 *
 * 将缓冲区描述符转换为公开的 Buffer 值。
 * 该函数把从零开始的描述符 ID 转换为从一开始的句柄。
 */
static inline Buffer
BufferDescriptorGetBuffer(const BufferDesc *bdesc)
{
	return (Buffer) (bdesc->buf_id + 1);
}

/*
 * Return the I/O condition variable associated with a buffer descriptor.
 * The function maps the descriptor ID into the per-buffer condition-variable array.
 *
 * 返回与缓冲区描述符关联的 I/O 条件变量。
 * 该函数将描述符 ID 映射到每缓冲区条件变量数组。
 */
static inline ConditionVariable *
BufferDescriptorGetIOCV(const BufferDesc *bdesc)
{
	return &(BufferIOCVArray[bdesc->buf_id]).cv;
}

/*
 * Return the content lock for a buffer descriptor.
 * The function exposes the descriptor's embedded LWLock as a lock pointer.
 *
 * 返回缓冲区描述符的内容锁。
 * 该函数将描述符内嵌的 LWLock 作为锁指针公开。
 */
static inline LWLock *
BufferDescriptorGetContentLock(const BufferDesc *bdesc)
{
	return (LWLock *) (&bdesc->content_lock);
}

/*
 * The freeNext field is either the index of the next freelist entry,
 * or one of these special values:
 *
 * freeNext 字段要么是下一个空闲列表条目的索引，要么是下列特殊值之一：
 */
#define FREENEXT_END_OF_LIST	(-1)
#define FREENEXT_NOT_IN_LIST	(-2)

/*
 * Functions for acquiring/releasing a shared buffer header's spinlock.  Do
 * not apply these to local buffers!
 *
 * 用于获取／释放共享缓冲区头部自旋锁的函数。请勿将它们应用于本地缓冲区！
 */
/*
 * Acquire a shared buffer header lock and return its state.
 * The function atomically locks the header so callers can inspect or update it.
 *
 * 获取共享缓冲区头部锁并返回其状态。
 * 该函数以原子方式锁定头部，使调用者可以检查或更新它。
 */
extern uint32 LockBufHdr(BufferDesc *desc);

/*
 * Release a shared buffer header lock with an updated state.
 * The function issues a write barrier and clears BM_LOCKED during the state write.
 *
 * 使用更新后的状态释放共享缓冲区头部锁。
 * 该函数发出写屏障，并在写入状态时清除 BM_LOCKED。
 */
static inline void
UnlockBufHdr(BufferDesc *desc, uint32 buf_state)
{
	pg_write_barrier();
	pg_atomic_write_u32(&desc->state, buf_state & (~BM_LOCKED));
}

/* in bufmgr.c */

/* 位于 bufmgr.c 中。 */

/*
 * Structure to sort buffers per file on checkpoints.
 *
 * This structure is allocated per buffer in shared memory, so it should be
 * kept as small as possible.
 *
 * 用于在检查点期间按文件对缓冲区排序的结构体。
 *
 * 该结构体在共享内存中按每个缓冲区分配，因此应尽可能小。
 */
typedef struct CkptSortItem
{
	Oid			tsId;
	RelFileNumber relNumber;
	ForkNumber	forkNum;
	BlockNumber blockNum;
	int			buf_id;
} CkptSortItem;

extern PGDLLIMPORT CkptSortItem *CkptBufferIds;

/* ResourceOwner callbacks to hold buffer I/Os and pins */

/* 用于持有缓冲区 I/O 和引脚的 ResourceOwner 回调。 */
extern PGDLLIMPORT const ResourceOwnerDesc buffer_io_resowner_desc;
extern PGDLLIMPORT const ResourceOwnerDesc buffer_pin_resowner_desc;

/* Convenience wrappers over ResourceOwnerRemember/Forget */

/* ResourceOwnerRemember/Forget 的便捷包装器。 */

/*
 * Record a buffer pin in a resource owner.
 * The function delegates ownership registration to the buffer-pin descriptor.
 *
 * 在资源所有者中记录一个缓冲区引脚。
 * 该函数通过缓冲区引脚描述符委托所有权登记。
 */
static inline void
ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerRemember(owner, Int32GetDatum(buffer), &buffer_pin_resowner_desc);
}
/*
 * Forget a buffer pin from a resource owner.
 * The function removes ownership registration through the buffer-pin descriptor.
 *
 * 从资源所有者中忘记一个缓冲区引脚。
 * 该函数通过缓冲区引脚描述符移除所有权登记。
 */
static inline void
ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerForget(owner, Int32GetDatum(buffer), &buffer_pin_resowner_desc);
}
/*
 * Record a buffer I/O in a resource owner.
 * The function delegates ownership registration to the buffer-I/O descriptor.
 *
 * 在资源所有者中记录一个缓冲区 I/O。
 * 该函数通过缓冲区 I/O 描述符委托所有权登记。
 */
static inline void
ResourceOwnerRememberBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerRemember(owner, Int32GetDatum(buffer), &buffer_io_resowner_desc);
}
/*
 * Forget a buffer I/O from a resource owner.
 * The function removes ownership registration through the buffer-I/O descriptor.
 *
 * 从资源所有者中忘记一个缓冲区 I/O。
 * 该函数通过缓冲区 I/O 描述符移除所有权登记。
 */
static inline void
ResourceOwnerForgetBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerForget(owner, Int32GetDatum(buffer), &buffer_io_resowner_desc);
}

/*
 * Internal buffer management routines
 *
 * 内部缓冲区管理例程。
 */
/* bufmgr.c */

/* bufmgr.c 中的内部接口。 */

/* Initialize a writeback context.
 * The function stores the coalescing limit and resets pending requests.
 *
 * 初始化写回上下文。
 * 该函数保存合并限制并重置待处理请求。
 */
extern void WritebackContextInit(WritebackContext *context, int *max_pending);

/* Issue all scheduled writeback requests.
 * The function submits pending flushes to the operating system for an I/O context.
 *
 * 发出所有已调度的写回请求。
 * 该函数为一个 I/O 上下文向操作系统提交待处理刷写。
 */
extern void IssuePendingWritebacks(WritebackContext *wb_context, IOContext io_context);

/* Schedule a buffer tag for writeback.
 * The function adds the tag to pending requests or submits it according to context limits.
 *
 * 为缓冲区标签调度写回。
 * 该函数将标签添加到待处理请求，或根据上下文限制提交它。
 */
extern void ScheduleBufferTagForWriteback(WritebackContext *wb_context,
										  IOContext io_context, BufferTag *tag);

/* solely to make it easier to write tests */

/* 仅用于简化测试编写。 */

/* Start buffer I/O for testing paths.
 * The function claims the buffer I/O state and reports whether it was started.
 *
 * 为测试路径启动缓冲区 I/O。
 * 该函数占用缓冲区 I/O 状态并报告是否已启动。
 */
extern bool StartBufferIO(BufferDesc *buf, bool forInput, bool nowait);

/* Complete buffer I/O for testing paths.
 * The function clears or sets state bits and releases associated ownership.
 *
 * 为测试路径完成缓冲区 I/O。
 * 该函数清除或设置状态位，并释放关联所有权。
 */
extern void TerminateBufferIO(BufferDesc *buf, bool clear_dirty, uint32 set_flag_bits,
							  bool forget_owner, bool release_aio);


/* freelist.c */

/* freelist.c 中的内部接口。 */

/* Choose the I/O context for an access strategy.
 * The function maps strategy policy to the context used for buffer I/O.
 *
 * 为访问策略选择 I/O 上下文。
 * 该函数将策略映射为缓冲区 I/O 使用的上下文。
 */
extern IOContext IOContextForStrategy(BufferAccessStrategy strategy);

/* Select a buffer using an access strategy.
 * The function searches the strategy or clock sweep and reports selection state.
 *
 * 使用访问策略选择一个缓冲区。
 * 该函数搜索策略或时钟扫描并报告选择状态。
 */
extern BufferDesc *StrategyGetBuffer(BufferAccessStrategy strategy,
									 uint32 *buf_state, bool *from_ring);

/* Return a buffer to the replacement strategy.
 * The function makes the descriptor available to strategy bookkeeping.
 *
 * 将缓冲区归还给替换策略。
 * 该函数使描述符可供策略记账使用。
 */
extern void StrategyFreeBuffer(BufferDesc *buf);

/* Decide whether an access strategy should reject a buffer.
 * The function applies strategy policy to a selected descriptor and ring origin.
 *
 * 决定访问策略是否应拒绝缓冲区。
 * 该函数将策略应用到选定描述符及其环来源。
 */
extern bool StrategyRejectBuffer(BufferAccessStrategy strategy,
								 BufferDesc *buf, bool from_ring);

/* Start synchronized clock-sweep allocation.
 * The function returns sweep progress and allocation counters to the caller.
 *
 * 启动同步时钟扫描分配。
 * 该函数向调用者返回扫描进度和分配计数器。
 */
extern int	StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc);

/* Notify the background writer of strategy activity.
 * The function signals the configured background writer process.
 *
 * 通知后台写入器策略活动。
 * 该函数向配置的后台写入器进程发送信号。
 */
extern void StrategyNotifyBgWriter(int bgwprocno);

/* Return shared-memory space required by the replacement strategy.
 * The function sizes strategy control structures before shared-memory allocation.
 *
 * 返回替换策略所需的共享内存空间。
 * 该函数在分配共享内存前计算策略控制结构大小。
 */
extern Size StrategyShmemSize(void);

/* Initialize replacement-strategy shared state.
 * The function creates or attaches strategy structures during startup.
 *
 * 初始化替换策略共享状态。
 * 该函数在启动期间创建或附接策略结构。
 */
extern void StrategyInitialize(bool init);

/* Test whether a free shared buffer is available.
 * The function consults replacement state without allocating a buffer.
 *
 * 测试是否存在可用的空闲共享缓冲区。
 * 该函数查询替换状态而不分配缓冲区。
 */
extern bool have_free_buffer(void);

/* buf_table.c */

/* buf_table.c 中的内部接口。 */

/* Return shared-memory space required for the buffer lookup table.
 * The function sizes the hash table for the requested number of entries.
 *
 * 返回缓冲区查找表所需的共享内存空间。
 * 该函数为请求的条目数计算哈希表大小。
 */
extern Size BufTableShmemSize(int size);

/* Initialize the buffer lookup table.
 * The function creates hash-table state for the requested capacity.
 *
 * 初始化缓冲区查找表。
 * 该函数为请求的容量创建哈希表状态。
 */
extern void InitBufTable(int size);

/* Compute the lookup hash code for a buffer tag.
 * The function hashes the complete tag for mapping-table operations.
 *
 * 计算缓冲区标签的查找哈希码。
 * 该函数为映射表操作哈希完整标签。
 */
extern uint32 BufTableHashCode(BufferTag *tagPtr);

/* Look up a buffer ID by tag and hash code.
 * The function searches the mapping table under the caller's partition lock.
 *
 * 按标签和哈希码查找缓冲区 ID。
 * 该函数在调用者的分区锁保护下搜索映射表。
 */
extern int	BufTableLookup(BufferTag *tagPtr, uint32 hashcode);

/* Insert a tag-to-buffer mapping.
 * The function adds the mapping to the appropriate hash partition.
 *
 * 插入标签到缓冲区的映射。
 * 该函数将映射添加到适当的哈希分区。
 */
extern int	BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id);

/* Delete a tag-to-buffer mapping.
 * The function removes the mapping from the appropriate hash partition.
 *
 * 删除标签到缓冲区的映射。
 * 该函数从适当的哈希分区移除映射。
 */
extern void BufTableDelete(BufferTag *tagPtr, uint32 hashcode);

/* localbuf.c */

/* localbuf.c 中的内部接口。 */

/* Pin a local buffer descriptor.
 * The function increments local ownership and optionally adjusts its usage count.
 *
 * 固定一个本地缓冲区描述符。
 * 该函数增加本地所有权，并可选择调整其使用计数。
 */
extern bool PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount);

/* Unpin a local buffer and update its resource owner.
 * The function releases one local pin through normal ownership bookkeeping.
 *
 * 取消固定本地缓冲区并更新其资源所有者。
 * 该函数通过常规所有权记账释放一个本地引脚。
 */
extern void UnpinLocalBuffer(Buffer buffer);

/* Unpin a local buffer without resource-owner bookkeeping.
 * The function releases a local pin for paths that already handle ownership.
 *
 * 取消固定本地缓冲区而不进行资源所有者记账。
 * 该函数为已处理所有权的路径释放本地引脚。
 */
extern void UnpinLocalBufferNoOwner(Buffer buffer);

/* Prefetch a local relation block.
 * The function prepares local-buffer I/O for the requested relation fork and block.
 *
 * 预取本地关系块。
 * 该函数为请求的关系 fork 和块准备本地缓冲区 I/O。
 */
extern PrefetchBufferResult PrefetchLocalBuffer(SMgrRelation smgr,
												ForkNumber forkNum,
												BlockNumber blockNum);

/* Allocate or find a local buffer for a relation block.
 * The function returns a descriptor and reports whether an existing page was found.
 *
 * 为关系块分配或查找本地缓冲区。
 * 该函数返回描述符，并报告是否找到了现有页面。
 */
extern BufferDesc *LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum,
									BlockNumber blockNum, bool *foundPtr);

/* Extend a relation through local buffered pages.
 * The function allocates and initializes local buffers up to the requested extent.
 *
 * 通过本地缓冲页面扩展关系。
 * 该函数分配并初始化本地缓冲区，直到请求的扩展范围。
 */
extern BlockNumber ExtendBufferedRelLocal(BufferManagerRelation bmr,
										  ForkNumber fork,
										  uint32 flags,
										  uint32 extend_by,
										  BlockNumber extend_upto,
										  Buffer *buffers,
										  uint32 *extended_by);

/* Mark a local buffer dirty.
 * The function records that the local page must be written before reuse.
 *
 * 将本地缓冲区标记为脏。
 * 该函数记录本地页面必须在重用前写出。
 */
extern void MarkLocalBufferDirty(Buffer buffer);

/* Complete local-buffer I/O.
 * The function updates state flags and optionally releases the associated AIO.
 *
 * 完成本地缓冲区 I/O。
 * 该函数更新状态标志，并可选择释放关联的 AIO。
 */
extern void TerminateLocalBufferIO(BufferDesc *bufHdr, bool clear_dirty,
								   uint32 set_flag_bits, bool release_aio);

/* Start local-buffer I/O.
 * The function claims the I/O state and reports whether the caller should perform I/O.
 *
 * 启动本地缓冲区 I/O。
 * 该函数占用 I/O 状态，并报告调用者是否应执行 I/O。
 */
extern bool StartLocalBufferIO(BufferDesc *bufHdr, bool forInput, bool nowait);

/* Flush a dirty local buffer to storage.
 * The function writes the page through the supplied storage-manager relation.
 *
 * 将脏本地缓冲区刷写到存储。
 * 该函数通过给定的存储管理器关系写出页面。
 */
extern void FlushLocalBuffer(BufferDesc *bufHdr, SMgrRelation reln);

/* Invalidate a local buffer descriptor.
 * The function discards its tag and optionally verifies that it is unreferenced.
 *
 * 使本地缓冲区描述符失效。
 * 该函数丢弃其标签，并可选择验证它未被引用。
 */
extern void InvalidateLocalBuffer(BufferDesc *bufHdr, bool check_unreferenced);

/* Drop local buffers for a relation fork from a block onward.
 * The function invalidates matching cached pages beginning at firstDelBlock.
 *
 * 从指定块起丢弃关系 fork 的本地缓冲区。
 * 该函数从 firstDelBlock 开始使匹配的缓存页面失效。
 */
extern void DropRelationLocalBuffers(RelFileLocator rlocator,
									 ForkNumber forkNum,
									 BlockNumber firstDelBlock);

/* Drop every local buffer for a relation.
 * The function invalidates all locally cached forks belonging to the relation.
 *
 * 丢弃关系的每个本地缓冲区。
 * 该函数使属于该关系的所有本地缓存 fork 失效。
 */
extern void DropRelationAllLocalBuffers(RelFileLocator rlocator);

/* Process local buffers at transaction end.
 * The function commits or discards transaction-scoped local-buffer state.
 *
 * 在事务结束时处理本地缓冲区。
 * 该函数提交或丢弃事务范围内的本地缓冲区状态。
 */
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif							/* BUFMGR_INTERNALS_H */
