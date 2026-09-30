/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "access/clog.h"
#include "access/xlogdefs.h"
#include "lib/ilist.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/pg_sema.h"
#include "storage/proclist_types.h"
#include "storage/procnumber.h"

/*
 * Each backend advertises up to PGPROC_MAX_CACHED_SUBXIDS TransactionIds
 * for non-aborted subtransactions of its current top transaction.  These
 * have to be treated as running XIDs by other backends.
 *
 * We also keep track of whether the cache overflowed (ie, the transaction has
 * generated at least one subtransaction that didn't fit in the cache).
 * If none of the caches have overflowed, we can assume that an XID that's not
 * listed anywhere in the PGPROC array is not a running transaction.  Else we
 * have to look at pg_subtrans.
 *
 * See src/test/isolation/specs/subxid-overflow.spec if you change this.
 */

/*
 * 每个后端都会为其当前顶层事务的未中止子事务公布最多
 * PGPROC_MAX_CACHED_SUBXIDS 个 TransactionId。其他后端必须将它们视为运行中的 XID。
 *
 * 还会跟踪缓存是否溢出（即事务至少生成了一个无法放入缓存的子事务）。若所有缓存均未
 * 溢出，可假定 PGPROC 数组中未列出的 XID 不是运行中事务；否则必须查看 pg_subtrans。
 *
 * 修改此处时请参阅 src/test/isolation/specs/subxid-overflow.spec。
 */
#define PGPROC_MAX_CACHED_SUBXIDS 64	/* XXX guessed-at value */

/* XXX：推测的值。 */

typedef struct XidCacheStatus
{
	/* number of cached subxids, never more than PGPROC_MAX_CACHED_SUBXIDS */

	/* 缓存的子 xid 数，绝不超过 PGPROC_MAX_CACHED_SUBXIDS。 */
	uint8		count;
	/* has PGPROC->subxids overflowed */

	/* PGPROC->subxids 是否已溢出。 */
	bool		overflowed;
} XidCacheStatus;

struct XidCache
{
	TransactionId xids[PGPROC_MAX_CACHED_SUBXIDS];
};

/*
 * Flags for PGPROC->statusFlags and PROC_HDR->statusFlags[]
 */

/*
 * PGPROC->statusFlags 和 PROC_HDR->statusFlags[] 的标志。
 */
#define		PROC_IS_AUTOVACUUM	0x01	/* is it an autovac worker? */

/* 是否是自动清理工作进程？ */
#define		PROC_IN_VACUUM		0x02	/* currently running lazy vacuum */

/* 当前正在运行惰性清理。 */
#define		PROC_IN_SAFE_IC		0x04	/* currently running CREATE INDEX
										 * CONCURRENTLY or REINDEX
										 * CONCURRENTLY on non-expressional,
										 * non-partial index */

/* 当前正在对非表达式、非部分索引执行 CREATE INDEX CONCURRENTLY 或 REINDEX CONCURRENTLY。 */
#define		PROC_VACUUM_FOR_WRAPAROUND	0x08	/* set by autovac only */

/* 仅由自动清理设置。 */
#define		PROC_IN_LOGICAL_DECODING	0x10	/* currently doing logical
												 * decoding outside xact */

/* 当前正在事务外执行逻辑解码。 */
#define		PROC_AFFECTS_ALL_HORIZONS	0x20	/* this proc's xmin must be
												 * included in vacuum horizons
												 * in all databases */

/* 此进程的 xmin 必须包含在所有数据库的清理视界中。 */

/* flags reset at EOXact */

/* 在 EOXact 时重置的标志。 */
#define		PROC_VACUUM_STATE_MASK \
	(PROC_IN_VACUUM | PROC_IN_SAFE_IC | PROC_VACUUM_FOR_WRAPAROUND)

/*
 * Xmin-related flags. Make sure any flags that affect how the process' Xmin
 * value is interpreted by VACUUM are included here.
 */

/*
 * 与 Xmin 相关的标志。确保所有影响 VACUUM 如何解释进程 Xmin 值的标志都包含在这里。
 */
#define		PROC_XMIN_FLAGS (PROC_IN_VACUUM | PROC_IN_SAFE_IC)

/*
 * We allow a limited number of "weak" relation locks (AccessShareLock,
 * RowShareLock, RowExclusiveLock) to be recorded in the PGPROC structure
 * (or rather in shared memory referenced from PGPROC) rather than the main
 * lock table.  This eases contention on the lock manager LWLocks.  See
 * storage/lmgr/README for additional details.
 */

/*
 * 允许将有限数量的“弱”关系锁（AccessShareLock、RowShareLock、RowExclusiveLock）记录
 * 在 PGPROC 结构体（或由 PGPROC 引用的共享内存）中，而不是主锁表中。这可减轻锁管理器
 * LWLock 上的争用。更多细节参见 storage/lmgr/README。
 */
extern PGDLLIMPORT int FastPathLockGroupsPerBackend;

/*
 * Define the maximum number of fast-path locking groups per backend.
 * This must be a power-of-two value.  The actual number of fast-path
 * lock groups is calculated in InitializeFastPathLocks() based on
 * max_locks_per_transaction.  1024 is an arbitrary upper limit (matching
 * max_locks_per_transaction = 16k).  Values over 1024 are unlikely to be
 * beneficial as there are bottlenecks we'll hit way before that.
 */

/*
 * 定义每后端快速路径锁组的最大数量。该值必须是 2 的幂。实际快速路径锁组数量由
 * InitializeFastPathLocks() 基于 max_locks_per_transaction 计算。1024 是任意上限
 * （匹配 max_locks_per_transaction = 16k）。超过 1024 的值不太可能有益，因为更早会
 * 遇到瓶颈。
 */
#define		FP_LOCK_GROUPS_PER_BACKEND_MAX	1024
#define		FP_LOCK_SLOTS_PER_GROUP		16	/* don't change */

/* 不要更改。 */
#define		FastPathLockSlotsPerBackend() \
	(FP_LOCK_SLOTS_PER_GROUP * FastPathLockGroupsPerBackend)

/*
 * Flags for PGPROC.delayChkptFlags
 *
 * These flags can be used to delay the start or completion of a checkpoint
 * for short periods. A flag is in effect if the corresponding bit is set in
 * the PGPROC of any backend.
 *
 * For our purposes here, a checkpoint has three phases: (1) determine the
 * location to which the redo pointer will be moved, (2) write all the
 * data durably to disk, and (3) WAL-log the checkpoint.
 *
 * Setting DELAY_CHKPT_START prevents the system from moving from phase 1
 * to phase 2. This is useful when we are performing a WAL-logged modification
 * of data that will be flushed to disk in phase 2. By setting this flag
 * before writing WAL and clearing it after we've both written WAL and
 * performed the corresponding modification, we ensure that if the WAL record
 * is inserted prior to the new redo point, the corresponding data changes will
 * also be flushed to disk before the checkpoint can complete. (In the
 * extremely common case where the data being modified is in shared buffers
 * and we acquire an exclusive content lock and MarkBufferDirty() on the
 * relevant buffers before writing WAL, this mechanism is not needed, because
 * phase 2 will block until we release the content lock and then flush the
 * modified data to disk.  See transam/README and SyncOneBuffer().)
 *
 * Setting DELAY_CHKPT_COMPLETE prevents the system from moving from phase 2
 * to phase 3. This is useful if we are performing a WAL-logged operation that
 * might invalidate buffers, such as relation truncation. In this case, we need
 * to ensure that any buffers which were invalidated and thus not flushed by
 * the checkpoint are actually destroyed on disk. Replay can cope with a file
 * or block that doesn't exist, but not with a block that has the wrong
 * contents.
 */

/*
 * PGPROC.delayChkptFlags 的标志。
 *
 * 这些标志可短暂延迟检查点的开始或完成。当任一后端的 PGPROC 中设置了对应位时，标志生效。
 *
 * 对此处目的而言，检查点有三个阶段：(1) 确定重做指针将移动到的位置，(2) 将所有数据
 * 持久写入磁盘，(3) 为检查点写入 WAL。
 *
 * 设置 DELAY_CHKPT_START 会阻止系统从阶段 1 进入阶段 2。当执行将在阶段 2 刷盘的
 * WAL 记录修改时很有用。在写入 WAL 前设置此标志，并在同时写入 WAL 和执行相应修改后
 * 清除它，可确保若 WAL 记录在新重做点之前插入，对应数据变化也会在检查点完成前刷盘。
 * （在极常见情形中，修改的数据位于共享缓冲区，且在写 WAL 前获取排他内容锁并对相关缓冲区
 * 调用 MarkBufferDirty()，则不需要该机制，因为阶段 2 会阻塞到释放内容锁后再将修改数据
 * 刷盘。参见 transam/README 和 SyncOneBuffer()。）
 *
 * 设置 DELAY_CHKPT_COMPLETE 会阻止系统从阶段 2 进入阶段 3。若执行可能使缓冲区失效的
 * WAL 记录操作（如关系截断），该标志很有用。此时需要确保已失效、因而未由检查点刷新的
 * 缓冲区确实在磁盘上销毁。重放可处理不存在的文件或块，但不能处理内容错误的块。
 */
#define DELAY_CHKPT_START		(1<<0)
#define DELAY_CHKPT_COMPLETE	(1<<1)

typedef enum
{
	PROC_WAIT_STATUS_OK,
	PROC_WAIT_STATUS_WAITING,
	PROC_WAIT_STATUS_ERROR,
} ProcWaitStatus;

/*
 * Each backend has a PGPROC struct in shared memory.  There is also a list of
 * currently-unused PGPROC structs that will be reallocated to new backends.
 *
 * links: list link for any list the PGPROC is in.  When waiting for a lock,
 * the PGPROC is linked into that lock's waitProcs queue.  A recycled PGPROC
 * is linked into ProcGlobal's freeProcs list.
 *
 * Note: twophase.c also sets up a dummy PGPROC struct for each currently
 * prepared transaction.  These PGPROCs appear in the ProcArray data structure
 * so that the prepared transactions appear to be still running and are
 * correctly shown as holding locks.  A prepared transaction PGPROC can be
 * distinguished from a real one at need by the fact that it has pid == 0.
 * The semaphore and lock-activity fields in a prepared-xact PGPROC are unused,
 * but its myProcLocks[] lists are valid.
 *
 * We allow many fields of this struct to be accessed without locks, such as
 * delayChkptFlags and isRegularBackend. However, keep in mind that writing
 * mirrored ones (see below) requires holding ProcArrayLock or XidGenLock in
 * at least shared mode, so that pgxactoff does not change concurrently.
 *
 * Mirrored fields:
 *
 * Some fields in PGPROC (see "mirrored in ..." comment) are mirrored into an
 * element of more densely packed ProcGlobal arrays. These arrays are indexed
 * by PGPROC->pgxactoff. Both copies need to be maintained coherently.
 *
 * NB: The pgxactoff indexed value can *never* be accessed without holding
 * locks.
 *
 * See PROC_HDR for details.
 */

/*
 * 每个后端在共享内存中都有一个 PGPROC 结构体。还维护一个当前未使用的 PGPROC 结构体
 * 列表，以便重新分配给新后端。
 *
 * links：PGPROC 所在任意链表的链接。等待锁时，PGPROC 链接到该锁的 waitProcs 队列；
 * 回收的 PGPROC 链接到 ProcGlobal 的 freeProcs 列表。
 *
 * 注意：twophase.c 还会为每个当前已准备事务设置虚拟 PGPROC。它们出现在 ProcArray 中，
 * 以使已准备事务显示为仍在运行并正确显示为持有锁。虚拟 PGPROC 的 pid == 0，可据此和
 * 真实 PGPROC 区分。已准备事务 PGPROC 的信号量和锁活动字段未使用，但其 myProcLocks[]
 * 列表有效。
 *
 * 允许无锁访问本结构体的许多字段，如 delayChkptFlags 和 isRegularBackend。但写入镜像
 * 字段（见下文）时至少须以共享模式持有 ProcArrayLock 或 XidGenLock，以防 pgxactoff
 * 并发改变。
 *
 * 镜像字段：
 *
 * PGPROC 中某些字段（参见“mirrored in ...”注释）镜像到更紧凑的 ProcGlobal 数组元素。
 * 这些数组由 PGPROC->pgxactoff 索引，两个副本必须保持一致。
 *
 * 注意：不持锁绝不能访问按 pgxactoff 索引的值。
 *
 * 细节参见 PROC_HDR。
 */
struct PGPROC
{
	dlist_node	links;			/* list link if process is in a list */

	/* 若进程在链表中，则为列表链接。 */
	dlist_head *procgloballist; /* procglobal list that owns this PGPROC */

	/* 拥有此 PGPROC 的 procglobal 链表。 */

	PGSemaphore sem;			/* ONE semaphore to sleep on */

	/* 用于休眠的唯一信号量。 */
	ProcWaitStatus waitStatus;

	Latch		procLatch;		/* generic latch for process */

	/* 进程通用闩锁。 */


	TransactionId xid;			/* id of top-level transaction currently being
								 * executed by this proc, if running and XID
								 * is assigned; else InvalidTransactionId.
								 * mirrored in ProcGlobal->xids[pgxactoff] */

	/* 此进程当前执行的顶层事务 ID；正在运行且已分配 XID 时有效，否则为
	 * InvalidTransactionId。镜像于 ProcGlobal->xids[pgxactoff]。 */

	TransactionId xmin;			/* minimal running XID as it was when we were
								 * starting our xact, excluding LAZY VACUUM:
								 * vacuum must not remove tuples deleted by
								 * xid >= xmin ! */

	/* 开始本事务时的最小运行中 XID（不含 LAZY VACUUM）：VACUUM 不得移除由
	 * xid >= xmin 删除的元组！ */

	int			pid;			/* Backend's process ID; 0 if prepared xact */

	/* 后端进程 ID；若为已准备事务则为 0。 */

	int			pgxactoff;		/* offset into various ProcGlobal->arrays with
								 * data mirrored from this PGPROC */

	/* 多个 ProcGlobal 数组中的偏移量，其中数据镜像自此 PGPROC。 */

	/*
	 * Currently running top-level transaction's virtual xid. Together these
	 * form a VirtualTransactionId, but we don't use that struct because this
	 * is not atomically assignable as whole, and we want to enforce code to
	 * consider both parts separately.  See comments at VirtualTransactionId.
	 */

	/*
	 * 当前运行顶层事务的虚拟 xid。两部分共同构成 VirtualTransactionId，但不使用该结构体，
	 * 因为它不能作为整体原子赋值，并且希望强制代码分别处理两部分。参见
	 * VirtualTransactionId 的注释。
	 */
	struct
	{
		ProcNumber	procNumber; /* For regular backends, equal to
								 * GetNumberFromPGProc(proc).  For prepared
								 * xacts, ID of the original backend that
								 * processed the transaction. For unused
								 * PGPROC entries, INVALID_PROC_NUMBER. */

		/* 对常规后端，等于 GetNumberFromPGProc(proc)。对已准备事务，为处理事务的原始后端
		 * 的 ID。对未使用 PGPROC 条目，为 INVALID_PROC_NUMBER。 */
		LocalTransactionId lxid;	/* local id of top-level transaction
									 * currently * being executed by this
									 * proc, if running; else
									 * InvalidLocalTransactionId */

		/* 此进程正在执行的顶层事务本地 ID；若未运行则为 InvalidLocalTransactionId。 */
	}			vxid;

	/* These fields are zero while a backend is still starting up: */

	/* 后端仍在启动时，这些字段为零： */
	Oid			databaseId;		/* OID of database this backend is using */

	/* 此后端正在使用的数据库 OID。 */
	Oid			roleId;			/* OID of role using this backend */

	/* 使用此后端的角色 OID。 */

	Oid			tempNamespaceId;	/* OID of temp schema this backend is
									 * using */

	/* 此后端正在使用的临时模式 OID。 */

	bool		isRegularBackend;	/* true if it's a regular backend. */

	/* 若为常规后端则为真。 */

	/*
	 * While in hot standby mode, shows that a conflict signal has been sent
	 * for the current transaction. Set/cleared while holding ProcArrayLock,
	 * though not required. Accessed without lock, if needed.
	 */

	/*
	 * 在热备模式期间，表明已为当前事务发送冲突信号。持有 ProcArrayLock 时设置/清除，
	 * 但并非必须。需要时可无锁访问。
	 */
	bool		recoveryConflictPending;

	/* Info about LWLock the process is currently waiting for, if any. */

	/* 进程当前正在等待的 LWLock 信息（如有）。 */
	uint8		lwWaiting;		/* see LWLockWaitState */

	/* 参见 LWLockWaitState。 */
	uint8		lwWaitMode;		/* lwlock mode being waited for */

	/* 正在等待的 lwlock 模式。 */
	proclist_node lwWaitLink;	/* position in LW lock wait list */

	/* 在 LW 锁等待列表中的位置。 */

	/* Support for condition variables. */

	/* 对条件变量的支持。 */
	proclist_node cvWaitLink;	/* position in CV wait list */

	/* 在 CV 等待列表中的位置。 */

	/* Info about lock the process is currently waiting for, if any. */

	/* 进程当前正在等待的锁信息（如有）。 */
	/* waitLock and waitProcLock are NULL if not currently waiting. */

	/* 当前未等待时 waitLock 和 waitProcLock 均为 NULL。 */
	LOCK	   *waitLock;		/* Lock object we're sleeping on ... */

	/* 正在休眠等待的 Lock 对象。 */
	PROCLOCK   *waitProcLock;	/* Per-holder info for awaited lock */

	/* 所等待锁的每持有者信息。 */
	LOCKMODE	waitLockMode;	/* type of lock we're waiting for */

	/* 正在等待的锁类型。 */
	LOCKMASK	heldLocks;		/* bitmask for lock types already held on this
								 * lock object by this backend */

	/* 此后端已在该锁对象上持有的锁类型位掩码。 */
	pg_atomic_uint64 waitStart; /* time at which wait for lock acquisition
								 * started */

	/* 开始等待获取锁的时间。 */

	int			delayChkptFlags;	/* for DELAY_CHKPT_* flags */

	/* 用于 DELAY_CHKPT_* 标志。 */

	uint8		statusFlags;	/* this backend's status flags, see PROC_*
								 * above. mirrored in
								 * ProcGlobal->statusFlags[pgxactoff] */

	/* 此后端的状态标志，参见上方 PROC_*。镜像于 ProcGlobal->statusFlags[pgxactoff]。 */

	/*
	 * Info to allow us to wait for synchronous replication, if needed.
	 * waitLSN is InvalidXLogRecPtr if not waiting; set only by user backend.
	 * syncRepState must not be touched except by owning process or WALSender.
	 * syncRepLinks used only while holding SyncRepLock.
	 */

	/*
	 * 必要时等待同步复制所需的信息。未等待时 waitLSN 为 InvalidXLogRecPtr；仅由用户后端
	 * 设置。除拥有进程或 WALSender 外不得访问 syncRepState。syncRepLinks 仅在持有
	 * SyncRepLock 时使用。
	 */
	XLogRecPtr	waitLSN;		/* waiting for this LSN or higher */

	/* 正在等待此 LSN 或更高位置。 */
	int			syncRepState;	/* wait state for sync rep */

	/* 同步复制等待状态。 */
	dlist_node	syncRepLinks;	/* list link if process is in syncrep queue */

	/* 若进程在同步复制队列中，则为列表链接。 */

	/*
	 * All PROCLOCK objects for locks held or awaited by this backend are
	 * linked into one of these lists, according to the partition number of
	 * their lock.
	 */

	/*
	 * 此后端持有或等待锁的所有 PROCLOCK 对象都按其锁的分区编号链接到这些列表之一。
	 */
	dlist_head	myProcLocks[NUM_LOCK_PARTITIONS];

	XidCacheStatus subxidStatus;	/* mirrored with
									 * ProcGlobal->subxidStates[i] */

	/* 镜像于 ProcGlobal->subxidStates[i]。 */
	struct XidCache subxids;	/* cache for subtransaction XIDs */

	/* 子事务 XID 的缓存。 */

	/* Support for group XID clearing. */

	/* 支持组 XID 清除。 */
	/* true, if member of ProcArray group waiting for XID clear */

	/* 若属于等待清除 XID 的 ProcArray 组，则为真。 */
	bool		procArrayGroupMember;
	/* next ProcArray group member waiting for XID clear */

	/* 等待清除 XID 的下一个 ProcArray 组成员。 */
	pg_atomic_uint32 procArrayGroupNext;

	/*
	 * latest transaction id among the transaction's main XID and
	 * subtransactions
	 */

	/*
	 * 事务主 XID 和子事务中的最新事务 ID。
	 */
	TransactionId procArrayGroupMemberXid;

	uint32		wait_event_info;	/* proc's wait information */

	/* 进程的等待信息。 */

	/* Support for group transaction status update. */

	/* 支持组事务状态更新。 */
	bool		clogGroupMember;	/* true, if member of clog group */

	/* 若属于 clog 组，则为真。 */
	pg_atomic_uint32 clogGroupNext; /* next clog group member */

	/* 下一个 clog 组成员。 */
	TransactionId clogGroupMemberXid;	/* transaction id of clog group member */

	/* clog 组成员的事务 ID。 */
	XidStatus	clogGroupMemberXidStatus;	/* transaction status of clog
											 * group member */

	/* clog 组成员的事务状态。 */
	int64		clogGroupMemberPage;	/* clog page corresponding to
										 * transaction id of clog group member */

	/* 与 clog 组成员事务 ID 对应的 clog 页面。 */
	XLogRecPtr	clogGroupMemberLsn; /* WAL location of commit record for clog
									 * group member */

	/* clog 组成员提交记录的 WAL 位置。 */

	/* Lock manager data, recording fast-path locks taken by this backend. */

	/* 锁管理器数据，记录此后端获取的快速路径锁。 */
	LWLock		fpInfoLock;		/* protects per-backend fast-path state */

	/* 保护每后端快速路径状态。 */
	uint64	   *fpLockBits;		/* lock modes held for each fast-path slot */

	/* 每个快速路径槽位持有的锁模式。 */
	Oid		   *fpRelId;		/* slots for rel oids */

	/* 关系 OID 的槽位。 */
	bool		fpVXIDLock;		/* are we holding a fast-path VXID lock? */

	/* 是否持有快速路径 VXID 锁？ */
	LocalTransactionId fpLocalTransactionId;	/* lxid for fast-path VXID
												 * lock */

	/* 快速路径 VXID 锁的 lxid。 */

	/*
	 * Support for lock groups.  Use LockHashPartitionLockByProc on the group
	 * leader to get the LWLock protecting these fields.
	 */

	/*
	 * 支持锁组。在组领导者上使用 LockHashPartitionLockByProc 获取保护这些字段的 LWLock。
	 */
	PGPROC	   *lockGroupLeader;	/* lock group leader, if I'm a member */

	/* 若我是成员，则为锁组领导者。 */
	dlist_head	lockGroupMembers;	/* list of members, if I'm a leader */

	/* 若我是领导者，则为成员列表。 */
	dlist_node	lockGroupLink;	/* my member link, if I'm a member */

	/* 若我是成员，则为我的成员链接。 */
};

/* NOTE: "typedef struct PGPROC PGPROC" appears in storage/lock.h. */

/* 注意：“typedef struct PGPROC PGPROC” 位于 storage/lock.h。 */


extern PGDLLIMPORT PGPROC *MyProc;

/*
 * There is one ProcGlobal struct for the whole database cluster.
 *
 * Adding/Removing an entry into the procarray requires holding *both*
 * ProcArrayLock and XidGenLock in exclusive mode (in that order). Both are
 * needed because the dense arrays (see below) are accessed from
 * GetNewTransactionId() and GetSnapshotData(), and we don't want to add
 * further contention by both using the same lock. Adding/Removing a procarray
 * entry is much less frequent.
 *
 * Some fields in PGPROC are mirrored into more densely packed arrays (e.g.
 * xids), with one entry for each backend. These arrays only contain entries
 * for PGPROCs that have been added to the shared array with ProcArrayAdd()
 * (in contrast to PGPROC array which has unused PGPROCs interspersed).
 *
 * The dense arrays are indexed by PGPROC->pgxactoff. Any concurrent
 * ProcArrayAdd() / ProcArrayRemove() can lead to pgxactoff of a procarray
 * member to change.  Therefore it is only safe to use PGPROC->pgxactoff to
 * access the dense array while holding either ProcArrayLock or XidGenLock.
 *
 * As long as a PGPROC is in the procarray, the mirrored values need to be
 * maintained in both places in a coherent manner.
 *
 * The denser separate arrays are beneficial for three main reasons: First, to
 * allow for as tight loops accessing the data as possible. Second, to prevent
 * updates of frequently changing data (e.g. xmin) from invalidating
 * cachelines also containing less frequently changing data (e.g. xid,
 * statusFlags). Third to condense frequently accessed data into as few
 * cachelines as possible.
 *
 * There are two main reasons to have the data mirrored between these dense
 * arrays and PGPROC. First, as explained above, a PGPROC's array entries can
 * only be accessed with either ProcArrayLock or XidGenLock held, whereas the
 * PGPROC entries do not require that (obviously there may still be locking
 * requirements around the individual field, separate from the concerns
 * here). That is particularly important for a backend to efficiently checks
 * it own values, which it often can safely do without locking.  Second, the
 * PGPROC fields allow to avoid unnecessary accesses and modification to the
 * dense arrays. A backend's own PGPROC is more likely to be in a local cache,
 * whereas the cachelines for the dense array will be modified by other
 * backends (often removing it from the cache for other cores/sockets). At
 * commit/abort time a check of the PGPROC value can avoid accessing/dirtying
 * the corresponding array value.
 *
 * Basically it makes sense to access the PGPROC variable when checking a
 * single backend's data, especially when already looking at the PGPROC for
 * other reasons already.  It makes sense to look at the "dense" arrays if we
 * need to look at many / most entries, because we then benefit from the
 * reduced indirection and better cross-process cache-ability.
 *
 * When entering a PGPROC for 2PC transactions with ProcArrayAdd(), the data
 * in the dense arrays is initialized from the PGPROC while it already holds
 * ProcArrayLock.
 */

/*
 * 整个数据库集群只有一个 ProcGlobal 结构体。
 *
 * 向 procarray 添加或移除条目时，须按顺序以排他模式同时持有 ProcArrayLock 和 XidGenLock。
 * 两者均需要，因为下述紧凑数组由 GetNewTransactionId() 和 GetSnapshotData() 访问，
 * 不希望二者使用同一把锁从而增加争用。添加/移除 procarray 条目的频率低得多。
 *
 * PGPROC 的部分字段镜像到更紧凑的数组（例如 xids），每个后端一个条目。这些数组仅包含
 * 已通过 ProcArrayAdd() 加入共享数组的 PGPROC 条目（不同于夹杂未使用 PGPROC 的 PGPROC
 * 数组）。
 *
 * 紧凑数组以 PGPROC->pgxactoff 索引。并发 ProcArrayAdd()/ProcArrayRemove() 会导致
 * procarray 成员的 pgxactoff 变化。因此仅在持有 ProcArrayLock 或 XidGenLock 时才能
 * 使用 PGPROC->pgxactoff 访问紧凑数组。
 *
 * PGPROC 位于 procarray 时，镜像值必须在两处保持一致。
 *
 * 单独更紧凑数组有三个主要好处：可使访问数据的循环尽可能紧凑；避免频繁变化数据（如 xmin）
 * 的更新使同样包含低频数据（如 xid、statusFlags）的缓存行失效；将频繁访问的数据压缩到
 * 尽可能少的缓存行中。
 *
 * 在紧凑数组与 PGPROC 之间镜像数据有两个主要原因。其一，如上所述，PGPROC 的数组条目
 * 只能在持有 ProcArrayLock 或 XidGenLock 时访问，而 PGPROC 条目不需要（但单个字段可能
 * 仍有其他加锁要求）。这对后端高效检查自身值尤为重要，通常可无锁安全完成。其二，PGPROC
 * 字段避免不必要地访问和修改紧凑数组。后端自身 PGPROC 更可能在本地缓存中，紧凑数组缓存行
 * 则会被其他后端修改（往往从其他核心/套接字缓存移除）。提交/中止时，检查 PGPROC 值可避免
 * 访问或弄脏对应数组值。
 *
 * 总之，检查单个后端数据时，尤其已经因其他原因查看 PGPROC 时，访问 PGPROC 变量合理。
 * 若需查看多数条目，访问“紧凑”数组合理，因为可受益于更少的间接寻址和更好的跨进程缓存性。
 *
 * 使用 ProcArrayAdd() 为两阶段提交事务加入 PGPROC 时，已持有 ProcArrayLock，紧凑数组
 * 中的数据会从 PGPROC 初始化。
 */
typedef struct PROC_HDR
{
	/* Array of PGPROC structures (not including dummies for prepared txns) */

	/* PGPROC 结构体数组（不包含已准备事务的虚拟条目）。 */
	PGPROC	   *allProcs;

	/* Array mirroring PGPROC.xid for each PGPROC currently in the procarray */

	/* 为当前在 procarray 中的每个 PGPROC 镜像 PGPROC.xid 的数组。 */
	TransactionId *xids;

	/*
	 * Array mirroring PGPROC.subxidStatus for each PGPROC currently in the
	 * procarray.
	 */

	/*
	 * 为当前在 procarray 中的每个 PGPROC 镜像 PGPROC.subxidStatus 的数组。
	 */
	XidCacheStatus *subxidStates;

	/*
	 * Array mirroring PGPROC.statusFlags for each PGPROC currently in the
	 * procarray.
	 */

	/*
	 * 为当前在 procarray 中的每个 PGPROC 镜像 PGPROC.statusFlags 的数组。
	 */
	uint8	   *statusFlags;

	/* Length of allProcs array */

	/* allProcs 数组长度。 */
	uint32		allProcCount;
	/* Head of list of free PGPROC structures */

	/* 空闲 PGPROC 结构体列表的头部。 */
	dlist_head	freeProcs;
	/* Head of list of autovacuum & special worker free PGPROC structures */

	/* 自动清理和特殊工作进程空闲 PGPROC 结构体列表的头部。 */
	dlist_head	autovacFreeProcs;
	/* Head of list of bgworker free PGPROC structures */

	/* 后台工作进程空闲 PGPROC 结构体列表的头部。 */
	dlist_head	bgworkerFreeProcs;
	/* Head of list of walsender free PGPROC structures */

	/* WAL 发送器空闲 PGPROC 结构体列表的头部。 */
	dlist_head	walsenderFreeProcs;
	/* First pgproc waiting for group XID clear */

	/* 等待组 XID 清除的第一个 pgproc。 */
	pg_atomic_uint32 procArrayGroupFirst;
	/* First pgproc waiting for group transaction status update */

	/* 等待组事务状态更新的第一个 pgproc。 */
	pg_atomic_uint32 clogGroupFirst;

	/*
	 * Current slot numbers of some auxiliary processes. There can be only one
	 * of each of these running at a time.
	 */

	/*
	 * 某些辅助进程当前的槽位编号。每种进程一次只能运行一个。
	 */
	ProcNumber	walwriterProc;
	ProcNumber	checkpointerProc;

	/* Current shared estimate of appropriate spins_per_delay value */

	/* 当前合适 spins_per_delay 值的共享估计。 */
	int			spins_per_delay;
	/* Buffer id of the buffer that Startup process waits for pin on, or -1 */

	/* Startup 进程等待固定的缓冲区 ID，或为 -1。 */
	int			startupBufferPinWaitBufId;
} PROC_HDR;

extern PGDLLIMPORT PROC_HDR *ProcGlobal;

extern PGDLLIMPORT PGPROC *PreparedXactProcs;

/*
 * Accessors for getting PGPROC given a ProcNumber and vice versa.
 */

/*
 * 根据 ProcNumber 获取 PGPROC 及反向获取的访问器。
 */
#define GetPGProcByNumber(n) (&ProcGlobal->allProcs[(n)])
#define GetNumberFromPGProc(proc) ((proc) - &ProcGlobal->allProcs[0])

/*
 * We set aside some extra PGPROC structures for "special worker" processes,
 * which are full-fledged backends (they can run transactions)
 * but are unique animals that there's never more than one of.
 * Currently there are two such processes: the autovacuum launcher
 * and the slotsync worker.
 */

/*
 * 为“特殊工作进程”预留额外 PGPROC 结构体。它们是完整后端（可运行事务），但每种都是
 * 永不超过一个的特殊实例。当前有两种：自动清理启动器和 slotsync 工作进程。
 */
#define NUM_SPECIAL_WORKER_PROCS	2

/*
 * We set aside some extra PGPROC structures for auxiliary processes,
 * ie things that aren't full-fledged backends (they cannot run transactions
 * or take heavyweight locks) but need shmem access.
 *
 * Background writer, checkpointer, WAL writer, WAL summarizer, and archiver
 * run during normal operation.  Startup process and WAL receiver also consume
 * 2 slots, but WAL writer is launched only after startup has exited, so we
 * only need 6 slots.
 */

/*
 * 为辅助进程预留额外 PGPROC 结构体，即非完整后端（不能运行事务或获取重量级锁）但需要访问
 * 共享内存的进程。
 *
 * 后台写入器、检查点进程、WAL 写入器、WAL 汇总器和归档器在正常运行期间执行。启动进程和
 * WAL 接收器还消耗 2 个槽位，但 WAL 写入器仅在启动进程退出后启动，因此只需 6 个槽位。
 */
#define MAX_IO_WORKERS          32
#define NUM_AUXILIARY_PROCS		(6 + MAX_IO_WORKERS)


/* configurable options */

/* 可配置选项。 */
extern PGDLLIMPORT int DeadlockTimeout;
extern PGDLLIMPORT int StatementTimeout;
extern PGDLLIMPORT int LockTimeout;
extern PGDLLIMPORT int IdleInTransactionSessionTimeout;
extern PGDLLIMPORT int TransactionTimeout;
extern PGDLLIMPORT int IdleSessionTimeout;
extern PGDLLIMPORT bool log_lock_waits;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT slock_t *ProcStructLock;
extern PGDLLIMPORT PGPROC *AuxiliaryProcs;
#endif


/*
 * Function Prototypes
 */

/*
 * 函数原型。
 */
/*
 * Returns the number of semaphores required by ProcGlobal.
 */

/*
 * 返回 ProcGlobal 所需的信号量数量。
 */
extern int	ProcGlobalSemas(void);
/*
 * Returns the shared-memory size needed for ProcGlobal state.
 */

/*
 * 返回 ProcGlobal 状态所需的共享内存大小。
 */
extern Size ProcGlobalShmemSize(void);
/*
 * Initializes global process structures and free-process lists.
 */

/*
 * 初始化全局进程结构和空闲进程列表。
 */
extern void InitProcGlobal(void);
/*
 * Initializes a regular backend's PGPROC and attaches it to shared state.
 */

/*
 * 初始化常规后端的 PGPROC 并将其附加到共享状态。
 */
extern void InitProcess(void);
/*
 * Completes backend process initialization after early shared-state setup.
 */

/*
 * 在早期共享状态设置后完成后端进程初始化。
 */
extern void InitProcessPhase2(void);
/*
 * Initializes an auxiliary process's PGPROC state.
 */

/*
 * 初始化辅助进程的 PGPROC 状态。
 */
extern void InitAuxiliaryProcess(void);

/*
 * Records the buffer ID on which Startup waits for a pin.
 */

/*
 * 记录 Startup 等待固定的缓冲区 ID。
 */
extern void SetStartupBufferPinWaitBufId(int bufid);
/*
 * Returns the buffer ID on which Startup currently waits for a pin.
 */

/*
 * 返回 Startup 当前等待固定的缓冲区 ID。
 */
extern int	GetStartupBufferPinWaitBufId(void);

/*
 * Tests whether at least N free PGPROC entries are available.
 */

/*
 * 检查是否至少有 N 个空闲 PGPROC 条目可用。
 */
extern bool HaveNFreeProcs(int n, int *nfree);
/*
 * Releases locks held by the current process at transaction completion.
 */

/*
 * 在事务结束时释放当前进程持有的锁。
 */
extern void ProcReleaseLocks(bool isCommit);

/*
 * Joins a lock wait queue or reports whether waiting is required.
 */

/*
 * 加入锁等待队列，或报告是否需要等待。
 */
extern ProcWaitStatus JoinWaitQueue(LOCALLOCK *locallock,
									LockMethod lockMethodTable, bool dontWait);
/*
 * Sleeps while waiting for a local lock request to be granted.
 */

/*
 * 在等待本地锁请求被授予时休眠。
 */
extern ProcWaitStatus ProcSleep(LOCALLOCK *locallock);
/*
 * Wakes a waiting process and supplies its final wait status.
 */

/*
 * 唤醒等待进程并提供其最终等待状态。
 */
extern void ProcWakeup(PGPROC *proc, ProcWaitStatus waitStatus);
/*
 * Wakes eligible waiters after a lock state changes.
 */

/*
 * 在锁状态变化后唤醒符合条件的等待者。
 */
extern void ProcLockWakeup(LockMethod lockMethodTable, LOCK *lock);
/*
 * Checks whether a waiting process should be alerted about deadlock detection.
 */

/*
 * 检查是否应向等待进程发出死锁检测提醒。
 */
extern void CheckDeadLockAlert(void);
/*
 * Cleans up process lock-wait state after a lock acquisition error.
 */

/*
 * 在锁获取错误后清理进程锁等待状态。
 */
extern void LockErrorCleanup(void);
/*
 * Collects lock holders and waiters for a local lock into diagnostic buffers.
 */

/*
 * 将本地锁的持有者和等待者收集到诊断缓冲区。
 */
extern void GetLockHoldersAndWaiters(LOCALLOCK *locallock,
									 StringInfo lock_holders_sbuf,
									 StringInfo lock_waiters_sbuf,
									 int *lockHoldersNum);

/*
 * Waits for a process signal while publishing its wait event.
 */

/*
 * 在发布等待事件的同时等待进程信号。
 */
extern void ProcWaitForSignal(uint32 wait_event_info);
/*
 * Sends a wakeup signal to the process identified by a process number.
 */

/*
 * 向由进程编号标识的进程发送唤醒信号。
 */
extern void ProcSendSignal(ProcNumber procNumber);

/*
 * Finds an auxiliary process's PGPROC entry by PID.
 */

/*
 * 按 PID 查找辅助进程的 PGPROC 条目。
 */
extern PGPROC *AuxiliaryPidGetProc(int pid);

/*
 * Makes the current process the leader of a lock group.
 */

/*
 * 使当前进程成为锁组领导者。
 */
extern void BecomeLockGroupLeader(void);
/*
 * Joins a process to a lock group led by the supplied backend.
 */

/*
 * 将进程加入由给定后端领导的锁组。
 */
extern bool BecomeLockGroupMember(PGPROC *leader, int pid);

#endif							/* _PROC_H_ */
