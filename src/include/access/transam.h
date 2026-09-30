/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres transaction access method support code
 *
 * postgres 事务访问方法支持代码。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/transam.h
 *
 * postgres 事务访问方法支持代码。
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

#include "access/xlogdefs.h"


/* ----------------
 *		Special transaction ID values
 *
 * 特殊事务 ID 值的定义及其有效性约束。
 *
 * BootstrapTransactionId is the XID for "bootstrap" operations, and
 * FrozenTransactionId is used for very old tuples.  Both should
 * always be considered valid.
 *
 * BootstrapTransactionId 是“引导”操作使用的 XID，FrozenTransactionId 用于非常旧的元组。两者都应始终被视为有效。
 *
 * FirstNormalTransactionId is the first "normal" transaction id.
 * Note: if you need to change it, you must change pg_class.h as well.
 *
 * FirstNormalTransactionId 是第一个“普通”事务 ID。注意：如果需要修改它，也必须一并修改 pg_class.h。
 * ----------------
 */
#define InvalidTransactionId		((TransactionId) 0)
#define BootstrapTransactionId		((TransactionId) 1)
#define FrozenTransactionId			((TransactionId) 2)
#define FirstNormalTransactionId	((TransactionId) 3)
#define MaxTransactionId			((TransactionId) 0xFFFFFFFF)

/* ----------------
 *		transaction ID manipulation macros
 *
 * 事务 ID 操作宏。
 * ----------------
 */
#define TransactionIdIsValid(xid)		((xid) != InvalidTransactionId)
#define TransactionIdIsNormal(xid)		((xid) >= FirstNormalTransactionId)
#define TransactionIdEquals(id1, id2)	((id1) == (id2))
#define TransactionIdStore(xid, dest)	(*(dest) = (xid))
#define StoreInvalidTransactionId(dest) (*(dest) = InvalidTransactionId)

#define EpochFromFullTransactionId(x)	((uint32) ((x).value >> 32))
#define XidFromFullTransactionId(x)		((uint32) (x).value)
#define U64FromFullTransactionId(x)		((x).value)
#define FullTransactionIdEquals(a, b)	((a).value == (b).value)
#define FullTransactionIdPrecedes(a, b)	((a).value < (b).value)
#define FullTransactionIdPrecedesOrEquals(a, b) ((a).value <= (b).value)
#define FullTransactionIdFollows(a, b) ((a).value > (b).value)
#define FullTransactionIdFollowsOrEquals(a, b) ((a).value >= (b).value)
#define FullTransactionIdIsValid(x)		TransactionIdIsValid(XidFromFullTransactionId(x))
#define InvalidFullTransactionId		FullTransactionIdFromEpochAndXid(0, InvalidTransactionId)
#define FirstNormalFullTransactionId	FullTransactionIdFromEpochAndXid(0, FirstNormalTransactionId)
#define FullTransactionIdIsNormal(x)	FullTransactionIdFollowsOrEquals(x, FirstNormalFullTransactionId)

/*
 * A 64 bit value that contains an epoch and a TransactionId.  This is
 * wrapped in a struct to prevent implicit conversion to/from TransactionId.
 * Not all values represent valid normal XIDs.
 *
 * 一个包含纪元（epoch）和 TransactionId 的 64 位值。它被封装在结构体中，以防止与 TransactionId 之间的隐式转换。并非所有取值都表示有效的普通 XID。
 */
typedef struct FullTransactionId
{
	uint64		value;
} FullTransactionId;

/*
 * Function: FullTransactionIdFromEpochAndXid.
 * Purpose: Performs the operation represented by full transaction id from epoch and xid.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：FullTransactionIdFromEpochAndXid。
 * 作用：执行 full transaction id from epoch and xid 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline FullTransactionId
FullTransactionIdFromEpochAndXid(uint32 epoch, TransactionId xid)
{
	FullTransactionId result;

	result.value = ((uint64) epoch) << 32 | xid;

	return result;
}

/*
 * Function: FullTransactionIdFromU64.
 * Purpose: Performs the operation represented by full transaction id from u64.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：FullTransactionIdFromU64。
 * 作用：执行 full transaction id from u64 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline FullTransactionId
FullTransactionIdFromU64(uint64 value)
{
	FullTransactionId result;

	result.value = value;

	return result;
}

/* advance a transaction ID variable, handling wraparound correctly */

/* 推进事务 ID 变量，并正确处理回绕。 */
#define TransactionIdAdvance(dest)	\
	do { \
		(dest)++; \
		if ((dest) < FirstNormalTransactionId) \
			(dest) = FirstNormalTransactionId; \
	} while(0)

/*
 * Retreat a FullTransactionId variable, stepping over xids that would appear
 * to be special only when viewed as 32bit XIDs.
 *
 * 说明完整事务 ID 的表示、比较或跨纪元换算规则。
 */
/*
 * Function: FullTransactionIdRetreat.
 * Purpose: Performs the operation represented by full transaction id retreat.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：FullTransactionIdRetreat。
 * 作用：执行 full transaction id retreat 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline void
FullTransactionIdRetreat(FullTransactionId *dest)
{
	dest->value--;

	/*
	 * In contrast to 32bit XIDs don't step over the "actual" special xids.
	 * For 64bit xids these can't be reached as part of a wraparound as they
	 * can in the 32bit case.
 *
 * 与 32 位 XID 不同，不要跨越“真正的”特殊 XID。对于 64 位 XID，这些值不会像 32 位情形那样在回绕过程中被触及。
	 */
	if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
		return;

	/*
	 * But we do need to step over XIDs that'd appear special only for 32bit
	 * XIDs.
 *
 * 但我们确实需要跨越那些仅在作为 32 位 XID 查看时才显得特殊的 XID。
	 */
	while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
		dest->value--;
}

/*
 * Advance a FullTransactionId variable, stepping over xids that would appear
 * to be special only when viewed as 32bit XIDs.
 *
 * 说明完整事务 ID 的表示、比较或跨纪元换算规则。
 */
/*
 * Function: FullTransactionIdAdvance.
 * Purpose: Performs the operation represented by full transaction id advance.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：FullTransactionIdAdvance。
 * 作用：执行 full transaction id advance 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline void
FullTransactionIdAdvance(FullTransactionId *dest)
{
	dest->value++;

	/* see FullTransactionIdAdvance() */

	/* 参见 FullTransactionIdAdvance()。 */
	if (FullTransactionIdPrecedes(*dest, FirstNormalFullTransactionId))
		return;

	while (XidFromFullTransactionId(*dest) < FirstNormalTransactionId)
		dest->value++;
}

/* back up a transaction ID variable, handling wraparound correctly */

/* 回退事务 ID 变量，并正确处理回绕。 */
#define TransactionIdRetreat(dest)	\
	do { \
		(dest)--; \
	} while ((dest) < FirstNormalTransactionId)

/* compare two XIDs already known to be normal; this is a macro for speed */

/* 比较两个已知为普通事务 ID 的 XID；为提高速度使用宏。 */
#define NormalTransactionIdPrecedes(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) < 0)

/* compare two XIDs already known to be normal; this is a macro for speed */

/* 比较两个已知为普通事务 ID 的 XID；为提高速度使用宏。 */
#define NormalTransactionIdFollows(id1, id2) \
	(AssertMacro(TransactionIdIsNormal(id1) && TransactionIdIsNormal(id2)), \
	(int32) ((id1) - (id2)) > 0)

/* ----------
 *		Object ID (OID) zero is InvalidOid.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 *
 *		OIDs 1-9999 are reserved for manual assignment (see .dat files in
 *		src/include/catalog/).  Of these, 8000-9999 are reserved for
 *		development purposes (such as in-progress patches and forks);
 *		they should not appear in released versions.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 *
 *		OIDs 10000-11999 are reserved for assignment by genbki.pl, for use
 *		when the .dat files in src/include/catalog/ do not specify an OID
 *		for a catalog entry that requires one.  Note that genbki.pl assigns
 *		these OIDs independently in each catalog, so they're not guaranteed
 *		to be globally unique.  Furthermore, the bootstrap backend and
 *		initdb's post-bootstrap processing can also assign OIDs in this range.
 *		The normal OID-generation logic takes care of any OID conflicts that
 *		might arise from that.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 *
 *		OIDs 12000-16383 are reserved for unpinned objects created by initdb's
 *		post-bootstrap processing.  initdb forces the OID generator up to
 *		12000 as soon as it's made the pinned objects it's responsible for.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 *
 *		OIDs beginning at 16384 are assigned from the OID generator
 *		during normal multiuser operation.  (We force the generator up to
 *		16384 as soon as we are in normal operation.)
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 *
 * The choices of 8000, 10000 and 12000 are completely arbitrary, and can be
 * moved if we run low on OIDs in any category.  Changing the macros below,
 * and updating relevant documentation (see bki.sgml and RELEASE_CHANGES),
 * should be sufficient to do this.  Moving the 16384 boundary between
 * initdb-assigned OIDs and user-defined objects would be substantially
 * more painful, however, since some user-defined OIDs will appear in
 * on-disk data; such a change would probably break pg_upgrade.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 *
 * NOTE: if the OID generator wraps around, we skip over OIDs 0-16383
 * and resume with 16384.  This minimizes the odds of OID conflict, by not
 * reassigning OIDs that might have been assigned during initdb.  Critically,
 * it also ensures that no user-created object will be considered pinned.
 *
 * 说明 OID 的保留区间、分配规则和回绕后的处理方式。
 * ----------
 */
#define FirstGenbkiObjectId		10000
#define FirstUnpinnedObjectId	12000
#define FirstNormalObjectId		16384

/*
 * TransamVariables is a data structure in shared memory that is used to track
 * OID and XID assignment state.  For largely historical reasons, there is
 * just one struct with different fields that are protected by different
 * LWLocks.
 *
 * TransamVariables 位于共享内存中，用于跟踪 OID 与 XID 的分配状态；不同字段由不同的 LWLock 保护。
 *
 * Note: xidWrapLimit and oldestXidDB are not "active" values, but are
 * used just to generate useful messages when xidWarnLimit or xidStopLimit
 * are exceeded.
 *
 * 注意：xidWrapLimit 与 oldestXidDB 并非“活动”值，仅用于在超过 xidWarnLimit 或 xidStopLimit 时生成有用的提示信息。
 */
typedef struct TransamVariablesData
{
	/*
	 * These fields are protected by OidGenLock.
 *
 * 这些字段由 OidGenLock 保护。
	 */
	Oid			nextOid;		/* next OID to assign */

	/* 下一个要处理或分配的值。 */
	uint32		oidCount;		/* OIDs available before must do XLOG work */

	/* 说明 OID 的保留区间、分配规则和回绕后的处理方式。 */

	/*
	 * These fields are protected by XidGenLock.
 *
 * 这些字段由 XidGenLock 保护。
	 */
	FullTransactionId nextXid;	/* next XID to assign */

	/* 下一个要处理或分配的值。 */

	TransactionId oldestXid;	/* cluster-wide minimum datfrozenxid */

	/* 集群范围内最小的 datfrozenxid。 */
	TransactionId xidVacLimit;	/* start forcing autovacuums here */

	/* 从此处开始强制触发 autovacuum。 */
	TransactionId xidWarnLimit; /* start complaining here */

	/* 从此处开始发出告警。 */
	TransactionId xidStopLimit; /* refuse to advance nextXid beyond here */

	/* 拒绝将 nextXid 推进到此处之后。 */
	TransactionId xidWrapLimit; /* where the world ends */

	/* 世界终结之处（回绕上限）。 */
	Oid			oldestXidDB;	/* database with minimum datfrozenxid */

	/* datfrozenxid 最小的数据库。 */

	/*
	 * These fields are protected by CommitTsLock
 *
 * 这些字段由 CommitTsLock 保护。
	 */
	TransactionId oldestCommitTsXid;
	TransactionId newestCommitTsXid;

	/*
	 * These fields are protected by ProcArrayLock.
 *
 * 这些字段由 ProcArrayLock 保护。
	 */
	FullTransactionId latestCompletedXid;	/* newest full XID that has
											 * committed or aborted */

	/*
	 * Number of top-level transactions with xids (i.e. which may have
	 * modified the database) that completed in some form since the start of
	 * the server. This currently is solely used to check whether
	 * GetSnapshotData() needs to recompute the contents of the snapshot, or
	 * not. There are likely other users of this.  Always above 1.
 *
 * 自服务器启动以来，以某种形式完成的、带有 xid（即可能修改过数据库）的顶层事务数量。目前仅用于检查 GetSnapshotData() 是否需要重新计算快照内容。可能还有其他使用者。其值始终大于 1。
	 */
	uint64		xactCompletionCount;

	/*
	 * These fields are protected by XactTruncationLock
 *
 * 这些字段由 XactTruncationLock 保护。
	 */
	TransactionId oldestClogXid;	/* oldest it's safe to look up in clog */

	/* 当前保存的最早值。 */

} TransamVariablesData;


/* ----------------
 *		extern declarations
 *
 * extern 声明。
 * ----------------
 */

/* in transam/xact.c */

/* 位于 transam/xact.c。 */
/*
 * Function: TransactionStartedDuringRecovery.
 * Purpose: Performs the operation represented by transaction started during recovery.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionStartedDuringRecovery。
 * 作用：执行 transaction started during recovery 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionStartedDuringRecovery(void);

/* in transam/varsup.c */

/* 位于 transam/varsup.c。 */
extern PGDLLIMPORT TransamVariablesData *TransamVariables;

/*
 * prototypes for functions in transam/transam.c
 *
 * transam/transam.c 中函数的原型声明。
 */
/*
 * Function: TransactionIdDidCommit.
 * Purpose: Performs the operation represented by transaction id did commit.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdDidCommit。
 * 作用：执行 transaction id did commit 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionIdDidCommit(TransactionId transactionId);
/*
 * Function: TransactionIdDidAbort.
 * Purpose: Performs the operation represented by transaction id did abort.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdDidAbort。
 * 作用：执行 transaction id did abort 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionIdDidAbort(TransactionId transactionId);
/*
 * Function: TransactionIdCommitTree.
 * Purpose: Performs the operation represented by transaction id commit tree.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdCommitTree。
 * 作用：执行 transaction id commit tree 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void TransactionIdCommitTree(TransactionId xid, int nxids, TransactionId *xids);
/*
 * Function: TransactionIdAsyncCommitTree.
 * Purpose: Performs the operation represented by transaction id async commit tree.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdAsyncCommitTree。
 * 作用：执行 transaction id async commit tree 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void TransactionIdAsyncCommitTree(TransactionId xid, int nxids, TransactionId *xids, XLogRecPtr lsn);
/*
 * Function: TransactionIdAbortTree.
 * Purpose: Performs the operation represented by transaction id abort tree.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdAbortTree。
 * 作用：执行 transaction id abort tree 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void TransactionIdAbortTree(TransactionId xid, int nxids, TransactionId *xids);
/*
 * Function: TransactionIdPrecedes.
 * Purpose: Performs the operation represented by transaction id precedes.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdPrecedes。
 * 作用：执行 transaction id precedes 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionIdPrecedes(TransactionId id1, TransactionId id2);
/*
 * Function: TransactionIdPrecedesOrEquals.
 * Purpose: Performs the operation represented by transaction id precedes or equals.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdPrecedesOrEquals。
 * 作用：执行 transaction id precedes or equals 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2);
/*
 * Function: TransactionIdFollows.
 * Purpose: Performs the operation represented by transaction id follows.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdFollows。
 * 作用：执行 transaction id follows 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionIdFollows(TransactionId id1, TransactionId id2);
/*
 * Function: TransactionIdFollowsOrEquals.
 * Purpose: Performs the operation represented by transaction id follows or equals.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdFollowsOrEquals。
 * 作用：执行 transaction id follows or equals 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2);
/*
 * Function: TransactionIdLatest.
 * Purpose: Performs the operation represented by transaction id latest.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdLatest。
 * 作用：执行 transaction id latest 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern TransactionId TransactionIdLatest(TransactionId mainxid,
										 int nxids, const TransactionId *xids);
/*
 * Function: TransactionIdGetCommitLSN.
 * Purpose: Performs the operation represented by transaction id get commit lsn.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdGetCommitLSN。
 * 作用：执行 transaction id get commit lsn 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern XLogRecPtr TransactionIdGetCommitLSN(TransactionId xid);

/* in transam/varsup.c */

/* 位于 transam/varsup.c。 */
/*
 * Function: VarsupShmemSize.
 * Purpose: Reports the shared-memory space required by varsup shmem size.
 * Core flow: It derives the allocation size from subsystem structures before initialization.
 *
 * 函数：VarsupShmemSize。
 * 作用：报告 varsup shmem size 所需的共享内存空间。
 * 核心流程：它在初始化前根据子系统结构计算分配大小。
 */
extern Size VarsupShmemSize(void);
/*
 * Function: VarsupShmemInit.
 * Purpose: Initializes the state required for varsup shmem init.
 * Core flow: It establishes the shared, local, or on-disk state needed by later operations.
 *
 * 函数：VarsupShmemInit。
 * 作用：初始化 varsup shmem init 所需的状态。
 * 核心流程：它建立后续操作所需的共享、本地或磁盘状态。
 */
extern void VarsupShmemInit(void);
/*
 * Function: GetNewTransactionId.
 * Purpose: Obtains or checks the state represented by get new transaction id.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：GetNewTransactionId。
 * 作用：获取或检查 get new transaction id 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern FullTransactionId GetNewTransactionId(bool isSubXact);
/*
 * Function: AdvanceNextFullTransactionIdPastXid.
 * Purpose: Updates the state represented by advance next full transaction id past xid.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：AdvanceNextFullTransactionIdPastXid。
 * 作用：更新 advance next full transaction id past xid 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern void AdvanceNextFullTransactionIdPastXid(TransactionId xid);
/*
 * Function: ReadNextFullTransactionId.
 * Purpose: Obtains or checks the state represented by read next full transaction id.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：ReadNextFullTransactionId。
 * 作用：获取或检查 read next full transaction id 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern FullTransactionId ReadNextFullTransactionId(void);
/*
 * Function: SetTransactionIdLimit.
 * Purpose: Updates the state represented by set transaction id limit.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：SetTransactionIdLimit。
 * 作用：更新 set transaction id limit 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern void SetTransactionIdLimit(TransactionId oldest_datfrozenxid,
								  Oid oldest_datoid);
/*
 * Function: AdvanceOldestClogXid.
 * Purpose: Updates the state represented by advance oldest clog xid.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：AdvanceOldestClogXid。
 * 作用：更新 advance oldest clog xid 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern void AdvanceOldestClogXid(TransactionId oldest_datfrozenxid);
/*
 * Function: ForceTransactionIdLimitUpdate.
 * Purpose: Updates the state represented by force transaction id limit update.
 * Core flow: It validates its inputs, changes the relevant state, and keeps associated metadata consistent.
 *
 * 函数：ForceTransactionIdLimitUpdate。
 * 作用：更新 force transaction id limit update 所表示的状态。
 * 核心流程：它校验输入，变更相关状态，并保持关联元数据一致。
 */
extern bool ForceTransactionIdLimitUpdate(void);
/*
 * Function: GetNewObjectId.
 * Purpose: Obtains or checks the state represented by get new object id.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：GetNewObjectId。
 * 作用：获取或检查 get new object id 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern Oid	GetNewObjectId(void);
/*
 * Function: StopGeneratingPinnedObjectIds.
 * Purpose: Performs the operation represented by stop generating pinned object ids.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：StopGeneratingPinnedObjectIds。
 * 作用：执行 stop generating pinned object ids 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern void StopGeneratingPinnedObjectIds(void);

#ifdef USE_ASSERT_CHECKING
/*
 * Function: AssertTransactionIdInAllowableRange.
 * Purpose: Obtains or checks the state represented by assert transaction id in allowable range.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：AssertTransactionIdInAllowableRange。
 * 作用：获取或检查 assert transaction id in allowable range 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern void AssertTransactionIdInAllowableRange(TransactionId xid);
#else
#define AssertTransactionIdInAllowableRange(xid) ((void)true)
#endif

/*
 * Some frontend programs include this header.  For compilers that emit static
 * inline functions even when they're unused, that leads to unsatisfied
 * external references; hence hide them with #ifndef FRONTEND.
 *
 * 部分前端程序包含此头文件；为避免未使用的静态内联函数产生未满足的外部引用，使用 #ifndef FRONTEND 隐藏它们。
 */
#ifndef FRONTEND

/*
 * For callers that just need the XID part of the next transaction ID.
 *
 * 供仅需要下一个事务 ID 中 XID 部分的调用方使用。
 */
/*
 * Function: ReadNextTransactionId.
 * Purpose: Obtains or checks the state represented by read next transaction id.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：ReadNextTransactionId。
 * 作用：获取或检查 read next transaction id 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
static inline TransactionId
ReadNextTransactionId(void)
{
	return XidFromFullTransactionId(ReadNextFullTransactionId());
}

/* return transaction ID backed up by amount, handling wraparound correctly */

/* 返回向后退指定数量且已正确处理回绕的事务 ID。 */
/*
 * Function: TransactionIdRetreatedBy.
 * Purpose: Performs the operation represented by transaction id retreated by.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdRetreatedBy。
 * 作用：执行 transaction id retreated by 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline TransactionId
TransactionIdRetreatedBy(TransactionId xid, uint32 amount)
{
	xid -= amount;

	while (xid < FirstNormalTransactionId)
		xid--;

	return xid;
}

/* return the older of the two IDs */

/* 返回两个 ID 中较早的一个。 */
/*
 * Function: TransactionIdOlder.
 * Purpose: Performs the operation represented by transaction id older.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：TransactionIdOlder。
 * 作用：执行 transaction id older 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline TransactionId
TransactionIdOlder(TransactionId a, TransactionId b)
{
	if (!TransactionIdIsValid(a))
		return b;

	if (!TransactionIdIsValid(b))
		return a;

	if (TransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* return the older of the two IDs, assuming they're both normal */

/* 在两个 ID 均为普通 ID 的前提下，返回较早的一个。 */
/*
 * Function: NormalTransactionIdOlder.
 * Purpose: Performs the operation represented by normal transaction id older.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：NormalTransactionIdOlder。
 * 作用：执行 normal transaction id older 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline TransactionId
NormalTransactionIdOlder(TransactionId a, TransactionId b)
{
	Assert(TransactionIdIsNormal(a));
	Assert(TransactionIdIsNormal(b));
	if (NormalTransactionIdPrecedes(a, b))
		return a;
	return b;
}

/* return the newer of the two IDs */

/* 返回两个 ID 中较新的一个。 */
/*
 * Function: FullTransactionIdNewer.
 * Purpose: Performs the operation represented by full transaction id newer.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：FullTransactionIdNewer。
 * 作用：执行 full transaction id newer 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline FullTransactionId
FullTransactionIdNewer(FullTransactionId a, FullTransactionId b)
{
	if (!FullTransactionIdIsValid(a))
		return b;

	if (!FullTransactionIdIsValid(b))
		return a;

	if (FullTransactionIdFollows(a, b))
		return a;
	return b;
}

/*
 * Compute FullTransactionId for the given TransactionId, assuming xid was
 * between [oldestXid, nextXid] at the time when TransamVariables->nextXid was
 * nextFullXid.  When adding calls, evaluate what prevents xid from preceding
 * oldestXid if SetTransactionIdLimit() runs between the collection of xid and
 * the collection of nextFullXid.
 *
 * TransamVariables 位于共享内存中，用于跟踪 OID 与 XID 的分配状态；不同字段由不同的 LWLock 保护。
 */
/*
 * Function: FullTransactionIdFromAllowableAt.
 * Purpose: Performs the operation represented by full transaction id from allowable at.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：FullTransactionIdFromAllowableAt。
 * 作用：执行 full transaction id from allowable at 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
static inline FullTransactionId
FullTransactionIdFromAllowableAt(FullTransactionId nextFullXid,
								 TransactionId xid)
{
	uint32		epoch;

	/* Special transaction ID. */

	/* 特殊事务 ID。 */
	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);

	Assert(TransactionIdPrecedesOrEquals(xid,
										 XidFromFullTransactionId(nextFullXid)));

	/*
	 * The 64 bit result must be <= nextFullXid, since nextFullXid hadn't been
	 * issued yet when xid was in the past.  The xid must therefore be from
	 * the epoch of nextFullXid or the epoch before.  We know this because we
	 * must remove (by freezing) an XID before assigning the XID half an epoch
	 * ahead of it.
 *
 * 该 64 位结果必须 <= nextFullXid，因为在 xid 处于过去时刻时 nextFullXid 尚未被分配。因此该 xid 必然来自 nextFullXid 所在的纪元或其前一个纪元。之所以能确定这一点，是因为在分配比某个 XID 领先半个纪元的 XID 之前，必须先（通过冻结）移除该 XID。
	 *
	 * The unlikely() branch hint is dubious.  It's perfect for the first 2^32
	 * XIDs of a cluster's life.  Right at 2^32 XIDs, misprediction shoots to
	 * 100%, then improves until perfection returns 2^31 XIDs later.  Since
	 * current callers pass relatively-recent XIDs, expect >90% prediction
	 * accuracy overall.  This favors average latency over tail latency.
 *
 * unlikely() 分支提示的效果存疑。它对于集群生命周期的前 2^32 个 XID 是完美的。恰好到 2^32 个 XID 时，预测错误率飙升至 100%，随后逐渐改善，直到 2^31 个 XID 之后再次达到完美。由于当前调用方传入的都是相对较新的 XID，整体预测准确率预计 >90%。这在平均延迟与尾部延迟之间更偏向于优化平均延迟。
	 */
	epoch = EpochFromFullTransactionId(nextFullXid);
	if (unlikely(xid > XidFromFullTransactionId(nextFullXid)))
	{
		Assert(epoch != 0);
		epoch--;
	}

	return FullTransactionIdFromEpochAndXid(epoch, xid);
}

#endif							/* FRONTEND */

#endif							/* TRANSAM_H */
