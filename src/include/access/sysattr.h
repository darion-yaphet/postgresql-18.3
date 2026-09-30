/*-------------------------------------------------------------------------
 *
 * sysattr.h
 *	  POSTGRES system attribute definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sysattr.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * sysattr.h POSTGRES 系统属性定义。 src/inc
 * lude/access/sysattr.h
 */
#ifndef SYSATTR_H
#define SYSATTR_H


/*
 * Attribute numbers for the system-defined attributes
 *
 * 中文翻译：
 * 系统定义属性的属性编号
 */
#define SelfItemPointerAttributeNumber			(-1)
#define MinTransactionIdAttributeNumber			(-2)
#define MinCommandIdAttributeNumber				(-3)
#define MaxTransactionIdAttributeNumber			(-4)
#define MaxCommandIdAttributeNumber				(-5)
#define TableOidAttributeNumber					(-6)
#define FirstLowInvalidHeapAttributeNumber		(-7)

#endif							/* SYSATTR_H */

/* 中文翻译：系统属性_H */
