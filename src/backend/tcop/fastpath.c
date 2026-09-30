/*-------------------------------------------------------------------------
 *
 * fastpath.c
 *	  routines to handle function requests from the frontend
 *
 *	  处理来自前端的函数请求的例程
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/fastpath.c
 *
 * NOTES
 *	  This cruft is the server side of PQfn.
 *
 *	  说明
 *	  这些遗留代码是 PQfn 的服务器端实现。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "tcop/fastpath.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * Formerly, this code attempted to cache the function and type info
 * looked up by fetch_fp_info, but only for the duration of a single
 * transaction command (since in theory the info could change between
 * commands).  This was utterly useless, because postgres.c executes
 * each fastpath call as a separate transaction command, and so the
 * cached data could never actually have been reused.  If it had worked
 * as intended, it would have had problems anyway with dangling references
 * in the FmgrInfo struct.  So, forget about caching and just repeat the
 * syscache fetches on each usage.  They're not *that* expensive.
 *
 * 以前，这段代码曾试图缓存 fetch_fp_info 查找到的函数和类型信息，
 * 但只在单个事务命令期间缓存（因为理论上这些信息可能在命令之间变化）。
 * 这完全没有用，因为 postgres.c 会把每个 fastpath 调用作为独立的事务命令执行，
 * 因此缓存数据实际上永远无法被重用。即使它按预期工作，也仍会遇到 FmgrInfo
 * 结构中悬空引用的问题。因此，放弃缓存，在每次使用时重复执行 syscache 获取。
 * 它们并没有那么昂贵。
 */
struct fp_info
{
	Oid			funcid;
	FmgrInfo	flinfo;			/* function lookup info for funcid
								 *
								 * funcid 的函数查找信息 */
	Oid			namespace;		/* other stuff from pg_proc
								 *
								 * 来自 pg_proc 的其他信息 */
	Oid			rettype;
	Oid			argtypes[FUNC_MAX_ARGS];
	char		fname[NAMEDATALEN]; /* function name for logging
									 *
									 * 用于日志记录的函数名称 */
};


static int16 parse_fcall_arguments(StringInfo msgBuf, struct fp_info *fip,
								   FunctionCallInfo fcinfo);

/* ----------------
 *		SendFunctionResult
 *
 *		发送函数结果
 *
 *		Send a fastpath function result in text or binary form, or mark it
 *		as NULL in the FunctionCallResponse message.
 *
 *		在 FunctionCallResponse 消息中以文本或二进制形式发送 fastpath
 *		函数结果，或将其标记为 NULL。
 * ----------------
 */
static void
SendFunctionResult(Datum retval, bool isnull, Oid rettype, int16 format)
{
	StringInfoData buf;

	pq_beginmessage(&buf, PqMsg_FunctionCallResponse);

	if (isnull)
	{
		pq_sendint32(&buf, -1);
	}
	else
	{
		if (format == 0)
		{
			Oid			typoutput;
			bool		typisvarlena;
			char	   *outputstr;

			getTypeOutputInfo(rettype, &typoutput, &typisvarlena);
			outputstr = OidOutputFunctionCall(typoutput, retval);
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr));
			pfree(outputstr);
		}
		else if (format == 1)
		{
			Oid			typsend;
			bool		typisvarlena;
			bytea	   *outputbytes;

			getTypeBinaryOutputInfo(rettype, &typsend, &typisvarlena);
			outputbytes = OidSendFunctionCall(typsend, retval);
			pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}

	pq_endmessage(&buf);
}

/*
 * fetch_fp_info
 *
 * 获取 fastpath 函数信息
 *
 * Performs catalog lookups to load a struct fp_info 'fip' for the
 * function 'func_id'.
 *
 * 执行目录查找，为函数 'func_id' 加载 struct fp_info 'fip'。
 */
static void
fetch_fp_info(Oid func_id, struct fp_info *fip)
{
	HeapTuple	func_htp;
	Form_pg_proc pp;

	Assert(fip != NULL);

	/*
	 * Since the validity of this structure is determined by whether the
	 * funcid is OK, we clear the funcid here.  It must not be set to the
	 * correct value until we are about to return with a good struct fp_info,
	 * since we can be interrupted (i.e., with an ereport(ERROR, ...)) at any
	 * time.  [No longer really an issue since we don't save the struct
	 * fp_info across transactions anymore, but keep it anyway.]
	 *
	 * 由于该结构的有效性由 funcid 是否有效决定，我们在这里清除 funcid。
	 * 在即将带着良好的 struct fp_info 返回之前，不能将它设置为正确值，
	 * 因为我们可能在任何时候被中断（即 ereport(ERROR, ...)）。[由于我们不再跨事务
	 * 保存 struct fp_info，这实际上已不再是问题，但仍保留这种做法。]
	 */
	MemSet(fip, 0, sizeof(struct fp_info));
	fip->funcid = InvalidOid;

	func_htp = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_id));
	if (!HeapTupleIsValid(func_htp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", func_id)));
	pp = (Form_pg_proc) GETSTRUCT(func_htp);

	/*
	 * reject pg_proc entries that are unsafe to call via fastpath
	 *
	 * 拒绝通过 fastpath 调用不安全的 pg_proc 条目。
	 */
	if (pp->prokind != PROKIND_FUNCTION || pp->proretset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot call function \"%s\" via fastpath interface",
						NameStr(pp->proname))));

	/*
	 * watch out for catalog entries with more than FUNC_MAX_ARGS args
	 *
	 * 留意参数数量超过 FUNC_MAX_ARGS 的目录条目。
	 */
	if (pp->pronargs > FUNC_MAX_ARGS)
		elog(ERROR, "function %s has more than %d arguments",
			 NameStr(pp->proname), FUNC_MAX_ARGS);

	fip->namespace = pp->pronamespace;
	fip->rettype = pp->prorettype;
	memcpy(fip->argtypes, pp->proargtypes.values, pp->pronargs * sizeof(Oid));
	strlcpy(fip->fname, NameStr(pp->proname), NAMEDATALEN);

	ReleaseSysCache(func_htp);

	fmgr_info(func_id, &fip->flinfo);

	/*
	 * This must be last!
	 *
	 * 这必须放在最后！
	 */
	fip->funcid = func_id;
}


/*
 * HandleFunctionRequest
 *
 * 处理函数请求
 *
 * Server side of PQfn (fastpath function calls from the frontend).
 * This corresponds to the libpq protocol symbol "F".
 *
 * PQfn 的服务器端（来自前端的 fastpath 函数调用）。
 * 这对应于 libpq 协议符号 "F"。
 *
 * The core flow validates transaction state, fetches function metadata,
 * checks permissions, parses arguments, invokes the function when allowed,
 * sends the result, and emits duration logging if requested.
 *
 * 核心流程是验证事务状态、获取函数元数据、检查权限、解析参数、在允许时调用函数、
 * 发送结果，并在请求时输出持续时间日志。
 *
 * INPUT:
 *		postgres.c has already read the message body and will pass it in
 *		msgBuf.
 *
 * 输入：
 *		postgres.c 已经读取消息体，并会通过 msgBuf 传入。
 *
 * Note: palloc()s done here and in the called function do not need to be
 * cleaned up explicitly.  We are called from PostgresMain() in the
 * MessageContext memory context, which will be automatically reset when
 * control returns to PostgresMain.
 *
 * 注意：这里以及被调用函数中执行的 palloc() 不需要显式清理。我们是在
 * MessageContext 内存上下文中从 PostgresMain() 被调用的，当控制返回
 * PostgresMain 时，该上下文会自动重置。
 */
void
HandleFunctionRequest(StringInfo msgBuf)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	Oid			fid;
	AclResult	aclresult;
	int16		rformat;
	Datum		retval;
	struct fp_info my_fp;
	struct fp_info *fip;
	bool		callit;
	bool		was_logged = false;
	char		msec_str[32];

	/*
	 * We only accept COMMIT/ABORT if we are in an aborted transaction, and
	 * COMMIT/ABORT cannot be executed through the fastpath interface.
	 *
	 * 只有在已中止的事务中我们才接受 COMMIT/ABORT，而 COMMIT/ABORT
	 * 不能通过 fastpath 接口执行。
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block")));

	/*
	 * Now that we know we are in a valid transaction, set snapshot in case
	 * needed by function itself or one of the datatype I/O routines.
	 *
	 * 既然已经知道我们处于有效事务中，就设置快照，以备函数自身或某个数据类型
	 * I/O 例程需要。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Begin parsing the buffer contents.
	 *
	 * 开始解析缓冲区内容。
	 */
	fid = (Oid) pq_getmsgint(msgBuf, 4);	/* function oid
										 *
										 * 函数 OID */

	/*
	 * There used to be a lame attempt at caching lookup info here. Now we
	 * just do the lookups on every call.
	 *
	 * 这里过去曾有一个拙劣的查找信息缓存尝试。现在我们会在每次调用时直接查找。
	 */
	fip = &my_fp;
	fetch_fp_info(fid, fip);

	/*
	 * Log as soon as we have the function OID and name
	 *
	 * 一旦获得函数 OID 和名称就记录日志。
	 */
	if (log_statement == LOGSTMT_ALL)
	{
		ereport(LOG,
				(errmsg("fastpath function call: \"%s\" (OID %u)",
						fip->fname, fid)));
		was_logged = true;
	}

	/*
	 * Check permission to access and call function.  Since we didn't go
	 * through a normal name lookup, we need to check schema usage too.
	 *
	 * 检查访问并调用函数的权限。由于我们没有经过普通名称查找，
	 * 因此也需要检查模式使用权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, fip->namespace, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(fip->namespace));
	InvokeNamespaceSearchHook(fip->namespace, true);

	aclresult = object_aclcheck(ProcedureRelationId, fid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   get_func_name(fid));
	InvokeFunctionExecuteHook(fid);

	/*
	 * Prepare function call info block and insert arguments.
	 *
	 * 准备函数调用信息块并插入参数。
	 *
	 * Note: for now we pass collation = InvalidOid, so collation-sensitive
	 * functions can't be called this way.  Perhaps we should pass
	 * DEFAULT_COLLATION_OID, instead?
	 *
	 * 注意：目前我们传递 collation = InvalidOid，因此不能以这种方式调用对排序规则敏感的
	 * 函数。或许应该改为传递 DEFAULT_COLLATION_OID？
	 */
	InitFunctionCallInfoData(*fcinfo, &fip->flinfo, 0, InvalidOid, NULL, NULL);

	rformat = parse_fcall_arguments(msgBuf, fip, fcinfo);

	/*
	 * Verify we reached the end of the message where expected.
	 *
	 * 验证我们在预期位置到达了消息末尾。
	 */
	pq_getmsgend(msgBuf);

	/*
	 * If func is strict, must not call it for null args.
	 *
	 * 如果函数是 strict，则不能在参数为 NULL 时调用它。
	 */
	callit = true;
	if (fip->flinfo.fn_strict)
	{
		int			i;

		for (i = 0; i < fcinfo->nargs; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				callit = false;
				break;
			}
		}
	}

	if (callit)
	{
		/*
		 * Okay, do it ...
		 *
		 * 好，执行调用……
		 */
		retval = FunctionCallInvoke(fcinfo);
	}
	else
	{
		fcinfo->isnull = true;
		retval = (Datum) 0;
	}

	/*
	 * ensure we do at least one CHECK_FOR_INTERRUPTS per function call
	 *
	 * 确保每次函数调用至少执行一次 CHECK_FOR_INTERRUPTS。
	 */
	CHECK_FOR_INTERRUPTS();

	SendFunctionResult(retval, fcinfo->isnull, fip->rettype, rformat);

	/*
	 * We no longer need the snapshot
	 *
	 * 我们不再需要该快照。
	 */
	PopActiveSnapshot();

	/*
	 * Emit duration logging if appropriate.
	 *
	 * 如果合适，输出持续时间日志。
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  fastpath function call: \"%s\" (OID %u)",
							msec_str, fip->fname, fid)));
			break;
	}
}

/*
 * Parse function arguments in a 3.0 protocol message
 *
 * 解析 3.0 协议消息中的函数参数
 *
 * Argument values are loaded into *fcinfo, and the desired result format
 * is returned.
 *
 * 参数值会加载到 *fcinfo 中，并返回期望的结果格式。
 *
 * The core flow reads format codes, validates argument counts, converts each
 * argument from text or binary form, and then reads the result format code.
 *
 * 核心流程是读取格式代码、验证参数数量、从文本或二进制形式转换每个参数，
 * 然后读取结果格式代码。
 */
static int16
parse_fcall_arguments(StringInfo msgBuf, struct fp_info *fip,
					  FunctionCallInfo fcinfo)
{
	int			nargs;
	int			i;
	int			numAFormats;
	int16	   *aformats = NULL;
	StringInfoData abuf;

	/*
	 * Get the argument format codes
	 *
	 * 获取参数格式代码。
	 */
	numAFormats = pq_getmsgint(msgBuf, 2);
	if (numAFormats > 0)
	{
		aformats = (int16 *) palloc(numAFormats * sizeof(int16));
		for (i = 0; i < numAFormats; i++)
			aformats[i] = pq_getmsgint(msgBuf, 2);
	}

	nargs = pq_getmsgint(msgBuf, 2);	/* # of arguments
										 *
										 * 参数数量 */

	if (fip->flinfo.fn_nargs != nargs || nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d arguments but function requires %d",
						nargs, fip->flinfo.fn_nargs)));

	fcinfo->nargs = nargs;

	if (numAFormats > 1 && numAFormats != nargs)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d argument formats but %d arguments",
						numAFormats, nargs)));

	initStringInfo(&abuf);

	/*
	 * Copy supplied arguments into arg vector.
	 *
	 * 将提供的参数复制到参数向量中。
	 */
	for (i = 0; i < nargs; ++i)
	{
		int			argsize;
		int16		aformat;

		argsize = pq_getmsgint(msgBuf, 4);
		if (argsize == -1)
		{
			fcinfo->args[i].isnull = true;
		}
		else
		{
			fcinfo->args[i].isnull = false;
			if (argsize < 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid argument size %d in function call message",
								argsize)));

			/*
			 * Reset abuf to empty, and insert raw data into it
			 *
			 * 将 abuf 重置为空，并把原始数据插入其中。
			 */
			resetStringInfo(&abuf);
			appendBinaryStringInfo(&abuf,
								   pq_getmsgbytes(msgBuf, argsize),
								   argsize);
		}

		if (numAFormats > 1)
			aformat = aformats[i];
		else if (numAFormats > 0)
			aformat = aformats[0];
		else
			aformat = 0;		/* default = text
								 *
								 * 默认 = 文本 */

		if (aformat == 0)
		{
			Oid			typinput;
			Oid			typioparam;
			char	   *pstring;

			getTypeInputInfo(fip->argtypes[i], &typinput, &typioparam);

			/*
			 * Since stringinfo.c keeps a trailing null in place even for
			 * binary data, the contents of abuf are a valid C string.  We
			 * have to do encoding conversion before calling the typinput
			 * routine, though.
			 *
			 * 由于 stringinfo.c 即使对二进制数据也会保留尾随的 null，
			 * abuf 的内容是有效的 C 字符串。不过，在调用 typinput 例程之前，
			 * 我们必须先做编码转换。
			 */
			if (argsize == -1)
				pstring = NULL;
			else
				pstring = pg_client_to_server(abuf.data, argsize);

			fcinfo->args[i].value = OidInputFunctionCall(typinput, pstring,
														 typioparam, -1);
			/*
			 * Free result of encoding conversion, if any
			 *
			 * 如果有编码转换结果，则释放它。
			 */
			if (pstring && pstring != abuf.data)
				pfree(pstring);
		}
		else if (aformat == 1)
		{
			Oid			typreceive;
			Oid			typioparam;
			StringInfo	bufptr;

			/*
			 * Call the argument type's binary input converter
			 *
			 * 调用参数类型的二进制输入转换器。
			 */
			getTypeBinaryInputInfo(fip->argtypes[i], &typreceive, &typioparam);

			if (argsize == -1)
				bufptr = NULL;
			else
				bufptr = &abuf;

			fcinfo->args[i].value = OidReceiveFunctionCall(typreceive, bufptr,
														   typioparam, -1);

			/*
			 * Trouble if it didn't eat the whole buffer
			 *
			 * 如果它没有消耗完整缓冲区，就是有问题。
			 */
			if (argsize != -1 && abuf.cursor != abuf.len)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("incorrect binary data format in function argument %d",
								i + 1)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", aformat)));
	}

	/*
	 * Return result format code
	 *
	 * 返回结果格式代码。
	 */
	return (int16) pq_getmsgint(msgBuf, 2);
}
