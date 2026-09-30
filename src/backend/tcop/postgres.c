/*-------------------------------------------------------------------------
 *
 * postgres.c
 *	  POSTGRES C Backend Interface
 *
 * postgres.c POSTGRES C 后端接口
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/postgres.c
 *
 * NOTES
 *	  this is the "main" module of the postgres backend and
 *	  hence the main module of the "traffic cop".
 *
 * 这是 postgres 后端的“主”模块，因此也是“交通警察”的主模块。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifdef USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#include "access/parallel.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/async.h"
#include "commands/event_trigger.h"
#include "commands/prepare.h"
#include "common/pg_prng.h"
#include "jit/jit.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "mb/stringinfo_mb.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "pg_getopt.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/slot.h"
#include "replication/walsender.h"
#include "rewrite/rewriteHandler.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinval.h"
#include "tcop/backend_startup.h"
#include "tcop/fastpath.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/* ----------------
 *		global variables
 *
 * 全局变量
 * ----------------
 */
const char *debug_query_string; /* client-supplied query string
								 *
								 * 客户端提供的查询字符串
								 */

/* Note: whereToSendOutput is initialized for the bootstrap/standalone case
 *
 * 注意：whereToSendOutput 是针对 bootstrap/standalone 情况进行初始化的
 */
CommandDest whereToSendOutput = DestDebug;

/* flag for logging end of session
 *
 * 用于记录会话结束的标志
 */
bool		Log_disconnections = false;

int			log_statement = LOGSTMT_NONE;

/* wait N seconds to allow attach from a debugger
 *
 * 等待 N 秒以允许从调试器附加
 */
int			PostAuthDelay = 0;

/* Time between checks that the client is still connected.
 *
 * 检查客户端是否仍处于连接状态之间的时间。
 */
int			client_connection_check_interval = 0;

/* flags for non-system relation kinds to restrict use
 *
 * 非系统关系类型标志以限制使用
 */
int			restrict_nonsystem_relation_kind;

/* ----------------
 *		private typedefs etc
 *
 * 私有 typedef 等
 * ----------------
 */

/* type of argument for bind_param_error_callback
 *
 * bind_param_error_callback 的参数类型
 */
typedef struct BindParamCbData
{
	const char *portalName;
	int			paramno;		/* zero-based param number, or -1 initially
				 *
				 * 从零开始的参数编号，或最初为 -1
				 */
	const char *paramval;		/* textual input string, if available
						 *
						 * 文本输入字符串（如果有）
						 */
} BindParamCbData;

/* ----------------
 *		private variables
 *
 * 私有变量
 * ----------------
 */

/*
 * Flag to keep track of whether we have started a transaction.
 * For extended query protocol this has to be remembered across messages.
 *
 * 用于跟踪我们是否已开始事务的标记。对于扩展查询协议，必须跨消息记住这一点。
 */
static bool xact_started = false;

/*
 * Flag to indicate that we are doing the outer loop's read-from-client,
 * as opposed to any random read from client that might happen within
 * commands like COPY FROM STDIN.
 *
 * 标志表示我们正在执行外部循环的从客户端读取，而不是在像 COPY FROM STDIN
 * 这样的命令中可能发生的从客户端进行的任何随机读取。
 */
static bool DoingCommandRead = false;

/*
 * Flags to implement skip-till-Sync-after-error behavior for messages of
 * the extended query protocol.
 *
 * 用于为扩展查询协议的消息实现错误后跳过直到同步行为的标志。
 */
static bool doing_extended_query_message = false;
static bool ignore_till_sync = false;

/*
 * If an unnamed prepared statement exists, it's stored here.
 * We keep it separate from the hashtable kept by commands/prepare.c
 * in order to reduce overhead for short-lived queries.
 *
 * 如果存在未命名的预准备语句，则将其存储在这里。我们将其与commands/prepare.c
 * 保存的哈希表分开，以减少短期查询的开销。
 */
static CachedPlanSource *unnamed_stmt_psrc = NULL;

/* assorted command-line switches
 *
 * 各种命令行开关
 */
static const char *userDoption = NULL;	/* -D switch
										 *
										 * -D 开关
										 */
static bool EchoQuery = false;	/* -E switch
								 *
								 * -E 开关
								 */
static bool UseSemiNewlineNewline = false;	/* -j switch
											 *
											 * -j 开关
											 */

/* whether or not, and why, we were canceled by conflict with recovery
 *
 * 是否以及为什么我们因与恢复冲突而被取消
 */
static volatile sig_atomic_t RecoveryConflictPending = false;
static volatile sig_atomic_t RecoveryConflictPendingReasons[NUM_PROCSIGNALS];

/* reused buffer to pass to SendRowDescriptionMessage()
 *
 * 重用缓冲区传递给 SendRowDescriptionMessage()
 */
static MemoryContext row_description_context = NULL;
static StringInfoData row_description_buf;

/* ----------------------------------------------------------------
 *		decls for routines only used in this file
 *
 * decls 仅用于此文件中的例程
 * ----------------------------------------------------------------
 */
static int	InteractiveBackend(StringInfo inBuf);
static int	interactive_getc(void);
static int	SocketBackend(StringInfo inBuf);
static int	ReadCommand(StringInfo inBuf);
static void forbidden_in_wal_sender(char firstchar);
static bool check_log_statement(List *stmt_list);
static int	errdetail_execute(List *raw_parsetree_list);
static int	errdetail_params(ParamListInfo params);
static int	errdetail_abort(void);
static void bind_param_error_callback(void *arg);
static void start_xact_command(void);
static void finish_xact_command(void);
static bool IsTransactionExitStmt(Node *parsetree);
static bool IsTransactionExitStmtList(List *pstmts);
static bool IsTransactionStmtList(List *pstmts);
static void drop_unnamed_stmt(void);
static void log_disconnections(int code, Datum arg);
static void enable_statement_timeout(void);
static void disable_statement_timeout(void);


/* ----------------------------------------------------------------
 *		infrastructure for valgrind debugging
 *
 * 用于 valgrind 调试的基础设施
 * ----------------------------------------------------------------
 */
#ifdef USE_VALGRIND
/* This variable should be set at the top of the main loop.
 *
 * 该变量应设置在主循环的顶部。
 */
static unsigned int old_valgrind_error_count;

/*
 * If Valgrind detected any errors since old_valgrind_error_count was updated,
 * report the current query as the cause.  This should be called at the end
 * of message processing.
 *
 * 如果自 old_valgrind_error_count 更新后 Valgrind
 * 检测到任何错误，请将当前查询报告为原因。这应该在消息处理结束时调用。
 */
static void
valgrind_report_error_query(const char *query)
{
	unsigned int valgrind_error_count = VALGRIND_COUNT_ERRORS;

	if (unlikely(valgrind_error_count != old_valgrind_error_count) &&
		query != NULL)
		VALGRIND_PRINTF("Valgrind detected %u error(s) during execution of \"%s\"\n",
						valgrind_error_count - old_valgrind_error_count,
						query);
}

#else							/* !USE_VALGRIND */
#define valgrind_report_error_query(query) ((void) 0)
#endif							/* USE_VALGRIND */


/* ----------------------------------------------------------------
 *		routines to obtain user input
 *
 * 获取用户输入的例程
 * ----------------------------------------------------------------
 */

/* ----------------
 *	InteractiveBackend() is called for user interactive connections
 *
 * InteractiveBackend() 被调用用于用户交互连接
 *
 *	the string entered by the user is placed in its parameter inBuf,
 *	and we act like a Q message was received.
 *
 * 用户输入的字符串被放在它的参数inBuf中，我们就像收到了一条Q消息一样。
如果看到文件结尾输入，则返回
 *
 *	EOF is returned if end-of-file input is seen; time to shut down.
 *
 * EOF；是时候关闭了。
 * ----------------
 */

static int
InteractiveBackend(StringInfo inBuf)
{
	int			c;				/* character read from getc()
				 *
				 * 从 getc() 读取的字符
				 */

	/*
	 * display a prompt and obtain input from the user
	 *
	 * 显示提示并获取用户输入
	 */
	printf("backend> ");
	fflush(stdout);

	resetStringInfo(inBuf);

	/*
	 * Read characters until EOF or the appropriate delimiter is seen.
	 *
	 * 读取字符，直到看到 EOF 或适当的分隔符。
	 */
	while ((c = interactive_getc()) != EOF)
	{
		if (c == '\n')
		{
			if (UseSemiNewlineNewline)
			{
				/*
				 * In -j mode, semicolon followed by two newlines ends the
				 * command; otherwise treat newline as regular character.
				 *
				 * -j 模式下，分号后跟两个换行符结束命令；否则将换行符视为常规字符。
				 */
				if (inBuf->len > 1 &&
					inBuf->data[inBuf->len - 1] == '\n' &&
					inBuf->data[inBuf->len - 2] == ';')
				{
					/* might as well drop the second newline
					 *
					 * 不妨删除第二个换行符
					 */
					break;
				}
			}
			else
			{
				/*
				 * In plain mode, newline ends the command unless preceded by
				 * backslash.
				 *
				 * 在普通模式下，换行符结束命令，除非前面有反斜杠。
				 */
				if (inBuf->len > 0 &&
					inBuf->data[inBuf->len - 1] == '\\')
				{
					/* discard backslash from inBuf
					 *
					 * 丢弃 inBuf 中的反斜杠
					 */
					inBuf->data[--inBuf->len] = '\0';
					/* discard newline too
					 *
					 * 也丢弃换行符
					 */
					continue;
				}
				else
				{
					/* keep the newline character, but end the command
					 *
					 * 保留换行符，但结束命令
					 */
					appendStringInfoChar(inBuf, '\n');
					break;
				}
			}
		}

		/* Not newline, or newline treated as regular character
		 *
		 * 不是换行符，或将换行符视为常规字符
		 */
		appendStringInfoChar(inBuf, (char) c);
	}

	/* No input before EOF signal means time to quit.
	 *
	 * EOF信号之前没有输入表示退出时间到了。
	 */
	if (c == EOF && inBuf->len == 0)
		return EOF;

	/*
	 * otherwise we have a user query so process it.
	 *
	 * 否则我们有一个用户查询，所以处理它。
	 */

	/* Add '\0' to make it look the same as message case.
	 *
	 * 添加“\0”使其看起来与消息大小写相同。
	 */
	appendStringInfoChar(inBuf, (char) '\0');

	/*
	 * if the query echo flag was given, print the query..
	 *
	 * 如果给出了查询回显标志，则打印查询。
	 */
	if (EchoQuery)
		printf("statement: %s\n", inBuf->data);
	fflush(stdout);

	return PqMsg_Query;
}

/*
 * interactive_getc -- collect one character from stdin
 *
 * Interactive_getc -- 从标准输入中收集一个字符
 *
 * Even though we are not reading from a "client" process, we still want to
 * respond to signals, particularly SIGTERM/SIGQUIT.
 *
 * 即使我们不是从“客户端”进程读取数据，我们仍然想要响应信号，特别是 SIGTERM/SIGQUIT。
 */
static int
interactive_getc(void)
{
	int			c;

	/*
	 * This will not process catchup interrupts or notifications while
	 * reading. But those can't really be relevant for a standalone backend
	 * anyway. To properly handle SIGTERM there's a hack in die() that
	 * directly processes interrupts at this stage...
	 *
	 * 读取时不会处理追赶中断或通知。但无论如何，这些与独立后端并不真正相关。为了正确处理 SIGTERM，die() 中有一个
	 * hack，可以在这个阶段直接处理中断......
	 */
	CHECK_FOR_INTERRUPTS();

	c = getc(stdin);

	ProcessClientReadInterrupt(false);

	return c;
}

/* ----------------
 *	SocketBackend()		Is called for frontend-backend connections
 *
 * SocketBackend() 前后端连接调用
 *
 *	Returns the message type code, and loads message body data into inBuf.
 *
 * 返回消息类型代码，并将消息体数据加载到inBuf中。
如果连接丢失，则返回
 *
 *	EOF is returned if the connection is lost.
 *
 * EOF。
 * ----------------
 */
static int
SocketBackend(StringInfo inBuf)
{
	int			qtype;
	int			maxmsglen;

	/*
	 * Get message type code from the frontend.
	 *
	 * 从前端获取消息类型代码。
	 */
	HOLD_CANCEL_INTERRUPTS();
	pq_startmsgread();
	qtype = pq_getbyte();

	if (qtype == EOF)			/* frontend disconnected
						 *
						 * 前端已断开连接
						 */
	{
		if (IsTransactionState())
			ereport(COMMERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("unexpected EOF on client connection with an open transaction")));
		else
		{
			/*
			 * Can't send DEBUG log messages to client at this point. Since
			 * we're disconnecting right away, we don't need to restore
			 * whereToSendOutput.
			 *
			 * 此时无法将 DEBUG 日志消息发送到客户端。由于我们立即断开连接，因此不需要恢复 whereToSendOutput。
			 */
			whereToSendOutput = DestNone;
			ereport(DEBUG1,
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
					 errmsg_internal("unexpected EOF on client connection")));
		}
		return qtype;
	}

	/*
	 * Validate message type code before trying to read body; if we have lost
	 * sync, better to say "command unknown" than to run out of memory because
	 * we used garbage as a length word.  We can also select a type-dependent
	 * limit on what a sane length word could be.  (The limit could be chosen
	 * more granularly, but it's not clear it's worth fussing over.)
	 *
	 * 在尝试读取正文之前验证消息类型代码；如果我们失去了同步，最好说“命令未知”，而不是内存不足，因为我们使用垃圾作为长度字。我们还可以选
	 * 择一个与类型相关的限制，限制单词的长度。 （可以更精细地选择限制，但尚不清楚是否值得大惊小怪。）
	 *
	 * This also gives us a place to set the doing_extended_query_message flag
	 * as soon as possible.
	 *
	 * 这也为我们提供了一个尽快设置doing_extended_query_message标志的地方。
	 */
	switch (qtype)
	{
		case PqMsg_Query:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case PqMsg_FunctionCall:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case PqMsg_Terminate:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			ignore_till_sync = false;
			break;

		case PqMsg_Bind:
		case PqMsg_Parse:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = true;
			break;

		case PqMsg_Close:
		case PqMsg_Describe:
		case PqMsg_Execute:
		case PqMsg_Flush:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = true;
			break;

		case PqMsg_Sync:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			/* stop any active skip-till-Sync
			 *
			 * 停止任何活动的skip-till-Sync
			 */
			ignore_till_sync = false;
			/* mark not-extended, so that a new error doesn't begin skip
			 *
			 * 标记未扩展，以便新错误不会开始跳过
			 */
			doing_extended_query_message = false;
			break;

		case PqMsg_CopyData:
			maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		case PqMsg_CopyDone:
		case PqMsg_CopyFail:
			maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
			doing_extended_query_message = false;
			break;

		default:

			/*
			 * Otherwise we got garbage from the frontend.  We treat this as
			 * fatal because we have probably lost message boundary sync, and
			 * there's no good way to recover.
			 *
			 * 否则我们会从前端得到垃圾。我们将此视为致命的，因为我们可能丢失了消息边界同步，并且没有好的方法来恢复。
			 */
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid frontend message type %d", qtype)));
			maxmsglen = 0;		/* keep compiler quiet
					 *
					 * 让编译器保持安静
					 */
			break;
	}

	/*
	 * In protocol version 3, all frontend messages have a length word next
	 * after the type code; we can read the message contents independently of
	 * the type.
	 *
	 * 在协议版本3中，所有前端消息在类型代码之后都有一个长度字；我们可以独立于类型来读取消息内容。
	 */
	if (pq_getmessage(inBuf, maxmsglen))
		return EOF;				/* suitable message already logged
				 *
				 * 合适的消息已记录
				 */
	RESUME_CANCEL_INTERRUPTS();

	return qtype;
}

/* ----------------
 *		ReadCommand reads a command from either the frontend or
 *		standard input, places it in inBuf, and returns the
 *		message type code (first byte of the message).
 *		EOF is returned if end of file.
 *
 * ReadCommand 从前端或标准输入读取命令，将其放入 inBuf
 * 中，并返回消息类型代码（消息的第一个字节）。如果文件结束则返回 EOF。
 * ----------------
 */
static int
ReadCommand(StringInfo inBuf)
{
	int			result;

	if (whereToSendOutput == DestRemote)
		result = SocketBackend(inBuf);
	else
		result = InteractiveBackend(inBuf);
	return result;
}

/*
 * ProcessClientReadInterrupt() - Process interrupts specific to client reads
 *
 * ProcessClientReadInterrupt() - 处理特定于客户端读取的中断
 *
 * This is called just before and after low-level reads.
 * 'blocked' is true if no data was available to read and we plan to retry,
 * false if about to read or done reading.
 *
 * 这在低级读取之前和之后调用。如果没有数据可供读取并且我们计划重试，则“blocked”为 true；如果即将读取或已完成读取，则为
 * false。
 *
 * Must preserve errno!
 *
 * 必须保留errno！
 */
void
ProcessClientReadInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (DoingCommandRead)
	{
		/* Check for general interrupts that arrived before/while reading
		 *
		 * 检查读取之前/读取时到达的一般中断
		 */
		CHECK_FOR_INTERRUPTS();

		/* Process sinval catchup interrupts, if any
		 *
		 * 处理 sinval 追赶中断（如果有）
		 */
		if (catchupInterruptPending)
			ProcessCatchupInterrupt();

		/* Process notify interrupts, if any
		 *
		 * 进程通知中断（如果有）
		 */
		if (notifyInterruptPending)
			ProcessNotifyInterrupt(true);
	}
	else if (ProcDiePending)
	{
		/*
		 * We're dying.  If there is no data available to read, then it's safe
		 * (and sane) to handle that now.  If we haven't tried to read yet,
		 * make sure the process latch is set, so that if there is no data
		 * then we'll come back here and die.  If we're done reading, also
		 * make sure the process latch is set, as we might've undesirably
		 * cleared it while reading.
		 *
		 * 我们快死了。如果没有可供读取的数据，那么现在处理它是安全（且理智）的。如果我们还没有尝试读取，请确保设置了进程锁存器，这样如果没有数
		 * 据，我们就会回到这里并死掉。如果我们完成读取，还要确保设置了进程锁存器，因为我们可能在读取时意外地清除了它。
		 */
		if (blocked)
			CHECK_FOR_INTERRUPTS();
		else
			SetLatch(MyLatch);
	}

	errno = save_errno;
}

/*
 * ProcessClientWriteInterrupt() - Process interrupts specific to client writes
 *
 * ProcessClientWriteInterrupt() - 处理特定于客户端写入的中断
 *
 * This is called just before and after low-level writes.
 * 'blocked' is true if no data could be written and we plan to retry,
 * false if about to write or done writing.
 *
 * 这在低级写入之前和之后调用。如果无法写入数据并且我们计划重试，则“blocked”为 true；如果即将写入或已完成写入，则为
 * false。
 *
 * Must preserve errno!
 *
 * 必须保留errno！
 */
void
ProcessClientWriteInterrupt(bool blocked)
{
	int			save_errno = errno;

	if (ProcDiePending)
	{
		/*
		 * We're dying.  If it's not possible to write, then we should handle
		 * that immediately, else a stuck client could indefinitely delay our
		 * response to the signal.  If we haven't tried to write yet, make
		 * sure the process latch is set, so that if the write would block
		 * then we'll come back here and die.  If we're done writing, also
		 * make sure the process latch is set, as we might've undesirably
		 * cleared it while writing.
		 *
		 * 我们快死了。如果无法写入，那么我们应该立即处理，否则卡住的客户端可能会无限期地延迟我们对信号的响应。如果我们还没有尝试写入，请确保设
		 * 置了进程锁存器，这样如果写入会阻塞，那么我们就会回到这里并死掉。如果我们完成写入，还要确保设置了进程锁存器，因为我们可能在写入时意外
		 * 地清除了它。
		 */
		if (blocked)
		{
			/*
			 * Don't mess with whereToSendOutput if ProcessInterrupts wouldn't
			 * service ProcDiePending.
			 *
			 * 如果 ProcessInterrupts 无法为 ProcDiePending 提供服务，请不要乱搞
			 * whereToSendOutput。
			 */
			if (InterruptHoldoffCount == 0 && CritSectionCount == 0)
			{
				/*
				 * We don't want to send the client the error message, as a)
				 * that would possibly block again, and b) it would likely
				 * lead to loss of protocol sync because we may have already
				 * sent a partial protocol message.
				 *
				 * 我们不想向客户端发送错误消息，因为a）可能会再次阻塞，b）可能会导致协议同步丢失，因为我们可能已经发送了部分协议消息。
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				CHECK_FOR_INTERRUPTS();
			}
		}
		else
			SetLatch(MyLatch);
	}

	errno = save_errno;
}

/*
 * Do raw parsing (only).
 *
 * 进行原始解析（仅）。
 *
 * A list of parsetrees (RawStmt nodes) is returned, since there might be
 * multiple commands in the given string.
 *
 * 返回解析树（RawStmt 节点）列表，因为给定字符串中可能有多个命令。
 *
 * NOTE: for interactive queries, it is important to keep this routine
 * separate from the analysis & rewrite stages.  Analysis and rewriting
 * cannot be done in an aborted transaction, since they require access to
 * database tables.  So, we rely on the raw parser to determine whether
 * we've seen a COMMIT or ABORT command; when we are in abort state, other
 * commands are not processed any further than the raw parse stage.
 *
 * 注意：对于交互式查询，将此例程与分析和重写阶段分开非常重要。无法在中止的事务中进行分析和重写，因为它们需要访问数据库表。因此，我们依
 * 靠原始解析器来确定我们是否看到了 COMMIT 或 ABORT
 * 命令；当我们处于中止状态时，除了原始解析阶段之外，其他命令不会被进一步处理。
 */
List *
pg_parse_query(const char *query_string)
{
	List	   *raw_parsetree_list;

	TRACE_POSTGRESQL_QUERY_PARSE_START(query_string);

	if (log_parser_stats)
		ResetUsage();

	raw_parsetree_list = raw_parser(query_string, RAW_PARSE_DEFAULT);

	if (log_parser_stats)
		ShowUsage("PARSER STATISTICS");

#ifdef DEBUG_NODE_TESTS_ENABLED

	/* Optional debugging check: pass raw parsetrees through copyObject()
	 *
	 * 可选调试检查：通过 copyObject() 传递原始解析树
	 */
	if (Debug_copy_parse_plan_trees)
	{
		List	   *new_list = copyObject(raw_parsetree_list);

		/* This checks both copyObject() and the equal() routines...
		 *
		 * 这会检查 copyObject() 和 equal() 例程...
		 */
		if (!equal(new_list, raw_parsetree_list))
			elog(WARNING, "copyObject() failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}

	/*
	 * Optional debugging check: pass raw parsetrees through
	 * outfuncs/readfuncs
	 *
	 * 可选调试检查：通过 outfuncs/readfuncs 传递原始解析树
	 */
	if (Debug_write_read_parse_plan_trees)
	{
		char	   *str = nodeToStringWithLocations(raw_parsetree_list);
		List	   *new_list = stringToNodeWithLocations(str);

		pfree(str);
		/* This checks both outfuncs/readfuncs and the equal() routines...
		 *
		 * 这会检查 outfuncs/readfuncs 和 equal() 例程...
		 */
		if (!equal(new_list, raw_parsetree_list))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal raw parse tree");
		else
			raw_parsetree_list = new_list;
	}

#endif							/* DEBUG_NODE_TESTS_ENABLED */

	TRACE_POSTGRESQL_QUERY_PARSE_DONE(query_string);

	return raw_parsetree_list;
}

/*
 * Given a raw parsetree (gram.y output), and optionally information about
 * types of parameter symbols ($n), perform parse analysis and rule rewriting.
 *
 * 给定一个原始解析树（gram.y 输出），以及有关参数符号类型（$n）的可选信息，执行解析分析和规则重写。
 *
 * A list of Query nodes is returned, since either the analyzer or the
 * rewriter might expand one query to several.
 *
 * 返回查询节点列表，因为分析器或重写器可能会将一个查询扩展为多个查询。
 *
 * NOTE: for reasons mentioned above, this must be separate from raw parsing.
 *
 * 注意：由于上述原因，这必须与原始解析分开。
 */
List *
pg_analyze_and_rewrite_fixedparams(RawStmt *parsetree,
								   const char *query_string,
								   const Oid *paramTypes,
								   int numParams,
								   QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) Perform parse analysis.
	 *
	 * (1) 执行解析分析。
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_fixedparams(parsetree, query_string, paramTypes, numParams,
									  queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) Rewrite the queries, as necessary
	 *
	 * (2) 根据需要重写查询
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * Do parse analysis and rewriting.  This is the same as
 * pg_analyze_and_rewrite_fixedparams except that it's okay to deduce
 * information about $n symbol datatypes from context.
 *
 * 进行解析分析和重写。这与 pg_analyze_and_rewrite_fixedparams 相同，只是可以从上下文中推断出有关
 * $n 符号数据类型的信息。
 */
List *
pg_analyze_and_rewrite_varparams(RawStmt *parsetree,
								 const char *query_string,
								 Oid **paramTypes,
								 int *numParams,
								 QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) Perform parse analysis.
	 *
	 * (1) 执行解析分析。
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_varparams(parsetree, query_string, paramTypes, numParams,
									queryEnv);

	/*
	 * Check all parameter types got determined.
	 *
	 * 检查所有已确定的参数类型。
	 */
	for (int i = 0; i < *numParams; i++)
	{
		Oid			ptype = (*paramTypes)[i];

		if (ptype == InvalidOid || ptype == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of parameter $%d",
							i + 1)));
	}

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) Rewrite the queries, as necessary
	 *
	 * (2) 根据需要重写查询
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * Do parse analysis and rewriting.  This is the same as
 * pg_analyze_and_rewrite_fixedparams except that, instead of a fixed list of
 * parameter datatypes, a parser callback is supplied that can do
 * external-parameter resolution and possibly other things.
 *
 * 进行解析分析和重写。这与 pg_analyze_and_rewrite_fixedparams
 * 相同，只是提供了一个解析器回调来代替参数数据类型的固定列表，它可以执行外部参数解析以及可能的其他操作。
 */
List *
pg_analyze_and_rewrite_withcb(RawStmt *parsetree,
							  const char *query_string,
							  ParserSetupHook parserSetup,
							  void *parserSetupArg,
							  QueryEnvironment *queryEnv)
{
	Query	   *query;
	List	   *querytree_list;

	TRACE_POSTGRESQL_QUERY_REWRITE_START(query_string);

	/*
	 * (1) Perform parse analysis.
	 *
	 * (1) 执行解析分析。
	 */
	if (log_parser_stats)
		ResetUsage();

	query = parse_analyze_withcb(parsetree, query_string, parserSetup, parserSetupArg,
								 queryEnv);

	if (log_parser_stats)
		ShowUsage("PARSE ANALYSIS STATISTICS");

	/*
	 * (2) Rewrite the queries, as necessary
	 *
	 * (2) 根据需要重写查询
	 */
	querytree_list = pg_rewrite_query(query);

	TRACE_POSTGRESQL_QUERY_REWRITE_DONE(query_string);

	return querytree_list;
}

/*
 * Perform rewriting of a query produced by parse analysis.
 *
 * 重写解析分析生成的查询。
 *
 * Note: query must just have come from the parser, because we do not do
 * AcquireRewriteLocks() on it.
 *
 * 注意：查询必须来自解析器，因为我们不对它执行 AcquireRewriteLocks() 。
 */
List *
pg_rewrite_query(Query *query)
{
	List	   *querytree_list;

	if (Debug_print_parse)
		elog_node_display(LOG, "parse tree", query,
						  Debug_pretty_print);

	if (log_parser_stats)
		ResetUsage();

	if (query->commandType == CMD_UTILITY)
	{
		/* don't rewrite utilities, just dump 'em into result list
		 *
		 * 不要重写实用程序，只需将它们转储到结果列表中
		 */
		querytree_list = list_make1(query);
	}
	else
	{
		/* rewrite regular queries
		 *
		 * 重写常规查询
		 */
		querytree_list = QueryRewrite(query);
	}

	if (log_parser_stats)
		ShowUsage("REWRITER STATISTICS");

#ifdef DEBUG_NODE_TESTS_ENABLED

	/* Optional debugging check: pass querytree through copyObject()
	 *
	 * 可选调试检查：通过 copyObject() 传递查询树
	 */
	if (Debug_copy_parse_plan_trees)
	{
		List	   *new_list;

		new_list = copyObject(querytree_list);
		/* This checks both copyObject() and the equal() routines...
		 *
		 * 这会检查 copyObject() 和 equal() 例程...
		 */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "copyObject() failed to produce an equal rewritten parse tree");
		else
			querytree_list = new_list;
	}

	/* Optional debugging check: pass querytree through outfuncs/readfuncs
	 *
	 * 可选调试检查：通过 outfuncs/readfuncs 传递 querytree
	 */
	if (Debug_write_read_parse_plan_trees)
	{
		List	   *new_list = NIL;
		ListCell   *lc;

		foreach(lc, querytree_list)
		{
			Query	   *curr_query = lfirst_node(Query, lc);
			char	   *str = nodeToStringWithLocations(curr_query);
			Query	   *new_query = stringToNodeWithLocations(str);

			/*
			 * queryId is not saved in stored rules, but we must preserve it
			 * here to avoid breaking pg_stat_statements.
			 *
			 * queryId 未保存在存储规则中，但我们必须将其保留在这里以避免破坏 pg_stat_statements。
			 */
			new_query->queryId = curr_query->queryId;

			new_list = lappend(new_list, new_query);
			pfree(str);
		}

		/* This checks both outfuncs/readfuncs and the equal() routines...
		 *
		 * 这会检查 outfuncs/readfuncs 和 equal() 例程...
		 */
		if (!equal(new_list, querytree_list))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal rewritten parse tree");
		else
			querytree_list = new_list;
	}

#endif							/* DEBUG_NODE_TESTS_ENABLED */

	if (Debug_print_rewritten)
		elog_node_display(LOG, "rewritten parse tree", querytree_list,
						  Debug_pretty_print);

	return querytree_list;
}


/*
 * Generate a plan for a single already-rewritten query.
 * This is a thin wrapper around planner() and takes the same parameters.
 *
 * 为单个已重写的查询生成计划。这是 planner() 的一个薄包装，并采用相同的参数。
 */
PlannedStmt *
pg_plan_query(Query *querytree, const char *query_string, int cursorOptions,
			  ParamListInfo boundParams)
{
	PlannedStmt *plan;

	/* Utility commands have no plans.
	 *
	 * 实用程序命令没有计划。
	 */
	if (querytree->commandType == CMD_UTILITY)
		return NULL;

	/* Planner must have a snapshot in case it calls user-defined functions.
	 *
	 * Planner 必须有一个快照，以防它调用用户定义的函数。
	 */
	Assert(ActiveSnapshotSet());

	TRACE_POSTGRESQL_QUERY_PLAN_START();

	if (log_planner_stats)
		ResetUsage();

	/* call the optimizer
	 *
	 * 调用优化器
	 */
	plan = planner(querytree, query_string, cursorOptions, boundParams);

	if (log_planner_stats)
		ShowUsage("PLANNER STATISTICS");

#ifdef DEBUG_NODE_TESTS_ENABLED

	/* Optional debugging check: pass plan tree through copyObject()
	 *
	 * 可选调试检查：通过 copyObject() 传递计划树
	 */
	if (Debug_copy_parse_plan_trees)
	{
		PlannedStmt *new_plan = copyObject(plan);

		/*
		 * equal() currently does not have routines to compare Plan nodes, so
		 * don't try to test equality here.  Perhaps fix someday?
		 *
		 * equal() 目前没有比较 Plan 节点的例程，所以不要尝试在这里测试相等性。也许有一天会修复？
		 */
#ifdef NOT_USED
		/* This checks both copyObject() and the equal() routines...
		 *
		 * 这会检查 copyObject() 和 equal() 例程...
		 */
		if (!equal(new_plan, plan))
			elog(WARNING, "copyObject() failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}

	/* Optional debugging check: pass plan tree through outfuncs/readfuncs
	 *
	 * 可选调试检查：通过 outfuncs/readfuncs 传递计划树
	 */
	if (Debug_write_read_parse_plan_trees)
	{
		char	   *str;
		PlannedStmt *new_plan;

		str = nodeToStringWithLocations(plan);
		new_plan = stringToNodeWithLocations(str);
		pfree(str);

		/*
		 * equal() currently does not have routines to compare Plan nodes, so
		 * don't try to test equality here.  Perhaps fix someday?
		 *
		 * equal() 目前没有比较 Plan 节点的例程，所以不要尝试在这里测试相等性。也许有一天会修复？
		 */
#ifdef NOT_USED
		/* This checks both outfuncs/readfuncs and the equal() routines...
		 *
		 * 这会检查 outfuncs/readfuncs 和 equal() 例程...
		 */
		if (!equal(new_plan, plan))
			elog(WARNING, "outfuncs/readfuncs failed to produce an equal plan tree");
		else
#endif
			plan = new_plan;
	}

#endif							/* DEBUG_NODE_TESTS_ENABLED */

	/*
	 * Print plan if debugging.
	 *
	 * 如果调试则打印计划。
	 */
	if (Debug_print_plan)
		elog_node_display(LOG, "plan", plan, Debug_pretty_print);

	TRACE_POSTGRESQL_QUERY_PLAN_DONE();

	return plan;
}

/*
 * Generate plans for a list of already-rewritten queries.
 *
 * 为已重写的查询列表生成计划。
 *
 * For normal optimizable statements, invoke the planner.  For utility
 * statements, just make a wrapper PlannedStmt node.
 *
 * 对于正常的可优化语句，调用规划器。对于实用程序语句，只需创建一个包装器 PlannedStmt 节点即可。
 *
 * The result is a list of PlannedStmt nodes.
 *
 * 结果是 PlannedStmt 节点的列表。
 */
List *
pg_plan_queries(List *querytrees, const char *query_string, int cursorOptions,
				ParamListInfo boundParams)
{
	List	   *stmt_list = NIL;
	ListCell   *query_list;

	foreach(query_list, querytrees)
	{
		Query	   *query = lfirst_node(Query, query_list);
		PlannedStmt *stmt;

		if (query->commandType == CMD_UTILITY)
		{
			/* Utility commands require no planning.
			 *
			 * 实用程序命令不需要规划。
			 */
			stmt = makeNode(PlannedStmt);
			stmt->commandType = CMD_UTILITY;
			stmt->canSetTag = query->canSetTag;
			stmt->utilityStmt = query->utilityStmt;
			stmt->stmt_location = query->stmt_location;
			stmt->stmt_len = query->stmt_len;
			stmt->queryId = query->queryId;
		}
		else
		{
			stmt = pg_plan_query(query, query_string, cursorOptions,
								 boundParams);
		}

		stmt_list = lappend(stmt_list, stmt);
	}

	return stmt_list;
}


/*
 * exec_simple_query
 *
 * Execute a "simple Query" protocol message.
 *
 * 执行“简单查询”协议消息。
 */
static void
exec_simple_query(const char *query_string)
{
	CommandDest dest = whereToSendOutput;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	bool		save_log_statement_stats = log_statement_stats;
	bool		was_logged = false;
	bool		use_implicit_block;
	char		msec_str[32];

	/*
	 * Report query to various monitoring facilities.
	 *
	 * 向各监控设施报告查询。
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	TRACE_POSTGRESQL_QUERY_START(query_string);

	/*
	 * We use save_log_statement_stats so ShowUsage doesn't report incorrect
	 * results because ResetUsage wasn't called.
	 *
	 * 我们使用 save_log_statement_stats，因此 ShowUsage 不会报告错误的结果，因为 ResetUsage
	 * 未被调用。
	 */
	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * Start up a transaction command.  All queries generated by the
	 * query_string will be in this same command block, *unless* we find a
	 * BEGIN/COMMIT/ABORT statement; we have to force a new xact command after
	 * one of those, else bad things will happen in xact.c. (Note that this
	 * will normally change current memory context.)
	 *
	 * 启动事务命令。由 query_string 生成的所有查询都将位于同一个命令块中，*除非*我们找到
	 * BEGIN/COMMIT/ABORT 语句；我们必须在其中一个命令之后强制执行一个新的 xact 命令，否则 xact.c
	 * 中将会发生不好的事情。 （请注意，这通常会更改当前的内存上下文。）
	 */
	start_xact_command();

	/*
	 * Zap any pre-existing unnamed statement.  (While not strictly necessary,
	 * it seems best to define simple-Query mode as if it used the unnamed
	 * statement and portal; this ensures we recover any storage used by prior
	 * unnamed operations.)
	 *
	 * 删除任何预先存在的未命名语句。
	 * （虽然不是绝对必要的，但似乎最好定义简单查询模式，就像它使用未命名的语句和门户一样；这确保我们恢复先前未命名操作使用的任何存储。）
	 */
	drop_unnamed_stmt();

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 *
	 * 切换到适当的上下文来构造解析树。
	 */
	oldcontext = MemoryContextSwitchTo(MessageContext);

	/*
	 * Do basic parsing of the query or queries (this should be safe even if
	 * we are in aborted transaction state!)
	 *
	 * 对一个或多个查询进行基本解析（即使我们处于中止事务状态，这也应该是安全的！）
	 */
	parsetree_list = pg_parse_query(query_string);

	/* Log immediately if dictated by log_statement
	 *
	 * 如果 log_statement 指定则立即记录
	 */
	if (check_log_statement(parsetree_list))
	{
		ereport(LOG,
				(errmsg("statement: %s", query_string),
				 errhidestmt(true),
				 errdetail_execute(parsetree_list)));
		was_logged = true;
	}

	/*
	 * Switch back to transaction context to enter the loop.
	 *
	 * 切换回事务上下文进入循环。
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * For historical reasons, if multiple SQL statements are given in a
	 * single "simple Query" message, we execute them as a single transaction,
	 * unless explicit transaction control commands are included to make
	 * portions of the list be separate transactions.  To represent this
	 * behavior properly in the transaction machinery, we use an "implicit"
	 * transaction block.
	 *
	 * 由于历史原因，如果在单个“简单查询”消息中给出多个 SQL 语句，我们将它们作为单个事务执行，除非包含显式事务控制命令以使列表的各个
	 * 部分成为单独的事务。为了在交易机制中正确地表示这种行为，我们使用“隐式”交易块。
	 */
	use_implicit_block = (list_length(parsetree_list) > 1);

	/*
	 * Run through the raw parsetree(s) and process each one.
	 *
	 * 运行原始解析树并处理每一个。
	 */
	foreach(parsetree_item, parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);
		bool		snapshot_set = false;
		CommandTag	commandTag;
		QueryCompletion qc;
		MemoryContext per_parsetree_context = NULL;
		List	   *querytree_list,
				   *plantree_list;
		Portal		portal;
		DestReceiver *receiver;
		int16		format;
		const char *cmdtagname;
		size_t		cmdtaglen;

		pgstat_report_query_id(0, true);
		pgstat_report_plan_id(0, true);

		/*
		 * Get the command name for use in status display (it also becomes the
		 * default completion tag, down inside PortalRun).  Set ps_status and
		 * do any special start-of-SQL-command processing needed by the
		 * destination.
		 *
		 * 获取用于状态显示的命令名称（它也成为默认的完成标记，位于 PortalRun 内部）。设置 ps_status
		 * 并执行目标所需的任何特殊 SQL 开始命令处理。
		 */
		commandTag = CreateCommandTag(parsetree->stmt);
		cmdtagname = GetCommandTagNameAndLen(commandTag, &cmdtaglen);

		set_ps_display_with_len(cmdtagname, cmdtaglen);

		BeginCommand(commandTag, dest);

		/*
		 * If we are in an aborted transaction, reject all commands except
		 * COMMIT/ABORT.  It is important that this test occur before we try
		 * to do parse analysis, rewrite, or planning, since all those phases
		 * try to do database accesses, which may fail in abort state. (It
		 * might be safe to allow some additional utility commands in this
		 * state, but not many...)
		 *
		 * 如果我们处于中止事务中，则拒绝除 COMMIT/ABORT 之外的所有命令。重要的是，在我们尝试进行解析分析、重写或规划之前进行此测
		 * 试，因为所有这些阶段都尝试进行数据库访问，这可能会在中止状态下失败。
		 * （在这种状态下允许一些额外的实用程序命令可能是安全的，但不是很多......）
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(parsetree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/* Make sure we are in a transaction command
		 *
		 * 确保我们处于事务命令中
		 */
		start_xact_command();

		/*
		 * If using an implicit transaction block, and we're not already in a
		 * transaction block, start an implicit block to force this statement
		 * to be grouped together with any following ones.  (We must do this
		 * each time through the loop; otherwise, a COMMIT/ROLLBACK in the
		 * list would cause later statements to not be grouped.)
		 *
		 * 如果使用隐式事务块，并且我们尚未处于事务块中，请启动一个隐式块以强制此语句与任何后续语句组合在一起。
		 * （我们必须在每次循环中执行此操作；否则，列表中的 COMMIT/ROLLBACK 将导致后面的语句无法分组。）
		 */
		if (use_implicit_block)
			BeginImplicitTransactionBlock();

		/* If we got a cancel signal in parsing or prior command, quit
		 *
		 * 如果我们在解析或之前的命令中收到取消信号，请退出
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Set up a snapshot if parse analysis/planning will need one.
		 *
		 * 如果解析分析/规划需要快照，则设置快照。
		 */
		if (analyze_requires_snapshot(parsetree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * OK to analyze, rewrite, and plan this query.
		 *
		 * 可以分析、重写和计划此查询。
		 *
		 * Switch to appropriate context for constructing query and plan trees
		 * (these can't be in the transaction context, as that will get reset
		 * when the command is COMMIT/ROLLBACK).  If we have multiple
		 * parsetrees, we use a separate context for each one, so that we can
		 * free that memory before moving on to the next one.  But for the
		 * last (or only) parsetree, just use MessageContext, which will be
		 * reset shortly after completion anyway.  In event of an error, the
		 * per_parsetree_context will be deleted when MessageContext is reset.
		 *
		 * 切换到适当的上下文来构建查询和计划树（这些不能位于事务上下文中，因为当命令为 COMMIT/ROLLBACK 时，事务上下文将被重置
		 * ）。如果我们有多个解析树，我们会为每个解析树使用一个单独的上下文，这样我们就可以在进入下一个解析树之前释放该内存。但对于最后一个（或
		 * 唯一一个）解析树，只需使用 MessageContext，无论如何它都会在完成后不久重置。如果发生错误，重置
		 * MessageContext 时将删除 per_parsetree_context。
		 */
		if (lnext(parsetree_list, parsetree_item) != NULL)
		{
			per_parsetree_context =
				AllocSetContextCreate(MessageContext,
									  "per-parsetree message context",
									  ALLOCSET_DEFAULT_SIZES);
			oldcontext = MemoryContextSwitchTo(per_parsetree_context);
		}
		else
			oldcontext = MemoryContextSwitchTo(MessageContext);

		querytree_list = pg_analyze_and_rewrite_fixedparams(parsetree, query_string,
															NULL, 0, NULL);

		plantree_list = pg_plan_queries(querytree_list, query_string,
										CURSOR_OPT_PARALLEL_OK, NULL);

		/*
		 * Done with the snapshot used for parsing/planning.
		 *
		 * 完成用于解析/规划的快照。
		 *
		 * While it looks promising to reuse the same snapshot for query
		 * execution (at least for simple protocol), unfortunately it causes
		 * execution to use a snapshot that has been acquired before locking
		 * any of the tables mentioned in the query.  This creates user-
		 * visible anomalies, so refrain.  Refer to
		 * https://postgr.es/m/flat/5075D8DF.6050500@fuzzy.cz for details.
		 *
		 * 虽然看起来有希望重用相同的快照来执行查询（至少对于简单协议），但不幸的是，它会导致执行使用在锁定查询中提到的任何表之前已获取的快照。
		 * 这会造成用户可见的异常，因此请避免这样做。详情请参阅
		 * https://postgr.es/m/flat/5075D8DF.6050500@fuzzy.cz。
		 */
		if (snapshot_set)
			PopActiveSnapshot();

		/* If we got a cancel signal in analysis or planning, quit
		 *
		 * 如果我们在分析或计划中收到取消信号，请退出
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Create unnamed portal to run the query or queries in. If there
		 * already is one, silently drop it.
		 *
		 * 创建未命名的门户来运行一个或多个查询。如果已经有一个，则默默地删除它。
		 */
		portal = CreatePortal("", true, true);
		/* Don't display the portal in pg_cursors
		 *
		 * 不要在 pg_cursors 中显示门户
		 */
		portal->visible = false;

		/*
		 * We don't have to copy anything into the portal, because everything
		 * we are passing here is in MessageContext or the
		 * per_parsetree_context, and so will outlive the portal anyway.
		 *
		 * 我们不必将任何内容复制到门户中，因为我们在这里传递的所有内容都在 MessageContext 或
		 * per_parsetree_context 中，因此无论如何都会比门户寿命更长。
		 */
		PortalDefineQuery(portal,
						  NULL,
						  query_string,
						  commandTag,
						  plantree_list,
						  NULL);

		/*
		 * Start the portal.  No parameters here.
		 *
		 * 启动门户。这里没有参数。
		 */
		PortalStart(portal, NULL, 0, InvalidSnapshot);

		/*
		 * Select the appropriate output format: text unless we are doing a
		 * FETCH from a binary cursor.  (Pretty grotty to have to do this here
		 * --- but it avoids grottiness in other places.  Ah, the joys of
		 * backward compatibility...)
		 *
		 * 选择适当的输出格式：文本，除非我们从二进制游标执行 FETCH。
		 * （不得不在这里这样做真是太糟糕了——但它避免了其他地方的糟糕。啊，向后兼容的乐趣......）
		 */
		format = 0;				/* TEXT is default
				 *
				 * TEXT 为默认值
				 */
		if (IsA(parsetree->stmt, FetchStmt))
		{
			FetchStmt  *stmt = (FetchStmt *) parsetree->stmt;

			if (!stmt->ismove)
			{
				Portal		fportal = GetPortalByName(stmt->portalname);

				if (PortalIsValid(fportal) &&
					(fportal->cursorOptions & CURSOR_OPT_BINARY))
					format = 1; /* BINARY
								 *
								 * 二进制 */
			}
		}
		PortalSetResultFormat(portal, 1, &format);

		/*
		 * Now we can create the destination receiver object.
		 *
		 * 现在我们可以创建目标接收者对象。
		 */
		receiver = CreateDestReceiver(dest);
		if (dest == DestRemote)
			SetRemoteDestReceiverParams(receiver, portal);

		/*
		 * Switch back to transaction context for execution.
		 *
		 * 切换回事务上下文执行。
		 */
		MemoryContextSwitchTo(oldcontext);

		/*
		 * Run the portal to completion, and then drop it (and the receiver).
		 *
		 * 运行门户直至完成，然后删除它（和接收器）。
		 */
		(void) PortalRun(portal,
						 FETCH_ALL,
						 true,	/* always top level
				 *
				 * 始终处于最高水平
				 */
						 receiver,
						 receiver,
						 &qc);

		receiver->rDestroy(receiver);

		PortalDrop(portal, false);

		if (lnext(parsetree_list, parsetree_item) == NULL)
		{
			/*
			 * If this is the last parsetree of the query string, close down
			 * transaction statement before reporting command-complete.  This
			 * is so that any end-of-transaction errors are reported before
			 * the command-complete message is issued, to avoid confusing
			 * clients who will expect either a command-complete message or an
			 * error, not one and then the other.  Also, if we're using an
			 * implicit transaction block, we must close that out first.
			 *
			 * 如果这是查询字符串的最后一个解析树，则在报告命令完成之前关闭事务语句。这样一来，任何事务结束错误都会在发出命令完成消息之前报告，以避
			 * 免让客户端感到困惑，因为客户端会期望命令完成消息或错误，而不是一个然后另一个。另外，如果我们使用隐式事务块，我们必须首先将其关闭。
			 */
			if (use_implicit_block)
				EndImplicitTransactionBlock();
			finish_xact_command();
		}
		else if (IsA(parsetree->stmt, TransactionStmt))
		{
			/*
			 * If this was a transaction control statement, commit it. We will
			 * start a new xact command for the next command.
			 *
			 * 如果这是事务控制语句，则提交它。我们将为下一个命令启动一个新的 xact 命令。
			 */
			finish_xact_command();
		}
		else
		{
			/*
			 * We had better not see XACT_FLAGS_NEEDIMMEDIATECOMMIT set if
			 * we're not calling finish_xact_command().  (The implicit
			 * transaction block should have prevented it from getting set.)
			 *
			 * 如果我们不调用 finish_xact_command()，我们最好不要看到
			 * XACT_FLAGS_NEEDIMMEDIATECOMMIT 设置。 （隐式事务块应该阻止它被设置。）
			 */
			Assert(!(MyXactFlags & XACT_FLAGS_NEEDIMMEDIATECOMMIT));

			/*
			 * We need a CommandCounterIncrement after every query, except
			 * those that start or end a transaction block.
			 *
			 * 每次查询后我们都需要一个 CommandCounterIncrement，除了那些开始或结束事务块的查询。
			 */
			CommandCounterIncrement();

			/*
			 * Disable statement timeout between queries of a multi-query
			 * string, so that the timeout applies separately to each query.
			 * (Our next loop iteration will start a fresh timeout.)
			 *
			 * 禁用多查询字符串的查询之间的语句超时，以便超时单独应用于每个查询。 （我们的下一个循环迭代将开始一个新的超时。）
			 */
			disable_statement_timeout();
		}

		/*
		 * Tell client that we're done with this query.  Note we emit exactly
		 * one EndCommand report for each raw parsetree, thus one for each SQL
		 * command the client sent, regardless of rewriting. (But a command
		 * aborted by error will not send an EndCommand report at all.)
		 *
		 * 告诉客户我们已经完成了这个查询。请注意，我们为每个原始解析树准确地发出一份 EndCommand 报告，因此为客户端发送的每个
		 * SQL 命令发出一份报告，无论是否重写。 （但是由于错误而中止的命令根本不会发送 EndCommand 报告。）
		 */
		EndCommand(&qc, dest, false);

		/* Now we may drop the per-parsetree context, if one was created.
		 *
		 * 现在我们可以删除每个解析树上下文（如果已创建）。
		 */
		if (per_parsetree_context)
			MemoryContextDelete(per_parsetree_context);
	}							/* end loop over parsetrees
		 *
		 * 结束解析树循环
		 */

	/*
	 * Close down transaction statement, if one is open.  (This will only do
	 * something if the parsetree list was empty; otherwise the last loop
	 * iteration already did it.)
	 *
	 * 关闭事务语句（如果有）。 （只有当解析树列表为空时，这才会执行某些操作；否则最后一个循环迭代已经执行了该操作。）
	 */
	finish_xact_command();

	/*
	 * If there were no parsetrees, return EmptyQueryResponse message.
	 *
	 * 如果没有解析树，则返回 EmptyQueryResponse 消息。
	 */
	if (!parsetree_list)
		NullCommand(dest);

	/*
	 * Emit duration logging if appropriate.
	 *
	 * 如果合适的话，发出持续时间日志记录。
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  statement: %s",
							msec_str, query_string),
					 errhidestmt(true),
					 errdetail_execute(parsetree_list)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("QUERY STATISTICS");

	TRACE_POSTGRESQL_QUERY_DONE(query_string);

	debug_query_string = NULL;
}

/*
 * exec_parse_message
 *
 * Execute a "Parse" protocol message.
 *
 * 执行“解析”协议消息。
 */
static void
exec_parse_message(const char *query_string,	/* string to execute
												 *
												 * 要执行的字符串
												 */
				   const char *stmt_name,	/* name for prepared stmt
								 *
								 * 准备好的 stmt 的名称
								 */
				   Oid *paramTypes, /* parameter types
						 *
						 * 参数类型
						 */
				   int numParams)	/* number of parameters
						 *
						 * 参数数量
						 */
{
	MemoryContext unnamed_stmt_context = NULL;
	MemoryContext oldcontext;
	List	   *parsetree_list;
	RawStmt    *raw_parse_tree;
	List	   *querytree_list;
	CachedPlanSource *psrc;
	bool		is_named;
	bool		save_log_statement_stats = log_statement_stats;
	char		msec_str[32];

	/*
	 * Report query to various monitoring facilities.
	 *
	 * 向各监控设施报告查询。
	 */
	debug_query_string = query_string;

	pgstat_report_activity(STATE_RUNNING, query_string);

	set_ps_display("PARSE");

	if (save_log_statement_stats)
		ResetUsage();

	ereport(DEBUG2,
			(errmsg_internal("parse %s: %s",
							 *stmt_name ? stmt_name : "<unnamed>",
							 query_string)));

	/*
	 * Start up a transaction command so we can run parse analysis etc. (Note
	 * that this will normally change current memory context.) Nothing happens
	 * if we are already in one.  This also arms the statement timeout if
	 * necessary.
	 *
	 * 启动一个事务命令，以便我们可以运行解析分析等。（请注意，这通常会更改当前内存上下文。）如果我们已经处于其中，则什么也不会发生。如有必
	 * 要，这还会设置语句超时。
	 */
	start_xact_command();

	/*
	 * Switch to appropriate context for constructing parsetrees.
	 *
	 * 切换到适当的上下文来构造解析树。
	 *
	 * We have two strategies depending on whether the prepared statement is
	 * named or not.  For a named prepared statement, we do parsing in
	 * MessageContext and copy the finished trees into the prepared
	 * statement's plancache entry; then the reset of MessageContext releases
	 * temporary space used by parsing and rewriting. For an unnamed prepared
	 * statement, we assume the statement isn't going to hang around long, so
	 * getting rid of temp space quickly is probably not worth the costs of
	 * copying parse trees.  So in this case, we create the plancache entry's
	 * query_context here, and do all the parsing work therein.
	 *
	 * 根据准备好的语句是否命名，我们有两种策略。对于命名的准备语句，我们在 MessageContext 中进行解析，并将完成的树复制到准
	 * 备语句的计划缓存条目中；那么MessageContext的重置会释放解析和重写所使用的临时空间。对于未命名的准备好的语句，我们假设该
	 * 语句不会长时间停留，因此快速摆脱临时空间可能不值得复制解析树的成本。因此，在本例中，我们在这里创建 plancache 条目的
	 * query_context，并在其中完成所有解析工作。
	 */
	is_named = (stmt_name[0] != '\0');
	if (is_named)
	{
		/* Named prepared statement --- parse in MessageContext
		 *
		 * 命名准备语句---在MessageContext中解析
		 */
		oldcontext = MemoryContextSwitchTo(MessageContext);
	}
	else
	{
		/* Unnamed prepared statement --- release any prior unnamed stmt
		 *
		 * 未命名准备好的语句 --- 释放任何先前的未命名 stmt
		 */
		drop_unnamed_stmt();
		/* Create context for parsing
		 *
		 * 创建解析上下文
		 */
		unnamed_stmt_context =
			AllocSetContextCreate(MessageContext,
								  "unnamed prepared statement",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(unnamed_stmt_context);
	}

	/*
	 * Do basic parsing of the query or queries (this should be safe even if
	 * we are in aborted transaction state!)
	 *
	 * 对一个或多个查询进行基本解析（即使我们处于中止事务状态，这也应该是安全的！）
	 */
	parsetree_list = pg_parse_query(query_string);

	/*
	 * We only allow a single user statement in a prepared statement. This is
	 * mainly to keep the protocol simple --- otherwise we'd need to worry
	 * about multiple result tupdescs and things like that.
	 *
	 * 我们只允许在准备好的语句中使用单个用户语句。这主要是为了保持协议简单——否则我们需要担心多个结果 tupdesc 之类的事情。
	 */
	if (list_length(parsetree_list) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot insert multiple commands into a prepared statement")));

	if (parsetree_list != NIL)
	{
		bool		snapshot_set = false;

		raw_parse_tree = linitial_node(RawStmt, parsetree_list);

		/*
		 * If we are in an aborted transaction, reject all commands except
		 * COMMIT/ROLLBACK.  It is important that this test occur before we
		 * try to do parse analysis, rewrite, or planning, since all those
		 * phases try to do database accesses, which may fail in abort state.
		 * (It might be safe to allow some additional utility commands in this
		 * state, but not many...)
		 *
		 * 如果我们处于中止事务中，则拒绝除 COMMIT/ROLLBACK 之外的所有命令。重要的是，在我们尝试进行解析分析、重写或规划之前进
		 * 行此测试，因为所有这些阶段都尝试进行数据库访问，这可能会在中止状态下失败。
		 * （在这种状态下允许一些额外的实用程序命令可能是安全的，但不是很多......）
		 */
		if (IsAbortedTransactionBlockState() &&
			!IsTransactionExitStmt(raw_parse_tree->stmt))
			ereport(ERROR,
					(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
					 errmsg("current transaction is aborted, "
							"commands ignored until end of transaction block"),
					 errdetail_abort()));

		/*
		 * Create the CachedPlanSource before we do parse analysis, since it
		 * needs to see the unmodified raw parse tree.
		 *
		 * 在进行解析分析之前创建 CachedPlanSource，因为它需要查看未修改的原始解析树。
		 */
		psrc = CreateCachedPlan(raw_parse_tree, query_string,
								CreateCommandTag(raw_parse_tree->stmt));

		/*
		 * Set up a snapshot if parse analysis will need one.
		 *
		 * 如果解析分析需要快照，请设置快照。
		 */
		if (analyze_requires_snapshot(raw_parse_tree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * Analyze and rewrite the query.  Note that the originally specified
		 * parameter set is not required to be complete, so we have to use
		 * pg_analyze_and_rewrite_varparams().
		 *
		 * 分析并重写查询。注意，最初指定的参数集并不要求完整，所以我们必须使用pg_analyze_and_rewrite_varparams
		 * ()。
		 */
		querytree_list = pg_analyze_and_rewrite_varparams(raw_parse_tree,
														  query_string,
														  &paramTypes,
														  &numParams,
														  NULL);

		/* Done with the snapshot used for parsing
		 *
		 * 完成用于解析的快照
		 */
		if (snapshot_set)
			PopActiveSnapshot();
	}
	else
	{
		/* Empty input string.  This is legal.
		 *
		 * 空输入字符串。这是合法的。
		 */
		raw_parse_tree = NULL;
		psrc = CreateCachedPlan(raw_parse_tree, query_string,
								CMDTAG_UNKNOWN);
		querytree_list = NIL;
	}

	/*
	 * CachedPlanSource must be a direct child of MessageContext before we
	 * reparent unnamed_stmt_context under it, else we have a disconnected
	 * circular subgraph.  Klugy, but less so than flipping contexts even more
	 * above.
	 *
	 * CachedPlanSource 必须是 MessageContext 的直接子级，然后才能在其下重新设置
	 * unnamed_stmt_context 的父级，否则我们将得到一个断开连接的循环子图。 Klugy，但还不如将上下文翻转得更上面。
	 */
	if (unnamed_stmt_context)
		MemoryContextSetParent(psrc->context, MessageContext);

	/* Finish filling in the CachedPlanSource
	 *
	 * 完成CachedPlanSource的填写
	 */
	CompleteCachedPlan(psrc,
					   querytree_list,
					   unnamed_stmt_context,
					   paramTypes,
					   numParams,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* allow parallel mode
								 *
								 * 允许并行模式
								 */
					   true);	/* fixed result
				 *
				 * 固定结果
				 */

	/* If we got a cancel signal during analysis, quit
	 *
	 * 如果我们在分析过程中收到取消信号，请退出
	 */
	CHECK_FOR_INTERRUPTS();

	if (is_named)
	{
		/*
		 * Store the query as a prepared statement.
		 *
		 * 将查询存储为准备好的语句。
		 */
		StorePreparedStatement(stmt_name, psrc, false);
	}
	else
	{
		/*
		 * We just save the CachedPlanSource into unnamed_stmt_psrc.
		 *
		 * 我们只是将 CachedPlanSource 保存到 unnamed_stmt_psrc 中。
		 */
		SaveCachedPlan(psrc);
		unnamed_stmt_psrc = psrc;
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * We do NOT close the open transaction command here; that only happens
	 * when the client sends Sync.  Instead, do CommandCounterIncrement just
	 * in case something happened during parse/plan.
	 *
	 * 我们不在这里关闭打开的事务命令；仅当客户端发送 Sync 时才会发生这种情况。相反，执行
	 * CommandCounterIncrement 以防在解析/计划期间发生某些情况。
	 */
	CommandCounterIncrement();

	/*
	 * Send ParseComplete.
	 *
	 * 发送解析完成。
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage(PqMsg_ParseComplete);

	/*
	 * Emit duration logging if appropriate.
	 *
	 * 如果合适的话，发出持续时间日志记录。
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  parse %s: %s",
							msec_str,
							 *stmt_name ? stmt_name : "<unnamed>",
							query_string),
					 errhidestmt(true)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("PARSE MESSAGE STATISTICS");

	debug_query_string = NULL;
}

/*
 * exec_bind_message
 *
 * Process a "Bind" message to create a portal from a prepared statement
 *
 * 处理“绑定”消息以从准备好的语句创建门户
 */
static void
exec_bind_message(StringInfo input_message)
{
	const char *portal_name;
	const char *stmt_name;
	int			numPFormats;
	int16	   *pformats = NULL;
	int			numParams;
	int			numRFormats;
	int16	   *rformats = NULL;
	CachedPlanSource *psrc;
	CachedPlan *cplan;
	Portal		portal;
	char	   *query_string;
	char	   *saved_stmt_name;
	ParamListInfo params;
	MemoryContext oldContext;
	bool		save_log_statement_stats = log_statement_stats;
	bool		snapshot_set = false;
	char		msec_str[32];
	ParamsErrorCbData params_data;
	ErrorContextCallback params_errcxt;
	ListCell   *lc;

	/* Get the fixed part of the message
	 *
	 * 获取消息的固定部分
	 */
	portal_name = pq_getmsgstring(input_message);
	stmt_name = pq_getmsgstring(input_message);

	ereport(DEBUG2,
			(errmsg_internal("bind %s to %s",
							 *portal_name ? portal_name : "<unnamed>",
							 *stmt_name ? stmt_name : "<unnamed>")));

	/* Find prepared statement
	 *
	 * 查找准备好的语句
	 */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* special-case the unnamed statement
		 *
		 * 未命名语句的特例
		 */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/*
	 * Report query to various monitoring facilities.
	 *
	 * 向各监控设施报告查询。
	 */
	debug_query_string = psrc->query_string;

	pgstat_report_activity(STATE_RUNNING, psrc->query_string);

	foreach(lc, psrc->query_list)
	{
		Query	   *query = lfirst_node(Query, lc);

		if (query->queryId != INT64CONST(0))
		{
			pgstat_report_query_id(query->queryId, false);
			break;
		}
	}

	set_ps_display("BIND");

	if (save_log_statement_stats)
		ResetUsage();

	/*
	 * Start up a transaction command so we can call functions etc. (Note that
	 * this will normally change current memory context.) Nothing happens if
	 * we are already in one.  This also arms the statement timeout if
	 * necessary.
	 *
	 * 启动一个事务命令，以便我们可以调用函数等。（请注意，这通常会更改当前的内存上下文。）如果我们已经处于其中，则什么也不会发生。如有必要
	 * ，这还会设置语句超时。
	 */
	start_xact_command();

	/* Switch back to message context
	 *
	 * 切换回消息上下文
	 */
	MemoryContextSwitchTo(MessageContext);

	/* Get the parameter format codes
	 *
	 * 获取参数格式代码
	 */
	numPFormats = pq_getmsgint(input_message, 2);
	if (numPFormats > 0)
	{
		pformats = palloc_array(int16, numPFormats);
		for (int i = 0; i < numPFormats; i++)
			pformats[i] = pq_getmsgint(input_message, 2);
	}

	/* Get the parameter value count
	 *
	 * 获取参数值个数
	 */
	numParams = pq_getmsgint(input_message, 2);

	if (numPFormats > 1 && numPFormats != numParams)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message has %d parameter formats but %d parameters",
						numPFormats, numParams)));

	if (numParams != psrc->num_params)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("bind message supplies %d parameters, but prepared statement \"%s\" requires %d",
						numParams, stmt_name, psrc->num_params)));

	/*
	 * If we are in aborted transaction state, the only portals we can
	 * actually run are those containing COMMIT or ROLLBACK commands. We
	 * disallow binding anything else to avoid problems with infrastructure
	 * that expects to run inside a valid transaction.  We also disallow
	 * binding any parameters, since we can't risk calling user-defined I/O
	 * functions.
	 *
	 * 如果我们处于中止事务状态，那么我们实际可以运行的唯一门户是那些包含 COMMIT 或 ROLLBACK 命令的门户。我们不允许绑定任
	 * 何其他内容，以避免在有效事务中运行的基础设施出现问题。我们也不允许绑定任何参数，因为我们不能冒险调用用户定义的 I/O 函数。
	 */
	if (IsAbortedTransactionBlockState() &&
		(!(psrc->raw_parse_tree &&
		   IsTransactionExitStmt(psrc->raw_parse_tree->stmt)) ||
		 numParams != 0))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/*
	 * Create the portal.  Allow silent replacement of an existing portal only
	 * if the unnamed portal is specified.
	 *
	 * 创建门户。仅当指定未命名门户时才允许静默替换现有门户。
	 */
	if (portal_name[0] == '\0')
		portal = CreatePortal(portal_name, true, true);
	else
		portal = CreatePortal(portal_name, false, false);

	/*
	 * Prepare to copy stuff into the portal's memory context.  We do all this
	 * copying first, because it could possibly fail (out-of-memory) and we
	 * don't want a failure to occur between GetCachedPlan and
	 * PortalDefineQuery; that would result in leaking our plancache refcount.
	 *
	 * 准备将内容复制到门户的内存上下文中。我们首先执行所有这些复制，因为它可能会失败（内存不足），并且我们不希望在
	 * GetCachedPlan 和 PortalDefineQuery 之间发生故障；这将导致泄漏我们的计划缓存引用计数。
	 */
	oldContext = MemoryContextSwitchTo(portal->portalContext);

	/* Copy the plan's query string into the portal
	 *
	 * 将计划的查询字符串复制到门户中
	 */
	query_string = pstrdup(psrc->query_string);

	/* Likewise make a copy of the statement name, unless it's unnamed
	 *
	 * 同样复制语句名称，除非它是未命名的
	 */
	if (stmt_name[0])
		saved_stmt_name = pstrdup(stmt_name);
	else
		saved_stmt_name = NULL;

	/*
	 * Set a snapshot if we have parameters to fetch (since the input
	 * functions might need it) or the query isn't a utility command (and
	 * hence could require redoing parse analysis and planning).  We keep the
	 * snapshot active till we're done, so that plancache.c doesn't have to
	 * take new ones.
	 *
	 * 如果我们有要获取的参数（因为输入函数可能需要它）或者查询不是实用程序命令（因此可能需要重新进行解析分析和规划），请设置快照。我们保持
	 * 快照处于活动状态直到完成，这样 plancache.c 就不必获取新的快照。
	 */
	if (numParams > 0 ||
		(psrc->raw_parse_tree &&
		 analyze_requires_snapshot(psrc->raw_parse_tree)))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}

	/*
	 * Fetch parameters, if any, and store in the portal's memory context.
	 *
	 * 获取参数（如果有）并将其存储在门户的内存上下文中。
	 */
	if (numParams > 0)
	{
		char	  **knownTextValues = NULL; /* allocate on first use
									 *
									 * 首次使用时分配
									 */
		BindParamCbData one_param_data;

		/*
		 * Set up an error callback so that if there's an error in this phase,
		 * we can report the specific parameter causing the problem.
		 *
		 * 设置错误回调，以便如果此阶段出现错误，我们可以报告导致问题的特定参数。
		 */
		one_param_data.portalName = portal->name;
		one_param_data.paramno = -1;
		one_param_data.paramval = NULL;
		params_errcxt.previous = error_context_stack;
		params_errcxt.callback = bind_param_error_callback;
		params_errcxt.arg = &one_param_data;
		error_context_stack = &params_errcxt;

		params = makeParamList(numParams);

		for (int paramno = 0; paramno < numParams; paramno++)
		{
			Oid			ptype = psrc->param_types[paramno];
			int32		plength;
			Datum		pval;
			bool		isNull;
			StringInfoData pbuf;
			char		csave;
			int16		pformat;

			one_param_data.paramno = paramno;
			one_param_data.paramval = NULL;

			plength = pq_getmsgint(input_message, 4);
			isNull = (plength == -1);

			if (!isNull)
			{
				char	   *pvalue;

				/*
				 * Rather than copying data around, we just initialize a
				 * StringInfo pointing to the correct portion of the message
				 * buffer.  We assume we can scribble on the message buffer to
				 * add a trailing NUL which is required for the input function
				 * call.
				 *
				 * 我们不复制数据，而是初始化一个指向消息缓冲区正确部分的 StringInfo
				 * 。我们假设我们可以在消息缓冲区上乱写以添加输入函数调用所需的尾随 NUL。
				 */
				pvalue = unconstify(char *, pq_getmsgbytes(input_message, plength));
				csave = pvalue[plength];
				pvalue[plength] = '\0';
				initReadOnlyStringInfo(&pbuf, pvalue, plength);
			}
			else
			{
				pbuf.data = NULL;	/* keep compiler quiet
						 *
						 * 让编译器保持安静
						 */
				csave = 0;
			}

			if (numPFormats > 1)
				pformat = pformats[paramno];
			else if (numPFormats > 0)
				pformat = pformats[0];
			else
				pformat = 0;	/* default = text
				 *
				 * 默认 = 文本
				 */

			if (pformat == 0)	/* text mode
						 *
						 * 文本模式
						 */
			{
				Oid			typinput;
				Oid			typioparam;
				char	   *pstring;

				getTypeInputInfo(ptype, &typinput, &typioparam);

				/*
				 * We have to do encoding conversion before calling the
				 * typinput routine.
				 *
				 * 在调用typinput例程之前我们必须进行编码转换。
				 */
				if (isNull)
					pstring = NULL;
				else
					pstring = pg_client_to_server(pbuf.data, plength);

				/* Now we can log the input string in case of error
				 *
				 * 现在我们可以记录输入字符串以防出现错误
				 */
				one_param_data.paramval = pstring;

				pval = OidInputFunctionCall(typinput, pstring, typioparam, -1);

				one_param_data.paramval = NULL;

				/*
				 * If we might need to log parameters later, save a copy of
				 * the converted string in MessageContext; then free the
				 * result of encoding conversion, if any was done.
				 *
				 * 如果我们稍后可能需要记录参数，请将转换后的字符串保存在 MessageContext 中；然后释放编码转换的结果（如果已完成）。
				 */
				if (pstring)
				{
					if (log_parameter_max_length_on_error != 0)
					{
						MemoryContext oldcxt;

						oldcxt = MemoryContextSwitchTo(MessageContext);

						if (knownTextValues == NULL)
							knownTextValues = palloc0_array(char *, numParams);

						if (log_parameter_max_length_on_error < 0)
							knownTextValues[paramno] = pstrdup(pstring);
						else
						{
							/*
							 * We can trim the saved string, knowing that we
							 * won't print all of it.  But we must copy at
							 * least two more full characters than
							 * BuildParamLogString wants to use; otherwise it
							 * might fail to include the trailing ellipsis.
							 *
							 * 我们可以修剪保存的字符串，因为我们知道我们不会打印所有字符串。但我们必须复制比 BuildParamLogString
							 * 想要使用的至少两个完整字符；否则它可能无法包含尾随省略号。
							 */
							knownTextValues[paramno] =
								pnstrdup(pstring,
										 log_parameter_max_length_on_error
										 + 2 * MAX_MULTIBYTE_CHAR_LEN);
						}

						MemoryContextSwitchTo(oldcxt);
					}
					if (pstring != pbuf.data)
						pfree(pstring);
				}
			}
			else if (pformat == 1)	/* binary mode
							 *
							 * 二进制模式
							 */
			{
				Oid			typreceive;
				Oid			typioparam;
				StringInfo	bufptr;

				/*
				 * Call the parameter type's binary input converter
				 *
				 * 调用参数类型的二进制输入转换器
				 */
				getTypeBinaryInputInfo(ptype, &typreceive, &typioparam);

				if (isNull)
					bufptr = NULL;
				else
					bufptr = &pbuf;

				pval = OidReceiveFunctionCall(typreceive, bufptr, typioparam, -1);

				/* Trouble if it didn't eat the whole buffer
				 *
				 * 如果它没有吃掉整个缓冲区就会有麻烦
				 */
				if (!isNull && pbuf.cursor != pbuf.len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in bind parameter %d",
									paramno + 1)));
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unsupported format code: %d",
								pformat)));
				pval = 0;		/* keep compiler quiet
				 *
				 * 让编译器保持安静
				 */
			}

			/* Restore message buffer contents
			 *
			 * 恢复消息缓冲区内容
			 */
			if (!isNull)
				pbuf.data[plength] = csave;

			params->params[paramno].value = pval;
			params->params[paramno].isnull = isNull;

			/*
			 * We mark the params as CONST.  This ensures that any custom plan
			 * makes full use of the parameter values.
			 *
			 * 我们将参数标记为 CONST。这可确保任何自定义计划都能充分利用参数值。
			 */
			params->params[paramno].pflags = PARAM_FLAG_CONST;
			params->params[paramno].ptype = ptype;
		}

		/* Pop the per-parameter error callback
		 *
		 * 弹出每个参数的错误回调
		 */
		error_context_stack = error_context_stack->previous;

		/*
		 * Once all parameters have been received, prepare for printing them
		 * in future errors, if configured to do so.  (This is saved in the
		 * portal, so that they'll appear when the query is executed later.)
		 *
		 * 收到所有参数后，准备在将来的错误中打印它们（如果已配置）。 （这保存在门户中，以便稍后执行查询时它们会出现。）
		 */
		if (log_parameter_max_length_on_error != 0)
			params->paramValuesStr =
				BuildParamLogString(params,
									knownTextValues,
									log_parameter_max_length_on_error);
	}
	else
		params = NULL;

	/* Done storing stuff in portal's context
	 *
	 * 已完成在门户上下文中存储内容
	 */
	MemoryContextSwitchTo(oldContext);

	/*
	 * Set up another error callback so that all the parameters are logged if
	 * we get an error during the rest of the BIND processing.
	 *
	 * 设置另一个错误回调，以便在 BIND 处理的其余部分出现错误时记录所有参数。
	 */
	params_data.portalName = portal->name;
	params_data.params = params;
	params_errcxt.previous = error_context_stack;
	params_errcxt.callback = ParamsErrorCallback;
	params_errcxt.arg = &params_data;
	error_context_stack = &params_errcxt;

	/* Get the result format codes
	 *
	 * 获取结果格式代码
	 */
	numRFormats = pq_getmsgint(input_message, 2);
	if (numRFormats > 0)
	{
		rformats = palloc_array(int16, numRFormats);
		for (int i = 0; i < numRFormats; i++)
			rformats[i] = pq_getmsgint(input_message, 2);
	}

	pq_getmsgend(input_message);

	/*
	 * Obtain a plan from the CachedPlanSource.  Any cruft from (re)planning
	 * will be generated in MessageContext.  The plan refcount will be
	 * assigned to the Portal, so it will be released at portal destruction.
	 *
	 * 从CachedPlanSource 获取计划。 （重新）规划产生的任何缺陷都将在 MessageContext
	 * 中生成。计划引用计数将分配给门户，因此它将在门户销毁时释放。
	 */
	cplan = GetCachedPlan(psrc, params, NULL, NULL);

	/*
	 * Now we can define the portal.
	 *
	 * 现在我们可以定义门户了。
	 *
	 * DO NOT put any code that could possibly throw an error between the
	 * above GetCachedPlan call and here.
	 *
	 * 不要在上面的 GetCachedPlan 调用和此处之间放置任何可能引发错误的代码。
	 */
	PortalDefineQuery(portal,
					  saved_stmt_name,
					  query_string,
					  psrc->commandTag,
					  cplan->stmt_list,
					  cplan);

	/* Portal is defined, set the plan ID based on its contents.
	 *
	 * Portal已定义，根据其内容设置计划ID。
	 */
	foreach(lc, portal->stmts)
	{
		PlannedStmt *plan = lfirst_node(PlannedStmt, lc);

		if (plan->planId != INT64CONST(0))
		{
			pgstat_report_plan_id(plan->planId, false);
			break;
		}
	}

	/* Done with the snapshot used for parameter I/O and parsing/planning
	 *
	 * 完成用于参数 I/O 和解析/规划的快照
	 */
	if (snapshot_set)
		PopActiveSnapshot();

	/*
	 * And we're ready to start portal execution.
	 *
	 * 我们已准备好开始门户执行。
	 */
	PortalStart(portal, params, 0, InvalidSnapshot);

	/*
	 * Apply the result format requests to the portal.
	 *
	 * 将结果格式请求应用到门户。
	 */
	PortalSetResultFormat(portal, numRFormats, rformats);

	/*
	 * Done binding; remove the parameters error callback.  Entries emitted
	 * later determine independently whether to log the parameters or not.
	 *
	 * 绑定完成；删除参数错误回调。稍后发出的条目独立确定是否记录参数。
	 */
	error_context_stack = error_context_stack->previous;

	/*
	 * Send BindComplete.
	 *
	 * 发送 BindComplete。
	 */
	if (whereToSendOutput == DestRemote)
		pq_putemptymessage(PqMsg_BindComplete);

	/*
	 * Emit duration logging if appropriate.
	 *
	 * 如果合适的话，发出持续时间日志记录。
	 */
	switch (check_log_duration(msec_str, false))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  bind %s%s%s: %s",
							msec_str,
							 *stmt_name ? stmt_name : "<unnamed>",
							 *portal_name ? "/" : "",
							 *portal_name ? portal_name : "",
							psrc->query_string),
					 errhidestmt(true),
					 errdetail_params(params)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("BIND MESSAGE STATISTICS");

	valgrind_report_error_query(debug_query_string);

	debug_query_string = NULL;
}

/*
 * exec_execute_message
 *
 * Process an "Execute" message for a portal
 *
 * 处理门户的“执行”消息
 */
static void
exec_execute_message(const char *portal_name, long max_rows)
{
	CommandDest dest;
	DestReceiver *receiver;
	Portal		portal;
	bool		completed;
	QueryCompletion qc;
	const char *sourceText;
	const char *prepStmtName;
	ParamListInfo portalParams;
	bool		save_log_statement_stats = log_statement_stats;
	bool		is_xact_command;
	bool		execute_is_fetch;
	bool		was_logged = false;
	char		msec_str[32];
	ParamsErrorCbData params_data;
	ErrorContextCallback params_errcxt;
	const char *cmdtagname;
	size_t		cmdtaglen;
	ListCell   *lc;

	/* Adjust destination to tell printtup.c what to do
	 *
	 * 调整目标以告诉 printtup.c 做什么
	 */
	dest = whereToSendOutput;
	if (dest == DestRemote)
		dest = DestRemoteExecute;

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * If the original query was a null string, just return
	 * EmptyQueryResponse.
	 *
	 * 如果原始查询是空字符串，则仅返回 EmptyQueryResponse。
	 */
	if (portal->commandTag == CMDTAG_UNKNOWN)
	{
		Assert(portal->stmts == NIL);
		NullCommand(dest);
		return;
	}

	/* Does the portal contain a transaction command?
	 *
	 * 门户是否包含交易命令？
	 */
	is_xact_command = IsTransactionStmtList(portal->stmts);

	/*
	 * We must copy the sourceText and prepStmtName into MessageContext in
	 * case the portal is destroyed during finish_xact_command.  We do not
	 * make a copy of the portalParams though, preferring to just not print
	 * them in that case.
	 *
	 * 我们必须将 sourceText 和 prepStmtName 复制到 MessageContext 中，以防门户在
	 * finish_xact_command 期间被破坏。不过，我们不会复制 PortalParams，在这种情况下宁愿不打印它们。
	 */
	sourceText = pstrdup(portal->sourceText);
	if (portal->prepStmtName)
		prepStmtName = pstrdup(portal->prepStmtName);
	else
		prepStmtName = "<unnamed>";
	portalParams = portal->portalParams;

	/*
	 * Report query to various monitoring facilities.
	 *
	 * 向各监控设施报告查询。
	 */
	debug_query_string = sourceText;

	pgstat_report_activity(STATE_RUNNING, sourceText);

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->queryId != INT64CONST(0))
		{
			pgstat_report_query_id(stmt->queryId, false);
			break;
		}
	}

	foreach(lc, portal->stmts)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);

		if (stmt->planId != INT64CONST(0))
		{
			pgstat_report_plan_id(stmt->planId, false);
			break;
		}
	}

	cmdtagname = GetCommandTagNameAndLen(portal->commandTag, &cmdtaglen);

	set_ps_display_with_len(cmdtagname, cmdtaglen);

	if (save_log_statement_stats)
		ResetUsage();

	BeginCommand(portal->commandTag, dest);

	/*
	 * Create dest receiver in MessageContext (we don't want it in transaction
	 * context, because that may get deleted if portal contains VACUUM).
	 *
	 * 在 MessageContext 中创建目标接收者（我们不希望它出现在事务上下文中，因为如果门户包含
	 * VACUUM，它可能会被删除）。
	 */
	receiver = CreateDestReceiver(dest);
	if (dest == DestRemoteExecute)
		SetRemoteDestReceiverParams(receiver, portal);

	/*
	 * Ensure we are in a transaction command (this should normally be the
	 * case already due to prior BIND).
	 *
	 * 确保我们处于事务命令中（由于之前的 BIND，通常应该是这种情况）。
	 */
	start_xact_command();

	/*
	 * If we re-issue an Execute protocol request against an existing portal,
	 * then we are only fetching more rows rather than completely re-executing
	 * the query from the start. atStart is never reset for a v3 portal, so we
	 * are safe to use this check.
	 *
	 * 如果我们针对现有门户重新发出执行协议请求，那么我们只是获取更多行，而不是从头开始完全重新执行查询。 v3 门户的 atStart
	 * 永远不会重置，因此我们可以安全地使用此检查。
	 */
	execute_is_fetch = !portal->atStart;

	/* Log immediately if dictated by log_statement
	 *
	 * 如果 log_statement 指定则立即记录
	 */
	if (check_log_statement(portal->stmts))
	{
		ereport(LOG,
				(errmsg("%s %s%s%s: %s",
						execute_is_fetch ?
						_("execute fetch from") :
						_("execute"),
						prepStmtName,
						 *portal_name ? "/" : "",
						 *portal_name ? portal_name : "",
						sourceText),
				 errhidestmt(true),
				 errdetail_params(portalParams)));
		was_logged = true;
	}

	/*
	 * If we are in aborted transaction state, the only portals we can
	 * actually run are those containing COMMIT or ROLLBACK commands.
	 *
	 * 如果我们处于中止事务状态，那么我们实际可以运行的唯一门户是那些包含 COMMIT 或 ROLLBACK 命令的门户。
	 */
	if (IsAbortedTransactionBlockState() &&
		!IsTransactionExitStmtList(portal->stmts))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	/* Check for cancel signal before we start execution
	 *
	 * 在开始执行之前检查取消信号
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Okay to run the portal.  Set the error callback so that parameters are
	 * logged.  The parameters must have been saved during the bind phase.
	 *
	 * 可以运行门户了。设置错误回调以便记录参数。参数必须在绑定阶段保存。
	 */
	params_data.portalName = portal->name;
	params_data.params = portalParams;
	params_errcxt.previous = error_context_stack;
	params_errcxt.callback = ParamsErrorCallback;
	params_errcxt.arg = &params_data;
	error_context_stack = &params_errcxt;

	if (max_rows <= 0)
		max_rows = FETCH_ALL;

	completed = PortalRun(portal,
						  max_rows,
						  true, /* always top level
				 *
				 * 始终处于最高水平
				 */
						  receiver,
						  receiver,
						  &qc);

	receiver->rDestroy(receiver);

	/* Done executing; remove the params error callback
	 *
	 * 执行完毕；删除params错误回调
	 */
	error_context_stack = error_context_stack->previous;

	if (completed)
	{
		if (is_xact_command || (MyXactFlags & XACT_FLAGS_NEEDIMMEDIATECOMMIT))
		{
			/*
			 * If this was a transaction control statement, commit it.  We
			 * will start a new xact command for the next command (if any).
			 * Likewise if the statement required immediate commit.  Without
			 * this provision, we wouldn't force commit until Sync is
			 * received, which creates a hazard if the client tries to
			 * pipeline immediate-commit statements.
			 *
			 * 如果这是事务控制语句，则提交它。我们将为下一个命令（如果有）启动一个新的 xact
			 * 命令。同样，如果该语句需要立即提交。如果没有这个规定，我们不会强制提交，直到收到同步，如果客户端尝试管道立即提交语句，这会产生危险。
			 */
			finish_xact_command();

			/*
			 * These commands typically don't have any parameters, and even if
			 * one did we couldn't print them now because the storage went
			 * away during finish_xact_command.  So pretend there were none.
			 *
			 * 这些命令通常没有任何参数，即使有，我们现在也无法打印它们，因为存储在 finish_xact_command
			 * 期间消失了。所以假装没有。
			 */
			portalParams = NULL;
		}
		else
		{
			/*
			 * We need a CommandCounterIncrement after every query, except
			 * those that start or end a transaction block.
			 *
			 * 每次查询后我们都需要一个 CommandCounterIncrement，除了那些开始或结束事务块的查询。
			 */
			CommandCounterIncrement();

			/*
			 * Set XACT_FLAGS_PIPELINING whenever we complete an Execute
			 * message without immediately committing the transaction.
			 *
			 * 每当我们完成执行消息而不立即提交事务时，设置 XACT_FLAGS_PIPELINING。
			 */
			MyXactFlags |= XACT_FLAGS_PIPELINING;

			/*
			 * Disable statement timeout whenever we complete an Execute
			 * message.  The next protocol message will start a fresh timeout.
			 *
			 * 每当我们完成执行消息时禁用语句超时。下一条协议消息将开始新的超时。
			 */
			disable_statement_timeout();
		}

		/* Send appropriate CommandComplete to client
		 *
		 * 发送适当的 CommandComplete 给客户端
		 */
		EndCommand(&qc, dest, false);
	}
	else
	{
		/* Portal run not complete, so send PortalSuspended
		 *
		 * Portal 运行未完成，因此发送 PortalSuspending
		 */
		if (whereToSendOutput == DestRemote)
			pq_putemptymessage(PqMsg_PortalSuspended);

		/*
		 * Set XACT_FLAGS_PIPELINING whenever we suspend an Execute message,
		 * too.
		 *
		 * 每当我们挂起执行消息时也设置 XACT_FLAGS_PIPELINING。
		 */
		MyXactFlags |= XACT_FLAGS_PIPELINING;
	}

	/*
	 * Emit duration logging if appropriate.
	 *
	 * 如果合适的话，发出持续时间日志记录。
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str),
					 errhidestmt(true)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  %s %s%s%s: %s",
							msec_str,
							execute_is_fetch ?
							_("execute fetch from") :
							_("execute"),
							prepStmtName,
							 *portal_name ? "/" : "",
							 *portal_name ? portal_name : "",
							sourceText),
					 errhidestmt(true),
					 errdetail_params(portalParams)));
			break;
	}

	if (save_log_statement_stats)
		ShowUsage("EXECUTE MESSAGE STATISTICS");

	valgrind_report_error_query(debug_query_string);

	debug_query_string = NULL;
}

/*
 * check_log_statement
 *		Determine whether command should be logged because of log_statement
 *
 * check_log_statement 确定是否应因 log_statement 而记录命令
 *
 * stmt_list can be either raw grammar output or a list of planned
 * statements
 *
 * stmt_list 可以是原始语法输出或计划语句列表
 */
static bool
check_log_statement(List *stmt_list)
{
	ListCell   *stmt_item;

	if (log_statement == LOGSTMT_NONE)
		return false;
	if (log_statement == LOGSTMT_ALL)
		return true;

	/* Else we have to inspect the statement(s) to see whether to log
	 *
	 * 否则我们必须检查语句以查看是否记录
	 */
	foreach(stmt_item, stmt_list)
	{
		Node	   *stmt = (Node *) lfirst(stmt_item);

		if (GetCommandLogLevel(stmt) <= log_statement)
			return true;
	}

	return false;
}

/*
 * check_log_duration
 *		Determine whether current command's duration should be logged
 *		We also check if this statement in this transaction must be logged
 *		(regardless of its duration).
 *
 * check_log_duration 确定是否应记录当前命令的持续时间
 * 我们还检查是否必须记录此事务中的此语句（无论其持续时间是多少）。
 *
 * Returns:
 *		0 if no logging is needed
 *		1 if just the duration should be logged
 *		2 if duration and query details should be logged
 *
 * 返回： 0 如果不需要记录 1 如果只需要记录持续时间 2 如果需要记录持续时间和查询详细信息
 *
 * If logging is needed, the duration in msec is formatted into msec_str[],
 * which must be a 32-byte buffer.
 *
 * 如果需要记录，则以毫秒为单位的持续时间将被格式化为 msec_str[]，它必须是 32 字节的缓冲区。
 * 如果调用者已经记录了查询详细信息，
 *
 * was_logged should be true if caller already logged query details (this
 * essentially prevents 2 from being returned).
 *
 * was_logged 应该为 true（这基本上可以防止返回 2）。
 */
int
check_log_duration(char *msec_str, bool was_logged)
{
	if (log_duration || log_min_duration_sample >= 0 ||
		log_min_duration_statement >= 0 || xact_is_sampled)
	{
		long		secs;
		int			usecs;
		int			msecs;
		bool		exceeded_duration;
		bool		exceeded_sample_duration;
		bool		in_sample = false;

		TimestampDifference(GetCurrentStatementStartTimestamp(),
							GetCurrentTimestamp(),
							&secs, &usecs);
		msecs = usecs / 1000;

		/*
		 * This odd-looking test for log_min_duration_* being exceeded is
		 * designed to avoid integer overflow with very long durations: don't
		 * compute secs * 1000 until we've verified it will fit in int.
		 *
		 * 这个看起来很奇怪的 log_min_duration_* 超出测试旨在避免持续时间很长的整数溢出：在我们验证它适合 int
		 * 之前不要计算 secs * 1000。
		 */
		exceeded_duration = (log_min_duration_statement == 0 ||
							 (log_min_duration_statement > 0 &&
							  (secs > log_min_duration_statement / 1000 ||
							   secs * 1000 + msecs >= log_min_duration_statement)));

		exceeded_sample_duration = (log_min_duration_sample == 0 ||
									(log_min_duration_sample > 0 &&
									 (secs > log_min_duration_sample / 1000 ||
									  secs * 1000 + msecs >= log_min_duration_sample)));

		/*
		 * Do not log if log_statement_sample_rate = 0. Log a sample if
		 * log_statement_sample_rate <= 1 and avoid unnecessary PRNG call if
		 * log_statement_sample_rate = 1.
		 *
		 * 如果 log_statement_sample_rate = 0，则不记录。如果 log_statement_sample_rate
		 * ＜= 1，则记录样本；如果 log_statement_sample_rate = 1，则避免不必要的 PRNG 调用。
		 */
		if (exceeded_sample_duration)
			in_sample = log_statement_sample_rate != 0 &&
				(log_statement_sample_rate == 1 ||
				 pg_prng_double(&pg_global_prng_state) <= log_statement_sample_rate);

		if (exceeded_duration || in_sample || log_duration || xact_is_sampled)
		{
			snprintf(msec_str, 32, "%ld.%03d",
					 secs * 1000 + msecs, usecs % 1000);
			if ((exceeded_duration || in_sample || xact_is_sampled) && !was_logged)
				return 2;
			else
				return 1;
		}
	}

	return 0;
}

/*
 * errdetail_execute
 *
 * Add an errdetail() line showing the query referenced by an EXECUTE, if any.
 * The argument is the raw parsetree list.
 *
 * 添加 errdetail() 行，显示 EXECUTE 引用的查询（如果有）。参数是原始解析树列表。
 */
static int
errdetail_execute(List *raw_parsetree_list)
{
	ListCell   *parsetree_item;

	foreach(parsetree_item, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, parsetree_item);

		if (IsA(parsetree->stmt, ExecuteStmt))
		{
			ExecuteStmt *stmt = (ExecuteStmt *) parsetree->stmt;
			PreparedStatement *pstmt;

			pstmt = FetchPreparedStatement(stmt->name, false);
			if (pstmt)
			{
				errdetail("prepare: %s", pstmt->plansource->query_string);
				return 0;
			}
		}
	}

	return 0;
}

/*
 * errdetail_params
 *
 * Add an errdetail() line showing bind-parameter data, if available.
 * Note that this is only used for statement logging, so it is controlled
 * by log_parameter_max_length not log_parameter_max_length_on_error.
 *
 * 添加显示绑定参数数据的 errdetail() 行（如果可用）。请注意，这仅用于语句日志记录，因此它由
 * log_parameter_max_length 而不是 log_parameter_max_length_on_error 控制。
 */
static int
errdetail_params(ParamListInfo params)
{
	if (params && params->numParams > 0 && log_parameter_max_length != 0)
	{
		char	   *str;

		str = BuildParamLogString(params, NULL, log_parameter_max_length);
		if (str && str[0] != '\0')
			errdetail("Parameters: %s", str);
	}

	return 0;
}

/*
 * errdetail_abort
 *
 * Add an errdetail() line showing abort reason, if any.
 *
 * 添加 errdetail() 行，显示中止原因（如果有）。
 */
static int
errdetail_abort(void)
{
	if (MyProc->recoveryConflictPending)
		errdetail("Abort reason: recovery conflict");

	return 0;
}

/*
 * errdetail_recovery_conflict
 *
 * Add an errdetail() line showing conflict source.
 *
 * 添加显示冲突源的 errdetail() 行。
 */
static int
errdetail_recovery_conflict(ProcSignalReason reason)
{
	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:
			errdetail("User was holding shared buffer pin for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOCK:
			errdetail("User was holding a relation lock for too long.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
			errdetail("User was or might have been using tablespace that must be dropped.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:
			errdetail("User query might have needed to see row versions that must be removed.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:
			errdetail("User was using a logical replication slot that must be invalidated.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:
			errdetail("User transaction caused buffer deadlock with recovery.");
			break;
		case PROCSIG_RECOVERY_CONFLICT_DATABASE:
			errdetail("User was connected to a database that must be dropped.");
			break;
		default:
			break;
			/* no errdetail
			 *
			 * 没有错误细节
			 */
	}

	return 0;
}

/*
 * bind_param_error_callback
 *
 * Error context callback used while parsing parameters in a Bind message
 *
 * 解析 Bind 消息中的参数时使用错误上下文回调
 */
static void
bind_param_error_callback(void *arg)
{
	BindParamCbData *data = (BindParamCbData *) arg;
	StringInfoData buf;
	char	   *quotedval;

	if (data->paramno < 0)
		return;

	/* If we have a textual value, quote it, and trim if necessary
	 *
	 * 如果我们有文本值，请引用它，并在必要时进行修剪
	 */
	if (data->paramval)
	{
		initStringInfo(&buf);
		appendStringInfoStringQuoted(&buf, data->paramval,
									 log_parameter_max_length_on_error);
		quotedval = buf.data;
	}
	else
		quotedval = NULL;

	if (data->portalName && data->portalName[0] != '\0')
	{
		if (quotedval)
			errcontext("portal \"%s\" parameter $%d = %s",
					   data->portalName, data->paramno + 1, quotedval);
		else
			errcontext("portal \"%s\" parameter $%d",
					   data->portalName, data->paramno + 1);
	}
	else
	{
		if (quotedval)
			errcontext("unnamed portal parameter $%d = %s",
					   data->paramno + 1, quotedval);
		else
			errcontext("unnamed portal parameter $%d",
					   data->paramno + 1);
	}

	if (quotedval)
		pfree(quotedval);
}

/*
 * exec_describe_statement_message
 *
 * Process a "Describe" message for a prepared statement
 *
 * 处理准备好的语句的“描述”消息
 */
static void
exec_describe_statement_message(const char *stmt_name)
{
	CachedPlanSource *psrc;

	/*
	 * Start up a transaction command. (Note that this will normally change
	 * current memory context.) Nothing happens if we are already in one.
	 *
	 * 启动事务命令。 （请注意，这通常会改变当前的内存上下文。）如果我们已经处于其中，则什么也不会发生。
	 */
	start_xact_command();

	/* Switch back to message context
	 *
	 * 切换回消息上下文
	 */
	MemoryContextSwitchTo(MessageContext);

	/* Find prepared statement
	 *
	 * 查找准备好的语句
	 */
	if (stmt_name[0] != '\0')
	{
		PreparedStatement *pstmt;

		pstmt = FetchPreparedStatement(stmt_name, true);
		psrc = pstmt->plansource;
	}
	else
	{
		/* special-case the unnamed statement
		 *
		 * 未命名语句的特例
		 */
		psrc = unnamed_stmt_psrc;
		if (!psrc)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
					 errmsg("unnamed prepared statement does not exist")));
	}

	/* Prepared statements shouldn't have changeable result descs
	 *
	 * 准备好的语句不应具有可更改的结果描述
	 */
	Assert(psrc->fixed_result);

	/*
	 * If we are in aborted transaction state, we can't run
	 * SendRowDescriptionMessage(), because that needs catalog accesses.
	 * Hence, refuse to Describe statements that return data.  (We shouldn't
	 * just refuse all Describes, since that might break the ability of some
	 * clients to issue COMMIT or ROLLBACK commands, if they use code that
	 * blindly Describes whatever it does.)  We can Describe parameters
	 * without doing anything dangerous, so we don't restrict that.
	 *
	 * 如果我们处于中止事务状态，则无法运行
	 * SendRowDescriptionMessage()，因为这需要目录访问。因此，拒绝描述返回数据的语句。
	 * （我们不应该拒绝所有描述，因为如果某些客户端使用盲目描述其功能的代码，这可能会破坏某些客户端发出 COMMIT 或 ROLLBACK
	 * 命令的能力。）我们可以描述参数而不做任何危险的事情，因此我们不限制这一点。
	 */
	if (IsAbortedTransactionBlockState() &&
		psrc->resultDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* can't actually do anything...
				 *
				 * 实际上什么也做不了...
				 */

	/*
	 * First describe the parameters...
	 *
	 * 首先描述一下参数...
	 */
	pq_beginmessage_reuse(&row_description_buf, PqMsg_ParameterDescription);
	pq_sendint16(&row_description_buf, psrc->num_params);

	for (int i = 0; i < psrc->num_params; i++)
	{
		Oid			ptype = psrc->param_types[i];

		pq_sendint32(&row_description_buf, (int) ptype);
	}
	pq_endmessage_reuse(&row_description_buf);

	/*
	 * Next send RowDescription or NoData to describe the result...
	 *
	 * 接下来发送 RowDescription 或 NoData 来描述结果...
	 */
	if (psrc->resultDesc)
	{
		List	   *tlist;

		/* Get the plan's primary targetlist
		 *
		 * 获取计划的主要目标列表
		 */
		tlist = CachedPlanGetTargetList(psrc, NULL);

		SendRowDescriptionMessage(&row_description_buf,
								  psrc->resultDesc,
								  tlist,
								  NULL);
	}
	else
		pq_putemptymessage(PqMsg_NoData);
}

/*
 * exec_describe_portal_message
 *
 * Process a "Describe" message for a portal
 *
 * 处理门户的“描述”消息
 */
static void
exec_describe_portal_message(const char *portal_name)
{
	Portal		portal;

	/*
	 * Start up a transaction command. (Note that this will normally change
	 * current memory context.) Nothing happens if we are already in one.
	 *
	 * 启动事务命令。 （请注意，这通常会改变当前的内存上下文。）如果我们已经处于其中，则什么也不会发生。
	 */
	start_xact_command();

	/* Switch back to message context
	 *
	 * 切换回消息上下文
	 */
	MemoryContextSwitchTo(MessageContext);

	portal = GetPortalByName(portal_name);
	if (!PortalIsValid(portal))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("portal \"%s\" does not exist", portal_name)));

	/*
	 * If we are in aborted transaction state, we can't run
	 * SendRowDescriptionMessage(), because that needs catalog accesses.
	 * Hence, refuse to Describe portals that return data.  (We shouldn't just
	 * refuse all Describes, since that might break the ability of some
	 * clients to issue COMMIT or ROLLBACK commands, if they use code that
	 * blindly Describes whatever it does.)
	 *
	 * 如果我们处于中止事务状态，则无法运行
	 * SendRowDescriptionMessage()，因为这需要目录访问。因此，拒绝描述返回数据的门户。
	 * （我们不应该拒绝所有描述，因为如果某些客户端使用盲目描述其所做的任何代码，这可能会破坏某些客户端发出 COMMIT 或
	 * ROLLBACK 命令的能力。）
	 */
	if (IsAbortedTransactionBlockState() &&
		portal->tupDesc)
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block"),
				 errdetail_abort()));

	if (whereToSendOutput != DestRemote)
		return;					/* can't actually do anything...
				 *
				 * 实际上什么也做不了...
				 */

	if (portal->tupDesc)
		SendRowDescriptionMessage(&row_description_buf,
								  portal->tupDesc,
								  FetchPortalTargetList(portal),
								  portal->formats);
	else
		pq_putemptymessage(PqMsg_NoData);
}


/*
 * Convenience routines for starting/committing a single command.
 *
 * 用于启动/提交单个命令的便捷例程。
 */
static void
start_xact_command(void)
{
	/*
	 * Ensure a transaction command is active for message processing, update
	 * pipelined execution state when needed, and arm relevant timeouts.
	 *
	 * 确保消息处理期间有活动的事务命令，在需要时更新流水线执行状态，并启用相关超时。
	 */
	if (!xact_started)
	{
		StartTransactionCommand();

		xact_started = true;
	}
	else if (MyXactFlags & XACT_FLAGS_PIPELINING)
	{
		/*
		 * When the first Execute message is completed, following commands
		 * will be done in an implicit transaction block created via
		 * pipelining. The transaction state needs to be updated to an
		 * implicit block if we're not already in a transaction block (like
		 * one started by an explicit BEGIN).
		 *
		 * 当第一个执行消息完成时，以下命令将在通过管道创建的隐式事务块中完成。如果我们尚未处于事务块中（例如由显式 BEGIN
		 * 启动的事务块），则事务状态需要更新为隐式块。
		 */
		BeginImplicitTransactionBlock();
	}

	/*
	 * Start statement timeout if necessary.  Note that this'll intentionally
	 * not reset the clock on an already started timeout, to avoid the timing
	 * overhead when start_xact_command() is invoked repeatedly, without an
	 * interceding finish_xact_command() (e.g. parse/bind/execute).  If that's
	 * not desired, the timeout has to be disabled explicitly.
	 *
	 * 如果需要，启动语句超时。请注意，这将故意不在已启动的超时上重置时钟，以避免重复调用 start_xact_command()
	 * 时的计时开销，而无需中间的 finish_xact_command() （例如解析/绑定/执行）。如果不需要，则必须显式禁用超时。
	 */
	enable_statement_timeout();

	/* Start timeout for checking if the client has gone away if necessary.
	 *
	 * 如有必要，启动超时以检查客户端是否已离开。
	 */
	if (client_connection_check_interval > 0 &&
		IsUnderPostmaster &&
		MyProcPort &&
		!get_timeout_active(CLIENT_CONNECTION_CHECK_TIMEOUT))
		enable_timeout_after(CLIENT_CONNECTION_CHECK_TIMEOUT,
							 client_connection_check_interval);
}

static void
finish_xact_command(void)
{
	/*
	 * Finish the active transaction command by disabling statement timeout,
	 * committing pending work, and clearing local transaction state.
	 *
	 * 通过禁用语句超时、提交待处理工作并清除本地事务状态来结束活动的事务命令。
	 */
	/* cancel active statement timeout after each command
	 *
	 * 在每个命令之后取消活动语句超时
	 */
	disable_statement_timeout();

	if (xact_started)
	{
		CommitTransactionCommand();

#ifdef MEMORY_CONTEXT_CHECKING
		/* Check all memory contexts that weren't freed during commit
		 *
		 * 检查提交期间未释放的所有内存上下文
		 */
		/* (those that were, were checked before being deleted)
		 *
		 * （那些在删除之前经过检查的）
		 */
		MemoryContextCheck(TopMemoryContext);
#endif

#ifdef SHOW_MEMORY_STATS
		/* Print mem stats after each commit for leak tracking
		 *
		 * 每次提交后打印内存统计信息以进行泄漏跟踪
		 */
		MemoryContextStats(TopMemoryContext);
#endif

		xact_started = false;
	}
}


/*
 * Convenience routines for checking whether a statement is one of the
 * ones that we allow in transaction-aborted state.
 *
 * 用于检查语句是否是我们允许处于事务中止状态的语句之一的便捷例程。
 */

/* Test a bare parsetree
 *
 * 测试一个裸解析树
 */
static bool
IsTransactionExitStmt(Node *parsetree)
{
	if (parsetree && IsA(parsetree, TransactionStmt))
	{
		TransactionStmt *stmt = (TransactionStmt *) parsetree;

		if (stmt->kind == TRANS_STMT_COMMIT ||
			stmt->kind == TRANS_STMT_PREPARE ||
			stmt->kind == TRANS_STMT_ROLLBACK ||
			stmt->kind == TRANS_STMT_ROLLBACK_TO)
			return true;
	}
	return false;
}

/* Test a list that contains PlannedStmt nodes
 *
 * 测试包含 PlannedStmt 节点的列表
 */
static bool
IsTransactionExitStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsTransactionExitStmt(pstmt->utilityStmt))
			return true;
	}
	return false;
}

/* Test a list that contains PlannedStmt nodes
 *
 * 测试包含 PlannedStmt 节点的列表
 */
static bool
IsTransactionStmtList(List *pstmts)
{
	if (list_length(pstmts) == 1)
	{
		PlannedStmt *pstmt = linitial_node(PlannedStmt, pstmts);

		if (pstmt->commandType == CMD_UTILITY &&
			IsA(pstmt->utilityStmt, TransactionStmt))
			return true;
	}
	return false;
}

/* Release any existing unnamed prepared statement
 *
 * 释放任何现有的未命名准备好的语句
 */
static void
drop_unnamed_stmt(void)
{
	/* paranoia to avoid a dangling pointer in case of error
	 *
	 * 偏执以避免出现错误时出现悬空指针
	 */
	if (unnamed_stmt_psrc)
	{
		CachedPlanSource *psrc = unnamed_stmt_psrc;

		unnamed_stmt_psrc = NULL;
		DropCachedPlan(psrc);
	}
}


/* --------------------------------
 *		signal handler routines used in PostgresMain()
 *
 * PostgresMain() 中使用的信号处理程序例程
 * --------------------------------
 */

/*
 * quickdie() occurs when signaled SIGQUIT by the postmaster.
 *
 * 当邮局管理员发出 SIGQUIT 信号时，quickdie() 就会发生。
 *
 * Either some backend has bought the farm, or we've been told to shut down
 * "immediately"; so we need to stop what we're doing and exit.
 *
 * 要么某个后端已经购买了农场，要么我们被告知“立即”关闭；所以我们需要停止正在做的事情并退出。
 */
void
quickdie(SIGNAL_ARGS)
{
	sigaddset(&BlockSig, SIGQUIT);	/* prevent nested calls
								 *
								 * 防止嵌套调用
								 */
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * Prevent interrupts while exiting; though we just blocked signals that
	 * would queue new interrupts, one may have been pending.  We don't want a
	 * quickdie() downgraded to a mere query cancel.
	 *
	 * 防止退出时中断；尽管我们只是阻止了对新中断进行排队的信号，但其中一个信号可能一直处于待处理状态。我们不希望将 Quickdie()
	 * 降级为单纯的查询取消。
	 */
	HOLD_INTERRUPTS();

	/*
	 * If we're aborting out of client auth, don't risk trying to send
	 * anything to the client; we will likely violate the protocol, not to
	 * mention that we may have interrupted the guts of OpenSSL or some
	 * authentication library.
	 *
	 * 如果我们要中止客户端身份验证，请不要冒险尝试向客户端发送任何内容；我们可能会违反协议，更不用说我们可能会中断 OpenSSL
	 * 或某些身份验证库的内部结构。
	 */
	if (ClientAuthInProgress && whereToSendOutput == DestRemote)
		whereToSendOutput = DestNone;

	/*
	 * Notify the client before exiting, to give a clue on what happened.
	 *
	 * 在退出之前通知客户，以提供发生了什么情况的线索。
	 *
	 * It's dubious to call ereport() from a signal handler.  It is certainly
	 * not async-signal safe.  But it seems better to try, than to disconnect
	 * abruptly and leave the client wondering what happened.  It's remotely
	 * possible that we crash or hang while trying to send the message, but
	 * receiving a SIGQUIT is a sign that something has already gone badly
	 * wrong, so there's not much to lose.  Assuming the postmaster is still
	 * running, it will SIGKILL us soon if we get stuck for some reason.
	 *
	 * 从信号处理程序中调用 ereport() 是可疑的。它当然不是异步信号安全的。但尝试一下似乎比突然断开连接并让客户想知道发生了什么要
	 * 好。在尝试发送消息时，我们很可能会崩溃或挂起，但收到 SIGQUIT
	 * 表明某些事情已经发生了严重错误，因此不会有太大损失。假设邮政管理员仍在运行，如果我们由于某种原因被卡住，它很快就会发出信号杀死我们。
	 *
	 * One thing we can do to make this a tad safer is to clear the error
	 * context stack, so that context callbacks are not called.  That's a lot
	 * less code that could be reached here, and the context info is unlikely
	 * to be very relevant to a SIGQUIT report anyway.
	 *
	 * 为了让这更安全，我们可以做的一件事是清除错误上下文堆栈，这样就不会调用上下文回调。这里可以到达的代码要少得多，并且无论如何，上下文信
	 * 息不太可能与 SIGQUIT 报告非常相关。
	 */
	error_context_stack = NULL;

	/*
	 * When responding to a postmaster-issued signal, we send the message only
	 * to the client; sending to the server log just creates log spam, plus
	 * it's more code that we need to hope will work in a signal handler.
	 *
	 * 当响应邮政局长发出的信号时，我们仅将消息发送给客户端；发送到服务器日志只会创建日志垃圾邮件，再加上我们需要希望在信号处理程序中工作的
	 * 更多代码。
	 *
	 * Ideally these should be ereport(FATAL), but then we'd not get control
	 * back to force the correct type of process exit.
	 *
	 * 理想情况下，这些应该是 ereport(FATAL)，但是这样我们就无法收回控制权来强制进程退出正确的类型。
	 */
	switch (GetQuitSignalReason())
	{
		case PMQUIT_NOT_SENT:
			/* Hmm, SIGQUIT arrived out of the blue
			 *
			 * 嗯，SIGQUIT 突然到来
			 */
			ereport(WARNING,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection because of unexpected SIGQUIT signal")));
			break;
		case PMQUIT_FOR_CRASH:
			/* A crash-and-restart cycle is in progress
			 *
			 * 崩溃和重启周期正在进行中
			 */
			ereport(WARNING_CLIENT_ONLY,
					(errcode(ERRCODE_CRASH_SHUTDOWN),
					 errmsg("terminating connection because of crash of another server process"),
					 errdetail("The postmaster has commanded this server process to roll back"
							   " the current transaction and exit, because another"
							   " server process exited abnormally and possibly corrupted"
							   " shared memory."),
					 errhint("In a moment you should be able to reconnect to the"
							 " database and repeat your command.")));
			break;
		case PMQUIT_FOR_STOP:
			/* Immediate-mode stop
			 *
			 * 立即模式停止
			 */
			ereport(WARNING_CLIENT_ONLY,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to immediate shutdown command")));
			break;
	}

	/*
	 * We DO NOT want to run proc_exit() or atexit() callbacks -- we're here
	 * because shared memory may be corrupted, so we don't want to try to
	 * clean up our transaction.  Just nail the windows shut and get out of
	 * town.  The callbacks wouldn't be safe to run from a signal handler,
	 * anyway.
	 *
	 * 我们不想运行 proc_exit() 或 atexit() 回调——我们在这里是因为共享内存可能已损坏，所以我们不想尝试清理我们的事
	 * 务。只要把窗户钉上，然后出城就可以了。无论如何，从信号处理程序运行回调是不安全的。
	 *
	 * Note we do _exit(2) not _exit(0).  This is to force the postmaster into
	 * a system reset cycle if someone sends a manual SIGQUIT to a random
	 * backend.  This is necessary precisely because we don't clean up our
	 * shared memory state.  (The "dead man switch" mechanism in pmsignal.c
	 * should ensure the postmaster sees this as a crash, too, but no harm in
	 * being doubly sure.)
	 *
	 * 注意我们执行 _exit(2) 而不是 _exit(0)。这是为了在有人向随机后端发送手动 SIGQUIT
	 * 时强制邮局管理员进入系统重置周期。这正是必要的，因为我们不清理共享内存状态。 （pmsignal.c 中的“dead man
	 * switch”机制应该确保邮局管理员也将其视为崩溃，但双重确定也没有什么坏处。）
	 */
	_exit(2);
}

/*
 * Shutdown signal from postmaster: abort transaction and exit
 * at soonest convenient time
 *
 * 来自邮政局长的关闭信号：中止交易并在最快方便的时间退出
 */
void
die(SIGNAL_ARGS)
{
	/* Don't joggle the elbow of proc_exit
	 *
	 * 不要摇动 proc_exit 的肘部
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		ProcDiePending = true;
	}

	/* for the cumulative stats system
	 *
	 * 用于累积统计系统
	 */
	pgStatSessionEndCause = DISCONNECT_KILLED;

	/* If we're still here, waken anything waiting on the process latch
	 *
	 * 如果我们还在这里，唤醒进程闩锁上等待的所有内容
	 */
	SetLatch(MyLatch);

	/*
	 * If we're in single user mode, we want to quit immediately - we can't
	 * rely on latches as they wouldn't work when stdin/stdout is a file.
	 * Rather ugly, but it's unlikely to be worthwhile to invest much more
	 * effort just for the benefit of single user mode.
	 *
	 * 如果我们处于单用户模式，我们希望立即退出 - 我们不能依赖锁存器，因为当 stdin/stdout
	 * 是文件时它们不起作用。相当丑陋，但不太值得仅仅为了单用户模式的好处而投入更多精力。
	 */
	if (DoingCommandRead && whereToSendOutput != DestRemote)
		ProcessInterrupts();
}

/*
 * Query-cancel signal from postmaster: abort current transaction
 * at soonest convenient time
 *
 * 来自邮政局长的查询取消信号：在方便的时候尽快中止当前事务
 */
void
StatementCancelHandler(SIGNAL_ARGS)
{
	/*
	 * Don't joggle the elbow of proc_exit
	 *
	 * 不要摇动 proc_exit 的肘部
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}

	/* If we're still here, waken anything waiting on the process latch
	 *
	 * 如果我们还在这里，唤醒进程闩锁上等待的所有内容
	 */
	SetLatch(MyLatch);
}

/* signal handler for floating point exception
 *
 * 浮点异常信号处理程序
 */
void
FloatExceptionHandler(SIGNAL_ARGS)
{
	/* We're not returning, so no need to save errno
	 *
	 * 我们不会返回，因此无需保存 errno
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FLOATING_POINT_EXCEPTION),
			 errmsg("floating-point exception"),
			 errdetail("An invalid floating-point operation was signaled. "
					   "This probably means an out-of-range result or an "
					   "invalid operation, such as division by zero.")));
}

/*
 * Tell the next CHECK_FOR_INTERRUPTS() to check for a particular type of
 * recovery conflict.  Runs in a SIGUSR1 handler.
 *
 * 告诉下一个 CHECK_FOR_INTERRUPTS() 检查特定类型的恢复冲突。在 SIGUSR1 处理程序中运行。
 */
void
HandleRecoveryConflictInterrupt(ProcSignalReason reason)
{
	RecoveryConflictPendingReasons[reason] = true;
	RecoveryConflictPending = true;
	InterruptPending = true;
	/* latch will be set by procsignal_sigusr1_handler
	 *
	 * 锁存器将由procsignal_sigusr1_handler设置
	 */
}

/*
 * Check one individual conflict reason.
 *
 * 检查一项单独的冲突原因。
 */
static void
ProcessRecoveryConflictInterrupt(ProcSignalReason reason)
{
	switch (reason)
	{
		case PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK:

			/*
			 * If we aren't waiting for a lock we can never deadlock.
			 *
			 * 如果我们不等待锁，我们永远不会死锁。
			 */
			if (GetAwaitedLock() == NULL)
				return;

			/* Intentional fall through to check wait for pin
			 *
			 * 故意失败以检查等待引脚
			 */
			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_BUFFERPIN:

			/*
			 * If PROCSIG_RECOVERY_CONFLICT_BUFFERPIN is requested but we
			 * aren't blocking the Startup process there is nothing more to
			 * do.
			 *
			 * 如果请求 PROCSIG_RECOVERY_CONFLICT_BUFFERPIN 但我们没有阻止启动过程，则无需执行任何操作。
			 *
			 * When PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK is requested,
			 * if we're waiting for locks and the startup process is not
			 * waiting for buffer pin (i.e., also waiting for locks), we set
			 * the flag so that ProcSleep() will check for deadlocks.
			 *
			 * 当请求 PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK
			 * 时，如果我们正在等待锁并且启动进程没有等待缓冲区引脚（即也在等待锁），我们设置标志以便 ProcSleep() 将检查死锁。
			 */
			if (!HoldingBufferPinThatDelaysRecovery())
			{
				if (reason == PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK &&
					GetStartupBufferPinWaitBufId() < 0)
					CheckDeadLockAlert();
				return;
			}

			MyProc->recoveryConflictPending = true;

			/* Intentional fall through to error handling
			 *
			 * 故意陷入错误处理
			 */
			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_LOCK:
		case PROCSIG_RECOVERY_CONFLICT_TABLESPACE:
		case PROCSIG_RECOVERY_CONFLICT_SNAPSHOT:

			/*
			 * If we aren't in a transaction any longer then ignore.
			 *
			 * 如果我们不再处于事务中，则忽略。
			 */
			if (!IsTransactionOrTransactionBlock())
				return;

			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT:

			/*
			 * If we're not in a subtransaction then we are OK to throw an
			 * ERROR to resolve the conflict.  Otherwise drop through to the
			 * FATAL case.
			 *
			 * 如果我们不在子事务中，那么我们可以抛出一个错误来解决冲突。否则直接进入致命案例。
			 *
			 * PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT is a special case that
			 * always throws an ERROR (ie never promotes to FATAL), though it
			 * still has to respect QueryCancelHoldoffCount, so it shares this
			 * code path.  Logical decoding slots are only acquired while
			 * performing logical decoding.  During logical decoding no user
			 * controlled code is run.  During [sub]transaction abort, the
			 * slot is released.  Therefore user controlled code cannot
			 * intercept an error before the replication slot is released.
			 *
			 * PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT 是一种特殊情况，总是抛出错误（即永远不会升级为
			 * FATAL），尽管它仍然必须遵守 QueryCancelHoldoffCount，因此它共享此代码路径。逻辑解码时隙仅在执行逻辑解码
			 * 时获取。在逻辑解码期间，不运行用户控制的代码。在[子]事务中止期间，槽被释放。因此，在释放复制槽之前，用户控制的代码无法拦截错误。
			 *
			 * XXX other times that we can throw just an ERROR *may* be
			 * PROCSIG_RECOVERY_CONFLICT_LOCK if no locks are held in parent
			 * transactions
			 *
			 * XXX 其他时候，如果父事务中没有持有锁，我们可以抛出一个错误 *可能* 是
			 * PROCSIG_RECOVERY_CONFLICT_LOCK
			 *
			 * PROCSIG_RECOVERY_CONFLICT_SNAPSHOT if no snapshots are held by
			 * parent transactions and the transaction is not
			 * transaction-snapshot mode
			 *
			 * PROCSIG_RECOVERY_CONFLICT_SNAPSHOT 如果父事务没有保存任何快照并且事务不是事务快照模式
			 *
			 * PROCSIG_RECOVERY_CONFLICT_TABLESPACE if no temp files or
			 * cursors open in parent transactions
			 *
			 * PROCSIG_RECOVERY_CONFLICT_TABLESPACE 如果父事务中没有打开临时文件或游标
			 */
			if (reason == PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT ||
				!IsSubTransaction())
			{
				/*
				 * If we already aborted then we no longer need to cancel.  We
				 * do this here since we do not wish to ignore aborted
				 * subtransactions, which must cause FATAL, currently.
				 *
				 * 如果我们已经中止，那么我们不再需要取消。我们在这里这样做是因为我们不希望忽略中止的子事务，目前这肯定会导致致命错误。
				 */
				if (IsAbortedTransactionBlockState())
					return;

				/*
				 * If a recovery conflict happens while we are waiting for
				 * input from the client, the client is presumably just
				 * sitting idle in a transaction, preventing recovery from
				 * making progress.  We'll drop through to the FATAL case
				 * below to dislodge it, in that case.
				 *
				 * 如果在我们等待客户端输入时发生恢复冲突，则客户端可能只是在事务中闲置，从而阻止恢复取得进展。在这种情况下，我们将通过下面的致命案例来
				 * 消除它。
				 */
				if (!DoingCommandRead)
				{
					/* Avoid losing sync in the FE/BE protocol.
					 *
					 * 避免在 FE/BE 协议中丢失同步。
					 */
					if (QueryCancelHoldoffCount != 0)
					{
						/*
						 * Re-arm and defer this interrupt until later.  See
						 * similar code in ProcessInterrupts().
						 *
						 * 重新准备并推迟此中断直到稍后。请参阅 ProcessInterrupts() 中的类似代码。
						 */
						RecoveryConflictPendingReasons[reason] = true;
						RecoveryConflictPending = true;
						InterruptPending = true;
						return;
					}

					/*
					 * We are cleared to throw an ERROR.  Either it's the
					 * logical slot case, or we have a top-level transaction
					 * that we can abort and a conflict that isn't inherently
					 * non-retryable.
					 *
					 * 我们可以抛出错误。要么是逻辑槽情况，要么我们有一个可以中止的顶级事务以及本质上不可重试的冲突。
					 */
					LockErrorCleanup();
					pgstat_report_recovery_conflict(reason);
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("canceling statement due to conflict with recovery"),
							 errdetail_recovery_conflict(reason)));
					break;
				}
			}

			/* Intentional fall through to session cancel
			 *
			 * 故意失败导致会话取消
			 */
			/* FALLTHROUGH */

		case PROCSIG_RECOVERY_CONFLICT_DATABASE:

			/*
			 * Retrying is not possible because the database is dropped, or we
			 * decided above that we couldn't resolve the conflict with an
			 * ERROR and fell through.  Terminate the session.
			 *
			 * 无法重试，因为数据库已被删除，或者我们在上面决定无法解决与错误的冲突并失败。终止会话。
			 */
			pgstat_report_recovery_conflict(reason);
			ereport(FATAL,
					(errcode(reason == PROCSIG_RECOVERY_CONFLICT_DATABASE ?
							 ERRCODE_DATABASE_DROPPED :
							 ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("terminating connection due to conflict with recovery"),
					 errdetail_recovery_conflict(reason),
					 errhint("In a moment you should be able to reconnect to the"
							 " database and repeat your command.")));
			break;

		default:
			elog(FATAL, "unrecognized conflict mode: %d", (int) reason);
	}
}

/*
 * Check each possible recovery conflict reason.
 *
 * 检查每个可能的恢复冲突原因。
 */
static void
ProcessRecoveryConflictInterrupts(void)
{
	/*
	 * We don't need to worry about joggling the elbow of proc_exit, because
	 * proc_exit_prepare() holds interrupts, so ProcessInterrupts() won't call
	 * us.
	 *
	 * 我们不需要担心 proc_exit 的肘部动作，因为 proc_exit_prepare() 保留中断，所以
	 * ProcessInterrupts() 不会调用我们。
	 */
	Assert(!proc_exit_inprogress);
	Assert(InterruptHoldoffCount == 0);
	Assert(RecoveryConflictPending);

	RecoveryConflictPending = false;

	for (ProcSignalReason reason = PROCSIG_RECOVERY_CONFLICT_FIRST;
		 reason <= PROCSIG_RECOVERY_CONFLICT_LAST;
		 reason++)
	{
		if (RecoveryConflictPendingReasons[reason])
		{
			RecoveryConflictPendingReasons[reason] = false;
			ProcessRecoveryConflictInterrupt(reason);
		}
	}
}

/*
 * ProcessInterrupts: out-of-line portion of CHECK_FOR_INTERRUPTS() macro
 *
 * ProcessInterrupts：CHECK_FOR_INTERRUPTS() 宏的外线部分
 *
 * If an interrupt condition is pending, and it's safe to service it,
 * then clear the flag and accept the interrupt.  Called only when
 * InterruptPending is true.
 *
 * 如果中断条件待处理，并且可以安全地为其提供服务，则清除标志并接受中断。仅当 InterruptPending 为 true 时调用。
 *
 * Note: if INTERRUPTS_CAN_BE_PROCESSED() is true, then ProcessInterrupts
 * is guaranteed to clear the InterruptPending flag before returning.
 * (This is not the same as guaranteeing that it's still clear when we
 * return; another interrupt could have arrived.  But we promise that
 * any pre-existing one will have been serviced.)
 *
 * 注意：如果 INTERRUPTS_CAN_BE_PROCESSED() 为 true，则 ProcessInterrupts
 * 保证在返回之前清除 InterruptPending 标志。
 * （这与保证我们返回时仍然清晰不同；另一个中断可能已经到来。但我们保证任何先前存在的中断都将得到服务。）
 */
void
ProcessInterrupts(void)
{
	/* OK to accept any interrupts now?
	 *
	 * 现在可以接受任何中断吗？
	 */
	if (InterruptHoldoffCount != 0 || CritSectionCount != 0)
		return;
	InterruptPending = false;

	if (ProcDiePending)
	{
		ProcDiePending = false;
		QueryCancelPending = false; /* ProcDie trumps QueryCancel
								 *
								 * ProcDie 胜过 QueryCancel
								 */
		LockErrorCleanup();
		/* As in quickdie, don't risk sending to client during auth
		 *
		 * 与 Quickdie 一样，不要冒险在身份验证期间发送给客户端
		 */
		if (ClientAuthInProgress && whereToSendOutput == DestRemote)
			whereToSendOutput = DestNone;
		if (ClientAuthInProgress)
			ereport(FATAL,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling authentication due to timeout")));
		else if (AmAutoVacuumWorkerProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating autovacuum process due to administrator command")));
		else if (IsLogicalWorker())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating logical replication worker due to administrator command")));
		else if (IsLogicalLauncher())
		{
			ereport(DEBUG1,
					(errmsg_internal("logical replication launcher shutting down")));

			/*
			 * The logical replication launcher can be stopped at any time.
			 * Use exit status 1 so the background worker is restarted.
			 *
			 * 逻辑复制启动器可以随时停止。使用退出状态 1 以便重新启动后台工作程序。
			 */
			proc_exit(1);
		}
		else if (AmWalReceiverProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating walreceiver process due to administrator command")));
		else if (AmBackgroundWorkerProcess())
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating background worker \"%s\" due to administrator command",
							MyBgworkerEntry->bgw_type)));
		else if (AmIoWorkerProcess())
		{
			ereport(DEBUG1,
					(errmsg_internal("io worker shutting down due to administrator command")));

			proc_exit(0);
		}
		else
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("terminating connection due to administrator command")));
	}

	if (CheckClientConnectionPending)
	{
		CheckClientConnectionPending = false;

		/*
		 * Check for lost connection and re-arm, if still configured, but not
		 * if we've arrived back at DoingCommandRead state.  We don't want to
		 * wake up idle sessions, and they already know how to detect lost
		 * connections.
		 *
		 * 检查是否丢失连接并重新启动（如果仍已配置），但如果我们已返回 DoingCommandRead
		 * 状态，则不重新启动。我们不想唤醒空闲会话，并且它们已经知道如何检测丢失的连接。
		 */
		if (!DoingCommandRead && client_connection_check_interval > 0)
		{
			if (!pq_check_connection())
				ClientConnectionLost = true;
			else
				enable_timeout_after(CLIENT_CONNECTION_CHECK_TIMEOUT,
									 client_connection_check_interval);
		}
	}

	if (ClientConnectionLost)
	{
		QueryCancelPending = false; /* lost connection trumps QueryCancel
								 *
								 * 失去连接胜过 QueryCancel
								 */
		LockErrorCleanup();
		/* don't send to client, we already know the connection to be dead.
		 *
		 * 不要发送给客户端，我们已经知道连接已断开。
		 */
		whereToSendOutput = DestNone;
		ereport(FATAL,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("connection to client lost")));
	}

	/*
	 * Don't allow query cancel interrupts while reading input from the
	 * client, because we might lose sync in the FE/BE protocol.  (Die
	 * interrupts are OK, because we won't read any further messages from the
	 * client in that case.)
	 *
	 * 从客户端读取输入时不允许查询取消中断，因为我们可能会在 FE/BE 协议中丢失同步。
	 * （模具中断是可以的，因为在这种情况下我们不会从客户端读取任何进一步的消息。）
	 *
	 * See similar logic in ProcessRecoveryConflictInterrupts().
	 *
	 * 请参阅 ProcessRecoveryConflictInterrupts() 中的类似逻辑。
	 */
	if (QueryCancelPending && QueryCancelHoldoffCount != 0)
	{
		/*
		 * Re-arm InterruptPending so that we process the cancel request as
		 * soon as we're done reading the message.  (XXX this is seriously
		 * ugly: it complicates INTERRUPTS_CAN_BE_PROCESSED(), and it means we
		 * can't use that macro directly as the initial test in this function,
		 * meaning that this code also creates opportunities for other bugs to
		 * appear.)
		 *
		 * 重新启动 InterruptPending，以便我们在阅读完消息后立即处理取消请求。 （XXX 这非常难看：它使
		 * INTERRUPTS_CAN_BE_PROCESSED()
		 * 变得复杂，这意味着我们不能直接使用该宏作为该函数中的初始测试，这意味着该代码还为其他错误的出现创造了机会。）
		 */
		InterruptPending = true;
	}
	else if (QueryCancelPending)
	{
		bool		lock_timeout_occurred;
		bool		stmt_timeout_occurred;

		QueryCancelPending = false;

		/*
		 * If LOCK_TIMEOUT and STATEMENT_TIMEOUT indicators are both set, we
		 * need to clear both, so always fetch both.
		 *
		 * 如果 LOCK_TIMEOUT 和 STATEMENT_TIMEOUT 指示器都被设置，我们需要清除它们，所以总是获取它们。
		 */
		lock_timeout_occurred = get_timeout_indicator(LOCK_TIMEOUT, true);
		stmt_timeout_occurred = get_timeout_indicator(STATEMENT_TIMEOUT, true);

		/*
		 * If both were set, we want to report whichever timeout completed
		 * earlier; this ensures consistent behavior if the machine is slow
		 * enough that the second timeout triggers before we get here.  A tie
		 * is arbitrarily broken in favor of reporting a lock timeout.
		 *
		 * 如果两者都设置了，我们要报告较早完成的超时；如果机器速度足够慢以至于在我们到达这里之前触发第二次超时，这可以确保一致的行为。任意打破
		 * 平局有利于报告锁定超时。
		 */
		if (lock_timeout_occurred && stmt_timeout_occurred &&
			get_timeout_finish_time(STATEMENT_TIMEOUT) < get_timeout_finish_time(LOCK_TIMEOUT))
			lock_timeout_occurred = false;	/* report stmt timeout
									 *
									 * 报告stmt超时
									 */

		if (lock_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("canceling statement due to lock timeout")));
		}
		if (stmt_timeout_occurred)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to statement timeout")));
		}
		if (AmAutoVacuumWorkerProcess())
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling autovacuum task")));
		}

		/*
		 * If we are reading a command from the client, just ignore the cancel
		 * request --- sending an extra error message won't accomplish
		 * anything.  Otherwise, go ahead and throw the error.
		 *
		 * 如果我们正在从客户端读取命令，只需忽略取消请求即可——发送额外的错误消息不会完成任何操作。否则，继续并抛出错误。
		 */
		if (!DoingCommandRead)
		{
			LockErrorCleanup();
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("canceling statement due to user request")));
		}
	}

	if (RecoveryConflictPending)
		ProcessRecoveryConflictInterrupts();

	if (IdleInTransactionSessionTimeoutPending)
	{
		/*
		 * If the GUC has been reset to zero, ignore the signal.  This is
		 * important because the GUC update itself won't disable any pending
		 * interrupt.  We need to unset the flag before the injection point,
		 * otherwise we could loop in interrupts checking.
		 *
		 * 如果GUC 已重置为零，则忽略该信号。这很重要，因为 GUC
		 * 更新本身不会禁用任何挂起的中断。我们需要在注入点之前取消设置标志，否则我们可能会循环中断检查。
		 */
		IdleInTransactionSessionTimeoutPending = false;
		if (IdleInTransactionSessionTimeout > 0)
		{
			INJECTION_POINT("idle-in-transaction-session-timeout", NULL);
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_IN_TRANSACTION_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-in-transaction timeout")));
		}
	}

	if (TransactionTimeoutPending)
	{
		/* As above, ignore the signal if the GUC has been reset to zero.
		 *
		 * 如上所述，如果GUC 已重置为零，请忽略该信号。
		 */
		TransactionTimeoutPending = false;
		if (TransactionTimeout > 0)
		{
			INJECTION_POINT("transaction-timeout", NULL);
			ereport(FATAL,
					(errcode(ERRCODE_TRANSACTION_TIMEOUT),
					 errmsg("terminating connection due to transaction timeout")));
		}
	}

	if (IdleSessionTimeoutPending)
	{
		/* As above, ignore the signal if the GUC has been reset to zero.
		 *
		 * 如上所述，如果GUC 已重置为零，请忽略该信号。
		 */
		IdleSessionTimeoutPending = false;
		if (IdleSessionTimeout > 0)
		{
			INJECTION_POINT("idle-session-timeout", NULL);
			ereport(FATAL,
					(errcode(ERRCODE_IDLE_SESSION_TIMEOUT),
					 errmsg("terminating connection due to idle-session timeout")));
		}
	}

	/*
	 * If there are pending stats updates and we currently are truly idle
	 * (matching the conditions in PostgresMain(), report stats now.
	 *
	 * 如果有待处理的统计信息更新，并且我们当前确实处于空闲状态（符合 PostgresMain() 中的条件，请立即报告统计信息。
	 */
	if (IdleStatsUpdateTimeoutPending &&
		DoingCommandRead && !IsTransactionOrTransactionBlock())
	{
		IdleStatsUpdateTimeoutPending = false;
		pgstat_report_stat(true);
	}

	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ParallelMessagePending)
		ProcessParallelMessages();

	if (LogMemoryContextPending)
		ProcessLogMemoryContextInterrupt();

	if (ParallelApplyMessagePending)
		ProcessParallelApplyMessages();
}

/*
 * GUC check_hook for client_connection_check_interval
 *
 * client_connection_check_interval 的 GUC check_hook
 */
bool
check_client_connection_check_interval(int *newval, void **extra, GucSource source)
{
	if (!WaitEventSetCanReportClosed() && *newval != 0)
	{
		GUC_check_errdetail("\"client_connection_check_interval\" must be set to 0 on this platform.");
		return false;
	}
	return true;
}

/*
 * GUC check_hook for log_parser_stats, log_planner_stats, log_executor_stats
 *
 * log_parser_stats、log_planner_stats、log_executor_stats 的 GUC
 * check_hook
 *
 * This function and check_log_stats interact to prevent their variables from
 * being set in a disallowed combination.  This is a hack that doesn't really
 * work right; for example it might fail while applying pg_db_role_setting
 * values even though the final state would have been acceptable.  However,
 * since these variables are legacy settings with little production usage,
 * we tolerate that.
 *
 * 该函数和 check_log_stats 交互以防止将它们的变量设置为不允许的组合。这是一个实际上并不能正常工作的
 * hack；例如，即使最终状态是可以接受的，应用 pg_db_role_setting
 * 值时也可能会失败。然而，由于这些变量是遗留设置，生产使用很少，所以我们容忍这种情况。
 */
bool
check_stage_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval && log_statement_stats)
	{
		GUC_check_errdetail("Cannot enable parameter when \"log_statement_stats\" is true.");
		return false;
	}
	return true;
}

/*
 * GUC check_hook for log_statement_stats
 *
 * log_statement_stats 的 GUC check_hook
 */
bool
check_log_stats(bool *newval, void **extra, GucSource source)
{
	if (*newval &&
		(log_parser_stats || log_planner_stats || log_executor_stats))
	{
		GUC_check_errdetail("Cannot enable \"log_statement_stats\" when "
							"\"log_parser_stats\", \"log_planner_stats\", "
							"or \"log_executor_stats\" is true.");
		return false;
	}
	return true;
}

/* GUC assign hook for transaction_timeout
 *
 * GUC 为 transaction_timeout 分配钩子
 */
void
assign_transaction_timeout(int newval, void *extra)
{
	if (IsTransactionState())
	{
		/*
		 * If transaction_timeout GUC has changed within the transaction block
		 * enable or disable the timer correspondingly.
		 *
		 * 如果 transaction_timeout GUC 在事务块内发生更改，则相应地启用或禁用计时器。
		 */
		if (newval > 0 && !get_timeout_active(TRANSACTION_TIMEOUT))
			enable_timeout_after(TRANSACTION_TIMEOUT, newval);
		else if (newval <= 0 && get_timeout_active(TRANSACTION_TIMEOUT))
			disable_timeout(TRANSACTION_TIMEOUT, false);
	}
}

/*
 * GUC check_hook for restrict_nonsystem_relation_kind
 *
 * 用于restrict_nonsystem_relation_kind的GUC check_hook
 */
bool
check_restrict_nonsystem_relation_kind(char **newval, void **extra, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			flags = 0;

	/* Need a modifiable copy of string
	 *
	 * 需要字符串的可修改副本
	 */
	rawstring = pstrdup(*newval);

	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list
		 *
		 * 列表中有语法错误
		 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		if (pg_strcasecmp(tok, "view") == 0)
			flags |= RESTRICT_RELKIND_VIEW;
		else if (pg_strcasecmp(tok, "foreign-table") == 0)
			flags |= RESTRICT_RELKIND_FOREIGN_TABLE;
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	/* Save the flags in *extra, for use by the assign function
	 *
	 * 将标志保存在*extra中，供分配函数使用
	 */
	 *extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	 *((int *) *extra) = flags;

	return true;
}

/*
 * GUC assign_hook for restrict_nonsystem_relation_kind
 *
 * 用于restrict_nonsystem_relation_kind的GUC allocate_hook
 */
void
assign_restrict_nonsystem_relation_kind(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	restrict_nonsystem_relation_kind = *flags;
}

/*
 * set_debug_options --- apply "-d N" command line option
 *
 * set_debug_options --- 应用“-d N”命令行选项
 *
 * -d is not quite the same as setting log_min_messages because it enables
 * other output options.
 *
 * -d 与设置 log_min_messages 不太一样，因为它启用其他输出选项。
 */
void
set_debug_options(int debug_flag, GucContext context, GucSource source)
{
	if (debug_flag > 0)
	{
		char		debugstr[64];

		sprintf(debugstr, "debug%d", debug_flag);
		SetConfigOption("log_min_messages", debugstr, context, source);
	}
	else
		SetConfigOption("log_min_messages", "notice", context, source);

	if (debug_flag >= 1 && context == PGC_POSTMASTER)
	{
		SetConfigOption("log_connections", "all", context, source);
		SetConfigOption("log_disconnections", "true", context, source);
	}
	if (debug_flag >= 2)
		SetConfigOption("log_statement", "all", context, source);
	if (debug_flag >= 3)
		SetConfigOption("debug_print_parse", "true", context, source);
	if (debug_flag >= 4)
		SetConfigOption("debug_print_plan", "true", context, source);
	if (debug_flag >= 5)
		SetConfigOption("debug_print_rewritten", "true", context, source);
}


/*
 * Apply a backend -f planner-disabling switch by mapping the option letter to
 * the matching enable_* GUC and setting it false.
 *
 * 通过将选项字母映射到对应的 enable_* GUC 并将其设为 false，应用后端 -f 规划器禁用开关。
 */
bool
set_plan_disabling_options(const char *arg, GucContext context, GucSource source)
{
	const char *tmp = NULL;

	switch (arg[0])
	{
		case 's':				/* seqscan
								 *
								 * 顺序扫描
								 */
			tmp = "enable_seqscan";
			break;
		case 'i':				/* indexscan
				 *
				 * 索引扫描
				 */
			tmp = "enable_indexscan";
			break;
		case 'o':				/* indexonlyscan
								 *
								 * 仅索引扫描
								 */
			tmp = "enable_indexonlyscan";
			break;
		case 'b':				/* bitmapscan
				 *
				 * 位图扫描
				 */
			tmp = "enable_bitmapscan";
			break;
		case 't':				/* tidscan
								 *
								 * TID 扫描
								 */
			tmp = "enable_tidscan";
			break;
		case 'n':				/* nestloop
				 *
				 * 嵌套循环
				 */
			tmp = "enable_nestloop";
			break;
		case 'm':				/* mergejoin
				 *
				 * 合并连接
				 */
			tmp = "enable_mergejoin";
			break;
		case 'h':				/* hashjoin
								 *
								 * 哈希连接
								 */
			tmp = "enable_hashjoin";
			break;
	}
	if (tmp)
	{
		SetConfigOption(tmp, "false", context, source);
		return true;
	}
	else
		return false;
}


/*
 * Return the log_*_stats GUC name selected by a backend -t statistics option,
 * or NULL when the option letter is not recognized.
 *
 * 返回后端 -t 统计选项选中的 log_*_stats GUC 名称；如果选项字母无法识别，则返回 NULL。
 */
const char *
get_stats_option_name(const char *arg)
{
	switch (arg[0])
	{
		case 'p':
			if (optarg[1] == 'a')	/* "parser"
						 *
						 * “解析器”
						 */
				return "log_parser_stats";
			else if (optarg[1] == 'l')	/* "planner"
								 *
								 * “计划者”
								 */
				return "log_planner_stats";
			break;

		case 'e':				/* "executor"
				 *
				 * “执行者”
				 */
			return "log_executor_stats";
			break;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 * process_postgres_switches
 *	   Parse command line arguments for backends
 *
 * process_postgres_switches 解析后端的命令行参数
 *
 * This is called twice, once for the "secure" options coming from the
 * postmaster or command line, and once for the "insecure" options coming
 * from the client's startup packet.  The latter have the same syntax but
 * may be restricted in what they can do.
 *
 * 这被调用两次，一次用于来自邮局管理员或命令行的“安全”选项，一次用于来自客户端启动数据包的“不安全”选项。后者具有相同的语法，但其功
 * 能可能受到限制。
无论哪种情况，
 *
 * argv[0] is ignored in either case (it's assumed to be the program name).
 *
 * argv[0] 都会被忽略（假定它是程序名称）。
 *
 * ctx is PGC_POSTMASTER for secure options, PGC_BACKEND for insecure options
 * coming from the client, or PGC_SU_BACKEND for insecure options coming from
 * a superuser client.
 *
 * ctx 对于安全选项是 PGC_POSTMASTER，对于来自客户端的不安全选项是
 * PGC_BACKEND，对于来自超级用户客户端的不安全选项是 PGC_SU_BACKEND。
 *
 * If a database name is present in the command line arguments, it's
 * returned into *dbname (this is allowed only if *dbname is initially NULL).
 *
 * 如果数据库名称出现在命令行参数中，则会将其返回到 *dbname（仅当 *dbname 最初为 NULL 时才允许这样做）。
 * ----------------------------------------------------------------
 */
void
process_postgres_switches(int argc, char *argv[], GucContext ctx,
						  const char **dbname)
{
	bool		secure = (ctx == PGC_POSTMASTER);
	int			errs = 0;
	GucSource	gucsource;
	int			flag;

	if (secure)
	{
		gucsource = PGC_S_ARGV; /* switches came from command line
							 *
							 * 开关来自命令行
							 */

		/* Ignore the initial --single argument, if present
		 *
		 * 忽略初始 --single 参数（如果存在）
		 */
		if (argc > 1 && strcmp(argv[1], "--single") == 0)
		{
			argv++;
			argc--;
		}
	}
	else
	{
		gucsource = PGC_S_CLIENT;	/* switches came from client
							 *
							 * 开关来自客户端
							 */
	}

#ifdef HAVE_INT_OPTERR

	/*
	 * Turn this off because it's either printed to stderr and not the log
	 * where we'd want it, or argv[0] is now "--single", which would make for
	 * a weird error message.  We print our own error message below.
	 *
	 * 关闭它，因为它要么打印到 stderr 而不是我们想要的日志，要么 argv[0]
	 * 现在是“--single”，这会产生奇怪的错误消息。我们在下面打印我们自己的错误消息。
	 */
	opterr = 0;
#endif

	/*
	 * Parse command-line options.  CAUTION: keep this in sync with
	 * postmaster/postmaster.c (the option sets should not conflict) and with
	 * the common help() function in main/main.c.
	 *
	 * 解析命令行选项。注意：保持与 postmaster/postmaster.c 同步（选项集不应冲突）以及 main/main.c
	 * 中的通用 help() 函数。
	 */
	while ((flag = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:nOPp:r:S:sTt:v:W:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, ctx, gucsource);
				break;

			case 'b':
				/* Undocumented flag used for binary upgrades
				 *
				 * 用于二进制升级的未记录标志
为了与邮政局长保持一致，
				 */
				if (secure)
					IsBinaryUpgrade = true;
				break;

			case 'C':
				/* ignored for consistency with the postmaster
				 *
				 * 被忽略
				 */
				break;

			case '-':

				/*
				 * Error if the user misplaced a special must-be-first option
				 * for dispatching to a subprogram.  parse_dispatch_option()
				 * returns DISPATCH_POSTMASTER if it doesn't find a match, so
				 * error for anything else.
				 *
				 * 如果用户错误放置了用于分派到子程序的特殊的必须优先选项，则会出错。如果 parse_dispatch_option()
				 * 没有找到匹配项，则返回 DISPATCH_POSTMASTER，因此其他任何内容都会出错。
				 */
				if (parse_dispatch_option(optarg) != DISPATCH_POSTMASTER)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("--%s must be first argument", optarg)));

				/* FALLTHROUGH */
			case 'c':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}
					SetConfigOption(name, value, ctx, gucsource);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				if (secure)
					userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), ctx, gucsource);
				break;

			case 'E':
				if (secure)
					EchoQuery = true;
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", ctx, gucsource);
				break;

			case 'F':
				SetConfigOption("fsync", "false", ctx, gucsource);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, ctx, gucsource))
					errs++;
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, ctx, gucsource);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", ctx, gucsource);
				break;

			case 'j':
				if (secure)
					UseSemiNewlineNewline = true;
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, ctx, gucsource);
				break;

			case 'l':
				SetConfigOption("ssl", "true", ctx, gucsource);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, ctx, gucsource);
				break;

			case 'n':
				/* ignored for consistency with postmaster
				 *
				 * 被忽略
				 */
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", ctx, gucsource);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", ctx, gucsource);
				break;

			case 'p':
				SetConfigOption("port", optarg, ctx, gucsource);
				break;

			case 'r':
				/* send output (stdout and stderr) to the given file
				 *
				 * 将输出（stdout 和 stderr）发送到给定文件
				 */
				if (secure)
					strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, ctx, gucsource);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", ctx, gucsource);
				break;

			case 'T':
				/* ignored for consistency with the postmaster
				 *
				 * 被忽略
				 */
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
						SetConfigOption(tmp, "true", ctx, gucsource);
					else
						errs++;
					break;
				}

			case 'v':

				/*
				 * -v is no longer used in normal operation, since
				 * FrontendProtocol is already set before we get here. We keep
				 * the switch only for possible use in standalone operation,
				 * in case we ever support using normal FE/BE protocol with a
				 * standalone backend.
				 *
				 * -v 在正常操作中不再使用，因为 FrontendProtocol
				 * 在我们到达这里之前已经设置好了。我们保留开关仅用于可能在独立操作中使用，以防我们支持使用带有独立后端的正常 FE/BE 协议。
				 */
				if (secure)
					FrontendProtocol = (ProtocolVersion) atoi(optarg);
				break;

			case 'W':
				SetConfigOption("post_auth_delay", optarg, ctx, gucsource);
				break;

			default:
				errs++;
				break;
		}

		if (errs)
			break;
	}

	/*
	 * Optional database name should be there only if *dbname is NULL.
	 *
	 * 仅当 *dbname 为 NULL 时才应该存在可选数据库名称。
	 */
	if (!errs && dbname && *dbname == NULL && argc - optind >= 1)
		 *dbname = strdup(argv[optind++]);

	if (errs || argc != optind)
	{
		if (errs)
			optind--;			/* complain about the previous argument
				 *
				 * 抱怨之前的争论
				 */

		/* spell the error message a bit differently depending on context
		 *
		 * 根据上下文，错误消息的拼写略有不同
		 */
		if (IsUnderPostmaster)
			ereport(FATAL,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid command-line argument for server process: %s", argv[optind]),
					errhint("Try \"%s --help\" for more information.", progname));
		else
			ereport(FATAL,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("%s: invalid command-line argument: %s",
						   progname, argv[optind]),
					errhint("Try \"%s --help\" for more information.", progname));
	}

	/*
	 * Reset getopt(3) library so that it will work correctly in subprocesses
	 * or when this function is called a second time with another array.
	 *
	 * 重置 getopt(3) 库，以便它在子进程中或使用另一个数组第二次调用此函数时正常工作。
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* some systems need this too
					 *
					 * 有些系统也需要这个
					 */
#endif
}


/*
 * PostgresSingleUserMain
 *     Entry point for single user mode. argc/argv are the command line
 *     arguments to be used.
 *
 * PostgresSingleUserMain 单用户模式的入口点。 argc/argv 是要使用的命令行参数。
 *
 * Performs single user specific setup then calls PostgresMain() to actually
 * process queries. Single user mode specific setup should go here, rather
 * than PostgresMain() or InitPostgres() when reasonably possible.
 *
 * 执行单用户特定设置，然后调用 PostgresMain() 来实际处理查询。在合理的情况下，单用户模式的特定设置应该放在此处，而不是
 * PostgresMain() 或 InitPostgres()。
 */
void
PostgresSingleUserMain(int argc, char *argv[],
					   const char *username)
{
	const char *dbname = NULL;

	Assert(!IsUnderPostmaster);

	/* Initialize startup process environment.
	 *
	 * 初始化启动进程环境。
	 */
	InitStandaloneProcess(argv[0]);

	/*
	 * Set default values for command-line options.
	 *
	 * 设置命令行选项的默认值。
	 */
	InitializeGUCOptions();

	/*
	 * Parse command-line options.
	 *
	 * 解析命令行选项。
	 */
	process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);

	/* Must have gotten a database name, or have a default (the username)
	 *
	 * 必须获得数据库名称，或者有默认值（用户名）
	 */
	if (dbname == NULL)
	{
		dbname = username;
		if (dbname == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s: no database nor user name specified",
							progname)));
	}

	/* Acquire configuration parameters
	 *
	 * 获取配置参数
	 */
	if (!SelectConfigFiles(userDoption, progname))
		proc_exit(1);

	/*
	 * Validate we have been given a reasonable-looking DataDir and change
	 * into it.
	 *
	 * 验证我们已经获得了一个看起来合理的 DataDir 并对其进行了更改。
	 */
	checkDataDir();
	ChangeToDataDir();

	/*
	 * Create lockfile for data directory.
	 *
	 * 为数据目录创建锁定文件。
	 */
	CreateDataDirLockFile(false);

	/* read control file (error checking and contains config )
	 *
	 * 读取控制文件（错误检查并包含配置）
	 */
	LocalProcessControlFile(false);

	/*
	 * process any libraries that should be preloaded at postmaster start
	 *
	 * 处理应在 postmaster 启动时预加载的任何库
	 */
	process_shared_preload_libraries();

	/* Initialize MaxBackends
	 *
	 * 初始化MaxBackends
	 */
	InitializeMaxBackends();

	/*
	 * We don't need postmaster child slots in single-user mode, but
	 * initialize them anyway to avoid having special handling.
	 *
	 * 我们不需要单用户模式下的 postmaster 子槽位，但无论如何都要初始化它们以避免进行特殊处理。
	 */
	InitPostmasterChildSlots();

	/* Initialize size of fast-path lock cache.
	 *
	 * 初始化快速路径锁缓存的大小。
	 */
	InitializeFastPathLocks();

	/*
	 * Give preloaded libraries a chance to request additional shared memory.
	 *
	 * 让预加载的库有机会请求额外的共享内存。
	 */
	process_shmem_requests();

	/*
	 * Now that loadable modules have had their chance to request additional
	 * shared memory, determine the value of any runtime-computed GUCs that
	 * depend on the amount of shared memory required.
	 *
	 * 既然可加载模块有机会请求额外的共享内存，请根据所需的共享内存量确定任何运行时计算的 GUC 的值。
	 */
	InitializeShmemGUCs();

	/*
	 * Now that modules have been loaded, we can process any custom resource
	 * managers specified in the wal_consistency_checking GUC.
	 *
	 * 现在模块已经加载，我们可以处理 wal_consistency_checking GUC 中指定的任何自定义资源管理器。
	 */
	InitializeWalConsistencyChecking();

	/*
	 * Create shared memory etc.  (Nothing's really "shared" in single-user
	 * mode, but we must have these data structures anyway.)
	 *
	 * 创建共享内存等（在单用户模式下没有什么是真正“共享”的，但无论如何我们必须拥有这些数据结构。）
	 */
	CreateSharedMemoryAndSemaphores();

	/*
	 * Estimate number of openable files.  This must happen after setting up
	 * semaphores, because on some platforms semaphores count as open files.
	 *
	 * 估计可打开文件的数量。这必须在设置信号量之后发生，因为在某些平台上信号量被视为打开的文件。
	 */
	set_max_safe_fds();

	/*
	 * Remember stand-alone backend startup time,roughly at the same point
	 * during startup that postmaster does so.
	 *
	 * 记住独立后端启动时间，大致在启动过程中与邮局管理员执行此操作的时间相同。
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * Create a per-backend PGPROC struct in shared memory. We must do this
	 * before we can use LWLocks.
	 *
	 * 在共享内存中创建每个后端 PGPROC 结构。在使用 LWLock 之前我们必须这样做。
	 */
	InitProcess();

	/*
	 * Now that sufficient infrastructure has been initialized, PostgresMain()
	 * can do the rest.
	 *
	 * 现在已经初始化了足够的基础设施，PostgresMain() 可以完成剩下的工作。
	 */
	PostgresMain(dbname, username);
}


/* ----------------------------------------------------------------
 * PostgresMain
 *	   postgres main loop -- all backends, interactive or otherwise loop here
 *
 * PostgresMain postgres 主循环——所有后端，交互式或其他方式在这里循环
 *
 * dbname is the name of the database to connect to, username is the
 * PostgreSQL user name to be used for the session.
 *
 * dbname 是要连接的数据库的名称，username 是用于会话的 PostgreSQL 用户名。
 *
 * NB: Single user mode specific setup should go to PostgresSingleUserMain()
 * if reasonably possible.
 *
 * 注意：如果合理可能的话，单用户模式特定设置应该转到 PostgresSingleUserMain() 。
 * ----------------------------------------------------------------
 */
void
PostgresMain(const char *dbname, const char *username)
{
	sigjmp_buf	local_sigjmp_buf;

	/* these must be volatile to ensure state is preserved across longjmp:
	 *
	 * 这些必须是易失性的，以确保在 longjmp 中保留状态：
	 */
	volatile bool send_ready_for_query = true;
	volatile bool idle_in_transaction_timeout_enabled = false;
	volatile bool idle_session_timeout_enabled = false;

	Assert(dbname != NULL);
	Assert(username != NULL);

	Assert(GetProcessingMode() == InitProcessing);

	/*
	 * Set up signal handlers.  (InitPostmasterChild or InitStandaloneProcess
	 * has already set up BlockSig and made that the active signal mask.)
	 *
	 * 设置信号处理程序。 （InitPostmasterChild 或 InitStandaloneProcess 已经设置了
	 * BlockSig 并将其设为活动信号掩码。）
	 *
	 * Note that postmaster blocked all signals before forking child process,
	 * so there is no race condition whereby we might receive a signal before
	 * we have set up the handler.
	 *
	 * 请注意，postmaster 在分叉子进程之前阻止了所有信号，因此不存在竞争条件，因此我们可能会在设置处理程序之前收到信号。
	 *
	 * Also note: it's best not to use any signals that are SIG_IGNored in the
	 * postmaster.  If such a signal arrives before we are able to change the
	 * handler to non-SIG_IGN, it'll get dropped.  Instead, make a dummy
	 * handler in the postmaster to reserve the signal. (Of course, this isn't
	 * an issue for signals that are locally generated, such as SIGALRM and
	 * SIGPIPE.)
	 *
	 * 另请注意：最好不要在 postmaster 中使用任何 SIG_IGNored 信号。如果这样的信号在我们能够将处理程序更改为非
	 * SIG_IGN 之前到达，它将被丢弃。相反，在 postmaster 中创建一个虚拟处理程序来保留信号。
	 * （当然，对于本地生成的信号，例如 SIGALRM 和 SIGPIPE，这不是问题。）
	 */
	if (am_walsender)
		WalSndSignals();
	else
	{
		pqsignal(SIGHUP, SignalHandlerForConfigReload);
		pqsignal(SIGINT, StatementCancelHandler);	/* cancel current query
											 *
											 * 取消当前查询
											 */
		pqsignal(SIGTERM, die); /* cancel current query and exit
							 *
							 * 取消当前查询并退出
							 */

		/*
		 * In a postmaster child backend, replace SignalHandlerForCrashExit
		 * with quickdie, so we can tell the client we're dying.
		 *
		 * 在postmaster子后端中，用quickdie替换SignalHandlerForCrashExit，这样我们就可以告诉客户端我
		 * 们快要死了。
		 *
		 * In a standalone backend, SIGQUIT can be generated from the keyboard
		 * easily, while SIGTERM cannot, so we make both signals do die()
		 * rather than quickdie().
		 *
		 * 在独立后端中，SIGQUIT 可以轻松地从键盘生成，而 SIGTERM 则不能，因此我们使这两个信号都执行 die() 而不是
		 * Quickdie()。
		 */
		if (IsUnderPostmaster)
			pqsignal(SIGQUIT, quickdie);	/* hard crash time
								 *
								 * 硬崩溃时间
								 */
		else
			pqsignal(SIGQUIT, die); /* cancel current query and exit
							 *
							 * 取消当前查询并退出
							 */
		InitializeTimeouts();	/* establishes SIGALRM handler
						 *
						 * 建立 SIGALRM 处理程序
						 */

		/*
		 * Ignore failure to write to frontend. Note: if frontend closes
		 * connection, we will notice it and exit cleanly when control next
		 * returns to outer loop.  This seems safer than forcing exit in the
		 * midst of output during who-knows-what operation...
		 *
		 * 忽略写入前端的失败。注意：如果前端关闭连接，我们会注意到它并在控制接下来返回到外循环时干净地退出。这似乎比在谁知道什么操作期间在输出
		 * 过程中强制退出更安全......
		 */
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGUSR2, SIG_IGN);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/*
		 * Reset some signals that are accepted by postmaster but not by
		 * backend
		 *
		 * 重置一些被postmaster接受但不被后端接受的信号
		 */
		pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some
									 * platforms
									 *
									 * system() 在某些平台上需要这个*/
	}

	/* Early initialization
	 *
	 * 早期初始化
	 */
	BaseInit();

	/* We need to allow SIGINT, etc during the initial transaction
	 *
	 * 我们需要在初始事务期间允许 SIGINT 等
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Generate a random cancel key, if this is a backend serving a
	 * connection. InitPostgres() will advertise it in shared memory.
	 *
	 * 如果这是服务连接的后端，则生成随机取消密钥。 InitPostgres() 将在共享内存中通告它。
	 */
	Assert(MyCancelKeyLength == 0);
	if (whereToSendOutput == DestRemote)
	{
		int			len;

		len = (MyProcPort == NULL || MyProcPort->proto >= PG_PROTOCOL(3, 2))
			? MAX_CANCEL_KEY_LENGTH : 4;
		if (!pg_strong_random(&MyCancelKey, len))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random cancel key")));
		}
		MyCancelKeyLength = len;
	}

	/*
	 * General initialization.
	 *
	 * 一般初始化。
	 *
	 * NOTE: if you are tempted to add code in this vicinity, consider putting
	 * it inside InitPostgres() instead.  In particular, anything that
	 * involves database access should be there, not here.
	 *
	 * 注意：如果您想在附近添加代码，请考虑将其放在 InitPostgres()
	 * 内。特别是，任何涉及数据库访问的内容都应该在那里，而不是在这里。
	 *
	 * Honor session_preload_libraries if not dealing with a WAL sender.
	 *
	 * 如果不处理 WAL 发送者，则尊重 session_preload_libraries。
	 */
	InitPostgres(dbname, InvalidOid,	/* database to connect to
									 *
									 * 要连接的数据库
									 */
				 username, InvalidOid,	/* role to connect as
							 *
							 * 连接角色
							 */
				 (!am_walsender) ? INIT_PG_LOAD_SESSION_LIBS : 0,
				 NULL);			/* no out_dbname
				 *
				 * 没有 out_dbname
				 */

	/*
	 * If the PostmasterContext is still around, recycle the space; we don't
	 * need it anymore after InitPostgres completes.
	 *
	 * 如果 PostmasterContext 仍然存在，则回收空间； InitPostgres 完成后我们就不再需要它了。
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	SetProcessingMode(NormalProcessing);

	/*
	 * Now all GUC states are fully set up.  Report them to client if
	 * appropriate.
	 *
	 * 现在所有 GUC 状态都已完全建立。如果合适的话，向客户报告。
	 */
	BeginReportingGUCOptions();

	/*
	 * Also set up handler to log session end; we have to wait till now to be
	 * sure Log_disconnections has its final value.
	 *
	 * 还设置处理程序来记录会话结束；我们必须等到现在才能确定 Log_disconnections 具有其最终值。
	 */
	if (IsUnderPostmaster && Log_disconnections)
		on_proc_exit(log_disconnections, 0);

	pgstat_report_connect(MyDatabaseId);

	/* Perform initialization specific to a WAL sender process.
	 *
	 * 执行特定于 WAL 发送进程的初始化。
	 */
	if (am_walsender)
		InitWalSender();

	/*
	 * Send this backend's cancellation info to the frontend.
	 *
	 * 将此后端的取消信息发送到前端。
	 */
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		Assert(MyCancelKeyLength > 0);
		pq_beginmessage(&buf, PqMsg_BackendKeyData);
		pq_sendint32(&buf, (int32) MyProcPid);

		pq_sendbytes(&buf, MyCancelKey, MyCancelKeyLength);
		pq_endmessage(&buf);
		/* Need not flush since ReadyForQuery will do it.
		 *
		 * 不需要刷新，因为 ReadyForQuery 会执行此操作。
		 */
	}

	/* Welcome banner for standalone case
	 *
	 * 独立案例的欢迎横幅
	 */
	if (whereToSendOutput == DestDebug)
		printf("\nPostgreSQL stand-alone backend %s\n", PG_VERSION);

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * 创建我们将在主循环中使用的内存上下文。
	 *
	 * MessageContext is reset once per iteration of the main loop, ie, upon
	 * completion of processing of each command message from the client.
	 *
	 * MessageContext 在主循环的每次迭代中重置一次，即在完成对来自客户端的每个命令消息的处理时。
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_SIZES);

	/*
	 * Create memory context and buffer used for RowDescription messages. As
	 * SendRowDescriptionMessage(), via exec_describe_statement_message(), is
	 * frequently executed for ever single statement, we don't want to
	 * allocate a separate buffer every time.
	 *
	 * 创建用于 RowDescription 消息的内存上下文和缓冲区。由于 SendRowDescriptionMessage() 通过
	 * exec_describe_statement_message() 经常对单个语句执行，因此我们不希望每次都分配单独的缓冲区。
	 */
	row_description_context = AllocSetContextCreate(TopMemoryContext,
													"RowDescriptionContext",
													ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(row_description_context);
	initStringInfo(&row_description_buf);
	MemoryContextSwitchTo(TopMemoryContext);

	/* Fire any defined login event triggers, if appropriate
	 *
	 * 触发任何已定义的登录事件触发器（如果适用）
	 */
	EventTriggerOnLogin();

	/*
	 * POSTGRES main processing loop begins here
	 *
	 * POSTGRES 主处理循环从这里开始
	 *
	 * If an exception is encountered, processing resumes here so we abort the
	 * current transaction and start a new one.
	 *
	 * 如果遇到异常，处理将在此处恢复，因此我们中止当前事务并开始一个新事务。
	 *
	 * You might wonder why this isn't coded as an infinite loop around a
	 * PG_TRY construct.  The reason is that this is the bottom of the
	 * exception stack, and so with PG_TRY there would be no exception handler
	 * in force at all during the CATCH part.  By leaving the outermost setjmp
	 * always active, we have at least some chance of recovering from an error
	 * during error recovery.  (If we get into an infinite loop thereby, it
	 * will soon be stopped by overflow of elog.c's internal state stack.)
	 *
	 * 您可能想知道为什么它没有被编码为围绕 PG_TRY 构造的无限循环。原因是这是异常堆栈的底部，因此使用 PG_TRY 在 CATCH
	 * 部分期间根本不会有有效的异常处理程序。通过让最外层的 setjmp 始终处于活动状态，我们至少有一些机会在错误恢复期间从错误中恢复。
	 * （如果我们因此进入无限循环，它很快就会因 elog.c 的内部状态堆栈溢出而停止。）
	 *
	 * Note that we use sigsetjmp(..., 1), so that this function's signal mask
	 * (to wit, UnBlockSig) will be restored when longjmp'ing to here.  This
	 * is essential in case we longjmp'd out of a signal handler on a platform
	 * where that leaves the signal blocked.  It's not redundant with the
	 * unblock in AbortTransaction() because the latter is only called if we
	 * were inside a transaction.
	 *
	 * 请注意，我们使用 sigsetjmp(..., 1)，这样当 longjmp 到这里时，该函数的信号掩码（即
	 * UnBlockSig）将被恢复。如果我们在平台上从信号处理程序中进行 longjmp 操作，导致信号被阻塞，这一点至关重要。它与
	 * AbortTransaction() 中的解锁并不多余，因为后者仅在我们处于事务内部时才会被调用。
	 */

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/*
		 * NOTE: if you are tempted to add more code in this if-block,
		 * consider the high probability that it should be in
		 * AbortTransaction() instead.  The only stuff done directly here
		 * should be stuff that is guaranteed to apply *only* for outer-level
		 * error recovery, such as adjusting the FE/BE protocol status.
		 *
		 * 注意：如果您想在这个 if 块中添加更多代码，请考虑它很可能应该在 AbortTransaction()
		 * 中。在这里直接完成的唯一事情应该是保证“仅”应用于外层错误恢复的事情，例如调整 FE/BE 协议状态。
		 */

		/* Since not using PG_TRY, must reset error stack by hand
		 *
		 * 由于不使用PG_TRY，必须手动重置错误堆栈
		 */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up
		 *
		 * 清理时防止中断
		 */
		HOLD_INTERRUPTS();

		/*
		 * Forget any pending QueryCancel request, since we're returning to
		 * the idle loop anyway, and cancel any active timeout requests.  (In
		 * future we might want to allow some timeout requests to survive, but
		 * at minimum it'd be necessary to do reschedule_timeouts(), in case
		 * we got here because of a query cancel interrupting the SIGALRM
		 * interrupt handler.)	Note in particular that we must clear the
		 * statement and lock timeout indicators, to prevent any future plain
		 * query cancels from being misreported as timeouts in case we're
		 * forgetting a timeout cancel.
		 *
		 * 忘记任何挂起的 QueryCancel 请求，因为我们无论如何都会返回到空闲循环，并取消任何活动的超时请求。
		 * （将来我们可能希望允许一些超时请求继续存在，但至少有必要执行 reschedule_timeouts() ，以防我们因为查询取消中断
		 * SIGALRM 中断处理程序而到达这里。）特别注意，我们必须清除语句并锁定超时指示器，以防止将来的任何普通查询取消被错误报告为超时，
		 * 以防万一我们忘记了超时取消。
		 */
		disable_all_timeouts(false);	/* do first to avoid race condition
								 *
								 * 首先执行以避免竞争条件
								 */
		QueryCancelPending = false;
		idle_in_transaction_timeout_enabled = false;
		idle_session_timeout_enabled = false;

		/* Not reading from the client anymore.
		 *
		 * 不再阅读客户端的内容。
		 */
		DoingCommandRead = false;

		/* Make sure libpq is in a good state
		 *
		 * 确保 libpq 处于良好状态
		 */
		pq_comm_reset();

		/* Report the error to the client and/or server log
		 *
		 * 将错误报告给客户端和/或服务器日志
		 */
		EmitErrorReport();

		/*
		 * If Valgrind noticed something during the erroneous query, print the
		 * query string, assuming we have one.
		 *
		 * 如果 Valgrind 在错误查询期间注意到某些内容，请打印查询字符串，假设我们有一个。
		 */
		valgrind_report_error_query(debug_query_string);

		/*
		 * Make sure debug_query_string gets reset before we possibly clobber
		 * the storage it points at.
		 *
		 * 确保 debug_query_string 在我们可能破坏它指向的存储之前重置。
		 */
		debug_query_string = NULL;

		/*
		 * Abort the current transaction in order to recover.
		 *
		 * 中止当前事务以恢复。
		 */
		AbortCurrentTransaction();

		if (am_walsender)
			WalSndErrorCleanup();

		PortalErrorCleanup();

		/*
		 * We can't release replication slots inside AbortTransaction() as we
		 * need to be able to start and abort transactions while having a slot
		 * acquired. But we never need to hold them across top level errors,
		 * so releasing here is fine. There also is a before_shmem_exit()
		 * callback ensuring correct cleanup on FATAL errors.
		 *
		 * 我们无法在 AbortTransaction()
		 * 内释放复制槽，因为我们需要能够在获取槽时启动和中止事务。但我们永远不需要让它们遇到顶级错误，所以在这里发布就可以了。还有一个
		 * before_shmem_exit() 回调确保正确清除致命错误。
		 */
		if (MyReplicationSlot != NULL)
			ReplicationSlotRelease();

		/* We also want to cleanup temporary slots on error.
		 *
		 * 我们还想在出错时清理临时槽。
		 */
		ReplicationSlotCleanup(false);

		jit_reset_after_error();

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 *
		 * 现在返回到正常的顶级上下文并清除 ErrorContext 以供下次使用。
		 */
		MemoryContextSwitchTo(MessageContext);
		FlushErrorState();

		/*
		 * If we were handling an extended-query-protocol message, initiate
		 * skip till next Sync.  This also causes us not to issue
		 * ReadyForQuery (until we get Sync).
		 *
		 * 如果我们正在处理扩展查询协议消息，则启动跳过直到下一次同步。这也导致我们不会发出 ReadyForQuery（直到我们获得同步）。
		 */
		if (doing_extended_query_message)
			ignore_till_sync = true;

		/* We don't have a transaction command open anymore
		 *
		 * 我们不再打开交易命令
		 */
		xact_started = false;

		/*
		 * If an error occurred while we were reading a message from the
		 * client, we have potentially lost track of where the previous
		 * message ends and the next one begins.  Even though we have
		 * otherwise recovered from the error, we cannot safely read any more
		 * messages from the client, so there isn't much we can do with the
		 * connection anymore.
		 *
		 * 如果我们在从客户端读取消息时发生错误，我们可能会丢失上一条消息的结束位置和下一条消息的开始位置。尽管我们已经从错误中恢复，但我们无法
		 * 安全地从客户端读取更多消息，因此我们无法再对连接做太多事情。
		 */
		if (pq_is_reading_msg())
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("terminating connection because protocol synchronization was lost")));

		/* Now we can allow interrupts again
		 *
		 * 现在我们可以再次允许中断
		 */
		RESUME_INTERRUPTS();
	}

	/* We can now handle ereport(ERROR)
	 *
	 * 我们现在可以处理 ereport(ERROR)
	 */
	PG_exception_stack = &local_sigjmp_buf;

	if (!ignore_till_sync)
		send_ready_for_query = true;	/* initially, or after error
								 *
								 * 最初或错误后
								 */

	/*
	 * Non-error queries loop here.
	 *
	 * 非错误查询在此循环。
	 */

	for (;;)
	{
		int			firstchar;
		StringInfoData input_message;

		/*
		 * At top of loop, reset extended-query-message flag, so that any
		 * errors encountered in "idle" state don't provoke skip.
		 *
		 * 在循环顶部，重置扩展查询消息标志，以便在“空闲”状态下遇到的任何错误都不会引起跳过。
		 */
		doing_extended_query_message = false;

		/*
		 * For valgrind reporting purposes, the "current query" begins here.
		 *
		 * 出于 valgrind 报告的目的，“当前查询”从这里开始。
		 */
#ifdef USE_VALGRIND
		old_valgrind_error_count = VALGRIND_COUNT_ERRORS;
#endif

		/*
		 * Release storage left over from prior query cycle, and create a new
		 * query input buffer in the cleared MessageContext.
		 *
		 * 释放先前查询周期剩余的存储，并在清除的 MessageContext 中创建新的查询输入缓冲区。
		 */
		MemoryContextSwitchTo(MessageContext);
		MemoryContextReset(MessageContext);

		initStringInfo(&input_message);

		/*
		 * Also consider releasing our catalog snapshot if any, so that it's
		 * not preventing advance of global xmin while we wait for the client.
		 *
		 * 还要考虑发布我们的目录快照（如果有），这样在我们等待客户端时就不会阻止全局 xmin 的前进。
		 */
		InvalidateCatalogSnapshotConditionally();

		/*
		 * (1) If we've reached idle state, tell the frontend we're ready for
		 * a new query.
		 *
		 * (1) 如果我们已达到空闲状态，请告诉前端我们已准备好进行新查询。
		 *
		 * Note: this includes fflush()'ing the last of the prior output.
		 *
		 * 注意：这包括 fflush() 对先前输出的最后一个进行处理。
		 *
		 * This is also a good time to flush out collected statistics to the
		 * cumulative stats system, and to update the PS stats display.  We
		 * avoid doing those every time through the message loop because it'd
		 * slow down processing of batched messages, and because we don't want
		 * to report uncommitted updates (that confuses autovacuum).  The
		 * notification processor wants a call too, if we are not in a
		 * transaction block.
		 *
		 * 这也是将收集的统计数据刷新到累积统计数据系统并更新 PS 统计数据显示的好时机。我们避免每次通过消息循环都执行这些操作，因为它会减慢
		 * 批量消息的处理速度，并且因为我们不想报告未提交的更新（这会混淆 autovacuum）。如果我们不在事务块中，通知处理器也需要调用。
		 *
		 * Also, if an idle timeout is enabled, start the timer for that.
		 *
		 * 另外，如果启用了空闲超时，请为此启动计时器。
		 */
		if (send_ready_for_query)
		{
			if (IsAbortedTransactionBlockState())
			{
				set_ps_display("idle in transaction (aborted)");
				pgstat_report_activity(STATE_IDLEINTRANSACTION_ABORTED, NULL);

				/* Start the idle-in-transaction timer
				 *
				 * 启动事务中空闲计时器
				 */
				if (IdleInTransactionSessionTimeout > 0
					&& (IdleInTransactionSessionTimeout < TransactionTimeout || TransactionTimeout == 0))
				{
					idle_in_transaction_timeout_enabled = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else if (IsTransactionOrTransactionBlock())
			{
				set_ps_display("idle in transaction");
				pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);

				/* Start the idle-in-transaction timer
				 *
				 * 启动事务中空闲计时器
				 */
				if (IdleInTransactionSessionTimeout > 0
					&& (IdleInTransactionSessionTimeout < TransactionTimeout || TransactionTimeout == 0))
				{
					idle_in_transaction_timeout_enabled = true;
					enable_timeout_after(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
										 IdleInTransactionSessionTimeout);
				}
			}
			else
			{
				long		stats_timeout;

				/*
				 * Process incoming notifies (including self-notifies), if
				 * any, and send relevant messages to the client.  Doing it
				 * here helps ensure stable behavior in tests: if any notifies
				 * were received during the just-finished transaction, they'll
				 * be seen by the client before ReadyForQuery is.
				 *
				 * 处理传入的通知（包括自我通知）（如果有），并向客户端发送相关消息。在这里执行此操作有助于确保测试中的稳定行为：如果在刚刚完成的事务期
				 * 间收到任何通知，客户端将在 ReadyForQuery 之前看到它们。
				 */
				if (notifyInterruptPending)
					ProcessNotifyInterrupt(false);

				/*
				 * Check if we need to report stats. If pgstat_report_stat()
				 * decides it's too soon to flush out pending stats / lock
				 * contention prevented reporting, it'll tell us when we
				 * should try to report stats again (so that stats updates
				 * aren't unduly delayed if the connection goes idle for a
				 * long time). We only enable the timeout if we don't already
				 * have a timeout in progress, because we don't disable the
				 * timeout below. enable_timeout_after() needs to determine
				 * the current timestamp, which can have a negative
				 * performance impact. That's OK because pgstat_report_stat()
				 * won't have us wake up sooner than a prior call.
				 *
				 * 检查我们是否需要报告统计数据。如果 pgstat_report_stat() 认为现在清除挂起的统计信息/锁争用阻止报告还为时过早，
				 * 它会告诉我们何时应该再次尝试报告统计信息（这样，如果连接长时间空闲，统计信息更新就不会被过度延迟）。仅当我们尚未设置超时时才启用超时
				 * ，因为我们不会禁用下面的超时。 enable_timeout_after()
				 * 需要确定当前时间戳，这可能会对性能产生负面影响。没关系，因为 pgstat_report_stat()
				 * 不会让我们比之前的调用更早醒来。
				 */
				stats_timeout = pgstat_report_stat(false);
				if (stats_timeout > 0)
				{
					if (!get_timeout_active(IDLE_STATS_UPDATE_TIMEOUT))
						enable_timeout_after(IDLE_STATS_UPDATE_TIMEOUT,
											 stats_timeout);
				}
				else
				{
					/* all stats flushed, no need for the timeout
					 *
					 * 所有统计数据均已刷新，无需超时
					 */
					if (get_timeout_active(IDLE_STATS_UPDATE_TIMEOUT))
						disable_timeout(IDLE_STATS_UPDATE_TIMEOUT, false);
				}

				set_ps_display("idle");
				pgstat_report_activity(STATE_IDLE, NULL);

				/* Start the idle-session timer
				 *
				 * 启动空闲会话计时器
				 */
				if (IdleSessionTimeout > 0)
				{
					idle_session_timeout_enabled = true;
					enable_timeout_after(IDLE_SESSION_TIMEOUT,
										 IdleSessionTimeout);
				}
			}

			/* Report any recently-changed GUC options
			 *
			 * 报告任何最近更改的 GUC 选项
			 */
			ReportChangedGUCOptions();

			/*
			 * The first time this backend is ready for query, log the
			 * durations of the different components of connection
			 * establishment and setup.
			 *
			 * 该后端第一次准备好查询时，记录连接建立和设置的不同组件的持续时间。
			 */
			if (conn_timing.ready_for_use == TIMESTAMP_MINUS_INFINITY &&
				(log_connections & LOG_CONNECTION_SETUP_DURATIONS) &&
				IsExternalConnectionBackend(MyBackendType))
			{
				uint64		total_duration,
							fork_duration,
							auth_duration;

				conn_timing.ready_for_use = GetCurrentTimestamp();

				total_duration =
					TimestampDifferenceMicroseconds(conn_timing.socket_create,
													conn_timing.ready_for_use);
				fork_duration =
					TimestampDifferenceMicroseconds(conn_timing.fork_start,
													conn_timing.fork_end);
				auth_duration =
					TimestampDifferenceMicroseconds(conn_timing.auth_start,
													conn_timing.auth_end);

				ereport(LOG,
						errmsg("connection ready: setup total=%.3f ms, fork=%.3f ms, authentication=%.3f ms",
							   (double) total_duration / NS_PER_US,
							   (double) fork_duration / NS_PER_US,
							   (double) auth_duration / NS_PER_US));
			}

			ReadyForQuery(whereToSendOutput);
			send_ready_for_query = false;
		}

		/*
		 * (2) Allow asynchronous signals to be executed immediately if they
		 * come in while we are waiting for client input. (This must be
		 * conditional since we don't want, say, reads on behalf of COPY FROM
		 * STDIN doing the same thing.)
		 *
		 * (2) 如果异步信号在我们等待客户端输入时传入，则允许立即执行。 （这必须是有条件的，因为我们不希望代表 COPY FROM
		 * STDIN 进行读取做同样的事情。）
		 */
		DoingCommandRead = true;

		/*
		 * (3) read a command (loop blocks here)
		 *
		 * (3) 读取命令（此处循环块）
		 */
		firstchar = ReadCommand(&input_message);

		/*
		 * (4) turn off the idle-in-transaction and idle-session timeouts if
		 * active.  We do this before step (5) so that any last-moment timeout
		 * is certain to be detected in step (5).
		 *
		 * (4) 关闭事务中空闲超时和空闲会话超时（如果处于活动状态）。我们在步骤 (5) 之前执行此操作，以便在步骤 (5)
		 * 中一定会检测到任何最后时刻的超时。
		 *
		 * At most one of these timeouts will be active, so there's no need to
		 * worry about combining the timeout.c calls into one.
		 *
		 * 这些超时中最多有一个会处于活动状态，因此无需担心将 timeout.c 调用合并为一个。
		 */
		if (idle_in_transaction_timeout_enabled)
		{
			disable_timeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT, false);
			idle_in_transaction_timeout_enabled = false;
		}
		if (idle_session_timeout_enabled)
		{
			disable_timeout(IDLE_SESSION_TIMEOUT, false);
			idle_session_timeout_enabled = false;
		}

		/*
		 * (5) disable async signal conditions again.
		 *
		 * (5) 再次禁用异步信号条件。
		 *
		 * Query cancel is supposed to be a no-op when there is no query in
		 * progress, so if a query cancel arrived while we were idle, just
		 * reset QueryCancelPending. ProcessInterrupts() has that effect when
		 * it's called when DoingCommandRead is set, so check for interrupts
		 * before resetting DoingCommandRead.
		 *
		 * 当没有正在进行的查询时，查询取消应该是无操作，因此如果在我们空闲时查询取消到达，只需重置 QueryCancelPending
		 * 即可。当设置 DoingCommandRead 时调用 ProcessInterrupts() 时会产生这种效果，因此在重置
		 * DoingCommandRead 之前检查中断。
		 */
		CHECK_FOR_INTERRUPTS();
		DoingCommandRead = false;

		/*
		 * (6) check for any other interesting events that happened while we
		 * slept.
		 *
		 * (6) 检查我们睡觉时发生的任何其他有趣的事件。
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * (7) process the command.  But ignore it if we're skipping till
		 * Sync.
		 *
		 * (7) 处理命令。但如果我们跳过直到同步，请忽略它。
		 */
		if (ignore_till_sync && firstchar != EOF)
			continue;

		switch (firstchar)
		{
			case PqMsg_Query:
				{
					const char *query_string;

					/* Set statement_timestamp()
					 *
					 * 设置statement_timestamp()
					 */
					SetCurrentStatementStartTimestamp();

					query_string = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					if (am_walsender)
					{
						if (!exec_replication_command(query_string))
							exec_simple_query(query_string);
					}
					else
						exec_simple_query(query_string);

					valgrind_report_error_query(query_string);

					send_ready_for_query = true;
				}
				break;

			case PqMsg_Parse:
				{
					const char *stmt_name;
					const char *query_string;
					int			numParams;
					Oid		   *paramTypes = NULL;

					forbidden_in_wal_sender(firstchar);

					/* Set statement_timestamp()
					 *
					 * 设置statement_timestamp()
					 */
					SetCurrentStatementStartTimestamp();

					stmt_name = pq_getmsgstring(&input_message);
					query_string = pq_getmsgstring(&input_message);
					numParams = pq_getmsgint(&input_message, 2);
					if (numParams > 0)
					{
						paramTypes = palloc_array(Oid, numParams);
						for (int i = 0; i < numParams; i++)
							paramTypes[i] = pq_getmsgint(&input_message, 4);
					}
					pq_getmsgend(&input_message);

					exec_parse_message(query_string, stmt_name,
									   paramTypes, numParams);

					valgrind_report_error_query(query_string);
				}
				break;

			case PqMsg_Bind:
				forbidden_in_wal_sender(firstchar);

				/* Set statement_timestamp()
				 *
				 * 设置statement_timestamp()
				 */
				SetCurrentStatementStartTimestamp();

				/*
				 * this message is complex enough that it seems best to put
				 * the field extraction out-of-line
				 *
				 * 此消息足够复杂，似乎最好将字段提取置于外线
				 */
				exec_bind_message(&input_message);

				/* exec_bind_message does valgrind_report_error_query
				 *
				 * exec_bind_message 执行 valgrind_report_error_query
				 */
				break;

			case PqMsg_Execute:
				{
					const char *portal_name;
					int			max_rows;

					forbidden_in_wal_sender(firstchar);

					/* Set statement_timestamp()
					 *
					 * 设置statement_timestamp()
					 */
					SetCurrentStatementStartTimestamp();

					portal_name = pq_getmsgstring(&input_message);
					max_rows = pq_getmsgint(&input_message, 4);
					pq_getmsgend(&input_message);

					exec_execute_message(portal_name, max_rows);

					/* exec_execute_message does valgrind_report_error_query
					 *
					 * exec_execute_message 执行 valgrind_report_error_query
					 */
				}
				break;

			case PqMsg_FunctionCall:
				forbidden_in_wal_sender(firstchar);

				/* Set statement_timestamp()
				 *
				 * 设置statement_timestamp()
				 */
				SetCurrentStatementStartTimestamp();

				/* Report query to various monitoring facilities.
				 *
				 * 向各监控设施报告查询。
				 */
				pgstat_report_activity(STATE_FASTPATH, NULL);
				set_ps_display("<FASTPATH>");

				/* start an xact for this function invocation
				 *
				 * 为此函数调用启动一个xact
				 */
				start_xact_command();

				/*
				 * Note: we may at this point be inside an aborted
				 * transaction.  We can't throw error for that until we've
				 * finished reading the function-call message, so
				 * HandleFunctionRequest() must check for it after doing so.
				 * Be careful not to do anything that assumes we're inside a
				 * valid transaction here.
				 *
				 * 注意：此时我们可能处于已中止的事务中。在读取完函数调用消息之前，我们不能为此抛出错误，因此
				 * HandleFunctionRequest() 必须在完成此操作后检查它。请小心，不要做任何假设我们处于有效交易中的事情。
				 */

				/* switch back to message context
				 *
				 * 切换回消息上下文
				 */
				MemoryContextSwitchTo(MessageContext);

				HandleFunctionRequest(&input_message);

				/* commit the function-invocation transaction
				 *
				 * 提交函数调用事务
				 */
				finish_xact_command();

				valgrind_report_error_query("fastpath function call");

				send_ready_for_query = true;
				break;

			case PqMsg_Close:
				{
					int			close_type;
					const char *close_target;

					forbidden_in_wal_sender(firstchar);

					close_type = pq_getmsgbyte(&input_message);
					close_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (close_type)
					{
						case 'S':
							if (close_target[0] != '\0')
								DropPreparedStatement(close_target, false);
							else
							{
								/* special-case the unnamed statement
								 *
								 * 未命名语句的特例
								 */
								drop_unnamed_stmt();
							}
							break;
						case 'P':
							{
								Portal		portal;

								portal = GetPortalByName(close_target);
								if (PortalIsValid(portal))
									PortalDrop(portal, false);
							}
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid CLOSE message subtype %d",
											close_type)));
							break;
					}

					if (whereToSendOutput == DestRemote)
						pq_putemptymessage(PqMsg_CloseComplete);

					valgrind_report_error_query("CLOSE message");
				}
				break;

			case PqMsg_Describe:
				{
					int			describe_type;
					const char *describe_target;

					forbidden_in_wal_sender(firstchar);

					/* Set statement_timestamp() (needed for xact)
					 *
					 * 设置statement_timestamp()（xact需要）
					 */
					SetCurrentStatementStartTimestamp();

					describe_type = pq_getmsgbyte(&input_message);
					describe_target = pq_getmsgstring(&input_message);
					pq_getmsgend(&input_message);

					switch (describe_type)
					{
						case 'S':
							exec_describe_statement_message(describe_target);
							break;
						case 'P':
							exec_describe_portal_message(describe_target);
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("invalid DESCRIBE message subtype %d",
											describe_type)));
							break;
					}

					valgrind_report_error_query("DESCRIBE message");
				}
				break;

			case PqMsg_Flush:
				pq_getmsgend(&input_message);
				if (whereToSendOutput == DestRemote)
					pq_flush();
				break;

			case PqMsg_Sync:
				pq_getmsgend(&input_message);

				/*
				 * If pipelining was used, we may be in an implicit
				 * transaction block. Close it before calling
				 * finish_xact_command.
				 *
				 * 如果使用了流水线，我们可能处于隐式事务块中。在调用 finish_xact_command 之前关闭它。
				 */
				EndImplicitTransactionBlock();
				finish_xact_command();
				valgrind_report_error_query("SYNC message");
				send_ready_for_query = true;
				break;

				/*
				 * PqMsg_Terminate means that the frontend is closing down the
				 * socket. EOF means unexpected loss of frontend connection.
				 * Either way, perform normal shutdown.
				 *
				 * PqMsg_Terminate 表示前端正在关闭套接字。 EOF 表示前端连接意外丢失。无论哪种方式，执行正常关闭。
				 */
			case EOF:

				/* for the cumulative statistics system
				 *
				 * 累计统计系统
				 */
				pgStatSessionEndCause = DISCONNECT_CLIENT_EOF;

				/* FALLTHROUGH */

			case PqMsg_Terminate:

				/*
				 * Reset whereToSendOutput to prevent ereport from attempting
				 * to send any more messages to client.
				 *
				 * 重置 whereToSendOutput 以防止 ereport 尝试向客户端发送更多消息。
				 */
				if (whereToSendOutput == DestRemote)
					whereToSendOutput = DestNone;

				/*
				 * NOTE: if you are tempted to add more code here, DON'T!
				 * Whatever you had in mind to do should be set up as an
				 * on_proc_exit or on_shmem_exit callback, instead. Otherwise
				 * it will fail to be called during other backend-shutdown
				 * scenarios.
				 *
				 * 注意：如果您想在此处添加更多代码，请不要！无论您打算做什么，都应该将其设置为 on_proc_exit 或
				 * on_shmem_exit 回调。否则在其他后端关闭场景下会调用失败。
				 */
				proc_exit(0);

			case PqMsg_CopyData:
			case PqMsg_CopyDone:
			case PqMsg_CopyFail:

				/*
				 * Accept but ignore these messages, per protocol spec; we
				 * probably got here because a COPY failed, and the frontend
				 * is still sending data.
				 *
				 * 根据协议规范接受但忽略这些消息；我们到达这里可能是因为复制失败，而前端仍在发送数据。
				 */
				break;

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d",
								firstchar)));
		}
	}							/* end of input-reading loop
		 *
		 * 输入读取循环结束
		 */
}

/*
 * Throw an error if we're a WAL sender process.
 *
 * 如果我们是 WAL 发送进程，则抛出错误。
 *
 * This is used to forbid anything else than simple query protocol messages
 * in a WAL sender process.  'firstchar' specifies what kind of a forbidden
 * message was received, and is used to construct the error message.
 *
 * 这用于禁止 WAL 发送进程中除简单查询协议消息之外的任何其他内容。 'firstchar'
 * 指定收到何种类型的禁止消息，并用于构造错误消息。
 */
static void
forbidden_in_wal_sender(char firstchar)
{
	if (am_walsender)
	{
		if (firstchar == PqMsg_FunctionCall)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("fastpath function calls not supported in a replication connection")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("extended query protocol not supported in a replication connection")));
	}
}


static struct rusage Save_r;
static struct timeval Save_t;

/*
 * Capture the current resource-usage baseline used by ShowUsage().
 *
 * 捕获 ShowUsage() 使用的当前资源使用量基准。
 */
void
ResetUsage(void)
{
	getrusage(RUSAGE_SELF, &Save_r);
	gettimeofday(&Save_t, NULL);
}

/*
 * Report elapsed resource usage since ResetUsage(), including CPU time and
 * platform rusage counters that are available.
 *
 * 报告自 ResetUsage() 以来经过的资源使用情况，包括 CPU 时间和可用的平台 rusage 计数器。
 */
void
ShowUsage(const char *title)
{
	StringInfoData str;
	struct timeval user,
				sys;
	struct timeval elapse_t;
	struct rusage r;

	getrusage(RUSAGE_SELF, &r);
	gettimeofday(&elapse_t, NULL);
	memcpy(&user, &r.ru_utime, sizeof(user));
	memcpy(&sys, &r.ru_stime, sizeof(sys));
	if (elapse_t.tv_usec < Save_t.tv_usec)
	{
		elapse_t.tv_sec--;
		elapse_t.tv_usec += 1000000;
	}
	if (r.ru_utime.tv_usec < Save_r.ru_utime.tv_usec)
	{
		r.ru_utime.tv_sec--;
		r.ru_utime.tv_usec += 1000000;
	}
	if (r.ru_stime.tv_usec < Save_r.ru_stime.tv_usec)
	{
		r.ru_stime.tv_sec--;
		r.ru_stime.tv_usec += 1000000;
	}

	/*
	 * The only stats we don't show here are ixrss, idrss, isrss.  It takes
	 * some work to interpret them, and most platforms don't fill them in.
	 *
	 * 我们在这里不显示的唯一统计数据是 ixrss、idrss、isrss。解释它们需要一些工作，而且大多数平台不会填写它们。
	 */
	initStringInfo(&str);

	appendStringInfoString(&str, "! system usage stats:\n");
	appendStringInfo(&str,
					 "!\t%ld.%06ld s user, %ld.%06ld s system, %ld.%06ld s elapsed\n",
					 (long) (r.ru_utime.tv_sec - Save_r.ru_utime.tv_sec),
					 (long) (r.ru_utime.tv_usec - Save_r.ru_utime.tv_usec),
					 (long) (r.ru_stime.tv_sec - Save_r.ru_stime.tv_sec),
					 (long) (r.ru_stime.tv_usec - Save_r.ru_stime.tv_usec),
					 (long) (elapse_t.tv_sec - Save_t.tv_sec),
					 (long) (elapse_t.tv_usec - Save_t.tv_usec));
	appendStringInfo(&str,
					 "!\t[%ld.%06ld s user, %ld.%06ld s system total]\n",
					 (long) user.tv_sec,
					 (long) user.tv_usec,
					 (long) sys.tv_sec,
					 (long) sys.tv_usec);
#ifndef WIN32

	/*
	 * The following rusage fields are not defined by POSIX, but they're
	 * present on all current Unix-like systems so we use them without any
	 * special checks.  Some of these could be provided in our Windows
	 * emulation in src/port/win32getrusage.c with more work.
	 *
	 * 以下 rusage 字段不是由 POSIX 定义的，但它们存在于所有当前的类 Unix
	 * 系统上，因此我们使用它们时无需任何特殊检查。其中一些可以通过更多工作在 src/port/win32getrusage.c 中的
	 * Windows 模拟中提供。
在 macOS 上
	 */
	appendStringInfo(&str,
					 "!\t%ld kB max resident size\n",
#if defined(__darwin__)
	/* in bytes on macOS
	 *
	 * （以字节为单位）
大多数其他平台上
	 */
					 r.ru_maxrss / 1024
#else
	/* in kilobytes on most other platforms
	 *
	 * 以千字节为单位
	 */
					 r.ru_maxrss
#endif
		);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] filesystem blocks in/out\n",
					 r.ru_inblock - Save_r.ru_inblock,
	/* they only drink coffee at dec
	 *
	 * 他们只在十二月喝咖啡
	 */
					 r.ru_oublock - Save_r.ru_oublock,
					 r.ru_inblock, r.ru_oublock);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] page faults/reclaims, %ld [%ld] swaps\n",
					 r.ru_majflt - Save_r.ru_majflt,
					 r.ru_minflt - Save_r.ru_minflt,
					 r.ru_majflt, r.ru_minflt,
					 r.ru_nswap - Save_r.ru_nswap,
					 r.ru_nswap);
	appendStringInfo(&str,
					 "!\t%ld [%ld] signals rcvd, %ld/%ld [%ld/%ld] messages rcvd/sent\n",
					 r.ru_nsignals - Save_r.ru_nsignals,
					 r.ru_nsignals,
					 r.ru_msgrcv - Save_r.ru_msgrcv,
					 r.ru_msgsnd - Save_r.ru_msgsnd,
					 r.ru_msgrcv, r.ru_msgsnd);
	appendStringInfo(&str,
					 "!\t%ld/%ld [%ld/%ld] voluntary/involuntary context switches\n",
					 r.ru_nvcsw - Save_r.ru_nvcsw,
					 r.ru_nivcsw - Save_r.ru_nivcsw,
					 r.ru_nvcsw, r.ru_nivcsw);
#endif							/* !WIN32 */

	/* remove trailing newline
	 *
	 * 删除尾随换行符
	 */
	if (str.data[str.len - 1] == '\n')
		str.data[--str.len] = '\0';

	ereport(LOG,
			(errmsg_internal("%s", title),
			 errdetail_internal("%s", str.data)));

	pfree(str.data);
}

/*
 * on_proc_exit handler to log end of session
 *
 * on_proc_exit 处理程序用于记录会话结束
 */
static void
log_disconnections(int code, Datum arg)
{
	Port	   *port = MyProcPort;
	long		secs;
	int			usecs;
	int			msecs;
	int			hours,
				minutes,
				seconds;

	TimestampDifference(MyStartTimestamp,
						GetCurrentTimestamp(),
						&secs, &usecs);
	msecs = usecs / 1000;

	hours = secs / SECS_PER_HOUR;
	secs %= SECS_PER_HOUR;
	minutes = secs / SECS_PER_MINUTE;
	seconds = secs % SECS_PER_MINUTE;

	ereport(LOG,
			(errmsg("disconnection: session time: %d:%02d:%02d.%03d "
					"user=%s database=%s host=%s%s%s",
					hours, minutes, seconds, msecs,
					port->user_name, port->database_name, port->remote_host,
					port->remote_port[0] ? " port=" : "", port->remote_port)));
}

/*
 * Start statement timeout timer, if enabled.
 *
 * 启动语句超时计时器（如果启用）。
 *
 * If there's already a timeout running, don't restart the timer.  That
 * enables compromises between accuracy of timeouts and cost of starting a
 * timeout.
 *
 * 如果已经超时，则不要重新启动计时器。这可以在超时的准确性和启动超时的成本之间进行折衷。
 */
static void
enable_statement_timeout(void)
{
	/* must be within an xact
	 *
	 * 必须在 xact 内
	 */
	Assert(xact_started);

	if (StatementTimeout > 0
		&& (StatementTimeout < TransactionTimeout || TransactionTimeout == 0))
	{
		if (!get_timeout_active(STATEMENT_TIMEOUT))
			enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout);
	}
	else
	{
		if (get_timeout_active(STATEMENT_TIMEOUT))
			disable_timeout(STATEMENT_TIMEOUT, false);
	}
}

/*
 * Disable statement timeout, if active.
 *
 * 禁用语句超时（如果处于活动状态）。
 */
static void
disable_statement_timeout(void)
{
	if (get_timeout_active(STATEMENT_TIMEOUT))
		disable_timeout(STATEMENT_TIMEOUT, false);
}
