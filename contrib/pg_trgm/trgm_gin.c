/*
 * contrib/pg_trgm/trgm_gin.c
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "fmgr.h"
#include "trgm.h"
#include "varatt.h"

PG_FUNCTION_INFO_V1(gin_extract_trgm);
PG_FUNCTION_INFO_V1(gin_extract_value_trgm);
PG_FUNCTION_INFO_V1(gin_extract_query_trgm);
PG_FUNCTION_INFO_V1(gin_trgm_consistent);
PG_FUNCTION_INFO_V1(gin_trgm_triconsistent);

/*
 * This function can only be called if a pre-9.1 version of the GIN operator
 * class definition is present in the catalogs (probably as a consequence
 * of upgrade-in-place).  Cope.
 *
 * 仅当目录中存在 9.1 之前版本的 GIN 运算符类定义时才能调用此函数（可能是就地升级的结果）。  应付。
 */
Datum
gin_extract_trgm(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() == 3)
		return gin_extract_value_trgm(fcinfo);
	if (PG_NARGS() == 7)
		return gin_extract_query_trgm(fcinfo);
	elog(ERROR, "unexpected number of arguments to gin_extract_trgm");
	PG_RETURN_NULL();
}

Datum
gin_extract_value_trgm(PG_FUNCTION_ARGS)
{
	text	   *val = (text *) PG_GETARG_TEXT_PP(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	TRGM	   *trg;
	int32		trglen;

	*nentries = 0;

	trg = generate_trgm(VARDATA_ANY(val), VARSIZE_ANY_EXHDR(val));
	trglen = ARRNELEM(trg);

	if (trglen > 0)
	{
		trgm	   *ptr;
		int32		i;

		*nentries = trglen;
		entries = (Datum *) palloc(sizeof(Datum) * trglen);

		ptr = GETARR(trg);
		for (i = 0; i < trglen; i++)
		{
			int32		item = trgm2int(ptr);

			entries[i] = Int32GetDatum(item);
			ptr++;
		}
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_extract_query_trgm(PG_FUNCTION_ARGS)
{
	text	   *val = (text *) PG_GETARG_TEXT_PP(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);

	/* bool   **pmatch = (bool **) PG_GETARG_POINTER(3);
	 *
	 * 布尔 **pmatch = (布尔 **) PG_GETARG_POINTER(3);
	 */
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

	/* bool   **nullFlags = (bool **) PG_GETARG_POINTER(5);
	 *
	 * 布尔 **nullFlags = (布尔 **) PG_GETARG_POINTER(5);
	 */
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries = NULL;
	TRGM	   *trg;
	int32		trglen;
	trgm	   *ptr;
	TrgmPackedGraph *graph;
	int32		i;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:
		case EqualStrategyNumber:
			trg = generate_trgm(VARDATA_ANY(val), VARSIZE_ANY_EXHDR(val));
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU
			 *
			 * 跌倒
			 */
		case LikeStrategyNumber:

			/*
			 * For wildcard search we extract all the trigrams that every
			 * potentially-matching string must include.
			 *
			 * 对于通配符搜索，我们提取每个潜在匹配字符串必须包含的所有三元组。
			 */
			trg = generate_wildcard_trgm(VARDATA_ANY(val),
										 VARSIZE_ANY_EXHDR(val));
			break;
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU
			 *
			 * 跌倒
			 */
		case RegExpStrategyNumber:
			trg = createTrgmNFA(val, PG_GET_COLLATION(),
								&graph, CurrentMemoryContext);
			if (trg && ARRNELEM(trg) > 0)
			{
				/*
				 * Successful regex processing: store NFA-like graph as
				 * extra_data.  GIN API requires an array of nentries
				 * Pointers, but we just put the same value in each element.
				 *
				 * 成功的正则表达式处理：将类似 NFA 的图存储为 extra_data。  GIN API 需要一个 nentries 指针数组，但我们只是在每个元素中放入相同的值。
				 */
				trglen = ARRNELEM(trg);
				*extra_data = (Pointer *) palloc(sizeof(Pointer) * trglen);
				for (i = 0; i < trglen; i++)
					(*extra_data)[i] = (Pointer) graph;
			}
			else
			{
				/* No result: have to do full index scan.
				 *
				 * 没有结果：必须进行全索引扫描。
				 */
				*nentries = 0;
				*searchMode = GIN_SEARCH_MODE_ALL;
				PG_RETURN_POINTER(entries);
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			trg = NULL;			/* keep compiler quiet */
			break;
	}

	trglen = ARRNELEM(trg);
	*nentries = trglen;

	if (trglen > 0)
	{
		entries = (Datum *) palloc(sizeof(Datum) * trglen);
		ptr = GETARR(trg);
		for (i = 0; i < trglen; i++)
		{
			int32		item = trgm2int(ptr);

			entries[i] = Int32GetDatum(item);
			ptr++;
		}
	}

	/*
	 * If no trigram was extracted then we have to scan all the index.
	 *
	 * 如果没有提取出三元组，那么我们必须扫描所有索引。
	 */
	if (trglen == 0)
		*searchMode = GIN_SEARCH_MODE_ALL;

	PG_RETURN_POINTER(entries);
}

Datum
gin_trgm_consistent(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* text    *query = PG_GETARG_TEXT_PP(2);
	 *
	 * 文本*查询= PG_GETARG_TEXT_PP(2);
	 */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res;
	int32		i,
				ntrue;
	double		nlimit;

	/* All cases served by this function are inexact
	 *
	 * 该函数提供的所有案例都是不准确的
	 */
	*recheck = true;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:
			nlimit = index_strategy_get_limit(strategy);

			/* Count the matches
			 *
			 * 计算匹配数
			 */
			ntrue = 0;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i])
					ntrue++;
			}

			/*--------------------
			 * If DIVUNION is defined then similarity formula is:
			 * c / (len1 + len2 - c)
			 * where c is number of common trigrams and it stands as ntrue in
			 * this code.  Here we don't know value of len2 but we can assume
			 * that c (ntrue) is a lower bound of len2, so upper bound of
			 * similarity is:
			 * c / (len1 + c - c)  => c / len1
			 * If DIVUNION is not defined then similarity formula is:
			 * c / max(len1, len2)
			 * And again, c (ntrue) is a lower bound of len2, but c <= len1
			 * just by definition and, consequently, upper bound of
			 * similarity is just c / len1.
			 * So, independently on DIVUNION the upper bound formula is the same.
			 *
			 * 如果定义了 DIVUNION，则相似度公式为：c / (len1 + len2 - c)，其中 c 是常见三元组的数量，在此代码中它表示为 ntrue。  这里我们不知道 len2 的值，但我们可以假设 c (ntrue) 是 len2 的下界，因此相似度的上限为： c / (len1 + c - c) => c / len1 如果未定义 DIVUNION 则相似度公式为： c / max(len1, len2) 再次，c (ntrue) 是 len2 的下界，但根据定义 c <= len1 ，因此相似度的上限就是 c /长度1。因此，独立于 DIVUNION 的上限公式是相同的。
			 */
			res = (nkeys == 0) ? false :
				(((((float4) ntrue) / ((float4) nkeys))) >= nlimit);
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU
			 *
			 * 跌倒
			 */
		case LikeStrategyNumber:
		case EqualStrategyNumber:
			/* Check if all extracted trigrams are presented.
			 *
			 * 检查是否显示了所有提取的三元组。
			 */
			res = true;
			for (i = 0; i < nkeys; i++)
			{
				if (!check[i])
				{
					res = false;
					break;
				}
			}
			break;
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU
			 *
			 * 跌倒
			 */
		case RegExpStrategyNumber:
			if (nkeys < 1)
			{
				/* Regex processing gave no result: do full index scan
				 *
				 * 正则表达式处理没有给出结果：执行完整索引扫描
				 */
				res = true;
			}
			else
				res = trigramsMatchGraph((TrgmPackedGraph *) extra_data[0],
										 check);
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = false;		/* keep compiler quiet */
			break;
	}

	PG_RETURN_BOOL(res);
}

/*
 * In all cases, GIN_TRUE is at least as favorable to inclusion as
 * GIN_MAYBE. If no better option is available, simply treat
 * GIN_MAYBE as if it were GIN_TRUE and apply the same test as the binary
 * consistent function.
 *
 * 在所有情况下，GIN_TRUE 至少与 GIN_MAYBE 一样有利于包含。如果没有更好的选项可用，只需将 GIN_MAYBE 视为 GIN_TRUE 并应用与二进制一致函数相同的测试。
 */
Datum
gin_trgm_triconsistent(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* text    *query = PG_GETARG_TEXT_PP(2);
	 *
	 * 文本*查询= PG_GETARG_TEXT_PP(2);
	 */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_MAYBE;
	int32		i,
				ntrue;
	bool	   *boolcheck;
	double		nlimit;

	switch (strategy)
	{
		case SimilarityStrategyNumber:
		case WordSimilarityStrategyNumber:
		case StrictWordSimilarityStrategyNumber:
			nlimit = index_strategy_get_limit(strategy);

			/* Count the matches
			 *
			 * 计算匹配数
			 */
			ntrue = 0;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] != GIN_FALSE)
					ntrue++;
			}

			/*
			 * See comment in gin_trgm_consistent() about * upper bound
			 * formula
			 *
			 * 请参阅 gin_trgm_concient() 中关于 * 上限公式的注释
			 */
			res = (nkeys == 0)
				? GIN_FALSE : (((((float4) ntrue) / ((float4) nkeys)) >= nlimit)
							   ? GIN_MAYBE : GIN_FALSE);
			break;
		case ILikeStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~~* with case-sensitive trigrams");
#endif
			/* FALL THRU
			 *
			 * 跌倒
			 */
		case LikeStrategyNumber:
		case EqualStrategyNumber:
			/* Check if all extracted trigrams are presented.
			 *
			 * 检查是否显示了所有提取的三元组。
			 */
			res = GIN_MAYBE;
			for (i = 0; i < nkeys; i++)
			{
				if (check[i] == GIN_FALSE)
				{
					res = GIN_FALSE;
					break;
				}
			}
			break;
		case RegExpICaseStrategyNumber:
#ifndef IGNORECASE
			elog(ERROR, "cannot handle ~* with case-sensitive trigrams");
#endif
			/* FALL THRU
			 *
			 * 跌倒
			 */
		case RegExpStrategyNumber:
			if (nkeys < 1)
			{
				/* Regex processing gave no result: do full index scan
				 *
				 * 正则表达式处理没有给出结果：执行完整索引扫描
				 */
				res = GIN_MAYBE;
			}
			else
			{
				/*
				 * As trigramsMatchGraph implements a monotonic boolean
				 * function, promoting all GIN_MAYBE keys to GIN_TRUE will
				 * give a conservative result.
				 *
				 * 由于 trigramsMatchGraph 实现了单调布尔函数，因此将所有 GIN_MAYBE 键提升为 GIN_TRUE 将给出保守的结果。
				 */
				boolcheck = (bool *) palloc(sizeof(bool) * nkeys);
				for (i = 0; i < nkeys; i++)
					boolcheck[i] = (check[i] != GIN_FALSE);
				if (!trigramsMatchGraph((TrgmPackedGraph *) extra_data[0],
										boolcheck))
					res = GIN_FALSE;
				pfree(boolcheck);
			}
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			res = GIN_FALSE;	/* keep compiler quiet */
			break;
	}

	/* All cases served by this function are inexact
	 *
	 * 该函数提供的所有案例都是不准确的
	 */
	Assert(res != GIN_TRUE);
	PG_RETURN_GIN_TERNARY_VALUE(res);
}
