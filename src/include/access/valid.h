/*-------------------------------------------------------------------------
 *
 * valid.h
 *	  POSTGRES tuple qualification validity definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/valid.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * valid.h POSTGRES 元组限定有效性定义。 src/in
 * clude/access/valid.h
 */
#ifndef VALID_H
#define VALID_H

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/tupdesc.h"

/*
 *		HeapKeyTest
 *
 *		Test a heap tuple to see if it satisfies a scan key.
 *
 * 中文翻译：
 * HeapKeyTest 测试堆元组以查看它是否满足扫描键。
 */
/*
 * Function HeapKeyTest carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapKeyTest通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline bool
HeapKeyTest(HeapTuple tuple, TupleDesc tupdesc, int nkeys, ScanKey keys)
{
	int			cur_nkeys = nkeys;
	ScanKey		cur_key = keys;

	for (; cur_nkeys--; cur_key++)
	{
		Datum		atp;
		bool		isnull;
		Datum		test;

		if (cur_key->sk_flags & SK_ISNULL)
			return false;

		atp = heap_getattr(tuple, cur_key->sk_attno, tupdesc, &isnull);

		if (isnull)
			return false;

		test = FunctionCall2Coll(&cur_key->sk_func,
								 cur_key->sk_collation,
								 atp, cur_key->sk_argument);

		if (!DatumGetBool(test))
			return false;
	}

	return true;
}

#endif							/* VALID_H */

/* 中文翻译：VALID_H */
