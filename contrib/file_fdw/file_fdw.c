/*-------------------------------------------------------------------------
 *
 * file_fdw.c
 *		  foreign-data wrapper for server-side flat files (or programs).
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/file_fdw/file_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_foreign_table.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/acl.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC_EXT(
					.name = "file_fdw",
					.version = PG_VERSION
);

/*
 * Describes the valid options for objects that use this wrapper.
 *
 * 描述使用此包装器的对象的有效选项。
 */
struct FileFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Valid options for file_fdw.
 * These options are based on the options for the COPY FROM command.
 * But note that force_not_null and force_null are handled as boolean options
 * attached to a column, not as table options.
 *
 * file_fdw 的有效选项。这些选项基于 COPY FROM 命令的选项。但请注意，force_not_null 和force_null 被作为附加到列的布尔选项处理，而不是作为表选项。
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileGetOptions(), which currently doesn't bother to look at user mappings.
 *
 * 注意：如果要为用户映射添加新选项，则需要修改 fileGetOptions()，目前该函数不关心用户映射。
 */
static const struct FileFdwOption valid_options[] = {
	/* Data source options
	 *
	 * 数据源选项
	 */
	{"filename", ForeignTableRelationId},
	{"program", ForeignTableRelationId},

	/* Format options
	 *
	 * 格式选项
	 */
	/* oids option is not supported
	 *
	 * 不支持 oids 选项
	 */
	{"format", ForeignTableRelationId},
	{"header", ForeignTableRelationId},
	{"delimiter", ForeignTableRelationId},
	{"quote", ForeignTableRelationId},
	{"escape", ForeignTableRelationId},
	{"null", ForeignTableRelationId},
	{"default", ForeignTableRelationId},
	{"encoding", ForeignTableRelationId},
	{"on_error", ForeignTableRelationId},
	{"log_verbosity", ForeignTableRelationId},
	{"reject_limit", ForeignTableRelationId},
	{"force_not_null", AttributeRelationId},
	{"force_null", AttributeRelationId},

	/*
	 * force_quote is not supported by file_fdw because it's for COPY TO.
	 *
	 * file_fdw 不支持force_quote，因为它用于复制到。
	 */

	/* Sentinel */
	{NULL, InvalidOid}
};

/*
 * FDW-specific information for RelOptInfo.fdw_private.
 *
 * RelOptInfo.fdw_private 的 FDW 特定信息。
 */
typedef struct FileFdwPlanState
{
	char	   *filename;		/* file or program to read from */
	bool		is_program;		/* true if filename represents an OS command */
	List	   *options;		/* merged COPY options, excluding filename and
								 * is_program */
	BlockNumber pages;			/* estimate of file's physical size */
	double		ntuples;		/* estimate of number of data rows */
} FileFdwPlanState;

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 *
 * ForeignScanState.fdw_state 的 FDW 特定信息。
 */
typedef struct FileFdwExecutionState
{
	char	   *filename;		/* file or program to read from */
	bool		is_program;		/* true if filename represents an OS command */
	List	   *options;		/* merged COPY options, excluding filename and
								 * is_program */
	CopyFromState cstate;		/* COPY execution state */
} FileFdwExecutionState;

/*
 * SQL functions
 *
 * SQL函数
 */
PG_FUNCTION_INFO_V1(file_fdw_handler);
PG_FUNCTION_INFO_V1(file_fdw_validator);

/*
 * FDW callback routines
 *
 * FDW 回调例程
 */
static void fileGetForeignRelSize(PlannerInfo *root,
								  RelOptInfo *baserel,
								  Oid foreigntableid);
static void fileGetForeignPaths(PlannerInfo *root,
								RelOptInfo *baserel,
								Oid foreigntableid);
static ForeignScan *fileGetForeignPlan(PlannerInfo *root,
									   RelOptInfo *baserel,
									   Oid foreigntableid,
									   ForeignPath *best_path,
									   List *tlist,
									   List *scan_clauses,
									   Plan *outer_plan);
static void fileExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void fileBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *fileIterateForeignScan(ForeignScanState *node);
static void fileReScanForeignScan(ForeignScanState *node);
static void fileEndForeignScan(ForeignScanState *node);
static bool fileAnalyzeForeignTable(Relation relation,
									AcquireSampleRowsFunc *func,
									BlockNumber *totalpages);
static bool fileIsForeignScanParallelSafe(PlannerInfo *root, RelOptInfo *rel,
										  RangeTblEntry *rte);

/*
 * Helper functions
 *
 * 辅助函数
 */
static bool is_valid_option(const char *option, Oid context);
static void fileGetOptions(Oid foreigntableid,
						   char **filename,
						   bool *is_program,
						   List **other_options);
static List *get_file_fdw_attribute_options(Oid relid);
static bool check_selective_binary_conversion(RelOptInfo *baserel,
											  Oid foreigntableid,
											  List **columns);
static void estimate_size(PlannerInfo *root, RelOptInfo *baserel,
						  FileFdwPlanState *fdw_private);
static void estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
						   FileFdwPlanState *fdw_private,
						   Cost *startup_cost, Cost *total_cost);
static int	file_acquire_sample_rows(Relation onerel, int elevel,
									 HeapTuple *rows, int targrows,
									 double *totalrows, double *totaldeadrows);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 *
 * 外部数据包装处理程序函数：返回一个结构体，其中包含指向我的回调例程的指针。
 */
Datum
file_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	fdwroutine->GetForeignRelSize = fileGetForeignRelSize;
	fdwroutine->GetForeignPaths = fileGetForeignPaths;
	fdwroutine->GetForeignPlan = fileGetForeignPlan;
	fdwroutine->ExplainForeignScan = fileExplainForeignScan;
	fdwroutine->BeginForeignScan = fileBeginForeignScan;
	fdwroutine->IterateForeignScan = fileIterateForeignScan;
	fdwroutine->ReScanForeignScan = fileReScanForeignScan;
	fdwroutine->EndForeignScan = fileEndForeignScan;
	fdwroutine->AnalyzeForeignTable = fileAnalyzeForeignTable;
	fdwroutine->IsForeignScanParallelSafe = fileIsForeignScanParallelSafe;

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * 验证为使用 file_fdw 的 FOREIGN DATA WRAPPER、SERVER、USER MAPPING 或 FOREIGN TABLE 提供的通用选项。
 *
 * Raise an ERROR if the option or its value is considered invalid.
 *
 * 如果选项或其值被认为无效，则引发错误。
 */
Datum
file_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	char	   *filename = NULL;
	DefElem    *force_not_null = NULL;
	DefElem    *force_null = NULL;
	List	   *other_options = NIL;
	ListCell   *cell;

	/*
	 * Check that only options supported by file_fdw, and allowed for the
	 * current object type, are given.
	 *
	 * 检查是否仅给出了 file_fdw 支持且当前对象类型允许的选项。
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			const struct FileFdwOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with a valid option that looks similar, if there is one.
			 *
			 * 指定了未知选项，抱怨它。提供一个提示，其中包含看起来相似的有效选项（如果有）。
			 */
			initClosestMatch(&match_state, def->defname, 4);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->optname);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));
		}

		/*
		 * Separate out filename, program, and column-specific options, since
		 * ProcessCopyOptions won't accept them.
		 *
		 * 将文件名、程序和特定于列的选项分开，因为 ProcessCopyOptions 不会接受它们。
		 */
		if (strcmp(def->defname, "filename") == 0 ||
			strcmp(def->defname, "program") == 0)
		{
			if (filename)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			/*
			 * Check permissions for changing which file or program is used by
			 * the file_fdw.
			 *
			 * 检查更改 file_fdw 使用的文件或程序的权限。
			 *
			 * Only members of the role 'pg_read_server_files' are allowed to
			 * set the 'filename' option of a file_fdw foreign table, while
			 * only members of the role 'pg_execute_server_program' are
			 * allowed to set the 'program' option.  This is because we don't
			 * want regular users to be able to control which file gets read
			 * or which program gets executed.
			 *
			 * 仅允许“pg_read_server_files”角色的成员设置 file_fdw 外部表的“filename”选项，而仅允许“pg_execute_server_program”角色的成员设置“program”选项。  这是因为我们不希望普通用户能够控制读取哪个文件或执行哪个程序。
			 *
			 * Putting this sort of permissions check in a validator is a bit
			 * of a crock, but there doesn't seem to be any other place that
			 * can enforce the check more cleanly.
			 *
			 * 将这种权限检查放在验证器中有点麻烦，但似乎没有任何其他地方可以更干净地强制执行检查。
			 *
			 * Note that the valid_options[] array disallows setting filename
			 * and program at any options level other than foreign table ---
			 * otherwise there'd still be a security hole.
			 *
			 * 请注意，valid_options[] 数组不允许在外部表以外的任何选项级别设置文件名和程序——否则仍然存在安全漏洞。
			 */
			if (strcmp(def->defname, "filename") == 0 &&
				!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set the \"%s\" option of a file_fdw foreign table",
								"filename"),
						 errdetail("Only roles with privileges of the \"%s\" role may set this option.",
								   "pg_read_server_files")));

			if (strcmp(def->defname, "program") == 0 &&
				!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set the \"%s\" option of a file_fdw foreign table",
								"program"),
						 errdetail("Only roles with privileges of the \"%s\" role may set this option.",
								   "pg_execute_server_program")));

			filename = defGetString(def);
		}

		/*
		 * force_not_null is a boolean option; after validation we can discard
		 * it - it will be retrieved later in get_file_fdw_attribute_options()
		 *
		 * force_not_null 是一个布尔选项；验证后我们可以丢弃它 - 稍后将在 get_file_fdw_attribute_options() 中检索它
		 */
		else if (strcmp(def->defname, "force_not_null") == 0)
		{
			if (force_not_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("Option \"force_not_null\" supplied more than once for a column.")));
			force_not_null = def;
			/* Don't care what the value is, as long as it's a legal boolean
			 *
			 * 不在乎值是什么，只要它是合法的布尔值即可
			 */
			(void) defGetBoolean(def);
		}
		/* See comments for force_not_null above
		 *
		 * 请参阅上面对force_not_null的评论
		 */
		else if (strcmp(def->defname, "force_null") == 0)
		{
			if (force_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("Option \"force_null\" supplied more than once for a column.")));
			force_null = def;
			(void) defGetBoolean(def);
		}
		else
			other_options = lappend(other_options, def);
	}

	/*
	 * Now apply the core COPY code's validation logic for more checks.
	 *
	 * 现在应用核心 COPY 代码的验证逻辑进行更多检查。
	 */
	ProcessCopyOptions(NULL, NULL, true, other_options);

	/*
	 * Either filename or program option is required for file_fdw foreign
	 * tables.
	 *
	 * file_fdw 外部表需要文件名或程序选项。
	 */
	if (catalog == ForeignTableRelationId && filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg("either filename or program is required for file_fdw foreign tables")));

	PG_RETURN_VOID();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 *
 * 检查提供的选项是否是有效选项之一。 context 是保存选项所属对象的目录的 Oid。
 */
static bool
is_valid_option(const char *option, Oid context)
{
	const struct FileFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a file_fdw foreign table.
 *
 * 获取 file_fdw 外部表的选项。
 *
 * We have to separate out filename/program from the other options because
 * those must not appear in the options list passed to the core COPY code.
 *
 * 我们必须将文件名/程序与其他选项分开，因为这些选项不得出现在传递给核心 COPY 代码的选项列表中。
 */
static void
fileGetOptions(Oid foreigntableid,
			   char **filename, bool *is_program, List **other_options)
{
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;
	List	   *options;
	ListCell   *lc;

	/*
	 * Extract options from FDW objects.  We ignore user mappings because
	 * file_fdw doesn't have any options that can be specified there.
	 *
	 * 从 FDW 对象中提取选项。  我们忽略用户映射，因为 file_fdw 没有任何可以在那里指定的选项。
	 *
	 * (XXX Actually, given the current contents of valid_options[], there's
	 * no point in examining anything except the foreign table's own options.
	 * Simplify?)
	 *
	 * （XXX 实际上，考虑到 valid_options[] 的当前内容，除了外部表自己的选项之外，没有必要检查任何内容。简化吗？）
	 */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);

	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);
	options = list_concat(options, get_file_fdw_attribute_options(foreigntableid));

	/*
	 * Separate out the filename or program option (we assume there is only
	 * one).
	 *
	 * 分离出文件名或程序选项（我们假设只有一个）。
	 */
	*filename = NULL;
	*is_program = false;
	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "filename") == 0)
		{
			*filename = defGetString(def);
			options = foreach_delete_current(options, lc);
			break;
		}
		else if (strcmp(def->defname, "program") == 0)
		{
			*filename = defGetString(def);
			*is_program = true;
			options = foreach_delete_current(options, lc);
			break;
		}
	}

	/*
	 * The validator should have checked that filename or program was included
	 * in the options, but check again, just in case.
	 *
	 * 验证器应该检查选项中是否包含文件名或程序，但为了以防万一，请再次检查。
	 */
	if (*filename == NULL)
		elog(ERROR, "either filename or program is required for file_fdw foreign tables");

	*other_options = options;
}

/*
 * Retrieve per-column generic options from pg_attribute and construct a list
 * of DefElems representing them.
 *
 * 从 pg_attribute 检索每列通用选项并构建代表它们的 DefElem 列表。
 *
 * At the moment we only have "force_not_null", and "force_null",
 * which should each be combined into a single DefElem listing all such
 * columns, since that's what COPY expects.
 *
 * 目前我们只有“force_not_null”和“force_null”，它们应该组合成一个单独的 DefElem 列出所有此类列，因为这是 COPY 所期望的。
 */
static List *
get_file_fdw_attribute_options(Oid relid)
{
	Relation	rel;
	TupleDesc	tupleDesc;
	AttrNumber	natts;
	AttrNumber	attnum;
	List	   *fnncolumns = NIL;
	List	   *fncolumns = NIL;

	List	   *options = NIL;

	rel = table_open(relid, AccessShareLock);
	tupleDesc = RelationGetDescr(rel);
	natts = tupleDesc->natts;

	/* Retrieve FDW options for all user-defined attributes.
	 *
	 * 检索所有用户定义属性的 FDW 选项。
	 */
	for (attnum = 1; attnum <= natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupleDesc, attnum - 1);
		List	   *column_options;
		ListCell   *lc;

		/* Skip dropped attributes.
		 *
		 * 跳过删除的属性。
		 */
		if (attr->attisdropped)
			continue;

		column_options = GetForeignColumnOptions(relid, attnum);
		foreach(lc, column_options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "force_not_null") == 0)
			{
				if (defGetBoolean(def))
				{
					char	   *attname = pstrdup(NameStr(attr->attname));

					fnncolumns = lappend(fnncolumns, makeString(attname));
				}
			}
			else if (strcmp(def->defname, "force_null") == 0)
			{
				if (defGetBoolean(def))
				{
					char	   *attname = pstrdup(NameStr(attr->attname));

					fncolumns = lappend(fncolumns, makeString(attname));
				}
			}
			/* maybe in future handle other column options here
			 *
			 * 也许将来在这里处理其他列选项
			 */
		}
	}

	table_close(rel, AccessShareLock);

	/*
	 * Return DefElem only when some column(s) have force_not_null /
	 * force_null options set
	 *
	 * 仅当某些列设置了force_not_null/force_null选项时才返回DefElem
	 */
	if (fnncolumns != NIL)
		options = lappend(options, makeDefElem("force_not_null", (Node *) fnncolumns, -1));

	if (fncolumns != NIL)
		options = lappend(options, makeDefElem("force_null", (Node *) fncolumns, -1));

	return options;
}

/*
 * fileGetForeignRelSize
 *		Obtain relation size estimates for a foreign table
 *
 * fileGetForeignRelSize 获取外部表的关系大小估计
 */
static void
fileGetForeignRelSize(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid)
{
	FileFdwPlanState *fdw_private;

	/*
	 * Fetch options.  We only need filename (or program) at this point, but
	 * we might as well get everything and not need to re-fetch it later in
	 * planning.
	 *
	 * 获取选项。  此时我们只需要文件名（或程序），但我们也可以获取所有内容，而无需在以后的计划中重新获取它。
	 */
	fdw_private = (FileFdwPlanState *) palloc(sizeof(FileFdwPlanState));
	fileGetOptions(foreigntableid,
				   &fdw_private->filename,
				   &fdw_private->is_program,
				   &fdw_private->options);
	baserel->fdw_private = fdw_private;

	/* Estimate relation size
	 *
	 * 估计关系大小
	 */
	estimate_size(root, baserel, fdw_private);
}

/*
 * fileGetForeignPaths
 *		Create possible access paths for a scan on the foreign table
 *
 * fileGetForeignPaths 创建外部表扫描可能的访问路径
 *
 *		Currently we don't support any push-down feature, so there is only one
 *		possible access path, which simply returns all records in the order in
 *		the data file.
 *
 * 目前我们不支持任何下推功能，因此只有一种可能的访问路径，即简单地按数据文件中的顺序返回所有记录。
 */
static void
fileGetForeignPaths(PlannerInfo *root,
					RelOptInfo *baserel,
					Oid foreigntableid)
{
	FileFdwPlanState *fdw_private = (FileFdwPlanState *) baserel->fdw_private;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *columns;
	List	   *coptions = NIL;

	/* Decide whether to selectively perform binary conversion
	 *
	 * 决定是否选择性地进行二进制转换
	 */
	if (check_selective_binary_conversion(baserel,
										  foreigntableid,
										  &columns))
		coptions = list_make1(makeDefElem("convert_selectively",
										  (Node *) columns, -1));

	/* Estimate costs
	 *
	 * 估算成本
	 */
	estimate_costs(root, baserel, fdw_private,
				   &startup_cost, &total_cost);

	/*
	 * Create a ForeignPath node and add it as only possible path.  We use the
	 * fdw_private list of the path to carry the convert_selectively option;
	 * it will be propagated into the fdw_private list of the Plan node.
	 *
	 * 创建一个foreignpath节点并将其添加为唯一可能的路径。  我们使用路径的fdw_private列表来携带convert_selectively选项；它将被传播到 Plan 节点的 fdw_private 列表中。
	 *
	 * We don't support pushing join clauses into the quals of this path, but
	 * it could still have required parameterization due to LATERAL refs in
	 * its tlist.
	 *
	 * 我们不支持将 join 子句推入此路径的 quals 中，但由于其 tlist 中的 LATERAL refs，它仍然可能需要参数化。
	 */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 NULL,	/* default pathtarget */
									 baserel->rows,
									 0,
									 startup_cost,
									 total_cost,
									 NIL,	/* no pathkeys */
									 baserel->lateral_relids,
									 NULL,	/* no extra plan */
									 NIL,	/* no fdw_restrictinfo list */
									 coptions));

	/*
	 * If data file was sorted, and we knew it somehow, we could insert
	 * appropriate pathkeys into the ForeignPath node to tell the planner
	 * that.
	 *
	 * 如果数据文件已排序，并且我们以某种方式知道它，我们可以将适当的路径键插入到ForeignPath节点中以告诉规划器。
	 */
}

/*
 * fileGetForeignPlan
 *		Create a ForeignScan plan node for scanning the foreign table
 *
 * fileGetForeignPlan 创建ForeignScan计划节点，用于扫描外表
 */
static ForeignScan *
fileGetForeignPlan(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid,
				   ForeignPath *best_path,
				   List *tlist,
				   List *scan_clauses,
				   Plan *outer_plan)
{
	Index		scan_relid = baserel->relid;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 *
	 * 我们没有本地能力来评估限制子句，因此我们只是将所有 scan_clauses 放入计划节点的限定列表中以供执行器检查。  因此，我们在这里要做的就是从子句中删除 RestrictInfo 节点并忽略伪常量（这将在其他地方处理）。
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node
	 *
	 * 创建ForeignScan节点
	 */
	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							best_path->fdw_private,
							NIL,	/* no custom tlist */
							NIL,	/* no remote quals */
							outer_plan);
}

/*
 * fileExplainForeignScan
 *		Produce extra output for EXPLAIN
 *
 * fileExplainForeignScan 为 EXPLAIN 生成额外的输出
 */
static void
fileExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	char	   *filename;
	bool		is_program;
	List	   *options;

	/* Fetch options --- we only need filename and is_program at this point
	 *
	 * 获取选项 --- 此时我们只需要文件名和 is_program
	 */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &is_program, &options);

	if (is_program)
		ExplainPropertyText("Foreign Program", filename, es);
	else
		ExplainPropertyText("Foreign File", filename, es);

	/* Suppress file size if we're not showing cost details
	 *
	 * 如果我们不显示成本详细信息，请抑制文件大小
	 */
	if (es->costs)
	{
		struct stat stat_buf;

		if (!is_program &&
			stat(filename, &stat_buf) == 0)
			ExplainPropertyInteger("Foreign File Size", "b",
								   (int64) stat_buf.st_size, es);
	}
}

/*
 * fileBeginForeignScan
 *		Initiate access to the file by creating CopyState
 *
 * fileBeginForeignScan 通过创建 CopyState 发起对文件的访问
 */
static void
fileBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	char	   *filename;
	bool		is_program;
	List	   *options;
	CopyFromState cstate;
	FileFdwExecutionState *festate;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 *
	 * 在 EXPLAIN（无 ANALYZE）情况下不执行任何操作。  node->fdw_state 保持 NULL。
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Fetch options of foreign table
	 *
	 * 获取外部表的选项
	 */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &is_program, &options);

	/* Add any options from the plan (currently only convert_selectively)
	 *
	 * 添加计划中的任何选项（当前仅选择性地转换）
	 */
	options = list_concat(options, plan->fdw_private);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 *
	 * 从 FDW 选项创建 CopyState。  我们总是获取所有列，以匹配预期的 ScanTupleSlot 签名。
	 */
	cstate = BeginCopyFrom(NULL,
						   node->ss.ss_currentRelation,
						   NULL,
						   filename,
						   is_program,
						   NULL,
						   NIL,
						   options);

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 *
	 * 将状态保存在node->fdw_state中。  我们必须保存足够的信息才能再次调用 BeginCopyFrom()。
	 */
	festate = (FileFdwExecutionState *) palloc(sizeof(FileFdwExecutionState));
	festate->filename = filename;
	festate->is_program = is_program;
	festate->options = options;
	festate->cstate = cstate;

	node->fdw_state = festate;
}

/*
 * fileIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 *
 * fileIterateForeignScan 从数据文件中读取下一条记录并将其作为虚拟元组存储到 ScanTupleSlot 中
 */
static TupleTableSlot *
fileIterateForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext;
	MemoryContext oldcontext = CurrentMemoryContext;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	CopyFromState cstate = festate->cstate;
	ErrorContextCallback errcallback;

	/* Set up callback to identify error line number.
	 *
	 * 设置回调来识别错误行号。
	 */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * We pass ExprContext because there might be a use of the DEFAULT option
	 * in COPY FROM, so we may need to evaluate default expressions.
	 *
	 * 我们传递 ExprContext 是因为 COPY FROM 中可能会使用 DEFAULT 选项，因此我们可能需要计算默认表达式。
	 */
	econtext = GetPerTupleExprContext(estate);

retry:

	/*
	 * DEFAULT expressions need to be evaluated in a per-tuple context, so
	 * switch in case we are doing that.
	 *
	 * DEFAULT 表达式需要在每个元组上下文中进行计算，因此请切换以防止我们这样做。
	 */
	MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple.  If we don't find another row in the file, we
	 * just skip the last step, leaving the slot empty as required.
	 *
	 * 将虚拟元组加载到槽中的协议首先是 ExecClearTuple，然后填充 value/isnull 数组，然后是 ExecStoreVirtualTuple。  如果我们在文件中找不到另一行，我们只需跳过最后一步，根据需要将插槽留空。
	 *
	 */
	ExecClearTuple(slot);

	if (NextCopyFrom(cstate, econtext, slot->tts_values, slot->tts_isnull))
	{
		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
			cstate->escontext->error_occurred)
		{
			/*
			 * Soft error occurred, skip this tuple and just make
			 * ErrorSaveContext ready for the next NextCopyFrom. Since we
			 * don't set details_wanted and error_data is not to be filled,
			 * just resetting error_occurred is enough.
			 *
			 * 发生软错误，跳过此元组并让 ErrorSaveContext 为下一个 NextCopyFrom 做好准备。由于我们没有设置details_wanted并且error_data也不需要填写，所以只需重置error_occurred就足够了。
			 */
			cstate->escontext->error_occurred = false;

			/* Switch back to original memory context
			 *
			 * 切换回原来的内存上下文
			 */
			MemoryContextSwitchTo(oldcontext);

			/*
			 * Make sure we are interruptible while repeatedly calling
			 * NextCopyFrom() until no soft error occurs.
			 *
			 * 确保我们在重复调用 NextCopyFrom() 时可中断，直到不发生软错误。
			 */
			CHECK_FOR_INTERRUPTS();

			/*
			 * Reset the per-tuple exprcontext, to clean-up after expression
			 * evaluations etc.
			 *
			 * 重置每个元组的 exprcontext，以便在表达式求值等之后进行清理。
			 */
			ResetPerTupleExprContext(estate);

			if (cstate->opts.reject_limit > 0 &&
				cstate->num_errors > cstate->opts.reject_limit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("skipped more than REJECT_LIMIT (%" PRId64 ") rows due to data type incompatibility",
								cstate->opts.reject_limit)));

			/* Repeat NextCopyFrom() until no soft error occurs
			 *
			 * 重复NextCopyFrom()，直到没有发生软错误
			 */
			goto retry;
		}

		ExecStoreVirtualTuple(slot);
	}

	/* Switch back to original memory context
	 *
	 * 切换回原来的内存上下文
	 */
	MemoryContextSwitchTo(oldcontext);

	/* Remove error callback.
	 *
	 * 删除错误回调。
	 */
	error_context_stack = errcallback.previous;

	return slot;
}

/*
 * fileReScanForeignScan
 *		Rescan table, possibly with new parameters
 *
 * fileReScanForeignScan 重新扫描表，可能使用新参数
 */
static void
fileReScanForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	EndCopyFrom(festate->cstate);

	festate->cstate = BeginCopyFrom(NULL,
									node->ss.ss_currentRelation,
									NULL,
									festate->filename,
									festate->is_program,
									NULL,
									NIL,
									festate->options);
}

/*
 * fileEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 *
 * fileEndForeignScan 完成外部表扫描并处置用于此扫描的对象
 */
static void
fileEndForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	/* if festate is NULL, we are in EXPLAIN; nothing to do
	 *
	 * 如果 festate 为 NULL，则处于 EXPLAIN 状态；无事可做
	 */
	if (!festate)
		return;

	if (festate->cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
		festate->cstate->num_errors > 0 &&
		festate->cstate->opts.log_verbosity >= COPY_LOG_VERBOSITY_DEFAULT)
		ereport(NOTICE,
				errmsg_plural("%" PRIu64 " row was skipped due to data type incompatibility",
							  "%" PRIu64 " rows were skipped due to data type incompatibility",
							  festate->cstate->num_errors,
							  festate->cstate->num_errors));

	EndCopyFrom(festate->cstate);
}

/*
 * fileAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 *
 * fileAnalyzeForeignTable 测试是否支持分析该外表
 */
static bool
fileAnalyzeForeignTable(Relation relation,
						AcquireSampleRowsFunc *func,
						BlockNumber *totalpages)
{
	char	   *filename;
	bool		is_program;
	List	   *options;
	struct stat stat_buf;

	/* Fetch options of foreign table
	 *
	 * 获取外部表的选项
	 */
	fileGetOptions(RelationGetRelid(relation), &filename, &is_program, &options);

	/*
	 * If this is a program instead of a file, just return false to skip
	 * analyzing the table.  We could run the program and collect stats on
	 * whatever it currently returns, but it seems likely that in such cases
	 * the output would be too volatile for the stats to be useful.  Maybe
	 * there should be an option to enable doing this?
	 *
	 * 如果这是一个程序而不是文件，则只需返回 false 即可跳过分析表。  我们可以运行该程序并收集其当前返回的任何内容的统计信息，但在这种情况下，输出可能太不稳定，统计信息无法发挥作用。  也许应该有一个选项来启用此操作？
	 */
	if (is_program)
		return false;

	/*
	 * Get size of the file.  (XXX if we fail here, would it be better to just
	 * return false to skip analyzing the table?)
	 *
	 * 获取文件的大小。  （XXX如果我们在这里失败了，是不是直接返回 false 来跳过分析表会更好？）
	 */
	if (stat(filename, &stat_buf) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						filename)));

	/*
	 * Convert size to pages.  Must return at least 1 so that we can tell
	 * later on that pg_class.relpages is not default.
	 *
	 * 将大小转换为页面。  必须至少返回 1，以便我们稍后可以知道 pg_class.relpages 不是默认值。
	 */
	*totalpages = (stat_buf.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (*totalpages < 1)
		*totalpages = 1;

	*func = file_acquire_sample_rows;

	return true;
}

/*
 * fileIsForeignScanParallelSafe
 *		Reading a file, or external program, in a parallel worker should work
 *		just the same as reading it in the leader, so mark scans safe.
 *
 * fileIsForeignScanParallelSafe 在并行工作线程中读取文件或外部程序应该与在领导者中读取文件或外部程序一样，因此将扫描标记为安全。
 */
static bool
fileIsForeignScanParallelSafe(PlannerInfo *root, RelOptInfo *rel,
							  RangeTblEntry *rte)
{
	return true;
}

/*
 * check_selective_binary_conversion
 *
 * Check to see if it's useful to convert only a subset of the file's columns
 * to binary.  If so, construct a list of the column names to be converted,
 * return that at *columns, and return true.  (Note that it's possible to
 * determine that no columns need be converted, for instance with a COUNT(*)
 * query.  So we can't use returning a NIL list to indicate failure.)
 *
 * 检查仅将文件列的一部分转换为二进制是否有用。  如果是，则构造要转换的列名列表，在 *columns 处返回该列表，并返回 true。  （请注意，可以确定没有列需要转换，例如使用 COUNT(*) 查询。因此我们不能使用返回 NIL 列表来指示失败。）
 */
static bool
check_selective_binary_conversion(RelOptInfo *baserel,
								  Oid foreigntableid,
								  List **columns)
{
	ForeignTable *table;
	ListCell   *lc;
	Relation	rel;
	TupleDesc	tupleDesc;
	int			attidx;
	Bitmapset  *attrs_used = NULL;
	bool		has_wholerow = false;
	int			numattrs;
	int			i;

	*columns = NIL;				/* default result */

	/*
	 * Check format of the file.  If binary format, this is irrelevant.
	 *
	 * 检查文件的格式。  如果是二进制格式，则这是无关紧要的。
	 */
	table = GetForeignTable(foreigntableid);
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "format") == 0)
		{
			char	   *format = defGetString(def);

			if (strcmp(format, "binary") == 0)
				return false;
			break;
		}
	}

	/* Collect all the attributes needed for joins or final output.
	 *
	 * 收集连接或最终输出所需的所有属性。
	 */
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &attrs_used);

	/* Add all the attributes used by restriction clauses.
	 *
	 * 添加限制子句使用的所有属性。
	 */
	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &attrs_used);
	}

	/* Convert attribute numbers to column names.
	 *
	 * 将属性编号转换为列名称。
	 */
	rel = table_open(foreigntableid, AccessShareLock);
	tupleDesc = RelationGetDescr(rel);

	attidx = -1;
	while ((attidx = bms_next_member(attrs_used, attidx)) >= 0)
	{
		/* attidx is zero-based, attnum is the normal attribute number
		 *
		 * attidx 从零开始，attnum 是普通属性编号
		 */
		AttrNumber	attnum = attidx + FirstLowInvalidHeapAttributeNumber;

		if (attnum == 0)
		{
			has_wholerow = true;
			break;
		}

		/* Ignore system attributes.
		 *
		 * 忽略系统属性。
		 */
		if (attnum < 0)
			continue;

		/* Get user attributes.
		 *
		 * 获取用户属性。
		 */
		if (attnum > 0)
		{
			Form_pg_attribute attr = TupleDescAttr(tupleDesc, attnum - 1);
			char	   *attname = NameStr(attr->attname);

			/* Skip dropped attributes (probably shouldn't see any here).
			 *
			 * 跳过删除的属性（可能在这里看不到任何属性）。
			 */
			if (attr->attisdropped)
				continue;

			/*
			 * Skip generated columns (COPY won't accept them in the column
			 * list)
			 *
			 * 跳过生成的列（COPY 不会在列列表中接受它们）
			 */
			if (attr->attgenerated)
				continue;
			*columns = lappend(*columns, makeString(pstrdup(attname)));
		}
	}

	/* Count non-dropped user attributes while we have the tupdesc.
	 *
	 * 当我们有 tupdesc 时，计算未删除的用户属性。
	 */
	numattrs = 0;
	for (i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupleDesc, i);

		if (attr->attisdropped)
			continue;
		numattrs++;
	}

	table_close(rel, AccessShareLock);

	/* If there's a whole-row reference, fail: we need all the columns.
	 *
	 * 如果存在整行引用，则会失败：我们需要所有列。
	 */
	if (has_wholerow)
	{
		*columns = NIL;
		return false;
	}

	/* If all the user attributes are needed, fail.
	 *
	 * 如果需要所有用户属性，则失败。
	 */
	if (numattrs == list_length(*columns))
	{
		*columns = NIL;
		return false;
	}

	return true;
}

/*
 * Estimate size of a foreign table.
 *
 * 估计外部表的大小。
 *
 * The main result is returned in baserel->rows.  We also set
 * fdw_private->pages and fdw_private->ntuples for later use in the cost
 * calculation.
 *
 * 主要结果在baserel->rows 中返回。  我们还设置了 fdw_private->pages 和 fdw_private->ntuples 以便稍后在成本计算中使用。
 */
static void
estimate_size(PlannerInfo *root, RelOptInfo *baserel,
			  FileFdwPlanState *fdw_private)
{
	struct stat stat_buf;
	BlockNumber pages;
	double		ntuples;
	double		nrows;

	/*
	 * Get size of the file.  It might not be there at plan time, though, in
	 * which case we have to use a default estimate.  We also have to fall
	 * back to the default if using a program as the input.
	 *
	 * 获取文件的大小。  不过，在计划时它可能不存在，在这种情况下我们必须使用默认估计。  如果使用程序作为输入，我们还必须回退到默认值。
	 */
	if (fdw_private->is_program || stat(fdw_private->filename, &stat_buf) < 0)
		stat_buf.st_size = 10 * BLCKSZ;

	/*
	 * Convert size to pages for use in I/O cost estimate later.
	 *
	 * 将大小转换为页面，以便稍后在 I/O 成本估算中使用。
	 */
	pages = (stat_buf.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (pages < 1)
		pages = 1;
	fdw_private->pages = pages;

	/*
	 * Estimate the number of tuples in the file.
	 *
	 * 估计文件中元组的数量。
	 */
	if (baserel->tuples >= 0 && baserel->pages > 0)
	{
		/*
		 * We have # of pages and # of tuples from pg_class (that is, from a
		 * previous ANALYZE), so compute a tuples-per-page estimate and scale
		 * that by the current file size.
		 *
		 * 我们有来自 pg_class 的页数和元组数（即来自之前的 ANALYZE），因此计算每页元组的估计值并按当前文件大小进行缩放。
		 */
		double		density;

		density = baserel->tuples / (double) baserel->pages;
		ntuples = clamp_row_est(density * (double) pages);
	}
	else
	{
		/*
		 * Otherwise we have to fake it.  We back into this estimate using the
		 * planner's idea of the relation width; which is bogus if not all
		 * columns are being read, not to mention that the text representation
		 * of a row probably isn't the same size as its internal
		 * representation.  Possibly we could do something better, but the
		 * real answer to anyone who complains is "ANALYZE" ...
		 *
		 * 否则我们就必须伪造它。  我们使用规划者关于关系宽度的想法来进行此估计；如果不是所有列都被读取，那么这是假的，更不用说行的文本表示形式可能与其内部表示形式的大小不同。  也许我们可以做得更好，但对于任何抱怨的人来说，真正的答案是“分析”......
		 */
		int			tuple_width;

		tuple_width = MAXALIGN(baserel->reltarget->width) +
			MAXALIGN(SizeofHeapTupleHeader);
		ntuples = clamp_row_est((double) stat_buf.st_size /
								(double) tuple_width);
	}
	fdw_private->ntuples = ntuples;

	/*
	 * Now estimate the number of rows returned by the scan after applying the
	 * baserestrictinfo quals.
	 *
	 * 现在估计应用 baserestrictinfo quals 后扫描返回的行数。
	 */
	nrows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);

	nrows = clamp_row_est(nrows);

	/* Save the output-rows estimate for the planner
	 *
	 * 为规划器保存输出行估计
	 */
	baserel->rows = nrows;
}

/*
 * Estimate costs of scanning a foreign table.
 *
 * 估计扫描外部表的成本。
 *
 * Results are returned in *startup_cost and *total_cost.
 *
 * 结果在 *startup_cost 和 *total_cost 中返回。
 */
static void
estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
			   FileFdwPlanState *fdw_private,
			   Cost *startup_cost, Cost *total_cost)
{
	BlockNumber pages = fdw_private->pages;
	double		ntuples = fdw_private->ntuples;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/*
	 * We estimate costs almost the same way as cost_seqscan(), thus assuming
	 * that I/O costs are equivalent to a regular table file of the same size.
	 * However, we take per-tuple CPU costs as 10x of a seqscan, to account
	 * for the cost of parsing records.
	 *
	 * 我们估计成本的方式几乎与 cost_seqscan() 相同，因此假设 I/O 成本相当于相同大小的常规表文件。然而，我们将每个元组的 CPU 成本视为 seqscan 的 10 倍，以考虑解析记录的成本。
	 *
	 * In the case of a program source, this calculation is even more divorced
	 * from reality, but we have no good alternative; and it's not clear that
	 * the numbers we produce here matter much anyway, since there's only one
	 * access path for the rel.
	 *
	 * 如果是程序源的话，这样的计算就更脱离实际了，但我们也没有什么好的办法；并且不清楚我们在这里生成的数字是否重要，因为 rel 只有一个访问路径。
	 */
	run_cost += seq_page_cost * pages;

	*startup_cost = baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost * 10 + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;
	*total_cost = *startup_cost + run_cost;
}

/*
 * file_acquire_sample_rows -- acquire a random sample of rows from the table
 *
 * file_acquire_sample_rows -- 从表中获取行的随机样本
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the file and return it into
 * *totalrows.  Rows skipped due to on_error = 'ignore' are not included
 * in this count.  Note that *totaldeadrows is always set to 0.
 *
 * 选定的行将在调用者分配的数组 rows[] 中返回，该数组必须至少具有 targrows 条目。实际选择的行数作为函数结果返回。我们还计算文件中的总行数并将其返回到 *totalrows 中。  由于 on_error = 'ignore' 而跳过的行不包含在此计数中。  请注意，*totaldeadrows 始终设置为 0。
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the file.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 *
 * 请注意，返回的行列表并不总是按文件中的物理位置排序。  因此，稍后得出的相关性估计可能没有意义，但没关系，因为我们当前不使用估计（规划器只关注索引扫描的相关性）。
 */
static int
file_acquire_sample_rows(Relation onerel, int elevel,
						 HeapTuple *rows, int targrows,
						 double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;
	double		rowstoskip = -1;	/* -1 means not set yet */
	ReservoirStateData rstate;
	TupleDesc	tupDesc;
	Datum	   *values;
	bool	   *nulls;
	bool		found;
	char	   *filename;
	bool		is_program;
	List	   *options;
	CopyFromState cstate;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext = CurrentMemoryContext;
	MemoryContext tupcontext;

	Assert(onerel);
	Assert(targrows > 0);

	tupDesc = RelationGetDescr(onerel);
	values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));

	/* Fetch options of foreign table
	 *
	 * 获取外部表的选项
	 */
	fileGetOptions(RelationGetRelid(onerel), &filename, &is_program, &options);

	/*
	 * Create CopyState from FDW options.
	 *
	 * 从 FDW 选项创建 CopyState。
	 */
	cstate = BeginCopyFrom(NULL, onerel, NULL, filename, is_program, NULL, NIL,
						   options);

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read
	 * rows from the file with Copy routines.
	 *
	 * 使用每个元组内存上下文来防止用于通过复制例程从文件中读取行的内存泄漏。
	 */
	tupcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "file_fdw temporary context",
									   ALLOCSET_DEFAULT_SIZES);

	/* Prepare for sampling rows
	 *
	 * 准备采样行
	 */
	reservoir_init_selection_state(&rstate, targrows);

	/* Set up callback to identify error line number.
	 *
	 * 设置回调来识别错误行号。
	 */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	*totalrows = 0;
	*totaldeadrows = 0;
	for (;;)
	{
		/* Check for user-requested abort or sleep
		 *
		 * 检查用户请求的中止或睡眠
		 */
		vacuum_delay_point(true);

		/* Fetch next row
		 *
		 * 获取下一行
		 */
		MemoryContextReset(tupcontext);
		MemoryContextSwitchTo(tupcontext);

		found = NextCopyFrom(cstate, NULL, values, nulls);

		MemoryContextSwitchTo(oldcontext);

		if (!found)
			break;

		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
			cstate->escontext->error_occurred)
		{
			/*
			 * Soft error occurred, skip this tuple and just make
			 * ErrorSaveContext ready for the next NextCopyFrom. Since we
			 * don't set details_wanted and error_data is not to be filled,
			 * just resetting error_occurred is enough.
			 *
			 * 发生软错误，跳过此元组并让 ErrorSaveContext 为下一个 NextCopyFrom 做好准备。由于我们没有设置details_wanted并且error_data也不需要填写，所以只需重置error_occurred就足够了。
			 */
			cstate->escontext->error_occurred = false;

			/* Repeat NextCopyFrom() until no soft error occurs
			 *
			 * 重复NextCopyFrom()，直到没有发生软错误
			 */
			continue;
		}

		/*
		 * The first targrows sample rows are simply copied into the
		 * reservoir.  Then we start replacing tuples in the sample until we
		 * reach the end of the relation. This algorithm is from Jeff Vitter's
		 * paper (see more info in commands/analyze.c).
		 *
		 * 第一个 targrows 样本行只需复制到水库中即可。  然后我们开始替换样本中的元组，直到到达关系的末尾。该算法来自 Jeff Vitter 的论文（更多信息请参阅commands/analyze.c）。
		 */
		if (numrows < targrows)
		{
			rows[numrows++] = heap_form_tuple(tupDesc, values, nulls);
		}
		else
		{
			/*
			 * t in Vitter's paper is the number of records already processed.
			 * If we need to compute a new S value, we must use the
			 * not-yet-incremented value of totalrows as t.
			 *
			 * Vitter 论文中的 t 是已处理的记录数。如果我们需要计算一个新的S值，我们必须使用totalrows尚未增加的值作为t。
			 */
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rstate, *totalrows, targrows);

			if (rowstoskip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old tuple
				 * at random
				 *
				 * 找到一个合适的元组，所以保存它，随机替换一个旧元组
				 */
				int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = heap_form_tuple(tupDesc, values, nulls);
			}

			rowstoskip -= 1;
		}

		*totalrows += 1;
	}

	/* Remove error callback.
	 *
	 * 删除错误回调。
	 */
	error_context_stack = errcallback.previous;

	/* Clean up.
	 *
	 * 清理。
	 */
	MemoryContextDelete(tupcontext);

	if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
		cstate->num_errors > 0 &&
		cstate->opts.log_verbosity >= COPY_LOG_VERBOSITY_DEFAULT)
		ereport(NOTICE,
				errmsg_plural("%" PRIu64 " row was skipped due to data type incompatibility",
							  "%" PRIu64 " rows were skipped due to data type incompatibility",
							  cstate->num_errors,
							  cstate->num_errors));

	EndCopyFrom(cstate);

	pfree(values);
	pfree(nulls);

	/*
	 * Emit some interesting relation info
	 *
	 * 发出一些有趣的关系信息
	 */
	ereport(elevel,
			(errmsg("\"%s\": file contains %.0f rows; "
					"%d rows in sample",
					RelationGetRelationName(onerel),
					*totalrows, numrows)));

	return numrows;
}
