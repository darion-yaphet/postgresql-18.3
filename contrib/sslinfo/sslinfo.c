/*
 * module for PostgreSQL to access client SSL certificate information
 *
 * PostgreSQL访问客户端SSL证书信息的模块
 *
 * Written by Victor B. Wagner <vitus@cryptocom.ru>, Cryptocom LTD
 * This file is distributed under BSD-style license.
 *
 * 作者：Victor B. Wagner <vitus@cryptocom.ru>，Cryptocom LTD 该文件根据 BSD 风格许可证分发。
 *
 * contrib/sslinfo/sslinfo.c
 */

#include "postgres.h"

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>

#include "access/htup_details.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC_EXT(
					.name = "sslinfo",
					.version = PG_VERSION
);

static Datum X509_NAME_field_to_text(X509_NAME *name, text *fieldName);
static Datum ASN1_STRING_to_text(ASN1_STRING *str);

/*
 * Function context for data persisting over repeated calls.
 *
 * 通过重复调用保留数据的函数上下文。
 */
typedef struct
{
	TupleDesc	tupdesc;
} SSLExtensionInfoContext;

/*
 * Indicates whether current session uses SSL
 *
 * 指示当前会话是否使用 SSL
 *
 * Function has no arguments.  Returns bool.  True if current session
 * is SSL session and false if it is local or non-ssl session.
 *
 * 函数没有参数。  返回布尔值。  如果当前会话是 SSL 会话，则为 true；如果当前会话是本地或非 ssl 会话，则为 false。
 */
PG_FUNCTION_INFO_V1(ssl_is_used);
Datum
ssl_is_used(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(MyProcPort->ssl_in_use);
}


/*
 * Returns SSL version currently in use.
 *
 * 返回当前使用的 SSL 版本。
 */
PG_FUNCTION_INFO_V1(ssl_version);
Datum
ssl_version(PG_FUNCTION_ARGS)
{
	const char *version;

	if (!MyProcPort->ssl_in_use)
		PG_RETURN_NULL();

	version = be_tls_get_version(MyProcPort);
	if (version == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(version));
}


/*
 * Returns SSL cipher currently in use.
 *
 * 返回当前使用的 SSL 密码。
 */
PG_FUNCTION_INFO_V1(ssl_cipher);
Datum
ssl_cipher(PG_FUNCTION_ARGS)
{
	const char *cipher;

	if (!MyProcPort->ssl_in_use)
		PG_RETURN_NULL();

	cipher = be_tls_get_cipher(MyProcPort);
	if (cipher == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(cipher));
}


/*
 * Indicates whether current client provided a certificate
 *
 * 指示当前客户端是否提供了证书
 *
 * Function has no arguments.  Returns bool.  True if current session
 * is SSL session and client certificate is verified, otherwise false.
 *
 * 函数没有参数。  返回布尔值。  如果当前会话是 SSL 会话并且客户端证书已验证，则为 true，否则为 false。
 */
PG_FUNCTION_INFO_V1(ssl_client_cert_present);
Datum
ssl_client_cert_present(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(MyProcPort->peer_cert_valid);
}


/*
 * Returns serial number of certificate used to establish current
 * session
 *
 * 返回用于建立当前会话的证书的序列号
 *
 * Function has no arguments.  It returns the certificate serial
 * number as numeric or null if current session doesn't use SSL or if
 * SSL connection is established without sending client certificate.
 *
 * 函数没有参数。  如果当前会话不使用 SSL 或在未发送客户端证书的情况下建立 SSL 连接，则它将返回数字形式的证书序列号或 null。
 */
PG_FUNCTION_INFO_V1(ssl_client_serial);
Datum
ssl_client_serial(PG_FUNCTION_ARGS)
{
	char decimal[NAMEDATALEN];
	Datum		result;

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	be_tls_get_peer_serial(MyProcPort, decimal, NAMEDATALEN);

	if (!*decimal)
		PG_RETURN_NULL();

	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(decimal),
								 ObjectIdGetDatum(0),
								 Int32GetDatum(-1));
	return result;
}


/*
 * Converts OpenSSL ASN1_STRING structure into text
 *
 * 将 OpenSSL ASN1_STRING 结构转换为文本
 *
 * Converts ASN1_STRING into text, converting all the characters into
 * current database encoding if possible.  Any invalid characters are
 * replaced by question marks.
 *
 * 将 ASN1_STRING 转换为文本，如果可能，将所有字符转换为当前数据库编码。  任何无效字符都将替换为问号。
 *
 * Parameter: str - OpenSSL ASN1_STRING structure.  Memory management
 * of this structure is responsibility of caller.
 *
 * 参数：str - OpenSSL ASN1_STRING 结构。  该结构的内存管理由调用者负责。
 *
 * Returns Datum, which can be directly returned from a C language SQL
 * function.
 *
 * 返回Datum，可以直接从C语言SQL函数返回。
 */
static Datum
ASN1_STRING_to_text(ASN1_STRING *str)
{
	BIO		   *membuf;
	size_t		size;
	char		nullterm;
	char	   *sp;
	char	   *dp;
	text	   *result;

	membuf = BIO_new(BIO_s_mem());
	if (membuf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create OpenSSL BIO structure")));
	(void) BIO_set_close(membuf, BIO_CLOSE);
	ASN1_STRING_print_ex(membuf, str,
						 ((ASN1_STRFLGS_RFC2253 & ~ASN1_STRFLGS_ESC_MSB)
						  | ASN1_STRFLGS_UTF8_CONVERT));
	/* ensure null termination of the BIO's content
	 *
	 * 确保 BIO 内容的空终止
	 */
	nullterm = '\0';
	BIO_write(membuf, &nullterm, 1);
	size = BIO_get_mem_data(membuf, &sp);
	dp = pg_any_to_server(sp, size - 1, PG_UTF8);
	result = cstring_to_text(dp);
	if (dp != sp)
		pfree(dp);
	if (BIO_free(membuf) != 1)
		elog(ERROR, "could not free OpenSSL BIO structure");

	PG_RETURN_TEXT_P(result);
}


/*
 * Returns specified field of specified X509_NAME structure
 *
 * 返回指定X509_NAME结构的指定字段
 *
 * Common part of ssl_client_dn and ssl_issuer_dn functions.
 *
 * ssl_client_dn 和 ssl_issuer_dn 函数的公共部分。
 *
 * Parameter: X509_NAME *name - either subject or issuer of certificate
 * Parameter: text fieldName  - field name string like 'CN' or commonName
 *			  to be looked up in the OpenSSL ASN1 OID database
 *
 * 参数：X509_NAME *name - 证书的主题或颁发者 参数：text fieldName - 要在 OpenSSL ASN1 OID 数据库中查找的字段名称字符串，如“CN”或 commonName
 *
 * Returns result of ASN1_STRING_to_text applied to appropriate
 * part of name
 *
 * 返回应用于名称适当部分的 ASN1_STRING_to_text 的结果
 */
static Datum
X509_NAME_field_to_text(X509_NAME *name, text *fieldName)
{
	char	   *string_fieldname;
	int			nid,
				index;
	ASN1_STRING *data;

	string_fieldname = text_to_cstring(fieldName);
	nid = OBJ_txt2nid(string_fieldname);
	if (nid == NID_undef)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid X.509 field name: \"%s\"",
						string_fieldname)));
	pfree(string_fieldname);
	index = X509_NAME_get_index_by_NID(name, nid, -1);
	if (index < 0)
		return (Datum) 0;
	data = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index));
	return ASN1_STRING_to_text(data);
}


/*
 * Returns specified field of client certificate distinguished name
 *
 * 返回客户端证书专有名称的指定字段
 *
 * Receives field name (like 'commonName' and 'emailAddress') and
 * returns appropriate part of certificate subject converted into
 * database encoding.
 *
 * 接收字段名称（如“commonName”和“emailAddress”）并返回转换为数据库编码的证书主题的适当部分。
 *
 * Parameter: fieldname text - will be looked up in OpenSSL object
 * identifier database
 *
 * 参数：字段名文本 - 将在 OpenSSL 对象标识符数据库中查找
 *
 * Returns text string with appropriate value.
 *
 * 返回具有适当值的文本字符串。
 *
 * Throws an error if argument cannot be converted into ASN1 OID by
 * OpenSSL.  Returns null if no client certificate is present, or if
 * there is no field with such name in the certificate.
 *
 * 如果 OpenSSL 无法将参数转换为 ASN1 OID，则会引发错误。  如果不存在客户端证书，或者证书中不存在具有该名称的字段，则返回 null。
 */
PG_FUNCTION_INFO_V1(ssl_client_dn_field);
Datum
ssl_client_dn_field(PG_FUNCTION_ARGS)
{
	text	   *fieldname = PG_GETARG_TEXT_PP(0);
	Datum		result;

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	result = X509_NAME_field_to_text(X509_get_subject_name(MyProcPort->peer), fieldname);

	if (!result)
		PG_RETURN_NULL();
	else
		return result;
}


/*
 * Returns specified field of client certificate issuer name
 *
 * 返回客户端证书颁发者名称的指定字段
 *
 * Receives field name (like 'commonName' and 'emailAddress') and
 * returns appropriate part of certificate subject converted into
 * database encoding.
 *
 * 接收字段名称（如“commonName”和“emailAddress”）并返回转换为数据库编码的证书主题的适当部分。
 *
 * Parameter: fieldname text - would be looked up in OpenSSL object
 * identifier database
 *
 * 参数：字段名文本 - 将在 OpenSSL 对象标识符数据库中查找
 *
 * Returns text string with appropriate value.
 *
 * 返回具有适当值的文本字符串。
 *
 * Throws an error if argument cannot be converted into ASN1 OID by
 * OpenSSL.  Returns null if no client certificate is present, or if
 * there is no field with such name in the certificate.
 *
 * 如果 OpenSSL 无法将参数转换为 ASN1 OID，则会引发错误。  如果不存在客户端证书，或者证书中不存在具有该名称的字段，则返回 null。
 */
PG_FUNCTION_INFO_V1(ssl_issuer_field);
Datum
ssl_issuer_field(PG_FUNCTION_ARGS)
{
	text	   *fieldname = PG_GETARG_TEXT_PP(0);
	Datum		result;

	if (!(MyProcPort->peer))
		PG_RETURN_NULL();

	result = X509_NAME_field_to_text(X509_get_issuer_name(MyProcPort->peer), fieldname);

	if (!result)
		PG_RETURN_NULL();
	else
		return result;
}


/*
 * Returns current client certificate subject as one string
 *
 * 以一个字符串形式返回当前客户端证书主题
 *
 * This function returns distinguished name (subject) of the client
 * certificate used in the current SSL connection, converting it into
 * the current database encoding.
 *
 * 此函数返回当前 SSL 连接中使用的客户端证书的专有名称（主题），并将其转换为当前数据库编码。
 *
 * Returns text datum.
 *
 * 返回文本数据。
 */
PG_FUNCTION_INFO_V1(ssl_client_dn);
Datum
ssl_client_dn(PG_FUNCTION_ARGS)
{
	char		subject[NAMEDATALEN];

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	be_tls_get_peer_subject_name(MyProcPort, subject, NAMEDATALEN);

	if (!*subject)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(subject));
}


/*
 * Returns current client certificate issuer as one string
 *
 * 将当前客户端证书颁发者作为一个字符串返回
 *
 * This function returns issuer's distinguished name of the client
 * certificate used in the current SSL connection, converting it into
 * the current database encoding.
 *
 * 此函数返回当前 SSL 连接中使用的客户端证书的颁发者的可分辨名称，并将其转换为当前数据库编码。
 *
 * Returns text datum.
 *
 * 返回文本数据。
 */
PG_FUNCTION_INFO_V1(ssl_issuer_dn);
Datum
ssl_issuer_dn(PG_FUNCTION_ARGS)
{
	char		issuer[NAMEDATALEN];

	if (!MyProcPort->ssl_in_use || !MyProcPort->peer_cert_valid)
		PG_RETURN_NULL();

	be_tls_get_peer_issuer_name(MyProcPort, issuer, NAMEDATALEN);

	if (!*issuer)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(issuer));
}


/*
 * Returns information about available SSL extensions.
 *
 * 返回有关可用 SSL 扩展的信息。
 *
 * Returns setof record made of the following values:
 * - name of the extension.
 * - value of the extension.
 * - critical status of the extension.
 *
 * 返回由以下值组成的记录集： - 扩展名。 - 扩展的值。 - 分机的紧急状态。
 */
PG_FUNCTION_INFO_V1(ssl_extension_info);
Datum
ssl_extension_info(PG_FUNCTION_ARGS)
{
	X509	   *cert = MyProcPort->peer;
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;
	MemoryContext oldcontext;
	SSLExtensionInfoContext *fctx;

	if (SRF_IS_FIRSTCALL())
	{

		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence
		 *
		 * 创建用于交叉调用持久化的函数上下文
		 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * Switch to memory context appropriate for multiple function calls
		 *
		 * 切换到适合多个函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Create a user function context for cross-call persistence
		 *
		 * 创建用户函数上下文以进行交叉调用持久化
		 */
		fctx = (SSLExtensionInfoContext *) palloc(sizeof(SSLExtensionInfoContext));

		/* Construct tuple descriptor
		 *
		 * 构造元组描述符
		 */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context that cannot accept type record")));
		fctx->tupdesc = BlessTupleDesc(tupdesc);

		/* Set max_calls as a count of extensions in certificate
		 *
		 * 将 max_calls 设置为证书中的扩展计数
		 */
		max_calls = cert != NULL ? X509_get_ext_count(cert) : 0;

		if (max_calls > 0)
		{
			/* got results, keep track of them
			 *
			 * 得到结果，跟踪它们
			 */
			funcctx->max_calls = max_calls;
			funcctx->user_fctx = fctx;
		}
		else
		{
			/* fast track when no results
			 *
			 * 没有结果时快速跟踪
			 */
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function
	 *
	 * 每次调用函数时完成的事情
	 */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * Initialize per-call variables.
	 *
	 * 初始化每次调用变量。
	 */
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	fctx = funcctx->user_fctx;

	/* do while there are more left to send
	 *
	 * 趁还有更多邮件要发送时做
	 */
	if (call_cntr < max_calls)
	{
		Datum		values[3];
		bool		nulls[3];
		char	   *buf;
		HeapTuple	tuple;
		Datum		result;
		BIO		   *membuf;
		X509_EXTENSION *ext;
		ASN1_OBJECT *obj;
		int			nid;
		int			len;

		/* need a BIO for this
		 *
		 * 为此需要一个 BIO
		 */
		membuf = BIO_new(BIO_s_mem());
		if (membuf == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create OpenSSL BIO structure")));

		/* Get the extension from the certificate
		 *
		 * 从证书中获取扩展名
		 */
		ext = X509_get_ext(cert, call_cntr);
		obj = X509_EXTENSION_get_object(ext);

		/* Get the extension name
		 *
		 * 获取扩展名
		 */
		nid = OBJ_obj2nid(obj);
		if (nid == NID_undef)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unknown OpenSSL extension in certificate at position %d",
							call_cntr)));
		values[0] = CStringGetTextDatum(OBJ_nid2sn(nid));
		nulls[0] = false;

		/* Get the extension value
		 *
		 * 获取扩展值
		 */
		if (X509V3_EXT_print(membuf, ext, 0, 0) <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not print extension value in certificate at position %d",
							call_cntr)));
		len = BIO_get_mem_data(membuf, &buf);
		values[1] = PointerGetDatum(cstring_to_text_with_len(buf, len));
		nulls[1] = false;

		/* Get critical status
		 *
		 * 获得危急状态
		 */
		values[2] = BoolGetDatum(X509_EXTENSION_get_critical(ext));
		nulls[2] = false;

		/* Build tuple
		 *
		 * 构建元组
		 */
		tuple = heap_form_tuple(fctx->tupdesc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		if (BIO_free(membuf) != 1)
			elog(ERROR, "could not free OpenSSL BIO structure");

		SRF_RETURN_NEXT(funcctx, result);
	}

	/* All done
	 *
	 * 全部完成
	 */
	SRF_RETURN_DONE(funcctx);
}
