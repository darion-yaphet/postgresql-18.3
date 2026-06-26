/*-------------------------------------------------------------------------
 *
 * autoprewarm.c
 *		Periodically dump information about the blocks present in
 *		shared_buffers, and reload them on server restart.
 *
 *		Due to locking considerations, we can't actually begin prewarming
 *		until the server reaches a consistent state.  We need the catalogs
 *		to be consistent so that we can figure out which relation to lock,
 *		and we need to lock the relations so that we don't try to prewarm
 *		pages from a relation that is in the process of being dropped.
 *
 *		While prewarming, autoprewarm will use two workers.  There's a
 *		leader worker that reads and sorts the list of blocks to be
 *		prewarmed and then launches a per-database worker for each
 *		relevant database in turn.  The former keeps running after the
 *		initial prewarm is complete to update the dump file periodically.
 *
 *	Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 *	IDENTIFICATION
 *		contrib/pg_prewarm/autoprewarm.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/relation.h"
#include "access/xact.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/buf_internals.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/procsignal.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/relfilenumbermap.h"
#include "utils/timestamp.h"

#define AUTOPREWARM_FILE "autoprewarm.blocks"

/* Metadata for each block we dump.
 *
 * 我们转储的每个块的元数据。
 */
typedef struct BlockInfoRecord
{
	Oid			database;
	Oid			tablespace;
	RelFileNumber filenumber;
	ForkNumber	forknum;
	BlockNumber blocknum;
} BlockInfoRecord;

/* Shared state information for autoprewarm bgworker.
 *
 * 自动预热 bgworker 的共享状态信息。
 */
typedef struct AutoPrewarmSharedState
{
	LWLock		lock;			/* mutual exclusion */
	pid_t		bgworker_pid;	/* for main bgworker */
	pid_t		pid_using_dumpfile; /* for autoprewarm or block dump */

	/* Following items are for communication with per-database worker
	 *
	 * 以下项目用于与每个数据库工作人员进行通信
	 */
	dsm_handle	block_info_handle;
	Oid			database;
	int			prewarm_start_idx;
	int			prewarm_stop_idx;
	int			prewarmed_blocks;
} AutoPrewarmSharedState;

/*
 * Private data passed through the read stream API for our use in the
 * callback.
 *
 * 通过读取流 API 传递的私有数据供我们在回调中使用。
 */
typedef struct AutoPrewarmReadStreamData
{
	/* The array of records containing the blocks we should prewarm.
	 *
	 * 包含我们应该预热的块的记录数组。
	 */
	BlockInfoRecord *block_info;

	/*
	 * pos is the read stream callback's index into block_info. Because the
	 * read stream may read ahead, pos is likely to be ahead of the index in
	 * the main loop in autoprewarm_database_main().
	 *
	 * pos 是读取流回调在 block_info 中的索引。由于读取流可能会提前读取，因此 pos 很可能位于 autoprewarm_database_main() 中主循环中的索引之前。
	 */
	int			pos;
	Oid			tablespace;
	RelFileNumber filenumber;
	ForkNumber	forknum;
	BlockNumber nblocks;
} AutoPrewarmReadStreamData;


PGDLLEXPORT void autoprewarm_main(Datum main_arg);
PGDLLEXPORT void autoprewarm_database_main(Datum main_arg);

PG_FUNCTION_INFO_V1(autoprewarm_start_worker);
PG_FUNCTION_INFO_V1(autoprewarm_dump_now);

static void apw_load_buffers(void);
static int	apw_dump_now(bool is_bgworker, bool dump_unlogged);
static void apw_start_leader_worker(void);
static void apw_start_database_worker(void);
static bool apw_init_shmem(void);
static void apw_detach_shmem(int code, Datum arg);
static int	apw_compare_blockinfo(const void *p, const void *q);

/* Pointer to shared-memory state.
 *
 * 指向共享内存状态的指针。
 */
static AutoPrewarmSharedState *apw_state = NULL;

/* GUC variables.
 *
 * GUC 变量。
 */
static bool autoprewarm = true; /* start worker? */
static int	autoprewarm_interval = 300; /* dump interval */

/*
 * Module load callback.
 *
 * 模块加载回调。
 */
void
_PG_init(void)
{
	DefineCustomIntVariable("pg_prewarm.autoprewarm_interval",
							"Sets the interval between dumps of shared buffers",
							"If set to zero, time-based dumping is disabled.",
							&autoprewarm_interval,
							300,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* can't define PGC_POSTMASTER variable after startup
	 *
	 * 启动后无法定义 PGC_POSTMASTER 变量
	 */
	DefineCustomBoolVariable("pg_prewarm.autoprewarm",
							 "Starts the autoprewarm worker.",
							 NULL,
							 &autoprewarm,
							 true,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_prewarm");

	/* Register autoprewarm worker, if enabled.
	 *
	 * 注册自动预热工作线程（如果启用）。
	 */
	if (autoprewarm)
		apw_start_leader_worker();
}

/*
 * Main entry point for the leader autoprewarm process.  Per-database workers
 * have a separate entry point.
 *
 * 领导者自动预热过程的主要入口点。  每个数据库工作人员都有一个单独的入口点。
 */
void
autoprewarm_main(Datum main_arg)
{
	bool		first_time = true;
	bool		final_dump_allowed = true;
	TimestampTz last_dump_time = 0;

	/* Establish signal handlers; once that's done, unblock signals.
	 *
	 * 建立信号处理程序；完成后，解锁信号。
	 */
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	BackgroundWorkerUnblockSignals();

	/* Create (if necessary) and attach to our shared memory area.
	 *
	 * 创建（如果需要）并附加到我们的共享内存区域。
	 */
	if (apw_init_shmem())
		first_time = false;

	/*
	 * Set on-detach hook so that our PID will be cleared on exit.
	 *
	 * 设置 on-detach 钩子，以便我们的 PID 在退出时被清除。
	 *
	 * NB: Autoprewarm's state is stored in a DSM segment, and DSM segments
	 * are detached before calling the on_shmem_exit callbacks, so we must put
	 * apw_detach_shmem in the before_shmem_exit callback list.
	 *
	 * 注意：Autoprewarm的状态存储在DSM段中，并且在调用on_shmem_exit回调之前DSM段被分离，因此我们必须将apw_detach_shmem放在before_shmem_exit回调列表中。
	 */
	before_shmem_exit(apw_detach_shmem, 0);

	/*
	 * Store our PID in the shared memory area --- unless there's already
	 * another worker running, in which case just exit.
	 *
	 * 将我们的 PID 存储在共享内存区域中 --- 除非已经有另一个工作进程在运行，在这种情况下只需退出。
	 */
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	if (apw_state->bgworker_pid != InvalidPid)
	{
		LWLockRelease(&apw_state->lock);
		ereport(LOG,
				(errmsg("autoprewarm worker is already running under PID %d",
						(int) apw_state->bgworker_pid)));
		return;
	}
	apw_state->bgworker_pid = MyProcPid;
	LWLockRelease(&apw_state->lock);

	/*
	 * Preload buffers from the dump file only if we just created the shared
	 * memory region.  Otherwise, it's either already been done or shouldn't
	 * be done - e.g. because the old dump file has been overwritten since the
	 * server was started.
	 *
	 * 仅当我们刚刚创建共享内存区域时才从转储文件预加载缓冲区。  否则，它要么已经完成，要么不应该完成 - 例如因为自服务器启动以来旧的转储文件已被覆盖。
	 *
	 * There's not much point in performing a dump immediately after we finish
	 * preloading; so, if we do end up preloading, consider the last dump time
	 * to be equal to the current time.
	 *
	 * 完成预加载后立即执行转储并没有多大意义；因此，如果我们最终进行预加载，请考虑上次转储时间等于当前时间。
	 *
	 * If apw_load_buffers() is terminated early by a shutdown request,
	 * prevent dumping out our state below the loop, because we'd effectively
	 * just truncate the saved state to however much we'd managed to preload.
	 *
	 * 如果 apw_load_buffers() 被关闭请求提前终止，请防止在循环下转储我们的状态，因为我们实际上只是将保存的状态截断为我们设法预加载的状态。
	 */
	if (first_time)
	{
		apw_load_buffers();
		final_dump_allowed = !ShutdownRequestPending;
		last_dump_time = GetCurrentTimestamp();
	}

	/* Periodically dump buffers until terminated.
	 *
	 * 定期转储缓冲区直至终止。
	 */
	while (!ShutdownRequestPending)
	{
		/* In case of a SIGHUP, just reload the configuration.
		 *
		 * 如果出现 SIGHUP，只需重新加载配置即可。
		 */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (autoprewarm_interval <= 0)
		{
			/* We're only dumping at shutdown, so just wait forever.
			 *
			 * 我们只在关闭时倾销，所以请永远等待。
			 */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
							 -1L,
							 PG_WAIT_EXTENSION);
		}
		else
		{
			TimestampTz next_dump_time;
			long		delay_in_ms;

			/* Compute the next dump time.
			 *
			 * 计算下一次转储时间。
			 */
			next_dump_time =
				TimestampTzPlusMilliseconds(last_dump_time,
											autoprewarm_interval * 1000);
			delay_in_ms =
				TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												next_dump_time);

			/* Perform a dump if it's time.
			 *
			 * 如果时机成熟，请执行转储。
			 */
			if (delay_in_ms <= 0)
			{
				last_dump_time = GetCurrentTimestamp();
				apw_dump_now(true, false);
				continue;
			}

			/* Sleep until the next dump time.
			 *
			 * 睡觉直到下一次转储时间。
			 */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 delay_in_ms,
							 PG_WAIT_EXTENSION);
		}

		/* Reset the latch, loop.
		 *
		 * 重置锁存器，循环。
		 */
		ResetLatch(MyLatch);
	}

	/*
	 * Dump one last time.  We assume this is probably the result of a system
	 * shutdown, although it's possible that we've merely been terminated.
	 *
	 * 最后扔一次。  我们认为这可能是系统关闭的结果，尽管我们可能只是被终止了。
	 */
	if (final_dump_allowed)
		apw_dump_now(true, true);
}

/*
 * Read the dump file and launch per-database workers one at a time to
 * prewarm the buffers found there.
 *
 * 读取转储文件并一次启动每个数据库的工作程序以预热其中找到的缓冲区。
 */
static void
apw_load_buffers(void)
{
	FILE	   *file = NULL;
	int			num_elements,
				i;
	BlockInfoRecord *blkinfo;
	dsm_segment *seg;

	/*
	 * Skip the prewarm if the dump file is in use; otherwise, prevent any
	 * other process from writing it while we're using it.
	 *
	 * 如果转储文件正在使用，则跳过预热；否则，在我们使用它时阻止任何其他进程写入它。
	 */
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	if (apw_state->pid_using_dumpfile == InvalidPid)
		apw_state->pid_using_dumpfile = MyProcPid;
	else
	{
		LWLockRelease(&apw_state->lock);
		ereport(LOG,
				(errmsg("skipping prewarm because block dump file is being written by PID %d",
						(int) apw_state->pid_using_dumpfile)));
		return;
	}
	LWLockRelease(&apw_state->lock);

	/*
	 * Open the block dump file.  Exit quietly if it doesn't exist, but report
	 * any other error.
	 *
	 * 打开块转储文件。  如果不存在则安静退出，但报告任何其他错误。
	 */
	file = AllocateFile(AUTOPREWARM_FILE, "r");
	if (!file)
	{
		if (errno == ENOENT)
		{
			LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
			apw_state->pid_using_dumpfile = InvalidPid;
			LWLockRelease(&apw_state->lock);
			return;				/* No file to load. */
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						AUTOPREWARM_FILE)));
	}

	/* First line of the file is a record count.
	 *
	 * 文件的第一行是记录计数。
	 */
	if (fscanf(file, "<<%d>>\n", &num_elements) != 1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						AUTOPREWARM_FILE)));

	/* Allocate a dynamic shared memory segment to store the record data.
	 *
	 * 分配动态共享内存段来存储记录数据。
	 */
	seg = dsm_create(sizeof(BlockInfoRecord) * num_elements, 0);
	blkinfo = (BlockInfoRecord *) dsm_segment_address(seg);

	/* Read records, one per line.
	 *
	 * 读取记录，每行一条。
	 */
	for (i = 0; i < num_elements; i++)
	{
		unsigned	forknum;

		if (fscanf(file, "%u,%u,%u,%u,%u\n", &blkinfo[i].database,
				   &blkinfo[i].tablespace, &blkinfo[i].filenumber,
				   &forknum, &blkinfo[i].blocknum) != 5)
			ereport(ERROR,
					(errmsg("autoprewarm block dump file is corrupted at line %d",
							i + 1)));
		blkinfo[i].forknum = forknum;
	}

	FreeFile(file);

	/* Sort the blocks to be loaded.
	 *
	 * 对要加载的块进行排序。
	 */
	qsort(blkinfo, num_elements, sizeof(BlockInfoRecord),
		  apw_compare_blockinfo);

	/* Populate shared memory state.
	 *
	 * 填充共享内存状态。
	 */
	apw_state->block_info_handle = dsm_segment_handle(seg);
	apw_state->prewarm_start_idx = apw_state->prewarm_stop_idx = 0;
	apw_state->prewarmed_blocks = 0;

	/* Get the info position of the first block of the next database.
	 *
	 * 获取下一个数据库的第一个块的信息位置。
	 */
	while (apw_state->prewarm_start_idx < num_elements)
	{
		int			j = apw_state->prewarm_start_idx;
		Oid			current_db = blkinfo[j].database;

		/*
		 * Advance the prewarm_stop_idx to the first BlockInfoRecord that does
		 * not belong to this database.
		 *
		 * 将 prewarm_stop_idx 前进到不属于该数据库的第一个 BlockInfoRecord。
		 */
		j++;
		while (j < num_elements)
		{
			if (current_db != blkinfo[j].database)
			{
				/*
				 * Combine BlockInfoRecords for global objects with those of
				 * the database.
				 *
				 * 将全局对象的 BlockInfoRecords 与数据库的 BlockInfoRecords 结合起来。
				 */
				if (current_db != InvalidOid)
					break;
				current_db = blkinfo[j].database;
			}

			j++;
		}

		/*
		 * If we reach this point with current_db == InvalidOid, then only
		 * BlockInfoRecords belonging to global objects exist.  We can't
		 * prewarm without a database connection, so just bail out.
		 *
		 * 如果我们以 current_db == InvalidOid 达到这一点，则仅存在属于全局对象的 BlockInfoRecord。  如果没有数据库连接，我们就无法预热，所以就退出吧。
		 */
		if (current_db == InvalidOid)
			break;

		/* Configure stop point and database for next per-database worker.
		 *
		 * 为下一个每个数据库工作线程配置停止点和数据库。
		 */
		apw_state->prewarm_stop_idx = j;
		apw_state->database = current_db;
		Assert(apw_state->prewarm_start_idx < apw_state->prewarm_stop_idx);

		/* If we've run out of free buffers, don't launch another worker.
		 *
		 * 如果我们用完了可用缓冲区，请不要启动另一个工作线程。
		 */
		if (!have_free_buffer())
			break;

		/*
		 * Likewise, don't launch if we've already been told to shut down.
		 * (The launch would fail anyway, but we might as well skip it.)
		 *
		 * 同样，如果我们已经被告知关闭，则不要启动。 （无论如何，启动都会失败，但我们不妨跳过它。）
		 */
		if (ShutdownRequestPending)
			break;

		/*
		 * Start a per-database worker to load blocks for this database; this
		 * function will return once the per-database worker exits.
		 *
		 * 启动每个数据库的工作进程来加载该数据库的块；一旦每个数据库工作线程退出，该函数就会返回。
		 */
		apw_start_database_worker();

		/* Prepare for next database.
		 *
		 * 为下一个数据库做准备。
		 */
		apw_state->prewarm_start_idx = apw_state->prewarm_stop_idx;
	}

	/* Clean up.
	 *
	 * 清理。
	 */
	dsm_detach(seg);
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	apw_state->block_info_handle = DSM_HANDLE_INVALID;
	apw_state->pid_using_dumpfile = InvalidPid;
	LWLockRelease(&apw_state->lock);

	/* Report our success, if we were able to finish.
	 *
	 * 如果我们能够完成，请报告我们的成功。
	 */
	if (!ShutdownRequestPending)
		ereport(LOG,
				(errmsg("autoprewarm successfully prewarmed %d of %d previously-loaded blocks",
						apw_state->prewarmed_blocks, num_elements)));
}

/*
 * Return the next block number of a specific relation and fork to read
 * according to the array of BlockInfoRecord.
 *
 * 根据BlockInfoRecord数组返回要读取的特定关系和分叉的下一个块号。
 */
static BlockNumber
apw_read_stream_next_block(ReadStream *stream,
						   void *callback_private_data,
						   void *per_buffer_data)
{
	AutoPrewarmReadStreamData *p = callback_private_data;

	CHECK_FOR_INTERRUPTS();

	while (p->pos < apw_state->prewarm_stop_idx)
	{
		BlockInfoRecord blk = p->block_info[p->pos];

		if (!have_free_buffer())
		{
			p->pos = apw_state->prewarm_stop_idx;
			return InvalidBlockNumber;
		}

		if (blk.tablespace != p->tablespace)
			return InvalidBlockNumber;

		if (blk.filenumber != p->filenumber)
			return InvalidBlockNumber;

		if (blk.forknum != p->forknum)
			return InvalidBlockNumber;

		p->pos++;

		/*
		 * Check whether blocknum is valid and within fork file size.
		 * Fast-forward through any invalid blocks. We want p->pos to reflect
		 * the location of the next relation or fork before ending the stream.
		 *
		 * 检查 blocknum 是否有效且在 fork 文件大小之内。快进通过任何无效块。我们希望 p->pos 在结束流之前反映下一个关系或分叉的位置。
		 */
		if (blk.blocknum >= p->nblocks)
			continue;

		return blk.blocknum;
	}

	return InvalidBlockNumber;
}

/*
 * Prewarm all blocks for one database (and possibly also global objects, if
 * those got grouped with this database).
 *
 * 预热一个数据库的所有块（也可能预热全局对象，如果这些对象与该数据库分组）。
 */
void
autoprewarm_database_main(Datum main_arg)
{
	BlockInfoRecord *block_info;
	int			i;
	BlockInfoRecord blk;
	dsm_segment *seg;

	/* Establish signal handlers; once that's done, unblock signals.
	 *
	 * 建立信号处理程序；完成后，解锁信号。
	 */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Connect to correct database and get block information.
	 *
	 * 连接到正确的数据库并获取块信息。
	 */
	apw_init_shmem();
	seg = dsm_attach(apw_state->block_info_handle);
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));
	BackgroundWorkerInitializeConnectionByOid(apw_state->database, InvalidOid, 0);
	block_info = (BlockInfoRecord *) dsm_segment_address(seg);

	i = apw_state->prewarm_start_idx;
	blk = block_info[i];

	/*
	 * Loop until we run out of blocks to prewarm or until we run out of free
	 * buffers.
	 *
	 * 循环直到我们用完预热块或直到我们用完可用缓冲区。
	 */
	while (i < apw_state->prewarm_stop_idx && have_free_buffer())
	{
		Oid			tablespace = blk.tablespace;
		RelFileNumber filenumber = blk.filenumber;
		Oid			reloid;
		Relation	rel;

		/*
		 * All blocks between prewarm_start_idx and prewarm_stop_idx should
		 * belong either to global objects or the same database.
		 *
		 * prewarm_start_idx 和 prewarm_stop_idx 之间的所有块应属于全局对象或同一数据库。
		 */
		Assert(blk.database == apw_state->database || blk.database == 0);

		StartTransactionCommand();

		reloid = RelidByRelfilenumber(blk.tablespace, blk.filenumber);
		if (!OidIsValid(reloid) ||
			(rel = try_relation_open(reloid, AccessShareLock)) == NULL)
		{
			/* We failed to open the relation, so there is nothing to close.
			 *
			 * 我们未能打开关系，因此没有什么可以关闭的。
			 */
			CommitTransactionCommand();

			/*
			 * Fast-forward to the next relation. We want to skip all of the
			 * other records referencing this relation since we know we can't
			 * open it. That way, we avoid repeatedly trying and failing to
			 * open the same relation.
			 *
			 * 快进到下一个关系。我们想要跳过引用此关系的所有其他记录，因为我们知道无法打开它。这样，我们就可以避免反复尝试打开同一个关系却失败。
			 */
			for (; i < apw_state->prewarm_stop_idx; i++)
			{
				blk = block_info[i];
				if (blk.tablespace != tablespace ||
					blk.filenumber != filenumber)
					break;
			}

			/* Time to try and open our newfound relation
			 *
			 * 是时候尝试打开我们新建立的关系了
			 */
			continue;
		}

		/*
		 * We have a relation; now let's loop until we find a valid fork of
		 * the relation or we run out of free buffers. Once we've read from
		 * all valid forks or run out of options, we'll close the relation and
		 * move on.
		 *
		 * 我们有关系；现在让我们循环直到找到关系的有效分叉或者用完可用缓冲区。一旦我们读取了所有有效的分叉或用完选项，我们将关闭关系并继续。
		 */
		while (i < apw_state->prewarm_stop_idx &&
			   blk.tablespace == tablespace &&
			   blk.filenumber == filenumber &&
			   have_free_buffer())
		{
			ForkNumber	forknum = blk.forknum;
			BlockNumber nblocks;
			struct AutoPrewarmReadStreamData p;
			ReadStream *stream;
			Buffer		buf;

			/*
			 * smgrexists is not safe for illegal forknum, hence check whether
			 * the passed forknum is valid before using it in smgrexists.
			 *
			 * smgrexists 对于非法的 forknum 并不安全，因此在 smgrexists 中使用它之前请检查传递的 forknum 是否有效。
			 */
			if (blk.forknum <= InvalidForkNumber ||
				blk.forknum > MAX_FORKNUM ||
				!smgrexists(RelationGetSmgr(rel), blk.forknum))
			{
				/*
				 * Fast-forward to the next fork. We want to skip all of the
				 * other records referencing this fork since we already know
				 * it's not valid.
				 *
				 * 快进到下一个分叉。我们想要跳过引用此分叉的所有其他记录，因为我们已经知道它无效。
				 */
				for (; i < apw_state->prewarm_stop_idx; i++)
				{
					blk = block_info[i];
					if (blk.tablespace != tablespace ||
						blk.filenumber != filenumber ||
						blk.forknum != forknum)
						break;
				}

				/* Time to check if this newfound fork is valid
				 *
				 * 是时候检查这个新发现的分叉是否有效了
				 */
				continue;
			}

			nblocks = RelationGetNumberOfBlocksInFork(rel, blk.forknum);

			p = (struct AutoPrewarmReadStreamData)
			{
				.block_info = block_info,
					.pos = i,
					.tablespace = tablespace,
					.filenumber = filenumber,
					.forknum = forknum,
					.nblocks = nblocks,
			};

			stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
												READ_STREAM_DEFAULT |
												READ_STREAM_USE_BATCHING,
												NULL,
												rel,
												p.forknum,
												apw_read_stream_next_block,
												&p,
												0);

			/*
			 * Loop until we've prewarmed all the blocks from this fork. The
			 * read stream callback will check that we still have free buffers
			 * before requesting each block from the read stream API.
			 *
			 * 循环直到我们预热了该分叉中的所有块。读取流回调将在从读取流 API 请求每个块之前检查我们是否仍然有空闲缓冲区。
			 */
			while ((buf = read_stream_next_buffer(stream, NULL)) != InvalidBuffer)
			{
				apw_state->prewarmed_blocks++;
				ReleaseBuffer(buf);
			}

			read_stream_end(stream);

			/* Advance i past all the blocks just prewarmed.
			 *
			 * 推进我经过所有刚刚预热的块。
			 */
			i = p.pos;
			blk = block_info[i];
		}

		relation_close(rel, AccessShareLock);
		CommitTransactionCommand();
	}

	dsm_detach(seg);
}

/*
 * Dump information on blocks in shared buffers.  We use a text format here
 * so that it's easy to understand and even change the file contents if
 * necessary.
 * Returns the number of blocks dumped.
 *
 * 转储共享缓冲区中块的信息。  我们在这里使用文本格式，以便于理解，甚至在必要时更改文件内容。返回转储的块数。
 */
static int
apw_dump_now(bool is_bgworker, bool dump_unlogged)
{
	int			num_blocks;
	int			i;
	int			ret;
	BlockInfoRecord *block_info_array;
	BufferDesc *bufHdr;
	FILE	   *file;
	char		transient_dump_file_path[MAXPGPATH];
	pid_t		pid;

	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	pid = apw_state->pid_using_dumpfile;
	if (apw_state->pid_using_dumpfile == InvalidPid)
		apw_state->pid_using_dumpfile = MyProcPid;
	LWLockRelease(&apw_state->lock);

	if (pid != InvalidPid)
	{
		if (!is_bgworker)
			ereport(ERROR,
					(errmsg("could not perform block dump because dump file is being used by PID %d",
							(int) apw_state->pid_using_dumpfile)));

		ereport(LOG,
				(errmsg("skipping block dump because it is already being performed by PID %d",
						(int) apw_state->pid_using_dumpfile)));
		return 0;
	}

	/*
	 * With sufficiently large shared_buffers, allocation will exceed 1GB, so
	 * allow for a huge allocation to prevent outright failure.
	 *
	 * 如果共享缓冲区足够大，分配将超过 1GB，因此允许进行巨大分配以防止彻底失败。
	 *
	 * (In the future, it might be a good idea to redesign this to use a more
	 * memory-efficient data structure.)
	 *
	 * （将来，重新设计它以使用内存效率更高的数据结构可能是个好主意。）
	 */
	block_info_array = (BlockInfoRecord *)
		palloc_extended((sizeof(BlockInfoRecord) * NBuffers), MCXT_ALLOC_HUGE);

	for (num_blocks = 0, i = 0; i < NBuffers; i++)
	{
		uint32		buf_state;

		CHECK_FOR_INTERRUPTS();

		bufHdr = GetBufferDescriptor(i);

		/* Lock each buffer header before inspecting.
		 *
		 * 在检查之前锁定每个缓冲区标头。
		 */
		buf_state = LockBufHdr(bufHdr);

		/*
		 * Unlogged tables will be automatically truncated after a crash or
		 * unclean shutdown. In such cases we need not prewarm them. Dump them
		 * only if requested by caller.
		 *
		 * 崩溃或不正常关闭后，未记录的表将被自动截断。在这种情况下，我们不需要预热它们。仅在调用者请求时转储它们。
		 */
		if (buf_state & BM_TAG_VALID &&
			((buf_state & BM_PERMANENT) || dump_unlogged))
		{
			block_info_array[num_blocks].database = bufHdr->tag.dbOid;
			block_info_array[num_blocks].tablespace = bufHdr->tag.spcOid;
			block_info_array[num_blocks].filenumber =
				BufTagGetRelNumber(&bufHdr->tag);
			block_info_array[num_blocks].forknum =
				BufTagGetForkNum(&bufHdr->tag);
			block_info_array[num_blocks].blocknum = bufHdr->tag.blockNum;
			++num_blocks;
		}

		UnlockBufHdr(bufHdr, buf_state);
	}

	snprintf(transient_dump_file_path, MAXPGPATH, "%s.tmp", AUTOPREWARM_FILE);
	file = AllocateFile(transient_dump_file_path, "w");
	if (!file)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						transient_dump_file_path)));

	ret = fprintf(file, "<<%d>>\n", num_blocks);
	if (ret < 0)
	{
		int			save_errno = errno;

		FreeFile(file);
		unlink(transient_dump_file_path);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						transient_dump_file_path)));
	}

	for (i = 0; i < num_blocks; i++)
	{
		CHECK_FOR_INTERRUPTS();

		ret = fprintf(file, "%u,%u,%u,%u,%u\n",
					  block_info_array[i].database,
					  block_info_array[i].tablespace,
					  block_info_array[i].filenumber,
					  (uint32) block_info_array[i].forknum,
					  block_info_array[i].blocknum);
		if (ret < 0)
		{
			int			save_errno = errno;

			FreeFile(file);
			unlink(transient_dump_file_path);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							transient_dump_file_path)));
		}
	}

	pfree(block_info_array);

	/*
	 * Rename transient_dump_file_path to AUTOPREWARM_FILE to make things
	 * permanent.
	 *
	 * 将transient_dump_file_path重命名为AUTOPREWARM_FILE以使事情永久化。
	 */
	ret = FreeFile(file);
	if (ret != 0)
	{
		int			save_errno = errno;

		unlink(transient_dump_file_path);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						transient_dump_file_path)));
	}

	(void) durable_rename(transient_dump_file_path, AUTOPREWARM_FILE, ERROR);
	apw_state->pid_using_dumpfile = InvalidPid;

	ereport(DEBUG1,
			(errmsg_internal("wrote block details for %d blocks", num_blocks)));
	return num_blocks;
}

/*
 * SQL-callable function to launch autoprewarm.
 *
 * 用于启动自动预热的 SQL 可调用函数。
 */
Datum
autoprewarm_start_worker(PG_FUNCTION_ARGS)
{
	pid_t		pid;

	if (!autoprewarm)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("autoprewarm is disabled")));

	apw_init_shmem();
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	pid = apw_state->bgworker_pid;
	LWLockRelease(&apw_state->lock);

	if (pid != InvalidPid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("autoprewarm worker is already running under PID %d",
						(int) pid)));

	apw_start_leader_worker();

	PG_RETURN_VOID();
}

/*
 * SQL-callable function to perform an immediate block dump.
 *
 * SQL 可调用函数来执行立即块转储。
 *
 * Note: this is declared to return int8, as insurance against some
 * very distant day when we might make NBuffers wider than int.
 *
 * 注意：声明返回 int8，作为针对某个非常遥远的日子我们可能使 NBuffer 比 int 更宽的保险。
 */
Datum
autoprewarm_dump_now(PG_FUNCTION_ARGS)
{
	int			num_blocks;

	apw_init_shmem();

	PG_ENSURE_ERROR_CLEANUP(apw_detach_shmem, 0);
	{
		num_blocks = apw_dump_now(false, true);
	}
	PG_END_ENSURE_ERROR_CLEANUP(apw_detach_shmem, 0);

	PG_RETURN_INT64((int64) num_blocks);
}

static void
apw_init_state(void *ptr)
{
	AutoPrewarmSharedState *state = (AutoPrewarmSharedState *) ptr;

	LWLockInitialize(&state->lock, LWLockNewTrancheId());
	state->bgworker_pid = InvalidPid;
	state->pid_using_dumpfile = InvalidPid;
}

/*
 * Allocate and initialize autoprewarm related shared memory, if not already
 * done, and set up backend-local pointer to that state.  Returns true if an
 * existing shared memory segment was found.
 *
 * 分配并初始化与自动预热相关的共享内存（如果尚未完成），并设置指向该状态的后端本地指针。  如果找到现有共享内存段，则返回 true。
 */
static bool
apw_init_shmem(void)
{
	bool		found;

	apw_state = GetNamedDSMSegment("autoprewarm",
								   sizeof(AutoPrewarmSharedState),
								   apw_init_state,
								   &found);
	LWLockRegisterTranche(apw_state->lock.tranche, "autoprewarm");

	return found;
}

/*
 * Clear our PID from autoprewarm shared state.
 *
 * 将 PID 从自动预热共享状态中清除。
 */
static void
apw_detach_shmem(int code, Datum arg)
{
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	if (apw_state->pid_using_dumpfile == MyProcPid)
		apw_state->pid_using_dumpfile = InvalidPid;
	if (apw_state->bgworker_pid == MyProcPid)
		apw_state->bgworker_pid = InvalidPid;
	LWLockRelease(&apw_state->lock);
}

/*
 * Start autoprewarm leader worker process.
 *
 * 启动自动预热领导者工作进程。
 */
static void
apw_start_leader_worker(void)
{
	BackgroundWorker worker = {0};
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	strcpy(worker.bgw_library_name, "pg_prewarm");
	strcpy(worker.bgw_function_name, "autoprewarm_main");
	strcpy(worker.bgw_name, "autoprewarm leader");
	strcpy(worker.bgw_type, "autoprewarm leader");

	if (process_shared_preload_libraries_in_progress)
	{
		RegisterBackgroundWorker(&worker);
		return;
	}

	/* must set notify PID to wait for startup
	 *
	 * 必须设置notify PID等待启动
	 */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You may need to increase \"max_worker_processes\".")));

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));
}

/*
 * Start autoprewarm per-database worker process.
 *
 * 启动每个数据库工作进程的自动预热。
 */
static void
apw_start_database_worker(void)
{
	BackgroundWorker worker = {0};
	BackgroundWorkerHandle *handle;

	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strcpy(worker.bgw_library_name, "pg_prewarm");
	strcpy(worker.bgw_function_name, "autoprewarm_database_main");
	strcpy(worker.bgw_name, "autoprewarm worker");
	strcpy(worker.bgw_type, "autoprewarm worker");

	/* must set notify PID to wait for shutdown
	 *
	 * 必须设置notify PID等待关机
	 */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("registering dynamic bgworker autoprewarm failed"),
				 errhint("Consider increasing the configuration parameter \"%s\".", "max_worker_processes")));

	/*
	 * Ignore return value; if it fails, postmaster has died, but we have
	 * checks for that elsewhere.
	 *
	 * 忽略返回值；如果失败，邮政局长就死了，但我们在其他地方对此进行了检查。
	 */
	WaitForBackgroundWorkerShutdown(handle);
}

/* Compare member elements to check whether they are not equal.
 *
 * 比较成员元素以检查它们是否不相等。
 */
#define cmp_member_elem(fld)	\
do { \
	if (a->fld < b->fld)		\
		return -1;				\
	else if (a->fld > b->fld)	\
		return 1;				\
} while(0)

/*
 * apw_compare_blockinfo
 *
 * We depend on all records for a particular database being consecutive
 * in the dump file; each per-database worker will preload blocks until
 * it sees a block for some other database.  Sorting by tablespace,
 * filenumber, forknum, and blocknum isn't critical for correctness, but
 * helps us get a sequential I/O pattern.
 *
 * 我们依赖于转储文件中特定数据库的所有记录都是连续的；每个数据库工作线程都会预加载块，直到它看到其他数据库的块为止。  按表空间、文件号、forknum 和 blocknum 排序对于正确性并不重要，但可以帮助我们获得顺序 I/O 模式。
 */
static int
apw_compare_blockinfo(const void *p, const void *q)
{
	const BlockInfoRecord *a = (const BlockInfoRecord *) p;
	const BlockInfoRecord *b = (const BlockInfoRecord *) q;

	cmp_member_elem(database);
	cmp_member_elem(tablespace);
	cmp_member_elem(filenumber);
	cmp_member_elem(forknum);
	cmp_member_elem(blocknum);

	return 0;
}
