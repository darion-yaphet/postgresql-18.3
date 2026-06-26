/*-------------------------------------------------------------------------
 *
 * pg_amcheck.c
 *		Detects corruption within database relations.
 *		检测数据库关系文件中的损坏。
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_amcheck/pg_amcheck.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * pg_amcheck 核心流程与主要函数说明（中文说明）：
 *
 * 1. 核心流程：
 *    - 这是一个命令行工具，在客户端运行，通过建立与数据库的并行连接来验证各关系（表和索引）的逻辑一致性。
 *    - 解析命令行选项（如 -t/--table, -i/--index, -d/--database 选项）以建立需要验证和排除的关系白名单/黑名单。
 *    - 建立与主维护数据库的连接，获取满足条件的目标数据库列表。
 *    - 对每个待检查数据库执行查询：
 *      a) 验证是否安装了 contrib/amcheck 插件，并获取其对应的模式（schema）空间位置与版本。
 *      b) 执行大型 SQL 查询，根据用户提供的正则匹配过滤规则（如 include/exclude schema/table/index 等），
 *         生成具体的待验证关系列表（并按大小降序排序以优化工作调度）。
 *    - 建立并行工作通道槽（ParallelSlotsSetup），使用服务器端单连接独立后端异步执行 amcheck 命令：
 *      - 对于堆表（Heap）：调用 verify_heapam() 函数，验证页面格式、结构，以及表与其 TOAST 关联表的一致性。
 *      - 对于 B 树索引（B-tree）：调用 bt_index_check() 或 bt_index_parent_check() 函数。
 *    - 实时收集行输出和服务器 ERROR，并将详细的逻辑页损坏信息归档报告给用户。
 *
 * 2. 核心函数：
 *    - main(): 主入口点，解析参数并控制整个跨数据库、跨关系的 amcheck 调度和主事件循环。
 *    - compile_database_list(): 根据提供的过滤模式查询满足条件的 checkable 数据库。
 *    - compile_relation_list_one_db(): 核心元数据查询函数，构建当前数据库中的所有待检查关系项列表。
 *    - prepare_heap_command() / prepare_btree_command(): 构建传递给数据库后端执行 amcheck 校验所需的 SQL 命令字符串。
 *    - verify_heap_slot_handler() / verify_btree_slot_handler(): 异步并行槽回调处理函数，用于解析并输出检验得到的页损坏信息。
 */
#include "postgres_fe.h"

#include <limits.h>
#include <time.h>

#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_namespace_d.h"
#include "common/logging.h"
#include "common/username.h"
#include "fe_utils/cancel.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/parallel_slot.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/simple_list.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "pgtime.h"
#include "storage/block.h"

typedef struct PatternInfo
{
	const char *pattern;		/* Unaltered pattern from the command line */
								/* 命令行中未更改的原始模式 */
	char	   *db_regex;		/* Database regexp parsed from pattern, or
								 * NULL */
								/* 从模式中解析出的数据库正则表达式，或
								 * NULL */
	char	   *nsp_regex;		/* Schema regexp parsed from pattern, or NULL */
								/* 从模式中解析出的命名空间（模式）正则表达式，或 NULL */
	char	   *rel_regex;		/* Relation regexp parsed from pattern, or
								 * NULL */
								/* 从模式中解析出的关系正则表达式，或
								 * NULL */
	bool		heap_only;		/* true if rel_regex should only match heap
								 * tables */
								/* 如果 rel_regex 应该仅匹配堆表，则为 true */
	bool		btree_only;		/* true if rel_regex should only match btree
								 * indexes */
								/* 如果 rel_regex 应该仅匹配 B 树索引，则为 true */
	bool		matched;		/* true if the pattern matched in any database */
								/* 如果该模式在任何数据库中匹配成功，则为 true */
} PatternInfo;

typedef struct PatternInfoArray
{
	PatternInfo *data;
	size_t		len;
} PatternInfoArray;

/* pg_amcheck command line options controlled by user flags */
/* 由用户标志控制的 pg_amcheck 命令行选项 */
typedef struct AmcheckOptions
{
	bool		dbpattern;
	bool		alldb;
	bool		echo;
	bool		verbose;
	bool		strict_names;
	bool		show_progress;
	int			jobs;

	/*
	 * Whether to install missing extensions, and optionally the name of the
	 * schema in which to install the extension's objects.
	 */
	/*
	 * 是否安装缺失的扩展，以及（可选的）要安装该扩展对象的 schema（模式）名称。
	 */
	bool		install_missing;
	char	   *install_schema;

	/* Objects to check or not to check, as lists of PatternInfo structs. */
	/* 要检查或不检查的对象，作为 PatternInfo 结构体的列表。 */
	PatternInfoArray include;
	PatternInfoArray exclude;

	/*
	 * As an optimization, if any pattern in the exclude list applies to heap
	 * tables, or similarly if any such pattern applies to btree indexes, or
	 * to schemas, then these will be true, otherwise false.  These should
	 * always agree with what you'd conclude by grep'ing through the exclude
	 * list.
	 */
	/*
	 * 作为一种优化，如果排除列表中的任何模式适用于堆表，或者类似地适用于 B 树索引或
	 * 命名空间，则这些变量将为 true，否则为 false。这些变量应始终与您在
	 * 排除列表中搜索得出的结论一致。
	 */
	bool		excludetbl;
	bool		excludeidx;
	bool		excludensp;

	/*
	 * If any inclusion pattern exists, then we should only be checking
	 * matching relations rather than all relations, so this is true iff
	 * include is empty.
	 */
	/*
	 * 如果存在任何包含模式，则我们应该只检查匹配的关系，而不是所有关系，
	 * 因此当且仅当 include 为空时，此变量为 true。
	 */
	bool		allrel;

	/* heap table checking options */
	/* 堆表检查选项 */
	bool		no_toast_expansion;
	bool		reconcile_toast;
	bool		on_error_stop;
	int64		startblock;
	int64		endblock;
	const char *skip;

	/* btree index checking options */
	/* B 树索引检查选项 */
	bool		parent_check;
	bool		rootdescend;
	bool		heapallindexed;
	bool		checkunique;

	/* heap and btree hybrid option */
	/* 堆和 B 树的混合选项 */
	bool		no_btree_expansion;
} AmcheckOptions;

static AmcheckOptions opts = {
	.dbpattern = false,
	.alldb = false,
	.echo = false,
	.verbose = false,
	.strict_names = true,
	.show_progress = false,
	.jobs = 1,
	.install_missing = false,
	.install_schema = "pg_catalog",
	.include = {NULL, 0},
	.exclude = {NULL, 0},
	.excludetbl = false,
	.excludeidx = false,
	.excludensp = false,
	.allrel = true,
	.no_toast_expansion = false,
	.reconcile_toast = true,
	.on_error_stop = false,
	.startblock = -1,
	.endblock = -1,
	.skip = "none",
	.parent_check = false,
	.rootdescend = false,
	.heapallindexed = false,
	.checkunique = false,
	.no_btree_expansion = false
};

static const char *progname = NULL;

/* Whether all relations have so far passed their corruption checks */
/* 到目前为止，所有关系是否都通过了它们的损坏检查 */
static bool all_checks_pass = true;

/* Time last progress report was displayed */
/* 上一次显示进度报告的时间 */
static pg_time_t last_progress_report = 0;
static bool progress_since_last_stderr = false;

typedef struct DatabaseInfo
{
	char	   *datname;
	char	   *amcheck_schema; /* escaped, quoted literal */
	bool		is_checkunique;
} DatabaseInfo;

typedef struct RelationInfo
{
	const DatabaseInfo *datinfo;	/* shared by other relinfos */
									/* 由其他关系信息共享 */
	Oid			reloid;
	bool		is_heap;		/* true if heap, false if btree */
								/* 如果是堆表则为 true，如果是 B 树索引则为 false */
	char	   *nspname;
	char	   *relname;
	int			relpages;
	int			blocks_to_check;
	char	   *sql;			/* set during query run, pg_free'd after */
								/* 在查询运行期间设置，之后被 pg_free 释放 */
} RelationInfo;

/*
 * Query for determining if contrib's amcheck is installed.  If so, selects the
 * namespace name where amcheck's functions can be found.
 */
/*
 * 用于确定是否已安装 contrib 的 amcheck 的查询。如果是，选择可以找到 amcheck 函数的
 * 命名空间名称。
 */
static const char *const amcheck_sql =
"SELECT n.nspname, x.extversion FROM pg_catalog.pg_extension x"
"\nJOIN pg_catalog.pg_namespace n ON x.extnamespace = n.oid"
"\nWHERE x.extname = 'amcheck'";

static void prepare_heap_command(PQExpBuffer sql, RelationInfo *rel,
								 PGconn *conn);
static void prepare_btree_command(PQExpBuffer sql, RelationInfo *rel,
								  PGconn *conn);
static void run_command(ParallelSlot *slot, const char *sql);
static bool verify_heap_slot_handler(PGresult *res, PGconn *conn,
									 void *context);
static bool verify_btree_slot_handler(PGresult *res, PGconn *conn, void *context);
static void help(const char *progname);
static void progress_report(uint64 relations_total, uint64 relations_checked,
							uint64 relpages_total, uint64 relpages_checked,
							const char *datname, bool force, bool finished);

static void append_database_pattern(PatternInfoArray *pia, const char *pattern,
									int encoding);
static void append_schema_pattern(PatternInfoArray *pia, const char *pattern,
								  int encoding);
static void append_relation_pattern(PatternInfoArray *pia, const char *pattern,
									int encoding);
static void append_heap_pattern(PatternInfoArray *pia, const char *pattern,
								int encoding);
static void append_btree_pattern(PatternInfoArray *pia, const char *pattern,
								 int encoding);
static void compile_database_list(PGconn *conn, SimplePtrList *databases,
								  const char *initial_dbname);
static void compile_relation_list_one_db(PGconn *conn, SimplePtrList *relations,
										 const DatabaseInfo *dat,
										 uint64 *pagecount);

#define log_no_match(...) do { \
		if (opts.strict_names) \
			pg_log_error(__VA_ARGS__); \
		else \
			pg_log_warning(__VA_ARGS__); \
	} while(0)

#define FREE_AND_SET_NULL(x) do { \
	pg_free(x); \
	(x) = NULL; \
	} while (0)

int
main(int argc, char *argv[])
{
	PGconn	   *conn = NULL;
	SimplePtrListCell *cell;
	SimplePtrList databases = {NULL, NULL};
	SimplePtrList relations = {NULL, NULL};
	bool		failed = false;
	const char *latest_datname;
	int			parallel_workers;
	ParallelSlotArray *sa;
	PQExpBufferData sql;
	uint64		reltotal = 0;
	uint64		pageschecked = 0;
	uint64		pagestotal = 0;
	uint64		relprogress = 0;
	int			pattern_id;

	static struct option long_options[] = {
		/* Connection options */
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"maintenance-db", required_argument, NULL, 1},

		/* check options */
		{"all", no_argument, NULL, 'a'},
		{"database", required_argument, NULL, 'd'},
		{"exclude-database", required_argument, NULL, 'D'},
		{"echo", no_argument, NULL, 'e'},
		{"index", required_argument, NULL, 'i'},
		{"exclude-index", required_argument, NULL, 'I'},
		{"jobs", required_argument, NULL, 'j'},
		{"progress", no_argument, NULL, 'P'},
		{"relation", required_argument, NULL, 'r'},
		{"exclude-relation", required_argument, NULL, 'R'},
		{"schema", required_argument, NULL, 's'},
		{"exclude-schema", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"exclude-table", required_argument, NULL, 'T'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-dependent-indexes", no_argument, NULL, 2},
		{"no-dependent-toast", no_argument, NULL, 3},
		{"exclude-toast-pointers", no_argument, NULL, 4},
		{"on-error-stop", no_argument, NULL, 5},
		{"skip", required_argument, NULL, 6},
		{"startblock", required_argument, NULL, 7},
		{"endblock", required_argument, NULL, 8},
		{"rootdescend", no_argument, NULL, 9},
		{"no-strict-names", no_argument, NULL, 10},
		{"heapallindexed", no_argument, NULL, 11},
		{"parent-check", no_argument, NULL, 12},
		{"install-missing", optional_argument, NULL, 13},
		{"checkunique", no_argument, NULL, 14},

		{NULL, 0, NULL, 0}
	};

	int			optindex;
	int			c;

	const char *db = NULL;
	const char *maintenance_db = NULL;

	const char *host = NULL;
	const char *port = NULL;
	const char *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	int			encoding = pg_get_encoding_from_locale(NULL, false);
	ConnParams	cparams;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_amcheck"));

	handle_help_version_opts(argc, argv, progname, help);

	/* process command-line options */
	/* 处理命令行选项 */
	while ((c = getopt_long(argc, argv, "ad:D:eh:Hi:I:j:p:Pr:R:s:S:t:T:U:vwW",
							long_options, &optindex)) != -1)
	{
		char	   *endptr;
		unsigned long optval;

		switch (c)
		{
			case 'a':
				opts.alldb = true;
				break;
			case 'd':
				opts.dbpattern = true;
				append_database_pattern(&opts.include, optarg, encoding);
				break;
			case 'D':
				opts.dbpattern = true;
				append_database_pattern(&opts.exclude, optarg, encoding);
				break;
			case 'e':
				opts.echo = true;
				break;
			case 'h':
				host = pg_strdup(optarg);
				break;
			case 'i':
				opts.allrel = false;
				append_btree_pattern(&opts.include, optarg, encoding);
				break;
			case 'I':
				opts.excludeidx = true;
				append_btree_pattern(&opts.exclude, optarg, encoding);
				break;
			case 'j':
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &opts.jobs))
					exit(1);
				break;
			case 'p':
				port = pg_strdup(optarg);
				break;
			case 'P':
				opts.show_progress = true;
				break;
			case 'r':
				opts.allrel = false;
				append_relation_pattern(&opts.include, optarg, encoding);
				break;
			case 'R':
				opts.excludeidx = true;
				opts.excludetbl = true;
				append_relation_pattern(&opts.exclude, optarg, encoding);
				break;
			case 's':
				opts.allrel = false;
				append_schema_pattern(&opts.include, optarg, encoding);
				break;
			case 'S':
				opts.excludensp = true;
				append_schema_pattern(&opts.exclude, optarg, encoding);
				break;
			case 't':
				opts.allrel = false;
				append_heap_pattern(&opts.include, optarg, encoding);
				break;
			case 'T':
				opts.excludetbl = true;
				append_heap_pattern(&opts.exclude, optarg, encoding);
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'v':
				opts.verbose = true;
				pg_logging_increase_verbosity();
				break;
			case 'w':
				prompt_password = TRI_NO;
				break;
			case 'W':
				prompt_password = TRI_YES;
				break;
			case 1:
				maintenance_db = pg_strdup(optarg);
				break;
			case 2:
				opts.no_btree_expansion = true;
				break;
			case 3:
				opts.no_toast_expansion = true;
				break;
			case 4:
				opts.reconcile_toast = false;
				break;
			case 5:
				opts.on_error_stop = true;
				break;
			case 6:
				if (pg_strcasecmp(optarg, "all-visible") == 0)
					opts.skip = "all-visible";
				else if (pg_strcasecmp(optarg, "all-frozen") == 0)
					opts.skip = "all-frozen";
				else if (pg_strcasecmp(optarg, "none") == 0)
					opts.skip = "none";
				else
					pg_fatal("invalid argument for option %s", "--skip");
				break;
			case 7:
				errno = 0;
				optval = strtoul(optarg, &endptr, 10);
				if (endptr == optarg || *endptr != '\0' || errno != 0)
					pg_fatal("invalid start block");
				if (optval > MaxBlockNumber)
					pg_fatal("start block out of bounds");
				opts.startblock = optval;
				break;
			case 8:
				errno = 0;
				optval = strtoul(optarg, &endptr, 10);
				if (endptr == optarg || *endptr != '\0' || errno != 0)
					pg_fatal("invalid end block");
				if (optval > MaxBlockNumber)
					pg_fatal("end block out of bounds");
				opts.endblock = optval;
				break;
			case 9:
				opts.rootdescend = true;
				opts.parent_check = true;
				break;
			case 10:
				opts.strict_names = false;
				break;
			case 11:
				opts.heapallindexed = true;
				break;
			case 12:
				opts.parent_check = true;
				break;
			case 13:
				opts.install_missing = true;
				if (optarg)
					opts.install_schema = pg_strdup(optarg);
				break;
			case 14:
				opts.checkunique = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				/* getopt_long 已经发出了投诉 */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (opts.endblock >= 0 && opts.endblock < opts.startblock)
		pg_fatal("end block precedes start block");

	/*
	 * A single non-option arguments specifies a database name or connection
	 * string.
	 */
	/*
	 * 单个非选项参数指定数据库名称或连接字符串。
	 */
	if (optind < argc)
	{
		db = argv[optind];
		optind++;
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/* fill cparams except for dbname, which is set below */
	/* 填充 cparams，除了在下方设置的 dbname */
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.dbname = NULL;
	cparams.override_dbname = NULL;

	setup_cancel_handler(NULL);

	/* choose the database for our initial connection */
	/* 选择我们初始连接的数据库 */
	if (opts.alldb)
	{
		if (db != NULL)
			pg_fatal("cannot specify a database name with --all");
		cparams.dbname = maintenance_db;
	}
	else if (db != NULL)
	{
		if (opts.dbpattern)
			pg_fatal("cannot specify both a database name and database patterns");
		cparams.dbname = db;
	}

	if (opts.alldb || opts.dbpattern)
	{
		conn = connectMaintenanceDatabase(&cparams, progname, opts.echo);
		compile_database_list(conn, &databases, NULL);
	}
	else
	{
		if (cparams.dbname == NULL)
		{
			if (getenv("PGDATABASE"))
				cparams.dbname = getenv("PGDATABASE");
			else if (getenv("PGUSER"))
				cparams.dbname = getenv("PGUSER");
			else
				cparams.dbname = get_user_name_or_exit(progname);
		}
		conn = connectDatabase(&cparams, progname, opts.echo, false, true);
		compile_database_list(conn, &databases, PQdb(conn));
	}

	if (databases.head == NULL)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		pg_log_warning("no databases to check");
		exit(0);
	}

	/*
	 * Compile a list of all relations spanning all databases to be checked.
	 */
	/*
	 * 编译一个跨越所有要检查数据库的关系列表。
	 */
	for (cell = databases.head; cell; cell = cell->next)
	{
		PGresult   *result;
		int			ntups;
		const char *amcheck_schema = NULL;
		DatabaseInfo *dat = (DatabaseInfo *) cell->ptr;

		cparams.override_dbname = dat->datname;
		if (conn == NULL || strcmp(PQdb(conn), dat->datname) != 0)
		{
			if (conn != NULL)
				disconnectDatabase(conn);
			conn = connectDatabase(&cparams, progname, opts.echo, false, true);
		}

		/*
		 * Optionally install amcheck if not already installed in this
		 * database.
		 */
		/*
		 * 如果此数据库中尚未安装 amcheck，则选择性地安装它。
		 */
		if (opts.install_missing)
		{
			char	   *schema;
			char	   *install_sql;

			/*
			 * Must re-escape the schema name for each database, as the
			 * escaping rules may change.
			 */
			/*
			 * 必须为每个数据库重新转义模式（schema）名称，因为转义规则可能会改变。
			 */
			schema = PQescapeIdentifier(conn, opts.install_schema,
										strlen(opts.install_schema));
			install_sql = psprintf("CREATE EXTENSION IF NOT EXISTS amcheck WITH SCHEMA %s",
								   schema);

			executeCommand(conn, install_sql, opts.echo);
			pfree(install_sql);
			PQfreemem(schema);
		}

		/*
		 * Verify that amcheck is installed for this next database.  User
		 * error could result in a database not having amcheck that should
		 * have it, but we also could be iterating over multiple databases
		 * where not all of them have amcheck installed (for example,
		 * 'template1').
		 */
		/*
		 * 验证此下一个数据库是否已安装 amcheck。用户错误可能导致原本应该安装
		 * amcheck 的数据库实际上没有安装，但我们也可能是在迭代多个数据库，
		 * 其中并非所有数据库都安装了 amcheck（例如 'template1'）。
		 */
		result = executeQuery(conn, amcheck_sql, opts.echo);
		if (PQresultStatus(result) != PGRES_TUPLES_OK)
		{
			/* Querying the catalog failed. */
			/* 查询系统表失败。 */
			pg_log_error("database \"%s\": %s",
						 PQdb(conn), PQerrorMessage(conn));
			pg_log_error_detail("Query was: %s", amcheck_sql);
			PQclear(result);
			disconnectDatabase(conn);
			exit(1);
		}
		ntups = PQntuples(result);
		if (ntups == 0)
		{
			/* Querying the catalog succeeded, but amcheck is missing. */
			/* 查询系统表成功，但缺少 amcheck。 */
			pg_log_warning("skipping database \"%s\": amcheck is not installed",
						   PQdb(conn));
			PQclear(result);
			disconnectDatabase(conn);
			conn = NULL;
			continue;
		}
		amcheck_schema = PQgetvalue(result, 0, 0);
		if (opts.verbose)
			pg_log_info("in database \"%s\": using amcheck version \"%s\" in schema \"%s\"",
						PQdb(conn), PQgetvalue(result, 0, 1), amcheck_schema);
		dat->amcheck_schema = PQescapeIdentifier(conn, amcheck_schema,
												 strlen(amcheck_schema));

		/*
		 * Check the version of amcheck extension. Skip requested unique
		 * constraint check with warning if it is not yet supported by
		 * amcheck.
		 */
		/*
		 * 检查 amcheck 扩展的版本。如果 amcheck 尚不支持请求的唯一性约束检查，
		 * 则跳过并发出警告。
		 */
		if (opts.checkunique == true)
		{
			/*
			 * Now amcheck has only major and minor versions in the string but
			 * we also support revision just in case. Now it is expected to be
			 * zero.
			 */
			/*
			 * 尽管当前 amcheck 的版本字符串中只有主版本号和次版本号，但我们也支持修订号以防万一。
			 * 目前预计它为零。
			 */
			int			vmaj = 0,
						vmin = 0,
						vrev = 0;
			const char *amcheck_version = PQgetvalue(result, 0, 1);

			sscanf(amcheck_version, "%d.%d.%d", &vmaj, &vmin, &vrev);

			/*
			 * checkunique option is supported in amcheck since version 1.4
			 */
			/*
			 * 自 1.4 版本起，amcheck 支持 checkunique 选项
			 */
			if ((vmaj == 1 && vmin < 4) || vmaj == 0)
			{
				pg_log_warning("option %s is not supported by amcheck version %s",
							   "--checkunique", amcheck_version);
				dat->is_checkunique = false;
			}
			else
				dat->is_checkunique = true;
		}

		PQclear(result);

		compile_relation_list_one_db(conn, &relations, dat, &pagestotal);
	}

	/*
	 * Check that all inclusion patterns matched at least one schema or
	 * relation that we can check.
	 */
	/*
	 * 检查所有包含模式是否至少匹配了一个我们可以检查的模式（schema）或关系。
	 */
	for (pattern_id = 0; pattern_id < opts.include.len; pattern_id++)
	{
		PatternInfo *pat = &opts.include.data[pattern_id];

		if (!pat->matched && (pat->nsp_regex != NULL || pat->rel_regex != NULL))
		{
			failed = opts.strict_names;

			if (pat->heap_only)
				log_no_match("no heap tables to check matching \"%s\"",
							 pat->pattern);
			else if (pat->btree_only)
				log_no_match("no btree indexes to check matching \"%s\"",
							 pat->pattern);
			else if (pat->rel_regex == NULL)
				log_no_match("no relations to check in schemas matching \"%s\"",
							 pat->pattern);
			else
				log_no_match("no relations to check matching \"%s\"",
							 pat->pattern);
		}
	}

	if (failed)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		exit(1);
	}

	/*
	 * Set parallel_workers to the lesser of opts.jobs and the number of
	 * relations.
	 */
	/*
	 * 将 parallel_workers 设置为 opts.jobs 和关系数量中较小的一个。
	 */
	parallel_workers = 0;
	for (cell = relations.head; cell; cell = cell->next)
	{
		reltotal++;
		if (parallel_workers < opts.jobs)
			parallel_workers++;
	}

	if (reltotal == 0)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		pg_fatal("no relations to check");
	}
	progress_report(reltotal, relprogress, pagestotal, pageschecked,
					NULL, true, false);

	/*
	 * Main event loop.
	 *
	 * We use server-side parallelism to check up to parallel_workers
	 * relations in parallel.  The list of relations was computed in database
	 * order, which minimizes the number of connects and disconnects as we
	 * process the list.
	 */
	/*
	 * 主事件循环。
	 *
	 * 我们使用服务器端并行机制并行检查多达 parallel_workers 个关系。关系列表是按
	 * 数据库顺序计算的，这在我们处理列表时最小化了连接和断开连接的次数。
	 */
	latest_datname = NULL;
	sa = ParallelSlotsSetup(parallel_workers, &cparams, progname, opts.echo,
							NULL);
	if (conn != NULL)
	{
		ParallelSlotsAdoptConn(sa, conn);
		conn = NULL;
	}

	initPQExpBuffer(&sql);
	for (relprogress = 0, cell = relations.head; cell; cell = cell->next)
	{
		ParallelSlot *free_slot;
		RelationInfo *rel;

		rel = (RelationInfo *) cell->ptr;

		if (CancelRequested)
		{
			failed = true;
			break;
		}

		/*
		 * The list of relations is in database sorted order.  If this next
		 * relation is in a different database than the last one seen, we are
		 * about to start checking this database.  Note that other slots may
		 * still be working on relations from prior databases.
		 */
		/*
		 * 关系列表是按数据库排序的。如果这下一个关系与上一个看到的关系处于不同的数据库，
		 * 我们就要开始检查这个数据库了。请注意，其他插槽（slots）可能仍在处理先前数据库中的关系。
		 */
		latest_datname = rel->datinfo->datname;

		progress_report(reltotal, relprogress, pagestotal, pageschecked,
						latest_datname, false, false);

		relprogress++;
		pageschecked += rel->blocks_to_check;

		/*
		 * Get a parallel slot for the next amcheck command, blocking if
		 * necessary until one is available, or until a previously issued slot
		 * command fails, indicating that we should abort checking the
		 * remaining objects.
		 */
		/*
		 * 为下一个 amcheck 命令获取一个并行插槽，如果需要，阻塞直到有一个可用，
		 * 或者直到先前发出的插槽命令失败，这表明我们应该中止检查剩余的对象。
		 */
		free_slot = ParallelSlotsGetIdle(sa, rel->datinfo->datname);
		if (!free_slot)
		{
			/*
			 * Something failed.  We don't need to know what it was, because
			 * the handler should already have emitted the necessary error
			 * messages.
			 */
			/*
			 * 发生了故障。我们不需要知道具体是什么，因为处理程序（handler）
			 * 应该已经发出了必要的错误信息。
			 */
			failed = true;
			break;
		}

		if (opts.verbose)
			PQsetErrorVerbosity(free_slot->connection, PQERRORS_VERBOSE);

		/*
		 * Execute the appropriate amcheck command for this relation using our
		 * slot's database connection.  We do not wait for the command to
		 * complete, nor do we perform any error checking, as that is done by
		 * the parallel slots and our handler callback functions.
		 */
		/*
		 * 使用我们插槽的数据库连接为该关系执行适当的 amcheck 命令。我们不等待命令
		 * 完成，也不执行任何错误检查，因为这些工作由并行插槽和我们的回调处理函数完成。
		 */
		if (rel->is_heap)
		{
			if (opts.verbose)
			{
				if (opts.show_progress && progress_since_last_stderr)
					fprintf(stderr, "\n");
				pg_log_info("checking heap table \"%s.%s.%s\"",
							rel->datinfo->datname, rel->nspname, rel->relname);
				progress_since_last_stderr = false;
			}
			prepare_heap_command(&sql, rel, free_slot->connection);
			rel->sql = pstrdup(sql.data);	/* pg_free'd after command */
			ParallelSlotSetHandler(free_slot, verify_heap_slot_handler, rel);
			run_command(free_slot, rel->sql);
		}
		else
		{
			if (opts.verbose)
			{
				if (opts.show_progress && progress_since_last_stderr)
					fprintf(stderr, "\n");

				pg_log_info("checking btree index \"%s.%s.%s\"",
							rel->datinfo->datname, rel->nspname, rel->relname);
				progress_since_last_stderr = false;
			}
			prepare_btree_command(&sql, rel, free_slot->connection);
			rel->sql = pstrdup(sql.data);	/* pg_free'd after command */
			ParallelSlotSetHandler(free_slot, verify_btree_slot_handler, rel);
			run_command(free_slot, rel->sql);
		}
	}
	termPQExpBuffer(&sql);

	if (!failed)
	{

		/*
		 * Wait for all slots to complete, or for one to indicate that an
		 * error occurred.  Like above, we rely on the handler emitting the
		 * necessary error messages.
		 */
		/*
		 * 等待所有插槽完成，或者等待其中一个指示发生错误。如上所述，我们依赖
		 * 处理程序发出必要的错误信息。
		 */
		if (sa && !ParallelSlotsWaitCompletion(sa))
			failed = true;

		progress_report(reltotal, relprogress, pagestotal, pageschecked, NULL, true, true);
	}

	if (sa)
	{
		ParallelSlotsTerminate(sa);
		FREE_AND_SET_NULL(sa);
	}

	if (failed)
		exit(1);

	if (!all_checks_pass)
		exit(2);
}

/*
 * prepare_heap_command
 *
 * Creates a SQL command for running amcheck checking on the given heap
 * relation.  The command is phrased as a SQL query, with column order and
 * names matching the expectations of verify_heap_slot_handler, which will
 * receive and handle each row returned from the verify_heapam() function.
 *
 * The constructed SQL command will silently skip temporary tables, as checking
 * them would needlessly draw errors from the underlying amcheck function.
 *
 * sql: buffer into which the heap table checking command will be written
 * rel: relation information for the heap table to be checked
 * conn: the connection to be used, for string escaping purposes
 */
/*
 * prepare_heap_command
 *
 * 创建一个用于在给定的堆表关系上运行 amcheck 检查的 SQL 命令。该命令以 SQL 查询的形式
 * 表达，其列顺序和名称符合 verify_heap_slot_handler 的预期，该处理程序将接收并处理
 * 从 verify_heapam() 函数返回的每一行。
 *
 * 构建的 SQL 命令将静默跳过临时表，因为检查它们会无谓地引发底层 amcheck 函数的错误。
 *
 * sql: 将写入堆表检查命令的缓冲区
 * rel: 要检查的堆表的关系信息
 * conn: 要使用的连接，用于字符串转义目的
 */
static void
prepare_heap_command(PQExpBuffer sql, RelationInfo *rel, PGconn *conn)
{
	resetPQExpBuffer(sql);
	appendPQExpBuffer(sql,
					  "SELECT v.blkno, v.offnum, v.attnum, v.msg "
					  "FROM pg_catalog.pg_class c, %s.verify_heapam("
					  "\nrelation := c.oid, on_error_stop := %s, check_toast := %s, skip := '%s'",
					  rel->datinfo->amcheck_schema,
					  opts.on_error_stop ? "true" : "false",
					  opts.reconcile_toast ? "true" : "false",
					  opts.skip);

	if (opts.startblock >= 0)
		appendPQExpBuffer(sql, ", startblock := " INT64_FORMAT, opts.startblock);
	if (opts.endblock >= 0)
		appendPQExpBuffer(sql, ", endblock := " INT64_FORMAT, opts.endblock);

	appendPQExpBuffer(sql,
					  "\n) v WHERE c.oid = %u "
					  "AND c.relpersistence != " CppAsString2(RELPERSISTENCE_TEMP),
					  rel->reloid);
}

/*
 * prepare_btree_command
 *
 * Creates a SQL command for running amcheck checking on the given btree index
 * relation.  The command does not select any columns, as btree checking
 * functions do not return any, but rather return corruption information by
 * raising errors, which verify_btree_slot_handler expects.
 *
 * The constructed SQL command will silently skip temporary indexes, and
 * indexes being reindexed concurrently, as checking them would needlessly draw
 * errors from the underlying amcheck functions.
 *
 * sql: buffer into which the heap table checking command will be written
 * rel: relation information for the index to be checked
 * conn: the connection to be used, for string escaping purposes
 */
/*
 * prepare_btree_command
 *
 * 创建一个用于在给定的 B 树索引关系上运行 amcheck 检查的 SQL 命令。该命令不选择任何列，
 * 因为 B 树检查函数不返回任何列，而是通过引发错误（ERROR）来返回损坏信息，
 * verify_btree_slot_handler 正是期望如此。
 *
 * 构建的 SQL 命令将静默跳过临时索引，以及并发重建的索引（CONCURRENTLY），
 * 因为检查它们会无谓地引发底层 amcheck 函数的错误。
 *
 * sql: 将写入堆表检查命令的缓冲区
 * rel: 要检查的索引的关系信息
 * conn: 要使用的连接，用于字符串转义目的
 */
static void
prepare_btree_command(PQExpBuffer sql, RelationInfo *rel, PGconn *conn)
{
	resetPQExpBuffer(sql);

	if (opts.parent_check)
		appendPQExpBuffer(sql,
						  "SELECT %s.bt_index_parent_check("
						  "index := c.oid, heapallindexed := %s, rootdescend := %s "
						  "%s)"
						  "\nFROM pg_catalog.pg_class c, pg_catalog.pg_index i "
						  "WHERE c.oid = %u "
						  "AND c.oid = i.indexrelid "
						  "AND c.relpersistence != " CppAsString2(RELPERSISTENCE_TEMP) " "
						  "AND i.indisready AND i.indisvalid AND i.indislive",
						  rel->datinfo->amcheck_schema,
						  (opts.heapallindexed ? "true" : "false"),
						  (opts.rootdescend ? "true" : "false"),
						  (rel->datinfo->is_checkunique ? ", checkunique := true" : ""),
						  rel->reloid);
	else
		appendPQExpBuffer(sql,
						  "SELECT %s.bt_index_check("
						  "index := c.oid, heapallindexed := %s "
						  "%s)"
						  "\nFROM pg_catalog.pg_class c, pg_catalog.pg_index i "
						  "WHERE c.oid = %u "
						  "AND c.oid = i.indexrelid "
						  "AND c.relpersistence != " CppAsString2(RELPERSISTENCE_TEMP) " "
						  "AND i.indisready AND i.indisvalid AND i.indislive",
						  rel->datinfo->amcheck_schema,
						  (opts.heapallindexed ? "true" : "false"),
						  (rel->datinfo->is_checkunique ? ", checkunique := true" : ""),
						  rel->reloid);
}

/*
 * run_command
 *
 * Sends a command to the server without waiting for the command to complete.
 * Logs an error if the command cannot be sent, but otherwise any errors are
 * expected to be handled by a ParallelSlotHandler.
 *
 * If reconnecting to the database is necessary, the cparams argument may be
 * modified.
 *
 * slot: slot with connection to the server we should use for the command
 * sql: query to send
 */
/*
 * run_command
 *
 * 向服务器发送命令，而无需等待命令完成。如果无法发送命令则记录错误，
 * 但除此之外，任何错误均应由 ParallelSlotHandler 处理。
 *
 * 如果需要重新连接数据库，可能会修改 cparams 参数。
 *
 * slot: 包含我们应用于该命令的服务器连接的插槽
 * sql: 要发送的查询
 */
static void
run_command(ParallelSlot *slot, const char *sql)
{
	if (opts.echo)
		printf("%s\n", sql);

	if (PQsendQuery(slot->connection, sql) == 0)
	{
		pg_log_error("error sending command to database \"%s\": %s",
					 PQdb(slot->connection),
					 PQerrorMessage(slot->connection));
		pg_log_error_detail("Command was: %s", sql);
		exit(1);
	}
}

/*
 * should_processing_continue
 *
 * Checks a query result returned from a query (presumably issued on a slot's
 * connection) to determine if parallel slots should continue issuing further
 * commands.
 *
 * Note: Heap relation corruption is reported by verify_heapam() via the result
 * set, rather than an ERROR, but running verify_heapam() on a corrupted heap
 * table may still result in an error being returned from the server due to
 * missing relation files, bad checksums, etc.  The btree corruption checking
 * functions always use errors to communicate corruption messages.  We can't
 * just abort processing because we got a mere ERROR.
 *
 * res: result from an executed sql query
 */
/*
 * should_processing_continue
 *
 * 检查从查询中返回的查询结果（可能是通过插槽连接发出的），以确定并行插槽是否应继续
 * 发出进一步的命令。
 *
 * 注意：堆表损坏是由 verify_heapam() 通过结果集报告的，而不是通过 ERROR 报告。但是，
 * 由于缺少关系文件、坏校验和等，在损坏的堆表上运行 verify_heapam() 仍可能导致服务器返回错误。
 * B 树损坏检查函数始终使用错误来传递损坏消息。我们不能仅仅因为收到普通的 ERROR 就中止处理。
 *
 * res: 执行的 sql 查询的结果
 */
static bool
should_processing_continue(PGresult *res)
{
	const char *severity;

	switch (PQresultStatus(res))
	{
			/* These are expected and ok */
	/* 这些是符合预期的，没有问题 */
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
		case PGRES_NONFATAL_ERROR:
			break;

			/* This is expected but requires closer scrutiny */
	/* 这是符合预期的，但需要更仔细的审查 */
		case PGRES_FATAL_ERROR:
			severity = PQresultErrorField(res, PG_DIAG_SEVERITY_NONLOCALIZED);
			if (severity == NULL)
				return false;	/* libpq failure, probably lost connection */
	/* libpq 失败，可能丢失了连接 */
			if (strcmp(severity, "FATAL") == 0)
				return false;
			if (strcmp(severity, "PANIC") == 0)
				return false;
			break;

			/* These are unexpected */
		case PGRES_BAD_RESPONSE:
		case PGRES_EMPTY_QUERY:
		case PGRES_COPY_OUT:
		case PGRES_COPY_IN:
		case PGRES_COPY_BOTH:
		case PGRES_SINGLE_TUPLE:
		case PGRES_PIPELINE_SYNC:
		case PGRES_PIPELINE_ABORTED:
		case PGRES_TUPLES_CHUNK:
			return false;
	}
	return true;
}

/*
 * Returns a copy of the argument string with all lines indented four spaces.
 *
 * The caller should pg_free the result when finished with it.
 */
/*
 * 返回参数字符串的副本，其中所有行均缩进四个空格。
 *
 * 调用者在使用完毕后应 pg_free 释放结果。
 */
static char *
indent_lines(const char *str)
{
	PQExpBufferData buf;
	const char *c;
	char	   *result;

	initPQExpBuffer(&buf);
	appendPQExpBufferStr(&buf, "    ");
	for (c = str; *c; c++)
	{
		appendPQExpBufferChar(&buf, *c);
		if (c[0] == '\n' && c[1] != '\0')
			appendPQExpBufferStr(&buf, "    ");
	}
	result = pstrdup(buf.data);
	termPQExpBuffer(&buf);

	return result;
}

/*
 * verify_heap_slot_handler
 *
 * ParallelSlotHandler that receives results from a heap table checking command
 * created by prepare_heap_command and outputs the results for the user.
 *
 * res: result from an executed sql query
 * conn: connection on which the sql query was executed
 * context: the sql query being handled, as a cstring
 */
/*
 * verify_heap_slot_handler
 *
 * 并行插槽处理程序，它接收由 prepare_heap_command 创建的堆表检查命令返回的结果，
 * 并向用户输出这些结果。
 *
 * res: 执行的 sql 查询的结果
 * conn: 执行 sql 查询所在的连接
 * context: 正在处理的 sql 查询，作为 cstring
 */
static bool
verify_heap_slot_handler(PGresult *res, PGconn *conn, void *context)
{
	RelationInfo *rel = (RelationInfo *) context;

	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		int			i;
		int			ntups = PQntuples(res);

		if (ntups > 0)
			all_checks_pass = false;

		for (i = 0; i < ntups; i++)
		{
			const char *msg;

			/* The message string should never be null, but check */
	/* 消息字符串永远不应为空，但仍做检查 */
			if (PQgetisnull(res, i, 3))
				msg = "NO MESSAGE";
			else
				msg = PQgetvalue(res, i, 3);

			if (!PQgetisnull(res, i, 2))
				printf(_("heap table \"%s.%s.%s\", block %s, offset %s, attribute %s:\n"),
					   rel->datinfo->datname, rel->nspname, rel->relname,
					   PQgetvalue(res, i, 0),	/* blkno */
					   PQgetvalue(res, i, 1),	/* offnum */
					   PQgetvalue(res, i, 2));	/* attnum */

			else if (!PQgetisnull(res, i, 1))
				printf(_("heap table \"%s.%s.%s\", block %s, offset %s:\n"),
					   rel->datinfo->datname, rel->nspname, rel->relname,
					   PQgetvalue(res, i, 0),	/* blkno */
					   PQgetvalue(res, i, 1));	/* offnum */

			else if (!PQgetisnull(res, i, 0))
				printf(_("heap table \"%s.%s.%s\", block %s:\n"),
					   rel->datinfo->datname, rel->nspname, rel->relname,
					   PQgetvalue(res, i, 0));	/* blkno */

			else
				printf(_("heap table \"%s.%s.%s\":\n"),
					   rel->datinfo->datname, rel->nspname, rel->relname);

			printf("    %s\n", msg);
		}
	}
	else if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		char	   *msg = indent_lines(PQerrorMessage(conn));

		all_checks_pass = false;
		printf(_("heap table \"%s.%s.%s\":\n"),
			   rel->datinfo->datname, rel->nspname, rel->relname);
		printf("%s", msg);
		if (opts.verbose)
			printf(_("query was: %s\n"), rel->sql);
		FREE_AND_SET_NULL(msg);
	}

	FREE_AND_SET_NULL(rel->sql);
	FREE_AND_SET_NULL(rel->nspname);
	FREE_AND_SET_NULL(rel->relname);

	return should_processing_continue(res);
}

/*
 * verify_btree_slot_handler
 *
 * ParallelSlotHandler that receives results from a btree checking command
 * created by prepare_btree_command and outputs them for the user.  The results
 * from the btree checking command is assumed to be empty, but when the results
 * are an error code, the useful information about the corruption is expected
 * in the connection's error message.
 *
 * res: result from an executed sql query
 * conn: connection on which the sql query was executed
 * context: unused
 */
/*
 * verify_btree_slot_handler
 *
 * 并行插槽处理程序，接收由 prepare_btree_command 创建的 B 树检查命令的结果，
 * 并将其输出给用户。B 树检查命令的常规返回结果应为空，但当结果是一个错误代码时，
 * 有关损坏的有用信息应包含在连接的错误消息中。
 *
 * res: 执行的 sql 查询的结果
 * conn: 执行 sql 查询所在的连接
 * context: 未使用
 */
static bool
verify_btree_slot_handler(PGresult *res, PGconn *conn, void *context)
{
	RelationInfo *rel = (RelationInfo *) context;

	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		int			ntups = PQntuples(res);

		if (ntups > 1)
		{
			/*
			 * We expect the btree checking functions to return one void row
			 * each, or zero rows if the check was skipped due to the object
			 * being in the wrong state to be checked, so we should output
			 * some sort of warning if we get anything more, not because it
			 * indicates corruption, but because it suggests a mismatch
			 * between amcheck and pg_amcheck versions.
			 *
			 * In conjunction with --progress, anything written to stderr at
			 * this time would present strangely to the user without an extra
			 * newline, so we print one.  If we were multithreaded, we'd have
			 * to avoid splitting this across multiple calls, but we're in an
			 * event loop, so it doesn't matter.
			 */
			/*
			 * 我们希望每个 B 树检查函数都返回一个空行（void row），或者在由于对象处于错误状态
			 * 而跳过检查时返回零行，因此如果得到更多数据，我们应该输出某种警告，
			 * 这不是因为它表示数据损坏，而是因为它暗示 amcheck 和 pg_amcheck 版本不匹配。
			 *
			 * 结合使用 --progress 选项，如果不额外换行，此时写入 stderr 的任何内容都会给用户带来
			 * 奇怪的展现，所以我们打印一个换行。如果是多线程的，我们将不得不避免跨多次调用拆分此打印，
			 * 但我们处于事件循环中，所以没关系。
			 */
			if (opts.show_progress && progress_since_last_stderr)
				fprintf(stderr, "\n");
			pg_log_warning("btree index \"%s.%s.%s\": btree checking function returned unexpected number of rows: %d",
						   rel->datinfo->datname, rel->nspname, rel->relname, ntups);
			if (opts.verbose)
				pg_log_warning_detail("Query was: %s", rel->sql);
			pg_log_warning_hint("Are %s's and amcheck's versions compatible?",
								progname);
			progress_since_last_stderr = false;
		}
	}
	else
	{
		char	   *msg = indent_lines(PQerrorMessage(conn));

		all_checks_pass = false;
		printf(_("btree index \"%s.%s.%s\":\n"),
			   rel->datinfo->datname, rel->nspname, rel->relname);
		printf("%s", msg);
		if (opts.verbose)
			printf(_("query was: %s\n"), rel->sql);
		FREE_AND_SET_NULL(msg);
	}

	FREE_AND_SET_NULL(rel->sql);
	FREE_AND_SET_NULL(rel->nspname);
	FREE_AND_SET_NULL(rel->relname);

	return should_processing_continue(res);
}

/*
 * help
 *
 * Prints help page for the program
 *
 * progname: the name of the executed program, such as "pg_amcheck"
 */
/*
 * help
 *
 * 打印程序的帮助页面
 *
 * progname: 执行的程序名称，例如 "pg_amcheck"
 */
static void
help(const char *progname)
{
	printf(_("%s checks objects in a PostgreSQL database for corruption.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);
	printf(_("\nTarget options:\n"));
	printf(_("  -a, --all                       check all databases\n"));
	printf(_("  -d, --database=PATTERN          check matching database(s)\n"));
	printf(_("  -D, --exclude-database=PATTERN  do NOT check matching database(s)\n"));
	printf(_("  -i, --index=PATTERN             check matching index(es)\n"));
	printf(_("  -I, --exclude-index=PATTERN     do NOT check matching index(es)\n"));
	printf(_("  -r, --relation=PATTERN          check matching relation(s)\n"));
	printf(_("  -R, --exclude-relation=PATTERN  do NOT check matching relation(s)\n"));
	printf(_("  -s, --schema=PATTERN            check matching schema(s)\n"));
	printf(_("  -S, --exclude-schema=PATTERN    do NOT check matching schema(s)\n"));
	printf(_("  -t, --table=PATTERN             check matching table(s)\n"));
	printf(_("  -T, --exclude-table=PATTERN     do NOT check matching table(s)\n"));
	printf(_("      --no-dependent-indexes      do NOT expand list of relations to include indexes\n"));
	printf(_("      --no-dependent-toast        do NOT expand list of relations to include TOAST tables\n"));
	printf(_("      --no-strict-names           do NOT require patterns to match objects\n"));
	printf(_("\nTable checking options:\n"));
	printf(_("      --exclude-toast-pointers    do NOT follow relation TOAST pointers\n"));
	printf(_("      --on-error-stop             stop checking at end of first corrupt page\n"));
	printf(_("      --skip=OPTION               do NOT check \"all-frozen\" or \"all-visible\" blocks\n"));
	printf(_("      --startblock=BLOCK          begin checking table(s) at the given block number\n"));
	printf(_("      --endblock=BLOCK            check table(s) only up to the given block number\n"));
	printf(_("\nB-tree index checking options:\n"));
	printf(_("      --checkunique               check unique constraint if index is unique\n"));
	printf(_("      --heapallindexed            check that all heap tuples are found within indexes\n"));
	printf(_("      --parent-check              check index parent/child relationships\n"));
	printf(_("      --rootdescend               search from root page to refind tuples\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME             database server host or socket directory\n"));
	printf(_("  -p, --port=PORT                 database server port\n"));
	printf(_("  -U, --username=USERNAME         user name to connect as\n"));
	printf(_("  -w, --no-password               never prompt for password\n"));
	printf(_("  -W, --password                  force password prompt\n"));
	printf(_("      --maintenance-db=DBNAME     alternate maintenance database\n"));
	printf(_("\nOther options:\n"));
	printf(_("  -e, --echo                      show the commands being sent to the server\n"));
	printf(_("  -j, --jobs=NUM                  use this many concurrent connections to the server\n"));
	printf(_("  -P, --progress                  show progress information\n"));
	printf(_("  -v, --verbose                   write a lot of output\n"));
	printf(_("  -V, --version                   output version information, then exit\n"));
	printf(_("      --install-missing           install missing extensions\n"));
	printf(_("  -?, --help                      show this help, then exit\n"));

	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Print a progress report based on the global variables.
 *
 * Progress report is written at maximum once per second, unless the force
 * parameter is set to true.
 *
 * If finished is set to true, this is the last progress report. The cursor
 * is moved to the next line.
 */
/*
 * 根据全局变量打印进度报告。
 *
 * 进度报告最多每秒写入一次，除非将 force 参数设置为 true。
 *
 * 如果 finished 设置为 true，这是最后一次进度报告。光标将移动到下一行。
 */
static void
progress_report(uint64 relations_total, uint64 relations_checked,
				uint64 relpages_total, uint64 relpages_checked,
				const char *datname, bool force, bool finished)
{
	int			percent_rel = 0;
	int			percent_pages = 0;
	char		checked_rel[32];
	char		total_rel[32];
	char		checked_pages[32];
	char		total_pages[32];
	pg_time_t	now;

	if (!opts.show_progress)
		return;

	now = time(NULL);
	if (now == last_progress_report && !force && !finished)
		return;					/* Max once per second */

	last_progress_report = now;
	if (relations_total)
		percent_rel = (int) (relations_checked * 100 / relations_total);
	if (relpages_total)
		percent_pages = (int) (relpages_checked * 100 / relpages_total);

	snprintf(checked_rel, sizeof(checked_rel), UINT64_FORMAT, relations_checked);
	snprintf(total_rel, sizeof(total_rel), UINT64_FORMAT, relations_total);
	snprintf(checked_pages, sizeof(checked_pages), UINT64_FORMAT, relpages_checked);
	snprintf(total_pages, sizeof(total_pages), UINT64_FORMAT, relpages_total);

#define VERBOSE_DATNAME_LENGTH 35
	if (opts.verbose)
	{
		if (!datname)

			/*
			 * No datname given, so clear the status line (used for first and
			 * last call)
			 */
			/*
			 * 未给出 datname，因此清除状态行（用于第一次和最后一次调用）
			 */
			fprintf(stderr,
					_("%*s/%s relations (%d%%), %*s/%s pages (%d%%) %*s"),
					(int) strlen(total_rel),
					checked_rel, total_rel, percent_rel,
					(int) strlen(total_pages),
					checked_pages, total_pages, percent_pages,
					VERBOSE_DATNAME_LENGTH + 2, "");
		else
		{
			bool		truncate = (strlen(datname) > VERBOSE_DATNAME_LENGTH);

			fprintf(stderr,
					_("%*s/%s relations (%d%%), %*s/%s pages (%d%%) (%s%-*.*s)"),
					(int) strlen(total_rel),
					checked_rel, total_rel, percent_rel,
					(int) strlen(total_pages),
					checked_pages, total_pages, percent_pages,
			/* Prefix with "..." if we do leading truncation */
			/* 如果我们进行前导截断，则前缀为 "..." */
					truncate ? "..." : "",
					truncate ? VERBOSE_DATNAME_LENGTH - 3 : VERBOSE_DATNAME_LENGTH,
					truncate ? VERBOSE_DATNAME_LENGTH - 3 : VERBOSE_DATNAME_LENGTH,
			/* Truncate datname at beginning if it's too long */
			/* 如果太长，则在开始处截断 datname */
					truncate ? datname + strlen(datname) - VERBOSE_DATNAME_LENGTH + 3 : datname);
		}
	}
	else
		fprintf(stderr,
				_("%*s/%s relations (%d%%), %*s/%s pages (%d%%)"),
				(int) strlen(total_rel),
				checked_rel, total_rel, percent_rel,
				(int) strlen(total_pages),
				checked_pages, total_pages, percent_pages);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	/*
	 * 如果向终端报告并且尚未完成，则保持在同一行。
	 */
	if (!finished && isatty(fileno(stderr)))
	{
		fputc('\r', stderr);
		progress_since_last_stderr = true;
	}
	else
		fputc('\n', stderr);
}

/*
 * Extend the pattern info array to hold one additional initialized pattern
 * info entry.
 *
 * Returns a pointer to the new entry.
 */
/*
 * 扩展模式信息数组以容纳另外一个已初始化的模式信息条目。
 *
 * 返回指向新条目的指针。
 */
static PatternInfo *
extend_pattern_info_array(PatternInfoArray *pia)
{
	PatternInfo *result;

	pia->len++;
	pia->data = (PatternInfo *) pg_realloc(pia->data, pia->len * sizeof(PatternInfo));
	result = &pia->data[pia->len - 1];
	memset(result, 0, sizeof(*result));

	return result;
}

/*
 * append_database_pattern
 *
 * Adds the given pattern interpreted as a database name pattern.
 *
 * pia: the pattern info array to be appended
 * pattern: the database name pattern
 * encoding: client encoding for parsing the pattern
 */
/*
 * append_database_pattern
 *
 * 添加给定模式，解析为数据库名称模式。
 *
 * pia: 要追加的模式信息数组
 * pattern: 数据库名称模式
 * encoding: 用于解析模式的客户端编码
 */
static void
append_database_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	PQExpBufferData buf;
	int			dotcnt;
	PatternInfo *info = extend_pattern_info_array(pia);

	initPQExpBuffer(&buf);
	patternToSQLRegex(encoding, NULL, NULL, &buf, pattern, false, false,
					  &dotcnt);
	if (dotcnt > 0)
	{
		pg_log_error("improper qualified name (too many dotted names): %s", pattern);
		exit(2);
	}
	info->pattern = pattern;
	info->db_regex = pstrdup(buf.data);

	termPQExpBuffer(&buf);
}

/*
 * append_schema_pattern
 *
 * Adds the given pattern interpreted as a schema name pattern.
 *
 * pia: the pattern info array to be appended
 * pattern: the schema name pattern
 * encoding: client encoding for parsing the pattern
 */
/*
 * append_schema_pattern
 *
 * 添加给定模式，解析为模式（schema）名称模式。
 *
 * pia: 要追加的模式信息数组
 * pattern: 模式（schema）名称模式
 * encoding: 用于解析模式的客户端编码
 */
static void
append_schema_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	PQExpBufferData dbbuf;
	PQExpBufferData nspbuf;
	int			dotcnt;
	PatternInfo *info = extend_pattern_info_array(pia);

	initPQExpBuffer(&dbbuf);
	initPQExpBuffer(&nspbuf);

	patternToSQLRegex(encoding, NULL, &dbbuf, &nspbuf, pattern, false, false,
					  &dotcnt);
	if (dotcnt > 1)
	{
		pg_log_error("improper qualified name (too many dotted names): %s", pattern);
		exit(2);
	}
	info->pattern = pattern;
	if (dbbuf.data[0])
	{
		opts.dbpattern = true;
		info->db_regex = pstrdup(dbbuf.data);
	}
	if (nspbuf.data[0])
		info->nsp_regex = pstrdup(nspbuf.data);

	termPQExpBuffer(&dbbuf);
	termPQExpBuffer(&nspbuf);
}

/*
 * append_relation_pattern_helper
 *
 * Adds to a list the given pattern interpreted as a relation pattern.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 * heap_only: whether the pattern should only be matched against heap tables
 * btree_only: whether the pattern should only be matched against btree indexes
 */
/*
 * append_relation_pattern_helper
 *
 * 将给定模式（解析为关系模式）添加到列表中。
 *
 * pia: 要追加的模式信息数组
 * pattern: 关系名称模式
 * encoding: 用于解析模式的客户端编码
 * heap_only: 该模式是否应该仅匹配堆表
 * btree_only: 该模式是否应该仅匹配 B 树索引
 */
static void
append_relation_pattern_helper(PatternInfoArray *pia, const char *pattern,
							   int encoding, bool heap_only, bool btree_only)
{
	PQExpBufferData dbbuf;
	PQExpBufferData nspbuf;
	PQExpBufferData relbuf;
	int			dotcnt;
	PatternInfo *info = extend_pattern_info_array(pia);

	initPQExpBuffer(&dbbuf);
	initPQExpBuffer(&nspbuf);
	initPQExpBuffer(&relbuf);

	patternToSQLRegex(encoding, &dbbuf, &nspbuf, &relbuf, pattern, false,
					  false, &dotcnt);
	if (dotcnt > 2)
	{
		pg_log_error("improper relation name (too many dotted names): %s", pattern);
		exit(2);
	}
	info->pattern = pattern;
	if (dbbuf.data[0])
	{
		opts.dbpattern = true;
		info->db_regex = pstrdup(dbbuf.data);
	}
	if (nspbuf.data[0])
		info->nsp_regex = pstrdup(nspbuf.data);
	if (relbuf.data[0])
		info->rel_regex = pstrdup(relbuf.data);

	termPQExpBuffer(&dbbuf);
	termPQExpBuffer(&nspbuf);
	termPQExpBuffer(&relbuf);

	info->heap_only = heap_only;
	info->btree_only = btree_only;
}

/*
 * append_relation_pattern
 *
 * Adds the given pattern interpreted as a relation pattern, to be matched
 * against both heap tables and btree indexes.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 */
/*
 * append_relation_pattern
 *
 * 添加给定的关系模式，用于与堆表和 B 树索引进行匹配。
 *
 * pia: 要追加的模式信息数组
 * pattern: 关系名称模式
 * encoding: 用于解析模式的客户端编码
 */
static void
append_relation_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	append_relation_pattern_helper(pia, pattern, encoding, false, false);
}

/*
 * append_heap_pattern
 *
 * Adds the given pattern interpreted as a relation pattern, to be matched only
 * against heap tables.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 */
/*
 * append_heap_pattern
 *
 * 添加给定的关系模式，仅与堆表进行匹配。
 *
 * pia: 要追加的模式信息数组
 * pattern: 关系名称模式
 * encoding: 用于解析模式的客户端编码
 */
static void
append_heap_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	append_relation_pattern_helper(pia, pattern, encoding, true, false);
}

/*
 * append_btree_pattern
 *
 * Adds the given pattern interpreted as a relation pattern, to be matched only
 * against btree indexes.
 *
 * pia: the pattern info array to be appended
 * pattern: the relation name pattern
 * encoding: client encoding for parsing the pattern
 */
/*
 * append_btree_pattern
 *
 * 添加给定的关系模式，仅与 B 树索引进行匹配。
 *
 * pia: 要追加的模式信息数组
 * pattern: 关系名称模式
 * encoding: 用于解析模式的客户端编码
 */
static void
append_btree_pattern(PatternInfoArray *pia, const char *pattern, int encoding)
{
	append_relation_pattern_helper(pia, pattern, encoding, false, true);
}

/*
 * append_db_pattern_cte
 *
 * Appends to the buffer the body of a Common Table Expression (CTE) containing
 * the database portions filtered from the list of patterns expressed as two
 * columns:
 *
 *     pattern_id: the index of this pattern in pia->data[]
 *     rgx: the database regular expression parsed from the pattern
 *
 * Patterns without a database portion are skipped.  Patterns with more than
 * just a database portion are optionally skipped, depending on argument
 * 'inclusive'.
 *
 * buf: the buffer to be appended
 * pia: the array of patterns to be inserted into the CTE
 * conn: the database connection
 * inclusive: whether to include patterns with schema and/or relation parts
 *
 * Returns whether any database patterns were appended.
 */
/*
 * append_db_pattern_cte
 *
 * 向缓冲区追加公共表表达式（CTE）的主体，该表达式包含从模式列表中过滤的数据库部分，
 * 表达为两列：
 *
 *     pattern_id: 此模式在 pia->data[] 中的索引
 *     rgx: 从模式中解析出的数据库正则表达式
 *
 * 跳过没有数据库部分的模式。根据参数 'inclusive'，具有多于仅数据库部分的模式可能会被跳过。
 *
 * buf: 要追加的缓冲区
 * pia: 要插入 CTE 的模式数组
 * conn: 数据库连接
 * inclusive: 是否包含带有 schema 和/或 relation 部分的模式
 *
 * 返回是否追加了任何数据库模式。
 */
static bool
append_db_pattern_cte(PQExpBuffer buf, const PatternInfoArray *pia,
					  PGconn *conn, bool inclusive)
{
	int			pattern_id;
	const char *comma;
	bool		have_values;

	comma = "";
	have_values = false;
	for (pattern_id = 0; pattern_id < pia->len; pattern_id++)
	{
		PatternInfo *info = &pia->data[pattern_id];

		if (info->db_regex != NULL &&
			(inclusive || (info->nsp_regex == NULL && info->rel_regex == NULL)))
		{
			if (!have_values)
				appendPQExpBufferStr(buf, "\nVALUES");
			have_values = true;
			appendPQExpBuffer(buf, "%s\n(%d, ", comma, pattern_id);
			appendStringLiteralConn(buf, info->db_regex, conn);
			appendPQExpBufferChar(buf, ')');
			comma = ",";
		}
	}

	if (!have_values)
		appendPQExpBufferStr(buf, "\nSELECT NULL, NULL, NULL WHERE false");

	return have_values;
}

/*
 * compile_database_list
 *
 * If any database patterns exist, or if --all was given, compiles a distinct
 * list of databases to check using a SQL query based on the patterns plus the
 * literal initial database name, if given.  If no database patterns exist and
 * --all was not given, the query is not necessary, and only the initial
 * database name (if any) is added to the list.
 *
 * conn: connection to the initial database
 * databases: the list onto which databases should be appended
 * initial_dbname: an optional extra database name to include in the list
 */
/*
 * compile_database_list
 *
 * 如果存在任何数据库模式，或者如果给出了 --all 选项，则通过基于该模式以及字面初始数据库名称
 * （如果给定）的 SQL 查询，编译一个要去检查的非重复数据库列表。如果不存在数据库模式且没有
 * 给出 --all，则不需要执行此查询，并且仅将初始数据库名称（如果有）添加到列表中。
 *
 * conn: 到初始数据库的连接
 * databases: 数据库应追加到的目标列表
 * initial_dbname: 要包含在列表中的可选附加数据库名称
 */
static void
compile_database_list(PGconn *conn, SimplePtrList *databases,
					  const char *initial_dbname)
{
	PGresult   *res;
	PQExpBufferData sql;
	int			ntups;
	int			i;
	bool		fatal;

	if (initial_dbname)
	{
		DatabaseInfo *dat = (DatabaseInfo *) pg_malloc0(sizeof(DatabaseInfo));

		/* This database is included.  Add to list */
	/* 此数据库已被包含。添加到列表中 */
		if (opts.verbose)
			pg_log_info("including database \"%s\"", initial_dbname);

		dat->datname = pstrdup(initial_dbname);
		simple_ptr_list_append(databases, dat);
	}

	initPQExpBuffer(&sql);

	/* Append the include patterns CTE. */
	/* 追加包含模式的公共表表达式（include patterns CTE）。 */
	appendPQExpBufferStr(&sql, "WITH include_raw (pattern_id, rgx) AS (");
	if (!append_db_pattern_cte(&sql, &opts.include, conn, true) &&
		!opts.alldb)
	{
		/*
		 * None of the inclusion patterns (if any) contain database portions,
		 * so there is no need to query the database to resolve database
		 * patterns.
		 *
		 * Since we're also not operating under --all, we don't need to query
		 * the exhaustive list of connectable databases, either.
		 */
		/*
		 * 包含模式（如果有）中均不包含数据库部分，因此无需查询数据库来解析数据库模式。
		 *
		 * 另外由于我们不在 --all 选项下运行，我们也不需要查询可连接数据库的详尽列表。
		 */
		termPQExpBuffer(&sql);
		return;
	}

	/* Append the exclude patterns CTE. */
	/* 追加排除模式的公共表表达式（exclude patterns CTE）。 */
	appendPQExpBufferStr(&sql, "),\nexclude_raw (pattern_id, rgx) AS (");
	append_db_pattern_cte(&sql, &opts.exclude, conn, false);
	appendPQExpBufferStr(&sql, "),");

	/*
	 * Append the database CTE, which includes whether each database is
	 * connectable and also joins against exclude_raw to determine whether
	 * each database is excluded.
	 */
	/*
	 * 追加数据库公共表表达式（database CTE），该表达式包括每个数据库是否可连接，
	 * 并且还与 exclude_raw 进行连接（join）以确定是否排除每个数据库。
	 */
	appendPQExpBufferStr(&sql,
						 "\ndatabase (datname) AS ("
						 "\nSELECT d.datname "
						 "FROM pg_catalog.pg_database d "
						 "LEFT OUTER JOIN exclude_raw e "
						 "ON d.datname ~ e.rgx "
						 "\nWHERE d.datallowconn AND datconnlimit != -2 "
						 "AND e.pattern_id IS NULL"
						 "),"

	/*
	 * Append the include_pat CTE, which joins the include_raw CTE against the
	 * databases CTE to determine if all the inclusion patterns had matches,
	 * and whether each matched pattern had the misfortune of only matching
	 * excluded or unconnectable databases.
	 */
	/*
	 * 追加 include_pat 公共表表达式，它将 include_raw 与 databases 进行连接，
	 * 以确定所有包含模式是否有匹配，以及每个匹配模式是否不幸地仅匹配了排除的或不可连接的数据库。
	 */
						 "\ninclude_pat (pattern_id, checkable) AS ("
						 "\nSELECT i.pattern_id, "
						 "COUNT(*) FILTER ("
						 "WHERE d IS NOT NULL"
						 ") AS checkable"
						 "\nFROM include_raw i "
						 "LEFT OUTER JOIN database d "
						 "ON d.datname ~ i.rgx"
						 "\nGROUP BY i.pattern_id"
						 "),"

	/*
	 * Append the filtered_databases CTE, which selects from the database CTE
	 * optionally joined against the include_raw CTE to only select databases
	 * that match an inclusion pattern.  This appears to duplicate what the
	 * include_pat CTE already did above, but here we want only databases, and
	 * there we wanted patterns.
	 */
	/*
	 * 追加 filtered_databases 公共表表达式，它从 database 中进行选择，
	 * （可选地与 include_raw 关联）以仅选择与包含模式相匹配的数据库。这似乎与上面
	 * include_pat 的操作重复，但在这里我们只需要数据库，而那里我们需要模式。
	 */
						 "\nfiltered_databases (datname) AS ("
						 "\nSELECT DISTINCT d.datname "
						 "FROM database d");
	if (!opts.alldb)
		appendPQExpBufferStr(&sql,
							 " INNER JOIN include_raw i "
							 "ON d.datname ~ i.rgx");
	appendPQExpBufferStr(&sql,
						 ")"

	/*
	 * Select the checkable databases and the unmatched inclusion patterns.
	 */
	/*
	 * 选择可检查的数据库和未匹配的包含模式。
	 */
						 "\nSELECT pattern_id, datname FROM ("
						 "\nSELECT pattern_id, NULL::TEXT AS datname "
						 "FROM include_pat "
						 "WHERE checkable = 0 "
						 "UNION ALL"
						 "\nSELECT NULL, datname "
						 "FROM filtered_databases"
						 ") AS combined_records"
						 "\nORDER BY pattern_id NULLS LAST, datname");

	res = executeQuery(conn, sql.data, opts.echo);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", sql.data);
		disconnectDatabase(conn);
		exit(1);
	}
	termPQExpBuffer(&sql);

	ntups = PQntuples(res);
	for (fatal = false, i = 0; i < ntups; i++)
	{
		int			pattern_id = -1;
		const char *datname = NULL;

		if (!PQgetisnull(res, i, 0))
			pattern_id = atoi(PQgetvalue(res, i, 0));
		if (!PQgetisnull(res, i, 1))
			datname = PQgetvalue(res, i, 1);

		if (pattern_id >= 0)
		{
			/*
			 * Current record pertains to an inclusion pattern that matched no
			 * checkable databases.
			 */
			/*
			 * 当前记录属于未匹配到任何可检查数据库的包含模式。
			 */
			fatal = opts.strict_names;
			if (pattern_id >= opts.include.len)
				pg_fatal("internal error: received unexpected database pattern_id %d",
						 pattern_id);
			log_no_match("no connectable databases to check matching \"%s\"",
						 opts.include.data[pattern_id].pattern);
		}
		else
		{
			DatabaseInfo *dat;

			/* Current record pertains to a database */
			/* 当前记录属于一个数据库 */
			Assert(datname != NULL);

			/* Avoid entering a duplicate entry matching the initial_dbname */
			/* 避免插入与 initial_dbname 重复的条目 */
			if (initial_dbname != NULL && strcmp(initial_dbname, datname) == 0)
				continue;

			/* This database is included.  Add to list */
			/* 该数据库已被包含。添加到列表中 */
	/* 此数据库已被包含。添加到列表中 */
			if (opts.verbose)
				pg_log_info("including database \"%s\"", datname);

			dat = (DatabaseInfo *) pg_malloc0(sizeof(DatabaseInfo));
			dat->datname = pstrdup(datname);
			simple_ptr_list_append(databases, dat);
		}
	}
	PQclear(res);

	if (fatal)
	{
		if (conn != NULL)
			disconnectDatabase(conn);
		exit(1);
	}
}

/*
 * append_rel_pattern_raw_cte
 *
 * Appends to the buffer the body of a Common Table Expression (CTE) containing
 * the given patterns as six columns:
 *
 *     pattern_id: the index of this pattern in pia->data[]
 *     db_regex: the database regexp parsed from the pattern, or NULL if the
 *               pattern had no database part
 *     nsp_regex: the namespace regexp parsed from the pattern, or NULL if the
 *                pattern had no namespace part
 *     rel_regex: the relname regexp parsed from the pattern, or NULL if the
 *                pattern had no relname part
 *     heap_only: true if the pattern applies only to heap tables (not indexes)
 *     btree_only: true if the pattern applies only to btree indexes (not tables)
 *
 * buf: the buffer to be appended
 * patterns: the array of patterns to be inserted into the CTE
 * conn: the database connection
 */
/*
 * append_rel_pattern_raw_cte
 *
 * 向缓冲区追加公共表表达式（CTE）的主体，其中包含给定模式，表示为六列：
 *
 *     pattern_id: 此模式在 pia->data[] 中的索引
 *     db_regex: 从模式中解析出的数据库正则表达式，如果该模式没有数据库部分，则为 NULL
 *     nsp_regex: 从模式中解析出的命名空间（schema）正则表达式，如果该模式没有命名空间部分，则为 NULL
 *     rel_regex: 从模式中解析出的关系（relname）正则表达式，如果该模式没有关系名部分，则为 NULL
 *     heap_only: 如果该模式仅适用于堆表（而非索引），则为 true
 *     btree_only: 如果该模式仅适用于 B 树索引（而非表），则为 true
 *
 * buf: 要追加的缓冲区
 * patterns: 要插入 CTE 的模式数组
 * conn: 数据库连接
 */
static void
append_rel_pattern_raw_cte(PQExpBuffer buf, const PatternInfoArray *pia,
						   PGconn *conn)
{
	int			pattern_id;
	const char *comma;
	bool		have_values;

	comma = "";
	have_values = false;
	for (pattern_id = 0; pattern_id < pia->len; pattern_id++)
	{
		PatternInfo *info = &pia->data[pattern_id];

		if (!have_values)
			appendPQExpBufferStr(buf, "\nVALUES");
		have_values = true;
		appendPQExpBuffer(buf, "%s\n(%d::INTEGER, ", comma, pattern_id);
		if (info->db_regex == NULL)
			appendPQExpBufferStr(buf, "NULL");
		else
			appendStringLiteralConn(buf, info->db_regex, conn);
		appendPQExpBufferStr(buf, "::TEXT, ");
		if (info->nsp_regex == NULL)
			appendPQExpBufferStr(buf, "NULL");
		else
			appendStringLiteralConn(buf, info->nsp_regex, conn);
		appendPQExpBufferStr(buf, "::TEXT, ");
		if (info->rel_regex == NULL)
			appendPQExpBufferStr(buf, "NULL");
		else
			appendStringLiteralConn(buf, info->rel_regex, conn);
		if (info->heap_only)
			appendPQExpBufferStr(buf, "::TEXT, true::BOOLEAN");
		else
			appendPQExpBufferStr(buf, "::TEXT, false::BOOLEAN");
		if (info->btree_only)
			appendPQExpBufferStr(buf, ", true::BOOLEAN");
		else
			appendPQExpBufferStr(buf, ", false::BOOLEAN");
		appendPQExpBufferChar(buf, ')');
		comma = ",";
	}

	if (!have_values)
		appendPQExpBufferStr(buf,
							 "\nSELECT NULL::INTEGER, NULL::TEXT, NULL::TEXT, "
							 "NULL::TEXT, NULL::BOOLEAN, NULL::BOOLEAN "
							 "WHERE false");
}

/*
 * append_rel_pattern_filtered_cte
 *
 * Appends to the buffer a Common Table Expression (CTE) which selects
 * all patterns from the named raw CTE, filtered by database.  All patterns
 * which have no database portion or whose database portion matches our
 * connection's database name are selected, with other patterns excluded.
 *
 * The basic idea here is that if we're connected to database "foo" and we have
 * patterns "foo.bar.baz", "alpha.beta" and "one.two.three", we only want to
 * use the first two while processing relations in this database, as the third
 * one is not relevant.
 *
 * buf: the buffer to be appended
 * raw: the name of the CTE to select from
 * filtered: the name of the CTE to create
 * conn: the database connection
 */
/*
 * append_rel_pattern_filtered_cte
 *
 * 向缓冲区追加公共表表达式（CTE），该表达式从指定的原始 CTE 中选择所有模式，并按数据库进行过滤。
 * 选择所有没有数据库部分、或者数据库部分与我们的连接数据库名称相匹配的模式，排除其他模式。
 *
 * 这里的基本思想是，如果我们连接到数据库 "foo"，且有模式 "foo.bar.baz"、"alpha.beta" 和
 * "one.two.three"，在我们处理该数据库中的关系时，我们只想使用前两个，因为第三个无关。
 *
 * buf: 要追加的缓冲区
 * raw: 要从中选择的原始 CTE 的名称
 * filtered: 要创建的过滤后 CTE 的名称
 * conn: 数据库连接
 */
static void
append_rel_pattern_filtered_cte(PQExpBuffer buf, const char *raw,
								const char *filtered, PGconn *conn)
{
	appendPQExpBuffer(buf,
					  "\n%s (pattern_id, nsp_regex, rel_regex, heap_only, btree_only) AS ("
					  "\nSELECT pattern_id, nsp_regex, rel_regex, heap_only, btree_only "
					  "FROM %s r"
					  "\nWHERE (r.db_regex IS NULL "
					  "OR ",
					  filtered, raw);
	appendStringLiteralConn(buf, PQdb(conn), conn);
	appendPQExpBufferStr(buf, " ~ r.db_regex)");
	appendPQExpBufferStr(buf,
						 " AND (r.nsp_regex IS NOT NULL"
						 " OR r.rel_regex IS NOT NULL)"
						 "),");
}

/*
 * compile_relation_list_one_db
 *
 * Compiles a list of relations to check within the currently connected
 * database based on the user supplied options, sorted by descending size,
 * and appends them to the given list of relations.
 *
 * The cells of the constructed list contain all information about the relation
 * necessary to connect to the database and check the object, including which
 * database to connect to, where contrib/amcheck is installed, and the Oid and
 * type of object (heap table vs. btree index).  Rather than duplicating the
 * database details per relation, the relation structs use references to the
 * same database object, provided by the caller.
 *
 * conn: connection to this next database, which should be the same as in 'dat'
 * relations: list onto which the relations information should be appended
 * dat: the database info struct for use by each relation
 * pagecount: gets incremented by the number of blocks to check in all
 * relations added
 */
/*
 * compile_relation_list_one_db
 *
 * 根据用户提供的选项，在当前连接的数据库中编译要检查的关系列表（按大小降序排序），
 * 并将它们追加到给定的关系列表中。
 *
 * 构建的列表单元包含连接到数据库和检查对象所需的所有关系信息，包括连接到哪个数据库、
 * 在哪里安装了 contrib/amcheck，以及对象的 Oid 和类型（堆表与 B 树索引）。
 * 关系结构体使用对调用者提供的同一个数据库对象的引用，而不是为每个关系复制数据库细节。
 *
 * conn: 到这下一个数据库的连接，应该与 'dat' 中的相同
 * relations: 关系信息应追加到的目标列表
 * dat: 供每个关系使用的数据库信息结构体
 * pagecount: 加上所有添加的关系中要检查的数据库块数
 */
static void
compile_relation_list_one_db(PGconn *conn, SimplePtrList *relations,
							 const DatabaseInfo *dat,
							 uint64 *pagecount)
{
	PGresult   *res;
	PQExpBufferData sql;
	int			ntups;
	int			i;

	initPQExpBuffer(&sql);
	appendPQExpBufferStr(&sql, "WITH");

	/* Append CTEs for the relation inclusion patterns, if any */
	/* 追加关系包含模式的公共表表达式（如有） */
	if (!opts.allrel)
	{
		appendPQExpBufferStr(&sql,
							 " include_raw (pattern_id, db_regex, nsp_regex, rel_regex, heap_only, btree_only) AS (");
		append_rel_pattern_raw_cte(&sql, &opts.include, conn);
		appendPQExpBufferStr(&sql, "\n),");
		append_rel_pattern_filtered_cte(&sql, "include_raw", "include_pat", conn);
	}

	/* Append CTEs for the relation exclusion patterns, if any */
	/* 追加关系排除模式的公共表表达式（如有） */
	if (opts.excludetbl || opts.excludeidx || opts.excludensp)
	{
		appendPQExpBufferStr(&sql,
							 " exclude_raw (pattern_id, db_regex, nsp_regex, rel_regex, heap_only, btree_only) AS (");
		append_rel_pattern_raw_cte(&sql, &opts.exclude, conn);
		appendPQExpBufferStr(&sql, "\n),");
		append_rel_pattern_filtered_cte(&sql, "exclude_raw", "exclude_pat", conn);
	}

	/* Append the relation CTE. */
	/* 追加关系公共表表达式（relation CTE）。 */
	appendPQExpBufferStr(&sql,
						 " relation (pattern_id, oid, nspname, relname, reltoastrelid, relpages, is_heap, is_btree) AS ("
						 "\nSELECT DISTINCT ON (c.oid");
	if (!opts.allrel)
		appendPQExpBufferStr(&sql, ", ip.pattern_id) ip.pattern_id,");
	else
		appendPQExpBufferStr(&sql, ") NULL::INTEGER AS pattern_id,");
	appendPQExpBuffer(&sql,
					  "\nc.oid, n.nspname, c.relname, c.reltoastrelid, c.relpages, "
					  "c.relam = %u AS is_heap, "
					  "c.relam = %u AS is_btree"
					  "\nFROM pg_catalog.pg_class c "
					  "INNER JOIN pg_catalog.pg_namespace n "
					  "ON c.relnamespace = n.oid",
					  HEAP_TABLE_AM_OID, BTREE_AM_OID);
	if (!opts.allrel)
		appendPQExpBuffer(&sql,
						  "\nINNER JOIN include_pat ip"
						  "\nON (n.nspname ~ ip.nsp_regex OR ip.nsp_regex IS NULL)"
						  "\nAND (c.relname ~ ip.rel_regex OR ip.rel_regex IS NULL)"
						  "\nAND (c.relam = %u OR NOT ip.heap_only)"
						  "\nAND (c.relam = %u OR NOT ip.btree_only)",
						  HEAP_TABLE_AM_OID, BTREE_AM_OID);
	if (opts.excludetbl || opts.excludeidx || opts.excludensp)
		appendPQExpBuffer(&sql,
						  "\nLEFT OUTER JOIN exclude_pat ep"
						  "\nON (n.nspname ~ ep.nsp_regex OR ep.nsp_regex IS NULL)"
						  "\nAND (c.relname ~ ep.rel_regex OR ep.rel_regex IS NULL)"
						  "\nAND (c.relam = %u OR NOT ep.heap_only OR ep.rel_regex IS NULL)"
						  "\nAND (c.relam = %u OR NOT ep.btree_only OR ep.rel_regex IS NULL)",
						  HEAP_TABLE_AM_OID, BTREE_AM_OID);

	/*
	 * Exclude temporary tables and indexes, which must necessarily belong to
	 * other sessions.  (We don't create any ourselves.)  We must ultimately
	 * exclude indexes marked invalid or not ready, but we delay that decision
	 * until firing off the amcheck command, as the state of an index may
	 * change by then.
	 */
	/*
	 * 排除临时表和临时索引，因为它们必定属于其他会话。（我们自己不创建任何临时对象。）
	 * 我们最终必须排除被标记为无效（invalid）或尚未就绪（not ready）的索引，
	 * 但我们会将这一决定延迟到触发 amcheck 命令时，因为届时索引的状态可能会发生改变。
	 */
	appendPQExpBufferStr(&sql, "\nWHERE c.relpersistence != "
						 CppAsString2(RELPERSISTENCE_TEMP));
	if (opts.excludetbl || opts.excludeidx || opts.excludensp)
		appendPQExpBufferStr(&sql, "\nAND ep.pattern_id IS NULL");

	/*
	 * We need to be careful not to break the --no-dependent-toast and
	 * --no-dependent-indexes options.  By default, the btree indexes, toast
	 * tables, and toast table btree indexes associated with primary heap
	 * tables are included, using their own CTEs below.  We implement the
	 * --exclude-* options by not creating those CTEs, but that's no use if
	 * we've already selected the toast and indexes here.  On the other hand,
	 * we want inclusion patterns that match indexes or toast tables to be
	 * honored.  So, if inclusion patterns were given, we want to select all
	 * tables, toast tables, or indexes that match the patterns.  But if no
	 * inclusion patterns were given, and we're simply matching all relations,
	 * then we only want to match the primary tables here.
	 */
	/*
	 * 我们必须小心，不要破坏 --no-dependent-toast 和 --no-dependent-indexes 选项。
	 * 默认情况下，与主要堆表关联的 B 树索引、TOAST 表和 TOAST 表的 B 树索引都会被包含在内，
	 * 并使用下面它们自己的公共表表达式。我们通过不创建这些 CTE 来实现 --exclude-* 选项，
	 * 但如果我们已经在这里选择了 TOAST 和索引，那就没有用了。另一方面，我们希望匹配索引或
	 * TOAST 表的包含模式得到遵守。因此，如果给出了包含模式，我们希望选择所有与该模式匹配的表、
	 * TOAST 表或索引。但是如果未给出包含模式，而我们只是简单地匹配所有关系，
	 * 那么在此处我们只想匹配主表。
	 */
	if (opts.allrel)
		appendPQExpBuffer(&sql,
						  " AND c.relam = %u "
						  "AND c.relkind IN ("
						  CppAsString2(RELKIND_RELATION) ", "
						  CppAsString2(RELKIND_SEQUENCE) ", "
						  CppAsString2(RELKIND_MATVIEW) ", "
						  CppAsString2(RELKIND_TOASTVALUE) ") "
						  "AND c.relnamespace != %u",
						  HEAP_TABLE_AM_OID, PG_TOAST_NAMESPACE);
	else
		appendPQExpBuffer(&sql,
						  " AND c.relam IN (%u, %u)"
						  "AND c.relkind IN ("
						  CppAsString2(RELKIND_RELATION) ", "
						  CppAsString2(RELKIND_SEQUENCE) ", "
						  CppAsString2(RELKIND_MATVIEW) ", "
						  CppAsString2(RELKIND_TOASTVALUE) ", "
						  CppAsString2(RELKIND_INDEX) ") "
						  "AND ((c.relam = %u AND c.relkind IN ("
						  CppAsString2(RELKIND_RELATION) ", "
						  CppAsString2(RELKIND_SEQUENCE) ", "
						  CppAsString2(RELKIND_MATVIEW) ", "
						  CppAsString2(RELKIND_TOASTVALUE) ")) OR "
						  "(c.relam = %u AND c.relkind = "
						  CppAsString2(RELKIND_INDEX) "))",
						  HEAP_TABLE_AM_OID, BTREE_AM_OID,
						  HEAP_TABLE_AM_OID, BTREE_AM_OID);

	appendPQExpBufferStr(&sql,
						 "\nORDER BY c.oid)");

	if (!opts.no_toast_expansion)
	{
		/*
		 * Include a CTE for toast tables associated with primary heap tables
		 * selected above, filtering by exclusion patterns (if any) that match
		 * toast table names.
		 */
		/*
		 * 包含一个针对与上方选定主堆表关联的 TOAST 表的公共表表达式，
		 * 并通过匹配 TOAST 表名称的排除模式（如有）进行过滤。
		 */
		appendPQExpBufferStr(&sql,
							 ", toast (oid, nspname, relname, relpages) AS ("
							 "\nSELECT t.oid, 'pg_toast', t.relname, t.relpages"
							 "\nFROM pg_catalog.pg_class t "
							 "INNER JOIN relation r "
							 "ON r.reltoastrelid = t.oid");
		if (opts.excludetbl || opts.excludensp)
			appendPQExpBufferStr(&sql,
								 "\nLEFT OUTER JOIN exclude_pat ep"
								 "\nON ('pg_toast' ~ ep.nsp_regex OR ep.nsp_regex IS NULL)"
								 "\nAND (t.relname ~ ep.rel_regex OR ep.rel_regex IS NULL)"
								 "\nAND ep.heap_only"
								 "\nWHERE ep.pattern_id IS NULL"
								 "\nAND t.relpersistence != " CppAsString2(RELPERSISTENCE_TEMP));
		appendPQExpBufferStr(&sql,
							 "\n)");
	}
	if (!opts.no_btree_expansion)
	{
		/*
		 * Include a CTE for btree indexes associated with primary heap tables
		 * selected above, filtering by exclusion patterns (if any) that match
		 * btree index names.
		 */
		/*
		 * 包含一个针对与上方选定主堆表关联的 B 树索引的公共表表达式，
		 * 并通过匹配 B 树索引名称的排除模式（如有）进行过滤。
		 */
		appendPQExpBufferStr(&sql,
							 ", index (oid, nspname, relname, relpages) AS ("
							 "\nSELECT c.oid, r.nspname, c.relname, c.relpages "
							 "FROM relation r"
							 "\nINNER JOIN pg_catalog.pg_index i "
							 "ON r.oid = i.indrelid "
							 "INNER JOIN pg_catalog.pg_class c "
							 "ON i.indexrelid = c.oid "
							 "AND c.relpersistence != " CppAsString2(RELPERSISTENCE_TEMP));
		if (opts.excludeidx || opts.excludensp)
			appendPQExpBufferStr(&sql,
								 "\nINNER JOIN pg_catalog.pg_namespace n "
								 "ON c.relnamespace = n.oid"
								 "\nLEFT OUTER JOIN exclude_pat ep "
								 "ON (n.nspname ~ ep.nsp_regex OR ep.nsp_regex IS NULL) "
								 "AND (c.relname ~ ep.rel_regex OR ep.rel_regex IS NULL) "
								 "AND ep.btree_only"
								 "\nWHERE ep.pattern_id IS NULL");
		else
			appendPQExpBufferStr(&sql,
								 "\nWHERE true");
		appendPQExpBuffer(&sql,
						  " AND c.relam = %u "
						  "AND c.relkind = " CppAsString2(RELKIND_INDEX),
						  BTREE_AM_OID);
		if (opts.no_toast_expansion)
			appendPQExpBuffer(&sql,
							  " AND c.relnamespace != %u",
							  PG_TOAST_NAMESPACE);
		appendPQExpBufferStr(&sql, "\n)");
	}

	if (!opts.no_toast_expansion && !opts.no_btree_expansion)
	{
		/*
		 * Include a CTE for btree indexes associated with toast tables of
		 * primary heap tables selected above, filtering by exclusion patterns
		 * (if any) that match the toast index names.
		 */
		/*
		 * 包含一个针对与上方选定主堆表的 TOAST 表关联的 B 树索引的公共表表达式，
		 * 并通过匹配 TOAST 索引名称的排除模式（如有）进行过滤。
		 */
		appendPQExpBufferStr(&sql,
							 ", toast_index (oid, nspname, relname, relpages) AS ("
							 "\nSELECT c.oid, 'pg_toast', c.relname, c.relpages "
							 "FROM toast t "
							 "INNER JOIN pg_catalog.pg_index i "
							 "ON t.oid = i.indrelid"
							 "\nINNER JOIN pg_catalog.pg_class c "
							 "ON i.indexrelid = c.oid "
							 "AND c.relpersistence != " CppAsString2(RELPERSISTENCE_TEMP));
		if (opts.excludeidx)
			appendPQExpBufferStr(&sql,
								 "\nLEFT OUTER JOIN exclude_pat ep "
								 "ON ('pg_toast' ~ ep.nsp_regex OR ep.nsp_regex IS NULL) "
								 "AND (c.relname ~ ep.rel_regex OR ep.rel_regex IS NULL) "
								 "AND ep.btree_only "
								 "WHERE ep.pattern_id IS NULL");
		else
			appendPQExpBufferStr(&sql,
								 "\nWHERE true");
		appendPQExpBuffer(&sql,
						  " AND c.relam = %u"
						  " AND c.relkind = " CppAsString2(RELKIND_INDEX) ")",
						  BTREE_AM_OID);
	}

	/*
	 * Roll-up distinct rows from CTEs.
	 *
	 * Relations that match more than one pattern may occur more than once in
	 * the list, and indexes and toast for primary relations may also have
	 * matched in their own right, so we rely on UNION to deduplicate the
	 * list.
	 */
	/*
	 * 汇总来自各公共表表达式的非重复行。
	 *
	 * 匹配多个模式的关系在列表中可能会出现多次，且主要关系的索引和 TOAST 也可能因其自身的
	 * 权利而匹配，因此我们依赖 UNION 对列表进行去重。
	 */
	appendPQExpBufferStr(&sql,
						 "\nSELECT pattern_id, is_heap, is_btree, oid, nspname, relname, relpages "
						 "FROM (");
	appendPQExpBufferStr(&sql,
	/* Inclusion patterns that failed to match */
	/* 未能匹配成功的包含模式 */
						 "\nSELECT pattern_id, is_heap, is_btree, "
						 "NULL::OID AS oid, "
						 "NULL::TEXT AS nspname, "
						 "NULL::TEXT AS relname, "
						 "NULL::INTEGER AS relpages"
						 "\nFROM relation "
						 "WHERE pattern_id IS NOT NULL "
						 "UNION"
	/* Primary relations */
						 "\nSELECT NULL::INTEGER AS pattern_id, "
						 "is_heap, is_btree, oid, nspname, relname, relpages "
						 "FROM relation");
	if (!opts.no_toast_expansion)
		appendPQExpBufferStr(&sql,
							 " UNION"
		/* Toast tables for primary relations */
	/* 主要关系的 TOAST 表 */
							 "\nSELECT NULL::INTEGER AS pattern_id, TRUE AS is_heap, "
							 "FALSE AS is_btree, oid, nspname, relname, relpages "
							 "FROM toast");
	if (!opts.no_btree_expansion)
		appendPQExpBufferStr(&sql,
							 " UNION"
		/* Indexes for primary relations */
	/* 主要关系的索引 */
							 "\nSELECT NULL::INTEGER AS pattern_id, FALSE AS is_heap, "
							 "TRUE AS is_btree, oid, nspname, relname, relpages "
							 "FROM index");
	if (!opts.no_toast_expansion && !opts.no_btree_expansion)
		appendPQExpBufferStr(&sql,
							 " UNION"
		/* Indexes for toast relations */
	/* TOAST 关系的索引 */
							 "\nSELECT NULL::INTEGER AS pattern_id, FALSE AS is_heap, "
							 "TRUE AS is_btree, oid, nspname, relname, relpages "
							 "FROM toast_index");
	appendPQExpBufferStr(&sql,
						 "\n) AS combined_records "
						 "ORDER BY relpages DESC NULLS FIRST, oid");

	res = executeQuery(conn, sql.data, opts.echo);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", sql.data);
		disconnectDatabase(conn);
		exit(1);
	}
	termPQExpBuffer(&sql);

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		int			pattern_id = -1;
		bool		is_heap = false;
		bool		is_btree PG_USED_FOR_ASSERTS_ONLY = false;
		Oid			oid = InvalidOid;
		const char *nspname = NULL;
		const char *relname = NULL;
		int			relpages = 0;

		if (!PQgetisnull(res, i, 0))
			pattern_id = atoi(PQgetvalue(res, i, 0));
		if (!PQgetisnull(res, i, 1))
			is_heap = (PQgetvalue(res, i, 1)[0] == 't');
		if (!PQgetisnull(res, i, 2))
			is_btree = (PQgetvalue(res, i, 2)[0] == 't');
		if (!PQgetisnull(res, i, 3))
			oid = atooid(PQgetvalue(res, i, 3));
		if (!PQgetisnull(res, i, 4))
			nspname = PQgetvalue(res, i, 4);
		if (!PQgetisnull(res, i, 5))
			relname = PQgetvalue(res, i, 5);
		if (!PQgetisnull(res, i, 6))
			relpages = atoi(PQgetvalue(res, i, 6));

		if (pattern_id >= 0)
		{
			/*
			 * Current record pertains to an inclusion pattern.  Record that
			 * it matched.
			 */
			/*
			 * 当前记录属于一个包含模式。记录其匹配成功。
			 */

			if (pattern_id >= opts.include.len)
				pg_fatal("internal error: received unexpected relation pattern_id %d",
						 pattern_id);

			opts.include.data[pattern_id].matched = true;
		}
		else
		{
			/* Current record pertains to a relation */
		/* 当前记录属于一个关系 */

			RelationInfo *rel = (RelationInfo *) pg_malloc0(sizeof(RelationInfo));

			Assert(OidIsValid(oid));
			Assert((is_heap && !is_btree) || (is_btree && !is_heap));

			rel->datinfo = dat;
			rel->reloid = oid;
			rel->is_heap = is_heap;
			rel->nspname = pstrdup(nspname);
			rel->relname = pstrdup(relname);
			rel->relpages = relpages;
			rel->blocks_to_check = relpages;
			if (is_heap && (opts.startblock >= 0 || opts.endblock >= 0))
			{
				/*
				 * We apply --startblock and --endblock to heap tables, but
				 * not btree indexes, and for progress purposes we need to
				 * track how many blocks we expect to check.
				 */
				/*
				 * 我们将 --startblock 和 --endblock 应用于堆表，但不应用于 B 树索引，
				 * 且为了进度的目的，我们需要跟踪预计要检查的块数。
				 */
				if (opts.endblock >= 0 && rel->blocks_to_check > opts.endblock)
					rel->blocks_to_check = opts.endblock + 1;
				if (opts.startblock >= 0)
				{
					if (rel->blocks_to_check > opts.startblock)
						rel->blocks_to_check -= opts.startblock;
					else
						rel->blocks_to_check = 0;
				}
			}
			*pagecount += rel->blocks_to_check;

			simple_ptr_list_append(relations, rel);
		}
	}
	PQclear(res);
}
