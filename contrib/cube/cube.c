/******************************************************************************
  contrib/cube/cube.c

  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
 *
 * 该文件包含可以绑定到 Postgres 后端并由后端在处理查询过程中调用的例程。  这些例程的调用格式由 Postgres 体系结构决定。
******************************************************************************/

#include "postgres.h"

#include <math.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "cubedata.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/float.h"

PG_MODULE_MAGIC_EXT(
					.name = "cube",
					.version = PG_VERSION
);

/*
 * Taken from the intarray contrib header
 *
 * 取自 intarray contrib 标头
 */
#define ARRPTR(x)  ( (double *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))

/*
** Input/Output routines
*
* * 输入/输出例程
*/
PG_FUNCTION_INFO_V1(cube_in);
PG_FUNCTION_INFO_V1(cube_a_f8_f8);
PG_FUNCTION_INFO_V1(cube_a_f8);
PG_FUNCTION_INFO_V1(cube_out);
PG_FUNCTION_INFO_V1(cube_send);
PG_FUNCTION_INFO_V1(cube_recv);
PG_FUNCTION_INFO_V1(cube_f8);
PG_FUNCTION_INFO_V1(cube_f8_f8);
PG_FUNCTION_INFO_V1(cube_c_f8);
PG_FUNCTION_INFO_V1(cube_c_f8_f8);
PG_FUNCTION_INFO_V1(cube_dim);
PG_FUNCTION_INFO_V1(cube_ll_coord);
PG_FUNCTION_INFO_V1(cube_ur_coord);
PG_FUNCTION_INFO_V1(cube_coord);
PG_FUNCTION_INFO_V1(cube_coord_llur);
PG_FUNCTION_INFO_V1(cube_subset);

/*
** GiST support methods
*
* * GiST支持方法
*/

PG_FUNCTION_INFO_V1(g_cube_consistent);
PG_FUNCTION_INFO_V1(g_cube_compress);
PG_FUNCTION_INFO_V1(g_cube_decompress);
PG_FUNCTION_INFO_V1(g_cube_penalty);
PG_FUNCTION_INFO_V1(g_cube_picksplit);
PG_FUNCTION_INFO_V1(g_cube_union);
PG_FUNCTION_INFO_V1(g_cube_same);
PG_FUNCTION_INFO_V1(g_cube_distance);

/*
** B-tree support functions
*
* * B树支持函数
*/
PG_FUNCTION_INFO_V1(cube_eq);
PG_FUNCTION_INFO_V1(cube_ne);
PG_FUNCTION_INFO_V1(cube_lt);
PG_FUNCTION_INFO_V1(cube_gt);
PG_FUNCTION_INFO_V1(cube_le);
PG_FUNCTION_INFO_V1(cube_ge);
PG_FUNCTION_INFO_V1(cube_cmp);

/*
** R-tree support functions
*
* * R树支持函数
*/

PG_FUNCTION_INFO_V1(cube_contains);
PG_FUNCTION_INFO_V1(cube_contained);
PG_FUNCTION_INFO_V1(cube_overlap);
PG_FUNCTION_INFO_V1(cube_union);
PG_FUNCTION_INFO_V1(cube_inter);
PG_FUNCTION_INFO_V1(cube_size);

/*
** miscellaneous
*
* * 各种各样的
*/
PG_FUNCTION_INFO_V1(distance_taxicab);
PG_FUNCTION_INFO_V1(cube_distance);
PG_FUNCTION_INFO_V1(distance_chebyshev);
PG_FUNCTION_INFO_V1(cube_is_point);
PG_FUNCTION_INFO_V1(cube_enlarge);

/*
** For internal use only
*
* * 仅供内部使用
*/
int32		cube_cmp_v0(NDBOX *a, NDBOX *b);
bool		cube_contains_v0(NDBOX *a, NDBOX *b);
bool		cube_overlap_v0(NDBOX *a, NDBOX *b);
NDBOX	   *cube_union_v0(NDBOX *a, NDBOX *b);
void		rt_cube_size(NDBOX *a, double *size);
NDBOX	   *g_cube_binary_union(NDBOX *r1, NDBOX *r2, int *sizep);
bool		g_cube_leaf_consistent(NDBOX *key, NDBOX *query, StrategyNumber strategy);
bool		g_cube_internal_consistent(NDBOX *key, NDBOX *query, StrategyNumber strategy);

/*
** Auxiliary functions
*
* * 辅助功能
*/
static double distance_1D(double a1, double a2, double b1, double b2);
static bool cube_is_point_internal(NDBOX *cube);


/*****************************************************************************
 * Input/Output functions
 *
 * 输入/输出功能
 *****************************************************************************/

/* NdBox = [(lowerleft),(upperright)]
 *
 * NdBox = [(左下),(右上角)]
 */
/* [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))]
 *
 * [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))]
 *
 * [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))]
 */
Datum
cube_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	NDBOX	   *result;
	Size		scanbuflen;
	yyscan_t	scanner;

	cube_scanner_init(str, &scanbuflen, &scanner);

	cube_yyparse(&result, scanbuflen, fcinfo->context, scanner);

	/* We might as well run this even on failure.
	 *
	 * 即使失败我们也可以运行它。
	 */
	cube_scanner_finish(scanner);

	PG_RETURN_NDBOX_P(result);
}


/*
** Allows the construction of a cube from 2 float[]'s
*
* * 允许用 2 个 float[] 构造一个立方体
*/
Datum
cube_a_f8_f8(PG_FUNCTION_ARGS)
{
	ArrayType  *ur = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *ll = PG_GETARG_ARRAYTYPE_P(1);
	NDBOX	   *result;
	int			i;
	int			dim;
	int			size;
	bool		point;
	double	   *dur,
			   *dll;

	if (array_contains_nulls(ur) || array_contains_nulls(ll))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("cannot work with arrays containing NULLs")));

	dim = ARRNELEMS(ur);
	if (dim > CUBE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("can't extend cube"),
				 errdetail("A cube cannot have more than %d dimensions.",
						   CUBE_MAX_DIM)));

	if (ARRNELEMS(ll) != dim)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("UR and LL arrays must be of same length")));

	dur = ARRPTR(ur);
	dll = ARRPTR(ll);

	/* Check if it's a point
	 *
	 * 检查是否是一个点
	 */
	point = true;
	for (i = 0; i < dim; i++)
	{
		if (dur[i] != dll[i])
		{
			point = false;
			break;
		}
	}

	size = point ? POINT_SIZE(dim) : CUBE_SIZE(dim);
	result = (NDBOX *) palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, dim);

	for (i = 0; i < dim; i++)
		result->x[i] = dur[i];

	if (!point)
	{
		for (i = 0; i < dim; i++)
			result->x[i + dim] = dll[i];
	}
	else
		SET_POINT_BIT(result);

	PG_RETURN_NDBOX_P(result);
}

/*
** Allows the construction of a zero-volume cube from a float[]
*
* * 允许从 float[] 构造零体积立方体
*/
Datum
cube_a_f8(PG_FUNCTION_ARGS)
{
	ArrayType  *ur = PG_GETARG_ARRAYTYPE_P(0);
	NDBOX	   *result;
	int			i;
	int			dim;
	int			size;
	double	   *dur;

	if (array_contains_nulls(ur))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("cannot work with arrays containing NULLs")));

	dim = ARRNELEMS(ur);
	if (dim > CUBE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("array is too long"),
				 errdetail("A cube cannot have more than %d dimensions.",
						   CUBE_MAX_DIM)));

	dur = ARRPTR(ur);

	size = POINT_SIZE(dim);
	result = (NDBOX *) palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, dim);
	SET_POINT_BIT(result);

	for (i = 0; i < dim; i++)
		result->x[i] = dur[i];

	PG_RETURN_NDBOX_P(result);
}

Datum
cube_subset(PG_FUNCTION_ARGS)
{
	NDBOX	   *c = PG_GETARG_NDBOX_P(0);
	ArrayType  *idx = PG_GETARG_ARRAYTYPE_P(1);
	NDBOX	   *result;
	int			size,
				dim,
				i;
	int		   *dx;

	if (array_contains_nulls(idx))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("cannot work with arrays containing NULLs")));

	dx = (int32 *) ARR_DATA_PTR(idx);

	dim = ARRNELEMS(idx);
	if (dim > CUBE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("array is too long"),
				 errdetail("A cube cannot have more than %d dimensions.",
						   CUBE_MAX_DIM)));

	size = IS_POINT(c) ? POINT_SIZE(dim) : CUBE_SIZE(dim);
	result = (NDBOX *) palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, dim);

	if (IS_POINT(c))
		SET_POINT_BIT(result);

	for (i = 0; i < dim; i++)
	{
		if ((dx[i] <= 0) || (dx[i] > DIM(c)))
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
					 errmsg("Index out of bounds")));
		result->x[i] = c->x[dx[i] - 1];
		if (!IS_POINT(c))
			result->x[i + dim] = c->x[dx[i] + DIM(c) - 1];
	}

	PG_FREE_IF_COPY(c, 0);
	PG_RETURN_NDBOX_P(result);
}

Datum
cube_out(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	StringInfoData buf;
	int			dim = DIM(cube);
	int			i;

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');
	for (i = 0; i < dim; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, float8out_internal(LL_COORD(cube, i)));
	}
	appendStringInfoChar(&buf, ')');

	if (!cube_is_point_internal(cube))
	{
		appendStringInfoString(&buf, ",(");
		for (i = 0; i < dim; i++)
		{
			if (i > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, float8out_internal(UR_COORD(cube, i)));
		}
		appendStringInfoChar(&buf, ')');
	}

	PG_FREE_IF_COPY(cube, 0);
	PG_RETURN_CSTRING(buf.data);
}

/*
 * cube_send - a binary output handler for cube type
 *
 * cube_send - 立方体类型的二进制输出处理程序
 */
Datum
cube_send(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	StringInfoData buf;
	int32		i,
				nitems = DIM(cube);

	pq_begintypsend(&buf);
	pq_sendint32(&buf, cube->header);
	if (!IS_POINT(cube))
		nitems += nitems;
	/* for symmetry with cube_recv, we don't use LL_COORD/UR_COORD here
	 *
	 * 为了与cube_recv对称，我们在这里不使用LL_COORD/UR_COORD
	 */
	for (i = 0; i < nitems; i++)
		pq_sendfloat8(&buf, cube->x[i]);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * cube_recv - a binary input handler for cube type
 *
 * cube_recv - 立方体类型的二进制输入处理程序
 */
Datum
cube_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		header;
	int32		i,
				nitems;
	NDBOX	   *cube;

	header = pq_getmsgint(buf, sizeof(int32));
	nitems = (header & DIM_MASK);
	if (nitems > CUBE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cube dimension is too large"),
				 errdetail("A cube cannot have more than %d dimensions.",
						   CUBE_MAX_DIM)));
	if ((header & POINT_BIT) == 0)
		nitems += nitems;
	cube = palloc(offsetof(NDBOX, x) + sizeof(double) * nitems);
	SET_VARSIZE(cube, offsetof(NDBOX, x) + sizeof(double) * nitems);
	cube->header = header;
	for (i = 0; i < nitems; i++)
		cube->x[i] = pq_getmsgfloat8(buf);

	PG_RETURN_NDBOX_P(cube);
}


/*****************************************************************************
 *						   GiST functions
 *
 * GiST 函数
 *****************************************************************************/

/*
** The GiST Consistent method for boxes
** Should return false if for all data items x below entry,
** the predicate x op query == false, where op is the oper
** corresponding to strategy in the pg_amop table.
*
* * 框的 GiST 一致性方法 * 如果对于条目下面的所有数据项 x，则应返回 false， * 谓词 x op 查询 == false，其中 op 是与 pg_amop 表中的策略相对应的操作。
*/
Datum
g_cube_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	NDBOX	   *query = PG_GETARG_NDBOX_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3);
	 *
	 * Oid 子类型 = PG_GETARG_OID(3);
	 */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		res;

	/* All cases served by this function are exact
	 *
	 * 该函数服务的所有案例都是准确的
	 */
	*recheck = false;

	/*
	 * if entry is not leaf, use g_cube_internal_consistent, else use
	 * g_cube_leaf_consistent
	 *
	 * 如果条目不是叶子，则使用 g_cube_internal_concient，否则使用 g_cube_leaf_concient
	 */
	if (GIST_LEAF(entry))
		res = g_cube_leaf_consistent(DatumGetNDBOXP(entry->key),
									 query, strategy);
	else
		res = g_cube_internal_consistent(DatumGetNDBOXP(entry->key),
										 query, strategy);

	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}


/*
** The GiST Union method for boxes
** returns the minimal bounding box that encloses all the entries in entryvec
*
* * 框的 GiST Union 方法 * 返回包含 Entryvec 中所有条目的最小边界框
*/
Datum
g_cube_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	NDBOX	   *out = (NDBOX *) NULL;
	NDBOX	   *tmp;
	int			i;

	tmp = DatumGetNDBOXP(entryvec->vector[0].key);

	/*
	 * sizep = sizeof(NDBOX); -- NDBOX has variable size
	 *
	 * sizep = sizeof(NDBOX); -- NDBOX 具有可变大小
	 */
	*sizep = VARSIZE(tmp);

	for (i = 1; i < entryvec->n; i++)
	{
		out = g_cube_binary_union(tmp,
								  DatumGetNDBOXP(entryvec->vector[i].key),
								  sizep);
		tmp = out;
	}

	PG_RETURN_POINTER(out);
}

/*
** GiST Compress and Decompress methods for boxes
** do not do anything.
*
* * 盒子的 GiST 压缩和解压缩方法 * 不执行任何操作。
*/

Datum
g_cube_compress(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

Datum
g_cube_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	NDBOX	   *key = DatumGetNDBOXP(entry->key);

	if (key != DatumGetNDBOXP(entry->key))
	{
		GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);
		PG_RETURN_POINTER(retval);
	}
	PG_RETURN_POINTER(entry);
}


/*
** The GiST Penalty method for boxes
** As in the R-tree paper, we use change in area as our penalty metric
*
* * 盒子的 GiST 惩罚方法 * 与 R 树论文中一样，我们使用面积变化作为惩罚指标
*/
Datum
g_cube_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	NDBOX	   *ud;
	double		tmp1,
				tmp2;

	ud = cube_union_v0(DatumGetNDBOXP(origentry->key),
					   DatumGetNDBOXP(newentry->key));
	rt_cube_size(ud, &tmp1);
	rt_cube_size(DatumGetNDBOXP(origentry->key), &tmp2);
	*result = (float) (tmp1 - tmp2);

	PG_RETURN_FLOAT8(*result);
}



/*
** The GiST PickSplit method for boxes
** We use Guttman's poly time split algorithm
*
* * 盒子的 GiST PickSplit 方法 * 我们使用 Guttman 的多时间分割算法
*/
Datum
g_cube_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber i,
				j;
	NDBOX	   *datum_alpha,
			   *datum_beta;
	NDBOX	   *datum_l,
			   *datum_r;
	NDBOX	   *union_d,
			   *union_dl,
			   *union_dr;
	NDBOX	   *inter_d;
	bool		firsttime;
	double		size_alpha,
				size_beta,
				size_union,
				size_inter;
	double		size_waste,
				waste;
	double		size_l,
				size_r;
	int			nbytes;
	OffsetNumber seed_1 = 1,
				seed_2 = 2;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;

	maxoff = entryvec->n - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	firsttime = true;
	waste = 0.0;

	for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i))
	{
		datum_alpha = DatumGetNDBOXP(entryvec->vector[i].key);
		for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j))
		{
			datum_beta = DatumGetNDBOXP(entryvec->vector[j].key);

			/* compute the wasted space by unioning these guys
			 *
			 * 通过联合这些人来计算浪费的空间
			 */
			/* size_waste = size_union - size_inter;
			 *
			 * size_waste = size_union - size_inter；
			 *
			 * size_waste = size_union - size_inter；
			 */
			union_d = cube_union_v0(datum_alpha, datum_beta);
			rt_cube_size(union_d, &size_union);
			inter_d = DatumGetNDBOXP(DirectFunctionCall2(cube_inter,
														 entryvec->vector[i].key,
														 entryvec->vector[j].key));
			rt_cube_size(inter_d, &size_inter);
			size_waste = size_union - size_inter;

			/*
			 * are these a more promising split than what we've already seen?
			 *
			 * 这些分裂是否比我们已经看到的更有希望？
			 */

			if (size_waste > waste || firsttime)
			{
				waste = size_waste;
				seed_1 = i;
				seed_2 = j;
				firsttime = false;
			}
		}
	}

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	datum_alpha = DatumGetNDBOXP(entryvec->vector[seed_1].key);
	datum_l = cube_union_v0(datum_alpha, datum_alpha);
	rt_cube_size(datum_l, &size_l);
	datum_beta = DatumGetNDBOXP(entryvec->vector[seed_2].key);
	datum_r = cube_union_v0(datum_beta, datum_beta);
	rt_cube_size(datum_r, &size_r);

	/*
	 * Now split up the regions between the two seeds.  An important property
	 * of this split algorithm is that the split vector v has the indices of
	 * items to be split in order in its left and right vectors.  We exploit
	 * this property by doing a merge in the code that actually splits the
	 * page.
	 *
	 * 现在分割两个种子之间的区域。  该分割算法的一个重要属性是分割向量 v 在其左向量和右向量中具有要按顺序分割的项目的索引。  我们通过在实际拆分页面的代码中进行合并来利用此属性。
	 *
	 * For efficiency, we also place the new index tuple in this loop. This is
	 * handled at the very end, when we have placed all the existing tuples
	 * and i == maxoff + 1.
	 *
	 * 为了提高效率，我们还将新的索引元组放入此循环中。这是在最后处理的，当我们放置了所有现有的元组并且 i == maxoff + 1 时。
	 */

	maxoff = OffsetNumberNext(maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		/*
		 * If we've already decided where to place this item, just put it on
		 * the right list.  Otherwise, we need to figure out which page needs
		 * the least enlargement in order to store the item.
		 *
		 * 如果我们已经决定放置此项目的位置，只需将其放在正确的列表中即可。  否则，我们需要找出哪一页需要最小的放大才能存储该项目。
		 */

		if (i == seed_1)
		{
			*left++ = i;
			v->spl_nleft++;
			continue;
		}
		else if (i == seed_2)
		{
			*right++ = i;
			v->spl_nright++;
			continue;
		}

		/* okay, which page needs least enlargement?
		 *
		 * 好的，哪一页需要最小放大？
		 */
		datum_alpha = DatumGetNDBOXP(entryvec->vector[i].key);
		union_dl = cube_union_v0(datum_l, datum_alpha);
		union_dr = cube_union_v0(datum_r, datum_alpha);
		rt_cube_size(union_dl, &size_alpha);
		rt_cube_size(union_dr, &size_beta);

		/* pick which page to add it to
		 *
		 * 选择要将其添加到哪个页面
		 */
		if (size_alpha - size_l < size_beta - size_r)
		{
			datum_l = union_dl;
			size_l = size_alpha;
			*left++ = i;
			v->spl_nleft++;
		}
		else
		{
			datum_r = union_dr;
			size_r = size_beta;
			*right++ = i;
			v->spl_nright++;
		}
	}
	*left = *right = FirstOffsetNumber; /* sentinel value */

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}

/*
** Equality method
*
* * 平等法
*/
Datum
g_cube_same(PG_FUNCTION_ARGS)
{
	NDBOX	   *b1 = PG_GETARG_NDBOX_P(0);
	NDBOX	   *b2 = PG_GETARG_NDBOX_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	if (cube_cmp_v0(b1, b2) == 0)
		*result = true;
	else
		*result = false;

	PG_RETURN_NDBOX_P(result);
}

/*
** SUPPORT ROUTINES
*
* * 支持例程
*/
bool
g_cube_leaf_consistent(NDBOX *key,
					   NDBOX *query,
					   StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = cube_overlap_v0(key, query);
			break;
		case RTSameStrategyNumber:
			retval = (cube_cmp_v0(key, query) == 0);
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = cube_contains_v0(key, query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval = cube_contains_v0(query, key);
			break;
		default:
			retval = false;
	}
	return retval;
}

bool
g_cube_internal_consistent(NDBOX *key,
						   NDBOX *query,
						   StrategyNumber strategy)
{
	bool		retval;

	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = (bool) cube_overlap_v0(key, query);
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = (bool) cube_contains_v0(key, query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval = (bool) cube_overlap_v0(key, query);
			break;
		default:
			retval = false;
	}
	return retval;
}

NDBOX *
g_cube_binary_union(NDBOX *r1, NDBOX *r2, int *sizep)
{
	NDBOX	   *retval;

	retval = cube_union_v0(r1, r2);
	*sizep = VARSIZE(retval);

	return retval;
}


/* cube_union_v0 */
NDBOX *
cube_union_v0(NDBOX *a, NDBOX *b)
{
	int			i;
	NDBOX	   *result;
	int			dim;
	int			size;

	/* trivial case
	 *
	 * 小事
	 */
	if (a == b)
		return a;

	/* swap the arguments if needed, so that 'a' is always larger than 'b'
	 *
	 * 如果需要，交换参数，以便“a”始终大于“b”
	 */
	if (DIM(a) < DIM(b))
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
	}
	dim = DIM(a);

	size = CUBE_SIZE(dim);
	result = palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, dim);

	/* First compute the union of the dimensions present in both args
	 *
	 * 首先计算两个参数中存在的维度的并集
	 */
	for (i = 0; i < DIM(b); i++)
	{
		result->x[i] = Min(Min(LL_COORD(a, i), UR_COORD(a, i)),
						   Min(LL_COORD(b, i), UR_COORD(b, i)));
		result->x[i + DIM(a)] = Max(Max(LL_COORD(a, i), UR_COORD(a, i)),
									Max(LL_COORD(b, i), UR_COORD(b, i)));
	}
	/* continue on the higher dimensions only present in 'a'
	 *
	 * 继续仅存在于“a”中的更高维度
	 */
	for (; i < DIM(a); i++)
	{
		result->x[i] = Min(0,
						   Min(LL_COORD(a, i), UR_COORD(a, i))
			);
		result->x[i + dim] = Max(0,
								 Max(LL_COORD(a, i), UR_COORD(a, i))
			);
	}

	/*
	 * Check if the result was in fact a point, and set the flag in the datum
	 * accordingly. (we don't bother to repalloc it smaller)
	 *
	 * 检查结果是否确实是一个点，并相应地在数据中设置标志。 （我们不费心将其重新分配得更小）
	 */
	if (cube_is_point_internal(result))
	{
		size = POINT_SIZE(dim);
		SET_VARSIZE(result, size);
		SET_POINT_BIT(result);
	}

	return result;
}

Datum
cube_union(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0);
	NDBOX	   *b = PG_GETARG_NDBOX_P(1);
	NDBOX	   *res;

	res = cube_union_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_NDBOX_P(res);
}

/* cube_inter */
Datum
cube_inter(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0);
	NDBOX	   *b = PG_GETARG_NDBOX_P(1);
	NDBOX	   *result;
	bool		swapped = false;
	int			i;
	int			dim;
	int			size;

	/* swap the arguments if needed, so that 'a' is always larger than 'b'
	 *
	 * 如果需要，交换参数，以便“a”始终大于“b”
	 */
	if (DIM(a) < DIM(b))
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
		swapped = true;
	}
	dim = DIM(a);

	size = CUBE_SIZE(dim);
	result = (NDBOX *) palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, dim);

	/* First compute intersection of the dimensions present in both args
	 *
	 * 首先计算两个参数中存在的维度的交集
	 */
	for (i = 0; i < DIM(b); i++)
	{
		result->x[i] = Max(Min(LL_COORD(a, i), UR_COORD(a, i)),
						   Min(LL_COORD(b, i), UR_COORD(b, i)));
		result->x[i + DIM(a)] = Min(Max(LL_COORD(a, i), UR_COORD(a, i)),
									Max(LL_COORD(b, i), UR_COORD(b, i)));
	}
	/* continue on the higher dimensions only present in 'a'
	 *
	 * 继续仅存在于“a”中的更高维度
	 */
	for (; i < DIM(a); i++)
	{
		result->x[i] = Max(0,
						   Min(LL_COORD(a, i), UR_COORD(a, i))
			);
		result->x[i + DIM(a)] = Min(0,
									Max(LL_COORD(a, i), UR_COORD(a, i))
			);
	}

	/*
	 * Check if the result was in fact a point, and set the flag in the datum
	 * accordingly. (we don't bother to repalloc it smaller)
	 *
	 * 检查结果是否确实是一个点，并相应地在数据中设置标志。 （我们不费心将其重新分配得更小）
	 */
	if (cube_is_point_internal(result))
	{
		size = POINT_SIZE(dim);
		result = repalloc(result, size);
		SET_VARSIZE(result, size);
		SET_POINT_BIT(result);
	}

	if (swapped)
	{
		PG_FREE_IF_COPY(b, 0);
		PG_FREE_IF_COPY(a, 1);
	}
	else
	{
		PG_FREE_IF_COPY(a, 0);
		PG_FREE_IF_COPY(b, 1);
	}

	/*
	 * Is it OK to return a non-null intersection for non-overlapping boxes?
	 *
	 * 是否可以为非重叠框返回非空交集？
	 */
	PG_RETURN_NDBOX_P(result);
}

/* cube_size */
Datum
cube_size(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0);
	double		result;

	rt_cube_size(a, &result);
	PG_FREE_IF_COPY(a, 0);
	PG_RETURN_FLOAT8(result);
}

void
rt_cube_size(NDBOX *a, double *size)
{
	double		result;
	int			i;

	if (a == (NDBOX *) NULL)
	{
		/* special case for GiST
		 *
		 * GiST 的特殊情况
		 */
		result = 0.0;
	}
	else if (IS_POINT(a) || DIM(a) == 0)
	{
		/* necessarily has zero size
		 *
		 * 大小必然为零
		 */
		result = 0.0;
	}
	else
	{
		result = 1.0;
		for (i = 0; i < DIM(a); i++)
			result *= fabs(UR_COORD(a, i) - LL_COORD(a, i));
	}
	*size = result;
}

/* make up a metric in which one box will be 'lower' than the other
   -- this can be useful for sorting and to determine uniqueness */
int32
cube_cmp_v0(NDBOX *a, NDBOX *b)
{
	int			i;
	int			dim;

	dim = Min(DIM(a), DIM(b));

	/* compare the common dimensions
	 *
	 * 比较常用尺寸
	 */
	for (i = 0; i < dim; i++)
	{
		if (Min(LL_COORD(a, i), UR_COORD(a, i)) >
			Min(LL_COORD(b, i), UR_COORD(b, i)))
			return 1;
		if (Min(LL_COORD(a, i), UR_COORD(a, i)) <
			Min(LL_COORD(b, i), UR_COORD(b, i)))
			return -1;
	}
	for (i = 0; i < dim; i++)
	{
		if (Max(LL_COORD(a, i), UR_COORD(a, i)) >
			Max(LL_COORD(b, i), UR_COORD(b, i)))
			return 1;
		if (Max(LL_COORD(a, i), UR_COORD(a, i)) <
			Max(LL_COORD(b, i), UR_COORD(b, i)))
			return -1;
	}

	/* compare extra dimensions to zero
	 *
	 * 将额外维度与零进行比较
	 */
	if (DIM(a) > DIM(b))
	{
		for (i = dim; i < DIM(a); i++)
		{
			if (Min(LL_COORD(a, i), UR_COORD(a, i)) > 0)
				return 1;
			if (Min(LL_COORD(a, i), UR_COORD(a, i)) < 0)
				return -1;
		}
		for (i = dim; i < DIM(a); i++)
		{
			if (Max(LL_COORD(a, i), UR_COORD(a, i)) > 0)
				return 1;
			if (Max(LL_COORD(a, i), UR_COORD(a, i)) < 0)
				return -1;
		}

		/*
		 * if all common dimensions are equal, the cube with more dimensions
		 * wins
		 *
		 * 如果所有公共维度都相等，则维度更多的立方体获胜
		 */
		return 1;
	}
	if (DIM(a) < DIM(b))
	{
		for (i = dim; i < DIM(b); i++)
		{
			if (Min(LL_COORD(b, i), UR_COORD(b, i)) > 0)
				return -1;
			if (Min(LL_COORD(b, i), UR_COORD(b, i)) < 0)
				return 1;
		}
		for (i = dim; i < DIM(b); i++)
		{
			if (Max(LL_COORD(b, i), UR_COORD(b, i)) > 0)
				return -1;
			if (Max(LL_COORD(b, i), UR_COORD(b, i)) < 0)
				return 1;
		}

		/*
		 * if all common dimensions are equal, the cube with more dimensions
		 * wins
		 *
		 * 如果所有公共维度都相等，则维度更多的立方体获胜
		 */
		return -1;
	}

	/* They're really equal
	 *
	 * 他们真的是平等的
	 */
	return 0;
}

Datum
cube_cmp(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(res);
}


Datum
cube_eq(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res == 0);
}


Datum
cube_ne(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res != 0);
}


Datum
cube_lt(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res < 0);
}


Datum
cube_gt(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res > 0);
}


Datum
cube_le(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res <= 0);
}


Datum
cube_ge(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	int32		res;

	res = cube_cmp_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res >= 0);
}


/* Contains */
/* Box(A) CONTAINS Box(B) IFF pt(A) < pt(B)
 *
 * 方框(A)包含方框(B) IFF pt(A) < pt(B)
 */
bool
cube_contains_v0(NDBOX *a, NDBOX *b)
{
	int			i;

	if ((a == NULL) || (b == NULL))
		return false;

	if (DIM(a) < DIM(b))
	{
		/*
		 * the further comparisons will make sense if the excess dimensions of
		 * (b) were zeroes Since both UL and UR coordinates must be zero, we
		 * can check them all without worrying about which is which.
		 *
		 * 如果 (b) 的多余尺寸为零，则进一步的比较将有意义。由于 UL 和 UR 坐标都必须为零，因此我们可以检查它们，而不必担心哪个是哪个。
		 */
		for (i = DIM(a); i < DIM(b); i++)
		{
			if (LL_COORD(b, i) != 0)
				return false;
			if (UR_COORD(b, i) != 0)
				return false;
		}
	}

	/* Can't care less about the excess dimensions of (a), if any
	 *
	 * 不在乎（a）的多余尺寸（如果有的话）
	 */
	for (i = 0; i < Min(DIM(a), DIM(b)); i++)
	{
		if (Min(LL_COORD(a, i), UR_COORD(a, i)) >
			Min(LL_COORD(b, i), UR_COORD(b, i)))
			return false;
		if (Max(LL_COORD(a, i), UR_COORD(a, i)) <
			Max(LL_COORD(b, i), UR_COORD(b, i)))
			return false;
	}

	return true;
}

Datum
cube_contains(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	bool		res;

	res = cube_contains_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res);
}

/* Contained */
/* Box(A) Contained by Box(B) IFF Box(B) Contains Box(A)
 *
 * 框(A) 包含框(B) IFF 框(B) 包含框(A)
 */
Datum
cube_contained(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	bool		res;

	res = cube_contains_v0(b, a);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res);
}

/* Overlap */
/* Box(A) Overlap Box(B) IFF (pt(a)LL < pt(B)UR) && (pt(b)LL < pt(a)UR)
 *
 * 框(A) 重叠框(B) IFF (pt(a)LL < pt(B)UR) && (pt(b)LL < pt(a)UR)
 */
bool
cube_overlap_v0(NDBOX *a, NDBOX *b)
{
	int			i;

	if ((a == NULL) || (b == NULL))
		return false;

	/* swap the box pointers if needed
	 *
	 * 如果需要，交换框指针
	 */
	if (DIM(a) < DIM(b))
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
	}

	/* compare within the dimensions of (b)
	 *
	 * 在 (b) 的范围内进行比较
	 */
	for (i = 0; i < DIM(b); i++)
	{
		if (Min(LL_COORD(a, i), UR_COORD(a, i)) > Max(LL_COORD(b, i), UR_COORD(b, i)))
			return false;
		if (Max(LL_COORD(a, i), UR_COORD(a, i)) < Min(LL_COORD(b, i), UR_COORD(b, i)))
			return false;
	}

	/* compare to zero those dimensions in (a) absent in (b)
	 *
	 * 将 (a) 中 (b) 中不存在的那些维度与零进行比较
	 */
	for (i = DIM(b); i < DIM(a); i++)
	{
		if (Min(LL_COORD(a, i), UR_COORD(a, i)) > 0)
			return false;
		if (Max(LL_COORD(a, i), UR_COORD(a, i)) < 0)
			return false;
	}

	return true;
}


Datum
cube_overlap(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	bool		res;

	res = cube_overlap_v0(a, b);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_BOOL(res);
}


/* Distance */
/* The distance is computed as a per axis sum of the squared distances
   between 1D projections of the boxes onto Cartesian axes. Assuming zero
   distance between overlapping projections, this metric coincides with the
 *
 * 盒子在笛卡尔轴上的一维投影之间。假设重叠投影之间的距离为零，该度量与
   "common sense" geometric distance */
Datum
cube_distance(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	bool		swapped = false;
	double		d,
				distance;
	int			i;

	/* swap the box pointers if needed
	 *
	 * 如果需要，交换框指针
	 */
	if (DIM(a) < DIM(b))
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
		swapped = true;
	}

	distance = 0.0;
	/* compute within the dimensions of (b)
	 *
	 * 在 (b) 的维度内计算
	 */
	for (i = 0; i < DIM(b); i++)
	{
		d = distance_1D(LL_COORD(a, i), UR_COORD(a, i), LL_COORD(b, i), UR_COORD(b, i));
		distance += d * d;
	}

	/* compute distance to zero for those dimensions in (a) absent in (b)
	 *
	 * 计算 (a) 中 (b) 中不存在的维度到零的距离
	 */
	for (i = DIM(b); i < DIM(a); i++)
	{
		d = distance_1D(LL_COORD(a, i), UR_COORD(a, i), 0.0, 0.0);
		distance += d * d;
	}

	if (swapped)
	{
		PG_FREE_IF_COPY(b, 0);
		PG_FREE_IF_COPY(a, 1);
	}
	else
	{
		PG_FREE_IF_COPY(a, 0);
		PG_FREE_IF_COPY(b, 1);
	}

	PG_RETURN_FLOAT8(sqrt(distance));
}

Datum
distance_taxicab(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	bool		swapped = false;
	double		distance;
	int			i;

	/* swap the box pointers if needed
	 *
	 * 如果需要，交换框指针
	 */
	if (DIM(a) < DIM(b))
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
		swapped = true;
	}

	distance = 0.0;
	/* compute within the dimensions of (b)
	 *
	 * 在 (b) 的维度内计算
	 */
	for (i = 0; i < DIM(b); i++)
		distance += fabs(distance_1D(LL_COORD(a, i), UR_COORD(a, i),
									 LL_COORD(b, i), UR_COORD(b, i)));

	/* compute distance to zero for those dimensions in (a) absent in (b)
	 *
	 * 计算 (a) 中 (b) 中不存在的维度到零的距离
	 */
	for (i = DIM(b); i < DIM(a); i++)
		distance += fabs(distance_1D(LL_COORD(a, i), UR_COORD(a, i),
									 0.0, 0.0));

	if (swapped)
	{
		PG_FREE_IF_COPY(b, 0);
		PG_FREE_IF_COPY(a, 1);
	}
	else
	{
		PG_FREE_IF_COPY(a, 0);
		PG_FREE_IF_COPY(b, 1);
	}

	PG_RETURN_FLOAT8(distance);
}

Datum
distance_chebyshev(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0),
			   *b = PG_GETARG_NDBOX_P(1);
	bool		swapped = false;
	double		d,
				distance;
	int			i;

	/* swap the box pointers if needed
	 *
	 * 如果需要，交换框指针
	 */
	if (DIM(a) < DIM(b))
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
		swapped = true;
	}

	distance = 0.0;
	/* compute within the dimensions of (b)
	 *
	 * 在 (b) 的维度内计算
	 */
	for (i = 0; i < DIM(b); i++)
	{
		d = fabs(distance_1D(LL_COORD(a, i), UR_COORD(a, i),
							 LL_COORD(b, i), UR_COORD(b, i)));
		if (d > distance)
			distance = d;
	}

	/* compute distance to zero for those dimensions in (a) absent in (b)
	 *
	 * 计算 (a) 中 (b) 中不存在的维度到零的距离
	 */
	for (i = DIM(b); i < DIM(a); i++)
	{
		d = fabs(distance_1D(LL_COORD(a, i), UR_COORD(a, i), 0.0, 0.0));
		if (d > distance)
			distance = d;
	}

	if (swapped)
	{
		PG_FREE_IF_COPY(b, 0);
		PG_FREE_IF_COPY(a, 1);
	}
	else
	{
		PG_FREE_IF_COPY(a, 0);
		PG_FREE_IF_COPY(b, 1);
	}

	PG_RETURN_FLOAT8(distance);
}

Datum
g_cube_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	NDBOX	   *cube = DatumGetNDBOXP(entry->key);
	double		retval;

	if (strategy == CubeKNNDistanceCoord)
	{
		/*
		 * Handle ordering by ~> operator.  See comments of cube_coord_llur()
		 * for details
		 *
		 * 通过 ~> 操作符处理排序。  详细信息请参见cube_coord_llur()的注释
		 */
		int			coord = PG_GETARG_INT32(1);
		bool		isLeaf = GistPageIsLeaf(entry->page);
		bool		inverse = false;

		/* 0 is the only unsupported coordinate value
		 *
		 * 0 是唯一不受支持的坐标值
		 */
		if (coord == 0)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
					 errmsg("zero cube index is not defined")));

		/* Return inversed value for negative coordinate
		 *
		 * 返回负坐标的反转值
		 */
		if (coord < 0)
		{
			coord = -coord;
			inverse = true;
		}

		if (coord <= 2 * DIM(cube))
		{
			/* dimension index
			 *
			 * 维度索引
			 */
			int			index = (coord - 1) / 2;

			/* whether this is upper bound (lower bound otherwise)
			 *
			 * 这是否是上限（否则是下限）
			 */
			bool		upper = ((coord - 1) % 2 == 1);

			if (IS_POINT(cube))
			{
				retval = cube->x[index];
			}
			else
			{
				if (isLeaf)
				{
					/* For leaf just return required upper/lower bound
					 *
					 * 对于叶子，只需返回所需的上限/下限
					 */
					if (upper)
						retval = Max(cube->x[index], cube->x[index + DIM(cube)]);
					else
						retval = Min(cube->x[index], cube->x[index + DIM(cube)]);
				}
				else
				{
					/*
					 * For non-leaf we should always return lower bound,
					 * because even upper bound of a child in the subtree can
					 * be as small as our lower bound.  For inversed case we
					 * return upper bound because it becomes lower bound for
					 * inversed value.
					 *
					 * 对于非叶子，我们应该始终返回下界，因为即使子树中子树的上界也可以与下界一样小。  对于反转的情况，我们返回上限，因为它成为反转值的下界。
					 */
					if (!inverse)
						retval = Min(cube->x[index], cube->x[index + DIM(cube)]);
					else
						retval = Max(cube->x[index], cube->x[index + DIM(cube)]);
				}
			}
		}
		else
		{
			retval = 0.0;
		}

		/* Inverse return value if needed
		 *
		 * 如果需要则返回相反值
		 */
		if (inverse)
			retval = -retval;
	}
	else
	{
		NDBOX	   *query = PG_GETARG_NDBOX_P(1);

		switch (strategy)
		{
			case CubeKNNDistanceTaxicab:
				retval = DatumGetFloat8(DirectFunctionCall2(distance_taxicab,
															PointerGetDatum(cube), PointerGetDatum(query)));
				break;
			case CubeKNNDistanceEuclid:
				retval = DatumGetFloat8(DirectFunctionCall2(cube_distance,
															PointerGetDatum(cube), PointerGetDatum(query)));
				break;
			case CubeKNNDistanceChebyshev:
				retval = DatumGetFloat8(DirectFunctionCall2(distance_chebyshev,
															PointerGetDatum(cube), PointerGetDatum(query)));
				break;
			default:
				elog(ERROR, "unrecognized cube strategy number: %d", strategy);
				retval = 0;		/* keep compiler quiet */
				break;
		}
	}
	PG_RETURN_FLOAT8(retval);
}

static double
distance_1D(double a1, double a2, double b1, double b2)
{
	/* interval (a) is entirely on the left of (b)
	 *
	 * 区间 (a) 完全位于 (b) 的左侧
	 */
	if ((a1 <= b1) && (a2 <= b1) && (a1 <= b2) && (a2 <= b2))
		return (Min(b1, b2) - Max(a1, a2));

	/* interval (a) is entirely on the right of (b)
	 *
	 * 区间 (a) 完全位于 (b) 的右侧
	 */
	if ((a1 > b1) && (a2 > b1) && (a1 > b2) && (a2 > b2))
		return (Min(a1, a2) - Max(b1, b2));

	/* the rest are all sorts of intersections
	 *
	 * 剩下的都是各种路口
	 */
	return 0.0;
}

/* Test if a box is also a point
 *
 * 测试一个盒子是否也是一个点
 */
Datum
cube_is_point(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	bool		result;

	result = cube_is_point_internal(cube);
	PG_FREE_IF_COPY(cube, 0);
	PG_RETURN_BOOL(result);
}

static bool
cube_is_point_internal(NDBOX *cube)
{
	int			i;

	if (IS_POINT(cube))
		return true;

	/*
	 * Even if the point-flag is not set, all the lower-left coordinates might
	 * match the upper-right coordinates, so that the value is in fact a
	 * point. Such values don't arise with current code - the point flag is
	 * always set if appropriate - but they might be present on-disk in
	 * clusters upgraded from pre-9.4 versions.
	 *
	 * 即使未设置点标志，所有左下坐标也可能与右上角坐标匹配，因此该值实际上是一个点。当前代码不会出现此类值 - 如果合适，点标志始终会被设置 - 但它们可能存在于从 9.4 之前版本升级的集群中的磁盘上。
	 */
	for (i = 0; i < DIM(cube); i++)
	{
		if (LL_COORD(cube, i) != UR_COORD(cube, i))
			return false;
	}
	return true;
}

/* Return dimensions in use in the data structure
 *
 * 返回数据结构中使用的维度
 */
Datum
cube_dim(PG_FUNCTION_ARGS)
{
	NDBOX	   *c = PG_GETARG_NDBOX_P(0);
	int			dim = DIM(c);

	PG_FREE_IF_COPY(c, 0);
	PG_RETURN_INT32(dim);
}

/* Return a specific normalized LL coordinate
 *
 * 返回特定的归一化 LL 坐标
 */
Datum
cube_ll_coord(PG_FUNCTION_ARGS)
{
	NDBOX	   *c = PG_GETARG_NDBOX_P(0);
	int			n = PG_GETARG_INT32(1);
	double		result;

	if (DIM(c) >= n && n > 0)
		result = Min(LL_COORD(c, n - 1), UR_COORD(c, n - 1));
	else
		result = 0;

	PG_FREE_IF_COPY(c, 0);
	PG_RETURN_FLOAT8(result);
}

/* Return a specific normalized UR coordinate
 *
 * 返回特定的标准化 UR 坐标
 */
Datum
cube_ur_coord(PG_FUNCTION_ARGS)
{
	NDBOX	   *c = PG_GETARG_NDBOX_P(0);
	int			n = PG_GETARG_INT32(1);
	double		result;

	if (DIM(c) >= n && n > 0)
		result = Max(LL_COORD(c, n - 1), UR_COORD(c, n - 1));
	else
		result = 0;

	PG_FREE_IF_COPY(c, 0);
	PG_RETURN_FLOAT8(result);
}

/*
 * Function returns cube coordinate.
 * Numbers from 1 to DIM denotes first corner coordinates.
 * Numbers from DIM+1 to 2*DIM denotes second corner coordinates.
 *
 * 函数返回立方体坐标。从 1 到 DIM 的数字表示第一个角坐标。从 DIM+1 到 2*DIM 的数字表示第二个角坐标。
 */
Datum
cube_coord(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	int			coord = PG_GETARG_INT32(1);

	if (coord <= 0 || coord > 2 * DIM(cube))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("cube index %d is out of bounds", coord)));

	if (IS_POINT(cube))
		PG_RETURN_FLOAT8(cube->x[(coord - 1) % DIM(cube)]);
	else
		PG_RETURN_FLOAT8(cube->x[coord - 1]);
}


/*----
 * This function works like cube_coord(), but rearranges coordinates in the
 * way suitable to support coordinate ordering using KNN-GiST.  For historical
 * reasons this extension allows us to create cubes in form ((2,1),(1,2)) and
 * instead of normalizing such cube to ((1,1),(2,2)) it stores cube in original
 * way.  But in order to get cubes ordered by one of dimensions from the index
 * without explicit sort step we need this representation-independent coordinate
 * getter.  Moreover, indexed dataset may contain cubes of different dimensions
 * number.  Accordingly, this coordinate getter should be able to return
 * lower/upper bound for particular dimension independently on number of cube
 * dimensions.  Also, KNN-GiST supports only ascending sorting.  In order to
 * support descending sorting, this function returns inverse of value when
 * negative coordinate is given.
 *
 * 该函数的工作方式类似于cube_coord()，但以适合使用 KNN-GiST 支持坐标排序的方式重新排列坐标。  由于历史原因，此扩展允许我们以 ((2,1),(1,2)) 形式创建多维数据集，而不是将此类多维数据集标准化为 ((1,1),(2,2))，而是以原始方式存储多维数据集。  但是，为了在没有显式排序步骤的情况下从索引中获取按维度之一排序的立方体，我们需要这种与表示无关的坐标获取器。  此外，索引数据集可能包含不同维度的立方体数量。  因此，该坐标获取器应该能够独立于立方体维度的数量返回特定维度的下限/上限。  此外，KNN-GiST 仅支持升序排序。  为了支持降序排序，当给定负坐标时，该函数返回值的倒数。
 *
 * Long story short, this function uses following meaning of coordinates:
 * # (2 * N - 1) -- lower bound of Nth dimension,
 * # (2 * N) -- upper bound of Nth dimension,
 * # - (2 * N - 1) -- negative of lower bound of Nth dimension,
 * # - (2 * N) -- negative of upper bound of Nth dimension.
 *
 * 长话短说，该函数使用的坐标含义如下： # (2 * N - 1) -- 第 N 维下界， # (2 * N) -- 第 N 维上界， # - (2 * N - 1) -- 第 N 维下界负值， # - (2 * N) -- 第 N 维上界负值。
 *
 * When given coordinate exceeds number of cube dimensions, then 0 returned
 * (reproducing logic of GiST indexing of variable-length cubes).
 *
 * 当给定的坐标超过立方体维度数时，则返回 0（再现可变长度立方体的 GiST 索引逻辑）。
 */
Datum
cube_coord_llur(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	int			coord = PG_GETARG_INT32(1);
	bool		inverse = false;
	float8		result;

	/* 0 is the only unsupported coordinate value
	 *
	 * 0 是唯一不受支持的坐标值
	 */
	if (coord == 0)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("zero cube index is not defined")));

	/* Return inversed value for negative coordinate
	 *
	 * 返回负坐标的反转值
	 */
	if (coord < 0)
	{
		coord = -coord;
		inverse = true;
	}

	if (coord <= 2 * DIM(cube))
	{
		/* dimension index
		 *
		 * 维度索引
		 */
		int			index = (coord - 1) / 2;

		/* whether this is upper bound (lower bound otherwise)
		 *
		 * 这是否是上限（否则是下限）
		 */
		bool		upper = ((coord - 1) % 2 == 1);

		if (IS_POINT(cube))
		{
			result = cube->x[index];
		}
		else
		{
			if (upper)
				result = Max(cube->x[index], cube->x[index + DIM(cube)]);
			else
				result = Min(cube->x[index], cube->x[index + DIM(cube)]);
		}
	}
	else
	{
		/*
		 * Return zero if coordinate is out of bound.  That reproduces logic
		 * of how cubes with low dimension number are expanded during GiST
		 * indexing.
		 *
		 * 如果坐标超出范围，则返回零。  这再现了在 GiST 索引期间如何扩展低维度多维数据集的逻辑。
		 */
		result = 0.0;
	}

	/* Inverse value if needed
	 *
	 * 如果需要则取反值
	 */
	if (inverse)
		result = -result;

	PG_RETURN_FLOAT8(result);
}

/* Increase or decrease box size by a radius in at least n dimensions.
 *
 * 在至少 n 维中按半径增加或减少长方体尺寸。
 */
Datum
cube_enlarge(PG_FUNCTION_ARGS)
{
	NDBOX	   *a = PG_GETARG_NDBOX_P(0);
	double		r = PG_GETARG_FLOAT8(1);
	int32		n = PG_GETARG_INT32(2);
	NDBOX	   *result;
	int			dim = 0;
	int			size;
	int			i,
				j;

	if (n > CUBE_MAX_DIM)
		n = CUBE_MAX_DIM;
	if (r > 0 && n > 0)
		dim = n;
	if (DIM(a) > dim)
		dim = DIM(a);

	size = CUBE_SIZE(dim);
	result = (NDBOX *) palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, dim);

	for (i = 0, j = dim; i < DIM(a); i++, j++)
	{
		if (LL_COORD(a, i) >= UR_COORD(a, i))
		{
			result->x[i] = UR_COORD(a, i) - r;
			result->x[j] = LL_COORD(a, i) + r;
		}
		else
		{
			result->x[i] = LL_COORD(a, i) - r;
			result->x[j] = UR_COORD(a, i) + r;
		}
		if (result->x[i] > result->x[j])
		{
			result->x[i] = (result->x[i] + result->x[j]) / 2;
			result->x[j] = result->x[i];
		}
	}
	/* dim > a->dim only if r > 0
	 *
	 * 仅当 r > 0 时暗淡 > a->dim
	 */
	for (; i < dim; i++, j++)
	{
		result->x[i] = -r;
		result->x[j] = r;
	}

	/*
	 * Check if the result was in fact a point, and set the flag in the datum
	 * accordingly. (we don't bother to repalloc it smaller)
	 *
	 * 检查结果是否确实是一个点，并相应地在数据中设置标志。 （我们不费心将其重新分配得更小）
	 */
	if (cube_is_point_internal(result))
	{
		size = POINT_SIZE(dim);
		SET_VARSIZE(result, size);
		SET_POINT_BIT(result);
	}

	PG_FREE_IF_COPY(a, 0);
	PG_RETURN_NDBOX_P(result);
}

/* Create a one dimensional box with identical upper and lower coordinates
 *
 * 创建一个具有相同上下坐标的一维盒子
 */
Datum
cube_f8(PG_FUNCTION_ARGS)
{
	double		x = PG_GETARG_FLOAT8(0);
	NDBOX	   *result;
	int			size;

	size = POINT_SIZE(1);
	result = (NDBOX *) palloc0(size);
	SET_VARSIZE(result, size);
	SET_DIM(result, 1);
	SET_POINT_BIT(result);
	result->x[0] = x;

	PG_RETURN_NDBOX_P(result);
}

/* Create a one dimensional box
 *
 * 创建一个一维盒子
 */
Datum
cube_f8_f8(PG_FUNCTION_ARGS)
{
	double		x0 = PG_GETARG_FLOAT8(0);
	double		x1 = PG_GETARG_FLOAT8(1);
	NDBOX	   *result;
	int			size;

	if (x0 == x1)
	{
		size = POINT_SIZE(1);
		result = (NDBOX *) palloc0(size);
		SET_VARSIZE(result, size);
		SET_DIM(result, 1);
		SET_POINT_BIT(result);
		result->x[0] = x0;
	}
	else
	{
		size = CUBE_SIZE(1);
		result = (NDBOX *) palloc0(size);
		SET_VARSIZE(result, size);
		SET_DIM(result, 1);
		result->x[0] = x0;
		result->x[1] = x1;
	}

	PG_RETURN_NDBOX_P(result);
}

/* Add a dimension to an existing cube with the same values for the new
   coordinate */
Datum
cube_c_f8(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	double		x = PG_GETARG_FLOAT8(1);
	NDBOX	   *result;
	int			size;
	int			i;

	if (DIM(cube) + 1 > CUBE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("can't extend cube"),
				 errdetail("A cube cannot have more than %d dimensions.",
						   CUBE_MAX_DIM)));

	if (IS_POINT(cube))
	{
		size = POINT_SIZE((DIM(cube) + 1));
		result = (NDBOX *) palloc0(size);
		SET_VARSIZE(result, size);
		SET_DIM(result, DIM(cube) + 1);
		SET_POINT_BIT(result);
		for (i = 0; i < DIM(cube); i++)
			result->x[i] = cube->x[i];
		result->x[DIM(result) - 1] = x;
	}
	else
	{
		size = CUBE_SIZE((DIM(cube) + 1));
		result = (NDBOX *) palloc0(size);
		SET_VARSIZE(result, size);
		SET_DIM(result, DIM(cube) + 1);
		for (i = 0; i < DIM(cube); i++)
		{
			result->x[i] = cube->x[i];
			result->x[DIM(result) + i] = cube->x[DIM(cube) + i];
		}
		result->x[DIM(result) - 1] = x;
		result->x[2 * DIM(result) - 1] = x;
	}

	PG_FREE_IF_COPY(cube, 0);
	PG_RETURN_NDBOX_P(result);
}

/* Add a dimension to an existing cube
 *
 * 向现有多维数据集添加维度
 */
Datum
cube_c_f8_f8(PG_FUNCTION_ARGS)
{
	NDBOX	   *cube = PG_GETARG_NDBOX_P(0);
	double		x1 = PG_GETARG_FLOAT8(1);
	double		x2 = PG_GETARG_FLOAT8(2);
	NDBOX	   *result;
	int			size;
	int			i;

	if (DIM(cube) + 1 > CUBE_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("can't extend cube"),
				 errdetail("A cube cannot have more than %d dimensions.",
						   CUBE_MAX_DIM)));

	if (IS_POINT(cube) && (x1 == x2))
	{
		size = POINT_SIZE((DIM(cube) + 1));
		result = (NDBOX *) palloc0(size);
		SET_VARSIZE(result, size);
		SET_DIM(result, DIM(cube) + 1);
		SET_POINT_BIT(result);
		for (i = 0; i < DIM(cube); i++)
			result->x[i] = cube->x[i];
		result->x[DIM(result) - 1] = x1;
	}
	else
	{
		size = CUBE_SIZE((DIM(cube) + 1));
		result = (NDBOX *) palloc0(size);
		SET_VARSIZE(result, size);
		SET_DIM(result, DIM(cube) + 1);
		for (i = 0; i < DIM(cube); i++)
		{
			result->x[i] = LL_COORD(cube, i);
			result->x[DIM(result) + i] = UR_COORD(cube, i);
		}
		result->x[DIM(result) - 1] = x1;
		result->x[2 * DIM(result) - 1] = x2;
	}

	PG_FREE_IF_COPY(cube, 0);
	PG_RETURN_NDBOX_P(result);
}
