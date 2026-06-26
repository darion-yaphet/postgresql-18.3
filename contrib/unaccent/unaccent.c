/*-------------------------------------------------------------------------
 *
 * unaccent.c
 *	  Text search unaccent dictionary
 *
 * Copyright (c) 2009-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/unaccent/unaccent.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_ts_dict.h"
#include "commands/defrem.h"
#include "lib/stringinfo.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC_EXT(
					.name = "unaccent",
					.version = PG_VERSION
);

/*
 * An unaccent dictionary uses a trie to find a string to replace.  Each node
 * of the trie is an array of 256 TrieChar structs; the N-th element of the
 * array corresponds to next byte value N.  That element can contain both a
 * replacement string (to be used if the source string ends with this byte)
 * and a link to another trie node (to be followed if there are more bytes).
 *
 * 非重音字典使用 trie 来查找要替换的字符串。  trie 的每个节点都是一个由 256 个 TrieChar 结构体组成的数组；数组的第 N 个元素对应于下一个字节值 N。该元素可以包含替换字符串（如果源字符串以此字节结尾则使用）和到另一个 trie 节点的链接（如果有更多字节则要跟随）。
 *
 * Note that the trie search logic pays no attention to multibyte character
 * boundaries.  This is OK as long as both the data entered into the trie and
 * the data we're trying to look up are validly encoded; no partial-character
 * matches will occur.
 *
 * 请注意，trie 搜索逻辑不关注多字节字符边界。  只要输入到 trie 的数据和我们尝试查找的数据都经过有效编码，就可以了；不会发生部分字符匹配。
 */
typedef struct TrieChar
{
	struct TrieChar *nextChar;
	char	   *replaceTo;
	int			replacelen;
} TrieChar;

/*
 * placeChar - put str into trie's structure, byte by byte.
 *
 * placeChar - 将 str 逐字节放入 trie 结构中。
 *
 * If node is NULL, we need to make a new node, which will be returned;
 * otherwise the return value is the same as node.
 *
 * 如果节点为NULL，我们需要创建一个新节点，该节点将被返回；否则返回值与node相同。
 */
static TrieChar *
placeChar(TrieChar *node, const unsigned char *str, int lenstr,
		  const char *replaceTo, int replacelen)
{
	TrieChar   *curnode;

	if (!node)
		node = (TrieChar *) palloc0(sizeof(TrieChar) * 256);

	Assert(lenstr > 0);			/* else str[0] doesn't exist */

	curnode = node + *str;

	if (lenstr <= 1)
	{
		if (curnode->replaceTo)
			ereport(WARNING,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("duplicate source strings, first one will be used")));
		else
		{
			curnode->replacelen = replacelen;
			curnode->replaceTo = (char *) palloc(replacelen);
			memcpy(curnode->replaceTo, replaceTo, replacelen);
		}
	}
	else
	{
		curnode->nextChar = placeChar(curnode->nextChar, str + 1, lenstr - 1,
									  replaceTo, replacelen);
	}

	return node;
}

/*
 * initTrie  - create trie from file.
 *
 * initTrie - 从文件创建 trie。
 *
 * Function converts UTF8-encoded file into current encoding.
 *
 * 函数将UTF8编码的文件转换为当前编码。
 */
static TrieChar *
initTrie(const char *filename)
{
	TrieChar   *volatile rootTrie = NULL;
	MemoryContext ccxt = CurrentMemoryContext;
	tsearch_readline_state trst;
	volatile bool skip;

	filename = get_tsearch_config_filename(filename, "rules");
	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open unaccent file \"%s\": %m",
						filename)));

	do
	{
		/*
		 * pg_do_encoding_conversion() (called by tsearch_readline()) will
		 * emit exception if it finds untranslatable characters in current
		 * locale. We just skip such lines, continuing with the next.
		 *
		 * 如果 pg_do_encoding_conversion() （由 tsearch_readline() 调用）在当前语言环境中发现不可翻译的字符，则会发出异常。我们只是跳过这些行，继续下一行。
		 */
		skip = true;

		PG_TRY();
		{
			char	   *line;

			while ((line = tsearch_readline(&trst)) != NULL)
			{
				/*----------
				 * The format of each line must be "src" or "src trg", where
				 * src and trg are sequences of one or more non-whitespace
				 * characters, separated by whitespace.  Whitespace at start
				 * or end of line is ignored.  If trg is omitted, an empty
				 * string is used as the replacement.  trg can be optionally
				 * quoted, in which case whitespaces are included in it.
				 *
				 * 每行的格式必须为“src”或“src trg”，其中 src 和 trg 是一个或多个非空白字符的序列，以空格分隔。  行首或行尾的空格将被忽略。  如果省略 trg，则使用空字符串作为替换。  trg 可以选择被引用，在这种情况下，其中包含空格。
				 *
				 * We use a simple state machine, with states
				 *	0	initial (before src)
				 *	1	in src
				 *	2	in whitespace after src
				 *	3	in trg (non-quoted)
				 *	4	in trg (quoted)
				 *	5	in whitespace after trg
				 *	-1	syntax error detected (two strings)
				 *	-2	syntax error detected (unfinished quoted string)
				 *
				 * 我们使用一个简单的状态机，状态 0 初始（在 src 之前） 1 在 src 中 2 在 src 之后的空白中 3 在 trg 中（未加引号） 4 在 trg 中（加引号） 5 在 trg 后的空白中 -1 检测到语法错误（两个字符串） -2 检测到语法错误（未完成的引用字符串）
				 *----------
				 */
				int			state;
				char	   *ptr;
				char	   *src = NULL;
				char	   *trg = NULL;
				char	   *trgstore = NULL;
				int			ptrlen;
				int			srclen = 0;
				int			trglen = 0;
				int			trgstorelen = 0;
				bool		trgquoted = false;

				state = 0;
				for (ptr = line; *ptr; ptr += ptrlen)
				{
					ptrlen = pg_mblen_cstr(ptr);
					/* ignore whitespace, but end src or trg
					 *
					 * 忽略空格，但结束 src 或 trg
					 */
					if (isspace((unsigned char) *ptr))
					{
						if (state == 1)
							state = 2;
						else if (state == 3)
							state = 5;
						/* whitespaces are OK in quoted area
						 *
						 * 引用区域中的空格是可以的
						 */
						if (state != 4)
							continue;
					}
					switch (state)
					{
						case 0:
							/* start of src
							 *
							 * src 的开始
							 */
							src = ptr;
							srclen = ptrlen;
							state = 1;
							break;
						case 1:
							/* continue src
							 *
							 * 继续源代码
							 */
							srclen += ptrlen;
							break;
						case 2:
							/* start of trg
							 *
							 * trg 的开始
							 */
							if (*ptr == '"')
							{
								trgquoted = true;
								state = 4;
							}
							else
								state = 3;

							trg = ptr;
							trglen = ptrlen;
							break;
						case 3:
							/* continue non-quoted trg
							 *
							 * 继续未引用的 trg
							 */
							trglen += ptrlen;
							break;
						case 4:
							/* continue quoted trg
							 *
							 * 继续引用 trg
							 */
							trglen += ptrlen;

							/*
							 * If this is a quote, consider it as the end of
							 * trg except if the follow-up character is itself
							 * a quote.
							 *
							 * 如果这是一个引号，则将其视为 trg 的结尾，除非后续字符本身就是一个引号。
							 */
							if (*ptr == '"')
							{
								if (*(ptr + 1) == '"')
								{
									ptr++;
									trglen += 1;
								}
								else
									state = 5;
							}
							break;
						default:
							/* bogus line format
							 *
							 * 伪行格式
							 */
							state = -1;
							break;
					}
				}

				if (state == 1 || state == 2)
				{
					/* trg was omitted, so use ""
					 *
					 * trg 被省略，所以使用“”
					 */
					trg = "";
					trglen = 0;
				}

				/* If still in a quoted area, fallback to an error */
				/*
				 * 如果仍在引用区域中，则回退到错误
				 */
				if (state == 4)
					state = -2;

				/* If trg was quoted, remove its quotes and unescape it
				 *
				 * 如果 trg 被引用，则删除其引号并取消转义
				 */
				if (trgquoted && state > 0)
				{
					/* Ignore first and end quotes
					 *
					 * 忽略首引号和尾引号
					 */
					trgstore = (char *) palloc(sizeof(char) * (trglen - 2));
					trgstorelen = 0;
					for (int i = 1; i < trglen - 1; i++)
					{
						trgstore[trgstorelen] = trg[i];
						trgstorelen++;
						/* skip second double quotes
						 *
						 * 跳过第二个双引号
						 */
						if (trg[i] == '"' && trg[i + 1] == '"')
							i++;
					}
				}
				else
				{
					trgstore = (char *) palloc(sizeof(char) * trglen);
					trgstorelen = trglen;
					memcpy(trgstore, trg, trgstorelen);
				}

				if (state > 0)
					rootTrie = placeChar(rootTrie,
										 (unsigned char *) src, srclen,
										 trgstore, trgstorelen);
				else if (state == -1)
					ereport(WARNING,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid syntax: more than two strings in unaccent rule")));
				else if (state == -2)
					ereport(WARNING,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid syntax: unfinished quoted string in unaccent rule")));

				pfree(trgstore);
				pfree(line);
			}
			skip = false;
		}
		PG_CATCH();
		{
			ErrorData  *errdata;
			MemoryContext ecxt;

			ecxt = MemoryContextSwitchTo(ccxt);
			errdata = CopyErrorData();
			if (errdata->sqlerrcode == ERRCODE_UNTRANSLATABLE_CHARACTER)
			{
				FlushErrorState();
			}
			else
			{
				MemoryContextSwitchTo(ecxt);
				PG_RE_THROW();
			}
		}
		PG_END_TRY();
	}
	while (skip);

	tsearch_readline_end(&trst);

	return rootTrie;
}

/*
 * findReplaceTo - find longest possible match in trie
 *
 * findReplaceTo - 在 trie 中查找最长的可能匹配
 *
 * On success, returns pointer to ending subnode, plus length of matched
 * source string in *p_matchlen.  On failure, returns NULL.
 *
 * 成功时，返回指向结束子节点的指针，加上 *p_matchlen 中匹配源字符串的长度。  失败时返回 NULL。
 */
static TrieChar *
findReplaceTo(TrieChar *node, const unsigned char *src, int srclen,
			  int *p_matchlen)
{
	TrieChar   *result = NULL;
	int			matchlen = 0;

	*p_matchlen = 0;			/* prevent uninitialized-variable warnings */

	while (node && matchlen < srclen)
	{
		node = node + src[matchlen];
		matchlen++;

		if (node->replaceTo)
		{
			result = node;
			*p_matchlen = matchlen;
		}

		node = node->nextChar;
	}

	return result;
}

PG_FUNCTION_INFO_V1(unaccent_init);
Datum
unaccent_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	TrieChar   *rootTrie = NULL;
	bool		fileloaded = false;
	ListCell   *l;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (strcmp(defel->defname, "rules") == 0)
		{
			if (fileloaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple Rules parameters")));
			rootTrie = initTrie(defGetString(defel));
			fileloaded = true;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized Unaccent parameter: \"%s\"",
							defel->defname)));
		}
	}

	if (!fileloaded)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing Rules parameter")));
	}

	PG_RETURN_POINTER(rootTrie);
}

PG_FUNCTION_INFO_V1(unaccent_lexize);
Datum
unaccent_lexize(PG_FUNCTION_ARGS)
{
	TrieChar   *rootTrie = (TrieChar *) PG_GETARG_POINTER(0);
	char	   *srcchar = (char *) PG_GETARG_POINTER(1);
	int32		len = PG_GETARG_INT32(2);
	char	   *srcstart = srcchar;
	const char *srcend = srcstart + len;
	TSLexeme   *res;
	StringInfoData buf;

	/* we allocate storage for the buffer only if needed
	 *
	 * 仅在需要时才为缓冲区分配存储空间
	 */
	buf.data = NULL;

	while (len > 0)
	{
		TrieChar   *node;
		int			matchlen;

		node = findReplaceTo(rootTrie, (unsigned char *) srcchar, len,
							 &matchlen);
		if (node && node->replaceTo)
		{
			if (buf.data == NULL)
			{
				/* initialize buffer
				 *
				 * 初始化缓冲区
				 */
				initStringInfo(&buf);
				/* insert any data we already skipped over
				 *
				 * 插入我们已经跳过的任何数据
				 */
				if (srcchar != srcstart)
					appendBinaryStringInfo(&buf, srcstart, srcchar - srcstart);
			}
			appendBinaryStringInfo(&buf, node->replaceTo, node->replacelen);
		}
		else
		{
			matchlen = pg_mblen_range(srcchar, srcend);
			if (buf.data != NULL)
				appendBinaryStringInfo(&buf, srcchar, matchlen);
		}

		srcchar += matchlen;
		len -= matchlen;
	}

	/* return a result only if we made at least one substitution
	 *
	 * 仅当我们至少进行一次替换时才返回结果
	 */
	if (buf.data != NULL)
	{
		res = (TSLexeme *) palloc0(sizeof(TSLexeme) * 2);
		res->lexeme = buf.data;
		res->flags = TSL_FILTER;
	}
	else
		res = NULL;

	PG_RETURN_POINTER(res);
}

/*
 * Function-like wrapper for dictionary
 *
 * 类似函数的字典包装器
 */
PG_FUNCTION_INFO_V1(unaccent_dict);
Datum
unaccent_dict(PG_FUNCTION_ARGS)
{
	text	   *str;
	int			strArg;
	Oid			dictOid;
	TSDictionaryCacheEntry *dict;
	TSLexeme   *res;

	if (PG_NARGS() == 1)
	{
		/*
		 * Use the "unaccent" dictionary that is in the same schema that this
		 * function is in.
		 *
		 * 使用与此函数处于同一架构中的“非重音”字典。
		 */
		Oid			procnspid = get_func_namespace(fcinfo->flinfo->fn_oid);
		const char *dictname = "unaccent";

		dictOid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
								  PointerGetDatum(dictname),
								  ObjectIdGetDatum(procnspid));
		if (!OidIsValid(dictOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("text search dictionary \"%s.%s\" does not exist",
							get_namespace_name(procnspid), dictname)));
		strArg = 0;
	}
	else
	{
		dictOid = PG_GETARG_OID(0);
		strArg = 1;
	}
	str = PG_GETARG_TEXT_PP(strArg);

	dict = lookup_ts_dictionary_cache(dictOid);

	res = (TSLexeme *) DatumGetPointer(FunctionCall4(&(dict->lexize),
													 PointerGetDatum(dict->dictData),
													 PointerGetDatum(VARDATA_ANY(str)),
													 Int32GetDatum(VARSIZE_ANY_EXHDR(str)),
													 PointerGetDatum(NULL)));

	PG_FREE_IF_COPY(str, strArg);

	if (res == NULL)
	{
		PG_RETURN_TEXT_P(PG_GETARG_TEXT_P_COPY(strArg));
	}
	else if (res->lexeme == NULL)
	{
		pfree(res);
		PG_RETURN_TEXT_P(PG_GETARG_TEXT_P_COPY(strArg));
	}
	else
	{
		text	   *txt = cstring_to_text(res->lexeme);

		pfree(res->lexeme);
		pfree(res);

		PG_RETURN_TEXT_P(txt);
	}
}
