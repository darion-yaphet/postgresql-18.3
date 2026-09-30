/*-------------------------------------------------------------------------
 * worker.c
 *	   PostgreSQL logical replication worker (apply)
 *
 * worker.c：PostgreSQL 逻辑复制 apply worker。
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/worker.c
 *
 * NOTES
 *	  This file contains the worker which applies logical changes as they come
 *	  from remote logical replication stream.
 *
 * NOTES：本文件包含 apply worker，它把远端逻辑复制流送来的变更应用到本地。
 *
 *	  The main worker (apply) is started by logical replication worker
 *	  launcher for every enabled subscription in a database. It uses
 *	  walsender protocol to communicate with publisher.
 *
 * 主 apply worker 由逻辑复制 worker launcher 为数据库中每个已启用的订阅启动。
 * 它使用 walsender 协议与发布端通信。
 *
 *	  This module includes server facing code and shares libpqwalreceiver
 *	  module with walreceiver for providing the libpq specific functionality.
 *
 * 本模块包含面向服务器的代码，并与 walreceiver 共用 libpqwalreceiver 模块，以提供
 * libpq 相关功能。
 *
 *
 * STREAMED TRANSACTIONS
 * ---------------------
 * Streamed transactions (large transactions exceeding a memory limit on the
 * upstream) are applied using one of two approaches:
 *
 * 流式事务
 * 上游超过内存上限的大事务采用以下两种方式之一来应用：
 *
 * 1) Write to temporary files and apply when the final commit arrives
 *
 * 1) 写入临时文件，等最终 commit 到达后再应用。
 *
 * This approach is used when the user has set the subscription's streaming
 * option as on.
 *
 * 当用户把订阅的 streaming 选项设为 on 时使用这种方式。
 *
 * Unlike the regular (non-streamed) case, handling streamed transactions has
 * to handle aborts of both the toplevel transaction and subtransactions. This
 * is achieved by tracking offsets for subtransactions, which is then used
 * to truncate the file with serialized changes.
 *
 * 与普通（非流式）情况不同，处理流式事务必须同时处理顶层事务和子事务的中止。做法是记录子事务的偏移，
 * 再用它截断已序列化变更的文件。
 *
 * The files are placed in tmp file directory by default, and the filenames
 * include both the XID of the toplevel transaction and OID of the
 * subscription. This is necessary so that different workers processing a
 * remote transaction with the same XID doesn't interfere.
 *
 * 文件默认放在临时文件目录，文件名同时包含顶层事务的 XID 和订阅的 OID。这样不同
 * worker 处理 XID 相同的远端事务时不会互相干扰。
 *
 * We use BufFiles instead of using normal temporary files because (a) the
 * BufFile infrastructure supports temporary files that exceed the OS file size
 * limit, (b) provides a way for automatic clean up on the error and (c) provides
 * a way to survive these files across local transactions and allow to open and
 * close at stream start and close. We decided to use FileSet
 * infrastructure as without that it deletes the files on the closure of the
 * file and if we decide to keep stream files open across the start/stop stream
 * then it will consume a lot of memory (more than 8K for each BufFile and
 * there could be multiple such BufFiles as the subscriber could receive
 * multiple start/stop streams for different transactions before getting the
 * commit). Moreover, if we don't use FileSet then we also need to invent
 * a new way to pass filenames to BufFile APIs so that we are allowed to open
 * the file we desired across multiple stream-open calls for the same
 * transaction.
 *
 * 我们使用 BufFile 而不是普通临时文件，因为：(a) BufFile 基础设施支持超过操作系统文件大小限制的临时文件；
 * (b) 出错时可以自动清理；(c) 这些文件可以跨本地事务保留，并在流开始和结束时打开和关闭。
 * 我们决定使用 FileSet 基础设施，否则关闭文件时就会删掉这些文件；如果让流文件在
 * start/stop stream 之间一直保持打开，则会占用大量内存（每个 BufFile 超过 8K，
 * 而且订阅端在收到 commit 之前可能为不同事务收到多次 start/stop stream，从而存在多个
 * BufFile）。此外，如果不用 FileSet，还得另想办法把文件名传给 BufFile API，才能在同一事务的多次
 * stream-open 调用中打开想要的文件。
 *
 * 2) Parallel apply workers.
 *
 * 2) 并行 apply worker。
 *
 * This approach is used when the user has set the subscription's streaming
 * option as parallel. See logical/applyparallelworker.c for information about
 * this approach.
 *
 * 当用户把订阅的 streaming 选项设为 parallel 时使用这种方式。详见 logical/applyparallelworker.c。
 *
 * TWO_PHASE TRANSACTIONS
 * ----------------------
 * Two phase transactions are replayed at prepare and then committed or
 * rolled back at commit prepared and rollback prepared respectively. It is
 * possible to have a prepared transaction that arrives at the apply worker
 * when the tablesync is busy doing the initial copy. In this case, the apply
 * worker skips all the prepared operations [e.g. inserts] while the tablesync
 * is still busy (see the condition of should_apply_changes_for_rel). The
 * tablesync worker might not get such a prepared transaction because say it
 * was prior to the initial consistent point but might have got some later
 * commits. Now, the tablesync worker will exit without doing anything for the
 * prepared transaction skipped by the apply worker as the sync location for it
 * will be already ahead of the apply worker's current location. This would lead
 * to an "empty prepare", because later when the apply worker does the commit
 * prepare, there is nothing in it (the inserts were skipped earlier).
 *
 * 两阶段事务
 * 两阶段事务在 prepare 时重放，然后分别在 commit prepared 和 rollback prepared
 * 时提交或回滚。有可能在 tablesync 忙于初始拷贝时，已准备事务到达 apply worker。
 * 此时只要 tablesync 仍在忙，apply worker 会跳过所有已准备操作，例如 INSERT（见
 * should_apply_changes_for_rel 的条件）。tablesync worker 可能收不到这样的已准备事务，
 * 例如它发生在初始一致点之前，但可能收到一些更晚的 commit。于是 tablesync worker
 * 退出时不会处理被 apply worker 跳过的已准备事务，因为它的同步位置已经超前于
 * apply worker 的当前位置。这会造成空的 prepare：稍后 apply worker 执行 commit
 * prepared 时里面什么都没有，因为 INSERT 先前已被跳过。
 *
 * To avoid this, and similar prepare confusions the subscription's two_phase
 * commit is enabled only after the initial sync is over. The two_phase option
 * has been implemented as a tri-state with values DISABLED, PENDING, and
 * ENABLED.
 *
 * 为避免这种情况以及类似的 prepare 混淆，订阅的 two_phase 提交只在初始同步结束后才启用。
 * two_phase 选项实现为三态：DISABLED、PENDING 和 ENABLED。
 *
 * Even if the user specifies they want a subscription with two_phase = on,
 * internally it will start with a tri-state of PENDING which only becomes
 * ENABLED after all tablesync initializations are completed - i.e. when all
 * tablesync workers have reached their READY state. In other words, the value
 * PENDING is only a temporary state for subscription start-up.
 *
 * 即使用户指定 two_phase = on，内部也会先从三态 PENDING 开始，只有全部 tablesync
 * 初始化完成，也就是所有 tablesync worker 都进入 READY 之后，才变为 ENABLED。
 * 换句话说，PENDING 只是订阅启动时的临时状态。
 *
 * Until the two_phase is properly available (ENABLED) the subscription will
 * behave as if two_phase = off. When the apply worker detects that all
 * tablesyncs have become READY (while the tri-state was PENDING) it will
 * restart the apply worker process. This happens in
 * process_syncing_tables_for_apply.
 *
 * 在 two_phase 真正可用（ENABLED）之前，订阅的行为如同 two_phase = off。当 apply
 * worker 发现所有 tablesync 都已 READY（此时三态仍是 PENDING）时，会重启 apply
 * worker 进程。这发生在 process_syncing_tables_for_apply 中。
 *
 * When the (re-started) apply worker finds that all tablesyncs are READY for a
 * two_phase tri-state of PENDING it start streaming messages with the
 * two_phase option which in turn enables the decoding of two-phase commits at
 * the publisher. Then, it updates the tri-state value from PENDING to ENABLED.
 * Now, it is possible that during the time we have not enabled two_phase, the
 * publisher (replication server) would have skipped some prepares but we
 * ensure that such prepares are sent along with commit prepare, see
 * ReorderBufferFinishPrepared.
 *
 * 重启后的 apply worker 若发现 two_phase 三态为 PENDING 且所有 tablesync 都已
 * READY，便开始以 two_phase 选项流式接收消息，从而在发布端启用两阶段提交的解码。
 * 然后把三态从 PENDING 更新为 ENABLED。在尚未启用 two_phase 的这段时间里，发布端（复制服务器）
 * 可能跳过了一些 prepare，但我们保证这些 prepare 会随 commit prepared 一起发送，
 * 见 ReorderBufferFinishPrepared。
 *
 * If the subscription has no tables then a two_phase tri-state PENDING is
 * left unchanged. This lets the user still do an ALTER SUBSCRIPTION REFRESH
 * PUBLICATION which might otherwise be disallowed (see below).
 *
 * 如果订阅没有表，two_phase 三态 PENDING 会保持不变。这样用户仍可执行 ALTER SUBSCRIPTION
 * REFRESH PUBLICATION，否则该命令可能被禁止（见下文）。
 *
 * If ever a user needs to be aware of the tri-state value, they can fetch it
 * from the pg_subscription catalog (see column subtwophasestate).
 *
 * 如果用户需要知道三态的值，可以从 pg_subscription 目录读取，见列 subtwophasestate。
 *
 * Finally, to avoid problems mentioned in previous paragraphs from any
 * subsequent (not READY) tablesyncs (need to toggle two_phase option from 'on'
 * to 'off' and then again back to 'on') there is a restriction for
 * ALTER SUBSCRIPTION REFRESH PUBLICATION. This command is not permitted when
 * the two_phase tri-state is ENABLED, except when copy_data = false.
 *
 * 最后，为避免前文所述问题出现在后续尚未 READY 的 tablesync 上（否则需要把 two_phase
 * 从 on 拨到 off 再拨回 on），对 ALTER SUBSCRIPTION REFRESH PUBLICATION 有限制。
 * 当 two_phase 三态为 ENABLED 时不允许该命令，除非 copy_data = false。
 *
 * We can get prepare of the same GID more than once for the genuine cases
 * where we have defined multiple subscriptions for publications on the same
 * server and prepared transaction has operations on tables subscribed to those
 * subscriptions. For such cases, if we use the GID sent by publisher one of
 * the prepares will be successful and others will fail, in which case the
 * server will send them again. Now, this can lead to a deadlock if user has
 * set synchronous_standby_names for all the subscriptions on subscriber. To
 * avoid such deadlocks, we generate a unique GID (consisting of the
 * subscription oid and the xid of the prepared transaction) for each prepare
 * transaction on the subscriber.
 *
 * 同一 GID 的 prepare 可能因正当原因到达多次：同一服务器上为发布定义了多个订阅，
 * 且已准备事务操作了这些订阅所订阅的表。这种情况下如果使用发布端发来的 GID，其中一个
 * prepare 会成功，其余会失败，服务器会再次发送它们。若用户为订阅端上所有订阅都设置了
 * synchronous_standby_names，这可能导致死锁。为避免这种死锁，我们在订阅端为每个
 * prepare 事务生成唯一 GID，由订阅 oid 和已准备事务的 xid 组成。
 *
 * FAILOVER
 * ----------------------
 * The logical slot on the primary can be synced to the standby by specifying
 * failover = true when creating the subscription. Enabling failover allows us
 * to smoothly transition to the promoted standby, ensuring that we can
 * subscribe to the new primary without losing any data.
 *
 * 故障转移
 * 创建订阅时指定 failover = true，可以把主库上的逻辑槽同步到备库。启用 failover
 * 后可以平滑切换到被提升的备库，确保订阅新主库时不丢失数据。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/table.h"
#include "access/tableam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/walwriter.h"
#include "replication/conflict.h"
#include "replication/logicallauncher.h"
#include "replication/logicalproto.h"
#include "replication/logicalrelation.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/walreceiver.h"
#include "replication/worker_internal.h"
#include "rewrite/rewriteHandler.h"
#include "storage/buffile.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/dynahash.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/usercontext.h"

#define NAPTIME_PER_CYCLE 1000	/* max sleep time between cycles (1s)
				 *
				 * 周期之间的最长休眠时间（1 秒）
				 */

typedef struct FlushPosition
{
	dlist_node	node;
	XLogRecPtr	local_end;
	XLogRecPtr	remote_end;
} FlushPosition;

static dlist_head lsn_mapping = DLIST_STATIC_INIT(lsn_mapping);

typedef struct ApplyExecutionData
{
	EState	   *estate;			/* executor state, used to track resources
						 *
						 * 执行器状态，用于跟踪资源
						 */

	LogicalRepRelMapEntry *targetRel;	/* replication target rel
						 *
						 * 复制目标关系
						 */
	ResultRelInfo *targetRelInfo;	/* ResultRelInfo for same
					 *
					 * 同一目标的 ResultRelInfo
					 */

	/* These fields are used when the target relation is partitioned:
	 *
	 * 目标关系为分区表时使用这些字段：
	 */
	ModifyTableState *mtstate;	/* dummy ModifyTable state
					 *
					 * 占位用的 ModifyTable 状态
					 */
	PartitionTupleRouting *proute;	/* partition routing info
					 *
					 * 分区路由信息
					 */
} ApplyExecutionData;

/* Struct for saving and restoring apply errcontext information
 *
 * 用于保存和恢复 apply 错误上下文信息的结构
 */
typedef struct ApplyErrorCallbackArg
{
	LogicalRepMsgType command;	/* 0 if invalid
					 *
					 * 无效时为 0
					 */
	LogicalRepRelMapEntry *rel;

	/* Remote node information
	 *
	 * 远端节点信息
	 */
	int			remote_attnum;	/* -1 if invalid
						 *
						 * 无效时为 -1
						 */
	TransactionId remote_xid;
	XLogRecPtr	finish_lsn;
	char	   *origin_name;
} ApplyErrorCallbackArg;

/*
 * The action to be taken for the changes in the transaction.
 *
 * 对该事务中的变更所采取的动作。
 *
 * TRANS_LEADER_APPLY:
 * This action means that we are in the leader apply worker or table sync
 * worker. The changes of the transaction are either directly applied or
 * are read from temporary files (for streaming transactions) and then
 * applied by the worker.
 *
 * TRANS_LEADER_APPLY：表示当前处于 leader apply worker 或表同步 worker。事务变更要么直接应用，
 * 要么对流式事务从临时文件读出后再由该 worker 应用。
 *
 * TRANS_LEADER_SERIALIZE:
 * This action means that we are in the leader apply worker or table sync
 * worker. Changes are written to temporary files and then applied when the
 * final commit arrives.
 *
 * TRANS_LEADER_SERIALIZE：表示当前处于 leader apply worker 或表同步 worker。变更写入临时文件，
 * 等最终 commit 到达后再应用。
 *
 * TRANS_LEADER_SEND_TO_PARALLEL:
 * This action means that we are in the leader apply worker and need to send
 * the changes to the parallel apply worker.
 *
 * TRANS_LEADER_SEND_TO_PARALLEL：表示当前处于 leader apply worker，需要把变更发给并行
 * apply worker。
 *
 * TRANS_LEADER_PARTIAL_SERIALIZE:
 * This action means that we are in the leader apply worker and have sent some
 * changes directly to the parallel apply worker and the remaining changes are
 * serialized to a file, due to timeout while sending data. The parallel apply
 * worker will apply these serialized changes when the final commit arrives.
 *
 * TRANS_LEADER_PARTIAL_SERIALIZE：表示当前处于 leader apply worker，已把部分变更直接发给并行
 * apply worker，其余变更因发送超时而序列化到文件。并行 apply worker 会在最终
 * commit 到达时应用这些已序列化变更。
 *
 * We can't use TRANS_LEADER_SERIALIZE for this case because, in addition to
 * serializing changes, the leader worker also needs to serialize the
 * STREAM_XXX message to a file, and wait for the parallel apply worker to
 * finish the transaction when processing the transaction finish command. So
 * this new action was introduced to keep the code and logic clear.
 *
 * 这种情况不能用 TRANS_LEADER_SERIALIZE，因为除了序列化变更，leader worker 还要把
 * STREAM_XXX 消息序列化到文件，并在处理事务结束命令时等待并行 apply worker 完成该事务。
 * 因此引入这个新动作，以保持代码和逻辑清晰。
 *
 * TRANS_PARALLEL_APPLY:
 * This action means that we are in the parallel apply worker and changes of
 * the transaction are applied directly by the worker.
 *
 * TRANS_PARALLEL_APPLY：表示当前处于并行 apply worker，事务变更由该 worker 直接应用。
 */
typedef enum
{
	/* The action for non-streaming transactions.
	 *
	 * 非流式事务使用的动作。
	 */
	TRANS_LEADER_APPLY,

	/* Actions for streaming transactions.
	 *
	 * 流式事务使用的动作。
	 */
	TRANS_LEADER_SERIALIZE,
	TRANS_LEADER_SEND_TO_PARALLEL,
	TRANS_LEADER_PARTIAL_SERIALIZE,
	TRANS_PARALLEL_APPLY,
} TransApplyAction;

/* errcontext tracker
 *
 * errcontext 跟踪器
 */
static ApplyErrorCallbackArg apply_error_callback_arg =
{
	.command = 0,
	.rel = NULL,
	.remote_attnum = -1,
	.remote_xid = InvalidTransactionId,
	.finish_lsn = InvalidXLogRecPtr,
	.origin_name = NULL,
};

ErrorContextCallback *apply_error_context_stack = NULL;

MemoryContext ApplyMessageContext = NULL;
MemoryContext ApplyContext = NULL;

/* per stream context for streaming transactions
 *
 * 流式事务的每个流所用的内存上下文
 */
static MemoryContext LogicalStreamingContext = NULL;

WalReceiverConn *LogRepWorkerWalRcvConn = NULL;

Subscription *MySubscription = NULL;
static bool MySubscriptionValid = false;

static List *on_commit_wakeup_workers_subids = NIL;

bool		in_remote_transaction = false;
static XLogRecPtr remote_final_lsn = InvalidXLogRecPtr;

/* fields valid only when processing streamed transaction
 *
 * 仅在处理流式事务时有效的字段
 */
static bool in_streamed_transaction = false;

static TransactionId stream_xid = InvalidTransactionId;

/*
 * The number of changes applied by parallel apply worker during one streaming
 * block.
 *
 * 并行 apply worker 在一个流式块中应用的变更数。
 */
static uint32 parallel_stream_nchanges = 0;

/* Are we initializing an apply worker?
 *
 * 是否正在初始化 apply worker？
 */
bool		InitializingApplyWorker = false;

/*
 * We enable skipping all data modification changes (INSERT, UPDATE, etc.) for
 * the subscription if the remote transaction's finish LSN matches the subskiplsn.
 * Once we start skipping changes, we don't stop it until we skip all changes of
 * the transaction even if pg_subscription is updated and MySubscription->skiplsn
 * gets changed or reset during that. Also, in streaming transaction cases (streaming = on),
 * we don't skip receiving and spooling the changes since we decide whether or not
 * to skip applying the changes when starting to apply changes. The subskiplsn is
 * cleared after successfully skipping the transaction or applying non-empty
 * transaction. The latter prevents the mistakenly specified subskiplsn from
 * being left. Note that we cannot skip the streaming transactions when using
 * parallel apply workers because we cannot get the finish LSN before applying
 * the changes. So, we don't start parallel apply worker when finish LSN is set
 * by the user.
 *
 * 若远端事务的结束 LSN 与 subskiplsn 匹配，则对该订阅跳过所有数据修改变更（INSERT、
 * UPDATE 等）。一旦开始跳过，就会跳过该事务的全部变更，即使期间 pg_subscription
 * 被更新、MySubscription->skiplsn 被修改或重置也不中途停止。另外，在流式事务（streaming
 * = on）中，我们不跳过接收和暂存变更，因为是否跳过应用要到开始应用变更时才决定。
 * 成功跳过该事务，或应用了非空事务之后，会清除 subskiplsn。后者是为了避免错误指定的
 * subskiplsn 一直留着。注意，使用并行 apply worker 时无法跳过流式事务，因为在应用变更之前拿不到结束
 * LSN。因此用户设置了结束 LSN 时，我们不启动并行 apply worker。
 */
static XLogRecPtr skip_xact_finish_lsn = InvalidXLogRecPtr;
#define is_skipping_changes() (unlikely(!XLogRecPtrIsInvalid(skip_xact_finish_lsn)))

/* BufFile handle of the current streaming file
 *
 * 当前流式文件的 BufFile 句柄
 */
static BufFile *stream_fd = NULL;

typedef struct SubXactInfo
{
	TransactionId xid;			/* XID of the subxact
						 *
						 * 子事务的 XID
						 */
	int			fileno;			/* file number in the buffile
							 *
							 * buffile 中的文件编号
							 */
	off_t		offset;			/* offset in the file
						 *
						 * 文件中的偏移
						 */
} SubXactInfo;

/* Sub-transaction data for the current streaming transaction
 *
 * 当前流式事务的子事务数据
 */
typedef struct ApplySubXactData
{
	uint32		nsubxacts;		/* number of sub-transactions
						 *
						 * 子事务数量
						 */
	uint32		nsubxacts_max;	/* current capacity of subxacts
					 *
					 * subxacts 的当前容量
					 */
	TransactionId subxact_last; /* xid of the last sub-transaction
				     *
				     * 最后一个子事务的 xid
				     */
	SubXactInfo *subxacts;		/* sub-xact offset in changes file
					 *
					 * 变更文件中的子事务偏移
					 */
} ApplySubXactData;

static ApplySubXactData subxact_data = {0, 0, InvalidTransactionId, NULL};

static inline void subxact_filename(char *path, Oid subid, TransactionId xid);
static inline void changes_filename(char *path, Oid subid, TransactionId xid);

/*
 * Information about subtransactions of a given toplevel transaction.
 *
 * 给定顶层事务的子事务信息。
 */
static void subxact_info_write(Oid subid, TransactionId xid);
static void subxact_info_read(Oid subid, TransactionId xid);
static void subxact_info_add(TransactionId xid);
static inline void cleanup_subxact_info(void);

/*
 * Serialize and deserialize changes for a toplevel transaction.
 *
 * 对顶层事务的变更做序列化与反序列化。
 */
static void stream_open_file(Oid subid, TransactionId xid,
							 bool first_segment);
static void stream_write_change(char action, StringInfo s);
static void stream_open_and_write_change(TransactionId xid, char action, StringInfo s);
static void stream_close_file(void);

static void send_feedback(XLogRecPtr recvpos, bool force, bool requestReply);

static void apply_handle_commit_internal(LogicalRepCommitData *commit_data);
static void apply_handle_insert_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot);
static void apply_handle_update_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot,
										 LogicalRepTupleData *newtup,
										 Oid localindexoid);
static void apply_handle_delete_internal(ApplyExecutionData *edata,
										 ResultRelInfo *relinfo,
										 TupleTableSlot *remoteslot,
										 Oid localindexoid);
static bool FindReplTupleInLocalRel(ApplyExecutionData *edata, Relation localrel,
									LogicalRepRelation *remoterel,
									Oid localidxoid,
									TupleTableSlot *remoteslot,
									TupleTableSlot **localslot);
static void apply_handle_tuple_routing(ApplyExecutionData *edata,
									   TupleTableSlot *remoteslot,
									   LogicalRepTupleData *newtup,
									   CmdType operation);

/* Functions for skipping changes
 *
 * 跳过变更所用的函数
 */
static void maybe_start_skipping_changes(XLogRecPtr finish_lsn);
static void stop_skipping_changes(void);
static void clear_subscription_skip_lsn(XLogRecPtr finish_lsn);

/* Functions for apply error callback
 *
 * apply 错误回调所用的函数
 */
static inline void set_apply_error_context_xact(TransactionId xid, XLogRecPtr lsn);
static inline void reset_apply_error_context_info(void);

static TransApplyAction get_transaction_apply_action(TransactionId xid,
													 ParallelApplyWorkerInfo **winfo);

static void replorigin_reset(int code, Datum arg);

/*
 * Form the origin name for the subscription.
 *
 * 为订阅构造 origin 名称。
 *
 * This is a common function for tablesync and other workers. Tablesync workers
 * must pass a valid relid. Other callers must pass relid = InvalidOid.
 *
 * 这是 tablesync 与其他 worker 共用的函数。tablesync worker 必须传入有效的 relid。
 * 其他调用方必须传入 relid = InvalidOid。
 *
 * Return the name in the supplied buffer.
 *
 * 把名称写入调用方提供的缓冲区。
 */
void
ReplicationOriginNameForLogicalRep(Oid suboid, Oid relid,
								   char *originname, Size szoriginname)
{
	if (OidIsValid(relid))
	{
		/* Replication origin name for tablesync workers.
		 *
		 * tablesync worker 使用的复制 origin 名称。
		 */
		snprintf(originname, szoriginname, "pg_%u_%u", suboid, relid);
	}
	else
	{
		/* Replication origin name for non-tablesync workers.
		 *
		 * 非 tablesync worker 使用的复制 origin 名称。
		 */
		snprintf(originname, szoriginname, "pg_%u", suboid);
	}
}

/*
 * Should this worker apply changes for given relation.
 *
 * 此 worker 是否应对给定关系应用变更。
 *
 * This is mainly needed for initial relation data sync as that runs in
 * separate worker process running in parallel and we need some way to skip
 * changes coming to the leader apply worker during the sync of a table.
 *
 * 这主要用于关系的初始数据同步：同步在并行的独立 worker 进程中进行，需要某种方式跳过表同步期间到达
 * leader apply worker 的变更。
 *
 * Note we need to do smaller or equals comparison for SYNCDONE state because
 * it might hold position of end of initial slot consistent point WAL
 * record + 1 (ie start of next record) and next record can be COMMIT of
 * transaction we are now processing (which is what we set remote_final_lsn
 * to in apply_handle_begin).
 *
 * 对 SYNCDONE 状态必须做小于或等于比较，因为它可能保存初始槽一致点 WAL 记录末尾的位置加
 * 1（即下一条记录的起点），而下一条记录可能是当前正在处理的事务的 COMMIT，也就是
 * apply_handle_begin 里设置 remote_final_lsn 的那个值。
 *
 * Note that for streaming transactions that are being applied in the parallel
 * apply worker, we disallow applying changes if the target table in the
 * subscription is not in the READY state, because we cannot decide whether to
 * apply the change as we won't know remote_final_lsn by that time.
 *
 * 对于正在并行 apply worker 中应用的流式事务，若订阅中的目标表不处于 READY 状态，
 * 则不允许应用变更，因为那时还不知道 remote_final_lsn，无法决定是否应用。
 *
 * We already checked this in pa_can_start() before assigning the
 * streaming transaction to the parallel worker, but it also needs to be
 * checked here because if the user executes ALTER SUBSCRIPTION ... REFRESH
 * PUBLICATION in parallel, the new table can be added to pg_subscription_rel
 * while applying this transaction.
 *
 * 在把流式事务分配给并行 worker 之前，pa_can_start() 已经检查过这一点，但这里仍需检查：
 * 用户若并行执行 ALTER SUBSCRIPTION ... REFRESH PUBLICATION，应用此事务期间可能把新表加入
 * pg_subscription_rel。
 */
static bool
should_apply_changes_for_rel(LogicalRepRelMapEntry *rel)
{
	switch (MyLogicalRepWorker->type)
	{
		case WORKERTYPE_TABLESYNC:
			return MyLogicalRepWorker->relid == rel->localreloid;

		case WORKERTYPE_PARALLEL_APPLY:
			/* We don't synchronize rel's that are in unknown state.
			 *
			 * 我们不同步处于未知状态的关系。
			 */
			if (rel->state != SUBREL_STATE_READY &&
				rel->state != SUBREL_STATE_UNKNOWN)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("logical replication parallel apply worker for subscription \"%s\" will stop",
								MySubscription->name),
						 errdetail("Cannot handle streamed replication transactions using parallel apply workers until all tables have been synchronized.")));

			return rel->state == SUBREL_STATE_READY;

		case WORKERTYPE_APPLY:
			return (rel->state == SUBREL_STATE_READY ||
					(rel->state == SUBREL_STATE_SYNCDONE &&
					 rel->statelsn <= remote_final_lsn));

		case WORKERTYPE_UNKNOWN:
			/* Should never happen.
			 *
			 * 不应发生。
			 */
			elog(ERROR, "Unknown worker type");
	}

	return false;				/* dummy for compiler
						 *
						 * 供编译器使用的哑返回值
						 */
}

/*
 * Begin one step (one INSERT, UPDATE, etc) of a replication transaction.
 *
 * 开始复制事务的一步（一次 INSERT、UPDATE 等）。
 *
 * Start a transaction, if this is the first step (else we keep using the
 * existing transaction).
 * Also provide a global snapshot and ensure we run in ApplyMessageContext.
 *
 * 若这是第一步则启动事务，否则继续使用已有事务。同时提供全局快照，并确保在 ApplyMessageContext
 * 中运行。
 */
static void
begin_replication_step(void)
{
	SetCurrentStatementStartTimestamp();

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		maybe_reread_subscription();
	}

	PushActiveSnapshot(GetTransactionSnapshot());

	MemoryContextSwitchTo(ApplyMessageContext);
}

/*
 * Finish up one step of a replication transaction.
 * Callers of begin_replication_step() must also call this.
 *
 * 结束复制事务的一步。begin_replication_step() 的调用方也必须调用本函数。
 *
 * We don't close out the transaction here, but we should increment
 * the command counter to make the effects of this step visible.
 *
 * 这里不结束事务，但应递增命令计数器，使这一步的效果可见。
 */
static void
end_replication_step(void)
{
	PopActiveSnapshot();

	CommandCounterIncrement();
}

/*
 * Handle streamed transactions for both the leader apply worker and the
 * parallel apply workers.
 *
 * 为 leader apply worker 和并行 apply worker 处理流式事务。
 *
 * In the streaming case (receiving a block of the streamed transaction), for
 * serialize mode, simply redirect it to a file for the proper toplevel
 * transaction, and for parallel mode, the leader apply worker will send the
 * changes to parallel apply workers and the parallel apply worker will define
 * savepoints if needed. (LOGICAL_REP_MSG_RELATION or LOGICAL_REP_MSG_TYPE
 * messages will be applied by both leader apply worker and parallel apply
 * workers).
 *
 * 在流式情况下（收到流式事务的一个块）：序列化模式只是把它转到对应顶层事务的文件；
 * 并行模式下，leader apply worker 把变更发给并行 apply worker，并行 apply worker
 * 在需要时定义保存点。LOGICAL_REP_MSG_RELATION 或 LOGICAL_REP_MSG_TYPE 消息会由
 * leader apply worker 和并行 apply worker 双方应用。
 *
 * Returns true for streamed transactions (when the change is either serialized
 * to file or sent to parallel apply worker), false otherwise (regular mode or
 * needs to be processed by parallel apply worker).
 *
 * 对流式事务返回 true（变更已序列化到文件或已发给并行 apply worker），否则返回
 * false（普通模式，或需要由并行 apply worker 处理）。
 *
 * Exception: If the message being processed is LOGICAL_REP_MSG_RELATION
 * or LOGICAL_REP_MSG_TYPE, return false even if the message needs to be sent
 * to a parallel apply worker.
 *
 * 例外：若正在处理的消息是 LOGICAL_REP_MSG_RELATION 或 LOGICAL_REP_MSG_TYPE，
 * 即使该消息需要发给并行 apply worker，也返回 false。
 */
static bool
handle_streamed_transaction(LogicalRepMsgType action, StringInfo s)
{
	TransactionId current_xid;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;
	StringInfoData original_msg;

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	/* not in streaming mode
	 *
	 * 不在流式模式
	 */
	if (apply_action == TRANS_LEADER_APPLY)
		return false;

	Assert(TransactionIdIsValid(stream_xid));

	/*
	 * The parallel apply worker needs the xid in this message to decide
	 * whether to define a savepoint, so save the original message that has
	 * not moved the cursor after the xid. We will serialize this message to a
	 * file in PARTIAL_SERIALIZE mode.
	 *
	 * 并行 apply worker 需要这条消息里的 xid 来决定是否定义保存点，因此保存尚未把游标移过
	 * xid 的原始消息。在 PARTIAL_SERIALIZE 模式下会把这条消息序列化到文件。
	 */
	original_msg = *s;

	/*
	 * We should have received XID of the subxact as the first part of the
	 * message, so extract it.
	 *
	 * 消息的第一部分应该是子事务的 XID，把它提取出来。
	 */
	current_xid = pq_getmsgint(s, 4);

	if (!TransactionIdIsValid(current_xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid transaction ID in streamed replication transaction")));

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:
			Assert(stream_fd);

			/* Add the new subxact to the array (unless already there).
			 *
			 * 把新的子事务加入数组（若尚未存在）。
			 */
			subxact_info_add(current_xid);

			/* Write the change to the current file
			 *
			 * 把变更写入当前文件
			 */
			stream_write_change(action, s);
			return true;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * XXX The publisher side doesn't always send relation/type update
			 * messages after the streaming transaction, so also update the
			 * relation/type in leader apply worker. See function
			 * cleanup_rel_sync_cache.
			 *
			 * XXX：发布端并不总是在流式事务之后发送关系或类型更新消息，
			 * 因此也在 leader apply worker 中更新关系或类型。见函数
			 * cleanup_rel_sync_cache。
			 */
			if (pa_send_data(winfo, s->len, s->data))
				return (action != LOGICAL_REP_MSG_RELATION &&
						action != LOGICAL_REP_MSG_TYPE);

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 *
			 * 无法把变更发给并行 apply worker 时，切换到序列化模式。
			 */
			pa_switch_to_partial_serialize(winfo, false);

			/* fall through
			 *
			 * 落入下一分支
			 */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			stream_write_change(action, &original_msg);

			/* Same reason as TRANS_LEADER_SEND_TO_PARALLEL case.
			 *
			 * 原因与 TRANS_LEADER_SEND_TO_PARALLEL 的情况相同。
			 */
			return (action != LOGICAL_REP_MSG_RELATION &&
					action != LOGICAL_REP_MSG_TYPE);

		case TRANS_PARALLEL_APPLY:
			parallel_stream_nchanges += 1;

			/* Define a savepoint for a subxact if needed.
			 *
			 * 如有需要，为子事务定义保存点。
			 */
			pa_start_subtrans(current_xid, stream_xid);
			return false;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			return false;		/* silence compiler warning
						 *
						 * 消除编译器警告
						 */
	}
}

/*
 * Executor state preparation for evaluation of constraint expressions,
 * indexes and triggers for the specified relation.
 *
 * 为指定关系准备执行器状态，以便计算约束表达式、索引和触发器。
 *
 * Note that the caller must open and close any indexes to be updated.
 *
 * 注意：调用方必须自行打开和关闭要更新的索引。
 */
static ApplyExecutionData *
create_edata_for_relation(LogicalRepRelMapEntry *rel)
{
	ApplyExecutionData *edata;
	EState	   *estate;
	RangeTblEntry *rte;
	List	   *perminfos = NIL;
	ResultRelInfo *resultRelInfo;

	edata = (ApplyExecutionData *) palloc0(sizeof(ApplyExecutionData));
	edata->targetRel = rel;

	edata->estate = estate = CreateExecutorState();

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(rel->localrel);
	rte->relkind = rel->localrel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;

	addRTEPermissionInfo(&perminfos, rte);

	ExecInitRangeTable(estate, list_make1(rte), perminfos,
					   bms_make_singleton(1));

	edata->targetRelInfo = resultRelInfo = makeNode(ResultRelInfo);

	/*
	 * Use Relation opened by logicalrep_rel_open() instead of opening it
	 * again.
	 *
	 * 使用 logicalrep_rel_open() 已经打开的 Relation，不要再次打开。
	 */
	InitResultRelInfo(resultRelInfo, rel->localrel, 1, NULL, 0);

	/*
	 * We put the ResultRelInfo in the es_opened_result_relations list, even
	 * though we don't populate the es_result_relations array.  That's a bit
	 * bogus, but it's enough to make ExecGetTriggerResultRel() find them.
	 *
	 * 把 ResultRelInfo 放入 es_opened_result_relations 列表，即使没有填充
	 * es_result_relations 数组。这样做有些取巧，但足以让 ExecGetTriggerResultRel()
	 * 找到它们。
	 *
	 * ExecOpenIndices() is not called here either, each execution path doing
	 * an apply operation being responsible for that.
	 *
	 * 这里也不调用 ExecOpenIndices()，每条执行 apply 操作的路径自己负责打开索引。
	 */
	estate->es_opened_result_relations =
		lappend(estate->es_opened_result_relations, resultRelInfo);

	estate->es_output_cid = GetCurrentCommandId(true);

	/* Prepare to catch AFTER triggers.
	 *
	 * 准备捕获 AFTER 触发器。
	 */
	AfterTriggerBeginQuery();

	/* other fields of edata remain NULL for now
	 *
	 * edata 的其他字段暂时保持 NULL
	 */

	return edata;
}

/*
 * Finish any operations related to the executor state created by
 * create_edata_for_relation().
 *
 * 完成与 create_edata_for_relation() 所创建执行器状态相关的操作。
 */
static void
finish_edata(ApplyExecutionData *edata)
{
	EState	   *estate = edata->estate;

	/* Handle any queued AFTER triggers.
	 *
	 * 处理已排队的 AFTER 触发器。
	 */
	AfterTriggerEndQuery(estate);

	/* Shut down tuple routing, if any was done.
	 *
	 * 若做过元组路由，则将其关闭。
	 */
	if (edata->proute)
		ExecCleanupTupleRouting(edata->mtstate, edata->proute);

	/*
	 * Cleanup.  It might seem that we should call ExecCloseResultRelations()
	 * here, but we intentionally don't.  It would close the rel we added to
	 * es_opened_result_relations above, which is wrong because we took no
	 * corresponding refcount.  We rely on ExecCleanupTupleRouting() to close
	 * any other relations opened during execution.
	 *
	 * 清理。看起来似乎应该在这里调用 ExecCloseResultRelations()，但我们有意不调用。
	 * 它会关闭上面加入 es_opened_result_relations 的关系，这是错的，因为我们没有取得对应的引用计数。
	 * 我们依赖 ExecCleanupTupleRouting() 关闭执行期间打开的其他关系。
	 */
	ExecResetTupleTable(estate->es_tupleTable, false);
	FreeExecutorState(estate);
	pfree(edata);
}

/*
 * Executes default values for columns for which we can't map to remote
 * relation columns.
 *
 * 为无法映射到远端关系列的列执行默认值。
 *
 * This allows us to support tables which have more columns on the downstream
 * than on the upstream.
 *
 * 这样就能支持下游比上游列更多的表。
 */
static void
slot_fill_defaults(LogicalRepRelMapEntry *rel, EState *estate,
				   TupleTableSlot *slot)
{
	TupleDesc	desc = RelationGetDescr(rel->localrel);
	int			num_phys_attrs = desc->natts;
	int			i;
	int			attnum,
				num_defaults = 0;
	int		   *defmap;
	ExprState **defexprs;
	ExprContext *econtext;

	econtext = GetPerTupleExprContext(estate);

	/* We got all the data via replication, no need to evaluate anything.
	 *
	 * 数据已全部通过复制得到，无需再计算任何表达式。
	 */
	if (num_phys_attrs == rel->remoterel.natts)
		return;

	defmap = (int *) palloc(num_phys_attrs * sizeof(int));
	defexprs = (ExprState **) palloc(num_phys_attrs * sizeof(ExprState *));

	Assert(rel->attrmap->maplen == num_phys_attrs);
	for (attnum = 0; attnum < num_phys_attrs; attnum++)
	{
		Expr	   *defexpr;

		if (TupleDescAttr(desc, attnum)->attisdropped || TupleDescAttr(desc, attnum)->attgenerated)
			continue;

		if (rel->attrmap->attnums[attnum] >= 0)
			continue;

		defexpr = (Expr *) build_column_default(rel->localrel, attnum + 1);

		if (defexpr != NULL)
		{
			/* Run the expression through planner
			 *
			 * 让表达式经过 planner
			 */
			defexpr = expression_planner(defexpr);

			/* Initialize executable expression in copycontext
			 *
			 * 在 copycontext 中初始化可执行表达式
			 */
			defexprs[num_defaults] = ExecInitExpr(defexpr, NULL);
			defmap[num_defaults] = attnum;
			num_defaults++;
		}
	}

	for (i = 0; i < num_defaults; i++)
		slot->tts_values[defmap[i]] =
			ExecEvalExpr(defexprs[i], econtext, &slot->tts_isnull[defmap[i]]);
}

/*
 * Store tuple data into slot.
 *
 * 把元组数据存入 slot。
 *
 * Incoming data can be either text or binary format.
 *
 * 传入数据可以是文本格式或二进制格式。
 */
static void
slot_store_data(TupleTableSlot *slot, LogicalRepRelMapEntry *rel,
				LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	ExecClearTuple(slot);

	/* Call the "in" function for each non-dropped, non-null attribute
	 *
	 * 对每个未删除且非空的属性调用 in 函数
	 */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			Assert(remoteattnum < tupleData->ncols);

			/* Set attnum for error callback
			 *
			 * 为错误回调设置 attnum
			 */
			apply_error_callback_arg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * In some code paths we may be asked to re-parse the same
				 * tuple data.  Reset the StringInfo's cursor so that works.
				 *
				 * 某些代码路径可能要求重新解析同一份元组数据。
				 * 重置 StringInfo 的游标以便这样做。
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* Trouble if it didn't eat the whole buffer
				 *
				 * 若没有吃完整个缓冲区则出错
				 */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/*
				 * NULL value from remote.  (We don't expect to see
				 * LOGICALREP_COLUMN_UNCHANGED here, but if we do, treat it as
				 * NULL.)
				 *
				 * 来自远端的 NULL 值。这里不期望看到 LOGICALREP_COLUMN_UNCHANGED，
				 * 但如果出现，就当作 NULL。
				 */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			/* Reset attnum for error callback
			 *
			 * 为错误回调重置 attnum
			 */
			apply_error_callback_arg.remote_attnum = -1;
		}
		else
		{
			/*
			 * We assign NULL to dropped attributes and missing values
			 * (missing values should be later filled using
			 * slot_fill_defaults).
			 *
			 * 对已删除属性和缺失值赋 NULL（缺失值稍后应由 slot_fill_defaults
			 * 填充）。
			 */
			slot->tts_values[i] = (Datum) 0;
			slot->tts_isnull[i] = true;
		}
	}

	ExecStoreVirtualTuple(slot);
}

/*
 * Replace updated columns with data from the LogicalRepTupleData struct.
 * This is somewhat similar to heap_modify_tuple but also calls the type
 * input functions on the user data.
 *
 * 用 LogicalRepTupleData 结构中的数据替换被更新的列。这有点类似 heap_modify_tuple，
 * 但还会对用户数据调用类型输入函数。
 *
 * "slot" is filled with a copy of the tuple in "srcslot", replacing
 * columns provided in "tupleData" and leaving others as-is.
 *
 * slot 中填入 srcslot 元组的副本，替换 tupleData 提供的列，其余列保持原样。
 *
 * Caution: unreplaced pass-by-ref columns in "slot" will point into the
 * storage for "srcslot".  This is OK for current usage, but someday we may
 * need to materialize "slot" at the end to make it independent of "srcslot".
 *
 * 注意：slot 中未被替换的传引用列会指向 srcslot 的存储。对当前用法这是可以的，
 * 但将来也许需要在结束时物化 slot，使它独立于 srcslot。
 */
static void
slot_modify_data(TupleTableSlot *slot, TupleTableSlot *srcslot,
				 LogicalRepRelMapEntry *rel,
				 LogicalRepTupleData *tupleData)
{
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	/* We'll fill "slot" with a virtual tuple, so we must start with ...
	 *
	 * 将用虚拟元组填充 slot，因此必须先清空
	 */
	ExecClearTuple(slot);

	/*
	 * Copy all the column data from srcslot, so that we'll have valid values
	 * for unreplaced columns.
	 *
	 * 从 srcslot 复制全部列数据，这样未被替换的列也有有效值。
	 */
	Assert(natts == srcslot->tts_tupleDescriptor->natts);
	slot_getallattrs(srcslot);
	memcpy(slot->tts_values, srcslot->tts_values, natts * sizeof(Datum));
	memcpy(slot->tts_isnull, srcslot->tts_isnull, natts * sizeof(bool));

	/* Call the "in" function for each replaced attribute
	 *
	 * 对每个被替换的属性调用 in 函数
	 */
	Assert(natts == rel->attrmap->maplen);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (remoteattnum < 0)
			continue;

		Assert(remoteattnum < tupleData->ncols);

		if (tupleData->colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
		{
			StringInfo	colvalue = &tupleData->colvalues[remoteattnum];

			/* Set attnum for error callback
			 *
			 * 为错误回调设置 attnum
			 */
			apply_error_callback_arg.remote_attnum = remoteattnum;

			if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_TEXT)
			{
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(att->atttypid, &typinput, &typioparam);
				slot->tts_values[i] =
					OidInputFunctionCall(typinput, colvalue->data,
										 typioparam, att->atttypmod);
				slot->tts_isnull[i] = false;
			}
			else if (tupleData->colstatus[remoteattnum] == LOGICALREP_COLUMN_BINARY)
			{
				Oid			typreceive;
				Oid			typioparam;

				/*
				 * In some code paths we may be asked to re-parse the same
				 * tuple data.  Reset the StringInfo's cursor so that works.
				 *
				 * 某些代码路径可能要求重新解析同一份元组数据。
				 * 重置 StringInfo 的游标以便这样做。
				 */
				colvalue->cursor = 0;

				getTypeBinaryInputInfo(att->atttypid, &typreceive, &typioparam);
				slot->tts_values[i] =
					OidReceiveFunctionCall(typreceive, colvalue,
										   typioparam, att->atttypmod);

				/* Trouble if it didn't eat the whole buffer
				 *
				 * 若没有吃完整个缓冲区则出错
				 */
				if (colvalue->cursor != colvalue->len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format in logical replication column %d",
									remoteattnum + 1)));
				slot->tts_isnull[i] = false;
			}
			else
			{
				/* must be LOGICALREP_COLUMN_NULL
				 *
				 * 必须是 LOGICALREP_COLUMN_NULL
				 */
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}

			/* Reset attnum for error callback
			 *
			 * 为错误回调重置 attnum
			 */
			apply_error_callback_arg.remote_attnum = -1;
		}
	}

	/* And finally, declare that "slot" contains a valid virtual tuple
	 *
	 * 最后，声明 slot 中含有一个有效的虚拟元组
	 */
	ExecStoreVirtualTuple(slot);
}

/*
 * Handle BEGIN message.
 *
 * 处理 BEGIN 消息。
 */
static void
apply_handle_begin(StringInfo s)
{
	LogicalRepBeginData begin_data;

	/* There must not be an active streaming transaction.
	 *
	 * 不得存在活动的流式事务。
	 */
	Assert(!TransactionIdIsValid(stream_xid));

	logicalrep_read_begin(s, &begin_data);
	set_apply_error_context_xact(begin_data.xid, begin_data.final_lsn);

	remote_final_lsn = begin_data.final_lsn;

	maybe_start_skipping_changes(begin_data.final_lsn);

	in_remote_transaction = true;

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Handle COMMIT message.
 *
 * 处理 COMMIT 消息。
 *
 * TODO, support tracking of multiple origins
 *
 * TODO，支持跟踪多个 origin
 */
static void
apply_handle_commit(StringInfo s)
{
	LogicalRepCommitData commit_data;

	logicalrep_read_commit(s, &commit_data);

	if (commit_data.commit_lsn != remote_final_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("incorrect commit LSN %X/%X in commit message (expected %X/%X)",
								 LSN_FORMAT_ARGS(commit_data.commit_lsn),
								 LSN_FORMAT_ARGS(remote_final_lsn))));

	apply_handle_commit_internal(&commit_data);

	/* Process any tables that are being synchronized in parallel.
	 *
	 * 处理正在并行同步的表。
	 */
	process_syncing_tables(commit_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * Handle BEGIN PREPARE message.
 *
 * 处理 BEGIN PREPARE 消息。
 */
static void
apply_handle_begin_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData begin_data;

	/* Tablesync should never receive prepare.
	 *
	 * tablesync 绝不应收到 prepare。
	 */
	if (am_tablesync_worker())
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("tablesync worker received a BEGIN PREPARE message")));

	/* There must not be an active streaming transaction.
	 *
	 * 不得存在活动的流式事务。
	 */
	Assert(!TransactionIdIsValid(stream_xid));

	logicalrep_read_begin_prepare(s, &begin_data);
	set_apply_error_context_xact(begin_data.xid, begin_data.prepare_lsn);

	remote_final_lsn = begin_data.prepare_lsn;

	maybe_start_skipping_changes(begin_data.prepare_lsn);

	in_remote_transaction = true;

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Common function to prepare the GID.
 *
 * 准备 GID 的公共函数。
 */
static void
apply_handle_prepare_internal(LogicalRepPreparedTxnData *prepare_data)
{
	char		gid[GIDSIZE];

	/*
	 * Compute unique GID for two_phase transactions. We don't use GID of
	 * prepared transaction sent by server as that can lead to deadlock when
	 * we have multiple subscriptions from same node point to publications on
	 * the same node. See comments atop worker.c
	 *
	 * 为 two_phase 事务计算唯一 GID。我们不使用服务器发来的已准备事务 GID，
	 * 因为当多个订阅从同一节点指向同一节点上的发布时，那会导致死锁。见 worker.c
	 * 顶部的注释。
	 */
	TwoPhaseTransactionGid(MySubscription->oid, prepare_data->xid,
						   gid, sizeof(gid));

	/*
	 * BeginTransactionBlock is necessary to balance the EndTransactionBlock
	 * called within the PrepareTransactionBlock below.
	 *
	 * 需要 BeginTransactionBlock，以便与下面 PrepareTransactionBlock 中调用的
	 * EndTransactionBlock 配对。
	 */
	if (!IsTransactionBlock())
	{
		BeginTransactionBlock();
		CommitTransactionCommand(); /* Completes the preceding Begin command.
					     *
					     * 完成前面的 Begin 命令。
					     */
	}

	/*
	 * Update origin state so we can restart streaming from correct position
	 * in case of crash.
	 *
	 * 更新 origin 状态，以便崩溃后能从正确位置重新开始流式传输。
	 */
	replorigin_session_origin_lsn = prepare_data->end_lsn;
	replorigin_session_origin_timestamp = prepare_data->prepare_time;

	PrepareTransactionBlock(gid);
}

/*
 * Handle PREPARE message.
 *
 * 处理 PREPARE 消息。
 */
static void
apply_handle_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData prepare_data;

	logicalrep_read_prepare(s, &prepare_data);

	if (prepare_data.prepare_lsn != remote_final_lsn)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("incorrect prepare LSN %X/%X in prepare message (expected %X/%X)",
								 LSN_FORMAT_ARGS(prepare_data.prepare_lsn),
								 LSN_FORMAT_ARGS(remote_final_lsn))));

	/*
	 * Unlike commit, here, we always prepare the transaction even though no
	 * change has happened in this transaction or all changes are skipped. It
	 * is done this way because at commit prepared time, we won't know whether
	 * we have skipped preparing a transaction because of those reasons.
	 *
	 * 与 commit 不同，这里即使本事务没有发生变更或所有变更都被跳过，也始终
	 * prepare 该事务。这样做是因为到 commit prepared 时，我们无法知道是否因这些原因跳过了
	 * prepare。
	 *
	 * XXX, We can optimize such that at commit prepared time, we first check
	 * whether we have prepared the transaction or not but that doesn't seem
	 * worthwhile because such cases shouldn't be common.
	 *
	 * XXX，可以优化为在 commit prepared 时先检查是否已经 prepare 过该事务，
	 * 但这类情况不常见，似乎不值得。
	 */
	begin_replication_step();

	apply_handle_prepare_internal(&prepare_data);

	end_replication_step();
	CommitTransactionCommand();
	pgstat_report_stat(false);

	/*
	 * It is okay not to set the local_end LSN for the prepare because we
	 * always flush the prepare record. So, we can send the acknowledgment of
	 * the remote_end LSN as soon as prepare is finished.
	 *
	 * 可以不为 prepare 设置 local_end LSN，因为我们总会刷出 prepare 记录。
	 * 因此 prepare 一完成就可以发送 remote_end LSN 的确认。
	 *
	 * XXX For the sake of consistency with commit, we could have set it with
	 * the LSN of prepare but as of now we don't track that value similar to
	 * XactLastCommitEnd, and adding it for this purpose doesn't seems worth
	 * it.
	 *
	 * XXX，为了与 commit 保持一致，本可以用 prepare 的 LSN 来设置它，但目前我们并不像
	 * XactLastCommitEnd 那样跟踪该值，为此增加跟踪似乎不值得。
	 */
	store_flush_position(prepare_data.end_lsn, InvalidXLogRecPtr);

	in_remote_transaction = false;

	/* Process any tables that are being synchronized in parallel.
	 *
	 * 处理正在并行同步的表。
	 */
	process_syncing_tables(prepare_data.end_lsn);

	/*
	 * Since we have already prepared the transaction, in a case where the
	 * server crashes before clearing the subskiplsn, it will be left but the
	 * transaction won't be resent. But that's okay because it's a rare case
	 * and the subskiplsn will be cleared when finishing the next transaction.
	 *
	 * 由于事务已经 prepare，如果服务器在清除 subskiplsn 之前崩溃，subskiplsn
	 * 会留下，但该事务不会被重发。这可以接受，因为这种情况罕见，而且完成下一个事务时会清除
	 * subskiplsn。
	 */
	stop_skipping_changes();
	clear_subscription_skip_lsn(prepare_data.prepare_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * Handle a COMMIT PREPARED of a previously PREPARED transaction.
 *
 * 处理先前 PREPARE 过的事务的 COMMIT PREPARED。
 *
 * Note that we don't need to wait here if the transaction was prepared in a
 * parallel apply worker. In that case, we have already waited for the prepare
 * to finish in apply_handle_stream_prepare() which will ensure all the
 * operations in that transaction have happened in the subscriber, so no
 * concurrent transaction can cause deadlock or transaction dependency issues.
 *
 * 若该事务是在并行 apply worker 中 prepare 的，这里不需要等待。那种情况下，我们已经在
 * apply_handle_stream_prepare() 中等待 prepare 完成，从而保证该事务的全部操作已在订阅端发生，
 * 因此不会有并发事务造成死锁或事务依赖问题。
 */
static void
apply_handle_commit_prepared(StringInfo s)
{
	LogicalRepCommitPreparedTxnData prepare_data;
	char		gid[GIDSIZE];

	logicalrep_read_commit_prepared(s, &prepare_data);
	set_apply_error_context_xact(prepare_data.xid, prepare_data.commit_lsn);

	/* Compute GID for two_phase transactions.
	 *
	 * 为 two_phase 事务计算 GID。
	 */
	TwoPhaseTransactionGid(MySubscription->oid, prepare_data.xid,
						   gid, sizeof(gid));

	/* There is no transaction when COMMIT PREPARED is called
	 *
	 * 调用 COMMIT PREPARED 时并不存在事务
	 */
	begin_replication_step();

	/*
	 * Update origin state so we can restart streaming from correct position
	 * in case of crash.
	 *
	 * 更新 origin 状态，以便崩溃后能从正确位置重新开始流式传输。
	 */
	replorigin_session_origin_lsn = prepare_data.end_lsn;
	replorigin_session_origin_timestamp = prepare_data.commit_time;

	FinishPreparedTransaction(gid, true);
	end_replication_step();
	CommitTransactionCommand();
	pgstat_report_stat(false);

	store_flush_position(prepare_data.end_lsn, XactLastCommitEnd);
	in_remote_transaction = false;

	/* Process any tables that are being synchronized in parallel.
	 *
	 * 处理正在并行同步的表。
	 */
	process_syncing_tables(prepare_data.end_lsn);

	clear_subscription_skip_lsn(prepare_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * Handle a ROLLBACK PREPARED of a previously PREPARED TRANSACTION.
 *
 * 处理先前 PREPARED TRANSACTION 的 ROLLBACK PREPARED。
 *
 * Note that we don't need to wait here if the transaction was prepared in a
 * parallel apply worker. In that case, we have already waited for the prepare
 * to finish in apply_handle_stream_prepare() which will ensure all the
 * operations in that transaction have happened in the subscriber, so no
 * concurrent transaction can cause deadlock or transaction dependency issues.
 *
 * 若该事务是在并行 apply worker 中 prepare 的，这里不需要等待。那种情况下，我们已经在
 * apply_handle_stream_prepare() 中等待 prepare 完成，从而保证该事务的全部操作已在订阅端发生，
 * 因此不会有并发事务造成死锁或事务依赖问题。
 */
static void
apply_handle_rollback_prepared(StringInfo s)
{
	LogicalRepRollbackPreparedTxnData rollback_data;
	char		gid[GIDSIZE];

	logicalrep_read_rollback_prepared(s, &rollback_data);
	set_apply_error_context_xact(rollback_data.xid, rollback_data.rollback_end_lsn);

	/* Compute GID for two_phase transactions.
	 *
	 * 为 two_phase 事务计算 GID。
	 */
	TwoPhaseTransactionGid(MySubscription->oid, rollback_data.xid,
						   gid, sizeof(gid));

	/*
	 * It is possible that we haven't received prepare because it occurred
	 * before walsender reached a consistent point or the two_phase was still
	 * not enabled by that time, so in such cases, we need to skip rollback
	 * prepared.
	 *
	 * 有可能我们没有收到 prepare，因为它发生在 walsender 到达一致点之前，
	 * 或者当时 two_phase 尚未启用。这种情况下需要跳过 rollback prepared。
	 */
	if (LookupGXact(gid, rollback_data.prepare_end_lsn,
					rollback_data.prepare_time))
	{
		/*
		 * Update origin state so we can restart streaming from correct
		 * position in case of crash.
		 *
		 * 更新 origin 状态，以便崩溃后能从正确位置重新开始流式传输。
		 */
		replorigin_session_origin_lsn = rollback_data.rollback_end_lsn;
		replorigin_session_origin_timestamp = rollback_data.rollback_time;

		/* There is no transaction when ABORT/ROLLBACK PREPARED is called
		 *
		 * 调用 ABORT/ROLLBACK PREPARED 时并不存在事务
		 */
		begin_replication_step();
		FinishPreparedTransaction(gid, false);
		end_replication_step();
		CommitTransactionCommand();

		clear_subscription_skip_lsn(rollback_data.rollback_end_lsn);
	}

	pgstat_report_stat(false);

	/*
	 * It is okay not to set the local_end LSN for the rollback of prepared
	 * transaction because we always flush the WAL record for it. See
	 * apply_handle_prepare.
	 *
	 * 可以不为已准备事务的回滚设置 local_end LSN，因为我们总会刷出对应的
	 * WAL 记录。见 apply_handle_prepare。
	 */
	store_flush_position(rollback_data.rollback_end_lsn, InvalidXLogRecPtr);
	in_remote_transaction = false;

	/* Process any tables that are being synchronized in parallel.
	 *
	 * 处理正在并行同步的表。
	 */
	process_syncing_tables(rollback_data.rollback_end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);
	reset_apply_error_context_info();
}

/*
 * Handle STREAM PREPARE.
 *
 * 处理 STREAM PREPARE。
 */
static void
apply_handle_stream_prepare(StringInfo s)
{
	LogicalRepPreparedTxnData prepare_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed.
	 *
	 * 在消息被消费之前先保存它。
	 */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM PREPARE message without STREAM STOP")));

	/* Tablesync should never receive prepare.
	 *
	 * tablesync 绝不应收到 prepare。
	 */
	if (am_tablesync_worker())
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("tablesync worker received a STREAM PREPARE message")));

	logicalrep_read_stream_prepare(s, &prepare_data);
	set_apply_error_context_xact(prepare_data.xid, prepare_data.prepare_lsn);

	apply_action = get_transaction_apply_action(prepare_data.xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * The transaction has been serialized to file, so replay all the
			 * spooled operations.
			 *
			 * 事务已序列化到文件，因此重放全部暂存的操作。
			 */
			apply_spooled_messages(MyLogicalRepWorker->stream_fileset,
								   prepare_data.xid, prepare_data.prepare_lsn);

			/* Mark the transaction as prepared.
			 *
			 * 把事务标记为已准备。
			 */
			apply_handle_prepare_internal(&prepare_data);

			CommitTransactionCommand();

			/*
			 * It is okay not to set the local_end LSN for the prepare because
			 * we always flush the prepare record. See apply_handle_prepare.
			 *
			 * 可以不为 prepare 设置 local_end LSN，因为我们总会刷出
			 * prepare 记录。见 apply_handle_prepare。
			 */
			store_flush_position(prepare_data.end_lsn, InvalidXLogRecPtr);

			in_remote_transaction = false;

			/* Unlink the files with serialized changes and subxact info.
			 *
			 * 删除保存序列化变更和子事务信息的文件。
			 */
			stream_cleanup_files(MyLogicalRepWorker->subid, prepare_data.xid);

			elog(DEBUG1, "finished processing the STREAM PREPARE command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			if (pa_send_data(winfo, s->len, s->data))
			{
				/* Finish processing the streaming transaction.
				 *
				 * 结束对流式事务的处理。
				 */
				pa_xact_finish(winfo, prepare_data.end_lsn);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 *
			 * 无法把变更发给并行 apply worker 时，切换到序列化模式。
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through
			 *
			 * 落入下一分支
			 */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			stream_open_and_write_change(prepare_data.xid,
										 LOGICAL_REP_MSG_STREAM_PREPARE,
										 &original_msg);

			pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);

			/* Finish processing the streaming transaction.
			 *
			 * 结束对流式事务的处理。
			 */
			pa_xact_finish(winfo, prepare_data.end_lsn);
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * If the parallel apply worker is applying spooled messages then
			 * close the file before preparing.
			 *
			 * 若并行 apply worker 正在应用暂存消息，则在 prepare
			 * 之前关闭文件。
			 */
			if (stream_fd)
				stream_close_file();

			begin_replication_step();

			/* Mark the transaction as prepared.
			 *
			 * 把事务标记为已准备。
			 */
			apply_handle_prepare_internal(&prepare_data);

			end_replication_step();

			CommitTransactionCommand();

			/*
			 * It is okay not to set the local_end LSN for the prepare because
			 * we always flush the prepare record. See apply_handle_prepare.
			 *
			 * 可以不为 prepare 设置 local_end LSN，因为我们总会刷出
			 * prepare 记录。见 apply_handle_prepare。
			 */
			MyParallelShared->last_commit_end = InvalidXLogRecPtr;

			pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);
			pa_unlock_transaction(MyParallelShared->xid, AccessExclusiveLock);

			pa_reset_subtrans();

			elog(DEBUG1, "finished processing the STREAM PREPARE command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	pgstat_report_stat(false);

	/* Process any tables that are being synchronized in parallel.
	 *
	 * 处理正在并行同步的表。
	 */
	process_syncing_tables(prepare_data.end_lsn);

	/*
	 * Similar to prepare case, the subskiplsn could be left in a case of
	 * server crash but it's okay. See the comments in apply_handle_prepare().
	 *
	 * 与 prepare 的情况类似，服务器崩溃时 subskiplsn 可能留下，但这可以接受。
	 * 见 apply_handle_prepare() 中的注释。
	 */
	stop_skipping_changes();
	clear_subscription_skip_lsn(prepare_data.prepare_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * Handle ORIGIN message.
 *
 * 处理 ORIGIN 消息。
 *
 * TODO, support tracking of multiple origins
 *
 * TODO，支持跟踪多个 origin
 */
static void
apply_handle_origin(StringInfo s)
{
	/*
	 * ORIGIN message can only come inside streaming transaction or inside
	 * remote transaction and before any actual writes.
	 *
	 * ORIGIN 消息只能出现在流式事务内部，或出现在远端事务内部且在任何实际写入之前。
	 */
	if (!in_streamed_transaction &&
		(!in_remote_transaction ||
		 (IsTransactionState() && !am_tablesync_worker())))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("ORIGIN message sent out of order")));
}

/*
 * Initialize fileset (if not already done).
 *
 * 初始化 fileset（若尚未初始化）。
 *
 * Create a new file when first_segment is true, otherwise open the existing
 * file.
 *
 * first_segment 为真时创建新文件，否则打开已有文件。
 */
void
stream_start_internal(TransactionId xid, bool first_segment)
{
	begin_replication_step();

	/*
	 * Initialize the worker's stream_fileset if we haven't yet. This will be
	 * used for the entire duration of the worker so create it in a permanent
	 * context. We create this on the very first streaming message from any
	 * transaction and then use it for this and other streaming transactions.
	 * Now, we could create a fileset at the start of the worker as well but
	 * then we won't be sure that it will ever be used.
	 *
	 * 若尚未初始化，则初始化 worker 的 stream_fileset。它会在 worker 的整个生命周期使用，
	 * 因此在永久上下文中创建。我们在任意事务的第一条流式消息到来时创建它，
	 * 然后用于该事务以及其他流式事务。也可以在 worker 启动时就创建 fileset，
	 * 但那样无法确定它是否会被用到。
	 */
	if (!MyLogicalRepWorker->stream_fileset)
	{
		MemoryContext oldctx;

		oldctx = MemoryContextSwitchTo(ApplyContext);

		MyLogicalRepWorker->stream_fileset = palloc(sizeof(FileSet));
		FileSetInit(MyLogicalRepWorker->stream_fileset);

		MemoryContextSwitchTo(oldctx);
	}

	/* Open the spool file for this transaction.
	 *
	 * 打开此事务的 spool 文件。
	 */
	stream_open_file(MyLogicalRepWorker->subid, xid, first_segment);

	/* If this is not the first segment, open existing subxact file.
	 *
	 * 若这不是第一段，则打开已有的子事务文件。
	 */
	if (!first_segment)
		subxact_info_read(MyLogicalRepWorker->subid, xid);

	end_replication_step();
}

/*
 * Handle STREAM START message.
 *
 * 处理 STREAM START 消息。
 */
static void
apply_handle_stream_start(StringInfo s)
{
	bool		first_segment;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed.
	 *
	 * 在消息被消费之前先保存它。
	 */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("duplicate STREAM START message")));

	/* There must not be an active streaming transaction.
	 *
	 * 不得存在活动的流式事务。
	 */
	Assert(!TransactionIdIsValid(stream_xid));

	/* notify handle methods we're processing a remote transaction
	 *
	 * 通知处理函数：我们正在处理一个远端事务
	 */
	in_streamed_transaction = true;

	/* extract XID of the top-level transaction
	 *
	 * 提取顶层事务的 XID
	 */
	stream_xid = logicalrep_read_stream_start(s, &first_segment);

	if (!TransactionIdIsValid(stream_xid))
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("invalid transaction ID in streamed replication transaction")));

	set_apply_error_context_xact(stream_xid, InvalidXLogRecPtr);

	/* Try to allocate a worker for the streaming transaction.
	 *
	 * 尝试为该流式事务分配一个 worker。
	 */
	if (first_segment)
		pa_allocate_worker(stream_xid);

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:

			/*
			 * Function stream_start_internal starts a transaction. This
			 * transaction will be committed on the stream stop unless it is a
			 * tablesync worker in which case it will be committed after
			 * processing all the messages. We need this transaction for
			 * handling the BufFile, used for serializing the streaming data
			 * and subxact info.
			 *
			 * stream_start_internal 会启动一个事务。该事务在 stream
			 * stop 时提交，除非是 tablesync worker，那种情况下会在处理完所有消息后提交。
			 * 需要这个事务来处理用于序列化流式数据和子事务信息的
			 * BufFile。
			 */
			stream_start_internal(stream_xid, first_segment);
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * Once we start serializing the changes, the parallel apply
			 * worker will wait for the leader to release the stream lock
			 * until the end of the transaction. So, we don't need to release
			 * the lock or increment the stream count in that case.
			 *
			 * 一旦开始序列化变更，并行 apply worker 会一直等待 leader
			 * 释放流锁，直到事务结束。因此这种情况下不需要释放锁，
			 * 也不需要增加流计数。
			 */
			if (pa_send_data(winfo, s->len, s->data))
			{
				/*
				 * Unlock the shared object lock so that the parallel apply
				 * worker can continue to receive changes.
				 *
				 * 解锁共享对象锁，使并行 apply worker 可以继续接收变更。
				 */
				if (!first_segment)
					pa_unlock_stream(winfo->shared->xid, AccessExclusiveLock);

				/*
				 * Increment the number of streaming blocks waiting to be
				 * processed by parallel apply worker.
				 *
				 * 增加等待并行 apply worker 处理的流式块数量。
				 */
				pg_atomic_add_fetch_u32(&winfo->shared->pending_stream_count, 1);

				/* Cache the parallel apply worker for this transaction.
				 *
				 * 缓存此事务的并行 apply worker。
				 */
				pa_set_stream_apply_worker(winfo);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 *
			 * 无法把变更发给并行 apply worker 时，切换到序列化模式。
			 */
			pa_switch_to_partial_serialize(winfo, !first_segment);

			/* fall through
			 *
			 * 落入下一分支
			 */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			/*
			 * Open the spool file unless it was already opened when switching
			 * to serialize mode. The transaction started in
			 * stream_start_internal will be committed on the stream stop.
			 *
			 * 除非切换到序列化模式时已经打开，否则打开 spool 文件。
			 * stream_start_internal 中启动的事务会在 stream stop
			 * 时提交。
			 */
			if (apply_action != TRANS_LEADER_SEND_TO_PARALLEL)
				stream_start_internal(stream_xid, first_segment);

			stream_write_change(LOGICAL_REP_MSG_STREAM_START, &original_msg);

			/* Cache the parallel apply worker for this transaction.
			 *
			 * 缓存此事务的并行 apply worker。
			 */
			pa_set_stream_apply_worker(winfo);
			break;

		case TRANS_PARALLEL_APPLY:
			if (first_segment)
			{
				/* Hold the lock until the end of the transaction.
				 *
				 * 持有锁直到事务结束。
				 */
				pa_lock_transaction(MyParallelShared->xid, AccessExclusiveLock);
				pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_STARTED);

				/*
				 * Signal the leader apply worker, as it may be waiting for
				 * us.
				 *
				 * 向 leader apply worker 发信号，因为它可能正在等我们。
				 */
				logicalrep_worker_wakeup(MyLogicalRepWorker->subid, InvalidOid);
			}

			parallel_stream_nchanges = 0;
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	pgstat_report_activity(STATE_RUNNING, NULL);
}

/*
 * Update the information about subxacts and close the file.
 *
 * 更新子事务信息并关闭文件。
 *
 * This function should be called when the stream_start_internal function has
 * been called.
 *
 * 本函数应在已调用 stream_start_internal 之后调用。
 */
void
stream_stop_internal(TransactionId xid)
{
	/*
	 * Serialize information about subxacts for the toplevel transaction, then
	 * close the stream messages spool file.
	 *
	 * 序列化顶层事务的子事务信息，然后关闭流消息的 spool 文件。
	 */
	subxact_info_write(MyLogicalRepWorker->subid, xid);
	stream_close_file();

	/* We must be in a valid transaction state
	 *
	 * 必须处于有效的事务状态
	 */
	Assert(IsTransactionState());

	/* Commit the per-stream transaction
	 *
	 * 提交每个流对应的事务
	 */
	CommitTransactionCommand();

	/* Reset per-stream context
	 *
	 * 重置每个流的上下文
	 */
	MemoryContextReset(LogicalStreamingContext);
}

/*
 * Handle STREAM STOP message.
 *
 * 处理 STREAM STOP 消息。
 */
static void
apply_handle_stream_stop(StringInfo s)
{
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	if (!in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM STOP message without STREAM START")));

	apply_action = get_transaction_apply_action(stream_xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_SERIALIZE:
			stream_stop_internal(stream_xid);
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * Lock before sending the STREAM_STOP message so that the leader
			 * can hold the lock first and the parallel apply worker will wait
			 * for leader to release the lock. See Locking Considerations atop
			 * applyparallelworker.c.
			 *
			 * 在发送 STREAM_STOP 消息之前加锁，使 leader 能先持有锁，
			 * 并行 apply worker 则等待 leader 释放锁。见 applyparallelworker.c
			 * 顶部的 Locking Considerations。
			 */
			pa_lock_stream(winfo->shared->xid, AccessExclusiveLock);

			if (pa_send_data(winfo, s->len, s->data))
			{
				pa_set_stream_apply_worker(NULL);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 *
			 * 无法把变更发给并行 apply worker 时，切换到序列化模式。
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through
			 *
			 * 落入下一分支
			 */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			stream_write_change(LOGICAL_REP_MSG_STREAM_STOP, s);
			stream_stop_internal(stream_xid);
			pa_set_stream_apply_worker(NULL);
			break;

		case TRANS_PARALLEL_APPLY:
			elog(DEBUG1, "applied %u changes in the streaming chunk",
				 parallel_stream_nchanges);

			/*
			 * By the time parallel apply worker is processing the changes in
			 * the current streaming block, the leader apply worker may have
			 * sent multiple streaming blocks. This can lead to parallel apply
			 * worker start waiting even when there are more chunk of streams
			 * in the queue. So, try to lock only if there is no message left
			 * in the queue. See Locking Considerations atop
			 * applyparallelworker.c.
			 *
			 * 当并行 apply worker 正在处理当前流式块中的变更时，leader
			 * apply worker 可能已经发送了多个流式块。这会导致并行
			 * apply worker 在队列里还有更多流数据块时就开始等待。
			 * 因此仅当队列中没有剩余消息时才尝试加锁。见 applyparallelworker.c
			 * 顶部的 Locking Considerations。
			 *
			 * Note that here we have a race condition where we can start
			 * waiting even when there are pending streaming chunks. This can
			 * happen if the leader sends another streaming block and acquires
			 * the stream lock again after the parallel apply worker checks
			 * that there is no pending streaming block and before it actually
			 * starts waiting on a lock. We can handle this case by not
			 * allowing the leader to increment the stream block count during
			 * the time parallel apply worker acquires the lock but it is not
			 * clear whether that is worth the complexity.
			 *
			 * 注意这里有竞态：即使还有待处理的流式块，我们也可能开始等待。
			 * 如果 leader 在并行 apply worker 检查到没有待处理流式块之后、
			 * 真正开始等锁之前，又发送了另一个流式块并再次获得流锁，
			 * 就会发生这种情况。可以在并行 apply worker 获取锁期间禁止
			 * leader 增加流块计数来处理，但尚不清楚是否值得为此增加复杂度。
			 *
			 * Now, if this missed chunk contains rollback to savepoint, then
			 * there is a risk of deadlock which probably shouldn't happen
			 * after restart.
			 *
			 * 如果这个被漏掉的块包含 rollback to savepoint，则存在死锁风险，
			 * 重启之后大概不应再发生。
			 */
			pa_decr_and_wait_stream_block();
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	in_streamed_transaction = false;
	stream_xid = InvalidTransactionId;

	/*
	 * The parallel apply worker could be in a transaction in which case we
	 * need to report the state as STATE_IDLEINTRANSACTION.
	 *
	 * 并行 apply worker 可能处于事务中，此时需要把状态报告为 STATE_IDLEINTRANSACTION。
	 */
	if (IsTransactionOrTransactionBlock())
		pgstat_report_activity(STATE_IDLEINTRANSACTION, NULL);
	else
		pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * Helper function to handle STREAM ABORT message when the transaction was
 * serialized to file.
 *
 * 当事务已序列化到文件时，处理 STREAM ABORT 消息的辅助函数。
 */
static void
stream_abort_internal(TransactionId xid, TransactionId subxid)
{
	/*
	 * If the two XIDs are the same, it's in fact abort of toplevel xact, so
	 * just delete the files with serialized info.
	 *
	 * 若两个 XID 相同，实际上是顶层事务的中止，直接删除带有序列化信息的文件。
	 */
	if (xid == subxid)
		stream_cleanup_files(MyLogicalRepWorker->subid, xid);
	else
	{
		/*
		 * OK, so it's a subxact. We need to read the subxact file for the
		 * toplevel transaction, determine the offset tracked for the subxact,
		 * and truncate the file with changes. We also remove the subxacts
		 * with higher offsets (or rather higher XIDs).
		 *
		 * 这是子事务。需要读取顶层事务的子事务文件，确定为该子事务记录的偏移，
		 * 并截断变更文件。同时移除偏移更高（或者说 XID 更高）的子事务。
		 *
		 * We intentionally scan the array from the tail, because we're likely
		 * aborting a change for the most recent subtransactions.
		 *
		 * 我们有意从数组尾部扫描，因为中止的多半是最近子事务的变更。
		 *
		 * We can't use the binary search here as subxact XIDs won't
		 * necessarily arrive in sorted order, consider the case where we have
		 * released the savepoint for multiple subtransactions and then
		 * performed rollback to savepoint for one of the earlier
		 * sub-transaction.
		 *
		 * 这里不能用二分查找，因为子事务 XID 不一定按序到达。考虑这样一种情况：
		 * 我们释放了多个子事务的保存点，然后对其中一个更早的子事务执行了
		 * rollback to savepoint。
		 */
		int64		i;
		int64		subidx;
		BufFile    *fd;
		bool		found = false;
		char		path[MAXPGPATH];

		subidx = -1;
		begin_replication_step();
		subxact_info_read(MyLogicalRepWorker->subid, xid);

		for (i = subxact_data.nsubxacts; i > 0; i--)
		{
			if (subxact_data.subxacts[i - 1].xid == subxid)
			{
				subidx = (i - 1);
				found = true;
				break;
			}
		}

		/*
		 * If it's an empty sub-transaction then we will not find the subxid
		 * here so just cleanup the subxact info and return.
		 *
		 * 若是空的子事务，这里找不到该 subxid，只需清理子事务信息并返回。
		 */
		if (!found)
		{
			/* Cleanup the subxact info
			 *
			 * 清理子事务信息
			 */
			cleanup_subxact_info();
			end_replication_step();
			CommitTransactionCommand();
			return;
		}

		/* open the changes file
		 *
		 * 打开变更文件
		 */
		changes_filename(path, MyLogicalRepWorker->subid, xid);
		fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path,
								O_RDWR, false);

		/* OK, truncate the file at the right offset
		 *
		 * 在正确的偏移处截断文件
		 */
		BufFileTruncateFileSet(fd, subxact_data.subxacts[subidx].fileno,
							   subxact_data.subxacts[subidx].offset);
		BufFileClose(fd);

		/* discard the subxacts added later
		 *
		 * 丢弃后来加入的子事务
		 */
		subxact_data.nsubxacts = subidx;

		/* write the updated subxact list
		 *
		 * 写回更新后的子事务列表
		 */
		subxact_info_write(MyLogicalRepWorker->subid, xid);

		end_replication_step();
		CommitTransactionCommand();
	}
}

/*
 * Handle STREAM ABORT message.
 *
 * 处理 STREAM ABORT 消息。
 */
static void
apply_handle_stream_abort(StringInfo s)
{
	TransactionId xid;
	TransactionId subxid;
	LogicalRepStreamAbortData abort_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed.
	 *
	 * 在消息被消费之前先保存它。
	 */
	StringInfoData original_msg = *s;
	bool		toplevel_xact;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM ABORT message without STREAM STOP")));

	/* We receive abort information only when we can apply in parallel.
	 *
	 * 只有能够并行应用时，我们才会收到中止信息。
	 */
	logicalrep_read_stream_abort(s, &abort_data,
								 MyLogicalRepWorker->parallel_apply);

	xid = abort_data.xid;
	subxid = abort_data.subxid;
	toplevel_xact = (xid == subxid);

	set_apply_error_context_xact(subxid, abort_data.abort_lsn);

	apply_action = get_transaction_apply_action(xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * We are in the leader apply worker and the transaction has been
			 * serialized to file.
			 *
			 * 当前处于 leader apply worker，且事务已序列化到文件。
			 */
			stream_abort_internal(xid, subxid);

			elog(DEBUG1, "finished processing the STREAM ABORT command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			/*
			 * For the case of aborting the subtransaction, we increment the
			 * number of streaming blocks and take the lock again before
			 * sending the STREAM_ABORT to ensure that the parallel apply
			 * worker will wait on the lock for the next set of changes after
			 * processing the STREAM_ABORT message if it is not already
			 * waiting for STREAM_STOP message.
			 *
			 * 对于中止子事务的情况，我们在发送 STREAM_ABORT 之前再次增加流式块计数并加锁，
			 * 以确保并行 apply worker 在处理完 STREAM_ABORT 消息后，
			 * 若尚未在等待 STREAM_STOP 消息，会在锁上等待下一批变更。
			 *
			 * It is important to perform this locking before sending the
			 * STREAM_ABORT message so that the leader can hold the lock first
			 * and the parallel apply worker will wait for the leader to
			 * release the lock. This is the same as what we do in
			 * apply_handle_stream_stop. See Locking Considerations atop
			 * applyparallelworker.c.
			 *
			 * 必须在发送 STREAM_ABORT 消息之前完成加锁，这样 leader
			 * 能先持有锁，并行 apply worker 等待 leader 释放锁。这与
			 * apply_handle_stream_stop 中的做法相同。见 applyparallelworker.c
			 * 顶部的 Locking Considerations。
			 */
			if (!toplevel_xact)
			{
				pa_unlock_stream(xid, AccessExclusiveLock);
				pg_atomic_add_fetch_u32(&winfo->shared->pending_stream_count, 1);
				pa_lock_stream(xid, AccessExclusiveLock);
			}

			if (pa_send_data(winfo, s->len, s->data))
			{
				/*
				 * Unlike STREAM_COMMIT and STREAM_PREPARE, we don't need to
				 * wait here for the parallel apply worker to finish as that
				 * is not required to maintain the commit order and won't have
				 * the risk of failures due to transaction dependencies and
				 * deadlocks. However, it is possible that before the parallel
				 * worker finishes and we clear the worker info, the xid
				 * wraparound happens on the upstream and a new transaction
				 * with the same xid can appear and that can lead to duplicate
				 * entries in ParallelApplyTxnHash. Yet another problem could
				 * be that we may have serialized the changes in partial
				 * serialize mode and the file containing xact changes may
				 * already exist, and after xid wraparound trying to create
				 * the file for the same xid can lead to an error. To avoid
				 * these problems, we decide to wait for the aborts to finish.
				 *
				 * 与 STREAM_COMMIT 和 STREAM_PREPARE 不同，这里不需要等待并行
				 * apply worker 结束，因为这不是维持提交顺序所必需的，
				 * 也不会有事务依赖和死锁导致失败的风险。但是，
				 * 在并行 worker 结束、我们清除 worker 信息之前，
				 * 上游可能发生 xid 回绕，出现 xid 相同的新事务，
				 * 从而导致 ParallelApplyTxnHash 中出现重复项。
				 * 另一个问题是，我们可能已在部分序列化模式下把变更序列化，
				 * 包含事务变更的文件可能已经存在，xid 回绕后为同一
				 * xid 创建文件会出错。为避免这些问题，我们决定等待中止完成。
				 *
				 * Note, it is okay to not update the flush location position
				 * for aborts as in worst case that means such a transaction
				 * won't be sent again after restart.
				 *
				 * 注意，不为中止更新刷盘位置是可以的，最坏情况是重启后这样的事务不会再次发送。
				 */
				if (toplevel_xact)
					pa_xact_finish(winfo, InvalidXLogRecPtr);

				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 *
			 * 无法把变更发给并行 apply worker 时，切换到序列化模式。
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through
			 *
			 * 落入下一分支
			 */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			/*
			 * Parallel apply worker might have applied some changes, so write
			 * the STREAM_ABORT message so that it can rollback the
			 * subtransaction if needed.
			 *
			 * 并行 apply worker 可能已经应用了一些变更，因此写入
			 * STREAM_ABORT 消息，以便它在需要时回滚子事务。
			 */
			stream_open_and_write_change(xid, LOGICAL_REP_MSG_STREAM_ABORT,
										 &original_msg);

			if (toplevel_xact)
			{
				pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);
				pa_xact_finish(winfo, InvalidXLogRecPtr);
			}
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * If the parallel apply worker is applying spooled messages then
			 * close the file before aborting.
			 *
			 * 若并行 apply worker 正在应用暂存消息，则在中止之前关闭文件。
			 */
			if (toplevel_xact && stream_fd)
				stream_close_file();

			pa_stream_abort(&abort_data);

			/*
			 * We need to wait after processing rollback to savepoint for the
			 * next set of changes.
			 *
			 * 处理完 rollback to savepoint 后，需要等待下一批变更。
			 *
			 * We have a race condition here due to which we can start waiting
			 * here when there are more chunk of streams in the queue. See
			 * apply_handle_stream_stop.
			 *
			 * 这里有竞态，队列中还有更多流数据块时我们也可能开始等待。
			 * 见 apply_handle_stream_stop。
			 */
			if (!toplevel_xact)
				pa_decr_and_wait_stream_block();

			elog(DEBUG1, "finished processing the STREAM ABORT command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	reset_apply_error_context_info();
}

/*
 * Ensure that the passed location is fileset's end.
 *
 * 确保传入的位置是 fileset 的末尾。
 */
static void
ensure_last_message(FileSet *stream_fileset, TransactionId xid, int fileno,
					off_t offset)
{
	char		path[MAXPGPATH];
	BufFile    *fd;
	int			last_fileno;
	off_t		last_offset;

	Assert(!IsTransactionState());

	begin_replication_step();

	changes_filename(path, MyLogicalRepWorker->subid, xid);

	fd = BufFileOpenFileSet(stream_fileset, path, O_RDONLY, false);

	BufFileSeek(fd, 0, 0, SEEK_END);
	BufFileTell(fd, &last_fileno, &last_offset);

	BufFileClose(fd);

	end_replication_step();

	if (last_fileno != fileno || last_offset != offset)
		elog(ERROR, "unexpected message left in streaming transaction's changes file \"%s\"",
			 path);
}

/*
 * Common spoolfile processing.
 *
 * 处理 spool 文件的公共逻辑。
 */
void
apply_spooled_messages(FileSet *stream_fileset, TransactionId xid,
					   XLogRecPtr lsn)
{
	int			nchanges;
	char		path[MAXPGPATH];
	char	   *buffer = NULL;
	MemoryContext oldcxt;
	ResourceOwner oldowner;
	int			fileno;
	off_t		offset;

	if (!am_parallel_apply_worker())
		maybe_start_skipping_changes(lsn);

	/* Make sure we have an open transaction
	 *
	 * 确保已有打开的事务
	 */
	begin_replication_step();

	/*
	 * Allocate file handle and memory required to process all the messages in
	 * TopTransactionContext to avoid them getting reset after each message is
	 * processed.
	 *
	 * 在 TopTransactionContext 中分配处理全部消息所需的文件句柄和内存，避免每处理一条消息就被重置。
	 */
	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	/* Open the spool file for the committed/prepared transaction
	 *
	 * 打开已提交或已准备事务的 spool 文件
	 */
	changes_filename(path, MyLogicalRepWorker->subid, xid);
	elog(DEBUG1, "replaying changes from file \"%s\"", path);

	/*
	 * Make sure the file is owned by the toplevel transaction so that the
	 * file will not be accidentally closed when aborting a subtransaction.
	 *
	 * 确保文件归顶层事务所有，以免中止子事务时被意外关闭。
	 */
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = TopTransactionResourceOwner;

	stream_fd = BufFileOpenFileSet(stream_fileset, path, O_RDONLY, false);

	CurrentResourceOwner = oldowner;

	buffer = palloc(BLCKSZ);

	MemoryContextSwitchTo(oldcxt);

	remote_final_lsn = lsn;

	/*
	 * Make sure the handle apply_dispatch methods are aware we're in a remote
	 * transaction.
	 *
	 * 确保 apply_dispatch 的处理函数知道当前处于远端事务中。
	 */
	in_remote_transaction = true;
	pgstat_report_activity(STATE_RUNNING, NULL);

	end_replication_step();

	/*
	 * Read the entries one by one and pass them through the same logic as in
	 * apply_dispatch.
	 *
	 * 逐条读取记录，并走与 apply_dispatch 相同的逻辑。
	 */
	nchanges = 0;
	while (true)
	{
		StringInfoData s2;
		size_t		nbytes;
		int			len;

		CHECK_FOR_INTERRUPTS();

		/* read length of the on-disk record
		 *
		 * 读取磁盘记录的长度
		 */
		nbytes = BufFileReadMaybeEOF(stream_fd, &len, sizeof(len), true);

		/* have we reached end of the file?
		 *
		 * 是否已到达文件末尾？
		 */
		if (nbytes == 0)
			break;

		/* do we have a correct length?
		 *
		 * 长度是否正确？
		 */
		if (len <= 0)
			elog(ERROR, "incorrect length %d in streaming transaction's changes file \"%s\"",
				 len, path);

		/* make sure we have sufficiently large buffer
		 *
		 * 确保缓冲区足够大
		 */
		buffer = repalloc(buffer, len);

		/* and finally read the data into the buffer
		 *
		 * 最后把数据读入缓冲区
		 */
		BufFileReadExact(stream_fd, buffer, len);

		BufFileTell(stream_fd, &fileno, &offset);

		/* init a stringinfo using the buffer and call apply_dispatch
		 *
		 * 用该缓冲区初始化 stringinfo 并调用 apply_dispatch
		 */
		initReadOnlyStringInfo(&s2, buffer, len);

		/* Ensure we are reading the data into our memory context.
		 *
		 * 确保把数据读入我们的内存上下文。
		 */
		oldcxt = MemoryContextSwitchTo(ApplyMessageContext);

		apply_dispatch(&s2);

		MemoryContextReset(ApplyMessageContext);

		MemoryContextSwitchTo(oldcxt);

		nchanges++;

		/*
		 * It is possible the file has been closed because we have processed
		 * the transaction end message like stream_commit in which case that
		 * must be the last message.
		 *
		 * 文件可能已经关闭，因为我们已经处理了事务结束消息（例如 stream_commit）
		 * ，那种情况下它必须是最后一条消息。
		 */
		if (!stream_fd)
		{
			ensure_last_message(stream_fileset, xid, fileno, offset);
			break;
		}

		if (nchanges % 1000 == 0)
			elog(DEBUG1, "replayed %d changes from file \"%s\"",
				 nchanges, path);
	}

	if (stream_fd)
		stream_close_file();

	elog(DEBUG1, "replayed %d (all) changes from file \"%s\"",
		 nchanges, path);

	return;
}

/*
 * Handle STREAM COMMIT message.
 *
 * 处理 STREAM COMMIT 消息。
 */
static void
apply_handle_stream_commit(StringInfo s)
{
	TransactionId xid;
	LogicalRepCommitData commit_data;
	ParallelApplyWorkerInfo *winfo;
	TransApplyAction apply_action;

	/* Save the message before it is consumed.
	 *
	 * 在消息被消费之前先保存它。
	 */
	StringInfoData original_msg = *s;

	if (in_streamed_transaction)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg_internal("STREAM COMMIT message without STREAM STOP")));

	xid = logicalrep_read_stream_commit(s, &commit_data);
	set_apply_error_context_xact(xid, commit_data.commit_lsn);

	apply_action = get_transaction_apply_action(xid, &winfo);

	switch (apply_action)
	{
		case TRANS_LEADER_APPLY:

			/*
			 * The transaction has been serialized to file, so replay all the
			 * spooled operations.
			 *
			 * 事务已序列化到文件，因此重放全部暂存的操作。
			 */
			apply_spooled_messages(MyLogicalRepWorker->stream_fileset, xid,
								   commit_data.commit_lsn);

			apply_handle_commit_internal(&commit_data);

			/* Unlink the files with serialized changes and subxact info.
			 *
			 * 删除保存序列化变更和子事务信息的文件。
			 */
			stream_cleanup_files(MyLogicalRepWorker->subid, xid);

			elog(DEBUG1, "finished processing the STREAM COMMIT command");
			break;

		case TRANS_LEADER_SEND_TO_PARALLEL:
			Assert(winfo);

			if (pa_send_data(winfo, s->len, s->data))
			{
				/* Finish processing the streaming transaction.
				 *
				 * 结束对流式事务的处理。
				 */
				pa_xact_finish(winfo, commit_data.end_lsn);
				break;
			}

			/*
			 * Switch to serialize mode when we are not able to send the
			 * change to parallel apply worker.
			 *
			 * 无法把变更发给并行 apply worker 时，切换到序列化模式。
			 */
			pa_switch_to_partial_serialize(winfo, true);

			/* fall through
			 *
			 * 落入下一分支
			 */
		case TRANS_LEADER_PARTIAL_SERIALIZE:
			Assert(winfo);

			stream_open_and_write_change(xid, LOGICAL_REP_MSG_STREAM_COMMIT,
										 &original_msg);

			pa_set_fileset_state(winfo->shared, FS_SERIALIZE_DONE);

			/* Finish processing the streaming transaction.
			 *
			 * 结束对流式事务的处理。
			 */
			pa_xact_finish(winfo, commit_data.end_lsn);
			break;

		case TRANS_PARALLEL_APPLY:

			/*
			 * If the parallel apply worker is applying spooled messages then
			 * close the file before committing.
			 *
			 * 若并行 apply worker 正在应用暂存消息，则在提交之前关闭文件。
			 */
			if (stream_fd)
				stream_close_file();

			apply_handle_commit_internal(&commit_data);

			MyParallelShared->last_commit_end = XactLastCommitEnd;

			/*
			 * It is important to set the transaction state as finished before
			 * releasing the lock. See pa_wait_for_xact_finish.
			 *
			 * 释放锁之前必须把事务状态设为已结束。见 pa_wait_for_xact_finish。
			 */
			pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);
			pa_unlock_transaction(xid, AccessExclusiveLock);

			pa_reset_subtrans();

			elog(DEBUG1, "finished processing the STREAM COMMIT command");
			break;

		default:
			elog(ERROR, "unexpected apply action: %d", (int) apply_action);
			break;
	}

	/* Process any tables that are being synchronized in parallel.
	 *
	 * 处理正在并行同步的表。
	 */
	process_syncing_tables(commit_data.end_lsn);

	pgstat_report_activity(STATE_IDLE, NULL);

	reset_apply_error_context_info();
}

/*
 * Helper function for apply_handle_commit and apply_handle_stream_commit.
 *
 * apply_handle_commit 和 apply_handle_stream_commit 的辅助函数。
 */
static void
apply_handle_commit_internal(LogicalRepCommitData *commit_data)
{
	if (is_skipping_changes())
	{
		stop_skipping_changes();

		/*
		 * Start a new transaction to clear the subskiplsn, if not started
		 * yet.
		 *
		 * 若尚未开始事务，则启动一个新事务以清除 subskiplsn。
		 */
		if (!IsTransactionState())
			StartTransactionCommand();
	}

	if (IsTransactionState())
	{
		/*
		 * The transaction is either non-empty or skipped, so we clear the
		 * subskiplsn.
		 *
		 * 该事务非空或者已被跳过，因此清除 subskiplsn。
		 */
		clear_subscription_skip_lsn(commit_data->commit_lsn);

		/*
		 * Update origin state so we can restart streaming from correct
		 * position in case of crash.
		 *
		 * 更新 origin 状态，以便崩溃后能从正确位置重新开始流式传输。
		 */
		replorigin_session_origin_lsn = commit_data->end_lsn;
		replorigin_session_origin_timestamp = commit_data->committime;

		CommitTransactionCommand();

		if (IsTransactionBlock())
		{
			EndTransactionBlock(false);
			CommitTransactionCommand();
		}

		pgstat_report_stat(false);

		store_flush_position(commit_data->end_lsn, XactLastCommitEnd);
	}
	else
	{
		/* Process any invalidation messages that might have accumulated.
		 *
		 * 处理可能已经积压的失效消息。
		 */
		AcceptInvalidationMessages();
		maybe_reread_subscription();
	}

	in_remote_transaction = false;
}

/*
 * Handle RELATION message.
 *
 * 处理 RELATION 消息。
 *
 * Note we don't do validation against local schema here. The validation
 * against local schema is postponed until first change for given relation
 * comes as we only care about it when applying changes for it anyway and we
 * do less locking this way.
 *
 * 这里不对照本地模式做校验。校验推迟到该关系的第一次变更到来时，因为我们只在应用它的变更时才关心，
 * 而且这样加锁更少。
 */
static void
apply_handle_relation(StringInfo s)
{
	LogicalRepRelation *rel;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_RELATION, s))
		return;

	rel = logicalrep_read_rel(s);
	logicalrep_relmap_update(rel);

	/* Also reset all entries in the partition map that refer to remoterel.
	 *
	 * 同时重置分区映射中所有引用 remoterel 的项。
	 */
	logicalrep_partmap_reset_relmap(rel);
}

/*
 * Handle TYPE message.
 *
 * 处理 TYPE 消息。
 *
 * This implementation pays no attention to TYPE messages; we expect the user
 * to have set things up so that the incoming data is acceptable to the input
 * functions for the locally subscribed tables.  Hence, we just read and
 * discard the message.
 *
 * 本实现不关注 TYPE 消息；我们期望用户已经设置好，使传入数据能被本地订阅表的输入函数接受。
 * 因此只读取并丢弃该消息。
 */
static void
apply_handle_type(StringInfo s)
{
	LogicalRepTyp typ;

	if (handle_streamed_transaction(LOGICAL_REP_MSG_TYPE, s))
		return;

	logicalrep_read_typ(s, &typ);
}

/*
 * Check that we (the subscription owner) have sufficient privileges on the
 * target relation to perform the given operation.
 *
 * 检查我们（订阅所有者）对目标关系是否有足够权限来执行给定操作。
 */
static void
TargetPrivilegesCheck(Relation rel, AclMode mode)
{
	Oid			relid;
	AclResult	aclresult;

	relid = RelationGetRelid(rel);
	aclresult = pg_class_aclcheck(relid, GetUserId(), mode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   get_rel_name(relid));

	/*
	 * We lack the infrastructure to honor RLS policies.  It might be possible
	 * to add such infrastructure here, but tablesync workers lack it, too, so
	 * we don't bother.  RLS does not ordinarily apply to TRUNCATE commands,
	 * but it seems dangerous to replicate a TRUNCATE and then refuse to
	 * replicate subsequent INSERTs, so we forbid all commands the same.
	 *
	 * 我们缺少遵守 RLS 策略的基础设施。也许可以在这里补上，但 tablesync worker
	 * 同样没有，所以不做。RLS 通常不适用于 TRUNCATE 命令，但如果复制了 TRUNCATE
	 * 却拒绝复制后续的 INSERT，会很危险，因此对所有命令一视同仁地禁止。
	 */
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("user \"%s\" cannot replicate into relation with row-level security enabled: \"%s\"",
						GetUserNameFromId(GetUserId(), true),
						RelationGetRelationName(rel))));
}

/*
 * Handle INSERT message.
 *
 * 处理 INSERT 消息。
 */

static void
apply_handle_insert(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData newtup;
	LogicalRepRelId relid;
	UserContext ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 *
	 * 若正在跳过数据修改变更或正在处理流式事务，则快速返回。
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_INSERT, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_insert(s, &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 *
		 * 事务中途该关系不会变得需要关注，因此解锁是安全的。
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/*
	 * Make sure that any user-supplied code runs as the table owner, unless
	 * the user has opted out of that behavior.
	 *
	 * 确保任何用户提供的代码都以表所有者身份运行，除非用户选择退出该行为。
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* Set relation for error callback
	 *
	 * 为错误回调设置关系
	 */
	apply_error_callback_arg.rel = rel;

	/* Initialize the executor state.
	 *
	 * 初始化执行器状态。
	 */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/* Process and store remote tuple in the slot
	 *
	 * 处理远端元组并存入 slot
	 */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &newtup);
	slot_fill_defaults(rel, estate, remoteslot);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, insert the tuple into a partition.
	 *
	 * 若是分区表，把元组插入某个分区。
	 */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, NULL, CMD_INSERT);
	else
	{
		ResultRelInfo *relinfo = edata->targetRelInfo;

		ExecOpenIndices(relinfo, false);
		apply_handle_insert_internal(edata, relinfo, remoteslot);
		ExecCloseIndices(relinfo);
	}

	finish_edata(edata);

	/* Reset relation for error callback
	 *
	 * 为错误回调重置关系
	 */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * Workhorse for apply_handle_insert()
 * relinfo is for the relation we're actually inserting into
 * (could be a child partition of edata->targetRelInfo)
 *
 * apply_handle_insert() 的主要实现。relinfo 是实际插入的关系，可能是 edata->targetRelInfo
 * 的子分区。
 */
static void
apply_handle_insert_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot)
{
	EState	   *estate = edata->estate;

	/* Caller should have opened indexes already.
	 *
	 * 调用方应该已经打开索引。
	 */
	Assert(relinfo->ri_IndexRelationDescs != NULL ||
		   !relinfo->ri_RelationDesc->rd_rel->relhasindex ||
		   RelationGetIndexList(relinfo->ri_RelationDesc) == NIL);

	/* Caller will not have done this bit.
	 *
	 * 调用方不会做这一步。
	 */
	Assert(relinfo->ri_onConflictArbiterIndexes == NIL);
	InitConflictIndexes(relinfo);

	/* Do the insert.
	 *
	 * 执行插入。
	 */
	TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_INSERT);
	ExecSimpleRelationInsert(relinfo, estate, remoteslot);
}

/*
 * Check if the logical replication relation is updatable and throw
 * appropriate error if it isn't.
 *
 * 检查逻辑复制关系是否可更新，若不可更新则抛出相应错误。
 */
static void
check_relation_updatable(LogicalRepRelMapEntry *rel)
{
	/*
	 * For partitioned tables, we only need to care if the target partition is
	 * updatable (aka has PK or RI defined for it).
	 *
	 * 对分区表，只需关心目标分区是否可更新，即是否为其定义了主键或副本标识。
	 */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* Updatable, no error.
	 *
	 * 可更新，不报错。
	 */
	if (rel->updatable)
		return;

	/*
	 * We are in error mode so it's fine this is somewhat slow. It's better to
	 * give user correct error.
	 *
	 * 当前处于错误处理路径，稍慢一点没关系。把正确的错误信息给用户更重要。
	 */
	if (OidIsValid(GetRelationIdentityOrPK(rel->localrel)))
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publisher did not send replica identity column "
						"expected by the logical replication target relation \"%s.%s\"",
						rel->remoterel.nspname, rel->remoterel.relname)));
	}

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("logical replication target relation \"%s.%s\" has "
					"neither REPLICA IDENTITY index nor PRIMARY "
					"KEY and published relation does not have "
					"REPLICA IDENTITY FULL",
					rel->remoterel.nspname, rel->remoterel.relname)));
}

/*
 * Handle UPDATE message.
 *
 * 处理 UPDATE 消息。
 *
 * TODO: FDW support
 *
 * TODO：FDW 支持
 */
static void
apply_handle_update(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepRelId relid;
	UserContext ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	LogicalRepTupleData oldtup;
	LogicalRepTupleData newtup;
	bool		has_oldtup;
	TupleTableSlot *remoteslot;
	RTEPermissionInfo *target_perminfo;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 *
	 * 若正在跳过数据修改变更或正在处理流式事务，则快速返回。
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_UPDATE, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_update(s, &has_oldtup, &oldtup,
								   &newtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 *
		 * 事务中途该关系不会变得需要关注，因此解锁是安全的。
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/* Set relation for error callback
	 *
	 * 为错误回调设置关系
	 */
	apply_error_callback_arg.rel = rel;

	/* Check if we can do the update.
	 *
	 * 检查是否可以执行更新。
	 */
	check_relation_updatable(rel);

	/*
	 * Make sure that any user-supplied code runs as the table owner, unless
	 * the user has opted out of that behavior.
	 *
	 * 确保任何用户提供的代码都以表所有者身份运行，除非用户选择退出该行为。
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* Initialize the executor state.
	 *
	 * 初始化执行器状态。
	 */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/*
	 * Populate updatedCols so that per-column triggers can fire, and so
	 * executor can correctly pass down indexUnchanged hint.  This could
	 * include more columns than were actually changed on the publisher
	 * because the logical replication protocol doesn't contain that
	 * information.  But it would for example exclude columns that only exist
	 * on the subscriber, since we are not touching those.
	 *
	 * 填充 updatedCols，以便按列触发器能够触发，并且执行器能正确向下传递
	 * indexUnchanged 提示。这里可能包含比发布端实际变更更多的列，因为逻辑复制协议不包含那份信息。
	 * 但例如只存在于订阅端的列会被排除，因为我们并不触碰它们。
	 */
	target_perminfo = list_nth(estate->es_rteperminfos, 0);
	for (int i = 0; i < remoteslot->tts_tupleDescriptor->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(remoteslot->tts_tupleDescriptor, i);
		int			remoteattnum = rel->attrmap->attnums[i];

		if (!att->attisdropped && remoteattnum >= 0)
		{
			Assert(remoteattnum < newtup.ncols);
			if (newtup.colstatus[remoteattnum] != LOGICALREP_COLUMN_UNCHANGED)
				target_perminfo->updatedCols =
					bms_add_member(target_perminfo->updatedCols,
								   i + 1 - FirstLowInvalidHeapAttributeNumber);
		}
	}

	/* Build the search tuple.
	 *
	 * 构造用于查找的元组。
	 */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel,
					has_oldtup ? &oldtup : &newtup);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, apply update to correct partition.
	 *
	 * 若是分区表，把更新应用到正确的分区。
	 */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, &newtup, CMD_UPDATE);
	else
		apply_handle_update_internal(edata, edata->targetRelInfo,
									 remoteslot, &newtup, rel->localindexoid);

	finish_edata(edata);

	/* Reset relation for error callback
	 *
	 * 为错误回调重置关系
	 */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * Workhorse for apply_handle_update()
 * relinfo is for the relation we're actually updating in
 * (could be a child partition of edata->targetRelInfo)
 *
 * apply_handle_update() 的主要实现。relinfo 是实际更新的关系，可能是 edata->targetRelInfo
 * 的子分区。
 */
static void
apply_handle_update_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot,
							 LogicalRepTupleData *newtup,
							 Oid localindexoid)
{
	EState	   *estate = edata->estate;
	LogicalRepRelMapEntry *relmapentry = edata->targetRel;
	Relation	localrel = relinfo->ri_RelationDesc;
	EPQState	epqstate;
	TupleTableSlot *localslot = NULL;
	ConflictTupleInfo conflicttuple = {0};
	bool		found;
	MemoryContext oldctx;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1, NIL);
	ExecOpenIndices(relinfo, false);

	found = FindReplTupleInLocalRel(edata, localrel,
									&relmapentry->remoterel,
									localindexoid,
									remoteslot, &localslot);

	/*
	 * Tuple found.
	 *
	 * 已找到元组。
	 *
	 * Note this will fail if there are other conflicting unique indexes.
	 *
	 * 注意：若还有其他冲突的唯一索引，这里会失败。
	 */
	if (found)
	{
		/*
		 * Report the conflict if the tuple was modified by a different
		 * origin.
		 *
		 * 若元组被不同的 origin 修改过，则报告冲突。
		 */
		if (GetTupleTransactionInfo(localslot, &conflicttuple.xmin,
									&conflicttuple.origin, &conflicttuple.ts) &&
			conflicttuple.origin != replorigin_session_origin)
		{
			TupleTableSlot *newslot;

			/* Store the new tuple for conflict reporting
			 *
			 * 保存新元组，供冲突报告使用
			 */
			newslot = table_slot_create(localrel, &estate->es_tupleTable);
			slot_store_data(newslot, relmapentry, newtup);

			conflicttuple.slot = localslot;

			ReportApplyConflict(estate, relinfo, LOG, CT_UPDATE_ORIGIN_DIFFERS,
								remoteslot, newslot,
								list_make1(&conflicttuple));
		}

		/* Process and store remote tuple in the slot
		 *
		 * 处理远端元组并存入 slot
		 */
		oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
		slot_modify_data(remoteslot, localslot, relmapentry, newtup);
		MemoryContextSwitchTo(oldctx);

		EvalPlanQualSetSlot(&epqstate, remoteslot);

		InitConflictIndexes(relinfo);

		/* Do the actual update.
		 *
		 * 执行实际的更新。
		 */
		TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_UPDATE);
		ExecSimpleRelationUpdate(relinfo, estate, &epqstate, localslot,
								 remoteslot);
	}
	else
	{
		TupleTableSlot *newslot = localslot;

		/* Store the new tuple for conflict reporting
		 *
		 * 保存新元组，供冲突报告使用
		 */
		slot_store_data(newslot, relmapentry, newtup);

		/*
		 * The tuple to be updated could not be found.  Do nothing except for
		 * emitting a log message.
		 *
		 * 找不到要更新的元组。除了发出一条日志外什么也不做。
		 */
		ReportApplyConflict(estate, relinfo, LOG, CT_UPDATE_MISSING,
							remoteslot, newslot, list_make1(&conflicttuple));
	}

	/* Cleanup.
	 *
	 * 清理。
	 */
	ExecCloseIndices(relinfo);
	EvalPlanQualEnd(&epqstate);
}

/*
 * Handle DELETE message.
 *
 * 处理 DELETE 消息。
 *
 * TODO: FDW support
 *
 * TODO：FDW 支持
 */
static void
apply_handle_delete(StringInfo s)
{
	LogicalRepRelMapEntry *rel;
	LogicalRepTupleData oldtup;
	LogicalRepRelId relid;
	UserContext ucxt;
	ApplyExecutionData *edata;
	EState	   *estate;
	TupleTableSlot *remoteslot;
	MemoryContext oldctx;
	bool		run_as_owner;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 *
	 * 若正在跳过数据修改变更或正在处理流式事务，则快速返回。
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_DELETE, s))
		return;

	begin_replication_step();

	relid = logicalrep_read_delete(s, &oldtup);
	rel = logicalrep_rel_open(relid, RowExclusiveLock);
	if (!should_apply_changes_for_rel(rel))
	{
		/*
		 * The relation can't become interesting in the middle of the
		 * transaction so it's safe to unlock it.
		 *
		 * 事务中途该关系不会变得需要关注，因此解锁是安全的。
		 */
		logicalrep_rel_close(rel, RowExclusiveLock);
		end_replication_step();
		return;
	}

	/* Set relation for error callback
	 *
	 * 为错误回调设置关系
	 */
	apply_error_callback_arg.rel = rel;

	/* Check if we can do the delete.
	 *
	 * 检查是否可以执行删除。
	 */
	check_relation_updatable(rel);

	/*
	 * Make sure that any user-supplied code runs as the table owner, unless
	 * the user has opted out of that behavior.
	 *
	 * 确保任何用户提供的代码都以表所有者身份运行，除非用户选择退出该行为。
	 */
	run_as_owner = MySubscription->runasowner;
	if (!run_as_owner)
		SwitchToUntrustedUser(rel->localrel->rd_rel->relowner, &ucxt);

	/* Initialize the executor state.
	 *
	 * 初始化执行器状态。
	 */
	edata = create_edata_for_relation(rel);
	estate = edata->estate;
	remoteslot = ExecInitExtraTupleSlot(estate,
										RelationGetDescr(rel->localrel),
										&TTSOpsVirtual);

	/* Build the search tuple.
	 *
	 * 构造用于查找的元组。
	 */
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	slot_store_data(remoteslot, rel, &oldtup);
	MemoryContextSwitchTo(oldctx);

	/* For a partitioned table, apply delete to correct partition.
	 *
	 * 若是分区表，把删除应用到正确的分区。
	 */
	if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		apply_handle_tuple_routing(edata,
								   remoteslot, NULL, CMD_DELETE);
	else
	{
		ResultRelInfo *relinfo = edata->targetRelInfo;

		ExecOpenIndices(relinfo, false);
		apply_handle_delete_internal(edata, relinfo,
									 remoteslot, rel->localindexoid);
		ExecCloseIndices(relinfo);
	}

	finish_edata(edata);

	/* Reset relation for error callback
	 *
	 * 为错误回调重置关系
	 */
	apply_error_callback_arg.rel = NULL;

	if (!run_as_owner)
		RestoreUserContext(&ucxt);

	logicalrep_rel_close(rel, NoLock);

	end_replication_step();
}

/*
 * Workhorse for apply_handle_delete()
 * relinfo is for the relation we're actually deleting from
 * (could be a child partition of edata->targetRelInfo)
 *
 * apply_handle_delete() 的主要实现。relinfo 是实际删除的关系，可能是 edata->targetRelInfo
 * 的子分区。
 */
static void
apply_handle_delete_internal(ApplyExecutionData *edata,
							 ResultRelInfo *relinfo,
							 TupleTableSlot *remoteslot,
							 Oid localindexoid)
{
	EState	   *estate = edata->estate;
	Relation	localrel = relinfo->ri_RelationDesc;
	LogicalRepRelation *remoterel = &edata->targetRel->remoterel;
	EPQState	epqstate;
	TupleTableSlot *localslot;
	ConflictTupleInfo conflicttuple = {0};
	bool		found;

	EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1, NIL);

	/* Caller should have opened indexes already.
	 *
	 * 调用方应该已经打开索引。
	 */
	Assert(relinfo->ri_IndexRelationDescs != NULL ||
		   !localrel->rd_rel->relhasindex ||
		   RelationGetIndexList(localrel) == NIL);

	found = FindReplTupleInLocalRel(edata, localrel, remoterel, localindexoid,
									remoteslot, &localslot);

	/* If found delete it.
	 *
	 * 若找到则删除它。
	 */
	if (found)
	{
		/*
		 * Report the conflict if the tuple was modified by a different
		 * origin.
		 *
		 * 若元组被不同的 origin 修改过，则报告冲突。
		 */
		if (GetTupleTransactionInfo(localslot, &conflicttuple.xmin,
									&conflicttuple.origin, &conflicttuple.ts) &&
			conflicttuple.origin != replorigin_session_origin)
		{
			conflicttuple.slot = localslot;
			ReportApplyConflict(estate, relinfo, LOG, CT_DELETE_ORIGIN_DIFFERS,
								remoteslot, NULL,
								list_make1(&conflicttuple));
		}

		EvalPlanQualSetSlot(&epqstate, localslot);

		/* Do the actual delete.
		 *
		 * 执行实际的删除。
		 */
		TargetPrivilegesCheck(relinfo->ri_RelationDesc, ACL_DELETE);
		ExecSimpleRelationDelete(relinfo, estate, &epqstate, localslot);
	}
	else
	{
		/*
		 * The tuple to be deleted could not be found.  Do nothing except for
		 * emitting a log message.
		 *
		 * 找不到要删除的元组。除了发出一条日志外什么也不做。
		 */
		ReportApplyConflict(estate, relinfo, LOG, CT_DELETE_MISSING,
							remoteslot, NULL, list_make1(&conflicttuple));
	}

	/* Cleanup.
	 *
	 * 清理。
	 */
	EvalPlanQualEnd(&epqstate);
}

/*
 * Try to find a tuple received from the publication side (in 'remoteslot') in
 * the corresponding local relation using either replica identity index,
 * primary key, index or if needed, sequential scan.
 *
 * 尝试在对应的本地关系中查找从发布端收到的元组（位于 remoteslot），使用副本标识索引、
 * 主键、索引，必要时使用顺序扫描。
 *
 * Local tuple, if found, is returned in '*localslot'.
 *
 * 若找到本地元组，则写入 localslot 并返回。
 */
static bool
FindReplTupleInLocalRel(ApplyExecutionData *edata, Relation localrel,
						LogicalRepRelation *remoterel,
						Oid localidxoid,
						TupleTableSlot *remoteslot,
						TupleTableSlot **localslot)
{
	EState	   *estate = edata->estate;
	bool		found;

	/*
	 * Regardless of the top-level operation, we're performing a read here, so
	 * check for SELECT privileges.
	 *
	 * 无论顶层操作是什么，这里执行的是读取，因此检查 SELECT 权限。
	 */
	TargetPrivilegesCheck(localrel, ACL_SELECT);

	*localslot = table_slot_create(localrel, &estate->es_tupleTable);

	Assert(OidIsValid(localidxoid) ||
		   (remoterel->replident == REPLICA_IDENTITY_FULL));

	if (OidIsValid(localidxoid))
	{
#ifdef USE_ASSERT_CHECKING
		Relation	idxrel = index_open(localidxoid, AccessShareLock);

		/* Index must be PK, RI, or usable for REPLICA IDENTITY FULL tables
		 *
		 * 索引必须是主键、副本标识，或可用于 REPLICA IDENTITY FULL
		 * 的表
		 */
		Assert(GetRelationIdentityOrPK(localrel) == localidxoid ||
			   (remoterel->replident == REPLICA_IDENTITY_FULL &&
				IsIndexUsableForReplicaIdentityFull(idxrel,
													edata->targetRel->attrmap)));
		index_close(idxrel, AccessShareLock);
#endif

		found = RelationFindReplTupleByIndex(localrel, localidxoid,
											 LockTupleExclusive,
											 remoteslot, *localslot);
	}
	else
		found = RelationFindReplTupleSeq(localrel, LockTupleExclusive,
										 remoteslot, *localslot);

	return found;
}

/*
 * This handles insert, update, delete on a partitioned table.
 *
 * 处理分区表上的 insert、update、delete。
 */
static void
apply_handle_tuple_routing(ApplyExecutionData *edata,
						   TupleTableSlot *remoteslot,
						   LogicalRepTupleData *newtup,
						   CmdType operation)
{
	EState	   *estate = edata->estate;
	LogicalRepRelMapEntry *relmapentry = edata->targetRel;
	ResultRelInfo *relinfo = edata->targetRelInfo;
	Relation	parentrel = relinfo->ri_RelationDesc;
	ModifyTableState *mtstate;
	PartitionTupleRouting *proute;
	ResultRelInfo *partrelinfo;
	Relation	partrel;
	TupleTableSlot *remoteslot_part;
	TupleConversionMap *map;
	MemoryContext oldctx;
	LogicalRepRelMapEntry *part_entry = NULL;
	AttrMap    *attrmap = NULL;

	/* ModifyTableState is needed for ExecFindPartition().
	 *
	 * ExecFindPartition() 需要 ModifyTableState。
	 */
	edata->mtstate = mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = NULL;
	mtstate->ps.state = estate;
	mtstate->operation = operation;
	mtstate->resultRelInfo = relinfo;

	/* ... as is PartitionTupleRouting.
	 *
	 * PartitionTupleRouting 也同样需要。
	 */
	edata->proute = proute = ExecSetupPartitionTupleRouting(estate, parentrel);

	/*
	 * Find the partition to which the "search tuple" belongs.
	 *
	 * 找出搜索元组所属的分区。
	 */
	Assert(remoteslot != NULL);
	oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
	partrelinfo = ExecFindPartition(mtstate, relinfo, proute,
									remoteslot, estate);
	Assert(partrelinfo != NULL);
	partrel = partrelinfo->ri_RelationDesc;

	/*
	 * Check for supported relkind.  We need this since partitions might be of
	 * unsupported relkinds; and the set of partitions can change, so checking
	 * at CREATE/ALTER SUBSCRIPTION would be insufficient.
	 *
	 * 检查是否为支持的 relkind。需要这样做是因为分区可能是不支持的 relkind，
	 * 而且分区集合会变化，所以只在 CREATE/ALTER SUBSCRIPTION 时检查是不够的。
	 */
	CheckSubscriptionRelkind(partrel->rd_rel->relkind,
							 get_namespace_name(RelationGetNamespace(partrel)),
							 RelationGetRelationName(partrel));

	/*
	 * To perform any of the operations below, the tuple must match the
	 * partition's rowtype. Convert if needed or just copy, using a dedicated
	 * slot to store the tuple in any case.
	 *
	 * 要执行下面的任何操作，元组必须匹配分区的行类型。必要时转换，或者只是复制，
	 * 无论哪种情况都用专用 slot 存放元组。
	 */
	remoteslot_part = partrelinfo->ri_PartitionTupleSlot;
	if (remoteslot_part == NULL)
		remoteslot_part = table_slot_create(partrel, &estate->es_tupleTable);
	map = ExecGetRootToChildMap(partrelinfo, estate);
	if (map != NULL)
	{
		attrmap = map->attrMap;
		remoteslot_part = execute_attr_map_slot(attrmap, remoteslot,
												remoteslot_part);
	}
	else
	{
		remoteslot_part = ExecCopySlot(remoteslot_part, remoteslot);
		slot_getallattrs(remoteslot_part);
	}
	MemoryContextSwitchTo(oldctx);

	/* Check if we can do the update or delete on the leaf partition.
	 *
	 * 检查是否可以在叶子分区上执行更新或删除。
	 */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		part_entry = logicalrep_partition_open(relmapentry, partrel,
											   attrmap);
		check_relation_updatable(part_entry);
	}

	switch (operation)
	{
		case CMD_INSERT:
			apply_handle_insert_internal(edata, partrelinfo,
										 remoteslot_part);
			break;

		case CMD_DELETE:
			apply_handle_delete_internal(edata, partrelinfo,
										 remoteslot_part,
										 part_entry->localindexoid);
			break;

		case CMD_UPDATE:

			/*
			 * For UPDATE, depending on whether or not the updated tuple
			 * satisfies the partition's constraint, perform a simple UPDATE
			 * of the partition or move the updated tuple into a different
			 * suitable partition.
			 *
			 * 对于 UPDATE，根据更新后的元组是否仍满足该分区约束，
			 * 要么对该分区做简单 UPDATE，要么把更新后的元组移到另一个合适的分区。
			 */
			{
				TupleTableSlot *localslot;
				ResultRelInfo *partrelinfo_new;
				Relation	partrel_new;
				bool		found;
				EPQState	epqstate;
				ConflictTupleInfo conflicttuple = {0};

				/* Get the matching local tuple from the partition.
				 *
				 * 从该分区取得匹配的本地元组。
				 */
				found = FindReplTupleInLocalRel(edata, partrel,
												&part_entry->remoterel,
												part_entry->localindexoid,
												remoteslot_part, &localslot);
				if (!found)
				{
					TupleTableSlot *newslot = localslot;

					/* Store the new tuple for conflict reporting
					 *
					 * 保存新元组，供冲突报告使用
					 */
					slot_store_data(newslot, part_entry, newtup);

					/*
					 * The tuple to be updated could not be found.  Do nothing
					 * except for emitting a log message.
					 *
					 * 找不到要更新的元组。除了发出一条日志外什么也不做。
					 */
					ReportApplyConflict(estate, partrelinfo, LOG,
										CT_UPDATE_MISSING, remoteslot_part,
										newslot, list_make1(&conflicttuple));

					return;
				}

				/*
				 * Report the conflict if the tuple was modified by a
				 * different origin.
				 *
				 * 若元组被不同的 origin 修改过，则报告冲突。
				 */
				if (GetTupleTransactionInfo(localslot, &conflicttuple.xmin,
											&conflicttuple.origin,
											&conflicttuple.ts) &&
					conflicttuple.origin != replorigin_session_origin)
				{
					TupleTableSlot *newslot;

					/* Store the new tuple for conflict reporting
					 *
					 * 保存新元组，供冲突报告使用
					 */
					newslot = table_slot_create(partrel, &estate->es_tupleTable);
					slot_store_data(newslot, part_entry, newtup);

					conflicttuple.slot = localslot;

					ReportApplyConflict(estate, partrelinfo, LOG, CT_UPDATE_ORIGIN_DIFFERS,
										remoteslot_part, newslot,
										list_make1(&conflicttuple));
				}

				/*
				 * Apply the update to the local tuple, putting the result in
				 * remoteslot_part.
				 *
				 * 把更新应用到本地元组，结果放入 remoteslot_part。
				 */
				oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
				slot_modify_data(remoteslot_part, localslot, part_entry,
								 newtup);
				MemoryContextSwitchTo(oldctx);

				EvalPlanQualInit(&epqstate, estate, NULL, NIL, -1, NIL);

				/*
				 * Does the updated tuple still satisfy the current
				 * partition's constraint?
				 *
				 * 更新后的元组是否仍满足当前分区的约束？
				 */
				if (!partrel->rd_rel->relispartition ||
					ExecPartitionCheck(partrelinfo, remoteslot_part, estate,
									   false))
				{
					/*
					 * Yes, so simply UPDATE the partition.  We don't call
					 * apply_handle_update_internal() here, which would
					 * normally do the following work, to avoid repeating some
					 * work already done above to find the local tuple in the
					 * partition.
					 *
					 * 是，因此直接 UPDATE 该分区。这里不调用
					 * apply_handle_update_internal()，它通常会做下面这些工作，
					 * 以避免重复上面为在分区中查找本地元组已经做过的工作。
					 */
					InitConflictIndexes(partrelinfo);

					EvalPlanQualSetSlot(&epqstate, remoteslot_part);
					TargetPrivilegesCheck(partrelinfo->ri_RelationDesc,
										  ACL_UPDATE);
					ExecSimpleRelationUpdate(partrelinfo, estate, &epqstate,
											 localslot, remoteslot_part);
				}
				else
				{
					/* Move the tuple into the new partition.
					 *
					 * 把元组移到新分区。
					 */

					/*
					 * New partition will be found using tuple routing, which
					 * can only occur via the parent table.  We might need to
					 * convert the tuple to the parent's rowtype.  Note that
					 * this is the tuple found in the partition, not the
					 * original search tuple received by this function.
					 *
					 * 新分区将通过元组路由找到，而元组路由只能经由父表进行。
					 * 我们可能需要把元组转换成父表的行类型。
					 * 注意这是在分区中找到的元组，不是本函数收到的原始搜索元组。
					 */
					if (map)
					{
						TupleConversionMap *PartitionToRootMap =
							convert_tuples_by_name(RelationGetDescr(partrel),
												   RelationGetDescr(parentrel));

						remoteslot =
							execute_attr_map_slot(PartitionToRootMap->attrMap,
												  remoteslot_part, remoteslot);
					}
					else
					{
						remoteslot = ExecCopySlot(remoteslot, remoteslot_part);
						slot_getallattrs(remoteslot);
					}

					/* Find the new partition.
					 *
					 * 找到新分区。
					 */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					partrelinfo_new = ExecFindPartition(mtstate, relinfo,
														proute, remoteslot,
														estate);
					MemoryContextSwitchTo(oldctx);
					Assert(partrelinfo_new != partrelinfo);
					partrel_new = partrelinfo_new->ri_RelationDesc;

					/* Check that new partition also has supported relkind.
					 *
					 * 检查新分区的 relkind 也受支持。
					 */
					CheckSubscriptionRelkind(partrel_new->rd_rel->relkind,
											 get_namespace_name(RelationGetNamespace(partrel_new)),
											 RelationGetRelationName(partrel_new));

					/* DELETE old tuple found in the old partition.
					 *
					 * 删除在旧分区中找到的旧元组。
					 */
					EvalPlanQualSetSlot(&epqstate, localslot);
					TargetPrivilegesCheck(partrelinfo->ri_RelationDesc, ACL_DELETE);
					ExecSimpleRelationDelete(partrelinfo, estate, &epqstate, localslot);

					/* INSERT new tuple into the new partition.
					 *
					 * 把新元组插入新分区。
					 */

					/*
					 * Convert the replacement tuple to match the destination
					 * partition rowtype.
					 *
					 * 把替换元组转换成目标分区的行类型。
					 */
					oldctx = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));
					remoteslot_part = partrelinfo_new->ri_PartitionTupleSlot;
					if (remoteslot_part == NULL)
						remoteslot_part = table_slot_create(partrel_new,
															&estate->es_tupleTable);
					map = ExecGetRootToChildMap(partrelinfo_new, estate);
					if (map != NULL)
					{
						remoteslot_part = execute_attr_map_slot(map->attrMap,
																remoteslot,
																remoteslot_part);
					}
					else
					{
						remoteslot_part = ExecCopySlot(remoteslot_part,
													   remoteslot);
						slot_getallattrs(remoteslot);
					}
					MemoryContextSwitchTo(oldctx);
					apply_handle_insert_internal(edata, partrelinfo_new,
												 remoteslot_part);
				}

				EvalPlanQualEnd(&epqstate);
			}
			break;

		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) operation);
			break;
	}
}

/*
 * Handle TRUNCATE message.
 *
 * 处理 TRUNCATE 消息。
 *
 * TODO: FDW support
 *
 * TODO：FDW 支持
 */
static void
apply_handle_truncate(StringInfo s)
{
	bool		cascade = false;
	bool		restart_seqs = false;
	List	   *remote_relids = NIL;
	List	   *remote_rels = NIL;
	List	   *rels = NIL;
	List	   *part_rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *lc;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/*
	 * Quick return if we are skipping data modification changes or handling
	 * streamed transactions.
	 *
	 * 若正在跳过数据修改变更或正在处理流式事务，则快速返回。
	 */
	if (is_skipping_changes() ||
		handle_streamed_transaction(LOGICAL_REP_MSG_TRUNCATE, s))
		return;

	begin_replication_step();

	remote_relids = logicalrep_read_truncate(s, &cascade, &restart_seqs);

	foreach(lc, remote_relids)
	{
		LogicalRepRelId relid = lfirst_oid(lc);
		LogicalRepRelMapEntry *rel;

		rel = logicalrep_rel_open(relid, lockmode);
		if (!should_apply_changes_for_rel(rel))
		{
			/*
			 * The relation can't become interesting in the middle of the
			 * transaction so it's safe to unlock it.
			 *
			 * 事务中途该关系不会变得需要关注，因此解锁是安全的。
			 */
			logicalrep_rel_close(rel, lockmode);
			continue;
		}

		remote_rels = lappend(remote_rels, rel);
		TargetPrivilegesCheck(rel->localrel, ACL_TRUNCATE);
		rels = lappend(rels, rel->localrel);
		relids = lappend_oid(relids, rel->localreloid);
		if (RelationIsLogicallyLogged(rel->localrel))
			relids_logged = lappend_oid(relids_logged, rel->localreloid);

		/*
		 * Truncate partitions if we got a message to truncate a partitioned
		 * table.
		 *
		 * 若收到的是截断分区表的消息，则截断其分区。
		 */
		if (rel->localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			ListCell   *child;
			List	   *children = find_all_inheritors(rel->localreloid,
													   lockmode,
													   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);
				Relation	childrel;

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors already got lock
				 *
				 * find_all_inheritors 已经取得锁
				 */
				childrel = table_open(childrelid, NoLock);

				/*
				 * Ignore temp tables of other backends.  See similar code in
				 * ExecuteTruncate().
				 *
				 * 忽略其他后端的临时表。见 ExecuteTruncate()
				 * 中的类似代码。
				 */
				if (RELATION_IS_OTHER_TEMP(childrel))
				{
					table_close(childrel, lockmode);
					continue;
				}

				TargetPrivilegesCheck(childrel, ACL_TRUNCATE);
				rels = lappend(rels, childrel);
				part_rels = lappend(part_rels, childrel);
				relids = lappend_oid(relids, childrelid);
				/* Log this relation only if needed for logical decoding
				 *
				 * 仅在逻辑解码需要时记录该关系
				 */
				if (RelationIsLogicallyLogged(childrel))
					relids_logged = lappend_oid(relids_logged, childrelid);
			}
		}
	}

	/*
	 * Even if we used CASCADE on the upstream primary we explicitly default
	 * to replaying changes without further cascading. This might be later
	 * changeable with a user specified option.
	 *
	 * 即使上游主库使用了 CASCADE，我们默认重放变更时也不再进一步级联。以后也许可以用用户指定的选项改变这一点。
	 *
	 * MySubscription->runasowner tells us whether we want to execute
	 * replication actions as the subscription owner; the last argument to
	 * TruncateGuts tells it whether we want to switch to the table owner.
	 * Those are exactly opposite conditions.
	 *
	 * MySubscription->runasowner 告诉我们是否要以订阅所有者身份执行复制动作；
	 * 传给 TruncateGuts 的最后一个参数告诉它是否要切换到表所有者。这两个条件恰好相反。
	 */
	ExecuteTruncateGuts(rels,
						relids,
						relids_logged,
						DROP_RESTRICT,
						restart_seqs,
						!MySubscription->runasowner);
	foreach(lc, remote_rels)
	{
		LogicalRepRelMapEntry *rel = lfirst(lc);

		logicalrep_rel_close(rel, NoLock);
	}
	foreach(lc, part_rels)
	{
		Relation	rel = lfirst(lc);

		table_close(rel, NoLock);
	}

	end_replication_step();
}


/*
 * Logical replication protocol message dispatcher.
 *
 * 逻辑复制协议消息分发器。
 */
void
apply_dispatch(StringInfo s)
{
	LogicalRepMsgType action = pq_getmsgbyte(s);
	LogicalRepMsgType saved_command;

	/*
	 * Set the current command being applied. Since this function can be
	 * called recursively when applying spooled changes, save the current
	 * command.
	 *
	 * 设置当前正在应用的命令。由于应用暂存变更时本函数可能递归调用，先保存当前命令。
	 */
	saved_command = apply_error_callback_arg.command;
	apply_error_callback_arg.command = action;

	switch (action)
	{
		case LOGICAL_REP_MSG_BEGIN:
			apply_handle_begin(s);
			break;

		case LOGICAL_REP_MSG_COMMIT:
			apply_handle_commit(s);
			break;

		case LOGICAL_REP_MSG_INSERT:
			apply_handle_insert(s);
			break;

		case LOGICAL_REP_MSG_UPDATE:
			apply_handle_update(s);
			break;

		case LOGICAL_REP_MSG_DELETE:
			apply_handle_delete(s);
			break;

		case LOGICAL_REP_MSG_TRUNCATE:
			apply_handle_truncate(s);
			break;

		case LOGICAL_REP_MSG_RELATION:
			apply_handle_relation(s);
			break;

		case LOGICAL_REP_MSG_TYPE:
			apply_handle_type(s);
			break;

		case LOGICAL_REP_MSG_ORIGIN:
			apply_handle_origin(s);
			break;

		case LOGICAL_REP_MSG_MESSAGE:

			/*
			 * Logical replication does not use generic logical messages yet.
			 * Although, it could be used by other applications that use this
			 * output plugin.
			 *
			 * 逻辑复制尚未使用通用逻辑消息。不过使用此输出插件的其他应用可能会用到。
			 */
			break;

		case LOGICAL_REP_MSG_STREAM_START:
			apply_handle_stream_start(s);
			break;

		case LOGICAL_REP_MSG_STREAM_STOP:
			apply_handle_stream_stop(s);
			break;

		case LOGICAL_REP_MSG_STREAM_ABORT:
			apply_handle_stream_abort(s);
			break;

		case LOGICAL_REP_MSG_STREAM_COMMIT:
			apply_handle_stream_commit(s);
			break;

		case LOGICAL_REP_MSG_BEGIN_PREPARE:
			apply_handle_begin_prepare(s);
			break;

		case LOGICAL_REP_MSG_PREPARE:
			apply_handle_prepare(s);
			break;

		case LOGICAL_REP_MSG_COMMIT_PREPARED:
			apply_handle_commit_prepared(s);
			break;

		case LOGICAL_REP_MSG_ROLLBACK_PREPARED:
			apply_handle_rollback_prepared(s);
			break;

		case LOGICAL_REP_MSG_STREAM_PREPARE:
			apply_handle_stream_prepare(s);
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid logical replication message type \"??? (%d)\"", action)));
	}

	/* Reset the current command
	 *
	 * 重置当前命令
	 */
	apply_error_callback_arg.command = saved_command;
}

/*
 * Figure out which write/flush positions to report to the walsender process.
 *
 * 确定要向 walsender 进程报告哪些 write/flush 位置。
 *
 * We can't simply report back the last LSN the walsender sent us because the
 * local transaction might not yet be flushed to disk locally. Instead we
 * build a list that associates local with remote LSNs for every commit. When
 * reporting back the flush position to the sender we iterate that list and
 * check which entries on it are already locally flushed. Those we can report
 * as having been flushed.
 *
 * 不能简单地把 walsender 发给我们的最后一个 LSN 报告回去，因为本地事务可能尚未刷到本地磁盘。
 * 因此我们建立一个列表，为每次 commit 关联本地 LSN 与远端 LSN。向发送端报告刷盘位置时，
 * 遍历该列表，检查哪些项已经在本地刷盘。这些就可以报告为已刷盘。
 *
 * The have_pending_txes is true if there are outstanding transactions that
 * need to be flushed.
 *
 * 若有尚未刷盘的未完成事务，则 have_pending_txes 为真。
 */
static void
get_flush_position(XLogRecPtr *write, XLogRecPtr *flush,
				   bool *have_pending_txes)
{
	dlist_mutable_iter iter;
	XLogRecPtr	local_flush = GetFlushRecPtr(NULL);

	*write = InvalidXLogRecPtr;
	*flush = InvalidXLogRecPtr;

	dlist_foreach_modify(iter, &lsn_mapping)
	{
		FlushPosition *pos =
			dlist_container(FlushPosition, node, iter.cur);

		*write = pos->remote_end;

		if (pos->local_end <= local_flush)
		{
			*flush = pos->remote_end;
			dlist_delete(iter.cur);
			pfree(pos);
		}
		else
		{
			/*
			 * Don't want to uselessly iterate over the rest of the list which
			 * could potentially be long. Instead get the last element and
			 * grab the write position from there.
			 *
			 * 不想无谓地遍历列表的剩余部分，它可能很长。改为取最后一个元素，
			 * 并从那里取得写入位置。
			 */
			pos = dlist_tail_element(FlushPosition, node,
									 &lsn_mapping);
			*write = pos->remote_end;
			*have_pending_txes = true;
			return;
		}
	}

	*have_pending_txes = !dlist_is_empty(&lsn_mapping);
}

/*
 * Store current remote/local lsn pair in the tracking list.
 *
 * 把当前的远端与本地 lsn 对存入跟踪列表。
 */
void
store_flush_position(XLogRecPtr remote_lsn, XLogRecPtr local_lsn)
{
	FlushPosition *flushpos;

	/*
	 * Skip for parallel apply workers, because the lsn_mapping is maintained
	 * by the leader apply worker.
	 *
	 * 并行 apply worker 跳过，因为 lsn_mapping 由 leader apply worker 维护。
	 */
	if (am_parallel_apply_worker())
		return;

	/* Need to do this in permanent context
	 *
	 * 需要在永久上下文中做这件事
	 */
	MemoryContextSwitchTo(ApplyContext);

	/* Track commit lsn 
	 *
	 * 跟踪提交 LSN
	 */
	flushpos = (FlushPosition *) palloc(sizeof(FlushPosition));
	flushpos->local_end = local_lsn;
	flushpos->remote_end = remote_lsn;

	dlist_push_tail(&lsn_mapping, &flushpos->node);
	MemoryContextSwitchTo(ApplyMessageContext);
}


/* Update statistics of the worker.
 *
 * 更新 worker 的统计信息。
 */
static void
UpdateWorkerStats(XLogRecPtr last_lsn, TimestampTz send_time, bool reply)
{
	MyLogicalRepWorker->last_lsn = last_lsn;
	MyLogicalRepWorker->last_send_time = send_time;
	MyLogicalRepWorker->last_recv_time = GetCurrentTimestamp();
	if (reply)
	{
		MyLogicalRepWorker->reply_lsn = last_lsn;
		MyLogicalRepWorker->reply_time = send_time;
	}
}

/*
 * Apply main loop.
 *
 * 应用主循环。
 */
static void
LogicalRepApplyLoop(XLogRecPtr last_received)
{
	TimestampTz last_recv_timestamp = GetCurrentTimestamp();
	bool		ping_sent = false;
	TimeLineID	tli;
	ErrorContextCallback errcallback;

	/*
	 * Init the ApplyMessageContext which we clean up after each replication
	 * protocol message.
	 *
	 * 初始化 ApplyMessageContext，每处理一条复制协议消息后清理它。
	 */
	ApplyMessageContext = AllocSetContextCreate(ApplyContext,
												"ApplyMessageContext",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * This memory context is used for per-stream data when the streaming mode
	 * is enabled. This context is reset on each stream stop.
	 *
	 * 启用流式模式时，此内存上下文用于每个流的数据。每次 stream stop 时重置该上下文。
	 */
	LogicalStreamingContext = AllocSetContextCreate(ApplyContext,
													"LogicalStreamingContext",
													ALLOCSET_DEFAULT_SIZES);

	/* mark as idle, before starting to loop
	 *
	 * 开始循环之前标记为空闲
	 */
	pgstat_report_activity(STATE_IDLE, NULL);

	/*
	 * Push apply error context callback. Fields will be filled while applying
	 * a change.
	 *
	 * 压入 apply 错误上下文回调。字段会在应用变更时填充。
	 */
	errcallback.callback = apply_error_callback;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;
	apply_error_context_stack = error_context_stack;

	/* This outer loop iterates once per wait.
	 *
	 * 外层循环每次等待迭代一次。
	 */
	for (;;)
	{
		pgsocket	fd = PGINVALID_SOCKET;
		int			rc;
		int			len;
		char	   *buf = NULL;
		bool		endofstream = false;
		long		wait_time;

		CHECK_FOR_INTERRUPTS();

		MemoryContextSwitchTo(ApplyMessageContext);

		len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);

		if (len != 0)
		{
			/* Loop to process all available data (without blocking).
			 *
			 * 循环处理所有已到达的数据，不阻塞。
			 */
			for (;;)
			{
				CHECK_FOR_INTERRUPTS();

				if (len == 0)
				{
					break;
				}
				else if (len < 0)
				{
					ereport(LOG,
							(errmsg("data stream from publisher has ended")));
					endofstream = true;
					break;
				}
				else
				{
					int			c;
					StringInfoData s;

					if (ConfigReloadPending)
					{
						ConfigReloadPending = false;
						ProcessConfigFile(PGC_SIGHUP);
					}

					/* Reset timeout.
					 *
					 * 重置超时。
					 */
					last_recv_timestamp = GetCurrentTimestamp();
					ping_sent = false;

					/* Ensure we are reading the data into our memory context.
					 *
					 * 确保把数据读入我们的内存上下文。
					 */
					MemoryContextSwitchTo(ApplyMessageContext);

					initReadOnlyStringInfo(&s, buf, len);

					c = pq_getmsgbyte(&s);

					if (c == 'w')
					{
						XLogRecPtr	start_lsn;
						XLogRecPtr	end_lsn;
						TimestampTz send_time;

						start_lsn = pq_getmsgint64(&s);
						end_lsn = pq_getmsgint64(&s);
						send_time = pq_getmsgint64(&s);

						if (last_received < start_lsn)
							last_received = start_lsn;

						if (last_received < end_lsn)
							last_received = end_lsn;

						UpdateWorkerStats(last_received, send_time, false);

						apply_dispatch(&s);
					}
					else if (c == 'k')
					{
						XLogRecPtr	end_lsn;
						TimestampTz timestamp;
						bool		reply_requested;

						end_lsn = pq_getmsgint64(&s);
						timestamp = pq_getmsgint64(&s);
						reply_requested = pq_getmsgbyte(&s);

						if (last_received < end_lsn)
							last_received = end_lsn;

						send_feedback(last_received, reply_requested, false);
						UpdateWorkerStats(last_received, timestamp, true);
					}
					/* other message types are purposefully ignored
					 *
					 * 其他消息类型被有意忽略
					 */

					MemoryContextReset(ApplyMessageContext);
				}

				len = walrcv_receive(LogRepWorkerWalRcvConn, &buf, &fd);
			}
		}

		/* confirm all writes so far
		 *
		 * 确认到目前为止的全部写入
		 */
		send_feedback(last_received, false, false);

		if (!in_remote_transaction && !in_streamed_transaction)
		{
			/*
			 * If we didn't get any transactions for a while there might be
			 * unconsumed invalidation messages in the queue, consume them
			 * now.
			 *
			 * 若有一段时间没有收到任何事务，队列里可能有未消费的失效消息，
			 * 现在把它们消费掉。
			 */
			AcceptInvalidationMessages();
			maybe_reread_subscription();

			/* Process any table synchronization changes.
			 *
			 * 处理表同步变更。
			 */
			process_syncing_tables(last_received);
		}

		/* Cleanup the memory.
		 *
		 * 清理内存。
		 */
		MemoryContextReset(ApplyMessageContext);
		MemoryContextSwitchTo(TopMemoryContext);

		/* Check if we need to exit the streaming loop.
		 *
		 * 检查是否需要退出流式循环。
		 */
		if (endofstream)
			break;

		/*
		 * Wait for more data or latch.  If we have unflushed transactions,
		 * wake up after WalWriterDelay to see if they've been flushed yet (in
		 * which case we should send a feedback message).  Otherwise, there's
		 * no particular urgency about waking up unless we get data or a
		 * signal.
		 *
		 * 等待更多数据或闩锁。若有未刷盘的事务，则在 WalWriterDelay 之后醒来，
		 * 看它们是否已经刷盘；若已刷盘则应发送反馈消息。否则，除非收到数据或信号，
		 * 没有特别理由急着醒来。
		 */
		if (!dlist_is_empty(&lsn_mapping))
			wait_time = WalWriterDelay;
		else
			wait_time = NAPTIME_PER_CYCLE;

		rc = WaitLatchOrSocket(MyLatch,
							   WL_SOCKET_READABLE | WL_LATCH_SET |
							   WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							   fd, wait_time,
							   WAIT_EVENT_LOGICAL_APPLY_MAIN);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (rc & WL_TIMEOUT)
		{
			/*
			 * We didn't receive anything new. If we haven't heard anything
			 * from the server for more than wal_receiver_timeout / 2, ping
			 * the server. Also, if it's been longer than
			 * wal_receiver_status_interval since the last update we sent,
			 * send a status update to the primary anyway, to report any
			 * progress in applying WAL.
			 *
			 * 没有收到新数据。若超过 wal_receiver_timeout / 2 没有从服务器收到任何消息，
			 * 则 ping 服务器。另外，若距离上次发送更新已超过 wal_receiver_status_interval，
			 * 也无论如何向主库发送状态更新，以报告应用 WAL 的进展。
			 */
			bool		requestReply = false;

			/*
			 * Check if time since last receive from primary has reached the
			 * configured limit.
			 *
			 * 检查自上次从主库收到数据以来的时间是否已达到配置的上限。
			 */
			if (wal_receiver_timeout > 0)
			{
				TimestampTz now = GetCurrentTimestamp();
				TimestampTz timeout;

				timeout =
					TimestampTzPlusMilliseconds(last_recv_timestamp,
												wal_receiver_timeout);

				if (now >= timeout)
					ereport(ERROR,
							(errcode(ERRCODE_CONNECTION_FAILURE),
							 errmsg("terminating logical replication worker due to timeout")));

				/* Check to see if it's time for a ping.
				 *
				 * 检查是否到了 ping 的时间。
				 */
				if (!ping_sent)
				{
					timeout = TimestampTzPlusMilliseconds(last_recv_timestamp,
														  (wal_receiver_timeout / 2));
					if (now >= timeout)
					{
						requestReply = true;
						ping_sent = true;
					}
				}
			}

			send_feedback(last_received, requestReply, requestReply);

			/*
			 * Force reporting to ensure long idle periods don't lead to
			 * arbitrarily delayed stats. Stats can only be reported outside
			 * of (implicit or explicit) transactions. That shouldn't lead to
			 * stats being delayed for long, because transactions are either
			 * sent as a whole on commit or streamed. Streamed transactions
			 * are spilled to disk and applied on commit.
			 *
			 * 强制上报，避免长时间空闲导致统计被任意推迟。统计只能在隐式或显式事务之外上报。
			 * 这不应导致统计被推迟太久，因为事务要么在 commit 时整批发送，
			 * 要么被流式传输。流式事务会溢写到磁盘，并在 commit 时应用。
			 */
			if (!IsTransactionState())
				pgstat_report_stat(true);
		}
	}

	/* Pop the error context stack
	 *
	 * 弹出错误上下文栈
	 */
	error_context_stack = errcallback.previous;
	apply_error_context_stack = error_context_stack;

	/* All done
	 *
	 * 全部完成
	 */
	walrcv_endstreaming(LogRepWorkerWalRcvConn, &tli);
}

/*
 * Send a Standby Status Update message to server.
 *
 * 向服务器发送 Standby Status Update 消息。
 *
 * 'recvpos' is the latest LSN we've received data to, force is set if we need
 * to send a response to avoid timeouts.
 *
 * recvpos 是我们已收到数据的最新 LSN；若需要发送响应以避免超时，则设置 force。
 */
static void
send_feedback(XLogRecPtr recvpos, bool force, bool requestReply)
{
	static StringInfo reply_message = NULL;
	static TimestampTz send_time = 0;

	static XLogRecPtr last_recvpos = InvalidXLogRecPtr;
	static XLogRecPtr last_writepos = InvalidXLogRecPtr;
	static XLogRecPtr last_flushpos = InvalidXLogRecPtr;

	XLogRecPtr	writepos;
	XLogRecPtr	flushpos;
	TimestampTz now;
	bool		have_pending_txes;

	/*
	 * If the user doesn't want status to be reported to the publisher, be
	 * sure to exit before doing anything at all.
	 *
	 * 若用户不希望向发布端报告状态，务必在做任何事情之前退出。
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* It's legal to not pass a recvpos
	 *
	 * 不传入 recvpos 是合法的
	 */
	if (recvpos < last_recvpos)
		recvpos = last_recvpos;

	get_flush_position(&writepos, &flushpos, &have_pending_txes);

	/*
	 * No outstanding transactions to flush, we can report the latest received
	 * position. This is important for synchronous replication.
	 *
	 * 没有待刷盘的未完成事务时，可以报告最新收到的位置。这对同步复制很重要。
	 */
	if (!have_pending_txes)
		flushpos = writepos = recvpos;

	if (writepos < last_writepos)
		writepos = last_writepos;

	if (flushpos < last_flushpos)
		flushpos = last_flushpos;

	now = GetCurrentTimestamp();

	/* if we've already reported everything we're good
	 *
	 * 若已经报告了全部内容，则无需再做
	 */
	if (!force &&
		writepos == last_writepos &&
		flushpos == last_flushpos &&
		!TimestampDifferenceExceeds(send_time, now,
									wal_receiver_status_interval * 1000))
		return;
	send_time = now;

	if (!reply_message)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(ApplyContext);

		reply_message = makeStringInfo();
		MemoryContextSwitchTo(oldctx);
	}
	else
		resetStringInfo(reply_message);

	pq_sendbyte(reply_message, 'r');
	pq_sendint64(reply_message, recvpos);	/* write
						 *
						 * 写入位置
						 */
	pq_sendint64(reply_message, flushpos);	/* flush
						 *
						 * 刷盘位置
						 */
	pq_sendint64(reply_message, writepos);	/* apply
						 *
						 * 应用位置
						 */
	pq_sendint64(reply_message, now);	/* sendTime
						 *
						 * 发送时间
						 */
	pq_sendbyte(reply_message, requestReply);	/* replyRequested
							 *
							 * 是否请求回复
							 */

	elog(DEBUG2, "sending feedback (force %d) to recv %X/%X, write %X/%X, flush %X/%X",
		 force,
		 LSN_FORMAT_ARGS(recvpos),
		 LSN_FORMAT_ARGS(writepos),
		 LSN_FORMAT_ARGS(flushpos));

	walrcv_send(LogRepWorkerWalRcvConn,
				reply_message->data, reply_message->len);

	if (recvpos > last_recvpos)
		last_recvpos = recvpos;
	if (writepos > last_writepos)
		last_writepos = writepos;
	if (flushpos > last_flushpos)
		last_flushpos = flushpos;
}

/*
 * Exit routine for apply workers due to subscription parameter changes.
 *
 * 因订阅参数变化而让 apply worker 退出的例程。
 */
static void
apply_worker_exit(void)
{
	if (am_parallel_apply_worker())
	{
		/*
		 * Don't stop the parallel apply worker as the leader will detect the
		 * subscription parameter change and restart logical replication later
		 * anyway. This also prevents the leader from reporting errors when
		 * trying to communicate with a stopped parallel apply worker, which
		 * would accidentally disable subscriptions if disable_on_error was
		 * set.
		 *
		 * 不要停止并行 apply worker，因为 leader 会检测到订阅参数变化并稍后重启逻辑复制。
		 * 这也避免 leader 在试图与已停止的并行 apply worker 通信时报告错误，
		 * 否则若设置了 disable_on_error，会意外禁用订阅。
		 */
		return;
	}

	/*
	 * Reset the last-start time for this apply worker so that the launcher
	 * will restart it without waiting for wal_retrieve_retry_interval if the
	 * subscription is still active, and so that we won't leak that hash table
	 * entry if it isn't.
	 *
	 * 重置此 apply worker 的上次启动时间，这样若订阅仍处于活动状态，launcher
	 * 会重启它而不等待 wal_retrieve_retry_interval；若订阅不再活动，也不会泄漏该哈希表项。
	 */
	if (am_leader_apply_worker())
		ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

	proc_exit(0);
}

/*
 * Reread subscription info if needed.
 *
 * 必要时重新读取订阅信息。
 *
 * For significant changes, we react by exiting the current process; a new
 * one will be launched afterwards if needed.
 *
 * 对于重大变化，我们通过退出当前进程来响应；之后如有需要会启动新进程。
 */
void
maybe_reread_subscription(void)
{
	MemoryContext oldctx;
	Subscription *newsub;
	bool		started_tx = false;

	/* When cache state is valid there is nothing to do here.
	 *
	 * 缓存状态有效时，这里无事可做。
	 */
	if (MySubscriptionValid)
		return;

	/* This function might be called inside or outside of transaction.
	 *
	 * 本函数可能在事务内或事务外被调用。
	 */
	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/* Ensure allocations in permanent context.
	 *
	 * 确保在永久上下文中分配。
	 */
	oldctx = MemoryContextSwitchTo(ApplyContext);

	newsub = GetSubscription(MyLogicalRepWorker->subid, true);

	/*
	 * Exit if the subscription was removed. This normally should not happen
	 * as the worker gets killed during DROP SUBSCRIPTION.
	 *
	 * 若订阅已被删除则退出。正常不应发生，因为 DROP SUBSCRIPTION 期间 worker
	 * 会被杀掉。
	 */
	if (!newsub)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription \"%s\" will stop because the subscription was removed",
						MySubscription->name)));

		/* Ensure we remove no-longer-useful entry for worker's start time
		 *
		 * 确保移除对 worker 启动时间不再有用的条目
		 */
		if (am_leader_apply_worker())
			ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

		proc_exit(0);
	}

	/* Exit if the subscription was disabled.
	 *
	 * 若订阅已被禁用则退出。
	 */
	if (!newsub->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription \"%s\" will stop because the subscription was disabled",
						MySubscription->name)));

		apply_worker_exit();
	}

	/* !slotname should never happen when enabled is true.
	 *
	 * enabled 为真时 slotname 绝不应为空。
	 */
	Assert(newsub->slotname);

	/* two-phase cannot be altered while the worker is running
	 *
	 * worker 运行期间不能修改两阶段选项
	 */
	Assert(newsub->twophasestate == MySubscription->twophasestate);

	/*
	 * Exit if any parameter that affects the remote connection was changed.
	 * The launcher will start a new worker but note that the parallel apply
	 * worker won't restart if the streaming option's value is changed from
	 * 'parallel' to any other value or the server decides not to stream the
	 * in-progress transaction.
	 *
	 * 若任何影响远端连接的参数发生变化则退出。launcher 会启动新的 worker，
	 * 但请注意：若 streaming 选项的值从 parallel 改为其他值，或者服务器决定不再流式传输进行中的事务，
	 * 并行 apply worker 不会重启。
	 */
	if (strcmp(newsub->conninfo, MySubscription->conninfo) != 0 ||
		strcmp(newsub->name, MySubscription->name) != 0 ||
		strcmp(newsub->slotname, MySubscription->slotname) != 0 ||
		newsub->binary != MySubscription->binary ||
		newsub->stream != MySubscription->stream ||
		newsub->passwordrequired != MySubscription->passwordrequired ||
		strcmp(newsub->origin, MySubscription->origin) != 0 ||
		newsub->owner != MySubscription->owner ||
		!equal(newsub->publications, MySubscription->publications))
	{
		if (am_parallel_apply_worker())
			ereport(LOG,
					(errmsg("logical replication parallel apply worker for subscription \"%s\" will stop because of a parameter change",
							MySubscription->name)));
		else
			ereport(LOG,
					(errmsg("logical replication worker for subscription \"%s\" will restart because of a parameter change",
							MySubscription->name)));

		apply_worker_exit();
	}

	/*
	 * Exit if the subscription owner's superuser privileges have been
	 * revoked.
	 *
	 * 若订阅所有者的超级用户权限被撤销则退出。
	 */
	if (!newsub->ownersuperuser && MySubscription->ownersuperuser)
	{
		if (am_parallel_apply_worker())
			ereport(LOG,
					errmsg("logical replication parallel apply worker for subscription \"%s\" will stop because the subscription owner's superuser privileges have been revoked",
						   MySubscription->name));
		else
			ereport(LOG,
					errmsg("logical replication worker for subscription \"%s\" will restart because the subscription owner's superuser privileges have been revoked",
						   MySubscription->name));

		apply_worker_exit();
	}

	/* Check for other changes that should never happen too.
	 *
	 * 也检查其他绝不应该发生的变化。
	 */
	if (newsub->dbid != MySubscription->dbid)
	{
		elog(ERROR, "subscription %u changed unexpectedly",
			 MyLogicalRepWorker->subid);
	}

	/* Clean old subscription info and switch to new one.
	 *
	 * 清理旧的订阅信息并切换到新的。
	 */
	FreeSubscription(MySubscription);
	MySubscription = newsub;

	MemoryContextSwitchTo(oldctx);

	/* Change synchronous commit according to the user's wishes
	 *
	 * 按用户意愿更改 synchronous commit
	 */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	if (started_tx)
		CommitTransactionCommand();

	MySubscriptionValid = true;
}

/*
 * Callback from subscription syscache invalidation.
 *
 * 订阅系统缓存失效时的回调。
 */
static void
subscription_change_cb(Datum arg, int cacheid, uint32 hashvalue)
{
	MySubscriptionValid = false;
}

/*
 * subxact_info_write
 *	  Store information about subxacts for a toplevel transaction.
 *
 * subxact_info_write：保存某个顶层事务的子事务信息。
 *
 * For each subxact we store offset of its first change in the main file.
 * The file is always over-written as a whole.
 *
 * 对每个子事务，保存它在主文件中第一次变更的偏移。该文件总是被整体覆盖写入。
 *
 * XXX We should only store subxacts that were not aborted yet.
 *
 * XXX：应当只保存尚未中止的子事务。
 */
static void
subxact_info_write(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	Size		len;
	BufFile    *fd;

	Assert(TransactionIdIsValid(xid));

	/* construct the subxact filename
	 *
	 * 构造子事务文件名
	 */
	subxact_filename(path, subid, xid);

	/* Delete the subxacts file, if exists.
	 *
	 * 若子事务文件存在则删除。
	 */
	if (subxact_data.nsubxacts == 0)
	{
		cleanup_subxact_info();
		BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, true);

		return;
	}

	/*
	 * Create the subxact file if it not already created, otherwise open the
	 * existing file.
	 *
	 * 若子事务文件尚未创建则创建，否则打开已有文件。
	 */
	fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path, O_RDWR,
							true);
	if (fd == NULL)
		fd = BufFileCreateFileSet(MyLogicalRepWorker->stream_fileset, path);

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* Write the subxact count and subxact info
	 *
	 * 写入子事务数量和子事务信息
	 */
	BufFileWrite(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));
	BufFileWrite(fd, subxact_data.subxacts, len);

	BufFileClose(fd);

	/* free the memory allocated for subxact info
	 *
	 * 释放为子事务信息分配的内存
	 */
	cleanup_subxact_info();
}

/*
 * subxact_info_read
 *	  Restore information about subxacts of a streamed transaction.
 *
 * subxact_info_read：恢复流式事务的子事务信息。
 *
 * Read information about subxacts into the structure subxact_data that can be
 * used later.
 *
 * 把子事务信息读入 subxact_data 结构，供以后使用。
 */
static void
subxact_info_read(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];
	Size		len;
	BufFile    *fd;
	MemoryContext oldctx;

	Assert(!subxact_data.subxacts);
	Assert(subxact_data.nsubxacts == 0);
	Assert(subxact_data.nsubxacts_max == 0);

	/*
	 * If the subxact file doesn't exist that means we don't have any subxact
	 * info.
	 *
	 * 若子事务文件不存在，表示没有任何子事务信息。
	 */
	subxact_filename(path, subid, xid);
	fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset, path, O_RDONLY,
							true);
	if (fd == NULL)
		return;

	/* read number of subxact items
	 *
	 * 读取子事务项的数量
	 */
	BufFileReadExact(fd, &subxact_data.nsubxacts, sizeof(subxact_data.nsubxacts));

	len = sizeof(SubXactInfo) * subxact_data.nsubxacts;

	/* we keep the maximum as a power of 2
	 *
	 * 我们把最大容量保持为 2 的幂
	 */
	subxact_data.nsubxacts_max = 1 << my_log2(subxact_data.nsubxacts);

	/*
	 * Allocate subxact information in the logical streaming context. We need
	 * this information during the complete stream so that we can add the sub
	 * transaction info to this. On stream stop we will flush this information
	 * to the subxact file and reset the logical streaming context.
	 *
	 * 在逻辑流式上下文中分配子事务信息。整个流期间都需要这些信息，以便把子事务信息追加进去。
	 * stream stop 时把这些信息刷到子事务文件，并重置逻辑流式上下文。
	 */
	oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
	subxact_data.subxacts = palloc(subxact_data.nsubxacts_max *
								   sizeof(SubXactInfo));
	MemoryContextSwitchTo(oldctx);

	if (len > 0)
		BufFileReadExact(fd, subxact_data.subxacts, len);

	BufFileClose(fd);
}

/*
 * subxact_info_add
 *	  Add information about a subxact (offset in the main file).
 *
 * subxact_info_add：添加一条子事务信息，即它在主文件中的偏移。
 */
static void
subxact_info_add(TransactionId xid)
{
	SubXactInfo *subxacts = subxact_data.subxacts;
	int64		i;

	/* We must have a valid top level stream xid and a stream fd.
	 *
	 * 必须有有效的顶层流 xid 和流文件描述符。
	 */
	Assert(TransactionIdIsValid(stream_xid));
	Assert(stream_fd != NULL);

	/*
	 * If the XID matches the toplevel transaction, we don't want to add it.
	 *
	 * 若该 XID 与顶层事务相同，则不添加它。
	 */
	if (stream_xid == xid)
		return;

	/*
	 * In most cases we're checking the same subxact as we've already seen in
	 * the last call, so make sure to ignore it (this change comes later).
	 *
	 * 多数情况下我们检查的是上次调用已经见过的同一个子事务，因此要忽略它，
	 * 这次变更会稍后到来。
	 */
	if (subxact_data.subxact_last == xid)
		return;

	/* OK, remember we're processing this XID.
	 *
	 * 记住当前正在处理这个 XID。
	 */
	subxact_data.subxact_last = xid;

	/*
	 * Check if the transaction is already present in the array of subxact. We
	 * intentionally scan the array from the tail, because we're likely adding
	 * a change for the most recent subtransactions.
	 *
	 * 检查该事务是否已在子事务数组中。我们有意从数组尾部扫描，因为多半是在为最近的子事务添加变更。
	 *
	 * XXX Can we rely on the subxact XIDs arriving in sorted order? That
	 * would allow us to use binary search here.
	 *
	 * XXX：能否依赖子事务 XID 按序到达？若可以，这里就能使用二分查找。
	 */
	for (i = subxact_data.nsubxacts; i > 0; i--)
	{
		/* found, so we're done
		 *
		 * 已找到，因此结束
		 */
		if (subxacts[i - 1].xid == xid)
			return;
	}

	/* This is a new subxact, so we need to add it to the array.
	 *
	 * 这是新的子事务，需要把它加入数组。
	 */
	if (subxact_data.nsubxacts == 0)
	{
		MemoryContext oldctx;

		subxact_data.nsubxacts_max = 128;

		/*
		 * Allocate this memory for subxacts in per-stream context, see
		 * subxact_info_read.
		 *
		 * 在每个流的上下文中为子事务分配这块内存，见 subxact_info_read。
		 */
		oldctx = MemoryContextSwitchTo(LogicalStreamingContext);
		subxacts = palloc(subxact_data.nsubxacts_max * sizeof(SubXactInfo));
		MemoryContextSwitchTo(oldctx);
	}
	else if (subxact_data.nsubxacts == subxact_data.nsubxacts_max)
	{
		subxact_data.nsubxacts_max *= 2;
		subxacts = repalloc(subxacts,
							subxact_data.nsubxacts_max * sizeof(SubXactInfo));
	}

	subxacts[subxact_data.nsubxacts].xid = xid;

	/*
	 * Get the current offset of the stream file and store it as offset of
	 * this subxact.
	 *
	 * 取得流文件的当前偏移，并把它存为该子事务的偏移。
	 */
	BufFileTell(stream_fd,
				&subxacts[subxact_data.nsubxacts].fileno,
				&subxacts[subxact_data.nsubxacts].offset);

	subxact_data.nsubxacts++;
	subxact_data.subxacts = subxacts;
}

/* format filename for file containing the info about subxacts
 *
 * 格式化保存子事务信息的文件名
 */
static inline void
subxact_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.subxacts", subid, xid);
}

/* format filename for file containing serialized changes
 *
 * 格式化保存序列化变更的文件名
 */
static inline void
changes_filename(char *path, Oid subid, TransactionId xid)
{
	snprintf(path, MAXPGPATH, "%u-%u.changes", subid, xid);
}

/*
 * stream_cleanup_files
 *	  Cleanup files for a subscription / toplevel transaction.
 *
 * stream_cleanup_files：清理某个订阅或顶层事务的文件。
 *
 * Remove files with serialized changes and subxact info for a particular
 * toplevel transaction. Each subscription has a separate set of files
 * for any toplevel transaction.
 *
 * 删除特定顶层事务的序列化变更文件和子事务信息文件。每个订阅对任何顶层事务都有一套独立的文件。
 */
void
stream_cleanup_files(Oid subid, TransactionId xid)
{
	char		path[MAXPGPATH];

	/* Delete the changes file.
	 *
	 * 删除变更文件。
	 */
	changes_filename(path, subid, xid);
	BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, false);

	/* Delete the subxact file, if it exists.
	 *
	 * 若子事务文件存在则删除。
	 */
	subxact_filename(path, subid, xid);
	BufFileDeleteFileSet(MyLogicalRepWorker->stream_fileset, path, true);
}

/*
 * stream_open_file
 *	  Open a file that we'll use to serialize changes for a toplevel
 * transaction.
 *
 * stream_open_file：打开用于序列化顶层事务变更的文件。
 *
 * Open a file for streamed changes from a toplevel transaction identified
 * by stream_xid (global variable). If it's the first chunk of streamed
 * changes for this transaction, create the buffile, otherwise open the
 * previously created file.
 *
 * 为 stream_xid（全局变量）所标识的顶层事务打开流式变更文件。若这是该事务流式变更的第一块，
 * 则创建 buffile，否则打开先前创建的文件。
 */
static void
stream_open_file(Oid subid, TransactionId xid, bool first_segment)
{
	char		path[MAXPGPATH];
	MemoryContext oldcxt;

	Assert(OidIsValid(subid));
	Assert(TransactionIdIsValid(xid));
	Assert(stream_fd == NULL);


	changes_filename(path, subid, xid);
	elog(DEBUG1, "opening file \"%s\" for streamed changes", path);

	/*
	 * Create/open the buffiles under the logical streaming context so that we
	 * have those files until stream stop.
	 *
	 * 在逻辑流式上下文中创建或打开 buffile，以便这些文件保留到 stream stop。
	 */
	oldcxt = MemoryContextSwitchTo(LogicalStreamingContext);

	/*
	 * If this is the first streamed segment, create the changes file.
	 * Otherwise, just open the file for writing, in append mode.
	 *
	 * 若这是第一个流式段，则创建变更文件。否则只以追加模式打开文件用于写入。
	 */
	if (first_segment)
		stream_fd = BufFileCreateFileSet(MyLogicalRepWorker->stream_fileset,
										 path);
	else
	{
		/*
		 * Open the file and seek to the end of the file because we always
		 * append the changes file.
		 *
		 * 打开文件并寻位到文件末尾，因为我们总是追加变更文件。
		 */
		stream_fd = BufFileOpenFileSet(MyLogicalRepWorker->stream_fileset,
									   path, O_RDWR, false);
		BufFileSeek(stream_fd, 0, 0, SEEK_END);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * stream_close_file
 *	  Close the currently open file with streamed changes.
 *
 * stream_close_file：关闭当前打开的流式变更文件。
 */
static void
stream_close_file(void)
{
	Assert(stream_fd != NULL);

	BufFileClose(stream_fd);

	stream_fd = NULL;
}

/*
 * stream_write_change
 *	  Serialize a change to a file for the current toplevel transaction.
 *
 * stream_write_change：把一条变更序列化到当前顶层事务的文件。
 *
 * The change is serialized in a simple format, with length (not including
 * the length), action code (identifying the message type) and message
 * contents (without the subxact TransactionId value).
 *
 * 变更以简单格式序列化：长度（不包括长度本身）、动作码（标识消息类型）以及消息内容（不含子事务的
 * TransactionId 值）。
 */
static void
stream_write_change(char action, StringInfo s)
{
	int			len;

	Assert(stream_fd != NULL);

	/* total on-disk size, including the action type character
	 *
	 * 磁盘上的总大小，包括动作类型字符
	 */
	len = (s->len - s->cursor) + sizeof(char);

	/* first write the size
	 *
	 * 先写入大小
	 */
	BufFileWrite(stream_fd, &len, sizeof(len));

	/* then the action
	 *
	 * 然后写入动作
	 */
	BufFileWrite(stream_fd, &action, sizeof(action));

	/* and finally the remaining part of the buffer (after the XID)
	 *
	 * 最后写入缓冲区剩余部分（XID 之后）
	 */
	len = (s->len - s->cursor);

	BufFileWrite(stream_fd, &s->data[s->cursor], len);
}

/*
 * stream_open_and_write_change
 *	  Serialize a message to a file for the given transaction.
 *
 * stream_open_and_write_change：把一条消息序列化到给定事务的文件。
 *
 * This function is similar to stream_write_change except that it will open the
 * target file if not already before writing the message and close the file at
 * the end.
 *
 * 本函数与 stream_write_change 类似，区别是若目标文件尚未打开，会在写消息之前打开，
 * 并在结束时关闭文件。
 */
static void
stream_open_and_write_change(TransactionId xid, char action, StringInfo s)
{
	Assert(!in_streamed_transaction);

	if (!stream_fd)
		stream_start_internal(xid, false);

	stream_write_change(action, s);
	stream_stop_internal(xid);
}

/*
 * Sets streaming options including replication slot name and origin start
 * position. Workers need these options for logical replication.
 *
 * 设置流式选项，包括复制槽名称和 origin 起始位置。worker 进行逻辑复制时需要这些选项。
 */
void
set_stream_options(WalRcvStreamOptions *options,
				   char *slotname,
				   XLogRecPtr *origin_startpos)
{
	int			server_version;

	options->logical = true;
	options->startpoint = *origin_startpos;
	options->slotname = slotname;

	server_version = walrcv_server_version(LogRepWorkerWalRcvConn);
	options->proto.logical.proto_version =
		server_version >= 160000 ? LOGICALREP_PROTO_STREAM_PARALLEL_VERSION_NUM :
		server_version >= 150000 ? LOGICALREP_PROTO_TWOPHASE_VERSION_NUM :
		server_version >= 140000 ? LOGICALREP_PROTO_STREAM_VERSION_NUM :
		LOGICALREP_PROTO_VERSION_NUM;

	options->proto.logical.publication_names = MySubscription->publications;
	options->proto.logical.binary = MySubscription->binary;

	/*
	 * Assign the appropriate option value for streaming option according to
	 * the 'streaming' mode and the publisher's ability to support that mode.
	 *
	 * 根据 streaming 模式以及发布端是否支持该模式，为 streaming 选项赋予适当的值。
	 */
	if (server_version >= 160000 &&
		MySubscription->stream == LOGICALREP_STREAM_PARALLEL)
	{
		options->proto.logical.streaming_str = "parallel";
		MyLogicalRepWorker->parallel_apply = true;
	}
	else if (server_version >= 140000 &&
			 MySubscription->stream != LOGICALREP_STREAM_OFF)
	{
		options->proto.logical.streaming_str = "on";
		MyLogicalRepWorker->parallel_apply = false;
	}
	else
	{
		options->proto.logical.streaming_str = NULL;
		MyLogicalRepWorker->parallel_apply = false;
	}

	options->proto.logical.twophase = false;
	options->proto.logical.origin = pstrdup(MySubscription->origin);
}

/*
 * Cleanup the memory for subxacts and reset the related variables.
 *
 * 清理子事务占用的内存并重置相关变量。
 */
static inline void
cleanup_subxact_info()
{
	if (subxact_data.subxacts)
		pfree(subxact_data.subxacts);

	subxact_data.subxacts = NULL;
	subxact_data.subxact_last = InvalidTransactionId;
	subxact_data.nsubxacts = 0;
	subxact_data.nsubxacts_max = 0;
}

/*
 * Common function to run the apply loop with error handling. Disable the
 * subscription, if necessary.
 *
 * 带错误处理地运行 apply 循环的公共函数。必要时禁用订阅。
 *
 * Note that we don't handle FATAL errors which are probably because
 * of system resource error and are not repeatable.
 *
 * 注意我们不处理 FATAL 错误，它们多半是系统资源错误且不可重复。
 */
void
start_apply(XLogRecPtr origin_startpos)
{
	PG_TRY();
	{
		LogicalRepApplyLoop(origin_startpos);
	}
	PG_CATCH();
	{
		/*
		 * Reset the origin state to prevent the advancement of origin
		 * progress if we fail to apply. Otherwise, this will result in
		 * transaction loss as that transaction won't be sent again by the
		 * server.
		 *
		 * 重置 origin 状态，以防止应用失败时 origin 进度前移。否则会造成事务丢失，
		 * 因为服务器不会再次发送该事务。
		 */
		replorigin_reset(0, (Datum) 0);

		if (MySubscription->disableonerr)
			DisableSubscriptionAndExit();
		else
		{
			/*
			 * Report the worker failed while applying changes. Abort the
			 * current transaction so that the stats message is sent in an
			 * idle state.
			 *
			 * 报告 worker 在应用变更时失败。中止当前事务，以便统计消息在空闲状态下发送。
			 */
			AbortOutOfAnyTransaction();
			pgstat_report_subscription_error(MySubscription->oid, !am_tablesync_worker());

			PG_RE_THROW();
		}
	}
	PG_END_TRY();
}

/*
 * Runs the leader apply worker.
 *
 * 运行 leader apply worker。
 *
 * It sets up replication origin, streaming options and then starts streaming.
 *
 * 它设置复制 origin、流式选项，然后开始流式传输。
 */
static void
run_apply_worker()
{
	char		originname[NAMEDATALEN];
	XLogRecPtr	origin_startpos = InvalidXLogRecPtr;
	char	   *slotname = NULL;
	WalRcvStreamOptions options;
	RepOriginId originid;
	TimeLineID	startpointTLI;
	char	   *err;
	bool		must_use_password;

	slotname = MySubscription->slotname;

	/*
	 * This shouldn't happen if the subscription is enabled, but guard against
	 * DDL bugs or manual catalog changes.  (libpqwalreceiver will crash if
	 * slot is NULL.)
	 *
	 * 订阅已启用时不应发生这种情况，但仍要防范 DDL 缺陷或手工修改目录。若
	 * slot 为 NULL，libpqwalreceiver 会崩溃。
	 */
	if (!slotname)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("subscription has no replication slot set")));

	/* Setup replication origin tracking.
	 *
	 * 设置复制 origin 跟踪。
	 */
	ReplicationOriginNameForLogicalRep(MySubscription->oid, InvalidOid,
									   originname, sizeof(originname));
	StartTransactionCommand();
	originid = replorigin_by_name(originname, true);
	if (!OidIsValid(originid))
		originid = replorigin_create(originname);
	replorigin_session_setup(originid, 0);
	replorigin_session_origin = originid;
	origin_startpos = replorigin_session_get_progress(false);
	CommitTransactionCommand();

	/* Is the use of a password mandatory?
	 *
	 * 是否强制使用密码？
	 */
	must_use_password = MySubscription->passwordrequired &&
		!MySubscription->ownersuperuser;

	LogRepWorkerWalRcvConn = walrcv_connect(MySubscription->conninfo, true,
											true, must_use_password,
											MySubscription->name, &err);

	if (LogRepWorkerWalRcvConn == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("apply worker for subscription \"%s\" could not connect to the publisher: %s",
						MySubscription->name, err)));

	/*
	 * We don't really use the output identify_system for anything but it does
	 * some initializations on the upstream so let's still call it.
	 *
	 * 我们并不真正使用 identify_system 的输出，但它会在上游做一些初始化，
	 * 所以仍然调用它。
	 */
	(void) walrcv_identify_system(LogRepWorkerWalRcvConn, &startpointTLI);

	set_apply_error_context_origin(originname);

	set_stream_options(&options, slotname, &origin_startpos);

	/*
	 * Even when the two_phase mode is requested by the user, it remains as
	 * the tri-state PENDING until all tablesyncs have reached READY state.
	 * Only then, can it become ENABLED.
	 *
	 * 即使用户请求了 two_phase 模式，在所有 tablesync 达到 READY 之前，它仍保持三态
	 * PENDING。只有那时才能变为 ENABLED。
	 *
	 * Note: If the subscription has no tables then leave the state as
	 * PENDING, which allows ALTER SUBSCRIPTION ... REFRESH PUBLICATION to
	 * work.
	 *
	 * 注意：若订阅没有表，则保持 PENDING 状态，以便 ALTER SUBSCRIPTION ...
	 * REFRESH PUBLICATION 能够工作。
	 */
	if (MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING &&
		AllTablesyncsReady())
	{
		/* Start streaming with two_phase enabled
		 *
		 * 以启用 two_phase 的方式开始流式传输
		 */
		options.proto.logical.twophase = true;
		walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);

		StartTransactionCommand();

		/*
		 * Updating pg_subscription might involve TOAST table access, so
		 * ensure we have a valid snapshot.
		 *
		 * 更新 pg_subscription 可能访问 TOAST 表，因此确保已有有效快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		UpdateTwoPhaseState(MySubscription->oid, LOGICALREP_TWOPHASE_STATE_ENABLED);
		MySubscription->twophasestate = LOGICALREP_TWOPHASE_STATE_ENABLED;
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	else
	{
		walrcv_startstreaming(LogRepWorkerWalRcvConn, &options);
	}

	ereport(DEBUG1,
			(errmsg_internal("logical replication apply worker for subscription \"%s\" two_phase is %s",
							 MySubscription->name,
							 MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_DISABLED ? "DISABLED" :
							 MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_PENDING ? "PENDING" :
							 MySubscription->twophasestate == LOGICALREP_TWOPHASE_STATE_ENABLED ? "ENABLED" :
							 "?")));

	/* Run the main loop.
	 *
	 * 运行主循环。
	 */
	start_apply(origin_startpos);
}

/*
 * Common initialization for leader apply worker, parallel apply worker and
 * tablesync worker.
 *
 * leader apply worker、并行 apply worker 和 tablesync worker 的公共初始化。
 *
 * Initialize the database connection, in-memory subscription and necessary
 * config options.
 *
 * 初始化数据库连接、内存中的订阅以及必要的配置选项。
 */
void
InitializeLogRepWorker(void)
{
	MemoryContext oldctx;

	/* Run as replica session replication role.
	 *
	 * 以 replica 会话复制角色运行。
	 */
	SetConfigOption("session_replication_role", "replica",
					PGC_SUSET, PGC_S_OVERRIDE);

	/* Connect to our database.
	 *
	 * 连接到我们的数据库。
	 */
	BackgroundWorkerInitializeConnectionByOid(MyLogicalRepWorker->dbid,
											  MyLogicalRepWorker->userid,
											  0);

	/*
	 * Set always-secure search path, so malicious users can't redirect user
	 * code (e.g. pg_index.indexprs).
	 *
	 * 设置始终安全的 search_path，防止恶意用户重定向用户代码，例如 pg_index.indexprs。
	 */
	SetConfigOption("search_path", "", PGC_SUSET, PGC_S_OVERRIDE);

	/* Load the subscription into persistent memory context.
	 *
	 * 把订阅加载到持久内存上下文。
	 */
	ApplyContext = AllocSetContextCreate(TopMemoryContext,
										 "ApplyContext",
										 ALLOCSET_DEFAULT_SIZES);
	StartTransactionCommand();
	oldctx = MemoryContextSwitchTo(ApplyContext);

	/*
	 * Lock the subscription to prevent it from being concurrently dropped,
	 * then re-verify its existence. After the initialization, the worker will
	 * be terminated gracefully if the subscription is dropped.
	 *
	 * 锁定订阅以防止它被并发删除，然后再次确认它存在。初始化之后，若订阅被删除，
	 * worker 会被优雅终止。
	 */
	LockSharedObject(SubscriptionRelationId, MyLogicalRepWorker->subid, 0,
					 AccessShareLock);
	MySubscription = GetSubscription(MyLogicalRepWorker->subid, true);
	if (!MySubscription)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription %u will not start because the subscription was removed during startup",
						MyLogicalRepWorker->subid)));

		/* Ensure we remove no-longer-useful entry for worker's start time
		 *
		 * 确保移除对 worker 启动时间不再有用的条目
		 */
		if (am_leader_apply_worker())
			ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

		proc_exit(0);
	}

	MySubscriptionValid = true;
	MemoryContextSwitchTo(oldctx);

	if (!MySubscription->enabled)
	{
		ereport(LOG,
				(errmsg("logical replication worker for subscription \"%s\" will not start because the subscription was disabled during startup",
						MySubscription->name)));

		apply_worker_exit();
	}

	/* Setup synchronous commit according to the user's wishes
	 *
	 * 按用户意愿设置 synchronous commit
	 */
	SetConfigOption("synchronous_commit", MySubscription->synccommit,
					PGC_BACKEND, PGC_S_OVERRIDE);

	/*
	 * Keep us informed about subscription or role changes. Note that the
	 * role's superuser privilege can be revoked.
	 *
	 * 持续关注订阅或角色的变化。注意角色的超级用户权限可能被撤销。
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONOID,
								  subscription_change_cb,
								  (Datum) 0);

	CacheRegisterSyscacheCallback(AUTHOID,
								  subscription_change_cb,
								  (Datum) 0);

	if (am_tablesync_worker())
		ereport(LOG,
				(errmsg("logical replication table synchronization worker for subscription \"%s\", table \"%s\" has started",
						MySubscription->name,
						get_rel_name(MyLogicalRepWorker->relid))));
	else
		ereport(LOG,
				(errmsg("logical replication apply worker for subscription \"%s\" has started",
						MySubscription->name)));

	CommitTransactionCommand();

	/*
	 * Register a callback to reset the origin state before aborting any
	 * pending transaction during shutdown (see ShutdownPostgres()). This will
	 * avoid origin advancement for an incomplete transaction which could
	 * otherwise lead to its loss as such a transaction won't be sent by the
	 * server again.
	 *
	 * 注册一个回调，在关闭期间中止任何未完成事务之前重置 origin 状态（见
	 * ShutdownPostgres()）。这样可避免未完成事务的 origin 前移，否则该事务会丢失，
	 * 因为服务器不会再次发送它。
	 *
	 * Note that even a LOG or DEBUG statement placed after setting the origin
	 * state may process a shutdown signal before committing the current apply
	 * operation. So, it is important to register such a callback here.
	 *
	 * 注意，即使在设置 origin 状态之后放一条 LOG 或 DEBUG 语句，也可能在提交当前
	 * apply 操作之前处理关闭信号。因此在这里注册该回调很重要。
	 *
	 * Register this callback here to ensure that all types of logical
	 * replication workers that set up origins and apply remote transactions
	 * are protected.
	 *
	 * 在这里注册该回调，以确保所有会设置 origin 并应用远端事务的逻辑复制
	 * worker 都受到保护。
	 */
	before_shmem_exit(replorigin_reset, (Datum) 0);
}

/*
 * Reset the origin state.
 *
 * 重置 origin 状态。
 */
static void
replorigin_reset(int code, Datum arg)
{
	replorigin_session_origin = InvalidRepOriginId;
	replorigin_session_origin_lsn = InvalidXLogRecPtr;
	replorigin_session_origin_timestamp = 0;
}

/* Common function to setup the leader apply or tablesync worker.
 *
 * 设置 leader apply 或 tablesync worker 的公共函数。
 */
void
SetupApplyOrSyncWorker(int worker_slot)
{
	/* Attach to slot
	 *
	 * 挂接到槽
	 */
	logicalrep_worker_attach(worker_slot);

	Assert(am_tablesync_worker() || am_leader_apply_worker());

	/* Setup signal handling
	 *
	 * 设置信号处理
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * We don't currently need any ResourceOwner in a walreceiver process, but
	 * if we did, we could call CreateAuxProcessResourceOwner here.
	 *
	 * 目前 walreceiver 进程不需要任何 ResourceOwner，但如果需要，可以在这里调用
	 * CreateAuxProcessResourceOwner。
	 */

	/* Initialise stats to a sanish value
	 *
	 * 把统计初始化为一个大致合理的值
	 */
	MyLogicalRepWorker->last_send_time = MyLogicalRepWorker->last_recv_time =
		MyLogicalRepWorker->reply_time = GetCurrentTimestamp();

	/* Load the libpq-specific functions
	 *
	 * 装载 libpq 专用函数
	 */
	load_file("libpqwalreceiver", false);

	InitializeLogRepWorker();

	/* Connect to the origin and start the replication.
	 *
	 * 连接到源端并开始复制。
	 */
	elog(DEBUG1, "connecting to publisher using connection string \"%s\"",
		 MySubscription->conninfo);

	/*
	 * Setup callback for syscache so that we know when something changes in
	 * the subscription relation state.
	 *
	 * 为系统缓存设置回调，以便在订阅关系状态发生变化时得知。
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONRELMAP,
								  invalidate_syncing_table_states,
								  (Datum) 0);
}

/*
 * 核心流程：
 * ApplyWorkerMain 是 apply worker 入口。先经 SetupApplyOrSyncWorker 挂接 worker 槽、
 * 装载 libpqwalreceiver，再由 InitializeLogRepWorker 连接本库并加载订阅。
 * 随后 run_apply_worker 建立复制 origin 与流式选项，进入 LogicalRepApplyLoop。
 * 循环里由 apply_dispatch 按协议消息分发：普通事务直接应用 DML；
 * streaming = on 时把大事务写入临时文件，提交时再重放；
 * streaming = parallel 时交给并行 apply worker。
 * 两阶段事务在 two_phase 变为 ENABLED 后于 PREPARE 时准备，再在 COMMIT PREPARED
 * 或 ROLLBACK PREPARED 时结束。
 */
/* Logical Replication Apply worker entry point
 *
 * 逻辑复制 apply worker 的入口
 */
void
ApplyWorkerMain(Datum main_arg)
{
	int			worker_slot = DatumGetInt32(main_arg);

	InitializingApplyWorker = true;

	SetupApplyOrSyncWorker(worker_slot);

	InitializingApplyWorker = false;

	run_apply_worker();

	proc_exit(0);
}

/*
 * After error recovery, disable the subscription in a new transaction
 * and exit cleanly.
 *
 * 错误恢复之后，在新事务中禁用订阅并干净退出。
 */
void
DisableSubscriptionAndExit(void)
{
	/*
	 * Emit the error message, and recover from the error state to an idle
	 * state
	 *
	 * 发出错误消息，并从错误状态恢复到空闲状态
	 */
	HOLD_INTERRUPTS();

	EmitErrorReport();
	AbortOutOfAnyTransaction();
	FlushErrorState();

	RESUME_INTERRUPTS();

	/* Report the worker failed during either table synchronization or apply
	 *
	 * 报告 worker 在表同步或应用期间失败
	 */
	pgstat_report_subscription_error(MyLogicalRepWorker->subid,
									 !am_tablesync_worker());

	/* Disable the subscription
	 *
	 * 禁用订阅
	 */
	StartTransactionCommand();

	/*
	 * Updating pg_subscription might involve TOAST table access, so ensure we
	 * have a valid snapshot.
	 *
	 * 更新 pg_subscription 可能访问 TOAST 表，因此确保已有有效快照。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	DisableSubscription(MySubscription->oid);
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Ensure we remove no-longer-useful entry for worker's start time
	 *
	 * 确保移除对 worker 启动时间不再有用的条目
	 */
	if (am_leader_apply_worker())
		ApplyLauncherForgetWorkerStartTime(MyLogicalRepWorker->subid);

	/* Notify the subscription has been disabled and exit
	 *
	 * 通知订阅已被禁用并退出
	 */
	ereport(LOG,
			errmsg("subscription \"%s\" has been disabled because of an error",
				   MySubscription->name));

	proc_exit(0);
}

/*
 * Is current process a logical replication worker?
 *
 * 当前进程是否为逻辑复制 worker？
 */
bool
IsLogicalWorker(void)
{
	return MyLogicalRepWorker != NULL;
}

/*
 * Is current process a logical replication parallel apply worker?
 *
 * 当前进程是否为逻辑复制并行 apply worker？
 */
bool
IsLogicalParallelApplyWorker(void)
{
	return IsLogicalWorker() && am_parallel_apply_worker();
}

/*
 * Start skipping changes of the transaction if the given LSN matches the
 * LSN specified by subscription's skiplsn.
 *
 * 若给定 LSN 与订阅 skiplsn 指定的 LSN 匹配，则开始跳过该事务的变更。
 */
static void
maybe_start_skipping_changes(XLogRecPtr finish_lsn)
{
	Assert(!is_skipping_changes());
	Assert(!in_remote_transaction);
	Assert(!in_streamed_transaction);

	/*
	 * Quick return if it's not requested to skip this transaction. This
	 * function is called for every remote transaction and we assume that
	 * skipping the transaction is not used often.
	 *
	 * 若并未要求跳过此事务则快速返回。本函数对每个远端事务都会调用，我们假定跳过事务并不常用。
	 */
	if (likely(XLogRecPtrIsInvalid(MySubscription->skiplsn) ||
			   MySubscription->skiplsn != finish_lsn))
		return;

	/* Start skipping all changes of this transaction
	 *
	 * 开始跳过此事务的全部变更
	 */
	skip_xact_finish_lsn = finish_lsn;

	ereport(LOG,
			errmsg("logical replication starts skipping transaction at LSN %X/%X",
				   LSN_FORMAT_ARGS(skip_xact_finish_lsn)));
}

/*
 * Stop skipping changes by resetting skip_xact_finish_lsn if enabled.
 *
 * 若已启用，则通过重置 skip_xact_finish_lsn 停止跳过变更。
 */
static void
stop_skipping_changes(void)
{
	if (!is_skipping_changes())
		return;

	ereport(LOG,
			(errmsg("logical replication completed skipping transaction at LSN %X/%X",
					LSN_FORMAT_ARGS(skip_xact_finish_lsn))));

	/* Stop skipping changes
	 *
	 * 停止跳过变更
	 */
	skip_xact_finish_lsn = InvalidXLogRecPtr;
}

/*
 * Clear subskiplsn of pg_subscription catalog.
 *
 * 清除 pg_subscription 目录中的 subskiplsn。
 *
 * finish_lsn is the transaction's finish LSN that is used to check if the
 * subskiplsn matches it. If not matched, we raise a warning when clearing the
 * subskiplsn in order to inform users for cases e.g., where the user mistakenly
 * specified the wrong subskiplsn.
 *
 * finish_lsn 是事务的结束 LSN，用来检查 subskiplsn 是否与它匹配。若不匹配，清除
 * subskiplsn 时发出警告，以便告知用户，例如用户误指定了错误的 subskiplsn。
 */
static void
clear_subscription_skip_lsn(XLogRecPtr finish_lsn)
{
	Relation	rel;
	Form_pg_subscription subform;
	HeapTuple	tup;
	XLogRecPtr	myskiplsn = MySubscription->skiplsn;
	bool		started_tx = false;

	if (likely(XLogRecPtrIsInvalid(myskiplsn)) || am_parallel_apply_worker())
		return;

	if (!IsTransactionState())
	{
		StartTransactionCommand();
		started_tx = true;
	}

	/*
	 * Updating pg_subscription might involve TOAST table access, so ensure we
	 * have a valid snapshot.
	 *
	 * 更新 pg_subscription 可能访问 TOAST 表，因此确保已有有效快照。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Protect subskiplsn of pg_subscription from being concurrently updated
	 * while clearing it.
	 *
	 * 保护 pg_subscription 的 subskiplsn，防止在清除时被并发更新。
	 */
	LockSharedObject(SubscriptionRelationId, MySubscription->oid, 0,
					 AccessShareLock);

	rel = table_open(SubscriptionRelationId, RowExclusiveLock);

	/* Fetch the existing tuple.
	 *
	 * 读取已有元组。
	 */
	tup = SearchSysCacheCopy1(SUBSCRIPTIONOID,
							  ObjectIdGetDatum(MySubscription->oid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "subscription \"%s\" does not exist", MySubscription->name);

	subform = (Form_pg_subscription) GETSTRUCT(tup);

	/*
	 * Clear the subskiplsn. If the user has already changed subskiplsn before
	 * clearing it we don't update the catalog and the replication origin
	 * state won't get advanced. So in the worst case, if the server crashes
	 * before sending an acknowledgment of the flush position the transaction
	 * will be sent again and the user needs to set subskiplsn again. We can
	 * reduce the possibility by logging a replication origin WAL record to
	 * advance the origin LSN instead but there is no way to advance the
	 * origin timestamp and it doesn't seem to be worth doing anything about
	 * it since it's a very rare case.
	 *
	 * 清除 subskiplsn。若用户在清除之前已经改过 subskiplsn，我们不更新目录，
	 * 复制 origin 状态也不会前移。因此最坏情况下，若服务器在发送刷盘位置确认之前崩溃，
	 * 该事务会被再次发送，用户需要重新设置 subskiplsn。可以通过记录一条复制
	 * origin WAL 记录来前移 origin LSN，从而降低这种可能性，但没有办法前移
	 * origin 时间戳，而且这种情况非常罕见，似乎不值得为此做更多处理。
	 */
	if (subform->subskiplsn == myskiplsn)
	{
		bool		nulls[Natts_pg_subscription];
		bool		replaces[Natts_pg_subscription];
		Datum		values[Natts_pg_subscription];

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replaces, false, sizeof(replaces));

		/* reset subskiplsn
		 *
		 * 重置 subskiplsn
		 */
		values[Anum_pg_subscription_subskiplsn - 1] = LSNGetDatum(InvalidXLogRecPtr);
		replaces[Anum_pg_subscription_subskiplsn - 1] = true;

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
								replaces);
		CatalogTupleUpdate(rel, &tup->t_self, tup);

		if (myskiplsn != finish_lsn)
			ereport(WARNING,
					errmsg("skip-LSN of subscription \"%s\" cleared", MySubscription->name),
					errdetail("Remote transaction's finish WAL location (LSN) %X/%X did not match skip-LSN %X/%X.",
							  LSN_FORMAT_ARGS(finish_lsn),
							  LSN_FORMAT_ARGS(myskiplsn)));
	}

	heap_freetuple(tup);
	table_close(rel, NoLock);

	PopActiveSnapshot();

	if (started_tx)
		CommitTransactionCommand();
}

/* Error callback to give more context info about the change being applied
 *
 * 错误回调，给出正在应用的变更的更多上下文信息
 */
void
apply_error_callback(void *arg)
{
	ApplyErrorCallbackArg *errarg = &apply_error_callback_arg;

	if (apply_error_callback_arg.command == 0)
		return;

	Assert(errarg->origin_name);

	if (errarg->rel == NULL)
	{
		if (!TransactionIdIsValid(errarg->remote_xid))
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\"",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command));
		else if (XLogRecPtrIsInvalid(errarg->finish_lsn))
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" in transaction %u",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command),
					   errarg->remote_xid);
		else
			errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" in transaction %u, finished at %X/%X",
					   errarg->origin_name,
					   logicalrep_message_type(errarg->command),
					   errarg->remote_xid,
					   LSN_FORMAT_ARGS(errarg->finish_lsn));
	}
	else
	{
		if (errarg->remote_attnum < 0)
		{
			if (XLogRecPtrIsInvalid(errarg->finish_lsn))
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" in transaction %u",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->remote_xid);
			else
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" in transaction %u, finished at %X/%X",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->remote_xid,
						   LSN_FORMAT_ARGS(errarg->finish_lsn));
		}
		else
		{
			if (XLogRecPtrIsInvalid(errarg->finish_lsn))
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" column \"%s\" in transaction %u",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->rel->remoterel.attnames[errarg->remote_attnum],
						   errarg->remote_xid);
			else
				errcontext("processing remote data for replication origin \"%s\" during message type \"%s\" for replication target relation \"%s.%s\" column \"%s\" in transaction %u, finished at %X/%X",
						   errarg->origin_name,
						   logicalrep_message_type(errarg->command),
						   errarg->rel->remoterel.nspname,
						   errarg->rel->remoterel.relname,
						   errarg->rel->remoterel.attnames[errarg->remote_attnum],
						   errarg->remote_xid,
						   LSN_FORMAT_ARGS(errarg->finish_lsn));
		}
	}
}

/* Set transaction information of apply error callback
 *
 * 设置 apply 错误回调的事务信息
 */
static inline void
set_apply_error_context_xact(TransactionId xid, XLogRecPtr lsn)
{
	apply_error_callback_arg.remote_xid = xid;
	apply_error_callback_arg.finish_lsn = lsn;
}

/* Reset all information of apply error callback
 *
 * 重置 apply 错误回调的全部信息
 */
static inline void
reset_apply_error_context_info(void)
{
	apply_error_callback_arg.command = 0;
	apply_error_callback_arg.rel = NULL;
	apply_error_callback_arg.remote_attnum = -1;
	set_apply_error_context_xact(InvalidTransactionId, InvalidXLogRecPtr);
}

/*
 * Request wakeup of the workers for the given subscription OID
 * at commit of the current transaction.
 *
 * 请求在当前事务提交时唤醒给定订阅 OID 的 worker。
 *
 * This is used to ensure that the workers process assorted changes
 * as soon as possible.
 *
 * 用于确保 worker 尽快处理各类变更。
 */
void
LogicalRepWorkersWakeupAtCommit(Oid subid)
{
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	on_commit_wakeup_workers_subids =
		list_append_unique_oid(on_commit_wakeup_workers_subids, subid);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Wake up the workers of any subscriptions that were changed in this xact.
 *
 * 唤醒本事务中发生过变化的任何订阅的 worker。
 */
void
AtEOXact_LogicalRepWorkers(bool isCommit)
{
	if (isCommit && on_commit_wakeup_workers_subids != NIL)
	{
		ListCell   *lc;

		LWLockAcquire(LogicalRepWorkerLock, LW_SHARED);
		foreach(lc, on_commit_wakeup_workers_subids)
		{
			Oid			subid = lfirst_oid(lc);
			List	   *workers;
			ListCell   *lc2;

			workers = logicalrep_workers_find(subid, true, false);
			foreach(lc2, workers)
			{
				LogicalRepWorker *worker = (LogicalRepWorker *) lfirst(lc2);

				logicalrep_worker_wakeup_ptr(worker);
			}
		}
		LWLockRelease(LogicalRepWorkerLock);
	}

	/* The List storage will be reclaimed automatically in xact cleanup.
	 *
	 * List 的存储会在事务清理时自动回收。
	 */
	on_commit_wakeup_workers_subids = NIL;
}

/*
 * Allocate the origin name in long-lived context for error context message.
 *
 * 在长生命周期的上下文中为错误上下文消息分配 origin 名称。
 */
void
set_apply_error_context_origin(char *originname)
{
	apply_error_callback_arg.origin_name = MemoryContextStrdup(ApplyContext,
															   originname);
}

/*
 * Return the action to be taken for the given transaction. See
 * TransApplyAction for information on each of the actions.
 *
 * 返回对给定事务所采取的动作。各动作的说明见 TransApplyAction。
 *
 * *winfo is assigned to the destination parallel worker info when the leader
 * apply worker has to pass all the transaction's changes to the parallel
 * apply worker.
 *
 * 当 leader apply worker 必须把该事务的全部变更交给并行 apply worker 时，把 winfo
 * 设为目的地并行 worker 的信息。
 */
static TransApplyAction
get_transaction_apply_action(TransactionId xid, ParallelApplyWorkerInfo **winfo)
{
	*winfo = NULL;

	if (am_parallel_apply_worker())
	{
		return TRANS_PARALLEL_APPLY;
	}

	/*
	 * If we are processing this transaction using a parallel apply worker
	 * then either we send the changes to the parallel worker or if the worker
	 * is busy then serialize the changes to the file which will later be
	 * processed by the parallel worker.
	 *
	 * 若正在用并行 apply worker 处理此事务，则要么把变更发给并行 worker，
	 * 要么在该 worker 繁忙时把变更序列化到文件，稍后由并行 worker 处理。
	 */
	*winfo = pa_find_worker(xid);

	if (*winfo && (*winfo)->serialize_changes)
	{
		return TRANS_LEADER_PARTIAL_SERIALIZE;
	}
	else if (*winfo)
	{
		return TRANS_LEADER_SEND_TO_PARALLEL;
	}

	/*
	 * If there is no parallel worker involved to process this transaction
	 * then we either directly apply the change or serialize it to a file
	 * which will later be applied when the transaction finish message is
	 * processed.
	 *
	 * 若没有并行 worker 参与处理此事务，则要么直接应用变更，要么把它序列化到文件，
	 * 等处理事务结束消息时再应用。
	 */
	else if (in_streamed_transaction)
	{
		return TRANS_LEADER_SERIALIZE;
	}
	else
	{
		return TRANS_LEADER_APPLY;
	}
}
