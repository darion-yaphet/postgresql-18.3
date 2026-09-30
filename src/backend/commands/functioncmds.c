/*-------------------------------------------------------------------------
 *
 * functioncmds.c
 *
 * 源文件 functioncmds.c。
 *
 *	  Routines for CREATE and DROP FUNCTION commands and CREATE and DROP
 *	  CAST commands.
 *
 * CREATE FUNCTION、DROP FUNCTION 以及 CREATE CAST、DROP CAST 命令的例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/functioncmds.c
 *
 * DESCRIPTION
 *	  These routines take the parse tree and pick out the
 *	  appropriate arguments/flags, and pass the results to the
 *	  corresponding "FooCreate" routines (in src/backend/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * 说明：这些例程从解析树中取出相应的参数和标志，交给目录模块中真正改写系统表的 FooCreate 例程。它们也会校验用户是否有权执行该命令。
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send procedures
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 * 说明：这些对象必须按下列顺序定义并提交：CREATE FUNCTION 的输入/输出与 recv/send 过程；CREATE TYPE 的类型；CREATE OPERATOR 的操作符。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/proclang.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "pgstat.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * 核心流程概览：
 * CreateFunction：解析 RETURNS、参数与选项，校验语言权限后调用 ProcedureCreate。
 * RemoveFunctionById：按 OID 删除 pg_proc；若存在 pg_aggregate 元组则一并删除。
 * AlterFunction：校验属主后更新 volatility、parallel、SUPPORT、proconfig 等属性。
 * CreateCast：校验类型与转换方法；二进制兼容转换仅超级用户可创建。
 * CreateTransform：登记类型与语言对应的 transform 函数及其依赖。
 * ExecuteDoStmt：执行匿名过程语言代码块。
 * ExecuteCallStmt：调用过程，处理输出，并限制非原子上下文中的事务控制。
 */

/*
 *	 Examine the RETURNS clause of the CREATE FUNCTION statement
 *	 and return information about it as *prorettype_p and *returnsSet_p.
 *
 * 检查 CREATE FUNCTION 语句的 RETURNS 子句，把相关信息通过 *prorettype_p 和 *returnsSet_p 返回。
 *
 * This is more complex than the average typename lookup because we want to
 * allow a shell type to be used, or even created if the specified return type
 * doesn't exist yet.  (Without this, there's no way to define the I/O procs
 * for a new type.)  But SQL function creation won't cope, so error out if
 * the target language is SQL.  (We do this here, not in the SQL-function
 * validator, so as not to produce a NOTICE and then an ERROR for the same
 * condition.)
 *
 * 这比普通的类型名查找更复杂，因为我们允许使用外壳类型，甚至在指定的返回类型尚不存在时创建它。
 * （否则无法为新类型定义 I/O 过程。）但创建 SQL 函数无法应付这种情况，因此若目标语言是 SQL 则报错。
 * （在这里做，而不是在 SQL 函数校验器里做，以免对同一条件先 NOTICE 再 ERROR。）
 */
static void
compute_return_type(TypeName *returnType, Oid languageOid,
					Oid *prorettype_p, bool *returnsSet_p)
{
	Oid			rettype;
	Type		typtup;
	AclResult	aclresult;

	typtup = LookupTypeName(NULL, returnType, NULL, false);

	if (typtup)
	{
		if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
		{
			if (languageOid == SQLlanguageId)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function cannot return shell type %s",
								TypeNameToString(returnType))));
			else
				ereport(NOTICE,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("return type %s is only a shell",
								TypeNameToString(returnType))));
		}
		rettype = typeTypeId(typtup);
		ReleaseSysCache(typtup);
	}
	else
	{
		char	   *typnam = TypeNameToString(returnType);
		Oid			namespaceId;
		char	   *typname;
		ObjectAddress address;

		/*
		 * Only C-coded functions can be I/O functions.  We enforce this
		 * restriction here mainly to prevent littering the catalogs with
		 * shell types due to simple typos in user-defined function
		 * definitions.
		 *
		 * 只有 C 编写的函数才能作为 I/O 函数。这里强制这一限制，主要是防止用户定义函数时的简单拼写错误往目录里留下外壳类型。
		 */
		if (languageOid != INTERNALlanguageId &&
			languageOid != ClanguageId)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist", typnam)));

		/* Reject if there's typmod decoration, too */
		/*
		 *
		 * 若还有 typmod 修饰，也拒绝
		 */
		if (returnType->typmods != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type modifier cannot be specified for shell type \"%s\"",
							typnam)));

		/* Otherwise, go ahead and make a shell type */
		/*
		 *
		 * 否则，继续创建一个外壳类型
		 */
		ereport(NOTICE,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" is not yet defined", typnam),
				 errdetail("Creating a shell type definition.")));
		namespaceId = QualifiedNameGetCreationNamespace(returnType->names,
														&typname);
		aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(namespaceId));
		address = TypeShellMake(typname, namespaceId, GetUserId());
		rettype = address.objectId;
		Assert(OidIsValid(rettype));
	}

	aclresult = object_aclcheck(TypeRelationId, rettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, rettype);

	*prorettype_p = rettype;
	*returnsSet_p = returnType->setof;
}

/*
 * Interpret the function parameter list of a CREATE FUNCTION,
 * CREATE PROCEDURE, or CREATE AGGREGATE statement.
 *
 * 解释 CREATE FUNCTION、CREATE PROCEDURE 或 CREATE AGGREGATE 语句的函数参数列表。
 *
 * Input parameters:
 * parameters: list of FunctionParameter structs
 * languageOid: OID of function language (InvalidOid if it's CREATE AGGREGATE)
 * objtype: identifies type of object being created
 *
 * 输入参数：parameters 是 FunctionParameter 结构的列表；languageOid 是函数语言的 OID
 * （CREATE AGGREGATE 时为 InvalidOid）；objtype 标识正在创建的对象类型。
 *
 * Results are stored into output parameters.  parameterTypes must always
 * be created, but the other arrays/lists can be NULL pointers if not needed.
 * variadicArgType is set to the variadic array type if there's a VARIADIC
 * parameter (there can be only one); or to InvalidOid if not.
 * requiredResultType is set to InvalidOid if there are no OUT parameters,
 * else it is set to the OID of the implied result type.
 *
 * 结果写入输出参数。parameterTypes 必须总是创建，其他数组或列表若不需要可以为 NULL。
 * 若有 VARIADIC 参数（只能有一个），variadicArgType 设为该可变数组类型，否则为 InvalidOid。
 * 若没有 OUT 参数，requiredResultType 为 InvalidOid，否则设为隐含结果类型的 OID。
 */
void
interpret_function_parameter_list(ParseState *pstate,
								  List *parameters,
								  Oid languageOid,
								  ObjectType objtype,
								  oidvector **parameterTypes,
								  List **parameterTypes_list,
								  ArrayType **allParameterTypes,
								  ArrayType **parameterModes,
								  ArrayType **parameterNames,
								  List **inParameterNames_list,
								  List **parameterDefaults,
								  Oid *variadicArgType,
								  Oid *requiredResultType)
{
	int			parameterCount = list_length(parameters);
	Oid		   *inTypes;
	int			inCount = 0;
	Datum	   *allTypes;
	Datum	   *paramModes;
	Datum	   *paramNames;
	int			outCount = 0;
	int			varCount = 0;
	bool		have_names = false;
	bool		have_defaults = false;
	ListCell   *x;
	int			i;

	*variadicArgType = InvalidOid;	/* default result */
	/*
	 *
	 * 默认结果
	 */
	*requiredResultType = InvalidOid;	/* default result */
	/*
	 *
	 * 默认结果
	 */

	inTypes = (Oid *) palloc(parameterCount * sizeof(Oid));
	allTypes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramModes = (Datum *) palloc(parameterCount * sizeof(Datum));
	paramNames = (Datum *) palloc0(parameterCount * sizeof(Datum));
	*parameterDefaults = NIL;

	/* Scan the list and extract data into work arrays */
	/*
	 *
	 * 扫描列表并把数据提取到工作数组
	 */
	i = 0;
	foreach(x, parameters)
	{
		FunctionParameter *fp = (FunctionParameter *) lfirst(x);
		TypeName   *t = fp->argType;
		FunctionParameterMode fpmode = fp->mode;
		bool		isinput = false;
		Oid			toid;
		Type		typtup;
		AclResult	aclresult;

		/* For our purposes here, a defaulted mode spec is identical to IN */
		/*
		 *
		 * 就这里的目的而言，缺省的模式说明等同于 IN
		 */
		if (fpmode == FUNC_PARAM_DEFAULT)
			fpmode = FUNC_PARAM_IN;

		typtup = LookupTypeName(pstate, t, NULL, false);
		if (typtup)
		{
			if (!((Form_pg_type) GETSTRUCT(typtup))->typisdefined)
			{
				/* As above, hard error if language is SQL */
				/*
				 *
				 * 同上，若语言是 SQL 则硬性报错
				 */
				if (languageOid == SQLlanguageId)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("SQL function cannot accept shell type %s",
									TypeNameToString(t)),
							 parser_errposition(pstate, t->location)));
				/* We don't allow creating aggregates on shell types either */
				/*
				 *
				 * 也不允许在外壳类型上创建聚集
				 */
				else if (objtype == OBJECT_AGGREGATE)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("aggregate cannot accept shell type %s",
									TypeNameToString(t)),
							 parser_errposition(pstate, t->location)));
				else
					ereport(NOTICE,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("argument type %s is only a shell",
									TypeNameToString(t)),
							 parser_errposition(pstate, t->location)));
			}
			toid = typeTypeId(typtup);
			ReleaseSysCache(typtup);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type %s does not exist",
							TypeNameToString(t)),
					 parser_errposition(pstate, t->location)));
			toid = InvalidOid;	/* keep compiler quiet */
			/*
			 *
			 * 让编译器保持安静
			 */
		}

		aclresult = object_aclcheck(TypeRelationId, toid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, toid);

		if (t->setof)
		{
			if (objtype == OBJECT_AGGREGATE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregates cannot accept set arguments"),
						 parser_errposition(pstate, fp->location)));
			else if (objtype == OBJECT_PROCEDURE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("procedures cannot accept set arguments"),
						 parser_errposition(pstate, fp->location)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("functions cannot accept set arguments"),
						 parser_errposition(pstate, fp->location)));
		}

		/* handle input parameters */
		/*
		 *
		 * 处理输入参数
		 */
		if (fpmode != FUNC_PARAM_OUT && fpmode != FUNC_PARAM_TABLE)
		{
			/* other input parameters can't follow a VARIADIC parameter */
			/*
			 *
			 * 其他输入参数不能跟在 VARIADIC 参数之后
			 */
			if (varCount > 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("VARIADIC parameter must be the last input parameter"),
						 parser_errposition(pstate, fp->location)));
			inTypes[inCount++] = toid;
			isinput = true;
			if (parameterTypes_list)
				*parameterTypes_list = lappend_oid(*parameterTypes_list, toid);
		}

		/* handle output parameters */
		/*
		 *
		 * 处理输出参数
		 */
		if (fpmode != FUNC_PARAM_IN && fpmode != FUNC_PARAM_VARIADIC)
		{
			if (objtype == OBJECT_PROCEDURE)
			{
				/*
				 * We disallow OUT-after-VARIADIC only for procedures.  While
				 * such a case causes no confusion in ordinary function calls,
				 * it would cause confusion in a CALL statement.
				 *
				 * 只对过程禁止 VARIADIC 之后再出现 OUT。普通函数调用中这种情况不会造成混淆，但在 CALL 语句中会。
				 */
				if (varCount > 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("VARIADIC parameter must be the last parameter"),
							 parser_errposition(pstate, fp->location)));
				/* Procedures with output parameters always return RECORD */
				/*
				 *
				 * 带输出参数的过程总是返回 RECORD
				 */
				*requiredResultType = RECORDOID;
			}
			else if (outCount == 0) /* save first output param's type */
			/*
			 *
			 * 保存第一个输出参数的类型
			 */
				*requiredResultType = toid;
			outCount++;
		}

		if (fpmode == FUNC_PARAM_VARIADIC)
		{
			*variadicArgType = toid;
			varCount++;
			/* validate variadic parameter type */
			/*
			 *
			 * 校验可变参数的类型
			 */
			switch (toid)
			{
				case ANYARRAYOID:
				case ANYCOMPATIBLEARRAYOID:
				case ANYOID:
					/* okay */
					/*
					 *
					 * 可以
					 */
					break;
				default:
					if (!OidIsValid(get_element_type(toid)))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("VARIADIC parameter must be an array"),
								 parser_errposition(pstate, fp->location)));
					break;
			}
		}

		allTypes[i] = ObjectIdGetDatum(toid);

		paramModes[i] = CharGetDatum(fpmode);

		if (fp->name && fp->name[0])
		{
			ListCell   *px;

			/*
			 * As of Postgres 9.0 we disallow using the same name for two
			 * input or two output function parameters.  Depending on the
			 * function's language, conflicting input and output names might
			 * be bad too, but we leave it to the PL to complain if so.
			 *
			 * 自 Postgres 9.0 起，不允许两个输入参数或两个输出参数使用相同的名字。
			 * 视函数语言而定，输入名与输出名冲突也可能不好，但若如此，留给过程语言去抱怨。
			 */
			foreach(px, parameters)
			{
				FunctionParameter *prevfp = (FunctionParameter *) lfirst(px);
				FunctionParameterMode prevfpmode;

				if (prevfp == fp)
					break;
				/* as above, default mode is IN */
				/*
				 *
				 * 同上，默认模式是 IN
				 */
				prevfpmode = prevfp->mode;
				if (prevfpmode == FUNC_PARAM_DEFAULT)
					prevfpmode = FUNC_PARAM_IN;
				/* pure in doesn't conflict with pure out */
				/*
				 *
				 * 纯 IN 与纯 OUT 不冲突
				 */
				if ((fpmode == FUNC_PARAM_IN ||
					 fpmode == FUNC_PARAM_VARIADIC) &&
					(prevfpmode == FUNC_PARAM_OUT ||
					 prevfpmode == FUNC_PARAM_TABLE))
					continue;
				if ((prevfpmode == FUNC_PARAM_IN ||
					 prevfpmode == FUNC_PARAM_VARIADIC) &&
					(fpmode == FUNC_PARAM_OUT ||
					 fpmode == FUNC_PARAM_TABLE))
					continue;
				if (prevfp->name && prevfp->name[0] &&
					strcmp(prevfp->name, fp->name) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("parameter name \"%s\" used more than once",
									fp->name),
							 parser_errposition(pstate, fp->location)));
			}

			paramNames[i] = CStringGetTextDatum(fp->name);
			have_names = true;
		}

		if (inParameterNames_list)
			*inParameterNames_list = lappend(*inParameterNames_list, makeString(fp->name ? fp->name : pstrdup("")));

		if (fp->defexpr)
		{
			Node	   *def;

			if (!isinput)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("only input parameters can have default values"),
						 parser_errposition(pstate, fp->location)));

			def = transformExpr(pstate, fp->defexpr,
								EXPR_KIND_FUNCTION_DEFAULT);
			def = coerce_to_specific_type(pstate, def, toid, "DEFAULT");
			assign_expr_collations(pstate, def);

			/*
			 * Make sure no variables are referred to (this is probably dead
			 * code now that add_missing_from is history).
			 *
			 * 确保没有引用变量（add_missing_from 已成为历史之后，这段很可能是死代码）。
			 */
			if (pstate->p_rtable != NIL ||
				contain_var_clause(def))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
						 errmsg("cannot use table references in parameter default value"),
						 parser_errposition(pstate, fp->location)));

			/*
			 * transformExpr() should have already rejected subqueries,
			 * aggregates, and window functions, based on the EXPR_KIND_ for a
			 * default expression.
			 *
			 * transformExpr() 应根据默认表达式的 EXPR_KIND_，已经拒绝了子查询、聚集和窗口函数。
			 *
			 * It can't return a set either --- but coerce_to_specific_type
			 * already checked that for us.
			 *
			 * 它也不能返回集合，但 coerce_to_specific_type 已经替我们检查过了。
			 *
			 * Note: the point of these restrictions is to ensure that an
			 * expression that, on its face, hasn't got subplans, aggregates,
			 * etc cannot suddenly have them after function default arguments
			 * are inserted.
			 *
			 * 注意：这些限制是为了保证，一个表面上没有子计划、聚集等内容的表达式，
			 * 在插入函数默认参数之后不会突然出现这些东西。
			 */

			*parameterDefaults = lappend(*parameterDefaults, def);
			have_defaults = true;
		}
		else
		{
			if (isinput && have_defaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("input parameters after one with a default value must also have defaults"),
						 parser_errposition(pstate, fp->location)));

			/*
			 * For procedures, we also can't allow OUT parameters after one
			 * with a default, because the same sort of confusion arises in a
			 * CALL statement.
			 *
			 * 对过程而言，也不能允许在带默认值的参数之后再出现 OUT 参数，因为 CALL 语句中会出现同类混淆。
			 */
			if (objtype == OBJECT_PROCEDURE && have_defaults)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("procedure OUT parameters cannot appear after one with a default value"),
						 parser_errposition(pstate, fp->location)));
		}

		i++;
	}

	/* Now construct the proper outputs as needed */
	/*
	 *
	 * 现在按需要构造正确的输出
	 */
	*parameterTypes = buildoidvector(inTypes, inCount);

	if (outCount > 0 || varCount > 0)
	{
		*allParameterTypes = construct_array_builtin(allTypes, parameterCount, OIDOID);
		*parameterModes = construct_array_builtin(paramModes, parameterCount, CHAROID);
		if (outCount > 1)
			*requiredResultType = RECORDOID;
		/* otherwise we set requiredResultType correctly above */
		/*
		 *
		 * 否则我们已在上面正确设置了 requiredResultType
		 */
	}
	else
	{
		*allParameterTypes = NULL;
		*parameterModes = NULL;
	}

	if (have_names)
	{
		for (i = 0; i < parameterCount; i++)
		{
			if (paramNames[i] == PointerGetDatum(NULL))
				paramNames[i] = CStringGetTextDatum("");
		}
		*parameterNames = construct_array_builtin(paramNames, parameterCount, TEXTOID);
	}
	else
		*parameterNames = NULL;
}


/*
 * Recognize one of the options that can be passed to both CREATE
 * FUNCTION and ALTER FUNCTION and return it via one of the out
 * parameters. Returns true if the passed option was recognized. If
 * the out parameter we were going to assign to points to non-NULL,
 * raise a duplicate-clause error.  (We don't try to detect duplicate
 * SET parameters though --- if you're redundant, the last one wins.)
 *
 * 识别既能传给 CREATE FUNCTION 也能传给 ALTER FUNCTION 的选项之一，并通过某个输出参数返回。
 * 若识别了传入的选项则返回 true。若即将赋值的输出参数已非 NULL，则报重复子句错误。
 * （我们不试图检测重复的 SET 参数：若写重了，以后面的为准。）
 */
static bool
compute_common_attribute(ParseState *pstate,
						 bool is_procedure,
						 DefElem *defel,
						 DefElem **volatility_item,
						 DefElem **strict_item,
						 DefElem **security_item,
						 DefElem **leakproof_item,
						 List **set_items,
						 DefElem **cost_item,
						 DefElem **rows_item,
						 DefElem **support_item,
						 DefElem **parallel_item)
{
	if (strcmp(defel->defname, "volatility") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*volatility_item)
			errorConflictingDefElem(defel, pstate);

		*volatility_item = defel;
	}
	else if (strcmp(defel->defname, "strict") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*strict_item)
			errorConflictingDefElem(defel, pstate);

		*strict_item = defel;
	}
	else if (strcmp(defel->defname, "security") == 0)
	{
		if (*security_item)
			errorConflictingDefElem(defel, pstate);

		*security_item = defel;
	}
	else if (strcmp(defel->defname, "leakproof") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*leakproof_item)
			errorConflictingDefElem(defel, pstate);

		*leakproof_item = defel;
	}
	else if (strcmp(defel->defname, "set") == 0)
	{
		*set_items = lappend(*set_items, defel->arg);
	}
	else if (strcmp(defel->defname, "cost") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*cost_item)
			errorConflictingDefElem(defel, pstate);

		*cost_item = defel;
	}
	else if (strcmp(defel->defname, "rows") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*rows_item)
			errorConflictingDefElem(defel, pstate);

		*rows_item = defel;
	}
	else if (strcmp(defel->defname, "support") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*support_item)
			errorConflictingDefElem(defel, pstate);

		*support_item = defel;
	}
	else if (strcmp(defel->defname, "parallel") == 0)
	{
		if (is_procedure)
			goto procedure_error;
		if (*parallel_item)
			errorConflictingDefElem(defel, pstate);

		*parallel_item = defel;
	}
	else
		return false;

	/* Recognized an option */
	/*
	 *
	 * 识别了一个选项
	 */
	return true;

procedure_error:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("invalid attribute in procedure definition"),
			 parser_errposition(pstate, defel->location)));
	return false;
}

/*
 * 把 volatility 选项解析为 PROVOLATILE_IMMUTABLE、STABLE 或 VOLATILE。
 */
static char
interpret_func_volatility(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "immutable") == 0)
		return PROVOLATILE_IMMUTABLE;
	else if (strcmp(str, "stable") == 0)
		return PROVOLATILE_STABLE;
	else if (strcmp(str, "volatile") == 0)
		return PROVOLATILE_VOLATILE;
	else
	{
		elog(ERROR, "invalid volatility \"%s\"", str);
		return 0;				/* keep compiler quiet */
		/*
		 *
		 * 让编译器保持安静
		 */
	}
}

/*
 * 把 parallel 选项解析为 PROPARALLEL_SAFE、RESTRICTED 或 UNSAFE。
 */
static char
interpret_func_parallel(DefElem *defel)
{
	char	   *str = strVal(defel->arg);

	if (strcmp(str, "safe") == 0)
		return PROPARALLEL_SAFE;
	else if (strcmp(str, "unsafe") == 0)
		return PROPARALLEL_UNSAFE;
	else if (strcmp(str, "restricted") == 0)
		return PROPARALLEL_RESTRICTED;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"parallel\" must be SAFE, RESTRICTED, or UNSAFE")));
		return PROPARALLEL_UNSAFE;	/* keep compiler quiet */
		/*
		 *
		 * 让编译器保持安静
		 */
	}
}

/*
 * Update a proconfig value according to a list of VariableSetStmt items.
 *
 * 根据 VariableSetStmt 项的列表更新 proconfig 值。
 *
 * The input and result may be NULL to signify a null entry.
 *
 * 输入和结果都可以是 NULL，表示空项。
 */
static ArrayType *
update_proconfig_value(ArrayType *a, List *set_items)
{
	ListCell   *l;

	foreach(l, set_items)
	{
		VariableSetStmt *sstmt = lfirst_node(VariableSetStmt, l);

		if (sstmt->kind == VAR_RESET_ALL)
			a = NULL;
		else
		{
			char	   *valuestr = ExtractSetVariableArgs(sstmt);

			if (valuestr)
				a = GUCArrayAdd(a, sstmt->name, valuestr);
			else				/* RESET */
			/*
			 *
			 * RESET 语句。
			 */
				a = GUCArrayDelete(a, sstmt->name);
		}
	}

	return a;
}

/*
 * 解析 SUPPORT 选项给出的函数，并校验其接受并返回 INTERNAL。
 */
static Oid
interpret_func_support(DefElem *defel)
{
	List	   *procName = defGetQualifiedName(defel);
	Oid			procOid;
	Oid			argList[1];

	/*
	 * Support functions always take one INTERNAL argument and return
	 * INTERNAL.
	 *
	 * 支持函数总是接受一个 INTERNAL 参数并返回 INTERNAL。
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procName, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procName, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("support function %s must return type %s",
						NameListToString(procName), "internal")));

	/*
	 * Someday we might want an ACL check here; but for now, we insist that
	 * you be superuser to specify a support function, so privilege on the
	 * support function is moot.
	 *
	 * 将来也许要在这里做 ACL 检查；但目前规定指定支持函数必须是超级用户，因此支持函数本身的权限无关紧要。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to specify a support function")));

	return procOid;
}


/*
 * Dissect the list of options assembled in gram.y into function
 * attributes.
 *
 * 把 gram.y 中组装的选项列表拆成函数属性。
 */
static void
compute_function_attributes(ParseState *pstate,
							bool is_procedure,
							List *options,
							List **as,
							char **language,
							Node **transform,
							bool *windowfunc_p,
							char *volatility_p,
							bool *strict_p,
							bool *security_definer,
							bool *leakproof_p,
							ArrayType **proconfig,
							float4 *procost,
							float4 *prorows,
							Oid *prosupport,
							char *parallel_p)
{
	ListCell   *option;
	DefElem    *as_item = NULL;
	DefElem    *language_item = NULL;
	DefElem    *transform_item = NULL;
	DefElem    *windowfunc_item = NULL;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_item = NULL;
	DefElem    *leakproof_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;
	DefElem    *support_item = NULL;
	DefElem    *parallel_item = NULL;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_item)
				errorConflictingDefElem(defel, pstate);
			as_item = defel;
		}
		else if (strcmp(defel->defname, "language") == 0)
		{
			if (language_item)
				errorConflictingDefElem(defel, pstate);
			language_item = defel;
		}
		else if (strcmp(defel->defname, "transform") == 0)
		{
			if (transform_item)
				errorConflictingDefElem(defel, pstate);
			transform_item = defel;
		}
		else if (strcmp(defel->defname, "window") == 0)
		{
			if (windowfunc_item)
				errorConflictingDefElem(defel, pstate);
			if (is_procedure)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("invalid attribute in procedure definition"),
						 parser_errposition(pstate, defel->location)));
			windowfunc_item = defel;
		}
		else if (compute_common_attribute(pstate,
										  is_procedure,
										  defel,
										  &volatility_item,
										  &strict_item,
										  &security_item,
										  &leakproof_item,
										  &set_items,
										  &cost_item,
										  &rows_item,
										  &support_item,
										  &parallel_item))
		{
			/* recognized common option */
			/*
			 *
			 * 识别了公共选项
			 */
			continue;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (as_item)
		*as = (List *) as_item->arg;
	if (language_item)
		*language = strVal(language_item->arg);
	if (transform_item)
		*transform = transform_item->arg;
	if (windowfunc_item)
		*windowfunc_p = boolVal(windowfunc_item->arg);
	if (volatility_item)
		*volatility_p = interpret_func_volatility(volatility_item);
	if (strict_item)
		*strict_p = boolVal(strict_item->arg);
	if (security_item)
		*security_definer = boolVal(security_item->arg);
	if (leakproof_item)
		*leakproof_p = boolVal(leakproof_item->arg);
	if (set_items)
		*proconfig = update_proconfig_value(NULL, set_items);
	if (cost_item)
	{
		*procost = defGetNumeric(cost_item);
		if (*procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		*prorows = defGetNumeric(rows_item);
		if (*prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
	}
	if (support_item)
		*prosupport = interpret_func_support(support_item);
	if (parallel_item)
		*parallel_p = interpret_func_parallel(parallel_item);
}


/*
 * For a dynamically linked C language object, the form of the clause is
 *
 * 对于动态链接的 C 语言对象，子句形式是
 *
 *	   AS <object file name> [, <link symbol name> ]
 *
 * AS 对象文件名 [, 链接符号名 ]
 *
 * In all other cases
 *
 * 在所有其他情况下
 *
 *	   AS <object reference, or sql code>
 *
 * AS 对象引用，或 SQL 代码
 */
static void
interpret_AS_clause(Oid languageOid, const char *languageName,
					char *funcname, List *as, Node *sql_body_in,
					List *parameterTypes, List *inParameterNames,
					char **prosrc_str_p, char **probin_str_p,
					Node **sql_body_out,
					const char *queryString)
{
	if (!sql_body_in && !as)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("no function body specified")));

	if (sql_body_in && as)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("duplicate function body specified")));

	if (sql_body_in && languageOid != SQLlanguageId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("inline SQL function body only valid for language SQL")));

	*sql_body_out = NULL;

	if (languageOid == ClanguageId)
	{
		/*
		 * For "C" language, store the file name in probin and, when given,
		 * the link symbol name in prosrc.  If link symbol is omitted,
		 * substitute procedure name.  We also allow link symbol to be
		 * specified as "-", since that was the habit in PG versions before
		 * 8.4, and there might be dump files out there that don't translate
		 * that back to "omitted".
		 *
		 * 对于 C 语言，把文件名存入 probin；若给出了链接符号名，则存入 prosrc。
		 * 若省略链接符号，则用过程名代替。也允许把链接符号写成 "-"，
		 * 因为 PG 8.4 之前习惯如此，而且可能还有转储文件没有把它还原成省略。
		 */
		*probin_str_p = strVal(linitial(as));
		if (list_length(as) == 1)
			*prosrc_str_p = funcname;
		else
		{
			*prosrc_str_p = strVal(lsecond(as));
			if (strcmp(*prosrc_str_p, "-") == 0)
				*prosrc_str_p = funcname;
		}
	}
	else if (sql_body_in)
	{
		SQLFunctionParseInfoPtr pinfo;

		pinfo = (SQLFunctionParseInfoPtr) palloc0(sizeof(SQLFunctionParseInfo));

		pinfo->fname = funcname;
		pinfo->nargs = list_length(parameterTypes);
		pinfo->argtypes = (Oid *) palloc(pinfo->nargs * sizeof(Oid));
		pinfo->argnames = (char **) palloc(pinfo->nargs * sizeof(char *));
		for (int i = 0; i < list_length(parameterTypes); i++)
		{
			char	   *s = strVal(list_nth(inParameterNames, i));

			pinfo->argtypes[i] = list_nth_oid(parameterTypes, i);
			if (IsPolymorphicType(pinfo->argtypes[i]))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SQL function with unquoted function body cannot have polymorphic arguments")));

			if (s[0] != '\0')
				pinfo->argnames[i] = s;
			else
				pinfo->argnames[i] = NULL;
		}

		if (IsA(sql_body_in, List))
		{
			List	   *stmts = linitial_node(List, castNode(List, sql_body_in));
			ListCell   *lc;
			List	   *transformed_stmts = NIL;

			foreach(lc, stmts)
			{
				Node	   *stmt = lfirst(lc);
				Query	   *q;
				ParseState *pstate = make_parsestate(NULL);

				pstate->p_sourcetext = queryString;
				sql_fn_parser_setup(pstate, pinfo);
				q = transformStmt(pstate, stmt);
				if (q->commandType == CMD_UTILITY)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("%s is not yet supported in unquoted SQL function body",
								   GetCommandTagName(CreateCommandTag(q->utilityStmt))));
				transformed_stmts = lappend(transformed_stmts, q);
				free_parsestate(pstate);
			}

			*sql_body_out = (Node *) list_make1(transformed_stmts);
		}
		else
		{
			Query	   *q;
			ParseState *pstate = make_parsestate(NULL);

			pstate->p_sourcetext = queryString;
			sql_fn_parser_setup(pstate, pinfo);
			q = transformStmt(pstate, sql_body_in);
			if (q->commandType == CMD_UTILITY)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("%s is not yet supported in unquoted SQL function body",
							   GetCommandTagName(CreateCommandTag(q->utilityStmt))));
			free_parsestate(pstate);

			*sql_body_out = (Node *) q;
		}

		/*
		 * We must put something in prosrc.  For the moment, just record an
		 * empty string.  It might be useful to store the original text of the
		 * CREATE FUNCTION statement --- but to make actual use of that in
		 * error reports, we'd also have to adjust readfuncs.c to not throw
		 * away node location fields when reading prosqlbody.
		 *
		 * prosrc 里必须放点东西。目前只记录一个空字符串。把 CREATE FUNCTION 语句的原文存下来也许有用，
		 * 但要在错误报告中真正用上它，还得调整 readfuncs.c，读 prosqlbody 时不要丢掉节点位置字段。
		 */
		*prosrc_str_p = pstrdup("");

		/* But we definitely don't need probin. */
		/*
		 *
		 * 但我们确实不需要 probin。
		 */
		*probin_str_p = NULL;
	}
	else
	{
		/* Everything else wants the given string in prosrc. */
		/*
		 *
		 * 其他情况都把给定字符串放进 prosrc。
		 */
		*prosrc_str_p = strVal(linitial(as));
		*probin_str_p = NULL;

		if (list_length(as) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only one AS item needed for language \"%s\"",
							languageName)));

		if (languageOid == INTERNALlanguageId)
		{
			/*
			 * In PostgreSQL versions before 6.5, the SQL name of the created
			 * function could not be different from the internal name, and
			 * "prosrc" wasn't used.  So there is code out there that does
			 * CREATE FUNCTION xyz AS '' LANGUAGE internal. To preserve some
			 * modicum of backwards compatibility, accept an empty "prosrc"
			 * value as meaning the supplied SQL function name.
			 *
			 * 在 PostgreSQL 6.5 之前，所创建函数的 SQL 名不能与内部名不同，而且不用 prosrc。
			 * 因此外面有这样的代码：CREATE FUNCTION xyz AS '' LANGUAGE internal。
			 * 为保留一点向后兼容，把空的 prosrc 接受为表示所给的 SQL 函数名。
			 */
			if (strlen(*prosrc_str_p) == 0)
				*prosrc_str_p = funcname;
		}
	}
}


/*
 * CreateFunction
 *	 Execute a CREATE FUNCTION (or CREATE PROCEDURE) utility statement.
 *
 * CreateFunction：执行 CREATE FUNCTION（或 CREATE PROCEDURE）效用语句。
 */
ObjectAddress
CreateFunction(ParseState *pstate, CreateFunctionStmt *stmt)
{
	char	   *probin_str;
	char	   *prosrc_str;
	Node	   *prosqlbody;
	Oid			prorettype;
	bool		returnsSet;
	char	   *language;
	Oid			languageOid;
	Oid			languageValidator;
	Node	   *transformDefElem = NULL;
	char	   *funcname;
	Oid			namespaceId;
	AclResult	aclresult;
	oidvector  *parameterTypes;
	List	   *parameterTypes_list = NIL;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *inParameterNames_list = NIL;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	List	   *trftypes_list = NIL;
	List	   *trfoids_list = NIL;
	ArrayType  *trftypes;
	Oid			requiredResultType;
	bool		isWindowFunc,
				isStrict,
				security,
				isLeakProof;
	char		volatility;
	ArrayType  *proconfig;
	float4		procost;
	float4		prorows;
	Oid			prosupport;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	List	   *as_clause;
	char		parallel;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表转换成一个名字和命名空间
	 */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->funcname,
													&funcname);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查我们在目标命名空间中有创建权限
	 */
	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceId));

	/* Set default attributes */
	/*
	 *
	 * 设置默认属性
	 */
	as_clause = NIL;
	language = NULL;
	isWindowFunc = false;
	isStrict = false;
	security = false;
	isLeakProof = false;
	volatility = PROVOLATILE_VOLATILE;
	proconfig = NULL;
	procost = -1;				/* indicates not set */
	/*
	 *
	 * 表示尚未设置
	 */
	prorows = -1;				/* indicates not set */
	/*
	 *
	 * 表示尚未设置
	 */
	prosupport = InvalidOid;
	parallel = PROPARALLEL_UNSAFE;

	/* Extract non-default attributes from stmt->options list */
	/*
	 *
	 * 从 stmt->options 列表中提取非默认属性
	 */
	compute_function_attributes(pstate,
								stmt->is_procedure,
								stmt->options,
								&as_clause, &language, &transformDefElem,
								&isWindowFunc, &volatility,
								&isStrict, &security, &isLeakProof,
								&proconfig, &procost, &prorows,
								&prosupport, &parallel);

	if (!language)
	{
		if (stmt->sql_body)
			language = "sql";
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("no language specified")));
	}

	/* Look up the language and validate permissions */
	/*
	 *
	 * 查找语言并校验权限
	 */
	languageTuple = SearchSysCache1(LANGNAME, PointerGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", language),
				 (extension_file_exists(language) ?
				  errhint("Use CREATE EXTENSION to load the language into the database.") : 0)));

	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
	languageOid = languageStruct->oid;

	if (languageStruct->lanpltrusted)
	{
		/* if trusted language, need USAGE privilege */
		/*
		 *
		 * 若是可信语言，需要 USAGE 权限
		 */
		aclresult = object_aclcheck(LanguageRelationId, languageOid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}
	else
	{
		/* if untrusted language, must be superuser */
		/*
		 *
		 * 若是不可信语言，必须是超级用户
		 */
		if (!superuser())
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}

	languageValidator = languageStruct->lanvalidator;

	ReleaseSysCache(languageTuple);

	/*
	 * Only superuser is allowed to create leakproof functions because
	 * leakproof functions can see tuples which have not yet been filtered out
	 * by security barrier views or row-level security policies.
	 *
	 * 只有超级用户可以创建 leakproof 函数，因为 leakproof 函数能看到尚未被安全屏障视图或行级安全策略滤掉的元组。
	 */
	if (isLeakProof && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can define a leakproof function")));

	if (transformDefElem)
	{
		ListCell   *lc;

		foreach(lc, castNode(List, transformDefElem))
		{
			Oid			typeid = typenameTypeId(NULL,
												lfirst_node(TypeName, lc));
			Oid			elt = get_base_element_type(typeid);
			Oid			transformid;

			typeid = elt ? elt : typeid;
			transformid = get_transform_oid(typeid, languageOid, false);
			trftypes_list = lappend_oid(trftypes_list, typeid);
			trfoids_list = lappend_oid(trfoids_list, transformid);
		}
	}

	/*
	 * Convert remaining parameters of CREATE to form wanted by
	 * ProcedureCreate.
	 *
	 * 把 CREATE 的其余参数转换成 ProcedureCreate 所需的形式。
	 */
	interpret_function_parameter_list(pstate,
									  stmt->parameters,
									  languageOid,
									  stmt->is_procedure ? OBJECT_PROCEDURE : OBJECT_FUNCTION,
									  &parameterTypes,
									  &parameterTypes_list,
									  &allParameterTypes,
									  &parameterModes,
									  &parameterNames,
									  &inParameterNames_list,
									  &parameterDefaults,
									  &variadicArgType,
									  &requiredResultType);

	if (stmt->is_procedure)
	{
		Assert(!stmt->returnType);
		prorettype = requiredResultType ? requiredResultType : VOIDOID;
		returnsSet = false;
	}
	else if (stmt->returnType)
	{
		/* explicit RETURNS clause */
		/*
		 *
		 * 显式的 RETURNS 子句
		 */
		compute_return_type(stmt->returnType, languageOid,
							&prorettype, &returnsSet);
		if (OidIsValid(requiredResultType) && prorettype != requiredResultType)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("function result type must be %s because of OUT parameters",
							format_type_be(requiredResultType))));
	}
	else if (OidIsValid(requiredResultType))
	{
		/* default RETURNS clause from OUT parameters */
		/*
		 *
		 * 由 OUT 参数得到的默认 RETURNS 子句
		 */
		prorettype = requiredResultType;
		returnsSet = false;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function result type must be specified")));
		/* Alternative possibility: default to RETURNS VOID */
		/*
		 *
		 * 另一种可能：默认 RETURNS VOID
		 */
		prorettype = VOIDOID;
		returnsSet = false;
	}

	if (trftypes_list != NIL)
	{
		ListCell   *lc;
		Datum	   *arr;
		int			i;

		arr = palloc(list_length(trftypes_list) * sizeof(Datum));
		i = 0;
		foreach(lc, trftypes_list)
			arr[i++] = ObjectIdGetDatum(lfirst_oid(lc));
		trftypes = construct_array_builtin(arr, list_length(trftypes_list), OIDOID);
	}
	else
	{
		/* store SQL NULL instead of empty array */
		/*
		 *
		 * 存 SQL NULL，而不是空数组
		 */
		trftypes = NULL;
	}

	interpret_AS_clause(languageOid, language, funcname, as_clause, stmt->sql_body,
						parameterTypes_list, inParameterNames_list,
						&prosrc_str, &probin_str, &prosqlbody,
						pstate->p_sourcetext);

	/*
	 * Set default values for COST and ROWS depending on other parameters;
	 * reject ROWS if it's not returnsSet.  NB: pg_dump knows these default
	 * values, keep it in sync if you change them.
	 *
	 * 根据其他参数为 COST 和 ROWS 设默认值；若不是 returnsSet 则拒绝 ROWS。
	 * 注意：pg_dump 知道这些默认值，若更改请保持同步。
	 */
	if (procost < 0)
	{
		/* SQL and PL-language functions are assumed more expensive */
		/*
		 *
		 * SQL 和 PL 语言函数假定更昂贵
		 */
		if (languageOid == INTERNALlanguageId ||
			languageOid == ClanguageId)
			procost = 1;
		else
			procost = 100;
	}
	if (prorows < 0)
	{
		if (returnsSet)
			prorows = 1000;
		else
			prorows = 0;		/* dummy value if not returnsSet */
			/*
			 *
			 * 若不是 returnsSet，则用一个占位值
			 */
	}
	else if (!returnsSet)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ROWS is not applicable when function does not return a set")));

	/*
	 * And now that we have all the parameters, and know we're permitted to do
	 * so, go ahead and create the function.
	 *
	 * 现在参数已经齐备，并且知道我们有权这样做，于是创建该函数。
	 */
	return ProcedureCreate(funcname,
						   namespaceId,
						   stmt->replace,
						   returnsSet,
						   prorettype,
						   GetUserId(),
						   languageOid,
						   languageValidator,
						   prosrc_str,	/* converted to text later */
						   /*
						    *
						    * 稍后转换成 text
						    */
						   probin_str,	/* converted to text later */
						   /*
						    *
						    * 稍后转换成 text
						    */
						   prosqlbody,
						   stmt->is_procedure ? PROKIND_PROCEDURE : (isWindowFunc ? PROKIND_WINDOW : PROKIND_FUNCTION),
						   security,
						   isLeakProof,
						   isStrict,
						   volatility,
						   parallel,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   PointerGetDatum(trftypes),
						   trfoids_list,
						   PointerGetDatum(proconfig),
						   prosupport,
						   procost,
						   prorows);
}

/*
 * Guts of function deletion.
 *
 * 函数删除的核心逻辑。
 *
 * Note: this is also used for aggregate deletion, since the OIDs of
 * both functions and aggregates point to pg_proc.
 *
 * 注意：聚集删除也用这段，因为函数和聚集的 OID 都指向 pg_proc。
 */
void
RemoveFunctionById(Oid funcOid)
{
	Relation	relation;
	HeapTuple	tup;
	char		prokind;

	/*
	 * Delete the pg_proc tuple.
	 *
	 * 删除 pg_proc 元组。
	 */
	relation = table_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
	/*
	 *
	 * 不应该发生
	 */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	prokind = ((Form_pg_proc) GETSTRUCT(tup))->prokind;

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);

	pgstat_drop_function(funcOid);

	/*
	 * If there's a pg_aggregate tuple, delete that too.
	 *
	 * 若有 pg_aggregate 元组，也把它删掉。
	 */
	if (prokind == PROKIND_AGGREGATE)
	{
		relation = table_open(AggregateRelationId, RowExclusiveLock);

		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(funcOid));
		if (!HeapTupleIsValid(tup)) /* should not happen */
		/*
		 *
		 * 不应该发生
		 */
			elog(ERROR, "cache lookup failed for pg_aggregate tuple for function %u", funcOid);

		CatalogTupleDelete(relation, &tup->t_self);

		ReleaseSysCache(tup);

		table_close(relation, RowExclusiveLock);
	}
}

/*
 * Implements the ALTER FUNCTION utility command (except for the
 * RENAME and OWNER clauses, which are handled as part of the generic
 * ALTER framework).
 *
 * 实现 ALTER FUNCTION 效用命令（RENAME 和 OWNER 子句除外，它们由通用 ALTER 框架处理）。
 */
ObjectAddress
AlterFunction(ParseState *pstate, AlterFunctionStmt *stmt)
{
	HeapTuple	tup;
	Oid			funcOid;
	Form_pg_proc procForm;
	bool		is_procedure;
	Relation	rel;
	ListCell   *l;
	DefElem    *volatility_item = NULL;
	DefElem    *strict_item = NULL;
	DefElem    *security_def_item = NULL;
	DefElem    *leakproof_item = NULL;
	List	   *set_items = NIL;
	DefElem    *cost_item = NULL;
	DefElem    *rows_item = NULL;
	DefElem    *support_item = NULL;
	DefElem    *parallel_item = NULL;
	ObjectAddress address;

	rel = table_open(ProcedureRelationId, RowExclusiveLock);

	funcOid = LookupFuncWithArgs(stmt->objtype, stmt->func, false);

	ObjectAddressSet(address, ProcedureRelationId, funcOid);

	tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(funcOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
	/*
	 *
	 * 不应该发生
	 */
		elog(ERROR, "cache lookup failed for function %u", funcOid);

	procForm = (Form_pg_proc) GETSTRUCT(tup);

	/* Permission check: must own function */
	/*
	 *
	 * 权限检查：必须拥有该函数
	 */
	if (!object_ownercheck(ProcedureRelationId, funcOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, stmt->objtype,
					   NameListToString(stmt->func->objname));

	if (procForm->prokind == PROKIND_AGGREGATE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function",
						NameListToString(stmt->func->objname))));

	is_procedure = (procForm->prokind == PROKIND_PROCEDURE);

	/* Examine requested actions. */
	/*
	 *
	 * 检查请求的动作。
	 */
	foreach(l, stmt->actions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (compute_common_attribute(pstate,
									 is_procedure,
									 defel,
									 &volatility_item,
									 &strict_item,
									 &security_def_item,
									 &leakproof_item,
									 &set_items,
									 &cost_item,
									 &rows_item,
									 &support_item,
									 &parallel_item) == false)
			elog(ERROR, "option \"%s\" not recognized", defel->defname);
	}

	if (volatility_item)
		procForm->provolatile = interpret_func_volatility(volatility_item);
	if (strict_item)
		procForm->proisstrict = boolVal(strict_item->arg);
	if (security_def_item)
		procForm->prosecdef = boolVal(security_def_item->arg);
	if (leakproof_item)
	{
		procForm->proleakproof = boolVal(leakproof_item->arg);
		if (procForm->proleakproof && !superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("only superuser can define a leakproof function")));
	}
	if (cost_item)
	{
		procForm->procost = defGetNumeric(cost_item);
		if (procForm->procost <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COST must be positive")));
	}
	if (rows_item)
	{
		procForm->prorows = defGetNumeric(rows_item);
		if (procForm->prorows <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS must be positive")));
		if (!procForm->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ROWS is not applicable when function does not return a set")));
	}
	if (support_item)
	{
		/* interpret_func_support handles the privilege check */
		/*
		 *
		 * interpret_func_support 负责权限检查
		 */
		Oid			newsupport = interpret_func_support(support_item);

		/* Add or replace dependency on support function */
		/*
		 *
		 * 添加或替换对支持函数的依赖
		 */
		if (OidIsValid(procForm->prosupport))
		{
			if (changeDependencyFor(ProcedureRelationId, funcOid,
									ProcedureRelationId, procForm->prosupport,
									newsupport) != 1)
				elog(ERROR, "could not change support dependency for function %s",
					 get_func_name(funcOid));
		}
		else
		{
			ObjectAddress referenced;

			referenced.classId = ProcedureRelationId;
			referenced.objectId = newsupport;
			referenced.objectSubId = 0;
			recordDependencyOn(&address, &referenced, DEPENDENCY_NORMAL);
		}

		procForm->prosupport = newsupport;
	}
	if (parallel_item)
		procForm->proparallel = interpret_func_parallel(parallel_item);
	if (set_items)
	{
		Datum		datum;
		bool		isnull;
		ArrayType  *a;
		Datum		repl_val[Natts_pg_proc];
		bool		repl_null[Natts_pg_proc];
		bool		repl_repl[Natts_pg_proc];

		/* extract existing proconfig setting */
		/*
		 *
		 * 取出已有的 proconfig 设置
		 */
		datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proconfig, &isnull);
		a = isnull ? NULL : DatumGetArrayTypeP(datum);

		/* update according to each SET or RESET item, left to right */
		/*
		 *
		 * 按从左到右的每个 SET 或 RESET 项更新
		 */
		a = update_proconfig_value(a, set_items);

		/* update the tuple */
		/*
		 *
		 * 更新元组
		 */
		memset(repl_repl, false, sizeof(repl_repl));
		repl_repl[Anum_pg_proc_proconfig - 1] = true;

		if (a == NULL)
		{
			repl_val[Anum_pg_proc_proconfig - 1] = (Datum) 0;
			repl_null[Anum_pg_proc_proconfig - 1] = true;
		}
		else
		{
			repl_val[Anum_pg_proc_proconfig - 1] = PointerGetDatum(a);
			repl_null[Anum_pg_proc_proconfig - 1] = false;
		}

		tup = heap_modify_tuple(tup, RelationGetDescr(rel),
								repl_val, repl_null, repl_repl);
	}
	/* DO NOT put more touches of procForm below here; it's now dangling. */
	/*
	 *
	 * 不要在此之后再碰 procForm；它现在已经悬空。
	 */

	/* Do the update */
	/*
	 *
	 * 执行更新
	 */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(ProcedureRelationId, funcOid, 0);

	table_close(rel, NoLock);
	heap_freetuple(tup);

	return address;
}


/*
 * CREATE CAST
 *
 * CREATE CAST 命令。
 */
ObjectAddress
CreateCast(CreateCastStmt *stmt)
{
	Oid			sourcetypeid;
	Oid			targettypeid;
	char		sourcetyptype;
	char		targettyptype;
	Oid			funcid;
	Oid			incastid = InvalidOid;
	Oid			outcastid = InvalidOid;
	int			nargs;
	char		castcontext;
	char		castmethod;
	HeapTuple	tuple;
	AclResult	aclresult;
	ObjectAddress myself;

	sourcetypeid = typenameTypeId(NULL, stmt->sourcetype);
	targettypeid = typenameTypeId(NULL, stmt->targettype);
	sourcetyptype = get_typtype(sourcetypeid);
	targettyptype = get_typtype(targettypeid);

	/* No pseudo-types allowed */
	/*
	 *
	 * 不允许伪类型
	 */
	if (sourcetyptype == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("source data type %s is a pseudo-type",
						TypeNameToString(stmt->sourcetype))));

	if (targettyptype == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("target data type %s is a pseudo-type",
						TypeNameToString(stmt->targettype))));

	/* Permission check */
	/*
	 *
	 * 权限检查
	 */
	if (!object_ownercheck(TypeRelationId, sourcetypeid, GetUserId())
		&& !object_ownercheck(TypeRelationId, targettypeid, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of type %s or type %s",
						format_type_be(sourcetypeid),
						format_type_be(targettypeid))));

	aclresult = object_aclcheck(TypeRelationId, sourcetypeid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, sourcetypeid);

	aclresult = object_aclcheck(TypeRelationId, targettypeid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettypeid);

	/* Domains are allowed for historical reasons, but we warn */
	/*
	 *
	 * 出于历史原因允许域，但我们会警告
	 */
	if (sourcetyptype == TYPTYPE_DOMAIN)
		ereport(WARNING,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cast will be ignored because the source data type is a domain")));

	else if (targettyptype == TYPTYPE_DOMAIN)
		ereport(WARNING,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cast will be ignored because the target data type is a domain")));

	/* Determine the cast method */
	/*
	 *
	 * 确定强制转换方法
	 */
	if (stmt->func != NULL)
		castmethod = COERCION_METHOD_FUNCTION;
	else if (stmt->inout)
		castmethod = COERCION_METHOD_INOUT;
	else
		castmethod = COERCION_METHOD_BINARY;

	if (castmethod == COERCION_METHOD_FUNCTION)
	{
		Form_pg_proc procstruct;

		funcid = LookupFuncWithArgs(OBJECT_FUNCTION, stmt->func, false);

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", funcid);

		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		nargs = procstruct->pronargs;
		if (nargs < 1 || nargs > 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must take one to three arguments")));
		if (!IsBinaryCoercibleWithCast(sourcetypeid,
									   procstruct->proargtypes.values[0],
									   &incastid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("argument of cast function must match or be binary-coercible from source data type")));
		if (nargs > 1 && procstruct->proargtypes.values[1] != INT4OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("second argument of cast function must be type %s",
							"integer")));
		if (nargs > 2 && procstruct->proargtypes.values[2] != BOOLOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("third argument of cast function must be type %s",
							"boolean")));
		if (!IsBinaryCoercibleWithCast(procstruct->prorettype,
									   targettypeid,
									   &outcastid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of cast function must match or be binary-coercible to target data type")));

		/*
		 * Restricting the volatility of a cast function may or may not be a
		 * good idea in the abstract, but it definitely breaks many old
		 * user-defined types.  Disable this check --- tgl 2/1/03
		 *
		 * 抽象地说，限制强制转换函数的易变性未必是好主意，但它肯定会弄坏许多旧的用户定义类型。
		 * 禁用这项检查。tgl，2003-02-01
		 */
#ifdef NOT_USED
		if (procstruct->provolatile == PROVOLATILE_VOLATILE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must not be volatile")));
#endif
		if (procstruct->prokind != PROKIND_FUNCTION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must be a normal function")));
		if (procstruct->proretset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cast function must not return a set")));

		ReleaseSysCache(tuple);
	}
	else
	{
		funcid = InvalidOid;
		nargs = 0;
	}

	if (castmethod == COERCION_METHOD_BINARY)
	{
		int16		typ1len;
		int16		typ2len;
		bool		typ1byval;
		bool		typ2byval;
		char		typ1align;
		char		typ2align;

		/*
		 * Must be superuser to create binary-compatible casts, since
		 * erroneous casts can easily crash the backend.
		 *
		 * 创建二进制兼容的强制转换必须是超级用户，因为错误的转换很容易让后端崩溃。
		 */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to create a cast WITHOUT FUNCTION")));

		/*
		 * Also, insist that the types match as to size, alignment, and
		 * pass-by-value attributes; this provides at least a crude check that
		 * they have similar representations.  A pair of types that fail this
		 * test should certainly not be equated.
		 *
		 * 另外，坚持要求类型在大小、对齐和传值属性上匹配；这至少粗略检查它们有相似的表示。
		 * 通不过这项检查的一对类型肯定不应被视为等同。
		 */
		get_typlenbyvalalign(sourcetypeid, &typ1len, &typ1byval, &typ1align);
		get_typlenbyvalalign(targettypeid, &typ2len, &typ2byval, &typ2align);
		if (typ1len != typ2len ||
			typ1byval != typ2byval ||
			typ1align != typ2align)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("source and target data types are not physically compatible")));

		/*
		 * We know that composite, array, range and enum types are never
		 * binary-compatible with each other.  They all have OIDs embedded in
		 * them.
		 *
		 * 我们知道复合、数组、范围和枚举类型彼此从不是二进制兼容的。它们内部都嵌有 OID。
		 *
		 * Theoretically you could build a user-defined base type that is
		 * binary-compatible with such a type.  But we disallow it anyway, as
		 * in practice such a cast is surely a mistake.  You can always work
		 * around that by writing a cast function.
		 *
		 * 理论上可以做一个与这类类型二进制兼容的用户定义基类型。但我们仍然禁止，因为实践中这种转换肯定是错误。
		 * 若确实需要，总可以写一个强制转换函数来绕过。
		 *
		 * NOTE: if we ever have a kind of container type that doesn't need to
		 * be rejected for this reason, we'd likely need to recursively apply
		 * all of these same checks to the contained type(s).
		 *
		 * 注意：若将来有一种不必因此被拒绝的容器类型，很可能需要对所含类型递归应用同样的全部检查。
		 */
		if (sourcetyptype == TYPTYPE_COMPOSITE ||
			targettyptype == TYPTYPE_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("composite data types are not binary-compatible")));

		if (OidIsValid(get_element_type(sourcetypeid)) ||
			OidIsValid(get_element_type(targettypeid)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("array data types are not binary-compatible")));

		if (sourcetyptype == TYPTYPE_RANGE ||
			targettyptype == TYPTYPE_RANGE ||
			sourcetyptype == TYPTYPE_MULTIRANGE ||
			targettyptype == TYPTYPE_MULTIRANGE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("range data types are not binary-compatible")));

		if (sourcetyptype == TYPTYPE_ENUM ||
			targettyptype == TYPTYPE_ENUM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("enum data types are not binary-compatible")));

		/*
		 * We also disallow creating binary-compatibility casts involving
		 * domains.  Casting from a domain to its base type is already
		 * allowed, and casting the other way ought to go through domain
		 * coercion to permit constraint checking.  Again, if you're intent on
		 * having your own semantics for that, create a no-op cast function.
		 *
		 * 也不允许创建涉及域的二进制兼容强制转换。从域到其基类型的转换已经允许，
		 * 另一个方向应走域强制，以便检查约束。若你坚持要自己的语义，请创建一个空操作的强制转换函数。
		 *
		 * NOTE: if we were to relax this, the above checks for composites
		 * etc. would have to be modified to look through domains to their
		 * base types.
		 *
		 * 注意：若放宽这一点，上面对复合类型等的检查必须改成透过域看到它们的基类型。
		 */
		if (sourcetyptype == TYPTYPE_DOMAIN ||
			targettyptype == TYPTYPE_DOMAIN)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("domain data types must not be marked binary-compatible")));
	}

	/*
	 * Allow source and target types to be same only for length coercion
	 * functions.  We assume a multi-arg function does length coercion.
	 *
	 * 仅长度强制函数允许源类型与目标类型相同。我们假定多参数函数做的是长度强制。
	 */
	if (sourcetypeid == targettypeid && nargs < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("source data type and target data type are the same")));

	/* convert CoercionContext enum to char value for castcontext */
	/*
	 *
	 * 把 CoercionContext 枚举转换成 castcontext 用的字符值
	 */
	switch (stmt->context)
	{
		case COERCION_IMPLICIT:
			castcontext = COERCION_CODE_IMPLICIT;
			break;
		case COERCION_ASSIGNMENT:
			castcontext = COERCION_CODE_ASSIGNMENT;
			break;
			/* COERCION_PLPGSQL is intentionally not covered here */
			/*
			 *
			 * 这里有意不覆盖 COERCION_PLPGSQL
			 */
		case COERCION_EXPLICIT:
			castcontext = COERCION_CODE_EXPLICIT;
			break;
		default:
			elog(ERROR, "unrecognized CoercionContext: %d", stmt->context);
			castcontext = 0;	/* keep compiler quiet */
			/*
			 *
			 * 让编译器保持安静
			 */
			break;
	}

	myself = CastCreate(sourcetypeid, targettypeid, funcid, incastid, outcastid,
						castcontext, castmethod, DEPENDENCY_NORMAL);
	return myself;
}


/*
 * 校验 transform 函数不是 volatile、是普通函数、不返回集合，且只接受一个 internal 参数。
 */
static void
check_transform_function(Form_pg_proc procstruct)
{
	if (procstruct->provolatile == PROVOLATILE_VOLATILE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must not be volatile")));
	if (procstruct->prokind != PROKIND_FUNCTION)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must be a normal function")));
	if (procstruct->proretset)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must not return a set")));
	if (procstruct->pronargs != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("transform function must take one argument")));
	if (procstruct->proargtypes.values[0] != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("first argument of transform function must be type %s",
						"internal")));
}


/*
 * CREATE TRANSFORM
 *
 * CREATE TRANSFORM 命令。
 */
ObjectAddress
CreateTransform(CreateTransformStmt *stmt)
{
	Oid			typeid;
	char		typtype;
	Oid			langid;
	Oid			fromsqlfuncid;
	Oid			tosqlfuncid;
	AclResult	aclresult;
	Form_pg_proc procstruct;
	Datum		values[Natts_pg_transform];
	bool		nulls[Natts_pg_transform] = {0};
	bool		replaces[Natts_pg_transform] = {0};
	Oid			transformid;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Relation	relation;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;
	bool		is_replace;

	/*
	 * Get the type
	 *
	 * 取得类型
	 */
	typeid = typenameTypeId(NULL, stmt->type_name);
	typtype = get_typtype(typeid);

	if (typtype == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("data type %s is a pseudo-type",
						TypeNameToString(stmt->type_name))));

	if (typtype == TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("data type %s is a domain",
						TypeNameToString(stmt->type_name))));

	if (!object_ownercheck(TypeRelationId, typeid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeid);

	aclresult = object_aclcheck(TypeRelationId, typeid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, typeid);

	/*
	 * Get the language
	 *
	 * 取得语言
	 */
	langid = get_language_oid(stmt->lang, false);

	aclresult = object_aclcheck(LanguageRelationId, langid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_LANGUAGE, stmt->lang);

	/*
	 * Get the functions
	 *
	 * 取得函数
	 */
	if (stmt->fromsql)
	{
		fromsqlfuncid = LookupFuncWithArgs(OBJECT_FUNCTION, stmt->fromsql, false);

		if (!object_ownercheck(ProcedureRelationId, fromsqlfuncid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, NameListToString(stmt->fromsql->objname));

		aclresult = object_aclcheck(ProcedureRelationId, fromsqlfuncid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, NameListToString(stmt->fromsql->objname));

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(fromsqlfuncid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", fromsqlfuncid);
		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		if (procstruct->prorettype != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of FROM SQL function must be %s",
							"internal")));
		check_transform_function(procstruct);
		ReleaseSysCache(tuple);
	}
	else
		fromsqlfuncid = InvalidOid;

	if (stmt->tosql)
	{
		tosqlfuncid = LookupFuncWithArgs(OBJECT_FUNCTION, stmt->tosql, false);

		if (!object_ownercheck(ProcedureRelationId, tosqlfuncid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION, NameListToString(stmt->tosql->objname));

		aclresult = object_aclcheck(ProcedureRelationId, tosqlfuncid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, NameListToString(stmt->tosql->objname));

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(tosqlfuncid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", tosqlfuncid);
		procstruct = (Form_pg_proc) GETSTRUCT(tuple);
		if (procstruct->prorettype != typeid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("return data type of TO SQL function must be the transform data type")));
		check_transform_function(procstruct);
		ReleaseSysCache(tuple);
	}
	else
		tosqlfuncid = InvalidOid;

	/*
	 * Ready to go
	 *
	 * 可以开始了
	 */
	values[Anum_pg_transform_trftype - 1] = ObjectIdGetDatum(typeid);
	values[Anum_pg_transform_trflang - 1] = ObjectIdGetDatum(langid);
	values[Anum_pg_transform_trffromsql - 1] = ObjectIdGetDatum(fromsqlfuncid);
	values[Anum_pg_transform_trftosql - 1] = ObjectIdGetDatum(tosqlfuncid);

	relation = table_open(TransformRelationId, RowExclusiveLock);

	tuple = SearchSysCache2(TRFTYPELANG,
							ObjectIdGetDatum(typeid),
							ObjectIdGetDatum(langid));
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_transform form = (Form_pg_transform) GETSTRUCT(tuple);

		if (!stmt->replace)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("transform for type %s language \"%s\" already exists",
							format_type_be(typeid),
							stmt->lang)));

		replaces[Anum_pg_transform_trffromsql - 1] = true;
		replaces[Anum_pg_transform_trftosql - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(relation), values, nulls, replaces);
		CatalogTupleUpdate(relation, &newtuple->t_self, newtuple);

		transformid = form->oid;
		ReleaseSysCache(tuple);
		is_replace = true;
	}
	else
	{
		transformid = GetNewOidWithIndex(relation, TransformOidIndexId,
										 Anum_pg_transform_oid);
		values[Anum_pg_transform_oid - 1] = ObjectIdGetDatum(transformid);
		newtuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);
		CatalogTupleInsert(relation, newtuple);
		is_replace = false;
	}

	if (is_replace)
		deleteDependencyRecordsFor(TransformRelationId, transformid, true);

	addrs = new_object_addresses();

	/* make dependency entries */
	/*
	 *
	 * 建立依赖项
	 */
	ObjectAddressSet(myself, TransformRelationId, transformid);

	/* dependency on language */
	/*
	 *
	 * 对语言的依赖
	 */
	ObjectAddressSet(referenced, LanguageRelationId, langid);
	add_exact_object_address(&referenced, addrs);

	/* dependency on type */
	/*
	 *
	 * 对类型的依赖
	 */
	ObjectAddressSet(referenced, TypeRelationId, typeid);
	add_exact_object_address(&referenced, addrs);

	/* dependencies on functions */
	/*
	 *
	 * 对函数的依赖
	 */
	if (OidIsValid(fromsqlfuncid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, fromsqlfuncid);
		add_exact_object_address(&referenced, addrs);
	}
	if (OidIsValid(tosqlfuncid))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, tosqlfuncid);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* dependency on extension */
	/*
	 *
	 * 对扩展的依赖
	 */
	recordDependencyOnCurrentExtension(&myself, is_replace);

	/* Post creation hook for new transform */
	/*
	 *
	 * 新 transform 的创建后钩子
	 */
	InvokeObjectPostCreateHook(TransformRelationId, transformid, 0);

	heap_freetuple(newtuple);

	table_close(relation, RowExclusiveLock);

	return myself;
}


/*
 * get_transform_oid - given type OID and language OID, look up a transform OID
 *
 * get_transform_oid：给定类型 OID 和语言 OID，查找 transform 的 OID
 *
 * If missing_ok is false, throw an error if the transform is not found.  If
 * true, just return InvalidOid.
 *
 * 若 missing_ok 为 false，找不到 transform 时抛错。若为 true，则只返回 InvalidOid。
 */
Oid
get_transform_oid(Oid type_id, Oid lang_id, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid2(TRFTYPELANG, Anum_pg_transform_oid,
						  ObjectIdGetDatum(type_id),
						  ObjectIdGetDatum(lang_id));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("transform for type %s language \"%s\" does not exist",
						format_type_be(type_id),
						get_language_name(lang_id, false))));
	return oid;
}


/*
 * Subroutine for ALTER FUNCTION/AGGREGATE SET SCHEMA/RENAME
 *
 * ALTER FUNCTION/AGGREGATE SET SCHEMA/RENAME 的子程序
 *
 * Is there a function with the given name and signature already in the given
 * namespace?  If so, raise an appropriate error message.
 *
 * 给定命名空间中是否已有同名同签名的函数？若有，则发出合适的错误信息。
 */
void
IsThereFunctionInNamespace(const char *proname, int pronargs,
						   oidvector *proargtypes, Oid nspOid)
{
	/* check for duplicate name (more friendly than unique-index failure) */
	/*
	 *
	 * 检查重名（比唯一索引失败更友好）
	 */
	if (SearchSysCacheExists3(PROCNAMEARGSNSP,
							  CStringGetDatum(proname),
							  PointerGetDatum(proargtypes),
							  ObjectIdGetDatum(nspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s already exists in schema \"%s\"",
						funcname_signature_string(proname, pronargs,
												  NIL, proargtypes->values),
						get_namespace_name(nspOid))));
}

/*
 * ExecuteDoStmt
 *		Execute inline procedural-language code
 *
 * ExecuteDoStmt：执行内联的过程语言代码
 *
 * See at ExecuteCallStmt() about the atomic argument.
 *
 * 关于 atomic 参数，见 ExecuteCallStmt()。
 */
void
ExecuteDoStmt(ParseState *pstate, DoStmt *stmt, bool atomic)
{
	InlineCodeBlock *codeblock = makeNode(InlineCodeBlock);
	ListCell   *arg;
	DefElem    *as_item = NULL;
	DefElem    *language_item = NULL;
	char	   *language;
	Oid			laninline;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;

	/* Process options we got from gram.y */
	/*
	 *
	 * 处理从 gram.y 得到的选项
	 */
	foreach(arg, stmt->args)
	{
		DefElem    *defel = (DefElem *) lfirst(arg);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_item)
				errorConflictingDefElem(defel, pstate);
			as_item = defel;
		}
		else if (strcmp(defel->defname, "language") == 0)
		{
			if (language_item)
				errorConflictingDefElem(defel, pstate);
			language_item = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (as_item)
		codeblock->source_text = strVal(as_item->arg);
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("no inline code specified")));

	/* if LANGUAGE option wasn't specified, use the default */
	/*
	 *
	 * 若未指定 LANGUAGE 选项，则使用默认值
	 */
	if (language_item)
		language = strVal(language_item->arg);
	else
		language = "plpgsql";

	/* Look up the language and validate permissions */
	/*
	 *
	 * 查找语言并校验权限
	 */
	languageTuple = SearchSysCache1(LANGNAME, PointerGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("language \"%s\" does not exist", language),
				 (extension_file_exists(language) ?
				  errhint("Use CREATE EXTENSION to load the language into the database.") : 0)));

	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);
	codeblock->langOid = languageStruct->oid;
	codeblock->langIsTrusted = languageStruct->lanpltrusted;
	codeblock->atomic = atomic;

	if (languageStruct->lanpltrusted)
	{
		/* if trusted language, need USAGE privilege */
		/*
		 *
		 * 若是可信语言，需要 USAGE 权限
		 */
		AclResult	aclresult;

		aclresult = object_aclcheck(LanguageRelationId, codeblock->langOid, GetUserId(),
									ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}
	else
	{
		/* if untrusted language, must be superuser */
		/*
		 *
		 * 若是不可信语言，必须是超级用户
		 */
		if (!superuser())
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_LANGUAGE,
						   NameStr(languageStruct->lanname));
	}

	/* get the handler function's OID */
	/*
	 *
	 * 取得处理函数的 OID
	 */
	laninline = languageStruct->laninline;
	if (!OidIsValid(laninline))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("language \"%s\" does not support inline code execution",
						NameStr(languageStruct->lanname))));

	ReleaseSysCache(languageTuple);

	/* execute the inline handler */
	/*
	 *
	 * 执行内联处理函数
	 */
	OidFunctionCall1(laninline, PointerGetDatum(codeblock));
}

/*
 * Execute CALL statement
 *
 * 执行 CALL 语句
 *
 * Inside a top-level CALL statement, transaction-terminating commands such as
 * COMMIT or a PL-specific equivalent are allowed.  The terminology in the SQL
 * standard is that CALL establishes a non-atomic execution context.  Most
 * other commands establish an atomic execution context, in which transaction
 * control actions are not allowed.  If there are nested executions of CALL,
 * we want to track the execution context recursively, so that the nested
 * CALLs can also do transaction control.  Note, however, that for example in
 * CALL -> SELECT -> CALL, the second call cannot do transaction control,
 * because the SELECT in between establishes an atomic execution context.
 *
 * 在顶层 CALL 语句内部，允许 COMMIT 这类结束事务的命令，或过程语言中的等价物。
 * SQL 标准的术语是：CALL 建立一个非原子执行上下文。其他大多数命令建立原子执行上下文，其中不允许事务控制动作。
 * 若 CALL 嵌套执行，我们要递归跟踪执行上下文，使嵌套的 CALL 也能做事务控制。
 * 但要注意，例如在 CALL 再到 SELECT 再到 CALL 时，第二个 CALL 不能做事务控制，因为中间的 SELECT 建立了原子执行上下文。
 *
 * So when ExecuteCallStmt() is called from the top level, we pass in atomic =
 * false (recall that that means transactions = yes).  We then create a
 * CallContext node with content atomic = false, which is passed in the
 * fcinfo->context field to the procedure invocation.  The language
 * implementation should then take appropriate measures to allow or prevent
 * transaction commands based on that information, e.g., call
 * SPI_connect_ext(SPI_OPT_NONATOMIC).  The language should also pass on the
 * atomic flag to any nested invocations to CALL.
 *
 * 因此当从顶层调用 ExecuteCallStmt() 时，传入 atomic = false（记住这意味着允许事务）。
 * 然后创建一个内容为 atomic = false 的 CallContext 节点，通过 fcinfo->context 传给过程调用。
 * 语言实现应据此允许或禁止事务命令，例如调用 SPI_connect_ext(SPI_OPT_NONATOMIC)。
 * 语言还应把 atomic 标志传给任何嵌套的 CALL。
 *
 * The expression data structures and execution context that we create
 * within this function are children of the portalContext of the Portal
 * that the CALL utility statement runs in.  Therefore, any pass-by-ref
 * values that we're passing to the procedure will survive transaction
 * commits that might occur inside the procedure.
 *
 * 本函数内创建的表达式数据结构和执行上下文，是 CALL 效用语句所在 Portal 的 portalContext 的子上下文。
 * 因此，我们传给过程的按引用传递的值，能在过程内部可能发生的事务提交之后仍然存活。
 */
void
ExecuteCallStmt(CallStmt *stmt, ParamListInfo params, bool atomic, DestReceiver *dest)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	ListCell   *lc;
	FuncExpr   *fexpr;
	int			nargs;
	int			i;
	AclResult	aclresult;
	FmgrInfo	flinfo;
	CallContext *callcontext;
	EState	   *estate;
	ExprContext *econtext;
	HeapTuple	tp;
	PgStat_FunctionCallUsage fcusage;
	Datum		retval;

	fexpr = stmt->funcexpr;
	Assert(fexpr);
	Assert(IsA(fexpr, FuncExpr));

	aclresult = object_aclcheck(ProcedureRelationId, fexpr->funcid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_PROCEDURE, get_func_name(fexpr->funcid));

	/* Prep the context object we'll pass to the procedure */
	/*
	 *
	 * 准备将传给过程的上下文对象
	 */
	callcontext = makeNode(CallContext);
	callcontext->atomic = atomic;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", fexpr->funcid);

	/*
	 * If proconfig is set we can't allow transaction commands because of the
	 * way the GUC stacking works: The transaction boundary would have to pop
	 * the proconfig setting off the stack.  That restriction could be lifted
	 * by redesigning the GUC nesting mechanism a bit.
	 *
	 * 若设置了 proconfig，则不能允许事务命令，因为 GUC 栈是这样工作的：事务边界必须把 proconfig 设置从栈上弹出。
	 * 稍微重新设计 GUC 嵌套机制后，这个限制可以取消。
	 */
	if (!heap_attisnull(tp, Anum_pg_proc_proconfig, NULL))
		callcontext->atomic = true;

	/*
	 * In security definer procedures, we can't allow transaction commands.
	 * StartTransaction() insists that the security context stack is empty,
	 * and AbortTransaction() resets the security context.  This could be
	 * reorganized, but right now it doesn't work.
	 *
	 * 在 security definer 过程中不能允许事务命令。StartTransaction() 坚持安全上下文栈为空，
	 * AbortTransaction() 会重置安全上下文。这可以重组，但目前行不通。
	 */
	if (((Form_pg_proc) GETSTRUCT(tp))->prosecdef)
		callcontext->atomic = true;

	ReleaseSysCache(tp);

	/* safety check; see ExecInitFunc() */
	/*
	 *
	 * 安全检查；见 ExecInitFunc()
	 */
	nargs = list_length(fexpr->args);
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a procedure",
							   "cannot pass more than %d arguments to a procedure",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* Initialize function call structure */
	/*
	 *
	 * 初始化函数调用结构
	 */
	InvokeFunctionExecuteHook(fexpr->funcid);
	fmgr_info(fexpr->funcid, &flinfo);
	fmgr_info_set_expr((Node *) fexpr, &flinfo);
	InitFunctionCallInfoData(*fcinfo, &flinfo, nargs, fexpr->inputcollid,
							 (Node *) callcontext, NULL);

	/*
	 * Evaluate procedure arguments inside a suitable execution context.  Note
	 * we can't free this context till the procedure returns.
	 *
	 * 在合适的执行上下文中计算过程参数。注意在过程返回之前不能释放这个上下文。
	 */
	estate = CreateExecutorState();
	estate->es_param_list_info = params;
	econtext = CreateExprContext(estate);

	/*
	 * If we're called in non-atomic context, we also have to ensure that the
	 * argument expressions run with an up-to-date snapshot.  Our caller will
	 * have provided a current snapshot in atomic contexts, but not in
	 * non-atomic contexts, because the possibility of a COMMIT/ROLLBACK
	 * destroying the snapshot makes higher-level management too complicated.
	 *
	 * 若在非原子上下文中被调用，还必须保证参数表达式使用最新的快照运行。
	 * 在原子上下文中，调用方会提供当前快照；在非原子上下文中则不会，
	 * 因为 COMMIT/ROLLBACK 可能毁掉快照，使更高层的管理过于复杂。
	 */
	if (!atomic)
		PushActiveSnapshot(GetTransactionSnapshot());

	i = 0;
	foreach(lc, fexpr->args)
	{
		ExprState  *exprstate;
		Datum		val;
		bool		isnull;

		exprstate = ExecPrepareExpr(lfirst(lc), estate);

		val = ExecEvalExprSwitchContext(exprstate, econtext, &isnull);

		fcinfo->args[i].value = val;
		fcinfo->args[i].isnull = isnull;

		i++;
	}

	/* Get rid of temporary snapshot for arguments, if we made one */
	/*
	 *
	 * 若为参数创建过临时快照，则去掉它
	 */
	if (!atomic)
		PopActiveSnapshot();

	/* Here we actually call the procedure */
	/*
	 *
	 * 这里真正调用该过程
	 */
	pgstat_init_function_usage(fcinfo, &fcusage);
	retval = FunctionCallInvoke(fcinfo);
	pgstat_end_function_usage(&fcusage, true);

	/* Handle the procedure's outputs */
	/*
	 *
	 * 处理过程的输出
	 */
	if (fexpr->funcresulttype == VOIDOID)
	{
		/* do nothing */
		/*
		 *
		 * 什么都不做
		 */
	}
	else if (fexpr->funcresulttype == RECORDOID)
	{
		/* send tuple to client */
		/*
		 *
		 * 把元组发给客户端
		 */
		HeapTupleHeader td;
		Oid			tupType;
		int32		tupTypmod;
		TupleDesc	retdesc;
		HeapTupleData rettupdata;
		TupOutputState *tstate;
		TupleTableSlot *slot;

		if (fcinfo->isnull)
			elog(ERROR, "procedure returned null record");

		/*
		 * Ensure there's an active snapshot whilst we execute whatever's
		 * involved here.  Note that this is *not* sufficient to make the
		 * world safe for TOAST pointers to be included in the returned data:
		 * the referenced data could have gone away while we didn't hold a
		 * snapshot.  Hence, it's incumbent on PLs that can do COMMIT/ROLLBACK
		 * to not return TOAST pointers, unless those pointers were fetched
		 * after the last COMMIT/ROLLBACK in the procedure.
		 *
		 * 在执行这里涉及的任何事情时，确保有一个活动快照。注意这并不足以让返回数据中包含 TOAST 指针变得安全：
		 * 在我们不持有快照期间，被引用的数据可能已经消失。
		 * 因此，能做 COMMIT/ROLLBACK 的过程语言有责任不返回 TOAST 指针，除非这些指针是在过程中最后一次 COMMIT/ROLLBACK 之后取得的。
		 *
		 * XXX that is a really nasty, hard-to-test requirement.  Is there a
		 * way to remove it?
		 *
		 * XXX：这是一个非常讨厌、难以测试的要求。有办法去掉它吗？
		 */
		EnsurePortalSnapshotExists();

		td = DatumGetHeapTupleHeader(retval);
		tupType = HeapTupleHeaderGetTypeId(td);
		tupTypmod = HeapTupleHeaderGetTypMod(td);
		retdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

		tstate = begin_tup_output_tupdesc(dest, retdesc,
										  &TTSOpsHeapTuple);

		rettupdata.t_len = HeapTupleHeaderGetDatumLength(td);
		ItemPointerSetInvalid(&(rettupdata.t_self));
		rettupdata.t_tableOid = InvalidOid;
		rettupdata.t_data = td;

		slot = ExecStoreHeapTuple(&rettupdata, tstate->slot, false);
		tstate->dest->receiveSlot(slot, tstate->dest);

		end_tup_output(tstate);

		ReleaseTupleDesc(retdesc);
	}
	else
		elog(ERROR, "unexpected result type for procedure: %u",
			 fexpr->funcresulttype);

	FreeExecutorState(estate);
}

/*
 * Construct the tuple descriptor for a CALL statement return
 *
 * 为 CALL 语句的返回值构造元组描述符
 */
TupleDesc
CallStmtResultDesc(CallStmt *stmt)
{
	FuncExpr   *fexpr;
	HeapTuple	tuple;
	TupleDesc	tupdesc;

	fexpr = stmt->funcexpr;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for procedure %u", fexpr->funcid);

	tupdesc = build_function_result_tupdesc_t(tuple);

	ReleaseSysCache(tuple);

	/*
	 * The result of build_function_result_tupdesc_t has the right column
	 * names, but it just has the declared output argument types, which is the
	 * wrong thing in polymorphic cases.  Get the correct types by examining
	 * stmt->outargs.  We intentionally keep the atttypmod as -1 and the
	 * attcollation as the type's default, since that's always the appropriate
	 * thing for function outputs; there's no point in considering any
	 * additional info available from outargs.  Note that tupdesc is null if
	 * there are no outargs.
	 *
	 * build_function_result_tupdesc_t 的结果有正确的列名，但只有声明的输出参数类型，多态情况下这是错的。
	 * 通过检查 stmt->outargs 取得正确类型。我们有意把 atttypmod 保持为 -1，attcollation 保持为类型默认值，
	 * 因为对函数输出这总是合适的；没有必要考虑 outargs 中的额外信息。若没有 outargs，tupdesc 为 null。
	 */
	if (tupdesc)
	{
		Assert(tupdesc->natts == list_length(stmt->outargs));
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			Node	   *outarg = (Node *) list_nth(stmt->outargs, i);

			TupleDescInitEntry(tupdesc,
							   i + 1,
							   NameStr(att->attname),
							   exprType(outarg),
							   -1,
							   0);
		}
	}

	return tupdesc;
}
