/*-------------------------------------------------------------------------
 *
 * smgr.h
 *	  storage manager switch public interface declarations.
 *
 *
 *	  存储管理器切换层的公共接口声明。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/smgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMGR_H
#define SMGR_H

#include "lib/ilist.h"
#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * smgr.c maintains a table of SMgrRelation objects, which are essentially
 * cached file handles.  An SMgrRelation is created (if not already present)
 * by smgropen(), and destroyed by smgrdestroy().  Note that neither of these
 * operations imply I/O, they just create or destroy a hashtable entry.  (But
 * smgrdestroy() may release associated resources, such as OS-level file
 * descriptors.)
 *
 * An SMgrRelation may be "pinned", to prevent it from being destroyed while
 * it's in use.  We use this to prevent pointers in relcache to smgr from being
 * invalidated.  SMgrRelations that are not pinned are deleted at end of
 * transaction.
 */

/*
 * smgr.c 维护一个 SMgrRelation 对象表，它们本质上是缓存的文件句柄。
 * smgropen() 会创建（若尚不存在）SMgrRelation，smgrdestroy() 会销毁它。
 * 请注意，这两个操作都不意味着 I/O；它们只创建或销毁一个哈希表条目。
 * （但 smgrdestroy() 可能释放关联资源，例如操作系统级文件描述符。）
 *
 * SMgrRelation 可以被“固定”，以防止其在使用期间被销毁。我们用此机制防止
 * relcache 中指向 smgr 的指针失效。未固定的 SMgrRelation 会在事务结束时删除。
 */
typedef struct SMgrRelationData
{
	/* rlocator is the hashtable lookup key, so it must be first! */

	/* rlocator 是哈希表查找键，因此必须位于首位！ */
	RelFileLocatorBackend smgr_rlocator;	/* relation physical identifier */

										/* 关系物理标识符。 */

	/*
	 * The following fields are reset to InvalidBlockNumber upon a cache flush
	 * event, and hold the last known size for each fork.  This information is
	 * currently only reliable during recovery, since there is no cache
	 * invalidation for fork extension.
	 */

	/*
	 * 发生缓存刷新事件时，以下字段会重置为 InvalidBlockNumber，并保存每个
	 * fork 最后已知的大小。此信息目前仅在恢复期间可靠，因为 fork 扩展没有
	 * 对应的缓存失效机制。
	 */
	BlockNumber smgr_targblock; /* current insertion target block */

									/* 当前插入目标块。 */
	BlockNumber smgr_cached_nblocks[MAX_FORKNUM + 1];	/* last known size */

									/* 最后已知大小。 */

	/* additional public fields may someday exist here */

	/* 此处将来可能会有其他公共字段。 */

	/*
	 * Fields below here are intended to be private to smgr.c and its
	 * submodules.  Do not touch them from elsewhere.
	 */

	/*
	 * 以下字段供 smgr.c 及其子模块私用。
	 * 请勿从其他位置访问它们。
	 */
	int			smgr_which;		/* storage manager selector */

									/* 存储管理器选择器。 */

	/*
	 * for md.c; per-fork arrays of the number of open segments
	 * (md_num_open_segs) and the segments themselves (md_seg_fds).
	 */

	/*
	 * 供 md.c 使用；每个 fork 的已打开段数量数组（md_num_open_segs）
	 * 和段本身（md_seg_fds）。
	 */
	int			md_num_open_segs[MAX_FORKNUM + 1];
	struct _MdfdVec *md_seg_fds[MAX_FORKNUM + 1];

	/*
	 * Pinning support.  If unpinned (ie. pincount == 0), 'node' is a list
	 * link in list of all unpinned SMgrRelations.
	 */

	/*
	 * 固定支持。若未固定（即 pincount == 0），node 是所有未固定
	 * SMgrRelation 链表中的一个链接。
	 */
	int			pincount;
	dlist_node	node;
} SMgrRelationData;

typedef SMgrRelationData *SMgrRelation;

#define SmgrIsTemp(smgr) \
	RelFileLocatorBackendIsTemp((smgr)->smgr_rlocator)

extern PGDLLIMPORT const PgAioTargetInfo aio_smgr_target_info;

/* Initialize the storage-manager subsystem.
 * Startup installs manager state and the relation-handle cache before any
 * relation file operations are dispatched.
 *
 * 初始化存储管理器子系统。
 * 启动过程会在分派任何关系文件操作前建立管理器状态和关系句柄缓存。
 */
extern void smgrinit(void);

/* Open or find a cached handle for a physical relation.
 * The locator keys the handle cache; this operation creates metadata only and
 * does not itself perform relation-file I/O.
 *
 * 打开或查找物理关系的缓存句柄。
 * 定位器是句柄缓存的键；此操作只创建元数据，本身不执行关系文件 I/O。
 */
extern SMgrRelation smgropen(RelFileLocator rlocator, ProcNumber backend);

/* Test whether a relation fork exists on storage.
 * The selected manager probes the physical fork and returns its existence.
 *
 * 测试关系 fork 是否存在于存储中。
 * 所选管理器探测物理 fork 并返回其是否存在。
 */
extern bool smgrexists(SMgrRelation reln, ForkNumber forknum);

/* Pin a relation handle while callers retain references to it.
 * The pin count prevents transaction-end cleanup from destroying the cached
 * handle.
 *
 * 在调用方保留引用期间固定关系句柄。
 * 固定计数会阻止事务结束清理销毁该缓存句柄。
 */
extern void smgrpin(SMgrRelation reln);

/* Release one pin on a relation handle.
 * When the count reaches zero, normal transaction-end cleanup may reclaim the
 * handle.
 *
 * 释放关系句柄上的一个固定引用。
 * 计数归零后，常规的事务结束清理可以回收该句柄。
 */
extern void smgrunpin(SMgrRelation reln);

/* Mark a relation handle closed for the current transaction.
 * The handle is released from active use and becomes eligible for later
 * cleanup according to its pin state.
 *
 * 将关系句柄标记为在当前事务中关闭。
 * 该句柄不再处于活动使用状态，并根据其固定状态可在稍后清理。
 */
extern void smgrclose(SMgrRelation reln);

/* Destroy every cached storage-manager handle.
 * Shutdown walks the handle table and releases remaining manager resources.
 *
 * 销毁每个缓存的存储管理器句柄。
 * 关闭过程遍历句柄表并释放剩余的管理器资源。
 */
extern void smgrdestroyall(void);

/* Release a relation handle when it is no longer needed.
 * The routine removes an unpinned handle from the cache and frees associated
 * storage-manager resources.
 *
 * 在不再需要关系句柄时释放它。
 * 此例程从缓存中移除未固定句柄并释放关联存储管理器资源。
 */
extern void smgrrelease(SMgrRelation reln);

/* Release all currently releasable relation handles.
 * The cache is scanned and each unpinned entry is discarded.
 *
 * 释放当前所有可释放的关系句柄。
 * 此函数扫描缓存并丢弃每个未固定条目。
 */
extern void smgrreleaseall(void);

/* Release cached handles for a physical relation locator.
 * The routine finds matching cached entries and removes those no longer safe
 * after a locator-level invalidation.
 *
 * 释放某个物理关系定位器的缓存句柄。
 * 此例程查找匹配缓存条目，并移除在定位器级失效后不再安全的条目。
 */
extern void smgrreleaserellocator(RelFileLocatorBackend rlocator);

/* Create a physical relation fork.
 * The selected manager creates the fork and uses isRedo to apply recovery
 * semantics when the operation is replayed from WAL.
 *
 * 创建物理关系 fork。
 * 所选管理器创建该 fork，并在从 WAL 重放操作时依据 isRedo 应用恢复语义。
 */
extern void smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);

/* Synchronize a collection of relation handles.
 * The routine dispatches sync work for each supplied handle, normally during
 * checkpoint-related processing.
 *
 * 同步一组关系句柄。
 * 此例程为每个给定句柄分派同步工作，通常在与检查点相关的处理中调用。
 */
extern void smgrdosyncall(SMgrRelation *rels, int nrels);

/* Unlink a collection of relation handles.
 * The selected manager removes each physical relation, applying redo rules
 * when requested.
 *
 * 取消链接一组关系句柄。
 * 所选管理器移除每个物理关系，并在需要时应用重做规则。
 */
extern void smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo);

/* Extend a relation fork by writing one block.
 * The manager writes buffer data at blocknum and coordinates the requested
 * fsync behavior.
 *
 * 通过写入一个块来扩展关系 fork。
 * 管理器在 blocknum 写入缓冲区数据，并协调所请求的 fsync 行为。
 */
extern void smgrextend(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, const void *buffer, bool skipFsync);
/* Extend a relation fork with zero-filled blocks.
 * The manager appends nblocks at blocknum and optionally defers fsync.
 *
 * 使用零填充块扩展关系 fork。
 * 管理器从 blocknum 开始追加 nblocks 个块，并可选择延后 fsync。
 */
extern void smgrzeroextend(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, int nblocks, bool skipFsync);
/* Request prefetch of consecutive relation blocks.
 * The manager asks the operating system or storage layer to stage the range
 * for an upcoming read and reports whether the request was accepted.
 *
 * 请求预取连续的关系块。
 * 管理器请求操作系统或存储层为即将发生的读取准备该范围，并报告请求是否被接受。
 */
extern bool smgrprefetch(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks);
/* Return the maximum blocks that may be combined in one I/O request.
 * The selected manager derives the limit for the relation and starting block.
 *
 * 返回一次 I/O 请求可合并的最大块数。
 * 所选管理器根据关系和起始块确定该限制。
 */
extern uint32 smgrmaxcombine(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blocknum);
/* Read consecutive relation blocks into a vector of buffers.
 * The manager maps the block range to storage and fills one buffer per block.
 *
 * 将连续关系块读入缓冲区向量。
 * 管理器将块范围映射到存储，并为每个块填充一个缓冲区。
 */
extern void smgrreadv(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum,
					  void **buffers, BlockNumber nblocks);
/* Start an asynchronous vectored relation read.
 * The supplied AIO handle records target metadata, then the manager queues
 * the requested block range for completion.
 *
 * 启动异步向量关系读取。
 * 提供的 AIO 句柄记录目标元数据，然后管理器将所请求块范围排入等待完成的队列。
 */
extern void smgrstartreadv(PgAioHandle *ioh,
						   SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum,
						   void **buffers, BlockNumber nblocks);
/* Write consecutive relation blocks from a vector of buffers.
 * The manager persists each buffer in the target range and honors the fsync
 * deferral request.
 *
 * 从缓冲区向量写入连续关系块。
 * 管理器持久化目标范围内的每个缓冲区，并遵守延后 fsync 的请求。
 */
extern void smgrwritev(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum,
					   const void **buffers, BlockNumber nblocks,
					   bool skipFsync);
/* Ask the storage layer to begin writeback for a block range.
 * The request schedules dirty data for flushing without waiting for durable
 * completion.
 *
 * 请求存储层开始回写一个块范围。
 * 该请求安排脏数据进行刷新，而不等待持久化完成。
 */
extern void smgrwriteback(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, BlockNumber nblocks);
/* Return the current number of blocks in a relation fork.
 * The manager queries storage to obtain an authoritative size.
 *
 * 返回关系 fork 当前的块数。
 * 管理器查询存储以获得权威大小。
 */
extern BlockNumber smgrnblocks(SMgrRelation reln, ForkNumber forknum);

/* Return the cached number of blocks in a relation fork.
 * The routine uses the handle's cached value and avoids an I/O size query.
 *
 * 返回关系 fork 的缓存块数。
 * 此例程使用句柄的缓存值，避免执行 I/O 大小查询。
 */
extern BlockNumber smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum);

/* Truncate one or more relation forks to requested sizes.
 * The manager compares old and new block counts, then shortens each selected
 * fork using the supplied arrays.
 *
 * 将一个或多个关系 fork 截断到所请求大小。
 * 管理器比较旧、新块数，然后使用给定数组缩短每个选定 fork。
 */
extern void smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks,
						 BlockNumber *old_nblocks,
						 BlockNumber *nblocks);
/* Force immediate synchronization of a relation fork.
 * The manager issues the durability operation before returning to the caller.
 *
 * 强制立即同步一个关系 fork。
 * 管理器在返回调用方前发出持久化操作。
 */
extern void smgrimmedsync(SMgrRelation reln, ForkNumber forknum);

/* Register a relation fork for later synchronization.
 * The fork is queued so checkpoint processing can batch the durability work.
 *
 * 注册一个关系 fork 以供稍后同步。
 * 该 fork 被加入队列，使检查点处理能够批量完成持久化工作。
 */
extern void smgrregistersync(SMgrRelation reln, ForkNumber forknum);

/* Perform end-of-transaction storage-manager cleanup.
 * Transaction completion releases unpinned cached handles and resets
 * transaction-local manager state.
 *
 * 执行事务结束时的存储管理器清理。
 * 事务完成会释放未固定的缓存句柄并重置事务本地管理器状态。
 */
extern void AtEOXact_SMgr(void);

/* Process a barrier request that releases storage-manager handles.
 * The barrier path applies the release action in each backend and reports
 * whether it handled the request.
 *
 * 处理释放存储管理器句柄的屏障请求。
 * 屏障路径在每个后端中应用释放操作，并报告是否处理了该请求。
 */
extern bool ProcessBarrierSmgrRelease(void);

/* Read one relation block through the vectored read interface.
 * The wrapper creates a one-element buffer vector and delegates to smgrreadv.
 *
 * 通过向量读取接口读取一个关系块。
 * 此包装函数创建单元素缓冲区向量并委托给 smgrreadv。
 */
static inline void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 void *buffer)
{
	smgrreadv(reln, forknum, blocknum, &buffer, 1);
}

/* Write one relation block through the vectored write interface.
 * The wrapper creates a one-element buffer vector and delegates to smgrwritev.
 *
 * 通过向量写入接口写入一个关系块。
 * 此包装函数创建单元素缓冲区向量并委托给 smgrwritev。
 */
static inline void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  const void *buffer, bool skipFsync)
{
	smgrwritev(reln, forknum, blocknum, &buffer, 1, skipFsync);
}

/* Configure an AIO handle for a storage-manager target.
 * The routine records the relation, fork, block range, and fsync policy so
 * later asynchronous I/O dispatch uses the correct target.
 *
 * 为存储管理器目标配置 AIO 句柄。
 * 此例程记录关系、fork、块范围和 fsync 策略，使后续异步 I/O 分派使用正确目标。
 */
extern void pgaio_io_set_target_smgr(PgAioHandle *ioh,
									 SMgrRelationData *smgr,
									 ForkNumber forknum,
									 BlockNumber blocknum,
									 int nblocks,
									 bool skip_fsync);

#endif							/* SMGR_H */
