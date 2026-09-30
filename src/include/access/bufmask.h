/*-------------------------------------------------------------------------
 *
 * bufmask.h
 *	  Definitions for buffer masking routines, used to mask certain bits
 *	  in a page which can be different when the WAL is generated
 *	  and when the WAL is applied. This is really the job of each
 *	  individual rmgr, but we make things easier by providing some
 *	  common routines to handle cases which occur in multiple rmgrs.
 *
 * 缓冲区掩码例程的定义，用于掩盖页面中在生成 WAL 时与应用 WAL 时可能不同的
 * 特定位。这本应由各个 rmgr 分别完成，但这里提供了可处理多个 rmgr 中共同情况的
 * 通用例程，从而简化实现。
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * src/include/access/bufmask.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef BUFMASK_H
#define BUFMASK_H

#include "storage/block.h"
#include "storage/bufmgr.h"

/* Marker used to mask pages consistently */

/* 用于一致掩盖页面内容的标记。 */
#define MASK_MARKER		0

/* Mask a page's LSN and checksum fields for WAL comparison.
 * The routine clears values that legitimately differ between original and
 * replayed pages before their contents are compared.
 *
 * 为 WAL 比较掩盖页面的 LSN 和校验和字段。
 * 此例程会在比较页面内容前清除原始页面与重放页面之间允许不同的值。
 */
extern void mask_page_lsn_and_checksum(Page page);

/* Mask page hint bits that WAL replay may set independently.
 * The routine removes non-WAL-logged hint state before page comparison.
 *
 * 掩盖 WAL 重放可能独立设置的页面提示位。
 * 此例程在页面比较前移除未写入 WAL 的提示状态。
 */
extern void mask_page_hint_bits(Page page);

/* Mask unused page space before comparison.
 * The routine fills irrelevant free bytes with a stable marker.
 *
 * 在比较前掩盖页面中未使用的空间。
 * 此例程用稳定标记填充无关的空闲字节。
 */
extern void mask_unused_space(Page page);

/* Mask line-pointer flag bits that can legitimately vary.
 * The routine normalizes the flag representation used in page comparison.
 *
 * 掩盖可以合法变化的行指针标志位。
 * 此例程规范化页面比较使用的标志表示。
 */
extern void mask_lp_flags(Page page);

/* Apply all common page-content masks.
 * The routine combines field, hint, free-space, and line-pointer masking to
 * produce a comparison-safe page image.
 *
 * 应用全部通用页面内容掩码。
 * 此例程组合字段、提示位、空闲空间和行指针掩码，生成可安全比较的页面映像。
 */
extern void mask_page_content(Page page);

#endif
