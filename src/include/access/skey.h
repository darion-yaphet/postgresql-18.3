/*-------------------------------------------------------------------------
 *
 * skey.h
 *	  POSTGRES scan key definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/skey.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * skey.h POSTGRES 扫描键定义。 src/include
 * /access/skey.h
 */
#ifndef SKEY_H
#define SKEY_H

#include "access/attnum.h"
#include "access/stratnum.h"
#include "fmgr.h"


/*
 * A ScanKey represents the application of a comparison operator between
 * a table or index column and a constant.  When it's part of an array of
 * ScanKeys, the comparison conditions are implicitly ANDed.  The index
 * column is the left argument of the operator, if it's a binary operator.
 * (The data structure can support unary indexable operators too; in that
 * case sk_argument would go unused.  This is not currently implemented.)
 *
 * For an index scan, sk_strategy and sk_subtype must be set correctly for
 * the operator.  When using a ScanKey in a heap scan, these fields are not
 * used and may be set to InvalidStrategy/InvalidOid.
 *
 * If the operator is collation-sensitive, sk_collation must be set
 * correctly as well.
 *
 * A ScanKey can also represent a ScalarArrayOpExpr, that is a condition
 * "column op ANY(ARRAY[...])".  This is signaled by the SK_SEARCHARRAY
 * flag bit.  The sk_argument is not a value of the operator's right-hand
 * argument type, but rather an array of such values, and the per-element
 * comparisons are to be ORed together.
 *
 * A ScanKey can also represent a condition "column IS NULL" or "column
 * IS NOT NULL"; these cases are signaled by the SK_SEARCHNULL and
 * SK_SEARCHNOTNULL flag bits respectively.  The argument is always NULL,
 * and the sk_strategy, sk_subtype, sk_collation, and sk_func fields are
 * not used (unless set by the index AM).
 *
 * SK_SEARCHARRAY, SK_SEARCHNULL and SK_SEARCHNOTNULL are supported only
 * for index scans, not heap scans; and not all index AMs support them,
 * only those that set amsearcharray or amsearchnulls respectively.
 *
 * A ScanKey can also represent an ordering operator invocation, that is
 * an ordering requirement "ORDER BY indexedcol op constant".  This looks
 * the same as a comparison operator, except that the operator doesn't
 * (usually) yield boolean.  We mark such ScanKeys with SK_ORDER_BY.
 * SK_SEARCHARRAY, SK_SEARCHNULL, SK_SEARCHNOTNULL cannot be used here.
 *
 * Note: in some places, ScanKeys are used as a convenient representation
 * for the invocation of an access method support procedure.  In this case
 * sk_strategy/sk_subtype are not meaningful (but sk_collation can be); and
 * sk_func may refer to a function that returns something other than boolean.
 *
 * 中文翻译：
 * ScanKey 表示表或索引列与常量之间比较运算符的应用。当它是 S
 * canKeys 数组的一部分时，比较条件会隐式进行 AND 运算。如
 * 果是二元运算符，则索引列是运算符的左参数。 （数据结构也可以支持一元
 * 可索引运算符；在这种情况下，sk_argument 将不被使用。当前
 * 尚未实现。）对于索引扫描，必须为运算符正确设置 sk_strateg
 * y 和 sk_subtype。在堆扫描中使用 ScanKey 时，不
 * 会使用这些字段，并且可能会设置为 InvalidStrategy/I
 * nvalidOid。如果运算符对排序规则敏感，则还必须正确设置 sk
 * _collat​​ion。 ScanKey 还可以表示 Scalar
 * ArrayOpExpr，即条件“column op ANY(ARRA
 * Y[...])”。这是由 SK_SEARCHARRAY 标志位指示的
 * 。 sk_argument 不是运算符右侧参数类型的值，而是此类值的
 * 数组，并且每个元素的比较将进行 OR 运算。 ScanKey 还可以
 * 表示“列 IS NULL”或“列 IS NOT NULL”条件；这些
 * 情况分别由 SK_SEARCHNULL 和 SK_SEARCHNOT
 * NULL 标志位表示。该参数始终为 NULL，并且不使用 sk_st
 * rategy、sk_subtype、sk_collat​​ion 和
 *  sk_func 字段（除非由索引 AM 设置）。 SK_SEARC
 * HARRAY、SK_SEARCHNULL 和 SK_SEARCHNO
 * TNULL 仅支持索引扫描，不支持堆扫描；并非所有索引 AM 都支持
 * 它们，只有那些分别设置 amsearcharray 或 amsear
 * chnulls 的索引 AM 才支持它们。 ScanKey 还可以表
 * 示排序运算符调用，即排序要求“ORDER BY indexedcol
 *  op Constant”。这看起来与比较运算符相同，只是该运算符（
 * 通常）不产生布尔值。我们用 SK_ORDER_BY 标记此类 Sca
 * nKey。此处不能使用 SK_SEARCHARRAY、SK_SEAR
 * CHNULL、SK_SEARCHNOTNULL。注意：在某些地方，S
 * canKeys 被用作调用访问方法支持过程的方便表示。在这种情况下
 * sk_strategy/sk_subtype 没有意义（但 sk_c
 * ollat​​ion 可以）； sk_func 可能指返回布尔值以外
 * 的值的函数。
 */
typedef struct ScanKeyData
{
	int			sk_flags;		/* flags, see below */

	/* 中文翻译：标志，见下文 */
	AttrNumber	sk_attno;		/* table or index column number */

	/* 中文翻译：表或索引列号 */
	StrategyNumber sk_strategy; /* operator strategy number */

	/* 中文翻译：运营商策略号 */
	Oid			sk_subtype;		/* strategy subtype */

	/* 中文翻译：策略子类型 */
	Oid			sk_collation;	/* collation to use, if needed */

	/* 中文翻译：如果需要的话要使用的排序规则 */
	FmgrInfo	sk_func;		/* lookup info for function to call */

	/* 中文翻译：查找要调用的函数的信息 */
	Datum		sk_argument;	/* data to compare */

	/* 中文翻译：要比较的数据 */
} ScanKeyData;

typedef ScanKeyData *ScanKey;

/*
 * About row comparisons:
 *
 * The ScanKey data structure also supports row comparisons, that is ordered
 * tuple comparisons like (x, y) > (c1, c2), having the SQL-spec semantics
 * "x > c1 OR (x = c1 AND y > c2)".  Note that this is currently only
 * implemented for btree index searches, not for heapscans or any other index
 * type.  A row comparison is represented by a "header" ScanKey entry plus
 * a separate array of ScanKeys, one for each column of the row comparison.
 * The header entry has these properties:
 *		sk_flags = SK_ROW_HEADER
 *		sk_attno = index column number for leading column of row comparison
 *		sk_strategy = btree strategy code for semantics of row comparison
 *				(ie, < <= > or >=)
 *		sk_subtype, sk_collation, sk_func: not used
 *		sk_argument: pointer to subsidiary ScanKey array
 * If the header is part of a ScanKey array that's sorted by attno, it
 * must be sorted according to the leading column number.
 *
 * The subsidiary ScanKey array appears in logical column order of the row
 * comparison, which may be different from index column order.  The array
 * elements are like a normal ScanKey array except that:
 *		sk_flags must include SK_ROW_MEMBER, plus SK_ROW_END in the last
 *				element (needed since row header does not include a count)
 *		sk_func points to the btree comparison support function for the
 *				opclass, NOT the operator's implementation function.
 * sk_strategy must be the same in all elements of the subsidiary array,
 * that is, the same as in the header entry.
 * SK_SEARCHARRAY, SK_SEARCHNULL, SK_SEARCHNOTNULL cannot be used here.
 *
 * 中文翻译：
 * 关于行比较：ScanKey 数据结构还支持行比较，即有序元组比较，如
 *  (x, y) > (c1, c2)，具有 SQL 规范语义“x >
 *  c1 OR (x = c1 AND y > c2)”。请注意，这目
 * 前仅针对 btree 索引搜索实现，不适用于堆扫描或任何其他索引类型
 * 。行比较由“标题”ScanKey 条目加上单独的 ScanKey 数
 * 组表示，行比较的每一列都有一个 ScanKey。标头条目具有以下属性
 * ： sk_flags = SK_ROW_HEADER sk_attn
 * o = 行比较前导列的索引列号 sk_strategy = 用于行比
 * 较语义的 btree 策略代码（即 < <= > 或 >=） sk_
 * subtype、sk_collation、sk_func：未使用 s
 * k_argument：指向辅助 ScanKey 数组的指针 如果标头
 * 是按 attno 排序的 ScanKey 数组的一部分，它必须根据前
 * 导列号排序。辅助 ScanKey 数组按行比较的逻辑列顺序出现，可能
 * 与索引列顺序不同。数组元素与普通的 ScanKey 数组类似，不同之
 * 处在于： sk_flags 必须包含 SK_ROW_MEMBER，并
 * 在最后一个元素中加上 SK_ROW_END（因为行头不包含计数而需要
 * ） sk_func 指向 opclass 的 btree 比较支持函
 * 数，而不是运算符的实现函数。辅助数组的所有元素中的 sk_strat
 * egy 必须相同，即与标头条目中的相同。此处不能使用 SK_SEAR
 * CHARRAY、SK_SEARCHNULL、SK_SEARCHNOT
 * NULL。
 */

/*
 * ScanKeyData sk_flags
 *
 * sk_flags bits 0-15 are reserved for system-wide use (symbols for those
 * bits should be defined here).  Bits 16-31 are reserved for use within
 * individual index access methods.
 *
 * 中文翻译：
 * ScanKeyData sk_flags sk_flags 位 0-
 * 15 保留供系统范围使用（这些位的符号应在此处定义）。位 16-31
 *  保留用于单独的索引访问方法。
 */
#define SK_ISNULL			0x0001	/* sk_argument is NULL */

/* 中文翻译：sk_argument 为 NULL */
#define SK_UNARY			0x0002	/* unary operator (not supported!) */

/* 中文翻译：一元运算符（不支持！） */
#define SK_ROW_HEADER		0x0004	/* row comparison header (see above) */

/* 中文翻译：行比较标题（见上文） */
#define SK_ROW_MEMBER		0x0008	/* row comparison member (see above) */

/* 中文翻译：行比较成员（见上文） */
#define SK_ROW_END			0x0010	/* last row comparison member */

/* 中文翻译：最后一行比较成员 */
#define SK_SEARCHARRAY		0x0020	/* scankey represents ScalarArrayOp */

/* 中文翻译：scankey代表ScalarArrayOp */
#define SK_SEARCHNULL		0x0040	/* scankey represents "col IS NULL" */

/* 中文翻译：scankey 代表“col IS NULL” */
#define SK_SEARCHNOTNULL	0x0080	/* scankey represents "col IS NOT NULL" */

/* 中文翻译：scankey 代表“col IS NOT NULL” */
#define SK_ORDER_BY			0x0100	/* scankey is for ORDER BY op */

/* 中文翻译：scankey 用于 ORDER BY 操作 */


/*
 * prototypes for functions in access/common/scankey.c
 *
 * 中文翻译：
 * access/common/scankey.c 中函数的原型
 */
/*
 * Function ScanKeyInit initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 ScanKeyInit通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void ScanKeyInit(ScanKey entry,
						AttrNumber attributeNumber,
						StrategyNumber strategy,
						RegProcedure procedure,
						Datum argument);
/*
 * Function ScanKeyEntryInitialize initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 ScanKeyEntryInitialize通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void ScanKeyEntryInitialize(ScanKey entry,
								   int flags,
								   AttrNumber attributeNumber,
								   StrategyNumber strategy,
								   Oid subtype,
								   Oid collation,
								   RegProcedure procedure,
								   Datum argument);
/*
 * Function ScanKeyEntryInitializeWithInfo initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 ScanKeyEntryInitializeWithInfo通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void ScanKeyEntryInitializeWithInfo(ScanKey entry,
										   int flags,
										   AttrNumber attributeNumber,
										   StrategyNumber strategy,
										   Oid subtype,
										   Oid collation,
										   FmgrInfo *finfo,
										   Datum argument);

#endif							/* SKEY_H */

/* 中文翻译：SKEY_H */
