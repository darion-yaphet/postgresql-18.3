/*-------------------------------------------------------------------------
 *
 * lwlock.h
 *	  Lightweight lock manager
 *
 *	  轻量级锁管理器。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lwlock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LWLOCK_H
#define LWLOCK_H

#ifdef FRONTEND
#error "lwlock.h may not be included from frontend code"
#endif

#include "port/atomics.h"
#include "storage/lwlocknames.h"
#include "storage/proclist_types.h"

struct PGPROC;

/* what state of the wait process is a backend in */

/* 后端中的等待进程处于何种状态。 */
typedef enum LWLockWaitState
{
	LW_WS_NOT_WAITING,			/* not currently waiting / woken up */

	/* 当前未等待或已被唤醒。 */
	LW_WS_WAITING,				/* currently waiting */

	/* 当前正在等待。 */
	LW_WS_PENDING_WAKEUP,		/* removed from waitlist, but not yet
								 * signalled */

	/* 已从等待列表移除，但尚未收到信号。 */
}			LWLockWaitState;

/*
 * Code outside of lwlock.c should not manipulate the contents of this
 * structure directly, but we have to declare it here to allow LWLocks to be
 * incorporated into other data structures.
 */

/* lwlock.c 外部的代码不应直接操作该结构内容，但为允许将 LWLock 嵌入其他数据结构，必须在此声明它。 */
typedef struct LWLock
{
	uint16		tranche;		/* tranche ID */

	/* tranche ID。 */
	pg_atomic_uint32 state;		/* state of exclusive/nonexclusive lockers */

	/* 排他和非排他持锁者的状态。 */
	proclist_head waiters;		/* list of waiting PGPROCs */

	/* 等待中的 PGPROC 列表。 */
#ifdef LOCK_DEBUG
	pg_atomic_uint32 nwaiters;	/* number of waiters */

	/* 等待者数量。 */
	struct PGPROC *owner;		/* last exclusive owner of the lock */

	/* 锁的最后一个排他所有者。 */
#endif
} LWLock;

/*
 * In most cases, it's desirable to force each tranche of LWLocks to be aligned
 * on a cache line boundary and make the array stride a power of 2.  This saves
 * a few cycles in indexing, but more importantly ensures that individual
 * LWLocks don't cross cache line boundaries.  This reduces cache contention
 * problems, especially on AMD Opterons.  In some cases, it's useful to add
 * even more padding so that each LWLock takes up an entire cache line; this is
 * useful, for example, in the main LWLock array, where the overall number of
 * locks is small but some are heavily contended.
 */

/* 大多数情况下，应强制每个 LWLock tranche 按缓存行边界对齐，并使数组步幅为 2 的幂。这样可节省少量
 * 索引周期，更重要的是确保单个 LWLock 不跨越缓存行边界，从而降低缓存争用。某些情况下需要更多填充，
 * 使每个 LWLock 占满一个缓存行；例如主 LWLock 数组中的锁总数较少但部分竞争激烈。 */
#define LWLOCK_PADDED_SIZE	PG_CACHE_LINE_SIZE

StaticAssertDecl(sizeof(LWLock) <= LWLOCK_PADDED_SIZE,
				 "Miscalculated LWLock padding");

/* LWLock, padded to a full cache line size */

/* 填充到完整缓存行大小的 LWLock。 */
typedef union LWLockPadded
{
	LWLock		lock;
	char		pad[LWLOCK_PADDED_SIZE];
} LWLockPadded;

extern PGDLLIMPORT LWLockPadded *MainLWLockArray;

/* struct for storing named tranche information */

/* 用于存储具名 tranche 信息的结构。 */
typedef struct NamedLWLockTranche
{
	int			trancheId;
	char	   *trancheName;
} NamedLWLockTranche;

extern PGDLLIMPORT NamedLWLockTranche *NamedLWLockTrancheArray;
extern PGDLLIMPORT int NamedLWLockTrancheRequests;

/*
 * It's a bit odd to declare NUM_BUFFER_PARTITIONS and NUM_LOCK_PARTITIONS
 * here, but we need them to figure out offsets within MainLWLockArray, and
 * having this file include lock.h or bufmgr.h would be backwards.
 */

/* 在此声明 NUM_BUFFER_PARTITIONS 和 NUM_LOCK_PARTITIONS 看起来有些奇怪，但计算 MainLWLockArray
 * 内的偏移量需要它们，而让本文件包含 lock.h 或 bufmgr.h 会造成反向依赖。 */

/* Number of partitions of the shared buffer mapping hashtable */

/* 共享缓冲区映射哈希表的分区数。 */
#define NUM_BUFFER_PARTITIONS  128

/* Number of partitions the shared lock tables are divided into */

/* 共享锁表划分出的分区数。 */
#define LOG2_NUM_LOCK_PARTITIONS  4
#define NUM_LOCK_PARTITIONS  (1 << LOG2_NUM_LOCK_PARTITIONS)

/* Number of partitions the shared predicate lock tables are divided into */

/* 共享谓词锁表划分出的分区数。 */
#define LOG2_NUM_PREDICATELOCK_PARTITIONS  4
#define NUM_PREDICATELOCK_PARTITIONS  (1 << LOG2_NUM_PREDICATELOCK_PARTITIONS)

/* Offsets for various chunks of preallocated lwlocks. */

/* 预分配 lwlock 各区域的偏移量。 */
#define BUFFER_MAPPING_LWLOCK_OFFSET	NUM_INDIVIDUAL_LWLOCKS
#define LOCK_MANAGER_LWLOCK_OFFSET		\
	(BUFFER_MAPPING_LWLOCK_OFFSET + NUM_BUFFER_PARTITIONS)
#define PREDICATELOCK_MANAGER_LWLOCK_OFFSET \
	(LOCK_MANAGER_LWLOCK_OFFSET + NUM_LOCK_PARTITIONS)
#define NUM_FIXED_LWLOCKS \
	(PREDICATELOCK_MANAGER_LWLOCK_OFFSET + NUM_PREDICATELOCK_PARTITIONS)

typedef enum LWLockMode
{
	LW_EXCLUSIVE,
	LW_SHARED,
	LW_WAIT_UNTIL_FREE,			/* A special mode used in PGPROC->lwWaitMode,
								 * when waiting for lock to become free. Not
								 * to be used as LWLockAcquire argument */

	/* PGPROC->lwWaitMode 在等待锁变为空闲时使用的特殊模式，不可作为 LWLockAcquire 参数。 */
} LWLockMode;


#ifdef LOCK_DEBUG
extern PGDLLIMPORT bool Trace_lwlocks;
#endif

/* Acquires an LWLock in the requested mode, waiting until it is available.
 *
 * 请求的模式获取 LWLock，并等待其可用。
 */
extern bool LWLockAcquire(LWLock *lock, LWLockMode mode);
/* Attempts to acquire an LWLock without waiting.
 *
 * 尝试不等待地获取 LWLock。
 */
extern bool LWLockConditionalAcquire(LWLock *lock, LWLockMode mode);
/* Acquires an LWLock if free, or arranges to wait for its release.
 *
 * 若 LWLock 空闲则获取它，否则安排等待其释放。
 */
extern bool LWLockAcquireOrWait(LWLock *lock, LWLockMode mode);
/* Releases an LWLock held by the current backend and wakes waiters.
 *
 * 释放当前后端持有的 LWLock，并唤醒等待者。
 */
extern void LWLockRelease(LWLock *lock);
/* Releases an LWLock after atomically updating an associated variable.
 *
 * 在原子更新关联变量后释放 LWLock。
 */
extern void LWLockReleaseClearVar(LWLock *lock, pg_atomic_uint64 *valptr, uint64 val);
/* Releases every LWLock held by the current backend.
 *
 * 释放当前后端持有的每个 LWLock。
 */
extern void LWLockReleaseAll(void);
/* Marks an LWLock as no longer owned without releasing its shared state.
 *
 * 标记 LWLock 不再由当前后端拥有，但不释放其共享状态。
 */
extern void LWLockDisown(LWLock *lock);
/* Releases an LWLock previously disowned by the current backend.
 *
 * 释放当前后端先前放弃所有权的 LWLock。
 */
extern void LWLockReleaseDisowned(LWLock *lock, LWLockMode mode);
/* Invokes a callback for each LWLock held by the current backend.
 *
 * 为当前后端持有的每个 LWLock 调用回调函数。
 */
extern void ForEachLWLockHeldByMe(void (*callback) (LWLock *, LWLockMode, void *),
								  void *context);
/* Reports whether the current backend holds an LWLock.
 *
 * 报告当前后端是否持有该 LWLock。
 */
extern bool LWLockHeldByMe(LWLock *lock);
/* Reports whether the current backend holds any LWLock in a strided array.
 *
 * 报告当前后端是否持有跨步数组中的任一 LWLock。
 */
extern bool LWLockAnyHeldByMe(LWLock *lock, int nlocks, size_t stride);
/* Reports whether the current backend holds an LWLock in a given mode.
 *
 * 报告当前后端是否以给定模式持有 LWLock。
 */
extern bool LWLockHeldByMeInMode(LWLock *lock, LWLockMode mode);

/* Waits for an associated variable to change while coordinating with an LWLock.
 *
 * 在与 LWLock 协调时等待关联变量发生变化。
 */
extern bool LWLockWaitForVar(LWLock *lock, pg_atomic_uint64 *valptr, uint64 oldval, uint64 *newval);
/* Updates an associated variable and wakes LWLock variable waiters.
 *
 * 更新关联变量并唤醒 LWLock 变量等待者。
 */
extern void LWLockUpdateVar(LWLock *lock, pg_atomic_uint64 *valptr, uint64 val);

/* Returns the shared-memory size required for LWLock state.
 *
 * 返回 LWLock 状态所需的共享内存大小。
 */
extern Size LWLockShmemSize(void);
/* Creates the fixed LWLocks in shared memory.
 *
 * 在共享内存中创建固定 LWLock。
 */
extern void CreateLWLocks(void);
/* Initializes backend access to the LWLock subsystem.
 *
 * 初始化后端对 LWLock 子系统的访问。
 */
extern void InitLWLockAccess(void);

/* Returns the descriptive identifier for an LWLock wait event.
 *
 * 返回 LWLock 等待事件的描述性标识符。
 */
extern const char *GetLWLockIdentifier(uint32 classId, uint16 eventId);

/*
 * Extensions (or core code) can obtain an LWLocks by calling
 * RequestNamedLWLockTranche() during postmaster startup.  Subsequently,
 * call GetNamedLWLockTranche() to obtain a pointer to an array containing
 * the number of LWLocks requested.
 */

/* 扩展（或核心代码）可在 postmaster 启动时调用 RequestNamedLWLockTranche() 获取 LWLock tranche，
 * 随后调用 GetNamedLWLockTranche() 获取包含请求数量 LWLock 的数组指针。 */
/* Requests a named tranche containing a specified number of LWLocks.
 *
 * 请求包含指定数量 LWLock 的具名 tranche。
 */
extern void RequestNamedLWLockTranche(const char *tranche_name, int num_lwlocks);
/* Returns the LWLock array allocated for a named tranche.
 *
 * 返回为具名 tranche 分配的 LWLock 数组。
 */
extern LWLockPadded *GetNamedLWLockTranche(const char *tranche_name);

/*
 * There is another, more flexible method of obtaining lwlocks. First, call
 * LWLockNewTrancheId just once to obtain a tranche ID; this allocates from
 * a shared counter.  Next, each individual process using the tranche should
 * call LWLockRegisterTranche() to associate that tranche ID with a name.
 * Finally, LWLockInitialize should be called just once per lwlock, passing
 * the tranche ID as an argument.
 *
 * It may seem strange that each process using the tranche must register it
 * separately, but dynamic shared memory segments aren't guaranteed to be
 * mapped at the same address in all coordinating backends, so storing the
 * registration in the main shared memory segment wouldn't work for that case.
 */

/* 还有一种更灵活的获得 lwlock 的方法。先仅调用一次 LWLockNewTrancheId 获得 tranche ID（从共享计数器
 * 分配）；随后每个使用该 tranche 的进程调用 LWLockRegisterTranche() 将 ID 与名称关联；最后每个
 * lwlock 仅调用一次 LWLockInitialize，并传入 tranche ID。每个进程都必须单独注册，因为动态共享内存段
 * 不保证在协调后端中映射到同一地址，将注册存入主共享内存段无法支持该情形。 */
/* Allocates and returns a new user-defined LWLock tranche ID.
 *
 * 分配并返回新的用户定义 LWLock tranche ID。
 */
extern int	LWLockNewTrancheId(void);
/* Associates a tranche ID with its descriptive name in one process.
 *
 * 在一个进程中将 tranche ID 与其描述名称关联。
 */
extern void LWLockRegisterTranche(int tranche_id, const char *tranche_name);
/* Initializes one LWLock with its tranche identity.
 *
 * 使用 tranche 身份初始化一个 LWLock。
 */
extern void LWLockInitialize(LWLock *lock, int tranche_id);

/*
 * Every tranche ID less than NUM_INDIVIDUAL_LWLOCKS is reserved; also,
 * we reserve additional tranche IDs for builtin tranches not included in
 * the set of individual LWLocks.  A call to LWLockNewTrancheId will never
 * return a value less than LWTRANCHE_FIRST_USER_DEFINED.
 */

/* 所有小于 NUM_INDIVIDUAL_LWLOCKS 的 tranche ID 都被保留；还会为未包含在独立 LWLock 集中的内建
 * tranche 保留额外 ID。LWLockNewTrancheId 不会返回小于 LWTRANCHE_FIRST_USER_DEFINED 的值。 */
typedef enum BuiltinTrancheIds
{
	LWTRANCHE_XACT_BUFFER = NUM_INDIVIDUAL_LWLOCKS,
	LWTRANCHE_COMMITTS_BUFFER,
	LWTRANCHE_SUBTRANS_BUFFER,
	LWTRANCHE_MULTIXACTOFFSET_BUFFER,
	LWTRANCHE_MULTIXACTMEMBER_BUFFER,
	LWTRANCHE_NOTIFY_BUFFER,
	LWTRANCHE_SERIAL_BUFFER,
	LWTRANCHE_WAL_INSERT,
	LWTRANCHE_BUFFER_CONTENT,
	LWTRANCHE_REPLICATION_ORIGIN_STATE,
	LWTRANCHE_REPLICATION_SLOT_IO,
	LWTRANCHE_LOCK_FASTPATH,
	LWTRANCHE_BUFFER_MAPPING,
	LWTRANCHE_LOCK_MANAGER,
	LWTRANCHE_PREDICATE_LOCK_MANAGER,
	LWTRANCHE_PARALLEL_HASH_JOIN,
	LWTRANCHE_PARALLEL_BTREE_SCAN,
	LWTRANCHE_PARALLEL_QUERY_DSA,
	LWTRANCHE_PER_SESSION_DSA,
	LWTRANCHE_PER_SESSION_RECORD_TYPE,
	LWTRANCHE_PER_SESSION_RECORD_TYPMOD,
	LWTRANCHE_SHARED_TUPLESTORE,
	LWTRANCHE_SHARED_TIDBITMAP,
	LWTRANCHE_PARALLEL_APPEND,
	LWTRANCHE_PER_XACT_PREDICATE_LIST,
	LWTRANCHE_PGSTATS_DSA,
	LWTRANCHE_PGSTATS_HASH,
	LWTRANCHE_PGSTATS_DATA,
	LWTRANCHE_LAUNCHER_DSA,
	LWTRANCHE_LAUNCHER_HASH,
	LWTRANCHE_DSM_REGISTRY_DSA,
	LWTRANCHE_DSM_REGISTRY_HASH,
	LWTRANCHE_COMMITTS_SLRU,
	LWTRANCHE_MULTIXACTMEMBER_SLRU,
	LWTRANCHE_MULTIXACTOFFSET_SLRU,
	LWTRANCHE_NOTIFY_SLRU,
	LWTRANCHE_SERIAL_SLRU,
	LWTRANCHE_SUBTRANS_SLRU,
	LWTRANCHE_XACT_SLRU,
	LWTRANCHE_PARALLEL_VACUUM_DSA,
	LWTRANCHE_AIO_URING_COMPLETION,
	LWTRANCHE_FIRST_USER_DEFINED,
}			BuiltinTrancheIds;

/*
 * Prior to PostgreSQL 9.4, we used an enum type called LWLockId to refer
 * to LWLocks.  New code should instead use LWLock *.  However, for the
 * convenience of third-party code, we include the following typedef.
 */

/* PostgreSQL 9.4 之前使用名为 LWLockId 的枚举类型引用 LWLock。新代码应改用 LWLock *；为方便第三方
 * 代码，仍保留以下 typedef。 */
typedef LWLock *LWLockId;

#endif							/* LWLOCK_H */
