/*-------------------------------------------------------------------------
 *
 * extension.c
 *	  Commands to manipulate extensions
 *
 * 本文件实现操作扩展的命令。
 *
 * Extensions in PostgreSQL allow management of collections of SQL objects.
 *
 * PostgreSQL 中的扩展用于管理一组 SQL 对象。
 *
 * All we need internally to manage an extension is an OID so that the
 * dependent objects can be associated with it.  An extension is created by
 * populating the pg_extension catalog from a "control" file.
 * The extension control file is parsed with the same parser we use for
 * postgresql.conf.  An extension also has an installation script file,
 * containing SQL commands to create the extension's objects.
 *
 * 内部管理扩展只需要一个 OID，以便把依赖对象关联到它。
 * 扩展通过用控制文件填充 pg_extension 目录来创建。
 * 扩展控制文件使用与 postgresql.conf 相同的解析器。
 * 扩展还有一个安装脚本，其中是创建该扩展对象的 SQL 命令。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/extension.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/schemacmds.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/queryjumble.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/conffiles.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/* GUC */
/*
 *
 * GUC
 */
char	   *Extension_control_path;

/* Globally visible state variables */
/*
 *
 * 全局可见的状态变量
 */
bool		creating_extension = false;
Oid			CurrentExtensionObject = InvalidOid;

/*
 * Internal data structure to hold the results of parsing a control file
 *
 * 保存控制文件解析结果的内部数据结构
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* name of the extension */
	/*
	 *
	 * 扩展名
	 */
	char	   *basedir;		/* base directory where control and script
								 * files are located */
	/*
	 *
	 * 控制文件与脚本文件所在的基目录
	 */
	char	   *control_dir;	/* directory where control file was found */
	/*
	 *
	 * 找到控制文件的目录
	 */
	char	   *directory;		/* directory for script files */
	/*
	 *
	 * 脚本文件目录
	 */
	char	   *default_version;	/* default install target version, if any */
	/*
	 *
	 * 默认安装目标版本，若有
	 */
	char	   *module_pathname;	/* string to substitute for
									 * MODULE_PATHNAME */
	/*
	 *
	 * 用于替换 MODULE_PATHNAME 的字符串
	 */
	char	   *comment;		/* comment, if any */
	/*
	 *
	 * 注释，若有
	 */
	char	   *schema;			/* target schema (allowed if !relocatable) */
	/*
	 *
	 * 目标模式（在不可重定位时允许）
	 */
	bool		relocatable;	/* is ALTER EXTENSION SET SCHEMA supported? */
	/*
	 *
	 * 是否支持 ALTER EXTENSION SET SCHEMA？
	 */
	bool		superuser;		/* must be superuser to install? */
	/*
	 *
	 * 安装时是否必须是超级用户？
	 */
	bool		trusted;		/* allow becoming superuser on the fly? */
	/*
	 *
	 * 是否允许临时变成超级用户？
	 */
	int			encoding;		/* encoding of the script file, or -1 */
	/*
	 *
	 * 脚本文件的编码，或 -1
	 */
	List	   *requires;		/* names of prerequisite extensions */
	/*
	 *
	 * 前置扩展的名称
	 */
	List	   *no_relocate;	/* names of prerequisite extensions that
								 * should not be relocated */
	/*
	 *
	 * 不应被重定位的前置扩展名称
	 */
} ExtensionControlFile;

/*
 * Internal data structure for update path information
 *
 * 更新路径信息的内部数据结构
 */
typedef struct ExtensionVersionInfo
{
	char	   *name;			/* name of the starting version */
	/*
	 *
	 * 起始版本的名称
	 */
	List	   *reachable;		/* List of ExtensionVersionInfo's */
	/*
	 *
	 * ExtensionVersionInfo 的 List
	 */
	bool		installable;	/* does this version have an install script? */
	/*
	 *
	 * 该版本是否有安装脚本？
	 */
	/* working state for Dijkstra's algorithm: */
	/*
	 *
	 * Dijkstra 算法的工作状态：
	 */
	bool		distance_known; /* is distance from start known yet? */
	/*
	 *
	 * 到起点的距离是否已经确定？
	 */
	int			distance;		/* current worst-case distance estimate */
	/*
	 *
	 * 当前最坏情况的距离估计
	 */
	struct ExtensionVersionInfo *previous;	/* current best predecessor */
	/*
	 *
	 * 当前最佳前驱
	 */
} ExtensionVersionInfo;

/*
 * Information for script_error_callback()
 *
 * script_error_callback() 所用的信息
 */
typedef struct
{
	const char *sql;			/* entire script file contents */
	/*
	 *
	 * 整个脚本文件的内容
	 */
	const char *filename;		/* script file pathname */
	/*
	 *
	 * 脚本文件路径名
	 */
	ParseLoc	stmt_location;	/* current stmt start loc, or -1 if unknown */
	/*
	 *
	 * 当前语句的起始位置，未知时为 -1
	 */
	ParseLoc	stmt_len;		/* length in bytes; 0 means "rest of string" */
	/*
	 *
	 * 字节长度；0 表示“字符串的其余部分”
	 */
} script_error_callback_arg;

/*
 * Cache structure for get_function_sibling_type (and maybe later,
 * allied lookup functions).
 *
 * get_function_sibling_type（以及以后可能的同类查找函数）所用
 * 的缓存结构。
 */
typedef struct ExtensionSiblingCache
{
	struct ExtensionSiblingCache *next; /* list link */
	/*
	 *
	 * 链表链接
	 */
	/* lookup key: requesting function's OID and type name */
	/*
	 *
	 * 查找键：请求函数的 OID 与类型名
	 */
	Oid			reqfuncoid;
	const char *typname;
	bool		valid;			/* is entry currently valid? */
	/*
	 *
	 * 该项当前是否有效？
	 */
	uint32		exthash;		/* cache hash of owning extension's OID */
	/*
	 *
	 * 所属扩展 OID 的缓存散列
	 */
	Oid			typeoid;		/* OID associated with typname */
	/*
	 *
	 * 与 typname 关联的 OID
	 */
} ExtensionSiblingCache;

/* Head of linked list of ExtensionSiblingCache structs */
/*
 *
 * ExtensionSiblingCache 结构链表的头
 */
static ExtensionSiblingCache *ext_sibling_list = NULL;

/* Local functions */
/*
 *
 * 本文件内部函数
 */
static void ext_sibling_callback(Datum arg, int cacheid, uint32 hashvalue);
static List *find_update_path(List *evi_list,
							  ExtensionVersionInfo *evi_start,
							  ExtensionVersionInfo *evi_target,
							  bool reject_indirect,
							  bool reinitialize);
static Oid	get_required_extension(char *reqExtensionName,
								   char *extensionName,
								   char *origSchemaName,
								   bool cascade,
								   List *parents,
								   bool is_create);
static void get_available_versions_for_extension(ExtensionControlFile *pcontrol,
												 Tuplestorestate *tupstore,
												 TupleDesc tupdesc);
static Datum convert_requires_to_datum(List *requires);
static void ApplyExtensionUpdates(Oid extensionOid,
								  ExtensionControlFile *pcontrol,
								  const char *initialVersion,
								  List *updateVersions,
								  char *origSchemaName,
								  bool cascade,
								  bool is_create);
static void ExecAlterExtensionContentsRecurse(AlterExtensionContentsStmt *stmt,
											  ObjectAddress extension,
											  ObjectAddress object);
static char *read_whole_file(const char *filename, int *length);
static ExtensionControlFile *new_ExtensionControlFile(const char *extname);

char	   *find_in_paths(const char *basename, List *paths);

/*
 * get_extension_oid - given an extension name, look up the OID
 *
 * get_extension_oid：给定扩展名，查找其 OID
 *
 * If missing_ok is false, throw an error if extension name not found.  If
 * true, just return InvalidOid.
 *
 * missing_ok 为 false 时，找不到扩展名就报错。
 * 为 true 时只返回 InvalidOid。
 */
Oid
get_extension_oid(const char *extname, bool missing_ok)
{
	Oid			result;

	result = GetSysCacheOid1(EXTENSIONNAME, Anum_pg_extension_oid,
							 CStringGetDatum(extname));

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						extname)));

	return result;
}

/*
 * get_extension_name - given an extension OID, look up the name
 *
 * get_extension_name：给定扩展 OID，查找其名称
 *
 * Returns a palloc'd string, or NULL if no such extension.
 *
 * 返回 palloc 的字符串；若没有该扩展则返回 NULL。
 */
char *
get_extension_name(Oid ext_oid)
{
	char	   *result;
	HeapTuple	tuple;

	tuple = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext_oid));

	if (!HeapTupleIsValid(tuple))
		return NULL;

	result = pstrdup(NameStr(((Form_pg_extension) GETSTRUCT(tuple))->extname));
	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_extension_schema - given an extension OID, fetch its extnamespace
 *
 * get_extension_schema：给定扩展 OID，
 * 取其 extnamespace
 *
 * Returns InvalidOid if no such extension.
 *
 * 若没有该扩展则返回 InvalidOid。
 */
Oid
get_extension_schema(Oid ext_oid)
{
	Oid			result;
	HeapTuple	tuple;

	tuple = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext_oid));

	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_function_sibling_type - find a type belonging to same extension as func
 *
 * get_function_sibling_type：查找与函数属于同一扩展的类型
 *
 * Returns the type's OID, or InvalidOid if not found.
 *
 * 返回该类型的 OID；找不到则返回 InvalidOid。
 *
 * This is useful in extensions, which won't have fixed object OIDs.
 * We work from the calling function's own OID, which it can get from its
 * FunctionCallInfo parameter, and look up the owning extension and thence
 * a type belonging to the same extension.
 *
 * 这对扩展很有用，因为扩展没有固定的对象 OID。我们从调用函数自身的 OID 出发
 * （可从其 FunctionCallInfo 参数得到），查找所属扩展，
 * 再找到属于同一扩展的类型。
 *
 * Notice that the type is specified by name only, without a schema.
 * That's because this will typically be used by relocatable extensions
 * which can't make a-priori assumptions about which schema their objects
 * are in.  As long as the extension only defines one type of this name,
 * the answer is unique anyway.
 *
 * 注意类型只按名称指定，不带模式。因为可重定位扩展通常不能事先假定对象所在的模式。
 * 只要该扩展只定义一个此名称的类型，答案就是唯一的。
 *
 * We might later add the ability to look up functions, operators, etc.
 *
 * 以后可能会增加查找函数、操作符等的能力。
 *
 * This code is simply a frontend for some pg_depend lookups.  Those lookups
 * are fairly expensive, so we provide a simple cache facility.  We assume
 * that the passed typname is actually a C constant, or at least permanently
 * allocated, so that we need not copy that string.
 *
 * 这段代码只是一些 pg_depend 查找的前端。那些查找相当昂贵，
 * 因此提供一个简单缓存。
 * 假定传入的 typname 实际是 C 常量，或至少是永久分配的，
 * 因此不必复制该字符串。
 */
Oid
get_function_sibling_type(Oid funcoid, const char *typname)
{
	ExtensionSiblingCache *cache_entry;
	Oid			extoid;
	Oid			typeoid;

	/*
	 * See if we have the answer cached.  Someday there may be enough callers
	 * to justify a hash table, but for now, a simple linked list is fine.
	 *
	 * 看答案是否已缓存。将来调用者够多时也许值得用哈希表，目前简单链表就够了。
	 */
	for (cache_entry = ext_sibling_list; cache_entry != NULL;
		 cache_entry = cache_entry->next)
	{
		if (funcoid == cache_entry->reqfuncoid &&
			strcmp(typname, cache_entry->typname) == 0)
			break;
	}
	if (cache_entry && cache_entry->valid)
		return cache_entry->typeoid;

	/*
	 * Nope, so do the expensive lookups.  We do not expect failures, so we do
	 * not cache negative results.
	 *
	 * 没有，于是做昂贵的查找。我们不预期失败，因此不缓存否定结果。
	 */
	extoid = getExtensionOfObject(ProcedureRelationId, funcoid);
	if (!OidIsValid(extoid))
		return InvalidOid;
	typeoid = getExtensionType(extoid, typname);
	if (!OidIsValid(typeoid))
		return InvalidOid;

	/*
	 * Build, or revalidate, cache entry.
	 *
	 * 建立或重新验证缓存项。
	 */
	if (cache_entry == NULL)
	{
		/* Register invalidation hook if this is first entry */
		/*
		 *
		 * 若这是第一项，则注册失效钩子
		 */
		if (ext_sibling_list == NULL)
			CacheRegisterSyscacheCallback(EXTENSIONOID,
										  ext_sibling_callback,
										  (Datum) 0);

		/* Momentarily zero the space to ensure valid flag is false */
		/*
		 *
		 * 暂时把这块空间清零，以保证 valid 标志为 false
		 */
		cache_entry = (ExtensionSiblingCache *)
			MemoryContextAllocZero(CacheMemoryContext,
								   sizeof(ExtensionSiblingCache));
		cache_entry->next = ext_sibling_list;
		ext_sibling_list = cache_entry;
	}

	cache_entry->reqfuncoid = funcoid;
	cache_entry->typname = typname;
	cache_entry->exthash = GetSysCacheHashValue1(EXTENSIONOID,
												 ObjectIdGetDatum(extoid));
	cache_entry->typeoid = typeoid;
	/* Mark it valid only once it's fully populated */
	/*
	 *
	 * 只有完全填好之后才把它标为有效
	 */
	cache_entry->valid = true;

	return typeoid;
}

/*
 * ext_sibling_callback
 *		Syscache inval callback function for EXTENSIONOID cache
 *
 * ext_sibling_callback：EXTENSIONOID
 * 缓存的系统缓存失效回调
 *
 * It seems sufficient to invalidate ExtensionSiblingCache entries when
 * the owning extension's pg_extension entry is modified or deleted.
 * Neither a requesting function's OID, nor the OID of the object it's
 * looking for, could change without an extension update or drop/recreate.
 *
 * 所属扩展的 pg_extension 项被修改或删除时，
 * 使 ExtensionSiblingCache 项失效似乎就够了。
 * 请求函数的 OID，以及它所查找对象的 OID，若不经过扩展更新或删除/重建，
 * 都不会改变。
 */
static void
ext_sibling_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	ExtensionSiblingCache *cache_entry;

	for (cache_entry = ext_sibling_list; cache_entry != NULL;
		 cache_entry = cache_entry->next)
	{
		if (hashvalue == 0 ||
			cache_entry->exthash == hashvalue)
			cache_entry->valid = false;
	}
}

/*
 * Utility functions to check validity of extension and version names
 *
 * 检查扩展名与版本名是否合法的实用函数
 */
static void
check_valid_extension_name(const char *extensionname)
{
	int			namelen = strlen(extensionname);

	/*
	 * Disallow empty names (the parser rejects empty identifiers anyway, but
	 * let's check).
	 *
	 * 不允许空名称（解析器本来就会拒绝空标识符，但这里还是检查）。
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not be empty.")));

	/*
	 * No double dashes, since that would make script filenames ambiguous.
	 *
	 * 不允许双横线，否则脚本文件名会产生歧义。
	 */
	if (strstr(extensionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain \"--\".")));

	/*
	 * No leading or trailing dash either.  (We could probably allow this, but
	 * it would require much care in filename parsing and would make filenames
	 * visually if not formally ambiguous.  Since there's no real-world use
	 * case, let's just forbid it.)
	 *
	 * 开头或结尾也不允许横线。（也许可以允许，但文件名解析要非常小心，
	 * 而且文件名即使形式上不歧义，看起来也会歧义。既然没有实际用途，就直接禁止。）
	 */
	if (extensionname[0] == '-' || extensionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not begin or end with \"-\".")));

	/*
	 * No directory separators either (this is sufficient to prevent ".."
	 * style attacks).
	 *
	 * 也不允许目录分隔符（这足以防止 “..” 风格的攻击）。
	 */
	if (first_dir_separator(extensionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain directory separator characters.")));
}

/*
 *
 * 校验扩展版本名：不得为空，不得包含 "--"，首尾也不得为 "-"。
 */

static void
check_valid_version_name(const char *versionname)
{
	int			namelen = strlen(versionname);

	/*
	 * Disallow empty names (we could possibly allow this, but there seems
	 * little point).
	 *
	 * 不允许空名称（也许可以允许，但似乎没什么意义）。
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not be empty.")));

	/*
	 * No double dashes, since that would make script filenames ambiguous.
	 *
	 * 不允许双横线，否则脚本文件名会产生歧义。
	 */
	if (strstr(versionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain \"--\".")));

	/*
	 * No leading or trailing dash either.
	 *
	 * 开头或结尾也不允许横线。
	 */
	if (versionname[0] == '-' || versionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not begin or end with \"-\".")));

	/*
	 * No directory separators either (this is sufficient to prevent ".."
	 * style attacks).
	 *
	 * 也不允许目录分隔符（这足以防止 “..” 风格的攻击）。
	 */
	if (first_dir_separator(versionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain directory separator characters.")));
}

/*
 * Utility functions to handle extension-related path names
 *
 * 处理扩展相关路径名的实用函数
 */
static bool
is_extension_control_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".control") == 0);
}

/*
 *
 * 判断文件名是否为扩展 SQL 脚本（以 .sql 结尾）。
 */

static bool
is_extension_script_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".sql") == 0);
}

/*
 * Return a list of directories declared on extension_control_path GUC.
 *
 * 返回 extension_control_path 这个 GUC 所声明的目录列表。
 */
static List *
get_extension_control_directories(void)
{
	char		sharepath[MAXPGPATH];
	char	   *system_dir;
	char	   *ecp;
	List	   *paths = NIL;

	get_share_path(my_exec_path, sharepath);

	system_dir = psprintf("%s/extension", sharepath);

	if (strlen(Extension_control_path) == 0)
	{
		paths = lappend(paths, system_dir);
	}
	else
	{
		/* Duplicate the string so we can modify it */
		/*
		 *
		 * 复制该字符串以便我们可以修改它
		 */
		ecp = pstrdup(Extension_control_path);

		for (;;)
		{
			int			len;
			char	   *mangled;
			char	   *piece = first_path_var_separator(ecp);

			/* Get the length of the next path on ecp */
			/*
			 *
			 * 取得 ecp 上下一段路径的长度
			 */
			if (piece == NULL)
				len = strlen(ecp);
			else
				len = piece - ecp;

			/* Copy the next path found on ecp */
			/*
			 *
			 * 复制 ecp 上找到的下一段路径
			 */
			piece = palloc(len + 1);
			strlcpy(piece, ecp, len + 1);

			/*
			 * Substitute the path macro if needed or append "extension"
			 * suffix if it is a custom extension control path.
			 *
			 * 如有需要则替换路径宏；若是自定义扩展控制路径，则追加 extension 后缀。
			 */
			if (strcmp(piece, "$system") == 0)
				mangled = substitute_path_macro(piece, "$system", system_dir);
			else
				mangled = psprintf("%s/extension", piece);

			pfree(piece);

			/* Canonicalize the path based on the OS and add to the list */
			/*
			 *
			 * 按操作系统规范化路径并加入列表
			 */
			canonicalize_path(mangled);
			paths = lappend(paths, mangled);

			/* Break if ecp is empty or move to the next path on ecp */
			/*
			 *
			 * 若 ecp 为空则停止，否则移到 ecp 的下一段路径
			 */
			if (ecp[len] == '\0')
				break;
			else
				ecp += len + 1;
		}
	}

	return paths;
}

/*
 * Find control file for extension with name in control->name, looking in the
 * path.  Return the full file name, or NULL if not found.  If found, the
 * directory is recorded in control->control_dir.
 *
 * 在路径中查找名为 control->name 的扩展控制文件。返回完整文件名，
 * 找不到则返回 NULL。
 * 若找到，把目录记入 control->control_dir。
 */
static char *
find_extension_control_filename(ExtensionControlFile *control)
{
	char	   *basename;
	char	   *result;
	List	   *paths;

	Assert(control->name);

	basename = psprintf("%s.control", control->name);

	paths = get_extension_control_directories();
	result = find_in_paths(basename, paths);

	if (result)
	{
		const char *p;

		p = strrchr(result, '/');
		Assert(p);
		control->control_dir = pnstrdup(result, p - result);
	}

	return result;
}

/*
 *
 * 返回该扩展脚本文件所在的目录。
 */

static char *
get_extension_script_directory(ExtensionControlFile *control)
{
	/*
	 * The directory parameter can be omitted, absolute, or relative to the
	 * installation's base directory, which can be the sharedir or a custom
	 * path that it was set extension_control_path. It depends where the
	 * .control file was found.
	 *
	 * directory 参数可以省略、为绝对路径，或相对于安装基目录。
	 * 基目录可以是 sharedir，或 extension_control_path
	 * 中设置的自定义路径。
	 * 取决于 .control 文件是在哪里找到的。
	 */
	if (!control->directory)
		return pstrdup(control->control_dir);

	if (is_absolute_path(control->directory))
		return pstrdup(control->directory);

	Assert(control->basedir != NULL);
	return psprintf("%s/%s", control->basedir, control->directory);
}

/*
 *
 * 构造指定版本的辅助控制文件路径。
 */

static char *
get_extension_aux_control_filename(ExtensionControlFile *control,
								   const char *version)
{
	char	   *result;
	char	   *scriptdir;

	scriptdir = get_extension_script_directory(control);

	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/%s--%s.control",
			 scriptdir, control->name, version);

	pfree(scriptdir);

	return result;
}

/*
 *
 * 构造从 from_version 升级到 version，或直接安装 version 的脚本路径。
 */

static char *
get_extension_script_filename(ExtensionControlFile *control,
							  const char *from_version, const char *version)
{
	char	   *result;
	char	   *scriptdir;

	scriptdir = get_extension_script_directory(control);

	result = (char *) palloc(MAXPGPATH);
	if (from_version)
		snprintf(result, MAXPGPATH, "%s/%s--%s--%s.sql",
				 scriptdir, control->name, from_version, version);
	else
		snprintf(result, MAXPGPATH, "%s/%s--%s.sql",
				 scriptdir, control->name, version);

	pfree(scriptdir);

	return result;
}


/*
 * Parse contents of primary or auxiliary control file, and fill in
 * fields of *control.  We parse primary file if version == NULL,
 * else the optional auxiliary file for that version.
 *
 * 解析主控制文件或辅助控制文件的内容，并填入 *control 的字段。
 * version 为 NULL 时解析主文件，否则解析该版本可选的辅助文件。
 *
 * The control file will be search on Extension_control_path paths if
 * control->control_dir is NULL, otherwise it will use the value of control_dir
 * to read and parse the .control file, so it assume that the control_dir is a
 * valid path for the control file being parsed.
 *
 * 若 control->control_dir 为 NULL，
 * 则在 Extension_control_path 的路径上搜索控制文件，
 * 否则用 control_dir 的值来读取并解析 .control 文件，
 * 因此假定 control_dir 是正在解析的控制文件的有效路径。
 *
 * Control files are supposed to be very short, half a dozen lines,
 * so we don't worry about memory allocation risks here.  Also we don't
 * worry about what encoding it's in; all values are expected to be ASCII.
 *
 * 控制文件应该很短，大约半打行，因此这里不担心内存分配风险。
 * 也不关心它的编码；所有值都预期是 ASCII。
 */
static void
parse_extension_control_file(ExtensionControlFile *control,
							 const char *version)
{
	char	   *filename;
	FILE	   *file;
	ConfigVariable *item,
			   *head = NULL,
			   *tail = NULL;

	/*
	 * Locate the file to read.  Auxiliary files are optional.
	 *
	 * 定位要读取的文件。辅助文件是可选的。
	 */
	if (version)
		filename = get_extension_aux_control_filename(control, version);
	else
	{
		/*
		 * If control_dir is already set, use it, else do a path search.
		 *
		 * 若 control_dir 已经设置则使用它，否则做路径搜索。
		 */
		if (control->control_dir)
		{
			filename = psprintf("%s/%s.control", control->control_dir, control->name);
		}
		else
			filename = find_extension_control_filename(control);
	}

	if (!filename)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" is not available", control->name),
				 errhint("The extension must first be installed on the system where PostgreSQL is running.")));
	}

	/* Assert that the control_dir ends with /extension */
	/*
	 *
	 * 断言 control_dir 以 /extension 结尾
	 */
	Assert(control->control_dir != NULL);
	Assert(strcmp(control->control_dir + strlen(control->control_dir) - strlen("/extension"), "/extension") == 0);

	control->basedir = pnstrdup(
								control->control_dir,
								strlen(control->control_dir) - strlen("/extension"));

	if ((file = AllocateFile(filename, "r")) == NULL)
	{
		/* no complaint for missing auxiliary file */
		/*
		 *
		 * 辅助文件缺失时不抱怨
		 */
		if (errno == ENOENT && version)
		{
			pfree(filename);
			return;
		}

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open extension control file \"%s\": %m",
						filename)));
	}

	/*
	 * Parse the file content, using GUC's file parsing code.  We need not
	 * check the return value since any errors will be thrown at ERROR level.
	 *
	 * 用 GUC 的文件解析代码解析文件内容。不必检查返回值，
	 * 因为任何错误都会以 ERROR 级别抛出。
	 */
	(void) ParseConfigFp(file, filename, CONF_FILE_START_DEPTH, ERROR,
						 &head, &tail);

	FreeFile(file);

	/*
	 * Convert the ConfigVariable list into ExtensionControlFile entries.
	 *
	 * 把 ConfigVariable 列表转换成
	 * ExtensionControlFile 的字段。
	 */
	for (item = head; item != NULL; item = item->next)
	{
		if (strcmp(item->name, "directory") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->directory = pstrdup(item->value);
		}
		else if (strcmp(item->name, "default_version") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->default_version = pstrdup(item->value);
		}
		else if (strcmp(item->name, "module_pathname") == 0)
		{
			control->module_pathname = pstrdup(item->value);
		}
		else if (strcmp(item->name, "comment") == 0)
		{
			control->comment = pstrdup(item->value);
		}
		else if (strcmp(item->name, "schema") == 0)
		{
			control->schema = pstrdup(item->value);
		}
		else if (strcmp(item->name, "relocatable") == 0)
		{
			if (!parse_bool(item->value, &control->relocatable))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "superuser") == 0)
		{
			if (!parse_bool(item->value, &control->superuser))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "trusted") == 0)
		{
			if (!parse_bool(item->value, &control->trusted))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "encoding") == 0)
		{
			control->encoding = pg_valid_server_encoding(item->value);
			if (control->encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("\"%s\" is not a valid encoding name",
								item->value)));
		}
		else if (strcmp(item->name, "requires") == 0)
		{
			/* Need a modifiable copy of string */
			/*
			 *
			 * 需要一份可修改的字符串副本
			 */
			char	   *rawnames = pstrdup(item->value);

			/* Parse string into list of identifiers */
			/*
			 *
			 * 把字符串解析成标识符列表
			 */
			if (!SplitIdentifierString(rawnames, ',', &control->requires))
			{
				/* syntax error in name list */
				/*
				 *
				 * 名称列表有语法错误
				 */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else if (strcmp(item->name, "no_relocate") == 0)
		{
			/* Need a modifiable copy of string */
			/*
			 *
			 * 需要一份可修改的字符串副本
			 */
			char	   *rawnames = pstrdup(item->value);

			/* Parse string into list of identifiers */
			/*
			 *
			 * 把字符串解析成标识符列表
			 */
			if (!SplitIdentifierString(rawnames, ',', &control->no_relocate))
			{
				/* syntax error in name list */
				/*
				 *
				 * 名称列表有语法错误
				 */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized parameter \"%s\" in file \"%s\"",
							item->name, filename)));
	}

	FreeConfigVariables(head);

	if (control->relocatable && control->schema != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"schema\" cannot be specified when \"relocatable\" is true")));

	pfree(filename);
}

/*
 * Read the primary control file for the specified extension.
 *
 * 读取指定扩展的主控制文件。
 */
static ExtensionControlFile *
read_extension_control_file(const char *extname)
{
	ExtensionControlFile *control = new_ExtensionControlFile(extname);

	/*
	 * Parse the primary control file.
	 *
	 * 解析主控制文件。
	 */
	parse_extension_control_file(control, NULL);

	return control;
}

/*
 * Read the auxiliary control file for the specified extension and version.
 *
 * 读取指定扩展和版本的辅助控制文件。
 *
 * Returns a new modified ExtensionControlFile struct; the original struct
 * (reflecting just the primary control file) is not modified.
 *
 * 返回一个新的、已修改的 ExtensionControlFile 结构；
 * 原始结构（只反映主控制文件）不会被修改。
 */
static ExtensionControlFile *
read_extension_aux_control_file(const ExtensionControlFile *pcontrol,
								const char *version)
{
	ExtensionControlFile *acontrol;

	/*
	 * Flat-copy the struct.  Pointer fields share values with original.
	 *
	 * 浅拷贝该结构。指针字段与原始结构共享值。
	 */
	acontrol = (ExtensionControlFile *) palloc(sizeof(ExtensionControlFile));
	memcpy(acontrol, pcontrol, sizeof(ExtensionControlFile));

	/*
	 * Parse the auxiliary control file, overwriting struct fields
	 *
	 * 解析辅助控制文件，覆盖结构字段
	 */
	parse_extension_control_file(acontrol, version);

	return acontrol;
}

/*
 * Read an SQL script file into a string, and convert to database encoding
 *
 * 把 SQL 脚本文件读入字符串，并转换到数据库编码
 */
static char *
read_extension_script_file(const ExtensionControlFile *control,
						   const char *filename)
{
	int			src_encoding;
	char	   *src_str;
	char	   *dest_str;
	int			len;

	src_str = read_whole_file(filename, &len);

	/* use database encoding if not given */
	/*
	 *
	 * 若未给出则使用数据库编码
	 */
	if (control->encoding < 0)
		src_encoding = GetDatabaseEncoding();
	else
		src_encoding = control->encoding;

	/* make sure that source string is valid in the expected encoding */
	/*
	 *
	 * 确认源字符串在预期编码中有效
	 */
	(void) pg_verify_mbstr(src_encoding, src_str, len, false);

	/*
	 * Convert the encoding to the database encoding. read_whole_file
	 * null-terminated the string, so if no conversion happens the string is
	 * valid as is.
	 *
	 * 把编码转换到数据库编码。read_whole_file 已给字符串加了结尾的空字符，
	 * 因此若不发生转换，该字符串原样有效。
	 */
	dest_str = pg_any_to_server(src_str, len, src_encoding);

	return dest_str;
}

/*
 * error context callback for failures in script-file execution
 *
 * 脚本文件执行失败时的错误上下文回调
 */
static void
script_error_callback(void *arg)
{
	script_error_callback_arg *callback_arg = (script_error_callback_arg *) arg;
	const char *query = callback_arg->sql;
	int			location = callback_arg->stmt_location;
	int			len = callback_arg->stmt_len;
	int			syntaxerrposition;
	const char *lastslash;

	/*
	 * If there is a syntax error position, convert to internal syntax error;
	 * otherwise report the current query as an item of context stack.
	 *
	 * 若有语法错误位置，则转换成内部语法错误；否则把当前查询作为上下文栈的一项报告。
	 *
	 * Note: we'll provide no context except the filename if there's neither
	 * an error position nor any known current query.  That shouldn't happen
	 * though: all errors reported during raw parsing should come with an
	 * error position.
	 *
	 * 注意：若既没有错误位置也没有已知的当前查询，则除文件名外不提供上下文。
	 * 不过这不该发生：原始解析期间报告的所有错误都应带有错误位置。
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		/*
		 * If we do not know the bounds of the current statement (as would
		 * happen for an error occurring during initial raw parsing), we have
		 * to use a heuristic to decide how much of the script to show.  We'll
		 * also use the heuristic in the unlikely case that syntaxerrposition
		 * is outside what we think the statement bounds are.
		 *
		 * 若不知道当前语句的边界（例如错误发生在最初的原始解析期间），
		 * 必须用启发式决定展示脚本的多少内容。若 syntaxerrposition
		 * 落在我们认为的语句边界之外，
		 * 这种不太可能的情况也会使用该启发式。
		 */
		if (location < 0 || syntaxerrposition < location ||
			(len > 0 && syntaxerrposition > location + len))
		{
			/*
			 * Our heuristic is pretty simple: look for semicolon-newline
			 * sequences, and break at the last one strictly before
			 * syntaxerrposition and the first one strictly after.  It's
			 * certainly possible to fool this with semicolon-newline embedded
			 * in a string literal, but it seems better to do this than to
			 * show the entire extension script.
			 *
			 * 启发式很简单：查找分号加换行的序列，在严格位于 syntaxerrposition
			 * 之前的最后一个
			 * 以及严格位于其后的第一个处断开。字符串字面量中嵌入的分号加换行可以骗过它，
			 * 但这样做似乎好过展示整个扩展脚本。
			 *
			 * Notice we cope with Windows-style newlines (\r\n) regardless of
			 * platform.  This is because there might be such newlines in
			 * script files on other platforms.
			 *
			 * 注意无论平台如何，我们都处理 Windows 风格的换行（\r\n）。
			 * 因为其他平台上的脚本文件里也可能有这种换行。
			 */
			int			slen = strlen(query);

			location = len = 0;
			for (int loc = 0; loc < slen; loc++)
			{
				if (query[loc] != ';')
					continue;
				if (query[loc + 1] == '\r')
					loc++;
				if (query[loc + 1] == '\n')
				{
					int			bkpt = loc + 2;

					if (bkpt < syntaxerrposition)
						location = bkpt;
					else if (bkpt > syntaxerrposition)
					{
						len = bkpt - location;
						break;	/* no need to keep searching */
						/*
						 *
						 * 不必继续搜索
						 */
					}
				}
			}
		}

		/* Trim leading/trailing whitespace, for consistency */
		/*
		 *
		 * 去掉首尾空白，以保持一致
		 */
		query = CleanQuerytext(query, &location, &len);

		/*
		 * Adjust syntaxerrposition.  It shouldn't be pointing into the
		 * whitespace we just trimmed, but cope if it is.
		 *
		 * 调整 syntaxerrposition。它不该指向我们刚裁掉的空白，
		 * 但若是这样也要应付。
		 */
		syntaxerrposition -= location;
		if (syntaxerrposition < 0)
			syntaxerrposition = 0;
		else if (syntaxerrposition > len)
			syntaxerrposition = len;

		/* And report. */
		/*
		 *
		 * 然后报告。
		 */
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(pnstrdup(query, len));
	}
	else if (location >= 0)
	{
		/*
		 * Since no syntax cursor will be shown, it's okay and helpful to trim
		 * the reported query string to just the current statement.
		 *
		 * 既然不会显示语法光标，把所报告的查询字符串裁成只含当前语句是可以的，也有帮助。
		 */
		query = CleanQuerytext(query, &location, &len);
		errcontext("SQL statement \"%.*s\"", len, query);
	}

	/*
	 * Trim the reported file name to remove the path.  We know that
	 * get_extension_script_filename() inserted a '/', regardless of whether
	 * we're on Windows.
	 *
	 * 把所报告的文件名去掉路径。我们知道 get_extension_script_fil
	 * ename() 插入了 “/”，
	 * 无论是否在 Windows 上。
	 */
	lastslash = strrchr(callback_arg->filename, '/');
	if (lastslash)
		lastslash++;
	else
		lastslash = callback_arg->filename; /* shouldn't happen, but cope */
		/*
		 *
		 * 不该发生，但还是应付
		 */

	/*
	 * If we have a location (which, as said above, we really always should)
	 * then report a line number to aid in localizing problems in big scripts.
	 *
	 * 若有位置（如上所述，我们其实总是应该有），则报告行号，以便在大脚本中定位问题。
	 */
	if (location >= 0)
	{
		int			linenumber = 1;

		for (query = callback_arg->sql; *query; query++)
		{
			if (--location < 0)
				break;
			if (*query == '\n')
				linenumber++;
		}
		errcontext("extension script file \"%s\", near line %d",
				   lastslash, linenumber);
	}
	else
		errcontext("extension script file \"%s\"", lastslash);
}

/*
 * Execute given SQL string.
 *
 * 执行给定的 SQL 字符串。
 *
 * The filename the string came from is also provided, for error reporting.
 *
 * 同时提供该字符串来自的文件名，用于错误报告。
 *
 * Note: it's tempting to just use SPI to execute the string, but that does
 * not work very well.  The really serious problem is that SPI will parse,
 * analyze, and plan the whole string before executing any of it; of course
 * this fails if there are any plannable statements referring to objects
 * created earlier in the script.  A lesser annoyance is that SPI insists
 * on printing the whole string as errcontext in case of any error, and that
 * could be very long.
 *
 * 注意：直接用 SPI 执行该字符串很诱人，但效果不好。
 * 真正严重的问题是 SPI 会在执行任何部分之前
 * 解析、分析并规划整个字符串；若有可规划语句引用脚本前面创建的对象，这就会失败。
 * 较小的麻烦是出错时 SPI 坚持把整个字符串作为 errcontext 打印，
 * 而它可能非常长。
 */
static void
execute_sql_string(const char *sql, const char *filename)
{
	script_error_callback_arg callback_arg;
	ErrorContextCallback scripterrcontext;
	List	   *raw_parsetree_list;
	DestReceiver *dest;
	ListCell   *lc1;

	/*
	 * Setup error traceback support for ereport().
	 *
	 * 为 ereport() 设置错误回溯支持。
	 */
	callback_arg.sql = sql;
	callback_arg.filename = filename;
	callback_arg.stmt_location = -1;
	callback_arg.stmt_len = -1;

	scripterrcontext.callback = script_error_callback;
	scripterrcontext.arg = (void *) &callback_arg;
	scripterrcontext.previous = error_context_stack;
	error_context_stack = &scripterrcontext;

	/*
	 * Parse the SQL string into a list of raw parse trees.
	 *
	 * 把 SQL 字符串解析成原始解析树列表。
	 */
	raw_parsetree_list = pg_parse_query(sql);

	/* All output from SELECTs goes to the bit bucket */
	/*
	 *
	 * SELECT 的全部输出都丢弃
	 */
	dest = CreateDestReceiver(DestNone);

	/*
	 * Do parse analysis, rule rewrite, planning, and execution for each raw
	 * parsetree.  We must fully execute each query before beginning parse
	 * analysis on the next one, since there may be interdependencies.
	 *
	 * 对每棵原始解析树做解析分析、规则重写、规划与执行。
	 * 必须在开始分析下一条之前完整执行当前查询，因为它们之间可能有依赖。
	 */
	foreach(lc1, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
		MemoryContext per_parsetree_context,
					oldcontext;
		List	   *stmt_list;
		ListCell   *lc2;

		/* Report location of this query for error context callback */
		/*
		 *
		 * 为错误上下文回调报告本查询的位置
		 */
		callback_arg.stmt_location = parsetree->stmt_location;
		callback_arg.stmt_len = parsetree->stmt_len;

		/*
		 * We do the work for each parsetree in a short-lived context, to
		 * limit the memory used when there are many commands in the string.
		 *
		 * 每棵解析树的工作放在短命上下文中，以限制字符串里有很多命令时的内存使用。
		 */
		per_parsetree_context =
			AllocSetContextCreate(CurrentMemoryContext,
								  "execute_sql_string per-statement context",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(per_parsetree_context);

		/* Be sure parser can see any DDL done so far */
		/*
		 *
		 * 确保解析器能看到到目前为止所做的 DDL
		 */
		CommandCounterIncrement();

		stmt_list = pg_analyze_and_rewrite_fixedparams(parsetree,
													   sql,
													   NULL,
													   0,
													   NULL);
		stmt_list = pg_plan_queries(stmt_list, sql, CURSOR_OPT_PARALLEL_OK, NULL);

		foreach(lc2, stmt_list)
		{
			PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);

			CommandCounterIncrement();

			PushActiveSnapshot(GetTransactionSnapshot());

			if (stmt->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;

				qdesc = CreateQueryDesc(stmt,
										sql,
										GetActiveSnapshot(), NULL,
										dest, NULL, NULL, 0);

				ExecutorStart(qdesc, 0);
				ExecutorRun(qdesc, ForwardScanDirection, 0);
				ExecutorFinish(qdesc);
				ExecutorEnd(qdesc);

				FreeQueryDesc(qdesc);
			}
			else
			{
				if (IsA(stmt->utilityStmt, TransactionStmt))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("transaction control statements are not allowed within an extension script")));

				ProcessUtility(stmt,
							   sql,
							   false,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
			}

			PopActiveSnapshot();
		}

		/* Clean up per-parsetree context. */
		/*
		 *
		 * 清理每棵解析树的上下文。
		 */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(per_parsetree_context);
	}

	error_context_stack = scripterrcontext.previous;

	/* Be sure to advance the command counter after the last script command */
	/*
	 *
	 * 确保在最后一条脚本命令之后推进命令计数器
	 */
	CommandCounterIncrement();
}

/*
 * Policy function: is the given extension trusted for installation by a
 * non-superuser?
 *
 * 策略函数：给定扩展是否被信任，可由非超级用户安装？
 *
 * (Update the errhint logic below if you change this.)
 *
 * （若改变这一点，请同时更新下面的 errhint 逻辑。）
 */
static bool
extension_is_trusted(ExtensionControlFile *control)
{
	AclResult	aclresult;

	/* Never trust unless extension's control file says it's okay */
	/*
	 *
	 * 除非扩展的控制文件说可以，否则永不信任
	 */
	if (!control->trusted)
		return false;
	/* Allow if user has CREATE privilege on current database */
	/*
	 *
	 * 若用户对当前数据库有 CREATE 权限则允许
	 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult == ACLCHECK_OK)
		return true;
	return false;
}

/*
 * Execute the appropriate script file for installing or updating the extension
 *
 * 执行用于安装或更新扩展的相应脚本文件
 *
 * If from_version isn't NULL, it's an update
 *
 * 若 from_version 不是 NULL，则这是一次更新
 *
 * Note: requiredSchemas must be one-for-one with the control->requires list
 *
 * 注意：requiredSchemas 必须与 control->requires
 * 列表一一对应
 */
static void
execute_extension_script(Oid extensionOid, ExtensionControlFile *control,
						 const char *from_version,
						 const char *version,
						 List *requiredSchemas,
						 const char *schemaName)
{
	bool		switch_to_superuser = false;
	char	   *filename;
	Oid			save_userid = 0;
	int			save_sec_context = 0;
	int			save_nestlevel;
	StringInfoData pathbuf;
	ListCell   *lc;
	ListCell   *lc2;

	/*
	 * Enforce superuser-ness if appropriate.  We postpone these checks until
	 * here so that the control flags are correctly associated with the right
	 * script(s) if they happen to be set in secondary control files.
	 *
	 * 在适当时强制要求超级用户。把这些检查推迟到这里，
	 * 以便控制标志若设在次级控制文件中，能正确关联到对应的脚本。
	 */
	if (control->superuser && !superuser())
	{
		if (extension_is_trusted(control))
			switch_to_superuser = true;
		else if (from_version == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to create this extension.")
					 : errhint("Must be superuser to create this extension.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to update extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to update this extension.")
					 : errhint("Must be superuser to update this extension.")));
	}

	filename = get_extension_script_filename(control, from_version, version);

	if (from_version == NULL)
		elog(DEBUG1, "executing extension script for \"%s\" version '%s'", control->name, version);
	else
		elog(DEBUG1, "executing extension script for \"%s\" update from version '%s' to '%s'", control->name, from_version, version);

	/*
	 * If installing a trusted extension on behalf of a non-superuser, become
	 * the bootstrap superuser.  (This switch will be cleaned up automatically
	 * if the transaction aborts, as will the GUC changes below.)
	 *
	 * 若代表非超级用户安装受信任扩展，则变成引导超级用户。
	 * （若事务中止，这次切换会自动清理，下面的 GUC 变更也一样。）
	 */
	if (switch_to_superuser)
	{
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	}

	/*
	 * Force client_min_messages and log_min_messages to be at least WARNING,
	 * so that we won't spam the user with useless NOTICE messages from common
	 * script actions like creating shell types.
	 *
	 * 强制 client_min_messages 与 log_min_messages
	 * 至少为 WARNING，
	 * 以免创建 shell 类型这类常见脚本动作用无用的 NOTICE 打扰用户。
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the script execution.  guc.c also
	 * takes care of undoing the setting on error.
	 *
	 * 我们使用等价于函数 SET 选项的方式，使该设置恰好在脚本执行期间持续。
	 * guc.c 也会在出错时负责撤销该设置。
	 *
	 * log_min_messages can't be set by ordinary users, so for that one we
	 * pretend to be superuser.
	 *
	 * 普通用户不能设置 log_min_messages，因此对这一项我们假装是超级用户。
	 */
	save_nestlevel = NewGUCNestLevel();

	if (client_min_messages < WARNING)
		(void) set_config_option("client_min_messages", "warning",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (log_min_messages < WARNING)
		(void) set_config_option_ext("log_min_messages", "warning",
									 PGC_SUSET, PGC_S_SESSION,
									 BOOTSTRAP_SUPERUSERID,
									 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Similarly disable check_function_bodies, to ensure that SQL functions
	 * won't be parsed during creation.
	 *
	 * 同样禁用 check_function_bodies，
	 * 以确保创建期间不会解析 SQL 函数。
	 */
	if (check_function_bodies)
		(void) set_config_option("check_function_bodies", "off",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Set up the search path to have the target schema first, making it be
	 * the default creation target namespace.  Then add the schemas of any
	 * prerequisite extensions, unless they are in pg_catalog which would be
	 * searched anyway.  (Listing pg_catalog explicitly in a non-first
	 * position would be bad for security.)  Finally add pg_temp to ensure
	 * that temp objects can't take precedence over others.
	 *
	 * 把搜索路径设成目标模式在最前，使它成为默认的创建目标命名空间。
	 * 然后加入任何前置扩展的模式，除非它们在 pg_catalog 中（反正会被搜索）。
	 * （把 pg_catalog 显式列在非首位对安全有害。）最后加入 pg_temp，
	 * 确保临时对象不能优先于其他对象。
	 */
	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));
	foreach(lc, requiredSchemas)
	{
		Oid			reqschema = lfirst_oid(lc);
		char	   *reqname = get_namespace_name(reqschema);

		if (reqname && strcmp(reqname, "pg_catalog") != 0)
			appendStringInfo(&pathbuf, ", %s", quote_identifier(reqname));
	}
	appendStringInfoString(&pathbuf, ", pg_temp");

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Set creating_extension and related variables so that
	 * recordDependencyOnCurrentExtension and other functions do the right
	 * things.  On failure, ensure we reset these variables.
	 *
	 * 设置 creating_extension 及相关变量，
	 * 使 recordDependencyOnCurrentExtension
	 * 等函数做正确的事。
	 * 失败时确保重置这些变量。
	 */
	creating_extension = true;
	CurrentExtensionObject = extensionOid;
	PG_TRY();
	{
		char	   *c_sql = read_extension_script_file(control, filename);
		Datum		t_sql;

		/*
		 * We filter each substitution through quote_identifier().  When the
		 * arg contains one of the following characters, no one collection of
		 * quoting can work inside $$dollar-quoted string literals$$,
		 * 'single-quoted string literals', and outside of any literal.  To
		 * avoid a security snare for extension authors, error on substitution
		 * for arguments containing these.
		 *
		 * 每个替换都经过 quote_identifier() 过滤。当参数含有下列字符之一时，
		 * 没有任何一种引号方式能同时在 $$美元引号字符串$$、
		 * '单引号字符串' 以及任何字面量之外工作。
		 * 为避免给扩展作者设下安全陷阱，对含有这些字符的参数拒绝替换。
		 */
		const char *quoting_relevant_chars = "\"$'\\";

		/* We use various functions that want to operate on text datums */
		/*
		 *
		 * 我们使用若干希望对 text 数据操作的函数
		 */
		t_sql = CStringGetTextDatum(c_sql);

		/*
		 * Reduce any lines beginning with "\echo" to empty.  This allows
		 * scripts to contain messages telling people not to run them via
		 * psql, which has been found to be necessary due to old habits.
		 *
		 * 把以 \echo 开头的行变成空行。这样脚本可以包含告诉人们不要用 psql
		 * 运行它们的消息；
		 * 由于旧习惯，这被发现是必要的。
		 */
		t_sql = DirectFunctionCall4Coll(textregexreplace,
										C_COLLATION_OID,
										t_sql,
										CStringGetTextDatum("^\\\\echo.*$"),
										CStringGetTextDatum(""),
										CStringGetTextDatum("ng"));

		/*
		 * If the script uses @extowner@, substitute the calling username.
		 *
		 * 若脚本使用 @extowner@，则替换成调用者的用户名。
		 */
		if (strstr(c_sql, "@extowner@"))
		{
			Oid			uid = switch_to_superuser ? save_userid : GetUserId();
			const char *userName = GetUserNameFromId(uid, false);
			const char *qUserName = quote_identifier(userName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extowner@"),
											CStringGetTextDatum(qUserName));
			if (strpbrk(userName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension owner: must not contain any of \"%s\"",
								quoting_relevant_chars)));
		}

		/*
		 * If it's not relocatable, substitute the target schema name for
		 * occurrences of @extschema@.
		 *
		 * 若不可重定位，则把出现的 @extschema@ 替换成目标模式名。
		 *
		 * For a relocatable extension, we needn't do this.  There cannot be
		 * any need for @extschema@, else it wouldn't be relocatable.
		 *
		 * 对可重定位扩展不必这样做。不可能需要 @extschema@，否则它就不可重定位。
		 */
		if (!control->relocatable)
		{
			Datum		old = t_sql;
			const char *qSchemaName = quote_identifier(schemaName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extschema@"),
											CStringGetTextDatum(qSchemaName));
			if (t_sql != old && strpbrk(schemaName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension \"%s\" schema: must not contain any of \"%s\"",
								control->name, quoting_relevant_chars)));
		}

		/*
		 * Likewise, substitute required extensions' schema names for
		 * occurrences of @extschema:extension_name@.
		 *
		 * 同样，把出现的 @extschema:extension_name@
		 * 替换成所需扩展的模式名。
		 */
		Assert(list_length(control->requires) == list_length(requiredSchemas));
		forboth(lc, control->requires, lc2, requiredSchemas)
		{
			Datum		old = t_sql;
			char	   *reqextname = (char *) lfirst(lc);
			Oid			reqschema = lfirst_oid(lc2);
			char	   *schemaName = get_namespace_name(reqschema);
			const char *qSchemaName = quote_identifier(schemaName);
			char	   *repltoken;

			repltoken = psprintf("@extschema:%s@", reqextname);
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum(repltoken),
											CStringGetTextDatum(qSchemaName));
			if (t_sql != old && strpbrk(schemaName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension \"%s\" schema: must not contain any of \"%s\"",
								reqextname, quoting_relevant_chars)));
		}

		/*
		 * If module_pathname was set in the control file, substitute its
		 * value for occurrences of MODULE_PATHNAME.
		 *
		 * 若控制文件设置了 module_pathname，
		 * 则把出现的 MODULE_PATHNAME 替换成它的值。
		 */
		if (control->module_pathname)
		{
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("MODULE_PATHNAME"),
											CStringGetTextDatum(control->module_pathname));
		}

		/* And now back to C string */
		/*
		 *
		 * 然后回到 C 字符串
		 */
		c_sql = text_to_cstring(DatumGetTextPP(t_sql));

		execute_sql_string(c_sql, filename);
	}
	PG_FINALLY();
	{
		creating_extension = false;
		CurrentExtensionObject = InvalidOid;
	}
	PG_END_TRY();

	/*
	 * Restore the GUC variables we set above.
	 *
	 * 恢复我们在上面设置的 GUC 变量。
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/*
	 * Restore authentication state if needed.
	 *
	 * 如有需要，恢复认证状态。
	 */
	if (switch_to_superuser)
		SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * Find or create an ExtensionVersionInfo for the specified version name
 *
 * 查找或创建指定版本名的 ExtensionVersionInfo
 *
 * Currently, we just use a List of the ExtensionVersionInfo's.  Searching
 * for them therefore uses about O(N^2) time when there are N versions of
 * the extension.  We could change the data structure to a hash table if
 * this ever becomes a bottleneck.
 *
 * 目前只用 ExtensionVersionInfo 的 List。
 * 因此当扩展有 N 个版本时，查找大约是 O(N^2)。
 * 若这成为瓶颈，可以把数据结构改成哈希表。
 */
static ExtensionVersionInfo *
get_ext_ver_info(const char *versionname, List **evi_list)
{
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	foreach(lc, *evi_list)
	{
		evi = (ExtensionVersionInfo *) lfirst(lc);
		if (strcmp(evi->name, versionname) == 0)
			return evi;
	}

	evi = (ExtensionVersionInfo *) palloc(sizeof(ExtensionVersionInfo));
	evi->name = pstrdup(versionname);
	evi->reachable = NIL;
	evi->installable = false;
	/* initialize for later application of Dijkstra's algorithm */
	/*
	 *
	 * 为稍后应用 Dijkstra 算法做初始化
	 */
	evi->distance_known = false;
	evi->distance = INT_MAX;
	evi->previous = NULL;

	*evi_list = lappend(*evi_list, evi);

	return evi;
}

/*
 * Locate the nearest unprocessed ExtensionVersionInfo
 *
 * 定位最近的、尚未处理的 ExtensionVersionInfo
 *
 * This part of the algorithm is also about O(N^2).  A priority queue would
 * make it much faster, but for now there's no need.
 *
 * 算法的这一部分也大约是 O(N^2)。优先队列会快得多，但目前没有必要。
 */
static ExtensionVersionInfo *
get_nearest_unprocessed_vertex(List *evi_list)
{
	ExtensionVersionInfo *evi = NULL;
	ListCell   *lc;

	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);

		/* only vertices whose distance is still uncertain are candidates */
		/*
		 *
		 * 只有距离仍不确定的顶点才是候选
		 */
		if (evi2->distance_known)
			continue;
		/* remember the closest such vertex */
		/*
		 *
		 * 记住最近的那个顶点
		 */
		if (evi == NULL ||
			evi->distance > evi2->distance)
			evi = evi2;
	}

	return evi;
}

/*
 * Obtain information about the set of update scripts available for the
 * specified extension.  The result is a List of ExtensionVersionInfo
 * structs, each with a subsidiary list of the ExtensionVersionInfos for
 * the versions that can be reached in one step from that version.
 *
 * 取得指定扩展可用更新脚本集合的信息。结果是 ExtensionVersionInfo
 * 结构的 List，
 * 每个都带有一个从属列表，列出从该版本一步可以到达的版本的
 * ExtensionVersionInfo。
 */
static List *
get_ext_ver_list(ExtensionControlFile *control)
{
	List	   *evi_list = NIL;
	int			extnamelen = strlen(control->name);
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	location = get_extension_script_directory(control);
	dir = AllocateDir(location);
	while ((de = ReadDir(dir, location)) != NULL)
	{
		char	   *vername;
		char	   *vername2;
		ExtensionVersionInfo *evi;
		ExtensionVersionInfo *evi2;

		/* must be a .sql file ... */
		/*
		 *
		 * 必须是 .sql 文件……
		 */
		if (!is_extension_script_filename(de->d_name))
			continue;

		/* ... matching extension name followed by separator */
		/*
		 *
		 * ……并且匹配扩展名后跟分隔符
		 */
		if (strncmp(de->d_name, control->name, extnamelen) != 0 ||
			de->d_name[extnamelen] != '-' ||
			de->d_name[extnamelen + 1] != '-')
			continue;

		/* extract version name(s) from 'extname--something.sql' filename */
		/*
		 *
		 * 从 extname--something.sql 文件名中提取版本名
		 */
		vername = pstrdup(de->d_name + extnamelen + 2);
		*strrchr(vername, '.') = '\0';
		vername2 = strstr(vername, "--");
		if (!vername2)
		{
			/* It's an install, not update, script; record its version name */
			/*
			 *
			 * 这是安装脚本而不是更新脚本；记录其版本名
			 */
			evi = get_ext_ver_info(vername, &evi_list);
			evi->installable = true;
			continue;
		}
		*vername2 = '\0';		/* terminate first version */
		/*
		 *
		 * 结束第一个版本名
		 */
		vername2 += 2;			/* and point to second */
		/*
		 *
		 * 并指向第二个
		 */

		/* if there's a third --, it's bogus, ignore it */
		/*
		 *
		 * 若还有第三个 --，则是非法的，忽略它
		 */
		if (strstr(vername2, "--"))
			continue;

		/* Create ExtensionVersionInfos and link them together */
		/*
		 *
		 * 创建 ExtensionVersionInfo 并把它们链接起来
		 */
		evi = get_ext_ver_info(vername, &evi_list);
		evi2 = get_ext_ver_info(vername2, &evi_list);
		evi->reachable = lappend(evi->reachable, evi2);
	}
	FreeDir(dir);

	return evi_list;
}

/*
 * Given an initial and final version name, identify the sequence of update
 * scripts that have to be applied to perform that update.
 *
 * 给定初始版本名与最终版本名，确定执行该更新必须应用的更新脚本序列。
 *
 * Result is a List of names of versions to transition through (the initial
 * version is *not* included).
 *
 * 结果是要经过的版本名 List（不包括初始版本）。
 */
static List *
identify_update_path(ExtensionControlFile *control,
					 const char *oldVersion, const char *newVersion)
{
	List	   *result;
	List	   *evi_list;
	ExtensionVersionInfo *evi_start;
	ExtensionVersionInfo *evi_target;

	/* Extract the version update graph from the script directory */
	/*
	 *
	 * 从脚本目录提取版本更新图
	 */
	evi_list = get_ext_ver_list(control);

	/* Initialize start and end vertices */
	/*
	 *
	 * 初始化起点与终点顶点
	 */
	evi_start = get_ext_ver_info(oldVersion, &evi_list);
	evi_target = get_ext_ver_info(newVersion, &evi_list);

	/* Find shortest path */
	/*
	 *
	 * 寻找最短路径
	 */
	result = find_update_path(evi_list, evi_start, evi_target, false, false);

	if (result == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("extension \"%s\" has no update path from version \"%s\" to version \"%s\"",
						control->name, oldVersion, newVersion)));

	return result;
}

/*
 * Apply Dijkstra's algorithm to find the shortest path from evi_start to
 * evi_target.
 *
 * 用 Dijkstra 算法寻找从 evi_start 到 evi_target
 * 的最短路径。
 *
 * If reject_indirect is true, ignore paths that go through installable
 * versions.  This saves work when the caller will consider starting from
 * all installable versions anyway.
 *
 * 若 reject_indirect 为 true，则忽略经过可安装版本的路径。
 * 当调用者反正会考虑从所有可安装版本出发时，这可以省工作。
 *
 * If reinitialize is false, assume the ExtensionVersionInfo list has not
 * been used for this before, and the initialization done by get_ext_ver_info
 * is still good.  Otherwise, reinitialize all transient fields used here.
 *
 * 若 reinitialize 为 false，
 * 假定这份 ExtensionVersionInfo 列表此前没被这样用过，
 * get_ext_ver_info 所做的初始化仍然有效。
 * 否则重新初始化这里用到的全部临时字段。
 *
 * Result is a List of names of versions to transition through (the initial
 * version is *not* included).  Returns NIL if no such path.
 *
 * 结果是要经过的版本名 List（不包括初始版本）。若没有这样的路径则返回 NIL。
 */
static List *
find_update_path(List *evi_list,
				 ExtensionVersionInfo *evi_start,
				 ExtensionVersionInfo *evi_target,
				 bool reject_indirect,
				 bool reinitialize)
{
	List	   *result;
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	/* Caller error if start == target */
	/*
	 *
	 * 若 start 等于 target，则是调用者错误
	 */
	Assert(evi_start != evi_target);
	/* Caller error if reject_indirect and target is installable */
	/*
	 *
	 * 若 reject_indirect 且目标可安装，则是调用者错误
	 */
	Assert(!(reject_indirect && evi_target->installable));

	if (reinitialize)
	{
		foreach(lc, evi_list)
		{
			evi = (ExtensionVersionInfo *) lfirst(lc);
			evi->distance_known = false;
			evi->distance = INT_MAX;
			evi->previous = NULL;
		}
	}

	evi_start->distance = 0;

	while ((evi = get_nearest_unprocessed_vertex(evi_list)) != NULL)
	{
		if (evi->distance == INT_MAX)
			break;				/* all remaining vertices are unreachable */
			/*
			 *
			 * 其余顶点都不可达
			 */
		evi->distance_known = true;
		if (evi == evi_target)
			break;				/* found shortest path to target */
			/*
			 *
			 * 已找到到目标的最短路径
			 */
		foreach(lc, evi->reachable)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);
			int			newdist;

			/* if reject_indirect, treat installable versions as unreachable */
			/*
			 *
			 * 若 reject_indirect，把可安装版本当作不可达
			 */
			if (reject_indirect && evi2->installable)
				continue;
			newdist = evi->distance + 1;
			if (newdist < evi2->distance)
			{
				evi2->distance = newdist;
				evi2->previous = evi;
			}
			else if (newdist == evi2->distance &&
					 evi2->previous != NULL &&
					 strcmp(evi->name, evi2->previous->name) < 0)
			{
				/*
				 * Break ties in favor of the version name that comes first
				 * according to strcmp().  This behavior is undocumented and
				 * users shouldn't rely on it.  We do it just to ensure that
				 * if there is a tie, the update path that is chosen does not
				 * depend on random factors like the order in which directory
				 * entries get visited.
				 *
				 * 平局时偏向按 strcmp() 排在前面的版本名。该行为未写入文档，用户不应依赖它。
				 * 我们这样做只是为了保证平局时所选更新路径不取决于目录项访问顺序这类随机因素。
				 */
				evi2->previous = evi;
			}
		}
	}

	/* Return NIL if target is not reachable from start */
	/*
	 *
	 * 若从起点无法到达目标，则返回 NIL
	 */
	if (!evi_target->distance_known)
		return NIL;

	/* Build and return list of version names representing the update path */
	/*
	 *
	 * 建立并返回表示更新路径的版本名列表
	 */
	result = NIL;
	for (evi = evi_target; evi != evi_start; evi = evi->previous)
		result = lcons(evi->name, result);

	return result;
}

/*
 * Given a target version that is not directly installable, find the
 * best installation sequence starting from a directly-installable version.
 *
 * 给定一个不能直接安装的目标版本，寻找从某个可直接安装的版本出发的最佳安装序列。
 *
 * evi_list: previously-collected version update graph
 * evi_target: member of that list that we want to reach
 *
 * evi_list：先前收集的版本更新图
 * evi_target：该列表中我们想到达的成员
 *
 * Returns the best starting-point version, or NULL if there is none.
 * On success, *best_path is set to the path from the start point.
 *
 * 返回最佳起点版本；若没有则返回 NULL。成功时把 *best_path
 * 设为从该起点出发的路径。
 *
 * If there's more than one possible start point, prefer shorter update paths,
 * and break any ties arbitrarily on the basis of strcmp'ing the starting
 * versions' names.
 *
 * 若有多个可能的起点，优先更短的更新路径，并用起点版本名的 strcmp 任意打破平局。
 */
static ExtensionVersionInfo *
find_install_path(List *evi_list, ExtensionVersionInfo *evi_target,
				  List **best_path)
{
	ExtensionVersionInfo *evi_start = NULL;
	ListCell   *lc;

	*best_path = NIL;

	/*
	 * We don't expect to be called for an installable target, but if we are,
	 * the answer is easy: just start from there, with an empty update path.
	 *
	 * 我们不期望对可安装的目标被调用，但若是这样，答案很简单：就从那里开始，更新路径为空。
	 */
	if (evi_target->installable)
		return evi_target;

	/* Consider all installable versions as start points */
	/*
	 *
	 * 把所有可安装版本都当作起点来考虑
	 */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc);
		List	   *path;

		if (!evi1->installable)
			continue;

		/*
		 * Find shortest path from evi1 to evi_target; but no need to consider
		 * paths going through other installable versions.
		 *
		 * 寻找从 evi1 到 evi_target 的最短路径；
		 * 但不必考虑经过其他可安装版本的路径。
		 */
		path = find_update_path(evi_list, evi1, evi_target, true, true);
		if (path == NIL)
			continue;

		/* Remember best path */
		/*
		 *
		 * 记住最佳路径
		 */
		if (evi_start == NULL ||
			list_length(path) < list_length(*best_path) ||
			(list_length(path) == list_length(*best_path) &&
			 strcmp(evi_start->name, evi1->name) < 0))
		{
			evi_start = evi1;
			*best_path = path;
		}
	}

	return evi_start;
}

/*
 * CREATE EXTENSION worker
 *
 * CREATE EXTENSION 的实际工作函数
 *
 * When CASCADE is specified, CreateExtensionInternal() recurses if required
 * extensions need to be installed.  To sanely handle cyclic dependencies,
 * the "parents" list contains a list of names of extensions already being
 * installed, allowing us to error out if we recurse to one of those.
 *
 * 指定 CASCADE 时，若所需扩展需要安装，
 * CreateExtensionInternal() 会递归。
 * 为合理地处理循环依赖，parents 列表含有正在安装的扩展名，
 * 若递归到其中之一则报错。
 */
static ObjectAddress
CreateExtensionInternal(char *extensionName,
						char *schemaName,
						const char *versionName,
						bool cascade,
						List *parents,
						bool is_create)
{
	char	   *origSchemaName = schemaName;
	Oid			schemaOid = InvalidOid;
	Oid			extowner = GetUserId();
	ExtensionControlFile *pcontrol;
	ExtensionControlFile *control;
	char	   *filename;
	struct stat fst;
	List	   *updateVersions;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Oid			extensionOid;
	ObjectAddress address;
	ListCell   *lc;

	/*
	 * Read the primary control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding at this
	 * point.
	 *
	 * 读取主控制文件。注意假定它不含任何非 ASCII 数据，因此此时不必担心编码。
	 */
	pcontrol = read_extension_control_file(extensionName);

	/*
	 * Determine the version to install
	 *
	 * 确定要安装的版本
	 */
	if (versionName == NULL)
	{
		if (pcontrol->default_version)
			versionName = pcontrol->default_version;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("version to install must be specified")));
	}
	check_valid_version_name(versionName);

	/*
	 * Figure out which script(s) we need to run to install the desired
	 * version of the extension.  If we do not have a script that directly
	 * does what is needed, we try to find a sequence of update scripts that
	 * will get us there.
	 *
	 * 弄清要运行哪些脚本来安装扩展的目标版本。
	 * 若没有直接完成所需工作的脚本，则尝试找到一组能到达那里的更新脚本。
	 */
	filename = get_extension_script_filename(pcontrol, NULL, versionName);
	if (stat(filename, &fst) == 0)
	{
		/* Easy, no extra scripts */
		/*
		 *
		 * 简单情况，没有额外脚本
		 */
		updateVersions = NIL;
	}
	else
	{
		/* Look for best way to install this version */
		/*
		 *
		 * 寻找安装该版本的最佳方式
		 */
		List	   *evi_list;
		ExtensionVersionInfo *evi_start;
		ExtensionVersionInfo *evi_target;

		/* Extract the version update graph from the script directory */
		/*
		 *
		 * 从脚本目录提取版本更新图
		 */
		evi_list = get_ext_ver_list(pcontrol);

		/* Identify the target version */
		/*
		 *
		 * 确定目标版本
		 */
		evi_target = get_ext_ver_info(versionName, &evi_list);

		/* Identify best path to reach target */
		/*
		 *
		 * 确定到达目标的最佳路径
		 */
		evi_start = find_install_path(evi_list, evi_target,
									  &updateVersions);

		/* Fail if no path ... */
		/*
		 *
		 * 若没有路径则失败……
		 */
		if (evi_start == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("extension \"%s\" has no installation script nor update path for version \"%s\"",
							pcontrol->name, versionName)));

		/* Otherwise, install best starting point and then upgrade */
		/*
		 *
		 * 否则先安装最佳起点，然后再升级
		 */
		versionName = evi_start->name;
	}

	/*
	 * Fetch control parameters for installation target version
	 *
	 * 取安装目标版本的控制参数
	 */
	control = read_extension_aux_control_file(pcontrol, versionName);

	/*
	 * Determine the target schema to install the extension into
	 *
	 * 确定要把扩展安装进的目标模式
	 */
	if (schemaName)
	{
		/* If the user is giving us the schema name, it must exist already. */
		/*
		 *
		 * 若用户给出了模式名，该模式必须已经存在。
		 */
		schemaOid = get_namespace_oid(schemaName, false);
	}

	if (control->schema != NULL)
	{
		/*
		 * The extension is not relocatable and the author gave us a schema
		 * for it.
		 *
		 * 扩展不可重定位，且作者为我们指定了它的模式。
		 *
		 * Unless CASCADE parameter was given, it's an error to give a schema
		 * different from control->schema if control->schema is specified.
		 *
		 * 除非给出了 CASCADE 参数，否则若 control->schema 已指定，
		 * 给出与它不同的模式就是错误。
		 */
		if (schemaName && strcmp(control->schema, schemaName) != 0 &&
			!cascade)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" must be installed in schema \"%s\"",
							control->name,
							control->schema)));

		/* Always use the schema from control file for current extension. */
		/*
		 *
		 * 当前扩展始终使用控制文件中的模式。
		 */
		schemaName = control->schema;

		/* Find or create the schema in case it does not exist. */
		/*
		 *
		 * 若模式不存在则查找或创建它。
		 */
		schemaOid = get_namespace_oid(schemaName, true);

		if (!OidIsValid(schemaOid))
		{
			CreateSchemaStmt *csstmt = makeNode(CreateSchemaStmt);

			csstmt->schemaname = schemaName;
			csstmt->authrole = NULL;	/* will be created by current user */
			/*
			 *
			 * 将由当前用户创建
			 */
			csstmt->schemaElts = NIL;
			csstmt->if_not_exists = false;
			CreateSchemaCommand(csstmt, "(generated CREATE SCHEMA command)",
								-1, -1);

			/*
			 * CreateSchemaCommand includes CommandCounterIncrement, so new
			 * schema is now visible.
			 *
			 * CreateSchemaCommand 包含
			 * CommandCounterIncrement，因此新模式现在可见。
			 */
			schemaOid = get_namespace_oid(schemaName, false);
		}
	}
	else if (!OidIsValid(schemaOid))
	{
		/*
		 * Neither user nor author of the extension specified schema; use the
		 * current default creation namespace, which is the first explicit
		 * entry in the search_path.
		 *
		 * 用户和扩展作者都没有指定模式；使用当前默认的创建命名空间，
		 * 即 search_path 中第一个显式项。
		 */
		List	   *search_path = fetch_search_path(false);

		if (search_path == NIL) /* nothing valid in search_path? */
		/*
		 *
		 * search_path 中没有有效项？
		 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
		schemaOid = linitial_oid(search_path);
		schemaName = get_namespace_name(schemaOid);
		if (schemaName == NULL) /* recently-deleted namespace? */
		/*
		 *
		 * 最近被删除的命名空间？
		 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));

		list_free(search_path);
	}

	/*
	 * Make note if a temporary namespace has been accessed in this
	 * transaction.
	 *
	 * 若本事务访问过临时命名空间，则记下来。
	 */
	if (isTempNamespace(schemaOid))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/*
	 * We don't check creation rights on the target namespace here.  If the
	 * extension script actually creates any objects there, it will fail if
	 * the user doesn't have such permissions.  But there are cases such as
	 * procedural languages where it's convenient to set schema = pg_catalog
	 * yet we don't want to restrict the command to users with ACL_CREATE for
	 * pg_catalog.
	 *
	 * 这里不检查对目标命名空间的创建权限。若扩展脚本真的在那里创建任何对象，
	 * 用户没有相应权限时会失败。但有些情况（例如过程语言）把 schema 设为
	 * pg_catalog 很方便，
	 * 而我们不想把该命令限制为对 pg_catalog 有 ACL_CREATE 的用户。
	 */

	/*
	 * Look up the prerequisite extensions, install them if necessary, and
	 * build lists of their OIDs and the OIDs of their target schemas.
	 *
	 * 查找前置扩展，必要时安装它们，并建立它们的 OID 列表及其目标模式的 OID 列表。
	 */
	requiredExtensions = NIL;
	requiredSchemas = NIL;
	foreach(lc, control->requires)
	{
		char	   *curreq = (char *) lfirst(lc);
		Oid			reqext;
		Oid			reqschema;

		reqext = get_required_extension(curreq,
										extensionName,
										origSchemaName,
										cascade,
										parents,
										is_create);
		reqschema = get_extension_schema(reqext);
		requiredExtensions = lappend_oid(requiredExtensions, reqext);
		requiredSchemas = lappend_oid(requiredSchemas, reqschema);
	}

	/*
	 * Insert new tuple into pg_extension, and create dependency entries.
	 *
	 * 向 pg_extension 插入新元组，并创建依赖项。
	 */
	address = InsertExtensionTuple(control->name, extowner,
								   schemaOid, control->relocatable,
								   versionName,
								   PointerGetDatum(NULL),
								   PointerGetDatum(NULL),
								   requiredExtensions);
	extensionOid = address.objectId;

	/*
	 * Apply any control-file comment on extension
	 *
	 * 若控制文件有注释，则应用到扩展上
	 */
	if (control->comment != NULL)
		CreateComments(extensionOid, ExtensionRelationId, 0, control->comment);

	/*
	 * Execute the installation script file
	 *
	 * 执行安装脚本文件
	 */
	execute_extension_script(extensionOid, control,
							 NULL, versionName,
							 requiredSchemas,
							 schemaName);

	/*
	 * If additional update scripts have to be executed, apply the updates as
	 * though a series of ALTER EXTENSION UPDATE commands were given
	 *
	 * 若还必须执行额外的更新脚本，则像给出一系列 ALTER EXTENSION
	 * UPDATE 命令那样应用这些更新
	 */
	ApplyExtensionUpdates(extensionOid, pcontrol,
						  versionName, updateVersions,
						  origSchemaName, cascade, is_create);

	return address;
}

/*
 * Get the OID of an extension listed in "requires", possibly creating it.
 *
 * 取得 requires 中列出的某个扩展的 OID，必要时创建它。
 */
static Oid
get_required_extension(char *reqExtensionName,
					   char *extensionName,
					   char *origSchemaName,
					   bool cascade,
					   List *parents,
					   bool is_create)
{
	Oid			reqExtensionOid;

	reqExtensionOid = get_extension_oid(reqExtensionName, true);
	if (!OidIsValid(reqExtensionOid))
	{
		if (cascade)
		{
			/* Must install it. */
			/*
			 *
			 * 必须安装它。
			 */
			ObjectAddress addr;
			List	   *cascade_parents;
			ListCell   *lc;

			/* Check extension name validity before trying to cascade. */
			/*
			 *
			 * 在尝试级联之前先检查扩展名是否合法。
			 */
			check_valid_extension_name(reqExtensionName);

			/* Check for cyclic dependency between extensions. */
			/*
			 *
			 * 检查扩展之间的循环依赖。
			 */
			foreach(lc, parents)
			{
				char	   *pname = (char *) lfirst(lc);

				if (strcmp(pname, reqExtensionName) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg("cyclic dependency detected between extensions \"%s\" and \"%s\"",
									reqExtensionName, extensionName)));
			}

			ereport(NOTICE,
					(errmsg("installing required extension \"%s\"",
							reqExtensionName)));

			/* Add current extension to list of parents to pass down. */
			/*
			 *
			 * 把当前扩展加入要向下传递的父列表。
			 */
			cascade_parents = lappend(list_copy(parents), extensionName);

			/*
			 * Create the required extension.  We propagate the SCHEMA option
			 * if any, and CASCADE, but no other options.
			 *
			 * 创建所需的扩展。我们传播 SCHEMA 选项（若有）和 CASCADE，
			 * 但不传播其他选项。
			 */
			addr = CreateExtensionInternal(reqExtensionName,
										   origSchemaName,
										   NULL,
										   cascade,
										   cascade_parents,
										   is_create);

			/* Get its newly-assigned OID. */
			/*
			 *
			 * 取得它新分配的 OID。
			 */
			reqExtensionOid = addr.objectId;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("required extension \"%s\" is not installed",
							reqExtensionName),
					 is_create ?
					 errhint("Use CREATE EXTENSION ... CASCADE to install required extensions too.") : 0));
	}

	return reqExtensionOid;
}

/*
 * 核心流程概览：
 * CreateExtension：读取主控制文件，确定版本与前置扩展，必要时 CASCADE 安装依赖，
 * 写入 pg_extension 后执行安装脚本，并按更新路径执行后续脚本。
 * find_update_path 在版本脚本图上用 Dijkstra 寻找最短更新路径。
 * ApplyExtensionUpdates：逐个执行更新脚本，并改写 pg_extension 与依赖。
 * ExecAlterExtensionContentsStmt：ALTER EXTENSION ADD/DROP，维护成员依赖与 extconfig。
 * AlterExtensionNamespace：在 relocatable 扩展上执行 ALTER EXTENSION SET SCHEMA。
 */

/*
 * CREATE EXTENSION
 *
 * CREATE EXTENSION
 */
ObjectAddress
CreateExtension(ParseState *pstate, CreateExtensionStmt *stmt)
{
	DefElem    *d_schema = NULL;
	DefElem    *d_new_version = NULL;
	DefElem    *d_cascade = NULL;
	char	   *schemaName = NULL;
	char	   *versionName = NULL;
	bool		cascade = false;
	ListCell   *lc;

	/* Check extension name validity before any filesystem access */
	/*
	 *
	 * 在任何文件系统访问之前检查扩展名是否合法
	 */
	check_valid_extension_name(stmt->extname);

	/*
	 * Check for duplicate extension name.  The unique index on
	 * pg_extension.extname would catch this anyway, and serves as a backstop
	 * in case of race conditions; but this is a friendlier error message, and
	 * besides we need a check to support IF NOT EXISTS.
	 *
	 * 检查扩展名是否重复。pg_extension.extname
	 * 上的唯一索引反正会抓住这一点，
	 * 并在竞态时作为后盾；但这里的错误信息更友好，
	 * 而且我们需要这次检查来支持 IF NOT EXISTS。
	 */
	if (get_extension_oid(stmt->extname, true) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists, skipping",
							stmt->extname)));
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists",
							stmt->extname)));
	}

	/*
	 * We use global variables to track the extension being created, so we can
	 * create only one extension at the same time.
	 *
	 * 我们用全局变量跟踪正在创建的扩展，因此同一时间只能创建一个扩展。
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested CREATE EXTENSION is not supported")));

	/* Deconstruct the statement option list */
	/*
	 *
	 * 拆解语句的选项列表
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "schema") == 0)
		{
			if (d_schema)
				errorConflictingDefElem(defel, pstate);
			d_schema = defel;
			schemaName = defGetString(d_schema);
		}
		else if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				errorConflictingDefElem(defel, pstate);
			d_new_version = defel;
			versionName = defGetString(d_new_version);
		}
		else if (strcmp(defel->defname, "cascade") == 0)
		{
			if (d_cascade)
				errorConflictingDefElem(defel, pstate);
			d_cascade = defel;
			cascade = defGetBoolean(d_cascade);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/* Call CreateExtensionInternal to do the real work. */
	/*
	 *
	 * 调用 CreateExtensionInternal 做实际工作。
	 */
	return CreateExtensionInternal(stmt->extname,
								   schemaName,
								   versionName,
								   cascade,
								   NIL,
								   true);
}

/*
 * InsertExtensionTuple
 *
 * InsertExtensionTuple
 *
 * Insert the new pg_extension row, and create extension's dependency entries.
 * Return the OID assigned to the new row.
 *
 * 插入新的 pg_extension 行，并创建扩展的依赖项。返回分配给新行的 OID。
 *
 * This is exported for the benefit of pg_upgrade, which has to create a
 * pg_extension entry (and the extension-level dependencies) without
 * actually running the extension's script.
 *
 * 导出此函数是为了 pg_upgrade：它必须创建 pg_extension
 * 项（以及扩展级依赖），
 * 而不真正运行扩展的脚本。
 *
 * extConfig and extCondition should be arrays or PointerGetDatum(NULL).
 * We declare them as plain Datum to avoid needing array.h in extension.h.
 *
 * extConfig 与 extCondition 应为数组或
 * PointerGetDatum(NULL)。
 * 把它们声明为普通 Datum，以免 extension.h 需要 array.h。
 */
ObjectAddress
InsertExtensionTuple(const char *extName, Oid extOwner,
					 Oid schemaOid, bool relocatable, const char *extVersion,
					 Datum extConfig, Datum extCondition,
					 List *requiredExtensions)
{
	Oid			extensionOid;
	Relation	rel;
	Datum		values[Natts_pg_extension];
	bool		nulls[Natts_pg_extension];
	HeapTuple	tuple;
	ObjectAddress myself;
	ObjectAddress nsp;
	ObjectAddresses *refobjs;
	ListCell   *lc;

	/*
	 * Build and insert the pg_extension tuple
	 *
	 * 构造并插入 pg_extension 元组
	 */
	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	extensionOid = GetNewOidWithIndex(rel, ExtensionOidIndexId,
									  Anum_pg_extension_oid);
	values[Anum_pg_extension_oid - 1] = ObjectIdGetDatum(extensionOid);
	values[Anum_pg_extension_extname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(extName));
	values[Anum_pg_extension_extowner - 1] = ObjectIdGetDatum(extOwner);
	values[Anum_pg_extension_extnamespace - 1] = ObjectIdGetDatum(schemaOid);
	values[Anum_pg_extension_extrelocatable - 1] = BoolGetDatum(relocatable);
	values[Anum_pg_extension_extversion - 1] = CStringGetTextDatum(extVersion);

	if (extConfig == PointerGetDatum(NULL))
		nulls[Anum_pg_extension_extconfig - 1] = true;
	else
		values[Anum_pg_extension_extconfig - 1] = extConfig;

	if (extCondition == PointerGetDatum(NULL))
		nulls[Anum_pg_extension_extcondition - 1] = true;
	else
		values[Anum_pg_extension_extcondition - 1] = extCondition;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);

	/*
	 * Record dependencies on owner, schema, and prerequisite extensions
	 *
	 * 记录对属主、模式和前置扩展的依赖
	 */
	recordDependencyOnOwner(ExtensionRelationId, extensionOid, extOwner);

	refobjs = new_object_addresses();

	ObjectAddressSet(myself, ExtensionRelationId, extensionOid);

	ObjectAddressSet(nsp, NamespaceRelationId, schemaOid);
	add_exact_object_address(&nsp, refobjs);

	foreach(lc, requiredExtensions)
	{
		Oid			reqext = lfirst_oid(lc);
		ObjectAddress otherext;

		ObjectAddressSet(otherext, ExtensionRelationId, reqext);
		add_exact_object_address(&otherext, refobjs);
	}

	/* Record all of them (this includes duplicate elimination) */
	/*
	 *
	 * 把它们全部记录下来（这包括消除重复）
	 */
	record_object_address_dependencies(&myself, refobjs, DEPENDENCY_NORMAL);
	free_object_addresses(refobjs);

	/* Post creation hook for new extension */
	/*
	 *
	 * 新扩展的创建后钩子
	 */
	InvokeObjectPostCreateHook(ExtensionRelationId, extensionOid, 0);

	return myself;
}

/*
 * Guts of extension deletion.
 *
 * 扩展删除的核心实现。
 *
 * All we need do here is remove the pg_extension tuple itself.  Everything
 * else is taken care of by the dependency infrastructure.
 *
 * 这里只需删除 pg_extension 元组本身。其余都由依赖基础设施处理。
 */
void
RemoveExtensionById(Oid extId)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Disallow deletion of any extension that's currently open for insertion;
	 * else subsequent executions of recordDependencyOnCurrentExtension()
	 * could create dangling pg_depend records that refer to a no-longer-valid
	 * pg_extension OID.  This is needed not so much because we think people
	 * might write "DROP EXTENSION foo" in foo's own script files, as because
	 * errors in dependency management in extension script files could give
	 * rise to cases where an extension is dropped as a result of recursing
	 * from some contained object.  Because of that, we must test for the case
	 * here, not at some higher level of the DROP EXTENSION command.
	 *
	 * 不允许删除当前正打开以供插入的任何扩展；否则随后执行
	 * recordDependencyOnCurrentExtension()
	 * 可能创建指向不再有效的 pg_extension OID 的悬空 pg_depend
	 * 记录。
	 * 需要这样做，与其说是因为我们认为有人会在 foo 自己的脚本里写 DROP
	 * EXTENSION foo，
	 * 不如说是因为扩展脚本中的依赖管理错误可能导致从某个所含对象递归时删除扩展。
	 * 因此必须在这里测试这种情况，而不是在 DROP EXTENSION 命令的更高层。
	 */
	if (extId == CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot drop extension \"%s\" because it is being modified",
						get_extension_name(extId))));

	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extId));
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	/*
	 *
	 * 假定最多只有一条匹配元组
	 */
	if (HeapTupleIsValid(tuple))
		CatalogTupleDelete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	table_close(rel, RowExclusiveLock);
}

/*
 * This function lists the available extensions (one row per primary control
 * file in the control directory).  We parse each control file and report the
 * interesting fields.
 *
 * 本函数列出可用扩展（控制目录中每个主控制文件一行）。
 * 我们解析每个控制文件并报告有用的字段。
 *
 * The system view pg_available_extensions provides a user interface to this
 * SRF, adding information about whether the extensions are installed in the
 * current DB.
 *
 * 系统视图 pg_available_extensions 为本 SRF 提供用户接口，
 * 并加上这些扩展是否已安装在当前数据库中的信息。
 */
Datum
pg_available_extensions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *locations;
	DIR		   *dir;
	struct dirent *de;
	List	   *found_ext = NIL;

	/* Build tuplestore to hold the result rows */
	/*
	 *
	 * 建立用于保存结果行的 tuplestore
	 */
	InitMaterializedSRF(fcinfo, 0);

	locations = get_extension_control_directories();

	foreach_ptr(char, location, locations)
	{
		dir = AllocateDir(location);

		/*
		 * If the control directory doesn't exist, we want to silently return
		 * an empty set.  Any other error will be reported by ReadDir.
		 *
		 * 若控制目录不存在，我们希望静默返回空集。任何其他错误将由 ReadDir 报告。
		 */
		if (dir == NULL && errno == ENOENT)
		{
			/* do nothing */
			/*
			 *
			 * 什么也不做
			 */
		}
		else
		{
			while ((de = ReadDir(dir, location)) != NULL)
			{
				ExtensionControlFile *control;
				char	   *extname;
				String	   *extname_str;
				Datum		values[3];
				bool		nulls[3];

				if (!is_extension_control_filename(de->d_name))
					continue;

				/* extract extension name from 'name.control' filename */
				/*
				 *
				 * 从 name.control 文件名中提取扩展名
				 */
				extname = pstrdup(de->d_name);
				*strrchr(extname, '.') = '\0';

				/* ignore it if it's an auxiliary control file */
				/*
				 *
				 * 若是辅助控制文件则忽略
				 */
				if (strstr(extname, "--"))
					continue;

				/*
				 * Ignore already-found names.  They are not reachable by the
				 * path search, so don't shown them.
				 *
				 * 忽略已经找到的名称。路径搜索到不了它们，因此不要显示。
				 */
				extname_str = makeString(extname);
				if (list_member(found_ext, extname_str))
					continue;
				else
					found_ext = lappend(found_ext, extname_str);

				control = new_ExtensionControlFile(extname);
				control->control_dir = pstrdup(location);
				parse_extension_control_file(control, NULL);

				memset(values, 0, sizeof(values));
				memset(nulls, 0, sizeof(nulls));

				/* name */
				/*
				 *
				 * 名称
				 */
				values[0] = DirectFunctionCall1(namein,
												CStringGetDatum(control->name));
				/* default_version */
				/*
				 *
				 * default_version
				 */
				if (control->default_version == NULL)
					nulls[1] = true;
				else
					values[1] = CStringGetTextDatum(control->default_version);
				/* comment */
				/*
				 *
				 * 注释
				 */
				if (control->comment == NULL)
					nulls[2] = true;
				else
					values[2] = CStringGetTextDatum(control->comment);

				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
									 values, nulls);
			}

			FreeDir(dir);
		}
	}

	return (Datum) 0;
}

/*
 * This function lists the available extension versions (one row per
 * extension installation script).  For each version, we parse the related
 * control file(s) and report the interesting fields.
 *
 * 本函数列出可用的扩展版本（每个扩展安装脚本一行）。对每个版本，
 * 我们解析相关控制文件并报告有用的字段。
 *
 * The system view pg_available_extension_versions provides a user interface
 * to this SRF, adding information about which versions are installed in the
 * current DB.
 *
 * 系统视图 pg_available_extension_versions 为本
 * SRF 提供用户接口，并加上哪些版本已安装在当前数据库中的信息。
 */
Datum
pg_available_extension_versions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *locations;
	DIR		   *dir;
	struct dirent *de;
	List	   *found_ext = NIL;

	/* Build tuplestore to hold the result rows */
	/*
	 *
	 * 建立用于保存结果行的 tuplestore
	 */
	InitMaterializedSRF(fcinfo, 0);

	locations = get_extension_control_directories();

	foreach_ptr(char, location, locations)
	{
		dir = AllocateDir(location);

		/*
		 * If the control directory doesn't exist, we want to silently return
		 * an empty set.  Any other error will be reported by ReadDir.
		 *
		 * 若控制目录不存在，我们希望静默返回空集。任何其他错误将由 ReadDir 报告。
		 */
		if (dir == NULL && errno == ENOENT)
		{
			/* do nothing */
			/*
			 *
			 * 什么也不做
			 */
		}
		else
		{
			while ((de = ReadDir(dir, location)) != NULL)
			{
				ExtensionControlFile *control;
				char	   *extname;
				String	   *extname_str;

				if (!is_extension_control_filename(de->d_name))
					continue;

				/* extract extension name from 'name.control' filename */
				/*
				 *
				 * 从 name.control 文件名中提取扩展名
				 */
				extname = pstrdup(de->d_name);
				*strrchr(extname, '.') = '\0';

				/* ignore it if it's an auxiliary control file */
				/*
				 *
				 * 若是辅助控制文件则忽略
				 */
				if (strstr(extname, "--"))
					continue;

				/*
				 * Ignore already-found names.  They are not reachable by the
				 * path search, so don't shown them.
				 *
				 * 忽略已经找到的名称。路径搜索到不了它们，因此不要显示。
				 */
				extname_str = makeString(extname);
				if (list_member(found_ext, extname_str))
					continue;
				else
					found_ext = lappend(found_ext, extname_str);

				/* read the control file */
				/*
				 *
				 * 读取控制文件
				 */
				control = new_ExtensionControlFile(extname);
				control->control_dir = pstrdup(location);
				parse_extension_control_file(control, NULL);

				/* scan extension's script directory for install scripts */
				/*
				 *
				 * 扫描扩展的脚本目录以寻找安装脚本
				 */
				get_available_versions_for_extension(control, rsinfo->setResult,
													 rsinfo->setDesc);
			}

			FreeDir(dir);
		}
	}

	return (Datum) 0;
}

/*
 * Inner loop for pg_available_extension_versions:
 *		read versions of one extension, add rows to tupstore
 *
 * pg_available_extension_versions
 * 的内层循环：读取一个扩展的各个版本，并把行加入 tupstore
 */
static void
get_available_versions_for_extension(ExtensionControlFile *pcontrol,
									 Tuplestorestate *tupstore,
									 TupleDesc tupdesc)
{
	List	   *evi_list;
	ListCell   *lc;

	/* Extract the version update graph from the script directory */
	/*
	 *
	 * 从脚本目录提取版本更新图
	 */
	evi_list = get_ext_ver_list(pcontrol);

	/* For each installable version ... */
	/*
	 *
	 * 对每个可安装版本……
	 */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi = (ExtensionVersionInfo *) lfirst(lc);
		ExtensionControlFile *control;
		Datum		values[8];
		bool		nulls[8];
		ListCell   *lc2;

		if (!evi->installable)
			continue;

		/*
		 * Fetch parameters for specific version (pcontrol is not changed)
		 *
		 * 取特定版本的参数（pcontrol 不会被改变）
		 */
		control = read_extension_aux_control_file(pcontrol, evi->name);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* name */
		/*
		 *
		 * 名称
		 */
		values[0] = DirectFunctionCall1(namein,
										CStringGetDatum(control->name));
		/* version */
		/*
		 *
		 * 版本
		 */
		values[1] = CStringGetTextDatum(evi->name);
		/* superuser */
		/*
		 *
		 * superuser
		 */
		values[2] = BoolGetDatum(control->superuser);
		/* trusted */
		/*
		 *
		 * trusted
		 */
		values[3] = BoolGetDatum(control->trusted);
		/* relocatable */
		/*
		 *
		 * relocatable
		 */
		values[4] = BoolGetDatum(control->relocatable);
		/* schema */
		/*
		 *
		 * schema
		 */
		if (control->schema == NULL)
			nulls[5] = true;
		else
			values[5] = DirectFunctionCall1(namein,
											CStringGetDatum(control->schema));
		/* requires */
		/*
		 *
		 * requires
		 */
		if (control->requires == NIL)
			nulls[6] = true;
		else
			values[6] = convert_requires_to_datum(control->requires);
		/* comment */
		/*
		 *
		 * 注释
		 */
		if (control->comment == NULL)
			nulls[7] = true;
		else
			values[7] = CStringGetTextDatum(control->comment);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/*
		 * Find all non-directly-installable versions that would be installed
		 * starting from this version, and report them, inheriting the
		 * parameters that aren't changed in updates from this version.
		 *
		 * 找出从该版本出发安装时会装上的、不能直接安装的全部版本，并报告它们，
		 * 继承更新中相对该版本没有改变的参数。
		 */
		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *best_path;

			if (evi2->installable)
				continue;
			if (find_install_path(evi_list, evi2, &best_path) == evi)
			{
				/*
				 * Fetch parameters for this version (pcontrol is not changed)
				 *
				 * 取该版本的参数（pcontrol 不会被改变）
				 */
				control = read_extension_aux_control_file(pcontrol, evi2->name);

				/* name stays the same */
				/*
				 *
				 * 名称保持不变
				 */
				/* version */
				/*
				 *
				 * 版本
				 */
				values[1] = CStringGetTextDatum(evi2->name);
				/* superuser */
				/*
				 *
				 * superuser
				 */
				values[2] = BoolGetDatum(control->superuser);
				/* trusted */
				/*
				 *
				 * trusted
				 */
				values[3] = BoolGetDatum(control->trusted);
				/* relocatable */
				/*
				 *
				 * relocatable
				 */
				values[4] = BoolGetDatum(control->relocatable);
				/* schema stays the same */
				/*
				 *
				 * 模式保持不变
				 */
				/* requires */
				/*
				 *
				 * requires
				 */
				if (control->requires == NIL)
					nulls[6] = true;
				else
				{
					values[6] = convert_requires_to_datum(control->requires);
					nulls[6] = false;
				}
				/* comment stays the same */
				/*
				 *
				 * 注释保持不变
				 */

				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
	}
}

/*
 * Test whether the given extension exists (not whether it's installed)
 *
 * 测试给定扩展是否存在（不是测试它是否已安装）
 *
 * This checks for the existence of a matching control file in the extension
 * directory.  That's not a bulletproof check, since the file might be
 * invalid, but this is only used for hints so it doesn't have to be 100%
 * right.
 *
 * 这检查扩展目录中是否有匹配的控制文件。这不是万无一失的检查，因为文件可能无效，
 * 但这只用于提示，所以不必百分之百正确。
 */
bool
extension_file_exists(const char *extensionName)
{
	bool		result = false;
	List	   *locations;
	DIR		   *dir;
	struct dirent *de;

	locations = get_extension_control_directories();

	foreach_ptr(char, location, locations)
	{
		dir = AllocateDir(location);

		/*
		 * If the control directory doesn't exist, we want to silently return
		 * false.  Any other error will be reported by ReadDir.
		 *
		 * 若控制目录不存在，我们希望静默返回 false。
		 * 任何其他错误将由 ReadDir 报告。
		 */
		if (dir == NULL && errno == ENOENT)
		{
			/* do nothing */
			/*
			 *
			 * 什么也不做
			 */
		}
		else
		{
			while ((de = ReadDir(dir, location)) != NULL)
			{
				char	   *extname;

				if (!is_extension_control_filename(de->d_name))
					continue;

				/* extract extension name from 'name.control' filename */
				/*
				 *
				 * 从 name.control 文件名中提取扩展名
				 */
				extname = pstrdup(de->d_name);
				*strrchr(extname, '.') = '\0';

				/* ignore it if it's an auxiliary control file */
				/*
				 *
				 * 若是辅助控制文件则忽略
				 */
				if (strstr(extname, "--"))
					continue;

				/* done if it matches request */
				/*
				 *
				 * 若与请求匹配则完成
				 */
				if (strcmp(extname, extensionName) == 0)
				{
					result = true;
					break;
				}
			}

			FreeDir(dir);
		}
		if (result)
			break;
	}

	return result;
}

/*
 * Convert a list of extension names to a name[] Datum
 *
 * 把扩展名列表转换成 name[] 的 Datum
 */
static Datum
convert_requires_to_datum(List *requires)
{
	Datum	   *datums;
	int			ndatums;
	ArrayType  *a;
	ListCell   *lc;

	ndatums = list_length(requires);
	datums = (Datum *) palloc(ndatums * sizeof(Datum));
	ndatums = 0;
	foreach(lc, requires)
	{
		char	   *curreq = (char *) lfirst(lc);

		datums[ndatums++] =
			DirectFunctionCall1(namein, CStringGetDatum(curreq));
	}
	a = construct_array_builtin(datums, ndatums, NAMEOID);
	return PointerGetDatum(a);
}

/*
 * This function reports the version update paths that exist for the
 * specified extension.
 *
 * 本函数报告指定扩展存在的版本更新路径。
 */
Datum
pg_extension_update_paths(PG_FUNCTION_ARGS)
{
	Name		extname = PG_GETARG_NAME(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *evi_list;
	ExtensionControlFile *control;
	ListCell   *lc1;

	/* Check extension name validity before any filesystem access */
	/*
	 *
	 * 在任何文件系统访问之前检查扩展名是否合法
	 */
	check_valid_extension_name(NameStr(*extname));

	/* Build tuplestore to hold the result rows */
	/*
	 *
	 * 建立用于保存结果行的 tuplestore
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* Read the extension's control file */
	/*
	 *
	 * 读取该扩展的控制文件
	 */
	control = read_extension_control_file(NameStr(*extname));

	/* Extract the version update graph from the script directory */
	/*
	 *
	 * 从脚本目录提取版本更新图
	 */
	evi_list = get_ext_ver_list(control);

	/* Iterate over all pairs of versions */
	/*
	 *
	 * 遍历所有版本对
	 */
	foreach(lc1, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc1);
		ListCell   *lc2;

		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *path;
			Datum		values[3];
			bool		nulls[3];

			if (evi1 == evi2)
				continue;

			/* Find shortest path from evi1 to evi2 */
			/*
			 *
			 * 寻找从 evi1 到 evi2 的最短路径
			 */
			path = find_update_path(evi_list, evi1, evi2, false, true);

			/* Emit result row */
			/*
			 *
			 * 输出结果行
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* source */
			/*
			 *
			 * 源版本
			 */
			values[0] = CStringGetTextDatum(evi1->name);
			/* target */
			/*
			 *
			 * 目标版本
			 */
			values[1] = CStringGetTextDatum(evi2->name);
			/* path */
			/*
			 *
			 * 路径
			 */
			if (path == NIL)
				nulls[2] = true;
			else
			{
				StringInfoData pathbuf;
				ListCell   *lcv;

				initStringInfo(&pathbuf);
				/* The path doesn't include start vertex, but show it */
				/*
				 *
				 * 路径不包括起点顶点，但显示它
				 */
				appendStringInfoString(&pathbuf, evi1->name);
				foreach(lcv, path)
				{
					char	   *versionName = (char *) lfirst(lcv);

					appendStringInfoString(&pathbuf, "--");
					appendStringInfoString(&pathbuf, versionName);
				}
				values[2] = CStringGetTextDatum(pathbuf.data);
				pfree(pathbuf.data);
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * pg_extension_config_dump
 *
 * pg_extension_config_dump
 *
 * Record information about a configuration table that belongs to an
 * extension being created, but whose contents should be dumped in whole
 * or in part during pg_dump.
 *
 * 记录属于正在创建的扩展、但在 pg_dump 期间应全部或部分转储其内容的配置表信息。
 */
Datum
pg_extension_config_dump(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *wherecond = PG_GETARG_TEXT_PP(1);
	char	   *tablename;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	Datum		elementDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/*
	 * We only allow this to be called from an extension's SQL script. We
	 * shouldn't need any permissions check beyond that.
	 *
	 * 只允许从扩展的 SQL 脚本中调用。除此之外不应需要任何权限检查。
	 */
	if (!creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s can only be called from an SQL script executed by CREATE EXTENSION",
						"pg_extension_config_dump()")));

	/*
	 * Check that the table exists and is a member of the extension being
	 * created.  This ensures that we don't need to register an additional
	 * dependency to protect the extconfig entry.
	 *
	 * 检查该表存在，并且是正在创建的扩展的成员。这确保我们不必再登记额外依赖来保护
	 * extconfig 项。
	 */
	tablename = get_rel_name(tableoid);
	if (tablename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("OID %u does not refer to a table", tableoid)));
	if (getExtensionOfObject(RelationRelationId, tableoid) !=
		CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" is not a member of the extension being created",
						tablename)));

	/*
	 * Add the table OID and WHERE condition to the extension's extconfig and
	 * extcondition arrays.
	 *
	 * 把表 OID 和 WHERE 条件加入扩展的 extconfig 与
	 * extcondition 数组。
	 *
	 * If the table is already in extconfig, treat this as an update of the
	 * WHERE condition.
	 *
	 * 若该表已在 extconfig 中，则把这当作对 WHERE 条件的更新。
	 */

	/* Find the pg_extension tuple */
	/*
	 *
	 * 找到 pg_extension 元组
	 */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(CurrentExtensionObject));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
	/*
	 *
	 * 不该发生
	 */
		elog(ERROR, "could not find tuple for extension %u",
			 CurrentExtensionObject);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Build or modify the extconfig value */
	/*
	 *
	 * 建立或修改 extconfig 值
	 */
	elementDatum = ObjectIdGetDatum(tableoid);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* Previously empty extconfig, so build 1-element array */
		/*
		 *
		 * 先前 extconfig 为空，因此建立单元素数组
		 */
		arrayLength = 0;
		arrayIndex = 1;

		a = construct_array_builtin(&elementDatum, 1, OIDOID);
	}
	else
	{
		/* Modify or extend existing extconfig array */
		/*
		 *
		 * 修改或扩展已有的 extconfig 数组
		 */
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = arrayLength + 1;	/* set up to add after end */
		/*
		 *
		 * 准备加到末尾之后
		 */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i + 1; /* replace this element instead */
				/*
				 *
				 * 改为替换这个元素
				 */
				break;
			}
		}

		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  /*
					   *
					   * 变长数组
					   */
					  sizeof(Oid) /* OID's typlen */ ,
					  /*
					   *
					   * OID 的 typlen
					   */
					  true /* OID's typbyval */ ,
					  /*
					   *
					   * OID 的 typbyval
					   */
					  TYPALIGN_INT /* OID's typalign */ );
					  /*
					   *
					   * OID 的 typalign
					   */
	}
	repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Build or modify the extcondition value */
	/*
	 *
	 * 建立或修改 extcondition 值
	 */
	elementDatum = PointerGetDatum(wherecond);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		if (arrayLength != 0)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		a = construct_array_builtin(&elementDatum, 1, TEXTOID);
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		/* Add or replace at same index as in extconfig */
		/*
		 *
		 * 在与 extconfig 相同的下标处添加或替换
		 */
		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena array */ ,
					  /*
					   *
					   * 变长数组
					   */
					  -1 /* TEXT's typlen */ ,
					  /*
					   *
					   * TEXT 的 typlen
					   */
					  false /* TEXT's typbyval */ ,
					  /*
					   *
					   * TEXT 的 typbyval
					   */
					  TYPALIGN_INT /* TEXT's typalign */ );
					  /*
					   *
					   * TEXT 的 typalign
					   */
	}
	repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * pg_get_loaded_modules
 *
 * pg_get_loaded_modules
 *
 * SQL-callable function to get per-loaded-module information.  Modules
 * (shared libraries) aren't necessarily one-to-one with extensions, but
 * they're sufficiently closely related to make this file a good home.
 *
 * 可从 SQL 调用的函数，用于取得每个已加载模块的信息。
 * 模块（共享库）不一定与扩展一一对应，
 * 但关系足够密切，放在本文件中合适。
 */
Datum
pg_get_loaded_modules(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	DynamicFileList *file_scanner;

	/* Build tuplestore to hold the result rows */
	/*
	 *
	 * 建立用于保存结果行的 tuplestore
	 */
	InitMaterializedSRF(fcinfo, 0);

	for (file_scanner = get_first_loaded_module(); file_scanner != NULL;
		 file_scanner = get_next_loaded_module(file_scanner))
	{
		const char *library_path,
				   *module_name,
				   *module_version;
		const char *sep;
		Datum		values[3] = {0};
		bool		nulls[3] = {0};

		get_loaded_module_details(file_scanner,
								  &library_path,
								  &module_name,
								  &module_version);

		if (module_name == NULL)
			nulls[0] = true;
		else
			values[0] = CStringGetTextDatum(module_name);
		if (module_version == NULL)
			nulls[1] = true;
		else
			values[1] = CStringGetTextDatum(module_version);

		/* For security reasons, we don't show the directory path */
		/*
		 *
		 * 出于安全原因，我们不显示目录路径
		 */
		sep = last_dir_separator(library_path);
		if (sep)
			library_path = sep + 1;
		values[2] = CStringGetTextDatum(library_path);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * extension_config_remove
 *
 * extension_config_remove
 *
 * Remove the specified table OID from extension's extconfig, if present.
 * This is not currently exposed as a function, but it could be;
 * for now, we just invoke it from ALTER EXTENSION DROP.
 *
 * 若存在，则从扩展的 extconfig 中移除指定的表 OID。目前没有作为函数暴露，
 * 但可以这么做；现在只从 ALTER EXTENSION DROP 调用它。
 */
static void
extension_config_remove(Oid extensionoid, Oid tableoid)
{
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/* Find the pg_extension tuple */
	/*
	 *
	 * 找到 pg_extension 元组
	 */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionoid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
	/*
	 *
	 * 不该发生
	 */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionoid);

	/* Search extconfig for the tableoid */
	/*
	 *
	 * 在 extconfig 中搜索该 tableoid
	 */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* nothing to do */
		/*
		 *
		 * 无需做任何事
		 */
		a = NULL;
		arrayLength = 0;
		arrayIndex = -1;
	}
	else
	{
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = -1;		/* flag for no deletion needed */
		/*
		 *
		 * 表示不需要删除的标志
		 */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i; /* index to remove */
				/*
				 *
				 * 要移除的下标
				 */
				break;
			}
		}
	}

	/* If tableoid is not in extconfig, nothing to do */
	/*
	 *
	 * 若 tableoid 不在 extconfig 中，则无需做任何事
	 */
	if (arrayIndex < 0)
	{
		systable_endscan(extScan);
		table_close(extRel, RowExclusiveLock);
		return;
	}

	/* Modify or delete the extconfig value */
	/*
	 *
	 * 修改或删除 extconfig 值
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (arrayLength <= 1)
	{
		/* removing only element, just set array to null */
		/*
		 *
		 * 只剩这一个元素，直接把数组设为 null
		 */
		repl_null[Anum_pg_extension_extconfig - 1] = true;
	}
	else
	{
		/* squeeze out the target element */
		/*
		 *
		 * 把目标元素挤出去
		 */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* We already checked there are no nulls */
		/*
		 *
		 * 我们已经检查过没有空值
		 */
		deconstruct_array_builtin(a, OIDOID, &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array_builtin(dvalues, arrayLength - 1, OIDOID);

		repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* Modify or delete the extcondition value */
	/*
	 *
	 * 修改或删除 extcondition 值
	 */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		elog(ERROR, "extconfig and extcondition arrays do not match");
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");
	}

	if (arrayLength <= 1)
	{
		/* removing only element, just set array to null */
		/*
		 *
		 * 只剩这一个元素，直接把数组设为 null
		 */
		repl_null[Anum_pg_extension_extcondition - 1] = true;
	}
	else
	{
		/* squeeze out the target element */
		/*
		 *
		 * 把目标元素挤出去
		 */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* We already checked there are no nulls */
		/*
		 *
		 * 我们已经检查过没有空值
		 */
		deconstruct_array_builtin(a, TEXTOID, &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array_builtin(dvalues, arrayLength - 1, TEXTOID);

		repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);
}

/*
 * Execute ALTER EXTENSION SET SCHEMA
 *
 * 执行 ALTER EXTENSION SET SCHEMA
 */
ObjectAddress
AlterExtensionNamespace(const char *extensionName, const char *newschema, Oid *oldschema)
{
	Oid			extensionOid;
	Oid			nspOid;
	Oid			oldNspOid;
	AclResult	aclresult;
	Relation	extRel;
	ScanKeyData key[2];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Form_pg_extension extForm;
	Relation	depRel;
	SysScanDesc depScan;
	HeapTuple	depTup;
	ObjectAddresses *objsMoved;
	ObjectAddress extAddr;

	extensionOid = get_extension_oid(extensionName, false);

	nspOid = LookupCreationNamespace(newschema);

	/*
	 * Permission check: must own extension.  Note that we don't bother to
	 * check ownership of the individual member objects ...
	 *
	 * 权限检查：必须拥有该扩展。注意我们不费力检查各个成员对象的所有权……
	 */
	if (!object_ownercheck(ExtensionRelationId, extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   extensionName);

	/* Permission check: must have creation rights in target namespace */
	/*
	 *
	 * 权限检查：必须对目标命名空间有创建权限
	 */
	aclresult = object_aclcheck(NamespaceRelationId, nspOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, newschema);

	/*
	 * If the schema is currently a member of the extension, disallow moving
	 * the extension into the schema.  That would create a dependency loop.
	 *
	 * 若该模式当前是扩展的成员，则不允许把扩展移入该模式。那会形成依赖环。
	 */
	if (getExtensionOfObject(NamespaceRelationId, nspOid) == extensionOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot move extension \"%s\" into schema \"%s\" "
						"because the extension contains the schema",
						extensionName, newschema)));

	/* Locate the pg_extension tuple */
	/*
	 *
	 * 定位 pg_extension 元组
	 */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* should not happen */
	/*
	 *
	 * 不该发生
	 */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionOid);

	/* Copy tuple so we can modify it below */
	/*
	 *
	 * 复制元组，以便下面可以修改它
	 */
	extTup = heap_copytuple(extTup);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	systable_endscan(extScan);

	/*
	 * If the extension is already in the target schema, just silently do
	 * nothing.
	 *
	 * 若扩展已经在目标模式中，则静默地什么也不做。
	 */
	if (extForm->extnamespace == nspOid)
	{
		table_close(extRel, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/* Check extension is supposed to be relocatable */
	/*
	 *
	 * 检查扩展应当是可重定位的
	 */
	if (!extForm->extrelocatable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" does not support SET SCHEMA",
						NameStr(extForm->extname))));

	objsMoved = new_object_addresses();

	/* store the OID of the namespace to-be-changed */
	/*
	 *
	 * 保存即将被改变的命名空间 OID
	 */
	oldNspOid = extForm->extnamespace;

	/*
	 * Scan pg_depend to find objects that depend directly on the extension,
	 * and alter each one's schema.
	 *
	 * 扫描 pg_depend，找出直接依赖于该扩展的对象，并改变每个对象的模式。
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress dep;
		Oid			dep_oldNspOid;

		/*
		 * If a dependent extension has a no_relocate request for this
		 * extension, disallow SET SCHEMA.  (XXX it's a bit ugly to do this in
		 * the same loop that's actually executing the renames: we may detect
		 * the error condition only after having expended a fair amount of
		 * work.  However, the alternative is to do two scans of pg_depend,
		 * which seems like optimizing for failure cases.  The rename work
		 * will all roll back cleanly enough if we do fail here.)
		 *
		 * 若某个依赖扩展对本扩展有 no_relocate 请求，
		 * 则不允许 SET SCHEMA。
		 * （XXX 在实际执行重命名的同一循环里做这件事有点难看：可能在花了不少工作之后才发现
		 * 错误。
		 * 替代做法是扫描 pg_depend 两次，那像是为失败情形做优化。若这里失败，
		 * 重命名工作都会干净回滚。）
		 */
		if (pg_depend->deptype == DEPENDENCY_NORMAL &&
			pg_depend->classid == ExtensionRelationId)
		{
			char	   *depextname = get_extension_name(pg_depend->objid);
			ExtensionControlFile *dcontrol;
			ListCell   *lc;

			dcontrol = read_extension_control_file(depextname);
			foreach(lc, dcontrol->no_relocate)
			{
				char	   *nrextname = (char *) lfirst(lc);

				if (strcmp(nrextname, NameStr(extForm->extname)) == 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot SET SCHEMA of extension \"%s\" because other extensions prevent it",
									NameStr(extForm->extname)),
							 errdetail("Extension \"%s\" requests no relocation of extension \"%s\".",
									   depextname,
									   NameStr(extForm->extname))));
				}
			}
		}

		/*
		 * Otherwise, ignore non-membership dependencies.  (Currently, the
		 * only other case we could see here is a normal dependency from
		 * another extension.)
		 *
		 * 否则忽略非成员依赖。（目前这里能看到的另一种情况是来自另一个扩展的普通依赖。）
		 */
		if (pg_depend->deptype != DEPENDENCY_EXTENSION)
			continue;

		dep.classId = pg_depend->classid;
		dep.objectId = pg_depend->objid;
		dep.objectSubId = pg_depend->objsubid;

		if (dep.objectSubId != 0)	/* should not happen */
		/*
		 *
		 * 不该发生
		 */
			elog(ERROR, "extension should not have a sub-object dependency");

		/* Relocate the object */
		/*
		 *
		 * 重定位该对象
		 */
		dep_oldNspOid = AlterObjectNamespace_oid(dep.classId,
												 dep.objectId,
												 nspOid,
												 objsMoved);

		/*
		 * If not all the objects had the same old namespace (ignoring any
		 * that are not in namespaces or are dependent types), complain.
		 *
		 * 若并非所有对象都有相同的旧命名空间（忽略不在命名空间中的或依赖类型），则报错。
		 */
		if (dep_oldNspOid != InvalidOid && dep_oldNspOid != oldNspOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" does not support SET SCHEMA",
							NameStr(extForm->extname)),
					 errdetail("%s is not in the extension's schema \"%s\"",
							   getObjectDescription(&dep, false),
							   get_namespace_name(oldNspOid))));
	}

	/* report old schema, if caller wants it */
	/*
	 *
	 * 若调用者需要，则报告旧模式
	 */
	if (oldschema)
		*oldschema = oldNspOid;

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	/* Now adjust pg_extension.extnamespace */
	/*
	 *
	 * 现在调整 pg_extension.extnamespace
	 */
	extForm->extnamespace = nspOid;

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	table_close(extRel, RowExclusiveLock);

	/* update dependency to point to the new schema */
	/*
	 *
	 * 更新依赖，使其指向新模式
	 */
	if (changeDependencyFor(ExtensionRelationId, extensionOid,
							NamespaceRelationId, oldNspOid, nspOid) != 1)
		elog(ERROR, "could not change schema dependency for extension %s",
			 NameStr(extForm->extname));

	InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

	ObjectAddressSet(extAddr, ExtensionRelationId, extensionOid);

	return extAddr;
}

/*
 * Execute ALTER EXTENSION UPDATE
 *
 * 执行 ALTER EXTENSION UPDATE
 */
ObjectAddress
ExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt)
{
	DefElem    *d_new_version = NULL;
	char	   *versionName;
	char	   *oldVersionName;
	ExtensionControlFile *control;
	Oid			extensionOid;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	List	   *updateVersions;
	Datum		datum;
	bool		isnull;
	ListCell   *lc;
	ObjectAddress address;

	/*
	 * We use global variables to track the extension being created, so we can
	 * create/update only one extension at the same time.
	 *
	 * 我们用全局变量跟踪正在创建的扩展，因此同一时间只能创建一个或更新一个扩展。
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested ALTER EXTENSION is not supported")));

	/*
	 * Look up the extension --- it must already exist in pg_extension
	 *
	 * 查找该扩展——它必须已经存在于 pg_extension 中
	 */
	extRel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->extname));

	extScan = systable_beginscan(extRel, ExtensionNameIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						stmt->extname)));

	extensionOid = ((Form_pg_extension) GETSTRUCT(extTup))->oid;

	/*
	 * Determine the existing version we are updating from
	 *
	 * 确定我们正在从其更新的现有版本
	 */
	datum = heap_getattr(extTup, Anum_pg_extension_extversion,
						 RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extversion is null");
	oldVersionName = text_to_cstring(DatumGetTextPP(datum));

	systable_endscan(extScan);

	table_close(extRel, AccessShareLock);

	/* Permission check: must own extension */
	/*
	 *
	 * 权限检查：必须拥有该扩展
	 */
	if (!object_ownercheck(ExtensionRelationId, extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * Read the primary control file.  Note we assume that it does not contain
	 * any non-ASCII data, so there is no need to worry about encoding at this
	 * point.
	 *
	 * 读取主控制文件。注意假定它不含任何非 ASCII 数据，因此此时不必担心编码。
	 */
	control = read_extension_control_file(stmt->extname);

	/*
	 * Read the statement option list
	 *
	 * 读取语句的选项列表
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				errorConflictingDefElem(defel, pstate);
			d_new_version = defel;
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/*
	 * Determine the version to update to
	 *
	 * 确定要更新到的版本
	 */
	if (d_new_version && d_new_version->arg)
		versionName = strVal(d_new_version->arg);
	else if (control->default_version)
		versionName = control->default_version;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("version to install must be specified")));
		versionName = NULL;		/* keep compiler quiet */
		/*
		 *
		 * 避免编译器告警
		 */
	}
	check_valid_version_name(versionName);

	/*
	 * If we're already at that version, just say so
	 *
	 * 若已经是该版本，只说明这一点
	 */
	if (strcmp(oldVersionName, versionName) == 0)
	{
		ereport(NOTICE,
				(errmsg("version \"%s\" of extension \"%s\" is already installed",
						versionName, stmt->extname)));
		return InvalidObjectAddress;
	}

	/*
	 * Identify the series of update script files we need to execute
	 *
	 * 确定需要执行的一系列更新脚本文件
	 */
	updateVersions = identify_update_path(control,
										  oldVersionName,
										  versionName);

	/*
	 * Update the pg_extension row and execute the update scripts, one at a
	 * time
	 *
	 * 更新 pg_extension 行，并逐个执行更新脚本
	 */
	ApplyExtensionUpdates(extensionOid, control,
						  oldVersionName, updateVersions,
						  NULL, false, false);

	ObjectAddressSet(address, ExtensionRelationId, extensionOid);

	return address;
}

/*
 * Apply a series of update scripts as though individual ALTER EXTENSION
 * UPDATE commands had been given, including altering the pg_extension row
 * and dependencies each time.
 *
 * 像给出单独的 ALTER EXTENSION UPDATE
 * 命令那样应用一系列更新脚本，
 * 每次都修改 pg_extension 行和依赖。
 *
 * This might be more work than necessary, but it ensures that old update
 * scripts don't break if newer versions have different control parameters.
 *
 * 这可能比必要的工作更多，但它保证较新版本的控制参数不同时，旧的更新脚本不会坏掉。
 */
static void
ApplyExtensionUpdates(Oid extensionOid,
					  ExtensionControlFile *pcontrol,
					  const char *initialVersion,
					  List *updateVersions,
					  char *origSchemaName,
					  bool cascade,
					  bool is_create)
{
	const char *oldVersionName = initialVersion;
	ListCell   *lcv;

	foreach(lcv, updateVersions)
	{
		char	   *versionName = (char *) lfirst(lcv);
		ExtensionControlFile *control;
		char	   *schemaName;
		Oid			schemaOid;
		List	   *requiredExtensions;
		List	   *requiredSchemas;
		Relation	extRel;
		ScanKeyData key[1];
		SysScanDesc extScan;
		HeapTuple	extTup;
		Form_pg_extension extForm;
		Datum		values[Natts_pg_extension];
		bool		nulls[Natts_pg_extension];
		bool		repl[Natts_pg_extension];
		ObjectAddress myself;
		ListCell   *lc;

		/*
		 * Fetch parameters for specific version (pcontrol is not changed)
		 *
		 * 取特定版本的参数（pcontrol 不会被改变）
		 */
		control = read_extension_aux_control_file(pcontrol, versionName);

		/* Find the pg_extension tuple */
		/*
		 *
		 * 找到 pg_extension 元组
		 */
		extRel = table_open(ExtensionRelationId, RowExclusiveLock);

		ScanKeyInit(&key[0],
					Anum_pg_extension_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(extensionOid));

		extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
									 NULL, 1, key);

		extTup = systable_getnext(extScan);

		if (!HeapTupleIsValid(extTup))	/* should not happen */
		/*
		 *
		 * 不该发生
		 */
			elog(ERROR, "could not find tuple for extension %u",
				 extensionOid);

		extForm = (Form_pg_extension) GETSTRUCT(extTup);

		/*
		 * Determine the target schema (set by original install)
		 *
		 * 确定目标模式（由最初的安装设置）
		 */
		schemaOid = extForm->extnamespace;
		schemaName = get_namespace_name(schemaOid);

		/*
		 * Modify extrelocatable and extversion in the pg_extension tuple
		 *
		 * 修改 pg_extension 元组中的 extrelocatable 与
		 * extversion
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		memset(repl, 0, sizeof(repl));

		values[Anum_pg_extension_extrelocatable - 1] =
			BoolGetDatum(control->relocatable);
		repl[Anum_pg_extension_extrelocatable - 1] = true;
		values[Anum_pg_extension_extversion - 1] =
			CStringGetTextDatum(versionName);
		repl[Anum_pg_extension_extversion - 1] = true;

		extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
								   values, nulls, repl);

		CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

		systable_endscan(extScan);

		table_close(extRel, RowExclusiveLock);

		/*
		 * Look up the prerequisite extensions for this version, install them
		 * if necessary, and build lists of their OIDs and the OIDs of their
		 * target schemas.
		 *
		 * 查找该版本的前置扩展，必要时安装它们，并建立它们的 OID 列表及其目标模式的
		 * OID 列表。
		 */
		requiredExtensions = NIL;
		requiredSchemas = NIL;
		foreach(lc, control->requires)
		{
			char	   *curreq = (char *) lfirst(lc);
			Oid			reqext;
			Oid			reqschema;

			reqext = get_required_extension(curreq,
											control->name,
											origSchemaName,
											cascade,
											NIL,
											is_create);
			reqschema = get_extension_schema(reqext);
			requiredExtensions = lappend_oid(requiredExtensions, reqext);
			requiredSchemas = lappend_oid(requiredSchemas, reqschema);
		}

		/*
		 * Remove and recreate dependencies on prerequisite extensions
		 *
		 * 删除并重建对前置扩展的依赖
		 */
		deleteDependencyRecordsForClass(ExtensionRelationId, extensionOid,
										ExtensionRelationId,
										DEPENDENCY_NORMAL);

		myself.classId = ExtensionRelationId;
		myself.objectId = extensionOid;
		myself.objectSubId = 0;

		foreach(lc, requiredExtensions)
		{
			Oid			reqext = lfirst_oid(lc);
			ObjectAddress otherext;

			otherext.classId = ExtensionRelationId;
			otherext.objectId = reqext;
			otherext.objectSubId = 0;

			recordDependencyOn(&myself, &otherext, DEPENDENCY_NORMAL);
		}

		InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

		/*
		 * Finally, execute the update script file
		 *
		 * 最后执行更新脚本文件
		 */
		execute_extension_script(extensionOid, control,
								 oldVersionName, versionName,
								 requiredSchemas,
								 schemaName);

		/*
		 * Update prior-version name and loop around.  Since
		 * execute_sql_string did a final CommandCounterIncrement, we can
		 * update the pg_extension row again.
		 *
		 * 更新先前版本名并循环。由于 execute_sql_string 做了最后一次
		 * CommandCounterIncrement，
		 * 我们可以再次更新 pg_extension 行。
		 */
		oldVersionName = versionName;
	}
}

/*
 * Execute ALTER EXTENSION ADD/DROP
 *
 * 执行 ALTER EXTENSION ADD/DROP
 *
 * Return value is the address of the altered extension.
 *
 * 返回值是被修改扩展的地址。
 *
 * objAddr is an output argument which, if not NULL, is set to the address of
 * the added/dropped object.
 *
 * objAddr 是输出参数；若不为 NULL，则设为被加入或删除的对象的地址。
 */
ObjectAddress
ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
							   ObjectAddress *objAddr)
{
	ObjectAddress extension;
	ObjectAddress object;
	Relation	relation;

	switch (stmt->objtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_EXTENSION:
		case OBJECT_INDEX:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLESPACE:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot add an object of this type to an extension")));
			break;
		default:
			/* OK */
			/*
			 *
			 * 可以
			 */
			break;
	}

	/*
	 * Find the extension and acquire a lock on it, to ensure it doesn't get
	 * dropped concurrently.  A sharable lock seems sufficient: there's no
	 * reason not to allow other sorts of manipulations, such as add/drop of
	 * other objects, to occur concurrently.  Concurrently adding/dropping the
	 * *same* object would be bad, but we prevent that by using a non-sharable
	 * lock on the individual object, below.
	 *
	 * 找到该扩展并对其加锁，以确保它不会被并发删除。可共享锁似乎足够：
	 * 没有理由不允许同时进行其他操作，例如加入或删除其他对象。
	 * 并发加入或删除同一个对象是不好的，但下面我们通过对单个对象使用不可共享锁来防止这一点。
	 */
	extension = get_object_address(OBJECT_EXTENSION,
								   (Node *) makeString(stmt->extname),
								   &relation, AccessShareLock, false);

	/* Permission check: must own extension */
	/*
	 *
	 * 权限检查：必须拥有该扩展
	 */
	if (!object_ownercheck(ExtensionRelationId, extension.objectId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * Translate the parser representation that identifies the object into an
	 * ObjectAddress.  get_object_address() will throw an error if the object
	 * does not exist, and will also acquire a lock on the object to guard
	 * against concurrent DROP and ALTER EXTENSION ADD/DROP operations.
	 *
	 * 把标识对象的解析器表示转换成 ObjectAddress。若对象不存在，
	 * get_object_address() 会抛出错误，
	 * 并会对对象加锁，以防并发的 DROP 与 ALTER EXTENSION
	 * ADD/DROP。
	 */
	object = get_object_address(stmt->objtype, stmt->object,
								&relation, ShareUpdateExclusiveLock, false);

	Assert(object.objectSubId == 0);
	if (objAddr)
		*objAddr = object;

	/* Permission check: must own target object, too */
	/*
	 *
	 * 权限检查：也必须拥有目标对象
	 */
	check_object_ownership(GetUserId(), stmt->objtype, object,
						   stmt->object, relation);

	/* Do the update, recursing to any dependent objects */
	/*
	 *
	 * 执行更新，并递归到任何依赖对象
	 */
	ExecAlterExtensionContentsRecurse(stmt, extension, object);

	/* Finish up */
	/*
	 *
	 * 收尾
	 */
	InvokeObjectPostAlterHook(ExtensionRelationId, extension.objectId, 0);

	/*
	 * If get_object_address() opened the relation for us, we close it to keep
	 * the reference count correct - but we retain any locks acquired by
	 * get_object_address() until commit time, to guard against concurrent
	 * activity.
	 *
	 * 若 get_object_address() 为我们打开了关系，
	 * 则关闭它以保持引用计数正确，
	 * 但保留 get_object_address() 取得的任何锁直到提交，
	 * 以防并发活动。
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return extension;
}

/*
 * ExecAlterExtensionContentsRecurse
 *		Subroutine for ExecAlterExtensionContentsStmt
 *
 * ExecAlterExtensionContentsRecurse：ExecAlte
 * rExtensionContentsStmt 的子程序
 *
 * Do the bare alteration of object's membership in extension,
 * without permission checks.  Recurse to dependent objects, if any.
 *
 * 只做对象在扩展中成员关系的裸修改，不做权限检查。若有依赖对象则递归到它们。
 */
static void
ExecAlterExtensionContentsRecurse(AlterExtensionContentsStmt *stmt,
								  ObjectAddress extension,
								  ObjectAddress object)
{
	Oid			oldExtension;

	/*
	 * Check existing extension membership.
	 *
	 * 检查现有的扩展成员关系。
	 */
	oldExtension = getExtensionOfObject(object.classId, object.objectId);

	if (stmt->action > 0)
	{
		/*
		 * ADD, so complain if object is already attached to some extension.
		 *
		 * 这是 ADD，因此若对象已经附属于某个扩展则报错。
		 */
		if (OidIsValid(oldExtension))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is already a member of extension \"%s\"",
							getObjectDescription(&object, false),
							get_extension_name(oldExtension))));

		/*
		 * Prevent a schema from being added to an extension if the schema
		 * contains the extension.  That would create a dependency loop.
		 *
		 * 若模式包含该扩展，则阻止把该模式加入扩展。那会形成依赖环。
		 */
		if (object.classId == NamespaceRelationId &&
			object.objectId == get_extension_schema(extension.objectId))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot add schema \"%s\" to extension \"%s\" "
							"because the schema contains the extension",
							get_namespace_name(object.objectId),
							stmt->extname)));

		/*
		 * OK, add the dependency.
		 *
		 * 可以，添加该依赖。
		 */
		recordDependencyOn(&object, &extension, DEPENDENCY_EXTENSION);

		/*
		 * Also record the initial ACL on the object, if any.
		 *
		 * 若对象有初始 ACL，也记录下来。
		 *
		 * Note that this will handle the object's ACLs, as well as any ACLs
		 * on object subIds.  (In other words, when the object is a table,
		 * this will record the table's ACL and the ACLs for the columns on
		 * the table, if any).
		 *
		 * 注意这会处理对象的 ACL，以及对象 subId 上的任何 ACL。
		 * （换句话说，当对象是表时，这会记录表的 ACL 以及表上各列的 ACL，若有。）
		 */
		recordExtObjInitPriv(object.objectId, object.classId);
	}
	else
	{
		/*
		 * DROP, so complain if it's not a member.
		 *
		 * 这是 DROP，因此若它不是成员则报错。
		 */
		if (oldExtension != extension.objectId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is not a member of extension \"%s\"",
							getObjectDescription(&object, false),
							stmt->extname)));

		/*
		 * OK, drop the dependency.
		 *
		 * 可以，删除该依赖。
		 */
		if (deleteDependencyRecordsForClass(object.classId, object.objectId,
											ExtensionRelationId,
											DEPENDENCY_EXTENSION) != 1)
			elog(ERROR, "unexpected number of extension dependency records");

		/*
		 * If it's a relation, it might have an entry in the extension's
		 * extconfig array, which we must remove.
		 *
		 * 若它是关系，扩展的 extconfig 数组中可能有一项，必须移除。
		 */
		if (object.classId == RelationRelationId)
			extension_config_remove(extension.objectId, object.objectId);

		/*
		 * Remove all the initial ACLs, if any.
		 *
		 * 移除全部初始 ACL，若有。
		 *
		 * Note that this will remove the object's ACLs, as well as any ACLs
		 * on object subIds.  (In other words, when the object is a table,
		 * this will remove the table's ACL and the ACLs for the columns on
		 * the table, if any).
		 *
		 * 注意这会移除对象的 ACL，以及对象 subId 上的任何 ACL。
		 * （换句话说，当对象是表时，这会移除表的 ACL 以及表上各列的 ACL，若有。）
		 */
		removeExtObjInitPriv(object.objectId, object.classId);
	}

	/*
	 * Recurse to any dependent objects; currently, this includes the array
	 * type of a base type, the multirange type associated with a range type,
	 * and the rowtype of a table.
	 *
	 * 递归到任何依赖对象；目前这包括基类型的数组类型、与范围类型关联的多重范围类型，
	 * 以及表的行类型。
	 */
	if (object.classId == TypeRelationId)
	{
		ObjectAddress depobject;

		depobject.classId = TypeRelationId;
		depobject.objectSubId = 0;

		/* If it has an array type, update that too */
		/*
		 *
		 * 若它有数组类型，也更新那个
		 */
		depobject.objectId = get_array_type(object.objectId);
		if (OidIsValid(depobject.objectId))
			ExecAlterExtensionContentsRecurse(stmt, extension, depobject);

		/* If it is a range type, update the associated multirange too */
		/*
		 *
		 * 若它是范围类型，也更新关联的多重范围类型
		 */
		if (type_is_range(object.objectId))
		{
			depobject.objectId = get_range_multirange(object.objectId);
			if (!OidIsValid(depobject.objectId))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("could not find multirange type for data type %s",
								format_type_be(object.objectId))));
			ExecAlterExtensionContentsRecurse(stmt, extension, depobject);
		}
	}
	if (object.classId == RelationRelationId)
	{
		ObjectAddress depobject;

		depobject.classId = TypeRelationId;
		depobject.objectSubId = 0;

		/* It might not have a rowtype, but if it does, update that */
		/*
		 *
		 * 它可能没有行类型，但若有，则更新那个
		 */
		depobject.objectId = get_rel_type_id(object.objectId);
		if (OidIsValid(depobject.objectId))
			ExecAlterExtensionContentsRecurse(stmt, extension, depobject);
	}
}

/*
 * Read the whole of file into memory.
 *
 * 把整个文件读入内存。
 *
 * The file contents are returned as a single palloc'd chunk. For convenience
 * of the callers, an extra \0 byte is added to the end.  That is not counted
 * in the length returned into *length.
 *
 * 文件内容作为单独一块 palloc 内存返回。为方便调用者，
 * 末尾额外加一个 \0 字节。
 * 该字节不计入返回到 *length 的长度。
 */
static char *
read_whole_file(const char *filename, int *length)
{
	char	   *buf;
	FILE	   *file;
	size_t		bytes_to_read;
	struct stat fst;

	if (stat(filename, &fst) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));

	if (fst.st_size > (MaxAllocSize - 1))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file \"%s\" is too large", filename)));
	bytes_to_read = (size_t) fst.st_size;

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						filename)));

	buf = (char *) palloc(bytes_to_read + 1);

	bytes_to_read = fread(buf, 1, bytes_to_read, file);

	if (ferror(file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));

	FreeFile(file);

	buf[bytes_to_read] = '\0';

	/*
	 * On Windows, manually convert Windows-style newlines (\r\n) to the Unix
	 * convention of \n only.  This avoids gotchas due to script files
	 * possibly getting converted when being transferred between platforms.
	 * Ideally we'd do this by using text mode to read the file, but that also
	 * causes control-Z to be treated as end-of-file.  Historically we've
	 * allowed control-Z in script files, so breaking that seems unwise.
	 *
	 * 在 Windows 上，手动把 Windows 风格的换行（\r\n）转换成
	 * Unix 的仅 \n。
	 * 这避免脚本文件在平台之间传输时可能被转换而带来的陷阱。
	 * 理想情况下用文本模式读文件就能做到，
	 * 但那也会把 control-Z 当成文件结束。
	 * 历史上我们允许脚本文件中有 control-Z，打破这一点似乎不明智。
	 */
#ifdef WIN32
	{
		char	   *s,
				   *d;

		for (s = d = buf; *s; s++)
		{
			if (!(*s == '\r' && s[1] == '\n'))
				*d++ = *s;
		}
		*d = '\0';
		bytes_to_read = d - buf;
	}
#endif

	*length = bytes_to_read;
	return buf;
}

/*
 *
 * 分配 ExtensionControlFile，并把指针字段初始化为空。
 */

static ExtensionControlFile *
new_ExtensionControlFile(const char *extname)
{
	/*
	 * Set up default values.  Pointer fields are initially null.
	 *
	 * 设置默认值。指针字段最初为 null。
	 */
	ExtensionControlFile *control = palloc0_object(ExtensionControlFile);

	control->name = pstrdup(extname);
	control->relocatable = false;
	control->superuser = true;
	control->trusted = false;
	control->encoding = -1;

	return control;
}

/*
 * Work in a very similar way with find_in_path but it receives an already
 * parsed List of paths to search the basename and it do not support macro
 * replacement or custom error messages (for simplicity).
 *
 * 工作方式与 find_in_path 非常相似，
 * 但它接收已经解析好的路径 List 来搜索基名，
 * 并且为了简单，不支持宏替换或自定义错误信息。
 *
 * By "already parsed List of paths" this function expected that paths already
 * have all macros replaced.
 *
 * 所谓“已经解析好的路径 List”，是指这些路径中的全部宏都已被替换。
 */
char *
find_in_paths(const char *basename, List *paths)
{
	ListCell   *cell;

	foreach(cell, paths)
	{
		char	   *path = lfirst(cell);
		char	   *full;

		Assert(path != NULL);

		path = pstrdup(path);
		canonicalize_path(path);

		/* only absolute paths */
		/*
		 *
		 * 仅绝对路径
		 */
		if (!is_absolute_path(path))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_NAME),
					errmsg("component in parameter \"%s\" is not an absolute path", "extension_control_path"));

		full = psprintf("%s/%s", path, basename);

		if (pg_file_exists(full))
			return full;

		pfree(path);
		pfree(full);
	}

	return NULL;
}
