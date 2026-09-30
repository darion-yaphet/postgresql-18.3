/*-------------------------------------------------------------------------
 *
 * fsm_internals.h
 *	  internal functions for free space map
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fsm_internals.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 空闲空间映射的内部函数。
 */
#ifndef FSM_INTERNALS_H
#define FSM_INTERNALS_H

#include "storage/buf.h"
#include "storage/bufpage.h"

/*
 * Structure of a FSM page. See src/backend/storage/freespace/README for
 * details.
 */

/*
 * FSM 页的结构。详见 src/backend/storage/freespace/README。
 */
typedef struct
{
	/*
	 * fsm_search_avail() tries to spread the load of multiple backends by
	 * returning different pages to different backends in a round-robin
	 * fashion. fp_next_slot points to the next slot to be returned (assuming
	 * there's enough space on it for the request). It's defined as an int,
	 * because it's updated without an exclusive lock. uint16 would be more
	 * appropriate, but int is more likely to be atomically
	 * fetchable/storable.
	 */

/*
 * fsm_search_avail() 通过以轮询方式向不同后端返回不同页面来分散多个后端的负载。
 * fp_next_slot 指向下一个将返回的槽位（假定它有足够空间满足请求）。该字段定义为
 * int，因为更新时不持有排他锁。uint16 更合适，但 int 更可能支持原子读取和写入。
 */
	int			fp_next_slot;

	/*
	 * fp_nodes contains the binary tree, stored in array. The first
	 * NonLeafNodesPerPage elements are upper nodes, and the following
	 * LeafNodesPerPage elements are leaf nodes. Unused nodes are zero.
	 */

/*
 * fp_nodes 包含以数组形式存储的二叉树。前 NonLeafNodesPerPage 个元素为上层节点，
 * 后续 LeafNodesPerPage 个元素为叶节点。未使用的节点为零。
 */
	uint8		fp_nodes[FLEXIBLE_ARRAY_MEMBER];
} FSMPageData;

typedef FSMPageData *FSMPage;

/*
 * Number of non-leaf and leaf nodes, and nodes in total, on an FSM page.
 * These definitions are internal to fsmpage.c.
 */

/*
 * FSM 页上的非叶节点、叶节点及节点总数。这些定义仅供 fsmpage.c 内部使用。
 */
#define NodesPerPage (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
					  offsetof(FSMPageData, fp_nodes))

#define NonLeafNodesPerPage (BLCKSZ / 2 - 1)
#define LeafNodesPerPage (NodesPerPage - NonLeafNodesPerPage)

/*
 * Number of FSM "slots" on a FSM page. This is what should be used
 * outside fsmpage.c.
 */

/*
 * FSM 页上的 FSM“槽位”数量。这是 fsmpage.c 外部应使用的定义。
 */
#define SlotsPerFSMPage LeafNodesPerPage

/* Prototypes for functions in fsmpage.c */

/*
 * fsmpage.c 中函数的原型。
 */
/*
 * Searches the FSM tree for a slot whose available space meets the minimum.
 * It optionally advances the round-robin cursor while observing lock ownership.
 */

/*
 * 在 FSM 树中搜索可用空间满足最小值的槽位；它可选地推进轮询游标，并考虑锁的持有状态。
 */
extern int	fsm_search_avail(Buffer buf, uint8 minvalue, bool advancenext,
							 bool exclusive_lock_held);
/*
 * Returns the available-space value stored for one FSM slot.
 */

/*
 * 返回一个 FSM 槽位中存储的可用空间值。
 */
extern uint8 fsm_get_avail(Page page, int slot);
/*
 * Returns the largest available-space value represented by the page.
 */

/*
 * 返回该页表示的最大可用空间值。
 */
extern uint8 fsm_get_max_avail(Page page);
/*
 * Updates a slot's available-space value and reports whether the tree changed.
 */

/*
 * 更新槽位的可用空间值，并报告树是否改变。
 */
extern bool fsm_set_avail(Page page, int slot, uint8 value);
/*
 * Removes availability information for slots beyond the new limit.
 */

/*
 * 移除超出新限制的槽位的可用空间信息。
 */
extern bool fsm_truncate_avail(Page page, int nslots);
/*
 * Rebuilds an FSM page's internal summary tree from its leaf values.
 */

/*
 * 根据叶节点值重建 FSM 页的内部摘要树。
 */
extern bool fsm_rebuild_page(Page page);

#endif							/* FSM_INTERNALS_H */
