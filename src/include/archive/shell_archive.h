/*-------------------------------------------------------------------------
 *
 * shell_archive.h
 *		Exports for archiving via shell.
 *		通过 shell 进行归档的导出。
 *
 * 实现核心流程概述：
 * 本头文件定义了 PostgreSQL 内置的基于 shell 命令的归档模块接口。
 * 虽然 PostgreSQL 支持通过加载外部共享库来实现归档模块，但为了兼容性和简化配置，
 * 基于 shell 命令（archive_command）的归档逻辑被直接构建在服务器核心中。
 *
 * 核心流程如下：
 * 1. 模块初始化：归档进程（archiver）启动时调用 shell_archive_init()。
 * 2. 回调注册：该函数返回一个 ArchiveModuleCallbacks 结构，包含了执行归档动作的具体回调。
 * 3. 命令执行：核心代码根据这些回调，通过 shell 执行用户配置的 archive_command 脚本来搬运 WAL 日志。
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * src/include/archive/shell_archive.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SHELL_ARCHIVE_H
#define _SHELL_ARCHIVE_H

#include "archive/archive_module.h"

/*
 * Since the logic for archiving via a shell command is in the core server
 * and does not need to be loaded via a shared library, it has a special
 * initialization function.
 * 由于通过 shell 命令进行归档的逻辑位于核心服务器中，且不需要通过共享库加载，因此它具有特殊的初始化函数。
 */
extern const ArchiveModuleCallbacks *shell_archive_init(void);

#endif							/* _SHELL_ARCHIVE_H */
