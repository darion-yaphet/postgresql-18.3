/*
 * multixact.h
 *
 * 说明多事务 ID 的成员状态、共享内存维护及恢复相关约束。
 *
 * PostgreSQL multi-transaction-log manager
 *
  * PostgreSQL 多事务日志管理器
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/multixact.h
 *
 * 说明多事务 ID 的成员状态、共享内存维护及恢复相关约束。
 */
#ifndef MULTIXACT_H
#define MULTIXACT_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/sync.h"


/*
 * The first two MultiXactId values are reserved to store the truncation Xid
 * and epoch of the first segment, so we start assigning multixact values from
 * 2.
 *
 * 说明多事务 ID 的成员状态、共享内存维护及恢复相关约束。
 */
#define InvalidMultiXactId	((MultiXactId) 0)
#define FirstMultiXactId	((MultiXactId) 1)
#define MaxMultiXactId		((MultiXactId) 0xFFFFFFFF)

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

#define MaxMultiXactOffset	((MultiXactOffset) 0xFFFFFFFF)

/*
 * Possible multixact lock modes ("status").  The first four modes are for
 * tuple locks (FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, FOR UPDATE); the
 * next two are used for update and delete modes.
 *
 * 说明多事务 ID 的成员状态、共享内存维护及恢复相关约束。
 */
typedef enum
{
	MultiXactStatusForKeyShare = 0x00,
	MultiXactStatusForShare = 0x01,
	MultiXactStatusForNoKeyUpdate = 0x02,
	MultiXactStatusForUpdate = 0x03,
	/* an update that doesn't touch "key" columns */

	/* 不涉及“关键”列的更新 */
	MultiXactStatusNoKeyUpdate = 0x04,
	/* other updates, and delete */

	/* 其他更新，并删除 */
	MultiXactStatusUpdate = 0x05,
} MultiXactStatus;

#define MaxMultiXactStatus MultiXactStatusUpdate

/* does a status value correspond to a tuple update? */

/* 状态值是否对应于元组更新？ */
#define ISUPDATE_from_mxstatus(status) \
			((status) > MultiXactStatusForUpdate)


typedef struct MultiXactMember
{
	TransactionId xid;
	MultiXactStatus status;
} MultiXactMember;


/* ----------------
 *		multixact-related XLOG entries
 *
 * 说明多事务 ID 的成员状态、共享内存维护及恢复相关约束。
 * ----------------
 */

#define XLOG_MULTIXACT_ZERO_OFF_PAGE	0x00
#define XLOG_MULTIXACT_ZERO_MEM_PAGE	0x10
#define XLOG_MULTIXACT_CREATE_ID		0x20
#define XLOG_MULTIXACT_TRUNCATE_ID		0x30

typedef struct xl_multixact_create
{
	MultiXactId mid;			/* new MultiXact's ID */

	/* 新多事务 ID。 */
	MultiXactOffset moff;		/* its starting offset in members file */

	/* 它在成员文件中的起始偏移量。 */
	int32		nmembers;		/* number of member XIDs */

	/* 成员 XID 的数量。 */
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} xl_multixact_create;

#define SizeOfMultiXactCreate (offsetof(xl_multixact_create, members))

typedef struct xl_multixact_truncate
{
	Oid			oldestMultiDB;

	/* to-be-truncated range of multixact offsets */

	/* 即将截断的多事务偏移量范围。 */
	MultiXactId startTruncOff;	/* just for completeness' sake */

	/* 仅为保证信息完整。 */
	MultiXactId endTruncOff;

	/* to-be-truncated range of multixact members */

	/* 即将截断的多事务成员范围。 */
	MultiXactOffset startTruncMemb;
	MultiXactOffset endTruncMemb;
} xl_multixact_truncate;

#define SizeOfMultiXactTruncate (sizeof(xl_multixact_truncate))


/*
 * Function: MultiXactIdCreate.
 * Purpose: Performs the operation represented by multi xact id create.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdCreate。
 * 作用：执行 multi xact id create 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern MultiXactId MultiXactIdCreate(TransactionId xid1,
									 MultiXactStatus status1, TransactionId xid2,
									 MultiXactStatus status2);
/*
 * Function: MultiXactIdExpand.
 * Purpose: Performs the operation represented by multi xact id expand.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdExpand。
 * 作用：执行 multi xact id expand 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid,
									 MultiXactStatus status);
/*
 * Function: MultiXactIdCreateFromMembers.
 * Purpose: Performs the operation represented by multi xact id create from members.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdCreateFromMembers。
 * 作用：执行 multi xact id create from members 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern MultiXactId MultiXactIdCreateFromMembers(int nmembers,
												MultiXactMember *members);

/*
 * Function: ReadNextMultiXactId.
 * Purpose: Obtains or checks the state represented by read next multi xact id.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：ReadNextMultiXactId。
 * 作用：获取或检查 read next multi xact id 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern MultiXactId ReadNextMultiXactId(void);
/*
 * Function: ReadMultiXactIdRange.
 * Purpose: Obtains or checks the state represented by read multi xact id range.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：ReadMultiXactIdRange。
 * 作用：获取或检查 read multi xact id range 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern void ReadMultiXactIdRange(MultiXactId *oldest, MultiXactId *next);
/*
 * Function: MultiXactIdIsRunning.
 * Purpose: Performs the operation represented by multi xact id is running.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdIsRunning。
 * 作用：执行 multi xact id is running 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern bool MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly);
/*
 * Function: MultiXactIdSetOldestMember.
 * Purpose: Performs the operation represented by multi xact id set oldest member.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdSetOldestMember。
 * 作用：执行 multi xact id set oldest member 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void MultiXactIdSetOldestMember(void);
/*
 * Function: GetMultiXactIdMembers.
 * Purpose: Obtains or checks the state represented by get multi xact id members.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：GetMultiXactIdMembers。
 * 作用：获取或检查 get multi xact id members 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern int	GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
								  bool from_pgupgrade, bool isLockOnly);
/*
 * Function: MultiXactIdPrecedes.
 * Purpose: Performs the operation represented by multi xact id precedes.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdPrecedes。
 * 作用：执行 multi xact id precedes 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);
/*
 * Function: MultiXactIdPrecedesOrEquals.
 * Purpose: Performs the operation represented by multi xact id precedes or equals.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactIdPrecedesOrEquals。
 * 作用：执行 multi xact id precedes or equals 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern bool MultiXactIdPrecedesOrEquals(MultiXactId multi1,
										MultiXactId multi2);

/*
 * Function: multixactoffsetssyncfiletag.
 * Purpose: Performs the operation represented by multixactoffsetssyncfiletag.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixactoffsetssyncfiletag。
 * 作用：执行 multixactoffsetssyncfiletag 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern int	multixactoffsetssyncfiletag(const FileTag *ftag, char *path);
/*
 * Function: multixactmemberssyncfiletag.
 * Purpose: Performs the operation represented by multixactmemberssyncfiletag.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixactmemberssyncfiletag。
 * 作用：执行 multixactmemberssyncfiletag 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern int	multixactmemberssyncfiletag(const FileTag *ftag, char *path);

/*
 * Function: AtEOXact_MultiXact.
 * Purpose: Performs the operation represented by at eoxact multi xact.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：AtEOXact_MultiXact。
 * 作用：执行 at eoxact multi xact 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void AtEOXact_MultiXact(void);
/*
 * Function: AtPrepare_MultiXact.
 * Purpose: Performs the operation represented by at prepare multi xact.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：AtPrepare_MultiXact。
 * 作用：执行 at prepare multi xact 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void AtPrepare_MultiXact(void);
/*
 * Function: PostPrepare_MultiXact.
 * Purpose: Performs the operation represented by post prepare multi xact.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：PostPrepare_MultiXact。
 * 作用：执行 post prepare multi xact 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void PostPrepare_MultiXact(TransactionId xid);

/*
 * Function: MultiXactShmemSize.
 * Purpose: Reports the shared-memory space required by multi xact shmem size.
 * Core flow: It derives the allocation size from the subsystem structures and returns it before shared-memory initialization.
 *
 * 函数：MultiXactShmemSize。
 * 作用：报告 multi xact shmem size 所需的共享内存空间。
 * 核心流程：它根据子系统结构计算分配大小，并在共享内存初始化前返回该值。
 */
extern Size MultiXactShmemSize(void);
/*
 * Function: MultiXactShmemInit.
 * Purpose: Initializes the state required for multi xact shmem init.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：MultiXactShmemInit。
 * 作用：初始化 multi xact shmem init 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void MultiXactShmemInit(void);
/*
 * Function: BootStrapMultiXact.
 * Purpose: Initializes the state required for boot strap multi xact.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：BootStrapMultiXact。
 * 作用：初始化 boot strap multi xact 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void BootStrapMultiXact(void);
/*
 * Function: StartupMultiXact.
 * Purpose: Initializes the state required for startup multi xact.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：StartupMultiXact。
 * 作用：初始化 startup multi xact 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void StartupMultiXact(void);
/*
 * Function: TrimMultiXact.
 * Purpose: Updates the state represented by trim multi xact.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：TrimMultiXact。
 * 作用：更新 trim multi xact 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void TrimMultiXact(void);
/*
 * Function: SetMultiXactIdLimit.
 * Purpose: Updates the state represented by set multi xact id limit.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：SetMultiXactIdLimit。
 * 作用：更新 set multi xact id limit 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void SetMultiXactIdLimit(MultiXactId oldest_datminmxid,
								Oid oldest_datoid,
								bool is_startup);
/*
 * Function: MultiXactGetCheckptMulti.
 * Purpose: Performs the operation represented by multi xact get checkpt multi.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactGetCheckptMulti。
 * 作用：执行 multi xact get checkpt multi 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void MultiXactGetCheckptMulti(bool is_shutdown,
									 MultiXactId *nextMulti,
									 MultiXactOffset *nextMultiOffset,
									 MultiXactId *oldestMulti,
									 Oid *oldestMultiDB);
/*
 * Function: CheckPointMultiXact.
 * Purpose: Obtains or checks the state represented by check point multi xact.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：CheckPointMultiXact。
 * 作用：获取或检查 check point multi xact 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern void CheckPointMultiXact(void);
/*
 * Function: GetOldestMultiXactId.
 * Purpose: Obtains or checks the state represented by get oldest multi xact id.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：GetOldestMultiXactId。
 * 作用：获取或检查 get oldest multi xact id 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern MultiXactId GetOldestMultiXactId(void);
/*
 * Function: TruncateMultiXact.
 * Purpose: Updates the state represented by truncate multi xact.
 * Core flow: It validates the supplied inputs, changes the relevant transaction or WAL state, and keeps the associated metadata consistent.
 *
 * 函数：TruncateMultiXact。
 * 作用：更新 truncate multi xact 所表示的状态。
 * 核心流程：它校验输入，变更相关事务或 WAL 状态，并保持关联元数据一致。
 */
extern void TruncateMultiXact(MultiXactId newOldestMulti,
							  Oid newOldestMultiDB);
/*
 * Function: MultiXactSetNextMXact.
 * Purpose: Performs the operation represented by multi xact set next mxact.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactSetNextMXact。
 * 作用：执行 multi xact set next mxact 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void MultiXactSetNextMXact(MultiXactId nextMulti,
								  MultiXactOffset nextMultiOffset);
/*
 * Function: MultiXactAdvanceNextMXact.
 * Purpose: Performs the operation represented by multi xact advance next mxact.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactAdvanceNextMXact。
 * 作用：执行 multi xact advance next mxact 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void MultiXactAdvanceNextMXact(MultiXactId minMulti,
									  MultiXactOffset minMultiOffset);
/*
 * Function: MultiXactAdvanceOldest.
 * Purpose: Performs the operation represented by multi xact advance oldest.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactAdvanceOldest。
 * 作用：执行 multi xact advance oldest 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void MultiXactAdvanceOldest(MultiXactId oldestMulti, Oid oldestMultiDB);
/*
 * Function: MultiXactMemberFreezeThreshold.
 * Purpose: Performs the operation represented by multi xact member freeze threshold.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：MultiXactMemberFreezeThreshold。
 * 作用：执行 multi xact member freeze threshold 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern int	MultiXactMemberFreezeThreshold(void);

/*
 * Function: multixact_twophase_recover.
 * Purpose: Performs the operation represented by multixact twophase recover.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixact_twophase_recover。
 * 作用：执行 multixact twophase recover 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void multixact_twophase_recover(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
/*
 * Function: multixact_twophase_postcommit.
 * Purpose: Performs the operation represented by multixact twophase postcommit.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixact_twophase_postcommit。
 * 作用：执行 multixact twophase postcommit 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void multixact_twophase_postcommit(TransactionId xid, uint16 info,
										  void *recdata, uint32 len);
/*
 * Function: multixact_twophase_postabort.
 * Purpose: Performs the operation represented by multixact twophase postabort.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixact_twophase_postabort。
 * 作用：执行 multixact twophase postabort 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void multixact_twophase_postabort(TransactionId xid, uint16 info,
										 void *recdata, uint32 len);

/*
 * Function: multixact_redo.
 * Purpose: Performs the operation represented by multixact redo.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixact_redo。
 * 作用：执行 multixact redo 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void multixact_redo(XLogReaderState *record);
/*
 * Function: multixact_desc.
 * Purpose: Performs the operation represented by multixact desc.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixact_desc。
 * 作用：执行 multixact desc 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern void multixact_desc(StringInfo buf, XLogReaderState *record);
/*
 * Function: multixact_identify.
 * Purpose: Performs the operation represented by multixact identify.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：multixact_identify。
 * 作用：执行 multixact identify 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern const char *multixact_identify(uint8 info);
/*
 * Function: mxid_to_string.
 * Purpose: Performs the operation represented by mxid to string.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：mxid_to_string。
 * 作用：执行 mxid to string 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
extern char *mxid_to_string(MultiXactId multi, int nmembers,
							MultiXactMember *members);

#endif							/* MULTIXACT_H */
