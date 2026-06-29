/*-------------------------------------------------------------------------
 *
 * pgarch.c
 *
 *	PostgreSQL WAL archiver
 *	PostgreSQL WAL 归档器
 *
 *	All functions relating to archiver are included here
 *	所有与归档器相关的函数都包含在这里
 *
 *	- All functions executed by archiver process
 *	- 由归档进程执行的所有函数
 *
 *	- archiver is forked from postmaster, and the two
 *	processes then communicate using signals. All functions
 *	executed by postmaster are included in this file.
 *	- 归档器由 postmaster 进程派生（fork），然后这两个进程使用信号进行通信。
 *	所有由 postmaster 执行的函数也都包含在此文件中。
 *
 *	Initial author: Simon Riggs		simon@2ndquadrant.com
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pgarch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "archive/archive_module.h"
#include "archive/shell_archive.h"
#include "lib/binaryheap.h"
#include "libpq/pqsignal.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/interrupt.h"
#include "postmaster/pgarch.h"
#include "storage/aio_subsys.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timeout.h"

/* ----------
 * Timer definitions.
 * 定时器定义。
 * ----------
 */
#define PGARCH_AUTOWAKE_INTERVAL                                               \
  60 /* How often to force a poll of the                                       \
      * archive status directory; in seconds.                                  \
      * 强制轮询归档状态目录的频率；以秒为单位。 */

#define PGARCH_RESTART_INTERVAL                                                \
  10 /* How often to attempt to restart a                                      \
      * failed archiver; in seconds.                                           \
      * 尝试重启失败的归档器的频率；以秒为单位。 */

/*
 * Maximum number of retries allowed when attempting to archive a WAL
 * file.
 * 尝试归档 WAL 文件时允许的最大重试次数。
 */
#define NUM_ARCHIVE_RETRIES 3

/*
 * Maximum number of retries allowed when attempting to remove an
 * orphan archive status file.
 * 尝试删除孤儿归档状态文件时允许的最大重试次数。
 */
#define NUM_ORPHAN_CLEANUP_RETRIES 3

/*
 * Maximum number of .ready files to gather per directory scan.
 * 每次目录扫描收集的 .ready 文件的最大数量。
 */
#define NUM_FILES_PER_DIRECTORY_SCAN 64

/* Shared memory area for archiver process
 * 归档进程的共享内存区域 */
typedef struct PgArchData {
  int pgprocno; /* proc number of archiver process */
                /* 归档进程的进程号 */

  /*
   * Forces a directory scan in pgarch_readyXlog().
   * 在 pgarch_readyXlog() 中强制进行目录扫描。
   */
  pg_atomic_uint32 force_dir_scan;
} PgArchData;

char *XLogArchiveLibrary = "";
char *arch_module_check_errdetail_string;

/* ----------
 * Local data
 * 本地数据
 * ----------
 */
static time_t last_sigterm_time = 0;
static PgArchData *PgArch = NULL;
static const ArchiveModuleCallbacks *ArchiveCallbacks;
static ArchiveModuleState *archive_module_state;
static MemoryContext archive_context;

/*
 * Stuff for tracking multiple files to archive from each scan of
 * archive_status.  Minimizing the number of directory scans when there are
 * many files to archive can significantly improve archival rate.
 *
 * 用于跟踪每次扫描 archive_status
 * 时要归档的多个文件的信息。在有许多文件需要归档时，
 * 尽量减少目录扫描次数可以显著提高归档速率。
 *
 * arch_heap is a max-heap that is used during the directory scan to track
 * the highest-priority files to archive.  After the directory scan
 * completes, the file names are stored in ascending order of priority in
 * arch_files.  pgarch_readyXlog() returns files from arch_files until it
 * is empty, at which point another directory scan must be performed.
 * arch_heap 是一个最大堆，用于在目录扫描期间跟踪优先级最高的归档文件。
 * 目录扫描完成后，文件名按优先级升序存储在 arch_files 中。
 * pgarch_readyXlog() 从 arch_files
 * 返回文件直到它变为空，届时必须执行另一次目录扫描。
 *
 * We only need this data in the archiver process, so make it a palloc'd
 * struct rather than a bunch of static arrays.
 * 我们只需要在归档进程中使用这些数据，因此将其设为 palloc
 * 分配的结构体，而不是一堆静态数组。
 */
struct arch_files_state {
  binaryheap *arch_heap;
  int arch_files_size; /* number of live entries in arch_files[] */
                       /* arch_files[] 中处于活动状态的条目数 */
  char *arch_files[NUM_FILES_PER_DIRECTORY_SCAN];
  /* buffers underlying heap, and later arch_files[], entries:
   * 缓冲区，用于底层堆以及稍后的 arch_files[] 条目： */
  char arch_filenames[NUM_FILES_PER_DIRECTORY_SCAN][MAX_XFN_CHARS + 1];
};

static struct arch_files_state *arch_files = NULL;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 * 由中断处理程序设置的标志，用于稍后在主循环中提供服务。
 */
static volatile sig_atomic_t ready_to_stop = false;

/* ----------
 * Local function forward declarations
 * 本地函数前向声明
 * ----------
 */
static void pgarch_waken_stop(SIGNAL_ARGS);
static void pgarch_MainLoop(void);
static void pgarch_ArchiverCopyLoop(void);
static bool pgarch_archiveXlog(char *xlog);
static bool pgarch_readyXlog(char *xlog);
static void pgarch_archiveDone(char *xlog);
static void pgarch_die(int code, Datum arg);
static void ProcessPgArchInterrupts(void);
static int ready_file_comparator(Datum a, Datum b, void *arg);
static void LoadArchiveLibrary(void);
static void pgarch_call_module_shutdown_cb(int code, Datum arg);

/* Report shared memory space needed by PgArchShmemInit
 * 报告 PgArchShmemInit 所需的共享内存空间
 *
 * Function purpose: Report shmem size needed by PgArch.
 * 函数作用：报告 PgArch 所需的共享内存空间大小。
 */
Size PgArchShmemSize(void) {
  Size size = 0;

  size = add_size(size, sizeof(PgArchData));

  return size;
}

/* Allocate and initialize archiver-related shared memory
 * 分配并初始化与归档器相关的共享内存
 *
 * Function purpose: Allocate and initialize PgArchData shmem struct.
 * 函数作用：在共享内存中分配并初始化 PgArchData 结构。
 */
void PgArchShmemInit(void) {
  bool found;

  PgArch =
      (PgArchData *)ShmemInitStruct("Archiver Data", PgArchShmemSize(), &found);

  if (!found) {
    /* First time through, so initialize
     * 第一次执行，进行初始化 */
    MemSet(PgArch, 0, PgArchShmemSize());
    PgArch->pgprocno = INVALID_PROC_NUMBER;
    pg_atomic_init_u32(&PgArch->force_dir_scan, 0);
  }
}

/*
 * PgArchCanRestart
 *
 * Return true and archiver is allowed to restart if enough time has
 * passed since it was launched last to reach PGARCH_RESTART_INTERVAL.
 * Otherwise return false.
 * 如果自上次启动以来已经过了足够的时间（达到 PGARCH_RESTART_INTERVAL），则返回
 * true 允许归档器重启。 否则返回 false。
 *
 * This is a safety valve to protect against continuous respawn attempts if the
 * archiver is dying immediately at launch. Note that since we will retry to
 * launch the archiver from the postmaster main loop, we will get another
 * chance later.
 * 这是一个安全阀，用于防止归档器在启动后立即死亡时连续尝试重启。
 * 请注意，由于我们将尝试从 postmaster
 * 主循环重新启动归档器，因此稍后我们将获得另一次机会。
 *
 * Function purpose: Rate-limit archiver process restart attempts.
 * 函数作用：控制归档进程重启的时间间隔，防止因异常频繁拉起而导致死循环。
 */
bool PgArchCanRestart(void) {
  static time_t last_pgarch_start_time = 0;
  time_t curtime = time(NULL);

  /*
   * Return false and don't restart archiver if too soon since last archiver
   * start.
   * 如果离上次归档器启动时间太短，则返回 false 且不要重启归档器。
   */
  if ((unsigned int)(curtime - last_pgarch_start_time) <
      (unsigned int)PGARCH_RESTART_INTERVAL)
    return false;

  last_pgarch_start_time = curtime;
  return true;
}

/* Main entry point for archiver process
 * 归档进程的主入口点
 *
 * Function purpose: Main entry point of the WAL Archiver process.
 * 函数作用：WAL 归档进程的入口主程序。
 *
 * Core workflow:
 * 核心流程：
 * 1. Initialize process type B_ARCHIVER and call AuxiliaryProcessMainCommon().
 *    初始化进程类型为 B_ARCHIVER 并执行通用的辅助进程环境初始化。
 * 2. Setup signals (SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, SIGQUIT, etc.).
 *    绑定对应的信号处理函数并解除信号屏蔽。
 * 3. Setup shmem exiting callbacks and initialize PGPROC metadata.
 *    注册退出回调并向共享内存登记进程编号。
 * 4. Load the library/callbacks for archiving, and allocate max-heap workspace.
 *    加载配置的归档库（如无则用 shell），并初始化用于处理文件的最大堆工作区。
 * 5. Run pgarch_MainLoop() to periodically search and archive WAL.
 *    进入主循环函数 pgarch_MainLoop() 开始执行实际归档。
 */
void PgArchiverMain(const void *startup_data, size_t startup_data_len) {
  Assert(startup_data_len == 0);

  MyBackendType = B_ARCHIVER;
  AuxiliaryProcessMainCommon();

  /*
   * Ignore all signals usually bound to some action in the postmaster,
   * except for SIGHUP, SIGTERM, SIGUSR1, SIGUSR2, and SIGQUIT.
   * 忽略通常绑定 to postmaster 中某些操作的所有信号，
   * 但 SIGHUP、SIGTERM、SIGUSR1、SIGUSR2 和 SIGQUIT 除外。
   */
  pqsignal(SIGHUP, SignalHandlerForConfigReload);
  pqsignal(SIGINT, SIG_IGN);
  pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
  /* SIGQUIT handler was already set up by InitPostmasterChild
   * SIGQUIT 处理程序已由 InitPostmasterChild 设置 */
  pqsignal(SIGALRM, SIG_IGN);
  pqsignal(SIGPIPE, SIG_IGN);
  pqsignal(SIGUSR1, procsignal_sigusr1_handler);
  pqsignal(SIGUSR2, pgarch_waken_stop);

  /* Reset some signals that are accepted by postmaster but not here
   * 重置一些由 postmaster 接受但在此处不接受的信号 */
  pqsignal(SIGCHLD, SIG_DFL);

  /* Unblock signals (they were blocked when the postmaster forked us)
   * 取消阻止信号（它们在 postmaster fork 我们时被阻止了） */
  sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

  /* We shouldn't be launched unnecessarily.
   * 我们不应该被不必要地启动。 */
  Assert(XLogArchivingActive());

  /* Arrange to clean up at archiver exit
   * 安排在归档器退出时进行清理 */
  on_shmem_exit(pgarch_die, 0);

  /*
   * Advertise our proc number so that backends can use our latch to wake us
   * up while we're sleeping.
   * 公布我们的进程编号，以便后端可以在我们睡眠时使用我们的 latch 来唤醒我们。
   */
  PgArch->pgprocno = MyProcNumber;

  /* Create workspace for pgarch_readyXlog()
   * 为 pgarch_readyXlog() 创建工作空间 */
  arch_files = palloc(sizeof(struct arch_files_state));
  arch_files->arch_files_size = 0;

  /* Initialize our max-heap for prioritizing files to archive.
   * 初始化用于确定归档文件优先级的最大堆。 */
  arch_files->arch_heap = binaryheap_allocate(NUM_FILES_PER_DIRECTORY_SCAN,
                                              ready_file_comparator, NULL);

  /* Initialize our memory context.
   * 初始化我们的内存上下文。 */
  archive_context = AllocSetContextCreate(TopMemoryContext, "archiver",
                                          ALLOCSET_DEFAULT_SIZES);

  /* Load the archive_library.
   * 加载 archive_library。 */
  LoadArchiveLibrary();

  pgarch_MainLoop();

  proc_exit(0);
}

/*
 * Wake up the archiver
 * 唤醒归档器
 *
 * Function purpose: Waken the archiver by setting its process latch.
 * 函数作用：通过触发归档进程的锁存器（Latch）将其唤醒。
 */
void PgArchWakeup(void) {
  int arch_pgprocno = PgArch->pgprocno;

  /*
   * We don't acquire ProcArrayLock here.  It's actually fine because
   * procLatch isn't ever freed, so we just can potentially set the wrong
   * process' (or no process') latch.  Even in that case the archiver will
   * be relaunched shortly and will start archiving.
   * 我们在这里不获取 ProcArrayLock。这实际上没问题，因为 procLatch
   * 永远不会被释放， 所以我们可能只是设置了错误的进程（or 没有进程）的 latch。
   * 即便如此，归档器很快就会重新启动并开始归档。
   */
  if (arch_pgprocno != INVALID_PROC_NUMBER)
    SetLatch(&ProcGlobal->allProcs[arch_pgprocno].procLatch);
}

/* SIGUSR2 signal handler for archiver process
 * 归档进程的 SIGUSR2 信号处理程序
 *
 * Function purpose: Handle SIGUSR2 to signal archiver to perform a final cycle and stop.
 * 函数作用：处理 SIGUSR2 信号，用于通知归档进程完成最后一轮工作后退出。
 */
static void pgarch_waken_stop(SIGNAL_ARGS) {
  /* set flag to do a final cycle and shut down afterwards
   * 设置标志以执行最后一个周期并在此后关闭 */
  ready_to_stop = true;
  SetLatch(MyLatch);
}

/*
 * pgarch_MainLoop
 *
 * Main loop for archiver
 * 归档器的主循环
 *
 * Function purpose: Maintain the run cycle for WAL Archiver.
 * 函数作用：执行 WAL 归档进程的主业务循环。
 */
static void pgarch_MainLoop(void) {
  bool time_to_stop;

  /*
   * There shouldn't be anything for the archiver to do except to wait for a
   * signal ... however, the archiver exists to protect our data, so it
   * wakes up occasionally to allow itself to be proactive.
   * 归档器除了等待信号之外不应该有任何事情要做……然而，归档器的存在是为了保护我们的数据，
   * 所以它偶尔会醒来以允许自己保持主动。
   */
  do {
    ResetLatch(MyLatch);

    /* When we get SIGUSR2, we do one more archive cycle, then exit
     * 当我们收到 SIGUSR2 时，我们再执行一个归档周期，然后退出 */
    time_to_stop = ready_to_stop;

    /* Check for barrier events and config update
     * 检查屏障事件和配置更新 */
    ProcessPgArchInterrupts();

    /*
     * If we've gotten SIGTERM, we normally just sit and do nothing until
     * SIGUSR2 arrives.  However, that means a random SIGTERM would
     * disable archiving indefinitely, which doesn't seem like a good
     * idea.  If more than 60 seconds pass since SIGTERM, exit anyway, so
     * that the postmaster can start a new archiver if needed.
     * 如果我们收到了 SIGTERM，我们通常只是坐着什么都不做，直到 SIGUSR2 到来。
     * 然而，这意味着随机的 SIGTERM 会无限期地禁用归档，这似乎不是一个好主意。
     * 如果自 SIGTERM 以来过去了超过 60 秒，无论如何都要退出，以便 postmaster
     * 可以在需要时启动新的归档器。
     */
    if (ShutdownRequestPending) {
      time_t curtime = time(NULL);

      if (last_sigterm_time == 0)
        last_sigterm_time = curtime;
      else if ((unsigned int)(curtime - last_sigterm_time) >= (unsigned int)60)
        break;
    }

    /* Do what we're here for
     * 做我们该做的事 */
    pgarch_ArchiverCopyLoop();

    /*
     * Sleep until a signal is received, or until a poll is forced by
     * PGARCH_AUTOWAKE_INTERVAL, or until postmaster dies.
     * 睡眠直到收到信号，或者直到由 PGARCH_AUTOWAKE_INTERVAL 强制轮询，或者直到
     * postmaster 死亡。
     */
    if (!time_to_stop) /* Don't wait during last iteration
                        * 在最后一次迭代期间不要等待 */
    {
      int rc;

      rc =
          WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                    PGARCH_AUTOWAKE_INTERVAL * 1000L, WAIT_EVENT_ARCHIVER_MAIN);
      if (rc & WL_POSTMASTER_DEATH)
        time_to_stop = true;
    }

    /*
     * The archiver quits either when the postmaster dies (not expected)
     * or after completing one more archiving cycle after receiving
     * SIGUSR2.
     * 归档器在以下情况下退出：postmaster 死亡（非预期情况），
     * 或者在收到 SIGUSR2 后完成最后一个归档周期。
     */
  } while (!time_to_stop);
}

/*
 * pgarch_ArchiverCopyLoop
 *
 * Archives all outstanding xlogs then returns
 * 归档所有待处理的 xlog 然后返回
 *
 * Function purpose: Loop through available WAL files and call archive functions.
 * 函数作用：遍历检测待处理的准备好的 WAL 物理文件，并不断复制直到全部归档完成。
 *
 * Core workflow:
 * 核心流程：
 * 1. Call pgarch_readyXlog() to find the oldest unarchived WAL.
 *    调用 pgarch_readyXlog() 寻找最旧且待归档的 WAL 物理文件名。
 * 2. Check for shutdown signal and check orphan files.
 *    检查关闭状态，以及是否存在因系统崩溃导致的孤儿状态文件，如是则 unlink() 清理。
 * 3. Call pgarch_archiveXlog() to execute callback.
 *    调用 pgarch_archiveXlog() 进行具体归档动作。
 * 4. Call pgarch_archiveDone() to rename status from .ready to .done, and report stats.
 *    归档成功后调用 pgarch_archiveDone() 将对应的 .ready 状态重命名为 .done 并上报统计。
 */
static void pgarch_ArchiverCopyLoop(void) {
  char xlog[MAX_XFN_CHARS + 1];

  /* force directory scan in the first call to pgarch_readyXlog()
   * 在第一次调用 pgarch_readyXlog() 时强制进行目录扫描 */
  arch_files->arch_files_size = 0;

  /*
   * loop through all xlogs with archive_status of .ready and archive
   * them...mostly we expect this to be a single file, though it is possible
   * some backend will add files onto the list of those that need archiving
   * while we are still copying earlier archives
   * 循环遍历所有 archive_status 为 .ready 的 xlog 并归档它们……
   * 我们通常期望只有一个文件，尽管在正在复制较早的归档文件时，
   * 某些后端可能会将文件添加到需要归档的列表中。
   */
  while (pgarch_readyXlog(xlog)) {
    int failures = 0;
    int failures_orphan = 0;

    for (;;) {
      struct stat stat_buf;
      char pathname[MAXPGPATH];

      /*
       * Do not initiate any more archive commands after receiving
       * SIGTERM, nor after the postmaster has died unexpectedly. The
       * first condition is to try to keep from having init SIGKILL the
       * command, and the second is to avoid conflicts with another
       * archiver spawned by a newer postmaster.
       * 在收到 SIGTERM 后，以及在 postmaster
       * 意外死亡后，不要再启动任何归档命令。 第一个条件是试图防止 init
       * 进程对命令执行 SIGKILL， 第二个条件是避免与由新的 postmaster
       * 生成的另一个归档器产生冲突。
       */
      if (ShutdownRequestPending || !PostmasterIsAlive())
        return;

      /*
       * Check for barrier events and config update.  This is so that
       * we'll adopt a new setting for archive_command as soon as
       * possible, even if there is a backlog of files to be archived.
       * 检查屏障事件和配置更新。这样做是为了让我们尽快采用 archive_command
       * 的新设置， 即使有大量积压文件等待归档。
       */
      ProcessPgArchInterrupts();

      /* Reset variables that might be set by the callback
       * 重置可能由回调函数设置的变量 */
      arch_module_check_errdetail_string = NULL;

      /* can't do anything if not configured ...
       * 如果未配置，则无法执行任何操作…… */
      if (ArchiveCallbacks->check_configured_cb != NULL &&
          !ArchiveCallbacks->check_configured_cb(archive_module_state)) {
        ereport(
            WARNING,
            (errmsg(
                 "\"archive_mode\" enabled, yet archiving is not configured"),
             arch_module_check_errdetail_string
                 ? errdetail_internal("%s", arch_module_check_errdetail_string)
                 : 0));
        return;
      }

      /*
       * Since archive status files are not removed in a durable manner,
       * a system crash could leave behind .ready files for WAL segments
       * that have already been recycled or removed.  In this case,
       * simply remove the orphan status file and move on.  unlink() is
       * used here as even on subsequent crashes the same orphan files
       * would get removed, so there is no need to worry about
       * durability.
       * 由于归档状态文件不是以持久方式删除的，系统崩溃可能会留下已回收或已删除的
       * WAL 段的 .ready 文件。 在这种情况下，只需删除孤儿状态文件并继续。
       * 此处使用
       * unlink()，因为即使在随后的崩溃中，相同的孤儿文件也会被删除，因此无需担心持久性。
       */
      snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);
      if (stat(pathname, &stat_buf) != 0 && errno == ENOENT) {
        char xlogready[MAXPGPATH];

        StatusFilePath(xlogready, xlog, ".ready");
        if (unlink(xlogready) == 0) {
          ereport(WARNING, (errmsg("removed orphan archive status file \"%s\"",
                                   xlogready)));

          /* leave loop and move to the next status file
           * 离开循环并移动 to下一个状态文件 */
          break;
        }

        if (++failures_orphan >= NUM_ORPHAN_CLEANUP_RETRIES) {
          ereport(WARNING,
                  (errmsg("removal of orphan archive status file \"%s\" failed "
                          "too many times, will try again later",
                          xlogready)));

          /* give up cleanup of orphan status files
           * 放弃清理孤儿状态文件 */
          return;
        }

        /* wait a bit before retrying
         * 重试前等待一会儿 */
        pg_usleep(1000000L);
        continue;
      }

      if (pgarch_archiveXlog(xlog)) {
        /* successful
         * 成功 */
        pgarch_archiveDone(xlog);

        /*
         * Tell the cumulative stats system about the WAL file that we
         * successfully archived
         * 告诉累积统计系统我们已成功归档的 WAL 文件
         */
        pgstat_report_archiver(xlog, false);

        break; /* out of inner retry loop
                * 跳出内部重试循环 */
      } else {
        /*
         * Tell the cumulative stats system about the WAL file that we
         * failed to archive
         * 告诉累积统计系统归档失败的 WAL 文件
         */
        pgstat_report_archiver(xlog, true);

        if (++failures >= NUM_ARCHIVE_RETRIES) {
          ereport(WARNING,
                  (errmsg("archiving write-ahead log file \"%s\" failed too "
                          "many times, will try again later",
                          xlog)));
          return; /* give up archiving for now
                   * 暂时放弃归档 */
        }
        pg_usleep(1000000L); /* wait a bit before retrying
                              * 重试前等待一会儿 */
      }
    }
  }
}

/*
 * pgarch_archiveXlog
 *
 * Invokes archive_file_cb to copy one archive file to wherever it should go
 * 调用 archive_file_cb 将一个归档文件复制到它应该去的地方
 *
 * Returns true if successful
 * 如果成功则返回 true
 *
 * Function purpose: Call module callback to archive a single WAL file.
 * 函数作用：调用具体归档加载库的回调动作函数执行单文件归档复制。
 */
static bool pgarch_archiveXlog(char *xlog) {
  sigjmp_buf local_sigjmp_buf;
  MemoryContext oldcontext;
  char pathname[MAXPGPATH];
  char activitymsg[MAXFNAMELEN + 16];
  bool ret;

  snprintf(pathname, MAXPGPATH, XLOGDIR "/%s", xlog);

  /* Report archive activity in PS display
   * 在 PS 显示中报告归档活动 */
  snprintf(activitymsg, sizeof(activitymsg), "archiving %s", xlog);
  set_ps_display(activitymsg);

  oldcontext = MemoryContextSwitchTo(archive_context);

  /*
   * Since the archiver operates at the bottom of the exception stack,
   * ERRORs turn into FATALs and cause the archiver process to restart.
   * However, using ereport(ERROR, ...) when there are problems is easy to
   * code and maintain.  Therefore, we create our own exception handler to
   * catch ERRORs and return false instead of restarting the archiver
   * whenever there is a failure.
   * 由于归档器在异常栈的底部运行，ERROR 会变成 FATAL 并导致归档进程重启。
   * 然而，在出现问题时使用 ereport(ERROR, ...) 易于编写和维护。
   * 因此，我们创建了自己的异常处理程序来捕获 ERROR，并在发生故障时返回
   * false，而不是重启归档器。
   *
   * We assume ERRORs from the archiving callback are the most common
   * exceptions experienced by the archiver, so we opt to handle exceptions
   * here instead of PgArchiverMain() to avoid reinitializing the archiver
   * too frequently.  We could instead add a sigsetjmp() block to
   * PgArchiverMain() and use PG_TRY/PG_CATCH here, but the extra code to
   * avoid the odd archiver restart doesn't seem worth it.
   * 我们假设来自归档回调的 ERROR 是归档器经历的最常见异常，
   * 因此我们选择在此处处理异常而不是在 PgArchiverMain()
   * 中，以避免过度频繁地重新初始化归档器。 我们本来可以在 PgArchiverMain()
   * 中添加一个 sigsetjmp() block 并在这里使用 PG_TRY/PG_CATCH，
   * 但为了避免偶尔的归档器重启而增加额外代码似乎不值得。
   */
  if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
    /* Since not using PG_TRY, must reset error stack by hand
     * 由于未使用 PG_TRY，必须手动重置错误栈 */
    error_context_stack = NULL;

    /* Prevent interrupts while cleaning up
     * 清理时防止中断 */
    HOLD_INTERRUPTS();

    /* Report the error to the server log.
     * 将错误报告给服务器日志。 */
    EmitErrorReport();

    /*
     * Try to clean up anything the archive module left behind.  We try to
     * cover anything that an archive module could conceivably have left
     * behind, but it is of course possible that modules could be doing
     * unexpected things that require additional cleanup.  Module authors
     * should be sure to do any extra required cleanup in a PG_CATCH block
     * within the archiving callback, and they are encouraged to notify
     * the pgsql-hackers mailing list so that we can add it here.
     * 尝试清理归档模块留下的任何东西。我们试图涵盖归档模块可能留下的任何东西，
     * 但当然模块可能会做一些需要额外清理的意外事情。
     * 模块作者应确保在归档回调中的 PG_CATCH 块中进行任何所需的额外清理，
     * 并鼓励他们通知 pgsql-hackers 邮件列表，以便我们可以在这里添加它。
     */
    disable_all_timeouts(false);
    LWLockReleaseAll();
    ConditionVariableCancelSleep();
    pgstat_report_wait_end();
    pgaio_error_cleanup();
    ReleaseAuxProcessResources(false);
    AtEOXact_Files(false);
    AtEOXact_HashTables(false);

    /*
     * Return to the original memory context and clear ErrorContext for
     * next time.
     * 返回原始内存上下文并清除 ErrorContext 以备下次使用。
     */
    MemoryContextSwitchTo(oldcontext);
    FlushErrorState();

    /* Flush any leaked data
     * 刷新任何泄露的数据 */
    MemoryContextReset(archive_context);

    /* Remove our exception handler
     * 移除我们的异常处理程序 */
    PG_exception_stack = NULL;

    /* Now we can allow interrupts again
     * 现在我们可以再次允许中断了 */
    RESUME_INTERRUPTS();

    /* Report failure so that the archiver retries this file
     * 报告失败，以便归档器重试此文件 */
    ret = false;
  } else {
    /* Enable our exception handler
     * 启用我们的异常处理程序 */
    PG_exception_stack = &local_sigjmp_buf;

    /* Archive the file!
     * 归档文件！ */
    ret =
        ArchiveCallbacks->archive_file_cb(archive_module_state, xlog, pathname);

    /* Remove our exception handler
     * 移除我们的异常处理程序 */
    PG_exception_stack = NULL;

    /* Reset our memory context and switch back to the original one
     * 重置我们的内存上下文并切回到原始状态 */
    MemoryContextSwitchTo(oldcontext);
    MemoryContextReset(archive_context);
  }

  if (ret)
    snprintf(activitymsg, sizeof(activitymsg), "last was %s", xlog);
  else
    snprintf(activitymsg, sizeof(activitymsg), "failed on %s", xlog);
  set_ps_display(activitymsg);

  return ret;
}

/*
 * pgarch_readyXlog
 *
 * Return name of the oldest xlog file that has not yet been archived.
 * 返回尚未归档的最旧 xlog 文件的名称。
 * No notification is set that file archiving is now in progress, so
 * this would need to be extended if multiple concurrent archival
 * tasks were created. If a failure occurs, we will completely
 * re-copy the file at the next available opportunity.
 * 没有设置文件归档正在进行的通知，因此如果创建了多个并发归档任务，则需要对此进行扩展。
 * 如果发生故障，我们将在下次机会时完全重新复制该文件。
 *
 * It is important that we return the oldest, so that we archive xlogs
 * order that they were written, for two reasons:
 * 1) to maintain the sequential chain of xlogs required for recovery
 * 2) because the oldest ones will sooner become candidates for
 * recycling at time of checkpoint
 * 返回最旧的文件非常重要，这样我们就可以按写入顺序归档 xlog，原因有二：
 * 1) 维持恢复所需的 xlog 顺序链
 * 2) 因为最旧的文件会更早成为检查点时回收的候选对象
 *
 * NOTE: the "oldest" comparison will consider any .history file to be older
 * than any other file except another .history file.  Segments on a timeline
 * with a smaller ID will be older than all segments on a timeline with a
 * larger ID; the net result being that past timelines are given higher
 * priority for archiving.  This seems okay, or at least not obviously worth
 * changing.
 * 注意：“最旧”比较会将任何 .history 文件视为比除另一个 .history
 * 文件之外的任何其他文件都旧。 ID 较小的时轴上的段将比 ID
 * 较大的时轴上的所有段都旧；
 * 最终结果是过去的时轴被赋予更高的归档优先级。这看起来没开发，或者至少不明显值得改变。
 *
 * Function purpose: Scan archive_status directory and returns next file name.
 * 函数作用：扫描 archive_status 目录并利用优先堆筛选出下一批需要归档的最旧 WAL 文件。
 */
static bool pgarch_readyXlog(char *xlog) {
  char XLogArchiveStatusDir[MAXPGPATH];
  DIR *rldir;
  struct dirent *rlde;

  /*
   * If a directory scan was requested, clear the stored file names and
   * proceed.
   * 如果请求了目录扫描，请清除存储的文件名并继续。
   */
  if (pg_atomic_exchange_u32(&PgArch->force_dir_scan, 0) == 1)
    arch_files->arch_files_size = 0;

  /*
   * If we still have stored file names from the previous directory scan,
   * try to return one of those.  We check to make sure the status file is
   * still present, as the archive_command for a previous file may have
   * already marked it done.
   * 如果我们仍有上次目录扫描存储的文件名，请尝试返回其中一个。
   * 我们检查以确保状态文件仍然存在，因为上一个文件的 archive_command
   * 可能已经将其标记为已完成。
   */
  while (arch_files->arch_files_size > 0) {
    struct stat st;
    char status_file[MAXPGPATH];
    char *arch_file;

    arch_files->arch_files_size--;
    arch_file = arch_files->arch_files[arch_files->arch_files_size];
    StatusFilePath(status_file, arch_file, ".ready");

    if (stat(status_file, &st) == 0) {
      strcpy(xlog, arch_file);
      return true;
    } else if (errno != ENOENT)
      ereport(ERROR, (errcode_for_file_access(),
                      errmsg("could not stat file \"%s\": %m", status_file)));
  }

  /* arch_heap is probably empty, but let's make sure
   * arch_heap 可能是空的，但让我们确认一下 */
  binaryheap_reset(arch_files->arch_heap);

  /*
   * Open the archive status directory and read through the list of files
   * with the .ready suffix, looking for the earliest files.
   * 打开归档状态目录并读取带有 .ready 后缀的文件列表，查找最早的文件。
   */
  snprintf(XLogArchiveStatusDir, MAXPGPATH, XLOGDIR "/archive_status");
  rldir = AllocateDir(XLogArchiveStatusDir);

  while ((rlde = ReadDir(rldir, XLogArchiveStatusDir)) != NULL) {
    int basenamelen = (int)strlen(rlde->d_name) - 6;
    char basename[MAX_XFN_CHARS + 1];
    char *arch_file;

    /* Ignore entries with unexpected number of characters
     * 忽略字符数异常的条目 */
    if (basenamelen < MIN_XFN_CHARS || basenamelen > MAX_XFN_CHARS)
      continue;

    /* Ignore entries with unexpected characters
     * 忽略包含异常字符的条目 */
    if (strspn(rlde->d_name, VALID_XFN_CHARS) < basenamelen)
      continue;

    /* Ignore anything not suffixed with .ready
     * 忽略任何不以 .ready 结尾的文件 */
    if (strcmp(rlde->d_name + basenamelen, ".ready") != 0)
      continue;

    /* Truncate off the .ready
     * 截掉 .ready 后缀 */
    memcpy(basename, rlde->d_name, basenamelen);
    basename[basenamelen] = '\0';

    /*
     * Store the file in our max-heap if it has a high enough priority.
     * 如果文件具有足够高的优先级，则将其存储在我们的最大堆中。
     */
    if (arch_files->arch_heap->bh_size < NUM_FILES_PER_DIRECTORY_SCAN) {
      /* If the heap isn't full yet, quickly add it.
       * 如果堆尚未满，快速添加它。 */
      arch_file = arch_files->arch_filenames[arch_files->arch_heap->bh_size];
      strcpy(arch_file, basename);
      binaryheap_add_unordered(arch_files->arch_heap,
                               CStringGetDatum(arch_file));

      /* If we just filled the heap, make it a valid one.
       * 如果我们刚刚填满了堆，使其成为有效的堆。 */
      if (arch_files->arch_heap->bh_size == NUM_FILES_PER_DIRECTORY_SCAN)
        binaryheap_build(arch_files->arch_heap);
    } else if (ready_file_comparator(binaryheap_first(arch_files->arch_heap),
                                     CStringGetDatum(basename), NULL) > 0) {
      /*
       * Remove the lowest priority file and add the current one to the
       * heap.
       * 移除优先级最低的文件，并将当前文件添加到堆中。
       */
      arch_file =
          DatumGetCString(binaryheap_remove_first(arch_files->arch_heap));
      strcpy(arch_file, basename);
      binaryheap_add(arch_files->arch_heap, CStringGetDatum(arch_file));
    }
  }
  FreeDir(rldir);

  /* If no files were found, simply return.
   * 如果没有找到文件，只需返回。 */
  if (arch_files->arch_heap->bh_size == 0)
    return false;

  /*
   * If we didn't fill the heap, we didn't make it a valid one.  Do that
   * now.
   * 如果我们没有填满堆，那么它就不是有效的堆。现在进行构建。
   */
  if (arch_files->arch_heap->bh_size < NUM_FILES_PER_DIRECTORY_SCAN)
    binaryheap_build(arch_files->arch_heap);

  /*
   * Fill arch_files array with the files to archive in ascending order of
   * priority.
   * 按优先级升序将要归档的文件填充到 arch_files 数组中。
   */
  arch_files->arch_files_size = arch_files->arch_heap->bh_size;
  for (int i = 0; i < arch_files->arch_files_size; i++)
    arch_files->arch_files[i] =
        DatumGetCString(binaryheap_remove_first(arch_files->arch_heap));

  /* Return the highest priority file.
   * 返回优先级最高的文件。 */
  arch_files->arch_files_size--;
  strcpy(xlog, arch_files->arch_files[arch_files->arch_files_size]);

  return true;
}

/*
 * ready_file_comparator
 *
 * Compares the archival priority of the given files to archive.  If "a"
 * has a higher priority than "b", a negative value will be returned.  If
 * "b" has a higher priority than "a", a positive value will be returned.
 * If "a" and "b" have equivalent values, 0 will be returned.
 * 比较给定的要归档文件的归档优先级。如果“a”的优先级高于“b”，则返回负值。
 * 如果“b”的优先级高于“a”，则返回正值。如果“a”和“b”的值相等，则返回 0。
 *
 * Function purpose: Compare archival priority (history files first, then older files).
 * 函数作用：确定待归档文件的优先级顺序（.history 历史文件最高，其次是最旧的文件）。
 */
static int ready_file_comparator(Datum a, Datum b, void *arg) {
  char *a_str = DatumGetCString(a);
  char *b_str = DatumGetCString(b);
  bool a_history = IsTLHistoryFileName(a_str);
  bool b_history = IsTLHistoryFileName(b_str);

  /* Timeline history files always have the highest priority.
   * 时轴历史文件始终具有最高优先级。 */
  if (a_history != b_history)
    return a_history ? -1 : 1;

  /* Priority is given to older files.
   * 优先级赋予较旧的文件。 */
  return strcmp(a_str, b_str);
}

/*
 * PgArchForceDirScan
 *
 * When called, the next call to pgarch_readyXlog() will perform a
 * directory scan.  This is useful for ensuring that important files such
 * as timeline history files are archived as quickly as possible.
 * 调用时，下次对 pgarch_readyXlog() 的调用将执行目录扫描。
 * 这对于确保在此类重要文件（如时轴历史文件）产生时尽可能快地进行归档非常有用。
 *
 * Function purpose: Force next pgarch_readyXlog call to scan directory.
 * 函数作用：强制下一次 pgarch_readyXlog() 执行物理目录扫描。
 */
void PgArchForceDirScan(void) {
  pg_atomic_write_membarrier_u32(&PgArch->force_dir_scan, 1);
}

/*
 * pgarch_archiveDone
 *
 * Emit notification that an xlog file has been successfully archived.
 * We do this by renaming the status file from NNN.ready to NNN.done.
 * Eventually, a checkpoint process will notice this and delete both the
 * NNN.done file and the xlog file itself.
 * 发出通知，指示 xlog 文件已成功归档。
 * 我们通过将状态文件从 NNN.ready 重命名为 NNN.done 来实现这一点。
 * 最终，检查点进程将注意到这一点，并删除 NNN.done 文件和 xlog 文件本身。
 *
 * Function purpose: Rename .ready file to .done.
 * 函数作用：归档成功后，将对应的 ready 状态文件重命名为 done。
 */
static void pgarch_archiveDone(char *xlog) {
  char rlogready[MAXPGPATH];
  char rlogdone[MAXPGPATH];

  StatusFilePath(rlogready, xlog, ".ready");
  StatusFilePath(rlogdone, xlog, ".done");

  /*
   * To avoid extra overhead, we don't durably rename the .ready file to
   * .done.  Archive commands and libraries must gracefully handle attempts
   * to re-archive files (e.g., if the server crashes just before this
   * function is called), so it should be okay if the .ready file reappears
   * after a crash.
   * 为了避免额外的开销，我们不会以持久化的方式将 .ready 文件重命名为 .done。
   * 归档命令和库必须优雅地处理重新归档文件的尝试（例如，如果服务器就在调用此函数之前崩溃），
   * 因此如果 .ready 文件在崩溃后重新出现也应该是可以接受的。
   */
  if (rename(rlogready, rlogdone) < 0)
    ereport(WARNING, (errcode_for_file_access(),
                      errmsg("could not rename file \"%s\" to \"%s\": %m",
                             rlogready, rlogdone)));
}

/*
 * pgarch_die
 *
 * Exit-time cleanup handler
 * 退出时的清理处理程序
 *
 * Function purpose: Reset archiver proc number to invalid.
 * 函数作用：退出时，将共享内存中的 archiver 进程编号重置为无效值。
 */
static void pgarch_die(int code, Datum arg) {
  PgArch->pgprocno = INVALID_PROC_NUMBER;
}

/*
 * Interrupt handler for WAL archiver process.
 * WAL 归档进程的中断处理程序。
 *
 * This is called in the loops pgarch_MainLoop and pgarch_ArchiverCopyLoop.
 * It checks for barrier events, config update and request for logging of
 * memory contexts, but not shutdown request because how to handle
 * shutdown request is different between those loops.
 * 此函数在 pgarch_MainLoop 和 pgarch_ArchiverCopyLoop 循环中调用。
 * 它检查屏障事件、配置更新和内存上下文记录请求，但不包括停机请求，
 * 因为这两个循环处理停机请求的方式不同。
 *
 * Function purpose: Handle pending signals/GUC reloads.
 * 函数作用：检查和处理中断屏障、内存快照写入及 GUC 配置文件的动态重新载入。
 */
static void ProcessPgArchInterrupts(void) {
  if (ProcSignalBarrierPending)
    ProcessProcSignalBarrier();

  /* Perform logging of memory contexts of this process
   * 记录此进程的内存上下文 */
  if (LogMemoryContextPending)
    ProcessLogMemoryContextInterrupt();

  if (ConfigReloadPending) {
    char *archiveLib = pstrdup(XLogArchiveLibrary);
    bool archiveLibChanged;

    ConfigReloadPending = false;
    ProcessConfigFile(PGC_SIGHUP);

    if (XLogArchiveLibrary[0] != '\0' && XLogArchiveCommand[0] != '\0')
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("both \"archive_command\" and \"archive_library\" set"),
               errdetail("Only one of \"archive_command\", \"archive_library\" "
                         "may be set.")));

    archiveLibChanged = strcmp(XLogArchiveLibrary, archiveLib) != 0;
    pfree(archiveLib);

    if (archiveLibChanged) {
      /*
       * Point: We simply restart the archiver.  The new archive module will be loaded when the
       * new archiver process starts up.  Note that this triggers the
       * module's shutdown callback, if defined.
       * 注意：我们只需重启归档器。当新的归档进程启动时，将加载新的归档模块。
       * 请注意，这将触发模块的停机回调（如果已定义）。
       */
      ereport(LOG, (errmsg("restarting archiver process because value of "
                           "\"archive_library\" was changed")));

      proc_exit(0);
    }
  }
}

/*
 * LoadArchiveLibrary
 *
 * Loads the archiving callbacks into our local ArchiveCallbacks.
 * 将归档回调加载到本地 ArchiveCallbacks 中。
 *
 * Function purpose: Load and initialize the archive library module or shell.
 * 函数作用：根据配置加载动态库或 shell 并调用相应的回调初始化模块状态。
 */
static void LoadArchiveLibrary(void) {
  ArchiveModuleInit archive_init;

  if (XLogArchiveLibrary[0] != '\0' && XLogArchiveCommand[0] != '\0')
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("both \"archive_command\" and \"archive_library\" set"),
             errdetail("Only one of \"archive_command\", \"archive_library\" "
                       "may be set.")));

  /*
   * If shell archiving is enabled, use our special initialization function.
   * Otherwise, load the library and call its _PG_archive_module_init().
   * 如果启用了 shell 归档，请使用我们的特殊初始化函数。
   * 否则，加载库并调用其 _PG_archive_module_init()。
   */
  if (XLogArchiveLibrary[0] == '\0')
    archive_init = shell_archive_init;
  else
    archive_init = (ArchiveModuleInit)load_external_function(
        XLogArchiveLibrary, "_PG_archive_module_init", false, NULL);

  if (archive_init == NULL)
    ereport(ERROR, (errmsg("archive modules have to define the symbol %s",
                           "_PG_archive_module_init")));

  ArchiveCallbacks = (*archive_init)();

  if (ArchiveCallbacks->archive_file_cb == NULL)
    ereport(ERROR,
            (errmsg("archive modules must register an archive callback")));

  archive_module_state =
      (ArchiveModuleState *)palloc0(sizeof(ArchiveModuleState));
  if (ArchiveCallbacks->startup_cb != NULL)
    ArchiveCallbacks->startup_cb(archive_module_state);

  before_shmem_exit(pgarch_call_module_shutdown_cb, 0);
}

/*
 * Call the shutdown callback of the loaded archive module, if defined.
 * 如果已定义，调用已加载归档模块的停机回调。
 *
 * Function purpose: Trigger shutdown callback.
 * 函数作用：调用当前加载模块对应的注销/关闭回调函数。
 */
static void pgarch_call_module_shutdown_cb(int code, Datum arg) {
  if (ArchiveCallbacks->shutdown_cb != NULL)
    ArchiveCallbacks->shutdown_cb(archive_module_state);
}
