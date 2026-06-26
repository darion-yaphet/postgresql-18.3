/*
 * ISSN.h
 *	  PostgreSQL type definitions for ISNs (ISBN, ISMN, ISSN, EAN13, UPC)
 *
 * ISSN.h ISN 的 PostgreSQL 类型定义（ISBN、ISMN、ISSN、EAN13、UPC）
 *
 * No information available for UPC prefixes
 *
 * 没有可用的 UPC 前缀信息
 *
 *
 * IDENTIFICATION
 *	  contrib/isn/UPC.h
 *
 * 识别 contrib/isn/UPC.h
 *
 */

/* where the digit set begins, and how many of them are in the table
 *
 * 数字集从哪里开始，以及表中有多少个数字
 */
static const unsigned UPC_index[10][2] = {
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
	{0, 0},
};
static const char *UPC_range[][2] = {
	{NULL, NULL}
};
