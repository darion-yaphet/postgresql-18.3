%{
/*-------------------------------------------------------------------------
 *
 * syncrep_gram.y				- Parser for synchronous_standby_names
 *
 * syncrep_gram.y：解析 synchronous_standby_names 的语法。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep_gram.y
 *
 *-------------------------------------------------------------------------
 */

/*
 * 核心流程：把 synchronous_standby_names 归约为备库名单和 ANY 或 FIRST 数量，再压成 SyncRepConfigData。
 */

#include "postgres.h"

#include "nodes/pg_list.h"
#include "replication/syncrep.h"

#include "syncrep_gram.h"

static SyncRepConfigData *create_syncrep_config(const char *num_sync,
					List *members, uint8 syncrep_method);

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.
 *
 * Bison 不会分配需要跨多次解析调用存活的内存，因此这里用 palloc 代替 malloc。
 * 解析中途报错时，可以避免内存泄漏。
 */
#define YYMALLOC palloc
#define YYFREE   pfree

%}

%parse-param {SyncRepConfigData **syncrep_parse_result_p}
%parse-param {char **syncrep_parse_error_msg_p}
%parse-param {yyscan_t yyscanner}
%lex-param   {char **syncrep_parse_error_msg_p}
%lex-param   {yyscan_t yyscanner}
%pure-parser
%expect 0
%name-prefix="syncrep_yy"

%union
{
	char	   *str;
	List	   *list;
	SyncRepConfigData *config;
}

%token <str> NAME NUM JUNK ANY FIRST

%type <config> result standby_config
%type <list> standby_list
%type <str> standby_name

%start result

%%
result:
		standby_config				{
										*syncrep_parse_result_p = $1;
										(void) yynerrs; /* suppress compiler warning
														 *
														 * 抑制编译器警告。
														 */
									}
	;

standby_config:
		standby_list				{ $$ = create_syncrep_config("1", $1, SYNC_REP_PRIORITY); }
		| NUM '(' standby_list ')'		{ $$ = create_syncrep_config($1, $3, SYNC_REP_PRIORITY); }
		| ANY NUM '(' standby_list ')'		{ $$ = create_syncrep_config($2, $4, SYNC_REP_QUORUM); }
		| FIRST NUM '(' standby_list ')'		{ $$ = create_syncrep_config($2, $4, SYNC_REP_PRIORITY); }
	;

standby_list:
		standby_name						{ $$ = list_make1($1); }
		| standby_list ',' standby_name		{ $$ = lappend($1, $3); }
	;

standby_name:
		NAME						{ $$ = $1; }
		| NUM						{ $$ = $1; }
	;
%%

/*
 * 把解析出的备库名单压成扁平的 SyncRepConfigData。
 */
static SyncRepConfigData *
create_syncrep_config(const char *num_sync, List *members, uint8 syncrep_method)
{
	SyncRepConfigData *config;
	int			size;
	ListCell   *lc;
	char	   *ptr;

	/* Compute space needed for flat representation
	 *
	 * 计算扁平表示所需的空间。
	 */
	size = offsetof(SyncRepConfigData, member_names);
	foreach(lc, members)
	{
		char	   *standby_name = (char *) lfirst(lc);

		size += strlen(standby_name) + 1;
	}

	/* And transform the data into flat representation
	 *
	 * 再把数据转成扁平表示。
	 */
	config = (SyncRepConfigData *) palloc(size);

	config->config_size = size;
	config->num_sync = atoi(num_sync);
	config->syncrep_method = syncrep_method;
	config->nmembers = list_length(members);
	ptr = config->member_names;
	foreach(lc, members)
	{
		char	   *standby_name = (char *) lfirst(lc);

		strcpy(ptr, standby_name);
		ptr += strlen(standby_name) + 1;
	}

	return config;
}
