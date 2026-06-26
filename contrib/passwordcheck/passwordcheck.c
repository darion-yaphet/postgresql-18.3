/*-------------------------------------------------------------------------
 *
 * passwordcheck.c
 *
 *
 * Copyright (c) 2009-2025, PostgreSQL Global Development Group
 *
 * Author: Laurenz Albe <laurenz.albe@wien.gv.at>
 *
 * IDENTIFICATION
 *	  contrib/passwordcheck/passwordcheck.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#ifdef USE_CRACKLIB
#include <crack.h>
#endif

#include "commands/user.h"
#include "fmgr.h"
#include "libpq/crypt.h"

PG_MODULE_MAGIC_EXT(
					.name = "passwordcheck",
					.version = PG_VERSION
);

/* Saved hook value
 *
 * 保存的钩子值
 */
static check_password_hook_type prev_check_password_hook = NULL;

/* GUC variables
 *
 * GUC变量
 */
static int	min_password_length = 8;

/*
 * check_password
 *
 * performs checks on an encrypted or unencrypted password
 * ereport's if not acceptable
 *
 * 如果不可接受，则对加密或未加密的密码进行检查
 *
 * username: name of role being created or changed
 * password: new password (possibly already encrypted)
 * password_type: PASSWORD_TYPE_* code, to indicate if the password is
 *			in plaintext or encrypted form.
 * validuntil_time: password expiration time, as a timestamptz Datum
 * validuntil_null: true if password expiration time is NULL
 *
 * 用户名：正在创建或更改的角色名称 密码：新密码（可能已加密） 密码类型：PASSWORD_TYPE_* 代码，指示密码是明文还是加密形式。 validuntil_time：密码过期时间，作为时间戳数据 validuntil_null：如果密码过期时间为 NULL，则为 true
 *
 * This sample implementation doesn't pay any attention to the password
 * expiration time, but you might wish to insist that it be non-null and
 * not too far in the future.
 *
 * 此示例实现不关注密码过期时间，但您可能希望坚持它为非空且在未来不会太远。
 */
static void
check_password(const char *username,
			   const char *shadow_pass,
			   PasswordType password_type,
			   Datum validuntil_time,
			   bool validuntil_null)
{
	if (prev_check_password_hook)
		prev_check_password_hook(username, shadow_pass,
								 password_type, validuntil_time,
								 validuntil_null);

	if (password_type != PASSWORD_TYPE_PLAINTEXT)
	{
		/*
		 * Unfortunately we cannot perform exhaustive checks on encrypted
		 * passwords - we are restricted to guessing. (Alternatively, we could
		 * insist on the password being presented non-encrypted, but that has
		 * its own security disadvantages.)
		 *
		 * 不幸的是，我们无法对加密密码进行详尽的检查 - 我们只能猜测。 （或者，我们可以坚持以非加密方式提供密码，但这有其自身的安全缺点。）
		 *
		 * We only check for username = password.
		 *
		 * 我们只检查用户名=密码。
		 */
		const char *logdetail = NULL;

		if (plain_crypt_verify(username, shadow_pass, username, &logdetail) == STATUS_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password must not equal user name")));
	}
	else
	{
		/*
		 * For unencrypted passwords we can perform better checks
		 *
		 * 对于未加密的密码，我们可以执行更好的检查
		 */
		const char *password = shadow_pass;
		int			pwdlen = strlen(password);
		int			i;
		bool		pwd_has_letter,
					pwd_has_nonletter;
#ifdef USE_CRACKLIB
		const char *reason;
#endif

		/* enforce minimum length
		 *
		 * 强制执行最小长度
		 */
		if (pwdlen < min_password_length)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password is too short"),
					 errdetail("password must be at least \"passwordcheck.min_password_length\" (%d) bytes long",
							   min_password_length)));

		/* check if the password contains the username
		 *
		 * 检查密码是否包含用户名
		 */
		if (strstr(password, username))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password must not contain user name")));

		/* check if the password contains both letters and non-letters
		 *
		 * 检查密码是否同时包含字母和非字母
		 */
		pwd_has_letter = false;
		pwd_has_nonletter = false;
		for (i = 0; i < pwdlen; i++)
		{
			/*
			 * isalpha() does not work for multibyte encodings but let's
			 * consider non-ASCII characters non-letters
			 *
			 * isalpha() 不适用于多字节编码，但让我们考虑非 ASCII 字符非字母
			 */
			if (isalpha((unsigned char) password[i]))
				pwd_has_letter = true;
			else
				pwd_has_nonletter = true;
		}
		if (!pwd_has_letter || !pwd_has_nonletter)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password must contain both letters and nonletters")));

#ifdef USE_CRACKLIB
		/* call cracklib to check password
		 *
		 * 调用cracklib检查密码
		 */
		if ((reason = FascistCheck(password, CRACKLIB_DICTPATH)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("password is easily cracked"),
					 errdetail_log("cracklib diagnostic: %s", reason)));
#endif
	}

	/* all checks passed, password is ok
	 *
	 * 所有检查均已通过，密码正确
	 */
}

/*
 * Module initialization function
 *
 * 模块初始化函数
 */
void
_PG_init(void)
{
	/* Define custom GUC variables.
	 *
	 * 定义自定义 GUC 变量。
	 */
	DefineCustomIntVariable("passwordcheck.min_password_length",
							"Minimum allowed password length.",
							NULL,
							&min_password_length,
							8,
							0, INT_MAX,
							PGC_SUSET,
							GUC_UNIT_BYTE,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("passwordcheck");

	/* activate password checks when the module is loaded
	 *
	 * 加载模块时激活密码检查
	 */
	prev_check_password_hook = check_password_hook;
	check_password_hook = check_password;
}
