/*
 * op function for ltree and lquery
 * Teodor Sigaev <teodor@stack.net>
 * contrib/ltree/lquery_op.c
 *
 * ltree 和 lquery 的 op 函数 Teodor Sigaev <teodor@stack.net> contrib/ltree/lquery_op.c
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_collation.h"
#include "ltree.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/formatting.h"

PG_FUNCTION_INFO_V1(ltq_regex);
PG_FUNCTION_INFO_V1(ltq_rregex);

PG_FUNCTION_INFO_V1(lt_q_regex);
PG_FUNCTION_INFO_V1(lt_q_rregex);

#define NEXTVAL(x) ( (lquery*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

static char *
getlexeme(char *start, char *end, int *len)
{
	char	   *ptr;

	while (start < end && t_iseq(start, '_'))
		start += pg_mblen_range(start, end);

	ptr = start;
	if (ptr >= end)
		return NULL;

	while (ptr < end && !t_iseq(ptr, '_'))
		ptr += pg_mblen_range(ptr, end);

	*len = ptr - start;
	return start;
}

bool
compare_subnode(ltree_level *t, char *qn, int len,
				ltree_prefix_eq_func prefix_eq, bool anyend)
{
	char	   *endt = t->name + t->len;
	char	   *endq = qn + len;
	char	   *tn;
	int			lent,
				lenq;
	bool		isok;

	while ((qn = getlexeme(qn, endq, &lenq)) != NULL)
	{
		tn = t->name;
		isok = false;
		while ((tn = getlexeme(tn, endt, &lent)) != NULL)
		{
			if ((lent == lenq || (lent > lenq && anyend)) &&
				(*prefix_eq) (qn, lenq, tn, lent))
			{

				isok = true;
				break;
			}
			tn += lent;
		}

		if (!isok)
			return false;
		qn += lenq;
	}

	return true;
}

/*
 * Check if 'a' is a prefix of 'b'.
 *
 * 检查“a”是否是“b”的前缀。
 */
bool
ltree_prefix_eq(const char *a, size_t a_sz, const char *b, size_t b_sz)
{
	if (a_sz > b_sz)
		return false;
	else
		return (strncmp(a, b, a_sz) == 0);
}

/*
 * Case-insensitive check if 'a' is a prefix of 'b'.
 *
 * 不区分大小写检查“a”是否是“b”的前缀。
 */
bool
ltree_prefix_eq_ci(const char *a, size_t a_sz, const char *b, size_t b_sz)
{
	static pg_locale_t locale = NULL;
	size_t		al_sz = a_sz + 1;
	size_t		al_len;
	char	   *al;
	size_t		bl_sz = b_sz + 1;
	size_t		bl_len;
	char	   *bl;
	bool		res;

	if (!locale)
		locale = pg_newlocale_from_collation(DEFAULT_COLLATION_OID);

	if (locale->ctype_is_c)
	{
		if (a_sz > b_sz)
			return false;

		for (int i = 0; i < a_sz; i++)
		{
			if (pg_ascii_tolower(a[i]) != pg_ascii_tolower(b[i]))
				return false;
		}

		return true;
	}

	al = palloc(al_sz);
	bl = palloc(bl_sz);

	/* casefold both a and b
	 *
	 * a 和 b 均折叠
	 */

	al_len = pg_strfold(al, al_sz, a, a_sz, locale);
	if (al_len + 1 > al_sz)
	{
		/* grow buffer if needed and retry
		 *
		 * 如果需要的话增加缓冲区并重试
		 */
		al_sz = al_len + 1;
		al = repalloc(al, al_sz);
		al_len = pg_strfold(al, al_sz, a, a_sz, locale);
		Assert(al_len + 1 <= al_sz);
	}

	bl_len = pg_strfold(bl, bl_sz, b, b_sz, locale);
	if (bl_len + 1 > bl_sz)
	{
		/* grow buffer if needed and retry
		 *
		 * 如果需要的话增加缓冲区并重试
		 */
		bl_sz = bl_len + 1;
		bl = repalloc(bl, bl_sz);
		bl_len = pg_strfold(bl, bl_sz, b, b_sz, locale);
		Assert(bl_len + 1 <= bl_sz);
	}

	if (al_len > bl_len)
		res = false;
	else
		res = (strncmp(al, bl, al_len) == 0);

	pfree(al);
	pfree(bl);

	return res;
}

/*
 * See if an lquery_level matches an ltree_level
 *
 * 查看 lquery_level 是否与 ltree_level 匹配
 *
 * This accounts for all flags including LQL_NOT, but does not
 * consider repetition counts.
 *
 * 这考虑了包括 LQL_NOT 在内的所有标志，但不考虑重复计数。
 */
static bool
checkLevel(lquery_level *curq, ltree_level *curt)
{
	lquery_variant *curvar = LQL_FIRST(curq);
	bool		success;

	success = (curq->flag & LQL_NOT) ? false : true;

	/* numvar == 0 means '*' which matches anything
	 *
	 * numvar == 0 表示 '*' 匹配任何内容
	 */
	if (curq->numvar == 0)
		return success;

	for (int i = 0; i < curq->numvar; i++)
	{
		ltree_prefix_eq_func prefix_eq;

		prefix_eq = (curvar->flag & LVAR_INCASE) ? ltree_prefix_eq_ci : ltree_prefix_eq;

		if (curvar->flag & LVAR_SUBLEXEME)
		{
			if (compare_subnode(curt, curvar->name, curvar->len, prefix_eq,
								(curvar->flag & LVAR_ANYEND)))
				return success;
		}
		else if ((curvar->len == curt->len ||
				  (curt->len > curvar->len && (curvar->flag & LVAR_ANYEND))) &&
				 (*prefix_eq) (curvar->name, curvar->len, curt->name, curt->len))
			return success;

		curvar = LVAR_NEXT(curvar);
	}
	return !success;
}

/*
 * Try to match an lquery (of qlen items) to an ltree (of tlen items)
 *
 * 尝试将 lquery（qlen 项）与 ltree（tlen 项）匹配
 */
static bool
checkCond(lquery_level *curq, int qlen,
		  ltree_level *curt, int tlen)
{
	/* Since this function recurses, it could be driven to stack overflow
	 *
	 * 由于该函数会递归，因此可能会导致堆栈溢出
	 */
	check_stack_depth();

	/* Pathological patterns could take awhile, too
	 *
	 * 病理模式也可能需要一段时间
	 */
	CHECK_FOR_INTERRUPTS();

	/* Loop while we have query items to consider
	 *
	 * 当我们有要考虑的查询项时循环
	 */
	while (qlen > 0)
	{
		int			low,
					high;
		lquery_level *nextq;

		/*
		 * Get min and max repetition counts for this query item, dealing with
		 * the backwards-compatibility hack that the low/high fields aren't
		 * meaningful for non-'*' items unless LQL_COUNT is set.
		 *
		 * 获取此查询项的最小和最大重复计数，处理向后兼容性黑客，除非设置了 LQL_COUNT，否则低/高字段对于非“*”项没有意义。
		 */
		if ((curq->flag & LQL_COUNT) || curq->numvar == 0)
			low = curq->low, high = curq->high;
		else
			low = high = 1;

		/*
		 * We may limit "high" to the remaining text length; this avoids
		 * separate tests below.
		 *
		 * 我们可能会将“高”限制为剩余文本长度；这避免了下面的单独测试。
		 */
		if (high > tlen)
			high = tlen;

		/* Fail if a match of required number of items is impossible
		 *
		 * 如果无法匹配所需数量的项目，则失败
		 */
		if (high < low)
			return false;

		/*
		 * Recursively check the rest of the pattern against each possible
		 * start point following some of this item's match(es).
		 *
		 * 根据该项目的某些匹配项之后的每个可能的起点，递归地检查模式的其余部分。
		 */
		nextq = LQL_NEXT(curq);
		qlen--;

		for (int matchcnt = 0; matchcnt < high; matchcnt++)
		{
			/*
			 * If we've consumed an acceptable number of matches of this item,
			 * and the rest of the pattern matches beginning here, we're good.
			 *
			 * 如果我们已经消耗了该项目的可接受数量的匹配，并且其余的模式匹配从这里开始，那么我们就很好。
			 */
			if (matchcnt >= low && checkCond(nextq, qlen, curt, tlen))
				return true;

			/*
			 * Otherwise, try to match one more text item to this query item.
			 *
			 * 否则，请尝试将另一个文本项与该查询项相匹配。
			 */
			if (!checkLevel(curq, curt))
				return false;

			curt = LEVEL_NEXT(curt);
			tlen--;
		}

		/*
		 * Once we've consumed "high" matches, we can succeed only if the rest
		 * of the pattern matches beginning here.  Loop around (if you prefer,
		 * think of this as tail recursion).
		 *
		 * 一旦我们消耗了“高”匹配，只有当模式的其余部分从这里开始匹配时，我们才能成功。  循环（如果您愿意，可以将其视为尾递归）。
		 */
		curq = nextq;
	}

	/*
	 * Once we're out of query items, we match only if there's no remaining
	 * text either.
	 *
	 * 一旦我们没有查询项，我们仅在没有剩余文本的情况下进行匹配。
	 */
	return (tlen == 0);
}

Datum
ltq_regex(PG_FUNCTION_ARGS)
{
	ltree	   *tree = PG_GETARG_LTREE_P(0);
	lquery	   *query = PG_GETARG_LQUERY_P(1);
	bool		res;

	res = checkCond(LQUERY_FIRST(query), query->numlevel,
					LTREE_FIRST(tree), tree->numlevel);

	PG_FREE_IF_COPY(tree, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}

Datum
ltq_rregex(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(ltq_regex,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										));
}

Datum
lt_q_regex(PG_FUNCTION_ARGS)
{
	ltree	   *tree = PG_GETARG_LTREE_P(0);
	ArrayType  *_query = PG_GETARG_ARRAYTYPE_P(1);
	lquery	   *query = (lquery *) ARR_DATA_PTR(_query);
	bool		res = false;
	int			num = ArrayGetNItems(ARR_NDIM(_query), ARR_DIMS(_query));

	if (ARR_NDIM(_query) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array must be one-dimensional")));
	if (array_contains_nulls(_query))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	while (num > 0)
	{
		if (DatumGetBool(DirectFunctionCall2(ltq_regex,
											 PointerGetDatum(tree), PointerGetDatum(query))))
		{

			res = true;
			break;
		}
		num--;
		query = NEXTVAL(query);
	}

	PG_FREE_IF_COPY(tree, 0);
	PG_FREE_IF_COPY(_query, 1);
	PG_RETURN_BOOL(res);
}

Datum
lt_q_rregex(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(DirectFunctionCall2(lt_q_regex,
										PG_GETARG_DATUM(1),
										PG_GETARG_DATUM(0)
										));
}
