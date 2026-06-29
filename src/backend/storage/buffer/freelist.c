/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *	  用于管理缓冲池替换策略的例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * 部分版权 (c) 1996-2025, PostgreSQL 全球开发组
 * 部分版权 (c) 1994, 加州大学董事会
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 * 标识
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 *
 * 核心流程解释：
 * 本文件管理 PostgreSQL 共享缓冲区（Shared Buffer Pool）的页面替换与分配策略（Freelist / Clock Sweep 替换算法）。
 * 核心流程如下：
 *
 * 1. 缓冲区分配核心入口 (StrategyGetBuffer):
 *    - 当缓冲区管理器（Buffer Manager）需要一个可用缓冲区时，它会调用 `StrategyGetBuffer()`。
 *    - 如果传入了特定的缓冲区访问策略对象（BufferAccessStrategy，如批量读、批量写或 Vacuum 环形缓冲区），
 *      它会首选调用 `GetBufferFromRing()` 尝试从其私有环形缓冲区中回收复用一个空闲页面，以避免对全局缓冲区造成“缓存污染”。
 *    - 接下来，如果当前共享内存空闲链表（Freelist）中有空闲缓冲区（即 `StrategyControl->firstFreeBuffer >= 0`），
 *      则通过获取自旋锁从链表头部取出一个缓冲区返回。
 *    - 如果上述方式都未成功，则执行经典的“时钟扫尾（Clock Sweep）”算法页面置换。
 *
 * 2. 时钟置换算法 (Clock Sweep / ClockSweepTick):
 *    - 调用 `ClockSweepTick()` 原子上调时钟扫描指针 `nextVictimBuffer`。通过模 `NBuffers` 计算当前正在考察的页面。
 *    - 对目标页面加锁，并检查其引用计数（Refcount）与使用计数（Usagecount）。
 *    - 如果页面已被 PIN 住（引用计数不为0），我们不能使用它，并且由于页面正在被使用，我们保持其使用计数（Usagecount）不变。
 *    - 如果页面未被 PIN 且使用计数大于0，我们通过减少该页面的使用计数（Usagecount）来给它一个“老化”的机会，然后继续扫描下一个页面。
 *    - 如果页面未被 PIN 且使用计数为0，我们找到了一个理想的可复用页面（Victim），对其加锁并返回。
 *
 * 3. 释放/回收流程 (StrategyFreeBuffer):
 *    - 当一个缓冲区的内容被无效化或彻底释放时（例如表被删除或 VACUUM 清空），调用 `StrategyFreeBuffer()` 将其放回 `StrategyControl` 共享空闲链表的头部。
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))


/*
 * The shared freelist control information.
 * 共享空闲链表控制信息。
 */
typedef struct
{
	/* Spinlock: protects the values below
	 * 自旋锁：保护下面的值 */
	slock_t		buffer_strategy_lock;

	/*
	 * Clock sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 * 时钟扫描指针：下一个要考察的缓冲区索引。请注意，
	 * 这不是一个具体的缓冲区 —— 我们只是一引值递增该值。因此，
	 * 要获得一个实际的缓冲区，需要将其模 NBuffers 使用。
	 */
	pg_atomic_uint32 nextVictimBuffer;

	int			firstFreeBuffer;	/* Head of list of unused buffers
									   未使用的缓冲区链表头部 */
	int			lastFreeBuffer; /* Tail of list of unused buffers
								   未使用的缓冲区链表尾部 */

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 * 注意：当 firstFreeBuffer 为 -1 时（即链表为空时），lastFreeBuffer 未定义
	 */

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 * 统计信息。这些计数器应该足够宽，以至于它们不能在单个 bgwriter 周期内溢出。
	 */
	uint32		completePasses; /* Complete cycles of the clock sweep
								   时钟扫描的完整循环次数 */
	pg_atomic_uint32 numBufferAllocs;	/* Buffers allocated since last reset
										   自上次重置以来分配的缓冲区数量 */

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 * 在有活动时要通知的后台写入进程号，如果没有则为 -1。见 StrategyNotifyBgWriter。
	 */
	int			bgwprocno;
} BufferStrategyControl;

/* Pointers to shared state
 * 指向共享状态的指针 */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 * 用于管理可复用共享缓冲区环的私有（非共享）状态。
 * 目前这是唯一的一种 BufferAccessStrategy 对象，但也许有一天我们会有更多种类。
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type
	 * 整体策略类型 */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array
	 * buffers[] 数组中的元素数量 */
	int			nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 * 环中“当前”槽的索引，即最近被 GetBufferFromRing 返回的槽。
	 */
	int			current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 * 缓冲区编号数组。InvalidBuffer（即零）表示我们尚未为此环槽选择缓冲区。
	 * 为了分配简单，它与该结构体的固定字段一起被 palloc 分配。
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


/* Prototypes for internal functions
 * 内部函数的函数声明 */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 * 时钟扫描推进 —— StrategyGetBuffer() 的辅助例程。
 *
 * 将时钟指针从当前位置向前移动一个缓冲区，并返回当前指针指向的缓冲区 ID。
 *
 * 函数作用：
 * 原子递增 nextVictimBuffer 并处理可能发生的值越界后的模 NBuffers 回绕。
 * 当回绕发生且模为 0 时，加锁递增完满圈数 completePasses 计数。
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 * 原子上将指针向前移动一个缓冲区 —— 如果有几个进程都在做这件事，这可能会导致返回的缓冲区顺序与表面顺序略有出入。
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors
		 * 始终对我们在 BufferDescriptors 中查找的内容进行回绕 */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 * 如果我们是刚引起回绕的那个进程，强制在持有自旋锁的同时递增 completePasses。
		 * 我们需要这个自旋锁，以便 StrategySyncStart() 可以返回一个包含 nextVictimBuffer 和 completePasses 的一致值。
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 * 在增加 completePasses 的同时获取自旋锁。这允许其他读取器以一致的方式读取 nextVictimBuffer
				 * 和 completePasses，这是 StrategySyncStart() 所必需的。
				 * 理论上延迟增加可能会导致 nextVictimBuffers 溢出，但这是极不可能的，并且不会产生特别有害的后果。
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * have_free_buffer -- a lockless check to see if there is a free buffer in
 *					   buffer pool.
 *
 * If the result is true that will become stale once free buffers are moved out
 * by other operations, so the caller who strictly want to use a free buffer
 * should not call this.
 * have_free_buffer —— 无锁检查缓冲池中是否有空闲缓冲区。
 *
 * 如果结果为真，那么一旦其他操作移出空闲缓冲区，该结果就会过时，因此严格希望使用空闲缓冲区的调用者不应调用此函数。
 *
 * 函数作用：
 * 进行一次无锁的非阻塞判定，仅简单检查 firstFreeBuffer 是否大于等于0。
 */
bool
have_free_buffer(void)
{
	if (StrategyControl->firstFreeBuffer >= 0)
		return true;
	else
		return false;
}

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.
 * 获取候选替换的缓冲区。
 *
 * 由 bufmgr 调用，以获取用于 BufferAlloc() 的下一个候选缓冲区。
 * BufferAlloc() 唯一的硬性要求是所选的缓冲区目前不能被任何人 pin 住。
 *
 * strategy 是一个 BufferAccessStrategy 对象，对于默认策略则为 NULL。
 *
 * 为了确保在我们这样做之前没有其他人可以 pin 住该缓冲区，我们必须在返回缓冲区时仍持有缓冲区头部的自旋锁。
 *
 * 函数作用：
 * 核心分配页面函数。
 * 1. 若有特定 strategy，先尝试 GetBufferFromRing()；
 * 2. 若空闲链表 freelist 中有富余，获取 buffer_strategy_lock 后从头部弹出一个空闲页；
 * 3. 否则，通过 Clock Sweep 置换查找一个未 pinned 且 usagecount 降至 0 的页面。
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 * 如果给定了策略对象，请查看它是否可以选择缓冲区。我们假设策略对象不需要 buffer_strategy_lock。
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 * 如果被要求，我们需要唤醒 bgwriter。由于我们不想为此依赖自旋锁，我们强制从共享内存读取一次，
	 * 然后根据该值设置 latch。我们需要这样做是因为，否则 bgwprocno 可能会在我们检查时/之后被重置，
	 * 因为编译器可能会重新从内存中读取。
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 * 如果 bgwriter 在不合时宜的时刻死亡，这可能会设置错误进程的 latch。但是由于 PGPROC->procLatch
	 * 永远不会被释放，这样做的最坏后果仅仅是我们设置了某个任意进程的 latch。
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch
		 * 在设置 latch 之前，先重置 bgwprocno */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 * 这里没有获取 ProcArrayLock，这稍微有点别扭。实际上没关系，
		 * 因为 procLatch 永远不会被释放，所以我们只是有可能设置了错误进程的（或没有进程的）latch。
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 * 我们对缓冲区分配请求进行计数，以便 bgwriter 可以估计缓冲区的消耗速率。
	 * 请注意，由策略对象回收的缓冲区故意不在此处计数。
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	/*
	 * First check, without acquiring the lock, whether there's buffers in the
	 * freelist. Since we otherwise don't require the spinlock in every
	 * StrategyGetBuffer() invocation, it'd be sad to acquire it here -
	 * uselessly in most cases. That obviously leaves a race where a buffer is
	 * put on the freelist but we don't see the store yet - but that's pretty
	 * harmless, it'll just get used during the next buffer acquisition.
	 * 首先在不获取锁的情况下检查空闲链表中有无缓冲区。因为我们平时在每次调用 StrategyGetBuffer()
	 * 时并不需要获取自旋锁，如果在这里（大多数情况下是徒劳地）获取自旋锁就太令人遗憾了。
	 * 这显然会留下竞争，即一个缓冲区被放到了空闲链表上，但我们还没有看到它的写入 —— 但这非常无害，
	 * 它将在下一次缓冲区获取时被使用。
	 *
	 * If there's buffers on the freelist, acquire the spinlock to pop one
	 * buffer of the freelist. Then check whether that buffer is usable and
	 * repeat if not.
	 * 如果空闲链表上有缓冲区，则获取自旋锁以从空闲链表中弹出一个缓冲区。然后检查该缓冲区是否可用，如果不可用则重复。
	 *
	 * Note that the freeNext fields are considered to be protected by the
	 * buffer_strategy_lock not the individual buffer spinlocks, so it's OK to
	 * manipulate them without holding the spinlock.
	 * 请注意，freeNext 字段被认为是由 buffer_strategy_lock 保护，而不是由单个缓冲区自旋锁保护，
	 * 因此在不持有自旋锁的情况下操作它们是可以的。
	 */
	if (StrategyControl->firstFreeBuffer >= 0)
	{
		while (true)
		{
			/* Acquire the spinlock to remove element from the freelist
			 * 获取自旋锁以从空闲链表中移除元素 */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

			if (StrategyControl->firstFreeBuffer < 0)
			{
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
				break;
			}

			buf = GetBufferDescriptor(StrategyControl->firstFreeBuffer);
			Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

			/* Unconditionally remove buffer from freelist
			 * 无条件从空闲链表移除缓冲区 */
			StrategyControl->firstFreeBuffer = buf->freeNext;
			buf->freeNext = FREENEXT_NOT_IN_LIST;

			/*
			 * Release the lock so someone else can access the freelist while
			 * we check out this buffer.
			 * 释放锁，以便在我们检查该缓冲区时，其他人可以访问空闲链表。
			 */
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/*
			 * If the buffer is pinned or has a nonzero usage_count, we cannot
			 * use it; discard it and retry.  (This can only happen if VACUUM
			 * put a valid buffer in the freelist and then someone else used
			 * it before we got to it.  It's probably impossible altogether as
			 * of 8.3, but we'd better check anyway.)
			 * 如果缓冲区被 pinned 或具有非零的 usage_count，我们不能使用它；放弃它并重试。
			 * （这只有在 VACUUM 将一个有效缓冲区放入空闲链表，然后其他人在我们获取它之前使用了它的情况下才会发生。
			 * 自 8.3 版本起这可能完全不可能发生，但我们最好还是检查一下。）
			 */
			local_buf_state = LockBufHdr(buf);
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
				&& BUF_STATE_GET_USAGECOUNT(local_buf_state) == 0)
			{
				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
			UnlockBufHdr(buf, local_buf_state);
		}
	}

	/* Nothing on the freelist, so run the "clock sweep" algorithm
	 * 空闲链表上没有任何东西，因此运行 “时钟扫描” (clock sweep) 算法 */
	trycounter = NBuffers;
	for (;;)
	{
		buf = GetBufferDescriptor(ClockSweepTick());

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; decrement the usage_count (unless pinned) and keep scanning.
		 * 如果缓冲区被 pin 住或具有非零的使用计数，我们不能使用它；
		 * 递减使用计数（除非被 pinned）并继续扫描。
		 */
		local_buf_state = LockBufHdr(buf);

		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{
			if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			{
				local_buf_state -= BUF_USAGECOUNT_ONE;

				trycounter = NBuffers;
			}
			else
			{
				/* Found a usable buffer
				 * 找到了一个可用的缓冲区 */
				if (strategy != NULL)
					AddBufferToRing(strategy, buf);
				*buf_state = local_buf_state;
				return buf;
			}
		}
		else if (--trycounter == 0)
		{
			/*
			 * We've scanned all the buffers without making any state changes,
			 * so all the buffers are pinned (or were when we looked at them).
			 * We could hope that someone will free one eventually, but it's
			 * probably better to fail than to risk getting stuck in an
			 * infinite loop.
			 * 我们已经在没有做任何状态改变的情况下扫描了所有的缓冲区，
			 * 因此所有的缓冲区都被 pin 住了（或者在我们看它们的时候是这样）。
			 * 我们可以指望最终有人会释放一个，但是失败可能比冒着陷入无限循环的风险要好。
			 */
			UnlockBufHdr(buf, local_buf_state);
			elog(ERROR, "no unpinned buffers available");
		}
		UnlockBufHdr(buf, local_buf_state);
	}
}

/*
 * StrategyFreeBuffer: put a buffer on the freelist
 * 将一个缓冲区放入空闲链表 (freelist)。
 *
 * 函数作用：
 * 释放一个页面的物理占用资源并回收到空闲链表头部。
 */
void
StrategyFreeBuffer(BufferDesc *buf)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 * 我们有可能会被要求将已经存在于空闲链表中的东西放入空闲链表；如果是这样，不要弄乱链表。
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
	}

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * StrategySyncStart -- tell BgBufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BgBufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 * 告知后台缓存同步器从哪里开始同步。
 *
 * 结果是最好首先同步的缓冲区索引。
 * BgBufferSync() 将从那里循环围绕缓冲区数组进行。
 *
 * 此外，如果传递了非空指针，我们还会返回完成的完整扫描圈数（这实际上是 nextVictimBuffer 的高阶位）
 * 以及最近分配的缓冲区数量。分配数量在读取后会被重置。
 *
 * 函数作用：
 * 返回当前的 nextVictimBuffer 指针所映射的 buffer 索引，
 * 使得后台 bgwriter 的脏页刷盘（BgBufferSync）可以紧跟着当前的时钟位置进行。
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 * 另外，加上在 completePasses 可以被递增之前发生的回绕次数。参见 ClockSweepTick()。
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 * 设置或清除分配通知的 latch。
 *
 * 如果 bgwprocno 不是 -1，下一次调用 StrategyGetBuffer 将设置该 latch。
 * 传入 -1 可在挂起的通知发生之前清除它。此功能由 bgwriter 进程使用，以从冬眠中唤醒自己，
 * 绝不是供其他任何人使用的。
 *
 * 函数作用：
 * 注册 bgwriter 进程的 procno。在下一次执行 `StrategyGetBuffer()` 时将通过此 latch 唤醒它，
 * 以便后台按需将脏数据写入磁盘。
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 * 我们获取 buffer_strategy_lock 仅仅是为了确保写入对 StrategyGetBuffer 看起来是原子的。
	 * bgwriter 应该很少调用此函数，因此为了安全起见没有任何性能损失。
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 * 估计空闲链表相关结构使用的共享内存大小。
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 * 注意：由于某些历史原因，缓冲区查找哈希表的大小也在此确定。
 *
 * 函数作用：
 * 计算在共享内存中用于存放 BufferStrategyControl 策略控制结构以及共享缓冲检索哈希表的大小总量。
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize
	 * 查找哈希表的大小 ... 参见 StrategyInitialize 中的注释 */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block
	 * 共享替换策略控制块的大小 */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *		初始化缓冲池页面替换策略。
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 * 假设：所有的缓冲区都已经构建成了一个链表。
 *		仅由 postmaster 并在初始化期间调用。
 *
 * 函数作用：
 * 全局初始化替换策略结构体。
 * 1. 调用 InitBufTable() 初始化共享哈希表大小；
 * 2. 在共享内存中获取/创建 "Buffer Strategy Status" 结构，并将全部的共享缓冲区连接到 firstFreeBuffer-lastFreeBuffer 空闲链表中。
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 * 初始化共享缓冲区查找哈希表。
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 * 由于我们不能容忍查找表条目用尽，我们必须确保在此处指定足够大的表大小。
	 * 稳态下的最大使用量当然是 NBuffers 个条目，但是 BufferAlloc() 在删除旧条目之前会尝试插入新条目。
	 * 原则上这可能会在每个分区中并发发生，因此我们可能需要多达 NBuffers + NUM_BUFFER_PARTITIONS 个条目。
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 * 获取或创建共享策略控制块
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 * 仅执行一次，通常在 postmaster 中进行
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by BufferManagerShmemInit().
		 * 为我们的策略抓取整个空闲缓冲区的链表。我们假设它之前已由 BufferManagerShmemInit() 设置好。
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* Initialize the clock sweep pointer
		 * 初始化时钟扫描指针 */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

		/* Clear statistics
		 * 清除统计数据 */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* No pending notification
		 * 没有挂起的通知 */
		StrategyControl->bgwprocno = -1;
	}
	else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 *				后台进程私有的缓冲区环管理
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 * 创建一个 BufferAccessStrategy 策略对象。
 *
 * 该对象是在当前内存上下文中分配的。
 *
 * 函数作用：
 * 根据传入的策略类型（BAS_NORMAL、BAS_BULKREAD、BAS_BULKWRITE、BAS_VACUUM），
 * 计算和推导所需的页面环大小（ring_size_kb），进而通过 `GetAccessStrategyWithSize()`
 * 在后端进程本地 MemoryContext 分配该环结构。如果是 BAS_NORMAL 则直接返回 NULL。
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 * 选择要使用的环大小。具体理由请参见 buffer/README。
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 * 注意：如果您更改了 BAS_BULKREAD 的环大小，另请参阅 access/heap/syncscan.c
	 * 中的 SYNC_SCAN_REPORT_INTERVAL。
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object
			 * 如果有人要 NORMAL，只需给他们一个 “默认” 的对象即可 */
			return NULL;

		case BAS_BULKREAD:
			{
				int			ring_max_kb;

				/*
				 * The ring always needs to be large enough to allow some
				 * separation in time between providing a buffer to the user
				 * of the strategy and that buffer being reused. Otherwise the
				 * user's pin will prevent reuse of the buffer, even without
				 * concurrent activity.
				 * 该环总是需要足够大，以允许在向策略用户提供缓冲区与该缓冲区被重新使用之间在时间上有一些间隔。
				 * 否则，即使没有并发活动，用户的 pin 也将阻止缓冲区的重新使用。
				 *
				 * We also need to ensure the ring always is large enough for
				 * SYNC_SCAN_REPORT_INTERVAL, as noted above.
				 * 如上所述，我们还需要确保环始终足够大以容纳 SYNC_SCAN_REPORT_INTERVAL。
				 *
				 * Thus we start out a minimal size and increase the size
				 * further if appropriate.
				 * 因此，我们先从一个最小尺寸开始，并在合适时进一步增加尺寸。
				 */
				ring_size_kb = 256;

				/*
				 * There's no point in a larger ring if we won't be allowed to
				 * pin sufficiently many buffers.  But we never limit to less
				 * than the minimal size above.
				 * 如果我们不被允许 pin 足够多的缓冲区，那么使用更大的环就没有任何意义。
				 * 但是我们永远不会限制到低于上述的最小尺寸。
				 */
				ring_max_kb = GetPinLimit() * (BLCKSZ / 1024);
				ring_max_kb = Max(ring_size_kb, ring_max_kb);

				/*
				 * We would like the ring to additionally have space for the
				 * configured degree of IO concurrency. While being read in,
				 * buffers can obviously not yet be reused.
				 * 我们希望该环还能容纳配置的并发 IO 程度的空间。在读入时，缓冲区显然还不能被重复使用。
				 *
				 * Each IO can be up to io_combine_limit blocks large, and we
				 * want to start up to effective_io_concurrency IOs.
				 * 每一个 IO 可以高达 io_combine_limit 块大，且我们希望最多启动 effective_io_concurrency 个 IO。
				 *
				 * Note that effective_io_concurrency may be 0, which disables
				 * AIO.
				 * 请注意，effective_io_concurrency 可能是 0，这会禁用 AIO。
				 */
				ring_size_kb += (BLCKSZ / 1024) *
					io_combine_limit * effective_io_concurrency;

				if (ring_size_kb > ring_max_kb)
					ring_size_kb = ring_max_kb;
				break;
			}
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 2048;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *		根据传入的大小分配创建一个 BufferAccessStrategy 页面环。
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 * 如果给定的环大小为 0，则不会创建 BufferAccessStrategy，并且该函数将返回 NULL。
 * ring_size_kb 不能为负数。
 *
 * 函数作用：
 * 根据需要的页面数，使用 palloc0 动态分配包含 ring_buffers 大小的
 * BufferAccessStrategyData 后端私有内存结构，并对其赋初始字段值。上限被截断至 shared_buffers 的 1/8 大小。
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *		获取环中缓冲区数量的访问器。
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 * 对 NULL 输入返回 0，以匹配 GetAccessStrategyWithSize() 在大小为 0 时返回 NULL 的行为。
 *
 * 函数作用：
 * 安全返回 strategy->nbuffers 的访问器函数。
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * GetAccessStrategyPinLimit -- get cap of number of buffers that should be pinned
 *		获取应被 pin 住的缓冲区数量的上限。
 *
 * When pinning extra buffers to look ahead, users of a ring-based strategy are
 * in danger of pinning too much of the ring at once while performing look-ahead.
 * For some strategies, that means "escaping" from the ring, and in others it
 * means forcing dirty data to disk very frequently with associated WAL
 * flushing.  Since external code has no insight into any of that, allow
 * individual strategy types to expose a clamp that should be applied when
 * deciding on a maximum number of buffers to pin at once.
 * 当 pin 住额外的缓冲区以进行向前看时，基于环策略的用户在执行向前看时面临一次性 pin 住过多环的危险。
 * 对于某些策略，这意味着“逃离”环，而在其他策略中，这意味着非常频繁地强制脏数据写入磁盘以及相关的 WAL 刷新。
 * 由于外部代码对这些完全不知情，允许单个策略类型暴露一个在决定一次性 pin 住的最大缓冲区数量时应应用的钳夹上限。
 *
 * Callers should combine this number with other relevant limits and take the
 * minimum.
 * 调用者应将此数字与其他相关的限制结合起来并取最小值。
 *
 * 函数作用：
 * 根据不同策略的并发 IO 老化考虑，告知调用者当前环最大一次性允许 PIN 多少个页面，
 * 避免一次性锁死过多页面导致策略回退为全局大分配（即“逃离”环）或强推脏数据导致 WAL 瓶颈。
 */
int
GetAccessStrategyPinLimit(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return NBuffers;

	switch (strategy->btype)
	{
		case BAS_BULKREAD:

			/*
			 * Since BAS_BULKREAD uses StrategyRejectBuffer(), dirty buffers
			 * shouldn't be a problem and the caller is free to pin up to the
			 * entire ring at once.
			 * 既然 BAS_BULKREAD 使用了 StrategyRejectBuffer()，脏页就不成问题，
			 * 调用者可以自由地一次性 pin 住整个环。
			 */
			return strategy->nbuffers;

		default:

			/*
			 * Tell caller not to pin more than half the buffers in the ring.
			 * This is a trade-off between look ahead distance and deferring
			 * writeback and associated WAL traffic.
			 * 告诉调用者一次性 pin 住不要超过环中一半的页面。
			 * 这是在向前看距离与延迟写回和相关的 WAL 流量之间的一种权衡。
			 */
			return strategy->nbuffers / 2;
	}
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 * 释放 BufferAccessStrategy 策略对象。
 *
 * 目前只需进行简单的 pfree 即可，但我们不希望调用者对 BufferAccessStrategy 的表现形式做过多假设。
 *
 * 函数作用：
 * 释放本地 palloc 分配的环对象。
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The bufhdr spin lock is held on the returned buffer.
 * 从环中获取一个缓冲区，如果环为空/不可用，则返回 NULL。
 *
 * 返回的缓冲区持有 bufhdr 自旋锁。
 *
 * 函数作用：
 * 遍历并将 strategy->current 槽向后推进一位。
 * 检查当前槽位保存的缓冲区状态。如果其使用计数（Usagecount）小于等于1，且引用计数（Refcount）为0，
 * 说明此环页已经被彻底老化且未被其它后台引用，因此我们可以成功地从环中回收利用它，并返回该页面（持有头部自旋锁）。
 * 否则返回 NULL，指示上层用普通分配从全局拿新页来替换该槽。
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */


	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 * 如果槽尚未被填满，告诉调用者使用正常的分配策略分配一个新缓冲区。
	 * 然后他将通过以新缓冲区调用 AddBufferToRing 来填满该槽。
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	/*
	 * If the buffer is pinned we cannot use it under any circumstances.
	 *
	 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
	 * since our own previous usage of the ring element would have left it
	 * there, but it might've been decremented by clock sweep since then). A
	 * higher usage_count indicates someone else has touched the buffer, so we
	 * shouldn't re-use it.
	 * 如果缓冲区被 pin 住，我们在任何情况下都不能使用它。
	 *
	 * 如果使用计数是 0 或 1，则该缓冲区是公平竞争的（我们期望为 1，
	 * 因为我们之前自己对该环元素的使用会将其留在那里，但自那时起它可能已被时钟扫描递减）。
	 * 更高的使用计数表示有其他人接触过该缓冲区，因此我们不应该重复使用它。
	 */
	buf = GetBufferDescriptor(bufnum - 1);
	local_buf_state = LockBufHdr(buf);
	if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
		&& BUF_STATE_GET_USAGECOUNT(local_buf_state) <= 1)
	{
		*buf_state = local_buf_state;
		return buf;
	}
	UnlockBufHdr(buf, local_buf_state);

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 * 告诉调用者使用正常的分配策略分配一个新缓冲区。然后他将通过 AddBufferToRing 替换此环元素。
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 * 将一个页面绑定/放入策略环当前槽位中。
 *
 * 调用者必须持有该缓冲区上的缓冲区头部自旋锁。因为这是在持有自旋锁的情况下调用的，所以最好开销极小。
 *
 * 函数作用：
 * 更新 strategy->buffers[current] 对应共享页的槽值。
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 * 实用程序函数，返回给定 BufferAccessStrategy 策略环的 IOContext。
 *
 * 函数作用：
 * 根据传入策略对象将其映射映射 to 具体的 I/O 运行环境（IOCONTEXT_BULKREAD、IOCONTEXT_BULKWRITE、IOCONTEXT_VACUUM 等），
 * 使得统计系统能够正确归类和统计不同策略下的物理 I/O 开销数据。
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * Currently, GetAccessStrategy() returns NULL for
			 * BufferAccessStrategyType BAS_NORMAL, so this case is
			 * unreachable.
			 * 目前，GetAccessStrategy() 对 BufferAccessStrategyType BAS_NORMAL 返回 NULL，
			 * 因此此情况不可达。
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 * 考虑拒绝一个脏页面。
 *
 * 当使用非默认策略时，如果在 StrategyGetBuffer 选择的缓冲区需要被写出，
 * 且这样做还需要刷新 WAL 时，缓冲区管理器将调用此函数。这给了我们选择不同 victim 的机会。
 *
 * 如果缓冲区管理器应该请求一个新的 victim，则返回真，如果此缓冲区应该被写入并重复使用，则返回假。
 *
 * 函数作用：
 * 在 BULKREAD 模式下，如果分配选定的页面不仅是脏的且需要连带写 WAL 刷盘，
 * 此时如果该页确实来源于该进程环（from_ring为真），我们将此页面从当前环中腾出，返回 true 指示拒绝此页面，
 * 重新寻找其它页面做 Victim，以避免阻塞整个批量读操作的吞吐。
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy
	 * 不要干扰普通缓冲区替换策略的行为 */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 * 将脏缓冲区从环中移除；如果环的所有成员都是脏的，这是为了防止无限循环所必需的。
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}
