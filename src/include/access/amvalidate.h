/*-------------------------------------------------------------------------
 *
 * amvalidate.h
 *	  Support routines for index access methods' amvalidate and
 *	  amadjustmembers functions.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * src/include/access/amvalidate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMVALIDATE_H
#define AMVALIDATE_H

#include "utils/catcache.h"


/* Struct returned (in a list) by identify_opfamily_groups() */

/* identify_opfamily_groups() 返回的结构体（以列表形式返回） */
typedef struct OpFamilyOpFuncGroup
{
	Oid			lefttype;		/* amoplefttype/amproclefttype */

	/* amoplefttype/amproclefttype（左操作数类型） */
	Oid			righttype;		/* amoprighttype/amprocrighttype */

	/* amoprighttype/amprocrighttype（右操作数类型） */
	uint64		operatorset;	/* bitmask of operators with these types */

	/* 具有这些类型的操作符位掩码 */
	uint64		functionset;	/* bitmask of support funcs with these types */

	/* 具有这些类型的支持函数位掩码 */
} OpFamilyOpFuncGroup;


/* Functions in access/index/amvalidate.c */

/* access/index/amvalidate.c 中的函数 */

/*
 * Scan an operator family's pg_amop and pg_amproc catalog entries and group
 * them by their left/right datatype pairs, returning the collected groups as
 * a list of OpFamilyOpFuncGroup structs for a validator to inspect.
 *
 * 扫描一个操作符族的 pg_amop 和 pg_amproc 系统目录条目，并按其左/右数据类型
 * 对进行分组，将收集到的分组以 OpFamilyOpFuncGroup 结构体列表的形式返回，
 * 供验证器检查。
 */
extern List *identify_opfamily_groups(CatCList *oprlist, CatCList *proclist);
/*
 * Verify that a support function has the expected signature: check that its
 * return type matches restype and that its argument types match the variadic
 * list of expected type OIDs, with the argument count bounded by minargs and
 * maxargs. When exact is true the return type must match precisely; otherwise
 * a binary-coercible return type is accepted. Returns true when the signature
 * is acceptable.
 *
 * 验证支持函数是否具有预期的签名：检查其返回类型是否与 restype 匹配，且其参数
 * 类型是否与可变参数列表中期望的类型 OID 匹配，参数个数由 minargs 和 maxargs
 * 限定。当 exact 为 true 时返回类型必须精确匹配；否则接受可二进制强制转换的
 * 返回类型。当签名可接受时返回 true。
 */
extern bool check_amproc_signature(Oid funcid, Oid restype, bool exact,
								   int minargs, int maxargs,...);
/*
 * Verify that a function has the signature required of an operator-class
 * options (amproc) procedure, i.e. it takes a single internal argument and
 * returns void. Internally this delegates to check_amproc_signature() with
 * the fixed expected types. Returns true when the signature is acceptable.
 *
 * 验证函数是否具有操作符类选项（amproc）过程所要求的签名，即它接受单个
 * internal 参数并返回 void。内部通过固定的期望类型委托给
 * check_amproc_signature()。当签名可接受时返回 true。
 */
extern bool check_amoptsproc_signature(Oid funcid);
/*
 * Verify that an operator has the expected signature: look up the operator by
 * opno and check that its result type matches restype and that its left and
 * right input types match lefttype and righttype. Returns true when the
 * operator's signature is acceptable.
 *
 * 验证操作符是否具有预期的签名：通过 opno 查找该操作符，并检查其结果类型是否与
 * restype 匹配，以及其左右输入类型是否与 lefttype 和 righttype 匹配。当操作符
 * 的签名可接受时返回 true。
 */
extern bool check_amop_signature(Oid opno, Oid restype,
								 Oid lefttype, Oid righttype);
/*
 * Find an opclass belonging to the given access method (amoid) and operator
 * family (opfamilyoid) whose input datatype is datatypeoid, by scanning
 * pg_opclass for a matching entry. Returns the OID of a suitable opclass, or
 * InvalidOid when none exists.
 *
 * 通过扫描 pg_opclass 查找匹配的条目，找到属于给定访问方法（amoid）和操作符族
 * （opfamilyoid）、且输入数据类型为 datatypeoid 的操作符类。返回合适的操作符类
 * 的 OID，若不存在则返回 InvalidOid。
 */
extern Oid	opclass_for_family_datatype(Oid amoid, Oid opfamilyoid,
										Oid datatypeoid);
/*
 * Determine whether the given operator family (opfamilyoid) can sort values
 * of datatypeoid, i.e. whether a btree opclass exists in that family for the
 * datatype. Internally this checks for a suitable opclass via
 * opclass_for_family_datatype(). Returns true when the type is sortable by
 * the family.
 *
 * 判断给定的操作符族（opfamilyoid）是否能对 datatypeoid 类型的值进行排序，即该
 * 族中是否存在针对该数据类型的 btree 操作符类。内部通过
 * opclass_for_family_datatype() 检查是否存在合适的操作符类。当该类型可被该族
 * 排序时返回 true。
 */
extern bool opfamily_can_sort_type(Oid opfamilyoid, Oid datatypeoid);

#endif							/* AMVALIDATE_H */

/* AMVALIDATE_H：amvalidate.h 的包含保护宏。 */
