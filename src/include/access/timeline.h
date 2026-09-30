/*
 * timeline.h
 *
 * 说明恢复时间线的历史记录、范围和切换点。
 *
 * Functions for reading and writing timeline history files.
 *
 * 说明恢复时间线的历史记录、范围和切换点。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/timeline.h
 *
 * 说明恢复时间线的历史记录、范围和切换点。
 */
#ifndef TIMELINE_H
#define TIMELINE_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

/*
 * A list of these structs describes the timeline history of the server. Each
 * TimeLineHistoryEntry represents a piece of WAL belonging to the history,
 * from newest to oldest. All WAL locations between 'begin' and 'end' belong to
 * the timeline represented by the entry. Together the 'begin' and 'end'
 * pointers of all the entries form a contiguous line from beginning of time
 * to infinity.
 *
 * 说明恢复时间线的历史记录、范围和切换点。
 */
typedef struct
{
	TimeLineID	tli;
	XLogRecPtr	begin;			/* inclusive */

	/* 包含边界。 */
	XLogRecPtr	end;			/* exclusive, InvalidXLogRecPtr means infinity */

	/* 不包含边界；InvalidXLogRecPtr 表示无穷大。 */
} TimeLineHistoryEntry;

/*
 * Function: readTimeLineHistory.
 * Purpose: Obtains or checks the state represented by read time line history.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：readTimeLineHistory。
 * 作用：获取或检查 read time line history 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern List *readTimeLineHistory(TimeLineID targetTLI);
/*
 * Function: existsTimeLineHistory.
 * Purpose: Performs the operation represented by exists time line history.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：existsTimeLineHistory。
 * 作用：执行 exists time line history 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool existsTimeLineHistory(TimeLineID probeTLI);
/*
 * Function: findNewestTimeLine.
 * Purpose: Obtains or checks the state represented by find newest time line.
 * Core flow: It examines the supplied identifiers and subsystem state, then returns the derived result.
 *
 * 函数：findNewestTimeLine。
 * 作用：获取或检查 find newest time line 所表示的状态。
 * 核心流程：它检查给定标识符和子系统状态，然后返回推导结果。
 */
extern TimeLineID findNewestTimeLine(TimeLineID startTLI);
/*
 * Function: writeTimeLineHistory.
 * Purpose: Creates or starts the work represented by write time line history.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：writeTimeLineHistory。
 * 作用：创建或启动 write time line history 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void writeTimeLineHistory(TimeLineID newTLI, TimeLineID parentTLI,
								 XLogRecPtr switchpoint, char *reason);
/*
 * Function: writeTimeLineHistoryFile.
 * Purpose: Creates or starts the work represented by write time line history file.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：writeTimeLineHistoryFile。
 * 作用：创建或启动 write time line history file 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void writeTimeLineHistoryFile(TimeLineID tli, char *content, int size);
/*
 * Function: restoreTimeLineHistoryFiles.
 * Purpose: Creates or starts the work represented by restore time line history files.
 * Core flow: It prepares the required context, performs the requested operation, and makes the result available to the owner.
 *
 * 函数：restoreTimeLineHistoryFiles。
 * 作用：创建或启动 restore time line history files 所表示的工作。
 * 核心流程：它准备所需上下文，完成请求操作，并向所属方提供结果。
 */
extern void restoreTimeLineHistoryFiles(TimeLineID begin, TimeLineID end);
/*
 * Function: tliInHistory.
 * Purpose: Performs the operation represented by tli in history.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：tliInHistory。
 * 作用：执行 tli in history 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern bool tliInHistory(TimeLineID tli, List *expectedTLEs);
/*
 * Function: tliOfPointInHistory.
 * Purpose: Performs the operation represented by tli of point in history.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：tliOfPointInHistory。
 * 作用：执行 tli of point in history 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern TimeLineID tliOfPointInHistory(XLogRecPtr ptr, List *history);
/*
 * Function: tliSwitchPoint.
 * Purpose: Performs the operation represented by tli switch point.
 * Core flow: It uses supplied arguments to access relevant subsystem state and returns or records the result.
 *
 * 函数：tliSwitchPoint。
 * 作用：执行 tli switch point 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录结果。
 */
extern XLogRecPtr tliSwitchPoint(TimeLineID tli, List *history,
								 TimeLineID *nextTLI);

#endif							/* TIMELINE_H */
