/*-------------------------------------------------------------------------
 *
 * session.h
 *	  Encapsulation of user session.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/include/access/session.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SESSION_H
#define SESSION_H

#include "lib/dshash.h"

/* Avoid including typcache.h */

/* 避免包含 typcache.h。 */
struct SharedRecordTypmodRegistry;

/*
 * A struct encapsulating some elements of a user's session.  For now this
 * manages state that applies to parallel query, but in principle it could
 * include other things that are currently global variables.
 *
 * 说明并行执行上下文、工作进程及共享状态的组织方式。
 */
typedef struct Session
{
	dsm_segment *segment;		/* The session-scoped DSM segment. */

	/* 会话范围的 DSM 段。 */
	dsa_area   *area;			/* The session-scoped DSA area. */

	/* 会话范围的 DSA 区域。 */

	/* State managed by typcache.c. */

	/* 由 typcache.c 管理的状态。 */
	struct SharedRecordTypmodRegistry *shared_typmod_registry;
	dshash_table *shared_record_table;
	dshash_table *shared_typmod_table;
} Session;

/*
 * Function: InitializeSession.
 * Purpose: Initializes the state required for initialize session.
 * Core flow: It establishes the relevant shared, local, or on-disk state so later operations can use the subsystem safely.
 *
 * 函数：InitializeSession。
 * 作用：初始化 initialize session 所需的状态。
 * 核心流程：它建立相关的共享、本地或磁盘状态，使后续操作能够安全使用该子系统。
 */
extern void InitializeSession(void);
/*
 * Function: GetSessionDsmHandle.
 * Purpose: Obtains or checks the state represented by get session dsm handle.
 * Core flow: It examines the supplied identifiers and the relevant subsystem state, then returns the derived result without changing the requested state.
 *
 * 函数：GetSessionDsmHandle。
 * 作用：获取或检查 get session dsm handle 所表示的状态。
 * 核心流程：它检查给定标识符和相关子系统状态，然后返回推导出的结果，而不改变请求的状态。
 */
extern dsm_handle GetSessionDsmHandle(void);
/*
 * Function: AttachSession.
 * Purpose: Creates, starts, or registers the resource represented by attach session.
 * Core flow: It prepares the required context, installs it in the owning subsystem, and returns or exposes the resulting resource.
 *
 * 函数：AttachSession。
 * 作用：创建、启动或注册 attach session 所表示的资源。
 * 核心流程：它准备所需上下文，将其安装到所属子系统，并返回或公开生成的资源。
 */
extern void AttachSession(dsm_handle handle);
/*
 * Function: DetachSession.
 * Purpose: Completes or releases the work represented by detach session.
 * Core flow: It applies the required completion or cleanup actions and leaves the related transaction or WAL state ready for subsequent work.
 *
 * 函数：DetachSession。
 * 作用：完成或释放 detach session 所表示的工作。
 * 核心流程：它执行所需的完成或清理操作，并使相关事务或 WAL 状态可用于后续工作。
 */
extern void DetachSession(void);

/* The current session, or NULL for none. */

/* 当前会话；不存在时为 NULL。 */
extern PGDLLIMPORT Session *CurrentSession;

#endif							/* SESSION_H */
