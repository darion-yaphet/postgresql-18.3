/*-------------------------------------------------------------------------
 *
 * conversioncmds.c
 *	  conversion creation command support code
 *
 * CREATE CONVERSION 命令的支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/conversioncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_conversion.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/conversioncmds.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"

/*
 * 核心流程概览：
 * CreateConversionCommand 解析转换名与命名空间，校验 CREATE 权限、
 * 源/目标编码（拒绝 SQL_ASCII）、转换函数的签名与 EXECUTE 权限，
 * 用空串试调用后交给 ConversionCreate 写入目录。
 */
/*
 * CREATE CONVERSION
 *
 * 执行 CREATE CONVERSION。
 */
ObjectAddress
CreateConversionCommand(CreateConversionStmt *stmt)
{
	Oid			namespaceId;
	char	   *conversion_name;
	AclResult	aclresult;
	int			from_encoding;
	int			to_encoding;
	Oid			funcoid;
	const char *from_encoding_name = stmt->for_encoding_name;
	const char *to_encoding_name = stmt->to_encoding_name;
	List	   *func_name = stmt->func_name;
	static const Oid funcargs[] = {INT4OID, INT4OID, CSTRINGOID, INTERNALOID, INT4OID, BOOLOID};
	char		result[1];
	Datum		funcresult;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表拆成名称和命名空间。
	 */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->conversion_name,
													&conversion_name);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceId));

	/* Check the encoding names */
	/*
	 *
	 * 检查编码名称。
	 */
	from_encoding = pg_char_to_encoding(from_encoding_name);
	if (from_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("source encoding \"%s\" does not exist",
						from_encoding_name)));

	to_encoding = pg_char_to_encoding(to_encoding_name);
	if (to_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("destination encoding \"%s\" does not exist",
						to_encoding_name)));

	/*
	 * We consider conversions to or from SQL_ASCII to be meaningless.  (If
	 * you wish to change this, note that pg_do_encoding_conversion() and its
	 * sister functions have hard-wired fast paths for any conversion in which
	 * the source or target encoding is SQL_ASCII, so that an encoding
	 * conversion function declared for such a case will never be used.)
	 *
	 * 与 SQL_ASCII 之间的转换被视为无意义。若要改变这一点，请注意
	 * pg_do_encoding_conversion() 及其同类函数对源或目标为 SQL_ASCII 的转换
	 * 写死了快速路径，因此为此声明的转换函数永远不会被调用。
	 */
	if (from_encoding == PG_SQL_ASCII || to_encoding == PG_SQL_ASCII)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion to or from \"SQL_ASCII\" is not supported")));

	/*
	 * Check the existence of the conversion function. Function name could be
	 * a qualified name.
	 *
	 * 检查转换函数是否存在。函数名可以是限定名。
	 */
	funcoid = LookupFuncName(func_name, sizeof(funcargs) / sizeof(Oid),
							 funcargs, false);

	/* Check it returns int4, else it's probably the wrong function */
	/*
	 *
	 * 检查返回类型是否为 int4，否则很可能不是正确的函数。
	 */
	if (get_func_rettype(funcoid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion function %s must return type %s",
						NameListToString(func_name), "integer")));

	/* Check we have EXECUTE rights for the function */
	/*
	 *
	 * 检查是否拥有该函数的 EXECUTE 权限。
	 */
	aclresult = object_aclcheck(ProcedureRelationId, funcoid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(func_name));

	/*
	 * Check that the conversion function is suitable for the requested source
	 * and target encodings. We do that by calling the function with an empty
	 * string; the conversion function should throw an error if it can't
	 * perform the requested conversion.
	 *
	 * 用空字符串调用转换函数，确认它适用于请求的源编码和目标编码。
	 * 无法完成转换时应报错。
	 */
	funcresult = OidFunctionCall6(funcoid,
								  Int32GetDatum(from_encoding),
								  Int32GetDatum(to_encoding),
								  CStringGetDatum(""),
								  CStringGetDatum(result),
								  Int32GetDatum(0),
								  BoolGetDatum(false));

	/*
	 * The function should return 0 for empty input. Might as well check that,
	 * too.
	 *
	 * 空输入时应返回 0，这里一并检查。
	 */
	if (DatumGetInt32(funcresult) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding conversion function %s returned incorrect result for empty input",
						NameListToString(func_name))));

	/*
	 * All seem ok, go ahead (possible failure would be a duplicate conversion
	 * name)
	 *
	 * 检查通过，继续创建（可能因转换名重复而失败）。
	 */
	return ConversionCreate(conversion_name, namespaceId, GetUserId(),
							from_encoding, to_encoding, funcoid, stmt->def);
}
