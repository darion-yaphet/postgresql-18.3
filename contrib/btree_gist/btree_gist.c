/*
 * contrib/btree_gist/btree_gist.c
 */
#include "postgres.h"

#include "access/cmptype.h"
#include "access/stratnum.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(
					.name = "btree_gist",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(gbt_decompress);
PG_FUNCTION_INFO_V1(gbtreekey_in);
PG_FUNCTION_INFO_V1(gbtreekey_out);
PG_FUNCTION_INFO_V1(gist_translate_cmptype_btree);

/**************************************************
 * In/Out for keys
 *
 * 按键输入/输出
 **************************************************/


Datum
gbtreekey_in(PG_FUNCTION_ARGS)
{
	Oid			typioparam = PG_GETARG_OID(1);

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s",
					format_type_extended(typioparam, -1,
										 FORMAT_TYPE_ALLOW_INVALID))));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

Datum
gbtreekey_out(PG_FUNCTION_ARGS)
{
	/* Sadly, we do not receive any indication of the specific type
	 *
	 * 遗憾的是，我们没有收到任何具体类型的指示
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot display a value of type %s", "gbtreekey?")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}


/*
** GiST DeCompress methods
** do not do anything.
*
* * GiST Decompress 方法 * 不执行任何操作。
*/
Datum
gbt_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

/*
 * Returns the btree number for supported operators, otherwise invalid.
 *
 * 返回支持的运算符的 btree 编号，否则无效。
 */
Datum
gist_translate_cmptype_btree(PG_FUNCTION_ARGS)
{
	CompareType cmptype = PG_GETARG_INT32(0);

	switch (cmptype)
	{
		case COMPARE_EQ:
			PG_RETURN_UINT16(BTEqualStrategyNumber);
		case COMPARE_LT:
			PG_RETURN_UINT16(BTLessStrategyNumber);
		case COMPARE_LE:
			PG_RETURN_UINT16(BTLessEqualStrategyNumber);
		case COMPARE_GT:
			PG_RETURN_UINT16(BTGreaterStrategyNumber);
		case COMPARE_GE:
			PG_RETURN_UINT16(BTGreaterEqualStrategyNumber);
		default:
			PG_RETURN_UINT16(InvalidStrategy);
	}
}
