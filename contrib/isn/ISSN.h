/*
 * ISSN.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * ISSN.h ISN 的 PostgreSQL 类型定义（ISBN、ISMN、ISSN、EAN13、UPC）
 *
 * Information recompiled by Kronuz on November 12, 2004
 * http://www.issn.org/
 *
 * 信息由 Kronuz 于 2004 年 11 月 12 日重新编译 http://www.issn.org/
 *
 * IDENTIFICATION
 *	  contrib/isn/ISSN.h
 *
 * 识别 contrib/isn/ISSN.h
 *
 * 1144-875X <=> 1144875(X) <=> 1144875 <=> (977)1144875 <=> 9771144875(00) <=> 977114487500(7) <=> 977-1144-875-00-7
 *
 *
 * ISSN			1	1	4	 4	  8    7	5
 * Weight		8	7	6	 5	  4    3	2
 * Product		8 + 7 + 24 + 20 + 32 + 21 + 10 = 122
 *				122 / 11 = 11 remainder 1
 * Check digit	11 - 1 = 10 = X
 * => 1144-875X
 *
 * ISSN 1 1 4 4 8 7 5 重量 8 7 6 5 4 3 2 产品 8 + 7 + 24 + 20 + 32 + 21 + 10 = 122 122 / 11 = 11 余数 1 校验位 11 - 1 = 10 = X => 1144-875X
 *
 * ISSN			9	7	 7	 1	 1	 4	  4   8    7   5	0	0
 * Weight		1	3	 1	 3	 1	 3	  1   3    1   3	1	3
 * Product		9 + 21 + 7 + 3 + 1 + 12 + 4 + 24 + 7 + 15 + 0 + 0 = 103
 *				103 / 10 = 10 remainder 3
 * Check digit	10 - 3 = 7
 * => 977-1144875-00-7 ??  <- supplemental number (number of the week, month, etc.)
 *				  ^^ 00 for non-daily publications (01=Monday, 02=Tuesday, ...)
 *
 * ISSN 9 7 7 1 1 4 4 8 7 5 0 0 重量 1 3 1 3 1 3 1 3 1 3 1 3 产品 9 + 21 + 7 + 3 + 1 + 12 + 4 + 24 + 7 + 15 + 0 + 0 = 103 103 / 10 = 10 余数 3 校验位 10 - 3 = 7 => 977-1144875-00-7 ??  <- 补充编号（周数、月数等）^^ 00 用于非每日出版物（01=星期一，02=星期二，...）
 *
 * The hyphenation is always in after the four digits of the ISSN code.
 *
 * 连字符始终位于 ISSN 代码的四位数字之后。
 *
 */

/* where the digit set begins, and how many of them are in the table
 *
 * 数字集从哪里开始，以及表中有多少个数字
 */
static const unsigned ISSN_index[10][2] = {
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
	{0, 1},
};
static const char *ISSN_range[][2] = {
	{"0000-000", "9999-999"},
	{NULL, NULL}
};
