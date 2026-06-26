/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *	  Timestamp and Interval typedefs and related macros.
 *	  时间戳（Timestamp）和时间间隔（Interval）的类型定义及相关宏。
 *
 * Note: this file must be includable in both frontend and backend contexts.
 * 注意：此文件必须能够在前端和后端上下文中被包含。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/datatype/timestamp.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * Core Flow Explanation:
 * PostgreSQL stores absolute timestamps as a single 64-bit integer representing
 * the number of microseconds since the Postgres epoch (2000-01-01 00:00:00 UTC).
 * Intervals are stored in a struct with three components: time (microseconds),
 * days, and months, to handle irregularities like leap years and daylight savings.
 *
 * 核心流程说明：
 * PostgreSQL 将绝对时间戳存储为单个 64 位整数，表示自 Postgres 纪元（2000-01-01 00:00:00 UTC）
 * 以来经过的微秒数。
 * 时间间隔（Interval）存储在一个包含三个部分的结构体中：时间（微秒）、天数和月数，
 * 以处理闰年和夏令时等带来的不规则性。
 */

#ifndef DATATYPE_TIMESTAMP_H
#define DATATYPE_TIMESTAMP_H

/*
 * Timestamp represents absolute time.
 * Timestamp 表示绝对时间。
 *
 * Interval represents delta time. Keep track of months (and years), days,
 * and hours/minutes/seconds separately since the elapsed time spanned is
 * unknown until instantiated relative to an absolute time.
 * Interval 表示增量时间（时差）。分别跟踪月（和年）、天以及小时/分钟/秒，
 * 因为在相对于绝对时间实例化之前，跨越的运行时间是未知的。
 *
 * Note that Postgres uses "time interval" to mean a bounded interval,
 * consisting of a beginning and ending time, not a time span - thomas 97/03/20
 * 注意，Postgres 使用 "time interval" 来表示一个有界的区间，
 * 由开始时间和结束时间组成，而不是一个时间跨度 - thomas 97/03/20
 *
 * Timestamps, as well as the h/m/s fields of intervals, are stored as
 * int64 values with units of microseconds.  (Once upon a time they were
 * double values with units of seconds.)
 * 时间戳以及时间间隔的 h/m/s 字段都存储为 int64 值，单位为微秒。
 * （曾几何时，它们是单位为秒的 double 值。）
 *
 * TimeOffset and fsec_t are convenience typedefs for temporary variables.
 * Do not use fsec_t in values stored on-disk.
 * Also, fsec_t is only meant for *fractional* seconds; beware of overflow
 * if the value you need to store could be many seconds.
 * TimeOffset 和 fsec_t 是为了方便临时变量而定义的类型。
 * 不要在大磁盘存储的值中使用 fsec_t。
 * 此外，fsec_t 仅用于“小数”秒；如果需要存储的值可能包含很多秒，请提防溢出。
 */

typedef int64 Timestamp;		/* 64-bit microsecond absolute time 64位微秒绝对时间 */
typedef int64 TimestampTz;		/* absolute time with time zone 带时区的绝对时间 */
typedef int64 TimeOffset;		/* time difference or duration 时间差或持续时间 */
typedef int32 fsec_t;			/* fractional seconds (in microseconds) 小数秒（以微秒为单位） */


/*
 * Storage format for type interval.
 * 类型 interval 的存储格式。
 */
typedef struct
{
	TimeOffset	time;			/* all time units other than days, months and
								 * years */
								/* 除了天、月和年以外的所有时间单位 */
	int32		day;			/* days, after time for alignment */
								/* 天数，放在 time 之后以实现对齐 */
	int32		month;			/* months and years, after time for alignment */
								/* 月数和年数，放在 time 之后以实现对齐 */
} Interval;

/*
 * Data structure representing a broken-down interval.
 * 表示分解后的时间间隔的数据结构。
 *
 * For historical reasons, this is modeled on struct pg_tm for timestamps.
 * Unlike the situation for timestamps, there's no magic interpretation
 * needed for months or years: they're just zero or not.  Note that fields
 * can be negative; however, because of the divisions done while converting
 * from struct Interval, only tm_mday could be INT_MIN.  This is important
 * because we may need to negate the values in some code paths.
 * 由于历史原因，这是模仿时间戳的 struct pg_tm 构建的。
 * 与时间戳的情况不同，对于月或年不需要特殊的解释：它们要么是零，要么不是。
 * 注意字段可以是负数；然而，由于从 struct Interval 转换时进行的除法运算，
 * 只有 tm_mday 可能是 INT_MIN。这很重要，因为在某些代码路径中我们可能需要对这些值取负。
 */
struct pg_itm
{
	int			tm_usec;
	int			tm_sec;
	int			tm_min;
	int64		tm_hour;		/* needs to be wide 需要足够宽 */
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
};

/*
 * Data structure for decoding intervals.  We could just use struct pg_itm,
 * but then the requirement for tm_usec to be 64 bits would propagate to
 * places where it's not really needed.  Also, omitting the fields that
 * aren't used during decoding seems like a good error-prevention measure.
 * 用于解码时间间隔的数据结构。我们可以只使用 struct pg_itm，
 * 但这样一来，对 tm_usec 为 64 位的要求就会传播到并不真正需要它的地方。
 * 此外，省略解码期间不使用的字段似乎是一种很好的错误预防措施。
 */
struct pg_itm_in
{
	int64		tm_usec;		/* needs to be wide 需要足够宽 */
	int			tm_mday;
	int			tm_mon;
	int			tm_year;
};


/* Limits on the "precision" option (typmod) for these data types */
/* 这些数据类型的“精度”选项（typmod）限制 */
#define MAX_TIMESTAMP_PRECISION 6
#define MAX_INTERVAL_PRECISION 6

/*
 *	Round off to MAX_TIMESTAMP_PRECISION decimal places.
 *	Note: this is also used for rounding off intervals.
 *	四舍五入到 MAX_TIMESTAMP_PRECISION 位小数。
 *	注意：这也用于时间间隔的四舍五入。
 */
#define TS_PREC_INV 1000000.0
#define TSROUND(j) (rint(((double) (j)) * TS_PREC_INV) / TS_PREC_INV)


/*
 * Assorted constants for datetime-related calculations
 * 各种用于日期时间相关计算的常量
 */

#define DAYS_PER_YEAR	365.25	/* assumes leap year every four years 假设每四年一个闰年 */
#define MONTHS_PER_YEAR 12
/*
 *	DAYS_PER_MONTH is very imprecise.  The more accurate value is
 *	365.2425/12 = 30.436875, or '30 days 10:29:06'.  Right now we only
 *	return an integral number of days, but someday perhaps we should
 *	also return a 'time' value to be used as well.  ISO 8601 suggests
 *	30 days.
 *	DAYS_PER_MONTH 非常不精确。更准确的值是 365.2425/12 = 30.436875，
 *	即 '30天 10:29:06'。目前我们只返回整数天，但也许有一天我们也应该
 *	返回一个同时使用的 'time' 值。ISO 8601 建议使用 30 天。
 */
#define DAYS_PER_MONTH	30		/* assumes exactly 30 days per month 假设每月正好 30 天 */
#define DAYS_PER_WEEK	7
#define HOURS_PER_DAY	24		/* assume no daylight savings time changes 假设没有夏令时变化 */

/*
 *	This doesn't adjust for uneven daylight savings time intervals or leap
 *	seconds, and it crudely estimates leap years.  A more accurate value
 *	for days per years is 365.2422.
 *	这不会根据不均匀的夏令时间间隔或闰秒进行调整，并且它粗略地估计了闰年。
 *	更准确的一年天数是 365.2422。
 */
#define SECS_PER_YEAR	(36525 * 864)	/* avoid floating-point computation 避免浮点运算 */
#define SECS_PER_DAY	86400
#define SECS_PER_HOUR	3600
#define SECS_PER_MINUTE 60
#define MINS_PER_HOUR	60

#define USECS_PER_DAY	INT64CONST(86400000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)
#define USECS_PER_MINUTE INT64CONST(60000000)
#define USECS_PER_SEC	INT64CONST(1000000)

/*
 * We allow numeric timezone offsets up to 15:59:59 either way from Greenwich.
 * Currently, the record holders for wackiest offsets in actual use are zones
 * Asia/Manila, at -15:56:08 until 1844, and America/Metlakatla, at +15:13:42
 * until 1867.  If we were to reject such values we would fail to dump and
 * restore old timestamptz values with these zone settings.
 * 我们允许格林威治标准时间前后高达 15:59:59 的数字时区偏移。
 * 目前，实际使用的最古怪偏移记录保持者是 Asia/Manila 时区（1844年之前为 -15:56:08）
 * 以及 America/Metlakatla 时区（1867年之前为 +15:13:42）。
 * 如果我们拒绝这些值，我们将无法使用这些时区设置转储和恢复旧的 timestamptz 值。
 */
#define MAX_TZDISP_HOUR		15	/* maximum allowed hour part 允许的最大小时部分 */
#define TZDISP_LIMIT		((MAX_TZDISP_HOUR + 1) * SECS_PER_HOUR)

/*
 * We reserve the minimum and maximum integer values to represent
 * timestamp (or timestamptz) -infinity and +infinity.
 * 我们保留最小和最大整数值来表示时间戳（或 timestamptz）的负无穷大和正无穷大。
 */
#define TIMESTAMP_MINUS_INFINITY	PG_INT64_MIN
#define TIMESTAMP_INFINITY	PG_INT64_MAX

/*
 * Historically these aliases for infinity have been used.
 * 历史上一直使用这些无穷大的别名。
 */
#define DT_NOBEGIN		TIMESTAMP_MINUS_INFINITY
#define DT_NOEND		TIMESTAMP_INFINITY

#define TIMESTAMP_NOBEGIN(j)	\
	do {(j) = DT_NOBEGIN;} while (0)

#define TIMESTAMP_IS_NOBEGIN(j) ((j) == DT_NOBEGIN)

#define TIMESTAMP_NOEND(j)		\
	do {(j) = DT_NOEND;} while (0)

#define TIMESTAMP_IS_NOEND(j)	((j) == DT_NOEND)

#define TIMESTAMP_NOT_FINITE(j) (TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))

/*
 * Infinite intervals are represented by setting all fields to the minimum or
 * maximum integer values.
 * 无穷大的间隔通过将所有字段设置为最小或最大整数值来表示。
 */
#define INTERVAL_NOBEGIN(i)	\
	do {	\
		(i)->time = PG_INT64_MIN;	\
		(i)->day = PG_INT32_MIN;	\
		(i)->month = PG_INT32_MIN;	\
	} while (0)

#define INTERVAL_IS_NOBEGIN(i)	\
	((i)->month == PG_INT32_MIN && (i)->day == PG_INT32_MIN && (i)->time == PG_INT64_MIN)

#define INTERVAL_NOEND(i)	\
	do {	\
		(i)->time = PG_INT64_MAX;	\
		(i)->day = PG_INT32_MAX;	\
		(i)->month = PG_INT32_MAX;	\
	} while (0)

#define INTERVAL_IS_NOEND(i)	\
	((i)->month == PG_INT32_MAX && (i)->day == PG_INT32_MAX && (i)->time == PG_INT64_MAX)

#define INTERVAL_NOT_FINITE(i) (INTERVAL_IS_NOBEGIN(i) || INTERVAL_IS_NOEND(i))

/*
 * Julian date support.
 * 儒略日支持。
 *
 * date2j() and j2date() nominally handle the Julian date range 0..INT_MAX,
 * or 4714-11-24 BC to 5874898-06-03 AD.  In practice, date2j() will work and
 * give correct negative Julian dates for dates before 4714-11-24 BC as well.
 * We rely on it to do so back to 4714-11-01 BC.  Allowing at least one day's
 * slop is necessary so that timestamp rotation doesn't produce dates that
 * would be rejected on input.  For example, '4714-11-24 00:00 GMT BC' is a
 * legal timestamptz value, but in zones east of Greenwich it would print as
 * sometime in the afternoon of 4714-11-23 BC; if we couldn't process such a
 * date we'd have a dump/reload failure.  So the idea is for IS_VALID_JULIAN
 * to accept a slightly wider range of dates than we really support, and
 * then we apply the exact checks in IS_VALID_DATE or IS_VALID_TIMESTAMP,
 * after timezone rotation if any.  To save a few cycles, we can make
 * IS_VALID_JULIAN check only to the month boundary, since its exact cutoffs
 * are not very critical in this scheme.
 * date2j() 和 j2date() 名义上处理 0..INT_MAX 范围内的儒略日，
 * 即公元前 4714-11-24 到公元 5874898-06-03。实际上，date2j() 也可以正常工作，
 * 并为公元前 4714-11-24 之前的日期提供正确的负儒略日。
 * 我们依靠它将其支持回溯到公元前 4714-11-01。允许至少一天的余量是必要的，
 * 这样时间戳轮转就不会产生在输入时被拒绝的日期。例如，'公元前 4714-11-24 00:00 GMT' 
 * 是一个合法的 timestamptz 值，但在格林威治以东的时区，它会显示为公元前 4714-11-23 下午的某个时间；
 * 如果我们不能处理这样的日期，我们就会遇到转储/加载失败。
 * 因此，这个想法是让 IS_VALID_JULIAN 接受比我们实际支持的稍宽的日期范围，
 * 然后在（如果有的话）时区轮转之后应用 IS_VALID_DATE 或 IS_VALID_TIMESTAMP 中的精确检查。
 * 为了节省一些周期，我们可以让 IS_VALID_JULIAN 仅检查到月份边界，
 * 因为在这个方案中，它的精确截断点并不是非常关键。
 *
 * It is correct that JULIAN_MINYEAR is -4713, not -4714; it is defined to
 * allow easy comparison to tm_year values, in which we follow the convention
 * that tm_year <= 0 represents abs(tm_year)+1 BC.
 * JULIAN_MINYEAR 为 -4713 而不是 -4714 是正确的；它的定义是为了方便与 tm_year 值进行比较，
 * 在 tm_year 中我们遵循 tm_year <= 0 表示 公元前 abs(tm_year)+1 的约定。
 */

#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH (11)
#define JULIAN_MINDAY (24)
#define JULIAN_MAXYEAR (5874898)
#define JULIAN_MAXMONTH (6)
#define JULIAN_MAXDAY (3)

#define IS_VALID_JULIAN(y,m,d) \
	(((y) > JULIAN_MINYEAR || \
	  ((y) == JULIAN_MINYEAR && ((m) >= JULIAN_MINMONTH))) && \
	 ((y) < JULIAN_MAXYEAR || \
	  ((y) == JULIAN_MAXYEAR && ((m) < JULIAN_MAXMONTH))))

/* Julian-date equivalents of Day 0 in Unix and Postgres reckoning */
/* Unix 和 Postgres 计数中第 0 天对应的儒略日 */
#define UNIX_EPOCH_JDATE		2440588 /* == date2j(1970, 1, 1) */
#define POSTGRES_EPOCH_JDATE	2451545 /* == date2j(2000, 1, 1) */

/*
 * Range limits for dates and timestamps.
 * 日期和时间戳的范围限制。
 *
 * We have traditionally allowed Julian day zero as a valid datetime value,
 * so that is the lower bound for both dates and timestamps.
 * 传统上，我们允许儒略日零点作为有效的日期时间值，因此这是日期和时间戳的下限。
 *
 * The upper limit for dates is 5874897-12-31, which is a bit less than what
 * the Julian-date code can allow.  For timestamps, the upper limit is
 * 294276-12-31.  The int64 overflow limit would be a few days later; again,
 * leaving some slop avoids worries about corner-case overflow, and provides
 * a simpler user-visible definition.
 * 日期的上限是 5874897-12-31，比儒略日代码允许的上限略低。
 * 对于时间戳，上限是 294276-12-31。int64 溢出限制会在几天后；
 * 同样，保留一些余量可以避免对边界情况溢出的担忧，并提供更简单的用户可见定义。
 */

/* First allowed date, and first disallowed date, in Julian-date form */
/* 儒略日形式中第一个允许的日期和第一个不允许的日期 */
#define DATETIME_MIN_JULIAN (0)
#define DATE_END_JULIAN (2147483494)	/* == date2j(JULIAN_MAXYEAR, 1, 1) */
#define TIMESTAMP_END_JULIAN (109203528)	/* == date2j(294277, 1, 1) */

/* Timestamp limits */
/* 时间戳限制 */
#define MIN_TIMESTAMP	INT64CONST(-211813488000000000)
/* == (DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY */
#define END_TIMESTAMP	INT64CONST(9223371331200000000)
/* == (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY */

/* Range-check a date (given in Postgres, not Julian, numbering) */
/* 范围检查日期（以 Postgres 而非儒略日编号给出） */
#define IS_VALID_DATE(d) \
	((DATETIME_MIN_JULIAN - POSTGRES_EPOCH_JDATE) <= (d) && \
	 (d) < (DATE_END_JULIAN - POSTGRES_EPOCH_JDATE))

/* Range-check a timestamp */
/* 范围检查时间戳 */
#define IS_VALID_TIMESTAMP(t)  (MIN_TIMESTAMP <= (t) && (t) < END_TIMESTAMP)

#endif							/* DATATYPE_TIMESTAMP_H */
