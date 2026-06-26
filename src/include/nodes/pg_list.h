/*-------------------------------------------------------------------------
 *
 * pg_list.h
 *	  interface for PostgreSQL generic list package
 *	  PostgreSQL 通用列表包的接口
 *
 * Once upon a time, parts of Postgres were written in Lisp and used real
 * cons-cell lists for major data structures.  When that code was rewritten
 * in C, we initially had a faithful emulation of cons-cell lists, which
 * unsurprisingly was a performance bottleneck.  A couple of major rewrites
 * later, these data structures are actually simple expansible arrays;
 * but the "List" name and a lot of the notation survives.
 * 很久以前，Postgres 的一部分是用 Lisp 编写的，并使用真实的 cons-cell 列表作为主要数据结构。
 * 当这些代码用 C 重写时，我们起初忠实地模拟了 cons-cell 列表，不出所料，这成为了性能瓶颈。
 * 经过几次重大重写后，这些数据结构实际上是简单的可扩展数组；但 “List” 的名称和许多记号被保留了下来。
 *
 * One important concession to the original implementation is that an empty
 * list is always represented by a null pointer (preferentially written NIL).
 * Non-empty lists have a header, which will not be relocated as long as the
 * list remains non-empty, and an expansible data array.
 * 对原始实现的一个重要妥协是，空列表始终由空指针表示（优先写作 NIL）。
 * 非空列表具有一个头部（header），只要列表保持非空，该头部就不会被移动，以及一个可扩展的数据数组。
 *
 * We support four types of lists:
 * 我们支持四种类型的列表：
 *
 *	T_List: lists of pointers
 *		(in practice usually pointers to Nodes, but not always;
 *		declared as "void *" to minimize casting annoyances)
 *	T_List: 指针列表
 *		（实践中通常是指向 Node 的指针，但不总是如此；
 *		声明为 "void *" 以减少强制类型转换的烦恼）
 *	T_IntList: lists of integers
 *	T_IntList: 整数列表
 *	T_OidList: lists of Oids
 *	T_OidList: Oid 列表
 *	T_XidList: lists of TransactionIds
 *		(the XidList infrastructure is less complete than the other cases)
 *	T_XidList: TransactionId 列表
 *		（XidList 的基础设施比其他情况不够完善）
 *
 * (At the moment, ints, Oids, and XIDs are the same size, but they may not
 * always be so; be careful to use the appropriate list type for your data.)
 * （目前，int、Oid 和 XID 的大小相同，但以后可能并非总是如此；请务必为你的数据使用适当的列表类型。）
 *
 * Core Flow Explanation / 核心流程说明:
 * - Empty List: Represented by NIL (NULL).
 * - 空列表：由 NIL（NULL）表示。
 * - Allocation: Non-empty lists consist of a List header and an array of ListCells.
 * - 内存分配：非空列表由一个 List 头部和一个 ListCell 数组组成。
 * - Growth: When adding elements beyond current capacity, the elements array is expanded (reallocated).
 * - 增长：当添加的元素超过当前容量时，elements 数组会被扩展（重新分配）。
 * - Iteration: Accomplished mainly through macros like foreach(), which iterate through the array indices.
 * - 遍历：主要通过 foreach() 等宏完成，这些宏通过数组索引进行迭代。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pg_list.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LIST_H
#define PG_LIST_H

#include "nodes/nodes.h"


typedef union ListCell
{
	void	   *ptr_value;
	int			int_value;
	Oid			oid_value;
	TransactionId xid_value;
} ListCell;

typedef struct List
{
	NodeTag		type;			/* T_List, T_IntList, T_OidList, or T_XidList */
	int			length;			/* number of elements currently present */
	/* 当前存在的元素数量 */
	int			max_length;		/* allocated length of elements[] */
	/* elements[] 的已分配长度 */
	ListCell   *elements;		/* re-allocatable array of cells */
	/* 可重新分配单元（cell）数组 */
	/* We may allocate some cells along with the List header: */
	/* 我们可能会随 List 头部一起分配一些单元： */
	ListCell	initial_elements[FLEXIBLE_ARRAY_MEMBER];
	/* If elements == initial_elements, it's not a separate allocation */
	/* 如果 elements == initial_elements，则它不是一个单独的分配 */
} List;

/*
 * The *only* valid representation of an empty list is NIL; in other
 * words, a non-NIL list is guaranteed to have length >= 1.
 * 空列表的*唯一*有效表示是 NIL；换句话说，非 NIL 列表保证长度 >= 1。
 */
#define NIL						((List *) NULL)

/*
 * State structs for various looping macros below.
 * 下面各种循环宏的状态结构体。
 */
typedef struct ForEachState
{
	const List *l;				/* list we're looping through */
	/* 我们正在循环遍历的列表 */
	int			i;				/* current element index */
	/* 当前元素索引 */
} ForEachState;

typedef struct ForBothState
{
	const List *l1;				/* lists we're looping through */
	const List *l2;
	int			i;				/* common element index */
} ForBothState;

typedef struct ForBothCellState
{
	const List *l1;				/* lists we're looping through */
	const List *l2;
	int			i1;				/* current element indexes */
	int			i2;
} ForBothCellState;

typedef struct ForThreeState
{
	const List *l1;				/* lists we're looping through */
	const List *l2;
	const List *l3;
	int			i;				/* common element index */
} ForThreeState;

typedef struct ForFourState
{
	const List *l1;				/* lists we're looping through */
	const List *l2;
	const List *l3;
	const List *l4;
	int			i;				/* common element index */
} ForFourState;

typedef struct ForFiveState
{
	const List *l1;				/* lists we're looping through */
	const List *l2;
	const List *l3;
	const List *l4;
	const List *l5;
	int			i;				/* common element index */
} ForFiveState;

/*
 * These routines are small enough, and used often enough, to justify being
 * inline.
 * 这些例程足够小，且被频繁使用，因此有理由设为内联（inline）。
 */

/* Fetch address of list's first cell; NULL if empty list */
/* 获取列表第一个单元（cell）的地址；如果是空列表则返回 NULL */
static inline ListCell *
list_head(const List *l)
{
	return l ? &l->elements[0] : NULL;
}

/* Fetch address of list's last cell; NULL if empty list */
/* 获取列表最后一个单元的地址；如果是空列表则返回 NULL */
static inline ListCell *
list_tail(const List *l)
{
	return l ? &l->elements[l->length - 1] : NULL;
}

/* Fetch address of list's second cell, if it has one, else NULL */
/* 获取列表第二个单元的地址，如果存在的话，否则返回 NULL */
static inline ListCell *
list_second_cell(const List *l)
{
	if (l && l->length >= 2)
		return &l->elements[1];
	else
		return NULL;
}

/* Fetch list's length */
/* 获取列表的长度 */
static inline int
list_length(const List *l)
{
	return l ? l->length : 0;
}

/*
 * Macros to access the data values within List cells.
 * 用于访问 List 单元内数据值的宏。
 *
 * Note that with the exception of the "xxx_node" macros, these are
 * lvalues and can be assigned to.
 * 请注意，除 “xxx_node” 宏之外，这些宏都是左值（lvalues），可以对其进行赋值。
 *
 * NB: There is an unfortunate legacy from a previous incarnation of
 * the List API: the macro lfirst() was used to mean "the data in this
 * cons cell". To avoid changing every usage of lfirst(), that meaning
 * has been kept. As a result, lfirst() takes a ListCell and returns
 * the data it contains; to get the data in the first cell of a
 * List, use linitial(). Worse, lsecond() is more closely related to
 * linitial() than lfirst(): given a List, lsecond() returns the data
 * in the second list cell.
 * 注意：List API 的早期版本留下了一个遗留问题：宏 lfirst() 的原意是 “此 cons 单元中的数据”。
 * 为了避免更改 lfirst() 的每一处用法，保留了这一含义。
 * 结果是，lfirst() 接收一个 ListCell 并返回它包含的数据；要获取 List 第一个单元中的数据，请使用 linitial()。
 * 更糟的是，lsecond() 与 linitial() 的关系比 lfirst() 更紧密：给定一个 List，lsecond() 返回第二个列表单元中的数据。
 */
#define lfirst(lc)				((lc)->ptr_value)
#define lfirst_int(lc)			((lc)->int_value)
#define lfirst_oid(lc)			((lc)->oid_value)
#define lfirst_xid(lc)			((lc)->xid_value)
#define lfirst_node(type,lc)	castNode(type, lfirst(lc))

#define linitial(l)				lfirst(list_nth_cell(l, 0))
#define linitial_int(l)			lfirst_int(list_nth_cell(l, 0))
#define linitial_oid(l)			lfirst_oid(list_nth_cell(l, 0))
#define linitial_node(type,l)	castNode(type, linitial(l))

#define lsecond(l)				lfirst(list_nth_cell(l, 1))
#define lsecond_int(l)			lfirst_int(list_nth_cell(l, 1))
#define lsecond_oid(l)			lfirst_oid(list_nth_cell(l, 1))
#define lsecond_node(type,l)	castNode(type, lsecond(l))

#define lthird(l)				lfirst(list_nth_cell(l, 2))
#define lthird_int(l)			lfirst_int(list_nth_cell(l, 2))
#define lthird_oid(l)			lfirst_oid(list_nth_cell(l, 2))
#define lthird_node(type,l)		castNode(type, lthird(l))

#define lfourth(l)				lfirst(list_nth_cell(l, 3))
#define lfourth_int(l)			lfirst_int(list_nth_cell(l, 3))
#define lfourth_oid(l)			lfirst_oid(list_nth_cell(l, 3))
#define lfourth_node(type,l)	castNode(type, lfourth(l))

#define llast(l)				lfirst(list_last_cell(l))
#define llast_int(l)			lfirst_int(list_last_cell(l))
#define llast_oid(l)			lfirst_oid(list_last_cell(l))
#define llast_xid(l)			lfirst_xid(list_last_cell(l))
#define llast_node(type,l)		castNode(type, llast(l))

/*
 * Convenience macros for building fixed-length lists
 * 用于构建固定长度列表的便捷宏
 */
#define list_make_ptr_cell(v)	((ListCell) {.ptr_value = (v)})
#define list_make_int_cell(v)	((ListCell) {.int_value = (v)})
#define list_make_oid_cell(v)	((ListCell) {.oid_value = (v)})
#define list_make_xid_cell(v)	((ListCell) {.xid_value = (v)})

#define list_make1(x1) \
	list_make1_impl(T_List, list_make_ptr_cell(x1))
#define list_make2(x1,x2) \
	list_make2_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2))
#define list_make3(x1,x2,x3) \
	list_make3_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2), \
					list_make_ptr_cell(x3))
#define list_make4(x1,x2,x3,x4) \
	list_make4_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2), \
					list_make_ptr_cell(x3), list_make_ptr_cell(x4))
#define list_make5(x1,x2,x3,x4,x5) \
	list_make5_impl(T_List, list_make_ptr_cell(x1), list_make_ptr_cell(x2), \
					list_make_ptr_cell(x3), list_make_ptr_cell(x4), \
					list_make_ptr_cell(x5))

#define list_make1_int(x1) \
	list_make1_impl(T_IntList, list_make_int_cell(x1))
#define list_make2_int(x1,x2) \
	list_make2_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2))
#define list_make3_int(x1,x2,x3) \
	list_make3_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2), \
					list_make_int_cell(x3))
#define list_make4_int(x1,x2,x3,x4) \
	list_make4_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2), \
					list_make_int_cell(x3), list_make_int_cell(x4))
#define list_make5_int(x1,x2,x3,x4,x5) \
	list_make5_impl(T_IntList, list_make_int_cell(x1), list_make_int_cell(x2), \
					list_make_int_cell(x3), list_make_int_cell(x4), \
					list_make_int_cell(x5))

#define list_make1_oid(x1) \
	list_make1_impl(T_OidList, list_make_oid_cell(x1))
#define list_make2_oid(x1,x2) \
	list_make2_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2))
#define list_make3_oid(x1,x2,x3) \
	list_make3_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2), \
					list_make_oid_cell(x3))
#define list_make4_oid(x1,x2,x3,x4) \
	list_make4_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2), \
					list_make_oid_cell(x3), list_make_oid_cell(x4))
#define list_make5_oid(x1,x2,x3,x4,x5) \
	list_make5_impl(T_OidList, list_make_oid_cell(x1), list_make_oid_cell(x2), \
					list_make_oid_cell(x3), list_make_oid_cell(x4), \
					list_make_oid_cell(x5))

#define list_make1_xid(x1) \
	list_make1_impl(T_XidList, list_make_xid_cell(x1))
#define list_make2_xid(x1,x2) \
	list_make2_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2))
#define list_make3_xid(x1,x2,x3) \
	list_make3_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2), \
					list_make_xid_cell(x3))
#define list_make4_xid(x1,x2,x3,x4) \
	list_make4_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2), \
					list_make_xid_cell(x3), list_make_xid_cell(x4))
#define list_make5_xid(x1,x2,x3,x4,x5) \
	list_make5_impl(T_XidList, list_make_xid_cell(x1), list_make_xid_cell(x2), \
					list_make_xid_cell(x3), list_make_xid_cell(x4), \
					list_make_xid_cell(x5))

/*
 * Locate the n'th cell (counting from 0) of the list.
 * It is an assertion failure if there is no such cell.
 * 定位列表的第 n 个单元（从 0 开始计数）。如果不存在该单元，则断言失败。
 */
static inline ListCell *
list_nth_cell(const List *list, int n)
{
	Assert(list != NIL);
	Assert(n >= 0 && n < list->length);
	return &list->elements[n];
}

/*
 * Return the last cell in a non-NIL List.
 * 返回非 NIL 列表中的最后一个单元。
 */
static inline ListCell *
list_last_cell(const List *list)
{
	Assert(list != NIL);
	return &list->elements[list->length - 1];
}

/*
 * Return the pointer value contained in the n'th element of the
 * specified list. (List elements begin at 0.)
 * 返回指定列表第 n 个元素中包含的指针值。（列表元素从 0 开始。）
 */
static inline void *
list_nth(const List *list, int n)
{
	Assert(IsA(list, List));
	return lfirst(list_nth_cell(list, n));
}

/*
 * Return the integer value contained in the n'th element of the
 * specified list.
 * 返回指定列表第 n 个元素中包含的整数值。
 */
static inline int
list_nth_int(const List *list, int n)
{
	Assert(IsA(list, IntList));
	return lfirst_int(list_nth_cell(list, n));
}

/*
 * Return the OID value contained in the n'th element of the specified
 * list.
 * 返回指定列表第 n 个元素中包含的 OID 值。
 */
static inline Oid
list_nth_oid(const List *list, int n)
{
	Assert(IsA(list, OidList));
	return lfirst_oid(list_nth_cell(list, n));
}

#define list_nth_node(type,list,n)	castNode(type, list_nth(list, n))

/*
 * Get the given ListCell's index (from 0) in the given List.
 * 获取给定 ListCell 在给定 List 中的索引（从 0 开始）。
 */
static inline int
list_cell_number(const List *l, const ListCell *c)
{
	Assert(c >= &l->elements[0] && c < &l->elements[l->length]);
	return c - l->elements;
}

/*
 * Get the address of the next cell after "c" within list "l", or NULL if none.
 * 获取列表 “l” 中 “c” 之后下一个单元的地址，如果没有则返回 NULL。
 */
static inline ListCell *
lnext(const List *l, const ListCell *c)
{
	Assert(c >= &l->elements[0] && c < &l->elements[l->length]);
	c++;
	if (c < &l->elements[l->length])
		return (ListCell *) c;
	else
		return NULL;
}

/*
 * foreach -
 *	  a convenience macro for looping through a list
 *    遍历列表的便捷宏
 *
 * "cell" must be the name of a "ListCell *" variable; it's made to point
 * to each List element in turn.  "cell" will be NULL after normal exit from
 * the loop, but an early "break" will leave it pointing at the current
 * List element.
 * “cell” 必须是一个 “ListCell *” 变量的名称；它被设置为依次指向每个 List 元素。
 * 在正常退出循环后，“cell” 将为 NULL，但提前 “break” 将使其指向当前的 List 元素。
 *
 * Beware of changing the List object while the loop is iterating.
 * The current semantics are that we examine successive list indices in
 * each iteration, so that insertion or deletion of list elements could
 * cause elements to be re-visited or skipped unexpectedly.  Previous
 * implementations of foreach() behaved differently.  However, it's safe
 * to append elements to the List (or in general, insert them after the
 * current element); such new elements are guaranteed to be visited.
 * Also, the current element of the List can be deleted, if you use
 * foreach_delete_current() to do so.  BUT: either of these actions will
 * invalidate the "cell" pointer for the remainder of the current iteration.
 * 注意在循环迭代期间更改 List 对象。
 * 当前的语义是我们在每次迭代中检查连续的列表索引，因此插入或删除列表元素可能会导致元素被意外地重复访问或跳过。
 * foreach() 以前的实现行为有所不同。然而，向 List 追加元素（或者通常来说，在当前元素之后插入元素）是安全的；
 * 这种新元素保证会被访问到。此外，如果你使用 foreach_delete_current() 也可以删除 List 的当前元素。
 * 但是：这两种操作都会使当前迭代剩余部分的 “cell” 指针失效。
 */
#define foreach(cell, lst)	\
	for (ForEachState cell##__state = {(lst), 0}; \
		 (cell##__state.l != NIL && \
		  cell##__state.i < cell##__state.l->length) ? \
		 (cell = &cell##__state.l->elements[cell##__state.i], true) : \
		 (cell = NULL, false); \
		 cell##__state.i++)

/*
 * foreach_delete_current -
 *	  delete the current list element from the List associated with a
 *	  surrounding foreach() or foreach_*() loop, returning the new List
 *	  pointer; pass the name of the iterator variable.
 *    从周围的 foreach() 或 foreach_*() 循环关联的 List 中删除当前列表元素，并返回新的 List 指针；传递迭代器变量的名称。
 *
 * This is similar to list_delete_cell(), but it also adjusts the loop's state
 * so that no list elements will be missed.  Do not delete elements from an
 * active foreach or foreach_* loop's list in any other way!
 * 这类似于 list_delete_cell()，但它还会调整循环的状态，以便不会遗漏任何列表元素。
 * 请勿以任何其他方式从活动的 foreach 或 foreach_* 循环列表中删除元素！
 */
#define foreach_delete_current(lst, var_or_cell)	\
	((List *) (var_or_cell##__state.l = list_delete_nth_cell(lst, var_or_cell##__state.i--)))

/*
 * foreach_current_index -
 *	  get the zero-based list index of a surrounding foreach() or foreach_*()
 *	  loop's current element; pass the name of the iterator variable.
 *    获取周围 foreach() 或 foreach_*() 循环当前元素的从零开始的列表索引；传递迭代器变量的名称。
 *
 * Beware of using this after foreach_delete_current(); the value will be
 * out of sync for the rest of the current loop iteration.  Anyway, since
 * you just deleted the current element, the value is pretty meaningless.
 * 注意在 foreach_delete_current() 之后使用此宏；该值在当前循环迭代的剩余部分将不同步。
 * 无论如何，既然你刚刚删除了当前元素，这个值已经没多大意义了。
 */
#define foreach_current_index(var_or_cell)  (var_or_cell##__state.i)

/*
 * for_each_from -
 *	  Like foreach(), but start from the N'th (zero-based) list element,
 *	  not necessarily the first one.
 *    类似于 foreach()，但从第 N 个（从零开始）列表元素开始，而不一定是第一个。
 *
 * It's okay for N to exceed the list length, but not for it to be negative.
 * N 可以超过列表长度，但不能为负数。
 *
 * The caveats for foreach() apply equally here.
 * foreach() 的注意事项同样适用于此处。
 */
#define for_each_from(cell, lst, N)	\
	for (ForEachState cell##__state = for_each_from_setup(lst, N); \
		 (cell##__state.l != NIL && \
		  cell##__state.i < cell##__state.l->length) ? \
		 (cell = &cell##__state.l->elements[cell##__state.i], true) : \
		 (cell = NULL, false); \
		 cell##__state.i++)

static inline ForEachState
for_each_from_setup(const List *lst, int N)
{
	ForEachState r = {lst, N};

	Assert(N >= 0);
	return r;
}

/*
 * for_each_cell -
 *	  a convenience macro which loops through a list starting from a
 *	  specified cell
 *    一个从指定单元开始遍历列表的便捷宏
 *
 * The caveats for foreach() apply equally here.
 * foreach() 的注意事项同样适用于此处。
 */
#define for_each_cell(cell, lst, initcell)	\
	for (ForEachState cell##__state = for_each_cell_setup(lst, initcell); \
		 (cell##__state.l != NIL && \
		  cell##__state.i < cell##__state.l->length) ? \
		 (cell = &cell##__state.l->elements[cell##__state.i], true) : \
		 (cell = NULL, false); \
		 cell##__state.i++)

static inline ForEachState
for_each_cell_setup(const List *lst, const ListCell *initcell)
{
	ForEachState r = {lst,
	initcell ? list_cell_number(lst, initcell) : list_length(lst)};

	return r;
}

/*
 * Convenience macros that loop through a list without needing a separate
 * "ListCell *" variable.  Instead, the macros declare a locally-scoped loop
 * variable with the provided name and the appropriate type.
 * 无需单独的 “ListCell *” 变量即可遍历列表的便捷宏。相反，这些宏使用提供的名称和适当的类型声明一个局部作用域的循环变量。
 *
 * Since the variable is scoped to the loop, it's not possible to detect an
 * early break by checking its value after the loop completes, as is common
 * practice.  If you need to do this, you can either use foreach() instead or
 * manually track early breaks with a separate variable declared outside of the
 * loop.
 * 由于变量的作用域仅限于循环，因此无法像通常做法那样在循环完成后通过检查其值来检测提前 break。
 * 如果你需要这样做，可以改用 foreach()，或者使用在循环之外声明的单独变量来手动跟踪提前 break。
 *
 * Note that the caveats described in the comment above the foreach() macro
 * also apply to these convenience macros.
 * 请注意，foreach() 宏上方的注释中所述的注意事项也适用于这些便捷宏。
 */
#define foreach_ptr(type, var, lst) foreach_internal(type, *, var, lst, lfirst)
#define foreach_int(var, lst)	foreach_internal(int, , var, lst, lfirst_int)
#define foreach_oid(var, lst)	foreach_internal(Oid, , var, lst, lfirst_oid)
#define foreach_xid(var, lst)	foreach_internal(TransactionId, , var, lst, lfirst_xid)

/*
 * The internal implementation of the above macros.  Do not use directly.
 * 上述宏的内部实现。请勿直接使用。
 *
 * This macro actually generates two loops in order to declare two variables of
 * different types.  The outer loop only iterates once, so we expect optimizing
 * compilers will unroll it, thereby optimizing it away.
 * 此宏实际上生成了两个循环，以便声明两个不同类型的变量。
 * 外层循环仅迭代一次，因此我们期望优化编译器会将其展开，从而将其优化掉。
 */
#define foreach_internal(type, pointer, var, lst, func) \
	for (type pointer var = 0, pointer var##__outerloop = (type pointer) 1; \
		 var##__outerloop; \
		 var##__outerloop = 0) \
		for (ForEachState var##__state = {(lst), 0}; \
			 (var##__state.l != NIL && \
			  var##__state.i < var##__state.l->length && \
			 (var = (type pointer) func(&var##__state.l->elements[var##__state.i]), true)); \
			 var##__state.i++)

/*
 * foreach_node -
 *	  The same as foreach_ptr, but asserts that the element is of the specified
 *	  node type.
 *    与 foreach_ptr 相同，但断言元素属于指定的节点类型。
 */
#define foreach_node(type, var, lst) \
	for (type * var = 0, *var##__outerloop = (type *) 1; \
		 var##__outerloop; \
		 var##__outerloop = 0) \
		for (ForEachState var##__state = {(lst), 0}; \
			 (var##__state.l != NIL && \
			  var##__state.i < var##__state.l->length && \
			 (var = lfirst_node(type, &var##__state.l->elements[var##__state.i]), true)); \
			 var##__state.i++)

/*
 * forboth -
 *	  a convenience macro for advancing through two linked lists
 *	  simultaneously. This macro loops through both lists at the same
 *	  time, stopping when either list runs out of elements. Depending
 *	  on the requirements of the call site, it may also be wise to
 *	  assert that the lengths of the two lists are equal. (But, if they
 *	  are not, some callers rely on the ending cell values being separately
 *	  NULL or non-NULL as defined here; don't try to optimize that.)
 *    同时遍历两个链表的便捷宏。此宏同时循环遍历两个列表，当任一列表元素耗尽时停止。
 *    根据调用点的要求，断言两个列表的长度相等可能是明智的。（但是，如果长度不等，
 *    某些调用者依赖于此处定义的结束单元值分别为 NULL 或非 NULL；不要尝试优化这一点。）
 *
 * The caveats for foreach() apply equally here.
 * foreach() 的注意事项同样适用于此处。
 */
#define forboth(cell1, list1, cell2, list2)							\
	for (ForBothState cell1##__state = {(list1), (list2), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 (cell1 != NULL && cell2 != NULL); \
		 cell1##__state.i++)

#define multi_for_advance_cell(cell, state, l, i) \
	(cell = (state.l != NIL && state.i < state.l->length) ? \
	 &state.l->elements[state.i] : NULL)

/*
 * for_both_cell -
 *	  a convenience macro which loops through two lists starting from the
 *	  specified cells of each. This macro loops through both lists at the same
 *	  time, stopping when either list runs out of elements.  Depending on the
 *	  requirements of the call site, it may also be wise to assert that the
 *	  lengths of the two lists are equal, and initcell1 and initcell2 are at
 *	  the same position in the respective lists.
 *    一个从每个列表的指定单元开始，同时遍历两个列表的便捷宏。
 *    此宏同时循环遍历两个列表，当任一列表元素耗尽时停止。
 *    根据调用点的要求，断言两个列表的长度相等，且 initcell1 和 initcell2 在各自列表中的位置相同，可能是明智的。
 *
 * The caveats for foreach() apply equally here.
 * foreach() 的注意事项同样适用于此处。
 */
#define for_both_cell(cell1, list1, initcell1, cell2, list2, initcell2)	\
	for (ForBothCellState cell1##__state = \
			 for_both_cell_setup(list1, initcell1, list2, initcell2); \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i1), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i2), \
		 (cell1 != NULL && cell2 != NULL); \
		 cell1##__state.i1++, cell1##__state.i2++)

static inline ForBothCellState
for_both_cell_setup(const List *list1, const ListCell *initcell1,
					const List *list2, const ListCell *initcell2)
{
	ForBothCellState r = {list1, list2,
		initcell1 ? list_cell_number(list1, initcell1) : list_length(list1),
	initcell2 ? list_cell_number(list2, initcell2) : list_length(list2)};

	return r;
}

/*
 * forthree -
 *	  the same for three lists
 *    对三个列表执行相同操作
 */
#define forthree(cell1, list1, cell2, list2, cell3, list3) \
	for (ForThreeState cell1##__state = {(list1), (list2), (list3), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 multi_for_advance_cell(cell3, cell1##__state, l3, i), \
		 (cell1 != NULL && cell2 != NULL && cell3 != NULL); \
		 cell1##__state.i++)

/*
 * forfour -
 *	  the same for four lists
 *    对四个列表执行相同操作
 */
#define forfour(cell1, list1, cell2, list2, cell3, list3, cell4, list4) \
	for (ForFourState cell1##__state = {(list1), (list2), (list3), (list4), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 multi_for_advance_cell(cell3, cell1##__state, l3, i), \
		 multi_for_advance_cell(cell4, cell1##__state, l4, i), \
		 (cell1 != NULL && cell2 != NULL && cell3 != NULL && cell4 != NULL); \
		 cell1##__state.i++)

/*
 * forfive -
 *	  the same for five lists
 *    对五个列表执行相同操作
 */
#define forfive(cell1, list1, cell2, list2, cell3, list3, cell4, list4, cell5, list5) \
	for (ForFiveState cell1##__state = {(list1), (list2), (list3), (list4), (list5), 0}; \
		 multi_for_advance_cell(cell1, cell1##__state, l1, i), \
		 multi_for_advance_cell(cell2, cell1##__state, l2, i), \
		 multi_for_advance_cell(cell3, cell1##__state, l3, i), \
		 multi_for_advance_cell(cell4, cell1##__state, l4, i), \
		 multi_for_advance_cell(cell5, cell1##__state, l5, i), \
		 (cell1 != NULL && cell2 != NULL && cell3 != NULL && \
		  cell4 != NULL && cell5 != NULL); \
		 cell1##__state.i++)

/* Functions in src/backend/nodes/list.c */
/* src/backend/nodes/list.c 中的函数 */

extern List *list_make1_impl(NodeTag t, ListCell datum1);
extern List *list_make2_impl(NodeTag t, ListCell datum1, ListCell datum2);
extern List *list_make3_impl(NodeTag t, ListCell datum1, ListCell datum2,
							 ListCell datum3);
extern List *list_make4_impl(NodeTag t, ListCell datum1, ListCell datum2,
							 ListCell datum3, ListCell datum4);
extern List *list_make5_impl(NodeTag t, ListCell datum1, ListCell datum2,
							 ListCell datum3, ListCell datum4,
							 ListCell datum5);

pg_nodiscard extern List *lappend(List *list, void *datum);
pg_nodiscard extern List *lappend_int(List *list, int datum);
pg_nodiscard extern List *lappend_oid(List *list, Oid datum);
pg_nodiscard extern List *lappend_xid(List *list, TransactionId datum);
/* lappend: 向列表末尾追加一个元素。 */

pg_nodiscard extern List *list_insert_nth(List *list, int pos, void *datum);
pg_nodiscard extern List *list_insert_nth_int(List *list, int pos, int datum);
pg_nodiscard extern List *list_insert_nth_oid(List *list, int pos, Oid datum);
/* list_insert_nth: 在指定位置插入一个元素。 */

pg_nodiscard extern List *lcons(void *datum, List *list);
pg_nodiscard extern List *lcons_int(int datum, List *list);
pg_nodiscard extern List *lcons_oid(Oid datum, List *list);
/* lcons: 在列表开头插入一个元素。 */

pg_nodiscard extern List *list_concat(List *list1, const List *list2);
pg_nodiscard extern List *list_concat_copy(const List *list1, const List *list2);
/* list_concat: 连接两个列表。 */

pg_nodiscard extern List *list_truncate(List *list, int new_size);
/* list_truncate: 截断列表到指定大小。 */

extern bool list_member(const List *list, const void *datum);
extern bool list_member_ptr(const List *list, const void *datum);
extern bool list_member_int(const List *list, int datum);
extern bool list_member_oid(const List *list, Oid datum);
extern bool list_member_xid(const List *list, TransactionId datum);
/* list_member: 检查元素是否在列表中。 */

pg_nodiscard extern List *list_delete(List *list, void *datum);
pg_nodiscard extern List *list_delete_ptr(List *list, void *datum);
pg_nodiscard extern List *list_delete_int(List *list, int datum);
pg_nodiscard extern List *list_delete_oid(List *list, Oid datum);
pg_nodiscard extern List *list_delete_first(List *list);
pg_nodiscard extern List *list_delete_last(List *list);
pg_nodiscard extern List *list_delete_first_n(List *list, int n);
pg_nodiscard extern List *list_delete_nth_cell(List *list, int n);
pg_nodiscard extern List *list_delete_cell(List *list, ListCell *cell);
/* list_delete: 从列表中删除指定元素或单元。 */

extern List *list_union(const List *list1, const List *list2);
extern List *list_union_ptr(const List *list1, const List *list2);
extern List *list_union_int(const List *list1, const List *list2);
extern List *list_union_oid(const List *list1, const List *list2);
/* list_union: 返回两个列表的并集。 */

extern List *list_intersection(const List *list1, const List *list2);
extern List *list_intersection_int(const List *list1, const List *list2);
/* list_intersection: 返回两个列表的交集。 */

/* currently, there's no need for list_intersection_ptr etc */
/* 目前不需要 list_intersection_ptr 等 */

extern List *list_difference(const List *list1, const List *list2);
extern List *list_difference_ptr(const List *list1, const List *list2);
extern List *list_difference_int(const List *list1, const List *list2);
extern List *list_difference_oid(const List *list1, const List *list2);
/* list_difference: 返回两个列表的差集（list1 - list2）。 */

pg_nodiscard extern List *list_append_unique(List *list, void *datum);
pg_nodiscard extern List *list_append_unique_ptr(List *list, void *datum);
pg_nodiscard extern List *list_append_unique_int(List *list, int datum);
pg_nodiscard extern List *list_append_unique_oid(List *list, Oid datum);
/* list_append_unique: 如果元素尚不存在，则将其追加到列表末尾。 */

pg_nodiscard extern List *list_concat_unique(List *list1, const List *list2);
pg_nodiscard extern List *list_concat_unique_ptr(List *list1, const List *list2);
pg_nodiscard extern List *list_concat_unique_int(List *list1, const List *list2);
pg_nodiscard extern List *list_concat_unique_oid(List *list1, const List *list2);
/* list_concat_unique: 连接两个列表，并确保结果中元素唯一。 */

extern void list_deduplicate_oid(List *list);
/* list_deduplicate_oid: 原地移除 OidList 中的重复项（通常需要先排序）。 */

extern void list_free(List *list);
extern void list_free_deep(List *list);
/* list_free: 释放列表头部和元素数组。list_free_deep 还会释放指针列表中的所有元素。 */

pg_nodiscard extern List *list_copy(const List *oldlist);
pg_nodiscard extern List *list_copy_head(const List *oldlist, int len);
pg_nodiscard extern List *list_copy_tail(const List *oldlist, int nskip);
pg_nodiscard extern List *list_copy_deep(const List *oldlist);
/* list_copy: 返回列表的浅拷贝或深拷贝。 */

typedef int (*list_sort_comparator) (const ListCell *a, const ListCell *b);
extern void list_sort(List *list, list_sort_comparator cmp);
/* list_sort: 使用指定的比较器对列表进行原地排序。 */

extern int	list_int_cmp(const ListCell *p1, const ListCell *p2);
extern int	list_oid_cmp(const ListCell *p1, const ListCell *p2);
/* 通用的整数和 Oid 比较器。 */

#endif							/* PG_LIST_H */
