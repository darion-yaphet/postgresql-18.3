/*-------------------------------------------------------------------------
 *
 * explain_state.c
 *	  Code for initializing and accessing ExplainState objects
 *
 * 初始化并访问 ExplainState 对象的代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * In-core options have hard-coded fields inside ExplainState; e.g. if
 * the user writes EXPLAIN (BUFFERS) then ExplainState's "buffers" member
 * will be set to true. Extensions can also register options using
 * RegisterExtensionExplainOption; so that e.g. EXPLAIN (BICYCLE 'red')
 * will invoke a designated handler that knows what the legal values are
 * for the BICYCLE option. However, it's not enough for an extension to be
 * able to parse new options: it also needs a place to store the results
 * of that parsing, and an ExplainState has no 'bicycle' field.
 *
 * 内建选项在 ExplainState 里有固定字段，例如 EXPLAIN (BUFFERS) 会把
 * buffers 设为 true。扩展可用 RegisterExtensionExplainOption 注册选项，
 * 例如 EXPLAIN (BICYCLE 'red') 会调用知道合法取值的处理函数。
 * 扩展还需要存放解析结果的地方，而 ExplainState 没有对应字段。
 *
 * To solve this problem, an ExplainState can contain an array of opaque
 * pointers, one per extension. An extension can use GetExplainExtensionId
 * to acquire an integer ID to acquire an offset into this array that is
 * reserved for its exclusive use, and then use GetExplainExtensionState
 * and SetExplainExtensionState to read and write its own private state
 * within an ExplainState.
 *
 * 为此，ExplainState 可以包含每个扩展一个的不透明指针数组。
 * 扩展用 GetExplainExtensionId 取得整数 ID，作为该数组中专供自己使用的下标，
 * 再用 GetExplainExtensionState 和 SetExplainExtensionState 读写私有状态。
 *
 * Note that there is no requirement that the name of the option match
 * the name of the extension; e.g. a pg_explain_conveyance extension could
 * implement options for BICYCLE, MONORAIL, etc.
 *
 * 选项名不必与扩展名相同。例如 pg_explain_conveyance 可以实现 BICYCLE、MONORAIL 等选项。
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain_state.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_state.h"

/*
 * 核心流程概览：
 * NewExplainState 建立带默认选项的 ExplainState。
 * ParseExplainOptionList 解析 EXPLAIN 选项并做一致性检查。
 * 扩展通过 RegisterExtensionExplainOption 注册选项，用 GetExplainExtensionId
 * 取得槽位，再用 Get/SetExplainExtensionState 存放私有解析结果。
 */
/* Hook to perform additional EXPLAIN options validation */
/*
 *
 * 用于额外校验 EXPLAIN 选项的钩子。
 */
explain_validate_options_hook_type explain_validate_options_hook = NULL;

typedef struct
{
	const char *option_name;
	ExplainOptionHandler option_handler;
} ExplainExtensionOption;

static const char **ExplainExtensionNameArray = NULL;
static int	ExplainExtensionNamesAssigned = 0;
static int	ExplainExtensionNamesAllocated = 0;

static ExplainExtensionOption *ExplainExtensionOptionArray = NULL;
static int	ExplainExtensionOptionsAssigned = 0;
static int	ExplainExtensionOptionsAllocated = 0;

/*
 * Create a new ExplainState struct initialized with default options.
 *
 * 创建带默认选项的 ExplainState。
 */
ExplainState *
NewExplainState(void)
{
	ExplainState *es = (ExplainState *) palloc0(sizeof(ExplainState));

	/* Set default options (most fields can be left as zeroes). */
	/*
	 *
	 * 设置默认选项（多数字段保持为零）。
	 */
	es->costs = true;
	/* Prepare output buffer. */
	/*
	 *
	 * 准备输出缓冲区。
	 */
	es->str = makeStringInfo();

	return es;
}

/*
 * Parse a list of EXPLAIN options and update an ExplainState accordingly.
 *
 * 解析 EXPLAIN 选项列表并据此更新 ExplainState。
 */
void
ParseExplainOptionList(ExplainState *es, List *options, ParseState *pstate)
{
	ListCell   *lc;
	bool		timing_set = false;
	bool		buffers_set = false;
	bool		summary_set = false;

	/* Parse options list. */
	/*
	 *
	 * 解析选项列表。
	 */
	foreach(lc, options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "analyze") == 0)
			es->analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "verbose") == 0)
			es->verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "costs") == 0)
			es->costs = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffers") == 0)
		{
			buffers_set = true;
			es->buffers = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "wal") == 0)
			es->wal = defGetBoolean(opt);
		else if (strcmp(opt->defname, "settings") == 0)
			es->settings = defGetBoolean(opt);
		else if (strcmp(opt->defname, "generic_plan") == 0)
			es->generic = defGetBoolean(opt);
		else if (strcmp(opt->defname, "timing") == 0)
		{
			timing_set = true;
			es->timing = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "summary") == 0)
		{
			summary_set = true;
			es->summary = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "memory") == 0)
			es->memory = defGetBoolean(opt);
		else if (strcmp(opt->defname, "serialize") == 0)
		{
			if (opt->arg)
			{
				char	   *p = defGetString(opt);

				if (strcmp(p, "off") == 0 || strcmp(p, "none") == 0)
					es->serialize = EXPLAIN_SERIALIZE_NONE;
				else if (strcmp(p, "text") == 0)
					es->serialize = EXPLAIN_SERIALIZE_TEXT;
				else if (strcmp(p, "binary") == 0)
					es->serialize = EXPLAIN_SERIALIZE_BINARY;
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized value for %s option \"%s\": \"%s\"",
									"EXPLAIN", opt->defname, p),
							 parser_errposition(pstate, opt->location)));
			}
			else
			{
				/* SERIALIZE without an argument is taken as 'text' */
				/*
				 *
				 * 不带参数的 SERIALIZE 视为 'text'。
				 */
				es->serialize = EXPLAIN_SERIALIZE_TEXT;
			}
		}
		else if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			if (strcmp(p, "text") == 0)
				es->format = EXPLAIN_FORMAT_TEXT;
			else if (strcmp(p, "xml") == 0)
				es->format = EXPLAIN_FORMAT_XML;
			else if (strcmp(p, "json") == 0)
				es->format = EXPLAIN_FORMAT_JSON;
			else if (strcmp(p, "yaml") == 0)
				es->format = EXPLAIN_FORMAT_YAML;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for %s option \"%s\": \"%s\"",
								"EXPLAIN", opt->defname, p),
						 parser_errposition(pstate, opt->location)));
		}
		else if (!ApplyExtensionExplainOption(es, opt, pstate))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							"EXPLAIN", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* check that WAL is used with EXPLAIN ANALYZE */
	/*
	 *
	 * 检查 WAL 是否与 EXPLAIN ANALYZE 一起使用。
	 */
	if (es->wal && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE", "WAL")));

	/* if the timing was not set explicitly, set default value */
	/*
	 *
	 * 若未显式设置 timing，则使用默认值。
	 */
	es->timing = (timing_set) ? es->timing : es->analyze;

	/* if the buffers was not set explicitly, set default value */
	/*
	 *
	 * 若未显式设置 buffers，则使用默认值。
	 */
	es->buffers = (buffers_set) ? es->buffers : es->analyze;

	/* check that timing is used with EXPLAIN ANALYZE */
	/*
	 *
	 * 检查 timing 是否与 EXPLAIN ANALYZE 一起使用。
	 */
	if (es->timing && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE", "TIMING")));

	/* check that serialize is used with EXPLAIN ANALYZE */
	/*
	 *
	 * 检查 serialize 是否与 EXPLAIN ANALYZE 一起使用。
	 */
	if (es->serialize != EXPLAIN_SERIALIZE_NONE && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE", "SERIALIZE")));

	/* check that GENERIC_PLAN is not used with EXPLAIN ANALYZE */
	/*
	 *
	 * 检查 GENERIC_PLAN 没有与 EXPLAIN ANALYZE 一起使用。
	 */
	if (es->generic && es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s options %s and %s cannot be used together",
						"EXPLAIN", "ANALYZE", "GENERIC_PLAN")));

	/* if the summary was not set explicitly, set default value */
	/*
	 *
	 * 若未显式设置 summary，则使用默认值。
	 */
	es->summary = (summary_set) ? es->summary : es->analyze;

	/* plugin specific option validation */
	/*
	 *
	 * 插件特定的选项校验。
	 */
	if (explain_validate_options_hook)
		(*explain_validate_options_hook) (es, options, pstate);
}

/*
 * Map the name of an EXPLAIN extension to an integer ID.
 *
 * 把 EXPLAIN 扩展名映射为整数 ID。
 *
 * Within the lifetime of a particular backend, the same name will be mapped
 * to the same ID every time. IDs are not stable across backends. Use the ID
 * that you get from this function to call GetExplainExtensionState and
 * SetExplainExtensionState.
 *
 * 在同一个后端的生命周期内，同一名字总是映射到同一 ID。
 * ID 在不同后端之间不稳定。用本函数返回的 ID 调用
 * GetExplainExtensionState 和 SetExplainExtensionState。
 *
 * extension_name is assumed to be a constant string or allocated in storage
 * that will never be freed.
 *
 * 假定 extension_name 是常量字符串，或分配在永不释放的存储中。
 */
int
GetExplainExtensionId(const char *extension_name)
{
	/* Search for an existing extension by this name; if found, return ID. */
	/*
	 *
	 * 按该名字查找已有扩展；找到则返回 ID。
	 */
	for (int i = 0; i < ExplainExtensionNamesAssigned; ++i)
		if (strcmp(ExplainExtensionNameArray[i], extension_name) == 0)
			return i;

	/* If there is no array yet, create one. */
	/*
	 *
	 * 若还没有数组，则创建一个。
	 */
	if (ExplainExtensionNameArray == NULL)
	{
		ExplainExtensionNamesAllocated = 16;
		ExplainExtensionNameArray = (const char **)
			MemoryContextAlloc(TopMemoryContext,
							   ExplainExtensionNamesAllocated
							   * sizeof(char *));
	}

	/* If there's an array but it's currently full, expand it. */
	/*
	 *
	 * 若数组已满，则扩容。
	 */
	if (ExplainExtensionNamesAssigned >= ExplainExtensionNamesAllocated)
	{
		int			i = pg_nextpower2_32(ExplainExtensionNamesAssigned + 1);

		ExplainExtensionNameArray = (const char **)
			repalloc(ExplainExtensionNameArray, i * sizeof(char *));
		ExplainExtensionNamesAllocated = i;
	}

	/* Assign and return new ID. */
	/*
	 *
	 * 分配并返回新的 ID。
	 */
	ExplainExtensionNameArray[ExplainExtensionNamesAssigned] = extension_name;
	return ExplainExtensionNamesAssigned++;
}

/*
 * Get extension-specific state from an ExplainState.
 *
 * 从 ExplainState 读取扩展私有状态。
 *
 * See comments for SetExplainExtensionState, below.
 *
 * 见下面 SetExplainExtensionState 的注释。
 */
void *
GetExplainExtensionState(ExplainState *es, int extension_id)
{
	Assert(extension_id >= 0);

	if (extension_id >= es->extension_state_allocated)
		return NULL;

	return es->extension_state[extension_id];
}

/*
 * Store extension-specific state into an ExplainState.
 *
 * 把扩展私有状态写入 ExplainState。
 *
 * To use this function, first obtain an integer extension_id using
 * GetExplainExtensionId. Then use this function to store an opaque pointer
 * in the ExplainState. Later, you can retrieve the opaque pointer using
 * GetExplainExtensionState.
 *
 * 使用本函数前，先用 GetExplainExtensionId 取得整数 extension_id，
 * 再把不透明指针存入 ExplainState。之后可用 GetExplainExtensionState 取回。
 */
void
SetExplainExtensionState(ExplainState *es, int extension_id, void *opaque)
{
	Assert(extension_id >= 0);

	/* If there is no array yet, create one. */
	/*
	 *
	 * 若还没有数组，则创建一个。
	 */
	if (es->extension_state == NULL)
	{
		es->extension_state_allocated =
			Max(16, pg_nextpower2_32(extension_id + 1));
		es->extension_state =
			palloc0(es->extension_state_allocated * sizeof(void *));
	}

	/* If there's an array but it's currently full, expand it. */
	/*
	 *
	 * 若数组已满，则扩容。
	 */
	if (extension_id >= es->extension_state_allocated)
	{
		int			i;

		i = pg_nextpower2_32(extension_id + 1);
		es->extension_state = (void **)
			repalloc0(es->extension_state,
					  es->extension_state_allocated * sizeof(void *),
					  i * sizeof(void *));
		es->extension_state_allocated = i;
	}

	es->extension_state[extension_id] = opaque;
}

/*
 * Register a new EXPLAIN option.
 *
 * 注册一个新的 EXPLAIN 选项。
 *
 * When option_name is used as an EXPLAIN option, handler will be called and
 * should update the ExplainState passed to it. See comments at top of file
 * for a more detailed explanation.
 *
 * 当 option_name 用作 EXPLAIN 选项时会调用 handler，由它更新传入的 ExplainState。
 * 详见文件顶部的说明。
 *
 * option_name is assumed to be a constant string or allocated in storage
 * that will never be freed.
 *
 * 假定 option_name 是常量字符串，或分配在永不释放的存储中。
 */
void
RegisterExtensionExplainOption(const char *option_name,
							   ExplainOptionHandler handler)
{
	ExplainExtensionOption *exopt;

	/* Search for an existing option by this name; if found, update handler. */
	/*
	 *
	 * 按该名字查找已有选项；找到则更新处理函数。
	 */
	for (int i = 0; i < ExplainExtensionOptionsAssigned; ++i)
	{
		if (strcmp(ExplainExtensionOptionArray[i].option_name,
				   option_name) == 0)
		{
			ExplainExtensionOptionArray[i].option_handler = handler;
			return;
		}
	}

	/* If there is no array yet, create one. */
	/*
	 *
	 * 若还没有数组，则创建一个。
	 */
	if (ExplainExtensionOptionArray == NULL)
	{
		ExplainExtensionOptionsAllocated = 16;
		ExplainExtensionOptionArray = (ExplainExtensionOption *)
			MemoryContextAlloc(TopMemoryContext,
							   ExplainExtensionOptionsAllocated
							   * sizeof(char *));
	}

	/* If there's an array but it's currently full, expand it. */
	/*
	 *
	 * 若数组已满，则扩容。
	 */
	if (ExplainExtensionOptionsAssigned >= ExplainExtensionOptionsAllocated)
	{
		int			i = pg_nextpower2_32(ExplainExtensionOptionsAssigned + 1);

		ExplainExtensionOptionArray = (ExplainExtensionOption *)
			repalloc(ExplainExtensionOptionArray, i * sizeof(char *));
		ExplainExtensionOptionsAllocated = i;
	}

	/* Assign and return new ID. */
	/*
	 *
	 * 分配并返回新的 ID。
	 */
	exopt = &ExplainExtensionOptionArray[ExplainExtensionOptionsAssigned++];
	exopt->option_name = option_name;
	exopt->option_handler = handler;
}

/*
 * Apply an EXPLAIN option registered by an extension.
 *
 * 应用扩展注册的 EXPLAIN 选项。
 *
 * If no extension has registered the named option, returns false. Otherwise,
 * calls the appropriate handler function and then returns true.
 *
 * 若没有扩展注册该选项，返回 false；否则调用相应处理函数并返回 true。
 */
bool
ApplyExtensionExplainOption(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	for (int i = 0; i < ExplainExtensionOptionsAssigned; ++i)
	{
		if (strcmp(ExplainExtensionOptionArray[i].option_name,
				   opt->defname) == 0)
		{
			ExplainExtensionOptionArray[i].option_handler(es, opt, pstate);
			return true;
		}
	}

	return false;
}
