/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  Exports from postmaster/postmaster.c.
 *	  来自 postmaster/postmaster.c 的导出内容。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

#include "lib/ilist.h"
#include "miscadmin.h"

/*
 * A struct representing an active postmaster child process.  This is used
 * mainly to keep track of how many children we have and send them appropriate
 * signals when necessary.  All postmaster child processes are assigned a
 * PMChild entry.  That includes "normal" client sessions, but also autovacuum
 * workers, walsenders, background workers, and aux processes.  (Note that at
 * the time of launch, walsenders are labeled B_BACKEND; we relabel them to
 * B_WAL_SENDER upon noticing they've changed their PMChildFlags entry.  Hence
 * that check must be done before any operation that needs to distinguish
 * walsenders from normal backends.)
 *
 * 这是一个表示活跃的 postmaster 子进程的结构体。它主要用于跟踪我们拥有多少
 * 个子进程，并在必要时向它们发送适当的信号。所有 postmaster 子进程都会被
 * 分配一个 PMChild 条目。这包括“正常的”客户端会话，但也包括 autovacuum 
 * 工作进程、walsender、后台工作进程和辅助进程。（请注意，在启动时，
 * walsender 被标记为 B_BACKEND；在注意到它们更改了 PMChildFlags 条目后，
 * 我们会将它们重新标记为 B_WAL_SENDER。因此，该检查必须在任何需要区分 
 * walsender 和普通后端的辅助操作之前完成。）
 *
 * "dead-end" children are also allocated a PMChild entry: these are children
 * launched just for the purpose of sending a friendly rejection message to a
 * would-be client.  We must track them because they are attached to shared
 * memory, but we know they will never become live backends.
 *
 * “死路（dead-end）”子进程也被分配了一个 PMChild 条目：这些子进程的启动
 * 仅仅是为了向潜在的客户端发送友好的拒绝消息。我们必须跟踪它们，因为它们
 * 附着在共享内存上，但我们知道它们永远不会成为活跃的后端进程。
 *
 * child_slot is an identifier that is unique across all running child
 * processes.  It is used as an index into the PMChildFlags array.  dead-end
 * children are not assigned a child_slot and have child_slot == 0 (valid
 * child_slot ids start from 1).
 *
 * child_slot 是一个在所有运行的子进程中唯一的标识符。它被用作 PMChildFlags 
 * 数组的索引。死路子进程不会被分配 child_slot，且其 child_slot == 0 
 * （有效的 child_slot ID 从 1 开始）。
 */
typedef struct
{
	pid_t		pid;			/* process id of backend */
	int			child_slot;		/* PMChildSlot for this backend, if any */
	BackendType bkend_type;		/* child process flavor, see above */
	struct RegisteredBgWorker *rw;	/* bgworker info, if this is a bgworker */
	bool		bgworker_notify;	/* gets bgworker start/stop notifications */
	dlist_node	elem;			/* list link in ActiveChildList */
} PMChild;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT int num_pmchild_slots;
#endif

/* GUC options */
/* GUC 选项 */
extern PGDLLIMPORT bool EnableSSL;
extern PGDLLIMPORT int SuperuserReservedConnections;
extern PGDLLIMPORT int ReservedConnections;
extern PGDLLIMPORT int PostPortNumber;
extern PGDLLIMPORT int Unix_socket_permissions;
extern PGDLLIMPORT char *Unix_socket_group;
extern PGDLLIMPORT char *Unix_socket_directories;
extern PGDLLIMPORT char *ListenAddresses;
extern PGDLLIMPORT bool ClientAuthInProgress;
extern PGDLLIMPORT int PreAuthDelay;
extern PGDLLIMPORT int AuthenticationTimeout;
extern PGDLLIMPORT bool log_hostname;
extern PGDLLIMPORT bool enable_bonjour;
extern PGDLLIMPORT char *bonjour_name;
extern PGDLLIMPORT bool restart_after_crash;
extern PGDLLIMPORT bool remove_temp_files_after_crash;
extern PGDLLIMPORT bool send_abort_for_crash;
extern PGDLLIMPORT bool send_abort_for_kill;

#ifdef WIN32
extern PGDLLIMPORT HANDLE PostmasterHandle;
#else
extern PGDLLIMPORT int postmaster_alive_fds[2];

/*
 * Constants that represent which of postmaster_alive_fds is held by
 * postmaster, and which is used in children to check for postmaster death.
 *
 * 这些常量表示 postmaster_alive_fds 中的哪一个由 postmaster 持有，
 * 哪一个由子进程用于检查 postmaster 是否死亡。
 */
#define POSTMASTER_FD_WATCH		0	/* used in children to check for
									 * postmaster death */
									/* 子进程用于检查 postmaster 是否死亡 */
#define POSTMASTER_FD_OWN		1	/* kept open by postmaster only */
									/* 仅由 postmaster 保持打开状态 */
#endif

extern PGDLLIMPORT const char *progname;

extern PGDLLIMPORT bool redirection_done;
extern PGDLLIMPORT bool LoadedSSL;

pg_noreturn extern void PostmasterMain(int argc, char *argv[]);
extern void ClosePostmasterPorts(bool am_syslogger);
extern void InitProcessGlobals(void);

extern int	MaxLivePostmasterChildren(void);

extern bool PostmasterMarkPIDForWorkerNotify(int);

#ifdef WIN32
extern void pgwin32_register_deadchild_callback(HANDLE procHandle, DWORD procId);
#endif

/* defined in globals.c */
/* 在 globals.c 中定义 */
extern PGDLLIMPORT struct ClientSocket *MyClientSocket;

/* prototypes for functions in launch_backend.c */
/* launch_backend.c 中函数的原型 */
extern pid_t postmaster_child_launch(BackendType child_type,
									 int child_slot,
									 void *startup_data,
									 size_t startup_data_len,
									 struct ClientSocket *client_sock);
const char *PostmasterChildName(BackendType child_type);
#ifdef EXEC_BACKEND
pg_noreturn extern void SubPostmasterMain(int argc, char *argv[]);
#endif

/* defined in pmchild.c */
/* 在 pmchild.c 中定义 */
extern PGDLLIMPORT dlist_head ActiveChildList;

extern void InitPostmasterChildSlots(void);
extern PMChild *AssignPostmasterChildSlot(BackendType btype);
extern PMChild *AllocDeadEndChild(void);
extern bool ReleasePostmasterChildSlot(PMChild *pmchild);
extern PMChild *FindPostmasterChildByPid(int pid);

/*
 * These values correspond to the special must-be-first options for dispatching
 * to various subprograms.  parse_dispatch_option() can be used to convert an
 * option name to one of these values.
 *
 * 这些值对应于分发到各种子程序的特殊的、必须排在首位的选项。
 * parse_dispatch_option() 可用于将选项名称转换为这些值之一。
 */
typedef enum DispatchOption
{
	DISPATCH_CHECK,
	DISPATCH_BOOT,
	DISPATCH_FORKCHILD,
	DISPATCH_DESCRIBE_CONFIG,
	DISPATCH_SINGLE,
	DISPATCH_POSTMASTER,		/* must be last */
} DispatchOption;

extern DispatchOption parse_dispatch_option(const char *name);

#endif							/* _POSTMASTER_H */
