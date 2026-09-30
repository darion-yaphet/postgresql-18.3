%top{
/*-------------------------------------------------------------------------
 *
 * repl_scanner.l
 *	  a lexical scanner for the replication commands
 *
 * 复制命令的词法扫描器。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/repl_scanner.l
 *
 *-------------------------------------------------------------------------
 */

/*
 * 核心流程：识别复制协议命令的关键字、标识符、字符串和 LSN，交给 repl_gram.y。
 */

#include "postgres.h"

#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "parser/scansup.h"

/*
 * NB: include repl_gram.h only AFTER including walsender_private.h, because
 * walsender_private includes headers that define XLogRecPtr.
 *
 * 注意：必须先包含 walsender_private.h 再包含 repl_gram.h，因为前者的头文件定义了 XLogRecPtr。
 */
#include "replication/walsender_private.h"
#include "repl_gram.h"
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

struct replication_yy_extra_type
{
	/* Pushed-back token (we only handle one)
	 *
	 * 压回的记号，这里只保留一个。
	 */
	int			repl_pushed_back_token;

	/* Work area for collecting literals
	 *
	 * 收集字面量的工作区。
	 */
	StringInfoData litbuf;
};

static void startlit(yyscan_t yyscanner);
static char *litbufdup(yyscan_t yyscanner);
static void addlit(char *ytext, int yleng, yyscan_t yyscanner);
static void addlitchar(unsigned char ychar, yyscan_t yyscanner);

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
%option prefix="replication_yy"
%option extra-type="struct replication_yy_extra_type *"

/*
 * Exclusive states:
 *  <xd> delimited identifiers (double-quoted identifiers)
 *  <xq> standard single-quoted strings
 *
 * 排他状态：xd 为双引号界定的标识符，xq 为标准单引号字符串。
 */
%x xd
%x xq

space			[ \t\n\r\f\v]

quote			'
quotestop		{quote}

/* Extended quote
 * xqdouble implements embedded quote, ''''
 *
 * 扩展引号：xqdouble 表示嵌入的引号，即连续两个单引号。
 */
xqstart			{quote}
xqdouble		{quote}{quote}
xqinside		[^']+

/* Double quote
 * Allows embedded spaces and other special characters into identifiers.
 *
 * 双引号：允许标识符中出现空格和其他特殊字符。
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

digit			[0-9]
hexdigit		[0-9A-Fa-f]

ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

%%

%{
	/* This code is inserted at the start of replication_yylex()
	 *
	 * 这段代码会插入到 replication_yylex() 的开头。
	 */

	/* If we have a pushed-back token, return that.
	 *
	 * 若有压回的记号，先返回它。
	 */
	if (yyextra->repl_pushed_back_token)
	{
		int			result = yyextra->repl_pushed_back_token;

		yyextra->repl_pushed_back_token = 0;
		return result;
	}
%}

BASE_BACKUP			{ return K_BASE_BACKUP; }
IDENTIFY_SYSTEM		{ return K_IDENTIFY_SYSTEM; }
READ_REPLICATION_SLOT	{ return K_READ_REPLICATION_SLOT; }
SHOW		{ return K_SHOW; }
TIMELINE			{ return K_TIMELINE; }
START_REPLICATION	{ return K_START_REPLICATION; }
CREATE_REPLICATION_SLOT		{ return K_CREATE_REPLICATION_SLOT; }
DROP_REPLICATION_SLOT		{ return K_DROP_REPLICATION_SLOT; }
ALTER_REPLICATION_SLOT		{ return K_ALTER_REPLICATION_SLOT; }
TIMELINE_HISTORY	{ return K_TIMELINE_HISTORY; }
PHYSICAL			{ return K_PHYSICAL; }
RESERVE_WAL			{ return K_RESERVE_WAL; }
LOGICAL				{ return K_LOGICAL; }
SLOT				{ return K_SLOT; }
TEMPORARY			{ return K_TEMPORARY; }
TWO_PHASE			{ return K_TWO_PHASE; }
EXPORT_SNAPSHOT		{ return K_EXPORT_SNAPSHOT; }
NOEXPORT_SNAPSHOT	{ return K_NOEXPORT_SNAPSHOT; }
USE_SNAPSHOT		{ return K_USE_SNAPSHOT; }
WAIT				{ return K_WAIT; }
UPLOAD_MANIFEST		{ return K_UPLOAD_MANIFEST; }

{space}+		{ /* do nothing
				   *
				   * 什么也不做。
				   */ }

{digit}+		{
					yylval->uintval = strtoul(yytext, NULL, 10);
					return UCONST;
				}

{hexdigit}+\/{hexdigit}+		{
					uint32	hi,
							lo;
					if (sscanf(yytext, "%X/%X", &hi, &lo) != 2)
						replication_yyerror(NULL, yyscanner, "invalid streaming start location");
					yylval->recptr = ((uint64) hi) << 32 | lo;
					return RECPTR;
				}

{xqstart}		{
					BEGIN(xq);
					startlit(yyscanner);
				}

<xq>{quotestop}	{
					yyless(1);
					BEGIN(INITIAL);
					yylval->str = litbufdup(yyscanner);
					return SCONST;
				}

<xq>{xqdouble}	{
					addlitchar('\'', yyscanner);
				}

<xq>{xqinside}  {
					addlit(yytext, yyleng, yyscanner);
				}

{xdstart}		{
					BEGIN(xd);
					startlit(yyscanner);
				}

<xd>{xdstop}	{
					int			len;

					yyless(1);
					BEGIN(INITIAL);
					yylval->str = litbufdup(yyscanner);
					len = strlen(yylval->str);
					truncate_identifier(yylval->str, len, true);
					return IDENT;
				}

<xd>{xdinside}  {
					addlit(yytext, yyleng, yyscanner);
				}

{identifier}	{
					int			len = strlen(yytext);

					yylval->str = downcase_truncate_identifier(yytext, len, true);
					return IDENT;
				}

.				{
					/* Any char not recognized above is returned as itself
					 *
					 * 上面没有识别的字符按自身返回。
					 */
					return yytext[0];
				}

<xq,xd><<EOF>>	{ replication_yyerror(NULL, yyscanner, "unterminated quoted string"); }


<<EOF>>			{
					yyterminate();
				}

%%

/* LCOV_EXCL_STOP */

/* see scan.l
 *
 * 见 scan.l。
 */
#undef yyextra
#define yyextra (((struct yyguts_t *) yyscanner)->yyextra_r)

/*
 * 开始收集一个字面量。
 */
static void
startlit(yyscan_t yyscanner)
{
	initStringInfo(&yyextra->litbuf);
}

/*
 * 返回已经收集好的字面量缓冲区。
 */
static char *
litbufdup(yyscan_t yyscanner)
{
	return yyextra->litbuf.data;
}

/*
 * 把一段文本追加到字面量缓冲区。
 */
static void
addlit(char *ytext, int yleng, yyscan_t yyscanner)
{
	appendBinaryStringInfo(&yyextra->litbuf, ytext, yleng);
}

/*
 * 把一个字符追加到字面量缓冲区。
 */
static void
addlitchar(unsigned char ychar, yyscan_t yyscanner)
{
	appendStringInfoChar(&yyextra->litbuf, ychar);
}

/*
  * (The first argument is enforced by Bison to match the first argument of
  * yyparse(), but it is not used here.)
  *
  * 第一个参数由 Bison 强制与 yyparse() 的第一个参数一致，此处并未使用。
  */
void
replication_yyerror(Node **replication_parse_result_p, yyscan_t yyscanner, const char *message)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg_internal("%s", message)));
}

/*
 * 初始化复制命令扫描器，并装入待解析字符串。
 */
void
replication_scanner_init(const char *str, yyscan_t *yyscannerp)
{
	yyscan_t	yyscanner;
	struct replication_yy_extra_type *yyext = palloc0_object(struct replication_yy_extra_type);

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	yyset_extra(yyext, yyscanner);

	yy_scan_string(str, yyscanner);
}

/*
 * 释放复制命令扫描器的额外状态并销毁 yyscanner。
 */
void
replication_scanner_finish(yyscan_t yyscanner)
{
	pfree(yyextra);
	yylex_destroy(yyscanner);
}

/*
 * Check to see if the first token of a command is a WalSender keyword.
 *
 * 检查命令的第一个记号是不是 WalSender 关键字。
 *
 * To keep repl_scanner.l minimal, we don't ask it to know every construct
 * that the core lexer knows.  Therefore, we daren't lex more than the
 * first token of a general SQL command.  That will usually look like an
 * IDENT token here, although some other cases are possible.
 *
 * 为了让 repl_scanner.l 保持精简，不要求它认识核心词法器的全部构造。
 * 因此对普通 SQL 命令最多只扫描第一个记号。它在这里通常像 IDENT，但也可能是别的情况。
 */
bool
replication_scanner_is_replication_command(yyscan_t yyscanner)
{
	YYSTYPE		dummy;
	int			first_token = replication_yylex(&dummy, yyscanner);

	switch (first_token)
	{
		case K_IDENTIFY_SYSTEM:
		case K_BASE_BACKUP:
		case K_START_REPLICATION:
		case K_CREATE_REPLICATION_SLOT:
		case K_DROP_REPLICATION_SLOT:
		case K_ALTER_REPLICATION_SLOT:
		case K_READ_REPLICATION_SLOT:
		case K_TIMELINE_HISTORY:
		case K_UPLOAD_MANIFEST:
		case K_SHOW:
			/* Yes; push back the first token so we can parse later.
			 *
			 * 是复制命令；把第一个记号压回去，稍后再解析。
			 */
			yyextra->repl_pushed_back_token = first_token;
			return true;
		default:
			/* Nope; we don't bother to push back the token.
			 *
			 * 不是；不必把记号压回去。
			 */
			return false;
	}
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
