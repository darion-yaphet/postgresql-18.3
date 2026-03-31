/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *	  POSTGRES copy 命令的使用定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/copy.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"

/*
 * Core flow of COPY command:
 * 1. DoCopy: The high-level entry point that handles both COPY FROM and COPY TO.
 * 2. ProcessCopyOptions: Parses the options (FORMAT, DELIMITER, NULL, etc.) into CopyFormatOptions.
 * 3. For COPY FROM:
 *    - BeginCopyFrom: Sets up the state, opens the source (file, program, or stdin).
 *    - CopyFrom: The main loop that reads rows and inserts them into the table.
 *    - EndCopyFrom: Finalizes the operation and shuts down the state.
 * 4. For COPY TO:
 *    - BeginCopyTo: Sets up the state, opens the destination (file, program, or stdout).
 *    - DoCopyTo: The main process that reads data from the table/query and sends it out.
 *    - EndCopyTo: Finalizes the operation and shuts down the state.
 *
 * COPY 命令的核心流程：
 * 1. DoCopy: 处理 COPY FROM 和 COPY TO 的高级入口点。
 * 2. ProcessCopyOptions: 将选项（FORMAT, DELIMITER, NULL 等）解析为 CopyFormatOptions结构体。
 * 3. 对于 COPY FROM:
 *    - BeginCopyFrom: 设置状态，打开数据源（文件、程序或标准输入）。
 *    - CopyFrom: 读取行并将其插入表中的主循环。
 *    - EndCopyFrom: 完成操作并关闭状态。
 * 4. 对于 COPY TO:
 *    - BeginCopyTo: 设置状态，打开目标（文件、程序或标准输出）。
 *    - DoCopyTo: 从表/查询中读取数据并将其输出的主进程。
 *    - EndCopyTo: 完成操作并关闭状态。
 */

/*
 * Represents whether a header line should be present, and whether it must
 * match the actual names (which implies "true").
 * 表示是否应该存在标题行，以及它是否必须与实际名称匹配（这意味着 "true"）。
 */
typedef enum CopyHeaderChoice
{
	COPY_HEADER_FALSE = 0,
	COPY_HEADER_TRUE,
	COPY_HEADER_MATCH,
} CopyHeaderChoice;

/*
 * Represents where to save input processing errors.  More values to be added
 * in the future.
 * 表示输入处理错误保存的位置。将来会添加更多值。
 */
typedef enum CopyOnErrorChoice
{
	COPY_ON_ERROR_STOP = 0,		/* immediately throw errors, default */
								/* 立即抛出错误，默认值 */
	COPY_ON_ERROR_IGNORE,		/* ignore errors */
								/* 忽略错误 */
} CopyOnErrorChoice;

/*
 * Represents verbosity of logged messages by COPY command.
 * 表示 COPY 命令记录的消息的详细程度。
 */
typedef enum CopyLogVerbosityChoice
{
	COPY_LOG_VERBOSITY_SILENT = -1, /* logs none */
									/* 不记录任何日志 */
	COPY_LOG_VERBOSITY_DEFAULT = 0, /* logs no additional messages. As this is
									 * the default, assign 0 */
									/* 不记录额外消息。由于这是默认值，因此分配 0 */
	COPY_LOG_VERBOSITY_VERBOSE, /* logs additional messages */
								/* 记录额外消息 */
} CopyLogVerbosityChoice;

/*
 * A struct to hold COPY options, in a parsed form. All of these are related
 * to formatting, except for 'freeze', which doesn't really belong here, but
 * it's expedient to parse it along with all the other options.
 * 一个用于保存解析后的 COPY 选项的结构体。除了 'freeze' 之外，所有这些选项都与格式化有关。
 * 'freeze' 并不真正属于这里，但将其与其他所有选项一起解析很方便。
 */
typedef struct CopyFormatOptions
{
	/* parameters from the COPY command */
	/* 来自 COPY 命令的参数 */
	int			file_encoding;	/* file or remote side's character encoding,
								 * -1 if not specified */
								/* 文件或远端的字符编码，如果未指定则为 -1 */
	bool		binary;			/* binary format? */
								/* 是否为二进制格式？ */
	bool		freeze;			/* freeze rows on loading? */
								/* 加载时是否冻结行？ */
	bool		csv_mode;		/* Comma Separated Value format? */
								/* 是否为逗号分隔值 (CSV) 格式？ */
	CopyHeaderChoice header_line;	/* header line? */
									/* 是否有标题行？ */
	char	   *null_print;		/* NULL marker string (server encoding!) */
									/* NULL 标记字符串（服务器编码！） */
	int			null_print_len; /* length of same */
									/* 同上字符串的长度 */
	char	   *null_print_client;	/* same converted to file encoding */
									/* 转换为文件编码的同一字符串 */
	char	   *default_print;	/* DEFAULT marker string */
									/* DEFAULT 标记字符串 */
	int			default_print_len;	/* length of same */
									/* 同上字符串的长度 */
	char	   *delim;			/* column delimiter (must be 1 byte) */
									/* 列分隔符（必须是 1 字节） */
	char	   *quote;			/* CSV quote char (must be 1 byte) */
									/* CSV 引用字符（必须是 1 字节） */
	char	   *escape;			/* CSV escape char (must be 1 byte) */
									/* CSV 转义字符（必须是 1 字节） */
	List	   *force_quote;	/* list of column names */
								/* 列名列表 */
	bool		force_quote_all;	/* FORCE_QUOTE *? */
									/* 是否强制引用所有列？ */
	bool	   *force_quote_flags;	/* per-column CSV FQ flags */
									/* 每列的 CSV 强制引用标志 */
	List	   *force_notnull;	/* list of column names */
								/* 列名列表 */
	bool		force_notnull_all;	/* FORCE_NOT_NULL *? */
									/* 是否强制所有列非空？ */
	bool	   *force_notnull_flags;	/* per-column CSV FNN flags */
									/* 每列的 CSV 强制非空标志 */
	List	   *force_null;		/* list of column names */
								/* 列名列表 */
	bool		force_null_all; /* FORCE_NULL *? */
								/* 是否强制所有列为 NULL？ */
	bool	   *force_null_flags;	/* per-column CSV FN flags */
									/* 每列的 CSV 强制 NULL 标志 */
	bool		convert_selectively;	/* do selective binary conversion? */
										/* 是否进行选择性二进制转换？ */
	CopyOnErrorChoice on_error; /* what to do when error happened */
								/* 发生错误时该怎么办 */
	CopyLogVerbosityChoice log_verbosity;	/* verbosity of logged messages */
											/* 记录消息的详细程度 */
	int64		reject_limit;	/* maximum tolerable number of errors */
								/* 可容忍的最大错误数 */
	List	   *convert_select; /* list of column names (can be NIL) */
								/* 列名列表（可以为 NIL） */
} CopyFormatOptions;

/* These are private in commands/copy[from|to].c */
/* 这些在 commands/copy[from|to].c 中是私有的 */
typedef struct CopyFromStateData *CopyFromState;
typedef struct CopyToStateData *CopyToState;

/* Callback functions for COPY FROM and COPY TO */
/* COPY FROM 和 COPY TO 的回调函数 */
typedef int (*copy_data_source_cb) (void *outbuf, int minread, int maxread);
typedef void (*copy_data_dest_cb) (void *data, int len);

/*
 * DoCopy: Main entry point for the COPY command.
 * 处理 COPY 命令的主入口点。
 */
extern void DoCopy(ParseState *pstate, const CopyStmt *stmt,
				   int stmt_location, int stmt_len,
				   uint64 *processed);

/*
 * ProcessCopyOptions: Parse options from a COPY command into CopyFormatOptions.
 * 将 COPY 命令中的选项解析为 CopyFormatOptions 结构体。
 */
extern void ProcessCopyOptions(ParseState *pstate, CopyFormatOptions *opts_out, bool is_from, List *options);

/*
 * BeginCopyFrom: Initialize state for a COPY FROM operation.
 * 初始化 COPY FROM 操作的状态。
 */
extern CopyFromState BeginCopyFrom(ParseState *pstate, Relation rel, Node *whereClause,
								   const char *filename,
								   bool is_program, copy_data_source_cb data_source_cb, List *attnamelist, List *options);

/*
 * EndCopyFrom: Clean up state after a COPY FROM operation.
 * 清理 COPY FROM 操作后的状态。
 */
extern void EndCopyFrom(CopyFromState cstate);

/*
 * NextCopyFrom: Fetch next row during COPY FROM. Returns true if row found.
 * 在 COPY FROM 期间获取下一行。如果找到行，则返回 true。
 */
extern bool NextCopyFrom(CopyFromState cstate, ExprContext *econtext,
						 Datum *values, bool *nulls);

/*
 * NextCopyFromRawFields: Fetch next row's raw fields during COPY FROM.
 * 在 COPY FROM 期间获取下一行的原始字段。
 */
extern bool NextCopyFromRawFields(CopyFromState cstate,
								  char ***fields, int *nfields);

/*
 * CopyFromErrorCallback: Error callback function for COPY FROM.
 * COPY FROM 的错误回调函数。
 */
extern void CopyFromErrorCallback(void *arg);

/*
 * CopyLimitPrintoutLength: Limit the printed length of the input string in error messages.
 * 限制错误消息中输入字符串的打印长度。
 */
extern char *CopyLimitPrintoutLength(const char *str);

/*
 * CopyFrom: Main loop for COPY FROM, processes all rows.
 * COPY FROM 的主循环，处理所有行。
 */
extern uint64 CopyFrom(CopyFromState cstate);

/*
 * CreateCopyDestReceiver: Create a DestReceiver for handling COPY query results.
 * 创建一个用于处理 COPY 查询结果的 DestReceiver。
 */
extern DestReceiver *CreateCopyDestReceiver(void);

/*
 * internal prototypes
 * 内部原型
 */

/*
 * BeginCopyTo: Initialize state for a COPY TO operation.
 * 初始化 COPY TO 操作的状态。
 */
extern CopyToState BeginCopyTo(ParseState *pstate, Relation rel, RawStmt *raw_query,
							   Oid queryRelId, const char *filename, bool is_program,
							   copy_data_dest_cb data_dest_cb, List *attnamelist, List *options);

/*
 * EndCopyTo: Clean up state after a COPY TO operation.
 * 清理 COPY TO 操作后的状态。
 */
extern void EndCopyTo(CopyToState cstate);

/*
 * DoCopyTo: Execute a COPY TO operation.
 * 执行 COPY TO 操作。
 */
extern uint64 DoCopyTo(CopyToState cstate);

/*
 * CopyGetAttnums: Get attribute numbers for the columns to be copied.
 * 获取要执行 COPY 的列的属性编号。
 */
extern List *CopyGetAttnums(TupleDesc tupDesc, Relation rel,
							List *attnamelist);

#endif							/* COPY_H */
