/*-------------------------------------------------------------------------
 *
 * hstore_subs.c
 *	  Subscripting support functions for hstore.
 *
 * This is a great deal simpler than array_subs.c, because the result of
 * subscripting an hstore is just a text string (the value for the key).
 * We do not need to support array slicing notation, nor multiple subscripts.
 * Less obviously, because the subscript result is never a SQL container
 * type, there will never be any nested-assignment scenarios, so we do not
 * need a fetch_old function.  In turn, that means we can drop the
 * check_subscripts function and just let the fetch and assign functions
 * do everything.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  contrib/hstore/hstore_subs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execExpr.h"
#include "hstore.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"


/*
 * Finish parse analysis of a SubscriptingRef expression for hstore.
 *
 * 完成 hstore 的 SubscriptingRef 表达式的解析分析。
 *
 * Verify there's just one subscript, coerce it to text,
 * and set the result type of the SubscriptingRef node.
 *
 * 验证只有一个下标，将其强制为文本，然后设置 SubscriptingRef 节点的结果类型。
 */
static void
hstore_subscript_transform(SubscriptingRef *sbsref,
						   List *indirection,
						   ParseState *pstate,
						   bool isSlice,
						   bool isAssignment)
{
	A_Indices  *ai;
	Node	   *subexpr;

	/* We support only single-subscript, non-slice cases
	 *
	 * 我们仅支持单下标、非切片情况
	 */
	if (isSlice || list_length(indirection) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hstore allows only one subscript"),
				 parser_errposition(pstate,
									exprLocation((Node *) indirection))));

	/* Transform the subscript expression to type text
	 *
	 * 将下标表达式转换为键入文本
	 */
	ai = linitial_node(A_Indices, indirection);
	Assert(ai->uidx != NULL && ai->lidx == NULL && !ai->is_slice);

	subexpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
	/* If it's not text already, try to coerce
	 *
	 * 如果还不是文本，请尝试强制
	 */
	subexpr = coerce_to_target_type(pstate,
									subexpr, exprType(subexpr),
									TEXTOID, -1,
									COERCION_ASSIGNMENT,
									COERCE_IMPLICIT_CAST,
									-1);
	if (subexpr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("hstore subscript must have type text"),
				 parser_errposition(pstate, exprLocation(ai->uidx))));

	/* ... and store the transformed subscript into the SubscriptRef node
	 *
	 * ...并将转换后的下标存储到 SubscriptRef 节点中
	 */
	sbsref->refupperindexpr = list_make1(subexpr);
	sbsref->reflowerindexpr = NIL;

	/* Determine the result type of the subscripting operation; always text
	 *
	 * 确定下标操作的结果类型；总是发短信
	 */
	sbsref->refrestype = TEXTOID;
	sbsref->reftypmod = -1;
}

/*
 * Evaluate SubscriptingRef fetch for hstore.
 *
 * 评估 hstore 的 SubscriptingRef 获取。
 *
 * Source container is in step's result variable (it's known not NULL, since
 * we set fetch_strict to true), and the subscript expression is in the
 * upperindex[] array.
 *
 * 源容器位于步骤的结果变量中（已知它不为 NULL，因为我们将 fetch_strict 设置为 true），下标表达式位于 upperindex[] 数组中。
 */
static void
hstore_subscript_fetch(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	HStore	   *hs;
	text	   *key;
	HEntry	   *entries;
	int			idx;
	text	   *out;

	/* Should not get here if source hstore is null
	 *
	 * 如果源 hstore 为空，则不应到达此处
	 */
	Assert(!(*op->resnull));

	/* Check for null subscript
	 *
	 * 检查下标是否为空
	 */
	if (sbsrefstate->upperindexnull[0])
	{
		*op->resnull = true;
		return;
	}

	/* OK, fetch/detoast the hstore and subscript
	 *
	 * OK，获取/detoast hstore 和下标
	 */
	hs = DatumGetHStoreP(*op->resvalue);
	key = DatumGetTextPP(sbsrefstate->upperindex[0]);

	/* The rest is basically the same as hstore_fetchval()
	 *
	 * 其余与hstore_fetchval()基本相同
	 */
	entries = ARRPTR(hs);
	idx = hstoreFindKey(hs, NULL,
						VARDATA_ANY(key), VARSIZE_ANY_EXHDR(key));

	if (idx < 0 || HSTORE_VALISNULL(entries, idx))
	{
		*op->resnull = true;
		return;
	}

	out = cstring_to_text_with_len(HSTORE_VAL(entries, STRPTR(hs), idx),
								   HSTORE_VALLEN(entries, idx));

	*op->resvalue = PointerGetDatum(out);
}

/*
 * Evaluate SubscriptingRef assignment for hstore.
 *
 * 评估 hstore 的 SubscriptingRef 分配。
 *
 * Input container (possibly null) is in result area, replacement value is in
 * SubscriptingRefState's replacevalue/replacenull.
 *
 * 输入容器（可能为空）位于结果区域中，替换值位于 SubscriptingRefState 的replacevalue/replacenull 中。
 */
static void
hstore_subscript_assign(ExprState *state,
						ExprEvalStep *op,
						ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	text	   *key;
	Pairs		p;
	HStore	   *out;

	/* Check for null subscript
	 *
	 * 检查下标是否为空
	 */
	if (sbsrefstate->upperindexnull[0])
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("hstore subscript in assignment must not be null")));

	/* OK, fetch/detoast the subscript
	 *
	 * OK，获取/取消下标
	 */
	key = DatumGetTextPP(sbsrefstate->upperindex[0]);

	/* Create a Pairs entry for subscript + replacement value
	 *
	 * 为下标 + 替换值创建 Pairs 条目
	 */
	p.needfree = false;
	p.key = VARDATA_ANY(key);
	p.keylen = hstoreCheckKeyLen(VARSIZE_ANY_EXHDR(key));

	if (sbsrefstate->replacenull)
	{
		p.vallen = 0;
		p.isnull = true;
	}
	else
	{
		text	   *val = DatumGetTextPP(sbsrefstate->replacevalue);

		p.val = VARDATA_ANY(val);
		p.vallen = hstoreCheckValLen(VARSIZE_ANY_EXHDR(val));
		p.isnull = false;
	}

	if (*op->resnull)
	{
		/* Just build a one-element hstore (cf. hstore_from_text)
		 *
		 * 只需构建一个单元素 hstore（参见 hstore_from_text）
		 */
		out = hstorePairs(&p, 1, p.keylen + p.vallen);
	}
	else
	{
		/*
		 * Otherwise, merge the new key into the hstore.  Based on
		 * hstore_concat.
		 *
		 * 否则，将新密钥合并到 hstore 中。  基于hstore_concat。
		 */
		HStore	   *hs = DatumGetHStoreP(*op->resvalue);
		int			s1count = HS_COUNT(hs);
		int			outcount = 0;
		int			vsize;
		char	   *ps1,
				   *bufd,
				   *pd;
		HEntry	   *es1,
				   *ed;
		int			s1idx;
		int			s2idx;

		/* Allocate result without considering possibility of duplicate
		 *
		 * 分配结果而不考虑重复的可能性
		 */
		vsize = CALCDATASIZE(s1count + 1, VARSIZE(hs) + p.keylen + p.vallen);
		out = palloc(vsize);
		SET_VARSIZE(out, vsize);
		HS_SETCOUNT(out, s1count + 1);

		ps1 = STRPTR(hs);
		bufd = pd = STRPTR(out);
		es1 = ARRPTR(hs);
		ed = ARRPTR(out);

		for (s1idx = s2idx = 0; s1idx < s1count || s2idx < 1; ++outcount)
		{
			int			difference;

			if (s1idx >= s1count)
				difference = 1;
			else if (s2idx >= 1)
				difference = -1;
			else
			{
				int			s1keylen = HSTORE_KEYLEN(es1, s1idx);
				int			s2keylen = p.keylen;

				if (s1keylen == s2keylen)
					difference = memcmp(HSTORE_KEY(es1, ps1, s1idx),
										p.key,
										s1keylen);
				else
					difference = (s1keylen > s2keylen) ? 1 : -1;
			}

			if (difference >= 0)
			{
				HS_ADDITEM(ed, bufd, pd, p);
				++s2idx;
				if (difference == 0)
					++s1idx;
			}
			else
			{
				HS_COPYITEM(ed, bufd, pd,
							HSTORE_KEY(es1, ps1, s1idx),
							HSTORE_KEYLEN(es1, s1idx),
							HSTORE_VALLEN(es1, s1idx),
							HSTORE_VALISNULL(es1, s1idx));
				++s1idx;
			}
		}

		HS_FINALIZE(out, outcount, bufd, pd);
	}

	*op->resvalue = PointerGetDatum(out);
	*op->resnull = false;
}

/*
 * Set up execution state for an hstore subscript operation.
 *
 * 设置hstore下标操作的执行状态。
 */
static void
hstore_exec_setup(const SubscriptingRef *sbsref,
				  SubscriptingRefState *sbsrefstate,
				  SubscriptExecSteps *methods)
{
	/* Assert we are dealing with one subscript
	 *
	 * 断言我们正在处理一个下标
	 */
	Assert(sbsrefstate->numlower == 0);
	Assert(sbsrefstate->numupper == 1);
	/* We can't check upperprovided[0] here, but it must be true
	 *
	 * 我们无法在这里检查 upperprovided[0]，但它必须是 true
	 */

	/* Pass back pointers to appropriate step execution functions
	 *
	 * 将指针传回适当的步骤执行函数
	 */
	methods->sbs_check_subscripts = NULL;
	methods->sbs_fetch = hstore_subscript_fetch;
	methods->sbs_assign = hstore_subscript_assign;
	methods->sbs_fetch_old = NULL;
}

/*
 * hstore_subscript_handler
 *		Subscripting handler for hstore.
 *
 * hstore_subscript_handler hstore 的订阅处理程序。
 */
PG_FUNCTION_INFO_V1(hstore_subscript_handler);
Datum
hstore_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = hstore_subscript_transform,
		.exec_setup = hstore_exec_setup,
		.fetch_strict = true,	/* fetch returns NULL for NULL inputs */
		.fetch_leakproof = true,	/* fetch returns NULL for bad subscript */
		.store_leakproof = false	/* ... but assignment throws error */
	};

	PG_RETURN_POINTER(&sbsroutines);
}
