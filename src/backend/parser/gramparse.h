/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *		Shared definitions for the "raw" parser (flex and bison phases only)
 *		“原始”解析器的共享定义（仅限 flex 和 bison 阶段）
 *
 * NOTE: this file is only meant to be included in the core parsing files,
 * i.e., parser.c, gram.y, and scan.l.
 *
 * 注意：此文件仅用于包含在核心解析文件中，即 parser.c、gram.y 和 scan.l。
 * Definitions that are needed outside the core parser should be in parser.h.
 * 核心解析器之外需要的定义应该放在 parser.h 中。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/parser/gramparse.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H

#include "nodes/parsenodes.h"
#include "parser/scanner.h"

/*
 * NB: include gram.h only AFTER including scanner.h, because scanner.h
 * is what #defines YYLTYPE.
 * 注意：仅在包含 scanner.h 之后才包含 gram.h，因为 scanner.h 定义了 YYLTYPE。
 */
#include "gram.h"

/*
 * The YY_EXTRA data that a flex scanner allows us to pass around.  Private
 * state needed for raw parsing/lexing goes here.
 * flex 扫描器允许我们传递的 YY_EXTRA 数据。原始解析/词法分析所需的私有状态放在这里。
 */
typedef struct base_yy_extra_type
{
	/*
	 * Fields used by the core scanner.
	 * 核心扫描器使用的字段。
	 */
	core_yy_extra_type core_yy_extra;

	/*
	 * State variables for base_yylex().
	 * base_yylex() 的状态变量。
	 */
	bool		have_lookahead; /* is lookahead info valid?
								 * 前瞻信息是否有效？ */
	int			lookahead_token;	/* one-token lookahead
									 * 单个标记前瞻 */
	core_YYSTYPE lookahead_yylval;	/* yylval for lookahead token
									 * 前瞻标记的 yylval */
	YYLTYPE		lookahead_yylloc;	/* yylloc for lookahead token
									 * 前瞻标记的 yylloc */
	char	   *lookahead_end;	/* end of current token
								 * 当前标记的结束位置 */
	char		lookahead_hold_char;	/* to be put back at *lookahead_end
										 * 将放回 *lookahead_end 的字符 */

	/*
	 * State variables that belong to the grammar.
	 * 属于语法的状态变量。
	 */
	List	   *parsetree;		/* final parse result is delivered here
								 * 最终解析结果交付于此 */
} base_yy_extra_type;

/*
 * In principle we should use yyget_extra() to fetch the yyextra field
 * from a yyscanner struct.  However, flex always puts that field first,
 * and this is sufficiently performance-critical to make it seem worth
 * cheating a bit to use an inline macro.
 *
 * 原则上我们应该使用 yyget_extra() 从 yyscanner 结构中获取 yyextra 字段。
 * 然而，flex 总是把那个字段放在第一位，而且这在性能上非常关键，值得稍微“作弊”一下使用内联宏。
 */
#define pg_yyget_extra(yyscanner) (*((base_yy_extra_type **) (yyscanner)))


/* from parser.c
 * 来自 parser.c */
extern int	base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp,
					   core_yyscan_t yyscanner);

/* from gram.y
 * 来自 gram.y */
extern void parser_init(base_yy_extra_type *yyext);
extern int	base_yyparse(core_yyscan_t yyscanner);

#endif							/* GRAMPARSE_H */
