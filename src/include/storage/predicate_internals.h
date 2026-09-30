/*-------------------------------------------------------------------------
 *
 * predicate_internals.h
 *	  POSTGRES internal predicate locking definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/predicate_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICATE_INTERNALS_H
#define PREDICATE_INTERNALS_H

#include "lib/ilist.h"
#include "storage/lock.h"
#include "storage/lwlock.h"

/*
 * Commit number.
 */

/*
 * 提交编号。
 */
typedef uint64 SerCommitSeqNo;

/*
 * Reserved commit sequence numbers:
 *	- 0 is reserved to indicate a non-existent SLRU entry; it cannot be
 *	  used as a SerCommitSeqNo, even an invalid one
 *	- InvalidSerCommitSeqNo is used to indicate a transaction that
 *	  hasn't committed yet, so use a number greater than all valid
 *	  ones to make comparison do the expected thing
 *	- RecoverySerCommitSeqNo is used to refer to transactions that
 *	  happened before a crash/recovery, since we restart the sequence
 *	  at that point.  It's earlier than all normal sequence numbers,
 *	  and is only used by recovered prepared transactions
 */

/*
 * 保留的提交序列号：
 *	- 0 保留用于表示不存在的 SLRU 条目；即使是无效值，也不能作为 SerCommitSeqNo 使用
 *	- InvalidSerCommitSeqNo 用于表示尚未提交的事务，因此使用大于所有有效值的数字，
 *	  以使比较产生预期结果
 *	- RecoverySerCommitSeqNo 用于引用崩溃/恢复之前发生的事务，因为序列会在该点重新
 *	  开始。它早于所有普通序列号，且仅用于已恢复的准备事务
 */
#define InvalidSerCommitSeqNo		((SerCommitSeqNo) PG_UINT64_MAX)
#define RecoverySerCommitSeqNo		((SerCommitSeqNo) 1)
#define FirstNormalSerCommitSeqNo	((SerCommitSeqNo) 2)

/*
 * The SERIALIZABLEXACT struct contains information needed for each
 * serializable database transaction to support SSI techniques.
 *
 * A home-grown list is maintained in shared memory to manage these.
 * An entry is used when the serializable transaction acquires a snapshot.
 * Unless the transaction is rolled back, this entry must generally remain
 * until all concurrent transactions have completed.  (There are special
 * optimizations for READ ONLY transactions which often allow them to be
 * cleaned up earlier.)  A transaction which is rolled back is cleaned up
 * as soon as possible.
 *
 * Eligibility for cleanup of committed transactions is generally determined
 * by comparing the transaction's finishedBefore field to
 * SxactGlobalXmin.
 */

/*
 * SERIALIZABLEXACT 结构体包含支持 SSI 技术所需的每个可串行化数据库事务的信息。
 *
 * 共享内存中维护自建链表来管理这些对象。可串行化事务获取快照时使用一个条目。除非事务
 * 回滚，该条目通常必须保留到所有并发事务完成。（对只读事务有特殊优化，通常可更早清理。）
 * 已回滚事务会尽快清理。
 *
 * 已提交事务是否可清理通常通过比较事务的 finishedBefore 字段与 SxactGlobalXmin 确定。
 */
typedef struct SERIALIZABLEXACT
{
	VirtualTransactionId vxid;	/* The executing process always has one of
								 * these. */

	/* 执行进程始终具有其中一个。 */

	/*
	 * We use two numbers to track the order that transactions commit. Before
	 * commit, a transaction is marked as prepared, and prepareSeqNo is set.
	 * Shortly after commit, it's marked as committed, and commitSeqNo is set.
	 * This doesn't give a strict commit order, but these two values together
	 * are good enough for us, as we can always err on the safe side and
	 * assume that there's a conflict, if we can't be sure of the exact
	 * ordering of two commits.
	 *
	 * Note that a transaction is marked as prepared for a short period during
	 * commit processing, even if two-phase commit is not used. But with
	 * two-phase commit, a transaction can stay in prepared state for some
	 * time.
	 */

	/*
	 * 使用两个数字跟踪事务提交顺序。提交前，事务标记为已准备并设置 prepareSeqNo；提交
	 * 后不久标记为已提交并设置 commitSeqNo。这并不提供严格提交顺序，但二者结合足够
	 * 使用，因为无法确定两次提交精确顺序时，始终可保守地假定存在冲突。
	 *
	 * 请注意，即使不使用两阶段提交，事务在提交处理中也会短暂标记为已准备。但使用两阶段
	 * 提交时，事务可在已准备状态停留一段时间。
	 */
	SerCommitSeqNo prepareSeqNo;
	SerCommitSeqNo commitSeqNo;

	/* these values are not both interesting at the same time */

	/* 这两个值不会同时有意义。 */
	union
	{
		SerCommitSeqNo earliestOutConflictCommit;	/* when committed with
													 * conflict out */

		/* 提交时存在向外冲突。 */
		SerCommitSeqNo lastCommitBeforeSnapshot;	/* when not committed or
													 * no conflict out */

		/* 未提交或不存在向外冲突时。 */
	}			SeqNo;
	dlist_head	outConflicts;	/* list of write transactions whose data we
								 * couldn't read. */

	/* 数据无法被我们读取的写事务列表。 */
	dlist_head	inConflicts;	/* list of read transactions which couldn't
								 * see our write. */

	/* 无法看到我们写入的读事务列表。 */
	dlist_head	predicateLocks; /* list of associated PREDICATELOCK objects */

	/* 关联的 PREDICATELOCK 对象列表。 */
	dlist_node	finishedLink;	/* list link in
								 * FinishedSerializableTransactions */

	/* FinishedSerializableTransactions 中的列表链接。 */
	dlist_node	xactLink;		/* PredXact->activeList/availableList */

	/* PredXact->activeList/availableList 中的列表链接。 */

	/*
	 * perXactPredicateListLock is only used in parallel queries: it protects
	 * this SERIALIZABLEXACT's predicate lock list against other workers of
	 * the same session.
	 */

	/*
	 * perXactPredicateListLock 仅在并行查询中使用：它保护此 SERIALIZABLEXACT 的谓词锁
	 * 列表，防止同一会话中的其他工作进程并发访问。
	 */
	LWLock		perXactPredicateListLock;

	/*
	 * for r/o transactions: list of concurrent r/w transactions that we could
	 * potentially have conflicts with, and vice versa for r/w transactions
	 */

	/*
	 * 对只读事务：可能与我们发生冲突的并发读写事务列表；对读写事务则反过来。
	 */
	dlist_head	possibleUnsafeConflicts;

	TransactionId topXid;		/* top level xid for the transaction, if one
								 * exists; else invalid */

	/* 事务的顶层 xid；若不存在则无效。 */
	TransactionId finishedBefore;	/* invalid means still running; else the
									 * struct expires when no serializable
									 * xids are before this. */

	/* 无效表示仍在运行；否则当前没有可串行化 xid 位于其前时结构体失效。 */
	TransactionId xmin;			/* the transaction's snapshot xmin */

	/* 事务快照的 xmin。 */
	uint32		flags;			/* OR'd combination of values defined below */

	/* 下方定义的值按位或形成的组合。 */
	int			pid;			/* pid of associated process */

	/* 关联进程的 pid。 */
	int			pgprocno;		/* pgprocno of associated process */

	/* 关联进程的 pgprocno。 */
} SERIALIZABLEXACT;

#define SXACT_FLAG_COMMITTED			0x00000001	/* already committed */

/* 已提交。 */
#define SXACT_FLAG_PREPARED				0x00000002	/* about to commit */

/* 即将提交。 */
#define SXACT_FLAG_ROLLED_BACK			0x00000004	/* already rolled back */

/* 已回滚。 */
#define SXACT_FLAG_DOOMED				0x00000008	/* will roll back */

/* 将回滚。 */
/*
 * The following flag actually means that the flagged transaction has a
 * conflict out *to a transaction which committed ahead of it*.  It's hard
 * to get that into a name of a reasonable length.
 */

/*
 * 以下标志实际表示被标记的事务对一个在其之前提交的事务存在向外冲突。很难用合理长度的
 * 名称表达这一含义。
 */
#define SXACT_FLAG_CONFLICT_OUT			0x00000010
#define SXACT_FLAG_READ_ONLY			0x00000020
#define SXACT_FLAG_DEFERRABLE_WAITING	0x00000040
#define SXACT_FLAG_RO_SAFE				0x00000080
#define SXACT_FLAG_RO_UNSAFE			0x00000100
#define SXACT_FLAG_SUMMARY_CONFLICT_IN	0x00000200
#define SXACT_FLAG_SUMMARY_CONFLICT_OUT 0x00000400
/*
 * The following flag means the transaction has been partially released
 * already, but is being preserved because parallel workers might have a
 * reference to it.  It'll be recycled by the leader at end-of-transaction.
 */

/*
 * 以下标志表示事务已部分释放，但因并行工作进程可能仍持有引用而保留。领导者将在事务结束
 * 时回收它。
 */
#define SXACT_FLAG_PARTIALLY_RELEASED	0x00000800

typedef struct PredXactListData
{
	dlist_head	availableList;
	dlist_head	activeList;

	/*
	 * These global variables are maintained when registering and cleaning up
	 * serializable transactions.  They must be global across all backends,
	 * but are not needed outside the predicate.c source file. Protected by
	 * SerializableXactHashLock.
	 */

	/*
	 * 这些全局变量在注册和清理可串行化事务时维护。它们必须跨所有后端全局可见，但在
	 * predicate.c 源文件外不需要。由 SerializableXactHashLock 保护。
	 */
	TransactionId SxactGlobalXmin;	/* global xmin for active serializable
									 * transactions */

	/* 活动可串行化事务的全局 xmin。 */
	int			SxactGlobalXminCount;	/* how many active serializable
										 * transactions have this xmin */

	/* 具有该 xmin 的活动可串行化事务数量。 */
	int			WritableSxactCount; /* how many non-read-only serializable
									 * transactions are active */

	/* 活动的非只读可串行化事务数量。 */
	SerCommitSeqNo LastSxactCommitSeqNo;	/* a strictly monotonically
											 * increasing number for commits
											 * of serializable transactions */

	/* 可串行化事务提交使用的严格单调递增编号。 */
	/* Protected by SerializableXactHashLock. */

	/* 由 SerializableXactHashLock 保护。 */
	SerCommitSeqNo CanPartialClearThrough;	/* can clear predicate locks and
											 * inConflicts for committed
											 * transactions through this seq
											 * no */

	/* 可清除达到该序列号的已提交事务的谓词锁和 inConflicts。 */
	/* Protected by SerializableFinishedListLock. */

	/* 由 SerializableFinishedListLock 保护。 */
	SerCommitSeqNo HavePartialClearedThrough;	/* have cleared through this
												 * seq no */

	/* 已清除到该序列号。 */
	SERIALIZABLEXACT *OldCommittedSxact;	/* shared copy of dummy sxact */

	/* 虚拟 sxact 的共享副本。 */

	SERIALIZABLEXACT *element;
}			PredXactListData;

typedef struct PredXactListData *PredXactList;

#define PredXactListDataSize \
		((Size)MAXALIGN(sizeof(PredXactListData)))


/*
 * The following types are used to provide lists of rw-conflicts between
 * pairs of transactions.  Since exactly the same information is needed,
 * they are also used to record possible unsafe transaction relationships
 * for purposes of identifying safe snapshots for read-only transactions.
 *
 * When a RWConflictData is not in use to record either type of relationship
 * between a pair of transactions, it is kept on an "available" list.  The
 * outLink field is used for maintaining that list.
 */

/*
 * 以下类型用于提供事务对之间读写冲突的列表。由于需要完全相同的信息，它们还用于记录可能
 * 不安全的事务关系，以识别只读事务的安全快照。
 *
 * 当 RWConflictData 未用于记录事务对之间的任一种关系时，它保留在“可用”链表中。outLink
 * 字段用于维护该链表。
 */
typedef struct RWConflictData
{
	dlist_node	outLink;		/* link for list of conflicts out from a sxact */

	/* 从 sxact 向外冲突列表的链接。 */
	dlist_node	inLink;			/* link for list of conflicts in to a sxact */

	/* 指向 sxact 的传入冲突列表链接。 */
	SERIALIZABLEXACT *sxactOut;
	SERIALIZABLEXACT *sxactIn;
} RWConflictData;

typedef struct RWConflictData *RWConflict;

#define RWConflictDataSize \
		((Size)MAXALIGN(sizeof(RWConflictData)))

typedef struct RWConflictPoolHeaderData
{
	dlist_head	availableList;
	RWConflict	element;
}			RWConflictPoolHeaderData;

typedef struct RWConflictPoolHeaderData *RWConflictPoolHeader;

#define RWConflictPoolHeaderDataSize \
		((Size)MAXALIGN(sizeof(RWConflictPoolHeaderData)))


/*
 * The SERIALIZABLEXIDTAG struct identifies an xid assigned to a serializable
 * transaction or any of its subtransactions.
 */

/*
 * SERIALIZABLEXIDTAG 结构体标识分配给可串行化事务或其任意子事务的 xid。
 */
typedef struct SERIALIZABLEXIDTAG
{
	TransactionId xid;
} SERIALIZABLEXIDTAG;

/*
 * The SERIALIZABLEXID struct provides a link from a TransactionId for a
 * serializable transaction to the related SERIALIZABLEXACT record, even if
 * the transaction has completed and its connection has been closed.
 *
 * These are created as new top level transaction IDs are first assigned to
 * transactions which are participating in predicate locking.  This may
 * never happen for a particular transaction if it doesn't write anything.
 * They are removed with their related serializable transaction objects.
 *
 * The SubTransGetTopmostTransaction method is used where necessary to get
 * from an XID which might be from a subtransaction to the top level XID.
 */

/*
 * SERIALIZABLEXID 结构体提供从可串行化事务的 TransactionId 到相关 SERIALIZABLEXACT
 * 记录的链接，即使该事务已完成且连接已关闭。
 *
 * 当参与谓词锁定的事务首次分配新的顶层事务 ID 时创建这些记录。若特定事务不写入任何
 * 内容，这可能永不发生。它们会与关联的可串行化事务对象一同移除。
 *
 * 必要时使用 SubTransGetTopmostTransaction，从可能来自子事务的 XID 获取顶层 XID。
 */
typedef struct SERIALIZABLEXID
{
	/* hash key */

	/* 哈希键。 */
	SERIALIZABLEXIDTAG tag;

	/* data */

	/* 数据。 */
	SERIALIZABLEXACT *myXact;	/* pointer to the top level transaction data */

	/* 指向顶层事务数据的指针。 */
} SERIALIZABLEXID;


/*
 * The PREDICATELOCKTARGETTAG struct identifies a database object which can
 * be the target of predicate locks.
 *
 * Note that the hash function being used doesn't properly respect tag
 * length -- if the length of the structure isn't a multiple of four bytes it
 * will go to a four byte boundary past the end of the tag.  If you change
 * this struct, make sure any slack space is initialized, so that any random
 * bytes in the middle or at the end are not included in the hash.
 *
 * TODO SSI: If we always use the same fields for the same type of value, we
 * should rename these.  Holding off until it's clear there are no exceptions.
 * Since indexes are relations with blocks and tuples, it's looking likely that
 * the rename will be possible.  If not, we may need to divide the last field
 * and use part of it for a target type, so that we know how to interpret the
 * data..
 */

/*
 * PREDICATELOCKTARGETTAG 结构体标识可作为谓词锁目标的数据库对象。
 *
 * 注意：所用哈希函数不能正确处理标签长度——如果结构体长度不是四字节的倍数，它将读取到
 * 标签末尾之后的四字节边界。若修改此结构体，必须初始化所有空余空间，以避免中间或末尾的
 * 随机字节参与哈希。
 *
 * TODO SSI：若始终对同类值使用相同字段，应重命名它们。等待确认不存在例外。由于索引是
 * 具有块和元组的关系，看起来可以重命名。否则可能需要拆分最后一个字段，并用其中一部分
 * 表示目标类型，以便解释数据。
 */
typedef struct PREDICATELOCKTARGETTAG
{
	uint32		locktag_field1; /* a 32-bit ID field */

	/* 一个 32 位 ID 字段。 */
	uint32		locktag_field2; /* a 32-bit ID field */

	/* 一个 32 位 ID 字段。 */
	uint32		locktag_field3; /* a 32-bit ID field */

	/* 一个 32 位 ID 字段。 */
	uint32		locktag_field4; /* a 32-bit ID field */

	/* 一个 32 位 ID 字段。 */
} PREDICATELOCKTARGETTAG;

/*
 * The PREDICATELOCKTARGET struct represents a database object on which there
 * are predicate locks.
 *
 * A hash list of these objects is maintained in shared memory.  An entry is
 * added when a predicate lock is requested on an object which doesn't
 * already have one.  An entry is removed when the last lock is removed from
 * its list.
 */

/*
 * PREDICATELOCKTARGET 结构体表示其上存在谓词锁的数据库对象。
 *
 * 共享内存中维护这些对象的哈希列表。对尚无谓词锁的对象请求谓词锁时添加条目；从其列表中
 * 移除最后一个锁时删除条目。
 */
typedef struct PREDICATELOCKTARGET
{
	/* hash key */

	/* 哈希键。 */
	PREDICATELOCKTARGETTAG tag; /* unique identifier of lockable object */

	/* 可锁对象的唯一标识符。 */

	/* data */

	/* 数据。 */
	dlist_head	predicateLocks; /* list of PREDICATELOCK objects assoc. with
								 * predicate lock target */

	/* 与谓词锁目标关联的 PREDICATELOCK 对象列表。 */
} PREDICATELOCKTARGET;


/*
 * The PREDICATELOCKTAG struct identifies an individual predicate lock.
 *
 * It is the combination of predicate lock target (which is a lockable
 * object) and a serializable transaction which has acquired a lock on that
 * target.
 */

/*
 * PREDICATELOCKTAG 结构体标识单个谓词锁。
 *
 * 它由谓词锁目标（一个可锁对象）与在该目标上获取锁的可串行化事务组成。
 */
typedef struct PREDICATELOCKTAG
{
	PREDICATELOCKTARGET *myTarget;
	SERIALIZABLEXACT *myXact;
} PREDICATELOCKTAG;

/*
 * The PREDICATELOCK struct represents an individual lock.
 *
 * An entry can be created here when the related database object is read, or
 * by promotion of multiple finer-grained targets.  All entries related to a
 * serializable transaction are removed when that serializable transaction is
 * cleaned up.  Entries can also be removed when they are combined into a
 * single coarser-grained lock entry.
 */

/*
 * PREDICATELOCK 结构体表示单个锁。
 *
 * 读取相关数据库对象时，或提升多个更细粒度目标时，可在这里创建条目。与可串行化事务相关
 * 的所有条目会在该事务清理时移除。条目也可在合并为单个更粗粒度锁条目时移除。
 */
typedef struct PREDICATELOCK
{
	/* hash key */

	/* 哈希键。 */
	PREDICATELOCKTAG tag;		/* unique identifier of lock */

	/* 锁的唯一标识符。 */

	/* data */

	/* 数据。 */
	dlist_node	targetLink;		/* list link in PREDICATELOCKTARGET's list of
								 * predicate locks */

	/* PREDICATELOCKTARGET 谓词锁列表中的链接。 */
	dlist_node	xactLink;		/* list link in SERIALIZABLEXACT's list of
								 * predicate locks */

	/* SERIALIZABLEXACT 谓词锁列表中的链接。 */
	SerCommitSeqNo commitSeqNo; /* only used for summarized predicate locks */

	/* 仅用于汇总的谓词锁。 */
} PREDICATELOCK;


/*
 * The LOCALPREDICATELOCK struct represents a local copy of data which is
 * also present in the PREDICATELOCK table, organized for fast access without
 * needing to acquire a LWLock.  It is strictly for optimization.
 *
 * Each serializable transaction creates its own local hash table to hold a
 * collection of these.  This information is used to determine when a number
 * of fine-grained locks should be promoted to a single coarser-grained lock.
 * The information is maintained more-or-less in parallel to the
 * PREDICATELOCK data, but because this data is not protected by locks and is
 * only used in an optimization heuristic, it is allowed to drift in a few
 * corner cases where maintaining exact data would be expensive.
 *
 * The hash table is created when the serializable transaction acquires its
 * snapshot, and its memory is released upon completion of the transaction.
 */

/*
 * LOCALPREDICATELOCK 结构体表示也存在于 PREDICATELOCK 表中的数据的本地副本，按无需
 * 获取 LWLock 即可快速访问的方式组织。它严格用于优化。
 *
 * 每个可串行化事务创建自己的本地哈希表以容纳这些对象。该信息用于确定何时将多个细粒度
 * 锁提升为单个粗粒度锁。信息大致与 PREDICATELOCK 数据并行维护，但由于不受锁保护且仅
 * 用于优化启发式，在精确维护代价高昂的少数边界情况中允许发生偏差。
 *
 * 该哈希表在可串行化事务获取快照时创建，并在事务完成时释放其内存。
 */
typedef struct LOCALPREDICATELOCK
{
	/* hash key */

	/* 哈希键。 */
	PREDICATELOCKTARGETTAG tag; /* unique identifier of lockable object */

	/* 可锁对象的唯一标识符。 */

	/* data */

	/* 数据。 */
	bool		held;			/* is lock held, or just its children?	*/

	/* 是否持有锁，还是仅其子项持有锁？ */
	int			childLocks;		/* number of child locks currently held */

	/* 当前持有的子锁数量。 */
} LOCALPREDICATELOCK;


/*
 * The types of predicate locks which can be acquired.
 */

/*
 * 可获取的谓词锁类型。
 */
typedef enum PredicateLockTargetType
{
	PREDLOCKTAG_RELATION,
	PREDLOCKTAG_PAGE,
	PREDLOCKTAG_TUPLE,
	/* TODO SSI: Other types may be needed for index locking */

	/* TODO SSI：索引锁定可能需要其他类型。 */
} PredicateLockTargetType;


/*
 * This structure is used to quickly capture a copy of all predicate
 * locks.  This is currently used only by the pg_lock_status function,
 * which in turn is used by the pg_locks view.
 */

/*
 * 此结构体用于快速捕获所有谓词锁的副本。目前仅由 pg_lock_status 函数使用，而该函数又
 * 由 pg_locks 视图使用。
 */
typedef struct PredicateLockData
{
	int			nelements;
	PREDICATELOCKTARGETTAG *locktags;
	SERIALIZABLEXACT *xacts;
} PredicateLockData;


/*
 * These macros define how we map logical IDs of lockable objects into the
 * physical fields of PREDICATELOCKTARGETTAG.   Use these to set up values,
 * rather than accessing the fields directly.  Note multiple eval of target!
 */

/*
 * 这些宏定义如何将可锁对象的逻辑 ID 映射到 PREDICATELOCKTARGETTAG 的物理字段。应使用
 * 它们设置值，而非直接访问字段。注意 target 会被多次求值！
 */
#define SET_PREDICATELOCKTARGETTAG_RELATION(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = InvalidBlockNumber, \
	 (locktag).locktag_field4 = InvalidOffsetNumber)

#define SET_PREDICATELOCKTARGETTAG_PAGE(locktag,dboid,reloid,blocknum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = InvalidOffsetNumber)

#define SET_PREDICATELOCKTARGETTAG_TUPLE(locktag,dboid,reloid,blocknum,offnum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = (offnum))

#define GET_PREDICATELOCKTARGETTAG_DB(locktag) \
	((Oid) (locktag).locktag_field1)
#define GET_PREDICATELOCKTARGETTAG_RELATION(locktag) \
	((Oid) (locktag).locktag_field2)
#define GET_PREDICATELOCKTARGETTAG_PAGE(locktag) \
	((BlockNumber) (locktag).locktag_field3)
#define GET_PREDICATELOCKTARGETTAG_OFFSET(locktag) \
	((OffsetNumber) (locktag).locktag_field4)
#define GET_PREDICATELOCKTARGETTAG_TYPE(locktag)							 \
	(((locktag).locktag_field4 != InvalidOffsetNumber) ? PREDLOCKTAG_TUPLE : \
	 (((locktag).locktag_field3 != InvalidBlockNumber) ? PREDLOCKTAG_PAGE :   \
	  PREDLOCKTAG_RELATION))

/*
 * Two-phase commit statefile records. There are two types: for each
 * transaction, we generate one per-transaction record and a variable
 * number of per-predicate-lock records.
 */

/*
 * 两阶段提交状态文件记录。有两种类型：每个事务生成一条每事务记录，以及数量可变的每谓词锁
 * 记录。
 */
typedef enum TwoPhasePredicateRecordType
{
	TWOPHASEPREDICATERECORD_XACT,
	TWOPHASEPREDICATERECORD_LOCK,
} TwoPhasePredicateRecordType;

/*
 * Per-transaction information to reconstruct a SERIALIZABLEXACT. Not
 * much is needed because most of it not meaningful for a recovered
 * prepared transaction.
 *
 * In particular, we do not record the in and out conflict lists for a
 * prepared transaction because the associated SERIALIZABLEXACTs will
 * not be available after recovery. Instead, we simply record the
 * existence of each type of conflict by setting the transaction's
 * summary conflict in/out flag.
 */

/*
 * 用于重建 SERIALIZABLEXACT 的每事务信息。所需信息不多，因为其中大部分对已恢复的准备
 * 事务没有意义。
 *
 * 特别是，不记录准备事务的传入和传出冲突列表，因为恢复后关联的 SERIALIZABLEXACT 不可用。
 * 相反，通过设置事务的汇总传入/传出冲突标志，仅记录每种冲突是否存在。
 */
typedef struct TwoPhasePredicateXactRecord
{
	TransactionId xmin;
	uint32		flags;
} TwoPhasePredicateXactRecord;

/* Per-lock state */

/* 每锁状态。 */
typedef struct TwoPhasePredicateLockRecord
{
	PREDICATELOCKTARGETTAG target;
	uint32		filler;			/* to avoid length change in back-patched fix */

	/* 避免反补丁修复中长度变化。 */
} TwoPhasePredicateLockRecord;

typedef struct TwoPhasePredicateRecord
{
	TwoPhasePredicateRecordType type;
	union
	{
		TwoPhasePredicateXactRecord xactRecord;
		TwoPhasePredicateLockRecord lockRecord;
	}			data;
} TwoPhasePredicateRecord;

/*
 * Define a macro to use for an "empty" SERIALIZABLEXACT reference.
 */

/*
 * 定义用于“空” SERIALIZABLEXACT 引用的宏。
 */
#define InvalidSerializableXact ((SERIALIZABLEXACT *) NULL)


/*
 * Function definitions for functions needing awareness of predicate
 * locking internals.
 */

/*
 * 需要了解谓词锁内部结构的函数定义。
 */
/*
 * Collects predicate-lock status data for lock-status reporting.
 */

/*
 * 收集用于锁状态报告的谓词锁状态数据。
 */
extern PredicateLockData *GetPredicateLockStatusData(void);
/*
 * Finds processes that block safe snapshots and stores their PIDs.
 */

/*
 * 查找阻塞安全快照的进程并存储其 PID。
 */
extern int	GetSafeSnapshotBlockingPids(int blocked_pid,
										int *output, int output_size);

#endif							/* PREDICATE_INTERNALS_H */
