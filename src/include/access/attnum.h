/*-------------------------------------------------------------------------
 *
 * attnum.h
 *	  POSTGRES attribute number definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/attnum.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * attnum.h POSTGRES 属性编号定义。 src/incl
 * ude/access/attnum.h
 */
#ifndef ATTNUM_H
#define ATTNUM_H


/*
 * user defined attribute numbers start at 1.   -ay 2/95
 *
 * 中文翻译：
 * 用户定义的属性编号从 1 开始。-ay 2/95
 */
typedef int16 AttrNumber;

#define InvalidAttrNumber		0
#define MaxAttrNumber			32767

/* ----------------
 *		support macros
 * ----------------
 *
 * 中文翻译：
 * 支持宏
 */
/*
 * AttributeNumberIsValid
 *		True iff the attribute number is valid.
 *
 * 中文翻译：
 * AttributeNumberIsValid 当且仅当属性编号有效时
 * 为 True。
 */
#define AttributeNumberIsValid(attributeNumber) \
	((bool) ((attributeNumber) != InvalidAttrNumber))

/*
 * AttrNumberIsForUserDefinedAttr
 *		True iff the attribute number corresponds to a user defined attribute.
 *
 * 中文翻译：
 * AttrNumberIsForUserDefinedAttr 当且仅
 * 当属性编号对应于用户定义的属性时为 True。
 */
#define AttrNumberIsForUserDefinedAttr(attributeNumber) \
	((bool) ((attributeNumber) > 0))

/*
 * AttrNumberGetAttrOffset
 *		Returns the attribute offset for an attribute number.
 *
 * Note:
 *		Assumes the attribute number is for a user defined attribute.
 *
 * 中文翻译：
 * AttrNumberGetAttrOffset 返回属性编号的属性偏
 * 移量。注意：假设属性编号用于用户定义的属性。
 */
#define AttrNumberGetAttrOffset(attNum) \
( \
	AssertMacro(AttrNumberIsForUserDefinedAttr(attNum)), \
	((attNum) - 1) \
)

/*
 * AttrOffsetGetAttrNumber
 *		Returns the attribute number for an attribute offset.
 *
 * 中文翻译：
 * AttrOffsetGetAttrNumber 返回属性偏移量的属性
 * 编号。
 */
#define AttrOffsetGetAttrNumber(attributeOffset) \
	 ((AttrNumber) (1 + (attributeOffset)))

#endif							/* ATTNUM_H */

/* 中文翻译：ATTNUM_H */
