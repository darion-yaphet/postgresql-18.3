/*-------------------------------------------------------------------------
 *
 * cmptype.h
 *	  POSTGRES compare type definitions.
 *
 * 定义比较结果所使用的枚举值，并与 B-tree 策略编号对应。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/cmptype.h
 *
  * src/include/access/cmptype.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CMPTYPE_H
#define CMPTYPE_H

/*
 * CompareType - fundamental semantics of certain operators
 *
  * CompareType - 某些运算符的基本语义
 *
 * These enum symbols represent the fundamental semantics of certain operators
 * that the system needs to have some hardcoded knowledge about.  (For
 * example, RowCompareExpr needs to know which operators can be determined to
 * act like =, <>, <, etc.)  Index access methods map (some of) strategy
 * numbers to these values so that the system can know about the meaning of
 * (some of) the operators without needing hardcoded knowledge of index AM's
 * strategy numbering.
 *
  * 这些枚举符号代表系统需要具有一些硬编码知识的某些运算符的基本语义。  （例如，RowCompareExpr 需要知道哪些运算符可以确定为 =、<>、< 等）。索引访问方法将（某些）策略编号映射到这些值，以便系统可以了解（某些）运算符的含义，而无需硬编码索引 AM 策略编号的知识。
 *
 * XXX Currently, this mapping is not fully developed and most values are
 * chosen to match btree strategy numbers, which is not going to work very
 * well for other access methods.
 *
  * XXX 目前，此映射尚未完全开发，并且选择大多数值来匹配 btree 策略编号，这对于其他访问方法来说效果不佳。
 */
typedef enum CompareType
{
	COMPARE_INVALID = 0,
	COMPARE_LT = 1,				/* BTLessStrategyNumber */

	/* B-tree 小于策略编号。 */
	COMPARE_LE = 2,				/* BTLessEqualStrategyNumber */

	/* B-tree 小于等于策略编号。 */
	COMPARE_EQ = 3,				/* BTEqualStrategyNumber */

	/* B-tree 等于策略编号。 */
	COMPARE_GE = 4,				/* BTGreaterEqualStrategyNumber */

	/* B-tree 大于等于策略编号。 */
	COMPARE_GT = 5,				/* BTGreaterStrategyNumber */

	/* B-tree 大于策略编号。 */
	COMPARE_NE = 6,				/* no such btree strategy */

	/* 不存在这样的 B-tree 策略。 */
	COMPARE_OVERLAP,
	COMPARE_CONTAINED_BY,
} CompareType;

#endif							/* CMPTYPE_H */
