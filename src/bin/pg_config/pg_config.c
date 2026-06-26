/*-------------------------------------------------------------------------
 *
 * pg_config.c
 *
 * This program reports various pieces of information about the
 * installed version of PostgreSQL.  Packages that interface to
 * PostgreSQL can use it to configure their build.
 * 本程序输出已安装 PostgreSQL 的各类路径与编译选项等信息，供与 PostgreSQL
 * 对接的软件包在构建时查询使用。
 *
 * This is a C implementation of the previous shell script written by
 * Peter Eisentraut <peter_e@gmx.net>, with adjustments made to
 * accommodate the possibility that the installation has been relocated from
 * the place originally configured.
 * 由原先 Peter Eisentraut 的 shell 脚本改写为 C，并考虑安装目录可能相对
 * configure 时前缀发生迁移（relocatable）的情况。
 *
 * author of C translation: Andrew Dunstan	   mailto:andrew@dunslane.net
 * C 版译者：Andrew Dunstan。
 *
 * This code is released under the terms of the PostgreSQL License.
 * 本代码按 PostgreSQL 许可证发布。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * 部分版权 (c) 1996-2025，PostgreSQL 全球开发组。
 *
 * src/bin/pg_config/pg_config.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/config_info.h"

/*
 * 核心流程：解析 argv → 若含 --help/-? 则打印用法并退出；
 * 用 find_my_exec 定位本可执行文件路径，get_configdata 从安装布局读出全部配置项；
 * 无参数则逐行打印「名称 = 值」；否则对每个参数在 info_items 中查找对应项，
 * 经 show_item 只输出该项的 setting；未知选项则报错并提示 --help。
 */

static const char *progname;

/*
 * Table of known information items
 * 命令行开关与 get_configdata 返回项中 name 字段的对应表。
 *
 * Be careful to keep this in sync with the help() display.
 * 修改时需与 help() 中的选项列表保持一致。
 */
typedef struct
{
	const char *switchname;
	const char *configname;
} InfoItem;

static const InfoItem info_items[] = {
	{"--bindir", "BINDIR"},
	{"--docdir", "DOCDIR"},
	{"--htmldir", "HTMLDIR"},
	{"--includedir", "INCLUDEDIR"},
	{"--pkgincludedir", "PKGINCLUDEDIR"},
	{"--includedir-server", "INCLUDEDIR-SERVER"},
	{"--libdir", "LIBDIR"},
	{"--pkglibdir", "PKGLIBDIR"},
	{"--localedir", "LOCALEDIR"},
	{"--mandir", "MANDIR"},
	{"--sharedir", "SHAREDIR"},
	{"--sysconfdir", "SYSCONFDIR"},
	{"--pgxs", "PGXS"},
	{"--configure", "CONFIGURE"},
	{"--cc", "CC"},
	{"--cppflags", "CPPFLAGS"},
	{"--cflags", "CFLAGS"},
	{"--cflags_sl", "CFLAGS_SL"},
	{"--ldflags", "LDFLAGS"},
	{"--ldflags_ex", "LDFLAGS_EX"},
	{"--ldflags_sl", "LDFLAGS_SL"},
	{"--libs", "LIBS"},
	{"--version", "VERSION"},
	{NULL, NULL}
};


/*
 * Print usage and option descriptions (localized via gettext).
 * 打印用法与各 -- 选项说明（经 gettext 本地化）。
 */
static void
help(void)
{
	printf(_("\n%s provides information about the installed version of PostgreSQL.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  --bindir              show location of user executables\n"));
	printf(_("  --docdir              show location of documentation files\n"));
	printf(_("  --htmldir             show location of HTML documentation files\n"));
	printf(_("  --includedir          show location of C header files of the client\n"
			 "                        interfaces\n"));
	printf(_("  --pkgincludedir       show location of other C header files\n"));
	printf(_("  --includedir-server   show location of C header files for the server\n"));
	printf(_("  --libdir              show location of object code libraries\n"));
	printf(_("  --pkglibdir           show location of dynamically loadable modules\n"));
	printf(_("  --localedir           show location of locale support files\n"));
	printf(_("  --mandir              show location of manual pages\n"));
	printf(_("  --sharedir            show location of architecture-independent support files\n"));
	printf(_("  --sysconfdir          show location of system-wide configuration files\n"));
	printf(_("  --pgxs                show location of extension makefile\n"));
	printf(_("  --configure           show options given to \"configure\" script when\n"
			 "                        PostgreSQL was built\n"));
	printf(_("  --cc                  show CC value used when PostgreSQL was built\n"));
	printf(_("  --cppflags            show CPPFLAGS value used when PostgreSQL was built\n"));
	printf(_("  --cflags              show CFLAGS value used when PostgreSQL was built\n"));
	printf(_("  --cflags_sl           show CFLAGS_SL value used when PostgreSQL was built\n"));
	printf(_("  --ldflags             show LDFLAGS value used when PostgreSQL was built\n"));
	printf(_("  --ldflags_ex          show LDFLAGS_EX value used when PostgreSQL was built\n"));
	printf(_("  --ldflags_sl          show LDFLAGS_SL value used when PostgreSQL was built\n"));
	printf(_("  --libs                show LIBS value used when PostgreSQL was built\n"));
	printf(_("  --version             show the PostgreSQL version\n"));
	printf(_("  -?, --help            show this help, then exit\n"));
	printf(_("\nWith no arguments, all known items are shown.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Hint user to run --help after an invalid argument.
 * 在非法参数后提示用户使用 --help。
 */
static void
advice(void)
{
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
}

/*
 * Print the setting string for one config item name, if present in configdata.
 * 在 configdata 中按名称查找并打印该项的 setting（仅值一行，无则静默跳过）。
 */
static void
show_item(const char *configname,
		  ConfigData *configdata,
		  size_t configdata_len)
{
	int			i;

	for (i = 0; i < configdata_len; i++)
	{
		if (strcmp(configname, configdata[i].name) == 0)
			printf("%s\n", configdata[i].setting);
	}
}

/*
 * Entry: locale, progname, handle --help, resolve own path, load config, dispatch.
 * 入口：设置区域与程序名、处理 --help、解析自身路径、载入安装配置并按参数输出。
 */
int
main(int argc, char **argv)
{
	ConfigData *configdata;
	size_t		configdata_len;
	char		my_exec_path[MAXPGPATH];
	int			i;
	int			j;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_config"));

	progname = get_progname(argv[0]);

	/* check for --help */
	/* 优先扫描 --help / -?，立即打印帮助并退出 */
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0)
		{
			help();
			exit(0);
		}
	}

	if (find_my_exec(argv[0], my_exec_path) < 0)
	{
		fprintf(stderr, _("%s: could not find own program executable\n"), progname);
		exit(1);
	}

	configdata = get_configdata(my_exec_path, &configdata_len);
	/* no arguments -> print everything */
	/* 无参数：列出全部配置项「名称 = 值」 */
	if (argc < 2)
	{
		for (i = 0; i < configdata_len; i++)
			printf("%s = %s\n", configdata[i].name, configdata[i].setting);
		exit(0);
	}

	/* otherwise print requested items */
	/* 有参数：按顺序处理每个 argv，匹配 info_items 后输出对应项 */
	for (i = 1; i < argc; i++)
	{
		for (j = 0; info_items[j].switchname != NULL; j++)
		{
			if (strcmp(argv[i], info_items[j].switchname) == 0)
			{
				show_item(info_items[j].configname,
						  configdata, configdata_len);
				break;
			}
		}
		if (info_items[j].switchname == NULL)
		{
			fprintf(stderr, _("%s: invalid argument: %s\n"),
					progname, argv[i]);
			advice();
			exit(1);
		}
	}

	return 0;
}
