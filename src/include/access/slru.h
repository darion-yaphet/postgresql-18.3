/*-------------------------------------------------------------------------
 *
 * slru.h
 *		Simple LRU buffering for transaction status logfiles
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slru.h
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLRU_H
#define SLRU_H

#include "access/xlogdefs.h"
#include "storage/lwlock.h"
#include "storage/sync.h"

/*
 * To avoid overflowing internal arithmetic and the size_t data type, the
 * number of buffers must not exceed this number.
 *
 * 为避免内部算术运算和 size_t 数据类型溢出，缓冲区数量不得超过此值。
 */
#define SLRU_MAX_ALLOWED_BUFFERS ((1024 * 1024 * 1024) / BLCKSZ)

/*
 * Define SLRU segment size.  A page is the same BLCKSZ as is used everywhere
 * else in Postgres.  The segment size can be chosen somewhat arbitrarily;
 * we make it 32 pages by default, or 256Kb, i.e. 1M transactions for CLOG
 * or 64K transactions for SUBTRANS.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * page numbering also wraps around at 0xFFFFFFFF/xxxx_XACTS_PER_PAGE (where
 * xxxx is CLOG or SUBTRANS, respectively), and segment numbering at
 * 0xFFFFFFFF/xxxx_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need
 * take no explicit notice of that fact in slru.c, except when comparing
 * segment and page numbers in SimpleLruTruncate (see PagePrecedes()).
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 */
#define SLRU_PAGES_PER_SEGMENT	32

/*
 * Page status codes.  Note that these do not include the "dirty" bit.
 * page_dirty can be true only in the VALID or WRITE_IN_PROGRESS states;
 * in the latter case it implies that the page has been re-dirtied since
 * the write started.
 *
 * 页面状态码。注意这些状态码不包含“脏”位。page_dirty 只有在 VALID 或 WRITE_IN_PROGRESS 状态下才可能为真；在后一种情况下，它意味着自写出开始以来该页面又被重新弄脏。
 */
typedef enum
{
	SLRU_PAGE_EMPTY,			/* buffer is not in use */

	/* 缓冲区未被使用。 */
	SLRU_PAGE_READ_IN_PROGRESS, /* page is being read in */

	/* 正在读入页面。 */
	SLRU_PAGE_VALID,			/* page is valid and not being written */

	/* 页面有效且未在写出。 */
	SLRU_PAGE_WRITE_IN_PROGRESS,	/* page is being written out */

	/* 正在写出页面。 */
} SlruPageStatus;

/*
 * Shared-memory state
 *
 * 共享内存状态。
 *
 * SLRU bank locks are used to protect access to the other fields, except
 * latest_page_number, which uses atomics; see comment in slru.c.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 */
typedef struct SlruSharedData
{
	/* Number of buffers managed by this SLRU structure */

	/* 对应项目的数量。 */
	int			num_slots;

	/*
	 * Arrays holding info for each buffer slot.  Page number is undefined
	 * when status is EMPTY, as is page_lru_count.
 *
 * 保存每个缓冲区槽位信息的数组。当状态为 EMPTY 时，页号未定义，page_lru_count 也是如此。
	 */
	char	  **page_buffer;
	SlruPageStatus *page_status;
	bool	   *page_dirty;
	int64	   *page_number;
	int		   *page_lru_count;

	/* The buffer_locks protects the I/O on each buffer slots */

	/* buffer_locks 保护各缓冲区槽位上的 I/O。 */
	LWLockPadded *buffer_locks;

	/* Locks to protect the in memory buffer slot access in SLRU bank. */

	/* 用于保护 SLRU 分区中内存缓冲区槽位访问的锁。 */
	LWLockPadded *bank_locks;

	/*----------
	 * A bank-wise LRU counter is maintained because we do a victim buffer
	 * search within a bank. Furthermore, manipulating an individual bank
	 * counter avoids frequent cache invalidation since we update it every time
	 * we access the page.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
	 *
	 * We mark a page "most recently used" by setting
	 *		page_lru_count[slotno] = ++bank_cur_lru_count[bankno];
	 * The oldest page in the bank is therefore the one with the highest value
	 * of
	 * 		bank_cur_lru_count[bankno] - page_lru_count[slotno]
	 * The counts will eventually wrap around, but this calculation still
	 * works as long as no page's age exceeds INT_MAX counts.
 *
 * 我们通过设置
 *		page_lru_count[slotno] = ++bank_cur_lru_count[bankno];
 * 将某个页面标记为“最近使用”。因此，分区中最旧的页面就是使
 * 		bank_cur_lru_count[bankno] - page_lru_count[slotno]
 * 取值最大的那个页面。计数最终会回绕，但只要没有页面的年龄超过 INT_MAX 次计数，该计算仍然有效。
	 *----------
	 */
	int		   *bank_cur_lru_count;

	/*
	 * Optional array of WAL flush LSNs associated with entries in the SLRU
	 * pages.  If not zero/NULL, we must flush WAL before writing pages (true
	 * for pg_xact, false for everything else).  group_lsn[] has
	 * lsn_groups_per_page entries per buffer slot, each containing the
	 * highest LSN known for a contiguous group of SLRU entries on that slot's
	 * page.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
	 */
	XLogRecPtr *group_lsn;
	int			lsn_groups_per_page;

	/*
	 * latest_page_number is the page number of the current end of the log;
	 * this is not critical data, since we use it only to avoid swapping out
	 * the latest page.  (An exception: an accurate latest_page_number is
	 * needed on pg_multixact/offsets to replay WAL generated with older minor
	 * versions correctly.  See RecordNewMultiXact().)
 *
 * 说明预写式日志（WAL）相关的状态、记录或恢复约束。
	 */
	pg_atomic_uint64 latest_page_number;

	/* SLRU's index for statistics purposes (might not be unique) */

	/* 用于统计的 SLRU 索引（可能不唯一）。 */
	int			slru_stats_idx;
} SlruSharedData;

typedef SlruSharedData *SlruShared;

/*
 * SlruCtlData is an unshared structure that points to the active information
 * in shared memory.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 */
typedef struct SlruCtlData
{
	SlruShared	shared;

	/* Number of banks in this SLRU. */

	/* 对应项目的数量。 */
	uint16		nbanks;

	/*
	 * If true, use long segment file names.  Otherwise, use short file names.
 *
 * 若为真，则使用长段文件名；否则使用短文件名。
	 *
	 * For details about the file name format, see SlruFileName().
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
	 */
	bool		long_segment_names;

	/*
	 * Which sync handler function to use when handing sync requests over to
	 * the checkpointer.  SYNC_HANDLER_NONE to disable fsync (eg pg_notify).
 *
 * 在将同步请求移交给 checkpointer 时使用哪个同步处理函数。SYNC_HANDLER_NONE 表示禁用 fsync（例如 pg_notify）。
	 */
	SyncRequestHandler sync_handler;

	/*
	 * Decide whether a page is "older" for truncation and as a hint for
	 * evicting pages in LRU order.  Return true if every entry of the first
	 * argument is older than every entry of the second argument.  Note that
	 * !PagePrecedes(a,b) && !PagePrecedes(b,a) need not imply a==b; it also
	 * arises when some entries are older and some are not.  For SLRUs using
	 * SimpleLruTruncate(), this must use modular arithmetic.  (For others,
	 * the behavior of this callback has no functional implications.)  Use
	 * SlruPagePrecedesUnitTests() in SLRUs meeting its criteria.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
	 */
	bool		(*PagePrecedes) (int64, int64);

	/*
	 * Dir is set during SimpleLruInit and does not change thereafter. Since
	 * it's always the same, it doesn't need to be in shared memory.
 *
 * Dir 在 SimpleLruInit 期间设置，之后不再改变。由于它始终相同，因此无需放在共享内存中。
	 */
	char		Dir[64];
} SlruCtlData;

typedef SlruCtlData *SlruCtl;

/*
 * Get the SLRU bank lock for given SlruCtl and the pageno.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 *
 * This lock needs to be acquired to access the slru buffer slots in the
 * respective bank.
 *
 * 说明简单 LRU（SLRU）缓存的页面状态、锁、目录扫描或维护机制。
 */
/*
 * Function: SimpleLruGetBankLock.
 * Purpose: Performs the operation represented by simple lru get bank lock.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruGetBankLock。
 * 作用：执行 simple lru get bank lock 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline LWLock *
SimpleLruGetBankLock(SlruCtl ctl, int64 pageno)
{
	int			bankno;

	bankno = pageno % ctl->nbanks;
	return &(ctl->shared->bank_locks[bankno].lock);
}

/*
 * Function: SimpleLruShmemSize.
 * Purpose: Reports the shared-memory space required by simple lru shmem size.
 * Core flow: It derives the allocation size from subsystem structures before initialization.
 *
 * 函数：SimpleLruShmemSize。
 * 作用：报告 simple lru shmem size 所需的共享内存空间。
 * 核心流程：它在初始化前根据子系统结构计算分配大小。
 */
extern Size SimpleLruShmemSize(int nslots, int nlsns);
/*
 * Function: SimpleLruAutotuneBuffers.
 * Purpose: Performs the operation represented by simple lru autotune buffers.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruAutotuneBuffers。
 * 作用：执行 simple lru autotune buffers 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern int	SimpleLruAutotuneBuffers(int divisor, int max);
/*
 * Function: SimpleLruInit.
 * Purpose: Performs the operation represented by simple lru init.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruInit。
 * 作用：执行 simple lru init 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SimpleLruInit(SlruCtl ctl, const char *name, int nslots, int nlsns,
						  const char *subdir, int buffer_tranche_id,
						  int bank_tranche_id, SyncRequestHandler sync_handler,
						  bool long_segment_names);
/*
 * Function: SimpleLruZeroPage.
 * Purpose: Performs the operation represented by simple lru zero page.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruZeroPage。
 * 作用：执行 simple lru zero page 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern int	SimpleLruZeroPage(SlruCtl ctl, int64 pageno);
/*
 * Function: SimpleLruReadPage.
 * Purpose: Performs the operation represented by simple lru read page.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruReadPage。
 * 作用：执行 simple lru read page 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern int	SimpleLruReadPage(SlruCtl ctl, int64 pageno, bool write_ok,
							  TransactionId xid);
/*
 * Function: SimpleLruReadPage_ReadOnly.
 * Purpose: Performs the operation represented by simple lru read page read only.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruReadPage_ReadOnly。
 * 作用：执行 simple lru read page read only 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern int	SimpleLruReadPage_ReadOnly(SlruCtl ctl, int64 pageno,
									   TransactionId xid);
/*
 * Function: SimpleLruWritePage.
 * Purpose: Performs the operation represented by simple lru write page.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruWritePage。
 * 作用：执行 simple lru write page 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SimpleLruWritePage(SlruCtl ctl, int slotno);
/*
 * Function: SimpleLruWriteAll.
 * Purpose: Performs the operation represented by simple lru write all.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruWriteAll。
 * 作用：执行 simple lru write all 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SimpleLruWriteAll(SlruCtl ctl, bool allow_redirtied);
#ifdef USE_ASSERT_CHECKING
/*
 * Function: SlruPagePrecedesUnitTests.
 * Purpose: Performs the operation represented by slru page precedes unit tests.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SlruPagePrecedesUnitTests。
 * 作用：执行 slru page precedes unit tests 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SlruPagePrecedesUnitTests(SlruCtl ctl, int per_page);
#else
#define SlruPagePrecedesUnitTests(ctl, per_page) do {} while (0)
#endif
/*
 * Function: SimpleLruTruncate.
 * Purpose: Performs the operation represented by simple lru truncate.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruTruncate。
 * 作用：执行 simple lru truncate 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SimpleLruTruncate(SlruCtl ctl, int64 cutoffPage);
/*
 * Function: SimpleLruDoesPhysicalPageExist.
 * Purpose: Performs the operation represented by simple lru does physical page exist.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SimpleLruDoesPhysicalPageExist。
 * 作用：执行 simple lru does physical page exist 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool SimpleLruDoesPhysicalPageExist(SlruCtl ctl, int64 pageno);

typedef bool (*SlruScanCallback) (SlruCtl ctl, char *filename, int64 segpage,
								  void *data);
/*
 * Function: SlruScanDirectory.
 * Purpose: Performs the operation represented by slru scan directory.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SlruScanDirectory。
 * 作用：执行 slru scan directory 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool SlruScanDirectory(SlruCtl ctl, SlruScanCallback callback, void *data);
/*
 * Function: SlruDeleteSegment.
 * Purpose: Performs the operation represented by slru delete segment.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SlruDeleteSegment。
 * 作用：执行 slru delete segment 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void SlruDeleteSegment(SlruCtl ctl, int64 segno);

/*
 * Function: SlruSyncFileTag.
 * Purpose: Performs the operation represented by slru sync file tag.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SlruSyncFileTag。
 * 作用：执行 slru sync file tag 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern int	SlruSyncFileTag(SlruCtl ctl, const FileTag *ftag, char *path);

/* SlruScanDirectory public callbacks */

/* SlruScanDirectory 的公共回调函数。 */
/*
 * Function: SlruScanDirCbReportPresence.
 * Purpose: Performs the operation represented by slru scan dir cb report presence.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SlruScanDirCbReportPresence。
 * 作用：执行 slru scan dir cb report presence 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool SlruScanDirCbReportPresence(SlruCtl ctl, char *filename,
										int64 segpage, void *data);
/*
 * Function: SlruScanDirCbDeleteAll.
 * Purpose: Performs the operation represented by slru scan dir cb delete all.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：SlruScanDirCbDeleteAll。
 * 作用：执行 slru scan dir cb delete all 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool SlruScanDirCbDeleteAll(SlruCtl ctl, char *filename, int64 segpage,
								   void *data);
/*
 * Function: check_slru_buffers.
 * Purpose: Obtains or checks the state represented by check slru buffers.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：check_slru_buffers。
 * 作用：获取或检查 check slru buffers 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern bool check_slru_buffers(const char *name, int *newval);

#endif							/* SLRU_H */
