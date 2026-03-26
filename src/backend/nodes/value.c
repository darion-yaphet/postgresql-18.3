/*-------------------------------------------------------------------------
 *
 * value.c
 *	  implementation of value nodes
 *	  值节点（Value Nodes）的实现
 *
 * 实现核心流程概述：
 * 本文件提供了一组简单的“构造函数”（Factory Functions），用于创建 PostgreSQL 内部使用的
 * 基础值节点（Value Nodes）。这些节点主要用于在解析器和表达式处理过程中封装着字面量。
 *
 * 核心流程如下：
 * 1. 结构封装：利用 makeNode() 宏分配特定的节点结构（如 Integer, Float, String 等）。
 * 2. 字段初始化：将传入的值赋给新分配节点的对应字段（如 ival, fval, sval 等）。
 * 3. 内存管理：部分函数（如 makeFloat, makeString）要求调用者提供已分配好的 palloc 字符串，
 *    这些节点结构将持有该字符串的指针。
 *
 * Copyright (c) 2003-2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/value.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/value.h"

/*
 * =========================================================================
 * 值节点构造函数
 * 这些函数负责将原始 C 数据类型封装为 PostgreSQL 的节点（Node）对象。
 * =========================================================================
 */

/*
 *	makeInteger
 *	构造整数节点
 */
Integer *
makeInteger(int i)
{
	Integer    *v = makeNode(Integer);

	v->ival = i;
	return v;
}

/*
 *	makeFloat
 *	构造浮点数节点
 *
 * Caller is responsible for passing a palloc'd string.
 * 调用者负责传递一个由 palloc 分配的字符串。
 */
Float *
makeFloat(char *numericStr)
{
	Float	   *v = makeNode(Float);

	v->fval = numericStr;
	return v;
}

/*
 *	makeBoolean
 *	构造布尔值节点
 */
Boolean *
makeBoolean(bool val)
{
	Boolean    *v = makeNode(Boolean);

	v->boolval = val;
	return v;
}

/*
 *	makeString
 *	构造字符串节点
 *
 * Caller is responsible for passing a palloc'd string.
 * 调用者负责传递一个由 palloc 分配的字符串。
 */
String *
makeString(char *str)
{
	String	   *v = makeNode(String);

	v->sval = str;
	return v;
}

/*
 *	makeBitString
 *	构造位串节点
 *
 * Caller is responsible for passing a palloc'd string.
 * 调用者负责传递一个由 palloc 分配的字符串。
 */
BitString *
makeBitString(char *str)
{
	BitString  *v = makeNode(BitString);

	v->bsval = str;
	return v;
}
