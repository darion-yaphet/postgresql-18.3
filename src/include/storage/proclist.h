/*-------------------------------------------------------------------------
 *
 * proclist.h
 *		operations on doubly-linked lists of pgprocnos
 *
 * The interface is similar to dlist from ilist.h, but uses pgprocno instead
 * of pointers.  This allows proclist_head to be mapped at different addresses
 * in different backends.
 *
 * See proclist_types.h for the structs that these functions operate on.  They
 * are separated to break a header dependency cycle with proc.h.
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist.h
 *-------------------------------------------------------------------------
 */
#ifndef PROCLIST_H
#define PROCLIST_H

#include "storage/proc.h"
#include "storage/proclist_types.h"

/*
 * Initialize a proclist.
 */

/*
 * 初始化一个 proclist。
 */
/*
 * Resets both endpoints so subsequent insertions establish a new list.
 */

/*
 * 重置两个端点，使后续插入操作建立一个新链表。
 */
static inline void
proclist_init(proclist_head *list)
{
	list->head = list->tail = INVALID_PROC_NUMBER;
}

/*
 * Is the list empty?
 */

/*
 * 链表是否为空？
 */
/*
 * Determines emptiness by checking the sentinel value of the head endpoint.
 */

/*
 * 通过检查头端点的哨兵值确定是否为空。
 */
static inline bool
proclist_is_empty(const proclist_head *list)
{
	return list->head == INVALID_PROC_NUMBER;
}

/*
 * Get a pointer to a proclist_node inside a given PGPROC, given a procno and
 * the proclist_node field's offset within struct PGPROC.
 */

/*
 * 根据 procno 和 struct PGPROC 内 proclist_node 字段的偏移量，获取给定 PGPROC
 * 内部 proclist_node 的指针。
 */
/*
 * Locates the PGPROC first, then applies the supplied member offset.
 */

/*
 * 先定位 PGPROC，再应用给定成员偏移量。
 */
static inline proclist_node *
proclist_node_get(int procno, size_t node_offset)
{
	char	   *entry = (char *) GetPGProcByNumber(procno);

	return (proclist_node *) (entry + node_offset);
}

/*
 * Insert a process at the beginning of a list.
 */

/*
 * 在链表开头插入一个进程。
 */
/*
 * Links an unused node before the current head and updates both endpoints when empty.
 */

/*
 * 将未使用节点链接到当前头部之前，并在链表为空时更新两个端点。
 */
static inline void
proclist_push_head_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	Assert(node->next == 0 && node->prev == 0);

	if (list->head == INVALID_PROC_NUMBER)
	{
		Assert(list->tail == INVALID_PROC_NUMBER);
		node->next = node->prev = INVALID_PROC_NUMBER;
		list->head = list->tail = procno;
	}
	else
	{
		Assert(list->tail != INVALID_PROC_NUMBER);
		Assert(list->head != procno);
		Assert(list->tail != procno);
		node->next = list->head;
		proclist_node_get(node->next, node_offset)->prev = procno;
		node->prev = INVALID_PROC_NUMBER;
		list->head = procno;
	}
}

/*
 * Insert a process at the end of a list.
 */

/*
 * 在链表末尾插入一个进程。
 */
/*
 * Links an unused node after the current tail and updates both endpoints when empty.
 */

/*
 * 将未使用节点链接到当前尾部之后，并在链表为空时更新两个端点。
 */
static inline void
proclist_push_tail_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	Assert(node->next == 0 && node->prev == 0);

	if (list->tail == INVALID_PROC_NUMBER)
	{
		Assert(list->head == INVALID_PROC_NUMBER);
		node->next = node->prev = INVALID_PROC_NUMBER;
		list->head = list->tail = procno;
	}
	else
	{
		Assert(list->head != INVALID_PROC_NUMBER);
		Assert(list->head != procno);
		Assert(list->tail != procno);
		node->prev = list->tail;
		proclist_node_get(node->prev, node_offset)->next = procno;
		node->next = INVALID_PROC_NUMBER;
		list->tail = procno;
	}
}

/*
 * Delete a process from a list --- it must be in the list!
 */

/*
 * 从链表删除一个进程——它必须在链表中！
 */
/*
 * Relinks neighboring nodes, updates affected endpoints, and clears the removed node links.
 */

/*
 * 重新链接相邻节点，更新受影响端点，并清除已删除节点的链接。
 */
static inline void
proclist_delete_offset(proclist_head *list, int procno, size_t node_offset)
{
	proclist_node *node = proclist_node_get(procno, node_offset);

	Assert(node->next != 0 || node->prev != 0);

	if (node->prev == INVALID_PROC_NUMBER)
	{
		Assert(list->head == procno);
		list->head = node->next;
	}
	else
		proclist_node_get(node->prev, node_offset)->next = node->next;

	if (node->next == INVALID_PROC_NUMBER)
	{
		Assert(list->tail == procno);
		list->tail = node->prev;
	}
	else
		proclist_node_get(node->next, node_offset)->prev = node->prev;

	node->next = node->prev = 0;
}

/*
 * Check if a process is currently in a list.  It must be known that the
 * process is not in any _other_ proclist that uses the same proclist_node,
 * so that the only possibilities are that it is in this list or none.
 */

/*
 * 检查一个进程当前是否在链表中。必须已知该进程不在使用同一 proclist_node 的任何其他
 * proclist 中，因此它只能在此链表中或不在任何链表中。
 */
/*
 * Checks empty links first, then validates head and tail membership in constant time.
 */

/*
 * 先检查空链接，再以常数时间验证头部和尾部成员关系。
 */
static inline bool
proclist_contains_offset(const proclist_head *list, int procno,
						 size_t node_offset)
{
	const proclist_node *node = proclist_node_get(procno, node_offset);

	/* If it's not in any list, it's definitely not in this one. */

	/* 如果它不在任何链表中，则肯定不在此链表中。 */
	if (node->prev == 0 && node->next == 0)
		return false;

	/*
	 * It must, in fact, be in this list.  Ideally, in assert-enabled builds,
	 * we'd verify that.  But since this function is typically used while
	 * holding a spinlock, crawling the whole list is unacceptable.  However,
	 * we can verify matters in O(1) time when the node is a list head or
	 * tail, and that seems worth doing, since in practice that should often
	 * be enough to catch mistakes.
	 */

	/*
	 * 它实际上必须在此链表中。理想情况下，在启用断言的构建中应验证这一点。但此函数通常
	 * 在持有自旋锁时使用，遍历整个链表不可接受。不过，当节点是链表头或尾时，可在 O(1)
	 * 时间验证；这值得做，因为实践中通常足以捕获错误。
	 */
	Assert(node->prev != INVALID_PROC_NUMBER || list->head == procno);
	Assert(node->next != INVALID_PROC_NUMBER || list->tail == procno);

	return true;
}

/*
 * Remove and return the first process from a list (there must be one).
 */

/*
 * 从链表移除并返回第一个进程（必须存在一个）。
 */
/*
 * Fetches the head process, delegates unlinking, and returns the removed PGPROC.
 */

/*
 * 获取头部进程，委托执行解除链接，并返回已删除的 PGPROC。
 */
static inline PGPROC *
proclist_pop_head_node_offset(proclist_head *list, size_t node_offset)
{
	PGPROC	   *proc;

	Assert(!proclist_is_empty(list));
	proc = GetPGProcByNumber(list->head);
	proclist_delete_offset(list, list->head, node_offset);
	return proc;
}

/*
 * Helper macros to avoid repetition of offsetof(PGPROC, <member>).
 * 'link_member' is the name of a proclist_node member in PGPROC.
 */

/*
 * 用于避免重复书写 offsetof(PGPROC, <member>) 的辅助宏。link_member 是 PGPROC 中
 * proclist_node 成员的名称。
 */
#define proclist_delete(list, procno, link_member) \
	proclist_delete_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_push_head(list, procno, link_member) \
	proclist_push_head_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_push_tail(list, procno, link_member) \
	proclist_push_tail_offset((list), (procno), offsetof(PGPROC, link_member))
#define proclist_pop_head_node(list, link_member) \
	proclist_pop_head_node_offset((list), offsetof(PGPROC, link_member))
#define proclist_contains(list, procno, link_member) \
	proclist_contains_offset((list), (procno), offsetof(PGPROC, link_member))

/*
 * Iterate through the list pointed at by 'lhead', storing the current
 * position in 'iter'.  'link_member' is the name of a proclist_node member in
 * PGPROC.  Access the current position with iter.cur.
 *
 * The only list modification allowed while iterating is deleting the current
 * node with proclist_delete(list, iter.cur, node_offset).
 */

/*
 * 遍历由 lhead 指向的链表，将当前位置存储在 iter 中。link_member 是 PGPROC 中
 * proclist_node 成员的名称。通过 iter.cur 访问当前位置。
 *
 * 迭代期间仅允许通过 proclist_delete(list, iter.cur, node_offset) 删除当前节点。
 */
#define proclist_foreach_modify(iter, lhead, link_member)					\
	for (AssertVariableIsOfTypeMacro(iter, proclist_mutable_iter),			\
		 AssertVariableIsOfTypeMacro(lhead, proclist_head *),				\
		 (iter).cur = (lhead)->head,										\
		 (iter).next = (iter).cur == INVALID_PROC_NUMBER ? INVALID_PROC_NUMBER :	\
			 proclist_node_get((iter).cur,									\
							   offsetof(PGPROC, link_member))->next;		\
		 (iter).cur != INVALID_PROC_NUMBER;									\
		 (iter).cur = (iter).next,											\
		 (iter).next = (iter).cur == INVALID_PROC_NUMBER ? INVALID_PROC_NUMBER :	\
			 proclist_node_get((iter).cur,									\
							   offsetof(PGPROC, link_member))->next)

#endif							/* PROCLIST_H */
