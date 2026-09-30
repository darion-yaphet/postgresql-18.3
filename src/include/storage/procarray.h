/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procarray.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCARRAY_H
#define PROCARRAY_H

#include "storage/lock.h"
#include "storage/standby.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


/*
 * Returns the shared-memory space needed by the process array.
 */

/*
 * 返回进程数组所需的共享内存空间。
 */
extern Size ProcArrayShmemSize(void);
/*
 * Initializes the process array in shared memory.
 */

/*
 * 初始化共享内存中的进程数组。
 */
extern void ProcArrayShmemInit(void);
/*
 * Adds a process to the process array when it becomes visible.
 */

/*
 * 在进程变为可见时将其加入进程数组。
 */
extern void ProcArrayAdd(PGPROC *proc);
/*
 * Removes a process from the process array after recording its latest XID.
 */

/*
 * 记录进程最新 XID 后，将其从进程数组移除。
 */
extern void ProcArrayRemove(PGPROC *proc, TransactionId latestXid);

/*
 * Finalizes a transaction's process-array state at transaction end.
 */

/*
 * 在事务结束时完成该事务的进程数组状态处理。
 */
extern void ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid);
/*
 * Clears a process's active transaction fields in the process array.
 */

/*
 * 清除进程数组中该进程的活动事务字段。
 */
extern void ProcArrayClearTransaction(PGPROC *proc);

/*
 * Initializes process-array state for recovery up to an XID.
 */

/*
 * 将进程数组恢复状态初始化到指定 XID。
 */
extern void ProcArrayInitRecovery(TransactionId initializedUptoXID);
/*
 * Applies running-transaction information received during recovery.
 */

/*
 * 应用恢复期间接收的运行中事务信息。
 */
extern void ProcArrayApplyRecoveryInfo(RunningTransactions running);
/*
 * Applies a recovered top-level XID and its subtransaction assignments.
 */

/*
 * 应用恢复得到的顶层 XID 及其子事务分配。
 */
extern void ProcArrayApplyXidAssignment(TransactionId topxid,
										int nsubxids, TransactionId *subxids);

/*
 * Records a transaction ID as known-assigned during recovery.
 */

/*
 * 在恢复期间将一个事务 ID 记录为已知已分配。
 */
extern void RecordKnownAssignedTransactionIds(TransactionId xid);
/*
 * Expires known-assigned IDs for a completed transaction tree.
 */

/*
 * 使已完成事务树的已知已分配 ID 失效。
 */
extern void ExpireTreeKnownAssignedTransactionIds(TransactionId xid,
												  int nsubxids, TransactionId *subxids,
												  TransactionId max_xid);
/*
 * Expires all known-assigned transaction IDs.
 */

/*
 * 使所有已知已分配事务 ID 失效。
 */
extern void ExpireAllKnownAssignedTransactionIds(void);
/*
 * Expires known-assigned transaction IDs older than an XID.
 */

/*
 * 使早于指定 XID 的已知已分配事务 ID 失效。
 */
extern void ExpireOldKnownAssignedTransactionIds(TransactionId xid);
/*
 * Performs idle-time maintenance of known-assigned transaction IDs.
 */

/*
 * 在空闲期间维护已知已分配事务 ID。
 */
extern void KnownAssignedTransactionIdsIdleMaintenance(void);

/*
 * Returns the maximum number of XIDs that a snapshot may contain.
 */

/*
 * 返回快照可能包含的最大 XID 数。
 */
extern int	GetMaxSnapshotXidCount(void);
/*
 * Returns the maximum number of subtransaction XIDs in a snapshot.
 */

/*
 * 返回快照中子事务 XID 的最大数量。
 */
extern int	GetMaxSnapshotSubxidCount(void);

/*
 * Builds a snapshot from the current process-array state.
 */

/*
 * 根据当前进程数组状态构建快照。
 */
extern Snapshot GetSnapshotData(Snapshot snapshot);

/*
 * Installs an imported xmin and identifies its source virtual transaction.
 */

/*
 * 安装导入的 xmin 并标识其来源虚拟事务。
 */
extern bool ProcArrayInstallImportedXmin(TransactionId xmin,
										 VirtualTransactionId *sourcevxid);
/*
 * Installs a restored xmin for a process during recovery.
 */

/*
 * 在恢复期间为进程安装还原的 xmin。
 */
extern bool ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc);

/*
 * Collects information about currently running transactions.
 */

/*
 * 收集当前运行中事务的信息。
 */
extern RunningTransactions GetRunningTransactionData(void);

/*
 * Tests whether an XID is currently in progress.
 */

/*
 * 检查一个 XID 是否正在执行。
 */
extern bool TransactionIdIsInProgress(TransactionId xid);
/*
 * Tests whether an XID belongs to an active backend.
 */

/*
 * 检查一个 XID 是否属于活动后端。
 */
extern bool TransactionIdIsActive(TransactionId xid);
/*
 * Finds the oldest transaction that cannot be removed for a relation.
 */

/*
 * 查找对关系而言不可移除的最早事务。
 */
extern TransactionId GetOldestNonRemovableTransactionId(Relation rel);
/*
 * Returns the oldest XID that must still be considered running.
 */

/*
 * 返回仍必须视为运行中的最早 XID。
 */
extern TransactionId GetOldestTransactionIdConsideredRunning(void);
/*
 * Returns the oldest XID among active processes.
 */

/*
 * 返回活动进程中的最早 XID。
 */
extern TransactionId GetOldestActiveTransactionId(void);
/*
 * Returns the oldest XID safe for logical decoding.
 */

/*
 * 返回可安全用于逻辑解码的最早 XID。
 */
extern TransactionId GetOldestSafeDecodingTransactionId(bool catalogOnly);
/*
 * Collects normal and catalog replication horizons.
 */

/*
 * 收集普通和目录复制视界。
 */
extern void GetReplicationHorizons(TransactionId *xmin, TransactionId *catalog_xmin);

/*
 * Collects virtual transactions that delay checkpoint processing.
 */

/*
 * 收集会延迟检查点处理的虚拟事务。
 */
extern VirtualTransactionId *GetVirtualXIDsDelayingChkpt(int *nvxids, int type);
/*
 * Tests whether any supplied virtual transaction delays checkpoint processing.
 */

/*
 * 检查给定虚拟事务中是否有会延迟检查点处理的事务。
 */
extern bool HaveVirtualXIDsDelayingChkpt(VirtualTransactionId *vxids,
										 int nvxids, int type);

/*
 * Returns the PGPROC entry for a process number.
 */

/*
 * 返回给定进程编号对应的 PGPROC 条目。
 */
extern PGPROC *ProcNumberGetProc(int procNumber);
/*
 * Retrieves transaction IDs and subtransaction state for a process number.
 */

/*
 * 获取给定进程编号的事务 ID 和子事务状态。
 */
extern void ProcNumberGetTransactionIds(int procNumber, TransactionId *xid,
										TransactionId *xmin, int *nsubxid,
										bool *overflowed);
/*
 * Returns the PGPROC entry for a backend PID.
 */

/*
 * 返回给定后端 PID 对应的 PGPROC 条目。
 */
extern PGPROC *BackendPidGetProc(int pid);
/*
 * Returns a backend's PGPROC entry while retaining the required lock.
 */

/*
 * 在持有所需锁的情况下返回后端的 PGPROC 条目。
 */
extern PGPROC *BackendPidGetProcWithLock(int pid);
/*
 * Finds the backend PID currently associated with an XID.
 */

/*
 * 查找当前与 XID 关联的后端 PID。
 */
extern int	BackendXidGetPid(TransactionId xid);
/*
 * Tests whether a PID belongs to a PostgreSQL backend.
 */

/*
 * 检查一个 PID 是否属于 PostgreSQL 后端。
 */
extern bool IsBackendPid(int pid);

/*
 * Collects virtual transactions matching current-transaction filters.
 */

/*
 * 收集符合当前事务筛选条件的虚拟事务。
 */
extern VirtualTransactionId *GetCurrentVirtualXIDs(TransactionId limitXmin,
												   bool excludeXmin0, bool allDbs, int excludeVacuum,
												   int *nvxids);
/*
 * Collects virtual transactions conflicting with a horizon in one database.
 */

/*
 * 收集与一个数据库中某个视界冲突的虚拟事务。
 */
extern VirtualTransactionId *GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid);
/*
 * Cancels a virtual transaction by locating it and sending a signal.
 */

/*
 * 通过定位虚拟事务并发送信号来取消它。
 */
extern pid_t CancelVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode);
/*
 * Signals a virtual transaction and reports its process ID.
 */

/*
 * 向虚拟事务发送信号并报告其进程 ID。
 */
extern pid_t SignalVirtualTransaction(VirtualTransactionId vxid, ProcSignalReason sigmode,
									  bool conflictPending);

/*
 * Tests whether at least a requested number of backends are active.
 */

/*
 * 检查是否至少有请求数量的后端处于活动状态。
 */
extern bool MinimumActiveBackends(int min);
/*
 * Counts backends connected to a database.
 */

/*
 * 统计连接到某个数据库的后端数。
 */
extern int	CountDBBackends(Oid databaseid);
/*
 * Counts client connections to a database.
 */

/*
 * 统计连接到某个数据库的客户端连接数。
 */
extern int	CountDBConnections(Oid databaseid);
/*
 * Cancels backends of a database using the requested signal mode.
 */

/*
 * 使用请求的信号模式取消某个数据库的后端。
 */
extern void CancelDBBackends(Oid databaseid, ProcSignalReason sigmode, bool conflictPending);
/*
 * Counts backends running under a role.
 */

/*
 * 统计以某个角色身份运行的后端数。
 */
extern int	CountUserBackends(Oid roleid);
/*
 * Counts backends and prepared transactions in other databases.
 */

/*
 * 统计其他数据库中的后端和已准备事务。
 */
extern bool CountOtherDBBackends(Oid databaseId,
								 int *nbackends, int *nprepared);
/*
 * Terminates backends connected to other databases.
 */

/*
 * 终止连接到其他数据库的后端。
 */
extern void TerminateOtherDBBackends(Oid databaseId);

/*
 * Removes completed XIDs from per-process subtransaction caches.
 */

/*
 * 从每进程子事务缓存中移除已完成的 XID。
 */
extern void XidCacheRemoveRunningXids(TransactionId xid,
									  int nxids, const TransactionId *xids,
									  TransactionId latestXid);

/*
 * Sets replication-slot xmin horizons, coordinating optional locking.
 */

/*
 * 设置复制槽 xmin 视界，并协调可选的加锁。
 */
extern void ProcArraySetReplicationSlotXmin(TransactionId xmin,
											TransactionId catalog_xmin, bool already_locked);

/*
 * Retrieves the current replication-slot xmin horizons.
 */

/*
 * 获取当前复制槽 xmin 视界。
 */
extern void ProcArrayGetReplicationSlotXmin(TransactionId *xmin,
											TransactionId *catalog_xmin);

#endif							/* PROCARRAY_H */
