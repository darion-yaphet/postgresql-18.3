/*-------------------------------------------------------------------------
 *
 * parse_param.c
 *	  handle parameters in parser
 *	  处理解析器中的参数
 *
 * This code covers two cases that are used within the core backend:
 *		* a fixed list of parameters with known types
 *		* an expandable list of parameters whose types can optionally
 *		  be determined from context
 * In both cases, only explicit $n references (ParamRef nodes) are supported.
 * 这一代码涵盖了核心后端中使用的两种情况：
 *		* 具有已知类型的固定参数列表
 *		* 具有可选根据上下文确定的可扩展参数列表
 * 在这两种情况下，仅支持显式的 $n 引用（ParamRef 节点）。
 *
 * Note that other approaches to parameters are possible using the parser
 * hooks defined in ParseState.
 * 注意，使用 ParseState 中定义的解析器挂钩，也可以采用其他参数处理方法。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 部分版权 (c) 1996-2025, PostgreSQL 全球开发组
 * 部分版权 (c) 1994, 加州大学董事会
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_param.c
 * 标识
 *	  src/backend/parser/parse_param.c
 *
 *-------------------------------------------------------------------------
 *
 * 核心流程解释：
 * 本文件主要用于在 PostgreSQL 解析器中处理参数引用（形如 $1, $2 等的 ParamRef 节点）。
 * 解析器对参数的处理主要包括两个核心流程：
 *
 * 1. 固定参数处理流程（Fixed Parameters）：
 *    - 调用 setup_parse_fixed_parameters 进行初始化，设置 fixed_paramref_hook 钩子函数。
 *    - 当解析器遇到 $n 参数引用时，触发 fixed_paramref_hook 钩子。
 *    - 钩子函数根据已知的参数类型数组，验证参数编号是否越界或合法，并构建并返回一个包含对应类型 Oid 的 Param 节点。
 *
 * 2. 可变参数处理流程（Variable Parameters）：
 *    - 调用 setup_parse_variable_parameters 进行初始化，设置 variable_paramref_hook 和 variable_coerce_param_hook 钩子函数。
 *    - 当遇到 $n 时，触发 variable_paramref_hook 钩子。如果参数编号超出了当前已知的参数数组大小，则动态扩容该类型数组。
 *    - 新遇到的参数最初被标记为 UNKNOWNOID（未知类型）。
 *    - 在后续的表达式分析中，如果遇到需要对该参数进行隐式类型转换（如比较、赋值），则会调用 variable_coerce_param_hook 钩子。
 *    - 在 variable_coerce_param_hook 中，解析器根据目标上下文类型更新对应的参数类型。如果多次推导出的类型不一致，则抛出“参数类型不一致”的错误。
 *    - 解析完成后，调用 check_variable_parameters 遍历整个查询树，确保所有具有相同编号 of 参数都被推导为一致的类型，对于无法推导出具体类型的参数也会进行校验。
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_param.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


typedef struct FixedParamState
{
	const Oid  *paramTypes;		/* array of parameter type OIDs
								   参数类型 OID 数组 */
	int			numParams;		/* number of array entries
								   数组条目数量 */
} FixedParamState;

/*
 * In the varparams case, the caller-supplied OID array (if any) can be
 * re-palloc'd larger at need.  A zero array entry means that parameter number
 * hasn't been seen, while UNKNOWNOID means the parameter has been used but
 * its type is not yet known.
 * 在可变参数 (varparams) 的情况下，调用者提供的 OID 数组（如果有的话）可以在需要时通过 re-palloc 扩容。
 * 值为零的数组条目表示该参数编号尚未被遇到，而 UNKNOWNOID 表示该参数已被使用但其类型尚不清楚。
 */
typedef struct VarParamState
{
	Oid		  **paramTypes;		/* array of parameter type OIDs
								   参数类型 OID 数组 */
	int		   *numParams;		/* number of array entries
								   数组条目数量 */
} VarParamState;

static Node *fixed_paramref_hook(ParseState *pstate, ParamRef *pref);
static Node *variable_paramref_hook(ParseState *pstate, ParamRef *pref);
static Node *variable_coerce_param_hook(ParseState *pstate, Param *param,
										Oid targetTypeId, int32 targetTypeMod,
										int location);
static bool check_parameter_resolution_walker(Node *node, ParseState *pstate);
static bool query_contains_extern_params_walker(Node *node, void *context);


/*
 * Set up to process a query containing references to fixed parameters.
 * 设置以处理包含对固定参数引用的查询。
 *
 * 函数作用：
 * 初始化 ParseState，为固定数量和已知类型的参数绑定解析挂钩。
 * 它会将固定参数状态结构体 FixedParamState 绑定到 pstate->p_ref_hook_state 中，
 * 并注册 fixed_paramref_hook 钩子函数，这样当解析器遇到类似 $1 的参数引用时，
 * 就会调用该钩子函数。
 */
void
setup_parse_fixed_parameters(ParseState *pstate,
							 const Oid *paramTypes, int numParams)
{
	FixedParamState *parstate = palloc(sizeof(FixedParamState));

	parstate->paramTypes = paramTypes;
	parstate->numParams = numParams;
	pstate->p_ref_hook_state = parstate;
	pstate->p_paramref_hook = fixed_paramref_hook;
	/* no need to use p_coerce_param_hook */
}

/*
 * Set up to process a query containing references to variable parameters.
 * 设置以处理包含对可变参数引用的查询。
 *
 * 函数作用：
 * 初始化 ParseState，为可变数量和类型的参数绑定解析挂钩。
 * 它不仅注册了处理参数引用的 variable_paramref_hook 钩子，
 * 还注册了 variable_coerce_param_hook 钩子，以便在后续解析过程中，
 * 根据参数的使用上下文来隐式推导和强转参数的具体数据类型。
 */
void
setup_parse_variable_parameters(ParseState *pstate,
								Oid **paramTypes, int *numParams)
{
	VarParamState *parstate = palloc(sizeof(VarParamState));

	parstate->paramTypes = paramTypes;
	parstate->numParams = numParams;
	pstate->p_ref_hook_state = parstate;
	pstate->p_paramref_hook = variable_paramref_hook;
	pstate->p_coerce_param_hook = variable_coerce_param_hook;
}

/*
 * Transform a ParamRef using fixed parameter types.
 * 使用固定参数类型转换 ParamRef。
 *
 * 函数作用：
 * 当解析器遇到固定参数引用（形如 $n）时，会调用此钩子函数。
 * 它会验证参数编号是否合法（不能小于等于0，也不能超过传入的参数总数，且类型必须有效），
 * 然后构建并返回一个 PARAM_EXTERN 类型的 Param 节点，其类型直接从预设的 paramTypes 数组中获取。
 */
static Node *
fixed_paramref_hook(ParseState *pstate, ParamRef *pref)
{
	FixedParamState *parstate = (FixedParamState *) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	Param	   *param;

	/* Check parameter number is valid
	 * 检查参数编号是否有效
	 */
	if (paramno <= 0 || paramno > parstate->numParams ||
		!OidIsValid(parstate->paramTypes[paramno - 1]))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", paramno),
				 parser_errposition(pstate, pref->location)));

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = parstate->paramTypes[paramno - 1];
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

/*
 * Transform a ParamRef using variable parameter types.
 *
 * The only difference here is we must enlarge the parameter type array
 * as needed.
 * 使用可变参数类型转换 ParamRef。
 *
 * 这里的唯一区别是，我们必须在需要时扩大参数类型数组。
 *
 * 函数作用：
 * 当解析器遇到可变参数引用（形如 $n）时调用此钩子。
 * 如果遇到的参数编号大于当前参数类型数组的大小，则通过 repalloc0_array 或 palloc0_array 进行动态扩容。
 * 对于初次遇到的参数，其类型暂时设为 UNKNOWNOID。最后构造并返回一个 Param 节点。
 */
static Node *
variable_paramref_hook(ParseState *pstate, ParamRef *pref)
{
	VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	Oid		   *pptype;
	Param	   *param;

	/* Check parameter number is in range
	 * 检查参数编号是否在范围内
	 */
	if (paramno <= 0 || paramno > MaxAllocSize / sizeof(Oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("there is no parameter $%d", paramno),
				 parser_errposition(pstate, pref->location)));
	if (paramno > *parstate->numParams)
	{
		/* Need to enlarge param array
		 * 需要扩大参数数组
		 */
		if (*parstate->paramTypes)
			*parstate->paramTypes = repalloc0_array(*parstate->paramTypes, Oid,
													*parstate->numParams, paramno);
		else
			*parstate->paramTypes = palloc0_array(Oid, paramno);
		*parstate->numParams = paramno;
	}

	/* Locate param's slot in array
	 * 在数组中定位参数的槽位
	 */
	pptype = &(*parstate->paramTypes)[paramno - 1];

	/* If not seen before, initialize to UNKNOWN type
	 * 如果以前没见过，初始化为 UNKNOWN 类型
	 */
	if (*pptype == InvalidOid)
		*pptype = UNKNOWNOID;

	/*
	 * If the argument is of type void and it's procedure call, interpret it
	 * as unknown.  This allows the JDBC driver to not have to distinguish
	 * function and procedure calls.  See also another component of this hack
	 * in ParseFuncOrColumn().
	 * 如果参数的类型为 void 且是一个过程调用，则将其解释为 unknown。
	 * 这使得 JDBC 驱动程序不需要区分函数调用和过程调用。
	 * 另请参阅 ParseFuncOrColumn() 中该黑客手段的另一个组成部分。
	 */
	if (*pptype == VOIDOID && pstate->p_expr_kind == EXPR_KIND_CALL_ARGUMENT)
		*pptype = UNKNOWNOID;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = *pptype;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

/*
 * Coerce a Param to a query-requested datatype, in the varparams case.
 * 在可变参数 (varparams) 的情况下，将 Param 强制转换为查询请求的数据类型。
 *
 * 函数作用：
 * 在分析可变参数的过程中，当需要对尚未确定类型的参数（类型为 UNKNOWNOID）进行隐式类型转换时调用此钩子。
 * - 如果该参数类型尚未被成功推导，则将其类型记录并更新为目标强制转换类型（targetTypeId）。
 * - 如果已经推导过该参数的类型，且与当前目标类型一致，则不做处理。
 * - 如果已经推导过该参数类型但与当前目标类型不一致，则抛出类型不一致的错误。
 * 随后，函数更新 Param 节点的参数类型为 targetTypeId。
 */
static Node *
variable_coerce_param_hook(ParseState *pstate, Param *param,
						   Oid targetTypeId, int32 targetTypeMod,
						   int location)
{
	if (param->paramkind == PARAM_EXTERN && param->paramtype == UNKNOWNOID)
	{
		/*
		 * Input is a Param of previously undetermined type, and we want to
		 * update our knowledge of the Param's type.
		 * 输入是一个先前未确定类型的 Param，我们希望更新我们对该 Param 类型的了解。
		 */
		VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;
		Oid		   *paramTypes = *parstate->paramTypes;
		int			paramno = param->paramid;

		if (paramno <= 0 ||		/* shouldn't happen, but...
								   不应该发生，但是... */
			paramno > *parstate->numParams)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PARAMETER),
					 errmsg("there is no parameter $%d", paramno),
					 parser_errposition(pstate, param->location)));

		if (paramTypes[paramno - 1] == UNKNOWNOID)
		{
			/* We've successfully resolved the type
			 * 我们已成功解析出该类型
			 */
			paramTypes[paramno - 1] = targetTypeId;
		}
		else if (paramTypes[paramno - 1] == targetTypeId)
		{
			/* We previously resolved the type, and it matches
			 * 我们之前已经解析过该类型，且匹配成功
			 */
		}
		else
		{
			/* Oops
			 * 发生错误
			 */
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
					 errmsg("inconsistent types deduced for parameter $%d",
							paramno),
					 errdetail("%s versus %s",
							   format_type_be(paramTypes[paramno - 1]),
							   format_type_be(targetTypeId)),
					 parser_errposition(pstate, param->location)));
		}

		param->paramtype = targetTypeId;

		/*
		 * Note: it is tempting here to set the Param's paramtypmod to
		 * targetTypeMod, but that is probably unwise because we have no
		 * infrastructure that enforces that the value delivered for a Param
		 * will match any particular typmod.  Leaving it -1 ensures that a
		 * run-time length check/coercion will occur if needed.
		 * 注意：这里很想将 Param 的 paramtypmod 设置为 targetTypeMod，但这可能是明智的，
		 * 因为我们没有基础设施来强制要求为 Param 交付的值必须匹配任何特定的 typmod。
		 * 将其保留为 -1 可以确保在需要时会发生运行时长度检查/强转。
		 */
		param->paramtypmod = -1;

		/*
		 * This module always sets a Param's collation to be the default for
		 * its datatype.  If that's not what you want, you should be using the
		 * more general parser substitution hooks.
		 * 该模块总是将 Param 的排序规则 (collation) 设置为其数据类型的默认排序规则。
		 * 如果这不是你想要的，你应该使用更通用的解析器替换钩子。
		 */
		param->paramcollid = get_typcollation(param->paramtype);

		/* Use the leftmost of the param's and coercion's locations
		 * 使用参数位置和强转位置中偏左的那个
		 */
		if (location >= 0 &&
			(param->location < 0 || location < param->location))
			param->location = location;

		return (Node *) param;
	}

	/* Else signal to proceed with normal coercion
	 * 否则，指示继续进行正常的强制转换
	 */
	return NULL;
}

/*
 * Check for consistent assignment of variable parameters after completion
 * of parsing with parse_variable_parameters.
 *
 * Note: this code intentionally does not check that all parameter positions
 * were used, nor that all got non-UNKNOWN types assigned.  Caller of parser
 * should enforce that if it's important.
 * 在使用 parse_variable_parameters 完成解析后，检查可变参数的一致性分配。
 *
 * 注意：此代码故意不检查是否使用了所有的参数位置，也不检查所有位置是否都分配了非 UNKNOWN 类型。
 * 如果这重要，解析器的调用者应该强制执行此操作。
 *
 * 函数作用：
 * 在整个查询树解析分析完成后，如果确实生成了至少一个可变参数，
 * 就会调用本函数。它通过遍历整个查询树（调用 query_tree_walker），
 * 利用 check_parameter_resolution_walker 校验每个 Param 节点的具体类型是否与最终推导出的统一类型一致。
 */
void
check_variable_parameters(ParseState *pstate, Query *query)
{
	VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;

	/* If numParams is zero then no Params were generated, so no work
	 * 如果 numParams 为零，则没有生成任何 Param，因此无需工作
	 */
	if (*parstate->numParams > 0)
		(void) query_tree_walker(query,
								 check_parameter_resolution_walker,
								 pstate, 0);
}

/*
 * Traverse a fully-analyzed tree to verify that parameter symbols
 * match their types.  We need this because some Params might still
 * be UNKNOWN, if there wasn't anything to force their coercion,
 * and yet other instances seen later might have gotten coerced.
 * 遍历一个已完全分析的树，以验证参数符号与其类型是否匹配。
 * 我们需要这样做，因为如果没有什么能强制它们进行强转，某些 Param 可能仍然是 UNKNOWN，
 * 然而随后看到的其他实例可能已经得到了强转。
 *
 * 函数作用：
 * 用于遍历查询树的 walker 回调函数。
 * 它会深入遍历包括子查询在内的各种节点，定位所有的外部参数（PARAM_EXTERN）。
 * 校验每个 Param 的 paramtype 是否与最终记录在参数类型数组中的推导类型一致。
 * 如果发现某个参数无法确定其数据类型（例如，它的类型仍然是 UNKNOWNOID，或者与最终确定的类型不符），则报错。
 */
static bool
check_parameter_resolution_walker(Node *node, ParseState *pstate)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
		{
			VarParamState *parstate = (VarParamState *) pstate->p_ref_hook_state;
			int			paramno = param->paramid;

			if (paramno <= 0 || /* shouldn't happen, but... */
				paramno > *parstate->numParams)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_PARAMETER),
						 errmsg("there is no parameter $%d", paramno),
						 parser_errposition(pstate, param->location)));

			if (param->paramtype != (*parstate->paramTypes)[paramno - 1])
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_PARAMETER),
						 errmsg("could not determine data type of parameter $%d",
								paramno),
						 parser_errposition(pstate, param->location)));
		}
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery
		 * 递归进入 RTE 子查询或尚未计划的子链接子查询
		 */
		return query_tree_walker((Query *) node,
								 check_parameter_resolution_walker,
								 pstate, 0);
	}
	return expression_tree_walker(node, check_parameter_resolution_walker,
								  pstate);
}

/*
 * Check to see if a fully-parsed query tree contains any PARAM_EXTERN Params.
 * 检查一个完全解析的查询树是否包含任何 PARAM_EXTERN 参数。
 *
 * 函数作用：
 * 遍历并检查传入的查询树（Query）中是否包含任何外部参数（PARAM_EXTERN）。
 * 它利用 query_contains_extern_params_walker 深度搜索整个查询树，
 * 如果找到至少一个 PARAM_EXTERN，则返回 true，否则返回 false。
 */
bool
query_contains_extern_params(Query *query)
{
	return query_tree_walker(query,
							 query_contains_extern_params_walker,
							 NULL, 0);
}

/*
 * query_contains_extern_params_walker
 *
 * Walker function to check if a node or its children contain PARAM_EXTERN Params.
 * 用于检查节点或其子节点是否包含 PARAM_EXTERN 参数的遍历函数。
 *
 * 函数作用：
 * 用于遍历查询树的 walker 回调函数，专门用来查找外部参数。
 * 只要遇到 Param 节点且其类型为 PARAM_EXTERN，就立即返回 true 终止遍历。
 * 同样会递归进入子查询。
 */
static bool
query_contains_extern_params_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN)
			return true;
		return false;
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery
		 * 递归进入 RTE 子查询或尚未计划的子链接子查询
		 */
		return query_tree_walker((Query *) node,
								 query_contains_extern_params_walker,
								 context, 0);
	}
	return expression_tree_walker(node, query_contains_extern_params_walker,
								  context);
}
