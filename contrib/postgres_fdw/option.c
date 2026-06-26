/*-------------------------------------------------------------------------
 *
 * option.c
 *		  FDW and GUC option handling for postgres_fdw
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/option.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "libpq/libpq-be.h"
#include "postgres_fdw.h"
#include "utils/guc.h"
#include "utils/varlena.h"

/*
 * Describes the valid options for objects that this wrapper uses.
 *
 * 描述此包装器使用的对象的有效选项。
 */
typedef struct PgFdwOption
{
	const char *keyword;
	Oid			optcontext;		/* OID of catalog in which option may appear */
	bool		is_libpq_opt;	/* true if it's used in libpq */
} PgFdwOption;

/*
 * Valid options for postgres_fdw.
 * Allocated and filled in InitPgFdwOptions.
 *
 * postgres_fdw 的有效选项。分配并填充InitPgFdwOptions。
 */
static PgFdwOption *postgres_fdw_options;

/*
 * Valid options for libpq.
 * Allocated and filled in InitPgFdwOptions.
 *
 * libpq 的有效选项。分配并填充InitPgFdwOptions。
 */
static PQconninfoOption *libpq_options;

/*
 * GUC parameters
 *
 * GUC参数
 */
char	   *pgfdw_application_name = NULL;

/*
 * Helper functions
 *
 * 辅助函数
 */
static void InitPgFdwOptions(void);
static bool is_valid_option(const char *keyword, Oid context);
static bool is_libpq_option(const char *keyword);

#include "miscadmin.h"

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses postgres_fdw.
 *
 * 验证为使用 postgres_fdw 的 FOREIGN DATA WRAPPER、SERVER、USER MAPPING 或 FOREIGN TABLE 提供的通用选项。
 *
 * Raise an ERROR if the option or its value is considered invalid.
 *
 * 如果选项或其值被认为无效，则引发错误。
 */
PG_FUNCTION_INFO_V1(postgres_fdw_validator);

Datum
postgres_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *cell;

	/* Build our options lists if we didn't yet.
	 *
	 * 如果我们还没有的话，请建立我们的选项列表。
	 */
	InitPgFdwOptions();

	/*
	 * Check that only options supported by postgres_fdw, and allowed for the
	 * current object type, are given.
	 *
	 * 检查是否仅给出了 postgres_fdw 支持且当前对象类型允许的选项。
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with a valid option that looks similar, if there is one.
			 *
			 * 指定了未知选项，抱怨它。提供一个提示，其中包含看起来相似的有效选项（如果有）。
			 */
			PgFdwOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			initClosestMatch(&match_state, def->defname, 4);
			for (opt = postgres_fdw_options; opt->keyword; opt++)
			{
				if (catalog == opt->optcontext)
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->keyword);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));
		}

		/*
		 * Validate option value, when we can do so without any context.
		 *
		 * 当我们可以在没有任何上下文的情况下验证选项值时。
		 */
		if (strcmp(def->defname, "use_remote_estimate") == 0 ||
			strcmp(def->defname, "updatable") == 0 ||
			strcmp(def->defname, "truncatable") == 0 ||
			strcmp(def->defname, "async_capable") == 0 ||
			strcmp(def->defname, "parallel_commit") == 0 ||
			strcmp(def->defname, "parallel_abort") == 0 ||
			strcmp(def->defname, "keep_connections") == 0)
		{
			/* these accept only boolean values
			 *
			 * 这些仅接受布尔值
			 */
			(void) defGetBoolean(def);
		}
		else if (strcmp(def->defname, "fdw_startup_cost") == 0 ||
				 strcmp(def->defname, "fdw_tuple_cost") == 0)
		{
			/*
			 * These must have a floating point value greater than or equal to
			 * zero.
			 *
			 * 它们的浮点值必须大于或等于零。
			 */
			char	   *value;
			double		real_val;
			bool		is_parsed;

			value = defGetString(def);
			is_parsed = parse_real(value, &real_val, 0, NULL);

			if (!is_parsed)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for floating point option \"%s\": %s",
								def->defname, value)));

			if (real_val < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"%s\" must be a floating point value greater than or equal to zero",
								def->defname)));
		}
		else if (strcmp(def->defname, "extensions") == 0)
		{
			/* check list syntax, warn about uninstalled extensions
			 *
			 * 检查列表语法，警告已卸载的扩展
			 */
			(void) ExtractExtensionList(defGetString(def), true);
		}
		else if (strcmp(def->defname, "fetch_size") == 0 ||
				 strcmp(def->defname, "batch_size") == 0)
		{
			char	   *value;
			int			int_val;
			bool		is_parsed;

			value = defGetString(def);
			is_parsed = parse_int(value, &int_val, 0, NULL);

			if (!is_parsed)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for integer option \"%s\": %s",
								def->defname, value)));

			if (int_val <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"%s\" must be an integer value greater than zero",
								def->defname)));
		}
		else if (strcmp(def->defname, "password_required") == 0)
		{
			bool		pw_required = defGetBoolean(def);

			/*
			 * Only the superuser may set this option on a user mapping, or
			 * alter a user mapping on which this option is set. We allow a
			 * user to clear this option if it's set - in fact, we don't have
			 * a choice since we can't see the old mapping when validating an
			 * alter.
			 *
			 * 只有超级用户可以在用户映射上设置此选项，或更改设置了此选项的用户映射。如果已设置，我们允许用户清除此选项 - 事实上，我们别无选择，因为在验证更改时我们看不到旧的映射。
			 */
			if (!superuser() && !pw_required)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("password_required=false is superuser-only"),
						 errhint("User mappings with the password_required option set to false may only be created or modified by the superuser.")));
		}
		else if (strcmp(def->defname, "sslcert") == 0 ||
				 strcmp(def->defname, "sslkey") == 0)
		{
			/* similarly for sslcert / sslkey on user mapping
			 *
			 * 与用户映射上的 sslcert / sslkey 类似
			 */
			if (catalog == UserMappingRelationId && !superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("sslcert and sslkey are superuser-only"),
						 errhint("User mappings with the sslcert or sslkey options set may only be created or modified by the superuser.")));
		}
		else if (strcmp(def->defname, "analyze_sampling") == 0)
		{
			char	   *value;

			value = defGetString(def);

			/* we recognize off/auto/random/system/bernoulli
			 *
			 * 我们识别关闭/自动/随机/系统/伯努利
			 */
			if (strcmp(value, "off") != 0 &&
				strcmp(value, "auto") != 0 &&
				strcmp(value, "random") != 0 &&
				strcmp(value, "system") != 0 &&
				strcmp(value, "bernoulli") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for string option \"%s\": %s",
								def->defname, value)));
		}
	}

	PG_RETURN_VOID();
}

/*
 * Initialize option lists.
 *
 * 初始化选项列表。
 */
static void
InitPgFdwOptions(void)
{
	int			num_libpq_opts;
	PQconninfoOption *lopt;
	PgFdwOption *popt;

	/* non-libpq FDW-specific FDW options
	 *
	 * 非 libpq FDW 特定的 FDW 选项
	 */
	static const PgFdwOption non_libpq_options[] = {
		{"schema_name", ForeignTableRelationId, false},
		{"table_name", ForeignTableRelationId, false},
		{"column_name", AttributeRelationId, false},
		/* use_remote_estimate is available on both server and table
		 *
		 * use_remote_estimate 在服务器和表上均可用
		 */
		{"use_remote_estimate", ForeignServerRelationId, false},
		{"use_remote_estimate", ForeignTableRelationId, false},
		/* cost factors
		 *
		 * 成本因素
		 */
		{"fdw_startup_cost", ForeignServerRelationId, false},
		{"fdw_tuple_cost", ForeignServerRelationId, false},
		/* shippable extensions
		 *
		 * 可交付的扩展
		 */
		{"extensions", ForeignServerRelationId, false},
		/* updatable is available on both server and table
		 *
		 * 可更新在服务器和表上均可用
		 */
		{"updatable", ForeignServerRelationId, false},
		{"updatable", ForeignTableRelationId, false},
		/* truncatable is available on both server and table
		 *
		 * truncatable 在服务器和表上均可用
		 */
		{"truncatable", ForeignServerRelationId, false},
		{"truncatable", ForeignTableRelationId, false},
		/* fetch_size is available on both server and table
		 *
		 * fetch_size 在服务器和表上都可用
		 */
		{"fetch_size", ForeignServerRelationId, false},
		{"fetch_size", ForeignTableRelationId, false},
		/* batch_size is available on both server and table
		 *
		 * batch_size 在服务器和表上都可用
		 */
		{"batch_size", ForeignServerRelationId, false},
		{"batch_size", ForeignTableRelationId, false},
		/* async_capable is available on both server and table
		 *
		 * async_capable 在服务器和表上均可用
		 */
		{"async_capable", ForeignServerRelationId, false},
		{"async_capable", ForeignTableRelationId, false},
		{"parallel_commit", ForeignServerRelationId, false},
		{"parallel_abort", ForeignServerRelationId, false},
		{"keep_connections", ForeignServerRelationId, false},
		{"password_required", UserMappingRelationId, false},

		/* sampling is available on both server and table
		 *
		 * 服务器和表上均可进行采样
		 */
		{"analyze_sampling", ForeignServerRelationId, false},
		{"analyze_sampling", ForeignTableRelationId, false},

		{"use_scram_passthrough", ForeignServerRelationId, false},
		{"use_scram_passthrough", UserMappingRelationId, false},

		/*
		 * sslcert and sslkey are in fact libpq options, but we repeat them
		 * here to allow them to appear in both foreign server context (when
		 * we generate libpq options) and user mapping context (from here).
		 *
		 * sslcert 和 sslkey 实际上是 libpq 选项，但我们在这里重复它们，以允许它们出现在外部服务器上下文（当我们生成 libpq 选项时）和用户映射上下文（从此处开始）中。
		 */
		{"sslcert", UserMappingRelationId, true},
		{"sslkey", UserMappingRelationId, true},

		/*
		 * gssdelegation is also a libpq option but should be allowed in a
		 * user mapping context too
		 *
		 * gssdelegation 也是一个 libpq 选项，但也应该在用户映射上下文中允许
		 */
		{"gssdelegation", UserMappingRelationId, true},

		{NULL, InvalidOid, false}
	};

	/* Prevent redundant initialization.
	 *
	 * 防止冗余初始化。
	 */
	if (postgres_fdw_options)
		return;

	/*
	 * Get list of valid libpq options.
	 *
	 * 获取有效 libpq 选项的列表。
	 *
	 * To avoid unnecessary work, we get the list once and use it throughout
	 * the lifetime of this backend process.  We don't need to care about
	 * memory context issues, because PQconndefaults allocates with malloc.
	 *
	 * 为了避免不必要的工作，我们只获取一次列表，并在后端进程的整个生命周期中使用它。  我们不需要关心内存上下文问题，因为 PQconndefaults 使用 malloc 进行分配。
	 */
	libpq_options = PQconndefaults();
	if (!libpq_options)			/* assume reason for failure is OOM */
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Could not get libpq's default connection options.")));

	/* Count how many libpq options are available.
	 *
	 * 计算有多少个 libpq 选项可用。
	 */
	num_libpq_opts = 0;
	for (lopt = libpq_options; lopt->keyword; lopt++)
		num_libpq_opts++;

	/*
	 * Construct an array which consists of all valid options for
	 * postgres_fdw, by appending FDW-specific options to libpq options.
	 *
	 * 通过将 FDW 特定选项附加到 libpq 选项，构造一个由 postgres_fdw 的所有有效选项组成的数组。
	 *
	 * We use plain malloc here to allocate postgres_fdw_options because it
	 * lives as long as the backend process does.  Besides, keeping
	 * libpq_options in memory allows us to avoid copying every keyword
	 * string.
	 *
	 * 我们在这里使用普通的 malloc 来分配 postgres_fdw_options，因为它的生命周期与后端进程的生命周期一样长。  此外，将 libpq_options 保留在内存中可以让我们避免复制每个关键字字符串。
	 */
	postgres_fdw_options = (PgFdwOption *)
		malloc(sizeof(PgFdwOption) * num_libpq_opts +
			   sizeof(non_libpq_options));
	if (postgres_fdw_options == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	popt = postgres_fdw_options;
	for (lopt = libpq_options; lopt->keyword; lopt++)
	{
		/* Hide debug options, as well as settings we override internally.
		 *
		 * 隐藏调试选项以及我们在内部覆盖的设置。
		 */
		if (strchr(lopt->dispchar, 'D') ||
			strcmp(lopt->keyword, "fallback_application_name") == 0 ||
			strcmp(lopt->keyword, "client_encoding") == 0)
			continue;

		/*
		 * Disallow OAuth options for now, since the builtin flow communicates
		 * on stderr by default and can't cache tokens yet.
		 *
		 * 目前禁止 OAuth 选项，因为默认情况下内置流在 stderr 上进行通信并且尚无法缓存令牌。
		 */
		if (strncmp(lopt->keyword, "oauth_", strlen("oauth_")) == 0)
			continue;

		/* We don't have to copy keyword string, as described above.
		 *
		 * 我们不必复制关键字字符串，如上所述。
		 */
		popt->keyword = lopt->keyword;

		/*
		 * "user" and any secret options are allowed only on user mappings.
		 * Everything else is a server option.
		 *
		 * 仅在用户映射上允许“user”和任何秘密选项。其他一切都是服务器选项。
		 */
		if (strcmp(lopt->keyword, "user") == 0 || strchr(lopt->dispchar, '*'))
			popt->optcontext = UserMappingRelationId;
		else
			popt->optcontext = ForeignServerRelationId;
		popt->is_libpq_opt = true;

		popt++;
	}

	/* Append FDW-specific options and dummy terminator.
	 *
	 * 附加 FDW 特定选项和虚拟终止符。
	 */
	memcpy(popt, non_libpq_options, sizeof(non_libpq_options));
}

/*
 * Check whether the given option is one of the valid postgres_fdw options.
 * context is the Oid of the catalog holding the object the option is for.
 *
 * 检查给定选项是否是有效的 postgres_fdw 选项之一。 context 是保存选项所属对象的目录的 Oid。
 */
static bool
is_valid_option(const char *keyword, Oid context)
{
	PgFdwOption *opt;

	Assert(postgres_fdw_options);	/* must be initialized already */

	for (opt = postgres_fdw_options; opt->keyword; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->keyword, keyword) == 0)
			return true;
	}

	return false;
}

/*
 * Check whether the given option is one of the valid libpq options.
 *
 * 检查给定选项是否是有效的 libpq 选项之一。
 */
static bool
is_libpq_option(const char *keyword)
{
	PgFdwOption *opt;

	Assert(postgres_fdw_options);	/* must be initialized already */

	for (opt = postgres_fdw_options; opt->keyword; opt++)
	{
		if (opt->is_libpq_opt && strcmp(opt->keyword, keyword) == 0)
			return true;
	}

	return false;
}

/*
 * Generate key-value arrays which include only libpq options from the
 * given list (which can contain any kind of options).  Caller must have
 * allocated large-enough arrays.  Returns number of options found.
 *
 * 生成键值数组，其中仅包含给定列表中的 libpq 选项（可以包含任何类型的选项）。  调用者必须分配足够大的数组。  返回找到的选项数量。
 */
int
ExtractConnectionOptions(List *defelems, const char **keywords,
						 const char **values)
{
	ListCell   *lc;
	int			i;

	/* Build our options lists if we didn't yet.
	 *
	 * 如果我们还没有的话，请建立我们的选项列表。
	 */
	InitPgFdwOptions();

	i = 0;
	foreach(lc, defelems)
	{
		DefElem    *d = (DefElem *) lfirst(lc);

		if (is_libpq_option(d->defname))
		{
			keywords[i] = d->defname;
			values[i] = defGetString(d);
			i++;
		}
	}
	return i;
}

/*
 * Parse a comma-separated string and return a List of the OIDs of the
 * extensions named in the string.  If any names in the list cannot be
 * found, report a warning if warnOnMissing is true, else just silently
 * ignore them.
 *
 * 解析逗号分隔的字符串并返回字符串中指定的扩展的 OID 列表。  如果在列表中找不到任何名称，则在 warnOnMissing 为 true 的情况下报告警告，否则只是默默地忽略它们。
 */
List *
ExtractExtensionList(const char *extensionsString, bool warnOnMissing)
{
	List	   *extensionOids = NIL;
	List	   *extlist;
	ListCell   *lc;

	/* SplitIdentifierString scribbles on its input, so pstrdup first
	 *
	 * SplitIdentifierString 在其输入上乱写乱画，因此首先 pstrdup
	 */
	if (!SplitIdentifierString(pstrdup(extensionsString), ',', &extlist))
	{
		/* syntax error in name list
		 *
		 * 名称列表中的语法错误
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" must be a list of extension names",
						"extensions")));
	}

	foreach(lc, extlist)
	{
		const char *extension_name = (const char *) lfirst(lc);
		Oid			extension_oid = get_extension_oid(extension_name, true);

		if (OidIsValid(extension_oid))
		{
			extensionOids = lappend_oid(extensionOids, extension_oid);
		}
		else if (warnOnMissing)
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("extension \"%s\" is not installed",
							extension_name)));
		}
	}

	list_free(extlist);
	return extensionOids;
}

/*
 * Replace escape sequences beginning with % character in the given
 * application_name with status information, and return it.
 *
 * 将给定 application_name 中以 % 字符开头的转义序列替换为状态信息，然后返回它。
 *
 * This function always returns a palloc'd string, so the caller is
 * responsible for pfreeing it.
 *
 * 该函数始终返回一个已分配的字符串，因此调用者负责释放它。
 */
char *
process_pgfdw_appname(const char *appname)
{
	const char *p;
	StringInfoData buf;

	initStringInfo(&buf);

	for (p = appname; *p != '\0'; p++)
	{
		if (*p != '%')
		{
			/* literal char, just copy
			 *
			 * 文字字符，只需复制
			 */
			appendStringInfoChar(&buf, *p);
			continue;
		}

		/* must be a '%', so skip to the next char
		 *
		 * 必须是“%”，因此跳到下一个字符
		 */
		p++;
		if (*p == '\0')
			break;				/* format error - ignore it */
		else if (*p == '%')
		{
			/* string contains %%
			 *
			 * 字符串包含%%
			 */
			appendStringInfoChar(&buf, '%');
			continue;
		}

		/* process the option
		 *
		 * 处理选项
		 */
		switch (*p)
		{
			case 'a':
				appendStringInfoString(&buf, application_name);
				break;
			case 'c':
				appendStringInfo(&buf, "%" PRIx64 ".%x", MyStartTime, MyProcPid);
				break;
			case 'C':
				appendStringInfoString(&buf, cluster_name);
				break;
			case 'd':
				if (MyProcPort)
				{
					const char *dbname = MyProcPort->database_name;

					if (dbname)
						appendStringInfoString(&buf, dbname);
					else
						appendStringInfoString(&buf, "[unknown]");
				}
				break;
			case 'p':
				appendStringInfo(&buf, "%d", MyProcPid);
				break;
			case 'u':
				if (MyProcPort)
				{
					const char *username = MyProcPort->user_name;

					if (username)
						appendStringInfoString(&buf, username);
					else
						appendStringInfoString(&buf, "[unknown]");
				}
				break;
			default:
				/* format error - ignore it
				 *
				 * 格式错误 - 忽略它
				 */
				break;
		}
	}

	return buf.data;
}

/*
 * Module load callback
 *
 * 模块加载回调
 */
void
_PG_init(void)
{
	/*
	 * Unlike application_name GUC, don't set GUC_IS_NAME flag nor check_hook
	 * to allow postgres_fdw.application_name to be any string more than
	 * NAMEDATALEN characters and to include non-ASCII characters. Instead,
	 * remote server truncates application_name of remote connection to less
	 * than NAMEDATALEN and replaces any non-ASCII characters in it with a '?'
	 * character.
	 *
	 * 与 application_name GUC 不同，不要设置 GUC_IS_NAME 标志或 check_hook 以允许 postgres_fdw.application_name 为多于 NAMEDATALEN 字符的任何字符串并包含非 ASCII 字符。相反，远程服务器会将远程连接的 application_name 截断为小于 NAMEDATALEN，并用“?”替换其中的任何非 ASCII 字符。特点。
	 */
	DefineCustomStringVariable("postgres_fdw.application_name",
							   "Sets the application name to be used on the remote server.",
							   NULL,
							   &pgfdw_application_name,
							   NULL,
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("postgres_fdw");
}
