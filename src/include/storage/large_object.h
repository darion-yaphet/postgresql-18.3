/*-------------------------------------------------------------------------
 *
 * large_object.h
 *	  Declarations for PostgreSQL large objects.  POSTGRES 4.2 supported
 *	  zillions of large objects (internal, external, jaquith, inversion).
 *	  Now we only support inversion.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/large_object.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * PostgreSQL 大对象声明。POSTGRES 4.2 曾支持大量大对象类型（内部、外部、jaquith、
 * inversion）；现在只支持 inversion。
 */
#ifndef LARGE_OBJECT_H
#define LARGE_OBJECT_H

#include "utils/snapshot.h"


/*----------
 * Data about a currently-open large object.
 *
 * id is the logical OID of the large object
 * snapshot is the snapshot to use for read/write operations
 * subid is the subtransaction that opened the desc (or currently owns it)
 * offset is the current seek offset within the LO
 * flags contains some flag bits
 *
 * NOTE: as of v11, permission checks are made when the large object is
 * opened; therefore IFS_RDLOCK/IFS_WRLOCK indicate that read or write mode
 * has been requested *and* the corresponding permission has been checked.
 *
 * NOTE: before 7.1, we also had to store references to the separate table
 * and index of a specific large object.  Now they all live in pg_largeobject
 * and are accessed via a common relation descriptor.
 *----------
 */

/*
 * 当前打开的大对象数据。
 * id 是大对象的逻辑 OID；snapshot 是读写操作使用的快照；subid 是打开描述符（或当前拥有它）的
 * 子事务；offset 是 LO 中当前查找偏移量；flags 包含若干标志位。
 * 注意：自 v11 起，打开大对象时会执行权限检查；因此 IFS_RDLOCK/IFS_WRLOCK 表示已请求读或
 * 写模式，并且相应权限已经过检查。
 * 注意：在 7.1 之前，还需保存特定大对象的独立表和索引引用。现在它们都位于 pg_largeobject 中，
 * 并通过通用关系描述符访问。
 */
typedef struct LargeObjectDesc
{
	Oid			id;				/* LO's identifier */

/*
 * LO 的标识符。
 */
	Snapshot	snapshot;		/* snapshot to use */

/*
 * 要使用的快照。
 */
	SubTransactionId subid;		/* owning subtransaction ID */

/*
 * 所属子事务 ID。
 */
	uint64		offset;			/* current seek pointer */

/*
 * 当前查找指针。
 */
	int			flags;			/* see flag bits below */

/*
 * 见下方标志位。
 */

/* bits in flags: */

/*
 * flags 中的位：
 */
#define IFS_RDLOCK		(1 << 0)	/* LO was opened for reading */

/*
 * LO 已为读取而打开。
 */
#define IFS_WRLOCK		(1 << 1)	/* LO was opened for writing */

/*
 * LO 已为写入而打开。
 */

} LargeObjectDesc;


/*
 * Each "page" (tuple) of a large object can hold this much data
 *
 * We could set this as high as BLCKSZ less some overhead, but it seems
 * better to make it a smaller value, so that not as much space is used
 * up when a page-tuple is updated.  Note that the value is deliberately
 * chosen large enough to trigger the tuple toaster, so that we will
 * attempt to compress page tuples in-line.  (But they won't be moved off
 * unless the user creates a toast-table for pg_largeobject...)
 *
 * Also, it seems to be a smart move to make the page size be a power of 2,
 * since clients will often be written to send data in power-of-2 blocks.
 * This avoids unnecessary tuple updates caused by partial-page writes.
 *
 * NB: Changing LOBLKSIZE requires an initdb.
 */

/*
 * 大对象的每个“页面”（元组）可容纳这么多数据。
 * 可以将其设为 BLCKSZ 减去一些开销的值，但较小的值更好，因为更新页面元组时不会占用太多空间。
 * 该值被特意设得足够大以触发元组 TOAST，从而尝试就地压缩页面元组（除非用户为 pg_largeobject
 * 创建 toast 表，否则不会将其移出行外）。
 * 将页面大小设为 2 的幂也很合适，因为客户端通常会按 2 的幂大小发送数据块，从而避免部分页面
 * 写入导致的不必要元组更新。
 * 注意：更改 LOBLKSIZE 需要执行 initdb。
 */
#define LOBLKSIZE		(BLCKSZ / 4)

/*
 * Maximum length in bytes for a large object.  To make this larger, we'd
 * have to widen pg_largeobject.pageno as well as various internal variables.
 */

/*
 * 大对象的最大字节长度。若要增大此值，必须同时加宽 pg_largeobject.pageno 及多个内部变量。
 */
#define MAX_LARGE_OBJECT_SIZE	((int64) INT_MAX * LOBLKSIZE)


/*
 * GUC: backwards-compatibility flag to suppress LO permission checks
 */

/*
 * GUC：用于抑制 LO 权限检查的向后兼容标志。
 */
extern PGDLLIMPORT bool lo_compat_privileges;

/*
 * Function definitions...
 */

/*
 * 函数定义。
 */

/* inversion stuff in inv_api.c */

/*
 * inv_api.c 中的 inversion 函数。
 */
/*
 * Closes the large-object relation at transaction end when appropriate.
 */

/*
 * 在适当时于事务结束阶段关闭大对象关系。
 */
extern void close_lo_relation(bool isCommit);
/*
 * Creates a large object, using the supplied OID when permitted.
 */

/*
 * 创建大对象，并在允许时使用提供的 OID。
 */
extern Oid	inv_create(Oid lobjId);
/*
 * Opens a large object with the requested flags and memory context.
 */

/*
 * 使用请求的标志和内存上下文打开大对象。
 */
extern LargeObjectDesc *inv_open(Oid lobjId, int flags, MemoryContext mcxt);
/*
 * Closes an open large-object descriptor.
 */

/*
 * 关闭打开的大对象描述符。
 */
extern void inv_close(LargeObjectDesc *obj_desc);
/*
 * Removes a large object and its stored pages.
 */

/*
 * 删除大对象及其存储页面。
 */
extern int	inv_drop(Oid lobjId);
/*
 * Moves the current position in an open large object and returns the result.
 */

/*
 * 移动打开的大对象中的当前位置，并返回结果位置。
 */
extern int64 inv_seek(LargeObjectDesc *obj_desc, int64 offset, int whence);
/*
 * Returns the current position in an open large object.
 */

/*
 * 返回打开的大对象中的当前位置。
 */
extern int64 inv_tell(LargeObjectDesc *obj_desc);
/*
 * Reads bytes from an open large object into a caller buffer.
 */

/*
 * 从打开的大对象读取字节到调用方缓冲区。
 */
extern int	inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes);
/*
 * Writes caller bytes to an open large object.
 */

/*
 * 将调用方字节写入打开的大对象。
 */
extern int	inv_write(LargeObjectDesc *obj_desc, const char *buf, int nbytes);
/*
 * Changes an open large object's length to the requested value.
 */

/*
 * 将打开的大对象长度改为请求的值。
 */
extern void inv_truncate(LargeObjectDesc *obj_desc, int64 len);

#endif							/* LARGE_OBJECT_H */
