/*
 * contrib/tablefunc/tablefunc.c
 *
 *
 * tablefunc
 *
 * Sample to demonstrate C functions which return setof scalar
 * and setof composite.
 * Joe Conway <mail@joeconway.com>
 * And contributors:
 * Nabil Sayegh <postgresql@e-trolley.de>
 *
 * Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/pg_prng.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(
					.name = "tablefunc",
					.version = PG_VERSION
);

static HTAB *load_categories_hash(char *cats_sql, MemoryContext per_query_ctx);
static Tuplestorestate *get_crosstab_tuplestore(char *sql,
												HTAB *crosstab_hash,
												TupleDesc tupdesc,
												bool randomAccess);
static void validateConnectbyTupleDesc(TupleDesc td, bool show_branch, bool show_serial);
static void compatCrosstabTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc);
static void compatConnectbyTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc);
static void get_normal_pair(float8 *x1, float8 *x2);
static Tuplestorestate *connectby(char *relname,
								  char *key_fld,
								  char *parent_key_fld,
								  char *orderby_fld,
								  char *branch_delim,
								  char *start_with,
								  int max_depth,
								  bool show_branch,
								  bool show_serial,
								  MemoryContext per_query_ctx,
								  bool randomAccess,
								  AttInMetadata *attinmeta);
static void build_tuplestore_recursively(char *key_fld,
										 char *parent_key_fld,
										 char *relname,
										 char *orderby_fld,
										 char *branch_delim,
										 char *start_with,
										 char *branch,
										 int level,
										 int *serial,
										 int max_depth,
										 bool show_branch,
										 bool show_serial,
										 MemoryContext per_query_ctx,
										 AttInMetadata *attinmeta,
										 Tuplestorestate *tupstore);

typedef struct
{
	float8		mean;			/* mean of the distribution */
	float8		stddev;			/* stddev of the distribution */
	float8		carry_val;		/* hold second generated value */
	bool		use_carry;		/* use second generated value */
} normal_rand_fctx;

#define xpfree(var_) \
	do { \
		if (var_ != NULL) \
		{ \
			pfree(var_); \
			var_ = NULL; \
		} \
	} while (0)

#define xpstrdup(tgtvar_, srcvar_) \
	do { \
		if (srcvar_) \
			tgtvar_ = pstrdup(srcvar_); \
		else \
			tgtvar_ = NULL; \
	} while (0)

#define xstreq(tgtvar_, srcvar_) \
	(((tgtvar_ == NULL) && (srcvar_ == NULL)) || \
	 ((tgtvar_ != NULL) && (srcvar_ != NULL) && (strcmp(tgtvar_, srcvar_) == 0)))

/* sign, 10 digits, '\0'
 *
 * 符号，10 位数字，'\0'
 */
#define INT32_STRLEN	12

/* stored info for a crosstab category
 *
 * 交叉表类别的存储信息
 */
typedef struct crosstab_cat_desc
{
	char	   *catname;		/* full category name */
	uint64		attidx;			/* zero based */
} crosstab_cat_desc;

#define MAX_CATNAME_LEN			NAMEDATALEN
#define INIT_CATS				64

#define crosstab_HashTableLookup(HASHTAB, CATNAME, CATDESC) \
do { \
	crosstab_HashEnt *hentry; char key[MAX_CATNAME_LEN]; \
	\
	MemSet(key, 0, MAX_CATNAME_LEN); \
	snprintf(key, MAX_CATNAME_LEN - 1, "%s", CATNAME); \
	hentry = (crosstab_HashEnt*) hash_search(HASHTAB, \
										 key, HASH_FIND, NULL); \
	if (hentry) \
		CATDESC = hentry->catdesc; \
	else \
		CATDESC = NULL; \
} while(0)

#define crosstab_HashTableInsert(HASHTAB, CATDESC) \
do { \
	crosstab_HashEnt *hentry; bool found; char key[MAX_CATNAME_LEN]; \
	\
	MemSet(key, 0, MAX_CATNAME_LEN); \
	snprintf(key, MAX_CATNAME_LEN - 1, "%s", CATDESC->catname); \
	hentry = (crosstab_HashEnt*) hash_search(HASHTAB, \
										 key, HASH_ENTER, &found); \
	if (found) \
		ereport(ERROR, \
				(errcode(ERRCODE_DUPLICATE_OBJECT), \
				 errmsg("duplicate category name"))); \
	hentry->catdesc = CATDESC; \
} while(0)

/* hash table
 *
 * 哈希表
 */
typedef struct crosstab_hashent
{
	char		internal_catname[MAX_CATNAME_LEN];
	crosstab_cat_desc *catdesc;
} crosstab_HashEnt;

/*
 * normal_rand - return requested number of random values
 * with a Gaussian (Normal) distribution.
 *
 * Normal_rand - 返回请求数量的具有高斯（正态）分布的随机值。
 *
 * inputs are int numvals, float8 mean, and float8 stddev
 * returns setof float8
 *
 * 输入为 int numvals、float8mean，float8 stddev 返回 setof float8
 */
PG_FUNCTION_INFO_V1(normal_rand);
Datum
normal_rand(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	uint64		call_cntr;
	uint64		max_calls;
	normal_rand_fctx *fctx;
	float8		mean;
	float8		stddev;
	float8		carry_val;
	bool		use_carry;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function
	 *
	 * 仅在第一次调用函数时完成的操作
	 */
	if (SRF_IS_FIRSTCALL())
	{
		int32		num_tuples;

		/* create a function context for cross-call persistence
		 *
		 * 创建用于交叉调用持久化的函数上下文
		 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 *
		 * 切换到适合多个函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* total number of tuples to be returned
		 *
		 * 要返回的元组总数
		 */
		num_tuples = PG_GETARG_INT32(0);
		if (num_tuples < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("number of rows cannot be negative")));
		funcctx->max_calls = num_tuples;

		/* allocate memory for user context
		 *
		 * 为用户上下文分配内存
		 */
		fctx = (normal_rand_fctx *) palloc(sizeof(normal_rand_fctx));

		/*
		 * Use fctx to keep track of upper and lower bounds from call to call.
		 * It will also be used to carry over the spare value we get from the
		 * Box-Muller algorithm so that we only actually calculate a new value
		 * every other call.
		 *
		 * 使用 fctx 跟踪每次调用的上限和下限。它还将用于继承我们从 Box-Muller 算法获得的备用值，以便我们实际上每次调用时只计算一个新值。
		 */
		fctx->mean = PG_GETARG_FLOAT8(1);
		fctx->stddev = PG_GETARG_FLOAT8(2);
		fctx->carry_val = 0;
		fctx->use_carry = false;

		funcctx->user_fctx = fctx;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function
	 *
	 * 每次调用函数时完成的事情
	 */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	fctx = funcctx->user_fctx;
	mean = fctx->mean;
	stddev = fctx->stddev;
	carry_val = fctx->carry_val;
	use_carry = fctx->use_carry;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		float8		result;

		if (use_carry)
		{
			/*
			 * reset use_carry and use second value obtained on last pass
			 *
			 * 重置 use_carry 并使用上一次获得的第二个值
			 */
			fctx->use_carry = false;
			result = carry_val;
		}
		else
		{
			float8		normval_1;
			float8		normval_2;

			/* Get the next two normal values
			 *
			 * 获取接下来的两个正常值
			 */
			get_normal_pair(&normval_1, &normval_2);

			/* use the first
			 *
			 * 使用第一个
			 */
			result = mean + (stddev * normval_1);

			/* and save the second
			 *
			 * 并保存第二个
			 */
			fctx->carry_val = mean + (stddev * normval_2);
			fctx->use_carry = true;
		}

		/* send the result
		 *
		 * 发送结果
		 */
		SRF_RETURN_NEXT(funcctx, Float8GetDatum(result));
	}
	else
		/* do when there is no more left
		 *
		 * 当没有剩下的时候做
		 */
		SRF_RETURN_DONE(funcctx);
}

/*
 * get_normal_pair()
 * Assigns normally distributed (Gaussian) values to a pair of provided
 * parameters, with mean 0, standard deviation 1.
 *
 * get_normal_pair() 将正态分布（高斯）值分配给一对提供的参数，平均值为 0，标准差为 1。
 *
 * This routine implements Algorithm P (Polar method for normal deviates)
 * from Knuth's _The_Art_of_Computer_Programming_, Volume 2, 3rd ed., pages
 * 122-126. Knuth cites his source as "The polar method", G. E. P. Box, M. E.
 * Muller, and G. Marsaglia, _Annals_Math,_Stat._ 29 (1958), 610-611.
 *
 * 该例程实现 Knuth 的《计算机编程艺术》第 2 卷第 3 版第 122-126 页中的算法 P（用于正常偏差的极坐标法）。 Knuth 引用了他的资料来源“极地方法”，G. E. P. Box、M. E. Muller 和 G. Marsaglia，_Annals_Math,_Stat._ 29 (1958), 610-611。
 *
 */
static void
get_normal_pair(float8 *x1, float8 *x2)
{
	float8		u1,
				u2,
				v1,
				v2,
				s;

	do
	{
		u1 = pg_prng_double(&pg_global_prng_state);
		u2 = pg_prng_double(&pg_global_prng_state);

		v1 = (2.0 * u1) - 1.0;
		v2 = (2.0 * u2) - 1.0;

		s = v1 * v1 + v2 * v2;
	} while (s >= 1.0);

	if (s == 0)
	{
		*x1 = 0;
		*x2 = 0;
	}
	else
	{
		s = sqrt((-2.0 * log(s)) / s);
		*x1 = v1 * s;
		*x2 = v2 * s;
	}
}

/*
 * crosstab - create a crosstab of rowids and values columns from a
 * SQL statement returning one rowid column, one category column,
 * and one value column.
 *
 * crosstab - 从返回一个 rowid 列、一个类别列和一个值列的 SQL 语句创建 rowid 和值列的交叉表。
 *
 * e.g. given sql which produces:
 *
 * 例如给定的 sql 会产生：
 *
 *			rowid	cat		value
 *			------+-------+-------
 *			row1	cat1	val1
 *			row1	cat2	val2
 *			row1	cat3	val3
 *			row1	cat4	val4
 *			row2	cat1	val5
 *			row2	cat2	val6
 *			row2	cat3	val7
 *			row2	cat4	val8
 *
 * rowid 猫值 ------+--------+-------- row1 cat1 val1 row1 cat2 val2 row1 cat3 val3 row1 cat4 val4 row2 cat1 val5 row2 cat2 val6 row2 cat3 val7 row2 cat4 val8
 *
 * crosstab returns:
 *					<===== values columns =====>
 *			rowid	cat1	cat2	cat3	cat4
 *			------+-------+-------+-------+-------
 *			row1	val1	val2	val3	val4
 *			row2	val5	val6	val7	val8
 *
 * 交叉表返回： <===== 值列 =====> rowid cat1 cat2 cat3 cat4 ------+-------+-------+-------+-------- row1 val1 val2 val3 val4 row2 val5 val6 val7 val8
 *
 * NOTES:
 * 1. SQL result must be ordered by 1,2.
 * 2. The number of values columns depends on the tuple description
 *	  of the function's declared return type.  The return type's columns
 *	  must match the datatypes of the SQL query's result.  The datatype
 *	  of the category column can be anything, however.
 * 3. Missing values (i.e. not enough adjacent rows of same rowid to
 *	  fill the number of result values columns) are filled in with nulls.
 * 4. Extra values (i.e. too many adjacent rows of same rowid to fill
 *	  the number of result values columns) are skipped.
 * 5. Rows with all nulls in the values columns are skipped.
 *
 * 注意： 1. SQL结果必须按1,2排序。 2. 值列的数量取决于函数声明的返回类型的元组描述。  返回类型的列必须与 SQL 查询结果的数据类型匹配。  但是，类别列的数据类型可以是任何类型。 3. 缺失值（即相同 rowid 的相邻行不足以填充结果值列的数量）用空值填充。 4. 跳过额外值（即同一 rowid 的相邻行太多，无法填充结果值列的数量）。 5. 跳过值列中全部为空的行。
 */
PG_FUNCTION_INFO_V1(crosstab);
Datum
crosstab(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	uint64		call_cntr;
	uint64		max_calls;
	AttInMetadata *attinmeta;
	SPITupleTable *spi_tuptable;
	TupleDesc	spi_tupdesc;
	bool		firstpass;
	char	   *lastrowid;
	int			i;
	int			num_categories;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int			ret;
	uint64		proc;

	/* check to see if caller supports us returning a tuplestore
	 *
	 * 检查调用者是否支持我们返回元组存储
	 */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;

	/* Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	/* Retrieve the desired rows
	 *
	 * 检索所需的行
	 */
	ret = SPI_execute(sql, true, 0);
	proc = SPI_processed;

	/* If no qualifying tuples, fall out early
	 *
	 * 如果没有符合条件的元组，则尽早退出
	 */
	if (ret != SPI_OK_SELECT || proc == 0)
	{
		SPI_finish();
		rsinfo->isDone = ExprEndResult;
		PG_RETURN_NULL();
	}

	spi_tuptable = SPI_tuptable;
	spi_tupdesc = spi_tuptable->tupdesc;

	/*----------
	 * The provided SQL query must always return three columns.
	 *
	 * 提供的 SQL 查询必须始终返回三列。
	 *
	 * 1. rowname
	 *	the label or identifier for each row in the final result
	 * 2. category
	 *	the label or identifier for each column in the final result
	 * 3. values
	 *	the value for each column in the final result
	 *
	 * 1. rowname 最终结果中每行的标签或标识符 2. Category 最终结果中每列的标签或标识符 3. Values 最终结果中每列的值
	 *----------
	 */
	if (spi_tupdesc->natts != 3)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid crosstab source data query"),
				 errdetail("The query must return 3 columns: row_name, category, and value.")));

	/* get a tuple descriptor for our result type
	 *
	 * 获取结果类型的元组描述符
	 */
	switch (get_call_result_type(fcinfo, NULL, &tupdesc))
	{
		case TYPEFUNC_COMPOSITE:
			/* success */
			break;
		case TYPEFUNC_RECORD:
			/* failed to determine actual type of RECORD
			 *
			 * 无法确定 RECORD 的实际类型
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
			break;
		default:
			/* result type isn't composite
			 *
			 * 结果类型不是复合类型
			 */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("return type must be a row type")));
			break;
	}

	/*
	 * Check that return tupdesc is compatible with the data we got from SPI,
	 * at least based on number and type of attributes
	 *
	 * 检查返回的 tupdesc 是否与我们从 SPI 获取的数据兼容，至少基于属性的数量和类型
	 */
	compatCrosstabTupleDescs(tupdesc, spi_tupdesc);

	/*
	 * switch to long-lived memory context
	 *
	 * 切换到长期内存上下文
	 */
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* make sure we have a persistent copy of the result tupdesc
	 *
	 * 确保我们有结果 tupdesc 的持久副本
	 */
	tupdesc = CreateTupleDescCopy(tupdesc);

	/* initialize our tuplestore in long-lived context
	 *
	 * 在长期上下文中初始化我们的元组存储
	 */
	tupstore =
		tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Generate attribute metadata needed later to produce tuples from raw C
	 * strings
	 *
	 * 生成稍后需要的属性元数据，以从原始 C 字符串生成元组
	 */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* total number of tuples to be examined
	 *
	 * 要检查的元组总数
	 */
	max_calls = proc;

	/* the return tuple always must have 1 rowid + num_categories columns
	 *
	 * 返回元组始终必须有 1 rowid + num_categories 列
	 */
	num_categories = tupdesc->natts - 1;

	firstpass = true;
	lastrowid = NULL;

	for (call_cntr = 0; call_cntr < max_calls; call_cntr++)
	{
		bool		skip_tuple = false;
		char	  **values;

		/* allocate and zero space
		 *
		 * 分配和清零空间
		 */
		values = (char **) palloc0((1 + num_categories) * sizeof(char *));

		/*
		 * now loop through the sql results and assign each value in sequence
		 * to the next category
		 *
		 * 现在循环遍历sql结果并将每个值按顺序分配给下一个类别
		 */
		for (i = 0; i < num_categories; i++)
		{
			HeapTuple	spi_tuple;
			char	   *rowid;

			/* see if we've gone too far already
			 *
			 * 看看我们是否已经走得太远了
			 */
			if (call_cntr >= max_calls)
				break;

			/* get the next sql result tuple
			 *
			 * 获取下一个sql结果元组
			 */
			spi_tuple = spi_tuptable->vals[call_cntr];

			/* get the rowid from the current sql result tuple
			 *
			 * 从当前sql结果元组中获取rowid
			 */
			rowid = SPI_getvalue(spi_tuple, spi_tupdesc, 1);

			/*
			 * If this is the first pass through the values for this rowid,
			 * set the first column to rowid
			 *
			 * 如果这是第一次传递此 rowid 的值，请将第一列设置为 rowid
			 */
			if (i == 0)
			{
				xpstrdup(values[0], rowid);

				/*
				 * Check to see if the rowid is the same as that of the last
				 * tuple sent -- if so, skip this tuple entirely
				 *
				 * 检查 rowid 是否与最后发送的元组相同 - 如果是，则完全跳过该元组
				 */
				if (!firstpass && xstreq(lastrowid, rowid))
				{
					xpfree(rowid);
					skip_tuple = true;
					break;
				}
			}

			/*
			 * If rowid hasn't changed on us, continue building the output
			 * tuple.
			 *
			 * 如果 rowid 没有改变，则继续构建输出元组。
			 */
			if (xstreq(rowid, values[0]))
			{
				/*
				 * Get the next category item value, which is always attribute
				 * number three.
				 *
				 * 获取下一个类别项值，该值始终是属性号三。
				 *
				 * Be careful to assign the value to the array index based on
				 * which category we are presently processing.
				 *
				 * 请注意根据我们当前正在处理的类别将值分配给数组索引。
				 */
				values[1 + i] = SPI_getvalue(spi_tuple, spi_tupdesc, 3);

				/*
				 * increment the counter since we consume a row for each
				 * category, but not for last pass because the outer loop will
				 * do that for us
				 *
				 * 增加计数器，因为我们为每个类别消耗一行，但不会为最后一次消耗一行，因为外部循环将为我们执行此操作
				 */
				if (i < (num_categories - 1))
					call_cntr++;
				xpfree(rowid);
			}
			else
			{
				/*
				 * We'll fill in NULLs for the missing values, but we need to
				 * decrement the counter since this sql result row doesn't
				 * belong to the current output tuple.
				 *
				 * 我们将为缺失值填充 NULL，但我们需要递减计数器，因为该 sql 结果行不属于当前输出元组。
				 */
				call_cntr--;
				xpfree(rowid);
				break;
			}
		}

		if (!skip_tuple)
		{
			HeapTuple	tuple;

			/* build the tuple and store it
			 *
			 * 构建元组并存储它
			 */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);
			heap_freetuple(tuple);
		}

		/* Remember current rowid
		 *
		 * 记住当前rowid
		 */
		xpfree(lastrowid);
		xpstrdup(lastrowid, values[0]);
		firstpass = false;

		/* Clean up
		 *
		 * 清理
		 */
		for (i = 0; i < num_categories + 1; i++)
			if (values[i] != NULL)
				pfree(values[i]);
		pfree(values);
	}

	/* let the caller know we're sending back a tuplestore
	 *
	 * 让调用者知道我们正在发回一个元组存储
	 */
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	/* release SPI related resources (and return to caller's context)
	 *
	 * 释放SPI相关资源（并返回调用者上下文）
	 */
	SPI_finish();

	return (Datum) 0;
}

/*
 * crosstab_hash - reimplement crosstab as materialized function and
 * properly deal with missing values (i.e. don't pack remaining
 * values to the left)
 *
 * crosstab_hash - 将交叉表重新实现为物化函数并正确处理缺失值（即不要将剩余值打包到左侧）
 *
 * crosstab - create a crosstab of rowids and values columns from a
 * SQL statement returning one rowid column, one category column,
 * and one value column.
 *
 * crosstab - 从返回一个 rowid 列、一个类别列和一个值列的 SQL 语句创建 rowid 和值列的交叉表。
 *
 * e.g. given sql which produces:
 *
 * 例如给定的 sql 会产生：
 *
 *			rowid	cat		value
 *			------+-------+-------
 *			row1	cat1	val1
 *			row1	cat2	val2
 *			row1	cat4	val4
 *			row2	cat1	val5
 *			row2	cat2	val6
 *			row2	cat3	val7
 *			row2	cat4	val8
 *
 * rowid 猫值 ------+--------+-------- row1 cat1 val1 row1 cat2 val2 row1 cat4 val4 row2 cat1 val5 row2 cat2 val6 row2 cat3 val7 row2 cat4 val8
 *
 * crosstab returns:
 *					<===== values columns =====>
 *			rowid	cat1	cat2	cat3	cat4
 *			------+-------+-------+-------+-------
 *			row1	val1	val2	null	val4
 *			row2	val5	val6	val7	val8
 *
 * 交叉表返回： <===== 值列 ====> rowid cat1 cat2 cat3 cat4 ------+--------+-------+-------+-------- row1 val1 val2 null val4 row2 val5 val6 val7 val8
 *
 * NOTES:
 * 1. SQL result must be ordered by 1.
 * 2. The number of values columns depends on the tuple description
 *	  of the function's declared return type.
 * 3. Missing values (i.e. missing category) are filled in with nulls.
 * 4. Extra values (i.e. not in category results) are skipped.
 *
 * 注意： 1. SQL 结果必须按 1 排序。 2. 值列的数量取决于函数声明的返回类型的元组描述。 3. 缺失值（即缺失类别）用空值填充。 4. 跳过额外值（即不在类别结果中）。
 */
PG_FUNCTION_INFO_V1(crosstab_hash);
Datum
crosstab_hash(PG_FUNCTION_ARGS)
{
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *cats_sql = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	HTAB	   *crosstab_hash;

	/* check to see if caller supports us returning a tuplestore
	 *
	 * 检查调用者是否支持我们返回元组存储
	 */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description
	 *
	 * 获取请求的返回元组描述
	 */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/*
	 * Check to make sure we have a reasonable tuple descriptor
	 *
	 * 检查以确保我们有一个合理的元组描述符
	 *
	 * Note we will attempt to coerce the values into whatever the return
	 * attribute type is and depend on the "in" function to complain if
	 * needed.
	 *
	 * 请注意，我们将尝试将值强制转换为任何返回属性类型，并在需要时依赖“in”函数进行抱怨。
	 */
	if (tupdesc->natts < 2)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid crosstab return type"),
				 errdetail("Return row must have at least two columns.")));

	/* load up the categories hash table
	 *
	 * 加载类别哈希表
	 */
	crosstab_hash = load_categories_hash(cats_sql, per_query_ctx);

	/* let the caller know we're sending back a tuplestore
	 *
	 * 让调用者知道我们正在发回一个元组存储
	 */
	rsinfo->returnMode = SFRM_Materialize;

	/* now go build it
	 *
	 * 现在去构建它
	 */
	rsinfo->setResult = get_crosstab_tuplestore(sql,
												crosstab_hash,
												tupdesc,
												rsinfo->allowedModes & SFRM_Materialize_Random);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 *
	 * SFRM_Materialize 模式期望我们返回 NULL Datum。实际的元组位于我们的元组存储中，并通过 rsinfo->setResult 传回。 rsinfo->setDesc 设置为我们实际用来构建元组的元组描述，因此调用者可以验证我们做了它所期望的事情。
	 */
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	return (Datum) 0;
}

/*
 * load up the categories hash table
 *
 * 加载类别哈希表
 */
static HTAB *
load_categories_hash(char *cats_sql, MemoryContext per_query_ctx)
{
	HTAB	   *crosstab_hash;
	HASHCTL		ctl;
	int			ret;
	uint64		proc;
	MemoryContext SPIcontext;

	/* initialize the category hash table
	 *
	 * 初始化类别哈希表
	 */
	ctl.keysize = MAX_CATNAME_LEN;
	ctl.entrysize = sizeof(crosstab_HashEnt);
	ctl.hcxt = per_query_ctx;

	/*
	 * use INIT_CATS, defined above as a guess of how many hash table entries
	 * to create, initially
	 *
	 * 使用上面定义的 INIT_CATS 来猜测最初要创建多少个哈希表条目
	 */
	crosstab_hash = hash_create("crosstab hash",
								INIT_CATS,
								&ctl,
								HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	/* Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	/* Retrieve the category name rows
	 *
	 * 检索类别名称行
	 */
	ret = SPI_execute(cats_sql, true, 0);
	proc = SPI_processed;

	/* Check for qualifying tuples
	 *
	 * 检查合格的元组
	 */
	if ((ret == SPI_OK_SELECT) && (proc > 0))
	{
		SPITupleTable *spi_tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = spi_tuptable->tupdesc;
		uint64		i;

		/*
		 * The provided categories SQL query must always return one column:
		 * category - the label or identifier for each column
		 *
		 * 提供的类别 SQL 查询必须始终返回一列：类别 - 每列的标签或标识符
		 */
		if (spi_tupdesc->natts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid crosstab categories query"),
					 errdetail("The query must return one column.")));

		for (i = 0; i < proc; i++)
		{
			crosstab_cat_desc *catdesc;
			char	   *catname;
			HeapTuple	spi_tuple;

			/* get the next sql result tuple
			 *
			 * 获取下一个sql结果元组
			 */
			spi_tuple = spi_tuptable->vals[i];

			/* get the category from the current sql result tuple
			 *
			 * 从当前sql结果元组中获取类别
			 */
			catname = SPI_getvalue(spi_tuple, spi_tupdesc, 1);
			if (catname == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("crosstab category value must not be null")));

			SPIcontext = MemoryContextSwitchTo(per_query_ctx);

			catdesc = (crosstab_cat_desc *) palloc(sizeof(crosstab_cat_desc));
			catdesc->catname = catname;
			catdesc->attidx = i;

			/* Add the proc description block to the hashtable
			 *
			 * 将proc描述块添加到哈希表中
			 */
			crosstab_HashTableInsert(crosstab_hash, catdesc);

			MemoryContextSwitchTo(SPIcontext);
		}
	}

	if (SPI_finish() != SPI_OK_FINISH)
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "load_categories_hash: SPI_finish() failed");

	return crosstab_hash;
}

/*
 * create and populate the crosstab tuplestore using the provided source query
 *
 * 使用提供的源查询创建并填充交叉表元组存储
 */
static Tuplestorestate *
get_crosstab_tuplestore(char *sql,
						HTAB *crosstab_hash,
						TupleDesc tupdesc,
						bool randomAccess)
{
	Tuplestorestate *tupstore;
	int			num_categories = hash_get_num_entries(crosstab_hash);
	AttInMetadata *attinmeta = TupleDescGetAttInMetadata(tupdesc);
	char	  **values;
	HeapTuple	tuple;
	int			ret;
	uint64		proc;

	/* initialize our tuplestore (while still in query context!)
	 *
	 * 初始化我们的元组存储（同时仍在查询上下文中！）
	 */
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);

	/* Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	/* Now retrieve the crosstab source rows
	 *
	 * 现在检索交叉表源行
	 */
	ret = SPI_execute(sql, true, 0);
	proc = SPI_processed;

	/* Check for qualifying tuples
	 *
	 * 检查合格的元组
	 */
	if ((ret == SPI_OK_SELECT) && (proc > 0))
	{
		SPITupleTable *spi_tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = spi_tuptable->tupdesc;
		int			ncols = spi_tupdesc->natts;
		char	   *rowid;
		char	   *lastrowid = NULL;
		bool		firstpass = true;
		uint64		i;
		int			j;
		int			result_ncols;

		if (num_categories == 0)
		{
			/* no qualifying category tuples
			 *
			 * 没有合格的类别元组
			 */
			ereport(ERROR,
					(errcode(ERRCODE_CARDINALITY_VIOLATION),
					 errmsg("crosstab categories query must return at least one row")));
		}

		/*
		 * The provided SQL query must always return at least three columns:
		 *
		 * 提供的 SQL 查询必须始终返回至少三列：
		 *
		 * 1. rowname	the label for each row - column 1 in the final result
		 * 2. category	the label for each value-column in the final result 3.
		 * value	 the values used to populate the value-columns
		 *
		 * 1. rowname 最终结果中每行 - 列 1 的标签 2. 对最终结果中每个值列的标签进行分类 3. value 用于填充值列的值
		 *
		 * If there are more than three columns, the last two are taken as
		 * "category" and "values". The first column is taken as "rowname".
		 * Additional columns (2 thru N-2) are assumed the same for the same
		 * "rowname", and are copied into the result tuple from the first time
		 * we encounter a particular rowname.
		 *
		 * 如果超过三列，则最后两列被视为“类别”和“值”。第一列被视为“rowname”。对于相同的“rowname”，附加列（2 到 N-2）被假定为相同，并且从我们第一次遇到特定的 rowname 时就被复制到结果元组中。
		 */
		if (ncols < 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid crosstab source data query"),
					 errdetail("The query must return at least 3 columns: row_name, category, and value.")));

		result_ncols = (ncols - 2) + num_categories;

		/* Recheck to make sure output tuple descriptor looks reasonable
		 *
		 * 重新检查以确保输出元组描述符看起来合理
		 */
		if (tupdesc->natts != result_ncols)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("invalid crosstab return type"),
					 errdetail("Return row must have %d columns, not %d.",
							   result_ncols, tupdesc->natts)));

		/* allocate space and make sure it's clear
		 *
		 * 分配空间并确保其清晰
		 */
		values = (char **) palloc0(result_ncols * sizeof(char *));

		for (i = 0; i < proc; i++)
		{
			HeapTuple	spi_tuple;
			crosstab_cat_desc *catdesc;
			char	   *catname;

			/* get the next sql result tuple
			 *
			 * 获取下一个sql结果元组
			 */
			spi_tuple = spi_tuptable->vals[i];

			/* get the rowid from the current sql result tuple
			 *
			 * 从当前sql结果元组中获取rowid
			 */
			rowid = SPI_getvalue(spi_tuple, spi_tupdesc, 1);

			/*
			 * if we're on a new output row, grab the column values up to
			 * column N-2 now
			 *
			 * 如果我们位于新的输出行，请立即获取直到 N-2 列的列值
			 */
			if (firstpass || !xstreq(lastrowid, rowid))
			{
				/*
				 * a new row means we need to flush the old one first, unless
				 * we're on the very first row
				 *
				 * 新行意味着我们需要先刷新旧行，除非我们在第一行
				 */
				if (!firstpass)
				{
					/* rowid changed, flush the previous output row
					 *
					 * rowid 改变，刷新之前的输出行
					 */
					tuple = BuildTupleFromCStrings(attinmeta, values);

					tuplestore_puttuple(tupstore, tuple);

					for (j = 0; j < result_ncols; j++)
						xpfree(values[j]);
				}

				values[0] = rowid;
				for (j = 1; j < ncols - 2; j++)
					values[j] = SPI_getvalue(spi_tuple, spi_tupdesc, j + 1);

				/* we're no longer on the first pass
				 *
				 * 我们不再处于第一关
				 */
				firstpass = false;
			}

			/* look up the category and fill in the appropriate column
			 *
			 * 查找类别并填写相应的列
			 */
			catname = SPI_getvalue(spi_tuple, spi_tupdesc, ncols - 1);

			if (catname != NULL)
			{
				crosstab_HashTableLookup(crosstab_hash, catname, catdesc);

				if (catdesc)
					values[catdesc->attidx + ncols - 2] =
						SPI_getvalue(spi_tuple, spi_tupdesc, ncols);
			}

			xpfree(lastrowid);
			xpstrdup(lastrowid, rowid);
		}

		/* flush the last output row
		 *
		 * 刷新最后一个输出行
		 */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		tuplestore_puttuple(tupstore, tuple);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		/* internal error
		 *
		 * 内部错误
		 */
		elog(ERROR, "get_crosstab_tuplestore: SPI_finish() failed");

	return tupstore;
}

/*
 * connectby_text - produce a result set from a hierarchical (parent/child)
 * table.
 *
 * connectby_text - 从分层（父/子）表生成结果集。
 *
 * e.g. given table foo:
 *
 * 例如给定表 foo：
 *
 *			keyid	parent_keyid pos
 *			------+------------+--
 *			row1	NULL		 0
 *			row2	row1		 0
 *			row3	row1		 0
 *			row4	row2		 1
 *			row5	row2		 0
 *			row6	row4		 0
 *			row7	row3		 0
 *			row8	row6		 0
 *			row9	row5		 0
 *
 * keyidparent_keyid pos ------+------------+-- row1 NULL 0 row2 row1 0 row3 row1 0 row4 row2 1 row5 row2 0 row6 row4 0 row7 row3 0 row8 row6 0 row9 row5 0
 *
 *
 * connectby(text relname, text keyid_fld, text parent_keyid_fld
 *			  [, text orderby_fld], text start_with, int max_depth
 *			  [, text branch_delim])
 * connectby('foo', 'keyid', 'parent_keyid', 'pos', 'row2', 0, '~') returns:
 *
 * connectby（文本relname，文本keyid_fld，文本parent_keyid_fld [，文本orderby_fld]，文本start_with，int max_深度[，文本branch_delim]）connectby（'foo'，'keyid'，'parent_keyid'，'pos'，'row2'，0，'〜'）返回：
 *
 *		keyid	parent_id	level	 branch				serial
 *		------+-----------+--------+-----------------------
 *		row2	NULL		  0		  row2				  1
 *		row5	row2		  1		  row2~row5			  2
 *		row9	row5		  2		  row2~row5~row9	  3
 *		row4	row2		  1		  row2~row4			  4
 *		row6	row4		  2		  row2~row4~row6	  5
 *		row8	row6		  3		  row2~row4~row6~row8 6
 *
 * keyid Parent_id 级别分支序列 ------+------------+--------+------------------------ row2 NULL 0 row2 1 row5 row2 1 row2~row5 2 row9 row5 2 row2~row5~row9 3 row4 row2 1 row2~row4 4 row6 row4 2 row2~row4~row6 5 row8 row6 3 row2~row4~row6~row8 6
 *
 */
PG_FUNCTION_INFO_V1(connectby_text);

#define CONNECTBY_NCOLS					4
#define CONNECTBY_NCOLS_NOBRANCH		3

Datum
connectby_text(PG_FUNCTION_ARGS)
{
	char	   *relname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *key_fld = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *parent_key_fld = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char	   *start_with = text_to_cstring(PG_GETARG_TEXT_PP(3));
	int			max_depth = PG_GETARG_INT32(4);
	char	   *branch_delim = NULL;
	bool		show_branch = false;
	bool		show_serial = false;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore
	 *
	 * 检查调用者是否支持我们返回元组存储
	 */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	if (fcinfo->nargs == 6)
	{
		branch_delim = text_to_cstring(PG_GETARG_TEXT_PP(5));
		show_branch = true;
	}
	else
		/* default is no show, tilde for the delimiter
		 *
		 * 默认不显示，波形符为分隔符
		 */
		branch_delim = pstrdup("~");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description
	 *
	 * 获取请求的返回元组描述
	 */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/* does it meet our needs
	 *
	 * 它满足我们的需求吗
	 */
	validateConnectbyTupleDesc(tupdesc, show_branch, show_serial);

	/* OK, use it then
	 *
	 * 好的，那就用吧
	 */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* OK, go to work
	 *
	 * 好的，去上班吧
	 */
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = connectby(relname,
								  key_fld,
								  parent_key_fld,
								  NULL,
								  branch_delim,
								  start_with,
								  max_depth,
								  show_branch,
								  show_serial,
								  per_query_ctx,
								  rsinfo->allowedModes & SFRM_Materialize_Random,
								  attinmeta);
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 *
	 * SFRM_Materialize 模式期望我们返回 NULL Datum。实际的元组位于我们的元组存储中，并通过 rsinfo->setResult 传回。 rsinfo->setDesc 设置为我们实际用来构建元组的元组描述，因此调用者可以验证我们做了它所期望的事情。
	 */
	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(connectby_text_serial);
Datum
connectby_text_serial(PG_FUNCTION_ARGS)
{
	char	   *relname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *key_fld = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *parent_key_fld = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char	   *orderby_fld = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *start_with = text_to_cstring(PG_GETARG_TEXT_PP(4));
	int			max_depth = PG_GETARG_INT32(5);
	char	   *branch_delim = NULL;
	bool		show_branch = false;
	bool		show_serial = true;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore
	 *
	 * 检查调用者是否支持我们返回元组存储
	 */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize) ||
		rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	if (fcinfo->nargs == 7)
	{
		branch_delim = text_to_cstring(PG_GETARG_TEXT_PP(6));
		show_branch = true;
	}
	else
		/* default is no show, tilde for the delimiter
		 *
		 * 默认不显示，波形符为分隔符
		 */
		branch_delim = pstrdup("~");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* get the requested return tuple description
	 *
	 * 获取请求的返回元组描述
	 */
	tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/* does it meet our needs
	 *
	 * 它满足我们的需求吗
	 */
	validateConnectbyTupleDesc(tupdesc, show_branch, show_serial);

	/* OK, use it then
	 *
	 * 好的，那就用吧
	 */
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* OK, go to work
	 *
	 * 好的，去上班吧
	 */
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = connectby(relname,
								  key_fld,
								  parent_key_fld,
								  orderby_fld,
								  branch_delim,
								  start_with,
								  max_depth,
								  show_branch,
								  show_serial,
								  per_query_ctx,
								  rsinfo->allowedModes & SFRM_Materialize_Random,
								  attinmeta);
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 *
	 * SFRM_Materialize 模式期望我们返回 NULL Datum。实际的元组位于我们的元组存储中，并通过 rsinfo->setResult 传回。 rsinfo->setDesc 设置为我们实际用来构建元组的元组描述，因此调用者可以验证我们做了它所期望的事情。
	 */
	return (Datum) 0;
}


/*
 * connectby - does the real work for connectby_text()
 *
 * connectby - 为 connectby_text() 做真正的工作
 */
static Tuplestorestate *
connectby(char *relname,
		  char *key_fld,
		  char *parent_key_fld,
		  char *orderby_fld,
		  char *branch_delim,
		  char *start_with,
		  int max_depth,
		  bool show_branch,
		  bool show_serial,
		  MemoryContext per_query_ctx,
		  bool randomAccess,
		  AttInMetadata *attinmeta)
{
	Tuplestorestate *tupstore = NULL;
	MemoryContext oldcontext;
	int			serial = 1;

	/* Connect to SPI manager
	 *
	 * 连接到 SPI 管理器
	 */
	SPI_connect();

	/* switch to longer term context to create the tuple store
	 *
	 * 切换到长期上下文来创建元组存储
	 */
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* initialize our tuplestore
	 *
	 * 初始化我们的元组存储
	 */
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);

	MemoryContextSwitchTo(oldcontext);

	/* now go get the whole tree
	 *
	 * 现在去获取整棵树
	 */
	build_tuplestore_recursively(key_fld,
								 parent_key_fld,
								 relname,
								 orderby_fld,
								 branch_delim,
								 start_with,
								 start_with,	/* current_branch */
								 0, /* initial level is 0 */
								 &serial,	/* initial serial is 1 */
								 max_depth,
								 show_branch,
								 show_serial,
								 per_query_ctx,
								 attinmeta,
								 tupstore);

	SPI_finish();

	return tupstore;
}

static void
build_tuplestore_recursively(char *key_fld,
							 char *parent_key_fld,
							 char *relname,
							 char *orderby_fld,
							 char *branch_delim,
							 char *start_with,
							 char *branch,
							 int level,
							 int *serial,
							 int max_depth,
							 bool show_branch,
							 bool show_serial,
							 MemoryContext per_query_ctx,
							 AttInMetadata *attinmeta,
							 Tuplestorestate *tupstore)
{
	TupleDesc	tupdesc = attinmeta->tupdesc;
	int			ret;
	uint64		proc;
	int			serial_column;
	StringInfoData sql;
	char	  **values;
	char	   *current_key;
	char	   *current_key_parent;
	char		current_level[INT32_STRLEN];
	char		serial_str[INT32_STRLEN];
	char	   *current_branch;
	HeapTuple	tuple;

	if (max_depth > 0 && level > max_depth)
		return;

	initStringInfo(&sql);

	/* Build initial sql statement
	 *
	 * 构建初始sql语句
	 */
	if (!show_serial)
	{
		appendStringInfo(&sql, "SELECT %s, %s FROM %s WHERE %s = %s AND %s IS NOT NULL AND %s <> %s",
						 key_fld,
						 parent_key_fld,
						 relname,
						 parent_key_fld,
						 quote_literal_cstr(start_with),
						 key_fld, key_fld, parent_key_fld);
		serial_column = 0;
	}
	else
	{
		appendStringInfo(&sql, "SELECT %s, %s FROM %s WHERE %s = %s AND %s IS NOT NULL AND %s <> %s ORDER BY %s",
						 key_fld,
						 parent_key_fld,
						 relname,
						 parent_key_fld,
						 quote_literal_cstr(start_with),
						 key_fld, key_fld, parent_key_fld,
						 orderby_fld);
		serial_column = 1;
	}

	if (show_branch)
		values = (char **) palloc((CONNECTBY_NCOLS + serial_column) * sizeof(char *));
	else
		values = (char **) palloc((CONNECTBY_NCOLS_NOBRANCH + serial_column) * sizeof(char *));

	/* First time through, do a little setup
	 *
	 * 第一次通过，做一些设置
	 */
	if (level == 0)
	{
		/* root value is the one we initially start with
		 *
		 * 根值是我们最初开始的值
		 */
		values[0] = start_with;

		/* root value has no parent
		 *
		 * 根值没有父值
		 */
		values[1] = NULL;

		/* root level is 0
		 *
		 * 根级别为0
		 */
		sprintf(current_level, "%d", level);
		values[2] = current_level;

		/* root branch is just starting root value
		 *
		 * 根分支只是开始根值
		 */
		if (show_branch)
			values[3] = start_with;

		/* root starts the serial with 1
		 *
		 * root 以 1 开始序列
		 */
		if (show_serial)
		{
			sprintf(serial_str, "%d", (*serial)++);
			if (show_branch)
				values[4] = serial_str;
			else
				values[3] = serial_str;
		}

		/* construct the tuple
		 *
		 * 构造元组
		 */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* now store it
		 *
		 * 现在存储它
		 */
		tuplestore_puttuple(tupstore, tuple);

		/* increment level
		 *
		 * 增量级别
		 */
		level++;
	}

	/* Retrieve the desired rows
	 *
	 * 检索所需的行
	 */
	ret = SPI_execute(sql.data, true, 0);
	proc = SPI_processed;

	/* Check for qualifying tuples
	 *
	 * 检查合格的元组
	 */
	if ((ret == SPI_OK_SELECT) && (proc > 0))
	{
		HeapTuple	spi_tuple;
		SPITupleTable *tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = tuptable->tupdesc;
		uint64		i;
		StringInfoData branchstr;
		StringInfoData chk_branchstr;
		StringInfoData chk_current_key;

		/*
		 * Check that return tupdesc is compatible with the one we got from
		 * the query.
		 *
		 * 检查返回的 tupdesc 是否与我们从查询中获得的兼容。
		 */
		compatConnectbyTupleDescs(tupdesc, spi_tupdesc);

		initStringInfo(&branchstr);
		initStringInfo(&chk_branchstr);
		initStringInfo(&chk_current_key);

		for (i = 0; i < proc; i++)
		{
			/* initialize branch for this pass
			 *
			 * 为此通道初始化分支
			 */
			appendStringInfoString(&branchstr, branch);
			appendStringInfo(&chk_branchstr, "%s%s%s", branch_delim, branch, branch_delim);

			/* get the next sql result tuple
			 *
			 * 获取下一个sql结果元组
			 */
			spi_tuple = tuptable->vals[i];

			/* get the current key (might be NULL)
			 *
			 * 获取当前密钥（可能为 NULL）
			 */
			current_key = SPI_getvalue(spi_tuple, spi_tupdesc, 1);

			/* get the parent key (might be NULL)
			 *
			 * 获取父键（可能为 NULL）
			 */
			current_key_parent = SPI_getvalue(spi_tuple, spi_tupdesc, 2);

			/* get the current level
			 *
			 * 获取当前级别
			 */
			sprintf(current_level, "%d", level);

			/* check to see if this key is also an ancestor
			 *
			 * 检查该键是否也是祖先键
			 */
			if (current_key)
			{
				appendStringInfo(&chk_current_key, "%s%s%s",
								 branch_delim, current_key, branch_delim);
				if (strstr(chk_branchstr.data, chk_current_key.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg("infinite recursion detected")));
			}

			/* OK, extend the branch
			 *
			 * OK，扩展分支
			 */
			if (current_key)
				appendStringInfo(&branchstr, "%s%s", branch_delim, current_key);
			current_branch = branchstr.data;

			/* build a tuple
			 *
			 * 构建一个元组
			 */
			values[0] = current_key;
			values[1] = current_key_parent;
			values[2] = current_level;
			if (show_branch)
				values[3] = current_branch;
			if (show_serial)
			{
				sprintf(serial_str, "%d", (*serial)++);
				if (show_branch)
					values[4] = serial_str;
				else
					values[3] = serial_str;
			}

			tuple = BuildTupleFromCStrings(attinmeta, values);

			/* store the tuple for later use
			 *
			 * 存储元组以供以后使用
			 */
			tuplestore_puttuple(tupstore, tuple);

			heap_freetuple(tuple);

			/* recurse using current_key as the new start_with
			 *
			 * 使用 current_key 作为新的 start_with 进行递归
			 */
			if (current_key)
				build_tuplestore_recursively(key_fld,
											 parent_key_fld,
											 relname,
											 orderby_fld,
											 branch_delim,
											 current_key,
											 current_branch,
											 level + 1,
											 serial,
											 max_depth,
											 show_branch,
											 show_serial,
											 per_query_ctx,
											 attinmeta,
											 tupstore);

			xpfree(current_key);
			xpfree(current_key_parent);

			/* reset branch for next pass
			 *
			 * 重置分支以进行下一次传递
			 */
			resetStringInfo(&branchstr);
			resetStringInfo(&chk_branchstr);
			resetStringInfo(&chk_current_key);
		}

		xpfree(branchstr.data);
		xpfree(chk_branchstr.data);
		xpfree(chk_current_key.data);
	}
}

/*
 * Check expected (query runtime) tupdesc suitable for Connectby
 *
 * 检查适合 Connectby 的预期（查询运行时）tupdesc
 */
static void
validateConnectbyTupleDesc(TupleDesc td, bool show_branch, bool show_serial)
{
	int			expected_cols;

	/* are there the correct number of columns
	 *
	 * 列数是否正确
	 */
	if (show_branch)
		expected_cols = CONNECTBY_NCOLS;
	else
		expected_cols = CONNECTBY_NCOLS_NOBRANCH;
	if (show_serial)
		expected_cols++;

	if (td->natts != expected_cols)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Return row must have %d columns, not %d.",
						   expected_cols, td->natts)));

	/* the first two columns will be checked against the input tuples later
	 *
	 * 稍后将根据输入元组检查前两列
	 */

	/* check that the type of the third column is INT4
	 *
	 * 检查第三列的类型是否为 INT4
	 */
	if (TupleDescAttr(td, 2)->atttypid != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Third return column (depth) must be type %s.",
						   format_type_be(INT4OID))));

	/* check that the type of the branch column is TEXT if applicable
	 *
	 * 检查分支列的类型是否为 TEXT（如果适用）
	 */
	if (show_branch && TupleDescAttr(td, 3)->atttypid != TEXTOID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Fourth return column (branch) must be type %s.",
						   format_type_be(TEXTOID))));

	/* check that the type of the serial column is INT4 if applicable
	 *
	 * 检查串行列的类型是否为 INT4（如果适用）
	 */
	if (show_branch && show_serial &&
		TupleDescAttr(td, 4)->atttypid != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Fifth return column (serial) must be type %s.",
						   format_type_be(INT4OID))));
	if (!show_branch && show_serial &&
		TupleDescAttr(td, 3)->atttypid != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Fourth return column (serial) must be type %s.",
						   format_type_be(INT4OID))));

	/* OK, the tupdesc is valid for our purposes
	 *
	 * 好的，tupdesc 对于我们的目的来说是有效的
	 */
}

/*
 * Check if output tupdesc and SQL query's tupdesc are compatible
 *
 * 检查输出 tupdesc 和 SQL 查询的 tupdesc 是否兼容
 */
static void
compatConnectbyTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc)
{
	Oid			ret_atttypid;
	Oid			sql_atttypid;
	int32		ret_atttypmod;
	int32		sql_atttypmod;

	/*
	 * Query result must have at least 2 columns.
	 *
	 * 查询结果必须至少有 2 列。
	 */
	if (sql_tupdesc->natts < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid connectby source data query"),
				 errdetail("The query must return at least two columns.")));

	/*
	 * These columns must match the result type indicated by the calling
	 * query.
	 *
	 * 这些列必须与调用查询指示的结果类型匹配。
	 */
	ret_atttypid = TupleDescAttr(ret_tupdesc, 0)->atttypid;
	sql_atttypid = TupleDescAttr(sql_tupdesc, 0)->atttypid;
	ret_atttypmod = TupleDescAttr(ret_tupdesc, 0)->atttypmod;
	sql_atttypmod = TupleDescAttr(sql_tupdesc, 0)->atttypmod;
	if (ret_atttypid != sql_atttypid ||
		(ret_atttypmod >= 0 && ret_atttypmod != sql_atttypmod))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Source key type %s does not match return key type %s.",
						   format_type_with_typemod(sql_atttypid, sql_atttypmod),
						   format_type_with_typemod(ret_atttypid, ret_atttypmod))));

	ret_atttypid = TupleDescAttr(ret_tupdesc, 1)->atttypid;
	sql_atttypid = TupleDescAttr(sql_tupdesc, 1)->atttypid;
	ret_atttypmod = TupleDescAttr(ret_tupdesc, 1)->atttypmod;
	sql_atttypmod = TupleDescAttr(sql_tupdesc, 1)->atttypmod;
	if (ret_atttypid != sql_atttypid ||
		(ret_atttypmod >= 0 && ret_atttypmod != sql_atttypmod))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid connectby return type"),
				 errdetail("Source parent key type %s does not match return parent key type %s.",
						   format_type_with_typemod(sql_atttypid, sql_atttypmod),
						   format_type_with_typemod(ret_atttypid, ret_atttypmod))));

	/* OK, the two tupdescs are compatible for our purposes
	 *
	 * 好的，这两个 tupdesc 对于我们的目的来说是兼容的
	 */
}

/*
 * Check if crosstab output tupdesc agrees with input tupdesc
 *
 * 检查交叉表输出 tupdesc 是否与输入 tupdesc 一致
 */
static void
compatCrosstabTupleDescs(TupleDesc ret_tupdesc, TupleDesc sql_tupdesc)
{
	int			i;
	Oid			ret_atttypid;
	Oid			sql_atttypid;
	int32		ret_atttypmod;
	int32		sql_atttypmod;

	if (ret_tupdesc->natts < 2)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid crosstab return type"),
				 errdetail("Return row must have at least two columns.")));
	Assert(sql_tupdesc->natts == 3);	/* already checked by caller */

	/* check the row_name types match
	 *
	 * 检查 row_name 类型是否匹配
	 */
	ret_atttypid = TupleDescAttr(ret_tupdesc, 0)->atttypid;
	sql_atttypid = TupleDescAttr(sql_tupdesc, 0)->atttypid;
	ret_atttypmod = TupleDescAttr(ret_tupdesc, 0)->atttypmod;
	sql_atttypmod = TupleDescAttr(sql_tupdesc, 0)->atttypmod;
	if (ret_atttypid != sql_atttypid ||
		(ret_atttypmod >= 0 && ret_atttypmod != sql_atttypmod))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("invalid crosstab return type"),
				 errdetail("Source row_name datatype %s does not match return row_name datatype %s.",
						   format_type_with_typemod(sql_atttypid, sql_atttypmod),
						   format_type_with_typemod(ret_atttypid, ret_atttypmod))));

	/*
	 * attribute [1] of sql tuple is the category; no need to check it
	 * attribute [2] of sql tuple should match attributes [1] to [natts - 1]
	 * of the return tuple
	 *
	 * sql元组的属性[1]是类别；无需检查 sql 元组的属性 [2] 应与返回元组的属性 [1] 到 [natts - 1] 匹配
	 */
	sql_atttypid = TupleDescAttr(sql_tupdesc, 2)->atttypid;
	sql_atttypmod = TupleDescAttr(sql_tupdesc, 2)->atttypmod;
	for (i = 1; i < ret_tupdesc->natts; i++)
	{
		ret_atttypid = TupleDescAttr(ret_tupdesc, i)->atttypid;
		ret_atttypmod = TupleDescAttr(ret_tupdesc, i)->atttypmod;

		if (ret_atttypid != sql_atttypid ||
			(ret_atttypmod >= 0 && ret_atttypmod != sql_atttypmod))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("invalid crosstab return type"),
					 errdetail("Source value datatype %s does not match return value datatype %s in column %d.",
							   format_type_with_typemod(sql_atttypid, sql_atttypmod),
							   format_type_with_typemod(ret_atttypid, ret_atttypmod),
							   i + 1)));
	}

	/* OK, the two tupdescs are compatible for our purposes
	 *
	 * 好的，这两个 tupdesc 对于我们的目的来说是兼容的
	 */
}
