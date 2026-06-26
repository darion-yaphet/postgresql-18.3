/*
 * This is a port of the Double Metaphone algorithm for use in PostgreSQL.
 *
 * 这是用于 PostgreSQL 的 Double Metaphone 算法的移植。
 *
 * contrib/fuzzystrmatch/dmetaphone.c
 *
 * Double Metaphone computes 2 "sounds like" strings - a primary and an
 * alternate. In most cases they are the same, but for foreign names
 * especially they can be a bit different, depending on pronunciation.
 *
 * Double Metaphone 计算 2 个“听起来像”字符串 - 一个主字符串和一个备用字符串。在大多数情况下，它们是相同的，但特别是对于外国名字，它们可能会有点不同，具体取决于发音。
 *
 * Information on using Double Metaphone can be found at
 *	 http://www.codeproject.com/string/dmetaphone1.asp
 * and the original article describing it can be found at
 *	 http://drdobbs.com/184401251
 *
 * 有关使用 Double Metaphone 的信息可以在 http://www.codeproject.com/string/dmetaphone1.asp 找到，描述它的原始文章可以在 http://drdobbs.com/184401251 找到
 *
 * For PostgreSQL we provide 2 functions - one for the primary and one for
 * the alternate. That way the functions are pure text->text mappings that
 * are useful in functional indexes. These are 'dmetaphone' for the
 * primary and 'dmetaphone_alt' for the alternate.
 *
 * 对于 PostgreSQL，我们提供 2 个函数 - 一个用于主函数，另一个用于备用函数。这样，函数就是纯文本->文本映射，这在函数索引中很有用。这些是用于主设备的“dmetaphone”和用于备用设备的“dmetaphone_alt”。
 *
 * Assuming that dmetaphone.so is in $libdir, the SQL to set up the
 * functions looks like this:
 *
 * 假设 dmetaphone.so 位于 $libdir 中，设置函数的 SQL 如下所示：
 *
 * CREATE FUNCTION dmetaphone (text) RETURNS text
 *	  LANGUAGE C IMMUTABLE STRICT
 *	  AS '$libdir/dmetaphone', 'dmetaphone';
 *
 * 创建函数 dmetaphone (text) 返回文本 LANGUAGE C IMMUTABLE STRICT AS '$libdir/dmetaphone', 'dmetaphone';
 *
 * CREATE FUNCTION dmetaphone_alt (text) RETURNS text
 *	  LANGUAGE C IMMUTABLE STRICT
 *	  AS '$libdir/dmetaphone', 'dmetaphone_alt';
 *
 * 创建函数 dmetaphone_alt (text) 返回文本 LANGUAGE C IMMUTABLE STRICT AS '$libdir/dmetaphone', 'dmetaphone_alt';
 *
 * Note that you have to declare the functions IMMUTABLE if you want to
 * use them in functional indexes, and you have to declare them as STRICT
 * as they do not check for NULL input, and will segfault if given NULL input.
 * (See below for alternative ) Declaring them as STRICT means PostgreSQL
 * will never call them with NULL, but instead assume the result is NULL,
 * which is what we (I) want.
 *
 * 请注意，如果要在函数索引中使用函数 IMMUTABLE，则必须声明它们，并且必须将它们声明为 STRICT，因为它们不检查 NULL 输入，并且如果给定 NULL 输入，则会出现段错误。 （参见下面的替代方案）将它们声明为 STRICT 意味着 PostgreSQL 永远不会使用 NULL 调用它们，而是假设结果为 NULL，这正是我们（我）想要的。
 *
 * Alternatively, compile with -DDMETAPHONE_NOSTRICT and the functions
 * will detect NULL input and return NULL. The you don't have to declare them
 * as STRICT.
 *
 * 或者，使用 -DDMETAPHONE_NOSTRICT 进行编译，函数将检测 NULL 输入并返回 NULL。您不必将它们声明为 STRICT。
 *
 * There is a small inefficiency here - each function call actually computes
 * both the primary and the alternate and then throws away the one it doesn't
 * need. That's the way the perl module was written, because perl can handle
 * a list return more easily than we can in PostgreSQL. The result has been
 * fast enough for my needs, but it could maybe be optimized a bit to remove
 * that behaviour.
 *
 * 这里有一点效率低下——每个函数调用实际上都会计算主要函数和备用函数，然后丢弃不需要的函数。这就是 perl 模块的编写方式，因为 perl 可以比 PostgreSQL 更轻松地处理列表返回。结果已经足够快满足我的需要，但它可能可以进行一些优化以消除这种行为。
 *
 */


/***************************** COPYRIGHT NOTICES ***********************

Most of this code is directly from the Text::DoubleMetaphone perl module
version 0.05 available from https://www.cpan.org/.
It bears this copyright notice:


  Copyright 2000, Maurice Aubrey <maurice@hevanet.com>.
  All rights reserved.

  This code is based heavily on the C++ implementation by
  Lawrence Philips and incorporates several bug fixes courtesy
  of Kevin Atkinson <kevina@users.sourceforge.net>.

  This module is free software; you may redistribute it and/or
  modify it under the same terms as Perl itself.

The remaining code is authored by Andrew Dunstan <amdunstan@ncshp.org> and
<andrew@dunslane.net> and is covered this copyright:

  Copyright 2003, North Carolina State Highway Patrol.
  All rights reserved.

  Permission to use, copy, modify, and distribute this software and its
  documentation for any purpose, without fee, and without a written agreement
  is hereby granted, provided that the above copyright notice and this
  paragraph and the following two paragraphs appear in all copies.

  IN NO EVENT SHALL THE NORTH CAROLINA STATE HIGHWAY PATROL BE LIABLE TO ANY
  PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
  INCLUDING LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
  DOCUMENTATION, EVEN IF THE NORTH CAROLINA STATE HIGHWAY PATROL HAS BEEN
  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  THE NORTH CAROLINA STATE HIGHWAY PATROL SPECIFICALLY DISCLAIMS ANY
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED
  HEREUNDER IS ON AN "AS IS" BASIS, AND THE NORTH CAROLINA STATE HIGHWAY PATROL
  HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
  MODIFICATIONS.

***********************************************************************/


/* include these first, according to the docs
 *
 * 根据文档，首先包括这些
 */
#ifndef DMETAPHONE_MAIN

#include "postgres.h"

#include "utils/builtins.h"

/* turn off assertions for embedded function
 *
 * 关闭嵌入函数的断言
 */
#define NDEBUG

#else							/* DMETAPHONE_MAIN */

/* we need these if we didn't get them from postgres.h
 *
 * 如果我们没有从 postgres.h 获取它们，我们就需要它们
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#endif							/* DMETAPHONE_MAIN */

#include <assert.h>
#include <ctype.h>

/* prototype for the main function we got from the perl module
 *
 * 我们从 perl 模块获得的 main 函数的原型
 */
static void DoubleMetaphone(char *str, char **codes);

#ifndef DMETAPHONE_MAIN

/*
 * The PostgreSQL visible dmetaphone function.
 *
 * PostgreSQL可见dmetaphone函数。
 */

PG_FUNCTION_INFO_V1(dmetaphone);

Datum
dmetaphone(PG_FUNCTION_ARGS)
{
	text	   *arg;
	char	   *aptr,
			   *codes[2],
			   *code;

#ifdef DMETAPHONE_NOSTRICT
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
#endif
	arg = PG_GETARG_TEXT_PP(0);
	aptr = text_to_cstring(arg);

	DoubleMetaphone(aptr, codes);
	code = codes[0];
	if (!code)
		code = "";

	PG_RETURN_TEXT_P(cstring_to_text(code));
}

/*
 * The PostgreSQL visible dmetaphone_alt function.
 *
 * PostgreSQL可见dmetaphone_alt函数。
 */

PG_FUNCTION_INFO_V1(dmetaphone_alt);

Datum
dmetaphone_alt(PG_FUNCTION_ARGS)
{
	text	   *arg;
	char	   *aptr,
			   *codes[2],
			   *code;

#ifdef DMETAPHONE_NOSTRICT
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
#endif
	arg = PG_GETARG_TEXT_PP(0);
	aptr = text_to_cstring(arg);

	DoubleMetaphone(aptr, codes);
	code = codes[1];
	if (!code)
		code = "";

	PG_RETURN_TEXT_P(cstring_to_text(code));
}


/* here is where we start the code imported from the perl module
 *
 * 这是我们开始从 perl 模块导入代码的地方
 */

/* all memory handling is done with these macros
 *
 * 所有内存处理都是通过这些宏完成的
 */

#define META_MALLOC(v,n,t) \
		  (v = (t*)palloc(((n)*sizeof(t))))

#define META_REALLOC(v,n,t) \
					  (v = (t*)repalloc((v),((n)*sizeof(t))))

/*
 * Don't do pfree - it seems to cause a SIGSEGV sometimes - which might have just
 * been caused by reloading the module in development.
 * So we rely on context cleanup - Tom Lane says pfree shouldn't be necessary
 * in a case like this.
 *
 * 不要执行 pfree - 有时它似乎会导致 SIGSEGV - 这可能是由于在开发中重新加载模块而引起的。所以我们依赖于上下文清理 - Tom Lane 说在这种情况下 pfree 不是必需的。
 */

#define META_FREE(x) ((void)true)	/* pfree((x)) */
#else							/* not defined DMETAPHONE_MAIN */

/* use the standard malloc library when not running in PostgreSQL
 *
 * 不在 PostgreSQL 中运行时使用标准 malloc 库
 */

#define META_MALLOC(v,n,t) \
		  (v = (t*)malloc(((n)*sizeof(t))))

#define META_REALLOC(v,n,t) \
					  (v = (t*)realloc((v),((n)*sizeof(t))))

#define META_FREE(x) free((x))
#endif							/* defined DMETAPHONE_MAIN */



/* this typedef was originally in the perl module's .h file
 *
 * 这个 typedef 最初位于 perl 模块的 .h 文件中
 */

typedef struct
{
	char	   *str;
	int			length;
	int			bufsize;
	int			free_string_on_destroy;
}

metastring;

/*
 * remaining perl module funcs unchanged except for declaring them static
 * and reformatting to PostgreSQL indentation and to fit in 80 cols.
 *
 * 其余的 Perl 模块功能保持不变，除了将它们声明为静态并重新格式化为 PostgreSQL 缩进并适应 80 列之外。
 *
 */

static metastring *
NewMetaString(const char *init_str)
{
	metastring *s;
	char		empty_string[] = "";

	META_MALLOC(s, 1, metastring);
	assert(s != NULL);

	if (init_str == NULL)
		init_str = empty_string;
	s->length = strlen(init_str);
	/* preallocate a bit more for potential growth
	 *
	 * 为潜在增长预先分配更多资金
	 */
	s->bufsize = s->length + 7;

	META_MALLOC(s->str, s->bufsize, char);
	assert(s->str != NULL);

	memcpy(s->str, init_str, s->length + 1);
	s->free_string_on_destroy = 1;

	return s;
}


static void
DestroyMetaString(metastring *s)
{
	if (s == NULL)
		return;

	if (s->free_string_on_destroy && (s->str != NULL))
		META_FREE(s->str);

	META_FREE(s);
}


static void
IncreaseBuffer(metastring *s, int chars_needed)
{
	META_REALLOC(s->str, (s->bufsize + chars_needed + 10), char);
	assert(s->str != NULL);
	s->bufsize = s->bufsize + chars_needed + 10;
}


static void
MakeUpper(metastring *s)
{
	char	   *i;

	for (i = s->str; *i; i++)
		*i = toupper((unsigned char) *i);
}


static int
IsVowel(metastring *s, int pos)
{
	char		c;

	if ((pos < 0) || (pos >= s->length))
		return 0;

	c = *(s->str + pos);
	if ((c == 'A') || (c == 'E') || (c == 'I') || (c == 'O') ||
		(c == 'U') || (c == 'Y'))
		return 1;

	return 0;
}


static int
SlavoGermanic(metastring *s)
{
	if (strstr(s->str, "W"))
		return 1;
	else if (strstr(s->str, "K"))
		return 1;
	else if (strstr(s->str, "CZ"))
		return 1;
	else if (strstr(s->str, "WITZ"))
		return 1;
	else
		return 0;
}


static char
GetAt(metastring *s, int pos)
{
	if ((pos < 0) || (pos >= s->length))
		return '\0';

	return ((char) *(s->str + pos));
}


static void
SetAt(metastring *s, int pos, char c)
{
	if ((pos < 0) || (pos >= s->length))
		return;

	*(s->str + pos) = c;
}


/*
   Caveats: the START value is 0 based
 *
 * 注意事项：START 值是从 0 开始的
*/
static int
StringAt(metastring *s, int start, int length,...)
{
	char	   *test;
	char	   *pos;
	va_list		ap;

	if ((start < 0) || (start >= s->length))
		return 0;

	pos = (s->str + start);
	va_start(ap, length);

	do
	{
		test = va_arg(ap, char *);
		if (*test && (strncmp(pos, test, length) == 0))
		{
			va_end(ap);
			return 1;
		}
	}
	while (strcmp(test, "") != 0);

	va_end(ap);

	return 0;
}


static void
MetaphAdd(metastring *s, const char *new_str)
{
	int			add_length;

	if (new_str == NULL)
		return;

	add_length = strlen(new_str);
	if ((s->length + add_length) > (s->bufsize - 1))
		IncreaseBuffer(s, add_length);

	strcat(s->str, new_str);
	s->length += add_length;
}


static void
DoubleMetaphone(char *str, char **codes)
{
	int			length;
	metastring *original;
	metastring *primary;
	metastring *secondary;
	int			current;
	int			last;

	current = 0;
	/* we need the real length and last prior to padding
	 *
	 * 我们需要填充之前的真实长度和最后长度
	 */
	length = strlen(str);
	last = length - 1;
	original = NewMetaString(str);
	/* Pad original so we can index beyond end
	 *
	 * 填充原始内容，以便我们可以索引超出末尾
	 */
	MetaphAdd(original, "     ");

	primary = NewMetaString("");
	secondary = NewMetaString("");
	primary->free_string_on_destroy = 0;
	secondary->free_string_on_destroy = 0;

	MakeUpper(original);

	/* skip these when at start of word
	 *
	 * 在单词开头时跳过这些
	 */
	if (StringAt(original, 0, 2, "GN", "KN", "PN", "WR", "PS", ""))
		current += 1;

	/* Initial 'X' is pronounced 'Z' e.g. 'Xavier'
	 *
	 * 首字母“X”发音为“Z”，例如“泽维尔”
	 */
	if (GetAt(original, 0) == 'X')
	{
		MetaphAdd(primary, "S");	/* 'Z' maps to 'S' */
		MetaphAdd(secondary, "S");
		current += 1;
	}

	/* main loop
	 *
	 * 主循环
	 */
	while ((primary->length < 4) || (secondary->length < 4))
	{
		if (current >= length)
			break;

		switch (GetAt(original, current))
		{
			case 'A':
			case 'E':
			case 'I':
			case 'O':
			case 'U':
			case 'Y':
				if (current == 0)
				{
					/* all init vowels now map to 'A'
					 *
					 * 所有起始元音现在映射到“A”
					 */
					MetaphAdd(primary, "A");
					MetaphAdd(secondary, "A");
				}
				current += 1;
				break;

			case 'B':

				/* "-mb", e.g", "dumb", already skipped over...
				 *
				 * “-mb”，例如“，”愚蠢的“，已经跳过了......
				 */
				MetaphAdd(primary, "P");
				MetaphAdd(secondary, "P");

				if (GetAt(original, current + 1) == 'B')
					current += 2;
				else
					current += 1;
				break;

			case '\xc7':		/* C with cedilla */
				MetaphAdd(primary, "S");
				MetaphAdd(secondary, "S");
				current += 1;
				break;

			case 'C':
				/* various germanic
				 *
				 * 各种日耳曼语
				 */
				if ((current > 1)
					&& !IsVowel(original, current - 2)
					&& StringAt(original, (current - 1), 3, "ACH", "")
					&& ((GetAt(original, current + 2) != 'I')
						&& ((GetAt(original, current + 2) != 'E')
							|| StringAt(original, (current - 2), 6, "BACHER",
										"MACHER", ""))))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "K");
					current += 2;
					break;
				}

				/* special case 'caesar'
				 *
				 * 特殊情况“凯撒”
				 */
				if ((current == 0)
					&& StringAt(original, current, 6, "CAESAR", ""))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "S");
					current += 2;
					break;
				}

				/* italian 'chianti'
				 *
				 * 意大利语“基安蒂”
				 */
				if (StringAt(original, current, 4, "CHIA", ""))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "K");
					current += 2;
					break;
				}

				if (StringAt(original, current, 2, "CH", ""))
				{
					/* find 'michael'
					 *
					 * 找到“迈克尔”
					 */
					if ((current > 0)
						&& StringAt(original, current, 4, "CHAE", ""))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "X");
						current += 2;
						break;
					}

					/* greek roots e.g. 'chemistry', 'chorus'
					 *
					 * 希腊词根，例如“化学”、“合唱”
					 */
					if ((current == 0)
						&& (StringAt(original, (current + 1), 5,
									 "HARAC", "HARIS", "")
							|| StringAt(original, (current + 1), 3, "HOR",
										"HYM", "HIA", "HEM", ""))
						&& !StringAt(original, 0, 5, "CHORE", ""))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
						current += 2;
						break;
					}

					/* germanic, greek, or otherwise 'ch' for 'kh' sound
					 *
					 * 日耳曼语、希腊语或其他“ch”代表“kh”音
					 */
					if ((StringAt(original, 0, 4, "VAN ", "VON ", "")
						 || StringAt(original, 0, 3, "SCH", ""))
					/* 'architect but not 'arch', 'orchestra', 'orchid'
					 *
					 * “建筑师”，但不是“拱门”、“管弦乐队”、“兰花”
					 */
						|| StringAt(original, (current - 2), 6, "ORCHES",
									"ARCHIT", "ORCHID", "")
						|| StringAt(original, (current + 2), 1, "T", "S",
									"")
						|| ((StringAt(original, (current - 1), 1,
									  "A", "O", "U", "E", "")
							 || (current == 0))

					/*
					 * e.g., 'wachtler', 'wechsler', but not 'tichner'
					 *
					 * 例如，“wachtler”、“wechsler”，但不是“tichner”
					 */
							&& StringAt(original, (current + 2), 1, "L", "R",
										"N", "M", "B", "H", "F", "V", "W",
										" ", "")))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
					}
					else
					{
						if (current > 0)
						{
							if (StringAt(original, 0, 2, "MC", ""))
							{
								/* e.g., "McHugh"
								 *
								 * 例如，“麦克休”
								 */
								MetaphAdd(primary, "K");
								MetaphAdd(secondary, "K");
							}
							else
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "K");
							}
						}
						else
						{
							MetaphAdd(primary, "X");
							MetaphAdd(secondary, "X");
						}
					}
					current += 2;
					break;
				}
				/* e.g, 'czerny'
				 *
				 * 例如，“车尔尼”
				 */
				if (StringAt(original, current, 2, "CZ", "")
					&& !StringAt(original, (current - 2), 4, "WICZ", ""))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "X");
					current += 2;
					break;
				}

				/* e.g., 'focaccia'
				 *
				 * 例如，“佛卡夏”
				 */
				if (StringAt(original, (current + 1), 3, "CIA", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "X");
					current += 3;
					break;
				}

				/* double 'C', but not if e.g. 'McClellan'
				 *
				 * 双“C”，但如果例如“麦克莱伦”
				 */
				if (StringAt(original, current, 2, "CC", "")
					&& !((current == 1) && (GetAt(original, 0) == 'M')))
				{
					/* 'bellocchio' but not 'bacchus'
					 *
					 * “bellocchio”但不是“bacchus”
					 */
					if (StringAt(original, (current + 2), 1, "I", "E", "H", "")
						&& !StringAt(original, (current + 2), 2, "HU", ""))
					{
						/* 'accident', 'accede' 'succeed'
						 *
						 * ‘意外’、‘接受’、‘成功’
						 */
						if (((current == 1)
							 && (GetAt(original, current - 1) == 'A'))
							|| StringAt(original, (current - 1), 5, "UCCEE",
										"UCCES", ""))
						{
							MetaphAdd(primary, "KS");
							MetaphAdd(secondary, "KS");
							/* 'bacci', 'bertucci', other italian
							 *
							 * 'bacci'、'bertucci'、其他意大利语
							 */
						}
						else
						{
							MetaphAdd(primary, "X");
							MetaphAdd(secondary, "X");
						}
						current += 3;
						break;
					}
					else
					{			/* Pierce's rule */
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
						current += 2;
						break;
					}
				}

				if (StringAt(original, current, 2, "CK", "CG", "CQ", ""))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "K");
					current += 2;
					break;
				}

				if (StringAt(original, current, 2, "CI", "CE", "CY", ""))
				{
					/* italian vs. english
					 *
					 * 意大利语与英语
					 */
					if (StringAt
						(original, current, 3, "CIO", "CIE", "CIA", ""))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "X");
					}
					else
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
					}
					current += 2;
					break;
				}

				/* else */
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");

				/* name sent in 'mac caffrey', 'mac gregor
				 *
				 * 名称以“mac caffrey”、“mac gregor”形式发送
				 */
				if (StringAt(original, (current + 1), 2, " C", " Q", " G", ""))
					current += 3;
				else if (StringAt(original, (current + 1), 1, "C", "K", "Q", "")
						 && !StringAt(original, (current + 1), 2,
									  "CE", "CI", ""))
					current += 2;
				else
					current += 1;
				break;

			case 'D':
				if (StringAt(original, current, 2, "DG", ""))
				{
					if (StringAt(original, (current + 2), 1,
								 "I", "E", "Y", ""))
					{
						/* e.g. 'edge'
						 *
						 * 例如'边缘'
						 */
						MetaphAdd(primary, "J");
						MetaphAdd(secondary, "J");
						current += 3;
						break;
					}
					else
					{
						/* e.g. 'edgar'
						 *
						 * 例如'埃德加'
						 */
						MetaphAdd(primary, "TK");
						MetaphAdd(secondary, "TK");
						current += 2;
						break;
					}
				}

				if (StringAt(original, current, 2, "DT", "DD", ""))
				{
					MetaphAdd(primary, "T");
					MetaphAdd(secondary, "T");
					current += 2;
					break;
				}

				/* else */
				MetaphAdd(primary, "T");
				MetaphAdd(secondary, "T");
				current += 1;
				break;

			case 'F':
				if (GetAt(original, current + 1) == 'F')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "F");
				MetaphAdd(secondary, "F");
				break;

			case 'G':
				if (GetAt(original, current + 1) == 'H')
				{
					if ((current > 0) && !IsVowel(original, current - 1))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
						current += 2;
						break;
					}

					if (current < 3)
					{
						/* 'ghislane', ghiradelli
						 *
						 * “吉斯兰”，吉拉德利
						 */
						if (current == 0)
						{
							if (GetAt(original, current + 2) == 'I')
							{
								MetaphAdd(primary, "J");
								MetaphAdd(secondary, "J");
							}
							else
							{
								MetaphAdd(primary, "K");
								MetaphAdd(secondary, "K");
							}
							current += 2;
							break;
						}
					}

					/*
					 * Parker's rule (with some further refinements) - e.g.,
					 * 'hugh'
					 *
					 * 帕克规则（有一些进一步的改进） - 例如，“休”
					 */
					if (((current > 1)
						 && StringAt(original, (current - 2), 1,
									 "B", "H", "D", ""))
					/* e.g., 'bough'
					 *
					 * 例如，“树枝”
					 */
						|| ((current > 2)
							&& StringAt(original, (current - 3), 1,
										"B", "H", "D", ""))
					/* e.g., 'broughton'
					 *
					 * 例如，“布劳顿”
					 */
						|| ((current > 3)
							&& StringAt(original, (current - 4), 1,
										"B", "H", "")))
					{
						current += 2;
						break;
					}
					else
					{
						/*
						 * e.g., 'laugh', 'McLaughlin', 'cough', 'gough',
						 * 'rough', 'tough'
						 *
						 * 例如，“笑”、“麦克劳克林”、“咳嗽”、“咳嗽”、“粗暴”、“强硬”
						 */
						if ((current > 2)
							&& (GetAt(original, current - 1) == 'U')
							&& StringAt(original, (current - 3), 1, "C",
										"G", "L", "R", "T", ""))
						{
							MetaphAdd(primary, "F");
							MetaphAdd(secondary, "F");
						}
						else if ((current > 0)
								 && GetAt(original, current - 1) != 'I')
						{


							MetaphAdd(primary, "K");
							MetaphAdd(secondary, "K");
						}

						current += 2;
						break;
					}
				}

				if (GetAt(original, current + 1) == 'N')
				{
					if ((current == 1) && IsVowel(original, 0)
						&& !SlavoGermanic(original))
					{
						MetaphAdd(primary, "KN");
						MetaphAdd(secondary, "N");
					}
					else
						/* not e.g. 'cagney'
						 *
						 * 不是例如“卡格尼”
						 */
						if (!StringAt(original, (current + 2), 2, "EY", "")
							&& (GetAt(original, current + 1) != 'Y')
							&& !SlavoGermanic(original))
					{
						MetaphAdd(primary, "N");
						MetaphAdd(secondary, "KN");
					}
					else
					{
						MetaphAdd(primary, "KN");
						MetaphAdd(secondary, "KN");
					}
					current += 2;
					break;
				}

				/* 'tagliaro'
				 *
				 * '塔利亚罗'
				 */
				if (StringAt(original, (current + 1), 2, "LI", "")
					&& !SlavoGermanic(original))
				{
					MetaphAdd(primary, "KL");
					MetaphAdd(secondary, "L");
					current += 2;
					break;
				}

				/* -ges-,-gep-,-gel-, -gie- at beginning
				 *
				 * -ges-、-gep-、-gel-、-gie- 开头
				 */
				if ((current == 0)
					&& ((GetAt(original, current + 1) == 'Y')
						|| StringAt(original, (current + 1), 2, "ES", "EP",
									"EB", "EL", "EY", "IB", "IL", "IN", "IE",
									"EI", "ER", "")))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "J");
					current += 2;
					break;
				}

				/* -ger-,  -gy-
				 *
				 * -ger-、-gy-
				 *
				 * -ger-、-gy-
				 */
				if ((StringAt(original, (current + 1), 2, "ER", "")
					 || (GetAt(original, current + 1) == 'Y'))
					&& !StringAt(original, 0, 6,
								 "DANGER", "RANGER", "MANGER", "")
					&& !StringAt(original, (current - 1), 1, "E", "I", "")
					&& !StringAt(original, (current - 1), 3, "RGY", "OGY", ""))
				{
					MetaphAdd(primary, "K");
					MetaphAdd(secondary, "J");
					current += 2;
					break;
				}

				/* italian e.g, 'biaggi'
				 *
				 * 意大利语，例如“biaggi”
				 */
				if (StringAt(original, (current + 1), 1, "E", "I", "Y", "")
					|| StringAt(original, (current - 1), 4,
								"AGGI", "OGGI", ""))
				{
					/* obvious germanic
					 *
					 * 明显的日耳曼语
					 */
					if ((StringAt(original, 0, 4, "VAN ", "VON ", "")
						 || StringAt(original, 0, 3, "SCH", ""))
						|| StringAt(original, (current + 1), 2, "ET", ""))
					{
						MetaphAdd(primary, "K");
						MetaphAdd(secondary, "K");
					}
					else
					{
						/* always soft if french ending
						 *
						 * 如果是法式结局，总是很柔和
						 */
						if (StringAt
							(original, (current + 1), 4, "IER ", ""))
						{
							MetaphAdd(primary, "J");
							MetaphAdd(secondary, "J");
						}
						else
						{
							MetaphAdd(primary, "J");
							MetaphAdd(secondary, "K");
						}
					}
					current += 2;
					break;
				}

				if (GetAt(original, current + 1) == 'G')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");
				break;

			case 'H':
				/* only keep if first & before vowel or btw. 2 vowels
				 *
				 * 仅保留第一个和之前的元音或顺便说一句。 2个元音
				 */
				if (((current == 0) || IsVowel(original, current - 1))
					&& IsVowel(original, current + 1))
				{
					MetaphAdd(primary, "H");
					MetaphAdd(secondary, "H");
					current += 2;
				}
				else
					/* also takes care of 'HH'
					 *
					 * 还照顾“HH”
					 */
					current += 1;
				break;

			case 'J':
				/* obvious spanish, 'jose', 'san jacinto'
				 *
				 * 明显的西班牙语，“何塞”，“圣哈辛托”
				 */
				if (StringAt(original, current, 4, "JOSE", "")
					|| StringAt(original, 0, 4, "SAN ", ""))
				{
					if (((current == 0)
						 && (GetAt(original, current + 4) == ' '))
						|| StringAt(original, 0, 4, "SAN ", ""))
					{
						MetaphAdd(primary, "H");
						MetaphAdd(secondary, "H");
					}
					else
					{
						MetaphAdd(primary, "J");
						MetaphAdd(secondary, "H");
					}
					current += 1;
					break;
				}

				if ((current == 0)
					&& !StringAt(original, current, 4, "JOSE", ""))
				{
					MetaphAdd(primary, "J");	/* Yankelovich/Jankelowicz */
					MetaphAdd(secondary, "A");
				}
				else
				{
					/* spanish pron. of e.g. 'bajador'
					 *
					 * 西班牙语代词。例如'巴哈多尔'
					 */
					if (IsVowel(original, current - 1)
						&& !SlavoGermanic(original)
						&& ((GetAt(original, current + 1) == 'A')
							|| (GetAt(original, current + 1) == 'O')))
					{
						MetaphAdd(primary, "J");
						MetaphAdd(secondary, "H");
					}
					else
					{
						if (current == last)
						{
							MetaphAdd(primary, "J");
							MetaphAdd(secondary, "");
						}
						else
						{
							if (!StringAt(original, (current + 1), 1, "L", "T",
										  "K", "S", "N", "M", "B", "Z", "")
								&& !StringAt(original, (current - 1), 1,
											 "S", "K", "L", ""))
							{
								MetaphAdd(primary, "J");
								MetaphAdd(secondary, "J");
							}
						}
					}
				}

				if (GetAt(original, current + 1) == 'J')	/* it could happen! */
					current += 2;
				else
					current += 1;
				break;

			case 'K':
				if (GetAt(original, current + 1) == 'K')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");
				break;

			case 'L':
				if (GetAt(original, current + 1) == 'L')
				{
					/* spanish e.g. 'cabrillo', 'gallegos'
					 *
					 * 西班牙语 例如“卡布里洛”、“加勒戈斯”
					 */
					if (((current == (length - 3))
						 && StringAt(original, (current - 1), 4, "ILLO",
									 "ILLA", "ALLE", ""))
						|| ((StringAt(original, (last - 1), 2, "AS", "OS", "")
							 || StringAt(original, last, 1, "A", "O", ""))
							&& StringAt(original, (current - 1), 4,
										"ALLE", "")))
					{
						MetaphAdd(primary, "L");
						MetaphAdd(secondary, "");
						current += 2;
						break;
					}
					current += 2;
				}
				else
					current += 1;
				MetaphAdd(primary, "L");
				MetaphAdd(secondary, "L");
				break;

			case 'M':
				if ((StringAt(original, (current - 1), 3, "UMB", "")
					 && (((current + 1) == last)
						 || StringAt(original, (current + 2), 2, "ER", "")))
				/* 'dumb','thumb'
				 *
				 * ‘笨蛋’、‘拇指’
				 */
					|| (GetAt(original, current + 1) == 'M'))
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "M");
				MetaphAdd(secondary, "M");
				break;

			case 'N':
				if (GetAt(original, current + 1) == 'N')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "N");
				MetaphAdd(secondary, "N");
				break;

			case '\xd1':		/* N with tilde */
				current += 1;
				MetaphAdd(primary, "N");
				MetaphAdd(secondary, "N");
				break;

			case 'P':
				if (GetAt(original, current + 1) == 'H')
				{
					MetaphAdd(primary, "F");
					MetaphAdd(secondary, "F");
					current += 2;
					break;
				}

				/* also account for "campbell", "raspberry"
				 *
				 * 还占“坎贝尔”、“覆盆子”
				 */
				if (StringAt(original, (current + 1), 1, "P", "B", ""))
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "P");
				MetaphAdd(secondary, "P");
				break;

			case 'Q':
				if (GetAt(original, current + 1) == 'Q')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "K");
				MetaphAdd(secondary, "K");
				break;

			case 'R':
				/* french e.g. 'rogier', but exclude 'hochmeier'
				 *
				 * 法语 例如“rogier”，但排除“hochmeier”
				 */
				if ((current == last)
					&& !SlavoGermanic(original)
					&& StringAt(original, (current - 2), 2, "IE", "")
					&& !StringAt(original, (current - 4), 2, "ME", "MA", ""))
				{
					MetaphAdd(primary, "");
					MetaphAdd(secondary, "R");
				}
				else
				{
					MetaphAdd(primary, "R");
					MetaphAdd(secondary, "R");
				}

				if (GetAt(original, current + 1) == 'R')
					current += 2;
				else
					current += 1;
				break;

			case 'S':
				/* special cases 'island', 'isle', 'carlisle', 'carlysle'
				 *
				 * 特殊情况 'island'、'isle'、'carlisle'、'carlysle'
				 */
				if (StringAt(original, (current - 1), 3, "ISL", "YSL", ""))
				{
					current += 1;
					break;
				}

				/* special case 'sugar-'
				 *
				 * 特殊情况“糖-”
				 */
				if ((current == 0)
					&& StringAt(original, current, 5, "SUGAR", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "S");
					current += 1;
					break;
				}

				if (StringAt(original, current, 2, "SH", ""))
				{
					/* germanic */
					if (StringAt
						(original, (current + 1), 4, "HEIM", "HOEK", "HOLM",
						 "HOLZ", ""))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
					}
					else
					{
						MetaphAdd(primary, "X");
						MetaphAdd(secondary, "X");
					}
					current += 2;
					break;
				}

				/* italian & armenian
				 *
				 * 意大利语和亚美尼亚语
				 */
				if (StringAt(original, current, 3, "SIO", "SIA", "")
					|| StringAt(original, current, 4, "SIAN", ""))
				{
					if (!SlavoGermanic(original))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "X");
					}
					else
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
					}
					current += 3;
					break;
				}

				/*
				 * german & anglicisations, e.g. 'smith' match 'schmidt',
				 * 'snider' match 'schneider' also, -sz- in slavic language
				 * although in hungarian it is pronounced 's'
				 *
				 * 德语和英语化，例如'smith' 匹配 'schmidt'，'snider' 也匹​​配 'schneider'，斯拉夫语中的 -sz- 尽管在匈牙利语中发音为 's'
				 */
				if (((current == 0)
					 && StringAt(original, (current + 1), 1,
								 "M", "N", "L", "W", ""))
					|| StringAt(original, (current + 1), 1, "Z", ""))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "X");
					if (StringAt(original, (current + 1), 1, "Z", ""))
						current += 2;
					else
						current += 1;
					break;
				}

				if (StringAt(original, current, 2, "SC", ""))
				{
					/* Schlesinger's rule
					 *
					 * 施莱辛格法则
					 */
					if (GetAt(original, current + 2) == 'H')
					{
						/* dutch origin, e.g. 'school', 'schooner'
						 *
						 * 荷兰血统，例如“学校”、“帆船”
						 */
						if (StringAt(original, (current + 3), 2,
									 "OO", "ER", "EN",
									 "UY", "ED", "EM", ""))
						{
							/* 'schermerhorn', 'schenker'
							 *
							 * '谢默霍恩', '申克'
							 */
							if (StringAt(original, (current + 3), 2,
										 "ER", "EN", ""))
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "SK");
							}
							else
							{
								MetaphAdd(primary, "SK");
								MetaphAdd(secondary, "SK");
							}
							current += 3;
							break;
						}
						else
						{
							if ((current == 0) && !IsVowel(original, 3)
								&& (GetAt(original, 3) != 'W'))
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "S");
							}
							else
							{
								MetaphAdd(primary, "X");
								MetaphAdd(secondary, "X");
							}
							current += 3;
							break;
						}
					}

					if (StringAt(original, (current + 2), 1,
								 "I", "E", "Y", ""))
					{
						MetaphAdd(primary, "S");
						MetaphAdd(secondary, "S");
						current += 3;
						break;
					}
					/* else */
					MetaphAdd(primary, "SK");
					MetaphAdd(secondary, "SK");
					current += 3;
					break;
				}

				/* french e.g. 'resnais', 'artois'
				 *
				 * 法语 例如“雷奈”、“阿图瓦”
				 */
				if ((current == last)
					&& StringAt(original, (current - 2), 2, "AI", "OI", ""))
				{
					MetaphAdd(primary, "");
					MetaphAdd(secondary, "S");
				}
				else
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "S");
				}

				if (StringAt(original, (current + 1), 1, "S", "Z", ""))
					current += 2;
				else
					current += 1;
				break;

			case 'T':
				if (StringAt(original, current, 4, "TION", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "X");
					current += 3;
					break;
				}

				if (StringAt(original, current, 3, "TIA", "TCH", ""))
				{
					MetaphAdd(primary, "X");
					MetaphAdd(secondary, "X");
					current += 3;
					break;
				}

				if (StringAt(original, current, 2, "TH", "")
					|| StringAt(original, current, 3, "TTH", ""))
				{
					/* special case 'thomas', 'thames' or germanic
					 *
					 * 特殊情况“托马斯”、“泰晤士”或日耳曼语
					 */
					if (StringAt(original, (current + 2), 2, "OM", "AM", "")
						|| StringAt(original, 0, 4, "VAN ", "VON ", "")
						|| StringAt(original, 0, 3, "SCH", ""))
					{
						MetaphAdd(primary, "T");
						MetaphAdd(secondary, "T");
					}
					else
					{
						MetaphAdd(primary, "0");
						MetaphAdd(secondary, "T");
					}
					current += 2;
					break;
				}

				if (StringAt(original, (current + 1), 1, "T", "D", ""))
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "T");
				MetaphAdd(secondary, "T");
				break;

			case 'V':
				if (GetAt(original, current + 1) == 'V')
					current += 2;
				else
					current += 1;
				MetaphAdd(primary, "F");
				MetaphAdd(secondary, "F");
				break;

			case 'W':
				/* can also be in middle of word
				 *
				 * 也可以在单词中间
				 */
				if (StringAt(original, current, 2, "WR", ""))
				{
					MetaphAdd(primary, "R");
					MetaphAdd(secondary, "R");
					current += 2;
					break;
				}

				if ((current == 0)
					&& (IsVowel(original, current + 1)
						|| StringAt(original, current, 2, "WH", "")))
				{
					/* Wasserman should match Vasserman
					 *
					 * 瓦瑟曼应该匹配瓦瑟曼
					 */
					if (IsVowel(original, current + 1))
					{
						MetaphAdd(primary, "A");
						MetaphAdd(secondary, "F");
					}
					else
					{
						/* need Uomo to match Womo
						 *
						 * 需要 Uomo 来匹配 Womo
						 */
						MetaphAdd(primary, "A");
						MetaphAdd(secondary, "A");
					}
				}

				/* Arnow should match Arnoff
				 *
				 * 阿诺应该与阿诺夫匹配
				 */
				if (((current == last) && IsVowel(original, current - 1))
					|| StringAt(original, (current - 1), 5, "EWSKI", "EWSKY",
								"OWSKI", "OWSKY", "")
					|| StringAt(original, 0, 3, "SCH", ""))
				{
					MetaphAdd(primary, "");
					MetaphAdd(secondary, "F");
					current += 1;
					break;
				}

				/* polish e.g. 'filipowicz'
				 *
				 * 波兰语例如'菲利波维奇'
				 */
				if (StringAt(original, current, 4, "WICZ", "WITZ", ""))
				{
					MetaphAdd(primary, "TS");
					MetaphAdd(secondary, "FX");
					current += 4;
					break;
				}

				/* else skip it
				 *
				 * 否则跳过它
				 */
				current += 1;
				break;

			case 'X':
				/* french e.g. breaux
				 *
				 * 法语 例如布劳克斯
				 */
				if (!((current == last)
					  && (StringAt(original, (current - 3), 3,
								   "IAU", "EAU", "")
						  || StringAt(original, (current - 2), 2,
									  "AU", "OU", ""))))
				{
					MetaphAdd(primary, "KS");
					MetaphAdd(secondary, "KS");
				}


				if (StringAt(original, (current + 1), 1, "C", "X", ""))
					current += 2;
				else
					current += 1;
				break;

			case 'Z':
				/* chinese pinyin e.g. 'zhao'
				 *
				 * 汉语拼音 例如‘赵’
				 */
				if (GetAt(original, current + 1) == 'H')
				{
					MetaphAdd(primary, "J");
					MetaphAdd(secondary, "J");
					current += 2;
					break;
				}
				else if (StringAt(original, (current + 1), 2,
								  "ZO", "ZI", "ZA", "")
						 || (SlavoGermanic(original)
							 && ((current > 0)
								 && GetAt(original, current - 1) != 'T')))
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "TS");
				}
				else
				{
					MetaphAdd(primary, "S");
					MetaphAdd(secondary, "S");
				}

				if (GetAt(original, current + 1) == 'Z')
					current += 2;
				else
					current += 1;
				break;

			default:
				current += 1;
		}

		/*
		 * printf("PRIMARY: %s\n", primary->str); printf("SECONDARY: %s\n",
		 * secondary->str);
		 *
		 * printf("主：%s\n", 主->str); printf("二级: %s\n", 二级->str);
		 */
	}


	if (primary->length > 4)
		SetAt(primary, 4, '\0');

	if (secondary->length > 4)
		SetAt(secondary, 4, '\0');

	*codes = primary->str;
	*++codes = secondary->str;

	DestroyMetaString(original);
	DestroyMetaString(primary);
	DestroyMetaString(secondary);
}

#ifdef DMETAPHONE_MAIN

/* just for testing - not part of the perl code
 *
 * 仅用于测试 - 不是 Perl 代码的一部分
 */

main(int argc, char **argv)
{
	char	   *codes[2];

	if (argc > 1)
	{
		DoubleMetaphone(argv[1], codes);
		printf("%s|%s\n", codes[0], codes[1]);
	}
}

#endif
