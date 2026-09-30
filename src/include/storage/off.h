/*-------------------------------------------------------------------------
 *
 * off.h
 *	  POSTGRES disk "offset" definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/off.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OFF_H
#define OFF_H

#include "storage/itemid.h"
/*
 * OffsetNumber:
 *
 * this is a 1-based index into the linp (ItemIdData) array in the
 * header of each disk page.
 */

/*
 * OffsetNumber：
 *
 * 这是每个磁盘页面头部 linp（ItemIdData）数组中从 1 开始的索引。
 */
typedef uint16 OffsetNumber;

#define InvalidOffsetNumber		((OffsetNumber) 0)
#define FirstOffsetNumber		((OffsetNumber) 1)
#define MaxOffsetNumber			((OffsetNumber) (BLCKSZ / sizeof(ItemIdData)))

/* ----------------
 *		support macros
 * ----------------
 */

/* ----------------
 *		支持宏
 * ----------------
 */

/*
 * OffsetNumberIsValid
 *		True iff the offset number is valid.
 */

/*
 * OffsetNumberIsValid
 *		当且仅当偏移编号有效时为真。
 */
#define OffsetNumberIsValid(offsetNumber) \
	((bool) ((offsetNumber != InvalidOffsetNumber) && \
			 (offsetNumber <= MaxOffsetNumber)))

/*
 * OffsetNumberNext
 * OffsetNumberPrev
 *		Increments/decrements the argument.  These macros look pointless
 *		but they help us disambiguate the different manipulations on
 *		OffsetNumbers (e.g., sometimes we subtract one from an
 *		OffsetNumber to move back, and sometimes we do so to form a
 *		real C array index).
 */

/*
 * OffsetNumberNext
 * OffsetNumberPrev
 *		递增/递减参数。这些宏看似没有必要，但可帮助我们区分对 OffsetNumber 的不同
 *		操作（例如，有时从 OffsetNumber 减一是为了向后移动，有时则是为了形成真正的
 *		C 数组索引）。
 */
#define OffsetNumberNext(offsetNumber) \
	((OffsetNumber) (1 + (offsetNumber)))
#define OffsetNumberPrev(offsetNumber) \
	((OffsetNumber) (-1 + (offsetNumber)))

#endif							/* OFF_H */
