/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES buffer manager definitions.
 *
 *	  POSTGRES 缓冲区管理器定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include "port/pg_iovec.h"
#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

typedef void *Block;

/*
 * Possible arguments for GetAccessStrategy().
 *
 * If adding a new BufferAccessStrategyType, also add a new IOContext so
 * IO statistics using this strategy are tracked.
 *
 * GetAccessStrategy() 的可能参数。
 *
 * 如果新增 BufferAccessStrategyType，也要新增一个 IOContext，以跟踪使用该策略的
 * IO 统计信息。
 */
typedef enum BufferAccessStrategyType
{
	BAS_NORMAL,					/* Normal random access */

								/* 普通随机访问。 */
	BAS_BULKREAD,				/* Large read-only scan (hint bit updates are
								 * ok) */

								/* 大型只读扫描（提示位更新可接受）。 */
	BAS_BULKWRITE,				/* Large multi-block write (e.g. COPY IN) */

								/* 大型多块写入（例如 COPY IN）。 */
	BAS_VACUUM,					/* VACUUM */

								/* VACUUM。 */
} BufferAccessStrategyType;

/* Possible modes for ReadBufferExtended() */

/* ReadBufferExtended() 的可能模式。 */
typedef enum
{
	RBM_NORMAL,					/* Normal read */

								/* 普通读取。 */
	RBM_ZERO_AND_LOCK,			/* Don't read from disk, caller will
								 * initialize. Also locks the page. */

								/* 不从磁盘读取，由调用者初始化。也会锁定页面。 */
	RBM_ZERO_AND_CLEANUP_LOCK,	/* Like RBM_ZERO_AND_LOCK, but locks the page
								 * in "cleanup" mode */

								/* 类似 RBM_ZERO_AND_LOCK，但以“清理”模式锁定页面。 */
	RBM_ZERO_ON_ERROR,			/* Read, but return an all-zeros page on error */

								/* 读取，但出错时返回全零页面。 */
	RBM_NORMAL_NO_LOG,			/* Don't log page as invalid during WAL
								 * replay; otherwise same as RBM_NORMAL */

								/* WAL 重放期间不将页面记录为无效；其他方面与 RBM_NORMAL 相同。 */
} ReadBufferMode;

/*
 * Type returned by PrefetchBuffer().
 *
 * PrefetchBuffer() 返回的类型。
 */
typedef struct PrefetchBufferResult
{
	Buffer		recent_buffer;	/* If valid, a hit (recheck needed!) */

								/* 若有效，表示命中（需要重新检查！）。 */
	bool		initiated_io;	/* If true, a miss resulting in async I/O */

								/* 若为 true，表示未命中并导致异步 I/O。 */
} PrefetchBufferResult;

/*
 * Flags influencing the behaviour of ExtendBufferedRel*
 *
 * 影响 ExtendBufferedRel* 行为的标志。
 */
typedef enum ExtendBufferedFlags
{
	/*
	 * Don't acquire extension lock. This is safe only if the relation isn't
	 * shared, an access exclusive lock is held or if this is the startup
	 * process.
	 *
	 * 不获取扩展锁。仅当关系不共享、持有访问排他锁或当前为启动进程时才安全。
	 */
	EB_SKIP_EXTENSION_LOCK = (1 << 0),

	/* Is this extension part of recovery? */

	/* 此扩展是否属于恢复过程？ */
	EB_PERFORMING_RECOVERY = (1 << 1),

	/*
	 * Should the fork be created if it does not currently exist? This likely
	 * only ever makes sense for relation forks.
	 *
	 * 若 fork 当前不存在，是否应创建？这可能仅对关系 fork 有意义。
	 */
	EB_CREATE_FORK_IF_NEEDED = (1 << 2),

	/* Should the first (possibly only) return buffer be returned locked? */

	/* 是否应锁定返回第一个（可能是唯一的）缓冲区？ */
	EB_LOCK_FIRST = (1 << 3),

	/* Should the smgr size cache be cleared? */

	/* 是否应清除 smgr 大小缓存？ */
	EB_CLEAR_SIZE_CACHE = (1 << 4),

	/* internal flags follow */

	/* 下列为内部标志。 */
	EB_LOCK_TARGET = (1 << 5),
}			ExtendBufferedFlags;

/*
 * Some functions identify relations either by relation or smgr +
 * relpersistence.  Used via the BMR_REL()/BMR_SMGR() macros below.  This
 * allows us to use the same function for both recovery and normal operation.
 *
 * 某些函数通过关系或 smgr + relpersistence 标识关系。通过下方 BMR_REL()/BMR_SMGR()
 * 宏使用。这使我们能将同一函数用于恢复和普通操作。
 */
typedef struct BufferManagerRelation
{
	Relation	rel;
	struct SMgrRelationData *smgr;
	char		relpersistence;
} BufferManagerRelation;

#define BMR_REL(p_rel) ((BufferManagerRelation){.rel = p_rel})
#define BMR_SMGR(p_smgr, p_relpersistence) ((BufferManagerRelation){.smgr = p_smgr, .relpersistence = p_relpersistence})

/* Zero out page if reading fails. */

/* 读取失败时将页面置零。 */
#define READ_BUFFERS_ZERO_ON_ERROR (1 << 0)
/* Call smgrprefetch() if I/O necessary. */

/* 如有必要，调用 smgrprefetch()。 */
#define READ_BUFFERS_ISSUE_ADVICE (1 << 1)
/* Don't treat page as invalid due to checksum failures. */

/* 不因校验和失败将页面视为无效。 */
#define READ_BUFFERS_IGNORE_CHECKSUM_FAILURES (1 << 2)
/* IO will immediately be waited for */

/* 将立即等待 I/O。 */
#define READ_BUFFERS_SYNCHRONOUSLY (1 << 3)


struct ReadBuffersOperation
{
	/* The following members should be set by the caller. */

	/* 下列成员应由调用者设置。 */
	Relation	rel;			/* optional */

								/* 可选。 */
	struct SMgrRelationData *smgr;
	char		persistence;
	ForkNumber	forknum;
	BufferAccessStrategy strategy;

	/*
	 * The following private members are private state for communication
	 * between StartReadBuffers() and WaitReadBuffers(), initialized only if
	 * an actual read is required, and should not be modified.
 *
 * 下列私有成员是在 StartReadBuffers() 和 WaitReadBuffers() 之间传递的私有状态，
 * 仅在实际需要读取时初始化，且不应修改。
	 */
	Buffer	   *buffers;
	BlockNumber blocknum;
	int			flags;
	int16		nblocks;
	int16		nblocks_done;
	PgAioWaitRef io_wref;
	PgAioReturn io_return;
};

typedef struct ReadBuffersOperation ReadBuffersOperation;

/* forward declared, to avoid having to expose buf_internals.h here */

/* 前向声明，以避免在此公开 buf_internals.h。 */
struct WritebackContext;

/* forward declared, to avoid including smgr.h here */

/* 前向声明，以避免在此包含 smgr.h。 */
struct SMgrRelationData;

/* in globals.c ... this duplicates miscadmin.h */

/* 位于 globals.c 中……这与 miscadmin.h 重复。 */
extern PGDLLIMPORT int NBuffers;

/* in bufmgr.c */

/* 位于 bufmgr.c 中。 */
extern PGDLLIMPORT bool zero_damaged_pages;
extern PGDLLIMPORT int bgwriter_lru_maxpages;
extern PGDLLIMPORT double bgwriter_lru_multiplier;
extern PGDLLIMPORT bool track_io_timing;

#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 16
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 16
extern PGDLLIMPORT int effective_io_concurrency;
extern PGDLLIMPORT int maintenance_io_concurrency;

#define MAX_IO_COMBINE_LIMIT PG_IOV_MAX
#define DEFAULT_IO_COMBINE_LIMIT Min(MAX_IO_COMBINE_LIMIT, (128 * 1024) / BLCKSZ)
extern PGDLLIMPORT int io_combine_limit;	/* min of the two GUCs below */

									/* 下方两个 GUC 中的较小值。 */
extern PGDLLIMPORT int io_combine_limit_guc;
extern PGDLLIMPORT int io_max_combine_limit;

extern PGDLLIMPORT int checkpoint_flush_after;
extern PGDLLIMPORT int backend_flush_after;
extern PGDLLIMPORT int bgwriter_flush_after;

extern PGDLLIMPORT const PgAioHandleCallbacks aio_shared_buffer_readv_cb;
extern PGDLLIMPORT const PgAioHandleCallbacks aio_local_buffer_readv_cb;

/* in buf_init.c */

/* 位于 buf_init.c 中。 */
extern PGDLLIMPORT char *BufferBlocks;

/* in localbuf.c */

/* 位于 localbuf.c 中。 */
extern PGDLLIMPORT int NLocBuffer;
extern PGDLLIMPORT Block *LocalBufferBlockPointers;
extern PGDLLIMPORT int32 *LocalRefCount;

/* upper limit for effective_io_concurrency */

/* effective_io_concurrency 的上限。 */
#define MAX_IO_CONCURRENCY 1000

/* special block number for ReadBuffer() */

/* ReadBuffer() 的特殊块号。 */
#define P_NEW	InvalidBlockNumber	/* grow the file to get a new page */

									/* 扩展文件以获取新页面。 */

/*
 * Buffer content lock modes (mode argument for LockBuffer())
 *
 * 缓冲区内容锁模式（LockBuffer() 的 mode 参数）。
 */
#define BUFFER_LOCK_UNLOCK		0
#define BUFFER_LOCK_SHARE		1
#define BUFFER_LOCK_EXCLUSIVE	2


/*
 * prototypes for functions in bufmgr.c
 *
 * bufmgr.c 中函数的原型。
 */
/*
 * Prefetch a shared relation block.
 * The function starts asynchronous I/O if the requested block is not cached.
 *
 * 预取共享关系块。
 * 该函数在请求的块未缓存时启动异步 I/O。
 */
extern PrefetchBufferResult PrefetchSharedBuffer(struct SMgrRelationData *smgr_reln,
												 ForkNumber forkNum,
												 BlockNumber blockNum);

/*
 * Prefetch a relation block through its relcache entry.
 * The function resolves storage metadata then starts asynchronous prefetch when needed.
 *
 * 通过关系缓存条目预取关系块。
 * 该函数解析存储元数据，并在需要时启动异步预取。
 */
extern PrefetchBufferResult PrefetchBuffer(Relation reln, ForkNumber forkNum,
										   BlockNumber blockNum);

/*
 * Check whether a recent buffer still matches a requested block.
 * The function validates the cached handle before a caller reuses the hit.
 *
 * 检查最近缓冲区是否仍匹配请求的块。
 * 该函数在调用者重用命中前验证缓存句柄。
 */
extern bool ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum,
							 BlockNumber blockNum, Buffer recent_buffer);

/*
 * Read a main-fork relation block using normal defaults.
 * The function obtains a pinned buffer, performing I/O if the page is absent.
 *
 * 使用常规默认值读取主 fork 的关系块。
 * 该函数获得一个已固定缓冲区；若页面不存在则执行 I/O。
 */
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);

/*
 * Read a relation block with an explicit fork, mode, and strategy.
 * The function locates or initializes a buffer according to the requested read policy.
 *
 * 使用显式 fork、模式和策略读取关系块。
 * 该函数根据请求的读取策略定位或初始化缓冲区。
 */
extern Buffer ReadBufferExtended(Relation reln, ForkNumber forkNum,
								 BlockNumber blockNum, ReadBufferMode mode,
								 BufferAccessStrategy strategy);

/*
 * Read a block without requiring a relcache entry.
 * The function identifies storage directly from the locator and returns a pinned buffer.
 *
 * 无需关系缓存条目读取一个块。
 * 该函数直接通过定位符标识存储，并返回已固定的缓冲区。
 */
extern Buffer ReadBufferWithoutRelcache(RelFileLocator rlocator,
										ForkNumber forkNum, BlockNumber blockNum,
										ReadBufferMode mode, BufferAccessStrategy strategy,
										bool permanent);

/*
 * Start reading one block within a multi-read operation.
 * The function adds the block to operation state and reports whether I/O was needed.
 *
 * 在多块读取操作中启动读取一个块。
 * 该函数将块添加到操作状态，并报告是否需要 I/O。
 */
extern bool StartReadBuffer(ReadBuffersOperation *operation,
							Buffer *buffer,
							BlockNumber blocknum,
							int flags);

/*
 * Start reading a batch of blocks.
 * The function prepares buffer and AIO state for all requested blocks.
 *
 * 启动读取一批块。
 * 该函数为所有请求的块准备缓冲区和 AIO 状态。
 */
extern bool StartReadBuffers(ReadBuffersOperation *operation,
							 Buffer *buffers,
							 BlockNumber blockNum,
							 int *nblocks,
							 int flags);

/*
 * Wait for a started batch read to finish.
 * The function completes outstanding I/O and finalizes operation result state.
 *
 * 等待已启动的批量读取完成。
 * 该函数完成未完成的 I/O 并最终确定操作结果状态。
 */
extern void WaitReadBuffers(ReadBuffersOperation *operation);

/*
 * Release a pin on a buffer.
 * The function decrements buffer ownership through the current resource owner.
 *
 * 释放一个缓冲区引脚。
 * 该函数通过当前资源所有者减少缓冲区所有权。
 */
extern void ReleaseBuffer(Buffer buffer);

/*
 * Unlock a buffer and release its pin.
 * The function first removes the content lock, then performs normal buffer release.
 *
 * 解锁缓冲区并释放其引脚。
 * 该函数先移除内容锁，再执行常规缓冲区释放。
 */
extern void UnlockReleaseBuffer(Buffer buffer);

/*
 * Test whether the current backend holds an exclusive buffer lock.
 * The function checks the content-lock mode for the supplied buffer.
 *
 * 测试当前后端是否持有排他缓冲区锁。
 * 该函数检查给定缓冲区的内容锁模式。
 */
extern bool BufferIsExclusiveLocked(Buffer buffer);

/*
 * Test whether a buffer is dirty.
 * The function reads buffer state to determine whether the page needs writing.
 *
 * 测试缓冲区是否为脏。
 * 该函数读取缓冲区状态以确定页面是否需要写出。
 */
extern bool BufferIsDirty(Buffer buffer);

/*
 * Mark a buffer dirty.
 * The function updates buffer state so later writeback persists the page.
 *
 * 将缓冲区标记为脏。
 * 该函数更新缓冲区状态，使后续写回持久化该页面。
 */
extern void MarkBufferDirty(Buffer buffer);

/*
 * Increment the pin count for a buffer.
 * The function records an additional reference in current ownership state.
 *
 * 增加缓冲区的引脚计数。
 * 该函数在当前所有权状态中记录额外引用。
 */
extern void IncrBufferRefCount(Buffer buffer);

/*
 * Verify that a buffer is pinned exactly once.
 * The function checks pin ownership invariants for callers requiring sole ownership.
 *
 * 验证缓冲区恰好被固定一次。
 * 该函数检查需要独占所有权的调用者的引脚不变量。
 */
extern void CheckBufferIsPinnedOnce(Buffer buffer);

/*
 * Release one buffer and read a requested relation block.
 * The function combines pin release with a subsequent normal buffer read.
 *
 * 释放一个缓冲区并读取请求的关系块。
 * 该函数将引脚释放与随后的常规缓冲区读取结合起来。
 */
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
								   BlockNumber blockNum);

/*
 * Extend a relation by one buffered block.
 * The function allocates a new page and returns its pinned buffer.
 *
 * 将关系扩展一个缓冲块。
 * 该函数分配一个新页面并返回其已固定缓冲区。
 */
extern Buffer ExtendBufferedRel(BufferManagerRelation bmr,
								ForkNumber forkNum,
								BufferAccessStrategy strategy,
								uint32 flags);

/*
 * Extend a relation by multiple buffered blocks.
 * The function allocates a contiguous extension and reports resulting buffers and count.
 *
 * 将关系扩展多个缓冲块。
 * 该函数分配连续扩展，并报告生成的缓冲区和数量。
 */
extern BlockNumber ExtendBufferedRelBy(BufferManagerRelation bmr,
									   ForkNumber fork,
									   BufferAccessStrategy strategy,
									   uint32 flags,
									   uint32 extend_by,
									   Buffer *buffers,
									   uint32 *extended_by);

/*
 * Extend a relation until a target block exists.
 * The function grows the requested fork and returns a buffer for the target page.
 *
 * 将关系扩展到目标块存在为止。
 * 该函数增长请求的 fork，并返回目标页面的缓冲区。
 */
extern Buffer ExtendBufferedRelTo(BufferManagerRelation bmr,
								  ForkNumber fork,
								  BufferAccessStrategy strategy,
								  uint32 flags,
								  BlockNumber extend_to,
								  ReadBufferMode mode);

/*
 * Initialize backend buffer-manager access.
 * The function sets up per-backend buffer state before buffer operations.
 *
 * 初始化后端缓冲区管理器访问。
 * 该函数在缓冲区操作前设置每后端缓冲区状态。
 */
extern void InitBufferManagerAccess(void);

/*
 * Process buffer state at transaction end.
 * The function releases transaction-scoped pins and applies commit or abort handling.
 *
 * 在事务结束时处理缓冲区状态。
 * 该函数释放事务范围内的引脚，并应用提交或中止处理。
 */
extern void AtEOXact_Buffers(bool isCommit);
#ifdef USE_ASSERT_CHECKING
/*
 * Assert that held buffer locks allow catalog reads.
 * The function checks lock-order invariants in assertion-enabled builds.
 *
 * 断言持有的缓冲区锁允许读取系统目录。
 * 该函数在启用断言的构建中检查锁顺序不变量。
 */
extern void AssertBufferLocksPermitCatalogRead(void);
#endif

/*
 * Return diagnostic text for a buffer's reference count.
 * The function formats current pin ownership for debugging.
 *
 * 返回缓冲区引用计数的诊断文本。
 * 该函数格式化当前引脚所有权以便调试。
 */
extern char *DebugPrintBufferRefcount(Buffer buffer);

/*
 * Flush buffers for a checkpoint.
 * The function schedules or writes dirty shared buffers according to checkpoint flags.
 *
 * 为检查点刷写缓冲区。
 * 该函数根据检查点标志调度或写出脏共享缓冲区。
 */
extern void CheckPointBuffers(int flags);

/*
 * Return the block number contained in a buffer.
 * The function reads the buffer tag after validating the buffer handle.
 *
 * 返回缓冲区包含的块号。
 * 该函数在验证缓冲区句柄后读取缓冲区标签。
 */
extern BlockNumber BufferGetBlockNumber(Buffer buffer);

/*
 * Return the number of blocks in a relation fork.
 * The function obtains the current storage size through relation metadata.
 *
 * 返回关系 fork 中的块数量。
 * 该函数通过关系元数据获取当前存储大小。
 */
extern BlockNumber RelationGetNumberOfBlocksInFork(Relation relation,
												   ForkNumber forkNum);

/*
 * Flush one buffer to storage.
 * The function writes a dirty page and completes its I/O state transition.
 *
 * 将一个缓冲区刷写到存储。
 * 该函数写出一个脏页面并完成其 I/O 状态转换。
 */
extern void FlushOneBuffer(Buffer buffer);

/*
 * Flush all cached buffers for a relation.
 * The function locates matching dirty pages and writes them to storage.
 *
 * 刷写关系的所有缓存缓冲区。
 * 该函数定位匹配的脏页面并将其写入存储。
 */
extern void FlushRelationBuffers(Relation rel);

/*
 * Flush all cached buffers for multiple storage relations.
 * The function iterates the supplied relations and writes their dirty pages.
 *
 * 刷写多个存储关系的所有缓存缓冲区。
 * 该函数遍历给定关系并写出它们的脏页面。
 */
extern void FlushRelationsAllBuffers(struct SMgrRelationData **smgrs, int nrels);

/*
 * Create destination relation data by copying source relation data.
 * The function copies all required relation forks while honoring persistence.
 *
 * 通过复制源关系数据创建目标关系数据。
 * 该函数在遵守持久性要求的同时复制所有需要的关系 fork。
 */
extern void CreateAndCopyRelationData(RelFileLocator src_rlocator,
									  RelFileLocator dst_rlocator,
									  bool permanent);

/*
 * Flush cached buffers belonging to a database.
 * The function finds dirty pages for the database and writes them out.
 *
 * 刷写属于一个数据库的缓存缓冲区。
 * 该函数查找该数据库的脏页面并将其写出。
 */
extern void FlushDatabaseBuffers(Oid dbid);
/*
 * Drop cached buffers for selected relation forks.
 * The function invalidates pages at or after the supplied deletion blocks.
 *
 * 丢弃选定关系 fork 的缓存缓冲区。
 * 该函数使给定删除块及其后的页面失效。
 */
extern void DropRelationBuffers(struct SMgrRelationData *smgr_reln,
								ForkNumber *forkNum,
								int nforks, BlockNumber *firstDelBlock);

/*
 * Drop all cached buffers for multiple relations.
 * The function invalidates matching pages across the supplied storage relations.
 *
 * 丢弃多个关系的所有缓存缓冲区。
 * 该函数使给定存储关系中匹配的页面失效。
 */
extern void DropRelationsAllBuffers(struct SMgrRelationData **smgr_reln,
									int nlocators);

/*
 * Drop all cached buffers for a database.
 * The function invalidates pages whose tags belong to the database identifier.
 *
 * 丢弃一个数据库的所有缓存缓冲区。
 * 该函数使标签属于该数据库标识符的页面失效。
 */
extern void DropDatabaseBuffers(Oid dbid);

#define RelationGetNumberOfBlocks(reln) \
	RelationGetNumberOfBlocksInFork(reln, MAIN_FORKNUM)

/*
 * Test whether a buffer belongs to permanent storage.
 * The function inspects the buffer tag and persistence flags.
 *
 * 测试缓冲区是否属于永久存储。
 * 该函数检查缓冲区标签和持久性标志。
 */
extern bool BufferIsPermanent(Buffer buffer);

/*
 * Atomically return the page LSN from a buffer.
 * The function reads WAL position while preserving concurrent access safety.
 *
 * 以原子方式返回缓冲区中的页面 LSN。
 * 该函数在保持并发访问安全的同时读取 WAL 位置。
 */
extern XLogRecPtr BufferGetLSNAtomic(Buffer buffer);

/*
 * Return relation, fork, and block identity for a buffer.
 * The function copies the current buffer tag into caller-provided outputs.
 *
 * 返回一个缓冲区的关系、fork 和块标识。
 * 该函数将当前缓冲区标签复制到调用者提供的输出。
 */
extern void BufferGetTag(Buffer buffer, RelFileLocator *rlocator,
						 ForkNumber *forknum, BlockNumber *blknum);

/*
 * Mark a buffer dirty for a hint-bit update.
 * The function applies lightweight dirty tracking with the page-format hint.
 *
 * 为提示位更新将缓冲区标记为脏。
 * 该函数使用页面格式提示应用轻量级脏页跟踪。
 */
extern void MarkBufferDirtyHint(Buffer buffer, bool buffer_std);

/*
 * Unlock every buffer lock held by the current backend.
 * The function releases tracked content locks during cleanup paths.
 *
 * 解锁当前后端持有的每个缓冲区锁。
 * 该函数在清理路径中释放已跟踪的内容锁。
 */
extern void UnlockBuffers(void);

/*
 * Acquire or release a buffer content lock in the requested mode.
 * The function applies the mode transition to the buffer's content LWLock.
 *
 * 按请求的模式获取或释放缓冲区内容锁。
 * 该函数将模式转换应用到缓冲区的内容 LWLock。
 */
extern void LockBuffer(Buffer buffer, int mode);

/*
 * Try to acquire an exclusive buffer content lock without waiting.
 * The function returns whether the requested lock was obtained immediately.
 *
 * 尝试不等待地获取排他缓冲区内容锁。
 * 该函数返回是否立即获得请求的锁。
 */
extern bool ConditionalLockBuffer(Buffer buffer);

/*
 * Acquire a cleanup lock on a buffer.
 * The function waits for conditions that permit exclusive cleanup access.
 *
 * 获取缓冲区上的清理锁。
 * 该函数等待允许进行排他清理访问的条件。
 */
extern void LockBufferForCleanup(Buffer buffer);

/*
 * Try to acquire a cleanup lock without waiting.
 * The function tests cleanup eligibility and returns whether it acquired the lock.
 *
 * 尝试不等待地获取清理锁。
 * 该函数测试清理资格并返回是否已获得锁。
 */
extern bool ConditionalLockBufferForCleanup(Buffer buffer);

/*
 * Test whether a buffer can be cleaned up now.
 * The function checks pin and lock state for cleanup eligibility.
 *
 * 测试缓冲区当前是否可清理。
 * 该函数检查引脚和锁状态以确定清理资格。
 */
extern bool IsBufferCleanupOK(Buffer buffer);

/*
 * Test whether a held buffer pin delays recovery.
 * The function checks the current backend's pin state against recovery rules.
 *
 * 测试持有的缓冲区引脚是否延迟恢复。
 * 该函数根据恢复规则检查当前后端的引脚状态。
 */
extern bool HoldingBufferPinThatDelaysRecovery(void);

/*
 * Synchronize dirty buffers for the background writer.
 * The function selects and writes buffers using the supplied writeback context.
 *
 * 为后台写入器同步脏缓冲区。
 * 该函数使用给定写回上下文选择并写出缓冲区。
 */
extern bool BgBufferSync(struct WritebackContext *wb_context);

/*
 * Return the shared-buffer pin limit for the current backend.
 * The function derives the limit from configured and current pin resources.
 *
 * 返回当前后端的共享缓冲区引脚限制。
 * 该函数从配置和当前引脚资源派生该限制。
 */
extern uint32 GetPinLimit(void);

/*
 * Return the local-buffer pin limit for the current backend.
 * The function derives the limit for local buffer resources.
 *
 * 返回当前后端的本地缓冲区引脚限制。
 * 该函数派生本地缓冲区资源的限制。
 */
extern uint32 GetLocalPinLimit(void);

/*
 * Return the available additional shared-buffer pin capacity.
 * The function calculates headroom beyond the normal pin requirement.
 *
 * 返回可用的额外共享缓冲区引脚容量。
 * 该函数计算正常引脚需求之外的余量。
 */
extern uint32 GetAdditionalPinLimit(void);

/*
 * Return the available additional local-buffer pin capacity.
 * The function calculates local-buffer pin headroom for the backend.
 *
 * 返回可用的额外本地缓冲区引脚容量。
 * 该函数计算后端的本地缓冲区引脚余量。
 */
extern uint32 GetAdditionalLocalPinLimit(void);

/*
 * Limit requested additional shared-buffer pins.
 * The function reduces the request to capacity available to the backend.
 *
 * 限制请求的额外共享缓冲区引脚。
 * 该函数将请求减少到后端可用容量。
 */
extern void LimitAdditionalPins(uint32 *additional_pins);

/*
 * Limit requested additional local-buffer pins.
 * The function reduces the local request to capacity available to the backend.
 *
 * 限制请求的额外本地缓冲区引脚。
 * 该函数将本地请求减少到后端可用容量。
 */
extern void LimitAdditionalLocalPins(uint32 *additional_pins);

/*
 * Evict one unpinned shared buffer.
 * The function selects an eligible buffer and reports whether it required flushing.
 *
 * 驱逐一个未固定的共享缓冲区。
 * 该函数选择符合条件的缓冲区，并报告它是否需要刷写。
 */
extern bool EvictUnpinnedBuffer(Buffer buf, bool *buffer_flushed);

/*
 * Evict all eligible unpinned shared buffers.
 * The function scans buffers and returns eviction, flush, and skip counts.
 *
 * 驱逐所有符合条件的未固定共享缓冲区。
 * 该函数扫描缓冲区并返回驱逐、刷写和跳过计数。
 */
extern void EvictAllUnpinnedBuffers(int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);

/*
 * Evict eligible unpinned buffers for one relation.
 * The function restricts the scan to pages tagged with the supplied relation.
 *
 * 驱逐一个关系中符合条件的未固定缓冲区。
 * 该函数将扫描限制为带有给定关系标签的页面。
 */
extern void EvictRelUnpinnedBuffers(Relation rel,
									int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);

/* in buf_init.c */

/* 位于 buf_init.c 中。 */

/*
 * Initialize buffer-manager shared memory.
 * The function creates or attaches shared buffer descriptors during startup.
 *
 * 初始化缓冲区管理器共享内存。
 * 该函数在启动期间创建或附接共享缓冲区描述符。
 */
extern void BufferManagerShmemInit(void);

/*
 * Return buffer-manager shared-memory size.
 * The function sums shared buffer and control structure requirements.
 *
 * 返回缓冲区管理器共享内存大小。
 * 该函数汇总共享缓冲区和控制结构需求。
 */
extern Size BufferManagerShmemSize(void);

/* in localbuf.c */

/* 位于 localbuf.c 中。 */

/*
 * Release local buffers during backend process exit.
 * The function flushes or discards backend-local buffer resources.
 *
 * 在后端进程退出时释放本地缓冲区。
 * 该函数刷写或丢弃后端本地缓冲区资源。
 */
extern void AtProcExit_LocalBuffers(void);

/* in freelist.c */

/* 位于 freelist.c 中。 */

/*
 * Create an access strategy of the requested type.
 * The function initializes a strategy with its default ring configuration.
 *
 * 创建请求类型的访问策略。
 * 该函数使用默认环配置初始化策略。
 */
extern BufferAccessStrategy GetAccessStrategy(BufferAccessStrategyType btype);

/*
 * Create an access strategy with an explicit ring size.
 * The function initializes strategy state using the requested cache ring capacity.
 *
 * 使用显式环大小创建访问策略。
 * 该函数使用请求的缓存环容量初始化策略状态。
 */
extern BufferAccessStrategy GetAccessStrategyWithSize(BufferAccessStrategyType btype,
													  int ring_size_kb);

/*
 * Return the number of buffers managed by an access strategy.
 * The function exposes the strategy's ring capacity to callers.
 *
 * 返回访问策略管理的缓冲区数量。
 * 该函数向调用者公开策略的环容量。
 */
extern int	GetAccessStrategyBufferCount(BufferAccessStrategy strategy);

/*
 * Return the pin limit associated with an access strategy.
 * The function derives the required pin budget from strategy configuration.
 *
 * 返回与访问策略关联的引脚限制。
 * 该函数从策略配置派生所需的引脚预算。
 */
extern int	GetAccessStrategyPinLimit(BufferAccessStrategy strategy);

/*
 * Free an access strategy.
 * The function releases strategy and ring resources after use.
 *
 * 释放访问策略。
 * 该函数在使用后释放策略和环资源。
 */
extern void FreeAccessStrategy(BufferAccessStrategy strategy);


/* inline functions */

/* 内联函数。 */

/*
 * Although this header file is nominally backend-only, certain frontend
 * programs like pg_waldump include it.  For compilers that emit static
 * inline functions even when they're unused, that leads to unsatisfied
 * external references; hence hide these with #ifndef FRONTEND.
 *
 * 尽管此头文件名义上仅供后端使用，pg_waldump 等某些前端程序也会包含它。对于即使
 * 未使用也会生成静态内联函数的编译器，这会导致未满足的外部引用；因此使用
 * #ifndef FRONTEND 隐藏这些函数。
 */

#ifndef FRONTEND

/*
 * BufferIsValid
 *		True iff the given buffer number is valid (either as a shared
 *		or local buffer).
 *
 * Note: For a long time this was defined the same as BufferIsPinned,
 * that is it would say False if you didn't hold a pin on the buffer.
 * I believe this was bogus and served only to mask logic errors.
 * Code should always know whether it has a buffer reference,
 * independently of the pin state.
 *
 * Note: For a further long time this was not quite the inverse of the
 * BufferIsInvalid() macro, in that it also did sanity checks to verify
 * that the buffer number was in range.  Most likely, this macro was
 * originally intended only to be used in assertions, but its use has
 * since expanded quite a bit, and the overhead of making those checks
 * even in non-assert-enabled builds can be significant.  Thus, we've
 * now demoted the range checks to assertions within the macro itself.
 *
 * BufferIsValid
 *		仅当给定缓冲区编号有效（共享缓冲区或本地缓冲区）时返回 true。
 *
 * 注意：很长一段时间内，它的定义与 BufferIsPinned 相同，即当未持有缓冲区引脚时
 * 返回 false。我认为这是错误的，只会掩盖逻辑错误。代码应始终独立于引脚状态，
 * 知道自己是否具有缓冲区引用。
 *
 * 注意：又经过很长一段时间，它并不完全是 BufferIsInvalid() 宏的反面，因为它还会
 * 执行健全性检查以验证缓冲区编号处于范围内。此宏最初很可能仅打算用于断言，但其用途
 * 后来已显著扩展，即使在未启用断言的构建中执行这些检查，开销也可能很大。因此，我们
 * 现已将范围检查降级为宏自身中的断言。
 */
static inline bool
BufferIsValid(Buffer bufnum)
{
	Assert(bufnum <= NBuffers);
	Assert(bufnum >= -NLocBuffer);

	return bufnum != InvalidBuffer;
}

/*
 * BufferGetBlock
 *		Returns a reference to a disk page image associated with a buffer.
 *
 * Note:
 *		Assumes buffer is valid.
 *
 * BufferGetBlock
 *		返回与缓冲区关联的磁盘页面映像引用。
 *
 * 注意：
 *		假定缓冲区有效。
 */
static inline Block
BufferGetBlock(Buffer buffer)
{
	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferBlockPointers[-buffer - 1];
	else
		return (Block) (BufferBlocks + ((Size) (buffer - 1)) * BLCKSZ);
}

/*
 * BufferGetPageSize
 *		Returns the page size within a buffer.
 *
 * Notes:
 *		Assumes buffer is valid.
 *
 *		The buffer can be a raw disk block and need not contain a valid
 *		(formatted) disk page.
 *
 * BufferGetPageSize
 *		返回缓冲区内的页面大小。
 *
 * 注意：
 *		假定缓冲区有效。
 *
 *		缓冲区可以是原始磁盘块，且不必包含有效（已格式化）的磁盘页面。
 */
/* XXX should dig out of buffer descriptor */

/* XXX：应从缓冲区描述符中取得。 */
static inline Size
BufferGetPageSize(Buffer buffer)
{
	Assert(BufferIsValid(buffer));
	return (Size) BLCKSZ;
}

/*
 * BufferGetPage
 *		Returns the page associated with a buffer.
 *
 * BufferGetPage
 *		返回与缓冲区关联的页面。
 */
static inline Page
BufferGetPage(Buffer buffer)
{
	return (Page) BufferGetBlock(buffer);
}

#endif							/* FRONTEND */

#endif							/* BUFMGR_H */
