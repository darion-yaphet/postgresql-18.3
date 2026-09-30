/*-------------------------------------------------------------------------
 * applyparallelworker.c
 *	   Support routines for applying xact by parallel apply worker
 *
 * 由并行 apply worker 应用事务的支持例程。
 *
 * Copyright (c) 2023-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/applyparallelworker.c
 *
 * This file contains the code to launch, set up, and teardown a parallel apply
 * worker which receives the changes from the leader worker and invokes routines
 * to apply those on the subscriber database. Additionally, this file contains
 * routines that are intended to support setting up, using, and tearing down a
 * ParallelApplyWorkerInfo which is required so the leader worker and parallel
 * apply workers can communicate with each other.
 *
 * 本文件负责启动、建立并拆除并行 apply worker。它从 leader worker 接收
 * 变更，并调用例程应用到订阅端数据库。另外还包含建立、使用和拆除
 * ParallelApplyWorkerInfo 的例程，供 leader worker 与并行 apply worker
 * 互相通信。
 *
 * The parallel apply workers are assigned (if available) as soon as xact's
 * first stream is received for subscriptions that have set their 'streaming'
 * option as parallel. The leader apply worker will send changes to this new
 * worker via shared memory. We keep this worker assigned till the transaction
 * commit is received and also wait for the worker to finish at commit. This
 * preserves commit ordering and avoid file I/O in most cases, although we
 * still need to spill to a file if there is no worker available. See comments
 * atop logical/worker to know more about streamed xacts whose changes are
 * spilled to disk. It is important to maintain commit order to avoid failures
 * due to: (a) transaction dependencies - say if we insert a row in the first
 * transaction and update it in the second transaction on publisher then
 * allowing the subscriber to apply both in parallel can lead to failure in the
 * update; (b) deadlocks - allowing transactions that update the same set of
 * rows/tables in the opposite order to be applied in parallel can lead to
 * deadlocks.
 *
 * 订阅把 streaming 设为 parallel 后，事务的第一个 stream 一到，就分配
 * 一个可用的并行 apply worker。leader apply worker 经共享内存把变更发
 * 给这个 worker，并一直占用到收到事务提交，提交时还要等它做完。这样能
 * 保持提交顺序，多数情况下也避免文件 I/O；没有可用 worker 时仍要把变更
 * 溢写到文件。溢写到磁盘的流式事务见 logical/worker 文件头的注释。必须
 * 保持提交顺序，否则会失败：（a）事务依赖，例如发布端在第一个事务插入
 * 一行、在第二个事务更新它，订阅端若并行应用，更新可能失败；（b）死锁，
 * 以相反顺序更新同一批行或表的事务若并行应用，可能死锁。
 *
 * A worker pool is used to avoid restarting workers for each streaming
 * transaction. We maintain each worker's information (ParallelApplyWorkerInfo)
 * in the ParallelApplyWorkerPool. After successfully launching a new worker,
 * its information is added to the ParallelApplyWorkerPool. Once the worker
 * finishes applying the transaction, it is marked as available for re-use.
 * Now, before starting a new worker to apply the streaming transaction, we
 * check the list for any available worker. Note that we retain a maximum of
 * half the max_parallel_apply_workers_per_subscription workers in the pool and
 * after that, we simply exit the worker after applying the transaction.
 *
 * 用 worker 池避免每个流式事务都重新启动 worker。每个 worker 的信息（
 * ParallelApplyWorkerInfo）保存在 ParallelApplyWorkerPool 里。成功启动
 * 后把信息加入池；事务应用完后标记为可再次使用。开始新的流式事务前，先
 * 在列表里找空闲 worker。池里最多保留
 * max_parallel_apply_workers_per_subscription 的一半，超过之后，事务应
 * 用完就直接让该 worker 退出。
 *
 * XXX This worker pool threshold is arbitrary and we can provide a GUC
 * variable for this in the future if required.
 *
 * XXX 这个池大小阈值是随意定的，以后如果需要可以做成 GUC。
 *
 * The leader apply worker will create a separate dynamic shared memory segment
 * when each parallel apply worker starts. The reason for this design is that
 * we cannot predict how many workers will be needed. It may be possible to
 * allocate enough shared memory in one segment based on the maximum number of
 * parallel apply workers (max_parallel_apply_workers_per_subscription), but
 * this would waste memory if no process is actually started.
 *
 * 每个并行 apply worker 启动时，leader apply worker 都会单独建一段动态
 * 共享内存。因为无法预先知道实际需要多少 worker。也可以按并行 apply
 * worker 的上限 max_parallel_apply_workers_per_subscription 在一段里一
 * 次分配够，但若实际上没有进程启动，就会浪费内存。
 *
 * The dynamic shared memory segment contains: (a) a shm_mq that is used to
 * send changes in the transaction from leader apply worker to parallel apply
 * worker; (b) another shm_mq that is used to send errors (and other messages
 * reported via elog/ereport) from the parallel apply worker to leader apply
 * worker; (c) necessary information to be shared among parallel apply workers
 * and the leader apply worker (i.e. members of ParallelApplyWorkerShared).
 *
 * 这段动态共享内存包含：（a）一条 shm_mq，用来把事务中的变更从 leader
 * apply worker 发给并行 apply worker；（b）另一条 shm_mq，用来把错误以
 * 及经 elog/ereport 报告的其他消息从并行 apply worker 发回 leader
 * apply worker；（c）并行 apply worker 与 leader apply worker 之间要共
 * 享的信息，即 ParallelApplyWorkerShared 的成员。
 *
 * Locking Considerations
 * ----------------------
 * We have a risk of deadlock due to concurrently applying the transactions in
 * parallel mode that were independent on the publisher side but became
 * dependent on the subscriber side due to the different database structures
 * (like schema of subscription tables, constraints, etc.) on each side. This
 * can happen even without parallel mode when there are concurrent operations
 * on the subscriber. In order to detect the deadlocks among leader (LA) and
 * parallel apply (PA) workers, we used lmgr locks when the PA waits for the
 * next stream (set of changes) and LA waits for PA to finish the transaction.
 * An alternative approach could be to not allow parallelism when the schema of
 * tables is different between the publisher and subscriber but that would be
 * too restrictive and would require the publisher to send much more
 * information than it is currently sending.
 *
 * 加锁方面的考虑。并行应用那些在发布端彼此独立、但因两端库结构不同（例
 * 如订阅表的 schema、约束等）而在订阅端变得有依赖的事务时，有死锁风险。
 * 即使不用并行模式，订阅端上的并发操作也可能如此。为了检测 leader（LA）
 * 与并行 apply（PA）worker 之间的死锁，在 PA 等待下一批 stream（一组变
 * 更）、LA 等待 PA 做完事务时使用 lmgr 锁。另一种做法是表结构在发布端
 * 与订阅端不同时就不允许并行，但那太严格，也要求发布端发送比现在多得多
 * 的信息。
 *
 * Consider a case where the subscribed table does not have a unique key on the
 * publisher and has a unique key on the subscriber. The deadlock can happen in
 * the following ways:
 *
 * 考虑订阅表在发布端没有唯一键、在订阅端有唯一键的情况。死锁可能按下面
 * 几种方式发生：
 *
 * 1) Deadlock between the leader apply worker and a parallel apply worker
 *
 * 1) leader apply worker 与一个并行 apply worker 之间的死锁
 *
 * Consider that the parallel apply worker (PA) is executing TX-1 and the
 * leader apply worker (LA) is executing TX-2 concurrently on the subscriber.
 * Now, LA is waiting for PA because of the unique key constraint of the
 * subscribed table while PA is waiting for LA to send the next stream of
 * changes or transaction finish command message.
 *
 * 设并行 apply worker（PA）正在订阅端执行 TX-1，leader apply worker（
 * LA）同时执行 TX-2。LA 因订阅表的唯一键约束而等待 PA，PA 则在等 LA 发
 * 送下一批变更或事务结束命令。
 *
 * In order for lmgr to detect this, we have LA acquire a session lock on the
 * remote transaction (by pa_lock_stream()) and have PA wait on the lock before
 * trying to receive the next stream of changes. Specifically, LA will acquire
 * the lock in AccessExclusive mode before sending the STREAM_STOP and will
 * release it if already acquired after sending the STREAM_START, STREAM_ABORT
 * (for toplevel transaction), STREAM_PREPARE, and STREAM_COMMIT. The PA will
 * acquire the lock in AccessShare mode after processing STREAM_STOP and
 * STREAM_ABORT (for subtransaction) and then release the lock immediately
 * after acquiring it.
 *
 * 为了让 lmgr 能发现这种情况，LA 对远程事务加会话锁（pa_lock_stream()），
 * PA 在尝试接收下一批变更之前等待该锁。具体而言，LA 在发送 STREAM_STOP
 * 之前以 AccessExclusive 模式获取该锁，并在发送 STREAM_START、
 * STREAM_ABORT（顶层事务）、STREAM_PREPARE 和 STREAM_COMMIT 之后，若已
 * 经持有就释放。PA 在处理完 STREAM_STOP 和 STREAM_ABORT（子事务）后以
 * AccessShare 模式获取该锁，拿到后立刻释放。
 *
 * The lock graph for the above example will look as follows:
 * LA (waiting to acquire the lock on the unique index) -> PA (waiting to
 * acquire the stream lock) -> LA
 *
 * 上例的锁等待关系是：LA 等待唯一索引上的锁，PA 等待流锁，然后回到 LA。
 *
 * This way, when PA is waiting for LA for the next stream of changes, we can
 * have a wait-edge from PA to LA in lmgr, which will make us detect the
 * deadlock between LA and PA.
 *
 * 这样，当 PA 等待 LA 发送下一批变更时，lmgr 中就有一条从 PA 到 LA 的
 * 等待边，从而能检测出 LA 与 PA 之间的死锁。
 *
 * 2) Deadlock between the leader apply worker and parallel apply workers
 *
 * 2) leader apply worker 与多个并行 apply worker 之间的死锁
 *
 * This scenario is similar to the first case but TX-1 and TX-2 are executed by
 * two parallel apply workers (PA-1 and PA-2 respectively). In this scenario,
 * PA-2 is waiting for PA-1 to complete its transaction while PA-1 is waiting
 * for subsequent input from LA. Also, LA is waiting for PA-2 to complete its
 * transaction in order to preserve the commit order. There is a deadlock among
 * the three processes.
 *
 * 此情形与第一种类似，但 TX-1 和 TX-2 分别由两个并行 apply worker（PA-
 * 1 和 PA-2）执行。PA-2 等 PA-1 做完事务，PA-1 等 LA 的后续输入，而 LA
 * 为了保持提交顺序又在等 PA-2 做完事务。三个进程之间形成死锁。
 *
 * In order for lmgr to detect this, we have PA acquire a session lock (this is
 * a different lock than referred in the previous case, see
 * pa_lock_transaction()) on the transaction being applied and have LA wait on
 * the lock before proceeding in the transaction finish commands. Specifically,
 * PA will acquire this lock in AccessExclusive mode before executing the first
 * message of the transaction and release it at the xact end. LA will acquire
 * this lock in AccessShare mode at transaction finish commands and release it
 * immediately.
 *
 * 为了让 lmgr 能发现这种情况，PA 对正在应用的事务加会话锁（这与上一情
 * 形的锁不同，见 pa_lock_transaction()），LA 在继续处理事务结束命令之
 * 前等待该锁。具体而言，PA 在执行该事务的第一条消息之前以
 * AccessExclusive 模式获取此锁，并在事务结束时释放。LA 在事务结束命令
 * 处以 AccessShare 模式获取此锁，并立刻释放。
 *
 * The lock graph for the above example will look as follows:
 * LA (waiting to acquire the transaction lock) -> PA-2 (waiting to acquire the
 * lock due to unique index constraint) -> PA-1 (waiting to acquire the stream
 * lock) -> LA
 *
 * 上例的锁等待关系是：LA 等待事务锁，PA-2 因唯一索引约束等待锁，PA-1
 * 等待流锁，然后回到 LA。
 *
 * This way when LA is waiting to finish the transaction end command to preserve
 * the commit order, we will be able to detect deadlock, if any.
 *
 * 这样，当 LA 为保持提交顺序而等待事务结束命令完成时，就能检测出死锁。
 *
 * One might think we can use XactLockTableWait(), but XactLockTableWait()
 * considers PREPARED TRANSACTION as still in progress which means the lock
 * won't be released even after the parallel apply worker has prepared the
 * transaction.
 *
 * 有人可能想到用 XactLockTableWait()，但它把 PREPARED TRANSACTION 仍视
 * 为进行中，因此即使并行 apply worker 已经完成 prepare，锁也不会释放。
 *
 * 3) Deadlock when the shm_mq buffer is full
 *
 * 3) shm_mq 缓冲区写满时的死锁
 *
 * In the previous scenario (ie. PA-1 and PA-2 are executing transactions
 * concurrently), if the shm_mq buffer between LA and PA-2 is full, LA has to
 * wait to send messages, and this wait doesn't appear in lmgr.
 *
 * 在前一情形中（即 PA-1 与 PA-2 并发执行事务），若 LA 与 PA-2 之间的
 * shm_mq 缓冲区已满，LA 必须等待才能发送消息，而这次等待不会出现在
 * lmgr 里。
 *
 * To avoid this wait, we use a non-blocking write and wait with a timeout. If
 * the timeout is exceeded, the LA will serialize all the pending messages to
 * a file and indicate PA-2 that it needs to read that file for the remaining
 * messages. Then LA will start waiting for commit as in the previous case
 * which will detect deadlock if any. See pa_send_data() and
 * enum TransApplyAction.
 *
 * 为避免这种等待，这里采用非阻塞写，并带超时地等待。若超时，LA 把所有
 * 待发消息序列化到文件，并通知 PA-2 剩余消息要去读该文件。然后 LA 像前
 * 一情形那样开始等待提交，从而能检测死锁。见 pa_send_data() 与枚举
 * TransApplyAction。
 *
 * Lock types
 * ----------
 * Both the stream lock and the transaction lock mentioned above are
 * session-level locks because both locks could be acquired outside the
 * transaction, and the stream lock in the leader needs to persist across
 * transaction boundaries i.e. until the end of the streaming transaction.
 *
 * 锁的类型。上面提到的流锁和事务锁都是会话级锁，因为两者都可能在事务之
 * 外获取，而且 leader 上的流锁需要跨越事务边界，一直保持到流式事务结束。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "replication/logicallauncher.h"
#include "replication/logicalworker.h"
#include "replication/origin.h"
#include "replication/worker_internal.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

/*
 * 核心流程：
 * leader 在流式事务的第一个 stream 到达时调用 pa_allocate_worker，从
 * ParallelApplyWorkerPool 取空闲 worker，或经 pa_launch_parallel_worker 与
 * pa_setup_dsm 建立 DSM 后拉起新 worker。变更由 pa_send_data 经 shm_mq 送出；
 * 写超时则 pa_switch_to_partial_serialize 把剩余变更落到文件。提交时
 * pa_xact_finish 等待该 worker 结束以保持提交顺序，再由 pa_free_worker 回收或退出。
 * worker 入口是 ParallelApplyWorkerMain：挂上 DSM、消息队列和 error queue 后进入
 * LogicalParallelApplyLoop，用 apply_dispatch 应用变更。错误经 error queue 回到
 * leader 的 ProcessParallelApplyMessages。死锁检测使用 pa_lock_stream 与
 * pa_lock_transaction 两组会话锁。
 */

#define PG_LOGICAL_APPLY_SHM_MAGIC 0x787ca067

/*
 * DSM keys for parallel apply worker. Unlike other parallel execution code,
 * since we don't need to worry about DSM keys conflicting with plan_node_id we
 * can use small integers.
 *
 * 并行 apply worker 使用的 DSM 键。与其他并行执行代码不同，这里不必担
 * 心 DSM 键与 plan_node_id 冲突，因此可以用较小的整数。
 */
#define PARALLEL_APPLY_KEY_SHARED		1
#define PARALLEL_APPLY_KEY_MQ			2
#define PARALLEL_APPLY_KEY_ERROR_QUEUE	3

/* Queue size of DSM, 16 MB for now.
 *
 * DSM 队列大小，目前为 16 MB。
 */
#define DSM_QUEUE_SIZE	(16 * 1024 * 1024)

/*
 * Error queue size of DSM. It is desirable to make it large enough that a
 * typical ErrorResponse can be sent without blocking. That way, a worker that
 * errors out can write the whole message into the queue and terminate without
 * waiting for the user backend.
 *
 * DSM 错误队列的大小。最好大到能不阻塞地送出一条典型的 ErrorResponse。
 * 这样，出错的 worker 可以把整条消息写入队列后直接退出，不必等待用户后端。
 */
#define DSM_ERROR_QUEUE_SIZE			(16 * 1024)

/*
 * There are three fields in each message received by the parallel apply
 * worker: start_lsn, end_lsn and send_time. Because we have updated these
 * statistics in the leader apply worker, we can ignore these fields in the
 * parallel apply worker (see function LogicalRepApplyLoop).
 *
 * 并行 apply worker 收到的每条消息里有三个字段：start_lsn、end_lsn 和
 * send_time。这些统计已经在 leader apply worker 里更新过，因此并行
 * apply worker 可以忽略它们（见函数 LogicalRepApplyLoop）。
 */
#define SIZE_STATS_MESSAGE (2 * sizeof(XLogRecPtr) + sizeof(TimestampTz))

/*
 * The type of session-level lock on a transaction being applied on a logical
 * replication subscriber.
 *
 * 在逻辑复制订阅端上，对正在应用的事务所加的会话级锁类型。
 */
#define PARALLEL_APPLY_LOCK_STREAM	0
#define PARALLEL_APPLY_LOCK_XACT	1

/*
 * Hash table entry to map xid to the parallel apply worker state.
 *
 * 把 xid 映射到并行 apply worker 状态的哈希表项。
 */
typedef struct ParallelApplyWorkerEntry
{
	TransactionId xid;			/* Hash key -- must be first
								 *
								 * 哈希键，必须放在首位
								 */
	ParallelApplyWorkerInfo *winfo;
} ParallelApplyWorkerEntry;

/*
 * A hash table used to cache the state of streaming transactions being applied
 * by the parallel apply workers.
 *
 * 用于缓存正在由并行 apply worker 应用的流式事务状态的哈希表。
 */
static HTAB *ParallelApplyTxnHash = NULL;

/*
* A list (pool) of active parallel apply workers. The information for
* the new worker is added to the list after successfully launching it. The
* list entry is removed if there are already enough workers in the worker
* pool at the end of the transaction. For more information about the worker
* pool, see comments atop this file.
*
* 活跃并行 apply worker 的列表（池）。成功启动新 worker 后，把它的信息
* 加入列表。事务结束时，若池里 worker 已经足够多，就从列表中删掉该项。
* 关于 worker 池的更多说明见本文件头部注释。
 */
static List *ParallelApplyWorkerPool = NIL;

/*
 * Information shared between leader apply worker and parallel apply worker.
 *
 * leader apply worker 与并行 apply worker 之间共享的信息。
 */
ParallelApplyWorkerShared *MyParallelShared = NULL;

/*
 * Is there a message sent by a parallel apply worker that the leader apply
 * worker needs to receive?
 *
 * 是否有并行 apply worker 发出、需要由 leader apply worker 接收的消息？
 */
volatile sig_atomic_t ParallelApplyMessagePending = false;

/*
 * Cache the parallel apply worker information required for applying the
 * current streaming transaction. It is used to save the cost of searching the
 * hash table when applying the changes between STREAM_START and STREAM_STOP.
 *
 * 缓存应用当前流式事务所需的并行 apply worker 信息。在 STREAM_START 与
 * STREAM_STOP 之间应用变更时，用它省去查找哈希表的开销。
 */
static ParallelApplyWorkerInfo *stream_apply_worker = NULL;

/* A list to maintain subtransactions, if any.
 *
 * 用来维护子事务（如果有）的列表。
 */
static List *subxactlist = NIL;

static void pa_free_worker_info(ParallelApplyWorkerInfo *winfo);
static ParallelTransState pa_get_xact_state(ParallelApplyWorkerShared *wshared);
static PartialFileSetState pa_get_fileset_state(void);

/*
 * Returns true if it is OK to start a parallel apply worker, false otherwise.
 *
 * 可以启动并行 apply worker 时返回 true，否则返回 false。
 */
static bool
pa_can_start(void)
{
	/* Only leader apply workers can start parallel apply workers.
	 *
	 * 只有 leader apply worker 才能启动并行 apply worker。
	 */
	if (!am_leader_apply_worker())
		return false;

	/*
	 * It is good to check for any change in the subscription parameter to
	 * avoid the case where for a very long time the change doesn't get
	 * reflected. This can happen when there is a constant flow of streaming
	 * transactions that are handled by parallel apply workers.
	 *
	 * 这里检查订阅参数是否有变化，以免很长时间都看不到变更。持续不断、并由
	 * 并行 apply worker 处理的流式事务就会造成这种情况。
	 *
	 * It is better to do it before the below checks so that the latest values
	 * of subscription can be used for the checks.
	 *
	 * 最好在下面的检查之前做这件事，这样检查时用的是订阅的最新值。
	 */
	maybe_reread_subscription();

	/*
	 * Don't start a new parallel apply worker if the subscription is not
	 * using parallel streaming mode, or if the publisher does not support
	 * parallel apply.
	 *
	 * 若订阅没有使用并行流式模式，或发布端不支持并行 apply，就不要启动新的
	 * 并行 apply worker。
	 */
	if (!MyLogicalRepWorker->parallel_apply)
		return false;

	/*
	 * Don't start a new parallel worker if user has set skiplsn as it's
	 * possible that they want to skip the streaming transaction. For
	 * streaming transactions, we need to serialize the transaction to a file
	 * so that we can get the last LSN of the transaction to judge whether to
	 * skip before starting to apply the change.
	 *
	 * 若用户设置了 skiplsn，不要启动新的并行 worker，因为他们可能想跳过这
	 * 个流式事务。对流式事务，需要先把事务序列化到文件，才能拿到事务的最后
	 * 一个 LSN，从而在开始应用变更之前判断要不要跳过。
	 *
	 * One might think that we could allow parallelism if the first lsn of the
	 * transaction is greater than skiplsn, but we don't send it with the
	 * STREAM START message, and it doesn't seem worth sending the extra eight
	 * bytes with the STREAM START to enable parallelism for this case.
	 *
	 * 有人可能认为，若事务的第一个 LSN 大于 skiplsn 就可以允许并行。但
	 * STREAM START 消息并不带上这个 LSN，也不值得为这种情况多发八个字节来
	 * 启用并行。
	 */
	if (!XLogRecPtrIsInvalid(MySubscription->skiplsn))
		return false;

	/*
	 * For streaming transactions that are being applied using a parallel
	 * apply worker, we cannot decide whether to apply the change for a
	 * relation that is not in the READY state (see
	 * should_apply_changes_for_rel) as we won't know remote_final_lsn by that
	 * time. So, we don't start the new parallel apply worker in this case.
	 *
	 * 对于正由并行 apply worker 应用的流式事务，此时还不知道
	 * remote_final_lsn，因此无法决定是否对尚未处于 READY 状态的关系应用变
	 * 更（见 should_apply_changes_for_rel）。所以这种情况下不启动新的并行
	 * apply worker。
	 */
	if (!AllTablesyncsReady())
		return false;

	return true;
}

/*
 * Set up a dynamic shared memory segment.
 *
 * 建立一段动态共享内存。
 *
 * We set up a control region that contains a fixed-size worker info
 * (ParallelApplyWorkerShared), a message queue, and an error queue.
 *
 * 建立一块控制区，其中包含固定大小的 worker 信息（
 * ParallelApplyWorkerShared）、一条消息队列和一条错误队列。
 *
 * Returns true on success, false on failure.
 *
 * 成功返回 true，失败返回 false。
 */
static bool
pa_setup_dsm(ParallelApplyWorkerInfo *winfo)
{
	shm_toc_estimator e;
	Size		segsize;
	dsm_segment *seg;
	shm_toc    *toc;
	ParallelApplyWorkerShared *shared;
	shm_mq	   *mq;
	Size		queue_size = DSM_QUEUE_SIZE;
	Size		error_queue_size = DSM_ERROR_QUEUE_SIZE;

	/*
	 * Estimate how much shared memory we need.
	 *
	 * 估算需要多少共享内存。
	 *
	 * Because the TOC machinery may choose to insert padding of oddly-sized
	 * requests, we must estimate each chunk separately.
	 *
	 * TOC 机制可能会给大小不规则的请求插入填充，因此必须分别估算每一块。
	 *
	 * We need one key to register the location of the header, and two other
	 * keys to track the locations of the message queue and the error message
	 * queue.
	 *
	 * 需要一个键来登记头部的位置，另外两个键用来跟踪消息队列和错误消息队列
	 * 的位置。
	 */
	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, sizeof(ParallelApplyWorkerShared));
	shm_toc_estimate_chunk(&e, queue_size);
	shm_toc_estimate_chunk(&e, error_queue_size);

	shm_toc_estimate_keys(&e, 3);
	segsize = shm_toc_estimate(&e);

	/* Create the shared memory segment and establish a table of contents.
	 *
	 * 创建共享内存段，并建立目录。
	 */
	seg = dsm_create(shm_toc_estimate(&e), 0);
	if (!seg)
		return false;

	toc = shm_toc_create(PG_LOGICAL_APPLY_SHM_MAGIC, dsm_segment_address(seg),
						 segsize);

	/* Set up the header region.
	 *
	 * 建立头部区域。
	 */
	shared = shm_toc_allocate(toc, sizeof(ParallelApplyWorkerShared));
	SpinLockInit(&shared->mutex);

	shared->xact_state = PARALLEL_TRANS_UNKNOWN;
	pg_atomic_init_u32(&(shared->pending_stream_count), 0);
	shared->last_commit_end = InvalidXLogRecPtr;
	shared->fileset_state = FS_EMPTY;

	shm_toc_insert(toc, PARALLEL_APPLY_KEY_SHARED, shared);

	/* Set up message queue for the worker.
	 *
	 * 为该 worker 建立消息队列。
	 */
	mq = shm_mq_create(shm_toc_allocate(toc, queue_size), queue_size);
	shm_toc_insert(toc, PARALLEL_APPLY_KEY_MQ, mq);
	shm_mq_set_sender(mq, MyProc);

	/* Attach the queue.
	 *
	 * 挂上该队列。
	 */
	winfo->mq_handle = shm_mq_attach(mq, seg, NULL);

	/* Set up error queue for the worker.
	 *
	 * 为该 worker 建立错误队列。
	 */
	mq = shm_mq_create(shm_toc_allocate(toc, error_queue_size),
					   error_queue_size);
	shm_toc_insert(toc, PARALLEL_APPLY_KEY_ERROR_QUEUE, mq);
	shm_mq_set_receiver(mq, MyProc);

	/* Attach the queue.
	 *
	 * 挂上该队列。
	 */
	winfo->error_mq_handle = shm_mq_attach(mq, seg, NULL);

	/* Return results to caller.
	 *
	 * 把结果返回给调用方。
	 */
	winfo->dsm_seg = seg;
	winfo->shared = shared;

	return true;
}

/*
 * Try to get a parallel apply worker from the pool. If none is available then
 * start a new one.
 *
 * 尝试从池中取一个并行 apply worker。如果没有可用的，就新启动一个。
 */
static ParallelApplyWorkerInfo *
pa_launch_parallel_worker(void)
{
	MemoryContext oldcontext;
	bool		launched;
	ParallelApplyWorkerInfo *winfo;
	ListCell   *lc;

	/* Try to get an available parallel apply worker from the worker pool.
	 *
	 * 尝试从 worker 池中取一个空闲的并行 apply worker。
	 */
	foreach(lc, ParallelApplyWorkerPool)
	{
		winfo = (ParallelApplyWorkerInfo *) lfirst(lc);

		if (!winfo->in_use)
			return winfo;
	}

	/*
	 * Start a new parallel apply worker.
	 *
	 * 启动一个新的并行 apply worker。
	 *
	 * The worker info can be used for the lifetime of the worker process, so
	 * create it in a permanent context.
	 *
	 * worker 信息的寿命与 worker 进程相同，因此在永久内存上下文中创建它。
	 */
	oldcontext = MemoryContextSwitchTo(ApplyContext);

	winfo = (ParallelApplyWorkerInfo *) palloc0(sizeof(ParallelApplyWorkerInfo));

	/* Setup shared memory.
	 *
	 * 建立共享内存。
	 */
	if (!pa_setup_dsm(winfo))
	{
		MemoryContextSwitchTo(oldcontext);
		pfree(winfo);
		return NULL;
	}

	launched = logicalrep_worker_launch(WORKERTYPE_PARALLEL_APPLY,
										MyLogicalRepWorker->dbid,
										MySubscription->oid,
										MySubscription->name,
										MyLogicalRepWorker->userid,
										InvalidOid,
										dsm_segment_handle(winfo->dsm_seg));

	if (launched)
	{
		ParallelApplyWorkerPool = lappend(ParallelApplyWorkerPool, winfo);
	}
	else
	{
		pa_free_worker_info(winfo);
		winfo = NULL;
	}

	MemoryContextSwitchTo(oldcontext);

	return winfo;
}

/*
 * Allocate a parallel apply worker that will be used for the specified xid.
 *
 * 为指定的 xid 分配一个并行 apply worker。
 *
 * We first try to get an available worker from the pool, if any and then try
 * to launch a new worker. On successful allocation, remember the worker
 * information in the hash table so that we can get it later for processing the
 * streaming changes.
 *
 * 先尝试从池中取一个可用 worker（如果有），然后再尝试启动新 worker。分
 * 配成功后，把 worker 信息记入哈希表，以便稍后处理流式变更时取用。
 */
void
pa_allocate_worker(TransactionId xid)
{
	bool		found;
	ParallelApplyWorkerInfo *winfo = NULL;
	ParallelApplyWorkerEntry *entry;

	if (!pa_can_start())
		return;

	winfo = pa_launch_parallel_worker();
	if (!winfo)
		return;

	/* First time through, initialize parallel apply worker state hashtable.
	 *
	 * 第一次进入时，初始化并行 apply worker 状态哈希表。
	 */
	if (!ParallelApplyTxnHash)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(TransactionId);
		ctl.entrysize = sizeof(ParallelApplyWorkerEntry);
		ctl.hcxt = ApplyContext;

		ParallelApplyTxnHash = hash_create("logical replication parallel apply workers hash",
										   16, &ctl,
										   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* Create an entry for the requested transaction.
	 *
	 * 为请求的事务创建一项。
	 */
	entry = hash_search(ParallelApplyTxnHash, &xid, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "hash table corrupted");

	/* Update the transaction information in shared memory.
	 *
	 * 更新共享内存中的事务信息。
	 */
	SpinLockAcquire(&winfo->shared->mutex);
	winfo->shared->xact_state = PARALLEL_TRANS_UNKNOWN;
	winfo->shared->xid = xid;
	SpinLockRelease(&winfo->shared->mutex);

	winfo->in_use = true;
	winfo->serialize_changes = false;
	entry->winfo = winfo;
}

/*
 * Find the assigned worker for the given transaction, if any.
 *
 * 查找指定事务已分配的 worker（如果有）。
 */
ParallelApplyWorkerInfo *
pa_find_worker(TransactionId xid)
{
	bool		found;
	ParallelApplyWorkerEntry *entry;

	if (!TransactionIdIsValid(xid))
		return NULL;

	if (!ParallelApplyTxnHash)
		return NULL;

	/* Return the cached parallel apply worker if valid.
	 *
	 * 若缓存的并行 apply worker 仍然有效，则返回它。
	 */
	if (stream_apply_worker)
		return stream_apply_worker;

	/* Find an entry for the requested transaction.
	 *
	 * 查找请求事务所对应的项。
	 */
	entry = hash_search(ParallelApplyTxnHash, &xid, HASH_FIND, &found);
	if (found)
	{
		/* The worker must not have exited.
		 *
		 * 该 worker 必定尚未退出。
		 */
		Assert(entry->winfo->in_use);
		return entry->winfo;
	}

	return NULL;
}

/*
 * Makes the worker available for reuse.
 *
 * 使该 worker 可以再次使用。
 *
 * This removes the parallel apply worker entry from the hash table so that it
 * can't be used. If there are enough workers in the pool, it stops the worker
 * and frees the corresponding info. Otherwise it just marks the worker as
 * available for reuse.
 *
 * 这会从哈希表中删除该并行 apply worker 项，使它不能再被使用。若池中
 * worker 已经足够多，就停掉该 worker 并释放对应信息；否则只把它标为可
 * 再次使用。
 *
 * For more information about the worker pool, see comments atop this file.
 *
 * 关于 worker 池的更多说明见本文件头部注释。
 */
static void
pa_free_worker(ParallelApplyWorkerInfo *winfo)
{
	Assert(!am_parallel_apply_worker());
	Assert(winfo->in_use);
	Assert(pa_get_xact_state(winfo->shared) == PARALLEL_TRANS_FINISHED);

	if (!hash_search(ParallelApplyTxnHash, &winfo->shared->xid, HASH_REMOVE, NULL))
		elog(ERROR, "hash table corrupted");

	/*
	 * Stop the worker if there are enough workers in the pool.
	 *
	 * 若池中 worker 已经足够多，就停掉该 worker。
	 *
	 * XXX Additionally, we also stop the worker if the leader apply worker
	 * serialize part of the transaction data due to a send timeout. This is
	 * because the message could be partially written to the queue and there
	 * is no way to clean the queue other than resending the message until it
	 * succeeds. Instead of trying to send the data which anyway would have
	 * been serialized and then letting the parallel apply worker deal with
	 * the spurious message, we stop the worker.
	 *
	 * XXX 此外，若 leader apply worker 因发送超时而把部分事务数据序列化了，
	 * 也会停掉该 worker。因为消息可能只写进队列一部分，除了不断重发直到成
	 * 功，没有别的办法清空队列。与其再去发送反正都已经序列化的数据，再让并
	 * 行 apply worker 去处理这条多余消息，不如直接停掉该 worker。
	 */
	if (winfo->serialize_changes ||
		list_length(ParallelApplyWorkerPool) >
		(max_parallel_apply_workers_per_subscription / 2))
	{
		logicalrep_pa_worker_stop(winfo);
		pa_free_worker_info(winfo);

		return;
	}

	winfo->in_use = false;
	winfo->serialize_changes = false;
}

/*
 * Free the parallel apply worker information and unlink the files with
 * serialized changes if any.
 *
 * 释放并行 apply worker 的信息，并删除序列化变更文件（如果有）。
 */
static void
pa_free_worker_info(ParallelApplyWorkerInfo *winfo)
{
	Assert(winfo);

	if (winfo->mq_handle)
		shm_mq_detach(winfo->mq_handle);

	if (winfo->error_mq_handle)
		shm_mq_detach(winfo->error_mq_handle);

	/* Unlink the files with serialized changes.
	 *
	 * 删除序列化变更文件。
	 */
	if (winfo->serialize_changes)
		stream_cleanup_files(MyLogicalRepWorker->subid, winfo->shared->xid);

	if (winfo->dsm_seg)
		dsm_detach(winfo->dsm_seg);

	/* Remove from the worker pool.
	 *
	 * 从 worker 池中移除。
	 */
	ParallelApplyWorkerPool = list_delete_ptr(ParallelApplyWorkerPool, winfo);

	pfree(winfo);
}

/*
 * Detach the error queue for all parallel apply workers.
 *
 * 断开所有并行 apply worker 的错误队列。
 */
void
pa_detach_all_error_mq(void)
{
	ListCell   *lc;

	foreach(lc, ParallelApplyWorkerPool)
	{
		ParallelApplyWorkerInfo *winfo = (ParallelApplyWorkerInfo *) lfirst(lc);

		if (winfo->error_mq_handle)
		{
			shm_mq_detach(winfo->error_mq_handle);
			winfo->error_mq_handle = NULL;
		}
	}
}

/*
 * Check if there are any pending spooled messages.
 *
 * 检查是否还有待处理的已转储消息。
 */
static bool
pa_has_spooled_message_pending()
{
	PartialFileSetState fileset_state;

	fileset_state = pa_get_fileset_state();

	return (fileset_state != FS_EMPTY);
}

/*
 * Replay the spooled messages once the leader apply worker has finished
 * serializing changes to the file.
 *
 * 在 leader apply worker 把变更序列化到文件之后，重放这些已转储的消息。
 *
 * Returns false if there aren't any pending spooled messages, true otherwise.
 *
 * 若没有待处理的已转储消息则返回 false，否则返回 true。
 */
static bool
pa_process_spooled_messages_if_required(void)
{
	PartialFileSetState fileset_state;

	fileset_state = pa_get_fileset_state();

	if (fileset_state == FS_EMPTY)
		return false;

	/*
	 * If the leader apply worker is busy serializing the partial changes then
	 * acquire the stream lock now and wait for the leader worker to finish
	 * serializing the changes. Otherwise, the parallel apply worker won't get
	 * a chance to receive a STREAM_STOP (and acquire the stream lock) until
	 * the leader had serialized all changes which can lead to undetected
	 * deadlock.
	 *
	 * 若 leader apply worker 正在忙着序列化部分变更，就现在获取流锁，并等
	 * 待 leader 把变更序列化完。否则，并行 apply worker 要等到 leader 把全
	 * 部变更都序列化完，才有机会收到 STREAM_STOP（并获取流锁），这会导致死
	 * 锁无法被发现。
	 *
	 * Note that the fileset state can be FS_SERIALIZE_DONE once the leader
	 * worker has finished serializing the changes.
	 *
	 * 注意：leader worker 把变更序列化完之后，文件集状态可以是
	 * FS_SERIALIZE_DONE。
	 */
	if (fileset_state == FS_SERIALIZE_IN_PROGRESS)
	{
		pa_lock_stream(MyParallelShared->xid, AccessShareLock);
		pa_unlock_stream(MyParallelShared->xid, AccessShareLock);

		fileset_state = pa_get_fileset_state();
	}

	/*
	 * We cannot read the file immediately after the leader has serialized all
	 * changes to the file because there may still be messages in the memory
	 * queue. We will apply all spooled messages the next time we call this
	 * function and that will ensure there are no messages left in the memory
	 * queue.
	 *
	 * leader 把全部变更写入文件后不能立刻读该文件，因为内存队列里可能还有
	 * 消息。下次调用本函数时再应用全部已转储消息，这样才能保证内存队列里没
	 * 有残留消息。
	 */
	if (fileset_state == FS_SERIALIZE_DONE)
	{
		pa_set_fileset_state(MyParallelShared, FS_READY);
	}
	else if (fileset_state == FS_READY)
	{
		apply_spooled_messages(&MyParallelShared->fileset,
							   MyParallelShared->xid,
							   InvalidXLogRecPtr);
		pa_set_fileset_state(MyParallelShared, FS_EMPTY);
	}

	return true;
}

/*
 * Interrupt handler for main loop of parallel apply worker.
 *
 * 并行 apply worker 主循环的中断处理函数。
 */
static void
ProcessParallelApplyInterrupts(void)
{
	CHECK_FOR_INTERRUPTS();

	if (ShutdownRequestPending)
	{
		ereport(LOG,
				(errmsg("logical replication parallel apply worker for subscription \"%s\" has finished",
						MySubscription->name)));

		proc_exit(0);
	}

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}
}

/* Parallel apply worker main loop.
 *
 * 并行 apply worker 的主循环。
 */
static void
LogicalParallelApplyLoop(shm_mq_handle *mqh)
{
	shm_mq_result shmq_res;
	ErrorContextCallback errcallback;
	MemoryContext oldcxt = CurrentMemoryContext;

	/*
	 * Init the ApplyMessageContext which we clean up after each replication
	 * protocol message.
	 *
	 * 初始化 ApplyMessageContext，每处理完一条复制协议消息后清理它。
	 */
	ApplyMessageContext = AllocSetContextCreate(ApplyContext,
												"ApplyMessageContext",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * Push apply error context callback. Fields will be filled while applying
	 * a change.
	 *
	 * 压入 apply 错误上下文回调。字段会在应用一条变更时填上。
	 */
	errcallback.callback = apply_error_callback;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (;;)
	{
		void	   *data;
		Size		len;

		ProcessParallelApplyInterrupts();

		/* Ensure we are reading the data into our memory context.
		 *
		 * 确保把数据读进我们自己的内存上下文。
		 */
		MemoryContextSwitchTo(ApplyMessageContext);

		shmq_res = shm_mq_receive(mqh, &len, &data, true);

		if (shmq_res == SHM_MQ_SUCCESS)
		{
			StringInfoData s;
			int			c;

			if (len == 0)
				elog(ERROR, "invalid message length");

			initReadOnlyStringInfo(&s, data, len);

			/*
			 * The first byte of messages sent from leader apply worker to
			 * parallel apply workers can only be 'w'.
			 *
			 * 从 leader apply worker 发给并行 apply worker 的消息，第一个字节只能
			 * 是 w。
			 */
			c = pq_getmsgbyte(&s);
			if (c != 'w')
				elog(ERROR, "unexpected message \"%c\"", c);

			/*
			 * Ignore statistics fields that have been updated by the leader
			 * apply worker.
			 *
			 * 忽略已经由 leader apply worker 更新过的统计字段。
			 *
			 * XXX We can avoid sending the statistics fields from the leader
			 * apply worker but for that, it needs to rebuild the entire
			 * message by removing these fields which could be more work than
			 * simply ignoring these fields in the parallel apply worker.
			 *
			 * XXX 可以不让 leader apply worker 发送这些统计字段，但那就得重建整条
			 * 消息并去掉这些字段，工作量可能比在并行 apply worker 里直接忽略它们还大。
			 */
			s.cursor += SIZE_STATS_MESSAGE;

			apply_dispatch(&s);
		}
		else if (shmq_res == SHM_MQ_WOULD_BLOCK)
		{
			/* Replay the changes from the file, if any.
			 *
			 * 如有变更文件，则从文件重放变更。
			 */
			if (!pa_process_spooled_messages_if_required())
			{
				int			rc;

				/* Wait for more work.
				 *
				 * 等待更多工作。
				 */
				rc = WaitLatch(MyLatch,
							   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							   1000L,
							   WAIT_EVENT_LOGICAL_PARALLEL_APPLY_MAIN);

				if (rc & WL_LATCH_SET)
					ResetLatch(MyLatch);
			}
		}
		else
		{
			Assert(shmq_res == SHM_MQ_DETACHED);

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("lost connection to the logical replication apply worker")));
		}

		MemoryContextReset(ApplyMessageContext);
		MemoryContextSwitchTo(oldcxt);
	}

	/* Pop the error context stack.
	 *
	 * 弹出错误上下文栈。
	 */
	error_context_stack = errcallback.previous;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Make sure the leader apply worker tries to read from our error queue one more
 * time. This guards against the case where we exit uncleanly without sending
 * an ErrorResponse, for example because some code calls proc_exit directly.
 *
 * 确保 leader apply worker 再从我们的错误队列读一次。这是为了防止未发
 * 送 ErrorResponse 就非正常退出的情况，例如某些代码直接调用 proc_exit。
 *
 * Also explicitly detach from dsm segment to invoke on_dsm_detach callbacks,
 * if any. See ParallelWorkerShutdown for details.
 *
 * 同时显式脱离 dsm 段，以便调用 on_dsm_detach 回调（如果有）。详见
 * ParallelWorkerShutdown。
 */
static void
pa_shutdown(int code, Datum arg)
{
	SendProcSignal(MyLogicalRepWorker->leader_pid,
				   PROCSIG_PARALLEL_APPLY_MESSAGE,
				   INVALID_PROC_NUMBER);

	dsm_detach((dsm_segment *) DatumGetPointer(arg));
}

/*
 * Parallel apply worker entry point.
 *
 * 并行 apply worker 的入口。
 */
void
ParallelApplyWorkerMain(Datum main_arg)
{
	ParallelApplyWorkerShared *shared;
	dsm_handle	handle;
	dsm_segment *seg;
	shm_toc    *toc;
	shm_mq	   *mq;
	shm_mq_handle *mqh;
	shm_mq_handle *error_mqh;
	RepOriginId originid;
	int			worker_slot = DatumGetInt32(main_arg);
	char		originname[NAMEDATALEN];

	InitializingApplyWorker = true;

	/*
	 * Setup signal handling.
	 *
	 * 设置信号处理。
	 *
	 * Note: We intentionally used SIGUSR2 to trigger a graceful shutdown
	 * initiated by the leader apply worker. This helps to differentiate it
	 * from the case where we abort the current transaction and exit on
	 * receiving SIGTERM.
	 *
	 * 注意：这里有意用 SIGUSR2 触发由 leader apply worker 发起的优雅关闭。
	 * 这样就能把它和收到 SIGTERM 时中止当前事务并退出的情况区分开。
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);
	BackgroundWorkerUnblockSignals();

	/*
	 * Attach to the dynamic shared memory segment for the parallel apply, and
	 * find its table of contents.
	 *
	 * 挂上并行 apply 使用的动态共享内存段，并找到它的目录。
	 *
	 * Like parallel query, we don't need resource owner by this time. See
	 * ParallelWorkerMain.
	 *
	 * 与并行查询一样，此时还不需要 resource owner。见 ParallelWorkerMain。
	 */
	memcpy(&handle, MyBgworkerEntry->bgw_extra, sizeof(dsm_handle));
	seg = dsm_attach(handle);
	if (!seg)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));

	toc = shm_toc_attach(PG_LOGICAL_APPLY_SHM_MAGIC, dsm_segment_address(seg));
	if (!toc)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid magic number in dynamic shared memory segment")));

	/* Look up the shared information.
	 *
	 * 查找共享信息。
	 */
	shared = shm_toc_lookup(toc, PARALLEL_APPLY_KEY_SHARED, false);
	MyParallelShared = shared;

	/*
	 * Attach to the message queue.
	 *
	 * 挂上消息队列。
	 */
	mq = shm_toc_lookup(toc, PARALLEL_APPLY_KEY_MQ, false);
	shm_mq_set_receiver(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);

	/*
	 * Primary initialization is complete. Now, we can attach to our slot.
	 * This is to ensure that the leader apply worker does not write data to
	 * the uninitialized memory queue.
	 *
	 * 主要初始化已经完成。现在可以挂上自己的槽位。这样 leader apply worker
	 * 就不会把数据写进尚未初始化的内存队列。
	 */
	logicalrep_worker_attach(worker_slot);

	/*
	 * Register the shutdown callback after we are attached to the worker
	 * slot. This is to ensure that MyLogicalRepWorker remains valid when this
	 * callback is invoked.
	 *
	 * 在挂上 worker 槽位之后注册关闭回调。这样在回调被调用时，
	 * MyLogicalRepWorker 仍然有效。
	 */
	before_shmem_exit(pa_shutdown, PointerGetDatum(seg));

	SpinLockAcquire(&MyParallelShared->mutex);
	MyParallelShared->logicalrep_worker_generation = MyLogicalRepWorker->generation;
	MyParallelShared->logicalrep_worker_slot_no = worker_slot;
	SpinLockRelease(&MyParallelShared->mutex);

	/*
	 * Attach to the error queue.
	 *
	 * 挂上错误队列。
	 */
	mq = shm_toc_lookup(toc, PARALLEL_APPLY_KEY_ERROR_QUEUE, false);
	shm_mq_set_sender(mq, MyProc);
	error_mqh = shm_mq_attach(mq, seg, NULL);

	pq_redirect_to_shm_mq(seg, error_mqh);
	pq_set_parallel_leader(MyLogicalRepWorker->leader_pid,
						   INVALID_PROC_NUMBER);

	MyLogicalRepWorker->last_send_time = MyLogicalRepWorker->last_recv_time =
		MyLogicalRepWorker->reply_time = 0;

	InitializeLogRepWorker();

	InitializingApplyWorker = false;

	/* Setup replication origin tracking.
	 *
	 * 设置复制源跟踪。
	 */
	StartTransactionCommand();
	ReplicationOriginNameForLogicalRep(MySubscription->oid, InvalidOid,
									   originname, sizeof(originname));
	originid = replorigin_by_name(originname, false);

	/*
	 * The parallel apply worker doesn't need to monopolize this replication
	 * origin which was already acquired by its leader process.
	 *
	 * 并行 apply worker 不必独占这个复制源，它已经由其 leader 进程获取。
	 */
	replorigin_session_setup(originid, MyLogicalRepWorker->leader_pid);
	replorigin_session_origin = originid;
	CommitTransactionCommand();

	/*
	 * Setup callback for syscache so that we know when something changes in
	 * the subscription relation state.
	 *
	 * 为系统缓存注册回调，以便在订阅关系状态发生变化时得到通知。
	 */
	CacheRegisterSyscacheCallback(SUBSCRIPTIONRELMAP,
								  invalidate_syncing_table_states,
								  (Datum) 0);

	set_apply_error_context_origin(originname);

	LogicalParallelApplyLoop(mqh);

	/*
	 * The parallel apply worker must not get here because the parallel apply
	 * worker will only stop when it receives a SIGTERM or SIGUSR2 from the
	 * leader, or SIGINT from itself, or when there is an error. None of these
	 * cases will allow the code to reach here.
	 *
	 * 并行 apply worker 不应该执行到这里。它只会在收到 leader 发来的
	 * SIGTERM 或 SIGUSR2、自己发出的 SIGINT，或发生错误时停止。这些情况都
	 * 不会让代码走到这里。
	 */
	Assert(false);
}

/*
 * Handle receipt of an interrupt indicating a parallel apply worker message.
 *
 * 处理表示并行 apply worker 有消息到来的中断。
 *
 * Note: this is called within a signal handler! All we can do is set a flag
 * that will cause the next CHECK_FOR_INTERRUPTS() to invoke
 * ProcessParallelApplyMessages().
 *
 * 注意：此函数在信号处理函数里被调用。这里只能置一个标志，让下一次
 * CHECK_FOR_INTERRUPTS() 去调用 ProcessParallelApplyMessages()。
 */
void
HandleParallelApplyMessageInterrupt(void)
{
	InterruptPending = true;
	ParallelApplyMessagePending = true;
	SetLatch(MyLatch);
}

/*
 * Process a single protocol message received from a single parallel apply
 * worker.
 *
 * 处理从某一个并行 apply worker 收到的单条协议消息。
 */
static void
ProcessParallelApplyMessage(StringInfo msg)
{
	char		msgtype;

	msgtype = pq_getmsgbyte(msg);

	switch (msgtype)
	{
		case 'E':				/* ErrorResponse
								 *
								 * 错误响应
								 */
			{
				ErrorData	edata;

				/* Parse ErrorResponse.
				 *
				 * 解析 ErrorResponse。
				 */
				pq_parse_errornotice(msg, &edata);

				/*
				 * If desired, add a context line to show that this is a
				 * message propagated from a parallel apply worker. Otherwise,
				 * it can sometimes be confusing to understand what actually
				 * happened.
				 *
				 * 如果需要，加一行上下文，表明这是从并行 apply worker 传上来的消息。否
				 * 则有时会搞不清实际发生了什么。
				 */
				if (edata.context)
					edata.context = psprintf("%s\n%s", edata.context,
											 _("logical replication parallel apply worker"));
				else
					edata.context = pstrdup(_("logical replication parallel apply worker"));

				/*
				 * Context beyond that should use the error context callbacks
				 * that were in effect in LogicalRepApplyLoop().
				 *
				 * 除此之外的上下文应使用 LogicalRepApplyLoop() 当时生效的错误上下文回调。
				 */
				error_context_stack = apply_error_context_stack;

				/*
				 * The actual error must have been reported by the parallel
				 * apply worker.
				 *
				 * 实际错误必须已经由并行 apply worker 报告过。
				 */
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("logical replication parallel apply worker exited due to error"),
						 errcontext("%s", edata.context)));
			}

			/*
			 * Don't need to do anything about NoticeResponse and
			 * NotifyResponse as the logical replication worker doesn't need
			 * to send messages to the client.
			 *
			 * 不必处理 NoticeResponse 和 NotifyResponse，因为逻辑复制 worker 不需
			 * 要向客户端发送消息。
			 */
		case 'N':
		case 'A':
			break;

		default:
			elog(ERROR, "unrecognized message type received from logical replication parallel apply worker: %c (message length %d bytes)",
				 msgtype, msg->len);
	}
}

/*
 * Handle any queued protocol messages received from parallel apply workers.
 *
 * 处理从并行 apply worker 收到的、已排队的协议消息。
 */
void
ProcessParallelApplyMessages(void)
{
	ListCell   *lc;
	MemoryContext oldcontext;

	static MemoryContext hpam_context = NULL;

	/*
	 * This is invoked from ProcessInterrupts(), and since some of the
	 * functions it calls contain CHECK_FOR_INTERRUPTS(), there is a potential
	 * for recursive calls if more signals are received while this runs. It's
	 * unclear that recursive entry would be safe, and it doesn't seem useful
	 * even if it is safe, so let's block interrupts until done.
	 *
	 * 本函数由 ProcessInterrupts() 调用。它调用的某些函数里含有
	 * CHECK_FOR_INTERRUPTS()，因此运行期间若再收到信号，有可能递归进入。递
	 * 归进入是否安全并不清楚，即便安全似乎也没有用处，所以做完之前先屏蔽中断。
	 */
	HOLD_INTERRUPTS();

	/*
	 * Moreover, CurrentMemoryContext might be pointing almost anywhere. We
	 * don't want to risk leaking data into long-lived contexts, so let's do
	 * our work here in a private context that we can reset on each use.
	 *
	 * 此外，CurrentMemoryContext 可能指向几乎任何地方。为避免把数据泄漏进
	 * 长寿命的上下文，这里在一个每次使用都可以重置的私有上下文中工作。
	 */
	if (!hpam_context)			/* first time through?
								 *
								 * 第一次进入？
								 */
		hpam_context = AllocSetContextCreate(TopMemoryContext,
											 "ProcessParallelApplyMessages",
											 ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(hpam_context);

	oldcontext = MemoryContextSwitchTo(hpam_context);

	ParallelApplyMessagePending = false;

	foreach(lc, ParallelApplyWorkerPool)
	{
		shm_mq_result res;
		Size		nbytes;
		void	   *data;
		ParallelApplyWorkerInfo *winfo = (ParallelApplyWorkerInfo *) lfirst(lc);

		/*
		 * The leader will detach from the error queue and set it to NULL
		 * before preparing to stop all parallel apply workers, so we don't
		 * need to handle error messages anymore. See
		 * logicalrep_worker_detach.
		 *
		 * leader 在准备停止所有并行 apply worker 之前会脱离错误队列并将其置为
		 * NULL，因此不必再处理错误消息。见 logicalrep_worker_detach。
		 */
		if (!winfo->error_mq_handle)
			continue;

		res = shm_mq_receive(winfo->error_mq_handle, &nbytes, &data, true);

		if (res == SHM_MQ_WOULD_BLOCK)
			continue;
		else if (res == SHM_MQ_SUCCESS)
		{
			StringInfoData msg;

			initStringInfo(&msg);
			appendBinaryStringInfo(&msg, data, nbytes);
			ProcessParallelApplyMessage(&msg);
			pfree(msg.data);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("lost connection to the logical replication parallel apply worker")));
	}

	MemoryContextSwitchTo(oldcontext);

	/* Might as well clear the context on our way out
	 *
	 * 离开时不妨把该上下文清掉
	 */
	MemoryContextReset(hpam_context);

	RESUME_INTERRUPTS();
}

/*
 * Send the data to the specified parallel apply worker via shared-memory
 * queue.
 *
 * 经共享内存队列把数据发给指定的并行 apply worker。
 *
 * Returns false if the attempt to send data via shared memory times out, true
 * otherwise.
 *
 * 若经共享内存发送数据超时则返回 false，否则返回 true。
 */
bool
pa_send_data(ParallelApplyWorkerInfo *winfo, Size nbytes, const void *data)
{
	int			rc;
	shm_mq_result result;
	TimestampTz startTime = 0;

	Assert(!IsTransactionState());
	Assert(!winfo->serialize_changes);

	/*
	 * We don't try to send data to parallel worker for 'immediate' mode. This
	 * is primarily used for testing purposes.
	 *
	 * 在 immediate 模式下不尝试把数据发给并行 worker。这主要用于测试。
	 */
	if (unlikely(debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE))
		return false;

/*
 * This timeout is a bit arbitrary but testing revealed that it is sufficient
 * to send the message unless the parallel apply worker is waiting on some
 * lock or there is a serious resource crunch. See the comments atop this file
 * to know why we are using a non-blocking way to send the message.
 *
 * 这个超时有些随意，但测试表明，除非并行 apply worker 正在等某把锁，或
 * 资源严重紧张，否则它足够把消息发出去。为何用非阻塞方式发送，见本文件
 * 头部注释。
 */
#define SHM_SEND_RETRY_INTERVAL_MS 1000
#define SHM_SEND_TIMEOUT_MS		(10000 - SHM_SEND_RETRY_INTERVAL_MS)

	for (;;)
	{
		result = shm_mq_send(winfo->mq_handle, nbytes, data, true, true);

		if (result == SHM_MQ_SUCCESS)
			return true;
		else if (result == SHM_MQ_DETACHED)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not send data to shared-memory queue")));

		Assert(result == SHM_MQ_WOULD_BLOCK);

		/* Wait before retrying.
		 *
		 * 重试前先等待。
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   SHM_SEND_RETRY_INTERVAL_MS,
					   WAIT_EVENT_LOGICAL_APPLY_SEND_DATA);

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}

		if (startTime == 0)
			startTime = GetCurrentTimestamp();
		else if (TimestampDifferenceExceeds(startTime, GetCurrentTimestamp(),
											SHM_SEND_TIMEOUT_MS))
			return false;
	}
}

/*
 * Switch to PARTIAL_SERIALIZE mode for the current transaction -- this means
 * that the current data and any subsequent data for this transaction will be
 * serialized to a file. This is done to prevent possible deadlocks with
 * another parallel apply worker (refer to the comments atop this file).
 *
 * 把当前事务切换到 PARTIAL_SERIALIZE 模式，也就是当前这条数据以及此后
 * 该事务的数据都会序列化到文件。这样做是为了避免与另一个并行 apply
 * worker 发生死锁（见本文件头部注释）。
 */
void
pa_switch_to_partial_serialize(ParallelApplyWorkerInfo *winfo,
							   bool stream_locked)
{
	ereport(LOG,
			(errmsg("logical replication apply worker will serialize the remaining changes of remote transaction %u to a file",
					winfo->shared->xid)));

	/*
	 * The parallel apply worker could be stuck for some reason (say waiting
	 * on some lock by other backend), so stop trying to send data directly to
	 * it and start serializing data to the file instead.
	 *
	 * 并行 apply worker 可能因某种原因卡住（例如在等其他后端持有的锁），因
	 * 此不再尝试直接把数据发给它，改为把数据序列化到文件。
	 */
	winfo->serialize_changes = true;

	/* Initialize the stream fileset.
	 *
	 * 初始化流文件集。
	 */
	stream_start_internal(winfo->shared->xid, true);

	/*
	 * Acquires the stream lock if not already to make sure that the parallel
	 * apply worker will wait for the leader to release the stream lock until
	 * the end of the transaction.
	 *
	 * 若尚未持有流锁则获取它，以确保并行 apply worker 会一直等到 leader 在
	 * 事务结束时释放流锁。
	 */
	if (!stream_locked)
		pa_lock_stream(winfo->shared->xid, AccessExclusiveLock);

	pa_set_fileset_state(winfo->shared, FS_SERIALIZE_IN_PROGRESS);
}

/*
 * Wait until the parallel apply worker's transaction state has reached or
 * exceeded the given xact_state.
 *
 * 等待，直到并行 apply worker 的事务状态达到或超过给定的 xact_state。
 */
static void
pa_wait_for_xact_state(ParallelApplyWorkerInfo *winfo,
					   ParallelTransState xact_state)
{
	for (;;)
	{
		/*
		 * Stop if the transaction state has reached or exceeded the given
		 * xact_state.
		 *
		 * 若事务状态已达到或超过给定的 xact_state，则停止等待。
		 */
		if (pa_get_xact_state(winfo->shared) >= xact_state)
			break;

		/* Wait to be signalled.
		 *
		 * 等待被唤醒。
		 */
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L,
						 WAIT_EVENT_LOGICAL_PARALLEL_APPLY_STATE_CHANGE);

		/* Reset the latch so we don't spin.
		 *
		 * 重置 latch，避免空转。
		 */
		ResetLatch(MyLatch);

		/* An interrupt may have occurred while we were waiting.
		 *
		 * 等待期间可能已经发生中断。
		 */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Wait until the parallel apply worker's transaction finishes.
 *
 * 等待并行 apply worker 的事务结束。
 */
static void
pa_wait_for_xact_finish(ParallelApplyWorkerInfo *winfo)
{
	/*
	 * Wait until the parallel apply worker set the state to
	 * PARALLEL_TRANS_STARTED which means it has acquired the transaction
	 * lock. This is to prevent leader apply worker from acquiring the
	 * transaction lock earlier than the parallel apply worker.
	 *
	 * 一直等到并行 apply worker 把状态设为 PARALLEL_TRANS_STARTED，这表示
	 * 它已经拿到事务锁。这样可以防止 leader apply worker 比并行 apply
	 * worker 更早拿到事务锁。
	 */
	pa_wait_for_xact_state(winfo, PARALLEL_TRANS_STARTED);

	/*
	 * Wait for the transaction lock to be released. This is required to
	 * detect deadlock among leader and parallel apply workers. Refer to the
	 * comments atop this file.
	 *
	 * 等待事务锁被释放。这是为了检测 leader 与并行 apply worker 之间的死锁。
	 * 见本文件头部注释。
	 */
	pa_lock_transaction(winfo->shared->xid, AccessShareLock);
	pa_unlock_transaction(winfo->shared->xid, AccessShareLock);

	/*
	 * Check if the state becomes PARALLEL_TRANS_FINISHED in case the parallel
	 * apply worker failed while applying changes causing the lock to be
	 * released.
	 *
	 * 检查状态是否变为 PARALLEL_TRANS_FINISHED，以防并行 apply worker 在应
	 * 用变更时失败而导致锁被释放。
	 */
	if (pa_get_xact_state(winfo->shared) != PARALLEL_TRANS_FINISHED)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lost connection to the logical replication parallel apply worker")));
}

/*
 * Set the transaction state for a given parallel apply worker.
 *
 * 设置指定并行 apply worker 的事务状态。
 */
void
pa_set_xact_state(ParallelApplyWorkerShared *wshared,
				  ParallelTransState xact_state)
{
	SpinLockAcquire(&wshared->mutex);
	wshared->xact_state = xact_state;
	SpinLockRelease(&wshared->mutex);
}

/*
 * Get the transaction state for a given parallel apply worker.
 *
 * 取得指定并行 apply worker 的事务状态。
 */
static ParallelTransState
pa_get_xact_state(ParallelApplyWorkerShared *wshared)
{
	ParallelTransState xact_state;

	SpinLockAcquire(&wshared->mutex);
	xact_state = wshared->xact_state;
	SpinLockRelease(&wshared->mutex);

	return xact_state;
}

/*
 * Cache the parallel apply worker information.
 *
 * 缓存并行 apply worker 的信息。
 */
void
pa_set_stream_apply_worker(ParallelApplyWorkerInfo *winfo)
{
	stream_apply_worker = winfo;
}

/*
 * Form a unique savepoint name for the streaming transaction.
 *
 * 为流式事务生成唯一的保存点名称。
 *
 * Note that different subscriptions for publications on different nodes can
 * receive same remote xid, so we need to use subscription id along with it.
 *
 * 注意：不同节点上的发布所对应的不同订阅可能收到相同的远程 xid，因此需
 * 要连同订阅 id 一起使用。
 *
 * Returns the name in the supplied buffer.
 *
 * 把名称写入调用方提供的缓冲区。
 */
static void
pa_savepoint_name(Oid suboid, TransactionId xid, char *spname, Size szsp)
{
	snprintf(spname, szsp, "pg_sp_%u_%u", suboid, xid);
}

/*
 * Define a savepoint for a subxact in parallel apply worker if needed.
 *
 * 如有需要，在并行 apply worker 里为子事务定义保存点。
 *
 * The parallel apply worker can figure out if a new subtransaction was
 * started by checking if the new change arrived with a different xid. In that
 * case define a named savepoint, so that we are able to rollback to it
 * if required.
 *
 * 并行 apply worker 可以根据新变更是否带着不同的 xid，判断是否开始了新
 * 的子事务。若是，则定义一个命名保存点，以便在需要时回滚到那里。
 */
void
pa_start_subtrans(TransactionId current_xid, TransactionId top_xid)
{
	if (current_xid != top_xid &&
		!list_member_xid(subxactlist, current_xid))
	{
		MemoryContext oldctx;
		char		spname[NAMEDATALEN];

		pa_savepoint_name(MySubscription->oid, current_xid,
						  spname, sizeof(spname));

		elog(DEBUG1, "defining savepoint %s in logical replication parallel apply worker", spname);

		/* We must be in transaction block to define the SAVEPOINT.
		 *
		 * 定义 SAVEPOINT 时必须处于事务块中。
		 */
		if (!IsTransactionBlock())
		{
			if (!IsTransactionState())
				StartTransactionCommand();

			BeginTransactionBlock();
			CommitTransactionCommand();
		}

		DefineSavepoint(spname);

		/*
		 * CommitTransactionCommand is needed to start a subtransaction after
		 * issuing a SAVEPOINT inside a transaction block (see
		 * StartSubTransaction()).
		 *
		 * 在事务块内发出 SAVEPOINT 之后，需要 CommitTransactionCommand 才能真
		 * 正开始子事务（见 StartSubTransaction()）。
		 */
		CommitTransactionCommand();

		oldctx = MemoryContextSwitchTo(TopTransactionContext);
		subxactlist = lappend_xid(subxactlist, current_xid);
		MemoryContextSwitchTo(oldctx);
	}
}

/* Reset the list that maintains subtransactions.
 *
 * 重置维护子事务的列表。
 */
void
pa_reset_subtrans(void)
{
	/*
	 * We don't need to free this explicitly as the allocated memory will be
	 * freed at the transaction end.
	 *
	 * 不必显式释放这块内存，事务结束时会把它释放掉。
	 */
	subxactlist = NIL;
}

/*
 * Handle STREAM ABORT message when the transaction was applied in a parallel
 * apply worker.
 *
 * 在事务由并行 apply worker 应用时，处理 STREAM ABORT 消息。
 */
void
pa_stream_abort(LogicalRepStreamAbortData *abort_data)
{
	TransactionId xid = abort_data->xid;
	TransactionId subxid = abort_data->subxid;

	/*
	 * Update origin state so we can restart streaming from correct position
	 * in case of crash.
	 *
	 * 更新 origin 状态，以便崩溃后能从正确位置重新开始流式传输。
	 */
	replorigin_session_origin_lsn = abort_data->abort_lsn;
	replorigin_session_origin_timestamp = abort_data->abort_time;

	/*
	 * If the two XIDs are the same, it's in fact abort of toplevel xact, so
	 * just free the subxactlist.
	 *
	 * 若两个 XID 相同，实际上是顶层事务的中止，因此只需清空 subxactlist。
	 */
	if (subxid == xid)
	{
		pa_set_xact_state(MyParallelShared, PARALLEL_TRANS_FINISHED);

		/*
		 * Release the lock as we might be processing an empty streaming
		 * transaction in which case the lock won't be released during
		 * transaction rollback.
		 *
		 * 释放该锁。因为我们可能正在处理一个空的流式事务，这种情况下回滚事务时
		 * 不会释放该锁。
		 *
		 * Note that it's ok to release the transaction lock before aborting
		 * the transaction because even if the parallel apply worker dies due
		 * to crash or some other reason, such a transaction would still be
		 * considered aborted.
		 *
		 * 注意：在中止事务之前释放事务锁是可以的。即使并行 apply worker 因崩溃
		 * 或其他原因退出，这样的事务仍然会被视为已中止。
		 */
		pa_unlock_transaction(xid, AccessExclusiveLock);

		AbortCurrentTransaction();

		if (IsTransactionBlock())
		{
			EndTransactionBlock(false);
			CommitTransactionCommand();
		}

		pa_reset_subtrans();

		pgstat_report_activity(STATE_IDLE, NULL);
	}
	else
	{
		/* OK, so it's a subxact. Rollback to the savepoint.
		 *
		 * 这是子事务。回滚到保存点。
		 */
		int			i;
		char		spname[NAMEDATALEN];

		pa_savepoint_name(MySubscription->oid, subxid, spname, sizeof(spname));

		elog(DEBUG1, "rolling back to savepoint %s in logical replication parallel apply worker", spname);

		/*
		 * Search the subxactlist, determine the offset tracked for the
		 * subxact, and truncate the list.
		 *
		 * 在 subxactlist 中查找，确定为该子事务记录的偏移，并截断列表。
		 *
		 * Note that for an empty sub-transaction we won't find the subxid
		 * here.
		 *
		 * 注意：空的子事务在这里找不到 subxid。
		 */
		for (i = list_length(subxactlist) - 1; i >= 0; i--)
		{
			TransactionId xid_tmp = lfirst_xid(list_nth_cell(subxactlist, i));

			if (xid_tmp == subxid)
			{
				RollbackToSavepoint(spname);
				CommitTransactionCommand();
				subxactlist = list_truncate(subxactlist, i);
				break;
			}
		}
	}
}

/*
 * Set the fileset state for a particular parallel apply worker. The fileset
 * will be set once the leader worker serialized all changes to the file
 * so that it can be used by parallel apply worker.
 *
 * 设置某个并行 apply worker 的文件集状态。leader worker 把全部变更序列
 * 化到文件之后会设置该文件集，供并行 apply worker 使用。
 */
void
pa_set_fileset_state(ParallelApplyWorkerShared *wshared,
					 PartialFileSetState fileset_state)
{
	SpinLockAcquire(&wshared->mutex);
	wshared->fileset_state = fileset_state;

	if (fileset_state == FS_SERIALIZE_DONE)
	{
		Assert(am_leader_apply_worker());
		Assert(MyLogicalRepWorker->stream_fileset);
		wshared->fileset = *MyLogicalRepWorker->stream_fileset;
	}

	SpinLockRelease(&wshared->mutex);
}

/*
 * Get the fileset state for the current parallel apply worker.
 *
 * 取得当前并行 apply worker 的文件集状态。
 */
static PartialFileSetState
pa_get_fileset_state(void)
{
	PartialFileSetState fileset_state;

	Assert(am_parallel_apply_worker());

	SpinLockAcquire(&MyParallelShared->mutex);
	fileset_state = MyParallelShared->fileset_state;
	SpinLockRelease(&MyParallelShared->mutex);

	return fileset_state;
}

/*
 * Helper functions to acquire and release a lock for each stream block.
 *
 * 为每个流块获取和释放锁的辅助函数。
 *
 * Set locktag_field4 to PARALLEL_APPLY_LOCK_STREAM to indicate that it's a
 * stream lock.
 *
 * 把 locktag_field4 设为 PARALLEL_APPLY_LOCK_STREAM，表示这是流锁。
 *
 * Refer to the comments atop this file to see how the stream lock is used.
 *
 * 流锁如何使用，见本文件头部注释。
 */
void
pa_lock_stream(TransactionId xid, LOCKMODE lockmode)
{
	LockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
								   PARALLEL_APPLY_LOCK_STREAM, lockmode);
}

/*
 * 释放流块上的会话锁。
 */
void
pa_unlock_stream(TransactionId xid, LOCKMODE lockmode)
{
	UnlockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
									 PARALLEL_APPLY_LOCK_STREAM, lockmode);
}

/*
 * Helper functions to acquire and release a lock for each local transaction
 * apply.
 *
 * 为每次本地事务应用获取和释放锁的辅助函数。
 *
 * Set locktag_field4 to PARALLEL_APPLY_LOCK_XACT to indicate that it's a
 * transaction lock.
 *
 * 把 locktag_field4 设为 PARALLEL_APPLY_LOCK_XACT，表示这是事务锁。
 *
 * Note that all the callers must pass a remote transaction ID instead of a
 * local transaction ID as xid. This is because the local transaction ID will
 * only be assigned while applying the first change in the parallel apply but
 * it's possible that the first change in the parallel apply worker is blocked
 * by a concurrently executing transaction in another parallel apply worker. We
 * can only communicate the local transaction id to the leader after applying
 * the first change so it won't be able to wait after sending the xact finish
 * command using this lock.
 *
 * 注意：所有调用方传入的 xid 必须是远程事务 ID，而不是本地事务 ID。因
 * 为本地事务 ID 要到并行 apply 应用第一条变更时才会分配，而并行 apply
 * worker 的第一条变更可能被另一个并行 apply worker 中并发执行的事务堵
 * 住。只有应用完第一条变更之后才能把本地事务 ID 告诉 leader，因此
 * leader 在发出事务结束命令后无法用这把锁来等待。
 *
 * Refer to the comments atop this file to see how the transaction lock is
 * used.
 *
 * 事务锁如何使用，见本文件头部注释。
 */
void
pa_lock_transaction(TransactionId xid, LOCKMODE lockmode)
{
	LockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
								   PARALLEL_APPLY_LOCK_XACT, lockmode);
}

/*
 * 释放正在应用的事务上的会话锁。
 */
void
pa_unlock_transaction(TransactionId xid, LOCKMODE lockmode)
{
	UnlockApplyTransactionForSession(MyLogicalRepWorker->subid, xid,
									 PARALLEL_APPLY_LOCK_XACT, lockmode);
}

/*
 * Decrement the number of pending streaming blocks and wait on the stream lock
 * if there is no pending block available.
 *
 * 减少待处理的流块数量；若已经没有待处理的流块，则在流锁上等待。
 */
void
pa_decr_and_wait_stream_block(void)
{
	Assert(am_parallel_apply_worker());

	/*
	 * It is only possible to not have any pending stream chunks when we are
	 * applying spooled messages.
	 *
	 * 只有在应用已转储的消息时，才可能没有任何待处理的流块。
	 */
	if (pg_atomic_read_u32(&MyParallelShared->pending_stream_count) == 0)
	{
		if (pa_has_spooled_message_pending())
			return;

		elog(ERROR, "invalid pending streaming chunk 0");
	}

	if (pg_atomic_sub_fetch_u32(&MyParallelShared->pending_stream_count, 1) == 0)
	{
		pa_lock_stream(MyParallelShared->xid, AccessShareLock);
		pa_unlock_stream(MyParallelShared->xid, AccessShareLock);
	}
}

/*
 * Finish processing the streaming transaction in the leader apply worker.
 *
 * 在 leader apply worker 中结束对流式事务的处理。
 */
void
pa_xact_finish(ParallelApplyWorkerInfo *winfo, XLogRecPtr remote_lsn)
{
	Assert(am_leader_apply_worker());

	/*
	 * Unlock the shared object lock so that parallel apply worker can
	 * continue to receive and apply changes.
	 *
	 * 释放共享对象锁，使并行 apply worker 可以继续接收并应用变更。
	 */
	pa_unlock_stream(winfo->shared->xid, AccessExclusiveLock);

	/*
	 * Wait for that worker to finish. This is necessary to maintain commit
	 * order which avoids failures due to transaction dependencies and
	 * deadlocks.
	 *
	 * 等待该 worker 结束。这样才能保持提交顺序，避免因事务依赖和死锁而失败。
	 */
	pa_wait_for_xact_finish(winfo);

	if (!XLogRecPtrIsInvalid(remote_lsn))
		store_flush_position(remote_lsn, winfo->shared->last_commit_end);

	pa_free_worker(winfo);
}
