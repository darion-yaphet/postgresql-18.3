/*-------------------------------------------------------------------------
 *
 * visibilitymapdefs.h
 *		macros for accessing contents of visibility map pages
 *
 *
 * Copyright (c) 2021-2025, PostgreSQL Global Development Group
 *
 * src/include/access/visibilitymapdefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAPDEFS_H
#define VISIBILITYMAPDEFS_H

/* Number of bits for one heap page */

/* 中文翻译：一个堆页的位数 */
#define BITS_PER_HEAPBLOCK 2

/* Flags for bit map */

/* 中文翻译：位图标志 */
#define VISIBILITYMAP_ALL_VISIBLE	0x01
#define VISIBILITYMAP_ALL_FROZEN	0x02
#define VISIBILITYMAP_VALID_BITS	0x03	/* OR of all valid visibilitymap
											 * flags bits */

/* 中文翻译：
 * 所有有效可见性映射标志位的或
 */
/*
 * To detect recovery conflicts during logical decoding on a standby, we need
 * to know if a table is a user catalog table. For that we add an additional
 * bit into xl_heap_visible.flags, in addition to the above.
 *
 * NB: VISIBILITYMAP_XLOG_* may not be passed to visibilitymap_set().
 *
 * 中文翻译：
 * 为了检测备用数据库上逻辑解码期间的恢复冲突，我们需要知道表是否是用户
 * 目录表。为此，除了上述内容之外，我们还在 xl_heap_visib
 * le.flags 中添加了一个额外的位。注意：VISIBILITYM
 * AP_XLOG_* 可能无法传递给visibilitymap_set
 * ()。
 */
#define VISIBILITYMAP_XLOG_CATALOG_REL	0x04
#define VISIBILITYMAP_XLOG_VALID_BITS	(VISIBILITYMAP_VALID_BITS | VISIBILITYMAP_XLOG_CATALOG_REL)

#endif							/* VISIBILITYMAPDEFS_H */

/* 中文翻译：可见性MAPDEFS_H */
