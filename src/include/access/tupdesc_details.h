/*-------------------------------------------------------------------------
 *
 * tupdesc_details.h
 *	  POSTGRES tuple descriptor definitions we can't include everywhere
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc_details.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * tupdesc_details.h POSTGRES 元组描述符定义
 * ，我们不能在所有地方都包含 src/include/access/t
 * updesc_details.h
 */

#ifndef TUPDESC_DETAILS_H
#define TUPDESC_DETAILS_H

/*
 * Structure used to represent value to be used when the attribute is not
 * present at all in a tuple, i.e. when the column was created after the tuple
 *
 * 中文翻译：
 * 用于表示当元组中根本不存在该属性时（即在元组之后创建列时）要使用的值
 * 的结构
 */
typedef struct AttrMissing
{
	bool		am_present;		/* true if non-NULL missing value exists */

	/* 中文翻译：如果存在非 NULL 缺失值则为 true */
	Datum		am_value;		/* value when attribute is missing */

	/* 中文翻译：属性缺失时的值 */
} AttrMissing;

#endif							/* TUPDESC_DETAILS_H */

/* 中文翻译：TUPDESC_DETAILS_H */
