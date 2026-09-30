/*-------------------------------------------------------------------------
 *
 * buf.h
 *	  Basic buffer manager data types.
 *
 *	  缓冲区管理器的基本数据类型。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_H
#define BUF_H

/*
 * Buffer identifiers.
 *
 * 缓冲区标识符。
 *
 * Zero is invalid, positive is the index of a shared buffer (1..NBuffers),
 * negative is the index of a local buffer (-1 .. -NLocBuffer).
 *
 * 零表示无效值，正数是共享缓冲区的索引（1..NBuffers），
 * 负数是本地缓冲区的索引（-1 .. -NLocBuffer）。
 */
typedef int Buffer;

#define InvalidBuffer	0

/*
 * BufferIsInvalid
 *		True iff the buffer is invalid.
 *
 *		仅当缓冲区无效时返回 true。
 */
#define BufferIsInvalid(buffer) ((buffer) == InvalidBuffer)

/*
 * BufferIsLocal
 *		True iff the buffer is local (not visible to other backends).
 *
 *		仅当缓冲区为本地缓冲区（其他后端不可见）时返回 true。
 */
#define BufferIsLocal(buffer)	((buffer) < 0)

/*
 * Buffer access strategy objects.
 *
 * 缓冲区访问策略对象。
 *
 * BufferAccessStrategyData is private to freelist.c
 *
 * BufferAccessStrategyData 是 freelist.c 的私有数据。
 */
typedef struct BufferAccessStrategyData *BufferAccessStrategy;

#endif							/* BUF_H */
