/*
 * brin_revmap.h
 *		Prototypes for BRIN reverse range maps
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin_revmap.h
 */

#ifndef BRIN_REVMAP_H
#define BRIN_REVMAP_H

#include "access/brin_tuple.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "storage/off.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

/* struct definition lives in brin_revmap.c */

/* 结构体定义位于 brin_revmap.c 中 */
typedef struct BrinRevmap BrinRevmap;

/*
 * Open and initialize access to a BRIN index's reverse range map, reading
 * the metapage to learn the pages-per-range value (returned via the output
 * parameter) and returning a handle used for subsequent revmap operations.
 *
 * 打开并初始化对某个 BRIN 索引反向范围映射（revmap）的访问，读取元页
 * 以获知每个范围的页数（通过输出参数返回），并返回一个供后续 revmap
 * 操作使用的句柄。
 */
extern BrinRevmap *brinRevmapInitialize(Relation idxrel,
										BlockNumber *pagesPerRange);

/*
 * Release resources associated with a BRIN revmap access handle, unpinning
 * any buffers it holds and freeing the handle itself.
 *
 * 释放与某个 BRIN revmap 访问句柄相关联的资源，取消其持有的所有缓冲区
 * 的固定（unpin），并释放该句柄本身。
 */
extern void brinRevmapTerminate(BrinRevmap *revmap);

/*
 * Ensure that the revmap contains enough pages to hold an entry for the
 * given heap block, physically extending the revmap (allocating new revmap
 * pages) if necessary.
 *
 * 确保 revmap 中包含足够多的页来容纳给定堆块对应的条目，必要时通过
 * 分配新的 revmap 页来物理扩展 revmap。
 */
extern void brinRevmapExtend(BrinRevmap *revmap,
							 BlockNumber heapBlk);

/*
 * Lock the revmap page that holds the entry for the given heap block for
 * update, returning the pinned and locked buffer so the caller can safely
 * modify the mapping for that block range.
 *
 * 为更新而锁定持有给定堆块条目的 revmap 页，返回已固定并加锁的缓冲区，
 * 以便调用者能够安全地修改该块范围的映射。
 */
extern Buffer brinLockRevmapPageForUpdate(BrinRevmap *revmap,
										  BlockNumber heapBlk);

/*
 * Set the revmap entry for a heap block range so it points at the given
 * item pointer (tid), i.e. record which index tuple summarizes that range.
 *
 * 设置某个堆块范围的 revmap 条目，使其指向给定的项指针（tid），即记录
 * 哪个索引元组汇总（summarize）了该范围。
 */
extern void brinSetHeapBlockItemptr(Buffer buf, BlockNumber pagesPerRange,
									BlockNumber heapBlk, ItemPointerData tid);

/*
 * Look up and return the on-disk BRIN tuple that summarizes the range
 * containing the given heap block, following the revmap to the regular page,
 * returning the buffer, offset and size via output parameters; the mode
 * controls the buffer locking used.
 *
 * 查找并返回汇总了包含给定堆块的范围的磁盘上 BRIN 元组，沿着 revmap
 * 定位到普通页，通过输出参数返回缓冲区、偏移量和大小；mode 参数控制
 * 所使用的缓冲区加锁方式。
 */
extern BrinTuple *brinGetTupleForHeapBlock(BrinRevmap *revmap,
										   BlockNumber heapBlk, Buffer *buf, OffsetNumber *off,
										   Size *size, int mode);

/*
 * Remove the summary for the page range containing the given heap block,
 * clearing its revmap entry and the associated index tuple so the range
 * becomes unsummarized (and can be re-summarized later).  Returns true if a
 * summary was actually removed.
 *
 * 移除包含给定堆块的页范围的汇总信息，清除其 revmap 条目及关联的索引
 * 元组，使该范围变为未汇总状态（之后可以重新汇总）。若确实移除了某个
 * 汇总则返回 true。
 */
extern bool brinRevmapDesummarizeRange(Relation idxrel, BlockNumber heapBlk);

#endif							/* BRIN_REVMAP_H */
