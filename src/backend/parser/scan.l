%top{
/*-------------------------------------------------------------------------
 *
 * scan.l
 *	  lexical scanner for PostgreSQL
 *	  PostgreSQL 的词法扫描器
 *
 * NOTE NOTE NOTE:
 * 注意 注意 注意：
 *
 * The rules in this file must be kept in sync with src/fe_utils/psqlscan.l
 * and src/interfaces/ecpg/preproc/pgc.l!
 * 本文件中的规则必须与 src/fe_utils/psqlscan.l 和 src/interfaces/ecpg/preproc/pgc.l 保持同步！
 *
 * The rules are designed so that the scanner never has to backtrack,
 * in the sense that there is always a rule that can match the input
 * consumed so far (the rule action may internally throw back some input
 * with yyless(), however).  As explained in the flex manual, this makes
 * for a useful speed increase --- several percent faster when measuring
 * raw parsing (Flex + Bison).  The extra complexity is mostly in the rules
 * for handling float numbers and continued string literals.  If you change
 * the lexical rules, verify that you haven't broken the no-backtrack
 * property by running flex with the "-b" option and checking that the
 * resulting "lex.backup" file says that no backing up is needed.  (As of
 * Postgres 9.2, this check is made automatically by the Makefile.)
 * 这些规则设计为使扫描器永远不需要回溯，
 * 从某种意义上说，总会有一个规则能够匹配到目前为止所消耗的输入
 * （当然，规则动作内部可能会通过 yyless() 退回一些输入）。
 * 正如 flex 手册中所解释的那样，这可以带来非常有用的速度提升 —— 
 * 在测量原始解析（Flex + Bison）时速度会快几个百分点。
 * 额外的复杂性主要在于处理浮点数和连续字符串字面量的规则。
 * 如果您修改了词法规则，请通过使用 "-b" 选项运行 flex 并检查生成的
 * "lex.backup" 文件是否显示不需要备份，来验证您没有破坏无回溯属性。
 * （自 Postgres 9.2 起，此检查由 Makefile 自动进行。）
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 部分版权 (c) 1996-2025, PostgreSQL 全球开发组
 * 部分版权 (c) 1994, 加州大学董事会
 *
 * IDENTIFICATION
 *	  src/backend/parser/scan.l
 * 标识
 *	  src/backend/parser/scan.l
 *
 *-------------------------------------------------------------------------
 *
 * 核心流程解释：
 * 本文件是 PostgreSQL 的词法扫描器（Lexical Scanner）的 Flex 源代码文件（scan.l）。
 * 核心流程如下：
 * 
 * 1. 初始化阶段 (Initialization):
 *    - 当解析器开始解析一条 SQL 语句时，首先调用 `scanner_init()`。
 *    - 在 `scanner_init()` 中，通过 `yylex_init()` 初始化 Flex 扫描器，并分配和复制输入字符串缓冲区（scanbuf）及字面量临时缓冲区（literalbuf）。
 *    - 设置 GUC 词法相关参数（如 backslash_quote、standard_conforming_strings 等），并调用 `yy_scan_buffer()` 准备扫描。
 * 
 * 2. 词法扫描阶段 (Scanning):
 *    - Bison 解析器或外部调用者调用 `core_yylex()`（或 `yylex()` 宏定义）开始扫描 Token。
 *    - 扫描器在 `INITIAL` 状态下匹配关键字（由 `ScanKeywordLookup()` 在 `kwlist.h` 中检索）、标识符（identifier）、数值字面量（decinteger、real、numeric等）、特殊符号（self、operator等）。
 *    - 遇到带引号、转义或多行注释的复杂文本时，扫描器会通过 `BEGIN()` 宏切换到各种专用的排他性状态（Exclusive States），例如：
 *      * `<xc>`：C风格的嵌套注释，忽略内容并计数 `xcdepth` 深度，直到遇到 `*/`。
 *      * `<xq>`、`<xe>`：处理标准与转义的单引号字符串。
 *      * `<xdolq>`：处理美元占位符引号（$foo$...$foo$），支持保留原样不转义。
 *      * `<xd>`、`<xui>`：处理双引号界定的标识符和 Unicode 标识符。
 *    - 拼装字面量时使用 `startlit()`、`addlit()`、`addlitchar()` 动态追加缓冲区内容，并在结束时通过 `litbufdup()` 复制生成 Token 的值赋给 `yylval` 返回给 Bison 解析器。
 * 
 * 3. 清理阶段 (Cleanup):
 *    - 词法解析完成后，调用 `scanner_finish()` 释放缓冲区，由 Flex 完成内存清理。
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>

#include "common/string.h"
#include "gramparse.h"
#include "nodes/miscnodes.h"
#include "parser/parser.h"		/* only needed for GUC variables */
#include "parser/scansup.h"
#include "port/pg_bitutils.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
}

%{

/* LCOV_EXCL_START */

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error)
 * 避免在致命的扫描器错误上调用 exit()（有点丑陋 —— 见 yy_fatal_error） */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

/*
 * GUC variables.  This is a DIRECT violation of the warning given at the
 * head of gram.y, ie flex/bison code must not depend on any GUC variables;
 * as such, changing their values can induce very unintuitive behavior.
 * But we shall have to live with it until we can remove these variables.
 * GUC 变量。这直接违反了 gram.y 头部给出的警告，即 flex/bison 代码绝不能依赖任何 GUC 变量；
 * 因此，更改它们的值可能会导致非常不直观的行为。
 * 但在我们可以移除这些变量之前，我们不得不接受这一点。
 */
int			backslash_quote = BACKSLASH_QUOTE_SAFE_ENCODING;
bool		escape_string_warning = true;
bool		standard_conforming_strings = true;

/*
 * Constant data exported from this file.  This array maps from the
 * zero-based keyword numbers returned by ScanKeywordLookup to the
 * Bison token numbers needed by gram.y.  This is exported because
 * callers need to pass it to scanner_init, if they are using the
 * standard keyword list ScanKeywords.
 * 从此文件导出的常量数据。此数组将 ScanKeywordLookup 返回的从零开始的关键字编号，
 * 映射到 gram.y 所需的 Bison token 编号。导出此数组是因为，
 * 如果调用者使用标准关键字列表 ScanKeywords，则需要将其传递给 scanner_init。
 */
#define PG_KEYWORD(kwname, value, category, collabel) value,

const uint16 ScanKeywordTokens[] = {
#include "parser/kwlist.h"
};

#undef PG_KEYWORD

/*
 * Set the type of YYSTYPE.
 * 设置 YYSTYPE 的类型。
 */
#define YYSTYPE core_YYSTYPE

/*
 * Each call to yylex must set yylloc to the location of the found token
 * (expressed as a byte offset from the start of the input text).
 * When we parse a token that requires multiple lexer rules to process,
 * this should be done in the first such rule, else yylloc will point
 * into the middle of the token.
 * 每次对 yylex 的调用必须将 yylloc 设置为找到的 token 的位置
 * （表示为自输入文本开头的字节偏移量）。
 * 当我们解析一个需要多个词法器规则来处理的 token 时，
 * 这应该在第一个此类规则中完成，否则 yylloc 将指向 token 的中间。
 */
#define SET_YYLLOC()  (*(yylloc) = yytext - yyextra->scanbuf)

/*
 * Advance yylloc by the given number of bytes.
 * 将 yylloc 推进给定的字节数。
 */
#define ADVANCE_YYLLOC(delta)  ( *(yylloc) += (delta) )

/*
 * Sometimes, we do want yylloc to point into the middle of a token; this is
 * useful for instance to throw an error about an escape sequence within a
 * string literal.  But if we find no error there, we want to revert yylloc
 * to the token start, so that that's the location reported to the parser.
 * Use PUSH_YYLLOC/POP_YYLLOC to save/restore yylloc around such code.
 * (Currently the implied "stack" is just one location, but someday we might
 * need to nest these.)
 * 有时，我们确实希望 yylloc 指向 token 的中间；这很有用，
 * 例如可以针对字符串字面量中的转义序列抛出错误。
 * 但如果我们在此处未发现错误，我们希望将 yylloc 恢复为 token 开头，
 * 以便将其作为报告给解析器的位置。
 * 使用 PUSH_YYLLOC/POP_YYLLOC 在此类代码前后保存/恢复 yylloc。
 * （目前隐式的“栈”只有一个位置，但也许有一天我们需要嵌套它们。）
 */
#define PUSH_YYLLOC()	(yyextra->save_yylloc = *(yylloc))
#define POP_YYLLOC()	(*(yylloc) = yyextra->save_yylloc)

#define startlit()	( yyextra->literallen = 0 )
static void addlit(char *ytext, int yleng, core_yyscan_t yyscanner);
static void addlitchar(unsigned char ychar, core_yyscan_t yyscanner);
static char *litbufdup(core_yyscan_t yyscanner);
static unsigned char unescape_single_char(unsigned char c, core_yyscan_t yyscanner);
static int	process_integer_literal(const char *token, YYSTYPE *lval, int base);
static void addunicode(pg_wchar c, yyscan_t yyscanner);

#define yyerror(msg)  scanner_yyerror(msg, yyscanner)

#define lexer_errposition()  scanner_errposition(*(yylloc), yyscanner)

static void check_string_escape_warning(unsigned char ychar, core_yyscan_t yyscanner);
static void check_escape_warning(core_yyscan_t yyscanner);

%}

%option reentrant
%option bison-bridge
%option bison-locations
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
%option prefix="core_yy"
%option extra-type="core_yy_extra_type *"

/*
 * OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting state, to which all non-conditional rules apply.
 * Exclusive states change parsing rules while the state is active.  When in
 * an exclusive state, only those rules defined for that state apply.
 * 好了，这里是 lex/flex 规则行为的简短说明。
 * 总是选择与输入字符串匹配的最长模式。
 * 对于等长的模式，选择规则列表中最先出现的模式。
 * INITIAL 是起始状态，所有无条件规则均适用于该状态。
 * 排他性状态在激活时会改变解析规则。当处于排他性状态时，仅适用于为该状态定义的规则。
 *
 * We use exclusive states for quoted strings, extended comments,
 * and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xb> bit string literal
 *  <xc> extended C-style comments
 *  <xd> delimited identifiers (double-quoted identifiers)
 *  <xh> hexadecimal byte string
 *  <xq> standard quoted strings
 *  <xqs> quote stop (detect continued strings)
 *  <xe> extended quoted strings (support backslash escape sequences)
 *  <xdolq> $foo$ quoted strings
 *  <xui> quoted identifier with Unicode escapes
 *  <xus> quoted string with Unicode escapes
 *  <xeu> Unicode surrogate pair in extended quoted string
 * 我们对带引号的字符串、扩展注释使用排他性状态，
 * 并用以消除数值字符串的解析麻烦。
 * 排他性状态：
 *  <xb> 比特位字符串字面量
 *  <xc> 扩展 C 风格注释
 *  <xd> 界定标识符（双引号标识符）
 *  <xh> 十六进制字节字符串
 *  <xq> 标准带引号字符串
 *  <xqs> 引号终止（检测连续的字符串）
 *  <xe> 扩展的带引号字符串（支持反斜杠转义序列）
 *  <xdolq> $foo$ 带引号字符串
 *  <xui> 带有 Unicode 转义的带引号标识符
 *  <xus> 带有 Unicode 转义的带引号字符串
 *  <xeu> 扩展带引号字符串中的 Unicode 代理对
 *
 * Remember to add an <<EOF>> case whenever you add a new exclusive state!
 * The default one is probably not the right thing.
 * 无论何时添加新的排他性状态，请务必记住添加一个 <<EOF>> 情况！
 * 默认的情况可能并不是正确的选择。
 */

%x xb
%x xc
%x xd
%x xh
%x xq
%x xqs
%x xe
%x xdolq
%x xui
%x xus
%x xeu

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  Comments that start with -- and extend to the next
 * newline are treated as equivalent to a single whitespace character.
 * 为了使 Windows 和 Mac 客户端以及 Unix 客户端的世界都安全，
 * 我们接受 \n 或 \r 作为换行符。DOS 风格的 \r\n 序列将被视为两个连续的换行符，
 * 但这不会引起任何问题。以 -- 开头并延伸到下一个换行符的注释被视为等同于单个空格字符。
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 * 注意一个细微之处：如果 -- 后面没有换行符，我们将吸收所有内容直至输入结束作为注释。这是正确的。
 * 如果输入未以换行符结尾，旧版本的 Postgres 无法将 -- 识别为注释。
 *
 * non_newline_space tracks all the other space characters except newlines.
 * non_newline_space 跟踪除换行符之外的所有其他空格字符。
 *
 * XXX if you change the set of whitespace characters, fix scanner_isspace()
 * to agree.
 * XXX 如果您更改了空白字符集，请修正 scanner_isspace() 以使其保持一致。
 */

space				[ \t\n\r\f\v]
non_newline_space	[ \t\f\v]
newline				[\n\r]
non_newline			[^\n\r]

comment			("--"{non_newline}*)

whitespace		({space}+|{comment})

/*
 * SQL requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 * SQL 要求在要连接的字符串字面量之间的空白字符中至少有一个换行符。
 * 愚蠢，但我们凭什么争论呢？请注意 {whitespace_with_newline} 后面不应该有 *，
 * 而 {whitespace} 后面通常应该有一个 *...
 */

special_whitespace		({space}+|{comment}{newline})
non_newline_whitespace	({non_newline_space}|{comment})
whitespace_with_newline	({non_newline_whitespace}*{newline}{special_whitespace}*)

quote			'
/* If we see {quote} then {quotecontinue}, the quoted string continues
 * 如果我们看到 {quote}，接着是 {quotecontinue}，则带引号的字符串继续 */
quotecontinue	{whitespace_with_newline}{quote}

/*
 * {quotecontinuefail} is needed to avoid lexer backup when we fail to match
 * {quotecontinue}.  It might seem that this could just be {whitespace}*,
 * but if there's a dash after {whitespace_with_newline}, it must be consumed
 * to see if there's another dash --- which would start a {comment} and thus
 * allow continuation of the {quotecontinue} token.
 * 需要 {quotecontinuefail} 以免在未能匹配 {quotecontinue} 时发生词法分析器回溯。
 * 这似乎可能只是 {whitespace}*，但如果 {whitespace_with_newline} 后面有一个破折号，
 * 它就必须被消耗掉以查看是否存在另一个破折号 —— 另一个破折号将开始一个 {comment}，
 * 从而允许 {quotecontinue} token 的延续。
 */
quotecontinuefail	{whitespace}*"-"?

/* Bit string
 * It is tempting to scan the string for only those characters
 * which are allowed. However, this leads to silently swallowed
 * characters if illegal characters are included in the string.
 * For example, if xbinside is [01] then B'ABCD' is interpreted
 * as a zero-length string, and the ABCD' is lost!
 * Better to pass the string forward and let the input routines
 * validate the contents.
 * 比特位字符串
 * 仅仅扫描那些允许的字符是很有诱惑力的。然而，如果字符串中包含非法字符，这会导致非法字符被默默吞掉。
 * 例如，如果 xbinside 为 [01]，那么 B'ABCD' 将被解释为一个零长度字符串，而 ABCD' 就丢失了！
 * 最好是将该字符串向前传递，并让输入例程验证其内容。
 */
xbstart			[bB]{quote}
xbinside		[^']*

/* Hexadecimal byte string
 * 十六进制字节字符串 */
xhstart			[xX]{quote}
xhinside		[^']*

/* National character
 * 国家字符 */
xnstart			[nN]{quote}

/* Quoted string that allows backslash escapes
 * 允许反斜杠转义的带引号的字符串 */
xestart			[eE]{quote}
xeinside		[^\\']+
xeescape		[\\][^0-7]
xeoctesc		[\\][0-7]{1,3}
xehexesc		[\\]x[0-9A-Fa-f]{1,2}
xeunicode		[\\](u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{8})
xeunicodefail	[\\](u[0-9A-Fa-f]{0,3}|U[0-9A-Fa-f]{0,7})

/* Extended quote
 * xqdouble implements embedded quote, ''''
 * 扩展引号
 * xqdouble 实现了嵌入式引号，''''
 */
xqstart			{quote}
xqdouble		{quote}{quote}
xqinside		[^']+

/* $foo$ style quotes ("dollar quoting")
 * The quoted string starts with $foo$ where "foo" is an optional string
 * in the form of an identifier, except that it may not contain "$",
 * and extends to the first occurrence of an identical string.
 * There is *no* processing of the quoted text.
 *
 * {dolqfailed} is an error rule to avoid scanner backup when {dolqdelim}
 * fails to match its trailing "$".
 * $foo$ 风格引号（“美元符号引用”）
 * 引用字符串以 $foo$ 开头，其中 "foo" 是标识符形式的可选字符串，但它不能包含 "$"，
 * 并延伸至首次出现相同字符串的地方。
 * 对被引用的文本“不”进行任何处理。
 *
 * {dolqfailed} 是一个错误规则，用于避免 {dolqdelim} 未能匹配其末尾的 "$" 时发生扫描器回溯。
 */
dolq_start		[A-Za-z\200-\377_]
dolq_cont		[A-Za-z\200-\377_0-9]
dolqdelim		\$({dolq_start}{dolq_cont}*)?\$
dolqfailed		\${dolq_start}{dolq_cont}*
dolqinside		[^$]+

/* Double quote
 * Allows embedded spaces and other special characters into identifiers.
 * 双引号
 * 允许在标识符中嵌入空格和其他特殊字符。
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

/* Quoted identifier with Unicode escapes
 * 带有 Unicode 转义的带引号的标识符 */
xuistart		[uU]&{dquote}

/* Quoted string with Unicode escapes
 * 带有 Unicode 转义的带引号的字符串 */
xusstart		[uU]&{quote}

/* error rule to avoid backup
 * 避免回溯的错误规则 */
xufailed		[uU]&


/* C-style comments
 * C 风格的注释
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!  Also, if we
 * have something like plus-slash-star, lex will think this is a 3-character
 * operator whereas we want to see it as a + operator and a comment start.
 * “扩展注释”语法与允许的运算符语法非常相似。
 * 这里棘手的部分是让 lex 将斜杠星号开头的字符串识别为注释，因为如果将其解释为运算符，
 * 会产生更长的匹配 —— 记住 lex 将更倾向于更长的匹配！此外，如果我们遇到类似加号-斜杠-星号的内容，
 * lex 会认为这是一个 3 字符的运算符，而我们希望将其视为一个 + 运算符和一个注释的开始。
 *
 * The solution is two-fold:
 * 1. append {op_chars}* to xcstart so that it matches as much text as
 *    {operator} would. Then the tie-breaker (first matching rule of same
 *    length) ensures xcstart wins.  We put back the extra stuff with yyless()
 *    in case it contains a star-slash that should terminate the comment.
 * 2. In the operator rule, check for slash-star within the operator, and
 *    if found throw it back with yyless().  This handles the plus-slash-star
 *    problem.
 * Dash-dash comments have similar interactions with the operator rule.
 * 解决方案是两方面的：
 * 1. 将 {op_chars}* 附加到 xcstart，使其匹配与 {operator} 匹配的相同多的文本。
 *    然后，平分决胜者（同一长度的第一个匹配规则）可确保 xcstart 获胜。
 *    我们用 yyless() 把多余的内容放回去，以防它包含应该终止注释的星号-斜杠。
 * 2. 在运算符规则中，检查运算符内是否有斜杠-星号，如果发现，则用 yyless() 将其退回。
 *    这就处理了加号-斜杠-星号的问题。
 * 双破折号注释与运算符规则也有类似的相互作用。
 */
xcstart			\/\*{op_chars}*
xcstop			\*+\/
xcinside		[^*/]+

ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

/* Assorted special-case operators and operator-like tokens
 * 各种特殊情况下的运算符和类似运算符的 token */
typecast		"::"
dot_dot			\.\.
colon_equals	":="

/*
 * These operator-like tokens (unlike the above ones) also match the {operator}
 * rule, which means that they might be overridden by a longer match if they
 * are followed by a comment start or a + or - character. Accordingly, if you
 * add to this list, you must also add corresponding code to the {operator}
 * block to return the correct token in such cases. (This is not needed in
 * psqlscan.l since the token value is ignored there.)
 * 这些类似运算符的 token（与上述 token 不同）也匹配 {operator} 规则，
 * 这意味着如果它们后面跟有注释开始或者 + 或 - 字符，则它们可能会被更长的匹配所覆盖。
 * 因此，如果您将其添加到该列表中，您还必须将相应的代码添加到 {operator} 块中，
 * 以便在这些情况下返回正确的 token。（在 psqlscan.l 中不需要这样做，因为那里的 token 值被忽略了。）
 */
equals_greater	"=>"
less_equals		"<="
greater_equals	">="
less_greater	"<>"
not_equals		"!="

/*
 * "self" is the set of chars that should be returned as single-character
 * tokens.  "op_chars" is the set of chars that can make up "Op" tokens,
 * which can be one or more characters long (but if a single-char token
 * appears in the "self" set, it is not to be returned as an Op).  Note
 * that the sets overlap, but each has some chars that are not in the other.
 * “self” 是应作为单字符 token 返回的字符集。
 * “op_chars” 是可以组成 “Op” token 的字符集，可以是一个或多个字符长
 * （但如果单字符 token 出现在 “self” 集中，则它不能作为 Op 返回）。
 * 请注意，这两个集合有重叠，但每个集合都有一些不在另一个集合中的字符。
 *
 * If you change either set, adjust the character lists appearing in the
 * rule for "operator"!
 * 如果您更改了其中任何一个集合，请调整出现在 “operator” 规则中的字符列表！
 */
self			[,()\[\].;\:\+\-\*\/\%\^\<\>\=]
op_chars		[\~\!\@\#\^\&\|\`\?\+\-\*\/\%\<\>\=]
operator		{op_chars}+

/*
 * Numbers
 * 数值
 *
 * Unary minus is not part of a number here.  Instead we pass it separately to
 * the parser, and there it gets coerced via doNegate().
 * 在这里，一元负号不是数值的一部分。相反，我们将其分别传递给解析器，
 * 并在那里通过 doNegate() 对其进行转换。
 *
 * {numericfail} is used because we would like "1..10" to lex as 1, dot_dot, 10.
 * 使用 {numericfail} 是因为我们希望将 "1..10" 词法解析为 1，dot_dot，10。
 *
 * {realfail} is added to prevent the need for scanner
 * backup when the {real} rule fails to match completely.
 * 添加 {realfail} 是为了在 {real} 规则未能完全匹配时防止需要扫描器回溯。
 */
decdigit		[0-9]
hexdigit		[0-9A-Fa-f]
octdigit		[0-7]
bindigit		[0-1]

decinteger		{decdigit}(_?{decdigit})*
hexinteger		0[xX](_?{hexdigit})+
octinteger		0[oO](_?{octdigit})+
bininteger		0[bB](_?{bindigit})+

hexfail			0[xX]_?
octfail			0[oO]_?
binfail			0[bB]_?

numeric			(({decinteger}\.{decinteger}?)|(\.{decinteger}))
numericfail		{decinteger}\.\.

real			({decinteger}|{numeric})[Ee][-+]?{decinteger}
realfail		({decinteger}|{numeric})[Ee][-+]

/* Positional parameters don't accept underscores.
 * 位置参数不接受下划线。 */
param			\${decdigit}+

/*
 * An identifier immediately following an integer literal is disallowed because
 * in some cases it's ambiguous what is meant: for example, 0x1234 could be
 * either a hexinteger or a decinteger "0" and an identifier "x1234".  We can
 * detect such problems by seeing if integer_junk matches a longer substring
 * than any of the XXXinteger patterns (decinteger, hexinteger, octinteger,
 * bininteger).  One "junk" pattern is sufficient because
 * {decinteger}{identifier} will match all the same strings we'd match with
 * {hexinteger}{identifier} etc.
 * 紧随整数字面量之后的标识符是不允许的，因为在某些情况下，它的含义是有歧义的：
 * 例如，0x1234 既可以是一个十六进制整数，也可以是一个十进制整数 “0” 和一个标识符 “x1234”。
 * 我们可以通过查看 integer_junk 是否匹配比任何 XXXinteger 模式
 * （decinteger、hexinteger、octinteger、bininteger）更长的主子串来检测此类问题。
 * 一个 “junk” 模式就足够了，因为 {decinteger}{identifier} 将匹配与我们用 {hexinteger}{identifier} 等匹配的所有相同的字符串。
 *
 * Note that the rule for integer_junk must appear after the ones for
 * XXXinteger to make this work correctly: 0x1234 will match both hexinteger
 * and integer_junk, and we need hexinteger to be chosen in that case.
 * 请注意，为了使此机制正确工作，integer_junk 规则必须出现在 XXXinteger 规则之后：
 * 在这种情况下，0x1234 将同时匹配 hexinteger 和 integer_junk，我们希望选择 hexinteger。
 *
 * Also disallow strings matched by numeric_junk, real_junk and param_junk
 * for consistency.
 * 为保持一致性，还禁止与 numeric_junk、real_junk 和 param_junk 匹配的字符串。
 */
integer_junk	{decinteger}{identifier}
numeric_junk	{numeric}{identifier}
real_junk		{real}{identifier}
param_junk		\${decdigit}+{identifier}

other			.

/*
 * Dollar quoted strings are totally opaque, and no escaping is done on them.
 * Other quoted strings must allow some special characters such as single-quote
 *  and newline.
 * 美元符引用的字符串是完全不透明的，且不对其进行任何转义。
 * 其他带引号的字符串必须允许一些特殊字符，如单引号和换行符。
 *
 * Embedded single-quotes are implemented both in the SQL standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * 嵌入的单引号既在 SQL 标准样式中实现（两个相邻的单引号 "''"），
 * 也在 Postgres/Java 样式的转义引号 "\'" 中实现。
 *
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string.
 * 其他嵌入的转义字符被显式匹配，并从字符串中删除了前导反斜杠。
 *
 * Note that xcstart must appear before operator, as explained above!
 *  Also whitespace (comment) must appear before operator.
 * 请注意，如上所述，xcstart 必须出现在 operator 之前！
 * 此外，空白字符（注释）必须出现在 operator 之前。
 */

%%

{whitespace}	{
					/* ignore */
				}

{xcstart}		{
					/* Set location in case of syntax error in comment */
					SET_YYLLOC();
					yyextra->xcdepth = 0;
					BEGIN(xc);
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

<xc>{
{xcstart}		{
					(yyextra->xcdepth)++;
					/* Put back any characters past slash-star; see above */
					yyless(2);
				}

{xcstop}		{
					if (yyextra->xcdepth <= 0)
						BEGIN(INITIAL);
					else
						(yyextra->xcdepth)--;
				}

{xcinside}		{
					/* ignore */
				}

{op_chars}		{
					/* ignore */
				}

\*+				{
					/* ignore */
				}

<<EOF>>			{
					yyerror("unterminated /* comment");
				}
} /* <xc> */

{xbstart}		{
					/* Binary bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "b" on the string
					 * to mark it for the input routine as a binary string.
					 * 二进制比特类型。
					 * 在某些时候，我们应该简单地将该字符串向前传递给解析器，并在那里对其进行标记。
					 * 与此同时，在字符串前加上前导 "b"，以向输入例程标记其为二进制字符串。
					 */
					SET_YYLLOC();
					BEGIN(xb);
					startlit();
					addlitchar('b', yyscanner);
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					addlit(yytext, yyleng, yyscanner);
				}
<xb><<EOF>>		{ yyerror("unterminated bit string literal"); }

{xhstart}		{
					/* Hexadecimal bit type.
					 * At some point we should simply pass the string
					 * forward to the parser and label it there.
					 * In the meantime, place a leading "x" on the string
					 * to mark it for the input routine as a hex string.
					 * 十六进制比特类型。
					 * 在某些时候，我们应该简单地将该字符串向前传递给解析器，并在那里对其进行标记。
					 * 与此同时，在字符串前加上前导 "x"，以向输入例程标记其为十六进制字符串。
					 */
					SET_YYLLOC();
					BEGIN(xh);
					startlit();
					addlitchar('x', yyscanner);
				}
<xh><<EOF>>		{ yyerror("unterminated hexadecimal string literal"); }

{xnstart}		{
					/* National character.
					 * We will pass this along as a normal character string,
					 * but preceded with an internally-generated "NCHAR".
					 * 国家字符。
					 * 我们将把它作为一个普通字符字符串传递下去，但在其前面加上一个内部生成的 "NCHAR"。
					 */
					int		kwnum;

					SET_YYLLOC();
					yyless(1);	/* eat only 'n' this time */

					kwnum = ScanKeywordLookup("nchar",
											  yyextra->keywordlist);
					if (kwnum >= 0)
					{
						yylval->keyword = GetScanKeyword(kwnum,
														 yyextra->keywordlist);
						return yyextra->keyword_tokens[kwnum];
					}
					else
					{
						/* If NCHAR isn't a keyword, just return "n"
						 * 如果 NCHAR 不是关键字，只需返回 "n" */
						yylval->str = pstrdup("n");
						return IDENT;
					}
				}

{xqstart}		{
					yyextra->warn_on_first_escape = true;
					yyextra->saw_non_ascii = false;
					SET_YYLLOC();
					if (yyextra->standard_conforming_strings)
						BEGIN(xq);
					else
						BEGIN(xe);
					startlit();
				}
{xestart}		{
					yyextra->warn_on_first_escape = false;
					yyextra->saw_non_ascii = false;
					SET_YYLLOC();
					BEGIN(xe);
					startlit();
				}
{xusstart}		{
					SET_YYLLOC();
					if (!yyextra->standard_conforming_strings)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("unsafe use of string constant with Unicode escapes"),
								 errdetail("String constants with Unicode escapes cannot be used when \"standard_conforming_strings\" is off."),
								 lexer_errposition()));
					BEGIN(xus);
					startlit();
				}

<xb,xh,xq,xe,xus>{quote} {
					/*
					 * When we are scanning a quoted string and see an end
					 * quote, we must look ahead for a possible continuation.
					 * If we don't see one, we know the end quote was in fact
					 * the end of the string.  To reduce the lexer table size,
					 * we use a single "xqs" state to do the lookahead for all
					 * types of strings.
					 * 当我们正在扫描一个带引号的字符串并看到一个结束引号时，我们必须向前看是否存在可能的延续。
					 * 如果我们没有看到，我们知道结束引号实际上就是字符串的末尾。
					 * 为了减小词法分析器表的大小，我们使用单个 "xqs" 状态对所有类型的字符串进行向前看。
					 */
					yyextra->state_before_str_stop = YYSTATE;
					BEGIN(xqs);
				}
<xqs>{quotecontinue} {
					/*
					 * Found a quote continuation, so return to the in-quote
					 * state and continue scanning the literal.  Nothing is
					 * added to the literal's contents.
					 * 找到了引号延续，因此返回到引号内状态并继续扫描字面量。
					 * 字面量的内容中不添加任何内容。
					 */
					BEGIN(yyextra->state_before_str_stop);
				}
<xqs>{quotecontinuefail} |
<xqs>{other} |
<xqs><<EOF>>	{
					/*
					 * Failed to see a quote continuation.  Throw back
					 * everything after the end quote, and handle the string
					 * according to the state we were in previously.
					 * 未能看到引号延续。退回结束引号之后的所有内容，
					 * 并根据我们之前所处的状态来处理该字符串。
					 */
					yyless(0);
					BEGIN(INITIAL);

					switch (yyextra->state_before_str_stop)
					{
						case xb:
							yylval->str = litbufdup(yyscanner);
							return BCONST;
						case xh:
							yylval->str = litbufdup(yyscanner);
							return XCONST;
						case xq:
						case xe:
							/*
							 * Check that the data remains valid, if it might
							 * have been made invalid by unescaping any chars.
							 * 检查数据是否仍然有效，以防通过反转义任何字符可能已使数据无效。
							 */
							if (yyextra->saw_non_ascii)
								pg_verifymbstr(yyextra->literalbuf,
											   yyextra->literallen,
											   false);
							yylval->str = litbufdup(yyscanner);
							return SCONST;
						case xus:
							yylval->str = litbufdup(yyscanner);
							return USCONST;
						default:
							yyerror("unhandled previous state in xqs");
					}
				}

<xq,xe,xus>{xqdouble} {
					addlitchar('\'', yyscanner);
				}
<xq,xus>{xqinside}  {
					addlit(yytext, yyleng, yyscanner);
				}
<xe>{xeinside}  {
					addlit(yytext, yyleng, yyscanner);
				}
<xe>{xeunicode} {
					pg_wchar	c = strtoul(yytext + 2, NULL, 16);

					/*
					 * For consistency with other productions, issue any
					 * escape warning with cursor pointing to start of string.
					 * We might want to change that, someday.
					 * 为了与其他产生式保持一致，在发出任何转义警告时，游标要指向字符串的开头。
					 * 也许有一天我们会想要改变这一点。
					 */
					check_escape_warning(yyscanner);

					/* Remember start of overall string token ...
					 * 记住整个字符串 token 的开始 ... */
					PUSH_YYLLOC();
					/* ... and set the error cursor to point at this esc seq
					 * ... 并将错误游标设置为指向该转义序列 */
					SET_YYLLOC();

					if (is_utf16_surrogate_first(c))
					{
						yyextra->utf16_first_part = c;
						BEGIN(xeu);
					}
					else if (is_utf16_surrogate_second(c))
						yyerror("invalid Unicode surrogate pair");
					else
						addunicode(c, yyscanner);

					/* Restore yylloc to be start of string token
					 * 将 yylloc 恢复为字符串 token 的开始 */
					POP_YYLLOC();
				}
<xeu>{xeunicode} {
					pg_wchar	c = strtoul(yytext + 2, NULL, 16);

					/* Remember start of overall string token ... */
					PUSH_YYLLOC();
					/* ... and set the error cursor to point at this esc seq */
					SET_YYLLOC();

					if (!is_utf16_surrogate_second(c))
						yyerror("invalid Unicode surrogate pair");

					c = surrogate_pair_to_codepoint(yyextra->utf16_first_part, c);

					addunicode(c, yyscanner);

					/* Restore yylloc to be start of string token */
					POP_YYLLOC();

					BEGIN(xe);
				}
<xeu>. |
<xeu>\n |
<xeu><<EOF>>	{
					/* Set the error cursor to point at missing esc seq
					 * 将错误游标设置为指向丢失的转义序列 */
					SET_YYLLOC();
					yyerror("invalid Unicode surrogate pair");
				}
<xe,xeu>{xeunicodefail}	{
					/* Set the error cursor to point at malformed esc seq
					 * 将错误游标设置为指向畸形的转义序列 */
					SET_YYLLOC();
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
							 errmsg("invalid Unicode escape"),
							 errhint("Unicode escapes must be \\uXXXX or \\UXXXXXXXX."),
							 lexer_errposition()));
				}
<xe>{xeescape}  {
					if (yytext[1] == '\'')
					{
						if (yyextra->backslash_quote == BACKSLASH_QUOTE_OFF ||
							(yyextra->backslash_quote == BACKSLASH_QUOTE_SAFE_ENCODING &&
							 PG_ENCODING_IS_CLIENT_ONLY(pg_get_client_encoding())))
							ereport(ERROR,
									(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
									 errmsg("unsafe use of \\' in a string literal"),
									 errhint("Use '' to write quotes in strings. \\' is insecure in client-only encodings."),
									 lexer_errposition()));
					}
					check_string_escape_warning(yytext[1], yyscanner);
					addlitchar(unescape_single_char(yytext[1], yyscanner),
							   yyscanner);
				}
<xe>{xeoctesc}  {
					unsigned char c = strtoul(yytext + 1, NULL, 8);

					check_escape_warning(yyscanner);
					addlitchar(c, yyscanner);
					if (c == '\0' || IS_HIGHBIT_SET(c))
						yyextra->saw_non_ascii = true;
				}
<xe>{xehexesc}  {
					unsigned char c = strtoul(yytext + 2, NULL, 16);

					check_escape_warning(yyscanner);
					addlitchar(c, yyscanner);
					if (c == '\0' || IS_HIGHBIT_SET(c))
						yyextra->saw_non_ascii = true;
				}
<xe>.			{
					/* This is only needed for \ just before EOF
					 * 这仅对于 EOF 紧前方的 \ 是必需的 */
					addlitchar(yytext[0], yyscanner);
				}
<xq,xe,xus><<EOF>>		{ yyerror("unterminated quoted string"); }

{dolqdelim}		{
					SET_YYLLOC();
					yyextra->dolqstart = pstrdup(yytext);
					BEGIN(xdolq);
					startlit();
				}
{dolqfailed}	{
					SET_YYLLOC();
					/* throw back all but the initial "$"
					 * 退回除首个 "$" 之外的所有内容 */
					yyless(1);
					/* and treat it as {other}
					 * 并将其视为 {other} */
					return yytext[0];
				}
<xdolq>{dolqdelim} {
					if (strcmp(yytext, yyextra->dolqstart) == 0)
					{
						pfree(yyextra->dolqstart);
						yyextra->dolqstart = NULL;
						BEGIN(INITIAL);
						yylval->str = litbufdup(yyscanner);
						return SCONST;
					}
					else
					{
						/*
						 * When we fail to match $...$ to dolqstart, transfer
						 * the $... part to the output, but put back the final
						 * $ for rescanning.  Consider $delim$...$junk$delim$
						 * 当我们未能将 $...$ 与 dolqstart 匹配时，将 $...$ 部分转移到输出，
						 * 但退回最后的 $ 以进行重新扫描。考虑 $delim$...$junk$delim$
						 */
						addlit(yytext, yyleng - 1, yyscanner);
						yyless(yyleng - 1);
					}
				}
<xdolq>{dolqinside} {
					addlit(yytext, yyleng, yyscanner);
				}
<xdolq>{dolqfailed} {
					addlit(yytext, yyleng, yyscanner);
				}
<xdolq>.		{
					/* This is only needed for $ inside the quoted text
					 * 这仅对于被引用文本内的 $ 是必需的 */
					addlitchar(yytext[0], yyscanner);
				}
<xdolq><<EOF>>	{ yyerror("unterminated dollar-quoted string"); }

{xdstart}		{
					SET_YYLLOC();
					BEGIN(xd);
					startlit();
				}
{xuistart}		{
					SET_YYLLOC();
					BEGIN(xui);
					startlit();
				}
<xd>{xdstop}	{
					char	   *ident;

					BEGIN(INITIAL);
					if (yyextra->literallen == 0)
						yyerror("zero-length delimited identifier");
					ident = litbufdup(yyscanner);
					if (yyextra->literallen >= NAMEDATALEN)
						truncate_identifier(ident, yyextra->literallen, true);
					yylval->str = ident;
					return IDENT;
				}
<xui>{dquote}	{
					BEGIN(INITIAL);
					if (yyextra->literallen == 0)
						yyerror("zero-length delimited identifier");
					/* can't truncate till after we de-escape the ident
					 * 在我们对标识符进行反转义之前不能截断 */
					yylval->str = litbufdup(yyscanner);
					return UIDENT;
				}
<xd,xui>{xddouble}	{
					addlitchar('"', yyscanner);
				}
<xd,xui>{xdinside}	{
					addlit(yytext, yyleng, yyscanner);
				}
<xd,xui><<EOF>>		{ yyerror("unterminated quoted identifier"); }

{xufailed}	{
					char	   *ident;

					SET_YYLLOC();
					/* throw back all but the initial u/U
					 * 退回除首个 u/U 之外的所有内容 */
					yyless(1);
					/* and treat it as {identifier}
					 * 并将其视为 {identifier} */
					ident = downcase_truncate_identifier(yytext, yyleng, true);
					yylval->str = ident;
					return IDENT;
				}

{typecast}		{
					SET_YYLLOC();
					return TYPECAST;
				}

{dot_dot}		{
					SET_YYLLOC();
					return DOT_DOT;
				}

{colon_equals}	{
					SET_YYLLOC();
					return COLON_EQUALS;
				}

{equals_greater} {
					SET_YYLLOC();
					return EQUALS_GREATER;
				}

{less_equals}	{
					SET_YYLLOC();
					return LESS_EQUALS;
				}

{greater_equals} {
					SET_YYLLOC();
					return GREATER_EQUALS;
				}

{less_greater}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS
					 * 我们接受 "<>" 和 "!="，都表示 NOT_EQUALS */
					SET_YYLLOC();
					return NOT_EQUALS;
				}

{not_equals}	{
					/* We accept both "<>" and "!=" as meaning NOT_EQUALS */
					SET_YYLLOC();
					return NOT_EQUALS;
				}

{self}			{
					SET_YYLLOC();
					return yytext[0];
				}

{operator}		{
					/*
					 * Check for embedded slash-star or dash-dash; those
					 * are comment starts, so operator must stop there.
					 * Note that slash-star or dash-dash at the first
					 * character will match a prior rule, not this one.
					 * 检查嵌入的斜杠-星号或双破折号；这些是注释的开始，因此运算符必须在那里停止。
					 * 请注意，首个字符处的斜杠-星号或双破折号将匹配先前的规则，而不是此规则。
					 */
					int			nchars = yyleng;
					char	   *slashstar = strstr(yytext, "/*");
					char	   *dashdash = strstr(yytext, "--");

					if (slashstar && dashdash)
					{
						/* if both appear, take the first one
						 * 如果两者都出现，取第一个 */
						if (slashstar > dashdash)
							slashstar = dashdash;
					}
					else if (!slashstar)
						slashstar = dashdash;
					if (slashstar)
						nchars = slashstar - yytext;

					/*
					 * For SQL compatibility, '+' and '-' cannot be the
					 * last char of a multi-char operator unless the operator
					 * contains chars that are not in SQL operators.
					 * The idea is to lex '=-' as two operators, but not
					 * to forbid operator names like '?-' that could not be
					 * sequences of SQL operators.
					 * 为了 SQL 兼容性，'+' 和 '-' 不能是多字符运算符的最后一个字符，
					 * 除非该运算符包含不在 SQL 运算符中的字符。
					 * 其想法是将 '=-' 词法解析为两个运算符，但不禁止像 '?-' 这样不可能是 SQL 运算符序列的运算符名称。
					 */
					if (nchars > 1 &&
						(yytext[nchars - 1] == '+' ||
						 yytext[nchars - 1] == '-'))
					{
						int			ic;

						for (ic = nchars - 2; ic >= 0; ic--)
						{
							char c = yytext[ic];
							if (c == '~' || c == '!' || c == '@' ||
								c == '#' || c == '^' || c == '&' ||
								c == '|' || c == '`' || c == '?' ||
								c == '%')
								break;
						}
						if (ic < 0)
						{
							/*
							 * didn't find a qualifying character, so remove
							 * all trailing [+-]
							 * 未找到符合条件的字符，因此移除所有末尾的 [+-]
							 */
							do {
								nchars--;
							} while (nchars > 1 &&
								 (yytext[nchars - 1] == '+' ||
								  yytext[nchars - 1] == '-'));
						}
					}

					SET_YYLLOC();

					if (nchars < yyleng)
					{
						/* Strip the unwanted chars from the token
						 * 从 token 中剥离不需要的字符 */
						yyless(nchars);
						/*
						 * If what we have left is only one char, and it's
						 * one of the characters matching "self", then
						 * return it as a character token the same way
						 * that the "self" rule would have.
						 * 如果我们剩下的只有一个字符，并且它是与 “self” 匹配的字符之一，
						 * 则以与 “self” 规则相同的方式将其作为字符 token 返回。
						 */
						if (nchars == 1 &&
							strchr(",()[].;:+-*/%^<>=", yytext[0]))
							return yytext[0];
						/*
						 * Likewise, if what we have left is two chars, and
						 * those match the tokens ">=", "<=", "=>", "<>" or
						 * "!=", then we must return the appropriate token
						 * rather than the generic Op.
						 * 同样，如果我们剩下的是两个字符，且它们匹配 token ">="、"<="、"=>"、"<>" 或 "!="，
						 * 则我们必须返回相应的 token，而不是通用的 Op。
						 */
						if (nchars == 2)
						{
							if (yytext[0] == '=' && yytext[1] == '>')
								return EQUALS_GREATER;
							if (yytext[0] == '>' && yytext[1] == '=')
								return GREATER_EQUALS;
							if (yytext[0] == '<' && yytext[1] == '=')
								return LESS_EQUALS;
							if (yytext[0] == '<' && yytext[1] == '>')
								return NOT_EQUALS;
							if (yytext[0] == '!' && yytext[1] == '=')
								return NOT_EQUALS;
						}
					}

					/*
					 * Complain if operator is too long.  Unlike the case
					 * for identifiers, we make this an error not a notice-
					 * and-truncate, because the odds are we are looking at
					 * a syntactic mistake anyway.
					 * 如果运算符太长则发出抱怨。与标识符的情况不同，我们将此视为错误，而不是通知并截断，
					 * 因为无论如何我们很可能正面临着语法错误。
					 */
					if (nchars >= NAMEDATALEN)
						yyerror("operator too long");

					yylval->str = pstrdup(yytext);
					return Op;
				}

{param}			{
					ErrorSaveContext escontext = {T_ErrorSaveContext};
					int32		val;

					SET_YYLLOC();
					val = pg_strtoint32_safe(yytext + 1, (Node *) &escontext);
					if (escontext.error_occurred)
						yyerror("parameter number too large");
					yylval->ival = val;
					return PARAM;
				}
{param_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after parameter");
				}

{decinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 10);
				}
{hexinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 16);
				}
{octinteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 8);
				}
{bininteger}	{
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 2);
				}
{hexfail}		{
					SET_YYLLOC();
					yyerror("invalid hexadecimal integer");
				}
{octfail}		{
					SET_YYLLOC();
					yyerror("invalid octal integer");
				}
{binfail}		{
					SET_YYLLOC();
					yyerror("invalid binary integer");
				}
{numeric}		{
					SET_YYLLOC();
					yylval->str = pstrdup(yytext);
					return FCONST;
				}
{numericfail}	{
					/* throw back the .., and treat as integer
					 * 退回 ..，并将其视为整数 */
					yyless(yyleng - 2);
					SET_YYLLOC();
					return process_integer_literal(yytext, yylval, 10);
				}
{real}			{
					SET_YYLLOC();
					yylval->str = pstrdup(yytext);
					return FCONST;
				}
{realfail}		{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{integer_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{numeric_junk}	{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}
{real_junk}		{
					SET_YYLLOC();
					yyerror("trailing junk after numeric literal");
				}


{identifier}	{
					int			kwnum;
					char	   *ident;

					SET_YYLLOC();

					/* Is it a keyword?
					 * 它是关键字吗？ */
					kwnum = ScanKeywordLookup(yytext,
											  yyextra->keywordlist);
					if (kwnum >= 0)
					{
						yylval->keyword = GetScanKeyword(kwnum,
														 yyextra->keywordlist);
						return yyextra->keyword_tokens[kwnum];
					}

					/*
					 * No.  Convert the identifier to lower case, and truncate
					 * if necessary.
					 * 否。将标识符转换为小写，并在必要时进行截断。
					 */
					ident = downcase_truncate_identifier(yytext, yyleng, true);
					yylval->str = ident;
					return IDENT;
				}

{other}			{
					SET_YYLLOC();
					return yytext[0];
				}

<<EOF>>			{
					SET_YYLLOC();
					yyterminate();
				}

%%

/* LCOV_EXCL_STOP */

/*
 * Arrange access to yyextra for subroutines of the main yylex() function.
 * We expect each subroutine to have a yyscanner parameter.  Rather than
 * use the yyget_xxx functions, which might or might not get inlined by the
 * compiler, we cheat just a bit and cast yyscanner to the right type.
 * 为主 yylex() 函数的子程序安排对 yyextra 的访问。
 * 我们期望每个子程序都有一个 yyscanner 参数。与使用可能被编译器内联也可能不被内联的 yyget_xxx 函数不同，
 * 我们稍微作弊一下，将 yyscanner 强转为正确的类型。
 */
#undef yyextra
#define yyextra  (((struct yyguts_t *) yyscanner)->yyextra_r)

/* Likewise for a couple of other things we need.
 * 同样适用于我们需要的其他几样东西。 */
#undef yylloc
#define yylloc	(((struct yyguts_t *) yyscanner)->yylloc_r)
#undef yyleng
#define yyleng	(((struct yyguts_t *) yyscanner)->yyleng_r)


/*
 * scanner_errposition
 *		Report a lexer or grammar error cursor position, if possible.
 *		如果可能，报告词法分析器或语法错误的游标位置。
 *
 * This is expected to be used within an ereport() call, or via an error
 * callback such as setup_scanner_errposition_callback().  The return value
 * is a dummy (always 0, in fact).
 * 这预计在 ereport() 调用中使用，或者通过错误回调（如 setup_scanner_errposition_callback()）使用。
 * 返回值是虚拟的（实际上始终为 0）。
 *
 * Note that this can only be used for messages emitted during raw parsing
 * (essentially, scan.l, parser.c, and gram.y), since it requires the
 * yyscanner struct to still be available.
 * 请注意，这只能用于在原始解析期间发出的消息（基本上是 scan.l、parser.c 和 gram.y），
 * 因为它需要 yyscanner 结构体仍然可用。
 *
 * 函数作用：
 * 将当前的字节偏移量位置转换为以字符数为单位的位置，并将其注册到 errposition 错误反馈机制中。
 */
int
scanner_errposition(int location, core_yyscan_t yyscanner)
{
	int			pos;

	if (location < 0)
		return 0;				/* no-op if location is unknown
								   如果位置未知，则不执行任何操作 */

	/* Convert byte offset to character number
	 * 将字节偏移量转换为字符数 */
	pos = pg_mbstrlen_with_len(yyextra->scanbuf, location) + 1;
	/* And pass it to the ereport mechanism
	 * 并将其传递给 ereport 机制 */
	return errposition(pos);
}

/*
 * Error context callback for inserting scanner error location.
 * 用于插入扫描器错误位置的错误上下文回调。
 *
 * Note that this will be called for *any* error occurring while the
 * callback is installed.  We avoid inserting an irrelevant error location
 * if the error is a query cancel --- are there any other important cases?
 * 请注意，这将在安装了回调时发生任何错误时被调用。
 * 如果错误是查询取消，我们将避免插入不相关的错误位置 —— 还有其他重要情况吗？
 *
 * 函数作用：
 * 错误发生时的上下文回调函数。当发生错误（且非查询取消）时，
 * 它会自动将注册的扫描器位置附加到错误报告中，以提供精确的位置信息。
 */
static void
scb_error_callback(void *arg)
{
	ScannerCallbackState *scbstate = (ScannerCallbackState *) arg;

	if (geterrcode() != ERRCODE_QUERY_CANCELED)
		(void) scanner_errposition(scbstate->location, scbstate->yyscanner);
}

/*
 * setup_scanner_errposition_callback
 *		Arrange for non-scanner errors to report an error position
 *		安排非扫描器错误报告错误位置
 *
 * Sometimes the scanner calls functions that aren't part of the scanner
 * subsystem and can't reasonably be passed the yyscanner pointer; yet
 * we would like any errors thrown in those functions to be tagged with an
 * error location.  Use this function to set up an error context stack
 * entry that will accomplish that.  Usage pattern:
 * 有时，扫描器会调用不属于扫描器子系统的函数，并且无法合理地传递 yyscanner 指针；
 * 然而，我们希望在这些函数中抛出的任何错误都被标记上错误位置。
 * 使用此函数设置一个错误上下文栈条目来实现这一目的。使用模式：
 *
 *		declare a local variable "ScannerCallbackState scbstate"
 *		...
 *		setup_scanner_errposition_callback(&scbstate, yyscanner, location);
 *		call function that might throw error;
 *		cancel_scanner_errposition_callback(&scbstate);
 *
 * 函数作用：
 * 建立并注册一个错误上下文回调，使调用一些第三方或外部库函数时，
 * 若在其中触发了异常/错误，也能标记并向用户反馈是在 SQL 中的哪个精确位置发生的。
 */
void
setup_scanner_errposition_callback(ScannerCallbackState *scbstate,
								   core_yyscan_t yyscanner,
								   int location)
{
	/* Setup error traceback support for ereport()
	 * 为 ereport() 设置错误回溯支持 */
	scbstate->yyscanner = yyscanner;
	scbstate->location = location;
	scbstate->errcallback.callback = scb_error_callback;
	scbstate->errcallback.arg = scbstate;
	scbstate->errcallback.previous = error_context_stack;
	error_context_stack = &scbstate->errcallback;
}

/*
 * Cancel a previously-set-up errposition callback.
 * 取消先前设置的 errposition 回调。
 *
 * 函数作用：
 * 清理撤销之前使用 `setup_scanner_errposition_callback()` 建立的回调状态，
 * 将 error_context_stack 恢复到之前的节点。
 */
void
cancel_scanner_errposition_callback(ScannerCallbackState *scbstate)
{
	/* Pop the error context stack
	 * 弹出错误上下文栈 */
	error_context_stack = scbstate->errcallback.previous;
}

/*
 * scanner_yyerror
 *		Report a lexer or grammar error.
 *		报告词法分析器或语法错误。
 *
 * The message's cursor position is whatever YYLLOC was last set to,
 * ie, the start of the current token if called within yylex(), or the
 * most recently lexed token if called from the grammar.
 * This is OK for syntax error messages from the Bison parser, because Bison
 * parsers report error as soon as the first unparsable token is reached.
 * Beware of using yyerror for other purposes, as the cursor position might
 * be misleading!
 * 信息的游标位置是上一次设置 of YYLLOC 处的任何内容，
 * 即如果在 yylex() 中调用，则是当前 token 的开始，如果从语法中调用，则是最近解析的 token。
 * 对于来自 Bison 解析器的语法错误信息，这是可以的，因为一旦达到第一个不可解析的 token，Bison 解析器就会报告错误。
 * 谨防将 yyerror 用于其他目的，因为游标位置可能会误导！
 *
 * 函数作用：
 * 根据错误发生的位置是否在输入文本结束处，抛出一个语法错误（errcode为 ERRCODE_SYNTAX_ERROR），
 * 显示详细的错误语境信息，例如“syntax error at end of input”或“syntax error at or near...”。
 */
void
scanner_yyerror(const char *message, core_yyscan_t yyscanner)
{
	const char *loc = yyextra->scanbuf + *yylloc;

	if (*loc == YY_END_OF_BUFFER_CHAR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: %s is typically the translation of "syntax error"
		 * 翻译者：%s 通常是 "syntax error"（语法错误）的翻译 */
				 errmsg("%s at end of input", _(message)),
				 lexer_errposition()));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/* translator: first %s is typically the translation of "syntax error"
		 * 翻译者：第一个 %s 通常是 "syntax error"（语法错误）的翻译 */
				 errmsg("%s at or near \"%s\"", _(message), loc),
				 lexer_errposition()));
	}
}


/*
 * Called before any actual parsing is done
 * 在进行任何实际解析之前调用
 *
 * 函数作用：
 * 词法扫描器的入口初始化函数。
 * - 调用 yylex_init() 初始化 Flex。
 * - 设置扫描器的自定义扩展信息，如关键字列表及其 Token 映射、反斜杠转义警示 GUC 状态。
 * - 拷贝传入的 SQL 查询字符串，追加两个 Flex 缓冲结束标志符 '\0'。
 * - 通过 yy_scan_buffer 分配扫描缓冲区，并初始化临时拼接字符串字面量所需的 literalbuf 缓冲区。
 */
core_yyscan_t
scanner_init(const char *str,
			 core_yy_extra_type *yyext,
			 const ScanKeywordList *keywordlist,
			 const uint16 *keyword_tokens)
{
	Size		slen = strlen(str);
	yyscan_t	scanner;

	if (yylex_init(&scanner) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	core_yyset_extra(yyext, scanner);

	yyext->keywordlist = keywordlist;
	yyext->keyword_tokens = keyword_tokens;

	yyext->backslash_quote = backslash_quote;
	yyext->escape_string_warning = escape_string_warning;
	yyext->standard_conforming_strings = standard_conforming_strings;

	/*
	 * Make a scan buffer with special termination needed by flex.
	 * 制作一个具有 flex 所需特殊终止符的扫描缓冲区。
	 */
	yyext->scanbuf = (char *) palloc(slen + 2);
	yyext->scanbuflen = slen;
	memcpy(yyext->scanbuf, str, slen);
	yyext->scanbuf[slen] = yyext->scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	yy_scan_buffer(yyext->scanbuf, slen + 2, scanner);

	/* initialize literal buffer to a reasonable but expansible size
	 * 将字面量缓冲区初始化为合理但可扩容的大小 */
	yyext->literalalloc = 1024;
	yyext->literalbuf = (char *) palloc(yyext->literalalloc);
	yyext->literallen = 0;

	return scanner;
}


/*
 * Called after parsing is done to clean up after scanner_init()
 * 在解析完成后调用，用于在 scanner_init() 之后进行清理
 *
 * 函数作用：
 * 用于在 SQL 解析结束时进行内存回收。
 * 如果之前分配的 SQL 缓冲（scanbuf）或拼接字面量缓冲（literalbuf）占用的内存超出了 8KB 的阈值，
 * 就会在此处主动释放它们以避免内存泄漏或过度占用空间，而较小空间的缓存则通常通过整个解析 MemoryContext 的销毁自动回收。
 */
void
scanner_finish(core_yyscan_t yyscanner)
{
	/*
	 * We don't bother to call yylex_destroy(), because all it would do is
	 * pfree a small amount of control storage.  It's cheaper to leak the
	 * storage until the parsing context is destroyed.  The amount of space
	 * involved is usually negligible compared to the output parse tree
	 * anyway.
	 * 我们不麻烦去调用 yylex_destroy()，因为它的全部工作只是 pfree 一小部分控制存储空间。
	 * 泄露这些存储空间，直到解析上下文被销毁是更便宜的选择。反正涉及的空间量与输出解析树相比通常是微不足道的。
	 *
	 * We do bother to pfree the scanbuf and literal buffer, but only if they
	 * represent a nontrivial amount of space.  The 8K cutoff is arbitrary.
	 * 我们确实不嫌麻烦去 pfree 扫描缓冲区和字面量缓冲区，但前提是它们代表了不小的一大块空间。8K 的截止值是任意的。
	 */
	if (yyextra->scanbuflen >= 8192)
		pfree(yyextra->scanbuf);
	if (yyextra->literalalloc >= 8192)
		pfree(yyextra->literalbuf);
}


/*
 * addlit
 *
 * Append characters to the literal text buffer, enlarging the buffer if needed.
 * 向字面量文本缓冲区追加字符，如果需要则扩大缓冲区。
 *
 * 函数作用：
 * 当需要动态存储字面量内容时被调用，它支持自动扩展字面量缓冲区（按照2的幂指数大小进行扩容），
 * 并使用 memcpy 将扫描到的 yytext 内容追加 to literalbuf 的尾端。
 */
static void
addlit(char *ytext, int yleng, core_yyscan_t yyscanner)
{
	/* enlarge buffer if needed
	 * 如果需要，扩大缓冲区 */
	if ((yyextra->literallen + yleng) >= yyextra->literalalloc)
	{
		yyextra->literalalloc = pg_nextpower2_32(yyextra->literallen + yleng + 1);
		yyextra->literalbuf = (char *) repalloc(yyextra->literalbuf,
												yyextra->literalalloc);
	}
	/* append new data
	 * 追加新数据 */
	memcpy(yyextra->literalbuf + yyextra->literallen, ytext, yleng);
	yyextra->literallen += yleng;
}


/*
 * addlitchar
 *
 * Append a single character to the literal text buffer, enlarging if needed.
 * 向字面量文本缓冲区追加单个字符，如果需要则扩大缓冲区。
 *
 * 函数作用：
 * 用于单个字符（例如转义后的字符，或者双引号标识符内的嵌入单引号字符等）的动态追加，
 * 同样在需要时对 literalbuf 缓冲容量进行翻倍扩容。
 */
static void
addlitchar(unsigned char ychar, core_yyscan_t yyscanner)
{
	/* enlarge buffer if needed */
	if ((yyextra->literallen + 1) >= yyextra->literalalloc)
	{
		yyextra->literalalloc *= 2;
		yyextra->literalbuf = (char *) repalloc(yyextra->literalbuf,
												yyextra->literalalloc);
	}
	/* append new data */
	yyextra->literalbuf[yyextra->literallen] = ychar;
	yyextra->literallen += 1;
}


/*
 * Create a palloc'd copy of literalbuf, adding a trailing null.
 * 创建 literalbuf 的 palloc 分配的副本，并添加尾随的空字符 (null)。
 *
 * 函数作用：
 * 将累积的临时 literalbuf 中的字符串内容拷贝到一个新分配的 palloc 字符串中，
 * 并以 '\0' 结尾。通常作为完成一个带引号的字符串或标识符词法解析时的最终结果返回。
 */
static char *
litbufdup(core_yyscan_t yyscanner)
{
	int			llen = yyextra->literallen;
	char	   *new;

	new = palloc(llen + 1);
	memcpy(new, yyextra->literalbuf, llen);
	new[llen] = '\0';
	return new;
}

/*
 * Process {decinteger}, {hexinteger}, etc.  Note this will also do the right
 * thing with {numeric}, ie digits and a decimal point.
 * 处理 {decinteger}、{hexinteger} 等。请注意，这也将正确处理 {numeric}，即数字和小数点。
 *
 * 函数作用：
 * 尝试将数字文本以指定的基数（10、16、8、2）解析转换为一个 32 位整数（int32）。
 * 如果数字由于过大发生溢出，或者包含小数点无法放入 int32，则回退为浮点数词法解析，
 * 并返回 FCONST（浮点数常量 token 标识）；否则返回 ICONST（整数常量 token 标识）。
 */
static int
process_integer_literal(const char *token, YYSTYPE *lval, int base)
{
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	int32		val;

	val = pg_strtoint32_safe(token, (Node *) &escontext);
	if (escontext.error_occurred)
	{
		/* integer too large (or contains decimal pt), treat it as a float
		 * 整数太大（或包含小数点），将其视为浮点数 */
		lval->str = pstrdup(token);
		return FCONST;
	}
	lval->ival = val;
	return ICONST;
}

/*
 * addunicode
 *
 * Validate a Unicode codepoint and convert it to the server's encoding.
 * 验证 Unicode 码点并将其转换为服务器编码。
 *
 * 函数作用：
 * 验证指定的 Unicode 码位有效性，并通过外部回调 pg_unicode_to_server 将其转为
 * 服务器当前的字符编码格式（如 UTF-8），之后追加到 literalbuf 字面量缓冲区中。
 */
static void
addunicode(pg_wchar c, core_yyscan_t yyscanner)
{
	ScannerCallbackState scbstate;
	char		buf[MAX_UNICODE_EQUIVALENT_STRING + 1];

	if (!is_valid_unicode_codepoint(c))
		yyerror("invalid Unicode escape value");

	/*
	 * We expect that pg_unicode_to_server() will complain about any
	 * unconvertible code point, so we don't have to set saw_non_ascii.
	 * 我们期望 pg_unicode_to_server() 会对任何不可转换的码点发出警告，
	 * 因此我们不必设置 saw_non_ascii。
	 */
	setup_scanner_errposition_callback(&scbstate, yyscanner, *(yylloc));
	pg_unicode_to_server(c, (unsigned char *) buf);
	cancel_scanner_errposition_callback(&scbstate);
	addlit(buf, strlen(buf), yyscanner);
}

/*
 * unescape_single_char
 *
 * Unescape standard single character sequences.
 * 反转义标准的单字符序列。
 *
 * 函数作用：
 * 核心转义字符映射函数，用于将标准的转义（如 \b、\f、\n、\r、\t、\v）
 * 还原为真实的系统底层字符编码，如果遇到非 7位 ASCII 的非标准转义，标记 `saw_non_ascii` 为真。
 */
static unsigned char
unescape_single_char(unsigned char c, core_yyscan_t yyscanner)
{
	switch (c)
	{
		case 'b':
			return '\b';
		case 'f':
			return '\f';
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case 'v':
			return '\v';
		default:
			/* check for backslash followed by non-7-bit-ASCII
			 * 检查反斜杠后面是否跟有非 7位 ASCII 字符 */
			if (c == '\0' || IS_HIGHBIT_SET(c))
				yyextra->saw_non_ascii = true;

			return c;
	}
}

/*
 * check_string_escape_warning
 *
 * Checks if a string escape is nonstandard and issues warning if needed.
 * 检查字符串转义是否非标准，并根据需要发出警告。
 *
 * 函数作用：
 * 针对 SQL 语法中的非常规转义（例如转义单引号、反斜杠等）进行安全性/标准兼容性检验。
 * 根据 GUC 配置的 `escape_string_warning`，为每一条字符串只报出一次警告。
 */
static void
check_string_escape_warning(unsigned char ychar, core_yyscan_t yyscanner)
{
	if (ychar == '\'')
	{
		if (yyextra->warn_on_first_escape && yyextra->escape_string_warning)
			ereport(WARNING,
					(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
					 errmsg("nonstandard use of \\' in a string literal"),
					 errhint("Use '' to write quotes in strings, or use the escape string syntax (E'...')."),
					 lexer_errposition()));
		yyextra->warn_on_first_escape = false;	/* warn only once per string
											   每个字符串仅警告一次 */
	}
	else if (ychar == '\\')
	{
		if (yyextra->warn_on_first_escape && yyextra->escape_string_warning)
			ereport(WARNING,
					(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
					 errmsg("nonstandard use of \\\\ in a string literal"),
					 errhint("Use the escape string syntax for backslashes, e.g., E'\\\\'."),
					 lexer_errposition()));
		yyextra->warn_on_first_escape = false;	/* warn only once per string
										   每个字符串仅警告一次 */
	}
	else
		check_escape_warning(yyscanner);
}

/*
 * check_escape_warning
 *
 * Issues nonstandard escape warning for string literals.
 * 对字符串字面量发出非标准转义警告。
 *
 * 函数作用：
 * 针对常规的 Unicode 或其它非标准类型的字符串转义发出兼容性警告。
 */
static void
check_escape_warning(core_yyscan_t yyscanner)
{
	if (yyextra->warn_on_first_escape && yyextra->escape_string_warning)
		ereport(WARNING,
				(errcode(ERRCODE_NONSTANDARD_USE_OF_ESCAPE_CHARACTER),
				 errmsg("nonstandard use of escape in a string literal"),
				 errhint("Use the escape string syntax for escapes, e.g., E'\\r\\n'."),
				 lexer_errposition()));
	yyextra->warn_on_first_escape = false;	/* warn only once per string */
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 * 使 flex 使用 palloc() 而不是 malloc() 的接口函数。
 * 最好把这些设为 static，但 flex 坚持以其他方式进行。
 *
 * 函数作用：
 * 对 Flex 的内部内存管理进行重定义重载。
 * 用 PostgreSQL 的 `palloc`、`repalloc` 和 `pfree` 替换默认系统分配函数，
 * 从而确保 Flex 的临时内存和解析树的内存管理受控于 PostgreSQL 的事务/会话 MemoryContext 生命周期中。
 */

void *
core_yyalloc(yy_size_t bytes, core_yyscan_t yyscanner)
{
	return palloc(bytes);
}

void *
core_yyrealloc(void *ptr, yy_size_t bytes, core_yyscan_t yyscanner)
{
	if (ptr)
		return repalloc(ptr, bytes);
	else
		return palloc(bytes);
}

void
core_yyfree(void *ptr, core_yyscan_t yyscanner)
{
	if (ptr)
		pfree(ptr);
}
