/*-------------------------------------------------------------------------
 *
 * xact.h
 *	  postgres transaction system definitions
 *
 * 中文翻译：xact.h postgres 事务系统定义
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xact.h
 *
 * 中文翻译：src/include/access/xact.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include "access/transam.h"
#include "access/xlogreader.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"

/*
 * Maximum size of Global Transaction ID (including '\0').
 *
 * 全局事务 ID 的最大长度（包含结尾的空字符），并要求 GIDSIZE 能容纳在 TwoPhaseFileHeader 的 uint16 gidlen 字段中。
 *
 * Note that the max value of GIDSIZE must fit in the uint16 gidlen,
 * specified in TwoPhaseFileHeader.
 *
 * 中文翻译：请注意，GIDSIZE 的最大值必须适合在 TwoPhaseFileHeader 中指定的 uint16 gidlen。
 */
#define GIDSIZE 200

/*
 * Xact isolation levels
 *
 * 事务隔离级别。
 */
#define XACT_READ_UNCOMMITTED	0
#define XACT_READ_COMMITTED		1
#define XACT_REPEATABLE_READ	2
#define XACT_SERIALIZABLE		3

extern PGDLLIMPORT int DefaultXactIsoLevel;
extern PGDLLIMPORT int XactIsoLevel;

/*
 * We implement three isolation levels internally.
 * The two stronger ones use one snapshot per database transaction;
 * the others use one snapshot per statement.
 * Serializable uses predicate locks in addition to snapshots.
 * These macros should be used to check which isolation level is selected.
 *
 * 内部实现三种隔离级别：较强的两个级别每个数据库事务使用一个快照，其他级别每条语句使用一个快照；可串行化级别还使用谓词锁。
 */
#define IsolationUsesXactSnapshot() (XactIsoLevel >= XACT_REPEATABLE_READ)
#define IsolationIsSerializable() (XactIsoLevel == XACT_SERIALIZABLE)

/* Xact read-only state */

/* 事务只读状态。 */
extern PGDLLIMPORT bool DefaultXactReadOnly;
extern PGDLLIMPORT bool XactReadOnly;

/* flag for logging statements in this transaction */

/* 用于记录本事务中语句的标志。 */
extern PGDLLIMPORT bool xact_is_sampled;

/*
 * Xact is deferrable -- only meaningful (currently) for read only
 * SERIALIZABLE transactions
 *
 * Xact 的可延迟属性当前只对只读的 SERIALIZABLE 事务有意义。
 */
extern PGDLLIMPORT bool DefaultXactDeferrable;
extern PGDLLIMPORT bool XactDeferrable;

typedef enum
{
	SYNCHRONOUS_COMMIT_OFF,		/* asynchronous commit */

	/* 中文翻译：异步提交 */
	SYNCHRONOUS_COMMIT_LOCAL_FLUSH, /* wait for local flush only */

	/* 中文翻译：仅等待本地刷新 */
	SYNCHRONOUS_COMMIT_REMOTE_WRITE,	/* wait for local flush and remote
										 * write */
	SYNCHRONOUS_COMMIT_REMOTE_FLUSH,	/* wait for local and remote flush */

	/* 中文翻译：等待本地和远程刷新 */
	SYNCHRONOUS_COMMIT_REMOTE_APPLY,	/* wait for local and remote flush and
										 * remote apply */
}			SyncCommitLevel;

/* Define the default setting for synchronous_commit */

/* 定义 synchronous_commit 的默认设置。 */
#define SYNCHRONOUS_COMMIT_ON	SYNCHRONOUS_COMMIT_REMOTE_FLUSH

/* Synchronous commit level */

/* 同步提交级别。 */
extern PGDLLIMPORT int synchronous_commit;

/* used during logical streaming of a transaction */

/* 在事务逻辑流式传输期间使用。 */
extern PGDLLIMPORT TransactionId CheckXidAlive;
extern PGDLLIMPORT bool bsysscan;

/*
 * Miscellaneous flag bits to record events which occur on the top level
 * transaction. These flags are only persisted in MyXactFlags and are intended
 * so we remember to do certain things later in the transaction. This is
 * globally accessible, so can be set from anywhere in the code which requires
 * recording flags.
 *
 * 这些标志记录顶层事务期间发生的事件，保存在 MyXactFlags 中，以便在事务后续阶段执行相应操作。
 */
extern PGDLLIMPORT int MyXactFlags;

/*
 * XACT_FLAGS_ACCESSEDTEMPNAMESPACE - set when a temporary object is accessed.
 * We don't allow PREPARE TRANSACTION in that case.
 *
 * 访问临时对象时设置该标志；此时不允许 PREPARE TRANSACTION。
 */
#define XACT_FLAGS_ACCESSEDTEMPNAMESPACE		(1U << 0)

/*
 * XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK - records whether the top level xact
 * logged any Access Exclusive Locks.
 *
 * 记录顶层事务是否记录了 Access Exclusive Lock。
 */
#define XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK	(1U << 1)

/*
 * XACT_FLAGS_NEEDIMMEDIATECOMMIT - records whether the top level statement
 * is one that requires immediate commit, such as CREATE DATABASE.
 *
 * 记录顶层语句是否需要立即提交，例如 CREATE DATABASE。
 */
#define XACT_FLAGS_NEEDIMMEDIATECOMMIT			(1U << 2)

/*
 * XACT_FLAGS_PIPELINING - set when we complete an extended-query-protocol
 * Execute message.  This is useful for detecting that an implicit transaction
 * block has been created via pipelining.
 *
 * 完成扩展查询协议的 Execute 消息时设置该标志，用于识别由管道化创建的隐式事务块。
 */
#define XACT_FLAGS_PIPELINING					(1U << 3)

/*
 *	start- and end-of-transaction callbacks for dynamically loaded modules
 *
 * 供动态加载模块使用的事务开始和结束回调。
 */
typedef enum
{
	XACT_EVENT_COMMIT,
	XACT_EVENT_PARALLEL_COMMIT,
	XACT_EVENT_ABORT,
	XACT_EVENT_PARALLEL_ABORT,
	XACT_EVENT_PREPARE,
	XACT_EVENT_PRE_COMMIT,
	XACT_EVENT_PARALLEL_PRE_COMMIT,
	XACT_EVENT_PRE_PREPARE,
} XactEvent;

typedef void (*XactCallback) (XactEvent event, void *arg);

typedef enum
{
	SUBXACT_EVENT_START_SUB,
	SUBXACT_EVENT_COMMIT_SUB,
	SUBXACT_EVENT_ABORT_SUB,
	SUBXACT_EVENT_PRE_COMMIT_SUB,
} SubXactEvent;

typedef void (*SubXactCallback) (SubXactEvent event, SubTransactionId mySubid,
								 SubTransactionId parentSubid, void *arg);

/* Data structure for Save/RestoreTransactionCharacteristics */

/* 用于保存和恢复事务特征的数据结构。 */
typedef struct SavedTransactionCharacteristics
{
	int			save_XactIsoLevel;
	bool		save_XactReadOnly;
	bool		save_XactDeferrable;
} SavedTransactionCharacteristics;


/* ----------------
 *		transaction-related XLOG entries
 *
 * 与事务相关的 XLOG 条目。
 * ----------------
 */

/*
 * XLOG allows to store some information in high 4 bits of log record xl_info
 * field. We use 3 for the opcode, and one about an optional flag variable.
 *
 * XLOG 可在日志记录 xl_info 字段的高 4 位存储信息；其中 3 位用于操作码，1 位用于可选标志变量。
 */
#define XLOG_XACT_COMMIT			0x00
#define XLOG_XACT_PREPARE			0x10
#define XLOG_XACT_ABORT				0x20
#define XLOG_XACT_COMMIT_PREPARED	0x30
#define XLOG_XACT_ABORT_PREPARED	0x40
#define XLOG_XACT_ASSIGNMENT		0x50
#define XLOG_XACT_INVALIDATIONS		0x60
/* free opcode 0x70 */

/* 空闲操作码 0x70。 */

/* mask for filtering opcodes out of xl_info */

/* 用于从 xl_info 中筛选操作码的掩码。 */
#define XLOG_XACT_OPMASK			0x70

/* does this record have a 'xinfo' field or not */

/* 该记录是否具有 xinfo 字段。 */
#define XLOG_XACT_HAS_INFO			0x80

/*
 * The following flags, stored in xinfo, determine which information is
 * contained in commit/abort records.
 *
 * 存储在 xinfo 中的以下标志决定提交/中止记录包含哪些信息。
 */
#define XACT_XINFO_HAS_DBINFO			(1U << 0)
#define XACT_XINFO_HAS_SUBXACTS			(1U << 1)
#define XACT_XINFO_HAS_RELFILELOCATORS	(1U << 2)
#define XACT_XINFO_HAS_INVALS			(1U << 3)
#define XACT_XINFO_HAS_TWOPHASE			(1U << 4)
#define XACT_XINFO_HAS_ORIGIN			(1U << 5)
#define XACT_XINFO_HAS_AE_LOCKS			(1U << 6)
#define XACT_XINFO_HAS_GID				(1U << 7)
#define XACT_XINFO_HAS_DROPPED_STATS	(1U << 8)

/*
 * Also stored in xinfo, these indicating a variety of additional actions that
 * need to occur when emulating transaction effects during recovery.
 *
 * 中文翻译：也存储在 xinfo 中，这些指示在恢复期间模拟事务效果时需要发生的各种附加操作。
 *
 * They are named XactCompletion... to differentiate them from
 * EOXact... routines which run at the end of the original transaction
 * completion.
 *
 * 中文翻译：它们被命名为 XactCompletion...，以区别于在原始事务完成结束时运行的 EOXact... 例程。
 */
#define XACT_COMPLETION_APPLY_FEEDBACK			(1U << 29)
#define XACT_COMPLETION_UPDATE_RELCACHE_FILE	(1U << 30)
#define XACT_COMPLETION_FORCE_SYNC_COMMIT		(1U << 31)

/* Access macros for above flags */

/* 访问上述标志的宏。 */
#define XactCompletionApplyFeedback(xinfo) \
	((xinfo & XACT_COMPLETION_APPLY_FEEDBACK) != 0)
#define XactCompletionRelcacheInitFileInval(xinfo) \
	((xinfo & XACT_COMPLETION_UPDATE_RELCACHE_FILE) != 0)
#define XactCompletionForceSyncCommit(xinfo) \
	((xinfo & XACT_COMPLETION_FORCE_SYNC_COMMIT) != 0)

typedef struct xl_xact_assignment
{
	TransactionId xtop;			/* assigned XID's top-level XID */

	/* 中文翻译：已分配 XID 的顶层 XID */
	int			nsubxacts;		/* number of subtransaction XIDs */

	/* 中文翻译：子事务 XID 的数量 */
	TransactionId xsub[FLEXIBLE_ARRAY_MEMBER];	/* assigned subxids */

	/* 中文翻译：已分配的子事务 XID */
} xl_xact_assignment;

#define MinSizeOfXactAssignment offsetof(xl_xact_assignment, xsub)

/*
 * Commit and abort records can contain a lot of information. But a large
 * portion of the records won't need all possible pieces of information. So we
 * only include what's needed.
 *
 * 中文翻译：提交和中止记录可以包含大量信息。但大部分记录并不需要所有可能的信息。所以我们只包含需要的内容。
 *
 * A minimal commit/abort record only consists of a xl_xact_commit/abort
 * struct. The presence of additional information is indicated by bits set in
 * 'xl_xact_xinfo->xinfo'. The presence of the xinfo field itself is signaled
 * by a set XLOG_XACT_HAS_INFO bit in the xl_info field.
 *
 * 中文翻译：最小提交/中止记录仅包含 xl_xact_commit/abort 结构。附加信息的存在由“xl_xact_xinfo->xinfo”中设置的位指示。 xinfo 字段本身的存在由 xl_info 字段中设置的 XLOG_XACT_HAS_INFO 位来表示。
 *
 * NB: All the individual data chunks should be sized to multiples of
 * sizeof(int) and only require int32 alignment. If they require bigger
 * alignment, they need to be copied upon reading.
 *
 * 中文翻译：注意：所有单独的数据块的大小应为 sizeof(int) 的倍数，并且仅需要 int32 对齐。如果它们需要更大的对齐，则需要在阅读时进行复制。
 */

/* sub-records for commit/abort */

/* 提交/中止的子记录。 */

typedef struct xl_xact_xinfo
{
	/*
	 * Even though we right now only require two bytes of space in xinfo we
	 * use four so following records don't have to care about alignment.
	 * Commit records can be large, so copying large portions isn't
	 * attractive.
 *
 * 中文翻译：尽管我们现在在 xinfo 中只需要两个字节的空间，但我们使用四个字节，因此后面的记录不必关心对齐。提交记录可能很大，因此复制大部分记录并不具有吸引力。
	 */
	uint32		xinfo;
} xl_xact_xinfo;

typedef struct xl_xact_dbinfo
{
	Oid			dbId;			/* MyDatabaseId */

	/* MyDatabaseId。 */
	Oid			tsId;			/* MyDatabaseTableSpace */

	/* MyDatabaseTableSpace。 */
} xl_xact_dbinfo;

typedef struct xl_xact_subxacts
{
	int			nsubxacts;		/* number of subtransaction XIDs */

	/* 中文翻译：子事务 XID 的数量 */
	TransactionId subxacts[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_subxacts;
#define MinSizeOfXactSubxacts offsetof(xl_xact_subxacts, subxacts)

typedef struct xl_xact_relfilelocators
{
	int			nrels;			/* number of relations */

	/* 中文翻译：关系数 */
	RelFileLocator xlocators[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_relfilelocators;
#define MinSizeOfXactRelfileLocators offsetof(xl_xact_relfilelocators, xlocators)

/*
 * A transactionally dropped statistics entry.
 *
 * 中文翻译：事务性删除的统计条目。
 *
 * Declared here rather than pgstat.h because pgstat.h can't be included from
 * frontend code, but the WAL format needs to be readable by frontend
 * programs.
 *
 * 中文翻译：在这里声明而不是 pgstat.h，因为 pgstat.h 不能从前端代码中包含，但 WAL 格式需要被前端程序读取。
 */
typedef struct xl_xact_stats_item
{
	int			kind;
	Oid			dboid;

	/*
	 * This stores the value of PgStat_HashKey.objid as two uint32 as all the
	 * fields of xl_xact_xinfo should be multiples of size(int).
 *
 * 中文翻译：这将 PgStat_HashKey.objid 的值存储为两个 uint32，因为 xl_xact_xinfo 的所有字段都应该是 size(int) 的倍数。
	 */
	uint32		objid_lo;
	uint32		objid_hi;
} xl_xact_stats_item;

typedef struct xl_xact_stats_items
{
	int			nitems;
	xl_xact_stats_item items[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_stats_items;
#define MinSizeOfXactStatsItems offsetof(xl_xact_stats_items, items)

typedef struct xl_xact_invals
{
	int			nmsgs;			/* number of shared inval msgs */

	/* 中文翻译：共享无效消息数 */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_xact_invals;
#define MinSizeOfXactInvals offsetof(xl_xact_invals, msgs)

typedef struct xl_xact_twophase
{
	TransactionId xid;
} xl_xact_twophase;

typedef struct xl_xact_origin
{
	XLogRecPtr	origin_lsn;
	TimestampTz origin_timestamp;
} xl_xact_origin;

typedef struct xl_xact_commit
{
	TimestampTz xact_time;		/* time of commit */

	/* 中文翻译：提交时间 */

	/* xl_xact_xinfo follows if XLOG_XACT_HAS_INFO */

	/* 中文翻译：如果 XLOG_XACT_HAS_INFO 则遵循 xl_xact_xinfo */
	/* xl_xact_dbinfo follows if XINFO_HAS_DBINFO */

	/* 中文翻译：如果 XINFO_HAS_DBINFO 则遵循 xl_xact_dbinfo */
	/* xl_xact_subxacts follows if XINFO_HAS_SUBXACT */

	/* 中文翻译：如果 XINFO_HAS_SUBXACT 则遵循 xl_xact_subxacts */
	/* xl_xact_relfilelocators follows if XINFO_HAS_RELFILELOCATORS */

	/* 中文翻译：如果 XINFO_HAS_RELFILELOCATORS 则遵循 xl_xact_relfilelocators */
	/* xl_xact_stats_items follows if XINFO_HAS_DROPPED_STATS */

	/* 中文翻译：如果 XINFO_HAS_DROPPED_STATS 则遵循 xl_xact_stats_items */
	/* xl_xact_invals follows if XINFO_HAS_INVALS */

	/* 中文翻译：如果 XINFO_HAS_INVALS 则遵循 xl_xact_invals */
	/* xl_xact_twophase follows if XINFO_HAS_TWOPHASE */

	/* 中文翻译：如果 XINFO_HAS_TWOPHASE 则遵循 xl_xact_twophase */
	/* twophase_gid follows if XINFO_HAS_GID. As a null-terminated string. */

	/* 中文翻译：如果 XINFO_HAS_GID 则遵循twophase_gid。作为以 null 结尾的字符串。 */
	/* xl_xact_origin follows if XINFO_HAS_ORIGIN, stored unaligned! */

	/* 中文翻译：如果 XINFO_HAS_ORIGIN 则遵循 xl_xact_origin，未对齐存储！ */
} xl_xact_commit;
#define MinSizeOfXactCommit (offsetof(xl_xact_commit, xact_time) + sizeof(TimestampTz))

typedef struct xl_xact_abort
{
	TimestampTz xact_time;		/* time of abort */

	/* 中文翻译：中止时间 */

	/* xl_xact_xinfo follows if XLOG_XACT_HAS_INFO */

	/* 中文翻译：如果 XLOG_XACT_HAS_INFO 则遵循 xl_xact_xinfo */
	/* xl_xact_dbinfo follows if XINFO_HAS_DBINFO */

	/* 中文翻译：如果 XINFO_HAS_DBINFO 则遵循 xl_xact_dbinfo */
	/* xl_xact_subxacts follows if XINFO_HAS_SUBXACT */

	/* 中文翻译：如果 XINFO_HAS_SUBXACT 则遵循 xl_xact_subxacts */
	/* xl_xact_relfilelocators follows if XINFO_HAS_RELFILELOCATORS */

	/* 中文翻译：如果 XINFO_HAS_RELFILELOCATORS 则遵循 xl_xact_relfilelocators */
	/* xl_xact_stats_items follows if XINFO_HAS_DROPPED_STATS */

	/* 中文翻译：如果 XINFO_HAS_DROPPED_STATS 则遵循 xl_xact_stats_items */
	/* No invalidation messages needed. */

	/* 不需要失效消息。 */
	/* xl_xact_twophase follows if XINFO_HAS_TWOPHASE */

	/* 中文翻译：如果 XINFO_HAS_TWOPHASE 则遵循 xl_xact_twophase */
	/* twophase_gid follows if XINFO_HAS_GID. As a null-terminated string. */

	/* 中文翻译：如果 XINFO_HAS_GID 则遵循twophase_gid。作为以 null 结尾的字符串。 */
	/* xl_xact_origin follows if XINFO_HAS_ORIGIN, stored unaligned! */

	/* 中文翻译：如果 XINFO_HAS_ORIGIN 则遵循 xl_xact_origin，未对齐存储！ */
} xl_xact_abort;
#define MinSizeOfXactAbort sizeof(xl_xact_abort)

typedef struct xl_xact_prepare
{
	uint32		magic;			/* format identifier */

	/* 格式标识符。 */
	uint32		total_len;		/* actual file length */

	/* 实际文件长度。 */
	TransactionId xid;			/* original transaction XID */

	/* 原始事务 XID。 */
	Oid			database;		/* OID of database it was in */

	/* 所在数据库的 OID。 */
	TimestampTz prepared_at;	/* time of preparation */

	/* 准备时间。 */
	Oid			owner;			/* user running the transaction */

	/* 运行该事务的用户。 */
	int32		nsubxacts;		/* number of following subxact XIDs */

	/* 中文翻译：以下 subxact XID 的数量 */
	int32		ncommitrels;	/* number of delete-on-commit rels */

	/* 中文翻译：提交时删除 rel 的数量 */
	int32		nabortrels;		/* number of delete-on-abort rels */

	/* 中文翻译：中止删除相关数 */
	int32		ncommitstats;	/* number of stats to drop on commit */

	/* 中文翻译：提交时要删除的统计数据数量 */
	int32		nabortstats;	/* number of stats to drop on abort */

	/* 中文翻译：中止时丢弃的统计数据数量 */
	int32		ninvalmsgs;		/* number of cache invalidation messages */

	/* 中文翻译：缓存失效消息数 */
	bool		initfileinval;	/* does relcache init file need invalidation? */

	/* 中文翻译：relcache init 文件需要失效吗？ */
	uint16		gidlen;			/* length of the GID - GID follows the header */

	/* 中文翻译：GID 的长度 - GID 位于标头后面 */
	XLogRecPtr	origin_lsn;		/* lsn of this record at origin node */

	/* 中文翻译：该记录在原始节点的lsn */
	TimestampTz origin_timestamp;	/* time of prepare at origin node */

	/* 中文翻译：源节点准备时间 */
} xl_xact_prepare;

/*
 * Commit/Abort records in the above form are a bit verbose to parse, so
 * there's a deconstructed versions generated by ParseCommit/AbortRecord() for
 * easier consumption.
 *
 * 上述格式的提交/中止记录解析较冗长，因此 ParseCommit/AbortRecord() 会生成拆解后的版本以便消费。
 */
typedef struct xl_xact_parsed_commit
{
	TimestampTz xact_time;
	uint32		xinfo;

	Oid			dbId;			/* MyDatabaseId */

	/* MyDatabaseId。 */
	Oid			tsId;			/* MyDatabaseTableSpace */

	/* MyDatabaseTableSpace。 */

	int			nsubxacts;
	TransactionId *subxacts;

	int			nrels;
	RelFileLocator *xlocators;

	int			nstats;
	xl_xact_stats_item *stats;

	int			nmsgs;
	SharedInvalidationMessage *msgs;

	TransactionId twophase_xid; /* only for 2PC */

	/* 中文翻译：仅限 2 件 */
	char		twophase_gid[GIDSIZE];	/* only for 2PC */

	/* 中文翻译：仅限 2 件 */
	int			nabortrels;		/* only for 2PC */

	/* 中文翻译：仅限 2 件 */
	RelFileLocator *abortlocators;	/* only for 2PC */

	/* 中文翻译：仅限 2 件 */
	int			nabortstats;	/* only for 2PC */

	/* 中文翻译：仅限 2 件 */
	xl_xact_stats_item *abortstats; /* only for 2PC */

	/* 中文翻译：仅限 2 件 */

	XLogRecPtr	origin_lsn;
	TimestampTz origin_timestamp;
} xl_xact_parsed_commit;

typedef xl_xact_parsed_commit xl_xact_parsed_prepare;

typedef struct xl_xact_parsed_abort
{
	TimestampTz xact_time;
	uint32		xinfo;

	Oid			dbId;			/* MyDatabaseId */

	/* MyDatabaseId。 */
	Oid			tsId;			/* MyDatabaseTableSpace */

	/* MyDatabaseTableSpace。 */

	int			nsubxacts;
	TransactionId *subxacts;

	int			nrels;
	RelFileLocator *xlocators;

	int			nstats;
	xl_xact_stats_item *stats;

	TransactionId twophase_xid; /* only for 2PC */

	/* 中文翻译：仅限 2 件 */
	char		twophase_gid[GIDSIZE];	/* only for 2PC */

	/* 中文翻译：仅限 2 件 */

	XLogRecPtr	origin_lsn;
	TimestampTz origin_timestamp;
} xl_xact_parsed_abort;


/* ----------------
 *		extern definitions
 *
 * 外部定义。
 * ----------------
 */
/*
 * Function: IsTransactionState.
 * Purpose: Obtains or checks the transaction state represented by is transaction state.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsTransactionState。
 * 作用：获取或检查 is transaction state 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsTransactionState(void);
/*
 * Function: IsAbortedTransactionBlockState.
 * Purpose: Obtains or checks the transaction state represented by is aborted transaction block state.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsAbortedTransactionBlockState。
 * 作用：获取或检查 is aborted transaction block state 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsAbortedTransactionBlockState(void);
/*
 * Function: GetTopTransactionId.
 * Purpose: Obtains or checks the transaction state represented by get top transaction id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetTopTransactionId。
 * 作用：获取或检查 get top transaction id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TransactionId GetTopTransactionId(void);
/*
 * Function: GetTopTransactionIdIfAny.
 * Purpose: Obtains or checks the transaction state represented by get top transaction id if any.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetTopTransactionIdIfAny。
 * 作用：获取或检查 get top transaction id if any 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TransactionId GetTopTransactionIdIfAny(void);
/*
 * Function: GetCurrentTransactionId.
 * Purpose: Obtains or checks the transaction state represented by get current transaction id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentTransactionId。
 * 作用：获取或检查 get current transaction id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TransactionId GetCurrentTransactionId(void);
/*
 * Function: GetCurrentTransactionIdIfAny.
 * Purpose: Obtains or checks the transaction state represented by get current transaction id if any.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentTransactionIdIfAny。
 * 作用：获取或检查 get current transaction id if any 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TransactionId GetCurrentTransactionIdIfAny(void);
/*
 * Function: GetStableLatestTransactionId.
 * Purpose: Obtains or checks the transaction state represented by get stable latest transaction id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetStableLatestTransactionId。
 * 作用：获取或检查 get stable latest transaction id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TransactionId GetStableLatestTransactionId(void);
/*
 * Function: GetCurrentSubTransactionId.
 * Purpose: Obtains or checks the transaction state represented by get current sub transaction id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentSubTransactionId。
 * 作用：获取或检查 get current sub transaction id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern SubTransactionId GetCurrentSubTransactionId(void);
/*
 * Function: GetTopFullTransactionId.
 * Purpose: Obtains or checks the transaction state represented by get top full transaction id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetTopFullTransactionId。
 * 作用：获取或检查 get top full transaction id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern FullTransactionId GetTopFullTransactionId(void);
/*
 * Function: GetTopFullTransactionIdIfAny.
 * Purpose: Obtains or checks the transaction state represented by get top full transaction id if any.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetTopFullTransactionIdIfAny。
 * 作用：获取或检查 get top full transaction id if any 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern FullTransactionId GetTopFullTransactionIdIfAny(void);
/*
 * Function: GetCurrentFullTransactionId.
 * Purpose: Obtains or checks the transaction state represented by get current full transaction id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentFullTransactionId。
 * 作用：获取或检查 get current full transaction id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern FullTransactionId GetCurrentFullTransactionId(void);
/*
 * Function: GetCurrentFullTransactionIdIfAny.
 * Purpose: Obtains or checks the transaction state represented by get current full transaction id if any.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentFullTransactionIdIfAny。
 * 作用：获取或检查 get current full transaction id if any 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern FullTransactionId GetCurrentFullTransactionIdIfAny(void);
/*
 * Function: MarkCurrentTransactionIdLoggedIfAny.
 * Purpose: Updates or enforces the transaction state represented by mark current transaction id logged if any.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：MarkCurrentTransactionIdLoggedIfAny。
 * 作用：更新或约束 mark current transaction id logged if any 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void MarkCurrentTransactionIdLoggedIfAny(void);
/*
 * Function: SubTransactionIsActive.
 * Purpose: Performs the transaction operation represented by sub transaction is active.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：SubTransactionIsActive。
 * 作用：执行 sub transaction is active 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern bool SubTransactionIsActive(SubTransactionId subxid);
/*
 * Function: GetCurrentCommandId.
 * Purpose: Obtains or checks the transaction state represented by get current command id.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentCommandId。
 * 作用：获取或检查 get current command id 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern CommandId GetCurrentCommandId(bool used);
/*
 * Function: SetParallelStartTimestamps.
 * Purpose: Updates or enforces the transaction state represented by set parallel start timestamps.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：SetParallelStartTimestamps。
 * 作用：更新或约束 set parallel start timestamps 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void SetParallelStartTimestamps(TimestampTz xact_ts, TimestampTz stmt_ts);
/*
 * Function: GetCurrentTransactionStartTimestamp.
 * Purpose: Obtains or checks the transaction state represented by get current transaction start timestamp.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentTransactionStartTimestamp。
 * 作用：获取或检查 get current transaction start timestamp 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TimestampTz GetCurrentTransactionStartTimestamp(void);
/*
 * Function: GetCurrentStatementStartTimestamp.
 * Purpose: Obtains or checks the transaction state represented by get current statement start timestamp.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentStatementStartTimestamp。
 * 作用：获取或检查 get current statement start timestamp 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TimestampTz GetCurrentStatementStartTimestamp(void);
/*
 * Function: GetCurrentTransactionStopTimestamp.
 * Purpose: Obtains or checks the transaction state represented by get current transaction stop timestamp.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentTransactionStopTimestamp。
 * 作用：获取或检查 get current transaction stop timestamp 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern TimestampTz GetCurrentTransactionStopTimestamp(void);
/*
 * Function: SetCurrentStatementStartTimestamp.
 * Purpose: Updates or enforces the transaction state represented by set current statement start timestamp.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：SetCurrentStatementStartTimestamp。
 * 作用：更新或约束 set current statement start timestamp 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void SetCurrentStatementStartTimestamp(void);
/*
 * Function: GetCurrentTransactionNestLevel.
 * Purpose: Obtains or checks the transaction state represented by get current transaction nest level.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：GetCurrentTransactionNestLevel。
 * 作用：获取或检查 get current transaction nest level 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern int	GetCurrentTransactionNestLevel(void);
/*
 * Function: TransactionIdIsCurrentTransactionId.
 * Purpose: Performs the transaction operation represented by transaction id is current transaction id.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：TransactionIdIsCurrentTransactionId。
 * 作用：执行 transaction id is current transaction id 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
/*
 * Function: CommandCounterIncrement.
 * Purpose: Performs the transaction operation represented by command counter increment.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：CommandCounterIncrement。
 * 作用：执行 command counter increment 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void CommandCounterIncrement(void);
/*
 * Function: ForceSyncCommit.
 * Purpose: Updates or enforces the transaction state represented by force sync commit.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：ForceSyncCommit。
 * 作用：更新或约束 force sync commit 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void ForceSyncCommit(void);
/*
 * Function: StartTransactionCommand.
 * Purpose: Starts the transaction operation represented by start transaction command.
 * Core flow: It creates the required transaction context, records its state, and makes it available to following commands.
 *
 * 函数：StartTransactionCommand。
 * 作用：启动 start transaction command 所表示的事务操作。
 * 核心流程：它创建所需事务上下文，记录其状态，并使后续命令可以使用。
 */
extern void StartTransactionCommand(void);
/*
 * Function: SaveTransactionCharacteristics.
 * Purpose: Updates or enforces the transaction state represented by save transaction characteristics.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：SaveTransactionCharacteristics。
 * 作用：更新或约束 save transaction characteristics 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void SaveTransactionCharacteristics(SavedTransactionCharacteristics *s);
/*
 * Function: RestoreTransactionCharacteristics.
 * Purpose: Updates or enforces the transaction state represented by restore transaction characteristics.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：RestoreTransactionCharacteristics。
 * 作用：更新或约束 restore transaction characteristics 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void RestoreTransactionCharacteristics(const SavedTransactionCharacteristics *s);
/*
 * Function: CommitTransactionCommand.
 * Purpose: Completes the transaction operation represented by commit transaction command.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：CommitTransactionCommand。
 * 作用：完成 commit transaction command 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void CommitTransactionCommand(void);
/*
 * Function: AbortCurrentTransaction.
 * Purpose: Completes the transaction operation represented by abort current transaction.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：AbortCurrentTransaction。
 * 作用：完成 abort current transaction 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void AbortCurrentTransaction(void);
/*
 * Function: BeginTransactionBlock.
 * Purpose: Starts the transaction operation represented by begin transaction block.
 * Core flow: It creates the required transaction context, records its state, and makes it available to following commands.
 *
 * 函数：BeginTransactionBlock。
 * 作用：启动 begin transaction block 所表示的事务操作。
 * 核心流程：它创建所需事务上下文，记录其状态，并使后续命令可以使用。
 */
extern void BeginTransactionBlock(void);
/*
 * Function: EndTransactionBlock.
 * Purpose: Completes the transaction operation represented by end transaction block.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：EndTransactionBlock。
 * 作用：完成 end transaction block 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern bool EndTransactionBlock(bool chain);
/*
 * Function: PrepareTransactionBlock.
 * Purpose: Performs the transaction operation represented by prepare transaction block.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：PrepareTransactionBlock。
 * 作用：执行 prepare transaction block 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern bool PrepareTransactionBlock(const char *gid);
/*
 * Function: UserAbortTransactionBlock.
 * Purpose: Performs the transaction operation represented by user abort transaction block.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：UserAbortTransactionBlock。
 * 作用：执行 user abort transaction block 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void UserAbortTransactionBlock(bool chain);
/*
 * Function: BeginImplicitTransactionBlock.
 * Purpose: Starts the transaction operation represented by begin implicit transaction block.
 * Core flow: It creates the required transaction context, records its state, and makes it available to following commands.
 *
 * 函数：BeginImplicitTransactionBlock。
 * 作用：启动 begin implicit transaction block 所表示的事务操作。
 * 核心流程：它创建所需事务上下文，记录其状态，并使后续命令可以使用。
 */
extern void BeginImplicitTransactionBlock(void);
/*
 * Function: EndImplicitTransactionBlock.
 * Purpose: Completes the transaction operation represented by end implicit transaction block.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：EndImplicitTransactionBlock。
 * 作用：完成 end implicit transaction block 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void EndImplicitTransactionBlock(void);
/*
 * Function: ReleaseSavepoint.
 * Purpose: Completes the transaction operation represented by release savepoint.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：ReleaseSavepoint。
 * 作用：完成 release savepoint 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void ReleaseSavepoint(const char *name);
/*
 * Function: DefineSavepoint.
 * Purpose: Updates or enforces the transaction state represented by define savepoint.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：DefineSavepoint。
 * 作用：更新或约束 define savepoint 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void DefineSavepoint(const char *name);
/*
 * Function: RollbackToSavepoint.
 * Purpose: Completes the transaction operation represented by rollback to savepoint.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：RollbackToSavepoint。
 * 作用：完成 rollback to savepoint 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void RollbackToSavepoint(const char *name);
/*
 * Function: BeginInternalSubTransaction.
 * Purpose: Starts the transaction operation represented by begin internal sub transaction.
 * Core flow: It creates the required transaction context, records its state, and makes it available to following commands.
 *
 * 函数：BeginInternalSubTransaction。
 * 作用：启动 begin internal sub transaction 所表示的事务操作。
 * 核心流程：它创建所需事务上下文，记录其状态，并使后续命令可以使用。
 */
extern void BeginInternalSubTransaction(const char *name);
/*
 * Function: ReleaseCurrentSubTransaction.
 * Purpose: Completes the transaction operation represented by release current sub transaction.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：ReleaseCurrentSubTransaction。
 * 作用：完成 release current sub transaction 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void ReleaseCurrentSubTransaction(void);
/*
 * Function: RollbackAndReleaseCurrentSubTransaction.
 * Purpose: Completes the transaction operation represented by rollback and release current sub transaction.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：RollbackAndReleaseCurrentSubTransaction。
 * 作用：完成 rollback and release current sub transaction 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void RollbackAndReleaseCurrentSubTransaction(void);
/*
 * Function: IsSubTransaction.
 * Purpose: Obtains or checks the transaction state represented by is sub transaction.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsSubTransaction。
 * 作用：获取或检查 is sub transaction 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsSubTransaction(void);
/*
 * Function: EstimateTransactionStateSpace.
 * Purpose: Performs the transaction operation represented by estimate transaction state space.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：EstimateTransactionStateSpace。
 * 作用：执行 estimate transaction state space 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern Size EstimateTransactionStateSpace(void);
/*
 * Function: SerializeTransactionState.
 * Purpose: Performs the transaction operation represented by serialize transaction state.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：SerializeTransactionState。
 * 作用：执行 serialize transaction state 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void SerializeTransactionState(Size maxsize, char *start_address);
/*
 * Function: StartParallelWorkerTransaction.
 * Purpose: Starts the transaction operation represented by start parallel worker transaction.
 * Core flow: It creates the required transaction context, records its state, and makes it available to following commands.
 *
 * 函数：StartParallelWorkerTransaction。
 * 作用：启动 start parallel worker transaction 所表示的事务操作。
 * 核心流程：它创建所需事务上下文，记录其状态，并使后续命令可以使用。
 */
extern void StartParallelWorkerTransaction(char *tstatespace);
/*
 * Function: EndParallelWorkerTransaction.
 * Purpose: Completes the transaction operation represented by end parallel worker transaction.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：EndParallelWorkerTransaction。
 * 作用：完成 end parallel worker transaction 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void EndParallelWorkerTransaction(void);
/*
 * Function: IsTransactionBlock.
 * Purpose: Obtains or checks the transaction state represented by is transaction block.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsTransactionBlock。
 * 作用：获取或检查 is transaction block 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsTransactionBlock(void);
/*
 * Function: IsTransactionOrTransactionBlock.
 * Purpose: Obtains or checks the transaction state represented by is transaction or transaction block.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsTransactionOrTransactionBlock。
 * 作用：获取或检查 is transaction or transaction block 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsTransactionOrTransactionBlock(void);
/*
 * Function: TransactionBlockStatusCode.
 * Purpose: Performs the transaction operation represented by transaction block status code.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：TransactionBlockStatusCode。
 * 作用：执行 transaction block status code 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern char TransactionBlockStatusCode(void);
/*
 * Function: AbortOutOfAnyTransaction.
 * Purpose: Completes the transaction operation represented by abort out of any transaction.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：AbortOutOfAnyTransaction。
 * 作用：完成 abort out of any transaction 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void AbortOutOfAnyTransaction(void);
/*
 * Function: PreventInTransactionBlock.
 * Purpose: Updates or enforces the transaction state represented by prevent in transaction block.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：PreventInTransactionBlock。
 * 作用：更新或约束 prevent in transaction block 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void PreventInTransactionBlock(bool isTopLevel, const char *stmtType);
/*
 * Function: RequireTransactionBlock.
 * Purpose: Updates or enforces the transaction state represented by require transaction block.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：RequireTransactionBlock。
 * 作用：更新或约束 require transaction block 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void RequireTransactionBlock(bool isTopLevel, const char *stmtType);
/*
 * Function: WarnNoTransactionBlock.
 * Purpose: Updates or enforces the transaction state represented by warn no transaction block.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：WarnNoTransactionBlock。
 * 作用：更新或约束 warn no transaction block 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void WarnNoTransactionBlock(bool isTopLevel, const char *stmtType);
/*
 * Function: IsInTransactionBlock.
 * Purpose: Obtains or checks the transaction state represented by is in transaction block.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsInTransactionBlock。
 * 作用：获取或检查 is in transaction block 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsInTransactionBlock(bool isTopLevel);
/*
 * Function: RegisterXactCallback.
 * Purpose: Performs the transaction operation represented by register xact callback.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：RegisterXactCallback。
 * 作用：执行 register xact callback 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void RegisterXactCallback(XactCallback callback, void *arg);
/*
 * Function: UnregisterXactCallback.
 * Purpose: Performs the transaction operation represented by unregister xact callback.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：UnregisterXactCallback。
 * 作用：执行 unregister xact callback 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void UnregisterXactCallback(XactCallback callback, void *arg);
/*
 * Function: RegisterSubXactCallback.
 * Purpose: Performs the transaction operation represented by register sub xact callback.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：RegisterSubXactCallback。
 * 作用：执行 register sub xact callback 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void RegisterSubXactCallback(SubXactCallback callback, void *arg);
/*
 * Function: UnregisterSubXactCallback.
 * Purpose: Performs the transaction operation represented by unregister sub xact callback.
 * Core flow: It uses the supplied arguments and transaction context to produce the requested result.
 *
 * 函数：UnregisterSubXactCallback。
 * 作用：执行 unregister sub xact callback 所表示的事务操作。
 * 核心流程：它使用给定参数和事务上下文生成请求的结果。
 */
extern void UnregisterSubXactCallback(SubXactCallback callback, void *arg);

/*
 * Function: IsSubxactTopXidLogPending.
 * Purpose: Obtains or checks the transaction state represented by is subxact top xid log pending.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsSubxactTopXidLogPending。
 * 作用：获取或检查 is subxact top xid log pending 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsSubxactTopXidLogPending(void);
/*
 * Function: MarkSubxactTopXidLogged.
 * Purpose: Updates or enforces the transaction state represented by mark subxact top xid logged.
 * Core flow: It validates the request, changes or checks the transaction context, and keeps later transaction processing consistent.
 *
 * 函数：MarkSubxactTopXidLogged。
 * 作用：更新或约束 mark subxact top xid logged 所表示的事务状态。
 * 核心流程：它校验请求，变更或检查事务上下文，并保持后续事务处理一致。
 */
extern void MarkSubxactTopXidLogged(void);

/*
 * Function: xactGetCommittedChildren.
 * Purpose: Performs the transaction WAL operation represented by xact get committed children.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：xactGetCommittedChildren。
 * 作用：执行 xact get committed children 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern int	xactGetCommittedChildren(TransactionId **ptr);

/*
 * Function: XactLogCommitRecord.
 * Purpose: Performs the transaction WAL operation represented by xact log commit record.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：XactLogCommitRecord。
 * 作用：执行 xact log commit record 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern XLogRecPtr XactLogCommitRecord(TimestampTz commit_time,
									  int nsubxacts, TransactionId *subxacts,
									  int nrels, RelFileLocator *rels,
									  int ndroppedstats,
									  xl_xact_stats_item *droppedstats,
									  int nmsgs, SharedInvalidationMessage *msgs,
									  bool relcacheInval,
									  int xactflags,
									  TransactionId twophase_xid,
									  const char *twophase_gid);

/*
 * Function: XactLogAbortRecord.
 * Purpose: Performs the transaction WAL operation represented by xact log abort record.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：XactLogAbortRecord。
 * 作用：执行 xact log abort record 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern XLogRecPtr XactLogAbortRecord(TimestampTz abort_time,
									 int nsubxacts, TransactionId *subxacts,
									 int nrels, RelFileLocator *rels,
									 int ndroppedstats,
									 xl_xact_stats_item *droppedstats,
									 int xactflags, TransactionId twophase_xid,
									 const char *twophase_gid);
/*
 * Function: xact_redo.
 * Purpose: Performs the transaction WAL operation represented by xact redo.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：xact_redo。
 * 作用：执行 xact redo 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern void xact_redo(XLogReaderState *record);

/* xactdesc.c */

/* xactdesc.c 中的实现。 */
/*
 * Function: xact_desc.
 * Purpose: Performs the transaction WAL operation represented by xact desc.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：xact_desc。
 * 作用：执行 xact desc 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern void xact_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function: xact_identify.
 * Purpose: Performs the transaction WAL operation represented by xact identify.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：xact_identify。
 * 作用：执行 xact identify 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern const char *xact_identify(uint8 info);

/* also in xactdesc.c, so they can be shared between front/backend code */

/* 也位于 xactdesc.c，以便前端和后端代码共享。 */
/*
 * Function: ParseCommitRecord.
 * Purpose: Performs the transaction WAL operation represented by parse commit record.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：ParseCommitRecord。
 * 作用：执行 parse commit record 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern void ParseCommitRecord(uint8 info, xl_xact_commit *xlrec, xl_xact_parsed_commit *parsed);
/*
 * Function: ParseAbortRecord.
 * Purpose: Performs the transaction WAL operation represented by parse abort record.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：ParseAbortRecord。
 * 作用：执行 parse abort record 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern void ParseAbortRecord(uint8 info, xl_xact_abort *xlrec, xl_xact_parsed_abort *parsed);
/*
 * Function: ParsePrepareRecord.
 * Purpose: Performs the transaction WAL operation represented by parse prepare record.
 * Core flow: It interprets the supplied transaction or WAL record and records, replays, or reports the resulting state.
 *
 * 函数：ParsePrepareRecord。
 * 作用：执行 parse prepare record 所表示的事务 WAL 操作。
 * 核心流程：它解释给定事务或 WAL 记录，并记录、重放或报告生成的状态。
 */
extern void ParsePrepareRecord(uint8 info, xl_xact_prepare *xlrec, xl_xact_parsed_prepare *parsed);

/*
 * Function: EnterParallelMode.
 * Purpose: Starts the transaction operation represented by enter parallel mode.
 * Core flow: It creates the required transaction context, records its state, and makes it available to following commands.
 *
 * 函数：EnterParallelMode。
 * 作用：启动 enter parallel mode 所表示的事务操作。
 * 核心流程：它创建所需事务上下文，记录其状态，并使后续命令可以使用。
 */
extern void EnterParallelMode(void);
/*
 * Function: ExitParallelMode.
 * Purpose: Completes the transaction operation represented by exit parallel mode.
 * Core flow: It applies commit, abort, release, or cleanup work and leaves the transaction state ready for subsequent processing.
 *
 * 函数：ExitParallelMode。
 * 作用：完成 exit parallel mode 所表示的事务操作。
 * 核心流程：它执行提交、中止、释放或清理工作，并使事务状态可供后续处理使用。
 */
extern void ExitParallelMode(void);
/*
 * Function: IsInParallelMode.
 * Purpose: Obtains or checks the transaction state represented by is in parallel mode.
 * Core flow: It examines the current transaction context and supplied identifiers, then returns the derived result.
 *
 * 函数：IsInParallelMode。
 * 作用：获取或检查 is in parallel mode 所表示的事务状态。
 * 核心流程：它检查当前事务上下文和给定标识符，然后返回推导结果。
 */
extern bool IsInParallelMode(void);

#endif							/* XACT_H */
