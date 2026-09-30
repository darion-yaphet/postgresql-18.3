/*-------------------------------------------------------------------------
 *
 * toast_helper.h
 *	  Helper functions for table AMs implementing compressed or
 *    out-of-line storage of varlena attributes.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/toast_helper.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_HELPER_H
#define TOAST_HELPER_H

#include "utils/rel.h"

/*
 * Information about one column of a tuple being toasted.
 *
 * NOTE: toast_action[i] can have these values:
 *		' '						default handling
 *		TYPSTORAGE_PLAIN		already processed --- don't touch it
 *		TYPSTORAGE_EXTENDED		incompressible, but OK to move off
 *
 * NOTE: toast_attr[i].tai_size is only made valid for varlena attributes with
 * toast_action[i] different from TYPSTORAGE_PLAIN.
 *
 * 中文翻译：
 * 有关正在烘烤的元组的一列的信息。注意：toast_action[i]
 *  可以具有以下值： ' ' 默认处理 TYPSTORAGE_PLAI
 * N 已处理 --- 不要触摸它 TYPSTORAGE_EXTENDE
 * D 不可压缩，但可以移开 注意：toast_attr[i].tai_
 * size 仅对 toast_action[i] 与 TYPSTORA
 * GE_PLAIN 不同的 varlena 属性有效。
 */
typedef struct
{
	struct varlena *tai_oldexternal;
	int32		tai_size;
	uint8		tai_colflags;
	char		tai_compression;
} ToastAttrInfo;

/*
 * Information about one tuple being toasted.
 *
 * 中文翻译：
 * 有关正在烘烤的一个元组的信息。
 */
typedef struct
{
	/*
	 * Before calling toast_tuple_init, the caller must initialize the
	 * following fields.  Each array must have a length equal to
	 * ttc_rel->rd_att->natts.  The ttc_oldvalues and ttc_oldisnull fields
	 * should be NULL in the case of an insert.
	 *
	 * 中文翻译：
	 * 在调用 toast_tuple_init 之前，调用者必须初始化以下
	 * 字段。每个数组的长度必须等于 ttc_rel->rd_att->na
	 * tts。在插入的情况下，ttc_oldvalues 和 ttc_ol
	 * disnull 字段应为 NULL。
	 */
	Relation	ttc_rel;		/* the relation that contains the tuple */

	/* 中文翻译：包含元组的关系 */
	Datum	   *ttc_values;		/* values from the tuple columns */

	/* 中文翻译：来自元组列的值 */
	bool	   *ttc_isnull;		/* null flags for the tuple columns */

	/* 中文翻译：元组列的空标志 */
	Datum	   *ttc_oldvalues;	/* values from previous tuple */

	/* 中文翻译：来自前一个元组的值 */
	bool	   *ttc_oldisnull;	/* null flags from previous tuple */

	/* 中文翻译：前一个元组的空标志 */

	/*
	 * Before calling toast_tuple_init, the caller should set ttc_attr to
	 * point to an array of ToastAttrInfo structures of a length equal to
	 * ttc_rel->rd_att->natts.  The contents of the array need not be
	 * initialized.  ttc_flags also does not need to be initialized.
	 *
	 * 中文翻译：
	 * 在调用toast_tuple_init之前，调用者应将ttc_att
	 * r设置为指向长度等于ttc_rel->rd_att->natts的T
	 * oastAttrInfo结构数组。数组的内容不需要初始化。 ttc_
	 * flags 也不需要初始化。
	 */
	uint8		ttc_flags;
	ToastAttrInfo *ttc_attr;
} ToastTupleContext;

/*
 * Flags indicating the overall state of a TOAST operation.
 *
 * TOAST_NEEDS_DELETE_OLD indicates that one or more old TOAST datums need
 * to be deleted.
 *
 * TOAST_NEEDS_FREE indicates that one or more TOAST values need to be freed.
 *
 * TOAST_HAS_NULLS indicates that nulls were found in the tuple being toasted.
 *
 * TOAST_NEEDS_CHANGE indicates that a new tuple needs to built; in other
 * words, the toaster did something.
 *
 * 中文翻译：
 * 指示 TOAST 操作总体状态的标志。 TOAST_NEEDS_DE
 * LETE_OLD 表示需要删除一个或多个旧的 TOAST 数据。 T
 * OAST_NEEDS_FREE 表示需要释放一个或多个 TOAST
 * 值。 TOAST_HAS_NULLS 指示在正在烘烤的元组中发现空值
 * 。 TOAST_NEEDS_CHANGE 表示需要构建一个新的元组；
 * 换句话说，烤面包机做了一些事情。
 */
#define TOAST_NEEDS_DELETE_OLD				0x0001
#define TOAST_NEEDS_FREE					0x0002
#define TOAST_HAS_NULLS						0x0004
#define TOAST_NEEDS_CHANGE					0x0008

/*
 * Flags indicating the status of a TOAST operation with respect to a
 * particular column.
 *
 * TOASTCOL_NEEDS_DELETE_OLD indicates that the old TOAST datums for this
 * column need to be deleted.
 *
 * TOASTCOL_NEEDS_FREE indicates that the value for this column needs to
 * be freed.
 *
 * TOASTCOL_IGNORE indicates that the toaster should not further process
 * this column.
 *
 * TOASTCOL_INCOMPRESSIBLE indicates that this column has been found to
 * be incompressible, but could be moved out-of-line.
 *
 * 中文翻译：
 * 指示与特定列相关的 TOAST 操作状态的标志。 TOASTCOL_
 * NEEDS_DELETE_OLD 指示需要删除该列的旧 TOAST
 * 数据。 TOASTCOL_NEEDS_FREE 指示需要释放该列的值
 * 。 TOASTCOL_IGNORE 指示烤面包机不应进一步处理此列。
 *  TOASTCOL_INCOMPRESSIBLE 表示已发现该列不可
 * 压缩，但可以移出线外。
 */
#define TOASTCOL_NEEDS_DELETE_OLD			TOAST_NEEDS_DELETE_OLD
#define TOASTCOL_NEEDS_FREE					TOAST_NEEDS_FREE
#define TOASTCOL_IGNORE						0x0010
#define TOASTCOL_INCOMPRESSIBLE				0x0020

/*
 * Function toast_tuple_init initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 toast_tuple_init通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void toast_tuple_init(ToastTupleContext *ttc);
/*
 * Function toast_tuple_find_biggest_attribute retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 toast_tuple_find_biggest_attribute通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern int	toast_tuple_find_biggest_attribute(ToastTupleContext *ttc,
											   bool for_compression,
											   bool check_main);
/*
 * Function toast_tuple_try_compression constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_tuple_try_compression通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void toast_tuple_try_compression(ToastTupleContext *ttc, int attribute);
/*
 * Function toast_tuple_externalize carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 toast_tuple_externalize通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void toast_tuple_externalize(ToastTupleContext *ttc, int attribute,
									int options);
/*
 * Function toast_tuple_cleanup carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 toast_tuple_cleanup通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void toast_tuple_cleanup(ToastTupleContext *ttc);

/*
 * Function toast_delete_external constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 toast_delete_external通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void toast_delete_external(Relation rel, const Datum *values, const bool *isnull,
								  bool is_speculative);

#endif
