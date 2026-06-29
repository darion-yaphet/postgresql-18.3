/*--------------------------------------------------------------------
 * bgworker.c
 *		POSTGRES pluggable background workers implementation
 *		POSTGRES 可插拔后台工作进程实现
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/bgworker.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/ascii.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

/*
 * The postmaster's list of registered background workers, in private memory.
 */
/*
 * postmaster 注册的后台工作进程列表，位于私有内存中。
 */
dlist_head	BackgroundWorkerList = DLIST_STATIC_INIT(BackgroundWorkerList);

/*
 * BackgroundWorkerSlots exist in shared memory and can be accessed (via
 * the BackgroundWorkerArray) by both the postmaster and by regular backends.
 * However, the postmaster cannot take locks, even spinlocks, because this
 * might allow it to crash or become wedged if shared memory gets corrupted.
 * Such an outcome is intolerable.  Therefore, we need a lockless protocol
 * for coordinating access to this data.
 * BackgroundWorkerSlots 存在于共享内存中，postmaster 和普通后端都可以
 * （通过 BackgroundWorkerArray）访问它们。
 * 然而，postmaster 不能获取锁，甚至是自旋锁，因为如果共享内存损坏，这可能会
 * 导致其崩溃或卡住。这样的后果是无法容忍的。因此，我们需要一个无锁协议
 * 来协调对这些数据的访问。
 *
 * The 'in_use' flag is used to hand off responsibility for the slot between
 * the postmaster and the rest of the system.  When 'in_use' is false,
 * the postmaster will ignore the slot entirely, except for the 'in_use' flag
 * itself, which it may read.  In this state, regular backends may modify the
 * slot.  Once a backend sets 'in_use' to true, the slot becomes the
 * responsibility of the postmaster.  Regular backends may no longer modify it,
 * but the postmaster may examine it.  Thus, a backend initializing a slot
 * must fully initialize the slot - and insert a write memory barrier - before
 * marking it as in use.
 * 'in_use' 标志用于在 postmaster 和系统的其余部分之间交接槽的控制权。
 * 当 'in_use' 为 false 时，postmaster 将完全忽略该槽，除了 'in_use' 标志
 * 本身（它可能会读取该标志）。在这种状态下，普通后端可以修改该槽。
 * 一旦后端将 'in_use' 设置为 true，该槽便由 postmaster 负责。
 * 普通后端不能再修改它，但 postmaster 可以检查它。因此，初始化槽的后端
 * 必须完全初始化该槽，并在将其标记为使用中之前插入一个写内存屏障。
 *
 * As an exception, however, even when the slot is in use, regular backends
 * may set the 'terminate' flag for a slot, telling the postmaster not
 * to restart it.  Once the background worker is no longer running, the slot
 * will be released for reuse.
 * 然而，作为一个例外，即使在槽被使用时，普通后端也可以为该槽设置 'terminate' 标志，
 * 告诉 postmaster 不要重新启动它。一旦后台工作进程不再运行，该槽将被释放以便重用。
 *
 * In addition to coordinating with the postmaster, backends modifying this
 * data structure must coordinate with each other.  Since they can take locks,
 * this is straightforward: any backend wishing to manipulate a slot must
 * take BackgroundWorkerLock in exclusive mode.  Backends wishing to read
 * data that might get concurrently modified by other backends should take
 * this lock in shared mode.  No matter what, backends reading this data
 * structure must be able to tolerate concurrent modifications by the
 * postmaster.
 * 除了与 postmaster 协调外，修改此数据结构的后端之间也必须相互协调。
 * 由于它们可以获取锁，这很简单：任何希望操作槽的后端都必须以排他模式获取
 * BackgroundWorkerLock。希望读取可能被其他后端并发修改的数据的后端应该以
 * 共享模式获取此锁。无论如何，读取此数据结构的后端必须能够容忍 postmaster 
 * 的并发修改。
 */
typedef struct BackgroundWorkerSlot
{
	bool		in_use;
	bool		terminate;
	pid_t		pid;			/* InvalidPid = not started yet; 0 = dead */
								/* InvalidPid = 尚未启动；0 = 已死亡 */
	uint64		generation;		/* incremented when slot is recycled */
								/* 槽被回收时递增 */
	BackgroundWorker worker;
} BackgroundWorkerSlot;

/*
 * In order to limit the total number of parallel workers (according to
 * max_parallel_workers GUC), we maintain the number of active parallel
 * workers.  Since the postmaster cannot take locks, two variables are used for
 * this purpose: the number of registered parallel workers (modified by the
 * backends, protected by BackgroundWorkerLock) and the number of terminated
 * parallel workers (modified only by the postmaster, lockless).  The active
 * number of parallel workers is the number of registered workers minus the
 * terminated ones.  These counters can of course overflow, but it's not
 * important here since the subtraction will still give the right number.
 */
/*
 * 为了限制并行工作进程的总数（根据 max_parallel_workers GUC），我们维护了活动并行
 * 工作进程的数量。由于 postmaster 不能获取锁，为此目的使用了两个变量：
 * 已注册的并行工作进程数量（由后端修改，受 BackgroundWorkerLock 保护）和
 * 已终止的并行工作进程数量（仅由 postmaster 修改，无锁）。活动的并行工作进程数量
 * 是已注册的工作进程数量减去已终止的工作进程数量。这些计数器当然可能会溢出，
 * 但在这里并不重要，因为减法运算仍然会给出正确的数值。
 */
typedef struct BackgroundWorkerArray
{
	int			total_slots;
	uint32		parallel_register_count;
	uint32		parallel_terminate_count;
	BackgroundWorkerSlot slot[FLEXIBLE_ARRAY_MEMBER];
} BackgroundWorkerArray;

struct BackgroundWorkerHandle
{
	int			slot;
	uint64		generation;
};

static BackgroundWorkerArray *BackgroundWorkerData;

/*
 * List of internal background worker entry points.  We need this for
 * reasons explained in LookupBackgroundWorkerFunction(), below.
 */
/*
 * 内部后台工作进程入口点列表。我们需要这个，原因在下文的 LookupBackgroundWorkerFunction() 中解释。
 */
static const struct
{
	const char *fn_name;
	bgworker_main_type fn_addr;
}			InternalBGWorkers[] =

{
	{
		"ParallelWorkerMain", ParallelWorkerMain
	},
	{
		"ApplyLauncherMain", ApplyLauncherMain
	},
	{
		"ApplyWorkerMain", ApplyWorkerMain
	},
	{
		"ParallelApplyWorkerMain", ParallelApplyWorkerMain
	},
	{
		"TablesyncWorkerMain", TablesyncWorkerMain
	}
};

/* Private functions. */
/* 私有函数。 */
static bgworker_main_type LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname);


/*
 * Calculate shared memory needed.
 */
/*
 * 计算所需的共享内存大小。
 *
 * Function purpose: Calculate the shared memory size required for the BackgroundWorkerArray.
 * 函数作用：计算 BackgroundWorkerArray 所需的共享内存大小。
 */
Size
BackgroundWorkerShmemSize(void)
{
	Size		size;

	/* Array of workers is variably sized. */
	/* 工作进程数组的大小是可变的。 */
	size = offsetof(BackgroundWorkerArray, slot);
	size = add_size(size, mul_size(max_worker_processes,
								   sizeof(BackgroundWorkerSlot)));

	return size;
}

/*
 * Initialize shared memory.
 */
/*
 * 初始化共享内存。
 *
 * Function purpose: Initialize background worker shared memory structures.
 * 函数作用：初始化后台工作进程的共享内存结构。
 *
 * Core workflow:
 * 核心流程：
 * 1. Request shared memory via ShmemInitStruct.
 *    通过 ShmemInitStruct 申请共享内存。
 * 2. If not in child process, initialize slots, register static background workers into slots, and mark others as not in use.
 *    如果不在子进程中，初始化槽，将静态后台工作进程注册到槽中，并将其他槽标记为未使用。
 */
void
BackgroundWorkerShmemInit(void)
{
	bool		found;

	BackgroundWorkerData = ShmemInitStruct("Background Worker Data",
										   BackgroundWorkerShmemSize(),
										   &found);
	if (!IsUnderPostmaster)
	{
		dlist_iter	iter;
		int			slotno = 0;

		BackgroundWorkerData->total_slots = max_worker_processes;
		BackgroundWorkerData->parallel_register_count = 0;
		BackgroundWorkerData->parallel_terminate_count = 0;

		/*
		 * Copy contents of worker list into shared memory.  Record the shared
		 * memory slot assigned to each worker.  This ensures a 1-to-1
		 * correspondence between the postmaster's private list and the array
		 * in shared memory.
		 */
		/*
		 * 将工作进程列表的内容复制到共享内存中。记录分配给每个工作进程的共享
		 * 内存槽。这确保了 postmaster 的私有列表与共享内存中的数组之间存在
		 * 一对一的对应关系。
		 */
		dlist_foreach(iter, &BackgroundWorkerList)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
			RegisteredBgWorker *rw;

			rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
			Assert(slotno < max_worker_processes);
			slot->in_use = true;
			slot->terminate = false;
			slot->pid = InvalidPid;
			slot->generation = 0;
			rw->rw_shmem_slot = slotno;
			rw->rw_worker.bgw_notify_pid = 0;	/* might be reinit after crash */
												/* 崩溃后可能会重新初始化 */
			memcpy(&slot->worker, &rw->rw_worker, sizeof(BackgroundWorker));
			++slotno;
		}

		/*
		 * Mark any remaining slots as not in use.
		 */
		/*
		 * 将任何剩余的槽标记为未使用。
		 */
		while (slotno < max_worker_processes)
		{
			BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

			slot->in_use = false;
			++slotno;
		}
	}
	else
		Assert(found);
}

/*
 * Search the postmaster's backend-private list of RegisteredBgWorker objects
 * for the one that maps to the given slot number.
 */
/*
 * 在 postmaster 的后台私有 RegisteredBgWorker 对象列表中，搜索映射到给定槽号的那个对象。
 *
 * Function purpose: Lookup RegisteredBgWorker in BackgroundWorkerList by slot number.
 * 函数作用：通过槽号在 BackgroundWorkerList 中查找 RegisteredBgWorker。
 */
static RegisteredBgWorker *
FindRegisteredWorkerBySlotNumber(int slotno)
{
	dlist_iter	iter;

	dlist_foreach(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		if (rw->rw_shmem_slot == slotno)
			return rw;
	}

	return NULL;
}

/*
 * Notice changes to shared memory made by other backends.
 * Accept new worker requests only if allow_new_workers is true.
 *
 * This code runs in the postmaster, so we must be very careful not to assume
 * that shared memory contents are sane.  Otherwise, a rogue backend could
 * take out the postmaster.
 */
/*
 * 注意其他后端对共享内存所做的更改。
 * 仅当 allow_new_workers 为 true 时，才接受新的工作进程请求。
 *
 * 此代码在 postmaster 中运行，因此我们必须非常小心，不要假定共享内存内容是健全的。
 * 否则，流氓后端可能会搞垮 postmaster。
 *
 * Function purpose: Scan shared memory for worker requests (registration or termination) and update postmaster local state.
 * 函数作用：扫描共享内存中的工作进程请求（注册或终止）并更新 postmaster 的本地状态。
 *
 * Core workflow:
 * 核心流程：
 * 1. Validate total_slots size.
 *    校验共享内存中的总槽数。
 * 2. Scan each slot. If in_use is true, check if the worker is already registered.
 *    扫描每一个槽，如果 in_use 为真，检查该工作进程是否已在本地注册。
 * 3. Handle terminations: send SIGTERM to running worker, or free the slot if not started.
 *    处理终止请求：向正在运行的工作进程发送 SIGTERM 信号，或直接释放槽（如果尚未启动）。
 * 4. Handle registration: copy metadata to a new RegisteredBgWorker, apply checks, and insert into BackgroundWorkerList.
 *    处理注册请求：将元数据复制到新创建的 RegisteredBgWorker 中，进行安全性检查，并插入到 BackgroundWorkerList。
 */
void
BackgroundWorkerStateChange(bool allow_new_workers)
{
	int			slotno;

	/*
	 * The total number of slots stored in shared memory should match our
	 * notion of max_worker_processes.  If it does not, something is very
	 * wrong.  Further down, we always refer to this value as
	 * max_worker_processes, in case shared memory gets corrupted while we're
	 * looping.
	 */
	/*
	 * 共享内存中存储的总槽数应该与我们的 max_worker_processes 概念一致。
	 * 如果不一致，说明有些地方非常不对劲。在下文中，我们总是将此值称为
	 * max_worker_processes，以防在循环时共享内存损坏。
	 */
	if (max_worker_processes != BackgroundWorkerData->total_slots)
	{
		ereport(LOG,
				(errmsg("inconsistent background worker state (\"max_worker_processes\"=%d, total slots=%d)",
						max_worker_processes,
						BackgroundWorkerData->total_slots)));
		return;
	}

	/*
	 * Iterate through slots, looking for newly-registered workers or workers
	 * who must die.
	 */
	/*
	 * 遍历槽，寻找新注册的工作进程或必须终止的工作进程。
	 */
	for (slotno = 0; slotno < max_worker_processes; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];
		RegisteredBgWorker *rw;

		if (!slot->in_use)
			continue;

		/*
		 * Make sure we don't see the in_use flag before the updated slot
		 * contents.
		 */
		/*
		 * 确保我们在看到更新后的槽内容之前，不会先看到 in_use 标志。
		 */
		pg_read_barrier();

		/* See whether we already know about this worker. */
		/* 看看我们是否已经知道这个工作进程。 */
		rw = FindRegisteredWorkerBySlotNumber(slotno);
		if (rw != NULL)
		{
			/*
			 * In general, the worker data can't change after it's initially
			 * registered.  However, someone can set the terminate flag.
			 */
			/*
			 * 一般来说，工作进程数据在最初注册后不能更改。然而，有人可以设置 terminate 标志。
			 */
			if (slot->terminate && !rw->rw_terminate)
			{
				rw->rw_terminate = true;
				if (rw->rw_pid != 0)
					kill(rw->rw_pid, SIGTERM);
				else
				{
					/* Report never-started, now-terminated worker as dead. */
					/* 将从未启动、现在已终止的工作进程报告为已死。 */
					ReportBackgroundWorkerPID(rw);
				}
			}
			continue;
		}

		/*
		 * If we aren't allowing new workers, then immediately mark it for
		 * termination; the next stanza will take care of cleaning it up.
		 * Doing this ensures that any process waiting for the worker will get
		 * awoken, even though the worker will never be allowed to run.
		 */
		/*
		 * 如果我们不允许新的工作进程，则立即将其标记为终止；接下来的节将负责清理它。
		 * 这样做可以确保任何等待该工作进程的进程都会被唤醒，即使该工作进程永远不被允许运行。
		 */
		if (!allow_new_workers)
			slot->terminate = true;

		/*
		 * If the worker is marked for termination, we don't need to add it to
		 * the registered workers list; we can just free the slot. However, if
		 * bgw_notify_pid is set, the process that registered the worker may
		 * need to know that we've processed the terminate request, so be sure
		 * to signal it.
		 */
		/*
		 * 如果工作进程被标记为终止，我们不需要将其添加到已注册的工作进程列表中；
		 * 我们只需释放该槽。然而，如果设置了 bgw_notify_pid，注册该工作进程的进程可能
		 * 需要知道我们已经处理了终止请求，所以请务必向其发送信号。
		 */
		if (slot->terminate)
		{
			int			notify_pid;

			/*
			 * We need a memory barrier here to make sure that the load of
			 * bgw_notify_pid and the update of parallel_terminate_count
			 * complete before the store to in_use.
			 */
			/*
			 * 我们在这里需要一个内存屏障，以确保 bgw_notify_pid 的加载和
			 * parallel_terminate_count 的更新在写入 in_use 之前完成。
			 */
			notify_pid = slot->worker.bgw_notify_pid;
			if ((slot->worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
				BackgroundWorkerData->parallel_terminate_count++;
			slot->pid = 0;

			pg_memory_barrier();
			slot->in_use = false;

			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);

			continue;
		}

		/*
		 * Copy the registration data into the registered workers list.
		 */
		/*
		 * 将注册数据复制到已注册的工作进程列表中。
		 */
		rw = MemoryContextAllocExtended(PostmasterContext,
										sizeof(RegisteredBgWorker),
										MCXT_ALLOC_NO_OOM | MCXT_ALLOC_ZERO);
		if (rw == NULL)
		{
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
			return;
		}

		/*
		 * Copy strings in a paranoid way.  If shared memory is corrupted, the
		 * source data might not even be NUL-terminated.
		 */
		/*
		 * 以偏执的方式复制字符串。如果共享内存损坏，源数据甚至可能不是以 NUL 结尾的。
		 */
		ascii_safe_strlcpy(rw->rw_worker.bgw_name,
						   slot->worker.bgw_name, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_type,
						   slot->worker.bgw_type, BGW_MAXLEN);
		ascii_safe_strlcpy(rw->rw_worker.bgw_library_name,
						   slot->worker.bgw_library_name, MAXPGPATH);
		ascii_safe_strlcpy(rw->rw_worker.bgw_function_name,
						   slot->worker.bgw_function_name, BGW_MAXLEN);

		/*
		 * Copy various fixed-size fields.
		 *
		 * flags, start_time, and restart_time are examined by the postmaster,
		 * but nothing too bad will happen if they are corrupted.  The
		 * remaining fields will only be examined by the child process.  It
		 * might crash, but we won't.
		 */
		/*
		 * 复制各种固定大小的字段。
		 *
		 * flags、start_time 和 restart_time 会被 postmaster 检查，
		 * 但如果它们损坏了，也不会发生太糟糕的事情。其余字段将仅由子进程检查。
		 * 子进程可能会崩溃，但我们不会。
		 */
		rw->rw_worker.bgw_flags = slot->worker.bgw_flags;
		rw->rw_worker.bgw_start_time = slot->worker.bgw_start_time;
		rw->rw_worker.bgw_restart_time = slot->worker.bgw_restart_time;
		rw->rw_worker.bgw_main_arg = slot->worker.bgw_main_arg;
		memcpy(rw->rw_worker.bgw_extra, slot->worker.bgw_extra, BGW_EXTRALEN);

		/*
		 * Copy the PID to be notified about state changes, but only if the
		 * postmaster knows about a backend with that PID.  It isn't an error
		 * if the postmaster doesn't know about the PID, because the backend
		 * that requested the worker could have died (or been killed) just
		 * after doing so.  Nonetheless, at least until we get some experience
		 * with how this plays out in the wild, log a message at a relative
		 * high debug level.
		 */
		/*
		 * 复制要通知状态更改的 PID，但仅当 postmaster 知道具有该 PID 的后端时。
		 * 如果 postmaster 不知道该 PID，这并不是错误，因为请求工作进程的后端
		 * 可能在请求后立即死亡（或被杀）。尽管如此，至少在我们对这在实际中的运行
		 * 积累一些经验之前，在相对较高的调试级别上记录一条消息。
		 */
		rw->rw_worker.bgw_notify_pid = slot->worker.bgw_notify_pid;
		if (!PostmasterMarkPIDForWorkerNotify(rw->rw_worker.bgw_notify_pid))
		{
			elog(DEBUG1, "worker notification PID %d is not valid",
				 (int) rw->rw_worker.bgw_notify_pid);
			rw->rw_worker.bgw_notify_pid = 0;
		}

		/* Initialize postmaster bookkeeping. */
		/* 初始化 postmaster 簿记。 */
		rw->rw_pid = 0;
		rw->rw_crashed_at = 0;
		rw->rw_shmem_slot = slotno;
		rw->rw_terminate = false;

		/* Log it! */
		/* 记录日志！ */
		ereport(DEBUG1,
				(errmsg_internal("registering background worker \"%s\"",
								 rw->rw_worker.bgw_name)));

		dlist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
	}
}

/*
 * Forget about a background worker that's no longer needed.
 *
 * NOTE: The entry is unlinked from BackgroundWorkerList.  If the caller is
 * iterating through it, better use a mutable iterator!
 *
 * Caller is responsible for notifying bgw_notify_pid, if appropriate.
 *
 * This function must be invoked only in the postmaster.
 */
/*
 * 遗忘不再需要的后台工作进程。
 *
 * 注意：该条目将从 BackgroundWorkerList 中断开。如果调用者正在遍历它，最好使用可变迭代器！
 *
 * 如果适用，调用者负责通知 bgw_notify_pid。
 *
 * 此函数只能在 postmaster 中调用。
 *
 * Function purpose: Unregister and free a background worker.
 * 函数作用：注销并释放一个后台工作进程。
 */
void
ForgetBackgroundWorker(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	Assert(slot->in_use);

	/*
	 * We need a memory barrier here to make sure that the update of
	 * parallel_terminate_count completes before the store to in_use.
	 */
	/*
	 * 我们在这里需要一个内存屏障，以确保 parallel_terminate_count 
	 * 的更新在写入 in_use 之前完成。
	 */
	if ((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
		BackgroundWorkerData->parallel_terminate_count++;

	pg_memory_barrier();
	slot->in_use = false;

	ereport(DEBUG1,
			(errmsg_internal("unregistering background worker \"%s\"",
							 rw->rw_worker.bgw_name)));

	dlist_delete(&rw->rw_lnode);
	pfree(rw);
}

/*
 * Report the PID of a newly-launched background worker in shared memory.
 *
 * This function should only be called from the postmaster.
 */
/*
 * 在共享内存中报告新启动的后台工作进程的 PID。
 *
 * 此函数只能从 postmaster 中调用。
 *
 * Function purpose: Update a background worker's PID in shared memory and notify its parent process.
 * 函数作用：在共享内存中更新后台工作进程的 PID，并通知其父进程。
 */
void
ReportBackgroundWorkerPID(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->pid = rw->rw_pid;

	if (rw->rw_worker.bgw_notify_pid != 0)
		kill(rw->rw_worker.bgw_notify_pid, SIGUSR1);
}

/*
 * Report that the PID of a background worker is now zero because a
 * previously-running background worker has exited.
 *
 * NOTE: The entry may be unlinked from BackgroundWorkerList.  If the caller
 * is iterating through it, better use a mutable iterator!
 *
 * This function should only be called from the postmaster.
 */
/*
 * 报告由于先前运行的后台工作进程已退出，后台工作进程的 PID 现在为零。
 *
 * 注意：该条目可能会从 BackgroundWorkerList 中断开。如果调用者正在遍历它，最好使用可变迭代器！
 *
 * 此函数只能从 postmaster 中调用。
 *
 * Function purpose: Handle backend worker exit, deregistering it if requested or notify its parent.
 * 函数作用：处理后台工作进程的退出，在需要时注销该进程或向父进程发送通知。
 */
void
ReportBackgroundWorkerExit(RegisteredBgWorker *rw)
{
	BackgroundWorkerSlot *slot;
	int			notify_pid;

	Assert(rw->rw_shmem_slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];
	slot->pid = rw->rw_pid;
	notify_pid = rw->rw_worker.bgw_notify_pid;

	/*
	 * If this worker is slated for deregistration, do that before notifying
	 * the process which started it.  Otherwise, if that process tries to
	 * reuse the slot immediately, it might not be available yet.  In theory
	 * that could happen anyway if the process checks slot->pid at just the
	 * wrong moment, but this makes the window narrower.
	 */
	/*
	 * 如果此工作进程预定被注销，请在通知启动它的进程之前执行该操作。
	 * 否则，如果该进程试图立即重用该槽，它可能还不可用。理论上，如果该进程
	 * 在错误的时间检查 slot->pid，无论如何都可能会发生这种情况，但这样可以收窄这个窗口。
	 */
	if (rw->rw_terminate ||
		rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
		ForgetBackgroundWorker(rw);

	if (notify_pid != 0)
		kill(notify_pid, SIGUSR1);
}

/*
 * Cancel SIGUSR1 notifications for a PID belonging to an exiting backend.
 *
 * This function should only be called from the postmaster.
 */
/*
 * 取消属于退出后端的 PID 的 SIGUSR1 通知。
 *
 * 此函数只能从 postmaster 中调用。
 *
 * Function purpose: Stop notifications for background workers registered by a dying process.
 * 函数作用：停止向已退出的进程发送后台工作进程状态变更通知。
 */
void
BackgroundWorkerStopNotifications(pid_t pid)
{
	dlist_iter	iter;

	dlist_foreach(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		if (rw->rw_worker.bgw_notify_pid == pid)
			rw->rw_worker.bgw_notify_pid = 0;
	}
}

/*
 * Cancel any not-yet-started worker requests that have waiting processes.
 *
 * This is called during a normal ("smart" or "fast") database shutdown.
 * After this point, no new background workers will be started, so anything
 * that might be waiting for them needs to be kicked off its wait.  We do
 * that by canceling the bgworker registration entirely, which is perhaps
 * overkill, but since we're shutting down it does not matter whether the
 * registration record sticks around.
 *
 * This function should only be called from the postmaster.
 */
/*
 * 取消任何具有等待进程但尚未启动的工作进程请求。
 *
 * 这在正常的（“智能”或“快速”）数据库关闭期间调用。
 * 在此之后，不会启动新的后台工作进程，因此任何可能正在等待它们的进程都需要终止其等待。
 * 我们通过完全取消后台工作进程注册来做到这一点，这也许有点过头了，但是既然我们正在关闭，
 * 注册记录是否保留已经无关紧要了。
 *
 * 此函数只能从 postmaster 中调用。
 *
 * Function purpose: Clean up and unregister unstarted background workers during database shutdown.
 * 函数作用：在数据库关闭期间，清理并注销尚未启动的后台工作进程。
 */
void
ForgetUnstartedBackgroundWorkers(void)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;
		BackgroundWorkerSlot *slot;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);
		Assert(rw->rw_shmem_slot < max_worker_processes);
		slot = &BackgroundWorkerData->slot[rw->rw_shmem_slot];

		/* If it's not yet started, and there's someone waiting ... */
		/* 如果它还没有启动，并且有人在等待…… */
		if (slot->pid == InvalidPid &&
			rw->rw_worker.bgw_notify_pid != 0)
		{
			/* ... then zap it, and notify the waiter */
			/* ……那么清除它，并通知等待者 */
			int			notify_pid = rw->rw_worker.bgw_notify_pid;

			ForgetBackgroundWorker(rw);
			if (notify_pid != 0)
				kill(notify_pid, SIGUSR1);
		}
	}
}

/*
 * Reset background worker crash state.
 *
 * We assume that, after a crash-and-restart cycle, background workers without
 * the never-restart flag should be restarted immediately, instead of waiting
 * for bgw_restart_time to elapse.  On the other hand, workers with that flag
 * should be forgotten immediately, since we won't ever restart them.
 *
 * This function should only be called from the postmaster.
 */
/*
 * 重置后台工作进程崩溃状态。
 *
 * 我们假设，在崩溃重启周期之后，没有 never-restart 标志的后台工作进程应该立即重启，
 * 而不是等待 bgw_restart_time 流逝。另一方面，具有该标志的工作进程应该立即被遗忘，
 * 因为我们永远不会重启它们。
 *
 * 此函数只能从 postmaster 中调用。
 *
 * Function purpose: Reset crash timers/restart flags for workers after a system crash.
 * 函数作用：在系统崩溃重启后，重置后台工作进程的崩溃状态与重启策略。
 */
void
ResetBackgroundWorkerCrashTimes(void)
{
	dlist_mutable_iter iter;

	dlist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = dlist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
		{
			/*
			 * Workers marked BGW_NEVER_RESTART shouldn't get relaunched after
			 * the crash, so forget about them.  (If we wait until after the
			 * crash to forget about them, and they are parallel workers,
			 * parallel_terminate_count will get incremented after we've
			 * already zeroed parallel_register_count, which would be bad.)
			 */
			/*
			 * 标记为 BGW_NEVER_RESTART 的工作进程在崩溃后不应该重新启动，
			 * 因此遗忘它们。（如果我们等到崩溃后才遗忘它们，并且它们是并行
			 * 工作进程，在我们已经将 parallel_register_count 清零之后，
			 * parallel_terminate_count 会被递增，这会很糟。）
			 */
			ForgetBackgroundWorker(rw);
		}
		else
		{
			/*
			 * The accounting which we do via parallel_register_count and
			 * parallel_terminate_count would get messed up if a worker marked
			 * parallel could survive a crash and restart cycle. All such
			 * workers should be marked BGW_NEVER_RESTART, and thus control
			 * should never reach this branch.
			 */
			/*
			 * 如果标记为并行的工作进程能在崩溃和重启周期中存活下来，我们通过
			 * parallel_register_count 和 parallel_terminate_count 进行的会计
			 * 工作将会被搞乱。所有此类工作进程都应该被标记为 BGW_NEVER_RESTART，
			 * 因此控制流永远不应该到达此分支。
			 */
			Assert((rw->rw_worker.bgw_flags & BGWORKER_CLASS_PARALLEL) == 0);

			/*
			 * Allow this worker to be restarted immediately after we finish
			 * resetting.
			 */
			/*
			 * 允许此工作进程在我们完成重置后立即重新启动。
			 */
			rw->rw_crashed_at = 0;
			rw->rw_pid = 0;

			/*
			 * If there was anyone waiting for it, they're history.
			 */
			/*
			 * 如果有任何人在等待它，他们都成为历史了（取消通知）。
			 */
			rw->rw_worker.bgw_notify_pid = 0;
		}
	}
}

/*
 * Complain about the BackgroundWorker definition using error level elevel.
 * Return true if it looks ok, false if not (unless elevel >= ERROR, in
 * which case we won't return at all in the not-OK case).
 */
/*
 * 使用错误级别 elevel 对 BackgroundWorker 定义提出投诉。
 * 如果看起来没问题则返回 true，否则返回 false（除非 elevel >= ERROR，
 * 在这种情况下，在不正常的情况下我们根本不会返回）。
 *
 * Function purpose: Verify sanity of a background worker definition.
 * 函数作用：对后台工作进程的定义参数进行合法性检查。
 */
static bool
SanityCheckBackgroundWorker(BackgroundWorker *worker, int elevel)
{
	/* sanity check for flags */
	/* 标志的健壮性检查 */

	/*
	 * We used to support workers not connected to shared memory, but don't
	 * anymore. Thus this is a required flag now. We're not removing the flag
	 * for compatibility reasons and because the flag still provides some
	 * signal when reading code.
	 */
	/*
	 * 我们以前支持未连接到共享内存的工作进程，但现在不再支持。
	 * 因此这现在是一个必需的标志。由于兼容性原因以及因为该标志在阅读
	 * 代码时仍提供一些信号，我们没有删除该标志。
	 */
	if (!(worker->bgw_flags & BGWORKER_SHMEM_ACCESS))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": background workers without shared memory access are not supported",
						worker->bgw_name)));
		return false;
	}

	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		if (worker->bgw_start_time == BgWorkerStart_PostmasterStart)
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("background worker \"%s\": cannot request database access if starting at postmaster start",
							worker->bgw_name)));
			return false;
		}

		/* XXX other checks? */
		/* XXX 其他检查？ */
	}

	if ((worker->bgw_restart_time < 0 &&
		 worker->bgw_restart_time != BGW_NEVER_RESTART) ||
		(worker->bgw_restart_time > USECS_PER_DAY / 1000))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": invalid restart interval",
						worker->bgw_name)));
		return false;
	}

	/*
	 * Parallel workers may not be configured for restart, because the
	 * parallel_register_count/parallel_terminate_count accounting can't
	 * handle parallel workers lasting through a crash-and-restart cycle.
	 */
	/*
	 * 并行工作进程不能配置为重启，因为 parallel_register_count/parallel_terminate_count 
	 * 的会计机制无法处理在崩溃和重启周期中持久存在的并行工作进程。
	 */
	if (worker->bgw_restart_time != BGW_NEVER_RESTART &&
		(worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background worker \"%s\": parallel workers may not be configured for restart",
						worker->bgw_name)));
		return false;
	}

	/*
	 * If bgw_type is not filled in, use bgw_name.
	 */
	/*
	 * 如果没有填充 bgw_type，则使用 bgw_name。
	 */
	if (strcmp(worker->bgw_type, "") == 0)
		strcpy(worker->bgw_type, worker->bgw_name);

	return true;
}

/*
 * Standard SIGTERM handler for background workers
 */
/*
 * 后台工作进程的标准 SIGTERM 处理程序
 *
 * Function purpose: Handle SIGTERM exit signal for background workers.
 * 函数作用：处理后台工作进程的 SIGTERM 终止信号。
 */
static void
bgworker_die(SIGNAL_ARGS)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	ereport(FATAL,
			(errcode(ERRCODE_ADMIN_SHUTDOWN),
			 errmsg("terminating background worker \"%s\" due to administrator command",
					MyBgworkerEntry->bgw_type)));
}

/*
 * Main entry point for background worker processes.
 */
/*
 * 后台工作进程的主入口点。
 *
 * Function purpose: Execute the main flow of a background worker.
 * 函数作用：执行后台工作进程的主要生命周期流程。
 *
 * Core workflow:
 * 核心流程：
 * 1. Load background worker structure, delete PostmasterContext.
 *    加载工作进程结构，释放 PostmasterContext。
 * 2. Setup ps title and apply PostAuthDelay.
 *    设置进程的 ps 显示，并应用可能的授权延迟。
 * 3. Setup signal handlers (SIGINT, SIGTERM, SIGUSR1, etc.).
 *    配置信号处理程序（SIGINT, SIGTERM, SIGUSR1 等）。
 * 4. Register exception recovery handler via sigsetjmp.
 *    注册异常恢复处理程序。
 * 5. Call InitProcess() to setup PGPROC, BaseInit() for backend init.
 *    调用 InitProcess() 绑定 PGPROC 共享结构，调用 BaseInit() 初始化系统组件。
 * 6. Lookup entry point function via LookupBackgroundWorkerFunction and call it.
 *    通过 LookupBackgroundWorkerFunction 找到入口函数地址并调用之。
 * 7. Call proc_exit(0) upon return.
 *    正常返回后调用 proc_exit(0) 退出。
 */
void
BackgroundWorkerMain(const void *startup_data, size_t startup_data_len)
{
	sigjmp_buf	local_sigjmp_buf;
	BackgroundWorker *worker;
	bgworker_main_type entrypt;

	if (startup_data == NULL)
		elog(FATAL, "unable to find bgworker entry");
	Assert(startup_data_len == sizeof(BackgroundWorker));
	worker = MemoryContextAlloc(TopMemoryContext, sizeof(BackgroundWorker));
	memcpy(worker, startup_data, sizeof(BackgroundWorker));

	/*
	 * Now that we're done reading the startup data, release postmaster's
	 * working memory context.
	 */
	/*
	 * 既然我们已经读完了启动数据，请释放 postmaster 的工作内存上下文。
	 */
	if (PostmasterContext)
	{
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;
	}

	MyBgworkerEntry = worker;
	MyBackendType = B_BG_WORKER;
	init_ps_display(worker->bgw_name);

	Assert(GetProcessingMode() == InitProcessing);

	/* Apply PostAuthDelay */
	/* 应用 PostAuthDelay 延迟 */
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * Set up signal handlers.
	 */
	/*
	 * 设置信号处理程序。
	 */
	if (worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION)
	{
		/*
		 * SIGINT is used to signal canceling the current action
		 */
		/*
		 * SIGINT 用于发出取消当前动作的信号
		 */
		pqsignal(SIGINT, StatementCancelHandler);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/* XXX Any other handlers needed here? */
		/* XXX 这里还需要其他处理程序吗？ */
	}
	else
	{
		pqsignal(SIGINT, SIG_IGN);
		pqsignal(SIGUSR1, SIG_IGN);
		pqsignal(SIGFPE, SIG_IGN);
	}
	pqsignal(SIGTERM, bgworker_die);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	/* SIGQUIT 处理程序已由 InitPostmasterChild 设置 */
	pqsignal(SIGHUP, SIG_IGN);

	InitializeTimeouts();		/* establishes SIGALRM handler */ /* 建立 SIGALRM 处理程序 */

	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR2, SIG_IGN);
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * We just need to clean up, report the error, and go away.
	 */
	/*
	 * 如果遇到异常，处理将在此处恢复。
	 *
	 * 我们只需要清理，报告错误，然后离开。
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		/* 由于没有使用 PG_TRY，必须手动重置错误栈 */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		/* 在清理期间阻止中断 */
		HOLD_INTERRUPTS();

		/*
		 * sigsetjmp will have blocked all signals, but we may need to accept
		 * signals while communicating with our parallel leader.  Once we've
		 * done HOLD_INTERRUPTS() it should be safe to unblock signals.
		 */
		/*
		 * sigsetjmp 将阻塞所有信号，但在与我们的并行主控进程通信时，
		 * 我们可能需要接受信号。一旦我们执行了 HOLD_INTERRUPTS()，
		 * 解除信号阻塞应该是安全的。
		 */
		BackgroundWorkerUnblockSignals();

		/* Report the error to the parallel leader and the server log */
		/* 将错误报告给并行主控进程和服务器日志 */
		EmitErrorReport();

		/*
		 * Do we need more cleanup here?  For shmem-connected bgworkers, we
		 * will call InitProcess below, which will install ProcKill as exit
		 * callback.  That will take care of releasing locks, etc.
		 */
		/*
		 * 我们在这里需要更多的清理工作吗？对于连接共享内存的后台工作进程，我们
		 * 将在下面调用 InitProcess，它将安装 ProcKill 作为退出回调。这
		 * 将负责释放锁等。
		 */

		/* and go away */
		/* 然后离开 */
		proc_exit(1);
	}

	/* We can now handle ereport(ERROR) */
	/* 我们现在可以处理 ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Create a per-backend PGPROC struct in shared memory.  We must do this
	 * before we can use LWLocks or access any shared memory.
	 */
	/*
	 * 在共享内存中创建一个对应每个后台的 PGPROC 结构。我们必须在能够使用 LWLocks
	 * 或访问任何共享内存之前做这步。
	 */
	InitProcess();

	/*
	 * Early initialization.
	 */
	/*
	 * 早期初始化。
	 */
	BaseInit();

	/*
	 * Look up the entry point function, loading its library if necessary.
	 */
	/*
	 * 查找入口点函数，必要时加载其库。
	 */
	entrypt = LookupBackgroundWorkerFunction(worker->bgw_library_name,
											 worker->bgw_function_name);

	/*
	 * Note that in normal processes, we would call InitPostgres here.  For a
	 * worker, however, we don't know what database to connect to, yet; so we
	 * need to wait until the user code does it via
	 * BackgroundWorkerInitializeConnection().
	 */
	/*
	 * 请注意，在正常的进程中，我们将在这里调用 InitPostgres。然而，对于
	 * 工作进程，我们还不知道要连接到哪个数据库；因此，我们需要等到用户代码
	 * 通过 BackgroundWorkerInitializeConnection() 进行连接。
	 */

	/*
	 * Now invoke the user-defined worker code
	 */
	/*
	 * 现在调用用户定义的工作进程代码
	 */
	entrypt(worker->bgw_main_arg);

	/* ... and if it returns, we're done */
	/* ……如果它返回，我们就完成了 */
	proc_exit(0);
}

/*
 * Connect background worker to a database.
 */
/*
 * 将后台工作进程连接到数据库。
 *
 * Function purpose: Connect the background worker to the specified database by name.
 * 函数作用：通过数据库名和用户名建立后台工作进程的数据库连接。
 */
void
BackgroundWorkerInitializeConnection(const char *dbname, const char *username, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;
	bits32		init_flags = 0; /* never honor session_preload_libraries */
								/* 永远不理会 session_preload_libraries */

	/* ignore datallowconn and ACL_CONNECT? */
	/* 忽略 datallowconn 和 ACL_CONNECT？ */
	if (flags & BGWORKER_BYPASS_ALLOWCONN)
		init_flags |= INIT_PG_OVERRIDE_ALLOW_CONNS;
	/* ignore rolcanlogin? */
	/* 忽略 rolcanlogin？ */
	if (flags & BGWORKER_BYPASS_ROLELOGINCHECK)
		init_flags |= INIT_PG_OVERRIDE_ROLE_LOGIN;

	/* XXX is this the right errcode? */
	/* XXX 这是正确的错误码吗？ */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(dbname, InvalidOid,	/* database to connect to */ /* 要连接的数据库 */
				 username, InvalidOid,	/* role to connect as */ /* 要连接的角色 */
				 init_flags,
				 NULL);			/* no out_dbname */

	/* it had better not gotten out of "init" mode yet */
	/* 它最好还没有离开 “init” 模式 */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * Connect background worker to a database using OIDs.
 */
/*
 * 使用 OID 将后台工作进程连接到数据库。
 *
 * Function purpose: Connect the background worker to the database specified by OID.
 * 函数作用：通过数据库 OID 和角色 OID 建立后台工作进程的数据库连接。
 */
void
BackgroundWorkerInitializeConnectionByOid(Oid dboid, Oid useroid, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;
	bits32		init_flags = 0; /* never honor session_preload_libraries */

	/* ignore datallowconn and ACL_CONNECT? */
	/* 忽略 datallowconn 和 ACL_CONNECT？ */
	if (flags & BGWORKER_BYPASS_ALLOWCONN)
		init_flags |= INIT_PG_OVERRIDE_ALLOW_CONNS;
	/* ignore rolcanlogin? */
	/* 忽略 rolcanlogin？ */
	if (flags & BGWORKER_BYPASS_ROLELOGINCHECK)
		init_flags |= INIT_PG_OVERRIDE_ROLE_LOGIN;

	/* XXX is this the right errcode? */
	/* XXX 这是正确的错误码吗？ */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(NULL, dboid,	/* database to connect to */
				 NULL, useroid, /* role to connect as */
				 init_flags,
				 NULL);			/* no out_dbname */

	/* it had better not gotten out of "init" mode yet */
	/* 它最好还没有离开 “init” 模式 */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * Block/unblock signals in a background worker
 */
/*
 * 在后台工作进程中阻塞/解除阻塞信号
 *
 * Function purpose: Block signals in background worker.
 * 函数作用：在后台工作进程中阻塞信号。
 */
void
BackgroundWorkerBlockSignals(void)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);
}

/*
 * Function purpose: Unblock signals in background worker.
 * 函数作用：在后台工作进程中解除阻塞信号。
 */
void
BackgroundWorkerUnblockSignals(void)
{
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
}

/*
 * Register a new static background worker.
 *
 * This can only be called directly from postmaster or in the _PG_init
 * function of a module library that's loaded by shared_preload_libraries;
 * otherwise it will have no effect.
 */
/*
 * 注册一个新的静态后台工作进程。
 *
 * 此函数只能由 postmaster 直接调用，或者在由 shared_preload_libraries 
 * 加载的模块库的 _PG_init 函数中调用；否则它将不起作用。
 *
 * Function purpose: Register a static background worker before shmem init.
 * 函数作用：在共享内存初始化前注册一个静态后台工作进程。
 */
void
RegisterBackgroundWorker(BackgroundWorker *worker)
{
	RegisteredBgWorker *rw;
	static int	numworkers = 0;

	/*
	 * Static background workers can only be registered in the postmaster
	 * process.
	 */
	/*
	 * 静态后台工作进程只能在 postmaster 进程中注册。
	 */
	if (IsUnderPostmaster || !IsPostmasterEnvironment)
	{
		/*
		 * In EXEC_BACKEND or single-user mode, we process
		 * shared_preload_libraries in backend processes too.  We cannot
		 * register static background workers at that stage, but many
		 * libraries' _PG_init() functions don't distinguish whether they're
		 * being loaded in the postmaster or in a backend, they just check
		 * process_shared_preload_libraries_in_progress.  It's a bit sloppy,
		 * but for historical reasons we tolerate it.  In EXEC_BACKEND mode,
		 * the background workers should already have been registered when the
		 * library was loaded in postmaster.
		 */
		/*
		 * 在 EXEC_BACKEND 或单用户模式下，我们也在后端进程中处理
		 * shared_preload_libraries。在该阶段我们无法注册静态后台
		 * 工作进程，但许多库的 _PG_init() 函数并不区分它们是在 
		 * postmaster 还是在后端中被加载，它们只检查 
		 * process_shared_preload_libraries_in_progress。这有点粗糙，
		 * 但由于历史原因，我们容忍了它。在 EXEC_BACKEND 模式下，当在 
		 * postmaster 中加载库时，后台工作进程应该已经被注册了。
		 */
		if (process_shared_preload_libraries_in_progress)
			return;
		ereport(LOG,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background worker \"%s\": must be registered in \"shared_preload_libraries\"",
						worker->bgw_name)));
		return;
	}

	/*
	 * Cannot register static background workers after calling
	 * BackgroundWorkerShmemInit().
	 */
	/*
	 * 调用 BackgroundWorkerShmemInit() 之后不能再注册静态后台工作进程。
	 */
	if (BackgroundWorkerData != NULL)
		elog(ERROR, "cannot register background worker \"%s\" after shmem init",
			 worker->bgw_name);

	ereport(DEBUG1,
			(errmsg_internal("registering background worker \"%s\"", worker->bgw_name)));

	if (!SanityCheckBackgroundWorker(worker, LOG))
		return;

	if (worker->bgw_notify_pid != 0)
	{
		ereport(LOG,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background worker \"%s\": only dynamic background workers can request notification",
						worker->bgw_name)));
		return;
	}

	/*
	 * Enforce maximum number of workers.  Note this is overly restrictive: we
	 * could allow more non-shmem-connected workers, because these don't count
	 * towards the MAX_BACKENDS limit elsewhere.  For now, it doesn't seem
	 * important to relax this restriction.
	 */
	/*
	 * 强制执行最大工作进程数。注意这过于严格：我们可以允许更多未连接到共享内存
	 * 的工作进程，因为这些不计入其他地方的 MAX_BACKENDS 限制。目前，放宽此限制
	 * 似乎并不重要。
	 */
	if (++numworkers > max_worker_processes)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("too many background workers"),
				 errdetail_plural("Up to %d background worker can be registered with the current settings.",
								  "Up to %d background workers can be registered with the current settings.",
								  max_worker_processes,
								  max_worker_processes),
				 errhint("Consider increasing the configuration parameter \"%s\".", "max_worker_processes")));
		return;
	}

	/*
	 * Copy the registration data into the registered workers list.
	 */
	/*
	 * 将注册数据复制到已注册的工作进程列表中。
	 */
	rw = MemoryContextAllocExtended(PostmasterContext,
									sizeof(RegisteredBgWorker),
									MCXT_ALLOC_NO_OOM);
	if (rw == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return;
	}

	rw->rw_worker = *worker;
	rw->rw_pid = 0;
	rw->rw_crashed_at = 0;
	rw->rw_terminate = false;

	dlist_push_head(&BackgroundWorkerList, &rw->rw_lnode);
}

/*
 * Register a new background worker from a regular backend.
 *
 * Returns true on success and false on failure.  Failure typically indicates
 * that no background worker slots are currently available.
 *
 * If handle != NULL, we'll set *handle to a pointer that can subsequently
 * be used as an argument to GetBackgroundWorkerPid().  The caller can
 * free this pointer using pfree(), if desired.
 */
/*
 * 从普通后端注册一个新的后台工作进程。
 *
 * 成功返回 true，失败返回 false。失败通常表示当前没有可用的后台工作进程槽。
 *
 * 如果 handle != NULL，我们将 *handle 设置为一个指针，该指针随后可用作 
 * GetBackgroundWorkerPid() 的参数。如果需要，调用者可以使用 pfree() 释放此指针。
 *
 * Function purpose: Dynamically register a background worker from a running backend.
 * 函数作用：允许运行中的后台连接动态注册一个新的后台工作进程。
 *
 * Core workflow:
 * 核心流程：
 * 1. Validate permissions and sanity check parameters.
 *    校验环境及参数合法性。
 * 2. Acquire BackgroundWorkerLock in exclusive mode.
 *    以排他模式获取 BackgroundWorkerLock 锁。
 * 3. Verify total number of parallel workers does not exceed max_parallel_workers.
 *    若为并行子进程，验证当前并行数是否超过限制。
 * 4. Scan for an unused BackgroundWorkerSlot, copy metadata, set state to in_use, release lock.
 *    寻找未使用的槽并写入元数据，将其标记为 in_use 并释放锁。
 * 5. Signal postmaster via PMSIGNAL_BACKGROUND_WORKER_CHANGE to process slot.
 *    通过发送 PMSIGNAL_BACKGROUND_WORKER_CHANGE 信号通知 postmaster 处理状态变更。
 */
bool
RegisterDynamicBackgroundWorker(BackgroundWorker *worker,
								BackgroundWorkerHandle **handle)
{
	int			slotno;
	bool		success = false;
	bool		parallel;
	uint64		generation = 0;

	/*
	 * We can't register dynamic background workers from the postmaster. If
	 * this is a standalone backend, we're the only process and can't start
	 * any more.  In a multi-process environment, it might be theoretically
	 * possible, but we don't currently support it due to locking
	 * considerations; see comments on the BackgroundWorkerSlot data
	 * structure.
	 */
	/*
	 * 我们不能从 postmaster 注册动态后台工作进程。如果是独立的后端，我们是唯一
	 * 的进程，无法再启动任何进程。在多进程环境中，理论上这可能是可行的，但由于
	 * 锁的考虑，我们目前不支持它；请参阅 BackgroundWorkerSlot 数据结构的注释。
	 */
	if (!IsUnderPostmaster)
		return false;

	if (!SanityCheckBackgroundWorker(worker, ERROR))
		return false;

	parallel = (worker->bgw_flags & BGWORKER_CLASS_PARALLEL) != 0;

	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);

	/*
	 * If this is a parallel worker, check whether there are already too many
	 * parallel workers; if so, don't register another one.  Our view of
	 * parallel_terminate_count may be slightly stale, but that doesn't really
	 * matter: we would have gotten the same result if we'd arrived here
	 * slightly earlier anyway.  There's no help for it, either, since the
	 * postmaster must not take locks; a memory barrier wouldn't guarantee
	 * anything useful.
	 */
	/*
	 * 如果这是并行工作进程，请检查是否已经有太多的并行工作进程；如果是这样，
	 * 不要注册另一个。我们看到的 parallel_terminate_count 可能稍微过时，但这
	 * 并没有什么关系：即使我们稍微早一点到达这里，我们也会得到相同的结果。
	 * 这也是没有办法的，因为 postmaster 不能获取锁；内存屏障无法保证任何有用的东西。
	 */
	if (parallel && (BackgroundWorkerData->parallel_register_count -
					 BackgroundWorkerData->parallel_terminate_count) >=
		max_parallel_workers)
	{
		Assert(BackgroundWorkerData->parallel_register_count -
			   BackgroundWorkerData->parallel_terminate_count <=
			   MAX_PARALLEL_WORKER_LIMIT);
		LWLockRelease(BackgroundWorkerLock);
		return false;
	}

	/*
	 * Look for an unused slot.  If we find one, grab it.
	 */
	/*
	 * 寻找一个未使用的槽。如果我们找到了，就占有它。
	 */
	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; ++slotno)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (!slot->in_use)
		{
			memcpy(&slot->worker, worker, sizeof(BackgroundWorker));
			slot->pid = InvalidPid; /* indicates not started yet */
			slot->generation++;
			slot->terminate = false;
			generation = slot->generation;
			if (parallel)
				BackgroundWorkerData->parallel_register_count++;

			/*
			 * Make sure postmaster doesn't see the slot as in use before it
			 * sees the new contents.
			 */
			/*
			 * 确保 postmaster 在看到新内容之前，不会将槽视为已在使用中。
			 */
			pg_write_barrier();

			slot->in_use = true;
			success = true;
			break;
		}
	}

	LWLockRelease(BackgroundWorkerLock);

	/* If we found a slot, tell the postmaster to notice the change. */
	/* 如果我们找到了一个槽，通知 postmaster 注意这一更改。 */
	if (success)
		SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);

	/*
	 * If we found a slot and the user has provided a handle, initialize it.
	 */
	/*
	 * 如果我们找到了一个槽，并且用户提供了一个句柄，请对其进行初始化。
	 */
	if (success && handle)
	{
		*handle = palloc(sizeof(BackgroundWorkerHandle));
		(*handle)->slot = slotno;
		(*handle)->generation = generation;
	}

	return success;
}

/*
 * Get the PID of a dynamically-registered background worker.
 *
 * If the worker is determined to be running, the return value will be
 * BGWH_STARTED and *pidp will get the PID of the worker process.  If the
 * postmaster has not yet attempted to start the worker, the return value will
 * be BGWH_NOT_YET_STARTED.  Otherwise, the return value is BGWH_STOPPED.
 *
 * BGWH_STOPPED can indicate either that the worker is temporarily stopped
 * (because it is configured for automatic restart and exited non-zero),
 * or that the worker is permanently stopped (because it exited with exit
 * code 0, or was not configured for automatic restart), or even that the
 * worker was unregistered without ever starting (either because startup
 * failed and the worker is not configured for automatic restart, or because
 * TerminateBackgroundWorker was used before the worker was successfully
 * started).
 */
/*
 * 获取动态注册的后台工作进程的 PID。
 *
 * 如果确定工作进程正在运行，返回值将是 BGWH_STARTED，并且 *pidp 将获取工作进程
 * 的 PID。如果 postmaster 尚未尝试启动工作进程，返回值将是 BGWH_NOT_YET_STARTED。
 * 否则，返回值是 BGWH_STOPPED。
 *
 * BGWH_STOPPED 可以表示工作进程临时停止（因为配置为自动重启并且以非零值退出），
 * 或者工作进程永久停止（因为它以退出码 0 退出，或者没有配置为自动重启），甚至
 * 表示工作进程在从未启动的情况下被注销（要么是因为启动失败且未配置为自动重启，
 * 要么是因为在工作进程成功启动之前使用了 TerminateBackgroundWorker）。
 *
 * Function purpose: Check status and retrieve PID of a dynamic background worker.
 * 函数作用：查询并获取动态后台工作进程的状态和 PID。
 */
BgwHandleStatus
GetBackgroundWorkerPid(BackgroundWorkerHandle *handle, pid_t *pidp)
{
	BackgroundWorkerSlot *slot;
	pid_t		pid;

	Assert(handle->slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[handle->slot];

	/*
	 * We could probably arrange to synchronize access to data using memory
	 * barriers only, but for now, let's just keep it simple and grab the
	 * lock.  It seems unlikely that there will be enough traffic here to
	 * result in meaningful contention.
	 */
	/*
	 * 我们可能可以只使用内存屏障来协调对数据的访问，但目前，让我们保持简单
	 * 并获取锁。此处似乎不太可能有足够的流量导致有意义的争用。
	 */
	LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

	/*
	 * The generation number can't be concurrently changed while we hold the
	 * lock.  The pid, which is updated by the postmaster, can change at any
	 * time, but we assume such changes are atomic.  So the value we read
	 * won't be garbage, but it might be out of date by the time the caller
	 * examines it (but that's unavoidable anyway).
	 *
	 * The in_use flag could be in the process of changing from true to false,
	 * but if it is already false then it can't change further.
	 */
	/*
	 * 当我们持有锁时，代号（generation number）不能并发更改。由 postmaster 
	 * 更新的 pid 可以随时更改，但我们假设此类更改是原子的。因此我们读取的值不会
	 * 是垃圾，但在调用者检查它时可能已经过期（但无论如何这都是不可避免的）。
	 *
	 * in_use 标志可能正在从 true 变为 false，但如果它已经为 false，则不能进一步更改。
	 */
	if (handle->generation != slot->generation || !slot->in_use)
		pid = 0;
	else
		pid = slot->pid;

	/* All done. */
	/* 全部完成。 */
	LWLockRelease(BackgroundWorkerLock);

	if (pid == 0)
		return BGWH_STOPPED;
	else if (pid == InvalidPid)
		return BGWH_NOT_YET_STARTED;
	*pidp = pid;
	return BGWH_STARTED;
}

/*
 * Wait for a background worker to start up.
 *
 * This is like GetBackgroundWorkerPid(), except that if the worker has not
 * yet started, we wait for it to do so; thus, BGWH_NOT_YET_STARTED is never
 * returned.  However, if the postmaster has died, we give up and return
 * BGWH_POSTMASTER_DIED, since it that case we know that startup will not
 * take place.
 *
 * The caller *must* have set our PID as the worker's bgw_notify_pid,
 * else we will not be awoken promptly when the worker's state changes.
 */
/*
 * 等待后台工作进程启动。
 *
 * 这类似于 GetBackgroundWorkerPid()，不同之处在于如果工作进程尚未启动，
 * 我们等待它启动；因此，永远不会返回 BGWH_NOT_YET_STARTED。然而，如果 
 * postmaster 已经死亡，我们放弃并返回 BGWH_POSTMASTER_DIED，因为在这种情况下 
 * 我们知道启动不会发生。
 *
 * 调用者 *必须* 已将我们的 PID 设置为工作进程的 bgw_notify_pid，
 * 否则当工作进程状态发生变化时我们不会被及时唤醒。
 *
 * Function purpose: Block until the background worker starts running or postmaster dies.
 * 函数作用：阻塞等待后台工作进程启动，或直至 postmaster 死亡。
 */
BgwHandleStatus
WaitForBackgroundWorkerStartup(BackgroundWorkerHandle *handle, pid_t *pidp)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STARTED)
			*pidp = pid;
		if (status != BGWH_NOT_YET_STARTED)
			break;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0,
					   WAIT_EVENT_BGWORKER_STARTUP);

		if (rc & WL_POSTMASTER_DEATH)
		{
			status = BGWH_POSTMASTER_DIED;
			break;
		}

		ResetLatch(MyLatch);
	}

	return status;
}

/*
 * Wait for a background worker to stop.
 *
 * If the worker hasn't yet started, or is running, we wait for it to stop
 * and then return BGWH_STOPPED.  However, if the postmaster has died, we give
 * up and return BGWH_POSTMASTER_DIED, because it's the postmaster that
 * notifies us when a worker's state changes.
 *
 * The caller *must* have set our PID as the worker's bgw_notify_pid,
 * else we will not be awoken promptly when the worker's state changes.
 */
/*
 * 等待后台工作进程停止。
 *
 * 如果工作进程尚未启动，或正在运行，我们等待它停止然后返回 BGWH_STOPPED。
 * 然而，如果 postmaster 已经死亡，我们放弃并返回 BGWH_POSTMASTER_DIED，
 * 因为是在工作进程状态变化时由 postmaster 通知我们的。
 *
 * 调用者 *必须* 已将我们的 PID 设置为工作进程的 bgw_notify_pid，
 * 否则当工作进程状态发生变化时我们不会被及时唤醒。
 *
 * Function purpose: Block until the background worker shuts down or postmaster dies.
 * 函数作用：阻塞等待后台工作进程退出，或直至 postmaster 死亡。
 */
BgwHandleStatus
WaitForBackgroundWorkerShutdown(BackgroundWorkerHandle *handle)
{
	BgwHandleStatus status;
	int			rc;

	for (;;)
	{
		pid_t		pid;

		CHECK_FOR_INTERRUPTS();

		status = GetBackgroundWorkerPid(handle, &pid);
		if (status == BGWH_STOPPED)
			break;

		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_POSTMASTER_DEATH, 0,
					   WAIT_EVENT_BGWORKER_SHUTDOWN);

		if (rc & WL_POSTMASTER_DEATH)
		{
			status = BGWH_POSTMASTER_DIED;
			break;
		}

		ResetLatch(MyLatch);
	}

	return status;
}

/*
 * Instruct the postmaster to terminate a background worker.
 *
 * Note that it's safe to do this without regard to whether the worker is
 * still running, or even if the worker may already have exited and been
 * unregistered.
 */
/*
 * 指示 postmaster 终止后台工作进程。
 *
 * 请注意，安全执行此操作无需考虑工作进程是否仍在运行，甚至无需考虑工作进程
 * 是否可能已经退出并已被注销。
 *
 * Function purpose: Request postmaster to terminate a background worker.
 * 函数作用：发送信号以请求 postmaster 终止指定的后台工作进程。
 */
void
TerminateBackgroundWorker(BackgroundWorkerHandle *handle)
{
	BackgroundWorkerSlot *slot;
	bool		signal_postmaster = false;

	Assert(handle->slot < max_worker_processes);
	slot = &BackgroundWorkerData->slot[handle->slot];

	/* Set terminate flag in shared memory, unless slot has been reused. */
	/* 在共享内存中设置 terminate 标志，除非槽已被重用。 */
	LWLockAcquire(BackgroundWorkerLock, LW_EXCLUSIVE);
	if (handle->generation == slot->generation)
	{
		slot->terminate = true;
		signal_postmaster = true;
	}
	LWLockRelease(BackgroundWorkerLock);

	/* Make sure the postmaster notices the change to shared memory. */
	/* 确保 postmaster 注意到共享内存的更改。 */
	if (signal_postmaster)
		SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE);
}

/*
 * Look up (and possibly load) a bgworker entry point function.
 *
 * For functions contained in the core code, we use library name "postgres"
 * and consult the InternalBGWorkers array.  External functions are
 * looked up, and loaded if necessary, using load_external_function().
 *
 * The point of this is to pass function names as strings across process
 * boundaries.  We can't pass actual function addresses because of the
 * possibility that the function has been loaded at a different address
 * in a different process.  This is obviously a hazard for functions in
 * loadable libraries, but it can happen even for functions in the core code
 * on platforms using EXEC_BACKEND (e.g., Windows).
 *
 * At some point it might be worthwhile to get rid of InternalBGWorkers[]
 * in favor of applying load_external_function() for core functions too;
 * but that raises portability issues that are not worth addressing now.
 */
/*
 * 查找（并可能加载）后台工作进程入口点函数。
 *
 * 对于核心代码中包含的函数，我们使用库名称 “postgres” 并咨询 InternalBGWorkers 
 * 数组。外部函数使用 load_external_function() 进行查找，必要时进行加载。
 *
 * 这样做的目的是跨进程边界将函数名称作为字符串传递。我们不能传递实际的函数地址，
 * 因为该函数在不同进程中可能加载在不同的地址。对于可加载库中的函数，这显然是
 * 一个危害，但在使用 EXEC_BACKEND 的平台（例如 Windows）上，即使对于核心代码中
 * 的函数，它也可能会发生。
 *
 * 在某种程度上，为了对核心函数也应用 load_external_function()，摆脱 InternalBGWorkers[] 
 * 可能是值得的；但那会带来移植性问题，现在不值得解决。
 *
 * Function purpose: Lookup function address by library and function name.
 * 函数作用：通过动态库名称和函数名称查找到对应的函数指针地址。
 */
static bgworker_main_type
LookupBackgroundWorkerFunction(const char *libraryname, const char *funcname)
{
	/*
	 * If the function is to be loaded from postgres itself, search the
	 * InternalBGWorkers array.
	 */
	/*
	 * 如果要从 postgres 本身加载函数，请搜索 InternalBGWorkers 数组。
	 */
	if (strcmp(libraryname, "postgres") == 0)
	{
		int			i;

		for (i = 0; i < lengthof(InternalBGWorkers); i++)
		{
			if (strcmp(InternalBGWorkers[i].fn_name, funcname) == 0)
				return InternalBGWorkers[i].fn_addr;
		}

		/* We can only reach this by programming error. */
		/* 我们只能通过编程错误来到达这里。 */
		elog(ERROR, "internal function \"%s\" not found", funcname);
	}

	/* Otherwise load from external library. */
	/* 否则从外部库加载。 */
	return (bgworker_main_type)
		load_external_function(libraryname, funcname, true, NULL);
}

/*
 * Given a PID, get the bgw_type of the background worker.  Returns NULL if
 * not a valid background worker.
 *
 * The return value is in static memory belonging to this function, so it has
 * to be used before calling this function again.  This is so that the caller
 * doesn't have to worry about the background worker locking protocol.
 */
/*
 * 给定 PID，获取后台工作进程的 bgw_type。如果不是有效的后台工作进程，则返回 NULL。
 *
 * 返回值在此函数所属的静态内存中，因此必须在再次调用此函数之前使用。
 * 这样调用者就不用担心后台工作进程的锁协议。
 *
 * Function purpose: Lookup background worker type by PID.
 * 函数作用：通过 PID 获取对应的后台工作进程类型名称。
 */
const char *
GetBackgroundWorkerTypeByPid(pid_t pid)
{
	int			slotno;
	bool		found = false;
	static char result[BGW_MAXLEN];

	LWLockAcquire(BackgroundWorkerLock, LW_SHARED);

	for (slotno = 0; slotno < BackgroundWorkerData->total_slots; slotno++)
	{
		BackgroundWorkerSlot *slot = &BackgroundWorkerData->slot[slotno];

		if (slot->pid > 0 && slot->pid == pid)
		{
			strcpy(result, slot->worker.bgw_type);
			found = true;
			break;
		}
	}

	LWLockRelease(BackgroundWorkerLock);

	if (!found)
		return NULL;

	return result;
}
