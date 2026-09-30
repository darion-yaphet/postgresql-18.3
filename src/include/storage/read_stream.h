/*-------------------------------------------------------------------------
 *
 * read_stream.h
 *	  Mechanism for accessing buffered relation data with look-ahead
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/read_stream.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef READ_STREAM_H
#define READ_STREAM_H

#include "storage/bufmgr.h"
#include "storage/smgr.h"

/* Default tuning, reasonable for many users. */

/* 默认调优，适合许多用户。 */
#define READ_STREAM_DEFAULT 0x00

/*
 * I/O streams that are performing maintenance work on behalf of potentially
 * many users, and thus should be governed by maintenance_io_concurrency
 * instead of effective_io_concurrency.  For example, VACUUM or CREATE INDEX.
 */

/*
 * 代表潜在许多用户执行维护工作的 I/O 流应由 maintenance_io_concurrency 而非
 * effective_io_concurrency 管理，例如 VACUUM 或 CREATE INDEX。
 */
#define READ_STREAM_MAINTENANCE 0x01

/*
 * We usually avoid issuing prefetch advice automatically when sequential
 * access is detected, but this flag explicitly disables it, for cases that
 * might not be correctly detected.  Explicit advice is known to perform worse
 * than letting the kernel (at least Linux) detect sequential access.
 */

/*
 * 检测到顺序访问时，通常会避免自动发出预取建议；该标志会显式禁用这种做法，以处理
 * 可能无法正确检测的情形。已知显式建议的性能不如让内核（至少是 Linux）检测顺序访问。
 */
#define READ_STREAM_SEQUENTIAL 0x02

/*
 * We usually ramp up from smaller reads to larger ones, to support users who
 * don't know if it's worth reading lots of buffers yet.  This flag disables
 * that, declaring ahead of time that we'll be reading all available buffers.
 */

/*
 * 通常会从较小读取逐步提升到较大读取，以支持尚不确定是否值得读取大量缓冲区的用户。
 * 此标志禁用该行为，预先声明将读取所有可用缓冲区。
 */
#define READ_STREAM_FULL 0x04

/* ---
 * Opt-in to using AIO batchmode.
 *
 * Submitting IO in larger batches can be more efficient than doing so
 * one-by-one, particularly for many small reads. It does, however, require
 * the ReadStreamBlockNumberCB callback to abide by the restrictions of AIO
 * batching (c.f. pgaio_enter_batchmode()). Basically, the callback may not:
 *
 * a) block without first calling pgaio_submit_staged(), unless a
 *    to-be-waited-on lock cannot be part of a deadlock, e.g. because it is
 *    never held while waiting for IO.
 *
 * b) start another batch (without first exiting batchmode and re-entering
 *    before returning)
 *
 * As this requires care and is nontrivial in some cases, batching is only
 * used with explicit opt-in.
 * ---
 */

/* ---
 * 选择使用 AIO 批处理模式。
 *
 * 以更大的批次提交 I/O 可能比逐个提交更高效，尤其适用于大量小读取。不过，这要求
 * ReadStreamBlockNumberCB 回调遵守 AIO 批处理限制（参见 pgaio_enter_batchmode()）。
 * 基本上，该回调不得：
 *
 * a) 在未先调用 pgaio_submit_staged() 时阻塞，除非等待的锁不可能形成死锁，例如该锁
 *    从不在等待 I/O 时持有。
 *
 * b) 在返回之前启动另一批处理（除非先退出批处理模式并重新进入）。
 *
 * 由于这需要谨慎处理，且在部分场景并不简单，批处理仅在显式选择时使用。
 * ---
 */
#define READ_STREAM_USE_BATCHING 0x08

struct ReadStream;
typedef struct ReadStream ReadStream;

/* for block_range_read_stream_cb */

/* 用于 block_range_read_stream_cb。 */
typedef struct BlockRangeReadStreamPrivate
{
	BlockNumber current_blocknum;
	BlockNumber last_exclusive;
} BlockRangeReadStreamPrivate;

/* Callback that returns the next block number to read. */

/* 返回下一个要读取的块号的回调。 */
typedef BlockNumber (*ReadStreamBlockNumberCB) (ReadStream *stream,
												void *callback_private_data,
												void *per_buffer_data);

/*
 * Returns the next block in a configured block range.
 */

/*
 * 返回已配置块范围中的下一个块。
 */
extern BlockNumber block_range_read_stream_cb(ReadStream *stream,
											  void *callback_private_data,
											  void *per_buffer_data);
/*
 * Starts a buffered read stream for one relation and its fork.
 */

/*
 * 为一个关系及其分叉文件启动缓冲读取流。
 */
extern ReadStream *read_stream_begin_relation(int flags,
											  BufferAccessStrategy strategy,
											  Relation rel,
											  ForkNumber forknum,
											  ReadStreamBlockNumberCB callback,
											  void *callback_private_data,
											  size_t per_buffer_data_size);
/*
 * Advances the stream and returns its next pinned buffer.
 */

/*
 * 推进读取流并返回下一个已固定的缓冲区。
 */
extern Buffer read_stream_next_buffer(ReadStream *stream, void **per_buffer_data);
/*
 * Advances the stream and returns the next block number and strategy.
 */

/*
 * 推进读取流并返回下一个块号和访问策略。
 */
extern BlockNumber read_stream_next_block(ReadStream *stream,
										  BufferAccessStrategy *strategy);
/*
 * Starts a read stream using an already-open storage-manager relation.
 */

/*
 * 使用已打开的存储管理器关系启动读取流。
 */
extern ReadStream *read_stream_begin_smgr_relation(int flags,
												   BufferAccessStrategy strategy,
												   SMgrRelation smgr,
												   char smgr_persistence,
												   ForkNumber forknum,
												   ReadStreamBlockNumberCB callback,
												   void *callback_private_data,
												   size_t per_buffer_data_size);
/*
 * Resets a read stream so that it can begin a new pass.
 */

/*
 * 重置读取流，使其可以开始新的遍历。
 */
extern void read_stream_reset(ReadStream *stream);
/*
 * Ends a read stream and releases its resources.
 */

/*
 * 结束读取流并释放其资源。
 */
extern void read_stream_end(ReadStream *stream);

#endif							/* READ_STREAM_H */
