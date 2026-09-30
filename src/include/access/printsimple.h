/*-------------------------------------------------------------------------
 *
 * printsimple.h
 *	  print simple tuples without catalog access
 *
 *
 *	  无需目录访问即可输出简单元组。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/printsimple.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PRINTSIMPLE_H
#define PRINTSIMPLE_H

#include "tcop/dest.h"

/* Send one simple tuple to a destination receiver.
 * The routine formats values using the slot's immediately available metadata
 * and returns whether delivery succeeded.
 *
 * 向目标接收器发送一个简单元组。
 * 此例程使用槽中立即可用的元数据格式化值，并返回发送是否成功。
 */
extern bool printsimple(TupleTableSlot *slot, DestReceiver *self);

/* Initialize a simple tuple destination receiver.
 * Startup records the output operation and tuple descriptor used for later
 * tuple formatting.
 *
 * 初始化简单元组目标接收器。
 * 启动过程记录后续元组格式化所使用的输出操作和元组描述符。
 */
extern void printsimple_startup(DestReceiver *self, int operation,
								TupleDesc tupdesc);

#endif							/* PRINTSIMPLE_H */
