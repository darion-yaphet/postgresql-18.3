/*-------------------------------------------------------------------------
 *
 * binaryheap.c
 *	  A simple binary heap implementation
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/common/binaryheap.c
 *
 *-------------------------------------------------------------------------
 * binaryheap.c
 *	  A simple binary heap implementation
 *	  一个简单的二叉堆实现
 *
 *	  Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *	  部分版权所有 (c) 2012-2025, PostgreSQL 全球开发组
 *
 *	  IDENTIFICATION
 *	  标识
 *		  src/common/binaryheap.c
 */

/*
 * binaryheap.c --- Binary Heap Implementation.
 * 核心流程解释：
 * 1. 节点偏移量计算：堆在内存中用一维数组形式连续存储，通过 left_offset、right_offset 和 parent_offset 对应计算位置。
 * 2. 插入节点：由 binaryheap_add 执行。先加到数组末尾，然后调用 sift_up 递归向上比较并交换，直至堆序性质恢复。
 * 3. 弹出节点：由 binaryheap_remove_first 执行。先取出第 0 个根元素，再把数组最后一位节点移到第 0 位，之后调用 sift_down 自顶向下筛选，恢复堆性质。
 * 4. 批量建堆：通过 binaryheap_add_unordered 将所有节点无序写入，最后调用 binaryheap_build，从最后一个非叶子节点开始依次向下执行 sift_down。
 */
#ifdef FRONTEND
#include "postgres_fe.h"
#else
#include "postgres.h"
#endif

#include <math.h>

#ifdef FRONTEND
#include "common/logging.h"
#endif
#include "lib/binaryheap.h"

static void sift_down(binaryheap *heap, int node_off);
static void sift_up(binaryheap *heap, int node_off);

/*
 * binaryheap_allocate
 *
 * Returns a pointer to a newly-allocated heap that has the capacity to
 * store the given number of nodes, with the heap property defined by
 * the given comparator function, which will be invoked with the additional
 * argument specified by 'arg'.
 * binaryheap_allocate
 *
 * Returns a pointer to a newly-allocated heap that has the capacity to
 * store the given number of nodes, with the heap property defined by
 * the given comparator function, which will be invoked with the additional
 * argument specified by 'arg'.
 * 返回指向新分配的堆的指针。该堆有能力存储给定数量的节点，其堆属性由给定的比较器函数定义，
 * 该比较器函数将被调用，并带有 'arg' 指定的额外参数。
 *
 * Function Role: Allocate and initialize a binary heap.
 * 函数作用：分配并初始化一个二叉堆。
 */
binaryheap *
binaryheap_allocate(int capacity, binaryheap_comparator compare, void *arg)
{
	int			sz;
	binaryheap *heap;

	sz = offsetof(binaryheap, bh_nodes) + sizeof(bh_node_type) * capacity;
	heap = (binaryheap *) palloc(sz);
	heap->bh_space = capacity;
	heap->bh_compare = compare;
	heap->bh_arg = arg;

	heap->bh_size = 0;
	heap->bh_has_heap_property = true;

	return heap;
}

/*
 * binaryheap_reset
 *
 * Resets the heap to an empty state, losing its data content but not the
 * parameters passed at allocation.
 * binaryheap_reset
 *
 * Resets the heap to an empty state, losing its data content but not the
 * parameters passed at allocation.
 * 将堆重置为空状态，清除其数据内容，但保留分配时传递的参数。
 *
 * Function Role: Reset the binary heap size to 0.
 * 函数作用：将二叉堆的大小重置为0。
 */
void
binaryheap_reset(binaryheap *heap)
{
	heap->bh_size = 0;
	heap->bh_has_heap_property = true;
}

/*
 * binaryheap_free
 *
 * Releases memory used by the given binaryheap.
 * binaryheap_free
 *
 * Releases memory used by the given binaryheap.
 * 释放给定的二叉堆所使用的内存。
 *
 * Function Role: Free memory of binary heap.
 * 函数作用：释放二叉堆的内存。
 */
void
binaryheap_free(binaryheap *heap)
{
	pfree(heap);
}

/*
 * These utility functions return the offset of the left child, right
 * child, and parent of the node at the given index, respectively.
 *
 * The heap is represented as an array of nodes, with the root node
 * stored at index 0. The left child of node i is at index 2*i+1, and
 * the right child at 2*i+2. The parent of node i is at index (i-1)/2.
 * These utility functions return the offset of the left child, right
 * child, and parent of the node at the given index, respectively.
 * 这些辅助函数分别返回给定索引处的节点的左子节点、右子节点和父节点的偏移量。
 *
 * The heap is represented as an array of nodes, with the root node
 * stored at index 0. The left child of node i is at index 2*i+1, and
 * the right child at 2*i+2. The parent of node i is at index (i-1)/2.
 * 堆表示为节点数组，根节点存储在索引 0 处。节点 i 的左子节点在索引 2*i+1 处，
 * 右子节点在 2*i+2 处。节点 i 的父节点在索引 (i-1)/2 处。
 */

/*
 * left_offset --- Get left child offset.
 * 函数作用：返回给定父节点索引的左子节点偏移量。
 */
static inline int
left_offset(int i)
{
	return 2 * i + 1;
}

/*
 * right_offset --- Get right child offset.
 * 函数作用：返回给定父节点索引的右子节点偏移量。
 */
static inline int
right_offset(int i)
{
	return 2 * i + 2;
}

/*
 * parent_offset --- Get parent node offset.
 * 函数作用：返回给定子节点索引的父节点偏移量。
 */
static inline int
parent_offset(int i)
{
	return (i - 1) / 2;
}

/*
 * binaryheap_add_unordered
 *
 * Adds the given datum to the end of the heap's list of nodes in O(1) without
 * preserving the heap property. This is a convenience to add elements quickly
 * to a new heap. To obtain a valid heap, one must call binaryheap_build()
 * afterwards.
 * binaryheap_add_unordered
 *
 * Adds the given datum to the end of the heap's list of nodes in O(1) without
 * preserving the heap property. This is a convenience to add elements quickly
 * to a new heap. To obtain a valid heap, one must call binaryheap_build()
 * afterwards.
 * 在 O(1) 时间内将给定的数据添加到堆的节点列表的末尾，而不维护堆属性。
 * 这便于快速将元素添加到新堆中。要获得有效的堆，之后必须调用 binaryheap_build()。
 *
 * Function Role: Insert node to backend array without maintaining heap property.
 * 函数作用：将节点插入到底层数组末尾，不维护堆性质。
 */
void
binaryheap_add_unordered(binaryheap *heap, bh_node_type d)
{
	if (heap->bh_size >= heap->bh_space)
	{
#ifdef FRONTEND
		pg_fatal("out of binary heap slots");
#else
		elog(ERROR, "out of binary heap slots");
#endif
	}
	heap->bh_has_heap_property = false;
	heap->bh_nodes[heap->bh_size] = d;
	heap->bh_size++;
}

/*
 * binaryheap_build
 *
 * Assembles a valid heap in O(n) from the nodes added by
 * binaryheap_add_unordered(). Not needed otherwise.
 * binaryheap_build
 *
 * Assembles a valid heap in O(n) from the nodes added by
 * binaryheap_add_unordered(). Not needed otherwise.
 * 从通过 binaryheap_add_unordered() 添加的节点中在 O(n) 时间内构建一个有效的堆。
 * 否则不需要此操作。
 *
 * Function Role: Rebuild heap from unordered nodes.
 * 函数作用：从无序节点重建堆（堆化过程）。
 */
void
binaryheap_build(binaryheap *heap)
{
	int			i;

	for (i = parent_offset(heap->bh_size - 1); i >= 0; i--)
		sift_down(heap, i);
	heap->bh_has_heap_property = true;
}

/*
 * binaryheap_add
 *
 * Adds the given datum to the heap in O(log n) time, while preserving
 * the heap property.
 * binaryheap_add
 *
 * Adds the given datum to the heap in O(log n) time, while preserving
 * the heap property.
 * 在 O(log n) 时间内将给定的数据添加到堆中，同时维护堆属性。
 *
 * Function Role: Add node and restore heap property.
 * 函数作用：添加节点并恢复堆性质。
 */
void
binaryheap_add(binaryheap *heap, bh_node_type d)
{
	if (heap->bh_size >= heap->bh_space)
	{
#ifdef FRONTEND
		pg_fatal("out of binary heap slots");
#else
		elog(ERROR, "out of binary heap slots");
#endif
	}
	heap->bh_nodes[heap->bh_size] = d;
	heap->bh_size++;
	sift_up(heap, heap->bh_size - 1);
}

/*
 * binaryheap_first
 *
 * Returns a pointer to the first (root, topmost) node in the heap
 * without modifying the heap. The caller must ensure that this
 * routine is not used on an empty heap. Always O(1).
 * binaryheap_first
 *
 * Returns a pointer to the first (root, topmost) node in the heap
 * without modifying the heap. The caller must ensure that this
 * routine is not used on an empty heap. Always O(1).
 * 返回指向堆中第一个（根、最顶端）节点的指针，而不修改堆。调用者必须确保该例程不在空堆上使用。始终为 O(1)。
 *
 * Function Role: Retrieve root node of heap.
 * 函数作用：获取堆顶（根）节点。
 */
bh_node_type
binaryheap_first(binaryheap *heap)
{
	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);
	return heap->bh_nodes[0];
}

/*
 * binaryheap_remove_first
 *
 * Removes the first (root, topmost) node in the heap and returns a
 * pointer to it after rebalancing the heap. The caller must ensure
 * that this routine is not used on an empty heap. O(log n) worst
 * case.
 * binaryheap_remove_first
 *
 * Removes the first (root, topmost) node in the heap and returns a
 * pointer to it after rebalancing the heap. The caller must ensure
 * that this routine is not used on an empty heap. O(log n) worst
 * case.
 * 删除堆中的第一个（根、最顶端）节点，并在重新平衡堆之后返回指向它的指针。
 * 调用者必须确保此例程不会在空堆上使用。最坏情况下为 O(log n)。
 *
 * Function Role: Extract root node and rebalance heap.
 * 函数作用：弹出堆顶节点并重新平衡堆。
 */
bh_node_type
binaryheap_remove_first(binaryheap *heap)
{
	bh_node_type result;

	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);

	/* extract the root node, which will be the result - 提取根节点，这将是返回的结果 */
	result = heap->bh_nodes[0];

	/* easy if heap contains one element - 如果堆包含一个元素，则很简单 */
	if (heap->bh_size == 1)
	{
		heap->bh_size--;
		return result;
	}

	/*
	 * Remove the last node, placing it in the vacated root entry, and sift
	 * the new root node down to its correct position.
	 * Remove the last node, placing it in the vacated root entry, and sift
	 * the new root node down to its correct position.
	 * 移除最后一个节点，将其放置在空出来的根条目中，然后将新的根节点向下筛选（sift down）到其正确位置。
	 */
	heap->bh_nodes[0] = heap->bh_nodes[--heap->bh_size];
	sift_down(heap, 0);

	return result;
}

/*
 * binaryheap_remove_node
 *
 * Removes the nth (zero based) node from the heap.  The caller must ensure
 * that there are at least (n + 1) nodes in the heap.  O(log n) worst case.
 * binaryheap_remove_node
 *
 * Removes the nth (zero based) node from the heap.  The caller must ensure
 * that there are at least (n + 1) nodes in the heap.  O(log n) worst case.
 * 从堆中删除第 n 个（基于零）节点。调用者必须确保堆中至少有 (n + 1) 个节点。
 * 最坏情况下为 O(log n)。
 *
 * Function Role: Remove arbitrary node from heap.
 * 函数作用：从堆中移除任意指定位置的节点。
 */
void
binaryheap_remove_node(binaryheap *heap, int n)
{
	int			cmp;

	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);
	Assert(n >= 0 && n < heap->bh_size);

	/* compare last node to the one that is being removed - 将最后一个节点与要被移除的节点进行比较 */
	cmp = heap->bh_compare(heap->bh_nodes[--heap->bh_size],
						   heap->bh_nodes[n],
						   heap->bh_arg);

	/* remove the last node, placing it in the vacated entry - 移除最后一个节点，将其放置在空出来的条目中 */
	heap->bh_nodes[n] = heap->bh_nodes[heap->bh_size];

	/* sift as needed to preserve the heap property - 根据需要进行筛选以维护堆属性 */
	if (cmp > 0)
		sift_up(heap, n);
	else if (cmp < 0)
		sift_down(heap, n);
}

/*
 * binaryheap_replace_first
 *
 * Replace the topmost element of a non-empty heap, preserving the heap
 * property.  O(1) in the best case, or O(log n) if it must fall back to
 * sifting the new node down.
 * binaryheap_replace_first
 *
 * Replace the topmost element of a non-empty heap, preserving the heap
 * property.  O(1) in the best case, or O(log n) if it must fall back to
 * sifting the new node down.
 * 替换非空堆的最顶层元素，并维护堆属性。最好情况下为 O(1)，
 * 如果必须向下筛选新节点，则为 O(log n)。
 *
 * Function Role: Replace root node and sift down.
 * 函数作用：替换堆顶（根）节点并执行下滤调整。
 */
void
binaryheap_replace_first(binaryheap *heap, bh_node_type d)
{
	Assert(!binaryheap_empty(heap) && heap->bh_has_heap_property);

	heap->bh_nodes[0] = d;

	if (heap->bh_size > 1)
		sift_down(heap, 0);
}

/*
 * Sift a node up to the highest position it can hold according to the
 * comparator.
 * Sift a node up to the highest position it can hold according to the
 * comparator.
 * 根据比较器将节点向上筛选到它能持有的最高位置。
 *
 * Function Role: Sift up a node to restore heap property.
 * 函数作用：将一个节点上滤以恢复堆性质。
 */
static void
sift_up(binaryheap *heap, int node_off)
{
	bh_node_type node_val = heap->bh_nodes[node_off];

	/*
	 * Within the loop, the node_off'th array entry is a "hole" that
	 * notionally holds node_val, but we don't actually store node_val there
	 * till the end, saving some unnecessary data copying steps.
	 * Within the loop, the node_off'th array entry is a "hole" that
	 * notionally holds node_val, but we don't actually store node_val there
	 * till the end, saving some unnecessary data copying steps.
	 * 在循环中，第 node_off 个数组条目是一个概念上容纳 node_val 的“洞”，
	 * 但在结束之前，我们实际上并没有把 node_val 存放在那里，这省去了一些不必要的数据复制步骤。
	 */
	while (node_off != 0)
	{
		int			cmp;
		int			parent_off;
		bh_node_type parent_val;

		/*
		 * If this node is smaller than its parent, the heap condition is
		 * satisfied, and we're done.
		 * If this node is smaller than its parent, the heap condition is
		 * satisfied, and we're done.
		 * 如果此节点小于其父节点，则堆条件已满足，我们就完成了。
		 */
		parent_off = parent_offset(node_off);
		parent_val = heap->bh_nodes[parent_off];
		cmp = heap->bh_compare(node_val,
							   parent_val,
							   heap->bh_arg);
		if (cmp <= 0)
			break;

		/*
		 * Otherwise, swap the parent value with the hole, and go on to check
		 * the node's new parent.
		 * Otherwise, swap the parent value with the hole, and go on to check
		 * the node's new parent.
		 * 否则，将父节点值与该“洞”进行交换，然后继续检查节点的新父节点。
		 */
		heap->bh_nodes[node_off] = parent_val;
		node_off = parent_off;
	}
	/* Re-fill the hole - 重新填充“洞” */
	heap->bh_nodes[node_off] = node_val;
}

/*
 * Sift a node down from its current position to satisfy the heap
 * property.
 * Sift a node down from its current position to satisfy the heap
 * property.
 * 从节点的当前位置向下筛选节点以满足堆属性。
 *
 * Function Role: Sift down a node to restore heap property.
 * 函数作用：将一个节点下滤以恢复堆性质。
 */
static void
sift_down(binaryheap *heap, int node_off)
{
	bh_node_type node_val = heap->bh_nodes[node_off];

	/*
	 * Within the loop, the node_off'th array entry is a "hole" that
	 * notionally holds node_val, but we don't actually store node_val there
	 * till the end, saving some unnecessary data copying steps.
	 * Within the loop, the node_off'th array entry is a "hole" that
	 * notionally holds node_val, but we don't actually store node_val there
	 * till the end, saving some unnecessary data copying steps.
	 * 在循环中，第 node_off 个数组条目是一个概念上容纳 node_val 的“洞”，
	 * 但在结束之前，我们实际上并没有把 node_val 存放在那里，这省去了一些不必要的数据复制步骤。
	 */
	while (true)
	{
		int			left_off = left_offset(node_off);
		int			right_off = right_offset(node_off);
		int			swap_off = left_off;

		/* Is the right child larger than the left child? - 右子节点是否大于左子节点？ */
		if (right_off < heap->bh_size &&
			heap->bh_compare(heap->bh_nodes[left_off],
							 heap->bh_nodes[right_off],
							 heap->bh_arg) < 0)
			swap_off = right_off;

		/*
		 * If no children or parent is >= the larger child, heap condition is
		 * satisfied, and we're done.
		 * If no children or parent is >= the larger child, heap condition is
		 * satisfied, and we're done.
		 * 如果没有子节点，或者父节点 >= 较大的子节点，则堆条件已满足，我们就完成了。
		 */
		if (left_off >= heap->bh_size ||
			heap->bh_compare(node_val,
							 heap->bh_nodes[swap_off],
							 heap->bh_arg) >= 0)
			break;

		/*
		 * Otherwise, swap the hole with the child that violates the heap
		 * property; then go on to check its children.
		 * Otherwise, swap the hole with the child that violates the heap
		 * property; then go on to check its children.
		 * 否则，将该“洞”与违反堆属性的子节点进行交换；然后继续检查其子节点。
		 */
		heap->bh_nodes[node_off] = heap->bh_nodes[swap_off];
		node_off = swap_off;
	}
	/* Re-fill the hole - 重新填充“洞” */
	heap->bh_nodes[node_off] = node_val;
}
