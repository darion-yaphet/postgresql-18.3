/*-------------------------------------------------------------------------
 *
 * md.h
 *	  magnetic disk storage manager public interface declarations.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/md.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MD_H
#define MD_H

#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"

extern PGDLLIMPORT const PgAioHandleCallbacks aio_md_readv_cb;

/* md storage manager functionality */

/* md 存储管理器功能。 */
/*
 * Initializes the md storage manager for relation-file operations.
 */

/*
 * 初始化用于关系文件操作的 md 存储管理器。
 */
extern void mdinit(void);
/*
 * Opens the storage-manager state for a relation.
 */

/*
 * 打开关系的存储管理器状态。
 */
extern void mdopen(SMgrRelation reln);
/*
 * Closes the relation fork managed by md.
 */

/*
 * 关闭由 md 管理的关系分叉文件。
 */
extern void mdclose(SMgrRelation reln, ForkNumber forknum);
/*
 * Creates a relation fork, optionally while replaying WAL.
 */

/*
 * 创建关系分叉文件，并可在 WAL 重放期间使用。
 */
extern void mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
/*
 * Tests whether a relation fork exists on disk.
 */

/*
 * 检查关系分叉文件是否存在于磁盘上。
 */
extern bool mdexists(SMgrRelation reln, ForkNumber forknum);
/*
 * Removes a relation fork and coordinates redo processing when required.
 */

/*
 * 移除关系分叉文件，并在需要时协调重做处理。
 */
extern void mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo);
/*
 * Extends a relation fork by writing one block.
 */

/*
 * 通过写入一个块扩展关系分叉文件。
 */
extern void mdextend(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, const void *buffer, bool skipFsync);
/*
 * Extends a relation fork with zero-filled blocks.
 */

/*
 * 用零填充的块扩展关系分叉文件。
 */
extern void mdzeroextend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks, bool skipFsync);
/*
 * Requests prefetching of consecutive relation blocks.
 */

/*
 * 请求预取连续的关系块。
 */
extern bool mdprefetch(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum, int nblocks);
/*
 * Returns the largest I/O combination size for the requested blocks.
 */

/*
 * 返回请求块可合并执行 I/O 的最大大小。
 */
extern uint32 mdmaxcombine(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum);
/*
 * Reads consecutive blocks from a relation fork into supplied buffers.
 */

/*
 * 将关系分叉文件中的连续块读入提供的缓冲区。
 */
extern void mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					void **buffers, BlockNumber nblocks);
/*
 * Starts an asynchronous vector read for consecutive relation blocks.
 */

/*
 * 为连续关系块启动异步向量读取。
 */
extern void mdstartreadv(PgAioHandle *ioh,
						 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
						 void **buffers, BlockNumber nblocks);
/*
 * Writes consecutive buffers to a relation fork.
 */

/*
 * 将连续缓冲区写入关系分叉文件。
 */
extern void mdwritev(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum,
					 const void **buffers, BlockNumber nblocks, bool skipFsync);
/*
 * Advises writeback for a range of relation blocks.
 */

/*
 * 建议为一段关系块执行回写。
 */
extern void mdwriteback(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks);
/*
 * Returns the number of blocks currently in a relation fork.
 */

/*
 * 返回关系分叉文件当前包含的块数。
 */
extern BlockNumber mdnblocks(SMgrRelation reln, ForkNumber forknum);
/*
 * Truncates a relation fork from its current size to the requested size.
 */

/*
 * 将关系分叉文件从当前大小截断为请求大小。
 */
extern void mdtruncate(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber curnblk, BlockNumber nblocks);
/*
 * Immediately synchronizes a relation fork to durable storage.
 */

/*
 * 立即将关系分叉文件同步到持久存储。
 */
extern void mdimmedsync(SMgrRelation reln, ForkNumber forknum);
/*
 * Registers a relation fork for deferred synchronization.
 */

/*
 * 注册关系分叉文件以进行延迟同步。
 */
extern void mdregistersync(SMgrRelation reln, ForkNumber forknum);
/*
 * Obtains the file descriptor and offset for a relation block.
 */

/*
 * 获取关系块对应的文件描述符和偏移量。
 */
extern int	mdfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off);

/*
 * Discards queued synchronization requests for one database.
 */

/*
 * 丢弃某个数据库已排队的同步请求。
 */
extern void ForgetDatabaseSyncRequests(Oid dbid);
/*
 * Drops the specified relation files, including redo-aware cleanup.
 */

/*
 * 删除指定的关系文件，并执行可感知重做的清理。
 */
extern void DropRelationFiles(RelFileLocator *delrels, int ndelrels, bool isRedo);

/* md sync callbacks */

/* md 同步回调。 */
/*
 * Synchronizes the file named by a file-tag callback request.
 */

/*
 * 同步由文件标签回调请求指定的文件。
 */
extern int	mdsyncfiletag(const FileTag *ftag, char *path);
/*
 * Unlinks the file named by a file-tag callback request.
 */

/*
 * 删除由文件标签回调请求指定的文件。
 */
extern int	mdunlinkfiletag(const FileTag *ftag, char *path);
/*
 * Tests whether two file tags refer to the same md file.
 */

/*
 * 检查两个文件标签是否指向同一个 md 文件。
 */
extern bool mdfiletagmatches(const FileTag *ftag, const FileTag *candidate);

#endif							/* MD_H */
