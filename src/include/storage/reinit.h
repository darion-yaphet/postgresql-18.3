/*-------------------------------------------------------------------------
 *
 * reinit.h
 *	  Reinitialization of unlogged relations
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/reinit.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef REINIT_H
#define REINIT_H

#include "common/relpath.h"


/*
 * Resets unlogged relations according to the requested cleanup operation.
 */

/*
 * 根据请求的清理操作重置非日志关系。
 */
extern void ResetUnloggedRelations(int op);
/*
 * Parses a non-temporary relation filename into its physical identifiers.
 */

/*
 * 将非临时关系文件名解析为其物理标识符。
 */
extern bool parse_filename_for_nontemp_relation(const char *name,
												RelFileNumber *relnumber,
												ForkNumber *fork,
												unsigned *segno);

#define UNLOGGED_RELATION_CLEANUP		0x0001
#define UNLOGGED_RELATION_INIT			0x0002

#endif							/* REINIT_H */
