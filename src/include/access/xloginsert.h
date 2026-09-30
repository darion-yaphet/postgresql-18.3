/*
 * xloginsert.h
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 *
 * Functions for generating WAL records
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xloginsert.h
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
#ifndef XLOGINSERT_H
#define XLOGINSERT_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/*
 * The minimum size of the WAL construction working area. If you need to
 * register more than XLR_NORMAL_MAX_BLOCK_ID block references or have more
 * than XLR_NORMAL_RDATAS data chunks in a single WAL record, you must call
 * XLogEnsureRecordSpace() first to allocate more working memory.
 *
 * 说明预写式日志（WAL）的记录、插入、预取或文件处理约束。
 */
#define XLR_NORMAL_MAX_BLOCK_ID		4
#define XLR_NORMAL_RDATAS			20

/* flags for XLogRegisterBuffer */

/* XLogRegisterBuffer 的标志。 */
#define REGBUF_FORCE_IMAGE	0x01	/* force a full-page image */

/* 强制生成完整页面镜像。 */
#define REGBUF_NO_IMAGE		0x02	/* don't take a full-page image */

/* 不生成完整页面镜像。 */
#define REGBUF_WILL_INIT	(0x04 | 0x02)	/* page will be re-initialized at
											 * replay (implies NO_IMAGE) */
#define REGBUF_STANDARD		0x08	/* page follows "standard" page layout,
									 * (data between pd_lower and pd_upper
									 * will be skipped) */
#define REGBUF_KEEP_DATA	0x10	/* include data even if a full-page image
									 * is taken */
#define REGBUF_NO_CHANGE	0x20	/* intentionally register clean buffer */

/* 有意注册干净缓冲区。 */

/* prototypes for public functions in xloginsert.c: */

/* xloginsert.c 中公共函数的原型。 */
/*
 * Function: XLogBeginInsert.
 * Purpose: Performs the WAL operation represented by xlog begin insert.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogBeginInsert。
 * 作用：执行 xlog begin insert 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogBeginInsert(void);
/*
 * Function: XLogSetRecordFlags.
 * Purpose: Performs the WAL operation represented by xlog set record flags.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogSetRecordFlags。
 * 作用：执行 xlog set record flags 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogSetRecordFlags(uint8 flags);
/*
 * Function: XLogInsert.
 * Purpose: Performs the WAL operation represented by xlog insert.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogInsert。
 * 作用：执行 xlog insert 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogRecPtr XLogInsert(RmgrId rmid, uint8 info);
/*
 * Function: XLogEnsureRecordSpace.
 * Purpose: Performs the WAL operation represented by xlog ensure record space.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogEnsureRecordSpace。
 * 作用：执行 xlog ensure record space 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogEnsureRecordSpace(int max_block_id, int ndatas);
/*
 * Function: XLogRegisterData.
 * Purpose: Performs the WAL operation represented by xlog register data.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogRegisterData。
 * 作用：执行 xlog register data 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogRegisterData(const void *data, uint32 len);
/*
 * Function: XLogRegisterBuffer.
 * Purpose: Performs the WAL operation represented by xlog register buffer.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogRegisterBuffer。
 * 作用：执行 xlog register buffer 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogRegisterBuffer(uint8 block_id, Buffer buffer, uint8 flags);
/*
 * Function: XLogRegisterBlock.
 * Purpose: Performs the WAL operation represented by xlog register block.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogRegisterBlock。
 * 作用：执行 xlog register block 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogRegisterBlock(uint8 block_id, RelFileLocator *rlocator,
							  ForkNumber forknum, BlockNumber blknum, const PageData *page,
							  uint8 flags);
/*
 * Function: XLogRegisterBufData.
 * Purpose: Performs the WAL operation represented by xlog register buf data.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogRegisterBufData。
 * 作用：执行 xlog register buf data 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogRegisterBufData(uint8 block_id, const void *data, uint32 len);
/*
 * Function: XLogResetInsertion.
 * Purpose: Performs the WAL operation represented by xlog reset insertion.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogResetInsertion。
 * 作用：执行 xlog reset insertion 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void XLogResetInsertion(void);
/*
 * Function: XLogCheckBufferNeedsBackup.
 * Purpose: Performs the WAL operation represented by xlog check buffer needs backup.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogCheckBufferNeedsBackup。
 * 作用：执行 xlog check buffer needs backup 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern bool XLogCheckBufferNeedsBackup(Buffer buffer);

/*
 * Function: log_newpage.
 * Purpose: Performs the WAL operation represented by log newpage.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：log_newpage。
 * 作用：执行 log newpage 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogRecPtr log_newpage(RelFileLocator *rlocator, ForkNumber forknum,
							  BlockNumber blkno, Page page, bool page_std);
/*
 * Function: log_newpages.
 * Purpose: Performs the WAL operation represented by log newpages.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：log_newpages。
 * 作用：执行 log newpages 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void log_newpages(RelFileLocator *rlocator, ForkNumber forknum, int num_pages,
						 BlockNumber *blknos, Page *pages, bool page_std);
/*
 * Function: log_newpage_buffer.
 * Purpose: Performs the WAL operation represented by log newpage buffer.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：log_newpage_buffer。
 * 作用：执行 log newpage buffer 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogRecPtr log_newpage_buffer(Buffer buffer, bool page_std);
/*
 * Function: log_newpage_range.
 * Purpose: Performs the WAL operation represented by log newpage range.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：log_newpage_range。
 * 作用：执行 log newpage range 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void log_newpage_range(Relation rel, ForkNumber forknum,
							  BlockNumber startblk, BlockNumber endblk, bool page_std);
/*
 * Function: XLogSaveBufferForHint.
 * Purpose: Performs the WAL operation represented by xlog save buffer for hint.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：XLogSaveBufferForHint。
 * 作用：执行 xlog save buffer for hint 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern XLogRecPtr XLogSaveBufferForHint(Buffer buffer, bool buffer_std);

/*
 * Function: InitXLogInsert.
 * Purpose: Performs the WAL operation represented by init xlog insert.
 * Core flow: It prepares record-insertion or file context, performs the requested operation, and keeps WAL metadata consistent.
 *
 * 函数：InitXLogInsert。
 * 作用：执行 init xlog insert 所表示的 WAL 操作。
 * 核心流程：它准备记录插入或文件上下文，完成请求操作，并保持 WAL 元数据一致。
 */
extern void InitXLogInsert(void);

#endif							/* XLOGINSERT_H */
