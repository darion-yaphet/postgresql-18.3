/*-------------------------------------------------------------------------
 *
 * htup_details.h
 *	  POSTGRES heap tuple header definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup_details.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * htup_details.h POSTGRES 堆元组标头定义。 s
 * rc/include/access/htup_details.h
 */
#ifndef HTUP_DETAILS_H
#define HTUP_DETAILS_H

#include "access/htup.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "varatt.h"

/*
 * MaxTupleAttributeNumber limits the number of (user) columns in a tuple.
 * The key limit on this value is that the size of the fixed overhead for
 * a tuple, plus the size of the null-values bitmap (at 1 bit per column),
 * plus MAXALIGN alignment, must fit into t_hoff which is uint8.  On most
 * machines the upper limit without making t_hoff wider would be a little
 * over 1700.  We use round numbers here and for MaxHeapAttributeNumber
 * so that alterations in HeapTupleHeaderData layout won't change the
 * supported max number of columns.
 *
 * 中文翻译：
 * MaxTupleAttributeNumber 限制元组中（用户）列
 * 的数量。该值的关键限制是元组的固定开销的大小，加上空值位图的大小（每
 * 列 1 位），加上 MAXALIGN 对齐方式，必须适合 t_hof
 * f （uint8）。在大多数机器上，在不使 t_hoff 更宽的情况
 * 下，上限将略高于 1700。我们在此处和 MaxHeapAttrib
 * uteNumber 使用整数，以便 HeapTupleHeaderD
 * ata 布局中的更改不会更改支持的最大列数。
 */
#define MaxTupleAttributeNumber 1664	/* 8 * 208 */

/*
 * MaxHeapAttributeNumber limits the number of (user) columns in a table.
 * This should be somewhat less than MaxTupleAttributeNumber.  It must be
 * at least one less, else we will fail to do UPDATEs on a maximal-width
 * table (because UPDATE has to form working tuples that include CTID).
 * In practice we want some additional daylight so that we can gracefully
 * support operations that add hidden "resjunk" columns, for example
 * SELECT * FROM wide_table ORDER BY foo, bar, baz.
 * In any case, depending on column data types you will likely be running
 * into the disk-block-based limit on overall tuple size if you have more
 * than a thousand or so columns.  TOAST won't help.
 *
 * 中文翻译：
 * MaxHeapAttributeNumber 限制表中（用户）列的数
 * 量。这应该略小于 MaxTupleAttributeNumber。它
 * 必须至少少一，否则我们将无法对最大宽度表执行 UPDATE（因为 U
 * PDATE 必须形成包含 CTID 的工作元组）。在实践中，我们需要
 * 一些额外的日光，以便我们可以优雅地支持添加隐藏“resjunk”列的
 * 操作，例如 SELECT * FROM Wide_table ORD
 * ER BY foo, bar, baz。无论如何，根据列数据类型，如
 * 果您有超过一千个左右的列，您可能会遇到基于磁盘块的总体元组大小限制。
 *  TOAST 没有帮助。
 */
#define MaxHeapAttributeNumber	1600	/* 8 * 200 */

/*
 * Heap tuple header.  To avoid wasting space, the fields should be
 * laid out in such a way as to avoid structure padding.
 *
 * Datums of composite types (row types) share the same general structure
 * as on-disk tuples, so that the same routines can be used to build and
 * examine them.  However the requirements are slightly different: a Datum
 * does not need any transaction visibility information, and it does need
 * a length word and some embedded type information.  We can achieve this
 * by overlaying the xmin/cmin/xmax/cmax/xvac fields of a heap tuple
 * with the fields needed in the Datum case.  Typically, all tuples built
 * in-memory will be initialized with the Datum fields; but when a tuple is
 * about to be inserted in a table, the transaction fields will be filled,
 * overwriting the datum fields.
 *
 * The overall structure of a heap tuple looks like:
 *			fixed fields (HeapTupleHeaderData struct)
 *			nulls bitmap (if HEAP_HASNULL is set in t_infomask)
 *			alignment padding (as needed to make user data MAXALIGN'd)
 *			object ID (if HEAP_HASOID_OLD is set in t_infomask, not created
 *          anymore)
 *			user data fields
 *
 * We store five "virtual" fields Xmin, Cmin, Xmax, Cmax, and Xvac in three
 * physical fields.  Xmin and Xmax are always really stored, but Cmin, Cmax
 * and Xvac share a field.  This works because we know that Cmin and Cmax
 * are only interesting for the lifetime of the inserting and deleting
 * transaction respectively.  If a tuple is inserted and deleted in the same
 * transaction, we store a "combo" command id that can be mapped to the real
 * cmin and cmax, but only by use of local state within the originating
 * backend.  See combocid.c for more details.  Meanwhile, Xvac is only set by
 * old-style VACUUM FULL, which does not have any command sub-structure and so
 * does not need either Cmin or Cmax.  (This requires that old-style VACUUM
 * FULL never try to move a tuple whose Cmin or Cmax is still interesting,
 * ie, an insert-in-progress or delete-in-progress tuple.)
 *
 * A word about t_ctid: whenever a new tuple is stored on disk, its t_ctid
 * is initialized with its own TID (location).  If the tuple is ever updated,
 * its t_ctid is changed to point to the replacement version of the tuple.  Or
 * if the tuple is moved from one partition to another, due to an update of
 * the partition key, t_ctid is set to a special value to indicate that
 * (see ItemPointerSetMovedPartitions).  Thus, a tuple is the latest version
 * of its row iff XMAX is invalid or
 * t_ctid points to itself (in which case, if XMAX is valid, the tuple is
 * either locked or deleted).  One can follow the chain of t_ctid links
 * to find the newest version of the row, unless it was moved to a different
 * partition.  Beware however that VACUUM might
 * erase the pointed-to (newer) tuple before erasing the pointing (older)
 * tuple.  Hence, when following a t_ctid link, it is necessary to check
 * to see if the referenced slot is empty or contains an unrelated tuple.
 * Check that the referenced tuple has XMIN equal to the referencing tuple's
 * XMAX to verify that it is actually the descendant version and not an
 * unrelated tuple stored into a slot recently freed by VACUUM.  If either
 * check fails, one may assume that there is no live descendant version.
 *
 * t_ctid is sometimes used to store a speculative insertion token, instead
 * of a real TID.  A speculative token is set on a tuple that's being
 * inserted, until the inserter is sure that it wants to go ahead with the
 * insertion.  Hence a token should only be seen on a tuple with an XMAX
 * that's still in-progress, or invalid/aborted.  The token is replaced with
 * the tuple's real TID when the insertion is confirmed.  One should never
 * see a speculative insertion token while following a chain of t_ctid links,
 * because they are not used on updates, only insertions.
 *
 * Following the fixed header fields, the nulls bitmap is stored (beginning
 * at t_bits).  The bitmap is *not* stored if t_infomask shows that there
 * are no nulls in the tuple.  If an OID field is present (as indicated by
 * t_infomask), then it is stored just before the user data, which begins at
 * the offset shown by t_hoff.  Note that t_hoff must be a multiple of
 * MAXALIGN.
 *
 * 中文翻译：
 * 堆元组头。为了避免浪费空间，字段的布局方式应避免结构填充。复合类型（
 * 行类型）的数据与磁盘上的元组共享相同的通用结构，因此可以使用相同的例
 * 程来构建和检查它们。然而，要求略有不同：Datum 不需要任何事务可
 * 见性信息，它确实需要长度字和一些嵌入的类型信息。我们可以通过将堆元组
 * 的 xmin/cmin/xmax/cmax/xvac 字段与 Dat
 * um 情况下所需的字段叠加来实现此目的。通常，内存中构建的所有元组都
 * 将使用 Datum 字段进行初始化；但是当一个元组即将插入表中时，事
 * 务字段将被填充，覆盖数据字段。堆元组的整体结构如下所示： 固定字段（
 * HeapTupleHeaderData 结构） 空位图（如果在 t_
 * infomask 中设置了 HEAP_HASNULL） 对齐填充（根
 * 据需要使用户数据 MAXALIGN'd） 对象 ID（如果在 t_i
 * nfomask 中设置了 HEAP_HASOID_OLD，不再创建）
 *  用户数据字段 我们将五个“虚拟”字段 Xmin、Cmin、Xmax
 * 、Cmax 和 Xvac 存储在三个字段中物理场。 Xmin 和 X
 * max 始终被真正存储，但 Cmin、Cmax 和 Xvac 共享一
 * 个字段。这是有效的，因为我们知道 Cmin 和 Cmax 仅在插入和
 * 删除事务的生命周期中分别有意义。如果在同一事务中插入和删除一个元组，
 * 我们会存储一个“组合”命令 id，它可以映射到真实的 cmin 和
 * cmax，但只能使用原始后端中的本地状态。请参阅combocid.c
 *  了解更多详细信息。同时，Xvac仅由旧式VACUUM FULL设置
 * ，它没有任何命令子结构，因此不需要Cmin或Cmax。 （这要求旧式
 *  VACUUM FULL 永远不要尝试移动 Cmin 或 Cmax
 * 仍然感兴趣的元组，即正在插入或正在删除的元组。）关于 t_ctid
 * 的一句话：每当一个新元组存储在磁盘上时，它的 t_ctid 都会用它
 * 自己的 TID（位置）进行初始化。如果元组曾经更新过，则其 t_ct
 * id 会更改为指向元组的替换版本。或者，如果由于分区键的更新而将元组
 * 从一个分区移动到另一个分区，则 t_ctid 将设置为一个特殊值来指
 * 示这一情况（请参阅 ItemPointerSetMovedParti
 * tions）。因此，如果 XMAX 无效或 t_ctid 指向自身，
 * 则元组是其行的最新版本（在这种情况下，如果 XMAX 有效，则元组将
 * 被锁定或删除）。人们可以沿着 t_ctid 链接链找到该行的最新版本
 * ，除非它被移动到另一个分区。但请注意，VACUUM 可能会在删除指向
 * 的（较旧的）元组之前删除指向的（较新的）元组。因此，当跟踪 t_ct
 * id 链接时，有必要检查引用的槽是否为空或包含不相关的元组。检查引用
 * 元组的 XMIN 是否等于引用元组的 XMAX，以验证它实际上是后代
 * 版本，而不是存储在最近由 VACUUM 释放的槽中的不相关元组。如果
 * 任一检查失败，则可以假设不存在实时后代版本。 t_ctid 有时用于
 * 存储推测的插入令牌，而不是真正的 TID。在正在插入的元组上设置推测
 * 标记，直到插入器确定它想要继续插入为止。因此，令牌只能在具有仍在进行
 * 中或无效/中止的 XMAX 的元组上看到。当确认插入时，令牌将替换为
 * 元组的真实 TID。在跟踪 t_ctid 链接链时，永远不应该看到推
 * 测性插入标记，因为它们不用于更新，仅用于插入。在固定头字段之后，存储
 * 空位图（从 t_bits 开始）。如果 t_infomask 显示元
 * 组中没有空值，则不会存储位图。如果存在 OID 字段（如 t_inf
 * omask 所示），则它将存储在用户数据之前，该数据从 t_hoff
 *  显示的偏移量开始。请注意，t_hoff 必须是 MAXALIGN
 * 的倍数。
 */

typedef struct HeapTupleFields
{
	TransactionId t_xmin;		/* inserting xact ID */

	/* 中文翻译：插入 xact ID */
	TransactionId t_xmax;		/* deleting or locking xact ID */

	/* 中文翻译：删除或锁定 xact ID */

	union
	{
		CommandId	t_cid;		/* inserting or deleting command ID, or both */

		/* 中文翻译：插入或删除命令 ID，或两者 */
		TransactionId t_xvac;	/* old-style VACUUM FULL xact ID */

		/* 中文翻译：老式 VACUUM FULL xact ID */
	}			t_field3;
} HeapTupleFields;

typedef struct DatumTupleFields
{
	int32		datum_len_;		/* varlena header (do not touch directly!) */

	/* 中文翻译：varlena header（不要直接触摸！） */

	int32		datum_typmod;	/* -1, or identifier of a record type */

	/* 中文翻译：-1，或记录类型的标识符 */

	Oid			datum_typeid;	/* composite type OID, or RECORDOID */

	/* 中文翻译：复合类型 OID 或 RECORDOID */

	/*
	 * datum_typeid cannot be a domain over composite, only plain composite,
	 * even if the datum is meant as a value of a domain-over-composite type.
	 * This is in line with the general principle that CoerceToDomain does not
	 * change the physical representation of the base type value.
	 *
	 * Note: field ordering is chosen with thought that Oid might someday
	 * widen to 64 bits.
	 *
	 * 中文翻译：
	 * datum_typeid 不能是复合域，只能是普通复合域，即使数据是
	 * 复合域类型的值。这符合 CoerceToDomain 不改变基类型值
	 * 的物理表示的一般原则。注意：选择字段排序时考虑到 Oid 有一天可能
	 * 会扩展到 64 位。
	 */
} DatumTupleFields;

struct HeapTupleHeaderData
{
	union
	{
		HeapTupleFields t_heap;
		DatumTupleFields t_datum;
	}			t_choice;

	ItemPointerData t_ctid;		/* current TID of this or newer tuple (or a
								 * speculative insertion token) */

	/* 中文翻译：
	 * 该元组或更新元组的当前 TID（或推测插入标记）
	 */

	/* Fields below here must match MinimalTupleData! */

	/* 中文翻译：下面的字段必须与 MinimalTupleData 匹配！ */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2 2
	uint16		t_infomask2;	/* number of attributes + various flags */

	/* 中文翻译：属性数量+各种标志 */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK 3
	uint16		t_infomask;		/* various flag bits, see below */

	/* 中文翻译：各种标志位，见下文 */

#define FIELDNO_HEAPTUPLEHEADERDATA_HOFF 4
	uint8		t_hoff;			/* sizeof header incl. bitmap, padding */

	/* 中文翻译：包含标题的大小位图、填充 */

	/* ^ - 23 bytes - ^ */

	/* 中文翻译：^ - 23 字节 - ^ */

#define FIELDNO_HEAPTUPLEHEADERDATA_BITS 5
	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* bitmap of NULLs */

	/* 中文翻译：NULL 的位图 */

	/* MORE DATA FOLLOWS AT END OF STRUCT */

	/* 中文翻译：结构体末尾有更多数据 */
};

/* typedef appears in htup.h */

/* 中文翻译：typedef 出现在 htup.h 中 */

#define SizeofHeapTupleHeader offsetof(HeapTupleHeaderData, t_bits)

/*
 * information stored in t_infomask:
 *
 * 中文翻译：
 * t_infomask中存储的信息：
 */
#define HEAP_HASNULL			0x0001	/* has null attribute(s) */

/* 中文翻译：具有 null 属性 */
#define HEAP_HASVARWIDTH		0x0002	/* has variable-width attribute(s) */

/* 中文翻译：具有可变宽度属性 */
#define HEAP_HASEXTERNAL		0x0004	/* has external stored attribute(s) */

/* 中文翻译：有外部存储的属性 */
#define HEAP_HASOID_OLD			0x0008	/* has an object-id field */

/* 中文翻译：有一个 object-id 字段 */
#define HEAP_XMAX_KEYSHR_LOCK	0x0010	/* xmax is a key-shared locker */

/* 中文翻译：xmax是一个钥匙共享的储物柜 */
#define HEAP_COMBOCID			0x0020	/* t_cid is a combo CID */

/* 中文翻译：t_cid 是组合 CID */
#define HEAP_XMAX_EXCL_LOCK		0x0040	/* xmax is exclusive locker */

/* 中文翻译：xmax是专属储物柜 */
#define HEAP_XMAX_LOCK_ONLY		0x0080	/* xmax, if valid, is only a locker */

/* 中文翻译：xmax，如果有效，只是一个储物柜 */

 /* xmax is a shared locker */

 /* 中文翻译：xmax是一个共享储物柜 */
#define HEAP_XMAX_SHR_LOCK	(HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)

#define HEAP_LOCK_MASK	(HEAP_XMAX_SHR_LOCK | HEAP_XMAX_EXCL_LOCK | \
						 HEAP_XMAX_KEYSHR_LOCK)
#define HEAP_XMIN_COMMITTED		0x0100	/* t_xmin committed */

/* 中文翻译：t_xmin 已提交 */
#define HEAP_XMIN_INVALID		0x0200	/* t_xmin invalid/aborted */

/* 中文翻译：t_xmin 无效/中止 */
#define HEAP_XMIN_FROZEN		(HEAP_XMIN_COMMITTED|HEAP_XMIN_INVALID)
#define HEAP_XMAX_COMMITTED		0x0400	/* t_xmax committed */

/* 中文翻译：t_xmax 已提交 */
#define HEAP_XMAX_INVALID		0x0800	/* t_xmax invalid/aborted */

/* 中文翻译：t_xmax 无效/中止 */
#define HEAP_XMAX_IS_MULTI		0x1000	/* t_xmax is a MultiXactId */

/* 中文翻译：t_xmax 是一个 MultiXactId */
#define HEAP_UPDATED			0x2000	/* this is UPDATEd version of row */

/* 中文翻译：这是行的更新版本 */
#define HEAP_MOVED_OFF			0x4000	/* moved to another place by pre-9.0
										 * VACUUM FULL; kept for binary
										 * upgrade support */

/* 中文翻译：
 * 9.0 之前的 VACUUM FULL 移动到另一个地方；保留用于二
 * 进制升级支持
 */
#define HEAP_MOVED_IN			0x8000	/* moved from another place by pre-9.0
										 * VACUUM FULL; kept for binary
										 * upgrade support */

/* 中文翻译：
 * 9.0 之前的 VACUUM FULL 从另一个地方移动；保留用于二
 * 进制升级支持
 */
#define HEAP_MOVED (HEAP_MOVED_OFF | HEAP_MOVED_IN)

#define HEAP_XACT_MASK			0xFFF0	/* visibility-related bits */

/* 中文翻译：可见性相关位 */

/*
 * A tuple is only locked (i.e. not updated by its Xmax) if the
 * HEAP_XMAX_LOCK_ONLY bit is set; or, for pg_upgrade's sake, if the Xmax is
 * not a multi and the EXCL_LOCK bit is set.
 *
 * See also HeapTupleHeaderIsOnlyLocked, which also checks for a possible
 * aborted updater transaction.
 *
 * 中文翻译：
 * 如果设置了 HEAP_XMAX_LOCK_ONLY 位，则仅锁定元组
 * （即不由其 Xmax 更新）；或者，为了 pg_upgrade 的缘
 * 故，如果 Xmax 不是 multi 并且 EXCL_LOCK 位被
 * 设置。另请参阅 HeapTupleHeaderIsOnlyLocke
 * d，它还会检查可能中止的更新程序事务。
 */
/*
 * Function HEAP_XMAX_IS_LOCKED_ONLY updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HEAP_XMAX_IS_LOCKED_ONLY通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline bool
HEAP_XMAX_IS_LOCKED_ONLY(uint16 infomask)
{
	return (infomask & HEAP_XMAX_LOCK_ONLY) ||
		(infomask & (HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK)) == HEAP_XMAX_EXCL_LOCK;
}

/*
 * A tuple that has HEAP_XMAX_IS_MULTI and HEAP_XMAX_LOCK_ONLY but neither of
 * HEAP_XMAX_EXCL_LOCK and HEAP_XMAX_KEYSHR_LOCK must come from a tuple that was
 * share-locked in 9.2 or earlier and then pg_upgrade'd.
 *
 * In 9.2 and prior, HEAP_XMAX_IS_MULTI was only set when there were multiple
 * FOR SHARE lockers of that tuple.  That set HEAP_XMAX_LOCK_ONLY (with a
 * different name back then) but neither of HEAP_XMAX_EXCL_LOCK and
 * HEAP_XMAX_KEYSHR_LOCK.  That combination is no longer possible in 9.3 and
 * up, so if we see that combination we know for certain that the tuple was
 * locked in an earlier release; since all such lockers are gone (they cannot
 * survive through pg_upgrade), such tuples can safely be considered not
 * locked.
 *
 * We must not resolve such multixacts locally, because the result would be
 * bogus, regardless of where they stand with respect to the current valid
 * multixact range.
 *
 * 中文翻译：
 * 具有 HEAP_XMAX_IS_MULTI 和 HEAP_XMAX_
 * LOCK_ONLY 但没有 HEAP_XMAX_EXCL_LOCK
 * 和 HEAP_XMAX_KEYSHR_LOCK 的元组必须来自在 9
 * .2 或更早版本中共享锁定然后进行 pg_upgrade 的元组。在
 *  9.2 及更早版本中，仅当该元组有多个 FOR SHARE 储物柜
 * 时才设置 HEAP_XMAX_IS_MULTI。这设置了 HEAP_
 * XMAX_LOCK_ONLY （当时的名称不同），但没有设置 HEA
 * P_XMAX_EXCL_LOCK 和 HEAP_XMAX_KEYSH
 * R_LOCK。在 9.3 及更高版本中，该组合不再可能，因此，如果我
 * 们看到该组合，我们就可以确定该元组在早期版本中被锁定；由于所有此类储
 * 物柜都消失了（它们无法通过 pg_upgrade 生存），因此可以安
 * 全地认为此类元组未锁定。我们不能在本地解析此类多重行为，因为无论它们
 * 相对于当前有效的多重行为范围处于什么位置，结果都将是虚假的。
 */
/*
 * Function HEAP_LOCKED_UPGRADED updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HEAP_LOCKED_UPGRADED通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline bool
HEAP_LOCKED_UPGRADED(uint16 infomask)
{
	return
		(infomask & HEAP_XMAX_IS_MULTI) != 0 &&
		(infomask & HEAP_XMAX_LOCK_ONLY) != 0 &&
		(infomask & (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)) == 0;
}

/*
 * Use these to test whether a particular lock is applied to a tuple
 *
 * 中文翻译：
 * 使用它们来测试特定的锁是否应用于元组
 */
/*
 * Function HEAP_XMAX_IS_SHR_LOCKED updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HEAP_XMAX_IS_SHR_LOCKED通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline bool
HEAP_XMAX_IS_SHR_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_SHR_LOCK;
}

/*
 * Function HEAP_XMAX_IS_EXCL_LOCKED updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HEAP_XMAX_IS_EXCL_LOCKED通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline bool
HEAP_XMAX_IS_EXCL_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_EXCL_LOCK;
}

/*
 * Function HEAP_XMAX_IS_KEYSHR_LOCKED updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HEAP_XMAX_IS_KEYSHR_LOCKED通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline bool
HEAP_XMAX_IS_KEYSHR_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_KEYSHR_LOCK;
}

/* turn these all off when Xmax is to change */

/* 中文翻译：当 Xmax 改变时将这些全部关闭 */
#define HEAP_XMAX_BITS (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID | \
						HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK | HEAP_XMAX_LOCK_ONLY)

/*
 * information stored in t_infomask2:
 *
 * 中文翻译：
 * t_infomask2中存储的信息：
 */
#define HEAP_NATTS_MASK			0x07FF	/* 11 bits for number of attributes */

/* 中文翻译：11 位属性数 */
/* bits 0x1800 are available */

/* 中文翻译：位 0x1800 可用 */
#define HEAP_KEYS_UPDATED		0x2000	/* tuple was updated and key cols
										 * modified, or tuple deleted */

/* 中文翻译：
 * 元组已更新，键列已修改，或元组已删除
 */
#define HEAP_HOT_UPDATED		0x4000	/* tuple was HOT-updated */

/* 中文翻译：元组已热更新 */
#define HEAP_ONLY_TUPLE			0x8000	/* this is heap-only tuple */

/* 中文翻译：这是仅限堆的元组 */

#define HEAP2_XACT_MASK			0xE000	/* visibility-related bits */

/* 中文翻译：可见性相关位 */

/*
 * HEAP_TUPLE_HAS_MATCH is a temporary flag used during hash joins.  It is
 * only used in tuples that are in the hash table, and those don't need
 * any visibility information, so we can overlay it on a visibility flag
 * instead of using up a dedicated bit.
 *
 * 中文翻译：
 * HEAP_TUPLE_HAS_MATCH 是哈希连接期间使用的临时标
 * 志。它仅用于哈希表中的元组，并且这些元组不需要任何可见性信息，因此我
 * 们可以将其覆盖在可见性标志上，而不是使用专用位。
 */
#define HEAP_TUPLE_HAS_MATCH	HEAP_ONLY_TUPLE /* tuple has a join match */

/* 中文翻译：元组有连接匹配 */

/*
 * HeapTupleHeader accessor functions
 *
 * 中文翻译：
 * HeapTupleHeader 访问器函数
 */

static bool HeapTupleHeaderXminFrozen(const HeapTupleHeaderData *tup);

/*
 * HeapTupleHeaderGetRawXmin returns the "raw" xmin field, which is the xid
 * originally used to insert the tuple.  However, the tuple might actually
 * be frozen (via HeapTupleHeaderSetXminFrozen) in which case the tuple's xmin
 * is visible to every snapshot.  Prior to PostgreSQL 9.4, we actually changed
 * the xmin to FrozenTransactionId, and that value may still be encountered
 * on disk.
 *
 * 中文翻译：
 * HeapTupleHeaderGetRawXmin 返回“原始”xm
 * in 字段，它是最初用于插入元组的 xid。然而，元组实际上可能被冻
 * 结（通过 HeapTupleHeaderSetXminFrozen）
 * ，在这种情况下元组的 xmin 对每个快照都是可见的。在 Postg
 * reSQL 9.4 之前，我们实际上将 xmin 更改为 Froze
 * nTransactionId，并且在磁盘上仍然可能会遇到该值。
 */
/*
 * Function HeapTupleHeaderGetRawXmin retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetRawXmin通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline TransactionId
HeapTupleHeaderGetRawXmin(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_xmin;
}

/*
 * Function HeapTupleHeaderGetXmin retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetXmin通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline TransactionId
HeapTupleHeaderGetXmin(const HeapTupleHeaderData *tup)
{
	return HeapTupleHeaderXminFrozen(tup) ?
		FrozenTransactionId : HeapTupleHeaderGetRawXmin(tup);
}

/*
 * Function HeapTupleHeaderSetXmin updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetXmin通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetXmin(HeapTupleHeaderData *tup, TransactionId xid)
{
	tup->t_choice.t_heap.t_xmin = xid;
}

/*
 * Function HeapTupleHeaderXminCommitted carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapTupleHeaderXminCommitted通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline bool
HeapTupleHeaderXminCommitted(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & HEAP_XMIN_COMMITTED) != 0;
}

/*
 * Function HeapTupleHeaderXminInvalid evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHeaderXminInvalid通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHeaderXminInvalid(const HeapTupleHeaderData *tup) \
{
	return (tup->t_infomask & (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)) ==
		HEAP_XMIN_INVALID;
}

/*
 * Function HeapTupleHeaderXminFrozen carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapTupleHeaderXminFrozen通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline bool
HeapTupleHeaderXminFrozen(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & HEAP_XMIN_FROZEN) == HEAP_XMIN_FROZEN;
}

/*
 * Function HeapTupleHeaderSetXminCommitted updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetXminCommitted通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetXminCommitted(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminInvalid(tup));
	tup->t_infomask |= HEAP_XMIN_COMMITTED;
}

/*
 * Function HeapTupleHeaderSetXminInvalid updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetXminInvalid通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetXminInvalid(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminCommitted(tup));
	tup->t_infomask |= HEAP_XMIN_INVALID;
}

/*
 * Function HeapTupleHeaderSetXminFrozen updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetXminFrozen通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetXminFrozen(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminInvalid(tup));
	tup->t_infomask |= HEAP_XMIN_FROZEN;
}

/*
 * Function HeapTupleHeaderGetRawXmax retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetRawXmax通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline TransactionId
HeapTupleHeaderGetRawXmax(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_xmax;
}

/*
 * Function HeapTupleHeaderSetXmax updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetXmax通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetXmax(HeapTupleHeaderData *tup, TransactionId xid)
{
	tup->t_choice.t_heap.t_xmax = xid;
}

#ifndef FRONTEND
/*
 * HeapTupleHeaderGetRawXmax gets you the raw Xmax field.  To find out the Xid
 * that updated a tuple, you might need to resolve the MultiXactId if certain
 * bits are set.  HeapTupleHeaderGetUpdateXid checks those bits and takes care
 * to resolve the MultiXactId if necessary.  This might involve multixact I/O,
 * so it should only be used if absolutely necessary.
 *
 * 中文翻译：
 * HeapTupleHeaderGetRawXmax 为您提供原始 X
 * max 字段。要找出更新元组的 Xid，如果设置了某些位，您可能需要
 * 解析 MultiXactId。 HeapTupleHeaderGet
 * UpdateXid 检查这些位并在必要时注意解析 MultiXact
 * Id。这可能涉及 multixact I/O，因此仅应在绝对必要时使
 * 用它。
 */
/*
 * Function HeapTupleHeaderGetUpdateXid retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetUpdateXid通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline TransactionId
HeapTupleHeaderGetUpdateXid(const HeapTupleHeaderData *tup)
{
	if (!((tup)->t_infomask & HEAP_XMAX_INVALID) &&
		((tup)->t_infomask & HEAP_XMAX_IS_MULTI) &&
		!((tup)->t_infomask & HEAP_XMAX_LOCK_ONLY))
		return HeapTupleGetUpdateXid(tup);
	else
		return HeapTupleHeaderGetRawXmax(tup);
}
#endif							/* FRONTEND */

/* 中文翻译：前端 */

/*
 * HeapTupleHeaderGetRawCommandId will give you what's in the header whether
 * it is useful or not.  Most code should use HeapTupleHeaderGetCmin or
 * HeapTupleHeaderGetCmax instead, but note that those Assert that you can
 * get a legitimate result, ie you are in the originating transaction!
 *
 * 中文翻译：
 * HeapTupleHeaderGetRawCommandId 将为您
 * 提供标头中的内容，无论它是否有用。大多数代码应该使用 HeapTup
 * leHeaderGetCmin 或 HeapTupleHeaderG
 * etCmax 代替，但请注意，那些断言您可以获得合法结果，即您处于原
 * 始交易中！
 */
/*
 * Function HeapTupleHeaderGetRawCommandId retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetRawCommandId通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline CommandId
HeapTupleHeaderGetRawCommandId(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_field3.t_cid;
}

/* SetCmin is reasonably simple since we never need a combo CID */

/* 中文翻译：SetCmin 相当简单，因为我们从不需要组合 CID */
/*
 * Function HeapTupleHeaderSetCmin updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetCmin通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetCmin(HeapTupleHeaderData *tup, CommandId cid)
{
	Assert(!(tup->t_infomask & HEAP_MOVED));
	tup->t_choice.t_heap.t_field3.t_cid = cid;
	tup->t_infomask &= ~HEAP_COMBOCID;
}

/* SetCmax must be used after HeapTupleHeaderAdjustCmax; see combocid.c */

/* 中文翻译：SetCmax必须在HeapTupleHeaderAdjustCmax之后使用；参见combocid.c */
/*
 * Function HeapTupleHeaderSetCmax updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetCmax通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetCmax(HeapTupleHeaderData *tup, CommandId cid, bool iscombo)
{
	Assert(!((tup)->t_infomask & HEAP_MOVED));
	tup->t_choice.t_heap.t_field3.t_cid = cid;
	if (iscombo)
		tup->t_infomask |= HEAP_COMBOCID;
	else
		tup->t_infomask &= ~HEAP_COMBOCID;
}

/*
 * Function HeapTupleHeaderGetXvac retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetXvac通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline TransactionId
HeapTupleHeaderGetXvac(const HeapTupleHeaderData *tup)
{
	if (tup->t_infomask & HEAP_MOVED)
		return tup->t_choice.t_heap.t_field3.t_xvac;
	else
		return InvalidTransactionId;
}

/*
 * Function HeapTupleHeaderSetXvac updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetXvac通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetXvac(HeapTupleHeaderData *tup, TransactionId xid)
{
	Assert(tup->t_infomask & HEAP_MOVED);
	tup->t_choice.t_heap.t_field3.t_xvac = xid;
}

StaticAssertDecl(MaxOffsetNumber < SpecTokenOffsetNumber,
				 "invalid speculative token constant");

/*
 * Function HeapTupleHeaderIsSpeculative evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHeaderIsSpeculative通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHeaderIsSpeculative(const HeapTupleHeaderData *tup)
{
	return ItemPointerGetOffsetNumberNoCheck(&tup->t_ctid) == SpecTokenOffsetNumber;
}

/*
 * Function HeapTupleHeaderGetSpeculativeToken retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetSpeculativeToken通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline BlockNumber
HeapTupleHeaderGetSpeculativeToken(const HeapTupleHeaderData *tup)
{
	Assert(HeapTupleHeaderIsSpeculative(tup));
	return ItemPointerGetBlockNumber(&tup->t_ctid);
}

/*
 * Function HeapTupleHeaderSetSpeculativeToken updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetSpeculativeToken通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetSpeculativeToken(HeapTupleHeaderData *tup, BlockNumber token)
{
	ItemPointerSet(&tup->t_ctid, token, SpecTokenOffsetNumber);
}

/*
 * Function HeapTupleHeaderIndicatesMovedPartitions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapTupleHeaderIndicatesMovedPartitions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline bool
HeapTupleHeaderIndicatesMovedPartitions(const HeapTupleHeaderData *tup)
{
	return ItemPointerIndicatesMovedPartitions(&tup->t_ctid);
}

/*
 * Function HeapTupleHeaderSetMovedPartitions updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetMovedPartitions通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetMovedPartitions(HeapTupleHeaderData *tup)
{
	ItemPointerSetMovedPartitions(&tup->t_ctid);
}

/*
 * Function HeapTupleHeaderGetDatumLength retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetDatumLength通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline uint32
HeapTupleHeaderGetDatumLength(const HeapTupleHeaderData *tup)
{
	return VARSIZE(tup);
}

/*
 * Function HeapTupleHeaderSetDatumLength updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetDatumLength通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetDatumLength(HeapTupleHeaderData *tup, uint32 len)
{
	SET_VARSIZE(tup, len);
}

/*
 * Function HeapTupleHeaderGetTypeId retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetTypeId通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline Oid
HeapTupleHeaderGetTypeId(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_datum.datum_typeid;
}

/*
 * Function HeapTupleHeaderSetTypeId updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetTypeId通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetTypeId(HeapTupleHeaderData *tup, Oid datum_typeid)
{
	tup->t_choice.t_datum.datum_typeid = datum_typeid;
}

/*
 * Function HeapTupleHeaderGetTypMod retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 HeapTupleHeaderGetTypMod通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline int32
HeapTupleHeaderGetTypMod(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_datum.datum_typmod;
}

/*
 * Function HeapTupleHeaderSetTypMod updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetTypMod通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetTypMod(HeapTupleHeaderData *tup, int32 typmod)
{
	tup->t_choice.t_datum.datum_typmod = typmod;
}

/*
 * Note that we stop considering a tuple HOT-updated as soon as it is known
 * aborted or the would-be updating transaction is known aborted.  For best
 * efficiency, check tuple visibility before using this function, so that the
 * INVALID bits will be as up to date as possible.
 *
 * 中文翻译：
 * 请注意，一旦已知元组已中止或已知将要更新的事务已中止，我们就停止考虑
 * 元组热更新。为了获得最佳效率，请在使用此函数之前检查元组可见性，以便
 *  INVALID 位尽可能保持最新。
 */
/*
 * Function HeapTupleHeaderIsHotUpdated constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 HeapTupleHeaderIsHotUpdated通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
static inline bool
HeapTupleHeaderIsHotUpdated(const HeapTupleHeaderData *tup)
{
	return
		(tup->t_infomask2 & HEAP_HOT_UPDATED) != 0 &&
		(tup->t_infomask & HEAP_XMAX_INVALID) == 0 &&
		!HeapTupleHeaderXminInvalid(tup);
}

/*
 * Function HeapTupleHeaderSetHotUpdated updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetHotUpdated通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetHotUpdated(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 |= HEAP_HOT_UPDATED;
}

/*
 * Function HeapTupleHeaderClearHotUpdated updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderClearHotUpdated通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderClearHotUpdated(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 &= ~HEAP_HOT_UPDATED;
}

/*
 * Function HeapTupleHeaderIsHeapOnly evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHeaderIsHeapOnly通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHeaderIsHeapOnly(const HeapTupleHeaderData *tup) \
{
	return (tup->t_infomask2 & HEAP_ONLY_TUPLE) != 0;
}

/*
 * Function HeapTupleHeaderSetHeapOnly updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetHeapOnly通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetHeapOnly(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 |= HEAP_ONLY_TUPLE;
}

/*
 * Function HeapTupleHeaderClearHeapOnly updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderClearHeapOnly通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderClearHeapOnly(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 &= ~HEAP_ONLY_TUPLE;
}

/*
 * These are used with both HeapTuple and MinimalTuple, so they must be
 * macros.
 *
 * 中文翻译：
 * 它们与 HeapTuple 和 MinimalTuple 一起使用，
 * 因此它们必须是宏。
 */

#define HeapTupleHeaderGetNatts(tup) \
	((tup)->t_infomask2 & HEAP_NATTS_MASK)

#define HeapTupleHeaderSetNatts(tup, natts) \
( \
	(tup)->t_infomask2 = ((tup)->t_infomask2 & ~HEAP_NATTS_MASK) | (natts) \
)

#define HeapTupleHeaderHasExternal(tup) \
		(((tup)->t_infomask & HEAP_HASEXTERNAL) != 0)


/*
 * BITMAPLEN(NATTS) -
 *		Computes size of null bitmap given number of data columns.
 *
 * 中文翻译：
 * BITMAPLEN(NATTS) - 计算给定数据列数的空位图的大小
 * 。
 */
/*
 * Function BITMAPLEN converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 BITMAPLEN通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
static inline int
BITMAPLEN(int NATTS)
{
	return (NATTS + 7) / 8;
}

/*
 * MaxHeapTupleSize is the maximum allowed size of a heap tuple, including
 * header and MAXALIGN alignment padding.  Basically it's BLCKSZ minus the
 * other stuff that has to be on a disk page.  Since heap pages use no
 * "special space", there's no deduction for that.
 *
 * NOTE: we allow for the ItemId that must point to the tuple, ensuring that
 * an otherwise-empty page can indeed hold a tuple of this size.  Because
 * ItemIds and tuples have different alignment requirements, don't assume that
 * you can, say, fit 2 tuples of size MaxHeapTupleSize/2 on the same page.
 *
 * 中文翻译：
 * MaxHeapTupleSize 是堆元组允许的最大大小，包括标头和
 *  MAXALIGN 对齐填充。基本上它是 BLCKSZ 减去磁盘页面
 * 上必须存在的其他内容。由于堆页面不使用“特殊空间”，因此不会有任何扣
 * 除。注意：我们允许 ItemId 必须指向元组，以确保空页面确实可以
 * 容纳此大小的元组。由于 ItemId 和元组具有不同的对齐要求，因此
 * 不要假设您可以在同一页面上放置 2 个大小为 MaxHeapTupl
 * eSize/2 的元组。
 */
#define MaxHeapTupleSize  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)))
#define MinHeapTupleSize  MAXALIGN(SizeofHeapTupleHeader)

/*
 * MaxHeapTuplesPerPage is an upper bound on the number of tuples that can
 * fit on one heap page.  (Note that indexes could have more, because they
 * use a smaller tuple header.)  We arrive at the divisor because each tuple
 * must be maxaligned, and it must have an associated line pointer.
 *
 * Note: with HOT, there could theoretically be more line pointers (not actual
 * tuples) than this on a heap page.  However we constrain the number of line
 * pointers to this anyway, to avoid excessive line-pointer bloat and not
 * require increases in the size of work arrays.
 *
 * 中文翻译：
 * MaxHeapTuplesPerPage 是一个堆页上可以容纳的元组
 * 数量的上限。 （请注意，索引可以有更多，因为它们使用较小的元组标头。
 * ）我们得到除数是因为每个元组必须最大对齐，并且它必须有一个关联的行指
 * 针。注意：对于 HOT，理论上堆页上的行指针（不是实际的元组）可能比
 * 这个多。然而，我们无论如何都会限制行指针的数量，以避免过多的行指针膨
 * 胀，并且不需要增加工作数组的大小。
 */
#define MaxHeapTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))))

/*
 * MaxAttrSize is a somewhat arbitrary upper limit on the declared size of
 * data fields of char(n) and similar types.  It need not have anything
 * directly to do with the *actual* upper limit of varlena values, which
 * is currently 1Gb (see TOAST structures in postgres.h).  I've set it
 * at 10Mb which seems like a reasonable number --- tgl 8/6/00.
 *
 * 中文翻译：
 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
 */
#define MaxAttrSize		(10 * 1024 * 1024)


/*
 * MinimalTuple is an alternative representation that is used for transient
 * tuples inside the executor, in places where transaction status information
 * is not required, the tuple rowtype is known, and shaving off a few bytes
 * is worthwhile because we need to store many tuples.  The representation
 * is chosen so that tuple access routines can work with either full or
 * minimal tuples via a HeapTupleData pointer structure.  The access routines
 * see no difference, except that they must not access the transaction status
 * or t_ctid fields because those aren't there.
 *
 * For the most part, MinimalTuples should be accessed via TupleTableSlot
 * routines.  These routines will prevent access to the "system columns"
 * and thereby prevent accidental use of the nonexistent fields.
 *
 * MinimalTupleData contains a length word, some padding, and fields matching
 * HeapTupleHeaderData beginning with t_infomask2. The padding is chosen so
 * that offsetof(t_infomask2) is the same modulo MAXIMUM_ALIGNOF in both
 * structs.   This makes data alignment rules equivalent in both cases.
 *
 * When a minimal tuple is accessed via a HeapTupleData pointer, t_data is
 * set to point MINIMAL_TUPLE_OFFSET bytes before the actual start of the
 * minimal tuple --- that is, where a full tuple matching the minimal tuple's
 * data would start.  This trick is what makes the structs seem equivalent.
 *
 * Note that t_hoff is computed the same as in a full tuple, hence it includes
 * the MINIMAL_TUPLE_OFFSET distance.  t_len does not include that, however.
 *
 * MINIMAL_TUPLE_DATA_OFFSET is the offset to the first useful (non-pad) data
 * other than the length word.  tuplesort.c and tuplestore.c use this to avoid
 * writing the padding to disk.
 *
 * 中文翻译：
 * MinimalTuple 是一种替代表示形式，用于执行程序内的瞬态元
 * 组，在不需要事务状态信息、元组行类型已知的地方，并且削减一些字节是值
 * 得的，因为我们需要存储许多元组。选择表示形式，以便元组访问例程可以通
 * 过 HeapTupleData 指针结构处理完整元组或最小元组。访问
 * 例程没有看到任何区别，只是它们不能访问事务状态或 t_ctid 字段
 * ，因为这些字段不存在。在大多数情况下，MinimalTuple 应该
 * 通过 TupleTableSlot 例程访问。这些例程将防止访问“系
 * 统列”，从而防止意外使用不存在的字段。 MinimalTupleDa
 * ta 包含一个长度字、一些填充以及与以 t_infomask2 开头
 * 的 HeapTupleHeaderData 匹配的字段。选择填充以便
 * 两个结构中的 offsetof(t_infomask2) 与 MAX
 * IMUM_ALIGNOF 模相同。这使得数据对齐规则在两种情况下都是
 * 等效的。当通过HeapTupleData指针访问最小元组时，t_da
 * ta被设置为指向最小元组实际开始之前的MINIMAL_TUPLE_O
 * FFSET字节——也就是说，与最小元组的数据匹配的完整元组将开始。这
 * 个技巧使得结构看起来是等价的。请注意，t_hoff 的计算方式与完整
 * 元组中的计算方式相同，因此它包括 MINIMAL_TUPLE_OFF
 * SET 距离。然而 t_len 不包括这一点。 MINIMAL_TU
 * PLE_DATA_OFFSET 是除长度字之外的第一个有用（非填充）
 * 数据的偏移量。 tuplesort.c 和 tuplestore.c
 *  使用它来避免将填充写入磁盘。
 */
#define MINIMAL_TUPLE_OFFSET \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) / MAXIMUM_ALIGNOF * MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_PADDING \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) % MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_DATA_OFFSET \
	offsetof(MinimalTupleData, t_infomask2)

struct MinimalTupleData
{
	uint32		t_len;			/* actual length of minimal tuple */

	/* 中文翻译：最小元组的实际长度 */

	char		mt_padding[MINIMAL_TUPLE_PADDING];

	/* Fields below here must match HeapTupleHeaderData! */

	/* 中文翻译：这里下面的字段必须与 HeapTupleHeaderData 匹配！ */

	uint16		t_infomask2;	/* number of attributes + various flags */

	/* 中文翻译：属性数量+各种标志 */

	uint16		t_infomask;		/* various flag bits, see below */

	/* 中文翻译：各种标志位，见下文 */

	uint8		t_hoff;			/* sizeof header incl. bitmap, padding */

	/* 中文翻译：包含标题的大小位图、填充 */

	/* ^ - 23 bytes - ^ */

	/* 中文翻译：^ - 23 字节 - ^ */

	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* bitmap of NULLs */

	/* 中文翻译：NULL 的位图 */

	/* MORE DATA FOLLOWS AT END OF STRUCT */

	/* 中文翻译：结构体末尾有更多数据 */
};

/* typedef appears in htup.h */

/* 中文翻译：typedef 出现在 htup.h 中 */

#define SizeofMinimalTupleHeader offsetof(MinimalTupleData, t_bits)

/*
 * MinimalTuple accessor functions
 *
 * 中文翻译：
 * 本注释说明了相关声明、数据结构或访问流程的用途和约束。
 */

/*
 * Function HeapTupleHeaderHasMatch evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHeaderHasMatch通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHeaderHasMatch(const MinimalTupleData *tup)
{
	return (tup->t_infomask2 & HEAP_TUPLE_HAS_MATCH) != 0;
}

/*
 * Function HeapTupleHeaderSetMatch updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderSetMatch通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderSetMatch(MinimalTupleData *tup)
{
	tup->t_infomask2 |= HEAP_TUPLE_HAS_MATCH;
}

/*
 * Function HeapTupleHeaderClearMatch updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleHeaderClearMatch通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleHeaderClearMatch(MinimalTupleData *tup)
{
	tup->t_infomask2 &= ~HEAP_TUPLE_HAS_MATCH;
}


/*
 * GETSTRUCT - given a HeapTuple pointer, return address of the user data
 *
 * 中文翻译：
 * GETSTRUCT - 给定一个 HeapTuple 指针，返回用户
 * 数据的地址
 */
/*
 * Function GETSTRUCT retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 GETSTRUCT通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline void *
GETSTRUCT(const HeapTupleData *tuple)
{
	return ((char *) (tuple->t_data) + tuple->t_data->t_hoff);
}

/*
 * Accessor functions to be used with HeapTuple pointers.
 *
 * 中文翻译：
 * 与 HeapTuple 指针一起使用的访问器函数。
 */

/*
 * Function HeapTupleHasNulls evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHasNulls通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHasNulls(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASNULL) != 0;
}

/*
 * Function HeapTupleNoNulls carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapTupleNoNulls通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline bool
HeapTupleNoNulls(const HeapTupleData *tuple)
{
	return !HeapTupleHasNulls(tuple);
}

/*
 * Function HeapTupleHasVarWidth evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHasVarWidth通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHasVarWidth(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASVARWIDTH) != 0;
}

/*
 * Function HeapTupleAllFixed carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 HeapTupleAllFixed通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline bool
HeapTupleAllFixed(const HeapTupleData *tuple)
{
	return !HeapTupleHasVarWidth(tuple);
}

/*
 * Function HeapTupleHasExternal evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleHasExternal通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleHasExternal(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASEXTERNAL) != 0;
}

/*
 * Function HeapTupleIsHotUpdated constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 HeapTupleIsHotUpdated通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
static inline bool
HeapTupleIsHotUpdated(const HeapTupleData *tuple)
{
	return HeapTupleHeaderIsHotUpdated(tuple->t_data);
}

/*
 * Function HeapTupleSetHotUpdated updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleSetHotUpdated通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleSetHotUpdated(const HeapTupleData *tuple)
{
	HeapTupleHeaderSetHotUpdated(tuple->t_data);
}

/*
 * Function HeapTupleClearHotUpdated updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleClearHotUpdated通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleClearHotUpdated(const HeapTupleData *tuple)
{
	HeapTupleHeaderClearHotUpdated(tuple->t_data);
}

/*
 * Function HeapTupleIsHeapOnly evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 HeapTupleIsHeapOnly通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
HeapTupleIsHeapOnly(const HeapTupleData *tuple)
{
	return HeapTupleHeaderIsHeapOnly(tuple->t_data);
}

/*
 * Function HeapTupleSetHeapOnly updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleSetHeapOnly通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleSetHeapOnly(const HeapTupleData *tuple)
{
	HeapTupleHeaderSetHeapOnly(tuple->t_data);
}

/*
 * Function HeapTupleClearHeapOnly updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 HeapTupleClearHeapOnly通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
static inline void
HeapTupleClearHeapOnly(const HeapTupleData *tuple)
{
	HeapTupleHeaderClearHeapOnly(tuple->t_data);
}

/* prototypes for functions in common/heaptuple.c */

/* 中文翻译：common/heaptuple.c 中函数的原型 */
/*
 * Function heap_compute_data_size constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_compute_data_size通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern Size heap_compute_data_size(TupleDesc tupleDesc,
								   const Datum *values, const bool *isnull);
/*
 * Function heap_fill_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_fill_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void heap_fill_tuple(TupleDesc tupleDesc,
							const Datum *values, const bool *isnull,
							char *data, Size data_size,
							uint16 *infomask, bits8 *bit);
/*
 * Function heap_attisnull evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 heap_attisnull通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern bool heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc);
/*
 * Function nocachegetattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 nocachegetattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Datum nocachegetattr(HeapTuple tup, int attnum,
							TupleDesc tupleDesc);
/*
 * Function heap_getsysattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_getsysattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Datum heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
							 bool *isnull);
/*
 * Function getmissingattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 getmissingattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Datum getmissingattr(TupleDesc tupleDesc,
							int attnum, bool *isnull);
/*
 * Function heap_copytuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 heap_copytuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern HeapTuple heap_copytuple(HeapTuple tuple);
/*
 * Function heap_copytuple_with_tuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 heap_copytuple_with_tuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern void heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest);
/*
 * Function heap_copy_tuple_as_datum converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 heap_copy_tuple_as_datum通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern Datum heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc);
/*
 * Function heap_form_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_form_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern HeapTuple heap_form_tuple(TupleDesc tupleDescriptor,
								 const Datum *values, const bool *isnull);
/*
 * Function heap_modify_tuple carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_modify_tuple通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern HeapTuple heap_modify_tuple(HeapTuple tuple,
								   TupleDesc tupleDesc,
								   const Datum *replValues,
								   const bool *replIsnull,
								   const bool *doReplace);
/*
 * Function heap_modify_tuple_by_cols carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_modify_tuple_by_cols通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern HeapTuple heap_modify_tuple_by_cols(HeapTuple tuple,
										   TupleDesc tupleDesc,
										   int nCols,
										   const int *replCols,
										   const Datum *replValues,
										   const bool *replIsnull);
/*
 * Function heap_deform_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_deform_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
							  Datum *values, bool *isnull);
/*
 * Function heap_freetuple completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_freetuple在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_freetuple(HeapTuple htup);
/*
 * Function heap_form_minimal_tuple constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 heap_form_minimal_tuple通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern MinimalTuple heap_form_minimal_tuple(TupleDesc tupleDescriptor,
											const Datum *values, const bool *isnull,
											Size extra);
/*
 * Function heap_free_minimal_tuple completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 heap_free_minimal_tuple在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void heap_free_minimal_tuple(MinimalTuple mtup);
/*
 * Function heap_copy_minimal_tuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 heap_copy_minimal_tuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern MinimalTuple heap_copy_minimal_tuple(MinimalTuple mtup, Size extra);
/*
 * Function heap_tuple_from_minimal_tuple carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_tuple_from_minimal_tuple通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern HeapTuple heap_tuple_from_minimal_tuple(MinimalTuple mtup);
/*
 * Function minimal_tuple_from_heap_tuple carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 minimal_tuple_from_heap_tuple通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern MinimalTuple minimal_tuple_from_heap_tuple(HeapTuple htup, Size extra);
/*
 * Function varsize_any carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 varsize_any通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern size_t varsize_any(void *p);
/*
 * Function heap_expand_tuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 heap_expand_tuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern HeapTuple heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);
/*
 * Function minimal_expand_tuple converts or copies access-layer data by mapping the supplied representation through the required metadata and returning the transformed result.
 *
 * 函数 minimal_expand_tuple通过依据所需元数据映射传入表示来转换或复制访问层数据，并返回转换后的结果。
 */
extern MinimalTuple minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);

#ifndef FRONTEND
/*
 *	fastgetattr
 *		Fetch a user attribute's value as a Datum (might be either a
 *		value, or a pointer into the data area of the tuple).
 *
 *		This must not be used when a system attribute might be requested.
 *		Furthermore, the passed attnum MUST be valid.  Use heap_getattr()
 *		instead, if in doubt.
 *
 *		This gets called many times, so we macro the cacheable and NULL
 *		lookups, and call nocachegetattr() for the rest.
 *
 * 中文翻译：
 * fastgetattr 获取用户属性的值作为数据（可能是一个值，也可
 * 能是指向元组数据区域的指针）。当可能请求系统属性时不得使用此属性。此
 * 外，传递的 attnum 必须有效。如果有疑问，请改用 heap_g
 * etattr()。这会被调用很多次，因此我们宏化可缓存和 NULL
 * 查找，并调用 nocachegetattr() 来完成其余的操作。
 */
/*
 * Function fastgetattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 fastgetattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline Datum
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(attnum > 0);

	*isnull = false;
	if (HeapTupleNoNulls(tup))
	{
		CompactAttribute *att;

		att = TupleDescCompactAttr(tupleDesc, attnum - 1);
		if (att->attcacheoff >= 0)
			return fetchatt(att, (char *) tup->t_data + tup->t_data->t_hoff +
							att->attcacheoff);
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, tup->t_data->t_bits))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
}

/*
 *	heap_getattr
 *		Extract an attribute of a heap tuple and return it as a Datum.
 *		This works for either system or user attributes.  The given attnum
 *		is properly range-checked.
 *
 *		If the field in question has a NULL value, we return a zero Datum
 *		and set *isnull == true.  Otherwise, we set *isnull == false.
 *
 *		<tup> is the pointer to the heap tuple.  <attnum> is the attribute
 *		number of the column (field) caller wants.  <tupleDesc> is a
 *		pointer to the structure describing the row and all its fields.
 *
 *
 * 中文翻译：
 * heap_getattr 提取堆元组的属性并将其作为 Datum 返
 * 回。这适用于系统或用户属性。给定的 attnum 已正确进行范围检查
 * 。如果相关字段具有 NULL 值，我们将返回零 Datum 并设置
 * *isnull == true。否则，我们设置 *isnull ==
 *  false。 <tup> 是指向堆元组的指针。 <attnum>是
 * 调用者想要的列（字段）的属性号。 <tupleDesc> 是指向描述
 * 行及其所有字段的结构的指针。
 */
/*
 * Function heap_getattr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 heap_getattr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline Datum
heap_getattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	if (attnum > 0)
	{
		if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
			return getmissingattr(tupleDesc, attnum, isnull);
		else
			return fastgetattr(tup, attnum, tupleDesc, isnull);
	}
	else
		return heap_getsysattr(tup, attnum, tupleDesc, isnull);
}
#endif							/* FRONTEND */

/* 中文翻译：前端 */

#endif							/* HTUP_DETAILS_H */

/* 中文翻译：HTUP_DETAILS_H */
