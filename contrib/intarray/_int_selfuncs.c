/*-------------------------------------------------------------------------
 *
 * _int_selfuncs.c
 *	  Functions for selectivity estimation of intarray operators
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  contrib/intarray/_int_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "_int.h"
#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

PG_FUNCTION_INFO_V1(_int_overlap_sel);
PG_FUNCTION_INFO_V1(_int_contains_sel);
PG_FUNCTION_INFO_V1(_int_contained_sel);
PG_FUNCTION_INFO_V1(_int_overlap_joinsel);
PG_FUNCTION_INFO_V1(_int_contains_joinsel);
PG_FUNCTION_INFO_V1(_int_contained_joinsel);
PG_FUNCTION_INFO_V1(_int_matchsel);


static Selectivity int_query_opr_selec(ITEM *item, Datum *mcelems, float4 *mcefreqs,
									   int nmcelems, float4 minfreq);
static int	compare_val_int4(const void *a, const void *b);

/*
 * Wrappers around the default array selectivity estimation functions.
 *
 * 默认数组选择性估计函数的包装。
 *
 * The default array selectivity operators for the @>, && and @< operators
 * work fine for integer arrays. However, if we tried to just use arraycontsel
 * and arraycontjoinsel directly as the cost estimator functions for our
 * operators, they would not work as intended, because they look at the
 * operator's OID. Our operators behave exactly like the built-in anyarray
 * versions, but we must tell the cost estimator functions which built-in
 * operators they correspond to. These wrappers just replace the operator
 * OID with the corresponding built-in operator's OID, and call the built-in
 * function.
 *
 * @>、&& 和 @< 运算符的默认数组选择性运算符适用于整数数组。但是，如果我们尝试直接使用 arraycontsel 和 arraycontjoinsel 作为运算符的成本估算器函数，它们将无法按预期工作，因为它们会查看运算符的 OID。我们的运算符的行为与内置的anyarray 版本完全相同，但我们必须告诉成本估计器函数它们对应于哪些内置运算符。这些包装器只是将运算符 OID 替换为相应的内置运算符的 OID，并调用内置函数。
 */

Datum
_int_overlap_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(arraycontsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_OVERLAP_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3)));
}

Datum
_int_contains_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(arraycontsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINS_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3)));
}

Datum
_int_contained_sel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall4(arraycontsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINED_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3)));
}

Datum
_int_overlap_joinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall5(arraycontjoinsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_OVERLAP_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3),
										PG_GETARG_DATUM(4)));
}

Datum
_int_contains_joinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall5(arraycontjoinsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINS_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3),
										PG_GETARG_DATUM(4)));
}

Datum
_int_contained_joinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall5(arraycontjoinsel,
										PG_GETARG_DATUM(0),
										ObjectIdGetDatum(OID_ARRAY_CONTAINED_OP),
										PG_GETARG_DATUM(2),
										PG_GETARG_DATUM(3),
										PG_GETARG_DATUM(4)));
}


/*
 * _int_matchsel -- restriction selectivity function for intarray @@ query_int
 *
 * _int_matchsel -- intarray @@ query_int 的限制选择性函数
 */
Datum
_int_matchsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);

	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	QUERYTYPE  *query;
	Datum	   *mcelems = NULL;
	float4	   *mcefreqs = NULL;
	int			nmcelems = 0;
	float4		minfreq = 0.0;
	float4		nullfrac = 0.0;
	AttStatsSlot sslot;

	/*
	 * If expression is not "variable @@ something" or "something @@ variable"
	 * then punt and return a default estimate.
	 *
	 * 如果表达式不是“变量@@某事”或“某事@@变量”，则平铺并返回默认估计。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);

	/*
	 * Variable should be int[]. We don't support cases where variable is
	 * query_int.
	 *
	 * 变量应该是 int[]。我们不支持变量为 query_int 的情况。
	 */
	if (vardata.vartype != INT4ARRAYOID)
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 *
	 * 如果某个东西不是常量，也不能做任何有用的事情。
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);
	}

	/*
	 * The "@@" operator is strict, so we can cope with NULL right away.
	 *
	 * “@@”运算符是严格的，因此我们可以立即处理 NULL。
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * Verify that the Const is a query_int, else return a default estimate.
	 * (This could only fail if someone attached this estimator to the wrong
	 * operator.)
	 *
	 * 验证 Const 是否为 query_int，否则返回默认估计值。 （只有当有人将此估计器附加到错误的操作员时，这才会失败。）
	 */
	if (((Const *) other)->consttype !=
		get_function_sibling_type(fcinfo->flinfo->fn_oid, "query_int"))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);
	}

	query = DatumGetQueryTypeP(((Const *) other)->constvalue);

	/* Empty query matches nothing
	 *
	 * 空查询不匹配任何内容
	 */
	if (query->size == 0)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * Get the statistics for the intarray column.
	 *
	 * 获取 intarray 列的统计信息。
	 *
	 * We're interested in the Most-Common-Elements list, and the NULL
	 * fraction.
	 *
	 * 我们对最常见元素列表和 NULL 分数感兴趣。
	 */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		nullfrac = stats->stanullfrac;

		/*
		 * For an int4 array, the default array type analyze function will
		 * collect a Most Common Elements list, which is an array of int4s.
		 *
		 * 对于 int4 数组，默​​认数组类型分析函数将收集最常见元素列表，该列表是 int4 数组。
		 */
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			Assert(sslot.valuetype == INT4OID);

			/*
			 * There should be three more Numbers than Values, because the
			 * last three (for intarray) cells are taken for minimal, maximal
			 * and nulls frequency. Punt if not.
			 *
			 * 数字应该比值多三个，因为最后三个（对于 intarray）单元格用于最小、最大和空频率。没有的话就弃踢吧。
			 */
			if (sslot.nnumbers == sslot.nvalues + 3)
			{
				/* Grab the lowest frequency.
				 *
				 * 获取最低频率。
				 */
				minfreq = sslot.numbers[sslot.nnumbers - (sslot.nnumbers - sslot.nvalues)];

				mcelems = sslot.values;
				mcefreqs = sslot.numbers;
				nmcelems = sslot.nvalues;
			}
		}
	}
	else
		memset(&sslot, 0, sizeof(sslot));

	/* Process the logical expression in the query, using the stats
	 *
	 * 使用统计信息处理查询中的逻辑表达式
	 */
	selec = int_query_opr_selec(GETQUERY(query) + query->size - 1,
								mcelems, mcefreqs, nmcelems, minfreq);

	/* MCE stats count only non-null rows, so adjust for null rows.
	 *
	 * MCE 统计信息仅计算非空行，因此请针对空行进行调整。
	 */
	selec *= (1.0 - nullfrac);

	free_attstatsslot(&sslot);
	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * Estimate selectivity of single intquery operator
 *
 * 估计单个 intquery 运算符的选择性
 */
static Selectivity
int_query_opr_selec(ITEM *item, Datum *mcelems, float4 *mcefreqs,
					int nmcelems, float4 minfreq)
{
	Selectivity selec;

	/* since this function recurses, it could be driven to stack overflow
	 *
	 * 由于该函数会递归，因此可能会导致堆栈溢出
	 */
	check_stack_depth();

	if (item->type == VAL)
	{
		Datum	   *searchres;

		if (mcelems == NULL)
			return (Selectivity) DEFAULT_EQ_SEL;

		searchres = (Datum *) bsearch(&item->val, mcelems, nmcelems,
									  sizeof(Datum), compare_val_int4);
		if (searchres)
		{
			/*
			 * The element is in MCELEM.  Return precise selectivity (or at
			 * least as precise as ANALYZE could find out).
			 *
			 * 该元素位于 MCELEM 中。  返回精确的选择性（或至少与 ANALYZE 可以发现的一样精确）。
			 */
			selec = mcefreqs[searchres - mcelems];
		}
		else
		{
			/*
			 * The element is not in MCELEM.  Punt, but assume that the
			 * selectivity cannot be more than minfreq / 2.
			 *
			 * 该元素不在 MCELEM 中。  Punt，但假设选择性不能超过 minfreq / 2。
			 */
			selec = Min(DEFAULT_EQ_SEL, minfreq / 2);
		}
	}
	else if (item->type == OPR)
	{
		/* Current query node is an operator
		 *
		 * 当前查询节点是一个算子
		 */
		Selectivity s1,
					s2;

		s1 = int_query_opr_selec(item - 1, mcelems, mcefreqs, nmcelems,
								 minfreq);
		switch (item->val)
		{
			case (int32) '!':
				selec = 1.0 - s1;
				break;

			case (int32) '&':
				s2 = int_query_opr_selec(item + item->left, mcelems, mcefreqs,
										 nmcelems, minfreq);
				selec = s1 * s2;
				break;

			case (int32) '|':
				s2 = int_query_opr_selec(item + item->left, mcelems, mcefreqs,
										 nmcelems, minfreq);
				selec = s1 + s2 - s1 * s2;
				break;

			default:
				elog(ERROR, "unrecognized operator: %d", item->val);
				selec = 0;		/* keep compiler quiet */
				break;
		}
	}
	else
	{
		elog(ERROR, "unrecognized int query item type: %u", item->type);
		selec = 0;				/* keep compiler quiet */
	}

	/* Clamp intermediate results to stay sane despite roundoff error
	 *
	 * 尽管有舍入误差，但仍可限制中间结果以保持理智
	 */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * Comparison function for binary search in mcelem array.
 *
 * mcelem 数组中二分查找的比较函数。
 */
static int
compare_val_int4(const void *a, const void *b)
{
	int32		key = *(int32 *) a;
	int32		value = DatumGetInt32(*(const Datum *) b);

	if (key < value)
		return -1;
	else if (key > value)
		return 1;
	else
		return 0;
}
