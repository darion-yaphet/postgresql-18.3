/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 *	  包含控制 POSTGRES 工具命令执行的函数。它曾经作为 Lisp 系统和 C
 *	  系统之间的接口。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *
 * 标识
 *	  src/backend/tcop/utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/event_trigger.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/lockcmds.h"
#include "commands/matview.h"
#include "commands/policy.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/subscriptioncmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

/*
 * Hook for plugins to get control in ProcessUtility()
 *
 * 供插件在 ProcessUtility() 中取得控制权的钩子。
 */
ProcessUtility_hook_type ProcessUtility_hook = NULL;

/*
 * local function declarations
 *
 * 本地函数声明。
 */
static int	ClassifyUtilityCommandAsReadOnly(Node *parsetree);
static void ProcessUtilitySlow(ParseState *pstate,
							   PlannedStmt *pstmt,
							   const char *queryString,
							   ProcessUtilityContext context,
							   ParamListInfo params,
							   QueryEnvironment *queryEnv,
							   DestReceiver *dest,
							   QueryCompletion *qc);
static void ExecDropStmt(DropStmt *stmt, bool isTopLevel);

/*
 * CommandIsReadOnly: is an executable query read-only?
 *
 * CommandIsReadOnly：可执行查询是否为只读？
 *
 * This is a much stricter test than we apply for XactReadOnly mode;
 * the query must be *in truth* read-only, because the caller wishes
 * not to do CommandCounterIncrement for it.
 *
 * 这个检查比我们对 XactReadOnly 模式应用的检查严格得多；查询必须
 * 真正只读，因为调用者希望不为它执行 CommandCounterIncrement。
 *
 * Note: currently no need to support raw or analyzed queries here
 *
 * 注意：目前这里不需要支持原始查询或已分析查询。
 *
 * The function inspects the PlannedStmt command type, rejects row-locking
 * SELECTs and data-modifying CTEs, and treats utility commands as read/write.
 *
 * 此函数检查 PlannedStmt 的命令类型，拒绝带行锁的 SELECT 和修改数据的 CTE，
 * 并将工具命令视为读写命令。
 */
bool
CommandIsReadOnly(PlannedStmt *pstmt)
{
	Assert(IsA(pstmt, PlannedStmt));
	switch (pstmt->commandType)
	{
		case CMD_SELECT:
			if (pstmt->rowMarks != NIL)
				return false;	/* SELECT FOR [KEY] UPDATE/SHARE
								 *
								 * SELECT FOR [KEY] UPDATE/SHARE 查询
								 */
			else if (pstmt->hasModifyingCTE)
				return false;	/* data-modifying CTE
								 *
								 * 修改数据的 CTE
								 */
			else
				return true;
		case CMD_UPDATE:
		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_MERGE:
			return false;
		case CMD_UTILITY:
			/*
			 * For now, treat all utility commands as read/write
			 *
			 * 目前，将所有工具命令都视为读写命令。
			 */
			return false;
		default:
			elog(WARNING, "unrecognized commandType: %d",
				 (int) pstmt->commandType);
			break;
	}
	return false;
}

/*
 * Determine the degree to which a utility command is read only.
 *
 * 确定工具命令的只读程度。
 *
 * Note the definitions of the relevant flags in src/include/tcop/utility.h.
 *
 * 注意 src/include/tcop/utility.h 中相关标志的定义。
 *
 * The switch maps each utility node type to the read-only, recovery, and
 * parallel-mode flags that the caller should enforce.
 *
 * 该 switch 将每种工具节点类型映射到调用者应强制执行的只读、恢复和并行
 * 模式标志。
 */
static int
ClassifyUtilityCommandAsReadOnly(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_AlterCollationStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDatabaseStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_AlterDomainStmt:
		case T_AlterEnumStmt:
		case T_AlterEventTrigStmt:
		case T_AlterExtensionContentsStmt:
		case T_AlterExtensionStmt:
		case T_AlterFdwStmt:
		case T_AlterForeignServerStmt:
		case T_AlterFunctionStmt:
		case T_AlterObjectDependsStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOpFamilyStmt:
		case T_AlterOperatorStmt:
		case T_AlterOwnerStmt:
		case T_AlterPolicyStmt:
		case T_AlterPublicationStmt:
		case T_AlterRoleSetStmt:
		case T_AlterRoleStmt:
		case T_AlterSeqStmt:
		case T_AlterStatsStmt:
		case T_AlterSubscriptionStmt:
		case T_AlterTSConfigurationStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTableMoveAllStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterTableStmt:
		case T_AlterTypeStmt:
		case T_AlterUserMappingStmt:
		case T_CommentStmt:
		case T_CompositeTypeStmt:
		case T_CreateAmStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreateDomainStmt:
		case T_CreateEnumStmt:
		case T_CreateEventTrigStmt:
		case T_CreateExtensionStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateForeignTableStmt:
		case T_CreateFunctionStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_CreatePLangStmt:
		case T_CreatePolicyStmt:
		case T_CreatePublicationStmt:
		case T_CreateRangeStmt:
		case T_CreateRoleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStatsStmt:
		case T_CreateStmt:
		case T_CreateSubscriptionStmt:
		case T_CreateTableAsStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTransformStmt:
		case T_CreateTrigStmt:
		case T_CreateUserMappingStmt:
		case T_CreatedbStmt:
		case T_DefineStmt:
		case T_DropOwnedStmt:
		case T_DropRoleStmt:
		case T_DropStmt:
		case T_DropSubscriptionStmt:
		case T_DropTableSpaceStmt:
		case T_DropUserMappingStmt:
		case T_DropdbStmt:
		case T_GrantRoleStmt:
		case T_GrantStmt:
		case T_ImportForeignSchemaStmt:
		case T_IndexStmt:
		case T_ReassignOwnedStmt:
		case T_RefreshMatViewStmt:
		case T_RenameStmt:
		case T_RuleStmt:
		case T_SecLabelStmt:
		case T_TruncateStmt:
		case T_ViewStmt:
			{
				/*
				 * DDL is not read-only, and neither is TRUNCATE.
				 *
				 * DDL 不是只读的，TRUNCATE 也不是。
				 */
				return COMMAND_IS_NOT_READ_ONLY;
			}

		case T_AlterSystemStmt:
			{
				/*
				 * Surprisingly, ALTER SYSTEM meets all our definitions of
				 * read-only: it changes nothing that affects the output of
				 * pg_dump, it doesn't write WAL or imperil the application of
				 * future WAL, and it doesn't depend on any state that needs
				 * to be synchronized with parallel workers.
				 *
				 * 出人意料的是，ALTER SYSTEM 符合我们对只读的所有定义：
				 * 它不会改变影响 pg_dump 输出的任何内容，不会写 WAL 或危及
				 * 未来 WAL 的应用，也不依赖任何需要与并行工作进程同步的状态。
				 *
				 * So, despite the fact that it writes to a file, it's read
				 * only!
				 *
				 * 因此，尽管它会写入文件，它仍然是只读的！
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_CallStmt:
		case T_DoStmt:
			{
				/*
				 * Commands inside the DO block or the called procedure might
				 * not be read only, but they'll be checked separately when we
				 * try to execute them.  Here we only need to worry about the
				 * DO or CALL command itself.
				 *
				 * DO 块或被调用过程内部的命令可能不是只读的，但在尝试执行它们时
				 * 会分别检查。这里我们只需要关注 DO 或 CALL 命令本身。
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_CheckPointStmt:
			{
				/*
				 * You might think that this should not be permitted in
				 * recovery, but we interpret a CHECKPOINT command during
				 * recovery as a request for a restartpoint instead. We allow
				 * this since it can be a useful way of reducing switchover
				 * time when using various forms of replication.
				 *
				 * 你可能认为这在恢复期间不应被允许，但我们把恢复期间的 CHECKPOINT
				 * 命令解释为请求一个 restartpoint。我们允许这样做，因为在使用
				 * 各种复制形式时，它可能是减少切换时间的有用方式。
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ClosePortalStmt:
		case T_ConstraintsSetStmt:
		case T_DeallocateStmt:
		case T_DeclareCursorStmt:
		case T_DiscardStmt:
		case T_ExecuteStmt:
		case T_FetchStmt:
		case T_LoadStmt:
		case T_PrepareStmt:
		case T_UnlistenStmt:
		case T_VariableSetStmt:
			{
				/*
				 * These modify only backend-local state, so they're OK to run
				 * in a read-only transaction or on a standby. However, they
				 * are disallowed in parallel mode, because they either rely
				 * upon or modify backend-local state that might not be
				 * synchronized among cooperating backends.
				 *
				 * 这些命令只修改后端本地状态，因此可以在只读事务或备用服务器上
				 * 运行。但是它们在并行模式中被禁止，因为它们依赖或修改的后端本地
				 * 状态可能不会在协作后端之间同步。
				 */
				return COMMAND_OK_IN_RECOVERY | COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_ClusterStmt:
		case T_ReindexStmt:
		case T_VacuumStmt:
			{
				/*
				 * These commands write WAL, so they're not strictly
				 * read-only, and running them in parallel workers isn't
				 * supported.
				 *
				 * 这些命令会写 WAL，因此严格来说并非只读，并且不支持在并行工作
				 * 进程中运行它们。
				 *
				 * However, they don't change the database state in a way that
				 * would affect pg_dump output, so it's fine to run them in a
				 * read-only transaction. (CLUSTER might change the order of
				 * rows on disk, which could affect the ordering of pg_dump
				 * output, but that's not semantically significant.)
				 *
				 * 不过，它们不会以影响 pg_dump 输出的方式改变数据库状态，因此可以
				 * 在只读事务中运行它们。（CLUSTER 可能改变磁盘上行的顺序，从而
				 * 影响 pg_dump 输出的排序，但这在语义上并不重要。）
				 */
				return COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;

				/*
				 * You might think that COPY FROM is not at all read only, but
				 * it's OK to copy into a temporary table, because that
				 * wouldn't change the output of pg_dump.  If the target table
				 * turns out to be non-temporary, DoCopy itself will call
				 * PreventCommandIfReadOnly.
				 *
				 * 你可能认为 COPY FROM 完全不是只读的，但复制到临时表是可以的，
				 * 因为那不会改变 pg_dump 的输出。如果目标表最终不是临时表，
				 * DoCopy 自身会调用 PreventCommandIfReadOnly。
				 */
				if (stmt->is_from)
					return COMMAND_OK_IN_READ_ONLY_TXN;
				else
					return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ExplainStmt:
		case T_VariableShowStmt:
			{
				/*
				 * These commands don't modify any data and are safe to run in
				 * a parallel worker.
				 *
				 * 这些命令不会修改任何数据，可以安全地在并行工作进程中运行。
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ListenStmt:
		case T_NotifyStmt:
			{
				/*
				 * NOTIFY requires an XID assignment, so it can't be permitted
				 * on a standby. Perhaps LISTEN could, since without NOTIFY it
				 * would be OK to just do nothing, at least until promotion,
				 * but we currently prohibit it lest the user get the wrong
				 * idea.
				 *
				 * NOTIFY 需要分配 XID，因此不能允许它在备用服务器上执行。也许
				 * LISTEN 可以，因为没有 NOTIFY 时，至少在提升之前什么都不做也是
				 * 可以的，但我们目前禁止它，以免用户产生错误理解。
				 *
				 * (We do allow T_UnlistenStmt on a standby, though, because
				 * it's a no-op.)
				 *
				 * （不过，我们确实允许在备用服务器上执行 T_UnlistenStmt，因为它
				 * 是空操作。）
				 */
				return COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_LockStmt:
			{
				LockStmt   *stmt = (LockStmt *) parsetree;

				/*
				 * Only weaker locker modes are allowed during recovery. The
				 * restrictions here must match those in
				 * LockAcquireExtended().
				 *
				 * 恢复期间只允许较弱的加锁模式。这里的限制必须与
				 * LockAcquireExtended() 中的限制一致。
				 */
				if (stmt->mode > RowExclusiveLock)
					return COMMAND_OK_IN_READ_ONLY_TXN;
				else
					return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				/*
				 * PREPARE, COMMIT PREPARED, and ROLLBACK PREPARED all write
				 * WAL, so they're not read-only in the strict sense; but the
				 * first and third do not change pg_dump output, so they're OK
				 * in a read-only transactions.
				 *
				 * PREPARE、COMMIT PREPARED 和 ROLLBACK PREPARED 都会写 WAL，
				 * 因此严格来说它们不是只读的；但第一个和第三个不会改变 pg_dump
				 * 输出，因此可以在只读事务中执行。
				 *
				 * We also consider COMMIT PREPARED to be OK in a read-only
				 * transaction environment, by way of exception.
				 *
				 * 作为例外，我们也认为 COMMIT PREPARED 可以在只读事务环境中执行。
				 */
				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
					case TRANS_STMT_COMMIT:
					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_SAVEPOINT:
					case TRANS_STMT_RELEASE:
					case TRANS_STMT_ROLLBACK_TO:
						return COMMAND_IS_STRICTLY_READ_ONLY;

					case TRANS_STMT_PREPARE:
					case TRANS_STMT_COMMIT_PREPARED:
					case TRANS_STMT_ROLLBACK_PREPARED:
						return COMMAND_OK_IN_READ_ONLY_TXN;
				}
				elog(ERROR, "unrecognized TransactionStmtKind: %d",
					 (int) stmt->kind);
				return 0;		/* silence stupider compilers
								 *
								 * 让不够聪明的编译器安静。
								 */
			}

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			return 0;			/* silence stupider compilers
								 *
								 * 让不够聪明的编译器安静。
								 */
	}
}

/*
 * PreventCommandIfReadOnly: throw error if XactReadOnly
 *
 * PreventCommandIfReadOnly：如果 XactReadOnly 为真则抛出错误。
 *
 * This is useful partly to ensure consistency of the error message wording;
 * some callers have checked XactReadOnly for themselves.
 *
 * 这部分用于确保错误消息措辞一致；有些调用者已经自行检查过
 * XactReadOnly。
 *
 * The flow is a single state check followed by a standardized ERROR report.
 *
 * 核心流程是先做一次状态检查，然后报告标准化的 ERROR。
 */
void
PreventCommandIfReadOnly(const char *cmdname)
{
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/*
		 * translator: %s is name of a SQL command, eg CREATE
		 *
		 * translator：%s 是 SQL 命令名，例如 CREATE。
		 */
				 errmsg("cannot execute %s in a read-only transaction",
						cmdname)));
}

/*
 * PreventCommandIfParallelMode: throw error if current (sub)transaction is
 * in parallel mode.
 *
 * PreventCommandIfParallelMode：如果当前（子）事务处于并行模式则抛出错误。
 *
 * This is useful partly to ensure consistency of the error message wording;
 * some callers have checked IsInParallelMode() for themselves.
 *
 * 这部分用于确保错误消息措辞一致；有些调用者已经自行检查过
 * IsInParallelMode()。
 *
 * The flow is a single parallel-mode check followed by a standardized ERROR
 * report.
 *
 * 核心流程是先做一次并行模式检查，然后报告标准化的 ERROR。
 */
void
PreventCommandIfParallelMode(const char *cmdname)
{
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
		/*
		 * translator: %s is name of a SQL command, eg CREATE
		 *
		 * translator：%s 是 SQL 命令名，例如 CREATE。
		 */
				 errmsg("cannot execute %s during a parallel operation",
						cmdname)));
}

/*
 * PreventCommandDuringRecovery: throw error if RecoveryInProgress
 *
 * PreventCommandDuringRecovery：如果 RecoveryInProgress 为真则抛出错误。
 *
 * The majority of operations that are unsafe in a Hot Standby
 * will be rejected by XactReadOnly tests.  However there are a few
 * commands that are allowed in "read-only" xacts but cannot be allowed
 * in Hot Standby mode.  Those commands should call this function.
 *
 * 在 Hot Standby 中不安全的大多数操作都会被 XactReadOnly 检查拒绝。
 * 不过有少数命令允许在“只读”事务中执行，却不能允许在 Hot Standby
 * 模式中执行。这些命令应调用此函数。
 *
 * The flow checks recovery state and emits the common recovery-time rejection
 * message when needed.
 *
 * 核心流程是检查恢复状态，并在需要时发出通用的恢复期间拒绝消息。
 */
void
PreventCommandDuringRecovery(const char *cmdname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/*
		 * translator: %s is name of a SQL command, eg CREATE
		 *
		 * translator：%s 是 SQL 命令名，例如 CREATE。
		 */
				 errmsg("cannot execute %s during recovery",
						cmdname)));
}

/*
 * CheckRestrictedOperation: throw error for hazardous command if we're
 * inside a security restriction context.
 *
 * CheckRestrictedOperation：如果在安全限制上下文中执行危险命令，则抛出
 * 错误。
 *
 * This is needed to protect session-local state for which there is not any
 * better-defined protection mechanism, such as ownership.
 *
 * 这是为了保护会话本地状态；这些状态没有更明确的保护机制，例如
 * 所有权。
 *
 * The flow checks the security-restricted flag and rejects the command before
 * it can alter protected session-local state.
 *
 * 核心流程是检查安全限制标志，并在命令能够修改受保护的会话本地状态之前
 * 拒绝它。
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		/*
		 * translator: %s is name of a SQL command, eg PREPARE
		 *
		 * translator：%s 是 SQL 命令名，例如 PREPARE。
		 */
				 errmsg("cannot execute %s within security-restricted operation",
						cmdname)));
}

/*
 * ProcessUtility
 *		general utility function invoker
 *
 * ProcessUtility
 *		通用工具函数调用入口。
 *
 *	pstmt: PlannedStmt wrapper for the utility statement
 *	queryString: original source text of command
 *	readOnlyTree: if true, pstmt's node tree must not be modified
 *	context: identifies source of statement (toplevel client command,
 *		non-toplevel client command, subcommand of a larger utility command)
 *	params: parameters to use during execution
 *	queryEnv: environment for parse through execution (e.g., ephemeral named
 *		tables like trigger transition tables).  May be NULL.
 *	dest: where to send results
 *	qc: where to store command completion status data.  May be NULL,
 *		but if not, then caller must have initialized it.
 *
 *	pstmt：工具语句的 PlannedStmt 包装。
 *	queryString：命令的原始源文本。
 *	readOnlyTree：如果为 true，则不得修改 pstmt 的节点树。
 *	context：标识语句来源（顶层客户端命令、非顶层客户端命令，或更大
 *		工具命令的子命令）。
 *	params：执行期间使用的参数。
 *	queryEnv：从解析到执行使用的环境（例如触发器转换表这类临时
 *		命名表）。
 *		可以为 NULL。
 *	dest：结果发送位置。
 *	qc：保存命令完成状态数据的位置。可以为 NULL；如果不为 NULL，则
 *		调用者必须已经初始化它。
 *
 * Caller MUST supply a queryString; it is not allowed (anymore) to pass NULL.
 * If you really don't have source text, you can pass a constant string,
 * perhaps "(query not available)".
 *
 * 调用者必须提供 queryString；现在不再允许传入 NULL。如果确实没有源文本，
 * 可以传入一个常量字符串，例如 "(query not available)"。
 *
 * Note for users of ProcessUtility_hook: the same queryString may be passed
 * to multiple invocations of ProcessUtility when processing a query string
 * containing multiple semicolon-separated statements.  One should use
 * pstmt->stmt_location and pstmt->stmt_len to identify the substring
 * containing the current statement.  Keep in mind also that some utility
 * statements (e.g., CREATE SCHEMA) will recurse to ProcessUtility to process
 * sub-statements, often passing down the same queryString, stmt_location,
 * and stmt_len that were given for the whole statement.
 *
 * ProcessUtility_hook 的用户请注意：处理包含多个分号分隔语句的查询
 * 字符串时，同一个 queryString 可能会传给多次 ProcessUtility 调用。
 * 应使用 pstmt->stmt_location 和 pstmt->stmt_len 来识别包含当前语句的子串。
 * 还要记住，有些工具语句（例如 CREATE SCHEMA）会递归调用 ProcessUtility
 * 处理子语句，并且通常会向下传递为整个语句给出的同一个 queryString、
 * stmt_location 和 stmt_len。
 *
 * The flow validates the utility PlannedStmt and query string, then delegates
 * either to ProcessUtility_hook or to standard_ProcessUtility.
 *
 * 核心流程是验证工具 PlannedStmt 和查询字符串，然后委托给 ProcessUtility_hook
 * 或 standard_ProcessUtility。
 */
void
ProcessUtility(PlannedStmt *pstmt,
			   const char *queryString,
			   bool readOnlyTree,
			   ProcessUtilityContext context,
			   ParamListInfo params,
			   QueryEnvironment *queryEnv,
			   DestReceiver *dest,
			   QueryCompletion *qc)
{
	Assert(IsA(pstmt, PlannedStmt));
	Assert(pstmt->commandType == CMD_UTILITY);
	Assert(queryString != NULL);	/* required as of 8.4
									 *
									 * 自 8.4 起为必需。
									 */
	Assert(qc == NULL || qc->commandTag == CMDTAG_UNKNOWN);

	/*
	 * We provide a function hook variable that lets loadable plugins get
	 * control when ProcessUtility is called.  Such a plugin would normally
	 * call standard_ProcessUtility().
	 *
	 * 我们提供一个函数钩子变量，让可加载插件在 ProcessUtility 被调用时取得
	 * 控制权。这样的插件通常会调用 standard_ProcessUtility()。
	 */
	if (ProcessUtility_hook)
		(*ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
}

/*
 * standard_ProcessUtility itself deals only with utility commands for
 * which we do not provide event trigger support.  Commands that do have
 * such support are passed down to ProcessUtilitySlow, which contains the
 * necessary infrastructure for such triggers.
 *
 * standard_ProcessUtility 自身只处理那些我们没有提供事件触发器支持的工具
 * 命令。具备此类支持的命令会下传给 ProcessUtilitySlow，后者包含这些触发器
 * 所需的基础设施。
 *
 * This division is not just for performance: it's critical that the
 * event trigger code not be invoked when doing START TRANSACTION for
 * example, because we might need to refresh the event trigger cache,
 * which requires being in a valid transaction.
 *
 * 这种划分不只是为了性能：例如在执行 START TRANSACTION 时不能调用事件
 * 触发器代码，这一点很关键，因为我们可能需要刷新事件触发器缓存，而这
 * 要求已经处于有效事务中。
 *
 * When adding or moving utility commands, check that the documentation in
 * event-trigger.sgml is kept up to date.
 *
 * 添加或移动工具命令时，请检查 event-trigger.sgml 中的文档是否保持最新。
 *
 * The flow copies read-only trees when needed, enforces read-only restrictions,
 * builds a ParseState, dispatches fast-path commands directly, and sends
 * event-trigger-capable commands to ProcessUtilitySlow.
 *
 * 核心流程是在需要时复制只读树，强制执行只读限制，构建 ParseState，
 * 直接分发快速路径命令，并将支持事件触发器的命令发送给 ProcessUtilitySlow。
 */
void
standard_ProcessUtility(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	Node	   *parsetree;
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isAtomicContext = (!(context == PROCESS_UTILITY_TOPLEVEL || context == PROCESS_UTILITY_QUERY_NONATOMIC) || IsTransactionBlock());
	ParseState *pstate;
	int			readonly_flags;

	/*
	 * This can recurse, so check for excessive recursion
	 *
	 * 这里可能递归，因此检查是否递归过深。
	 */
	check_stack_depth();

	/*
	 * If the given node tree is read-only, make a copy to ensure that parse
	 * transformations don't damage the original tree.  This could be
	 * refactored to avoid making unnecessary copies in more cases, but it's
	 * not clear that it's worth a great deal of trouble over.  Statements
	 * that are complex enough to be expensive to copy are exactly the ones
	 * we'd need to copy, so that only marginal savings seem possible.
	 *
	 * 如果给定的节点树是只读的，则复制一份，以确保解析转换不会破坏
	 * 原始树。可以重构这里，以便在更多情况下避免不必要的复制，但并不
	 * 清楚这是否值得花费大量精力。复杂到复制成本很高的语句，恰恰
	 * 就是我们需要复制的语句，
	 * 因此似乎只能节省很少的开销。
	 */
	if (readOnlyTree)
		pstmt = copyObject(pstmt);
	parsetree = pstmt->utilityStmt;

	/*
	 * Prohibit read/write commands in read-only states.
	 *
	 * 在只读状态下禁止读写命令。
	 */
	readonly_flags = ClassifyUtilityCommandAsReadOnly(parsetree);
	if (readonly_flags != COMMAND_IS_STRICTLY_READ_ONLY &&
		(XactReadOnly || IsInParallelMode()))
	{
		CommandTag	commandtag = CreateCommandTag(parsetree);

		if ((readonly_flags & COMMAND_OK_IN_READ_ONLY_TXN) == 0)
			PreventCommandIfReadOnly(GetCommandTagName(commandtag));
		if ((readonly_flags & COMMAND_OK_IN_PARALLEL_MODE) == 0)
			PreventCommandIfParallelMode(GetCommandTagName(commandtag));
		if ((readonly_flags & COMMAND_OK_IN_RECOVERY) == 0)
			PreventCommandDuringRecovery(GetCommandTagName(commandtag));
	}

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	pstate->p_queryEnv = queryEnv;

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 *
			 * ******************** 事务 ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * START TRANSACTION, as defined by SQL99: Identical
						 * to BEGIN.  Same code for both.
						 *
						 * SQL99 定义的 START TRANSACTION：与 BEGIN 相同。
						 * 两者使用同一套代码。
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariable("transaction_isolation",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariable("transaction_read_only",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_deferrable") == 0)
									SetPGVariable("transaction_deferrable",
												  list_make1(item->arg),
												  true);
							}
						}
						break;

					case TRANS_STMT_COMMIT:
						if (!EndTransactionBlock(stmt->chain))
						{
							/*
							 * report unsuccessful commit in qc
							 *
							 * 在 qc 中报告未成功的提交。
							 */
							if (qc)
								SetQueryCompletion(qc, CMDTAG_ROLLBACK, 0);
						}
						break;

					case TRANS_STMT_PREPARE:
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/*
							 * report unsuccessful commit in qc
							 *
							 * 在 qc 中报告未成功的提交。
							 */
							if (qc)
								SetQueryCompletion(qc, CMDTAG_ROLLBACK, 0);
						}
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						PreventInTransactionBlock(isTopLevel, "COMMIT PREPARED");
						FinishPreparedTransaction(stmt->gid, true);
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						PreventInTransactionBlock(isTopLevel, "ROLLBACK PREPARED");
						FinishPreparedTransaction(stmt->gid, false);
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock(stmt->chain);
						break;

					case TRANS_STMT_SAVEPOINT:
						RequireTransactionBlock(isTopLevel, "SAVEPOINT");
						DefineSavepoint(stmt->savepoint_name);
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionBlock(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->savepoint_name);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionBlock(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->savepoint_name);

						/*
						 * CommitTransactionCommand is in charge of
						 * re-defining the savepoint again
						 *
						 * CommitTransactionCommand 负责再次重新定义保存点。
						 */
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 *
			 * Portal（游标）操作。
			 */
		case T_DeclareCursorStmt:
			PerformCursorOpen(pstate, (DeclareCursorStmt *) parsetree, params,
							  isTopLevel);
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest, qc);
			break;

		case T_DoStmt:
			ExecuteDoStmt(pstate, (DoStmt *) parsetree, isAtomicContext);
			break;

		case T_CreateTableSpaceStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			PreventInTransactionBlock(isTopLevel, "CREATE TABLESPACE");
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
			break;

		case T_DropTableSpaceStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			PreventInTransactionBlock(isTopLevel, "DROP TABLESPACE");
			DropTableSpace((DropTableSpaceStmt *) parsetree);
			break;

		case T_AlterTableSpaceOptionsStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			AlterTableSpaceOptions((AlterTableSpaceOptionsStmt *) parsetree);
			break;

		case T_TruncateStmt:
			ExecuteTruncate((TruncateStmt *) parsetree);
			break;

		case T_CopyStmt:
			{
				uint64		processed;

				DoCopy(pstate, (CopyStmt *) parsetree,
					   pstmt->stmt_location, pstmt->stmt_len,
					   &processed);
				if (qc)
					SetQueryCompletion(qc, CMDTAG_COPY, processed);
			}
			break;

		case T_PrepareStmt:
			CheckRestrictedOperation("PREPARE");
			PrepareQuery(pstate, (PrepareStmt *) parsetree,
						 pstmt->stmt_location, pstmt->stmt_len);
			break;

		case T_ExecuteStmt:
			ExecuteQuery(pstate,
						 (ExecuteStmt *) parsetree, NULL,
						 params,
						 dest, qc);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			GrantRole(pstate, (GrantRoleStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			PreventInTransactionBlock(isTopLevel, "CREATE DATABASE");
			createdb(pstate, (CreatedbStmt *) parsetree);
			break;

		case T_AlterDatabaseStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			AlterDatabase(pstate, (AlterDatabaseStmt *) parsetree, isTopLevel);
			break;

		case T_AlterDatabaseRefreshCollStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			AlterDatabaseRefreshColl((AlterDatabaseRefreshCollStmt *) parsetree);
			break;

		case T_AlterDatabaseSetStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
			break;

		case T_DropdbStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			PreventInTransactionBlock(isTopLevel, "DROP DATABASE");
			DropDatabase(pstate, (DropdbStmt *) parsetree);
			break;

			/*
			 * Query-level asynchronous notification
			 *
			 * 查询级异步通知。
			 */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				Async_Notify(stmt->conditionname, stmt->payload);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				CheckRestrictedOperation("LISTEN");

				/*
				 * We don't allow LISTEN in background processes, as there is
				 * no mechanism for them to collect NOTIFY messages, so they'd
				 * just block cleanout of the async SLRU indefinitely.
				 * (Authors of custom background workers could bypass this
				 * restriction by calling Async_Listen directly, but then it's
				 * on them to provide some mechanism to process the message
				 * queue.)  Note there seems no reason to forbid UNLISTEN.
				 *
				 * 我们不允许后台进程执行 LISTEN，因为它们没有机制收集 NOTIFY 消息，
				 * 因而只会无限期阻塞异步 SLRU 的清理。（自定义后台工作进程的作者
				 * 可以通过直接调用 Async_Listen 绕过此限制，但这样他们就要负责
				 * 提供某种机制来处理消息队列。）注意，似乎没有理由禁止 UNLISTEN。
				 */
				if (MyBackendType != B_BACKEND)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					/*
					 * translator: %s is name of a SQL command, eg LISTEN
					 *
					 * translator：%s 是 SQL 命令名，例如 LISTEN。
					 */
							 errmsg("cannot execute %s within a background process",
									"LISTEN")));

				Async_Listen(stmt->conditionname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				CheckRestrictedOperation("UNLISTEN");
				if (stmt->conditionname)
					Async_Unlisten(stmt->conditionname);
				else
					Async_UnlistenAll();
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary...
								 *
								 * 可能并非必要……
								 */
				/*
				 * Allowed names are restricted if you're not superuser
				 *
				 * 如果不是超级用户，则允许的名称会受到限制。
				 */
				load_file(stmt->filename, !superuser());
			}
			break;

		case T_CallStmt:
			ExecuteCallStmt(castNode(CallStmt, parsetree), params, isAtomicContext, dest);
			break;

		case T_ClusterStmt:
			cluster(pstate, (ClusterStmt *) parsetree, isTopLevel);
			break;

		case T_VacuumStmt:
			ExecVacuum(pstate, (VacuumStmt *) parsetree, isTopLevel);
			break;

		case T_ExplainStmt:
			ExplainQuery(pstate, (ExplainStmt *) parsetree, params, dest);
			break;

		case T_AlterSystemStmt:
			PreventInTransactionBlock(isTopLevel, "ALTER SYSTEM");
			AlterSystemSetConfigFile((AlterSystemStmt *) parsetree);
			break;

		case T_VariableSetStmt:
			ExecSetVariableStmt((VariableSetStmt *) parsetree, isTopLevel);
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/*
			 * should we allow DISCARD PLANS?
			 *
			 * 我们应当允许 DISCARD PLANS 吗？
			 */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
			break;

		case T_CreateEventTrigStmt:
			/*
			 * no event triggers on event triggers
			 *
			 * 事件触发器自身没有事件触发器。
			 */
			CreateEventTrigger((CreateEventTrigStmt *) parsetree);
			break;

		case T_AlterEventTrigStmt:
			/*
			 * no event triggers on event triggers
			 *
			 * 事件触发器自身没有事件触发器。
			 */
			AlterEventTrigger((AlterEventTrigStmt *) parsetree);
			break;

			/*
			 * ******************************** ROLE statements ****
			 *
			 * ******************************** 角色语句 ****
			 */
		case T_CreateRoleStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			CreateRole(pstate, (CreateRoleStmt *) parsetree);
			break;

		case T_AlterRoleStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			AlterRole(pstate, (AlterRoleStmt *) parsetree);
			break;

		case T_AlterRoleSetStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
			break;

		case T_DropRoleStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			DropRole((DropRoleStmt *) parsetree);
			break;

		case T_ReassignOwnedStmt:
			/*
			 * no event triggers for global objects
			 *
			 * 全局对象没有事件触发器。
			 */
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
			break;

		case T_LockStmt:

			/*
			 * Since the lock would just get dropped immediately, LOCK TABLE
			 * outside a transaction block is presumed to be user error.
			 *
			 * 由于该锁会立即被释放，事务块外的 LOCK TABLE 被视为用户错误。
			 */
			RequireTransactionBlock(isTopLevel, "LOCK TABLE");
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			WarnNoTransactionBlock(isTopLevel, "SET CONSTRAINTS");
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			if (!has_privs_of_role(GetUserId(), ROLE_PG_CHECKPOINT))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				/*
				 * translator: %s is name of a SQL command, eg CHECKPOINT
				 *
				 * translator：%s 是 SQL 命令名，例如 CHECKPOINT。
				 */
						 errmsg("permission denied to execute %s command",
								"CHECKPOINT"),
						 errdetail("Only roles with privileges of the \"%s\" role may execute this command.",
								   "pg_checkpoint")));

			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_WAIT |
							  (RecoveryInProgress() ? 0 : CHECKPOINT_FORCE));
			break;

			/*
			 * The following statements are supported by Event Triggers only
			 * in some cases, so we "fast path" them in the other cases.
			 *
			 * 以下语句只有在某些情况下才受事件触发器支持，因此在其他
			 * 情况下我们对它们使用“快速路径”。
			 */

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecuteGrantStmt(stmt);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->removeType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecDropStmt(stmt, isTopLevel);
			}
			break;

		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->renameType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecRenameStmt(stmt);
			}
			break;

		case T_AlterObjectDependsStmt:
			{
				AlterObjectDependsStmt *stmt = (AlterObjectDependsStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterObjectDependsStmt(stmt, NULL);
			}
			break;

		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterObjectSchemaStmt(stmt, NULL);
			}
			break;

		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterOwnerStmt(stmt);
			}
			break;

		case T_CommentStmt:
			{
				CommentStmt *stmt = (CommentStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					CommentObject(stmt);
				break;
			}

		case T_SecLabelStmt:
			{
				SecLabelStmt *stmt = (SecLabelStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecSecLabelStmt(stmt);
				break;
			}

		default:
			/*
			 * All other statement types have event trigger support
			 *
			 * 所有其他语句类型都有事件触发器支持。
			 */
			ProcessUtilitySlow(pstate, pstmt, queryString,
							   context, params, queryEnv,
							   dest, qc);
			break;
	}

	free_parsestate(pstate);

	/*
	 * Make effects of commands visible, for instance so that
	 * PreCommit_on_commit_actions() can see them (see for example bug
	 * #15631).
	 *
	 * 使命令效果可见，例如让 PreCommit_on_commit_actions() 能看到它们
	 * （参见 bug #15631）。
	 */
	CommandCounterIncrement();
}

/*
 * The "Slow" variant of ProcessUtility should only receive statements
 * supported by the event triggers facility.  Therefore, we always
 * perform the trigger support calls if the context allows it.
 *
 * ProcessUtility 的“Slow”变体只应接收事件触发器设施支持的语句。因此，
 * 只要上下文允许，我们总是执行触发器支持调用。
 *
 * The flow opens the event-trigger complete-query scope, fires start/end hooks
 * around the command switch, records the affected object, and guarantees
 * cleanup through PG_TRY/PG_FINALLY.
 *
 * 核心流程是打开事件触发器的完整查询作用域，在命令 switch 前后触发
 * 开始/结束钩子，记录受影响对象，并通过 PG_TRY/PG_FINALLY 保证清理。
 */
static void
ProcessUtilitySlow(ParseState *pstate,
				   PlannedStmt *pstmt,
				   const char *queryString,
				   ProcessUtilityContext context,
				   ParamListInfo params,
				   QueryEnvironment *queryEnv,
				   DestReceiver *dest,
				   QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isCompleteQuery = (context != PROCESS_UTILITY_SUBCOMMAND);
	bool		needCleanup;
	bool		commandCollected = false;
	ObjectAddress address;
	ObjectAddress secondaryObject = InvalidObjectAddress;

	/*
	 * All event trigger calls are done only when isCompleteQuery is true
	 *
	 * 只有在 isCompleteQuery 为 true 时才执行所有事件触发器调用。
	 */
	needCleanup = isCompleteQuery && EventTriggerBeginCompleteQuery();

	/*
	 * PG_TRY block is to ensure we call EventTriggerEndCompleteQuery
	 *
	 * PG_TRY 块用于确保我们调用 EventTriggerEndCompleteQuery。
	 */
	PG_TRY();
	{
		if (isCompleteQuery)
			EventTriggerDDLCommandStart(parsetree);

		switch (nodeTag(parsetree))
		{
				/*
				 * relation and attribute manipulation
				 *
				 * 关系和属性操作。
				 */
			case T_CreateSchemaStmt:
				CreateSchemaCommand((CreateSchemaStmt *) parsetree,
									queryString,
									pstmt->stmt_location,
									pstmt->stmt_len);

				/*
				 * EventTriggerCollectSimpleCommand called by
				 * CreateSchemaCommand
				 *
				 * EventTriggerCollectSimpleCommand 由 CreateSchemaCommand 调用。
				 */
				commandCollected = true;
				break;

			case T_CreateStmt:
			case T_CreateForeignTableStmt:
				{
					List	   *stmts;
					RangeVar   *table_rv = NULL;

					/*
					 * Run parse analysis ...
					 *
					 * 运行解析分析……
					 */
					stmts = transformCreateStmt((CreateStmt *) parsetree,
												queryString);

					/*
					 * ... and do it.  We can't use foreach() because we may
					 * modify the list midway through, so pick off the
					 * elements one at a time, the hard way.
					 *
					 * ……然后执行。我们不能使用 foreach()，因为可能会在中途修改
					 * 列表，所以只能用较麻烦的方式一次取出一个元素。
					 */
					while (stmts != NIL)
					{
						Node	   *stmt = (Node *) linitial(stmts);

						stmts = list_delete_first(stmts);

						if (IsA(stmt, CreateStmt))
						{
							CreateStmt *cstmt = (CreateStmt *) stmt;
							Datum		toast_options;
							const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;

							/*
							 * Remember transformed RangeVar for LIKE
							 *
							 * 为 LIKE 记住已转换的 RangeVar。
							 */
							table_rv = cstmt->relation;

							/*
							 * Create the table itself
							 *
							 * 创建表本身。
							 */
							address = DefineRelation(cstmt,
													 RELKIND_RELATION,
													 InvalidOid, NULL,
													 queryString);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);

							/*
							 * Let NewRelationCreateToastTable decide if this
							 * one needs a secondary relation too.
							 *
							 * 让 NewRelationCreateToastTable 决定这个对象是否也需要
							 * 一个辅助关系。
							 */
							CommandCounterIncrement();

							/*
							 * parse and validate reloptions for the toast
							 * table
							 *
							 * 解析并验证 toast 表的 reloptions。
							 */
							toast_options = transformRelOptions((Datum) 0,
																cstmt->options,
																"toast",
																validnsps,
																true,
																false);
							(void) heap_reloptions(RELKIND_TOASTVALUE,
												   toast_options,
												   true);

							NewRelationCreateToastTable(address.objectId,
														toast_options);
						}
						else if (IsA(stmt, CreateForeignTableStmt))
						{
							CreateForeignTableStmt *cstmt = (CreateForeignTableStmt *) stmt;

							/*
							 * Remember transformed RangeVar for LIKE
							 *
							 * 为 LIKE 记住已转换的 RangeVar。
							 */
							table_rv = cstmt->base.relation;

							/*
							 * Create the table itself
							 *
							 * 创建表本身。
							 */
							address = DefineRelation(&cstmt->base,
													 RELKIND_FOREIGN_TABLE,
													 InvalidOid, NULL,
													 queryString);
							CreateForeignTable(cstmt,
											   address.objectId);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);
						}
						else if (IsA(stmt, TableLikeClause))
						{
							/*
							 * Do delayed processing of LIKE options.  This
							 * will result in additional sub-statements for us
							 * to process.  Those should get done before any
							 * remaining actions, so prepend them to "stmts".
							 *
							 * 延迟处理 LIKE 选项。这会产生额外的子语句供我们处理。
							 * 这些子语句应在任何剩余动作之前完成，因此将它们前置到
							 * "stmts"。
							 */
							TableLikeClause *like = (TableLikeClause *) stmt;
							List	   *morestmts;

							Assert(table_rv != NULL);

							morestmts = expandTableLikeClause(table_rv, like);
							stmts = list_concat(morestmts, stmts);
						}
						else
						{
							/*
							 * Recurse for anything else.  Note the recursive
							 * call will stash the objects so created into our
							 * event trigger context.
							 *
							 * 对其他任何内容递归处理。注意，递归调用会把这样创建的对象
							 * 暂存到我们的事件触发器上下文中。
							 */
							PlannedStmt *wrapper;

							wrapper = makeNode(PlannedStmt);
							wrapper->commandType = CMD_UTILITY;
							wrapper->canSetTag = false;
							wrapper->utilityStmt = stmt;
							wrapper->stmt_location = pstmt->stmt_location;
							wrapper->stmt_len = pstmt->stmt_len;

							ProcessUtility(wrapper,
										   queryString,
										   false,
										   PROCESS_UTILITY_SUBCOMMAND,
										   params,
										   NULL,
										   None_Receiver,
										   NULL);
						}

						/*
						 * Need CCI between commands
						 *
						 * 命令之间需要 CCI。
						 */
						if (stmts != NIL)
							CommandCounterIncrement();
					}

					/*
					 * The multiple commands generated here are stashed
					 * individually, so disable collection below.
					 *
					 * 这里生成的多个命令会分别暂存，因此禁用下面的收集。
					 */
					commandCollected = true;
				}
				break;

			case T_AlterTableStmt:
				{
					AlterTableStmt *atstmt = (AlterTableStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
					ListCell   *cell;

					/*
					 * Disallow ALTER TABLE .. DETACH CONCURRENTLY in a
					 * transaction block or function.  (Perhaps it could be
					 * allowed in a procedure, but don't hold your breath.)
					 *
					 * 禁止在事务块或函数中执行 ALTER TABLE .. DETACH CONCURRENTLY。
					 * （也许可以允许在过程中执行，但不要抱太大希望。）
					 */
					foreach(cell, atstmt->cmds)
					{
						AlterTableCmd *cmd = (AlterTableCmd *) lfirst(cell);

						/*
						 * Disallow DETACH CONCURRENTLY in a transaction block
						 *
						 * 禁止在事务块中执行 DETACH CONCURRENTLY。
						 */
						if (cmd->subtype == AT_DetachPartition)
						{
							if (((PartitionCmd *) cmd->def)->concurrent)
								PreventInTransactionBlock(isTopLevel,
														  "ALTER TABLE ... DETACH CONCURRENTLY");
						}
					}

					/*
					 * Figure out lock mode, and acquire lock.  This also does
					 * basic permissions checks, so that we won't wait for a
					 * lock on (for example) a relation on which we have no
					 * permissions.
					 *
					 * 计算锁模式并获取锁。这也会做基本权限检查，从而避免在
					 * （例如）我们没有权限的关系上等待锁。
					 */
					lockmode = AlterTableGetLockLevel(atstmt->cmds);
					relid = AlterTableLookupRelation(atstmt, lockmode);

					if (OidIsValid(relid))
					{
						AlterTableUtilityContext atcontext;

						/*
						 * Set up info needed for recursive callbacks ...
						 *
						 * 设置递归回调所需的信息……
						 */
						atcontext.pstmt = pstmt;
						atcontext.queryString = queryString;
						atcontext.relid = relid;
						atcontext.params = params;
						atcontext.queryEnv = queryEnv;

						/*
						 * ... ensure we have an event trigger context ...
						 *
						 * ……确保我们拥有事件触发器上下文……
						 */
						EventTriggerAlterTableStart(parsetree);
						EventTriggerAlterTableRelid(relid);

						/*
						 * ... and do it
						 *
						 * ……然后执行。
						 */
						AlterTable(atstmt, lockmode, &atcontext);

						/*
						 * done
						 *
						 * 完成。
						 */
						EventTriggerAlterTableEnd();
					}
					else
						ereport(NOTICE,
								(errmsg("relation \"%s\" does not exist, skipping",
										atstmt->relation->relname)));
				}

				/*
				 * ALTER TABLE stashes commands internally
				 *
				 * ALTER TABLE 在内部暂存命令。
				 */
				commandCollected = true;
				break;

			case T_AlterDomainStmt:
				{
					AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

					/*
					 * Some or all of these functions are recursive to cover
					 * inherited things, so permission checks are done there.
					 *
					 * 这些函数中的一部分或全部会递归处理继承对象，因此
					 * 权限检查在那里完成。
					 */
					switch (stmt->subtype)
					{
						case 'T':	/* ALTER DOMAIN DEFAULT
									 *
									 * 修改域默认值
									 */

							/*
							 * Recursively alter column default for table and,
							 * if requested, for descendants
							 *
							 * 递归修改表的列默认值；如果请求，也修改后代的列默认值。
							 */
							address =
								AlterDomainDefault(stmt->typeName,
												   stmt->def);
							break;
						case 'N':	/* ALTER DOMAIN DROP NOT NULL
									 *
									 * 修改域以删除 NOT NULL
									 */
							address =
								AlterDomainNotNull(stmt->typeName,
												   false);
							break;
						case 'O':	/* ALTER DOMAIN SET NOT NULL
									 *
									 * 修改域以设置 NOT NULL
									 */
							address =
								AlterDomainNotNull(stmt->typeName,
												   true);
							break;
						case 'C':	/* ADD CONSTRAINT
									 *
									 * 添加约束
									 */
							address =
								AlterDomainAddConstraint(stmt->typeName,
														 stmt->def,
														 &secondaryObject);
							break;
						case 'X':	/* DROP CONSTRAINT
									 *
									 * 删除约束
									 */
							address =
								AlterDomainDropConstraint(stmt->typeName,
														  stmt->name,
														  stmt->behavior,
														  stmt->missing_ok);
							break;
						case 'V':	/* VALIDATE CONSTRAINT
									 *
									 * 验证约束
									 */
							address =
								AlterDomainValidateConstraint(stmt->typeName,
															  stmt->name);
							break;
						default:	/* oops
									 *
									 * 出错了
									 */
							elog(ERROR, "unrecognized alter domain type: %d",
								 (int) stmt->subtype);
							break;
					}
				}
				break;

				/*
				 * ************* object creation / destruction **************
				 *
				 * ************* 对象创建 / 删除 **************
				 */
			case T_DefineStmt:
				{
					DefineStmt *stmt = (DefineStmt *) parsetree;

					switch (stmt->kind)
					{
						case OBJECT_AGGREGATE:
							address =
								DefineAggregate(pstate, stmt->defnames, stmt->args,
												stmt->oldstyle,
												stmt->definition,
												stmt->replace);
							break;
						case OBJECT_OPERATOR:
							Assert(stmt->args == NIL);
							address = DefineOperator(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TYPE:
							Assert(stmt->args == NIL);
							address = DefineType(pstate,
												 stmt->defnames,
												 stmt->definition);
							break;
						case OBJECT_TSPARSER:
							Assert(stmt->args == NIL);
							address = DefineTSParser(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TSDICTIONARY:
							Assert(stmt->args == NIL);
							address = DefineTSDictionary(stmt->defnames,
														 stmt->definition);
							break;
						case OBJECT_TSTEMPLATE:
							Assert(stmt->args == NIL);
							address = DefineTSTemplate(stmt->defnames,
													   stmt->definition);
							break;
						case OBJECT_TSCONFIGURATION:
							Assert(stmt->args == NIL);
							address = DefineTSConfiguration(stmt->defnames,
															stmt->definition,
															&secondaryObject);
							break;
						case OBJECT_COLLATION:
							Assert(stmt->args == NIL);
							address = DefineCollation(pstate,
													  stmt->defnames,
													  stmt->definition,
													  stmt->if_not_exists);
							break;
						default:
							elog(ERROR, "unrecognized define stmt type: %d",
								 (int) stmt->kind);
							break;
					}
				}
				break;

			case T_IndexStmt:	/* CREATE INDEX
								 *
								 * 创建索引
								 */
				{
					IndexStmt  *stmt = (IndexStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
					int			nparts = -1;
					bool		is_alter_table;

					if (stmt->concurrent)
						PreventInTransactionBlock(isTopLevel,
												  "CREATE INDEX CONCURRENTLY");

					/*
					 * Look up the relation OID just once, right here at the
					 * beginning, so that we don't end up repeating the name
					 * lookup later and latching onto a different relation
					 * partway through.  To avoid lock upgrade hazards, it's
					 * important that we take the strongest lock that will
					 * eventually be needed here, so the lockmode calculation
					 * needs to match what DefineIndex() does.
					 *
					 * 在一开始就只查找一次关系 OID，这样就不会在稍后重复名称查找，
					 * 并在中途锁定到另一个关系。为避免锁升级风险，这里获取最终会
					 * 需要的最强锁很重要，因此锁模式计算需要与 DefineIndex() 的
					 * 做法一致。
					 */
					lockmode = stmt->concurrent ? ShareUpdateExclusiveLock
						: ShareLock;
					relid =
						RangeVarGetRelidExtended(stmt->relation, lockmode,
												 0,
												 RangeVarCallbackOwnsRelation,
												 NULL);

					/*
					 * CREATE INDEX on partitioned tables (but not regular
					 * inherited tables) recurses to partitions, so we must
					 * acquire locks early to avoid deadlocks.
					 *
					 * We also take the opportunity to verify that all
					 * partitions are something we can put an index on, to
					 * avoid building some indexes only to fail later.  While
					 * at it, also count the partitions, so that DefineIndex
					 * needn't do a duplicative find_all_inheritors search.
					 *
					 * 分区表（但不是普通继承表）上的 CREATE INDEX 会递归到分区，
					 * 因此我们必须尽早获取锁以避免死锁。
					 *
					 * 同时，我们也借此验证所有分区都是可以在其上建立索引的对象，
					 * 以免先构建了一些索引后才失败。顺便还会统计分区数量，这样
					 * DefineIndex 就不必重复执行 find_all_inheritors 搜索。
					 */
					if (stmt->relation->inh &&
						get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
					{
						ListCell   *lc;
						List	   *inheritors = NIL;

						inheritors = find_all_inheritors(relid, lockmode, NULL);
						foreach(lc, inheritors)
						{
							Oid			partrelid = lfirst_oid(lc);
							char		relkind = get_rel_relkind(partrelid);

							if (relkind != RELKIND_RELATION &&
								relkind != RELKIND_MATVIEW &&
								relkind != RELKIND_PARTITIONED_TABLE &&
								relkind != RELKIND_FOREIGN_TABLE)
								elog(ERROR, "unexpected relkind \"%c\" on partition \"%s\"",
									 relkind, stmt->relation->relname);

							if (relkind == RELKIND_FOREIGN_TABLE &&
								(stmt->unique || stmt->primary))
								ereport(ERROR,
										(errcode(ERRCODE_WRONG_OBJECT_TYPE),
										 errmsg("cannot create unique index on partitioned table \"%s\"",
												stmt->relation->relname),
										 errdetail("Table \"%s\" contains partitions that are foreign tables.",
												   stmt->relation->relname)));
						}
						/*
						 * count direct and indirect children, but not rel
						 *
						 * 统计直接和间接子项，但不包括 rel 自身。
						 */
						nparts = list_length(inheritors) - 1;
						list_free(inheritors);
					}

					/*
					 * If the IndexStmt is already transformed, it must have
					 * come from generateClonedIndexStmt, which in current
					 * usage means it came from expandTableLikeClause rather
					 * than from original parse analysis.  And that means we
					 * must treat it like ALTER TABLE ADD INDEX, not CREATE.
					 * (This is a bit grotty, but currently it doesn't seem
					 * worth adding a separate bool field for the purpose.)
					 *
					 * 如果 IndexStmt 已经转换过，它一定来自 generateClonedIndexStmt，
					 * 在当前用法中这意味着它来自 expandTableLikeClause，而不是来自
					 * 原始解析分析。这也意味着我们必须把它当作 ALTER TABLE ADD INDEX
					 * 处理，而不是 CREATE。（这有点粗糙，但目前似乎不值得为此添加
					 * 一个单独的 bool 字段。）
					 */
					is_alter_table = stmt->transformed;

					/*
					 * Run parse analysis ...
					 *
					 * 运行解析分析……
					 */
					stmt = transformIndexStmt(relid, stmt, queryString);

					/*
					 * ... and do it
					 *
					 * ……然后执行。
					 */
					EventTriggerAlterTableStart(parsetree);
					address =
						DefineIndex(relid,	/* OID of heap relation
											 *
											 * 堆关系的 OID
											 */
									stmt,
									InvalidOid, /* no predefined OID
												 *
												 * 没有预定义 OID
												 */
									InvalidOid, /* no parent index
												 *
												 * 没有父索引
												 */
									InvalidOid, /* no parent constraint
												 *
												 * 没有父约束
												 */
									nparts, /* # of partitions, or -1
											 *
											 * 分区数量，或 -1
											 */
									is_alter_table,
									true,	/* check_rights
											 *
											 * 检查权限
											 */
									true,	/* check_not_in_use
											 *
											 * 检查未在使用中
											 */
									false,	/* skip_build
											 *
											 * 跳过构建
											 */
									false); /* quiet
											 *
											 * 静默
											 */

					/*
					 * Add the CREATE INDEX node itself to stash right away;
					 * if there were any commands stashed in the ALTER TABLE
					 * code, we need them to appear after this one.
					 *
					 * 立即把 CREATE INDEX 节点本身加入暂存；如果 ALTER TABLE 代码中
					 * 已经暂存了任何命令，我们需要它们出现在这个命令之后。
					 */
					EventTriggerCollectSimpleCommand(address, secondaryObject,
													 parsetree);
					commandCollected = true;
					EventTriggerAlterTableEnd();
				}
				break;

			case T_ReindexStmt:
				ExecReindex(pstate, (ReindexStmt *) parsetree, isTopLevel);

				/*
				 * EventTriggerCollectSimpleCommand is called directly
				 *
				 * EventTriggerCollectSimpleCommand 会被直接调用。
				 */
				commandCollected = true;
				break;

			case T_CreateExtensionStmt:
				address = CreateExtension(pstate, (CreateExtensionStmt *) parsetree);
				break;

			case T_AlterExtensionStmt:
				address = ExecAlterExtensionStmt(pstate, (AlterExtensionStmt *) parsetree);
				break;

			case T_AlterExtensionContentsStmt:
				address = ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree,
														 &secondaryObject);
				break;

			case T_CreateFdwStmt:
				address = CreateForeignDataWrapper(pstate, (CreateFdwStmt *) parsetree);
				break;

			case T_AlterFdwStmt:
				address = AlterForeignDataWrapper(pstate, (AlterFdwStmt *) parsetree);
				break;

			case T_CreateForeignServerStmt:
				address = CreateForeignServer((CreateForeignServerStmt *) parsetree);
				break;

			case T_AlterForeignServerStmt:
				address = AlterForeignServer((AlterForeignServerStmt *) parsetree);
				break;

			case T_CreateUserMappingStmt:
				address = CreateUserMapping((CreateUserMappingStmt *) parsetree);
				break;

			case T_AlterUserMappingStmt:
				address = AlterUserMapping((AlterUserMappingStmt *) parsetree);
				break;

			case T_DropUserMappingStmt:
				RemoveUserMapping((DropUserMappingStmt *) parsetree);
				/*
				 * no commands stashed for DROP
				 *
				 * DROP 不暂存命令。
				 */
				commandCollected = true;
				break;

			case T_ImportForeignSchemaStmt:
				ImportForeignSchema((ImportForeignSchemaStmt *) parsetree);
				/*
				 * commands are stashed inside ImportForeignSchema
				 *
				 * 命令在 ImportForeignSchema 内部暂存。
				 */
				commandCollected = true;
				break;

			case T_CompositeTypeStmt:	/* CREATE TYPE (composite)
										 *
										 * CREATE TYPE（复合类型）
										 */
				{
					CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

					address = DefineCompositeType(stmt->typevar,
												  stmt->coldeflist);
				}
				break;

			case T_CreateEnumStmt:	/* CREATE TYPE AS ENUM
									 *
									 * 创建枚举类型
									 */
				address = DefineEnum((CreateEnumStmt *) parsetree);
				break;

			case T_CreateRangeStmt: /* CREATE TYPE AS RANGE
									 *
									 * 创建范围类型
									 */
				address = DefineRange(pstate, (CreateRangeStmt *) parsetree);
				break;

			case T_AlterEnumStmt:	/* ALTER TYPE (enum)
									 *
									 * ALTER TYPE（枚举类型）
									 */
				address = AlterEnum((AlterEnumStmt *) parsetree);
				break;

			case T_ViewStmt:	/* CREATE VIEW
								 *
								 * 创建视图
								 */
				EventTriggerAlterTableStart(parsetree);
				address = DefineView((ViewStmt *) parsetree, queryString,
									 pstmt->stmt_location, pstmt->stmt_len);
				EventTriggerCollectSimpleCommand(address, secondaryObject,
												 parsetree);
				/*
				 * stashed internally
				 *
				 * 在内部暂存。
				 */
				commandCollected = true;
				EventTriggerAlterTableEnd();
				break;

			case T_CreateFunctionStmt:	/* CREATE FUNCTION
										 *
										 * 创建函数
										 */
				address = CreateFunction(pstate, (CreateFunctionStmt *) parsetree);
				break;

			case T_AlterFunctionStmt:	/* ALTER FUNCTION
										 *
										 * 修改函数
										 */
				address = AlterFunction(pstate, (AlterFunctionStmt *) parsetree);
				break;

			case T_RuleStmt:	/* CREATE RULE
								 *
								 * 创建规则
								 */
				address = DefineRule((RuleStmt *) parsetree, queryString);
				break;

			case T_CreateSeqStmt:
				address = DefineSequence(pstate, (CreateSeqStmt *) parsetree);
				break;

			case T_AlterSeqStmt:
				address = AlterSequence(pstate, (AlterSeqStmt *) parsetree);
				break;

			case T_CreateTableAsStmt:
				address = ExecCreateTableAs(pstate, (CreateTableAsStmt *) parsetree,
											params, queryEnv, qc);
				break;

			case T_RefreshMatViewStmt:

				/*
				 * REFRESH CONCURRENTLY executes some DDL commands internally.
				 * Inhibit DDL command collection here to avoid those commands
				 * from showing up in the deparsed command queue.  The refresh
				 * command itself is queued, which is enough.
				 *
				 * REFRESH CONCURRENTLY 会在内部执行一些 DDL 命令。这里抑制 DDL
				 * 命令收集，以避免这些命令出现在反解析命令队列中。刷新命令本身
				 * 会入队，这已经足够。
				 */
				EventTriggerInhibitCommandCollection();
				PG_TRY(2);
				{
					address = ExecRefreshMatView((RefreshMatViewStmt *) parsetree,
												 queryString, qc);
				}
				PG_FINALLY(2);
				{
					EventTriggerUndoInhibitCommandCollection();
				}
				PG_END_TRY(2);
				break;

			case T_CreateTrigStmt:
				address = CreateTrigger((CreateTrigStmt *) parsetree,
										queryString, InvalidOid, InvalidOid,
										InvalidOid, InvalidOid, InvalidOid,
										InvalidOid, NULL, false, false);
				break;

			case T_CreatePLangStmt:
				address = CreateProceduralLanguage((CreatePLangStmt *) parsetree);
				break;

			case T_CreateDomainStmt:
				address = DefineDomain(pstate, (CreateDomainStmt *) parsetree);
				break;

			case T_CreateConversionStmt:
				address = CreateConversionCommand((CreateConversionStmt *) parsetree);
				break;

			case T_CreateCastStmt:
				address = CreateCast((CreateCastStmt *) parsetree);
				break;

			case T_CreateOpClassStmt:
				DefineOpClass((CreateOpClassStmt *) parsetree);
				/*
				 * command is stashed in DefineOpClass
				 *
				 * 命令在 DefineOpClass 中暂存。
				 */
				commandCollected = true;
				break;

			case T_CreateOpFamilyStmt:
				address = DefineOpFamily((CreateOpFamilyStmt *) parsetree);

				/*
				 * DefineOpFamily calls EventTriggerCollectSimpleCommand
				 * directly.
				 *
				 * DefineOpFamily 会直接调用 EventTriggerCollectSimpleCommand。
				 */
				commandCollected = true;
				break;

			case T_CreateTransformStmt:
				address = CreateTransform((CreateTransformStmt *) parsetree);
				break;

			case T_AlterOpFamilyStmt:
				AlterOpFamily((AlterOpFamilyStmt *) parsetree);
				/*
				 * commands are stashed in AlterOpFamily
				 *
				 * 命令在 AlterOpFamily 中暂存。
				 */
				commandCollected = true;
				break;

			case T_AlterTSDictionaryStmt:
				address = AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
				break;

			case T_AlterTSConfigurationStmt:
				AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);

				/*
				 * Commands are stashed in MakeConfigurationMapping and
				 * DropConfigurationMapping, which are called from
				 * AlterTSConfiguration
				 *
				 * 命令在 MakeConfigurationMapping 和 DropConfigurationMapping
				 * 中暂存，它们由 AlterTSConfiguration 调用。
				 */
				commandCollected = true;
				break;

			case T_AlterTableMoveAllStmt:
				AlterTableMoveAll((AlterTableMoveAllStmt *) parsetree);
				/*
				 * commands are stashed in AlterTableMoveAll
				 *
				 * 命令在 AlterTableMoveAll 中暂存。
				 */
				commandCollected = true;
				break;

			case T_DropStmt:
				ExecDropStmt((DropStmt *) parsetree, isTopLevel);
				/*
				 * no commands stashed for DROP
				 *
				 * DROP 不暂存命令。
				 */
				commandCollected = true;
				break;

			case T_RenameStmt:
				address = ExecRenameStmt((RenameStmt *) parsetree);
				break;

			case T_AlterObjectDependsStmt:
				address =
					ExecAlterObjectDependsStmt((AlterObjectDependsStmt *) parsetree,
											   &secondaryObject);
				break;

			case T_AlterObjectSchemaStmt:
				address =
					ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree,
											  &secondaryObject);
				break;

			case T_AlterOwnerStmt:
				address = ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);
				break;

			case T_AlterOperatorStmt:
				address = AlterOperator((AlterOperatorStmt *) parsetree);
				break;

			case T_AlterTypeStmt:
				address = AlterType((AlterTypeStmt *) parsetree);
				break;

			case T_CommentStmt:
				address = CommentObject((CommentStmt *) parsetree);
				break;

			case T_GrantStmt:
				ExecuteGrantStmt((GrantStmt *) parsetree);
				/*
				 * commands are stashed in ExecGrantStmt_oids
				 *
				 * 命令在 ExecGrantStmt_oids 中暂存。
				 */
				commandCollected = true;
				break;

			case T_DropOwnedStmt:
				DropOwnedObjects((DropOwnedStmt *) parsetree);
				/*
				 * no commands stashed for DROP
				 *
				 * DROP 不暂存命令。
				 */
				commandCollected = true;
				break;

			case T_AlterDefaultPrivilegesStmt:
				ExecAlterDefaultPrivilegesStmt(pstate, (AlterDefaultPrivilegesStmt *) parsetree);
				EventTriggerCollectAlterDefPrivs((AlterDefaultPrivilegesStmt *) parsetree);
				commandCollected = true;
				break;

			case T_CreatePolicyStmt:	/* CREATE POLICY
									 *
									 * 创建策略
									 */
				address = CreatePolicy((CreatePolicyStmt *) parsetree);
				break;

			case T_AlterPolicyStmt: /* ALTER POLICY
									 *
									 * 修改策略
									 */
				address = AlterPolicy((AlterPolicyStmt *) parsetree);
				break;

			case T_SecLabelStmt:
				address = ExecSecLabelStmt((SecLabelStmt *) parsetree);
				break;

			case T_CreateAmStmt:
				address = CreateAccessMethod((CreateAmStmt *) parsetree);
				break;

			case T_CreatePublicationStmt:
				address = CreatePublication(pstate, (CreatePublicationStmt *) parsetree);
				break;

			case T_AlterPublicationStmt:
				AlterPublication(pstate, (AlterPublicationStmt *) parsetree);

				/*
				 * AlterPublication calls EventTriggerCollectSimpleCommand
				 * directly
				 *
				 * AlterPublication 会直接调用 EventTriggerCollectSimpleCommand。
				 */
				commandCollected = true;
				break;

			case T_CreateSubscriptionStmt:
				address = CreateSubscription(pstate,
											 (CreateSubscriptionStmt *) parsetree,
											 isTopLevel);
				break;

			case T_AlterSubscriptionStmt:
				address = AlterSubscription(pstate,
											(AlterSubscriptionStmt *) parsetree,
											isTopLevel);
				break;

			case T_DropSubscriptionStmt:
				DropSubscription((DropSubscriptionStmt *) parsetree, isTopLevel);
				/*
				 * no commands stashed for DROP
				 *
				 * DROP 不暂存命令。
				 */
				commandCollected = true;
				break;

			case T_CreateStatsStmt:
				{
					Oid			relid;
					CreateStatsStmt *stmt = (CreateStatsStmt *) parsetree;
					RangeVar   *rel = (RangeVar *) linitial(stmt->relations);

					if (!IsA(rel, RangeVar))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("CREATE STATISTICS only supports relation names in the FROM clause")));

					/*
					 * CREATE STATISTICS will influence future execution plans
					 * but does not interfere with currently executing plans.
					 * So it should be enough to take ShareUpdateExclusiveLock
					 * on relation, conflicting with ANALYZE and other DDL
					 * that sets statistical information, but not with normal
					 * queries.
					 *
					 * CREATE STATISTICS 会影响未来的执行计划，但不会干扰当前正在执行的
					 * 计划。因此，在关系上获取 ShareUpdateExclusiveLock 应该足够，
					 * 它会与 ANALYZE 以及其他设置统计信息的 DDL 冲突，但不会与普通
					 * 查询冲突。
					 *
					 * XXX RangeVarCallbackOwnsRelation not needed here, to
					 * keep the same behavior as before.
					 *
					 * XXX 这里不需要 RangeVarCallbackOwnsRelation，以保持与以前相同
					 * 的行为。
					 */
					relid = RangeVarGetRelid(rel, ShareUpdateExclusiveLock, false);

					/*
					 * Run parse analysis ...
					 *
					 * 运行解析分析……
					 */
					stmt = transformStatsStmt(relid, stmt, queryString);

					address = CreateStatistics(stmt, true);
				}
				break;

			case T_AlterStatsStmt:
				address = AlterStatistics((AlterStatsStmt *) parsetree);
				break;

			case T_AlterCollationStmt:
				address = AlterCollation((AlterCollationStmt *) parsetree);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(parsetree));
				break;
		}

		/*
		 * Remember the object so that ddl_command_end event triggers have
		 * access to it.
		 *
		 * 记住该对象，使 ddl_command_end 事件触发器可以访问它。
		 */
		if (!commandCollected)
			EventTriggerCollectSimpleCommand(address, secondaryObject,
											 parsetree);

		if (isCompleteQuery)
		{
			EventTriggerSQLDrop(parsetree);
			EventTriggerDDLCommandEnd(parsetree);
		}
	}
	PG_FINALLY();
	{
		if (needCleanup)
			EventTriggerEndCompleteQuery();
	}
	PG_END_TRY();
}

/*
 * ProcessUtilityForAlterTable
 *		Recursive entry from ALTER TABLE
 *
 * ProcessUtilityForAlterTable
 *		来自 ALTER TABLE 的递归入口。
 *
 * ALTER TABLE sometimes generates subcommands such as CREATE INDEX.
 * It calls this, not the main entry point ProcessUtility, to execute
 * such subcommands.
 *
 * ALTER TABLE 有时会生成诸如 CREATE INDEX 这样的子命令。它会调用此函数，
 * 而不是主入口点 ProcessUtility，来执行这些子命令。
 *
 * stmt: the utility command to execute
 * context: opaque passthrough struct with the info we need
 *
 * stmt：要执行的工具命令。
 * context：不透明的透传结构，包含我们需要的信息。
 *
 * It's caller's responsibility to do CommandCounterIncrement after
 * calling this, if needed.
 *
 * 如果需要，调用此函数之后执行 CommandCounterIncrement 是调用者的责任。
 *
 * The flow closes the current ALTER TABLE event-trigger batch, wraps the
 * subcommand in a PlannedStmt, invokes ProcessUtility recursively, then
 * reopens the ALTER TABLE trigger context.
 *
 * 核心流程是关闭当前 ALTER TABLE 事件触发器批次，将子命令包装为
 * PlannedStmt，递归调用 ProcessUtility，然后重新打开 ALTER TABLE 触发器
 * 上下文。
 */
void
ProcessUtilityForAlterTable(Node *stmt, AlterTableUtilityContext *context)
{
	PlannedStmt *wrapper;

	/*
	 * For event triggers, we must "close" the current complex-command set,
	 * and start a new one afterwards; this is needed to ensure the ordering
	 * of command events is consistent with the way they were executed.
	 *
	 * 对于事件触发器，我们必须“关闭”当前复杂命令集合，然后再开始一个
	 * 新的集合；这是为了确保命令事件的顺序与它们的执行方式一致。
	 */
	EventTriggerAlterTableEnd();

	/*
	 * Create a suitable wrapper
	 *
	 * 创建一个合适的包装器。
	 */
	wrapper = makeNode(PlannedStmt);
	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = stmt;
	wrapper->stmt_location = context->pstmt->stmt_location;
	wrapper->stmt_len = context->pstmt->stmt_len;

	ProcessUtility(wrapper,
				   context->queryString,
				   false,
				   PROCESS_UTILITY_SUBCOMMAND,
				   context->params,
				   context->queryEnv,
				   None_Receiver,
				   NULL);

	EventTriggerAlterTableStart(context->pstmt->utilityStmt);
	EventTriggerAlterTableRelid(context->relid);
}

/*
 * Dispatch function for DropStmt
 *
 * DropStmt 的分发函数。
 *
 * The flow handles concurrent index drops as a top-level-only operation, routes
 * relation-like drops to RemoveRelations, and sends all other drops to
 * RemoveObjects.
 *
 * 核心流程是将并发索引删除作为仅限顶层的操作处理，把类似关系的删除
 * 路由到 RemoveRelations，并将所有其他删除发送给 RemoveObjects。
 */
static void
ExecDropStmt(DropStmt *stmt, bool isTopLevel)
{
	switch (stmt->removeType)
	{
		case OBJECT_INDEX:
			if (stmt->concurrent)
				PreventInTransactionBlock(isTopLevel,
										  "DROP INDEX CONCURRENTLY");
			/*
			 * fall through
			 *
			 * 继续向下执行。
			 */

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_FOREIGN_TABLE:
			RemoveRelations(stmt);
			break;
		default:
			RemoveObjects(stmt);
			break;
	}
}


/*
 * UtilityReturnsTuples
 *		Return "true" if this utility statement will send output to the
 *		destination.
 *
 * UtilityReturnsTuples
 *		如果此工具语句会向目标发送输出，则返回 "true"。
 *
 * Generally, there should be a case here for each case in ProcessUtility
 * where "dest" is passed on.
 *
 * 通常，ProcessUtility 中每个传递 "dest" 的分支都应在这里有对应分支。
 *
 * The flow switches on the utility node type and checks statement- or
 * portal-specific metadata to decide whether rows will be emitted.
 *
 * 核心流程是根据工具节点类型进行 switch，并检查语句或 portal 专有的
 * 元数据，以决定是否会输出行。
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CallStmt:
			{
				CallStmt   *stmt = (CallStmt *) parsetree;

				return (stmt->funcexpr->funcresulttype == RECORDOID);
			}
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;	/* not our business to raise error
									 *
									 * 抛出错误不是我们的职责。
									 */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;	/* not our business to raise error
									 *
									 * 抛出错误不是我们的职责。
									 */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		Fetch the actual output tuple descriptor for a utility statement
 *		for which UtilityReturnsTuples() previously returned "true".
 *
 * UtilityTupleDescriptor
 *		为先前 UtilityReturnsTuples() 返回 "true" 的工具语句获取实际输出
 *		元组描述符。
 *
 * The returned descriptor is created in (or copied into) the current memory
 * context.
 *
 * 返回的描述符会在当前内存上下文中创建（或复制到当前内存上下文中）。
 *
 * The flow mirrors UtilityReturnsTuples and fetches or constructs the tuple
 * descriptor from CALL, cursor, prepared statement, EXPLAIN, or SHOW metadata.
 *
 * 核心流程与 UtilityReturnsTuples 对应，并从 CALL、游标、预备语句、EXPLAIN
 * 或 SHOW 元数据中获取或构造元组描述符。
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CallStmt:
			return CallStmtResultDesc((CallStmt *) parsetree);

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* not our business to raise error
									 *
									 * 抛出错误不是我们的职责。
									 */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* not our business to raise error
									 *
									 * 抛出错误不是我们的职责。
									 */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * QueryReturnsTuples
 *		Return "true" if this Query will send output to the destination.
 *
 * QueryReturnsTuples
 *		如果此 Query 会向目标发送输出，则返回 "true"。
 *
 * The flow checks the Query command type, treats SELECT as tuple-producing,
 * and recognizes RETURNING lists on data-modifying commands.
 *
 * 核心流程是检查 Query 命令类型，将 SELECT 视为会产生元组，并识别
 * 数据修改命令上的 RETURNING 列表。
 */
#ifdef NOT_USED
bool
QueryReturnsTuples(Query *parsetree)
{
	switch (parsetree->commandType)
	{
		case CMD_SELECT:
			/*
			 * returns tuples
			 *
			 * 返回元组。
			 */
			return true;
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
		case CMD_MERGE:
			/*
			 * the forms with RETURNING return tuples
			 *
			 * 带 RETURNING 的形式会返回元组。
			 */
			if (parsetree->returningList)
				return true;
			break;
		case CMD_UTILITY:
			return UtilityReturnsTuples(parsetree->utilityStmt);
		case CMD_UNKNOWN:
		case CMD_NOTHING:
			/*
			 * probably shouldn't get here
			 *
			 * 可能不应到达这里。
			 */
			break;
	}
	return false;				/* default
								 *
								 * 默认值
								 */
}
#endif


/*
 * UtilityContainsQuery
 *		Return the contained Query, or NULL if there is none
 *
 * UtilityContainsQuery
 *		返回包含的 Query；如果没有则返回 NULL。
 *
 * Certain utility statements, such as EXPLAIN, contain a plannable Query.
 * This function encapsulates knowledge of exactly which ones do.
 * We assume it is invoked only on already-parse-analyzed statements
 * (else the contained parsetree isn't a Query yet).
 *
 * 某些工具语句（例如 EXPLAIN）包含可规划的 Query。此函数封装了哪些
 * 语句确实如此的知识。我们假定它只会在已经过解析分析的语句上调用
 * （否则包含的 parsetree 还不是 Query）。
 *
 * In some cases (currently, only EXPLAIN of CREATE TABLE AS/SELECT INTO and
 * CREATE MATERIALIZED VIEW), potentially Query-containing utility statements
 * can be nested.  This function will drill down to a non-utility Query, or
 * return NULL if none.
 *
 * 在某些情况下（目前只有 CREATE TABLE AS/SELECT INTO 和 CREATE
 * MATERIALIZED VIEW 的 EXPLAIN），可能包含 Query 的工具语句可以嵌套。
 * 此函数会深入到非工具 Query；如果没有则返回 NULL。
 *
 * The flow switches over utility wrappers that can contain a Query and recurses
 * through nested utility queries until it finds a plannable Query or none.
 *
 * 核心流程是对可能包含 Query 的工具包装进行 switch，并递归穿过嵌套
 * 的工具查询，直到找到可规划 Query 或确认不存在。
 */
Query *
UtilityContainsQuery(Node *parsetree)
{
	Query	   *qry;

	switch (nodeTag(parsetree))
	{
		case T_DeclareCursorStmt:
			qry = castNode(Query, ((DeclareCursorStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_ExplainStmt:
			qry = castNode(Query, ((ExplainStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_CreateTableAsStmt:
			qry = castNode(Query, ((CreateTableAsStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		default:
			return NULL;
	}
}


/*
 * AlterObjectTypeCommandTag
 *		helper function for CreateCommandTag
 *
 * AlterObjectTypeCommandTag
 *		CreateCommandTag 的辅助函数。
 *
 * This covers most cases where ALTER is used with an ObjectType enum.
 *
 * 这覆盖了 ALTER 与 ObjectType 枚举一起使用的大多数情况。
 *
 * The flow maps known ObjectType values to ALTER command tags and falls back to
 * CMDTAG_UNKNOWN for unsupported object types.
 *
 * 核心流程是把已知 ObjectType 值映射到 ALTER 命令标签，并对不支持的
 * 对象类型回退到 CMDTAG_UNKNOWN。
 */
static CommandTag
AlterObjectTypeCommandTag(ObjectType objtype)
{
	CommandTag	tag;

	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			tag = CMDTAG_ALTER_AGGREGATE;
			break;
		case OBJECT_ATTRIBUTE:
			tag = CMDTAG_ALTER_TYPE;
			break;
		case OBJECT_CAST:
			tag = CMDTAG_ALTER_CAST;
			break;
		case OBJECT_COLLATION:
			tag = CMDTAG_ALTER_COLLATION;
			break;
		case OBJECT_COLUMN:
			tag = CMDTAG_ALTER_TABLE;
			break;
		case OBJECT_CONVERSION:
			tag = CMDTAG_ALTER_CONVERSION;
			break;
		case OBJECT_DATABASE:
			tag = CMDTAG_ALTER_DATABASE;
			break;
		case OBJECT_DOMAIN:
		case OBJECT_DOMCONSTRAINT:
			tag = CMDTAG_ALTER_DOMAIN;
			break;
		case OBJECT_EXTENSION:
			tag = CMDTAG_ALTER_EXTENSION;
			break;
		case OBJECT_FDW:
			tag = CMDTAG_ALTER_FOREIGN_DATA_WRAPPER;
			break;
		case OBJECT_FOREIGN_SERVER:
			tag = CMDTAG_ALTER_SERVER;
			break;
		case OBJECT_FOREIGN_TABLE:
			tag = CMDTAG_ALTER_FOREIGN_TABLE;
			break;
		case OBJECT_FUNCTION:
			tag = CMDTAG_ALTER_FUNCTION;
			break;
		case OBJECT_INDEX:
			tag = CMDTAG_ALTER_INDEX;
			break;
		case OBJECT_LANGUAGE:
			tag = CMDTAG_ALTER_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			tag = CMDTAG_ALTER_LARGE_OBJECT;
			break;
		case OBJECT_OPCLASS:
			tag = CMDTAG_ALTER_OPERATOR_CLASS;
			break;
		case OBJECT_OPERATOR:
			tag = CMDTAG_ALTER_OPERATOR;
			break;
		case OBJECT_OPFAMILY:
			tag = CMDTAG_ALTER_OPERATOR_FAMILY;
			break;
		case OBJECT_POLICY:
			tag = CMDTAG_ALTER_POLICY;
			break;
		case OBJECT_PROCEDURE:
			tag = CMDTAG_ALTER_PROCEDURE;
			break;
		case OBJECT_ROLE:
			tag = CMDTAG_ALTER_ROLE;
			break;
		case OBJECT_ROUTINE:
			tag = CMDTAG_ALTER_ROUTINE;
			break;
		case OBJECT_RULE:
			tag = CMDTAG_ALTER_RULE;
			break;
		case OBJECT_SCHEMA:
			tag = CMDTAG_ALTER_SCHEMA;
			break;
		case OBJECT_SEQUENCE:
			tag = CMDTAG_ALTER_SEQUENCE;
			break;
		case OBJECT_TABLE:
		case OBJECT_TABCONSTRAINT:
			tag = CMDTAG_ALTER_TABLE;
			break;
		case OBJECT_TABLESPACE:
			tag = CMDTAG_ALTER_TABLESPACE;
			break;
		case OBJECT_TRIGGER:
			tag = CMDTAG_ALTER_TRIGGER;
			break;
		case OBJECT_EVENT_TRIGGER:
			tag = CMDTAG_ALTER_EVENT_TRIGGER;
			break;
		case OBJECT_TSCONFIGURATION:
			tag = CMDTAG_ALTER_TEXT_SEARCH_CONFIGURATION;
			break;
		case OBJECT_TSDICTIONARY:
			tag = CMDTAG_ALTER_TEXT_SEARCH_DICTIONARY;
			break;
		case OBJECT_TSPARSER:
			tag = CMDTAG_ALTER_TEXT_SEARCH_PARSER;
			break;
		case OBJECT_TSTEMPLATE:
			tag = CMDTAG_ALTER_TEXT_SEARCH_TEMPLATE;
			break;
		case OBJECT_TYPE:
			tag = CMDTAG_ALTER_TYPE;
			break;
		case OBJECT_VIEW:
			tag = CMDTAG_ALTER_VIEW;
			break;
		case OBJECT_MATVIEW:
			tag = CMDTAG_ALTER_MATERIALIZED_VIEW;
			break;
		case OBJECT_PUBLICATION:
			tag = CMDTAG_ALTER_PUBLICATION;
			break;
		case OBJECT_SUBSCRIPTION:
			tag = CMDTAG_ALTER_SUBSCRIPTION;
			break;
		case OBJECT_STATISTIC_EXT:
			tag = CMDTAG_ALTER_STATISTICS;
			break;
		default:
			tag = CMDTAG_UNKNOWN;
			break;
	}

	return tag;
}

/*
 * CreateCommandTag
 *		utility to get a CommandTag for the command operation,
 *		given either a raw (un-analyzed) parsetree, an analyzed Query,
 *		or a PlannedStmt.
 *
 * CreateCommandTag
 *		用于为命令操作获取 CommandTag 的工具函数；输入可以是原始（未分析）
 *		parsetree、已分析的 Query 或 PlannedStmt。
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * 它必须处理所有命令类型，但由于其中绝大多数是工具命令，把它放在这里
 * 似乎是合理的。
 *
 * The flow unwraps RawStmt, PlannedStmt, and Query nodes as needed, then maps
 * command-specific node tags and sub-kinds to their externally visible
 * CommandTag.
 *
 * 核心流程是按需解包 RawStmt、PlannedStmt 和 Query 节点，然后将特定命令的
 * 节点标签和子类型映射到其外部可见的 CommandTag。
 */
CommandTag
CreateCommandTag(Node *parsetree)
{
	CommandTag	tag;

	switch (nodeTag(parsetree))
	{
			/*
			 * recurse if we're given a RawStmt
			 *
			 * 如果给定 RawStmt，则递归处理。
			 */
		case T_RawStmt:
			tag = CreateCommandTag(((RawStmt *) parsetree)->stmt);
			break;

			/*
			 * raw plannable queries
			 *
			 * 原始可规划查询。
			 */
		case T_InsertStmt:
			tag = CMDTAG_INSERT;
			break;

		case T_DeleteStmt:
			tag = CMDTAG_DELETE;
			break;

		case T_UpdateStmt:
			tag = CMDTAG_UPDATE;
			break;

		case T_MergeStmt:
			tag = CMDTAG_MERGE;
			break;

		case T_SelectStmt:
			tag = CMDTAG_SELECT;
			break;

		case T_PLAssignStmt:
			tag = CMDTAG_SELECT;
			break;

			/*
			 * utility statements --- same whether raw or cooked
			 *
			 * 工具语句——无论原始还是处理后都相同。
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = CMDTAG_BEGIN;
						break;

					case TRANS_STMT_START:
						tag = CMDTAG_START_TRANSACTION;
						break;

					case TRANS_STMT_COMMIT:
						tag = CMDTAG_COMMIT;
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = CMDTAG_ROLLBACK;
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = CMDTAG_SAVEPOINT;
						break;

					case TRANS_STMT_RELEASE:
						tag = CMDTAG_RELEASE;
						break;

					case TRANS_STMT_PREPARE:
						tag = CMDTAG_PREPARE_TRANSACTION;
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = CMDTAG_COMMIT_PREPARED;
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = CMDTAG_ROLLBACK_PREPARED;
						break;

					default:
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = CMDTAG_DECLARE_CURSOR;
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = CMDTAG_CLOSE_CURSOR_ALL;
				else
					tag = CMDTAG_CLOSE_CURSOR;
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? CMDTAG_MOVE : CMDTAG_FETCH;
			}
			break;

		case T_CreateDomainStmt:
			tag = CMDTAG_CREATE_DOMAIN;
			break;

		case T_CreateSchemaStmt:
			tag = CMDTAG_CREATE_SCHEMA;
			break;

		case T_CreateStmt:
			tag = CMDTAG_CREATE_TABLE;
			break;

		case T_CreateTableSpaceStmt:
			tag = CMDTAG_CREATE_TABLESPACE;
			break;

		case T_DropTableSpaceStmt:
			tag = CMDTAG_DROP_TABLESPACE;
			break;

		case T_AlterTableSpaceOptionsStmt:
			tag = CMDTAG_ALTER_TABLESPACE;
			break;

		case T_CreateExtensionStmt:
			tag = CMDTAG_CREATE_EXTENSION;
			break;

		case T_AlterExtensionStmt:
			tag = CMDTAG_ALTER_EXTENSION;
			break;

		case T_AlterExtensionContentsStmt:
			tag = CMDTAG_ALTER_EXTENSION;
			break;

		case T_CreateFdwStmt:
			tag = CMDTAG_CREATE_FOREIGN_DATA_WRAPPER;
			break;

		case T_AlterFdwStmt:
			tag = CMDTAG_ALTER_FOREIGN_DATA_WRAPPER;
			break;

		case T_CreateForeignServerStmt:
			tag = CMDTAG_CREATE_SERVER;
			break;

		case T_AlterForeignServerStmt:
			tag = CMDTAG_ALTER_SERVER;
			break;

		case T_CreateUserMappingStmt:
			tag = CMDTAG_CREATE_USER_MAPPING;
			break;

		case T_AlterUserMappingStmt:
			tag = CMDTAG_ALTER_USER_MAPPING;
			break;

		case T_DropUserMappingStmt:
			tag = CMDTAG_DROP_USER_MAPPING;
			break;

		case T_CreateForeignTableStmt:
			tag = CMDTAG_CREATE_FOREIGN_TABLE;
			break;

		case T_ImportForeignSchemaStmt:
			tag = CMDTAG_IMPORT_FOREIGN_SCHEMA;
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = CMDTAG_DROP_TABLE;
					break;
				case OBJECT_SEQUENCE:
					tag = CMDTAG_DROP_SEQUENCE;
					break;
				case OBJECT_VIEW:
					tag = CMDTAG_DROP_VIEW;
					break;
				case OBJECT_MATVIEW:
					tag = CMDTAG_DROP_MATERIALIZED_VIEW;
					break;
				case OBJECT_INDEX:
					tag = CMDTAG_DROP_INDEX;
					break;
				case OBJECT_TYPE:
					tag = CMDTAG_DROP_TYPE;
					break;
				case OBJECT_DOMAIN:
					tag = CMDTAG_DROP_DOMAIN;
					break;
				case OBJECT_COLLATION:
					tag = CMDTAG_DROP_COLLATION;
					break;
				case OBJECT_CONVERSION:
					tag = CMDTAG_DROP_CONVERSION;
					break;
				case OBJECT_SCHEMA:
					tag = CMDTAG_DROP_SCHEMA;
					break;
				case OBJECT_TSPARSER:
					tag = CMDTAG_DROP_TEXT_SEARCH_PARSER;
					break;
				case OBJECT_TSDICTIONARY:
					tag = CMDTAG_DROP_TEXT_SEARCH_DICTIONARY;
					break;
				case OBJECT_TSTEMPLATE:
					tag = CMDTAG_DROP_TEXT_SEARCH_TEMPLATE;
					break;
				case OBJECT_TSCONFIGURATION:
					tag = CMDTAG_DROP_TEXT_SEARCH_CONFIGURATION;
					break;
				case OBJECT_FOREIGN_TABLE:
					tag = CMDTAG_DROP_FOREIGN_TABLE;
					break;
				case OBJECT_EXTENSION:
					tag = CMDTAG_DROP_EXTENSION;
					break;
				case OBJECT_FUNCTION:
					tag = CMDTAG_DROP_FUNCTION;
					break;
				case OBJECT_PROCEDURE:
					tag = CMDTAG_DROP_PROCEDURE;
					break;
				case OBJECT_ROUTINE:
					tag = CMDTAG_DROP_ROUTINE;
					break;
				case OBJECT_AGGREGATE:
					tag = CMDTAG_DROP_AGGREGATE;
					break;
				case OBJECT_OPERATOR:
					tag = CMDTAG_DROP_OPERATOR;
					break;
				case OBJECT_LANGUAGE:
					tag = CMDTAG_DROP_LANGUAGE;
					break;
				case OBJECT_CAST:
					tag = CMDTAG_DROP_CAST;
					break;
				case OBJECT_TRIGGER:
					tag = CMDTAG_DROP_TRIGGER;
					break;
				case OBJECT_EVENT_TRIGGER:
					tag = CMDTAG_DROP_EVENT_TRIGGER;
					break;
				case OBJECT_RULE:
					tag = CMDTAG_DROP_RULE;
					break;
				case OBJECT_FDW:
					tag = CMDTAG_DROP_FOREIGN_DATA_WRAPPER;
					break;
				case OBJECT_FOREIGN_SERVER:
					tag = CMDTAG_DROP_SERVER;
					break;
				case OBJECT_OPCLASS:
					tag = CMDTAG_DROP_OPERATOR_CLASS;
					break;
				case OBJECT_OPFAMILY:
					tag = CMDTAG_DROP_OPERATOR_FAMILY;
					break;
				case OBJECT_POLICY:
					tag = CMDTAG_DROP_POLICY;
					break;
				case OBJECT_TRANSFORM:
					tag = CMDTAG_DROP_TRANSFORM;
					break;
				case OBJECT_ACCESS_METHOD:
					tag = CMDTAG_DROP_ACCESS_METHOD;
					break;
				case OBJECT_PUBLICATION:
					tag = CMDTAG_DROP_PUBLICATION;
					break;
				case OBJECT_STATISTIC_EXT:
					tag = CMDTAG_DROP_STATISTICS;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_TruncateStmt:
			tag = CMDTAG_TRUNCATE_TABLE;
			break;

		case T_CommentStmt:
			tag = CMDTAG_COMMENT;
			break;

		case T_SecLabelStmt:
			tag = CMDTAG_SECURITY_LABEL;
			break;

		case T_CopyStmt:
			tag = CMDTAG_COPY;
			break;

		case T_RenameStmt:

			/*
			 * When the column is renamed, the command tag is created from its
			 * relation type
			 *
			 * 重命名列时，命令标签根据其关系类型创建。
			 */
			tag = AlterObjectTypeCommandTag(((RenameStmt *) parsetree)->renameType == OBJECT_COLUMN ?
											((RenameStmt *) parsetree)->relationType :
											((RenameStmt *) parsetree)->renameType);
			break;

		case T_AlterObjectDependsStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectDependsStmt *) parsetree)->objectType);
			break;

		case T_AlterObjectSchemaStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectSchemaStmt *) parsetree)->objectType);
			break;

		case T_AlterOwnerStmt:
			tag = AlterObjectTypeCommandTag(((AlterOwnerStmt *) parsetree)->objectType);
			break;

		case T_AlterTableMoveAllStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableMoveAllStmt *) parsetree)->objtype);
			break;

		case T_AlterTableStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableStmt *) parsetree)->objtype);
			break;

		case T_AlterDomainStmt:
			tag = CMDTAG_ALTER_DOMAIN;
			break;

		case T_AlterFunctionStmt:
			switch (((AlterFunctionStmt *) parsetree)->objtype)
			{
				case OBJECT_FUNCTION:
					tag = CMDTAG_ALTER_FUNCTION;
					break;
				case OBJECT_PROCEDURE:
					tag = CMDTAG_ALTER_PROCEDURE;
					break;
				case OBJECT_ROUTINE:
					tag = CMDTAG_ALTER_ROUTINE;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? CMDTAG_GRANT : CMDTAG_REVOKE;
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? CMDTAG_GRANT_ROLE : CMDTAG_REVOKE_ROLE;
			}
			break;

		case T_AlterDefaultPrivilegesStmt:
			tag = CMDTAG_ALTER_DEFAULT_PRIVILEGES;
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = CMDTAG_CREATE_AGGREGATE;
					break;
				case OBJECT_OPERATOR:
					tag = CMDTAG_CREATE_OPERATOR;
					break;
				case OBJECT_TYPE:
					tag = CMDTAG_CREATE_TYPE;
					break;
				case OBJECT_TSPARSER:
					tag = CMDTAG_CREATE_TEXT_SEARCH_PARSER;
					break;
				case OBJECT_TSDICTIONARY:
					tag = CMDTAG_CREATE_TEXT_SEARCH_DICTIONARY;
					break;
				case OBJECT_TSTEMPLATE:
					tag = CMDTAG_CREATE_TEXT_SEARCH_TEMPLATE;
					break;
				case OBJECT_TSCONFIGURATION:
					tag = CMDTAG_CREATE_TEXT_SEARCH_CONFIGURATION;
					break;
				case OBJECT_COLLATION:
					tag = CMDTAG_CREATE_COLLATION;
					break;
				case OBJECT_ACCESS_METHOD:
					tag = CMDTAG_CREATE_ACCESS_METHOD;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_CompositeTypeStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_CreateEnumStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_CreateRangeStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_AlterEnumStmt:
			tag = CMDTAG_ALTER_TYPE;
			break;

		case T_ViewStmt:
			tag = CMDTAG_CREATE_VIEW;
			break;

		case T_CreateFunctionStmt:
			if (((CreateFunctionStmt *) parsetree)->is_procedure)
				tag = CMDTAG_CREATE_PROCEDURE;
			else
				tag = CMDTAG_CREATE_FUNCTION;
			break;

		case T_IndexStmt:
			tag = CMDTAG_CREATE_INDEX;
			break;

		case T_RuleStmt:
			tag = CMDTAG_CREATE_RULE;
			break;

		case T_CreateSeqStmt:
			tag = CMDTAG_CREATE_SEQUENCE;
			break;

		case T_AlterSeqStmt:
			tag = CMDTAG_ALTER_SEQUENCE;
			break;

		case T_DoStmt:
			tag = CMDTAG_DO;
			break;

		case T_CreatedbStmt:
			tag = CMDTAG_CREATE_DATABASE;
			break;

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
			tag = CMDTAG_ALTER_DATABASE;
			break;

		case T_DropdbStmt:
			tag = CMDTAG_DROP_DATABASE;
			break;

		case T_NotifyStmt:
			tag = CMDTAG_NOTIFY;
			break;

		case T_ListenStmt:
			tag = CMDTAG_LISTEN;
			break;

		case T_UnlistenStmt:
			tag = CMDTAG_UNLISTEN;
			break;

		case T_LoadStmt:
			tag = CMDTAG_LOAD;
			break;

		case T_CallStmt:
			tag = CMDTAG_CALL;
			break;

		case T_ClusterStmt:
			tag = CMDTAG_CLUSTER;
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->is_vacuumcmd)
				tag = CMDTAG_VACUUM;
			else
				tag = CMDTAG_ANALYZE;
			break;

		case T_ExplainStmt:
			tag = CMDTAG_EXPLAIN;
			break;

		case T_CreateTableAsStmt:
			switch (((CreateTableAsStmt *) parsetree)->objtype)
			{
				case OBJECT_TABLE:
					if (((CreateTableAsStmt *) parsetree)->is_select_into)
						tag = CMDTAG_SELECT_INTO;
					else
						tag = CMDTAG_CREATE_TABLE_AS;
					break;
				case OBJECT_MATVIEW:
					tag = CMDTAG_CREATE_MATERIALIZED_VIEW;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_RefreshMatViewStmt:
			tag = CMDTAG_REFRESH_MATERIALIZED_VIEW;
			break;

		case T_AlterSystemStmt:
			tag = CMDTAG_ALTER_SYSTEM;
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = CMDTAG_SET;
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = CMDTAG_RESET;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_VariableShowStmt:
			tag = CMDTAG_SHOW;
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = CMDTAG_DISCARD_ALL;
					break;
				case DISCARD_PLANS:
					tag = CMDTAG_DISCARD_PLANS;
					break;
				case DISCARD_TEMP:
					tag = CMDTAG_DISCARD_TEMP;
					break;
				case DISCARD_SEQUENCES:
					tag = CMDTAG_DISCARD_SEQUENCES;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_CreateTransformStmt:
			tag = CMDTAG_CREATE_TRANSFORM;
			break;

		case T_CreateTrigStmt:
			tag = CMDTAG_CREATE_TRIGGER;
			break;

		case T_CreateEventTrigStmt:
			tag = CMDTAG_CREATE_EVENT_TRIGGER;
			break;

		case T_AlterEventTrigStmt:
			tag = CMDTAG_ALTER_EVENT_TRIGGER;
			break;

		case T_CreatePLangStmt:
			tag = CMDTAG_CREATE_LANGUAGE;
			break;

		case T_CreateRoleStmt:
			tag = CMDTAG_CREATE_ROLE;
			break;

		case T_AlterRoleStmt:
			tag = CMDTAG_ALTER_ROLE;
			break;

		case T_AlterRoleSetStmt:
			tag = CMDTAG_ALTER_ROLE;
			break;

		case T_DropRoleStmt:
			tag = CMDTAG_DROP_ROLE;
			break;

		case T_DropOwnedStmt:
			tag = CMDTAG_DROP_OWNED;
			break;

		case T_ReassignOwnedStmt:
			tag = CMDTAG_REASSIGN_OWNED;
			break;

		case T_LockStmt:
			tag = CMDTAG_LOCK_TABLE;
			break;

		case T_ConstraintsSetStmt:
			tag = CMDTAG_SET_CONSTRAINTS;
			break;

		case T_CheckPointStmt:
			tag = CMDTAG_CHECKPOINT;
			break;

		case T_ReindexStmt:
			tag = CMDTAG_REINDEX;
			break;

		case T_CreateConversionStmt:
			tag = CMDTAG_CREATE_CONVERSION;
			break;

		case T_CreateCastStmt:
			tag = CMDTAG_CREATE_CAST;
			break;

		case T_CreateOpClassStmt:
			tag = CMDTAG_CREATE_OPERATOR_CLASS;
			break;

		case T_CreateOpFamilyStmt:
			tag = CMDTAG_CREATE_OPERATOR_FAMILY;
			break;

		case T_AlterOpFamilyStmt:
			tag = CMDTAG_ALTER_OPERATOR_FAMILY;
			break;

		case T_AlterOperatorStmt:
			tag = CMDTAG_ALTER_OPERATOR;
			break;

		case T_AlterTypeStmt:
			tag = CMDTAG_ALTER_TYPE;
			break;

		case T_AlterTSDictionaryStmt:
			tag = CMDTAG_ALTER_TEXT_SEARCH_DICTIONARY;
			break;

		case T_AlterTSConfigurationStmt:
			tag = CMDTAG_ALTER_TEXT_SEARCH_CONFIGURATION;
			break;

		case T_CreatePolicyStmt:
			tag = CMDTAG_CREATE_POLICY;
			break;

		case T_AlterPolicyStmt:
			tag = CMDTAG_ALTER_POLICY;
			break;

		case T_CreateAmStmt:
			tag = CMDTAG_CREATE_ACCESS_METHOD;
			break;

		case T_CreatePublicationStmt:
			tag = CMDTAG_CREATE_PUBLICATION;
			break;

		case T_AlterPublicationStmt:
			tag = CMDTAG_ALTER_PUBLICATION;
			break;

		case T_CreateSubscriptionStmt:
			tag = CMDTAG_CREATE_SUBSCRIPTION;
			break;

		case T_AlterSubscriptionStmt:
			tag = CMDTAG_ALTER_SUBSCRIPTION;
			break;

		case T_DropSubscriptionStmt:
			tag = CMDTAG_DROP_SUBSCRIPTION;
			break;

		case T_AlterCollationStmt:
			tag = CMDTAG_ALTER_COLLATION;
			break;

		case T_PrepareStmt:
			tag = CMDTAG_PREPARE;
			break;

		case T_ExecuteStmt:
			tag = CMDTAG_EXECUTE;
			break;

		case T_CreateStatsStmt:
			tag = CMDTAG_CREATE_STATISTICS;
			break;

		case T_AlterStatsStmt:
			tag = CMDTAG_ALTER_STATISTICS;
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = CMDTAG_DEALLOCATE_ALL;
				else
					tag = CMDTAG_DEALLOCATE;
			}
			break;

			/*
			 * already-planned queries
			 *
			 * 已经规划的查询。
			 */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 *
						 * 我们在这里稍微多加注意，使结果可用于关于只读语句的报错。
						 */
						if (stmt->rowMarks != NIL)
						{
							/*
							 * not 100% but probably close enough
							 *
							 * 不是 100% 准确，但可能已经足够接近。
							 */
							switch (((PlanRowMark *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = CMDTAG_SELECT_FOR_KEY_SHARE;
									break;
								case LCS_FORSHARE:
									tag = CMDTAG_SELECT_FOR_SHARE;
									break;
								case LCS_FORNOKEYUPDATE:
									tag = CMDTAG_SELECT_FOR_NO_KEY_UPDATE;
									break;
								case LCS_FORUPDATE:
									tag = CMDTAG_SELECT_FOR_UPDATE;
									break;
								default:
									tag = CMDTAG_SELECT;
									break;
							}
						}
						else
							tag = CMDTAG_SELECT;
						break;
					case CMD_UPDATE:
						tag = CMDTAG_UPDATE;
						break;
					case CMD_INSERT:
						tag = CMDTAG_INSERT;
						break;
					case CMD_DELETE:
						tag = CMDTAG_DELETE;
						break;
					case CMD_MERGE:
						tag = CMDTAG_MERGE;
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

			/*
			 * parsed-and-rewritten-but-not-planned queries
			 *
			 * 已解析并重写但尚未规划的查询。
			 */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 *
						 * 我们在这里稍微多加注意，使结果可用于关于只读语句的报错。
						 */
						if (stmt->rowMarks != NIL)
						{
							/*
							 * not 100% but probably close enough
							 *
							 * 不是 100% 准确，但可能已经足够接近。
							 */
							switch (((RowMarkClause *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = CMDTAG_SELECT_FOR_KEY_SHARE;
									break;
								case LCS_FORSHARE:
									tag = CMDTAG_SELECT_FOR_SHARE;
									break;
								case LCS_FORNOKEYUPDATE:
									tag = CMDTAG_SELECT_FOR_NO_KEY_UPDATE;
									break;
								case LCS_FORUPDATE:
									tag = CMDTAG_SELECT_FOR_UPDATE;
									break;
								default:
									tag = CMDTAG_UNKNOWN;
									break;
							}
						}
						else
							tag = CMDTAG_SELECT;
						break;
					case CMD_UPDATE:
						tag = CMDTAG_UPDATE;
						break;
					case CMD_INSERT:
						tag = CMDTAG_INSERT;
						break;
					case CMD_DELETE:
						tag = CMDTAG_DELETE;
						break;
					case CMD_MERGE:
						tag = CMDTAG_MERGE;
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			tag = CMDTAG_UNKNOWN;
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		utility to get the minimum log_statement level for a command,
 *		given either a raw (un-analyzed) parsetree, an analyzed Query,
 *		or a PlannedStmt.
 *
 * GetCommandLogLevel
 *		用于获取命令最低 log_statement 级别的工具函数；输入可以是原始
 *		（未分析）parsetree、已分析的 Query 或 PlannedStmt。
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 *
 * 它必须处理所有命令类型，但由于其中绝大多数是工具命令，把它放在这里似乎
 * 是合理的。
 *
 * The flow unwraps statement wrappers, looks through PREPARE, EXECUTE, and
 * EXPLAIN ANALYZE when possible, and classifies the command as DDL, modifying,
 * or all-statements logging.
 *
 * 核心流程是解包语句包装，在可能时透过 PREPARE、EXECUTE 和 EXPLAIN ANALYZE
 * 查看内部语句，并将命令归类为 DDL、修改类或所有语句日志级别。
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/*
			 * recurse if we're given a RawStmt
			 *
			 * 如果给定 RawStmt，则递归处理。
			 */
		case T_RawStmt:
			lev = GetCommandLogLevel(((RawStmt *) parsetree)->stmt);
			break;

			/*
			 * raw plannable queries
			 *
			 * 原始可规划查询。
			 */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;	/* SELECT INTO
									 *
									 * SELECT INTO 查询
									 */
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PLAssignStmt:
			lev = LOGSTMT_ALL;
			break;

			/*
			 * utility statements --- same whether raw or cooked
			 *
			 * 工具语句——无论原始还是处理后都相同。
			 */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_ImportForeignSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_SecLabelStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/*
				 * Look through a PREPARE to the contained stmt
				 *
				 * 透过 PREPARE 查看其中包含的 stmt。
				 */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/*
				 * Look through an EXECUTE to the referenced stmt
				 *
				 * 透过 EXECUTE 查看所引用的 stmt。
				 */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps && ps->plansource->raw_parse_tree)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree->stmt);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectDependsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOperatorStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableMoveAllStmt:
		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDefaultPrivilegesStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRangeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CallStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;
				bool		analyze = false;
				ListCell   *lc;

				/*
				 * Look through an EXPLAIN ANALYZE to the contained stmt
				 *
				 * 透过 EXPLAIN ANALYZE 查看其中包含的 stmt。
				 */
				foreach(lc, stmt->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
					/*
					 * don't "break", as explain.c will use the last value
					 *
					 * 不要 "break"，因为 explain.c 会使用最后一个值。
					 */
				}
				if (analyze)
					return GetCommandLogLevel(stmt->query);

				/*
				 * Plain EXPLAIN isn't so interesting
				 *
				 * 普通 EXPLAIN 没那么值得关注。
				 */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_CreateTableAsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RefreshMatViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSystemStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* should this be DDL?
								 *
								 * 这应当算作 DDL 吗？
								 */
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTransformStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateAmStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePublicationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPublicationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStatsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterStatsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterCollationStmt:
			lev = LOGSTMT_DDL;
			break;

			/*
			 * already-planned queries
			 *
			 * 已经规划的查询。
			 */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
					case CMD_MERGE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/*
			 * parsed-and-rewritten-but-not-planned queries
			 *
			 * 已解析并重写但尚未规划的查询。
			 */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
					case CMD_MERGE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}
