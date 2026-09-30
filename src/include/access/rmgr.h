/*
 * rmgr.h
 *
  * rmgr.h
 *
 * Resource managers definition
 *
  * 资源管理器定义
 *
 * src/include/access/rmgr.h
 *
  * src/include/access/rmgr.h
 */
#ifndef RMGR_H
#define RMGR_H

typedef uint8 RmgrId;

/*
 * Built-in resource managers
 *
  * 内置资源管理器
 *
 * The actual numerical values for each rmgr ID are defined by the order
 * of entries in rmgrlist.h.
 *
  * 每个 rmgr ID 的实际数值由 rmgrlist.h 中的条目顺序定义。
 *
 * Note: RM_MAX_ID must fit in RmgrId; widening that type will affect the XLOG
 * file format.
 *
  * 注意：RM_MAX_ID 必须适合 RmgrId；扩大该类型将影响 XLOG 文件格式。
 */
#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode) \
	symname,

typedef enum RmgrIds
{
#include "access/rmgrlist.h"
	RM_NEXT_ID
}			RmgrIds;

#undef PG_RMGR

#define RM_MAX_ID			UINT8_MAX
#define RM_MAX_BUILTIN_ID	(RM_NEXT_ID - 1)
#define RM_MIN_CUSTOM_ID	128
#define RM_MAX_CUSTOM_ID	UINT8_MAX
#define RM_N_IDS			(UINT8_MAX + 1)
#define RM_N_BUILTIN_IDS	(RM_MAX_BUILTIN_ID + 1)
#define RM_N_CUSTOM_IDS		(RM_MAX_CUSTOM_ID - RM_MIN_CUSTOM_ID + 1)

/*
 * Function: RmgrIdIsBuiltin.
 * Purpose: Performs the operation represented by rmgr id is builtin.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：RmgrIdIsBuiltin。
 * 作用：执行 rmgr id is builtin 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
static inline bool
RmgrIdIsBuiltin(int rmid)
{
	return rmid <= RM_MAX_BUILTIN_ID;
}

/*
 * Function: RmgrIdIsCustom.
 * Purpose: Performs the operation represented by rmgr id is custom.
 * Core flow: It uses the supplied arguments to access the relevant subsystem state and returns or records the resulting state.
 *
 * 函数：RmgrIdIsCustom。
 * 作用：执行 rmgr id is custom 所表示的操作。
 * 核心流程：它使用给定参数访问相关子系统状态，并返回或记录生成的状态。
 */
static inline bool
RmgrIdIsCustom(int rmid)
{
	return rmid >= RM_MIN_CUSTOM_ID && rmid <= RM_MAX_CUSTOM_ID;
}

#define RmgrIdIsValid(rmid) (RmgrIdIsBuiltin((rmid)) || RmgrIdIsCustom((rmid)))

/*
 * RmgrId to use for extensions that require an RmgrId, but are still in
 * development and have not reserved their own unique RmgrId yet. See:
 * https://wiki.postgresql.org/wiki/CustomWALResourceManagers
 *
  * RmgrId 用于需要 RmgrId 的扩展，但仍在开发中，尚未保留自己唯一的 RmgrId。请参阅：https://wiki.postgresql.org/wiki/CustomWALResourceManagers
 */
#define RM_EXPERIMENTAL_ID		128

#endif							/* RMGR_H */
