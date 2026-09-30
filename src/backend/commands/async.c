/*-------------------------------------------------------------------------
 *
 * async.c
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * 异步通知：NOTIFY、LISTEN、UNLISTEN。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/async.c
 *
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Async Notification Model as of 9.0:
 *
 * 自 9.0 起的异步通知模型：
 *
 * 1. Multiple backends on same machine. Multiple backends listening on
 *	  several channels. (Channels are also called "conditions" in other
 *	  parts of the code.)
 *
 * 1. 同一台机器上有多个后端。多个后端监听若干 channel。
 * （channel 在代码其他地方也称为 condition。）
 *
 * 2. There is one central queue in disk-based storage (directory pg_notify/),
 *	  with actively-used pages mapped into shared memory by the slru.c module.
 *	  All notification messages are placed in the queue and later read out
 *	  by listening backends.
 *
 * 2. 磁盘上有一个中央队列（目录 pg_notify/），
 * 正被使用的页由 slru.c 映射进共享内存。
 * 所有通知消息先放入队列，再由正在监听的后端读出。
 *
 *	  There is no central knowledge of which backend listens on which channel;
 *	  every backend has its own list of interesting channels.
 *
 * 没有集中记录哪个后端在听哪个 channel；每个后端各自保存感兴趣的 channel 列表。
 *
 *	  Although there is only one queue, notifications are treated as being
 *	  database-local; this is done by including the sender's database OID
 *	  in each notification message.  Listening backends ignore messages
 *	  that don't match their database OID.  This is important because it
 *	  ensures senders and receivers have the same database encoding and won't
 *	  misinterpret non-ASCII text in the channel name or payload string.
 *
 * 虽然只有一个队列，通知仍按数据库局部处理：每条消息带上发送方的数据库 OID。
 * 监听后端忽略数据库 OID 不匹配的消息。这很重要，因为它保证收发双方编码一致，
 * 不会误解 channel 名或 payload 中的非 ASCII 文本。
 *
 *	  Since notifications are not expected to survive database crashes,
 *	  we can simply clean out the pg_notify data at any reboot, and there
 *	  is no need for WAL support or fsync'ing.
 *
 * 通知不需要在数据库崩溃后保留，因此任何重启都可以直接清掉 pg_notify 数据，
 * 不需要 WAL，也不需要 fsync。
 *
 * 3. Every backend that is listening on at least one channel registers by
 *	  entering its PID into the array in AsyncQueueControl. It then scans all
 *	  incoming notifications in the central queue and first compares the
 *	  database OID of the notification with its own database OID and then
 *	  compares the notified channel with the list of channels that it listens
 *	  to. In case there is a match it delivers the notification event to its
 *	  frontend.  Non-matching events are simply skipped.
 *
 * 3. 至少监听一个 channel 的后端把自己的 PID 登记进 AsyncQueueControl 数组。
 * 然后扫描中央队列中的新通知：先比较数据库 OID，再与自己监听的 channel 列表比较。
 * 匹配则把事件交给前端；不匹配则直接跳过。
 *
 * 4. The NOTIFY statement (routine Async_Notify) stores the notification in
 *	  a backend-local list which will not be processed until transaction end.
 *
 * 4. NOTIFY 语句（例程 Async_Notify）把通知放进后端本地列表，直到事务结束才处理。
 *
 *	  Duplicate notifications from the same transaction are sent out as one
 *	  notification only. This is done to save work when for example a trigger
 *	  on a 2 million row table fires a notification for each row that has been
 *	  changed. If the application needs to receive every single notification
 *	  that has been sent, it can easily add some unique string into the extra
 *	  payload parameter.
 *
 * 同一事务中的重复通知只发送一次。例如一张 200 万行的表上，触发器为每一行变更都发通知时，这样可以省工作。
 * 若应用需要收到每一次通知，可以在额外的 payload 参数里放一个唯一字符串。
 *
 *	  When the transaction is ready to commit, PreCommit_Notify() adds the
 *	  pending notifications to the head of the queue. The head pointer of the
 *	  queue always points to the next free position and a position is just a
 *	  page number and the offset in that page. This is done before marking the
 *	  transaction as committed in clog. If we run into problems writing the
 *	  notifications, we can still call elog(ERROR, ...) and the transaction
 *	  will roll back.
 *
 * 事务准备提交时，PreCommit_Notify() 把待发送通知加到队列头部。
 * 队列头指针始终指向下一个空闲位置，位置就是页号加页内偏移。
 * 这发生在把事务标为已提交写入 clog 之前。若写通知时出问题，仍可 elog(ERROR, ...) 并回滚事务。
 *
 *	  Once we have put all of the notifications into the queue, we return to
 *	  CommitTransaction() which will then do the actual transaction commit.
 *
 * 把所有通知放入队列后，返回 CommitTransaction()，由它完成真正的事务提交。
 *
 *	  After commit we are called another time (AtCommit_Notify()). Here we
 *	  make any actual updates to the effective listen state (listenChannels).
 *	  Then we signal any backends that may be interested in our messages
 *	  (including our own backend, if listening).  This is done by
 *	  SignalBackends(), which scans the list of listening backends and sends a
 *	  PROCSIG_NOTIFY_INTERRUPT signal to every listening backend (we don't
 *	  know which backend is listening on which channel so we must signal them
 *	  all).  We can exclude backends that are already up to date, though, and
 *	  we can also exclude backends that are in other databases (unless they
 *	  are way behind and should be kicked to make them advance their
 *	  pointers).
 *
 * 提交之后会再次被调用（AtCommit_Notify()）。这里更新实际的监听状态（listenChannels），
 * 然后向可能感兴趣的后端发信号（若自己也在监听，则包括自己）。
 * SignalBackends() 扫描监听后端列表，向每个监听后端发送 PROCSIG_NOTIFY_INTERRUPT
 * （不知道谁在听哪个 channel，所以必须全部通知）。
 * 已经跟上的后端可以排除；其他数据库中的后端也可以排除
 * （除非它们落后太多，需要唤醒以推进指针）。
 *
 *	  Finally, after we are out of the transaction altogether and about to go
 *	  idle, we scan the queue for messages that need to be sent to our
 *	  frontend (which might be notifies from other backends, or self-notifies
 *	  from our own).  This step is not part of the CommitTransaction sequence
 *	  for two important reasons.  First, we could get errors while sending
 *	  data to our frontend, and it's really bad for errors to happen in
 *	  post-commit cleanup.  Second, in cases where a procedure issues commits
 *	  within a single frontend command, we don't want to send notifies to our
 *	  frontend until the command is done; but notifies to other backends
 *	  should go out immediately after each commit.
 *
 * 最后，完全离开事务、即将空闲时，扫描队列中需要发给自己前端的消息
 * （可能来自其他后端，也可能是自己的 self-notify）。
 * 这一步不属于 CommitTransaction 序列，有两个重要原因。
 * 第一，向前端发送数据时可能出错，而在提交后的清理阶段出错非常糟糕。
 * 第二，若一个过程在单条前端命令里多次提交，我们希望等命令结束再把通知发给自己的前端；
 * 发给其他后端的通知则应在每次提交后立即送出。
 *
 * 5. Upon receipt of a PROCSIG_NOTIFY_INTERRUPT signal, the signal handler
 *	  sets the process's latch, which triggers the event to be processed
 *	  immediately if this backend is idle (i.e., it is waiting for a frontend
 *	  command and is not within a transaction block. C.f.
 *	  ProcessClientReadInterrupt()).  Otherwise the handler may only set a
 *	  flag, which will cause the processing to occur just before we next go
 *	  idle.
 *
 * 5. 收到 PROCSIG_NOTIFY_INTERRUPT 后，信号处理函数设置进程 latch。
 * 若此后端空闲（正在等待前端命令且不在事务块内，参见 ProcessClientReadInterrupt()），事件会立刻被处理。
 * 否则处理函数可能只设置标志，等到下次进入空闲之前再处理。
 *
 *	  Inbound-notify processing consists of reading all of the notifications
 *	  that have arrived since scanning last time. We read every notification
 *	  until we reach either a notification from an uncommitted transaction or
 *	  the head pointer's position.
 *
 * 入站通知处理就是读出自上次扫描以来到达的全部通知。
 * 一直读到一条来自未提交事务的通知，或读到头指针位置为止。
 *
 * 6. To limit disk space consumption, the tail pointer needs to be advanced
 *	  so that old pages can be truncated. This is relatively expensive
 *	  (notably, it requires an exclusive lock), so we don't want to do it
 *	  often. We make sending backends do this work if they advanced the queue
 *	  head into a new page, but only once every QUEUE_CLEANUP_DELAY pages.
 *
 * 6. 为限制磁盘占用，需要推进尾指针，以便截断旧页。这相对昂贵（尤其需要排他锁），不宜频繁做。
 * 若发送后端把队列头推进到新页，则由它来做这件事，但每 QUEUE_CLEANUP_DELAY 页才做一次。
 *
 * An application that listens on the same channel it notifies will get
 * NOTIFY messages for its own NOTIFYs.  These can be ignored, if not useful,
 * by comparing be_pid in the NOTIFY message to the application's own backend's
 * PID.  (As of FE/BE protocol 2.0, the backend's PID is provided to the
 * frontend during startup.)  The above design guarantees that notifies from
 * other backends will never be missed by ignoring self-notifies.
 *
 * 在自己通知的同一个 channel 上监听的应用，会收到自己 NOTIFY 的消息。
 * 若不需要，可把 NOTIFY 消息里的 be_pid 与应用自己后端的 PID 比较后忽略。
 * （自 FE/BE 协议 2.0 起，启动时会把后端 PID 提供给前端。）
 * 上述设计保证：忽略 self-notify 不会漏掉其他后端的通知。
 *
 * The amount of shared memory used for notify management (notify_buffers)
 * can be varied without affecting anything but performance.  The maximum
 * amount of notification data that can be queued at one time is determined
 * by max_notify_queue_pages GUC.
 *
 * 用于通知管理的共享内存量（notify_buffers）可以调整，只影响性能。
 * 一次能排队的通知数据上限由 GUC max_notify_queue_pages 决定。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <unistd.h>
#include <signal.h>

#include "access/parallel.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "commands/async.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/*
 * 核心流程概览：
 * Async_Notify / pg_notify：把 NOTIFY 放入事务本地的 pendingNotifies，提交时才写入全局队列。
 * LISTEN / UNLISTEN：经 queue_listen 记下待处理动作，到 AtCommit_Notify 才更新 listenChannels。
 * PreCommit_Notify：在写入 clog 之前登记监听者，并把通知写入 pg_notify 的 SLRU 队列。
 * AtCommit_Notify：更新监听状态，由 SignalBackends 发送 PROCSIG_NOTIFY_INTERRUPT，并按需推进队列尾。
 * ProcessNotifyInterrupt / asyncQueueReadAllNotifications：空闲时读队列，把匹配通知发给前端。
 * asyncQueueAdvanceTail：按各后端尾指针截断旧页；AsyncNotifyFreezeXids 在截断 CLOG 前冻结队列中的 XID。
 */


/*
 * Maximum size of a NOTIFY payload, including terminating NULL.  This
 * must be kept small enough so that a notification message fits on one
 * SLRU page.  The magic fudge factor here is noncritical as long as it's
 * more than AsyncQueueEntryEmptySize --- we make it significantly bigger
 * than that, so changes in that data structure won't affect user-visible
 * restrictions.
 *
 * NOTIFY payload 的最大长度，含结尾的 NULL。必须足够小，使一条通知消息能放进一个 SLRU 页。
 * 这里的余量并不关键，只要大于 AsyncQueueEntryEmptySize 即可；
 * 我们把它取得明显更大，这样该数据结构的变化不会影响用户可见的限制。
 */
#define NOTIFY_PAYLOAD_MAX_LENGTH	(BLCKSZ - NAMEDATALEN - 128)

/*
 * Struct representing an entry in the global notify queue
 *
 * 表示全局通知队列中一项的结构
 *
 * This struct declaration has the maximal length, but in a real queue entry
 * the data area is only big enough for the actual channel and payload strings
 * (each null-terminated).  AsyncQueueEntryEmptySize is the minimum possible
 * entry size, if both channel and payload strings are empty (but note it
 * doesn't include alignment padding).
 *
 * 这个结构声明使用最大长度，但真实队列项的数据区只够放下实际的 channel 和 payload 字符串（各自以 null 结尾）。
 * AsyncQueueEntryEmptySize 是两项都为空时的最小项大小（注意不含对齐填充）。
 *
 * The "length" field should always be rounded up to the next QUEUEALIGN
 * multiple so that all fields are properly aligned.
 *
 * length 字段应始终向上取整到下一个 QUEUEALIGN 的倍数，以便所有字段正确对齐。
 */
typedef struct AsyncQueueEntry
{
	int			length;			/* total allocated length of entry */
	/*
	 *
	 * 该项分配的总长度
	 */
	Oid			dboid;			/* sender's database OID */
	/*
	 *
	 * 发送方的数据库 OID
	 */
	TransactionId xid;			/* sender's XID */
	/*
	 *
	 * 发送方的 XID
	 */
	int32		srcPid;			/* sender's PID */
	/*
	 *
	 * 发送方的 PID
	 */
	char		data[NAMEDATALEN + NOTIFY_PAYLOAD_MAX_LENGTH];
} AsyncQueueEntry;

/* Currently, no field of AsyncQueueEntry requires more than int alignment */
/*
 *
 * 目前 AsyncQueueEntry 的字段都不需要超过 int 对齐
 */
#define QUEUEALIGN(len)		INTALIGN(len)

#define AsyncQueueEntryEmptySize	(offsetof(AsyncQueueEntry, data) + 2)

/*
 * Struct describing a queue position, and assorted macros for working with it
 *
 * 描述队列位置的结构，以及操作它的若干宏
 */
typedef struct QueuePosition
{
	int64		page;			/* SLRU page number */
	/*
	 *
	 * SLRU 页号
	 */
	int			offset;			/* byte offset within page */
	/*
	 *
	 * 页内字节偏移
	 */
} QueuePosition;

#define QUEUE_POS_PAGE(x)		((x).page)
#define QUEUE_POS_OFFSET(x)		((x).offset)

#define SET_QUEUE_POS(x,y,z) \
	do { \
		(x).page = (y); \
		(x).offset = (z); \
	} while (0)

#define QUEUE_POS_EQUAL(x,y) \
	((x).page == (y).page && (x).offset == (y).offset)

#define QUEUE_POS_IS_ZERO(x) \
	((x).page == 0 && (x).offset == 0)

/* choose logically smaller QueuePosition */
/*
 *
 * 选取逻辑上更小的 QueuePosition
 */
#define QUEUE_POS_MIN(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (x) : \
	 (x).page != (y).page ? (y) : \
	 (x).offset < (y).offset ? (x) : (y))

/* choose logically larger QueuePosition */
/*
 *
 * 选取逻辑上更大的 QueuePosition
 */
#define QUEUE_POS_MAX(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (y) : \
	 (x).page != (y).page ? (x) : \
	 (x).offset > (y).offset ? (x) : (y))

/*
 * Parameter determining how often we try to advance the tail pointer:
 * we do that after every QUEUE_CLEANUP_DELAY pages of NOTIFY data.  This is
 * also the distance by which a backend in another database needs to be
 * behind before we'll decide we need to wake it up to advance its pointer.
 *
 * 决定多久尝试推进一次尾指针的参数：每处理 QUEUE_CLEANUP_DELAY 页 NOTIFY 数据后做一次。
 * 其他数据库中的后端落后超过这段距离时，也会据此决定是否唤醒它以推进指针。
 *
 * Resist the temptation to make this really large.  While that would save
 * work in some places, it would add cost in others.  In particular, this
 * should likely be less than notify_buffers, to ensure that backends
 * catch up before the pages they'll need to read fall out of SLRU cache.
 *
 * 不要把它设得太大。有些地方能省工作，另一些地方会增加代价。
 * 尤其是它应小于 notify_buffers，以确保后端能在所需页面掉出 SLRU 缓存之前赶上。
 */
#define QUEUE_CLEANUP_DELAY 4

/*
 * Struct describing a listening backend's status
 *
 * 描述正在监听的后端状态的结构
 */
typedef struct QueueBackendStatus
{
	int32		pid;			/* either a PID or InvalidPid */
	/*
	 *
	 * PID，或者 InvalidPid
	 */
	Oid			dboid;			/* backend's database OID, or InvalidOid */
	/*
	 *
	 * 后端的数据库 OID，或者 InvalidOid
	 */
	ProcNumber	nextListener;	/* id of next listener, or INVALID_PROC_NUMBER */
	/*
	 *
	 * 下一个监听者的编号，或者 INVALID_PROC_NUMBER
	 */
	QueuePosition pos;			/* backend has read queue up to here */
	/*
	 *
	 * 此后端已把队列读到这里
	 */
} QueueBackendStatus;

/*
 * Shared memory state for LISTEN/NOTIFY (excluding its SLRU stuff)
 *
 * LISTEN/NOTIFY 的共享内存状态（不含其 SLRU 部分）
 *
 * The AsyncQueueControl structure is protected by the NotifyQueueLock and
 * NotifyQueueTailLock.
 *
 * AsyncQueueControl 结构由 NotifyQueueLock 和 NotifyQueueTailLock 保护。
 *
 * When holding NotifyQueueLock in SHARED mode, backends may only inspect
 * their own entries as well as the head and tail pointers. Consequently we
 * can allow a backend to update its own record while holding only SHARED lock
 * (since no other backend will inspect it).
 *
 * 以 SHARED 模式持有 NotifyQueueLock 时，后端只能查看自己的项以及头尾指针。
 * 因此后端只需 SHARED 锁就能更新自己的记录（其他后端不会查看它）。
 *
 * When holding NotifyQueueLock in EXCLUSIVE mode, backends can inspect the
 * entries of other backends and also change the head pointer. When holding
 * both NotifyQueueLock and NotifyQueueTailLock in EXCLUSIVE mode, backends
 * can change the tail pointers.
 *
 * 以 EXCLUSIVE 模式持有 NotifyQueueLock 时，后端可以查看其他后端的项，也可以修改头指针。
 * 同时以 EXCLUSIVE 模式持有 NotifyQueueLock 和 NotifyQueueTailLock 时，可以修改尾指针。
 *
 * SLRU buffer pool is divided in banks and bank wise SLRU lock is used as
 * the control lock for the pg_notify SLRU buffers.
 * In order to avoid deadlocks, whenever we need multiple locks, we first get
 * NotifyQueueTailLock, then NotifyQueueLock, and lastly SLRU bank lock.
 *
 * SLRU 缓冲池按 bank 划分，pg_notify 的 SLRU 缓冲区用按 bank 的 SLRU 锁作为控制锁。
 * 为避免死锁，需要多把锁时，先取 NotifyQueueTailLock，再取 NotifyQueueLock，最后取 SLRU bank 锁。
 *
 * Each backend uses the backend[] array entry with index equal to its
 * ProcNumber.  We rely on this to make SendProcSignal fast.
 *
 * 每个后端使用 backend[] 中下标等于自己 ProcNumber 的项。SendProcSignal 依赖这一点以保持快速。
 *
 * The backend[] array entries for actively-listening backends are threaded
 * together using firstListener and the nextListener links, so that we can
 * scan them without having to iterate over inactive entries.  We keep this
 * list in order by ProcNumber so that the scan is cache-friendly when there
 * are many active entries.
 *
 * 正在监听的后端的 backend[] 项用 firstListener 和 nextListener 串成链表，
 * 这样扫描时不必遍历不活动的项。链表按 ProcNumber 排序，活动项很多时扫描对缓存更友好。
 */
typedef struct AsyncQueueControl
{
	QueuePosition head;			/* head points to the next free location */
	/*
	 *
	 * head 指向下一个空闲位置
	 */
	QueuePosition tail;			/* tail must be <= the queue position of every
								 * listening backend */
	int64		stopPage;		/* oldest unrecycled page; must be <=
								 * tail.page */
	ProcNumber	firstListener;	/* id of first listener, or
								 * INVALID_PROC_NUMBER */
	TimestampTz lastQueueFillWarn;	/* time of last queue-full msg */
	/*
	 *
	 * 上次队列满警告的时间
	 */
	QueueBackendStatus backend[FLEXIBLE_ARRAY_MEMBER];
} AsyncQueueControl;

static AsyncQueueControl *asyncQueueControl;

#define QUEUE_HEAD					(asyncQueueControl->head)
#define QUEUE_TAIL					(asyncQueueControl->tail)
#define QUEUE_STOP_PAGE				(asyncQueueControl->stopPage)
#define QUEUE_FIRST_LISTENER		(asyncQueueControl->firstListener)
#define QUEUE_BACKEND_PID(i)		(asyncQueueControl->backend[i].pid)
#define QUEUE_BACKEND_DBOID(i)		(asyncQueueControl->backend[i].dboid)
#define QUEUE_NEXT_LISTENER(i)		(asyncQueueControl->backend[i].nextListener)
#define QUEUE_BACKEND_POS(i)		(asyncQueueControl->backend[i].pos)

/*
 * The SLRU buffer area through which we access the notification queue
 *
 * 用来访问通知队列的 SLRU 缓冲区
 */
static SlruCtlData NotifyCtlData;

#define NotifyCtl					(&NotifyCtlData)
#define QUEUE_PAGESIZE				BLCKSZ

#define QUEUE_FULL_WARN_INTERVAL	5000	/* warn at most once every 5s */
/*
 *
 * 最多每 5 秒警告一次
 */

/*
 * listenChannels identifies the channels we are actually listening to
 * (ie, have committed a LISTEN on).  It is a simple list of channel names,
 * allocated in TopMemoryContext.
 *
 * listenChannels 表示我们实际正在监听的 channel（即已提交的 LISTEN）。
 * 它是 channel 名的简单列表，分配在 TopMemoryContext 中。
 */
static List *listenChannels = NIL;	/* list of C strings */
/*
 *
 * C 字符串列表
 */

/*
 * State for pending LISTEN/UNLISTEN actions consists of an ordered list of
 * all actions requested in the current transaction.  As explained above,
 * we don't actually change listenChannels until we reach transaction commit.
 *
 * 待处理的 LISTEN/UNLISTEN 动作是当前事务中所有请求的有序列表。
 * 如上所述，要到事务提交时才真正修改 listenChannels。
 *
 * The list is kept in CurTransactionContext.  In subtransactions, each
 * subtransaction has its own list in its own CurTransactionContext, but
 * successful subtransactions attach their lists to their parent's list.
 * Failed subtransactions simply discard their lists.
 *
 * 列表保存在 CurTransactionContext 中。子事务各有自己的列表，放在各自的 CurTransactionContext 里；
 * 成功的子事务把列表挂到父事务的列表上。失败的子事务直接丢弃自己的列表。
 */
typedef enum
{
	LISTEN_LISTEN,
	LISTEN_UNLISTEN,
	LISTEN_UNLISTEN_ALL,
} ListenActionKind;

typedef struct
{
	ListenActionKind action;
	char		channel[FLEXIBLE_ARRAY_MEMBER]; /* nul-terminated string */
	/*
	 *
	 * 以 nul 结尾的字符串
	 */
} ListenAction;

typedef struct ActionList
{
	int			nestingLevel;	/* current transaction nesting depth */
	/*
	 *
	 * 当前事务嵌套深度
	 */
	List	   *actions;		/* list of ListenAction structs */
	/*
	 *
	 * ListenAction 结构的列表
	 */
	struct ActionList *upper;	/* details for upper transaction levels */
	/*
	 *
	 * 更上层事务的细节
	 */
} ActionList;

static ActionList *pendingActions = NULL;

/*
 * State for outbound notifies consists of a list of all channels+payloads
 * NOTIFYed in the current transaction.  We do not actually perform a NOTIFY
 * until and unless the transaction commits.  pendingNotifies is NULL if no
 * NOTIFYs have been done in the current (sub) transaction.
 *
 * 出站通知的状态是当前事务中所有已 NOTIFY 的 channel 与 payload 列表。
 * 只有事务提交时才真正执行 NOTIFY。若当前（子）事务没有 NOTIFY，则 pendingNotifies 为 NULL。
 *
 * We discard duplicate notify events issued in the same transaction.
 * Hence, in addition to the list proper (which we need to track the order
 * of the events, since we guarantee to deliver them in order), we build a
 * hash table which we can probe to detect duplicates.  Since building the
 * hash table is somewhat expensive, we do so only once we have at least
 * MIN_HASHABLE_NOTIFIES events queued in the current (sub) transaction;
 * before that we just scan the events linearly.
 *
 * 同一事务中发出的重复通知事件会被丢掉。因此除了真正的列表
 * （需要用它保持事件顺序，因为我们保证按序投递），还建一个哈希表来探测重复。
 * 建哈希表有些贵，所以要等到当前（子）事务中至少排入 MIN_HASHABLE_NOTIFIES 个事件才建；
 * 在那之前只线性扫描事件。
 *
 * The list is kept in CurTransactionContext.  In subtransactions, each
 * subtransaction has its own list in its own CurTransactionContext, but
 * successful subtransactions add their entries to their parent's list.
 * Failed subtransactions simply discard their lists.  Since these lists
 * are independent, there may be notify events in a subtransaction's list
 * that duplicate events in some ancestor (sub) transaction; we get rid of
 * the dups when merging the subtransaction's list into its parent's.
 *
 * 列表保存在 CurTransactionContext 中。子事务各有自己的列表；
 * 成功的子事务把项加入父事务的列表。失败的子事务直接丢弃。
 * 这些列表相互独立，子事务列表里可能有与祖先（子）事务重复的通知；
 * 合并到父列表时再去掉重复。
 *
 * Note: the action and notify lists do not interact within a transaction.
 * In particular, if a transaction does NOTIFY and then LISTEN on the same
 * condition name, it will get a self-notify at commit.  This is a bit odd
 * but is consistent with our historical behavior.
 *
 * 注意：动作列表和通知列表在事务内互不影响。
 * 特别是，若事务先 NOTIFY 再对同一条件名 LISTEN，提交时会收到 self-notify。
 * 这有点奇怪，但与历史上的行为一致。
 */
typedef struct Notification
{
	uint16		channel_len;	/* length of channel-name string */
	/*
	 *
	 * channel 名字符串的长度
	 */
	uint16		payload_len;	/* length of payload string */
	/*
	 *
	 * payload 字符串的长度
	 */
	/* null-terminated channel name, then null-terminated payload follow */
	/*
	 *
	 * 随后是以 null 结尾的 channel 名，再是以 null 结尾的 payload
	 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} Notification;

typedef struct NotificationList
{
	int			nestingLevel;	/* current transaction nesting depth */
	/*
	 *
	 * 当前事务嵌套深度
	 */
	List	   *events;			/* list of Notification structs */
	/*
	 *
	 * Notification 结构的列表
	 */
	HTAB	   *hashtab;		/* hash of NotificationHash structs, or NULL */
	/*
	 *
	 * NotificationHash 结构的哈希表，或 NULL
	 */
	struct NotificationList *upper; /* details for upper transaction levels */
	/*
	 *
	 * 更上层事务的细节
	 */
} NotificationList;

#define MIN_HASHABLE_NOTIFIES 16	/* threshold to build hashtab */
/*
 *
 * 建立哈希表的阈值
 */

struct NotificationHash
{
	Notification *event;		/* => the actual Notification struct */
	/*
	 *
	 * 即实际的 Notification 结构
	 */
};

static NotificationList *pendingNotifies = NULL;

/*
 * Inbound notifications are initially processed by HandleNotifyInterrupt(),
 * called from inside a signal handler. That just sets the
 * notifyInterruptPending flag and sets the process
 * latch. ProcessNotifyInterrupt() will then be called whenever it's safe to
 * actually deal with the interrupt.
 *
 * 入站通知首先由信号处理函数内部调用的 HandleNotifyInterrupt() 处理。
 * 它只设置 notifyInterruptPending 标志并设置进程 latch。
 * 到可以真正处理中断时，再调用 ProcessNotifyInterrupt()。
 */
volatile sig_atomic_t notifyInterruptPending = false;

/* True if we've registered an on_shmem_exit cleanup */
/*
 *
 * 若已登记 on_shmem_exit 清理，则为真
 */
static bool unlistenExitRegistered = false;

/* True if we're currently registered as a listener in asyncQueueControl */
/*
 *
 * 若当前已作为监听者登记在 asyncQueueControl 中，则为真
 */
static bool amRegisteredListener = false;

/* have we advanced to a page that's a multiple of QUEUE_CLEANUP_DELAY? */
/*
 *
 * 是否已经推进到 QUEUE_CLEANUP_DELAY 整数倍的页？
 */
static bool tryAdvanceTail = false;

/* GUC parameters */
/*
 *
 * GUC 参数
 */
bool		Trace_notify = false;

/* For 8 KB pages this gives 8 GB of disk space */
/*
 *
 * 对于 8 KB 的页，这对应 8 GB 磁盘空间
 */
int			max_notify_queue_pages = 1048576;

/* local function prototypes */
/*
 *
 * 本文件内部静态函数声明
 */
static inline int64 asyncQueuePageDiff(int64 p, int64 q);
static inline bool asyncQueuePagePrecedes(int64 p, int64 q);
static void queue_listen(ListenActionKind action, const char *channel);
static void Async_UnlistenOnExit(int code, Datum arg);
static void Exec_ListenPreCommit(void);
static void Exec_ListenCommit(const char *channel);
static void Exec_UnlistenCommit(const char *channel);
static void Exec_UnlistenAllCommit(void);
static bool IsListeningOn(const char *channel);
static void asyncQueueUnregister(void);
static bool asyncQueueIsFull(void);
static bool asyncQueueAdvance(volatile QueuePosition *position, int entryLength);
static void asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe);
static ListCell *asyncQueueAddEntries(ListCell *nextNotify);
static double asyncQueueUsage(void);
static void asyncQueueFillWarning(void);
static void SignalBackends(void);
static void asyncQueueReadAllNotifications(void);
static bool asyncQueueProcessPageEntries(QueuePosition *current,
										 QueuePosition stop,
										 Snapshot snapshot);
static void asyncQueueAdvanceTail(void);
static void ProcessIncomingNotify(bool flush);
static bool AsyncExistsPendingNotify(Notification *n);
static void AddEventToPendingNotifies(Notification *n);
static uint32 notification_hash(const void *key, Size keysize);
static int	notification_match(const void *key1, const void *key2, Size keysize);
static void ClearPendingActionsAndNotifies(void);

/*
 * Compute the difference between two queue page numbers.
 * Previously this function accounted for a wraparound.
 *
 * 计算两个队列页号之差。此函数以前还考虑过回绕。
 */
static inline int64
asyncQueuePageDiff(int64 p, int64 q)
{
	return p - q;
}

/*
 * Determines whether p precedes q.
 * Previously this function accounted for a wraparound.
 *
 * 判断 p 是否在 q 之前。此函数以前还考虑过回绕。
 */
static inline bool
asyncQueuePagePrecedes(int64 p, int64 q)
{
	return p < q;
}

/*
 * Report space needed for our shared memory area
 *
 * 报告本模块共享内存区域所需空间
 */
Size
AsyncShmemSize(void)
{
	Size		size;

	/* This had better match AsyncShmemInit */
	/*
	 *
	 * 这里最好与 AsyncShmemInit 一致
	 */
	size = mul_size(MaxBackends, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	size = add_size(size, SimpleLruShmemSize(notify_buffers, 0));

	return size;
}

/*
 * Initialize our shared memory area
 *
 * 初始化本模块的共享内存区域
 */
void
AsyncShmemInit(void)
{
	bool		found;
	Size		size;

	/*
	 * Create or attach to the AsyncQueueControl structure.
	 *
	 * 创建或挂接到 AsyncQueueControl 结构。
	 */
	size = mul_size(MaxBackends, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	asyncQueueControl = (AsyncQueueControl *)
		ShmemInitStruct("Async Queue Control", size, &found);

	if (!found)
	{
		/* First time through, so initialize it */
		/*
		 *
		 * 第一次经过，因此初始化它
		 */
		SET_QUEUE_POS(QUEUE_HEAD, 0, 0);
		SET_QUEUE_POS(QUEUE_TAIL, 0, 0);
		QUEUE_STOP_PAGE = 0;
		QUEUE_FIRST_LISTENER = INVALID_PROC_NUMBER;
		asyncQueueControl->lastQueueFillWarn = 0;
		for (int i = 0; i < MaxBackends; i++)
		{
			QUEUE_BACKEND_PID(i) = InvalidPid;
			QUEUE_BACKEND_DBOID(i) = InvalidOid;
			QUEUE_NEXT_LISTENER(i) = INVALID_PROC_NUMBER;
			SET_QUEUE_POS(QUEUE_BACKEND_POS(i), 0, 0);
		}
	}

	/*
	 * Set up SLRU management of the pg_notify data. Note that long segment
	 * names are used in order to avoid wraparound.
	 *
	 * 建立 pg_notify 数据的 SLRU 管理。使用较长的段名，以避免回绕。
	 */
	NotifyCtl->PagePrecedes = asyncQueuePagePrecedes;
	SimpleLruInit(NotifyCtl, "notify", notify_buffers, 0,
				  "pg_notify", LWTRANCHE_NOTIFY_BUFFER, LWTRANCHE_NOTIFY_SLRU,
				  SYNC_HANDLER_NONE, true);

	if (!found)
	{
		/*
		 * During start or reboot, clean out the pg_notify directory.
		 *
		 * 启动或重启期间，清空 pg_notify 目录。
		 */
		(void) SlruScanDirectory(NotifyCtl, SlruScanDirCbDeleteAll, NULL);
	}
}


/*
 * pg_notify -
 *	  SQL function to send a notification event
 *
 * pg_notify：发送通知事件的 SQL 函数
 */
Datum
pg_notify(PG_FUNCTION_ARGS)
{
	const char *channel;
	const char *payload;

	if (PG_ARGISNULL(0))
		channel = "";
	else
		channel = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (PG_ARGISNULL(1))
		payload = "";
	else
		payload = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* For NOTIFY as a statement, this is checked in ProcessUtility */
	/*
	 *
	 * 对于作为语句的 NOTIFY，这项检查在 ProcessUtility 中完成
	 */
	PreventCommandDuringRecovery("NOTIFY");

	Async_Notify(channel, payload);

	PG_RETURN_VOID();
}


/*
 * Async_Notify
 *
 * 函数 Async_Notify。
 *
 *		This is executed by the SQL notify command.
 *
 * 由 SQL 的 NOTIFY 命令执行。
 *
 *		Adds the message to the list of pending notifies.
 *		Actual notification happens during transaction commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 * 把消息加入待发送通知列表。真正的通知发生在事务提交期间。
 */
void
Async_Notify(const char *channel, const char *payload)
{
	int			my_level = GetCurrentTransactionNestLevel();
	size_t		channel_len;
	size_t		payload_len;
	Notification *n;
	MemoryContext oldcontext;

	if (IsParallelWorker())
		elog(ERROR, "cannot send notifications from a parallel worker");

	if (Trace_notify)
		elog(DEBUG1, "Async_Notify(%s)", channel);

	channel_len = channel ? strlen(channel) : 0;
	payload_len = payload ? strlen(payload) : 0;

	/* a channel name must be specified */
	/*
	 *
	 * 必须指定 channel 名
	 */
	if (channel_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name cannot be empty")));

	/* enforce length limits */
	/*
	 *
	 * 强制执行长度限制
	 */
	if (channel_len >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name too long")));

	if (payload_len >= NOTIFY_PAYLOAD_MAX_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("payload string too long")));

	/*
	 * We must construct the Notification entry, even if we end up not using
	 * it, in order to compare it cheaply to existing list entries.
	 *
	 * 即使最终不用，也必须构造 Notification 项，以便廉价地与已有列表项比较。
	 *
	 * The notification list needs to live until end of transaction, so store
	 * it in the transaction context.
	 *
	 * 通知列表需要活到事务结束，因此存放在事务上下文中。
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	n = (Notification *) palloc(offsetof(Notification, data) +
								channel_len + payload_len + 2);
	n->channel_len = channel_len;
	n->payload_len = payload_len;
	strcpy(n->data, channel);
	if (payload)
		strcpy(n->data + channel_len + 1, payload);
	else
		n->data[channel_len + 1] = '\0';

	if (pendingNotifies == NULL || my_level > pendingNotifies->nestingLevel)
	{
		NotificationList *notifies;

		/*
		 * First notify event in current (sub)xact. Note that we allocate the
		 * NotificationList in TopTransactionContext; the nestingLevel might
		 * get changed later by AtSubCommit_Notify.
		 *
		 * 当前（子）事务中的第一个通知事件。注意 NotificationList 分配在 TopTransactionContext 中；
		 * nestingLevel 以后可能被 AtSubCommit_Notify 改掉。
		 */
		notifies = (NotificationList *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(NotificationList));
		notifies->nestingLevel = my_level;
		notifies->events = list_make1(n);
		/* We certainly don't need a hashtable yet */
		/*
		 *
		 * 目前肯定还不需要哈希表
		 */
		notifies->hashtab = NULL;
		notifies->upper = pendingNotifies;
		pendingNotifies = notifies;
	}
	else
	{
		/* Now check for duplicates */
		/*
		 *
		 * 现在检查重复
		 */
		if (AsyncExistsPendingNotify(n))
		{
			/* It's a dup, so forget it */
			/*
			 *
			 * 是重复的，丢掉它
			 */
			pfree(n);
			MemoryContextSwitchTo(oldcontext);
			return;
		}

		/* Append more events to existing list */
		/*
		 *
		 * 把更多事件追加到已有列表
		 */
		AddEventToPendingNotifies(n);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * queue_listen
 *		Common code for listen, unlisten, unlisten all commands.
 *
 * queue_listen：listen、unlisten、unlisten all 命令的公共代码。
 *
 *		Adds the request to the list of pending actions.
 *		Actual update of the listenChannels list happens during transaction
 *		commit.
 *
 * 把请求加入待处理动作列表。listenChannels 列表的实际更新发生在事务提交时。
 */
static void
queue_listen(ListenActionKind action, const char *channel)
{
	MemoryContext oldcontext;
	ListenAction *actrec;
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Unlike Async_Notify, we don't try to collapse out duplicates. It would
	 * be too complicated to ensure we get the right interactions of
	 * conflicting LISTEN/UNLISTEN/UNLISTEN_ALL, and it's unlikely that there
	 * would be any performance benefit anyway in sane applications.
	 *
	 * 与 Async_Notify 不同，这里不试图合并重复项。
	 * 要正确处理互相冲突的 LISTEN/UNLISTEN/UNLISTEN_ALL 太复杂，
	 * 而且在正常应用里也不太可能有性能收益。
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	/* space for terminating null is included in sizeof(ListenAction) */
	/*
	 *
	 * 结尾的 null 所需空间已包含在 sizeof(ListenAction) 中
	 */
	actrec = (ListenAction *) palloc(offsetof(ListenAction, channel) +
									 strlen(channel) + 1);
	actrec->action = action;
	strcpy(actrec->channel, channel);

	if (pendingActions == NULL || my_level > pendingActions->nestingLevel)
	{
		ActionList *actions;

		/*
		 * First action in current sub(xact). Note that we allocate the
		 * ActionList in TopTransactionContext; the nestingLevel might get
		 * changed later by AtSubCommit_Notify.
		 *
		 * 当前子事务中的第一个动作。注意 ActionList 分配在 TopTransactionContext 中；
		 * nestingLevel 以后可能被 AtSubCommit_Notify 改掉。
		 */
		actions = (ActionList *)
			MemoryContextAlloc(TopTransactionContext, sizeof(ActionList));
		actions->nestingLevel = my_level;
		actions->actions = list_make1(actrec);
		actions->upper = pendingActions;
		pendingActions = actions;
	}
	else
		pendingActions->actions = lappend(pendingActions->actions, actrec);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Async_Listen
 *
 * 函数 Async_Listen。
 *
 *		This is executed by the SQL listen command.
 *
 * 由 SQL 的 LISTEN 命令执行。
 */
void
Async_Listen(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Listen(%s,%d)", channel, MyProcPid);

	queue_listen(LISTEN_LISTEN, channel);
}

/*
 * Async_Unlisten
 *
 * 函数 Async_Unlisten。
 *
 *		This is executed by the SQL unlisten command.
 *
 * 由 SQL 的 UNLISTEN 命令执行。
 */
void
Async_Unlisten(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Unlisten(%s,%d)", channel, MyProcPid);

	/* If we couldn't possibly be listening, no need to queue anything */
	/*
	 *
	 * 若不可能正在监听，则不必把任何东西入队
	 */
	if (pendingActions == NULL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN, channel);
}

/*
 * Async_UnlistenAll
 *
 * 函数 Async_UnlistenAll。
 *
 *		This is invoked by UNLISTEN * command, and also at backend exit.
 *
 * 由 UNLISTEN * 命令调用，后端退出时也会调用。
 */
void
Async_UnlistenAll(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_UnlistenAll(%d)", MyProcPid);

	/* If we couldn't possibly be listening, no need to queue anything */
	/*
	 *
	 * 若不可能正在监听，则不必把任何东西入队
	 */
	if (pendingActions == NULL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN_ALL, "");
}

/*
 * SQL function: return a set of the channel names this backend is actively
 * listening to.
 *
 * SQL 函数：返回此后端正在主动监听的 channel 名集合。
 *
 * Note: this coding relies on the fact that the listenChannels list cannot
 * change within a transaction.
 *
 * 注意：这段代码依赖 listenChannels 列表在事务内不会改变这一事实。
 */
Datum
pg_listening_channels(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	/* stuff done only on the first call of the function */
	/*
	 *
	 * 只在函数第一次调用时做的事情
	 */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		/*
		 *
		 * 创建跨调用保持的函数上下文
		 */
		funcctx = SRF_FIRSTCALL_INIT();
	}

	/* stuff done on every call of the function */
	/*
	 *
	 * 函数每次调用都做的事情
	 */
	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < list_length(listenChannels))
	{
		char	   *channel = (char *) list_nth(listenChannels,
												funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(channel));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Async_UnlistenOnExit
 *
 * 函数 Async_UnlistenOnExit。
 *
 * This is executed at backend exit if we have done any LISTENs in this
 * backend.  It might not be necessary anymore, if the user UNLISTENed
 * everything, but we don't try to detect that case.
 *
 * 若本后端做过任何 LISTEN，则在后端退出时执行。
 * 若用户已经 UNLISTEN 了全部 channel，也许不再必要，但我们不试图检测那种情况。
 */
static void
Async_UnlistenOnExit(int code, Datum arg)
{
	Exec_UnlistenAllCommit();
	asyncQueueUnregister();
}

/*
 * AtPrepare_Notify
 *
 * 函数 AtPrepare_Notify。
 *
 *		This is called at the prepare phase of a two-phase
 *		transaction.  Save the state for possible commit later.
 *
 * 在两阶段事务的 prepare 阶段调用。保存状态，以便稍后可能提交。
 */
void
AtPrepare_Notify(void)
{
	/* It's not allowed to have any pending LISTEN/UNLISTEN/NOTIFY actions */
	/*
	 *
	 * 不允许存在任何未完成的 LISTEN/UNLISTEN/NOTIFY 动作
	 */
	if (pendingActions || pendingNotifies)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has executed LISTEN, UNLISTEN, or NOTIFY")));
}

/*
 * PreCommit_Notify
 *
 * 函数 PreCommit_Notify。
 *
 *		This is called at transaction commit, before actually committing to
 *		clog.
 *
 * 在事务提交时、真正写入 clog 之前调用。
 *
 *		If there are pending LISTEN actions, make sure we are listed in the
 *		shared-memory listener array.  This must happen before commit to
 *		ensure we don't miss any notifies from transactions that commit
 *		just after ours.
 *
 * 若有待处理的 LISTEN 动作，确保我们已列入共享内存中的监听者数组。
 * 这必须在提交前发生，以免错过紧接在我们之后提交的事务所发的通知。
 *
 *		If there are outbound notify requests in the pendingNotifies list,
 *		add them to the global queue.  We do that before commit so that
 *		we can still throw error if we run out of queue space.
 *
 * 若 pendingNotifies 列表中有出站通知请求，把它们加入全局队列。
 * 在提交前做这件事，这样队列空间耗尽时仍可以抛错。
 */
void
PreCommit_Notify(void)
{
	ListCell   *p;

	if (!pendingActions && !pendingNotifies)
		return;					/* no relevant statements in this xact */
		/*
		 *
		 * 本事务中没有相关语句
		 */

	if (Trace_notify)
		elog(DEBUG1, "PreCommit_Notify");

	/* Preflight for any pending listen/unlisten actions */
	/*
	 *
	 * 为任何待处理的 listen/unlisten 动作做预先检查
	 */
	if (pendingActions != NULL)
	{
		foreach(p, pendingActions->actions)
		{
			ListenAction *actrec = (ListenAction *) lfirst(p);

			switch (actrec->action)
			{
				case LISTEN_LISTEN:
					Exec_ListenPreCommit();
					break;
				case LISTEN_UNLISTEN:
					/* there is no Exec_UnlistenPreCommit() */
					/*
					 *
					 * 没有 Exec_UnlistenPreCommit()
					 */
					break;
				case LISTEN_UNLISTEN_ALL:
					/* there is no Exec_UnlistenAllPreCommit() */
					/*
					 *
					 * 没有 Exec_UnlistenAllPreCommit()
					 */
					break;
			}
		}
	}

	/* Queue any pending notifies (must happen after the above) */
	/*
	 *
	 * 把任何待处理的通知入队（必须在上面的步骤之后）
	 */
	if (pendingNotifies)
	{
		ListCell   *nextNotify;

		/*
		 * Make sure that we have an XID assigned to the current transaction.
		 * GetCurrentTransactionId is cheap if we already have an XID, but not
		 * so cheap if we don't, and we'd prefer not to do that work while
		 * holding NotifyQueueLock.
		 *
		 * 确保当前事务已分配 XID。若已经有 XID，GetCurrentTransactionId 很便宜；
		 * 若还没有则不便宜，我们不希望在持有 NotifyQueueLock 时做这项工作。
		 */
		(void) GetCurrentTransactionId();

		/*
		 * Serialize writers by acquiring a special lock that we hold till
		 * after commit.  This ensures that queue entries appear in commit
		 * order, and in particular that there are never uncommitted queue
		 * entries ahead of committed ones, so an uncommitted transaction
		 * can't block delivery of deliverable notifications.
		 *
		 * 获取一把特殊锁并保持到提交之后，使写者串行化。
		 * 这保证队列项按提交顺序出现，特别是已提交项前面永远不会有未提交项，
		 * 从而未提交事务不会挡住已可投递的通知。
		 *
		 * We use a heavyweight lock so that it'll automatically be released
		 * after either commit or abort.  This also allows deadlocks to be
		 * detected, though really a deadlock shouldn't be possible here.
		 *
		 * 使用重量级锁，以便提交或中止后自动释放。这也能检测死锁，不过这里其实不应该发生死锁。
		 *
		 * The lock is on "database 0", which is pretty ugly but it doesn't
		 * seem worth inventing a special locktag category just for this.
		 * (Historical note: before PG 9.0, a similar lock on "database 0" was
		 * used by the flatfiles mechanism.)
		 *
		 * 锁加在 database 0 上，这相当难看，但不值得为此专门发明一种 locktag。
		 * （历史说明：PG 9.0 之前，flatfiles 机制也使用过 database 0 上的类似锁。）
		 */
		LockSharedObject(DatabaseRelationId, InvalidOid, 0,
						 AccessExclusiveLock);

		/* Now push the notifications into the queue */
		/*
		 *
		 * 现在把通知推进队列
		 */
		nextNotify = list_head(pendingNotifies->events);
		while (nextNotify != NULL)
		{
			/*
			 * Add the pending notifications to the queue.  We acquire and
			 * release NotifyQueueLock once per page, which might be overkill
			 * but it does allow readers to get in while we're doing this.
			 *
			 * 把待处理通知加入队列。每页获取并释放一次 NotifyQueueLock，
			 * 也许有些过度，但这样读者可以在我们进行时插入。
			 *
			 * A full queue is very uncommon and should really not happen,
			 * given that we have so much space available in the SLRU pages.
			 * Nevertheless we need to deal with this possibility. Note that
			 * when we get here we are in the process of committing our
			 * transaction, but we have not yet committed to clog, so at this
			 * point in time we can still roll the transaction back.
			 *
			 * 队列满非常少见，考虑到 SLRU 页有这么多空间，其实不该发生。但仍然必须处理这种可能。
			 * 注意走到这里时我们正在提交事务，但还没有写入 clog，因此此时仍可以回滚事务。
			 */
			LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
			asyncQueueFillWarning();
			if (asyncQueueIsFull())
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many notifications in the NOTIFY queue")));
			nextNotify = asyncQueueAddEntries(nextNotify);
			LWLockRelease(NotifyQueueLock);
		}

		/* Note that we don't clear pendingNotifies; AtCommit_Notify will. */
		/*
		 *
		 * 注意这里不清空 pendingNotifies；AtCommit_Notify 会做。
		 */
	}
}

/*
 * AtCommit_Notify
 *
 * 函数 AtCommit_Notify。
 *
 *		This is called at transaction commit, after committing to clog.
 *
 * 在事务提交时、写入 clog 之后调用。
 *
 *		Update listenChannels and clear transaction-local state.
 *
 * 更新 listenChannels，并清除事务本地状态。
 *
 *		If we issued any notifications in the transaction, send signals to
 *		listening backends (possibly including ourselves) to process them.
 *		Also, if we filled enough queue pages with new notifies, try to
 *		advance the queue tail pointer.
 *
 * 若本事务发出过任何通知，向正在监听的后端（可能包括自己）发信号让它们处理。
 * 另外，若新通知填满了足够多的队列页，尝试推进队列尾指针。
 */
void
AtCommit_Notify(void)
{
	ListCell   *p;

	/*
	 * Allow transactions that have not executed LISTEN/UNLISTEN/NOTIFY to
	 * return as soon as possible
	 *
	 * 没有执行过 LISTEN/UNLISTEN/NOTIFY 的事务应尽快返回
	 */
	if (!pendingActions && !pendingNotifies)
		return;

	if (Trace_notify)
		elog(DEBUG1, "AtCommit_Notify");

	/* Perform any pending listen/unlisten actions */
	/*
	 *
	 * 执行任何待处理的 listen/unlisten 动作
	 */
	if (pendingActions != NULL)
	{
		foreach(p, pendingActions->actions)
		{
			ListenAction *actrec = (ListenAction *) lfirst(p);

			switch (actrec->action)
			{
				case LISTEN_LISTEN:
					Exec_ListenCommit(actrec->channel);
					break;
				case LISTEN_UNLISTEN:
					Exec_UnlistenCommit(actrec->channel);
					break;
				case LISTEN_UNLISTEN_ALL:
					Exec_UnlistenAllCommit();
					break;
			}
		}
	}

	/* If no longer listening to anything, get out of listener array */
	/*
	 *
	 * 若不再监听任何东西，则退出监听者数组
	 */
	if (amRegisteredListener && listenChannels == NIL)
		asyncQueueUnregister();

	/*
	 * Send signals to listening backends.  We need do this only if there are
	 * pending notifies, which were previously added to the shared queue by
	 * PreCommit_Notify().
	 *
	 * 向正在监听的后端发信号。只有存在待处理通知时才需要这样做，
	 * 这些通知此前已由 PreCommit_Notify() 加入共享队列。
	 */
	if (pendingNotifies != NULL)
		SignalBackends();

	/*
	 * If it's time to try to advance the global tail pointer, do that.
	 *
	 * 若到了尝试推进全局尾指针的时候，就做这件事。
	 *
	 * (It might seem odd to do this in the sender, when more than likely the
	 * listeners won't yet have read the messages we just sent.  However,
	 * there's less contention if only the sender does it, and there is little
	 * need for urgency in advancing the global tail.  So this typically will
	 * be clearing out messages that were sent some time ago.)
	 *
	 * （在发送方做这件事看起来有点奇怪，因为监听者多半还没读到我们刚发出的消息。
	 * 但只让发送方做，争用更少，而且推进全局尾指针并不急迫。
	 * 因此这通常清掉的是一段时间以前发出的消息。）
	 */
	if (tryAdvanceTail)
	{
		tryAdvanceTail = false;
		asyncQueueAdvanceTail();
	}

	/* And clean up */
	/*
	 *
	 * 然后清理
	 */
	ClearPendingActionsAndNotifies();
}

/*
 * Exec_ListenPreCommit --- subroutine for PreCommit_Notify
 *
 * Exec_ListenPreCommit：PreCommit_Notify 的子程序
 *
 * This function must make sure we are ready to catch any incoming messages.
 *
 * 此函数必须确保我们已准备好接收任何到来的消息。
 */
static void
Exec_ListenPreCommit(void)
{
	QueuePosition head;
	QueuePosition max;
	ProcNumber	prevListener;

	/*
	 * Nothing to do if we are already listening to something, nor if we
	 * already ran this routine in this transaction.
	 *
	 * 若已经在监听某样东西，或本事务中已经运行过此例程，则无需做事。
	 */
	if (amRegisteredListener)
		return;

	if (Trace_notify)
		elog(DEBUG1, "Exec_ListenPreCommit(%d)", MyProcPid);

	/*
	 * Before registering, make sure we will unlisten before dying. (Note:
	 * this action does not get undone if we abort later.)
	 *
	 * 登记之前，确保进程死亡前会 unlisten。（注意：若稍后中止，这个动作不会被撤销。）
	 */
	if (!unlistenExitRegistered)
	{
		before_shmem_exit(Async_UnlistenOnExit, 0);
		unlistenExitRegistered = true;
	}

	/*
	 * This is our first LISTEN, so establish our pointer.
	 *
	 * 这是我们的第一次 LISTEN，因此建立自己的指针。
	 *
	 * We set our pointer to the global tail pointer and then move it forward
	 * over already-committed notifications.  This ensures we cannot miss any
	 * not-yet-committed notifications.  We might get a few more but that
	 * doesn't hurt.
	 *
	 * 把指针设为全局尾指针，然后向前跳过已经提交的通知。
	 * 这保证不会错过尚未提交的通知。可能会多看到几条，但没有害处。
	 *
	 * In some scenarios there might be a lot of committed notifications that
	 * have not yet been pruned away (because some backend is being lazy about
	 * reading them).  To reduce our startup time, we can look at other
	 * backends and adopt the maximum "pos" pointer of any backend that's in
	 * our database; any notifications it's already advanced over are surely
	 * committed and need not be re-examined by us.  (We must consider only
	 * backends connected to our DB, because others will not have bothered to
	 * check committed-ness of notifications in our DB.)
	 *
	 * 有些情况下，大量已提交通知还没被裁掉（因为某个后端懒得去读）。
	 * 为缩短启动时间，可以查看其他后端，采用本数据库中任何后端最大的 pos 指针；
	 * 它已经越过的通知肯定已提交，我们不必再检查。
	 * （只能考虑连接到本数据库的后端，因为其他后端不会费心检查本库通知是否已提交。）
	 *
	 * We need exclusive lock here so we can look at other backends' entries
	 * and manipulate the list links.
	 *
	 * 这里需要排他锁，以便查看其他后端的项并操作链表指针。
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	head = QUEUE_HEAD;
	max = QUEUE_TAIL;
	prevListener = INVALID_PROC_NUMBER;
	for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
	{
		if (QUEUE_BACKEND_DBOID(i) == MyDatabaseId)
			max = QUEUE_POS_MAX(max, QUEUE_BACKEND_POS(i));
		/* Also find last listening backend before this one */
		/*
		 *
		 * 同时找出位于此后端之前的最后一个监听后端
		 */
		if (i < MyProcNumber)
			prevListener = i;
	}
	QUEUE_BACKEND_POS(MyProcNumber) = max;
	QUEUE_BACKEND_PID(MyProcNumber) = MyProcPid;
	QUEUE_BACKEND_DBOID(MyProcNumber) = MyDatabaseId;
	/* Insert backend into list of listeners at correct position */
	/*
	 *
	 * 把后端插入监听者列表的正确位置
	 */
	if (prevListener != INVALID_PROC_NUMBER)
	{
		QUEUE_NEXT_LISTENER(MyProcNumber) = QUEUE_NEXT_LISTENER(prevListener);
		QUEUE_NEXT_LISTENER(prevListener) = MyProcNumber;
	}
	else
	{
		QUEUE_NEXT_LISTENER(MyProcNumber) = QUEUE_FIRST_LISTENER;
		QUEUE_FIRST_LISTENER = MyProcNumber;
	}
	LWLockRelease(NotifyQueueLock);

	/* Now we are listed in the global array, so remember we're listening */
	/*
	 *
	 * 现在已列入全局数组，因此记住我们正在监听
	 */
	amRegisteredListener = true;

	/*
	 * Try to move our pointer forward as far as possible.  This will skip
	 * over already-committed notifications, which we want to do because they
	 * might be quite stale.  Note that we are not yet listening on anything,
	 * so we won't deliver such notifications to our frontend.  Also, although
	 * our transaction might have executed NOTIFY, those message(s) aren't
	 * queued yet so we won't skip them here.
	 *
	 * 尽量把指针向前移动。这会跳过已经提交的通知；我们希望这样做，因为它们可能已经相当旧。
	 * 注意此时还没有监听任何 channel，因此不会把这类通知交给前端。
	 * 另外，虽然本事务可能执行过 NOTIFY，那些消息尚未入队，所以这里不会跳过它们。
	 */
	if (!QUEUE_POS_EQUAL(max, head))
		asyncQueueReadAllNotifications();
}

/*
 * Exec_ListenCommit --- subroutine for AtCommit_Notify
 *
 * Exec_ListenCommit：AtCommit_Notify 的子程序
 *
 * Add the channel to the list of channels we are listening on.
 *
 * 把该 channel 加入我们正在监听的 channel 列表。
 */
static void
Exec_ListenCommit(const char *channel)
{
	MemoryContext oldcontext;

	/* Do nothing if we are already listening on this channel */
	/*
	 *
	 * 若已经在监听此 channel，则什么都不做
	 */
	if (IsListeningOn(channel))
		return;

	/*
	 * Add the new channel name to listenChannels.
	 *
	 * 把新的 channel 名加入 listenChannels。
	 *
	 * XXX It is theoretically possible to get an out-of-memory failure here,
	 * which would be bad because we already committed.  For the moment it
	 * doesn't seem worth trying to guard against that, but maybe improve this
	 * later.
	 *
	 * XXX：理论上这里可能发生内存不足，而我们已经提交，那会很糟糕。
	 * 目前似乎不值得专门防范，也许以后再改进。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	listenChannels = lappend(listenChannels, pstrdup(channel));
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Exec_UnlistenCommit --- subroutine for AtCommit_Notify
 *
 * Exec_UnlistenCommit：AtCommit_Notify 的子程序
 *
 * Remove the specified channel name from listenChannels.
 *
 * 从 listenChannels 中去掉指定的 channel 名。
 */
static void
Exec_UnlistenCommit(const char *channel)
{
	ListCell   *q;

	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenCommit(%s,%d)", channel, MyProcPid);

	foreach(q, listenChannels)
	{
		char	   *lchan = (char *) lfirst(q);

		if (strcmp(lchan, channel) == 0)
		{
			listenChannels = foreach_delete_current(listenChannels, q);
			pfree(lchan);
			break;
		}
	}

	/*
	 * We do not complain about unlistening something not being listened;
	 * should we?
	 *
	 * 对并未监听的对象执行 unlisten 时我们不抱怨；是否应该抱怨？
	 */
}

/*
 * Exec_UnlistenAllCommit --- subroutine for AtCommit_Notify
 *
 * Exec_UnlistenAllCommit：AtCommit_Notify 的子程序
 *
 *		Unlisten on all channels for this backend.
 *
 * 取消此后端对所有 channel 的监听。
 */
static void
Exec_UnlistenAllCommit(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenAllCommit(%d)", MyProcPid);

	list_free_deep(listenChannels);
	listenChannels = NIL;
}

/*
 * Test whether we are actively listening on the given channel name.
 *
 * 测试是否正在主动监听给定的 channel 名。
 *
 * Note: this function is executed for every notification found in the queue.
 * Perhaps it is worth further optimization, eg convert the list to a sorted
 * array so we can binary-search it.  In practice the list is likely to be
 * fairly short, though.
 *
 * 注意：队列中每找到一条通知都会执行此函数。也许值得进一步优化，
 * 例如把列表转成有序数组以便二分查找。不过实际上列表往往相当短。
 */
static bool
IsListeningOn(const char *channel)
{
	ListCell   *p;

	foreach(p, listenChannels)
	{
		char	   *lchan = (char *) lfirst(p);

		if (strcmp(lchan, channel) == 0)
			return true;
	}
	return false;
}

/*
 * Remove our entry from the listeners array when we are no longer listening
 * on any channel.  NB: must not fail if we're already not listening.
 *
 * 当不再监听任何 channel 时，从监听者数组中去掉我们的项。注意：若已经不在监听，也不得失败。
 */
static void
asyncQueueUnregister(void)
{
	Assert(listenChannels == NIL);	/* else caller error */
	/*
	 *
	 * 否则是调用方错误
	 */

	if (!amRegisteredListener)	/* nothing to do */
	/*
	 *
	 * 无事可做
	 */
		return;

	/*
	 * Need exclusive lock here to manipulate list links.
	 *
	 * 操作链表指针需要排他锁。
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	/* Mark our entry as invalid */
	/*
	 *
	 * 把我们的项标为无效
	 */
	QUEUE_BACKEND_PID(MyProcNumber) = InvalidPid;
	QUEUE_BACKEND_DBOID(MyProcNumber) = InvalidOid;
	/* and remove it from the list */
	/*
	 *
	 * 并从列表中移除
	 */
	if (QUEUE_FIRST_LISTENER == MyProcNumber)
		QUEUE_FIRST_LISTENER = QUEUE_NEXT_LISTENER(MyProcNumber);
	else
	{
		for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
		{
			if (QUEUE_NEXT_LISTENER(i) == MyProcNumber)
			{
				QUEUE_NEXT_LISTENER(i) = QUEUE_NEXT_LISTENER(MyProcNumber);
				break;
			}
		}
	}
	QUEUE_NEXT_LISTENER(MyProcNumber) = INVALID_PROC_NUMBER;
	LWLockRelease(NotifyQueueLock);

	/* mark ourselves as no longer listed in the global array */
	/*
	 *
	 * 标明自己不再位于全局数组中
	 */
	amRegisteredListener = false;
}

/*
 * Test whether there is room to insert more notification messages.
 *
 * 测试是否还有空间插入更多通知消息。
 *
 * Caller must hold at least shared NotifyQueueLock.
 *
 * 调用方必须至少持有共享的 NotifyQueueLock。
 */
static bool
asyncQueueIsFull(void)
{
	int64		headPage = QUEUE_POS_PAGE(QUEUE_HEAD);
	int64		tailPage = QUEUE_POS_PAGE(QUEUE_TAIL);
	int64		occupied = headPage - tailPage;

	return occupied >= max_notify_queue_pages;
}

/*
 * Advance the QueuePosition to the next entry, assuming that the current
 * entry is of length entryLength.  If we jump to a new page the function
 * returns true, else false.
 *
 * 把 QueuePosition 推进到下一项，假定当前项长度为 entryLength。
 * 若跳到新页则返回 true，否则返回 false。
 */
static bool
asyncQueueAdvance(volatile QueuePosition *position, int entryLength)
{
	int64		pageno = QUEUE_POS_PAGE(*position);
	int			offset = QUEUE_POS_OFFSET(*position);
	bool		pageJump = false;

	/*
	 * Move to the next writing position: First jump over what we have just
	 * written or read.
	 *
	 * 移到下一个写入位置：先跳过刚刚写过或读过的内容。
	 */
	offset += entryLength;
	Assert(offset <= QUEUE_PAGESIZE);

	/*
	 * In a second step check if another entry can possibly be written to the
	 * page. If so, stay here, we have reached the next position. If not, then
	 * we need to move on to the next page.
	 *
	 * 第二步检查本页是否还能写下一项。若能，就留在这里，已经到达下一个位置。
	 * 若不能，则需要移到下一页。
	 */
	if (offset + QUEUEALIGN(AsyncQueueEntryEmptySize) > QUEUE_PAGESIZE)
	{
		pageno++;
		offset = 0;
		pageJump = true;
	}

	SET_QUEUE_POS(*position, pageno, offset);
	return pageJump;
}

/*
 * Fill the AsyncQueueEntry at *qe with an outbound notification message.
 *
 * 用一条出站通知消息填充 *qe 处的 AsyncQueueEntry。
 */
static void
asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe)
{
	size_t		channellen = n->channel_len;
	size_t		payloadlen = n->payload_len;
	int			entryLength;

	Assert(channellen < NAMEDATALEN);
	Assert(payloadlen < NOTIFY_PAYLOAD_MAX_LENGTH);

	/* The terminators are already included in AsyncQueueEntryEmptySize */
	/*
	 *
	 * 结束符已经包含在 AsyncQueueEntryEmptySize 中
	 */
	entryLength = AsyncQueueEntryEmptySize + payloadlen + channellen;
	entryLength = QUEUEALIGN(entryLength);
	qe->length = entryLength;
	qe->dboid = MyDatabaseId;
	qe->xid = GetCurrentTransactionId();
	qe->srcPid = MyProcPid;
	memcpy(qe->data, n->data, channellen + payloadlen + 2);
}

/*
 * Add pending notifications to the queue.
 *
 * 把待处理通知加入队列。
 *
 * We go page by page here, i.e. we stop once we have to go to a new page but
 * we will be called again and then fill that next page. If an entry does not
 * fit into the current page, we write a dummy entry with an InvalidOid as the
 * database OID in order to fill the page. So every page is always used up to
 * the last byte which simplifies reading the page later.
 *
 * 这里按页处理：一旦必须进入新页就停下来，之后会再次被调用并填下一页。
 * 若一项放不进当前页，就写一个数据库 OID 为 InvalidOid 的哑元项来填满本页。
 * 因此每页总会用到最后一个字节，以后读页更简单。
 *
 * We are passed the list cell (in pendingNotifies->events) containing the next
 * notification to write and return the first still-unwritten cell back.
 * Eventually we will return NULL indicating all is done.
 *
 * 传入的是 pendingNotifies->events 中包含下一条待写通知的链表单元，
 * 返回第一个仍未写完的单元。最终返回 NULL，表示全部完成。
 *
 * We are holding NotifyQueueLock already from the caller and grab
 * page specific SLRU bank lock locally in this function.
 *
 * 调用方已经持有 NotifyQueueLock，本函数内再局部获取页对应的 SLRU bank 锁。
 */
static ListCell *
asyncQueueAddEntries(ListCell *nextNotify)
{
	AsyncQueueEntry qe;
	QueuePosition queue_head;
	int64		pageno;
	int			offset;
	int			slotno;
	LWLock	   *prevlock;

	/*
	 * We work with a local copy of QUEUE_HEAD, which we write back to shared
	 * memory upon exiting.  The reason for this is that if we have to advance
	 * to a new page, SimpleLruZeroPage might fail (out of disk space, for
	 * instance), and we must not advance QUEUE_HEAD if it does.  (Otherwise,
	 * subsequent insertions would try to put entries into a page that slru.c
	 * thinks doesn't exist yet.)  So, use a local position variable.  Note
	 * that if we do fail, any already-inserted queue entries are forgotten;
	 * this is okay, since they'd be useless anyway after our transaction
	 * rolls back.
	 *
	 * 我们使用 QUEUE_HEAD 的本地副本，退出时再写回共享内存。
	 * 原因是：若必须推进到新页，SimpleLruZeroPage 可能失败（例如磁盘空间不足），
	 * 失败时绝不能推进 QUEUE_HEAD。（否则后续插入会试图把项放入 slru.c 认为尚不存在的页。）
	 * 因此使用局部位置变量。若失败，已经插入的队列项会被忘掉；这是可以的，因为事务回滚后它们本来也无用。
	 */
	queue_head = QUEUE_HEAD;

	/*
	 * If this is the first write since the postmaster started, we need to
	 * initialize the first page of the async SLRU.  Otherwise, the current
	 * page should be initialized already, so just fetch it.
	 *
	 * 若这是 postmaster 启动后的第一次写入，需要初始化 async SLRU 的第一页。
	 * 否则当前页应当已经初始化，直接取出来即可。
	 */
	pageno = QUEUE_POS_PAGE(queue_head);
	prevlock = SimpleLruGetBankLock(NotifyCtl, pageno);

	/* We hold both NotifyQueueLock and SLRU bank lock during this operation */
	/*
	 *
	 * 此操作期间同时持有 NotifyQueueLock 和 SLRU bank 锁
	 */
	LWLockAcquire(prevlock, LW_EXCLUSIVE);

	if (QUEUE_POS_IS_ZERO(queue_head))
		slotno = SimpleLruZeroPage(NotifyCtl, pageno);
	else
		slotno = SimpleLruReadPage(NotifyCtl, pageno, true,
								   InvalidTransactionId);

	/* Note we mark the page dirty before writing in it */
	/*
	 *
	 * 注意在写入之前先把页标为脏
	 */
	NotifyCtl->shared->page_dirty[slotno] = true;

	while (nextNotify != NULL)
	{
		Notification *n = (Notification *) lfirst(nextNotify);

		/* Construct a valid queue entry in local variable qe */
		/*
		 *
		 * 在局部变量 qe 中构造一条有效的队列项
		 */
		asyncQueueNotificationToEntry(n, &qe);

		offset = QUEUE_POS_OFFSET(queue_head);

		/* Check whether the entry really fits on the current page */
		/*
		 *
		 * 检查该项是否真的放得进当前页
		 */
		if (offset + qe.length <= QUEUE_PAGESIZE)
		{
			/* OK, so advance nextNotify past this item */
			/*
			 *
			 * 可以，于是把 nextNotify 移过这一项
			 */
			nextNotify = lnext(pendingNotifies->events, nextNotify);
		}
		else
		{
			/*
			 * Write a dummy entry to fill up the page. Actually readers will
			 * only check dboid and since it won't match any reader's database
			 * OID, they will ignore this entry and move on.
			 *
			 * 写一个哑元项填满本页。读者实际上只检查 dboid，它不会匹配任何读者的数据库 OID，
			 * 因此读者会忽略此项并继续。
			 */
			qe.length = QUEUE_PAGESIZE - offset;
			qe.dboid = InvalidOid;
			qe.xid = InvalidTransactionId;
			qe.data[0] = '\0';	/* empty channel */
			/*
			 *
			 * 空的 channel
			 */
			qe.data[1] = '\0';	/* empty payload */
			/*
			 *
			 * 空的 payload
			 */
		}

		/* Now copy qe into the shared buffer page */
		/*
		 *
		 * 现在把 qe 复制到共享缓冲页
		 */
		memcpy(NotifyCtl->shared->page_buffer[slotno] + offset,
			   &qe,
			   qe.length);

		/* Advance queue_head appropriately, and detect if page is full */
		/*
		 *
		 * 适当地推进 queue_head，并检测页是否已满
		 */
		if (asyncQueueAdvance(&(queue_head), qe.length))
		{
			LWLock	   *lock;

			pageno = QUEUE_POS_PAGE(queue_head);
			lock = SimpleLruGetBankLock(NotifyCtl, pageno);
			if (lock != prevlock)
			{
				LWLockRelease(prevlock);
				LWLockAcquire(lock, LW_EXCLUSIVE);
				prevlock = lock;
			}

			/*
			 * Page is full, so we're done here, but first fill the next page
			 * with zeroes.  The reason to do this is to ensure that slru.c's
			 * idea of the head page is always the same as ours, which avoids
			 * boundary problems in SimpleLruTruncate.  The test in
			 * asyncQueueIsFull() ensured that there is room to create this
			 * page without overrunning the queue.
			 *
			 * 页已满，因此这里结束，但先把下一页填零。
			 * 这样做是为了让 slru.c 认定的头页始终与我们一致，避免 SimpleLruTruncate 的边界问题。
			 * asyncQueueIsFull() 中的检查已保证创建此页不会越出队列。
			 */
			slotno = SimpleLruZeroPage(NotifyCtl, QUEUE_POS_PAGE(queue_head));

			/*
			 * If the new page address is a multiple of QUEUE_CLEANUP_DELAY,
			 * set flag to remember that we should try to advance the tail
			 * pointer (we don't want to actually do that right here).
			 *
			 * 若新页地址是 QUEUE_CLEANUP_DELAY 的倍数，设置标志，记住应尝试推进尾指针（不想就在这里真正去做）。
			 */
			if (QUEUE_POS_PAGE(queue_head) % QUEUE_CLEANUP_DELAY == 0)
				tryAdvanceTail = true;

			/* And exit the loop */
			/*
			 *
			 * 然后退出循环
			 */
			break;
		}
	}

	/* Success, so update the global QUEUE_HEAD */
	/*
	 *
	 * 成功，因此更新全局 QUEUE_HEAD
	 */
	QUEUE_HEAD = queue_head;

	LWLockRelease(prevlock);

	return nextNotify;
}

/*
 * SQL function to return the fraction of the notification queue currently
 * occupied.
 *
 * SQL 函数：返回通知队列当前被占用的比例。
 */
Datum
pg_notification_queue_usage(PG_FUNCTION_ARGS)
{
	double		usage;

	/* Advance the queue tail so we don't report a too-large result */
	/*
	 *
	 * 推进队列尾，以免报告过大的结果
	 */
	asyncQueueAdvanceTail();

	LWLockAcquire(NotifyQueueLock, LW_SHARED);
	usage = asyncQueueUsage();
	LWLockRelease(NotifyQueueLock);

	PG_RETURN_FLOAT8(usage);
}

/*
 * Return the fraction of the queue that is currently occupied.
 *
 * 返回队列当前被占用的比例。
 *
 * The caller must hold NotifyQueueLock in (at least) shared mode.
 *
 * 调用方必须至少以共享模式持有 NotifyQueueLock。
 *
 * Note: we measure the distance to the logical tail page, not the physical
 * tail page.  In some sense that's wrong, but the relative position of the
 * physical tail is affected by details such as SLRU segment boundaries,
 * so that a result based on that is unpleasantly unstable.
 *
 * 注意：我们度量到逻辑尾页的距离，而不是物理尾页。某种意义上这是错的，
 * 但物理尾的相对位置受 SLRU 段边界等细节影响，基于它的结果会不稳定得令人不快。
 */
static double
asyncQueueUsage(void)
{
	int64		headPage = QUEUE_POS_PAGE(QUEUE_HEAD);
	int64		tailPage = QUEUE_POS_PAGE(QUEUE_TAIL);
	int64		occupied = headPage - tailPage;

	if (occupied == 0)
		return (double) 0;		/* fast exit for common case */
		/*
		 *
		 * 常见情况的快速退出
		 */

	return (double) occupied / (double) max_notify_queue_pages;
}

/*
 * Check whether the queue is at least half full, and emit a warning if so.
 *
 * 检查队列是否至少半满，若是则发出警告。
 *
 * This is unlikely given the size of the queue, but possible.
 * The warnings show up at most once every QUEUE_FULL_WARN_INTERVAL.
 *
 * 考虑到队列的大小，这不太可能，但有可能。警告最多每 QUEUE_FULL_WARN_INTERVAL 出现一次。
 *
 * Caller must hold exclusive NotifyQueueLock.
 *
 * 调用方必须持有排他的 NotifyQueueLock。
 */
static void
asyncQueueFillWarning(void)
{
	double		fillDegree;
	TimestampTz t;

	fillDegree = asyncQueueUsage();
	if (fillDegree < 0.5)
		return;

	t = GetCurrentTimestamp();

	if (TimestampDifferenceExceeds(asyncQueueControl->lastQueueFillWarn,
								   t, QUEUE_FULL_WARN_INTERVAL))
	{
		QueuePosition min = QUEUE_HEAD;
		int32		minPid = InvalidPid;

		for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
		{
			Assert(QUEUE_BACKEND_PID(i) != InvalidPid);
			min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
			if (QUEUE_POS_EQUAL(min, QUEUE_BACKEND_POS(i)))
				minPid = QUEUE_BACKEND_PID(i);
		}

		ereport(WARNING,
				(errmsg("NOTIFY queue is %.0f%% full", fillDegree * 100),
				 (minPid != InvalidPid ?
				  errdetail("The server process with PID %d is among those with the oldest transactions.", minPid)
				  : 0),
				 (minPid != InvalidPid ?
				  errhint("The NOTIFY queue cannot be emptied until that process ends its current transaction.")
				  : 0)));

		asyncQueueControl->lastQueueFillWarn = t;
	}
}

/*
 * Send signals to listening backends.
 *
 * 向正在监听的后端发送信号。
 *
 * Normally we signal only backends in our own database, since only those
 * backends could be interested in notifies we send.  However, if there's
 * notify traffic in our database but no traffic in another database that
 * does have listener(s), those listeners will fall further and further
 * behind.  Waken them anyway if they're far enough behind, so that they'll
 * advance their queue position pointers, allowing the global tail to advance.
 *
 * 通常只向本数据库中的后端发信号，因为只有它们可能对我们发出的通知感兴趣。
 * 但是，若本库有通知流量，而另一个有监听者的数据库没有流量，那些监听者会越落越远。
 * 若它们落后得够远，仍然唤醒它们，以便推进队列位置指针，使全局尾可以前进。
 *
 * Since we know the ProcNumber and the Pid the signaling is quite cheap.
 *
 * 因为已知 ProcNumber 和 Pid，发信号相当便宜。
 *
 * This is called during CommitTransaction(), so it's important for it
 * to have very low probability of failure.
 *
 * 这在 CommitTransaction() 期间调用，因此失败概率必须非常低。
 */
static void
SignalBackends(void)
{
	int32	   *pids;
	ProcNumber *procnos;
	int			count;

	/*
	 * Identify backends that we need to signal.  We don't want to send
	 * signals while holding the NotifyQueueLock, so this loop just builds a
	 * list of target PIDs.
	 *
	 * 找出需要发信号的后端。不想在持有 NotifyQueueLock 时发信号，因此这个循环只建立目标 PID 列表。
	 *
	 * XXX in principle these pallocs could fail, which would be bad. Maybe
	 * preallocate the arrays?  They're not that large, though.
	 *
	 * XXX：原则上这些 palloc 可能失败，那会很糟糕。也许应预先分配数组？不过它们并不大。
	 */
	pids = (int32 *) palloc(MaxBackends * sizeof(int32));
	procnos = (ProcNumber *) palloc(MaxBackends * sizeof(ProcNumber));
	count = 0;

	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
	{
		int32		pid = QUEUE_BACKEND_PID(i);
		QueuePosition pos;

		Assert(pid != InvalidPid);
		pos = QUEUE_BACKEND_POS(i);
		if (QUEUE_BACKEND_DBOID(i) == MyDatabaseId)
		{
			/*
			 * Always signal listeners in our own database, unless they're
			 * already caught up (unlikely, but possible).
			 *
			 * 总是向本数据库中的监听者发信号，除非它们已经赶上（不太可能，但有可能）。
			 */
			if (QUEUE_POS_EQUAL(pos, QUEUE_HEAD))
				continue;
		}
		else
		{
			/*
			 * Listeners in other databases should be signaled only if they
			 * are far behind.
			 *
			 * 其他数据库中的监听者只有落后很远时才应收到信号。
			 */
			if (asyncQueuePageDiff(QUEUE_POS_PAGE(QUEUE_HEAD),
								   QUEUE_POS_PAGE(pos)) < QUEUE_CLEANUP_DELAY)
				continue;
		}
		/* OK, need to signal this one */
		/*
		 *
		 * 好，需要向这一个发信号
		 */
		pids[count] = pid;
		procnos[count] = i;
		count++;
	}
	LWLockRelease(NotifyQueueLock);

	/* Now send signals */
	/*
	 *
	 * 现在发送信号
	 */
	for (int i = 0; i < count; i++)
	{
		int32		pid = pids[i];

		/*
		 * If we are signaling our own process, no need to involve the kernel;
		 * just set the flag directly.
		 *
		 * 若信号发给自己的进程，不必经过内核；直接设置标志。
		 */
		if (pid == MyProcPid)
		{
			notifyInterruptPending = true;
			continue;
		}

		/*
		 * Note: assuming things aren't broken, a signal failure here could
		 * only occur if the target backend exited since we released
		 * NotifyQueueLock; which is unlikely but certainly possible. So we
		 * just log a low-level debug message if it happens.
		 *
		 * 注意：假定系统没有损坏，这里的信号失败只可能是因为目标后端在我们释放 NotifyQueueLock 之后退出了；
		 * 这不太可能但确实可能发生。因此发生时只记录一条低级别的调试消息。
		 */
		if (SendProcSignal(pid, PROCSIG_NOTIFY_INTERRUPT, procnos[i]) < 0)
			elog(DEBUG3, "could not signal backend with PID %d: %m", pid);
	}

	pfree(pids);
	pfree(procnos);
}

/*
 * AtAbort_Notify
 *
 * 函数 AtAbort_Notify。
 *
 *	This is called at transaction abort.
 *
 * 在事务中止时调用。
 *
 *	Gets rid of pending actions and outbound notifies that we would have
 *	executed if the transaction got committed.
 *
 * 丢掉若事务提交本来会执行的待处理动作和出站通知。
 */
void
AtAbort_Notify(void)
{
	/*
	 * If we LISTEN but then roll back the transaction after PreCommit_Notify,
	 * we have registered as a listener but have not made any entry in
	 * listenChannels.  In that case, deregister again.
	 *
	 * 若我们 LISTEN 了，但在 PreCommit_Notify 之后回滚事务，则已经登记为监听者，
	 * 却没有在 listenChannels 中留下任何项。这种情况下再次注销。
	 */
	if (amRegisteredListener && listenChannels == NIL)
		asyncQueueUnregister();

	/* And clean up */
	/*
	 *
	 * 然后清理
	 */
	ClearPendingActionsAndNotifies();
}

/*
 * AtSubCommit_Notify() --- Take care of subtransaction commit.
 *
 * AtSubCommit_Notify()：处理子事务提交。
 *
 * Reassign all items in the pending lists to the parent transaction.
 *
 * 把待处理列表中的所有项重新归给父事务。
 */
void
AtSubCommit_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* If there are actions at our nesting level, we must reparent them. */
	/*
	 *
	 * 若本嵌套层有动作，必须把它们重新挂到父事务。
	 */
	if (pendingActions != NULL &&
		pendingActions->nestingLevel >= my_level)
	{
		if (pendingActions->upper == NULL ||
			pendingActions->upper->nestingLevel < my_level - 1)
		{
			/* nothing to merge; give the whole thing to the parent */
			/*
			 *
			 * 没有需要合并的内容；把整个列表交给父事务
			 */
			--pendingActions->nestingLevel;
		}
		else
		{
			ActionList *childPendingActions = pendingActions;

			pendingActions = pendingActions->upper;

			/*
			 * Mustn't try to eliminate duplicates here --- see queue_listen()
			 *
			 * 这里不要试图消除重复，见 queue_listen()
			 */
			pendingActions->actions =
				list_concat(pendingActions->actions,
							childPendingActions->actions);
			pfree(childPendingActions);
		}
	}

	/* If there are notifies at our nesting level, we must reparent them. */
	/*
	 *
	 * 若本嵌套层有通知，必须把它们重新挂到父事务。
	 */
	if (pendingNotifies != NULL &&
		pendingNotifies->nestingLevel >= my_level)
	{
		Assert(pendingNotifies->nestingLevel == my_level);

		if (pendingNotifies->upper == NULL ||
			pendingNotifies->upper->nestingLevel < my_level - 1)
		{
			/* nothing to merge; give the whole thing to the parent */
			/*
			 *
			 * 没有需要合并的内容；把整个列表交给父事务
			 */
			--pendingNotifies->nestingLevel;
		}
		else
		{
			/*
			 * Formerly, we didn't bother to eliminate duplicates here, but
			 * now we must, else we fall foul of "Assert(!found)", either here
			 * or during a later attempt to build the parent-level hashtable.
			 *
			 * 以前这里懒得消除重复，但现在必须消除，否则会触发 Assert(!found)，
			 * 要么在这里，要么在以后尝试建立父层哈希表时。
			 */
			NotificationList *childPendingNotifies = pendingNotifies;
			ListCell   *l;

			pendingNotifies = pendingNotifies->upper;
			/* Insert all the subxact's events into parent, except for dups */
			/*
			 *
			 * 把子事务的所有事件插入父事务，重复的除外
			 */
			foreach(l, childPendingNotifies->events)
			{
				Notification *childn = (Notification *) lfirst(l);

				if (!AsyncExistsPendingNotify(childn))
					AddEventToPendingNotifies(childn);
			}
			pfree(childPendingNotifies);
		}
	}
}

/*
 * AtSubAbort_Notify() --- Take care of subtransaction abort.
 *
 * AtSubAbort_Notify()：处理子事务中止。
 */
void
AtSubAbort_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * All we have to do is pop the stack --- the actions/notifies made in
	 * this subxact are no longer interesting, and the space will be freed
	 * when CurTransactionContext is recycled. We still have to free the
	 * ActionList and NotificationList objects themselves, though, because
	 * those are allocated in TopTransactionContext.
	 *
	 * 要做的只是弹出栈：本子事务中的动作和通知不再有意义，
	 * CurTransactionContext 被回收时空间会释放。
	 * 但仍必须释放 ActionList 和 NotificationList 对象本身，因为它们分配在 TopTransactionContext 中。
	 *
	 * Note that there might be no entries at all, or no entries for the
	 * current subtransaction level, either because none were ever created, or
	 * because we reentered this routine due to trouble during subxact abort.
	 *
	 * 注意可能根本没有项，或没有当前子事务层的项，
	 * 要么因为从未创建，要么因为子事务中止过程中出问题而再次进入此例程。
	 */
	while (pendingActions != NULL &&
		   pendingActions->nestingLevel >= my_level)
	{
		ActionList *childPendingActions = pendingActions;

		pendingActions = pendingActions->upper;
		pfree(childPendingActions);
	}

	while (pendingNotifies != NULL &&
		   pendingNotifies->nestingLevel >= my_level)
	{
		NotificationList *childPendingNotifies = pendingNotifies;

		pendingNotifies = pendingNotifies->upper;
		pfree(childPendingNotifies);
	}
}

/*
 * HandleNotifyInterrupt
 *
 * 函数 HandleNotifyInterrupt。
 *
 *		Signal handler portion of interrupt handling. Let the backend know
 *		that there's a pending notify interrupt. If we're currently reading
 *		from the client, this will interrupt the read and
 *		ProcessClientReadInterrupt() will call ProcessNotifyInterrupt().
 *
 * 中断处理中的信号处理部分。让后端知道有待处理的通知中断。
 * 若当前正在从客户端读取，这次读取会被打断，ProcessClientReadInterrupt() 将调用 ProcessNotifyInterrupt()。
 */
void
HandleNotifyInterrupt(void)
{
	/*
	 * Note: this is called by a SIGNAL HANDLER. You must be very wary what
	 * you do here.
	 *
	 * 注意：这是由信号处理函数调用的。在这里做什么都必须非常小心。
	 */

	/* signal that work needs to be done */
	/*
	 *
	 * 表明有工作要做
	 */
	notifyInterruptPending = true;

	/* make sure the event is processed in due course */
	/*
	 *
	 * 确保该事件会在适当的时候被处理
	 */
	SetLatch(MyLatch);
}

/*
 * ProcessNotifyInterrupt
 *
 * 函数 ProcessNotifyInterrupt。
 *
 *		This is called if we see notifyInterruptPending set, just before
 *		transmitting ReadyForQuery at the end of a frontend command, and
 *		also if a notify signal occurs while reading from the frontend.
 *		HandleNotifyInterrupt() will cause the read to be interrupted
 *		via the process's latch, and this routine will get called.
 *		If we are truly idle (ie, *not* inside a transaction block),
 *		process the incoming notifies.
 *
 * 若看到 notifyInterruptPending 被设置，则在前端命令结束、发送 ReadyForQuery 之前调用；
 * 从前端读取时若发生通知信号也会调用。HandleNotifyInterrupt() 会通过进程 latch 打断读取，然后进入本例程。
 * 若真正空闲（即不在事务块内），则处理到来的通知。
 *
 *		If "flush" is true, force any frontend messages out immediately.
 *		This can be false when being called at the end of a frontend command,
 *		since we'll flush after sending ReadyForQuery.
 *
 * 若 flush 为真，立即把任何前端消息刷出去。
 * 在前端命令结束时调用可以把 flush 设为假，因为发送 ReadyForQuery 之后还会再刷。
 */
void
ProcessNotifyInterrupt(bool flush)
{
	if (IsTransactionOrTransactionBlock())
		return;					/* not really idle */
		/*
		 *
		 * 并非真正空闲
		 */

	/* Loop in case another signal arrives while sending messages */
	/*
	 *
	 * 循环，以防发送消息期间又有信号到达
	 */
	while (notifyInterruptPending)
		ProcessIncomingNotify(flush);
}


/*
 * Read all pending notifications from the queue, and deliver appropriate
 * ones to my frontend.  Stop when we reach queue head or an uncommitted
 * notification.
 *
 * 从队列读出所有待处理通知，并把合适的那些交给我的前端。
 * 到达队列头或一条未提交的通知时停止。
 */
static void
asyncQueueReadAllNotifications(void)
{
	QueuePosition pos;
	QueuePosition head;
	Snapshot	snapshot;

	/* Fetch current state */
	/*
	 *
	 * 取得当前状态
	 */
	LWLockAcquire(NotifyQueueLock, LW_SHARED);
	/* Assert checks that we have a valid state entry */
	/*
	 *
	 * Assert 检查我们有一条有效的状态项
	 */
	Assert(MyProcPid == QUEUE_BACKEND_PID(MyProcNumber));
	pos = QUEUE_BACKEND_POS(MyProcNumber);
	head = QUEUE_HEAD;
	LWLockRelease(NotifyQueueLock);

	if (QUEUE_POS_EQUAL(pos, head))
	{
		/* Nothing to do, we have read all notifications already. */
		/*
		 *
		 * 无事可做，通知已经全部读完。
		 */
		return;
	}

	/*----------
	 * Get snapshot we'll use to decide which xacts are still in progress.
	 * This is trickier than it might seem, because of race conditions.
	 * Consider the following example:
	 *
	 * 取得用来判断哪些事务仍在进行的快照。这比看起来更棘手，因为有竞态。考虑下面的例子：
	 *
	 * Backend 1:					 Backend 2:
	 *
	 * 后端 1 与后端 2 的交错时序：
	 *
	 * transaction starts
	 * UPDATE foo SET ...;
	 * NOTIFY foo;
	 * commit starts
	 * queue the notify message
	 *								 transaction starts
	 *								 LISTEN foo;  -- first LISTEN in session
	 *								 SELECT * FROM foo WHERE ...;
	 * commit to clog
	 *								 commit starts
	 *								 add backend 2 to array of listeners
	 *								 advance to queue head (this code)
	 *								 commit to clog
	 *
	 * Transaction 2's SELECT has not seen the UPDATE's effects, since that
	 * wasn't committed yet.  Ideally we'd ensure that client 2 would
	 * eventually get transaction 1's notify message, but there's no way
	 * to do that; until we're in the listener array, there's no guarantee
	 * that the notify message doesn't get removed from the queue.
	 *
	 * 事务 2 的 SELECT 没有看到 UPDATE 的效果，因为那时尚未提交。
	 * 理想情况下应保证客户端 2 最终能收到事务 1 的通知消息，但无法做到：
	 * 在进入监听者数组之前，不能保证通知消息不会被从队列中删掉。
	 *
	 * Therefore the coding technique transaction 2 is using is unsafe:
	 * applications must commit a LISTEN before inspecting database state,
	 * if they want to ensure they will see notifications about subsequent
	 * changes to that state.
	 *
	 * 因此事务 2 所用的编程方式是不安全的：若应用希望看到随后对该状态的变更通知，
	 * 必须先提交 LISTEN，再检查数据库状态。
	 *
	 * What we do guarantee is that we'll see all notifications from
	 * transactions committing after the snapshot we take here.
	 * Exec_ListenPreCommit has already added us to the listener array,
	 * so no not-yet-committed messages can be removed from the queue
	 * before we see them.
	 *
	 * 我们保证能看到在此处所取快照之后提交的事务所发出的全部通知。
	 * Exec_ListenPreCommit 已经把我们加入监听者数组，因此尚未提交的消息在我们看到之前不会被从队列中移除。
	 *----------
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());

	/*
	 * It is possible that we fail while trying to send a message to our
	 * frontend (for example, because of encoding conversion failure).  If
	 * that happens it is critical that we not try to send the same message
	 * over and over again.  Therefore, we set ExitOnAnyError to upgrade any
	 * ERRORs to FATAL, causing the client connection to be closed on error.
	 *
	 * 向前端发送消息时可能失败（例如编码转换失败）。若发生这种情况，
	 * 关键是不要一遍遍重试同一条消息。因此设置 ExitOnAnyError，把任何 ERROR 升级为 FATAL，出错时关闭客户端连接。
	 *
	 * We used to only skip over the offending message and try to soldier on,
	 * but it was somewhat questionable to lose a notification and give the
	 * client an ERROR instead.  A client application is not be prepared for
	 * that and can't tell that a notification was missed.  It was also not
	 * very useful in practice because notifications are often processed while
	 * a connection is idle and reading a message from the client, and in that
	 * state, any error is upgraded to FATAL anyway.  Closing the connection
	 * is a clear signal to the application that it might have missed
	 * notifications.
	 *
	 * 以前只跳过那条有问题的消息并试图继续，但丢掉一条通知并给客户端一个 ERROR 有些说不通。
	 * 客户端应用对此没有准备，也无法知道漏了一条通知。实践中这也没什么用，
	 * 因为通知常常在连接空闲、正在读客户端消息时处理，而在那种状态下任何错误反正都会升级为 FATAL。
	 * 关闭连接是向应用明确表示它可能漏掉了通知。
	 */
	{
		bool		save_ExitOnAnyError = ExitOnAnyError;
		bool		reachedStop;

		ExitOnAnyError = true;

		do
		{
			/*
			 * Process messages up to the stop position, end of page, or an
			 * uncommitted message.
			 *
			 * 处理消息，直到停止位置、页尾或一条未提交的消息。
			 *
			 * Our stop position is what we found to be the head's position
			 * when we entered this function. It might have changed already.
			 * But if it has, we will receive (or have already received and
			 * queued) another signal and come here again.
			 *
			 * 停止位置是进入本函数时看到的头位置。它可能已经变了。
			 * 但若变了，我们会收到（或已经收到并排队）另一个信号，然后再次来到这里。
			 *
			 * We are not holding NotifyQueueLock here! The queue can only
			 * extend beyond the head pointer (see above) and we leave our
			 * backend's pointer where it is so nobody will truncate or
			 * rewrite pages under us. Especially we don't want to hold a lock
			 * while sending the notifications to the frontend.
			 *
			 * 这里没有持有 NotifyQueueLock！队列只能延伸到头指针之外（见上文），
			 * 我们把自己的后端指针留在原地，因此没人会在我们脚下截断或重写页面。
			 * 尤其不想在向前端发送通知时持有锁。
			 */
			reachedStop = asyncQueueProcessPageEntries(&pos, head, snapshot);
		} while (!reachedStop);

		/* Update shared state */
		/*
		 *
		 * 更新共享状态
		 */
		LWLockAcquire(NotifyQueueLock, LW_SHARED);
		QUEUE_BACKEND_POS(MyProcNumber) = pos;
		LWLockRelease(NotifyQueueLock);

		ExitOnAnyError = save_ExitOnAnyError;
	}

	/* Done with snapshot */
	/*
	 *
	 * 快照用完了
	 */
	UnregisterSnapshot(snapshot);
}

/*
 * Fetch notifications from the shared queue, beginning at position current,
 * and deliver relevant ones to my frontend.
 *
 * 从共享队列的 current 位置开始取出通知，并把相关的那些交给我的前端。
 *
 * The function returns true once we have reached the stop position or an
 * uncommitted notification, and false if we have finished with the page.
 * In other words: once it returns true there is no need to look further.
 * The QueuePosition *current is advanced past all processed messages.
 *
 * 到达停止位置或一条未提交的通知时函数返回 true；若本页已经处理完则返回 false。
 * 也就是说，一旦返回 true，就不必再往后看。QueuePosition *current 会推进到所有已处理消息之后。
 */
static bool
asyncQueueProcessPageEntries(QueuePosition *current,
							 QueuePosition stop,
							 Snapshot snapshot)
{
	int64		curpage = QUEUE_POS_PAGE(*current);
	int			slotno;
	char	   *page_buffer;
	bool		reachedStop = false;
	bool		reachedEndOfPage;

	/*
	 * We copy the entries into a local buffer to avoid holding the SLRU lock
	 * while we transmit them to our frontend.  The local buffer must be
	 * adequately aligned, so use a union.
	 *
	 * 把项复制到本地缓冲区，以免在向前端传输时持有 SLRU 锁。本地缓冲区必须充分对齐，因此使用联合体。
	 */
	union
	{
		char		buf[QUEUE_PAGESIZE];
		AsyncQueueEntry align;
	}			local_buf;
	char	   *local_buf_end = local_buf.buf;

	slotno = SimpleLruReadPage_ReadOnly(NotifyCtl, curpage,
										InvalidTransactionId);
	page_buffer = NotifyCtl->shared->page_buffer[slotno];

	do
	{
		QueuePosition thisentry = *current;
		AsyncQueueEntry *qe;

		if (QUEUE_POS_EQUAL(thisentry, stop))
			break;

		qe = (AsyncQueueEntry *) (page_buffer + QUEUE_POS_OFFSET(thisentry));

		/*
		 * Advance *current over this message, possibly to the next page. As
		 * noted in the comments for asyncQueueReadAllNotifications, we must
		 * do this before possibly failing while processing the message.
		 *
		 * 把 *current 移过这条消息，可能进入下一页。如 asyncQueueReadAllNotifications 的注释所述，
		 * 必须在处理消息可能失败之前做这件事。
		 */
		reachedEndOfPage = asyncQueueAdvance(current, qe->length);

		/* Ignore messages destined for other databases */
		/*
		 *
		 * 忽略发给其他数据库的消息
		 */
		if (qe->dboid == MyDatabaseId)
		{
			if (XidInMVCCSnapshot(qe->xid, snapshot))
			{
				/*
				 * The source transaction is still in progress, so we can't
				 * process this message yet.  Break out of the loop, but first
				 * back up *current so we will reprocess the message next
				 * time.  (Note: it is unlikely but not impossible for
				 * TransactionIdDidCommit to fail, so we can't really avoid
				 * this advance-then-back-up behavior when dealing with an
				 * uncommitted message.)
				 *
				 * 源事务仍在进行，因此还不能处理这条消息。跳出循环，但先把 *current 退回去，下次再处理这条消息。
				 * （注意：TransactionIdDidCommit 失败虽然不太可能，但并非不可能，
				 * 因此处理未提交消息时，这种先前进再退回的行为无法真正避免。）
				 *
				 * Note that we must test XidInMVCCSnapshot before we test
				 * TransactionIdDidCommit, else we might return a message from
				 * a transaction that is not yet visible to snapshots; compare
				 * the comments at the head of heapam_visibility.c.
				 *
				 * 注意必须先测试 XidInMVCCSnapshot，再测试 TransactionIdDidCommit，
				 * 否则可能返回一条对快照尚不可见的事务的消息；对照 heapam_visibility.c 开头的注释。
				 *
				 * Also, while our own xact won't be listed in the snapshot,
				 * we need not check for TransactionIdIsCurrentTransactionId
				 * because our transaction cannot (yet) have queued any
				 * messages.
				 *
				 * 另外，虽然我们自己的事务不会出现在快照里，也不必检查 TransactionIdIsCurrentTransactionId，
				 * 因为我们的事务还不可能已经把任何消息入队。
				 */
				*current = thisentry;
				reachedStop = true;
				break;
			}

			/*
			 * Quick check for the case that we're not listening on any
			 * channels, before calling TransactionIdDidCommit().  This makes
			 * that case a little faster, but more importantly, it ensures
			 * that if there's a bad entry in the queue for which
			 * TransactionIdDidCommit() fails for some reason, we can skip
			 * over it on the first LISTEN in a session, and not get stuck on
			 * it indefinitely.
			 *
			 * 在调用 TransactionIdDidCommit() 之前，先快速检查我们是否没有监听任何 channel。
			 * 这让那种情况稍快一点，更重要的是：若队列里有一条坏项，TransactionIdDidCommit() 因某种原因失败，
			 * 我们可以在会话的第一次 LISTEN 时跳过它，而不会永远卡在上面。
			 */
			if (listenChannels == NIL)
				continue;

			if (TransactionIdDidCommit(qe->xid))
			{
				memcpy(local_buf_end, qe, qe->length);
				local_buf_end += qe->length;
			}
			else
			{
				/*
				 * The source transaction aborted or crashed, so we just
				 * ignore its notifications.
				 *
				 * 源事务已中止或崩溃，因此直接忽略它的通知。
				 */
			}
		}

		/* Loop back if we're not at end of page */
		/*
		 *
		 * 若还没到页尾则回到循环
		 */
	} while (!reachedEndOfPage);

	/* Release lock that we got from SimpleLruReadPage_ReadOnly() */
	/*
	 *
	 * 释放从 SimpleLruReadPage_ReadOnly() 得到的锁
	 */
	LWLockRelease(SimpleLruGetBankLock(NotifyCtl, curpage));

	/*
	 * Now that we have let go of the SLRU bank lock, send the notifications
	 * to our backend
	 *
	 * 已经放开 SLRU bank 锁，现在把通知发给我们的后端
	 */
	Assert(local_buf_end - local_buf.buf <= BLCKSZ);
	for (char *p = local_buf.buf; p < local_buf_end;)
	{
		AsyncQueueEntry *qe = (AsyncQueueEntry *) p;

		/* qe->data is the null-terminated channel name */
		/*
		 *
		 * qe->data 是以 null 结尾的 channel 名
		 */
		char	   *channel = qe->data;

		if (IsListeningOn(channel))
		{
			/* payload follows channel name */
			/*
			 *
			 * payload 跟在 channel 名之后
			 */
			char	   *payload = qe->data + strlen(channel) + 1;

			NotifyMyFrontEnd(channel, payload, qe->srcPid);
		}

		p += qe->length;
	}

	if (QUEUE_POS_EQUAL(*current, stop))
		reachedStop = true;

	return reachedStop;
}

/*
 * Advance the shared queue tail variable to the minimum of all the
 * per-backend tail pointers.  Truncate pg_notify space if possible.
 *
 * 把共享队列尾变量推进到所有后端尾指针的最小值。若可能，截断 pg_notify 空间。
 *
 * This is (usually) called during CommitTransaction(), so it's important for
 * it to have very low probability of failure.
 *
 * 这（通常）在 CommitTransaction() 期间调用，因此失败概率必须非常低。
 */
static void
asyncQueueAdvanceTail(void)
{
	QueuePosition min;
	int64		oldtailpage;
	int64		newtailpage;
	int64		boundary;

	/* Restrict task to one backend per cluster; see SimpleLruTruncate(). */
	/*
	 *
	 * 把任务限制为每个集群一个后端；见 SimpleLruTruncate()。
	 */
	LWLockAcquire(NotifyQueueTailLock, LW_EXCLUSIVE);

	/*
	 * Compute the new tail.  Pre-v13, it's essential that QUEUE_TAIL be exact
	 * (ie, exactly match at least one backend's queue position), so it must
	 * be updated atomically with the actual computation.  Since v13, we could
	 * get away with not doing it like that, but it seems prudent to keep it
	 * so.
	 *
	 * 计算新的尾。v13 之前，QUEUE_TAIL 必须精确（即至少与某个后端的队列位置完全一致），
	 * 因此必须与实际计算原子地更新。自 v13 起可以不这么做，但保持这样做似乎更稳妥。
	 *
	 * Also, because incoming backends will scan forward from QUEUE_TAIL, that
	 * must be advanced before we can truncate any data.  Thus, QUEUE_TAIL is
	 * the logical tail, while QUEUE_STOP_PAGE is the physical tail, or oldest
	 * un-truncated page.  When QUEUE_STOP_PAGE != QUEUE_POS_PAGE(QUEUE_TAIL),
	 * there are pages we can truncate but haven't yet finished doing so.
	 *
	 * 另外，新进入的后端会从 QUEUE_TAIL 向前扫描，因此必须先推进它，才能截断任何数据。
	 * 因此 QUEUE_TAIL 是逻辑尾，而 QUEUE_STOP_PAGE 是物理尾，即最老的尚未截断的页。
	 * 当 QUEUE_STOP_PAGE 不等于 QUEUE_POS_PAGE(QUEUE_TAIL) 时，有些页可以截断但还没做完。
	 *
	 * For concurrency's sake, we don't want to hold NotifyQueueLock while
	 * performing SimpleLruTruncate.  This is OK because no backend will try
	 * to access the pages we are in the midst of truncating.
	 *
	 * 为了并发，执行 SimpleLruTruncate 时不想持有 NotifyQueueLock。
	 * 这是可以的，因为没有后端会试图访问我们正在截断的页。
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	min = QUEUE_HEAD;
	for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
	{
		Assert(QUEUE_BACKEND_PID(i) != InvalidPid);
		min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
	}
	QUEUE_TAIL = min;
	oldtailpage = QUEUE_STOP_PAGE;
	LWLockRelease(NotifyQueueLock);

	/*
	 * We can truncate something if the global tail advanced across an SLRU
	 * segment boundary.
	 *
	 * 若全局尾越过了 SLRU 段边界，就可以截断一些内容。
	 *
	 * XXX it might be better to truncate only once every several segments, to
	 * reduce the number of directory scans.
	 *
	 * XXX：也许每隔若干段才截断一次更好，以减少目录扫描次数。
	 */
	newtailpage = QUEUE_POS_PAGE(min);
	boundary = newtailpage - (newtailpage % SLRU_PAGES_PER_SEGMENT);
	if (asyncQueuePagePrecedes(oldtailpage, boundary))
	{
		/*
		 * SimpleLruTruncate() will ask for SLRU bank locks but will also
		 * release the lock again.
		 *
		 * SimpleLruTruncate() 会请求 SLRU bank 锁，但也会再次释放该锁。
		 */
		SimpleLruTruncate(NotifyCtl, newtailpage);

		LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
		QUEUE_STOP_PAGE = newtailpage;
		LWLockRelease(NotifyQueueLock);
	}

	LWLockRelease(NotifyQueueTailLock);
}

/*
 * AsyncNotifyFreezeXids
 *
 * 函数 AsyncNotifyFreezeXids。
 *
 * Prepare the async notification queue for CLOG truncation by freezing
 * transaction IDs that are about to become inaccessible.
 *
 * 在截断 CLOG 之前，冻结即将无法访问的事务 ID，为异步通知队列做准备。
 *
 * This function is called by VACUUM before advancing datfrozenxid. It scans
 * the notification queue and replaces XIDs that would become inaccessible
 * after CLOG truncation with special markers:
 * - Committed transactions are set to FrozenTransactionId
 * - Aborted/crashed transactions are set to InvalidTransactionId
 *
 * VACUUM 在推进 datfrozenxid 之前调用此函数。它扫描通知队列，
 * 把截断 CLOG 后将无法访问的 XID 换成特殊标记：
 * 已提交事务设为 FrozenTransactionId，已中止或崩溃的事务设为 InvalidTransactionId。
 *
 * Only XIDs < newFrozenXid are processed, as those are the ones whose CLOG
 * pages will be truncated. If XID < newFrozenXid, it cannot still be running
 * (or it would have held back newFrozenXid through ProcArray).
 * Therefore, if TransactionIdDidCommit returns false, we know the transaction
 * either aborted explicitly or crashed, and we can safely mark it invalid.
 *
 * 只处理小于 newFrozenXid 的 XID，因为它们所在的 CLOG 页将被截断。
 * 若 XID 小于 newFrozenXid，它不可能仍在运行（否则会通过 ProcArray 挡住 newFrozenXid）。
 * 因此若 TransactionIdDidCommit 返回 false，可知该事务要么显式中止，要么崩溃，可以安全地标为无效。
 */
void
AsyncNotifyFreezeXids(TransactionId newFrozenXid)
{
	QueuePosition pos;
	QueuePosition head;
	int64		curpage = -1;
	int			slotno = -1;
	char	   *page_buffer = NULL;
	bool		page_dirty = false;

	/*
	 * Acquire locks in the correct order to avoid deadlocks. As per the
	 * locking protocol: NotifyQueueTailLock, then NotifyQueueLock, then SLRU
	 * bank locks.
	 *
	 * 按正确顺序获取锁以避免死锁。按加锁协议：先 NotifyQueueTailLock，再 NotifyQueueLock，然后是 SLRU bank 锁。
	 *
	 * We only need SHARED mode since we're just reading the head/tail
	 * positions, not modifying them.
	 *
	 * 只需要 SHARED 模式，因为只是读取头尾位置，并不修改它们。
	 */
	LWLockAcquire(NotifyQueueTailLock, LW_SHARED);
	LWLockAcquire(NotifyQueueLock, LW_SHARED);

	pos = QUEUE_TAIL;
	head = QUEUE_HEAD;

	/* Release NotifyQueueLock early, we only needed to read the positions */
	/*
	 *
	 * 尽早释放 NotifyQueueLock，我们只需要它来读取位置
	 */
	LWLockRelease(NotifyQueueLock);

	/*
	 * Scan the queue from tail to head, freezing XIDs as needed. We hold
	 * NotifyQueueTailLock throughout to ensure the tail doesn't move while
	 * we're working.
	 *
	 * 从尾到头扫描队列，按需要冻结 XID。全程持有 NotifyQueueTailLock，确保工作时尾不会移动。
	 */
	while (!QUEUE_POS_EQUAL(pos, head))
	{
		AsyncQueueEntry *qe;
		TransactionId xid;
		int64		pageno = QUEUE_POS_PAGE(pos);
		int			offset = QUEUE_POS_OFFSET(pos);

		/* If we need a different page, release old lock and get new one */
		/*
		 *
		 * 若需要另一页，释放旧锁并取得新锁
		 */
		if (pageno != curpage)
		{
			LWLock	   *lock;

			/* Release previous page if any */
			/*
			 *
			 * 若有上一页则释放
			 */
			if (slotno >= 0)
			{
				if (page_dirty)
				{
					NotifyCtl->shared->page_dirty[slotno] = true;
					page_dirty = false;
				}
				LWLockRelease(SimpleLruGetBankLock(NotifyCtl, curpage));
			}

			lock = SimpleLruGetBankLock(NotifyCtl, pageno);
			LWLockAcquire(lock, LW_EXCLUSIVE);
			slotno = SimpleLruReadPage(NotifyCtl, pageno, true,
									   InvalidTransactionId);
			page_buffer = NotifyCtl->shared->page_buffer[slotno];
			curpage = pageno;
		}

		qe = (AsyncQueueEntry *) (page_buffer + offset);
		xid = qe->xid;

		if (TransactionIdIsNormal(xid) &&
			TransactionIdPrecedes(xid, newFrozenXid))
		{
			if (TransactionIdDidCommit(xid))
			{
				qe->xid = FrozenTransactionId;
				page_dirty = true;
			}
			else
			{
				qe->xid = InvalidTransactionId;
				page_dirty = true;
			}
		}

		/* Advance to next entry */
		/*
		 *
		 * 前进到下一项
		 */
		asyncQueueAdvance(&pos, qe->length);
	}

	/* Release final page lock if we acquired one */
	/*
	 *
	 * 若取得过最后一页的锁则释放
	 */
	if (slotno >= 0)
	{
		if (page_dirty)
			NotifyCtl->shared->page_dirty[slotno] = true;
		LWLockRelease(SimpleLruGetBankLock(NotifyCtl, curpage));
	}

	LWLockRelease(NotifyQueueTailLock);
}

/*
 * ProcessIncomingNotify
 *
 * 函数 ProcessIncomingNotify。
 *
 *		Scan the queue for arriving notifications and report them to the front
 *		end.  The notifications might be from other sessions, or our own;
 *		there's no need to distinguish here.
 *
 * 扫描队列中到达的通知并报告给前端。通知可能来自其他会话，也可能来自自己；这里不必区分。
 *
 *		If "flush" is true, force any frontend messages out immediately.
 *
 * 若 flush 为真，立即把任何前端消息刷出去。
 *
 *		NOTE: since we are outside any transaction, we must create our own.
 *
 * 注意：因为我们在任何事务之外，必须自己创建一个事务。
 */
static void
ProcessIncomingNotify(bool flush)
{
	/* We *must* reset the flag */
	/*
	 *
	 * 必须重置该标志
	 */
	notifyInterruptPending = false;

	/* Do nothing else if we aren't actively listening */
	/*
	 *
	 * 若没有在主动监听，则不再做其他事
	 */
	if (listenChannels == NIL)
		return;

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify");

	set_ps_display("notify interrupt");

	/*
	 * We must run asyncQueueReadAllNotifications inside a transaction, else
	 * bad things happen if it gets an error.
	 *
	 * 必须在事务内运行 asyncQueueReadAllNotifications，否则它出错时会发生糟糕的事。
	 */
	StartTransactionCommand();

	asyncQueueReadAllNotifications();

	CommitTransactionCommand();

	/*
	 * If this isn't an end-of-command case, we must flush the notify messages
	 * to ensure frontend gets them promptly.
	 *
	 * 若这不是命令结束的情形，必须刷出通知消息，确保前端及时收到。
	 */
	if (flush)
		pq_flush();

	set_ps_display("idle");

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify: done");
}

/*
 * Send NOTIFY message to my front end.
 *
 * 把 NOTIFY 消息发给我的前端。
 */
void
NotifyMyFrontEnd(const char *channel, const char *payload, int32 srcPid)
{
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, PqMsg_NotificationResponse);
		pq_sendint32(&buf, srcPid);
		pq_sendstring(&buf, channel);
		pq_sendstring(&buf, payload);
		pq_endmessage(&buf);

		/*
		 * NOTE: we do not do pq_flush() here.  Some level of caller will
		 * handle it later, allowing this message to be combined into a packet
		 * with other ones.
		 *
		 * 注意：这里不做 pq_flush()。某一层调用方稍后会处理，使这条消息可以与其他消息打进同一个包。
		 */
	}
	else
		elog(INFO, "NOTIFY for \"%s\" payload \"%s\"", channel, payload);
}

/* Does pendingNotifies include a match for the given event? */
/*
 *
 * pendingNotifies 中是否有与给定事件匹配的项？
 */
static bool
AsyncExistsPendingNotify(Notification *n)
{
	if (pendingNotifies == NULL)
		return false;

	if (pendingNotifies->hashtab != NULL)
	{
		/* Use the hash table to probe for a match */
		/*
		 *
		 * 用哈希表探测是否匹配
		 */
		if (hash_search(pendingNotifies->hashtab,
						&n,
						HASH_FIND,
						NULL))
			return true;
	}
	else
	{
		/* Must scan the event list */
		/*
		 *
		 * 必须扫描事件列表
		 */
		ListCell   *l;

		foreach(l, pendingNotifies->events)
		{
			Notification *oldn = (Notification *) lfirst(l);

			if (n->channel_len == oldn->channel_len &&
				n->payload_len == oldn->payload_len &&
				memcmp(n->data, oldn->data,
					   n->channel_len + n->payload_len + 2) == 0)
				return true;
		}
	}

	return false;
}

/*
 * Add a notification event to a pre-existing pendingNotifies list.
 *
 * 把一个通知事件加入已经存在的 pendingNotifies 列表。
 *
 * Because pendingNotifies->events is already nonempty, this works
 * correctly no matter what CurrentMemoryContext is.
 *
 * 因为 pendingNotifies->events 已经非空，无论 CurrentMemoryContext 是什么，这样做都是正确的。
 */
static void
AddEventToPendingNotifies(Notification *n)
{
	Assert(pendingNotifies->events != NIL);

	/* Create the hash table if it's time to */
	/*
	 *
	 * 若到了该建哈希表的时候，就创建它
	 */
	if (list_length(pendingNotifies->events) >= MIN_HASHABLE_NOTIFIES &&
		pendingNotifies->hashtab == NULL)
	{
		HASHCTL		hash_ctl;
		ListCell   *l;

		/* Create the hash table */
		/*
		 *
		 * 创建哈希表
		 */
		hash_ctl.keysize = sizeof(Notification *);
		hash_ctl.entrysize = sizeof(struct NotificationHash);
		hash_ctl.hash = notification_hash;
		hash_ctl.match = notification_match;
		hash_ctl.hcxt = CurTransactionContext;
		pendingNotifies->hashtab =
			hash_create("Pending Notifies",
						256L,
						&hash_ctl,
						HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

		/* Insert all the already-existing events */
		/*
		 *
		 * 插入所有已经存在的事件
		 */
		foreach(l, pendingNotifies->events)
		{
			Notification *oldn = (Notification *) lfirst(l);
			bool		found;

			(void) hash_search(pendingNotifies->hashtab,
							   &oldn,
							   HASH_ENTER,
							   &found);
			Assert(!found);
		}
	}

	/* Add new event to the list, in order */
	/*
	 *
	 * 按顺序把新事件加入列表
	 */
	pendingNotifies->events = lappend(pendingNotifies->events, n);

	/* Add event to the hash table if needed */
	/*
	 *
	 * 若需要，把事件加入哈希表
	 */
	if (pendingNotifies->hashtab != NULL)
	{
		bool		found;

		(void) hash_search(pendingNotifies->hashtab,
						   &n,
						   HASH_ENTER,
						   &found);
		Assert(!found);
	}
}

/*
 * notification_hash: hash function for notification hash table
 *
 * notification_hash：通知哈希表的哈希函数
 *
 * The hash "keys" are pointers to Notification structs.
 *
 * 哈希键是指向 Notification 结构的指针。
 */
static uint32
notification_hash(const void *key, Size keysize)
{
	const Notification *k = *(const Notification *const *) key;

	Assert(keysize == sizeof(Notification *));
	/* We don't bother to include the payload's trailing null in the hash */
	/*
	 *
	 * 哈希时不把 payload 结尾的 null 算进去
	 */
	return DatumGetUInt32(hash_any((const unsigned char *) k->data,
								   k->channel_len + k->payload_len + 1));
}

/*
 * notification_match: match function to use with notification_hash
 *
 * notification_match：与 notification_hash 配合使用的匹配函数
 */
static int
notification_match(const void *key1, const void *key2, Size keysize)
{
	const Notification *k1 = *(const Notification *const *) key1;
	const Notification *k2 = *(const Notification *const *) key2;

	Assert(keysize == sizeof(Notification *));
	if (k1->channel_len == k2->channel_len &&
		k1->payload_len == k2->payload_len &&
		memcmp(k1->data, k2->data,
			   k1->channel_len + k1->payload_len + 2) == 0)
		return 0;				/* equal */
		/*
		 *
		 * 相等
		 */
	return 1;					/* not equal */
	/*
	 *
	 * 不相等
	 */
}

/* Clear the pendingActions and pendingNotifies lists. */
/*
 *
 * 清空 pendingActions 和 pendingNotifies 列表。
 */
static void
ClearPendingActionsAndNotifies(void)
{
	/*
	 * Everything's allocated in either TopTransactionContext or the context
	 * for the subtransaction to which it corresponds.  So, there's nothing to
	 * do here except reset the pointers; the space will be reclaimed when the
	 * contexts are deleted.
	 *
	 * 所有东西都分配在 TopTransactionContext 或对应子事务的上下文中。
	 * 因此这里除了重置指针无事可做；上下文删除时空间会被回收。
	 */
	pendingActions = NULL;
	pendingNotifies = NULL;
}

/*
 * GUC check_hook for notify_buffers
 *
 * notify_buffers 的 GUC check_hook
 */
bool
check_notify_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("notify_buffers", newval);
}
