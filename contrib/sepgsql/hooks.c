/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/hooks.c
 *
 * Entrypoints of the hooks in PostgreSQL, and dispatches the callbacks.
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/dependency.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "commands/seclabel.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "sepgsql.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/queryenvironment.h"

PG_MODULE_MAGIC_EXT(
					.name = "sepgsql",
					.version = PG_VERSION
);

/*
 * Declarations
 */

/*
 * Saved hook entries (if stacked)
 *
 * 保存的钩子条目（如果堆叠）
 */
static object_access_hook_type next_object_access_hook = NULL;
static ExecutorCheckPerms_hook_type next_exec_check_perms_hook = NULL;
static ProcessUtility_hook_type next_ProcessUtility_hook = NULL;

/*
 * Contextual information on DDL commands
 *
 * 有关 DDL 命令的上下文信息
 */
typedef struct
{
	NodeTag		cmdtype;

	/*
	 * Name of the template database given by users on CREATE DATABASE
	 * command. Elsewhere (including the case of default) NULL.
	 *
	 * 用户在 CREATE DATABASE 命令中指定的模板数据库的名称。其他地方（包括默认情况）为NULL。
	 */
	const char *createdb_dtemplate;
} sepgsql_context_info_t;

static sepgsql_context_info_t sepgsql_context_info;

/*
 * GUC: sepgsql.permissive = (on|off)
 *
 * GUC: sepgsql.permissive = (开|关)
 */
static bool sepgsql_permissive = false;

bool
sepgsql_get_permissive(void)
{
	return sepgsql_permissive;
}

/*
 * GUC: sepgsql.debug_audit = (on|off)
 *
 * GUC: sepgsql.debug_audit = (开|关)
 */
static bool sepgsql_debug_audit = false;

bool
sepgsql_get_debug_audit(void)
{
	return sepgsql_debug_audit;
}

/*
 * sepgsql_object_access
 *
 * Entrypoint of the object_access_hook. This routine performs as
 * a dispatcher of invocation based on access type and object classes.
 *
 * object_access_hook 的入口点。该例程充当基于访问类型和对象类的调用调度程序。
 */
static void
sepgsql_object_access(ObjectAccessType access,
					  Oid classId,
					  Oid objectId,
					  int subId,
					  void *arg)
{
	if (next_object_access_hook)
		(*next_object_access_hook) (access, classId, objectId, subId, arg);

	switch (access)
	{
		case OAT_POST_CREATE:
			{
				ObjectAccessPostCreate *pc_arg = arg;
				bool		is_internal;

				is_internal = pc_arg ? pc_arg->is_internal : false;

				switch (classId)
				{
					case DatabaseRelationId:
						Assert(!is_internal);
						sepgsql_database_post_create(objectId,
													 sepgsql_context_info.createdb_dtemplate);
						break;

					case NamespaceRelationId:
						Assert(!is_internal);
						sepgsql_schema_post_create(objectId);
						break;

					case RelationRelationId:
						if (subId == 0)
						{
							/*
							 * The cases in which we want to apply permission
							 * checks on creation of a new relation correspond
							 * to direct user invocation.  For internal uses,
							 * that is creation of toast tables, index rebuild
							 * or ALTER TABLE commands, we need neither
							 * assignment of security labels nor permission
							 * checks.
							 *
							 * 我们想要在创建新关系时应用权限检查的情况对应于直接用户调用。  对于内部使用，即创建 toast 表、索引重建或 ALTER TABLE 命令，我们既不需要分配安全标签，也不需要权限检查。
							 */
							if (is_internal)
								break;

							sepgsql_relation_post_create(objectId);
						}
						else
							sepgsql_attribute_post_create(objectId, subId);
						break;

					case ProcedureRelationId:
						Assert(!is_internal);
						sepgsql_proc_post_create(objectId);
						break;

					default:
						/* Ignore unsupported object classes
						 *
						 * 忽略不支持的对象类
						 */
						break;
				}
			}
			break;

		case OAT_DROP:
			{
				ObjectAccessDrop *drop_arg = (ObjectAccessDrop *) arg;

				/*
				 * No need to apply permission checks on object deletion due
				 * to internal cleanups; such as removal of temporary database
				 * object on session closed.
				 *
				 * 无需因内部清理而对对象删除应用权限检查；例如在会话关闭时删除临时数据库对象。
				 */
				if ((drop_arg->dropflags & PERFORM_DELETION_INTERNAL) != 0)
					break;

				switch (classId)
				{
					case DatabaseRelationId:
						sepgsql_database_drop(objectId);
						break;

					case NamespaceRelationId:
						sepgsql_schema_drop(objectId);
						break;

					case RelationRelationId:
						if (subId == 0)
							sepgsql_relation_drop(objectId);
						else
							sepgsql_attribute_drop(objectId, subId);
						break;

					case ProcedureRelationId:
						sepgsql_proc_drop(objectId);
						break;

					default:
						/* Ignore unsupported object classes
						 *
						 * 忽略不支持的对象类
						 */
						break;
				}
			}
			break;

		case OAT_TRUNCATE:
			{
				switch (classId)
				{
					case RelationRelationId:
						sepgsql_relation_truncate(objectId);
						break;
					default:
						/* Ignore unsupported object classes
						 *
						 * 忽略不支持的对象类
						 */
						break;
				}
			}
			break;

		case OAT_POST_ALTER:
			{
				ObjectAccessPostAlter *pa_arg = arg;
				bool		is_internal = pa_arg->is_internal;

				switch (classId)
				{
					case DatabaseRelationId:
						Assert(!is_internal);
						sepgsql_database_setattr(objectId);
						break;

					case NamespaceRelationId:
						Assert(!is_internal);
						sepgsql_schema_setattr(objectId);
						break;

					case RelationRelationId:
						if (subId == 0)
						{
							/*
							 * A case when we don't want to apply permission
							 * check is that relation is internally altered
							 * without user's intention. E.g, no need to check
							 * on toast table/index to be renamed at end of
							 * the table rewrites.
							 *
							 * 我们不想应用权限检查的一种情况是，关系在用户无意的情况下被内部更改。例如，无需检查要在表重写结束时重命名的 toast 表/索引。
							 */
							if (is_internal)
								break;

							sepgsql_relation_setattr(objectId);
						}
						else
							sepgsql_attribute_setattr(objectId, subId);
						break;

					case ProcedureRelationId:
						Assert(!is_internal);
						sepgsql_proc_setattr(objectId);
						break;

					default:
						/* Ignore unsupported object classes
						 *
						 * 忽略不支持的对象类
						 */
						break;
				}
			}
			break;

		case OAT_NAMESPACE_SEARCH:
			{
				ObjectAccessNamespaceSearch *ns_arg = arg;

				/*
				 * If stacked extension already decided not to allow users to
				 * search this schema, we just stick with that decision.
				 *
				 * 如果堆叠扩展已经决定不允许用户搜索此模式，我们就坚持该决定。
				 */
				if (!ns_arg->result)
					break;

				Assert(classId == NamespaceRelationId);
				Assert(ns_arg->result);
				ns_arg->result
					= sepgsql_schema_search(objectId,
											ns_arg->ereport_on_violation);
			}
			break;

		case OAT_FUNCTION_EXECUTE:
			{
				Assert(classId == ProcedureRelationId);
				sepgsql_proc_execute(objectId);
			}
			break;

		default:
			elog(ERROR, "unexpected object access type: %d", (int) access);
			break;
	}
}

/*
 * sepgsql_exec_check_perms
 *
 * Entrypoint of DML permissions
 *
 * DML权限入口点
 */
static bool
sepgsql_exec_check_perms(List *rangeTbls, List *rteperminfos, bool abort)
{
	/*
	 * If security provider is stacking and one of them replied 'false' at
	 * least, we don't need to check any more.
	 *
	 * 如果安全提供者正在堆叠并且其中一个至少回答“假”，那么我们不需要再检查。
	 */
	if (next_exec_check_perms_hook &&
		!(*next_exec_check_perms_hook) (rangeTbls, rteperminfos, abort))
		return false;

	if (!sepgsql_dml_privileges(rangeTbls, rteperminfos, abort))
		return false;

	return true;
}

/*
 * sepgsql_utility_command
 *
 * It tries to rough-grained control on utility commands; some of them can
 * break whole of the things if nefarious user would use.
 *
 * 它尝试对实用程序命令进行粗粒度控制；如果恶意用户使用，其中一些可能会破坏整个系统。
 */
static void
sepgsql_utility_command(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	sepgsql_context_info_t saved_context_info = sepgsql_context_info;
	ListCell   *cell;

	PG_TRY();
	{
		/*
		 * Check command tag to avoid nefarious operations, and save the
		 * current contextual information to determine whether we should apply
		 * permission checks here, or not.
		 *
		 * 检查命令标签以避免恶意操作，并保存当前上下文信息以确定我们是否应该在此处应用权限检查。
		 */
		sepgsql_context_info.cmdtype = nodeTag(parsetree);

		switch (nodeTag(parsetree))
		{
			case T_CreatedbStmt:

				/*
				 * We hope to reference name of the source database, but it
				 * does not appear in system catalog. So, we save it here.
				 *
				 * 我们希望引用源数据库的名称，但它没有出现在系统目录中。所以，我们把它保存在这里。
				 */
				foreach(cell, ((CreatedbStmt *) parsetree)->options)
				{
					DefElem    *defel = (DefElem *) lfirst(cell);

					if (strcmp(defel->defname, "template") == 0)
					{
						sepgsql_context_info.createdb_dtemplate
							= strVal(defel->arg);
						break;
					}
				}
				break;

			case T_LoadStmt:

				/*
				 * We reject LOAD command across the board on enforcing mode,
				 * because a binary module can arbitrarily override hooks.
				 *
				 * 我们在强制模式下全面拒绝 LOAD 命令，因为二进制模块可以任意覆盖钩子。
				 */
				if (sepgsql_getenforce())
				{
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("SELinux: LOAD is not permitted")));
				}
				break;
			default:

				/*
				 * Right now we don't check any other utility commands,
				 * because it needs more detailed information to make access
				 * control decision here, but we don't want to have two parse
				 * and analyze routines individually.
				 *
				 * 现在我们不检查任何其他实用程序命令，因为它需要更详细的信息来做出访问控制决策，但我们不想单独有两个解析和分析例程。
				 */
				break;
		}

		if (next_ProcessUtility_hook)
			(*next_ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
										 context, params, queryEnv,
										 dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
	}
	PG_FINALLY();
	{
		sepgsql_context_info = saved_context_info;
	}
	PG_END_TRY();
}

/*
 * Module load callback
 *
 * 模块加载回调
 */
void
_PG_init(void)
{
	/*
	 * We allow to load the SE-PostgreSQL module on single-user-mode or
	 * shared_preload_libraries settings only.
	 *
	 * 我们允许仅在单用户模式或shared_preload_libraries 设置上加载SE-PostgreSQL 模块。
	 */
	if (IsUnderPostmaster)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("sepgsql must be loaded via \"shared_preload_libraries\"")));

	/*
	 * Check availability of SELinux on the platform. If disabled, we cannot
	 * activate any SE-PostgreSQL features, and we have to skip rest of
	 * initialization.
	 *
	 * 检查平台上 SELinux 的可用性。如果禁用，我们将无法激活任何 SE-PostgreSQL 功能，并且必须跳过其余的初始化。
	 */
	if (is_selinux_enabled() < 1)
	{
		sepgsql_set_mode(SEPGSQL_MODE_DISABLED);
		return;
	}

	/*
	 * sepgsql.permissive = (on|off)
	 *
	 * sepgsql.permissive =（开|关）
	 *
	 * This variable controls performing mode of SE-PostgreSQL on user's
	 * session.
	 *
	 * 该变量控制 SE-PostgreSQL 在用户会话上的执行模式。
	 */
	DefineCustomBoolVariable("sepgsql.permissive",
							 "Turn on/off permissive mode in SE-PostgreSQL",
							 NULL,
							 &sepgsql_permissive,
							 false,
							 PGC_SIGHUP,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * sepgsql.debug_audit = (on|off)
	 *
	 * sepgsql.debug_audit =（开|关）
	 *
	 * This variable allows users to turn on/off audit logs on access control
	 * decisions, independent from auditallow/auditdeny setting in the
	 * security policy. We intend to use this option for debugging purpose.
	 *
	 * 此变量允许用户打开/关闭访问控制决策的审核日志，独立于安全策略中的审核允许/审核拒绝设置。我们打算使用此选项进行调试。
	 */
	DefineCustomBoolVariable("sepgsql.debug_audit",
							 "Turn on/off debug audit messages",
							 NULL,
							 &sepgsql_debug_audit,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("sepgsql");

	/* Initialize userspace access vector cache
	 *
	 * 初始化用户空间访问向量缓存
	 */
	sepgsql_avc_init();

	/* Initialize security label of the client and related stuff
	 *
	 * 初始化客户端的安全标签及相关内容
	 */
	sepgsql_init_client_label();

	/* Security label provider hook
	 *
	 * 安全标签提供商挂钩
	 */
	register_label_provider(SEPGSQL_LABEL_TAG,
							sepgsql_object_relabel);

	/* Object access hook
	 *
	 * 对象访问钩子
	 */
	next_object_access_hook = object_access_hook;
	object_access_hook = sepgsql_object_access;

	/* DML permission check
	 *
	 * DML权限检查
	 */
	next_exec_check_perms_hook = ExecutorCheckPerms_hook;
	ExecutorCheckPerms_hook = sepgsql_exec_check_perms;

	/* ProcessUtility hook
	 *
	 * ProcessUtility 钩子
	 */
	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = sepgsql_utility_command;

	/* init contextual info
	 *
	 * 初始化上下文信息
	 */
	memset(&sepgsql_context_info, 0, sizeof(sepgsql_context_info));
}
