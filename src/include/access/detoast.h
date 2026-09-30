/*-------------------------------------------------------------------------
 *
 * detoast.h
 *	  Access to compressed and external varlena values.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/detoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DETOAST_H
#define DETOAST_H

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "struct varatt_external" toast pointer.  This should be
 * just a memcpy, but some versions of gcc seem to produce broken code
 * that assumes the datum contents are aligned.  Introducing an explicit
 * intermediate "varattrib_1b_e *" variable seems to fix it.
 *
 * 中文翻译：
 * 用于将 EXTERNAL 数据可能未对齐的内容提取到本地“struc
 * t varatt_external”toast 指针中的宏。这应该只
 * 是一个 memcpy，但某些版本的 gcc 似乎会生成假设数据内容对
 * 齐的损坏代码。引入显式中间“varattrib_1b_e *”变量似
 * 乎可以修复它。
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */

/* 中文翻译：包含标准 TOAST 指针的 EXTERNAL 数据的大小 */
#define TOAST_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external))

/* Size of an EXTERNAL datum that contains an indirection pointer */

/* 中文翻译：包含间接指针的 EXTERNAL 数据的大小 */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

/* ----------
 * detoast_external_attr() -
 *
 *		Fetches an external stored attribute from the toast
 *		relation. Does NOT decompress it, if stored external
 *		in compressed format.
 * ----------
 *
 * 中文翻译：
 * detoast_external_attr() - 从 toast
 * 关系中获取外部存储的属性。如果以压缩格式存储在外部，则不会对其进行解
 * 压缩。
 */
/*
 * Function detoast_external_attr carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 detoast_external_attr通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern struct varlena *detoast_external_attr(struct varlena *attr);

/* ----------
 * detoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 *
 * 中文翻译：
 * detoast_attr() - 完全解构一个属性，根据需要获取和/
 * 或解压缩它。
 */
/*
 * Function detoast_attr carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 detoast_attr通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern struct varlena *detoast_attr(struct varlena *attr);

/* ----------
 * detoast_attr_slice() -
 *
 *		Fetches only the specified portion of an attribute.
 *		(Handles all cases for attribute storage)
 * ----------
 *
 * 中文翻译：
 * detoast_attr_slice() - 仅获取属性的指定部分。
 *  （处理属性存储的所有情况）
 */
/*
 * Function detoast_attr_slice carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 detoast_attr_slice通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern struct varlena *detoast_attr_slice(struct varlena *attr,
										  int32 sliceoffset,
										  int32 slicelength);

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 * ----------
 *
 * 中文翻译：
 * toast_raw_datum_size - 返回 varlena
 * 基准的原始（detoasted）大小
 */
/*
 * Function toast_raw_datum_size carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 toast_raw_datum_size通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Size toast_raw_datum_size(Datum value);

/* ----------
 * toast_datum_size -
 *
 *	Return the storage size of a varlena datum
 * ----------
 *
 * 中文翻译：
 * toast_datum_size - 返回 varlena 数据的存
 * 储大小
 */
/*
 * Function toast_datum_size carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 toast_datum_size通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern Size toast_datum_size(Datum value);

#endif							/* DETOAST_H */

/* 中文翻译：DETOAST_H */
