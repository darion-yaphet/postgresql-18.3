/*
 * contrib/hstore/hstore_gin.c
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"

#include "hstore.h"


/*
 * When using a GIN index for hstore, we choose to index both keys and values.
 * The storage format is "text" values, with K, V, or N prepended to the string
 * to indicate key, value, or null values.  (As of 9.1 it might be better to
 * store null values as nulls, but we'll keep it this way for on-disk
 * compatibility.)
 *
 * 当对 hstore 使用 GIN 索引时，我们选择对键和值都建立索引。存储格式是“文本”值，在字符串前面添加 K、V 或 N 来指示键、值或空值。  （从 9.1 开始，将 null 值存储为 null 可能会更好，但为了磁盘兼容性，我们将保持这种方式。）
 */
#define KEYFLAG		'K'
#define VALFLAG		'V'
#define NULLFLAG	'N'

PG_FUNCTION_INFO_V1(gin_extract_hstore);

/* Build an indexable text value
 *
 * 构建可索引的文本值
 */
static text *
makeitem(char *str, int len, char flag)
{
	text	   *item;

	item = (text *) palloc(VARHDRSZ + len + 1);
	SET_VARSIZE(item, VARHDRSZ + len + 1);

	*VARDATA(item) = flag;

	if (str && len > 0)
		memcpy(VARDATA(item) + 1, str, len);

	return item;
}

Datum
gin_extract_hstore(PG_FUNCTION_ARGS)
{
	HStore	   *hs = PG_GETARG_HSTORE_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	HEntry	   *hsent = ARRPTR(hs);
	char	   *ptr = STRPTR(hs);
	int			count = HS_COUNT(hs);
	int			i;

	*nentries = 2 * count;
	if (count)
		entries = (Datum *) palloc(sizeof(Datum) * 2 * count);

	for (i = 0; i < count; ++i)
	{
		text	   *item;

		item = makeitem(HSTORE_KEY(hsent, ptr, i),
						HSTORE_KEYLEN(hsent, i),
						KEYFLAG);
		entries[2 * i] = PointerGetDatum(item);

		if (HSTORE_VALISNULL(hsent, i))
			item = makeitem(NULL, 0, NULLFLAG);
		else
			item = makeitem(HSTORE_VAL(hsent, ptr, i),
							HSTORE_VALLEN(hsent, i),
							VALFLAG);
		entries[2 * i + 1] = PointerGetDatum(item);
	}

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(gin_extract_hstore_query);

Datum
gin_extract_hstore_query(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy == HStoreContainsStrategyNumber)
	{
		/* Query is an hstore, so just apply gin_extract_hstore...
		 *
		 * 查询是一个 hstore，所以只需应用 gin_extract_hstore...
		 */
		entries = (Datum *)
			DatumGetPointer(DirectFunctionCall2(gin_extract_hstore,
												PG_GETARG_DATUM(0),
												PointerGetDatum(nentries)));
		/* ... except that "contains {}" requires a full index scan
		 *
		 * ...除了“包含 {}”需要完整索引扫描
		 */
		if (entries == NULL)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		text	   *query = PG_GETARG_TEXT_PP(0);
		text	   *item;

		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		item = makeitem(VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query), KEYFLAG);
		entries[0] = PointerGetDatum(item);
	}
	else if (strategy == HStoreExistsAnyStrategyNumber ||
			 strategy == HStoreExistsAllStrategyNumber)
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i,
					j;
		text	   *item;

		deconstruct_array_builtin(query, TEXTOID, &key_datums, &key_nulls, &key_count);

		entries = (Datum *) palloc(sizeof(Datum) * key_count);

		for (i = 0, j = 0; i < key_count; ++i)
		{
			/* Nulls in the array are ignored, cf hstoreArrayToPairs
			 *
			 * 数组中的空值将被忽略，参见 hstoreArrayToPairs
			 */
			if (key_nulls[i])
				continue;
			item = makeitem(VARDATA(key_datums[i]), VARSIZE(key_datums[i]) - VARHDRSZ, KEYFLAG);
			entries[j++] = PointerGetDatum(item);
		}

		*nentries = j;
		/* ExistsAll with no keys should match everything
		 *
		 * 没有键的 ExistsAll 应该匹配所有内容
		 */
		if (j == 0 && strategy == HStoreExistsAllStrategyNumber)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else
	{
		elog(ERROR, "unrecognized strategy number: %d", strategy);
		entries = NULL;			/* keep compiler quiet */
	}

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(gin_consistent_hstore);

Datum
gin_consistent_hstore(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* HStore	   *query = PG_GETARG_HSTORE_P(2);
	 *
	 * HStore *查询 = PG_GETARG_HSTORE_P(2);
	 */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	 *
	 * 指针 *extra_data = (指针 *) PG_GETARG_POINTER(4);
	 */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == HStoreContainsStrategyNumber)
	{
		/*
		 * Index doesn't have information about correspondence of keys and
		 * values, so we need recheck.  However, if not all the keys are
		 * present, we can fail at once.
		 *
		 * 索引没有关于键和值对应关系的信息，因此我们需要重新检查。  然而，如果不是所有的键都存在，我们可能会立即失败。
		 */
		*recheck = true;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == HStoreExistsStrategyNumber)
	{
		/* Existence of key is guaranteed in default search mode
		 *
		 * 默认搜索模式下保证key存在
		 */
		*recheck = false;
		res = true;
	}
	else if (strategy == HStoreExistsAnyStrategyNumber)
	{
		/* Existence of key is guaranteed in default search mode
		 *
		 * 默认搜索模式下保证key存在
		 */
		*recheck = false;
		res = true;
	}
	else if (strategy == HStoreExistsAllStrategyNumber)
	{
		/* Testing for all the keys being present gives an exact result
		 *
		 * 测试所有存在的键会给出准确的结果
		 */
		*recheck = false;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}
