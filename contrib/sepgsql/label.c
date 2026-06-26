/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/label.c
 *
 * Routines to support SELinux labels (security context)
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <selinux/label.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/dbcommands.h"
#include "commands/seclabel.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "sepgsql.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Saved hook entries (if stacked)
 *
 * 保存的钩子条目（如果堆叠）
 */
static ClientAuthentication_hook_type next_client_auth_hook = NULL;
static needs_fmgr_hook_type next_needs_fmgr_hook = NULL;
static fmgr_hook_type next_fmgr_hook = NULL;

/*
 * client_label_*
 *
 * 客户标签_*
 *
 * security label of the database client.  Initially the client security label
 * is equal to client_label_peer, and can be changed by one or more calls to
 * sepgsql_setcon(), and also be temporarily overridden during execution of a
 * trusted-procedure.
 *
 * 数据库客户端的安全标签。  最初，客户端安全标签等于 client_label_peer，并且可以通过对 sepgsql_setcon() 的一次或多次调用进行更改，并且还可以在执行可信过程期间暂时覆盖。
 *
 * sepgsql_setcon() is a transaction-aware operation; a (sub-)transaction
 * rollback should also rollback the current client security label.  Therefore
 * we use the list client_label_pending of pending_label to keep track of which
 * labels were set during the (sub-)transactions.
 *
 * sepgsql_setcon() 是事务感知操作； （子）事务回滚还应该回滚当前客户端安全标签。  因此，我们使用pending_label 的列表client_label_pending 来跟踪在（子）交易期间设置了哪些标签。
 */
static char *client_label_peer = NULL;	/* set by getpeercon(3) */
static List *client_label_pending = NIL;	/* pending list being set by
											 * sepgsql_setcon() */
static char *client_label_committed = NULL; /* set by sepgsql_setcon(), and
											 * already committed */
static char *client_label_func = NULL;	/* set by trusted procedure */

typedef struct
{
	SubTransactionId subid;
	char	   *label;
} pending_label;

/*
 * sepgsql_get_client_label
 *
 * Returns the current security label of the client.  All code should use this
 * routine to get the current label, instead of referring to the client_label_*
 * variables above.
 *
 * 返回客户端当前的安全标签。  所有代码都应该使用此例程来获取当前标签，而不是引用上面的 client_label_* 变量。
 */
char *
sepgsql_get_client_label(void)
{
	/* trusted procedure client label override
	 *
	 * 可信过程客户端标签覆盖
	 */
	if (client_label_func)
		return client_label_func;

	/* uncommitted sepgsql_setcon() value
	 *
	 * 未提交的 sepgsql_setcon() 值
	 */
	if (client_label_pending)
	{
		pending_label *plabel = llast(client_label_pending);

		if (plabel->label)
			return plabel->label;
	}
	else if (client_label_committed)
		return client_label_committed;	/* set by sepgsql_setcon() committed */

	/* default label
	 *
	 * 默认标签
	 */
	Assert(client_label_peer != NULL);
	return client_label_peer;
}

/*
 * sepgsql_set_client_label
 *
 * This routine tries to switch the current security label of the client, and
 * checks related permissions.  The supplied new label shall be added to the
 * client_label_pending list, then saved at transaction-commit time to ensure
 * transaction-awareness.
 *
 * 该例程尝试切换客户端当前的安全标签，并检查相关权限。  提供的新标签应添加到 client_label_pending 列表中，然后在事务提交时保存以确保事务感知。
 */
static void
sepgsql_set_client_label(const char *new_label)
{
	const char *tcontext;
	MemoryContext oldcxt;
	pending_label *plabel;

	/* Reset to the initial client label, if NULL
	 *
	 * 如果为 NULL，则重置为初始客户端标签
	 */
	if (!new_label)
		tcontext = client_label_peer;
	else
	{
		if (security_check_context_raw(new_label) < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("SELinux: invalid security label: \"%s\"",
							new_label)));
		tcontext = new_label;
	}

	/* Check process:{setcurrent} permission.
	 *
	 * 检查进程：{setcurrent} 权限。
	 */
	sepgsql_avc_check_perms_label(sepgsql_get_client_label(),
								  SEPG_CLASS_PROCESS,
								  SEPG_PROCESS__SETCURRENT,
								  NULL,
								  true);
	/* Check process:{dyntransition} permission.
	 *
	 * 检查进程：{dyntransition} 权限。
	 */
	sepgsql_avc_check_perms_label(tcontext,
								  SEPG_CLASS_PROCESS,
								  SEPG_PROCESS__DYNTRANSITION,
								  NULL,
								  true);

	/*
	 * Append the supplied new_label on the pending list until the current
	 * transaction is committed.
	 *
	 * 将提供的 new_label 附加到待处理列表上，直到提交当前事务。
	 */
	oldcxt = MemoryContextSwitchTo(CurTransactionContext);

	plabel = palloc0(sizeof(pending_label));
	plabel->subid = GetCurrentSubTransactionId();
	if (new_label)
		plabel->label = pstrdup(new_label);
	client_label_pending = lappend(client_label_pending, plabel);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * sepgsql_xact_callback
 *
 * A callback routine of transaction commit/abort/prepare.  Commit or abort
 * changes in the client_label_pending list.
 *
 * 事务提交/中止/准备的回调例程。  提交或中止 client_label_pending 列表中的更改。
 */
static void
sepgsql_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT)
	{
		if (client_label_pending != NIL)
		{
			pending_label *plabel = llast(client_label_pending);
			char	   *new_label;

			if (plabel->label)
				new_label = MemoryContextStrdup(TopMemoryContext,
												plabel->label);
			else
				new_label = NULL;

			if (client_label_committed)
				pfree(client_label_committed);

			client_label_committed = new_label;

			/*
			 * XXX - Note that items of client_label_pending are allocated on
			 * CurTransactionContext, thus, all acquired memory region shall
			 * be released implicitly.
			 *
			 * XXX - 请注意，client_label_pending 的项目是在 CurTransactionContext 上分配的，因此，所有获取的内存区域都应隐式释放。
			 */
			client_label_pending = NIL;
		}
	}
	else if (event == XACT_EVENT_ABORT)
		client_label_pending = NIL;
}

/*
 * sepgsql_subxact_callback
 *
 * A callback routine of sub-transaction start/abort/commit.  Releases all
 * security labels that are set within the sub-transaction that is aborted.
 *
 * 子事务启动/中止/提交的回调例程。  释放在中止的子事务中设置的所有安全标签。
 */
static void
sepgsql_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	ListCell   *cell;

	if (event == SUBXACT_EVENT_ABORT_SUB)
	{
		foreach(cell, client_label_pending)
		{
			pending_label *plabel = lfirst(cell);

			if (plabel->subid == mySubid)
				client_label_pending
					= foreach_delete_current(client_label_pending, cell);
		}
	}
}

/*
 * sepgsql_client_auth
 *
 * Entrypoint of the client authentication hook.
 * It switches the client label according to getpeercon(), and the current
 * performing mode according to the GUC setting.
 *
 * 客户端身份验证挂钩的入口点。它根据 getpeercon() 切换客户端标签，并根据 GUC 设置切换当前执行模式。
 */
static void
sepgsql_client_auth(Port *port, int status)
{
	if (next_client_auth_hook)
		(*next_client_auth_hook) (port, status);

	/*
	 * In the case when authentication failed, the supplied socket shall be
	 * closed soon, so we don't need to do anything here.
	 *
	 * 如果身份验证失败，提供的套接字将很快关闭，因此我们不需要在这里执行任何操作。
	 */
	if (status != STATUS_OK)
		return;

	/*
	 * Getting security label of the peer process using API of libselinux.
	 *
	 * 使用 libselinux 的 API 获取对等进程的安全标签。
	 */
	if (getpeercon_raw(port->sock, &client_label_peer) < 0)
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: unable to get peer label: %m")));

	/*
	 * Switch the current performing mode from INTERNAL to either DEFAULT or
	 * PERMISSIVE.
	 *
	 * 将当前执行模式从 INTERNAL 切换为 DEFAULT 或 PERMISSIVE。
	 */
	if (sepgsql_get_permissive())
		sepgsql_set_mode(SEPGSQL_MODE_PERMISSIVE);
	else
		sepgsql_set_mode(SEPGSQL_MODE_DEFAULT);
}

/*
 * sepgsql_needs_fmgr_hook
 *
 * It informs the core whether the supplied function is trusted procedure,
 * or not. If true, sepgsql_fmgr_hook shall be invoked at start, end, and
 * abort time of function invocation.
 *
 * 它通知核心所提供的函数是否是可信过程。如果为 true，则应在函数调用的开始、结束和中止时调用 sepgsql_fmgr_hook。
 */
static bool
sepgsql_needs_fmgr_hook(Oid functionId)
{
	ObjectAddress object;

	if (next_needs_fmgr_hook &&
		(*next_needs_fmgr_hook) (functionId))
		return true;

	/*
	 * SELinux needs the function to be called via security_definer wrapper,
	 * if this invocation will take a domain-transition. We call these
	 * functions as trusted-procedure, if the security policy has a rule that
	 * switches security label of the client on execution.
	 *
	 * 如果此调用将进行域转换，SELinux 需要通过 security_definer 包装器调用该函数。如果安全策略具有在执行时切换客户端安全标签的规则，我们将这些函数称为可信过程。
	 */
	if (sepgsql_avc_trusted_proc(functionId) != NULL)
		return true;

	/*
	 * Even if not a trusted-procedure, this function should not be inlined
	 * unless the client has db_procedure:{execute} permission. Please note
	 * that it shall be actually failed later because of same reason with
	 * ACL_EXECUTE.
	 *
	 * 即使不是可信过程，也不应内联此函数，除非客户端具有 db_procedure:{execute} 权限。请注意，由于与 ACL_EXECUTE 相同的原因，稍后它实际上会失败。
	 */
	object.classId = ProcedureRelationId;
	object.objectId = functionId;
	object.objectSubId = 0;
	if (!sepgsql_avc_check_perms(&object,
								 SEPG_CLASS_DB_PROCEDURE,
								 SEPG_DB_PROCEDURE__EXECUTE |
								 SEPG_DB_PROCEDURE__ENTRYPOINT,
								 SEPGSQL_AVC_NOAUDIT, false))
		return true;

	return false;
}

/*
 * sepgsql_fmgr_hook
 *
 * It switches security label of the client on execution of trusted
 * procedures.
 *
 * 它在执行可信过程时切换客户端的安全标签。
 */
static void
sepgsql_fmgr_hook(FmgrHookEventType event,
				  FmgrInfo *flinfo, Datum *private)
{
	struct
	{
		char	   *old_label;
		char	   *new_label;
		Datum		next_private;
	}		   *stack;

	switch (event)
	{
		case FHET_START:
			stack = (void *) DatumGetPointer(*private);
			if (!stack)
			{
				MemoryContext oldcxt;

				oldcxt = MemoryContextSwitchTo(flinfo->fn_mcxt);
				stack = palloc(sizeof(*stack));
				stack->old_label = NULL;
				stack->new_label = sepgsql_avc_trusted_proc(flinfo->fn_oid);
				stack->next_private = 0;

				MemoryContextSwitchTo(oldcxt);

				/*
				 * process:transition permission between old and new label,
				 * when user tries to switch security label of the client on
				 * execution of trusted procedure.
				 *
				 * 过程：当用户在执行可信过程时尝试切换客户端的安全标签时，在新旧标签之间转换权限。
				 *
				 * Also, db_procedure:entrypoint permission should be checked
				 * whether this procedure can perform as an entrypoint of the
				 * trusted procedure, or not. Note that db_procedure:execute
				 * permission shall be checked individually.
				 *
				 * 此外，还应检查 db_procedure:entrypoint 权限，以确定该过程是否可以作为受信任过程的入口点执行。请注意，应单独检查 db_procedure:execute 权限。
				 */
				if (stack->new_label)
				{
					ObjectAddress object;

					object.classId = ProcedureRelationId;
					object.objectId = flinfo->fn_oid;
					object.objectSubId = 0;
					sepgsql_avc_check_perms(&object,
											SEPG_CLASS_DB_PROCEDURE,
											SEPG_DB_PROCEDURE__ENTRYPOINT,
											getObjectDescription(&object, false),
											true);

					sepgsql_avc_check_perms_label(stack->new_label,
												  SEPG_CLASS_PROCESS,
												  SEPG_PROCESS__TRANSITION,
												  NULL, true);
				}
				*private = PointerGetDatum(stack);
			}
			Assert(!stack->old_label);
			if (stack->new_label)
			{
				stack->old_label = client_label_func;
				client_label_func = stack->new_label;
			}
			if (next_fmgr_hook)
				(*next_fmgr_hook) (event, flinfo, &stack->next_private);
			break;

		case FHET_END:
		case FHET_ABORT:
			stack = (void *) DatumGetPointer(*private);

			if (next_fmgr_hook)
				(*next_fmgr_hook) (event, flinfo, &stack->next_private);

			if (stack->new_label)
			{
				client_label_func = stack->old_label;
				stack->old_label = NULL;
			}
			break;

		default:
			elog(ERROR, "unexpected event type: %d", (int) event);
			break;
	}
}

/*
 * sepgsql_init_client_label
 *
 * Initializes the client security label and sets up related hooks for client
 * label management.
 *
 * 初始化客户端安全标签并设置客户端标签管理的相关挂钩。
 */
void
sepgsql_init_client_label(void)
{
	/*
	 * Set up dummy client label.
	 *
	 * 设置虚拟客户端标签。
	 *
	 * XXX - note that PostgreSQL launches background worker process like
	 * autovacuum without authentication steps. So, we initialize sepgsql_mode
	 * with SEPGSQL_MODE_INTERNAL, and client_label with the security context
	 * of server process. Later, it also launches background of user session.
	 * In this case, the process is always hooked on post-authentication, and
	 * we can initialize the sepgsql_mode and client_label correctly.
	 *
	 * XXX - 请注意，PostgreSQL 会像 autovacuum 一样启动后台工作进程，而无需身份验证步骤。因此，我们使用 SEPGSQL_MODE_INTERNAL 初始化 sepgsql_mode，并使用服务器进程的安全上下文初始化 client_label。随后，它还会启动用户会话的后台。在这种情况下，进程始终挂接在身份验证后，我们可以正确初始化 sepgsql_mode 和 client_label。
	 */
	if (getcon_raw(&client_label_peer) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: failed to get server security label: %m")));

	/* Client authentication hook
	 *
	 * 客户端身份验证挂钩
	 */
	next_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = sepgsql_client_auth;

	/* Trusted procedure hooks
	 *
	 * 可信过程挂钩
	 */
	next_needs_fmgr_hook = needs_fmgr_hook;
	needs_fmgr_hook = sepgsql_needs_fmgr_hook;

	next_fmgr_hook = fmgr_hook;
	fmgr_hook = sepgsql_fmgr_hook;

	/* Transaction/Sub-transaction callbacks
	 *
	 * 交易/子交易回调
	 */
	RegisterXactCallback(sepgsql_xact_callback, NULL);
	RegisterSubXactCallback(sepgsql_subxact_callback, NULL);
}

/*
 * sepgsql_get_label
 *
 * It returns a security context of the specified database object.
 * If unlabeled or incorrectly labeled, the system "unlabeled" label
 * shall be returned.
 *
 * 它返回指定数据库对象的安全上下文。若未贴标或贴标错误，系统“未贴标”标签应退回。
 */
char *
sepgsql_get_label(Oid classId, Oid objectId, int32 subId)
{
	ObjectAddress object;
	char	   *label;

	object.classId = classId;
	object.objectId = objectId;
	object.objectSubId = subId;

	label = GetSecurityLabel(&object, SEPGSQL_LABEL_TAG);
	if (!label || security_check_context_raw(label))
	{
		char	   *unlabeled;

		if (security_get_initial_context_raw("unlabeled", &unlabeled) < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SELinux: failed to get initial security label: %m")));
		PG_TRY();
		{
			label = pstrdup(unlabeled);
		}
		PG_FINALLY();
		{
			freecon(unlabeled);
		}
		PG_END_TRY();
	}
	return label;
}

/*
 * sepgsql_object_relabel
 *
 * An entrypoint of SECURITY LABEL statement
 *
 * SECURITY LABEL 语句的入口点
 */
void
sepgsql_object_relabel(const ObjectAddress *object, const char *seclabel)
{
	/*
	 * validate format of the supplied security label, if it is security
	 * context of selinux.
	 *
	 * 验证所提供的安全标签的格式（如果它是 selinux 的安全上下文）。
	 */
	if (seclabel &&
		security_check_context_raw(seclabel) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("SELinux: invalid security label: \"%s\"", seclabel)));

	/*
	 * Do actual permission checks for each object classes
	 *
	 * 对每个对象类进行实际的权限检查
	 */
	switch (object->classId)
	{
		case DatabaseRelationId:
			sepgsql_database_relabel(object->objectId, seclabel);
			break;

		case NamespaceRelationId:
			sepgsql_schema_relabel(object->objectId, seclabel);
			break;

		case RelationRelationId:
			if (object->objectSubId == 0)
				sepgsql_relation_relabel(object->objectId,
										 seclabel);
			else
				sepgsql_attribute_relabel(object->objectId,
										  object->objectSubId,
										  seclabel);
			break;

		case ProcedureRelationId:
			sepgsql_proc_relabel(object->objectId, seclabel);
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("sepgsql provider does not support labels on %s",
							getObjectTypeDescription(object, false))));
			break;
	}
}

/*
 * TEXT sepgsql_getcon(VOID)
 *
 * 文本 sepgsql_getcon(VOID)
 *
 * It returns the security label of the client.
 *
 * 它返回客户端的安全标签。
 */
PG_FUNCTION_INFO_V1(sepgsql_getcon);
Datum
sepgsql_getcon(PG_FUNCTION_ARGS)
{
	char	   *client_label;

	if (!sepgsql_is_enabled())
		PG_RETURN_NULL();

	client_label = sepgsql_get_client_label();

	PG_RETURN_TEXT_P(cstring_to_text(client_label));
}

/*
 * BOOL sepgsql_setcon(TEXT)
 *
 * BOOL sepgsql_setcon(TEXT)
 *
 * It switches the security label of the client.
 *
 * 它切换客户端的安全标签。
 */
PG_FUNCTION_INFO_V1(sepgsql_setcon);
Datum
sepgsql_setcon(PG_FUNCTION_ARGS)
{
	const char *new_label;

	if (PG_ARGISNULL(0))
		new_label = NULL;
	else
		new_label = TextDatumGetCString(PG_GETARG_DATUM(0));

	sepgsql_set_client_label(new_label);

	PG_RETURN_BOOL(true);
}

/*
 * TEXT sepgsql_mcstrans_in(TEXT)
 *
 * 文本 sepgsql_mcstrans_in(文本)
 *
 * It translate the given qualified MLS/MCS range into raw format
 * when mcstrans daemon is working.
 *
 * 当 mcstrans 守护进程工作时，它将给定的合格 MLS/MCS 范围转换为原始格式。
 */
PG_FUNCTION_INFO_V1(sepgsql_mcstrans_in);
Datum
sepgsql_mcstrans_in(PG_FUNCTION_ARGS)
{
	text	   *label = PG_GETARG_TEXT_PP(0);
	char	   *raw_label;
	char	   *result;

	if (!sepgsql_is_enabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql is not enabled")));

	if (selinux_trans_to_raw_context(text_to_cstring(label),
									 &raw_label) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: could not translate security label: %m")));

	PG_TRY();
	{
		result = pstrdup(raw_label);
	}
	PG_FINALLY();
	{
		freecon(raw_label);
	}
	PG_END_TRY();

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * TEXT sepgsql_mcstrans_out(TEXT)
 *
 * 文本 sepgsql_mcstrans_out(文本)
 *
 * It translate the given raw MLS/MCS range into qualified format
 * when mcstrans daemon is working.
 *
 * 当 mcstrans 守护进程运行时，它将给定的原始 MLS/MCS 范围转换为合格的格式。
 */
PG_FUNCTION_INFO_V1(sepgsql_mcstrans_out);
Datum
sepgsql_mcstrans_out(PG_FUNCTION_ARGS)
{
	text	   *label = PG_GETARG_TEXT_PP(0);
	char	   *qual_label;
	char	   *result;

	if (!sepgsql_is_enabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql is not currently enabled")));

	if (selinux_raw_to_trans_context(text_to_cstring(label),
									 &qual_label) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: could not translate security label: %m")));

	PG_TRY();
	{
		result = pstrdup(qual_label);
	}
	PG_FINALLY();
	{
		freecon(qual_label);
	}
	PG_END_TRY();

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * quote_object_name
 *
 * Concatenate as many of the given strings as aren't NULL, with dots between.
 * Quote any of the strings that wouldn't be valid identifiers otherwise.
 *
 * 连接尽可能多的不为 NULL 的给定字符串，中间用点连接。引用任何否则不是有效标识符的字符串。
 */
static char *
quote_object_name(const char *src1, const char *src2,
				  const char *src3, const char *src4)
{
	StringInfoData result;

	initStringInfo(&result);
	if (src1)
		appendStringInfoString(&result, quote_identifier(src1));
	if (src2)
		appendStringInfo(&result, ".%s", quote_identifier(src2));
	if (src3)
		appendStringInfo(&result, ".%s", quote_identifier(src3));
	if (src4)
		appendStringInfo(&result, ".%s", quote_identifier(src4));
	return result.data;
}

/*
 * exec_object_restorecon
 *
 * This routine is a helper called by sepgsql_restorecon; it set up
 * initial security labels of database objects within the supplied
 * catalog OID.
 *
 * 该例程是由 sepgsql_restorecon 调用的帮助程序；它在提供的目录 OID 中设置数据库对象的初始安全标签。
 */
static void
exec_object_restorecon(struct selabel_handle *sehnd, Oid catalogId)
{
	Relation	rel;
	SysScanDesc sscan;
	HeapTuple	tuple;
	char	   *database_name = get_database_name(MyDatabaseId);
	char	   *namespace_name;
	Oid			namespace_id;
	char	   *relation_name;

	/*
	 * Open the target catalog. We don't want to allow writable accesses by
	 * other session during initial labeling.
	 *
	 * 打开目标目录。我们不希望在初始标记期间允许其他会话进行可写访问。
	 */
	rel = table_open(catalogId, AccessShareLock);

	sscan = systable_beginscan(rel, InvalidOid, false,
							   NULL, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		Form_pg_database datForm;
		Form_pg_namespace nspForm;
		Form_pg_class relForm;
		Form_pg_attribute attForm;
		Form_pg_proc proForm;
		char	   *objname;
		int			objtype = 1234;
		ObjectAddress object;
		char	   *context;

		/*
		 * The way to determine object name depends on object classes. So, any
		 * branches set up `objtype', `objname' and `object' here.
		 *
		 * 确定对象名称的方式取决于对象类。因此，任何分支都在这里设置“objtype”、“objname”和“object”。
		 */
		switch (catalogId)
		{
			case DatabaseRelationId:
				datForm = (Form_pg_database) GETSTRUCT(tuple);

				objtype = SELABEL_DB_DATABASE;

				objname = quote_object_name(NameStr(datForm->datname),
											NULL, NULL, NULL);

				object.classId = DatabaseRelationId;
				object.objectId = datForm->oid;
				object.objectSubId = 0;
				break;

			case NamespaceRelationId:
				nspForm = (Form_pg_namespace) GETSTRUCT(tuple);

				objtype = SELABEL_DB_SCHEMA;

				objname = quote_object_name(database_name,
											NameStr(nspForm->nspname),
											NULL, NULL);

				object.classId = NamespaceRelationId;
				object.objectId = nspForm->oid;
				object.objectSubId = 0;
				break;

			case RelationRelationId:
				relForm = (Form_pg_class) GETSTRUCT(tuple);

				if (relForm->relkind == RELKIND_RELATION ||
					relForm->relkind == RELKIND_PARTITIONED_TABLE)
					objtype = SELABEL_DB_TABLE;
				else if (relForm->relkind == RELKIND_SEQUENCE)
					objtype = SELABEL_DB_SEQUENCE;
				else if (relForm->relkind == RELKIND_VIEW)
					objtype = SELABEL_DB_VIEW;
				else
					continue;	/* no need to assign security label */

				namespace_name = get_namespace_name(relForm->relnamespace);
				objname = quote_object_name(database_name,
											namespace_name,
											NameStr(relForm->relname),
											NULL);
				pfree(namespace_name);

				object.classId = RelationRelationId;
				object.objectId = relForm->oid;
				object.objectSubId = 0;
				break;

			case AttributeRelationId:
				attForm = (Form_pg_attribute) GETSTRUCT(tuple);

				if (get_rel_relkind(attForm->attrelid) != RELKIND_RELATION &&
					get_rel_relkind(attForm->attrelid) != RELKIND_PARTITIONED_TABLE)
					continue;	/* no need to assign security label */

				objtype = SELABEL_DB_COLUMN;

				namespace_id = get_rel_namespace(attForm->attrelid);
				namespace_name = get_namespace_name(namespace_id);
				relation_name = get_rel_name(attForm->attrelid);
				objname = quote_object_name(database_name,
											namespace_name,
											relation_name,
											NameStr(attForm->attname));
				pfree(namespace_name);
				pfree(relation_name);

				object.classId = RelationRelationId;
				object.objectId = attForm->attrelid;
				object.objectSubId = attForm->attnum;
				break;

			case ProcedureRelationId:
				proForm = (Form_pg_proc) GETSTRUCT(tuple);

				objtype = SELABEL_DB_PROCEDURE;

				namespace_name = get_namespace_name(proForm->pronamespace);
				objname = quote_object_name(database_name,
											namespace_name,
											NameStr(proForm->proname),
											NULL);
				pfree(namespace_name);

				object.classId = ProcedureRelationId;
				object.objectId = proForm->oid;
				object.objectSubId = 0;
				break;

			default:
				elog(ERROR, "unexpected catalog id: %u", catalogId);
				objname = NULL; /* for compiler quiet */
				break;
		}

		if (selabel_lookup_raw(sehnd, &context, objname, objtype) == 0)
		{
			PG_TRY();
			{
				/*
				 * Check SELinux permission to relabel the fetched object,
				 * then do the actual relabeling.
				 *
				 * 检查 SELinux 权限以重新标记所获取的对象，然后进行实际的重新标记。
				 */
				sepgsql_object_relabel(&object, context);

				SetSecurityLabel(&object, SEPGSQL_LABEL_TAG, context);
			}
			PG_FINALLY();
			{
				freecon(context);
			}
			PG_END_TRY();
		}
		else if (errno == ENOENT)
			ereport(WARNING,
					(errmsg("SELinux: no initial label assigned for %s (type=%d), skipping",
							objname, objtype)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SELinux: could not determine initial security label for %s (type=%d): %m", objname, objtype)));

		pfree(objname);
	}
	systable_endscan(sscan);

	table_close(rel, NoLock);
}

/*
 * BOOL sepgsql_restorecon(TEXT specfile)
 *
 * BOOL sepgsql_restorecon(TEXT 规范文件)
 *
 * This function tries to assign initial security labels on all the object
 * within the current database, according to the system setting.
 * It is typically invoked by sepgsql-install script just after initdb, to
 * assign initial security labels.
 *
 * 该函数尝试根据系统设置为当前数据库中的所有对象分配初始安全标签。它通常由 sepgsql-install 脚本在 initdb 之后调用，以分配初始安全标签。
 *
 * If @specfile is not NULL, it uses explicitly specified specfile, instead
 * of the system default.
 *
 * 如果@specfile不为NULL，则使用显式指定的specfile，而不是系统默认值。
 */
PG_FUNCTION_INFO_V1(sepgsql_restorecon);
Datum
sepgsql_restorecon(PG_FUNCTION_ARGS)
{
	struct selabel_handle *sehnd;
	struct selinux_opt seopts;

	/*
	 * SELinux has to be enabled on the running platform.
	 *
	 * 必须在运行平台上启用 SELinux。
	 */
	if (!sepgsql_is_enabled())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql is not currently enabled")));

	/*
	 * Check DAC permission. Only superuser can set up initial security
	 * labels, like root-user in filesystems
	 *
	 * 检查 DAC 权限。只有超级用户才能设置初始安全标签，例如文件系统中的 root 用户
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("SELinux: must be superuser to restore initial contexts")));

	/*
	 * Open selabel_lookup(3) stuff. It provides a set of mapping between an
	 * initial security label and object class/name due to the system setting.
	 *
	 * 打开selabel_lookup(3) 的东西。它根据系统设置提供了一组初始安全标签和对象类/名称之间的映射。
	 */
	if (PG_ARGISNULL(0))
	{
		seopts.type = SELABEL_OPT_UNUSED;
		seopts.value = NULL;
	}
	else
	{
		seopts.type = SELABEL_OPT_PATH;
		seopts.value = TextDatumGetCString(PG_GETARG_DATUM(0));
	}
	sehnd = selabel_open(SELABEL_CTX_DB, &seopts, 1);
	if (!sehnd)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: failed to initialize labeling handle: %m")));
	PG_TRY();
	{
		exec_object_restorecon(sehnd, DatabaseRelationId);
		exec_object_restorecon(sehnd, NamespaceRelationId);
		exec_object_restorecon(sehnd, RelationRelationId);
		exec_object_restorecon(sehnd, AttributeRelationId);
		exec_object_restorecon(sehnd, ProcedureRelationId);
	}
	PG_FINALLY();
	{
		selabel_close(sehnd);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(true);
}
