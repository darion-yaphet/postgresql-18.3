/* -------------------------------------------------------------------------
 *
 * auth_delay.c
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/auth_delay/auth_delay.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "libpq/auth.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
					.name = "auth_delay",
					.version = PG_VERSION
);

/* GUC Variables
 *
 * GUC变量
 */
static int	auth_delay_milliseconds = 0;

/* Original Hook
 *
 * 原创挂钩
 */
static ClientAuthentication_hook_type original_client_auth_hook = NULL;

/*
 * Check authentication
 *
 * 检查身份验证
 */
static void
auth_delay_checks(Port *port, int status)
{
	/*
	 * Any other plugins which use ClientAuthentication_hook.
	 *
	 * 使用 ClientAuthentication_hook 的任何其他插件。
	 */
	if (original_client_auth_hook)
		original_client_auth_hook(port, status);

	/*
	 * Inject a short delay if authentication failed.
	 *
	 * 如果身份验证失败，则注入短暂的延迟。
	 */
	if (status != STATUS_OK)
	{
		pg_usleep(1000L * auth_delay_milliseconds);
	}
}

/*
 * Module Load Callback
 *
 * 模块加载回调
 */
void
_PG_init(void)
{
	/* Define custom GUC variables
	 *
	 * 定义自定义 GUC 变量
	 */
	DefineCustomIntVariable("auth_delay.milliseconds",
							"Milliseconds to delay before reporting authentication failure",
							NULL,
							&auth_delay_milliseconds,
							0,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("auth_delay");

	/* Install Hooks
	 *
	 * 安装挂钩
	 */
	original_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = auth_delay_checks;
}
