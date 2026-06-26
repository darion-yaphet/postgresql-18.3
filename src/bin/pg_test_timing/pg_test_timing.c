/*
 *	pg_test_timing.c
 *		tests overhead of timing calls and their monotonicity:	that
 *		they always move forward
 *
 *	pg_test_timing.c
 *		测试计时调用的开销及其单调性：确保它们总是向前移动
 */

#include "postgres_fe.h"

#include <limits.h>

#include "getopt_long.h"
#include "portability/instr_time.h"

static const char *progname;

static unsigned int test_duration = 3;

static void handle_args(int argc, char *argv[]);
static uint64 test_timing(unsigned int duration);
static void output(uint64 loop_count);

/* record duration in powers of 2 microseconds */
/* 以 2 的幂次方微秒记录持续时间 */
static long long int histogram[32];

/*
 * main --- 主函数入口点
 */
int
main(int argc, char *argv[])
{
	uint64		loop_count;

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_test_timing"));
	progname = get_progname(argv[0]);

	handle_args(argc, argv);

	loop_count = test_timing(test_duration);

	output(loop_count);

	return 0;
}

/*
 * handle_args --- 解析命令行参数
 */
static void
handle_args(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"duration", required_argument, NULL, 'd'},
		{NULL, 0, NULL, 0}
	};

	int			option;			/* Command line option */
	int			optindex = 0;	/* used by getopt_long */
	unsigned long optval;		/* used for option parsing */
	char	   *endptr;

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			printf(_("Usage: %s [-d DURATION]\n"), progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_test_timing (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((option = getopt_long(argc, argv, "d:",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'd':
				errno = 0;
				optval = strtoul(optarg, &endptr, 10);

				if (endptr == optarg || *endptr != '\0' ||
					errno != 0 || optval != (unsigned int) optval)
				{
					fprintf(stderr, _("%s: invalid argument for option %s\n"),
							progname, "--duration");
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}

				test_duration = (unsigned int) optval;
				if (test_duration == 0)
				{
					fprintf(stderr, _("%s: %s must be in range %u..%u\n"),
							progname, "--duration", 1, UINT_MAX);
					exit(1);
				}
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
				break;
		}
	}

	if (argc > optind)
	{
		fprintf(stderr,
				_("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}


	printf(ngettext("Testing timing overhead for %u second.\n",
					"Testing timing overhead for %u seconds.\n",
					test_duration),
		   test_duration);
}

/*
 * test_timing --- 测试时钟源单调性与延迟开销的主测试循环
 */
static uint64
test_timing(unsigned int duration)
{
	uint64		total_time;
	int64		time_elapsed = 0;
	uint64		loop_count = 0;
	uint64		prev,
				cur;
	instr_time	start_time,
				end_time,
				temp;

	total_time = duration > 0 ? duration * INT64CONST(1000000) : 0;

	INSTR_TIME_SET_CURRENT(start_time);
	cur = INSTR_TIME_GET_MICROSEC(start_time);

	while (time_elapsed < total_time)
	{
		int32		diff,
					bits = 0;

		prev = cur;
		INSTR_TIME_SET_CURRENT(temp);
		cur = INSTR_TIME_GET_MICROSEC(temp);
		diff = cur - prev;

		/* Did time go backwards? */
		/* 时间是否倒退了？ */
		if (diff < 0)
		{
			fprintf(stderr, _("Detected clock going backwards in time.\n"));
			fprintf(stderr, _("Time warp: %d ms\n"), diff);
			exit(1);
		}

		/* What is the highest bit in the time diff? */
		/* 时间差值中的最高有效位是什么？ */
		while (diff)
		{
			diff >>= 1;
			bits++;
		}

		/* Update appropriate duration bucket */
		/* 更新相应的持续时间存储桶（直方图） */
		histogram[bits]++;

		loop_count++;
		INSTR_TIME_SUBTRACT(temp, start_time);
		time_elapsed = INSTR_TIME_GET_MICROSEC(temp);
	}

	INSTR_TIME_SET_CURRENT(end_time);

	INSTR_TIME_SUBTRACT(end_time, start_time);

	printf(_("Per loop time including overhead: %0.2f ns\n"),
		   INSTR_TIME_GET_DOUBLE(end_time) * 1e9 / loop_count);

	return loop_count;
}

/*
 * output --- 输出测试后的延迟直方图统计表格
 */
static void
output(uint64 loop_count)
{
	int64		max_bit = 31,
				i;
	char	   *header1 = _("< us");
	char	   *header2 = /* xgettext:no-c-format */ _("% of total");
	char	   *header3 = _("count");
	int			len1 = strlen(header1);
	int			len2 = strlen(header2);
	int			len3 = strlen(header3);

	/* find highest bit value */
	/* 寻找最高有效位 */
	while (max_bit > 0 && histogram[max_bit] == 0)
		max_bit--;

	printf(_("Histogram of timing durations:\n"));
	printf("%*s   %*s %*s\n",
		   Max(6, len1), header1,
		   Max(10, len2), header2,
		   Max(10, len3), header3);

	for (i = 0; i <= max_bit; i++)
		printf("%*ld    %*.5f %*lld\n",
			   Max(6, len1), 1l << i,
			   Max(10, len2) - 1, (double) histogram[i] * 100 / loop_count,
			   Max(10, len3), histogram[i]);
}
