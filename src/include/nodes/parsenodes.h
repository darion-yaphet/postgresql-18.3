/*-------------------------------------------------------------------------
 *
 * parsenodes.h
 *	  definitions for parse tree nodes
 *
 * Many of the node types used in parsetrees include a "location" field.
 * This is a byte (not character) offset in the original source text, to be
 * used for positioning an error cursor when there is an error related to
 * the node.  Access to the original source text is needed to make use of
 * the location.  At the topmost (statement) level, we also provide a
 * statement length, likewise measured in bytes, for convenience in
 * identifying statement boundaries in multi-statement source strings.
 *
 * 解析树节点的定义。
 * 解析树中使用的许多节点类型都包含一个 "location" 字段。
 * 这是原始源文本中的字节（非字符）偏移量，用于在发生与节点相关的错误时
 * 定位错误光标。使用该位置信息需要访问原始源文本。
 * 在最顶层（语句）级别，我们还提供语句长度（同样以字节为单位），
 * 以便于识别多语句源字符串中的语句边界。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/parsenodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSENODES_H
#define PARSENODES_H

#include "common/relpath.h"
#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "partitioning/partdefs.h"

/* Possible sources of a Query */
/* Query 的可能来源 */
typedef enum QuerySource
{
  QSRC_ORIGINAL,          /* original parsetree (explicit query) */
                          /* 原始解析树（显式查询） */
  QSRC_PARSER,            /* added by parse analysis (now unused) */
                          /* 由解析分析添加（现已不使用） */
  QSRC_INSTEAD_RULE,      /* added by unconditional INSTEAD rule */
                          /* 由无条件 INSTEAD 规则添加 */
  QSRC_QUAL_INSTEAD_RULE, /* added by conditional INSTEAD rule */
                          /* 由条件 INSTEAD 规则添加 */
  QSRC_NON_INSTEAD_RULE,  /* added by non-INSTEAD rule */
                          /* 由非 INSTEAD 规则添加 */
} QuerySource;

/* Sort ordering options for ORDER BY and CREATE INDEX */
/* ORDER BY 和 CREATE INDEX 的排序选项 */
typedef enum SortByDir
{
  SORTBY_DEFAULT,
  SORTBY_ASC,
  SORTBY_DESC,
  SORTBY_USING, /* not allowed in CREATE INDEX ... */
} SortByDir;

typedef enum SortByNulls
{
  SORTBY_NULLS_DEFAULT,
  SORTBY_NULLS_FIRST,
  SORTBY_NULLS_LAST,
} SortByNulls;

/* Options for [ ALL | DISTINCT ] */
/* [ ALL | DISTINCT ] 的选项 */
typedef enum SetQuantifier
{
  SET_QUANTIFIER_DEFAULT,
  SET_QUANTIFIER_ALL,
  SET_QUANTIFIER_DISTINCT,
} SetQuantifier;

/*
 * Grantable rights are encoded so that we can OR them together in a bitmask.
 * The present representation of AclItem limits us to 32 distinct rights,
 * even though AclMode is defined as uint64.  See utils/acl.h.
 *
 * Caution: changing these codes breaks stored ACLs, hence forces initdb.
 *
 * 可授予的权限以位掩码方式编码，以便通过 OR 运算组合。
 * 当前 AclItem 的表示方式将权限数量限制为 32 种，
 * 尽管 AclMode 被定义为 uint64。详见 utils/acl.h。
 *
 * 注意：更改这些编码会破坏已存储的 ACL，因此需要重新执行 initdb。
 */
typedef uint64 AclMode; /* a bitmask of privilege bits */
                        /* 权限位的位掩码 */

#define ACL_INSERT (1 << 0) /* for relations */
#define ACL_SELECT (1 << 1)
#define ACL_UPDATE (1 << 2)
#define ACL_DELETE (1 << 3)
#define ACL_TRUNCATE (1 << 4)
#define ACL_REFERENCES (1 << 5)
#define ACL_TRIGGER (1 << 6)
#define ACL_EXECUTE (1 << 7)       /* for functions */
#define ACL_USAGE (1 << 8)         /* for various object types */
#define ACL_CREATE (1 << 9)        /* for namespaces and databases */
#define ACL_CREATE_TEMP (1 << 10)  /* for databases */
#define ACL_CONNECT (1 << 11)      /* for databases */
#define ACL_SET (1 << 12)          /* for configuration parameters */
#define ACL_ALTER_SYSTEM (1 << 13) /* for configuration parameters */
#define ACL_MAINTAIN (1 << 14)     /* for relations */
#define N_ACL_RIGHTS 15            /* 1 plus the last 1<<x */
#define ACL_NO_RIGHTS 0
/* Currently, SELECT ... FOR [KEY] UPDATE/SHARE requires UPDATE privileges */
#define ACL_SELECT_FOR_UPDATE ACL_UPDATE

/*****************************************************************************
 *	Query Tree
 *****************************************************************************/

/*
 * Query -
 *	  Parse analysis turns all statements into a Query tree
 *	  for further processing by the rewriter and planner.
 *
 *	  Utility statements (i.e. non-optimizable statements) have the
 *	  utilityStmt field set, and the rest of the Query is mostly dummy.
 *
 *	  Planning converts a Query tree into a Plan tree headed by a
 *PlannedStmt node --- the Query structure is not used by the executor.
 *
 *	  All the fields ignored for the query jumbling are not semantically
 *	  significant (such as alias names), as is ignored anything that can
 *	  be deduced from child nodes (else we'd just be double-hashing that
 *	  piece of information).
 *
 * Query -
 *	  解析分析将所有语句转换为 Query 树，
 *	  供重写器和规划器做进一步处理。
 *
 *	  工具语句（即不可优化的语句）会设置 utilityStmt 字段，
 *	  Query 的其余部分大多是占位内容。
 *
 *	  规划阶段将 Query 树转换为以 PlannedStmt 节点为头部的计划树——
 *	  执行器不使用 Query 结构体。
 *
 *	  所有在查询指纹计算中被忽略的字段在语义上都不重要（如别名），
 *	  同样被忽略的还有可以从子节点推导出的信息（否则我们就会对
 *	  那部分信息进行双重哈希处理）。
 */
typedef struct Query
{
  NodeTag type;

  CmdType commandType; /* select|insert|update|delete|merge|utility */
                       /* select|insert|update|delete|merge|utility 类型 */

  /* where did I come from? */
  /* 我从哪里来？（查询来源） */
  QuerySource querySource pg_node_attr(query_jumble_ignore);

  /*
   * query identifier (can be set by plugins); ignored for equal, as it
   * might not be set; also not stored.  This is the result of the query
   * jumble, hence ignored.
   *
   * We store this as a signed value as this is the form it's displayed to
   * users in places such as EXPLAIN and pg_stat_statements.  Primarily this
   * is done due to lack of an SQL type to represent the full range of
   * uint64.
   *
   * 查询标识符（可由插件设置）；在相等性比较中被忽略，因为它可能未被设置；
   * 同时也不会被存储。这是查询指纹的结果，因此被忽略。
   *
   * 我们将其存储为有符号值，因为这是在 EXPLAIN 和 pg_stat_statements
   * 等地方向用户展示的形式。主要是由于缺少能表示 uint64 完整范围的 SQL 类型。
   */
  int64 queryId pg_node_attr(equal_ignore, query_jumble_ignore,
                             read_write_ignore, read_as(0));

  /* do I set the command result tag? */
  /* 我是否设置命令结果标签？ */
  bool canSetTag pg_node_attr(query_jumble_ignore);

  Node *utilityStmt; /* non-null if commandType == CMD_UTILITY */
                     /* 如果 commandType == CMD_UTILITY 则非空 */

  /*
   * rtable index of target relation for INSERT/UPDATE/DELETE/MERGE; 0 for
   * SELECT.  This is ignored in the query jumble as unrelated to the
   * compilation of the query ID.
   *
   * INSERT/UPDATE/DELETE/MERGE 目标关系的范围表索引；SELECT 时为 0。
   * 在查询指纹计算中被忽略，因为与查询 ID 的编译无关。
   */
  int resultRelation pg_node_attr(query_jumble_ignore);

  /* has aggregates in tlist or havingQual */
  /* 目标列表或 HAVING 子句中含有聚合函数 */
  bool hasAggs pg_node_attr(query_jumble_ignore);
  /* has window functions in tlist */
  /* 目标列表中含有窗口函数 */
  bool hasWindowFuncs pg_node_attr(query_jumble_ignore);
  /* has set-returning functions in tlist */
  /* 目标列表中含有集合返回函数 */
  bool hasTargetSRFs pg_node_attr(query_jumble_ignore);
  /* has subquery SubLink */
  /* 含有子查询 SubLink */
  bool hasSubLinks pg_node_attr(query_jumble_ignore);
  /* distinctClause is from DISTINCT ON */
  /* distinctClause 来自 DISTINCT ON */
  bool hasDistinctOn pg_node_attr(query_jumble_ignore);
  /* WITH RECURSIVE was specified */
  /* 指定了 WITH RECURSIVE */
  bool hasRecursive pg_node_attr(query_jumble_ignore);
  /* has INSERT/UPDATE/DELETE/MERGE in WITH */
  /* WITH 子句中含有 INSERT/UPDATE/DELETE/MERGE */
  bool hasModifyingCTE pg_node_attr(query_jumble_ignore);
  /* FOR [KEY] UPDATE/SHARE was specified */
  /* 指定了 FOR [KEY] UPDATE/SHARE */
  bool hasForUpdate pg_node_attr(query_jumble_ignore);
  /* rewriter has applied some RLS policy */
  /* 重写器已应用某些行级安全策略 */
  bool hasRowSecurity pg_node_attr(query_jumble_ignore);
  /* parser has added an RTE_GROUP RTE */
  /* 解析器已添加 RTE_GROUP 范围表项 */
  bool hasGroupRTE pg_node_attr(query_jumble_ignore);
  /* is a RETURN statement */
  /* 是 RETURN 语句 */
  bool isReturn pg_node_attr(query_jumble_ignore);

  List *cteList; /* WITH list (of CommonTableExpr's) */
                 /* WITH 列表（CommonTableExpr 列表） */
  List *rtable;  /* list of range table entries */
                 /* 范围表项列表 */

  /*
   * list of RTEPermissionInfo nodes for the rtable entries having
   * perminfoindex > 0
   *
   * perminfoindex > 0 的范围表项所对应的 RTEPermissionInfo 节点列表
   */
  List *rteperminfos pg_node_attr(query_jumble_ignore);
  FromExpr *jointree; /* table join tree (FROM and WHERE clauses);
                       * also USING clause for MERGE */
                      /* 表连接树（FROM 和 WHERE 子句）；
                       * MERGE 时也包含 USING 子句 */

  List *mergeActionList; /* list of actions for MERGE (only) */
                         /* MERGE 语句的动作列表（仅用于 MERGE） */

  /*
   * rtable index of target relation for MERGE to pull data. Initially, this
   * is the same as resultRelation, but after query rewriting, if the target
   * relation is a trigger-updatable view, this is the index of the expanded
   * view subquery, whereas resultRelation is the index of the target view.
   *
   * MERGE 拉取数据的目标关系的范围表索引。初始时与 resultRelation 相同，
   * 但在查询重写后，如果目标关系是可通过触发器更新的视图，
   * 这里是展开视图子查询的索引，而 resultRelation 是目标视图的索引。
   */
  int mergeTargetRelation pg_node_attr(query_jumble_ignore);

  /* join condition between source and target for MERGE */
  /* MERGE 语句中源表与目标表之间的连接条件 */
  Node *mergeJoinCondition;

  List *targetList; /* target list (of TargetEntry) */
                    /* 目标列表（TargetEntry 列表） */

  /* OVERRIDING clause */
  /* OVERRIDING 子句 */
  OverridingKind override pg_node_attr(query_jumble_ignore);

  OnConflictExpr *onConflict; /* ON CONFLICT DO [NOTHING | UPDATE] */
                              /* ON CONFLICT DO [NOTHING | UPDATE] */

  /*
   * The following three fields describe the contents of the RETURNING list
   * for INSERT/UPDATE/DELETE/MERGE. returningOldAlias and returningNewAlias
   * are the alias names for OLD and NEW, which may be user-supplied values,
   * the defaults "old" and "new", or NULL (if the default "old"/"new" is
   * already in use as the alias for some other relation).
   *
   * 以下三个字段描述 INSERT/UPDATE/DELETE/MERGE 的 RETURNING 列表内容。
   * returningOldAlias 和 returningNewAlias 是 OLD 和 NEW 的别名，
   * 可能是用户提供的值、默认的 "old" 和 "new"，
   * 或 NULL（如果默认的 "old"/"new" 已被其他关系用作别名）。
   */
  char *returningOldAlias pg_node_attr(query_jumble_ignore);
  char *returningNewAlias pg_node_attr(query_jumble_ignore);
  List *returningList; /* return-values list (of TargetEntry) */
                       /* 返回值列表（TargetEntry 列表） */

  List *groupClause;  /* a list of SortGroupClause's */
                      /* SortGroupClause 列表 */
  bool groupDistinct; /* is the group by clause distinct? */
                      /* group by 子句是否含有 distinct？ */

  List *groupingSets; /* a list of GroupingSet's if present */
                      /* 如果存在，为 GroupingSet 列表 */

  Node *havingQual; /* qualifications applied to groups */
                    /* 应用于分组的限定条件 */

  List *windowClause; /* a list of WindowClause's */
                      /* WindowClause 列表 */

  List *distinctClause; /* a list of SortGroupClause's */
                        /* SortGroupClause 列表 */

  List *sortClause; /* a list of SortGroupClause's */
                    /* SortGroupClause 列表 */

  Node *limitOffset;       /* # of result tuples to skip (int8 expr) */
                           /* 要跳过的结果元组数量（int8 表达式） */
  Node *limitCount;        /* # of result tuples to return (int8 expr) */
                           /* 要返回的结果元组数量（int8 表达式） */
  LimitOption limitOption; /* limit type */
                           /* limit 类型 */

  List *rowMarks; /* a list of RowMarkClause's */
                  /* RowMarkClause 列表 */

  Node *setOperations; /* set-operation tree if this is top level of
                        * a UNION/INTERSECT/EXCEPT query */
                       /* 集合操作树，如果这是 UNION/INTERSECT/EXCEPT
                        * 查询的顶层 */

  /*
   * A list of pg_constraint OIDs that the query depends on to be
   * semantically valid
   *
   * 查询在语义上有效所依赖的 pg_constraint OID 列表
   */
  List *constraintDeps pg_node_attr(query_jumble_ignore);

  /* a list of WithCheckOption's (added during rewrite) */
  /* WithCheckOption 列表（在重写期间添加） */
  List *withCheckOptions pg_node_attr(query_jumble_ignore);

  /*
   * The following two fields identify the portion of the source text string
   * containing this query.  They are typically only populated in top-level
   * Queries, not in sub-queries.  When not set, they might both be zero, or
   * both be -1 meaning "unknown".
   *
   * 以下两个字段标识包含此查询的源文本字符串的部分。
   * 它们通常只在顶层查询中填充，不在子查询中填充。
   * 未设置时，两者可能都为零，或都为 -1 表示"未知"。
   */
  /* start location, or -1 if unknown */
  /* 起始位置，-1 表示未知 */
  ParseLoc stmt_location;
  /* length in bytes; 0 means "rest of string" */
  /* 长度（字节数）；0 表示"字符串其余部分" */
  ParseLoc stmt_len pg_node_attr(query_jumble_ignore);
} Query;

/****************************************************************************
 *	Supporting data structures for Parse Trees
 *
 *	Most of these node types appear in raw parsetrees output by the grammar,
 *	and get transformed to something else by the analyzer.  A few of them
 *	are used as-is in transformed querytrees.
 *
 *	解析树的辅助数据结构。
 *	这些节点类型大多出现在语法解析器输出的原始解析树中，
 *	经分析器转换为其他内容。少数在转换后的查询树中原样使用。
 ****************************************************************************/

/*
 * TypeName - specifies a type in definitions
 *
 * For TypeName structures generated internally, it is often easier to
 * specify the type by OID than by name.  If "names" is NIL then the
 * actual type OID is given by typeOid, otherwise typeOid is unused.
 * Similarly, if "typmods" is NIL then the actual typmod is expected to
 * be prespecified in typemod, otherwise typemod is unused.
 *
 * If pct_type is true, then names is actually a field name and we look up
 * the type of that field.  Otherwise (the normal case), names is a type
 * name possibly qualified with schema and database name.
 *
 * TypeName - 在定义中指定类型
 *
 * 对于内部生成的 TypeName 结构，通常通过 OID 指定类型比通过名称更方便。
 * 如果 "names" 为 NIL，则实际类型 OID 由 typeOid 提供，否则 typeOid 不使用。
 * 类似地，如果 "typmods" 为 NIL，则实际的 typmod 预期在 typemod
 * 中预先指定，否则 typemod 不使用。
 *
 * 如果 pct_type 为 true，则 names 实际上是一个字段名，我们查找该字段的类型。
 * 否则（正常情况），names 是可能以模式和数据库名限定的类型名。
 */
typedef struct TypeName
{
  NodeTag type;
  List *names;       /* qualified name (list of String nodes) */
                     /* 限定名称（String 节点列表） */
  Oid typeOid;       /* type identified by OID */
                     /* 由 OID 标识的类型 */
  bool setof;        /* is a set? */
                     /* 是否为集合？ */
  bool pct_type;     /* %TYPE specified? */
                     /* 是否指定了 %TYPE？ */
  List *typmods;     /* type modifier expression(s) */
                     /* 类型修饰符表达式 */
  int32 typemod;     /* prespecified type modifier */
                     /* 预先指定的类型修饰符 */
  List *arrayBounds; /* array bounds */
                     /* 数组边界 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} TypeName;

/*
 * ColumnRef - specifies a reference to a column, or possibly a whole tuple
 *
 * The "fields" list must be nonempty.  It can contain String nodes
 * (representing names) and A_Star nodes (representing occurrence of a '*').
 * Currently, A_Star must appear only as the last list element --- the grammar
 * is responsible for enforcing this!
 *
 * Note: any container subscripting or selection of fields from composite
 * columns is represented by an A_Indirection node above the ColumnRef. However,
 * for simplicity in the normal case, initial field selection from a table
 * name is represented within ColumnRef and not by adding A_Indirection.
 *
 * ColumnRef - 指定对一列的引用，或可能是整个元组
 *
 * "fields" 列表必须非空。它可以包含 String 节点（表示名称）和
 * A_Star 节点（表示 '*' 的出现）。目前，A_Star 只能当列表最后一个元素——
 * 语法分析器负责强制执行此规则！
 *
 * 注意：对容器的下标访问或组合列的字段选择，由 ColumnRef 上方的
 * A_Indirection 节点表示。但为了简化正常情况，从表名进行的初始
 * 字段选择在 ColumnRef 内部表示，而不是通过添加 A_Indirection。
 */
typedef struct ColumnRef
{
  NodeTag type;
  List *fields;      /* field names (String nodes) or A_Star */
                     /* 字段名称（String 节点）或 A_Star */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} ColumnRef;

/*
 * ParamRef - specifies a $n parameter reference
 *
 * ParamRef - 指定 $n 参数引用
 */
typedef struct ParamRef
{
  NodeTag type;
  int number;        /* the number of the parameter */
                     /* 参数编号 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} ParamRef;

/*
 * A_Expr - infix, prefix, and postfix expressions
 *
 * A_Expr - 中缀、前缀和后缀表达式
 */
typedef enum A_Expr_Kind
{
  AEXPR_OP,              /* normal operator */
  AEXPR_OP_ANY,          /* scalar op ANY (array) */
  AEXPR_OP_ALL,          /* scalar op ALL (array) */
  AEXPR_DISTINCT,        /* IS DISTINCT FROM - name must be "=" */
  AEXPR_NOT_DISTINCT,    /* IS NOT DISTINCT FROM - name must be "=" */
  AEXPR_NULLIF,          /* NULLIF - name must be "=" */
  AEXPR_IN,              /* [NOT] IN - name must be "=" or "<>" */
  AEXPR_LIKE,            /* [NOT] LIKE - name must be "~~" or "!~~" */
  AEXPR_ILIKE,           /* [NOT] ILIKE - name must be "~~*" or "!~~*" */
  AEXPR_SIMILAR,         /* [NOT] SIMILAR - name must be "~" or "!~" */
  AEXPR_BETWEEN,         /* name must be "BETWEEN" */
  AEXPR_NOT_BETWEEN,     /* name must be "NOT BETWEEN" */
  AEXPR_BETWEEN_SYM,     /* name must be "BETWEEN SYMMETRIC" */
  AEXPR_NOT_BETWEEN_SYM, /* name must be "NOT BETWEEN SYMMETRIC" */
} A_Expr_Kind;

typedef struct A_Expr
{
  pg_node_attr(custom_read_write)

      NodeTag type;
  A_Expr_Kind kind; /* see above */
                    /* 见上文 */
  List *name;       /* possibly-qualified name of operator */
                    /* 可能带有限定的操作符名称 */
  Node *lexpr;      /* left argument, or NULL if none */
                    /* 左参数，若无则为 NULL */
  Node *rexpr;      /* right argument, or NULL if none */
                    /* 右参数，若无则为 NULL */

  /*
   * If rexpr is a list of some kind, we separately track its starting and
   * ending location; it's not the same as the starting and ending location
   * of the token itself.
   *
   * 如果 rexpr 是某种类型的列表，我们分别跟踪其起始和结束位置；
   * 这与标记（token）本身的起始和结束位置不同。
   */
  ParseLoc rexpr_list_start;
  ParseLoc rexpr_list_end;
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} A_Expr;

/*
 * A_Const - a literal constant
 *
 * Value nodes are inline for performance.  You can treat 'val' as a node,
 * as in IsA(&val, Integer).  'val' is not valid if isnull is true.
 *
 * A_Const - 字面常量
 *
 * Value 节点为了性能而内联。可以将 'val' 视为节点，
 * 如 IsA(&val, Integer) 中那样。当 isnull 为 true 时，'val' 无效。
 */
union ValUnion {
  Node node;
  Integer ival;
  Float fval;
  Boolean boolval;
  String sval;
  BitString bsval;
};

typedef struct A_Const
{
  pg_node_attr(custom_copy_equal, custom_read_write, custom_query_jumble)

      NodeTag type;
  union ValUnion val; /* constant value */
                      /* 常量值 */
  bool isnull;       /* SQL NULL constant */
                     /* SQL NULL 常量 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} A_Const;

/*
 * TypeCast - a CAST expression
 *
 * TypeCast - CAST 表达式
 */
typedef struct TypeCast
{
  NodeTag type;
  Node *arg;          /* the expression being casted */
                      /* 被转换的表达式 */
  TypeName *typeName; /* the target type */
                      /* 目标类型 */
  ParseLoc location;  /* token location, or -1 if unknown */
                      /* 标记位置，若未知则为 -1 */
} TypeCast;

/*
 * CollateClause - a COLLATE expression
 *
 * CollateClause - COLLATE 表达式
 */
typedef struct CollateClause
{
  NodeTag type;
  Node *arg;         /* input expression */
                     /* 输入表达式 */
  List *collname;    /* possibly-qualified collation name */
                     /* 可能带有限定的排序规则名称 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} CollateClause;

/*
 * RoleSpec - a role name or one of a few special values.
 *
 * RoleSpec - 角色名称或少数几个特殊值之一。
 */
typedef enum RoleSpecType
{
  ROLESPEC_CSTRING,      /* role name is stored as a C string */
  ROLESPEC_CURRENT_ROLE, /* role spec is CURRENT_ROLE */
  ROLESPEC_CURRENT_USER, /* role spec is CURRENT_USER */
  ROLESPEC_SESSION_USER, /* role spec is SESSION_USER */
  ROLESPEC_PUBLIC,       /* role name is "public" */
} RoleSpecType;

typedef struct RoleSpec
{
  NodeTag type;
  RoleSpecType roletype; /* Type of this rolespec */
                         /* 此 rolespec 的类型 */
  char *rolename;        /* filled only for ROLESPEC_CSTRING */
                         /* 仅在 ROLESPEC_CSTRING 时填充 */
  ParseLoc location;     /* token location, or -1 if unknown */
                         /* 标记位置，若未知则为 -1 */
} RoleSpec;

/*
 * FuncCall - a function or aggregate invocation
 *
 * agg_order (if not NIL) indicates we saw 'foo(... ORDER BY ...)', or if
 * agg_within_group is true, it was 'foo(...) WITHIN GROUP (ORDER BY ...)'.
 * agg_star indicates we saw a 'foo(*)' construct, while agg_distinct
 * indicates we saw 'foo(DISTINCT ...)'.  In any of these cases, the
 * construct *must* be an aggregate call.  Otherwise, it might be either an
 * aggregate or some other kind of function.  However, if FILTER or OVER is
 * present it had better be an aggregate or window function.
 *
 * Normally, you'd initialize this via makeFuncCall() and then only change the
 * parts of the struct its defaults don't match afterwards, as needed.
 *
 * FuncCall - 函数或聚合调用
 *
 * agg_order（如果非 NIL）表示我们看到了 'foo(... ORDER BY ...)'，
 * 或者如果 agg_within_group 为 true，则是 'foo(...) WITHIN GROUP (ORDER BY
 * ...)'。 agg_star 表示我们看到了 'foo(*)' 结构，而 agg_distinct 表示我们看到了
 * 'foo(DISTINCT ...)'。在任一这些情况下，该结构*必须*是聚合调用。
 * 否则，它可能是聚合函数或其他类型的函数。但如果存在 FILTER 或 OVER，
 * 则必须是聚合函数或窗口函数。
 *
 * 通常情况下，针对此结构体你应通过 makeFuncCall() 初始化，
 * 然后仅按需更改与默认值不匹配的部分。
 */
typedef struct FuncCall
{
  NodeTag type;
  List *funcname;          /* qualified name of function */
                           /* 函数的限定名称 */
  List *args;              /* the arguments (list of exprs) */
                           /* 参数（表达式列表） */
  List *agg_order;         /* ORDER BY (list of SortBy) */
                           /* ORDER BY（SortBy 列表） */
  Node *agg_filter;        /* FILTER clause, if any */
                           /* FILTER 子句，如果有 */
  struct WindowDef *over;  /* OVER clause, if any */
                           /* OVER 子句，如果有 */
  bool agg_within_group;   /* ORDER BY appeared in WITHIN GROUP */
                           /* ORDER BY 出现在 WITHIN GROUP 中 */
  bool agg_star;           /* argument was really '*' */
                           /* 参数确实是 '*' */
  bool agg_distinct;       /* arguments were labeled DISTINCT */
                           /* 参数被标记为 DISTINCT */
  bool func_variadic;      /* last argument was labeled VARIADIC */
                           /* 最后一个参数被标记为 VARIADIC */
  CoercionForm funcformat; /* how to display this node */
                           /* 如何显示此节点 */
  ParseLoc location;       /* token location, or -1 if unknown */
                           /* 标记位置，若未知则为 -1 */
} FuncCall;

/*
 * A_Star - '*' representing all columns of a table or compound field
 *
 * This can appear within ColumnRef.fields, A_Indirection.indirection, and
 * ResTarget.indirection lists.
 *
 * A_Star - '*' 表示表或复合字段的所有列
 *
 * 可以出现在 ColumnRef.fields、A_Indirection.indirection 和
 * ResTarget.indirection 列表中。
 */
typedef struct A_Star
{
  NodeTag type;
} A_Star;

/*
 * A_Indices - array subscript or slice bounds ([idx] or [lidx:uidx])
 *
 * In slice case, either or both of lidx and uidx can be NULL (omitted).
 * In non-slice case, uidx holds the single subscript and lidx is always NULL.
 *
 * A_Indices - 数组下标或切片边界（[idx] 或 [lidx:uidx]）
 *
 * 在切片情况下，lidx 和 uidx 之一或两者都可以为 NULL（省略）。
 * 在非切片情况下，uidx 存放单个下标，lidx 始终为 NULL。
 */
typedef struct A_Indices
{
  NodeTag type;
  bool is_slice; /* true if slice (i.e., colon present) */
                 /* 如果是切片（即存在冒号）则为 true */
  Node *lidx;    /* slice lower bound, if any */
                 /* 切片下界，如果有 */
  Node *uidx;    /* subscript, or slice upper bound if any */
                 /* 下标，或者切片上界，如果有 */
} A_Indices;

/*
 * A_Indirection - select a field and/or array element from an expression
 *
 * The indirection list can contain A_Indices nodes (representing
 * subscripting), String nodes (representing field selection --- the
 * string value is the name of the field to select), and A_Star nodes
 * (representing selection of all fields of a composite type).
 * For example, a complex selection operation like
 *				(foo).field1[42][7].field2
 * would be represented with a single A_Indirection node having a 4-element
 * indirection list.
 *
 * Currently, A_Star must appear only as the last list element --- the grammar
 * is responsible for enforcing this!
 *
 * A_Indirection - 从表达式中选择字段和/或数组元素
 *
 * 间接列表可以包含 A_Indices 节点（表示下标访问）、
 * String 节点（表示字段选择——字符串值是要选择的字段名）
 * 和 A_Star 节点（表示选择复合类型的所有字段）。
 * 例如，这样的复杂选择操作：
 *				(foo).field1[42][7].field2
 * 将用一个具有 4 个元素的间接列表的单个 A_Indirection 节点表示。
 *
 * 目前，A_Star 只能当列表最后一个元素——语法分析器负责强制执行此规则！
 */
typedef struct A_Indirection
{
  NodeTag type;
  Node *arg;         /* the thing being selected from */
                     /* 被选择的对象 */
  List *indirection; /* subscripts and/or field names and/or * */
                     /* 下标和/或字段名和/或 * */
} A_Indirection;

/*
 * A_ArrayExpr - an ARRAY[] construct
 *
 * A_ArrayExpr - ARRAY[] 结构
 */
typedef struct A_ArrayExpr
{
  NodeTag type;
  List *elements;      /* array element expressions */
                       /* 数组元素表达式 */
  ParseLoc list_start; /* start of the element list */
                       /* 元素列表起始位置 */
  ParseLoc list_end;   /* end of the elements list */
                       /* 元素列表结束位置 */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} A_ArrayExpr;

/*
 * ResTarget -
 *	  result target (used in target list of pre-transformed parse trees)
 *
 * In a SELECT target list, 'name' is the column label from an
 * 'AS ColumnLabel' clause, or NULL if there was none, and 'val' is the
 * value expression itself.  The 'indirection' field is not used.
 *
 * INSERT uses ResTarget in its target-column-names list.  Here, 'name' is
 * the name of the destination column, 'indirection' stores any subscripts
 * attached to the destination, and 'val' is not used.
 *
 * In an UPDATE target list, 'name' is the name of the destination column,
 * 'indirection' stores any subscripts attached to the destination, and
 * 'val' is the expression to assign.
 *
 * See A_Indirection for more info about what can appear in 'indirection'.
 *
 * ResTarget -
 *	  结果目标（用于转换前解析树的目标列表中）
 *
 * 在 SELECT 目标列表中，'name' 是来自 'AS ColumnLabel' 子句的列标签，
 * 或若没有则为 NULL，'val' 是値表达式本身。不使用 'indirection' 字段。
 *
 * INSERT 在其目标列名列表中使用 ResTarget。此处，'name' 是目标列的名称，
 * 'indirection' 存储附加到目标的任何下标，'val' 不使用。
 *
 * 在 UPDATE 目标列表中，'name' 是目标列的名称，
 * 'indirection' 存储附加到目标的任何下标，'val' 是要赋值的表达式。
 *
 * 有关 'indirection' 中可以出现的内容，请参阅 A_Indirection。
 */
typedef struct ResTarget
{
  NodeTag type;
  char *name;        /* column name or NULL */
                     /* 列名或 NULL */
  List *indirection; /* subscripts, field names, and '*', or NIL */
                     /* 下标、字段名、'*' 或 NIL */
  Node *val;         /* the value expression to compute or assign */
                     /* 要计算或赋值的値表达式 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} ResTarget;

/*
 * MultiAssignRef - element of a row source expression for UPDATE
 *
 * In an UPDATE target list, when we have SET (a,b,c) = row-valued-expression,
 * we generate separate ResTarget items for each of a,b,c.  Their "val" trees
 * are MultiAssignRef nodes numbered 1..n, linking to a common copy of the
 * row-valued-expression (which parse analysis will process only once, when
 * handling the MultiAssignRef with colno=1).
 *
 * MultiAssignRef - UPDATE 的行源表达式元素
 *
 * 在 UPDATE 目标列表中，当我们有 SET (a,b,c) = 行値表达式时，
 * 我们为 a、b、c 分别生成单独的 ResTarget 项。它们的 "val" 树
 * 是编号为 1..n 的 MultiAssignRef 节点，链接到行値表达式的公共副本
 * （解析分析将只处理一次，即处理 colno=1 的 MultiAssignRef 时）。
 */
typedef struct MultiAssignRef
{
  NodeTag type;
  Node *source; /* the row-valued expression */
                /* 行値表达式 */
  int colno;    /* column number for this target (1..n) */
                /* 此目标的列号 (1..n) */
  int ncolumns; /* number of targets in the construct */
                /* 结构中的目标数量 */
} MultiAssignRef;

/*
 * SortBy - for ORDER BY clause
 *
 * SortBy - 用于 ORDER BY 子句
 */
typedef struct SortBy
{
  NodeTag type;
  Node *node;               /* expression to sort on */
                            /* 用于排序的表达式 */
  SortByDir sortby_dir;     /* ASC/DESC/USING/default */
                            /* ASC/DESC/USING/默认 */
  SortByNulls sortby_nulls; /* NULLS FIRST/LAST */
                            /* NULLS FIRST/LAST */
  List *useOp;              /* name of op to use, if SORTBY_USING */
                            /* 如果指定了 SORTBY_USING，则是要使用的操作符名称 */
  ParseLoc location;        /* operator location, or -1 if none/unknown */
                            /* 操作符位置，若无或未知则为 -1 */
} SortBy;

/*
 * WindowDef - raw representation of WINDOW and OVER clauses
 *
 * For entries in a WINDOW list, "name" is the window name being defined.
 * For OVER clauses, we use "name" for the "OVER window" syntax, or "refname"
 * for the "OVER (window)" syntax, which is subtly different --- the latter
 * implies overriding the window frame clause.
 *
 * WindowDef - WINDOW 和 OVER 子句的原始表示
 *
 * 对于 WINDOW 列表中的项，"name" 是正在定义的窗口名。
 * 对于 OVER 子句，我们将 "name" 用于 "OVER window" 语法，
 * 或将 "refname" 用于 "OVER (window)" 语法，两者有微妙的区别——
 * 后者意味着覆盖窗口帧子句。
 */
typedef struct WindowDef
{
  NodeTag type;
  char *name;            /* window's own name */
                         /* 窗口自身的名称 */
  char *refname;         /* referenced window name, if any */
                         /* 被引用的窗口名称，如果有 */
  List *partitionClause; /* PARTITION BY expression list */
                         /* PARTITION BY 表达式列表 */
  List *orderClause;     /* ORDER BY (list of SortBy) */
                         /* ORDER BY（SortBy 列表） */
  int frameOptions;      /* frame_clause options, see below */
                         /* 框架子句（frame_clause）选项，见下文 */
  Node *startOffset;     /* expression for starting bound, if any */
                         /* 起始边界表达式，如果有 */
  Node *endOffset;       /* expression for ending bound, if any */
                         /* 结束边界表达式，如果有 */
  ParseLoc location;     /* parse location, or -1 if none/unknown */
                         /* 解析位置，若无或未知则为 -1 */
} WindowDef;

/*
 * frameOptions is an OR of these bits.  The NONDEFAULT and BETWEEN bits are
 * used so that ruleutils.c can tell which properties were specified and
 * which were defaulted; the correct behavioral bits must be set either way.
 * The START_foo and END_foo options must come in pairs of adjacent bits for
 * the convenience of gram.y, even though some of them are useless/invalid.
 *
 * frameOptions 是这些位的或运算结果。NONDEFAULT 和 BETWEEN 位
 * 用于让 ruleutils.c 能分辨哪些属性是明确指定的，哪些是默认的；
 * 无论哪种情况，正确的行为位必须被设置。
 * START_foo 和 END_foo 选项必须成对的相邻位出现，以方便 gram.y，
 * 尽管其中一些无用／无效。
 */
#define FRAMEOPTION_NONDEFAULT 0x00001                /* any specified? */
#define FRAMEOPTION_RANGE 0x00002                     /* RANGE behavior */
#define FRAMEOPTION_ROWS 0x00004                      /* ROWS behavior */
#define FRAMEOPTION_GROUPS 0x00008                    /* GROUPS behavior */
#define FRAMEOPTION_BETWEEN 0x00010                   /* BETWEEN given? */
#define FRAMEOPTION_START_UNBOUNDED_PRECEDING 0x00020 /* start is U. P. */
#define FRAMEOPTION_END_UNBOUNDED_PRECEDING 0x00040   /* (disallowed) */
#define FRAMEOPTION_START_UNBOUNDED_FOLLOWING 0x00080 /* (disallowed) */
#define FRAMEOPTION_END_UNBOUNDED_FOLLOWING 0x00100   /* end is U. F. */
#define FRAMEOPTION_START_CURRENT_ROW 0x00200         /* start is C. R. */
#define FRAMEOPTION_END_CURRENT_ROW 0x00400           /* end is C. R. */
#define FRAMEOPTION_START_OFFSET_PRECEDING 0x00800    /* start is O. P. */
#define FRAMEOPTION_END_OFFSET_PRECEDING 0x01000      /* end is O. P. */
#define FRAMEOPTION_START_OFFSET_FOLLOWING 0x02000    /* start is O. F. */
#define FRAMEOPTION_END_OFFSET_FOLLOWING 0x04000      /* end is O. F. */
#define FRAMEOPTION_EXCLUDE_CURRENT_ROW 0x08000       /* omit C.R. */
#define FRAMEOPTION_EXCLUDE_GROUP 0x10000             /* omit C.R. & peers */
#define FRAMEOPTION_EXCLUDE_TIES 0x20000              /* omit C.R.'s peers */

#define FRAMEOPTION_START_OFFSET                                               \
  (FRAMEOPTION_START_OFFSET_PRECEDING | FRAMEOPTION_START_OFFSET_FOLLOWING)
#define FRAMEOPTION_END_OFFSET                                                 \
  (FRAMEOPTION_END_OFFSET_PRECEDING | FRAMEOPTION_END_OFFSET_FOLLOWING)
#define FRAMEOPTION_EXCLUSION                                                  \
  (FRAMEOPTION_EXCLUDE_CURRENT_ROW | FRAMEOPTION_EXCLUDE_GROUP |               \
   FRAMEOPTION_EXCLUDE_TIES)

#define FRAMEOPTION_DEFAULTS                                                   \
  (FRAMEOPTION_RANGE | FRAMEOPTION_START_UNBOUNDED_PRECEDING |                 \
   FRAMEOPTION_END_CURRENT_ROW)

/*
 * RangeSubselect - subquery appearing in a FROM clause
 *
 * RangeSubselect - 出现在 FROM 子句中的子查询
 */
typedef struct RangeSubselect
{
  NodeTag type;
  bool lateral;   /* does it have LATERAL prefix? */
                  /* 是否有 LATERAL 前缀？ */
  Node *subquery; /* the untransformed sub-select clause */
                  /* 未转换的子查询 SELECT 子句 */
  Alias *alias;   /* table alias & optional column aliases */
                  /* 表别名及可选的列别名 */
} RangeSubselect;

/*
 * RangeFunction - function call appearing in a FROM clause
 *
 * functions is a List because we use this to represent the construct
 * ROWS FROM(func1(...), func2(...), ...).  Each element of this list is a
 * two-element sublist, the first element being the untransformed function
 * call tree, and the second element being a possibly-empty list of ColumnDef
 * nodes representing any columndef list attached to that function within the
 * ROWS FROM() syntax.
 *
 * alias and coldeflist represent any alias and/or columndef list attached
 * at the top level.  (We disallow coldeflist appearing both here and
 * per-function, but that's checked in parse analysis, not by the grammar.)
 *
 * RangeFunction - 出现在 FROM 子句中的函数调用
 *
 * functions 是一个 List，因为我们用它来表示 ROWS FROM(func1(...), func2(...),
 * ...) 结构。
 * 列表中每个元素是一个两元素子列表，第一个元素是未转换的函数调用树，
 * 第二个元素是 ROWS FROM() 语法中附加到该函数的列定义列表（可能为空）。
 *
 * alias 和 coldeflist 表示附加在顶层的任何别名和/或列定义列表。
 * （我们不允许 coldeflist
 * 同时在此处和每个函数处出现，但这是在解析分析中检查的，而不是语法解析器。）
 */
typedef struct RangeFunction
{
  NodeTag type;
  bool lateral;     /* does it have LATERAL prefix? */
                    /* 是否有 LATERAL 前缀？ */
  bool ordinality;  /* does it have WITH ORDINALITY suffix? */
                    /* 是否有 WITH ORDINALITY 后缀？ */
  bool is_rowsfrom; /* is result of ROWS FROM() syntax? */
                    /* 是否为 ROWS FROM() 语法的结果？ */
  List *functions;  /* per-function information, see above */
                    /* 每个函数的信息，见上文 */
  Alias *alias;     /* table alias & optional column aliases */
                    /* 表别名及可选的列别名 */
  List *coldeflist; /* list of ColumnDef nodes to describe result
                     * of function returning RECORD */
                    /* ColumnDef 节点列表，用于描述返回 RECORD 的函数结果 */
} RangeFunction;

/*
 * RangeTableFunc - raw form of "table functions" such as XMLTABLE
 *
 * Note: JSON_TABLE is also a "table function", but it uses JsonTable node,
 * not RangeTableFunc.
 *
 * RangeTableFunc - 表函数（如 XMLTABLE）的原始形式
 *
 * 注意：JSON_TABLE 也是“表函数”，但它使用 JsonTable 节点，而不是
 * RangeTableFunc。
 */
typedef struct RangeTableFunc
{
  NodeTag type;
  bool lateral;      /* does it have LATERAL prefix? */
                     /* 是否有 LATERAL 前缀？ */
  Node *docexpr;     /* document expression */
                     /* 文档表达式 */
  Node *rowexpr;     /* row generator expression */
                     /* 行生成器表达式 */
  List *namespaces;  /* list of namespaces as ResTarget */
                     /* ResTarget 形式的命名空间列表 */
  List *columns;     /* list of RangeTableFuncCol */
                     /* RangeTableFuncCol 列表 */
  Alias *alias;      /* table alias & optional column aliases */
                     /* 表别名及可选的列别名 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} RangeTableFunc;

/*
 * RangeTableFuncCol - one column in a RangeTableFunc->columns
 *
 * If for_ordinality is true (FOR ORDINALITY), then the column is an int4
 * column and the rest of the fields are ignored.
 *
 * RangeTableFuncCol - RangeTableFunc->columns 中的一列
 *
 * 如果 for_ordinality 为 true（FOR ORDINALITY），则该列是 int4
 * 列，其余字段被忽略。
 */
typedef struct RangeTableFuncCol
{
  NodeTag type;
  char *colname;       /* name of generated column */
                       /* 生成列的名称 */
  TypeName *typeName;  /* type of generated column */
                       /* 生成列的类型 */
  bool for_ordinality; /* does it have FOR ORDINALITY? */
                       /* 是否带有 FOR ORDINALITY？ */
  bool is_not_null;    /* does it have NOT NULL? */
                       /* 是否带有 NOT NULL？ */
  Node *colexpr;       /* column filter expression */
                       /* 列过滤表达式 */
  Node *coldefexpr;    /* column default value expression */
                       /* 列默认值表达式 */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} RangeTableFuncCol;

/*
 * RangeTableSample - TABLESAMPLE appearing in a raw FROM clause
 *
 * This node, appearing only in raw parse trees, represents
 *		<relation> TABLESAMPLE <method> (<params>) REPEATABLE (<num>)
 * Currently, the <relation> can only be a RangeVar, but we might in future
 * allow RangeSubselect and other options.  Note that the RangeTableSample
 * is wrapped around the node representing the <relation>, rather than being
 * a subfield of it.
 *
 * RangeTableSample - 出现在原始 FROM 子句中的 TABLESAMPLE
 *
 * 该节点仅出现在原始解析树中，表示：
 *		<relation> TABLESAMPLE <method> (<params>) REPEATABLE (<num>)
 * 目前，<relation> 只能是 RangeVar，但未来可能允许 RangeSubselect 和其他选项。
 * 注意：RangeTableSample 被包裹在表示 <relation> 的节点外面，而不是其子字段。
 */
typedef struct RangeTableSample
{
  NodeTag type;
  Node *relation;    /* relation to be sampled */
                     /* 要采样的关系 */
  List *method;      /* sampling method name (possibly qualified) */
                     /* 采样方法名称（可能被限定） */
  List *args;        /* argument(s) for sampling method */
                     /* 采样方法的参数 */
  Node *repeatable;  /* REPEATABLE expression, or NULL if none */
                     /* REPEATABLE 表达式，或为 NULL */
  ParseLoc location; /* method name location, or -1 if unknown */
                     /* 方法名称位置，若未知则为 -1 */
} RangeTableSample;

/*
 * ColumnDef - column definition (used in various creates)
 *
 * If the column has a default value, we may have the value expression
 * in either "raw" form (an untransformed parse tree) or "cooked" form
 * (a post-parse-analysis, executable expression tree), depending on
 * how this ColumnDef node was created (by parsing, or by inheritance
 * from an existing relation).  We should never have both in the same node!
 *
 * Similarly, we may have a COLLATE specification in either raw form
 * (represented as a CollateClause with arg==NULL) or cooked form
 * (the collation's OID).
 *
 * The constraints list may contain a CONSTR_DEFAULT item in a raw
 * parsetree produced by gram.y, but transformCreateStmt will remove
 * the item and set raw_default instead.  CONSTR_DEFAULT items
 * should not appear in any subsequent processing.
 *
 * ColumnDef - 列定义（用于各种 CREATE 语句）
 *
 * 如果列有默认値，我们可能以“原始”形式（未转换的解析树）或“已处理”形式
 * （解析后可执行的表达式树）存储値表达式，具体取决于 ColumnDef 节点的创建方式
 * （由解析器创建，或由现有关系继承而来）。同一节点中不应展时存在两种形式！
 *
 * 类似地，我们可能以原始形式（使用 arg==NULL 的 CollateClause 表示）
 * 或已处理形式（排序规则的 OID）存储 COLLATE 说明。
 *
 * 约束列表在 gram.y 生成的原始解析树中可能包含 CONSTR_DEFAULT 项，
 * 但 transformCreateStmt 将会删除该项并改为设置 raw_default。
 * 后续处理中不应出现 CONSTR_DEFAULT 项。
 */
typedef struct ColumnDef
{
  NodeTag type;
  char *colname;              /* name of column */
                              /* 列名 */
  TypeName *typeName;         /* type of column */
                              /* 列类型 */
  char *compression;          /* compression method for column */
                              /* 列的压缩方法 */
  int16 inhcount;             /* number of times column is inherited */
                              /* 列被继承的次数 */
  bool is_local;              /* column has local (non-inherited) def'n */
                              /* 列是否有本地（非继承）定义 */
  bool is_not_null;           /* NOT NULL constraint specified? */
                              /* 是否指定了 NOT NULL 约束？ */
  bool is_from_type;          /* column definition came from table type */
                              /* 列定义是否来自表类型 */
  char storage;               /* attstorage setting, or 0 for default */
                              /* attstorage 设置，0 为默认 */
  char *storage_name;         /* attstorage setting name or NULL for default */
                              /* attstorage 设置名称，NULL 为默认 */
  Node *raw_default;          /* default value (untransformed parse tree) */
                              /* 默认值（未转换的解析树） */
  Node *cooked_default;       /* default value (transformed expr tree) */
                              /* 默认值（已转换的表达式树） */
  char identity;              /* attidentity setting */
                              /* attidentity 设置 */
  RangeVar *identitySequence; /* to store identity sequence name for
                               * ALTER TABLE ... ADD COLUMN */
                              /* 用于存储 ALTER TABLE ... ADD COLUMN 的
                               * 标识序列名称 */
  char generated;             /* attgenerated setting */
                              /* attgenerated 设置 */
  CollateClause *collClause;  /* untransformed COLLATE spec, if any */
                              /* 未转换的 COLLATE 规范，如果有 */
  Oid collOid;                /* collation Oid (InvalidOid if not set) */
                              /* 排序规则 OID（如果未设置则为 InvalidOid） */
  List *constraints;          /* other constraints on column */
                              /* 列的其他约束 */
  List *fdwoptions;           /* per-column FDW options */
                              /* 每列的 FDW 选项 */
  ParseLoc location;          /* parse location, or -1 if none/unknown */
                              /* 解析位置，若无或未知则为 -1 */
} ColumnDef;

/*
 * TableLikeClause - CREATE TABLE ( ... LIKE ... ) clause
 *
 * TableLikeClause - CREATE TABLE ( ... LIKE ... ) 子句
 */
typedef struct TableLikeClause
{
  NodeTag type;
  RangeVar *relation; /* relation to be copied */
                      /* 要拷贝的关系 */
  bits32 options;  /* OR of TableLikeOption flags */
                   /* TableLikeOption 标志的位或组合 */
  Oid relationOid; /* If table has been looked up, its OID */
                   /* 如果表已查找到，其 OID */
} TableLikeClause;

typedef enum TableLikeOption
{
  CREATE_TABLE_LIKE_COMMENTS = 1 << 0,
  CREATE_TABLE_LIKE_COMPRESSION = 1 << 1,
  CREATE_TABLE_LIKE_CONSTRAINTS = 1 << 2,
  CREATE_TABLE_LIKE_DEFAULTS = 1 << 3,
  CREATE_TABLE_LIKE_GENERATED = 1 << 4,
  CREATE_TABLE_LIKE_IDENTITY = 1 << 5,
  CREATE_TABLE_LIKE_INDEXES = 1 << 6,
  CREATE_TABLE_LIKE_STATISTICS = 1 << 7,
  CREATE_TABLE_LIKE_STORAGE = 1 << 8,
  CREATE_TABLE_LIKE_ALL = PG_INT32_MAX
} TableLikeOption;

/*
 * IndexElem - index parameters (used in CREATE INDEX, and in ON CONFLICT)
 *
 * For a plain index attribute, 'name' is the name of the table column to
 * index, and 'expr' is NULL.  For an index expression, 'name' is NULL and
 * 'expr' is the expression tree.
 *
 * IndexElem - 索引参数（用于 CREATE INDEX 和 ON CONFLICT）
 *
 * 对于普通索引属性，'name' 是要建索引的表列名，'expr' 为 NULL。
 * 对于表达式索引，'name' 为 NULL，'expr' 是表达式树。
 */
typedef struct IndexElem
{
  NodeTag type;
  char *name;                 /* name of attribute to index, or NULL */
                              /* 建立索引的属性名，或为 NULL */
  Node *expr;                 /* expression to index, or NULL */
                              /* 建立索引的表达式，或为 NULL */
  char *indexcolname;         /* name for index column; NULL = default */
                              /* 索引列的名称；NULL 表示默认 */
  List *collation;            /* name of collation; NIL = default */
                              /* 排序规则名称；NIL 表示默认 */
  List *opclass;              /* name of desired opclass; NIL = default */
                              /* 所需的操作符类（opclass）名称；NIL 表示默认 */
  List *opclassopts;          /* opclass-specific options, or NIL */
                              /* 操作符类特定的选项，或为 NIL */
  SortByDir ordering;         /* ASC/DESC/default */
                              /* ASC/DESC/默认 */
  SortByNulls nulls_ordering; /* FIRST/LAST/default */
                              /* FIRST/LAST/默认 */
} IndexElem;

/*
 * DefElem - a generic "name = value" option definition
 *
 * In some contexts the name can be qualified.  Also, certain SQL commands
 * allow a SET/ADD/DROP action to be attached to option settings, so it's
 * convenient to carry a field for that too.  (Note: currently, it is our
 * practice that the grammar allows namespace and action only in statements
 * where they are relevant; C code can just ignore those fields in other
 * statements.)
 *
 * DefElem - 通用的 "name = value" 选项定义
 *
 * 在某些上下文中，名称可以限定。此外，某些 SQL 命令允许将 SET/ADD/DROP
 * 动作附加到选项设置，因此也方便携带一个对应字段。
 * （注意：目前，我们的做法是语法分析器只在相关语句中允许命名空间和动作；
 * C 代码在其他语句中可以忽略这些字段。）
 */
typedef enum DefElemAction
{
  DEFELEM_UNSPEC, /* no action given */
  DEFELEM_SET,
  DEFELEM_ADD,
  DEFELEM_DROP,
} DefElemAction;

typedef struct DefElem
{
  NodeTag type;
  char *defnamespace; /* NULL if unqualified name */
                      /* 如果是非限定名，则为 NULL */
  char *defname;
  Node *arg;               /* typically Integer, Float, String, or
                            * TypeName */
                           /* 通常为 Integer, Float, String, 或 TypeName */
  DefElemAction defaction; /* unspecified action, or SET/ADD/DROP */
                           /* 未指定的动作，或 SET/ADD/DROP */
  ParseLoc location;       /* token location, or -1 if unknown */
                           /* 标记位置，若未知则为 -1 */
} DefElem;

/*
 * LockingClause - raw representation of FOR [NO KEY] UPDATE/[KEY] SHARE
 *		options
 *
 * Note: lockedRels == NIL means "all relations in query".  Otherwise it
 * is a list of RangeVar nodes.  (We use RangeVar mainly because it carries
 * a location field --- currently, parse analysis insists on unqualified
 * names in LockingClause.)
 *
 * LockingClause - FOR [NO KEY] UPDATE/[KEY] SHARE 选项的原始表示
 *
 * 注意：lockedRels == NIL 表示“查询中的所有关系”。否则它是一个
 * RangeVar 节点列表。（我们使用 RangeVar 主要是因为它携带位置字段——
 * 目前解析分析强制 LockingClause 中使用非限定名。）
 */
typedef struct LockingClause
{
  NodeTag type;
  List *lockedRels; /* FOR [KEY] UPDATE/SHARE relations */
                    /* FOR [KEY] UPDATE/SHARE 的关系列表 */
  LockClauseStrength strength;
  LockWaitPolicy waitPolicy; /* NOWAIT and SKIP LOCKED */
                             /* NOWAIT 和 SKIP LOCKED */
} LockingClause;

/*
 * XMLSERIALIZE (in raw parse tree only)
 *
 * XMLSERIALIZE（仅在原始解析树中）
 */
typedef struct XmlSerialize
{
  NodeTag type;
  XmlOptionType xmloption; /* DOCUMENT or CONTENT */
                           /* DOCUMENT 或 CONTENT */
  Node *expr;
  TypeName *typeName;
  bool indent;       /* [NO] INDENT */
                     /* 是否带有 INDENT */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} XmlSerialize;

/* Partitioning related definitions */
/* 分区相关定义 */

/*
 * PartitionElem - parse-time representation of a single partition key
 *
 * expr can be either a raw expression tree or a parse-analyzed expression.
 * We don't store these on-disk, though.
 *
 * PartitionElem - 单个分区键的解析时表示
 *
 * expr 可以是原始表达式树或已解析分析的表达式。不过，我们不将它们存储在磁盘上。
 */
typedef struct PartitionElem
{
  NodeTag type;
  char *name;        /* name of column to partition on, or NULL */
                     /* 用于分区的列名，或为 NULL */
  Node *expr;        /* expression to partition on, or NULL */
                     /* 用于分区的表达式，或为 NULL */
  List *collation;   /* name of collation; NIL = default */
                     /* 排序规则名称；NIL 表示默认 */
  List *opclass;     /* name of desired opclass; NIL = default */
                     /* 所需的操作符类名称；NIL 表示默认 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} PartitionElem;

typedef enum PartitionStrategy
{
  PARTITION_STRATEGY_LIST = 'l',
  PARTITION_STRATEGY_RANGE = 'r',
  PARTITION_STRATEGY_HASH = 'h',
} PartitionStrategy;

/*
 * PartitionSpec - parse-time representation of a partition key specification
 *
 * This represents the key space we will be partitioning on.
 *
 * PartitionSpec - 分区键规范的解析时表示
 *
 * 表示我们将进行分区的键空间。
 */
typedef struct PartitionSpec
{
  NodeTag type;
  PartitionStrategy strategy; /* partitioning strategy */
                              /* 分区策略 */
  List *partParams;           /* List of PartitionElems */
                              /* PartitionElem 列表 */
  ParseLoc location;          /* token location, or -1 if unknown */
                              /* 标记位置，若未知则为 -1 */
} PartitionSpec;

/*
 * PartitionBoundSpec - a partition bound specification
 *
 * This represents the portion of the partition key space assigned to a
 * particular partition.  These are stored on disk in pg_class.relpartbound.
 *
 * PartitionBoundSpec - 分区边界规范
 *
 * 表示分配给特定分区的分区键空间部分。存储在磁盘上的 pg_class.relpartbound
 * 字段中。
 */
struct PartitionBoundSpec
{
  NodeTag type;

  char strategy;   /* see PARTITION_STRATEGY codes above */
                   /* 见上文 PARTITION_STRATEGY 编码 */
  bool is_default; /* is it a default partition bound? */
                   /* 这是否为默认分区边界？ */

  /* Partitioning info for HASH strategy: */
  /* HASH 策略的分区信息： */
  int modulus;     /* modulus */
                   /* 模数 */
  int remainder;   /* remainder */
                   /* 余数 */

  /* Partitioning info for LIST strategy: */
  /* LIST 策略的分区信息： */
  List *listdatums; /* List of Consts (or A_Consts in raw tree) */
                    /* Const 列表（或原始树中的 A_Const） */

  /* Partitioning info for RANGE strategy: */
  /* RANGE 策略的分区信息： */
  List *lowerdatums; /* List of PartitionRangeDatums */
                     /* PartitionRangeDatums 列表 */
  List *upperdatums; /* List of PartitionRangeDatums */
                     /* PartitionRangeDatums 列表 */

  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
};

/*
 * PartitionRangeDatum - one of the values in a range partition bound
 *
 * This can be MINVALUE, MAXVALUE or a specific bounded value.
 *
 * PartitionRangeDatum - 范围分区边界中的一个値
 *
 * 可以是 MINVALUE、MAXVALUE 或特定的有界値。
 */
typedef enum PartitionRangeDatumKind
{
  PARTITION_RANGE_DATUM_MINVALUE = -1, /* less than any other value */
  PARTITION_RANGE_DATUM_VALUE = 0,     /* a specific (bounded) value */
  PARTITION_RANGE_DATUM_MAXVALUE = 1,  /* greater than any other value */
} PartitionRangeDatumKind;

typedef struct PartitionRangeDatum
{
  NodeTag type;

  PartitionRangeDatumKind kind; /* MINVALUE, MAXVALUE or VALUE */
                                /* MINVALUE, MAXVALUE 或 VALUE */
  Node *value; /* Const (or A_Const in raw tree), if kind is
                * PARTITION_RANGE_DATUM_VALUE, else NULL */
               /* Const（或原始树中的 A_Const），如果 kind 为
                * PARTITION_RANGE_DATUM_VALUE，否则为 NULL */

  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} PartitionRangeDatum;

/*
 * PartitionCmd - info for ALTER TABLE/INDEX ATTACH/DETACH PARTITION commands
 *
 * PartitionCmd - ALTER TABLE/INDEX ATTACH/DETACH PARTITION 命令的信息
 */
typedef struct PartitionCmd
{
  NodeTag type;
  RangeVar *name;            /* name of partition to attach/detach */
                             /* 要挂载/卸载的分区名称 */
  PartitionBoundSpec *bound; /* FOR VALUES, if attaching */
                             /* FOR VALUES，如果是挂载操作 */
  bool concurrent;           /* whether to perform concurrently */
                             /* 是否并发执行 */
} PartitionCmd;

/****************************************************************************
 *	Nodes for a Query tree
 ****************************************************************************/

/*--------------------
 * RangeTblEntry -
 *	  A range table is a List of RangeTblEntry nodes.
 *
 *	  A range table entry may represent a plain relation, a sub-select in
 *	  FROM, or the result of a JOIN clause.  (Only explicit JOIN syntax
 *	  produces an RTE, not the implicit join resulting from multiple FROM
 *	  items.  This is because we only need the RTE to deal with SQL features
 *	  like outer joins and join-output-column aliasing.)  Other special
 *	  RTE types also exist, as indicated by RTEKind.
 *
 *	  Note that we consider RTE_RELATION to cover anything that has a
 *pg_class entry.  relkind distinguishes the sub-cases.
 *
 *	  alias is an Alias node representing the AS alias-clause attached to
 *the FROM expression, or NULL if no clause.
 *
 *	  eref is the table reference name and column reference names (either
 *	  real or aliases).  Note that system columns (OID etc) are not included
 *	  in the column list.
 *	  eref->aliasname is required to be present, and should generally be
 *used to identify the RTE for error messages etc.
 *
 *	  In RELATION RTEs, the colnames in both alias and eref are indexed by
 *	  physical attribute number; this means there must be colname entries
 *for dropped columns.  When building an RTE we insert empty strings ("") for
 *	  dropped columns.  Note however that a stored rule may have nonempty
 *	  colnames for columns dropped since the rule was created (and for that
 *	  matter the colnames might be out of date due to column renamings).
 *	  The same comments apply to FUNCTION RTEs when a function's return type
 *	  is a named composite type.
 *
 *	  In JOIN RTEs, the colnames in both alias and eref are one-to-one with
 *	  joinaliasvars entries.  A JOIN RTE will omit columns of its inputs
 *when those columns are known to be dropped at parse time.  Again, however, a
 *stored rule might contain entries for columns dropped since the rule was
 *created.  (This is only possible for columns not actually referenced in the
 *rule.)  When loading a stored rule, we replace the joinaliasvars items for any
 *such columns with null pointers.  (We can't simply delete them from the
 *joinaliasvars list, because that would affect the attnums of Vars referencing
 *the rest of the list.)
 *
 *	  inFromCl marks those range variables that are listed in the FROM
 *clause. It's false for RTEs that are added to a query behind the scenes, such
 *	  as the NEW and OLD variables for a rule, or the subqueries of a UNION.
 *	  This flag is not used during parsing (except in
 *transformLockingClause, q.v.); the parser now uses a separate "namespace" data
 *structure to control visibility.  But it is needed by ruleutils.c to determine
 *	  whether RTEs should be shown in decompiled queries.
 *
 *	  securityQuals is a list of security barrier quals (boolean
 *expressions), to be tested in the listed order before returning a row from the
 *	  relation.  It is always NIL in parser output.  Entries are added by
 *the rewriter to implement security-barrier views and/or row-level security.
 *	  Note that the planner turns each boolean expression into an implicitly
 *	  AND'ed sublist, as is its usual habit with qualification expressions.
 *
 * RangeTblEntry -
 *	  范围表是 RangeTblEntry 节点的列表。
 *
 *	  范围表项可以表示普通关系、FROM 中的子查询、或 JOIN 子句的结果。
 *	  （只有显式的 JOIN 语法会产生 RTE，多个 FROM 项导致的隐式连接不产生
 *RTE。 这是因为我们只需要 RTE 来处理外连接和连接输出列别名等 SQL 功能。）
 *	  RTEKind 指示的其他特殊 RTE 类型也存在。
 *
 *	  注意，我们认为 RTE_RELATION 覆盖了任何拥有 pg_class 项的内容。relkind
 *区分子情况。
 *
 *	  alias 是一个 Alias 节点，表示附加到 FROM 表达式的 AS 别名子句，或
 *NULL（如果没有子句）。
 *
 *	  eref 是表引用名和列引用名（真实的或别名）。注意系统列（OID
 *等）不包括在列列表中。 eref->aliasname 必须存在，通常用于错误消息等地方标识
 *RTE。
 *
 *	  在 RELATION RTE 中，alias 和 eref 中的 colnames 按物理属性编号建索引；
 *	  这意味着必须有已删除列的 colname 项。构建 RTE
 *时我们为已删除列插入空字符串 ""。
 *	  但注意，存储的规则可能对规则创建后被删除的列有非空 colnames
 *	  （即 colnames 可能因列重命名而过时）。
 *	  同样的注释适用于 FUNCTION RTE，当函数返回类型是命名的复合类型时。
 *
 *	  在 JOIN RTE 中，alias 和 eref 中的 colnames 与 joinaliasvars
 *项一一对应。 JOIN RTE 会在解析时已知列已删除时省略输入列。
 *	  但存储的规则中可能包含规则创建后被删除列的项。
 *	  （这只对规则中实际未引用的列可能）。加载存储规则时，我们用空指针替换此类列的
 *joinaliasvars 项。 （不能简单删除 joinaliasvars
 *列表中的项，因为此操作会影响引用列表其余部分的 Var 的 attnums。）
 *
 *	  inFromCl 标记 FROM 子句中列出的范围变量。对于隐式添加到查询中的 RTE，
 *	  如规则的 NEW 和 OLD 变量或 UNION 的子查询，它为 false。
 *	  解析期间不使用此标志（除了
 *transformLockingClause）；解析器现在使用单独的 "namespace"
 *数据结构来控制可见性。但 ruleutils.c 需要它来确定已编译查询中是否显示 RTE。
 *
 *	  securityQuals 是安全隔离限定条件（布尔表达式）列表，将在按列表顺序
 *	  的处理在关系返回行之前测试。在解析器输出中始终为 NIL。解析器
 *	  添加项以实现安全隔离视图和/或行级安全。
 *	  注意，规划器将每个布尔表达式转化为隐式 AND
 *连接的子列表，这是它处理限定表达式的常用习惯。
 *--------------------
 */
typedef enum RTEKind
{
  RTE_RELATION,        /* ordinary relation reference */
                       /* 普通关系引用 */
  RTE_SUBQUERY,        /* subquery in FROM */
                       /* FROM 中的子查询 */
  RTE_JOIN,            /* join */
                       /* 连接 */
  RTE_FUNCTION,        /* function in FROM */
                       /* FROM 中的函数 */
  RTE_TABLEFUNC,       /* TableFunc(.., column list) */
                       /* TableFunc(.., 列列表) */
  RTE_VALUES,          /* VALUES (<exprlist>), (<exprlist>), ... */
                       /* VALUES (<表达式列表>), (<表达式列表>), ... */
  RTE_CTE,             /* common table expr (WITH list element) */
                       /* 公用表表达式（WITH 列表元素） */
  RTE_NAMEDTUPLESTORE, /* tuplestore, e.g. for AFTER triggers */
                       /* 元组存储，例如用于 AFTER 触发器 */
  RTE_RESULT,          /* RTE represents an empty FROM clause; such
                        * RTEs are added by the planner, they're not
                        * present during parsing or rewriting */
                       /* RTE 表示空的 FROM 子句；此类 RTE 由规划器添加，
                        * 在解析或重写期间不存在 */
  RTE_GROUP,           /* the grouping step */
                       /* 分组步骤 */
} RTEKind;

typedef struct RangeTblEntry
{
  pg_node_attr(custom_read_write)

      NodeTag type;

  /*
   * Fields valid in all RTEs:
   *
   * put alias + eref first to make dump more legible
   *
   * 所有 RTE 中有效的字段：
   *
   * 将 alias + eref 放在前面以使转储更易读
   */
  /* user-written alias clause, if any */
  /* 用户编写的别名子句（如果有） */
  Alias *alias pg_node_attr(query_jumble_ignore);

  /*
   * Expanded reference names.  This uses a custom query jumble function so
   * that the table name is included in the computation, but not its list of
   * columns.
   *
   * 展开的引用名。这使用自定义查询指纹函数，以将表名纳入计算，
   * 但不包括其列列表。
   */
  Alias *eref pg_node_attr(custom_query_jumble);

  RTEKind rtekind; /* see above */
                   /* 见上文 */

  /*
   * Fields valid for a plain relation RTE (else zero):
   *
   * inh is true for relation references that should be expanded to include
   * inheritance children, if the rel has any.  In the parser, this will
   * only be true for RTE_RELATION entries.  The planner also uses this
   * field to mark RTE_SUBQUERY entries that contain UNION ALL queries that
   * it has flattened into pulled-up subqueries (creating a structure much
   * like the effects of inheritance).
   *
   * rellockmode is really LOCKMODE, but it's declared int to avoid having
   * to include lock-related headers here.  It must be RowExclusiveLock if
   * the RTE is an INSERT/UPDATE/DELETE/MERGE target, else RowShareLock if
   * the RTE is a SELECT FOR UPDATE/FOR SHARE target, else AccessShareLock.
   *
   * Note: in some cases, rule expansion may result in RTEs that are marked
   * with RowExclusiveLock even though they are not the target of the
   * current query; this happens if a DO ALSO rule simply scans the original
   * target table.  We leave such RTEs with their original lockmode so as to
   * avoid getting an additional, lesser lock.
   *
   * perminfoindex is 1-based index of the RTEPermissionInfo belonging to
   * this RTE in the containing struct's list of same; 0 if permissions need
   * not be checked for this RTE.
   *
   * As a special case, relid, relkind, rellockmode, and perminfoindex can
   * also be set (nonzero) in an RTE_SUBQUERY RTE.  This occurs when we
   * convert an RTE_RELATION RTE naming a view into an RTE_SUBQUERY
   * containing the view's query.  We still need to perform run-time locking
   * and permission checks on the view, even though it's not directly used
   * in the query anymore, and the most expedient way to do that is to
   * retain these fields from the old state of the RTE.
   *
   * As a special case, RTE_NAMEDTUPLESTORE can also set relid to indicate
   * that the tuple format of the tuplestore is the same as the referenced
   * relation.  This allows plans referencing AFTER trigger transition
   * tables to be invalidated if the underlying table is altered.
   *
   * 普通关系 RTE 有效的字段（其他情况为零）：
   *
   * inh 对于应展开以包含继承子表的关系引用为 true（如果该关系有继承子表）。
   * 在解析器中，这只对 RTE_RELATION 项为 true。规划器也使用此字段标记
   * 包含已被展开为提起子查询的 UNION ALL 查询的 RTE_SUBQUERY 项。
   *
   * rellockmode 实际上是 LOCKMODE，但被声明为 int 以避免在此包含锁相关头文件。
   * 如果 RTE 是 INSERT/UPDATE/DELETE/MERGE 目标，则必须为 RowExclusiveLock；
   * 如果是 SELECT FOR UPDATE/FOR SHARE 目标，则为 RowShareLock；否则为
   * AccessShareLock。
   *
   * 注意：在某些情况下，规则展开可能导致 RTE 被标记为 RowExclusiveLock，
   * 即使它不是当前查询的目标；这发生在 DO ALSO 规则只承扫原始目标表时。
   * 我们保留这些 RTE 的原始锁模式以避免获得额外的较小锁。
   *
   * perminfoindex 是属于此 RTE 的 RTEPermissionInfo 在包含结构同类列表中的 1
   * 基索引； 0 表示此 RTE 不需要检查权限。
   *
   * 作为特殊情况，relid、relkind、rellockmode 和 perminfoindex
   * 也可以设定（非零）于 RTE_SUBQUERY RTE。 当我们将命名视图的 RTE_RELATION RTE
   * 转换为包含视图查询的 RTE_SUBQUERY 时发生。
   * 即使视图不再直接用于查询中，我们仍需对其执行运行时锁定和权限检查。
   * 最便捷的方式是保留这些来自 RTE 旧状态的字段。
   *
   * 作为特殊情况，RTE_NAMEDTUPLESTORE 也可设定 relid
   * 以表明元组存储的元组格式与引用关系相同。 这允许在底层表被更改时让引用 AFTER
   * 觧发器过渡表的计划失效。
   */
  /* OID of the relation */
  /* 关系的 OID */
  Oid relid pg_node_attr(query_jumble_ignore);
  /* inheritance requested? */
  /* 是否请求继承？ */
  bool inh;
  /* relation kind (see pg_class.relkind) */
  /* 关系类型（参见 pg_class.relkind） */
  char relkind pg_node_attr(query_jumble_ignore);
  /* lock level that query requires on the rel */
  /* 查询对关系要求的锁定级别 */
  int rellockmode pg_node_attr(query_jumble_ignore);
  /* index of RTEPermissionInfo entry, or 0 */
  /* RTEPermissionInfo 项的索引，或 0 */
  Index perminfoindex pg_node_attr(query_jumble_ignore);
  /* sampling info, or NULL */
  /* 采样信息，或 NULL */
  struct TableSampleClause *tablesample;

  /*
   * Fields valid for a subquery RTE (else NULL):
   *
   * 子查询 RTE 有效的字段（其他情况为 NULL）：
   */
  /* the sub-query */
  /* 子查询 */
  Query *subquery;
  /* is from security_barrier view? */
  /* 是由 security_barrier 视图来的吗？ */
  bool security_barrier pg_node_attr(query_jumble_ignore);

  /*
   * Fields valid for a join RTE (else NULL/zero):
   *
   * joinaliasvars is a list of (usually) Vars corresponding to the columns
   * of the join result.  An alias Var referencing column K of the join
   * result can be replaced by the K'th element of joinaliasvars --- but to
   * simplify the task of reverse-listing aliases correctly, we do not do
   * that until planning time.  In detail: an element of joinaliasvars can
   * be a Var of one of the join's input relations, or such a Var with an
   * implicit coercion to the join's output column type, or a COALESCE
   * expression containing the two input column Vars (possibly coerced).
   * Elements beyond the first joinmergedcols entries are always just Vars,
   * and are never referenced from elsewhere in the query (that is, join
   * alias Vars are generated only for merged columns).  We keep these
   * entries only because they're needed in expandRTE() and similar code.
   *
   * Vars appearing within joinaliasvars are marked with varnullingrels sets
   * that describe the nulling effects of this join and lower ones.  This is
   * essential for FULL JOIN cases, because the COALESCE expression only
   * describes the semantics correctly if its inputs have been nulled by the
   * join.  For other cases, it allows expandRTE() to generate a valid
   * representation of the join's output without consulting additional
   * parser state.
   *
   * Within a Query loaded from a stored rule, it is possible for non-merged
   * joinaliasvars items to be null pointers, which are placeholders for
   * (necessarily unreferenced) columns dropped since the rule was made.
   * Also, once planning begins, joinaliasvars items can be almost anything,
   * as a result of subquery-flattening substitutions.
   *
   * joinleftcols is an integer list of physical column numbers of the left
   * join input rel that are included in the join; likewise joinrighttcols
   * for the right join input rel.  (Which rels those are can be determined
   * from the associated JoinExpr.)  If the join is USING/NATURAL, then the
   * first joinmergedcols entries in each list identify the merged columns.
   * The merged columns come first in the join output, then remaining
   * columns of the left input, then remaining columns of the right.
   *
   * Note that input columns could have been dropped after creation of a
   * stored rule, if they are not referenced in the query (in particular,
   * merged columns could not be dropped); this is not accounted for in
   * joinleftcols/joinrighttcols.
   *
   * 连接 RTE 有效的字段（其他情况为 NULL/零）：
   *
   * joinaliasvars 是一个（通常是）Var 列表，对应连接结果的列。引用连接结果
   * 第 K 列的别名 Var 可以被 joinaliasvars 的第 K 个元素替代——但为了简化
   * 正确反向列出别名的任务，我们到规划时才这样做。
   * 具体来说：joinaliasvars 的元素可以是连接输入关系之一的 Var，
   * 或带有到连接输出列类型的隐式转换的 Var，或包含两个输入列 Var 的 COALESCE
   * 表达式。 前 joinmergedcols 个项之后的元素始终只是
   * Var，不会在查询其他地方引用。 我们保留这些项仅因为 expandRTE()
   * 等代码需要它们。
   *
   * joinaliasvars 中的 Var 被标记了 varnullingrels 集合，它描述此连接
   * 及其下层连接的空化效果。这对 FULL JOIN 情况至关重要，因为 COALESCE 表达式
   * 只有当其输入被连接空化后才能正确描述语义。
   * 对于其他情况，它允许 expandRTE() 在不查阅额外解析器状态的情况下生成
   * 连接输出的有效表示。
   *
   * joinleftcols 是包含在连接中的左连接输入关系的物理列编号的整数列表；
   * joinrighttcols 类似。如果连接是 USING/NATURAL，则每个列表的前
   * joinmergedcols 项标识合并列。
   * 合并列先在连接输出中，然后是左输入的剩余列，再然后是右输入的剩余列。
   *
   * 注意，存储规则创建后输入列可能被删除（如果查询中未引用它们，
   * 具体来说，合并列不能被删除）；这在 joinleftcols/joinrighttcols 中没有反映。
   */
  JoinType jointype; /* type of join */
                     /* 连接类型 */
  /* number of merged (JOIN USING) columns */
  /* 合并（JOIN USING）列的数量 */
  int joinmergedcols pg_node_attr(query_jumble_ignore);
  /* list of alias-var expansions */
  /* 别名变量展开列表 */
  List *joinaliasvars pg_node_attr(query_jumble_ignore);
  /* left-side input column numbers */
  /* 左侧输入列编号 */
  List *joinleftcols pg_node_attr(query_jumble_ignore);
  /* right-side input column numbers */
  /* 右侧输入列编号 */
  List *joinrightcols pg_node_attr(query_jumble_ignore);

  /*
   * join_using_alias is an alias clause attached directly to JOIN/USING. It
   * is different from the alias field (below) in that it does not hide the
   * range variables of the tables being joined.
   *
   * join_using_alias 是直接附加到 JOIN/USING 的别名子句。它与下面的 alias
   * 字段不同， 因为它不隐藏正在连接的表的范围变量。
   */
  Alias *join_using_alias pg_node_attr(query_jumble_ignore);

  /*
   * Fields valid for a function RTE (else NIL/zero):
   *
   * When funcordinality is true, the eref->colnames list includes an alias
   * for the ordinality column.  The ordinality column is otherwise
   * implicit, and must be accounted for "by hand" in places such as
   * expandRTE().
   *
   * 函数 RTE 有效的字段（其他情况为 NIL/零）：
   *
   * 当 funcordinality 为 true 时，eref->colnames 列表包含序号列的别名。
   * 序号列在其他情况下是隐式的，必须在 expandRTE() 等地方"手动"考虑。
   */
  /* list of RangeTblFunction nodes */
  /* RangeTblFunction 节点列表 */
  List *functions;
  /* is this called WITH ORDINALITY? */
  /* 是否以 WITH ORDINALITY 调用？ */
  bool funcordinality;

  /*
   * Fields valid for a TableFunc RTE (else NULL):
   *
   * TableFunc RTE 有效的字段（其他情况为 NULL）：
   */
  TableFunc *tablefunc; /* TableFunc 节点 */

  /*
   * Fields valid for a values RTE (else NIL):
   *
   * values RTE 有效的字段（其他情况为 NIL）：
   */
  /* list of expression lists */
  /* 表达式列表的列表 */
  List *values_lists;

  /*
   * Fields valid for a CTE RTE (else NULL/zero):
   *
   * CTE RTE 有效的字段（其他情况为 NULL/零）：
   */
  /* name of the WITH list item */
  /* WITH 列表项的名称 */
  char *ctename;
  /* number of query levels up */
  /* 查询层级向上的数量 */
  Index ctelevelsup;
  /* is this a recursive self-reference? */
  /* 这是否是递归自引用？ */
  bool self_reference pg_node_attr(query_jumble_ignore);

  /*
   * Fields valid for CTE, VALUES, ENR, and TableFunc RTEs (else NIL):
   *
   * We need these for CTE RTEs so that the types of self-referential
   * columns are well-defined.  For VALUES RTEs, storing these explicitly
   * saves having to re-determine the info by scanning the values_lists. For
   * ENRs, we store the types explicitly here (we could get the information
   * from the catalogs if 'relid' was supplied, but we'd still need these
   * for TupleDesc-based ENRs, so we might as well always store the type
   * info here).  For TableFuncs, these fields are redundant with data in
   * the TableFunc node, but keeping them here allows some code sharing with
   * the other cases.
   *
   * For ENRs only, we have to consider the possibility of dropped columns.
   * A dropped column is included in these lists, but it will have zeroes in
   * all three lists (as well as an empty-string entry in eref).  Testing
   * for zero coltype is the standard way to detect a dropped column.
   *
   * CTE、VALUES、ENR 和 TableFunc RTE 有效的字段（其他情况为 NIL）：
   *
   * CTE RTE 需要这些以使自引用列的类型有明确定义。对于 VALUES RTE，显式存储
   * 这些列表可省去通过扫描 values_lists 重新确定信息。对于 ENR，我们在此
   * 显式存储类型（如果提供了 'relid' 可以从目录获取信息，但对于基于 TupleDesc
   * 的 ENR 仍需要这些，因此我们始终在此存储类型信息）。对于 TableFunc，
   * 这些字段与 TableFunc 节点中的数据冠余，但将它们保留在此可以与其他情况
   * 共享一些代码。
   *
   * 仅对于 ENR，我们必须考虑已删除列的可能性。已删除列包含在这些列表中，
   * 但在所有三个列表中都为零（以及 eref 中的空字符串项）。
   * 检查 coltype 是否为零是检测已删除列的标准方式。
   */
  /* OID list of column type OIDs */
  /* 列类型 OID 的 OID 列表 */
  List *coltypes pg_node_attr(query_jumble_ignore);
  /* integer list of column typmods */
  /* 列类型修饰符的整数列表 */
  List *coltypmods pg_node_attr(query_jumble_ignore);
  /* OID list of column collation OIDs */
  /* 列排序规则 OID 的 OID 列表 */
  List *colcollations pg_node_attr(query_jumble_ignore);

  /*
   * Fields valid for ENR RTEs (else NULL/zero):
   *
   * ENR RTE 有效的字段（其他情况为 NULL/零）：
   */
  /* name of ephemeral named relation */
  /* 短暂命名关系的名称 */
  char *enrname;
  /* estimated or actual from caller */
  /* 调用者提供的估计或实际元组数 */
  Cardinality enrtuples pg_node_attr(query_jumble_ignore);

  /*
   * Fields valid for a GROUP RTE (else NIL):
   *
   * GROUP RTE 有效的字段（其他情况为 NIL）：
   */
  /* list of grouping expressions */
  /* 分组表达式列表 */
  List *groupexprs;

  /*
   * Fields valid in all RTEs:
   *
   * 所有 RTE 中有效的字段：
   */
  /* was LATERAL specified? */
  /* 是否指定了 LATERAL？ */
  bool lateral pg_node_attr(query_jumble_ignore);
  /* present in FROM clause? */
  /* 是否在 FROM 子句中出现？ */
  bool inFromCl pg_node_attr(query_jumble_ignore);
  /* security barrier quals to apply, if any */
  /* 要应用的安全隔离限定条件（如果有） */
  List *securityQuals pg_node_attr(query_jumble_ignore);
} RangeTblEntry;

/*
 * RTEPermissionInfo
 * 		Per-relation information for permission checking. Added to the
 * Query node by the parser when adding the corresponding RTE to the query range
 * table and subsequently editorialized on by the rewriter if needed after rule
 * expansion.
 *
 * Only the relations directly mentioned in the query are checked for
 * access permissions by the core executor, so only their RTEPermissionInfos
 * are present in the Query.  However, extensions may want to check inheritance
 * children too, depending on the value of rte->inh, so it's copied in 'inh'
 * for their perusal.
 *
 * requiredPerms and checkAsUser specify run-time access permissions checks
 * to be performed at query startup.  The user must have *all* of the
 * permissions that are OR'd together in requiredPerms (never 0!).  If
 * checkAsUser is not zero, then do the permissions checks using the access
 * rights of that user, not the current effective user ID.  (This allows rules
 * to act as setuid gateways.)
 *
 * For SELECT/INSERT/UPDATE permissions, if the user doesn't have table-wide
 * permissions then it is sufficient to have the permissions on all columns
 * identified in selectedCols (for SELECT) and/or insertedCols and/or
 * updatedCols (INSERT with ON CONFLICT DO UPDATE may have all 3).
 * selectedCols, insertedCols and updatedCols are bitmapsets, which cannot have
 * negative integer members, so we subtract FirstLowInvalidHeapAttributeNumber
 * from column numbers before storing them in these fields.  A whole-row Var
 * reference is represented by setting the bit for InvalidAttrNumber.
 *
 * updatedCols is also used in some other places, for example, to determine
 * which triggers to fire and in FDWs to know which changed columns they need
 * to ship off.
 *
 * RTEPermissionInfo
 * 		每个关系的权限检查信息。当解析器将对应 RTE
 * 添加到查询范围表时添加到 Query 节点， 并在规则展开后如需由重写器进行编辑。
 *
 * 核心执行器只检查查询中直接提到的关系的访问权限，因此 Query 中只包含它们的
 * RTEPermissionInfo。但延伸展可能也想检查继承子表，这取决于 rte->inh 的值，
 * 因此将其复制到 'inh' 以供他们参阅。
 *
 * requiredPerms 和 checkAsUser 指定在查询启动时执行的运行时访问权限检查。
 * 用户必须拥有 requiredPerms 中所有 OR 在一起的权限（永远不为 0！）。
 * 如果 checkAsUser 不为零，则使用该用户的访问权限而非当前有效用户 ID
 * 进行权限检查。 （这允许规则充当 setuid 网关。）
 *
 * 对于 SELECT/INSERT/UPDATE 权限，如果用户没有表级权限，则对 selectedCols
 * （SELECT）和/或 insertedCols 和/或 updatedCols 中标识的所有列有权限即可。
 * selectedCols、insertedCols 和 updatedCols 是位图集，不能有负整数成员，
 * 因此在存储到这些字段前，我们从列编号中减去
 * FirstLowInvalidHeapAttributeNumber。 整行 Var 引用通过设置 InvalidAttrNumber
 * 的位来表示。
 *
 * updatedCols 也在其他地方使用，例如确定像哪些觧发器以及 FDW
 * 中得知哪些已更改列需要传输。
 */
typedef struct RTEPermissionInfo
{
  NodeTag type;

  Oid relid;               /* relation OID */
                           /* 关系 OID */
  bool inh;                /* separately check inheritance children? */
                           /* 是否单独检查继承子表？ */
  AclMode requiredPerms;   /* bitmask of required access permissions */
                           /* 所需访问权限的位掩码 */
  Oid checkAsUser;         /* if valid, check access as this role */
                           /* 若有效，以此角色身份检查访问权限 */
  Bitmapset *selectedCols; /* columns needing SELECT permission */
                           /* 需要 SELECT 权限的列 */
  Bitmapset *insertedCols; /* columns needing INSERT permission */
                           /* 需要 INSERT 权限的列 */
  Bitmapset *updatedCols;  /* columns needing UPDATE permission */
                           /* 需要 UPDATE 权限的列 */
} RTEPermissionInfo;

/*
 * RangeTblFunction -
 *	  RangeTblEntry subsidiary data for one function in a FUNCTION RTE.
 *
 * If the function had a column definition list (required for an
 * otherwise-unspecified RECORD result), funccolnames lists the names given
 * in the definition list, funccoltypes lists their declared column types,
 * funccoltypmods lists their typmods, funccolcollations their collations.
 * Otherwise, those fields are NIL.
 *
 * Notice we don't attempt to store info about the results of functions
 * returning named composite types, because those can change from time to
 * time.  We do however remember how many columns we thought the type had
 * (including dropped columns!), so that we can successfully ignore any
 * columns added after the query was parsed.
 *
 * The query jumbling only needs to track the function expression.
 *
 * RangeTblFunction -
 *	  FUNCTION RTE 中一个函数的 RangeTblEntry 辅助数据。
 *
 * 如果函数有列定义列表（对于未指定的 RECORD 结果必需），
 * funccolnames 列出定义列表中给定的名称，funccoltypes 列出其声明的列类型，
 * funccoltypmods 列出类型修饰符，funccolcollations 列出排序规则。
 * 否则，这些字段为 NIL。
 *
 * 注意，我们不试图存储返回命名复合类型的函数结果信息，因为那些信息可能随时变化。
 * 但我们记得我们认为类型有多少列（包括已删除的列！），以便成功忽略解析查询后添加的任何列。
 *
 * 查询指纹只需跟踪函数表达式。
 */
typedef struct RangeTblFunction
{
  NodeTag type;

  Node *funcexpr; /* expression tree for func call */
                  /* 函数调用的表达式树 */
  /* number of columns it contributes to RTE */
  /* 它对 RTE 贡献的列数 */
  int funccolcount pg_node_attr(query_jumble_ignore);
  /* These fields record the contents of a column definition list, if any: */
  /* 以下字段记录列定义列表的内容（如果有）： */
  /* column names (list of String) */
  /* 列名列表（String 列表） */
  List *funccolnames pg_node_attr(query_jumble_ignore);
  /* OID list of column type OIDs */
  /* 列类型 OID 的 OID 列表 */
  List *funccoltypes pg_node_attr(query_jumble_ignore);
  /* integer list of column typmods */
  /* 列类型修饰符的整数列表 */
  List *funccoltypmods pg_node_attr(query_jumble_ignore);
  /* OID list of column collation OIDs */
  /* 列排序规则 OID 的 OID 列表 */
  List *funccolcollations pg_node_attr(query_jumble_ignore);

  /* This is set during planning for use by the executor: */
  /* 这在规划期间设定，以便执行器使用： */
  /* PARAM_EXEC Param IDs affecting this func */
  /* 影响此函数的 PARAM_EXEC Param ID */
  Bitmapset *funcparams pg_node_attr(query_jumble_ignore);
} RangeTblFunction;

/*
 * TableSampleClause - TABLESAMPLE appearing in a transformed FROM clause
 *
 * Unlike RangeTableSample, this is a subnode of the relevant RangeTblEntry.
 *
 * TableSampleClause - 出现在转换后的 FROM 子句中的 TABLESAMPLE
 *
 * 与 RangeTableSample 不同，这是相关 RangeTblEntry 的子节点。
 */
typedef struct TableSampleClause
{
  NodeTag type;
  Oid tsmhandler;   /* OID of the tablesample handler function */
                    /* 采样处理器函数的 OID */
  List *args;       /* tablesample argument expression(s) */
                    /* 采样参数表达式 */
  Expr *repeatable; /* REPEATABLE expression, or NULL if none */
                    /* REPEATABLE 表达式，若无则为 NULL */
} TableSampleClause;

/*
 * WithCheckOption -
 *		representation of WITH CHECK OPTION checks to be applied to new
 *tuples when inserting/updating an auto-updatable view, or RLS WITH CHECK
 *		policies to be applied when inserting/updating a relation with
 *RLS.
 *
 * WithCheckOption -
 *		辽避自动可更新视图中插入/更新元组时应用的 WITH CHECK OPTION
 *检查的表示， 或插入/更新具有 RLS 关系时应用的 RLS WITH CHECK 策略。
 */
typedef enum WCOKind
{
  WCO_VIEW_CHECK,             /* WCO on an auto-updatable view */
  WCO_RLS_INSERT_CHECK,       /* RLS INSERT WITH CHECK policy */
  WCO_RLS_UPDATE_CHECK,       /* RLS UPDATE WITH CHECK policy */
  WCO_RLS_CONFLICT_CHECK,     /* RLS ON CONFLICT DO UPDATE USING policy */
  WCO_RLS_MERGE_UPDATE_CHECK, /* RLS MERGE UPDATE USING policy */
  WCO_RLS_MERGE_DELETE_CHECK, /* RLS MERGE DELETE USING policy */
} WCOKind;

typedef struct WithCheckOption
{
  NodeTag type;
  WCOKind kind;  /* kind of WCO */
                 /* WCO 类型 */
  char *relname; /* name of relation that specified the WCO */
                 /* 指定该 WCO 的关系名称 */
  char *polname; /* name of RLS policy being checked */
                 /* 正在检查的 RLS 策略名称 */
  Node *qual;    /* constraint qual to check */
                 /* 要检查的约束限定条件 */
  bool cascaded; /* true for a cascaded WCO on a view */
                 /* 对于视图上的级联 WCO 为 true */
} WithCheckOption;

/*
 * SortGroupClause -
 *		representation of ORDER BY, GROUP BY, PARTITION BY,
 *		DISTINCT, DISTINCT ON items
 *
 * You might think that ORDER BY is only interested in defining ordering,
 * and GROUP/DISTINCT are only interested in defining equality.  However,
 * one way to implement grouping is to sort and then apply a "uniq"-like
 * filter.  So it's also interesting to keep track of possible sort operators
 * for GROUP/DISTINCT, and in particular to try to sort for the grouping
 * in a way that will also yield a requested ORDER BY ordering.  So we need
 * to be able to compare ORDER BY and GROUP/DISTINCT lists, which motivates
 * the decision to give them the same representation.
 *
 * tleSortGroupRef must match ressortgroupref of exactly one entry of the
 *		query's targetlist; that is the expression to be sorted or
 *grouped by. eqop is the OID of the equality operator. sortop is the OID of the
 *ordering operator (a "<" or ">" operator), or InvalidOid if not available.
 * nulls_first means about what you'd expect.  If sortop is InvalidOid
 *		then nulls_first is meaningless and should be set to false.
 * hashable is true if eqop is hashable (note this condition also depends
 *		on the datatype of the input expression).
 *
 * In an ORDER BY item, all fields must be valid.  (The eqop isn't essential
 * here, but it's cheap to get it along with the sortop, and requiring it
 * to be valid eases comparisons to grouping items.)  Note that this isn't
 * actually enough information to determine an ordering: if the sortop is
 * collation-sensitive, a collation OID is needed too.  We don't store the
 * collation in SortGroupClause because it's not available at the time the
 * parser builds the SortGroupClause; instead, consult the exposed collation
 * of the referenced targetlist expression to find out what it is.
 *
 * In a grouping item, eqop must be valid.  If the eqop is a btree equality
 * operator, then sortop should be set to a compatible ordering operator.
 * We prefer to set eqop/sortop/nulls_first to match any ORDER BY item that
 * the query presents for the same tlist item.  If there is none, we just
 * use the default ordering op for the datatype.
 *
 * If the tlist item's type has a hash opclass but no btree opclass, then
 * we will set eqop to the hash equality operator, sortop to InvalidOid,
 * and nulls_first to false.  A grouping item of this kind can only be
 * implemented by hashing, and of course it'll never match an ORDER BY item.
 *
 * The hashable flag is provided since we generally have the requisite
 * information readily available when the SortGroupClause is constructed,
 * and it's relatively expensive to get it again later.  Note there is no
 * need for a "sortable" flag since OidIsValid(sortop) serves the purpose.
 *
 * A query might have both ORDER BY and DISTINCT (or DISTINCT ON) clauses.
 * In SELECT DISTINCT, the distinctClause list is as long or longer than the
 * sortClause list, while in SELECT DISTINCT ON it's typically shorter.
 * The two lists must match up to the end of the shorter one --- the parser
 * rearranges the distinctClause if necessary to make this true.  (This
 * restriction ensures that only one sort step is needed to both satisfy the
 * ORDER BY and set up for the Unique step.  This is semantically necessary
 * for DISTINCT ON, and presents no real drawback for DISTINCT.)
 *
 * SortGroupClause -
 *		ORDER BY、GROUP BY、PARTITION BY、DISTINCT、DISTINCT ON 项的表示
 *
 * 你可能认为 ORDER BY 只关心定义排序，GROUP/DISTINCT 只关心定义等山性。
 * 但实现分组的一种方式是排序然后应用类似 "uniq" 的过滤器。因此，跟踪
 * GROUP/DISTINCT 的可能排序算子也很有意义，尤其是试着以将产生请求的
 * ORDER BY 排序的方式进行分组排序。因此我们需要能够比较 ORDER BY 和
 * GROUP/DISTINCT 列表，这也是给它们相同表示形式的动机。
 *
 * tleSortGroupRef 必须与查询目标列表中正好一个项的 ressortgroupref 匹配；
 *		这就是要排序或分组的表达式。
 * eqop 是等山运算符的 OID。
 * sortop 是排序运算符（"<" 或 ">" 运算符）的 OID，不可用时为 InvalidOid。
 * nulls_first 的含义与预期相同。如果 sortop 为 InvalidOid，则 nulls_first
 *无意义应设为 false。 hashable 为 true 当 eqop
 *可哈希（注意该条件也取决于输入表达式的数据类型）。
 *
 * 在 ORDER BY 项中，所有字段必须有效。如果 sortop 对排序规则敏感，
 * 还需要一个排序规则 OID。我们不将排序规则存储在 SortGroupClause 中，
 * 因为解析器构建 SortGroupClause
 *时还不可用；请查询引用目标列表表达式的公开排序规则。
 *
 * 在分组项中，eqop 必须有效。如果 eqop 是 btree 等山运算符，则 sortop
 *应设为兼容的排序运算符。 我们尽量设置 eqop/sortop/nulls_first
 *以匹配查询针对同一 tlist 项呼现的任何 ORDER BY 项。
 * 如果没有，就使用该数据类型的默认排序运算符。
 *
 * 如果 tlist 项类型有哈希操作符类但没有 btree 操作符类，
 * 我们将设定 eqop 为哈希等山运算符，sortop 为 InvalidOid，nulls_first 为
 *false。 此类分组项只能通过哈希实现，当然也不会匹配 ORDER BY 项。
 *
 * 提供 hashable 标志是因为构建 SortGroupClause 时通常有现成的所需信息，
 * 且稍后再获取相对昂贵。注意不需要 "sortable" 标志，因为 OidIsValid(sortop)
 *可以起到同样和用途。
 *
 * 查询可能同时有 ORDER BY 和 DISTINCT（或 DISTINCT ON）子句。
 * 在 SELECT DISTINCT 中，distinctClause 列表与 sortClause 列表一样长或更长，
 * 而 SELECT DISTINCT ON 中通常较短。两个列表必须匹配至较短的一个的末尾——
 * 必要时解析器会重新排列 distinctClause 这是真的。（此限制确保只需 ORDER
 *BY和建立 Unique 步骤的排序算法。对 DISTINCT ON 语义上必要，对 DISTINCT
 *则没有实际缺点。）
 */
typedef struct SortGroupClause
{
  NodeTag type;
  Index tleSortGroupRef; /* reference into targetlist */
                         /* 对目标列表（targetlist）的引用 */
  Oid eqop;              /* the equality operator ('=' op) */
                         /* 等号运算符（'=' 运算符） */
  Oid sortop;            /* the ordering operator ('<' op), or 0 */
                         /* 排序运算符（'<' 运算符），或为 0 */
  bool reverse_sort;     /* is sortop a "greater than" operator? */
                         /* sortop 是否为"大于"运算符？ */
  bool nulls_first;      /* do NULLs come before normal values? */
                         /* NULL 是否排在普通值之前？ */
  /* can eqop be implemented by hashing? */
  /* eqop 是否可以通过哈希实现？ */
  bool hashable pg_node_attr(query_jumble_ignore);
} SortGroupClause;

/*
 * GroupingSet -
 *		representation of CUBE, ROLLUP and GROUPING SETS clauses
 *
 * In a Query with grouping sets, the groupClause contains a flat list of
 * SortGroupClause nodes for each distinct expression used.  The actual
 * structure of the GROUP BY clause is given by the groupingSets tree.
 *
 * In the raw parser output, GroupingSet nodes (of all types except SIMPLE
 * which is not used) are potentially mixed in with the expressions in the
 * groupClause of the SelectStmt.  (An expression can't contain a GroupingSet,
 * but a list may mix GroupingSet and expression nodes.)  At this stage, the
 * content of each node is a list of expressions, some of which may be RowExprs
 * which represent sublists rather than actual row constructors, and nested
 * GroupingSet nodes where legal in the grammar.  The structure directly
 * reflects the query syntax.
 *
 * In parse analysis, the transformed expressions are used to build the tlist
 * and groupClause list (of SortGroupClause nodes), and the groupingSets tree
 * is eventually reduced to a fixed format:
 *
 * EMPTY nodes represent (), and obviously have no content
 *
 * SIMPLE nodes represent a list of one or more expressions to be treated as an
 * atom by the enclosing structure; the content is an integer list of
 * ressortgroupref values (see SortGroupClause)
 *
 * CUBE and ROLLUP nodes contain a list of one or more SIMPLE nodes.
 *
 * SETS nodes contain a list of EMPTY, SIMPLE, CUBE or ROLLUP nodes, but after
 * parse analysis they cannot contain more SETS nodes; enough of the syntactic
 * transforms of the spec have been applied that we no longer have arbitrarily
 * deep nesting (though we still preserve the use of cube/rollup).
 *
 * Note that if the groupingSets tree contains no SIMPLE nodes (only EMPTY
 * nodes at the leaves), then the groupClause will be empty, but this is still
 * an aggregation query (similar to using aggs or HAVING without GROUP BY).
 *
 * As an example, the following clause:
 *
 * GROUP BY GROUPING SETS ((a,b), CUBE(c,(d,e)))
 *
 * looks like this after raw parsing:
 *
 * SETS( RowExpr(a,b) , CUBE( c, RowExpr(d,e) ) )
 *
 * and parse analysis converts it to:
 *
 * SETS( SIMPLE(1,2), CUBE( SIMPLE(3), SIMPLE(4,5) ) )
 *
 * GroupingSet -
 *		CUBE、ROLLUP 和 GROUPING SETS 子句的表示
 *
 * 在有分组集的查询中，groupClause 包含每个使用的展开表达式的 SortGroupClause
 *节点的平列表。 GROUP BY 子句的实际结构由 groupingSets 树给出。
 *
 * 在原始解析器输出中，GroupingSet 节点（除 SIMPLE 类型外）可能与 SelectStmt 的
 *groupClause 中 的表达式混在一起。解析分析后，groupingSets
 *树最终被简化为固定格式。
 */
typedef enum GroupingSetKind
{
  GROUPING_SET_EMPTY,
  GROUPING_SET_SIMPLE,
  GROUPING_SET_ROLLUP,
  GROUPING_SET_CUBE,
  GROUPING_SET_SETS,
} GroupingSetKind;

typedef struct GroupingSet
{
  NodeTag type;
  GroupingSetKind kind pg_node_attr(query_jumble_ignore); /* kind of grouping set */
                                                          /* 分组集类型 */
  List *content;                                          /* content of grouping set */
                                                          /* 分组集内容 */
  ParseLoc location;                                      /* token location, or -1 if unknown */
                                                          /* 标记位置，若未知则为 -1 */
} GroupingSet;

/*
 * WindowClause -
 *		transformed representation of WINDOW and OVER clauses
 *
 * A parsed Query's windowClause list contains these structs.  "name" is set
 * if the clause originally came from WINDOW, and is NULL if it originally
 * was an OVER clause (but note that we collapse out duplicate OVERs).
 * partitionClause and orderClause are lists of SortGroupClause structs.
 * partitionClause is sanitized by the query planner to remove any columns or
 * expressions belonging to redundant PathKeys.
 * If we have RANGE with offset PRECEDING/FOLLOWING, the semantics of that are
 * specified by startInRangeFunc/inRangeColl/inRangeAsc/inRangeNullsFirst
 * for the start offset, or endInRangeFunc/inRange* for the end offset.
 * winref is an ID number referenced by WindowFunc nodes; it must be unique
 * among the members of a Query's windowClause list.
 * When refname isn't null, the partitionClause is always copied from there;
 * the orderClause might or might not be copied (see copiedOrder); the framing
 * options are never copied, per spec.
 *
 * The information relevant for the query jumbling is the partition clause
 * type and its bounds.
 *
 * WindowClause -
 *		WINDOW 和 OVER 子句的转换表示
 *
 * 已解析的 Query 的 windowClause 列表包含这些结构。如果子句原来来自
 *WINDOW，则设定 "name"; 如果原始为 OVER 子句，则为 NULL（注意我们合并重复的
 *OVER）。 partitionClause 和 orderClause 是 SortGroupClause 结构的列表。
 * partitionClause 由查询规划器清理以删除属于冗余 PathKeys 的列或表达式。
 * winref 是 WindowFunc 节点引用的 ID 号；它在 Query 的 windowClause
 *列表中必须唯一。 当 refname 非空时，partitionClause 始终当此复制；orderClause
 *可能或不可能被复制； 框选项永远不复制。
 *
 * 与查询指纹相关的信息是分区子句类型及其边界。
 */
typedef struct WindowClause
{
  NodeTag type;
  /* window name (NULL in an OVER clause) */
  /* 窗口名称（在 OVER 子句中为 NULL） */
  char *name pg_node_attr(query_jumble_ignore);
  /* referenced window name, if any */
  /* 被引用的窗口名称（如果有） */
  char *refname pg_node_attr(query_jumble_ignore);
  List *partitionClause; /* PARTITION BY list */
                         /* PARTITION BY 列表 */
  /* ORDER BY list */
  /* ORDER BY 列表 */
  List *orderClause;
  int frameOptions;  /* frame_clause options, see WindowDef */
                     /* 框架子句选项，见 WindowDef */
  Node *startOffset; /* expression for starting bound, if any */
                     /* 起始边界表达式（如果有） */
  Node *endOffset;   /* expression for ending bound, if any */
                     /* 结束边界表达式（如果有） */
  /* in_range function for startOffset */
  /* 用于 startOffset 的 in_range 函数 */
  Oid startInRangeFunc pg_node_attr(query_jumble_ignore);
  /* in_range function for endOffset */
  /* 用于 endOffset 的 in_range 函数 */
  Oid endInRangeFunc pg_node_attr(query_jumble_ignore);
  /* collation for in_range tests */
  /* 用于 in_range 测试的排序规则 */
  Oid inRangeColl pg_node_attr(query_jumble_ignore);
  /* use ASC sort order for in_range tests? */
  /* in_range 测试是否使用 ASC 排序？ */
  bool inRangeAsc pg_node_attr(query_jumble_ignore);
  /* nulls sort first for in_range tests? */
  /* in_range 测试中 NULL 是否排在最前面？ */
  bool inRangeNullsFirst pg_node_attr(query_jumble_ignore);
  Index winref; /* ID referenced by window functions */
                /* 窗口函数引用的 ID */
  /* did we copy orderClause from refname? */
  /* 是否从 refname 复制了 orderClause？ */
  bool copiedOrder pg_node_attr(query_jumble_ignore);
} WindowClause;

/*
 * RowMarkClause -
 *	   parser output representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * Query.rowMarks contains a separate RowMarkClause node for each relation
 * identified as a FOR [KEY] UPDATE/SHARE target.  If one of these clauses
 * is applied to a subquery, we generate RowMarkClauses for all normal and
 * subquery rels in the subquery, but they are marked pushedDown = true to
 * distinguish them from clauses that were explicitly written at this query
 * level.  Also, Query.hasForUpdate tells whether there were explicit FOR
 * UPDATE/SHARE/KEY SHARE clauses in the current query level.
 *
 * RowMarkClause -
 *	   FOR [KEY] UPDATE/SHARE 子句的解析器输出表示
 *
 * Query.rowMarks 包含每个被标识为 FOR [KEY] UPDATE/SHARE 目标的关系的单独
 *RowMarkClause 节点。
 * 如果其中一个子句应用于子查询，我们为子查询中的所有普通关系和子查询关系生成
 *RowMarkClause， 但它们被标记 pushedDown = true
 *以将其与在此查询级别显式编写的子句区分开。 此外，Query.hasForUpdate
 *告知当前查询级别是否有显式的 FOR UPDATE/SHARE/KEY SHARE 子句。
 */
typedef struct RowMarkClause
{
  NodeTag type;
  Index rti; /* range table index of target relation */
             /* 目标关系的范围表索引 */
  LockClauseStrength strength; /* lock strength */
                               /* 锁定强度 */
  LockWaitPolicy waitPolicy;   /* NOWAIT and SKIP LOCKED */
                               /* NOWAIT 和 SKIP LOCKED */
  bool pushedDown;             /* pushed down from higher query level? */
                               /* 是否从更高查询级别下推？ */
} RowMarkClause;

/*
 * WithClause -
 *	   representation of WITH clause
 *
 * Note: WithClause does not propagate into the Query representation;
 * but CommonTableExpr does.
 *
 * WithClause -
 *	   WITH 子句的表示
 *
 * 注意：WithClause 不会传播到 Query 表示中；但 CommonTableExpr 会。
 */
typedef struct WithClause
{
  NodeTag type;
  List *ctes;        /* list of CommonTableExprs */
                     /* CommonTableExpr 列表 */
  bool recursive;    /* true = WITH RECURSIVE */
                     /* true = WITH RECURSIVE */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} WithClause;

/*
 * InferClause -
 *		ON CONFLICT unique index inference clause
 *
 * Note: InferClause does not propagate into the Query representation.
 *
 * InferClause -
 *		ON CONFLICT 唯一索引推断子句
 *
 * 注意：InferClause 不会传播到 Query 表示中。
 */
typedef struct InferClause
{
  NodeTag type;
  List *indexElems;  /* IndexElems to infer unique index */
                     /* 用于推断唯一索引的 IndexElem 列表 */
  Node *whereClause; /* qualification (partial-index predicate) */
                     /* 限定条件（部分索引断言） */
  char *conname;     /* Constraint name, or NULL if unnamed */
                     /* 约束名称，若无名则为 NULL */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} InferClause;

/*
 * OnConflictClause -
 *		representation of ON CONFLICT clause
 *
 * Note: OnConflictClause does not propagate into the Query representation.
 *
 * OnConflictClause -
 *		ON CONFLICT 子句的表示
 *
 * 注意：OnConflictClause 不会传播到 Query 表示中。
 */
typedef struct OnConflictClause
{
  NodeTag type;
  OnConflictAction action; /* DO NOTHING or UPDATE? */
                           /* DO NOTHING 还是 UPDATE？ */
  InferClause *infer;      /* Optional index inference clause */
                           /* 可选的索引推断子句 */
  List *targetList;        /* the target list (of ResTarget) */
                           /* 目标列表（ResTarget 列表） */
  Node *whereClause;       /* qualifications */
                           /* 限定条件 */
  ParseLoc location;       /* token location, or -1 if unknown */
                           /* 标记位置，若未知则为 -1 */
} OnConflictClause;

/*
 * CommonTableExpr -
 *	   representation of WITH list element
 *
 * CommonTableExpr -
 *	   WITH 列表元素的表示
 */

typedef enum CTEMaterialize
{
  CTEMaterializeDefault, /* no option specified */
  CTEMaterializeAlways,  /* MATERIALIZED */
  CTEMaterializeNever,   /* NOT MATERIALIZED */
} CTEMaterialize;

typedef struct CTESearchClause
{
  NodeTag type;
  List *search_col_list;     /* search column list */
                             /* 搜索列列表 */
  bool search_breadth_first; /* search breadth first? */
                             /* 是否广度优先搜索？ */
  char *search_seq_column;   /* search sequence column */
                             /* 搜索顺序列 */
  ParseLoc location;         /* token location, or -1 if unknown */
                             /* 标记位置，若未知则为 -1 */
} CTESearchClause;

typedef struct CTECycleClause
{
  NodeTag type;
  List *cycle_col_list;      /* cycle column list */
                             /* 循环列列表 */
  char *cycle_mark_column;   /* cycle mark column */
                             /* 循环标记列 */
  Node *cycle_mark_value;    /* cycle mark value */
                             /* 循环标记值 */
  Node *cycle_mark_default;  /* cycle mark default value */
                             /* 循环标记默认值 */
  char *cycle_path_column;   /* cycle path column */
                             /* 循环路径列 */
  ParseLoc location;         /* token location, or -1 if unknown */
                             /* 标记位置，若未知则为 -1 */
  /* These fields are set during parse analysis: */
  /* 以下字段在解析分析期间设定： */
  Oid cycle_mark_type;       /* common type of _value and _default */
                             /* _value 和 _default 的共同类型 */
  int cycle_mark_typmod;     /* typmod for cycle mark column */
                             /* 循环标记列的类型修饰符 */
  Oid cycle_mark_collation;  /* collation for cycle mark column */
                             /* 循环标记列的排序规则 */
  Oid cycle_mark_neop;       /* <> operator for type */
                             /* 该类型的 <> 运算符 */
} CTECycleClause;

typedef struct CommonTableExpr
{
  NodeTag type;

  /*
   * Query name (never qualified).  The string name is included in the query
   * jumbling because RTE_CTE RTEs need it.
   *
   * 查询名称（永不限定）。字符串名称包含在查询指纹中，因为 RTE_CTE 需要它。
   */
  char *ctename;
  /* optional list of column names */
  /* 可选的列名列表 */
  List *aliascolnames pg_node_attr(query_jumble_ignore);
  CTEMaterialize ctematerialized; /* is this an optimization fence? */
                                  /* 这是优化屏障吗？ */
  /* SelectStmt/InsertStmt/etc before parse analysis, Query afterwards: */
  /* 解析分析前为 SelectStmt/InsertStmt 等，分析后为 Query： */
  Node *ctequery; /* the CTE's subquery */
                  /* CTE 的子查询 */
  CTESearchClause *search_clause pg_node_attr(query_jumble_ignore);
  CTECycleClause *cycle_clause pg_node_attr(query_jumble_ignore);
  ParseLoc location; /* token location, or -1 if unknown */
  /* These fields are set during parse analysis: */
  /* 这些字段在解析分析期间设定： */
  /* is this CTE actually recursive? */
  /* 这个 CTE 实际上是递归的吗？ */
  bool cterecursive pg_node_attr(query_jumble_ignore);

  /*
   * Number of RTEs referencing this CTE (excluding internal
   * self-references), irrelevant for query jumbling.
   *
   * 引用此 CTE 的 RTE 数量（不包括内部自引用），与查询指纹无关。
   */
  int cterefcount pg_node_attr(query_jumble_ignore);
  /* list of output column names */
  /* 输出列名列表 */
  List *ctecolnames pg_node_attr(query_jumble_ignore);
  /* OID list of output column type OIDs */
  /* 输出列类型 OID 的 OID 列表 */
  List *ctecoltypes pg_node_attr(query_jumble_ignore);
  /* integer list of output column typmods */
  /* 输出列类型修饰符的整数列表 */
  List *ctecoltypmods pg_node_attr(query_jumble_ignore);
  /* OID list of column collation OIDs */
  /* 列排序规则 OID 的 OID 列表 */
  List *ctecolcollations pg_node_attr(query_jumble_ignore);
} CommonTableExpr;

/* Convenience macro to get the output tlist of a CTE's query */
/* 获取 CTE 查询的输出 tlist 的便据宏 */
#define GetCTETargetList(cte)                                                  \
  (AssertMacro(IsA((cte)->ctequery, Query)),                                   \
   ((Query *)(cte)->ctequery)->commandType == CMD_SELECT                       \
       ? ((Query *)(cte)->ctequery)->targetList                                \
       : ((Query *)(cte)->ctequery)->returningList)

/*
 * MergeWhenClause -
 *		raw parser representation of a WHEN clause in a MERGE statement
 *
 * This is transformed into MergeAction by parse analysis
 *
 * MergeWhenClause -
 *		MERGE 语句中 WHEN 子句的原始解析器表示
 *
 * 这由解析分析转换为 MergeAction
 */
typedef struct MergeWhenClause
{
  NodeTag type;
  MergeMatchKind matchKind; /* MATCHED/NOT MATCHED BY SOURCE/TARGET */
                            /* MATCHED/NOT MATCHED BY SOURCE/TARGET 类型 */
  CmdType commandType;      /* INSERT/UPDATE/DELETE/DO NOTHING */
                            /* INSERT/UPDATE/DELETE/DO NOTHING 动作 */
  OverridingKind override;  /* OVERRIDING clause */
                            /* OVERRIDING 子句 */
  Node *condition;          /* WHEN conditions (raw parser) */
                            /* WHEN 条件（原始解析器） */
  List *targetList;         /* INSERT/UPDATE targetlist */
                            /* INSERT/UPDATE 目标列表 */
  /* the following members are only used in INSERT actions */
  /* 以下成员仅用于 INSERT 动作 */
  List *values;             /* VALUES to INSERT, or NULL */
                            /* 要 INSERT 的 VALUES，或为 NULL */
} MergeWhenClause;

/*
 * ReturningOptionKind -
 *		Possible kinds of option in RETURNING WITH(...) list
 *
 * Currently, this is used only for specifying OLD/NEW aliases.
 *
 * ReturningOptionKind -
 *		RETURNING WITH(...) 列表中选项的可能类型
 *
 * 目前，这仅用于指定 OLD/NEW 别名。
 */
typedef enum ReturningOptionKind
{
  RETURNING_OPTION_OLD, /* specify alias for OLD in RETURNING */
  RETURNING_OPTION_NEW, /* specify alias for NEW in RETURNING */
} ReturningOptionKind;

/*
 * ReturningOption -
 *		An individual option in the RETURNING WITH(...) list
 *
 * ReturningOption -
 *		RETURNING WITH(...) 列表中的单个选项
 */
typedef struct ReturningOption
{
  NodeTag type;
  ReturningOptionKind option; /* specified option */
                              /* 指定的选项 */
  char *value;                /* option's value */
                              /* 选项值 */
  ParseLoc location;          /* token location, or -1 if unknown */
                              /* 标记位置，若未知则为 -1 */
} ReturningOption;

/*
 * ReturningClause -
 *		List of RETURNING expressions, together with any WITH(...)
 *options
 *
 * ReturningClause -
 *		RETURNING 表达式列表，连同任何 WITH(...) 选项
 */
typedef struct ReturningClause
{
  NodeTag type;
  List *options; /* list of ReturningOption elements */
                 /* ReturningOption 元素列表 */
  List *exprs;   /* list of expressions to return */
                 /* 要返回的表达式列表 */
} ReturningClause;

/*
 * TriggerTransition -
 *	   representation of transition row or table naming clause
 *
 * Only transition tables are initially supported in the syntax, and only for
 * AFTER triggers, but other permutations are accepted by the parser so we can
 * give a meaningful message from C code.
 *
 * TriggerTransition -
 *	   过渡行或表命名子句的表示
 *
 * 语法上初始只支持过渡表，且只限于 AFTER 觧发器，
 * 但解析器接受其他排列，以便我们可以从 C 代码给出有意义的错误信息。
 */
typedef struct TriggerTransition
{
  NodeTag type;
  char *name;   /* name of the transition table/row */
                /* 过渡表/行的名称 */
  bool isNew;   /* NEW or OLD? */
                /* 是 NEW 还是 OLD？ */
  bool isTable; /* Table or row? */
                /* 是表还是行？ */
} TriggerTransition;

/* Nodes for SQL/JSON support */
/* SQL/JSON 支持的节点 */

/*
 * JsonOutput -
 *		representation of JSON output clause (RETURNING type [FORMAT
 *format])
 *
 * JsonOutput -
 *		JSON 输出子句的表示（RETURNING type [FORMAT format]）
 */
typedef struct JsonOutput
{
  NodeTag type;
  TypeName *typeName;       /* RETURNING type name, if specified */
                            /* RETURNING 类型名，若已指定 */
  JsonReturning *returning; /* RETURNING FORMAT clause and type Oids */
                            /* RETURNING FORMAT 子句和类型 OID */
} JsonOutput;

/*
 * JsonArgument -
 *		representation of argument from JSON PASSING clause
 *
 * JsonArgument -
 *		JSON PASSING 子句中参数的表示
 */
typedef struct JsonArgument
{
  NodeTag type;
  JsonValueExpr *val; /* argument value expression */
                      /* 参数值表达式 */
  char *name;         /* argument name */
                      /* 参数名称 */
} JsonArgument;

/*
 * JsonQuotes -
 *		representation of [KEEP|OMIT] QUOTES clause for JSON_QUERY()
 *
 * JsonQuotes -
 *		JSON_QUERY() 的 [KEEP|OMIT] QUOTES 子句表示
 */
typedef enum JsonQuotes
{
  JS_QUOTES_UNSPEC, /* unspecified */
  JS_QUOTES_KEEP,   /* KEEP QUOTES */
  JS_QUOTES_OMIT,   /* OMIT QUOTES */
} JsonQuotes;

/*
 * JsonFuncExpr -
 *		untransformed representation of function expressions for
 *		SQL/JSON query functions
 *
 * JsonFuncExpr -
 *		SQL/JSON 查询函数的函数表达式的未转换表示
 */
typedef struct JsonFuncExpr
{
  NodeTag type;
  JsonExprOp op;               /* expression type */
                               /* 表达式类型 */
  char *column_name;           /* JSON_TABLE() column name or NULL if this is
                                * not for a JSON_TABLE() */
                               /* JSON_TABLE() 列名，如果不是用于 JSON_TABLE() 则为 NULL */
  JsonValueExpr *context_item; /* context item expression */
                               /* 上下文项表达式 */
  Node *pathspec;              /* JSON path specification expression */
                               /* JSON 路径规范表达式 */
  List *passing;               /* list of PASSING clause arguments, if any */
                               /* PASSING 子句参数列表，如果有 */
  JsonOutput *output;          /* output clause, if specified */
                               /* 输出子句，若已指定 */
  JsonBehavior *on_empty;      /* ON EMPTY behavior */
                               /* ON EMPTY 行为 */
  JsonBehavior *on_error;      /* ON ERROR behavior */
                               /* ON ERROR 行为 */
  JsonWrapper wrapper;         /* array wrapper behavior (JSON_QUERY only) */
                               /* 数组包装器行为（仅限 JSON_QUERY） */
  JsonQuotes quotes;           /* omit or keep quotes? (JSON_QUERY only) */
                               /* 省略还是保留引号？（仅限 JSON_QUERY） */
  ParseLoc location;           /* token location, or -1 if unknown */
                               /* 标记位置，若未知则为 -1 */
} JsonFuncExpr;

/*
 * JsonTablePathSpec
 *		untransformed specification of JSON path expression with an
 *optional name
 *
 * JsonTablePathSpec
 *		带可选名称的 JSON 路径表达式的未转换规范
 */
typedef struct JsonTablePathSpec
{
  NodeTag type;

  Node *string;           /* JSON path expression string */
                          /* JSON 路径表达式字符串 */
  char *name;             /* optional name for the path */
                          /* 路径的可选名称 */
  ParseLoc name_location; /* location of 'name' */
                          /* 'name' 的位置 */
  ParseLoc location;      /* location of 'string' */
                          /* 'string' 的位置 */
} JsonTablePathSpec;

/*
 * JsonTable -
 *		untransformed representation of JSON_TABLE
 *
 * JsonTable -
 *		JSON_TABLE 的未转换表示
 */
typedef struct JsonTable
{
  NodeTag type;
  JsonValueExpr *context_item; /* context item expression */
                               /* 上下文项表达式 */
  JsonTablePathSpec *pathspec; /* JSON path specification */
                               /* JSON 路径规范 */
  List *passing;               /* list of PASSING clause arguments, if any */
                               /* PASSING 子句参数列表，如果有 */
  List *columns;               /* list of JsonTableColumn */
                               /* JsonTableColumn 列表 */
  JsonBehavior *on_error;      /* ON ERROR behavior */
                               /* ON ERROR 行为 */
  Alias *alias;                /* table alias in FROM clause */
                               /* FROM 子句中的表别名 */
  bool lateral;                /* does it have LATERAL prefix? */
                               /* 它是否有 LATERAL 前缀？ */
  ParseLoc location;           /* token location, or -1 if unknown */
                               /* 标记位置，若未知则为 -1 */
} JsonTable;

/*
 * JsonTableColumnType -
 *		enumeration of JSON_TABLE column types
 *
 * JsonTableColumnType -
 *		JSON_TABLE 列类型的枚举
 */
typedef enum JsonTableColumnType
{
  JTC_FOR_ORDINALITY,
  JTC_REGULAR,
  JTC_EXISTS,
  JTC_FORMATTED,
  JTC_NESTED,
} JsonTableColumnType;

/*
 * JsonTableColumn -
 *		untransformed representation of JSON_TABLE column
 *
 * JsonTableColumn -
 *		JSON_TABLE 列的未转换表示
 */
typedef struct JsonTableColumn
{
  NodeTag type;
  JsonTableColumnType coltype; /* column type */
                               /* 列类型 */
  char *name;                  /* column name */
                               /* 列名 */
  TypeName *typeName;          /* column type name */
                               /* 列类型名称 */
  JsonTablePathSpec *pathspec; /* JSON path specification */
                               /* JSON 路径规范 */
  JsonFormat *format;          /* JSON format clause, if specified */
                               /* JSON 格式子句，若已指定 */
  JsonWrapper wrapper;         /* WRAPPER behavior for formatted columns */
                               /* 格式化列的 WRAPPER 行为 */
  JsonQuotes quotes;           /* omit or keep quotes on scalar strings? */
                               /* 在标量字符串上省略还是保留引号？ */
  List *columns;               /* nested columns */
                               /* 嵌套列 */
  JsonBehavior *on_empty;      /* ON EMPTY behavior */
                               /* ON EMPTY 行为 */
  JsonBehavior *on_error;      /* ON ERROR behavior */
                               /* ON ERROR 行为 */
  ParseLoc location;           /* token location, or -1 if unknown */
                               /* 标记位置，若未知则为 -1 */
} JsonTableColumn;

/*
 * JsonKeyValue -
 *		untransformed representation of JSON object key-value pair for
 *		JSON_OBJECT() and JSON_OBJECTAGG()
 *
 * JsonKeyValue -
 *		JSON_OBJECT() 和 JSON_OBJECTAGG() 的 JSON 对象键値对的未转换表示
 */
typedef struct JsonKeyValue
{
  NodeTag type;
  Expr *key;            /* key expression */
                        /* 键表达式 */
  JsonValueExpr *value; /* JSON value expression */
                        /* JSON 值表达式 */
} JsonKeyValue;

/*
 * JsonParseExpr -
 *		untransformed representation of JSON()
 *
 * JsonParseExpr -
 *		JSON() 的未转换表示
 */
typedef struct JsonParseExpr
{
  NodeTag type;
  JsonValueExpr *expr; /* string expression */
                       /* 字符串表达式 */
  JsonOutput *output;  /* RETURNING clause, if specified */
                       /* RETURNING 子句，若已指定 */
  bool unique_keys;    /* WITH UNIQUE KEYS? */
                       /* 是否带有 UNIQUE KEYS？ */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} JsonParseExpr;

/*
 * JsonScalarExpr -
 *		untransformed representation of JSON_SCALAR()
 *
 * JsonScalarExpr -
 *		JSON_SCALAR() 的未转换表示
 */
typedef struct JsonScalarExpr
{
  NodeTag type;
  Expr *expr;         /* scalar expression */
                      /* 标量表达式 */
  JsonOutput *output; /* RETURNING clause, if specified */
                      /* RETURNING 子句，若已指定 */
  ParseLoc location;  /* token location, or -1 if unknown */
                      /* 标记位置，若未知则为 -1 */
} JsonScalarExpr;

/*
 * JsonSerializeExpr -
 *		untransformed representation of JSON_SERIALIZE() function
 *
 * JsonSerializeExpr -
 *		JSON_SERIALIZE() 函数的未转换表示
 */
typedef struct JsonSerializeExpr
{
  NodeTag type;
  JsonValueExpr *expr; /* json value expression */
                       /* JSON 值表达式 */
  JsonOutput *output;  /* RETURNING clause, if specified  */
                       /* RETURNING 子句，若已指定 */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} JsonSerializeExpr;

/*
 * JsonObjectConstructor -
 *		untransformed representation of JSON_OBJECT() constructor
 *
 * JsonObjectConstructor -
 *		JSON_OBJECT() 构造函数的未转换表示
 */
typedef struct JsonObjectConstructor
{
  NodeTag type;
  List *exprs;         /* list of JsonKeyValue pairs */
                       /* JsonKeyValue 对列表 */
  JsonOutput *output;  /* RETURNING clause, if specified  */
                       /* RETURNING 子句，若已指定 */
  bool absent_on_null; /* skip NULL values? */
                       /* 是否跳过 NULL 值？ */
  bool unique;         /* check key uniqueness? */
                       /* 是否检查键唯一性？ */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} JsonObjectConstructor;

/*
 * JsonArrayConstructor -
 *		untransformed representation of JSON_ARRAY(element,...)
 *constructor
 *
 * JsonArrayConstructor -
 *		JSON_ARRAY(element,...) 构造函数的未转换表示
 */
typedef struct JsonArrayConstructor
{
  NodeTag type;
  List *exprs;         /* list of JsonValueExpr elements */
                       /* JsonValueExpr 元素列表 */
  JsonOutput *output;  /* RETURNING clause, if specified  */
                       /* RETURNING 子句，若已指定 */
  bool absent_on_null; /* skip NULL elements? */
                       /* 是否跳过 NULL 元素？ */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} JsonArrayConstructor;

/*
 * JsonArrayQueryConstructor -
 *		untransformed representation of JSON_ARRAY(subquery) constructor
 *
 * JsonArrayQueryConstructor -
 *		JSON_ARRAY(子查询) 构造函数的未转换表示
 */
typedef struct JsonArrayQueryConstructor
{
  NodeTag type;
  Node *query;         /* subquery */
                       /* 子查询 */
  JsonOutput *output;  /* RETURNING clause, if specified  */
                       /* RETURNING 子句，若已指定 */
  JsonFormat *format;  /* FORMAT clause for subquery, if specified */
                       /* 子查询的 FORMAT 子句，若已指定 */
  bool absent_on_null; /* skip NULL elements? */
                       /* 是否跳过 NULL 元素？ */
  ParseLoc location;   /* token location, or -1 if unknown */
                       /* 标记位置，若未知则为 -1 */
} JsonArrayQueryConstructor;

/*
 * JsonAggConstructor -
 *		common fields of untransformed representation of
 *		JSON_ARRAYAGG() and JSON_OBJECTAGG()
 *
 * JsonAggConstructor -
 *		JSON_ARRAYAGG() 和 JSON_OBJECTAGG() 未转换表示的公共字段
 */
typedef struct JsonAggConstructor
{
  NodeTag type;
  JsonOutput *output;     /* RETURNING clause, if any */
                          /* RETURNING 子句，如果有 */
  Node *agg_filter;       /* FILTER clause, if any */
                          /* FILTER 子句，如果有 */
  List *agg_order;        /* ORDER BY clause, if any */
                          /* ORDER BY 子句，如果有 */
  struct WindowDef *over; /* OVER clause, if any */
                          /* OVER 子句，如果有 */
  ParseLoc location;      /* token location, or -1 if unknown */
                          /* 标记位置，若未知则为 -1 */
} JsonAggConstructor;

/*
 * JsonObjectAgg -
 *		untransformed representation of JSON_OBJECTAGG()
 *
 * JsonObjectAgg -
 *		JSON_OBJECTAGG() 的未转换表示
 */
typedef struct JsonObjectAgg
{
  NodeTag type;
  JsonAggConstructor *constructor; /* common fields */
                                   /* 公共字段 */
  JsonKeyValue *arg;               /* object key-value pair */
                                   /* 对象键值对 */
  bool absent_on_null;             /* skip NULL values? */
                                   /* 是否跳过 NULL 值？ */
  bool unique;                     /* check key uniqueness? */
                                   /* 是否检查键唯一性？ */
} JsonObjectAgg;

/*
 * JsonArrayAgg -
 *		untransformed representation of JSON_ARRAYAGG()
 *
 * JsonArrayAgg -
 *		JSON_ARRAYAGG() 的未转换表示
 */
typedef struct JsonArrayAgg
{
  NodeTag type;
  JsonAggConstructor *constructor; /* common fields */
                                   /* 公共字段 */
  JsonValueExpr *arg;              /* array element expression */
                                   /* 数组元素表达式 */
  bool absent_on_null;             /* skip NULL elements? */
                                   /* 是否跳过 NULL 元素？ */
} JsonArrayAgg;

/*****************************************************************************
 *		Raw Grammar Output Statements
 *
 *		原始语法输出语句
 *****************************************************************************/

/*
 *		RawStmt --- container for any one statement's raw parse tree
 *
 *		RawStmt --- 任何单个语句的原始解析树的容器
 *
 * Parse analysis converts a raw parse tree headed by a RawStmt node into
 * an analyzed statement headed by a Query node.  For optimizable statements,
 * the conversion is complex.  For utility statements, the parser usually just
 * transfers the raw parse tree (sans RawStmt) into the utilityStmt field of
 * the Query node, and all the useful work happens at execution time.
 *
 * 解析分析将以 RawStmt 节点为首的原始解析树转换为以 Query 节点为首的分析后的语句。
 * 对于可优化的语句，转换是复杂的。对于工具语句，解析器通常只是将原始解析树
 * （不含 RawStmt）移动到 Query 节点的 utilityStmt 字段中，
 * 所有的实际工作都在执行时发生。
 *
 * stmt_location/stmt_len identify the portion of the source text string
 * containing this raw statement (useful for multi-statement strings).
 *
 * stmt_location/stmt_len 标识源文本字符串中包含此原始语句的部分
 * （对于多语句字符串很有用）。
 *
 * This is irrelevant for query jumbling, as this is not used in parsed
 * queries.
 *
 * 这对于查询指纹计算（query jumbling）无关紧要，因为这不用于已解析的查询。
 */
typedef struct RawStmt
{
  pg_node_attr(no_query_jumble)

      NodeTag type;
  Node *stmt;             /* raw parse tree */
                          /* 原始解析树 */
  ParseLoc stmt_location; /* start location, or -1 if unknown */
                          /* 起始位置，若未知则为 -1 */
  ParseLoc stmt_len;      /* length in bytes; 0 means "rest of string" */
                          /* 字节长度；0 表示“字符串的剩余部分” */
} RawStmt;

/*****************************************************************************
 *		Optimizable Statements
 *
 *		可优化的语句
 *****************************************************************************/

/* ----------------------
 *		Insert Statement
 *
 * The source expression is represented by SelectStmt for both the
 * SELECT and VALUES cases.  If selectStmt is NULL, then the query
 * is INSERT ... DEFAULT VALUES.
 *
 *		插入语句
 *
 * 无论是 SELECT 还是 VALUES 情况，源表达式都由 SelectStmt 表示。
 * 如果 selectStmt 为 NULL，则查询为 INSERT ... DEFAULT VALUES。
 * ----------------------
 */
typedef struct InsertStmt
{
  NodeTag type;
  RangeVar *relation; /* relation to insert into */
                      /* 要插入的关系 */
  List *cols;         /* optional: names of the target columns */
                      /* 可选：目标列的名称 */
  Node *selectStmt;   /* the source SELECT/VALUES, or NULL */
                      /* 源 SELECT/VALUES，或 NULL */
  OnConflictClause *onConflictClause; /* ON CONFLICT clause */
                                      /* ON CONFLICT 子句 */
  ReturningClause *returningClause;   /* RETURNING clause */
                                      /* RETURNING 子句 */
  WithClause *withClause;             /* WITH clause */
                                      /* WITH 子句 */
  OverridingKind override;            /* OVERRIDING clause */
                                      /* OVERRIDING 子句 */
} InsertStmt;

/* ----------------------
 *		Delete Statement
 *
 *		删除语句
 * ----------------------
 */
typedef struct DeleteStmt
{
  NodeTag type;
  RangeVar *relation;               /* relation to delete from */
                                    /* 要删除的关系 */
  List *usingClause;                /* optional using clause for more tables */
                                    /* 用于更多表的可选 using 子句 */
  Node *whereClause;                /* qualifications */
                                    /* 限定条件 */
  ReturningClause *returningClause; /* RETURNING clause */
                                    /* RETURNING 子句 */
  WithClause *withClause;           /* WITH clause */
                                    /* WITH 子句 */
} DeleteStmt;

/* ----------------------
 *		Update Statement
 *
 *		更新语句
 * ----------------------
 */
typedef struct UpdateStmt
{
  NodeTag type;
  RangeVar *relation;               /* relation to update */
                                    /* 要更新的关系 */
  List *targetList;                 /* the target list (of ResTarget) */
                                    /* 目标列表（ResTarget 列表） */
  Node *whereClause;                /* qualifications */
                                    /* 限定条件 */
  List *fromClause;                 /* optional from clause for more tables */
                                    /* 用于更多表的可选 from 子句 */
  ReturningClause *returningClause; /* RETURNING clause */
                                    /* RETURNING 子句 */
  WithClause *withClause;           /* WITH clause */
                                    /* WITH 子句 */
} UpdateStmt;

/* ----------------------
 *		Merge Statement
 *
 *		合并语句
 * ----------------------
 */
typedef struct MergeStmt
{
  NodeTag type;
  RangeVar *relation;     /* target relation to merge into */
                          /* 要合并到的目标关系 */
  Node *sourceRelation;   /* source relation */
                          /* 源关系 */
  Node *joinCondition;    /* join condition between source and target */
                          /* 源和目标之间的连接条件 */
  List *mergeWhenClauses; /* list of MergeWhenClause(es) */
                          /* MergeWhenClause 列表 */
  ReturningClause *returningClause; /* RETURNING clause */
                                    /* RETURNING 子句 */
  WithClause *withClause;           /* WITH clause */
                                    /* WITH 子句 */
} MergeStmt;

/* ----------------------
 *		Select Statement
 *
 * A "simple" SELECT is represented in the output of gram.y by a single
 * SelectStmt node; so is a VALUES construct.  A query containing set
 * operators (UNION, INTERSECT, EXCEPT) is represented by a tree of SelectStmt
 * nodes, in which the leaf nodes are component SELECTs and the internal nodes
 * represent UNION, INTERSECT, or EXCEPT operators.  Using the same node
 * type for both leaf and internal nodes allows gram.y to stick ORDER BY,
 * LIMIT, etc, clause values into a SELECT statement without worrying
 * whether it is a simple or compound SELECT.
 *
 *		查询语句
 *
 * 一个“简单的” SELECT 在 gram.y 的输出中由一个 SelectStmt 节点表示；
 * VALUES 结构也是如此。包含集合操作符（UNION、INTERSECT、EXCEPT）的查询
 * 由 SelectStmt 节点树表示，其中叶子节点是各种 SELECT，内部节点
 * 表示 UNION、INTERSECT 或 EXCEPT 操作符。为叶子节点和内部节点使用相同的
 * 节点类型，使得 gram.y 可以将 ORDER BY、LIMIT 等子句值粘合到 SELECT 语句中，
 * 而不必担心它是简单查询还是复合查询。
 * ----------------------
 */
typedef enum SetOperation
{
  SETOP_NONE = 0,
  SETOP_UNION,
  SETOP_INTERSECT,
  SETOP_EXCEPT,
} SetOperation;

typedef struct SelectStmt
{
  NodeTag type;

  /*
   * These fields are used only in "leaf" SelectStmts.
   *
   * 这些字段仅用于“叶子” SelectStmts。
   */
  List *distinctClause;   /* NULL, list of DISTINCT ON exprs, or
                           * lcons(NIL,NIL) for all (SELECT DISTINCT) */
                          /* NULL，DISTINCT ON 表达式列表，或者
                           * 表示所有的 (SELECT DISTINCT) 的 lcons(NIL,NIL) */
  IntoClause *intoClause; /* target for SELECT INTO */
                          /* SELECT INTO 的目标 */
  List *targetList;       /* the target list (of ResTarget) */
                          /* 目标列表（ResTarget 列表） */
  List *fromClause;       /* the FROM clause */
                          /* FROM 子句 */
  Node *whereClause;      /* WHERE qualification */
                          /* WHERE 限定条件 */
  List *groupClause;      /* GROUP BY clauses */
                          /* GROUP BY 子句 */
  bool groupDistinct;     /* Is this GROUP BY DISTINCT? */
                          /* 是否为 GROUP BY DISTINCT？ */
  Node *havingClause;     /* HAVING conditional-expression */
                          /* HAVING 条件表达式 */
  List *windowClause;     /* WINDOW window_name AS (...), ... */
                          /* WINDOW 子句 */

  /*
   * In a "leaf" node representing a VALUES list, the above fields are all
   * null, and instead this field is set.  Note that the elements of the
   * sublists are just expressions, without ResTarget decoration. Also note
   * that a list element can be DEFAULT (represented as a SetToDefault
   * node), regardless of the context of the VALUES list. It's up to parse
   * analysis to reject that where not valid.
   *
   * 在表示 VALUES 列表的“叶子”节点中，上述字段均为 null，
   * 而是设置此字段。请注意，子列表的元素只是表达式，没有 ResTarget 修饰。
   * 另请注意，列表元素可以是 DEFAULT（表示为 SetToDefault 节点），
   * 无论 VALUES 列表的上下文如何。由解析分析决定在何处拒绝无效的用法。
   */
  List *valuesLists; /* untransformed list of expression lists */
                     /* 未转换的表达式列表的列表 */

  /*
   * These fields are used in both "leaf" SelectStmts and upper-level
   * SelectStmts.
   *
   * 这些字段在“叶子” SelectStmt 和上层 SelectStmt 中都有使用。
   */
  List *sortClause;        /* sort clause (a list of SortBy's) */
                           /* 排序子句（SortBy 列表） */
  Node *limitOffset;       /* # of result tuples to skip */
                           /* 要跳过的结果元组数量 */
  Node *limitCount;        /* # of result tuples to return */
                           /* 要返回的结果元组数量 */
  LimitOption limitOption; /* limit type */
                           /* limit 类型 */
  List *lockingClause;     /* FOR UPDATE (list of LockingClause's) */
                           /* FOR UPDATE（LockingClause 列表） */
  WithClause *withClause;  /* WITH clause */
                           /* WITH 子句 */

  /*
   * These fields are used only in upper-level SelectStmts.
   *
   * 这些字段仅用于上层 SelectStmt。
   */
  SetOperation op;         /* type of set op */
                           /* 集合操作类型 */
  bool all;                /* ALL specified? */
                           /* 是否指定了 ALL？ */
  struct SelectStmt *larg; /* left child */
                           /* 左子节点 */
  struct SelectStmt *rarg; /* right child */
                           /* 右子节点 */
  /* Eventually add fields for CORRESPONDING spec here */
  /* 最终在这里添加 CORRESPONDING 规范的字段 */
} SelectStmt;

/* ----------------------
 *		Set Operation node for post-analysis query trees
 *
 * After parse analysis, a SELECT with set operations is represented by a
 * top-level Query node containing the leaf SELECTs as subqueries in its
 * range table.  Its setOperations field shows the tree of set operations,
 * with leaf SelectStmt nodes replaced by RangeTblRef nodes, and internal
 * nodes replaced by SetOperationStmt nodes.  Information about the output
 * column types is added, too.  (Note that the child nodes do not necessarily
 * produce these types directly, but we've checked that their output types
 * can be coerced to the output column type.)  Also, if it's not UNION ALL,
 * information about the types' sort/group semantics is provided in the form
 * of a SortGroupClause list (same representation as, eg, DISTINCT).
 * The resolved common column collations are provided too; but note that if
 * it's not UNION ALL, it's okay for a column to not have a common collation,
 * so a member of the colCollations list could be InvalidOid even though the
 * column has a collatable type.
 * ----------------------
 */
typedef struct SetOperationStmt
{
  NodeTag type;
  SetOperation op; /* type of set op */
                   /* 集合操作类型 */
  bool all;        /* ALL specified? */
                   /* 是否指定了 ALL？ */
  Node *larg;      /* left child */
                   /* 左子节点 */
  Node *rarg;      /* right child */
                   /* 右子节点 */
  /* Eventually add fields for CORRESPONDING spec here */
  /* 最终在这里添加 CORRESPONDING 规范的字段 */

  /* Fields derived during parse analysis (irrelevant for query jumbling): */
  /* OID list of output column type OIDs */
  List *colTypes pg_node_attr(query_jumble_ignore);
  /* integer list of output column typmods */
  List *colTypmods pg_node_attr(query_jumble_ignore);
  /* OID list of output column collation OIDs */
  List *colCollations pg_node_attr(query_jumble_ignore);
  /* a list of SortGroupClause's */
  List *groupClauses pg_node_attr(query_jumble_ignore);
  /* groupClauses is NIL if UNION ALL, but must be set otherwise */
} SetOperationStmt;

/*
 * RETURN statement (inside SQL function body)
 */
typedef struct ReturnStmt
{
  NodeTag type;
  Node *returnval; /* returned expression, or NULL */
                   /* 返回的表达式，或为 NULL */
} ReturnStmt;

/* ----------------------
 *		PL/pgSQL Assignment Statement
 *
 * Like SelectStmt, this is transformed into a SELECT Query.
 * However, the targetlist of the result looks more like an UPDATE.
 * ----------------------
 */
typedef struct PLAssignStmt
{
  NodeTag type;

  char *name;        /* initial column name */
                     /* 初始列名 */
  List *indirection; /* subscripts and field names, if any */
                     /* 下标和字段名，如果有 */
  int nnames;        /* number of names to use in ColumnRef */
                     /* ColumnRef 中使用的名称数量 */
  SelectStmt *val;   /* the PL/pgSQL expression to assign */
                     /* 要赋值的 PL/pgSQL 表达式 */
  ParseLoc location; /* name's token location, or -1 if unknown */
                     /* 名称标记的位置，若未知则为 -1 */
} PLAssignStmt;

/*****************************************************************************
 *		Other Statements (no optimizations required)
 *
 *		These are not touched by parser/analyze.c except to put them
 *into the utilityStmt field of a Query.  This is eventually passed to
 *		ProcessUtility (by-passing rewriting and planning).  Some of the
 *		statements do need attention from parse analysis, and this is
 *		done by routines in parser/parse_utilcmd.c after ProcessUtility
 *		receives the command for execution.
 *		DECLARE CURSOR, EXPLAIN, and CREATE TABLE AS are special cases:
 *		they contain optimizable statements, which get processed
 *normally by parser/analyze.c.
 *****************************************************************************/

/*
 * When a command can act on several kinds of objects with only one
 * parse structure required, use these constants to designate the
 * object type.  Note that commands typically don't support all the types.
 */

typedef enum ObjectType
{
  OBJECT_ACCESS_METHOD,
  OBJECT_AGGREGATE,
  OBJECT_AMOP,
  OBJECT_AMPROC,
  OBJECT_ATTRIBUTE, /* type's attribute, when distinct from column */
  OBJECT_CAST,
  OBJECT_COLUMN,
  OBJECT_COLLATION,
  OBJECT_CONVERSION,
  OBJECT_DATABASE,
  OBJECT_DEFAULT,
  OBJECT_DEFACL,
  OBJECT_DOMAIN,
  OBJECT_DOMCONSTRAINT,
  OBJECT_EVENT_TRIGGER,
  OBJECT_EXTENSION,
  OBJECT_FDW,
  OBJECT_FOREIGN_SERVER,
  OBJECT_FOREIGN_TABLE,
  OBJECT_FUNCTION,
  OBJECT_INDEX,
  OBJECT_LANGUAGE,
  OBJECT_LARGEOBJECT,
  OBJECT_MATVIEW,
  OBJECT_OPCLASS,
  OBJECT_OPERATOR,
  OBJECT_OPFAMILY,
  OBJECT_PARAMETER_ACL,
  OBJECT_POLICY,
  OBJECT_PROCEDURE,
  OBJECT_PUBLICATION,
  OBJECT_PUBLICATION_NAMESPACE,
  OBJECT_PUBLICATION_REL,
  OBJECT_ROLE,
  OBJECT_ROUTINE,
  OBJECT_RULE,
  OBJECT_SCHEMA,
  OBJECT_SEQUENCE,
  OBJECT_SUBSCRIPTION,
  OBJECT_STATISTIC_EXT,
  OBJECT_TABCONSTRAINT,
  OBJECT_TABLE,
  OBJECT_TABLESPACE,
  OBJECT_TRANSFORM,
  OBJECT_TRIGGER,
  OBJECT_TSCONFIGURATION,
  OBJECT_TSDICTIONARY,
  OBJECT_TSPARSER,
  OBJECT_TSTEMPLATE,
  OBJECT_TYPE,
  OBJECT_USER_MAPPING,
  OBJECT_VIEW,
} ObjectType;

/* ----------------------
 *		Create Schema Statement
 *
 * NOTE: the schemaElts list contains raw parsetrees for component statements
 * of the schema, such as CREATE TABLE, GRANT, etc.  These are analyzed and
 * executed after the schema itself is created.
 * ----------------------
 */
typedef struct CreateSchemaStmt
{
  NodeTag type;
  char *schemaname;   /* the name of the schema to create */
                      /* 要创建的模式名称 */
  RoleSpec *authrole; /* the owner of the created schema */
                      /* 所创建模式的所有者 */
  List *schemaElts;   /* schema components (list of parsenodes) */
                      /* 模式组件（解析节点列表） */
  bool if_not_exists; /* just do nothing if schema already exists? */
                      /* 如果模式已存在，是否不执行任何操作？ */
} CreateSchemaStmt;

typedef enum DropBehavior
{
  DROP_RESTRICT, /* drop fails if any dependent objects */
  DROP_CASCADE,  /* remove dependent objects too */
} DropBehavior;

/* ----------------------
 *	Alter Table
 * ----------------------
 */
typedef struct AlterTableStmt
{
  NodeTag type;
  RangeVar *relation; /* table to work on */
                      /* 要操作的表 */
  List *cmds;         /* list of subcommands */
                      /* 子命令列表 */
  ObjectType objtype; /* type of object */
                      /* 对象类型 */
  bool missing_ok;    /* skip error if table missing */
                      /* 若表不存在，跳过错误 */
} AlterTableStmt;

typedef enum AlterTableType
{
  AT_AddColumn,                 /* add column */
  AT_AddColumnToView,           /* implicitly via CREATE OR REPLACE VIEW */
  AT_ColumnDefault,             /* alter column default */
  AT_CookedColumnDefault,       /* add a pre-cooked column default */
  AT_DropNotNull,               /* alter column drop not null */
  AT_SetNotNull,                /* alter column set not null */
  AT_SetExpression,             /* alter column set expression */
  AT_DropExpression,            /* alter column drop expression */
  AT_SetStatistics,             /* alter column set statistics */
  AT_SetOptions,                /* alter column set ( options ) */
  AT_ResetOptions,              /* alter column reset ( options ) */
  AT_SetStorage,                /* alter column set storage */
  AT_SetCompression,            /* alter column set compression */
  AT_DropColumn,                /* drop column */
  AT_AddIndex,                  /* add index */
  AT_ReAddIndex,                /* internal to commands/tablecmds.c */
  AT_AddConstraint,             /* add constraint */
  AT_ReAddConstraint,           /* internal to commands/tablecmds.c */
  AT_ReAddDomainConstraint,     /* internal to commands/tablecmds.c */
  AT_AlterConstraint,           /* alter constraint */
  AT_ValidateConstraint,        /* validate constraint */
  AT_AddIndexConstraint,        /* add constraint using existing index */
  AT_DropConstraint,            /* drop constraint */
  AT_ReAddComment,              /* internal to commands/tablecmds.c */
  AT_AlterColumnType,           /* alter column type */
  AT_AlterColumnGenericOptions, /* alter column OPTIONS (...) */
  AT_ChangeOwner,               /* change owner */
  AT_ClusterOn,                 /* CLUSTER ON */
  AT_DropCluster,               /* SET WITHOUT CLUSTER */
  AT_SetLogged,                 /* SET LOGGED */
  AT_SetUnLogged,               /* SET UNLOGGED */
  AT_DropOids,                  /* SET WITHOUT OIDS */
  AT_SetAccessMethod,           /* SET ACCESS METHOD */
  AT_SetTableSpace,             /* SET TABLESPACE */
  AT_SetRelOptions,             /* SET (...) -- AM specific parameters */
  AT_ResetRelOptions,           /* RESET (...) -- AM specific parameters */
  AT_ReplaceRelOptions,         /* replace reloption list in its entirety */
  AT_EnableTrig,                /* ENABLE TRIGGER name */
  AT_EnableAlwaysTrig,          /* ENABLE ALWAYS TRIGGER name */
  AT_EnableReplicaTrig,         /* ENABLE REPLICA TRIGGER name */
  AT_DisableTrig,               /* DISABLE TRIGGER name */
  AT_EnableTrigAll,             /* ENABLE TRIGGER ALL */
  AT_DisableTrigAll,            /* DISABLE TRIGGER ALL */
  AT_EnableTrigUser,            /* ENABLE TRIGGER USER */
  AT_DisableTrigUser,           /* DISABLE TRIGGER USER */
  AT_EnableRule,                /* ENABLE RULE name */
  AT_EnableAlwaysRule,          /* ENABLE ALWAYS RULE name */
  AT_EnableReplicaRule,         /* ENABLE REPLICA RULE name */
  AT_DisableRule,               /* DISABLE RULE name */
  AT_AddInherit,                /* INHERIT parent */
  AT_DropInherit,               /* NO INHERIT parent */
  AT_AddOf,                     /* OF <type_name> */
  AT_DropOf,                    /* NOT OF */
  AT_ReplicaIdentity,           /* REPLICA IDENTITY */
  AT_EnableRowSecurity,         /* ENABLE ROW SECURITY */
  AT_DisableRowSecurity,        /* DISABLE ROW SECURITY */
  AT_ForceRowSecurity,          /* FORCE ROW SECURITY */
  AT_NoForceRowSecurity,        /* NO FORCE ROW SECURITY */
  AT_GenericOptions,            /* OPTIONS (...) */
  AT_AttachPartition,           /* ATTACH PARTITION */
  AT_DetachPartition,           /* DETACH PARTITION */
  AT_DetachPartitionFinalize,   /* DETACH PARTITION FINALIZE */
  AT_AddIdentity,               /* ADD IDENTITY */
  AT_SetIdentity,               /* SET identity column options */
  AT_DropIdentity,              /* DROP IDENTITY */
  AT_ReAddStatistics,           /* internal to commands/tablecmds.c */
} AlterTableType;

typedef struct AlterTableCmd /* one subcommand of an ALTER TABLE */
{
  NodeTag type;
  AlterTableType subtype; /* Type of table alteration to apply */
                          /* 要应用的表修改类型 */
  char *name;             /* column, constraint, or trigger to act on,
                           * or tablespace, access method */
                          /* 要操作的列、约束或触发器，或者是表空间、访问方法 */
  int16 num;              /* attribute number for columns referenced by
                           * number */
                          /* 按编号引用的列属性编号 */
  RoleSpec *newowner;     /* new owner for the table */
                          /* 表的新所有者 */
  Node *def;              /* definition of new column, index,
                           * constraint, or parent table */
                          /* 新列、索引、约束或父表的定义 */
  DropBehavior behavior;  /* RESTRICT or CASCADE for DROP cases */
                          /* DROP 情况下的 RESTRICT 或 CASCADE */
  bool missing_ok;        /* skip error if missing? */
                          /* 如果不存在，是否跳过错误？ */
  bool recurse;           /* exec-time recursion */
                          /* 执行时递归 */
} AlterTableCmd;

/* Ad-hoc node for AT_AlterConstraint */
typedef struct ATAlterConstraint
{
  NodeTag type;
  char *conname;            /* Constraint name */
                            /* 约束名称 */
  bool alterEnforceability; /* changing enforceability properties? */
                            /* 是否更改强制执行属性？ */
  bool is_enforced;         /* ENFORCED? */
                            /* 是否 ENFORCED？ */
  bool alterDeferrability;  /* changing deferrability properties? */
                            /* 是否更改延迟性属性？ */
  bool deferrable;          /* DEFERRABLE? */
                            /* 是否 DEFERRABLE？ */
  bool initdeferred;        /* INITIALLY DEFERRED? */
                            /* 是否 INITIALLY DEFERRED？ */
  bool alterInheritability; /* changing inheritability properties */
                            /* 更改继承性属性 */
  bool noinherit;           /* INHERIT or NO INHERIT */
                            /* INHERIT 还是 NO INHERIT */
} ATAlterConstraint;

/* Ad-hoc node for AT_ReplicaIdentity */
typedef struct ReplicaIdentityStmt
{
  NodeTag type;
  char identity_type; /* type of replica identity */
                      /* 复制标识类型 */
  char *name;         /* index name, if identity_type is 'i' */
                      /* 如果 identity_type 为 'i', 则为索引名称 */
} ReplicaIdentityStmt;

/* ----------------------
 * Alter Collation
 * ----------------------
 */
typedef struct AlterCollationStmt
{
  NodeTag type;
  List *collname; /* collation name */
                  /* 排序规则名称 */
} AlterCollationStmt;

/* ----------------------
 *	Alter Domain
 *
 * The fields are used in different ways by the different variants of
 * this command.
 * ----------------------
 */
typedef struct AlterDomainStmt
{
  NodeTag type;
  char subtype;          /*------------
                          *	T = alter column default
                          *	N = alter column drop not null
                          *	O = alter column set not null
                          *	C = add constraint
                          *	X = drop constraint
                          *------------
                          */
  List *typeName;        /* domain to work on */
                         /* 要操作的域 */
  char *name;            /* column or constraint name to act on */
                         /* 要操作的列或约束名 */
  Node *def;             /* definition of default or constraint */
                         /* 默认值或约束的定义 */
  DropBehavior behavior; /* RESTRICT or CASCADE for DROP cases */
                         /* DROP 情况下的 RESTRICT 或 CASCADE */
  bool missing_ok;       /* skip error if missing? */
                         /* 如果不存在，是否跳过错误？ */
} AlterDomainStmt;

/* ----------------------
 *		Grant|Revoke Statement
 * ----------------------
 */
typedef enum GrantTargetType
{
  ACL_TARGET_OBJECT,        /* grant on specific named object(s) */
  ACL_TARGET_ALL_IN_SCHEMA, /* grant on all objects in given schema(s) */
  ACL_TARGET_DEFAULTS,      /* ALTER DEFAULT PRIVILEGES */
} GrantTargetType;

typedef struct GrantStmt
{
  NodeTag type;
  bool is_grant;            /* true = GRANT, false = REVOKE */
                            /* true 为 GRANT, false 为 REVOKE */
  GrantTargetType targtype; /* type of the grant target */
                            /* 授权目标的类型 */
  ObjectType objtype;       /* kind of object being operated on */
                            /* 被操作对象的种类 */
  List *objects;            /* list of RangeVar nodes, ObjectWithArgs
                             * nodes, or plain names (as String values) */
                            /* RangeVar 节点、ObjectWithArgs 节点或纯名称
                             * （String 值）的列表 */
  List *privileges;         /* list of AccessPriv nodes */
                            /* AccessPriv 节点列表 */
  /* privileges == NIL denotes ALL PRIVILEGES */
  /* privileges == NIL 表示所有权限（ALL PRIVILEGES） */
  List *grantees;    /* list of RoleSpec nodes */
                     /* RoleSpec 节点列表 */
  bool grant_option; /* grant or revoke grant option */
                     /* 授权或撤销授权选项 */
  RoleSpec *grantor; /* grantor role spec */
                     /* 授权者角色规范 */
  DropBehavior behavior; /* drop behavior (for REVOKE) */
                         /* 删除行为（对于 REVOKE） */
} GrantStmt;

/*
 * ObjectWithArgs represents a function/procedure/operator name plus parameter
 * identification.
 *
 * objargs includes only the types of the input parameters of the object.
 * In some contexts, that will be all we have, and it's enough to look up
 * objects according to the traditional Postgres rules (i.e., when only input
 * arguments matter).
 *
 * objfuncargs, if not NIL, carries the full specification of the parameter
 * list, including parameter mode annotations.
 *
 * Some grammar productions can set args_unspecified = true instead of
 * providing parameter info.  In this case, lookup will succeed only if
 * the object name is unique.  Note that otherwise, NIL parameter lists
 * mean zero arguments.
 */
typedef struct ObjectWithArgs
{
  NodeTag type;
  List *objname;         /* qualified name of function/operator */
                         /* 函数/操作符的限定名称 */
  List *objargs;         /* list of Typename nodes (input args only) */
                         /* Typename 节点列表（仅限输入参数） */
  List *objfuncargs;     /* list of FunctionParameter nodes */
                         /* FunctionParameter 节点列表 */
  bool args_unspecified; /* argument list was omitted? */
                         /* 参数列表是否被省略？ */
} ObjectWithArgs;

/*
 * An access privilege, with optional list of column names
 * priv_name == NULL denotes ALL PRIVILEGES (only used with a column list)
 * cols == NIL denotes "all columns"
 * Note that simple "ALL PRIVILEGES" is represented as a NIL list, not
 * an AccessPriv with both fields null.
 */
typedef struct AccessPriv
{
  NodeTag type;
  char *priv_name; /* string name of privilege */
                   /* 权限的字符串名称 */
  List *cols;      /* list of String */
                   /* 字符串列表 */
} AccessPriv;

/* ----------------------
 *		Grant/Revoke Role Statement
 *
 * Note: because of the parsing ambiguity with the GRANT <privileges>
 * statement, granted_roles is a list of AccessPriv; the execution code
 * should complain if any column lists appear.  grantee_roles is a list
 * of role names, as String values.
 * ----------------------
 */
typedef struct GrantRoleStmt
{
  NodeTag type;
  List *granted_roles;   /* list of roles to be granted/revoked */
                         /* 要授予/撤销的角色列表 */
  List *grantee_roles;   /* list of member roles to add/delete */
                         /* 要添加/删除的成员角色列表 */
  bool is_grant;         /* true = GRANT, false = REVOKE */
                         /* true 为 GRANT, false 为 REVOKE */
  List *opt;             /* options e.g. WITH GRANT OPTION */
                         /* 选项，例如 WITH GRANT OPTION */
  RoleSpec *grantor;     /* set grantor to other than current role */
                         /* 将授权者设置为当前角色以外的角色 */
  DropBehavior behavior; /* drop behavior (for REVOKE) */
                         /* 删除行为（对于 REVOKE） */
} GrantRoleStmt;

/* ----------------------
 *	Alter Default Privileges Statement
 * ----------------------
 */
typedef struct AlterDefaultPrivilegesStmt
{
  NodeTag type;
  List *options;     /* list of DefElem */
                     /* DefElem 列表 */
  GrantStmt *action; /* GRANT/REVOKE action (with objects=NIL) */
                     /* GRANT/REVOKE 动作（objects=NIL） */
} AlterDefaultPrivilegesStmt;

/* ----------------------
 *		Copy Statement
 *
 * We support "COPY relation FROM file", "COPY relation TO file", and
 * "COPY (query) TO file".  In any given CopyStmt, exactly one of "relation"
 * and "query" must be non-NULL.
 * ----------------------
 */
typedef struct CopyStmt
{
  NodeTag type;
  RangeVar *relation; /* the relation to copy */
                      /* 要复制的关系 */
  Node *query;        /* the query (SELECT or DML statement with
                       * RETURNING) to copy, as a raw parse tree */
                      /* 要复制的查询（带有 RETURNING 的 SELECT 或 DML 语句），
                       * 作为原始解析树 */
  List *attlist;      /* List of column names (as Strings), or NIL
                       * for all columns */
                      /* 列名列表（String 列表），或为 NIL 表示所有列 */
  bool is_from;       /* TO or FROM */
                      /* 是 TO 还是 FROM */
  bool is_program;    /* is 'filename' a program to popen? */
                      /* 'filename' 是否为一个要通过 popen 执行的程序？ */
  char *filename;     /* filename, or NULL for STDIN/STDOUT */
                      /* 文件名，或为 NULL 表示 STDIN/STDOUT */
  List *options;      /* List of DefElem nodes */
                      /* DefElem 节点列表 */
  Node *whereClause;  /* WHERE condition (or NULL) */
                      /* WHERE 条件（或 NULL） */
} CopyStmt;

/* ----------------------
 * SET Statement (includes RESET)
 *
 * "SET var TO DEFAULT" and "RESET var" are semantically equivalent, but we
 * preserve the distinction in VariableSetKind for CreateCommandTag().
 * ----------------------
 */
typedef enum VariableSetKind
{
  VAR_SET_VALUE,   /* SET var = value */
  VAR_SET_DEFAULT, /* SET var TO DEFAULT */
  VAR_SET_CURRENT, /* SET var FROM CURRENT */
  VAR_SET_MULTI,   /* special case for SET TRANSACTION ... */
  VAR_RESET,       /* RESET var */
  VAR_RESET_ALL,   /* RESET ALL */
} VariableSetKind;

typedef struct VariableSetStmt
{
  pg_node_attr(custom_query_jumble)

      NodeTag type;
  VariableSetKind kind; /* type of variable set */
                        /* 变量设置类型 */
  /* variable to be set */
  /* 要设置的变量 */
  char *name;
  /* List of A_Const nodes */
  /* A_Const 节点列表 */
  List *args;

  /*
   * True if arguments should be accounted for in query jumbling.  We use a
   * separate flag rather than query_jumble_ignore on "args" as several
   * grammar flavors of SET rely on a list of values that are parsed
   * directly from the grammar's keywords.
   *
   * 如果参数应在查询指纹中计算，则为 true。我们使用一个单独的标志，
   * 而不是在 "args" 上使用 query_jumble_ignore, 因为 SET 的几种语法变体
   * 依赖于直接从语法关键字解析的值列表。
   */
  bool jumble_args;
  /* SET LOCAL? */
  /* 是否为 SET LOCAL？ */
  bool is_local;
  /* token location, or -1 if unknown */
  /* 标记位置，若未知则为 -1 */
  ParseLoc location pg_node_attr(query_jumble_location);
} VariableSetStmt;

/* ----------------------
 * Show Statement
 * ----------------------
 */
typedef struct VariableShowStmt
{
  NodeTag type;
  char *name; /* variable to be shown */
              /* 要显示的变量 */
} VariableShowStmt;

/* ----------------------
 *		Create Table Statement
 *
 * NOTE: in the raw gram.y output, ColumnDef and Constraint nodes are
 * intermixed in tableElts, and constraints and nnconstraints are NIL.  After
 * parse analysis, tableElts contains just ColumnDefs, nnconstraints contains
 * Constraint nodes of CONSTR_NOTNULL type from various sources, and
 * constraints contains just CONSTR_CHECK Constraint nodes.
 * ----------------------
 */

typedef struct CreateStmt
{
  NodeTag type;
  RangeVar *relation;            /* relation to create */
                                 /* 要创建的关系 */
  List *tableElts;               /* column definitions (list of ColumnDef) */
                                 /* 列定义（ColumnDef 列表） */
  List *inhRelations;            /* relations to inherit from (list of
                                  * RangeVar) */
                                 /* 要继承的关系列表（RangeVar 列表） */
  PartitionBoundSpec *partbound; /* FOR VALUES clause */
                                 /* FOR VALUES 子句 */
  PartitionSpec *partspec;       /* PARTITION BY clause */
                                 /* PARTITION BY 子句 */
  TypeName *ofTypename;          /* OF typename */
                                 /* OF 别名（OF typename） */
  List *constraints;             /* constraints (list of Constraint nodes) */
                                 /* 约束（Constraint 节点列表） */
  List *nnconstraints;           /* NOT NULL constraints (ditto) */
                                 /* NOT NULL 约束（同上） */
  List *options;                 /* options from WITH clause */
                                 /* 来自 WITH 子句的选项 */
  OnCommitAction oncommit;       /* what do we do at COMMIT? */
                                 /* COMMIT 时该做什么？ */
  char *tablespacename;          /* table space to use, or NULL */
                                 /* 要使用的表空间，或 NULL */
  char *accessMethod;            /* table access method */
                                 /* 表访问方法 */
  bool if_not_exists;            /* just do nothing if it already exists? */
                                 /* 如果已存在，是否不执行任何操作？ */
} CreateStmt;

/* ----------
 * Definitions for constraints in CreateStmt
 *
 * Note that column defaults are treated as a type of constraint,
 * even though that's a bit odd semantically.
 *
 * For constraints that use expressions (CONSTR_CHECK, CONSTR_DEFAULT)
 * we may have the expression in either "raw" form (an untransformed
 * parse tree) or "cooked" form (the nodeToString representation of
 * an executable expression tree), depending on how this Constraint
 * node was created (by parsing, or by inheritance from an existing
 * relation).  We should never have both in the same node!
 *
 * FKCONSTR_ACTION_xxx values are stored into pg_constraint.confupdtype
 * and pg_constraint.confdeltype columns; FKCONSTR_MATCH_xxx values are
 * stored into pg_constraint.confmatchtype.  Changing the code values may
 * require an initdb!
 *
 * skip_validation implements the SQL NOT VALID/deferred-validation path:
 * existing table rows are not scanned when catalog entries are installed,
 * but the constraint is not marked valid unless initially_valid is true.
 * Fail-safe: callers that defer validation must leave initially_valid false
 * so ALTER TABLE VALIDATE CONSTRAINT remains the explicit validation step.
 * The only supported case with both true is a relation proven empty.
 *
 * Constraint attributes (DEFERRABLE etc) are initially represented as
 * separate Constraint nodes for simplicity of parsing.  parse_utilcmd.c makes
 * a pass through the constraints list to insert the info into the appropriate
 * Constraint node.
 * ----------
 */

typedef enum ConstrType /* types of constraints */
{ CONSTR_NULL,          /* not standard SQL, but a lot of people
                         * expect it */
  CONSTR_NOTNULL,
  CONSTR_DEFAULT,
  CONSTR_IDENTITY,
  CONSTR_GENERATED,
  CONSTR_CHECK,
  CONSTR_PRIMARY,
  CONSTR_UNIQUE,
  CONSTR_EXCLUSION,
  CONSTR_FOREIGN,
  CONSTR_ATTR_DEFERRABLE, /* attributes for previous constraint node */
  CONSTR_ATTR_NOT_DEFERRABLE,
  CONSTR_ATTR_DEFERRED,
  CONSTR_ATTR_IMMEDIATE,
  CONSTR_ATTR_ENFORCED,
  CONSTR_ATTR_NOT_ENFORCED,
} ConstrType;

/* Foreign key action codes */
#define FKCONSTR_ACTION_NOACTION 'a'
#define FKCONSTR_ACTION_RESTRICT 'r'
#define FKCONSTR_ACTION_CASCADE 'c'
#define FKCONSTR_ACTION_SETNULL 'n'
#define FKCONSTR_ACTION_SETDEFAULT 'd'

/* Foreign key matchtype codes */
#define FKCONSTR_MATCH_FULL 'f'
#define FKCONSTR_MATCH_PARTIAL 'p'
#define FKCONSTR_MATCH_SIMPLE 's'

typedef struct Constraint
{
  NodeTag type;
  ConstrType contype;        /* see above */
  char *conname;             /* Constraint name, or NULL if unnamed */
  bool deferrable;           /* DEFERRABLE? */
  bool initdeferred;         /* INITIALLY DEFERRED? */
  bool is_enforced;          /* enforced constraint? */
  bool skip_validation;      /* defer validation of pre-existing rows? */
  bool initially_valid;      /* mark the new constraint as valid? */
  bool is_no_inherit;        /* is constraint non-inheritable? */
  Node *raw_expr;            /* CHECK or DEFAULT expression, as
                              * untransformed parse tree */
  char *cooked_expr;         /* CHECK or DEFAULT expression, as
                              * nodeToString representation */
  char generated_when;       /* ALWAYS or BY DEFAULT */
  char generated_kind;       /* STORED or VIRTUAL */
  bool nulls_not_distinct;   /* null treatment for UNIQUE constraints */
  List *keys;                /* String nodes naming referenced key
                              * column(s); for UNIQUE/PK/NOT NULL */
  bool without_overlaps;     /* WITHOUT OVERLAPS specified */
  List *including;           /* String nodes naming referenced nonkey
                              * column(s); for UNIQUE/PK */
  List *exclusions;          /* list of (IndexElem, operator name) pairs;
                              * for exclusion constraints */
  List *options;             /* options from WITH clause */
                             /* 来自 WITH 子句的选项 */
  char *indexname;           /* existing index to use; otherwise NULL */
                             /* 要使用的现有索引名称；否则为 NULL */
  char *indexspace;          /* index tablespace; NULL for default */
                             /* 索引表空间；NULL 表示默认值 */
  bool reset_default_tblspc; /* reset default_tablespace prior to
                              * creating the index */
                             /* 在创建索引前重置 default_tablespace */
  char *access_method;       /* index access method; NULL for default */
                             /* 索引访问方法；NULL 表示默认值 */
  Node *where_clause;        /* partial index predicate */
                             /* 部分索引谓词 */

  /* Fields used for FOREIGN KEY constraints: */
  /* 用于外键约束的字段： */
  RangeVar *pktable;     /* Primary key table */
                         /* 主键表 */
  List *fk_attrs;        /* Attributes of foreign key */
                         /* 外键属性 */
  List *pk_attrs;        /* Corresponding attrs in PK table */
                         /* 主键表中的对应属性 */
  bool fk_with_period;   /* Last attribute of FK uses PERIOD */
                         /* 外键的最后一个属性使用 PERIOD */
  bool pk_with_period;   /* Last attribute of PK uses PERIOD */
                         /* 主键的最后一个属性使用 PERIOD */
  char fk_matchtype;     /* FULL, PARTIAL, SIMPLE */
                         /* 匹配类型：FULL, PARTIAL, SIMPLE */
  char fk_upd_action;    /* ON UPDATE action */
                         /* ON UPDATE 动作 */
  char fk_del_action;    /* ON DELETE action */
  List *fk_del_set_cols; /* ON DELETE SET NULL/DEFAULT (col1, col2) */
                         /* ON DELETE SET NULL/DEFAULT (col1, col2) 列表 */
  List *old_conpfeqop;   /* pg_constraint.conpfeqop of my former self */
                         /* 映射旧约束的相等性操作符 */
  Oid old_pktable_oid;   /* pg_constraint.confrelid of my former
                          * self */
                         /* 旧主键表的 OID */

  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} Constraint;

/* ----------------------
 *		Create/Drop Table Space Statements
 * ----------------------
 */

typedef struct CreateTableSpaceStmt
{
  NodeTag type;
  char *tablespacename; /* name of the tablespace */
                        /* 表空间名称 */
  RoleSpec *owner;      /* owner of the tablespace */
                        /* 表空间所有者 */
  char *location;       /* physical location of the tablespace */
                        /* 表空间的物理位置 */
  List *options;        /* generic options to tablespace */
                        /* 表空间的通用选项 */
} CreateTableSpaceStmt;

typedef struct DropTableSpaceStmt
{
  NodeTag type;
  char *tablespacename; /* name of the tablespace */
                        /* 表空间名称 */
  bool missing_ok;      /* skip error if missing? */
                        /* 如果缺失，是否跳过错误？ */
} DropTableSpaceStmt;

typedef struct AlterTableSpaceOptionsStmt
{
  NodeTag type;
  char *tablespacename; /* name of the tablespace */
                        /* 表空间名称 */
  List *options;        /* list of options to set/reset */
                        /* 设置/重置的选项列表 */
  bool isReset;         /* reset or set? */
                        /* 是重置还是设置？ */
} AlterTableSpaceOptionsStmt;

typedef struct AlterTableMoveAllStmt
{
  NodeTag type;
  char *orig_tablespacename; /* source tablespace */
                             /* 原始（源）表空间 */
  ObjectType objtype;        /* Object type to move */
                             /* 要移动的对象类型 */
  List *roles;               /* List of roles to move objects of */
                             /* 要移动其所属对象的角色列表 */
  char *new_tablespacename;  /* destination tablespace */
                             /* 新目标表空间 */
  bool nowait;               /* wait for locks? */
                             /* 是否等待锁？ */
} AlterTableMoveAllStmt;

/* ----------------------
 *		Create/Alter Extension Statements
 * ----------------------
 */

typedef struct CreateExtensionStmt
{
  NodeTag type;
  char *extname;
  bool if_not_exists; /* just do nothing if it already exists? */
  List *options;      /* List of DefElem nodes */
} CreateExtensionStmt;

/* Only used for ALTER EXTENSION UPDATE; later might need an action field */
typedef struct AlterExtensionStmt
{
  NodeTag type;
  char *extname;
  List *options; /* List of DefElem nodes */
} AlterExtensionStmt;

typedef struct AlterExtensionContentsStmt
{
  NodeTag type;
  char *extname;      /* Extension's name */
  int action;         /* +1 = add object, -1 = drop object */
  ObjectType objtype; /* Object's type */
  Node *object;       /* Qualified name of the object */
} AlterExtensionContentsStmt;

/* ----------------------
 *		Create/Alter FOREIGN DATA WRAPPER Statements
 * ----------------------
 */

typedef struct CreateFdwStmt
{
  NodeTag type;
  char *fdwname;      /* foreign-data wrapper name */
  List *func_options; /* HANDLER/VALIDATOR options */
  List *options;      /* generic options to FDW */
} CreateFdwStmt;

typedef struct AlterFdwStmt
{
  NodeTag type;
  char *fdwname;      /* foreign-data wrapper name */
  List *func_options; /* HANDLER/VALIDATOR options */
  List *options;      /* generic options to FDW */
} AlterFdwStmt;

/* ----------------------
 *		Create/Alter FOREIGN SERVER Statements
 * ----------------------
 */

typedef struct CreateForeignServerStmt
{
  NodeTag type;
  char *servername;   /* server name */
  char *servertype;   /* optional server type */
  char *version;      /* optional server version */
  char *fdwname;      /* FDW name */
  bool if_not_exists; /* just do nothing if it already exists? */
  List *options;      /* generic options to server */
} CreateForeignServerStmt;

typedef struct AlterForeignServerStmt
{
  NodeTag type;
  char *servername; /* server name */
  char *version;    /* optional server version */
  List *options;    /* generic options to server */
  bool has_version; /* version specified */
} AlterForeignServerStmt;

/* ----------------------
 *		Create FOREIGN TABLE Statement
 * ----------------------
 */

typedef struct CreateForeignTableStmt
{
  CreateStmt base;
  char *servername;
  List *options;
} CreateForeignTableStmt;

/* ----------------------
 *		Create/Drop USER MAPPING Statements
 * ----------------------
 */

typedef struct CreateUserMappingStmt
{
  NodeTag type;
  RoleSpec *user;     /* user role */
  char *servername;   /* server name */
  bool if_not_exists; /* just do nothing if it already exists? */
  List *options;      /* generic options to server */
} CreateUserMappingStmt;

typedef struct AlterUserMappingStmt
{
  NodeTag type;
  RoleSpec *user;   /* user role */
  char *servername; /* server name */
  List *options;    /* generic options to server */
} AlterUserMappingStmt;

typedef struct DropUserMappingStmt
{
  NodeTag type;
  RoleSpec *user;   /* user role */
  char *servername; /* server name */
  bool missing_ok;  /* ignore missing mappings */
} DropUserMappingStmt;

/* ----------------------
 *		Import Foreign Schema Statement
 * ----------------------
 */

typedef enum ImportForeignSchemaType
{
  FDW_IMPORT_SCHEMA_ALL,      /* all relations wanted */
  FDW_IMPORT_SCHEMA_LIMIT_TO, /* include only listed tables in import */
  FDW_IMPORT_SCHEMA_EXCEPT,   /* exclude listed tables from import */
} ImportForeignSchemaType;

typedef struct ImportForeignSchemaStmt
{
  NodeTag type;
  char *server_name;                 /* FDW server name */
                                     /* FDW 服务器名称 */
  char *remote_schema;               /* remote schema name to query */
                                     /* 要查询的远程模式名称 */
  char *local_schema;                /* local schema to create objects in */
                                     /* 要在其中创建对象的本地模式 */
  ImportForeignSchemaType list_type; /* type of table list */
                                     /* 表列表的类型 */
  List *table_list;                  /* List of RangeVar */
                                     /* RangeVar 列表 */
  List *options;                     /* list of options to pass to FDW */
                                     /* 传递给 FDW 的选项列表 */
} ImportForeignSchemaStmt;

/*----------------------
 *		Create POLICY Statement
 *----------------------
 */
typedef struct CreatePolicyStmt
{
  NodeTag type;
  char *policy_name; /* Policy's name */
                     /* 策略名称 */
  RangeVar *table;   /* the table name the policy applies to */
                     /* 策略应用的表名称 */
  char *cmd_name;    /* the command name the policy applies to */
                     /* 策略应用的操作（命令）名称 */
  bool permissive;   /* restrictive or permissive policy */
                     /* 限制性或许可性策略 */
  List *roles;       /* the roles associated with the policy */
                     /* 与策略关联的角色 */
  Node *qual;        /* the policy's condition */
                     /* 策略的条件 */
  Node *with_check;  /* the policy's WITH CHECK condition. */
                     /* 策略的 WITH CHECK 条件 */
} CreatePolicyStmt;

/*----------------------
 *		Alter POLICY Statement
 *----------------------
 */
typedef struct AlterPolicyStmt
{
  NodeTag type;
  char *policy_name; /* Policy's name */
                     /* 策略名称 */
  RangeVar *table;   /* the table name the policy applies to */
                     /* 策略应用到的表名 */
  List *roles;       /* the roles associated with the policy */
                     /* 与策略关联的角色 */
  Node *qual;        /* the policy's condition */
                     /* 策略的条件（qual） */
  Node *with_check;  /* the policy's WITH CHECK condition. */
                     /* 策略的 WITH CHECK 条件 */
} AlterPolicyStmt;

/*----------------------
 *		Create ACCESS METHOD Statement
 *----------------------
 */
typedef struct CreateAmStmt
{
  NodeTag type;
  char *amname;       /* access method name */
                      /* 访问方法名称 */
  List *handler_name; /* handler function name */
                      /* 处理函数名称 */
  char amtype;        /* type of access method */
                      /* 访问方法类型 */
} CreateAmStmt;

/* ----------------------
 *		Create TRIGGER Statement
 * ----------------------
 */
typedef struct CreateTrigStmt
{
  NodeTag type;
  bool replace;       /* replace trigger if already exists */
                      /* 如果触发器已存在则替换 */
  bool isconstraint;  /* This is a constraint trigger */
                      /* 这是一个约束触发器 */
  char *trigname;     /* TRIGGER's name */
                      /* 触发器名称 */
  RangeVar *relation; /* relation trigger is on */
                      /* 触发器所属的关系 */
  List *funcname;     /* qual. name of function to call */
                      /* 要调用的函数的限定名称 */
  List *args;         /* list of String or NIL */
                      /* 字符串列表或 NIL */
  bool row;           /* ROW/STATEMENT */
                      /* 行级/语句级 */
  /* timing uses the TRIGGER_TYPE bits defined in catalog/pg_trigger.h */
  /* timing 使用 catalog/pg_trigger.h 中定义的 TRIGGER_TYPE 位 */
  int16 timing; /* BEFORE, AFTER, or INSTEAD */
                /* BEFORE, AFTER 或 INSTEAD */
  /* events uses the TRIGGER_TYPE bits defined in catalog/pg_trigger.h */
  /* events 使用 catalog/pg_trigger.h 中定义的 TRIGGER_TYPE 位 */
  int16 events;     /* "OR" of INSERT/UPDATE/DELETE/TRUNCATE */
                    /* INSERT/UPDATE/DELETE/TRUNCATE 的“或”位掩码 */
  List *columns;    /* column names, or NIL for all columns */
                    /* 列名，或为 NIL 表示所有列 */
  Node *whenClause; /* qual expression, or NULL if none */
                    /* 限定表达式，若无则为 NULL */
  /* explicitly named transition data */
  /* 显式命名的过渡数据 */
  List *transitionRels; /* TriggerTransition nodes, or NIL if none */
                        /* TriggerTransition 节点列表，若无则为 NIL */
  /* The remaining fields are only used for constraint triggers */
  /* 其余字段仅用于约束触发器 */
  bool deferrable;     /* [NOT] DEFERRABLE */
                       /* [NOT] DEFERRABLE */
  bool initdeferred;   /* INITIALLY {DEFERRED|IMMEDIATE} */
                       /* INITIALLY {DEFERRED|IMMEDIATE} */
  RangeVar *constrrel; /* opposite relation, if RI trigger */
                       /* 相反的关系，如果是 RI 触发器 */
} CreateTrigStmt;

/* ----------------------
 *		Create EVENT TRIGGER Statement
 * ----------------------
 */
typedef struct CreateEventTrigStmt
{
  NodeTag type;
  char *trigname;   /* TRIGGER's name */
                    /* 触发器名称 */
  char *eventname;  /* event's identifier */
                    /* 事件标识符 */
  List *whenclause; /* list of DefElems indicating filtering */
                    /* 指示过滤的 DefElem 列表 */
  List *funcname;   /* qual. name of function to call */
                    /* 要调用的函数的限定名称 */
} CreateEventTrigStmt;

/* ----------------------
 *		Alter EVENT TRIGGER Statement
 * ----------------------
 */
typedef struct AlterEventTrigStmt
{
  NodeTag type;
  char *trigname; /* TRIGGER's name */
                  /* 触发器名称 */
  char tgenabled; /* trigger's firing configuration WRT
                   * session_replication_role */
                  /* 触发器相对于 session_replication_role 的启动配置 */
} AlterEventTrigStmt;

/* ----------------------
 *		Create LANGUAGE Statements
 * ----------------------
 */
typedef struct CreatePLangStmt
{
  NodeTag type;
  bool replace;      /* T => replace if already exists */
                     /* T => 如果已存在则替换 */
  char *plname;      /* PL name */
                     /* PL 名称 */
  List *plhandler;   /* PL call handler function (qual. name) */
                     /* PL 调用处理函数（限定名称） */
  List *plinline;    /* optional inline function (qual. name) */
                     /* 可选的内联函数（限定名称） */
  List *plvalidator; /* optional validator function (qual. name) */
                     /* 可选的验证函数（限定名称） */
  bool pltrusted;    /* PL is trusted */
                     /* PL 是可信的 */
} CreatePLangStmt;

/* ----------------------
 *	Create/Alter/Drop Role Statements
 *
 * Note: these node types are also used for the backwards-compatible
 * Create/Alter/Drop User/Group statements.  In the ALTER and DROP cases
 * there's really no need to distinguish what the original spelling was,
 * but for CREATE we mark the type because the defaults vary.
 * ----------------------
 */
typedef enum RoleStmtType
{
  ROLESTMT_ROLE,
  ROLESTMT_USER,
  ROLESTMT_GROUP,
} RoleStmtType;

typedef struct CreateRoleStmt
{
  NodeTag type;
  RoleStmtType stmt_type; /* ROLE/USER/GROUP */
                          /* ROLE/USER/GROUP 类型 */
  char *role;             /* role name */
                          /* 角色名称 */
  List *options;          /* List of DefElem nodes */
                          /* DefElem 节点列表 */
} CreateRoleStmt;

typedef struct AlterRoleStmt
{
  NodeTag type;
  RoleSpec *role; /* role */
                  /* 角色 */
  List *options;  /* List of DefElem nodes */
                  /* DefElem 节点列表 */
  int action;     /* +1 = add members, -1 = drop members */
                  /* +1 = 添加成员, -1 = 删除成员 */
} AlterRoleStmt;

typedef struct AlterRoleSetStmt
{
  NodeTag type;
  RoleSpec *role;           /* role */
                            /* 角色 */
  char *database;           /* database name, or NULL */
                            /* 数据库名称，或为 NULL */
  VariableSetStmt *setstmt; /* SET or RESET subcommand */
                            /* SET 或 RESET 子命令 */
} AlterRoleSetStmt;

typedef struct DropRoleStmt
{
  NodeTag type;
  List *roles;     /* List of roles to remove */
                   /* 要删除的角色列表 */
  bool missing_ok; /* skip error if a role is missing? */
                   /* 若角色缺失是否跳过错误？ */
} DropRoleStmt;

/* ----------------------
 *		{Create|Alter} SEQUENCE Statement
 * ----------------------
 */

typedef struct CreateSeqStmt
{
  NodeTag type;
  RangeVar *sequence; /* the sequence to create */
                      /* 要创建的序列 */
  List *options;      /* sequence options */
                      /* 序列选项 */
  Oid ownerId; /* ID of owner, or InvalidOid for default */
               /* 所有者 ID，或 InvalidOid 表示默认值 */
  bool for_identity;  /* sequence is for an identity column */
                      /* 序列是用于标识列的 */
  bool if_not_exists; /* just do nothing if it already exists? */
                      /* 如果已存在是否不执行操作？ */
} CreateSeqStmt;

typedef struct AlterSeqStmt
{
  NodeTag type;
  RangeVar *sequence; /* the sequence to alter */
                      /* 要修改的序列 */
  List *options;      /* sequence options */
                      /* 序列选项 */
  bool for_identity;  /* sequence is for an identity column */
                      /* 序列是用于标识列的 */
  bool missing_ok;    /* skip error if missing? */
                      /* 若缺失是否跳过错误？ */
} AlterSeqStmt;

/* ----------------------
 *		Create {Aggregate|Operator|Type} Statement
 * ----------------------
 */
typedef struct DefineStmt
{
  NodeTag type;
  ObjectType kind;    /* aggregate, operator, type */
                      /* 聚合、操作符、类型 */
  bool oldstyle;      /* hack to signal old CREATE AGG syntax */
                      /* 用于标记旧 CREATE AGG 语法的黑科技 */
  List *defnames;     /* qualified name (list of String) */
                      /* 限定名称（String 列表） */
  List *args;         /* a list of TypeName (if needed) */
                      /* TypeName 列表（如果需要） */
  List *definition;   /* a list of DefElem */
                      /* DefElem 列表 */
  bool if_not_exists; /* just do nothing if it already exists? */
                      /* 如果已存在是否不执行操作？ */
  bool replace;       /* replace if already exists? */
                      /* 如果已存在是否替换？ */
} DefineStmt;

/* ----------------------
 *		Create Domain Statement
 * ----------------------
 */
typedef struct CreateDomainStmt
{
  NodeTag type;
  List *domainname;          /* qualified name (list of String) */
                             /* 限定名称（String 列表） */
  TypeName *typeName;        /* the base type */
                             /* 基础类型 */
  CollateClause *collClause; /* untransformed COLLATE spec, if any */
                             /* 未转换的 COLLATE 规范（如果有） */
  List *constraints;         /* constraints (list of Constraint nodes) */
                             /* 约束（Constraint 节点列表） */
} CreateDomainStmt;

/* ----------------------
 *		Create Operator Class Statement
 * ----------------------
 */
typedef struct CreateOpClassStmt
{
  NodeTag type;
  List *opclassname;  /* qualified name (list of String) */
                      /* 限定名称（String 列表） */
  List *opfamilyname; /* qualified name (ditto); NIL if omitted */
                      /* 限定名称（同上）；若省略则为 NIL */
  char *amname;       /* name of index AM opclass is for */
                      /* 操作符类所属的索引访问方法名称 */
  TypeName *datatype; /* datatype of indexed column */
                      /* 被索引列的数据类型 */
  List *items;        /* List of CreateOpClassItem nodes */
                      /* CreateOpClassItem 节点列表 */
  bool isDefault;     /* Should be marked as default for type? */
                      /* 是否应当被标记为类型的默认操作符类？ */
} CreateOpClassStmt;

#define OPCLASS_ITEM_OPERATOR 1
#define OPCLASS_ITEM_FUNCTION 2
#define OPCLASS_ITEM_STORAGETYPE 3

typedef struct CreateOpClassItem
{
  NodeTag type;
  int itemtype;         /* see codes above */
                        /* 见上述代码 */
  ObjectWithArgs *name; /* operator or function name and args */
                        /* 操作符或函数名称及参数 */
  int number;           /* strategy num or support proc num */
                        /* 策略号或支持过程号 */
  List *order_family;   /* only used for ordering operators */
                        /* 仅用于排序操作符 */
  List *class_args;     /* amproclefttype/amprocrighttype or
                         * amoplefttype/amoprighttype */
                        /* amproclefttype/amprocrighttype 或
                         * amoplefttype/amoprighttype */
  /* fields used for a storagetype item: */
  /* 用于 storagetype 项的字段： */
  TypeName *storedtype; /* datatype stored in index */
                        /* 存储在索引中的数据类型 */
} CreateOpClassItem;

/* ----------------------
 *		Create Operator Family Statement
 * ----------------------
 */
typedef struct CreateOpFamilyStmt
{
  NodeTag type;
  List *opfamilyname; /* qualified name (list of String) */
                      /* 限定名称（String 列表） */
  char *amname;       /* name of index AM opfamily is for */
                      /* 操作符族所属的索引访问方法名称 */
} CreateOpFamilyStmt;

/* ----------------------
 *		Alter Operator Family Statement
 * ----------------------
 */
typedef struct AlterOpFamilyStmt
{
  NodeTag type;
  List *opfamilyname; /* qualified name (list of String) */
                      /* 限定名称（String 列表） */
  char *amname;       /* name of index AM opfamily is for */
                      /* 操作符族所属的索引访问方法名称 */
  bool isDrop;        /* ADD or DROP the items? */
                      /* 是添加还是删除项？ */
  List *items;        /* List of CreateOpClassItem nodes */
                      /* CreateOpClassItem 节点列表 */
} AlterOpFamilyStmt;

/* ----------------------
 *		Drop Table|Sequence|View|Index|Type|Domain|Conversion|Schema
 *Statement
 * ----------------------
 */

typedef struct DropStmt
{
  NodeTag type;
  List *objects;         /* list of names */
                         /* 名称列表 */
  ObjectType removeType; /* object type */
                         /* 对象类型 */
  DropBehavior behavior; /* RESTRICT or CASCADE behavior */
                         /* RESTRICT 或 CASCADE 行为 */
  bool missing_ok;       /* skip error if object is missing? */
                         /* 若对象缺失是否跳过错误？ */
  bool concurrent;       /* drop index concurrently? */
                         /* 是否并发删除索引？ */
} DropStmt;

/* ----------------------
 *				Truncate Table Statement
 * ----------------------
 */
typedef struct TruncateStmt
{
  NodeTag type;
  List *relations;       /* relations (RangeVars) to be truncated */
                         /* 要截断的关系（RangeVar 列表） */
  bool restart_seqs;     /* restart owned sequences? */
                         /* 是否重启拥有的序列？ */
  DropBehavior behavior; /* RESTRICT or CASCADE behavior */
                         /* RESTRICT 或 CASCADE 行为 */
} TruncateStmt;

/* ----------------------
 *				Comment On Statement
 * ----------------------
 */
typedef struct CommentStmt
{
  NodeTag type;
  ObjectType objtype; /* Object's type */
                      /* 对象类型 */
  Node *object;       /* Qualified name of the object */
                      /* 对象的限定名称 */
  char *comment;      /* Comment to insert, or NULL to remove */
                      /* 要插入的注释，或为 NULL 表示删除 */
} CommentStmt;

/* ----------------------
 *				SECURITY LABEL Statement
 * ----------------------
 */
typedef struct SecLabelStmt
{
  NodeTag type;
  ObjectType objtype; /* Object's type */
                      /* 对象类型 */
  Node *object;       /* Qualified name of the object */
                      /* 对象的限定名称 */
  char *provider;     /* Label provider (or NULL) */
                      /* 标签提供者（或为 NULL） */
  char *label;        /* New security label to be assigned */
                      /* 要分配的新安全标签 */
} SecLabelStmt;

/* ----------------------
 *		Declare Cursor Statement
 *
 * The "query" field is initially a raw parse tree, and is converted to a
 * Query node during parse analysis.  Note that rewriting and planning
 * of the query are always postponed until execution.
 *
 *		声明游标语句
 *
 * "query" 字段最初是一个原始解析树，在解析分析过程中被转换为 Query 节点。
 * 请注意，查询的重写和规划总是推迟到执行时。
 * ----------------------
 */
#define CURSOR_OPT_BINARY 0x0001      /* BINARY */
#define CURSOR_OPT_SCROLL 0x0002      /* SCROLL explicitly given */
                                      /* 显式给出的 SCROLL */
#define CURSOR_OPT_NO_SCROLL 0x0004   /* NO SCROLL explicitly given */
                                      /* 显式给出的 NO SCROLL */
#define CURSOR_OPT_INSENSITIVE 0x0008 /* INSENSITIVE */
#define CURSOR_OPT_ASENSITIVE 0x0010  /* ASENSITIVE */
#define CURSOR_OPT_HOLD 0x0020        /* WITH HOLD */
/* these planner-control flags do not correspond to any SQL grammar: */
/* 这些规划器控制标志不对应任何 SQL 语法： */
#define CURSOR_OPT_FAST_PLAN 0x0100    /* prefer fast-start plan */
                                       /* 偏好快速启动计划 */
#define CURSOR_OPT_GENERIC_PLAN 0x0200 /* force use of generic plan */
                                       /* 强制使用通用计划 */
#define CURSOR_OPT_CUSTOM_PLAN 0x0400  /* force use of custom plan */
                                       /* 强制使用自定义计划 */
#define CURSOR_OPT_PARALLEL_OK 0x0800  /* parallel mode OK */
                                       /* 可以使用并行模式 */

typedef struct DeclareCursorStmt
{
  NodeTag type;
  char *portalname; /* name of the portal (cursor) */
                    /* portal（游标）的名称 */
  int options;      /* bitmask of options (see above) */
                    /* 选项的位掩码（见上文） */
  Node *query;      /* the query (see comments above) */
                    /* 查询（见上文注释） */
} DeclareCursorStmt;

/* ----------------------
 *		Close Portal Statement
 *
 *		关闭 Portal（游标）语句
 * ----------------------
 */
typedef struct ClosePortalStmt
{
  NodeTag type;
  char *portalname; /* name of the portal (cursor) */
                    /* portal（游标）的名称 */
                    /* NULL means CLOSE ALL */
                    /* NULL 表示 CLOSE ALL */
} ClosePortalStmt;

/* ----------------------
 *		Fetch Statement (also Move)
 *
 *		Fetch 语句（也包括 Move）
 * ----------------------
 */
typedef enum FetchDirection
{
  /* for these, howMany is how many rows to fetch; FETCH_ALL means ALL */
  /* 对于这些情况，howMany 是要获取的行数；FETCH_ALL 表示全部 */
  FETCH_FORWARD,
  FETCH_BACKWARD,
  /* for these, howMany indicates a position; only one row is fetched */
  /* 对于这些情况，howMany 指示一个位置；仅获取一行 */
  FETCH_ABSOLUTE,
  FETCH_RELATIVE,
} FetchDirection;

#define FETCH_ALL LONG_MAX

typedef struct FetchStmt
{
  NodeTag type;
  FetchDirection direction; /* see above */
                            /* 见上文 */
  long howMany;             /* number of rows, or position argument */
                            /* 行数或位置参数 */
  char *portalname;         /* name of portal (cursor) */
                            /* portal（游标）的名称 */
  bool ismove;              /* true if MOVE */
                            /* 如果是 MOVE 则为 true */
} FetchStmt;

/* ----------------------
 *		Create Index Statement
 *
 * This represents creation of an index and/or an associated constraint.
 * If isconstraint is true, we should create a pg_constraint entry along
 * with the index.  But if indexOid isn't InvalidOid, we are not creating an
 * index, just a UNIQUE/PKEY constraint using an existing index.  isconstraint
 * must always be true in this case, and the fields describing the index
 * properties are empty.
 *
 *		创建索引语句
 *
 * 这表示创建一个索引和/或关联的约束。
 * 如果 isconstraint 为 true，我们应当在创建索引的同时创建一个 pg_constraint 条目。
 * 但如果 indexOid 不是 InvalidOid，我们就不是在创建索引，而只是使用现有索引
 * 创建一个 UNIQUE/PKEY 约束。在这种情况下，isconstraint 必须始终为 true，
 * 且描述索引属性的字段为空。
 * ----------------------
 */
typedef struct IndexStmt
{
  NodeTag type;
  char *idxname;              /* name of new index, or NULL for default */
                              /* 新索引的名称，或为 NULL 表示默认名称 */
  RangeVar *relation;         /* relation to build index on */
                              /* 在其上构建索引的关系 */
  char *accessMethod;         /* name of access method (eg. btree) */
                              /* 访问方法名称（如 btree） */
  char *tableSpace;           /* tablespace, or NULL for default */
                              /* 表空间，或为 NULL 表示默认表空间 */
  List *indexParams;          /* columns to index: a list of IndexElem */
                              /* 要索引的列：IndexElem 列表 */
  List *indexIncludingParams; /* additional columns to index: a list
                               * of IndexElem */
                              /* 要索引的其他列：IndexElem 列表 */
  List *options;              /* WITH clause options: a list of DefElem */
                              /* WITH 子句选项：DefElem 列表 */
  Node *whereClause;          /* qualification (partial-index predicate) */
                              /* 限定条件（部分索引谓词） */
  List *excludeOpNames;       /* exclusion operator names, or NIL if none */
                              /* 排除操作符名称，若无则为 NIL */
  char *idxcomment;           /* comment to apply to index, or NULL */
                              /* 要应用于索引的注释，或为 NULL */
  Oid indexOid;               /* OID of an existing index, if any */
                              /* 现有索引的 OID（如果有） */
  RelFileNumber oldNumber;    /* relfilenumber of existing storage, if any */
                              /* 现有存储的元文件编号（如果有） */
  SubTransactionId oldCreateSubid; /* rd_createSubid of oldNumber */
                                   /* oldNumber 的 rd_createSubid */
  SubTransactionId oldFirstRelfilelocatorSubid; /* rd_firstRelfilelocatorSubid
                                                 * of oldNumber */
                                                /* oldNumber 的 rd_firstRelfilelocatorSubid */
  bool unique;                                  /* is index unique? */
                                                /* 索引是否唯一？ */
  bool nulls_not_distinct;   /* null treatment for UNIQUE constraints */
                             /* UNIQUE 约束中 NULL 的处理 */
  bool primary;              /* is index a primary key? */
                             /* 索引是否为主键？ */
  bool isconstraint;         /* is it for a pkey/unique constraint? */
                             /* 它是为了主键/唯一约束吗？ */
  bool iswithoutoverlaps;    /* is the constraint WITHOUT OVERLAPS? */
                             /* 约束是否带有 WITHOUT OVERLAPS？ */
  bool deferrable;           /* is the constraint DEFERRABLE? */
                             /* 约束是否可延迟？ */
  bool initdeferred;         /* is the constraint INITIALLY DEFERRED? */
                             /* 约束是否初始延迟？ */
  bool transformed;          /* true when transformIndexStmt is finished */
                             /* transformIndexStmt 完成后为 true */
  bool concurrent;           /* should this be a concurrent index build? */
                             /* 这是否应当是并发索引构建？ */
  bool if_not_exists;        /* just do nothing if index already exists? */
                             /* 如果索引已存在，是否不执行操作？ */
  bool reset_default_tblspc; /* reset default_tablespace prior to
                              * executing */
                             /* 执行前是否重置 default_tablespace */
} IndexStmt;

/* ----------------------
 *		Create Statistics Statement
 *
 *		创建统计信息语句
 * ----------------------
 */
typedef struct CreateStatsStmt
{
  NodeTag type;
  List *defnames;     /* qualified name (list of String) */
                      /* 限定名称（String 列表） */
  List *stat_types;   /* stat types (list of String) */
                      /* 统计类型（String 列表） */
  List *exprs;        /* expressions to build statistics on */
                      /* 要在其上构建统计信息的表达式 */
  List *relations;    /* rels to build stats on (list of RangeVar) */
                      /* 要在其上构建统计信息的关系（RangeVar 列表） */
  char *stxcomment;   /* comment to apply to stats, or NULL */
                      /* 要应用于统计信息的注释，或为 NULL */
  bool transformed;   /* true when transformStatsStmt is finished */
                      /* transformStatsStmt 完成后为 true */
  bool if_not_exists; /* do nothing if stats name already exists */
                      /* 如果统计信息名称已存在，则不执行操作 */
} CreateStatsStmt;

/*
 * StatsElem - statistics parameters (used in CREATE STATISTICS)
 *
 * StatsElem - 统计参数（用于 CREATE STATISTICS）
 *
 * For a plain attribute, 'name' is the name of the referenced table column
 * and 'expr' is NULL.  For an expression, 'name' is NULL and 'expr' is the
 * expression tree.
 *
 * 对于普通属性，'name' 是被引用的表列名，且 'expr' 为 NULL。
 * 对于表达式，'name' 为 NULL，且 'expr' 是表达式树。
 */
typedef struct StatsElem
{
  NodeTag type;
  char *name; /* name of attribute to index, or NULL */
              /* 要索引的属性名称，或为 NULL */
  Node *expr; /* expression to index, or NULL */
              /* 要索引的表达式，或为 NULL */
} StatsElem;

/* ----------------------
 *		Alter Statistics Statement
 *
 *		修改统计信息语句
 * ----------------------
 */
typedef struct AlterStatsStmt
{
  NodeTag type;
  List *defnames;      /* qualified name (list of String) */
                       /* 限定名称（String 列表） */
  Node *stxstattarget; /* statistics target */
                       /* 统计目标 */
  bool missing_ok;     /* skip error if statistics object is missing */
                       /* 如果统计对象缺失，是否跳过错误 */
} AlterStatsStmt;

/* ----------------------
 *		Create Function Statement
 *
 *		创建函数语句
 * ----------------------
 */
typedef struct CreateFunctionStmt
{
  NodeTag type;
  bool is_procedure;    /* it's really CREATE PROCEDURE */
                        /* 它实际上是 CREATE PROCEDURE */
  bool replace;         /* T => replace if already exists */
                        /* T => 如果已存在则替换 */
  List *funcname;       /* qualified name of function to create */
                        /* 要创建的函数的限定名称 */
  List *parameters;     /* a list of FunctionParameter */
                        /* FunctionParameter 列表 */
  TypeName *returnType; /* the return type */
                        /* 返回类型 */
  List *options;        /* a list of DefElem */
                        /* DefElem 列表 */
  Node *sql_body;
} CreateFunctionStmt;

typedef enum FunctionParameterMode
{
  /* the assigned enum values appear in pg_proc, don't change 'em! */
  /* 分配的枚举值出现在 pg_proc 中，不要更改它们！ */
  FUNC_PARAM_IN = 'i',       /* input only */
                             /* 仅输入 */
  FUNC_PARAM_OUT = 'o',      /* output only */
                             /* 仅输出 */
  FUNC_PARAM_INOUT = 'b',    /* both */
                             /* 两者皆有 */
  FUNC_PARAM_VARIADIC = 'v', /* variadic (always input) */
                             /* 可变参数（始终为输入） */
  FUNC_PARAM_TABLE = 't',    /* table function output column */
                             /* 表函数输出列 */
  /* this is not used in pg_proc: */
  /* 这不在 pg_proc 中使用： */
  FUNC_PARAM_DEFAULT = 'd', /* default; effectively same as IN */
                            /* 默认；实际上与 IN 相同 */
} FunctionParameterMode;

typedef struct FunctionParameter
{
  NodeTag type;
  char *name;                 /* parameter name, or NULL if not given */
                              /* 参数名称，如果未给出则为 NULL */
  TypeName *argType;          /* TypeName for parameter type */
                              /* 参数类型的 TypeName */
  FunctionParameterMode mode; /* IN/OUT/etc */
                              /* IN/OUT 等 */
  Node *defexpr;              /* raw default expr, or NULL if not given */
                              /* 原始默认表达式，如果未给出则为 NULL */
  ParseLoc location;          /* token location, or -1 if unknown */
                              /* 标记位置，若未知则为 -1 */
} FunctionParameter;

typedef struct AlterFunctionStmt
{
  NodeTag type;
  ObjectType objtype;
  ObjectWithArgs *func; /* name and args of function */
                        /* 函数名称和参数 */
  List *actions;        /* list of DefElem */
                        /* DefElem 列表 */
} AlterFunctionStmt;

/* ----------------------
 *		DO Statement
 *
 * DoStmt is the raw parser output, InlineCodeBlock is the execution-time API
 *
 *		DO 语句
 *
 * DoStmt 是原始解析器输出，InlineCodeBlock 是执行时 API
 * ----------------------
 */
typedef struct DoStmt
{
  NodeTag type;
  List *args; /* List of DefElem nodes */
              /* DefElem 节点列表 */
} DoStmt;

typedef struct InlineCodeBlock
{
  pg_node_attr(nodetag_only) /* this is not a member of parse trees */

      NodeTag type;
  char *source_text;  /* source text of anonymous code block */
                      /* 匿名代码块的源文本 */
  Oid langOid;        /* OID of selected language */
                      /* 所选语言的 OID */
  bool langIsTrusted; /* trusted property of the language */
                      /* 语言的信任属性 */
  bool atomic;        /* atomic execution context */
                      /* 原子执行上下文 */
} InlineCodeBlock;

/* ----------------------
 *		CALL statement
 *
 * OUT-mode arguments are removed from the transformed funcexpr.  The outargs
 * list contains copies of the expressions for all output arguments, in the
 * order of the procedure's declared arguments.  (outargs is never evaluated,
 * but is useful to the caller as a reference for what to assign to.)
 * The transformed call state is not relevant in the query jumbling, only the
 * function call is.
 *
 *		CALL 语句
 *
 * OUT 模式参数将从转换后的 funcexpr 中移除。outargs 列表包含所有输出参数
 * 表达式的副本，顺序与存储过程声明的参数顺序一致。（outargs 永远不会被
 * 求值，但它对于调用方来说非常有用，可以作为赋值的参考。）
 * 转换后的调用状态与查询指纹计算无关，只有函数调用本身有关。
 * ----------------------
 */
typedef struct CallStmt
{
  NodeTag type;
  /* from the parser */
  /* 来自解析器 */
  FuncCall *funccall pg_node_attr(query_jumble_ignore);
  /* transformed call, with only input args */
  /* 转换后的调用，仅包含输入参数 */
  FuncExpr *funcexpr;
  /* transformed output-argument expressions */
  /* 转换后的输出参数表达式 */
  List *outargs;
} CallStmt;

typedef struct CallContext
{
  pg_node_attr(nodetag_only) /* this is not a member of parse trees */

      NodeTag type;
  bool atomic;
} CallContext;

/* ----------------------
 *		Alter Object Rename Statement
 *
 *		重命名对象语句
 * ----------------------
 */
typedef struct RenameStmt
{
  NodeTag type;
  ObjectType renameType;   /* OBJECT_TABLE, OBJECT_COLUMN, etc */
                           /* OBJECT_TABLE, OBJECT_COLUMN 等 */
  ObjectType relationType; /* if column name, associated relation type */
                           /* 如果是列名，则是关联的关系类型 */
  RangeVar *relation;      /* in case it's a table */
                           /* 针对表的情况 */
  Node *object;            /* in case it's some other object */
                           /* 针对其他对象的情况 */
  char *subname;           /* name of contained object (column, rule,
                            * trigger, etc) */
                           /* 所包含对象的名称（列、规则、触发器等） */
  char *newname;           /* the new name */
                           /* 新名称 */
  DropBehavior behavior;   /* RESTRICT or CASCADE behavior */
                           /* RESTRICT 或 CASCADE 行为 */
  bool missing_ok;         /* skip error if missing? */
                           /* 若缺失是否跳过错误？ */
} RenameStmt;

/* ----------------------
 * ALTER object DEPENDS ON EXTENSION extname
 *
 * 修改对象，使其依赖于扩展 extname
 * ----------------------
 */
typedef struct AlterObjectDependsStmt
{
  NodeTag type;
  ObjectType objectType; /* OBJECT_FUNCTION, OBJECT_TRIGGER, etc */
                         /* OBJECT_FUNCTION, OBJECT_TRIGGER 等 */
  RangeVar *relation;    /* in case a table is involved */
                         /* 涉及表的情况 */
  Node *object;          /* name of the object */
                         /* 对象名称 */
  String *extname;       /* extension name */
                         /* 扩展名称 */
  bool remove;           /* set true to remove dep rather than add */
                         /* 设置为 true 表示移除依赖而非添加 */
} AlterObjectDependsStmt;

/* ----------------------
 *		ALTER object SET SCHEMA Statement
 *
 *		修改对象模式语句
 * ----------------------
 */
typedef struct AlterObjectSchemaStmt
{
  NodeTag type;
  ObjectType objectType; /* OBJECT_TABLE, OBJECT_TYPE, etc */
                         /* OBJECT_TABLE, OBJECT_TYPE 等 */
  RangeVar *relation;    /* in case it's a table */
                         /* 针对表的情况 */
  Node *object;          /* in case it's some other object */
                         /* 针对其他对象的情况 */
  char *newschema;       /* the new schema */
                         /* 新模式 */
  bool missing_ok;       /* skip error if missing? */
                         /* 若缺失是否跳过错误？ */
} AlterObjectSchemaStmt;

/* ----------------------
 *		Alter Object Owner Statement
 *
 *		修改对象所有者语句
 * ----------------------
 */
typedef struct AlterOwnerStmt
{
  NodeTag type;
  ObjectType objectType; /* OBJECT_TABLE, OBJECT_TYPE, etc */
                         /* OBJECT_TABLE, OBJECT_TYPE 等 */
  RangeVar *relation;    /* in case it's a table */
                         /* 针对表的情况 */
  Node *object;          /* in case it's some other object */
                         /* 针对其他对象的情况 */
  RoleSpec *newowner;    /* the new owner */
                         /* 新所有者 */
} AlterOwnerStmt;

/* ----------------------
 *		Alter Operator Set ( this-n-that )
 *
 *		修改操作符设置
 * ----------------------
 */
typedef struct AlterOperatorStmt
{
  NodeTag type;
  ObjectWithArgs *opername; /* operator name and argument types */
                            /* 操作符名称和参数类型 */
  List *options;            /* List of DefElem nodes */
                            /* DefElem 节点列表 */
} AlterOperatorStmt;

/* ------------------------
 *		Alter Type Set ( this-n-that )
 *
 *		修改类型设置
 * ------------------------
 */
typedef struct AlterTypeStmt
{
  NodeTag type;
  List *typeName; /* type name (possibly qualified) */
                  /* 类型名称（可能被限定） */
  List *options;  /* List of DefElem nodes */
                  /* DefElem 节点列表 */
} AlterTypeStmt;

/* ----------------------
 *		Create Rule Statement
 *
 *		创建规则语句
 * ----------------------
 */
typedef struct RuleStmt
{
  NodeTag type;
  RangeVar *relation; /* relation the rule is for */
                      /* 该规则所属的关系 */
  char *rulename;     /* name of the rule */
                      /* 规则名称 */
  Node *whereClause;  /* qualifications */
                      /* 限定条件 */
  CmdType event;      /* SELECT, INSERT, etc */
                      /* 事件：SELECT, INSERT 等 */
  bool instead;       /* is a 'do instead'? */
                      /* 是否为 'do instead'？ */
  List *actions;      /* the action statements */
                      /* 动作语句 */
  bool replace;       /* OR REPLACE */
} RuleStmt;

/* ----------------------
 *		Notify Statement
 *
 *		通知语句（NOTIFY）
 * ----------------------
 */
typedef struct NotifyStmt
{
  NodeTag type;
  char *conditionname; /* condition name to notify */
                       /* 要通知的条件名称 */
  char *payload;       /* the payload string, or NULL if none */
                       /* 负载字符串，若无则为 NULL */
} NotifyStmt;

/* ----------------------
 *		Listen Statement
 *
 *		监听语句（LISTEN）
 * ----------------------
 */
typedef struct ListenStmt
{
  NodeTag type;
  char *conditionname; /* condition name to listen on */
                       /* 要监听的条件名称 */
} ListenStmt;

/* ----------------------
 *		Unlisten Statement
 *
 *		取消监听语句（UNLISTEN）
 * ----------------------
 */
typedef struct UnlistenStmt
{
  NodeTag type;
  char *conditionname; /* name to unlisten on, or NULL for all */
                       /* 要取消监听的名称，或为 NULL 表示全部 */
} UnlistenStmt;

/* ----------------------
 *		{Begin|Commit|Rollback} Transaction Statement
 *
 *		{Begin|Commit|Rollback} 事务语句
 * ----------------------
 */
typedef enum TransactionStmtKind
{
  TRANS_STMT_BEGIN,
  TRANS_STMT_START, /* semantically identical to BEGIN */
                    /* 语义上与 BEGIN 相同 */
  TRANS_STMT_COMMIT,
  TRANS_STMT_ROLLBACK,
  TRANS_STMT_SAVEPOINT,
  TRANS_STMT_RELEASE,
  TRANS_STMT_ROLLBACK_TO,
  TRANS_STMT_PREPARE,
  TRANS_STMT_COMMIT_PREPARED,
  TRANS_STMT_ROLLBACK_PREPARED,
} TransactionStmtKind;

typedef struct TransactionStmt
{
  NodeTag type;
  TransactionStmtKind kind; /* see above */
                            /* 见上文 */
  List *options;            /* for BEGIN/START commands */
                            /* 用于 BEGIN/START 命令 */
  /* for savepoint commands */
  /* 用于 SAVEPOINT 命令 */
  char *savepoint_name pg_node_attr(query_jumble_ignore);
  /* for two-phase-commit related commands */
  /* 用于两阶段提交相关命令 */
  char *gid pg_node_attr(query_jumble_ignore);
  bool chain; /* AND CHAIN option */
              /* AND CHAIN 选项 */
  /* token location, or -1 if unknown */
  /* 标记位置，若未知则为 -1 */
  ParseLoc location pg_node_attr(query_jumble_location);
} TransactionStmt;

/* ----------------------
 *		Create Type Statement, composite types
 *
 *		创建类型语句，复合类型
 * ----------------------
 */
typedef struct CompositeTypeStmt
{
  NodeTag type;
  RangeVar *typevar; /* the composite type to be created */
                     /* 要创建的复合类型 */
  List *coldeflist;  /* list of ColumnDef nodes */
                     /* ColumnDef 节点列表 */
} CompositeTypeStmt;

/* ----------------------
 *		Create Type Statement, enum types
 *
 *		创建类型语句，枚举类型
 * ----------------------
 */
typedef struct CreateEnumStmt
{
  NodeTag type;
  List *typeName; /* qualified name (list of String) */
                  /* 限定名称（String 列表） */
  List *vals;     /* enum values (list of String) */
                  /* 枚举值（String 列表） */
} CreateEnumStmt;

/* ----------------------
 *		Create Type Statement, range types
 *
 *		创建类型语句，范围类型
 * ----------------------
 */
typedef struct CreateRangeStmt
{
  NodeTag type;
  List *typeName; /* qualified name (list of String) */
                  /* 限定名称（String 列表） */
  List *params;   /* range parameters (list of DefElem) */
                  /* 范围参数（DefElem 列表） */
} CreateRangeStmt;

/* ----------------------
 *		Alter Type Statement, enum types
 *
 *		修改类型语句，枚举类型
 * ----------------------
 */
typedef struct AlterEnumStmt
{
  NodeTag type;
  List *typeName;          /* qualified name (list of String) */
                           /* 限定名称（String 列表） */
  char *oldVal;            /* old enum value's name, if renaming */
                           /* 旧枚举值名称（如果要重命名） */
  char *newVal;            /* new enum value's name */
                           /* 新枚举值名称 */
  char *newValNeighbor;    /* neighboring enum value, if specified */
                           /* 相邻枚举值（如果已指定） */
  bool newValIsAfter;      /* place new enum value after neighbor? */
                           /* 是否将新枚举值放在相邻值之后？ */
  bool skipIfNewValExists; /* no error if new already exists? */
                           /* 如果新值已存在，是否不报错？ */
} AlterEnumStmt;

/* ----------------------
 *		Create View Statement
 *
 *		创建视图语句
 * ----------------------
 */
typedef enum ViewCheckOption
{
  NO_CHECK_OPTION,
  LOCAL_CHECK_OPTION,
  CASCADED_CHECK_OPTION,
} ViewCheckOption;

typedef struct ViewStmt
{
  NodeTag type;
  RangeVar *view;                  /* the view to be created */
                                   /* 要创建的视图 */
  List *aliases;                   /* target column names */
                                   /* 目标列名 */
  Node *query;                     /* the SELECT query (as a raw parse tree) */
                                   /* SELECT 查询（作为原始解析树） */
  bool replace;                    /* replace an existing view? */
                                   /* 是否替换现有视图？ */
  List *options;                   /* options from WITH clause */
                                   /* WITH 子句中的选项 */
  ViewCheckOption withCheckOption; /* WITH CHECK OPTION */
} ViewStmt;

/* ----------------------
 *		Load Statement
 *
 *		加载语句（LOAD）
 * ----------------------
 */
typedef struct LoadStmt
{
  NodeTag type;
  char *filename; /* file to load */
                  /* 要加载的文件 */
} LoadStmt;

/* ----------------------
 *		Createdb Statement
 *
 *		创建数据库语句
 * ----------------------
 */
typedef struct CreatedbStmt
{
  NodeTag type;
  char *dbname;  /* name of database to create */
                 /* 要创建的数据库名称 */
  List *options; /* List of DefElem nodes */
                 /* DefElem 节点列表 */
} CreatedbStmt;

/* ----------------------
 *	Alter Database
 *
 *	修改数据库
 * ----------------------
 */
typedef struct AlterDatabaseStmt
{
  NodeTag type;
  char *dbname;  /* name of database to alter */
                 /* 要修改的数据库名称 */
  List *options; /* List of DefElem nodes */
                 /* DefElem 节点列表 */
} AlterDatabaseStmt;

typedef struct AlterDatabaseRefreshCollStmt
{
  NodeTag type;
  char *dbname;
} AlterDatabaseRefreshCollStmt;

typedef struct AlterDatabaseSetStmt
{
  NodeTag type;
  char *dbname;             /* database name */
                            /* 数据库名称 */
  VariableSetStmt *setstmt; /* SET or RESET subcommand */
                            /* SET 或 RESET 子命令 */
} AlterDatabaseSetStmt;

/* ----------------------
 *		Dropdb Statement
 *
 *		删除数据库语句
 * ----------------------
 */
typedef struct DropdbStmt
{
  NodeTag type;
  char *dbname;    /* database to drop */
                   /* 要删除的数据库 */
  bool missing_ok; /* skip error if db is missing? */
                   /* 如果数据库不存在，是否跳过错误？ */
  List *options;   /* currently only FORCE is supported */
                   /* 目前仅支持 FORCE */
} DropdbStmt;

/* ----------------------
 *		Alter System Statement
 *
 *		修改系统设置语句（ALTER SYSTEM）
 * ----------------------
 */
typedef struct AlterSystemStmt
{
  NodeTag type;
  VariableSetStmt *setstmt; /* SET subcommand */
                            /* SET 子命令 */
} AlterSystemStmt;

/* ----------------------
 *		Cluster Statement (support pbrown's cluster index
 *implementation)
 *
 *		集群语句（CLUSTER）（支持 pbrown 的集群索引实现）
 * ----------------------
 */
typedef struct ClusterStmt
{
  NodeTag type;
  RangeVar *relation; /* relation being indexed, or NULL if all */
                      /* 正在索引的关系，或为 NULL 表示全部 */
  char *indexname;    /* original index defined */
                      /* 定义的原始索引 */
  List *params;       /* list of DefElem nodes */
                      /* DefElem 节点列表 */
} ClusterStmt;

/* ----------------------
 *		Vacuum and Analyze Statements
 *
 * Even though these are nominally two statements, it's convenient to use
 * just one node type for both.
 *
 *		VACUUM 和 ANALYZE 语句
 *
 * 尽管名义上是两个语句，但为两者使用同一种节点类型会很方便。
 * ----------------------
 */
typedef struct VacuumStmt
{
  NodeTag type;
  List *options;     /* list of DefElem nodes */
                     /* DefElem 节点列表 */
  List *rels;        /* list of VacuumRelation, or NIL for all */
                     /* VacuumRelation 列表，或为 NIL 表示全部 */
  bool is_vacuumcmd; /* true for VACUUM, false for ANALYZE */
                     /* VACUUM 为 true，ANALYZE 为 false */
} VacuumStmt;

/*
 * Info about a single target table of VACUUM/ANALYZE.
 *
 * Info about a single target table of VACUUM/ANALYZE.
 *
 * If the OID field is set, it always identifies the table to process.
 * Then the relation field can be NULL; if it isn't, it's used only to report
 * failure to open/lock the relation.
 *
 * 如果设置了 OID 字段，它始终标识要处理的表。
 * 那么 relation 字段可以为 NULL；如果不为 NULL，它仅用于报告打开/锁定关系失败。
 */
typedef struct VacuumRelation
{
  NodeTag type;
  RangeVar *relation; /* table name to process, or NULL */
                      /* 要处理的表名，或为 NULL */
  Oid oid;            /* table's OID; InvalidOid if not looked up */
                      /* 表的 OID；如果未查找则为 InvalidOid */
  List *va_cols;      /* list of column names, or NIL for all */
                      /* 列名列表，或为 NIL 表示全部 */
} VacuumRelation;

/* ----------------------
 *		Explain Statement
 *
 * The "query" field is initially a raw parse tree, and is converted to a
 * Query node during parse analysis.  Note that rewriting and planning
 * of the query are always postponed until execution.
 *
 *		阐明计划语句（EXPLAIN）
 *
 * "query" 字段最初是一个原始解析树，在解析分析过程中被转换为 Query 节点。
 * 请注意，查询的重写和规划总是推迟到执行时。
 * ----------------------
 */
typedef struct ExplainStmt
{
  NodeTag type;
  Node *query;   /* the query (see comments above) */
                 /* 查询（见上文注释） */
  List *options; /* list of DefElem nodes */
                 /* DefElem 节点列表 */
} ExplainStmt;

/* ----------------------
 *		CREATE TABLE AS Statement (a/k/a SELECT INTO)
 *
 * A query written as CREATE TABLE AS will produce this node type natively.
 * A query written as SELECT ... INTO will be transformed to this form during
 * parse analysis.
 * A query written as CREATE MATERIALIZED view will produce this node type,
 * during parse analysis, since it needs all the same data.
 *
 * The "query" field is handled similarly to EXPLAIN, though note that it
 * can be a SELECT or an EXECUTE, but not other DML statements.
 *
 *		CREATE TABLE AS 语句（即 SELECT INTO）
 *
 * 以 CREATE TABLE AS 编写的查询将原生生成此节点类型。
 * 以 SELECT ... INTO 编写的查询将在语法分析期间转换为此形式。
 * 以 CREATE MATERIALIZED VIEW 编写的查询将在解析分析期间生成此节点类型，
 * 因为它需要所有相同的数据。
 *
 * "query" 字段的处理方式与 EXPLAIN 类似，但请注意它可以是 SELECT 或 EXECUTE，
 * 且不能是其他 DML 语句。
 * ----------------------
 */
typedef struct CreateTableAsStmt
{
  NodeTag type;
  Node *query;         /* the query (see comments above) */
                       /* 查询（见上文注释） */
  IntoClause *into;    /* destination table */
                       /* 目标表 */
  ObjectType objtype;  /* OBJECT_TABLE or OBJECT_MATVIEW */
                       /* OBJECT_TABLE 或 OBJECT_MATVIEW */
  bool is_select_into; /* it was written as SELECT INTO */
                       /* 它是以 SELECT INTO 编写的 */
  bool if_not_exists;  /* just do nothing if it already exists? */
                       /* 如果已存在是否不执行操作？ */
} CreateTableAsStmt;

/* ----------------------
 *		REFRESH MATERIALIZED VIEW Statement
 *
 *		刷新物化视图语句
 * ----------------------
 */
typedef struct RefreshMatViewStmt
{
  NodeTag type;
  bool concurrent;    /* allow concurrent access? */
                      /* 是否允许并发访问？ */
  bool skipData;      /* true for WITH NO DATA */
                      /* 对于 WITH NO DATA 为 true */
  RangeVar *relation; /* relation to insert into */
                      /* 要插入的关系 */
} RefreshMatViewStmt;

/* ----------------------
 * Checkpoint Statement
 *
 * 检查点语句（CHECKPOINT）
 * ----------------------
 */
typedef struct CheckPointStmt
{
  NodeTag type;
} CheckPointStmt;

/* ----------------------
 * Discard Statement
 *
 * 丢弃语句（DISCARD）
 * ----------------------
 */

typedef enum DiscardMode
{
  DISCARD_ALL,
  DISCARD_PLANS,
  DISCARD_SEQUENCES,
  DISCARD_TEMP,
} DiscardMode;

typedef struct DiscardStmt
{
  NodeTag type;
  DiscardMode target;
} DiscardStmt;

/* ----------------------
 *		LOCK Statement
 *
 *		锁定语句（LOCK）
 * ----------------------
 */
typedef struct LockStmt
{
  NodeTag type;
  List *relations; /* relations to lock */
                   /* 要锁定的关系 */
  int mode;        /* lock mode */
                   /* 锁定模式 */
  bool nowait;     /* no wait mode */
                   /* 不等待模式 */
} LockStmt;

/* ----------------------
 *		SET CONSTRAINTS Statement
 *
 *		设置约束语句（SET CONSTRAINTS）
 * ----------------------
 */
typedef struct ConstraintsSetStmt
{
  NodeTag type;
  List *constraints; /* List of names as RangeVars */
                     /* 作为 RangeVar 的名称列表 */
  bool deferred;
} ConstraintsSetStmt;

/* ----------------------
 *		REINDEX Statement
 *
 *		重建索引语句（REINDEX）
 * ----------------------
 */
typedef enum ReindexObjectType
{
  REINDEX_OBJECT_INDEX,    /* index */
                           /* 索引 */
  REINDEX_OBJECT_TABLE,    /* table or materialized view */
                           /* 表或物化视图 */
  REINDEX_OBJECT_SCHEMA,   /* schema */
                           /* 模式 */
  REINDEX_OBJECT_SYSTEM,   /* system catalogs */
                           /* 系统目录 */
  REINDEX_OBJECT_DATABASE, /* database */
                           /* 数据库 */
} ReindexObjectType;

typedef struct ReindexStmt
{
  NodeTag type;
  ReindexObjectType kind; /* REINDEX_OBJECT_INDEX, REINDEX_OBJECT_TABLE,
                           * etc. */
                          /* REINDEX_OBJECT_INDEX, REINDEX_OBJECT_TABLE 等 */
  RangeVar *relation;     /* Table or index to reindex */
                          /* 要重建索引的表或索引 */
  const char *name;       /* name of database to reindex */
                          /* 要重建索引的数据库名称 */
  List *params;           /* list of DefElem nodes */
                          /* DefElem 节点列表 */
} ReindexStmt;

/* ----------------------
 *		CREATE CONVERSION Statement
 *
 *		创建编码转换语句（CREATE CONVERSION）
 * ----------------------
 */
typedef struct CreateConversionStmt
{
  NodeTag type;
  List *conversion_name;   /* Name of the conversion */
                           /* 转换名称 */
  char *for_encoding_name; /* source encoding name */
                           /* 源编码名称 */
  char *to_encoding_name;  /* destination encoding name */
                           /* 目标编码名称 */
  List *func_name;         /* qualified conversion function name */
                           /* 限定的转换函数名称 */
  bool def;                /* is this a default conversion? */
                           /* 这是否为默认转换？ */
} CreateConversionStmt;

/* ----------------------
 *	CREATE CAST Statement
 *
 *	创建类型强转语句（CREATE CAST）
 * ----------------------
 */
typedef struct CreateCastStmt
{
  NodeTag type;
  TypeName *sourcetype;
  TypeName *targettype;
  ObjectWithArgs *func;
  CoercionContext context;
  bool inout;
} CreateCastStmt;

/* ----------------------
 *	CREATE TRANSFORM Statement
 *
 *	创建转换语句（CREATE TRANSFORM）
 * ----------------------
 */
typedef struct CreateTransformStmt
{
  NodeTag type;
  bool replace;
  TypeName *type_name;
  char *lang;
  ObjectWithArgs *fromsql;
  ObjectWithArgs *tosql;
} CreateTransformStmt;

/* ----------------------
 *		PREPARE Statement
 *
 *		预处理语句（PREPARE）
 * ----------------------
 */
typedef struct PrepareStmt
{
  NodeTag type;
  char *name;     /* Name of plan, arbitrary */
                  /* 计划名称，任意选择 */
  List *argtypes; /* Types of parameters (List of TypeName) */
                  /* 参数类型（TypeName 列表） */
  Node *query;    /* The query itself (as a raw parsetree) */
                  /* 查询本身（作为原始解析树） */
} PrepareStmt;

/* ----------------------
 *		EXECUTE Statement
 *
 *		执行语句（EXECUTE）
 * ----------------------
 */

typedef struct ExecuteStmt
{
  NodeTag type;
  char *name;   /* The name of the plan to execute */
                /* 要执行的计划名称 */
  List *params; /* Values to assign to parameters */
                /* 要分配给参数的值 */
} ExecuteStmt;

/* ----------------------
 *		DEALLOCATE Statement
 *
 *		释放预留语句（DEALLOCATE）
 * ----------------------
 */
typedef struct DeallocateStmt
{
  NodeTag type;
  /* The name of the plan to remove, NULL if DEALLOCATE ALL */
  /* 要移除的计划名称，若为 DEALLOCATE ALL 则为 NULL */
  char *name pg_node_attr(query_jumble_ignore);

  /*
   * True if DEALLOCATE ALL.  This is redundant with "name == NULL", but we
   * make it a separate field so that exactly this condition (and not the
   * precise name) will be accounted for in query jumbling.
   */
  /*
   * 如果是 DEALLOCATE ALL，则为 true。这与 "name == NULL" 是冗余的，但我们
   * 将其作为一个单独的字段，以便使这个特定的条件（而非精确名称）在查询指纹
   * 计算中得到体现。
   */
  bool isall;
  /* token location, or -1 if unknown */
  /* 标记位置，若未知则为 -1 */
  ParseLoc location pg_node_attr(query_jumble_location);
} DeallocateStmt;

/*
 *		DROP OWNED statement
 *
 *		删除拥有的对象语句（DROP OWNED）
 */
typedef struct DropOwnedStmt
{
  NodeTag type;
  List *roles;           /* list of RoleSpec nodes */
                         /* RoleSpec 节点列表 */
  DropBehavior behavior; /* RESTRICT or CASCADE behavior */
                         /* RESTRICT 或 CASCADE 行为 */
} DropOwnedStmt;

/*
 *		REASSIGN OWNED statement
 *
 *		重分配拥有的对象语句（REASSIGN OWNED）
 */
typedef struct ReassignOwnedStmt
{
  NodeTag type;
  List *roles;       /* list of RoleSpec nodes */
                     /* RoleSpec 节点列表 */
  RoleSpec *newrole; /* new owner */
                     /* 新所有者 */
} ReassignOwnedStmt;

/*
 * TS Dictionary stmts: DefineStmt, RenameStmt and DropStmt are default
 */
/*
 * 全文检索字典语句：DefineStmt, RenameStmt 和 DropStmt 是默认值
 */
typedef struct AlterTSDictionaryStmt
{
  NodeTag type;
  List *dictname; /* qualified name (list of String) */
                  /* 限定名称（String 列表） */
  List *options;  /* List of DefElem nodes */
                  /* DefElem 节点列表 */
} AlterTSDictionaryStmt;

/*
 * TS Configuration stmts: DefineStmt, RenameStmt and DropStmt are default
 */
/*
 * 全文检索配置语句：DefineStmt, RenameStmt 和 DropStmt 是默认值
 */
typedef enum AlterTSConfigType
{
  ALTER_TSCONFIG_ADD_MAPPING,
  ALTER_TSCONFIG_ALTER_MAPPING_FOR_TOKEN,
  ALTER_TSCONFIG_REPLACE_DICT,
  ALTER_TSCONFIG_REPLACE_DICT_FOR_TOKEN,
  ALTER_TSCONFIG_DROP_MAPPING,
} AlterTSConfigType;

typedef struct AlterTSConfigurationStmt
{
  NodeTag type;
  AlterTSConfigType kind; /* ALTER_TSCONFIG_ADD_MAPPING, etc */
                          /* ALTER_TSCONFIG_ADD_MAPPING 等 */
  List *cfgname;          /* qualified name (list of String) */
                          /* 限定名称（String 列表） */

  /*
   * dicts will be non-NIL if ADD/ALTER MAPPING was specified. If dicts is
   * NIL, but tokentype isn't, DROP MAPPING was specified.
   */
  /*
   * 如果指定了 ADD/ALTER MAPPING，dicts 将不为 NIL。如果 dicts 为 NIL，
   * 但 tokentype 不为 NIL，则表示指定了 DROP MAPPING。
   */
  List *tokentype; /* list of String */
                   /* String 列表 */
  List *dicts;     /* list of list of String */
                   /* String 列表的列表 */
  bool override;   /* if true - remove old variant */
                   /* 如果为 true，则移除旧变元 */
  bool replace;    /* if true - replace dictionary by another */
                   /* 如果为 true，则用另一个字典替换该字典 */
  bool missing_ok; /* for DROP - skip error if missing? */
                   /* 针对 DROP：如果缺失是否跳过错误？ */
} AlterTSConfigurationStmt;

typedef struct PublicationTable
{
  NodeTag type;
  RangeVar *relation; /* relation to be published */
                      /* 要发布的关系 */
  Node *whereClause;  /* qualifications */
                      /* 限定条件 */
  List *columns;      /* List of columns in a publication table */
                      /* 发布表中的列列表 */
} PublicationTable;

/*
 * Publication object type
 *
 * 发布对象类型
 */
typedef enum PublicationObjSpecType
{
  PUBLICATIONOBJ_TABLE,                /* A table */
                                       /* 一个表 */
  PUBLICATIONOBJ_TABLES_IN_SCHEMA,     /* All tables in schema */
                                       /* 模式中的所有表 */
  PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA, /* All tables in first element of
                                        * search_path */
                                       /* search_path 第一个元素中的所有表 */
  PUBLICATIONOBJ_CONTINUATION,         /* Continuation of previous type */
                                       /* 前一类型的延续 */
} PublicationObjSpecType;

typedef struct PublicationObjSpec
{
  NodeTag type;
  PublicationObjSpecType pubobjtype; /* type of this publication object */
                                     /* 该发布对象的类型 */
  char *name;                        /* name of the object */
                                     /* 对象名称 */
  PublicationTable *pubtable;        /* published table, if applicable */
                                     /* 已发布表，如果适用 */
  ParseLoc location; /* token location, or -1 if unknown */
                     /* 标记位置，若未知则为 -1 */
} PublicationObjSpec;

typedef struct CreatePublicationStmt
{
  NodeTag type;
  char *pubname;       /* Name of the publication */
                       /* 发布名称 */
  List *options;       /* List of DefElem nodes */
                       /* DefElem 节点列表 */
  List *pubobjects;    /* Optional list of publication objects */
                       /* 可选的发布对象列表 */
  bool for_all_tables; /* Special publication for all tables in db */
                       /* 针对数据库中所有表的特殊发布 */
} CreatePublicationStmt;

typedef enum AlterPublicationAction
{
  AP_AddObjects,  /* add objects to publication */
  AP_DropObjects, /* remove objects from publication */
  AP_SetObjects,  /* set list of objects */
} AlterPublicationAction;

typedef struct AlterPublicationStmt
{
  NodeTag type;
  char *pubname; /* Name of the publication */
                 /* 发布名称 */

  /* parameters used for ALTER PUBLICATION ... WITH */
  /* 用于 ALTER PUBLICATION ... WITH 的参数 */
  List *options; /* List of DefElem nodes */
                 /* DefElem 节点列表 */

  /*
   * Parameters used for ALTER PUBLICATION ... ADD/DROP/SET publication
   * objects.
   */
  /*
   * 用于 ALTER PUBLICATION ... ADD/DROP/SET 发布对象的参数。
   */
  List *pubobjects;              /* Optional list of publication objects */
                                 /* 可选的发布对象列表 */
  bool for_all_tables;           /* Special publication for all tables in db */
                                 /* 针对数据库中所有表的特殊发布 */
  AlterPublicationAction action; /* What action to perform with the given
                                  * objects */
                                 /* 对给定对象执行什么操作 */
} AlterPublicationStmt;

typedef struct CreateSubscriptionStmt
{
  NodeTag type;
  char *subname;     /* Name of the subscription */
                     /* 订阅名称 */
  char *conninfo;    /* Connection string to publisher */
                     /* 连接发布者的字符串 */
  List *publication; /* One or more publication to subscribe to */
                     /* 要订阅的一个或多个发布 */
  List *options;     /* List of DefElem nodes */
                     /* DefElem 节点列表 */
} CreateSubscriptionStmt;

typedef enum AlterSubscriptionType
{
  ALTER_SUBSCRIPTION_OPTIONS,
  ALTER_SUBSCRIPTION_CONNECTION,
  ALTER_SUBSCRIPTION_SET_PUBLICATION,
  ALTER_SUBSCRIPTION_ADD_PUBLICATION,
  ALTER_SUBSCRIPTION_DROP_PUBLICATION,
  ALTER_SUBSCRIPTION_REFRESH,
  ALTER_SUBSCRIPTION_ENABLED,
  ALTER_SUBSCRIPTION_SKIP,
} AlterSubscriptionType;

typedef struct AlterSubscriptionStmt
{
  NodeTag type;
  AlterSubscriptionType kind; /* ALTER_SUBSCRIPTION_OPTIONS, etc */
                              /* ALTER_SUBSCRIPTION_OPTIONS 等 */
  char *subname;              /* Name of the subscription */
                              /* 订阅名称 */
  char *conninfo;             /* Connection string to publisher */
                              /* 连接发布者的字符串 */
  List *publication;          /* One or more publication to subscribe to */
                              /* 要订阅的一个或多个发布 */
  List *options;              /* List of DefElem nodes */
                              /* DefElem 节点列表 */
} AlterSubscriptionStmt;

typedef struct DropSubscriptionStmt
{
  NodeTag type;
  char *subname;         /* Name of the subscription */
                         /* 订阅名称 */
  bool missing_ok;       /* Skip error if missing? */
                         /* 若缺失是否跳过错误？ */
  DropBehavior behavior; /* RESTRICT or CASCADE behavior */
                         /* RESTRICT 或 CASCADE 行为 */
} DropSubscriptionStmt;

#endif /* PARSENODES_H */
