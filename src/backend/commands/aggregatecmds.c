/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * 聚集操作命令的例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/aggregatecmds.c
 *
 * DESCRIPTION
 *	  The "DefineAggregate" routine takes the parse tree and picks out the
 *	  appropriate arguments/flags, passing the results to the
 *	  "AggregateCreate" routine (in src/backend/catalog), which does the
 *	  actual catalog-munging.  DefineAggregate also verifies the permission of
 *	  the user to execute the command.
 *
 * DefineAggregate 从分析树取出参数和标志，交给 catalog 中的 AggregateCreate
 * 去改系统目录。DefineAggregate 也会检查用户是否有权执行该命令。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"


/*
 * 核心流程概览：
 * DefineAggregate 解析旧式 BASETYPE 或新式参数列表，查找输入类型、
 * transtype、转移函数和初始值，做权限与伪类型检查后调用 AggregateCreate 写入目录。
 */
static char extractModify(DefElem *defel);


/*
 *	DefineAggregate
 *
 * DefineAggregate：
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise,
 * "args" is a pair, whose first element is a list of FunctionParameter structs
 * defining the agg's arguments (both direct and aggregated), and whose second
 * element is an Integer node with the number of direct args, or -1 if this
 * isn't an ordered-set aggregate.
 * "parameters" is a list of DefElem representing the agg's definition clauses.
 *
 * oldstyle 表示 8.2 之前的写法，输入类型由参数里的 BASETYPE 指定。
 * 否则 args 是一对：前者是定义直接参数和聚集参数的 FunctionParameter 列表，
 * 后者是直接参数个数的 Integer，有序集聚集之外则为 -1。
 * parameters 是表示聚集定义子句的 DefElem 列表。
 */
ObjectAddress
DefineAggregate(ParseState *pstate,
				List *name,
				List *args,
				bool oldstyle,
				List *parameters,
				bool replace)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	char		aggKind = AGGKIND_NORMAL;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	List	   *combinefuncName = NIL;
	List	   *serialfuncName = NIL;
	List	   *deserialfuncName = NIL;
	List	   *mtransfuncName = NIL;
	List	   *minvtransfuncName = NIL;
	List	   *mfinalfuncName = NIL;
	bool		finalfuncExtraArgs = false;
	bool		mfinalfuncExtraArgs = false;
	char		finalfuncModify = 0;
	char		mfinalfuncModify = 0;
	List	   *sortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	TypeName   *mtransType = NULL;
	int32		transSpace = 0;
	int32		mtransSpace = 0;
	char	   *initval = NULL;
	char	   *minitval = NULL;
	char	   *parallel = NULL;
	int			numArgs;
	int			numDirectArgs = 0;
	oidvector  *parameterTypes;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	Oid			transTypeId;
	Oid			mtransTypeId = InvalidOid;
	char		transTypeType;
	char		mtransTypeType = 0;
	char		proparallel = PROPARALLEL_UNSAFE;
	ListCell   *pl;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表拆成名称和命名空间。
	 */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(aggNamespace));

	/* Deconstruct the output of the aggr_args grammar production */
	/*
	 *
	 * 拆开 aggr_args 语法产生式的结果。
	 */
	if (!oldstyle)
	{
		Assert(list_length(args) == 2);
		numDirectArgs = intVal(lsecond(args));
		if (numDirectArgs >= 0)
			aggKind = AGGKIND_ORDERED_SET;
		else
			numDirectArgs = 0;
		args = linitial_node(List, args);
	}

	/* Examine aggregate's definition clauses */
	/*
	 *
	 * 检查聚集的定义子句。
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = lfirst_node(DefElem, pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
		 * for sfunc, stype, initcond.
		 *
		 * sfunc1、stype1、initcond1 是 sfunc、stype、initcond 的过时写法，仍然接受。
		 */
		if (strcmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "combinefunc") == 0)
			combinefuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "serialfunc") == 0)
			serialfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "deserialfunc") == 0)
			deserialfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "msfunc") == 0)
			mtransfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "minvfunc") == 0)
			minvtransfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "mfinalfunc") == 0)
			mfinalfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "finalfunc_extra") == 0)
			finalfuncExtraArgs = defGetBoolean(defel);
		else if (strcmp(defel->defname, "mfinalfunc_extra") == 0)
			mfinalfuncExtraArgs = defGetBoolean(defel);
		else if (strcmp(defel->defname, "finalfunc_modify") == 0)
			finalfuncModify = extractModify(defel);
		else if (strcmp(defel->defname, "mfinalfunc_modify") == 0)
			mfinalfuncModify = extractModify(defel);
		else if (strcmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "hypothetical") == 0)
		{
			if (defGetBoolean(defel))
			{
				if (aggKind == AGGKIND_NORMAL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("only ordered-set aggregates can be hypothetical")));
				aggKind = AGGKIND_HYPOTHETICAL;
			}
		}
		else if (strcmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "sspace") == 0)
			transSpace = defGetInt32(defel);
		else if (strcmp(defel->defname, "mstype") == 0)
			mtransType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "msspace") == 0)
			mtransSpace = defGetInt32(defel);
		else if (strcmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (strcmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else if (strcmp(defel->defname, "minitcond") == 0)
			minitval = defGetString(defel);
		else if (strcmp(defel->defname, "parallel") == 0)
			parallel = defGetString(defel);
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * make sure we have our required definitions
	 *
	 * 确认必需的定义都已给出。
	 */
	if (transType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate stype must be specified")));
	if (transfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate sfunc must be specified")));

	/*
	 * if mtransType is given, mtransfuncName and minvtransfuncName must be as
	 * well; if not, then none of the moving-aggregate options should have
	 * been given.
	 *
	 * 若给出了 mtransType，则必须同时给出 mtransfuncName 和 minvtransfuncName；
	 * 否则不应给出任何移动聚集选项。
	 */
	if (mtransType != NULL)
	{
		if (mtransfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate msfunc must be specified when mstype is specified")));
		if (minvtransfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate minvfunc must be specified when mstype is specified")));
	}
	else
	{
		if (mtransfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate msfunc must not be specified without mstype")));
		if (minvtransfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate minvfunc must not be specified without mstype")));
		if (mfinalfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate mfinalfunc must not be specified without mstype")));
		if (mtransSpace != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate msspace must not be specified without mstype")));
		if (minitval != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate minitcond must not be specified without mstype")));
	}

	/*
	 * Default values for modify flags can only be determined once we know the
	 * aggKind.
	 *
	 * 只有知道 aggKind 之后，才能确定 modify 标志的默认值。
	 */
	if (finalfuncModify == 0)
		finalfuncModify = (aggKind == AGGKIND_NORMAL) ? AGGMODIFY_READ_ONLY : AGGMODIFY_READ_WRITE;
	if (mfinalfuncModify == 0)
		mfinalfuncModify = (aggKind == AGGKIND_NORMAL) ? AGGMODIFY_READ_ONLY : AGGMODIFY_READ_WRITE;

	/*
	 * look up the aggregate's input datatype(s).
	 *
	 * 查找聚集的输入数据类型。
	 */
	if (oldstyle)
	{
		/*
		 * Old style: use basetype parameter.  This supports aggregates of
		 * zero or one input, with input type ANY meaning zero inputs.
		 *
		 * 旧风格：使用 basetype 参数。支持零个或一个输入，输入类型 ANY 表示零个输入。
		 *
		 * Historically we allowed the command to look like basetype = 'ANY'
		 * so we must do a case-insensitive comparison for the name ANY. Ugh.
		 *
		 * 历史上允许写成 basetype = 'ANY'，因此必须对名字 ANY 做不区分大小写的比较。
		 */
		Oid			aggArgTypes[1];

		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes[0] = InvalidOid;
		}
		else
		{
			numArgs = 1;
			aggArgTypes[0] = typenameTypeId(NULL, baseType);
		}
		parameterTypes = buildoidvector(aggArgTypes, numArgs);
		allParameterTypes = NULL;
		parameterModes = NULL;
		parameterNames = NULL;
		parameterDefaults = NIL;
		variadicArgType = InvalidOid;
	}
	else
	{
		/*
		 * New style: args is a list of FunctionParameters (possibly zero of
		 * 'em).  We share functioncmds.c's code for processing them.
		 *
		 * 新风格：args 是 FunctionParameter 列表（可以一个都没有）。处理逻辑与 functioncmds.c 共用。
		 */
		Oid			requiredResultType;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		numArgs = list_length(args);
		interpret_function_parameter_list(pstate,
										  args,
										  InvalidOid,
										  OBJECT_AGGREGATE,
										  &parameterTypes,
										  NULL,
										  &allParameterTypes,
										  &parameterModes,
										  &parameterNames,
										  NULL,
										  &parameterDefaults,
										  &variadicArgType,
										  &requiredResultType);
		/* Parameter defaults are not currently allowed by the grammar */
		/*
		 *
		 * 语法目前不允许参数默认值。
		 */
		Assert(parameterDefaults == NIL);
		/* There shouldn't have been any OUT parameters, either */
		/*
		 *
		 * 也不应出现 OUT 参数。
		 */
		Assert(requiredResultType == InvalidOid);
	}

	/*
	 * look up the aggregate's transtype.
	 *
	 * 查找聚集的转移类型 transtype。
	 *
	 * transtype can't be a pseudo-type, since we need to be able to store
	 * values of the transtype.  However, we can allow polymorphic transtype
	 * in some cases (AggregateCreate will check).  Also, we allow "internal"
	 * for functions that want to pass pointers to private data structures;
	 * but allow that only to superusers, since you could crash the system (or
	 * worse) by connecting up incompatible internal-using functions in an
	 * aggregate.
	 *
	 * transtype 不能是伪类型，因为必须能存储该类型的值。
	 * 某些情况下允许多态 transtype（由 AggregateCreate 检查）。
	 * 也允许 internal，供函数传递私有结构指针，但仅限超级用户，
	 * 否则把不兼容的 internal 函数接进聚集可能使系统崩溃。
	 */
	transTypeId = typenameTypeId(NULL, transType);
	transTypeType = get_typtype(transTypeId);
	if (transTypeType == TYPTYPE_PSEUDO &&
		!IsPolymorphicType(transTypeId))
	{
		if (transTypeId == INTERNALOID && superuser())
			 /* okay */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate transition data type cannot be %s",
							format_type_be(transTypeId))));
	}

	if (serialfuncName && deserialfuncName)
	{
		/*
		 * Serialization is only needed/allowed for transtype INTERNAL.
		 *
		 * 只有 transtype 为 INTERNAL 时才需要并允许序列化。
		 */
		if (transTypeId != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("serialization functions may be specified only when the aggregate transition data type is %s",
							format_type_be(INTERNALOID))));
	}
	else if (serialfuncName || deserialfuncName)
	{
		/*
		 * Cannot specify one function without the other.
		 *
		 * 不能只指定其中一个函数。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("must specify both or neither of serialization and deserialization functions")));
	}

	/*
	 * If a moving-aggregate transtype is specified, look that up.  Same
	 * restrictions as for transtype.
	 *
	 * 若指定了移动聚集的 transtype，则查找它。限制与 transtype 相同。
	 */
	if (mtransType)
	{
		mtransTypeId = typenameTypeId(NULL, mtransType);
		mtransTypeType = get_typtype(mtransTypeId);
		if (mtransTypeType == TYPTYPE_PSEUDO &&
			!IsPolymorphicType(mtransTypeId))
		{
			if (mtransTypeId == INTERNALOID && superuser())
				 /* okay */ ;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate transition data type cannot be %s",
								format_type_be(mtransTypeId))));
		}
	}

	/*
	 * If we have an initval, and it's not for a pseudotype (particularly a
	 * polymorphic type), make sure it's acceptable to the type's input
	 * function.  We will store the initval as text, because the input
	 * function isn't necessarily immutable (consider "now" for timestamp),
	 * and we want to use the runtime not creation-time interpretation of the
	 * value.  However, if it's an incorrect value it seems much more
	 * user-friendly to complain at CREATE AGGREGATE time.
	 *
	 * 若有 initval，且不是伪类型（尤其是多态类型），则确认类型的输入函数能接受它。
	 * initval 以文本保存，因为输入函数不一定 immutable（例如 timestamp 的 "now"），
	 * 应在运行时而不是创建时解释。但若值不合法，在 CREATE AGGREGATE 时就报错更友好。
	 */
	if (initval && transTypeType != TYPTYPE_PSEUDO)
	{
		Oid			typinput,
					typioparam;

		getTypeInputInfo(transTypeId, &typinput, &typioparam);
		(void) OidInputFunctionCall(typinput, initval, typioparam, -1);
	}

	/*
	 * Likewise for moving-aggregate initval.
	 *
	 * 移动聚集的 initval 同样处理。
	 */
	if (minitval && mtransTypeType != TYPTYPE_PSEUDO)
	{
		Oid			typinput,
					typioparam;

		getTypeInputInfo(mtransTypeId, &typinput, &typioparam);
		(void) OidInputFunctionCall(typinput, minitval, typioparam, -1);
	}

	if (parallel)
	{
		if (strcmp(parallel, "safe") == 0)
			proparallel = PROPARALLEL_SAFE;
		else if (strcmp(parallel, "restricted") == 0)
			proparallel = PROPARALLEL_RESTRICTED;
		else if (strcmp(parallel, "unsafe") == 0)
			proparallel = PROPARALLEL_UNSAFE;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("parameter \"parallel\" must be SAFE, RESTRICTED, or UNSAFE")));
	}

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 *
	 * 大部分参数检查在 AggregateCreate 内部完成。
	 */
	return AggregateCreate(aggName, /* aggregate name */
	/*
	 *
	 * 聚集名。
	 */
						   aggNamespace,	/* namespace */
						   /*
						    *
						    * 命名空间。
						    */
						   replace,
						   aggKind,
						   numArgs,
						   numDirectArgs,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   variadicArgType,
						   transfuncName,	/* step function name */
						   /*
						    *
						    * 步进函数名。
						    */
						   finalfuncName,	/* final function name */
						   /*
						    *
						    * 最终函数名。
						    */
						   combinefuncName, /* combine function name */
						   /*
						    *
						    * 合并函数名。
						    */
						   serialfuncName,	/* serial function name */
						   /*
						    *
						    * 序列化函数名。
						    */
						   deserialfuncName,	/* deserial function name */
						   /*
						    *
						    * 反序列化函数名。
						    */
						   mtransfuncName,	/* fwd trans function name */
						   /*
						    *
						    * 正向转移函数名。
						    */
						   minvtransfuncName,	/* inv trans function name */
						   /*
						    *
						    * 逆向转移函数名。
						    */
						   mfinalfuncName,	/* final function name */
						   /*
						    *
						    * 最终函数名。
						    */
						   finalfuncExtraArgs,
						   mfinalfuncExtraArgs,
						   finalfuncModify,
						   mfinalfuncModify,
						   sortoperatorName,	/* sort operator name */
						   /*
						    *
						    * 排序操作符名。
						    */
						   transTypeId, /* transition data type */
						   /*
						    *
						    * 转移数据类型。
						    */
						   transSpace,	/* transition space */
						   /*
						    *
						    * 转移状态空间。
						    */
						   mtransTypeId,	/* transition data type */
						   /*
						    *
						    * 转移数据类型。
						    */
						   mtransSpace, /* transition space */
						   /*
						    *
						    * 转移状态空间。
						    */
						   initval, /* initial condition */
						   /*
						    *
						    * 初始条件。
						    */
						   minitval,	/* initial condition */
						   /*
						    *
						    * 初始条件。
						    */
						   proparallel);	/* parallel safe? */
						   /*
						    *
						    * 是否 parallel safe？
						    */
}

/*
 * Convert the string form of [m]finalfunc_modify to the catalog representation
 *
 * 把 [m]finalfunc_modify 的字符串形式转换成目录表示。
 */
static char
extractModify(DefElem *defel)
{
	char	   *val = defGetString(defel);

	if (strcmp(val, "read_only") == 0)
		return AGGMODIFY_READ_ONLY;
	if (strcmp(val, "shareable") == 0)
		return AGGMODIFY_SHAREABLE;
	if (strcmp(val, "read_write") == 0)
		return AGGMODIFY_READ_WRITE;
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("parameter \"%s\" must be READ_ONLY, SHAREABLE, or READ_WRITE",
					defel->defname)));
	return 0;					/* keep compiler quiet */
	/*
	 *
	 * 避免编译器因未使用的返回值告警。
	 */
}
