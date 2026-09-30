/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Routines for handling specialized SET variables.
 *
 * 处理特殊 SET 变量的例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "catalog/pg_authid.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/guc_hooks.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tzparser.h"
#include "utils/varlena.h"

/*
 * 核心流程概览：
 * 本文件实现若干特殊 GUC 的 check_hook、assign_hook 与 show_hook。
 * check_* 校验新值并可写入 extra；assign_* 在赋值时生效（DateStyle、TimeZone、
 * client_encoding、session_authorization、role 等）；show_* 按展示格式返回当前值。
 * 事务类 GUC（transaction_read_only、transaction_isolation、transaction_deferrable）
 * 只在允许的事务状态下接受变更。
 */
/*
 * DATESTYLE
 *
 * DATESTYLE。
 */

/*
 * check_datestyle: GUC check_hook for datestyle
 *
 * check_datestyle：datestyle 的 GUC check_hook。
 */
bool
check_datestyle(char **newval, void **extra, GucSource source)
{
	int			newDateStyle = DateStyle;
	int			newDateOrder = DateOrder;
	bool		have_style = false;
	bool		have_order = false;
	bool		ok = true;
	char	   *rawstring;
	int		   *myextra;
	char	   *result;
	List	   *elemlist;
	ListCell   *l;

	/* Need a modifiable copy of string */
	/*
	 *
	 * 需要一份可修改的字符串副本。
	 */
	rawstring = pstrdup(*newval);

	/* Parse string into list of identifiers */
	/*
	 *
	 * 把字符串解析成标识符列表。
	 */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		/*
		 *
		 * 列表有语法错误。
		 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		/* Ugh. Somebody ought to write a table driven version -- mjl */
		/*
		 *
		 * 最好改成表驱动的实现。
		 */

		if (pg_strcasecmp(tok, "ISO") == 0)
		{
			if (have_style && newDateStyle != USE_ISO_DATES)
				ok = false;		/* conflicting styles */
				/*
				 *
				 * 风格互相冲突。
				 */
			newDateStyle = USE_ISO_DATES;
			have_style = true;
		}
		else if (pg_strcasecmp(tok, "SQL") == 0)
		{
			if (have_style && newDateStyle != USE_SQL_DATES)
				ok = false;		/* conflicting styles */
				/*
				 *
				 * 风格互相冲突。
				 */
			newDateStyle = USE_SQL_DATES;
			have_style = true;
		}
		else if (pg_strncasecmp(tok, "POSTGRES", 8) == 0)
		{
			if (have_style && newDateStyle != USE_POSTGRES_DATES)
				ok = false;		/* conflicting styles */
				/*
				 *
				 * 风格互相冲突。
				 */
			newDateStyle = USE_POSTGRES_DATES;
			have_style = true;
		}
		else if (pg_strcasecmp(tok, "GERMAN") == 0)
		{
			if (have_style && newDateStyle != USE_GERMAN_DATES)
				ok = false;		/* conflicting styles */
				/*
				 *
				 * 风格互相冲突。
				 */
			newDateStyle = USE_GERMAN_DATES;
			have_style = true;
			/* GERMAN also sets DMY, unless explicitly overridden */
			/*
			 *
			 * GERMAN 同时设置 DMY，除非被显式覆盖。
			 */
			if (!have_order)
				newDateOrder = DATEORDER_DMY;
		}
		else if (pg_strcasecmp(tok, "YMD") == 0)
		{
			if (have_order && newDateOrder != DATEORDER_YMD)
				ok = false;		/* conflicting orders */
				/*
				 *
				 * 日期顺序互相冲突。
				 */
			newDateOrder = DATEORDER_YMD;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "DMY") == 0 ||
				 pg_strncasecmp(tok, "EURO", 4) == 0)
		{
			if (have_order && newDateOrder != DATEORDER_DMY)
				ok = false;		/* conflicting orders */
				/*
				 *
				 * 日期顺序互相冲突。
				 */
			newDateOrder = DATEORDER_DMY;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "MDY") == 0 ||
				 pg_strcasecmp(tok, "US") == 0 ||
				 pg_strncasecmp(tok, "NONEURO", 7) == 0)
		{
			if (have_order && newDateOrder != DATEORDER_MDY)
				ok = false;		/* conflicting orders */
				/*
				 *
				 * 日期顺序互相冲突。
				 */
			newDateOrder = DATEORDER_MDY;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "DEFAULT") == 0)
		{
			/*
			 * Easiest way to get the current DEFAULT state is to fetch the
			 * DEFAULT string from guc.c and recursively parse it.
			 *
			 * 取得当前 DEFAULT 状态的最简办法是从 guc.c 取出 DEFAULT 字符串并递归解析。
			 *
			 * We can't simply "return check_datestyle(...)" because we need
			 * to handle constructs like "DEFAULT, ISO".
			 *
			 * 不能简单 return check_datestyle(...)，因为要处理 DEFAULT, ISO 这样的写法。
			 */
			char	   *subval;
			void	   *subextra = NULL;

			subval = guc_strdup(LOG, GetConfigOptionResetString("datestyle"));
			if (!subval)
			{
				ok = false;
				break;
			}
			if (!check_datestyle(&subval, &subextra, source))
			{
				guc_free(subval);
				ok = false;
				break;
			}
			myextra = (int *) subextra;
			if (!have_style)
				newDateStyle = myextra[0];
			if (!have_order)
				newDateOrder = myextra[1];
			guc_free(subval);
			guc_free(subextra);
		}
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	if (!ok)
	{
		GUC_check_errdetail("Conflicting \"DateStyle\" specifications.");
		return false;
	}

	/*
	 * Prepare the canonical string to return.  GUC wants it guc_malloc'd.
	 *
	 * 准备要返回的规范字符串。GUC 要求用 guc_malloc 分配。
	 */
	result = (char *) guc_malloc(LOG, 32);
	if (!result)
		return false;

	switch (newDateStyle)
	{
		case USE_ISO_DATES:
			strcpy(result, "ISO");
			break;
		case USE_SQL_DATES:
			strcpy(result, "SQL");
			break;
		case USE_GERMAN_DATES:
			strcpy(result, "German");
			break;
		default:
			strcpy(result, "Postgres");
			break;
	}
	switch (newDateOrder)
	{
		case DATEORDER_YMD:
			strcat(result, ", YMD");
			break;
		case DATEORDER_DMY:
			strcat(result, ", DMY");
			break;
		default:
			strcat(result, ", MDY");
			break;
	}

	guc_free(*newval);
	*newval = result;

	/*
	 * Set up the "extra" struct actually used by assign_datestyle.
	 *
	 * 设置 assign_datestyle 实际使用的 extra 结构。
	 */
	myextra = (int *) guc_malloc(LOG, 2 * sizeof(int));
	if (!myextra)
		return false;
	myextra[0] = newDateStyle;
	myextra[1] = newDateOrder;
	*extra = myextra;

	return true;
}

/*
 * assign_datestyle: GUC assign_hook for datestyle
 *
 * assign_datestyle：datestyle 的 GUC assign_hook。
 */
void
assign_datestyle(const char *newval, void *extra)
{
	int		   *myextra = (int *) extra;

	DateStyle = myextra[0];
	DateOrder = myextra[1];
}


/*
 * TIMEZONE
 *
 * TIMEZONE。
 */

/*
 * check_timezone: GUC check_hook for timezone
 *
 * check_timezone：timezone 的 GUC check_hook。
 */
bool
check_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;
	long		gmtoffset;
	char	   *endptr;
	double		hours;

	if (pg_strncasecmp(*newval, "interval", 8) == 0)
	{
		/*
		 * Support INTERVAL 'foo'.  This is for SQL spec compliance, not
		 * because it has any actual real-world usefulness.
		 *
		 * 支持 INTERVAL 'foo'。这是为了符合 SQL 标准，并不是因为它真有实际用处。
		 */
		const char *valueptr = *newval;
		char	   *val;
		Interval   *interval;

		valueptr += 8;
		while (isspace((unsigned char) *valueptr))
			valueptr++;
		if (*valueptr++ != '\'')
			return false;
		val = pstrdup(valueptr);
		/* Check and remove trailing quote */
		/*
		 *
		 * 检查并去掉末尾引号。
		 */
		endptr = strchr(val, '\'');
		if (!endptr || endptr[1] != '\0')
		{
			pfree(val);
			return false;
		}
		*endptr = '\0';

		/*
		 * Try to parse it.  XXX an invalid interval format will result in
		 * ereport(ERROR), which is not desirable for GUC.  We did what we
		 * could to guard against this in flatten_set_variable_args, but a
		 * string coming in from postgresql.conf might contain anything.
		 *
		 * 尝试解析。XXX 非法的 interval 格式会 ereport(ERROR)，对 GUC 并不理想。
		 * flatten_set_variable_args 已尽量防范，但 postgresql.conf 里的字符串可能是任意内容。
		 */
		interval = DatumGetIntervalP(DirectFunctionCall3(interval_in,
														 CStringGetDatum(val),
														 ObjectIdGetDatum(InvalidOid),
														 Int32GetDatum(-1)));

		pfree(val);
		if (interval->month != 0)
		{
			GUC_check_errdetail("Cannot specify months in time zone interval.");
			pfree(interval);
			return false;
		}
		if (interval->day != 0)
		{
			GUC_check_errdetail("Cannot specify days in time zone interval.");
			pfree(interval);
			return false;
		}

		/* Here we change from SQL to Unix sign convention */
		/*
		 *
		 * 这里把 SQL 的符号约定换成 Unix 的。
		 */
		gmtoffset = -(interval->time / USECS_PER_SEC);
		new_tz = pg_tzset_offset(gmtoffset);

		pfree(interval);
	}
	else
	{
		/*
		 * Try it as a numeric number of hours (possibly fractional).
		 *
		 * 尝试把它当作小时数（可以是小数）。
		 */
		hours = strtod(*newval, &endptr);
		if (endptr != *newval && *endptr == '\0')
		{
			/* Here we change from SQL to Unix sign convention */
			/*
			 *
			 * 这里把 SQL 的符号约定换成 Unix 的。
			 */
			gmtoffset = -hours * SECS_PER_HOUR;
			new_tz = pg_tzset_offset(gmtoffset);
		}
		else
		{
			/*
			 * Otherwise assume it is a timezone name, and try to load it.
			 *
			 * 否则假定它是时区名，并尝试加载。
			 */
			new_tz = pg_tzset(*newval);

			if (!new_tz)
			{
				/* Doesn't seem to be any great value in errdetail here */
				/*
				 *
				 * 这里加 errdetail 似乎没什么价值。
				 */
				return false;
			}

			if (!pg_tz_acceptable(new_tz))
			{
				GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
								 *newval);
				GUC_check_errdetail("PostgreSQL does not support leap seconds.");
				return false;
			}
		}
	}

	/* Test for failure in pg_tzset_offset, which we assume is out-of-range */
	/*
	 *
	 * 检查 pg_tzset_offset 是否失败，失败则视为超出范围。
	 */
	if (!new_tz)
	{
		GUC_check_errdetail("UTC timezone offset is out of range.");
		return false;
	}

	/*
	 * Pass back data for assign_timezone to use
	 *
	 * 把数据传回给 assign_timezone 使用。
	 */
	*extra = guc_malloc(LOG, sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_timezone: GUC assign_hook for timezone
 *
 * assign_timezone：timezone 的 GUC assign_hook。
 */
void
assign_timezone(const char *newval, void *extra)
{
	session_timezone = *((pg_tz **) extra);
	/* datetime.c's cache of timezone abbrevs may now be obsolete */
	/*
	 *
	 * datetime.c 中的时区缩写缓存现在可能已过时。
	 */
	ClearTimeZoneAbbrevCache();
}

/*
 * show_timezone: GUC show_hook for timezone
 *
 * show_timezone：timezone 的 GUC show_hook。
 */
const char *
show_timezone(void)
{
	const char *tzn;

	/* Always show the zone's canonical name */
	/*
	 *
	 * 始终显示时区的规范名称。
	 */
	tzn = pg_get_timezone_name(session_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * LOG_TIMEZONE
 *
 * LOG_TIMEZONE。
 *
 * For log_timezone, we don't support the interval-based methods of setting a
 * zone, which are only there for SQL spec compliance not because they're
 * actually useful.
 *
 * 对 log_timezone，不支持用 interval 设置时区；那些方式只是为了符合 SQL 标准，并没有实际用处。
 */

/*
 * check_log_timezone: GUC check_hook for log_timezone
 *
 * check_log_timezone：log_timezone 的 GUC check_hook。
 */
bool
check_log_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;

	/*
	 * Assume it is a timezone name, and try to load it.
	 *
	 * 假定它是时区名，并尝试加载。
	 */
	new_tz = pg_tzset(*newval);

	if (!new_tz)
	{
		/* Doesn't seem to be any great value in errdetail here */
		/*
		 *
		 * 这里加 errdetail 似乎没什么价值。
		 */
		return false;
	}

	if (!pg_tz_acceptable(new_tz))
	{
		GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
						 *newval);
		GUC_check_errdetail("PostgreSQL does not support leap seconds.");
		return false;
	}

	/*
	 * Pass back data for assign_log_timezone to use
	 *
	 * 把数据传回给 assign_log_timezone 使用。
	 */
	*extra = guc_malloc(LOG, sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_log_timezone: GUC assign_hook for log_timezone
 *
 * assign_log_timezone：log_timezone 的 GUC assign_hook。
 */
void
assign_log_timezone(const char *newval, void *extra)
{
	log_timezone = *((pg_tz **) extra);
}

/*
 * show_log_timezone: GUC show_hook for log_timezone
 *
 * show_log_timezone：log_timezone 的 GUC show_hook。
 */
const char *
show_log_timezone(void)
{
	const char *tzn;

	/* Always show the zone's canonical name */
	/*
	 *
	 * 始终显示时区的规范名称。
	 */
	tzn = pg_get_timezone_name(log_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * TIMEZONE_ABBREVIATIONS
 *
 * TIMEZONE_ABBREVIATIONS。
 */

/*
 * GUC check_hook for timezone_abbreviations
 *
 * timezone_abbreviations 的 GUC check_hook。
 */
bool
check_timezone_abbreviations(char **newval, void **extra, GucSource source)
{
	/*
	 * The boot_val for timezone_abbreviations is NULL.  When we see that we
	 * just do nothing.  If the value isn't overridden from the config file
	 * then pg_timezone_abbrev_initialize() will eventually replace it with
	 * "Default".  This hack has two purposes: to avoid wasting cycles loading
	 * values that might soon be overridden from the config file, and to avoid
	 * trying to read the timezone abbrev files during InitializeGUCOptions().
	 * The latter doesn't work in an EXEC_BACKEND subprocess because
	 * my_exec_path hasn't been set yet and so we can't locate PGSHAREDIR.
	 *
	 * timezone_abbreviations 的 boot_val 是 NULL。见到它就什么也不做。
	 * 若配置文件没有覆盖，pg_timezone_abbrev_initialize() 最终会把它换成 Default。
	 * 这样做有两个目的：避免加载很快会被配置覆盖的值，以及避免在 InitializeGUCOptions() 期间读时区缩写文件。
	 * 后者在 EXEC_BACKEND 子进程中行不通，因为 my_exec_path 尚未设置，找不到 PGSHAREDIR。
	 */
	if (*newval == NULL)
	{
		Assert(source == PGC_S_DEFAULT);
		return true;
	}

	/* OK, load the file and produce a guc_malloc'd TimeZoneAbbrevTable */
	/*
	 *
	 * 装载文件，并用 guc_malloc 生成 TimeZoneAbbrevTable。
	 */
	*extra = load_tzoffsets(*newval);

	/* tzparser.c returns NULL on failure, reporting via GUC_check_errmsg */
	/*
	 *
	 * 失败时 tzparser.c 返回 NULL，并通过 GUC_check_errmsg 报告。
	 */
	if (!*extra)
		return false;

	return true;
}

/*
 * GUC assign_hook for timezone_abbreviations
 *
 * timezone_abbreviations 的 GUC assign_hook。
 */
void
assign_timezone_abbreviations(const char *newval, void *extra)
{
	/* Do nothing for the boot_val default of NULL */
	/*
	 *
	 * boot_val 默认的 NULL 不做任何事。
	 */
	if (!extra)
		return;

	InstallTimeZoneAbbrevs((TimeZoneAbbrevTable *) extra);
}


/*
 * SET TRANSACTION READ ONLY and SET TRANSACTION READ WRITE
 *
 * SET TRANSACTION READ ONLY 与 SET TRANSACTION READ WRITE。
 *
 * We allow idempotent changes (r/w -> r/w and r/o -> r/o) at any time, and
 * we also always allow changes from read-write to read-only.  However,
 * read-only may be changed to read-write only when in a top-level transaction
 * that has not yet taken an initial snapshot.  Can't do it in a hot standby,
 * either.
 *
 * 幂等变更（读写到读写、只读到只读）随时允许，从读写改为只读也始终允许。
 * 但从只读改为读写只能在尚未取得初始快照的顶层事务中进行，热备中也不行。
 *
 * If we are not in a transaction at all, just allow the change; it means
 * nothing since XactReadOnly will be reset by the next StartTransaction().
 * The IsTransactionState() test protects us against trying to check
 * RecoveryInProgress() in contexts where shared memory is not accessible.
 * (Similarly, if we're restoring state in a parallel worker, just allow
 * the change.)
 *
 * 若不在任何事务中，则允许变更；下次 StartTransaction() 会重置 XactReadOnly，所以没有实际效果。
 * IsTransactionState() 用来避免在无法访问共享内存时检查 RecoveryInProgress()。
 * 在并行 worker 中恢复状态时同样直接允许。
 */
bool
check_transaction_read_only(bool *newval, void **extra, GucSource source)
{
	if (*newval == false && XactReadOnly && IsTransactionState() && !InitializingParallelWorker)
	{
		/* Can't go to r/w mode inside a r/o transaction */
		/*
		 *
		 * 不能在只读事务中切换到读写模式。
		 */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("cannot set transaction read-write mode inside a read-only transaction");
			return false;
		}
		/* Top level transaction can't change to r/w after first snapshot. */
		/*
		 *
		 * 顶层事务在取得第一个快照后不能改为读写。
		 */
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("transaction read-write mode must be set before any query");
			return false;
		}
		/* Can't go to r/w mode while recovery is still active */
		/*
		 *
		 * 恢复尚未结束时不能切换到读写模式。
		 */
		if (RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot set transaction read-write mode during recovery");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION ISOLATION LEVEL
 *
 * SET TRANSACTION ISOLATION LEVEL。
 *
 * We allow idempotent changes at any time, but otherwise this can only be
 * changed in a toplevel transaction that has not yet taken a snapshot.
 *
 * 幂等变更随时允许；其他情况下只能在尚未取得快照的顶层事务中修改。
 *
 * As in check_transaction_read_only, allow it if not inside a transaction,
 * or if restoring state in a parallel worker.
 *
 * 与 check_transaction_read_only 一样，不在事务中或正在并行 worker 中恢复状态时允许。
 */
bool
check_transaction_isolation(int *newval, void **extra, GucSource source)
{
	int			newXactIsoLevel = *newval;

	if (newXactIsoLevel != XactIsoLevel &&
		IsTransactionState() && !InitializingParallelWorker)
	{
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query");
			return false;
		}
		/* We ignore a subtransaction setting it to the existing value. */
		/*
		 *
		 * 子事务把它设成当前值时忽略。
		 */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction");
			return false;
		}
		/* Can't go to serializable mode while recovery is still active */
		/*
		 *
		 * 恢复尚未结束时不能切换到 serializable。
		 */
		if (newXactIsoLevel == XACT_SERIALIZABLE && RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot use serializable mode in a hot standby");
			GUC_check_errhint("You can use REPEATABLE READ instead.");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION [NOT] DEFERRABLE
 *
 * SET TRANSACTION [NOT] DEFERRABLE。
 */

bool
check_transaction_deferrable(bool *newval, void **extra, GucSource source)
{
	/* Just accept the value when restoring state in a parallel worker */
	/*
	 *
	 * 在并行 worker 中恢复状态时直接接受该值。
	 */
	if (InitializingParallelWorker)
		return true;

	if (IsSubTransaction())
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE cannot be called within a subtransaction");
		return false;
	}
	if (FirstSnapshotSet)
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE must be called before any query");
		return false;
	}

	return true;
}

/*
 * Random number seed
 *
 * 随机数种子。
 *
 * We can't roll back the random sequence on error, and we don't want
 * config file reloads to affect it, so we only want interactive SET SEED
 * commands to set it.  We use the "extra" storage to ensure that rollbacks
 * don't try to do the operation again.
 *
 * 出错时无法回滚随机序列，也不希望配置重载影响它，因此只有交互式 SET SEED 才会设置。
 * 用 extra 存储保证回滚不会再次执行该操作。
 */

bool
check_random_seed(double *newval, void **extra, GucSource source)
{
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	/* Arm the assign only if source of value is an interactive SET */
	/*
	 *
	 * 仅当值来自交互式 SET 时才武装 assign。
	 */
	*((int *) *extra) = (source >= PGC_S_INTERACTIVE);

	return true;
}

/*
 * 在交互式 SET SEED 时调用 setseed；extra 保证回滚不会再次执行。
 */
void
assign_random_seed(double newval, void *extra)
{
	/* We'll do this at most once for any setting of the GUC variable */
	/*
	 *
	 * 对该 GUC 的任意一次设置，最多执行一次。
	 */
	if (*((int *) extra))
		DirectFunctionCall1(setseed, Float8GetDatum(newval));
	*((int *) extra) = 0;
}

/*
 * SHOW seed 固定返回 unavailable，随机序列无法回读。
 */
const char *
show_random_seed(void)
{
	return "unavailable";
}


/*
 * SET CLIENT_ENCODING
 *
 * SET CLIENT_ENCODING。
 */

bool
check_client_encoding(char **newval, void **extra, GucSource source)
{
	int			encoding;
	const char *canonical_name;

	/* Look up the encoding by name */
	/*
	 *
	 * 按名称查找编码。
	 */
	encoding = pg_valid_client_encoding(*newval);
	if (encoding < 0)
		return false;

	/* Get the canonical name (no aliases, uniform case) */
	/*
	 *
	 * 取得规范名称（无别名，大小写统一）。
	 */
	canonical_name = pg_encoding_to_char(encoding);

	/*
	 * Parallel workers send data to the leader, not the client.  They always
	 * send data using the database encoding; therefore, we should never
	 * actually change the client encoding in a parallel worker.  However,
	 * during parallel worker startup, we want to accept the leader's
	 * client_encoding setting so that anyone who looks at the value in the
	 * worker sees the same value that they would see in the leader.  A change
	 * other than during startup, for example due to a SET clause attached to
	 * a function definition, should be rejected, as there is nothing we can
	 * do inside the worker to make it take effect.
	 *
	 * 并行 worker 把数据发给 leader 而不是客户端，并且始终使用数据库编码，因此不应真正改变 worker 的客户端编码。
	 * 但在 worker 启动期间要接受 leader 的 client_encoding，使查看该值的人看到与 leader 相同的值。
	 * 启动之外的变更（例如函数定义上的 SET 子句）应拒绝，因为 worker 内无法让它生效。
	 */
	if (IsParallelWorker() && !InitializingParallelWorker)
	{
		GUC_check_errcode(ERRCODE_INVALID_TRANSACTION_STATE);
		GUC_check_errdetail("Cannot change \"client_encoding\" during a parallel operation.");
		return false;
	}

	/*
	 * If we are not within a transaction then PrepareClientEncoding will not
	 * be able to look up the necessary conversion procs.  If we are still
	 * starting up, it will return "OK" anyway, and InitializeClientEncoding
	 * will fix things once initialization is far enough along.  After
	 * startup, we'll fail.  This would only happen if someone tries to change
	 * client_encoding in postgresql.conf and then SIGHUP existing sessions.
	 * It seems like a bad idea for client_encoding to change that way anyhow,
	 * so we don't go out of our way to support it.
	 *
	 * 若不在事务中，PrepareClientEncoding 无法查找所需的转换过程。启动过程中它仍会返回 OK，
	 * 等初始化足够靠后时由 InitializeClientEncoding 修正。启动之后则会失败。
	 * 这只会发生在有人在 postgresql.conf 里改 client_encoding 再 SIGHUP 现有会话时。
	 * 这样改变 client_encoding 本来就不好，因此不特意支持。
	 *
	 * In a parallel worker, we might as well skip PrepareClientEncoding since
	 * we're not going to use its results.
	 *
	 * 在并行 worker 中反正不用其结果，可以跳过 PrepareClientEncoding。
	 *
	 * Note: in the postmaster, or any other process that never calls
	 * InitializeClientEncoding, PrepareClientEncoding will always succeed,
	 * and so will SetClientEncoding; but they won't do anything, which is OK.
	 *
	 * 注意：在 postmaster 或其他从不调用 InitializeClientEncoding 的进程中，
	 * PrepareClientEncoding 和 SetClientEncoding 总会成功，但什么也不做，这是可以的。
	 */
	if (!IsParallelWorker() &&
		PrepareClientEncoding(encoding) < 0)
	{
		if (IsTransactionState())
		{
			/* Must be a genuine no-such-conversion problem */
			/*
			 *
			 * 这必定是真的不存在该转换。
			 */
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errdetail("Conversion between %s and %s is not supported.",
								canonical_name,
								GetDatabaseEncodingName());
		}
		else
		{
			/* Provide a useful complaint */
			/*
			 *
			 * 给出有用的报错。
			 */
			GUC_check_errdetail("Cannot change \"client_encoding\" now.");
		}
		return false;
	}

	/*
	 * Replace the user-supplied string with the encoding's canonical name.
	 * This gets rid of aliases and case-folding variations.
	 *
	 * 用编码的规范名称替换用户给出的字符串，以去掉别名和大小写差异。
	 *
	 * XXX Although canonicalizing seems like a good idea in the abstract, it
	 * breaks pre-9.1 JDBC drivers, which expect that if they send "UNICODE"
	 * as the client_encoding setting then it will read back the same way. As
	 * a workaround, don't replace the string if it's "UNICODE".  Remove that
	 * hack when pre-9.1 JDBC drivers are no longer in use.
	 *
	 * XXX 规范化抽象上看是好事，但会破坏 9.1 之前的 JDBC 驱动：它们发送 UNICODE 作为 client_encoding 时期望读回来仍是 UNICODE。
	 * 权宜之计是字符串为 UNICODE 时不替换。等这些驱动不再使用后再去掉这个特例。
	 */
	if (strcmp(*newval, canonical_name) != 0 &&
		strcmp(*newval, "UNICODE") != 0)
	{
		guc_free(*newval);
		*newval = guc_strdup(LOG, canonical_name);
		if (!*newval)
			return false;
	}

	/*
	 * Save the encoding's ID in *extra, for use by assign_client_encoding.
	 *
	 * 把编码 ID 存入 *extra，供 assign_client_encoding 使用。
	 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = encoding;

	return true;
}

/*
 * 按 check 阶段保存的编码 ID 调用 SetClientEncoding；并行 worker 中不覆盖。
 */
void
assign_client_encoding(const char *newval, void *extra)
{
	int			encoding = *((int *) extra);

	/*
	 * In a parallel worker, we never override the client encoding that was
	 * set by ParallelWorkerMain().
	 *
	 * 在并行 worker 中，绝不覆盖 ParallelWorkerMain() 已设置的客户端编码。
	 */
	if (IsParallelWorker())
		return;

	/* We do not expect an error if PrepareClientEncoding succeeded */
	/*
	 *
	 * 若 PrepareClientEncoding 已成功，则不应再出错。
	 */
	if (SetClientEncoding(encoding) < 0)
		elog(LOG, "SetClientEncoding(%d) failed", encoding);
}


/*
 * SET SESSION AUTHORIZATION
 *
 * SET SESSION AUTHORIZATION。
 */

typedef struct
{
	/* This is the "extra" state for both SESSION AUTHORIZATION and ROLE */
	/*
	 *
	 * 这是 SESSION AUTHORIZATION 和 ROLE 共用的 extra 状态。
	 */
	Oid			roleid;
	bool		is_superuser;
} role_auth_extra;

/*
 * 校验 SET SESSION AUTHORIZATION 的角色名，并把角色 OID 写入 extra。
 */
bool
check_session_authorization(char **newval, void **extra, GucSource source)
{
	HeapTuple	roleTup;
	Form_pg_authid roleform;
	Oid			roleid;
	bool		is_superuser;
	role_auth_extra *myextra;

	/* Do nothing for the boot_val default of NULL */
	/*
	 *
	 * boot_val 默认的 NULL 不做任何事。
	 */
	if (*newval == NULL)
		return true;

	if (InitializingParallelWorker)
	{
		/*
		 * In parallel worker initialization, we want to copy the leader's
		 * state even if it no longer matches the catalogs. ParallelWorkerMain
		 * already installed the correct role OID and superuser state.
		 *
		 * 并行 worker 初始化时，即使与目录不再一致，也要复制 leader 的状态。
		 * ParallelWorkerMain 已经装好了正确的角色 OID 和超级用户状态。
		 */
		roleid = GetSessionUserId();
		is_superuser = GetSessionUserIsSuperuser();
	}
	else
	{
		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The result of this is that
			 * session_authorization cannot be set in postgresql.conf, which
			 * seems like a good thing anyway, so we don't work hard to avoid
			 * it.
			 *
			 * 无法查目录，因此失败。结果是不能在 postgresql.conf 里设置 session_authorization。
			 * 这本来就是好事，所以不费力去避免。
			 */
			return false;
		}

		/*
		 * When source == PGC_S_TEST, we don't throw a hard error for a
		 * nonexistent user name or insufficient privileges, only a NOTICE.
		 * See comments in guc.h.
		 *
		 * 当 source 为 PGC_S_TEST 时，用户名不存在或权限不足不抛硬错误，只发 NOTICE。见 guc.h 的注释。
		 */

		/* Look up the username */
		/*
		 *
		 * 查找用户名。
		 */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*newval));
		if (!HeapTupleIsValid(roleTup))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", *newval)));
				return true;
			}
			GUC_check_errmsg("role \"%s\" does not exist", *newval);
			return false;
		}

		roleform = (Form_pg_authid) GETSTRUCT(roleTup);
		roleid = roleform->oid;
		is_superuser = roleform->rolsuper;

		ReleaseSysCache(roleTup);

		/*
		 * Only superusers may SET SESSION AUTHORIZATION a role other than
		 * itself. Note that in case of multiple SETs in a single session, the
		 * original authenticated user's superuserness is what matters.
		 *
		 * 只有超级用户才能 SET SESSION AUTHORIZATION 到自己以外的角色。
		 * 同一会话多次 SET 时，以最初认证用户是否为超级用户为准。
		 */
		if (roleid != GetAuthenticatedUserId() &&
			!superuser_arg(GetAuthenticatedUserId()))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission will be denied to set session authorization \"%s\"",
								*newval)));
				return true;
			}
			GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
			GUC_check_errmsg("permission denied to set session authorization \"%s\"",
							 *newval);
			return false;
		}
	}

	/* Set up "extra" struct for assign_session_authorization to use */
	/*
	 *
	 * 为 assign_session_authorization 准备 extra 结构。
	 */
	myextra = (role_auth_extra *) guc_malloc(LOG, sizeof(role_auth_extra));
	if (!myextra)
		return false;
	myextra->roleid = roleid;
	myextra->is_superuser = is_superuser;
	*extra = myextra;

	return true;
}

/*
 * 把会话用户设为 check_session_authorization 解析出的角色。
 */
void
assign_session_authorization(const char *newval, void *extra)
{
	role_auth_extra *myextra = (role_auth_extra *) extra;

	/* Do nothing for the boot_val default of NULL */
	/*
	 *
	 * boot_val 默认的 NULL 不做任何事。
	 */
	if (!myextra)
		return;

	SetSessionAuthorization(myextra->roleid, myextra->is_superuser);
}


/*
 * SET ROLE
 *
 * SET ROLE。
 *
 * The SQL spec requires "SET ROLE NONE" to unset the role, so we hardwire
 * a translation of "none" to InvalidOid.  Otherwise this is much like
 * SET SESSION AUTHORIZATION.
 *
 * SQL 标准要求 SET ROLE NONE 取消角色，因此把 none 固定翻译成 InvalidOid。
 * 除此之外与 SET SESSION AUTHORIZATION 很像。
 */

bool
check_role(char **newval, void **extra, GucSource source)
{
	HeapTuple	roleTup;
	Oid			roleid;
	bool		is_superuser;
	role_auth_extra *myextra;
	Form_pg_authid roleform;

	if (strcmp(*newval, "none") == 0)
	{
		/* hardwired translation */
		/*
		 *
		 * 固定的翻译。
		 */
		roleid = InvalidOid;
		is_superuser = false;
	}
	else if (InitializingParallelWorker)
	{
		/*
		 * In parallel worker initialization, we want to copy the leader's
		 * state even if it no longer matches the catalogs. ParallelWorkerMain
		 * already installed the correct role OID and superuser state.
		 *
		 * 并行 worker 初始化时，即使与目录不再一致，也要复制 leader 的状态。
		 * ParallelWorkerMain 已经装好了正确的角色 OID 和超级用户状态。
		 */
		roleid = GetCurrentRoleId();
		is_superuser = current_role_is_superuser;
	}
	else
	{
		if (!IsTransactionState())
		{
			/*
			 * Can't do catalog lookups, so fail.  The result of this is that
			 * role cannot be set in postgresql.conf, which seems like a good
			 * thing anyway, so we don't work hard to avoid it.
			 *
			 * 无法查目录，因此失败。结果是不能在 postgresql.conf 里设置 role。
			 * 这本来就是好事，所以不费力去避免。
			 */
			return false;
		}

		/*
		 * When source == PGC_S_TEST, we don't throw a hard error for a
		 * nonexistent user name or insufficient privileges, only a NOTICE.
		 * See comments in guc.h.
		 *
		 * 当 source 为 PGC_S_TEST 时，用户名不存在或权限不足不抛硬错误，只发 NOTICE。见 guc.h 的注释。
		 */

		/* Look up the username */
		/*
		 *
		 * 查找用户名。
		 */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*newval));
		if (!HeapTupleIsValid(roleTup))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", *newval)));
				return true;
			}
			GUC_check_errmsg("role \"%s\" does not exist", *newval);
			return false;
		}

		roleform = (Form_pg_authid) GETSTRUCT(roleTup);
		roleid = roleform->oid;
		is_superuser = roleform->rolsuper;

		ReleaseSysCache(roleTup);

		/* Verify that session user is allowed to become this role */
		/*
		 *
		 * 确认会话用户被允许成为该角色。
		 */
		if (!member_can_set_role(GetSessionUserId(), roleid))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission will be denied to set role \"%s\"",
								*newval)));
				return true;
			}
			GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
			GUC_check_errmsg("permission denied to set role \"%s\"",
							 *newval);
			return false;
		}
	}

	/* Set up "extra" struct for assign_role to use */
	/*
	 *
	 * 为 assign_role 准备 extra 结构。
	 */
	myextra = (role_auth_extra *) guc_malloc(LOG, sizeof(role_auth_extra));
	if (!myextra)
		return false;
	myextra->roleid = roleid;
	myextra->is_superuser = is_superuser;
	*extra = myextra;

	return true;
}

/*
 * 把当前角色设为 check_role 解析出的 roleid。
 */
void
assign_role(const char *newval, void *extra)
{
	role_auth_extra *myextra = (role_auth_extra *) extra;

	SetCurrentRoleId(myextra->roleid, myextra->is_superuser);
}

/*
 * 若未 SET ROLE 则显示 none，否则返回 GUC 中的角色名。
 */
const char *
show_role(void)
{
	/*
	 * Check whether SET ROLE is active; if not return "none".  This is a
	 * kluge to deal with the fact that SET SESSION AUTHORIZATION logically
	 * resets SET ROLE to NONE, but we cannot set the GUC role variable from
	 * assign_session_authorization (because we haven't got enough info to
	 * call set_config_option).
	 *
	 * 检查 SET ROLE 是否生效；若没有则返回 none。这是权宜之计：
	 * SET SESSION AUTHORIZATION 逻辑上会把 SET ROLE 重置为 NONE，
	 * 但 assign_session_authorization 没有足够信息去调用 set_config_option，无法改 GUC role。
	 */
	if (!OidIsValid(GetCurrentRoleId()))
		return "none";

	/* Otherwise we can just use the GUC string */
	/*
	 *
	 * 否则直接使用 GUC 字符串。
	 */
	return role_string ? role_string : "none";
}


/*
 * PATH VARIABLES
 *
 * 路径变量。
 *
 * check_canonical_path is used for log_directory and some other GUCs where
 * all we want to do is canonicalize the represented path name.
 *
 * check_canonical_path 用于 log_directory 等 GUC，只需要把路径名规范化。
 */

bool
check_canonical_path(char **newval, void **extra, GucSource source)
{
	/*
	 * Since canonicalize_path never enlarges the string, we can just modify
	 * newval in-place.  But watch out for NULL, which is the default value
	 * for external_pid_file.
	 *
	 * canonicalize_path 从不会把字符串变长，因此可以直接原地修改 newval。
	 * 但要注意 NULL，它是 external_pid_file 的默认值。
	 */
	if (*newval)
		canonicalize_path(*newval);
	return true;
}


/*
 * MISCELLANEOUS
 *
 * 杂项。
 */

/*
 * GUC check_hook for application_name
 *
 * application_name 的 GUC check_hook。
 */
bool
check_application_name(char **newval, void **extra, GucSource source)
{
	char	   *clean;
	char	   *ret;

	/* Only allow clean ASCII chars in the application name */
	/*
	 *
	 * application_name 只允许干净的 ASCII 字符。
	 */
	clean = pg_clean_ascii(*newval, MCXT_ALLOC_NO_OOM);
	if (!clean)
		return false;

	ret = guc_strdup(LOG, clean);
	if (!ret)
	{
		pfree(clean);
		return false;
	}

	guc_free(*newval);

	pfree(clean);
	*newval = ret;
	return true;
}

/*
 * GUC assign_hook for application_name
 *
 * application_name 的 GUC assign_hook。
 */
void
assign_application_name(const char *newval, void *extra)
{
	/* Update the pg_stat_activity view */
	/*
	 *
	 * 更新 pg_stat_activity 视图。
	 */
	pgstat_report_appname(newval);
}

/*
 * GUC check_hook for cluster_name
 *
 * cluster_name 的 GUC check_hook。
 */
bool
check_cluster_name(char **newval, void **extra, GucSource source)
{
	char	   *clean;
	char	   *ret;

	/* Only allow clean ASCII chars in the cluster name */
	/*
	 *
	 * cluster_name 只允许干净的 ASCII 字符。
	 */
	clean = pg_clean_ascii(*newval, MCXT_ALLOC_NO_OOM);
	if (!clean)
		return false;

	ret = guc_strdup(LOG, clean);
	if (!ret)
	{
		pfree(clean);
		return false;
	}

	guc_free(*newval);

	pfree(clean);
	*newval = ret;
	return true;
}

/*
 * GUC assign_hook for maintenance_io_concurrency
 *
 * maintenance_io_concurrency 的 GUC assign_hook。
 */
void
assign_maintenance_io_concurrency(int newval, void *extra)
{
	/*
	 * Reconfigure recovery prefetching, because a setting it depends on
	 * changed.
	 *
	 * 它所依赖的设置已变，因此重新配置恢复预取。
	 */
	maintenance_io_concurrency = newval;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
}

/*
 * GUC assign hooks that recompute io_combine_limit whenever
 * io_combine_limit_guc and io_max_combine_limit are changed.  These are needed
 * because the GUC subsystem doesn't support dependencies between GUCs, and
 * they may be assigned in either order.
 *
 * 在 io_combine_limit_guc 或 io_max_combine_limit 变化时重算 io_combine_limit 的 assign_hook。
 * GUC 子系统不支持 GUC 之间的依赖，且二者赋值顺序不定，因此需要这些钩子。
 */
void
assign_io_max_combine_limit(int newval, void *extra)
{
	io_combine_limit = Min(newval, io_combine_limit_guc);
}
/*
 * 在 io_combine_limit 变更时，按 io_max_combine_limit 重算实际合并上限。
 */
void
assign_io_combine_limit(int newval, void *extra)
{
	io_combine_limit = Min(io_max_combine_limit, newval);
}

/*
 * These show hooks just exist because we want to show the values in octal.
 *
 * 这些 show_hook 存在只是为了以八进制显示数值。
 */

/*
 * GUC show_hook for data_directory_mode
 *
 * data_directory_mode 的 GUC show_hook。
 */
const char *
show_data_directory_mode(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", data_directory_mode);
	return buf;
}

/*
 * GUC show_hook for log_file_mode
 *
 * log_file_mode 的 GUC show_hook。
 */
const char *
show_log_file_mode(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", Log_file_mode);
	return buf;
}

/*
 * GUC show_hook for unix_socket_permissions
 *
 * unix_socket_permissions 的 GUC show_hook。
 */
const char *
show_unix_socket_permissions(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", Unix_socket_permissions);
	return buf;
}


/*
 * These check hooks do nothing more than reject non-default settings
 * in builds that don't support them.
 *
 * 这些 check_hook 只是在不支持相应功能的构建中拒绝非默认设置。
 */

bool
check_bonjour(bool *newval, void **extra, GucSource source)
{
#ifndef USE_BONJOUR
	if (*newval)
	{
		GUC_check_errmsg("Bonjour is not supported by this build");
		return false;
	}
#endif
	return true;
}

/*
 * 拒绝 default_with_oids 的非默认值；WITH OIDS 表已不再支持。
 */
bool
check_default_with_oids(bool *newval, void **extra, GucSource source)
{
	if (*newval)
	{
		/* check the GUC's definition for an explanation */
		/*
		 *
		 * 原因见该 GUC 的定义。
		 */
		GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
		GUC_check_errmsg("tables declared WITH OIDS are not supported");

		return false;
	}

	return true;
}

/*
 * 在未编译 SSL 的构建中拒绝打开 ssl。
 */
bool
check_ssl(bool *newval, void **extra, GucSource source)
{
#ifndef USE_SSL
	if (*newval)
	{
		GUC_check_errmsg("SSL is not supported by this build");
		return false;
	}
#endif
	return true;
}
