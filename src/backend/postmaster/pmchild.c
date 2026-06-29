/*-------------------------------------------------------------------------
 *
 * pmchild.c
 *	  Functions for keeping track of postmaster child processes.
 *	  跟踪 postmaster 子进程的函数。
 *
 * Postmaster keeps track of all child processes so that when a process exits,
 * it knows what kind of a process it was and can clean up accordingly.  Every
 * child process is allocated a PMChild struct from a fixed pool of structs.
 * The size of the pool is determined by various settings that configure how
 * many worker processes and backend connections are allowed, i.e.
 * autovacuum_worker_slots, max_worker_processes, max_wal_senders, and
 * max_connections.
 * Postmaster 跟踪所有子进程，以便在子进程退出时，知道它是哪种类型的进程并进行相应的清理。
 * 每个子进程都会从一个固定的结构体池中分配一个 PMChild 结构。
 * 池的大小由配置允许多少个工作进程和后台连接的各种设置决定，即：
 * autovacuum_worker_slots、max_worker_processes、max_wal_senders 和 max_connections。
 *
 * Dead-end backends are handled slightly differently.  There is no limit
 * on the number of dead-end backends, and they do not need unique IDs, so
 * their PMChild structs are allocated dynamically, not from a pool.
 * 死路（Dead-end）后台进程的处理方式略有不同。对死路后台进程的数量没有限制，
 * 且它们不需要唯一的 ID，因此它们的 PMChild 结构是动态分配的，而不是从池中分配。
 *
 * The structures and functions in this file are private to the postmaster
 * process.  But note that there is an array in shared memory, managed by
 * pmsignal.c, that mirrors this.
 * 本文件中的结构和函数是 postmaster 进程私有的。但请注意，
 * 在共享内存中有一个由 pmsignal.c 管理的数组镜像了这一结构。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/pmchild.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"

/*
 * Freelists for different kinds of child processes.  We maintain separate
 * pools for each, so that for example launching a lot of regular backends
 * cannot prevent autovacuum or an aux process from launching.
 */
/*
 * 不同类型子进程的空闲列表（Freelists）。我们为每种进程维护独立的池，
 * 这样例如启动大量常规的后台进程不会阻碍 autovacuum 或辅助进程的启动。
 */
typedef struct PMChildPool
{
	int			size;			/* number of PMChild slots reserved for this
								 * kind of processes */
								/* 为这种进程保留的 PMChild 槽数量 */
	int			first_slotno;	/* first slot belonging to this pool */
								/* 属于该池的第一个槽编号 */
	dlist_head	freelist;		/* currently unused PMChild entries */
								/* 当前未使用的 PMChild 条目 */
} PMChildPool;

static PMChildPool pmchild_pools[BACKEND_NUM_TYPES];
NON_EXEC_STATIC int num_pmchild_slots = 0;

/*
 * List of active child processes.  This includes dead-end children.
 */
/*
 * 活动子进程列表。这包括死路（dead-end）子进程。
 */
dlist_head	ActiveChildList;

/*
 * MaxLivePostmasterChildren
 *
 * This reports the number of postmaster child processes that can be active.
 * It includes all children except for dead-end children.  This allows the
 * array in shared memory (PMChildFlags) to have a fixed maximum size.
 */
/*
 * MaxLivePostmasterChildren
 *
 * 这报告了可以处于活动状态的 postmaster 子进程的数量。
 * 它包括除死路子进程之外的所有子进程。这允许共享内存中的数组（PMChildFlags）具有固定的最大大小。
 *
 * Function purpose: Return maximum allowed active postmaster children (excluding dead-end ones).
 * 函数作用：返回允许的最大活动子进程数量（不包含死路后台进程）。
 */
int
MaxLivePostmasterChildren(void)
{
	if (num_pmchild_slots == 0)
		elog(ERROR, "PM child array not initialized yet");
	return num_pmchild_slots;
}

/*
 * Initialize at postmaster startup
 *
 * Note: This is not called on crash restart.  We rely on PMChild entries to
 * remain valid through the restart process.  This is important because the
 * syslogger survives through the crash restart process, so we must not
 * invalidate its PMChild slot.
 */
/*
 * 在 postmaster 启动时进行初始化
 *
 * 注意：崩溃重启时不会调用此函数。我们依赖于 PMChild 条目在重启过程中保持有效。
 * 这很重要，因为系统日志进程（syslogger）在崩溃重启过程中幸存下来，所以我们绝不能使其 PMChild 槽失效。
 *
 * Function purpose: Initialize postmaster child pools and slots during startup.
 * 函数作用：在系统启动时初始化子进程的池和分配槽。
 *
 * Core workflow:
 * 核心流程：
 * 1. Compute pool sizes for different backend types (regular backend, autovacuum, bgworker, etc.).
 *    计算不同后台进程类型的池大小（常规后台连接、autovacuum 线程、后台工作进程等）。
 * 2. Total up the number of slots and allocate memory for slots array.
 *    统计总槽位数量并分配 slots 数组内存。
 * 3. Initialize each slot, populate freelist for each pool, and init ActiveChildList.
 *    初始化每个槽位，填充每个池的空闲链表（freelist），并初始化活动子进程列表 ActiveChildList。
 */
void
InitPostmasterChildSlots(void)
{
	int			slotno;
	PMChild    *slots;

	/*
	 * We allow more connections here than we can have backends because some
	 * might still be authenticating; they might fail auth, or some existing
	 * backend might exit before the auth cycle is completed.  The exact
	 * MaxConnections limit is enforced when a new backend tries to join the
	 * PGPROC array.
	 *
	 * WAL senders start out as regular backends, so they share the same pool.
	 */
	/*
	 * 我们在此处允许比实际拥有的后台进程更多的连接，因为有些连接可能仍在进行身份验证；
	 * 它们可能在身份验证中失败，或者某些现有的后台进程在验证周期完成之前就已经退出。
	 * 确切的 MaxConnections 限制会在新后台尝试加入 PGPROC 数组时强制执行。
	 *
	 * WAL 发送端（WAL senders）刚开始是作为常规后台进程启动的，因此它们共享相同的池。
	 */
	pmchild_pools[B_BACKEND].size = 2 * (MaxConnections + max_wal_senders);

	pmchild_pools[B_AUTOVAC_WORKER].size = autovacuum_worker_slots;
	pmchild_pools[B_BG_WORKER].size = max_worker_processes;
	pmchild_pools[B_IO_WORKER].size = MAX_IO_WORKERS;

	/*
	 * There can be only one of each of these running at a time.  They each
	 * get their own pool of just one entry.
	 */
	/*
	 * 这些进程一次只能有一个在运行。它们各自拥有自己只有一个条目的池。
	 */
	pmchild_pools[B_AUTOVAC_LAUNCHER].size = 1;
	pmchild_pools[B_SLOTSYNC_WORKER].size = 1;
	pmchild_pools[B_ARCHIVER].size = 1;
	pmchild_pools[B_BG_WRITER].size = 1;
	pmchild_pools[B_CHECKPOINTER].size = 1;
	pmchild_pools[B_STARTUP].size = 1;
	pmchild_pools[B_WAL_RECEIVER].size = 1;
	pmchild_pools[B_WAL_SUMMARIZER].size = 1;
	pmchild_pools[B_WAL_WRITER].size = 1;
	pmchild_pools[B_LOGGER].size = 1;

	/* The rest of the pmchild_pools are left at zero size */
	/* 其余的 pmchild_pools 保持大小为零 */

	/* Count the total number of slots */
	/* 计算总槽数 */
	num_pmchild_slots = 0;
	for (int i = 0; i < BACKEND_NUM_TYPES; i++)
		num_pmchild_slots += pmchild_pools[i].size;

	/* Initialize them */
	/* 初始化它们 */
	slots = palloc(num_pmchild_slots * sizeof(PMChild));
	slotno = 0;
	for (int btype = 0; btype < BACKEND_NUM_TYPES; btype++)
	{
		pmchild_pools[btype].first_slotno = slotno + 1;
		dlist_init(&pmchild_pools[btype].freelist);

		for (int j = 0; j < pmchild_pools[btype].size; j++)
		{
			slots[slotno].pid = 0;
			slots[slotno].child_slot = slotno + 1;
			slots[slotno].bkend_type = B_INVALID;
			slots[slotno].rw = NULL;
			slots[slotno].bgworker_notify = false;
			dlist_push_tail(&pmchild_pools[btype].freelist, &slots[slotno].elem);
			slotno++;
		}
	}
	Assert(slotno == num_pmchild_slots);

	/* Initialize other structures */
	/* 初始化其他结构 */
	dlist_init(&ActiveChildList);
}

/*
 * Allocate a PMChild entry for a postmaster child process of given type.
 *
 * The entry is taken from the right pool for the type.
 *
 * pmchild->child_slot in the returned struct is unique among all active child
 * processes.
 */
/*
 * 为给定类型的 postmaster 子进程分配一个 PMChild 条目。
 *
 * 该条目是从该类型的正确池中获取的。
 *
 * 返回的结构中的 pmchild->child_slot 在所有活动子进程中是唯一的。
 *
 * Function purpose: Assign an available slot for a postmaster child of a given backend type.
 * 函数作用：为指定后台进程类型的子进程分配一个可用槽。
 *
 * Core workflow:
 * 核心流程：
 * 1. Retrieve the freelist for the given backend type.
 *    检索给定后台类型的空闲列表（freelist）。
 * 2. Pop a node, set its properties, verify correctness of child_slot.
 *    弹出一个节点，设置属性并确认其 child_slot 范围合法。
 * 3. Add to ActiveChildList and notify shared memory by calling MarkPostmasterChildSlotAssigned.
 *    将其添加到 ActiveChildList 并调用 MarkPostmasterChildSlotAssigned 标记共享内存中已被分配。
 */
PMChild *
AssignPostmasterChildSlot(BackendType btype)
{
	dlist_head *freelist;
	PMChild    *pmchild;

	if (pmchild_pools[btype].size == 0)
		elog(ERROR, "cannot allocate a PMChild slot for backend type %d", btype);

	freelist = &pmchild_pools[btype].freelist;
	if (dlist_is_empty(freelist))
		return NULL;

	pmchild = dlist_container(PMChild, elem, dlist_pop_head_node(freelist));
	pmchild->pid = 0;
	pmchild->bkend_type = btype;
	pmchild->rw = NULL;
	pmchild->bgworker_notify = true;

	/*
	 * pmchild->child_slot for each entry was initialized when the array of
	 * slots was allocated.  Sanity check it.
	 */
	/*
	 * 分配槽数组时，已对每个条目的 pmchild->child_slot 进行了初始化。对其进行健壮性检查。
	 */
	if (!(pmchild->child_slot >= pmchild_pools[btype].first_slotno &&
		  pmchild->child_slot < pmchild_pools[btype].first_slotno + pmchild_pools[btype].size))
	{
		elog(ERROR, "pmchild freelist for backend type %d is corrupt",
			 pmchild->bkend_type);
	}

	dlist_push_head(&ActiveChildList, &pmchild->elem);

	/* Update the status in the shared memory array */
	/* 更新共享内存数组中的状态 */
	MarkPostmasterChildSlotAssigned(pmchild->child_slot);

	elog(DEBUG2, "assigned pm child slot %d for %s",
		 pmchild->child_slot, PostmasterChildName(btype));

	return pmchild;
}

/*
 * Allocate a PMChild struct for a dead-end backend.  Dead-end children are
 * not assigned a child_slot number.  The struct is palloc'd; returns NULL if
 * out of memory.
 */
/*
 * 为死路（dead-end）后台进程分配一个 PMChild 结构。
 * 死路子进程不分配 child_slot 编号。该结构是通过 palloc 分配的；如果内存不足，则返回 NULL。
 *
 * Function purpose: Dynamically allocate a PMChild struct for a dead-end connection.
 * 函数作用：为无法正常处理且即将直接退出的死路（dead-end）连接动态分配 PMChild 结构。
 */
PMChild *
AllocDeadEndChild(void)
{
	PMChild    *pmchild;

	elog(DEBUG2, "allocating dead-end child");

	pmchild = (PMChild *) palloc_extended(sizeof(PMChild), MCXT_ALLOC_NO_OOM);
	if (pmchild)
	{
		pmchild->pid = 0;
		pmchild->child_slot = 0;
		pmchild->bkend_type = B_DEAD_END_BACKEND;
		pmchild->rw = NULL;
		pmchild->bgworker_notify = false;

		dlist_push_head(&ActiveChildList, &pmchild->elem);
	}

	return pmchild;
}

/*
 * Release a PMChild slot, after the child process has exited.
 *
 * Returns true if the child detached cleanly from shared memory, false
 * otherwise (see MarkPostmasterChildSlotUnassigned).
 */
/*
 * 在子进程退出后释放其 PMChild 槽。
 *
 * 如果子进程干净地脱离了共享内存，则返回 true，否则返回 false（参见 MarkPostmasterChildSlotUnassigned）。
 *
 * Function purpose: Release and return the PMChild entry to its pool or free the memory.
 * 函数作用：在子进程退出后，释放并将其 PMChild 槽归还至对应的空闲链表或释放内存。
 */
bool
ReleasePostmasterChildSlot(PMChild *pmchild)
{
	dlist_delete(&pmchild->elem);
	if (pmchild->bkend_type == B_DEAD_END_BACKEND)
	{
		elog(DEBUG2, "releasing dead-end backend");
		pfree(pmchild);
		return true;
	}
	else
	{
		PMChildPool *pool;

		elog(DEBUG2, "releasing pm child slot %d", pmchild->child_slot);

		/* WAL senders start out as regular backends, and share the pool */
		/* WAL 发送端初始为普通后台进程，共享连接池 */
		if (pmchild->bkend_type == B_WAL_SENDER)
			pool = &pmchild_pools[B_BACKEND];
		else
			pool = &pmchild_pools[pmchild->bkend_type];

		/* sanity check that we return the entry to the right pool */
		/* 确保我们将条目归还给正确的池的健壮性检查 */
		if (!(pmchild->child_slot >= pool->first_slotno &&
			  pmchild->child_slot < pool->first_slotno + pool->size))
		{
			elog(ERROR, "pmchild freelist for backend type %d is corrupt",
				 pmchild->bkend_type);
		}

		dlist_push_head(&pool->freelist, &pmchild->elem);
		return MarkPostmasterChildSlotUnassigned(pmchild->child_slot);
	}
}

/*
 * Find the PMChild entry of a running child process by PID.
 */
/*
 * 根据 PID 查找正在运行的子进程的 PMChild 条目。
 *
 * Function purpose: Lookup PMChild entry associated with the given PID in ActiveChildList.
 * 函数作用：根据进程 PID 在 ActiveChildList 中查找对应的 PMChild 结构体。
 */
PMChild *
FindPostmasterChildByPid(int pid)
{
	dlist_iter	iter;

	dlist_foreach(iter, &ActiveChildList)
	{
		PMChild    *bp = dlist_container(PMChild, elem, iter.cur);

		if (bp->pid == pid)
			return bp;
	}
	return NULL;
}

