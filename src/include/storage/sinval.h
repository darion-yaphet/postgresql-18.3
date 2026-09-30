/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 *	  POSTGRES 共享缓存失效通信定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sinval.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include <signal.h>

#include "storage/relfilelocator.h"

/*
 * We support several types of shared-invalidation messages:
 *	* invalidate a specific tuple in a specific catcache
 *	* invalidate all catcache entries from a given system catalog
 *	* invalidate a relcache entry for a specific logical relation
 *	* invalidate all relcache entries
 *	* invalidate an smgr cache entry for a specific physical relation
 *	* invalidate the mapped-relation mapping for a given database
 *	* invalidate any saved snapshot that might be used to scan a given relation
 *	* invalidate a RelationSyncCache entry for a specific relation
 * More types could be added if needed.  The message type is identified by
 * the first "int8" field of the message struct.  Zero or positive means a
 * specific-catcache inval message (and also serves as the catcache ID field).
 * Negative values identify the other message types, as per codes below.
 *
 * Catcache inval events are initially driven by detecting tuple inserts,
 * updates and deletions in system catalogs (see CacheInvalidateHeapTuple).
 * An update can generate two inval events, one for the old tuple and one for
 * the new, but this is reduced to one event if the tuple's hash key doesn't
 * change.  Note that the inval events themselves don't actually say whether
 * the tuple is being inserted or deleted.  Also, since we transmit only a
 * hash key, there is a small risk of unnecessary invalidations due to chance
 * matches of hash keys.
 *
 * Note that some system catalogs have multiple caches on them (with different
 * indexes).  On detecting a tuple invalidation in such a catalog, separate
 * catcache inval messages must be generated for each of its caches, since
 * the hash keys will generally be different.
 *
 * Catcache, relcache, relsynccache, and snapshot invalidations are
 * transactional, and so are sent to other backends upon commit.  Internally
 * to the generating backend, they are also processed at
 * CommandCounterIncrement so that later commands in the same transaction see
 * the new state.  The generating backend also has to process them at abort,
 * to flush out any cache state it's loaded from no-longer-valid entries.
 *
 * smgr and relation mapping invalidations are non-transactional: they are
 * sent immediately when the underlying file change is made.
 */

/*
 * 我们支持多种共享失效消息：
 *	* 使特定 catcache 中的特定元组失效
 *	* 使给定系统目录中的全部 catcache 条目失效
 *	* 使特定逻辑关系的 relcache 条目失效
 *	* 使全部 relcache 条目失效
 *	* 使特定物理关系的 smgr 缓存条目失效
 *	* 使给定数据库的映射关系映射失效
 *	* 使可能用于扫描给定关系的已保存快照失效
 *	* 使特定关系的 RelationSyncCache 条目失效
 * 如有需要还可以加入更多类型。消息类型由消息结构的第一个 “int8” 字段标识。
 * 零或正值表示特定 catcache 失效消息（也兼作 catcache ID 字段）。
 * 负值按照以下代码标识其他消息类型。
 *
 * Catcache 失效事件最初由检测系统目录中元组的插入、更新和删除驱动
 * （见 CacheInvalidateHeapTuple）。一次更新可能为旧元组和新元组各生成一个
 * 失效事件；若元组的哈希键未变化，则会缩减为一个事件。请注意，失效事件本身
 * 并不说明元组是在插入还是删除。而且由于只传输哈希键，哈希键偶然匹配会带来
 * 少量不必要失效的风险。
 *
 * 请注意，一些系统目录上有多个缓存（使用不同索引）。检测到此类目录中的元组
 * 失效时，必须为其每个缓存分别生成 catcache 失效消息，因为哈希键通常不同。
 *
 * Catcache、relcache、relsynccache 与快照失效是事务性的，因此会在提交时发送
 * 给其他后端。在产生消息的后端内部，也会在 CommandCounterIncrement 时处理它们，
 * 使同一事务中的后续命令看到新状态。产生消息的后端还必须在中止时处理它们，
 * 以清除从已不再有效条目加载的缓存状态。
 *
 * smgr 和关系映射失效不是事务性的：底层文件发生变更时会立即发送。
 */

typedef struct
{
	int8		id;				/* cache ID --- must be first */

							/* 缓存 ID —— 必须位于首位。 */
	Oid			dbId;			/* database ID, or 0 if a shared relation */

							/* 数据库 ID；共享关系时为 0。 */
	uint32		hashValue;		/* hash value of key for this catcache */

							/* 此 catcache 键的哈希值。 */
} SharedInvalCatcacheMsg;

#define SHAREDINVALCATALOG_ID	(-1)

typedef struct
{
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	Oid			dbId;			/* database ID, or 0 if a shared catalog */

							/* 数据库 ID；共享目录时为 0。 */
	Oid			catId;			/* ID of catalog whose contents are invalid */

							/* 内容需要失效的目录 ID。 */
} SharedInvalCatalogMsg;

#define SHAREDINVALRELCACHE_ID	(-2)

typedef struct
{
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	Oid			dbId;			/* database ID, or 0 if a shared relation */

							/* 数据库 ID；共享关系时为 0。 */
	Oid			relId;			/* relation ID, or 0 if whole relcache */

							/* 关系 ID；整个 relcache 时为 0。 */
} SharedInvalRelcacheMsg;

#define SHAREDINVALSMGR_ID		(-3)

typedef struct
{
	/* note: field layout chosen to pack into 16 bytes */

	/* 注意：选择此字段布局以封装为 16 字节。 */
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	int8		backend_hi;		/* high bits of backend procno, if temprel */

							/* 临时关系时后端进程号的高位。 */
	uint16		backend_lo;		/* low bits of backend procno, if temprel */

							/* 临时关系时后端进程号的低位。 */
	RelFileLocator rlocator;	/* spcOid, dbOid, relNumber */

							/* spcOid、dbOid、relNumber。 */
} SharedInvalSmgrMsg;

#define SHAREDINVALRELMAP_ID	(-4)

typedef struct
{
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	Oid			dbId;			/* database ID, or 0 for shared catalogs */

							/* 数据库 ID；共享目录时为 0。 */
} SharedInvalRelmapMsg;

#define SHAREDINVALSNAPSHOT_ID	(-5)

typedef struct
{
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	Oid			dbId;			/* database ID, or 0 if a shared relation */

							/* 数据库 ID；共享关系时为 0。 */
	Oid			relId;			/* relation ID */

							/* 关系 ID。 */
} SharedInvalSnapshotMsg;

#define SHAREDINVALRELSYNC_ID	(-6)

typedef struct
{
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	Oid			dbId;			/* database ID */

							/* 数据库 ID。 */
	Oid			relid;			/* relation ID, or 0 if whole
								 * RelationSyncCache */

							/* 关系 ID；整个 RelationSyncCache 时为 0。 */
} SharedInvalRelSyncMsg;

typedef union
{
	int8		id;				/* type field --- must be first */

							/* 类型字段 —— 必须位于首位。 */
	SharedInvalCatcacheMsg cc;
	SharedInvalCatalogMsg cat;
	SharedInvalRelcacheMsg rc;
	SharedInvalSmgrMsg sm;
	SharedInvalRelmapMsg rm;
	SharedInvalSnapshotMsg sn;
	SharedInvalRelSyncMsg rs;
} SharedInvalidationMessage;


/* Counter of messages processed; don't worry about overflow. */

/* 已处理消息的计数器；无需担心溢出。 */
extern PGDLLIMPORT uint64 SharedInvalidMessageCounter;

extern PGDLLIMPORT volatile sig_atomic_t catchupInterruptPending;

/* Send shared invalidation messages to the shared queue.
 * The messages are appended as a batch so other backends can process cache
 * changes at the appropriate transaction boundary.
 *
 * 将共享失效消息发送到共享队列。
 * 消息作为一个批次追加，使其他后端能在适当的事务边界处理缓存变更。
 */
extern void SendSharedInvalidMessages(const SharedInvalidationMessage *msgs,
									  int n);

/* Receive pending shared invalidation messages.
 * Each message is passed to invalFunction; a queue reset is reported through
 * resetFunction when incremental delivery is no longer possible.
 *
 * 接收待处理的共享失效消息。
 * 每条消息都会传给 invalFunction；无法再增量传递时，通过 resetFunction 报告队列重置。
 */
extern void ReceiveSharedInvalidMessages(void (*invalFunction) (SharedInvalidationMessage *msg),
										 void (*resetFunction) (void));

/* signal handler for catchup events (PROCSIG_CATCHUP_INTERRUPT) */

/* 用于追赶事件的信号处理程序（PROCSIG_CATCHUP_INTERRUPT）。 */

/* Mark a shared-invalidation catchup interrupt as pending.
 * The signal path records the event so normal backend processing can later
 * receive messages safely outside the signal handler.
 *
 * 将共享失效追赶中断标记为待处理。
 * 信号路径记录该事件，使常规后端处理稍后能在信号处理程序外安全接收消息。
 */
extern void HandleCatchupInterrupt(void);

/*
 * enable/disable processing of catchup events directly from signal handler.
 * The enable routine first performs processing of any catchup events that
 * have occurred since the last disable.
 */

/*
 * 直接在信号处理程序中启用或禁用追赶事件处理。
 * 启用例程会先处理自上次禁用以来发生的所有追赶事件。
 */

/* Process a pending catchup interrupt when direct handling is enabled.
 * It first consumes events accumulated while handling was disabled, then
 * keeps local cache state synchronized with the shared queue.
 *
 * 在启用直接处理时处理待处理的追赶中断。
 * 此函数先消费处理被禁用期间累积的事件，然后使本地缓存状态与共享队列保持同步。
 */
extern void ProcessCatchupInterrupt(void);

/* Return invalidation messages accumulated by the current transaction.
 * The routine exposes the commit-time batch and whether relcache init files
 * must be invalidated.
 *
 * 返回当前事务累积的失效消息。
 * 此例程提供提交时批次以及是否必须使 relcache 初始化文件失效。
 */
extern int	xactGetCommittedInvalidationMessages(SharedInvalidationMessage **msgs,
												 bool *RelcacheInitFileInval);

/* Return invalidation messages produced by in-place updates.
 * The routine collects the nontransactional batch and its relcache-init-file
 * invalidation flag for immediate processing.
 *
 * 返回就地更新产生的失效消息。
 * 此例程收集非事务性批次及其 relcache 初始化文件失效标志以供立即处理。
 */
extern int	inplaceGetInvalidationMessages(SharedInvalidationMessage **msgs,
										   bool *RelcacheInitFileInval);

/* Apply a committed batch of invalidation messages locally.
 * The routine dispatches all messages with the supplied database and
 * tablespace context, then handles any relcache-init-file invalidation.
 *
 * 在本地应用已提交的一批失效消息。
 * 此例程带着给定的数据库和表空间上下文分派所有消息，然后处理任何 relcache 初始化文件失效。
 */
extern void ProcessCommittedInvalidationMessages(SharedInvalidationMessage *msgs,
												 int nmsgs, bool RelcacheInitFileInval,
												 Oid dbid, Oid tsid);

/* Apply one invalidation message in the current backend.
 * The message type selects the appropriate local cache or storage action.
 *
 * 在当前后端中应用一条失效消息。
 * 消息类型会选择相应的本地缓存或存储操作。
 */
extern void LocalExecuteInvalidationMessage(SharedInvalidationMessage *msg);

#endif							/* SINVAL_H */
