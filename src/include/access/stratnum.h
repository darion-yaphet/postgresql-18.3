/*-------------------------------------------------------------------------
 *
 * stratnum.h
 *	  POSTGRES strategy number definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/stratnum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRATNUM_H
#define STRATNUM_H

/*
 * Strategy numbers identify the semantics that particular operators have
 * with respect to particular operator classes.  In some cases a strategy
 * subtype (an OID) is used as further information.
 */

/*
 * 策略号（Strategy number）用于标识特定运算符相对于特定运算符类所具有的语义。
 * 在某些情况下，会使用一个策略子类型（一个 OID）作为进一步的信息。
 */
typedef uint16 StrategyNumber;

#define InvalidStrategy ((StrategyNumber) 0)

/*
 * Strategy numbers for B-tree indexes.
 */

/*
 * B-tree 索引使用的策略号。
 */
#define BTLessStrategyNumber			1
#define BTLessEqualStrategyNumber		2
#define BTEqualStrategyNumber			3
#define BTGreaterEqualStrategyNumber	4
#define BTGreaterStrategyNumber			5

#define BTMaxStrategyNumber				5

/*
 *	Strategy numbers for hash indexes. There's only one valid strategy for
 *	hashing: equality.
 */

/*
 * 哈希（hash）索引使用的策略号。哈希只有一种有效的策略：相等（equality）。
 */
#define HTEqualStrategyNumber			1

#define HTMaxStrategyNumber				1

/*
 * Strategy numbers common to (some) GiST, SP-GiST and BRIN opclasses.
 *
 * The first few of these come from the R-Tree indexing method (hence the
 * names); the others have been added over time as they have been needed.
 */

/*
 * （部分）GiST、SP-GiST 和 BRIN 运算符类共用的策略号。
 *
 * 其中前几个来自 R-Tree 索引方法（因此有这些命名）；其余的则是随着时间推移在需要时逐步添加的。
 */
#define RTLeftStrategyNumber			1	/* for << */

/* 用于 <<（严格在左侧） */
#define RTOverLeftStrategyNumber		2	/* for &< */

/* 用于 &<（不超出右边界） */
#define RTOverlapStrategyNumber			3	/* for && */

/* 用于 &&（重叠） */
#define RTOverRightStrategyNumber		4	/* for &> */

/* 用于 &>（不超出左边界） */
#define RTRightStrategyNumber			5	/* for >> */

/* 用于 >>（严格在右侧） */
#define RTSameStrategyNumber			6	/* for ~= */

/* 用于 ~=（相同） */
#define RTContainsStrategyNumber		7	/* for @> */

/* 用于 @>（包含） */
#define RTContainedByStrategyNumber		8	/* for <@ */

/* 用于 <@（被包含于） */
#define RTOverBelowStrategyNumber		9	/* for &<| */

/* 用于 &<|（不超出上边界） */
#define RTBelowStrategyNumber			10	/* for <<| */

/* 用于 <<|（严格在下方） */
#define RTAboveStrategyNumber			11	/* for |>> */

/* 用于 |>>（严格在上方） */
#define RTOverAboveStrategyNumber		12	/* for |&> */

/* 用于 |&>（不超出下边界） */
#define RTOldContainsStrategyNumber		13	/* for old spelling of @> */

/* 用于 @> 的旧写法（包含） */
#define RTOldContainedByStrategyNumber	14	/* for old spelling of <@ */

/* 用于 <@ 的旧写法（被包含于） */
#define RTKNNSearchStrategyNumber		15	/* for <-> (distance) */

/* 用于 <->（距离，最近邻搜索） */
#define RTContainsElemStrategyNumber	16	/* for range types @> elem */

/* 用于范围类型 @> elem（范围包含元素） */
#define RTAdjacentStrategyNumber		17	/* for -|- */

/* 用于 -|-（相邻） */
#define RTEqualStrategyNumber			18	/* for = */

/* 用于 =（等于） */
#define RTNotEqualStrategyNumber		19	/* for != */

/* 用于 !=（不等于） */
#define RTLessStrategyNumber			20	/* for < */

/* 用于 <（小于） */
#define RTLessEqualStrategyNumber		21	/* for <= */

/* 用于 <=（小于等于） */
#define RTGreaterStrategyNumber			22	/* for > */

/* 用于 >（大于） */
#define RTGreaterEqualStrategyNumber	23	/* for >= */

/* 用于 >=（大于等于） */
#define RTSubStrategyNumber				24	/* for inet >> */

/* 用于 inet >>（子网包含） */
#define RTSubEqualStrategyNumber		25	/* for inet <<= */

/* 用于 inet <<=（子网被包含或相等） */
#define RTSuperStrategyNumber			26	/* for inet << */

/* 用于 inet <<（超网被包含） */
#define RTSuperEqualStrategyNumber		27	/* for inet >>= */

/* 用于 inet >>=（超网包含或相等） */
#define RTPrefixStrategyNumber			28	/* for text ^@ */

/* 用于 text ^@（前缀匹配） */
#define RTOldBelowStrategyNumber		29	/* for old spelling of <<| */

/* 用于 <<| 的旧写法（严格在下方） */
#define RTOldAboveStrategyNumber		30	/* for old spelling of |>> */

/* 用于 |>> 的旧写法（严格在上方） */

#define RTMaxStrategyNumber				30


#endif							/* STRATNUM_H */
