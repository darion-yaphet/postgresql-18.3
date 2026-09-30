/*-------------------------------------------------------------------------
 *
 * lwlocklist.h
 *
 * The predefined LWLock list is kept in its own source file for use by
 * automatic tools.  The exact representation of a keyword is determined by
 * the PG_LWLOCK macro, which is not defined in this file; it can be
 * defined by the caller for special purposes.
 *
 * Also, generate-lwlocknames.pl processes this file to create lwlocknames.h.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/include/storage/lwlocklist.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 预定义的 LWLock 列表保存在独立源文件中，以供自动化工具使用。关键字的确切表示由本文件
 * 未定义的 PG_LWLOCK 宏决定；调用方可为特殊用途定义它。
 * 此外，generate-lwlocknames.pl 会处理本文件以生成 lwlocknames.h。
 */

/*
 * Some commonly-used locks have predefined positions within MainLWLockArray;
 * these are defined here.  If you add a lock, add it to the end to avoid
 * renumbering the existing locks; if you remove a lock, consider leaving a gap
 * in the numbering sequence for the benefit of DTrace and other external
 * debugging scripts.  Also, do not forget to update the section
 * WaitEventLWLock of src/backend/utils/activity/wait_event_names.txt.
 *
 * Note that the names here don't include the Lock suffix, to appease the
 * C preprocessor; it's added elsewhere.
 */

/*
 * 一些常用锁在 MainLWLockArray 中具有预定义位置，并在此定义。新增锁时请追加到末尾，
 * 避免重新编号已有锁；删除锁时，请考虑保留编号空档，以方便 DTrace 和其他外部调试脚本。
 * 同时不要忘记更新 src/backend/utils/activity/wait_event_names.txt 的 WaitEventLWLock 部分。
 * 请注意，此处名称不含 Lock 后缀，以适应 C 预处理器；该后缀会在其他位置添加。
 */

/* 0 is available; was formerly BufFreelistLock */

/*
 * 0 可用；此前为 BufFreelistLock。
 */
PG_LWLOCK(1, ShmemIndex)
PG_LWLOCK(2, OidGen)
PG_LWLOCK(3, XidGen)
PG_LWLOCK(4, ProcArray)
PG_LWLOCK(5, SInvalRead)
PG_LWLOCK(6, SInvalWrite)
PG_LWLOCK(7, WALBufMapping)
PG_LWLOCK(8, WALWrite)
PG_LWLOCK(9, ControlFile)
/* 10 was CheckpointLock */

/*
 * 10 此前为 CheckpointLock。
 */
/* 11 was XactSLRULock */

/*
 * 11 此前为 XactSLRULock。
 */
/* 12 was SubtransSLRULock */

/*
 * 12 此前为 SubtransSLRULock。
 */
PG_LWLOCK(13, MultiXactGen)
/* 14 was MultiXactOffsetSLRULock */

/*
 * 14 此前为 MultiXactOffsetSLRULock。
 */
/* 15 was MultiXactMemberSLRULock */

/*
 * 15 此前为 MultiXactMemberSLRULock。
 */
PG_LWLOCK(16, RelCacheInit)
PG_LWLOCK(17, CheckpointerComm)
PG_LWLOCK(18, TwoPhaseState)
PG_LWLOCK(19, TablespaceCreate)
PG_LWLOCK(20, BtreeVacuum)
PG_LWLOCK(21, AddinShmemInit)
PG_LWLOCK(22, Autovacuum)
PG_LWLOCK(23, AutovacuumSchedule)
PG_LWLOCK(24, SyncScan)
PG_LWLOCK(25, RelationMapping)
/* 26 was NotifySLRULock */

/*
 * 26 此前为 NotifySLRULock。
 */
PG_LWLOCK(27, NotifyQueue)
PG_LWLOCK(28, SerializableXactHash)
PG_LWLOCK(29, SerializableFinishedList)
PG_LWLOCK(30, SerializablePredicateList)
/* 31 was SerialSLRULock */

/*
 * 31 此前为 SerialSLRULock。
 */
PG_LWLOCK(32, SyncRep)
PG_LWLOCK(33, BackgroundWorker)
PG_LWLOCK(34, DynamicSharedMemoryControl)
PG_LWLOCK(35, AutoFile)
PG_LWLOCK(36, ReplicationSlotAllocation)
PG_LWLOCK(37, ReplicationSlotControl)
/* 38 was CommitTsSLRULock */

/*
 * 38 此前为 CommitTsSLRULock。
 */
PG_LWLOCK(39, CommitTs)
PG_LWLOCK(40, ReplicationOrigin)
PG_LWLOCK(41, MultiXactTruncation)
/* 42 was OldSnapshotTimeMapLock */

/*
 * 42 此前为 OldSnapshotTimeMapLock。
 */
PG_LWLOCK(43, LogicalRepWorker)
PG_LWLOCK(44, XactTruncation)
/* 45 was XactTruncationLock until removal of BackendRandomLock */

/*
 * 在移除 BackendRandomLock 前，45 为 XactTruncationLock。
 */
PG_LWLOCK(46, WrapLimitsVacuum)
PG_LWLOCK(47, NotifyQueueTail)
PG_LWLOCK(48, WaitEventCustom)
PG_LWLOCK(49, WALSummarizer)
PG_LWLOCK(50, DSMRegistry)
PG_LWLOCK(51, InjectionPoint)
PG_LWLOCK(52, SerialControl)
PG_LWLOCK(53, AioWorkerSubmissionQueue)
