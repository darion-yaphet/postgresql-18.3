/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  This program acts as a clearing house for requests to the
 *	  POSTGRES system.  Frontend programs connect to the Postmaster,
 *	  and postmaster forks a new backend process to handle the
 *	  connection.
 *
 *	  此程序充当 POSTGRES 系统请求的结算中心。前端程序连接到 
 *	  Postmaster，postmaster 分配（fork）一个新的后端进程来处理连接。
 *
 *	  The postmaster also manages system-wide operations such as
 *	  startup and shutdown. The postmaster itself doesn't do those
 *	  operations, mind you --- it just forks off a subprocess to do them
 *	  at the right times.  It also takes care of resetting the system
 *	  if a backend crashes.
 *
 *	  Postmaster 还管理全系统的操作，例如启动和关闭。请注意，
 *	  Postmaster 本身并不执行这些操作 —— 它只是在适当的时间分配
 *	  出一个子进程来执行。如果后端崩溃，它还负责重置系统。
 *
 *	  The postmaster process creates the shared memory and semaphore
 *	  pools during startup, but as a rule does not touch them itself.
 *	  In particular, it is not a member of the PGPROC array of backends
 *	  and so it cannot participate in lock-manager operations.  Keeping
 *	  the postmaster away from shared memory operations makes it simpler
 *	  and more reliable.  The postmaster is almost always able to recover
 *	  from crashes of individual backends by resetting shared memory;
 *	  if it did much with shared memory then it would be prone to crashing
 *	  along with the backends.
 *
 *	  Postmaster 进程在启动期间创建共享内存和信号量池，但通常自己
 *	  不接触它们。特别地，它不是后端 PGPROC 数组的成员，因此不能
 *	  参与锁管理器操作。使 Postmaster 远离共享内存操作使其更简单、
 *	  更可靠。Postmaster 几乎总是能够通过重置共享内存从单个后端的
 *	  崩溃中恢复；如果它对共享内存做太多操作，那么它就容易随后端一
 *	  起崩溃。
 *
 *	  When a request message is received, we now fork() immediately.
 *	  The child process performs authentication of the request, and
 *	  then becomes a backend if successful.  This allows the auth code
 *	  to be written in a simple single-threaded style (as opposed to the
 *	  crufty "poor man's multitasking" code that used to be needed).
 *	  More importantly, it ensures that blockages in non-multithreaded
 *	  libraries like SSL or PAM cannot cause denial of service to other
 *	  clients.
 *
 *	  当收到请求消息时，我们现在立即 fork()。子进程对请求进行身份验证，
 *	  如果成功则成为后端。这允许以简单的单线程方式编写身份验证代码
 *	  （而不是以前需要的粗糙的“穷人的多任务”代码）。更重要的是，
 *	  它确保 SSL 或 PAM 等非多线程库中的阻塞不会导致对其他客户端的
 *	  拒绝服务。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/postmaster.c
 *
 * NOTES
 *
 * Initialization:
 *		The Postmaster sets up shared memory data structures
 *		for the backends.
 *
 * 初始化：
 *		Postmaster 为后端设置共享内存数据结构。
 *
 * Synchronization:
 *		The Postmaster shares memory with the backends but should avoid
 *		touching shared memory, so as not to become stuck if a crashing
 *		backend screws up locks or shared memory.  Likewise, the Postmaster
 *		should never block on messages from frontend clients.
 *
 * 同步：
 *		Postmaster 与后端共享内存，但应避免接触共享内存，以免在后端崩溃
 *		搞坏锁或共享内存时陷入停滞。同样，Postmaster 永远不应阻塞在来自
 *		前端客户端的消息上。
 *
 * Garbage Collection:
 *		The Postmaster cleans up after backends if they have an emergency
 *		exit and/or core dump.
 *
 * 垃圾回收：
 *		如果后端发生紧急退出和/或核心转储，Postmaster 会在后端之后进行清理。
 *
 * Error Reporting:
 *		Use write_stderr() only for reporting "interactive" errors
 *		(essentially, bogus arguments on the command line).  Once the
 *		postmaster is launched, use ereport().
 *
 * 错误报告：
 *		仅使用 write_stderr() 报告“交互式”错误（本质上是命令行上的虚假参数）。
 *		一旦 postmaster 启动，请使用 ereport()。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netdb.h>
#include <limits.h>

#ifdef USE_BONJOUR
#include <dns_sd.h>
#endif

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifdef HAVE_PTHREAD_IS_THREADED_NP
#include <pthread.h>
#endif

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "lib/ilist.h"
#include "libpq/libpq.h"
#include "libpq/pqsignal.h"
#include "pg_getopt.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "replication/logicallauncher.h"
#include "replication/slotsync.h"
#include "replication/walsender.h"
#include "storage/aio_subsys.h"
#include "storage/fd.h"
#include "storage/io_worker.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#ifdef EXEC_BACKEND
#include "common/file_utils.h"
#include "storage/pg_shmem.h"
#endif


/*
 * CountChildren and SignalChildren take a bitmask argument to represent
 * BackendTypes to count or signal.  Define a separate type and functions to
 * work with the bitmasks, to avoid accidentally passing a plain BackendType
 * in place of a bitmask or vice versa.
 *
 * CountChildren 和 SignalChildren 采用位掩码参数来表示要计数或发送信号的
 * BackendType。定义一个单独的类型和函数来处理位掩码，以避免意外地将普通的 
 * BackendType 传递到位掩码的位置，反之亦然。
 */
typedef struct
{
	uint32		mask;
} BackendTypeMask;

StaticAssertDecl(BACKEND_NUM_TYPES < 32, "too many backend types for uint32");

static const BackendTypeMask BTYPE_MASK_ALL = {(1 << BACKEND_NUM_TYPES) - 1};
static const BackendTypeMask BTYPE_MASK_NONE = {0};

static inline BackendTypeMask
btmask(BackendType t)
{
	BackendTypeMask mask = {.mask = 1 << t};

	return mask;
}

static inline BackendTypeMask
btmask_add_n(BackendTypeMask mask, int nargs, BackendType *t)
{
	for (int i = 0; i < nargs; i++)
		mask.mask |= 1 << t[i];
	return mask;
}

#define btmask_add(mask, ...) \
	btmask_add_n(mask, \
		lengthof(((BackendType[]){__VA_ARGS__})), \
		(BackendType[]){__VA_ARGS__} \
	)

static inline BackendTypeMask
btmask_del(BackendTypeMask mask, BackendType t)
{
	mask.mask &= ~(1 << t);
	return mask;
}

static inline BackendTypeMask
btmask_all_except_n(int nargs, BackendType *t)
{
	BackendTypeMask mask = BTYPE_MASK_ALL;

	for (int i = 0; i < nargs; i++)
		mask = btmask_del(mask, t[i]);
	return mask;
}

#define btmask_all_except(...) \
	btmask_all_except_n( \
		lengthof(((BackendType[]){__VA_ARGS__})), \
		(BackendType[]){__VA_ARGS__} \
	)

static inline bool
btmask_contains(BackendTypeMask mask, BackendType t)
{
	return (mask.mask & (1 << t)) != 0;
}


BackgroundWorker *MyBgworkerEntry = NULL;

/* The socket number we are listening for connections on */
/* 我们正在监听连接的套接字编号 */
int			PostPortNumber = DEF_PGPORT;

/* The directory names for Unix socket(s) */
/* Unix 套接字的目录名 */
char	   *Unix_socket_directories;

/* The TCP listen address(es) */
/* TCP 监听地址 */
char	   *ListenAddresses;

/*
 * SuperuserReservedConnections is the number of backends reserved for
 * superuser use, and ReservedConnections is the number of backends reserved
 * for use by roles with privileges of the pg_use_reserved_connections
 * predefined role.  These are taken out of the pool of MaxConnections backend
 * slots, so the number of backend slots available for roles that are neither
 * superuser nor have privileges of pg_use_reserved_connections is
 * (MaxConnections - SuperuserReservedConnections - ReservedConnections).
 *
 * SuperuserReservedConnections 是为超级用户预留的后端数量，ReservedConnections 
 * 是为具有 pg_use_reserved_connections 预定义角色权限的角色预留的后端数量。
 * 这些是从 MaxConnections 后端插槽池中提取的，因此既不是超级用户也没有 
 * pg_use_reserved_connections 权限的角色可用的后端插槽数量为 
 * (MaxConnections - SuperuserReservedConnections - ReservedConnections)。
 *
 * If the number of remaining slots is less than or equal to
 * SuperuserReservedConnections, only superusers can make new connections.  If
 * the number of remaining slots is greater than SuperuserReservedConnections
 * but less than or equal to
 * (SuperuserReservedConnections + ReservedConnections), only superusers and
 * roles with privileges of pg_use_reserved_connections can make new
 * connections.  Note that pre-existing superuser and
 * pg_use_reserved_connections connections don't count against the limits.
 *
 * 如果剩余插槽的数量小于或等于 SuperuserReservedConnections，则只有超级用户
 * 可以建立新连接。如果剩余插槽的数量大于 SuperuserReservedConnections 
 * 但小于或等于 (SuperuserReservedConnections + ReservedConnections)，
 * 则只有超级用户和具有 pg_use_reserved_connections 权限的角色可以建立新连接。
 * 请注意，预先存在的超级用户和 pg_use_reserved_connections 连接不计入限制。
 */
int			SuperuserReservedConnections;
int			ReservedConnections;

/* The socket(s) we're listening to. */
/* 我们正在监听的套接字。 */
#define MAXLISTEN	64
static int	NumListenSockets = 0;
static pgsocket *ListenSockets = NULL;

/* still more option variables */
/* 还有更多选项变量 */
bool		EnableSSL = false;

int			PreAuthDelay = 0;
int			AuthenticationTimeout = 60;

bool		log_hostname;		/* for ps display and logging */
								/* 用于 ps 显示和日志记录 */

bool		enable_bonjour = false;
char	   *bonjour_name;
bool		restart_after_crash = true;
bool		remove_temp_files_after_crash = true;

/*
 * When terminating child processes after fatal errors, like a crash of a
 * child process, we normally send SIGQUIT -- and most other comments in this
 * file are written on the assumption that we do -- but developers might
 * prefer to use SIGABRT to collect per-child core dumps.
 *
 * 当在致命错误（如子进程崩溃）后终止子进程时，我们通常发送 SIGQUIT —— 
 * 并且此文件中的大多数其他注释都是在这一假设下编写的 —— 但开发人员可能
 * 更愿意使用 SIGABRT 来收集每个子进程的核心转储。
 */
bool		send_abort_for_crash = false;
bool		send_abort_for_kill = false;

/* special child processes; NULL when not running */
/* 特殊子进程；未运行时为 NULL */
static PMChild *StartupPMChild = NULL,
		   *BgWriterPMChild = NULL,
		   *CheckpointerPMChild = NULL,
		   *WalWriterPMChild = NULL,
		   *WalReceiverPMChild = NULL,
		   *WalSummarizerPMChild = NULL,
		   *AutoVacLauncherPMChild = NULL,
		   *PgArchPMChild = NULL,
		   *SysLoggerPMChild = NULL,
		   *SlotSyncWorkerPMChild = NULL;

/* Startup process's status */
/* 启动进程的状态 */
typedef enum
{
	STARTUP_NOT_RUNNING,
	STARTUP_RUNNING,
	STARTUP_SIGNALED,			/* we sent it a SIGQUIT or SIGKILL */
	STARTUP_CRASHED,
} StartupStatusEnum;

static StartupStatusEnum StartupStatus = STARTUP_NOT_RUNNING;

/* Startup/shutdown state */
/* 启动/关闭状态 */
#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2
#define			ImmediateShutdown	3

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* T if recovering from backend crash */

/*
 * We use a simple state machine to control startup, shutdown, and
 * crash recovery (which is rather like shutdown followed by startup).
 *
 * 我们使用一个简单的状态机来控制启动、关闭和崩溃恢复（这相当于是关闭后紧接
 * 着启动）。
 *
 * After doing all the postmaster initialization work, we enter PM_STARTUP
 * state and the startup process is launched. The startup process begins by
 * reading the control file and other preliminary initialization steps.
 * In a normal startup, or after crash recovery, the startup process exits
 * with exit code 0 and we switch to PM_RUN state.  However, archive recovery
 * is handled specially since it takes much longer and we would like to support
 * hot standby during archive recovery.
 *
 * 在完成所有 postmaster 初始化工作后，我们进入 PM_STARTUP 状态并启动
 * 启动进程。启动进程首先读取控制文件并执行其他初步初始化步骤。在正常启动
 * 或崩溃恢复后，启动进程以退出代码 0 退出，我们切换到 PM_RUN 状态。
 * 但是，归档恢复被特别处理，因为它耗时更长，并且我们希望在归档恢复期间
 * 支持热备（Hot Standby）。
 *
 * When the startup process is ready to start archive recovery, it signals the
 * postmaster, and we switch to PM_RECOVERY state. The background writer and
 * checkpointer are launched, while the startup process continues applying WAL.
 * If Hot Standby is enabled, then, after reaching a consistent point in WAL
 * redo, startup process signals us again, and we switch to PM_HOT_STANDBY
 * state and begin accepting connections to perform read-only queries.  When
 * archive recovery is finished, the startup process exits with exit code 0
 * and we switch to PM_RUN state.
 *
 * 当启动进程准备好开始归档恢复时，它会通知 postmaster，我们切换到 
 * PM_RECOVERY 状态。启动后台写入器（background writer）和检查点进程
 * （checkpointer），同时启动进程继续应用 WAL。如果启用了热备，那么在
 * 达到 WAL 重做的一致点后，启动进程会再次通知我们，我们切换到 
 * PM_HOT_STANDBY 状态并开始接受连接以执行只读查询。当归档恢复完成时，
 * 启动进程以退出代码 0 退出，我们切换到 PM_RUN 状态。
 *
 * Normal child backends can only be launched when we are in PM_RUN or
 * PM_HOT_STANDBY state.  (connsAllowed can also restrict launching.)
 * In other states we handle connection requests by launching "dead-end"
 * child processes, which will simply send the client an error message and
 * quit.  (We track these in the ActiveChildList so that we can know when they
 * are all gone; this is important because they're still connected to shared
 * memory, and would interfere with an attempt to destroy the shmem segment,
 * possibly leading to SHMALL failure when we try to make a new one.)
 * In PM_WAIT_DEAD_END state we are waiting for all the dead-end children
 * to drain out of the system, and therefore stop accepting connection
 * requests at all until the last existing child has quit (which hopefully
 * will not be very long).
 *
 * 正常的子后端只有在我们处于 PM_RUN 或 PM_HOT_STANDBY 状态时才能启动。
 * （connsAllowed 也可以限制启动。）在其他状态下，我们通过启动“死路
 * （dead-end）”子进程来处理连接请求，这些进程只是向客户端发送错误消息
 * 然后退出。（我们在 ActiveChildList 中跟踪这些进程，以便知道它们何时
 * 全部消失；这很重要，因为它们仍然连接到共享内存，并且会干扰销毁共享内存
 * 段的尝试，当尝试创建新段时可能导致 SHMALL 失败。）在 PM_WAIT_DEAD_END 
 * 状态下，我们正在等待所有死路子进程从系统中排出，因此在最后一个现有子
 * 进程退出之前完全停止接受连接请求（希望这不会太久）。
 *
 * Notice that this state variable does not distinguish *why* we entered
 * states later than PM_RUN --- Shutdown and FatalError must be consulted
 * to find that out.  FatalError is never true in PM_RECOVERY, PM_HOT_STANDBY,
 * or PM_RUN states, nor in PM_WAIT_XLOG_SHUTDOWN states (because we don't
 * enter those states when trying to recover from a crash).  It can be true in
 * PM_STARTUP state, because we don't clear it until we've successfully
 * started WAL redo.
 *
 * 请注意，此状态变量不区分我们进入 PM_RUN 之后的状态的“原因” —— 必须
 * 咨询 Shutdown 和 FatalError 才能查明原因。FatalError 在 PM_RECOVERY、
 * PM_HOT_STANDBY 或 PM_RUN 状态下永远不会为真，在 PM_WAIT_XLOG_SHUTDOWN 
 * 状态下也不会为真（因为我们在尝试从崩溃中恢复时不会进入这些状态）。它在 
 * PM_STARTUP 状态下可能为真，因为在成功开始 WAL 重做之前我们不会清除它。
 */
typedef enum
{
	PM_INIT,					/* postmaster starting */
	PM_STARTUP,					/* waiting for startup subprocess */
	PM_RECOVERY,				/* in archive recovery mode */
	PM_HOT_STANDBY,				/* in hot standby mode */
	PM_RUN,						/* normal "database is alive" state */
	PM_STOP_BACKENDS,			/* need to stop remaining backends */
	PM_WAIT_BACKENDS,			/* waiting for live backends to exit */
	PM_WAIT_XLOG_SHUTDOWN,		/* waiting for checkpointer to do shutdown
								 * ckpt */
								/* 等待检查点进程执行关闭检查点 */
	PM_WAIT_XLOG_ARCHIVAL,		/* waiting for archiver and walsenders to
								 * finish */
								/* 等待归档器和 walsender 完成 */
	PM_WAIT_IO_WORKERS,			/* waiting for io workers to exit */
	PM_WAIT_CHECKPOINTER,		/* waiting for checkpointer to shut down */
	PM_WAIT_DEAD_END,			/* waiting for dead-end children to exit */
	PM_NO_CHILDREN,				/* all important children have exited */
} PMState;

static PMState pmState = PM_INIT;

/*
 * While performing a "smart shutdown", we restrict new connections but stay
 * in PM_RUN or PM_HOT_STANDBY state until all the client backends are gone.
 * connsAllowed is a sub-state indicator showing the active restriction.
 * It is of no interest unless pmState is PM_RUN or PM_HOT_STANDBY.
 *
 * 在执行“智能关闭（smart shutdown）”期间，我们限制新连接，但保持在 
 * PM_RUN 或 PM_HOT_STANDBY 状态，直到所有客户端后端都消失。
 * connsAllowed 是一个子状态指示器，显示当前的限制。除非 pmState 
 * 是 PM_RUN 或 PM_HOT_STANDBY，否则它没有任何意义。
 */
static bool connsAllowed = true;

/* Start time of SIGKILL timeout during immediate shutdown or child crash */
/* Zero means timeout is not running */
static time_t AbortStartTime = 0;

/* Length of said timeout */
#define SIGKILL_CHILDREN_AFTER_SECS		5

static bool ReachedNormalRunning = false;	/* T if we've reached PM_RUN */

bool		ClientAuthInProgress = false;	/* T during new-client
											 * authentication */
											/* 新客户端身份验证期间为真 */

bool		redirection_done = false;	/* stderr redirected for syslogger? */

/* received START_AUTOVAC_LAUNCHER signal */
static bool start_autovac_launcher = false;

/* the launcher needs to be signaled to communicate some condition */
static bool avlauncher_needs_signal = false;

/* received START_WALRECEIVER signal */
static bool WalReceiverRequested = false;

/* set when there's a worker that needs to be started up */
static bool StartWorkerNeeded = true;
static bool HaveCrashedWorker = false;

/* set when signals arrive */
static volatile sig_atomic_t pending_pm_pmsignal;
static volatile sig_atomic_t pending_pm_child_exit;
static volatile sig_atomic_t pending_pm_reload_request;
static volatile sig_atomic_t pending_pm_shutdown_request;
static volatile sig_atomic_t pending_pm_fast_shutdown_request;
static volatile sig_atomic_t pending_pm_immediate_shutdown_request;

/* event multiplexing object */
static WaitEventSet *pm_wait_set;

#ifdef USE_SSL
/* Set when and if SSL has been initialized properly */
bool		LoadedSSL = false;
#endif

#ifdef USE_BONJOUR
static DNSServiceRef bonjour_sdref = NULL;
#endif

/* State for IO worker management. */
static int	io_worker_count = 0;
static PMChild *io_worker_children[MAX_IO_WORKERS];

/*
 * postmaster.c - function prototypes
 * postmaster.c - 函数原型
 */
static void CloseServerPorts(int status, Datum arg);
static void unlink_external_pid_file(int status, Datum arg);
static void getInstallationPaths(const char *argv0);
static void checkControlFile(void);
static void handle_pm_pmsignal_signal(SIGNAL_ARGS);
static void handle_pm_child_exit_signal(SIGNAL_ARGS);
static void handle_pm_reload_request_signal(SIGNAL_ARGS);
static void handle_pm_shutdown_request_signal(SIGNAL_ARGS);
static void process_pm_pmsignal(void);
static void process_pm_child_exit(void);
static void process_pm_reload_request(void);
static void process_pm_shutdown_request(void);
static void dummy_handler(SIGNAL_ARGS);
static void CleanupBackend(PMChild *bp, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname,
						 int pid, int exitstatus);
static void PostmasterStateMachine(void);
static void UpdatePMState(PMState newState);

pg_noreturn static void ExitPostmaster(int status);
static int	ServerLoop(void);
static int	BackendStartup(ClientSocket *client_sock);
static void report_fork_failure_to_client(ClientSocket *client_sock, int errnum);
static CAC_state canAcceptConnections(BackendType backend_type);
static void signal_child(PMChild *pmchild, int signal);
static bool SignalChildren(int signal, BackendTypeMask targetMask);
static void TerminateChildren(int signal);
static int	CountChildren(BackendTypeMask targetMask);
static void LaunchMissingBackgroundProcesses(void);
static void maybe_start_bgworkers(void);
static bool maybe_reap_io_worker(int pid);
static void maybe_adjust_io_workers(void);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static PMChild *StartChildProcess(BackendType type);
static void StartSysLogger(void);
static void StartAutovacuumWorker(void);
static bool StartBackgroundWorker(RegisteredBgWorker *rw);
static void InitPostmasterDeathWatchHandle(void);

#ifdef WIN32
#define WNOHANG 0				/* ignored, so any integer value will do */

static pid_t waitpid(pid_t pid, int *exitstatus, int options);
static void WINAPI pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired);

static HANDLE win32ChildQueue;

typedef struct
{
	HANDLE		waitHandle;
	HANDLE		procHandle;
	DWORD		procId;
} win32_deadchild_waitinfo;
#endif							/* WIN32 */

/* Macros to check exit status of a child process */
#define EXIT_STATUS_0(st)  ((st) == 0)
#define EXIT_STATUS_1(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 1)
#define EXIT_STATUS_3(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 3)

#ifndef WIN32
/*
 * File descriptors for pipe used to monitor if postmaster is alive.
 * First is POSTMASTER_FD_WATCH, second is POSTMASTER_FD_OWN.
 *
 * 用于监视 postmaster 是否存活的管道的文件描述符。
 * 第一个是 POSTMASTER_FD_WATCH，第二个是 POSTMASTER_FD_OWN。
 */
int			postmaster_alive_fds[2] = {-1, -1};
#else
/* Process handle of postmaster used for the same purpose on Windows */
/* 在 Windows 上用于相同目的的 postmaster 进程句柄 */
HANDLE		PostmasterHandle;
#endif

/*
 * Postmaster main entry point
 * Postmaster 主入口点
 */
void
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char	   *userDoption = NULL;
	bool		listen_addr_saved = false;
	char	   *output_config_variable = NULL;

	InitProcessGlobals();

	PostmasterPid = MyProcPid;

	IsPostmasterEnvironment = true;

	/*
	 * Start our win32 signal implementation
	 * 启动我们的 win32 信号实现
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	/*
	 * We should not be creating any files or directories before we check the
	 * data directory (see checkDataDir()), but just in case set the umask to
	 * the most restrictive (owner-only) permissions.
	 *
	 * 在检查数据目录之前（见 checkDataDir()），我们不应该创建任何文件
	 * 或目录，但以防万一，将 umask 设置为最严格的（仅限所有者）权限。
	 *
	 * checkDataDir() will reset the umask based on the data directory
	 * permissions.
	 *
	 * checkDataDir() 将根据数据目录权限重置 umask。
	 */
	umask(PG_MODE_MASK_OWNER);

	/*
	 * By default, palloc() requests in the postmaster will be allocated in
	 * the PostmasterContext, which is space that can be recycled by backends.
	 * Allocated data that needs to be available to backends should be
	 * allocated in TopMemoryContext.
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(PostmasterContext);

	/* Initialize paths to installation files */
	getInstallationPaths(argv[0]);

	/*
	 * Set up signal handlers for the postmaster process.
	 *
	 * 为 postmaster 进程设置信号处理程序。
	 *
	 * CAUTION: when changing this list, check for side-effects on the signal
	 * handling setup of child processes.  See tcop/postgres.c,
	 * bootstrap/bootstrap.c, postmaster/bgwriter.c, postmaster/walwriter.c,
	 * postmaster/autovacuum.c, postmaster/pgarch.c, postmaster/syslogger.c,
	 * postmaster/bgworker.c and postmaster/checkpointer.c.
	 *
	 * 注意：更改此列表时，请检查对子进程信号处理设置的副作用。
	 * 请参阅 tcop/postgres.c、bootstrap/bootstrap.c、postmaster/bgwriter.c、
	 * postmaster/walwriter.c、postmaster/autovacuum.c、postmaster/pgarch.c、
	 * postmaster/syslogger.c、postmaster/bgworker.c 和 postmaster/checkpointer.c。
	 */
	pqinitmask();
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	pqsignal(SIGHUP, handle_pm_reload_request_signal);
	pqsignal(SIGINT, handle_pm_shutdown_request_signal);
	pqsignal(SIGQUIT, handle_pm_shutdown_request_signal);
	pqsignal(SIGTERM, handle_pm_shutdown_request_signal);
	pqsignal(SIGALRM, SIG_IGN); /* ignored */
	pqsignal(SIGPIPE, SIG_IGN); /* ignored */
	pqsignal(SIGUSR1, handle_pm_pmsignal_signal);
	pqsignal(SIGUSR2, dummy_handler);	/* unused, reserve for children */
	pqsignal(SIGCHLD, handle_pm_child_exit_signal);

	/* This may configure SIGURG, depending on platform. */
	/* 这可能会配置 SIGURG，具体取决于平台。 */
	InitializeWaitEventSupport();
	InitProcessLocalLatch();

	/*
	 * No other place in Postgres should touch SIGTTIN/SIGTTOU handling.  We
	 * ignore those signals in a postmaster environment, so that there is no
	 * risk of a child process freezing up due to writing to stderr.  But for
	 * a standalone backend, their default handling is reasonable.  Hence, all
	 * child processes should just allow the inherited settings to stand.
	 *
	 * Postgres 的其他任何地方都不应触及 SIGTTIN/SIGTTOU 处理。我们在 
	 * postmaster 环境中忽略这些信号，这样子进程就不会因为写入 stderr 
	 * 而面临冻结的风险。但对于独立后端，它们的默认处理是合理的。
	 * 因此，所有子进程都应该允许继承的设置保持不变。
	 */
#ifdef SIGTTIN
	pqsignal(SIGTTIN, SIG_IGN); /* ignored */
#endif
#ifdef SIGTTOU
	pqsignal(SIGTTOU, SIG_IGN); /* ignored */
#endif

	/* ignore SIGXFSZ, so that ulimit violations work like disk full */
	/* 忽略 SIGXFSZ，以便 ulimit 冲突的作用类似于磁盘已满 */
#ifdef SIGXFSZ
	pqsignal(SIGXFSZ, SIG_IGN); /* ignored */
#endif

	/* Begin accepting signals. */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * Options setup
	 */
	InitializeGUCOptions();

	opterr = 1;

	/*
	 * Parse command-line options.  CAUTION: keep this in sync with
	 * tcop/postgres.c (the option sets should not conflict) and with the
	 * common help() function in main/main.c.
	 *
	 * 解析命令行选项。注意：保持这与 tcop/postgres.c（选项集不应冲突）
	 * 以及 main/main.c 中的通用 help() 函数同步。
	 */
	while ((opt = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:OPp:r:S:sTt:W:-:")) != -1)
	{
		switch (opt)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'b':
				/* Undocumented flag used for binary upgrades */
				IsBinaryUpgrade = true;
				break;

			case 'C':
				output_config_variable = strdup(optarg);
				break;

			case '-':

				/*
				 * Error if the user misplaced a special must-be-first option
				 * for dispatching to a subprogram.  parse_dispatch_option()
				 * returns DISPATCH_POSTMASTER if it doesn't find a match, so
				 * error for anything else.
				 *
				 * 如果用户错放了用于分发到子程序的特殊的“必须排在首位”选项，则报错。
				 * 如果找不到匹配项，parse_dispatch_option() 返回 
				 * DISPATCH_POSTMASTER，因此对于其他任何内容都报错。
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
						if (opt == '-')
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

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'E':
				SetConfigOption("log_statement", "all", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, PGC_POSTMASTER, PGC_S_ARGV))
				{
					write_stderr("%s: invalid argument for option -f: \"%s\"\n",
								 progname, optarg);
					ExitPostmaster(1);
				}
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'j':
				/* only used by interactive backend */
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'l':
				SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'p':
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'r':
				/* only used by single-user backend */
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'T':

				/*
				 * This option used to be defined as sending SIGSTOP after a
				 * backend crash, but sending SIGABRT seems more useful.
				 *
				 * 此选项过去被定义为在后端崩溃后发送 SIGSTOP，但发送 
				 * SIGABRT 似乎更有用。
				 */
				SetConfigOption("send_abort_for_crash", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
					{
						SetConfigOption(tmp, "true", PGC_POSTMASTER, PGC_S_ARGV);
					}
					else
					{
						write_stderr("%s: invalid argument for option -t: \"%s\"\n",
									 progname, optarg);
						ExitPostmaster(1);
					}
					break;
				}

			case 'W':
				SetConfigOption("post_auth_delay", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster accepts no non-option switch arguments.
	 */
	if (optind < argc)
	{
		write_stderr("%s: invalid argument: \"%s\"\n",
					 progname, argv[optind]);
		write_stderr("Try \"%s --help\" for more information.\n",
					 progname);
		ExitPostmaster(1);
	}

	/*
	 * Locate the proper configuration files and data directory, and read
	 * postgresql.conf for the first time.
	 *
	 * 找到合适的配置文件和数据目录，并第一次读取 postgresql.conf。
	 */
	if (!SelectConfigFiles(userDoption, progname))
		ExitPostmaster(2);

	if (output_config_variable != NULL)
	{
		/*
		 * If this is a runtime-computed GUC, it hasn't yet been initialized,
		 * and the present value is not useful.  However, this is a convenient
		 * place to print the value for most GUCs because it is safe to run
		 * postmaster startup to this point even if the server is already
		 * running.  For the handful of runtime-computed GUCs that we cannot
		 * provide meaningful values for yet, we wait until later in
		 * postmaster startup to print the value.  We won't be able to use -C
		 * on running servers for those GUCs, but using this option now would
		 * lead to incorrect results for them.
		 *
		 * 如果这是一个在运行时计算的 GUC，它尚未初始化，且当前值没有用。
		 * 但是，对于大多数 GUC 来说，这是一个打印值的方便位置，因为即使服务器
		 * 已经在运行， postmaster 启动到这一点也是安全的。对于少数我们目前无法
		 * 提供有意义值的运行时计算 GUC，我们等到 postmaster 启动后期再打印值。
		 * 我们将无法在运行中的服务器上对那些 GUC 使用 -C，但现在使用此选项会导致
		 * 它们得到不正确的结果。
		 */
		int			flags = GetConfigOptionFlags(output_config_variable, true);

		if ((flags & GUC_RUNTIME_COMPUTED) == 0)
		{
			/*
			 * "-C guc" was specified, so print GUC's value and exit.  No
			 * extra permission check is needed because the user is reading
			 * inside the data dir.
			 *
			 * 指定了 "-C guc"，因此打印 GUC 的值并退出。不需要额外的权限检查，
			 * 因为用户是在数据目录内部读取。
			 */
			const char *config_val = GetConfigOption(output_config_variable,
													 false, false);

			puts(config_val ? config_val : "");
			ExitPostmaster(0);
		}

		/*
		 * A runtime-computed GUC will be printed later on.  As we initialize
		 * a server startup sequence, silence any log messages that may show
		 * up in the output generated.  FATAL and more severe messages are
		 * useful to show, even if one would only expect at least PANIC.  LOG
		 * entries are hidden.
		 *
		 * 运行时计算的 GUC 将在稍后打印。当我们初始化服务器启动顺序时，
		 * 静默生成的输出中可能出现的任何日志消息。即使只期望至少有 PANIC，
		 * 显示 FATAL 及更严重的消息也是有用的。LOG 条目将被隐藏。
		 */
		SetConfigOption("log_min_messages", "FATAL", PGC_SUSET,
						PGC_S_OVERRIDE);
	}

	/* Verify that DataDir looks reasonable */
	/* 验证 DataDir 是否看起来合理 */
	checkDataDir();

	/* Check that pg_control exists */
	/* 检查 pg_control 是否存在 */
	checkControlFile();

	/* And switch working directory into it */
	/* 并将工作目录切换到其中 */
	ChangeToDataDir();

	/*
	 * Check for invalid combinations of GUC settings.
	 * 检查无效的 GUC 设置组合。
	 */
	if (SuperuserReservedConnections + ReservedConnections >= MaxConnections)
	{
		write_stderr("%s: \"superuser_reserved_connections\" (%d) plus \"reserved_connections\" (%d) must be less than \"max_connections\" (%d)\n",
					 progname,
					 SuperuserReservedConnections, ReservedConnections,
					 MaxConnections);
		ExitPostmaster(1);
	}
	if (XLogArchiveMode > ARCHIVE_MODE_OFF && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL archival cannot be enabled when \"wal_level\" is \"minimal\"")));
	if (max_wal_senders > 0 && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL streaming (\"max_wal_senders\" > 0) requires \"wal_level\" to be \"replica\" or \"logical\"")));
	if (summarize_wal && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL cannot be summarized when \"wal_level\" is \"minimal\"")));

	/*
	 * Other one-time internal sanity checks can go here, if they are fast.
	 * (Put any slow processing further down, after postmaster.pid creation.)
	 *
	 * 其他一次性的内部健壮性检查可以放在这里（如果执行速度快）。
	 * （将任何缓慢的处理放在更后面的 postmaster.pid 创建之后。）
	 */
	if (!CheckDateTokenTables())
	{
		write_stderr("%s: invalid datetoken tables, please fix\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * Now that we are done processing the postmaster arguments, reset
	 * getopt(3) library so that it will work correctly in subprocesses.
	 *
	 * 现在我们已经处理完了 postmaster 参数，重置 getopt(3) 库，
	 * 以便它在子进程中能正常工作。
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* some systems need this too */
#endif

	/* For debugging: display postmaster environment */
	/* 用于调试：显示 postmaster 环境 */
	if (message_level_is_interesting(DEBUG3))
	{
#if !defined(WIN32) || defined(_MSC_VER)
		extern char **environ;
#endif
		char	  **p;
		StringInfoData si;

		initStringInfo(&si);

		appendStringInfoString(&si, "initial environment dump:");
		for (p = environ; *p; ++p)
			appendStringInfo(&si, "\n%s", *p);

		ereport(DEBUG3, errmsg_internal("%s", si.data));
		pfree(si.data);
	}

	/*
	 * Create lockfile for data directory.
	 *
	 * 为数据目录创建锁文件。
	 *
	 * We want to do this before we try to grab the input sockets, because the
	 * data directory interlock is more reliable than the socket-file
	 * interlock (thanks to whoever decided to put socket files in /tmp :-().
	 * For the same reason, it's best to grab the TCP socket(s) before the
	 * Unix socket(s).
	 *
	 * 我们希望在尝试抓取输入套接字之前执行此操作，因为数据目录互锁比
	 * 套接字文件互锁更可靠（感谢那些决定将套接字文件放在 /tmp 中的人 :-()。
	 * 出于同样的原因，最好在 Unix 套接字之前抓取 TCP 套接字。
	 *
	 * Also note that this internally sets up the on_proc_exit function that
	 * is responsible for removing both data directory and socket lockfiles;
	 * so it must happen before opening sockets so that at exit, the socket
	 * lockfiles go away after CloseServerPorts runs.
	 *
	 * 还要注意，这在内部设置了 on_proc_exit 函数，该函数负责删除数据目录
	 * 和套接字锁文件；因此它必须在打开套接字之前发生，以便在退出时，
	 * 对应套接字的锁文件在 CloseServerPorts 运行后消失。
	 */
	CreateDataDirLockFile(true);

	/*
	 * Read the control file (for error checking and config info).
	 *
	 * 读取控制文件（用于错误检查和配置信息）。
	 *
	 * Since we verify the control file's CRC, this has a useful side effect
	 * on machines where we need a run-time test for CRC support instructions.
	 * The postmaster will do the test once at startup, and then its child
	 * processes will inherit the correct function pointer and not need to
	 * repeat the test.
	 *
	 * 由于我们验证了控制文件的 CRC，这在我们需要对 CRC 支持指令进行运行时
	 * 测试的机器上具有有益的副作用。Postmaster 将在启动时执行一次测试，
	 * 然后其子进程将继承正确的函数指针，无需重复测试。
	 */
	LocalProcessControlFile(false);

	/*
	 * Register the apply launcher.  It's probably a good idea to call this
	 * before any modules had a chance to take the background worker slots.
	 *
	 * 注册应用启动器（apply launcher）。在任何模块有机会占用后台工作进程
	 * 插槽之前调用它可能是一个好主意。
	 */
	ApplyLauncherRegister();

	/*
	 * process any libraries that should be preloaded at postmaster start
	 * 处理应在 postmaster 启动时预加载的任何库
	 */
	process_shared_preload_libraries();

	/*
	 * Initialize SSL library, if specified.
	 * 如果已指定，则初始化 SSL 库。
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		(void) secure_initialize(true);
		LoadedSSL = true;
	}
#endif

	/*
	 * Now that loadable modules have had their chance to alter any GUCs,
	 * calculate MaxBackends and initialize the machinery to track child
	 * processes.
	 *
	 * 现在可加载模块已经有机会更改任何 GUC，计算 MaxBackends 并初始化
	 * 跟踪子进程的机制。
	 */
	InitializeMaxBackends();
	InitPostmasterChildSlots();

	/*
	 * Calculate the size of the PGPROC fast-path lock arrays.
	 * 计算 PGPROC 快速路径锁数组的大小。
	 */
	InitializeFastPathLocks();

	/*
	 * Give preloaded libraries a chance to request additional shared memory.
	 * 给预加载库一个请求额外共享内存的机会。
	 */
	process_shmem_requests();

	/*
	 * Now that loadable modules have had their chance to request additional
	 * shared memory, determine the value of any runtime-computed GUCs that
	 * depend on the amount of shared memory required.
	 *
	 * 现在可加载模块已经有机会请求额外的共享内存，确定任何依赖于所需共享
	 * 内存量的运行时计算 GUC 的值。
	 */
	InitializeShmemGUCs();

	/*
	 * Now that modules have been loaded, we can process any custom resource
	 * managers specified in the wal_consistency_checking GUC.
	 *
	 * 现在模块已加载，我们可以处理 wal_consistency_checking GUC 中指定的
	 * 任何自定义资源管理器。
	 */
	InitializeWalConsistencyChecking();

	/*
	 * If -C was specified with a runtime-computed GUC, we held off printing
	 * the value earlier, as the GUC was not yet initialized.  We handle -C
	 * for most GUCs before we lock the data directory so that the option may
	 * be used on a running server.  However, a handful of GUCs are runtime-
	 * computed and do not have meaningful values until after locking the data
	 * directory, and we cannot safely calculate their values earlier on a
	 * running server.  At this point, such GUCs should be properly
	 * initialized, and we haven't yet set up shared memory, so this is a good
	 * time to handle the -C option for these special GUCs.
	 *
	 * 如果 -C 指定的是运行时计算的 GUC，由于 GUC 尚未初始化，我们之前推迟
	 * 了打印该值。对于大多数 GUC，我们在锁定数据目录之前处理 -C，以便可以在
	 * 运行中的服务器上使用该选项。但是，少数 GUC 是运行时计算的，在锁定数据目
	 * 录之后才具有意义的值，并且我们无法在运行中的服务器上提前安全地计算 
	 * 它们的值。到此为止，此类 GUC 应该已经正确初始化，且我们尚未设置共享内存， 
	 * 因此这是处理这些特殊 GUC 的 -C 选项的好时机。
	 */
	if (output_config_variable != NULL)
	{
		const char *config_val = GetConfigOption(output_config_variable,
												 false, false);

		puts(config_val ? config_val : "");
		ExitPostmaster(0);
	}

	/*
	 * Set up shared memory and semaphores.
	 * 设置共享内存和信号量。
	 *
	 * Note: if using SysV shmem and/or semas, each postmaster startup will
	 * normally choose the same IPC keys.  This helps ensure that we will
	 * clean up dead IPC objects if the postmaster crashes and is restarted.
	 *
	 * 注意：如果使用 SysV 共享内存和/或信号量，每次 postmaster 启动通常选择
	 * 相同的 IPC 键。这有助于确保在 postmaster 崩溃并重启时清理过期的 IPC 对象。
	 */
	CreateSharedMemoryAndSemaphores();

	/*
	 * Estimate number of openable files.  This must happen after setting up
	 * semaphores, because on some platforms semaphores count as open files.
	 */
	set_max_safe_fds();

	/*
	 * Initialize pipe (or process handle on Windows) that allows children to
	 * wake up from sleep on postmaster death.
	 *
	 * 初始化管道（或 Windows 上的进程句柄），允许子进程在 postmaster 死亡时
	 * 从睡眠中醒来。
	 */
	InitPostmasterDeathWatchHandle();

#ifdef WIN32

	/*
	 * Initialize I/O completion port used to deliver list of dead children.
	 */
	win32ChildQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (win32ChildQueue == NULL)
		ereport(FATAL,
				(errmsg("could not create I/O completion port for child queue")));
#endif

#ifdef EXEC_BACKEND
	/* Write out nondefault GUC settings for child processes to use */
	write_nondefault_variables(PGC_POSTMASTER);

	/*
	 * Clean out the temp directory used to transmit parameters to child
	 * processes (see internal_forkexec).  We must do this before launching
	 * any child processes, else we have a race condition: we could remove a
	 * parameter file before the child can read it.  It should be safe to do
	 * so now, because we verified earlier that there are no conflicting
	 * Postgres processes in this data directory.
	 */
	RemovePgTempFilesInDir(PG_TEMP_FILES_DIR, true, false);
#endif

	/*
	 * Forcibly remove the files signaling a standby promotion request.
	 * Otherwise, the existence of those files triggers a promotion too early,
	 * whether a user wants that or not.
	 *
	 * 强制删除表示备用库晋升（standby promotion）请求的文件。否则，无论用户
	 * 是否想要，这些文件的存在都会过早触发晋升。
	 *
	 * This removal of files is usually unnecessary because they can exist
	 * only during a few moments during a standby promotion. However there is
	 * a race condition: if pg_ctl promote is executed and creates the files
	 * during a promotion, the files can stay around even after the server is
	 * brought up to be the primary.  Then, if a new standby starts by using
	 * the backup taken from the new primary, the files can exist at server
	 * startup and must be removed in order to avoid an unexpected promotion.
	 *
	 * 删除这些文件通常是不必要的，因为它们仅在备用库晋升期间存在片刻。
	 * 但存在竞争条件：如果在晋升期间执行了 pg_ctl promote 并创建了文件，
	 * 即使服务器已提升为主要库（primary），该文件也可能保留下来。然后，
	 * 如果通过使用从新主库提取的备份启动了新的备库，则这些文件可能在服务器
	 * 启动时存在，必须将其删除以避免意外晋升。
	 *
	 * Note that promotion signal files need to be removed before the startup
	 * process is invoked. Because, after that, they can be used by
	 * postmaster's SIGUSR1 signal handler.
	 *
	 * 请注意，必须在调用启动进程之前删除晋升信号文件。因为在那之后，
	 * 它们可能会被 postmaster 的 SIGUSR1 信号处理程序使用。
	 */
	RemovePromoteSignalFiles();

	/* Do the same for logrotate signal file */
	/* 对日志轮转（logrotate）信号文件执行相同操作 */
	RemoveLogrotateSignalFiles();

	/* Remove any outdated file holding the current log filenames. */
	/* 删除任何包含当前日志文件名的过时文件。 */
	if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						LOG_METAINFO_DATAFILE)));

	/*
	 * If enabled, start up syslogger collection subprocess
	 * 如果启用，启动 syslogger 收集子进程
	 */
	if (Logging_collector)
		StartSysLogger();

	/*
	 * Reset whereToSendOutput from DestDebug (its starting state) to
	 * DestNone. This stops ereport from sending log messages to stderr unless
	 * Log_destination permits.  We don't do this until the postmaster is
	 * fully launched, since startup failures may as well be reported to
	 * stderr.
	 *
	 * 将 whereToSendOutput 从 DestDebug（其初始状态）重置为 DestNone。 
	 * 这将停止 ereport 向 stderr 发送日志消息，除非 Log_destination 允许。
	 * 在 postmaster 完全启动之前，我们不会执行此操作，因为启动失败也可能
	 * 需要报告给 stderr。
	 *
	 * If we are in fact disabling logging to stderr, first emit a log message
	 * saying so, to provide a breadcrumb trail for users who may not remember
	 * that their logging is configured to go somewhere else.
	 *
	 * 如果我们实际上禁用了 stderr 日志记录，请先发出一条说明此情况的
	 * 日志消息，为那些可能忘记日志配置到了其他地方的用户提供线索。
	 */
	if (!(Log_destination & LOG_DESTINATION_STDERR))
		ereport(LOG,
				(errmsg("ending log output to stderr"),
				 errhint("Future log output will go to log destination \"%s\".",
						 Log_destination_string)));

	whereToSendOutput = DestNone;

	/*
	 * Report server startup in log.  While we could emit this much earlier,
	 * it seems best to do so after starting the log collector, if we intend
	 * to use one.
	 *
	 * 在日志中报告服务器启动。虽然我们可以更早地发出此消息，但如果我们打算
	 * 使用日志收集器，最好在启动日志收集器之后再这样做。
	 */
	ereport(LOG,
			(errmsg("starting %s", PG_VERSION_STR)));

	/*
	 * Establish input sockets.
	 * 建立输入套接字。
	 *
	 * First set up an on_proc_exit function that's charged with closing the
	 * sockets again at postmaster shutdown.
	 * 首先设置一个 on_proc_exit 函数，它负责在 postmaster 关闭时再次
	 * 关闭套接字。
	 */
	ListenSockets = palloc(MAXLISTEN * sizeof(pgsocket));
	on_proc_exit(CloseServerPorts, 0);

	if (ListenAddresses)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* Need a modifiable copy of ListenAddresses */
		/* 需要一个可修改的 ListenAddresses 副本 */
		rawstring = pstrdup(ListenAddresses);

		/* Parse string into list of hostnames */
		/* 将字符串解析为主机名列表 */
		if (!SplitGUCList(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"listen_addresses")));
		}

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (strcmp(curhost, "*") == 0)
				status = ListenServerPort(AF_UNSPEC, NULL,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSockets,
										  &NumListenSockets,
										  MAXLISTEN);
			else
				status = ListenServerPort(AF_UNSPEC, curhost,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSockets,
										  &NumListenSockets,
										  MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* record the first successful host addr in lockfile */
				/* 在锁文件中记录第一个成功的主机地址 */
				if (!listen_addr_saved)
				{
					AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
					listen_addr_saved = true;
				}
			}
			else
				ereport(WARNING,
						(errmsg("could not create listen socket for \"%s\"",
								curhost)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any TCP/IP sockets")));

		list_free(elemlist);
		pfree(rawstring);
	}

#ifdef USE_BONJOUR
	/* Register for Bonjour only if we opened TCP socket(s) */
	/* 仅当我们打开了 TCP 套接字时才注册 Bonjour */
	if (enable_bonjour && NumListenSockets > 0)
	{
		DNSServiceErrorType err;

		/*
		 * We pass 0 for interface_index, which will result in registering on
		 * all "applicable" interfaces.  It's not entirely clear from the
		 * DNS-SD docs whether this would be appropriate if we have bound to
		 * just a subset of the available network interfaces.
		 */
		err = DNSServiceRegister(&bonjour_sdref,
								 0,
								 0,
								 bonjour_name,
								 "_postgresql._tcp.",
								 NULL,
								 NULL,
								 pg_hton16(PostPortNumber),
								 0,
								 NULL,
								 NULL,
								 NULL);
		if (err != kDNSServiceErr_NoError)
			ereport(LOG,
					(errmsg("DNSServiceRegister() failed: error code %ld",
							(long) err)));

		/*
		 * We don't bother to read the mDNS daemon's reply, and we expect that
		 * it will automatically terminate our registration when the socket is
		 * closed at postmaster termination.  So there's nothing more to be
		 * done here.  However, the bonjour_sdref is kept around so that
		 * forked children can close their copies of the socket.
		 *
		 * 我们懒得读 mDNS 守护进程的回复，并期望当套接字在 postmaster 
		 * 终止时关闭后，它会自动终止我们的注册。所以这里没别的事要做。
		 * 但是，bonjour_sdref 仍保留着，以便派生的子进程可以关闭其套接字副本。
		 */
	}
#endif

	if (Unix_socket_directories)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* Need a modifiable copy of Unix_socket_directories */
		/* 需要一个可修改的 Unix_socket_directories 副本 */
		rawstring = pstrdup(Unix_socket_directories);

		/* Parse string into list of directories */
		/* 将字符串解析为目录列表 */
		if (!SplitDirectoriesString(rawstring, ',', &elemlist))
		{
			/* syntax error in list */
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"unix_socket_directories")));
		}

		foreach(l, elemlist)
		{
			char	   *socketdir = (char *) lfirst(l);

			status = ListenServerPort(AF_UNIX, NULL,
									  (unsigned short) PostPortNumber,
									  socketdir,
									  ListenSockets,
									  &NumListenSockets,
									  MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* record the first successful Unix socket in lockfile */
				/* 在锁文件中记录第一个成功的 Unix 套接字 */
				if (success == 1)
					AddToDataDirLockFile(LOCK_FILE_LINE_SOCKET_DIR, socketdir);
			}
			else
				ereport(WARNING,
						(errmsg("could not create Unix-domain socket in directory \"%s\"",
								socketdir)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any Unix-domain sockets")));

		list_free_deep(elemlist);
		pfree(rawstring);
	}

	/*
	 * check that we have some socket to listen on
	 * 检查我们是否有监听套接字
	 */
	if (NumListenSockets == 0)
		ereport(FATAL,
				(errmsg("no socket created for listening")));

	/*
	 * If no valid TCP ports, write an empty line for listen address,
	 * indicating the Unix socket must be used.  Note that this line is not
	 * added to the lock file until there is a socket backing it.
	 *
	 * 如果没有有效的 TCP 端口，请为监听地址写入一个空行，表示必须使用 
	 * Unix 套接字。请注意，在有套接字支持之前，该行不会添加到锁文件中。
	 */
	if (!listen_addr_saved)
		AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, "");

	/*
	 * Record postmaster options.  We delay this till now to avoid recording
	 * bogus options (eg, unusable port number).
	 *
	 * 记录 postmaster 选项。我们推迟到此时是为了避免记录虚假选项
	 * （例如不可用的端口号）。
	 */
	if (!CreateOptsFile(argc, argv, my_exec_path))
		ExitPostmaster(1);

	/*
	 * Write the external PID file if requested
	 * 如果有请求，编写外部 PID 文件
	 */
	if (external_pid_file)
	{
		FILE	   *fpidfile = fopen(external_pid_file, "w");

		if (fpidfile)
		{
			fprintf(fpidfile, "%d\n", MyProcPid);
			fclose(fpidfile);

			/* Make PID file world readable */
			/* 使 PID 文件全系统可读 */
			if (chmod(external_pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
				write_stderr("%s: could not change permissions of external PID file \"%s\": %m\n",
							 progname, external_pid_file);
		}
		else
			write_stderr("%s: could not write external PID file \"%s\": %m\n",
						 progname, external_pid_file);

		on_proc_exit(unlink_external_pid_file, 0);
	}

	/*
	 * Remove old temporary files.  At this point there can be no other
	 * Postgres processes running in this directory, so this should be safe.
	 *
	 * 删除旧的临时文件。此时，此目录中不能运行其他 Postgres 进程，因此这应该是安全的。
	 */
	RemovePgTempFiles();

	/*
	 * Initialize the autovacuum subsystem (again, no process start yet)
	 * 初始化自动处理子系统（同样，尚未启动进程）
	 */
	autovac_init();

	/*
	 * Load configuration files for client authentication.
	 * 为客户端身份验证加载配置文件。
	 */
	if (!load_hba())
	{
		/*
		 * It makes no sense to continue if we fail to load the HBA file,
		 * since there is no way to connect to the database in this case.
		 *
		 * 如果加载 HBA 文件失败，继续运行就没有意义了，因为在这种情况下
		 * 无法连接到数据库。
		 */
		ereport(FATAL,
		/* translator: %s is a configuration file */
				(errmsg("could not load %s", HbaFileName)));
	}
	if (!load_ident())
	{
		/*
		 * We can start up without the IDENT file, although it means that you
		 * cannot log in using any of the authentication methods that need a
		 * user name mapping. load_ident() already logged the details of error
		 * to the log.
		 *
		 * 我们可以在没有 IDENT 文件的情况下启动，尽管这意味着你无法使用任何需要
		 * 用户名映射的身份验证方法登录。load_ident() 已经将错误的详细信息记录
		 * 在日志中。
		 */
	}

#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * On macOS, libintl replaces setlocale() with a version that calls
	 * CFLocaleCopyCurrent() when its second argument is "" and every relevant
	 * environment variable is unset or empty.  CFLocaleCopyCurrent() makes
	 * the process multithreaded.  The postmaster calls sigprocmask() and
	 * calls fork() without an immediate exec(), both of which have undefined
	 * behavior in a multithreaded program.  A multithreaded postmaster is the
	 * normal case on Windows, which offers neither fork() nor sigprocmask().
	 * Currently, macOS is the only platform having pthread_is_threaded_np(),
	 * so we need not worry whether this HINT is appropriate elsewhere.
	 *
	 * 在 macOS 上，libintl 将 setlocale() 替换为调用 CFLocaleCopyCurrent() 
	 * 的版本（当其第二个参数为 "" 且每个相关的环境变量都未设置或为空时）。
	 * CFLocaleCopyCurrent() 使进程成为多线程。Postmaster 调用 sigprocmask() 
	 * 并在没有立即执行 exec() 的情况下调用 fork()，这两者在多线程程序中
	 * 都有未定义的行为。多线程 Postmaster 是 Windows 上的正常情况，它既不
	 * 提供 fork() 也不提供 sigprocmask()。目前，macOS 是唯一具有 
	 * pthread_is_threaded_np() 的平台，因此我们无需担心该“提示”是否 
	 * 适用于其他地方。
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded during startup"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/*
	 * Remember postmaster startup time
	 * 记录 postmaster 启动时间
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * Report postmaster status in the postmaster.pid file, to allow pg_ctl to
	 * see what's happening.
	 *
	 * 在 postmaster.pid 文件中报告 postmaster 状态，以便 pg_ctl 查看发生了什么。
	 */
	AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STARTING);

	UpdatePMState(PM_STARTUP);

	/* Make sure we can perform I/O while starting up. */
	/* 确保我们在启动时可以执行 I/O。 */
	maybe_adjust_io_workers();

	/* Start bgwriter and checkpointer so they can help with recovery */
	/* 启动 bgwriter 和检查点进程，以便它们协助恢复 */
	if (CheckpointerPMChild == NULL)
		CheckpointerPMChild = StartChildProcess(B_CHECKPOINTER);
	if (BgWriterPMChild == NULL)
		BgWriterPMChild = StartChildProcess(B_BG_WRITER);

	/*
	 * We're ready to rock and roll...
	 * 我们准备好了……
	 */
	StartupPMChild = StartChildProcess(B_STARTUP);
	Assert(StartupPMChild != NULL);
	StartupStatus = STARTUP_RUNNING;

	/* Some workers may be scheduled to start now */
	/* 某些工作进程可能定于现在启动 */
	maybe_start_bgworkers();

	status = ServerLoop();

	/*
	 * ServerLoop probably shouldn't ever return, but if it does, close down.
	 *
	 * ServerLoop 可能永远不应该返回，但如果返回了，请将其关闭。
	 */
	ExitPostmaster(status != STATUS_OK);

	abort();					/* not reached */
}


/*
 * on_proc_exit callback to close server's listen sockets
 * 在进程退出时回调以关闭服务器的监听套接字
 */
static void
CloseServerPorts(int status, Datum arg)
{
	int			i;

	/*
	 * First, explicitly close all the socket FDs.  We used to just let this
	 * happen implicitly at postmaster exit, but it's better to close them
	 * before we remove the postmaster.pid lockfile; otherwise there's a race
	 * condition if a new postmaster wants to re-use the TCP port number.
	 *
	 * 首先，显式关闭所有套接字文件描述符（FD）。过去我们只是在 
	 * postmaster 退出时让这隐式发生，但在删除 postmaster.pid 锁文件之前
	 * 关闭它们更好；否则，如果新的 postmaster 想要重用 TCP 端口号，
	 * 就会出现竞争条件。
	 */
	for (i = 0; i < NumListenSockets; i++)
	{
		if (closesocket(ListenSockets[i]) != 0)
			elog(LOG, "could not close listen socket: %m");
	}
	NumListenSockets = 0;

	/*
	 * Next, remove any filesystem entries for Unix sockets.  To avoid race
	 * conditions against incoming postmasters, this must happen after closing
	 * the sockets and before removing lock files.
	 *
	 * 接下来，删除 Unix 套接字的任何文件系统条目。为了避免针对传入的 
	 * postmaster 的竞争条件，这必须在关闭套接字之后、删除锁文件之前进行。
	 */
	RemoveSocketFiles();

	/*
	 * We don't do anything about socket lock files here; those will be
	 * removed in a later on_proc_exit callback.
	 *
	 * 我们这里不对套接字锁文件执行任何操作；这些将在稍后的 
	 * on_proc_exit 回调中删除。
	 */
}

/*
 * on_proc_exit callback to delete external_pid_file
 * 在进程退出时回调以删除外部 PID 文件
 */
static void
unlink_external_pid_file(int status, Datum arg)
{
	if (external_pid_file)
		unlink(external_pid_file);
}


/*
 * Compute and check the directory paths to files that are part of the
 * installation (as deduced from the postgres executable's own location)
 * 计算并检查属于安装一部分的文件目录路径（根据 postgres 可执行文件本身的位置推断）
 */
static void
getInstallationPaths(const char *argv0)
{
	DIR		   *pdir;

	/* Locate the postgres executable itself */
	/* 定位 postgres 可执行文件本身 */
	if (find_my_exec(argv0, my_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate my own executable path", argv0)));

#ifdef EXEC_BACKEND
	/* Locate executable backend before we change working directory */
	/* 在我们更改工作目录之前定位可执行后端 */
	if (find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
						postgres_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate matching postgres executable",
						argv0)));
#endif

	/*
	 * Locate the pkglib directory --- this has to be set early in case we try
	 * to load any modules from it in response to postgresql.conf entries.
	 * 定位 pkglib 目录 —— 这必须尽早设置，以防我们尝试响应 postgresql.conf 
	 * 条目从中加载任何模块。
	 */
	get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * Verify that there's a readable directory there; otherwise the Postgres
	 * installation is incomplete or corrupt.  (A typical cause of this
	 * failure is that the postgres executable has been moved or hardlinked to
	 * some directory that's not a sibling of the installation lib/
	 * directory.)
	 *
	 * 验证那里是否有一个可读目录；否则 Postgres 安装是不完整的或损坏的。
	 * （导致此故障的典型原因是 postgres 可执行文件已被移动或硬链接到某个
	 * 不是安装 lib/ 目录的兄弟目录的目录中。）
	 */
	pdir = AllocateDir(pkglib_path);
	if (pdir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						pkglib_path),
				 errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has been moved away from its proper location.",
						 my_exec_path)));
	FreeDir(pdir);

	/*
	 * It's not worth checking the share/ directory.  If the lib/ directory is
	 * there, then share/ probably is too.
	 * 不值得检查 share/ 目录。如果 lib/ 目录在那里，那么 share/ 可能也在。
	 */
}

/*
 * Check that pg_control exists in the correct location in the data directory.
 *
 * No attempt is made to validate the contents of pg_control here.  This is
 * just a sanity check to see if we are looking at a real data directory.
 *
 * 检查 pg_control 是否存在于数据目录中的正确位置。
 * 这里不尝试验证 pg_control 的内容。这只是为了查看我们是否正在查看
 * 一个真实的数据目录进行的健壮性检查。
 */
static void
checkControlFile(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;

	snprintf(path, sizeof(path), "%s/%s", DataDir, XLOG_CONTROL_FILE);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		write_stderr("%s: could not find the database system\n"
					 "Expected to find it in the directory \"%s\",\n"
					 "but could not open file \"%s\": %m\n",
					 progname, DataDir, path);
		ExitPostmaster(2);
	}
	FreeFile(fp);
}

/*
 * Determine how long should we let ServerLoop sleep, in milliseconds.
 *
 * 确定我们应该让 ServerLoop 睡眠多久（以毫秒为单位）。
 *
 * In normal conditions we wait at most one minute, to ensure that the other
 * background tasks handled by ServerLoop get done even when no requests are
 * arriving.  However, if there are background workers waiting to be started,
 * we don't actually sleep so that they are quickly serviced.  Other exception
 * cases are as shown in the code.
 *
 * 在正常情况下，我们最多等待一分钟，以确保即使没有请求到达，由 
 * ServerLoop 处理的其他后台任务也能完成。但是，如果有后台工作进程
 * 等待启动，我们实际上并不睡眠，以便它们得到快速处理。其他例外情况见代码。
 */
static int
DetermineSleepTime(void)
{
	TimestampTz next_wakeup = 0;

	/*
	 * Normal case: either there are no background workers at all, or we're in
	 * a shutdown sequence (during which we ignore bgworkers altogether).
	 *
	 * 正常情况：要么根本没有后台工作进程，要么我们处于关闭序列中
	 * （在关闭期间，我们完全忽略后台工作进程）。
	 */
	if (Shutdown > NoShutdown ||
		(!StartWorkerNeeded && !HaveCrashedWorker))
	{
		if (AbortStartTime != 0)
		{
			int			seconds;

			/* time left to abort; clamp to 0 in case it already expired */
			/* 距离中止还剩多少时间；如果已经过期则钳制到 0 */
			seconds = SIGKILL_CHILDREN_AFTER_SECS -
				(time(NULL) - AbortStartTime);

			return Max(seconds * 1000, 0);
		}
		else
			return 60 * 1000;
	}

	if (StartWorkerNeeded)
		return 0;

	if (HaveCrashedWorker)
	{
		dlist_mutable_iter iter;

		/*
		 * When there are crashed bgworkers, we sleep just long enough that
		 * they are restarted when they request to be.  Scan the list to
		 * determine the minimum of all wakeup times according to most recent
		 * crash time and requested restart interval.
		 *
		 * 当有崩溃的后台工作进程时，我们睡眠足够长的时间，以便它们在请求重启时
		 * 被重启。扫描列表，根据最近的崩溃时间和请求的重启间隔确定所有唤醒
		 * 时间的最小值。
		 */
		dlist_foreach_modify(iter, &BackgroundWorkerList)
		{
			RegisteredBgWorker *rw;
			TimestampTz this_wakeup;

			rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);

			if (rw->rw_crashed_at == 0)
				continue;

			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART
				|| rw->rw_terminate)
			{
				ForgetBackgroundWorker(rw);
				continue;
			}

			this_wakeup = TimestampTzPlusMilliseconds(rw->rw_crashed_at,
													  1000L * rw->rw_worker.bgw_restart_time);
			if (next_wakeup == 0 || this_wakeup < next_wakeup)
				next_wakeup = this_wakeup;
		}
	}

	if (next_wakeup != 0)
	{
		int			ms;

		/* result of TimestampDifferenceMilliseconds is in [0, INT_MAX] */
		ms = (int) TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												   next_wakeup);
		return Min(60 * 1000, ms);
	}

	return 60 * 1000;
}

/*
 * Activate or deactivate notifications of server socket events.  Since we
 * don't currently have a way to remove events from an existing WaitEventSet,
 * we'll just destroy and recreate the whole thing.  This is called during
 * shutdown so we can wait for backends to exit without accepting new
 * connections, and during crash reinitialization when we need to start
 * listening for new connections again.  The WaitEventSet will be freed in fork
 * children by ClosePostmasterPorts().
 *
 * 激活或停用服务器套接字事件的通知。由于我们目前没有从现有的 
 * WaitEventSet 中删除事件的方法，我们将销毁并重新创建整个对象。这在
 * 关闭期间调用，以便我们可以在不接受新连接的情况下等待后端退出；在
 * 崩溃重新初始化期间调用，当我们需要再次开始监听新连接时调用。
 * WaitEventSet 将在 fork 子进程中通过 ClosePostmasterPorts() 释放。
 */
static void
ConfigurePostmasterWaitSet(bool accept_connections)
{
	if (pm_wait_set)
		FreeWaitEventSet(pm_wait_set);
	pm_wait_set = NULL;

	pm_wait_set = CreateWaitEventSet(NULL,
									 accept_connections ? (1 + NumListenSockets) : 1);
	AddWaitEventToSet(pm_wait_set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch,
					  NULL);

	if (accept_connections)
	{
		for (int i = 0; i < NumListenSockets; i++)
			AddWaitEventToSet(pm_wait_set, WL_SOCKET_ACCEPT, ListenSockets[i],
							  NULL, NULL);
	}
}

/*
 * Main idle loop of postmaster
 * Postmaster 的主空闲循环
 */
static int
ServerLoop(void)
{
	time_t		last_lockfile_recheck_time,
				last_touch_time;
	WaitEvent	events[MAXLISTEN];
	int			nevents;

	ConfigurePostmasterWaitSet(true);
	last_lockfile_recheck_time = last_touch_time = time(NULL);

	for (;;)
	{
		time_t		now;

		nevents = WaitEventSetWait(pm_wait_set,
								   DetermineSleepTime(),
								   events,
								   lengthof(events),
								   0 /* postmaster posts no wait_events */ );
								   /* postmaster 不发布等待事件 */

		/*
		 * Latch set by signal handler, or new connection pending on any of
		 * our sockets? If the latter, fork a child process to deal with it.
		 *
		 * 信号处理程序设置了闩锁（latch），或者我们的任何套接字上正有待处理
		 * 的新连接？如果是后者，派生一个子进程来处理它。
		 */
		for (int i = 0; i < nevents; i++)
		{
			if (events[i].events & WL_LATCH_SET)
				ResetLatch(MyLatch);

			/*
			 * The following requests are handled unconditionally, even if we
			 * didn't see WL_LATCH_SET.  This gives high priority to shutdown
			 * and reload requests where the latch happens to appear later in
			 * events[] or will be reported by a later call to
			 * WaitEventSetWait().
			 *
			 * 即使我们没有看到 WL_LATCH_SET，也会无条件处理以下请求。
			 * 如果闩锁恰好出现在 events[] 中的后面位置，或者将由稍后的 
			 * WaitEventSetWait() 调用报告，这将为关闭和重新加载请求提供高优先级。
			 */
			if (pending_pm_shutdown_request)
				process_pm_shutdown_request();
			if (pending_pm_reload_request)
				process_pm_reload_request();
			if (pending_pm_child_exit)
				process_pm_child_exit();
			if (pending_pm_pmsignal)
				process_pm_pmsignal();

			if (events[i].events & WL_SOCKET_ACCEPT)
			{
				ClientSocket s;

				if (AcceptConnection(events[i].fd, &s) == STATUS_OK)
					BackendStartup(&s);

				/* We no longer need the open socket in this process */
				/* 我们在该进程中不再需要这个已打开的套接字 */
				if (s.sock != PGINVALID_SOCKET)
				{
					if (closesocket(s.sock) != 0)
						elog(LOG, "could not close client socket: %m");
				}
			}
		}

		/*
		 * If we need to launch any background processes after changing state
		 * or because some exited, do so now.
		 * 如果在更改状态后或者因为某些进程已退出而需要启动任何后台进程，请立即执行。
		 */
		LaunchMissingBackgroundProcesses();

		/* If we need to signal the autovacuum launcher, do so now */
		/* 如果我们需要向自动清理启动器发送信号，请立即发送 */
		if (avlauncher_needs_signal)
		{
			avlauncher_needs_signal = false;
			if (AutoVacLauncherPMChild != NULL)
				signal_child(AutoVacLauncherPMChild, SIGUSR2);
		}

#ifdef HAVE_PTHREAD_IS_THREADED_NP

		/*
		 * With assertions enabled, check regularly for appearance of
		 * additional threads.  All builds check at start and exit.
		 * 开启断言后，定期检查是否出现了额外的线程。所有编译版本都在启动和退出时检查。
		 */
		Assert(pthread_is_threaded_np() == 0);
#endif

		/*
		 * Lastly, check to see if it's time to do some things that we don't
		 * want to do every single time through the loop, because they're a
		 * bit expensive.  Note that there's up to a minute of slop in when
		 * these tasks will be performed, since DetermineSleepTime() will let
		 * us sleep at most that long; except for SIGKILL timeout which has
		 * special-case logic there.
		 *
		 * 最后，检查是否到了该做某些事情的时候了（由于这些事情有点昂贵， 
		 * 我们不希望在循环中每次都做）。请注意，执行这些任务的时间可能会有
		 * 高达一分钟的误差，因为 DetermineSleepTime() 会让我们最多睡眠
		 * 那么久；除了在那里有特殊逻辑处理的 SIGKILL 超时。
		 */
		now = time(NULL);

		/*
		 * If we already sent SIGQUIT to children and they are slow to shut
		 * down, it's time to send them SIGKILL (or SIGABRT if requested).
		 * This doesn't happen normally, but under certain conditions backends
		 * can get stuck while shutting down.  This is a last measure to get
		 * them unwedged.
		 *
		 * 如果我们已经向子进程发送了 SIGQUIT 且它们关闭缓慢，那么是时候向 
		 * 它们发送 SIGKILL（或如果要求则发送 SIGABRT）了。这在通常情况下
		 * 不会发生，但在某些条件下，后端在关闭时可能会卡住。这是使它们摆脱
		 * 停滞的最后手段。
		 *
		 * Note we also do this during recovery from a process crash.
		 * 注意，我们在进程崩溃恢复期间也会这样做。
		 */
		if ((Shutdown >= ImmediateShutdown || FatalError) &&
			AbortStartTime != 0 &&
			(now - AbortStartTime) >= SIGKILL_CHILDREN_AFTER_SECS)
		{
			/* We were gentle with them before. Not anymore */
			/* 我们之前对它们很绅士。现在不再是了 */
			ereport(LOG,
			/* translator: %s is SIGKILL or SIGABRT */
					(errmsg("issuing %s to recalcitrant children",
							send_abort_for_kill ? "SIGABRT" : "SIGKILL")));
			TerminateChildren(send_abort_for_kill ? SIGABRT : SIGKILL);
			/* reset flag so we don't SIGKILL again */
			/* 重置标志，这样我们就不会再次发送 SIGKILL */
			AbortStartTime = 0;
		}

		/*
		 * Once a minute, verify that postmaster.pid hasn't been removed or
		 * overwritten.  If it has, we force a shutdown.  This avoids having
		 * postmasters and child processes hanging around after their database
		 * is gone, and maybe causing problems if a new database cluster is
		 * created in the same place.  It also provides some protection
		 * against a DBA foolishly removing postmaster.pid and manually
		 * starting a new postmaster.  Data corruption is likely to ensue from
		 * that anyway, but we can minimize the damage by aborting ASAP.
		 *
		 * 每分钟验证一次 postmaster.pid 尚未被删除或重写。如果是这样， 
		 * 我们就强制关闭。这避免了 postmaster 和子进程在其数据库消失后仍然
		 * 逗留，并且在同一地方创建新的数据库集群时可能会导致问题。它还为防止
		 * 管理员愚蠢地删除 postmaster.pid 并在手动启动新的 postmaster 
		 * 提供了某种保护。无论如何，这都可能会导致数据损坏，但我们可以通过 
		 * ASAP 中止来尽量减少损失。
		 */
		if (now - last_lockfile_recheck_time >= 1 * SECS_PER_MINUTE)
		{
			if (!RecheckDataDirLockFile())
			{
				ereport(LOG,
						(errmsg("performing immediate shutdown because data directory lock file is invalid")));
				kill(MyProcPid, SIGQUIT);
			}
			last_lockfile_recheck_time = now;
		}

		/*
		 * Touch Unix socket and lock files every 58 minutes, to ensure that
		 * they are not removed by overzealous /tmp-cleaning tasks.  We assume
		 * no one runs cleaners with cutoff times of less than an hour ...
		 * 每 58 分钟检查（Touch）一次 Unix 套接字和锁文件，以确保它们不会
		 * 被过于急迫的 /tmp 清理任务删除。我们假设没有人运行清理截止时间
		 * 小于一小时的任务……
		 */
		if (now - last_touch_time >= 58 * SECS_PER_MINUTE)
		{
			TouchSocketFiles();
			TouchSocketLockFiles();
			last_touch_time = now;
		}
	}
}

/*
 * canAcceptConnections --- check to see if database state allows connections
 * of the specified type.  backend_type can be B_BACKEND or B_AUTOVAC_WORKER.
 * (Note that we don't yet know whether a normal B_BACKEND connection might
 * turn into a walsender.)
 *
 * canAcceptConnections —— 检查数据库状态是否允许指定类型的连接。
 * backend_type 可以是 B_BACKEND 或 B_AUTOVAC_WORKER。
 * （注意，我们还不知道正常的 B_BACKEND 连接是否会转变为 walsender。）
 */
static CAC_state
canAcceptConnections(BackendType backend_type)
{
	CAC_state	result = CAC_OK;

	Assert(backend_type == B_BACKEND || backend_type == B_AUTOVAC_WORKER);

	/*
	 * Can't start backends when in startup/shutdown/inconsistent recovery
	 * state.  We treat autovac workers the same as user backends for this
	 * purpose.
	 *
	 * 在启动/关闭/不一致恢复状态下无法启动后端。出于此目的，我们将
	 * 自动清理工作进程与用户后端同等对待。
	 */
	if (pmState != PM_RUN && pmState != PM_HOT_STANDBY)
	{
		if (Shutdown > NoShutdown)
			return CAC_SHUTDOWN;	/* shutdown is pending */
									/* 关闭挂起 */
		else if (!FatalError && pmState == PM_STARTUP)
			return CAC_STARTUP; /* normal startup */
								/* 正常启动 */
		else if (!FatalError && pmState == PM_RECOVERY)
			return CAC_NOTHOTSTANDBY;	/* not yet ready for hot standby */
										/* 尚未准备好进行热备 */
		else
			return CAC_RECOVERY;	/* else must be crash recovery */
									/* 否则必定是崩溃恢复 */
	}

	/*
	 * "Smart shutdown" restrictions are applied only to normal connections,
	 * not to autovac workers.
	 * “智能关闭（Smart shutdown）”限制仅适用于正常连接，而不适用于
	 * 自动清理工作进程。
	 */
	if (!connsAllowed && backend_type == B_BACKEND)
		return CAC_SHUTDOWN;	/* shutdown is pending */
								/* 关闭挂起 */

	return result;
}

/*
 * ClosePostmasterPorts -- close all the postmaster's open sockets
 *
 * This is called during child process startup to release file descriptors
 * that are not needed by that child process.  The postmaster still has
 * them open, of course.
 *
 * ClosePostmasterPorts —— 关闭 postmaster 所有打开的套接字。
 * 这是在子进程启动期间调用的，以释放该子进程不需要的文件描述符。当然， 
 * postmaster 仍然打开着它们。
 *
 * Note: we pass am_syslogger as a boolean because we don't want to set
 * the global variable yet when this is called.
 *
 * 注意：我们将 am_syslogger 作为一个布尔值传递，因为在调用此函数时
 * 我们还不想设置全局变量。
 */
void
ClosePostmasterPorts(bool am_syslogger)
{
	/* Release resources held by the postmaster's WaitEventSet. */
	/* 释放 postmaster 的 WaitEventSet 持有的资源。 */
	if (pm_wait_set)
	{
		FreeWaitEventSetAfterFork(pm_wait_set);
		pm_wait_set = NULL;
	}

#ifndef WIN32

	/*
	 * Close the write end of postmaster death watch pipe. It's important to
	 * do this as early as possible, so that if postmaster dies, others won't
	 * think that it's still running because we're holding the pipe open.
	 *
	 * 关闭 postmaster 死亡监视管道的写入端。尽早执行此操作很重要， 
	 * 这样如果 postmaster 死亡，其他人就不会因为我们保持管道处于
	 * 打开状态而任务它仍在运行。
	 */
	if (close(postmaster_alive_fds[POSTMASTER_FD_OWN]) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not close postmaster death monitoring pipe in child process: %m")));
	postmaster_alive_fds[POSTMASTER_FD_OWN] = -1;
	/* Notify fd.c that we released one pipe FD. */
	/* 通知 fd.c 们释放了一个管道 FD。 */
	ReleaseExternalFD();
#endif

	/*
	 * Close the postmaster's listen sockets.  These aren't tracked by fd.c,
	 * so we don't call ReleaseExternalFD() here.
	 *
	 * 关闭 postmaster 的监听套接字。这些不由 fd.c 跟踪， 
	 * 因此我们在这里不调用 ReleaseExternalFD()。
	 *
	 * The listen sockets are marked as FD_CLOEXEC, so this isn't needed in
	 * EXEC_BACKEND mode.
	 * 监听套接字被标记为 FD_CLOEXEC，因此在 EXEC_BACKEND 模式下不需要。
	 */
#ifndef EXEC_BACKEND
	if (ListenSockets)
	{
		for (int i = 0; i < NumListenSockets; i++)
		{
			if (closesocket(ListenSockets[i]) != 0)
				elog(LOG, "could not close listen socket: %m");
		}
		pfree(ListenSockets);
	}
	NumListenSockets = 0;
	ListenSockets = NULL;
#endif

	/*
	 * If using syslogger, close the read side of the pipe.  We don't bother
	 * tracking this in fd.c, either.
	 * 如果使用系统日志记录器（syslogger），请关闭管道的读取端。 
	 * 我们也不打算在 fd.c 中对此进行跟踪。
	 */
	if (!am_syslogger)
	{
#ifndef WIN32
		if (syslogPipe[0] >= 0)
			close(syslogPipe[0]);
		syslogPipe[0] = -1;
#else
		if (syslogPipe[0])
			CloseHandle(syslogPipe[0]);
		syslogPipe[0] = 0;
#endif
	}

#ifdef USE_BONJOUR
	/* If using Bonjour, close the connection to the mDNS daemon */
	/* 如果使用 Bonjour，请关闭与 mDNS 守护进程的连接 */
	if (bonjour_sdref)
		close(DNSServiceRefSockFD(bonjour_sdref));
#endif
}


/*
 * InitProcessGlobals -- set MyStartTime[stamp], random seeds
 * InitProcessGlobals —— 设置 MyStartTime[stamp] 和随机种子
 *
 * Called early in the postmaster and every backend.
 * 在 postmaster 和每个后端的早期调用。
 */
void
InitProcessGlobals(void)
{
	MyStartTimestamp = GetCurrentTimestamp();
	MyStartTime = timestamptz_to_time_t(MyStartTimestamp);

	/*
	 * Set a different global seed in every process.  We want something
	 * unpredictable, so if possible, use high-quality random bits for the
	 * seed.  Otherwise, fall back to a seed based on timestamp and PID.
	 *
	 * 在每个进程中设置不同的全局种子。我们想要一些不可预测的东西，因此如果
	 * 可能的话，使用高质量的随机位作为种子。否则，回退到基于时间戳和 PID 的种子。
	 */
	if (unlikely(!pg_prng_strong_seed(&pg_global_prng_state)))
	{
		uint64		rseed;

		/*
		 * Since PIDs and timestamps tend to change more frequently in their
		 * least significant bits, shift the timestamp left to allow a larger
		 * total number of seeds in a given time period.  Since that would
		 * leave only 20 bits of the timestamp that cycle every ~1 second,
		 * also mix in some higher bits.
		 *
		 * 由于 PID 和时间戳在其最低有效位中往往变化更频繁，因此将时间戳
		 * 向左移动以允许在给定时间段内获得更大的种子总数。由于这样只会留下 
		 * 20 位每秒周期性变化的时间戳，因此还要混合进一些高位。
		 */
		rseed = ((uint64) MyProcPid) ^
			((uint64) MyStartTimestamp << 12) ^
			((uint64) MyStartTimestamp >> 20);

		pg_prng_seed(&pg_global_prng_state, rseed);
	}

	/*
	 * Also make sure that we've set a good seed for random(3).  Use of that
	 * is deprecated in core Postgres, but extensions might use it.
	 *
	 * 此外，确保我们已经为 random(3) 设置了一个好的种子。在 Postgres 核心
	 * 中已弃用了这一用法，但扩展程序可能会用到它。
	 */
#ifndef WIN32
	srandom(pg_prng_uint32(&pg_global_prng_state));
#endif
}

/*
 * Child processes use SIGUSR1 to notify us of 'pmsignals'.  pg_ctl uses
 * SIGUSR1 to ask postmaster to check for logrotate and promote files.
 *
 * 子进程使用 SIGUSR1 来通知我们“pmsignal”。pg_ctl 使用 SIGUSR1 
 * 要求 postmaster 检查日志轮转（logrotate）和晋升（promote）文件。
 */
static void
handle_pm_pmsignal_signal(SIGNAL_ARGS)
{
	pending_pm_pmsignal = true;
	SetLatch(MyLatch);
}

/*
 * pg_ctl uses SIGHUP to request a reload of the configuration files.
 * pg_ctl 使用 SIGHUP 请求重新加载配置文件。
 */
static void
handle_pm_reload_request_signal(SIGNAL_ARGS)
{
	pending_pm_reload_request = true;
	SetLatch(MyLatch);
}

/*
 * Re-read config files, and tell children to do same.
 * 重新读取配置文件，并告知子进程也这样做。
 */
static void
process_pm_reload_request(void)
{
	pending_pm_reload_request = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received reload request signal")));

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
				(errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
		SignalChildren(SIGHUP, btmask_all_except(B_DEAD_END_BACKEND));

		/* Reload authentication config files too */
		if (!load_hba())
			ereport(LOG,
			/* translator: %s is a configuration file */
					(errmsg("%s was not reloaded", HbaFileName)));

		if (!load_ident())
			ereport(LOG,
					(errmsg("%s was not reloaded", IdentFileName)));

#ifdef USE_SSL
		/* Reload SSL configuration as well */
		/* 也重新加载 SSL 配置 */
		if (EnableSSL)
		{
			if (secure_initialize(false) == 0)
				LoadedSSL = true;
			else
				ereport(LOG,
						(errmsg("SSL configuration was not reloaded")));
		}
		else
		{
			secure_destroy();
			LoadedSSL = false;
		}
#endif

#ifdef EXEC_BACKEND
		/* Update the starting-point file for future children */
		/* 为未来的子进程更新起点文件 */
		write_nondefault_variables(PGC_SIGHUP);
#endif
	}
}

/*
 * pg_ctl uses SIGTERM, SIGINT and SIGQUIT to request different types of
 * shutdown.
 *
 * pg_ctl 使用 SIGTERM、SIGINT 和 SIGQUIT 请求不同类型的关闭。
 */
static void
handle_pm_shutdown_request_signal(SIGNAL_ARGS)
{
	switch (postgres_signal_arg)
	{
		case SIGTERM:
			/* smart is implied if the other two flags aren't set */
			/* 如果未设置另外两个标志，则暗示为“智能（smart）” */
			pending_pm_shutdown_request = true;
			break;
		case SIGINT:
			pending_pm_fast_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
		case SIGQUIT:
			pending_pm_immediate_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
	}
	SetLatch(MyLatch);
}

/*
 * Process shutdown request.
 * 处理关闭请求。
 */
static void
process_pm_shutdown_request(void)
{
	int			mode;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received shutdown request signal")));

	pending_pm_shutdown_request = false;

	/*
	 * If more than one shutdown request signal arrived since the last server
	 * loop, take the one that is the most immediate.  That matches the
	 * priority that would apply if we processed them one by one in any order.
	 *
	 * 如果自上次服务器循环以来到达了多个关闭请求信号，则采用最紧急的一个。 
	 * 这与我们按任何顺序逐一处理它们所适用的优先级相匹配。
	 */
	if (pending_pm_immediate_shutdown_request)
	{
		pending_pm_immediate_shutdown_request = false;
		pending_pm_fast_shutdown_request = false;
		mode = ImmediateShutdown;
	}
	else if (pending_pm_fast_shutdown_request)
	{
		pending_pm_fast_shutdown_request = false;
		mode = FastShutdown;
	}
	else
		mode = SmartShutdown;

	switch (mode)
	{
		case SmartShutdown:

			/*
			 * Smart Shutdown:
			 *
			 * Wait for children to end their work, then shut down.
			 *
			 * 智能关闭：
			 * 
			 * 等待子进程结束其工作，然后关闭。
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/*
			 * If we reached normal running, we go straight to waiting for
			 * client backends to exit.  If already in PM_STOP_BACKENDS or a
			 * later state, do not change it.
			 *
			 * 如果我们达到了正常运行状态，我们就直接等待客户端后端退出。
			 * 如果已经处于 PM_STOP_BACKENDS 或更晚的状态，请不要更改它。
			 */
			if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
				connsAllowed = false;
			else if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* There should be no clients, so proceed to stop children */
				/* 应该没有客户端，所以继续停止子进程 */
				UpdatePMState(PM_STOP_BACKENDS);
			}

			/*
			 * Now wait for online backup mode to end and backends to exit. If
			 * that is already the case, PostmasterStateMachine will take the
			 * next step.
			 *
			 * 现在等待在线备份模式结束和后端退出。如果已经是这种情况， 
			 * PostmasterStateMachine 将采取下一步。
			 */
			PostmasterStateMachine();
			break;

		case FastShutdown:

			/*
			 * Fast Shutdown:
			 *
			 * Abort all children with SIGTERM (rollback active transactions
			 * and exit) and shut down when they are gone.
			 *
			 * 快速关闭：
			 * 
			 * 使用 SIGTERM 中止所有子进程（回滚活动事务并退出），并在它们消失后关闭。
			 */
			if (Shutdown >= FastShutdown)
				break;
			Shutdown = FastShutdown;
			ereport(LOG,
					(errmsg("received fast shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* Just shut down background processes silently */
				/* 只是静默地关闭后台进程 */
				UpdatePMState(PM_STOP_BACKENDS);
			}
			else if (pmState == PM_RUN ||
					 pmState == PM_HOT_STANDBY)
			{
				/* Report that we're about to zap live client sessions */
				/* 报告我们即将清除（zap）活动的客户端会话 */
				ereport(LOG,
						(errmsg("aborting any active transactions")));
				UpdatePMState(PM_STOP_BACKENDS);
			}

			/*
			 * PostmasterStateMachine will issue any necessary signals, or
			 * take the next step if no child processes need to be killed.
			 *
			 * PostmasterStateMachine 将发出任何必要的信号，或者在不需要
			 * 杀死任何子进程的情况下采取下一步。
			 */
			PostmasterStateMachine();
			break;

		case ImmediateShutdown:

			/*
			 * Immediate Shutdown:
			 *
			 * abort all children with SIGQUIT, wait for them to exit,
			 * terminate remaining ones with SIGKILL, then exit without
			 * attempt to properly shut down the data base system.
			 *
			 * 立即关闭：
			 * 
			 * 使用 SIGQUIT 中止所有子进程，等待它们退出，使用 SIGKILL 
			 * 终止剩余的子进程，然后在不尝试正确关闭数据库系统的情况下退出。
			 */
			if (Shutdown >= ImmediateShutdown)
				break;
			Shutdown = ImmediateShutdown;
			ereport(LOG,
					(errmsg("received immediate shutdown request")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/* tell children to shut down ASAP */
			/* 告诉子进程尽快关闭 */
			/* (note we don't apply send_abort_for_crash here) */
			SetQuitSignalReason(PMQUIT_FOR_STOP);
			TerminateChildren(SIGQUIT);
			UpdatePMState(PM_WAIT_BACKENDS);

			/* set stopwatch for them to die */
			AbortStartTime = time(NULL);

			/*
			 * Now wait for backends to exit.  If there are none,
			 * PostmasterStateMachine will take the next step.
			 *
			 * 现在等待后端退出。如果没有，PostmasterStateMachine 将采取下一步。
			 */
			PostmasterStateMachine();
			break;
	}
}

static void
handle_pm_child_exit_signal(SIGNAL_ARGS)
{
	pending_pm_child_exit = true;
	SetLatch(MyLatch);
}

/*
 * Cleanup after a child process dies.
 * 在子进程死亡后进行清理。
 */
static void
process_pm_child_exit(void)
{
	int			pid;			/* process id of dead child process */
								/* 已死亡子进程的进程 ID */
	int			exitstatus;		/* its exit status */
								/* 它的退出状态 */

	pending_pm_child_exit = false;

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));

	while ((pid = waitpid(-1, &exitstatus, WNOHANG)) > 0)
	{
		PMChild    *pmchild;

		/*
		 * Check if this child was a startup process.
		 * 检查此子进程是否为启动进程。
		 */
		if (StartupPMChild && pid == StartupPMChild->pid)
		{
			ReleasePostmasterChildSlot(StartupPMChild);
			StartupPMChild = NULL;

			/*
			 * Startup process exited in response to a shutdown request (or it
			 * completed normally regardless of the shutdown request).
			 *
			 * 启动进程响应关闭请求而退出（或者无论关闭请求如何都正常完成了）。
			 */
			if (Shutdown > NoShutdown &&
				(EXIT_STATUS_0(exitstatus) || EXIT_STATUS_1(exitstatus)))
			{
				StartupStatus = STARTUP_NOT_RUNNING;
				UpdatePMState(PM_WAIT_BACKENDS);
				/* PostmasterStateMachine logic does the rest */
				continue;
			}

			if (EXIT_STATUS_3(exitstatus))
			{
				ereport(LOG,
						(errmsg("shutdown at recovery target")));
				StartupStatus = STARTUP_NOT_RUNNING;
				Shutdown = Max(Shutdown, SmartShutdown);
				TerminateChildren(SIGTERM);
				UpdatePMState(PM_WAIT_BACKENDS);
				/* PostmasterStateMachine logic does the rest */
				continue;
			}

			/*
			 * Unexpected exit of startup process (including FATAL exit)
			 * during PM_STARTUP is treated as catastrophic. There are no
			 * other processes running yet, so we can just exit.
			 *
			 * PM_STARTUP 期间启动进程的意外退出（包括 FATAL 退出）被视为灾难性的。
			 * 目前还没有其他进程在运行，所以我们可以直接退出。
			 */
			if (pmState == PM_STARTUP &&
				StartupStatus != STARTUP_SIGNALED &&
				!EXIT_STATUS_0(exitstatus))
			{
				LogChildExit(LOG, _("startup process"),
							 pid, exitstatus);
				ereport(LOG,
						(errmsg("aborting startup due to startup process failure")));
				ExitPostmaster(1);
			}

			/*
			 * After PM_STARTUP, any unexpected exit (including FATAL exit) of
			 * the startup process is catastrophic, so kill other children,
			 * and set StartupStatus so we don't try to reinitialize after
			 * they're gone.  Exception: if StartupStatus is STARTUP_SIGNALED,
			 * then we previously sent the startup process a SIGQUIT; so
			 * that's probably the reason it died, and we do want to try to
			 * restart in that case.
			 *
			 * 在 PM_STARTUP 之后，启动进程的任何意外退出（包括 FATAL 退出）都是
			 * 灾难性的，因此要杀死其他子进程，并设置 StartupStatus，以便我们
			 * 不会在它们消失后尝试重新初始化。例外：如果 StartupStatus 是 
			 * STARTUP_SIGNALED，那么我们之前向启动进程发送了 SIGQUIT；因此
			 * 这可能是它死亡的原因，在这种情况下我们确实想要尝试重启。
			 *
			 * This stanza also handles the case where we sent a SIGQUIT
			 * during PM_STARTUP due to some dead-end child crashing: in that
			 * situation, if the startup process dies on the SIGQUIT, we need
			 * to transition to PM_WAIT_BACKENDS state which will allow
			 * PostmasterStateMachine to restart the startup process.  (On the
			 * other hand, the startup process might complete normally, if we
			 * were too late with the SIGQUIT.  In that case we'll fall
			 * through and commence normal operations.)
			 *
			 * 这一节还处理了我们在 PM_STARTUP 期间由于某些“死路（dead-end）”子进程
			 * 崩溃而发送 SIGQUIT 的情况：在这种情况下，如果启动进程因 SIGQUIT 
			 * 而死亡，我们需要转换到 PM_WAIT_BACKENDS 状态，这将允许 
			 * PostmasterStateMachine 重启启动进程。（另一方面，如果我们发送 
			 * SIGQUIT 太晚，启动进程可能会正常完成。在这种情况下，我们将进入 
			 * 下一步并开始正常操作。）
			 */
			if (!EXIT_STATUS_0(exitstatus))
			{
				if (StartupStatus == STARTUP_SIGNALED)
				{
					StartupStatus = STARTUP_NOT_RUNNING;
					if (pmState == PM_STARTUP)
						UpdatePMState(PM_WAIT_BACKENDS);
				}
				else
					StartupStatus = STARTUP_CRASHED;
				HandleChildCrash(pid, exitstatus,
								 _("startup process"));
				continue;
			}

			/*
			 * Startup succeeded, commence normal operations
			 * 启动成功，开始正常操作
			 */
			StartupStatus = STARTUP_NOT_RUNNING;
			FatalError = false;
			AbortStartTime = 0;
			ReachedNormalRunning = true;
			UpdatePMState(PM_RUN);
			connsAllowed = true;

			/*
			 * At the next iteration of the postmaster's main loop, we will
			 * crank up the background tasks like the autovacuum launcher and
			 * background workers that were not started earlier already.
			 *
			 * 在 Postmaster 主循环的下一次迭代中，我们将启动像自动清理启动器
			 * 和后台工作进程等之前尚未启动的后台任务。
			 */
			StartWorkerNeeded = true;

			/* at this point we are really open for business */
			/* 此时我们已正式开门营业 */
			ereport(LOG,
					(errmsg("database system is ready to accept connections")));

			/* Report status */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif

			continue;
		}

		/*
		 * Was it the bgwriter?  Normal exit can be ignored; we'll start a new
		 * one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 *
		 * 是后台写进程（bgwriter）吗？正常退出可以忽略；如有必要，我们将在
		 * Postmaster 主循环的下一次迭代中启动一个新的。任何其他退出条件都 
		 * 被视为崩溃。
		 */
		if (BgWriterPMChild && pid == BgWriterPMChild->pid)
		{
			ReleasePostmasterChildSlot(BgWriterPMChild);
			BgWriterPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("background writer process"));
			continue;
		}

		/*
		 * Was it the checkpointer?
		 * 是检查点进程（checkpointer）吗？
		 */
		if (CheckpointerPMChild && pid == CheckpointerPMChild->pid)
		{
			ReleasePostmasterChildSlot(CheckpointerPMChild);
			CheckpointerPMChild = NULL;
			if (EXIT_STATUS_0(exitstatus) && pmState == PM_WAIT_CHECKPOINTER)
			{
				/*
				 * OK, we saw normal exit of the checkpointer after it's been
				 * told to shut down.  We know checkpointer wrote a shutdown
				 * checkpoint, otherwise we'd still be in
				 * PM_WAIT_XLOG_SHUTDOWN state.
				 *
				 * 好了，我们看到检查点进程在被告知关闭后正常退出。我们知道 
				 * 检查点进程已经写入了关闭检查点，否则我们仍然会处于 
				 * PM_WAIT_XLOG_SHUTDOWN 状态。
				 *
				 * At this point only dead-end children and logger should be
				 * left.
				 * 此时应该只剩下“死路（dead-end）”子进程和日志记录器。
				 */
				UpdatePMState(PM_WAIT_DEAD_END);
				ConfigurePostmasterWaitSet(false);
				SignalChildren(SIGTERM, btmask_all_except(B_LOGGER));
			}
			else
			{
				/*
				 * Any unexpected exit of the checkpointer (including FATAL
				 * exit) is treated as a crash.
				 * 检查点进程的任何意外退出（包括 FATAL 退出）都被视为崩溃。
				 */
				HandleChildCrash(pid, exitstatus,
								 _("checkpointer process"));
			}

			continue;
		}

		/*
		 * Was it the wal writer?  Normal exit can be ignored; we'll start a
		 * new one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 *
		 * 是 WAL 写进程吗？正常退出可以忽略；如有必要，我们将在 Postmaster
		 * 主循环的下一次迭代中启动一个新的。任何其他退出条件都被视为崩溃。
		 */
		if (WalWriterPMChild && pid == WalWriterPMChild->pid)
		{
			ReleasePostmasterChildSlot(WalWriterPMChild);
			WalWriterPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL writer process"));
			continue;
		}

		/*
		 * Was it the wal receiver?  If exit status is zero (normal) or one
		 * (FATAL exit), we assume everything is all right just like normal
		 * backends.  (If we need a new wal receiver, we'll start one at the
		 * next iteration of the postmaster's main loop.)
		 *
		 * 是 WAL 接收进程吗？如果退出状态为零（正常）或一（FATAL 退出），
		 * 我们假设一切正常，就像普通后端一样。（如果我们需要新的 WAL 接收
		 * 进程，我们将在 Postmaster 主循环的下一次迭代中启动一个。）
		 */
		if (WalReceiverPMChild && pid == WalReceiverPMChild->pid)
		{
			ReleasePostmasterChildSlot(WalReceiverPMChild);
			WalReceiverPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL receiver process"));
			continue;
		}

		/*
		 * Was it the wal summarizer? Normal exit can be ignored; we'll start
		 * a new one at the next iteration of the postmaster's main loop, if
		 * necessary.  Any other exit condition is treated as a crash.
		 *
		 * 是 WAL 汇总进程吗？正常退出可以忽略；如有必要，我们将在 Postmaster
		 * 主循环的下一次迭代中启动一个新的。任何其他退出条件都被视为崩溃。
		 */
		if (WalSummarizerPMChild && pid == WalSummarizerPMChild->pid)
		{
			ReleasePostmasterChildSlot(WalSummarizerPMChild);
			WalSummarizerPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL summarizer process"));
			continue;
		}

		/*
		 * Was it the autovacuum launcher?	Normal exit can be ignored; we'll
		 * start a new one at the next iteration of the postmaster's main
		 * loop, if necessary.  Any other exit condition is treated as a
		 * crash.
		 *
		 * 是自动清理启动进程吗？正常退出可以忽略；如有必要，我们将在 
		 * Postmaster 主循环的下一次迭代中启动一个新的。任何其他退出条件
		 * 都被视为崩溃。
		 */
		if (AutoVacLauncherPMChild && pid == AutoVacLauncherPMChild->pid)
		{
			ReleasePostmasterChildSlot(AutoVacLauncherPMChild);
			AutoVacLauncherPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("autovacuum launcher process"));
			continue;
		}

		/*
		 * Was it the archiver?  If exit status is zero (normal) or one (FATAL
		 * exit), we assume everything is all right just like normal backends
		 * and just try to start a new one on the next cycle of the
		 * postmaster's main loop, to retry archiving remaining files.
		 *
		 * 是归档进程吗？如果退出状态为零（正常）或一（FATAL 退出），我们
		 * 假设一切正常，就像普通后端一样，并尝试在 Postmaster 主循环的下一个
		 * 周期启动一个，以重试归档剩余文件。
		 */
		if (PgArchPMChild && pid == PgArchPMChild->pid)
		{
			ReleasePostmasterChildSlot(PgArchPMChild);
			PgArchPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("archiver process"));
			continue;
		}

		/* Was it the system logger?  If so, try to start a new one */
		/* 是系统日志进程吗？如果是，尝试启动一个新的 */
		if (SysLoggerPMChild && pid == SysLoggerPMChild->pid)
		{
			ReleasePostmasterChildSlot(SysLoggerPMChild);
			SysLoggerPMChild = NULL;

			/* for safety's sake, launch new logger *first* */
			/* 为了安全起见，*首先*启动新的日志进程 */
			if (Logging_collector)
				StartSysLogger();

			if (!EXIT_STATUS_0(exitstatus))
				LogChildExit(LOG, _("system logger process"),
							 pid, exitstatus);
			continue;
		}

		/*
		 * Was it the slot sync worker? Normal exit or FATAL exit can be
		 * ignored (FATAL can be caused by libpqwalreceiver on receiving
		 * shutdown request by the startup process during promotion); we'll
		 * start a new one at the next iteration of the postmaster's main
		 * loop, if necessary. Any other exit condition is treated as a crash.
		 *
		 * 是槽（slot）同步工作进程吗？正常退出或 FATAL 退出可以忽略（FATAL 
		 * 可能是由于 libpqwalreceiver 在晋升期间收到启动进程的关闭请求而
		 * 导致的）；如有必要，我们将在 Postmaster 主循环的下一次迭代中
		 * 启动一个新的。任何其他退出条件都被视为崩溃。
		 */
		if (SlotSyncWorkerPMChild && pid == SlotSyncWorkerPMChild->pid)
		{
			ReleasePostmasterChildSlot(SlotSyncWorkerPMChild);
			SlotSyncWorkerPMChild = NULL;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("slot sync worker process"));
			continue;
		}

		/* Was it an IO worker? */
		/* 是 IO 工作进程吗？ */
		if (maybe_reap_io_worker(pid))
		{
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus, _("io worker"));

			maybe_adjust_io_workers();
			continue;
		}

		/*
		 * Was it a backend or a background worker?
		 * 是后端进程还是后台工作进程？
		 */
		pmchild = FindPostmasterChildByPid(pid);
		if (pmchild)
		{
			CleanupBackend(pmchild, exitstatus);
		}

		/*
		 * We don't know anything about this child process.  That's highly
		 * unexpected, as we do track all the child processes that we fork.
		 *
		 * 我们对这个子进程一无所知。这非常令人意外，因为我们确实跟踪了
		 * 我们 fork 的所有子进程。
		 */
		else
		{
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus, _("untracked child process"));
			else
				LogChildExit(LOG, _("untracked child process"), pid, exitstatus);
		}
	}							/* loop over pending child-death reports */

	/*
	 * After cleaning out the SIGCHLD queue, see if we have any state changes
	 * or actions to make.
	 * 清理完 SIGCHLD 队列后，看看我们是否有任何状态更改或操作要做。
	 */
	PostmasterStateMachine();
}

/*
 * CleanupBackend -- cleanup after terminated backend or background worker.
 *
 * Remove all local state associated with the child process and release its
 * PMChild slot.
 *
 * CleanupBackend —— 在后端或后台工作进程终止后进行清理。
 * 删除与子进程关联的所有本地状态并释放其 PMChild 槽位。
 */
static void
CleanupBackend(PMChild *bp,
			   int exitstatus)	/* child's exit status. */
{
	char		namebuf[MAXPGPATH];
	const char *procname;
	bool		crashed = false;
	bool		logged = false;
	pid_t		bp_pid;
	bool		bp_bgworker_notify;
	BackendType bp_bkend_type;
	RegisteredBgWorker *rw;

	/* Construct a process name for the log message */
	/* 构造用于日志消息的进程名称 */
	if (bp->bkend_type == B_BG_WORKER)
	{
		snprintf(namebuf, MAXPGPATH, _("background worker \"%s\""),
				 bp->rw->rw_worker.bgw_type);
		procname = namebuf;
	}
	else
		procname = _(GetBackendTypeDesc(bp->bkend_type));

	/*
	 * If a backend dies in an ugly way then we must signal all other backends
	 * to quickdie.  If exit status is zero (normal) or one (FATAL exit), we
	 * assume everything is all right and proceed to remove the backend from
	 * the active child list.
	 *
	 * 如果一个后端以一种糟糕（ugly）的方式死去，那么我们必须向所有其他
	 * 后端发送信号以执行快速死亡（quickdie）。如果退出状态为零（正常） 
	 * 或一（FATAL 退出），我们假设一切正常，并继续从活动子进程列表中删除该后端。
	 */
	if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
		crashed = true;

#ifdef WIN32

	/*
	 * On win32, also treat ERROR_WAIT_NO_CHILDREN (128) as nonfatal case,
	 * since that sometimes happens under load when the process fails to start
	 * properly (long before it starts using shared memory). Microsoft reports
	 * it is related to mutex failure:
	 * http://archives.postgresql.org/pgsql-hackers/2010-09/msg00790.php
	 *
	 * 在 Win32 上，也将 ERROR_WAIT_NO_CHILDREN (128) 视为非致命情况， 
	 * 因为在负载下，当进程无法正确启动（在开始使用共享内存之前很久）时， 
	 * 有时会发生这种情况。微软报告称这与互斥量失败有关。
	 */
	if (exitstatus == ERROR_WAIT_NO_CHILDREN)
	{
		LogChildExit(LOG, procname, bp->pid, exitstatus);
		logged = true;
		crashed = false;
	}
#endif

	/*
	 * Release the PMChild entry.
	 *
	 * If the process attached to shared memory, this also checks that it
	 * detached cleanly.
	 *
	 * 释放 PMChild 条目。
	 * 如果该进程附加到了共享内存，这也将检查它是否已干净地分离。
	 */
	bp_pid = bp->pid;
	bp_bgworker_notify = bp->bgworker_notify;
	bp_bkend_type = bp->bkend_type;
	rw = bp->rw;
	if (!ReleasePostmasterChildSlot(bp))
	{
		/*
		 * Uh-oh, the child failed to clean itself up.  Treat as a crash after
		 * all.
		 */
		crashed = true;
	}
	bp = NULL;

	/*
	 * In a crash case, exit immediately without resetting background worker
	 * state. However, if restart_after_crash is enabled, the background
	 * worker state (e.g., rw_pid) still needs be reset so the worker can
	 * restart after crash recovery. This reset is handled in
	 * ResetBackgroundWorkerCrashTimes(), not here.
	 *
	 * 在崩溃情况下，立即退出而不重置后台工作进程状态。但是，如果启用了 
	 * restart_after_crash，则后台工作进程状态（例如 rw_pid）仍需重置， 
	 * 以便工作进程在崩溃恢复后可以重启。此重置在 
	 * ResetBackgroundWorkerCrashTimes() 中处理，而不是在这里。
	 */
	if (crashed)
	{
		HandleChildCrash(bp_pid, exitstatus, procname);
		return;
	}

	/*
	 * This backend may have been slated to receive SIGUSR1 when some
	 * background worker started or stopped.  Cancel those notifications, as
	 * we don't want to signal PIDs that are not PostgreSQL backends.  This
	 * gets skipped in the (probably very common) case where the backend has
	 * never requested any such notifications.
	 *
	 * 此后端可能曾定于在某些后台工作进程启动或停止时接收 SIGUSR1。 
	 * 取消这些通知，因为我们不想向不是 PostgreSQL 后端的 PID 发送信号。 
	 * 在后端从未请求过此类通知的情况下（可能非常常见），这会被跳过。
	 */
	if (bp_bgworker_notify)
		BackgroundWorkerStopNotifications(bp_pid);

	/*
	 * If it was a background worker, also update its RegisteredBgWorker
	 * entry.
	 * 如果它是后台工作进程，也要更新其 RegisteredBgWorker 条目。
	 */
	if (bp_bkend_type == B_BG_WORKER)
	{
		if (!EXIT_STATUS_0(exitstatus))
		{
			/* Record timestamp, so we know when to restart the worker. */
			/* 记录时间戳，以便我们知道何时重启该工作进程。 */
			rw->rw_crashed_at = GetCurrentTimestamp();
		}
		else
		{
			/* Zero exit status means terminate */
			/* 退出状态为零表示终止 */
			rw->rw_crashed_at = 0;
			rw->rw_terminate = true;
		}

		rw->rw_pid = 0;
		ReportBackgroundWorkerExit(rw); /* report child death */
										/* 报告子进程死亡 */

		if (!logged)
		{
			LogChildExit(EXIT_STATUS_0(exitstatus) ? DEBUG1 : LOG,
						 procname, bp_pid, exitstatus);
			logged = true;
		}

		/* have it be restarted */
		/* 让它被重启 */
		HaveCrashedWorker = true;
	}

	if (!logged)
		LogChildExit(DEBUG2, procname, bp_pid, exitstatus);
}

/*
 * Transition into FatalError state, in response to something bad having
 * happened. Commonly the caller will have logged the reason for entering
 * FatalError state.
 *
 * This should only be called when not already in FatalError or
 * ImmediateShutdown state.
 *
 * 响应发生的坏事，转换到 FatalError（致命错误）状态。调用者通常会记录 
 * 进入 FatalError 状态的原因。
 * 只有在尚未处于 FatalError 或 ImmediateShutdown 状态时才应调用此函数。
 */
static void
HandleFatalError(QuitSignalReason reason, bool consider_sigabrt)
{
	int			sigtosend;

	Assert(!FatalError);
	Assert(Shutdown != ImmediateShutdown);

	SetQuitSignalReason(reason);

	if (consider_sigabrt && send_abort_for_crash)
		sigtosend = SIGABRT;
	else
		sigtosend = SIGQUIT;

	/*
	 * Signal all other child processes to exit.
	 *
	 * We could exclude dead-end children here, but at least when sending
	 * SIGABRT it seems better to include them.
	 *
	 * 发出信号要求所有其他子进程退出。
	 * 我们可以在这里排除“死路（dead-end）”子进程，但至少在发送 SIGABRT 
	 * 时，包含它们似乎更好。
	 */
	TerminateChildren(sigtosend);

	FatalError = true;

	/*
	 * Choose the appropriate new state to react to the fatal error. Unless we
	 * were already in the process of shutting down, we go through
	 * PM_WAIT_BACKENDS. For errors during the shutdown sequence, we directly
	 * switch to PM_WAIT_DEAD_END.
	 *
	 * 选择合适的新状态以应对致命错误。除非我们已经处于关闭过程中， 
	 * 否则我们会进入 PM_WAIT_BACKENDS。对于关闭序列期间的错误， 
	 * 我们直接切换到 PM_WAIT_DEAD_END。
	 */
	switch (pmState)
	{
		case PM_INIT:
			/* shouldn't have any children */
			/* 不应该有任何子进程 */
			Assert(false);
			break;
		case PM_STARTUP:
			/* should have been handled in process_pm_child_exit */
			/* 应该已在 process_pm_child_exit 中处理 */
			Assert(false);
			break;

			/* wait for children to die */
			/* 等待子进程死亡 */
		case PM_RECOVERY:
		case PM_HOT_STANDBY:
		case PM_RUN:
		case PM_STOP_BACKENDS:
			UpdatePMState(PM_WAIT_BACKENDS);
			break;

		case PM_WAIT_BACKENDS:
			/* there might be more backends to wait for */
			/* 可能还有更多后端需要等待 */
			break;

		case PM_WAIT_XLOG_SHUTDOWN:
		case PM_WAIT_XLOG_ARCHIVAL:
		case PM_WAIT_CHECKPOINTER:
		case PM_WAIT_IO_WORKERS:

			/*
			 * NB: Similar code exists in PostmasterStateMachine()'s handling
			 * of FatalError in PM_STOP_BACKENDS/PM_WAIT_BACKENDS states.
			 * 
			 * 注意：PM_STOP_BACKENDS/PM_WAIT_BACKENDS 状态下对 FatalError 
			 * 的处理中也存在类似的代码。
			 */
			ConfigurePostmasterWaitSet(false);
			UpdatePMState(PM_WAIT_DEAD_END);
			break;

		case PM_WAIT_DEAD_END:
		case PM_NO_CHILDREN:
			break;
	}

	/*
	 * .. and if this doesn't happen quickly enough, now the clock is ticking
	 * for us to kill them without mercy.
	 *
	 * ……如果这没有很快发生，那么现在已经在计时了，我们将无情地杀死它们。
	 */
	if (AbortStartTime == 0)
		AbortStartTime = time(NULL);
}

/*
 * HandleChildCrash -- cleanup after failed backend, bgwriter, checkpointer,
 * walwriter, autovacuum, archiver, slot sync worker, or background worker.
 *
 * The objectives here are to clean up our local state about the child
 * process, and to signal all other remaining children to quickdie.
 *
 * The caller has already released its PMChild slot.
 *
 * HandleChildCrash —— 在后端、bgwriter、检查点进程、walwriter、自动清理、
 * 归档进程、槽同步工作进程或后台工作进程失败后进行清理。
 * 这里的目标是清理关于该子进程的本地状态，并向所有其他剩余子进程发出 
 * 信号以执行快速死亡（quickdie）。
 * 调用者必须已经释放了其 PMChild 槽位。
 */
static void
HandleChildCrash(int pid, int exitstatus, const char *procname)
{
	/*
	 * We only log messages and send signals if this is the first process
	 * crash and we're not doing an immediate shutdown; otherwise, we're only
	 * here to update postmaster's idea of live processes.  If we have already
	 * signaled children, nonzero exit status is to be expected, so don't
	 * clutter log.
	 *
	 * 我们只在这是第一次进程崩溃且我们不是在执行立即关闭时才记录消息并 
	 * 发送信号；否则，我们在这里只是为了更新 Postmaster 对活动进程的 
	 * 掌握情况。如果我们已经向子进程发送了信号，非零退出状态是预料之中的， 
	 * 因此不要弄乱日志。
	 */
	if (FatalError || Shutdown == ImmediateShutdown)
		return;

	LogChildExit(LOG, procname, pid, exitstatus);
	ereport(LOG,
			(errmsg("terminating any other active server processes")));

	/*
	 * Switch into error state. The crashed process has already been removed
	 * from ActiveChildList.
	 * 切换到错误状态。崩溃的进程已经从 ActiveChildList 中移除。
	 */
	HandleFatalError(PMQUIT_FOR_CRASH, true);
}

/*
 * Log the death of a child process.
 * 记录子进程的死亡情况。
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	/*
	 * size of activity_buffer is arbitrary, but set equal to default
	 * track_activity_query_size
	 * activity_buffer 的大小是任意的，但设置为等于默认的 track_activity_query_size
	 */
	char		activity_buffer[1024];
	const char *activity = NULL;

	if (!EXIT_STATUS_0(exitstatus))
		activity = pgstat_get_crashed_backend_activity(pid,
													   activity_buffer,
													   sizeof(activity_buffer));

	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" 
		  翻译： %s 是描述子进程的名词短语，如 "server process"（服务器进程）*/
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus)),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
	else if (WIFSIGNALED(exitstatus))
	{
#if defined(WIN32)
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" 
		  翻译： %s 是描述子进程的名词短语，如 "server process"（服务器进程）*/
				(errmsg("%s (PID %d) was terminated by exception 0x%X",
						procname, pid, WTERMSIG(exitstatus)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" 
		  翻译： %s 是描述子进程的名词短语，如 "server process"（服务器进程）*/
				(errmsg("%s (PID %d) was terminated by signal %d: %s",
						procname, pid, WTERMSIG(exitstatus),
						pg_strsignal(WTERMSIG(exitstatus))),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#endif
	}
	else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" 
		  翻译： %s 是描述子进程的名词短语，如 "server process"（服务器进程）*/
				(errmsg("%s (PID %d) exited with unrecognized status %d",
						procname, pid, exitstatus),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
}

/*
 * Advance the postmaster's state machine and take actions as appropriate
 *
 * This is common code for process_pm_shutdown_request(),
 * process_pm_child_exit() and process_pm_pmsignal(), which process the signals
 * that might mean we need to change state.
 *
 * 推进 Postmaster 的状态机并采取适当的操作。
 * 这是 process_pm_shutdown_request()、process_pm_child_exit() 和 
 * process_pm_pmsignal() 的通用代码，它们处理可能意味着我们需要更改状态的信号。
 */
static void
PostmasterStateMachine(void)
{
	/* If we're doing a smart shutdown, try to advance that state. */
	/* 如果我们正在执行智能关闭，请尝试推进该状态。 */
	if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
	{
		if (!connsAllowed)
		{
			/*
			 * This state ends when we have no normal client backends running.
			 * Then we're ready to stop other children.
			 *
			 * 当没有正常的客户端后端运行时，此状态结束。然后我们准备停止其他子进程。
			 */
			if (CountChildren(btmask(B_BACKEND)) == 0)
				UpdatePMState(PM_STOP_BACKENDS);
		}
	}

	/*
	 * In the PM_WAIT_BACKENDS state, wait for all the regular backends and
	 * processes like autovacuum and background workers that are comparable to
	 * backends to exit.
	 *
	 * 在 PM_WAIT_BACKENDS 状态下，等待所有常规后端进程以及类似于后端的
	 * 进程（如自动清理和后台工作进程）退出。
	 *
	 * PM_STOP_BACKENDS is a transient state that means the same as
	 * PM_WAIT_BACKENDS, but we signal the processes first, before waiting for
	 * them.  Treating it as a distinct pmState allows us to share this code
	 * across multiple shutdown code paths.
	 *
	 * PM_STOP_BACKENDS 是一个过渡状态，其含义与 PM_WAIT_BACKENDS 
	 * 相同，但我们在等待这些进程之前先向它们发送信号。将其视为独立的 
	 * pmState 允许我们在多个关闭代码路径中共享此代码。
	 */
	if (pmState == PM_STOP_BACKENDS || pmState == PM_WAIT_BACKENDS)
	{
		BackendTypeMask targetMask = BTYPE_MASK_NONE;

		/*
		 * PM_WAIT_BACKENDS state ends when we have no regular backends, no
		 * autovac launcher or workers, and no bgworkers (including
		 * unconnected ones).
		 *
		 * 当没有常规后端、没有自动清理启动器或工作进程，也没有后台工作进程
		 *（包括未连接的进程）时，PM_WAIT_BACKENDS 状态结束。
		 */
		targetMask = btmask_add(targetMask,
								B_BACKEND,
								B_AUTOVAC_LAUNCHER,
								B_AUTOVAC_WORKER,
								B_BG_WORKER);

		/*
		 * No walwriter, bgwriter, slot sync worker, or WAL summarizer either.
		 * 也没有 WAL 写进程、后台写进程、槽同步工作进程或 WAL 汇总进程。
		 */
		targetMask = btmask_add(targetMask,
								B_WAL_WRITER,
								B_BG_WRITER,
								B_SLOTSYNC_WORKER,
								B_WAL_SUMMARIZER);

		/* If we're in recovery, also stop startup and walreceiver procs */
		/* 如果我们处于恢复模式，也要停止启动进程和 WAL 接收进程 */
		targetMask = btmask_add(targetMask,
								B_STARTUP,
								B_WAL_RECEIVER);

		/*
		 * If we are doing crash recovery or an immediate shutdown then we
		 * expect archiver, checkpointer, io workers and walsender to exit as
		 * well, otherwise not.
		 *
		 * 如果我们正在执行崩溃恢复或立即关闭，那么我们希望归档进程、 
		 * 检查点进程、IO 工作进程和 walsender 也退出，否则不退出。
		 */
		if (FatalError || Shutdown >= ImmediateShutdown)
			targetMask = btmask_add(targetMask,
									B_CHECKPOINTER,
									B_ARCHIVER,
									B_IO_WORKER,
									B_WAL_SENDER);

		/*
		 * Normally archiver, checkpointer, IO workers and walsenders will
		 * continue running; they will be terminated later after writing the
		 * checkpoint record.  We also let dead-end children to keep running
		 * for now.  The syslogger process exits last.
		 *
		 * 通常归档进程、检查点进程、IO 工作进程和 walsender 将继续运行； 
		 * 它们将在稍后写入检查点记录后被终止。我们现在也让“死路（dead-end）” 
		 * 子进程继续运行。系统日志进程最后退出。
		 *
		 * This assertion checks that we have covered all backend types,
		 * either by including them in targetMask, or by noting here that they
		 * are allowed to continue running.
		 * 
		 * 此断言检查我们是否涵盖了所有后端类型，要么将它们包含在 targetMask 
		 * 中，要么在此注明允许它们继续运行。
		 */
#ifdef USE_ASSERT_CHECKING
		{
			BackendTypeMask remainMask = BTYPE_MASK_NONE;

			remainMask = btmask_add(remainMask,
									B_DEAD_END_BACKEND,
									B_LOGGER);

			/*
			 * Archiver, checkpointer, IO workers, and walsender may or may
			 * not be in targetMask already.
			 * 归档进程、检查点进程、IO 工作进程和 walsender 可能已经在 
			 * targetMask 中，也可能不在。
			 */
			remainMask = btmask_add(remainMask,
									B_ARCHIVER,
									B_CHECKPOINTER,
									B_IO_WORKER,
									B_WAL_SENDER);

			/* these are not real postmaster children */
			/* 这些不是真正的 Postmaster 子进程 */
			remainMask = btmask_add(remainMask,
									B_INVALID,
									B_STANDALONE_BACKEND);

			/* All types should be included in targetMask or remainMask */
			/* 所有的类型都应该包含在 targetMask 或 remainMask 中 */
			Assert((remainMask.mask | targetMask.mask) == BTYPE_MASK_ALL.mask);
		}
#endif

		/* If we had not yet signaled the processes to exit, do so now */
		/* 如果我们还没有向进程发送退出信号，请现在执行 */
		if (pmState == PM_STOP_BACKENDS)
		{
			/*
			 * Forget any pending requests for background workers, since we're
			 * no longer willing to launch any new workers.  (If additional
			 * requests arrive, BackgroundWorkerStateChange will reject them.)
			 *
			 * 遗忘任何挂起的后台工作进程请求，因为我们不再愿意启动任何新工作
			 * 进程。（如果有额外请求到达，BackgroundWorkerStateChange 将拒绝它们。）
			 */
			ForgetUnstartedBackgroundWorkers();

			SignalChildren(SIGTERM, targetMask);

			UpdatePMState(PM_WAIT_BACKENDS);
		}

		/* Are any of the target processes still running? */
		/* 还有任何目标进程在运行吗？ */
		if (CountChildren(targetMask) == 0)
		{
			if (Shutdown >= ImmediateShutdown || FatalError)
			{
				/*
				 * Stop any dead-end children and stop creating new ones.
				 *
				 * NB: Similar code exists in HandleFatalError(), when the
				 * error happens in pmState > PM_WAIT_BACKENDS.
				 *
				 * 停止任何“死路（dead-end）”子进程并停止创建新进程。
				 * 注意：如果错误发生在 pmState > PM_WAIT_BACKENDS 期间， 
				 * HandleFatalError() 中也存在类似代码。
				 */
				UpdatePMState(PM_WAIT_DEAD_END);
				ConfigurePostmasterWaitSet(false);
				SignalChildren(SIGQUIT, btmask(B_DEAD_END_BACKEND));

				/*
				 * We already SIGQUIT'd auxiliary processes (other than
				 * logger), if any, when we started immediate shutdown or
				 * entered FatalError state.
				 * 
				 * 当我们开始立即关闭或进入 FatalError 状态时，如果我们已经 
				 * 向辅助进程（日志进程除外）发送了 SIGQUIT 信号。
				 */
			}
			else
			{
				/*
				 * If we get here, we are proceeding with normal shutdown. All
				 * the regular children are gone, and it's time to tell the
				 * checkpointer to do a shutdown checkpoint.
				 *
				 * 如果我们到达这里，说明我们正在进行正常关闭。所有常规子进程
				 * 都已消失，是时候告诉检查点进程执行关闭检查点了。
				 */
				Assert(Shutdown > NoShutdown);
				/* Start the checkpointer if not running */
				/* 如果检查点进程未运行，则启动它 */
				if (CheckpointerPMChild == NULL)
					CheckpointerPMChild = StartChildProcess(B_CHECKPOINTER);
				/* And tell it to write the shutdown checkpoint */
				/* 并告知其写入关闭检查点 */
				if (CheckpointerPMChild != NULL)
				{
					signal_child(CheckpointerPMChild, SIGINT);
					UpdatePMState(PM_WAIT_XLOG_SHUTDOWN);
				}
				else
				{
					/*
					 * If we failed to fork a checkpointer, just shut down.
					 * Any required cleanup will happen at next restart. We
					 * set FatalError so that an "abnormal shutdown" message
					 * gets logged when we exit.
					 *
					 * 如果我们无法 fork 检查点进程，就直接关闭。 
					 * 任何所需的清理都将在下次启动时进行。我们设置了 FatalError， 
					 * 以便在退出时记录一条“异常关闭”消息。
					 *
					 * We don't consult send_abort_for_crash here, as it's
					 * unlikely that dumping cores would illuminate the reason
					 * for checkpointer fork failure.
					 * 
					 * 我们在这里不参考 send_abort_for_crash，因为 dump core 
					 * 不太可能解释检查点进程 fork 失败的原因。
					 *
					 * XXX: It may be worth to introduce a different PMQUIT
					 * value that signals that the cluster is in a bad state,
					 * without a process having crashed. But right now this
					 * path is very unlikely to be reached, so it isn't
					 * obviously worthwhile adding a distinct error message in
					 * quickdie().
					 *
					 * XXX：引入一个不同的 PMQUIT 值来指示集群处于糟糕状态（即便没有进程崩溃）
					 * 也许是值得的。但目前这条路径极不可能被触及，因此在 
					 * quickdie() 中添加一条独特的错误消息显然不值得。
					 */
					HandleFatalError(PMQUIT_FOR_CRASH, false);
				}
			}
		}
	}

	/*
	 * The state transition from PM_WAIT_XLOG_SHUTDOWN to
	 * PM_WAIT_XLOG_ARCHIVAL is in process_pm_pmsignal(), in response to
	 * PMSIGNAL_XLOG_IS_SHUTDOWN.
	 *
	 * 从 PM_WAIT_XLOG_SHUTDOWN 到 PM_WAIT_XLOG_ARCHIVAL 的状态转换 
	 * 发生在 process_pm_pmsignal() 中，以响应 PMSIGNAL_XLOG_IS_SHUTDOWN。
	 */

	if (pmState == PM_WAIT_XLOG_ARCHIVAL)
	{
		/*
		 * PM_WAIT_XLOG_ARCHIVAL state ends when there are no children other
		 * than checkpointer, io workers and dead-end children left. There
		 * shouldn't be any regular backends left by now anyway; what we're
		 * really waiting for is for walsenders and archiver to exit.
		 *
		 * 当除了检查点进程、IO 工作进程和“死路（dead-end）”子进程外，没有任何
		 * 其他子进程留下时，PM_WAIT_XLOG_ARCHIVAL 状态结束。到目前为止， 
		 * 无论如何都不应该剩下任何常规后端；我们真正等待的是 walsender 和归档进程退出。
		 */
		if (CountChildren(btmask_all_except(B_CHECKPOINTER, B_IO_WORKER,
											B_LOGGER, B_DEAD_END_BACKEND)) == 0)
		{
			UpdatePMState(PM_WAIT_IO_WORKERS);
			SignalChildren(SIGUSR2, btmask(B_IO_WORKER));
		}
	}

	if (pmState == PM_WAIT_IO_WORKERS)
	{
		/*
		 * PM_WAIT_IO_WORKERS state ends when there's only checkpointer and
		 * dead-end children left.
		 * 当只剩下检查点进程和“死路（dead-end）”子进程时，PM_WAIT_IO_WORKERS 状态结束。
		 */
		if (io_worker_count == 0)
		{
			UpdatePMState(PM_WAIT_CHECKPOINTER);

			/*
			 * Now that the processes mentioned above are gone, tell
			 * checkpointer to shut down too. That allows checkpointer to
			 * perform some last bits of cleanup without other processes
			 * interfering.
			 *
			 * 既然上述进程已经消失，也要告诉检查点进程关闭。这允许检查点进程 
			 * 在没有其他进程干扰的情况下执行最后一点清理工作。
			 */
			if (CheckpointerPMChild != NULL)
				signal_child(CheckpointerPMChild, SIGUSR2);
		}
	}

	/*
	 * The state transition from PM_WAIT_CHECKPOINTER to PM_WAIT_DEAD_END is
	 * in process_pm_child_exit().
	 *
	 * 从 PM_WAIT_CHECKPOINTER 到 PM_WAIT_DEAD_END 的状态转换 
	 * 发生在 process_pm_child_exit() 中。
	 */

	if (pmState == PM_WAIT_DEAD_END)
	{
		/*
		 * PM_WAIT_DEAD_END state ends when all other children are gone except
		 * for the logger.  During normal shutdown, all that remains are
		 * dead-end backends, but in FatalError processing we jump straight
		 * here with more processes remaining.  Note that they have already
		 * been sent appropriate shutdown signals, either during a normal
		 * state transition leading up to PM_WAIT_DEAD_END, or during
		 * FatalError processing.
		 *
		 * The reason we wait is to protect against a new postmaster starting
		 * conflicting subprocesses; this isn't an ironclad protection, but it
		 * at least helps in the shutdown-and-immediately-restart scenario.
		 */
		if (CountChildren(btmask_all_except(B_LOGGER)) == 0)
		{
			/* These other guys should be dead already */
			/* 这些其他进程应该已经死掉了 */
			Assert(StartupPMChild == NULL);
			Assert(WalReceiverPMChild == NULL);
			Assert(WalSummarizerPMChild == NULL);
			Assert(BgWriterPMChild == NULL);
			Assert(CheckpointerPMChild == NULL);
			Assert(WalWriterPMChild == NULL);
			Assert(AutoVacLauncherPMChild == NULL);
			Assert(SlotSyncWorkerPMChild == NULL);
			/* syslogger is not considered here */
			/* syslogger 在这里不予考虑 */
			UpdatePMState(PM_NO_CHILDREN);
		}
	}

	/*
	 * If we've been told to shut down, we exit as soon as there are no
	 * remaining children.  If there was a crash, cleanup will occur at the
	 * next startup.  (Before PostgreSQL 8.3, we tried to recover from the
	 * crash before exiting, but that seems unwise if we are quitting because
	 * we got SIGTERM from init --- there may well not be time for recovery
	 * before init decides to SIGKILL us.)
	 *
	 * Note that the syslogger continues to run.  It will exit when it sees
	 * EOF on its input pipe, which happens when there are no more upstream
	 * processes.
	 */
	if (Shutdown > NoShutdown && pmState == PM_NO_CHILDREN)
	{
		if (FatalError)
		{
			ereport(LOG, (errmsg("abnormal database system shutdown")));
			ExitPostmaster(1);
		}
		else
		{
			/*
			 * Normal exit from the postmaster is here.  We don't need to log
			 * anything here, since the UnlinkLockFiles proc_exit callback
			 * will do so, and that should be the last user-visible action.
			 *
			 * Postmaster 的正常退出就在这里。我们不需要在这里记录任何内容， 
			 * 因为 UnlinkLockFiles proc_exit 回调会这样做，而那应该是最后 
			 * 一个用户可见的操作。
			 */
			ExitPostmaster(0);
		}
	}

	/*
	 * If the startup process failed, or the user does not want an automatic
	 * restart after backend crashes, wait for all non-syslogger children to
	 * exit, and then exit postmaster.  We don't try to reinitialize when the
	 * startup process fails, because more than likely it will just fail again
	 * and we will keep trying forever.
	 *
	 * 如果启动进程失败，或者用户不希望在后端崩溃后自动重启，请等待所有 
	 * 非日志进程子进程退出，然后退出 Postmaster。当启动进程失败时， 
	 * 我们不尝试重新初始化，因为很可能它只会再次失败，我们将永远尝试下去。
	 */
	if (pmState == PM_NO_CHILDREN)
	{
		if (StartupStatus == STARTUP_CRASHED)
		{
			ereport(LOG,
					(errmsg("shutting down due to startup process failure")));
			ExitPostmaster(1);
		}
		if (!restart_after_crash)
		{
			ereport(LOG,
					(errmsg("shutting down because \"restart_after_crash\" is off")));
			ExitPostmaster(1);
		}
	}

	/*
	 * If we need to recover from a crash, wait for all non-syslogger children
	 * to exit, then reset shmem and start the startup process.
	 * 
	 * 如果我们需要从崩溃中恢复，请等待所有非日志进程子进程退出， 
	 * 然后重置共享内存并启动测试启动进程。
	 */
	if (FatalError && pmState == PM_NO_CHILDREN)
	{
		ereport(LOG,
				(errmsg("all server processes terminated; reinitializing")));

		/* remove leftover temporary files after a crash */
		/* 在崩溃后删除残留的临时文件 */
		if (remove_temp_files_after_crash)
			RemovePgTempFiles();

		/* allow background workers to immediately restart */
		/* 允许后台工作进程立即重启 */
		ResetBackgroundWorkerCrashTimes();

		shmem_exit(1);

		/* re-read control file into local memory */
		/* 将控制文件重新读入本地内存 */
		LocalProcessControlFile(true);

		/* re-create shared memory and semaphores */
		/* 重新创建共享内存和信号量 */
		CreateSharedMemoryAndSemaphores();

		UpdatePMState(PM_STARTUP);

		/* Make sure we can perform I/O while starting up. */
		/* 确保我们在启动时可以执行 I/O。 */
		maybe_adjust_io_workers();

		StartupPMChild = StartChildProcess(B_STARTUP);
		Assert(StartupPMChild != NULL);
		StartupStatus = STARTUP_RUNNING;
		/* crash recovery started, reset SIGKILL flag */
		/* 崩溃恢复已开始，重置 SIGKILL 标志 */
		AbortStartTime = 0;

		/* start accepting server socket connection events again */
		/* 重新开始接受服务器套接字连接事件 */
		ConfigurePostmasterWaitSet(true);
	}
}

static const char *
pmstate_name(PMState state)
{
#define PM_TOSTR_CASE(sym) case sym: return #sym
	switch (state)
	{
			PM_TOSTR_CASE(PM_INIT);
			PM_TOSTR_CASE(PM_STARTUP);
			PM_TOSTR_CASE(PM_RECOVERY);
			PM_TOSTR_CASE(PM_HOT_STANDBY);
			PM_TOSTR_CASE(PM_RUN);
			PM_TOSTR_CASE(PM_STOP_BACKENDS);
			PM_TOSTR_CASE(PM_WAIT_BACKENDS);
			PM_TOSTR_CASE(PM_WAIT_XLOG_SHUTDOWN);
			PM_TOSTR_CASE(PM_WAIT_XLOG_ARCHIVAL);
			PM_TOSTR_CASE(PM_WAIT_IO_WORKERS);
			PM_TOSTR_CASE(PM_WAIT_DEAD_END);
			PM_TOSTR_CASE(PM_WAIT_CHECKPOINTER);
			PM_TOSTR_CASE(PM_NO_CHILDREN);
	}
#undef PM_TOSTR_CASE

	pg_unreachable();
	return "";					/* silence compiler */
}

/*
 * Simple wrapper for updating pmState. The main reason to have this wrapper
 * is that it makes it easy to log all state transitions.
 *
 * 更新 pmState 的简单包装器。拥有此包装器的主要原因是由于它
 * 可以轻松记录所有状态转换。
 */
static void
UpdatePMState(PMState newState)
{
	elog(DEBUG1, "updating PMState from %s to %s",
		 pmstate_name(pmState), pmstate_name(newState));
	pmState = newState;
}

/*
 * Launch background processes after state change, or relaunch after an
 * existing process has exited.
 *
 * Check the current pmState and the status of any background processes.  If
 * there are any background processes missing that should be running in the
 * current state, but are not, launch them.
 *
 * 状态更改后启动后台进程，或者在现有进程退出后重新启动。
 * 检查当前 pmState 和任何后台进程的状态。如果缺少在当前状态下应运行 
 * 但未运行的任何后台进程，请启动它们。
 */
static void
LaunchMissingBackgroundProcesses(void)
{
	/* Syslogger is active in all states */
	/* 所有的状态中，日志进程（Syslogger）都是活动的 */
	if (SysLoggerPMChild == NULL && Logging_collector)
		StartSysLogger();

	/*
	 * The number of configured workers might have changed, or a prior start
	 * of a worker might have failed. Check if we need to start/stop any
	 * workers.
	 *
	 * A config file change will always lead to this function being called, so
	 * we always will process the config change in a timely manner.
	 *
	 * 配置的工作进程数量可能已更改，或者之前的某次启动失败。检查我们 
	 * 是否需要启动/停止任何工作进程。 
	 * 配置文件更改将始终导致调用此函数，因此我们将始终及时处理配置更改。
	 */
	maybe_adjust_io_workers();

	/*
	 * The checkpointer and the background writer are active from the start,
	 * until shutdown is initiated.
	 *
	 * (If the checkpointer is not running when we enter the
	 * PM_WAIT_XLOG_SHUTDOWN state, it is launched one more time to perform
	 * the shutdown checkpoint.  That's done in PostmasterStateMachine(), not
	 * here.)
	 *
	 * 检查点进程和后台写进程从一开始就处于活动状态，直到启动关闭。
	 * （如果我们在进入 PM_WAIT_XLOG_SHUTDOWN 状态时检查点进程未运行， 
	 * 则会再次启动它一次以执行关闭检查点。这在 PostmasterStateMachine() 
	 * 中完成，而不是在这里。）
	 */
	if (pmState == PM_RUN || pmState == PM_RECOVERY ||
		pmState == PM_HOT_STANDBY || pmState == PM_STARTUP)
	{
		if (CheckpointerPMChild == NULL)
			CheckpointerPMChild = StartChildProcess(B_CHECKPOINTER);
		if (BgWriterPMChild == NULL)
			BgWriterPMChild = StartChildProcess(B_BG_WRITER);
	}

	/*
	 * WAL writer is needed only in normal operation (else we cannot be
	 * writing any new WAL).
	 * WAL 写进程仅在正常操作中需要（否则我们无法写入任何新的 WAL）。
	 */
	if (WalWriterPMChild == NULL && pmState == PM_RUN)
		WalWriterPMChild = StartChildProcess(B_WAL_WRITER);

	/*
	 * We don't want autovacuum to run in binary upgrade mode because
	 * autovacuum might update relfrozenxid for empty tables before the
	 * physical files are put in place.
	 *
	 * 我们不希望自动清理在二进制升级模式下运行，因为在放置物理文件之前， 
	 * 自动清理可能会更新空表的 relfrozenxid。
	 */
	if (!IsBinaryUpgrade && AutoVacLauncherPMChild == NULL &&
		(AutoVacuumingActive() || start_autovac_launcher) &&
		pmState == PM_RUN)
	{
		AutoVacLauncherPMChild = StartChildProcess(B_AUTOVAC_LAUNCHER);
		if (AutoVacLauncherPMChild != NULL)
			start_autovac_launcher = false; /* signal processed */
											/* 信号已处理 */
	}

	/*
	 * If WAL archiving is enabled always, we are allowed to start archiver
	 * even during recovery.
	 *
	 * 如果始终启用 WAL 归档，我们甚至允许在恢复期间启动归档进程。
	 */
	if (PgArchPMChild == NULL &&
		((XLogArchivingActive() && pmState == PM_RUN) ||
		 (XLogArchivingAlways() && (pmState == PM_RECOVERY || pmState == PM_HOT_STANDBY))) &&
		PgArchCanRestart())
		PgArchPMChild = StartChildProcess(B_ARCHIVER);

	/*
	 * If we need to start a slot sync worker, try to do that now
	 *
	 * We allow to start the slot sync worker when we are on a hot standby,
	 * fast or immediate shutdown is not in progress, slot sync parameters are
	 * configured correctly, and it is the first time of worker's launch, or
	 * enough time has passed since the worker was launched last.
	 *
	 * 如果我们需要启动槽（slot）同步工作进程，请立即尝试。
	 * 当我们处于热备状态、快速或立即关闭尚未在进行中、槽同步参数配置正确、 
	 * 且该进程是第一次启动或自上次启动以来已过去足够时间时， 
	 * 我们允许启动槽同步工作进程。
	 */
	if (SlotSyncWorkerPMChild == NULL && pmState == PM_HOT_STANDBY &&
		Shutdown <= SmartShutdown && sync_replication_slots &&
		ValidateSlotSyncParams(LOG) && SlotSyncWorkerCanRestart())
		SlotSyncWorkerPMChild = StartChildProcess(B_SLOTSYNC_WORKER);

	/*
	 * If we need to start a WAL receiver, try to do that now
	 *
	 * Note: if a walreceiver process is already running, it might seem that
	 * we should clear WalReceiverRequested.  However, there's a race
	 * condition if the walreceiver terminates and the startup process
	 * immediately requests a new one: it's quite possible to get the signal
	 * for the request before reaping the dead walreceiver process.  Better to
	 * risk launching an extra walreceiver than to miss launching one we need.
	 * (The walreceiver code has logic to recognize that it should go away if
	 * not needed.)
	 *
	 * 如果我们需要启动 WAL 接收进程，请尝试立即执行。
	 * 注意：如果 WAL 接收进程已经在运行，似乎我们应该清除 WalReceiverRequested。 
	 * 然而，如果 WAL 接收进程终止而启动进程立即请求一个新的， 
	 * 则存在竞争条件：极有可能在回收死亡的 WAL 接收进程之前就收到了请求信号。 
	 * 风险启动一个额外的 WAL 接收进程比错过启动一个我们需要的进程要好。 
	 * （WAL 接收进程代码具有识别如果不需要则应退出的逻辑。）
	 */
	if (WalReceiverRequested)
	{
		if (WalReceiverPMChild == NULL &&
			(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
			 pmState == PM_HOT_STANDBY) &&
			Shutdown <= SmartShutdown)
		{
			WalReceiverPMChild = StartChildProcess(B_WAL_RECEIVER);
			if (WalReceiverPMChild != 0)
				WalReceiverRequested = false;
			/* else leave the flag set, so we'll try again later */
		}
	}

	/* If we need to start a WAL summarizer, try to do that now */
	/* 如果我们需要启动 WAL 汇总进程，请立即尝试 */
	if (summarize_wal && WalSummarizerPMChild == NULL &&
		(pmState == PM_RUN || pmState == PM_HOT_STANDBY) &&
		Shutdown <= SmartShutdown)
		WalSummarizerPMChild = StartChildProcess(B_WAL_SUMMARIZER);

	/* Get other worker processes running, if needed */
	/* 如果需要，运行其他工作进程 */
	if (StartWorkerNeeded || HaveCrashedWorker)
		maybe_start_bgworkers();
}

/*
 * Return string representation of signal.
 * 
 * 返回信号的字符串表示形式。
 * 
 * Because this is only implemented for signals we already rely on in this
 * file we don't need to deal with unimplemented or same-numeric-value signals
 * (as we'd e.g. have to for EWOULDBLOCK / EAGAIN).
 *
 * 由于这仅针对我们在本文件中已经依赖的信号实现，因此我们不需要处理未实现 
 * 或具有相同数值的信号（例如我们必须为 EWOULDBLOCK / EAGAIN 处理的情况）。
 */
static const char *
pm_signame(int signal)
{
#define PM_TOSTR_CASE(sym) case sym: return #sym
	switch (signal)
	{
			PM_TOSTR_CASE(SIGABRT);
			PM_TOSTR_CASE(SIGCHLD);
			PM_TOSTR_CASE(SIGHUP);
			PM_TOSTR_CASE(SIGINT);
			PM_TOSTR_CASE(SIGKILL);
			PM_TOSTR_CASE(SIGQUIT);
			PM_TOSTR_CASE(SIGTERM);
			PM_TOSTR_CASE(SIGUSR1);
			PM_TOSTR_CASE(SIGUSR2);
		default:
			/* all signals sent by postmaster should be listed here */
			Assert(false);
			return "(unknown)";
	}
#undef PM_TOSTR_CASE

	return "";					/* silence compiler */
}

/*
 * Send a signal to a postmaster child process
 * 
 * 向 Postmaster 子进程发送信号。
 *
 * On systems that have setsid(), each child process sets itself up as a
 * process group leader.  For signals that are generally interpreted in the
 * appropriate fashion, we signal the entire process group not just the
 * direct child process.  This allows us to, for example, SIGQUIT a blocked
 * archive_recovery script, or SIGINT a script being run by a backend via
 * system().
 *
 * 在具有 setsid() 的系统上，每个子进程都将自己设置为进程组组长。 
 * 对于通常以适当方式解释的信号，我们向整个进程组发送信号，而不仅仅是 
 * 直接子进程。例如，这允许我们 SIGQUIT 一个阻塞的 archive_recovery 脚本， 
 * 或者 SIGINT 一个由后端通过 system() 运行的脚本。
 *
 * There is a race condition for recently-forked children: they might not
 * have executed setsid() yet.  So we signal the child directly as well as
 * the group.  We assume such a child will handle the signal before trying
 * to spawn any grandchild processes.  We also assume that signaling the
 * child twice will not cause any problems.
 *
 * 对于最近 fork 的子进程存在竞争条件：它们可能尚未执行 setsid()。 
 * 因此我们直接向子进程及其所在组发送信号。我们假设这样的子进程在尝试 
 * 产生任何孙子进程之前将处理该信号。我们还假设向子进程发送两次信号 
 * 不会引起任何问题。
 */
static void
signal_child(PMChild *pmchild, int signal)
{
	pid_t		pid = pmchild->pid;

	ereport(DEBUG3,
			(errmsg_internal("sending signal %d/%s to %s process with pid %d",
							 signal, pm_signame(signal),
							 GetBackendTypeDesc(pmchild->bkend_type),
							 (int) pmchild->pid)));

	if (kill(pid, signal) < 0)
		elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) pid, signal);
#ifdef HAVE_SETSID
	switch (signal)
	{
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
		case SIGKILL:
		case SIGABRT:
			if (kill(-pid, signal) < 0)
				elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) (-pid), signal);
			break;
		default:
			break;
	}
#endif
}

/*
 * Send a signal to the targeted children.
 * 向目标子进程发送信号。
 */
static bool
SignalChildren(int signal, BackendTypeMask targetMask)
{
	dlist_iter	iter;
	bool		signaled = false;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		/*
		 * If we need to distinguish between B_BACKEND and B_WAL_SENDER, check
		 * if any B_BACKEND backends have recently announced that they are
		 * actually WAL senders.
		 *
		 * 如果我们需要区分 B_BACKEND 和 B_WAL_SENDER，请检查是否有任何 B_BACKEND 
		 * 后端最近宣布它们实际上是 WAL 发送进程（walsender）。
		 */
		if (btmask_contains(targetMask, B_WAL_SENDER) != btmask_contains(targetMask, B_BACKEND) &&
			bp->bkend_type == B_BACKEND)
		{
			if (IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = B_WAL_SENDER;
		}

		if (!btmask_contains(targetMask, bp->bkend_type))
			continue;

		signal_child(bp, signal);
		signaled = true;
	}
	return signaled;
}

/*
 * Send a termination signal to children.  This considers all of our children
 * processes, except syslogger.
 *
 * 向子进程发送终止信号。这考虑了我们所有的子进程，系统日志进程除外。
 */
static void
TerminateChildren(int signal)
{
	SignalChildren(signal, btmask_all_except(B_LOGGER));
	if (StartupPMChild != NULL)
	{
		if (signal == SIGQUIT || signal == SIGKILL || signal == SIGABRT)
			StartupStatus = STARTUP_SIGNALED;
	}
}

/*
 * BackendStartup -- start backend process
 *
 * returns: STATUS_ERROR if the fork failed, STATUS_OK otherwise.
 *
 * Note: if you change this code, also consider StartAutovacuumWorker and
 * StartBackgroundWorker.
 *
 * BackendStartup —— 启动后端进程
 * 返回值：如果 fork 失败则返回 STATUS_ERROR，否则返回 STATUS_OK。
 * 注意：如果您更改此代码，另请考虑 StartAutovacuumWorker 和 StartBackgroundWorker。
 */
static int
BackendStartup(ClientSocket *client_sock)
{
	PMChild    *bn = NULL;
	pid_t		pid;
	BackendStartupData startup_data;
	CAC_state	cac;

	/*
	 * Capture time that Postmaster got a socket from accept (for logging
	 * connection establishment and setup total duration).
	 *
	 * 捕获 Postmaster 从 accept 获取套接字的时间（用于记录连接建立和设置的总持续时间）。
	 */
	startup_data.socket_created = GetCurrentTimestamp();

	/*
	 * Allocate and assign the child slot.  Note we must do this before
	 * forking, so that we can handle failures (out of memory or child-process
	 * slots) cleanly.
	 *
	 * 分配并指派子进程槽位。请注意，我们必须在 fork 之前执行此操作，
	 * 以便我们可以干净地处理失败（内存不足或子进程槽位已满）。
	 */
	cac = canAcceptConnections(B_BACKEND);
	if (cac == CAC_OK)
	{
		/* Can change later to B_WAL_SENDER */
		/* 稍后可能更改为 B_WAL_SENDER */
		bn = AssignPostmasterChildSlot(B_BACKEND);
		if (!bn)
		{
			/*
			 * Too many regular child processes; launch a dead-end child
			 * process instead.
			 * 常规子进程过多；改为启动一个“死路（dead-end）”子进程。
			 */
			cac = CAC_TOOMANY;
		}
	}
	if (!bn)
	{
		bn = AllocDeadEndChild();
		if (!bn)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			return STATUS_ERROR;
		}
	}

	/* Pass down canAcceptConnections state */
	/* 传递 canAcceptConnections 状态 */
	startup_data.canAcceptConnections = cac;
	bn->rw = NULL;

	/* Hasn't asked to be notified about any bgworkers yet */
	/* 尚未要求接收关于任何后台工作进程（bgworker）的通知 */
	bn->bgworker_notify = false;

	pid = postmaster_child_launch(bn->bkend_type, bn->child_slot,
								  &startup_data, sizeof(startup_data),
								  client_sock);
	if (pid < 0)
	{
		/* in parent, fork failed */
		/* 在父进程中，fork 失败 */
		int			save_errno = errno;

		(void) ReleasePostmasterChildSlot(bn);
		errno = save_errno;
		ereport(LOG,
				(errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(client_sock, save_errno);
		return STATUS_ERROR;
	}

	/* in parent, successful fork */
	/* 在父进程中，fork 成功 */
	ereport(DEBUG2,
			(errmsg_internal("forked new %s, pid=%d socket=%d",
							 GetBackendTypeDesc(bn->bkend_type),
							 (int) pid, (int) client_sock->sock)));

	/*
	 * Everything's been successful, it's safe to add this backend to our list
	 * of backends.
	 *
	 * 一切顺利，可以将此后端安全地添加到我们的后端列表中。
	 */
	bn->pid = pid;
	return STATUS_OK;
}

/*
 * Try to report backend fork() failure to client before we close the
 * connection.  Since we do not care to risk blocking the postmaster on
 * this connection, we set the connection to non-blocking and try only once.
 *
 * This is grungy special-purpose code; we cannot use backend libpq since
 * it's not up and running.
 *
 * 在关闭连接之前，尝试向客户端报告后端 fork() 失败。由于我们不想承担在 
 * 此连接上阻塞 Postmaster 的风险，我们将连接设置为非阻塞并仅尝试一次。
 * 这是些蹩脚的特殊用途代码；我们不能使用后端 libpq，因为它尚未启动运行。
 */
static void
report_fork_failure_to_client(ClientSocket *client_sock, int errnum)
{
	char		buffer[1000];
	int			rc;

	/* Format the error message packet (always V2 protocol) */
	/* 格式化错误消息包（始终使用 V2 协议） */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 _("could not fork new process for connection: "),
			 strerror(errnum));

	/* Set port to non-blocking.  Don't do send() if this fails */
	/* 将端口设置为非阻塞。如果失败，则不执行 send() */
	if (!pg_set_noblock(client_sock->sock))
		return;

	/* We'll retry after EINTR, but ignore all other failures */
	/* 我们将在 EINTR 后重试，但忽略所有其他失败 */
	do
	{
		rc = send(client_sock->sock, buffer, strlen(buffer) + 1, 0);
	} while (rc < 0 && errno == EINTR);
}

/*
 * ExitPostmaster -- cleanup
 * 
 * ExitPostmaster —— 清理
 *
 * Do NOT call exit() directly --- always go through here!
 * 请勿直接调用 exit() —— 务必通过此处退出！
 */
static void
ExitPostmaster(int status)
{
#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * There is no known cause for a postmaster to become multithreaded after
	 * startup.  However, we might reach here via an error exit before
	 * reaching the test in PostmasterMain, so provide the same hint as there.
	 * This message uses LOG level, because an unclean shutdown at this point
	 * would usually not look much different from a clean shutdown.
	 *
	 * 暂无已知原因会导致 Postmaster 在启动后变成多线程。 
	 * 但是，我们可能会在到达 PostmasterMain 中的测试之前，通过错误退出到达此处， 
	 * 因此请提供与那里相同的提示。此消息使用 LOG 级别，因为此时的不干净关闭 
	 * 通常看起来与干净关闭没有太大区别。
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(LOG,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/* should cleanup shared memory and kill all backends */
	/* 应该清理共享内存并杀死所有后端 */
	/* 应该清理共享内存并杀死所有后端 */

	/*
	 * Not sure of the semantics here.  When the Postmaster dies, should the
	 * backends all be killed? probably not.
	 *
	 * MUST		-- vadim 05-10-1999
	 *
	 * 不确定这里的语义。当 Postmaster 死亡时，是否应该杀死所有后端？可能不应该。
	 * 必须 —— vadim 1999-05-10
	 */

	proc_exit(status);
}

/*
 * Handle pmsignal conditions representing requests from backends,
 * and check for promote and logrotate requests from pg_ctl.
 * 
 * 处理代表后端请求的 pmsignal 条件，并检查来自 pg_ctl 的晋升（promote）
 * 和日志轮转（logrotate）请求。
 */
static void
process_pm_pmsignal(void)
{
	bool		request_state_update = false;

	pending_pm_pmsignal = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received pmsignal signal")));

	/*
	 * RECOVERY_STARTED and BEGIN_HOT_STANDBY signals are ignored in
	 * unexpected states. If the startup process quickly starts up, completes
	 * recovery, exits, we might process the death of the startup process
	 * first. We don't want to go back to recovery in that case.
	 *
	 * RECOVERY_STARTED 和 BEGIN_HOT_STANDBY 信号在意外状态下会被忽略。 
	 * 如果启动进程快速启动、完成恢复并退出，我们可能会先处理启动进程的死亡。 
	 * 在这种情况下，我们不想回到恢复状态。
	 */
	if (CheckPostmasterSignal(PMSIGNAL_RECOVERY_STARTED) &&
		pmState == PM_STARTUP && Shutdown == NoShutdown)
	{
		/* WAL redo has started. We're out of reinitialization. */
		/* WAL 重做已开始。我们已退出重新初始化。 */
		FatalError = false;
		AbortStartTime = 0;
		reachedConsistency = false;

		/*
		 * Start the archiver if we're responsible for (re-)archiving received
		 * files.
		 * 如果我们负责对接收到的文件进行（重新）归档，请启动归档进程。
		 */
		Assert(PgArchPMChild == NULL);
		if (XLogArchivingAlways())
			PgArchPMChild = StartChildProcess(B_ARCHIVER);

		/*
		 * If we aren't planning to enter hot standby mode later, treat
		 * RECOVERY_STARTED as meaning we're out of startup, and report status
		 * accordingly.
		 *
		 * 如果我们不打算稍后进入热备模式，请将 RECOVERY_STARTED 视为 
		 * 我们已退出启动状态，并相应地报告状态。
		 */
		if (!EnableHotStandby)
		{
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STANDBY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif
		}

		UpdatePMState(PM_RECOVERY);
	}

	if (CheckPostmasterSignal(PMSIGNAL_RECOVERY_CONSISTENT) &&
		pmState == PM_RECOVERY && Shutdown == NoShutdown)
	{
		reachedConsistency = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY) &&
		(pmState == PM_RECOVERY && Shutdown == NoShutdown))
	{
		ereport(LOG,
				(errmsg("database system is ready to accept read-only connections")));

		/* Report status */
		AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
		sd_notify(0, "READY=1");
#endif

		UpdatePMState(PM_HOT_STANDBY);
		connsAllowed = true;

		/* Some workers may be scheduled to start now */
		StartWorkerNeeded = true;
	}

	/* Process background worker state changes. */
	/* 处理后台工作进程状态更改。 */
	if (CheckPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE))
	{
		/* Accept new worker requests only if not stopping. */
		/* 仅在未停止时才接受新的工作进程请求。 */
		BackgroundWorkerStateChange(pmState < PM_STOP_BACKENDS);
		StartWorkerNeeded = true;
	}

	/* Tell syslogger to rotate logfile if requested */
	/* 如果有请求，告知 syslogger 轮转日志文件 */
	if (SysLoggerPMChild != NULL)
	{
		if (CheckLogrotateSignal())
		{
			signal_child(SysLoggerPMChild, SIGUSR1);
			RemoveLogrotateSignalFiles();
		}
		else if (CheckPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE))
		{
			signal_child(SysLoggerPMChild, SIGUSR1);
		}
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/*
		 * Start one iteration of the autovacuum daemon, even if autovacuuming
		 * is nominally not enabled.  This is so we can have an active defense
		 * against transaction ID wraparound.  We set a flag for the main loop
		 * to do it rather than trying to do it here --- this is because the
		 * autovac process itself may send the signal, and we want to handle
		 * that by launching another iteration as soon as the current one
		 * completes.
		 *
		 * 启动自动清理守护进程的一次迭代，即使名义上未启用自动清理。 
		 * 这是为了我们能够主动防御事务 ID 回绕（wraparound）。 
		 * 我们为主循环设置一个标志来执行此操作，而不是尝试在这里执行 
		 * —— 这是因为自动清理进程本身可能会发送信号，而我们希望在 
		 * 当前迭代完成后立即启动另一次迭代来处理该信号。
		 */
		start_autovac_launcher = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/* The autovacuum launcher wants us to start a worker process. */
		/* 自动清理启动器希望我们启动一个工作进程。 */
		StartAutovacuumWorker();
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_WALRECEIVER))
	{
		/* Startup Process wants us to start the walreceiver process. */
		/* 启动进程希望我们启动 WAL 接收进程。 */
		WalReceiverRequested = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_XLOG_IS_SHUTDOWN))
	{
		/* Checkpointer completed the shutdown checkpoint */
		/* 检查点进程已完成关闭检查点 */
		if (pmState == PM_WAIT_XLOG_SHUTDOWN)
		{
			/*
			 * If we have an archiver subprocess, tell it to do a last archive
			 * cycle and quit. Likewise, if we have walsender processes, tell
			 * them to send any remaining WAL and quit.
			 *
			 * 如果我们有归档子进程，告知其执行最后一个归档周期并退出。 
			 * 同样，如果我们有 walsender 进程，告知其发送任何剩余的 WAL 并退出。
			 */
			Assert(Shutdown > NoShutdown);

			/* Waken archiver for the last time */
			/* 最后一次唤醒归档进程 */
			if (PgArchPMChild != NULL)
				signal_child(PgArchPMChild, SIGUSR2);

			/*
			 * Waken walsenders for the last time. No regular backends should
			 * be around anymore.
			 * 最后一次唤醒 walsender。此时不应再有任何常规后端。
			 */
			SignalChildren(SIGUSR2, btmask(B_WAL_SENDER));

			UpdatePMState(PM_WAIT_XLOG_ARCHIVAL);
		}
		else if (!FatalError && Shutdown != ImmediateShutdown)
		{
			/*
			 * Checkpointer only ought to perform the shutdown checkpoint
			 * during shutdown.  If somehow checkpointer did so in another
			 * situation, we have no choice but to crash-restart.
			 *
			 * It's possible however that we get PMSIGNAL_XLOG_IS_SHUTDOWN
			 * outside of PM_WAIT_XLOG_SHUTDOWN if an orderly shutdown was
			 * "interrupted" by a crash or an immediate shutdown.
			 *
			 * 检查点进程仅应在关闭期间执行关闭检查点。如果检查点进程在 
			 * 其他情况下执行了该操作，我们别无选择，只能崩溃重启。
			 * 但是，如果正常关闭被崩溃或立即关闭“中断”，我们也有可能在 
			 * PM_WAIT_XLOG_SHUTDOWN 之外收到 PMSIGNAL_XLOG_IS_SHUTDOWN。
			 */
			ereport(LOG,
					(errmsg("WAL was shut down unexpectedly")));

			/*
			 * Doesn't seem likely to help to take send_abort_for_crash into
			 * account here.
			 *
			 * 在这里考虑 send_abort_for_crash 似乎不太可能有帮助。
			 */
			HandleFatalError(PMQUIT_FOR_CRASH, false);
		}

		/*
		 * Need to run PostmasterStateMachine() to check if we already can go
		 * to the next state.
		 * 需要运行 PostmasterStateMachine() 来检查我们是否已经可以进入下一个状态。
		 */
		request_state_update = true;
	}

	/*
	 * Try to advance postmaster's state machine, if a child requests it.
	 * 如果子进程请求，尝试推进 Postmaster 的状态机。
	 */
	if (CheckPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE))
	{
		request_state_update = true;
	}

	/*
	 * Be careful about the order of this action relative to this function's
	 * other actions.  Generally, this should be after other actions, in case
	 * they have effects PostmasterStateMachine would need to know about.
	 * However, we should do it before the CheckPromoteSignal step, which
	 * cannot have any (immediate) effect on the state machine, but does
	 * depend on what state we're in now.
	 *
	 * 请注意此操作相对于此函数其他操作的顺序。通常，这应该在其他操作之后， 
	 * 以防它们产生 PostmasterStateMachine 需要知道的影响。但是，我们 
	 * 应该在 CheckPromoteSignal 步骤之前执行此操作，因为它对状态机 
	 * 没有任何（立即）影响，但确实取决于我们现在的状态。
	 */
	if (request_state_update)
	{
		PostmasterStateMachine();
	}

	if (StartupPMChild != NULL &&
		(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
		 pmState == PM_HOT_STANDBY) &&
		CheckPromoteSignal())
	{
		/*
		 * Tell startup process to finish recovery.
		 *
		 * Leave the promote signal file in place and let the Startup process
		 * do the unlink.
		 *
		 * 告知启动进程完成恢复。 
		 * 将晋升（promote）信号文件保留在原处，让启动进程执行 unlink。
		 */
		signal_child(StartupPMChild, SIGUSR2);
	}
}

/*
 * Dummy signal handler
 *
 * We use this for signals that we don't actually use in the postmaster,
 * but we do use in backends.  If we were to SIG_IGN such signals in the
 * postmaster, then a newly started backend might drop a signal that arrives
 * before it's able to reconfigure its signal processing.  (See notes in
 * tcop/postgres.c.)
 *
 * 虚拟信号处理器
 * 我们将此用于我们在 Postmaster 中实际上并不使用，但在后端中使用的信号。 
 * 如果我们在 Postmaster 中对这些信号执行 SIG_IGN，那么新启动的后端 
 * 可能会丢失在它能够重新配置其信号处理之前到达的信号。（参见 tcop/postgres.c 中的说明。）
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}

/*
 * Count up number of child processes of specified types.
 * 统计指定类型的子进程数量。
 */
static int
CountChildren(BackendTypeMask targetMask)
{
	dlist_iter	iter;
	int			cnt = 0;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		/*
		 * If we need to distinguish between B_BACKEND and B_WAL_SENDER, check
		 * if any B_BACKEND backends have recently announced that they are
		 * actually WAL senders.
		 *
		 * 如果我们需要区分 B_BACKEND 和 B_WAL_SENDER，请检查是否有任何 B_BACKEND 
		 * 后端最近宣布它们实际上是 WAL 发送进程（walsender）。
		 */
		if (btmask_contains(targetMask, B_WAL_SENDER) != btmask_contains(targetMask, B_BACKEND) &&
			bp->bkend_type == B_BACKEND)
		{
			if (IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = B_WAL_SENDER;
		}

		if (!btmask_contains(targetMask, bp->bkend_type))
			continue;

		ereport(DEBUG4,
				(errmsg_internal("%s process %d is still running",
								 GetBackendTypeDesc(bp->bkend_type), (int) bp->pid)));

		cnt++;
	}
	return cnt;
}


/*
 * StartChildProcess -- start an auxiliary process for the postmaster
 *
 * "type" determines what kind of child will be started.  All child types
 * initially go to AuxiliaryProcessMain, which will handle common setup.
 *
 * Return value of StartChildProcess is subprocess' PMChild entry, or NULL on
 * failure.
 *
 * StartChildProcess —— 为 Postmaster 启动一个辅助进程。
 * "type" 决定了将启动哪种类型的子进程。所有子进程类型最初都进入 
 * AuxiliaryProcessMain，它将处理通用设置。
 * StartChildProcess 的返回值是子进程的 PMChild 条目，如果失败则为 NULL。
 */
static PMChild *
StartChildProcess(BackendType type)
{
	PMChild    *pmchild;
	pid_t		pid;

	pmchild = AssignPostmasterChildSlot(type);
	if (!pmchild)
	{
		if (type == B_AUTOVAC_WORKER)
			ereport(LOG,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("no slot available for new autovacuum worker process")));
		else
		{
			/* shouldn't happen because we allocate enough slots */
			/* 不应该发生，因为我们分配了足够的槽位 */
			elog(LOG, "no postmaster child slot available for aux process");
		}
		return NULL;
	}

	pid = postmaster_child_launch(type, pmchild->child_slot, NULL, 0, NULL);
	if (pid < 0)
	{
		/* in parent, fork failed */
		/* 在父进程中，fork 失败 */
		ReleasePostmasterChildSlot(pmchild);
		ereport(LOG,
				(errmsg("could not fork \"%s\" process: %m", PostmasterChildName(type))));

		/*
		 * fork failure is fatal during startup, auxiliary processes, or COW.
		 *
		 * 在启动、辅助进程或写时复制（COW）期间，fork 失败是致命的。
		 */
		if (type == B_STARTUP)
			ExitPostmaster(1);
		return NULL;
	}

	/* in parent, successful fork */
	/* 在父进程中，fork 成功 */
	pmchild->pid = pid;
	return pmchild;
}

/*
 * StartSysLogger -- start the syslogger process
 * StartSysLogger —— 启动系统日志进程
 */
void
StartSysLogger(void)
{
	Assert(SysLoggerPMChild == NULL);

	SysLoggerPMChild = AssignPostmasterChildSlot(B_LOGGER);
	if (!SysLoggerPMChild)
		elog(PANIC, "no postmaster child slot available for syslogger");
	SysLoggerPMChild->pid = SysLogger_Start(SysLoggerPMChild->child_slot);
	if (SysLoggerPMChild->pid == 0)
	{
		ReleasePostmasterChildSlot(SysLoggerPMChild);
		SysLoggerPMChild = NULL;
	}
}

/*
 * StartAutovacuumWorker
 *		Start an autovac worker process.
 *
 * This function is here because it enters the resulting PID into the
 * postmaster's private backends list.
 *
 * NB -- this code very roughly matches BackendStartup.
 *
 * StartAutovacuumWorker
 * 启动一个自动清理工作进程。
 * 此函数在这里是因为它将生成的 PID 输入到 Postmaster 的专用后端列表中。
 * 注意 —— 此代码非常粗略地与 BackendStartup 匹配。
 */
static void
StartAutovacuumWorker(void)
{
	PMChild    *bn;

	/*
	 * If not in condition to run a process, don't try, but handle it like a
	 * fork failure.  This does not normally happen, since the signal is only
	 * supposed to be sent by autovacuum launcher when it's OK to do it, but
	 * we have to check to avoid race-condition problems during DB state
	 * changes.
	 *
	 * 如果不具备运行进程的条件，请不要尝试，而是像处理 fork 失败一样处理。 
	 * 这通常不会发生，因为信号只有在自动清理启动器认为可以执行时才会发送， 
	 * 但我们必须进行检查，以避免数据库状态更改期间的竞争条件问题。
	 */
	if (canAcceptConnections(B_AUTOVAC_WORKER) == CAC_OK)
	{
		bn = StartChildProcess(B_AUTOVAC_WORKER);
		if (bn)
		{
			bn->bgworker_notify = false;
			bn->rw = NULL;
			return;
		}
		else
		{
			/*
			 * fork failed, fall through to report -- actual error message was
			 * logged by StartChildProcess
			 */
		}
	}

	/*
	 * Report the failure to the launcher, if it's running.  (If it's not, we
	 * might not even be connected to shared memory, so don't try to call
	 * AutoVacWorkerFailed.)  Note that we also need to signal it so that it
	 * responds to the condition, but we don't do that here, instead waiting
	 * for ServerLoop to do it.  This way we avoid a ping-pong signaling in
	 * quick succession between the autovac launcher and postmaster in case
	 * things get ugly.
	 *
	 * 向启动器报告失败（如果它正在运行）。（如果未运行，我们甚至可能没有连接 
	 * 到共享内存，因此不要尝试调用 AutoVacWorkerFailed。）请注意， 
	 * 我们还需要向其发送信号以便其对该情况做出响应，但我们不在这里执行， 
	 * 而是等待 ServerLoop 来执行。这样，我们可以避免在自动清理启动器 
	 * 和 Postmaster 之间发生快速连续的乒乓式信号传递，以防万一。
	 */
	if (AutoVacLauncherPMChild != NULL)
	{
		AutoVacWorkerFailed();
		avlauncher_needs_signal = true;
	}
}


/*
 * Create the opts file
 * 创建 opts 文件
 */
static bool
CreateOptsFile(int argc, char *argv[], char *fullprogname)
{
	FILE	   *fp;
	int			i;

#define OPTS_FILE	"postmaster.opts"

	if ((fp = fopen(OPTS_FILE, "w")) == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " \"%s\"", argv[i]);
	fputs("\n", fp);

	if (fclose(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	return true;
}


/*
 * Start a new bgworker.
 * Starting time conditions must have been checked already.
 *
 * Returns true on success, false on failure.
 * In either case, update the RegisteredBgWorker's state appropriately.
 *
 * NB -- this code very roughly matches BackendStartup.
 *
 * 启动一个新的后台工作进程。启动时间条件必须已经过检查。
 * 成功返回 true，失败返回 false。在任一情况下，相应地更新 RegisteredBgWorker 的状态。
 * 注意 —— 此代码非常粗略地与 BackendStartup 匹配。
 */
static bool
StartBackgroundWorker(RegisteredBgWorker *rw)
{
	PMChild    *bn;
	pid_t		worker_pid;

	Assert(rw->rw_pid == 0);

	/*
	 * Allocate and assign the child slot.  Note we must do this before
	 * forking, so that we can handle failures (out of memory or child-process
	 * slots) cleanly.
	 *
	 * 分配并指派子进程槽位。请注意，我们必须在 fork 之前执行此操作，
	 * 以便我们可以干净地处理失败（内存不足或子进程槽位已满）。
	 *
	 * Treat failure as though the worker had crashed.  That way, the
	 * postmaster will wait a bit before attempting to start it again; if we
	 * tried again right away, most likely we'd find ourselves hitting the
	 * same resource-exhaustion condition.
	 * 
	 * 将失败视为工作进程已崩溃。这样，Postmaster 会在尝试再次启动它之前等待片刻； 
	 * 如果我们立即再次尝试，很可能会发现自己遇到了同样的资源枯竭情况。
	 */
	bn = AssignPostmasterChildSlot(B_BG_WORKER);
	if (bn == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("no slot available for new background worker process")));
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}
	bn->rw = rw;
	bn->bkend_type = B_BG_WORKER;
	bn->bgworker_notify = false;

	ereport(DEBUG1,
			(errmsg_internal("starting background worker process \"%s\"",
							 rw->rw_worker.bgw_name)));

	worker_pid = postmaster_child_launch(B_BG_WORKER, bn->child_slot,
										 &rw->rw_worker, sizeof(BackgroundWorker), NULL);
	if (worker_pid == -1)
	{
		/* in postmaster, fork failed ... */
		/* 在 Postmaster 中，fork 失败 …… */
		ereport(LOG,
				(errmsg("could not fork background worker process: %m")));
		/* undo what AssignPostmasterChildSlot did */
		/* 撤销 AssignPostmasterChildSlot 所做的操作 */
		ReleasePostmasterChildSlot(bn);

		/* mark entry as crashed, so we'll try again later */
		/* 将条目标记为已崩溃，以便我们稍后重试 */
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}

	/* in postmaster, fork successful ... */
	/* 在 Postmaster 中，fork 成功 …… */
	rw->rw_pid = worker_pid;
	bn->pid = rw->rw_pid;
	ReportBackgroundWorkerPID(rw);
	return true;
}

/*
 * Does the current postmaster state require starting a worker with the
 * specified start_time?
 * 
 * 当前 Postmaster 状态是否需要启动具有指定 start_time 的工作进程？
 */
static bool
bgworker_should_start_now(BgWorkerStartTime start_time)
{
	switch (pmState)
	{
		case PM_NO_CHILDREN:
		case PM_WAIT_CHECKPOINTER:
		case PM_WAIT_DEAD_END:
		case PM_WAIT_XLOG_ARCHIVAL:
		case PM_WAIT_XLOG_SHUTDOWN:
		case PM_WAIT_IO_WORKERS:
		case PM_WAIT_BACKENDS:
		case PM_STOP_BACKENDS:
			break;

		case PM_RUN:
			if (start_time == BgWorkerStart_RecoveryFinished)
				return true;
			/* fall through */

		case PM_HOT_STANDBY:
			if (start_time == BgWorkerStart_ConsistentState)
				return true;
			/* fall through */

		case PM_RECOVERY:
		case PM_STARTUP:
		case PM_INIT:
			if (start_time == BgWorkerStart_PostmasterStart)
				return true;
			/* fall through */
	}

	return false;
}

/*
 * If the time is right, start background worker(s).
 *
 * As a side effect, the bgworker control variables are set or reset
 * depending on whether more workers may need to be started.
 *
 * We limit the number of workers started per call, to avoid consuming the
 * postmaster's attention for too long when many such requests are pending.
 * As long as StartWorkerNeeded is true, ServerLoop will not block and will
 * call this function again after dealing with any other issues.
 *
 * 如果时机成熟，启动后台工作进程。
 * 作为副作用，后台工作进程控制变量会根据是否可能需要启动更多工作进程而设置或重置。
 * 我们限制每次调用启动的工作进程数量，以避免在有许多此类请求挂起时过多 
 * 占据 Postmaster 的注意力。只要 StartWorkerNeeded 为 true，ServerLoop 
 * 就不会被阻塞，并将在处理完任何其他问题后再次调用此函数。
 */
static void
maybe_start_bgworkers(void)
{
#define MAX_BGWORKERS_TO_LAUNCH 100
	int			num_launched = 0;
	TimestampTz now = 0;
	dlist_mutable_iter iter;

	/*
	 * During crash recovery, we have no need to be called until the state
	 * transition out of recovery.
	 *
	 * 在崩溃恢复期间，在状态转换出恢复模式之前，我们不需要被调用。
	 */
	if (FatalError)
	{
		StartWorkerNeeded = false;
		HaveCrashedWorker = false;
		return;
	}

	/* Don't need to be called again unless we find a reason for it below */
	/* 除非我们在下面找到原因，否则不需要再次被调用 */
	StartWorkerNeeded = false;
	HaveCrashedWorker = false;

	dlist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		/* ignore if already running */
		/* 如果已经在运行，请忽略 */
		if (rw->rw_pid != 0)
			continue;

		/* if marked for death, clean up and remove from list */
		/* 如果标记为死亡，请清理并从列表中删除 */
		if (rw->rw_terminate)
		{
			ForgetBackgroundWorker(rw);
			continue;
		}

		/*
		 * If this worker has crashed previously, maybe it needs to be
		 * restarted (unless on registration it specified it doesn't want to
		 * be restarted at all).  Check how long ago did a crash last happen.
		 * If the last crash is too recent, don't start it right away; let it
		 * be restarted once enough time has passed.
		 *
		 * 如果此工作进程以前崩溃过，它可能需要重新启动（除非在注册时它指定了 
		 * 根本不想重新启动）。检查上次崩溃发生距今多久。如果上次崩溃时间 
		 * 太近，请不要立即启动它；让它在过去足够长的时间后重新启动。
		 */
		if (rw->rw_crashed_at != 0)
		{
			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
			{
				int			notify_pid;

				notify_pid = rw->rw_worker.bgw_notify_pid;

				ForgetBackgroundWorker(rw);

				/* Report worker is gone now. */
				/* 报告工作进程现在已经消失。 */
				if (notify_pid != 0)
					kill(notify_pid, SIGUSR1);

				continue;
			}

			/* read system time only when needed */
			/* 仅在需要时读取系统时间 */
			if (now == 0)
				now = GetCurrentTimestamp();

			if (!TimestampDifferenceExceeds(rw->rw_crashed_at, now,
											rw->rw_worker.bgw_restart_time * 1000))
			{
				/* Set flag to remember that we have workers to start later */
				/* 设置标志以记住我们稍后有要启动的工作进程 */
				HaveCrashedWorker = true;
				continue;
			}
		}

		if (bgworker_should_start_now(rw->rw_worker.bgw_start_time))
		{
			/* reset crash time before trying to start worker */
			/* 在尝试启动工作进程之前重置崩溃时间 */
			rw->rw_crashed_at = 0;

			/*
			 * Try to start the worker.
			 *
			 * On failure, give up processing workers for now, but set
			 * StartWorkerNeeded so we'll come back here on the next iteration
			 * of ServerLoop to try again.  (We don't want to wait, because
			 * there might be additional ready-to-run workers.)  We could set
			 * HaveCrashedWorker as well, since this worker is now marked
			 * crashed, but there's no need because the next run of this
			 * function will do that.
			 *
			 * 尝试启动工作进程。
			 * 如果失败，暂时放弃处理工作进程，但设置 StartWorkerNeeded， 
			 * 以便我们在 ServerLoop 的下一次迭代中回到这里重试。 
			 * （我们不想等待，因为可能还有其他准备运行的工作进程。） 
			 * 我们也可以设置 HaveCrashedWorker，因为此工作进程现在已被 
			 * 标记为崩溃，但没有必要，因为此函数的下一次运行会执行此操作。
			 */
			if (!StartBackgroundWorker(rw))
			{
				StartWorkerNeeded = true;
				return;
			}

			/*
			 * If we've launched as many workers as allowed, quit, but have
			 * ServerLoop call us again to look for additional ready-to-run
			 * workers.  There might not be any, but we'll find out the next
			 * time we run.
			 *
			 * 如果我们已经启动了尽可能多的工作进程，请退出，但让 ServerLoop 
			 * 再次调用我们，以查找其他准备运行的工作进程。可能没有任何进程， 
			 * 但我们下次运行时会发现。
			 */
			if (++num_launched >= MAX_BGWORKERS_TO_LAUNCH)
			{
				StartWorkerNeeded = true;
				return;
			}
		}
	}
}

static bool
maybe_reap_io_worker(int pid)
{
	for (int i = 0; i < MAX_IO_WORKERS; ++i)
	{
		if (io_worker_children[i] &&
			io_worker_children[i]->pid == pid)
		{
			ReleasePostmasterChildSlot(io_worker_children[i]);

			--io_worker_count;
			io_worker_children[i] = NULL;
			return true;
		}
	}
	return false;
}

/*
 * Start or stop IO workers, to close the gap between the number of running
 * workers and the number of configured workers.  Used to respond to change of
 * the io_workers GUC (by increasing and decreasing the number of workers), as
 * well as workers terminating in response to errors (by starting
 * "replacement" workers).
 *
 * 启动或停止 IO 工作进程，以缩小运行中的工作进程数量与配置的工作进程数量之间的差距。
 * 用于响应 io_workers GUC 的更改（通过增加或减少工作进程数量），
 * 以及由于错误终止的工作进程（通过启动“替换”工作进程）。
 */
static void
maybe_adjust_io_workers(void)
{
	if (!pgaio_workers_enabled())
		return;

	/*
	 * If we're in final shutting down state, then we're just waiting for all
	 * processes to exit.
	 *
	 * 如果我们处于最终关闭状态，那么我们只是在等待所有进程退出。
	 */
	if (pmState >= PM_WAIT_IO_WORKERS)
		return;

	/* Don't start new workers during an immediate shutdown either. */
	/* 在立即关闭期间也不要启动新的工作进程。 */
	if (Shutdown >= ImmediateShutdown)
		return;

	/*
	 * Don't start new workers if we're in the shutdown phase of a crash
	 * restart. But we *do* need to start if we're already starting up again.
	 *
	 * 如果我们处于崩溃重启的关闭阶段，不要启动新的工作进程。
	 * 但如果我们已经再次启动中，我们*确实*需要启动。
	 */
	if (FatalError && pmState >= PM_STOP_BACKENDS)
		return;

	Assert(pmState < PM_WAIT_IO_WORKERS);

	/* Not enough running? */
	/* 运行中的不够？ */
	while (io_worker_count < io_workers)
	{
		PMChild    *child;
		int			i;

		/* find unused entry in io_worker_children array */
		/* 在 io_worker_children 数组中寻找未使用的条目 */
		for (i = 0; i < MAX_IO_WORKERS; ++i)
		{
			if (io_worker_children[i] == NULL)
				break;
		}
		if (i == MAX_IO_WORKERS)
			elog(ERROR, "could not find a free IO worker slot");

		/* Try to launch one. */
		/* 尝试启动一个。 */
		child = StartChildProcess(B_IO_WORKER);
		if (child != NULL)
		{
			io_worker_children[i] = child;
			++io_worker_count;
		}
		else
			break;				/* try again next time */
								/* 下次再试 */
	}

	/* Too many running? */
	/* 运行中的太多？ */
	if (io_worker_count > io_workers)
	{
		/* ask the IO worker in the highest slot to exit */
		/* 要求最高槽位的 IO 工作进程退出 */
		for (int i = MAX_IO_WORKERS - 1; i >= 0; --i)
		{
			if (io_worker_children[i] != NULL)
			{
				kill(io_worker_children[i]->pid, SIGUSR2);
				break;
			}
		}
	}
}


/*
 * When a backend asks to be notified about worker state changes, we
 * set a flag in its backend entry.  The background worker machinery needs
 * to know when such backends exit.
 *
 * 当一个后端请求在工作进程状态更改时获得通知，我们会在其后端条目中设置一个标志。
 * 后台工作进程机制需要知道此类后端何时退出。
 */
bool
PostmasterMarkPIDForWorkerNotify(int pid)
{
	dlist_iter	iter;
	PMChild    *bp;

	dlist_foreach(iter, &ActiveChildList)
	{
		bp = dlist_container(PMChild, elem, iter.cur);
		if (bp->pid == pid)
		{
			bp->bgworker_notify = true;
			return true;
		}
	}
	return false;
}

#ifdef WIN32

/*
 * Subset implementation of waitpid() for Windows.  We assume pid is -1
 * (that is, check all child processes) and options is WNOHANG (don't wait).
 *
 * 针对 Windows 的 waitpid() 子集实现。我们假设 pid 为 -1
 *（即检查所有子进程），选项为 WNOHANG（不等待）。
 */
static pid_t
waitpid(pid_t pid, int *exitstatus, int options)
{
	win32_deadchild_waitinfo *childinfo;
	DWORD		exitcode;
	DWORD		dwd;
	ULONG_PTR	key;
	OVERLAPPED *ovl;

	/* Try to consume one win32_deadchild_waitinfo from the queue. */
	/* 尝试从队列中消耗一个 win32_deadchild_waitinfo。 */
	if (!GetQueuedCompletionStatus(win32ChildQueue, &dwd, &key, &ovl, 0))
	{
		errno = EAGAIN;
		return -1;
	}

	childinfo = (win32_deadchild_waitinfo *) key;
	pid = childinfo->procId;

	/*
	 * Remove handle from wait - required even though it's set to wait only
	 * once
	 *
	 * 从等待中移除句柄——即使它被设置为仅等待一次，这也是必需的
	 */
	UnregisterWaitEx(childinfo->waitHandle, NULL);

	if (!GetExitCodeProcess(childinfo->procHandle, &exitcode))
	{
		/*
		 * Should never happen. Inform user and set a fixed exitcode.
		 *
		 * 永远不应该发生。通知用户并设置一个固定的退出码。
		 */
		write_stderr("could not read exit code for process\n");
		exitcode = 255;
	}
	*exitstatus = exitcode;

	/*
	 * Close the process handle.  Only after this point can the PID can be
	 * recycled by the kernel.
	 *
	 * 关闭进程句柄。只有在此之后，PID 才能被内核回收。
	 */
	CloseHandle(childinfo->procHandle);

	/*
	 * Free struct that was allocated before the call to
	 * RegisterWaitForSingleObject()
	 *
	 * 释放调用 RegisterWaitForSingleObject() 前分配的结构体
	 */
	pfree(childinfo);

	return pid;
}

/*
 * Note! Code below executes on a thread pool! All operations must
 * be thread safe! Note that elog() and friends must *not* be used.
 *
 * 注意！下面的代码在线程池上执行！所有操作必须是线程安全的！
 * 注意，*不能*使用 elog() 及相关函数。
 */
static void WINAPI
pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	/* Should never happen, since we use INFINITE as timeout value. */
	/* 既然我们使用 INFINITE 作为超时值，这永远不应该发生。 */
	if (TimerOrWaitFired)
		return;

	/*
	 * Post the win32_deadchild_waitinfo object for waitpid() to deal with. If
	 * that fails, we leak the object, but we also leak a whole process and
	 * get into an unrecoverable state, so there's not much point in worrying
	 * about that.  We'd like to panic, but we can't use that infrastructure
	 * from this thread.
	 *
	 * 提交 win32_deadchild_waitinfo 对象供 waitpid() 处理。如果失败，
	 * 我们会泄漏该对象，但我们也会泄漏整个进程并进入不可恢复的状态，
	 * 所以担心这一点意义不大。我们想调用 panic，但在该线程中无法使用该基础设施。
	 */
	if (!PostQueuedCompletionStatus(win32ChildQueue,
									0,
									(ULONG_PTR) lpParameter,
									NULL))
		write_stderr("could not post child completion status\n");

	/* Queue SIGCHLD signal. */
	/* 将 SIGCHLD 信号入队。 */
	pg_queue_signal(SIGCHLD);
}

/*
 * Queue a waiter to signal when this child dies.  The wait will be handled
 * automatically by an operating system thread pool.  The memory and the
 * process handle will be freed by a later call to waitpid().
 *
 * 当此子进程死亡时，排队一个等待者以发出信号。等待将由操作系统线程池自动处理。
 * 内存和进程句柄将由稍后的 waitpid() 调用释放。
 */
void
pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId)
{
	win32_deadchild_waitinfo *childinfo;

	childinfo = palloc(sizeof(win32_deadchild_waitinfo));
	childinfo->procHandle = procHandle;
	childinfo->procId = procId;

	if (!RegisterWaitForSingleObject(&childinfo->waitHandle,
									 procHandle,
									 pgwin32_deadchild_callback,
									 childinfo,
									 INFINITE,
									 WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD))
		ereport(FATAL,
				(errmsg_internal("could not register process for wait: error code %lu",
								 GetLastError())));
}

#endif							/* WIN32 */

/*
 * Initialize one and only handle for monitoring postmaster death.
 *
 * Called once in the postmaster, so that child processes can subsequently
 * monitor if their parent is dead.
 *
 * 初始化唯一的一个用于监控 postmaster 死亡的句柄。
 * 在 postmaster 中调用一次，以便子进程随后可以监控其父进程是否已死。
 */
static void
InitPostmasterDeathWatchHandle(void)
{
#ifndef WIN32

	/*
	 * Create a pipe. Postmaster holds the write end of the pipe open
	 * (POSTMASTER_FD_OWN), and children hold the read end. Children can pass
	 * the read file descriptor to select() to wake up in case postmaster
	 * dies, or check for postmaster death with a (read() == 0). Children must
	 * close the write end as soon as possible after forking, because EOF
	 * won't be signaled in the read end until all processes have closed the
	 * write fd. That is taken care of in ClosePostmasterPorts().
	 *
	 * 创建一个管道。Postmaster 保持管道的写入端打开（POSTMASTER_FD_OWN），
	 * 子进程持有读取端。子进程可以将读取文件描述符传递给 select()，
	 * 以在 postmaster 死亡时唤醒，或者通过 (read() == 0) 检查 postmaster 通讯死亡。
	 * 子进程必须在 fork 后尽快关闭写入端，因为直到所有进程都关闭了写入文件描述符，
	 * 读取端才会收到 EOF 信号。这在 ClosePostmasterPorts() 中处理。
	 */
	Assert(MyProcPid == PostmasterPid);
	if (pipe(postmaster_alive_fds) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not create pipe to monitor postmaster death: %m")));

	/* Notify fd.c that we've eaten two FDs for the pipe. */
	/* 通知 fd.c 我们已经为管道消耗了两个文件描述符。 */
	ReserveExternalFD();
	ReserveExternalFD();

	/*
	 * Set O_NONBLOCK to allow testing for the fd's presence with a read()
	 * call.
	 *
	 * 设置 O_NONBLOCK 以允许通过 read() 调用测试文件描述符的存在。
	 */
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFL, O_NONBLOCK) == -1)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to nonblocking mode: %m")));
#else

	/*
	 * On Windows, we use a process handle for the same purpose.
	 *
	 * 在 Windows 上，我们出于同样的目的使用进程句柄。
	 */
	if (DuplicateHandle(GetCurrentProcess(),
						GetCurrentProcess(),
						GetCurrentProcess(),
						&PostmasterHandle,
						0,
						TRUE,
						DUPLICATE_SAME_ACCESS) == 0)
		ereport(FATAL,
				(errmsg_internal("could not duplicate postmaster handle: error code %lu",
								 GetLastError())));
#endif							/* WIN32 */
}
