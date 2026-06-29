/*-------------------------------------------------------------------------
 *
 * main.c
 *	  Stub main() routine for the postgres executable.
 *	  postgres 可执行程序的桩 main() 例程。
 *
 * This does some essential startup tasks for any incarnation of postgres
 * (postmaster, standalone backend, standalone bootstrap process, or a
 * separately exec'd child of a postmaster) and then dispatches to the
 * proper FooMain() routine for the incarnation.
 * 这项工作为 postgres 的任何化身（postmaster、独立后端、独立引导进程或 postmaster 的单独 exec
 * 子进程）执行一些基本的启动任务，然后分发到该化身对应的 FooMain() 例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/main/main.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#if defined(WIN32)
#include <crtdbg.h>
#endif

#if defined(__NetBSD__)
#include <sys/param.h>
#endif

#include "bootstrap/bootstrap.h"
#include "common/username.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "tcop/tcopprot.h"
#include "utils/help_config.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ps_status.h"


const char *progname;
static bool reached_main = false;

/* names of special must-be-first options for dispatching to subprograms
 * 用于分发到子程序的特殊“必须排第一”选项的名称 */
static const char *const DispatchOptionNames[] =
{
	[DISPATCH_CHECK] = "check",
	[DISPATCH_BOOT] = "boot",
	[DISPATCH_FORKCHILD] = "forkchild",
	[DISPATCH_DESCRIBE_CONFIG] = "describe-config",
	[DISPATCH_SINGLE] = "single",
	/* DISPATCH_POSTMASTER has no name */
};

StaticAssertDecl(lengthof(DispatchOptionNames) == DISPATCH_POSTMASTER,
				 "array length mismatch");

static void startup_hacks(const char *progname);
static void init_locale(const char *categoryname, int category, const char *locale);
static void help(const char *progname);
static void check_root(const char *progname);


/*
 * Any Postgres server process begins execution here.
 * 任何 Postgres 服务器进程都从这里开始执行。
 */
int
main(int argc, char *argv[])
{
	bool		do_check_root = true;
	DispatchOption dispatch_option = DISPATCH_POSTMASTER;

	reached_main = true;

	/*
	 * If supported on the current platform, set up a handler to be called if
	 * the backend/postmaster crashes with a fatal signal or exception.
	 * 如果当前平台支持，设置一个处理程序，以便在后端/postmaster 因致命信号或异常崩溃时调用。
	 */
#if defined(WIN32)
	pgwin32_install_crashdump_handler();
#endif

	progname = get_progname(argv[0]);

	/*
	 * Platform-specific startup hacks
	 * 平台特定的启动技巧
	 */
	startup_hacks(progname);

	/*
	 * Remember the physical location of the initially given argv[] array for
	 * possible use by ps display.  On some platforms, the argv[] storage must
	 * be overwritten in order to set the process title for ps. In such cases
	 * save_ps_display_args makes and returns a new copy of the argv[] array.
	 * 记住初始提供的 argv[] 数组的物理位置，以便 ps 显示可能使用。
	 * 在某些平台上，为了设置 ps 的进程标题，必须覆盖 argv[] 存储空间。
	 * 在这种情况下，save_ps_display_args 会创建并返回 argv[] 数组的一个新副本。
	 *
	 * save_ps_display_args may also move the environment strings to make
	 * extra room. Therefore this should be done as early as possible during
	 * startup, to avoid entanglements with code that might save a getenv()
	 * result pointer.
	 * save_ps_display_args 也可能移动环境字符串以腾出额外空间。
	 * 因此，这应该在启动期间尽早完成，以避免与可能保存 getenv() 结果指针的代码发生纠缠。
	 */
	argv = save_ps_display_args(argc, argv);

	/*
	 * Fire up essential subsystems: error and memory management
	 * 启动基本子系统：错误和内存管理
	 *
	 * Code after this point is allowed to use elog/ereport, though
	 * localization of messages may not work right away, and messages won't go
	 * anywhere but stderr until GUC settings get loaded.
	 * 此时之后的代码允许使用 elog/ereport，尽管消息的本地化可能不会立即生效，
	 * 且在加载 GUC 设置之前消息只能发送到 stderr。
	 */
	MyProcPid = getpid();
	MemoryContextInit();

	/*
	 * Set reference point for stack-depth checking.  (There's no point in
	 * enabling this before error reporting works.)
	 * 设置栈深度检查的参考点。（在错误报告工作之前启用此项没有意义。）
	 */
	(void) set_stack_base();

	/*
	 * Set up locale information
	 * 设置区域（locale）信息
	 */
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("postgres"));

	/*
	 * In the postmaster, absorb the environment values for LC_COLLATE and
	 * LC_CTYPE.  Individual backends will change these later to settings
	 * taken from pg_database, but the postmaster cannot do that.  If we leave
	 * these set to "C" then message localization might not work well in the
	 * postmaster.
	 * 在 postmaster 中，吸收 LC_COLLATE 和 LC_CTYPE 的环境变量值。
	 * 单个后端稍后会将其更改为从 pg_database 获取的设置，但 postmaster 无法做到这一点。
	 * 如果我们让这些设置保持为“C”，那么消息本地化在 postmaster 中可能无法正常工作。
	 */
	init_locale("LC_COLLATE", LC_COLLATE, "");
	init_locale("LC_CTYPE", LC_CTYPE, "");

	/*
	 * LC_MESSAGES will get set later during GUC option processing, but we set
	 * it here to allow startup error messages to be localized.
	 * LC_MESSAGES 稍后将在 GUC 选项处理期间设置，但我们在这里设置它以允许启动错误消息被本地化。
	 */
#ifdef LC_MESSAGES
	init_locale("LC_MESSAGES", LC_MESSAGES, "");
#endif

	/* We keep these set to "C" always.  See pg_locale.c for explanation.
	 * 我们始终将这些设置为“C”。有关解释，请参见 pg_locale.c。 */
	init_locale("LC_MONETARY", LC_MONETARY, "C");
	init_locale("LC_NUMERIC", LC_NUMERIC, "C");
	init_locale("LC_TIME", LC_TIME, "C");

	/*
	 * Now that we have absorbed as much as we wish to from the locale
	 * environment, remove any LC_ALL setting, so that the environment
	 * variables installed by pg_perm_setlocale have force.
	 * 现在我们已经从区域环境中吸收了足够的内容，移除任何 LC_ALL 设置，
	 * 以便由 pg_perm_setlocale 安装的环境变量具有效力。
	 */
	unsetenv("LC_ALL");

	/*
	 * Catch standard options before doing much else, in particular before we
	 * insist on not being root.
	 * 在执行其他操作之前捕获标准选项，特别是还在坚持不以 root 身份运行之前。
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			fputs(PG_BACKEND_VERSIONSTR, stdout);
			exit(0);
		}

		/*
		 * In addition to the above, we allow "--describe-config" and "-C var"
		 * to be called by root.  This is reasonably safe since these are
		 * read-only activities.  The -C case is important because pg_ctl may
		 * try to invoke it while still holding administrator privileges on
		 * Windows.  Note that while -C can normally be in any argv position,
		 * if you want to bypass the root check you must put it first.  This
		 * reduces the risk that we might misinterpret some other mode's -C
		 * switch as being the postmaster/postgres one.
		 *
		 * 除了上述内容外，我们还允许 root 调用“--describe-config”和“-C var”。
		 * 既然这些是只读活动，这相当安全。-C 的情况很重要，因为 pg_ctl 可能在仍持有 Windows
		 * 管理员权限时尝试调用它。注意，虽然 -C 通常可以在 argv 的任何位置，
		 * 但如果你想规避 root 检查，必须将其放在第一位。
		 * 这降低了我们将其他模式的 -C 开关误认为是 postmaster/postgres 开关的风险。
		 */
		if (strcmp(argv[1], "--describe-config") == 0)
			do_check_root = false;
		else if (argc > 2 && strcmp(argv[1], "-C") == 0)
			do_check_root = false;
	}

	/*
	 * Make sure we are not running as root, unless it's safe for the selected
	 * option.
	 * 确保我们不是以 root 身份运行，除非对于所选选项是安全的。
	 */
	if (do_check_root)
		check_root(progname);

	/*
	 * Dispatch to one of various subprograms depending on first argument.
	 * 根据第一个参数分发到各个子程序。
	 */

	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-')
		dispatch_option = parse_dispatch_option(&argv[1][2]);

	switch (dispatch_option)
	{
		case DISPATCH_CHECK:
			BootstrapModeMain(argc, argv, true);
			break;
		case DISPATCH_BOOT:
			BootstrapModeMain(argc, argv, false);
			break;
		case DISPATCH_FORKCHILD:
#ifdef EXEC_BACKEND
			SubPostmasterMain(argc, argv);
#else
			Assert(false);		/* should never happen */
#endif
			break;
		case DISPATCH_DESCRIBE_CONFIG:
			GucInfoMain();
			break;
		case DISPATCH_SINGLE:
			PostgresSingleUserMain(argc, argv,
								   strdup(get_user_name_or_exit(progname)));
			break;
		case DISPATCH_POSTMASTER:
			PostmasterMain(argc, argv);
			break;
	}

	/* the functions above should not return
	 * 上述函数不应返回 */
	abort();
}

/*
 * Returns the matching DispatchOption value for the given option name.  If no
 * match is found, DISPATCH_POSTMASTER is returned.
 * 返回给定选项名称匹配的 DispatchOption 值。如果未找到匹配项，则返回 DISPATCH_POSTMASTER。
 */
DispatchOption
parse_dispatch_option(const char *name)
{
	for (int i = 0; i < lengthof(DispatchOptionNames); i++)
	{
		/*
		 * Unlike the other dispatch options, "forkchild" takes an argument,
		 * so we just look for the prefix for that one.  For non-EXEC_BACKEND
		 * builds, we never want to return DISPATCH_FORKCHILD, so skip over it
		 * in that case.
		 * 与其他分发选项不同，“forkchild”带有一个参数，因此我们只需查找该选项的前缀。
		 * 对于非 EXEC_BACKEND 构建，我们绝不想返回 DISPATCH_FORKCHILD，因此在那这种情况下跳过它。
		 */
		if (i == DISPATCH_FORKCHILD)
		{
#ifdef EXEC_BACKEND
			if (strncmp(DispatchOptionNames[DISPATCH_FORKCHILD], name,
						strlen(DispatchOptionNames[DISPATCH_FORKCHILD])) == 0)
				return DISPATCH_FORKCHILD;
#endif
			continue;
		}

		if (strcmp(DispatchOptionNames[i], name) == 0)
			return (DispatchOption) i;
	}

	/* no match means this is a postmaster */
	return DISPATCH_POSTMASTER;
}

/*
 * Place platform-specific startup hacks here.  This is the right
 * place to put code that must be executed early in the launch of any new
 * server process.  Note that this code will NOT be executed when a backend
 * or sub-bootstrap process is forked, unless we are in a fork/exec
 * environment (ie EXEC_BACKEND is defined).
 * 在此处放置特定于平台的启动技巧。
 * 这是放置必须在任何新服务器进程启动早期执行的代码的正确位置。
 * 请注意，当派生（fork）后端或子引导进程时，此代码将不会被执行，
 * 除非我们在 fork/exec 环境中（即定义了 EXEC_BACKEND）。
 *
 * XXX The need for code here is proof that the platform in question
 * is too brain-dead to provide a standard C execution environment
 * without help.  Avoid adding more here, if you can.
 * XXX 这里需要代码证明所讨论的平台太笨，如果不加帮助就无法提供标准 C 执行环境。
 * 如果可以，避免在这里添加更多内容。
 */
static void
startup_hacks(const char *progname)
{
	/*
	 * Windows-specific execution environment hacking.
	 * Windows 特定的执行环境技巧。
	 */
#ifdef WIN32
	{
		WSADATA		wsaData;
		int			err;

		/* Make output streams unbuffered by default
		 * 默认情况下使输出流不带缓冲 */
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		/* Prepare Winsock
		 * 准备 Winsock */
		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err != 0)
		{
			write_stderr("%s: WSAStartup failed: %d\n",
						 progname, err);
			exit(1);
		}

		/*
		 * By default abort() only generates a crash-dump in *non* debug
		 * builds. As our Assert() / ExceptionalCondition() uses abort(),
		 * leaving the default in place would make debugging harder.
		 * 默认情况下，abort() 仅在“非”调试构建中生成崩溃转储。
		 * 由于我们的 Assert() / ExceptionalCondition() 使用 abort()，采用默认设置会使调试更加困难。
		 *
		 * MINGW's own C runtime doesn't have _set_abort_behavior(). When
		 * targeting Microsoft's UCRT with mingw, it never links to the debug
		 * version of the library and thus doesn't need the call to
		 * _set_abort_behavior() either.
		 * MINGW 自有的 C 运行时没有 _set_abort_behavior()。
		 * 当使用 mingw 针对 Microsoft 的 UCRT 时，它永远不会链接到库的调试版本，
		 * 因此也不需要调用 _set_abort_behavior()。
		 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)
		_set_abort_behavior(_CALL_REPORTFAULT | _WRITE_ABORT_MSG,
							_CALL_REPORTFAULT | _WRITE_ABORT_MSG);
#endif							/* !defined(__MINGW32__) &&
								 * !defined(__MINGW64__) */

		/*
		 * SEM_FAILCRITICALERRORS causes more errors to be reported to
		 * callers.
		 * SEM_FAILCRITICALERRORS 会导致向调用者报告更多错误。
		 *
		 * We used to also specify SEM_NOGPFAULTERRORBOX, but that prevents
		 * windows crash reporting from working. Which includes registered
		 * just-in-time debuggers, making it unnecessarily hard to debug
		 * problems on windows. Now we try to disable sources of popups
		 * separately below (note that SEM_NOGPFAULTERRORBOX did not actually
		 * prevent all sources of such popups).
		 * 我们以前还指定了 SEM_NOGPFAULTERRORBOX，但那会阻止 Windows 崩溃报告工作。
		 * 这包括注册的即时调试器，这使得在 Windows 上调试问题变得极其困难。
		 * 现在我们尝试在下面分别禁用弹出窗口的来源（注意 SEM_NOGPFAULTERRORBOX 实际上并未阻止此类弹出窗口的所有来源）。
		 */
		SetErrorMode(SEM_FAILCRITICALERRORS);

		/*
		 * Show errors on stderr instead of popup box (note this doesn't
		 * affect errors originating in the C runtime, see below).
		 * 在 stderr 而不是弹出窗口中显示错误（注意这不会影响源自 C 运行时的错误，请参见下文）。
		 */
		_set_error_mode(_OUT_TO_STDERR);

		/*
		 * In DEBUG builds, errors, including assertions, C runtime errors are
		 * reported via _CrtDbgReport. By default such errors are displayed
		 * with a popup (even with NOGPFAULTERRORBOX), preventing forward
		 * progress. Instead report such errors stderr (and the debugger).
		 * This is C runtime specific and thus the above incantations aren't
		 * sufficient to suppress these popups.
		 * 在 DEBUG 构建中，错误（包括断言和 C 运行时错误）是通过 _CrtDbgReport 报告的。
		 * 默认情况下，此类错误会通过弹出窗口显示（即使使用了 NOGPFAULTERRORBOX），从而阻止程序继续。
		 * 相反，应在 stderr（以及调试器）中报告此类错误。
		 * 这是特定于 C 运行时的，因此上述咒语不足以抑制这些弹出窗口。
		 */
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	}
#endif							/* WIN32 */
}


/*
 * Make the initial permanent setting for a locale category.  If that fails,
 * perhaps due to LC_foo=invalid in the environment, use locale C.  If even
 * that fails, perhaps due to out-of-memory, the entire startup fails with it.
 * When this returns, we are guaranteed to have a setting for the given
 * category's environment variable.
 * 为区域（locale）类别进行初始永久设置。如果失败（可能是由于环境中的 LC_foo 无效），则使用区域 C。
 * 如果甚至这也失败（可能是由于内存不足），则整个启动也会随之失败。
 * 当此函数返回时，我们保证已为给定类别的环境变量进行了设置。
 */
static void
init_locale(const char *categoryname, int category, const char *locale)
{
	if (pg_perm_setlocale(category, locale) == NULL &&
		pg_perm_setlocale(category, "C") == NULL)
		elog(FATAL, "could not adopt \"%s\" locale nor C locale for %s",
			 locale, categoryname);
}



/*
 * Help display should match the options accepted by PostmasterMain()
 * and PostgresMain().
 * 帮助显示应与 PostmasterMain() 和 PostgresMain() 接受的选项匹配。
 *
 * XXX On Windows, non-ASCII localizations of these messages only display
 * correctly if the console output code page covers the necessary characters.
 * Messages emitted in write_console() do not exhibit this problem.
 * XXX 在 Windows 上，这些消息的非 ASCII 本地化仅在控制台输出代码页覆盖了必要字符时才能正确显示。
 * write_console() 中发出的消息不会出现此问题。
 */
static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL server.\n\n"), progname);
	printf(_("Usage:\n  %s [OPTION]...\n\n"), progname);
	printf(_("Options:\n"));
	printf(_("  -B NBUFFERS        number of shared buffers\n"));
	printf(_("  -c NAME=VALUE      set run-time parameter\n"));
	printf(_("  -C NAME            print value of run-time parameter, then exit\n"));
	printf(_("  -d 1-5             debugging level\n"));
	printf(_("  -D DATADIR         database directory\n"));
	printf(_("  -e                 use European date input format (DMY)\n"));
	printf(_("  -F                 turn fsync off\n"));
	printf(_("  -h HOSTNAME        host name or IP address to listen on\n"));
	printf(_("  -i                 enable TCP/IP connections (deprecated)\n"));
	printf(_("  -k DIRECTORY       Unix-domain socket location\n"));
#ifdef USE_SSL
	printf(_("  -l                 enable SSL connections\n"));
#endif
	printf(_("  -N MAX-CONNECT     maximum number of allowed connections\n"));
	printf(_("  -p PORT            port number to listen on\n"));
	printf(_("  -s                 show statistics after each query\n"));
	printf(_("  -S WORK-MEM        set amount of memory for sorts (in kB)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  --NAME=VALUE       set run-time parameter\n"));
	printf(_("  --describe-config  describe configuration parameters, then exit\n"));
	printf(_("  -?, --help         show this help, then exit\n"));

	printf(_("\nDeveloper options:\n"));
	printf(_("  -f s|i|o|b|t|n|m|h forbid use of some plan types\n"));
	printf(_("  -O                 allow system table structure changes\n"));
	printf(_("  -P                 disable system indexes\n"));
	printf(_("  -t pa|pl|ex        show timings after each query\n"));
	printf(_("  -T                 send SIGABRT to all backend processes if one dies\n"));
	printf(_("  -W NUM             wait NUM seconds to allow attach from a debugger\n"));

	printf(_("\nOptions for single-user mode:\n"));
	printf(_("  --single           selects single-user mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (defaults to user name)\n"));
	printf(_("  -d 0-5             override debugging level\n"));
	printf(_("  -E                 echo statement before execution\n"));
	printf(_("  -j                 do not use newline as interactive query delimiter\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nOptions for bootstrapping mode:\n"));
	printf(_("  --boot             selects bootstrapping mode (must be first argument)\n"));
	printf(_("  --check            selects check mode (must be first argument)\n"));
	printf(_("  DBNAME             database name (mandatory argument in bootstrapping mode)\n"));
	printf(_("  -r FILENAME        send stdout and stderr to given file\n"));

	printf(_("\nPlease read the documentation for the complete list of run-time\n"
			 "configuration settings and how to set them on the command line or in\n"
			 "the configuration file.\n\n"
			 "Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}



static void
check_root(const char *progname)
{
#ifndef WIN32
	if (geteuid() == 0)
	{
		write_stderr("\"root\" execution of the PostgreSQL server is not permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromise.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}

	/*
	 * Also make sure that real and effective uids are the same. Executing as
	 * a setuid program from a root shell is a security hole, since on many
	 * platforms a nefarious subroutine could setuid back to root if real uid
	 * is root.  (Since nobody actually uses postgres as a setuid program,
	 * trying to actively fix this situation seems more trouble than it's
	 * worth; we'll just expend the effort to check for it.)
	 *
	 * 还要确保实际 UID 和有效 UID 相同。作为来自 root shell 的 setuid 程序执行是一个安全漏洞，
	 * 因为在许多平台上，如果实际 UID 是 root，某个恶意的子程序可以 setuid 返回 root。
	 * （既然实际上没有人将 postgres 用作 setuid 程序，主动尝试修复这种情况似乎比它的价值更麻烦；
	 * 我们只需要花费精力进行检查即可。）
	 */
	if (getuid() != geteuid())
	{
		write_stderr("%s: real and effective user IDs must match\n",
					 progname);
		exit(1);
	}
#else							/* WIN32 */
	if (pgwin32_is_admin())
	{
		write_stderr("Execution of PostgreSQL by a user with administrative permissions is not\n"
					 "permitted.\n"
					 "The server must be started under an unprivileged user ID to prevent\n"
					 "possible system security compromises.  See the documentation for\n"
					 "more information on how to properly start the server.\n");
		exit(1);
	}
#endif							/* WIN32 */
}

/*
 * At least on linux, set_ps_display() breaks /proc/$pid/environ. The
 * sanitizer library uses /proc/$pid/environ to implement getenv() as it wants
 * to work independent of libc. Depending on which sanitizers are enabled,
 * the sanitizer library may not get initialized until after we've called
 * set_ps_display(), preventing the sanitizer from seeing environment-supplied
 * options.
 *
 * 至少在 Linux 上，set_ps_display() 会损坏 /proc/$pid/environ。
 * sanitizer 库使用 /proc/$pid/environ 来实现 getenv()，因为它希望独立于 libc 工作。
 * 根据启用了哪些 sanitizer，sanitizer 库可能直到我们调用 set_ps_display() 之后才初始化，
 * 从而阻止 sanitizer 看到由环境提供的选项。
 *
 * We can work around that by defining __ubsan_default_options, a weak symbol
 * libsanitizer uses to get defaults from the application, and return
 * getenv("UBSAN_OPTIONS"). But only if main already was reached, so that we
 * don't end up relying on a not-yet-working getenv().
 * 我们可以通过定义 __ubsan_default_options 来解决这个问题，这是一个弱符号，
 * libsanitizer 用来从应用程序获取默认值，并返回 getenv("UBSAN_OPTIONS")。
 * 但只有在主程序已经执行到的情况下才行，这样我们就不会最终依赖于一个尚未工作的 getenv()。
 *
 * On the other hand, with different sanitizers enabled, libsanitizer can
 * call this so early that it's not fully initialized itself, resulting in
 * recursion and a core dump within libsanitizer.  To prevent that, ensure
 * that this function is built without any sanitizer callbacks in it.
 * 另一方面，在启用不同的 sanitizer 的情况下，libsanitizer 可能会在它自身尚未完全初始化时就过早调用此函数，
 * 从而导致递归并在 libsanitizer 内发生核心转储。
 * 为了防止这种情况，请确保以此函数在没有任何 sanitizer 回调的情况下构建。
 *
 * As this function won't get called when not running a sanitizer, it doesn't
 * seem necessary to only compile it conditionally.
 * 由于在不运行 sanitizer 时不会调用此函数，因此似乎没有必要仅有条件地编译它。
 */
const char *__ubsan_default_options(void);

#if __has_attribute(disable_sanitizer_instrumentation)
__attribute__((disable_sanitizer_instrumentation))
#endif
const char *
__ubsan_default_options(void)
{
	/* don't call libc before it's guaranteed to be initialized
	 * 在保证 libc 被初始化之前不要调用它 */
	if (!reached_main)
		return "";

	return getenv("UBSAN_OPTIONS");
}
