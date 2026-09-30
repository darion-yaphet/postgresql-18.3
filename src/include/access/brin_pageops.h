/*
 * brin_pageops.h
 *		Prototypes for operating on BRIN pages.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/access/brin_pageops.h
 */
#ifndef BRIN_PAGEOPS_H
#define BRIN_PAGEOPS_H

#include "access/brin_revmap.h"

/*
 * Replace an existing on-disk BRIN tuple with an updated version.  If the
 * new tuple fits, it may be written in place on the same page (samepage);
 * otherwise a new tuple is inserted elsewhere and the revmap is updated to
 * point at the new location.  Returns true if the update succeeded.
 *
 * 用更新后的版本替换一个已存在的磁盘上的 BRIN 元组。如果新元组能够
 * 容纳，则可以就地写入同一页（samepage）；否则会在别处插入新元组，
 * 并更新 revmap 使其指向新位置。若更新成功则返回 true。
 */
extern bool brin_doupdate(Relation idxrel, BlockNumber pagesPerRange,
						  BrinRevmap *revmap, BlockNumber heapBlk,
						  Buffer oldbuf, OffsetNumber oldoff,
						  const BrinTuple *origtup, Size origsz,
						  const BrinTuple *newtup, Size newsz,
						  bool samepage);

/*
 * Determine whether a new tuple of size newsz can replace an existing tuple
 * of size origsz on the given buffer's page without needing to move it to a
 * different page (i.e. whether there is enough free space in place).
 *
 * 判断大小为 newsz 的新元组是否能够在给定缓冲区所在页上替换大小为
 * origsz 的已有元组，而无需将其移动到另一页（即就地是否有足够的
 * 空闲空间）。
 */
extern bool brin_can_do_samepage_update(Buffer buffer, Size origsz,
										Size newsz);

/*
 * Insert a new BRIN tuple into the index, finding or extending a page with
 * enough free space, writing the tuple there, and updating the revmap so the
 * heap block range maps to the tuple.  Returns the offset of the new tuple.
 *
 * 向索引中插入一个新的 BRIN 元组：查找或扩展一个具有足够空闲空间的
 * 页，将元组写入该页，并更新 revmap 使堆块范围映射到该元组。返回新
 * 元组的偏移量。
 */
extern OffsetNumber brin_doinsert(Relation idxrel, BlockNumber pagesPerRange,
								  BrinRevmap *revmap, Buffer *buffer, BlockNumber heapBlk,
								  BrinTuple *tup, Size itemsz);

/*
 * Initialize a BRIN index page of the given type by setting up its page
 * header and BRIN-specific special space so it is ready for use.
 *
 * 通过设置页头和 BRIN 专用的特殊空间来初始化给定类型的 BRIN 索引页，
 * 使其准备好被使用。
 */
extern void brin_page_init(Page page, uint16 type);

/*
 * Initialize the BRIN metapage, recording the pages-per-range setting and
 * the on-disk version so the index metadata is properly established.
 *
 * 初始化 BRIN 元页，记录每个范围的页数（pages-per-range）设置以及
 * 磁盘上的版本号，从而正确建立索引的元数据。
 */
extern void brin_metapage_init(Page page, BlockNumber pagesPerRange,
							   uint16 version);

/*
 * Mark a regular BRIN page as being evacuated in preparation for shrinking
 * the index during a summarization/cleanup pass.  Returns true if the page
 * was successfully put into the evacuating state.
 *
 * 将一个普通的 BRIN 页标记为正在疏散（evacuating），为在汇总/清理过程
 * 中收缩索引做准备。如果该页成功进入疏散状态则返回 true。
 */
extern bool brin_start_evacuating_page(Relation idxRel, Buffer buf);

/*
 * Move all tuples off a page that is being evacuated, re-inserting them into
 * other pages and updating the revmap accordingly, so the page can later be
 * freed.
 *
 * 将正在被疏散的页上的所有元组迁走，把它们重新插入到其他页并相应地
 * 更新 revmap，以便该页之后可以被释放。
 */
extern void brin_evacuate_page(Relation idxRel, BlockNumber pagesPerRange,
							   BrinRevmap *revmap, Buffer buf);

/*
 * Reclaim free space on a BRIN page during cleanup, updating the page's free
 * space information (e.g. the FSM) so the space becomes available for future
 * inserts.
 *
 * 在清理过程中回收 BRIN 页上的空闲空间，更新该页的空闲空间信息（例如
 * FSM），使这些空间可供未来的插入使用。
 */
extern void brin_page_cleanup(Relation idxrel, Buffer buf);

#endif							/* BRIN_PAGEOPS_H */
