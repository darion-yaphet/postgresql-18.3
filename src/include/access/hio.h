/*-------------------------------------------------------------------------
 *
 * hio.h
 *	  POSTGRES heap access method input/output definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hio.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * hio.h POSTGRES 堆访问方法输入/输出定义。 src/i
 * nclude/access/hio.h
 */
#ifndef HIO_H
#define HIO_H

#include "access/htup.h"
#include "storage/buf.h"
#include "utils/relcache.h"

/*
 * state for bulk inserts --- private to heapam.c and hio.c
 *
 * If current_buf isn't InvalidBuffer, then we are holding an extra pin
 * on that buffer.
 *
 * "typedef struct BulkInsertStateData *BulkInsertState" is in heapam.h
 *
 * 中文翻译：
 * 批量插入的状态 --- heapam.c 和 hio.c 私有 如果
 *  current_buf 不是 InvalidBuffer，那么我们
 * 在该缓冲区上持有一个额外的 pin。 “typedef struct
 *  BulkInsertStateData *BulkInsertSt
 * ate”位于 heapam.h 中
 */
typedef struct BulkInsertStateData
{
	BufferAccessStrategy strategy;	/* our BULKWRITE strategy object */

	/* 中文翻译：我们的 BULKWRITE 策略对象 */
	Buffer		current_buf;	/* current insertion target page */

	/* 中文翻译：当前插入目标页面 */

	/*
	 * State for bulk extensions.
	 *
	 * last_free..next_free are further pages that were unused at the time of
	 * the last extension. They might be in use by the time we use them
	 * though, so rechecks are needed.
	 *
	 * XXX: Eventually these should probably live in RelationData instead,
	 * alongside targetblock.
	 *
	 * already_extended_by is the number of pages that this bulk inserted
	 * extended by. If we already extended by a significant number of pages,
	 * we can be more aggressive about extending going forward.
	 *
	 * 中文翻译：
	 * 批量扩展的状态。 last_free..next_free 是上次扩
	 * 展时未使用的其他页面。不过，我们使用它们时它们可能正在使用中，因此需
	 * 要重新检查。 XXX：最终这些可能应该与 targetblock 一
	 * 起存在于 RelationData 中。 has_extended_
	 * by 是此批量插入扩展的页数。如果我们已经扩展了大量的页面，我们可以
	 * 更加积极地继续扩展。
	 */
	BlockNumber next_free;
	BlockNumber last_free;
	uint32		already_extended_by;
} BulkInsertStateData;


/*
 * Function RelationPutHeapTuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 RelationPutHeapTuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void RelationPutHeapTuple(Relation relation, Buffer buffer,
								 HeapTuple tuple, bool token);
/*
 * Function RelationGetBufferForTuple retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 RelationGetBufferForTuple通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Buffer RelationGetBufferForTuple(Relation relation, Size len,
										Buffer otherBuffer, int options,
										BulkInsertStateData *bistate,
										Buffer *vmbuffer, Buffer *vmbuffer_other,
										int num_pages);

#endif							/* HIO_H */

/* 中文翻译：HIO_H */
