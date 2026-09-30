/*-------------------------------------------------------------------------
 *
 * indexfsm.h
 *	  POSTGRES free space map for quickly finding an unused page in index
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/indexfsm.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * PostgreSQL 用于快速查找索引中未使用页面的空闲空间映射。
 */
#ifndef INDEXFSM_H_
#define INDEXFSM_H_

#include "storage/block.h"
#include "utils/relcache.h"

/*
 * Finds an unused page in an index through its free-space map.
 */

/*
 * 通过空闲空间映射查找索引中的未使用页面。
 */
extern BlockNumber GetFreeIndexPage(Relation rel);
/*
 * Records an index page as free in the index free-space map.
 */

/*
 * 在索引空闲空间映射中将索引页记录为空闲。
 */
extern void RecordFreeIndexPage(Relation rel, BlockNumber freeBlock);
/*
 * Records an index page as used in the index free-space map.
 */

/*
 * 在索引空闲空间映射中将索引页记录为已使用。
 */
extern void RecordUsedIndexPage(Relation rel, BlockNumber usedBlock);

/*
 * Vacuums stale entries from an index free-space map.
 */

/*
 * 从索引空闲空间映射中清理过期条目。
 */
extern void IndexFreeSpaceMapVacuum(Relation rel);

#endif							/* INDEXFSM_H_ */
