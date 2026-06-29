/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *	  缓冲区管理器初始化例程
 *
 * 实现核心流程概述：
 * 本文件负责 PostgreSQL 共享缓冲区环境（Buffer Pool）的全局初始化。
 * 它是数据库启动过程中建立内存管理基础的关键步骤。
 *
 * 核心流程如下：
 * 1. 内存估算：由 BufferManagerShmemSize() 计算所需的总共享内存大小，
 *    涵盖了缓冲区主体（BufferBlocks）、描述符（BufferDescriptors）及辅助同步结构。
 * 2. 共享内存分配：通过 ShmemInitStruct() 在共享内存段中预留空间，并进行对齐（Alignment）以优化性能。
 * 3. 结构初始化：
 *    - 初始化所有缓冲区描述符（BufferDesc），清除标签并重置原子状态。
 *    - 建立空闲列表（Freelist），初次启动时将所有缓冲区串联为未使用的线性链表。
 *    - 初始化轻量级锁（LWLock）和条件变量（Condition Variable），用于处理并发 I/O。
 * 4. 辅助结构集成：初始化策略管理器（StrategyInitialize）和检查点排序数组（CkptBufferIds）。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/aio.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;


/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 * 数据结构：
 * 		缓冲区存在于空闲列表（freelist）和查找数据结构中。
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 * 缓冲区查找：
 * 		有两个要点。首先，缓冲区必须在 I/O 开始之前就可供查找。
 * 		否则，第二个尝试读取该缓冲区的进程将分配其自己的副本，从而导致缓冲区池变得不一致。
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 * 缓冲区替换：
 * 		参见 freelist.c。当数据管理器正在使用或处于 I/O 过程中时，不能替换缓冲区。
 *
 *
 * Synchronization/Locking:
 * 同步/锁定：
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 * IO_IN_PROGRESS -- 这是缓冲区描述符中的一个标志。
 * 		必须在 I/O 启动时设置，并在 I/O 结束时清除。
 * 		它的存在是为了确保一个进程不会在另一个进程将其调入时开始使用该缓冲区。参见 WaitIO 及其相关例程。
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if an individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 * refcount -- 统计对一个缓冲区持有固定（pin）操作的进程数量。
 * 		在 I/O 期间以及 BufferAlloc() 之后立即将缓冲区固定。
 * 		固定操作必须在事务结束前释放。为了效率，如果单个后台进程多次固定同一缓冲区，共享 refcount 不会增加。
 * 		请检查 bufmgr.c 中的 PrivateRefCount 基础架构。
 */


/*
 * =========================================================================
 * 1. 缓冲区管理器共享内存初始化
 * 负责共享缓冲区池及其关联元数据的全局初始化。
 * =========================================================================
 */

/*
 * Initialize shared buffer pool
 * 初始化共享缓冲区池
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 * 这在共享内存初始化期间（无论是在 postmaster 中，还是在单机后台进程中）调用一次。
 */
void
BufferManagerShmemInit(void)
{
	bool		foundBufs,
				foundDescs,
				foundIOCV,
				foundBufCkpt;

	/* Align descriptors to a cacheline boundary.
	 * 将描述符对齐到高速缓存行（cacheline）边界。 */
	BufferDescriptors = (BufferDescPadded *)
		ShmemInitStruct("Buffer Descriptors",
						NBuffers * sizeof(BufferDescPadded),
						&foundDescs);

	/* Align buffer pool on IO page size boundary.
	 * 将缓冲区池对齐到 I/O 页大小边界。 */
	BufferBlocks = (char *)
		TYPEALIGN(PG_IO_ALIGN_SIZE,
				  ShmemInitStruct("Buffer Blocks",
								  NBuffers * (Size) BLCKSZ + PG_IO_ALIGN_SIZE,
								  &foundBufs));

	/* Align condition variables to cacheline boundary.
	 * 将条件变量对齐到高速缓存行边界。 */
	BufferIOCVArray = (ConditionVariableMinimallyPadded *)
		ShmemInitStruct("Buffer IO Condition Variables",
						NBuffers * sizeof(ConditionVariableMinimallyPadded),
						&foundIOCV);

	/*
	 * The array used to sort to-be-checkpointed buffer ids is located in
	 * shared memory, to avoid having to allocate significant amounts of
	 * memory at runtime. As that'd be in the middle of a checkpoint, or when
	 * the checkpointer is restarted, memory allocation failures would be
	 * painful.
	 * 用于对待检查点（checkpoint）缓冲区 ID 进行排序的数组位于共享内存中，以避免在运行时分配大量内存。
	 * 由于那是在检查点过程中或检查点进程重启时，内存分配失败将会非常麻烦。
	 */
	CkptBufferIds = (CkptSortItem *)
		ShmemInitStruct("Checkpoint BufferIds",
						NBuffers * sizeof(CkptSortItem), &foundBufCkpt);

	if (foundDescs || foundBufs || foundIOCV || foundBufCkpt)
	{
		/* should find all of these, or none of them
		 * 必须能找到所有这些，或者一个也找不到 */
		Assert(foundDescs && foundBufs && foundIOCV && foundBufCkpt);
		/* note: this path is only taken in EXEC_BACKEND case
		 * 注意：此路径仅在 EXEC_BACKEND 情况下采用 */
	}
	else
	{
		int			i;

		/*
		 * Initialize all the buffer headers.
		 * 初始化所有的缓冲区头部（header）。
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);

			ClearBufferTag(&buf->tag);

			pg_atomic_init_u32(&buf->state, 0);
			buf->wait_backend_pgprocno = INVALID_PROC_NUMBER;

			buf->buf_id = i;

			pgaio_wref_clear(&buf->io_wref);

			/*
			 * Initially link all the buffers together as unused. Subsequent
			 * management of this list is done by freelist.c.
			 * 最初将所有缓冲区作为未使用的项串联在一起。该列表后续的管理由 freelist.c 完成。
			 */
			buf->freeNext = i + 1;

			LWLockInitialize(BufferDescriptorGetContentLock(buf),
							 LWTRANCHE_BUFFER_CONTENT);

			ConditionVariableInit(BufferDescriptorGetIOCV(buf));
		}

		/* Correct last entry of linked list
		 * 修正链表的最后一个条目 */
		GetBufferDescriptor(NBuffers - 1)->freeNext = FREENEXT_END_OF_LIST;
	}

	/* Init other shared buffer-management stuff
	 * 初始化其他共享缓冲区管理相关内容 */
	StrategyInitialize(!foundDescs);

	/* Initialize per-backend file flush context
	 * 初始化每个后台进程的文件刷新（flush）上下文 */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * =========================================================================
 * 2. 共享内存大小计算
 * 负责计算缓冲区管理器运行所需的总内存量。
 * =========================================================================
 */

/*
 * BufferManagerShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 * 计算缓冲区池所需的共享内存大小，包括数据页、缓冲区描述符、哈希表等。
 */
Size
BufferManagerShmemSize(void)
{
	Size		size = 0;

	/* size of buffer descriptors
	 * 缓冲区描述符的大小 */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDescPadded)));
	/* to allow aligning buffer descriptors
	 * 以允许对齐缓冲区描述符 */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of data pages, plus alignment padding
	 * 数据页大小，外加对齐填充（padding） */
	size = add_size(size, PG_IO_ALIGN_SIZE);
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* size of stuff controlled by freelist.c
	 * 由 freelist.c 控制的内容的大小 */
	size = add_size(size, StrategyShmemSize());

	/* size of I/O condition variables
	 * I/O 条件变量的大小 */
	size = add_size(size, mul_size(NBuffers,
								   sizeof(ConditionVariableMinimallyPadded)));
	/* to allow aligning the above
	 * 以允许对齐上述内容 */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of checkpoint sort array in bufmgr.c
	 * bufmgr.c 中检查点排序数组的大小 */
	size = add_size(size, mul_size(NBuffers, sizeof(CkptSortItem)));

	return size;
}
