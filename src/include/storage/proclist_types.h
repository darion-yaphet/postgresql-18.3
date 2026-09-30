/*-------------------------------------------------------------------------
 *
 * proclist_types.h
 *		doubly-linked lists of pgprocnos
 *
 * See proclist.h for functions that operate on these types.
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/storage/proclist_types.h
 *-------------------------------------------------------------------------
 */

#ifndef PROCLIST_TYPES_H
#define PROCLIST_TYPES_H

#include "storage/procnumber.h"

/*
 * A node in a doubly-linked list of processes.  The link fields contain
 * the 0-based PGPROC indexes of the next and previous process, or
 * INVALID_PROC_NUMBER in the next-link of the last node and the prev-link
 * of the first node.  A node that is currently not in any list
 * should have next == prev == 0; this is not a possible state for a node
 * that is in a list, because we disallow circularity.
 */

/*
 * 进程双向链表中的节点。链接字段保存下一个和上一个进程的从 0 开始的 PGPROC 索引；
 * 最后一个节点的 next 链接和第一个节点的 prev 链接为 INVALID_PROC_NUMBER。当前不在
 * 任何链表中的节点应满足 next == prev == 0；由于禁止环形链表，这不是已在链表中的节点
 * 可能具有的状态。
 */
typedef struct proclist_node
{
	ProcNumber	next;			/* pgprocno of the next PGPROC */

	/* 下一个 PGPROC 的 pgprocno。 */
	ProcNumber	prev;			/* pgprocno of the prev PGPROC */

	/* 上一个 PGPROC 的 pgprocno。 */
} proclist_node;

/*
 * Header of a doubly-linked list of PGPROCs, identified by pgprocno.
 * An empty list is represented by head == tail == INVALID_PROC_NUMBER.
 */

/*
 * 由 pgprocno 标识的 PGPROC 双向链表头。空链表表示为
 * head == tail == INVALID_PROC_NUMBER。
 */
typedef struct proclist_head
{
	ProcNumber	head;			/* pgprocno of the head PGPROC */

	/* 头部 PGPROC 的 pgprocno。 */
	ProcNumber	tail;			/* pgprocno of the tail PGPROC */

	/* 尾部 PGPROC 的 pgprocno。 */
} proclist_head;

/*
 * List iterator allowing some modifications while iterating.
 */

/*
 * 允许在迭代期间进行某些修改的链表迭代器。
 */
typedef struct proclist_mutable_iter
{
	ProcNumber	cur;			/* pgprocno of the current PGPROC */

	/* 当前 PGPROC 的 pgprocno。 */
	ProcNumber	next;			/* pgprocno of the next PGPROC */

	/* 下一个 PGPROC 的 pgprocno。 */
} proclist_mutable_iter;

#endif							/* PROCLIST_TYPES_H */
