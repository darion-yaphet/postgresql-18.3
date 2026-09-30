/*-------------------------------------------------------------------------
 *
 * freespace.h
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/freespace.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * PostgreSQL 用于快速查找关系中空闲空间的空闲空间映射。
 */
#ifndef FREESPACE_H_
#define FREESPACE_H_

#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* prototypes for public functions in freespace.c */

/*
 * freespace.c 中公共函数的原型。
 */
/*
 * Returns the free space recorded for a relation page.
 */

/*
 * 返回关系页中记录的空闲空间。
 */
extern Size GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk);
/*
 * Finds a relation page with at least the requested amount of free space.
 */

/*
 * 查找具有至少所需空闲空间的关系页。
 */
extern BlockNumber GetPageWithFreeSpace(Relation rel, Size spaceNeeded);
/*
 * Records the old page's space and finds a page suitable for a new request.
 */

/*
 * 记录旧页的空闲空间，并查找适合新请求的页面。
 */
extern BlockNumber RecordAndGetPageWithFreeSpace(Relation rel,
												 BlockNumber oldPage,
												 Size oldSpaceAvail,
												 Size spaceNeeded);
/*
 * Records the free space available on a relation page.
 */

/*
 * 记录关系页上可用的空闲空间。
 */
extern void RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk,
									Size spaceAvail);
/*
 * Emits a WAL record describing a relation page's free-space value.
 */

/*
 * 写入描述关系页空闲空间值的 WAL 记录。
 */
extern void XLogRecordPageWithFreeSpace(RelFileLocator rlocator, BlockNumber heapBlk,
										Size spaceAvail);

/*
 * Prepares a relation's free-space map for truncation and returns its new length.
 */

/*
 * 为截断关系准备其空闲空间映射，并返回新的长度。
 */
extern BlockNumber FreeSpaceMapPrepareTruncateRel(Relation rel,
												  BlockNumber nblocks);
/*
 * Vacuums a relation's complete free-space map.
 */

/*
 * 清理关系的完整空闲空间映射。
 */
extern void FreeSpaceMapVacuum(Relation rel);
/*
 * Vacuums the specified range of a relation's free-space map.
 */

/*
 * 清理关系空闲空间映射的指定范围。
 */
extern void FreeSpaceMapVacuumRange(Relation rel, BlockNumber start,
									BlockNumber end);

#endif							/* FREESPACE_H_ */
