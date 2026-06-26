/*
 * Daitch-Mokotoff Soundex
 *
 * Copyright (c) 2023-2025, PostgreSQL Global Development Group
 *
 * This module was originally sponsored by Finance Norway /
 * Trafikkforsikringsforeningen, and implemented by Dag Lem <dag@nimrod.no>
 *
 * The implementation of the Daitch-Mokotoff Soundex System aims at correctness
 * and high performance, and can be summarized as follows:
 *
 * - The processing of each phoneme is initiated by an O(1) table lookup.
 * - For phonemes containing more than one character, a coding tree is traversed
 *   to process the complete phoneme.
 * - The (alternate) soundex codes are produced digit by digit in-place in
 *   another tree structure.
 *
 * References:
 *
 * https://www.avotaynu.com/soundex.htm
 * https://www.jewishgen.org/InfoFiles/Soundex.html
 * https://familypedia.fandom.com/wiki/Daitch-Mokotoff_Soundex
 * https://stevemorse.org/census/soundex.html (dmlat.php, dmsoundex.php)
 * https://github.com/apache/commons-codec/ (dmrules.txt, DaitchMokotoffSoundex.java)
 * https://metacpan.org/pod/Text::Phonetic (DaitchMokotoff.pm)
 *
 * A few notes on other implementations:
 *
 * - All other known implementations have the same unofficial rules for "UE",
 *   these are also adapted by this implementation (0, 1, NC).
 * - The only other known implementation which is capable of generating all
 *   correct soundex codes in all cases is the JOS Soundex Calculator at
 *   https://www.jewishgen.org/jos/jossound.htm
 * - "J" is considered (only) a vowel in dmlat.php
 * - The official rules for "RS" are commented out in dmlat.php
 * - Identical code digits for adjacent letters are not collapsed correctly in
 *   dmsoundex.php when double digit codes are involved. E.g. "BESST" yields
 *   744300 instead of 743000 as for "BEST".
 * - "J" is considered (only) a consonant in DaitchMokotoffSoundex.java
 * - "Y" is not considered a vowel in DaitchMokotoffSoundex.java
*/

#include "postgres.h"

#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


/*
 * The soundex coding chart table is adapted from
 * https://www.jewishgen.org/InfoFiles/Soundex.html
 * See daitch_mokotoff_header.pl for details.
 *
 * soundex 编码表改编自 https://www.jewishgen.org/InfoFiles/Soundex.html 详细信息请参阅 daitch_mokotoff_header.pl。
*/

/* Generated coding chart table
 *
 * 生成的编码图表表
 */
#include "daitch_mokotoff.h"

#define DM_CODE_DIGITS 6

/* Node in soundex code tree
 *
 * soundex 代码树中的节点
 */
typedef struct dm_node
{
	int			soundex_length; /* Length of generated soundex code */
	char		soundex[DM_CODE_DIGITS];	/* Soundex code */
	int			is_leaf;		/* Candidate for complete soundex code */
	int			last_update;	/* Letter number for last update of node */
	char		code_digit;		/* Last code digit, 0 - 9 */

	/*
	 * One or two alternate code digits leading to this node. If there are two
	 * digits, one of them is always an 'X'. Repeated code digits and 'X' lead
	 * back to the same node.
	 *
	 * 通向该节点的一或两个备用代码数字。如果有两位数字，其中一位始终是“X”。重复的代码数字和“X”会返回同一节点。
	 */
	char		prev_code_digits[2];
	/* One or two alternate code digits moving forward.
	 *
	 * 向前移动一或两个备用代码数字。
	 */
	char		next_code_digits[2];
	/* ORed together code index(es) used to reach current node.
	 *
	 * 将用于到达当前节点的代码索引组合在一起。
	 */
	int			prev_code_index;
	int			next_code_index;
	/* Possible nodes branching out from this node - digits 0-9.
	 *
	 * 从该节点分支出的可能节点 - 数字 0-9。
	 */
	struct dm_node *children[10];
	/* Next node in linked list. Alternating index for each iteration.
	 *
	 * 链表中的下一个节点。每次迭代的交替索引。
	 */
	struct dm_node *next[2];
} dm_node;

/* Template for new node in soundex code tree.
 *
 * soundex 代码树中新节点的模板。
 */
static const dm_node start_node = {
	.soundex_length = 0,
	.soundex = "000000",		/* Six digits */
	.is_leaf = 0,
	.last_update = 0,
	.code_digit = '\0',
	.prev_code_digits = {'\0', '\0'},
	.next_code_digits = {'\0', '\0'},
	.prev_code_index = 0,
	.next_code_index = 0,
	.children = {NULL},
	.next = {NULL}
};

/* Dummy soundex codes at end of input.
 *
 * 输入末尾的虚拟 soundex 代码。
 */
static const dm_codes end_codes[2] =
{
	{
		"X", "X", "X"
	}
};

/* Mapping from ISO8859-1 to upper-case ASCII, covering the range 0x60..0xFF.
 *
 * 从 ISO8859-1 映射到大写 ASCII，覆盖范围 0x60..0xFF。
 */
static const char iso8859_1_to_ascii_upper[] =
"`ABCDEFGHIJKLMNOPQRSTUVWXYZ{|}~                                  !                             ?AAAAAAECEEEEIIIIDNOOOOO*OUUUUYDSAAAAAAECEEEEIIIIDNOOOOO/OUUUUYDY";

/* Internal C implementation
 *
 * 内部C实现
 */
static bool daitch_mokotoff_coding(const char *word, ArrayBuildState *soundex);


PG_FUNCTION_INFO_V1(daitch_mokotoff);

Datum
daitch_mokotoff(PG_FUNCTION_ARGS)
{
	text	   *arg = PG_GETARG_TEXT_PP(0);
	Datum		retval;
	char	   *string;
	ArrayBuildState *soundex;
	MemoryContext old_ctx,
				tmp_ctx;

	/* Work in a temporary context to simplify cleanup.
	 *
	 * 在临时环境中工作以简化清理工作。
	 */
	tmp_ctx = AllocSetContextCreate(CurrentMemoryContext,
									"daitch_mokotoff temporary context",
									ALLOCSET_DEFAULT_SIZES);
	old_ctx = MemoryContextSwitchTo(tmp_ctx);

	/* We must convert the string to UTF-8 if it isn't already.
	 *
	 * 如果字符串尚未转换为 UTF-8，我们必须将其转换为 UTF-8。
	 */
	string = pg_server_to_any(text_to_cstring(arg), VARSIZE_ANY_EXHDR(arg),
							  PG_UTF8);

	/* The result is built in this ArrayBuildState.
	 *
	 * 结果构建在此 ArrayBuildState 中。
	 */
	soundex = initArrayResult(TEXTOID, tmp_ctx, false);

	if (!daitch_mokotoff_coding(string, soundex))
	{
		/* No encodable characters in input
		 *
		 * 输入中没有可编码的字符
		 */
		MemoryContextSwitchTo(old_ctx);
		MemoryContextDelete(tmp_ctx);
		PG_RETURN_NULL();
	}

	retval = makeArrayResult(soundex, old_ctx);

	MemoryContextSwitchTo(old_ctx);
	MemoryContextDelete(tmp_ctx);

	PG_RETURN_DATUM(retval);
}


/* Initialize soundex code tree node for next code digit.
 *
 * 初始化下一个代码数字的 soundex 代码树节点。
 */
static void
initialize_node(dm_node *node, int last_update)
{
	if (node->last_update < last_update)
	{
		node->prev_code_digits[0] = node->next_code_digits[0];
		node->prev_code_digits[1] = node->next_code_digits[1];
		node->next_code_digits[0] = '\0';
		node->next_code_digits[1] = '\0';
		node->prev_code_index = node->next_code_index;
		node->next_code_index = 0;
		node->is_leaf = 0;
		node->last_update = last_update;
	}
}


/* Update soundex code tree node with next code digit.
 *
 * 使用下一个代码数字更新 soundex 代码树节点。
 */
static void
add_next_code_digit(dm_node *node, int code_index, char code_digit)
{
	/* OR in index 1 or 2.
	 *
	 * OR 在索引 1 或 2 中。
	 */
	node->next_code_index |= code_index;

	if (!node->next_code_digits[0])
		node->next_code_digits[0] = code_digit;
	else if (node->next_code_digits[0] != code_digit)
		node->next_code_digits[1] = code_digit;
}


/* Mark soundex code tree node as leaf.
 *
 * 将 soundex 代码树节点标记为叶子。
 */
static void
set_leaf(dm_node *first_node[2], dm_node *last_node[2],
		 dm_node *node, int ix_node)
{
	if (!node->is_leaf)
	{
		node->is_leaf = 1;

		if (first_node[ix_node] == NULL)
			first_node[ix_node] = node;
		else
			last_node[ix_node]->next[ix_node] = node;

		last_node[ix_node] = node;
		node->next[ix_node] = NULL;
	}
}


/* Find next node corresponding to code digit, or create a new node.
 *
 * 查找代码位对应的下一个节点，或者创建一个新节点。
 */
static dm_node *
find_or_create_child_node(dm_node *parent, char code_digit,
						  ArrayBuildState *soundex)
{
	int			i = code_digit - '0';
	dm_node   **nodes = parent->children;
	dm_node    *node = nodes[i];

	if (node)
	{
		/* Found existing child node. Skip completed nodes.
		 *
		 * 找到现有的子节点。跳过已完成的节点。
		 */
		return node->soundex_length < DM_CODE_DIGITS ? node : NULL;
	}

	/* Create new child node.
	 *
	 * 创建新的子节点。
	 */
	node = palloc_object(dm_node);
	nodes[i] = node;

	*node = start_node;
	memcpy(node->soundex, parent->soundex, sizeof(parent->soundex));
	node->soundex_length = parent->soundex_length;
	node->soundex[node->soundex_length++] = code_digit;
	node->code_digit = code_digit;
	node->next_code_index = node->prev_code_index;

	if (node->soundex_length < DM_CODE_DIGITS)
	{
		return node;
	}
	else
	{
		/* Append completed soundex code to output array.
		 *
		 * 将完成的 soundex 代码附加到输出数组。
		 */
		text	   *out = cstring_to_text_with_len(node->soundex,
												   DM_CODE_DIGITS);

		accumArrayResult(soundex,
						 PointerGetDatum(out),
						 false,
						 TEXTOID,
						 CurrentMemoryContext);
		return NULL;
	}
}


/* Update node for next code digit(s).
 *
 * 更新下一个代码数字的节点。
 */
static void
update_node(dm_node *first_node[2], dm_node *last_node[2],
			dm_node *node, int ix_node,
			int letter_no, int prev_code_index, int next_code_index,
			const char *next_code_digits, int digit_no,
			ArrayBuildState *soundex)
{
	int			i;
	char		next_code_digit = next_code_digits[digit_no];
	int			num_dirty_nodes = 0;
	dm_node    *dirty_nodes[2];

	initialize_node(node, letter_no);

	if (node->prev_code_index && !(node->prev_code_index & prev_code_index))
	{
		/*
		 * If the sound (vowel / consonant) of this letter encoding doesn't
		 * correspond to the coding index of the previous letter, we skip this
		 * letter encoding. Note that currently, only "J" can be either a
		 * vowel or a consonant.
		 *
		 * 如果这个字母编码的声音（元音/辅音）与前一个字母的编码索引不对应，我们就跳过这个字母编码。请注意，目前只有“J”可以是元音或辅音。
		 */
		return;
	}

	if (next_code_digit == 'X' ||
		(digit_no == 0 &&
		 (node->prev_code_digits[0] == next_code_digit ||
		  node->prev_code_digits[1] == next_code_digit)))
	{
		/* The code digit is the same as one of the previous (i.e. not added).
		 *
		 * 代码数字与前面的数字相同（即未添加）。
		 */
		dirty_nodes[num_dirty_nodes++] = node;
	}

	if (next_code_digit != 'X' &&
		(digit_no > 0 ||
		 node->prev_code_digits[0] != next_code_digit ||
		 node->prev_code_digits[1]))
	{
		/* The code digit is different from one of the previous (i.e. added).
		 *
		 * 代码数字与之前的数字不同（即添加的）。
		 */
		node = find_or_create_child_node(node, next_code_digit, soundex);
		if (node)
		{
			initialize_node(node, letter_no);
			dirty_nodes[num_dirty_nodes++] = node;
		}
	}

	for (i = 0; i < num_dirty_nodes; i++)
	{
		/* Add code digit leading to the current node.
		 *
		 * 添加通向当前节点的代码位。
		 */
		add_next_code_digit(dirty_nodes[i], next_code_index, next_code_digit);

		if (next_code_digits[++digit_no])
		{
			update_node(first_node, last_node, dirty_nodes[i], ix_node,
						letter_no, prev_code_index, next_code_index,
						next_code_digits, digit_no,
						soundex);
		}
		else
		{
			/* Add incomplete leaf node to linked list.
			 *
			 * 将不完整的叶节点添加到链表中。
			 */
			set_leaf(first_node, last_node, dirty_nodes[i], ix_node);
		}
	}
}


/* Update soundex tree leaf nodes.
 *
 * 更新 soundex 树的叶子节点。
 */
static void
update_leaves(dm_node *first_node[2], int *ix_node, int letter_no,
			  const dm_codes *codes, const dm_codes *next_codes,
			  ArrayBuildState *soundex)
{
	int			i,
				j,
				code_index;
	dm_node    *node,
			   *last_node[2];
	const dm_code *code,
			   *next_code;
	int			ix_node_next = (*ix_node + 1) & 1;	/* Alternating index: 0, 1 */

	/* Initialize for new linked list of leaves.
	 *
	 * 初始化新的叶子链接列表。
	 */
	first_node[ix_node_next] = NULL;
	last_node[ix_node_next] = NULL;

	/* Process all nodes.
	 *
	 * 处理所有节点。
	 */
	for (node = first_node[*ix_node]; node; node = node->next[*ix_node])
	{
		/* One or two alternate code sequences.
		 *
		 * 一两个备用代码序列。
		 */
		for (i = 0; i < 2 && (code = codes[i]) && code[0][0]; i++)
		{
			/* Coding for previous letter - before vowel: 1, all other: 2
			 *
			 * 前一个字母的编码 - 元音之前：1，所有其他：2
			 */
			int			prev_code_index = (code[0][0] > '1') + 1;

			/* One or two alternate next code sequences.
			 *
			 * 一两个交替的下一个代码序列。
			 */
			for (j = 0; j < 2 && (next_code = next_codes[j]) && next_code[0][0]; j++)
			{
				/* Determine which code to use.
				 *
				 * 确定使用哪个代码。
				 */
				if (letter_no == 0)
				{
					/* This is the first letter.
					 *
					 * 这是第一封信。
					 */
					code_index = 0;
				}
				else if (next_code[0][0] <= '1')
				{
					/* The next letter is a vowel.
					 *
					 * 下一个字母是元音。
					 */
					code_index = 1;
				}
				else
				{
					/* All other cases.
					 *
					 * 所有其他情况。
					 */
					code_index = 2;
				}

				/* One or two sequential code digits.
				 *
				 * 一个或两个连续的代码数字。
				 */
				update_node(first_node, last_node, node, ix_node_next,
							letter_no, prev_code_index, code_index,
							code[code_index], 0,
							soundex);
			}
		}
	}

	*ix_node = ix_node_next;
}


/*
 * Return next character, converted from UTF-8 to uppercase ASCII.
 * *ix is the current string index and is incremented by the character length.
 *
 * 返回下一个字符，从 UTF-8 转换为大写 ASCII。 *ix 是当前字符串索引，并按字符长度递增。
 */
static char
read_char(const unsigned char *str, int *ix)
{
	/* Substitute character for skipped code points.
	 *
	 * 用字符替换跳过的代码点。
	 */
	const char	na = '\x1a';
	pg_wchar	c;

	/* Decode UTF-8 character to ISO 10646 code point.
	 *
	 * 将 UTF-8 字符解码为 ISO 10646 代码点。
	 */
	str += *ix;
	c = utf8_to_unicode(str);

	/* Advance *ix, but (for safety) not if we've reached end of string.
	 *
	 * 提前*ix，但是（为了安全）如果我们已经到达字符串末尾则不要。
	 */
	if (c)
		*ix += pg_utf_mblen(str);

	/* Convert. */
	if (c >= (unsigned char) '[' && c <= (unsigned char) ']')
	{
		/* ASCII characters [, \, and ] are reserved for conversions below.
		 *
		 * ASCII 字符 [、\ 和 ] 保留用于以下转换。
		 */
		return na;
	}
	else if (c < 0x60)
	{
		/* Other non-lowercase ASCII characters can be used as-is.
		 *
		 * 其他非小写 ASCII 字符可以按原样使用。
		 */
		return (char) c;
	}
	else if (c < 0x100)
	{
		/* ISO-8859-1 code point; convert to upper-case ASCII via table.
		 *
		 * ISO-8859-1 代码点；通过表格转换为大写 ASCII。
		 */
		return iso8859_1_to_ascii_upper[c - 0x60];
	}
	else
	{
		/* Conversion of non-ASCII characters in the coding chart.
		 *
		 * 编码表中非 ASCII 字符的转换。
		 */
		switch (c)
		{
			case 0x0104:		/* LATIN CAPITAL LETTER A WITH OGONEK */
			case 0x0105:		/* LATIN SMALL LETTER A WITH OGONEK */
				return '[';
			case 0x0118:		/* LATIN CAPITAL LETTER E WITH OGONEK */
			case 0x0119:		/* LATIN SMALL LETTER E WITH OGONEK */
				return '\\';
			case 0x0162:		/* LATIN CAPITAL LETTER T WITH CEDILLA */
			case 0x0163:		/* LATIN SMALL LETTER T WITH CEDILLA */
			case 0x021A:		/* LATIN CAPITAL LETTER T WITH COMMA BELOW */
			case 0x021B:		/* LATIN SMALL LETTER T WITH COMMA BELOW */
				return ']';
			default:
				return na;
		}
	}
}


/* Read next ASCII character, skipping any characters not in [A-\]].
 *
 * 读取下一个 ASCII 字符，跳过 [A-\]] 之外的任何字符。
 */
static char
read_valid_char(const char *str, int *ix)
{
	char		c;

	while ((c = read_char((const unsigned char *) str, ix)) != '\0')
	{
		if (c >= 'A' && c <= ']')
			break;
	}

	return c;
}


/* Return sound coding for "letter" (letter sequence)
 *
 * 返回“字母”的声音编码（字母序列）
 */
static const dm_codes *
read_letter(const char *str, int *ix)
{
	char		c,
				cmp;
	int			i,
				j;
	const dm_letter *letters;
	const dm_codes *codes;

	/* First letter in sequence.
	 *
	 * 按顺序排列第一个字母。
	 */
	if ((c = read_valid_char(str, ix)) == '\0')
		return NULL;

	letters = &letter_[c - 'A'];
	codes = letters->codes;
	i = *ix;

	/* Any subsequent letters in sequence.
	 *
	 * 按顺序排列的任何后续字母。
	 */
	while ((letters = letters->letters) && (c = read_valid_char(str, &i)))
	{
		for (j = 0; (cmp = letters[j].letter); j++)
		{
			if (cmp == c)
			{
				/* Letter found.
				 *
				 * 信找到了。
				 */
				letters = &letters[j];
				if (letters->codes)
				{
					/* Coding for letter sequence found.
					 *
					 * 找到字母序列的编码。
					 */
					codes = letters->codes;
					*ix = i;
				}
				break;
			}
		}
		if (!cmp)
		{
			/* The sequence of letters has no coding.
			 *
			 * 字母序列没有编码。
			 */
			break;
		}
	}

	return codes;
}


/*
 * Generate all Daitch-Mokotoff soundex codes for word,
 * adding them to the "soundex" ArrayBuildState.
 * Returns false if string has no encodable characters, else true.
 *
 * 生成单词的所有 Daitch-Mokotoff soundex 代码，将它们添加到“soundex”ArrayBuildState 中。如果字符串没有可编码字符，则返回 false，否则返回 true。
 */
static bool
daitch_mokotoff_coding(const char *word, ArrayBuildState *soundex)
{
	int			i = 0;
	int			letter_no = 0;
	int			ix_node = 0;
	const dm_codes *codes,
			   *next_codes;
	dm_node    *first_node[2],
			   *node;

	/* First letter.
	 *
	 * 第一个信。
	 */
	if (!(codes = read_letter(word, &i)))
	{
		/* No encodable character in input.
		 *
		 * 输入中没有可编码字符。
		 */
		return false;
	}

	/* Starting point.
	 *
	 * 起点。
	 */
	first_node[ix_node] = palloc_object(dm_node);
	*first_node[ix_node] = start_node;

	/*
	 * Loop until either the word input is exhausted, or all generated soundex
	 * codes are completed to six digits.
	 *
	 * 循环直到单词输入用完，或者所有生成的 soundex 代码都完成为六位数字。
	 */
	while (codes && first_node[ix_node])
	{
		next_codes = read_letter(word, &i);

		/* Update leaf nodes.
		 *
		 * 更新叶节点。
		 */
		update_leaves(first_node, &ix_node, letter_no,
					  codes, next_codes ? next_codes : end_codes,
					  soundex);

		codes = next_codes;
		letter_no++;
	}

	/* Append all remaining (incomplete) soundex codes to output array.
	 *
	 * 将所有剩余的（不完整的）soundex 代码附加到输出数组。
	 */
	for (node = first_node[ix_node]; node; node = node->next[ix_node])
	{
		text	   *out = cstring_to_text_with_len(node->soundex,
												   DM_CODE_DIGITS);

		accumArrayResult(soundex,
						 PointerGetDatum(out),
						 false,
						 TEXTOID,
						 CurrentMemoryContext);
	}

	return true;
}
