%top{
/*-------------------------------------------------------------------------
 *
 * syncrep_scanner.l
 *	  a lexical scanner for synchronous_standby_names
 *
 * synchronous_standby_names 的词法扫描器。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep_scanner.l
 *
 *-------------------------------------------------------------------------
 */

/*
 * 核心流程：把 synchronous_standby_names 切成 NAME、NUM、ANY、FIRST 以及括号和逗号，交给 syncrep_gram.y。
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/pg_list.h"

/*
 * NB: include syncrep_gram.h only AFTER including syncrep.h, because syncrep.h
 * includes node definitions needed for YYSTYPE.
 *
 * 注意：必须先包含 syncrep.h 再包含 syncrep_gram.h，因为 syncrep.h 提供 YYSTYPE 所需的节点定义。
 */
#include "replication/syncrep.h"
#include "syncrep_gram.h"
}

%{
/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error)
 *
 * 致命扫描错误时不要调用 exit()。做法略笨，见 yy_fatal_error。
 */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

/*
 * 把 flex 的致命错误转成 ereport(ERROR)，避免进程直接 exit()。
 */
static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

struct syncrep_yy_extra_type
{
	StringInfoData xdbuf;
};

/*
 * Better keep this definition here than put it in replication/syncrep.h and
 * save a bit of duplication.  Putting it in replication/syncrep.h would leak
 * the definition to other parts and possibly affect other scanners.
 *
 * 这个定义留在此处，比放进 replication/syncrep.h 更合适，也能少一点重复。
 * 放进 replication/syncrep.h 会把定义泄漏到其他地方，并可能影响别的扫描器。
*/
#define YY_DECL extern int	syncrep_yylex(union YYSTYPE *yylval_param, char **syncrep_parse_error_msg_p, yyscan_t yyscanner)

/* LCOV_EXCL_START */

%}

%option reentrant
%option bison-bridge
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyyalloc
%option noyyrealloc
%option noyyfree
%option warn
%option prefix="syncrep_yy"
%option extra-type="struct syncrep_yy_extra_type *"

/*
 * <xd> delimited identifiers (double-quoted identifiers)
 *
 * xd 状态：双引号界定的标识符。
 */
%x xd

space			[ \t\n\r\f\v]

digit			[0-9]
ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]
identifier		{ident_start}{ident_cont}*

dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

%%
{space}+	{ /* ignore
			   *
			   * 忽略。
			   */ }

	/* brute-force case insensitivity is safer than relying on flex -i
	 *
	 * 用穷举大小写比依赖 flex -i 更稳妥。
	 */

[Aa][Nn][Yy]			{ return ANY; }
[Ff][Ii][Rr][Ss][Tt]	{ return FIRST; }

{xdstart}	{
				initStringInfo(&yyextra->xdbuf);
				BEGIN(xd);
		}
<xd>{xddouble} {
				appendStringInfoChar(&yyextra->xdbuf, '"');
		}
<xd>{xdinside} {
				appendStringInfoString(&yyextra->xdbuf, yytext);
		}
<xd>{xdstop} {
				yylval->str = yyextra->xdbuf.data;
				yyextra->xdbuf.data = NULL;
				BEGIN(INITIAL);
				return NAME;
		}
<xd><<EOF>> {
				syncrep_yyerror(NULL, syncrep_parse_error_msg_p, yyscanner, "unterminated quoted identifier");
				return JUNK;
		}

{identifier} {
				yylval->str = pstrdup(yytext);
				return NAME;
		}

{digit}+	{
				yylval->str = pstrdup(yytext);
				return NUM;
		}

"*"		{
				yylval->str = "*";
				return NAME;
		}

","			{ return ','; }
"("			{ return '('; }
")"			{ return ')'; }

.			{ return JUNK; }
%%

/* LCOV_EXCL_STOP */

/* see scan.l
 *
 * 见 scan.l。
 */
#undef yyextra
#define yyextra (((struct yyguts_t *) yyscanner)->yyextra_r)

/*
 * This yyerror() function does not raise an error (elog or similar), it just
 * collects the error message in *syncrep_parse_error_msg_p and leaves it to
 * the ultimate caller of the syncrep parser to raise the error.  (The
 * ultimate caller will do that with special GUC error functions.)
 *
 * 这个 yyerror() 并不 elog 或类似地抛错，只把错误信息收集到 syncrep_parse_error_msg_p，
 * 留给 syncrep 语法的最终调用者去报错。最终调用者会用专用的 GUC 错误函数来做这件事。
 *
 * (The first argument is enforced by Bison to match the first argument of
 * yyparse(), but it is not used here.)
 *
 * 第一个参数由 Bison 强制与 yyparse() 的第一个参数一致，此处并未使用。
 */
void
syncrep_yyerror(SyncRepConfigData **syncrep_parse_result_p, char **syncrep_parse_error_msg_p, yyscan_t yyscanner, const char *message)
{
	struct yyguts_t *yyg = (struct yyguts_t *) yyscanner;	/* needed for yytext
															 * macro
															 *
															 * yytext 宏需要这个变量。
															 */

	/* report only the first error in a parse operation
	 *
	 * 一次解析只报告第一个错误。
	 */
	if (*syncrep_parse_error_msg_p)
		return;
	if (yytext[0])
		*syncrep_parse_error_msg_p = psprintf("%s at or near \"%s\"",
											  message, yytext);
	else
		*syncrep_parse_error_msg_p = psprintf("%s at end of input",
											  message);
}

/*
 * 初始化 synchronous_standby_names 扫描器，并装入待解析字符串。
 */
void
syncrep_scanner_init(const char *str, yyscan_t *yyscannerp)
{
	yyscan_t	yyscanner;
	struct syncrep_yy_extra_type *yyext = palloc0_object(struct syncrep_yy_extra_type);

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	yyset_extra(yyext, yyscanner);

	yy_scan_string(str, yyscanner);
}

/*
 * 释放扫描器的额外状态并销毁 yyscanner。
 */
void
syncrep_scanner_finish(yyscan_t yyscanner)
{
	pfree(yyextra);
	yylex_destroy(yyscanner);
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 *
 * 让 flex 使用 palloc() 而不是 malloc() 的接口函数。
 * 做成 static 更好，但 flex 不允许。
 */

void *
yyalloc(yy_size_t size, yyscan_t yyscanner)
{
	return palloc(size);
}

/*
 * 供 flex 调用的 realloc 包装：已有指针则 repalloc，否则 palloc。
 */
void *
yyrealloc(void *ptr, yy_size_t size, yyscan_t yyscanner)
{
	if (ptr)
		return repalloc(ptr, size);
	else
		return palloc(size);
}

/*
 * 供 flex 调用的 free 包装：指针非空时 pfree。
 */
void
yyfree(void *ptr, yyscan_t yyscanner)
{
	if (ptr)
		pfree(ptr);
}
