/*
 * contrib/xml2/xpath.c
 *
 * Parser interface for DOM-based parser (libxml) rather than
 * stream-based SAX-type parser
 *
 * 基于 DOM 的解析器 (libxml) 的解析器接口，而不是基于流的 SAX 类型解析器
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/xml.h"

/* libxml includes
 *
 * libxml 包括
 */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlerror.h>
#include <libxml/parserInternals.h>

PG_MODULE_MAGIC_EXT(
					.name = "xml2",
					.version = PG_VERSION
);

/* exported for use by xslt_proc.c
 *
 * 导出供 xslt_proc.c 使用
 */

PgXmlErrorContext *pgxml_parser_init(PgXmlStrictness strictness);

/* workspace for pgxml_xpath()
 *
 * pgxml_xpath() 的工作空间
 */

typedef struct
{
	xmlDocPtr	doctree;
	xmlXPathContextPtr ctxt;
	xmlXPathObjectPtr res;
} xpath_workspace;

/* local declarations
 *
 * 本地声明
 */

static xmlChar *pgxmlNodeSetToText(xmlNodeSetPtr nodeset,
								   xmlChar *toptagname, xmlChar *septagname,
								   xmlChar *plainsep);

static text *pgxml_result_to_text(xmlXPathObjectPtr res, xmlChar *toptag,
								  xmlChar *septag, xmlChar *plainsep);

static xmlChar *pgxml_texttoxmlchar(text *textstring);

static xmlXPathObjectPtr pgxml_xpath(text *document, xmlChar *xpath,
									 xpath_workspace *workspace);

static void cleanup_workspace(xpath_workspace *workspace);


/*
 * Initialize for xml parsing.
 *
 * 初始化 xml 解析。
 *
 * As with the underlying pg_xml_init function, calls to this MUST be followed
 * by a PG_TRY block that guarantees that pg_xml_done is called.
 *
 * 与底层 pg_xml_init 函数一样，对此函数的调用后面必须跟有保证调用 pg_xml_done 的 PG_TRY 块。
 */
PgXmlErrorContext *
pgxml_parser_init(PgXmlStrictness strictness)
{
	PgXmlErrorContext *xmlerrcxt;

	/* Set up error handling (we share the core's error handler)
	 *
	 * 设置错误处理（我们共享核心的错误处理程序）
	 */
	xmlerrcxt = pg_xml_init(strictness);

	/* Note: we're assuming an elog cannot be thrown by the following calls
	 *
	 * 注意：我们假设以下调用无法抛出 elog
	 */

	/* Initialize libxml
	 *
	 * 初始化libxml
	 */
	xmlInitParser();

	return xmlerrcxt;
}


/* Encodes special characters (<, >, &, " and \r) as XML entities
 *
 * 将特殊字符（<、>、&、" 和 \r）编码为 XML 实体
 */

PG_FUNCTION_INFO_V1(xml_encode_special_chars);

Datum
xml_encode_special_chars(PG_FUNCTION_ARGS)
{
	text	   *tin = PG_GETARG_TEXT_PP(0);
	text	   *tout;
	xmlChar    *ts,
			   *tt;

	ts = pgxml_texttoxmlchar(tin);

	tt = xmlEncodeSpecialChars(NULL, ts);

	pfree(ts);

	tout = cstring_to_text((char *) tt);

	xmlFree(tt);

	PG_RETURN_TEXT_P(tout);
}

/*
 * Function translates a nodeset into a text representation
 *
 * 函数将节点集转换为文本表示
 *
 * iterates over each node in the set and calls xmlNodeDump to write it to
 * an xmlBuffer -from which an xmlChar * string is returned.
 *
 * 迭代集合中的每个节点并调用 xmlNodeDump 将其写入 xmlBuffer，从中返回 xmlChar * 字符串。
 *
 * each representation is surrounded by <tagname> ... </tagname>
 *
 * 每个表示都被 <tagname> ... </tagname> 包围
 *
 * plainsep is an ordinary (not tag) separator - if used, then nodes are
 * cast to string as output method
 *
 * plainsep 是一个普通（非标签）分隔符 - 如果使用，则节点将转换为字符串作为输出方法
 */
static xmlChar *
pgxmlNodeSetToText(xmlNodeSetPtr nodeset,
				   xmlChar *toptagname,
				   xmlChar *septagname,
				   xmlChar *plainsep)
{
	xmlBufferPtr buf;
	xmlChar    *result;
	int			i;

	buf = xmlBufferCreate();

	if ((toptagname != NULL) && (xmlStrlen(toptagname) > 0))
	{
		xmlBufferWriteChar(buf, "<");
		xmlBufferWriteCHAR(buf, toptagname);
		xmlBufferWriteChar(buf, ">");
	}
	if (nodeset != NULL)
	{
		for (i = 0; i < nodeset->nodeNr; i++)
		{
			if (plainsep != NULL)
			{
				xmlBufferWriteCHAR(buf,
								   xmlXPathCastNodeToString(nodeset->nodeTab[i]));

				/* If this isn't the last entry, write the plain sep.
				 *
				 * 如果这不是最后一个条目，请写出简单的 sep。
				 */
				if (i < (nodeset->nodeNr) - 1)
					xmlBufferWriteChar(buf, (char *) plainsep);
			}
			else
			{
				if ((septagname != NULL) && (xmlStrlen(septagname) > 0))
				{
					xmlBufferWriteChar(buf, "<");
					xmlBufferWriteCHAR(buf, septagname);
					xmlBufferWriteChar(buf, ">");
				}
				xmlNodeDump(buf,
							nodeset->nodeTab[i]->doc,
							nodeset->nodeTab[i],
							1, 0);

				if ((septagname != NULL) && (xmlStrlen(septagname) > 0))
				{
					xmlBufferWriteChar(buf, "</");
					xmlBufferWriteCHAR(buf, septagname);
					xmlBufferWriteChar(buf, ">");
				}
			}
		}
	}

	if ((toptagname != NULL) && (xmlStrlen(toptagname) > 0))
	{
		xmlBufferWriteChar(buf, "</");
		xmlBufferWriteCHAR(buf, toptagname);
		xmlBufferWriteChar(buf, ">");
	}
	result = xmlStrdup(xmlBufferContent(buf));
	xmlBufferFree(buf);
	return result;
}


/* Translate a PostgreSQL "varlena" -i.e. a variable length parameter
 * into the libxml2 representation
 *
 * 进入 libxml2 表示形式
 */
static xmlChar *
pgxml_texttoxmlchar(text *textstring)
{
	return (xmlChar *) text_to_cstring(textstring);
}

/* Publicly visible XPath functions
 *
 * 公开可见的 XPath 函数
 */

/*
 * This is a "raw" xpath function. Check that it returns child elements
 * properly
 *
 * 这是一个“原始”xpath 函数。检查它是否正确返回子元素
 */
PG_FUNCTION_INFO_V1(xpath_nodeset);

Datum
xpath_nodeset(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *toptag = pgxml_texttoxmlchar(PG_GETARG_TEXT_PP(2));
	xmlChar    *septag = pgxml_texttoxmlchar(PG_GETARG_TEXT_PP(3));
	xmlChar    *xpath;
	text	   *xpres;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	xpres = pgxml_result_to_text(res, toptag, septag, NULL);

	cleanup_workspace(&workspace);

	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}

/*
 * The following function is almost identical, but returns the elements in
 * a list.
 *
 * 以下函数几乎相同，但返回列表中的元素。
 */
PG_FUNCTION_INFO_V1(xpath_list);

Datum
xpath_list(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *plainsep = pgxml_texttoxmlchar(PG_GETARG_TEXT_PP(2));
	xmlChar    *xpath;
	text	   *xpres;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	xpres = pgxml_result_to_text(res, NULL, NULL, plainsep);

	cleanup_workspace(&workspace);

	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}


PG_FUNCTION_INFO_V1(xpath_string);

Datum
xpath_string(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *xpath;
	int32		pathsize;
	text	   *xpres;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	pathsize = VARSIZE_ANY_EXHDR(xpathsupp);

	/*
	 * We encapsulate the supplied path with "string()" = 8 chars + 1 for NUL
	 * at end
	 *
	 * 我们用“string()”封装提供的路径= 8个字符+ 1个NUL在末尾
	 */
	/* We could try casting to string using the libxml function?
	 *
	 * 我们可以尝试使用 libxml 函数转换为字符串吗？
	 */

	xpath = (xmlChar *) palloc(pathsize + 9);
	memcpy(xpath, "string(", 7);
	memcpy(xpath + 7, VARDATA_ANY(xpathsupp), pathsize);
	xpath[pathsize + 7] = ')';
	xpath[pathsize + 8] = '\0';

	res = pgxml_xpath(document, xpath, &workspace);

	xpres = pgxml_result_to_text(res, NULL, NULL, NULL);

	cleanup_workspace(&workspace);

	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}


PG_FUNCTION_INFO_V1(xpath_number);

Datum
xpath_number(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *xpath;
	float4		fRes;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	pfree(xpath);

	if (res == NULL)
		PG_RETURN_NULL();

	fRes = xmlXPathCastToNumber(res);

	cleanup_workspace(&workspace);

	if (xmlXPathIsNaN(fRes))
		PG_RETURN_NULL();

	PG_RETURN_FLOAT4(fRes);
}


PG_FUNCTION_INFO_V1(xpath_bool);

Datum
xpath_bool(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *xpath;
	int			bRes;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	pfree(xpath);

	if (res == NULL)
		PG_RETURN_BOOL(false);

	bRes = xmlXPathCastToBoolean(res);

	cleanup_workspace(&workspace);

	PG_RETURN_BOOL(bRes);
}



/* Core function to evaluate XPath query
 *
 * 评估 XPath 查询的核心函数
 */

static xmlXPathObjectPtr
pgxml_xpath(text *document, xmlChar *xpath, xpath_workspace *workspace)
{
	int32		docsize = VARSIZE_ANY_EXHDR(document);
	PgXmlErrorContext *xmlerrcxt;
	xmlXPathCompExprPtr comppath;

	workspace->doctree = NULL;
	workspace->ctxt = NULL;
	workspace->res = NULL;

	xmlerrcxt = pgxml_parser_init(PG_XML_STRICTNESS_LEGACY);

	PG_TRY();
	{
		workspace->doctree = xmlReadMemory((char *) VARDATA_ANY(document),
										   docsize, NULL, NULL,
										   XML_PARSE_NOENT);
		if (workspace->doctree != NULL)
		{
			workspace->ctxt = xmlXPathNewContext(workspace->doctree);
			workspace->ctxt->node = xmlDocGetRootElement(workspace->doctree);

			/* compile the path
			 *
			 * 编译路径
			 */
			comppath = xmlXPathCtxtCompile(workspace->ctxt, xpath);
			if (comppath == NULL)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_ARGUMENT_FOR_XQUERY,
							"XPath Syntax Error");

			/* Now evaluate the path expression.
			 *
			 * 现在评估路径表达式。
			 */
			workspace->res = xmlXPathCompiledEval(comppath, workspace->ctxt);

			xmlXPathFreeCompExpr(comppath);
		}
	}
	PG_CATCH();
	{
		cleanup_workspace(workspace);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (workspace->res == NULL)
		cleanup_workspace(workspace);

	pg_xml_done(xmlerrcxt, false);

	return workspace->res;
}

/* Clean up after processing the result of pgxml_xpath()
 *
 * 处理pgxml_xpath()的结果后进行清理
 */
static void
cleanup_workspace(xpath_workspace *workspace)
{
	if (workspace->res)
		xmlXPathFreeObject(workspace->res);
	workspace->res = NULL;
	if (workspace->ctxt)
		xmlXPathFreeContext(workspace->ctxt);
	workspace->ctxt = NULL;
	if (workspace->doctree)
		xmlFreeDoc(workspace->doctree);
	workspace->doctree = NULL;
}

static text *
pgxml_result_to_text(xmlXPathObjectPtr res,
					 xmlChar *toptag,
					 xmlChar *septag,
					 xmlChar *plainsep)
{
	xmlChar    *xpresstr;
	text	   *xpres;

	if (res == NULL)
		return NULL;

	switch (res->type)
	{
		case XPATH_NODESET:
			xpresstr = pgxmlNodeSetToText(res->nodesetval,
										  toptag,
										  septag, plainsep);
			break;

		case XPATH_STRING:
			xpresstr = xmlStrdup(res->stringval);
			break;

		default:
			elog(NOTICE, "unsupported XQuery result: %d", res->type);
			xpresstr = xmlStrdup((const xmlChar *) "<unsupported/>");
	}

	/* Now convert this result back to text
	 *
	 * 现在将此结果转换回文本
	 */
	xpres = cstring_to_text((char *) xpresstr);

	/* Free various storage
	 *
	 * 免费各种存储
	 */
	xmlFree(xpresstr);

	return xpres;
}

/*
 * xpath_table is a table function. It needs some tidying (as do the
 * other functions here!
 *
 * xpath_table 是一个表函数。它需要一些整理（就像这里的其他功能一样！）
 */
PG_FUNCTION_INFO_V1(xpath_table);

Datum
xpath_table(PG_FUNCTION_ARGS)
{
	/* Function parameters
	 *
	 * 功能参数
	 */
	char	   *pkeyfield = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *xmlfield = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *relname = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char	   *xpathset = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *condition = text_to_cstring(PG_GETARG_TEXT_PP(4));

	/* SPI (input tuple) support
	 *
	 * SPI（输入元组）支持
	 */
	SPITupleTable *tuptable;
	HeapTuple	spi_tuple;
	TupleDesc	spi_tupdesc;


	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	AttInMetadata *attinmeta;

	char	  **values;
	xmlChar   **xpaths;
	char	   *pos;
	const char *pathsep = "|";

	int			numpaths;
	int			ret;
	uint64		proc;
	int			j;
	int			rownr;			/* For issuing multiple rows from one original
								 * document */
	bool		had_values;		/* To determine end of nodeset results */
	StringInfoData query_buf;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlDocPtr doctree = NULL;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	/* must have at least one output column (for the pkey)
	 *
	 * 必须至少有一个输出列（用于 pkey）
	 */
	if (rsinfo->setDesc->natts < 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("xpath_table must have at least one output column")));

	/*
	 * At the moment we assume that the returned attributes make sense for the
	 * XPath specified (i.e. we trust the caller). It's not fatal if they get
	 * it wrong - the input function for the column type will raise an error
	 * if the path result can't be converted into the correct binary
	 * representation.
	 *
	 * 目前我们假设返回的属性对于指定的 XPath 有意义（即我们信任调用者）。如果他们弄错了也不是致命的 - 如果路径结果无法转换为正确的二进制表示形式，则列类型的输入函数将引发错误。
	 */

	attinmeta = TupleDescGetAttInMetadata(rsinfo->setDesc);

	values = (char **) palloc(rsinfo->setDesc->natts * sizeof(char *));
	xpaths = (xmlChar **) palloc(rsinfo->setDesc->natts * sizeof(xmlChar *));

	/*
	 * Split XPaths. xpathset is a writable CString.
	 *
	 * 分割 XPath。 xpathset 是一个可写的 CString。
	 *
	 * Note that we stop splitting once we've done all needed for tupdesc
	 *
	 * 请注意，一旦完成 tupdesc 所需的所有操作，我们就会停止拆分
	 */
	numpaths = 0;
	pos = xpathset;
	while (numpaths < (rsinfo->setDesc->natts - 1))
	{
		xpaths[numpaths++] = (xmlChar *) pos;
		pos = strstr(pos, pathsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
			break;
	}

	/* Now build query
	 *
	 * 现在构建查询
	 */
	initStringInfo(&query_buf);

	/* Build initial sql statement
	 *
	 * 构建初始sql语句
	 */
	appendStringInfo(&query_buf, "SELECT %s, %s FROM %s WHERE %s",
					 pkeyfield,
					 xmlfield,
					 relname,
					 condition);

	SPI_connect();

	if ((ret = SPI_exec(query_buf.data, 0)) != SPI_OK_SELECT)
		elog(ERROR, "xpath_table: SPI execution failed for query %s",
			 query_buf.data);

	proc = SPI_processed;
	tuptable = SPI_tuptable;
	spi_tupdesc = tuptable->tupdesc;

	/*
	 * Check that SPI returned correct result. If you put a comma into one of
	 * the function parameters, this will catch it when the SPI query returns
	 * e.g. 3 columns.
	 *
	 * 检查 SPI 是否返回正确的结果。如果您将逗号放入函数参数之一，则当 SPI 查询返回时，这将捕获它，例如3 列。
	 */
	if (spi_tupdesc->natts != 2)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("expression returning multiple columns is not valid in parameter list"),
						errdetail("Expected two columns in SPI result, got %d.", spi_tupdesc->natts)));
	}

	/*
	 * Setup the parser.  This should happen after we are done evaluating the
	 * query, in case it calls functions that set up libxml differently.
	 *
	 * 设置解析器。  这应该在我们完成查询评估后发生，以防它调用以不同方式设置 libxml 的函数。
	 */
	xmlerrcxt = pgxml_parser_init(PG_XML_STRICTNESS_LEGACY);

	PG_TRY();
	{
		/* For each row i.e. document returned from SPI
		 *
		 * 对于每一行，即从 SPI 返回的文档
		 */
		uint64		i;

		for (i = 0; i < proc; i++)
		{
			char	   *pkey;
			char	   *xmldoc;
			xmlXPathContextPtr ctxt;
			xmlXPathObjectPtr res;
			xmlChar    *resstr;
			xmlXPathCompExprPtr comppath;
			HeapTuple	ret_tuple;

			/* Extract the row data as C Strings
			 *
			 * 将行数据提取为 C 字符串
			 */
			spi_tuple = tuptable->vals[i];
			pkey = SPI_getvalue(spi_tuple, spi_tupdesc, 1);
			xmldoc = SPI_getvalue(spi_tuple, spi_tupdesc, 2);

			/*
			 * Clear the values array, so that not-well-formed documents
			 * return NULL in all columns.  Note that this also means that
			 * spare columns will be NULL.
			 *
			 * 清除值数组，以便格式不正确的文档在所有列中返回 NULL。  请注意，这也意味着备用列将为 NULL。
			 */
			for (j = 0; j < rsinfo->setDesc->natts; j++)
				values[j] = NULL;

			/* Insert primary key
			 *
			 * 插入主键
			 */
			values[0] = pkey;

			/* Parse the document
			 *
			 * 解析文档
			 */
			if (xmldoc)
				doctree = xmlReadMemory(xmldoc, strlen(xmldoc),
										NULL, NULL,
										XML_PARSE_NOENT);
			else				/* treat NULL as not well-formed */
				doctree = NULL;

			if (doctree == NULL)
			{
				/* not well-formed, so output all-NULL tuple
				 *
				 * 格式不正确，因此输出全 NULL 元组
				 */
				ret_tuple = BuildTupleFromCStrings(attinmeta, values);
				tuplestore_puttuple(rsinfo->setResult, ret_tuple);
				heap_freetuple(ret_tuple);
			}
			else
			{
				/* New loop here - we have to deal with nodeset results
				 *
				 * 这里有新循环 - 我们必须处理节点集结果
				 */
				rownr = 0;

				do
				{
					/* Now evaluate the set of xpaths.
					 *
					 * 现在评估 xpath 集。
					 */
					had_values = false;
					for (j = 0; j < numpaths; j++)
					{
						ctxt = xmlXPathNewContext(doctree);
						ctxt->node = xmlDocGetRootElement(doctree);

						/* compile the path
						 *
						 * 编译路径
						 */
						comppath = xmlXPathCtxtCompile(ctxt, xpaths[j]);
						if (comppath == NULL)
							xml_ereport(xmlerrcxt, ERROR,
										ERRCODE_INVALID_ARGUMENT_FOR_XQUERY,
										"XPath Syntax Error");

						/* Now evaluate the path expression.
						 *
						 * 现在评估路径表达式。
						 */
						res = xmlXPathCompiledEval(comppath, ctxt);
						xmlXPathFreeCompExpr(comppath);

						if (res != NULL)
						{
							switch (res->type)
							{
								case XPATH_NODESET:
									/* We see if this nodeset has enough nodes
									 *
									 * 我们看看这个节点集是否有足够的节点
									 */
									if (res->nodesetval != NULL &&
										rownr < res->nodesetval->nodeNr)
									{
										resstr = xmlXPathCastNodeToString(res->nodesetval->nodeTab[rownr]);
										had_values = true;
									}
									else
										resstr = NULL;

									break;

								case XPATH_STRING:
									resstr = xmlStrdup(res->stringval);
									break;

								default:
									elog(NOTICE, "unsupported XQuery result: %d", res->type);
									resstr = xmlStrdup((const xmlChar *) "<unsupported/>");
							}

							/*
							 * Insert this into the appropriate column in the
							 * result tuple.
							 *
							 * 将其插入结果元组中的适当列中。
							 */
							values[j + 1] = (char *) resstr;
						}
						xmlXPathFreeContext(ctxt);
					}

					/* Now add the tuple to the output, if there is one.
					 *
					 * 现在将元组添加到输出（如果有）。
					 */
					if (had_values)
					{
						ret_tuple = BuildTupleFromCStrings(attinmeta, values);
						tuplestore_puttuple(rsinfo->setResult, ret_tuple);
						heap_freetuple(ret_tuple);
					}

					rownr++;
				} while (had_values);
			}

			if (doctree != NULL)
				xmlFreeDoc(doctree);
			doctree = NULL;

			if (pkey)
				pfree(pkey);
			if (xmldoc)
				pfree(xmldoc);
		}
	}
	PG_CATCH();
	{
		if (doctree != NULL)
			xmlFreeDoc(doctree);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (doctree != NULL)
		xmlFreeDoc(doctree);

	pg_xml_done(xmlerrcxt, false);

	SPI_finish();

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 *
	 * SFRM_Materialize 模式期望我们返回 NULL Datum。实际的元组位于我们的元组存储中，并通过 rsinfo->setResult 传回。 rsinfo->setDesc 设置为我们实际用来构建元组的元组描述，因此调用者可以验证我们做了它所期望的事情。
	 */
	return (Datum) 0;
}
