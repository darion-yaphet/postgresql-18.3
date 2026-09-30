/*-------------------------------------------------------------------------
 *
 * itemid.h
 *	  Standard POSTGRES buffer page item identifier/line pointer definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/itemid.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * PostgreSQL 标准缓冲页项标识符和行指针定义。
 */
#ifndef ITEMID_H
#define ITEMID_H

/*
 * A line pointer on a buffer page.  See buffer page definitions and comments
 * for an explanation of how line pointers are used.
 *
 * In some cases a line pointer is "in use" but does not have any associated
 * storage on the page.  By convention, lp_len == 0 in every line pointer
 * that does not have storage, independently of its lp_flags state.
 */

/*
 * 缓冲页上的行指针。有关行指针用途的说明，请参见缓冲页定义及其注释。
 * 在某些情况下，行指针“正在使用”，但在页面上没有关联存储。按约定，不具有存储的每个行指针
 * 的 lp_len 都为 0，与其 lp_flags 状态无关。
 */
typedef struct ItemIdData
{
	unsigned	lp_off:15,		/* offset to tuple (from start of page) */

/*
 * 到元组的偏移量（从页首开始）。
 */
				lp_flags:2,		/* state of line pointer, see below */

/*
 * 行指针状态，见下文。
 */
				lp_len:15;		/* byte length of tuple */

/*
 * 元组的字节长度。
 */
} ItemIdData;

typedef ItemIdData *ItemId;

/*
 * lp_flags has these possible states.  An UNUSED line pointer is available
 * for immediate re-use, the other states are not.
 */

/*
 * lp_flags 可以取以下状态。UNUSED 行指针可立即复用，其他状态不可。
 */
#define LP_UNUSED		0		/* unused (should always have lp_len=0) */

/*
 * 未使用（lp_len 应始终为 0）。
 */
#define LP_NORMAL		1		/* used (should always have lp_len>0) */

/*
 * 已使用（lp_len 应始终大于 0）。
 */
#define LP_REDIRECT		2		/* HOT redirect (should have lp_len=0) */

/*
 * HOT 重定向（lp_len 应为 0）。
 */
#define LP_DEAD			3		/* dead, may or may not have storage */

/*
 * 已死亡，可能有存储也可能没有。
 */

/*
 * Item offsets and lengths are represented by these types when
 * they're not actually stored in an ItemIdData.
 */

/*
 * 当项偏移量和长度并不实际存储在 ItemIdData 中时，使用这些类型表示它们。
 */
typedef uint16 ItemOffset;
typedef uint16 ItemLength;


/* ----------------
 *		support macros
 * ----------------
 */

/*
 * 支持宏。
 */

/*
 *		ItemIdGetLength
 */

/*
 * 获取项长度。
 */
#define ItemIdGetLength(itemId) \
   ((itemId)->lp_len)

/*
 *		ItemIdGetOffset
 */

/*
 * 获取项偏移量。
 */
#define ItemIdGetOffset(itemId) \
   ((itemId)->lp_off)

/*
 *		ItemIdGetFlags
 */

/*
 * 获取项标志。
 */
#define ItemIdGetFlags(itemId) \
   ((itemId)->lp_flags)

/*
 *		ItemIdGetRedirect
 * In a REDIRECT pointer, lp_off holds offset number for next line pointer
 */

/*
 * 获取重定向目标。在 REDIRECT 指针中，lp_off 保存下一个行指针的偏移量编号。
 */
#define ItemIdGetRedirect(itemId) \
   ((itemId)->lp_off)

/*
 * ItemIdIsValid
 *		True iff item identifier is valid.
 *		This is a pretty weak test, probably useful only in Asserts.
 */

/*
 * 当且仅当项标识符有效时为真。这是相当弱的检查，可能只适用于断言。
 */
#define ItemIdIsValid(itemId)	PointerIsValid(itemId)

/*
 * ItemIdIsUsed
 *		True iff item identifier is in use.
 */

/*
 * 当且仅当项标识符正在使用时为真。
 */
#define ItemIdIsUsed(itemId) \
	((itemId)->lp_flags != LP_UNUSED)

/*
 * ItemIdIsNormal
 *		True iff item identifier is in state NORMAL.
 */

/*
 * 当且仅当项标识符处于 NORMAL 状态时为真。
 */
#define ItemIdIsNormal(itemId) \
	((itemId)->lp_flags == LP_NORMAL)

/*
 * ItemIdIsRedirected
 *		True iff item identifier is in state REDIRECT.
 */

/*
 * 当且仅当项标识符处于 REDIRECT 状态时为真。
 */
#define ItemIdIsRedirected(itemId) \
	((itemId)->lp_flags == LP_REDIRECT)

/*
 * ItemIdIsDead
 *		True iff item identifier is in state DEAD.
 */

/*
 * 当且仅当项标识符处于 DEAD 状态时为真。
 */
#define ItemIdIsDead(itemId) \
	((itemId)->lp_flags == LP_DEAD)

/*
 * ItemIdHasStorage
 *		True iff item identifier has associated storage.
 */

/*
 * 当且仅当项标识符具有关联存储时为真。
 */
#define ItemIdHasStorage(itemId) \
	((itemId)->lp_len != 0)

/*
 * ItemIdSetUnused
 *		Set the item identifier to be UNUSED, with no storage.
 *		Beware of multiple evaluations of itemId!
 */

/*
 * 将项标识符设为无存储的 UNUSED。请注意 itemId 会被多次求值。
 */
#define ItemIdSetUnused(itemId) \
( \
	(itemId)->lp_flags = LP_UNUSED, \
	(itemId)->lp_off = 0, \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdSetNormal
 *		Set the item identifier to be NORMAL, with the specified storage.
 *		Beware of multiple evaluations of itemId!
 */

/*
 * 将项标识符设为具有指定存储的 NORMAL。请注意 itemId 会被多次求值。
 */
#define ItemIdSetNormal(itemId, off, len) \
( \
	(itemId)->lp_flags = LP_NORMAL, \
	(itemId)->lp_off = (off), \
	(itemId)->lp_len = (len) \
)

/*
 * ItemIdSetRedirect
 *		Set the item identifier to be REDIRECT, with the specified link.
 *		Beware of multiple evaluations of itemId!
 */

/*
 * 将项标识符设为具有指定链接的 REDIRECT。请注意 itemId 会被多次求值。
 */
#define ItemIdSetRedirect(itemId, link) \
( \
	(itemId)->lp_flags = LP_REDIRECT, \
	(itemId)->lp_off = (link), \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdSetDead
 *		Set the item identifier to be DEAD, with no storage.
 *		Beware of multiple evaluations of itemId!
 */

/*
 * 将项标识符设为无存储的 DEAD。请注意 itemId 会被多次求值。
 */
#define ItemIdSetDead(itemId) \
( \
	(itemId)->lp_flags = LP_DEAD, \
	(itemId)->lp_off = 0, \
	(itemId)->lp_len = 0 \
)

/*
 * ItemIdMarkDead
 *		Set the item identifier to be DEAD, keeping its existing storage.
 *
 * Note: in indexes, this is used as if it were a hint-bit mechanism;
 * we trust that multiple processors can do this in parallel and get
 * the same result.
 */

/*
 * 将项标识符设为 DEAD，并保留已有存储。
 * 注意：在索引中，它被当作提示位机制使用；我们相信多个处理器可并行执行此操作并得到相同结果。
 */
#define ItemIdMarkDead(itemId) \
( \
	(itemId)->lp_flags = LP_DEAD \
)

#endif							/* ITEMID_H */
