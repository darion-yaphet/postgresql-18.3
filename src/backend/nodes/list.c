/*-------------------------------------------------------------------------
 *
 * list.c
 *	  implementation for PostgreSQL generic list package
 *	  PostgreSQL 通用列表包的实现
 *
 * See comments in pg_list.h.
 * 参见 pg_list.h 中的注释。
 *
 *
 * 实现核心流程概述：
 * PostgreSQL 的 List 实现经历了一次重大的重构，从传统的双向或单向链表（cons cells）
 * 转向了基于数组（Array-based）的动态分配。主要流程如下：
 *
 * 1. 结构与存储：
 *    List 现在是一个包含标头（header）和元素数组（elements array）的连续内存块。
 *    为了优化性能，小型的 List 在分配标头时会直接包含一小块初始空间（initial_elements）。
 *
 * 2. 分配与扩容：
 *    - new_list()负责基础分配，倾向于使总分配量为 2 的幂以优化内存利用率。
 *    - 当元素数量超过当前 max_length 时，enlarge_list() 会触发扩容。
 *    - 一旦超出初始空间，elements 数组将切换到单独的 palloc 内存块。
 *
 * 3. 类型安全：
 *    List 支持指针、整数、OID 和事务 ID 四种类型，每种类型都有专门的 lappend/lcons 接口。
 *
 * 4. 插入与删除：
 *    - 插入和删除操作通过 memmove 维护数组的连续性，复杂度为 O(N)。
 *    - 对于大量元素的队列操作，建议谨慎使用，因为其性能不如真正的双端队列。
 *
 * 5. 调试支持：
 *    通过定义 DEBUG_LIST_MEMORY_USAGE，系统可以强制每次增删都移动内存，
 *    以帮助发现不安全地持有 ListCell 指针的代码段。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/list.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "nodes/pg_list.h"
#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


/*
 * The previous List implementation, since it used a separate palloc chunk
 * for each cons cell, had the property that adding or deleting list cells
 * did not move the storage of other existing cells in the list.  Quite a
 * bit of existing code depended on that, by retaining ListCell pointers
 * across such operations on a list.  There is no such guarantee in this
 * implementation, so instead we have debugging support that is meant to
 * help flush out now-broken assumptions.  Defining DEBUG_LIST_MEMORY_USAGE
 * while building this file causes the List operations to forcibly move
 * all cells in a list whenever a cell is added or deleted.  In combination
 * with MEMORY_CONTEXT_CHECKING and/or Valgrind, this can usually expose
 * broken code.  It's a bit expensive though, as there's many more palloc
 * cycles and a lot more data-copying than in a default build.
 * 之前的 List 实现由于为每个 cons 单元使用单独的 palloc 块，
 * 具有添加或删除列表单元不会移动列表中其他现有单元存储空间的属性。
 * 相当多现有代码依赖于这一点，即在对列表执行此类操作时保留 ListCell 指针。
 * 在此实现中没有这样的保证，因此我们提供了调试支持，旨在帮助清除现在已损坏的假设。
 * 在构建此文件时定义 DEBUG_LIST_MEMORY_USAGE 会导致每当添加或删除单元时，
 * List 操作都会强制移动列表中的所有单元。与 MEMORY_CONTEXT_CHECKING 和/或 Valgrind 结合使用，
 * 这通常可以暴露损坏的代码。不过这有点昂贵，因为与默认构建相比，会有更多的 palloc 周期和更多的数据复制。
 *
 * By default, we enable this when building for Valgrind.
 * 默认情况下，我们在为 Valgrind 构建时启用此功能。
 */
#ifdef USE_VALGRIND
#define DEBUG_LIST_MEMORY_USAGE
#endif

/* Overhead for the fixed part of a List header, measured in ListCells
 * List 标头固定部分的开销，以 ListCells 为单位测量 */
#define LIST_HEADER_OVERHEAD  \
	((int) ((offsetof(List, initial_elements) - 1) / sizeof(ListCell) + 1))

/*
 * Macros to simplify writing assertions about the type of a list; a
 * NIL list is considered to be an empty list of any type.
 * 用于简化编写有关列表类型的断言的宏；NIL 列表被认为是任何类型的空列表。
 */
#define IsPointerList(l)		((l) == NIL || IsA((l), List))
#define IsIntegerList(l)		((l) == NIL || IsA((l), IntList))
#define IsOidList(l)			((l) == NIL || IsA((l), OidList))
#define IsXidList(l)			((l) == NIL || IsA((l), XidList))

#ifdef USE_ASSERT_CHECKING
/*
 * Check that the specified List is valid (so far as we can tell).
 * 检查指定的 List 是否有效（据我们所知）。
 */
static void
check_list_invariants(const List *list)
{
	if (list == NIL)
		return;

	Assert(list->length > 0);
	Assert(list->length <= list->max_length);
	Assert(list->elements != NULL);

	Assert(list->type == T_List ||
		   list->type == T_IntList ||
		   list->type == T_OidList ||
		   list->type == T_XidList);
}
#else
#define check_list_invariants(l)  ((void) 0)
#endif							/* USE_ASSERT_CHECKING */

/*
 * =========================================================================
 * 1. 内部辅助函数：内存管理与扩容
 * 这些内部静态函数处理 List 结构的初始分配、内存布局以及动态扩容逻辑。
 * =========================================================================
 */

/*
 * Return a freshly allocated List with room for at least min_size cells.
 * 返回一个新分配的 List，其中至少有 min_size 个单元的空间。
 *
 * Since empty non-NIL lists are invalid, new_list() sets the initial length
 * to min_size, effectively marking that number of cells as valid; the caller
 * is responsible for filling in their data.
 * 由于空的非 NIL 列表是无效的，new_list() 将初始长度设置为 min_size，
 * 从而有效地将该数量的单元标记为有效；调用者负责填写数据。
 */
static List *
new_list(NodeTag type, int min_size)
{
	List	   *newlist;
	int			max_size;

	Assert(min_size > 0);

	/*
	 * We allocate all the requested cells, and possibly some more, as part of
	 * the same palloc request as the List header.  This is a big win for the
	 * typical case of short fixed-length lists.  It can lose if we allocate a
	 * moderately long list and then it gets extended; we'll be wasting more
	 * initial_elements[] space than if we'd made the header small.  However,
	 * rounding up the request as we do in the normal code path provides some
	 * defense against small extensions.
	 * 我们将所有请求的单元以及可能更多的单元作为与 List 标头相同的 palloc 请求的一部分进行分配。
	 * 对于短固定长度列表的典型情况，这是一个巨大的成功。如果我们分配一个中等长度的列表，
	 * 然后对其进行扩展，它可能会失败；与我们将标头变小相比，我们将浪费更多的 initial_elements[] 空间。
	 * 然而，像我们在正常代码路径中那样对请求进行向上取整，可以为小型扩展提供一些防御。
	 */

#ifndef DEBUG_LIST_MEMORY_USAGE

	/*
	 * Normally, we set up a list with some extra cells, to allow it to grow
	 * without a repalloc.  Prefer cell counts chosen to make the total
	 * allocation a power-of-2, since palloc would round it up to that anyway.
	 * (That stops being true for very large allocations, but very long lists
	 * are infrequent, so it doesn't seem worth special logic for such cases.)
	 * 通常，我们设置一个带有一些额外单元的列表，以允许它在没有 repalloc 的情况下增长。
	 * 优先选择使总分配量为 2 的幂的单元数，因为 palloc 无论如何都会将其向上取整为该值。
	 * （对于非常大的分配，这不再成立，但非常长的列表并不常见，因此对于此类情况似乎不值得使用特殊逻辑。）
	 *
	 * The minimum allocation is 8 ListCell units, providing either 4 or 5
	 * available ListCells depending on the machine's word width.  Counting
	 * palloc's overhead, this uses the same amount of space as a one-cell
	 * list did in the old implementation, and less space for any longer list.
	 * 最小分配量为 8 个 ListCell 单元，根据机器的字宽提供 4 或 5 个可用的 ListCell。
	 * 计算 palloc 的开销，这与旧实现中单单元列表使用的空间量相同，而对于任何更长的列表，其空间更少。
	 *
	 * We needn't worry about integer overflow; no caller passes min_size
	 * that's more than twice the size of an existing list, so the size limits
	 * within palloc will ensure that we don't overflow here.
	 * 我们无需担心整数溢出；没有调用者传递超过现有列表大小两倍的 min_size，
	 * 因此 palloc 内的大小限制将确保我们在此处不会溢出。
	 */
	max_size = pg_nextpower2_32(Max(8, min_size + LIST_HEADER_OVERHEAD));
	max_size -= LIST_HEADER_OVERHEAD;
#else

	/*
	 * For debugging, don't allow any extra space.  This forces any cell
	 * addition to go through enlarge_list() and thus move the existing data.
	 * 为了调试，不允许任何额外空间。这强制任何单元添加都必须通过 enlarge_list()，从而移动现有数据。
	 */
	max_size = min_size;
#endif

	newlist = (List *) palloc(offsetof(List, initial_elements) +
							  max_size * sizeof(ListCell));
	newlist->type = type;
	newlist->length = min_size;
	newlist->max_length = max_size;
	newlist->elements = newlist->initial_elements;

	return newlist;
}

/*
 * Enlarge an existing non-NIL List to have room for at least min_size cells.
 * 扩大现有的非 NIL 列表，使其至少有 min_size 个单元的空间。
 *
 * This does *not* update list->length, as some callers would find that
 * inconvenient.  (list->length had better be the correct number of existing
 * valid cells, though.)
 * 这 *不会* 更新 list->length，因为有些调用者可能会觉得这不方便。
 *（不过，list->length 最好是现有有效单元的正确数量。）
 */
static void
enlarge_list(List *list, int min_size)
{
	int			new_max_len;

	Assert(min_size > list->max_length);	/* else we shouldn't be here */

#ifndef DEBUG_LIST_MEMORY_USAGE

	/*
	 * As above, we prefer power-of-two total allocations; but here we need
	 * not account for list header overhead.
	 * 如上所述，我们更喜欢 2 的幂的总分配量；但这里我们无需考虑列表标头开销。
	 */

	/* clamp the minimum value to 16, a semi-arbitrary small power of 2
	 * 将最小值限制为 16，这是一个半任意的小 2 的幂 */
	new_max_len = pg_nextpower2_32(Max(16, min_size));

#else
	/* As above, don't allocate anything extra
	 * 如上所述，不要分配任何额外的东西 */
	new_max_len = min_size;
#endif

	if (list->elements == list->initial_elements)
	{
		/*
		 * Replace original in-line allocation with a separate palloc block.
		 * Ensure it is in the same memory context as the List header.  (The
		 * previous List implementation did not offer any guarantees about
		 * keeping all list cells in the same context, but it seems reasonable
		 * to create such a guarantee now.)
		 * 用单独的 palloc 块替换原始内联分配。确保它与 List 标头位于相同的内存上下文中。
		 *（之前的 List 实现并未提供有关将所有列表单元保持在相同上下文中的任何保证，但现在创建这样的保证似乎是合理的。）
		 */
		list->elements = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   new_max_len * sizeof(ListCell));
		memcpy(list->elements, list->initial_elements,
			   list->length * sizeof(ListCell));

		/*
		 * We must not move the list header, so it's unsafe to try to reclaim
		 * the initial_elements[] space via repalloc.  In debugging builds,
		 * however, we can clear that space and/or mark it inaccessible.
		 * (wipe_mem includes VALGRIND_MAKE_MEM_NOACCESS.)
		 * 我们绝不能移动列表标头，因此尝试通过 repalloc 回收 initial_elements[] 空间是不安全的。
		 * 然而，在调试构建中，我们可以清除该空间和/或将其标记为不可访问。
		 *（wipe_mem 包括 VALGRIND_MAKE_MEM_NOACCESS。）
		 */
#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(list->initial_elements,
				 list->max_length * sizeof(ListCell));
#else
		VALGRIND_MAKE_MEM_NOACCESS(list->initial_elements,
								   list->max_length * sizeof(ListCell));
#endif
	}
	else
	{
#ifndef DEBUG_LIST_MEMORY_USAGE
		/* Normally, let repalloc deal with enlargement
		 * 通常情况下，让 repalloc 处理扩容 */
		list->elements = (ListCell *) repalloc(list->elements,
											   new_max_len * sizeof(ListCell));
#else
		/*
		 * repalloc() might enlarge the space in-place, which we don't want
		 * for debugging purposes, so forcibly move the data somewhere else.
		 * repalloc() 可能会就地扩大空间，出于调试目的我们不希望这样做，因此强制将数据移动到其他地方。
		 */
		ListCell   *newelements;

		newelements = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   new_max_len * sizeof(ListCell));
		memcpy(newelements, list->elements,
			   list->length * sizeof(ListCell));
		pfree(list->elements);
		list->elements = newelements;
#endif
	}

	list->max_length = new_max_len;
}

/*
 * =========================================================================
 * 2. 列表构建便捷函数
 * 提供了一组简单接口，用于快速创建一个包含 1 到 5 个预定义成员的列表。
 * =========================================================================
 */

/*
 * Convenience functions to construct short Lists from given values.
 * (These are normally invoked via the list_makeN macros.)
 * 从给定值构造短列表的便捷函数。（这些通常通过 list_makeN 宏调用。）
 */
List *
list_make1_impl(NodeTag t, ListCell datum1)
{
	List	   *list = new_list(t, 1);

	list->elements[0] = datum1;
	check_list_invariants(list);
	return list;
}

List *
list_make2_impl(NodeTag t, ListCell datum1, ListCell datum2)
{
	List	   *list = new_list(t, 2);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	check_list_invariants(list);
	return list;
}

List *
list_make3_impl(NodeTag t, ListCell datum1, ListCell datum2,
				ListCell datum3)
{
	List	   *list = new_list(t, 3);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	list->elements[2] = datum3;
	check_list_invariants(list);
	return list;
}

List *
list_make4_impl(NodeTag t, ListCell datum1, ListCell datum2,
				ListCell datum3, ListCell datum4)
{
	List	   *list = new_list(t, 4);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	list->elements[2] = datum3;
	list->elements[3] = datum4;
	check_list_invariants(list);
	return list;
}

List *
list_make5_impl(NodeTag t, ListCell datum1, ListCell datum2,
				ListCell datum3, ListCell datum4, ListCell datum5)
{
	List	   *list = new_list(t, 5);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	list->elements[2] = datum3;
	list->elements[3] = datum4;
	list->elements[4] = datum5;
	check_list_invariants(list);
	return list;
}

/*
 * Make room for a new head cell in the given (non-NIL) list.
 * 在给定的（非 NIL）列表中为新的头部单元腾出空间。
 *
 * The data in the new head cell is undefined; the caller should be
 * sure to fill it in
 * 新头部单元中的数据未定义；调用者应确保填入数据。
 */
static void
new_head_cell(List *list)
{
	/* Enlarge array if necessary
	 * 如有必要，扩大数组 */
	if (list->length >= list->max_length)
		enlarge_list(list, list->length + 1);
	/* Now shove the existing data over
	 * 现在把现有的数据推开 */
	memmove(&list->elements[1], &list->elements[0],
			list->length * sizeof(ListCell));
	list->length++;
}

/*
 * Make room for a new tail cell in the given (non-NIL) list.
 * 在给定的（非 NIL）列表中为新的尾部单元腾出空间。
 *
 * The data in the new tail cell is undefined; the caller should be
 * sure to fill it in
 * 新尾部单元中的数据未定义；调用者应确保填入数据。
 */
static void
new_tail_cell(List *list)
{
	/* Enlarge array if necessary
	 * 如有必要，扩大数组 */
	if (list->length >= list->max_length)
		enlarge_list(list, list->length + 1);
	list->length++;
}

/*
 * =========================================================================
 * 3. 基础插入操作：追加 (Append) 与 前插 (Cons)
 * 提供向列表末尾或开头添加新元素的标准接口。
 * 注意：由于基于数组，前插操作涉及到 memmove，性能比链表弱。
 * =========================================================================
 */

/*
 * Append a pointer to the list. A pointer to the modified list is
 * returned. Note that this function may or may not destructively
 * modify the list; callers should always use this function's return
 * value, rather than continuing to use the pointer passed as the
 * first argument.
 * 将指针追加到列表。返回一个指向修改后列表的指针。
 * 请注意，此函数可能会也可能不会破坏性地修改列表；调用者应始终使用此函数的返回值，
 * 而不是继续使用作为第一个参数传递的指针。
 */
List *
lappend(List *list, void *datum)
{
	Assert(IsPointerList(list));

	if (list == NIL)
		list = new_list(T_List, 1);
	else
		new_tail_cell(list);

	llast(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Append an integer to the specified list. See lappend()
 * 将整数追加到指定列表。参见 lappend()
 */
List *
lappend_int(List *list, int datum)
{
	Assert(IsIntegerList(list));

	if (list == NIL)
		list = new_list(T_IntList, 1);
	else
		new_tail_cell(list);

	llast_int(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Append an OID to the specified list. See lappend()
 * 将 OID 追加到指定列表。参见 lappend()
 */
List *
lappend_oid(List *list, Oid datum)
{
	Assert(IsOidList(list));

	if (list == NIL)
		list = new_list(T_OidList, 1);
	else
		new_tail_cell(list);

	llast_oid(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Append a TransactionId to the specified list. See lappend()
 * 将 TransactionId 追加到指定列表。参见 lappend()
 */
List *
lappend_xid(List *list, TransactionId datum)
{
	Assert(IsXidList(list));

	if (list == NIL)
		list = new_list(T_XidList, 1);
	else
		new_tail_cell(list);

	llast_xid(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Make room for a new cell at position 'pos' (measured from 0).
 * 在位置 'pos'（从 0 开始测量）为新单元腾出空间。
 * 单元中的数据留作未定义，必须由调用者填写。
 * 'list' 被假定为 non-NIL，'pos' 必须是一个有效的列表位置，即 0 <= pos <= list 的长度。
 * 返回新单元的地址。
 */
static ListCell *
insert_new_cell(List *list, int pos)
{
	Assert(pos >= 0 && pos <= list->length);

	/* Enlarge array if necessary */
	if (list->length >= list->max_length)
		enlarge_list(list, list->length + 1);
	/* Now shove the existing data over */
	if (pos < list->length)
		memmove(&list->elements[pos + 1], &list->elements[pos],
				(list->length - pos) * sizeof(ListCell));
	list->length++;

	return &list->elements[pos];
}

/*
 * Insert the given datum at position 'pos' (measured from 0) in the list.
 * 在列表中的位置 'pos'（从 0 开始测量）插入给定的数据。
 * 'pos' 必须有效，即 0 <= pos <= 列表长度。
 *
 * Note that this takes time proportional to the distance to the end of the
 * list, since the following entries must be moved.
 * 注意，这所花费的时间与到列表末尾的距离成正比，因为必须移动后面的条目。
 */
List *
list_insert_nth(List *list, int pos, void *datum)
{
	if (list == NIL)
	{
		Assert(pos == 0);
		return list_make1(datum);
	}
	Assert(IsPointerList(list));
	lfirst(insert_new_cell(list, pos)) = datum;
	check_list_invariants(list);
	return list;
}

List *
list_insert_nth_int(List *list, int pos, int datum)
{
	if (list == NIL)
	{
		Assert(pos == 0);
		return list_make1_int(datum);
	}
	Assert(IsIntegerList(list));
	lfirst_int(insert_new_cell(list, pos)) = datum;
	check_list_invariants(list);
	return list;
}

List *
list_insert_nth_oid(List *list, int pos, Oid datum)
{
	if (list == NIL)
	{
		Assert(pos == 0);
		return list_make1_oid(datum);
	}
	Assert(IsOidList(list));
	lfirst_oid(insert_new_cell(list, pos)) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Prepend a new element to the list. A pointer to the modified list
 * is returned. Note that this function may or may not destructively
 * modify the list; callers should always use this function's return
 * value, rather than continuing to use the pointer passed as the
 * second argument.
 * 在列表前面添加一个新元素。返回指向修改后列表的指针。
 * 请请注意，此函数可能会也可能不会破坏性地修改列表；调用者应始终使用此函数的返回值，
 * 而不是继续使用传递的第二个参数指针。
 *
 * Note that this takes time proportional to the length of the list,
 * since the existing entries must be moved.
 * 请注意，这所花费的时间与列表的长度成正比，因为必须移动现有的条目。
 *
 * Caution: before Postgres 8.0, the original List was unmodified and
 * could be considered to retain its separate identity.  This is no longer
 * the case.
 * 注意：在 Postgres 8.0 之前，原始 List 是未修改的，可以认为保留了其独立的标识。
 * 现在情况不再如此。
 */
List *
lcons(void *datum, List *list)
{
	Assert(IsPointerList(list));

	if (list == NIL)
		list = new_list(T_List, 1);
	else
		new_head_cell(list);

	linitial(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Prepend an integer to the list. See lcons()
 * 在列表前面添加一个整数。参见 lcons()
 */
List *
lcons_int(int datum, List *list)
{
	Assert(IsIntegerList(list));

	if (list == NIL)
		list = new_list(T_IntList, 1);
	else
		new_head_cell(list);

	linitial_int(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * Prepend an OID to the list. See lcons()
 * 在列表前面添加一个 OID。参见 lcons()
 */
List *
lcons_oid(Oid datum, List *list)
{
	Assert(IsOidList(list));

	if (list == NIL)
		list = new_list(T_OidList, 1);
	else
		new_head_cell(list);

	linitial_oid(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * =========================================================================
 * 4. 列表连接与复制
 * 提供了列表之间的合并操作（concat）和内容复制功能。
 * =========================================================================
 */

/*
 * Concatenate list2 to the end of list1, and return list1.
 * 将 list2 连接到 list1 的末尾，并返回 list1。
 *
 * This is equivalent to lappend'ing each element of list2, in order, to list1.
 * list1 is destructively changed, list2 is not.  (However, in the case of
 * pointer lists, list1 and list2 will point to the same structures.)
 * 这相当于按顺序将 list2 的每个元素 lappend 到 list1 中。
 * list1 被破坏性地更改，list2 没变。（但是，在指针列表的情况下，list1 和 list2 将指向相同的结构。）
 *
 * Callers should be sure to use the return value as the new pointer to the
 * concatenated list: the 'list1' input pointer may or may not be the same
 * as the returned pointer.
 * 调用者应确保使用返回值作为连接后列表的新指针：“list1”输入指针可能与返回的指针相同，也可能不同。
 *
 * Note that this takes at least time proportional to the length of list2.
 * It'd typically be the case that we have to enlarge list1's storage,
 * probably adding time proportional to the length of list1.
 * 注意，这至少花费了与 list2 长度成正比的时间。通常情况下我们需要扩大 list1 的存储空间，
 * 这可能会增加与 list1 长度成正比的时间。
 */
List *
list_concat(List *list1, const List *list2)
{
	int			new_len;

	if (list1 == NIL)
		return list_copy(list2);
	if (list2 == NIL)
		return list1;

	Assert(list1->type == list2->type);

	new_len = list1->length + list2->length;
	/* Enlarge array if necessary */
	if (new_len > list1->max_length)
		enlarge_list(list1, new_len);

	/* Even if list1 == list2, using memcpy should be safe here */
	memcpy(&list1->elements[list1->length], &list2->elements[0],
		   list2->length * sizeof(ListCell));
	list1->length = new_len;

	check_list_invariants(list1);
	return list1;
}

/*
 * Form a new list by concatenating the elements of list1 and list2.
 * 通过连接 list1 和 list2 的元素形成一个新列表。
 *
 * Neither input list is modified.  (However, if they are pointer lists,
 * the output list will point to the same structures.)
 * 两个输入列表均不被修改。（但是，如果它们是指针列表，则输出列表将指向相同的结构。）
 *
 * This is equivalent to, but more efficient than,
 * list_concat(list_copy(list1), list2).
 * 这相当于 list_concat(list_copy(list1), list2)，但效率更高。
 * Note that some pre-v13 code might list_copy list2 as well, but that's
 * pointless now.
 * 请注意，一些 v13 之前的代码也可能 list_copy list2，但现在毫无意义。
 */
List *
list_concat_copy(const List *list1, const List *list2)
{
	List	   *result;
	int			new_len;

	if (list1 == NIL)
		return list_copy(list2);
	
	if (list2 == NIL)
		return list_copy(list1);

	Assert(list1->type == list2->type);

	new_len = list1->length + list2->length;
	result = new_list(list1->type, new_len);
	memcpy(result->elements, list1->elements,
		   list1->length * sizeof(ListCell));
	memcpy(result->elements + list1->length, list2->elements,
		   list2->length * sizeof(ListCell));

	check_list_invariants(result);
	return result;
}

/*
 * Truncate 'list' to contain no more than 'new_size' elements. This
 * modifies the list in-place! Despite this, callers should use the
 * pointer returned by this function to refer to the newly truncated
 * list -- it may or may not be the same as the pointer that was
 * passed.
 * 将 'list' 截断为包含不超过 'new_size' 个元素。这会就地修改列表！尽管如此，
 * 调用者应使用此函数返回的指针来引用新截断的列表 —— 它可能与传递的指针相同，也可能不相同。
 *
 * Note that any cells removed by list_truncate() are NOT pfree'd.
 * 请注意，list_truncate() 删除的任何单元都不会被 pfree。
 */
List *
list_truncate(List *list, int new_size)
{
	if (new_size <= 0)
		return NIL;				/* truncate to zero length */

	/* If asked to effectively extend the list, do nothing
	 * 如果被要求有效地扩展列表，则不执行任何操作 */
	if (new_size < list_length(list))
		list->length = new_size;

	/*
	 * Note: unlike the individual-list-cell deletion functions, we don't move
	 * the list cells to new storage, even in DEBUG_LIST_MEMORY_USAGE mode.
	 * This is because none of them can move in this operation, so just like
	 * in the old cons-cell-based implementation, this function doesn't
	 * invalidate any pointers to cells of the list.  This is also the reason
	 * for not wiping the memory of the deleted cells: the old code didn't
	 * free them either.  Perhaps later we'll tighten this up.
	 * 注意：不同于单个列表单元删除函数，即使在 DEBUG_LIST_MEMORY_USAGE 模式下，
	 * 我们也不会将列表单元移动到新的存储空间。这是因为在此操作中它们都不会移动，
	 * 所以就像在旧的基于 cons 单元的实现中一样，此函数不会使指向列表单元的任何指针失效。
	 * 这也是不擦除被删除单元内存的原因：旧代码也不释放它们。也许以后我们会加强这一点。
	 */

	return list;
}

/*
 * =========================================================================
 * 5. 查找与成员资格检查
 * 执行线性查找来确定给定的数据项是否已存在于列表中。
 * =========================================================================
 */

/*
 * Return true iff 'datum' is a member of the list. Equality is
 * determined via equal(), so callers should ensure that they pass a
 * Node as 'datum'.
 * 当且仅当 'datum' 是列表的成员时返回 true。相等性是通过 equal() 确定的，
 * 因此调用者应确保将 Node 作为 'datum' 传递。
 *
 * This does a simple linear search --- avoid using it on long lists.
 * 这执行简单的线性搜索 —— 避免在长列表上使用它。
 */
bool
list_member(const List *list, const void *datum)
{
	const ListCell *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (equal(lfirst(cell), datum))
			return true;
	}

	return false;
}

/*
 * Return true iff 'datum' is a member of the list. Equality is
 * 当且仅当 'datum' 是列表的成员时返回 true。相等性是通过使用简单的指针比较确定的。
 * determined by using simple pointer comparison.
 */
bool
list_member_ptr(const List *list, const void *datum)
{
	const ListCell *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst(cell) == datum)
			return true;
	}

	return false;
}

/*
 * Return true iff the integer 'datum' is a member of the list.
 * 当且仅当整数 'datum' 是列表的成员时返回 true。
 */
bool
list_member_int(const List *list, int datum)
{
	const ListCell *cell;

	Assert(IsIntegerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_int(cell) == datum)
			return true;
	}

	return false;
}

/*
 * Return true iff the OID 'datum' is a member of the list.
 * 当且仅当 OID 'datum' 是列表的成员时返回 true。
 */
bool
list_member_oid(const List *list, Oid datum)
{
	const ListCell *cell;

	Assert(IsOidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_oid(cell) == datum)
			return true;
	}

	return false;
}

/*
 * Return true iff the TransactionId 'datum' is a member of the list.
 * 当且仅当 TransactionId 'datum' 是列表的成员时返回 true。
 */
bool
list_member_xid(const List *list, TransactionId datum)
{
	const ListCell *cell;

	Assert(IsXidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_xid(cell) == datum)
			return true;
	}

	return false;
}

/*
 * =========================================================================
 * 6. 元素删除
 * 从列表中删除特定索引或特定值的元素。
 * 这些操作通常会涉及到数组元素的平移以保持连续性。
 * =========================================================================
 */

/*
 * Delete the n'th cell (counting from 0) in list.
 * 删除列表中的第 n 个单元（从 0 开始计数）。
 *
 * The List is pfree'd if this was the last member.
 * 如果这是最后一个成员，则 pfree 该列表。
 *
 * Note that this takes time proportional to the distance to the end of the
 * 注意，这所花费的时间与到列表末尾的距离成正比，因为必须移动后面的条目。
 * list, since the following entries must be moved.
 */
List *
list_delete_nth_cell(List *list, int n)
{
	check_list_invariants(list);

	Assert(n >= 0 && n < list->length);

	/*
	 * If we're about to delete the last node from the list, free the whole
	 * 如果我们即将从列表中删除最后一个节点，则释放整个列表并返回 NIL，这是零长度列表的唯一有效表示形式。
	 * list instead and return NIL, which is the only valid representation of
	 * a zero-length list.
	 */
	if (list->length == 1)
	{
		list_free(list);
		return NIL;
	}

	/*
	 * Otherwise, we normally just collapse out the removed element.  But for
	 * 否则，我们通常只是折叠掉被删除的元素。但出于调试目的，将整个列表内容移动到别处。
	 * debugging purposes, move the whole list contents someplace else.
	 *
	 * (Note that we *must* keep the contents in the same memory context.)
	 * （注意，我们 *必须* 将内容保持在相同的内存上下文中。）
	 */
#ifndef DEBUG_LIST_MEMORY_USAGE
	memmove(&list->elements[n], &list->elements[n + 1],
			(list->length - 1 - n) * sizeof(ListCell));
	list->length--;
#else
	{
		ListCell   *newelems;
		int			newmaxlen = list->length - 1;

		newelems = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   newmaxlen * sizeof(ListCell));
		memcpy(newelems, list->elements, n * sizeof(ListCell));
		memcpy(&newelems[n], &list->elements[n + 1],
			   (list->length - 1 - n) * sizeof(ListCell));
		if (list->elements != list->initial_elements)
			pfree(list->elements);
		else
		{
			/*
			 * As in enlarge_list(), clear the initial_elements[] space and/or
			 * 就像在 enlarge_list() 中一样，清除 initial_elements[] 空间和/或
			 * mark it inaccessible.
			 * 标记它不可访问。
			 */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(list->initial_elements,
					 list->max_length * sizeof(ListCell));
#else
			VALGRIND_MAKE_MEM_NOACCESS(list->initial_elements,
									   list->max_length * sizeof(ListCell));
#endif
		}
		list->elements = newelems;
		list->max_length = newmaxlen;
		list->length--;
		check_list_invariants(list);
	}
#endif

	return list;
}

/*
 * Delete 'cell' from 'list'.
 * 从 'list' 中删除 'cell'。
 *
 * The List is pfree'd if this was the last member.  However, we do not
 * 如果这是最后一个成员，则 pfree 该列表。但是，我们不会触及该单元可能指向的任何数据。
 * touch any data the cell might've been pointing to.
 *
 * Note that this takes time proportional to the distance to the end of the
 * 注意，这所花费的时间与到列表末尾的距离成正比，因为必须移动后面的条目。
 * list, since the following entries must be moved.
 */
List *
list_delete_cell(List *list, ListCell *cell)
{
	return list_delete_nth_cell(list, cell - list->elements);
}

/*
 * Delete the first cell in list that matches datum, if any.
 * 删除列表中第一个匹配数据（如果存在）的单元。
 * Equality is determined via equal().
 * 相等性通过 equal() 确定。
 *
 * This does a simple linear search --- avoid using it on long lists.
 * 这执行简单的线性搜索 —— 避免在长列表上使用它。
 */
List *
list_delete(List *list, void *datum)
{
	ListCell   *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (equal(lfirst(cell), datum))
			return list_delete_cell(list, cell);
	}

	/* Didn't find a match: return the list unmodified */
	/* 未找到匹配项：返回未修改的列表 */
	return list;
}

/* As above, but use simple pointer equality */
/* 同上，但使用简单的指针相等性 */
List *
list_delete_ptr(List *list, void *datum)
{
	ListCell   *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst(cell) == datum)
			return list_delete_cell(list, cell);
	}

	/* Didn't find a match: return the list unmodified */
	/* 未找到匹配项：返回未修改的列表 */
	return list;
}

/* As above, but for integers */
/* 同上，但用于整数 */
List *
list_delete_int(List *list, int datum)
{
	ListCell   *cell;

	Assert(IsIntegerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_int(cell) == datum)
			return list_delete_cell(list, cell);
	}

	/* Didn't find a match: return the list unmodified */
	/* 未找到匹配项：返回未修改的列表 */
	return list;
}

/* As above, but for OIDs */
/* 同上，但用于 OID */
List *
list_delete_oid(List *list, Oid datum)
{
	ListCell   *cell;

	Assert(IsOidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_oid(cell) == datum)
			return list_delete_cell(list, cell);
	}

	/* Didn't find a match: return the list unmodified */
	/* 未找到匹配项：返回未修改的列表 */
	return list;
}

/*
 * Delete the first element of the list.
 * 删除列表的第一个元素。
 *
 * This is useful to replace the Lisp-y code "list = lnext(list);" in cases
 * 这在需要修改列表而不是仅仅遍历列表的情况下，可以替代 Lisp 风格的代码 "list = lnext(list);"。
 * where the intent is to alter the list rather than just traverse it.
 * 请注意，列表被修改了，而 Lisp 风格的编码会保持原始列表头不变，以防有其他指针指向它。
 * Beware that the list is modified, whereas the Lisp-y coding leaves
 * the original list head intact in case there's another pointer to it.
 *
 * Note that this takes time proportional to the length of the list,
 * 请注意，这所花费的时间与列表的长度成正比，因为必须移动剩余的条目。
 * since the remaining entries must be moved.  Consider reversing the
 * 考虑反转列表顺序，以便可以使用 list_delete_last()。但是，
 * list order so that you can use list_delete_last() instead.  However,
 * 如果这导致您用 lcons() 替换 lappend()，那么您并没有改善情况。
 * if that causes you to replace lappend() with lcons(), you haven't
 * （简而言之，您可以从 List 中创建一个高效的栈，但不能创建一个高效的 FIFO 队列。）
 * improved matters.  (In short, you can make an efficient stack from
 * a List, but not an efficient FIFO queue.)
 */
List *
list_delete_first(List *list)
{
	check_list_invariants(list);

	if (list == NIL)
		return NIL;				/* would an error be better? */
	/* 抛出错误会更好吗？ */

	return list_delete_nth_cell(list, 0);
}

/*
 * Delete the last element of the list.
 * 删除列表的最后一个元素。
 */
List *
list_delete_last(List *list)
{
	check_list_invariants(list);

	if (list == NIL)
		return NIL;				/* would an error be better? */
	/* 抛出错误会更好吗？ */

	/* list_truncate won't free list if it goes to empty, but this should */
	/* 如果列表变空，list_truncate 不会释放列表，但这里应该释放 */
	if (list_length(list) <= 1)
	{
		list_free(list);
		return NIL;
	}

	return list_truncate(list, list_length(list) - 1);
}

/*
 * Delete the first N cells of the list.
 * 删除列表的前 N 个单元。
 *
 * The List is pfree'd if the request causes all cells to be deleted.
 * 如果请求导致所有单元被删除，则 List 会被 pfree。
 *
 * Note that this takes time proportional to the distance to the end of the
 * 注意，这所花费的时间与到列表末尾的距离成正比，因为必须移动后面的条目。
 * list, since the following entries must be moved.
 */
List *
list_delete_first_n(List *list, int n)
{
	check_list_invariants(list);

	/* No-op request? */
	/* 无操作请求？ */
	if (n <= 0)
		return list;

	/* Delete whole list? */
	/* 删除整个列表？ */
	if (n >= list_length(list))
	{
		list_free(list);
		return NIL;
	}

	/*
	 * Otherwise, we normally just collapse out the removed elements.  But for
	 * 否则，我们通常只是折叠掉被删除的元素。但出于调试目的，将整个列表内容移动到别处。
	 * debugging purposes, move the whole list contents someplace else.
	 *
	 * (Note that we *must* keep the contents in the same memory context.)
	 * （注意，我们 *必须* 将内容保持在相同的内存上下文中。）
	 */
#ifndef DEBUG_LIST_MEMORY_USAGE
	memmove(&list->elements[0], &list->elements[n],
			(list->length - n) * sizeof(ListCell));
	list->length -= n;
#else
	{
		ListCell   *newelems;
		int			newmaxlen = list->length - n;

		newelems = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   newmaxlen * sizeof(ListCell));
		memcpy(newelems, &list->elements[n], newmaxlen * sizeof(ListCell));
		if (list->elements != list->initial_elements)
			pfree(list->elements);
		else
		{
			/*
			 * As in enlarge_list(), clear the initial_elements[] space and/or
			 * 就像在 enlarge_list() 中一样，清除 initial_elements[] 空间和/或
			 * mark it inaccessible.
			 * 标记它不可访问。
			 */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(list->initial_elements,
					 list->max_length * sizeof(ListCell));
#else
			VALGRIND_MAKE_MEM_NOACCESS(list->initial_elements,
									   list->max_length * sizeof(ListCell));
#endif
		}
		list->elements = newelems;
		list->max_length = newmaxlen;
		list->length = newmaxlen;
		check_list_invariants(list);
	}
#endif

	return list;
}

/*
 * =========================================================================
 * 7. 集合运算（并集、交集、差集）
 * 模拟数学意义上的集合操作。
 * 这些算法通常具有 O(N*M) 的复杂度，应避免在大规模列表上使用。
 * =========================================================================
 */

/*
 * Generate the union of two lists. This is calculated by copying
 * 生成两个列表的并集。这是通过复制 list1，然后将 list2 中所有不在 list1 中的成员添加到其中来计算的。
 * list1 via list_copy(), then adding to it all the members of list2
 * that aren't already in list1.
 *
 * Whether an element is already a member of the list is determined
 * 元素是否已是列表成员是通过 equal() 确定的。
 * via equal(), so callers should ensure that they pass a
 * Node as 'datum'.
 *
 * The returned list is newly-allocated, although the content of the
 * 返回的列表是新分配的，尽管单元格的内容是相同的（即，任何指向的对象都不会被复制）。
 * cells is the same (i.e. any pointed-to objects are not copied).
 *
 * NB: this function will NOT remove any duplicates that are present
 * 注意：此函数不会删除 list1 中存在的任何重复项（因此，如果 list1 最初已知是唯一的，它才执行“并集”操作）。
 * in list1 (so it only performs a "union" if list1 is known unique to
 * 此外，如果您要编写 "x = list_union(x, y)"，您可能希望使用 list_concat_unique() 来避免浪费旧 x 列表的存储空间。
 * start with).  Also, if you are about to write "x = list_union(x, y)"
 * you probably want to use list_concat_unique() instead to avoid wasting
 * the storage of the old x list.
 *
 * Note that this takes time proportional to the product of the list
 * 请注意，这所花费的时间与列表长度的乘积成正比，因此请注意不要在长列表上使用它。（我们或许可以改进这一点，
 * lengths, so beware of using it on long lists.  (We could probably
 * 但如果这成为性能瓶颈，您真的应该使用其他数据结构。）
 * improve that, but really you should be using some other data structure
 * if this'd be a performance bottleneck.)
 */
List *
list_union(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member(result, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_union() determines duplicates via simple
 * list_union() 的此变体通过简单的指针比较来确定重复项。
 * pointer comparison.
 */
List *
list_union_ptr(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_ptr(result, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_union() operates upon lists of integers.
 * list_union() 的此变体对整数列表进行操作。
 */
List *
list_union_int(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_int(result, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_union() operates upon lists of OIDs.
 * list_union() 的此变体对 OID 列表进行操作。
 */
List *
list_union_oid(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_oid(result, lfirst_oid(cell)))
			result = lappend_oid(result, lfirst_oid(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * Return a list that contains all the cells that are in both list1 and
 * 返回一个包含 list1 和 list2 中所有单元的列表。返回的列表通过 palloc() 新分配，
 * list2.  The returned list is freshly allocated via palloc(), but the
 * 但单元本身指向与输入列表单元相同的对象。
 * cells themselves point to the same objects as the cells of the
 * input lists.
 *
 * Duplicate entries in list1 will not be suppressed, so it's only a true
 * list1 中的重复条目不会被抑制，因此只有当 list1 事先已知是唯一的时，它才是一个真正的“交集”。
 * "intersection" if list1 is known unique beforehand.
 *
 * This variant works on lists of pointers, and determines list
 * 此变体适用于指针列表，并通过 equal() 确定列表成员资格。
 * membership via equal().  Note that the list1 member will be pointed
 * 请注意，list1 成员将在结果中被指向。
 * to in the result.
 *
 * Note that this takes time proportional to the product of the list
 * 请注意，这所花费的时间与列表长度的乘积成正比，因此请注意不要在长列表上使用它。（我们或许可以改进这一点，
 * lengths, so beware of using it on long lists.  (We could probably
 * 但如果这成为性能瓶颈，您真的应该使用其他数据结构。）
 * improve that, but really you should be using some other data structure
 * if this'd be a performance bottleneck.)
 */
List *
list_intersection(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	if (list1 == NIL || list2 == NIL)
		return NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = NIL;
	foreach(cell, list1)
	{
		if (list_member(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * As list_intersection but operates on lists of integers.
 * 同 list_intersection，但对整数列表进行操作。
 */
List *
list_intersection_int(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	if (list1 == NIL || list2 == NIL)
		return NIL;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	result = NIL;
	foreach(cell, list1)
	{
		if (list_member_int(list2, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * Return a list that contains all the cells in list1 that are not in
 * 返回一个包含 list1 中所有不在 list2 中的单元的列表。返回的列表通过 palloc() 新分配，
 * list2. The returned list is freshly allocated via palloc(), but the
 * 但单元本身指向与输入列表单元相同的对象。
 * cells themselves point to the same objects as the cells of the
 * input lists.
 *
 * This variant works on lists of pointers, and determines list
 * 此变体适用于指针列表，并通过 equal() 确定列表成员资格。
 * membership via equal()
 *
 * Note that this takes time proportional to the product of the list
 * 请注意，这所花费的时间与列表长度的乘积成正比，因此请注意不要在长列表上使用它。（我们或许可以改进这一点，
 * lengths, so beware of using it on long lists.  (We could probably
 * 但如果这成为性能瓶颈，您真的应该使用其他数据结构。）
 * improve that, but really you should be using some other data structure
 * if this'd be a performance bottleneck.)
 */
List *
list_difference(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_difference() determines list membership via
 * list_difference() 的此变体通过简单的指针相等性确定列表成员资格。
 * simple pointer equality.
 */
List *
list_difference_ptr(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_ptr(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_difference() operates upon lists of integers.
 * list_difference() 的此变体对整数列表进行操作。
 */
List *
list_difference_int(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_int(list2, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * This variant of list_difference() operates upon lists of OIDs.
 * list_difference() 的此变体对 OID 列表进行操作。
 */
List *
list_difference_oid(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_oid(list2, lfirst_oid(cell)))
			result = lappend_oid(result, lfirst_oid(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * Append datum to list, but only if it isn't already in the list.
 * 将数据附加到列表，但仅当它不在列表中时。
 *
 * Whether an element is already a member of the list is determined
 * 元素是否已是列表成员是通过 equal() 确定的。
 * via equal().
 *
 * This does a simple linear search --- avoid using it on long lists.
 * 这执行简单的线性搜索 —— 避免在长列表上使用它。
 */
List *
list_append_unique(List *list, void *datum)
{
	if (list_member(list, datum))
		return list;
	else
		return lappend(list, datum);
}

/*
 * This variant of list_append_unique() determines list membership via
 * list_append_unique() 的此变体通过简单的指针相等性确定列表成员资格。
 * simple pointer equality.
 */
List *
list_append_unique_ptr(List *list, void *datum)
{
	if (list_member_ptr(list, datum))
		return list;
	else
		return lappend(list, datum);
}

/*
 * This variant of list_append_unique() operates upon lists of integers.
 * list_append_unique() 的此变体对整数列表进行操作。
 */
List *
list_append_unique_int(List *list, int datum)
{
	if (list_member_int(list, datum))
		return list;
	else
		return lappend_int(list, datum);
}

/*
 * This variant of list_append_unique() operates upon lists of OIDs.
 * list_append_unique() 的此变体对 OID 列表进行操作。
 */
List *
list_append_unique_oid(List *list, Oid datum)
{
	if (list_member_oid(list, datum))
		return list;
	else
		return lappend_oid(list, datum);
}

/*
 * Append to list1 each member of list2 that isn't already in list1.
 * 将 list2 中不在 list1 中的每个成员附加到 list1。
 *
 * Whether an element is already a member of the list is determined
 * 元素是否已是列表成员是通过 equal() 确定的。
 * via equal().
 *
 * This is almost the same functionality as list_union(), but list1 is
 * 这与 list_union() 的功能几乎相同，但 list1 是就地修改的，而不是被复制。
 * modified in-place rather than being copied. However, callers of this
 * 但是，此函数的调用者可能对排序有严格的期望——即，那些非重复的 list2 元素的相对顺序得到保留。
 * function may have strict ordering expectations -- i.e. that the relative
 * order of those list2 elements that are not duplicates is preserved.
 *
 * Note that this takes time proportional to the product of the list
 * 请注意，这所花费的时间与列表长度的乘积成正比，因此请注意不要在长列表上使用它。（我们或许可以改进这一点，
 * lengths, so beware of using it on long lists.  (We could probably
 * 但如果这成为性能瓶颈，您真的应该使用其他数据结构。）
 * improve that, but really you should be using some other data structure
 * if this'd be a performance bottleneck.)
 */
List *
list_concat_unique(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	foreach(cell, list2)
	{
		if (!list_member(list1, lfirst(cell)))
			list1 = lappend(list1, lfirst(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * This variant of list_concat_unique() determines list membership via
 * list_concat_unique() 的此变体通过简单的指针相等性确定列表成员资格。
 * simple pointer equality.
 */
List *
list_concat_unique_ptr(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	foreach(cell, list2)
	{
		if (!list_member_ptr(list1, lfirst(cell)))
			list1 = lappend(list1, lfirst(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * This variant of list_concat_unique() operates upon lists of integers.
 * list_concat_unique() 的此变体对整数列表进行操作。
 */
List *
list_concat_unique_int(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	foreach(cell, list2)
	{
		if (!list_member_int(list1, lfirst_int(cell)))
			list1 = lappend_int(list1, lfirst_int(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * This variant of list_concat_unique() operates upon lists of OIDs.
 * list_concat_unique() 的此变体对 OID 列表进行操作。
 */
List *
list_concat_unique_oid(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	foreach(cell, list2)
	{
		if (!list_member_oid(list1, lfirst_oid(cell)))
			list1 = lappend_oid(list1, lfirst_oid(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * Remove adjacent duplicates in a list of OIDs.
 * 删除 OID 列表中相邻的重复项。
 *
 * It is caller's responsibility to have sorted the list to bring duplicates
 * 调用者有责任对列表进行排序，以便将重复项聚集在一起，例如通过 list_sort(list, list_oid_cmp)。
 * together, perhaps via list_sort(list, list_oid_cmp).
 *
 * Note that this takes time proportional to the length of the list.
 * 请注意，这所花费的时间与列表的长度成正比。
 */
void
list_deduplicate_oid(List *list)
{
	int			len;

	Assert(IsOidList(list));
	len = list_length(list);
	if (len > 1)
	{
		ListCell   *elements = list->elements;
		int			i = 0;

		for (int j = 1; j < len; j++)
		{
			if (elements[i].oid_value != elements[j].oid_value)
				elements[++i].oid_value = elements[j].oid_value;
		}
		list->length = i + 1;
	}
	check_list_invariants(list);
}

/*
 * Free all storage in a list, and optionally the pointed-to elements
 * 释放列表中所有存储空间，并可选地释放指向的元素。
 */
static void
list_free_private(List *list, bool deep)
{
	if (list == NIL)
		return;					/* nothing to do */
	/* 无事可做 */

	check_list_invariants(list);

	if (deep)
	{
		for (int i = 0; i < list->length; i++)
			pfree(lfirst(&list->elements[i]));
	}
	if (list->elements != list->initial_elements)
		pfree(list->elements);
	pfree(list);
}

/*
 * =========================================================================
 * 8. 内存释放与清理
 * 处理列表及其内容的销毁逻辑。
 * =========================================================================
 */

/*
 * Free all the cells of the list, as well as the list itself. Any
 * 释放列表的所有单元格以及列表本身。列表单元格指向的任何对象都不会被释放。
 * objects that are pointed-to by the cells of the list are NOT
 * free'd.
 *
 * On return, the argument to this function has been freed, so the
 * 返回时，此函数的参数已被释放，因此调用者最好将其设置为 NIL 以确保安全。
 * caller would be wise to set it to NIL for safety's sake.
 */
void
list_free(List *list)
{
	list_free_private(list, false);
}

/*
 * Free all the cells of the list, the list itself, and all the
 * 释放列表的所有单元格、列表本身以及列表单元格指向的所有对象（列表中的每个元素必须包含指向 palloc() 分配的内存区域的指针！）
 * objects pointed-to by the cells of the list (each element in the
 * list must contain a pointer to a palloc()'d region of memory!)
 *
 * On return, the argument to this function has been freed, so the
 * 返回时，此函数的参数已被释放，因此调用者最好将其设置为 NIL 以确保安全。
 * caller would be wise to set it to NIL for safety's sake.
 */
void
list_free_deep(List *list)
{
	/*
	 * A "deep" free operation only makes sense on a list of pointers.
	 * “深度”释放操作仅对指针列表有意义。
	 */
	Assert(IsPointerList(list));
	list_free_private(list, true);
}

/*
 * Return a shallow copy of the specified list.
 * 返回指定列表的浅拷贝。
 */
List *
list_copy(const List *oldlist)
{
	List	   *newlist;

	if (oldlist == NIL)
		return NIL;

	newlist = new_list(oldlist->type, oldlist->length);
	memcpy(newlist->elements, oldlist->elements,
		   newlist->length * sizeof(ListCell));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * Return a shallow copy of the specified list containing only the first 'len'
 * 返回指定列表的浅拷贝，仅包含前 'len' 个元素。如果 oldlist 短于 'len'，则复制整个列表。
 * elements.  If oldlist is shorter than 'len' then we copy the entire list.
 */
List *
list_copy_head(const List *oldlist, int len)
{
	List	   *newlist;

	if (oldlist == NIL || len <= 0)
		return NIL;

	len = Min(oldlist->length, len);

	newlist = new_list(oldlist->type, len);
	memcpy(newlist->elements, oldlist->elements, len * sizeof(ListCell));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * Return a shallow copy of the specified list, without the first N elements.
 * 返回指定列表的浅拷贝，不包含前 N 个元素。
 */
List *
list_copy_tail(const List *oldlist, int nskip)
{
	List	   *newlist;

	if (nskip < 0)
		nskip = 0;				/* would it be better to elog? */
	/* 抛出 elog 会更好吗？ */

	if (oldlist == NIL || nskip >= oldlist->length)
		return NIL;

	newlist = new_list(oldlist->type, oldlist->length - nskip);
	memcpy(newlist->elements, &oldlist->elements[nskip],
		   newlist->length * sizeof(ListCell));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * Return a deep copy of the specified list.
 * 返回指定列表的深拷贝。
 *
 * The list elements are copied via copyObject(), so that this function's
 * 列表元素通过 copyObject() 复制，因此此函数对“深”拷贝的理解比 list_free_deep() 对同一词的理解要深得多。
 * idea of a "deep" copy is considerably deeper than what list_free_deep()
 * means by the same word.
 */
List *
list_copy_deep(const List *oldlist)
{
	List	   *newlist;

	if (oldlist == NIL)
		return NIL;

	/* This is only sensible for pointer Lists */
	Assert(IsA(oldlist, List));

	newlist = new_list(oldlist->type, oldlist->length);
	for (int i = 0; i < newlist->length; i++)
		lfirst(&newlist->elements[i]) =
			copyObjectImpl(lfirst(&oldlist->elements[i]));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * =========================================================================
 * 9. 排序支持
 * 提供通用排序接口及预定义的比较算法。
 * =========================================================================
 */

/*
 * Sort a list according to the specified comparator function.
 * 根据指定的比较函数对列表进行排序。
 *
 * The list is sorted in-place.
 * 该列表是就地排序的。
 *
 * The comparator function is declared to receive arguments of type
 * const ListCell *; this allows it to use lfirst() and variants
 * without casting its arguments.  Otherwise it behaves the same as
 * the comparator function for standard qsort().
 * 比较函数声明为接收 const ListCell * 类型的参数；这允许它使用 lfirst() 及其变体，
 * 而无需转换其参数。否则，它的行为与标准 qsort() 的比较函数相同。
 *
 * Like qsort(), this provides no guarantees about sort stability
 * for equal keys.
 * 与 qsort() 一样，这不能保证相等键的排序稳定性。
 *
 * This is based on qsort(), so it likewise has O(N log N) runtime.
 * 这基于 qsort()，因此其运行时间同样为 O(N log N)。
 */
void
list_sort(List *list, list_sort_comparator cmp)
{
	typedef int (*qsort_comparator) (const void *a, const void *b);
	int			len;

	check_list_invariants(list);

	/* Nothing to do if there's less than two elements
	 * 如果少于两个元素则无须操作 */
	len = list_length(list);
	if (len > 1)
		qsort(list->elements, len, sizeof(ListCell), (qsort_comparator) cmp);
}

/*
 * list_sort comparator for sorting a list into ascending int order.
 * 用于将列表按 int 升序排列的 list_sort 比较器。
 */
int
list_int_cmp(const ListCell *p1, const ListCell *p2)
{
	int			v1 = lfirst_int(p1);
	int			v2 = lfirst_int(p2);

	return pg_cmp_s32(v1, v2);
}

/*
 * list_sort comparator for sorting a list into ascending OID order.
 * 用于将列表按 OID 升序排列的 list_sort 比较器。
 */
int
list_oid_cmp(const ListCell *p1, const ListCell *p2)
{
	Oid			v1 = lfirst_oid(p1);
	Oid			v2 = lfirst_oid(p2);

	return pg_cmp_u32(v1, v2);
}
