/*-------------------------------------------------------------------------
 *
 * isn.c
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * Author:	German Mendez Bravo (Kronuz)
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/isn/isn.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "EAN13.h"
#include "ISBN.h"
#include "ISMN.h"
#include "ISSN.h"
#include "UPC.h"
#include "fmgr.h"
#include "isn.h"
#include "utils/guc.h"

PG_MODULE_MAGIC_EXT(
					.name = "isn",
					.version = PG_VERSION
);

#ifdef USE_ASSERT_CHECKING
#define ISN_DEBUG 1
#else
#define ISN_DEBUG 0
#endif

#define MAXEAN13LEN 18

enum isn_type
{
	INVALID, ANY, EAN13, ISBN, ISMN, ISSN, UPC
};

static const char *const isn_names[] = {"EAN13/UPC/ISxN", "EAN13/UPC/ISxN", "EAN13", "ISBN", "ISMN", "ISSN", "UPC"};

/* GUC value
 *
 * GUC值
 */
static bool g_weak = false;


/***********************************************************************
 **
 **		Routines for EAN13/UPC/ISxNs.
 *
 * * EAN13/UPC/ISxN 例程。
 **
 ** Note:
 **  In this code, a normalized string is one that is known to be a valid
 **  ISxN number containing only digits and hyphens and with enough space
 **  to hold the full 13 digits plus the maximum of four hyphens.
 *
 * * 注意： * 在此代码中，规范化字符串是已知有效的 ISxN 数字，仅包含数字和连字符，并且有足够的空间 * 容纳完整的 13 个数字加上最多四个连字符。
 ***********************************************************************/

/*----------------------------------------------------------
 * Debugging routines.
 *
 * 调试例程。
 *---------------------------------------------------------*/

/*
 * Check if the table and its index is correct (just for debugging)
 *
 * 检查表及其索引是否正确（仅用于调试）
 */
pg_attribute_unused()
static bool
check_table(const char *(*TABLE)[2], const unsigned TABLE_index[10][2])
{
	const char *aux1,
			   *aux2;
	int			a,
				b,
				x = 0,
				y = -1,
				i = 0,
				j,
				init = 0;

	if (TABLE == NULL || TABLE_index == NULL)
		return true;

	while (TABLE[i][0] && TABLE[i][1])
	{
		aux1 = TABLE[i][0];
		aux2 = TABLE[i][1];

		/* must always start with a digit:
		 *
		 * 必须始终以数字开头：
		 */
		if (!isdigit((unsigned char) *aux1) || !isdigit((unsigned char) *aux2))
			goto invalidtable;
		a = *aux1 - '0';
		b = *aux2 - '0';

		/* must always have the same format and length:
		 *
		 * 必须始终具有相同的格式和长度：
		 */
		while (*aux1 && *aux2)
		{
			if (!(isdigit((unsigned char) *aux1) &&
				  isdigit((unsigned char) *aux2)) &&
				(*aux1 != *aux2 || *aux1 != '-'))
				goto invalidtable;
			aux1++;
			aux2++;
		}
		if (*aux1 != *aux2)
			goto invalidtable;

		/* found a new range
		 *
		 * 找到了一个新的范围
		 */
		if (a > y)
		{
			/* check current range in the index:
			 *
			 * 检查索引中的当前范围：
			 */
			for (j = x; j <= y; j++)
			{
				if (TABLE_index[j][0] != init)
					goto invalidindex;
				if (TABLE_index[j][1] != i - init)
					goto invalidindex;
			}
			init = i;
			x = a;
		}

		/* Always get the new limit
		 *
		 * 总是得到新的限制
		 */
		y = b;
		if (y < x)
			goto invalidtable;
		i++;
	}

	return true;

invalidtable:
	elog(DEBUG1, "invalid table near {\"%s\", \"%s\"} (pos: %d)",
		 TABLE[i][0], TABLE[i][1], i);
	return false;

invalidindex:
	elog(DEBUG1, "index %d is invalid", j);
	return false;
}

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *
 * 格式化和转换例程。
 *---------------------------------------------------------*/

static unsigned
dehyphenate(char *bufO, char *bufI)
{
	unsigned	ret = 0;

	while (*bufI)
	{
		if (isdigit((unsigned char) *bufI))
		{
			*bufO++ = *bufI;
			ret++;
		}
		bufI++;
	}
	*bufO = '\0';
	return ret;
}

/*
 * hyphenate --- Try to hyphenate, in-place, the string starting at bufI
 *				  into bufO using the given hyphenation range TABLE.
 *				  Assumes the input string to be used is of only digits.
 *
 * hyphenate --- 尝试使用给定的连字符范围 TABLE 将从 bufI 开始的字符串就地连接到 bufO 中。假设要使用的输入字符串仅包含数字。
 *
 * Returns the number of characters actually hyphenated.
 *
 * 返回实际连字的字符数。
 */
static unsigned
hyphenate(char *bufO, char *bufI, const char *(*TABLE)[2], const unsigned TABLE_index[10][2])
{
	unsigned	ret = 0;
	const char *ean_aux1,
			   *ean_aux2,
			   *ean_p;
	char	   *firstdig,
			   *aux1,
			   *aux2;
	unsigned	search,
				upper,
				lower,
				step;
	bool		ean_in1,
				ean_in2;

	/* just compress the string if no further hyphenation is required
	 *
	 * 如果不需要进一步连字符，只需压缩字符串
	 */
	if (TABLE == NULL || TABLE_index == NULL)
	{
		while (*bufI)
		{
			*bufO++ = *bufI++;
			ret++;
		}
		*bufO = '\0';
		return (ret + 1);
	}

	/* add remaining hyphenations
	 *
	 * 添加剩余的连字符
	 */

	search = *bufI - '0';
	upper = lower = TABLE_index[search][0];
	upper += TABLE_index[search][1];
	lower--;

	step = (upper - lower) / 2;
	if (step == 0)
		return 0;
	search = lower + step;

	firstdig = bufI;
	ean_in1 = ean_in2 = false;
	ean_aux1 = TABLE[search][0];
	ean_aux2 = TABLE[search][1];
	do
	{
		if ((ean_in1 || *firstdig >= *ean_aux1) && (ean_in2 || *firstdig <= *ean_aux2))
		{
			if (*firstdig > *ean_aux1)
				ean_in1 = true;
			if (*firstdig < *ean_aux2)
				ean_in2 = true;
			if (ean_in1 && ean_in2)
				break;

			firstdig++, ean_aux1++, ean_aux2++;
			if (!(*ean_aux1 && *ean_aux2 && *firstdig))
				break;
			if (!isdigit((unsigned char) *ean_aux1))
				ean_aux1++, ean_aux2++;
		}
		else
		{
			/*
			 * check in what direction we should go and move the pointer
			 * accordingly
			 *
			 * 检查我们应该朝哪个方向移动并相应地移动指针
			 */
			if (*firstdig < *ean_aux1 && !ean_in1)
				upper = search;
			else
				lower = search;

			step = (upper - lower) / 2;
			search = lower + step;

			/* Initialize stuff again:
			 *
			 * 再次初始化东西：
			 */
			firstdig = bufI;
			ean_in1 = ean_in2 = false;
			ean_aux1 = TABLE[search][0];
			ean_aux2 = TABLE[search][1];
		}
	} while (step);

	if (step)
	{
		aux1 = bufO;
		aux2 = bufI;
		ean_p = TABLE[search][0];
		while (*ean_p && *aux2)
		{
			if (*ean_p++ != '-')
				*aux1++ = *aux2++;
			else
				*aux1++ = '-';
			ret++;
		}
		*aux1++ = '-';
		*aux1 = *aux2;			/* add a lookahead char */
		return (ret + 1);
	}
	return ret;
}

/*
 * weight_checkdig -- Receives a buffer with a normalized ISxN string number,
 *					   and the length to weight.
 *
 * Weight_checkdig —— 接收一个带有标准化 ISxN 字符串编号和重量长度的缓冲区。
 *
 * Returns the weight of the number (the check digit value, 0-10)
 *
 * 返回数字的权重（校验位值，0-10）
 */
static unsigned
weight_checkdig(char *isn, unsigned size)
{
	unsigned	weight = 0;

	while (*isn && size > 1)
	{
		if (isdigit((unsigned char) *isn))
		{
			weight += size-- * (*isn - '0');
		}
		isn++;
	}
	weight = weight % 11;
	if (weight != 0)
		weight = 11 - weight;
	return weight;
}


/*
 * checkdig --- Receives a buffer with a normalized ISxN string number,
 *				 and the length to check.
 *
 * checkdig --- 接收带有标准化 ISxN 字符串编号和要检查的长度的缓冲区。
 *
 * Returns the check digit value (0-9)
 *
 * 返回校验位值 (0-9)
 */
static unsigned
checkdig(char *num, unsigned size)
{
	unsigned	check = 0,
				check3 = 0;
	unsigned	pos = 0;

	if (*num == 'M')
	{							/* ISMN start with 'M' */
		check3 = 3;
		pos = 1;
	}
	while (*num && size > 1)
	{
		if (isdigit((unsigned char) *num))
		{
			if (pos++ % 2)
				check3 += *num - '0';
			else
				check += *num - '0';
			size--;
		}
		num++;
	}
	check = (check + 3 * check3) % 10;
	if (check != 0)
		check = 10 - check;
	return check;
}

/*
 * ean2isn --- Try to convert an ean13 number to a UPC/ISxN number.
 *			   This doesn't verify for a valid check digit.
 *
 * ean2isn --- 尝试将 ean13 数字转换为 UPC/ISxN 数字。这不会验证有效的校验位。
 *
 * If errorOK is false, ereport a useful error message if the ean13 is bad.
 * If errorOK is true, just return "false" for bad input.
 *
 * 如果 errorOK 为 false，则在 ean13 损坏时报告有用的错误消息。如果 errorOK 为 true，则对于错误输入仅返回“false”。
 */
static bool
ean2isn(ean13 ean, bool errorOK, ean13 *result, enum isn_type accept)
{
	enum isn_type type = INVALID;

	char		buf[MAXEAN13LEN + 1];
	char	   *aux;
	unsigned	digval;
	unsigned	search;
	ean13		ret = ean;

	ean >>= 1;
	/* verify it's in the EAN13 range
	 *
	 * 验证它是否在 EAN13 范围内
	 */
	if (ean > UINT64CONST(9999999999999))
		goto eantoobig;

	/* convert the number
	 *
	 * 转换数字
	 */
	search = 0;
	aux = buf + 13;
	*aux = '\0';				/* terminate string; aux points to last digit */
	do
	{
		digval = (unsigned) (ean % 10); /* get the decimal value */
		ean /= 10;				/* get next digit */
		*--aux = (char) (digval + '0'); /* convert to ascii and store */
	} while (ean && search++ < 12);
	while (search++ < 12)
		*--aux = '0';			/* fill the remaining EAN13 with '0' */

	/* find out the data type:
	 *
	 * 找出数据类型：
	 */
	if (strncmp("978", buf, 3) == 0)
	{							/* ISBN */
		type = ISBN;
	}
	else if (strncmp("977", buf, 3) == 0)
	{							/* ISSN */
		type = ISSN;
	}
	else if (strncmp("9790", buf, 4) == 0)
	{							/* ISMN */
		type = ISMN;
	}
	else if (strncmp("979", buf, 3) == 0)
	{							/* ISBN-13 */
		type = ISBN;
	}
	else if (*buf == '0')
	{							/* UPC */
		type = UPC;
	}
	else
	{
		type = EAN13;
	}
	if (accept != ANY && accept != EAN13 && accept != type)
		goto eanwrongtype;

	*result = ret;
	return true;

eanwrongtype:
	if (!errorOK)
	{
		if (type != EAN13)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("cannot cast EAN13(%s) to %s for number: \"%s\"",
							isn_names[type], isn_names[accept], buf)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("cannot cast %s to %s for number: \"%s\"",
							isn_names[type], isn_names[accept], buf)));
		}
	}
	return false;

eantoobig:
	if (!errorOK)
	{
		char		eanbuf[64];

		/*
		 * Format the number separately to keep the machine-dependent format
		 * code out of the translatable message text
		 *
		 * 单独格式化数字，以将与机器相关的格式代码排除在可翻译的消息文本之外
		 */
		snprintf(eanbuf, sizeof(eanbuf), EAN13_FORMAT, ean);
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for %s type",
						eanbuf, isn_names[type])));
	}
	return false;
}

/*
 * ean2UPC/ISxN --- Convert in-place a normalized EAN13 string to the corresponding
 *					UPC/ISxN string number. Assumes the input string is normalized.
 *
 * ean2UPC/ISxN --- 将规范化的 EAN13 字符串就地转换为相应的 UPC/ISxN 字符串编号。假设输入字符串已标准化。
 */
static inline void
ean2ISBN(char *isn)
{
	char	   *aux;
	unsigned	check;

	/*
	 * The number should come in this format: 978-0-000-00000-0 or may be an
	 * ISBN-13 number, 979-..., which does not have a short representation. Do
	 * the short output version if possible.
	 *
	 * 该号码应采用以下格式：978-0-000-00000-0，或者可以是 ISBN-13 号码，979-...，它没有简短的表示形式。如果可能的话，做短输出版本。
	 */
	if (strncmp("978-", isn, 4) == 0)
	{
		/* Strip the first part and calculate the new check digit
		 *
		 * 剥离第一部分并计算新的校验位
		 */
		hyphenate(isn, isn + 4, NULL, NULL);
		check = weight_checkdig(isn, 10);
		aux = strchr(isn, '\0');
		while (!isdigit((unsigned char) *--aux));
		if (check == 10)
			*aux = 'X';
		else
			*aux = check + '0';
	}
}

static inline void
ean2ISMN(char *isn)
{
	/* the number should come in this format: 979-0-000-00000-0
	 *
	 * 该号码应采用以下格式：979-0-000-00000-0
	 */
	/* Just strip the first part and change the first digit ('0') to 'M'
	 *
	 * 只需删除第一部分并将第一个数字（“0”）更改为“M”
	 */
	hyphenate(isn, isn + 4, NULL, NULL);
	isn[0] = 'M';
}

static inline void
ean2ISSN(char *isn)
{
	unsigned	check;

	/* the number should come in this format: 977-0000-000-00-0
	 *
	 * 该号码应采用以下格式：977-0000-000-00-0
	 */
	/* Strip the first part, crop, and calculate the new check digit
	 *
	 * 剥离第一部分，裁剪并计算新的校验位
	 */
	hyphenate(isn, isn + 4, NULL, NULL);
	check = weight_checkdig(isn, 8);
	if (check == 10)
		isn[8] = 'X';
	else
		isn[8] = check + '0';
	isn[9] = '\0';
}

static inline void
ean2UPC(char *isn)
{
	/* the number should come in this format: 000-000000000-0
	 *
	 * 该号码应采用以下格式：000-000000000-0
	 */
	/* Strip the first part, crop, and dehyphenate
	 *
	 * 剥离第一部分、裁剪并去掉连字符
	 */
	dehyphenate(isn, isn + 1);
	isn[12] = '\0';
}

/*
 * ean2* --- Converts a string of digits into an ean13 number.
 *			  Assumes the input string is a string with only digits
 *			  on it, and that it's within the range of ean13.
 *
 * ean2* --- 将数字字符串转换为 ean13 数字。假设输入字符串是一个只有数字的字符串，并且它在 ean13 的范围内。
 *
 * Returns the ean13 value of the string.
 *
 * 返回字符串的 ean13 值。
 */
static ean13
str2ean(const char *num)
{
	ean13		ean = 0;		/* current ean */

	while (*num)
	{
		if (isdigit((unsigned char) *num))
			ean = 10 * ean + (*num - '0');
		num++;
	}
	return (ean << 1);			/* also give room to a flag */
}

/*
 * ean2string --- Try to convert an ean13 number to a hyphenated string.
 *				  Assumes there's enough space in result to hold
 *				  the string (maximum MAXEAN13LEN+1 bytes)
 *				  This doesn't verify for a valid check digit.
 *
 * ean2string --- 尝试将 ean13 数字转换为带连字符的字符串。假设结果中有足够的空间来保存字符串（最大 MAXEAN13LEN+1 字节），这不会验证有效的校验位。
 *
 * If shortType is true, the returned string is in the old ISxN short format.
 * If errorOK is false, ereport a useful error message if the string is bad.
 * If errorOK is true, just return "false" for bad input.
 *
 * 如果shortType 为true，则返回的字符串采用旧的ISxN 短格式。如果 errorOK 为 false，则在字符串错误时报告有用的错误消息。如果 errorOK 为 true，则对于错误输入仅返回“false”。
 */
static bool
ean2string(ean13 ean, bool errorOK, char *result, bool shortType)
{
	const char *(*TABLE)[2];
	const unsigned (*TABLE_index)[2];
	enum isn_type type = INVALID;

	char	   *aux;
	unsigned	digval;
	unsigned	search;
	char		valid = '\0';	/* was the number initially written with a
								 * valid check digit? */

	TABLE_index = ISBN_index;

	if ((ean & 1) != 0)
		valid = '!';
	ean >>= 1;
	/* verify it's in the EAN13 range
	 *
	 * 验证它是否在 EAN13 范围内
	 */
	if (ean > UINT64CONST(9999999999999))
		goto eantoobig;

	/* convert the number
	 *
	 * 转换数字
	 */
	search = 0;
	aux = result + MAXEAN13LEN;
	*aux = '\0';				/* terminate string; aux points to last digit */
	*--aux = valid;				/* append '!' for numbers with invalid but
								 * corrected check digit */
	do
	{
		digval = (unsigned) (ean % 10); /* get the decimal value */
		ean /= 10;				/* get next digit */
		*--aux = (char) (digval + '0'); /* convert to ascii and store */
		if (search == 0)
			*--aux = '-';		/* the check digit is always there */
	} while (ean && search++ < 13);
	while (search++ < 13)
		*--aux = '0';			/* fill the remaining EAN13 with '0' */

	/* The string should be in this form: ???DDDDDDDDDDDD-D"
	 *
	 * 该字符串应采用以下形式：???DDDDDDDDDDDD-D”
	 */
	search = hyphenate(result, result + 3, EAN13_range, EAN13_index);

	/* verify it's a logically valid EAN13
	 *
	 * 验证它是逻辑上有效的 EAN13
	 */
	if (search == 0)
	{
		search = hyphenate(result, result + 3, NULL, NULL);
		goto okay;
	}

	/* find out what type of hyphenation is needed:
	 *
	 * 找出需要什么类型的连字符：
	 */
	if (strncmp("978-", result, search) == 0)
	{							/* ISBN -13 978-range */
		/* The string should be in this form: 978-??000000000-0"
		 *
		 * 该字符串应采用以下形式：978-??000000000-0"
		 */
		type = ISBN;
		TABLE = ISBN_range;
		TABLE_index = ISBN_index;
	}
	else if (strncmp("977-", result, search) == 0)
	{							/* ISSN */
		/* The string should be in this form: 977-??000000000-0"
		 *
		 * 该字符串应采用以下形式：977-??000000000-0"
		 */
		type = ISSN;
		TABLE = ISSN_range;
		TABLE_index = ISSN_index;
	}
	else if (strncmp("979-0", result, search + 1) == 0)
	{							/* ISMN */
		/* The string should be in this form: 979-0?000000000-0"
		 *
		 * 该字符串应采用以下形式：979-0?000000000-0"
		 */
		type = ISMN;
		TABLE = ISMN_range;
		TABLE_index = ISMN_index;
	}
	else if (strncmp("979-", result, search) == 0)
	{							/* ISBN-13 979-range */
		/* The string should be in this form: 979-??000000000-0"
		 *
		 * 该字符串应采用以下形式：979-??000000000-0"
		 */
		type = ISBN;
		TABLE = ISBN_range_new;
		TABLE_index = ISBN_index_new;
	}
	else if (*result == '0')
	{							/* UPC */
		/* The string should be in this form: 000-00000000000-0"
		 *
		 * 该字符串应采用以下形式：000-00000000000-0”
		 */
		type = UPC;
		TABLE = UPC_range;
		TABLE_index = UPC_index;
	}
	else
	{
		type = EAN13;
		TABLE = NULL;
		TABLE_index = NULL;
	}

	/* verify it's a logically valid EAN13/UPC/ISxN
	 *
	 * 验证它是逻辑上有效的 EAN13/UPC/ISxN
	 */
	digval = search;
	search = hyphenate(result + digval, result + digval + 2, TABLE, TABLE_index);

	/* verify it's a valid EAN13
	 *
	 * 验证它是有效的 EAN13
	 */
	if (search == 0)
	{
		search = hyphenate(result + digval, result + digval + 2, NULL, NULL);
		goto okay;
	}

okay:
	/* convert to the old short type:
	 *
	 * 转换为旧的短类型：
	 */
	if (shortType)
		switch (type)
		{
			case ISBN:
				ean2ISBN(result);
				break;
			case ISMN:
				ean2ISMN(result);
				break;
			case ISSN:
				ean2ISSN(result);
				break;
			case UPC:
				ean2UPC(result);
				break;
			default:
				break;
		}
	return true;

eantoobig:
	if (!errorOK)
	{
		char		eanbuf[64];

		/*
		 * Format the number separately to keep the machine-dependent format
		 * code out of the translatable message text
		 *
		 * 单独格式化数字，以将与机器相关的格式代码排除在可翻译的消息文本之外
		 */
		snprintf(eanbuf, sizeof(eanbuf), EAN13_FORMAT, ean);
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value \"%s\" is out of range for %s type",
						eanbuf, isn_names[type])));
	}
	return false;
}

/*
 * string2ean --- try to parse a string into an ean13.
 *
 * string2ean --- 尝试将字符串解析为 ean13。
 *
 * ereturn false with a useful error message if the string is bad.
 * Otherwise return true.
 *
 * 如果字符串错误，则返回 false 并显示有用的错误消息。否则返回真。
 *
 * if the input string ends with '!' it will always be treated as invalid
 * (even if the check digit is valid)
 *
 * 如果输入字符串以 '!' 结尾它将始终被视为无效（即使校验位有效）
 */
static bool
string2ean(const char *str, struct Node *escontext, ean13 *result,
		   enum isn_type accept)
{
	bool		digit,
				last;
	char		buf[17] = "                ";
	char	   *aux1 = buf + 3; /* leave space for the first part, in case
								 * it's needed */
	const char *aux2 = str;
	enum isn_type type = INVALID;
	unsigned	check = 0,
				rcheck = (unsigned) -1;
	unsigned	length = 0;
	bool		magic = false,
				valid = true;

	/* recognize and validate the number:
	 *
	 * 识别并验证号码：
	 */
	while (*aux2 && length <= 13)
	{
		last = (*(aux2 + 1) == '!' || *(aux2 + 1) == '\0'); /* is the last character */
		digit = (isdigit((unsigned char) *aux2) != 0);	/* is current character
														 * a digit? */
		if (*aux2 == '?' && last)	/* automagically calculate check digit if
									 * it's '?' */
			magic = digit = true;
		if (length == 0 && (*aux2 == 'M' || *aux2 == 'm'))
		{
			/* only ISMN can be here
			 *
			 * 只有 ISMN 可以在这里
			 */
			if (type != INVALID)
				goto eaninvalid;
			type = ISMN;
			*aux1++ = 'M';
			length++;
		}
		else if (length == 7 && (digit || *aux2 == 'X' || *aux2 == 'x') && last)
		{
			/* only ISSN can be here
			 *
			 * 这里只能有 ISSN
			 */
			if (type != INVALID)
				goto eaninvalid;
			type = ISSN;
			*aux1++ = toupper((unsigned char) *aux2);
			length++;
		}
		else if (length == 9 && (digit || *aux2 == 'X' || *aux2 == 'x') && last)
		{
			/* only ISBN and ISMN can be here
			 *
			 * 此处只能显示 ISBN 和 ISMN
			 */
			if (type != INVALID && type != ISMN)
				goto eaninvalid;
			if (type == INVALID)
				type = ISBN;	/* ISMN must start with 'M' */
			*aux1++ = toupper((unsigned char) *aux2);
			length++;
		}
		else if (length == 11 && digit && last)
		{
			/* only UPC can be here
			 *
			 * 这里只能有UPC
			 */
			if (type != INVALID)
				goto eaninvalid;
			type = UPC;
			*aux1++ = *aux2;
			length++;
		}
		else if (*aux2 == '-' || *aux2 == ' ')
		{
			/* skip, we could validate but I think it's worthless
			 *
			 * 跳过，我们可以验证，但我认为它毫无价值
			 */
		}
		else if (*aux2 == '!' && *(aux2 + 1) == '\0')
		{
			/* the invalid check digit suffix was found, set it
			 *
			 * 发现无效的校验位后缀，请设置
			 */
			if (!magic)
				valid = false;
			magic = true;
		}
		else if (!digit)
		{
			goto eaninvalid;
		}
		else
		{
			*aux1++ = *aux2;
			if (++length > 13)
				goto eantoobig;
		}
		aux2++;
	}
	*aux1 = '\0';				/* terminate the string */

	/* find the current check digit value
	 *
	 * 查找当前校验位值
	 */
	if (length == 13)
	{
		/* only EAN13 can be here
		 *
		 * 这里只有 EAN13 可以
		 */
		if (type != INVALID)
			goto eaninvalid;
		type = EAN13;
		check = buf[15] - '0';
	}
	else if (length == 12)
	{
		/* only UPC can be here
		 *
		 * 这里只能有UPC
		 */
		if (type != UPC)
			goto eaninvalid;
		check = buf[14] - '0';
	}
	else if (length == 10)
	{
		if (type != ISBN && type != ISMN)
			goto eaninvalid;
		if (buf[12] == 'X')
			check = 10;
		else
			check = buf[12] - '0';
	}
	else if (length == 8)
	{
		if (type != INVALID && type != ISSN)
			goto eaninvalid;
		type = ISSN;
		if (buf[10] == 'X')
			check = 10;
		else
			check = buf[10] - '0';
	}
	else
		goto eaninvalid;

	if (type == INVALID)
		goto eaninvalid;

	/* obtain the real check digit value, validate, and convert to ean13:
	 *
	 * 获取真实的校验位值，验证并转换为ean13：
	 */
	if (accept == EAN13 && type != accept)
		goto eanwrongtype;
	if (accept != ANY && type != EAN13 && type != accept)
		goto eanwrongtype;
	switch (type)
	{
		case EAN13:
			valid = (valid && ((rcheck = checkdig(buf + 3, 13)) == check || magic));
			/* now get the subtype of EAN13:
			 *
			 * 现在获取 EAN13 的子类型：
			 */
			if (buf[3] == '0')
				type = UPC;
			else if (strncmp("977", buf + 3, 3) == 0)
				type = ISSN;
			else if (strncmp("978", buf + 3, 3) == 0)
				type = ISBN;
			else if (strncmp("9790", buf + 3, 4) == 0)
				type = ISMN;
			else if (strncmp("979", buf + 3, 3) == 0)
				type = ISBN;
			if (accept != EAN13 && accept != ANY && type != accept)
				goto eanwrongtype;
			break;
		case ISMN:
			memcpy(buf, "9790", 4); /* this isn't for sure yet, for now ISMN
									 * it's only 9790 */
			valid = (valid && ((rcheck = checkdig(buf, 13)) == check || magic));
			break;
		case ISBN:
			memcpy(buf, "978", 3);
			valid = (valid && ((rcheck = weight_checkdig(buf + 3, 10)) == check || magic));
			break;
		case ISSN:
			memcpy(buf + 10, "00", 2);	/* append 00 as the normal issue
										 * publication code */
			memcpy(buf, "977", 3);
			valid = (valid && ((rcheck = weight_checkdig(buf + 3, 8)) == check || magic));
			break;
		case UPC:
			buf[2] = '0';
			valid = (valid && ((rcheck = checkdig(buf + 2, 13)) == check || magic));
		default:
			break;
	}

	/* fix the check digit:
	 *
	 * 修复校验位：
	 */
	for (aux1 = buf; *aux1 && *aux1 <= ' '; aux1++);
	aux1[12] = checkdig(aux1, 13) + '0';
	aux1[13] = '\0';

	if (!valid && !magic)
		goto eanbadcheck;

	*result = str2ean(aux1);
	*result |= valid ? 0 : 1;
	return true;

eanbadcheck:
	if (g_weak)
	{							/* weak input mode is activated: */
		/* set the "invalid-check-digit-on-input" flag
		 *
		 * 设置“输入时无效校验位”标志
		 */
		*result = str2ean(aux1);
		*result |= 1;
		return true;
	}

	if (rcheck == (unsigned) -1)
	{
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid %s number: \"%s\"",
						isn_names[accept], str)));
	}
	else
	{
		ereturn(escontext, false,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid check digit for %s number: \"%s\", should be %c",
						isn_names[accept], str, (rcheck == 10) ? ('X') : (rcheck + '0'))));
	}

eaninvalid:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for %s number: \"%s\"",
					isn_names[accept], str)));

eanwrongtype:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("cannot cast %s to %s for number: \"%s\"",
					isn_names[type], isn_names[accept], str)));

eantoobig:
	ereturn(escontext, false,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for %s type",
					str, isn_names[accept])));
}

/*----------------------------------------------------------
 * Exported routines.
 *
 * 导出的例程。
 *---------------------------------------------------------*/

void
_PG_init(void)
{
	if (ISN_DEBUG)
	{
		if (!check_table(EAN13_range, EAN13_index))
			elog(ERROR, "EAN13 failed check");
		if (!check_table(ISBN_range, ISBN_index))
			elog(ERROR, "ISBN failed check");
		if (!check_table(ISMN_range, ISMN_index))
			elog(ERROR, "ISMN failed check");
		if (!check_table(ISSN_range, ISSN_index))
			elog(ERROR, "ISSN failed check");
		if (!check_table(UPC_range, UPC_index))
			elog(ERROR, "UPC failed check");
	}

	/* Define a GUC variable for weak mode.
	 *
	 * 为弱模式定义 GUC 变量。
	 */
	DefineCustomBoolVariable("isn.weak",
							 "Accept input with invalid ISN check digits.",
							 NULL,
							 &g_weak,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("isn");
}

/* isn_out
 */
PG_FUNCTION_INFO_V1(isn_out);
Datum
isn_out(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);
	char	   *result;
	char		buf[MAXEAN13LEN + 1];

	(void) ean2string(val, false, buf, true);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/* ean13_out
 */
PG_FUNCTION_INFO_V1(ean13_out);
Datum
ean13_out(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);
	char	   *result;
	char		buf[MAXEAN13LEN + 1];

	(void) ean2string(val, false, buf, false);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/* ean13_in
 */
PG_FUNCTION_INFO_V1(ean13_in);
Datum
ean13_in(PG_FUNCTION_ARGS)
{
	const char *str = PG_GETARG_CSTRING(0);
	ean13		result;

	if (!string2ean(str, fcinfo->context, &result, EAN13))
		PG_RETURN_NULL();
	PG_RETURN_EAN13(result);
}

/* isbn_in
 */
PG_FUNCTION_INFO_V1(isbn_in);
Datum
isbn_in(PG_FUNCTION_ARGS)
{
	const char *str = PG_GETARG_CSTRING(0);
	ean13		result;

	if (!string2ean(str, fcinfo->context, &result, ISBN))
		PG_RETURN_NULL();
	PG_RETURN_EAN13(result);
}

/* ismn_in
 */
PG_FUNCTION_INFO_V1(ismn_in);
Datum
ismn_in(PG_FUNCTION_ARGS)
{
	const char *str = PG_GETARG_CSTRING(0);
	ean13		result;

	if (!string2ean(str, fcinfo->context, &result, ISMN))
		PG_RETURN_NULL();
	PG_RETURN_EAN13(result);
}

/* issn_in
 */
PG_FUNCTION_INFO_V1(issn_in);
Datum
issn_in(PG_FUNCTION_ARGS)
{
	const char *str = PG_GETARG_CSTRING(0);
	ean13		result;

	if (!string2ean(str, fcinfo->context, &result, ISSN))
		PG_RETURN_NULL();
	PG_RETURN_EAN13(result);
}

/* upc_in
 */
PG_FUNCTION_INFO_V1(upc_in);
Datum
upc_in(PG_FUNCTION_ARGS)
{
	const char *str = PG_GETARG_CSTRING(0);
	ean13		result;

	if (!string2ean(str, fcinfo->context, &result, UPC))
		PG_RETURN_NULL();
	PG_RETURN_EAN13(result);
}

/* casting functions
*/
PG_FUNCTION_INFO_V1(isbn_cast_from_ean13);
Datum
isbn_cast_from_ean13(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);
	ean13		result;

	(void) ean2isn(val, false, &result, ISBN);

	PG_RETURN_EAN13(result);
}

PG_FUNCTION_INFO_V1(ismn_cast_from_ean13);
Datum
ismn_cast_from_ean13(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);
	ean13		result;

	(void) ean2isn(val, false, &result, ISMN);

	PG_RETURN_EAN13(result);
}

PG_FUNCTION_INFO_V1(issn_cast_from_ean13);
Datum
issn_cast_from_ean13(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);
	ean13		result;

	(void) ean2isn(val, false, &result, ISSN);

	PG_RETURN_EAN13(result);
}

PG_FUNCTION_INFO_V1(upc_cast_from_ean13);
Datum
upc_cast_from_ean13(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);
	ean13		result;

	(void) ean2isn(val, false, &result, UPC);

	PG_RETURN_EAN13(result);
}


/* is_valid - returns false if the "invalid-check-digit-on-input" is set
 */
PG_FUNCTION_INFO_V1(is_valid);
Datum
is_valid(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);

	PG_RETURN_BOOL((val & 1) == 0);
}

/* make_valid - unsets the "invalid-check-digit-on-input" flag
 */
PG_FUNCTION_INFO_V1(make_valid);
Datum
make_valid(PG_FUNCTION_ARGS)
{
	ean13		val = PG_GETARG_EAN13(0);

	val &= ~((ean13) 1);
	PG_RETURN_EAN13(val);
}

/* this function temporarily sets weak input flag
 * (to lose the strictness of check digit acceptance)
 *
 * （失去校验位接受的严格性）
 */
PG_FUNCTION_INFO_V1(accept_weak_input);
Datum
accept_weak_input(PG_FUNCTION_ARGS)
{
	bool		newvalue = PG_GETARG_BOOL(0);

	(void) set_config_option("isn.weak", newvalue ? "on" : "off",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SET, true, 0, false);
	PG_RETURN_BOOL(g_weak);
}

PG_FUNCTION_INFO_V1(weak_input_status);
Datum
weak_input_status(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(g_weak);
}
