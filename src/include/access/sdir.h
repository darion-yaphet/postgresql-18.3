/*-------------------------------------------------------------------------
 *
 * sdir.h
 *	  POSTGRES scan direction definitions.
 *
 *
 *	  POSTGRES 扫描方向定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sdir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SDIR_H
#define SDIR_H


/*
 * Defines the direction for scanning a table or an index.  Scans are never
 * invoked using NoMovementScanDirection.  For convenience, we use the values
 * -1 and 1 for backward and forward scans.  This allows us to perform a few
 * mathematical tricks such as what is done in ScanDirectionCombine.
 */

/*
 * 定义扫描表或索引的方向。扫描绝不会使用 NoMovementScanDirection 调用。为方便起见，
 * 后向扫描和前向扫描分别使用 -1 和 1。这样可以执行一些数学技巧，例如
 * ScanDirectionCombine 所做的处理。
 */
typedef enum ScanDirection
{
	BackwardScanDirection = -1,
	NoMovementScanDirection = 0,
	ForwardScanDirection = 1
} ScanDirection;

/*
 * Determine the net effect of two direction specifications.
 * This relies on having ForwardScanDirection = +1, BackwardScanDirection = -1,
 * and will probably not do what you want if applied to any other values.
 */

/*
 * 确定两个方向说明的合成效果。此宏依赖 ForwardScanDirection = +1 与
 * BackwardScanDirection = -1；若用于其他值，很可能不会产生预期结果。
 */
#define ScanDirectionCombine(a, b)  ((a) * (b))

/*
 * ScanDirectionIsValid
 *		True iff scan direction is valid.
 */

/*
 * ScanDirectionIsValid
 *		当且仅当扫描方向有效时为真。
 */
#define ScanDirectionIsValid(direction) \
	((bool) (BackwardScanDirection <= (direction) && \
			 (direction) <= ForwardScanDirection))

/*
 * ScanDirectionIsBackward
 *		True iff scan direction is backward.
 */

/*
 * ScanDirectionIsBackward
 *		当且仅当扫描方向为后向时为真。
 */
#define ScanDirectionIsBackward(direction) \
	((bool) ((direction) == BackwardScanDirection))

/*
 * ScanDirectionIsNoMovement
 *		True iff scan direction indicates no movement.
 */

/*
 * ScanDirectionIsNoMovement
 *		当且仅当扫描方向表示不移动时为真。
 */
#define ScanDirectionIsNoMovement(direction) \
	((bool) ((direction) == NoMovementScanDirection))

/*
 * ScanDirectionIsForward
 *		True iff scan direction is forward.
 */

/*
 * ScanDirectionIsForward
 *		当且仅当扫描方向为前向时为真。
 */
#define ScanDirectionIsForward(direction) \
	((bool) ((direction) == ForwardScanDirection))

#endif							/* SDIR_H */
