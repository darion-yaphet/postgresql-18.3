/*-------------------------------------------------------------------------
 *
 * nodes.h
 *	  Definitions for tagged nodes.
 *	  带标签节点的定义。
 *
 * Core Flow Explanation / 核心流程说明:
 * - The Node System: PostgreSQL uses a custom object-oriented system where every data structure
 *   (Query, Plan, Expr, etc.) starts with a NodeTag field.
 * - 节点系统：PostgreSQL 使用一套自定义的面向对象系统，其中每个数据结构（如 Query, Plan, Expr 等）都以 NodeTag 字段开头。
 * - Generic Pointer: By convention, a pointer to any tagged struct can be cast to (Node *) safely.
 * - 通用指针：按照约定，指向任何带标签结构体的指针都可以安全地转换为 (Node *)。
 * - Macros: Use makeNode() to allocate, IsA() to check type, and castNode() to safely cast.
 * - 宏：使用 makeNode() 进行内存分配，使用 IsA() 检查类型，使用 castNode() 进行安全转换。
 * - Support Scripts: gen_node_support.pl uses pg_node_attr() markers to auto-generate code for
 *   copying, equality testing, and serialization (reading/writing nodes).
 * - 支撑脚本：gen_node_support.pl 使用 pg_node_attr() 标记来自动生成用于复制、等值测试和序列化（读/写节点）的代码。
 * - Serialization/Deserialization: Use nodeToString() to convert a node tree into a string
 *   and stringToNode() to restore it. This is used for storing views, rules, and rules in the catalogs.
 * - 序列化与反序列化：使用 nodeToString() 将节点树转换为字符串，使用 stringToNode() 将其还原。
 *   这用于在目录中存储视图、规则和规则等信息。
 * - Deep Copy and Equality: copyObject() performs a deep copy of a node tree, and equal() compares
 *   two node trees for semantic equality.
 * - 深拷贝与等值性：copyObject() 对节点树进行深拷贝，equal() 比较两棵节点树在语义上是否相等。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/nodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODES_H
#define NODES_H

/*
 * The first field of every node is NodeTag. Each node created (with makeNode)
 * will have one of the following tags as the value of its first field.
 * 每一个节点的第一个字段都是 NodeTag。每一个创建的节点（通过 makeNode）
 * 都会将以下标签之一作为其第一个字段的值。
 *
 * Note that inserting or deleting node types changes the numbers of other
 * node types later in the list.  This is no problem during development, since
 * the node numbers are never stored on disk.  But don't do it in a released
 * branch, because that would represent an ABI break for extensions.
 * 注意，插入或删除节点类型会改变列表中后续其他节点类型的编号。
 * 这种变动在开发阶段没有问题，因为节点编号从未存储在磁盘上。
 * 但不要在已发布的版本分支中这样做，因为这会导致扩展程序的 ABI（应用程序二进制接口）断裂。
 */
typedef enum NodeTag
{
	T_Invalid = 0,

#include "nodes/nodetags.h"
} NodeTag;

/*
 * pg_node_attr() - Used in node definitions to set extra information for
 * gen_node_support.pl
 * pg_node_attr() - 用于在节点定义中为 gen_node_support.pl 设置额外信息。
 *
 * Attributes can be attached to a node as a whole (place the attribute
 * specification on the first line after the struct's opening brace)
 * or to a specific field (place it at the end of that field's line).  The
 * argument is a comma-separated list of attributes.  Unrecognized attributes
 * cause an error.
 * 属性可以附加到整个节点上（将属性规范放在结构体左大括号后的第一行），
 * 也可以附加到特定字段上（放在该字段所在行的末尾）。参数是一个逗号分隔的属性列表。
 * 无法识别的属性会导致错误。
 *
 * Valid node attributes:
 * 有效的节点属性：
 *
 * - abstract: Abstract types are types that cannot be instantiated but that
 *   can be supertypes of other types.  We track their fields, so that
 *   subtypes can use them, but we don't emit a node tag, so you can't
 *   instantiate them.
 * - abstract（抽象）：抽象类型是不能被实例化的类型，但它们可以作为其他类型的父类型。
 *   我们跟踪它们的字段以便子类型使用，但我们不生成节点标签，因此你无法实例化它们。
 *
 * - custom_copy_equal: Has custom implementations in copyfuncs.c and
 *   equalfuncs.c.
 * - custom_copy_equal：在 copyfuncs.c 和 equalfuncs.c 中有自定义实现。
 *
 * - custom_read_write: Has custom implementations in outfuncs.c and
 *   readfuncs.c.
 * - custom_read_write：在 outfuncs.c 和 readfuncs.c 中有自定义实现。
 *
 * - custom_query_jumble: Has custom implementation in queryjumblefuncs.c.
 *   Also available as a node field attribute.
 * - custom_query_jumble：在 queryjumblefuncs.c 中有自定义实现。也可以作为节点字段属性使用。
 *
 * - no_copy: Does not support copyObject() at all.
 * - no_copy：完全不支持 copyObject()。
 *
 * - no_equal: Does not support equal() at all.
 * - no_equal：完全不支持 equal()。
 *
 * - no_copy_equal: Shorthand for both no_copy and no_equal.
 * - no_copy_equal：no_copy 和 no_equal 的简写。
 *
 * - no_query_jumble: Does not support JumbleQuery() at all.
 * - no_query_jumble：完全不支持 JumbleQuery()。
 *
 * - no_read: Does not support nodeRead() at all.
 * - no_read：完全不支持 nodeRead()。
 *
 * - nodetag_only: Does not support copyObject(), equal(), jumbleQuery()
 *   outNode() or nodeRead().
 * - nodetag_only：不支持 copyObject()、equal()、jumbleQuery()、outNode() 或 nodeRead()。
 *
 * - special_read_write: Has special treatment in outNode() and nodeRead().
 * - special_read_write：在 outNode() 和 nodeRead() 中有特殊处理。
 *
 * - nodetag_number(VALUE): assign the specified nodetag number instead of
 *   an auto-generated number.  Typically this would only be used in stable
 *   branches, to give a newly-added node type a number without breaking ABI
 *   by changing the numbers of existing node types.
 * - nodetag_number(VALUE)：分配指定的节点标签编号，而不是自动生成的编号。这通常仅在稳定分支中使用，
 *   以便为新添加的节点类型分配一个编号，而不会通过改变现有节点类型的编号来破坏 ABI。
 *
 * Node types can be supertypes of other types whether or not they are marked
 * abstract: if a node struct appears as the first field of another struct
 * type, then it is the supertype of that type.  The no_copy, no_equal,
 * no_query_jumble and no_read node attributes are automatically inherited
 * from the supertype.  (Notice that nodetag_only does not inherit, so it's
 * not quite equivalent to a combination of other attributes.)
 * 节点类型无论是否被标记为抽象，都可以是其他类型的父类型：如果一个节点结构体作为另一个结构体类型
 * 的第一个字段出现，那么它就是该类型的父类型。no_copy、no_equal、no_query_jumble 和 no_read
 * 节点属性会自动从父类型继承。（注意，nodetag_only 不会被继承，因此它并不完全等同于其他属性的组合。）
 *
 * Valid node field attributes:
 * 有效的节点字段属性：
 *
 * - array_size(OTHERFIELD): This field is a dynamically allocated array with
 *   size indicated by the mentioned other field.  The other field is either a
 *   scalar or a list, in which case the length of the list is used.
 * - array_size(OTHERFIELD)：此字段是一个动态分配的数组，其大小由提到的另一个字段指示。
 *   另一个字段可以是标量或列表，如果是列表，则使用列表的长度。
 *
 * - copy_as(VALUE): In copyObject(), replace the field's value with VALUE.
 * - copy_as(VALUE)：在 copyObject() 中，将字段的值替换为 VALUE。
 *
 * - copy_as_scalar: In copyObject(), copy the field as a scalar value
 *   (e.g. a pointer) even if it is a node-type pointer.
 * - copy_as_scalar：在 copyObject() 中，即使该字段是节点类型的指针，也将其作为标量值（例如指针）进行复制。
 *
 * - equal_as_scalar: In equal(), compare the field as a scalar value
 *   even if it is a node-type pointer.
 * - equal_as_scalar：在 equal() 中，即使该字段是节点类型的指针，也将其作为标量值进行比较。
 *
 * - equal_ignore: Ignore the field for equality.
 * - equal_ignore：在比较等值性时忽略此字段。
 *
 * - equal_ignore_if_zero: Ignore the field for equality if it is zero.
 *   (Otherwise, compare normally.)
 * - equal_ignore_if_zero：如果此字段为零，则在比较等值性时忽略它。（否则，正常进行比较。）
 *
 * - custom_query_jumble: Has custom implementation in queryjumblefuncs.c
 *   for the field of a node.  Also available as a node attribute.
 * - custom_query_jumble：在 queryjumblefuncs.c 中对节点的字段有自定义实现。也可以作为节点属性使用。
 *
 * - query_jumble_ignore: Ignore the field for the query jumbling.  Note
 *   that typmod and collation information are usually irrelevant for the
 *   query jumbling.
 * - query_jumble_ignore：在进行查询杂凑（query jumbling）时忽略此字段。注意，typmod 和
 *   排序规则（collation）信息通常与查询杂凑无关。
 *
 * - query_jumble_squash: Squash multiple values during query jumbling.
 * - query_jumble_squash：在查询杂凑期间合并多个值。
 *
 * - query_jumble_location: Mark the field as a location to track.  This is
 *   only allowed for integer fields that include "location" in their name.
 * - query_jumble_location：将此字段标记为要跟踪的位置。这仅允许用于名称中包含 “location” 的整数阶段。
 *
 * - read_as(VALUE): In nodeRead(), replace the field's value with VALUE.
 * - read_as(VALUE)：在 nodeRead() 中，将字段的值替换为 VALUE。
 *
 * - read_write_ignore: Ignore the field for read/write.  This is only allowed
 *   if the node type is marked no_read or read_as() is also specified.
 * - read_write_ignore：在读/写操作中忽略此字段。这仅在节点类型被标记为 no_read 或同时指定了 read_as() 时才被允许。
 *
 * - write_only_relids, write_only_nondefault_pathtarget, write_only_req_outer:
 *   Special handling for Path struct; see there.
 * - write_only_relids, write_only_nondefault_pathtarget, write_only_req_outer：
 *   针对 Path 结构体的特殊处理；详见该处。
 */
#define pg_node_attr(...)

/*
 * The first field of a node of any type is guaranteed to be the NodeTag.
 * Hence the type of any node can be gotten by casting it to Node. Declaring
 * a variable to be of Node * (instead of void *) can also facilitate
 * debugging.
 * 任何类型的节点的第一个字段都保证是 NodeTag。因此，通过将其转换为 Node，
 * 可以获取任何节点的类型。声明一个变量为 Node *（而不是 void *）也可以方便调试。
 */
typedef struct Node
{
	NodeTag		type;
} Node;

#define nodeTag(nodeptr)		(((const Node*)(nodeptr))->type)

/*
 * newNode -
 *	  create a new node of the specified size and tag the node with the
 *	  specified tag.
 * newNode -
 *	  创建一个指定大小的新节点，并用指定的标签为该节点打上标签。
 *
 * !WARNING!: Avoid using newNode directly. You should be using the
 *	  macro makeNode.  eg. to create a Query node, use makeNode(Query)
 * ！警告！：避免直接使用 newNode。你应该使用 makeNode 宏。
 * 例如，要创建一个 Query 节点，请使用 makeNode(Query)。
 */
static inline Node *
newNode(size_t size, NodeTag tag)
{
	Node	   *result;

	Assert(size >= sizeof(Node));	/* need the tag, at least */
	result = (Node *) palloc0(size);
	result->type = tag;

	return result;
}

#define makeNode(_type_)		((_type_ *) newNode(sizeof(_type_),T_##_type_))
#define NodeSetTag(nodeptr,t)	(((Node*)(nodeptr))->type = (t))

#define IsA(nodeptr,_type_)		(nodeTag(nodeptr) == T_##_type_)

/*
 * castNode(type, ptr) casts ptr to "type *", and if assertions are enabled,
 * verifies that the node has the appropriate type (using its nodeTag()).
 * castNode(type, ptr) 将 ptr 转换为 "type *"，如果启用了断言，
 * 则验证该节点是否具有适当的类型（使用其 nodeTag()）。
 *
 * Use an inline function when assertions are enabled, to avoid multiple
 * evaluations of the ptr argument (which could e.g. be a function call).
 * 当启用断言时，使用内联函数以避免对 ptr 参数的多重评估（ptr 可能会是函数调用）。
 */
#ifdef USE_ASSERT_CHECKING
/*
 * castNodeImpl -
 *	  Low-level implementation of castNode macro for assertion checking.
 * castNodeImpl -
 *	  用于断言检查的 castNode 宏的底层实现。
 */
static inline Node *
castNodeImpl(NodeTag type, void *ptr)
{
	Assert(ptr == NULL || nodeTag(ptr) == type);
	return (Node *) ptr;
}
#define castNode(_type_, nodeptr) ((_type_ *) castNodeImpl(T_##_type_, nodeptr))
#else
#define castNode(_type_, nodeptr) ((_type_ *) (nodeptr))
#endif							/* USE_ASSERT_CHECKING */


/* ----------------------------------------------------------------
 *					  extern declarations follow
 * ----------------------------------------------------------------
 */

/*
 * nodes/{outfuncs.c,print.c}
 * 节点序列化（输出）和打印相关的函数
 */
struct Bitmapset;				/* not to include bitmapset.h here */
struct StringInfoData;			/* not to include stringinfo.h here */

/*
 * outNode -
 *	  Serialize a node into a StringInfoData buffer.
 * outNode -
 *	  将一个节点序列化到 StringInfoData 缓冲区中。
 */
extern void outNode(struct StringInfoData *str, const void *obj);

/*
 * outToken -
 *	  Output a single token (string) with necessary escaping.
 * outToken -
 *	  输出一个带有必要转义的单个标记（字符串）。
 */
extern void outToken(struct StringInfoData *str, const char *s);

/*
 * outBitmapset -
 *	  Serialize a Bitmapset into a StringInfoData buffer.
 * outBitmapset -
 *	  将一个 Bitmapset 序列化到 StringInfoData 缓冲区中。
 */
extern void outBitmapset(struct StringInfoData *str,
						 const struct Bitmapset *bms);

/*
 * outDatum -
 *	  Serialize a Datum into a StringInfoData buffer.
 * outDatum -
 *	  将一个 Datum 序列化到 StringInfoData 缓冲区中。
 */
extern void outDatum(struct StringInfoData *str, uintptr_t value,
					 int typlen, bool typbyval);

/*
 * nodeToString -
 *	  Convenience wrapper to serialize a node tree into a C string.
 * nodeToString -
 *	  将节点树序列化为 C 字符串的便捷封装。
 */
extern char *nodeToString(const void *obj);

/*
 * nodeToStringWithLocations -
 *	  Similar to nodeToString, but includes parse location fields.
 * nodeToStringWithLocations -
 *	  类似于 nodeToString，但包含解析位置字段。
 */
extern char *nodeToStringWithLocations(const void *obj);

/*
 * bmsToString -
 *	  Convert a Bitmapset to its string representation.
 * bmsToString -
 *	  将 Bitmapset 转换为其字符串表示形式。
 */
extern char *bmsToString(const struct Bitmapset *bms);

/*
 * nodes/{readfuncs.c,read.c}
 * 节点从字符串还原（读取）相关的函数
 */

/*
 * stringToNode -
 *	  Deserialize a string representation into a node tree.
 * stringToNode -
 *	  将字符串表示形式反序列化为节点树。
 */
extern void *stringToNode(const char *str);

#ifdef DEBUG_NODE_TESTS_ENABLED
/*
 * stringToNodeWithLocations -
 *	  Similar to stringToNode, but handles location fields correctly.
 * stringToNodeWithLocations -
 *	  类似于 stringToNode，但能正确处理位置字段。
 */
extern void *stringToNodeWithLocations(const char *str);
#endif

/*
 * readBitmapset -
 *	  Internal function to read a Bitmapset from the current input string.
 * readBitmapset -
 *	  从当前输入字符串中读取 Bitmapset 的内部函数。
 */
extern struct Bitmapset *readBitmapset(void);

/*
 * readDatum -
 *	  Internal function to read a Datum from the current input string.
 * readDatum -
 *	  从当前输入字符串中读取 Datum 的内部函数。
 */
extern uintptr_t readDatum(bool typbyval);

/*
 * readBoolCols, readIntCols, readOidCols, readAttrNumberCols -
 *	  Internal functions to read arrays of specific types.
 * readBoolCols, readIntCols, readOidCols, readAttrNumberCols -
 *	  读取特定类型数组的内部函数。
 */
extern bool *readBoolCols(int numCols);
extern int *readIntCols(int numCols);
extern Oid *readOidCols(int numCols);
extern int16 *readAttrNumberCols(int numCols);

/*
 * nodes/copyfuncs.c
 * 节点复制相关的函数
 */

/*
 * copyObjectImpl -
 *	  Internal implementation for deep-copying a node tree.
 * copyObjectImpl -
 *	  深度复制节点树的内部实现。
 */
extern void *copyObjectImpl(const void *from);

/* cast result back to argument type, if supported by compiler */
#ifdef HAVE_TYPEOF
#define copyObject(obj) ((typeof(obj)) copyObjectImpl(obj))
#else
#define copyObject(obj) copyObjectImpl(obj)
#endif

/*
 * nodes/equalfuncs.c
 * 节点等值比较相关的函数
 */

/*
 * equal -
 *	  Compare two node trees for equality (semantic equivalence).
 * equal -
 *	  比较两棵节点树是否相等（语义等价）。
 */
extern bool equal(const void *a, const void *b);


/*
 * Typedef for parse location.  This is just an int, but this way
 * gen_node_support.pl knows which fields should get special treatment for
 * location values.
 * 解析位置的类型定义。这只是一个 int，但通过这种方式，
 * gen_node_support.pl 知道哪些字段应该得到位置值的特殊处理。
 *
 * -1 is used for unknown.
 * -1 表示位置未知。
 */
typedef int ParseLoc;

/*
 * Typedefs for identifying qualifier selectivities, plan costs, and row
 * counts as such.  These are just plain "double"s, but declaring a variable
 * as Selectivity, Cost, or Cardinality makes the intent more obvious.
 * 用于标识限定符选择性、计划代价和行数的类型定义。这些只是普通的 "double"，
 * 但将变量声明为 Selectivity、Cost 或 Cardinality 可以使意图更加明显。
 *
 * These could have gone into plannodes.h or some such, but many files
 * depend on them...
 * 这些本可以放在 plannodes.h 或类似文件中，但许多文件都依赖于它们……
 */
typedef double Selectivity;		/* fraction of tuples a qualifier will pass */
								/* 限定符允许通过的元组比例 */
typedef double Cost;			/* execution cost (in page-access units) */
								/* 执行代价（以页面访问为单位） */
typedef double Cardinality;		/* (estimated) number of rows or other integer
								 * count */
								/* （估算的）行数或其他整数计数 */


/*
 * CmdType -
 *	  enums for type of operation represented by a Query or PlannedStmt
 * CmdType -
 *	  由 Query 或 PlannedStmt 表示的操作类型的枚举
 *
 * This is needed in both parsenodes.h and plannodes.h, so put it here...
 * 这在 parsenodes.h 和 plannodes.h 中都需要，所以放在这里……
 */
typedef enum CmdType
{
	CMD_UNKNOWN,
	CMD_SELECT,					/* select stmt */
								/* select 语句 */
	CMD_UPDATE,					/* update stmt */
								/* update 语句 */
	CMD_INSERT,					/* insert stmt */
								/* insert 语句 */
	CMD_DELETE,					/* delete stmt */
								/* delete 语句 */
	CMD_MERGE,					/* merge stmt */
								/* merge 语句 */
	CMD_UTILITY,				/* cmds like create, destroy, copy, vacuum,
								 * etc. */
								/* 诸如 create, destroy, copy, vacuum 等辅助命令 */
	CMD_NOTHING,				/* dummy command for instead nothing rules
								 * with qual */
								/* 带有限定条件的 INSTEAD NOTHING 规则的虚构命令 */
} CmdType;


/*
 * JoinType -
 *	  enums for types of relation joins
 * JoinType -
 *	  关系连接类型的枚举
 *
 * JoinType determines the exact semantics of joining two relations using
 * a matching qualification.  For example, it tells what to do with a tuple
 * that has no match in the other relation.
 * JoinType 决定了使用匹配限定条件连接两个关系的精确语义。例如，它说明了如何处理在另一个关系中没有匹配项的元组。
 *
 * This is needed in both parsenodes.h and plannodes.h, so put it here...
 * 这在 parsenodes.h 和 plannodes.h 中都需要，所以放在这里……
 */
typedef enum JoinType
{
	/*
	 * The canonical kinds of joins according to the SQL JOIN syntax. Only
	 * these codes can appear in parser output (e.g., JoinExpr nodes).
	 * 根据 SQL JOIN 语法的典型连接种类。只有这些代码能出现在解析器的输出中（例如 JoinExpr 节点）。
	 */
	JOIN_INNER,					/* matching tuple pairs only */
								/* 仅匹配的元组对 */
	JOIN_LEFT,					/* pairs + unmatched LHS tuples */
								/* 匹配对 + LHS（左侧）不匹配的元组 */
	JOIN_FULL,					/* pairs + unmatched LHS + unmatched RHS */
								/* 匹配对 + LHS 不匹配 + RHS 不匹配 */
	JOIN_RIGHT,					/* pairs + unmatched RHS tuples */
								/* 匹配对 + RHS（右侧）不匹配的元组 */

	/*
	 * Semijoins and anti-semijoins (as defined in relational theory) do not
	 * appear in the SQL JOIN syntax, but there are standard idioms for
	 * representing them (e.g., using EXISTS).  The planner recognizes these
	 * cases and converts them to joins.  So the planner and executor must
	 * support these codes.  NOTE: in JOIN_SEMI output, it is unspecified
	 * which matching RHS row is joined to.  In JOIN_ANTI output, the row is
	 * guaranteed to be null-extended.
	 * 半连接（Semijoin）和反半连接（Anti-semijoin）（如关系理论中所定义）不会出现在 SQL JOIN 语法中，
	 * 但有标准的习惯用法来表示它们（例如使用 EXISTS）。规划器（planner）会识别这些情况并将其转换为连接。
	 * 因此规划器和执行器必须支持这些代码。
	 * 注意：在 JOIN_SEMI 的输出中，未指定连接到哪个匹配的 RHS 行。在 JOIN_ANTI 的输出中，该行保证会被 NULL 扩展。
	 */
	JOIN_SEMI,					/* 1 copy of each LHS row that has match(es) */
								/* 每个有匹配项的 LHS 行的一份副本 */
	JOIN_ANTI,					/* 1 copy of each LHS row that has no match */
								/* 每个没有匹配项的 LHS 行的一份副本 */
	JOIN_RIGHT_SEMI,			/* 1 copy of each RHS row that has match(es) */
								/* 每个有匹配项的 RHS 行的一份副本 */
	JOIN_RIGHT_ANTI,			/* 1 copy of each RHS row that has no match */
								/* 每个没有匹配项的 RHS 行的一份副本 */

	/*
	 * These codes are used internally in the planner, but are not supported
	 * by the executor (nor, indeed, by most of the planner).
	 * 这些代码仅在规划器内部使用，执行器不支持（事实上，规划器的大部分也不支持）。
	 */
	JOIN_UNIQUE_OUTER,			/* LHS path must be made unique */
								/* LHS 路径必须设计为唯一的 */
	JOIN_UNIQUE_INNER,			/* RHS path must be made unique */
								/* RHS 路径必须设计为唯一的 */

	/*
	 * We might need additional join types someday.
	 * 将来我们可能需要额外的连接类型。
	 */
} JoinType;

/*
 * OUTER joins are those for which pushed-down quals must behave differently
 * from the join's own quals.  This is in fact everything except INNER, SEMI
 * and RIGHT_SEMI joins.  However, this macro must also exclude the
 * JOIN_UNIQUE symbols since those are temporary proxies for what will
 * eventually be an INNER join.
 * OUTER（外部连接）是指那些下推的限定条件（quals）其行为必须与连接自身的限定条件有所不同的连接。
 * 事实上，除了 INNER、SEMI 和 RIGHT_SEMI 连接外，其他所有连接都是外部连接。
 * 然而，该宏还必须排除 JOIN_UNIQUE 符号，因为这些符号是最终将成为 INNER 连接的临时代理。
 *
 * Note: semijoins are a hybrid case, but we choose to treat them as not
 * being outer joins.  This is okay principally because the SQL syntax makes
 * it impossible to have a pushed-down qual that refers to the inner relation
 * of a semijoin; so there is no strong need to distinguish join quals from
 * pushed-down quals.  This is convenient because for almost all purposes,
 * quals attached to a semijoin can be treated the same as innerjoin quals.
 * 注意：半连接（semijoin）是一个混合情况，但我们选择不将它们视为外部连接。
 * 这主要是因为 SQL 语法使得不可能存在引用半连接内部关系的下推限定条件；
 * 因此没有强烈的必要去区分连接限定条件和下推限定条件。这很方便，
 * 因为对于几乎所有目的，附加到半连接的限定条件都可以被视为与内连接（innerjoin）限定条件相同。
 */
#define IS_OUTER_JOIN(jointype) \
	(((1 << (jointype)) & \
	  ((1 << JOIN_LEFT) | \
	   (1 << JOIN_FULL) | \
	   (1 << JOIN_RIGHT) | \
	   (1 << JOIN_ANTI) | \
	   (1 << JOIN_RIGHT_ANTI))) != 0)

/*
 * AggStrategy -
 *	  overall execution strategies for Agg plan nodes
 * AggStrategy -
 *	  Agg（聚合）计划节点的总体执行策略
 *
 * This is needed in both pathnodes.h and plannodes.h, so put it here...
 * 这在 pathnodes.h 和 plannodes.h 中都需要，所以放在这里……
 */
typedef enum AggStrategy
{
	AGG_PLAIN,					/* simple agg across all input rows */
								/* 对所有输入行进行简单聚合 */
	AGG_SORTED,					/* grouped agg, input must be sorted */
								/* 分组聚合，输入必须已排序 */
	AGG_HASHED,					/* grouped agg, use internal hashtable */
								/* 分组聚合，使用内部哈希表 */
	AGG_MIXED,					/* grouped agg, hash and sort both used */
								/* 分组聚合，混合使用哈希和排序 */
} AggStrategy;

/*
 * AggSplit -
 *	  splitting (partial aggregation) modes for Agg plan nodes
 * AggSplit -
 *	  Agg 计划节点的拆分（部分聚合）模式
 *
 * This is needed in both pathnodes.h and plannodes.h, so put it here...
 * 这在 pathnodes.h 和 plannodes.h 中都需要，所以放在这里……
 */

/* Primitive options supported by nodeAgg.c: */
/* nodeAgg.c 支持的基础选项： */
#define AGGSPLITOP_COMBINE		0x01	/* substitute combinefn for transfn */
										/* 用 combinefn 替换 transfn */
#define AGGSPLITOP_SKIPFINAL	0x02	/* skip finalfn, return state as-is */
										/* 跳过 finalfn，原样返回状态 */
#define AGGSPLITOP_SERIALIZE	0x04	/* apply serialfn to output */
										/* 对输出应用序列化函数 serialfn */
#define AGGSPLITOP_DESERIALIZE	0x08	/* apply deserialfn to input */
										/* 对输入应用反序列化函数 deserialfn */

/* Supported operating modes (i.e., useful combinations of these options): */
/* 支持的运行模式（即这些选项的有用组合）： */
typedef enum AggSplit
{
	/* Basic, non-split aggregation: */
	/* 基础、非拆分聚合： */
	AGGSPLIT_SIMPLE = 0,
	/* Initial phase of partial aggregation, with serialization: */
	/* 部分聚合的初始阶段，带有序列化： */
	AGGSPLIT_INITIAL_SERIAL = AGGSPLITOP_SKIPFINAL | AGGSPLITOP_SERIALIZE,
	/* Final phase of partial aggregation, with deserialization: */
	/* 部分聚合的最终阶段，带有反序列化： */
	AGGSPLIT_FINAL_DESERIAL = AGGSPLITOP_COMBINE | AGGSPLITOP_DESERIALIZE,
} AggSplit;

/* Test whether an AggSplit value selects each primitive option: */
/* 测试 AggSplit 值是否选择了每个基础选项： */
#define DO_AGGSPLIT_COMBINE(as)		(((as) & AGGSPLITOP_COMBINE) != 0)
#define DO_AGGSPLIT_SKIPFINAL(as)	(((as) & AGGSPLITOP_SKIPFINAL) != 0)
#define DO_AGGSPLIT_SERIALIZE(as)	(((as) & AGGSPLITOP_SERIALIZE) != 0)
#define DO_AGGSPLIT_DESERIALIZE(as) (((as) & AGGSPLITOP_DESERIALIZE) != 0)

/*
 * SetOpCmd and SetOpStrategy -
 *	  overall semantics and execution strategies for SetOp plan nodes
 * SetOpCmd 和 SetOpStrategy -
 *	  SetOp（集合操作）计划节点的总体语义和执行策略
 *
 * This is needed in both pathnodes.h and plannodes.h, so put it here...
 * 这在 pathnodes.h 和 plannodes.h 中都需要，所以放在这里……
 */
typedef enum SetOpCmd
{
	SETOPCMD_INTERSECT,			/* INTERSECT operation */
								/* INTERSECT（交集）操作 */
	SETOPCMD_INTERSECT_ALL,		/* INTERSECT ALL operation */
								/* INTERSECT ALL（交集，保留重复）操作 */
	SETOPCMD_EXCEPT,			/* EXCEPT operation */
								/* EXCEPT（差集）操作 */
	SETOPCMD_EXCEPT_ALL,		/* EXCEPT ALL operation */
								/* EXCEPT ALL（差集，保留重复）操作 */
} SetOpCmd;

typedef enum SetOpStrategy
{
	SETOP_SORTED,				/* input must be sorted */
								/* 输入必须已排序 */
	SETOP_HASHED,				/* use internal hashtable */
								/* 使用内部哈希表 */
} SetOpStrategy;

/*
 * OnConflictAction -
 *	  "ON CONFLICT" clause type of query
 * OnConflictAction -
 *	  查询的 "ON CONFLICT" 子句类型
 *
 * This is needed in both parsenodes.h and plannodes.h, so put it here...
 * 这在 parsenodes.h 和 plannodes.h 中都需要，所以放在这里……
 */
typedef enum OnConflictAction
{
	ONCONFLICT_NONE,			/* No "ON CONFLICT" clause */
								/* 无 "ON CONFLICT" 子句 */
	ONCONFLICT_NOTHING,			/* ON CONFLICT ... DO NOTHING */
								/* ON CONFLICT ... DO NOTHING */
	ONCONFLICT_UPDATE,			/* ON CONFLICT ... DO UPDATE */
								/* ON CONFLICT ... DO UPDATE */
} OnConflictAction;

/*
 * LimitOption -
 *	LIMIT option of query
 * LimitOption -
 *	查询的 LIMIT 选项
 *
 * This is needed in both parsenodes.h and plannodes.h, so put it here...
 * 这在 parsenodes.h 和 plannodes.h 中都需要，所以放在这里……
 */
typedef enum LimitOption
{
	LIMIT_OPTION_COUNT,			/* FETCH FIRST... ONLY */
								/* FETCH FIRST... ONLY */
	LIMIT_OPTION_WITH_TIES,		/* FETCH FIRST... WITH TIES */
								/* FETCH FIRST... WITH TIES */
} LimitOption;

#endif							/* NODES_H */
