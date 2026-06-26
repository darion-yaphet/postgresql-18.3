/*
 * contrib/seg/seg.c
 *
 ******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
 *
 * 该文件包含可以绑定到 Postgres 后端并由后端在处理查询过程中调用的例程。  这些例程的调用格式由 Postgres 体系结构决定。
******************************************************************************/

#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "fmgr.h"

#include "segdata.h"


#define DatumGetSegP(X) ((SEG *) DatumGetPointer(X))
#define PG_GETARG_SEG_P(n) DatumGetSegP(PG_GETARG_DATUM(n))


/*
#define GIST_DEBUG
#define GIST_QUERY_DEBUG
*/

PG_MODULE_MAGIC_EXT(
					.name = "seg",
					.version = PG_VERSION
);

/*
 * Auxiliary data structure for picksplit method.
 *
 * picksplit方法的辅助数据结构。
 */
typedef struct
{
	float		center;
	OffsetNumber index;
	SEG		   *data;
} gseg_picksplit_item;

/*
** Input/Output routines
*
* * 输入/输出例程
*/
PG_FUNCTION_INFO_V1(seg_in);
PG_FUNCTION_INFO_V1(seg_out);
PG_FUNCTION_INFO_V1(seg_size);
PG_FUNCTION_INFO_V1(seg_lower);
PG_FUNCTION_INFO_V1(seg_upper);
PG_FUNCTION_INFO_V1(seg_center);

/*
** GiST support methods
*
* * GiST支持方法
*/
PG_FUNCTION_INFO_V1(gseg_consistent);
PG_FUNCTION_INFO_V1(gseg_compress);
PG_FUNCTION_INFO_V1(gseg_decompress);
PG_FUNCTION_INFO_V1(gseg_picksplit);
PG_FUNCTION_INFO_V1(gseg_penalty);
PG_FUNCTION_INFO_V1(gseg_union);
PG_FUNCTION_INFO_V1(gseg_same);
static Datum gseg_leaf_consistent(Datum key, Datum query, StrategyNumber strategy);
static Datum gseg_internal_consistent(Datum key, Datum query, StrategyNumber strategy);
static Datum gseg_binary_union(Datum r1, Datum r2, int *sizep);


/*
** R-tree support functions
*
* * R树支持函数
*/
PG_FUNCTION_INFO_V1(seg_same);
PG_FUNCTION_INFO_V1(seg_contains);
PG_FUNCTION_INFO_V1(seg_contained);
PG_FUNCTION_INFO_V1(seg_overlap);
PG_FUNCTION_INFO_V1(seg_left);
PG_FUNCTION_INFO_V1(seg_over_left);
PG_FUNCTION_INFO_V1(seg_right);
PG_FUNCTION_INFO_V1(seg_over_right);
PG_FUNCTION_INFO_V1(seg_union);
PG_FUNCTION_INFO_V1(seg_inter);
static void rt_seg_size(SEG *a, float *size);

/*
** Various operators
*
* * 各种运营商
*/
PG_FUNCTION_INFO_V1(seg_cmp);
PG_FUNCTION_INFO_V1(seg_lt);
PG_FUNCTION_INFO_V1(seg_le);
PG_FUNCTION_INFO_V1(seg_gt);
PG_FUNCTION_INFO_V1(seg_ge);
PG_FUNCTION_INFO_V1(seg_different);

/*
** Auxiliary functions
*
* * 辅助功能
*/
static int	restore(char *result, float val, int n);


/*****************************************************************************
 * Input/Output functions
 *
 * 输入/输出功能
 *****************************************************************************/

Datum
seg_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	SEG		   *result = palloc(sizeof(SEG));
	yyscan_t	scanner;

	seg_scanner_init(str, &scanner);

	if (seg_yyparse(result, fcinfo->context, scanner) != 0)
		seg_yyerror(result, fcinfo->context, scanner, "bogus input");

	seg_scanner_finish(scanner);

	PG_RETURN_POINTER(result);
}

Datum
seg_out(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);
	char	   *result;
	char	   *p;

	p = result = (char *) palloc(40);

	if (seg->l_ext == '>' || seg->l_ext == '<' || seg->l_ext == '~')
		p += sprintf(p, "%c", seg->l_ext);

	if (seg->lower == seg->upper && seg->l_ext == seg->u_ext)
	{
		/*
		 * indicates that this interval was built by seg_in off a single point
		 *
		 * 表示该区间是由 seg_in 从单个点构建的
		 */
		p += restore(p, seg->lower, seg->l_sigd);
	}
	else
	{
		if (seg->l_ext != '-')
		{
			/* print the lower boundary if exists
			 *
			 * 如果存在则打印下边界
			 */
			p += restore(p, seg->lower, seg->l_sigd);
			p += sprintf(p, " ");
		}
		p += sprintf(p, "..");
		if (seg->u_ext != '-')
		{
			/* print the upper boundary if exists
			 *
			 * 如果存在则打印上边界
			 */
			p += sprintf(p, " ");
			if (seg->u_ext == '>' || seg->u_ext == '<' || seg->l_ext == '~')
				p += sprintf(p, "%c", seg->u_ext);
			p += restore(p, seg->upper, seg->u_sigd);
		}
	}

	PG_RETURN_CSTRING(result);
}

Datum
seg_center(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(((float) seg->lower + (float) seg->upper) / 2.0);
}

Datum
seg_lower(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(seg->lower);
}

Datum
seg_upper(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(seg->upper);
}


/*****************************************************************************
 *						   GiST functions
 *
 * GiST 函数
 *****************************************************************************/

/*
** The GiST Consistent method for segments
** Should return false if for all data items x below entry,
** the predicate x op query == false, where op is the oper
** corresponding to strategy in the pg_amop table.
*
* * 段的 GiST 一致性方法 * 如果对于条目下面的所有数据项 x，则应返回 false， * 谓词 x op 查询 == false，其中 op 是与 pg_amop 表中的策略相对应的操作。
*/
Datum
gseg_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		query = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3);
	 *
	 * Oid 子类型 = PG_GETARG_OID(3);
	 */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);

	/* All cases served by this function are exact
	 *
	 * 该函数服务的所有案例都是准确的
	 */
	*recheck = false;

	/*
	 * if entry is not leaf, use gseg_internal_consistent, else use
	 * gseg_leaf_consistent
	 *
	 * 如果条目不是叶子，则使用 gseg_internal_concient，否则使用 gseg_leaf_concient
	 */
	if (GIST_LEAF(entry))
		return gseg_leaf_consistent(entry->key, query, strategy);
	else
		return gseg_internal_consistent(entry->key, query, strategy);
}

/*
** The GiST Union method for segments
** returns the minimal bounding seg that encloses all the entries in entryvec
*
* * 段的 GiST Union 方法 * 返回包含 Entryvec 中所有条目的最小边界段
*/
Datum
gseg_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	int			numranges,
				i;
	Datum		out = 0;
	Datum		tmp;

#ifdef GIST_DEBUG
	fprintf(stderr, "union\n");
#endif

	numranges = entryvec->n;
	tmp = entryvec->vector[0].key;
	*sizep = sizeof(SEG);

	for (i = 1; i < numranges; i++)
	{
		out = gseg_binary_union(tmp, entryvec->vector[i].key, sizep);
		tmp = out;
	}

	PG_RETURN_DATUM(out);
}

/*
** GiST Compress and Decompress methods for segments
** do not do anything.
*
* * 段的 GiST 压缩和解压缩方法 * 不执行任何操作。
*/
Datum
gseg_compress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

Datum
gseg_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
** The GiST Penalty method for segments
** As in the R-tree paper, we use change in area as our penalty metric
*
* * 线段的 GiST 惩罚方法 * 与 R 树论文中一样，我们使用面积变化作为惩罚指标
*/
Datum
gseg_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	SEG		   *ud;
	float		tmp1,
				tmp2;

	ud = DatumGetSegP(DirectFunctionCall2(seg_union,
										  origentry->key,
										  newentry->key));
	rt_seg_size(ud, &tmp1);
	rt_seg_size(DatumGetSegP(origentry->key), &tmp2);
	*result = tmp1 - tmp2;

#ifdef GIST_DEBUG
	fprintf(stderr, "penalty\n");
	fprintf(stderr, "\t%g\n", *result);
#endif

	PG_RETURN_POINTER(result);
}

/*
 * Compare function for gseg_picksplit_item: sort by center.
 *
 * gseg_picksplit_item 的比较函数：按中心排序。
 */
static int
gseg_picksplit_item_cmp(const void *a, const void *b)
{
	const gseg_picksplit_item *i1 = (const gseg_picksplit_item *) a;
	const gseg_picksplit_item *i2 = (const gseg_picksplit_item *) b;

	if (i1->center < i2->center)
		return -1;
	else if (i1->center == i2->center)
		return 0;
	else
		return 1;
}

/*
 * The GiST PickSplit method for segments
 *
 * 用于分段的 GiST PickSplit 方法
 *
 * We used to use Guttman's split algorithm here, but since the data is 1-D
 * it's easier and more robust to just sort the segments by center-point and
 * split at the middle.
 *
 * 我们曾经在这里使用 Guttman 的分割算法，但由于数据是一维的，因此按中心点对段进行排序并在中间分割会更容易、更稳健。
 */
Datum
gseg_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			i;
	SEG		   *seg,
			   *seg_l,
			   *seg_r;
	gseg_picksplit_item *sort_items;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;
	OffsetNumber firstright;

#ifdef GIST_DEBUG
	fprintf(stderr, "picksplit\n");
#endif

	/* Valid items in entryvec->vector[] are indexed 1..maxoff
	 *
	 * Entryvec->vector[] 中的有效项索引为 1..maxoff
	 */
	maxoff = entryvec->n - 1;

	/*
	 * Prepare the auxiliary array and sort it.
	 *
	 * 准备辅助数组并排序。
	 */
	sort_items = (gseg_picksplit_item *)
		palloc(maxoff * sizeof(gseg_picksplit_item));
	for (i = 1; i <= maxoff; i++)
	{
		seg = DatumGetSegP(entryvec->vector[i].key);
		/* center calculation is done this way to avoid possible overflow
		 *
		 * 中心计算以这种方式完成以避免可能的溢出
		 */
		sort_items[i - 1].center = seg->lower * 0.5f + seg->upper * 0.5f;
		sort_items[i - 1].index = i;
		sort_items[i - 1].data = seg;
	}
	qsort(sort_items, maxoff, sizeof(gseg_picksplit_item),
		  gseg_picksplit_item_cmp);

	/* sort items below "firstright" will go into the left side
	 *
	 * 对“firstright”下方的项目进行排序将进入左侧
	 */
	firstright = maxoff / 2;

	v->spl_left = (OffsetNumber *) palloc(maxoff * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(maxoff * sizeof(OffsetNumber));
	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	/*
	 * Emit segments to the left output page, and compute its bounding box.
	 *
	 * 将段发送到左侧输出页面，并计算其边界框。
	 */
	seg_l = (SEG *) palloc(sizeof(SEG));
	memcpy(seg_l, sort_items[0].data, sizeof(SEG));
	*left++ = sort_items[0].index;
	v->spl_nleft++;
	for (i = 1; i < firstright; i++)
	{
		Datum		sortitem = PointerGetDatum(sort_items[i].data);

		seg_l = DatumGetSegP(DirectFunctionCall2(seg_union,
												 PointerGetDatum(seg_l),
												 sortitem));
		*left++ = sort_items[i].index;
		v->spl_nleft++;
	}

	/*
	 * Likewise for the right page.
	 *
	 * 右页也是如此。
	 */
	seg_r = (SEG *) palloc(sizeof(SEG));
	memcpy(seg_r, sort_items[firstright].data, sizeof(SEG));
	*right++ = sort_items[firstright].index;
	v->spl_nright++;
	for (i = firstright + 1; i < maxoff; i++)
	{
		Datum		sortitem = PointerGetDatum(sort_items[i].data);

		seg_r = DatumGetSegP(DirectFunctionCall2(seg_union,
												 PointerGetDatum(seg_r),
												 sortitem));
		*right++ = sort_items[i].index;
		v->spl_nright++;
	}

	v->spl_ldatum = PointerGetDatum(seg_l);
	v->spl_rdatum = PointerGetDatum(seg_r);

	PG_RETURN_POINTER(v);
}

/*
** Equality methods
*
* * 平等方法
*/
Datum
gseg_same(PG_FUNCTION_ARGS)
{
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (DirectFunctionCall2(seg_same, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)))
		*result = true;
	else
		*result = false;

#ifdef GIST_DEBUG
	fprintf(stderr, "same: %s\n", (*result ? "TRUE" : "FALSE"));
#endif

	PG_RETURN_POINTER(result);
}

/*
** SUPPORT ROUTINES
*
* * 支持例程
*/
static Datum
gseg_leaf_consistent(Datum key, Datum query, StrategyNumber strategy)
{
	Datum		retval;

#ifdef GIST_QUERY_DEBUG
	fprintf(stderr, "leaf_consistent, %d\n", strategy);
#endif

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = DirectFunctionCall2(seg_left, key, query);
			break;
		case RTOverLeftStrategyNumber:
			retval = DirectFunctionCall2(seg_over_left, key, query);
			break;
		case RTOverlapStrategyNumber:
			retval = DirectFunctionCall2(seg_overlap, key, query);
			break;
		case RTOverRightStrategyNumber:
			retval = DirectFunctionCall2(seg_over_right, key, query);
			break;
		case RTRightStrategyNumber:
			retval = DirectFunctionCall2(seg_right, key, query);
			break;
		case RTSameStrategyNumber:
			retval = DirectFunctionCall2(seg_same, key, query);
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = DirectFunctionCall2(seg_contains, key, query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval = DirectFunctionCall2(seg_contained, key, query);
			break;
		default:
			retval = false;
	}

	PG_RETURN_DATUM(retval);
}

static Datum
gseg_internal_consistent(Datum key, Datum query, StrategyNumber strategy)
{
	bool		retval;

#ifdef GIST_QUERY_DEBUG
	fprintf(stderr, "internal_consistent, %d\n", strategy);
#endif

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_over_right, key, query));
			break;
		case RTOverLeftStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_right, key, query));
			break;
		case RTOverlapStrategyNumber:
			retval =
				DatumGetBool(DirectFunctionCall2(seg_overlap, key, query));
			break;
		case RTOverRightStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_left, key, query));
			break;
		case RTRightStrategyNumber:
			retval =
				!DatumGetBool(DirectFunctionCall2(seg_over_left, key, query));
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval =
				DatumGetBool(DirectFunctionCall2(seg_contains, key, query));
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval =
				DatumGetBool(DirectFunctionCall2(seg_overlap, key, query));
			break;
		default:
			retval = false;
	}

	PG_RETURN_BOOL(retval);
}

static Datum
gseg_binary_union(Datum r1, Datum r2, int *sizep)
{
	Datum		retval;

	retval = DirectFunctionCall2(seg_union, r1, r2);
	*sizep = sizeof(SEG);

	return retval;
}


Datum
seg_contains(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL((a->lower <= b->lower) && (a->upper >= b->upper));
}

Datum
seg_contained(PG_FUNCTION_ARGS)
{
	Datum		a = PG_GETARG_DATUM(0);
	Datum		b = PG_GETARG_DATUM(1);

	PG_RETURN_DATUM(DirectFunctionCall2(seg_contains, b, a));
}

/*****************************************************************************
 * Operator class for R-tree indexing
 *
 * R 树索引的运算符类
 *****************************************************************************/

Datum
seg_same(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(DirectFunctionCall2(seg_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp == 0);
}

/*	seg_overlap -- does a overlap b?
 */
Datum
seg_overlap(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(((a->upper >= b->upper) && (a->lower <= b->upper)) ||
				   ((b->upper >= a->upper) && (b->lower <= a->upper)));
}

/*	seg_over_left -- is the right edge of (a) located at or left of the right edge of (b)?
 */
Datum
seg_over_left(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->upper <= b->upper);
}

/*	seg_left -- is (a) entirely on the left of (b)?
 */
Datum
seg_left(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->upper < b->lower);
}

/*	seg_right -- is (a) entirely on the right of (b)?
 */
Datum
seg_right(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->lower > b->upper);
}

/*	seg_over_right -- is the left edge of (a) located at or right of the left edge of (b)?
 */
Datum
seg_over_right(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	PG_RETURN_BOOL(a->lower >= b->lower);
}

Datum
seg_union(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);
	SEG		   *n;

	n = (SEG *) palloc(sizeof(*n));

	/* take max of upper endpoints
	 *
	 * 取上端点的最大值
	 */
	if (a->upper > b->upper)
	{
		n->upper = a->upper;
		n->u_sigd = a->u_sigd;
		n->u_ext = a->u_ext;
	}
	else
	{
		n->upper = b->upper;
		n->u_sigd = b->u_sigd;
		n->u_ext = b->u_ext;
	}

	/* take min of lower endpoints
	 *
	 * 取下端点的最小值
	 */
	if (a->lower < b->lower)
	{
		n->lower = a->lower;
		n->l_sigd = a->l_sigd;
		n->l_ext = a->l_ext;
	}
	else
	{
		n->lower = b->lower;
		n->l_sigd = b->l_sigd;
		n->l_ext = b->l_ext;
	}

	PG_RETURN_POINTER(n);
}

Datum
seg_inter(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);
	SEG		   *n;

	n = (SEG *) palloc(sizeof(*n));

	/* take min of upper endpoints
	 *
	 * 取上端点的最小值
	 */
	if (a->upper < b->upper)
	{
		n->upper = a->upper;
		n->u_sigd = a->u_sigd;
		n->u_ext = a->u_ext;
	}
	else
	{
		n->upper = b->upper;
		n->u_sigd = b->u_sigd;
		n->u_ext = b->u_ext;
	}

	/* take max of lower endpoints
	 *
	 * 取下端点的最大值
	 */
	if (a->lower > b->lower)
	{
		n->lower = a->lower;
		n->l_sigd = a->l_sigd;
		n->l_ext = a->l_ext;
	}
	else
	{
		n->lower = b->lower;
		n->l_sigd = b->l_sigd;
		n->l_ext = b->l_ext;
	}

	PG_RETURN_POINTER(n);
}

static void
rt_seg_size(SEG *a, float *size)
{
	if (a == (SEG *) NULL || a->upper <= a->lower)
		*size = 0.0;
	else
		*size = fabsf(a->upper - a->lower);
}

Datum
seg_size(PG_FUNCTION_ARGS)
{
	SEG		   *seg = PG_GETARG_SEG_P(0);

	PG_RETURN_FLOAT4(fabsf(seg->upper - seg->lower));
}


/*****************************************************************************
 *				   Miscellaneous operators
 *
 * 杂项运算符
 *****************************************************************************/
Datum
seg_cmp(PG_FUNCTION_ARGS)
{
	SEG		   *a = PG_GETARG_SEG_P(0);
	SEG		   *b = PG_GETARG_SEG_P(1);

	/*
	 * First compare on lower boundary position
	 *
	 * 首先比较下边界位置
	 */
	if (a->lower < b->lower)
		PG_RETURN_INT32(-1);
	if (a->lower > b->lower)
		PG_RETURN_INT32(1);

	/*
	 * a->lower == b->lower, so consider type of boundary.
	 *
	 * a->lower == b->lower，所以要考虑边界的类型。
	 *
	 * A '-' lower bound is < any other kind (this could only be relevant if
	 * -HUGE_VAL is used as a regular data value). A '<' lower bound is < any
	 * other kind except '-'. A '>' lower bound is > any other kind.
	 *
	 * “-”下限小于任何其他类型（仅当 -HUGE_VAL 用作常规数据值时才相关）。 '<' 下限是 < 除 '-' 之外的任何其他类型。 “>”下界是 > 任何其他类型。
	 */
	if (a->l_ext != b->l_ext)
	{
		if (a->l_ext == '-')
			PG_RETURN_INT32(-1);
		if (b->l_ext == '-')
			PG_RETURN_INT32(1);
		if (a->l_ext == '<')
			PG_RETURN_INT32(-1);
		if (b->l_ext == '<')
			PG_RETURN_INT32(1);
		if (a->l_ext == '>')
			PG_RETURN_INT32(1);
		if (b->l_ext == '>')
			PG_RETURN_INT32(-1);
	}

	/*
	 * For other boundary types, consider # of significant digits first.
	 *
	 * 对于其他边界类型，首先考虑有效数字的数量。
	 */
	if (a->l_sigd < b->l_sigd)	/* (a) is blurred and is likely to include (b) */
		PG_RETURN_INT32(-1);
	if (a->l_sigd > b->l_sigd)	/* (a) is less blurred and is likely to be
								 * included in (b) */
		PG_RETURN_INT32(1);

	/*
	 * For same # of digits, an approximate boundary is more blurred than
	 * exact.
	 *
	 * 对于相同的位数，近似边界比精确边界更模糊。
	 */
	if (a->l_ext != b->l_ext)
	{
		if (a->l_ext == '~')	/* (a) is approximate, while (b) is exact */
			PG_RETURN_INT32(-1);
		if (b->l_ext == '~')
			PG_RETURN_INT32(1);
		/* can't get here unless data is corrupt
		 *
		 * 除非数据损坏否则无法到达此处
		 */
		elog(ERROR, "bogus lower boundary types %d %d",
			 (int) a->l_ext, (int) b->l_ext);
	}

	/* at this point, the lower boundaries are identical
	 *
	 * 此时，下边界是相同的
	 */

	/*
	 * First compare on upper boundary position
	 *
	 * 首先比较上边界位置
	 */
	if (a->upper < b->upper)
		PG_RETURN_INT32(-1);
	if (a->upper > b->upper)
		PG_RETURN_INT32(1);

	/*
	 * a->upper == b->upper, so consider type of boundary.
	 *
	 * a->upper == b->upper，所以要考虑边界的类型。
	 *
	 * A '-' upper bound is > any other kind (this could only be relevant if
	 * HUGE_VAL is used as a regular data value). A '<' upper bound is < any
	 * other kind. A '>' upper bound is > any other kind except '-'.
	 *
	 * “-”上限是 > 任何其他类型（这仅在 HUGE_VAL 用作常规数据值时才相关）。 '<' 上限是 < 任何其他类型。 “>”上限是除“-”之外的任何其他类型。
	 */
	if (a->u_ext != b->u_ext)
	{
		if (a->u_ext == '-')
			PG_RETURN_INT32(1);
		if (b->u_ext == '-')
			PG_RETURN_INT32(-1);
		if (a->u_ext == '<')
			PG_RETURN_INT32(-1);
		if (b->u_ext == '<')
			PG_RETURN_INT32(1);
		if (a->u_ext == '>')
			PG_RETURN_INT32(1);
		if (b->u_ext == '>')
			PG_RETURN_INT32(-1);
	}

	/*
	 * For other boundary types, consider # of significant digits first. Note
	 * result here is converse of the lower-boundary case.
	 *
	 * 对于其他边界类型，首先考虑有效数字的数量。注意这里的结果与下边界情况相反。
	 */
	if (a->u_sigd < b->u_sigd)	/* (a) is blurred and is likely to include (b) */
		PG_RETURN_INT32(1);
	if (a->u_sigd > b->u_sigd)	/* (a) is less blurred and is likely to be
								 * included in (b) */
		PG_RETURN_INT32(-1);

	/*
	 * For same # of digits, an approximate boundary is more blurred than
	 * exact.  Again, result is converse of lower-boundary case.
	 *
	 * 对于相同的位数，近似边界比精确边界更模糊。  同样，结果与下边界情况相反。
	 */
	if (a->u_ext != b->u_ext)
	{
		if (a->u_ext == '~')	/* (a) is approximate, while (b) is exact */
			PG_RETURN_INT32(1);
		if (b->u_ext == '~')
			PG_RETURN_INT32(-1);
		/* can't get here unless data is corrupt
		 *
		 * 除非数据损坏否则无法到达此处
		 */
		elog(ERROR, "bogus upper boundary types %d %d",
			 (int) a->u_ext, (int) b->u_ext);
	}

	PG_RETURN_INT32(0);
}

Datum
seg_lt(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(DirectFunctionCall2(seg_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp < 0);
}

Datum
seg_le(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(DirectFunctionCall2(seg_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
seg_gt(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(DirectFunctionCall2(seg_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp > 0);
}

Datum
seg_ge(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(DirectFunctionCall2(seg_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp >= 0);
}


Datum
seg_different(PG_FUNCTION_ARGS)
{
	int			cmp = DatumGetInt32(DirectFunctionCall2(seg_cmp,
														PG_GETARG_DATUM(0),
														PG_GETARG_DATUM(1)));

	PG_RETURN_BOOL(cmp != 0);
}



/*****************************************************************************
 *				   Auxiliary functions
 *
 * 辅助功能
 *****************************************************************************/

/*
 * The purpose of this routine is to print the given floating point
 * value with exactly n significant digits.  Its behaviour
 * is similar to %.ng except it prints 8.00 where %.ng would
 * print 8.  Returns the length of the string written at "result".
 *
 * 该例程的目的是打印给定的浮点值，其中恰好有 n 个有效数字。  它的行为与 %.ng 类似，只是它打印 8.00，而 %.ng 将打印 8。返回写入“结果”的字符串的长度。
 *
 * Caller must provide a sufficiently large result buffer; 16 bytes
 * should be enough for all known float implementations.
 *
 * 调用者必须提供足够大的结果缓冲区； 16 个字节对于所有已知的浮点实现来说应该足够了。
 */
static int
restore(char *result, float val, int n)
{
	char		buf[25] = {
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '\0'
	};
	char	   *p;
	int			exp;
	int			i,
				dp,
				sign;

	/*
	 * Put a cap on the number of significant digits to avoid garbage in the
	 * output and ensure we don't overrun the result buffer.  (n should not be
	 * negative, but check to protect ourselves against corrupted data.)
	 *
	 * 对有效数字的数量设置上限，以避免输出中出现垃圾，并确保我们不会溢出结果缓冲区。  （n 不应为负数，但应进行检查以保护我们免受数据损坏。）
	 */
	if (n <= 0)
		n = FLT_DIG;
	else
		n = Min(n, FLT_DIG);

	/* remember the sign
	 *
	 * 记住标志
	 */
	sign = (val < 0 ? 1 : 0);

	/* print, in %e style to start with
	 *
	 * 打印，以 %e 样式开始
	 */
	sprintf(result, "%.*e", n - 1, val);

	/* find the exponent
	 *
	 * 找到指数
	 */
	p = strchr(result, 'e');

	/* punt if we have 'inf' or similar
	 *
	 * 如果我们有“inf”或类似的，则弃踢
	 */
	if (p == NULL)
		return strlen(result);

	exp = atoi(p + 1);
	if (exp == 0)
	{
		/* just truncate off the 'e+00'
		 *
		 * 只是截断“e+00”
		 */
		*p = '\0';
	}
	else
	{
		if (abs(exp) <= 4)
		{
			/*
			 * remove the decimal point from the mantissa and write the digits
			 * to the buf array
			 *
			 * 去掉尾数小数点，将数字写入buf数组
			 */
			for (p = result + sign, i = 10, dp = 0; *p != 'e'; p++, i++)
			{
				buf[i] = *p;
				if (*p == '.')
				{
					dp = i--;	/* skip the decimal point */
				}
			}
			if (dp == 0)
				dp = i--;		/* no decimal point was found in the above
								 * for() loop */

			if (exp > 0)
			{
				if (dp - 10 + exp >= n)
				{
					/*
					 * the decimal point is behind the last significant digit;
					 * the digits in between must be converted to the exponent
					 * and the decimal point placed after the first digit
					 *
					 * 小数点位于最后一位有效数字后面；中间的数字必须转换为指数，小数点放在第一位数字之后
					 */
					exp = dp - 10 + exp - n;
					buf[10 + n] = '\0';

					/* insert the decimal point
					 *
					 * 插入小数点
					 */
					if (n > 1)
					{
						dp = 11;
						for (i = 23; i > dp; i--)
							buf[i] = buf[i - 1];
						buf[dp] = '.';
					}

					/*
					 * adjust the exponent by the number of digits after the
					 * decimal point
					 *
					 * 按小数点后的位数调整指数
					 */
					if (n > 1)
						sprintf(&buf[11 + n], "e%d", exp + n - 1);
					else
						sprintf(&buf[11], "e%d", exp + n - 1);

					if (sign)
					{
						buf[9] = '-';
						strcpy(result, &buf[9]);
					}
					else
						strcpy(result, &buf[10]);
				}
				else
				{				/* insert the decimal point */
					dp += exp;
					for (i = 23; i > dp; i--)
						buf[i] = buf[i - 1];
					buf[11 + n] = '\0';
					buf[dp] = '.';
					if (sign)
					{
						buf[9] = '-';
						strcpy(result, &buf[9]);
					}
					else
						strcpy(result, &buf[10]);
				}
			}
			else
			{					/* exp <= 0 */
				dp += exp - 1;
				buf[10 + n] = '\0';
				buf[dp] = '.';
				if (sign)
				{
					buf[dp - 2] = '-';
					strcpy(result, &buf[dp - 2]);
				}
				else
					strcpy(result, &buf[dp - 1]);
			}
		}

		/* do nothing for abs(exp) > 4; %e must be OK
		 *
		 * 对于 abs(exp) > 4 不执行任何操作； %e 一定没问题
		 */
		/* just get rid of zeroes after [eE]- and +zeroes after [Ee].
		 *
		 * 只需去掉 [eE]- 后面的零和 [Ee] 后面的 + 零即可。
		 */

		/* ... this is not done yet.
		 *
		 * ...这还没有完成。
		 */
	}
	return strlen(result);
}


/*
** Miscellany
*
* * 杂项
*/

/* find out the number of significant digits in a string representing
 * a floating point number
 *
 * 浮点数
 */
int
significant_digits(const char *s)
{
	const char *p = s;
	int			n,
				c,
				zeroes;

	zeroes = 1;
	/* skip leading zeroes and sign
	 *
	 * 跳过前导零和符号
	 */
	for (c = *p; (c == '0' || c == '+' || c == '-') && c != 0; c = *(++p));

	/* skip decimal point and following zeroes
	 *
	 * 跳过小数点和后面的零
	 */
	for (c = *p; (c == '0' || c == '.') && c != 0; c = *(++p))
	{
		if (c != '.')
			zeroes++;
	}

	/* count significant digits (n)
	 *
	 * 计算有效数字 (n)
	 */
	for (c = *p, n = 0; c != 0; c = *(++p))
	{
		if (!((c >= '0' && c <= '9') || (c == '.')))
			break;
		if (c != '.')
			n++;
	}

	if (!n)
		return zeroes;

	return n;
}
