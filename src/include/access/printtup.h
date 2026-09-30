/*-------------------------------------------------------------------------
 *
 * printtup.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/printtup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRINTTUP_H
#define PRINTTUP_H

#include "utils/portal.h"

/* Create a destination receiver that sends tuples to a client.
 * The routine selects receiver behavior from the command destination and
 * returns the initialized receiver.
 *
 * 创建向客户端发送元组的目标接收器。
 * 此例程根据命令目标选择接收器行为，并返回初始化后的接收器。
 */
extern DestReceiver *printtup_create_DR(CommandDest dest);

/* Set portal-specific output parameters on a remote receiver.
 * The routine copies portal format and protocol state used when rows are sent.
 *
 * 在远程接收器上设置特定于门户的输出参数。
 * 此例程复制发送行时使用的门户格式和协议状态。
 */
extern void SetRemoteDestReceiverParams(DestReceiver *self, Portal portal);

/* Build a RowDescription message for a target list.
 * The routine serializes tuple descriptor and requested per-column formats
 * into the supplied message buffer.
 *
 * 为目标列表构建 RowDescription 消息。
 * 此例程将元组描述符和请求的逐列格式序列化到给定消息缓冲区。
 */
extern void SendRowDescriptionMessage(StringInfo buf,
									  TupleDesc typeinfo, List *targetlist, int16 *formats);

/* Initialize the debugging tuple receiver.
 * The routine records output operation and descriptor state before debug rows
 * are printed.
 *
 * 初始化调试元组接收器。
 * 此例程在输出调试行前记录输出操作和描述符状态。
 */
extern void debugStartup(DestReceiver *self, int operation,
						 TupleDesc typeinfo);

/* Print one tuple through the debugging receiver.
 * The routine renders the slot for diagnostic output and reports success.
 *
 * 通过调试接收器输出一个元组。
 * 此例程为诊断输出呈现该槽，并报告是否成功。
 */
extern bool debugtup(TupleTableSlot *slot, DestReceiver *self);

/* XXX these are really in executor/spi.c */

/* XXX：这些函数实际上位于 executor/spi.c 中。 */

/* Initialize SPI's tuple destination receiver.
 * The routine records output state before SPI emits tuples.
 *
 * 初始化 SPI 的元组目标接收器。
 * 此例程在 SPI 输出元组前记录输出状态。
 */
extern void spi_dest_startup(DestReceiver *self, int operation,
							 TupleDesc typeinfo);

/* Send one tuple through SPI's destination receiver.
 * The routine formats the slot using SPI output conventions and reports
 * delivery success.
 *
 * 通过 SPI 的目标接收器发送一个元组。
 * 此例程按 SPI 输出约定格式化该槽，并报告发送是否成功。
 */
extern bool spi_printtup(TupleTableSlot *slot, DestReceiver *self);

#endif							/* PRINTTUP_H */
