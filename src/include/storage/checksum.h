/*-------------------------------------------------------------------------
 *
 * checksum.h
 *	  Checksum implementation for data pages.
 *
 *	  数据页的校验和实现。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include "storage/block.h"

/*
 * Compute the checksum for a Postgres page.  The page must be aligned on a
 * 4-byte boundary.
 *
 * 计算一个 Postgres 页面校验和。该页面必须按 4 字节边界对齐。
 *
 * The function hashes the page contents with its block number while excluding the stored checksum field.
 *
 * 该函数将页面内容与其块号混合哈希，同时排除已存储的校验和字段。
 */
extern uint16 pg_checksum_page(char *page, BlockNumber blkno);

#endif							/* CHECKSUM_H */
