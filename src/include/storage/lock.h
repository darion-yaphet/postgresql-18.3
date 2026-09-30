/*-------------------------------------------------------------------------
 *
 * lock.h
 *	  POSTGRES low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lock.h
 *
 *-------------------------------------------------------------------------
 */

/* PostgreSQL 底层锁机制。 */
#ifndef LOCK_H_
#define LOCK_H_

#ifdef FRONTEND
#error "lock.h may not be included from frontend code"
#endif

#include "lib/ilist.h"
#include "storage/lockdefs.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

/* struct PGPROC is declared in proc.h, but must forward-reference it */

/* PGPROC 在 proc.h 中声明，但此处必须前向引用它。 */
typedef struct PGPROC PGPROC;

/* GUC variables */

/* GUC 变量。 */
extern PGDLLIMPORT int max_locks_per_xact;
extern PGDLLIMPORT bool log_lock_failures;

#ifdef LOCK_DEBUG
extern PGDLLIMPORT int Trace_lock_oidmin;
extern PGDLLIMPORT bool Trace_locks;
extern PGDLLIMPORT bool Trace_userlocks;
extern PGDLLIMPORT int Trace_lock_table;
extern PGDLLIMPORT bool Debug_deadlocks;
#endif							/* LOCK_DEBUG */


/*
 * Top-level transactions are identified by VirtualTransactionIDs comprising
 * PGPROC fields procNumber and lxid.  For recovered prepared transactions, the
 * LocalTransactionId is an ordinary XID; LOCKTAG_VIRTUALTRANSACTION never
 * refers to that kind.  These are guaranteed unique over the short term, but
 * will be reused after a database restart or XID wraparound; hence they
 * should never be stored on disk.
 *
 * Note that struct VirtualTransactionId can not be assumed to be atomically
 * assignable as a whole.  However, type LocalTransactionId is assumed to
 * be atomically assignable, and the proc number doesn't change often enough
 * to be a problem, so we can fetch or assign the two fields separately.
 * We deliberately refrain from using the struct within PGPROC, to prevent
 * coding errors from trying to use struct assignment with it; instead use
 * GET_VXID_FROM_PGPROC().
 */

/* 顶层事务由包含 PGPROC 字段 procNumber 和 lxid 的 VirtualTransactionID 标识。恢复的预备事务使用
 * 普通 XID，LOCKTAG_VIRTUALTRANSACTION 不会引用该类型。这些标识短期唯一，但会在数据库重启或 XID
 * 回绕后复用，因此不得持久化到磁盘。VirtualTransactionId 结构整体不保证原子赋值；但
 * LocalTransactionId 可原子赋值，procNumber 很少变化，所以分别访问两个字段。为避免将它用于 PGPROC
 * 结构赋值，应改用 GET_VXID_FROM_PGPROC()。 */
typedef struct
{
	ProcNumber	procNumber;		/* proc number of the PGPROC */

	/* PGPROC 的进程号。 */
	LocalTransactionId localTransactionId;	/* lxid from PGPROC */

	/* 来自 PGPROC 的 lxid。 */
} VirtualTransactionId;

#define InvalidLocalTransactionId		0
#define LocalTransactionIdIsValid(lxid) ((lxid) != InvalidLocalTransactionId)
#define VirtualTransactionIdIsValid(vxid) \
	(LocalTransactionIdIsValid((vxid).localTransactionId))
#define VirtualTransactionIdIsRecoveredPreparedXact(vxid) \
	((vxid).procNumber == INVALID_PROC_NUMBER)
#define VirtualTransactionIdEquals(vxid1, vxid2) \
	((vxid1).procNumber == (vxid2).procNumber && \
	 (vxid1).localTransactionId == (vxid2).localTransactionId)
#define SetInvalidVirtualTransactionId(vxid) \
	((vxid).procNumber = INVALID_PROC_NUMBER, \
	 (vxid).localTransactionId = InvalidLocalTransactionId)
#define GET_VXID_FROM_PGPROC(vxid_dst, proc) \
	((vxid_dst).procNumber = (proc).vxid.procNumber, \
		 (vxid_dst).localTransactionId = (proc).vxid.lxid)

/* MAX_LOCKMODES cannot be larger than the # of bits in LOCKMASK */

/* MAX_LOCKMODES 不能大于 LOCKMASK 中的位数。 */
#define MAX_LOCKMODES		10

#define LOCKBIT_ON(lockmode) (1 << (lockmode))
#define LOCKBIT_OFF(lockmode) (~(1 << (lockmode)))


/*
 * This data structure defines the locking semantics associated with a
 * "lock method".  The semantics specify the meaning of each lock mode
 * (by defining which lock modes it conflicts with).
 * All of this data is constant and is kept in const tables.
 *
 * numLockModes -- number of lock modes (READ,WRITE,etc) that
 *		are defined in this lock method.  Must be less than MAX_LOCKMODES.
 *
 * conflictTab -- this is an array of bitmasks showing lock
 *		mode conflicts.  conflictTab[i] is a mask with the j-th bit
 *		turned on if lock modes i and j conflict.  Lock modes are
 *		numbered 1..numLockModes; conflictTab[0] is unused.
 *
 * lockModeNames -- ID strings for debug printouts.
 *
 * trace_flag -- pointer to GUC trace flag for this lock method.  (The
 * GUC variable is not constant, but we use "const" here to denote that
 * it can't be changed through this reference.)
 */

/* 此数据结构定义与“锁方法”关联的锁语义。语义通过定义哪些锁模式冲突来说明每种锁模式的含义；这些
 * 数据均为常量并保存在常量表中。numLockModes 是锁方法中定义的模式数，conflictTab 是表示模式冲突
 * 的位掩码数组，lockModeNames 用于调试输出，trace_flag 指向该锁方法的 GUC 跟踪标志。 */
typedef struct LockMethodData
{
	int			numLockModes;
	const LOCKMASK *conflictTab;
	const char *const *lockModeNames;
	const bool *trace_flag;
} LockMethodData;

typedef const LockMethodData *LockMethod;

/*
 * Lock methods are identified by LOCKMETHODID.  (Despite the declaration as
 * uint16, we are constrained to 256 lockmethods by the layout of LOCKTAG.)
 */

/* 锁方法由 LOCKMETHODID 标识。尽管声明为 uint16，LOCKTAG 的布局将锁方法限制为 256 个。 */
typedef uint16 LOCKMETHODID;

/* These identify the known lock methods */

/* 这些值标识已知锁方法。 */
#define DEFAULT_LOCKMETHOD	1
#define USER_LOCKMETHOD		2

/*
 * LOCKTAG is the key information needed to look up a LOCK item in the
 * lock hashtable.  A LOCKTAG value uniquely identifies a lockable object.
 *
 * The LockTagType enum defines the different kinds of objects we can lock.
 * We can handle up to 256 different LockTagTypes.
 */

/* LOCKTAG 是在锁哈希表查找 LOCK 项所需的键，唯一标识可锁定对象。LockTagType 枚举定义可锁定的
 * 对象类型，最多支持 256 种。 */
typedef enum LockTagType
{
	LOCKTAG_RELATION,			/* whole relation */
	LOCKTAG_RELATION_EXTEND,	/* the right to extend a relation */
	LOCKTAG_DATABASE_FROZEN_IDS,	/* pg_database.datfrozenxid */
	LOCKTAG_PAGE,				/* one page of a relation */
	LOCKTAG_TUPLE,				/* one physical tuple */
	LOCKTAG_TRANSACTION,		/* transaction (for waiting for xact done) */
	LOCKTAG_VIRTUALTRANSACTION, /* virtual transaction (ditto) */
	LOCKTAG_SPECULATIVE_TOKEN,	/* speculative insertion Xid and token */
	LOCKTAG_OBJECT,				/* non-relation database object */
	LOCKTAG_USERLOCK,			/* reserved for old contrib/userlock code */
	LOCKTAG_ADVISORY,			/* advisory user locks */
	LOCKTAG_APPLY_TRANSACTION,	/* transaction being applied on a logical
								 * replication subscriber */
} LockTagType;

#define LOCKTAG_LAST_TYPE	LOCKTAG_APPLY_TRANSACTION

extern PGDLLIMPORT const char *const LockTagTypeNames[];

/*
 * The LOCKTAG struct is defined with malice aforethought to fit into 16
 * bytes with no padding.  Note that this would need adjustment if we were
 * to widen Oid, BlockNumber, or TransactionId to more than 32 bits.
 *
 * We include lockmethodid in the locktag so that a single hash table in
 * shared memory can store locks of different lockmethods.
 */

/* LOCKTAG 经过精心设计，可在无填充的情况下容纳于 16 字节；若 Oid、BlockNumber 或 TransactionId
 * 扩展到超过 32 位，则需调整。lockmethodid 包含在 locktag 中，使一个共享内存哈希表能存储不同锁方法。 */
typedef struct LOCKTAG
{
	uint32		locktag_field1; /* a 32-bit ID field */
	uint32		locktag_field2; /* a 32-bit ID field */
	uint32		locktag_field3; /* a 32-bit ID field */
	uint16		locktag_field4; /* a 16-bit ID field */
	uint8		locktag_type;	/* see enum LockTagType */
	uint8		locktag_lockmethodid;	/* lockmethod indicator */
} LOCKTAG;

/*
 * These macros define how we map logical IDs of lockable objects into
 * the physical fields of LOCKTAG.  Use these to set up LOCKTAG values,
 * rather than accessing the fields directly.  Note multiple eval of target!
 */

/* 这些宏定义如何将可锁定对象的逻辑 ID 映射到 LOCKTAG 的物理字段。应使用它们建立 LOCKTAG，而不是
 * 直接访问字段；请注意目标会被多次求值。 */

/* ID info for a relation is DB OID + REL OID; DB OID = 0 if shared */
#define SET_LOCKTAG_RELATION(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* same ID info as RELATION */
#define SET_LOCKTAG_RELATION_EXTEND(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_RELATION_EXTEND, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* ID info for frozen IDs is DB OID */
#define SET_LOCKTAG_DATABASE_FROZEN_IDS(locktag,dboid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = 0, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_DATABASE_FROZEN_IDS, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* ID info for a page is RELATION info + BlockNumber */
#define SET_LOCKTAG_PAGE(locktag,dboid,reloid,blocknum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_PAGE, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* ID info for a tuple is PAGE info + OffsetNumber */
#define SET_LOCKTAG_TUPLE(locktag,dboid,reloid,blocknum,offnum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = (offnum), \
	 (locktag).locktag_type = LOCKTAG_TUPLE, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* ID info for a transaction is its TransactionId */
#define SET_LOCKTAG_TRANSACTION(locktag,xid) \
	((locktag).locktag_field1 = (xid), \
	 (locktag).locktag_field2 = 0, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_TRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/* ID info for a virtual transaction is its VirtualTransactionId */
#define SET_LOCKTAG_VIRTUALTRANSACTION(locktag,vxid) \
	((locktag).locktag_field1 = (vxid).procNumber, \
	 (locktag).locktag_field2 = (vxid).localTransactionId, \
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_VIRTUALTRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/*
 * ID info for a speculative insert is TRANSACTION info +
 * its speculative insert counter.
 */
#define SET_LOCKTAG_SPECULATIVE_INSERTION(locktag,xid,token) \
	((locktag).locktag_field1 = (xid), \
	 (locktag).locktag_field2 = (token),		\
	 (locktag).locktag_field3 = 0, \
	 (locktag).locktag_field4 = 0, \
	 (locktag).locktag_type = LOCKTAG_SPECULATIVE_TOKEN, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/*
 * ID info for an object is DB OID + CLASS OID + OBJECT OID + SUBID
 *
 * Note: object ID has same representation as in pg_depend and
 * pg_description, but notice that we are constraining SUBID to 16 bits.
 * Also, we use DB OID = 0 for shared objects such as tablespaces.
 */
#define SET_LOCKTAG_OBJECT(locktag,dboid,classoid,objoid,objsubid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (classoid), \
	 (locktag).locktag_field3 = (objoid), \
	 (locktag).locktag_field4 = (objsubid), \
	 (locktag).locktag_type = LOCKTAG_OBJECT, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_ADVISORY(locktag,id1,id2,id3,id4) \
	((locktag).locktag_field1 = (id1), \
	 (locktag).locktag_field2 = (id2), \
	 (locktag).locktag_field3 = (id3), \
	 (locktag).locktag_field4 = (id4), \
	 (locktag).locktag_type = LOCKTAG_ADVISORY, \
	 (locktag).locktag_lockmethodid = USER_LOCKMETHOD)

/*
 * ID info for a remote transaction on a logical replication subscriber is: DB
 * OID + SUBSCRIPTION OID + TRANSACTION ID + OBJID
 */
#define SET_LOCKTAG_APPLY_TRANSACTION(locktag,dboid,suboid,xid,objid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (suboid), \
	 (locktag).locktag_field3 = (xid), \
	 (locktag).locktag_field4 = (objid), \
	 (locktag).locktag_type = LOCKTAG_APPLY_TRANSACTION, \
	 (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

/*
 * Per-locked-object lock information:
 *
 * tag -- uniquely identifies the object being locked
 * grantMask -- bitmask for all lock types currently granted on this object.
 * waitMask -- bitmask for all lock types currently awaited on this object.
 * procLocks -- list of PROCLOCK objects for this lock.
 * waitProcs -- queue of processes waiting for this lock.
 * requested -- count of each lock type currently requested on the lock
 *		(includes requests already granted!!).
 * nRequested -- total requested locks of all types.
 * granted -- count of each lock type currently granted on the lock.
 * nGranted -- total granted locks of all types.
 *
 * Note: these counts count 1 for each backend.  Internally to a backend,
 * there may be multiple grabs on a particular lock, but this is not reflected
 * into shared memory.
 */
typedef struct LOCK
{
	/* hash key */
	LOCKTAG		tag;			/* unique identifier of lockable object */

	/* data */
	LOCKMASK	grantMask;		/* bitmask for lock types already granted */
	LOCKMASK	waitMask;		/* bitmask for lock types awaited */
	dlist_head	procLocks;		/* list of PROCLOCK objects assoc. with lock */
	dclist_head waitProcs;		/* list of PGPROC objects waiting on lock */
	int			requested[MAX_LOCKMODES];	/* counts of requested locks */
	int			nRequested;		/* total of requested[] array */
	int			granted[MAX_LOCKMODES]; /* counts of granted locks */
	int			nGranted;		/* total of granted[] array */
} LOCK;

#define LOCK_LOCKMETHOD(lock) ((LOCKMETHODID) (lock).tag.locktag_lockmethodid)
#define LOCK_LOCKTAG(lock) ((LockTagType) (lock).tag.locktag_type)


/*
 * We may have several different backends holding or awaiting locks
 * on the same lockable object.  We need to store some per-holder/waiter
 * information for each such holder (or would-be holder).  This is kept in
 * a PROCLOCK struct.
 *
 * PROCLOCKTAG is the key information needed to look up a PROCLOCK item in the
 * proclock hashtable.  A PROCLOCKTAG value uniquely identifies the combination
 * of a lockable object and a holder/waiter for that object.  (We can use
 * pointers here because the PROCLOCKTAG need only be unique for the lifespan
 * of the PROCLOCK, and it will never outlive the lock or the proc.)
 *
 * Internally to a backend, it is possible for the same lock to be held
 * for different purposes: the backend tracks transaction locks separately
 * from session locks.  However, this is not reflected in the shared-memory
 * state: we only track which backend(s) hold the lock.  This is OK since a
 * backend can never block itself.
 *
 * The holdMask field shows the already-granted locks represented by this
 * proclock.  Note that there will be a proclock object, possibly with
 * zero holdMask, for any lock that the process is currently waiting on.
 * Otherwise, proclock objects whose holdMasks are zero are recycled
 * as soon as convenient.
 *
 * releaseMask is workspace for LockReleaseAll(): it shows the locks due
 * to be released during the current call.  This must only be examined or
 * set by the backend owning the PROCLOCK.
 *
 * Each PROCLOCK object is linked into lists for both the associated LOCK
 * object and the owning PGPROC object.  Note that the PROCLOCK is entered
 * into these lists as soon as it is created, even if no lock has yet been
 * granted.  A PGPROC that is waiting for a lock to be granted will also be
 * linked into the lock's waitProcs queue.
 */
typedef struct PROCLOCKTAG
{
	/* NB: we assume this struct contains no padding! */
	LOCK	   *myLock;			/* link to per-lockable-object information */
	PGPROC	   *myProc;			/* link to PGPROC of owning backend */
} PROCLOCKTAG;

typedef struct PROCLOCK
{
	/* tag */
	PROCLOCKTAG tag;			/* unique identifier of proclock object */

	/* data */
	PGPROC	   *groupLeader;	/* proc's lock group leader, or proc itself */
	LOCKMASK	holdMask;		/* bitmask for lock types currently held */
	LOCKMASK	releaseMask;	/* bitmask for lock types to be released */
	dlist_node	lockLink;		/* list link in LOCK's list of proclocks */
	dlist_node	procLink;		/* list link in PGPROC's list of proclocks */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) \
	LOCK_LOCKMETHOD(*((proclock).tag.myLock))

/*
 * Each backend also maintains a local hash table with information about each
 * lock it is currently interested in.  In particular the local table counts
 * the number of times that lock has been acquired.  This allows multiple
 * requests for the same lock to be executed without additional accesses to
 * shared memory.  We also track the number of lock acquisitions per
 * ResourceOwner, so that we can release just those locks belonging to a
 * particular ResourceOwner.
 *
 * When holding a lock taken "normally", the lock and proclock fields always
 * point to the associated objects in shared memory.  However, if we acquired
 * the lock via the fast-path mechanism, the lock and proclock fields are set
 * to NULL, since there probably aren't any such objects in shared memory.
 * (If the lock later gets promoted to normal representation, we may eventually
 * update our locallock's lock/proclock fields after finding the shared
 * objects.)
 *
 * Caution: a locallock object can be left over from a failed lock acquisition
 * attempt.  In this case its lock/proclock fields are untrustworthy, since
 * the shared lock object is neither held nor awaited, and hence is available
 * to be reclaimed.  If nLocks > 0 then these pointers must either be valid or
 * NULL, but when nLocks == 0 they should be considered garbage.
 */
typedef struct LOCALLOCKTAG
{
	LOCKTAG		lock;			/* identifies the lockable object */
	LOCKMODE	mode;			/* lock mode for this table entry */
} LOCALLOCKTAG;

typedef struct LOCALLOCKOWNER
{
	/*
	 * Note: if owner is NULL then the lock is held on behalf of the session;
	 * otherwise it is held on behalf of my current transaction.
	 *
	 * Must use a forward struct reference to avoid circularity.
	 */
	struct ResourceOwnerData *owner;
	int64		nLocks;			/* # of times held by this owner */
} LOCALLOCKOWNER;

typedef struct LOCALLOCK
{
	/* tag */
	LOCALLOCKTAG tag;			/* unique identifier of locallock entry */

	/* data */
	uint32		hashcode;		/* copy of LOCKTAG's hash value */
	LOCK	   *lock;			/* associated LOCK object, if any */
	PROCLOCK   *proclock;		/* associated PROCLOCK object, if any */
	int64		nLocks;			/* total number of times lock is held */
	int			numLockOwners;	/* # of relevant ResourceOwners */
	int			maxLockOwners;	/* allocated size of array */
	LOCALLOCKOWNER *lockOwners; /* dynamically resizable array */
	bool		holdsStrongLockCount;	/* bumped FastPathStrongRelationLocks */
	bool		lockCleared;	/* we read all sinval msgs for lock */
} LOCALLOCK;

#define LOCALLOCK_LOCKMETHOD(llock) ((llock).tag.lock.locktag_lockmethodid)
#define LOCALLOCK_LOCKTAG(llock) ((LockTagType) (llock).tag.lock.locktag_type)


/*
 * These structures hold information passed from lmgr internals to the lock
 * listing user-level functions (in lockfuncs.c).
 */

typedef struct LockInstanceData
{
	LOCKTAG		locktag;		/* tag for locked object */
	LOCKMASK	holdMask;		/* locks held by this PGPROC */
	LOCKMODE	waitLockMode;	/* lock awaited by this PGPROC, if any */
	VirtualTransactionId vxid;	/* virtual transaction ID of this PGPROC */
	TimestampTz waitStart;		/* time at which this PGPROC started waiting
								 * for lock */
	int			pid;			/* pid of this PGPROC */
	int			leaderPid;		/* pid of group leader; = pid if no group */
	bool		fastpath;		/* taken via fastpath? */
} LockInstanceData;

typedef struct LockData
{
	int			nelements;		/* The length of the array */
	LockInstanceData *locks;	/* Array of per-PROCLOCK information */
} LockData;

typedef struct BlockedProcData
{
	int			pid;			/* pid of a blocked PGPROC */
	/* Per-PROCLOCK information about PROCLOCKs of the lock the pid awaits */
	/* (these fields refer to indexes in BlockedProcsData.locks[]) */
	int			first_lock;		/* index of first relevant LockInstanceData */
	int			num_locks;		/* number of relevant LockInstanceDatas */
	/* PIDs of PGPROCs that are ahead of "pid" in the lock's wait queue */
	/* (these fields refer to indexes in BlockedProcsData.waiter_pids[]) */
	int			first_waiter;	/* index of first preceding waiter */
	int			num_waiters;	/* number of preceding waiters */
} BlockedProcData;

typedef struct BlockedProcsData
{
	BlockedProcData *procs;		/* Array of per-blocked-proc information */
	LockInstanceData *locks;	/* Array of per-PROCLOCK information */
	int		   *waiter_pids;	/* Array of PIDs of other blocked PGPROCs */
	int			nprocs;			/* # of valid entries in procs[] array */
	int			maxprocs;		/* Allocated length of procs[] array */
	int			nlocks;			/* # of valid entries in locks[] array */
	int			maxlocks;		/* Allocated length of locks[] array */
	int			npids;			/* # of valid entries in waiter_pids[] array */
	int			maxpids;		/* Allocated length of waiter_pids[] array */
} BlockedProcsData;


/* Result codes for LockAcquire() */
typedef enum
{
	LOCKACQUIRE_NOT_AVAIL,		/* lock not available, and dontWait=true */
	LOCKACQUIRE_OK,				/* lock successfully acquired */
	LOCKACQUIRE_ALREADY_HELD,	/* incremented count for lock already held */
	LOCKACQUIRE_ALREADY_CLEAR,	/* incremented count for lock already clear */
} LockAcquireResult;

/* Deadlock states identified by DeadLockCheck() */
typedef enum
{
	DS_NOT_YET_CHECKED,			/* no deadlock check has run yet */
	DS_NO_DEADLOCK,				/* no deadlock detected */
	DS_SOFT_DEADLOCK,			/* deadlock avoided by queue rearrangement */
	DS_HARD_DEADLOCK,			/* deadlock, no way out but ERROR */
	DS_BLOCKED_BY_AUTOVACUUM,	/* no deadlock; queue blocked by autovacuum
								 * worker */
} DeadLockState;

/*
 * The lockmgr's shared hash tables are partitioned to reduce contention.
 * To determine which partition a given locktag belongs to, compute the tag's
 * hash code with LockTagHashCode(), then apply one of these macros.
 * NB: NUM_LOCK_PARTITIONS must be a power of 2!
 */

/* 锁管理器的共享哈希表分区以降低争用。要确定 locktag 所属分区，先用 LockTagHashCode() 计算标签
 * 哈希码，再应用这些宏。注意：NUM_LOCK_PARTITIONS 必须是 2 的幂。 */
#define LockHashPartition(hashcode) \
	((hashcode) % NUM_LOCK_PARTITIONS)
#define LockHashPartitionLock(hashcode) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + \
		LockHashPartition(hashcode)].lock)
#define LockHashPartitionLockByIndex(i) \
	(&MainLWLockArray[LOCK_MANAGER_LWLOCK_OFFSET + (i)].lock)

/*
 * The deadlock detector needs to be able to access lockGroupLeader and
 * related fields in the PGPROC, so we arrange for those fields to be protected
 * by one of the lock hash partition locks.  Since the deadlock detector
 * acquires all such locks anyway, this makes it safe for it to access these
 * fields without doing anything extra.  To avoid contention as much as
 * possible, we map different PGPROCs to different partition locks.  The lock
 * used for a given lock group is determined by the group leader's pgprocno.
 */

/* 死锁检测器必须访问 PGPROC 中的 lockGroupLeader 及相关字段，因此这些字段由一个锁哈希分区锁保护。
 * 检测器本就会获取所有此类锁，因而可安全访问。为减少争用，不同 PGPROC 映射到不同分区锁；锁组使用
 * 的锁由组长 pgprocno 决定。 */
#define LockHashPartitionLockByProc(leader_pgproc) \
	LockHashPartitionLock(GetNumberFromPGProc(leader_pgproc))

/*
 * function prototypes
 */

/* 函数原型。 */
/* Initializes lock-manager shared-memory structures. */

/* 初始化锁管理器共享内存结构。 */
extern void LockManagerShmemInit(void);
/* Returns the shared-memory size needed by the lock manager. */

/* 返回锁管理器所需的共享内存大小。 */
extern Size LockManagerShmemSize(void);
/* Initializes this backend's access to the lock manager. */

/* 初始化当前后端对锁管理器的访问。 */
extern void InitLockManagerAccess(void);
/* Returns the lock method table for a LOCK object. */

/* 返回 LOCK 对象的锁方法表。 */
extern LockMethod GetLocksMethodTable(const LOCK *lock);
/* Returns the lock method table selected by a lock tag. */

/* 返回锁标签选择的锁方法表。 */
extern LockMethod GetLockTagsMethodTable(const LOCKTAG *locktag);
/* Computes the hash code used to partition and find a lock tag. */

/* 计算用于分区和查找锁标签的哈希码。 */
extern uint32 LockTagHashCode(const LOCKTAG *locktag);
/* Tests whether two lock modes conflict according to their method table. */

/* 根据锁方法表测试两个锁模式是否冲突。 */
extern bool DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2);
/* Acquires a lock, optionally waiting when a conflicting holder exists. */

/* 获取锁；存在冲突持有者时可选择等待。 */
extern LockAcquireResult LockAcquire(const LOCKTAG *locktag,
									 LOCKMODE lockmode,
									 bool sessionLock,
									 bool dontWait);
/* Acquires a lock with extended local-state and error-reporting controls. */

/* 使用扩展的本地状态和错误报告控制获取锁。 */
extern LockAcquireResult LockAcquireExtended(const LOCKTAG *locktag,
											 LOCKMODE lockmode,
											 bool sessionLock,
											 bool dontWait,
											 bool reportMemoryError,
											 LOCALLOCK **locallockp,
											 bool logLockFailure);
/* Rolls back bookkeeping for an in-progress strong-lock acquisition. */

/* 回滚正在进行的强锁获取的记录状态。 */
extern void AbortStrongLockAcquire(void);
/* Marks a locally held lock as clear after invalidation processing. */

/* 在失效处理后将本地持有锁标记为已清除。 */
extern void MarkLockClear(LOCALLOCK *locallock);
/* Releases one lock acquisition and updates local/shared lock state. */

/* 释放一次锁获取并更新本地和共享锁状态。 */
extern bool LockRelease(const LOCKTAG *locktag,
						LOCKMODE lockmode, bool sessionLock);
/* Releases all locks of a method, optionally including session locks. */

/* 释放一个锁方法的所有锁，并可包括会话锁。 */
extern void LockReleaseAll(LOCKMETHODID lockmethodid, bool allLocks);
/* Releases all session locks belonging to a lock method. */

/* 释放属于一个锁方法的全部会话锁。 */
extern void LockReleaseSession(LOCKMETHODID lockmethodid);
/* Releases locks owned by the current resource owner. */

/* 释放当前资源所有者拥有的锁。 */
extern void LockReleaseCurrentOwner(LOCALLOCK **locallocks, int nlocks);
/* Reassigns current-owner locks to the parent resource owner. */

/* 将当前所有者的锁重新分配给父资源所有者。 */
extern void LockReassignCurrentOwner(LOCALLOCK **locallocks, int nlocks);
/* Tests whether this backend holds the requested or a stronger lock. */

/* 测试当前后端是否持有所需或更强的锁。 */
extern bool LockHeldByMe(const LOCKTAG *locktag,
						 LOCKMODE lockmode, bool orstronger);
#ifdef USE_ASSERT_CHECKING
/* Returns the backend-local lock hash for assertion checks. */

/* 返回供断言检查使用的后端本地锁哈希表。 */
extern HTAB *GetLockMethodLocalHash(void);
#endif
/* Reports whether a lock mode currently has waiters. */

/* 报告锁模式当前是否有等待者。 */
extern bool LockHasWaiters(const LOCKTAG *locktag,
						   LOCKMODE lockmode, bool sessionLock);
/* Returns VXIDs that conflict with a requested lock tag and mode. */

/* 返回与所请求锁标签和模式冲突的 VXID。 */
extern VirtualTransactionId *GetLockConflicts(const LOCKTAG *locktag,
											  LOCKMODE lockmode, int *countp);
/* Saves locks belonging to a transaction being prepared. */

/* 保存正在预备事务所属的锁。 */
extern void AtPrepare_Locks(void);
/* Finalizes prepared-transaction lock state using its transaction ID. */

/* 使用事务 ID 完成预备事务的锁状态。 */
extern void PostPrepare_Locks(TransactionId xid);
/* Tests a requested lock against existing holders for conflicts. */

/* 将所请求锁与现有持有者比较以测试冲突。 */
extern bool LockCheckConflicts(LockMethod lockMethodTable,
							   LOCKMODE lockmode,
							   LOCK *lock, PROCLOCK *proclock);
/* Grants one lock mode to a PROCLOCK and updates shared counters. */

/* 向 PROCLOCK 授予一个锁模式并更新共享计数器。 */
extern void GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode);
/* Grants the lock currently awaited by this backend. */

/* 授予当前后端正在等待的锁。 */
extern void GrantAwaitedLock(void);
/* Returns the local lock entry currently awaited by this backend. */

/* 返回当前后端正在等待的本地锁条目。 */
extern LOCALLOCK *GetAwaitedLock(void);
/* Clears this backend's awaited-lock tracking state. */

/* 清除当前后端的待授予锁跟踪状态。 */
extern void ResetAwaitedLock(void);

/* Removes a process from a lock wait queue using its lock hash code. */

/* 使用锁哈希码将进程从锁等待队列移除。 */
extern void RemoveFromWaitQueue(PGPROC *proc, uint32 hashcode);
/* Collects a snapshot of lock status for lock-listing callers. */

/* 为锁列表调用方收集锁状态快照。 */
extern LockData *GetLockStatusData(void);
/* Collects blocker information for a process blocked on a lock. */

/* 收集被锁阻塞进程的阻塞者信息。 */
extern BlockedProcsData *GetBlockerStatusData(int blocked_pid);

/* Returns WAL-ready records for locks held by running transactions. */

/* 返回正在运行事务持有锁的 WAL 就绪记录。 */
extern xl_standby_lock *GetRunningTransactionLocks(int *nlocks);
/* Returns the human-readable name of a lock mode. */

/* 返回锁模式的人类可读名称。 */
extern const char *GetLockmodeName(LOCKMETHODID lockmethodid, LOCKMODE mode);

/* Recreates lock state from a two-phase commit record during recovery. */

/* 在恢复期间从两阶段提交记录重建锁状态。 */
extern void lock_twophase_recover(TransactionId xid, uint16 info,
								  void *recdata, uint32 len);
/* Releases two-phase locks after commit processing. */

/* 在提交处理后释放两阶段锁。 */
extern void lock_twophase_postcommit(TransactionId xid, uint16 info,
									 void *recdata, uint32 len);
/* Releases two-phase locks after abort processing. */

/* 在中止处理后释放两阶段锁。 */
extern void lock_twophase_postabort(TransactionId xid, uint16 info,
									void *recdata, uint32 len);
/* Restores two-phase lock state on a standby during recovery. */

/* 在恢复期间于备机恢复两阶段锁状态。 */
extern void lock_twophase_standby_recover(TransactionId xid, uint16 info,
										  void *recdata, uint32 len);

/* Runs deadlock detection for a waiting process and returns the outcome. */

/* 为等待进程执行死锁检测并返回结果。 */
extern DeadLockState DeadLockCheck(PGPROC *proc);
/* Returns the autovacuum process blocking the current lock wait, if any. */

/* 返回阻塞当前锁等待的 autovacuum 进程（如有）。 */
extern PGPROC *GetBlockingAutoVacuumPgproc(void);
/* Reports a detected deadlock and terminates the current statement. */

/* 报告检测到的死锁并终止当前语句。 */
pg_noreturn extern void DeadLockReport(void);
/* Records a simple deadlock edge for later detector processing. */

/* 记录简单死锁边，供检测器后续处理。 */
extern void RememberSimpleDeadLock(PGPROC *proc1,
								   LOCKMODE lockmode,
								   LOCK *lock,
								   PGPROC *proc2);
/* Initializes backend-local data used by deadlock checking. */

/* 初始化死锁检查使用的后端本地数据。 */
extern void InitDeadLockChecking(void);

/* Counts processes waiting for the specified lock tag. */

/* 统计等待指定锁标签的进程。 */
extern int	LockWaiterCount(const LOCKTAG *locktag);

#ifdef LOCK_DEBUG
/* Dumps locks associated with one process for debugging. */

/* 为调试转储一个进程关联的锁。 */
extern void DumpLocks(PGPROC *proc);
/* Dumps all lock-manager state for debugging. */

/* 为调试转储全部锁管理器状态。 */
extern void DumpAllLocks(void);
#endif

/* Lock a VXID (used to wait for a transaction to finish) */

/* 锁定 VXID（用于等待事务结束）。 */
/* Inserts a virtual transaction ID into the VXID lock table. */

/* 将虚拟事务 ID 插入 VXID 锁表。 */
extern void VirtualXactLockTableInsert(VirtualTransactionId vxid);
/* Removes a virtual transaction ID from the VXID lock table. */

/* 从 VXID 锁表中移除虚拟事务 ID。 */
extern void VirtualXactLockTableCleanup(void);
/* Acquires or waits for the lock associated with a virtual transaction. */

/* 获取或等待与虚拟事务关联的锁。 */
extern bool VirtualXactLock(VirtualTransactionId vxid, bool wait);

#endif							/* LOCK_H_ */
