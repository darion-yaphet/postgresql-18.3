/*
 * fork_process.c
 *	 A simple wrapper on top of fork(). This does not handle the
 *	 EXEC_BACKEND case; it might be extended to do so, but it would be
 *	 considerably more complex.
 *	 对 fork() 的一个简单封装。这并不处理 EXEC_BACKEND 情况；
 *	 它可以被扩展以支持该情况，但这会复杂得多。
 *
 * Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/fork_process.c
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/fork_process.h"

#ifndef WIN32
/*
 * Wrapper for fork(). Return values are the same as those for fork():
 * -1 if the fork failed, 0 in the child process, and the PID of the
 * child in the parent process.  Signals are blocked while forking, so
 * the child must unblock.
 */
/*
 * fork() 的包装函数。返回值与 fork() 相同：
 * 如果 fork 失败返回 -1，在子进程中返回 0，在父进程中返回子进程的 PID。
 * 在 fork 期间信号会被阻塞，因此子进程必须解除阻塞。
 *
 * Function purpose: Wrap fork() system call with custom signal masking and OOM score adjustment for PostgreSQL.
 * 函数作用：封装 fork() 系统调用，为 PostgreSQL 提供自定义信号屏蔽和 OOM（内存溢出）分值调整。
 *
 * Core workflow:
 * 核心流程：
 * 1. Flush all open stdio streams before forking.
 *    在 fork 之前刷新所有打开的标准输入输出流。
 * 2. Block signals using BlockSig mask.
 *    使用 BlockSig 掩码阻塞信号。
 * 3. Invoke system fork().
 *    调用系统 fork()。
 * 4. In child process: Update PID, adjust OOM score based on PG_OOM_ADJUST_FILE/PG_OOM_ADJUST_VALUE, and initialize random seed.
 *    在子进程中：更新 PID，根据环境变量调整 OOM 分数，并初始化随机数生成器。
 * 5. In parent process: Restore original signal mask.
 *    在父进程中：恢复原先的信号屏蔽字。
 */
pid_t
fork_process(void)
{
	pid_t		result;
	const char *oomfilename;
	sigset_t	save_mask;

#ifdef LINUX_PROFILE
	struct itimerval prof_itimer;
#endif

	/*
	 * Flush stdio channels just before fork, to avoid double-output problems.
	 */
	/*
	 * 在 fork 之前立即刷新 stdio 通道，以避免重复输出问题。
	 */
	fflush(NULL);

#ifdef LINUX_PROFILE

	/*
	 * Linux's fork() resets the profiling timer in the child process. If we
	 * want to profile child processes then we need to save and restore the
	 * timer setting.  This is a waste of time if not profiling, however, so
	 * only do it if commanded by specific -DLINUX_PROFILE switch.
	 */
	/*
	 * Linux 的 fork() 会重置子进程中的性能分析定时器。如果我们
	 * 想要对子进程进行性能分析，我们就需要保存并恢复定时器设置。
	 * 然而，如果不进行性能分析，这就是在浪费时间，因此
	 * 仅在由特定的 -DLINUX_PROFILE 宏控制时才执行此操作。
	 */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

	/*
	 * We start postmaster children with signals blocked.  This allows them to
	 * install their own handlers before unblocking, to avoid races where they
	 * might run the postmaster's handler and miss an important control
	 * signal. With more analysis this could potentially be relaxed.
	 */
	/*
	 * 我们在启动 postmaster 子进程时将其信号阻塞。这允许它们在解除阻塞之前
	 * 安装它们自己的处理程序，以避免它们可能运行 postmaster 的处理程序并丢失
	 * 重要控制信号的竞争情况。随着更深入的分析，这有可能被放宽。
	 */
	sigprocmask(SIG_SETMASK, &BlockSig, &save_mask);
	result = fork();
	if (result == 0)
	{
		/* fork succeeded, in child */
		/* fork 成功，在子进程中 */
		MyProcPid = getpid();
#ifdef LINUX_PROFILE
		setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

		/*
		 * By default, Linux tends to kill the postmaster in out-of-memory
		 * situations, because it blames the postmaster for the sum of child
		 * process sizes *including shared memory*.  (This is unbelievably
		 * stupid, but the kernel hackers seem uninterested in improving it.)
		 * Therefore it's often a good idea to protect the postmaster by
		 * setting its OOM score adjustment negative (which has to be done in
		 * a root-owned startup script).  Since the adjustment is inherited by
		 * child processes, this would ordinarily mean that all the
		 * postmaster's children are equally protected against OOM kill, which
		 * is not such a good idea.  So we provide this code to allow the
		 * children to change their OOM score adjustments again.  Both the
		 * file name to write to and the value to write are controlled by
		 * environment variables, which can be set by the same startup script
		 * that did the original adjustment.
		 */
		/*
		 * 默认情况下，Linux 倾向于在内存不足的情况下杀死 postmaster，
		 * 因为它将子进程大小的总和（包括共享内存）归咎于 postmaster。
		 * （这真是难以置信的愚蠢，但内核黑客们似乎对改进它不感兴趣。）
		 * 因此，通过将其 OOM 分数调整设置为负值（这必须在
		 * root 所有的启动脚本中完成）来保护 postmaster 通常是一个好主意。
		 * 由于该调整会被子进程继承，这通常意味着所有
		 * postmaster 的子进程都同样受到保护免受 OOM 杀戮，这不是个好主意。
		 * 所以我们提供这段代码，允许子进程再次更改它们的 OOM 分数调整。
		 * 要写入的文件名和要写入的值都由环境变量控制，
		 * 这些环境变量可以由执行原始调整的同一个启动脚本来设置。
		 */
		oomfilename = getenv("PG_OOM_ADJUST_FILE");

		if (oomfilename != NULL)
		{
			/*
			 * Use open() not stdio, to ensure we control the open flags. Some
			 * Linux security environments reject anything but O_WRONLY.
			 */
			/*
			 * 使用 open() 而不是 stdio，以确保我们能够控制打开标志。一些
			 * Linux 安全环境拒绝除 O_WRONLY 之外的任何操作。
			 */
			int			fd = open(oomfilename, O_WRONLY, 0);

			/* We ignore all errors */
			/* 我们忽略所有错误 */
			if (fd >= 0)
			{
				const char *oomvalue = getenv("PG_OOM_ADJUST_VALUE");
				int			rc;

				if (oomvalue == NULL)	/* supply a useful default */
										/* 提供一个有用的默认值 */
					oomvalue = "0";

				rc = write(fd, oomvalue, strlen(oomvalue));
				(void) rc;
				close(fd);
			}
		}

		/* do post-fork initialization for random number generation */
		/* 进行 fork 后的初始化以用于随机数生成 */
		pg_strong_random_init();
	}
	else
	{
		/* in parent, restore signal mask */
		/* 在父进程中，恢复信号掩码 */
		sigprocmask(SIG_SETMASK, &save_mask, NULL);
	}

	return result;
}

#endif							/* ! WIN32 */

