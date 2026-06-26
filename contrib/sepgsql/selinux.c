/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/selinux.c
 *
 * Interactions between userspace and selinux in kernelspace,
 * using libselinux api.
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"

#include "sepgsql.h"

/*
 * selinux_catalog
 *
 * This mapping table enables to translate the name of object classes and
 * access vectors to/from their own codes.
 * When we ask SELinux whether the required privileges are allowed or not,
 * we use security_compute_av(3). It needs us to represent object classes
 * and access vectors using 'external' codes defined in the security policy.
 * It is determined in the runtime, not build time. So, it needs an internal
 * service to translate object class/access vectors which we want to check
 * into the code which kernel want to be given.
 *
 * This mapping table enables to translate the name of object classes and access vectors to/from their own codes.当我们询问 SELinux 是否允许所需的权限时，我们使用 security_compute_av(3)。它需要我们使用安全策略中定义的“外部”代码来表示对象类和访问向量。 It is determined in the runtime, not build time. So, it needs an internal service to translate object class/access vectors which we want to check into the code which kernel want to be given.
 */
static struct
{
	const char *class_name;
	uint16		class_code;
	struct
	{
		const char *av_name;
		uint32		av_code;
	}			av[32];
}			selinux_catalog[] =

{
	{
		"process", SEPG_CLASS_PROCESS,
		{
			{
				"transition", SEPG_PROCESS__TRANSITION
			},
			{
				"dyntransition", SEPG_PROCESS__DYNTRANSITION
			},
			{
				"setcurrent", SEPG_PROCESS__SETCURRENT
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"file", SEPG_CLASS_FILE,
		{
			{
				"read", SEPG_FILE__READ
			},
			{
				"write", SEPG_FILE__WRITE
			},
			{
				"create", SEPG_FILE__CREATE
			},
			{
				"getattr", SEPG_FILE__GETATTR
			},
			{
				"unlink", SEPG_FILE__UNLINK
			},
			{
				"rename", SEPG_FILE__RENAME
			},
			{
				"append", SEPG_FILE__APPEND
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"dir", SEPG_CLASS_DIR,
		{
			{
				"read", SEPG_DIR__READ
			},
			{
				"write", SEPG_DIR__WRITE
			},
			{
				"create", SEPG_DIR__CREATE
			},
			{
				"getattr", SEPG_DIR__GETATTR
			},
			{
				"unlink", SEPG_DIR__UNLINK
			},
			{
				"rename", SEPG_DIR__RENAME
			},
			{
				"search", SEPG_DIR__SEARCH
			},
			{
				"add_name", SEPG_DIR__ADD_NAME
			},
			{
				"remove_name", SEPG_DIR__REMOVE_NAME
			},
			{
				"rmdir", SEPG_DIR__RMDIR
			},
			{
				"reparent", SEPG_DIR__REPARENT
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"lnk_file", SEPG_CLASS_LNK_FILE,
		{
			{
				"read", SEPG_LNK_FILE__READ
			},
			{
				"write", SEPG_LNK_FILE__WRITE
			},
			{
				"create", SEPG_LNK_FILE__CREATE
			},
			{
				"getattr", SEPG_LNK_FILE__GETATTR
			},
			{
				"unlink", SEPG_LNK_FILE__UNLINK
			},
			{
				"rename", SEPG_LNK_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"chr_file", SEPG_CLASS_CHR_FILE,
		{
			{
				"read", SEPG_CHR_FILE__READ
			},
			{
				"write", SEPG_CHR_FILE__WRITE
			},
			{
				"create", SEPG_CHR_FILE__CREATE
			},
			{
				"getattr", SEPG_CHR_FILE__GETATTR
			},
			{
				"unlink", SEPG_CHR_FILE__UNLINK
			},
			{
				"rename", SEPG_CHR_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"blk_file", SEPG_CLASS_BLK_FILE,
		{
			{
				"read", SEPG_BLK_FILE__READ
			},
			{
				"write", SEPG_BLK_FILE__WRITE
			},
			{
				"create", SEPG_BLK_FILE__CREATE
			},
			{
				"getattr", SEPG_BLK_FILE__GETATTR
			},
			{
				"unlink", SEPG_BLK_FILE__UNLINK
			},
			{
				"rename", SEPG_BLK_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"sock_file", SEPG_CLASS_SOCK_FILE,
		{
			{
				"read", SEPG_SOCK_FILE__READ
			},
			{
				"write", SEPG_SOCK_FILE__WRITE
			},
			{
				"create", SEPG_SOCK_FILE__CREATE
			},
			{
				"getattr", SEPG_SOCK_FILE__GETATTR
			},
			{
				"unlink", SEPG_SOCK_FILE__UNLINK
			},
			{
				"rename", SEPG_SOCK_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"fifo_file", SEPG_CLASS_FIFO_FILE,
		{
			{
				"read", SEPG_FIFO_FILE__READ
			},
			{
				"write", SEPG_FIFO_FILE__WRITE
			},
			{
				"create", SEPG_FIFO_FILE__CREATE
			},
			{
				"getattr", SEPG_FIFO_FILE__GETATTR
			},
			{
				"unlink", SEPG_FIFO_FILE__UNLINK
			},
			{
				"rename", SEPG_FIFO_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"db_database", SEPG_CLASS_DB_DATABASE,
		{
			{
				"create", SEPG_DB_DATABASE__CREATE
			},
			{
				"drop", SEPG_DB_DATABASE__DROP
			},
			{
				"getattr", SEPG_DB_DATABASE__GETATTR
			},
			{
				"setattr", SEPG_DB_DATABASE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_DATABASE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_DATABASE__RELABELTO
			},
			{
				"access", SEPG_DB_DATABASE__ACCESS
			},
			{
				"load_module", SEPG_DB_DATABASE__LOAD_MODULE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_schema", SEPG_CLASS_DB_SCHEMA,
		{
			{
				"create", SEPG_DB_SCHEMA__CREATE
			},
			{
				"drop", SEPG_DB_SCHEMA__DROP
			},
			{
				"getattr", SEPG_DB_SCHEMA__GETATTR
			},
			{
				"setattr", SEPG_DB_SCHEMA__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_SCHEMA__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_SCHEMA__RELABELTO
			},
			{
				"search", SEPG_DB_SCHEMA__SEARCH
			},
			{
				"add_name", SEPG_DB_SCHEMA__ADD_NAME
			},
			{
				"remove_name", SEPG_DB_SCHEMA__REMOVE_NAME
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_table", SEPG_CLASS_DB_TABLE,
		{
			{
				"create", SEPG_DB_TABLE__CREATE
			},
			{
				"drop", SEPG_DB_TABLE__DROP
			},
			{
				"getattr", SEPG_DB_TABLE__GETATTR
			},
			{
				"setattr", SEPG_DB_TABLE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_TABLE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_TABLE__RELABELTO
			},
			{
				"select", SEPG_DB_TABLE__SELECT
			},
			{
				"update", SEPG_DB_TABLE__UPDATE
			},
			{
				"insert", SEPG_DB_TABLE__INSERT
			},
			{
				"delete", SEPG_DB_TABLE__DELETE
			},
			{
				"lock", SEPG_DB_TABLE__LOCK
			},
			{
				"truncate", SEPG_DB_TABLE__TRUNCATE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_sequence", SEPG_CLASS_DB_SEQUENCE,
		{
			{
				"create", SEPG_DB_SEQUENCE__CREATE
			},
			{
				"drop", SEPG_DB_SEQUENCE__DROP
			},
			{
				"getattr", SEPG_DB_SEQUENCE__GETATTR
			},
			{
				"setattr", SEPG_DB_SEQUENCE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_SEQUENCE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_SEQUENCE__RELABELTO
			},
			{
				"get_value", SEPG_DB_SEQUENCE__GET_VALUE
			},
			{
				"next_value", SEPG_DB_SEQUENCE__NEXT_VALUE
			},
			{
				"set_value", SEPG_DB_SEQUENCE__SET_VALUE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_procedure", SEPG_CLASS_DB_PROCEDURE,
		{
			{
				"create", SEPG_DB_PROCEDURE__CREATE
			},
			{
				"drop", SEPG_DB_PROCEDURE__DROP
			},
			{
				"getattr", SEPG_DB_PROCEDURE__GETATTR
			},
			{
				"setattr", SEPG_DB_PROCEDURE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_PROCEDURE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_PROCEDURE__RELABELTO
			},
			{
				"execute", SEPG_DB_PROCEDURE__EXECUTE
			},
			{
				"entrypoint", SEPG_DB_PROCEDURE__ENTRYPOINT
			},
			{
				"install", SEPG_DB_PROCEDURE__INSTALL
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_column", SEPG_CLASS_DB_COLUMN,
		{
			{
				"create", SEPG_DB_COLUMN__CREATE
			},
			{
				"drop", SEPG_DB_COLUMN__DROP
			},
			{
				"getattr", SEPG_DB_COLUMN__GETATTR
			},
			{
				"setattr", SEPG_DB_COLUMN__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_COLUMN__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_COLUMN__RELABELTO
			},
			{
				"select", SEPG_DB_COLUMN__SELECT
			},
			{
				"update", SEPG_DB_COLUMN__UPDATE
			},
			{
				"insert", SEPG_DB_COLUMN__INSERT
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_tuple", SEPG_CLASS_DB_TUPLE,
		{
			{
				"relabelfrom", SEPG_DB_TUPLE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_TUPLE__RELABELTO
			},
			{
				"select", SEPG_DB_TUPLE__SELECT
			},
			{
				"update", SEPG_DB_TUPLE__UPDATE
			},
			{
				"insert", SEPG_DB_TUPLE__INSERT
			},
			{
				"delete", SEPG_DB_TUPLE__DELETE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_blob", SEPG_CLASS_DB_BLOB,
		{
			{
				"create", SEPG_DB_BLOB__CREATE
			},
			{
				"drop", SEPG_DB_BLOB__DROP
			},
			{
				"getattr", SEPG_DB_BLOB__GETATTR
			},
			{
				"setattr", SEPG_DB_BLOB__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_BLOB__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_BLOB__RELABELTO
			},
			{
				"read", SEPG_DB_BLOB__READ
			},
			{
				"write", SEPG_DB_BLOB__WRITE
			},
			{
				"import", SEPG_DB_BLOB__IMPORT
			},
			{
				"export", SEPG_DB_BLOB__EXPORT
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_language", SEPG_CLASS_DB_LANGUAGE,
		{
			{
				"create", SEPG_DB_LANGUAGE__CREATE
			},
			{
				"drop", SEPG_DB_LANGUAGE__DROP
			},
			{
				"getattr", SEPG_DB_LANGUAGE__GETATTR
			},
			{
				"setattr", SEPG_DB_LANGUAGE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_LANGUAGE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_LANGUAGE__RELABELTO
			},
			{
				"implement", SEPG_DB_LANGUAGE__IMPLEMENT
			},
			{
				"execute", SEPG_DB_LANGUAGE__EXECUTE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_view", SEPG_CLASS_DB_VIEW,
		{
			{
				"create", SEPG_DB_VIEW__CREATE
			},
			{
				"drop", SEPG_DB_VIEW__DROP
			},
			{
				"getattr", SEPG_DB_VIEW__GETATTR
			},
			{
				"setattr", SEPG_DB_VIEW__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_VIEW__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_VIEW__RELABELTO
			},
			{
				"expand", SEPG_DB_VIEW__EXPAND
			},
			{
				NULL, 0UL
			},
		}
	},
};

/*
 * sepgsql_mode
 *
 * SEPGSQL_MODE_DISABLED: Disabled on runtime
 * SEPGSQL_MODE_DEFAULT: Same as system settings
 * SEPGSQL_MODE_PERMISSIVE: Always permissive mode
 * SEPGSQL_MODE_INTERNAL: Same as permissive, except for no audit logs
 *
 * SEPGSQL_MODE_DISABLED：在运行时禁用 SEPGSQL_MODE_DEFAULT：与系统设置相同 SEPGSQL_MODE_PERMISSIVE：始终许可模式 SEPGSQL_MODE_INTERNAL：与许可模式相同，但没有审核日志
 */
static int	sepgsql_mode = SEPGSQL_MODE_INTERNAL;

/*
 * sepgsql_is_enabled
 */
bool
sepgsql_is_enabled(void)
{
	return (sepgsql_mode != SEPGSQL_MODE_DISABLED);
}

/*
 * sepgsql_get_mode
 */
int
sepgsql_get_mode(void)
{
	return sepgsql_mode;
}

/*
 * sepgsql_set_mode
 */
int
sepgsql_set_mode(int new_mode)
{
	int			old_mode = sepgsql_mode;

	sepgsql_mode = new_mode;

	return old_mode;
}

/*
 * sepgsql_getenforce
 *
 * It returns whether the current working mode tries to enforce access
 * control decision, or not. It shall be enforced when sepgsql_mode is
 * SEPGSQL_MODE_DEFAULT and system is running in enforcing mode.
 *
 * 它返回当前工作模式是否尝试强制执行访问控制决策。当 sepgsql_mode 为 SEPGSQL_MODE_DEFAULT 并且系统运行在强制模式下时，应强制执行。
 */
bool
sepgsql_getenforce(void)
{
	if (sepgsql_mode == SEPGSQL_MODE_DEFAULT &&
		selinux_status_getenforce() > 0)
		return true;

	return false;
}

/*
 * sepgsql_audit_log
 *
 * It generates a security audit record. It writes out audit records
 * into standard PG's logfile.
 *
 * 它生成安全审核记录。它将审计记录写入标准 PG 的日志文件中。
 *
 * SELinux can control what should be audited and should not using
 * "auditdeny" and "auditallow" rules in the security policy. In the
 * default, all the access violations are audited, and all the access
 * allowed are not audited. But we can set up the security policy, so
 * we can have exceptions. So, it is necessary to follow the suggestion
 * come from the security policy. (av_decision.auditallow and auditdeny)
 *
 * SELinux can control what should be audited and should not using "auditdeny" and "auditallow" rules in the security policy. In the default, all the access violations are audited, and all the access allowed are not audited. But we can set up the security policy, so we can have exceptions.因此，有必要遵循安全策略的建议。 (av_decision.auditallow and auditdeny)
 *
 * Security audit is an important feature, because it enables us to check
 * what was happen if we have a security incident. In fact, ISO/IEC15408
 * defines several security functionalities for audit features.
 *
 * 安全审计是一项重要功能，因为它使我们能够在发生安全事件时检查发生了什么。事实上，ISO/IEC15408 定义了多种审计功能的安全功能。
 */
void
sepgsql_audit_log(bool denied,
				  bool enforcing,
				  const char *scontext,
				  const char *tcontext,
				  uint16 tclass,
				  uint32 audited,
				  const char *audit_name)
{
	StringInfoData buf;
	const char *class_name;
	const char *av_name;
	int			i;

	/* lookup name of the object class
	 *
	 * 查找对象类的名称
	 */
	Assert(tclass < SEPG_CLASS_MAX);
	class_name = selinux_catalog[tclass].class_name;

	/* lookup name of the permissions
	 *
	 * 查找权限名称
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf, "%s {",
					 (denied ? "denied" : "allowed"));
	for (i = 0; selinux_catalog[tclass].av[i].av_name; i++)
	{
		if (audited & (1UL << i))
		{
			av_name = selinux_catalog[tclass].av[i].av_name;
			appendStringInfo(&buf, " %s", av_name);
		}
	}
	appendStringInfoString(&buf, " }");

	/*
	 * Call external audit module, if loaded
	 *
	 * 调用外部审计模块（如果已加载）
	 */
	appendStringInfo(&buf, " scontext=%s tcontext=%s tclass=%s",
					 scontext, tcontext, class_name);
	if (audit_name)
		appendStringInfo(&buf, " name=\"%s\"", audit_name);

	if (enforcing)
		appendStringInfoString(&buf, " permissive=0");
	else
		appendStringInfoString(&buf, " permissive=1");

	ereport(LOG, (errmsg("SELinux: %s", buf.data)));
}

/*
 * sepgsql_compute_avd
 *
 * It actually asks SELinux what permissions are allowed on a pair of
 * the security contexts and object class. It also returns what permissions
 * should be audited on access violation or allowed.
 * In most cases, subject's security context (scontext) is a client, and
 * target security context (tcontext) is a database object.
 *
 * 它实际上询问 SELinux 在一对安全上下文和对象类上允许哪些权限。它还返回应在访问违规时审核或允许哪些权限。在大多数情况下，主体的安全上下文（scontext）是客户端，目标安全上下文（tcontext）是数据库对象。
 *
 * The access control decision shall be set on the given av_decision.
 * The av_decision.allowed has a bitmask of SEPG_<class>__<perms>
 * to suggest a set of allowed actions in this object class.
 *
 * 访问控制决策应根据给定的 av_decision 设置。 av_decision.allowed 具有 SEPG_<class>__<perms> 位掩码，用于建议该对象类中允许的一组操作。
 */
void
sepgsql_compute_avd(const char *scontext,
					const char *tcontext,
					uint16 tclass,
					struct av_decision *avd)
{
	const char *tclass_name;
	security_class_t tclass_ex;
	struct av_decision avd_ex;
	int			i,
				deny_unknown = security_deny_unknown();

	/* Get external code of the object class
	 *
	 * 获取对象类的外部代码
	 */
	Assert(tclass < SEPG_CLASS_MAX);
	Assert(tclass == selinux_catalog[tclass].class_code);

	tclass_name = selinux_catalog[tclass].class_name;
	tclass_ex = string_to_security_class(tclass_name);

	if (tclass_ex == 0)
	{
		/*
		 * If the current security policy does not support permissions
		 * corresponding to database objects, we fill up them with dummy data.
		 * If security_deny_unknown() returns positive value, undefined
		 * permissions should be denied. Otherwise, allowed
		 *
		 * 如果当前的安全策略不支持数据库对象对应的权限，我们就用虚拟数据填充它们。如果 security_deny_unknown() 返回正值，则应拒绝未定义的权限。否则，允许
		 */
		avd->allowed = (security_deny_unknown() > 0 ? 0 : ~0);
		avd->auditallow = 0U;
		avd->auditdeny = ~0U;
		avd->flags = 0;

		return;
	}

	/*
	 * Ask SELinux what is allowed set of permissions on a pair of the
	 * security contexts and the given object class.
	 *
	 * 询问 SELinux 在一对安全上下文和给定的对象类上允许的权限集是什么。
	 */
	if (security_compute_av_flags_raw(scontext,
									  tcontext,
									  tclass_ex, 0, &avd_ex) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux could not compute av_decision: "
						"scontext=%s tcontext=%s tclass=%s: %m",
						scontext, tcontext, tclass_name)));

	/*
	 * SELinux returns its access control decision as a set of permissions
	 * represented in external code which depends on run-time environment. So,
	 * we need to translate it to the internal representation before returning
	 * results for the caller.
	 *
	 * SELinux 将其访问控制决策返回为一组以外部代码表示的权限，这取决于运行时环境。因此，在向调用者返回结果之前，我们需要将其转换为内部表示。
	 */
	memset(avd, 0, sizeof(struct av_decision));

	for (i = 0; selinux_catalog[tclass].av[i].av_name; i++)
	{
		access_vector_t av_code_ex;
		const char *av_name = selinux_catalog[tclass].av[i].av_name;
		uint32		av_code = selinux_catalog[tclass].av[i].av_code;

		av_code_ex = string_to_av_perm(tclass_ex, av_name);
		if (av_code_ex == 0)
		{
			/* fill up undefined permissions
			 *
			 * 填写未定义的权限
			 */
			if (!deny_unknown)
				avd->allowed |= av_code;
			avd->auditdeny |= av_code;

			continue;
		}

		if (avd_ex.allowed & av_code_ex)
			avd->allowed |= av_code;
		if (avd_ex.auditallow & av_code_ex)
			avd->auditallow |= av_code;
		if (avd_ex.auditdeny & av_code_ex)
			avd->auditdeny |= av_code;
	}
}

/*
 * sepgsql_compute_create
 *
 * It returns a default security context to be assigned on a new database
 * object. SELinux compute it based on a combination of client, upper object
 * which owns the new object and object class.
 *
 * 它返回要分配给新数据库对象的默认安全上下文。 SELinux 基于客户端、拥有新对象的上层对象和对象类的组合来计算它。
 *
 * For example, when a client (staff_u:staff_r:staff_t:s0) tries to create
 * a new table within a schema (system_u:object_r:sepgsql_schema_t:s0),
 * SELinux looks-up its security policy. If it has a special rule on the
 * combination of these security contexts and object class (db_table),
 * it returns the security context suggested by the special rule.
 * Otherwise, it returns the security context of schema, as is.
 *
 * 例如，当客户端 (staff_u:staff_r:staff_t:s0) 尝试在架构 (system_u:object_r:sepgsql_schema_t:s0) 中创建新表时，SELinux 会查找其安全策略。如果它对这些安全上下文和对象类（db_table）的组合有特殊规则，则它返回特殊规则建议的安全上下文。否则，它按原样返回模式的安全上下文。
 *
 * We expect the caller already applies sanity/validation checks on the
 * given security context.
 *
 * 我们期望调用者已经对给定的安全上下文应用了健全性/验证检查。
 *
 * scontext: security context of the subject (mostly, peer process).
 * tcontext: security context of the upper database object.
 * tclass: class code (SEPG_CLASS_*) of the new object in creation
 *
 * scontext：主体的安全上下文（主要是对等进程）。 tcontext：上层数据库对象的安全上下文。 tclass：创建时新对象的类代码（SEPG_CLASS_*）
 */
char *
sepgsql_compute_create(const char *scontext,
					   const char *tcontext,
					   uint16 tclass,
					   const char *objname)
{
	char	   *ncontext;
	security_class_t tclass_ex;
	const char *tclass_name;
	char	   *result;

	/* Get external code of the object class
	 *
	 * 获取对象类的外部代码
	 */
	Assert(tclass < SEPG_CLASS_MAX);

	tclass_name = selinux_catalog[tclass].class_name;
	tclass_ex = string_to_security_class(tclass_name);

	/*
	 * Ask SELinux what is the default context for the given object class on a
	 * pair of security contexts
	 *
	 * 询问 SELinux 在一对安全上下文中给定对象类的默认上下文是什么
	 */
	if (security_compute_create_name_raw(scontext,
										 tcontext,
										 tclass_ex,
										 objname,
										 &ncontext) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux could not compute a new context: "
						"scontext=%s tcontext=%s tclass=%s: %m",
						scontext, tcontext, tclass_name)));

	/*
	 * libselinux returns malloc()'ed string, so we need to copy it on the
	 * palloc()'ed region.
	 *
	 * libselinux 返回 malloc() 后的字符串，因此我们需要将其复制到 palloc() 后的区域。
	 */
	PG_TRY();
	{
		result = pstrdup(ncontext);
	}
	PG_FINALLY();
	{
		freecon(ncontext);
	}
	PG_END_TRY();

	return result;
}
