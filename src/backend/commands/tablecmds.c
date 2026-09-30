/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  Commands for creating and altering table structures and settings
 *
 * 创建与修改表结构及设置的命令实现。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/attmap.h"
#include "access/genam.h"
#include "access/gist.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "common/int.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"
#include "parser/parser.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/relcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/usercontext.h"

/*
 * 核心流程概览：
 * 本文件实现表 DDL。数据从语法树进入，经权限与锁检查后改系统目录，必要时再重写堆。
 * CREATE TABLE（DefineRelation）：解析命名空间、属主、表空间和访问方法，MergeAttributes
 * 合并继承列与约束，heap_create_with_catalog 建立目录项，再写入默认值、CHECK、继承关系、
 * 分区键与边界；若是分区，则克隆父表上的索引、触发器和外键。
 * DROP（RemoveRelations）：RangeVarCallbackForDropRelation 加锁并检查权限，收集对象后
 * 一次 performMultipleDeletions() 删除，避免互相依赖时误报 DROP RESTRICT。
 * TRUNCATE（ExecuteTruncate / ExecuteTruncateGuts）：排他锁、权限与并发检查，CASCADE 时
 * 吸入引用表，换新存储文件并处理 TOAST 与索引，按需重启 identity 序列，最后触发触发器。
 * ALTER TABLE（AlterTable -> ATController）分三阶段：Phase 1 由 ATPrepCmd 检查并入队；
 * Phase 2 由 ATRewriteCatalogs / ATExecCmd 按 AlterTablePass 分多遍改目录；Phase 3 由
 * ATRewriteTables 在需要时重写表并验证新约束。事件触发器包在阶段之外。
 * 边界：锁级别取各子命令的最强锁（AlterTableGetLockLevel）；继承与分区递归到子表；
 * 只改目录和必须重写堆分开处理；临时表 ON COMMIT 动作记在后端本地列表，提交时执行。
 */

/*
 * ON COMMIT action list
 *
 * ON COMMIT 动作列表
 */
typedef struct OnCommitItem
{
	Oid			relid;			/* relid of relation */
							/*
							 *
							 * 关系的 relid
							 */
	OnCommitAction oncommit;	/* what to do at end of xact */
					/*
					 *
					 * 事务结束时要执行的动作
					 */

	/*
	 * If this entry was created during the current transaction,
	 * creating_subid is the ID of the creating subxact; if created in a prior
	 * transaction, creating_subid is zero.  If deleted during the current
	 * transaction, deleting_subid is the ID of the deleting subxact; if no
	 * deletion request is pending, deleting_subid is zero.
	 *
	 * 若本项在当前事务中创建，creating_subid 是创建它的子事务 ID；若在更早的事务中创建，则为 0。若在当前事务中删除，
	 * deleting_subid 是删除它的子事务 ID；若没有待处理的删除请求，则为 0。
	 */
	SubTransactionId creating_subid;
	SubTransactionId deleting_subid;
} OnCommitItem;

static List *on_commits = NIL;


/*
 * State information for ALTER TABLE
 *
 * ALTER TABLE 的状态信息
 *
 * The pending-work queue for an ALTER TABLE is a List of AlteredTableInfo
 * structs, one for each table modified by the operation (the named table
 * plus any child tables that are affected).  We save lists of subcommands
 * to apply to this table (possibly modified by parse transformation steps);
 * these lists will be executed in Phase 2.  If a Phase 3 step is needed,
 * necessary information is stored in the constraints and newvals lists.
 *
 * ALTER TABLE 的待办队列是 AlteredTableInfo 结构的 List，操作改到的每张表各一项（指名的表，
 * 以及受影响的子表）。这里保存要作用到该表的子命令列表（解析变换后可能已改过），这些列表在 Phase 2 执行。若需要 Phase
 * 3，相关信息放在 constraints 和 newvals 列表里。
 *
 * Phase 2 is divided into multiple passes; subcommands are executed in
 * a pass determined by subcommand type.
 *
 * Phase 2 分成多遍；子命令按类型进入对应的遍次执行。
 */

typedef enum AlterTablePass
{
	AT_PASS_UNSET = -1,			/* UNSET will cause ERROR */
						/*
						 *
						 * UNSET 会触发 ERROR
						 */
	AT_PASS_DROP,				/* DROP (all flavors) */
						/*
						 *
						 * DROP（各种形式）
						 */
	AT_PASS_ALTER_TYPE,			/* ALTER COLUMN TYPE */
						/*
						 *
						 * ALTER COLUMN TYPE（修改列类型）
						 */
	AT_PASS_ADD_COL,			/* ADD COLUMN */
						/*
						 *
						 * ADD COLUMN（加列）
						 */
	AT_PASS_SET_EXPRESSION,		/* ALTER SET EXPRESSION */
					/*
					 *
					 * ALTER SET EXPRESSION（设置生成表达式）
					 */
	AT_PASS_OLD_INDEX,			/* re-add existing indexes */
						/*
						 *
						 * 重新加上已有索引
						 */
	AT_PASS_OLD_CONSTR,			/* re-add existing constraints */
						/*
						 *
						 * 重新加上已有约束
						 */
	/* We could support a RENAME COLUMN pass here, but not currently used */
	/*
	 *
	 * 这里本来可以加一遍 RENAME COLUMN，但目前没用到。
	 */
	AT_PASS_ADD_CONSTR,			/* ADD constraints (initial examination) */
						/*
						 *
						 * ADD 约束（初步检查）
						 */
	AT_PASS_COL_ATTRS,			/* set column attributes, eg NOT NULL */
						/*
						 *
						 * 设置列属性，例如 NOT NULL
						 */
	AT_PASS_ADD_INDEXCONSTR,	/* ADD index-based constraints */
					/*
					 *
					 * ADD 基于索引的约束
					 */
	AT_PASS_ADD_INDEX,			/* ADD indexes */
						/*
						 *
						 * ADD 索引
						 */
	AT_PASS_ADD_OTHERCONSTR,	/* ADD other constraints, defaults */
					/*
					 *
					 * ADD 其他约束与默认值
					 */
	AT_PASS_MISC,				/* other stuff */
						/*
						 *
						 * 其余操作
						 */
} AlterTablePass;

#define AT_NUM_PASSES			(AT_PASS_MISC + 1)

typedef struct AlteredTableInfo
{
	/* Information saved before any work commences: */
	/*
	 *
	 * 动手之前保存的信息：
	 */
	Oid			relid;			/* Relation to work on */
							/*
							 *
							 * 要处理的关系
							 */
	char		relkind;		/* Its relkind */
						/*
						 *
						 * 它的 relkind
						 */
	TupleDesc	oldDesc;		/* Pre-modification tuple descriptor */
						/*
						 *
						 * 修改前的元组描述符
						 */

	/*
	 * Transiently set during Phase 2, normally set to NULL.
	 *
	 * Phase 2 期间临时设置，平时为 NULL。
	 *
	 * ATRewriteCatalogs sets this when it starts, and closes when ATExecCmd
	 * returns control.  This can be exploited by ATExecCmd subroutines to
	 * close/reopen across transaction boundaries.
	 *
	 * ATRewriteCatalogs 开始时打开它，ATExecCmd 返回时关掉。ATExecCmd
	 * 的子程序可以借此在事务边界处先关再开。
	 */
	Relation	rel;

	/* Information saved by Phase 1 for Phase 2: */
	/*
	 *
	 * Phase 1 留给 Phase 2 的信息：
	 */
	List	   *subcmds[AT_NUM_PASSES]; /* Lists of AlterTableCmd */
					    /*
					     *
					     * AlterTableCmd 的列表
					     */
	/* Information saved by Phases 1/2 for Phase 3: */
	/*
	 *
	 * Phase 1/2 留给 Phase 3 的信息：
	 */
	List	   *constraints;	/* List of NewConstraint */
					/*
					 *
					 * NewConstraint 的 List
					 */
	List	   *newvals;		/* List of NewColumnValue */
					/*
					 *
					 * NewColumnValue 的 List
					 */
	List	   *afterStmts;		/* List of utility command parsetrees */
					/*
					 *
					 * 工具命令语法树的 List
					 */
	bool		verify_new_notnull; /* T if we should recheck NOT NULL */
					    /*
					     *
					     * 为真则需要重新检查 NOT NULL
					     */
	int			rewrite;		/* Reason for forced rewrite, if any */
							/*
							 *
							 * 若强制重写，这里记下原因
							 */
	bool		chgAccessMethod;	/* T if SET ACCESS METHOD is used */
						/*
						 *
						 * 为真表示用了 SET ACCESS METHOD
						 */
	Oid			newAccessMethod;	/* new access method; 0 means no change,
									 * if above is true */
							/*
							 *
							 * 新的访问方法；0 表示不变。仅当上面的标志为真时才有意义。
							 */
	Oid			newTableSpace;	/* new tablespace; 0 means no change */
						/*
						 *
						 * 新表空间；0 表示不变
						 */
	bool		chgPersistence; /* T if SET LOGGED/UNLOGGED is used */
					/*
					 *
					 * 为真表示用了 SET LOGGED/UNLOGGED
					 */
	char		newrelpersistence;	/* if above is true */
						/*
						 *
						 * 仅当上面的标志为真时有效
						 */
	Expr	   *partition_constraint;	/* for attach partition validation */
						/*
						 *
						 * 用于 ATTACH PARTITION 的校验
						 */
	/* true, if validating default due to some other attach/detach */
	/*
	 *
	 * 为真表示因别的挂接或分离而要校验默认分区
	 */
	bool		validate_default;
	/* Objects to rebuild after completing ALTER TYPE operations */
	/*
	 *
	 * ALTER TYPE 完成后要重建的对象
	 */
	List	   *changedConstraintOids;	/* OIDs of constraints to rebuild */
						/*
						 *
						 * 要重建的约束 OID
						 */
	List	   *changedConstraintDefs;	/* string definitions of same */
						/*
						 *
						 * 同上，字符串形式的定义
						 */
	List	   *changedIndexOids;	/* OIDs of indexes to rebuild */
					/*
					 *
					 * 要重建的索引 OID
					 */
	List	   *changedIndexDefs;	/* string definitions of same */
					/*
					 *
					 * 同上，字符串形式的定义
					 */
	char	   *replicaIdentityIndex;	/* index to reset as REPLICA IDENTITY */
						/*
						 *
						 * 要重新设为 REPLICA IDENTITY 的索引
						 */
	char	   *clusterOnIndex; /* index to use for CLUSTER */
				    /*
				     *
				     * 用于 CLUSTER 的索引
				     */
	List	   *changedStatisticsOids;	/* OIDs of statistics to rebuild */
						/*
						 *
						 * 要重建的统计对象 OID
						 */
	List	   *changedStatisticsDefs;	/* string definitions of same */
						/*
						 *
						 * 同上，字符串形式的定义
						 */
} AlteredTableInfo;

/* Struct describing one new constraint to check in Phase 3 scan */
/*
 *
 * 描述 Phase 3 扫描时要检查的一条新约束
 */
/* Note: new not-null constraints are handled elsewhere */
/*
 *
 * 注意：新的 NOT NULL 约束在别处处理
 */
typedef struct NewConstraint
{
	char	   *name;			/* Constraint name, or NULL if none */
						/*
						 *
						 * 约束名；没有则为 NULL
						 */
	ConstrType	contype;		/* CHECK or FOREIGN */
						/*
						 *
						 * CHECK 或 FOREIGN
						 */
	Oid			refrelid;		/* PK rel, if FOREIGN */
							/*
							 *
							 * 若是 FOREIGN，则为被引用的主键关系
							 */
	Oid			refindid;		/* OID of PK's index, if FOREIGN */
							/*
							 *
							 * 若是 FOREIGN，主键索引的 OID
							 */
	bool		conwithperiod;	/* Whether the new FOREIGN KEY uses PERIOD */
					/*
					 *
					 * 新的 FOREIGN KEY 是否使用 PERIOD
					 */
	Oid			conid;			/* OID of pg_constraint entry, if FOREIGN */
							/*
							 *
							 * 若是 FOREIGN，pg_constraint 项的 OID
							 */
	Node	   *qual;			/* Check expr or CONSTR_FOREIGN Constraint */
						/*
						 *
						 * CHECK 表达式，或 CONSTR_FOREIGN 约束
						 */
	ExprState  *qualstate;		/* Execution state for CHECK expr */
					/*
					 *
					 * CHECK 表达式的执行状态
					 */
} NewConstraint;

/*
 * Struct describing one new column value that needs to be computed during
 * Phase 3 copy (this could be either a new column with a non-null default, or
 * a column that we're changing the type of).  Columns without such an entry
 * are just copied from the old table during ATRewriteTable.  Note that the
 * expr is an expression over *old* table values, except when is_generated
 * is true; then it is an expression over columns of the *new* tuple.
 *
 * 描述 Phase 3 拷贝时需要计算的一个新列值（可能是带非空默认值的新列，也可能是正在改类型的列）。没有对应项的列在
 * ATRewriteTable 里直接从旧表拷贝。expr 是针对旧表值的表达式；但 is_generated 为真时，
 * 它是针对新元组各列的表达式。
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* which column */
						/*
						 *
						 * 哪一列
						 */
	Expr	   *expr;			/* expression to compute */
						/*
						 *
						 * 要计算的表达式
						 */
	ExprState  *exprstate;		/* execution state */
					/*
					 *
					 * 执行状态
					 */
	bool		is_generated;	/* is it a GENERATED expression? */
					/*
					 *
					 * 是否为 GENERATED 表达式
					 */
} NewColumnValue;

/*
 * Error-reporting support for RemoveRelations
 *
 * 供 RemoveRelations 报错用的辅助信息
 */
struct dropmsgstrings
{
	char		kind;
	int			nonexistent_code;
	const char *nonexistent_msg;
	const char *skipping_msg;
	const char *nota_msg;
	const char *drophint_msg;
};

static const struct dropmsgstrings dropmsgstringarray[] = {
	{RELKIND_RELATION,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("table \"%s\" does not exist"),
		gettext_noop("table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a table"),
	gettext_noop("Use DROP TABLE to remove a table.")},
	{RELKIND_SEQUENCE,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("sequence \"%s\" does not exist"),
		gettext_noop("sequence \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a sequence"),
	gettext_noop("Use DROP SEQUENCE to remove a sequence.")},
	{RELKIND_VIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("view \"%s\" does not exist"),
		gettext_noop("view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a view"),
	gettext_noop("Use DROP VIEW to remove a view.")},
	{RELKIND_MATVIEW,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("materialized view \"%s\" does not exist"),
		gettext_noop("materialized view \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a materialized view"),
	gettext_noop("Use DROP MATERIALIZED VIEW to remove a materialized view.")},
	{RELKIND_INDEX,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("index \"%s\" does not exist"),
		gettext_noop("index \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not an index"),
	gettext_noop("Use DROP INDEX to remove an index.")},
	{RELKIND_COMPOSITE_TYPE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("type \"%s\" does not exist"),
		gettext_noop("type \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a type"),
	gettext_noop("Use DROP TYPE to remove a type.")},
	{RELKIND_FOREIGN_TABLE,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("foreign table \"%s\" does not exist"),
		gettext_noop("foreign table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a foreign table"),
	gettext_noop("Use DROP FOREIGN TABLE to remove a foreign table.")},
	{RELKIND_PARTITIONED_TABLE,
		ERRCODE_UNDEFINED_TABLE,
		gettext_noop("table \"%s\" does not exist"),
		gettext_noop("table \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not a table"),
	gettext_noop("Use DROP TABLE to remove a table.")},
	{RELKIND_PARTITIONED_INDEX,
		ERRCODE_UNDEFINED_OBJECT,
		gettext_noop("index \"%s\" does not exist"),
		gettext_noop("index \"%s\" does not exist, skipping"),
		gettext_noop("\"%s\" is not an index"),
	gettext_noop("Use DROP INDEX to remove an index.")},
	{'\0', 0, NULL, NULL, NULL, NULL}
};

/* communication between RemoveRelations and RangeVarCallbackForDropRelation */
/*
 *
 * RemoveRelations 与 RangeVarCallbackForDropRelation 之间的通信数据
 */
struct DropRelationCallbackState
{
	/* These fields are set by RemoveRelations: */
	/*
	 *
	 * 这些字段由 RemoveRelations 设置：
	 */
	char		expected_relkind;
	LOCKMODE	heap_lockmode;
	/* These fields are state to track which subsidiary locks are held: */
	/*
	 *
	 * 这些字段用来记录持有哪些附属锁：
	 */
	Oid			heapOid;
	Oid			partParentOid;
	/* These fields are passed back by RangeVarCallbackForDropRelation: */
	/*
	 *
	 * 这些字段由 RangeVarCallbackForDropRelation 回传：
	 */
	char		actual_relkind;
	char		actual_relpersistence;
};

/* Alter table target-type flags for ATSimplePermissions */
/*
 *
 * 给 ATSimplePermissions 用的 ALTER TABLE 目标类型标志
 */
#define		ATT_TABLE				0x0001
#define		ATT_VIEW				0x0002
#define		ATT_MATVIEW				0x0004
#define		ATT_INDEX				0x0008
#define		ATT_COMPOSITE_TYPE		0x0010
#define		ATT_FOREIGN_TABLE		0x0020
#define		ATT_PARTITIONED_INDEX	0x0040
#define		ATT_SEQUENCE			0x0080
#define		ATT_PARTITIONED_TABLE	0x0100

/*
 * ForeignTruncateInfo
 *
 * 外部表 TRUNCATE 用的 ForeignTruncateInfo
 *
 * Information related to truncation of foreign tables.  This is used for
 * the elements in a hash table. It uses the server OID as lookup key,
 * and includes a per-server list of all foreign tables involved in the
 * truncation.
 *
 * 与截断外部表相关的信息，用作哈希表元素。以服务器 OID 为查找键，并包含该服务器上参与本次截断的全部外部表。
 */
typedef struct ForeignTruncateInfo
{
	Oid			serverid;
	List	   *rels;
} ForeignTruncateInfo;

/* Partial or complete FK creation in addFkConstraint() */
/*
 *
 * addFkConstraint() 里部分或完整地创建外键
 */
typedef enum addFkConstraintSides
{
	addFkReferencedSide,
	addFkReferencingSide,
	addFkBothSides,
} addFkConstraintSides;

/*
 * Partition tables are expected to be dropped when the parent partitioned
 * table gets dropped. Hence for partitioning we use AUTO dependency.
 * Otherwise, for regular inheritance use NORMAL dependency.
 *
 * 分区表应随父分区表一起被删除，所以分区用 AUTO 依赖。普通继承则用 NORMAL 依赖。
 */
#define child_dependency_type(child_is_partition)	\
	((child_is_partition) ? DEPENDENCY_AUTO : DEPENDENCY_NORMAL)

static void truncate_check_rel(Oid relid, Form_pg_class reltuple);
static void truncate_check_perms(Oid relid, Form_pg_class reltuple);
static void truncate_check_activity(Relation rel);
static void RangeVarCallbackForTruncate(const RangeVar *relation,
										Oid relId, Oid oldRelId, void *arg);
static List *MergeAttributes(List *columns, const List *supers, char relpersistence,
							 bool is_partition, List **supconstr,
							 List **supnotnulls);
static List *MergeCheckConstraint(List *constraints, const char *name, Node *expr, bool is_enforced);
static void MergeChildAttribute(List *inh_columns, int exist_attno, int newcol_attno, const ColumnDef *newdef);
static ColumnDef *MergeInheritedAttribute(List *inh_columns, int exist_attno, const ColumnDef *newdef);
static void MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel, bool ispartition);
static void MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel);
static void StoreCatalogInheritance(Oid relationId, List *supers,
									bool child_is_partition);
static void StoreCatalogInheritance1(Oid relationId, Oid parentOid,
									 int32 seqNumber, Relation inhRelation,
									 bool child_is_partition);
static int	findAttrByName(const char *attributeName, const List *columns);
static void AlterIndexNamespaces(Relation classRel, Relation rel,
								 Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved);
static void AlterSeqNamespaces(Relation classRel, Relation rel,
							   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved,
							   LOCKMODE lockmode);
static ObjectAddress ATExecAlterConstraint(List **wqueue, Relation rel,
										   ATAlterConstraint *cmdcon,
										   bool recurse, LOCKMODE lockmode);
static bool ATExecAlterConstraintInternal(List **wqueue, ATAlterConstraint *cmdcon, Relation conrel,
										  Relation tgrel, Relation rel, HeapTuple contuple,
										  bool recurse, LOCKMODE lockmode);
static bool ATExecAlterConstrEnforceability(List **wqueue, ATAlterConstraint *cmdcon,
											Relation conrel, Relation tgrel,
											Oid fkrelid, Oid pkrelid,
											HeapTuple contuple, LOCKMODE lockmode,
											Oid ReferencedParentDelTrigger,
											Oid ReferencedParentUpdTrigger,
											Oid ReferencingParentInsTrigger,
											Oid ReferencingParentUpdTrigger);
static bool ATExecAlterConstrDeferrability(List **wqueue, ATAlterConstraint *cmdcon,
										   Relation conrel, Relation tgrel, Relation rel,
										   HeapTuple contuple, bool recurse,
										   List **otherrelids, LOCKMODE lockmode);
static bool ATExecAlterConstrInheritability(List **wqueue, ATAlterConstraint *cmdcon,
											Relation conrel, Relation rel,
											HeapTuple contuple, LOCKMODE lockmode);
static void AlterConstrTriggerDeferrability(Oid conoid, Relation tgrel, Relation rel,
											bool deferrable, bool initdeferred,
											List **otherrelids);
static void AlterConstrEnforceabilityRecurse(List **wqueue, ATAlterConstraint *cmdcon,
											 Relation conrel, Relation tgrel,
											 Oid fkrelid, Oid pkrelid,
											 HeapTuple contuple, LOCKMODE lockmode,
											 Oid ReferencedParentDelTrigger,
											 Oid ReferencedParentUpdTrigger,
											 Oid ReferencingParentInsTrigger,
											 Oid ReferencingParentUpdTrigger);
static void AlterConstrDeferrabilityRecurse(List **wqueue, ATAlterConstraint *cmdcon,
											Relation conrel, Relation tgrel, Relation rel,
											HeapTuple contuple, bool recurse,
											List **otherrelids, LOCKMODE lockmode);
static void AlterConstrUpdateConstraintEntry(ATAlterConstraint *cmdcon, Relation conrel,
											 HeapTuple contuple);
static ObjectAddress ATExecValidateConstraint(List **wqueue,
											  Relation rel, char *constrName,
											  bool recurse, bool recursing, LOCKMODE lockmode);
static void QueueFKConstraintValidation(List **wqueue, Relation conrel, Relation fkrel,
										Oid pkrelid, HeapTuple contuple, LOCKMODE lockmode);
static void QueueCheckConstraintValidation(List **wqueue, Relation conrel, Relation rel,
										   char *constrName, HeapTuple contuple,
										   bool recurse, bool recursing, LOCKMODE lockmode);
static void QueueNNConstraintValidation(List **wqueue, Relation conrel, Relation rel,
										HeapTuple contuple, bool recurse, bool recursing,
										LOCKMODE lockmode);
static int	transformColumnNameList(Oid relId, List *colList,
									int16 *attnums, Oid *atttypids, Oid *attcollids);
static int	transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
									   List **attnamelist,
									   int16 *attnums, Oid *atttypids, Oid *attcollids,
									   Oid *opclasses, bool *pk_has_without_overlaps);
static Oid	transformFkeyCheckAttrs(Relation pkrel,
									int numattrs, int16 *attnums,
									bool with_period, Oid *opclasses,
									bool *pk_has_without_overlaps);
static void checkFkeyPermissions(Relation rel, int16 *attnums, int natts);
static CoercionPathType findFkeyCast(Oid targetTypeId, Oid sourceTypeId,
									 Oid *funcid);
static void validateForeignKeyConstraint(char *conname,
										 Relation rel, Relation pkrel,
										 Oid pkindOid, Oid constraintOid, bool hasperiod);
static void CheckAlterTableIsSafe(Relation rel);
static void ATController(AlterTableStmt *parsetree,
						 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
						 AlterTableUtilityContext *context);
static void ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  bool recurse, bool recursing, LOCKMODE lockmode,
					  AlterTableUtilityContext *context);
static void ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
							  AlterTableUtilityContext *context);
static void ATExecCmd(List **wqueue, AlteredTableInfo *tab,
					  AlterTableCmd *cmd, LOCKMODE lockmode, AlterTablePass cur_pass,
					  AlterTableUtilityContext *context);
static AlterTableCmd *ATParseTransformCmd(List **wqueue, AlteredTableInfo *tab,
										  Relation rel, AlterTableCmd *cmd,
										  bool recurse, LOCKMODE lockmode,
										  AlterTablePass cur_pass,
										  AlterTableUtilityContext *context);
static void ATRewriteTables(AlterTableStmt *parsetree,
							List **wqueue, LOCKMODE lockmode,
							AlterTableUtilityContext *context);
static void ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap);
static AlteredTableInfo *ATGetQueueEntry(List **wqueue, Relation rel);
static void ATSimplePermissions(AlterTableType cmdtype, Relation rel, int allowed_targets);
static void ATSimpleRecursion(List **wqueue, Relation rel,
							  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
							  AlterTableUtilityContext *context);
static void ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode);
static void ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
								  LOCKMODE lockmode,
								  AlterTableUtilityContext *context);
static List *find_typed_table_dependencies(Oid typeOid, const char *typeName,
										   DropBehavior behavior);
static void ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
							bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode,
							AlterTableUtilityContext *context);
static ObjectAddress ATExecAddColumn(List **wqueue, AlteredTableInfo *tab,
									 Relation rel, AlterTableCmd **cmd,
									 bool recurse, bool recursing,
									 LOCKMODE lockmode, AlterTablePass cur_pass,
									 AlterTableUtilityContext *context);
static bool check_for_column_name_collision(Relation rel, const char *colname,
											bool if_not_exists);
static void add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid);
static void add_column_collation_dependency(Oid relid, int32 attnum, Oid collid);
static ObjectAddress ATExecDropNotNull(Relation rel, const char *colName, bool recurse,
									   LOCKMODE lockmode);
static void set_attnotnull(List **wqueue, Relation rel, AttrNumber attnum,
						   bool is_valid, bool queue_validation);
static ObjectAddress ATExecSetNotNull(List **wqueue, Relation rel,
									  char *conName, char *colName,
									  bool recurse, bool recursing,
									  LOCKMODE lockmode);
static bool NotNullImpliedByRelConstraints(Relation rel, Form_pg_attribute attr);
static bool ConstraintImpliedByRelConstraint(Relation scanrel,
											 List *testConstraint, List *provenConstraint);
static ObjectAddress ATExecColumnDefault(Relation rel, const char *colName,
										 Node *newDefault, LOCKMODE lockmode);
static ObjectAddress ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
											   Node *newDefault);
static ObjectAddress ATExecAddIdentity(Relation rel, const char *colName,
									   Node *def, LOCKMODE lockmode, bool recurse, bool recursing);
static ObjectAddress ATExecSetIdentity(Relation rel, const char *colName,
									   Node *def, LOCKMODE lockmode, bool recurse, bool recursing);
static ObjectAddress ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode,
										bool recurse, bool recursing);
static ObjectAddress ATExecSetExpression(AlteredTableInfo *tab, Relation rel, const char *colName,
										 Node *newExpr, LOCKMODE lockmode);
static void ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode);
static ObjectAddress ATExecDropExpression(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode);
static ObjectAddress ATExecSetStatistics(Relation rel, const char *colName, int16 colNum,
										 Node *newValue, LOCKMODE lockmode);
static ObjectAddress ATExecSetOptions(Relation rel, const char *colName,
									  Node *options, bool isReset, LOCKMODE lockmode);
static ObjectAddress ATExecSetStorage(Relation rel, const char *colName,
									  Node *newValue, LOCKMODE lockmode);
static void ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
							 AlterTableCmd *cmd, LOCKMODE lockmode,
							 AlterTableUtilityContext *context);
static ObjectAddress ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
									  DropBehavior behavior,
									  bool recurse, bool recursing,
									  bool missing_ok, LOCKMODE lockmode,
									  ObjectAddresses *addrs);
static void ATPrepAddPrimaryKey(List **wqueue, Relation rel, AlterTableCmd *cmd,
								bool recurse, LOCKMODE lockmode,
								AlterTableUtilityContext *context);
static void verifyNotNullPKCompatible(HeapTuple tuple, const char *colname);
static ObjectAddress ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
									IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
										 CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode);
static ObjectAddress ATExecAddConstraint(List **wqueue,
										 AlteredTableInfo *tab, Relation rel,
										 Constraint *newConstraint, bool recurse, bool is_readd,
										 LOCKMODE lockmode);
static char *ChooseForeignKeyConstraintNameAddition(List *colnames);
static ObjectAddress ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
											  IndexStmt *stmt, LOCKMODE lockmode);
static ObjectAddress ATAddCheckNNConstraint(List **wqueue,
											AlteredTableInfo *tab, Relation rel,
											Constraint *constr,
											bool recurse, bool recursing, bool is_readd,
											LOCKMODE lockmode);
static ObjectAddress ATAddForeignKeyConstraint(List **wqueue, AlteredTableInfo *tab,
											   Relation rel, Constraint *fkconstraint,
											   bool recurse, bool recursing,
											   LOCKMODE lockmode);
static int	validateFkOnDeleteSetColumns(int numfks, const int16 *fkattnums,
										 int numfksetcols, int16 *fksetcolsattnums,
										 List *fksetcols);
static ObjectAddress addFkConstraint(addFkConstraintSides fkside,
									 char *constraintname,
									 Constraint *fkconstraint, Relation rel,
									 Relation pkrel, Oid indexOid,
									 Oid parentConstr,
									 int numfks, int16 *pkattnum, int16 *fkattnum,
									 Oid *pfeqoperators, Oid *ppeqoperators,
									 Oid *ffeqoperators, int numfkdelsetcols,
									 int16 *fkdelsetcols, bool is_internal,
									 bool with_period);
static void addFkRecurseReferenced(Constraint *fkconstraint,
								   Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
								   int numfks, int16 *pkattnum, int16 *fkattnum,
								   Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
								   int numfkdelsetcols, int16 *fkdelsetcols,
								   bool old_check_ok,
								   Oid parentDelTrigger, Oid parentUpdTrigger,
								   bool with_period);
static void addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint,
									Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
									int numfks, int16 *pkattnum, int16 *fkattnum,
									Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
									int numfkdelsetcols, int16 *fkdelsetcols,
									bool old_check_ok, LOCKMODE lockmode,
									Oid parentInsTrigger, Oid parentUpdTrigger,
									bool with_period);
static void CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
									   Relation partitionRel);
static void CloneFkReferenced(Relation parentRel, Relation partitionRel);
static void CloneFkReferencing(List **wqueue, Relation parentRel,
							   Relation partRel);
static void createForeignKeyCheckTriggers(Oid myRelOid, Oid refRelOid,
										  Constraint *fkconstraint, Oid constraintOid,
										  Oid indexOid,
										  Oid parentInsTrigger, Oid parentUpdTrigger,
										  Oid *insertTrigOid, Oid *updateTrigOid);
static void createForeignKeyActionTriggers(Oid myRelOid, Oid refRelOid,
										   Constraint *fkconstraint, Oid constraintOid,
										   Oid indexOid,
										   Oid parentDelTrigger, Oid parentUpdTrigger,
										   Oid *deleteTrigOid, Oid *updateTrigOid);
static bool tryAttachPartitionForeignKey(List **wqueue,
										 ForeignKeyCacheInfo *fk,
										 Relation partition,
										 Oid parentConstrOid, int numfks,
										 AttrNumber *mapped_conkey, AttrNumber *confkey,
										 Oid *conpfeqop,
										 Oid parentInsTrigger,
										 Oid parentUpdTrigger,
										 Relation trigrel);
static void AttachPartitionForeignKey(List **wqueue, Relation partition,
									  Oid partConstrOid, Oid parentConstrOid,
									  Oid parentInsTrigger, Oid parentUpdTrigger,
									  Relation trigrel);
static void RemoveInheritedConstraint(Relation conrel, Relation trigrel,
									  Oid conoid, Oid conrelid);
static void DropForeignKeyConstraintTriggers(Relation trigrel, Oid conoid,
											 Oid confrelid, Oid conrelid);
static void GetForeignKeyActionTriggers(Relation trigrel,
										Oid conoid, Oid confrelid, Oid conrelid,
										Oid *deleteTriggerOid,
										Oid *updateTriggerOid);
static void GetForeignKeyCheckTriggers(Relation trigrel,
									   Oid conoid, Oid confrelid, Oid conrelid,
									   Oid *insertTriggerOid,
									   Oid *updateTriggerOid);
static void ATExecDropConstraint(Relation rel, const char *constrName,
								 DropBehavior behavior, bool recurse,
								 bool missing_ok, LOCKMODE lockmode);
static ObjectAddress dropconstraint_internal(Relation rel,
											 HeapTuple constraintTup, DropBehavior behavior,
											 bool recurse, bool recursing,
											 bool missing_ok, LOCKMODE lockmode);
static void ATPrepAlterColumnType(List **wqueue,
								  AlteredTableInfo *tab, Relation rel,
								  bool recurse, bool recursing,
								  AlterTableCmd *cmd, LOCKMODE lockmode,
								  AlterTableUtilityContext *context);
static bool ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno);
static ObjectAddress ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
										   AlterTableCmd *cmd, LOCKMODE lockmode);
static void RememberAllDependentForRebuilding(AlteredTableInfo *tab, AlterTableType subtype,
											  Relation rel, AttrNumber attnum, const char *colName);
static void RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab);
static void RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab);
static void RememberStatisticsForRebuilding(Oid stxoid, AlteredTableInfo *tab);
static void ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab,
								   LOCKMODE lockmode);
static void ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId,
								 char *cmd, List **wqueue, LOCKMODE lockmode,
								 bool rewrite);
static void RebuildConstraintComment(AlteredTableInfo *tab, AlterTablePass pass,
									 Oid objid, Relation rel, List *domname,
									 const char *conname);
static void TryReuseIndex(Oid oldId, IndexStmt *stmt);
static void TryReuseForeignKey(Oid oldId, Constraint *con);
static ObjectAddress ATExecAlterColumnGenericOptions(Relation rel, const char *colName,
													 List *options, LOCKMODE lockmode);
static void change_owner_fix_column_acls(Oid relationOid,
										 Oid oldOwnerId, Oid newOwnerId);
static void change_owner_recurse_to_sequences(Oid relationOid,
											  Oid newOwnerId, LOCKMODE lockmode);
static ObjectAddress ATExecClusterOn(Relation rel, const char *indexName,
									 LOCKMODE lockmode);
static void ATExecDropCluster(Relation rel, LOCKMODE lockmode);
static void ATPrepSetAccessMethod(AlteredTableInfo *tab, Relation rel, const char *amname);
static void ATExecSetAccessMethodNoStorage(Relation rel, Oid newAccessMethodId);
static void ATPrepChangePersistence(AlteredTableInfo *tab, Relation rel,
									bool toLogged);
static void ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel,
								const char *tablespacename, LOCKMODE lockmode);
static void ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode);
static void ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace);
static void ATExecSetRelOptions(Relation rel, List *defList,
								AlterTableType operation,
								LOCKMODE lockmode);
static void ATExecEnableDisableTrigger(Relation rel, const char *trigname,
									   char fires_when, bool skip_system, bool recurse,
									   LOCKMODE lockmode);
static void ATExecEnableDisableRule(Relation rel, const char *rulename,
									char fires_when, LOCKMODE lockmode);
static void ATPrepAddInherit(Relation child_rel);
static ObjectAddress ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode);
static ObjectAddress ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode);
static void drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid,
								   DependencyType deptype);
static ObjectAddress ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode);
static void ATExecDropOf(Relation rel, LOCKMODE lockmode);
static void ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode);
static void ATExecGenericOptions(Relation rel, List *options);
static void ATExecSetRowSecurity(Relation rel, bool rls);
static void ATExecForceNoForceRowSecurity(Relation rel, bool force_rls);
static ObjectAddress ATExecSetCompression(Relation rel,
										  const char *column, Node *newValue, LOCKMODE lockmode);

static void index_copy_data(Relation rel, RelFileLocator newrlocator);
static const char *storage_name(char c);

static void RangeVarCallbackForDropRelation(const RangeVar *rel, Oid relOid,
											Oid oldRelOid, void *arg);
static void RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid,
											 Oid oldrelid, void *arg);
static PartitionSpec *transformPartitionSpec(Relation rel, PartitionSpec *partspec);
static void ComputePartitionAttrs(ParseState *pstate, Relation rel, List *partParams, AttrNumber *partattrs,
								  List **partexprs, Oid *partopclass, Oid *partcollation,
								  PartitionStrategy strategy);
static void CreateInheritance(Relation child_rel, Relation parent_rel, bool ispartition);
static void RemoveInheritance(Relation child_rel, Relation parent_rel,
							  bool expect_detached);
static ObjectAddress ATExecAttachPartition(List **wqueue, Relation rel,
										   PartitionCmd *cmd,
										   AlterTableUtilityContext *context);
static void AttachPartitionEnsureIndexes(List **wqueue, Relation rel, Relation attachrel);
static void QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
											   List *partConstraint,
											   bool validate_default);
static void CloneRowTriggersToPartition(Relation parent, Relation partition);
static void DetachAddConstraintIfNeeded(List **wqueue, Relation partRel);
static void DropClonedTriggersFromPartition(Oid partitionId);
static ObjectAddress ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab,
										   Relation rel, RangeVar *name,
										   bool concurrent);
static void DetachPartitionFinalize(Relation rel, Relation partRel,
									bool concurrent, Oid defaultPartOid);
static ObjectAddress ATExecDetachPartitionFinalize(Relation rel, RangeVar *name);
static ObjectAddress ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx,
											  RangeVar *name);
static void validatePartitionedIndex(Relation partedIdx, Relation partedTbl);
static void refuseDupeIndexAttach(Relation parentIdx, Relation partIdx,
								  Relation partitionTbl);
static void verifyPartitionIndexNotNull(IndexInfo *iinfo, Relation partition);
static List *GetParentedForeignKeyRefs(Relation partition);
static void ATDetachCheckNoForeignKeyRefs(Relation partition);
static char GetAttributeCompression(Oid atttypid, const char *compression);
static char GetAttributeStorage(Oid atttypid, const char *storagemode);


/* ----------------------------------------------------------------
 *		DefineRelation
 *				Creates a new relation.
 *
 * DefineRelation：创建新关系。
 *
 * stmt carries parsetree information from an ordinary CREATE TABLE statement.
 * The other arguments are used to extend the behavior for other cases:
 * relkind: relkind to assign to the new relation
 * ownerId: if not InvalidOid, use this as the new relation's owner.
 * typaddress: if not null, it's set to the pg_type entry's address.
 * queryString: for error reporting
 *
 * stmt 带着普通 CREATE TABLE 语句的语法树。其余参数用来扩展其他场景：relkind 是赋给新关系的
 * relkind；ownerId 若不是 InvalidOid，就用作新关系的属主；typaddress 非空时，会被设为
 * pg_type 项的地址；queryString 用于报错。
 *
 * Note that permissions checks are done against current user regardless of
 * ownerId.  A nonzero ownerId is used when someone is creating a relation
 * "on behalf of" someone else, so we still want to see that the current user
 * has permissions to do it.
 *
 * 注意：权限检查始终针对当前用户，与 ownerId 无关。非零 ownerId 表示替别人创建关系，此时仍要确认当前用户有权这么做。
 *
 * If successful, returns the address of the new relation.
 *
 * 成功时返回新关系的地址。
 * ----------------------------------------------------------------
 */
ObjectAddress
DefineRelation(CreateStmt *stmt, char relkind, Oid ownerId,
			   ObjectAddress *typaddress, const char *queryString)
{
	char		relname[NAMEDATALEN];
	Oid			namespaceId;
	Oid			relationId;
	Oid			tablespaceId;
	Relation	rel;
	TupleDesc	descriptor;
	List	   *inheritOids;
	List	   *old_constraints;
	List	   *old_notnulls;
	List	   *rawDefaults;
	List	   *cookedDefaults;
	List	   *nncols;
	List	   *connames = NIL;
	Datum		reloptions;
	ListCell   *listptr;
	AttrNumber	attnum;
	bool		partitioned;
	const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;
	Oid			ofTypeId;
	ObjectAddress address;
	LOCKMODE	parentLockmode;
	Oid			accessMethodId = InvalidOid;

	/*
	 * Truncate relname to appropriate length (probably a waste of time, as
	 * parser should have done this already).
	 *
	 * 把 relname 截到合法长度（解析器多半已经做过，这里可能是多余的）。
	 */
	strlcpy(relname, stmt->relation->relname, NAMEDATALEN);

	/*
	 * Check consistency of arguments
	 *
	 * 检查参数是否一致
	 */
	if (stmt->oncommit != ONCOMMIT_NOOP
		&& stmt->relation->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("ON COMMIT can only be used on temporary tables")));

	if (stmt->partspec != NULL)
	{
		if (relkind != RELKIND_RELATION)
			elog(ERROR, "unexpected relkind: %d", (int) relkind);

		relkind = RELKIND_PARTITIONED_TABLE;
		partitioned = true;
	}
	else
		partitioned = false;

	if (relkind == RELKIND_PARTITIONED_TABLE &&
		stmt->relation->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("partitioned tables cannot be unlogged")));

	/*
	 * Look up the namespace in which we are supposed to create the relation,
	 * check we have permission to create there, lock it against concurrent
	 * drop, and mark stmt->relation as RELPERSISTENCE_TEMP if a temporary
	 * namespace is selected.
	 *
	 * 查找要在其中创建关系的命名空间，检查是否有创建权限，加锁防止并发删除；若选中的是临时命名空间，把 stmt->relation
	 * 标成 RELPERSISTENCE_TEMP。
	 */
	namespaceId =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, NULL);

	/*
	 * Security check: disallow creating temp tables from security-restricted
	 * code.  This is needed because calling code might not expect untrusted
	 * tables to appear in pg_temp at the front of its search path.
	 *
	 * 安全检查：不允许在安全受限代码里创建临时表。否则调用方可能没料到不可信的表会出现在搜索路径最前面的 pg_temp 中。
	 */
	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP
		&& InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create temporary table within security-restricted operation")));

	/*
	 * Determine the lockmode to use when scanning parents.  A self-exclusive
	 * lock is needed here.
	 *
	 * 确定扫描父表时用的锁模式。这里需要自排他锁。
	 *
	 * For regular inheritance, if two backends attempt to add children to the
	 * same parent simultaneously, and that parent has no pre-existing
	 * children, then both will attempt to update the parent's relhassubclass
	 * field, leading to a "tuple concurrently updated" error.  Also, this
	 * interlocks against a concurrent ANALYZE on the parent table, which
	 * might otherwise be attempting to clear the parent's relhassubclass
	 * field, if its previous children were recently dropped.
	 *
	 * 普通继承时，若两个后端同时给同一个还没有子表的父表加孩子，双方都会去改父表的 relhassubclass，从而报出 tuple
	 * concurrently updated。这把锁也用来和父表上并发的 ANALYZE 互斥，否则 ANALYZE
	 * 可能在最近的子表刚被删除后试图清掉父表的 relhassubclass。
	 *
	 * If the child table is a partition, then we instead grab an exclusive
	 * lock on the parent because its partition descriptor will be changed by
	 * addition of the new partition.
	 *
	 * 若子表是分区，则改为对父表加排他锁，因为加入新分区会改父表的分区描述符。
	 */
	parentLockmode = (stmt->partbound != NULL ? AccessExclusiveLock :
					  ShareUpdateExclusiveLock);

	/* Determine the list of OIDs of the parents. */
	/*
	 *
	 * 确定父表 OID 列表。
	 */
	inheritOids = NIL;
	foreach(listptr, stmt->inhRelations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(listptr);
		Oid			parentOid;

		parentOid = RangeVarGetRelid(rv, parentLockmode, false);

		/*
		 * Reject duplications in the list of parents.
		 *
		 * 拒绝父表列表里的重复项。
		 */
		if (list_member_oid(inheritOids, parentOid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" would be inherited from more than once",
							get_rel_name(parentOid))));

		inheritOids = lappend_oid(inheritOids, parentOid);
	}

	/*
	 * Select tablespace to use: an explicitly indicated one, or (in the case
	 * of a partitioned table) the parent's, if it has one.
	 *
	 * 选择表空间：显式指定的那个；若是分区表且父表有表空间，则用父表的。
	 */
	if (stmt->tablespacename)
	{
		tablespaceId = get_tablespace_oid(stmt->tablespacename, false);

		if (partitioned && tablespaceId == MyDatabaseTableSpace)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
	}
	else if (stmt->partbound)
	{
		Assert(list_length(inheritOids) == 1);
		tablespaceId = get_rel_tablespace(linitial_oid(inheritOids));
	}
	else
		tablespaceId = InvalidOid;

	/* still nothing? use the default */
	/*
	 *
	 * 还是没有？那就用默认表空间
	 */
	if (!OidIsValid(tablespaceId))
		tablespaceId = GetDefaultTablespace(stmt->relation->relpersistence,
											partitioned);

	/* Check permissions except when using database's default */
	/*
	 *
	 * 除非用的是数据库默认表空间，否则检查权限
	 */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/* In all cases disallow placing user relations in pg_global */
	/*
	 *
	 * 任何情况下都不允许把用户关系放到 pg_global
	 */
	if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/* Identify user ID that will own the table */
	/*
	 *
	 * 确定将拥有该表的用户 ID
	 */
	if (!OidIsValid(ownerId))
		ownerId = GetUserId();

	/*
	 * Parse and validate reloptions, if any.
	 *
	 * 若有 reloptions，解析并校验。
	 */
	reloptions = transformRelOptions((Datum) 0, stmt->options, NULL, validnsps,
									 true, false);

	switch (relkind)
	{
		case RELKIND_VIEW:
			(void) view_reloptions(reloptions, true);
			break;
		case RELKIND_PARTITIONED_TABLE:
			(void) partitioned_table_reloptions(reloptions, true);
			break;
		default:
			(void) heap_reloptions(relkind, reloptions, true);
	}

	if (stmt->ofTypename)
	{
		AclResult	aclresult;

		ofTypeId = typenameTypeId(NULL, stmt->ofTypename);

		aclresult = object_aclcheck(TypeRelationId, ofTypeId, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, ofTypeId);
	}
	else
		ofTypeId = InvalidOid;

	/*
	 * Look up inheritance ancestors and generate relation schema, including
	 * inherited attributes.  (Note that stmt->tableElts is destructively
	 * modified by MergeAttributes.)
	 *
	 * 查找继承祖先并生成关系模式，包括继承来的属性。（注意 MergeAttributes 会破坏性地修改
	 * stmt->tableElts。）
	 */
	stmt->tableElts =
		MergeAttributes(stmt->tableElts, inheritOids,
						stmt->relation->relpersistence,
						stmt->partbound != NULL,
						&old_constraints, &old_notnulls);

	/*
	 * Create a tuple descriptor from the relation schema.  Note that this
	 * deals with column names, types, and in-descriptor NOT NULL flags, but
	 * not default values, NOT NULL or CHECK constraints; we handle those
	 * below.
	 *
	 * 由关系模式创建元组描述符。这里处理列名、类型以及描述符里的 NOT NULL 标志，但不处理默认值、NOT NULL 或
	 * CHECK 约束；那些在下面处理。
	 */
	descriptor = BuildDescForRelation(stmt->tableElts);

	/*
	 * Find columns with default values and prepare for insertion of the
	 * defaults.  Pre-cooked (that is, inherited) defaults go into a list of
	 * CookedConstraint structs that we'll pass to heap_create_with_catalog,
	 * while raw defaults go into a list of RawColumnDefault structs that will
	 * be processed by AddRelationNewConstraints.  (We can't deal with raw
	 * expressions until we can do transformExpr.)
	 *
	 * 找出带默认值的列，并准备插入这些默认值。已经煮好的（也就是继承来的）默认值放进 CookedConstraint 列表，交给
	 * heap_create_with_catalog；原始默认值放进 RawColumnDefault 列表，由
	 * AddRelationNewConstraints 处理。（在能做 transformExpr 之前，没法处理原始表达式。）
	 */
	rawDefaults = NIL;
	cookedDefaults = NIL;
	attnum = 0;

	foreach(listptr, stmt->tableElts)
	{
		ColumnDef  *colDef = lfirst(listptr);

		attnum++;
		if (colDef->raw_default != NULL)
		{
			RawColumnDefault *rawEnt;

			Assert(colDef->cooked_default == NULL);

			rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
			rawEnt->attnum = attnum;
			rawEnt->raw_default = colDef->raw_default;
			rawEnt->generated = colDef->generated;
			rawDefaults = lappend(rawDefaults, rawEnt);
		}
		else if (colDef->cooked_default != NULL)
		{
			CookedConstraint *cooked;

			cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
			cooked->contype = CONSTR_DEFAULT;
			cooked->conoid = InvalidOid;	/* until created */
							/*
							 *
							 * 创建完成之前
							 */
			cooked->name = NULL;
			cooked->attnum = attnum;
			cooked->expr = colDef->cooked_default;
			cooked->is_enforced = true;
			cooked->skip_validation = false;
			cooked->is_local = true;	/* not used for defaults */
							/*
							 *
							 * 默认值用不到
							 */
			cooked->inhcount = 0;	/* ditto */
						/*
						 *
						 * 同上
						 */
			cooked->is_no_inherit = false;
			cookedDefaults = lappend(cookedDefaults, cooked);
		}
	}

	/*
	 * For relations with table AM and partitioned tables, select access
	 * method to use: an explicitly indicated one, or (in the case of a
	 * partitioned table) the parent's, if it has one.
	 *
	 * 对有表访问方法的关系以及分区表，选择访问方法：显式指定的那个；若是分区表且父表有访问方法，则用父表的。
	 */
	if (stmt->accessMethod != NULL)
	{
		Assert(RELKIND_HAS_TABLE_AM(relkind) || relkind == RELKIND_PARTITIONED_TABLE);
		accessMethodId = get_table_am_oid(stmt->accessMethod, false);
	}
	else if (RELKIND_HAS_TABLE_AM(relkind) || relkind == RELKIND_PARTITIONED_TABLE)
	{
		if (stmt->partbound)
		{
			Assert(list_length(inheritOids) == 1);
			accessMethodId = get_rel_relam(linitial_oid(inheritOids));
		}

		if (RELKIND_HAS_TABLE_AM(relkind) && !OidIsValid(accessMethodId))
			accessMethodId = get_table_am_oid(default_table_access_method, false);
	}

	/*
	 * Create the relation.  Inherited defaults and CHECK constraints are
	 * passed in for immediate handling --- since they don't need parsing,
	 * they can be stored immediately.
	 *
	 * 创建关系。继承来的默认值和 CHECK 约束立刻处理，它们不需要再解析，可以直接存进去。
	 */
	relationId = heap_create_with_catalog(relname,
										  namespaceId,
										  tablespaceId,
										  InvalidOid,
										  InvalidOid,
										  ofTypeId,
										  ownerId,
										  accessMethodId,
										  descriptor,
										  list_concat(cookedDefaults,
													  old_constraints),
										  relkind,
										  stmt->relation->relpersistence,
										  false,
										  false,
										  stmt->oncommit,
										  reloptions,
										  true,
										  allowSystemTableMods,
										  false,
										  InvalidOid,
										  typaddress);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 *
	 * 必须推进命令计数器，新创建的关系元组才能在打开时被看见。
	 */
	CommandCounterIncrement();

	/*
	 * Open the new relation and acquire exclusive lock on it.  This isn't
	 * really necessary for locking out other backends (since they can't see
	 * the new rel anyway until we commit), but it keeps the lock manager from
	 * complaining about deadlock risks.
	 *
	 * 打开新关系并加上排他锁。这并不是为了挡住其他后端（提交前它们本来就看不见新关系），而是免得锁管理器抱怨死锁风险。
	 */
	rel = relation_open(relationId, AccessExclusiveLock);

	/*
	 * Now add any newly specified column default and generation expressions
	 * to the new relation.  These are passed to us in the form of raw
	 * parsetrees; we need to transform them to executable expression trees
	 * before they can be added. The most convenient way to do that is to
	 * apply the parser's transformExpr routine, but transformExpr doesn't
	 * work unless we have a pre-existing relation. So, the transformation has
	 * to be postponed to this final step of CREATE TABLE.
	 *
	 * 现在把新指定的列默认值和生成表达式加到新关系上。传进来的是原始语法树，入库前要变成可执行表达式树。最方便的办法是用解析器的
	 * transformExpr，但它必须已有关系才能工作。所以变换只能留到 CREATE TABLE 的这最后一步。
	 *
	 * This needs to be before processing the partitioning clauses because
	 * those could refer to generated columns.
	 *
	 * 必须在处理分区子句之前做，因为分区子句可能引用生成列。
	 */
	if (rawDefaults)
		AddRelationNewConstraints(rel, rawDefaults, NIL,
								  true, true, false, queryString);

	/*
	 * Make column generation expressions visible for use by partitioning.
	 *
	 * 让列的生成表达式对分区处理可见。
	 */
	CommandCounterIncrement();

	/* Process and store partition bound, if any. */
	/*
	 *
	 * 若有分区边界，处理并存入。
	 */
	if (stmt->partbound)
	{
		PartitionBoundSpec *bound;
		ParseState *pstate;
		Oid			parentId = linitial_oid(inheritOids),
					defaultPartOid;
		Relation	parent,
					defaultRel = NULL;
		ParseNamespaceItem *nsitem;

		/* Already have strong enough lock on the parent */
		/*
		 *
		 * 父表上已经有足够强的锁
		 */
		parent = table_open(parentId, NoLock);

		/*
		 * We are going to try to validate the partition bound specification
		 * against the partition key of parentRel, so it better have one.
		 *
		 * 接下来要用 parentRel 的分区键校验分区边界，所以它必须有分区键。
		 */
		if (parent->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("\"%s\" is not partitioned",
							RelationGetRelationName(parent))));

		/*
		 * The partition constraint of the default partition depends on the
		 * partition bounds of every other partition. It is possible that
		 * another backend might be about to execute a query on the default
		 * partition table, and that the query relies on previously cached
		 * default partition constraints. We must therefore take a table lock
		 * strong enough to prevent all queries on the default partition from
		 * proceeding until we commit and send out a shared-cache-inval notice
		 * that will make them update their index lists.
		 *
		 * 默认分区的分区约束依赖于其他每个分区的边界。可能有另一个后端正要对默认分区执行查询，而查询依赖先前缓存的默认分区约束。
		 * 因此必须加一把足够强的表锁，在我们提交并发出共享缓存失效通知、让它们更新索引列表之前，挡住默认分区上的所有查询。
		 *
		 * Order of locking: The relation being added won't be visible to
		 * other backends until it is committed, hence here in
		 * DefineRelation() the order of locking the default partition and the
		 * relation being added does not matter. But at all other places we
		 * need to lock the default relation before we lock the relation being
		 * added or removed i.e. we should take the lock in same order at all
		 * the places such that lock parent, lock default partition and then
		 * lock the partition so as to avoid a deadlock.
		 *
		 * 加锁顺序：正在加入的关系在提交前对其他后端不可见，所以在 DefineRelation() 里先锁默认分区还是先锁新关系无所谓。
		 * 但其他地方必须先锁默认分区，再锁要加入或移除的关系。各处都应保持同一顺序：先锁父表，再锁默认分区，然后锁该分区，以免死锁。
		 */
		defaultPartOid =
			get_default_oid_from_partdesc(RelationGetPartitionDesc(parent,
																   true));
		if (OidIsValid(defaultPartOid))
			defaultRel = table_open(defaultPartOid, AccessExclusiveLock);

		/* Transform the bound values */
		/*
		 *
		 * 变换边界值
		 */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * Add an nsitem containing this relation, so that transformExpr
		 * called on partition bound expressions is able to report errors
		 * using a proper context.
		 *
		 * 加一个包含本关系的 nsitem，这样对分区边界表达式调用 transformExpr 时才能用正确的上下文报错。
		 */
		nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
											   NULL, false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		bound = transformPartitionBound(pstate, parent, stmt->partbound);

		/*
		 * Check first that the new partition's bound is valid and does not
		 * overlap with any of existing partitions of the parent.
		 *
		 * 先检查新分区的边界合法，且不与父表已有分区重叠。
		 */
		check_new_partition_bound(relname, parent, bound, pstate);

		/*
		 * If the default partition exists, its partition constraints will
		 * change after the addition of this new partition such that it won't
		 * allow any row that qualifies for this new partition. So, check that
		 * the existing data in the default partition satisfies the constraint
		 * as it will exist after adding this partition.
		 *
		 * 若存在默认分区，加入这个新分区后它的分区约束会收紧，不再允许属于新分区的行。因此要检查默认分区里的现有数据满足加入本分区之后的约束。
		 */
		if (OidIsValid(defaultPartOid))
		{
			check_default_partition_contents(parent, defaultRel, bound);
			/* Keep the lock until commit. */
			/*
			 *
			 * 锁保持到提交。
			 */
			table_close(defaultRel, NoLock);
		}

		/* Update the pg_class entry. */
		/*
		 *
		 * 更新 pg_class 项。
		 */
		StorePartitionBound(rel, parent, bound);

		table_close(parent, NoLock);
	}

	/* Store inheritance information for new rel. */
	/*
	 *
	 * 为新关系保存继承信息。
	 */
	StoreCatalogInheritance(relationId, inheritOids, stmt->partbound != NULL);

	/*
	 * Process the partitioning specification (if any) and store the partition
	 * key information into the catalog.
	 *
	 * 处理分区说明（若有），并把分区键信息写入系统目录。
	 */
	if (partitioned)
	{
		ParseState *pstate;
		int			partnatts;
		AttrNumber	partattrs[PARTITION_MAX_KEYS];
		Oid			partopclass[PARTITION_MAX_KEYS];
		Oid			partcollation[PARTITION_MAX_KEYS];
		List	   *partexprs = NIL;

		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		partnatts = list_length(stmt->partspec->partParams);

		/* Protect fixed-size arrays here and in executor */
		/*
		 *
		 * 保护这里以及执行器里的定长数组
		 */
		if (partnatts > PARTITION_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot partition using more than %d columns",
							PARTITION_MAX_KEYS)));

		/*
		 * We need to transform the raw parsetrees corresponding to partition
		 * expressions into executable expression trees.  Like column defaults
		 * and CHECK constraints, we could not have done the transformation
		 * earlier.
		 *
		 * 需要把分区表达式对应的原始语法树变成可执行表达式树。和列默认值、CHECK 约束一样，这一步不能更早做。
		 */
		stmt->partspec = transformPartitionSpec(rel, stmt->partspec);

		ComputePartitionAttrs(pstate, rel, stmt->partspec->partParams,
							  partattrs, &partexprs, partopclass,
							  partcollation, stmt->partspec->strategy);

		StorePartitionKey(rel, stmt->partspec->strategy, partnatts, partattrs,
						  partexprs,
						  partopclass, partcollation);

		/* make it all visible */
		/*
		 *
		 * 使这些修改全部可见
		 */
		CommandCounterIncrement();
	}

	/*
	 * If we're creating a partition, create now all the indexes, triggers,
	 * FKs defined in the parent.
	 *
	 * 若正在创建分区，现在就把父表上定义的索引、触发器和外键都建出来。
	 *
	 * We can't do it earlier, because DefineIndex wants to know the partition
	 * key which we just stored.
	 *
	 * 不能再早，因为 DefineIndex 需要刚存进去的分区键。
	 */
	if (stmt->partbound)
	{
		Oid			parentId = linitial_oid(inheritOids);
		Relation	parent;
		List	   *idxlist;
		ListCell   *cell;

		/* Already have strong enough lock on the parent */
		/*
		 *
		 * 父表上已经有足够强的锁
		 */
		parent = table_open(parentId, NoLock);
		idxlist = RelationGetIndexList(parent);

		/*
		 * For each index in the parent table, create one in the partition
		 *
		 * 父表上的每个索引，都在分区上建一个
		 */
		foreach(cell, idxlist)
		{
			Relation	idxRel = index_open(lfirst_oid(cell), AccessShareLock);
			AttrMap    *attmap;
			IndexStmt  *idxstmt;
			Oid			constraintOid;

			if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			{
				if (idxRel->rd_index->indisunique)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot create foreign partition of partitioned table \"%s\"",
									RelationGetRelationName(parent)),
							 errdetail("Table \"%s\" contains indexes that are unique.",
									   RelationGetRelationName(parent))));
				else
				{
					index_close(idxRel, AccessShareLock);
					continue;
				}
			}

			attmap = build_attrmap_by_name(RelationGetDescr(rel),
										   RelationGetDescr(parent),
										   false);
			idxstmt =
				generateClonedIndexStmt(NULL, idxRel,
										attmap, &constraintOid);
			DefineIndex(RelationGetRelid(rel),
						idxstmt,
						InvalidOid,
						RelationGetRelid(idxRel),
						constraintOid,
						-1,
						false, false, false, false, false);

			index_close(idxRel, AccessShareLock);
		}

		list_free(idxlist);

		/*
		 * If there are any row-level triggers, clone them to the new
		 * partition.
		 *
		 * 若有行级触发器，克隆到新分区。
		 */
		if (parent->trigdesc != NULL)
			CloneRowTriggersToPartition(parent, rel);

		/*
		 * And foreign keys too.  Note that because we're freshly creating the
		 * table, there is no need to verify these new constraints.
		 *
		 * 外键也一样。因为表是刚创建的，不必校验这些新约束。
		 */
		CloneForeignKeyConstraints(NULL, parent, rel);

		table_close(parent, NoLock);
	}

	/*
	 * Now add any newly specified CHECK constraints to the new relation. Same
	 * as for defaults above, but these need to come after partitioning is set
	 * up.  We save the constraint names that were used, to avoid dupes below.
	 *
	 * 现在把新指定的 CHECK 约束加到新关系上。和上面的默认值类似，但必须在分区设置完成之后。记下用过的约束名，避免下面重复。
	 */
	if (stmt->constraints)
	{
		List	   *conlist;

		conlist = AddRelationNewConstraints(rel, NIL, stmt->constraints,
											true, true, false, queryString);
		foreach_ptr(CookedConstraint, cons, conlist)
		{
			if (cons->name != NULL)
				connames = lappend(connames, cons->name);
		}
	}

	/*
	 * Finally, merge the not-null constraints that are declared directly with
	 * those that come from parent relations (making sure to count inheritance
	 * appropriately for each), create them, and set the attnotnull flag on
	 * columns that don't yet have it.
	 *
	 * 最后，把直接声明的 NOT NULL 约束和来自父关系的合并（每种都按继承关系正确计数），创建它们，并给还没有
	 * attnotnull 标志的列置上。
	 */
	nncols = AddRelationNotNullConstraints(rel, stmt->nnconstraints,
										   old_notnulls, connames);
	foreach_int(attrnum, nncols)
		set_attnotnull(NULL, rel, attrnum, true, false);

	ObjectAddressSet(address, RelationRelationId, relationId);

	/*
	 * Clean up.  We keep lock on new relation (although it shouldn't be
	 * visible to anyone else anyway, until commit).
	 *
	 * 清理。新关系上的锁继续持有（提交前别人本来也看不见它）。
	 */
	relation_close(rel, NoLock);

	return address;
}

/*
 * BuildDescForRelation
 *
 * 函数 BuildDescForRelation
 *
 * Given a list of ColumnDef nodes, build a TupleDesc.
 *
 * 根据 ColumnDef 节点列表构造 TupleDesc。
 *
 * Note: This is only for the limited purpose of table and view creation.  Not
 * everything is filled in.  A real tuple descriptor should be obtained from
 * the relcache.
 *
 * 注意：这只服务于建表和建视图这个有限用途，并没有填全所有字段。真正的元组描述符应从 relcache 获取。
 */
TupleDesc
BuildDescForRelation(const List *columns)
{
	int			natts;
	AttrNumber	attnum;
	ListCell   *l;
	TupleDesc	desc;
	char	   *attname;
	Oid			atttypid;
	int32		atttypmod;
	Oid			attcollation;
	int			attdim;

	/*
	 * allocate a new tuple descriptor
	 *
	 * 分配一个新的元组描述符
	 */
	natts = list_length(columns);
	desc = CreateTemplateTupleDesc(natts);

	attnum = 0;

	foreach(l, columns)
	{
		ColumnDef  *entry = lfirst(l);
		AclResult	aclresult;
		Form_pg_attribute att;

		/*
		 * for each entry in the list, get the name and type information from
		 * the list and have TupleDescInitEntry fill in the attribute
		 * information we need.
		 *
		 * 对列表中每一项，取出名字和类型信息，让 TupleDescInitEntry 填好需要的属性信息。
		 */
		attnum++;

		attname = entry->colname;
		typenameTypeIdAndMod(NULL, entry->typeName, &atttypid, &atttypmod);

		aclresult = object_aclcheck(TypeRelationId, atttypid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, atttypid);

		attcollation = GetColumnDefCollation(NULL, entry, atttypid);
		attdim = list_length(entry->typeName->arrayBounds);
		if (attdim > PG_INT16_MAX)
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("too many array dimensions"));

		if (entry->typeName->setof)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" cannot be declared SETOF",
							attname)));

		TupleDescInitEntry(desc, attnum, attname,
						   atttypid, atttypmod, attdim);
		att = TupleDescAttr(desc, attnum - 1);

		/* Override TupleDescInitEntry's settings as requested */
		/*
		 *
		 * 按要求覆盖 TupleDescInitEntry 的设置
		 */
		TupleDescInitEntryCollation(desc, attnum, attcollation);

		/* Fill in additional stuff not handled by TupleDescInitEntry */
		/*
		 *
		 * 补上 TupleDescInitEntry 没处理的其余字段
		 */
		att->attnotnull = entry->is_not_null;
		att->attislocal = entry->is_local;
		att->attinhcount = entry->inhcount;
		att->attidentity = entry->identity;
		att->attgenerated = entry->generated;
		att->attcompression = GetAttributeCompression(att->atttypid, entry->compression);
		if (entry->storage)
			att->attstorage = entry->storage;
		else if (entry->storage_name)
			att->attstorage = GetAttributeStorage(att->atttypid, entry->storage_name);

		populate_compact_attribute(desc, attnum - 1);
	}

	return desc;
}

/*
 * Emit the right error or warning message for a "DROP" command issued on a
 * non-existent relation
 *
 * 对不存在的关系执行 DROP 时，发出对应的错误或警告。
 */
static void
DropErrorMsgNonExistent(RangeVar *rel, char rightkind, bool missing_ok)
{
	const struct dropmsgstrings *rentry;

	if (rel->schemaname != NULL &&
		!OidIsValid(LookupNamespaceNoError(rel->schemaname)))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"%s\" does not exist", rel->schemaname)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("schema \"%s\" does not exist, skipping",
							rel->schemaname)));
		}
		return;
	}

	for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++)
	{
		if (rentry->kind == rightkind)
		{
			if (!missing_ok)
			{
				ereport(ERROR,
						(errcode(rentry->nonexistent_code),
						 errmsg(rentry->nonexistent_msg, rel->relname)));
			}
			else
			{
				ereport(NOTICE, (errmsg(rentry->skipping_msg, rel->relname)));
				break;
			}
		}
	}

	Assert(rentry->kind != '\0');	/* Should be impossible */
					/*
					 *
					 * 按理不应发生
					 */
}

/*
 * Emit the right error message for a "DROP" command issued on a
 * relation of the wrong type
 *
 * 对类型不对的关系执行 DROP 时，发出对应的错误信息。
 */
static void
DropErrorMsgWrongType(const char *relname, char wrongkind, char rightkind)
{
	const struct dropmsgstrings *rentry;
	const struct dropmsgstrings *wentry;

	for (rentry = dropmsgstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	for (wentry = dropmsgstringarray; wentry->kind != '\0'; wentry++)
		if (wentry->kind == wrongkind)
			break;
	/* wrongkind could be something we don't have in our table... */
	/*
	 *
	 * wrongkind 可能是对照表里没有的类型……
	 */

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(rentry->nota_msg, relname),
			 (wentry->kind != '\0') ? errhint("%s", _(wentry->drophint_msg)) : 0));
}

/*
 * RemoveRelations
 *		Implements DROP TABLE, DROP INDEX, DROP SEQUENCE, DROP VIEW,
 *		DROP MATERIALIZED VIEW, DROP FOREIGN TABLE
 *
 * RemoveRelations：实现 DROP TABLE、DROP INDEX、DROP SEQUENCE、DROP VIEW、
 * DROP MATERIALIZED VIEW、DROP FOREIGN TABLE。
 */
void
RemoveRelations(DropStmt *drop)
{
	ObjectAddresses *objects;
	char		relkind;
	ListCell   *cell;
	int			flags = 0;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/* DROP CONCURRENTLY uses a weaker lock, and has some restrictions */
	/*
	 *
	 * DROP CONCURRENTLY 用更弱的锁，并且有一些限制。
	 */
	if (drop->concurrent)
	{
		/*
		 * Note that for temporary relations this lock may get upgraded later
		 * on, but as no other session can access a temporary relation, this
		 * is actually fine.
		 *
		 * 注意：临时关系上的这把锁稍后可能升级，但别的会话访问不了临时关系，所以没问题。
		 */
		lockmode = ShareUpdateExclusiveLock;
		Assert(drop->removeType == OBJECT_INDEX);
		if (list_length(drop->objects) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY does not support dropping multiple objects")));
		if (drop->behavior == DROP_CASCADE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DROP INDEX CONCURRENTLY does not support CASCADE")));
	}

	/*
	 * First we identify all the relations, then we delete them in a single
	 * performMultipleDeletions() call.  This is to avoid unwanted DROP
	 * RESTRICT errors if one of the relations depends on another.
	 *
	 * 先找出全部关系，再在一次 performMultipleDeletions() 调用里删除。这样若其中一个依赖于另一个，就不会误报
	 * DROP RESTRICT。
	 */

	/* Determine required relkind */
	/*
	 *
	 * 确定要求的 relkind
	 */
	switch (drop->removeType)
	{
		case OBJECT_TABLE:
			relkind = RELKIND_RELATION;
			break;

		case OBJECT_INDEX:
			relkind = RELKIND_INDEX;
			break;

		case OBJECT_SEQUENCE:
			relkind = RELKIND_SEQUENCE;
			break;

		case OBJECT_VIEW:
			relkind = RELKIND_VIEW;
			break;

		case OBJECT_MATVIEW:
			relkind = RELKIND_MATVIEW;
			break;

		case OBJECT_FOREIGN_TABLE:
			relkind = RELKIND_FOREIGN_TABLE;
			break;

		default:
			elog(ERROR, "unrecognized drop object type: %d",
				 (int) drop->removeType);
			relkind = 0;		/* keep compiler quiet */
						/*
						 *
						 * 免得编译器告警
						 */
			break;
	}

	/* Lock and validate each relation; build a list of object addresses */
	/*
	 *
	 * 锁定并校验每个关系，同时建立对象地址列表
	 */
	objects = new_object_addresses();

	foreach(cell, drop->objects)
	{
		RangeVar   *rel = makeRangeVarFromNameList((List *) lfirst(cell));
		Oid			relOid;
		ObjectAddress obj;
		struct DropRelationCallbackState state;

		/*
		 * These next few steps are a great deal like relation_openrv, but we
		 * don't bother building a relcache entry since we don't need it.
		 *
		 * 下面几步很像 relation_openrv，但我们不建 relcache 项，因为用不到。
		 *
		 * Check for shared-cache-inval messages before trying to access the
		 * relation.  This is needed to cover the case where the name
		 * identifies a rel that has been dropped and recreated since the
		 * start of our transaction: if we don't flush the old syscache entry,
		 * then we'll latch onto that entry and suffer an error later.
		 *
		 * 访问关系前先检查共享缓存失效消息。若该名字对应的关系在本事务开始后被删除又重建，不刷掉旧的 syscache 项就会抓到旧项，
		 * 稍后出错。
		 */
		AcceptInvalidationMessages();

		/* Look up the appropriate relation using namespace search. */
		/*
		 *
		 * 用命名空间搜索查找对应关系。
		 */
		state.expected_relkind = relkind;
		state.heap_lockmode = drop->concurrent ?
			ShareUpdateExclusiveLock : AccessExclusiveLock;
		/* We must initialize these fields to show that no locks are held: */
		/*
		 *
		 * 必须初始化这些字段，表示尚未持有任何锁：
		 */
		state.heapOid = InvalidOid;
		state.partParentOid = InvalidOid;

		relOid = RangeVarGetRelidExtended(rel, lockmode, RVR_MISSING_OK,
										  RangeVarCallbackForDropRelation,
										  &state);

		/* Not there? */
		/*
		 *
		 * 不存在？
		 */
		if (!OidIsValid(relOid))
		{
			DropErrorMsgNonExistent(rel, relkind, drop->missing_ok);
			continue;
		}

		/*
		 * Decide if concurrent mode needs to be used here or not.  The
		 * callback retrieved the rel's persistence for us.
		 *
		 * 决定这里是否使用并发模式。回调已经取回了关系的持久性。
		 */
		if (drop->concurrent &&
			state.actual_relpersistence != RELPERSISTENCE_TEMP)
		{
			Assert(list_length(drop->objects) == 1 &&
				   drop->removeType == OBJECT_INDEX);
			flags |= PERFORM_DELETION_CONCURRENTLY;
		}

		/*
		 * Concurrent index drop cannot be used with partitioned indexes,
		 * either.
		 *
		 * 分区索引同样不能用并发方式删除。
		 */
		if ((flags & PERFORM_DELETION_CONCURRENTLY) != 0 &&
			state.actual_relkind == RELKIND_PARTITIONED_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot drop partitioned index \"%s\" concurrently",
							rel->relname)));

		/*
		 * If we're told to drop a partitioned index, we must acquire lock on
		 * all the children of its parent partitioned table before proceeding.
		 * Otherwise we'd try to lock the child index partitions before their
		 * tables, leading to potential deadlock against other sessions that
		 * will lock those objects in the other order.
		 *
		 * 若要删除分区索引，必须先锁住其父分区表的所有子表再继续。否则会先锁子索引分区再锁它们的表，可能和按相反顺序加锁的其他会话死锁。
		 */
		if (state.actual_relkind == RELKIND_PARTITIONED_INDEX)
			(void) find_all_inheritors(state.heapOid,
									   state.heap_lockmode,
									   NULL);

		/* OK, we're ready to delete this one */
		/*
		 *
		 * 可以删除这一个了
		 */
		obj.classId = RelationRelationId;
		obj.objectId = relOid;
		obj.objectSubId = 0;

		add_exact_object_address(&obj, objects);
	}

	performMultipleDeletions(objects, drop->behavior, flags);

	free_object_addresses(objects);
}

/*
 * Before acquiring a table lock, check whether we have sufficient rights.
 * In the case of DROP INDEX, also try to lock the table before the index.
 * Also, if the table to be dropped is a partition, we try to lock the parent
 * first.
 *
 * 在获取表锁之前，检查权限是否足够。DROP INDEX 时还要尽量先锁表再锁索引。若要删除的表是分区，则先尝试锁父表。
 */
static void
RangeVarCallbackForDropRelation(const RangeVar *rel, Oid relOid, Oid oldRelOid,
								void *arg)
{
	HeapTuple	tuple;
	struct DropRelationCallbackState *state;
	char		expected_relkind;
	bool		is_partition;
	Form_pg_class classform;
	LOCKMODE	heap_lockmode;
	bool		invalid_system_index = false;

	state = (struct DropRelationCallbackState *) arg;
	heap_lockmode = state->heap_lockmode;

	/*
	 * If we previously locked some other index's heap, and the name we're
	 * looking up no longer refers to that relation, release the now-useless
	 * lock.
	 *
	 * 若先前锁了别的索引的堆表，而现在这个名字已经不再指向那个关系，就释放这把已经没用的锁。
	 */
	if (relOid != oldRelOid && OidIsValid(state->heapOid))
	{
		UnlockRelationOid(state->heapOid, heap_lockmode);
		state->heapOid = InvalidOid;
	}

	/*
	 * Similarly, if we previously locked some other partition's heap, and the
	 * name we're looking up no longer refers to that relation, release the
	 * now-useless lock.
	 *
	 * 类似地，若先前锁了别的分区的堆表，而现在这个名字已经不再指向那个关系，就释放这把已经没用的锁。
	 */
	if (relOid != oldRelOid && OidIsValid(state->partParentOid))
	{
		UnlockRelationOid(state->partParentOid, AccessExclusiveLock);
		state->partParentOid = InvalidOid;
	}

	/* Didn't find a relation, so no need for locking or permission checks. */
	/*
	 *
	 * 没找到关系，因此不必加锁或做权限检查。
	 */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped, so nothing to do */
							/*
							 *
							 * 已被并发删除，无需处理
							 */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	is_partition = classform->relispartition;

	/* Pass back some data to save lookups in RemoveRelations */
	/*
	 *
	 * 回传一些数据，省得 RemoveRelations 再查一次
	 */
	state->actual_relkind = classform->relkind;
	state->actual_relpersistence = classform->relpersistence;

	/*
	 * Both RELKIND_RELATION and RELKIND_PARTITIONED_TABLE are OBJECT_TABLE,
	 * but RemoveRelations() can only pass one relkind for a given relation.
	 * It chooses RELKIND_RELATION for both regular and partitioned tables.
	 * That means we must be careful before giving the wrong type error when
	 * the relation is RELKIND_PARTITIONED_TABLE.  An equivalent problem
	 * exists with indexes.
	 *
	 * RELKIND_RELATION 和 RELKIND_PARTITIONED_TABLE 都是 OBJECT_TABLE，但
	 * RemoveRelations() 对一个关系只能传入一种 relkind。它对普通表和分区表都选用
	 * RELKIND_RELATION。因此在关系实际是 RELKIND_PARTITIONED_TABLE 时，报类型错误必须小心。
	 * 索引也有同样的问题。
	 */
	if (classform->relkind == RELKIND_PARTITIONED_TABLE)
		expected_relkind = RELKIND_RELATION;
	else if (classform->relkind == RELKIND_PARTITIONED_INDEX)
		expected_relkind = RELKIND_INDEX;
	else
		expected_relkind = classform->relkind;

	if (state->expected_relkind != expected_relkind)
		DropErrorMsgWrongType(rel->relname, classform->relkind,
							  state->expected_relkind);

	/* Allow DROP to either table owner or schema owner */
	/*
	 *
	 * 表属主或模式属主都可以执行 DROP
	 */
	if (!object_ownercheck(RelationRelationId, relOid, GetUserId()) &&
		!object_ownercheck(NamespaceRelationId, classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(classform->relkind),
					   rel->relname);

	/*
	 * Check the case of a system index that might have been invalidated by a
	 * failed concurrent process and allow its drop. For the time being, this
	 * only concerns indexes of toast relations that became invalid during a
	 * REINDEX CONCURRENTLY process.
	 *
	 * 检查系统索引是否因失败的并发过程而失效，若是则允许删除。目前这只涉及在 REINDEX CONCURRENTLY 过程中变为无效的
	 * TOAST 关系索引。
	 */
	if (IsSystemClass(relOid, classform) && classform->relkind == RELKIND_INDEX)
	{
		HeapTuple	locTuple;
		Form_pg_index indexform;
		bool		indisvalid;

		locTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(relOid));
		if (!HeapTupleIsValid(locTuple))
		{
			ReleaseSysCache(tuple);
			return;
		}

		indexform = (Form_pg_index) GETSTRUCT(locTuple);
		indisvalid = indexform->indisvalid;
		ReleaseSysCache(locTuple);

		/* Mark object as being an invalid index of system catalogs */
		/*
		 *
		 * 把对象标成系统目录上的无效索引
		 */
		if (!indisvalid)
			invalid_system_index = true;
	}

	/* In the case of an invalid index, it is fine to bypass this check */
	/*
	 *
	 * 无效索引可以跳过这项检查
	 */
	if (!invalid_system_index && !allowSystemTableMods && IsSystemClass(relOid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rel->relname)));

	ReleaseSysCache(tuple);

	/*
	 * In DROP INDEX, attempt to acquire lock on the parent table before
	 * locking the index.  index_drop() will need this anyway, and since
	 * regular queries lock tables before their indexes, we risk deadlock if
	 * we do it the other way around.  No error if we don't find a pg_index
	 * entry, though --- the relation may have been dropped.  Note that this
	 * code will execute for either plain or partitioned indexes.
	 *
	 * DROP INDEX 时，先尝试锁父表再锁索引。index_drop() 反正也需要这把锁，而普通查询是先锁表再锁索引，
	 * 反过来做有死锁风险。找不到 pg_index 项也不报错，关系可能已经被删除。这段代码对普通索引和分区索引都会执行。
	 */
	if (expected_relkind == RELKIND_INDEX &&
		relOid != oldRelOid)
	{
		state->heapOid = IndexGetRelation(relOid, true);
		if (OidIsValid(state->heapOid))
			LockRelationOid(state->heapOid, heap_lockmode);
	}

	/*
	 * Similarly, if the relation is a partition, we must acquire lock on its
	 * parent before locking the partition.  That's because queries lock the
	 * parent before its partitions, so we risk deadlock if we do it the other
	 * way around.
	 *
	 * 类似地，若关系是分区，必须先锁父表再锁分区。查询也是先锁父表再锁分区，反过来做有死锁风险。
	 */
	if (is_partition && relOid != oldRelOid)
	{
		state->partParentOid = get_partition_parent(relOid, true);
		if (OidIsValid(state->partParentOid))
			LockRelationOid(state->partParentOid, AccessExclusiveLock);
	}
}

/*
 * ExecuteTruncate
 *		Executes a TRUNCATE command.
 *
 * ExecuteTruncate：执行 TRUNCATE 命令。
 *
 * This is a multi-relation truncate.  We first open and grab exclusive
 * lock on all relations involved, checking permissions and otherwise
 * verifying that the relation is OK for truncation.  Note that if relations
 * are foreign tables, at this stage, we have not yet checked that their
 * foreign data in external data sources are OK for truncation.  These are
 * checked when foreign data are actually truncated later.  In CASCADE mode,
 * relations having FK references to the targeted relations are automatically
 * added to the group; in RESTRICT mode, we check that all FK references are
 * internal to the group that's being truncated.  Finally all the relations
 * are truncated and reindexed.
 *
 * 这是多关系截断。先打开并排他锁住所有相关关系，检查权限并确认关系可以截断。若是外部表，这一步还没检查外部数据源里的数据能否截断，
 * 要等真正截断外部数据时再查。CASCADE 模式下，外键引用目标关系的表会自动加入这一组；RESTRICT 模式下，
 * 要确认所有外键引用都在被截断的这一组之内。最后截断全部关系并重建索引。
 */
void
ExecuteTruncate(TruncateStmt *stmt)
{
	List	   *rels = NIL;
	List	   *relids = NIL;
	List	   *relids_logged = NIL;
	ListCell   *cell;

	/*
	 * Open, exclusive-lock, and check all the explicitly-specified relations
	 *
	 * 打开、排他锁并检查所有显式指定的关系
	 */
	foreach(cell, stmt->relations)
	{
		RangeVar   *rv = lfirst(cell);
		Relation	rel;
		bool		recurse = rv->inh;
		Oid			myrelid;
		LOCKMODE	lockmode = AccessExclusiveLock;

		myrelid = RangeVarGetRelidExtended(rv, lockmode,
										   0, RangeVarCallbackForTruncate,
										   NULL);

		/* don't throw error for "TRUNCATE foo, foo" */
		/*
		 *
		 * 对 TRUNCATE foo, foo 这种重复不报错
		 */
		if (list_member_oid(relids, myrelid))
			continue;

		/* open the relation, we already hold a lock on it */
		/*
		 *
		 * 打开关系；锁已经持有
		 */
		rel = table_open(myrelid, NoLock);

		/*
		 * RangeVarGetRelidExtended() has done most checks with its callback,
		 * but other checks with the now-opened Relation remain.
		 *
		 * RangeVarGetRelidExtended() 已在回调里做了大部分检查，但还有一些检查要等 Relation 打开后再做。
		 */
		truncate_check_activity(rel);

		rels = lappend(rels, rel);
		relids = lappend_oid(relids, myrelid);

		/* Log this relation only if needed for logical decoding */
		/*
		 *
		 * 仅在逻辑解码需要时记录这个关系
		 */
		if (RelationIsLogicallyLogged(rel))
			relids_logged = lappend_oid(relids_logged, myrelid);

		if (recurse)
		{
			ListCell   *child;
			List	   *children;

			children = find_all_inheritors(myrelid, lockmode, NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);

				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors already got lock */
				/*
				 *
				 * find_all_inheritors 已经拿到锁
				 */
				rel = table_open(childrelid, NoLock);

				/*
				 * It is possible that the parent table has children that are
				 * temp tables of other backends.  We cannot safely access
				 * such tables (because of buffering issues), and the best
				 * thing to do is to silently ignore them.  Note that this
				 * check is the same as one of the checks done in
				 * truncate_check_activity() called below, still it is kept
				 * here for simplicity.
				 *
				 * 父表的子表可能是其他后端的临时表。这类表不能安全访问（有缓冲问题），最好的办法是悄悄忽略。这个检查和下文
				 * truncate_check_activity() 里的一项相同，为了简单仍留在这里。
				 */
				if (RELATION_IS_OTHER_TEMP(rel))
				{
					table_close(rel, lockmode);
					continue;
				}

				/*
				 * Inherited TRUNCATE commands perform access permission
				 * checks on the parent table only. So we skip checking the
				 * children's permissions and don't call
				 * truncate_check_perms() here.
				 *
				 * 继承来的 TRUNCATE 只检查父表的访问权限。因此这里跳过子表权限检查，也不调用 truncate_check_perms()。
				 */
				truncate_check_rel(RelationGetRelid(rel), rel->rd_rel);
				truncate_check_activity(rel);

				rels = lappend(rels, rel);
				relids = lappend_oid(relids, childrelid);

				/* Log this relation only if needed for logical decoding */
				/*
				 *
				 * 仅在逻辑解码需要时记录这个关系
				 */
				if (RelationIsLogicallyLogged(rel))
					relids_logged = lappend_oid(relids_logged, childrelid);
			}
		}
		else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot truncate only a partitioned table"),
					 errhint("Do not specify the ONLY keyword, or use TRUNCATE ONLY on the partitions directly.")));
	}

	ExecuteTruncateGuts(rels, relids, relids_logged,
						stmt->behavior, stmt->restart_seqs, false);

	/* And close the rels */
	/*
	 *
	 * 然后关闭这些关系
	 */
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		table_close(rel, NoLock);
	}
}

/*
 * ExecuteTruncateGuts
 *
 * 函数 ExecuteTruncateGuts
 *
 * Internal implementation of TRUNCATE.  This is called by the actual TRUNCATE
 * command (see above) as well as replication subscribers that execute a
 * replicated TRUNCATE action.
 *
 * TRUNCATE 的内部实现。真正的 TRUNCATE 命令（见上）以及执行复制过来的 TRUNCATE 动作的订阅端都会调用它。
 *
 * explicit_rels is the list of Relations to truncate that the command
 * specified.  relids is the list of Oids corresponding to explicit_rels.
 * relids_logged is the list of Oids (a subset of relids) that require
 * WAL-logging.  This is all a bit redundant, but the existing callers have
 * this information handy in this form.
 *
 * explicit_rels 是命令指定要截断的 Relation 列表。relids 是与之对应的 OID 列表。
 * relids_logged 是其中需要写 WAL 的 OID（relids 的子集）。有点重复，但现有调用方手里的就是这种形式。
 */
void
ExecuteTruncateGuts(List *explicit_rels,
					List *relids,
					List *relids_logged,
					DropBehavior behavior, bool restart_seqs,
					bool run_as_table_owner)
{
	List	   *rels;
	List	   *seq_relids = NIL;
	HTAB	   *ft_htab = NULL;
	EState	   *estate;
	ResultRelInfo *resultRelInfos;
	ResultRelInfo *resultRelInfo;
	SubTransactionId mySubid;
	ListCell   *cell;
	Oid		   *logrelids;

	/*
	 * Check the explicitly-specified relations.
	 *
	 * 检查显式指定的关系。
	 *
	 * In CASCADE mode, suck in all referencing relations as well.  This
	 * requires multiple iterations to find indirectly-dependent relations. At
	 * each phase, we need to exclusive-lock new rels before looking for their
	 * dependencies, else we might miss something.  Also, we check each rel as
	 * soon as we open it, to avoid a faux pas such as holding lock for a long
	 * time on a rel we have no permissions for.
	 *
	 * CASCADE 模式下，把所有引用这些关系的表也吸进来。间接依赖要多轮才能找全。每一轮都必须先对新关系加排他锁，再查它们的依赖，
	 * 否则可能漏掉。另外，一打开就检查每个关系，避免在没权限的关系上长时间持锁。
	 */
	rels = list_copy(explicit_rels);
	if (behavior == DROP_CASCADE)
	{
		for (;;)
		{
			List	   *newrelids;

			newrelids = heap_truncate_find_FKs(relids);
			if (newrelids == NIL)
				break;			/* nothing else to add */
							/*
							 *
							 * 没有别的要加了
							 */

			foreach(cell, newrelids)
			{
				Oid			relid = lfirst_oid(cell);
				Relation	rel;

				rel = table_open(relid, AccessExclusiveLock);
				ereport(NOTICE,
						(errmsg("truncate cascades to table \"%s\"",
								RelationGetRelationName(rel))));
				truncate_check_rel(relid, rel->rd_rel);
				truncate_check_perms(relid, rel->rd_rel);
				truncate_check_activity(rel);
				rels = lappend(rels, rel);
				relids = lappend_oid(relids, relid);

				/* Log this relation only if needed for logical decoding */
				/*
				 *
				 * 仅在逻辑解码需要时记录这个关系
				 */
				if (RelationIsLogicallyLogged(rel))
					relids_logged = lappend_oid(relids_logged, relid);
			}
		}
	}

	/*
	 * Check foreign key references.  In CASCADE mode, this should be
	 * unnecessary since we just pulled in all the references; but as a
	 * cross-check, do it anyway if in an Assert-enabled build.
	 *
	 * 检查外键引用。CASCADE 模式下刚把所有引用都拉进来了，这一步本应多余；但在打开断言的构建里仍做一次交叉检查。
	 */
#ifdef USE_ASSERT_CHECKING
	heap_truncate_check_FKs(rels, false);
#else
	if (behavior == DROP_RESTRICT)
		heap_truncate_check_FKs(rels, false);
#endif

	/*
	 * If we are asked to restart sequences, find all the sequences, lock them
	 * (we need AccessExclusiveLock for ResetSequence), and check permissions.
	 * We want to do this early since it's pointless to do all the truncation
	 * work only to fail on sequence permissions.
	 *
	 * 若要求重启序列，找出所有序列，加锁（ResetSequence 需要 AccessExclusiveLock）并检查权限。要尽早做：
	 * 截断做完才发现序列权限不够就白忙了。
	 */
	if (restart_seqs)
	{
		foreach(cell, rels)
		{
			Relation	rel = (Relation) lfirst(cell);
			List	   *seqlist = getOwnedSequences(RelationGetRelid(rel));
			ListCell   *seqcell;

			foreach(seqcell, seqlist)
			{
				Oid			seq_relid = lfirst_oid(seqcell);
				Relation	seq_rel;

				seq_rel = relation_open(seq_relid, AccessExclusiveLock);

				/* This check must match AlterSequence! */
				/*
				 *
				 * 这项检查必须和 AlterSequence 一致！
				 */
				if (!object_ownercheck(RelationRelationId, seq_relid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SEQUENCE,
								   RelationGetRelationName(seq_rel));

				seq_relids = lappend_oid(seq_relids, seq_relid);

				relation_close(seq_rel, NoLock);
			}
		}
	}

	/* Prepare to catch AFTER triggers. */
	/*
	 *
	 * 准备捕获 AFTER 触发器。
	 */
	AfterTriggerBeginQuery();

	/*
	 * To fire triggers, we'll need an EState as well as a ResultRelInfo for
	 * each relation.  We don't need to call ExecOpenIndices, though.
	 *
	 * 要触发触发器，需要 EState，以及每个关系一个 ResultRelInfo。不必调用 ExecOpenIndices。
	 *
	 * We put the ResultRelInfos in the es_opened_result_relations list, even
	 * though we don't have a range table and don't populate the
	 * es_result_relations array.  That's a bit bogus, but it's enough to make
	 * ExecGetTriggerResultRel() find them.
	 *
	 * 即使没有 rangetable、也不填充 es_result_relations 数组，仍把 ResultRelInfo 放进
	 * es_opened_result_relations。有点取巧，但足以让 ExecGetTriggerResultRel()
	 * 找到它们。
	 */
	estate = CreateExecutorState();
	resultRelInfos = (ResultRelInfo *)
		palloc(list_length(rels) * sizeof(ResultRelInfo));
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		InitResultRelInfo(resultRelInfo,
						  rel,
						  0,	/* dummy rangetable index */
							/*
							 *
							 * 占位用的 rangetable 下标
							 */
						  NULL,
						  0);
		estate->es_opened_result_relations =
			lappend(estate->es_opened_result_relations, resultRelInfo);
		resultRelInfo++;
	}

	/*
	 * Process all BEFORE STATEMENT TRUNCATE triggers before we begin
	 * truncating (this is because one of them might throw an error). Also, if
	 * we were to allow them to prevent statement execution, that would need
	 * to be handled here.
	 *
	 * 开始截断之前处理所有 BEFORE STATEMENT TRUNCATE 触发器（因为其中某个可能抛错）。
	 * 若将来允许它们阻止语句执行，也要在这里处理。
	 */
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		UserContext ucxt;

		if (run_as_table_owner)
			SwitchToUntrustedUser(resultRelInfo->ri_RelationDesc->rd_rel->relowner,
								  &ucxt);
		ExecBSTruncateTriggers(estate, resultRelInfo);
		if (run_as_table_owner)
			RestoreUserContext(&ucxt);
		resultRelInfo++;
	}

	/*
	 * OK, truncate each table.
	 *
	 * 开始逐表截断。
	 */
	mySubid = GetCurrentSubTransactionId();

	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		/* Skip partitioned tables as there is nothing to do */
		/*
		 *
		 * 分区表本身没有可截断的存储，跳过
		 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			continue;

		/*
		 * Build the lists of foreign tables belonging to each foreign server
		 * and pass each list to the foreign data wrapper's callback function,
		 * so that each server can truncate its all foreign tables in bulk.
		 * Each list is saved as a single entry in a hash table that uses the
		 * server OID as lookup key.
		 *
		 * 按外部服务器归集外部表，把每份列表交给外部数据包装器的回调，以便各服务器批量截断自己的全部外部表。每份列表作为哈希表的一项，
		 * 以服务器 OID 为键。
		 */
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			Oid			serverid = GetForeignServerIdByRelId(RelationGetRelid(rel));
			bool		found;
			ForeignTruncateInfo *ft_info;

			/* First time through, initialize hashtable for foreign tables */
			/*
			 *
			 * 第一次进入时，为外部表初始化哈希表
			 */
			if (!ft_htab)
			{
				HASHCTL		hctl;

				memset(&hctl, 0, sizeof(HASHCTL));
				hctl.keysize = sizeof(Oid);
				hctl.entrysize = sizeof(ForeignTruncateInfo);
				hctl.hcxt = CurrentMemoryContext;

				ft_htab = hash_create("TRUNCATE for Foreign Tables",
									  32,	/* start small and extend */
										/*
										 *
										 * 先小一点，不够再扩
										 */
									  &hctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			}

			/* Find or create cached entry for the foreign table */
			/*
			 *
			 * 查找或创建该外部表的缓存项
			 */
			ft_info = hash_search(ft_htab, &serverid, HASH_ENTER, &found);
			if (!found)
				ft_info->rels = NIL;

			/*
			 * Save the foreign table in the entry of the server that the
			 * foreign table belongs to.
			 *
			 * 把外部表记到它所属服务器的那一项里。
			 */
			ft_info->rels = lappend(ft_info->rels, rel);
			continue;
		}

		/*
		 * Normally, we need a transaction-safe truncation here.  However, if
		 * the table was either created in the current (sub)transaction or has
		 * a new relfilenumber in the current (sub)transaction, then we can
		 * just truncate it in-place, because a rollback would cause the whole
		 * table or the current physical file to be thrown away anyway.
		 *
		 * 通常这里需要事务安全的截断。但如果表是在当前（子）事务中创建的，或者在当前（子）事务中已有新的 relfilenumber，
		 * 就可以就地截断，因为回滚本来就会丢掉整张表或当前物理文件。
		 */
		if (rel->rd_createSubid == mySubid ||
			rel->rd_newRelfilelocatorSubid == mySubid)
		{
			/* Immediate, non-rollbackable truncation is OK */
			/*
			 *
			 * 可以立即做不可回滚的截断
			 */
			heap_truncate_one_rel(rel);
		}
		else
		{
			Oid			heap_relid;
			Oid			toast_relid;
			ReindexParams reindex_params = {0};

			/*
			 * This effectively deletes all rows in the table, and may be done
			 * in a serializable transaction.  In that case we must record a
			 * rw-conflict in to this transaction from each transaction
			 * holding a predicate lock on the table.
			 *
			 * 这实际上删掉表中所有行，而且可能发生在可串行化事务里。此时必须记录：每个在该表上持有谓词锁的事务，都与本事务存在读写冲突。
			 */
			CheckTableForSerializableConflictIn(rel);

			/*
			 * Need the full transaction-safe pushups.
			 *
			 * 需要走完整的事务安全流程。
			 *
			 * Create a new empty storage file for the relation, and assign it
			 * as the relfilenumber value. The old storage file is scheduled
			 * for deletion at commit.
			 *
			 * 为关系创建一个新的空存储文件，并把它设为 relfilenumber。旧存储文件安排在提交时删除。
			 */
			RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);

			heap_relid = RelationGetRelid(rel);

			/*
			 * The same for the toast table, if any.
			 *
			 * 若有 TOAST 表，同样处理。
			 */
			toast_relid = rel->rd_rel->reltoastrelid;
			if (OidIsValid(toast_relid))
			{
				Relation	toastrel = relation_open(toast_relid,
													 AccessExclusiveLock);

				RelationSetNewRelfilenumber(toastrel,
											toastrel->rd_rel->relpersistence);
				table_close(toastrel, NoLock);
			}

			/*
			 * Reconstruct the indexes to match, and we're done.
			 *
			 * 按新内容重建索引，到此完成。
			 */
			reindex_relation(NULL, heap_relid, REINDEX_REL_PROCESS_TOAST,
							 &reindex_params);
		}

		pgstat_count_truncate(rel);
	}

	/* Now go through the hash table, and truncate foreign tables */
	/*
	 *
	 * 现在遍历哈希表，截断外部表
	 */
	if (ft_htab)
	{
		ForeignTruncateInfo *ft_info;
		HASH_SEQ_STATUS seq;

		hash_seq_init(&seq, ft_htab);

		PG_TRY();
		{
			while ((ft_info = hash_seq_search(&seq)) != NULL)
			{
				FdwRoutine *routine = GetFdwRoutineByServerId(ft_info->serverid);

				/* truncate_check_rel() has checked that already */
				/*
				 *
				 * truncate_check_rel() 已经检查过了
				 */
				Assert(routine->ExecForeignTruncate != NULL);

				routine->ExecForeignTruncate(ft_info->rels,
											 behavior,
											 restart_seqs);
			}
		}
		PG_FINALLY();
		{
			hash_destroy(ft_htab);
		}
		PG_END_TRY();
	}

	/*
	 * Restart owned sequences if we were asked to.
	 *
	 * 若用户要求，重启所属序列。
	 */
	foreach(cell, seq_relids)
	{
		Oid			seq_relid = lfirst_oid(cell);

		ResetSequence(seq_relid);
	}

	/*
	 * Write a WAL record to allow this set of actions to be logically
	 * decoded.
	 *
	 * 写一条 WAL 记录，让这一组动作能被逻辑解码。
	 *
	 * Assemble an array of relids so we can write a single WAL record for the
	 * whole action.
	 *
	 * 把 relid 收成数组，以便整次动作只写一条 WAL 记录。
	 */
	if (relids_logged != NIL)
	{
		xl_heap_truncate xlrec;
		int			i = 0;

		/* should only get here if wal_level >= logical */
		/*
		 *
		 * 只有 wal_level 至少为 logical 时才会走到这里
		 */
		Assert(XLogLogicalInfoActive());

		logrelids = palloc(list_length(relids_logged) * sizeof(Oid));
		foreach(cell, relids_logged)
			logrelids[i++] = lfirst_oid(cell);

		xlrec.dbId = MyDatabaseId;
		xlrec.nrelids = list_length(relids_logged);
		xlrec.flags = 0;
		if (behavior == DROP_CASCADE)
			xlrec.flags |= XLH_TRUNCATE_CASCADE;
		if (restart_seqs)
			xlrec.flags |= XLH_TRUNCATE_RESTART_SEQS;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHeapTruncate);
		XLogRegisterData(logrelids, list_length(relids_logged) * sizeof(Oid));

		XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

		(void) XLogInsert(RM_HEAP_ID, XLOG_HEAP_TRUNCATE);
	}

	/*
	 * Process all AFTER STATEMENT TRUNCATE triggers.
	 *
	 * 处理所有 AFTER STATEMENT TRUNCATE 触发器。
	 */
	resultRelInfo = resultRelInfos;
	foreach(cell, rels)
	{
		UserContext ucxt;

		if (run_as_table_owner)
			SwitchToUntrustedUser(resultRelInfo->ri_RelationDesc->rd_rel->relowner,
								  &ucxt);
		ExecASTruncateTriggers(estate, resultRelInfo);
		if (run_as_table_owner)
			RestoreUserContext(&ucxt);
		resultRelInfo++;
	}

	/* Handle queued AFTER triggers */
	/*
	 *
	 * 处理已排队的 AFTER 触发器
	 */
	AfterTriggerEndQuery(estate);

	/* We can clean up the EState now */
	/*
	 *
	 * 现在可以清理 EState 了
	 */
	FreeExecutorState(estate);

	/*
	 * Close any rels opened by CASCADE (can't do this while EState still
	 * holds refs)
	 *
	 * 关闭 CASCADE 打开的关系（EState 还持有引用时不能关）
	 */
	rels = list_difference_ptr(rels, explicit_rels);
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		table_close(rel, NoLock);
	}
}

/*
 * Check that a given relation is safe to truncate.  Subroutine for
 * ExecuteTruncate() and RangeVarCallbackForTruncate().
 *
 * 检查给定关系能否安全截断。供 ExecuteTruncate() 和 RangeVarCallbackForTruncate()
 * 调用。
 */
static void
truncate_check_rel(Oid relid, Form_pg_class reltuple)
{
	char	   *relname = NameStr(reltuple->relname);

	/*
	 * Only allow truncate on regular tables, foreign tables using foreign
	 * data wrappers supporting TRUNCATE and partitioned tables (although, the
	 * latter are only being included here for the following checks; no
	 * physical truncation will occur in their case.).
	 *
	 * 只允许截断普通表、支持 TRUNCATE 的外部数据包装器上的外部表，以及分区表（分区表列在这里只是为了做下面这些检查；
	 * 它们本身不会做物理截断）。
	 */
	if (reltuple->relkind == RELKIND_FOREIGN_TABLE)
	{
		Oid			serverid = GetForeignServerIdByRelId(relid);
		FdwRoutine *fdwroutine = GetFdwRoutineByServerId(serverid);

		if (!fdwroutine->ExecForeignTruncate)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot truncate foreign table \"%s\"",
							relname)));
	}
	else if (reltuple->relkind != RELKIND_RELATION &&
			 reltuple->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", relname)));

	/*
	 * Most system catalogs can't be truncated at all, or at least not unless
	 * allow_system_table_mods=on. As an exception, however, we allow
	 * pg_largeobject to be truncated as part of pg_upgrade, because we need
	 * to change its relfilenode to match the old cluster, and allowing a
	 * TRUNCATE command to be executed is the easiest way of doing that.
	 *
	 * 大多数系统目录完全不能截断，至少在 allow_system_table_mods 未打开时不行。例外是 pg_upgrade
	 * 过程中允许截断 pg_largeobject，因为需要把它的 relfilenode 改成与旧集群一致，而执行 TRUNCATE
	 * 是最省事的办法。
	 */
	if (!allowSystemTableMods && IsSystemClass(relid, reltuple)
		&& (!IsBinaryUpgrade || relid != LargeObjectRelationId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						relname)));

	InvokeObjectTruncateHook(relid);
}

/*
 * Check that current user has the permission to truncate given relation.
 *
 * 检查当前用户是否有权截断给定关系。
 */
static void
truncate_check_perms(Oid relid, Form_pg_class reltuple)
{
	char	   *relname = NameStr(reltuple->relname);
	AclResult	aclresult;

	/* Permissions checks */
	/*
	 *
	 * 权限检查
	 */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_TRUNCATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(reltuple->relkind),
					   relname);
}

/*
 * Set of extra sanity checks to check if a given relation is safe to
 * truncate.  This is split with truncate_check_rel() as
 * RangeVarCallbackForTruncate() cannot open a Relation yet.
 *
 * 判断给定关系能否安全截断的额外健全性检查。与 truncate_check_rel() 分开，是因为
 * RangeVarCallbackForTruncate() 这时还打不开 Relation。
 */
static void
truncate_check_activity(Relation rel)
{
	/*
	 * Don't allow truncate on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 *
	 * 不允许截断其他后端的临时表，它们的本地缓冲管理器应付不了。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 *
	 * 还要检查当前事务里是否仍在使用该关系，包括打开的扫描和尚未处理的 AFTER 触发器事件。
	 */
	CheckTableNotInUse(rel, "TRUNCATE");
}

/*
 * storage_name
 *	  returns the name corresponding to a typstorage/attstorage enum value
 *
 * storage_name 把 typstorage/attstorage 枚举值转成名字
 */
static const char *
storage_name(char c)
{
	switch (c)
	{
		case TYPSTORAGE_PLAIN:
			return "PLAIN";
		case TYPSTORAGE_EXTERNAL:
			return "EXTERNAL";
		case TYPSTORAGE_EXTENDED:
			return "EXTENDED";
		case TYPSTORAGE_MAIN:
			return "MAIN";
		default:
			return "???";
	}
}

/*----------
 * MergeAttributes
 *		Returns new schema given initial schema and superclasses.
 *
 * MergeAttributes：根据初始模式和父表返回新的模式。
 *
 * Input arguments:
 * 'columns' is the column/attribute definition for the table. (It's a list
 *		of ColumnDef's.) It is destructively changed.
 * 'supers' is a list of OIDs of parent relations, already locked by caller.
 * 'relpersistence' is the persistence type of the table.
 * 'is_partition' tells if the table is a partition.
 *
 * 输入参数：columns 是表的列/属性定义（ColumnDef 的列表），会被就地修改。supers 是父关系 OID 列表，
 * 调用方已经加锁。relpersistence 是表的持久性。is_partition 表示该表是否为分区。
 *
 * Output arguments:
 * 'supconstr' receives a list of CookedConstraint representing
 *		CHECK constraints belonging to parent relations, updated as
 *		necessary to be valid for the child.
 * 'supnotnulls' receives a list of CookedConstraint representing
 *		not-null constraints based on those from parent relations.
 *
 * 输出参数：supconstr 收到父关系上的 CHECK 约束，已按需要调整为对子表有效的 CookedConstraint 列表。
 * supnotnulls 收到根据父关系 NOT NULL 约束生成的 CookedConstraint 列表。
 *
 * Return value:
 * Completed schema list.
 *
 * 返回值：完成合并后的模式列表。
 *
 * Notes:
 *	  The order in which the attributes are inherited is very important.
 *	  Intuitively, the inherited attributes should come first. If a table
 *	  inherits from multiple parents, the order of those attributes are
 *	  according to the order of the parents specified in CREATE TABLE.
 *
 * 说明：属性的继承顺序非常重要。直观上，继承来的属性应排在前面。若表继承多个父表，这些属性的顺序跟 CREATE TABLE
 * 里父表的书写顺序一致。
 *
 *	  Here's an example:
 *
 * 举个例子：
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  The order of the attributes of stud_emp is:
 *
 * stud_emp 的属性顺序是：
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   If the same attribute name appears multiple times, then it appears
 *	   in the result table in the proper location for its first appearance.
 *
 * 若同一个属性名出现多次，结果表里它出现在第一次出现时的位置。
 *
 *	   Constraints (including not-null constraints) for the child table
 *	   are the union of all relevant constraints, from both the child schema
 *	   and parent tables.  In addition, in legacy inheritance, each column that
 *	   appears in a primary key in any of the parents also gets a NOT NULL
 *	   constraint (partitioning doesn't need this, because the PK itself gets
 *	   inherited.)
 *
 * 子表的约束（含 NOT NULL）是子表模式与各父表相关约束的并集。另外，在传统继承里，任一父表主键中的列也会得到 NOT
 * NULL 约束（分区不需要这样做，因为主键本身会被继承）。
 *
 *	   The default value for a child column is defined as:
 *		(1) If the child schema specifies a default, that value is used.
 *		(2) If neither the child nor any parent specifies a default, then
 *			the column will not have a default.
 *		(3) If conflicting defaults are inherited from different parents
 *			(and not overridden by the child), an error is raised.
 *		(4) Otherwise the inherited default is used.
 *
 * 子列的默认值这样定：(1) 子表模式指定了默认值，就用它。(2) 子表和所有父表都没指定，则该列没有默认值。(3)
 * 不同父表继承来的默认值冲突，且子表没有覆盖，则报错。(4) 否则使用继承来的默认值。
 *
 *		Note that the default-value infrastructure is used for generated
 *		columns' expressions too, so most of the preceding paragraph applies
 *		to generation expressions too.  We insist that a child column be
 *		generated if and only if its parent(s) are, but it need not have
 *		the same generation expression.
 *
 * 注意：默认值这套机制也用于生成列的表达式，所以上一段大多同样适用。我们要求子列是生成列，当且仅当其父列是生成列，
 * 但生成表达式不必相同。
 *----------
 */
static List *
MergeAttributes(List *columns, const List *supers, char relpersistence,
				bool is_partition, List **supconstr, List **supnotnulls)
{
	List	   *inh_columns = NIL;
	List	   *constraints = NIL;
	List	   *nnconstraints = NIL;
	bool		have_bogus_defaults = false;
	int			child_attno;
	static Node bogus_marker = {0}; /* marks conflicting defaults */
					/*
					 *
					 * 用来标记互相冲突的默认值
					 */
	List	   *saved_columns = NIL;
	ListCell   *lc;

	/*
	 * Check for and reject tables with too many columns. We perform this
	 * check relatively early for two reasons: (a) we don't run the risk of
	 * overflowing an AttrNumber in subsequent code (b) an O(n^2) algorithm is
	 * okay if we're processing <= 1600 columns, but could take minutes to
	 * execute if the user attempts to create a table with hundreds of
	 * thousands of columns.
	 *
	 * 检查并拒绝列数过多的表。这个检查做得比较早，原因有二：(a) 后面的代码不会把 AttrNumber 溢出；(b) 列数不超过
	 * 1600 时 O(n^2) 算法可以接受，但用户若试图建出几十万列的表，可能要跑好几分钟。
	 *
	 * Note that we also need to check that we do not exceed this figure after
	 * including columns from inherited relations.
	 *
	 * 注意：把继承来的列算进去之后，还要再检查一次是否超过这个上限。
	 */
	if (list_length(columns) > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * Check for duplicate names in the explicit list of attributes.
	 *
	 * 检查显式属性列表里有没有重名。
	 *
	 * Although we might consider merging such entries in the same way that we
	 * handle name conflicts for inherited attributes, it seems to make more
	 * sense to assume such conflicts are errors.
	 *
	 * 虽然可以像处理继承属性重名那样合并这些项，但把这种冲突当成错误更合理。
	 *
	 * We don't use foreach() here because we have two nested loops over the
	 * columns list, with possible element deletions in the inner one.  If we
	 * used foreach_delete_current() it could only fix up the state of one of
	 * the loops, so it seems cleaner to use looping over list indexes for
	 * both loops.  Note that any deletion will happen beyond where the outer
	 * loop is, so its index never needs adjustment.
	 *
	 * 这里不用 foreach()，因为有两层嵌套循环都在扫 columns 列表，内层可能删除元素。
	 * foreach_delete_current() 只能修正其中一个循环的状态，所以两层都用下标循环更干净。
	 * 删除发生在外层循环当前位置之后，因此外层下标不用调整。
	 */
	for (int coldefpos = 0; coldefpos < list_length(columns); coldefpos++)
	{
		ColumnDef  *coldef = list_nth_node(ColumnDef, columns, coldefpos);

		if (!is_partition && coldef->typeName == NULL)
		{
			/*
			 * Typed table column option that does not belong to a column from
			 * the type.  This works because the columns from the type come
			 * first in the list.  (We omit this check for partition column
			 * lists; those are processed separately below.)
			 *
			 * 类型表上出现了并不属于该类型列的列选项。能这样判断，是因为来自类型的列排在列表最前面。（分区的列清单不做这项检查，
			 * 那些在下面单独处理。）
			 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							coldef->colname)));
		}

		/* restpos scans all entries beyond coldef; incr is in loop body */
		/*
		 *
		 * restpos 扫描 coldef 之后的所有项；incr 在循环体内
		 */
		for (int restpos = coldefpos + 1; restpos < list_length(columns);)
		{
			ColumnDef  *restdef = list_nth_node(ColumnDef, columns, restpos);

			if (strcmp(coldef->colname, restdef->colname) == 0)
			{
				if (coldef->is_from_type)
				{
					/*
					 * merge the column options into the column from the type
					 *
					 * 把列选项合并进来自类型的那一列
					 */
					coldef->is_not_null = restdef->is_not_null;
					coldef->raw_default = restdef->raw_default;
					coldef->cooked_default = restdef->cooked_default;
					coldef->constraints = restdef->constraints;
					coldef->is_from_type = false;
					columns = list_delete_nth_cell(columns, restpos);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_COLUMN),
							 errmsg("column \"%s\" specified more than once",
									coldef->colname)));
			}
			else
				restpos++;
		}
	}

	/*
	 * In case of a partition, there are no new column definitions, only dummy
	 * ColumnDefs created for column constraints.  Set them aside for now and
	 * process them at the end.
	 *
	 * 分区没有新的列定义，只有为列约束造出来的占位 ColumnDef。先放到一边，最后再处理。
	 */
	if (is_partition)
	{
		saved_columns = columns;
		columns = NIL;
	}

	/*
	 * Scan the parents left-to-right, and merge their attributes to form a
	 * list of inherited columns (inh_columns).
	 *
	 * 从左到右扫描父表，合并它们的属性，形成继承列列表 inh_columns。
	 */
	child_attno = 0;
	foreach(lc, supers)
	{
		Oid			parent = lfirst_oid(lc);
		Relation	relation;
		TupleDesc	tupleDesc;
		TupleConstr *constr;
		AttrMap    *newattmap;
		List	   *inherited_defaults;
		List	   *cols_with_defaults;
		List	   *nnconstrs;
		ListCell   *lc1;
		ListCell   *lc2;
		Bitmapset  *nncols = NULL;

		/* caller already got lock */
		/*
		 *
		 * 调用方已经加锁
		 */
		relation = table_open(parent, NoLock);

		/*
		 * Check for active uses of the parent partitioned table in the
		 * current transaction, such as being used in some manner by an
		 * enclosing command.
		 *
		 * 检查当前事务是否正在使用这个父分区表，例如被外层命令以某种方式用到。
		 */
		if (is_partition)
			CheckTableNotInUse(relation, "CREATE TABLE .. PARTITION OF");

		/*
		 * We do not allow partitioned tables and partitions to participate in
		 * regular inheritance.
		 *
		 * 不允许分区表和分区参与普通继承。
		 */
		if (relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE && !is_partition)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from partitioned table \"%s\"",
							RelationGetRelationName(relation))));
		if (relation->rd_rel->relispartition && !is_partition)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot inherit from partition \"%s\"",
							RelationGetRelationName(relation))));

		if (relation->rd_rel->relkind != RELKIND_RELATION &&
			relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
			relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("inherited relation \"%s\" is not a table or foreign table",
							RelationGetRelationName(relation))));

		/*
		 * If the parent is permanent, so must be all of its partitions.  Note
		 * that inheritance allows that case.
		 *
		 * 父表若是永久的，它的所有分区也必须是永久的。普通继承则允许子表不是永久的。
		 */
		if (is_partition &&
			relation->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
			relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot create a temporary relation as partition of permanent relation \"%s\"",
							RelationGetRelationName(relation))));

		/* Permanent rels cannot inherit from temporary ones */
		/*
		 *
		 * 永久关系不能继承临时关系
		 */
		if (relpersistence != RELPERSISTENCE_TEMP &&
			relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg(!is_partition
							? "cannot inherit from temporary relation \"%s\""
							: "cannot create a permanent relation as partition of temporary relation \"%s\"",
							RelationGetRelationName(relation))));

		/* If existing rel is temp, it must belong to this session */
		/*
		 *
		 * 若已有关系是临时的，它必须属于本会话
		 */
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
			!relation->rd_islocaltemp)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg(!is_partition
							? "cannot inherit from temporary relation of another session"
							: "cannot create as partition of temporary relation of another session")));

		/*
		 * We should have an UNDER permission flag for this, but for now,
		 * demand that creator of a child table own the parent.
		 *
		 * 本来应该有 UNDER 权限标志，但目前要求子表的创建者拥有父表。
		 */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(relation), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattmap->attnums[] will contain the child-table attribute numbers
		 * for the attributes of this parent table.  (They are not the same
		 * for parents after the first one, nor if we have dropped columns.)
		 *
		 * newattmap->attnums[] 将记下本父表各属性在子表中的属性号。第一个父表之后的父表，或者有被删除的列时，
		 * 这些号并不相同。
		 */
		newattmap = make_attrmap(tupleDesc->natts);

		/* We can't process inherited defaults until newattmap is complete. */
		/*
		 *
		 * newattmap 完成之前，不能处理继承来的默认值。
		 */
		inherited_defaults = cols_with_defaults = NIL;

		/*
		 * Request attnotnull on columns that have a not-null constraint
		 * that's not marked NO INHERIT (even if not valid).
		 *
		 * 若列上有未标成 NO INHERIT 的 NOT NULL 约束（即使尚未验证），就要求 attnotnull。
		 */
		nnconstrs = RelationGetNotNullConstraints(RelationGetRelid(relation),
												  true, false);
		foreach_ptr(CookedConstraint, cc, nnconstrs)
			nncols = bms_add_member(nncols, cc->attnum);

		for (AttrNumber parent_attno = 1; parent_attno <= tupleDesc->natts;
			 parent_attno++)
		{
			Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
														parent_attno - 1);
			char	   *attributeName = NameStr(attribute->attname);
			int			exist_attno;
			ColumnDef  *newdef;
			ColumnDef  *mergeddef;

			/*
			 * Ignore dropped columns in the parent.
			 *
			 * 忽略父表中已删除的列。
			 */
			if (attribute->attisdropped)
				continue;		/* leave newattmap->attnums entry as zero */
							/*
							 *
							 * 把 newattmap->attnums 对应项留成 0
							 */

			/*
			 * Create new column definition
			 *
			 * 创建新的列定义
			 */
			newdef = makeColumnDef(attributeName, attribute->atttypid,
								   attribute->atttypmod, attribute->attcollation);
			newdef->storage = attribute->attstorage;
			newdef->generated = attribute->attgenerated;
			if (CompressionMethodIsValid(attribute->attcompression))
				newdef->compression =
					pstrdup(GetCompressionMethodName(attribute->attcompression));

			/*
			 * Regular inheritance children are independent enough not to
			 * inherit identity columns.  But partitions are integral part of
			 * a partitioned table and inherit identity column.
			 *
			 * 普通继承的子表足够独立，不继承标识列。但分区是分区表不可分割的一部分，会继承标识列。
			 */
			if (is_partition)
				newdef->identity = attribute->attidentity;

			/*
			 * Does it match some previously considered column from another
			 * parent?
			 *
			 * 是否和先前从另一个父表考虑过的某列匹配？
			 */
			exist_attno = findAttrByName(attributeName, inh_columns);
			if (exist_attno > 0)
			{
				/*
				 * Yes, try to merge the two column definitions.
				 *
				 * 是的话，尝试合并这两个列定义。
				 */
				mergeddef = MergeInheritedAttribute(inh_columns, exist_attno, newdef);

				newattmap->attnums[parent_attno - 1] = exist_attno;

				/*
				 * Partitions have only one parent, so conflict should never
				 * occur.
				 *
				 * 分区只有一个父表，所以不该发生冲突。
				 */
				Assert(!is_partition);
			}
			else
			{
				/*
				 * No, create a new inherited column
				 *
				 * 否则，新建一个继承列
				 */
				newdef->inhcount = 1;
				newdef->is_local = false;
				inh_columns = lappend(inh_columns, newdef);

				newattmap->attnums[parent_attno - 1] = ++child_attno;
				mergeddef = newdef;
			}

			/*
			 * mark attnotnull if parent has it
			 *
			 * 若父表有 attnotnull，就标上
			 */
			if (bms_is_member(parent_attno, nncols))
				mergeddef->is_not_null = true;

			/*
			 * Locate default/generation expression if any
			 *
			 * 若有默认值或生成表达式，找出来
			 */
			if (attribute->atthasdef)
			{
				Node	   *this_default;

				this_default = TupleDescGetDefault(tupleDesc, parent_attno);
				if (this_default == NULL)
					elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
						 parent_attno, RelationGetRelationName(relation));

				/*
				 * If it's a GENERATED default, it might contain Vars that
				 * need to be mapped to the inherited column(s)' new numbers.
				 * We can't do that till newattmap is ready, so just remember
				 * all the inherited default expressions for the moment.
				 *
				 * 若是 GENERATED 默认值，里面可能有需要映射到继承列新编号的 Var。newattmap 还没准备好，做不了这件事，
				 * 所以先把所有继承来的默认表达式记下来。
				 */
				inherited_defaults = lappend(inherited_defaults, this_default);
				cols_with_defaults = lappend(cols_with_defaults, mergeddef);
			}
		}

		/*
		 * Now process any inherited default expressions, adjusting attnos
		 * using the completed newattmap map.
		 *
		 * 现在处理继承来的默认表达式，用已经完成的 newattmap 调整属性号。
		 */
		forboth(lc1, inherited_defaults, lc2, cols_with_defaults)
		{
			Node	   *this_default = (Node *) lfirst(lc1);
			ColumnDef  *def = (ColumnDef *) lfirst(lc2);
			bool		found_whole_row;

			/* Adjust Vars to match new table's column numbering */
			/*
			 *
			 * 调整 Var，使列号与新表一致
			 */
			this_default = map_variable_attnos(this_default,
											   1, 0,
											   newattmap,
											   InvalidOid, &found_whole_row);

			/*
			 * For the moment we have to reject whole-row variables.  We could
			 * convert them, if we knew the new table's rowtype OID, but that
			 * hasn't been assigned yet.  (A variable could only appear in a
			 * generation expression, so the error message is correct.)
			 *
			 * 目前必须拒绝整行变量。若已知新表的行类型 OID 就可以转换，但 OID 还没分配。（变量只可能出现在生成表达式里，
			 * 所以这条报错是对的。）
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
								   def->colname,
								   RelationGetRelationName(relation))));

			/*
			 * If we already had a default from some prior parent, check to
			 * see if they are the same.  If so, no problem; if not, mark the
			 * column as having a bogus default.  Below, we will complain if
			 * the bogus default isn't overridden by the child columns.
			 *
			 * 若先前某个父表已经给过默认值，检查两者是否相同。相同就没问题；不同就把该列标成默认值无效。下面若子列没有覆盖这个无效默认值，
			 * 就会报错。
			 */
			Assert(def->raw_default == NULL);
			if (def->cooked_default == NULL)
				def->cooked_default = this_default;
			else if (!equal(def->cooked_default, this_default))
			{
				def->cooked_default = &bogus_marker;
				have_bogus_defaults = true;
			}
		}

		/*
		 * Now copy the CHECK constraints of this parent, adjusting attnos
		 * using the completed newattmap map.  Identically named constraints
		 * are merged if possible, else we throw error.
		 *
		 * 现在拷贝这个父表的 CHECK 约束，用已经完成的 newattmap 调整属性号。同名约束尽量合并，否则报错。
		 */
		if (constr && constr->num_check > 0)
		{
			ConstrCheck *check = constr->check;

			for (int i = 0; i < constr->num_check; i++)
			{
				char	   *name = check[i].ccname;
				Node	   *expr;
				bool		found_whole_row;

				/* ignore if the constraint is non-inheritable */
				/*
				 *
				 * 不可继承的约束则忽略
				 */
				if (check[i].ccnoinherit)
					continue;

				/* Adjust Vars to match new table's column numbering */
				/*
				 *
				 * 调整 Var，使列号与新表一致
				 */
				expr = map_variable_attnos(stringToNode(check[i].ccbin),
										   1, 0,
										   newattmap,
										   InvalidOid, &found_whole_row);

				/*
				 * For the moment we have to reject whole-row variables. We
				 * could convert them, if we knew the new table's rowtype OID,
				 * but that hasn't been assigned yet.
				 *
				 * 目前必须拒绝整行变量。若已知新表的行类型 OID 就可以转换，但 OID 还没分配。
				 */
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
									   name,
									   RelationGetRelationName(relation))));

				constraints = MergeCheckConstraint(constraints, name, expr,
												   check[i].ccenforced);
			}
		}

		/*
		 * Also copy the not-null constraints from this parent.  The
		 * attnotnull markings were already installed above.
		 *
		 * 同时拷贝这个父表的 NOT NULL 约束。attnotnull 标记上面已经设好。
		 */
		foreach_ptr(CookedConstraint, nn, nnconstrs)
		{
			Assert(nn->contype == CONSTR_NOTNULL);

			nn->attnum = newattmap->attnums[nn->attnum - 1];

			nnconstraints = lappend(nnconstraints, nn);
		}

		free_attrmap(newattmap);

		/*
		 * Close the parent rel, but keep our lock on it until xact commit.
		 * That will prevent someone else from deleting or ALTERing the parent
		 * before the child is committed.
		 *
		 * 关闭父关系，但锁保持到事务提交。这样在子表提交前，别人无法删除或 ALTER 父表。
		 */
		table_close(relation, NoLock);
	}

	/*
	 * If we had no inherited attributes, the result columns are just the
	 * explicitly declared columns.  Otherwise, we need to merge the declared
	 * columns into the inherited column list.  Although, we never have any
	 * explicitly declared columns if the table is a partition.
	 *
	 * 若没有继承属性，结果列就是显式声明的列。否则要把声明的列并进继承列列表。不过分区表永远不会有自己显式声明的列。
	 */
	if (inh_columns != NIL)
	{
		int			newcol_attno = 0;

		foreach(lc, columns)
		{
			ColumnDef  *newdef = lfirst_node(ColumnDef, lc);
			char	   *attributeName = newdef->colname;
			int			exist_attno;

			/*
			 * Partitions have only one parent and have no column definitions
			 * of their own, so conflict should never occur.
			 *
			 * 分区只有一个父表，也没有自己的列定义，所以不该发生冲突。
			 */
			Assert(!is_partition);

			newcol_attno++;

			/*
			 * Does it match some inherited column?
			 *
			 * 是否和某个继承列匹配？
			 */
			exist_attno = findAttrByName(attributeName, inh_columns);
			if (exist_attno > 0)
			{
				/*
				 * Yes, try to merge the two column definitions.
				 *
				 * 是的话，尝试合并这两个列定义。
				 */
				MergeChildAttribute(inh_columns, exist_attno, newcol_attno, newdef);
			}
			else
			{
				/*
				 * No, attach new column unchanged to result columns.
				 *
				 * 不匹配，就把新列原样接到结果列上。
				 */
				inh_columns = lappend(inh_columns, newdef);
			}
		}

		columns = inh_columns;

		/*
		 * Check that we haven't exceeded the legal # of columns after merging
		 * in inherited columns.
		 *
		 * 合并继承列之后，检查有没有超过合法列数上限。
		 */
		if (list_length(columns) > MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("tables can have at most %d columns",
							MaxHeapAttributeNumber)));
	}

	/*
	 * Now that we have the column definition list for a partition, we can
	 * check whether the columns referenced in the column constraint specs
	 * actually exist.  Also, merge column defaults.
	 *
	 * 既然已经有分区的列定义列表，就可以检查列约束说明里引用的列是否真的存在。同时合并列默认值。
	 */
	if (is_partition)
	{
		foreach(lc, saved_columns)
		{
			ColumnDef  *restdef = lfirst(lc);
			bool		found = false;
			ListCell   *l;

			foreach(l, columns)
			{
				ColumnDef  *coldef = lfirst(l);

				if (strcmp(coldef->colname, restdef->colname) == 0)
				{
					found = true;

					/*
					 * Check for conflicts related to generated columns.
					 *
					 * 检查与生成列相关的冲突。
					 *
					 * Same rules as above: generated-ness has to match the
					 * parent, but the contents of the generation expression
					 * can be different.
					 *
					 * 规则同上：是否为生成列必须和父列一致，但生成表达式的内容可以不同。
					 */
					if (coldef->generated)
					{
						if (restdef->raw_default && !restdef->generated)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
									 errmsg("column \"%s\" inherits from generated column but specifies default",
											restdef->colname)));
						if (restdef->identity)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
									 errmsg("column \"%s\" inherits from generated column but specifies identity",
											restdef->colname)));
					}
					else
					{
						if (restdef->generated)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
									 errmsg("child column \"%s\" specifies generation expression",
											restdef->colname),
									 errhint("A child table column cannot be generated unless its parent column is.")));
					}

					if (coldef->generated && restdef->generated && coldef->generated != restdef->generated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
								 errmsg("column \"%s\" inherits from generated column of different kind",
										restdef->colname),
								 errdetail("Parent column is %s, child column is %s.",
										   coldef->generated == ATTRIBUTE_GENERATED_STORED ? "STORED" : "VIRTUAL",
										   restdef->generated == ATTRIBUTE_GENERATED_STORED ? "STORED" : "VIRTUAL")));

					/*
					 * Override the parent's default value for this column
					 * (coldef->cooked_default) with the partition's local
					 * definition (restdef->raw_default), if there's one. It
					 * should be physically impossible to get a cooked default
					 * in the local definition or a raw default in the
					 * inherited definition, but make sure they're nulls, for
					 * future-proofing.
					 *
					 * 若分区本地定义了默认值（restdef->raw_default），
					 * 就用它覆盖父列的默认值（coldef->cooked_default）。本地定义里出现已煮好的默认值，或继承定义里出现原始默认值，
					 * 按理不可能；为了以后保险，仍确认它们是空的。
					 */
					Assert(restdef->cooked_default == NULL);
					Assert(coldef->raw_default == NULL);
					if (restdef->raw_default)
					{
						coldef->raw_default = restdef->raw_default;
						coldef->cooked_default = NULL;
					}
				}
			}

			/* complain for constraints on columns not in parent */
			/*
			 *
			 * 父表没有的列上出现约束，则报错
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist",
								restdef->colname)));
		}
	}

	/*
	 * If we found any conflicting parent default values, check to make sure
	 * they were overridden by the child.
	 *
	 * 若发现父表默认值互相冲突，要确认子表已经覆盖了它们。
	 */
	if (have_bogus_defaults)
	{
		foreach(lc, columns)
		{
			ColumnDef  *def = lfirst(lc);

			if (def->cooked_default == &bogus_marker)
			{
				if (def->generated)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
							 errmsg("column \"%s\" inherits conflicting generation expressions",
									def->colname),
							 errhint("To resolve the conflict, specify a generation expression explicitly.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
							 errmsg("column \"%s\" inherits conflicting default values",
									def->colname),
							 errhint("To resolve the conflict, specify a default explicitly.")));
			}
		}
	}

	*supconstr = constraints;
	*supnotnulls = nnconstraints;

	return columns;
}


/*
 * MergeCheckConstraint
 *		Try to merge an inherited CHECK constraint with previous ones
 *
 * MergeCheckConstraint：尝试把继承来的 CHECK 约束和已有的合并。
 *
 * If we inherit identically-named constraints from multiple parents, we must
 * merge them, or throw an error if they don't have identical definitions.
 *
 * 若从多个父表继承了同名约束，必须合并；定义不完全相同则报错。
 *
 * constraints is a list of CookedConstraint structs for previous constraints.
 *
 * constraints 是先前约束的 CookedConstraint 结构列表。
 *
 * If the new constraint matches an existing one, then the existing
 * constraint's inheritance count is updated.  If there is a conflict (same
 * name but different expression), throw an error.  If the constraint neither
 * matches nor conflicts with an existing one, a new constraint is appended to
 * the list.
 *
 * 若新约束与已有约束匹配，就更新已有约束的继承计数。若冲突（同名但表达式不同），则报错。既不匹配也不冲突，就把新约束追加到列表。
 */
static List *
MergeCheckConstraint(List *constraints, const char *name, Node *expr, bool is_enforced)
{
	ListCell   *lc;
	CookedConstraint *newcon;

	foreach(lc, constraints)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lc);

		Assert(ccon->contype == CONSTR_CHECK);

		/* Non-matching names never conflict */
		/*
		 *
		 * 名字不同则永不冲突
		 */
		if (strcmp(ccon->name, name) != 0)
			continue;

		if (equal(expr, ccon->expr))
		{
			/* OK to merge constraint with existing */
			/*
			 *
			 * 可以和已有约束合并
			 */
			if (pg_add_s16_overflow(ccon->inhcount, 1,
									&ccon->inhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * When enforceability differs, the merged constraint should be
			 * marked as ENFORCED because one of the parents is ENFORCED.
			 *
			 * 可强制性不同时，合并后的约束应标成 ENFORCED，因为至少有一个父约束是 ENFORCED。
			 */
			if (!ccon->is_enforced && is_enforced)
			{
				ccon->is_enforced = true;
				ccon->skip_validation = false;
			}

			return constraints;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("check constraint name \"%s\" appears multiple times but with different expressions",
						name)));
	}

	/*
	 * Constraint couldn't be merged with an existing one and also didn't
	 * conflict with an existing one, so add it as a new one to the list.
	 *
	 * 既不能和已有约束合并，也没有冲突，就作为新约束加入列表。
	 */
	newcon = palloc0_object(CookedConstraint);
	newcon->contype = CONSTR_CHECK;
	newcon->name = pstrdup(name);
	newcon->expr = expr;
	newcon->inhcount = 1;
	newcon->is_enforced = is_enforced;
	newcon->skip_validation = !is_enforced;
	return lappend(constraints, newcon);
}

/*
 * MergeChildAttribute
 *		Merge given child attribute definition into given inherited attribute.
 *
 * MergeChildAttribute：把给定的子表属性定义并进给定的继承属性。
 *
 * Input arguments:
 * 'inh_columns' is the list of inherited ColumnDefs.
 * 'exist_attno' is the number of the inherited attribute in inh_columns
 * 'newcol_attno' is the attribute number in child table's schema definition
 * 'newdef' is the column/attribute definition from the child table.
 *
 * 输入参数：inh_columns 是继承来的 ColumnDef 列表。exist_attno 是该继承属性在
 * inh_columns 中的序号。newcol_attno 是子表模式定义里的属性号。newdef 是子表的列/属性定义。
 *
 * The ColumnDef in 'inh_columns' list is modified.  The child attribute's
 * ColumnDef remains unchanged.
 *
 * inh_columns 里的 ColumnDef 会被修改。子表属性的 ColumnDef 保持不变。
 *
 * Notes:
 * - The attribute is merged according to the rules laid out in the prologue
 *   of MergeAttributes().
 * - If matching inherited attribute exists but the child attribute can not be
 *   merged into it, the function throws respective errors.
 * - A partition can not have its own column definitions. Hence this function
 *   is applicable only to a regular inheritance child.
 *
 * 说明：属性按 MergeAttributes() 开头所述的规则合并。若已有匹配的继承属性，但子属性无法并进去，函数会报相应的错。
 * 分区不能有自己的列定义，因此本函数只适用于普通继承的子表。
 */
static void
MergeChildAttribute(List *inh_columns, int exist_attno, int newcol_attno, const ColumnDef *newdef)
{
	char	   *attributeName = newdef->colname;
	ColumnDef  *inhdef;
	Oid			inhtypeid,
				newtypeid;
	int32		inhtypmod,
				newtypmod;
	Oid			inhcollid,
				newcollid;

	if (exist_attno == newcol_attno)
		ereport(NOTICE,
				(errmsg("merging column \"%s\" with inherited definition",
						attributeName)));
	else
		ereport(NOTICE,
				(errmsg("moving and merging column \"%s\" with inherited definition", attributeName),
				 errdetail("User-specified column moved to the position of the inherited column.")));

	inhdef = list_nth_node(ColumnDef, inh_columns, exist_attno - 1);

	/*
	 * Must have the same type and typmod
	 *
	 * 类型和 typmod 必须相同
	 */
	typenameTypeIdAndMod(NULL, inhdef->typeName, &inhtypeid, &inhtypmod);
	typenameTypeIdAndMod(NULL, newdef->typeName, &newtypeid, &newtypmod);
	if (inhtypeid != newtypeid || inhtypmod != newtypmod)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" has a type conflict",
						attributeName),
				 errdetail("%s versus %s",
						   format_type_with_typemod(inhtypeid, inhtypmod),
						   format_type_with_typemod(newtypeid, newtypmod))));

	/*
	 * Must have the same collation
	 *
	 * 排序规则必须相同
	 */
	inhcollid = GetColumnDefCollation(NULL, inhdef, inhtypeid);
	newcollid = GetColumnDefCollation(NULL, newdef, newtypeid);
	if (inhcollid != newcollid)
		ereport(ERROR,
				(errcode(ERRCODE_COLLATION_MISMATCH),
				 errmsg("column \"%s\" has a collation conflict",
						attributeName),
				 errdetail("\"%s\" versus \"%s\"",
						   get_collation_name(inhcollid),
						   get_collation_name(newcollid))));

	/*
	 * Identity is never inherited by a regular inheritance child. Pick
	 * child's identity definition if there's one.
	 *
	 * 普通继承的子表从不继承标识属性。若子表自己定义了标识，就用子表的。
	 */
	inhdef->identity = newdef->identity;

	/*
	 * Copy storage parameter
	 *
	 * 拷贝存储参数
	 */
	if (inhdef->storage == 0)
		inhdef->storage = newdef->storage;
	else if (newdef->storage != 0 && inhdef->storage != newdef->storage)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" has a storage parameter conflict",
						attributeName),
				 errdetail("%s versus %s",
						   storage_name(inhdef->storage),
						   storage_name(newdef->storage))));

	/*
	 * Copy compression parameter
	 *
	 * 拷贝压缩参数
	 */
	if (inhdef->compression == NULL)
		inhdef->compression = newdef->compression;
	else if (newdef->compression != NULL)
	{
		if (strcmp(inhdef->compression, newdef->compression) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" has a compression method conflict",
							attributeName),
					 errdetail("%s versus %s", inhdef->compression, newdef->compression)));
	}

	/*
	 * Merge of not-null constraints = OR 'em together
	 *
	 * 合并 NOT NULL 约束，相当于把它们做或运算
	 */
	inhdef->is_not_null |= newdef->is_not_null;

	/*
	 * Check for conflicts related to generated columns.
	 *
	 * 检查与生成列相关的冲突。
	 *
	 * If the parent column is generated, the child column will be made a
	 * generated column if it isn't already.  If it is a generated column,
	 * we'll take its generation expression in preference to the parent's.  We
	 * must check that the child column doesn't specify a default value or
	 * identity, which matches the rules for a single column in
	 * parse_utilcmd.c.
	 *
	 * 若父列是生成列，子列还不是的话会被改成生成列。若子列已经是生成列，优先用子列的生成表达式，而不是父列的。
	 * 必须检查子列没有指定默认值或标识，这和 parse_utilcmd.c 里单列的规则一致。
	 *
	 * Conversely, if the parent column is not generated, the child column
	 * can't be either.  (We used to allow that, but it results in being able
	 * to override the generation expression via UPDATEs through the parent.)
	 *
	 * 反过来，若父列不是生成列，子列也不能是。（以前允许，但那样就能通过父表上的 UPDATE 覆盖生成表达式。）
	 */
	if (inhdef->generated)
	{
		if (newdef->raw_default && !newdef->generated)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
					 errmsg("column \"%s\" inherits from generated column but specifies default",
							inhdef->colname)));
		if (newdef->identity)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
					 errmsg("column \"%s\" inherits from generated column but specifies identity",
							inhdef->colname)));
	}
	else
	{
		if (newdef->generated)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
					 errmsg("child column \"%s\" specifies generation expression",
							inhdef->colname),
					 errhint("A child table column cannot be generated unless its parent column is.")));
	}

	if (inhdef->generated && newdef->generated && newdef->generated != inhdef->generated)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				 errmsg("column \"%s\" inherits from generated column of different kind",
						inhdef->colname),
				 errdetail("Parent column is %s, child column is %s.",
						   inhdef->generated == ATTRIBUTE_GENERATED_STORED ? "STORED" : "VIRTUAL",
						   newdef->generated == ATTRIBUTE_GENERATED_STORED ? "STORED" : "VIRTUAL")));

	/*
	 * If new def has a default, override previous default
	 *
	 * 若新定义有默认值，就覆盖先前的默认值
	 */
	if (newdef->raw_default != NULL)
	{
		inhdef->raw_default = newdef->raw_default;
		inhdef->cooked_default = newdef->cooked_default;
	}

	/* Mark the column as locally defined */
	/*
	 *
	 * 把该列标成局部定义
	 */
	inhdef->is_local = true;
}

/*
 * MergeInheritedAttribute
 *		Merge given parent attribute definition into specified attribute
 *		inherited from the previous parents.
 *
 * MergeInheritedAttribute：把给定的父属性定义并进先前父表已经继承来的那个属性。
 *
 * Input arguments:
 * 'inh_columns' is the list of previously inherited ColumnDefs.
 * 'exist_attno' is the number the existing matching attribute in inh_columns.
 * 'newdef' is the new parent column/attribute definition to be merged.
 *
 * 输入参数：inh_columns 是先前继承来的 ColumnDef 列表。exist_attno 是 inh_columns
 * 中已匹配属性的序号。newdef 是要合并的新父列/属性定义。
 *
 * The matching ColumnDef in 'inh_columns' list is modified and returned.
 *
 * inh_columns 中匹配的 ColumnDef 会被修改并返回。
 *
 * Notes:
 * - The attribute is merged according to the rules laid out in the prologue
 *   of MergeAttributes().
 * - If matching inherited attribute exists but the new attribute can not be
 *   merged into it, the function throws respective errors.
 * - A partition inherits from only a single parent. Hence this function is
 *   applicable only to a regular inheritance.
 *
 * 说明：属性按 MergeAttributes() 开头所述的规则合并。若已有匹配的继承属性，但新属性无法并进去，函数会报相应的错。
 * 分区只继承一个父表，因此本函数只适用于普通继承。
 */
static ColumnDef *
MergeInheritedAttribute(List *inh_columns,
						int exist_attno,
						const ColumnDef *newdef)
{
	char	   *attributeName = newdef->colname;
	ColumnDef  *prevdef;
	Oid			prevtypeid,
				newtypeid;
	int32		prevtypmod,
				newtypmod;
	Oid			prevcollid,
				newcollid;

	ereport(NOTICE,
			(errmsg("merging multiple inherited definitions of column \"%s\"",
					attributeName)));
	prevdef = list_nth_node(ColumnDef, inh_columns, exist_attno - 1);

	/*
	 * Must have the same type and typmod
	 *
	 * 类型和 typmod 必须相同
	 */
	typenameTypeIdAndMod(NULL, prevdef->typeName, &prevtypeid, &prevtypmod);
	typenameTypeIdAndMod(NULL, newdef->typeName, &newtypeid, &newtypmod);
	if (prevtypeid != newtypeid || prevtypmod != newtypmod)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("inherited column \"%s\" has a type conflict",
						attributeName),
				 errdetail("%s versus %s",
						   format_type_with_typemod(prevtypeid, prevtypmod),
						   format_type_with_typemod(newtypeid, newtypmod))));

	/*
	 * Must have the same collation
	 *
	 * 排序规则必须相同
	 */
	prevcollid = GetColumnDefCollation(NULL, prevdef, prevtypeid);
	newcollid = GetColumnDefCollation(NULL, newdef, newtypeid);
	if (prevcollid != newcollid)
		ereport(ERROR,
				(errcode(ERRCODE_COLLATION_MISMATCH),
				 errmsg("inherited column \"%s\" has a collation conflict",
						attributeName),
				 errdetail("\"%s\" versus \"%s\"",
						   get_collation_name(prevcollid),
						   get_collation_name(newcollid))));

	/*
	 * Copy/check storage parameter
	 *
	 * 拷贝或检查存储参数
	 */
	if (prevdef->storage == 0)
		prevdef->storage = newdef->storage;
	else if (prevdef->storage != newdef->storage)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("inherited column \"%s\" has a storage parameter conflict",
						attributeName),
				 errdetail("%s versus %s",
						   storage_name(prevdef->storage),
						   storage_name(newdef->storage))));

	/*
	 * Copy/check compression parameter
	 *
	 * 拷贝或检查压缩参数
	 */
	if (prevdef->compression == NULL)
		prevdef->compression = newdef->compression;
	else if (newdef->compression != NULL)
	{
		if (strcmp(prevdef->compression, newdef->compression) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" has a compression method conflict",
							attributeName),
					 errdetail("%s versus %s",
							   prevdef->compression, newdef->compression)));
	}

	/*
	 * Check for GENERATED conflicts
	 *
	 * 检查 GENERATED 冲突
	 */
	if (prevdef->generated != newdef->generated)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("inherited column \"%s\" has a generation conflict",
						attributeName)));

	/*
	 * Default and other constraints are handled by the caller.
	 *
	 * 默认值和其他约束由调用方处理。
	 */

	if (pg_add_s16_overflow(prevdef->inhcount, 1,
							&prevdef->inhcount))
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many inheritance parents"));

	return prevdef;
}

/*
 * StoreCatalogInheritance
 *		Updates the system catalogs with proper inheritance information.
 *
 * StoreCatalogInheritance：把正确的继承信息写入系统目录。
 *
 * supers is a list of the OIDs of the new relation's direct ancestors.
 *
 * supers 是新关系直接祖先的 OID 列表。
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers,
						bool child_is_partition)
{
	Relation	relation;
	int32		seqNumber;
	ListCell   *entry;

	/*
	 * sanity checks
	 *
	 * 健全性检查
	 */
	Assert(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * Store INHERITS information in pg_inherits using direct ancestors only.
	 * Also enter dependencies on the direct ancestors, and make sure they are
	 * marked with relhassubclass = true.
	 *
	 * 只把直接祖先的 INHERITS 信息写入 pg_inherits。同时记录对直接祖先的依赖，并确保它们的
	 * relhassubclass 为真。
	 *
	 * (Once upon a time, both direct and indirect ancestors were found here
	 * and then entered into pg_ipl.  Since that catalog doesn't exist
	 * anymore, there's no need to look for indirect ancestors.)
	 *
	 * 从前这里会找出直接和间接祖先，再写入 pg_ipl。那个目录已经不存在了，所以不必再找间接祖先。
	 */
	relation = table_open(InheritsRelationId, RowExclusiveLock);

	seqNumber = 1;
	foreach(entry, supers)
	{
		Oid			parentOid = lfirst_oid(entry);

		StoreCatalogInheritance1(relationId, parentOid, seqNumber, relation,
								 child_is_partition);
		seqNumber++;
	}

	table_close(relation, RowExclusiveLock);
}

/*
 * Make catalog entries showing relationId as being an inheritance child
 * of parentOid.  inhRelation is the already-opened pg_inherits catalog.
 *
 * 在目录里记下 relationId 是 parentOid 的继承子表。inhRelation 是已经打开的
 * pg_inherits 目录。
 */
static void
StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int32 seqNumber, Relation inhRelation,
						 bool child_is_partition)
{
	ObjectAddress childobject,
				parentobject;

	/* store the pg_inherits row */
	/*
	 *
	 * 写入 pg_inherits 行
	 */
	StoreSingleInheritance(relationId, parentOid, seqNumber);

	/*
	 * Store a dependency too
	 *
	 * 同时记一条依赖
	 */
	parentobject.classId = RelationRelationId;
	parentobject.objectId = parentOid;
	parentobject.objectSubId = 0;
	childobject.classId = RelationRelationId;
	childobject.objectId = relationId;
	childobject.objectSubId = 0;

	recordDependencyOn(&childobject, &parentobject,
					   child_dependency_type(child_is_partition));

	/*
	 * Post creation hook of this inheritance. Since object_access_hook
	 * doesn't take multiple object identifiers, we relay oid of parent
	 * relation using auxiliary_id argument.
	 *
	 * 这次继承的创建后钩子。object_access_hook 不接受多个对象标识，所以用 auxiliary_id 参数转交父关系的
	 * OID。
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 relationId, 0,
								 parentOid, false);

	/*
	 * Mark the parent as having subclasses.
	 *
	 * 把父表标成有子类。
	 */
	SetRelationHasSubclass(parentOid, true);
}

/*
 * Look for an existing column entry with the given name.
 *
 * 按给定名字查找已有列项。
 *
 * Returns the index (starting with 1) if attribute already exists in columns,
 * 0 if it doesn't.
 *
 * 若属性已在 columns 中，返回从 1 开始的下标；否则返回 0。
 */
static int
findAttrByName(const char *attributeName, const List *columns)
{
	ListCell   *lc;
	int			i = 1;

	foreach(lc, columns)
	{
		if (strcmp(attributeName, lfirst_node(ColumnDef, lc)->colname) == 0)
			return i;

		i++;
	}
	return 0;
}


/*
 * SetRelationHasSubclass
 *		Set the value of the relation's relhassubclass field in pg_class.
 *
 * SetRelationHasSubclass：设置 pg_class 中该关系的 relhassubclass 字段。
 *
 * It's always safe to set this field to true, because all SQL commands are
 * ready to see true and then find no children.  On the other hand, commands
 * generally assume zero children if this is false.
 *
 * 把这个字段设成真总是安全的，因为所有 SQL 命令都准备好看到真、然后发现其实没有子表。反过来，命令一般在它为假时假定没有子表。
 *
 * Caller must hold any self-exclusive lock until end of transaction.  If the
 * new value is false, caller must have acquired that lock before reading the
 * evidence that justified the false value.  That way, it properly waits if
 * another backend is simultaneously concluding no need to change the tuple
 * (new and old values are true).
 *
 * 调用方必须把自排他锁持有到事务结束。若新值是假，调用方必须在读到足以证明该为假的证据之前就拿到这把锁。这样，
 * 若另一个后端同时认定不必改元组（新旧值都是真），本后端会正确等待。
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing plans
 * referencing the relation to be rebuilt with the new list of children.
 * This must happen even if we find that no change is needed in the pg_class
 * row.
 *
 * 注意：这个操作的一个重要副作用是向所有后端（包括自己）发出 SI 失效消息，使引用该关系的计划按新的子表列表重建。即使发现
 * pg_class 行无需修改，也必须这样做。
 */
void
SetRelationHasSubclass(Oid relationId, bool relhassubclass)
{
	Relation	relationRelation;
	HeapTuple	tuple;
	Form_pg_class classtuple;

	Assert(CheckRelationOidLockedByMe(relationId,
									  ShareUpdateExclusiveLock, false) ||
		   CheckRelationOidLockedByMe(relationId,
									  ShareRowExclusiveLock, true));

	/*
	 * Fetch a modifiable copy of the tuple, modify it, update pg_class.
	 *
	 * 取出元组的可修改副本，改完后更新 pg_class。
	 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relationId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);
	classtuple = (Form_pg_class) GETSTRUCT(tuple);

	if (classtuple->relhassubclass != relhassubclass)
	{
		classtuple->relhassubclass = relhassubclass;
		CatalogTupleUpdate(relationRelation, &tuple->t_self, tuple);
	}
	else
	{
		/* no need to change tuple, but force relcache rebuild anyway */
		/*
		 *
		 * 元组不用改，但仍强制重建 relcache
		 */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}

/*
 * CheckRelationTableSpaceMove
 *		Check if relation can be moved to new tablespace.
 *
 * CheckRelationTableSpaceMove：检查关系能否移到新表空间。
 *
 * NOTE: The caller must hold AccessExclusiveLock on the relation.
 *
 * 注意：调用方必须对该关系持有 AccessExclusiveLock。
 *
 * Returns true if the relation can be moved to the new tablespace; raises
 * an error if it is not possible to do the move; returns false if the move
 * would have no effect.
 *
 * 能移动则返回真；不可能移动则报错；移动不会产生效果则返回假。
 */
bool
CheckRelationTableSpaceMove(Relation rel, Oid newTableSpaceId)
{
	Oid			oldTableSpaceId;

	/*
	 * No work if no change in tablespace.  Note that MyDatabaseTableSpace is
	 * stored as 0.
	 *
	 * 表空间没变就什么也不做。注意 MyDatabaseTableSpace 存成 0。
	 */
	oldTableSpaceId = rel->rd_rel->reltablespace;
	if (newTableSpaceId == oldTableSpaceId ||
		(newTableSpaceId == MyDatabaseTableSpace && oldTableSpaceId == 0))
		return false;

	/*
	 * We cannot support moving mapped relations into different tablespaces.
	 * (In particular this eliminates all shared catalogs.)
	 *
	 * 不支持把映射关系移到别的表空间。（这尤其排除了所有共享目录。）
	 */
	if (RelationIsMapped(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(rel))));

	/* Cannot move a non-shared relation into pg_global */
	/*
	 *
	 * 不能把非共享关系移进 pg_global
	 */
	if (newTableSpaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * Do not allow moving temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 *
	 * 不允许移动其他后端的临时表，它们的本地缓冲管理器应付不了。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move temporary tables of other sessions")));

	return true;
}

/*
 * SetRelationTableSpace
 *		Set new reltablespace and relfilenumber in pg_class entry.
 *
 * SetRelationTableSpace：在 pg_class 项里设置新的 reltablespace 和
 * relfilenumber。
 *
 * newTableSpaceId is the new tablespace for the relation, and
 * newRelFilenumber its new filenumber.  If newRelFilenumber is
 * InvalidRelFileNumber, this field is not updated.
 *
 * newTableSpaceId 是关系的新表空间，newRelFilenumber 是新的文件号。若
 * newRelFilenumber 为 InvalidRelFileNumber，则不更新该字段。
 *
 * NOTE: The caller must hold AccessExclusiveLock on the relation.
 *
 * 注意：调用方必须对该关系持有 AccessExclusiveLock。
 *
 * The caller of this routine had better check if a relation can be
 * moved to this new tablespace by calling CheckRelationTableSpaceMove()
 * first, and is responsible for making the change visible with
 * CommandCounterIncrement().
 *
 * 调用方最好先用 CheckRelationTableSpaceMove() 检查关系能否移到这个新表空间，并负责用
 * CommandCounterIncrement() 让修改可见。
 */
void
SetRelationTableSpace(Relation rel,
					  Oid newTableSpaceId,
					  RelFileNumber newRelFilenumber)
{
	Relation	pg_class;
	HeapTuple	tuple;
	ItemPointerData otid;
	Form_pg_class rd_rel;
	Oid			reloid = RelationGetRelid(rel);

	Assert(CheckRelationTableSpaceMove(rel, newTableSpaceId));

	/* Get a modifiable copy of the relation's pg_class row. */
	/*
	 *
	 * 取出该关系 pg_class 行的可修改副本。
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", reloid);
	otid = tuple->t_self;
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* Update the pg_class row. */
	/*
	 *
	 * 更新 pg_class 行。
	 */
	rd_rel->reltablespace = (newTableSpaceId == MyDatabaseTableSpace) ?
		InvalidOid : newTableSpaceId;
	if (RelFileNumberIsValid(newRelFilenumber))
		rd_rel->relfilenode = newRelFilenumber;
	CatalogTupleUpdate(pg_class, &otid, tuple);
	UnlockTuple(pg_class, &otid, InplaceUpdateTupleLock);

	/*
	 * Record dependency on tablespace.  This is only required for relations
	 * that have no physical storage.
	 *
	 * 记录对表空间的依赖。只有没有物理存储的关系才需要。
	 */
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		changeDependencyOnTablespace(RelationRelationId, reloid,
									 rd_rel->reltablespace);

	heap_freetuple(tuple);
	table_close(pg_class, RowExclusiveLock);
}

/*
 *		renameatt_check			- basic sanity checks before attribute rename
 *
 * renameatt_check：属性改名前的基本健全性检查
 */
static void
renameatt_check(Oid myrelid, Form_pg_class classform, bool recursing)
{
	char		relkind = classform->relkind;

	if (classform->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot rename column of typed table")));

	/*
	 * Renaming the columns of sequences or toast tables doesn't actually
	 * break anything from the system's point of view, since internal
	 * references are by attnum.  But it doesn't seem right to allow users to
	 * change names that are hardcoded into the system, hence the following
	 * restriction.
	 *
	 * 从系统角度看，给序列或 TOAST 表的列改名其实不会弄坏什么，内部引用用的是 attnum。但不该让用户改掉写死在系统里的名字，
	 * 所以有下面的限制。
	 */
	if (relkind != RELKIND_RELATION &&
		relkind != RELKIND_VIEW &&
		relkind != RELKIND_MATVIEW &&
		relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX &&
		relkind != RELKIND_FOREIGN_TABLE &&
		relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot rename columns of relation \"%s\"",
						NameStr(classform->relname)),
				 errdetail_relkind_not_supported(relkind)));

	/*
	 * permissions checking.  only the owner of a class can change its schema.
	 *
	 * 权限检查。只有类的属主才能改它的模式。
	 */
	if (!object_ownercheck(RelationRelationId, myrelid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(myrelid)),
					   NameStr(classform->relname));
	if (!allowSystemTableMods && IsSystemClass(myrelid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						NameStr(classform->relname))));
}

/*
 *		renameatt_internal		- workhorse for renameatt
 *
 * renameatt_internal：renameatt 的实际干活函数
 *
 * Return value is the attribute number in the 'myrelid' relation.
 *
 * 返回值是 myrelid 关系中的属性号。
 */
static AttrNumber
renameatt_internal(Oid myrelid,
				   const char *oldattname,
				   const char *newattname,
				   bool recurse,
				   bool recursing,
				   int expected_parents,
				   DropBehavior behavior)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	atttup;
	Form_pg_attribute attform;
	AttrNumber	attnum;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.
	 *
	 * 对目标表加排他锁，直到事务结束才释放。
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);
	renameatt_check(myrelid, RelationGetForm(targetrelation), recursing);

	/*
	 * if the 'recurse' flag is set then we are supposed to rename this
	 * attribute in all classes that inherit from 'relname' (as well as in
	 * 'relname').
	 *
	 * 若 recurse 标志为真，就要在 relname 以及所有继承自它的类里给这个属性改名。
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or nothing.
	 *
	 * 权限问题或属性重名都会让整个事务中止，这正是我们要的：要么全做，要么全不做。
	 */
	if (recurse)
	{
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		/*
		 * we need the number of parents for each child so that the recursive
		 * calls to renameatt() can determine whether there are any parents
		 * outside the inheritance hierarchy being processed.
		 *
		 * 需要每个子表的父表个数，这样递归调用 renameatt() 时才能判断是否还有不在当前处理的继承层次之外的父表。
		 */
		child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
										 &child_numparents);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 *
		 * find_all_inheritors 会递归搜索继承层次，我们只要处理它返回的全部 relid。
		 */
		forboth(lo, child_oids, li, child_numparents)
		{
			Oid			childrelid = lfirst_oid(lo);
			int			numparents = lfirst_int(li);

			if (childrelid == myrelid)
				continue;
			/* note we need not recurse again */
			/*
			 *
			 * 注意不必再递归一次
			 */
			renameatt_internal(childrelid, oldattname, newattname, false, true, numparents, behavior);
		}
	}
	else
	{
		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables; else the rename would put them out of step.
		 *
		 * 若被告知不要递归，最好没有子表；否则改名会让子表和父表对不上。
		 *
		 * expected_parents will only be 0 if we are not already recursing.
		 *
		 * 只有在还没开始递归时，expected_parents 才会是 0。
		 */
		if (expected_parents == 0 &&
			find_inheritance_children(myrelid, NoLock) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("inherited column \"%s\" must be renamed in child tables too",
							oldattname)));
	}

	/* rename attributes in typed tables of composite type */
	/*
	 *
	 * 给复合类型的类型表改属性名
	 */
	if (targetrelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
	{
		List	   *child_oids;
		ListCell   *lo;

		child_oids = find_typed_table_dependencies(targetrelation->rd_rel->reltype,
												   RelationGetRelationName(targetrelation),
												   behavior);

		foreach(lo, child_oids)
			renameatt_internal(lfirst_oid(lo), oldattname, newattname, true, true, 0, behavior);
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	atttup = SearchSysCacheCopyAttName(myrelid, oldattname);
	if (!HeapTupleIsValid(atttup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist",
						oldattname)));
	attform = (Form_pg_attribute) GETSTRUCT(atttup);

	attnum = attform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot rename system column \"%s\"",
						oldattname)));

	/*
	 * if the attribute is inherited, forbid the renaming.  if this is a
	 * top-level call to renameatt(), then expected_parents will be 0, so the
	 * effect of this code will be to prohibit the renaming if the attribute
	 * is inherited at all.  if this is a recursive call to renameatt(),
	 * expected_parents will be the number of parents the current relation has
	 * within the inheritance hierarchy being processed, so we'll prohibit the
	 * renaming only if there are additional parents from elsewhere.
	 *
	 * 若属性是继承来的，禁止改名。若这是 renameatt() 的顶层调用，expected_parents 为 0，
	 * 效果是只要属性有任何继承就禁止改名。若这是递归调用，expected_parents 是当前关系在正在处理的继承层次内的父表数，
	 * 因此只有还存在别处的额外父表时才禁止改名。
	 */
	if (attform->attinhcount > expected_parents)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot rename inherited column \"%s\"",
						oldattname)));

	/* new name should not already exist */
	/*
	 *
	 * 新名字不该已经存在
	 */
	(void) check_for_column_name_collision(targetrelation, newattname, false);

	/* apply the update */
	/*
	 *
	 * 执行更新
	 */
	namestrcpy(&(attform->attname), newattname);

	CatalogTupleUpdate(attrelation, &atttup->t_self, atttup);

	InvokeObjectPostAlterHook(RelationRelationId, myrelid, attnum);

	heap_freetuple(atttup);

	table_close(attrelation, RowExclusiveLock);

	relation_close(targetrelation, NoLock); /* close rel but keep lock */
						/*
						 *
						 * 关闭关系但保留锁
						 */

	return attnum;
}

/*
 * Perform permissions and integrity checks before acquiring a relation lock.
 *
 * 在获取关系锁之前做权限和完整性检查。
 */
static void
RangeVarCallbackForRenameAttribute(const RangeVar *rv, Oid relid, Oid oldrelid,
								   void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
							/*
							 *
							 * 已被并发删除
							 */
	form = (Form_pg_class) GETSTRUCT(tuple);
	renameatt_check(relid, form, false);
	ReleaseSysCache(tuple);
}

/*
 *		renameatt		- changes the name of an attribute in a relation
 *
 * renameatt：修改关系中某个属性的名字
 *
 * The returned ObjectAddress is that of the renamed column.
 *
 * 返回的 ObjectAddress 是被改名的那一列。
 */
ObjectAddress
renameatt(RenameStmt *stmt)
{
	Oid			relid;
	AttrNumber	attnum;
	ObjectAddress address;

	/* lock level taken here should match renameatt_internal */
	/*
	 *
	 * 这里取得的锁级别应与 renameatt_internal 一致
	 */
	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok ? RVR_MISSING_OK : 0,
									 RangeVarCallbackForRenameAttribute,
									 NULL);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	attnum =
		renameatt_internal(relid,
						   stmt->subname,	/* old att name */
									/*
									 *
									 * 旧属性名
									 */
						   stmt->newname,	/* new att name */
									/*
									 *
									 * 新属性名
									 */
						   stmt->relation->inh, /* recursive? */
									/*
									 *
									 * 是否递归？
									 */
						   false,	/* recursing? */
								/*
								 *
								 * 是否正处于递归中？
								 */
						   0,	/* expected inhcount */
							/*
							 *
							 * 期望的 inhcount
							 */
						   stmt->behavior);

	ObjectAddressSubSet(address, RelationRelationId, relid, attnum);

	return address;
}

/*
 * same logic as renameatt_internal
 *
 * 逻辑与 renameatt_internal 相同
 */
static ObjectAddress
rename_constraint_internal(Oid myrelid,
						   Oid mytypid,
						   const char *oldconname,
						   const char *newconname,
						   bool recurse,
						   bool recursing,
						   int expected_parents)
{
	Relation	targetrelation = NULL;
	Oid			constraintOid;
	HeapTuple	tuple;
	Form_pg_constraint con;
	ObjectAddress address;

	Assert(!myrelid || !mytypid);

	if (mytypid)
	{
		constraintOid = get_domain_constraint_oid(mytypid, oldconname, false);
	}
	else
	{
		targetrelation = relation_open(myrelid, AccessExclusiveLock);

		/*
		 * don't tell it whether we're recursing; we allow changing typed
		 * tables here
		 *
		 * 不告诉它我们是否在递归；这里允许修改类型表
		 */
		renameatt_check(myrelid, RelationGetForm(targetrelation), false);

		constraintOid = get_relation_constraint_oid(myrelid, oldconname, false);
	}

	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u",
			 constraintOid);
	con = (Form_pg_constraint) GETSTRUCT(tuple);

	if (myrelid &&
		(con->contype == CONSTRAINT_CHECK ||
		 con->contype == CONSTRAINT_NOTNULL) &&
		!con->connoinherit)
	{
		if (recurse)
		{
			List	   *child_oids,
					   *child_numparents;
			ListCell   *lo,
					   *li;

			child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
											 &child_numparents);

			forboth(lo, child_oids, li, child_numparents)
			{
				Oid			childrelid = lfirst_oid(lo);
				int			numparents = lfirst_int(li);

				if (childrelid == myrelid)
					continue;

				rename_constraint_internal(childrelid, InvalidOid, oldconname, newconname, false, true, numparents);
			}
		}
		else
		{
			if (expected_parents == 0 &&
				find_inheritance_children(myrelid, NoLock) != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("inherited constraint \"%s\" must be renamed in child tables too",
								oldconname)));
		}

		if (con->coninhcount > expected_parents)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot rename inherited constraint \"%s\"",
							oldconname)));
	}

	if (con->conindid
		&& (con->contype == CONSTRAINT_PRIMARY
			|| con->contype == CONSTRAINT_UNIQUE
			|| con->contype == CONSTRAINT_EXCLUSION))
		/* rename the index; this renames the constraint as well */
		/*
		 *
		 * 给索引改名；约束会一起改名
		 */
		RenameRelationInternal(con->conindid, newconname, false, true);
	else
		RenameConstraintById(constraintOid, newconname);

	ObjectAddressSet(address, ConstraintRelationId, constraintOid);

	ReleaseSysCache(tuple);

	if (targetrelation)
	{
		/*
		 * Invalidate relcache so as others can see the new constraint name.
		 *
		 * 使 relcache 失效，以便别人能看见新的约束名。
		 */
		CacheInvalidateRelcache(targetrelation);

		relation_close(targetrelation, NoLock); /* close rel but keep lock */
							/*
							 *
							 * 关闭关系但保留锁
							 */
	}

	return address;
}

/*
 * 重命名约束。处理表上的约束或域约束，必要时连同支撑索引一起改名。
 */
ObjectAddress
RenameConstraint(RenameStmt *stmt)
{
	Oid			relid = InvalidOid;
	Oid			typid = InvalidOid;

	if (stmt->renameType == OBJECT_DOMCONSTRAINT)
	{
		Relation	rel;
		HeapTuple	tup;

		typid = typenameTypeId(NULL, makeTypeNameFromNameList(castNode(List, stmt->object)));
		rel = table_open(TypeRelationId, RowExclusiveLock);
		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typid);
		checkDomainOwner(tup);
		ReleaseSysCache(tup);
		table_close(rel, NoLock);
	}
	else
	{
		/* lock level taken here should match rename_constraint_internal */
		/*
		 *
		 * 这里取得的锁级别应与 rename_constraint_internal 一致
		 */
		relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
										 stmt->missing_ok ? RVR_MISSING_OK : 0,
										 RangeVarCallbackForRenameAttribute,
										 NULL);
		if (!OidIsValid(relid))
		{
			ereport(NOTICE,
					(errmsg("relation \"%s\" does not exist, skipping",
							stmt->relation->relname)));
			return InvalidObjectAddress;
		}
	}

	return
		rename_constraint_internal(relid, typid,
								   stmt->subname,
								   stmt->newname,
								   (stmt->relation &&
									stmt->relation->inh),	/* recursive? */
												/*
												 *
												 * 是否递归？
												 */
								   false,	/* recursing? */
										/*
										 *
										 * 是否正处于递归中？
										 */
								   0 /* expected inhcount */ );
								     /*
								      *
								      * 期望的 inhcount
								      */
}

/*
 * Execute ALTER TABLE/INDEX/SEQUENCE/VIEW/MATERIALIZED VIEW/FOREIGN TABLE
 * RENAME
 *
 * 执行 ALTER TABLE/INDEX/SEQUENCE/VIEW/MATERIALIZED VIEW/FOREIGN
 * TABLE RENAME
 */
ObjectAddress
RenameRelation(RenameStmt *stmt)
{
	bool		is_index_stmt = stmt->renameType == OBJECT_INDEX;
	Oid			relid;
	ObjectAddress address;

	/*
	 * Grab an exclusive lock on the target table, index, sequence, view,
	 * materialized view, or foreign table, which we will NOT release until
	 * end of transaction.
	 *
	 * 对目标表、索引、序列、视图、物化视图或外部表加排他锁，直到事务结束才释放。
	 *
	 * Lock level used here should match RenameRelationInternal, to avoid lock
	 * escalation.  However, because ALTER INDEX can be used with any relation
	 * type, we mustn't believe without verification.
	 *
	 * 这里用的锁级别应与 RenameRelationInternal 一致，避免锁升级。不过 ALTER INDEX
	 * 可以用于任何关系类型，所以不能未经核实就相信它。
	 */
	for (;;)
	{
		LOCKMODE	lockmode;
		char		relkind;
		bool		obj_is_index;

		lockmode = is_index_stmt ? ShareUpdateExclusiveLock : AccessExclusiveLock;

		relid = RangeVarGetRelidExtended(stmt->relation, lockmode,
										 stmt->missing_ok ? RVR_MISSING_OK : 0,
										 RangeVarCallbackForAlterRelation,
										 stmt);

		if (!OidIsValid(relid))
		{
			ereport(NOTICE,
					(errmsg("relation \"%s\" does not exist, skipping",
							stmt->relation->relname)));
			return InvalidObjectAddress;
		}

		/*
		 * We allow mismatched statement and object types (e.g., ALTER INDEX
		 * to rename a table), but we might've used the wrong lock level.  If
		 * that happens, retry with the correct lock level.  We don't bother
		 * if we already acquired AccessExclusiveLock with an index, however.
		 *
		 * 我们允许语句类型和对象类型不一致（例如用 ALTER INDEX 给表改名），但那样可能用了错误的锁级别。若发生这种情况，
		 * 用正确的锁级别重试。不过若已经用索引拿到了 AccessExclusiveLock，就不必再折腾。
		 */
		relkind = get_rel_relkind(relid);
		obj_is_index = (relkind == RELKIND_INDEX ||
						relkind == RELKIND_PARTITIONED_INDEX);
		if (obj_is_index || is_index_stmt == obj_is_index)
			break;

		UnlockRelationOid(relid, lockmode);
		is_index_stmt = obj_is_index;
	}

	/* Do the work */
	/*
	 *
	 * 开始干活
	 */
	RenameRelationInternal(relid, stmt->newname, false, is_index_stmt);

	ObjectAddressSet(address, RelationRelationId, relid);

	return address;
}

/*
 *		RenameRelationInternal - change the name of a relation
 *
 * RenameRelationInternal：修改关系的名字
 */
void
RenameRelationInternal(Oid myrelid, const char *newrelname, bool is_internal, bool is_index)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
					/*
					 *
					 * 针对 RELATION 关系
					 */
	ItemPointerData otid;
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			namespaceId;

	/*
	 * Grab a lock on the target relation, which we will NOT release until end
	 * of transaction.  We need at least a self-exclusive lock so that
	 * concurrent DDL doesn't overwrite the rename if they start updating
	 * while still seeing the old version.  The lock also guards against
	 * triggering relcache reloads in concurrent sessions, which might not
	 * handle this information changing under them.  For indexes, we can use a
	 * reduced lock level because RelationReloadIndexInfo() handles indexes
	 * specially.
	 *
	 * 锁住目标关系，直到事务结束才释放。至少要自排他锁，免得并发 DDL 在仍看见旧版本时开始更新、把这次改名盖掉。
	 * 这把锁也防止并发会话触发 relcache 重载，它们可能处理不了眼皮底下变化的信息。对索引可以用更低的锁，因为
	 * RelationReloadIndexInfo() 对索引有专门处理。
	 */
	targetrelation = relation_open(myrelid, is_index ? ShareUpdateExclusiveLock : AccessExclusiveLock);
	namespaceId = RelationGetNamespace(targetrelation);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in use.
	 *
	 * 找到关系的 pg_class 元组，并确认 newrelname 没有被占用。
	 */
	relrelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))	/* shouldn't happen */
					/*
					 *
					 * 不该发生
					 */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	otid = reltup->t_self;
	relform = (Form_pg_class) GETSTRUCT(reltup);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname)));

	/*
	 * RenameRelation is careful not to believe the caller's idea of the
	 * relation kind being handled.  We don't have to worry about this, but
	 * let's not be totally oblivious to it.  We can process an index as
	 * not-an-index, but not the other way around.
	 *
	 * RenameRelation 小心地不轻信调用方对关系种类的判断。这里不必担心这一点，但也不要完全无视。可以把索引当成非索引来处理，
	 * 反过来不行。
	 */
	Assert(!is_index ||
		   is_index == (targetrelation->rd_rel->relkind == RELKIND_INDEX ||
						targetrelation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX));

	/*
	 * Update pg_class tuple with new relname.  (Scribbling on reltup is OK
	 * because it's a copy...)
	 *
	 * 用新的 relname 更新 pg_class 元组。（在 reltup 上涂改没问题，因为它是副本……）
	 */
	namestrcpy(&(relform->relname), newrelname);

	CatalogTupleUpdate(relrelation, &otid, reltup);
	UnlockTuple(relrelation, &otid, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHookArg(RelationRelationId, myrelid, 0,
								 InvalidOid, is_internal);

	heap_freetuple(reltup);
	table_close(relrelation, RowExclusiveLock);

	/*
	 * Also rename the associated type, if any.
	 *
	 * 若有关联类型，也给它改名。
	 */
	if (OidIsValid(targetrelation->rd_rel->reltype))
		RenameTypeInternal(targetrelation->rd_rel->reltype,
						   newrelname, namespaceId);

	/*
	 * Also rename the associated constraint, if any.
	 *
	 * 若有关联约束，也给它改名。
	 */
	if (targetrelation->rd_rel->relkind == RELKIND_INDEX ||
		targetrelation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		Oid			constraintId = get_index_constraint(myrelid);

		if (OidIsValid(constraintId))
			RenameConstraintById(constraintId, newrelname);
	}

	/*
	 * Close rel, but keep lock!
	 *
	 * 关闭关系，但保留锁！
	 */
	relation_close(targetrelation, NoLock);
}

/*
 *		ResetRelRewrite - reset relrewrite
 *
 * ResetRelRewrite：清掉 relrewrite
 */
void
ResetRelRewrite(Oid myrelid)
{
	Relation	relrelation;	/* for RELATION relation */
					/*
					 *
					 * 针对 RELATION 关系
					 */
	HeapTuple	reltup;
	Form_pg_class relform;

	/*
	 * Find relation's pg_class tuple.
	 *
	 * 找到关系的 pg_class 元组。
	 */
	relrelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))	/* shouldn't happen */
					/*
					 *
					 * 不该发生
					 */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	/*
	 * Update pg_class tuple.
	 *
	 * 更新 pg_class 元组。
	 */
	relform->relrewrite = InvalidOid;

	CatalogTupleUpdate(relrelation, &reltup->t_self, reltup);

	heap_freetuple(reltup);
	table_close(relrelation, RowExclusiveLock);
}

/*
 * Disallow ALTER TABLE (and similar commands) when the current backend has
 * any open reference to the target table besides the one just acquired by
 * the calling command; this implies there's an open cursor or active plan.
 * We need this check because our lock doesn't protect us against stomping
 * on our own foot, only other people's feet!
 *
 * 当前后端除了本命令刚拿到的那一个引用之外，若还打开着目标表的任何引用，就禁止 ALTER TABLE（及类似命令）；
 * 这意味着有打开的游标或活动计划。需要这项检查，是因为我们的锁防不住自己踩自己的脚，只能防别人的脚！
 *
 * For ALTER TABLE, the only case known to cause serious trouble is ALTER
 * COLUMN TYPE, and some changes are obviously pretty benign, so this could
 * possibly be relaxed to only error out for certain types of alterations.
 * But the use-case for allowing any of these things is not obvious, so we
 * won't work hard at it for now.
 *
 * 对 ALTER TABLE 来说，已知会惹大麻烦的主要是 ALTER COLUMN TYPE，有些改动显然相当无害，
 * 所以也许可以放宽到只对某些类型的修改报错。但允许这些操作的使用场景并不明显，所以暂时不在这上面花力气。
 *
 * We also reject these commands if there are any pending AFTER trigger events
 * for the rel.  This is certainly necessary for the rewriting variants of
 * ALTER TABLE, because they don't preserve tuple TIDs and so the pending
 * events would try to fetch the wrong tuples.  It might be overly cautious
 * in other cases, but again it seems better to err on the side of paranoia.
 *
 * 若该关系还有未处理的 AFTER 触发器事件，也拒绝这些命令。对会重写的 ALTER TABLE 变体这肯定有必要，
 * 因为它们不保留元组 TID，挂起的事件会去取错误的元组。其他情况下也许过于谨慎，但宁可偏执一点。
 *
 * REINDEX calls this with "rel" referencing the index to be rebuilt; here
 * we are worried about active indexscans on the index.  The trigger-event
 * check can be skipped, since we are doing no damage to the parent table.
 *
 * REINDEX 调用这里时，rel 指向要重建的索引；我们担心的是该索引上的活动索引扫描。触发器事件检查可以跳过，因为不会破坏父表。
 *
 * The statement name (eg, "ALTER TABLE") is passed for use in error messages.
 *
 * 语句名（例如 ALTER TABLE）传进来用于报错信息。
 */
void
CheckTableNotInUse(Relation rel, const char *stmt)
{
	int			expected_refcnt;

	expected_refcnt = rel->rd_isnailed ? 2 : 1;
	if (rel->rd_refcnt != expected_refcnt)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
		/* translator: first %s is a SQL command, eg ALTER TABLE */
		/*
		 *
		 * 翻译提示：第一个 %s 是 SQL 命令，例如 ALTER TABLE
		 */
				 errmsg("cannot %s \"%s\" because it is being used by active queries in this session",
						stmt, RelationGetRelationName(rel))));

	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX &&
		AfterTriggerPendingOnRel(RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
		/* translator: first %s is a SQL command, eg ALTER TABLE */
		/*
		 *
		 * 翻译提示：第一个 %s 是 SQL 命令，例如 ALTER TABLE
		 */
				 errmsg("cannot %s \"%s\" because it has pending trigger events",
						stmt, RelationGetRelationName(rel))));
}

/*
 * CheckAlterTableIsSafe
 *		Verify that it's safe to allow ALTER TABLE on this relation.
 *
 * CheckAlterTableIsSafe：确认对这个关系做 ALTER TABLE 是安全的。
 *
 * This consists of CheckTableNotInUse() plus a check that the relation
 * isn't another session's temp table.  We must split out the temp-table
 * check because there are callers of CheckTableNotInUse() that don't want
 * that, notably DROP TABLE.  (We must allow DROP or we couldn't clean out
 * an orphaned temp schema.)  Compare truncate_check_activity().
 *
 * 这包括 CheckTableNotInUse()，再加上关系不是另一个会话的临时表。临时表检查必须拆出来，因为有些
 * CheckTableNotInUse() 的调用方不想要它，尤其是 DROP TABLE。（必须允许 DROP，
 * 否则清不掉孤儿临时模式。）对照 truncate_check_activity()。
 */
static void
CheckAlterTableIsSafe(Relation rel)
{
	/*
	 * Don't allow ALTER on temp tables of other backends.  Their local buffer
	 * manager is not going to cope if we need to change the table's contents.
	 * Even if we don't, there may be optimizations that assume temp tables
	 * aren't subject to such interference.
	 *
	 * 不允许 ALTER 其他后端的临时表。若需要改表内容，它们的本地缓冲管理器应付不了。即使不改内容，
	 * 也可能有优化假定临时表不会受到这种干扰。
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 *
	 * 还要检查当前事务里是否仍在使用该关系，包括打开的扫描和尚未处理的 AFTER 触发器事件。
	 */
	CheckTableNotInUse(rel, "ALTER TABLE");
}

/*
 * AlterTableLookupRelation
 *		Look up, and lock, the OID for the relation named by an alter table
 *		statement.
 *
 * AlterTableLookupRelation：查找 ALTER TABLE 语句所点名的关系的 OID，并加锁。
 */
Oid
AlterTableLookupRelation(AlterTableStmt *stmt, LOCKMODE lockmode)
{
	return RangeVarGetRelidExtended(stmt->relation, lockmode,
									stmt->missing_ok ? RVR_MISSING_OK : 0,
									RangeVarCallbackForAlterRelation,
									stmt);
}

/*
 * AlterTable
 *		Execute ALTER TABLE, which can be a list of subcommands
 *
 * AlterTable：执行 ALTER TABLE，它可以是一串子命令
 *
 * ALTER TABLE is performed in three phases:
 *		1. Examine subcommands and perform pre-transformation checking.
 *		2. Validate and transform subcommands, and update system catalogs.
 *		3. Scan table(s) to check new constraints, and optionally recopy
 *		   the data into new table(s).
 * Phase 3 is not performed unless one or more of the subcommands requires
 * it.  The intention of this design is to allow multiple independent
 * updates of the table schema to be performed with only one pass over the
 * data.
 *
 * ALTER TABLE 分三阶段：1. 检查子命令并做变换前的检查。2. 校验并变换子命令，更新系统目录。3. 扫描表以检查新约束，
 * 并可选地把数据重拷到新表。只有某个子命令需要时才做 Phase 3。这样设计是为了让对表模式的多次独立更新只扫一遍数据。
 *
 * ATPrepCmd performs phase 1.  A "work queue" entry is created for
 * each table to be affected (there may be multiple affected tables if the
 * commands traverse a table inheritance hierarchy).  Also we do preliminary
 * validation of the subcommands.  Because earlier subcommands may change
 * the catalog state seen by later commands, there are limits to what can
 * be done in this phase.  Generally, this phase acquires table locks,
 * checks permissions and relkind, and recurses to find child tables.
 *
 * ATPrepCmd 执行阶段 1。每个受影响的表建一条工作队列项（命令若沿着表继承层次走，可能影响多张表）。同时对子命令做初步校验。
 * 因为前面的子命令可能改变后面命令看到的目录状态，这一阶段能做的事有限。一般是加表锁、检查权限和 relkind，并递归找出子表。
 *
 * ATRewriteCatalogs performs phase 2 for each affected table.
 * Certain subcommands need to be performed before others to avoid
 * unnecessary conflicts; for example, DROP COLUMN should come before
 * ADD COLUMN.  Therefore phase 1 divides the subcommands into multiple
 * lists, one for each logical "pass" of phase 2.
 *
 * ATRewriteCatalogs 对每张受影响的表执行阶段 2。有些子命令必须排在别的前面，以免无谓冲突；例如 DROP
 * COLUMN 应在 ADD COLUMN 之前。因此阶段 1 把子命令分成多个列表，对应阶段 2 的每一遍逻辑处理。
 *
 * ATRewriteTables performs phase 3 for those tables that need it.
 *
 * ATRewriteTables 对需要的表执行阶段 3。
 *
 * For most subcommand types, phases 2 and 3 do no explicit recursion,
 * since phase 1 already does it.  However, for certain subcommand types
 * it is only possible to determine how to recurse at phase 2 time; for
 * those cases, phase 1 sets the cmd->recurse flag.
 *
 * 对大多数子命令类型，阶段 2 和 3 不再显式递归，因为阶段 1 已经做过。但有些类型只能到阶段 2 才知道如何递归；对这些，阶段
 * 1 会设置 cmd->recurse 标志。
 *
 * Thanks to the magic of MVCC, an error anywhere along the way rolls back
 * the whole operation; we don't have to do anything special to clean up.
 *
 * 靠 MVCC，中途任何错误都会回滚整个操作；不必为清理做特殊处理。
 *
 * The caller must lock the relation, with an appropriate lock level
 * for the subcommands requested, using AlterTableGetLockLevel(stmt->cmds)
 * or higher. We pass the lock level down
 * so that we can apply it recursively to inherited tables. Note that the
 * lock level we want as we recurse might well be higher than required for
 * that specific subcommand. So we pass down the overall lock requirement,
 * rather than reassess it at lower levels.
 *
 * 调用方必须用 AlterTableGetLockLevel(stmt->cmds) 或更强的锁锁住关系，锁级别要适合所请求的子命令。
 * 我们把锁级别传下去，以便递归用到继承表上。注意递归时想要的锁级别很可能高于该子命令本身的需要。所以传下去的是整体锁要求，
 * 而不是在下层重新评估。
 *
 * The caller also provides a "context" which is to be passed back to
 * utility.c when we need to execute a subcommand such as CREATE INDEX.
 * Some of the fields therein, such as the relid, are used here as well.
 *
 * 调用方还提供一个 context，需要执行 CREATE INDEX 这类子命令时再传回 utility.c。其中有些字段（例如
 * relid）这里也会用到。
 */
void
AlterTable(AlterTableStmt *stmt, LOCKMODE lockmode,
		   AlterTableUtilityContext *context)
{
	Relation	rel;

	/* Caller is required to provide an adequate lock. */
	/*
	 *
	 * 调用方必须提供足够的锁。
	 */
	rel = relation_open(context->relid, NoLock);

	CheckAlterTableIsSafe(rel);

	ATController(stmt, rel, stmt->cmds, stmt->relation->inh, lockmode, context);
}

/*
 * AlterTableInternal
 *
 * 函数 AlterTableInternal
 *
 * ALTER TABLE with target specified by OID
 *
 * 按 OID 指定目标的 ALTER TABLE
 *
 * We do not reject if the relation is already open, because it's quite
 * likely that one or more layers of caller have it open.  That means it
 * is unsafe to use this entry point for alterations that could break
 * existing query plans.  On the assumption it's not used for such, we
 * don't have to reject pending AFTER triggers, either.
 *
 * 关系已经打开时我们不拒绝，因为很可能有一层或多层调用方开着它。这意味着用这个入口做可能破坏现有查询计划的修改是不安全的。
 * 假定不会这么用，因此也不必拒绝挂起的 AFTER 触发器。
 *
 * Also, since we don't have an AlterTableUtilityContext, this cannot be
 * used for any subcommand types that require parse transformation or
 * could generate subcommands that have to be passed to ProcessUtility.
 *
 * 另外，因为没有 AlterTableUtilityContext，任何需要解析变换、或可能生成必须交给
 * ProcessUtility 的子命令的子命令类型，都不能用这个入口。
 */
void
AlterTableInternal(Oid relid, List *cmds, bool recurse)
{
	Relation	rel;
	LOCKMODE	lockmode = AlterTableGetLockLevel(cmds);

	rel = relation_open(relid, lockmode);

	EventTriggerAlterTableRelid(relid);

	ATController(NULL, rel, cmds, recurse, lockmode, NULL);
}

/*
 * AlterTableGetLockLevel
 *
 * 函数 AlterTableGetLockLevel
 *
 * Sets the overall lock level required for the supplied list of subcommands.
 * Policy for doing this set according to needs of AlterTable(), see
 * comments there for overall explanation.
 *
 * 根据给定的子命令列表确定所需的总体锁级别。策略按 AlterTable() 的需要来定，总体说明见那里的注释。
 *
 * Function is called before and after parsing, so it must give same
 * answer each time it is called. Some subcommands are transformed
 * into other subcommand types, so the transform must never be made to a
 * lower lock level than previously assigned. All transforms are noted below.
 *
 * 本函数在解析前后都会被调用，所以每次必须给出相同答案。有些子命令会变换成别的子命令类型，变换后的锁级别绝不能低于先前指定的。
 * 所有变换都在下面注明。
 *
 * Since this is called before we lock the table we cannot use table metadata
 * to influence the type of lock we acquire.
 *
 * 因为这时还没锁表，不能用表的元数据来影响要获取的锁类型。
 *
 * There should be no lockmodes hardcoded into the subcommand functions. All
 * lockmode decisions for ALTER TABLE are made here only. The one exception is
 * ALTER TABLE RENAME which is treated as a different statement type T_RenameStmt
 * and does not travel through this section of code and cannot be combined with
 * any of the subcommands given here.
 *
 * 子命令函数里不该写死锁模式。ALTER TABLE 的所有锁模式决定都只在这里做出。唯一例外是 ALTER TABLE
 * RENAME，它被当成另一种语句类型 T_RenameStmt，不走这段代码，也不能和这里的任何子命令组合。
 *
 * Note that Hot Standby only knows about AccessExclusiveLocks on the primary
 * so any changes that might affect SELECTs running on standbys need to use
 * AccessExclusiveLocks even if you think a lesser lock would do, unless you
 * have a solution for that also.
 *
 * 注意：热备只知道主库上的 AccessExclusiveLock，所以任何可能影响备库上正在跑的 SELECT 的修改，
 * 即使你觉得更弱的锁就够，也必须用 AccessExclusiveLock，除非你另外有办法解决这个问题。
 *
 * Also note that pg_dump uses only an AccessShareLock, meaning that anything
 * that takes a lock less than AccessExclusiveLock can change object definitions
 * while pg_dump is running. Be careful to check that the appropriate data is
 * derived by pg_dump using an MVCC snapshot, rather than syscache lookups,
 * otherwise we might end up with an inconsistent dump that can't restore.
 *
 * 另外注意 pg_dump 只用 AccessShareLock，这意味着任何拿比 AccessExclusiveLock
 * 更弱的锁的操作，都可能在 pg_dump 运行时改变对象定义。要小心确认 pg_dump 是用 MVCC 快照而不是
 * syscache 查找得出相应数据，否则可能得到无法恢复的不一致转储。
 */
LOCKMODE
AlterTableGetLockLevel(List *cmds)
{
	/*
	 * This only works if we read catalog tables using MVCC snapshots.
	 *
	 * 只有用 MVCC 快照读目录表时，这才成立。
	 */
	ListCell   *lcmd;
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;

	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		LOCKMODE	cmd_lockmode = AccessExclusiveLock; /* default for compiler */
								    /*
								     *
								     * 给编译器的默认值
								     */

		switch (cmd->subtype)
		{
				/*
				 * These subcommands rewrite the heap, so require full locks.
				 *
				 * 这些子命令会重写堆，因此需要完全的锁。
				 */
			case AT_AddColumn:	/* may rewrite heap, in some cases and visible
								 * to SELECT */
						/*
						 *
						 * 有时会重写堆，并且对 SELECT 可见
						 */
			case AT_SetAccessMethod:	/* must rewrite heap */
							/*
							 *
							 * 必须重写堆
							 */
			case AT_SetTableSpace:	/* must rewrite heap */
						/*
						 *
						 * 必须重写堆
						 */
			case AT_AlterColumnType:	/* must rewrite heap */
							/*
							 *
							 * 必须重写堆
							 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands may require addition of toast tables. If
				 * we add a toast table to a table currently being scanned, we
				 * might miss data added to the new toast table by concurrent
				 * insert transactions.
				 *
				 * 这些子命令可能需要加 TOAST 表。若给正在被扫描的表加 TOAST 表，可能会漏掉并发插入事务写进新 TOAST 表的数据。
				 */
			case AT_SetStorage: /* may add toast tables, see
								 * ATRewriteCatalogs() */
					    /*
					     *
					     * 可能添加 TOAST 表，见 ATRewriteCatalogs()
					     */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Removing constraints can affect SELECTs that have been
				 * optimized assuming the constraint holds true. See also
				 * CloneFkReferenced.
				 *
				 * 去掉约束可能影响那些假定该约束成立而做过优化的 SELECT。另见 CloneFkReferenced。
				 */
			case AT_DropConstraint: /* as DROP INDEX */
						/*
						 *
						 * 与 DROP INDEX 相同
						 */
			case AT_DropNotNull:	/* may change some SQL plans */
						/*
						 *
						 * 可能改变某些 SQL 计划
						 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Subcommands that may be visible to concurrent SELECTs
				 *
				 * 可能对并发 SELECT 可见的子命令
				 */
			case AT_DropColumn: /* change visible to SELECT */
					    /*
					     *
					     * 对 SELECT 可见的改动
					     */
			case AT_AddColumnToView:	/* CREATE VIEW */
							/*
							 *
							 * CREATE VIEW（创建视图）
							 */
			case AT_DropOids:	/* used to equiv to DropColumn */
						/*
						 *
						 * 过去等价于 DropColumn
						 */
			case AT_EnableAlwaysRule:	/* may change SELECT rules */
							/*
							 *
							 * 可能改变 SELECT 规则
							 */
			case AT_EnableReplicaRule:	/* may change SELECT rules */
							/*
							 *
							 * 可能改变 SELECT 规则
							 */
			case AT_EnableRule: /* may change SELECT rules */
					    /*
					     *
					     * 可能改变 SELECT 规则
					     */
			case AT_DisableRule:	/* may change SELECT rules */
						/*
						 *
						 * 可能改变 SELECT 规则
						 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing owner may remove implicit SELECT privileges
				 *
				 * 改属主可能去掉隐式的 SELECT 权限
				 */
			case AT_ChangeOwner:	/* change visible to SELECT */
						/*
						 *
						 * 对 SELECT 可见的改动
						 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Changing foreign table options may affect optimization.
				 *
				 * 修改外部表选项可能影响优化。
				 */
			case AT_GenericOptions:
			case AT_AlterColumnGenericOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect write operations only.
				 *
				 * 这些子命令只影响写操作。
				 */
			case AT_EnableTrig:
			case AT_EnableAlwaysTrig:
			case AT_EnableReplicaTrig:
			case AT_EnableTrigAll:
			case AT_EnableTrigUser:
			case AT_DisableTrig:
			case AT_DisableTrigAll:
			case AT_DisableTrigUser:
				cmd_lockmode = ShareRowExclusiveLock;
				break;

				/*
				 * These subcommands affect write operations only. XXX
				 * Theoretically, these could be ShareRowExclusiveLock.
				 *
				 * 这些子命令只影响写操作。XXX 理论上这些可以是 ShareRowExclusiveLock。
				 */
			case AT_ColumnDefault:
			case AT_CookedColumnDefault:
			case AT_AlterConstraint:
			case AT_AddIndex:	/* from ADD CONSTRAINT */
						/*
						 *
						 * 来自 ADD CONSTRAINT
						 */
			case AT_AddIndexConstraint:
			case AT_ReplicaIdentity:
			case AT_SetNotNull:
			case AT_EnableRowSecurity:
			case AT_DisableRowSecurity:
			case AT_ForceRowSecurity:
			case AT_NoForceRowSecurity:
			case AT_AddIdentity:
			case AT_DropIdentity:
			case AT_SetIdentity:
			case AT_SetExpression:
			case AT_DropExpression:
			case AT_SetCompression:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_AddConstraint:
			case AT_ReAddConstraint:	/* becomes AT_AddConstraint */
							/*
							 *
							 * 变成 AT_AddConstraint
							 */
			case AT_ReAddDomainConstraint:	/* becomes AT_AddConstraint */
							/*
							 *
							 * 变成 AT_AddConstraint
							 */
				if (IsA(cmd->def, Constraint))
				{
					Constraint *con = (Constraint *) cmd->def;

					switch (con->contype)
					{
						case CONSTR_EXCLUSION:
						case CONSTR_PRIMARY:
						case CONSTR_UNIQUE:

							/*
							 * Cases essentially the same as CREATE INDEX. We
							 * could reduce the lock strength to ShareLock if
							 * we can work out how to allow concurrent catalog
							 * updates. XXX Might be set down to
							 * ShareRowExclusiveLock but requires further
							 * analysis.
							 *
							 * 情形本质上和 CREATE INDEX 一样。若能想出如何允许并发的目录更新，可以把锁降到 ShareLock。XXX
							 * 也许可以降到 ShareRowExclusiveLock，但还需要进一步分析。
							 */
							cmd_lockmode = AccessExclusiveLock;
							break;
						case CONSTR_FOREIGN:

							/*
							 * We add triggers to both tables when we add a
							 * Foreign Key, so the lock level must be at least
							 * as strong as CREATE TRIGGER.
							 *
							 * 加外键时会给两张表都加触发器，所以锁级别至少要和 CREATE TRIGGER 一样强。
							 */
							cmd_lockmode = ShareRowExclusiveLock;
							break;

						default:
							cmd_lockmode = AccessExclusiveLock;
					}
				}
				break;

				/*
				 * These subcommands affect inheritance behaviour. Queries
				 * started before us will continue to see the old inheritance
				 * behaviour, while queries started after we commit will see
				 * new behaviour. No need to prevent reads or writes to the
				 * subtable while we hook it up though. Changing the TupDesc
				 * may be a problem, so keep highest lock.
				 *
				 * 这些子命令影响继承行为。在我们之前开始的查询会继续看到旧的继承行为，提交之后开始的查询会看到新行为。
				 * 把子表挂上来时不必阻止对它的读写。但改变 TupDesc 可能有问题，所以保持最高锁。
				 */
			case AT_AddInherit:
			case AT_DropInherit:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect implicit row type conversion. They
				 * have affects similar to CREATE/DROP CAST on queries. don't
				 * provide for invalidating parse trees as a result of such
				 * changes, so we keep these at AccessExclusiveLock.
				 *
				 * 这些子命令影响隐式行类型转换，对查询的影响类似 CREATE/DROP CAST。目前没有办法因这类改动而使语法树失效，所以保持
				 * AccessExclusiveLock。
				 */
			case AT_AddOf:
			case AT_DropOf:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * Only used by CREATE OR REPLACE VIEW which must conflict
				 * with an SELECTs currently using the view.
				 *
				 * 只用于 CREATE OR REPLACE VIEW，它必须和当前正在使用该视图的 SELECT 冲突。
				 */
			case AT_ReplaceRelOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * These subcommands affect general strategies for performance
				 * and maintenance, though don't change the semantic results
				 * from normal data reads and writes. Delaying an ALTER TABLE
				 * behind currently active writes only delays the point where
				 * the new strategy begins to take effect, so there is no
				 * benefit in waiting. In this case the minimum restriction
				 * applies: we don't currently allow concurrent catalog
				 * updates.
				 *
				 * 这些子命令影响性能和维护的总体策略，但不改变普通读写的语义结果。让 ALTER TABLE 等在当前活跃的写后面，
				 * 只是推迟新策略生效的时刻，等并没有好处。这种情况下适用最低限制：我们目前不允许并发的目录更新。
				 */
			case AT_SetStatistics:	/* Uses MVCC in getTableAttrs() */
						/*
						 *
						 * 在 getTableAttrs() 中使用 MVCC
						 */
			case AT_ClusterOn:	/* Uses MVCC in getIndexes() */
						/*
						 *
						 * 在 getIndexes() 中使用 MVCC
						 */
			case AT_DropCluster:	/* Uses MVCC in getIndexes() */
						/*
						 *
						 * 在 getIndexes() 中使用 MVCC
						 */
			case AT_SetOptions: /* Uses MVCC in getTableAttrs() */
					    /*
					     *
					     * 在 getTableAttrs() 中使用 MVCC
					     */
			case AT_ResetOptions:	/* Uses MVCC in getTableAttrs() */
						/*
						 *
						 * 在 getTableAttrs() 中使用 MVCC
						 */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_SetLogged:
			case AT_SetUnLogged:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_ValidateConstraint: /* Uses MVCC in getConstraints() */
						    /*
						     *
						     * 在 getConstraints() 中使用 MVCC
						     */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

				/*
				 * Rel options are more complex than first appears. Options
				 * are set here for tables, views and indexes; for historical
				 * reasons these can all be used with ALTER TABLE, so we can't
				 * decide between them using the basic grammar.
				 *
				 * 关系选项比乍看更复杂。这里给表、视图和索引设置选项；由于历史原因，它们都能用 ALTER TABLE，
				 * 所以不能靠基本语法把它们区分开。
				 */
			case AT_SetRelOptions:	/* Uses MVCC in getIndexes() and
									 * getTables() */
						/*
						 *
						 * 在 getIndexes() 和 getTables() 中使用 MVCC
						 */
			case AT_ResetRelOptions:	/* Uses MVCC in getIndexes() and
										 * getTables() */
							/*
							 *
							 * 在 getIndexes() 和 getTables() 中使用 MVCC
							 */
				cmd_lockmode = AlterTableGetRelOptionsLockLevel((List *) cmd->def);
				break;

			case AT_AttachPartition:
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_DetachPartition:
				if (((PartitionCmd *) cmd->def)->concurrent)
					cmd_lockmode = ShareUpdateExclusiveLock;
				else
					cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_DetachPartitionFinalize:
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			default:			/* oops */
							/*
							 *
							 * 不该走到这里
							 */
				elog(ERROR, "unrecognized alter table type: %d",
					 (int) cmd->subtype);
				break;
		}

		/*
		 * Take the greatest lockmode from any subcommand
		 *
		 * 取所有子命令里最强的锁模式
		 */
		if (cmd_lockmode > lockmode)
			lockmode = cmd_lockmode;
	}

	return lockmode;
}

/*
 * ATController provides top level control over the phases.
 *
 * ATController 对各个阶段做顶层控制。
 *
 * parsetree is passed in to allow it to be passed to event triggers
 * when requested.
 *
 * 传入 parsetree，以便在需要时把它交给事件触发器。
 */
static void
ATController(AlterTableStmt *parsetree,
			 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
			 AlterTableUtilityContext *context)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;

	/* Phase 1: preliminary examination of commands, create work queue */
	/*
	 *
	 * Phase 1：初步检查命令，建立工作队列
	 */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		ATPrepCmd(&wqueue, rel, cmd, recurse, false, lockmode, context);
	}

	/* Close the relation, but keep lock until commit */
	/*
	 *
	 * 关闭关系，但锁保持到提交
	 */
	relation_close(rel, NoLock);

	/* Phase 2: update system catalogs */
	/*
	 *
	 * Phase 2：更新系统目录
	 */
	ATRewriteCatalogs(&wqueue, lockmode, context);

	/* Phase 3: scan/rewrite tables as needed, and run afterStmts */
	/*
	 *
	 * Phase 3：按需要扫描或重写表，并执行 afterStmts
	 */
	ATRewriteTables(parsetree, &wqueue, lockmode, context);
}

/*
 * ATPrepCmd
 *
 * 函数 ATPrepCmd
 *
 * Traffic cop for ALTER TABLE Phase 1 operations, including simple
 * recursion and permission checks.
 *
 * ALTER TABLE 阶段 1 的交通警察，包括简单递归和权限检查。
 *
 * Caller must have acquired appropriate lock type on relation already.
 * This lock should be held until commit.
 *
 * 调用方必须已经对关系拿到合适类型的锁。这把锁应保持到提交。
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing, LOCKMODE lockmode,
		  AlterTableUtilityContext *context)
{
	AlteredTableInfo *tab;
	AlterTablePass pass = AT_PASS_UNSET;

	/* Find or create work queue entry for this table */
	/*
	 *
	 * 查找或创建这张表的工作队列项
	 */
	tab = ATGetQueueEntry(wqueue, rel);

	/*
	 * Disallow any ALTER TABLE other than ALTER TABLE DETACH FINALIZE on
	 * partitions that are pending detach.
	 *
	 * 对正在等待分离完成的分区，除了 ALTER TABLE DETACH FINALIZE 之外，禁止任何 ALTER TABLE。
	 */
	if (rel->rd_rel->relispartition &&
		cmd->subtype != AT_DetachPartitionFinalize &&
		PartitionHasPendingDetach(RelationGetRelid(rel)))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot alter partition \"%s\" with an incomplete detach",
					   RelationGetRelationName(rel)),
				errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

	/*
	 * Copy the original subcommand for each table, so we can scribble on it.
	 * This avoids conflicts when different child tables need to make
	 * different parse transformations (for example, the same column may have
	 * different column numbers in different children).
	 *
	 * 为每张表复制原始子命令，以便在上面涂改。这样不同子表需要不同解析变换时不会冲突（例如同一列在不同子表里的列号可能不同）。
	 */
	cmd = copyObject(cmd);

	/*
	 * Do permissions and relkind checking, recursion to child tables if
	 * needed, and any additional phase-1 processing needed.  (But beware of
	 * adding any processing that looks at table details that another
	 * subcommand could change.  In some cases we reject multiple subcommands
	 * that could try to change the same state in contrary ways.)
	 *
	 * 做权限和 relkind 检查，需要时递归到子表，以及阶段 1 还需要的其他处理。
	 * （但小心不要加入会查看可能被另一个子命令改掉的表细节的处理。有时我们会拒绝多个可能以相反方式改同一状态的子命令。）
	 */
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
						/*
						 *
						 * ADD COLUMN（加列）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE |
								ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, false, cmd,
							lockmode, context);
			/* Recursion occurs during execution phase */
			/*
			 *
			 * 递归发生在执行阶段
			 */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_AddColumnToView:	/* add column via CREATE OR REPLACE VIEW */
						/*
						 *
						 * 通过 CREATE OR REPLACE VIEW 加列
						 */
			ATSimplePermissions(cmd->subtype, rel, ATT_VIEW);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, true, cmd,
							lockmode, context);
			/* Recursion occurs during execution phase */
			/*
			 *
			 * 递归发生在执行阶段
			 */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
					/*
					 *
					 * ALTER COLUMN DEFAULT（修改列默认值）
					 */

			/*
			 * We allow defaults on views so that INSERT into a view can have
			 * default-ish behavior.  This works because the rewriter
			 * substitutes default values into INSERTs before it expands
			 * rules.
			 *
			 * 允许视图有默认值，这样 INSERT 进视图可以有类似默认值的行为。这能工作，是因为重写器在展开规则之前就把默认值替换进
			 * INSERT。
			 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW |
								ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = cmd->def ? AT_PASS_ADD_OTHERCONSTR : AT_PASS_DROP;
			break;
		case AT_CookedColumnDefault:	/* add a pre-cooked default */
						/*
						 *
						 * 添加一个已经煮好的默认值
						 */
			/* This is currently used only in CREATE TABLE */
			/*
			 *
			 * 目前只在 CREATE TABLE 里使用
			 */
			/* (so the permission check really isn't necessary) */
			/*
			 *
			 * （所以权限检查其实并不必要）
			 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_AddIdentity:
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW |
								ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			/*
			 *
			 * 为阶段 2 准备递归；不需要其他准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_SetIdentity:
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW |
								ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			/*
			 *
			 * 为阶段 2 准备递归；不需要其他准备
			 */
			if (recurse)
				cmd->recurse = true;
			/* This should run after AddIdentity, so do it in MISC pass */
			/*
			 *
			 * 这应该在 AddIdentity 之后跑，所以放在 MISC 那一遍
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_DropIdentity:
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW |
								ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			/*
			 *
			 * 为阶段 2 准备递归；不需要其他准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
					/*
					 *
					 * ALTER COLUMN DROP NOT NULL（去掉非空）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			/*
			 *
			 * 为阶段 2 准备递归；不需要其他准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
						/*
						 *
						 * ALTER COLUMN SET NOT NULL（设为非空）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			/*
			 *
			 * 为阶段 2 准备递归；不需要其他准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_SetExpression:	/* ALTER COLUMN SET EXPRESSION */
					/*
					 *
					 * ALTER COLUMN SET EXPRESSION（设置生成表达式）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			pass = AT_PASS_SET_EXPRESSION;
			break;
		case AT_DropExpression: /* ALTER COLUMN DROP EXPRESSION */
					/*
					 *
					 * ALTER COLUMN DROP EXPRESSION（去掉生成表达式）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			ATPrepDropExpression(rel, cmd, recurse, recursing, lockmode);
			pass = AT_PASS_DROP;
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
					/*
					 *
					 * ALTER COLUMN SET STATISTICS（设置统计目标）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW |
								ATT_INDEX | ATT_PARTITIONED_INDEX | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( options ) */
						/*
						 *
						 * ALTER COLUMN SET ( options )（设置列选项）
						 */
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( options ) */
					/*
					 *
					 * ALTER COLUMN RESET ( options )（重置列选项）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE |
								ATT_MATVIEW | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
						/*
						 *
						 * ALTER COLUMN SET STORAGE（设置列存储方式）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE |
								ATT_MATVIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetCompression: /* ALTER COLUMN SET COMPRESSION */
					/*
					 *
					 * ALTER COLUMN SET COMPRESSION（设置列压缩）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_DropColumn:		/* DROP COLUMN */
						/*
						 *
						 * DROP COLUMN（删列）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE |
								ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepDropColumn(wqueue, rel, recurse, recursing, cmd,
							 lockmode, context);
			/* Recursion occurs during execution phase */
			/*
			 *
			 * 递归发生在执行阶段
			 */
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* ADD INDEX */
						/*
						 *
						 * ADD INDEX（加索引）
						 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_PARTITIONED_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
					/*
					 *
					 * ADD CONSTRAINT（加约束）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATPrepAddPrimaryKey(wqueue, rel, cmd, recurse, lockmode, context);
			if (recurse)
			{
				/* recurses at exec time; lock descendants and set flag */
				/*
				 *
				 * 在执行时递归；锁住后代并设置标志
				 */
				(void) find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);
				cmd->recurse = true;
			}
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
					    /*
					     *
					     * ADD CONSTRAINT USING INDEX（用已有索引加约束）
					     */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_PARTITIONED_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_ADD_INDEXCONSTR;
			break;
		case AT_DropConstraint: /* DROP CONSTRAINT */
					/*
					 *
					 * DROP CONSTRAINT（删约束）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
			/* Other recursion occurs during execution phase */
			/*
			 *
			 * 其他递归发生在执行阶段
			 */
			/* No command-specific prep needed except saving recurse flag */
			/*
			 *
			 * 除了保存 recurse 标志外，不需要针对该命令的专门准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:	/* ALTER COLUMN TYPE */
						/*
						 *
						 * ALTER COLUMN TYPE（修改列类型）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE |
								ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			/* See comments for ATPrepAlterColumnType */
			/*
			 *
			 * 见 ATPrepAlterColumnType 的注释
			 */
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, recurse, lockmode,
									  AT_PASS_UNSET, context);
			Assert(cmd != NULL);
			/* Performs own recursion */
			/*
			 *
			 * 自己负责递归
			 */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd,
								  lockmode, context);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_AlterColumnGenericOptions:
			ATSimplePermissions(cmd->subtype, rel, ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
					/*
					 *
					 * ALTER OWNER（改属主）
					 */
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
						/*
						 *
						 * CLUSTER ON（指定聚簇索引）
						 */
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
					/*
					 *
					 * SET WITHOUT CLUSTER（取消聚簇）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);
			/* These commands never recurse */
			/*
			 *
			 * 这些命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_SetLogged:		/* SET LOGGED */
						/*
						 *
						 * SET LOGGED（改为写日志）
						 */
		case AT_SetUnLogged:	/* SET UNLOGGED */
					/*
					 *
					 * SET UNLOGGED（改为不写日志）
					 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_SEQUENCE);
			if (tab->chgPersistence)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change persistence setting twice")));
			ATPrepChangePersistence(tab, rel, cmd->subtype == AT_SetLogged);
			pass = AT_PASS_MISC;
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
						/*
						 *
						 * SET WITHOUT OIDS（去掉 OID，已废弃）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			pass = AT_PASS_DROP;
			break;
		case AT_SetAccessMethod:	/* SET ACCESS METHOD */
						/*
						 *
						 * SET ACCESS METHOD（设置访问方法）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);

			/* check if another access method change was already requested */
			/*
			 *
			 * 检查是否已经请求过另一次访问方法变更
			 */
			if (tab->chgAccessMethod)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot have multiple SET ACCESS METHOD subcommands")));

			ATPrepSetAccessMethod(tab, rel, cmd->name);
			pass = AT_PASS_MISC;	/* does not matter; no work in Phase 2 */
						/*
						 *
						 * 无所谓；Phase 2 没有工作
						 */
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
					/*
					 *
					 * SET TABLESPACE（设置表空间）
					 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_PARTITIONED_TABLE |
								ATT_MATVIEW | ATT_INDEX | ATT_PARTITIONED_INDEX);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			ATPrepSetTableSpace(tab, rel, cmd->name, lockmode);
			pass = AT_PASS_MISC;	/* doesn't actually matter */
						/*
						 *
						 * 其实无所谓
						 */
			break;
		case AT_SetRelOptions:	/* SET (...) */
					/*
					 *
					 * SET (...)（设置关系选项）
					 */
		case AT_ResetRelOptions:	/* RESET (...) */
						/*
						 *
						 * RESET (...)（重置关系选项）
						 */
		case AT_ReplaceRelOptions:	/* reset them all, then set just these */
						/*
						 *
						 * 先全部重置，再只设置这些
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_VIEW |
								ATT_MATVIEW | ATT_INDEX);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_AddInherit:		/* INHERIT */
						/*
						 *
						 * INHERIT（加入继承）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			ATPrepAddInherit(rel);
			pass = AT_PASS_MISC;
			break;
		case AT_DropInherit:	/* NO INHERIT */
					/*
					 *
					 * NO INHERIT（脱离继承）
					 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_AlterConstraint:	/* ALTER CONSTRAINT */
						/*
						 *
						 * ALTER CONSTRAINT（修改约束）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE);
			/* Recursion occurs during execution phase */
			/*
			 *
			 * 递归发生在执行阶段
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
					    /*
					     *
					     * VALIDATE CONSTRAINT（验证约束）
					     */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Recursion occurs during execution phase */
			/*
			 *
			 * 递归发生在执行阶段
			 */
			/* No command-specific prep needed except saving recurse flag */
			/*
			 *
			 * 除了保存 recurse 标志外，不需要针对该命令的专门准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_ReplicaIdentity:	/* REPLICA IDENTITY ... */
						/*
						 *
						 * REPLICA IDENTITY ...（设置复制标识）
						 */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_MATVIEW);
			pass = AT_PASS_MISC;
			/* This command never recurses */
			/*
			 *
			 * 这条命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER variants */
						/*
						 *
						 * ENABLE TRIGGER 的各种形式
						 */
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* DISABLE TRIGGER variants */
					/*
					 *
					 * DISABLE TRIGGER 的各种形式
					 */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);
			/* Set up recursion for phase 2; no other prep needed */
			/*
			 *
			 * 为阶段 2 准备递归；不需要其他准备
			 */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_EnableRule:		/* ENABLE/DISABLE RULE variants */
						/*
						 *
						 * ENABLE/DISABLE RULE 的各种形式
						 */
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
		case AT_AddOf:			/* OF */
						/*
						 *
						 * OF（设为某类型的类型表）
						 */
		case AT_DropOf:			/* NOT OF */
						/*
						 *
						 * NOT OF（取消类型表）
						 */
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_PARTITIONED_TABLE);
			/* These commands never recurse */
			/*
			 *
			 * 这些命令从不递归
			 */
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_GenericOptions:
			ATSimplePermissions(cmd->subtype, rel, ATT_FOREIGN_TABLE);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_AttachPartition:
			ATSimplePermissions(cmd->subtype, rel,
								ATT_PARTITIONED_TABLE | ATT_PARTITIONED_INDEX);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartition:
			ATSimplePermissions(cmd->subtype, rel, ATT_PARTITIONED_TABLE);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartitionFinalize:
			ATSimplePermissions(cmd->subtype, rel, ATT_PARTITIONED_TABLE);
			/* No command-specific prep needed */
			/*
			 *
			 * 不需要针对该命令的专门准备
			 */
			pass = AT_PASS_MISC;
			break;
		default:				/* oops */
							/*
							 *
							 * 不该走到这里
							 */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = AT_PASS_UNSET;	/* keep compiler quiet */
						/*
						 *
						 * 免得编译器告警
						 */
			break;
	}
	Assert(pass > AT_PASS_UNSET);

	/* Add the subcommand to the appropriate list for phase 2 */
	/*
	 *
	 * 把子命令加到阶段 2 对应的列表里
	 */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * 函数 ATRewriteCatalogs
 *
 * Traffic cop for ALTER TABLE Phase 2 operations.  Subcommands are
 * dispatched in a "safe" execution order (designed to avoid unnecessary
 * conflicts).
 *
 * ALTER TABLE 阶段 2 的交通警察。子命令按一种安全的执行顺序分发（为了避免无谓冲突）。
 */
static void
ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	ListCell   *ltab;

	/*
	 * We process all the tables "in parallel", one pass at a time.  This is
	 * needed because we may have to propagate work from one table to another
	 * (specifically, ALTER TYPE on a foreign key's PK has to dispatch the
	 * re-adding of the foreign key constraint to the other table).  Work can
	 * only be propagated into later passes, however.
	 *
	 * 我们一轮一轮地并行处理所有表。这是必要的，因为工作可能要从一张表传到另一张（具体说，对主键做 ALTER TYPE 时，
	 * 要把重新加上外键约束的工作分发给另一张表）。不过工作只能传播到后面的遍次。
	 */
	for (AlterTablePass pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* Go through each table that needs to be processed */
		/*
		 *
		 * 遍历每张需要处理的表
		 */
		foreach(ltab, *wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			/*
			 * Open the relation and store it in tab.  This allows subroutines
			 * close and reopen, if necessary.  Appropriate lock was obtained
			 * by phase 1, needn't get it again.
			 *
			 * 打开关系并存进 tab。这样子程序可以在需要时先关再开。合适的锁已在阶段 1 拿到，不必再取。
			 */
			tab->rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
				ATExecCmd(wqueue, tab,
						  lfirst_node(AlterTableCmd, lcmd),
						  lockmode, pass, context);

			/*
			 * After the ALTER TYPE or SET EXPRESSION pass, do cleanup work
			 * (this is not done in ATExecAlterColumnType since it should be
			 * done only once if multiple columns of a table are altered).
			 *
			 * ALTER TYPE 或 SET EXPRESSION 那一遍之后做清理（不放在 ATExecAlterColumnType 里，
			 * 因为一张表改了多列时只应做一次）。
			 */
			if (pass == AT_PASS_ALTER_TYPE || pass == AT_PASS_SET_EXPRESSION)
				ATPostAlterTypeCleanup(wqueue, tab, lockmode);

			if (tab->rel)
			{
				relation_close(tab->rel, NoLock);
				tab->rel = NULL;
			}
		}
	}

	/* Check to see if a toast table must be added. */
	/*
	 *
	 * 检查是否必须添加 TOAST 表。
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/*
		 * If the table is source table of ATTACH PARTITION command, we did
		 * not modify anything about it that will change its toasting
		 * requirement, so no need to check.
		 *
		 * 若该表是 ATTACH PARTITION 命令的源表，我们没有改任何会影响其 TOAST 需求的东西，所以不必检查。
		 */
		if (((tab->relkind == RELKIND_RELATION ||
			  tab->relkind == RELKIND_PARTITIONED_TABLE) &&
			 tab->partition_constraint == NULL) ||
			tab->relkind == RELKIND_MATVIEW)
			AlterTableCreateToastTable(tab->relid, (Datum) 0, lockmode);
	}
}

/*
 * ATExecCmd: dispatch a subcommand to appropriate execution routine
 *
 * ATExecCmd：把子命令分发给对应的执行例程
 */
static void
ATExecCmd(List **wqueue, AlteredTableInfo *tab,
		  AlterTableCmd *cmd, LOCKMODE lockmode, AlterTablePass cur_pass,
		  AlterTableUtilityContext *context)
{
	ObjectAddress address = InvalidObjectAddress;
	Relation	rel = tab->rel;

	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* ADD COLUMN */
						/*
						 *
						 * ADD COLUMN（加列）
						 */
		case AT_AddColumnToView:	/* add column via CREATE OR REPLACE VIEW */
						/*
						 *
						 * 通过 CREATE OR REPLACE VIEW 加列
						 */
			address = ATExecAddColumn(wqueue, tab, rel, &cmd,
									  cmd->recurse, false,
									  lockmode, cur_pass, context);
			break;
		case AT_ColumnDefault:	/* ALTER COLUMN DEFAULT */
					/*
					 *
					 * ALTER COLUMN DEFAULT（修改列默认值）
					 */
			address = ATExecColumnDefault(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_CookedColumnDefault:	/* add a pre-cooked default */
						/*
						 *
						 * 添加一个已经煮好的默认值
						 */
			address = ATExecCookedColumnDefault(rel, cmd->num, cmd->def);
			break;
		case AT_AddIdentity:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			address = ATExecAddIdentity(rel, cmd->name, cmd->def, lockmode, cmd->recurse, false);
			break;
		case AT_SetIdentity:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			address = ATExecSetIdentity(rel, cmd->name, cmd->def, lockmode, cmd->recurse, false);
			break;
		case AT_DropIdentity:
			address = ATExecDropIdentity(rel, cmd->name, cmd->missing_ok, lockmode, cmd->recurse, false);
			break;
		case AT_DropNotNull:	/* ALTER COLUMN DROP NOT NULL */
					/*
					 *
					 * ALTER COLUMN DROP NOT NULL（去掉非空）
					 */
			address = ATExecDropNotNull(rel, cmd->name, cmd->recurse, lockmode);
			break;
		case AT_SetNotNull:		/* ALTER COLUMN SET NOT NULL */
						/*
						 *
						 * ALTER COLUMN SET NOT NULL（设为非空）
						 */
			address = ATExecSetNotNull(wqueue, rel, NULL, cmd->name,
									   cmd->recurse, false, lockmode);
			break;
		case AT_SetExpression:
			address = ATExecSetExpression(tab, rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_DropExpression:
			address = ATExecDropExpression(rel, cmd->name, cmd->missing_ok, lockmode);
			break;
		case AT_SetStatistics:	/* ALTER COLUMN SET STATISTICS */
					/*
					 *
					 * ALTER COLUMN SET STATISTICS（设置统计目标）
					 */
			address = ATExecSetStatistics(rel, cmd->name, cmd->num, cmd->def, lockmode);
			break;
		case AT_SetOptions:		/* ALTER COLUMN SET ( options ) */
						/*
						 *
						 * ALTER COLUMN SET ( options )（设置列选项）
						 */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, false, lockmode);
			break;
		case AT_ResetOptions:	/* ALTER COLUMN RESET ( options ) */
					/*
					 *
					 * ALTER COLUMN RESET ( options )（重置列选项）
					 */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, true, lockmode);
			break;
		case AT_SetStorage:		/* ALTER COLUMN SET STORAGE */
						/*
						 *
						 * ALTER COLUMN SET STORAGE（设置列存储方式）
						 */
			address = ATExecSetStorage(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetCompression: /* ALTER COLUMN SET COMPRESSION */
					/*
					 *
					 * ALTER COLUMN SET COMPRESSION（设置列压缩）
					 */
			address = ATExecSetCompression(rel, cmd->name, cmd->def,
										   lockmode);
			break;
		case AT_DropColumn:		/* DROP COLUMN */
						/*
						 *
						 * DROP COLUMN（删列）
						 */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, cmd->recurse, false,
									   cmd->missing_ok, lockmode,
									   NULL);
			break;
		case AT_AddIndex:		/* ADD INDEX */
						/*
						 *
						 * ADD INDEX（加索引）
						 */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false,
									 lockmode);
			break;
		case AT_ReAddIndex:		/* ADD INDEX */
						/*
						 *
						 * ADD INDEX（加索引）
						 */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true,
									 lockmode);
			break;
		case AT_ReAddStatistics:	/* ADD STATISTICS */
						/*
						 *
						 * ADD STATISTICS（加扩展统计）
						 */
			address = ATExecAddStatistics(tab, rel, (CreateStatsStmt *) cmd->def,
										  true, lockmode);
			break;
		case AT_AddConstraint:	/* ADD CONSTRAINT */
					/*
					 *
					 * ADD CONSTRAINT（加约束）
					 */
			/* Transform the command only during initial examination */
			/*
			 *
			 * 只在初步检查时变换该命令
			 */
			if (cur_pass == AT_PASS_ADD_CONSTR)
				cmd = ATParseTransformCmd(wqueue, tab, rel, cmd,
										  cmd->recurse, lockmode,
										  cur_pass, context);
			/* Depending on constraint type, might be no more work to do now */
			/*
			 *
			 * 视约束类型而定，现在可能没有更多工作
			 */
			if (cmd != NULL)
				address =
					ATExecAddConstraint(wqueue, tab, rel,
										(Constraint *) cmd->def,
										cmd->recurse, false, lockmode);
			break;
		case AT_ReAddConstraint:	/* Re-add pre-existing check constraint */
						/*
						 *
						 * 重新加上已有的 CHECK 约束
						 */
			address =
				ATExecAddConstraint(wqueue, tab, rel, (Constraint *) cmd->def,
									true, true, lockmode);
			break;
		case AT_ReAddDomainConstraint:	/* Re-add pre-existing domain check
										 * constraint */
						/*
						 *
						 * 重新加上已有的域 CHECK 约束
						 */
			address =
				AlterDomainAddConstraint(((AlterDomainStmt *) cmd->def)->typeName,
										 ((AlterDomainStmt *) cmd->def)->def,
										 NULL);
			break;
		case AT_ReAddComment:	/* Re-add existing comment */
					/*
					 *
					 * 重新加上已有注释
					 */
			address = CommentObject((CommentStmt *) cmd->def);
			break;
		case AT_AddIndexConstraint: /* ADD CONSTRAINT USING INDEX */
					    /*
					     *
					     * ADD CONSTRAINT USING INDEX（用已有索引加约束）
					     */
			address = ATExecAddIndexConstraint(tab, rel, (IndexStmt *) cmd->def,
											   lockmode);
			break;
		case AT_AlterConstraint:	/* ALTER CONSTRAINT */
						/*
						 *
						 * ALTER CONSTRAINT（修改约束）
						 */
			address = ATExecAlterConstraint(wqueue, rel,
											castNode(ATAlterConstraint, cmd->def),
											cmd->recurse, lockmode);
			break;
		case AT_ValidateConstraint: /* VALIDATE CONSTRAINT */
					    /*
					     *
					     * VALIDATE CONSTRAINT（验证约束）
					     */
			address = ATExecValidateConstraint(wqueue, rel, cmd->name, cmd->recurse,
											   false, lockmode);
			break;
		case AT_DropConstraint: /* DROP CONSTRAINT */
					/*
					 *
					 * DROP CONSTRAINT（删约束）
					 */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 cmd->recurse,
								 cmd->missing_ok, lockmode);
			break;
		case AT_AlterColumnType:	/* ALTER COLUMN TYPE */
						/*
						 *
						 * ALTER COLUMN TYPE（修改列类型）
						 */
			/* parse transformation was done earlier */
			/*
			 *
			 * 解析变换先前已经做过
			 */
			address = ATExecAlterColumnType(tab, rel, cmd, lockmode);
			break;
		case AT_AlterColumnGenericOptions:	/* ALTER COLUMN OPTIONS */
							/*
							 *
							 * ALTER COLUMN OPTIONS（修改列选项）
							 */
			address =
				ATExecAlterColumnGenericOptions(rel, cmd->name,
												(List *) cmd->def, lockmode);
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
					/*
					 *
					 * ALTER OWNER（改属主）
					 */
			ATExecChangeOwner(RelationGetRelid(rel),
							  get_rolespec_oid(cmd->newowner, false),
							  false, lockmode);
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
						/*
						 *
						 * CLUSTER ON（指定聚簇索引）
						 */
			address = ATExecClusterOn(rel, cmd->name, lockmode);
			break;
		case AT_DropCluster:	/* SET WITHOUT CLUSTER */
					/*
					 *
					 * SET WITHOUT CLUSTER（取消聚簇）
					 */
			ATExecDropCluster(rel, lockmode);
			break;
		case AT_SetLogged:		/* SET LOGGED */
						/*
						 *
						 * SET LOGGED（改为写日志）
						 */
		case AT_SetUnLogged:	/* SET UNLOGGED */
					/*
					 *
					 * SET UNLOGGED（改为不写日志）
					 */
			break;
		case AT_DropOids:		/* SET WITHOUT OIDS */
						/*
						 *
						 * SET WITHOUT OIDS（去掉 OID，已废弃）
						 */
			/* nothing to do here, oid columns don't exist anymore */
			/*
			 *
			 * 这里无事可做，OID 列已经不存在了
			 */
			break;
		case AT_SetAccessMethod:	/* SET ACCESS METHOD */
						/*
						 *
						 * SET ACCESS METHOD（设置访问方法）
						 */

			/*
			 * Only do this for partitioned tables, for which this is just a
			 * catalog change.  Tables with storage are handled by Phase 3.
			 *
			 * 只对分区表做这件事，对它们来说只是目录变更。有存储的表由 Phase 3 处理。
			 */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
				tab->chgAccessMethod)
				ATExecSetAccessMethodNoStorage(rel, tab->newAccessMethod);
			break;
		case AT_SetTableSpace:	/* SET TABLESPACE */
					/*
					 *
					 * SET TABLESPACE（设置表空间）
					 */

			/*
			 * Only do this for partitioned tables and indexes, for which this
			 * is just a catalog change.  Other relation types which have
			 * storage are handled by Phase 3.
			 *
			 * 只对分区表和分区索引做这件事，对它们来说只是目录变更。其他有存储的关系类型由 Phase 3 处理。
			 */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
				rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
				ATExecSetTableSpaceNoStorage(rel, tab->newTableSpace);

			break;
		case AT_SetRelOptions:	/* SET (...) */
					/*
					 *
					 * SET (...)（设置关系选项）
					 */
		case AT_ResetRelOptions:	/* RESET (...) */
						/*
						 *
						 * RESET (...)（重置关系选项）
						 */
		case AT_ReplaceRelOptions:	/* replace entire option list */
						/*
						 *
						 * 替换整个选项列表
						 */
			ATExecSetRelOptions(rel, (List *) cmd->def, cmd->subtype, lockmode);
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER name */
						/*
						 *
						 * ENABLE TRIGGER name（启用指定触发器）
						 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableAlwaysTrig:	/* ENABLE ALWAYS TRIGGER name */
						/*
						 *
						 * ENABLE ALWAYS TRIGGER name（始终启用指定触发器）
						 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ALWAYS, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableReplicaTrig:	/* ENABLE REPLICA TRIGGER name */
						/*
						 *
						 * ENABLE REPLICA TRIGGER name（在副本上启用指定触发器）
						 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_REPLICA, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
					/*
					 *
					 * DISABLE TRIGGER name（禁用指定触发器）
					 */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_DISABLED, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableTrigAll:	/* ENABLE TRIGGER ALL */
					/*
					 *
					 * ENABLE TRIGGER ALL（启用全部触发器）
					 */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrigAll: /* DISABLE TRIGGER ALL */
					/*
					 *
					 * DISABLE TRIGGER ALL（禁用全部触发器）
					 */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableTrigUser: /* ENABLE TRIGGER USER */
					/*
					 *
					 * ENABLE TRIGGER USER（启用用户触发器）
					 */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_FIRES_ON_ORIGIN, true,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrigUser:	/* DISABLE TRIGGER USER */
						/*
						 *
						 * DISABLE TRIGGER USER（禁用用户触发器）
						 */
			ATExecEnableDisableTrigger(rel, NULL,
									   TRIGGER_DISABLED, true,
									   cmd->recurse,
									   lockmode);
			break;

		case AT_EnableRule:		/* ENABLE RULE name */
						/*
						 *
						 * ENABLE RULE name（启用指定规则）
						 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_ORIGIN, lockmode);
			break;
		case AT_EnableAlwaysRule:	/* ENABLE ALWAYS RULE name */
						/*
						 *
						 * ENABLE ALWAYS RULE name（始终启用指定规则）
						 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ALWAYS, lockmode);
			break;
		case AT_EnableReplicaRule:	/* ENABLE REPLICA RULE name */
						/*
						 *
						 * ENABLE REPLICA RULE name（在副本上启用指定规则）
						 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_REPLICA, lockmode);
			break;
		case AT_DisableRule:	/* DISABLE RULE name */
					/*
					 *
					 * DISABLE RULE name（禁用指定规则）
					 */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_DISABLED, lockmode);
			break;

		case AT_AddInherit:
			address = ATExecAddInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_DropInherit:
			address = ATExecDropInherit(rel, (RangeVar *) cmd->def, lockmode);
			break;
		case AT_AddOf:
			address = ATExecAddOf(rel, (TypeName *) cmd->def, lockmode);
			break;
		case AT_DropOf:
			ATExecDropOf(rel, lockmode);
			break;
		case AT_ReplicaIdentity:
			ATExecReplicaIdentity(rel, (ReplicaIdentityStmt *) cmd->def, lockmode);
			break;
		case AT_EnableRowSecurity:
			ATExecSetRowSecurity(rel, true);
			break;
		case AT_DisableRowSecurity:
			ATExecSetRowSecurity(rel, false);
			break;
		case AT_ForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, true);
			break;
		case AT_NoForceRowSecurity:
			ATExecForceNoForceRowSecurity(rel, false);
			break;
		case AT_GenericOptions:
			ATExecGenericOptions(rel, (List *) cmd->def);
			break;
		case AT_AttachPartition:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				address = ATExecAttachPartition(wqueue, rel, (PartitionCmd *) cmd->def,
												context);
			else
				address = ATExecAttachPartitionIdx(wqueue, rel,
												   ((PartitionCmd *) cmd->def)->name);
			break;
		case AT_DetachPartition:
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, false, lockmode,
									  cur_pass, context);
			Assert(cmd != NULL);
			/* ATPrepCmd ensures it must be a table */
			/*
			 *
			 * ATPrepCmd 保证它必须是表
			 */
			Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			address = ATExecDetachPartition(wqueue, tab, rel,
											((PartitionCmd *) cmd->def)->name,
											((PartitionCmd *) cmd->def)->concurrent);
			break;
		case AT_DetachPartitionFinalize:
			address = ATExecDetachPartitionFinalize(rel, ((PartitionCmd *) cmd->def)->name);
			break;
		default:				/* oops */
							/*
							 *
							 * 不该走到这里
							 */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * Report the subcommand to interested event triggers.
	 *
	 * 把这个子命令报告给关心它的事件触发器。
	 */
	if (cmd)
		EventTriggerCollectAlterTableSubcmd((Node *) cmd, address);

	/*
	 * Bump the command counter to ensure the next subcommand in the sequence
	 * can see the changes so far
	 *
	 * 推进命令计数器，确保序列中的下一条子命令能看见目前为止的修改
	 */
	CommandCounterIncrement();
}

/*
 * ATParseTransformCmd: perform parse transformation for one subcommand
 *
 * ATParseTransformCmd：对一条子命令做解析变换
 *
 * Returns the transformed subcommand tree, if there is one, else NULL.
 *
 * 若有变换后的子命令树则返回它，否则返回 NULL。
 *
 * The parser may hand back additional AlterTableCmd(s) and/or other
 * utility statements, either before or after the original subcommand.
 * Other AlterTableCmds are scheduled into the appropriate slot of the
 * AlteredTableInfo (they had better be for later passes than the current one).
 * Utility statements that are supposed to happen before the AlterTableCmd
 * are executed immediately.  Those that are supposed to happen afterwards
 * are added to the tab->afterStmts list to be done at the very end.
 *
 * 解析器可能在原子命令之前或之后交回额外的 AlterTableCmd 和/或其他工具语句。其他 AlterTableCmd 被排进
 * AlteredTableInfo 的相应槽位（它们最好属于比当前更晚的遍次）。应发生在 AlterTableCmd
 * 之前的工具语句立刻执行。应发生在之后的则加入 tab->afterStmts，留到最后做。
 */
static AlterTableCmd *
ATParseTransformCmd(List **wqueue, AlteredTableInfo *tab, Relation rel,
					AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
					AlterTablePass cur_pass, AlterTableUtilityContext *context)
{
	AlterTableCmd *newcmd = NULL;
	AlterTableStmt *atstmt = makeNode(AlterTableStmt);
	List	   *beforeStmts;
	List	   *afterStmts;
	ListCell   *lc;

	/* Gin up an AlterTableStmt with just this subcommand and this table */
	/*
	 *
	 * 临时造一个只含这条子命令和这张表的 AlterTableStmt
	 */
	atstmt->relation =
		makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
					 pstrdup(RelationGetRelationName(rel)),
					 -1);
	atstmt->relation->inh = recurse;
	atstmt->cmds = list_make1(cmd);
	atstmt->objtype = OBJECT_TABLE; /* needn't be picky here */
					/*
					 *
					 * 这里不必挑剔
					 */
	atstmt->missing_ok = false;

	/* Transform the AlterTableStmt */
	/*
	 *
	 * 变换这个 AlterTableStmt
	 */
	atstmt = transformAlterTableStmt(RelationGetRelid(rel),
									 atstmt,
									 context->queryString,
									 &beforeStmts,
									 &afterStmts);

	/* Execute any statements that should happen before these subcommand(s) */
	/*
	 *
	 * 执行应发生在这些子命令之前的语句
	 */
	foreach(lc, beforeStmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		ProcessUtilityForAlterTable(stmt, context);
		CommandCounterIncrement();
	}

	/* Examine the transformed subcommands and schedule them appropriately */
	/*
	 *
	 * 检查变换后的子命令并妥善排期
	 */
	foreach(lc, atstmt->cmds)
	{
		AlterTableCmd *cmd2 = lfirst_node(AlterTableCmd, lc);
		AlterTablePass pass;

		/*
		 * This switch need only cover the subcommand types that can be added
		 * by parse_utilcmd.c; otherwise, we'll use the default strategy of
		 * executing the subcommand immediately, as a substitute for the
		 * original subcommand.  (Note, however, that this does cause
		 * AT_AddConstraint subcommands to be rescheduled into later passes,
		 * which is important for index and foreign key constraints.)
		 *
		 * 这个 switch 只需覆盖 parse_utilcmd.c 可能添加的子命令类型；其余情况用默认策略，立刻执行该子命令，
		 * 作为原子命令的替代。（不过请注意，这会使 AT_AddConstraint 子命令被重新排到后面的遍次，对索引和外键约束这很重要。）
		 *
		 * We assume we needn't do any phase-1 checks for added subcommands.
		 *
		 * 假定对新增的子命令不必再做阶段 1 检查。
		 */
		switch (cmd2->subtype)
		{
			case AT_AddIndex:
				pass = AT_PASS_ADD_INDEX;
				break;
			case AT_AddIndexConstraint:
				pass = AT_PASS_ADD_INDEXCONSTR;
				break;
			case AT_AddConstraint:
				/* Recursion occurs during execution phase */
				/*
				 *
				 * 递归发生在执行阶段
				 */
				if (recurse)
					cmd2->recurse = true;
				switch (castNode(Constraint, cmd2->def)->contype)
				{
					case CONSTR_NOTNULL:
						pass = AT_PASS_COL_ATTRS;
						break;
					case CONSTR_PRIMARY:
					case CONSTR_UNIQUE:
					case CONSTR_EXCLUSION:
						pass = AT_PASS_ADD_INDEXCONSTR;
						break;
					default:
						pass = AT_PASS_ADD_OTHERCONSTR;
						break;
				}
				break;
			case AT_AlterColumnGenericOptions:
				/* This command never recurses */
				/*
				 *
				 * 这条命令从不递归
				 */
				/* No command-specific prep needed */
				/*
				 *
				 * 不需要针对该命令的专门准备
				 */
				pass = AT_PASS_MISC;
				break;
			default:
				pass = cur_pass;
				break;
		}

		if (pass < cur_pass)
		{
			/* Cannot schedule into a pass we already finished */
			/*
			 *
			 * 不能排进已经做完的遍次
			 */
			elog(ERROR, "ALTER TABLE scheduling failure: too late for pass %d",
				 pass);
		}
		else if (pass > cur_pass)
		{
			/* OK, queue it up for later */
			/*
			 *
			 * 好，把它排到后面
			 */
			tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd2);
		}
		else
		{
			/*
			 * We should see at most one subcommand for the current pass,
			 * which is the transformed version of the original subcommand.
			 *
			 * 当前遍次最多应看到一条子命令，即原子命令的变换结果。
			 */
			if (newcmd == NULL && cmd->subtype == cmd2->subtype)
			{
				/* Found the transformed version of our subcommand */
				/*
				 *
				 * 找到了我们这条子命令的变换版本
				 */
				newcmd = cmd2;
			}
			else
				elog(ERROR, "ALTER TABLE scheduling failure: bogus item for pass %d",
					 pass);
		}
	}

	/* Queue up any after-statements to happen at the end */
	/*
	 *
	 * 把所有事后语句排到最后执行
	 */
	tab->afterStmts = list_concat(tab->afterStmts, afterStmts);

	return newcmd;
}

/*
 * ATRewriteTables: ALTER TABLE phase 3
 *
 * ATRewriteTables：ALTER TABLE 阶段 3
 */
static void
ATRewriteTables(AlterTableStmt *parsetree, List **wqueue, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	ListCell   *ltab;

	/* Go through each table that needs to be checked or rewritten */
	/*
	 *
	 * 遍历每张需要检查或重写的表
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/* Relations without storage may be ignored here */
		/*
		 *
		 * 没有存储的关系在这里可以忽略
		 */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		/*
		 * If we change column data types, the operation has to be propagated
		 * to tables that use this table's rowtype as a column type.
		 * tab->newvals will also be non-NULL in the case where we're adding a
		 * column with a default.  We choose to forbid that case as well,
		 * since composite types might eventually support defaults.
		 *
		 * 若改变列的数据类型，这个操作必须传播到把本表行类型用作列类型的那些表。加带默认值的列时 tab->newvals 也会非空。
		 * 这种情况我们也选择禁止，因为复合类型将来也许会支持默认值。
		 *
		 * (Eventually we'll probably need to check for composite type
		 * dependencies even when we're just scanning the table without a
		 * rewrite, but at the moment a composite type does not enforce any
		 * constraints, so it's not necessary/appropriate to enforce them just
		 * during ALTER.)
		 *
		 * （将来即使只是扫描表而不重写，可能也要检查复合类型依赖，但目前复合类型不强制任何约束，所以只在 ALTER
		 * 期间强制它们既无必要也不合适。）
		 */
		if (tab->newvals != NIL || tab->rewrite > 0)
		{
			Relation	rel;

			rel = table_open(tab->relid, NoLock);
			find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
			table_close(rel, NoLock);
		}

		/*
		 * We only need to rewrite the table if at least one column needs to
		 * be recomputed, or we are changing its persistence or access method.
		 *
		 * 只有至少有一列需要重算，或者正在改变持久性或访问方法时，才需要重写表。
		 *
		 * There are two reasons for requiring a rewrite when changing
		 * persistence: on one hand, we need to ensure that the buffers
		 * belonging to each of the two relations are marked with or without
		 * BM_PERMANENT properly.  On the other hand, since rewriting creates
		 * and assigns a new relfilenumber, we automatically create or drop an
		 * init fork for the relation as appropriate.
		 *
		 * 改变持久性时要求重写有两个原因：一方面要确保两张关系各自的缓冲区正确地带上或不带 BM_PERMANENT。另一方面，
		 * 重写会创建并分配新的 relfilenumber，从而按需要自动创建或删除该关系的 init fork。
		 */
		if (tab->rewrite > 0 && tab->relkind != RELKIND_SEQUENCE)
		{
			/* Build a temporary relation and copy data */
			/*
			 *
			 * 建一张临时关系并拷贝数据
			 */
			Relation	OldHeap;
			Oid			OIDNewHeap;
			Oid			NewAccessMethod;
			Oid			NewTableSpace;
			char		persistence;

			OldHeap = table_open(tab->relid, NoLock);

			/*
			 * We don't support rewriting of system catalogs; there are too
			 * many corner cases and too little benefit.  In particular this
			 * is certainly not going to work for mapped catalogs.
			 *
			 * 不支持重写系统目录；边角情况太多，好处太少。尤其对映射目录这肯定行不通。
			 */
			if (IsSystemRelation(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite system relation \"%s\"",
								RelationGetRelationName(OldHeap))));

			if (RelationIsUsedAsCatalogTable(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite table \"%s\" used as a catalog table",
								RelationGetRelationName(OldHeap))));

			/*
			 * Don't allow rewrite on temp tables of other backends ... their
			 * local buffer manager is not going to cope.  (This is redundant
			 * with the check in CheckAlterTableIsSafe, but for safety we'll
			 * check here too.)
			 *
			 * 不允许重写其他后端的临时表，它们的本地缓冲管理器应付不了。（这和 CheckAlterTableIsSafe 里的检查重复，
			 * 但为了安全这里再查一次。）
			 */
			if (RELATION_IS_OTHER_TEMP(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite temporary tables of other sessions")));

			/*
			 * Select destination tablespace (same as original unless user
			 * requested a change)
			 *
			 * 选择目标表空间（除非用户要求改，否则与原来相同）
			 */
			if (tab->newTableSpace)
				NewTableSpace = tab->newTableSpace;
			else
				NewTableSpace = OldHeap->rd_rel->reltablespace;

			/*
			 * Select destination access method (same as original unless user
			 * requested a change)
			 *
			 * 选择目标访问方法（除非用户要求改，否则与原来相同）
			 */
			if (tab->chgAccessMethod)
				NewAccessMethod = tab->newAccessMethod;
			else
				NewAccessMethod = OldHeap->rd_rel->relam;

			/*
			 * Select persistence of transient table (same as original unless
			 * user requested a change)
			 *
			 * 选择临时表的持久性（除非用户要求改，否则与原来相同）
			 */
			persistence = tab->chgPersistence ?
				tab->newrelpersistence : OldHeap->rd_rel->relpersistence;

			table_close(OldHeap, NoLock);

			/*
			 * Fire off an Event Trigger now, before actually rewriting the
			 * table.
			 *
			 * 在真正重写表之前，先触发一次事件触发器。
			 *
			 * We don't support Event Trigger for nested commands anywhere,
			 * here included, and parsetree is given NULL when coming from
			 * AlterTableInternal.
			 *
			 * 我们在任何地方都不支持嵌套命令的事件触发器，这里也一样；从 AlterTableInternal 进来时 parsetree 为
			 * NULL。
			 *
			 * And fire it only once.
			 *
			 * 而且只触发一次。
			 */
			if (parsetree)
				EventTriggerTableRewrite((Node *) parsetree,
										 tab->relid,
										 tab->rewrite);

			/*
			 * Create transient table that will receive the modified data.
			 *
			 * 创建将接收修改后数据的临时表。
			 *
			 * Ensure it is marked correctly as logged or unlogged.  We have
			 * to do this here so that buffers for the new relfilenumber will
			 * have the right persistence set, and at the same time ensure
			 * that the original filenumbers's buffers will get read in with
			 * the correct setting (i.e. the original one).  Otherwise a
			 * rollback after the rewrite would possibly result with buffers
			 * for the original filenumbers having the wrong persistence
			 * setting.
			 *
			 * 确保它被正确标成 logged 或 unlogged。必须在这里做，这样新 relfilenumber 的缓冲区会有正确的持久性，
			 * 同时原来文件号的缓冲区也会按正确设置（即原来的设置）读入。否则重写之后若回滚，原来文件号的缓冲区可能会带着错误的持久性设置。
			 *
			 * NB: This relies on swap_relation_files() also swapping the
			 * persistence. That wouldn't work for pg_class, but that can't be
			 * unlogged anyway.
			 *
			 * 注意：这依赖于 swap_relation_files() 也会交换持久性。对 pg_class 那行不通，不过它反正不能是
			 * unlogged。
			 */
			OIDNewHeap = make_new_heap(tab->relid, NewTableSpace, NewAccessMethod,
									   persistence, lockmode);

			/*
			 * Copy the heap data into the new table with the desired
			 * modifications, and test the current data within the table
			 * against new constraints generated by ALTER TABLE commands.
			 *
			 * 把堆数据按所需修改拷进新表，并用 ALTER TABLE 命令产生的新约束检验表中现有数据。
			 */
			ATRewriteTable(tab, OIDNewHeap);

			/*
			 * Swap the physical files of the old and new heaps, then rebuild
			 * indexes and discard the old heap.  We can use RecentXmin for
			 * the table's new relfrozenxid because we rewrote all the tuples
			 * in ATRewriteTable, so no older Xid remains in the table.  Also,
			 * we never try to swap toast tables by content, since we have no
			 * interest in letting this code work on system catalogs.
			 *
			 * 交换新旧堆的物理文件，然后重建索引并丢掉旧堆。表的新 relfrozenxid 可以用 RecentXmin，因为
			 * ATRewriteTable 重写了所有元组，表里不会留下更老的 Xid。另外，我们从不按内容交换 TOAST 表，
			 * 因为没兴趣让这段代码在系统目录上工作。
			 */
			finish_heap_swap(tab->relid, OIDNewHeap,
							 false, false, true,
							 !OidIsValid(tab->newTableSpace),
							 RecentXmin,
							 ReadNextMultiXactId(),
							 persistence);

			InvokeObjectPostAlterHook(RelationRelationId, tab->relid, 0);
		}
		else if (tab->rewrite > 0 && tab->relkind == RELKIND_SEQUENCE)
		{
			if (tab->chgPersistence)
				SequenceChangePersistence(tab->relid, tab->newrelpersistence);
		}
		else
		{
			/*
			 * If required, test the current data within the table against new
			 * constraints generated by ALTER TABLE commands, but don't
			 * rebuild data.
			 *
			 * 若需要，用 ALTER TABLE 命令产生的新约束检验表中现有数据，但不重建数据。
			 */
			if (tab->constraints != NIL || tab->verify_new_notnull ||
				tab->partition_constraint != NULL)
				ATRewriteTable(tab, InvalidOid);

			/*
			 * If we had SET TABLESPACE but no reason to reconstruct tuples,
			 * just do a block-by-block copy.
			 *
			 * 若做了 SET TABLESPACE 但没有理由重建元组，就按块拷贝。
			 */
			if (tab->newTableSpace)
				ATExecSetTableSpace(tab->relid, tab->newTableSpace, lockmode);
		}

		/*
		 * Also change persistence of owned sequences, so that it matches the
		 * table persistence.
		 *
		 * 同时改变所属序列的持久性，使它与表的持久性一致。
		 */
		if (tab->chgPersistence)
		{
			List	   *seqlist = getOwnedSequences(tab->relid);
			ListCell   *lc;

			foreach(lc, seqlist)
			{
				Oid			seq_relid = lfirst_oid(lc);

				SequenceChangePersistence(seq_relid, tab->newrelpersistence);
			}
		}
	}

	/*
	 * Foreign key constraints are checked in a final pass, since (a) it's
	 * generally best to examine each one separately, and (b) it's at least
	 * theoretically possible that we have changed both relations of the
	 * foreign key, and we'd better have finished both rewrites before we try
	 * to read the tables.
	 *
	 * 外键约束在最后一遍检查，因为 (a) 一般最好逐条检查；(b) 至少理论上可能外键的两边关系都改了，读表之前最好两边都重写完。
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation	rel = NULL;
		ListCell   *lcon;

		/* Relations without storage may be ignored here too */
		/*
		 *
		 * 没有存储的关系在这里同样可以忽略
		 */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		foreach(lcon, tab->constraints)
		{
			NewConstraint *con = lfirst(lcon);

			if (con->contype == CONSTR_FOREIGN)
			{
				Constraint *fkconstraint = (Constraint *) con->qual;
				Relation	refrel;

				if (rel == NULL)
				{
					/* Long since locked, no need for another */
					/*
					 *
					 * 早就锁过了，不必再锁
					 */
					rel = table_open(tab->relid, NoLock);
				}

				refrel = table_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint->conname, rel, refrel,
											 con->refindid,
											 con->conid,
											 con->conwithperiod);

				/*
				 * No need to mark the constraint row as validated, we did
				 * that when we inserted the row earlier.
				 *
				 * 不必再把约束行标成已验证，插入该行时已经标过。
				 */

				table_close(refrel, NoLock);
			}
		}

		if (rel)
			table_close(rel, NoLock);
	}

	/* Finally, run any afterStmts that were queued up */
	/*
	 *
	 * 最后，运行所有排好队的 afterStmts
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		ListCell   *lc;

		foreach(lc, tab->afterStmts)
		{
			Node	   *stmt = (Node *) lfirst(lc);

			ProcessUtilityForAlterTable(stmt, context);
			CommandCounterIncrement();
		}
	}
}

/*
 * ATRewriteTable: scan or rewrite one table
 *
 * ATRewriteTable：扫描或重写一张表
 *
 * A rewrite is requested by passing a valid OIDNewHeap; in that case, caller
 * must already hold AccessExclusiveLock on it.
 *
 * 若传入有效的 OIDNewHeap 就表示要求重写；此时调用方必须已经对它持有 AccessExclusiveLock。
 */
static void
ATRewriteTable(AlteredTableInfo *tab, Oid OIDNewHeap)
{
	Relation	oldrel;
	Relation	newrel;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	bool		needscan = false;
	List	   *notnull_attrs;
	List	   *notnull_virtual_attrs;
	int			i;
	ListCell   *l;
	EState	   *estate;
	CommandId	mycid;
	BulkInsertState bistate;
	int			ti_options;
	ExprState  *partqualstate = NULL;

	/*
	 * Open the relation(s).  We have surely already locked the existing
	 * table.
	 *
	 * 打开关系。现有表肯定已经锁过。
	 */
	oldrel = table_open(tab->relid, NoLock);
	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);	/* includes all mods */
						/*
						 *
						 * 包含所有修改
						 */

	if (OidIsValid(OIDNewHeap))
	{
		Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock,
										  false));
		newrel = table_open(OIDNewHeap, NoLock);
	}
	else
		newrel = NULL;

	/*
	 * Prepare a BulkInsertState and options for table_tuple_insert.  The FSM
	 * is empty, so don't bother using it.
	 *
	 * 为 table_tuple_insert 准备 BulkInsertState 和选项。FSM 是空的，不必用它。
	 */
	if (newrel)
	{
		mycid = GetCurrentCommandId(true);
		bistate = GetBulkInsertState();
		ti_options = TABLE_INSERT_SKIP_FSM;
	}
	else
	{
		/* keep compiler quiet about using these uninitialized */
		/*
		 *
		 * 免得编译器抱怨这些变量未初始化就被使用
		 */
		mycid = 0;
		bistate = NULL;
		ti_options = 0;
	}

	/*
	 * Generate the constraint and default execution states
	 *
	 * 生成约束和默认值的执行状态
	 */

	estate = CreateExecutorState();

	/* Build the needed expression execution states */
	/*
	 *
	 * 构造所需表达式的执行状态
	 */
	foreach(l, tab->constraints)
	{
		NewConstraint *con = lfirst(l);

		switch (con->contype)
		{
			case CONSTR_CHECK:
				needscan = true;
				con->qualstate = ExecPrepareExpr((Expr *) expand_generated_columns_in_expr(con->qual, oldrel, 1), estate);
				break;
			case CONSTR_FOREIGN:
				/* Nothing to do here */
				/*
				 *
				 * 这里无事可做
				 */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	/* Build expression execution states for partition check quals */
	/*
	 *
	 * 为分区检查条件构造表达式执行状态
	 */
	if (tab->partition_constraint)
	{
		needscan = true;
		partqualstate = ExecPrepareExpr(tab->partition_constraint, estate);
	}

	foreach(l, tab->newvals)
	{
		NewColumnValue *ex = lfirst(l);

		/* expr already planned */
		/*
		 *
		 * 表达式已经规划过
		 */
		ex->exprstate = ExecInitExpr((Expr *) ex->expr, NULL);
	}

	notnull_attrs = notnull_virtual_attrs = NIL;
	if (newrel || tab->verify_new_notnull)
	{
		/*
		 * If we are rebuilding the tuples OR if we added any new but not
		 * verified not-null constraints, check all *valid* not-null
		 * constraints. This is a bit of overkill but it minimizes risk of
		 * bugs.
		 *
		 * 若正在重建元组，或者加了任何新的但尚未验证的 NOT NULL 约束，就检查所有有效的 NOT NULL 约束。有点过头，但能把出
		 * bug 的风险降到最低。
		 *
		 * notnull_attrs does *not* collect attribute numbers for valid
		 * not-null constraints over virtual generated columns; instead, they
		 * are collected in notnull_virtual_attrs for verification elsewhere.
		 *
		 * notnull_attrs 并不收集虚拟生成列上有效 NOT NULL 约束的属性号；那些放进
		 * notnull_virtual_attrs，到别处验证。
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(newTupDesc, i);

			if (attr->attnullability == ATTNULLABLE_VALID &&
				!attr->attisdropped)
			{
				Form_pg_attribute wholeatt = TupleDescAttr(newTupDesc, i);

				if (wholeatt->attgenerated != ATTRIBUTE_GENERATED_VIRTUAL)
					notnull_attrs = lappend_int(notnull_attrs, wholeatt->attnum);
				else
					notnull_virtual_attrs = lappend_int(notnull_virtual_attrs,
														wholeatt->attnum);
			}
		}
		if (notnull_attrs || notnull_virtual_attrs)
			needscan = true;
	}

	if (newrel || needscan)
	{
		ExprContext *econtext;
		TupleTableSlot *oldslot;
		TupleTableSlot *newslot;
		TableScanDesc scan;
		MemoryContext oldCxt;
		List	   *dropped_attrs = NIL;
		ListCell   *lc;
		Snapshot	snapshot;
		ResultRelInfo *rInfo = NULL;

		/*
		 * When adding or changing a virtual generated column with a not-null
		 * constraint, we need to evaluate whether the generation expression
		 * is null.  For that, we borrow ExecRelGenVirtualNotNull().  Here, we
		 * prepare a dummy ResultRelInfo.
		 *
		 * 给带 NOT NULL 约束的虚拟生成列做新增或修改时，需要求值生成表达式是否为 null。为此借用
		 * ExecRelGenVirtualNotNull()。这里准备一个占位的 ResultRelInfo。
		 */
		if (notnull_virtual_attrs != NIL)
		{
			MemoryContext oldcontext;

			Assert(newTupDesc->constr->has_generated_virtual);
			Assert(newTupDesc->constr->has_not_null);
			oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
			rInfo = makeNode(ResultRelInfo);
			InitResultRelInfo(rInfo,
							  oldrel,
							  0,	/* dummy rangetable index */
								/*
								 *
								 * 占位用的 rangetable 下标
								 */
							  NULL,
							  estate->es_instrument);
			MemoryContextSwitchTo(oldcontext);
		}

		if (newrel)
			ereport(DEBUG1,
					(errmsg_internal("rewriting table \"%s\"",
									 RelationGetRelationName(oldrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("verifying table \"%s\"",
									 RelationGetRelationName(oldrel))));

		if (newrel)
		{
			/*
			 * All predicate locks on the tuples or pages are about to be made
			 * invalid, because we move tuples around.  Promote them to
			 * relation locks.
			 *
			 * 元组或页上的所有谓词锁即将失效，因为我们要搬动元组。把它们提升为关系锁。
			 */
			TransferPredicateLocksToHeapRelation(oldrel);
		}

		econtext = GetPerTupleExprContext(estate);

		/*
		 * Create necessary tuple slots. When rewriting, two slots are needed,
		 * otherwise one suffices. In the case where one slot suffices, we
		 * need to use the new tuple descriptor, otherwise some constraints
		 * can't be evaluated.  Note that even when the tuple layout is the
		 * same and no rewrite is required, the tupDescs might not be
		 * (consider ADD COLUMN without a default).
		 *
		 * 创建所需的元组槽。重写时需要两个槽，否则一个就够。只用一个槽时，必须用新的元组描述符，否则有些约束无法求值。注意即使元组布局相同、
		 * 不必重写，tupDesc 也可能不同（考虑不带默认值的 ADD COLUMN）。
		 */
		if (tab->rewrite)
		{
			Assert(newrel != NULL);
			oldslot = MakeSingleTupleTableSlot(oldTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(newrel));

			/*
			 * Set all columns in the new slot to NULL initially, to ensure
			 * columns added as part of the rewrite are initialized to NULL.
			 * That is necessary as tab->newvals will not contain an
			 * expression for columns with a NULL default, e.g. when adding a
			 * column without a default together with a column with a default
			 * requiring an actual rewrite.
			 *
			 * 先把新槽里的所有列设成 NULL，确保作为重写的一部分加进来的列被初始化为 NULL。这是必要的，因为对默认值为 NULL 的列，
			 * tab->newvals 不会有表达式，例如同时加一个无默认值的列和一个默认值会导致真正重写的列时。
			 */
			ExecStoreAllNullTuple(newslot);
		}
		else
		{
			oldslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = NULL;
		}

		/*
		 * Any attributes that are dropped according to the new tuple
		 * descriptor can be set to NULL. We precompute the list of dropped
		 * attributes to avoid needing to do so in the per-tuple loop.
		 *
		 * 按新元组描述符已被删除的属性可以设成 NULL。预先算出已删除属性的列表，免得在每个元组的循环里再算。
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (TupleDescAttr(newTupDesc, i)->attisdropped)
				dropped_attrs = lappend_int(dropped_attrs, i);
		}

		/*
		 * Scan through the rows, generating a new row if needed and then
		 * checking all the constraints.
		 *
		 * 逐行扫描，需要时生成新行，然后检查所有约束。
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(oldrel, snapshot, 0, NULL);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 *
		 * 切换到每元组内存上下文，每产生一个元组就重置，以免泄漏内存。
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, oldslot))
		{
			TupleTableSlot *insertslot;

			if (tab->rewrite > 0)
			{
				/* Extract data from old tuple */
				/*
				 *
				 * 从旧元组取出数据
				 */
				slot_getallattrs(oldslot);
				ExecClearTuple(newslot);

				/* copy attributes */
				/*
				 *
				 * 拷贝属性
				 */
				memcpy(newslot->tts_values, oldslot->tts_values,
					   sizeof(Datum) * oldslot->tts_nvalid);
				memcpy(newslot->tts_isnull, oldslot->tts_isnull,
					   sizeof(bool) * oldslot->tts_nvalid);

				/* Set dropped attributes to null in new tuple */
				/*
				 *
				 * 把新元组里已删除的属性设成 null
				 */
				foreach(lc, dropped_attrs)
					newslot->tts_isnull[lfirst_int(lc)] = true;

				/*
				 * Constraints and GENERATED expressions might reference the
				 * tableoid column, so fill tts_tableOid with the desired
				 * value.  (We must do this each time, because it gets
				 * overwritten with newrel's OID during storing.)
				 *
				 * 约束和 GENERATED 表达式可能引用 tableoid 列，所以用期望的值填 tts_tableOid。（每次都必须做，
				 * 因为存入时它会被盖成 newrel 的 OID。）
				 */
				newslot->tts_tableOid = RelationGetRelid(oldrel);

				/*
				 * Process supplied expressions to replace selected columns.
				 *
				 * 处理给出的表达式，替换选定的列。
				 *
				 * First, evaluate expressions whose inputs come from the old
				 * tuple.
				 *
				 * 先对输入来自旧元组的表达式求值。
				 */
				econtext->ecxt_scantuple = oldslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					if (ex->is_generated)
						continue;

					newslot->tts_values[ex->attnum - 1]
						= ExecEvalExpr(ex->exprstate,
									   econtext,
									   &newslot->tts_isnull[ex->attnum - 1]);
				}

				ExecStoreVirtualTuple(newslot);

				/*
				 * Now, evaluate any expressions whose inputs come from the
				 * new tuple.  We assume these columns won't reference each
				 * other, so that there's no ordering dependency.
				 *
				 * 再对输入来自新元组的表达式求值。假定这些列不会互相引用，因此没有顺序依赖。
				 */
				econtext->ecxt_scantuple = newslot;

				foreach(l, tab->newvals)
				{
					NewColumnValue *ex = lfirst(l);

					if (!ex->is_generated)
						continue;

					newslot->tts_values[ex->attnum - 1]
						= ExecEvalExpr(ex->exprstate,
									   econtext,
									   &newslot->tts_isnull[ex->attnum - 1]);
				}

				insertslot = newslot;
			}
			else
			{
				/*
				 * If there's no rewrite, old and new table are guaranteed to
				 * have the same AM, so we can just use the old slot to verify
				 * new constraints etc.
				 *
				 * 若不重写，新旧表保证使用相同的访问方法，所以可以直接用旧槽来验证新约束等。
				 */
				insertslot = oldslot;
			}

			/* Now check any constraints on the possibly-changed tuple */
			/*
			 *
			 * 现在检查可能已改变的元组上的所有约束
			 */
			econtext->ecxt_scantuple = insertslot;

			foreach_int(attn, notnull_attrs)
			{
				if (slot_attisnull(insertslot, attn))
				{
					Form_pg_attribute attr = TupleDescAttr(newTupDesc, attn - 1);

					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("column \"%s\" of relation \"%s\" contains null values",
									NameStr(attr->attname),
									RelationGetRelationName(oldrel)),
							 errtablecol(oldrel, attn)));
				}
			}

			if (notnull_virtual_attrs != NIL)
			{
				AttrNumber	attnum;

				attnum = ExecRelGenVirtualNotNull(rInfo, insertslot,
												  estate,
												  notnull_virtual_attrs);
				if (attnum != InvalidAttrNumber)
				{
					Form_pg_attribute attr = TupleDescAttr(newTupDesc, attnum - 1);

					ereport(ERROR,
							errcode(ERRCODE_NOT_NULL_VIOLATION),
							errmsg("column \"%s\" of relation \"%s\" contains null values",
								   NameStr(attr->attname),
								   RelationGetRelationName(oldrel)),
							errtablecol(oldrel, attnum));
				}
			}

			foreach(l, tab->constraints)
			{
				NewConstraint *con = lfirst(l);

				switch (con->contype)
				{
					case CONSTR_CHECK:
						if (!ExecCheck(con->qualstate, econtext))
							ereport(ERROR,
									(errcode(ERRCODE_CHECK_VIOLATION),
									 errmsg("check constraint \"%s\" of relation \"%s\" is violated by some row",
											con->name,
											RelationGetRelationName(oldrel)),
									 errtableconstraint(oldrel, con->name)));
						break;
					case CONSTR_NOTNULL:
					case CONSTR_FOREIGN:
						/* Nothing to do here */
						/*
						 *
						 * 这里无事可做
						 */
						break;
					default:
						elog(ERROR, "unrecognized constraint type: %d",
							 (int) con->contype);
				}
			}

			if (partqualstate && !ExecCheck(partqualstate, econtext))
			{
				if (tab->validate_default)
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("updated partition constraint for default partition \"%s\" would be violated by some row",
									RelationGetRelationName(oldrel)),
							 errtable(oldrel)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("partition constraint of relation \"%s\" is violated by some row",
									RelationGetRelationName(oldrel)),
							 errtable(oldrel)));
			}

			/* Write the tuple out to the new relation */
			/*
			 *
			 * 把元组写到新关系里
			 */
			if (newrel)
				table_tuple_insert(newrel, insertslot, mycid,
								   ti_options, bistate);

			ResetExprContext(econtext);

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		ExecDropSingleTupleTableSlot(oldslot);
		if (newslot)
			ExecDropSingleTupleTableSlot(newslot);
	}

	FreeExecutorState(estate);

	table_close(oldrel, NoLock);
	if (newrel)
	{
		FreeBulkInsertState(bistate);

		table_finish_bulk_insert(newrel, ti_options);

		table_close(newrel, NoLock);
	}
}

/*
 * ATGetQueueEntry: find or create an entry in the ALTER TABLE work queue
 *
 * ATGetQueueEntry：在 ALTER TABLE 工作队列里查找或创建一项
 */
static AlteredTableInfo *
ATGetQueueEntry(List **wqueue, Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	AlteredTableInfo *tab;
	ListCell   *ltab;

	foreach(ltab, *wqueue)
	{
		tab = (AlteredTableInfo *) lfirst(ltab);
		if (tab->relid == relid)
			return tab;
	}

	/*
	 * Not there, so add it.  Note that we make a copy of the relation's
	 * existing descriptor before anything interesting can happen to it.
	 *
	 * 不在队列里，于是加上。注意在发生任何有趣的事之前，先把关系现有描述符拷一份。
	 */
	tab = (AlteredTableInfo *) palloc0(sizeof(AlteredTableInfo));
	tab->relid = relid;
	tab->rel = NULL;			/* set later */
						/*
						 *
						 * 稍后再设
						 */
	tab->relkind = rel->rd_rel->relkind;
	tab->oldDesc = CreateTupleDescCopyConstr(RelationGetDescr(rel));
	tab->newAccessMethod = InvalidOid;
	tab->chgAccessMethod = false;
	tab->newTableSpace = InvalidOid;
	tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
	tab->chgPersistence = false;

	*wqueue = lappend(*wqueue, tab);

	return tab;
}

/*
 * 把 AlterTableType 转成报错用的 SQL 命令文本。
 */
static const char *
alter_table_type_to_string(AlterTableType cmdtype)
{
	switch (cmdtype)
	{
		case AT_AddColumn:
		case AT_AddColumnToView:
			return "ADD COLUMN";
		case AT_ColumnDefault:
		case AT_CookedColumnDefault:
			return "ALTER COLUMN ... SET DEFAULT";
		case AT_DropNotNull:
			return "ALTER COLUMN ... DROP NOT NULL";
		case AT_SetNotNull:
			return "ALTER COLUMN ... SET NOT NULL";
		case AT_SetExpression:
			return "ALTER COLUMN ... SET EXPRESSION";
		case AT_DropExpression:
			return "ALTER COLUMN ... DROP EXPRESSION";
		case AT_SetStatistics:
			return "ALTER COLUMN ... SET STATISTICS";
		case AT_SetOptions:
			return "ALTER COLUMN ... SET";
		case AT_ResetOptions:
			return "ALTER COLUMN ... RESET";
		case AT_SetStorage:
			return "ALTER COLUMN ... SET STORAGE";
		case AT_SetCompression:
			return "ALTER COLUMN ... SET COMPRESSION";
		case AT_DropColumn:
			return "DROP COLUMN";
		case AT_AddIndex:
		case AT_ReAddIndex:
			return NULL;		/* not real grammar */
						/*
						 *
						 * 并不是真正的语法
						 */
		case AT_AddConstraint:
		case AT_ReAddConstraint:
		case AT_ReAddDomainConstraint:
		case AT_AddIndexConstraint:
			return "ADD CONSTRAINT";
		case AT_AlterConstraint:
			return "ALTER CONSTRAINT";
		case AT_ValidateConstraint:
			return "VALIDATE CONSTRAINT";
		case AT_DropConstraint:
			return "DROP CONSTRAINT";
		case AT_ReAddComment:
			return NULL;		/* not real grammar */
						/*
						 *
						 * 并不是真正的语法
						 */
		case AT_AlterColumnType:
			return "ALTER COLUMN ... SET DATA TYPE";
		case AT_AlterColumnGenericOptions:
			return "ALTER COLUMN ... OPTIONS";
		case AT_ChangeOwner:
			return "OWNER TO";
		case AT_ClusterOn:
			return "CLUSTER ON";
		case AT_DropCluster:
			return "SET WITHOUT CLUSTER";
		case AT_SetAccessMethod:
			return "SET ACCESS METHOD";
		case AT_SetLogged:
			return "SET LOGGED";
		case AT_SetUnLogged:
			return "SET UNLOGGED";
		case AT_DropOids:
			return "SET WITHOUT OIDS";
		case AT_SetTableSpace:
			return "SET TABLESPACE";
		case AT_SetRelOptions:
			return "SET";
		case AT_ResetRelOptions:
			return "RESET";
		case AT_ReplaceRelOptions:
			return NULL;		/* not real grammar */
						/*
						 *
						 * 并不是真正的语法
						 */
		case AT_EnableTrig:
			return "ENABLE TRIGGER";
		case AT_EnableAlwaysTrig:
			return "ENABLE ALWAYS TRIGGER";
		case AT_EnableReplicaTrig:
			return "ENABLE REPLICA TRIGGER";
		case AT_DisableTrig:
			return "DISABLE TRIGGER";
		case AT_EnableTrigAll:
			return "ENABLE TRIGGER ALL";
		case AT_DisableTrigAll:
			return "DISABLE TRIGGER ALL";
		case AT_EnableTrigUser:
			return "ENABLE TRIGGER USER";
		case AT_DisableTrigUser:
			return "DISABLE TRIGGER USER";
		case AT_EnableRule:
			return "ENABLE RULE";
		case AT_EnableAlwaysRule:
			return "ENABLE ALWAYS RULE";
		case AT_EnableReplicaRule:
			return "ENABLE REPLICA RULE";
		case AT_DisableRule:
			return "DISABLE RULE";
		case AT_AddInherit:
			return "INHERIT";
		case AT_DropInherit:
			return "NO INHERIT";
		case AT_AddOf:
			return "OF";
		case AT_DropOf:
			return "NOT OF";
		case AT_ReplicaIdentity:
			return "REPLICA IDENTITY";
		case AT_EnableRowSecurity:
			return "ENABLE ROW SECURITY";
		case AT_DisableRowSecurity:
			return "DISABLE ROW SECURITY";
		case AT_ForceRowSecurity:
			return "FORCE ROW SECURITY";
		case AT_NoForceRowSecurity:
			return "NO FORCE ROW SECURITY";
		case AT_GenericOptions:
			return "OPTIONS";
		case AT_AttachPartition:
			return "ATTACH PARTITION";
		case AT_DetachPartition:
			return "DETACH PARTITION";
		case AT_DetachPartitionFinalize:
			return "DETACH PARTITION ... FINALIZE";
		case AT_AddIdentity:
			return "ALTER COLUMN ... ADD IDENTITY";
		case AT_SetIdentity:
			return "ALTER COLUMN ... SET";
		case AT_DropIdentity:
			return "ALTER COLUMN ... DROP IDENTITY";
		case AT_ReAddStatistics:
			return NULL;		/* not real grammar */
						/*
						 *
						 * 并不是真正的语法
						 */
	}

	return NULL;
}

/*
 * ATSimplePermissions
 *
 * 函数 ATSimplePermissions
 *
 * - Ensure that it is a relation (or possibly a view)
 * - Ensure this user is the owner
 * - Ensure that it is not a system table
 *
 * 确认它是关系（或可能是视图）；确认当前用户是属主；确认它不是系统表
 */
static void
ATSimplePermissions(AlterTableType cmdtype, Relation rel, int allowed_targets)
{
	int			actual_target;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			actual_target = ATT_TABLE;
			break;
		case RELKIND_PARTITIONED_TABLE:
			actual_target = ATT_PARTITIONED_TABLE;
			break;
		case RELKIND_VIEW:
			actual_target = ATT_VIEW;
			break;
		case RELKIND_MATVIEW:
			actual_target = ATT_MATVIEW;
			break;
		case RELKIND_INDEX:
			actual_target = ATT_INDEX;
			break;
		case RELKIND_PARTITIONED_INDEX:
			actual_target = ATT_PARTITIONED_INDEX;
			break;
		case RELKIND_COMPOSITE_TYPE:
			actual_target = ATT_COMPOSITE_TYPE;
			break;
		case RELKIND_FOREIGN_TABLE:
			actual_target = ATT_FOREIGN_TABLE;
			break;
		case RELKIND_SEQUENCE:
			actual_target = ATT_SEQUENCE;
			break;
		default:
			actual_target = 0;
			break;
	}

	/* Wrong target type? */
	/*
	 *
	 * 目标类型不对？
	 */
	if ((actual_target & allowed_targets) == 0)
	{
		const char *action_str = alter_table_type_to_string(cmdtype);

		if (action_str)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			/* translator: %s is a group of some SQL keywords */
			/*
			 *
			 * 翻译提示：%s 是一组 SQL 关键字
			 */
					 errmsg("ALTER action %s cannot be performed on relation \"%s\"",
							action_str, RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
		else
			/* internal error? */
			/*
			 *
			 * 内部错误？
			 */
			elog(ERROR, "invalid ALTER action attempted on relation \"%s\"",
				 RelationGetRelationName(rel));
	}

	/* Permissions checks */
	/*
	 *
	 * 权限检查
	 */
	if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));
}

/*
 * ATSimpleRecursion
 *
 * 函数 ATSimpleRecursion
 *
 * Simple table recursion sufficient for most ALTER TABLE operations.
 * All direct and indirect children are processed in an unspecified order.
 * Note that if a child inherits from the original table via multiple
 * inheritance paths, it will be visited just once.
 *
 * 对大多数 ALTER TABLE 操作，简单的表递归就够了。所有直接和间接子表按未指定的顺序处理。
 * 注意若某个子表经由多条继承路径继承自原表，只会访问一次。
 */
static void
ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	/*
	 * Propagate to children, if desired and if there are (or might be) any
	 * children.
	 *
	 * 若希望传播，并且存在（或可能存在）子表，就传播到子表。
	 */
	if (recurse && rel->rd_rel->relhassubclass)
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		children = find_all_inheritors(relid, lockmode, NULL);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 *
		 * find_all_inheritors 会递归搜索继承层次，我们只要处理它返回的全部 relid。
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;

			if (childrelid == relid)
				continue;
			/* find_all_inheritors already got lock */
			/*
			 *
			 * find_all_inheritors 已经拿到锁
			 */
			childrel = relation_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
}

/*
 * Obtain list of partitions of the given table, locking them all at the given
 * lockmode and ensuring that they all pass CheckAlterTableIsSafe.
 *
 * 取得给定表的分区列表，全部按给定锁模式加锁，并确保它们都通过 CheckAlterTableIsSafe。
 *
 * This function is a no-op if the given relation is not a partitioned table;
 * in particular, nothing is done if it's a legacy inheritance parent.
 *
 * 若给定关系不是分区表，本函数什么也不做；尤其当它只是传统继承的父表时不做任何事。
 */
static void
ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode)
{
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *inh;
		ListCell   *cell;

		inh = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);
		/* first element is the parent rel; must ignore it */
		/*
		 *
		 * 第一个元素是父关系；必须忽略它
		 */
		for_each_from(cell, inh, 1)
		{
			Relation	childrel;

			/* find_all_inheritors already got lock */
			/*
			 *
			 * find_all_inheritors 已经拿到锁
			 */
			childrel = table_open(lfirst_oid(cell), NoLock);
			CheckAlterTableIsSafe(childrel);
			table_close(childrel, NoLock);
		}
		list_free(inh);
	}
}

/*
 * ATTypedTableRecursion
 *
 * 函数 ATTypedTableRecursion
 *
 * Propagate ALTER TYPE operations to the typed tables of that type.
 * Also check the RESTRICT/CASCADE behavior.  Given CASCADE, also permit
 * recursion to inheritance children of the typed tables.
 *
 * 把 ALTER TYPE 操作传播到该类型的类型表。同时检查 RESTRICT/CASCADE 行为。若给出 CASCADE，
 * 还允许递归到类型表的继承子表。
 */
static void
ATTypedTableRecursion(List **wqueue, Relation rel, AlterTableCmd *cmd,
					  LOCKMODE lockmode, AlterTableUtilityContext *context)
{
	ListCell   *child;
	List	   *children;

	Assert(rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

	children = find_typed_table_dependencies(rel->rd_rel->reltype,
											 RelationGetRelationName(rel),
											 cmd->behavior);

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;

		childrel = relation_open(childrelid, lockmode);
		CheckAlterTableIsSafe(childrel);
		ATPrepCmd(wqueue, childrel, cmd, true, true, lockmode, context);
		relation_close(childrel, NoLock);
	}
}


/*
 * find_composite_type_dependencies
 *
 * 函数 find_composite_type_dependencies
 *
 * Check to see if the type "typeOid" is being used as a column in some table
 * (possibly nested several levels deep in composite types, arrays, etc!).
 * Eventually, we'd like to propagate the check or rewrite operation
 * into such tables, but for now, just error out if we find any.
 *
 * 检查类型 typeOid 是否被某张表当作列类型使用（可能在复合类型、数组等里面嵌了好几层）。
 * 将来我们希望把检查或重写操作传播到这些表，但目前只要发现就报错。
 *
 * Caller should provide either the associated relation of a rowtype,
 * or a type name (not both) for use in the error message, if any.
 *
 * 调用方应提供行类型的关联关系，或一个类型名（不要两个都给），以便在报错时使用。
 *
 * Note that "typeOid" is not necessarily a composite type; it could also be
 * another container type such as an array or range, or a domain over one of
 * these things.  The name of this function is therefore somewhat historical,
 * but it's not worth changing.
 *
 * 注意 typeOid 不一定是复合类型；它也可以是数组或范围这类容器类型，或这些东西上的域。因此函数名有点历史包袱，但不值得改。
 *
 * We assume that functions and views depending on the type are not reasons
 * to reject the ALTER.  (How safe is this really?)
 *
 * 我们假定依赖该类型的函数和视图不是拒绝这次 ALTER 的理由。（这到底有多安全？）
 */
void
find_composite_type_dependencies(Oid typeOid, Relation origRelation,
								 const char *origTypeName)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	/* since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 本函数会递归，有可能被逼到栈溢出
	 */
	check_stack_depth();

	/*
	 * We scan pg_depend to find those things that depend on the given type.
	 * (We assume we can ignore refobjsubid for a type.)
	 *
	 * 扫描 pg_depend，找出依赖给定类型的那些东西。（假定对类型可以忽略 refobjsubid。）
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		Relation	rel;
		TupleDesc	tupleDesc;
		Form_pg_attribute att;

		/* Check for directly dependent types */
		/*
		 *
		 * 检查直接依赖的类型
		 */
		if (pg_depend->classid == TypeRelationId)
		{
			/*
			 * This must be an array, domain, or range containing the given
			 * type, so recursively check for uses of this type.  Note that
			 * any error message will mention the original type not the
			 * container; this is intentional.
			 *
			 * 这必定是包含给定类型的数组、域或范围，于是递归检查该类型的使用。注意任何报错都会提到原始类型而不是容器；这是有意的。
			 */
			find_composite_type_dependencies(pg_depend->objid,
											 origRelation, origTypeName);
			continue;
		}

		/* Else, ignore dependees that aren't relations */
		/*
		 *
		 * 否则，忽略不是关系的依赖者
		 */
		if (pg_depend->classid != RelationRelationId)
			continue;

		rel = relation_open(pg_depend->objid, AccessShareLock);
		tupleDesc = RelationGetDescr(rel);

		/*
		 * If objsubid identifies a specific column, refer to that in error
		 * messages.  Otherwise, search to see if there's a user column of the
		 * type.  (We assume system columns are never of interesting types.)
		 * The search is needed because an index containing an expression
		 * column of the target type will just be recorded as a whole-relation
		 * dependency.  If we do not find a column of the type, the dependency
		 * must indicate that the type is transiently referenced in an index
		 * expression but not stored on disk, which we assume is OK, just as
		 * we do for references in views.  (It could also be that the target
		 * type is embedded in some container type that is stored in an index
		 * column, but the previous recursion should catch such cases.)
		 *
		 * 若 objsubid 指向某一列，报错时就提到那一列。否则搜索是否有用户列使用该类型。（假定系统列从不会是我们关心的类型。）
		 * 需要搜索，是因为包含目标类型表达式列的索引只会记成整关系依赖。若找不到该类型的列，则依赖表示该类型只是在索引表达式里被短暂引用、
		 * 并没有存到磁盘上，我们假定这没问题，就像视图里的引用一样。（也可能目标类型嵌在某个存在索引列里的容器类型中，
		 * 但前面的递归应能抓住这类情况。）
		 */
		if (pg_depend->objsubid > 0 && pg_depend->objsubid <= tupleDesc->natts)
			att = TupleDescAttr(tupleDesc, pg_depend->objsubid - 1);
		else
		{
			att = NULL;
			for (int attno = 1; attno <= tupleDesc->natts; attno++)
			{
				att = TupleDescAttr(tupleDesc, attno - 1);
				if (att->atttypid == typeOid && !att->attisdropped)
					break;
				att = NULL;
			}
			if (att == NULL)
			{
				/* No such column, so assume OK */
				/*
				 *
				 * 没有这样的列，于是假定没问题
				 */
				relation_close(rel, AccessShareLock);
				continue;
			}
		}

		/*
		 * We definitely should reject if the relation has storage.  If it's
		 * partitioned, then perhaps we don't have to reject: if there are
		 * partitions then we'll fail when we find one, else there is no
		 * stored data to worry about.  However, it's possible that the type
		 * change would affect conclusions about whether the type is sortable
		 * or hashable and thus (if it's a partitioning column) break the
		 * partitioning rule.  For now, reject for partitioned rels too.
		 *
		 * 关系若有存储，肯定应该拒绝。若它是分区的，也许不必拒绝：有分区的话我们会在找到分区时报错，否则没有已存储的数据可担心。
		 * 不过类型变更可能影响该类型是否可排序或可哈希，从而（若它是分区列）破坏分区规则。目前对分区关系也拒绝。
		 */
		if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind) ||
			RELKIND_HAS_PARTITIONS(rel->rd_rel->relkind))
		{
			if (origTypeName)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
								origTypeName,
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else if (origRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter type \"%s\" because column \"%s.%s\" uses it",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else if (origRelation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter foreign table \"%s\" because column \"%s.%s\" uses its row type",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot alter table \"%s\" because column \"%s.%s\" uses its row type",
								RelationGetRelationName(origRelation),
								RelationGetRelationName(rel),
								NameStr(att->attname))));
		}
		else if (OidIsValid(rel->rd_rel->reltype))
		{
			/*
			 * A view or composite type itself isn't a problem, but we must
			 * recursively check for indirect dependencies via its rowtype.
			 *
			 * 视图或复合类型本身不是问题，但必须递归检查经由其行类型的间接依赖。
			 */
			find_composite_type_dependencies(rel->rd_rel->reltype,
											 origRelation, origTypeName);
		}

		relation_close(rel, AccessShareLock);
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);
}


/*
 * find_typed_table_dependencies
 *
 * 函数 find_typed_table_dependencies
 *
 * Check to see if a composite type is being used as the type of a
 * typed table.  Abort if any are found and behavior is RESTRICT.
 * Else return the list of tables.
 *
 * 检查复合类型是否被用作类型表的类型。若找到且行为是 RESTRICT，就中止。否则返回这些表的列表。
 */
static List *
find_typed_table_dependencies(Oid typeOid, const char *typeName, DropBehavior behavior)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_reloftype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = table_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);

		if (behavior == DROP_RESTRICT)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot alter type \"%s\" because it is the type of a typed table",
							typeName),
					 errhint("Use ALTER ... CASCADE to alter the typed tables too.")));
		else
			result = lappend_oid(result, classform->oid);
	}

	table_endscan(scan);
	table_close(classRel, AccessShareLock);

	return result;
}


/*
 * check_of_type
 *
 * 函数 check_of_type
 *
 * Check whether a type is suitable for CREATE TABLE OF/ALTER TABLE OF.  If it
 * isn't suitable, throw an error.  Currently, we require that the type
 * originated with CREATE TYPE AS.  We could support any row type, but doing so
 * would require handling a number of extra corner cases in the DDL commands.
 * (Also, allowing domain-over-composite would open up a can of worms about
 * whether and how the domain's constraints should apply to derived tables.)
 *
 * 检查类型是否适合 CREATE TABLE OF/ALTER TABLE OF。不适合就报错。目前要求该类型来自 CREATE
 * TYPE AS。我们本可以支持任何行类型，但那样就要在 DDL 命令里处理许多额外的边角情况。（另外，
 * 允许复合类型上的域会引出一堆问题：域的约束是否以及如何应用到派生表。）
 */
void
check_of_type(HeapTuple typetuple)
{
	Form_pg_type typ = (Form_pg_type) GETSTRUCT(typetuple);
	bool		typeOk = false;

	if (typ->typtype == TYPTYPE_COMPOSITE)
	{
		Relation	typeRelation;

		Assert(OidIsValid(typ->typrelid));
		typeRelation = relation_open(typ->typrelid, AccessShareLock);
		typeOk = (typeRelation->rd_rel->relkind == RELKIND_COMPOSITE_TYPE);

		/*
		 * Close the parent rel, but keep our AccessShareLock on it until xact
		 * commit.  That will prevent someone else from deleting or ALTERing
		 * the type before the typed table creation/conversion commits.
		 *
		 * 关闭父关系，但把 AccessShareLock 保持到事务提交。这样在类型表创建或转换提交之前，别人无法删除或 ALTER
		 * 该类型。
		 */
		relation_close(typeRelation, NoLock);

		if (!typeOk)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is the row type of another table",
							format_type_be(typ->oid)),
					 errdetail("A typed table must use a stand-alone composite type created with CREATE TYPE.")));
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("type %s is not a composite type",
						format_type_be(typ->oid))));
}


/*
 * ALTER TABLE ADD COLUMN
 *
 * ALTER TABLE ADD COLUMN（加列）
 *
 * Adds an additional attribute to a relation making the assumption that
 * CHECK, NOT NULL, and FOREIGN KEY constraints will be removed from the
 * AT_AddColumn AlterTableCmd by parse_utilcmd.c and added as independent
 * AlterTableCmd's.
 *
 * 给关系增加一个属性。假定 CHECK、NOT NULL 和 FOREIGN KEY 约束会由 parse_utilcmd.c 从
 * AT_AddColumn 这条 AlterTableCmd 里拿掉，并作为独立的 AlterTableCmd 加上。
 *
 * ADD COLUMN cannot use the normal ALTER TABLE recursion mechanism, because we
 * have to decide at runtime whether to recurse or not depending on whether we
 * actually add a column or merely merge with an existing column.  (We can't
 * check this in a static pre-pass because it won't handle multiple inheritance
 * situations correctly.)
 *
 * ADD COLUMN 不能用普通的 ALTER TABLE 递归机制，因为必须在运行时决定是递归还是不递归，
 * 取决于我们是真的加了一列，还是只是和已有列合并。（不能在静态的预先扫描里检查，因为处理不好多重继承。）
 */
static void
ATPrepAddColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				bool is_view, AlterTableCmd *cmd, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);

	if (recurse && !is_view)
		cmd->recurse = true;
}

/*
 * Add a column to a table.  The return value is the address of the
 * new column in the parent relation.
 *
 * 给表加一列。返回值是父关系中新列的地址。
 *
 * cmd is pass-by-ref so that we can replace it with the parse-transformed
 * copy (but that happens only after we check for IF NOT EXISTS).
 *
 * cmd 按引用传递，这样我们可以把它换成解析变换后的副本（但这只在检查过 IF NOT EXISTS 之后才发生）。
 */
static ObjectAddress
ATExecAddColumn(List **wqueue, AlteredTableInfo *tab, Relation rel,
				AlterTableCmd **cmd, bool recurse, bool recursing,
				LOCKMODE lockmode, AlterTablePass cur_pass,
				AlterTableUtilityContext *context)
{
	Oid			myrelid = RelationGetRelid(rel);
	ColumnDef  *colDef = castNode(ColumnDef, (*cmd)->def);
	bool		if_not_exists = (*cmd)->missing_ok;
	Relation	pgclass,
				attrdesc;
	HeapTuple	reltup;
	Form_pg_class relform;
	Form_pg_attribute attribute;
	int			newattnum;
	char		relkind;
	Expr	   *defval;
	List	   *children;
	ListCell   *child;
	AlterTableCmd *childcmd;
	ObjectAddress address;
	TupleDesc	tupdesc;

	/* since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 本函数会递归，有可能被逼到栈溢出
	 */
	check_stack_depth();

	/* At top level, permission check was done in ATPrepCmd, else do it */
	/*
	 *
	 * 顶层的权限检查已在 ATPrepCmd 做过，否则在这里做
	 */
	if (recursing)
		ATSimplePermissions((*cmd)->subtype, rel,
							ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to a partition")));

	attrdesc = table_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * Are we adding the column to a recursion child?  If so, check whether to
	 * merge with an existing definition for the column.  If we do merge, we
	 * must not recurse.  Children will already have the column, and recursing
	 * into them would mess up attinhcount.
	 *
	 * 是否正在给递归中的子表加列？若是，检查要不要和该列已有定义合并。若合并，就绝不能再递归。子表已经有这一列，再递归进去会把
	 * attinhcount 搞乱。
	 */
	if (colDef->inhcount > 0)
	{
		HeapTuple	tuple;

		/* Does child already have a column by this name? */
		/*
		 *
		 * 子表是否已经有同名列？
		 */
		tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);
			Oid			ctypeId;
			int32		ctypmod;
			Oid			ccollid;

			/* Child column must match on type, typmod, and collation */
			/*
			 *
			 * 子列的类型、typmod 和排序规则必须匹配
			 */
			typenameTypeIdAndMod(NULL, colDef->typeName, &ctypeId, &ctypmod);
			if (ctypeId != childatt->atttypid ||
				ctypmod != childatt->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(rel), colDef->colname)));
			ccollid = GetColumnDefCollation(NULL, colDef, ctypeId);
			if (ccollid != childatt->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(rel), colDef->colname),
						 errdetail("\"%s\" versus \"%s\"",
								   get_collation_name(ccollid),
								   get_collation_name(childatt->attcollation))));

			/* Bump the existing child att's inhcount */
			/*
			 *
			 * 把已有子列的 inhcount 加一
			 */
			if (pg_add_s16_overflow(childatt->attinhcount, 1,
									&childatt->attinhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
			CatalogTupleUpdate(attrdesc, &tuple->t_self, tuple);

			heap_freetuple(tuple);

			/* Inform the user about the merge */
			/*
			 *
			 * 把这次合并告知用户
			 */
			ereport(NOTICE,
					(errmsg("merging definition of column \"%s\" for child \"%s\"",
							colDef->colname, RelationGetRelationName(rel))));

			table_close(attrdesc, RowExclusiveLock);

			/* Make the child column change visible */
			/*
			 *
			 * 让子列的修改可见
			 */
			CommandCounterIncrement();

			return InvalidObjectAddress;
		}
	}

	/* skip if the name already exists and if_not_exists is true */
	/*
	 *
	 * 若名字已存在且 if_not_exists 为真，则跳过
	 */
	if (!check_for_column_name_collision(rel, colDef->colname, if_not_exists))
	{
		table_close(attrdesc, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/*
	 * Okay, we need to add the column, so go ahead and do parse
	 * transformation.  This can result in queueing up, or even immediately
	 * executing, subsidiary operations (such as creation of unique indexes);
	 * so we mustn't do it until we have made the if_not_exists check.
	 *
	 * 好，需要加这一列，于是去做解析变换。这可能导致把附属操作（例如创建唯一索引）排进队列，甚至立刻执行；所以必须等做过
	 * if_not_exists 检查之后才能做。
	 *
	 * When recursing, the command was already transformed and we needn't do
	 * so again.  Also, if context isn't given we can't transform.  (That
	 * currently happens only for AT_AddColumnToView; we expect that view.c
	 * passed us a ColumnDef that doesn't need work.)
	 *
	 * 递归时命令已经变换过，不必再做。另外，若没有给出 context，也无法变换。（目前只在 AT_AddColumnToView
	 * 时发生；我们期望 view.c 传来的 ColumnDef 不需要再处理。）
	 */
	if (context != NULL && !recursing)
	{
		*cmd = ATParseTransformCmd(wqueue, tab, rel, *cmd, recurse, lockmode,
								   cur_pass, context);
		Assert(*cmd != NULL);
		colDef = castNode(ColumnDef, (*cmd)->def);
	}

	/*
	 * Regular inheritance children are independent enough not to inherit the
	 * identity column from parent hence cannot recursively add identity
	 * column if the table has inheritance children.
	 *
	 * 普通继承的子表足够独立，不继承父表的标识列，因此若表有继承子表，就不能递归地加标识列。
	 *
	 * Partitions, on the other hand, are integral part of a partitioned table
	 * and inherit identity column.  Hence propagate identity column down the
	 * partition hierarchy.
	 *
	 * 分区则是分区表不可分割的一部分，会继承标识列。因此把标识列沿着分区层次传下去。
	 */
	if (colDef->identity &&
		recurse &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		find_inheritance_children(myrelid, NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot recursively add identity column to table that has child tables")));

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relform = (Form_pg_class) GETSTRUCT(reltup);
	relkind = relform->relkind;

	/* Determine the new attribute's number */
	/*
	 *
	 * 确定新属性的编号
	 */
	newattnum = relform->relnatts + 1;
	if (newattnum > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * Construct new attribute's pg_attribute entry.
	 *
	 * 构造新属性的 pg_attribute 项。
	 */
	tupdesc = BuildDescForRelation(list_make1(colDef));

	attribute = TupleDescAttr(tupdesc, 0);

	/* Fix up attribute number */
	/*
	 *
	 * 修正属性号
	 */
	attribute->attnum = newattnum;

	/* make sure datatype is legal for a column */
	/*
	 *
	 * 确认数据类型可以作为列类型
	 */
	CheckAttributeType(NameStr(attribute->attname), attribute->atttypid, attribute->attcollation,
					   list_make1_oid(rel->rd_rel->reltype),
					   (attribute->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL ? CHKATYPE_IS_VIRTUAL : 0));

	InsertPgAttributeTuples(attrdesc, tupdesc, myrelid, NULL, NULL);

	table_close(attrdesc, RowExclusiveLock);

	/*
	 * Update pg_class tuple as appropriate
	 *
	 * 按需要更新 pg_class 元组
	 */
	relform->relnatts = newattnum;

	CatalogTupleUpdate(pgclass, &reltup->t_self, reltup);

	heap_freetuple(reltup);

	/* Post creation hook for new attribute */
	/*
	 *
	 * 新属性的创建后钩子
	 */
	InvokeObjectPostCreateHook(RelationRelationId, myrelid, newattnum);

	table_close(pgclass, RowExclusiveLock);

	/* Make the attribute's catalog entry visible */
	/*
	 *
	 * 使该属性的目录项可见
	 */
	CommandCounterIncrement();

	/*
	 * Store the DEFAULT, if any, in the catalogs
	 *
	 * 若有 DEFAULT，写入系统目录
	 */
	if (colDef->raw_default)
	{
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attribute->attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);
		rawEnt->generated = colDef->generated;

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 *
		 * 这个函数是给 CREATE TABLE 用的，所以它处理的是默认值的列表，而我们只做一项。
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);

		/* Make the additional catalog changes visible */
		/*
		 *
		 * 让额外的目录修改可见
		 */
		CommandCounterIncrement();
	}

	/*
	 * Tell Phase 3 to fill in the default expression, if there is one.
	 *
	 * 若有默认表达式，告诉 Phase 3 去填它。
	 *
	 * If there is no default, Phase 3 doesn't have to do anything, because
	 * that effectively means that the default is NULL.  The heap tuple access
	 * routines always check for attnum > # of attributes in tuple, and return
	 * NULL if so, so without any modification of the tuple data we will get
	 * the effect of NULL values in the new column.
	 *
	 * 若没有默认值，Phase 3 什么也不必做，因为这实际上意味着默认值是 NULL。堆元组访问例程总会检查 attnum
	 * 是否超过元组里的属性数，若超过就返回 NULL，所以不改元组数据就能得到新列全是 NULL 的效果。
	 *
	 * An exception occurs when the new column is of a domain type: the domain
	 * might have a not-null constraint, or a check constraint that indirectly
	 * rejects nulls.  If there are any domain constraints then we construct
	 * an explicit NULL default value that will be passed through
	 * CoerceToDomain processing.  (This is a tad inefficient, since it causes
	 * rewriting the table which we really wouldn't have to do; but we do it
	 * to preserve the historical behavior that such a failure will be raised
	 * only if the table currently contains some rows.)
	 *
	 * 新列是域类型时有个例外：域可能有 NOT NULL 约束，或间接拒绝 null 的检查约束。若存在任何域约束，就构造一个显式的
	 * NULL 默认值，让它走 CoerceToDomain 处理。（这有点低效，会导致本不必做的表重写；但我们这样做是为了保持历史行为：
	 * 只有表里当前有行时才会报这种失败。）
	 *
	 * Note: we use build_column_default, and not just the cooked default
	 * returned by AddRelationNewConstraints, so that the right thing happens
	 * when a datatype's default applies.
	 *
	 * 注意：我们用 build_column_default，而不是只用 AddRelationNewConstraints
	 * 返回的已煮好默认值，这样当数据类型自身的默认值适用时，行为才正确。
	 *
	 * Note: it might seem that this should happen at the end of Phase 2, so
	 * that the effects of subsequent subcommands can be taken into account.
	 * It's intentional that we do it now, though.  The new column should be
	 * filled according to what is said in the ADD COLUMN subcommand, so that
	 * the effects are the same as if this subcommand had been run by itself
	 * and the later subcommands had been issued in new ALTER TABLE commands.
	 *
	 * 注意：看起来这应该放在 Phase 2 结束时，以便计入后续子命令的影响。但现在就做是有意的。新列应按 ADD COLUMN
	 * 子命令所说的方式填充，这样效果就和单独跑这条子命令、以后的子命令在新的 ALTER TABLE 命令里发出一样。
	 *
	 * We can skip this entirely for relations without storage, since Phase 3
	 * is certainly not going to touch them.
	 *
	 * 对没有存储的关系可以完全跳过，因为 Phase 3 肯定不会碰它们。
	 */
	if (RELKIND_HAS_STORAGE(relkind))
	{
		bool		has_domain_constraints;
		bool		has_missing = false;

		/*
		 * For an identity column, we can't use build_column_default(),
		 * because the sequence ownership isn't set yet.  So do it manually.
		 *
		 * 对标识列不能用 build_column_default()，因为序列的所有权还没设好。所以手工来做。
		 */
		if (colDef->identity)
		{
			NextValueExpr *nve = makeNode(NextValueExpr);

			nve->seqid = RangeVarGetRelid(colDef->identitySequence, NoLock, false);
			nve->typeId = attribute->atttypid;

			defval = (Expr *) nve;
		}
		else
			defval = (Expr *) build_column_default(rel, attribute->attnum);

		/* Build CoerceToDomain(NULL) expression if needed */
		/*
		 *
		 * 若需要，构造 CoerceToDomain(NULL) 表达式
		 */
		has_domain_constraints = DomainHasConstraints(attribute->atttypid);
		if (!defval && has_domain_constraints)
		{
			Oid			baseTypeId;
			int32		baseTypeMod;
			Oid			baseTypeColl;

			baseTypeMod = attribute->atttypmod;
			baseTypeId = getBaseTypeAndTypmod(attribute->atttypid, &baseTypeMod);
			baseTypeColl = get_typcollation(baseTypeId);
			defval = (Expr *) makeNullConst(baseTypeId, baseTypeMod, baseTypeColl);
			defval = (Expr *) coerce_to_target_type(NULL,
													(Node *) defval,
													baseTypeId,
													attribute->atttypid,
													attribute->atttypmod,
													COERCION_ASSIGNMENT,
													COERCE_IMPLICIT_CAST,
													-1);
			if (defval == NULL) /* should not happen */
					    /*
					     *
					     * 不应发生
					     */
				elog(ERROR, "failed to coerce base type to domain");
		}

		if (defval)
		{
			NewColumnValue *newval;

			/* Prepare defval for execution, either here or in Phase 3 */
			/*
			 *
			 * 准备 defval 以便执行，要么在这里，要么在 Phase 3
			 */
			defval = expression_planner(defval);

			/* Add the new default to the newvals list */
			/*
			 *
			 * 把新的默认值加入 newvals 列表
			 */
			newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
			newval->attnum = attribute->attnum;
			newval->expr = defval;
			newval->is_generated = (colDef->generated != '\0');

			tab->newvals = lappend(tab->newvals, newval);

			/*
			 * Attempt to skip a complete table rewrite by storing the
			 * specified DEFAULT value outside of the heap.  This is only
			 * allowed for plain relations and non-generated columns, and the
			 * default expression can't be volatile (stable is OK).  Note that
			 * contain_volatile_functions deems CoerceToDomain immutable, but
			 * here we consider that coercion to a domain with constraints is
			 * volatile; else it might fail even when the table is empty.
			 *
			 * 尝试把指定的 DEFAULT 值存在堆之外，从而跳过完整的表重写。这只允许用于普通关系和非生成列，而且默认表达式不能是
			 * volatile（stable 可以）。注意 contain_volatile_functions 把
			 * CoerceToDomain 当成 immutable，但这里我们认为转换到带约束的域是 volatile；
			 * 否则即使表是空的也可能失败。
			 */
			if (rel->rd_rel->relkind == RELKIND_RELATION &&
				!colDef->generated &&
				!has_domain_constraints &&
				!contain_volatile_functions((Node *) defval))
			{
				EState	   *estate;
				ExprState  *exprState;
				Datum		missingval;
				bool		missingIsNull;

				/* Evaluate the default expression */
				/*
				 *
				 * 对默认表达式求值
				 */
				estate = CreateExecutorState();
				exprState = ExecPrepareExpr(defval, estate);
				missingval = ExecEvalExpr(exprState,
										  GetPerTupleExprContext(estate),
										  &missingIsNull);
				/* If it turns out NULL, nothing to do; else store it */
				/*
				 *
				 * 若结果是 NULL，则无事可做；否则存起来
				 */
				if (!missingIsNull)
				{
					StoreAttrMissingVal(rel, attribute->attnum, missingval);
					/* Make the additional catalog change visible */
					/*
					 *
					 * 让额外的目录修改可见
					 */
					CommandCounterIncrement();
					has_missing = true;
				}
				FreeExecutorState(estate);
			}
			else
			{
				/*
				 * Failed to use missing mode.  We have to do a table rewrite
				 * to install the value --- unless it's a virtual generated
				 * column.
				 *
				 * 没能走缺失值模式。必须重写表才能装上这个值，除非它是虚拟生成列。
				 */
				if (colDef->generated != ATTRIBUTE_GENERATED_VIRTUAL)
					tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
			}
		}

		if (!has_missing)
		{
			/*
			 * If the new column is NOT NULL, and there is no missing value,
			 * tell Phase 3 it needs to check for NULLs.
			 *
			 * 若新列是 NOT NULL，且没有缺失值，告诉 Phase 3 需要检查 NULL。
			 */
			tab->verify_new_notnull |= colDef->is_not_null;
		}
	}

	/*
	 * Add needed dependency entries for the new column.
	 *
	 * 为新列加上所需的依赖项。
	 */
	add_column_datatype_dependency(myrelid, newattnum, attribute->atttypid);
	add_column_collation_dependency(myrelid, newattnum, attribute->attcollation);

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 *
	 * 按需要传播到子表。和大多数其他 ALTER 例程不同，这里必须一层一层递归；不能用 find_all_inheritors
	 * 一次做完。
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * If we are told not to recurse, there had better not be any child
	 * tables; else the addition would put them out of step.
	 *
	 * 若被告知不要递归，最好没有子表；否则加列会让它们对不上。
	 */
	if (children && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column must be added to child tables too")));

	/* Children should see column as singly inherited */
	/*
	 *
	 * 子表应把这一列看成只继承了一次
	 */
	if (!recursing)
	{
		childcmd = copyObject(*cmd);
		colDef = castNode(ColumnDef, childcmd->def);
		colDef->inhcount = 1;
		colDef->is_local = false;
	}
	else
		childcmd = *cmd;		/* no need to copy again */
						/*
						 *
						 * 不必再拷贝一次
						 */

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children already got lock */
		/*
		 *
		 * find_inheritance_children 已经拿到锁
		 */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/* Find or create work queue entry for this table */
		/*
		 *
		 * 查找或创建这张表的工作队列项
		 */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to child; return value is ignored */
		/*
		 *
		 * 递归到子表；返回值被忽略
		 */
		ATExecAddColumn(wqueue, childtab, childrel,
						&childcmd, recurse, true,
						lockmode, cur_pass, context);

		table_close(childrel, NoLock);
	}

	ObjectAddressSubSet(address, RelationRelationId, myrelid, newattnum);
	return address;
}

/*
 * If a new or renamed column will collide with the name of an existing
 * column and if_not_exists is false then error out, else do nothing.
 *
 * 若新列或改名后的列会和已有列名冲突，且 if_not_exists 为假，则报错，否则什么也不做。
 */
static bool
check_for_column_name_collision(Relation rel, const char *colname,
								bool if_not_exists)
{
	HeapTuple	attTuple;
	int			attnum;

	/*
	 * this test is deliberately not attisdropped-aware, since if one tries to
	 * add a column matching a dropped column name, it's gonna fail anyway.
	 *
	 * 这个测试故意不理会 attisdropped，因为若试图加一列去匹配已删除列的名字，反正也会失败。
	 */
	attTuple = SearchSysCache2(ATTNAME,
							   ObjectIdGetDatum(RelationGetRelid(rel)),
							   PointerGetDatum(colname));
	if (!HeapTupleIsValid(attTuple))
		return true;

	attnum = ((Form_pg_attribute) GETSTRUCT(attTuple))->attnum;
	ReleaseSysCache(attTuple);

	/*
	 * We throw a different error message for conflicts with system column
	 * names, since they are normally not shown and the user might otherwise
	 * be confused about the reason for the conflict.
	 *
	 * 和系统列名冲突时我们报不同的错误信息，因为系统列通常不显示，否则用户可能搞不清冲突原因。
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column name \"%s\" conflicts with a system column name",
						colname)));
	else
	{
		if (if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" already exists, skipping",
							colname, RelationGetRelationName(rel))));
			return false;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" already exists",
						colname, RelationGetRelationName(rel))));
	}

	return true;
}

/*
 * Install a column's dependency on its datatype.
 *
 * 为列建立对其数据类型的依赖。
 */
static void
add_column_datatype_dependency(Oid relid, int32 attnum, Oid typid)
{
	ObjectAddress myself,
				referenced;

	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = attnum;
	referenced.classId = TypeRelationId;
	referenced.objectId = typid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * Install a column's dependency on its collation.
 *
 * 为列建立对其排序规则的依赖。
 */
static void
add_column_collation_dependency(Oid relid, int32 attnum, Oid collid)
{
	ObjectAddress myself,
				referenced;

	/* We know the default collation is pinned, so don't bother recording it */
	/*
	 *
	 * 已知默认排序规则是钉住的，不必记录
	 */
	if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
	{
		myself.classId = RelationRelationId;
		myself.objectId = relid;
		myself.objectSubId = attnum;
		referenced.classId = CollationRelationId;
		referenced.objectId = collid;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}

/*
 * ALTER TABLE ALTER COLUMN DROP NOT NULL
 *
 * ALTER TABLE ALTER COLUMN DROP NOT NULL（去掉列的非空约束）
 *
 * Return the address of the modified column.  If the column was already
 * nullable, InvalidObjectAddress is returned.
 *
 * 返回被修改列的地址。若该列已经可空，则返回 InvalidObjectAddress。
 */
static ObjectAddress
ATExecDropNotNull(Relation rel, const char *colName, bool recurse,
				  LOCKMODE lockmode)
{
	HeapTuple	tuple;
	HeapTuple	conTup;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attr_rel;
	ObjectAddress address;

	/*
	 * lookup the attribute
	 *
	 * 查找该属性
	 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	/* If the column is already nullable there's nothing to do. */
	/*
	 *
	 * 若该列已经可空，则无事可做。
	 */
	if (!attTup->attnotnull)
	{
		table_close(attr_rel, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/* Prevent them from altering a system attribute */
	/*
	 *
	 * 阻止他们修改系统属性
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colName, RelationGetRelationName(rel))));

	/*
	 * If rel is partition, shouldn't drop NOT NULL if parent has the same.
	 *
	 * 若 rel 是分区，父表有同样的 NOT NULL 时不应去掉。
	 */
	if (rel->rd_rel->relispartition)
	{
		Oid			parentId = get_partition_parent(RelationGetRelid(rel), false);
		Relation	parent = table_open(parentId, AccessShareLock);
		TupleDesc	tupDesc = RelationGetDescr(parent);
		AttrNumber	parent_attnum;

		parent_attnum = get_attnum(parentId, colName);
		if (TupleDescAttr(tupDesc, parent_attnum - 1)->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" is marked NOT NULL in parent table",
							colName)));
		table_close(parent, AccessShareLock);
	}

	/*
	 * Find the constraint that makes this column NOT NULL, and drop it.
	 * dropconstraint_internal() resets attnotnull.
	 *
	 * 找到使这一列为 NOT NULL 的约束并删掉。dropconstraint_internal() 会重置 attnotnull。
	 */
	conTup = findNotNullConstraintAttnum(RelationGetRelid(rel), attnum);
	if (conTup == NULL)
		elog(ERROR, "cache lookup failed for not-null constraint on column \"%s\" of relation \"%s\"",
			 colName, RelationGetRelationName(rel));

	/* The normal case: we have a pg_constraint row, remove it */
	/*
	 *
	 * 正常情况：有 pg_constraint 行，把它删掉
	 */
	dropconstraint_internal(rel, conTup, DROP_RESTRICT, recurse, false,
							false, lockmode);
	heap_freetuple(conTup);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	table_close(attr_rel, RowExclusiveLock);

	return address;
}

/*
 * set_attnotnull
 *		Helper to update/validate the pg_attribute status of a not-null
 *		constraint
 *
 * set_attnotnull：更新或验证 NOT NULL 约束对应的 pg_attribute 状态的辅助函数
 *
 * pg_attribute.attnotnull is set true, if it isn't already.
 * If queue_validation is true, also set up wqueue to validate the constraint.
 * wqueue may be given as NULL when validation is not needed (e.g., on table
 * creation).
 *
 * 若还没置上，就把 pg_attribute.attnotnull 设为真。若 queue_validation 为真，还要安排
 * wqueue 去验证该约束。不需要验证时（例如建表）可以把 wqueue 传成 NULL。
 */
static void
set_attnotnull(List **wqueue, Relation rel, AttrNumber attnum,
			   bool is_valid, bool queue_validation)
{
	Form_pg_attribute attr;
	CompactAttribute *thisatt;

	Assert(!queue_validation || wqueue);

	CheckAlterTableIsSafe(rel);

	/*
	 * Exit quickly by testing attnotnull from the tupledesc's copy of the
	 * attribute.
	 *
	 * 先看元组描述符副本里的 attnotnull，能早点退出就早点退出。
	 */
	attr = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	if (attr->attisdropped)
		return;

	if (!attr->attnotnull)
	{
		Relation	attr_rel;
		HeapTuple	tuple;

		attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

		tuple = SearchSysCacheCopyAttNum(RelationGetRelid(rel), attnum);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, RelationGetRelid(rel));

		thisatt = TupleDescCompactAttr(RelationGetDescr(rel), attnum - 1);
		thisatt->attnullability = ATTNULLABLE_VALID;

		attr = (Form_pg_attribute) GETSTRUCT(tuple);

		attr->attnotnull = true;
		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

		/*
		 * If the nullness isn't already proven by validated constraints, have
		 * ALTER TABLE phase 3 test for it.
		 *
		 * 若已验证的约束还没证明非空，就让 ALTER TABLE 的阶段 3 去测。
		 */
		if (queue_validation && wqueue &&
			!NotNullImpliedByRelConstraints(rel, attr))
		{
			AlteredTableInfo *tab;

			tab = ATGetQueueEntry(wqueue, rel);
			tab->verify_new_notnull = true;
		}

		CommandCounterIncrement();

		table_close(attr_rel, RowExclusiveLock);
		heap_freetuple(tuple);
	}
	else
	{
		CacheInvalidateRelcache(rel);
	}
}

/*
 * ALTER TABLE ALTER COLUMN SET NOT NULL
 *
 * ALTER TABLE ALTER COLUMN SET NOT NULL（把列设为非空）
 *
 * Add a not-null constraint to a single table and its children.  Returns
 * the address of the constraint added to the parent relation, if one gets
 * added, or InvalidObjectAddress otherwise.
 *
 * 给单张表及其子表加 NOT NULL 约束。若给父关系加上了约束，返回其地址，否则返回 InvalidObjectAddress。
 *
 * We must recurse to child tables during execution, rather than using
 * ALTER TABLE's normal prep-time recursion.
 *
 * 必须在执行期间递归到子表，而不能用 ALTER TABLE 通常在准备阶段做的递归。
 */
static ObjectAddress
ATExecSetNotNull(List **wqueue, Relation rel, char *conName, char *colName,
				 bool recurse, bool recursing, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	AttrNumber	attnum;
	ObjectAddress address;
	Constraint *constraint;
	CookedConstraint *ccon;
	List	   *cooked;
	bool		is_no_inherit = false;

	/* Guard against stack overflow due to overly deep inheritance tree. */
	/*
	 *
	 * 防止继承树过深导致栈溢出。
	 */
	check_stack_depth();

	/* At top level, permission check was done in ATPrepCmd, else do it */
	/*
	 *
	 * 顶层的权限检查已在 ATPrepCmd 做过，否则在这里做
	 */
	if (recursing)
	{
		ATSimplePermissions(AT_AddConstraint, rel,
							ATT_PARTITIONED_TABLE | ATT_TABLE | ATT_FOREIGN_TABLE);
		Assert(conName != NULL);
	}

	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	/*
	 *
	 * 阻止他们修改系统属性
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/* See if there's already a constraint */
	/*
	 *
	 * 看看是否已经有约束
	 */
	tuple = findNotNullConstraintAttnum(RelationGetRelid(rel), attnum);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_constraint conForm = (Form_pg_constraint) GETSTRUCT(tuple);
		bool		changed = false;

		/*
		 * Don't let a NO INHERIT constraint be changed into inherit.
		 *
		 * 不要把 NO INHERIT 约束改成可继承的。
		 */
		if (conForm->connoinherit && recurse)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot change NO INHERIT status of NOT NULL constraint \"%s\" on relation \"%s\"",
						   NameStr(conForm->conname),
						   RelationGetRelationName(rel)));

		/*
		 * If we find an appropriate constraint, we're almost done, but just
		 * need to change some properties on it: if we're recursing, increment
		 * coninhcount; if not, set conislocal if not already set.
		 *
		 * 若找到合适的约束，就快做完了，只需改它的一些属性：若正在递归，增加 coninhcount；否则若还没设 conislocal，
		 * 就设上。
		 */
		if (recursing)
		{
			if (pg_add_s16_overflow(conForm->coninhcount, 1,
									&conForm->coninhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
			changed = true;
		}
		else if (!conForm->conislocal)
		{
			conForm->conislocal = true;
			changed = true;
		}
		else if (!conForm->convalidated)
		{
			/*
			 * Flip attnotnull and convalidated, and also validate the
			 * constraint.
			 *
			 * 翻转 attnotnull 和 convalidated，并验证该约束。
			 */
			return ATExecValidateConstraint(wqueue, rel, NameStr(conForm->conname),
											recurse, recursing, lockmode);
		}

		if (changed)
		{
			Relation	constr_rel;

			constr_rel = table_open(ConstraintRelationId, RowExclusiveLock);

			CatalogTupleUpdate(constr_rel, &tuple->t_self, tuple);
			ObjectAddressSet(address, ConstraintRelationId, conForm->oid);
			table_close(constr_rel, RowExclusiveLock);
		}

		if (changed)
			return address;
		else
			return InvalidObjectAddress;
	}

	/*
	 * If we're asked not to recurse, and children exist, raise an error for
	 * partitioned tables.  For inheritance, we act as if NO INHERIT had been
	 * specified.
	 *
	 * 若要求不递归，且存在子表，对分区表报错。对普通继承，则当作指定了 NO INHERIT。
	 */
	if (!recurse &&
		find_inheritance_children(RelationGetRelid(rel),
								  NoLock) != NIL)
	{
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					errmsg("constraint must be added to child tables too"),
					errhint("Do not specify the ONLY keyword."));
		else
			is_no_inherit = true;
	}

	/*
	 * No constraint exists; we must add one.  First determine a name to use,
	 * if we haven't already.
	 *
	 * 还没有约束，必须加一个。若还没定名字，先定一个。
	 */
	if (!recursing)
	{
		Assert(conName == NULL);
		conName = ChooseConstraintName(RelationGetRelationName(rel),
									   colName, "not_null",
									   RelationGetNamespace(rel),
									   NIL);
	}

	constraint = makeNotNullConstraint(makeString(colName));
	constraint->is_no_inherit = is_no_inherit;
	constraint->conname = conName;

	/* and do it */
	/*
	 *
	 * 然后去做
	 */
	cooked = AddRelationNewConstraints(rel, NIL, list_make1(constraint),
									   false, !recursing, false, NULL);
	ccon = linitial(cooked);
	ObjectAddressSet(address, ConstraintRelationId, ccon->conoid);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	/* Mark pg_attribute.attnotnull for the column and queue validation */
	/*
	 *
	 * 给该列标上 pg_attribute.attnotnull，并排队验证
	 */
	set_attnotnull(wqueue, rel, attnum, true, true);

	/*
	 * Recurse to propagate the constraint to children that don't have one.
	 *
	 * 递归下去，把约束传播给还没有它的子表。
	 */
	if (recurse)
	{
		List	   *children;

		children = find_inheritance_children(RelationGetRelid(rel),
											 lockmode);

		foreach_oid(childoid, children)
		{
			Relation	childrel = table_open(childoid, NoLock);

			CommandCounterIncrement();

			ATExecSetNotNull(wqueue, childrel, conName, colName,
							 recurse, true, lockmode);
			table_close(childrel, NoLock);
		}
	}

	return address;
}

/*
 * NotNullImpliedByRelConstraints
 *		Does rel's existing constraints imply NOT NULL for the given attribute?
 *
 * NotNullImpliedByRelConstraints：关系上已有的约束是否蕴含给定属性为 NOT NULL？
 */
static bool
NotNullImpliedByRelConstraints(Relation rel, Form_pg_attribute attr)
{
	NullTest   *nnulltest = makeNode(NullTest);

	nnulltest->arg = (Expr *) makeVar(1,
									  attr->attnum,
									  attr->atttypid,
									  attr->atttypmod,
									  attr->attcollation,
									  0);
	nnulltest->nulltesttype = IS_NOT_NULL;

	/*
	 * argisrow = false is correct even for a composite column, because
	 * attnotnull does not represent a SQL-spec IS NOT NULL test in such a
	 * case, just IS DISTINCT FROM NULL.
	 *
	 * 即使是复合列，argisrow 为假也是对的，因为这种情况下 attnotnull 并不表示 SQL 标准的 IS NOT
	 * NULL 测试，只是 IS DISTINCT FROM NULL。
	 */
	nnulltest->argisrow = false;
	nnulltest->location = -1;

	if (ConstraintImpliedByRelConstraint(rel, list_make1(nnulltest), NIL))
	{
		ereport(DEBUG1,
				(errmsg_internal("existing constraints on column \"%s.%s\" are sufficient to prove that it does not contain nulls",
								 RelationGetRelationName(rel), NameStr(attr->attname))));
		return true;
	}

	return false;
}

/*
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT
 *
 * ALTER TABLE ALTER COLUMN SET/DROP DEFAULT（设置或去掉列默认值）
 *
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault, LOCKMODE lockmode)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	AttrNumber	attnum;
	ObjectAddress address;

	/*
	 * get the number of the attribute
	 *
	 * 取得该属性的编号
	 */
	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	/*
	 *
	 * 阻止他们修改系统属性
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (TupleDescAttr(tupdesc, attnum - 1)->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is an identity column",
						colName, RelationGetRelationName(rel)),
		/* translator: %s is an SQL ALTER command */
		/*
		 *
		 * 翻译提示：%s 是一条 SQL ALTER 命令
		 */
				 newDefault ? 0 : errhint("Use %s instead.",
										  "ALTER TABLE ... ALTER COLUMN ... DROP IDENTITY")));

	if (TupleDescAttr(tupdesc, attnum - 1)->attgenerated)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column \"%s\" of relation \"%s\" is a generated column",
						colName, RelationGetRelationName(rel)),
				 newDefault ?
		/* translator: %s is an SQL ALTER command */
		/*
		 *
		 * 翻译提示：%s 是一条 SQL ALTER 命令
		 */
				 errhint("Use %s instead.", "ALTER TABLE ... ALTER COLUMN ... SET EXPRESSION") :
				 (TupleDescAttr(tupdesc, attnum - 1)->attgenerated == ATTRIBUTE_GENERATED_STORED ?
				  errhint("Use %s instead.", "ALTER TABLE ... ALTER COLUMN ... DROP EXPRESSION") : 0)));

	/*
	 * Remove any old default for the column.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.
	 *
	 * 去掉该列的旧默认值。为安全起见这里用 RESTRICT，但目前不指望有东西依赖这个默认值。
	 *
	 * We treat removing the existing default as an internal operation when it
	 * is preparatory to adding a new default, but as a user-initiated
	 * operation when the user asked for a drop.
	 *
	 * 若去掉现有默认值是为了准备加新默认值，就把它当成内部操作；若用户明确要求去掉，就当成用户发起的操作。
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  newDefault != NULL);

	if (newDefault)
	{
		/* SET DEFAULT */
		/*
		 *
		 * SET DEFAULT（设置默认值）
		 */
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;
		rawEnt->generated = '\0';

		/*
		 * This function is intended for CREATE TABLE, so it processes a
		 * _list_ of defaults, but we just do one.
		 *
		 * 这个函数是给 CREATE TABLE 用的，所以它处理的是默认值的列表，而我们只做一项。
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * Add a pre-cooked default expression.
 *
 * 添加一个已经煮好的默认表达式。
 *
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
						  Node *newDefault)
{
	ObjectAddress address;

	/* We assume no checking is required */
	/*
	 *
	 * 假定不需要再检查
	 */

	/*
	 * Remove any old default for the column.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.  (In ordinary cases, there could not be a default in place
	 * anyway, but it's possible when combining LIKE with inheritance.)
	 *
	 * 去掉该列的旧默认值。为安全起见这里用 RESTRICT，但目前不指望有东西依赖这个默认值。（一般情况下本来也不会已有默认值，但
	 * LIKE 和继承组合时有可能。）
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  true);

	(void) StoreAttrDefault(rel, attnum, newDefault, true);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * ALTER TABLE ALTER COLUMN ADD IDENTITY
 *
 * ALTER TABLE ALTER COLUMN ADD IDENTITY（把列加为标识列）
 *
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecAddIdentity(Relation rel, const char *colName,
				  Node *def, LOCKMODE lockmode, bool recurse, bool recursing)
{
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	ObjectAddress address;
	ColumnDef  *cdef = castNode(ColumnDef, def);
	bool		ispartitioned;

	ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	if (ispartitioned && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot add identity to a column of only the partitioned table"),
				 errhint("Do not specify the ONLY keyword.")));

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				errmsg("cannot add identity to a column of a partition"));

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't alter a system attribute */
	/*
	 *
	 * 不能修改系统属性
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Creating a column as identity implies NOT NULL, so adding the identity
	 * to an existing column that is not NOT NULL would create a state that
	 * cannot be reproduced without contortions.
	 *
	 * 把列建成标识列意味着 NOT NULL，所以给一个还不是 NOT NULL 的已有列加标识，会产生一种不拐弯抹角就无法重现的状态。
	 */
	if (!attTup->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" must be declared NOT NULL before identity can be added",
						colName, RelationGetRelationName(rel))));

	/*
	 * On the other hand, if a not-null constraint exists, then verify that
	 * it's compatible.
	 *
	 * 另一方面，若已有 NOT NULL 约束，则验证它是否兼容。
	 */
	if (attTup->attnotnull)
	{
		HeapTuple	contup;
		Form_pg_constraint conForm;

		contup = findNotNullConstraintAttnum(RelationGetRelid(rel),
											 attnum);
		if (!HeapTupleIsValid(contup))
			elog(ERROR, "cache lookup failed for not-null constraint on column \"%s\" of relation \"%s\"",
				 colName, RelationGetRelationName(rel));

		conForm = (Form_pg_constraint) GETSTRUCT(contup);
		if (!conForm->convalidated)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("incompatible NOT VALID constraint \"%s\" on relation \"%s\"",
						   NameStr(conForm->conname), RelationGetRelationName(rel)),
					errhint("You might need to validate it using %s.",
							"ALTER TABLE ... VALIDATE CONSTRAINT"));
	}

	if (attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is already an identity column",
						colName, RelationGetRelationName(rel))));

	if (attTup->atthasdef)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" already has a default value",
						colName, RelationGetRelationName(rel))));

	attTup->attidentity = cdef->identity;
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/*
	 * Recurse to propagate the identity column to partitions.  Identity is
	 * not inherited in regular inheritance children.
	 *
	 * 递归下去，把标识列传播到分区。普通继承的子表不继承标识。
	 */
	if (recurse && ispartitioned)
	{
		List	   *children;
		ListCell   *lc;

		children = find_inheritance_children(RelationGetRelid(rel), lockmode);

		foreach(lc, children)
		{
			Relation	childrel;

			childrel = table_open(lfirst_oid(lc), NoLock);
			ATExecAddIdentity(childrel, colName, def, lockmode, recurse, true);
			table_close(childrel, NoLock);
		}
	}

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET { GENERATED or sequence options }
 *
 * ALTER TABLE ALTER COLUMN SET { GENERATED 或序列选项 }
 *
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecSetIdentity(Relation rel, const char *colName, Node *def,
				  LOCKMODE lockmode, bool recurse, bool recursing)
{
	ListCell   *option;
	DefElem    *generatedEl = NULL;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;
	bool		ispartitioned;

	ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	if (ispartitioned && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot change identity column of only the partitioned table"),
				 errhint("Do not specify the ONLY keyword.")));

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				errmsg("cannot change identity column of a partition"));

	foreach(option, castNode(List, def))
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "generated") == 0)
		{
			if (generatedEl)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			generatedEl = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * Even if there is nothing to change here, we run all the checks.  There
	 * will be a subsequent ALTER SEQUENCE that relies on everything being
	 * there.
	 *
	 * 即使这里没什么要改，也跑完全部检查。后面会有一条 ALTER SEQUENCE，它依赖这些东西都在。
	 */

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (!attTup->attidentity)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
						colName, RelationGetRelationName(rel))));

	if (generatedEl)
	{
		attTup->attidentity = defGetInt32(generatedEl);
		CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  attTup->attnum);
		ObjectAddressSubSet(address, RelationRelationId,
							RelationGetRelid(rel), attnum);
	}
	else
		address = InvalidObjectAddress;

	heap_freetuple(tuple);
	table_close(attrelation, RowExclusiveLock);

	/*
	 * Recurse to propagate the identity change to partitions. Identity is not
	 * inherited in regular inheritance children.
	 *
	 * 递归下去，把标识变更传播到分区。普通继承的子表不继承标识。
	 */
	if (generatedEl && recurse && ispartitioned)
	{
		List	   *children;
		ListCell   *lc;

		children = find_inheritance_children(RelationGetRelid(rel), lockmode);

		foreach(lc, children)
		{
			Relation	childrel;

			childrel = table_open(lfirst_oid(lc), NoLock);
			ATExecSetIdentity(childrel, colName, def, lockmode, recurse, true);
			table_close(childrel, NoLock);
		}
	}

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN DROP IDENTITY
 *
 * ALTER TABLE ALTER COLUMN DROP IDENTITY（去掉列的标识属性）
 *
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecDropIdentity(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode,
				   bool recurse, bool recursing)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	ObjectAddress address;
	Oid			seqid;
	ObjectAddress seqaddress;
	bool		ispartitioned;

	ispartitioned = (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
	if (ispartitioned && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop identity from a column of only the partitioned table"),
				 errhint("Do not specify the ONLY keyword.")));

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				errmsg("cannot drop identity from a column of a partition"));

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	if (!attTup->attidentity)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not an identity column",
							colName, RelationGetRelationName(rel))));
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" is not an identity column, skipping",
							colName, RelationGetRelationName(rel))));
			heap_freetuple(tuple);
			table_close(attrelation, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	attTup->attidentity = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attTup->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/*
	 * Recurse to drop the identity from column in partitions.  Identity is
	 * not inherited in regular inheritance children so ignore them.
	 *
	 * 递归下去，去掉分区中该列的标识。普通继承的子表不继承标识，忽略它们。
	 */
	if (recurse && ispartitioned)
	{
		List	   *children;
		ListCell   *lc;

		children = find_inheritance_children(RelationGetRelid(rel), lockmode);

		foreach(lc, children)
		{
			Relation	childrel;

			childrel = table_open(lfirst_oid(lc), NoLock);
			ATExecDropIdentity(childrel, colName, false, lockmode, recurse, true);
			table_close(childrel, NoLock);
		}
	}

	if (!recursing)
	{
		/* drop the internal sequence */
		/*
		 *
		 * 删掉内部序列
		 */
		seqid = getIdentitySequence(rel, attnum, false);
		deleteDependencyRecordsForClass(RelationRelationId, seqid,
										RelationRelationId, DEPENDENCY_INTERNAL);
		CommandCounterIncrement();
		seqaddress.classId = RelationRelationId;
		seqaddress.objectId = seqid;
		seqaddress.objectSubId = 0;
		performDeletion(&seqaddress, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
	}

	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET EXPRESSION
 *
 * ALTER TABLE ALTER COLUMN SET EXPRESSION（设置列的生成表达式）
 *
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecSetExpression(AlteredTableInfo *tab, Relation rel, const char *colName,
					Node *newExpr, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	char		attgenerated;
	bool		rewrite;
	Oid			attrdefoid;
	ObjectAddress address;
	Expr	   *defval;
	NewColumnValue *newval;
	RawColumnDefault *rawEnt;

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attTup->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	attgenerated = attTup->attgenerated;
	if (!attgenerated)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("column \"%s\" of relation \"%s\" is not a generated column",
						colName, RelationGetRelationName(rel))));

	/*
	 * TODO: This could be done, just need to recheck any constraints
	 * afterwards.
	 *
	 * TODO：这本来可以做，只是之后要重新检查所有约束。
	 */
	if (attgenerated == ATTRIBUTE_GENERATED_VIRTUAL &&
		rel->rd_att->constr && rel->rd_att->constr->num_check > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / SET EXPRESSION is not supported for virtual generated columns in tables with check constraints"),
				 errdetail("Column \"%s\" of relation \"%s\" is a virtual generated column.",
						   colName, RelationGetRelationName(rel))));

	if (attgenerated == ATTRIBUTE_GENERATED_VIRTUAL && attTup->attnotnull)
		tab->verify_new_notnull = true;

	/*
	 * We need to prevent this because a change of expression could affect a
	 * row filter and inject expressions that are not permitted in a row
	 * filter.  XXX We could try to have a more precise check to catch only
	 * publications with row filters, or even re-verify the row filter
	 * expressions.
	 *
	 * 必须阻止这样做，因为表达式变了可能影响行过滤器，并塞进行过滤器不允许的表达式。XXX 可以试着做更精确的检查，
	 * 只抓住带行过滤器的发布，甚至重新验证行过滤器表达式。
	 */
	if (attgenerated == ATTRIBUTE_GENERATED_VIRTUAL &&
		GetRelationPublications(RelationGetRelid(rel)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / SET EXPRESSION is not supported for virtual generated columns in tables that are part of a publication"),
				 errdetail("Column \"%s\" of relation \"%s\" is a virtual generated column.",
						   colName, RelationGetRelationName(rel))));

	rewrite = (attgenerated == ATTRIBUTE_GENERATED_STORED);

	ReleaseSysCache(tuple);

	if (rewrite)
	{
		/*
		 * Clear all the missing values if we're rewriting the table, since
		 * this renders them pointless.
		 *
		 * 若正在重写表，清掉所有缺失值，因为重写之后它们就没意义了。
		 */
		RelationClearMissing(rel);

		/* make sure we don't conflict with later attribute modifications */
		/*
		 *
		 * 确保不和后面的属性修改冲突
		 */
		CommandCounterIncrement();

		/*
		 * Find everything that depends on the column (constraints, indexes,
		 * etc), and record enough information to let us recreate the objects
		 * after rewrite.
		 *
		 * 找出所有依赖该列的东西（约束、索引等），并记下足够信息，以便重写后重建这些对象。
		 */
		RememberAllDependentForRebuilding(tab, AT_SetExpression, rel, attnum, colName);
	}

	/*
	 * Drop the dependency records of the GENERATED expression, in particular
	 * its INTERNAL dependency on the column, which would otherwise cause
	 * dependency.c to refuse to perform the deletion.
	 *
	 * 删掉 GENERATED 表达式的依赖记录，尤其是它对列的 INTERNAL 依赖，否则 dependency.c 会拒绝执行删除。
	 */
	attrdefoid = GetAttrDefaultOid(RelationGetRelid(rel), attnum);
	if (!OidIsValid(attrdefoid))
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 RelationGetRelid(rel), attnum);
	(void) deleteDependencyRecordsFor(AttrDefaultRelationId, attrdefoid, false);

	/* Make above changes visible */
	/*
	 *
	 * 让上面的修改可见
	 */
	CommandCounterIncrement();

	/*
	 * Get rid of the GENERATED expression itself.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * expression.
	 *
	 * 去掉 GENERATED 表达式本身。为安全起见这里用 RESTRICT，但目前不指望有东西依赖这个表达式。
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT,
					  false, false);

	/* Prepare to store the new expression, in the catalogs */
	/*
	 *
	 * 准备把新表达式存进系统目录
	 */
	rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
	rawEnt->attnum = attnum;
	rawEnt->raw_default = newExpr;
	rawEnt->generated = attgenerated;

	/* Store the generated expression */
	/*
	 *
	 * 存入生成表达式
	 */
	AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
							  false, true, false, NULL);

	/* Make above new expression visible */
	/*
	 *
	 * 让上面的新表达式可见
	 */
	CommandCounterIncrement();

	if (rewrite)
	{
		/* Prepare for table rewrite */
		/*
		 *
		 * 准备重写表
		 */
		defval = (Expr *) build_column_default(rel, attnum);

		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attnum;
		newval->expr = expression_planner(defval);
		newval->is_generated = true;

		tab->newvals = lappend(tab->newvals, newval);
		tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
	}

	/* Drop any pg_statistic entry for the column */
	/*
	 *
	 * 删掉该列的 pg_statistic 项
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * ALTER TABLE ALTER COLUMN DROP EXPRESSION
 *
 * ALTER TABLE ALTER COLUMN DROP EXPRESSION（去掉列的生成表达式）
 */
static void
ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode)
{
	/*
	 * Reject ONLY if there are child tables.  We could implement this, but it
	 * is a bit complicated.  GENERATED clauses must be attached to the column
	 * definition and cannot be added later like DEFAULT, so if a child table
	 * has a generation expression that the parent does not have, the child
	 * column will necessarily be an attislocal column.  So to implement ONLY
	 * here, we'd need extra code to update attislocal of the direct child
	 * tables, somewhat similar to how DROP COLUMN does it, so that the
	 * resulting state can be properly dumped and restored.
	 *
	 * 若有子表则拒绝 ONLY。这本来可以实现，但有点复杂。GENERATED 子句必须附在列定义上，不能像 DEFAULT
	 * 那样事后再加，所以若子表有而父表没有生成表达式，子列必然是 attislocal 列。因此要在这里实现 ONLY，
	 * 就得额外代码去更新直接子表的 attislocal，有点像 DROP COLUMN 那样，才能让结果状态被正确转储和恢复。
	 */
	if (!recurse &&
		find_inheritance_children(RelationGetRelid(rel), lockmode))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / DROP EXPRESSION must be applied to child tables too")));

	/*
	 * Cannot drop generation expression from inherited columns.
	 *
	 * 不能去掉继承列的生成表达式。
	 */
	if (!recursing)
	{
		HeapTuple	tuple;
		Form_pg_attribute attTup;

		tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), cmd->name);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							cmd->name, RelationGetRelationName(rel))));

		attTup = (Form_pg_attribute) GETSTRUCT(tuple);

		if (attTup->attinhcount > 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop generation expression from inherited column")));
	}
}

/*
 * Return the address of the affected column.
 *
 * 返回受影响列的地址。
 */
static ObjectAddress
ATExecDropExpression(Relation rel, const char *colName, bool missing_ok, LOCKMODE lockmode)
{
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Relation	attrelation;
	Oid			attrdefoid;
	ObjectAddress address;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * TODO: This could be done, but it would need a table rewrite to
	 * materialize the generated values.  Note that for the time being, we
	 * still error with missing_ok, so that we don't silently leave the column
	 * as generated.
	 *
	 * TODO：这本来可以做，但需要重写表来物化生成值。注意目前即使 missing_ok 我们仍报错，以免悄悄把列留成生成列。
	 */
	if (attTup->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / DROP EXPRESSION is not supported for virtual generated columns"),
				 errdetail("Column \"%s\" of relation \"%s\" is a virtual generated column.",
						   colName, RelationGetRelationName(rel))));

	if (!attTup->attgenerated)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("column \"%s\" of relation \"%s\" is not a generated column",
							colName, RelationGetRelationName(rel))));
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" is not a generated column, skipping",
							colName, RelationGetRelationName(rel))));
			heap_freetuple(tuple);
			table_close(attrelation, RowExclusiveLock);
			return InvalidObjectAddress;
		}
	}

	/*
	 * Mark the column as no longer generated.  (The atthasdef flag needs to
	 * get cleared too, but RemoveAttrDefault will handle that.)
	 *
	 * 把该列标成不再是生成列。（atthasdef 标志也要清掉，但 RemoveAttrDefault 会处理。）
	 */
	attTup->attgenerated = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/*
	 * Drop the dependency records of the GENERATED expression, in particular
	 * its INTERNAL dependency on the column, which would otherwise cause
	 * dependency.c to refuse to perform the deletion.
	 *
	 * 删掉 GENERATED 表达式的依赖记录，尤其是它对列的 INTERNAL 依赖，否则 dependency.c 会拒绝执行删除。
	 */
	attrdefoid = GetAttrDefaultOid(RelationGetRelid(rel), attnum);
	if (!OidIsValid(attrdefoid))
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 RelationGetRelid(rel), attnum);
	(void) deleteDependencyRecordsFor(AttrDefaultRelationId, attrdefoid, false);

	/* Make above changes visible */
	/*
	 *
	 * 让上面的修改可见
	 */
	CommandCounterIncrement();

	/*
	 * Get rid of the GENERATED expression itself.  We use RESTRICT here for
	 * safety, but at present we do not expect anything to depend on the
	 * default.
	 *
	 * 去掉 GENERATED 表达式本身。为安全起见这里用 RESTRICT，但目前不指望有东西依赖这个默认值。
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT,
					  false, false);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * ALTER TABLE ALTER COLUMN SET STATISTICS
 *
 * ALTER TABLE ALTER COLUMN SET STATISTICS（设置列的统计目标）
 *
 * Return value is the address of the modified column
 *
 * 返回值是被修改列的地址
 */
static ObjectAddress
ATExecSetStatistics(Relation rel, const char *colName, int16 colNum, Node *newValue, LOCKMODE lockmode)
{
	int			newtarget = 0;
	bool		newtarget_default;
	Relation	attrelation;
	HeapTuple	tuple,
				newtuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];

	/*
	 * We allow referencing columns by numbers only for indexes, since table
	 * column numbers could contain gaps if columns are later dropped.
	 *
	 * 只允许索引用列号引用列，因为表的列号在以后删列时可能出现空洞。
	 */
	if (rel->rd_rel->relkind != RELKIND_INDEX &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX &&
		!colName)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot refer to non-index column by number")));

	/* -1 was used in previous versions for the default setting */
	/*
	 *
	 * 以前的版本用 -1 表示默认设置
	 */
	if (newValue && intVal(newValue) != -1)
	{
		newtarget = intVal(newValue);
		newtarget_default = false;
	}
	else
		newtarget_default = true;

	if (!newtarget_default)
	{
		/*
		 * Limit target to a sane range
		 *
		 * 把目标限制在合理范围内
		 */
		if (newtarget < 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("statistics target %d is too low",
							newtarget)));
		}
		else if (newtarget > MAX_STATISTICS_TARGET)
		{
			newtarget = MAX_STATISTICS_TARGET;
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("lowering statistics target to %d",
							newtarget)));
		}
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	if (colName)
	{
		tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
	}
	else
	{
		tuple = SearchSysCacheAttNum(RelationGetRelid(rel), colNum);

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column number %d of relation \"%s\" does not exist",
							colNum, RelationGetRelationName(rel))));
	}

	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * Prevent this as long as the ANALYZE code skips virtual generated
	 * columns.
	 *
	 * 只要 ANALYZE 代码还跳过虚拟生成列，就阻止这样做。
	 */
	if (attrtuple->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter statistics on virtual generated column \"%s\"",
						colName)));

	if (rel->rd_rel->relkind == RELKIND_INDEX ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		if (attnum > rel->rd_index->indnkeyatts)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter statistics on included column \"%s\" of index \"%s\"",
							NameStr(attrtuple->attname), RelationGetRelationName(rel))));
		else if (rel->rd_index->indkey.values[attnum - 1] != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot alter statistics on non-expression column \"%s\" of index \"%s\"",
							NameStr(attrtuple->attname), RelationGetRelationName(rel)),
					 errhint("Alter statistics on table column instead.")));
	}

	/* Build new tuple. */
	/*
	 *
	 * 构造新元组。
	 */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (!newtarget_default)
		repl_val[Anum_pg_attribute_attstattarget - 1] = newtarget;
	else
		repl_null[Anum_pg_attribute_attstattarget - 1] = true;
	repl_repl[Anum_pg_attribute_attstattarget - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation),
								 repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(attrelation, &tuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * Return value is the address of the modified column
 *
 * 返回值是被修改列的地址
 */
static ObjectAddress
ATExecSetOptions(Relation rel, const char *colName, Node *options,
				 bool isReset, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple,
				newtuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	Datum		datum,
				newOptions;
	bool		isnull;
	ObjectAddress address;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/* Generate new proposed attoptions (text array) */
	/*
	 *
	 * 生成新提议的 attoptions（文本数组）
	 */
	datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
							&isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 castNode(List, options), NULL, NULL,
									 false, isReset);
	/* Validate new options */
	/*
	 *
	 * 校验新选项
	 */
	(void) attribute_reloptions(newOptions, true);

	/* Build new tuple. */
	/*
	 *
	 * 构造新元组。
	 */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_attribute_attoptions - 1] = true;
	repl_repl[Anum_pg_attribute_attoptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation),
								 repl_val, repl_null, repl_repl);

	/* Update system catalog. */
	/*
	 *
	 * 更新系统目录。
	 */
	CatalogTupleUpdate(attrelation, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	table_close(attrelation, RowExclusiveLock);

	return address;
}

/*
 * Helper function for ATExecSetStorage and ATExecSetCompression
 *
 * ATExecSetStorage 和 ATExecSetCompression 的辅助函数
 *
 * Set the attstorage and/or attcompression fields for index columns
 * associated with the specified table column.
 *
 * 为与指定表列关联的索引列设置 attstorage 和/或 attcompression 字段。
 */
static void
SetIndexStorageProperties(Relation rel, Relation attrelation,
						  AttrNumber attnum,
						  bool setstorage, char newstorage,
						  bool setcompression, char newcompression,
						  LOCKMODE lockmode)
{
	ListCell   *lc;

	foreach(lc, RelationGetIndexList(rel))
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	indrel;
		AttrNumber	indattnum = 0;
		HeapTuple	tuple;

		indrel = index_open(indexoid, lockmode);

		for (int i = 0; i < indrel->rd_index->indnatts; i++)
		{
			if (indrel->rd_index->indkey.values[i] == attnum)
			{
				indattnum = i + 1;
				break;
			}
		}

		if (indattnum == 0)
		{
			index_close(indrel, lockmode);
			continue;
		}

		tuple = SearchSysCacheCopyAttNum(RelationGetRelid(indrel), indattnum);

		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

			if (setstorage)
				attrtuple->attstorage = newstorage;

			if (setcompression)
				attrtuple->attcompression = newcompression;

			CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

			InvokeObjectPostAlterHook(RelationRelationId,
									  RelationGetRelid(rel),
									  attrtuple->attnum);

			heap_freetuple(tuple);
		}

		index_close(indrel, lockmode);
	}
}

/*
 * ALTER TABLE ALTER COLUMN SET STORAGE
 *
 * ALTER TABLE ALTER COLUMN SET STORAGE（设置列存储方式）
 *
 * Return value is the address of the modified column
 *
 * 返回值是被修改列的地址
 */
static ObjectAddress
ATExecSetStorage(Relation rel, const char *colName, Node *newValue, LOCKMODE lockmode)
{
	Relation	attrelation;
	HeapTuple	tuple;
	Form_pg_attribute attrtuple;
	AttrNumber	attnum;
	ObjectAddress address;

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = attrtuple->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	attrtuple->attstorage = GetAttributeStorage(attrtuple->atttypid, strVal(newValue));

	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attrtuple->attnum);

	/*
	 * Apply the change to indexes as well (only for simple index columns,
	 * matching behavior of index.c ConstructTupleDescriptor()).
	 *
	 * 把改动也应用到索引上（只针对简单索引列，与 index.c 的 ConstructTupleDescriptor() 行为一致）。
	 */
	SetIndexStorageProperties(rel, attrelation, attnum,
							  true, attrtuple->attstorage,
							  false, 0,
							  lockmode);

	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * ALTER TABLE DROP COLUMN
 *
 * ALTER TABLE DROP COLUMN（删列）
 *
 * DROP COLUMN cannot use the normal ALTER TABLE recursion mechanism,
 * because we have to decide at runtime whether to recurse or not depending
 * on whether attinhcount goes to zero or not.  (We can't check this in a
 * static pre-pass because it won't handle multiple inheritance situations
 * correctly.)
 *
 * DROP COLUMN 不能用普通的 ALTER TABLE 递归机制，因为必须在运行时根据 attinhcount
 * 是否降到零来决定是否递归。（不能在静态的预先扫描里检查，因为处理不好多重继承。）
 */
static void
ATPrepDropColumn(List **wqueue, Relation rel, bool recurse, bool recursing,
				 AlterTableCmd *cmd, LOCKMODE lockmode,
				 AlterTableUtilityContext *context)
{
	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop column from typed table")));

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);

	if (recurse)
		cmd->recurse = true;
}

/*
 * Drops column 'colName' from relation 'rel' and returns the address of the
 * dropped column.  The column is also dropped (or marked as no longer
 * inherited from relation) from the relation's inheritance children, if any.
 *
 * 从关系 rel 删掉列 colName，并返回被删列的地址。若有继承子表，该列也会从子表删掉（或标成不再从该关系继承）。
 *
 * In the recursive invocations for inheritance child relations, instead of
 * dropping the column directly (if to be dropped at all), its object address
 * is added to 'addrs', which must be non-NULL in such invocations.  All
 * columns are dropped at the same time after all the children have been
 * checked recursively.
 *
 * 对继承子关系的递归调用里，不直接删列（如果根本要删的话），而是把它的对象地址加进 addrs，这种调用里 addrs 必须非空。
 * 等所有子表都递归检查完，再一次删掉所有列。
 */
static ObjectAddress
ATExecDropColumn(List **wqueue, Relation rel, const char *colName,
				 DropBehavior behavior,
				 bool recurse, bool recursing,
				 bool missing_ok, LOCKMODE lockmode,
				 ObjectAddresses *addrs)
{
	HeapTuple	tuple;
	Form_pg_attribute targetatt;
	AttrNumber	attnum;
	List	   *children;
	ObjectAddress object;
	bool		is_expr;

	/* At top level, permission check was done in ATPrepCmd, else do it */
	/*
	 *
	 * 顶层的权限检查已在 ATPrepCmd 做过，否则在这里做
	 */
	if (recursing)
		ATSimplePermissions(AT_DropColumn, rel,
							ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* Initialize addrs on the first invocation */
	/*
	 *
	 * 第一次调用时初始化 addrs
	 */
	Assert(!recursing || addrs != NULL);

	/* since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 本函数会递归，有可能被逼到栈溢出
	 */
	check_stack_depth();

	if (!recursing)
		addrs = new_object_addresses();

	/*
	 * get the number of the attribute
	 *
	 * 取得该属性的编号
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							colName, RelationGetRelationName(rel))));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("column \"%s\" of relation \"%s\" does not exist, skipping",
							colName, RelationGetRelationName(rel))));
			return InvalidObjectAddress;
		}
	}
	targetatt = (Form_pg_attribute) GETSTRUCT(tuple);

	attnum = targetatt->attnum;

	/* Can't drop a system attribute */
	/*
	 *
	 * 不能删除系统属性
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop system column \"%s\"",
						colName)));

	/*
	 * Don't drop inherited columns, unless recursing (presumably from a drop
	 * of the parent column)
	 *
	 * 不要删除继承来的列，除非正在递归（想必是因为删了父列）
	 */
	if (targetatt->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited column \"%s\"",
						colName)));

	/*
	 * Don't drop columns used in the partition key, either.  (If we let this
	 * go through, the key column's dependencies would cause a cascaded drop
	 * of the whole table, which is surely not what the user expected.)
	 *
	 * 也不要删除分区键用到的列。（若放行，键列的依赖会导致整张表被级联删除，这肯定不是用户期望的。）
	 */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel))));

	ReleaseSysCache(tuple);

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 *
	 * 按需要传播到子表。和大多数其他 ALTER 例程不同，这里必须一层一层递归；不能用 find_all_inheritors
	 * 一次做完。
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	if (children)
	{
		Relation	attr_rel;
		ListCell   *child;

		/*
		 * In case of a partitioned table, the column must be dropped from the
		 * partitions as well.
		 *
		 * 若是分区表，该列也必须从分区里删掉。
		 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE && !recurse)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop column from only the partitioned table when partitions exist"),
					 errhint("Do not specify the ONLY keyword.")));

		attr_rel = table_open(AttributeRelationId, RowExclusiveLock);
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;
			Form_pg_attribute childatt;

			/* find_inheritance_children already got lock */
			/*
			 *
			 * find_inheritance_children 已经拿到锁
			 */
			childrel = table_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);

			tuple = SearchSysCacheCopyAttName(childrelid, colName);
			if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
							/*
							 *
							 * 不该发生
							 */
				elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
					 colName, childrelid);
			childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (childatt->attinhcount <= 0) /* shouldn't happen */
							/*
							 *
							 * 不该发生
							 */
				elog(ERROR, "relation %u has non-inherited attribute \"%s\"",
					 childrelid, colName);

			if (recurse)
			{
				/*
				 * If the child column has other definition sources, just
				 * decrement its inheritance count; if not, recurse to delete
				 * it.
				 *
				 * 若子列还有其他定义来源，只把它的继承计数减一；否则递归删除它。
				 */
				if (childatt->attinhcount == 1 && !childatt->attislocal)
				{
					/* Time to delete this child column, too */
					/*
					 *
					 * 也该删除这个子列了
					 */
					ATExecDropColumn(wqueue, childrel, colName,
									 behavior, true, true,
									 false, lockmode, addrs);
				}
				else
				{
					/* Child column must survive my deletion */
					/*
					 *
					 * 子列必须在我这次删除后仍然存在
					 */
					childatt->attinhcount--;

					CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

					/* Make update visible */
					/*
					 *
					 * 让更新可见
					 */
					CommandCounterIncrement();
				}
			}
			else
			{
				/*
				 * If we were told to drop ONLY in this table (no recursion),
				 * we need to mark the inheritors' attributes as locally
				 * defined rather than inherited.
				 *
				 * 若被告知只在这张表上删除（不递归），需要把继承者的属性标成局部定义，而不是继承来的。
				 */
				childatt->attinhcount--;
				childatt->attislocal = true;

				CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

				/* Make update visible */
				/*
				 *
				 * 让更新可见
				 */
				CommandCounterIncrement();
			}

			heap_freetuple(tuple);

			table_close(childrel, NoLock);
		}
		table_close(attr_rel, RowExclusiveLock);
	}

	/* Add object to delete */
	/*
	 *
	 * 把对象加入待删除列表
	 */
	object.classId = RelationRelationId;
	object.objectId = RelationGetRelid(rel);
	object.objectSubId = attnum;
	add_exact_object_address(&object, addrs);

	if (!recursing)
	{
		/* Recursion has ended, drop everything that was collected */
		/*
		 *
		 * 递归结束，删掉收集到的一切
		 */
		performMultipleDeletions(addrs, behavior, 0);
		free_object_addresses(addrs);
	}

	return object;
}

/*
 * Prepare to add a primary key on a table, by adding not-null constraints
 * on all columns.
 *
 * 准备在表上加主键：先给所有列加上 NOT NULL 约束。
 *
 * The not-null constraints for a primary key must cover the whole inheritance
 * hierarchy (failing to ensure that leads to funny corner cases).  For the
 * normal case where we're asked to recurse, this routine checks if the
 * not-null constraints exist already, and if not queues a requirement for
 * them to be created by phase 2.
 *
 * 主键的 NOT NULL 约束必须覆盖整个继承层次（做不到这一点会引出古怪的边角情况）。正常要求递归时，本例程检查 NOT
 * NULL 约束是否已存在，若没有就排队，让阶段 2 去创建。
 *
 * For the case where we're asked not to recurse, we verify that a not-null
 * constraint exists on each column of each (direct) child table, throwing an
 * error if not.  Not throwing an error would also work, because a not-null
 * constraint would be created anyway, but it'd cause a silent scan of the
 * child table to verify absence of nulls.  We prefer to let the user know so
 * that they can add the constraint manually without having to hold
 * AccessExclusiveLock while at it.
 *
 * 若要求不递归，我们验证每张（直接）子表的每一列上都有 NOT NULL 约束，没有就报错。不报错也能工作，因为反正会创建 NOT
 * NULL 约束，但那会导致悄悄扫描子表以确认没有 null。我们更希望让用户知道，这样他们可以手动加约束，而不必一直拿着
 * AccessExclusiveLock。
 *
 * However, it's also important that we do not acquire locks on children if
 * the not-null constraints already exist on the parent, to avoid risking
 * deadlocks during parallel pg_restore of PKs on partitioned tables.
 *
 * 同样重要的是，若父表上已经有 NOT NULL 约束，就不要去锁子表，以免并行 pg_restore 在分区表上建主键时有死锁风险。
 */
static void
ATPrepAddPrimaryKey(List **wqueue, Relation rel, AlterTableCmd *cmd,
					bool recurse, LOCKMODE lockmode,
					AlterTableUtilityContext *context)
{
	Constraint *pkconstr;
	List	   *children = NIL;
	bool		got_children = false;

	pkconstr = castNode(Constraint, cmd->def);
	if (pkconstr->contype != CONSTR_PRIMARY)
		return;

	/* Verify that columns are not-null, or request that they be made so */
	/*
	 *
	 * 验证列是非空的，或者要求把它们改成非空
	 */
	foreach_node(String, column, pkconstr->keys)
	{
		AlterTableCmd *newcmd;
		Constraint *nnconstr;
		HeapTuple	tuple;

		/*
		 * First check if a suitable constraint exists.  If it does, we don't
		 * need to request another one.  We do need to bail out if it's not
		 * valid, though.
		 *
		 * 先检查是否已有合适的约束。若有，就不必再要一个。但若它尚未验证，必须停下来。
		 */
		tuple = findNotNullConstraint(RelationGetRelid(rel), strVal(column));
		if (tuple != NULL)
		{
			verifyNotNullPKCompatible(tuple, strVal(column));

			/* All good with this one; don't request another */
			/*
			 *
			 * 这一列没问题；不要再要一个约束
			 */
			heap_freetuple(tuple);
			continue;
		}
		else if (!recurse)
		{
			/*
			 * No constraint on this column.  Asked not to recurse, we won't
			 * create one here, but verify that all children have one.
			 *
			 * 这一列上没有约束。既然要求不递归，这里就不创建，但要验证所有子表都有。
			 */
			if (!got_children)
			{
				children = find_inheritance_children(RelationGetRelid(rel),
													 lockmode);
				/* only search for children on the first time through */
				/*
				 *
				 * 只在第一次经过时搜索子表
				 */
				got_children = true;
			}

			foreach_oid(childrelid, children)
			{
				HeapTuple	tup;

				tup = findNotNullConstraint(childrelid, strVal(column));
				if (!tup)
					ereport(ERROR,
							errmsg("column \"%s\" of table \"%s\" is not marked NOT NULL",
								   strVal(column), get_rel_name(childrelid)));
				/* verify it's good enough */
				/*
				 *
				 * 验证它够用
				 */
				verifyNotNullPKCompatible(tup, strVal(column));
			}
		}

		/* This column is not already not-null, so add it to the queue */
		/*
		 *
		 * 这一列还不是非空的，于是把它加入队列
		 */
		nnconstr = makeNotNullConstraint(column);

		newcmd = makeNode(AlterTableCmd);
		newcmd->subtype = AT_AddConstraint;
		/* note we force recurse=true here; see above */
		/*
		 *
		 * 注意这里强制 recurse 为真；见上文
		 */
		newcmd->recurse = true;
		newcmd->def = (Node *) nnconstr;

		ATPrepCmd(wqueue, rel, newcmd, true, false, lockmode, context);
	}
}

/*
 * Verify whether the given not-null constraint is compatible with a
 * primary key.  If not, an error is thrown.
 *
 * 验证给定的 NOT NULL 约束是否与主键兼容。不兼容则报错。
 */
static void
verifyNotNullPKCompatible(HeapTuple tuple, const char *colname)
{
	Form_pg_constraint conForm = (Form_pg_constraint) GETSTRUCT(tuple);

	if (conForm->contype != CONSTRAINT_NOTNULL)
		elog(ERROR, "constraint %u is not a not-null constraint", conForm->oid);

	/* a NO INHERIT constraint is no good */
	/*
	 *
	 * NO INHERIT 约束不行
	 */
	if (conForm->connoinherit)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot create primary key on column \"%s\"", colname),
		/*- translator: fourth %s is a constraint characteristic such as NOT VALID */
		/*
		 *
		 * 翻译提示：第四个 %s 是约束特征，例如 NOT VALID
		 */
				errdetail("The constraint \"%s\" on column \"%s\" of table \"%s\", marked %s, is incompatible with a primary key.",
						  NameStr(conForm->conname), colname,
						  get_rel_name(conForm->conrelid), "NO INHERIT"),
				errhint("You might need to make the existing constraint inheritable using %s.",
						"ALTER TABLE ... ALTER CONSTRAINT ... INHERIT"));

	/* an unvalidated constraint is no good */
	/*
	 *
	 * 未验证的约束不行
	 */
	if (!conForm->convalidated)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot create primary key on column \"%s\"", colname),
		/*- translator: fourth %s is a constraint characteristic such as NOT VALID */
		/*
		 *
		 * 翻译提示：第四个 %s 是约束特征，例如 NOT VALID
		 */
				errdetail("The constraint \"%s\" on column \"%s\" of table \"%s\", marked %s, is incompatible with a primary key.",
						  NameStr(conForm->conname), colname,
						  get_rel_name(conForm->conrelid), "NOT VALID"),
				errhint("You might need to validate it using %s.",
						"ALTER TABLE ... VALIDATE CONSTRAINT"));
}

/*
 * ALTER TABLE ADD INDEX
 *
 * ALTER TABLE ADD INDEX（加索引）
 *
 * There is no such command in the grammar, but parse_utilcmd.c converts
 * UNIQUE and PRIMARY KEY constraints into AT_AddIndex subcommands.  This lets
 * us schedule creation of the index at the appropriate time during ALTER.
 *
 * 语法里没有这条命令，但 parse_utilcmd.c 会把 UNIQUE 和 PRIMARY KEY 约束转换成
 * AT_AddIndex 子命令。这样我们可以在 ALTER 期间的合适时刻安排创建索引。
 *
 * Return value is the address of the new index.
 *
 * 返回值是新索引的地址。
 */
static ObjectAddress
ATExecAddIndex(AlteredTableInfo *tab, Relation rel,
			   IndexStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	bool		check_rights;
	bool		skip_build;
	bool		quiet;
	ObjectAddress address;

	Assert(IsA(stmt, IndexStmt));
	Assert(!stmt->concurrent);

	/* The IndexStmt has already been through transformIndexStmt */
	/*
	 *
	 * IndexStmt 已经过 transformIndexStmt
	 */
	Assert(stmt->transformed);

	/* suppress schema rights check when rebuilding existing index */
	/*
	 *
	 * 重建已有索引时，跳过模式权限检查
	 */
	check_rights = !is_rebuild;
	/* skip index build if phase 3 will do it or we're reusing an old one */
	/*
	 *
	 * 若阶段 3 会建索引，或我们正在复用旧索引，则跳过索引构建
	 */
	skip_build = tab->rewrite > 0 || RelFileNumberIsValid(stmt->oldNumber);
	/* suppress notices when rebuilding existing index */
	/*
	 *
	 * 重建已有索引时不发通知
	 */
	quiet = is_rebuild;

	address = DefineIndex(RelationGetRelid(rel),
						  stmt,
						  InvalidOid,	/* no predefined OID */
								/*
								 *
								 * 没有预定义的 OID
								 */
						  InvalidOid,	/* no parent index */
								/*
								 *
								 * 没有父索引
								 */
						  InvalidOid,	/* no parent constraint */
								/*
								 *
								 * 没有父约束
								 */
						  -1,	/* total_parts unknown */
							/*
							 *
							 * total_parts 未知
							 */
						  true, /* is_alter_table */
							/*
							 *
							 * is_alter_table 标志
							 */
						  check_rights,
						  false,	/* check_not_in_use - we did it already */
								/*
								 *
								 * check_not_in_use：我们已经做过了
								 */
						  skip_build,
						  quiet);

	/*
	 * If TryReuseIndex() stashed a relfilenumber for us, we used it for the
	 * new index instead of building from scratch.  Restore associated fields.
	 * This may store InvalidSubTransactionId in both fields, in which case
	 * relcache.c will assume it can rebuild the relcache entry.  Hence, do
	 * this after the CCI that made catalog rows visible to any rebuild.  The
	 * DROP of the old edition of this index will have scheduled the storage
	 * for deletion at commit, so cancel that pending deletion.
	 *
	 * 若 TryReuseIndex() 给我们存过一个 relfilenumber，新索引就用它，而不是从头建。恢复相关字段。
	 * 这可能把两个字段都存成 InvalidSubTransactionId，此时 relcache.c 会认为可以重建
	 * relcache 项。因此要在使目录行对任何重建可见的 CCI 之后再做。这个索引旧版本的 DROP 已经安排在提交时删除存储，
	 * 所以取消那个待删除。
	 */
	if (RelFileNumberIsValid(stmt->oldNumber))
	{
		Relation	irel = index_open(address.objectId, NoLock);

		irel->rd_createSubid = stmt->oldCreateSubid;
		irel->rd_firstRelfilelocatorSubid = stmt->oldFirstRelfilelocatorSubid;
		RelationPreserveStorage(irel->rd_locator, true);
		index_close(irel, NoLock);
	}

	return address;
}

/*
 * ALTER TABLE ADD STATISTICS
 *
 * ALTER TABLE ADD STATISTICS（加扩展统计）
 *
 * This is no such command in the grammar, but we use this internally to add
 * AT_ReAddStatistics subcommands to rebuild extended statistics after a table
 * column type change.
 *
 * 语法里没有这条命令，但我们在内部用它来加 AT_ReAddStatistics 子命令，以便在表列类型改变后重建扩展统计。
 */
static ObjectAddress
ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
					CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	ObjectAddress address;

	Assert(IsA(stmt, CreateStatsStmt));

	/* The CreateStatsStmt has already been through transformStatsStmt */
	/*
	 *
	 * CreateStatsStmt 已经过 transformStatsStmt
	 */
	Assert(stmt->transformed);

	address = CreateStatistics(stmt, !is_rebuild);

	return address;
}

/*
 * ALTER TABLE ADD CONSTRAINT USING INDEX
 *
 * ALTER TABLE ADD CONSTRAINT USING INDEX（用已有索引加约束）
 *
 * Returns the address of the new constraint.
 *
 * 返回新约束的地址。
 */
static ObjectAddress
ATExecAddIndexConstraint(AlteredTableInfo *tab, Relation rel,
						 IndexStmt *stmt, LOCKMODE lockmode)
{
	Oid			index_oid = stmt->indexOid;
	Relation	indexRel;
	char	   *indexName;
	IndexInfo  *indexInfo;
	char	   *constraintName;
	char		constraintType;
	ObjectAddress address;
	bits16		flags;

	Assert(IsA(stmt, IndexStmt));
	Assert(OidIsValid(index_oid));
	Assert(stmt->isconstraint);

	/*
	 * Doing this on partitioned tables is not a simple feature to implement,
	 * so let's punt for now.
	 *
	 * 在分区表上做这件事不是个简单功能，暂时放弃。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX is not supported on partitioned tables")));

	indexRel = index_open(index_oid, AccessShareLock);

	indexName = pstrdup(RelationGetRelationName(indexRel));

	indexInfo = BuildIndexInfo(indexRel);

	/* this should have been checked at parse time */
	/*
	 *
	 * 这本应在解析时检查过
	 */
	if (!indexInfo->ii_Unique)
		elog(ERROR, "index \"%s\" is not unique", indexName);

	/*
	 * Determine name to assign to constraint.  We require a constraint to
	 * have the same name as the underlying index; therefore, use the index's
	 * existing name as the default constraint name, and if the user
	 * explicitly gives some other name for the constraint, rename the index
	 * to match.
	 *
	 * 确定要赋给约束的名字。我们要求约束与底层索引同名；因此默认用索引现有的名字作为约束名，若用户给约束显式指定了别的名字，
	 * 就把索引改成那个名字。
	 */
	constraintName = stmt->idxname;
	if (constraintName == NULL)
		constraintName = indexName;
	else if (strcmp(constraintName, indexName) != 0)
	{
		ereport(NOTICE,
				(errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX will rename index \"%s\" to \"%s\"",
						indexName, constraintName)));
		RenameRelationInternal(index_oid, constraintName, false, true);
	}

	/* Extra checks needed if making primary key */
	/*
	 *
	 * 若要建成主键，还需要额外检查
	 */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, true, stmt);

	/* Note we currently don't support EXCLUSION constraints here */
	/*
	 *
	 * 注意目前这里不支持 EXCLUSION 约束
	 */
	if (stmt->primary)
		constraintType = CONSTRAINT_PRIMARY;
	else
		constraintType = CONSTRAINT_UNIQUE;

	/* Create the catalog entries for the constraint */
	/*
	 *
	 * 为该约束创建目录项
	 */
	flags = INDEX_CONSTR_CREATE_UPDATE_INDEX |
		INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS |
		(stmt->initdeferred ? INDEX_CONSTR_CREATE_INIT_DEFERRED : 0) |
		(stmt->deferrable ? INDEX_CONSTR_CREATE_DEFERRABLE : 0) |
		(stmt->primary ? INDEX_CONSTR_CREATE_MARK_AS_PRIMARY : 0);

	address = index_constraint_create(rel,
									  index_oid,
									  InvalidOid,
									  indexInfo,
									  constraintName,
									  constraintType,
									  flags,
									  allowSystemTableMods,
									  false);	/* is_internal */
											/*
											 *
											 * is_internal 标志
											 */

	index_close(indexRel, NoLock);

	return address;
}

/*
 * ALTER TABLE ADD CONSTRAINT
 *
 * ALTER TABLE ADD CONSTRAINT（加约束）
 *
 * Return value is the address of the new constraint; if no constraint was
 * added, InvalidObjectAddress is returned.
 *
 * 返回值是新约束的地址；若没有加上约束，返回 InvalidObjectAddress。
 */
static ObjectAddress
ATExecAddConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					Constraint *newConstraint, bool recurse, bool is_readd,
					LOCKMODE lockmode)
{
	ObjectAddress address = InvalidObjectAddress;

	Assert(IsA(newConstraint, Constraint));

	/*
	 * Currently, we only expect to see CONSTR_CHECK, CONSTR_NOTNULL and
	 * CONSTR_FOREIGN nodes arriving here (see the preprocessing done in
	 * parse_utilcmd.c).
	 *
	 * 目前只指望看到 CONSTR_CHECK、CONSTR_NOTNULL 和 CONSTR_FOREIGN 节点到达这里（见
	 * parse_utilcmd.c 里的预处理）。
	 */
	switch (newConstraint->contype)
	{
		case CONSTR_CHECK:
		case CONSTR_NOTNULL:
			address =
				ATAddCheckNNConstraint(wqueue, tab, rel,
									   newConstraint, recurse, false, is_readd,
									   lockmode);
			break;

		case CONSTR_FOREIGN:

			/*
			 * Assign or validate constraint name
			 *
			 * 指定或校验约束名
			 */
			if (newConstraint->conname)
			{
				if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
										 RelationGetRelid(rel),
										 newConstraint->conname))
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("constraint \"%s\" for relation \"%s\" already exists",
									newConstraint->conname,
									RelationGetRelationName(rel))));
			}
			else
				newConstraint->conname =
					ChooseConstraintName(RelationGetRelationName(rel),
										 ChooseForeignKeyConstraintNameAddition(newConstraint->fk_attrs),
										 "fkey",
										 RelationGetNamespace(rel),
										 NIL);

			address = ATAddForeignKeyConstraint(wqueue, tab, rel,
												newConstraint,
												recurse, false,
												lockmode);
			break;

		default:
			elog(ERROR, "unrecognized constraint type: %d",
				 (int) newConstraint->contype);
	}

	return address;
}

/*
 * Generate the column-name portion of the constraint name for a new foreign
 * key given the list of column names that reference the referenced
 * table.  This will be passed to ChooseConstraintName along with the parent
 * table name and the "fkey" suffix.
 *
 * 为新外键生成约束名里的列名部分，依据引用被引用表的列名列表。它会连同父表名和 fkey 后缀一起传给
 * ChooseConstraintName。
 *
 * We know that less than NAMEDATALEN characters will actually be used, so we
 * can truncate the result once we've generated that many.
 *
 * 我们知道实际用到的字符少于 NAMEDATALEN，所以生成到这个长度就可以截断。
 *
 * XXX see also ChooseExtendedStatisticNameAddition and
 * ChooseIndexNameAddition.
 *
 * XXX 另见 ChooseExtendedStatisticNameAddition 和
 * ChooseIndexNameAddition。
 */
static char *
ChooseForeignKeyConstraintNameAddition(List *colnames)
{
	char		buf[NAMEDATALEN * 2];
	int			buflen = 0;
	ListCell   *lc;

	buf[0] = '\0';
	foreach(lc, colnames)
	{
		const char *name = strVal(lfirst(lc));

		if (buflen > 0)
			buf[buflen++] = '_';	/* insert _ between names */
						/*
						 *
						 * 在名字之间插入下划线
						 */

		/*
		 * At this point we have buflen <= NAMEDATALEN.  name should be less
		 * than NAMEDATALEN already, but use strlcpy for paranoia.
		 *
		 * 此时 buflen 小于等于 NAMEDATALEN。name 应该已经短于 NAMEDATALEN，但为了保险用 strlcpy。
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * Add a check or not-null constraint to a single table and its children.
 * Returns the address of the constraint added to the parent relation,
 * if one gets added, or InvalidObjectAddress otherwise.
 *
 * 给单张表及其子表加 CHECK 或 NOT NULL 约束。若给父关系加上了约束，返回其地址，否则返回
 * InvalidObjectAddress。
 *
 * Subroutine for ATExecAddConstraint.
 *
 * ATExecAddConstraint 的子程序。
 *
 * We must recurse to child tables during execution, rather than using
 * ALTER TABLE's normal prep-time recursion.  The reason is that all the
 * constraints *must* be given the same name, else they won't be seen as
 * related later.  If the user didn't explicitly specify a name, then
 * AddRelationNewConstraints would normally assign different names to the
 * child constraints.  To fix that, we must capture the name assigned at
 * the parent table and pass that down.
 *
 * 必须在执行期间递归到子表，而不能用 ALTER TABLE 通常在准备阶段做的递归。原因是所有约束必须用同一个名字，
 * 否则以后不会被看成相关的。若用户没有显式指定名字，AddRelationNewConstraints 通常会给子约束分配不同的名字。
 * 为了修正这一点，必须抓住父表上分配的名字并传下去。
 */
static ObjectAddress
ATAddCheckNNConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					   Constraint *constr, bool recurse, bool recursing,
					   bool is_readd, LOCKMODE lockmode)
{
	List	   *newcons;
	ListCell   *lcon;
	List	   *children;
	ListCell   *child;
	ObjectAddress address = InvalidObjectAddress;

	/* Guard against stack overflow due to overly deep inheritance tree. */
	/*
	 *
	 * 防止继承树过深导致栈溢出。
	 */
	check_stack_depth();

	/* At top level, permission check was done in ATPrepCmd, else do it */
	/*
	 *
	 * 顶层的权限检查已在 ATPrepCmd 做过，否则在这里做
	 */
	if (recursing)
		ATSimplePermissions(AT_AddConstraint, rel,
							ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/*
	 * Call AddRelationNewConstraints to do the work, making sure it works on
	 * a copy of the Constraint so transformExpr can't modify the original. It
	 * returns a list of cooked constraints.
	 *
	 * 调用 AddRelationNewConstraints 来干活，确保它处理的是 Constraint 的副本，免得
	 * transformExpr 改掉原来的。它返回已煮好的约束列表。
	 *
	 * If the constraint ends up getting merged with a pre-existing one, it's
	 * omitted from the returned list, which is what we want: we do not need
	 * to do any validation work.  That can only happen at child tables,
	 * though, since we disallow merging at the top level.
	 *
	 * 若约束最终和已有约束合并，它会从返回列表里省略，这正是我们要的：不必做任何验证工作。不过这只能发生在子表上，因为顶层不允许合并。
	 */
	newcons = AddRelationNewConstraints(rel, NIL,
										list_make1(copyObject(constr)),
										recursing || is_readd,	/* allow_merge */
													/*
													 *
													 * allow_merge 标志
													 */
										!recursing, /* is_local */
											    /*
											     *
											     * is_local 标志
											     */
										is_readd,	/* is_internal */
												/*
												 *
												 * is_internal 标志
												 */
										NULL);	/* queryString not available
												 * here */
											/*
											 *
											 * 这里拿不到 queryString
											 */

	/* we don't expect more than one constraint here */
	/*
	 *
	 * 这里不指望有多于一个约束
	 */
	Assert(list_length(newcons) <= 1);

	/* Add each to-be-validated constraint to Phase 3's queue */
	/*
	 *
	 * 把每个待验证的约束加入 Phase 3 的队列
	 */
	foreach(lcon, newcons)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lcon);

		if (!ccon->skip_validation && ccon->contype != CONSTR_NOTNULL)
		{
			NewConstraint *newcon;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = ccon->name;
			newcon->contype = ccon->contype;
			newcon->qual = ccon->expr;

			tab->constraints = lappend(tab->constraints, newcon);
		}

		/* Save the actually assigned name if it was defaulted */
		/*
		 *
		 * 若名字是默认生成的，保存实际分配的名字
		 */
		if (constr->conname == NULL)
			constr->conname = ccon->name;

		/*
		 * If adding a valid not-null constraint, set the pg_attribute flag
		 * and tell phase 3 to verify existing rows, if needed.  For an
		 * invalid constraint, just set attnotnull, without queueing
		 * verification.
		 *
		 * 若加上的是有效的 NOT NULL 约束，设置 pg_attribute 标志，并在需要时告诉阶段 3 验证现有行。对无效约束，
		 * 只设置 attnotnull，不排队验证。
		 */
		if (constr->contype == CONSTR_NOTNULL)
			set_attnotnull(wqueue, rel, ccon->attnum,
						   !constr->skip_validation,
						   !constr->skip_validation);

		ObjectAddressSet(address, ConstraintRelationId, ccon->conoid);
	}

	/* At this point we must have a locked-down name to use */
	/*
	 *
	 * 到这里必须有一个敲定的名字可用
	 */
	Assert(newcons == NIL || constr->conname != NULL);

	/* Advance command counter in case same table is visited multiple times */
	/*
	 *
	 * 推进命令计数器，以防同一张表被访问多次
	 */
	CommandCounterIncrement();

	/*
	 * If the constraint got merged with an existing constraint, we're done.
	 * We mustn't recurse to child tables in this case, because they've
	 * already got the constraint, and visiting them again would lead to an
	 * incorrect value for coninhcount.
	 *
	 * 若约束和已有约束合并了，就完成了。这种情况下绝不能再递归到子表，因为它们已经有该约束，再访问会导致 coninhcount 不正确。
	 */
	if (newcons == NIL)
		return address;

	/*
	 * If adding a NO INHERIT constraint, no need to find our children.
	 *
	 * 若加的是 NO INHERIT 约束，不必找子表。
	 */
	if (constr->is_no_inherit)
		return address;

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 *
	 * 按需要传播到子表。和大多数其他 ALTER 例程不同，这里必须一层一层递归；不能用 find_all_inheritors
	 * 一次做完。
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * Check if ONLY was specified with ALTER TABLE.  If so, allow the
	 * constraint creation only if there are no children currently. Error out
	 * otherwise.
	 *
	 * 检查 ALTER TABLE 是否指定了 ONLY。若是，只有当前没有子表才允许创建约束，否则报错。
	 */
	if (!recurse && children != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("constraint must be added to child tables too")));

	/*
	 * Recurse to create the constraint on each child.
	 *
	 * 递归到每个子表上创建该约束。
	 */
	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/* find_inheritance_children already got lock */
		/*
		 *
		 * find_inheritance_children 已经拿到锁
		 */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/* Find or create work queue entry for this table */
		/*
		 *
		 * 查找或创建这张表的工作队列项
		 */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* Recurse to this child */
		/*
		 *
		 * 递归到这个子表
		 */
		ATAddCheckNNConstraint(wqueue, childtab, childrel,
							   constr, recurse, true, is_readd, lockmode);

		table_close(childrel, NoLock);
	}

	return address;
}

/*
 * Add a foreign-key constraint to a single table; return the new constraint's
 * address.
 *
 * 给单张表加外键约束；返回新约束的地址。
 *
 * Subroutine for ATExecAddConstraint.  Must already hold exclusive
 * lock on the rel, and have done appropriate validity checks for it.
 * We do permissions checks here, however.
 *
 * ATExecAddConstraint 的子程序。必须已经对 rel 持有排他锁，并做过适当的有效性检查。不过权限检查在这里做。
 *
 * When the referenced or referencing tables (or both) are partitioned,
 * multiple pg_constraint rows are required -- one for each partitioned table
 * and each partition on each side (fortunately, not one for every combination
 * thereof).  We also need action triggers on each leaf partition on the
 * referenced side, and check triggers on each leaf partition on the
 * referencing side.
 *
 * 当被引用表或引用表（或两者）是分区的时，需要多行 pg_constraint：
 * 每一边的每个分区表和每个分区各一行（好在不是它们的每一种组合都要一行）。被引用侧的每个叶子分区还需要动作触发器，
 * 引用侧的每个叶子分区需要检查触发器。
 */
static ObjectAddress
ATAddForeignKeyConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
						  Constraint *fkconstraint,
						  bool recurse, bool recursing, LOCKMODE lockmode)
{
	Relation	pkrel;
	int16		pkattnum[INDEX_MAX_KEYS] = {0};
	int16		fkattnum[INDEX_MAX_KEYS] = {0};
	Oid			pktypoid[INDEX_MAX_KEYS] = {0};
	Oid			fktypoid[INDEX_MAX_KEYS] = {0};
	Oid			pkcolloid[INDEX_MAX_KEYS] = {0};
	Oid			fkcolloid[INDEX_MAX_KEYS] = {0};
	Oid			opclasses[INDEX_MAX_KEYS] = {0};
	Oid			pfeqoperators[INDEX_MAX_KEYS] = {0};
	Oid			ppeqoperators[INDEX_MAX_KEYS] = {0};
	Oid			ffeqoperators[INDEX_MAX_KEYS] = {0};
	int16		fkdelsetcols[INDEX_MAX_KEYS] = {0};
	bool		with_period;
	bool		pk_has_without_overlaps;
	int			i;
	int			numfks,
				numpks,
				numfkdelsetcols;
	Oid			indexOid;
	bool		old_check_ok;
	ObjectAddress address;
	ListCell   *old_pfeqop_item = list_head(fkconstraint->old_conpfeqop);

	/*
	 * Grab ShareRowExclusiveLock on the pk table, so that someone doesn't
	 * delete rows out from under us.
	 *
	 * 对主键表加 ShareRowExclusiveLock，免得有人在我们眼皮底下删行。
	 */
	if (OidIsValid(fkconstraint->old_pktable_oid))
		pkrel = table_open(fkconstraint->old_pktable_oid, ShareRowExclusiveLock);
	else
		pkrel = table_openrv(fkconstraint->pktable, ShareRowExclusiveLock);

	/*
	 * Validity checks (permission checks wait till we have the column
	 * numbers)
	 *
	 * 有效性检查（权限检查等到有了列号再做）
	 */
	if (!recurse && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot use ONLY for foreign key on partitioned table \"%s\" referencing relation \"%s\"",
					   RelationGetRelationName(rel),
					   RelationGetRelationName(pkrel)));

	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	if (!allowSystemTableMods && IsSystemRelation(pkrel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(pkrel))));

	/*
	 * References from permanent or unlogged tables to temp tables, and from
	 * permanent tables to unlogged tables, are disallowed because the
	 * referenced data can vanish out from under us.  References from temp
	 * tables to any other table type are also disallowed, because other
	 * backends might need to run the RI triggers on the perm table, but they
	 * can't reliably see tuples in the local buffers of other backends.
	 *
	 * 不允许永久表或 unlogged 表引用临时表，也不允许永久表引用 unlogged 表，因为被引用的数据可能从我们眼皮底下消失。
	 * 也不允许临时表引用任何其他类型的表，因为其他后端可能需要在永久表上跑 RI 触发器，但它们无法可靠地看见其他后端本地缓冲区里的元组。
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_PERMANENT:
			if (!RelationIsPermanent(pkrel))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on permanent tables may reference only permanent tables")));
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!RelationIsPermanent(pkrel)
				&& pkrel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on unlogged tables may reference only permanent or unlogged tables")));
			break;
		case RELPERSISTENCE_TEMP:
			if (pkrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables may reference only temporary tables")));
			if (!pkrel->rd_islocaltemp || !rel->rd_islocaltemp)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("constraints on temporary tables must involve temporary tables of this session")));
			break;
	}

	/*
	 * Look up the referencing attributes to make sure they exist, and record
	 * their attnums and type and collation OIDs.
	 *
	 * 查找引用属性，确认它们存在，并记下它们的 attnum、类型 OID 和排序规则 OID。
	 */
	numfks = transformColumnNameList(RelationGetRelid(rel),
									 fkconstraint->fk_attrs,
									 fkattnum, fktypoid, fkcolloid);
	with_period = fkconstraint->fk_with_period || fkconstraint->pk_with_period;
	if (with_period && !fkconstraint->fk_with_period)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_FOREIGN_KEY),
				errmsg("foreign key uses PERIOD on the referenced table but not the referencing table"));

	numfkdelsetcols = transformColumnNameList(RelationGetRelid(rel),
											  fkconstraint->fk_del_set_cols,
											  fkdelsetcols, NULL, NULL);
	numfkdelsetcols = validateFkOnDeleteSetColumns(numfks, fkattnum,
												   numfkdelsetcols,
												   fkdelsetcols,
												   fkconstraint->fk_del_set_cols);

	/*
	 * If the attribute list for the referenced table was omitted, lookup the
	 * definition of the primary key and use it.  Otherwise, validate the
	 * supplied attribute list.  In either case, discover the index OID and
	 * index opclasses, and the attnums and type and collation OIDs of the
	 * attributes.
	 *
	 * 若省略了被引用表的属性列表，就查找主键定义并使用它。否则校验给出的属性列表。两种情况下都找出索引 OID、索引操作符类，以及属性的
	 * attnum、类型 OID 和排序规则 OID。
	 */
	if (fkconstraint->pk_attrs == NIL)
	{
		numpks = transformFkeyGetPrimaryKey(pkrel, &indexOid,
											&fkconstraint->pk_attrs,
											pkattnum, pktypoid, pkcolloid,
											opclasses, &pk_has_without_overlaps);

		/* If the primary key uses WITHOUT OVERLAPS, the fk must use PERIOD */
		/*
		 *
		 * 若主键使用 WITHOUT OVERLAPS，外键必须使用 PERIOD
		 */
		if (pk_has_without_overlaps && !fkconstraint->fk_with_period)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_FOREIGN_KEY),
					errmsg("foreign key uses PERIOD on the referenced table but not the referencing table"));
	}
	else
	{
		numpks = transformColumnNameList(RelationGetRelid(pkrel),
										 fkconstraint->pk_attrs,
										 pkattnum, pktypoid, pkcolloid);

		/* Since we got pk_attrs, one should be a period. */
		/*
		 *
		 * 既然拿到了 pk_attrs，其中应有一个是 period。
		 */
		if (with_period && !fkconstraint->pk_with_period)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_FOREIGN_KEY),
					errmsg("foreign key uses PERIOD on the referencing table but not the referenced table"));

		/* Look for an index matching the column list */
		/*
		 *
		 * 寻找与列清单匹配的索引
		 */
		indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum,
										   with_period, opclasses, &pk_has_without_overlaps);
	}

	/*
	 * If the referenced primary key has WITHOUT OVERLAPS, the foreign key
	 * must use PERIOD.
	 *
	 * 若被引用的主键有 WITHOUT OVERLAPS，外键必须使用 PERIOD。
	 */
	if (pk_has_without_overlaps && !with_period)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_FOREIGN_KEY),
				errmsg("foreign key must use PERIOD when referencing a primary key using WITHOUT OVERLAPS"));

	/*
	 * Now we can check permissions.
	 *
	 * 现在可以检查权限了。
	 */
	checkFkeyPermissions(pkrel, pkattnum, numpks);

	/*
	 * Check some things for generated columns.
	 *
	 * 检查生成列的一些事项。
	 */
	for (i = 0; i < numfks; i++)
	{
		char		attgenerated = TupleDescAttr(RelationGetDescr(rel), fkattnum[i] - 1)->attgenerated;

		if (attgenerated)
		{
			/*
			 * Check restrictions on UPDATE/DELETE actions, per SQL standard
			 *
			 * 按 SQL 标准检查 UPDATE/DELETE 动作的限制
			 */
			if (fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETNULL ||
				fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETDEFAULT ||
				fkconstraint->fk_upd_action == FKCONSTR_ACTION_CASCADE)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s action for foreign key constraint containing generated column",
								"ON UPDATE")));
			if (fkconstraint->fk_del_action == FKCONSTR_ACTION_SETNULL ||
				fkconstraint->fk_del_action == FKCONSTR_ACTION_SETDEFAULT)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid %s action for foreign key constraint containing generated column",
								"ON DELETE")));
		}

		/*
		 * FKs on virtual columns are not supported.  This would require
		 * various additional support in ri_triggers.c, including special
		 * handling in ri_NullCheck(), ri_KeysEqual(),
		 * RI_FKey_fk_upd_check_required() (since all virtual columns appear
		 * as NULL there).  Also not really practical as long as you can't
		 * index virtual columns.
		 *
		 * 不支持虚拟列上的外键。这需要 ri_triggers.c 里各种额外支持，包括 ri_NullCheck()、
		 * ri_KeysEqual()、RI_FKey_fk_upd_check_required() 的特殊处理（那里所有虚拟列都显示为
		 * NULL）。而且只要还不能给虚拟列建索引，这也不太现实。
		 */
		if (attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key constraints on virtual generated columns are not supported")));
	}

	/*
	 * Some actions are currently unsupported for foreign keys using PERIOD.
	 *
	 * 使用 PERIOD 的外键目前不支持某些动作。
	 */
	if (fkconstraint->fk_with_period)
	{
		if (fkconstraint->fk_upd_action == FKCONSTR_ACTION_RESTRICT ||
			fkconstraint->fk_upd_action == FKCONSTR_ACTION_CASCADE ||
			fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETNULL ||
			fkconstraint->fk_upd_action == FKCONSTR_ACTION_SETDEFAULT)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("unsupported %s action for foreign key constraint using PERIOD",
						   "ON UPDATE"));

		if (fkconstraint->fk_del_action == FKCONSTR_ACTION_RESTRICT ||
			fkconstraint->fk_del_action == FKCONSTR_ACTION_CASCADE ||
			fkconstraint->fk_del_action == FKCONSTR_ACTION_SETNULL ||
			fkconstraint->fk_del_action == FKCONSTR_ACTION_SETDEFAULT)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("unsupported %s action for foreign key constraint using PERIOD",
						   "ON DELETE"));
	}

	/*
	 * Look up the equality operators to use in the constraint.
	 *
	 * 查找约束要用的相等操作符。
	 *
	 * Note that we have to be careful about the difference between the actual
	 * PK column type and the opclass' declared input type, which might be
	 * only binary-compatible with it.  The declared opcintype is the right
	 * thing to probe pg_amop with.
	 *
	 * 注意必须小心实际主键列类型和操作符类声明的输入类型之间的差别，后者可能只是与它二进制兼容。用声明的 opcintype 去查
	 * pg_amop 才是对的。
	 */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	/*
	 * On the strength of a previous constraint, we might avoid scanning
	 * tables to validate this one.  See below.
	 *
	 * 凭借先前的约束，也许可以避免扫描表来验证这一条。见下文。
	 */
	old_check_ok = (fkconstraint->old_conpfeqop != NIL);
	Assert(!old_check_ok || numfks == list_length(fkconstraint->old_conpfeqop));

	for (i = 0; i < numpks; i++)
	{
		Oid			pktype = pktypoid[i];
		Oid			fktype = fktypoid[i];
		Oid			fktyped;
		Oid			pkcoll = pkcolloid[i];
		Oid			fkcoll = fkcolloid[i];
		HeapTuple	cla_ht;
		Form_pg_opclass cla_tup;
		Oid			amid;
		Oid			opfamily;
		Oid			opcintype;
		bool		for_overlaps;
		CompareType cmptype;
		Oid			pfeqop;
		Oid			ppeqop;
		Oid			ffeqop;
		int16		eqstrategy;
		Oid			pfeqop_right;

		/* We need several fields out of the pg_opclass entry */
		/*
		 *
		 * 需要 pg_opclass 项里的几个字段
		 */
		cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclasses[i]));
		if (!HeapTupleIsValid(cla_ht))
			elog(ERROR, "cache lookup failed for opclass %u", opclasses[i]);
		cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
		amid = cla_tup->opcmethod;
		opfamily = cla_tup->opcfamily;
		opcintype = cla_tup->opcintype;
		ReleaseSysCache(cla_ht);

		/*
		 * Get strategy number from index AM.
		 *
		 * 从索引访问方法取得策略号。
		 *
		 * For a normal foreign-key constraint, this should not fail, since we
		 * already checked that the index is unique and should therefore have
		 * appropriate equal operators.  For a period foreign key, this could
		 * fail if we selected a non-matching exclusion constraint earlier.
		 * (XXX Maybe we should do these lookups earlier so we don't end up
		 * doing that.)
		 *
		 * 对普通外键约束这不应失败，因为我们已经检查过索引是唯一的，因此应有合适的相等操作符。对 period 外键，
		 * 若先前选了一个不匹配的排他约束，这里可能失败。（XXX 也许应更早做这些查找，以免走到这一步。）
		 */
		for_overlaps = with_period && i == numpks - 1;
		cmptype = for_overlaps ? COMPARE_OVERLAP : COMPARE_EQ;
		eqstrategy = IndexAmTranslateCompareType(cmptype, amid, opfamily, true);
		if (eqstrategy == InvalidStrategy)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					for_overlaps
					? errmsg("could not identify an overlaps operator for foreign key")
					: errmsg("could not identify an equality operator for foreign key"),
					errdetail("Could not translate compare type %d for operator family \"%s\" of access method \"%s\".",
							  cmptype, get_opfamily_name(opfamily, false), get_am_name(amid)));

		/*
		 * There had better be a primary equality operator for the index.
		 * We'll use it for PK = PK comparisons.
		 *
		 * 索引最好有一个主相等操作符。我们用它做 PK = PK 比较。
		 */
		ppeqop = get_opfamily_member(opfamily, opcintype, opcintype,
									 eqstrategy);

		if (!OidIsValid(ppeqop))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eqstrategy, opcintype, opcintype, opfamily);

		/*
		 * Are there equality operators that take exactly the FK type? Assume
		 * we should look through any domain here.
		 *
		 * 是否有正好接受外键类型的相等操作符？假定这里应看穿任何域。
		 */
		fktyped = getBaseType(fktype);

		pfeqop = get_opfamily_member(opfamily, opcintype, fktyped,
									 eqstrategy);
		if (OidIsValid(pfeqop))
		{
			pfeqop_right = fktyped;
			ffeqop = get_opfamily_member(opfamily, fktyped, fktyped,
										 eqstrategy);
		}
		else
		{
			/* keep compiler quiet */
			/*
			 *
			 * 免得编译器告警
			 */
			pfeqop_right = InvalidOid;
			ffeqop = InvalidOid;
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
		{
			/*
			 * Otherwise, look for an implicit cast from the FK type to the
			 * opcintype, and if found, use the primary equality operator.
			 * This is a bit tricky because opcintype might be a polymorphic
			 * type such as ANYARRAY or ANYENUM; so what we have to test is
			 * whether the two actual column types can be concurrently cast to
			 * that type.  (Otherwise, we'd fail to reject combinations such
			 * as int[] and point[].)
			 *
			 * 否则，寻找从外键类型到 opcintype 的隐式转换，若找到就用主相等操作符。这有点棘手，因为 opcintype 可能是
			 * ANYARRAY 或 ANYENUM 这类多态类型；所以要测试的是两个实际列类型能否同时转换到那个类型。（否则我们就无法拒绝
			 * int[] 和 point[] 这种组合。）
			 */
			Oid			input_typeids[2];
			Oid			target_typeids[2];

			input_typeids[0] = pktype;
			input_typeids[1] = fktype;
			target_typeids[0] = opcintype;
			target_typeids[1] = opcintype;
			if (can_coerce_type(2, input_typeids, target_typeids,
								COERCION_IMPLICIT))
			{
				pfeqop = ffeqop = ppeqop;
				pfeqop_right = opcintype;
			}
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("foreign key constraint \"%s\" cannot be implemented",
							fkconstraint->conname),
					 errdetail("Key columns \"%s\" of the referencing table and \"%s\" of the referenced table "
							   "are of incompatible types: %s and %s.",
							   strVal(list_nth(fkconstraint->fk_attrs, i)),
							   strVal(list_nth(fkconstraint->pk_attrs, i)),
							   format_type_be(fktype),
							   format_type_be(pktype))));

		/*
		 * This shouldn't be possible, but better check to make sure we have a
		 * consistent state for the check below.
		 *
		 * 这本不应可能，但最好检查一下，确保下面的检查处于一致状态。
		 */
		if ((OidIsValid(pkcoll) && !OidIsValid(fkcoll)) || (!OidIsValid(pkcoll) && OidIsValid(fkcoll)))
			elog(ERROR, "key columns are not both collatable");

		if (OidIsValid(pkcoll) && OidIsValid(fkcoll))
		{
			bool		pkcolldet;
			bool		fkcolldet;

			pkcolldet = get_collation_isdeterministic(pkcoll);
			fkcolldet = get_collation_isdeterministic(fkcoll);

			/*
			 * SQL requires that both collations are the same.  This is
			 * because we need a consistent notion of equality on both
			 * columns.  We relax this by allowing different collations if
			 * they are both deterministic.  (This is also for backward
			 * compatibility, because PostgreSQL has always allowed this.)
			 *
			 * SQL 要求两边的排序规则相同。因为我们需要对两列有一致的相等概念。若两者都是确定性排序规则，我们放宽这一点。（这也是为了向后兼容，
			 * PostgreSQL 一直允许这样。）
			 */
			if ((!pkcolldet || !fkcolldet) && pkcoll != fkcoll)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("foreign key constraint \"%s\" cannot be implemented", fkconstraint->conname),
						 errdetail("Key columns \"%s\" of the referencing table and \"%s\" of the referenced table "
								   "have incompatible collations: \"%s\" and \"%s\".  "
								   "If either collation is nondeterministic, then both collations have to be the same.",
								   strVal(list_nth(fkconstraint->fk_attrs, i)),
								   strVal(list_nth(fkconstraint->pk_attrs, i)),
								   get_collation_name(fkcoll),
								   get_collation_name(pkcoll))));
		}

		if (old_check_ok)
		{
			/*
			 * When a pfeqop changes, revalidate the constraint.  We could
			 * permit intra-opfamily changes, but that adds subtle complexity
			 * without any concrete benefit for core types.  We need not
			 * assess ppeqop or ffeqop, which RI_Initial_Check() does not use.
			 *
			 * 当 pfeqop 改变时，重新验证约束。我们可以允许操作符族内部的变更，但那会增加微妙的复杂度，对核心类型又没有具体好处。不必评估
			 * ppeqop 或 ffeqop，RI_Initial_Check() 不用它们。
			 */
			old_check_ok = (pfeqop == lfirst_oid(old_pfeqop_item));
			old_pfeqop_item = lnext(fkconstraint->old_conpfeqop,
									old_pfeqop_item);
		}
		if (old_check_ok)
		{
			Oid			old_fktype;
			Oid			new_fktype;
			CoercionPathType old_pathtype;
			CoercionPathType new_pathtype;
			Oid			old_castfunc;
			Oid			new_castfunc;
			Oid			old_fkcoll;
			Oid			new_fkcoll;
			Form_pg_attribute attr = TupleDescAttr(tab->oldDesc,
												   fkattnum[i] - 1);

			/*
			 * Identify coercion pathways from each of the old and new FK-side
			 * column types to the right (foreign) operand type of the pfeqop.
			 * We may assume that pg_constraint.conkey is not changing.
			 *
			 * 找出新旧外键侧列类型各自到 pfeqop 右（外部）操作数类型的转换路径。可以假定 pg_constraint.conkey
			 * 没有在变。
			 */
			old_fktype = attr->atttypid;
			new_fktype = fktype;
			old_pathtype = findFkeyCast(pfeqop_right, old_fktype,
										&old_castfunc);
			new_pathtype = findFkeyCast(pfeqop_right, new_fktype,
										&new_castfunc);

			old_fkcoll = attr->attcollation;
			new_fkcoll = fkcoll;

			/*
			 * Upon a change to the cast from the FK column to its pfeqop
			 * operand, revalidate the constraint.  For this evaluation, a
			 * binary coercion cast is equivalent to no cast at all.  While
			 * type implementors should design implicit casts with an eye
			 * toward consistency of operations like equality, we cannot
			 * assume here that they have done so.
			 *
			 * 外键列到其 pfeqop 操作数的转换一旦改变，就重新验证约束。对这次评估，二进制强制转换等价于完全没有转换。
			 * 类型实现者设计隐式转换时应考虑相等这类操作的一致性，但我们不能在这里假定他们已经这样做了。
			 *
			 * A function with a polymorphic argument could change behavior
			 * arbitrarily in response to get_fn_expr_argtype().  Therefore,
			 * when the cast destination is polymorphic, we only avoid
			 * revalidation if the input type has not changed at all.  Given
			 * just the core data types and operator classes, this requirement
			 * prevents no would-be optimizations.
			 *
			 * 带多态参数的函数可能因 get_fn_expr_argtype() 而任意改变行为。因此当转换目标是多态的时，
			 * 只有输入类型完全没变才跳过重新验证。只考虑核心数据类型和操作符类的话，这个要求并不会挡住任何本可做的优化。
			 *
			 * If the cast converts from a base type to a domain thereon, then
			 * that domain type must be the opcintype of the unique index.
			 * Necessarily, the primary key column must then be of the domain
			 * type.  Since the constraint was previously valid, all values on
			 * the foreign side necessarily exist on the primary side and in
			 * turn conform to the domain.  Consequently, we need not treat
			 * domains specially here.
			 *
			 * 若转换是从基类型转到其上的域，则该域类型必须是唯一索引的 opcintype。此时主键列必然也是该域类型。既然约束先前有效，
			 * 外键侧的所有值必然存在于主键侧，因而也符合该域。因此这里不必对域做特殊处理。
			 *
			 * If the collation changes, revalidation is required, unless both
			 * collations are deterministic, because those share the same
			 * notion of equality (because texteq reduces to bitwise
			 * equality).
			 *
			 * 若排序规则改变，需要重新验证，除非两边都是确定性排序规则，因为它们共享同一相等概念（texteq 归结为按位相等）。
			 *
			 * We need not directly consider the PK type.  It's necessarily
			 * binary coercible to the opcintype of the unique index column,
			 * and ri_triggers.c will only deal with PK datums in terms of
			 * that opcintype.  Changing the opcintype also changes pfeqop.
			 *
			 * 不必直接考虑主键类型。它必然可以二进制转换到唯一索引列的 opcintype，而 ri_triggers.c 只会按那个
			 * opcintype 处理主键数据。改变 opcintype 也会改变 pfeqop。
			 */
			old_check_ok = (new_pathtype == old_pathtype &&
							new_castfunc == old_castfunc &&
							(!IsPolymorphicType(pfeqop_right) ||
							 new_fktype == old_fktype) &&
							(new_fkcoll == old_fkcoll ||
							 (get_collation_isdeterministic(old_fkcoll) && get_collation_isdeterministic(new_fkcoll))));
		}

		pfeqoperators[i] = pfeqop;
		ppeqoperators[i] = ppeqop;
		ffeqoperators[i] = ffeqop;
	}

	/*
	 * For FKs with PERIOD we need additional operators to check whether the
	 * referencing row's range is contained by the aggregated ranges of the
	 * referenced row(s). For rangetypes and multirangetypes this is
	 * fk.periodatt <@ range_agg(pk.periodatt). Those are the only types we
	 * support for now. FKs will look these up at "runtime", but we should
	 * make sure the lookup works here, even if we don't use the values.
	 *
	 * 对带 PERIOD 的外键，还需要额外操作符来检查引用行的范围是否被被引用行（们）聚合后的范围所包含。对范围类型和多重范围类型，
	 * 这就是 fk.periodatt <@ range_agg(pk.periodatt)。目前只支持这些类型。外键会在运行时查找它们，
	 * 但即使这里不用这些值，也应确认查找能成功。
	 */
	if (with_period)
	{
		Oid			periodoperoid;
		Oid			aggedperiodoperoid;
		Oid			intersectoperoid;

		FindFKPeriodOpers(opclasses[numpks - 1], &periodoperoid, &aggedperiodoperoid,
						  &intersectoperoid);
	}

	/* First, create the constraint catalog entry itself. */
	/*
	 *
	 * 首先，创建约束目录项本身。
	 */
	address = addFkConstraint(addFkBothSides,
							  fkconstraint->conname, fkconstraint, rel, pkrel,
							  indexOid,
							  InvalidOid,	/* no parent constraint */
									/*
									 *
									 * 没有父约束
									 */
							  numfks,
							  pkattnum,
							  fkattnum,
							  pfeqoperators,
							  ppeqoperators,
							  ffeqoperators,
							  numfkdelsetcols,
							  fkdelsetcols,
							  false,
							  with_period);

	/* Next process the action triggers at the referenced side and recurse */
	/*
	 *
	 * 接着处理被引用侧的动作触发器并递归
	 */
	addFkRecurseReferenced(fkconstraint, rel, pkrel,
						   indexOid,
						   address.objectId,
						   numfks,
						   pkattnum,
						   fkattnum,
						   pfeqoperators,
						   ppeqoperators,
						   ffeqoperators,
						   numfkdelsetcols,
						   fkdelsetcols,
						   old_check_ok,
						   InvalidOid, InvalidOid,
						   with_period);

	/* Lastly create the check triggers at the referencing side and recurse */
	/*
	 *
	 * 最后在引用侧创建检查触发器并递归
	 */
	addFkRecurseReferencing(wqueue, fkconstraint, rel, pkrel,
							indexOid,
							address.objectId,
							numfks,
							pkattnum,
							fkattnum,
							pfeqoperators,
							ppeqoperators,
							ffeqoperators,
							numfkdelsetcols,
							fkdelsetcols,
							old_check_ok,
							lockmode,
							InvalidOid, InvalidOid,
							with_period);

	/*
	 * Done.  Close pk table, but keep lock until we've committed.
	 *
	 * 完成。关闭主键表，但锁保持到提交。
	 */
	table_close(pkrel, NoLock);

	return address;
}

/*
 * validateFkOnDeleteSetColumns
 *		Verifies that columns used in ON DELETE SET NULL/DEFAULT (...)
 *		column lists are valid.
 *
 * validateFkOnDeleteSetColumns：验证 ON DELETE SET NULL/DEFAULT (...)
 * 列清单里用到的列是否合法。
 *
 * If there are duplicates in the fksetcolsattnums[] array, this silently
 * removes the dups.  The new count of numfksetcols is returned.
 *
 * 若 fksetcolsattnums[] 数组里有重复，这里会悄悄去掉重复。返回新的 numfksetcols 计数。
 */
static int
validateFkOnDeleteSetColumns(int numfks, const int16 *fkattnums,
							 int numfksetcols, int16 *fksetcolsattnums,
							 List *fksetcols)
{
	int			numcolsout = 0;

	for (int i = 0; i < numfksetcols; i++)
	{
		int16		setcol_attnum = fksetcolsattnums[i];
		bool		seen = false;

		/* Make sure it's in fkattnums[] */
		/*
		 *
		 * 确认它在 fkattnums[] 里
		 */
		for (int j = 0; j < numfks; j++)
		{
			if (fkattnums[j] == setcol_attnum)
			{
				seen = true;
				break;
			}
		}

		if (!seen)
		{
			char	   *col = strVal(list_nth(fksetcols, i));

			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("column \"%s\" referenced in ON DELETE SET action must be part of foreign key", col)));
		}

		/* Now check for dups */
		/*
		 *
		 * 然后检查重复
		 */
		seen = false;
		for (int j = 0; j < numcolsout; j++)
		{
			if (fksetcolsattnums[j] == setcol_attnum)
			{
				seen = true;
				break;
			}
		}
		if (!seen)
			fksetcolsattnums[numcolsout++] = setcol_attnum;
	}
	return numcolsout;
}

/*
 * addFkConstraint
 *		Install pg_constraint entries to implement a foreign key constraint.
 *		Caller must separately invoke addFkRecurseReferenced and
 *		addFkRecurseReferencing, as appropriate, to install pg_trigger entries
 *		and (for partitioned tables) recurse to partitions.
 *
 * addFkConstraint：写入实现外键约束的 pg_constraint 项。调用方必须按情况另外调用
 * addFkRecurseReferenced 和 addFkRecurseReferencing，以安装 pg_trigger 项，
 * 并（对分区表）递归到分区。
 *
 * fkside: the side of the FK (or both) to create.  Caller should
 *      call addFkRecurseReferenced if this is addFkReferencedSide,
 *      addFkRecurseReferencing if it's addFkReferencingSide, or both if it's
 *      addFkBothSides.
 * constraintname: the base name for the constraint being added,
 *      copied to fkconstraint->conname if the latter is not set
 * fkconstraint: the constraint being added
 * rel: the root referencing relation
 * pkrel: the referenced relation; might be a partition, if recursing
 * indexOid: the OID of the index (on pkrel) implementing this constraint
 * parentConstr: the OID of a parent constraint; InvalidOid if this is a
 *      top-level constraint
 * numfks: the number of columns in the foreign key
 * pkattnum: the attnum array of referenced attributes
 * fkattnum: the attnum array of referencing attributes
 * pf/pp/ffeqoperators: OID array of operators between columns
 * numfkdelsetcols: the number of columns in the ON DELETE SET NULL/DEFAULT
 *      (...) clause
 * fkdelsetcols: the attnum array of the columns in the ON DELETE SET
 *      NULL/DEFAULT clause
 * with_period: true if this is a temporal FK
 *
 * fkside：要创建的外键侧（或两侧）。若是 addFkReferencedSide，调用方应调用
 * addFkRecurseReferenced；若是 addFkReferencingSide，应调用
 * addFkRecurseReferencing；若是 addFkBothSides，两边都调。constraintname：
 * 正在添加的约束的基名，若 fkconstraint->conname 未设置则拷进去。fkconstraint：正在添加的约束。
 * rel：根引用关系。pkrel：被引用关系；递归时可能是分区。indexOid：实现该约束的（pkrel 上的）索引 OID。
 * parentConstr：父约束的 OID；若这是顶层约束则为 InvalidOid。numfks：外键中的列数。pkattnum：
 * 被引用属性的 attnum 数组。fkattnum：引用属性的 attnum 数组。pf/pp/ffeqoperators：
 * 列之间操作符的 OID 数组。numfkdelsetcols：ON DELETE SET NULL/DEFAULT (...)
 * 子句中的列数。fkdelsetcols：该子句中列的 attnum 数组。with_period：为真表示这是时态外键。
 */
static ObjectAddress
addFkConstraint(addFkConstraintSides fkside,
				char *constraintname, Constraint *fkconstraint,
				Relation rel, Relation pkrel, Oid indexOid, Oid parentConstr,
				int numfks, int16 *pkattnum,
				int16 *fkattnum, Oid *pfeqoperators, Oid *ppeqoperators,
				Oid *ffeqoperators, int numfkdelsetcols, int16 *fkdelsetcols,
				bool is_internal, bool with_period)
{
	ObjectAddress address;
	Oid			constrOid;
	char	   *conname;
	bool		conislocal;
	int16		coninhcount;
	bool		connoinherit;

	/*
	 * Verify relkind for each referenced partition.  At the top level, this
	 * is redundant with a previous check, but we need it when recursing.
	 *
	 * 验证每个被引用分区的 relkind。在顶层这和先前的检查重复，但递归时需要它。
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	/*
	 * Caller supplies us with a constraint name; however, it may be used in
	 * this partition, so come up with a different one in that case.  Unless
	 * truncation to NAMEDATALEN dictates otherwise, the new name will be the
	 * supplied name with an underscore and digit(s) appended.
	 *
	 * 调用方给我们一个约束名；但它可能已被这个分区占用，那种情况下就另起一个。除非截断到 NAMEDATALEN 另有要求，
	 * 新名字是给出的名字加上下划线和数字。
	 */
	if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
							 RelationGetRelid(rel),
							 constraintname))
		conname = ChooseConstraintName(constraintname,
									   NULL,
									   "",
									   RelationGetNamespace(rel), NIL);
	else
		conname = constraintname;

	if (fkconstraint->conname == NULL)
		fkconstraint->conname = pstrdup(conname);

	if (OidIsValid(parentConstr))
	{
		conislocal = false;
		coninhcount = 1;
		connoinherit = false;
	}
	else
	{
		conislocal = true;
		coninhcount = 0;

		/*
		 * always inherit for partitioned tables, never for legacy inheritance
		 *
		 * 分区表总是继承，传统继承从不继承
		 */
		connoinherit = rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE;
	}

	/*
	 * Record the FK constraint in pg_constraint.
	 *
	 * 把外键约束记入 pg_constraint。
	 */
	constrOid = CreateConstraintEntry(conname,
									  RelationGetNamespace(rel),
									  CONSTRAINT_FOREIGN,
									  fkconstraint->deferrable,
									  fkconstraint->initdeferred,
									  fkconstraint->is_enforced,
									  fkconstraint->initially_valid,
									  parentConstr,
									  RelationGetRelid(rel),
									  fkattnum,
									  numfks,
									  numfks,
									  InvalidOid,	/* not a domain constraint */
											/*
											 *
											 * 不是域约束
											 */
									  indexOid,
									  RelationGetRelid(pkrel),
									  pkattnum,
									  pfeqoperators,
									  ppeqoperators,
									  ffeqoperators,
									  numfks,
									  fkconstraint->fk_upd_action,
									  fkconstraint->fk_del_action,
									  fkdelsetcols,
									  numfkdelsetcols,
									  fkconstraint->fk_matchtype,
									  NULL, /* no exclusion constraint */
										/*
										 *
										 * 不是排他约束
										 */
									  NULL, /* no check constraint */
										/*
										 *
										 * 不是检查约束
										 */
									  NULL,
									  conislocal,	/* islocal */
											/*
											 *
											 * islocal 标志
											 */
									  coninhcount,	/* inhcount */
											/*
											 *
											 * inhcount 计数
											 */
									  connoinherit, /* conNoInherit */
											/*
											 *
											 * conNoInherit 标志
											 */
									  with_period,	/* conPeriod */
											/*
											 *
											 * conPeriod 标志
											 */
									  is_internal); /* is_internal */
											/*
											 *
											 * is_internal 标志
											 */

	ObjectAddressSet(address, ConstraintRelationId, constrOid);

	/*
	 * In partitioning cases, create the dependency entries for this
	 * constraint.  (For non-partitioned cases, relevant entries were created
	 * by CreateConstraintEntry.)
	 *
	 * 在分区情况下，为这个约束创建依赖项。（非分区情况下，相关项已由 CreateConstraintEntry 创建。）
	 *
	 * On the referenced side, we need the constraint to have an internal
	 * dependency on its parent constraint; this means that this constraint
	 * cannot be dropped on its own -- only through the parent constraint. It
	 * also means the containing partition cannot be dropped on its own, but
	 * it can be detached, at which point this dependency is removed (after
	 * verifying that no rows are referenced via this FK.)
	 *
	 * 在被引用侧，需要让该约束对其父约束有内部依赖；这意味着不能单独删掉这个约束，只能通过父约束删除。这也意味着包含它的分区不能单独删除，
	 * 但可以分离，分离时会去掉这条依赖（在确认没有行经由这个外键被引用之后）。
	 *
	 * When processing the referencing side, we link the constraint via the
	 * special partitioning dependencies: the parent constraint is the primary
	 * dependent, and the partition on which the foreign key exists is the
	 * secondary dependency.  That way, this constraint is dropped if either
	 * of these objects is.
	 *
	 * 处理引用侧时，我们用特殊的分区依赖把约束连起来：父约束是主依赖者，外键所在的分区是次依赖。这样这两个对象中任何一个被删，
	 * 这个约束都会被删。
	 *
	 * Note that this is only necessary for the subsidiary pg_constraint rows
	 * in partitions; the topmost row doesn't need any of this.
	 *
	 * 注意这只对分区里的附属 pg_constraint 行有必要；最顶上的那一行不需要这些。
	 */
	if (OidIsValid(parentConstr))
	{
		ObjectAddress referenced;

		ObjectAddressSet(referenced, ConstraintRelationId, parentConstr);

		Assert(fkside != addFkBothSides);
		if (fkside == addFkReferencedSide)
			recordDependencyOn(&address, &referenced, DEPENDENCY_INTERNAL);
		else
		{
			recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_PRI);
			ObjectAddressSet(referenced, RelationRelationId, RelationGetRelid(rel));
			recordDependencyOn(&address, &referenced, DEPENDENCY_PARTITION_SEC);
		}
	}

	/* make new constraint visible, in case we add more */
	/*
	 *
	 * 让新约束可见，以防我们还要再加
	 */
	CommandCounterIncrement();

	return address;
}

/*
 * addFkRecurseReferenced
 *		Recursive helper for the referenced side of foreign key creation,
 *		which creates the action triggers and recurses
 *
 * addFkRecurseReferenced：外键创建时被引用侧的递归辅助函数，创建动作触发器并递归
 *
 * If the referenced relation is a plain relation, create the necessary action
 * triggers that implement the constraint.  If the referenced relation is a
 * partitioned table, then we create a pg_constraint row referencing the parent
 * of the referencing side for it and recurse on this routine for each
 * partition.
 *
 * 若被引用关系是普通关系，创建实现该约束所需的动作触发器。若被引用关系是分区表，则为它创建一行引用引用侧父表的
 * pg_constraint，并对每个分区递归调用本例程。
 *
 * fkconstraint: the constraint being added
 * rel: the root referencing relation
 * pkrel: the referenced relation; might be a partition, if recursing
 * indexOid: the OID of the index (on pkrel) implementing this constraint
 * parentConstr: the OID of a parent constraint; InvalidOid if this is a
 *      top-level constraint
 * numfks: the number of columns in the foreign key
 * pkattnum: the attnum array of referenced attributes
 * fkattnum: the attnum array of referencing attributes
 * numfkdelsetcols: the number of columns in the ON DELETE SET
 *      NULL/DEFAULT (...) clause
 * fkdelsetcols: the attnum array of the columns in the ON DELETE SET
 *      NULL/DEFAULT clause
 * pf/pp/ffeqoperators: OID array of operators between columns
 * old_check_ok: true if this constraint replaces an existing one that
 *      was already validated (thus this one doesn't need validation)
 * parentDelTrigger and parentUpdTrigger: when recursively called on a
 *      partition, the OIDs of the parent action triggers for DELETE and
 *      UPDATE respectively.
 * with_period: true if this is a temporal FK
 *
 * fkconstraint：正在添加的约束。rel：根引用关系。pkrel：被引用关系；递归时可能是分区。indexOid：
 * 实现该约束的（pkrel 上的）索引 OID。parentConstr：父约束 OID；顶层则为 InvalidOid。
 * numfks：外键列数。pkattnum：被引用属性的 attnum 数组。fkattnum：引用属性的 attnum 数组。
 * numfkdelsetcols：ON DELETE SET NULL/DEFAULT (...) 子句中的列数。
 * fkdelsetcols：该子句中列的 attnum 数组。pf/pp/ffeqoperators：列之间操作符的 OID 数组。
 * old_check_ok：为真表示本约束替换一个已经验证过的现有约束（因此本约束不必验证）。parentDelTrigger 和
 * parentUpdTrigger：递归到分区时，分别为 DELETE 和 UPDATE 的父动作触发器 OID。
 * with_period：为真表示时态外键。
 */
static void
addFkRecurseReferenced(Constraint *fkconstraint, Relation rel,
					   Relation pkrel, Oid indexOid, Oid parentConstr,
					   int numfks,
					   int16 *pkattnum, int16 *fkattnum, Oid *pfeqoperators,
					   Oid *ppeqoperators, Oid *ffeqoperators,
					   int numfkdelsetcols, int16 *fkdelsetcols,
					   bool old_check_ok,
					   Oid parentDelTrigger, Oid parentUpdTrigger,
					   bool with_period)
{
	Oid			deleteTriggerOid = InvalidOid,
				updateTriggerOid = InvalidOid;

	Assert(CheckRelationLockedByMe(pkrel, ShareRowExclusiveLock, true));
	Assert(CheckRelationLockedByMe(rel, ShareRowExclusiveLock, true));

	/*
	 * Create action triggers to enforce the constraint, or skip them if the
	 * constraint is NOT ENFORCED.
	 *
	 * 创建动作触发器来强制该约束；若约束是 NOT ENFORCED，则跳过。
	 */
	if (fkconstraint->is_enforced)
		createForeignKeyActionTriggers(RelationGetRelid(rel),
									   RelationGetRelid(pkrel),
									   fkconstraint,
									   parentConstr, indexOid,
									   parentDelTrigger, parentUpdTrigger,
									   &deleteTriggerOid, &updateTriggerOid);

	/*
	 * If the referenced table is partitioned, recurse on ourselves to handle
	 * each partition.  We need one pg_constraint row created for each
	 * partition in addition to the pg_constraint row for the parent table.
	 *
	 * 若被引用表是分区的，对自己递归以处理每个分区。除了父表的 pg_constraint 行之外，每个分区还需要一行。
	 */
	if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd = RelationGetPartitionDesc(pkrel, true);

		for (int i = 0; i < pd->nparts; i++)
		{
			Relation	partRel;
			AttrMap    *map;
			AttrNumber *mapped_pkattnum;
			Oid			partIndexId;
			ObjectAddress address;

			/* XXX would it be better to acquire these locks beforehand? */
			/*
			 *
			 * XXX 事先拿到这些锁会不会更好？
			 */
			partRel = table_open(pd->oids[i], ShareRowExclusiveLock);

			/*
			 * Map the attribute numbers in the referenced side of the FK
			 * definition to match the partition's column layout.
			 *
			 * 把外键定义里被引用侧的属性号映射成与分区列布局一致。
			 */
			map = build_attrmap_by_name_if_req(RelationGetDescr(partRel),
											   RelationGetDescr(pkrel),
											   false);
			if (map)
			{
				mapped_pkattnum = palloc(sizeof(AttrNumber) * numfks);
				for (int j = 0; j < numfks; j++)
					mapped_pkattnum[j] = map->attnums[pkattnum[j] - 1];
			}
			else
				mapped_pkattnum = pkattnum;

			/* Determine the index to use at this level */
			/*
			 *
			 * 确定这一层要用的索引
			 */
			partIndexId = index_get_partition(partRel, indexOid);
			if (!OidIsValid(partIndexId))
				elog(ERROR, "index for %u not found in partition %s",
					 indexOid, RelationGetRelationName(partRel));

			/* Create entry at this level ... */
			/*
			 *
			 * 在这一层创建项……
			 */
			address = addFkConstraint(addFkReferencedSide,
									  fkconstraint->conname, fkconstraint, rel,
									  partRel, partIndexId, parentConstr,
									  numfks, mapped_pkattnum,
									  fkattnum, pfeqoperators, ppeqoperators,
									  ffeqoperators, numfkdelsetcols,
									  fkdelsetcols, true, with_period);
			/* ... and recurse to our children */
			/*
			 *
			 * ……并递归到子分区
			 */
			addFkRecurseReferenced(fkconstraint, rel, partRel,
								   partIndexId, address.objectId, numfks,
								   mapped_pkattnum, fkattnum,
								   pfeqoperators, ppeqoperators, ffeqoperators,
								   numfkdelsetcols, fkdelsetcols,
								   old_check_ok,
								   deleteTriggerOid, updateTriggerOid,
								   with_period);

			/* Done -- clean up (but keep the lock) */
			/*
			 *
			 * 完成，清理（但保留锁）
			 */
			table_close(partRel, NoLock);
			if (map)
			{
				pfree(mapped_pkattnum);
				free_attrmap(map);
			}
		}
	}
}

/*
 * addFkRecurseReferencing
 *		Recursive helper for the referencing side of foreign key creation,
 *		which creates the check triggers and recurses
 *
 * addFkRecurseReferencing：外键创建时引用侧的递归辅助函数，创建检查触发器并递归
 *
 * If the referencing relation is a plain relation, create the necessary check
 * triggers that implement the constraint, and set up for Phase 3 constraint
 * verification.  If the referencing relation is a partitioned table, then
 * we create a pg_constraint row for it and recurse on this routine for each
 * partition.
 *
 * 若引用关系是普通关系，创建实现该约束所需的检查触发器，并安排阶段 3 的约束验证。若引用关系是分区表，则为它创建一行
 * pg_constraint，并对每个分区递归调用本例程。
 *
 * We assume that the referenced relation is locked against concurrent
 * deletions.  If it's a partitioned relation, every partition must be so
 * locked.
 *
 * 假定被引用关系已被锁住，防止并发删除。若它是分区关系，每个分区都必须这样锁住。
 *
 * wqueue: the ALTER TABLE work queue; NULL when not running as part
 *      of an ALTER TABLE sequence.
 * fkconstraint: the constraint being added
 * rel: the referencing relation; might be a partition, if recursing
 * pkrel: the root referenced relation
 * indexOid: the OID of the index (on pkrel) implementing this constraint
 * parentConstr: the OID of the parent constraint (there is always one)
 * numfks: the number of columns in the foreign key
 * pkattnum: the attnum array of referenced attributes
 * fkattnum: the attnum array of referencing attributes
 * pf/pp/ffeqoperators: OID array of operators between columns
 * numfkdelsetcols: the number of columns in the ON DELETE SET NULL/DEFAULT
 *      (...) clause
 * fkdelsetcols: the attnum array of the columns in the ON DELETE SET
 *      NULL/DEFAULT clause
 * old_check_ok: true if this constraint replaces an existing one that
 *      was already validated (thus this one doesn't need validation)
 * lockmode: the lockmode to acquire on partitions when recursing
 * parentInsTrigger and parentUpdTrigger: when being recursively called on
 *      a partition, the OIDs of the parent check triggers for INSERT and
 *      UPDATE respectively.
 * with_period: true if this is a temporal FK
 *
 * wqueue：ALTER TABLE 工作队列；若不是作为 ALTER TABLE 序列的一部分运行则为 NULL。
 * fkconstraint：正在添加的约束。rel：引用关系；递归时可能是分区。pkrel：根被引用关系。indexOid：
 * 实现该约束的（pkrel 上的）索引 OID。parentConstr：父约束 OID（总会有一个）。numfks：外键列数。
 * pkattnum：被引用属性的 attnum 数组。fkattnum：引用属性的 attnum 数组。
 * pf/pp/ffeqoperators：列之间操作符的 OID 数组。numfkdelsetcols：ON DELETE SET
 * NULL/DEFAULT (...) 子句中的列数。fkdelsetcols：该子句中列的 attnum 数组。
 * old_check_ok：为真表示本约束替换一个已经验证过的现有约束（因此不必验证）。lockmode：递归时对分区获取的锁模式。
 * parentInsTrigger 和 parentUpdTrigger：递归到分区时，分别为 INSERT 和 UPDATE
 * 的父检查触发器 OID。with_period：为真表示时态外键。
 */
static void
addFkRecurseReferencing(List **wqueue, Constraint *fkconstraint, Relation rel,
						Relation pkrel, Oid indexOid, Oid parentConstr,
						int numfks, int16 *pkattnum, int16 *fkattnum,
						Oid *pfeqoperators, Oid *ppeqoperators, Oid *ffeqoperators,
						int numfkdelsetcols, int16 *fkdelsetcols,
						bool old_check_ok, LOCKMODE lockmode,
						Oid parentInsTrigger, Oid parentUpdTrigger,
						bool with_period)
{
	Oid			insertTriggerOid = InvalidOid,
				updateTriggerOid = InvalidOid;

	Assert(OidIsValid(parentConstr));
	Assert(CheckRelationLockedByMe(rel, ShareRowExclusiveLock, true));
	Assert(CheckRelationLockedByMe(pkrel, ShareRowExclusiveLock, true));

	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * Add check triggers if the constraint is ENFORCED, and if needed,
	 * schedule them to be checked in Phase 3.
	 *
	 * 若约束是 ENFORCED，加上检查触发器，并在需要时安排它们在 Phase 3 检查。
	 *
	 * If the relation is partitioned, drill down to do it to its partitions.
	 *
	 * 若关系是分区的，深入到它的各个分区去做。
	 */
	if (fkconstraint->is_enforced)
		createForeignKeyCheckTriggers(RelationGetRelid(rel),
									  RelationGetRelid(pkrel),
									  fkconstraint,
									  parentConstr,
									  indexOid,
									  parentInsTrigger, parentUpdTrigger,
									  &insertTriggerOid, &updateTriggerOid);

	if (rel->rd_rel->relkind == RELKIND_RELATION)
	{
		/*
		 * Tell Phase 3 to check that the constraint is satisfied by existing
		 * rows. We can skip this during table creation, when constraint is
		 * specified as NOT ENFORCED, or when requested explicitly by
		 * specifying NOT VALID in an ADD FOREIGN KEY command, and when we're
		 * recreating a constraint following a SET DATA TYPE operation that
		 * did not impugn its validity.
		 *
		 * 告诉 Phase 3 检查现有行是否满足该约束。建表时、约束指定为 NOT ENFORCED 时、ADD FOREIGN KEY
		 * 显式指定 NOT VALID 时，以及在一次并未动摇其有效性的 SET DATA TYPE 之后重建约束时，可以跳过。
		 */
		if (wqueue && !old_check_ok && !fkconstraint->skip_validation &&
			fkconstraint->is_enforced)
		{
			NewConstraint *newcon;
			AlteredTableInfo *tab;

			tab = ATGetQueueEntry(wqueue, rel);

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = get_constraint_name(parentConstr);
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = RelationGetRelid(pkrel);
			newcon->refindid = indexOid;
			newcon->conid = parentConstr;
			newcon->conwithperiod = fkconstraint->fk_with_period;
			newcon->qual = (Node *) fkconstraint;

			tab->constraints = lappend(tab->constraints, newcon);
		}
	}
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc pd = RelationGetPartitionDesc(rel, true);
		Relation	trigrel;

		/*
		 * Triggers of the foreign keys will be manipulated a bunch of times
		 * in the loop below.  To avoid repeatedly opening/closing the trigger
		 * catalog relation, we open it here and pass it to the subroutines
		 * called below.
		 *
		 * 下面的循环会多次摆弄外键的触发器。为避免反复开关触发器目录关系，在这里打开它并传给下面调用的子程序。
		 */
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

		/*
		 * Recurse to take appropriate action on each partition; either we
		 * find an existing constraint to reparent to ours, or we create a new
		 * one.
		 *
		 * 递归到每个分区采取适当动作：要么找到一个现有约束重新挂到我们的约束下，要么新建一个。
		 */
		for (int i = 0; i < pd->nparts; i++)
		{
			Relation	partition = table_open(pd->oids[i], lockmode);
			List	   *partFKs;
			AttrMap    *attmap;
			AttrNumber	mapped_fkattnum[INDEX_MAX_KEYS];
			bool		attached;
			ObjectAddress address;

			CheckAlterTableIsSafe(partition);

			attmap = build_attrmap_by_name(RelationGetDescr(partition),
										   RelationGetDescr(rel),
										   false);
			for (int j = 0; j < numfks; j++)
				mapped_fkattnum[j] = attmap->attnums[fkattnum[j] - 1];

			/* Check whether an existing constraint can be repurposed */
			/*
			 *
			 * 检查能否把现有约束改作他用
			 */
			partFKs = copyObject(RelationGetFKeyList(partition));
			attached = false;
			foreach_node(ForeignKeyCacheInfo, fk, partFKs)
			{
				if (tryAttachPartitionForeignKey(wqueue,
												 fk,
												 partition,
												 parentConstr,
												 numfks,
												 mapped_fkattnum,
												 pkattnum,
												 pfeqoperators,
												 insertTriggerOid,
												 updateTriggerOid,
												 trigrel))
				{
					attached = true;
					break;
				}
			}
			if (attached)
			{
				table_close(partition, NoLock);
				continue;
			}

			/*
			 * No luck finding a good constraint to reuse; create our own.
			 *
			 * 没能找到可复用的好约束；自己创建一个。
			 */
			address = addFkConstraint(addFkReferencingSide,
									  fkconstraint->conname, fkconstraint,
									  partition, pkrel, indexOid, parentConstr,
									  numfks, pkattnum,
									  mapped_fkattnum, pfeqoperators,
									  ppeqoperators, ffeqoperators,
									  numfkdelsetcols, fkdelsetcols, true,
									  with_period);

			/* call ourselves to finalize the creation and we're done */
			/*
			 *
			 * 调用自己来完成创建，然后就结束
			 */
			addFkRecurseReferencing(wqueue, fkconstraint, partition, pkrel,
									indexOid,
									address.objectId,
									numfks,
									pkattnum,
									mapped_fkattnum,
									pfeqoperators,
									ppeqoperators,
									ffeqoperators,
									numfkdelsetcols,
									fkdelsetcols,
									old_check_ok,
									lockmode,
									insertTriggerOid,
									updateTriggerOid,
									with_period);

			table_close(partition, NoLock);
		}

		table_close(trigrel, RowExclusiveLock);
	}
}

/*
 * CloneForeignKeyConstraints
 *		Clone foreign keys from a partitioned table to a newly acquired
 *		partition.
 *
 * CloneForeignKeyConstraints：把外键从分区表克隆到新获得的分区。
 *
 * partitionRel is a partition of parentRel, so we can be certain that it has
 * the same columns with the same datatypes.  The columns may be in different
 * order, though.
 *
 * partitionRel 是 parentRel 的分区，因此可以确定它有相同数据类型的相同列。不过列的顺序可能不同。
 *
 * wqueue must be passed to set up phase 3 constraint checking, unless the
 * referencing-side partition is known to be empty (such as in CREATE TABLE /
 * PARTITION OF).
 *
 * 必须传入 wqueue 以安排阶段 3 的约束检查，除非已知引用侧分区是空的（例如 CREATE TABLE /
 * PARTITION OF）。
 */
static void
CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
						   Relation partitionRel)
{
	/* This only works for declarative partitioning */
	/*
	 *
	 * 这只对声明式分区有效
	 */
	Assert(parentRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * First, clone constraints where the parent is on the referencing side.
	 *
	 * 首先，克隆父表位于引用侧的那些约束。
	 */
	CloneFkReferencing(wqueue, parentRel, partitionRel);

	/*
	 * Clone constraints for which the parent is on the referenced side.
	 *
	 * 再克隆父表位于被引用侧的那些约束。
	 */
	CloneFkReferenced(parentRel, partitionRel);
}

/*
 * CloneFkReferenced
 *		Subroutine for CloneForeignKeyConstraints
 *
 * CloneFkReferenced：CloneForeignKeyConstraints 的子程序
 *
 * Find all the FKs that have the parent relation on the referenced side;
 * clone those constraints to the given partition.  This is to be called
 * when the partition is being created or attached.
 *
 * 找出所有被引用侧是父关系的外键，把这些约束克隆到给定分区。在创建或挂接分区时调用。
 *
 * This recurses to partitions, if the relation being attached is partitioned.
 * Recursion is done by calling addFkRecurseReferenced.
 *
 * 若正在挂接的关系本身是分区的，会递归到分区。递归通过调用 addFkRecurseReferenced 完成。
 */
static void
CloneFkReferenced(Relation parentRel, Relation partitionRel)
{
	Relation	pg_constraint;
	AttrMap    *attmap;
	ListCell   *cell;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	List	   *clone = NIL;
	Relation	trigrel;

	/*
	 * Search for any constraints where this partition's parent is in the
	 * referenced side.  However, we must not clone any constraint whose
	 * parent constraint is also going to be cloned, to avoid duplicates.  So
	 * do it in two steps: first construct the list of constraints to clone,
	 * then go over that list cloning those whose parents are not in the list.
	 * (We must not rely on the parent being seen first, since the catalog
	 * scan could return children first.)
	 *
	 * 搜索被引用侧是这个分区的父表的约束。但不能克隆那些其父约束也即将被克隆的约束，以免重复。所以分两步：先构造要克隆的约束列表，
	 * 再遍历该列表，只克隆其父约束不在列表里的那些。（不能依赖先看到父约束，因为目录扫描可能先返回子约束。）
	 */
	pg_constraint = table_open(ConstraintRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parentRel)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));
	/* This is a seqscan, as we don't have a usable index ... */
	/*
	 *
	 * 这是顺序扫描，因为没有可用的索引……
	 */
	scan = systable_beginscan(pg_constraint, InvalidOid, true,
							  NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		clone = lappend_oid(clone, constrForm->oid);
	}
	systable_endscan(scan);
	table_close(pg_constraint, RowShareLock);

	/*
	 * Triggers of the foreign keys will be manipulated a bunch of times in
	 * the loop below.  To avoid repeatedly opening/closing the trigger
	 * catalog relation, we open it here and pass it to the subroutines called
	 * below.
	 *
	 * 下面的循环会多次摆弄外键的触发器。为避免反复开关触发器目录关系，在这里打开它并传给下面调用的子程序。
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	attmap = build_attrmap_by_name(RelationGetDescr(partitionRel),
								   RelationGetDescr(parentRel),
								   false);
	foreach(cell, clone)
	{
		Oid			constrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	fkRel;
		Oid			indexOid;
		Oid			partIndexId;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_confkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		int			numfkdelsetcols;
		AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		ObjectAddress address;
		Oid			deleteTriggerOid = InvalidOid,
					updateTriggerOid = InvalidOid;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * As explained above: don't try to clone a constraint for which we're
		 * going to clone the parent.
		 *
		 * 如上所述：不要去克隆一个我们即将克隆其父约束的约束。
		 */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* We need the same lock level that CreateTrigger will acquire */
		/*
		 *
		 * 需要和 CreateTrigger 将要获取的相同锁级别
		 */
		fkRel = table_open(constrForm->conrelid, ShareRowExclusiveLock);

		indexOid = constrForm->conindid;
		DeconstructFkConstraintRow(tuple,
								   &numfks,
								   conkey,
								   confkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop,
								   &numfkdelsetcols,
								   confdelsetcols);

		for (int i = 0; i < numfks; i++)
			mapped_confkey[i] = attmap->attnums[confkey[i] - 1];

		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		fkconstraint->conname = NameStr(constrForm->conname);
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		/*
		 *
		 * fk_attrs 在下面确定
		 */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->is_enforced = constrForm->conenforced;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = constrForm->convalidated;

		/* set up colnames that are used to generate the constraint name */
		/*
		 *
		 * 准备用来生成约束名的列名
		 */
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(fkRel),
								conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		/*
		 * Add the new foreign key constraint pointing to the new partition.
		 * Because this new partition appears in the referenced side of the
		 * constraint, we don't need to set up for Phase 3 check.
		 *
		 * 加上指向新分区的新外键约束。因为这个新分区出现在约束的被引用侧，不必安排 Phase 3 检查。
		 */
		partIndexId = index_get_partition(partitionRel, indexOid);
		if (!OidIsValid(partIndexId))
			elog(ERROR, "index for %u not found in partition %s",
				 indexOid, RelationGetRelationName(partitionRel));

		/*
		 * Get the "action" triggers belonging to the constraint to pass as
		 * parent OIDs for similar triggers that will be created on the
		 * partition in addFkRecurseReferenced().
		 *
		 * 取出属于该约束的动作触发器，作为将在分区上由 addFkRecurseReferenced() 创建的同类触发器的父 OID。
		 */
		if (constrForm->conenforced)
			GetForeignKeyActionTriggers(trigrel, constrOid,
										constrForm->confrelid, constrForm->conrelid,
										&deleteTriggerOid, &updateTriggerOid);

		/* Add this constraint ... */
		/*
		 *
		 * 加上这个约束……
		 */
		address = addFkConstraint(addFkReferencedSide,
								  fkconstraint->conname, fkconstraint, fkRel,
								  partitionRel, partIndexId, constrOid,
								  numfks, mapped_confkey,
								  conkey, conpfeqop, conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols, false,
								  constrForm->conperiod);
		/* ... and recurse */
		/*
		 *
		 * ……并递归
		 */
		addFkRecurseReferenced(fkconstraint,
							   fkRel,
							   partitionRel,
							   partIndexId,
							   address.objectId,
							   numfks,
							   mapped_confkey,
							   conkey,
							   conpfeqop,
							   conppeqop,
							   conffeqop,
							   numfkdelsetcols,
							   confdelsetcols,
							   true,
							   deleteTriggerOid,
							   updateTriggerOid,
							   constrForm->conperiod);

		table_close(fkRel, NoLock);
		ReleaseSysCache(tuple);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * CloneFkReferencing
 *		Subroutine for CloneForeignKeyConstraints
 *
 * CloneFkReferencing：CloneForeignKeyConstraints 的子程序
 *
 * For each FK constraint of the parent relation in the given list, find an
 * equivalent constraint in its partition relation that can be reparented;
 * if one cannot be found, create a new constraint in the partition as its
 * child.
 *
 * 对给定列表里父关系的每个外键约束，在其分区关系中找一个可以重新挂父的等价约束；找不到就在分区里作为其子约束新建一个。
 *
 * If wqueue is given, it is used to set up phase-3 verification for each
 * cloned constraint; omit it if such verification is not needed
 * (example: the partition is being created anew).
 *
 * 若给出 wqueue，用它为每个克隆的约束安排阶段 3 验证；不需要这种验证时就省略（例如分区是新建的）。
 */
static void
CloneFkReferencing(List **wqueue, Relation parentRel, Relation partRel)
{
	AttrMap    *attmap;
	List	   *partFKs;
	List	   *clone = NIL;
	ListCell   *cell;
	Relation	trigrel;

	/* obtain a list of constraints that we need to clone */
	/*
	 *
	 * 取得需要克隆的约束列表
	 */
	foreach(cell, RelationGetFKeyList(parentRel))
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);

		/*
		 * Refuse to attach a table as partition that this partitioned table
		 * already has a foreign key to.  This isn't useful schema, which is
		 * proven by the fact that there have been no user complaints that
		 * it's already impossible to achieve this in the opposite direction,
		 * i.e., creating a foreign key that references a partition.  This
		 * restriction allows us to dodge some complexities around
		 * pg_constraint and pg_trigger row creations that would be needed
		 * during ATTACH/DETACH for this kind of relationship.
		 *
		 * 拒绝把一张表挂成这个分区表已经有外键指向它的分区。这种模式没用，证据是反过来（创建引用某个分区的外键）本来就做不到，
		 * 却从没有用户抱怨。这个限制让我们避开 ATTACH/DETACH 这种关系时 pg_constraint 和 pg_trigger
		 * 行创建的一些复杂性。
		 */
		if (fk->confrelid == RelationGetRelid(partRel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot attach table \"%s\" as a partition because it is referenced by foreign key \"%s\"",
							RelationGetRelationName(partRel),
							get_constraint_name(fk->conoid))));

		clone = lappend_oid(clone, fk->conoid);
	}

	/*
	 * Silently do nothing if there's nothing to do.  In particular, this
	 * avoids throwing a spurious error for foreign tables.
	 *
	 * 若无事可做就悄悄返回。尤其这样可以避免对外部表抛出多余的错误。
	 */
	if (clone == NIL)
		return;

	if (partRel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * Triggers of the foreign keys will be manipulated a bunch of times in
	 * the loop below.  To avoid repeatedly opening/closing the trigger
	 * catalog relation, we open it here and pass it to the subroutines called
	 * below.
	 *
	 * 下面的循环会多次摆弄外键的触发器。为避免反复开关触发器目录关系，在这里打开它并传给下面调用的子程序。
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * The constraint key may differ, if the columns in the partition are
	 * different.  This map is used to convert them.
	 *
	 * 若分区里的列不同，约束键可能不同。用这张映射来转换它们。
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(partRel),
								   RelationGetDescr(parentRel),
								   false);

	partFKs = copyObject(RelationGetFKeyList(partRel));

	foreach(cell, clone)
	{
		Oid			parentConstrOid = lfirst_oid(cell);
		Form_pg_constraint constrForm;
		Relation	pkrel;
		HeapTuple	tuple;
		int			numfks;
		AttrNumber	conkey[INDEX_MAX_KEYS];
		AttrNumber	mapped_conkey[INDEX_MAX_KEYS];
		AttrNumber	confkey[INDEX_MAX_KEYS];
		Oid			conpfeqop[INDEX_MAX_KEYS];
		Oid			conppeqop[INDEX_MAX_KEYS];
		Oid			conffeqop[INDEX_MAX_KEYS];
		int			numfkdelsetcols;
		AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
		Constraint *fkconstraint;
		bool		attached;
		Oid			indexOid;
		ObjectAddress address;
		ListCell   *lc;
		Oid			insertTriggerOid = InvalidOid,
					updateTriggerOid = InvalidOid;
		bool		with_period;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(parentConstrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u",
				 parentConstrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Don't clone constraints whose parents are being cloned */
		/*
		 *
		 * 不要克隆那些父约束正在被克隆的约束
		 */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * Need to prevent concurrent deletions.  If pkrel is a partitioned
		 * relation, that means to lock all partitions.
		 *
		 * 需要防止并发删除。若 pkrel 是分区关系，意味着要锁住所有分区。
		 */
		pkrel = table_open(constrForm->confrelid, ShareRowExclusiveLock);
		if (pkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			(void) find_all_inheritors(RelationGetRelid(pkrel),
									   ShareRowExclusiveLock, NULL);

		DeconstructFkConstraintRow(tuple, &numfks, conkey, confkey,
								   conpfeqop, conppeqop, conffeqop,
								   &numfkdelsetcols, confdelsetcols);
		for (int i = 0; i < numfks; i++)
			mapped_conkey[i] = attmap->attnums[conkey[i] - 1];

		/*
		 * Get the "check" triggers belonging to the constraint, if it is
		 * ENFORCED, to pass as parent OIDs for similar triggers that will be
		 * created on the partition in addFkRecurseReferencing().  They are
		 * also passed to tryAttachPartitionForeignKey() below to simply
		 * assign as parents to the partition's existing "check" triggers,
		 * that is, if the corresponding constraints is deemed attachable to
		 * the parent constraint.
		 *
		 * 若约束是 ENFORCED，取出属于它的检查触发器，作为将在分区上由 addFkRecurseReferencing()
		 * 创建的同类触发器的父 OID。若对应约束被认为可以挂到父约束上，它们也会传给下面的
		 * tryAttachPartitionForeignKey()，直接指定为分区已有检查触发器的父触发器。
		 */
		if (constrForm->conenforced)
			GetForeignKeyCheckTriggers(trigrel, constrForm->oid,
									   constrForm->confrelid, constrForm->conrelid,
									   &insertTriggerOid, &updateTriggerOid);

		/*
		 * Before creating a new constraint, see whether any existing FKs are
		 * fit for the purpose.  If one is, attach the parent constraint to
		 * it, and don't clone anything.  This way we avoid the expensive
		 * verification step and don't end up with a duplicate FK, and we
		 * don't need to recurse to partitions for this constraint.
		 *
		 * 创建新约束之前，先看有没有现成的外键合用。若有，把父约束挂上去，什么也不克隆。这样可以避开昂贵的验证，也不会留下重复外键，
		 * 并且不必为这个约束递归到分区。
		 */
		attached = false;
		foreach(lc, partFKs)
		{
			ForeignKeyCacheInfo *fk = lfirst_node(ForeignKeyCacheInfo, lc);

			if (tryAttachPartitionForeignKey(wqueue,
											 fk,
											 partRel,
											 parentConstrOid,
											 numfks,
											 mapped_conkey,
											 confkey,
											 conpfeqop,
											 insertTriggerOid,
											 updateTriggerOid,
											 trigrel))
			{
				attached = true;
				table_close(pkrel, NoLock);
				break;
			}
		}
		if (attached)
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* No dice.  Set up to create our own constraint */
		/*
		 *
		 * 没戏。准备创建我们自己的约束
		 */
		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		/* ->conname determined below */
		/*
		 *
		 * conname 在下面确定
		 */
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs determined below */
		/*
		 *
		 * fk_attrs 在下面确定
		 */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->is_enforced = constrForm->conenforced;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = constrForm->convalidated;
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(partRel),
								mapped_conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		indexOid = constrForm->conindid;
		with_period = constrForm->conperiod;

		/* Create the pg_constraint entry at this level */
		/*
		 *
		 * 在这一层创建 pg_constraint 项
		 */
		address = addFkConstraint(addFkReferencingSide,
								  NameStr(constrForm->conname), fkconstraint,
								  partRel, pkrel, indexOid, parentConstrOid,
								  numfks, confkey,
								  mapped_conkey, conpfeqop,
								  conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols,
								  false, with_period);

		/* Done with the cloned constraint's tuple */
		/*
		 *
		 * 克隆约束的元组处理完了
		 */
		ReleaseSysCache(tuple);

		/* Create the check triggers, and recurse to partitions, if any */
		/*
		 *
		 * 创建检查触发器，若有分区则递归下去
		 */
		addFkRecurseReferencing(wqueue,
								fkconstraint,
								partRel,
								pkrel,
								indexOid,
								address.objectId,
								numfks,
								confkey,
								mapped_conkey,
								conpfeqop,
								conppeqop,
								conffeqop,
								numfkdelsetcols,
								confdelsetcols,
								false,	/* no old check exists */
									/*
									 *
									 * 不存在旧的检查
									 */
								AccessExclusiveLock,
								insertTriggerOid,
								updateTriggerOid,
								with_period);
		table_close(pkrel, NoLock);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * When the parent of a partition receives [the referencing side of] a foreign
 * key, we must propagate that foreign key to the partition.  However, the
 * partition might already have an equivalent foreign key; this routine
 * compares the given ForeignKeyCacheInfo (in the partition) to the FK defined
 * by the other parameters.  If they are equivalent, create the link between
 * the two constraints and return true.
 *
 * 当分区的父表收到外键的引用侧时，必须把该外键传播到分区。但分区可能已经有等价的外键；本例程把给定的
 * ForeignKeyCacheInfo（在分区里）和其余参数定义的外键比较。若等价，就在两个约束之间建立链接并返回真。
 *
 * If the given FK does not match the one defined by rest of the params,
 * return false.
 *
 * 若给定外键与其余参数定义的那个不匹配，返回假。
 */
static bool
tryAttachPartitionForeignKey(List **wqueue,
							 ForeignKeyCacheInfo *fk,
							 Relation partition,
							 Oid parentConstrOid,
							 int numfks,
							 AttrNumber *mapped_conkey,
							 AttrNumber *confkey,
							 Oid *conpfeqop,
							 Oid parentInsTrigger,
							 Oid parentUpdTrigger,
							 Relation trigrel)
{
	HeapTuple	parentConstrTup;
	Form_pg_constraint parentConstr;
	HeapTuple	partcontup;
	Form_pg_constraint partConstr;

	parentConstrTup = SearchSysCache1(CONSTROID,
									  ObjectIdGetDatum(parentConstrOid));
	if (!HeapTupleIsValid(parentConstrTup))
		elog(ERROR, "cache lookup failed for constraint %u", parentConstrOid);
	parentConstr = (Form_pg_constraint) GETSTRUCT(parentConstrTup);

	/*
	 * Do some quick & easy initial checks.  If any of these fail, we cannot
	 * use this constraint.
	 *
	 * 先做一些又快又简单的初步检查。任何一项失败，就不能用这个约束。
	 */
	if (fk->confrelid != parentConstr->confrelid || fk->nkeys != numfks)
	{
		ReleaseSysCache(parentConstrTup);
		return false;
	}
	for (int i = 0; i < numfks; i++)
	{
		if (fk->conkey[i] != mapped_conkey[i] ||
			fk->confkey[i] != confkey[i] ||
			fk->conpfeqop[i] != conpfeqop[i])
		{
			ReleaseSysCache(parentConstrTup);
			return false;
		}
	}

	/* Looks good so far; perform more extensive checks. */
	/*
	 *
	 * 到目前看起来不错；做更全面的检查。
	 */
	partcontup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
	if (!HeapTupleIsValid(partcontup))
		elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
	partConstr = (Form_pg_constraint) GETSTRUCT(partcontup);

	/*
	 * An error should be raised if the constraint enforceability is
	 * different. Returning false without raising an error, as we do for other
	 * attributes, could lead to a duplicate constraint with the same
	 * enforceability as the parent. While this may be acceptable, it may not
	 * be ideal. Therefore, it's better to raise an error and allow the user
	 * to correct the enforceability before proceeding.
	 *
	 * 若约束的可强制性不同，应报错。像其他属性那样不报错只返回假，可能导致出现一个与父约束可强制性相同的重复约束。这也许可以接受，
	 * 但并不理想。因此最好报错，让用户先改可强制性再继续。
	 */
	if (partConstr->conenforced != parentConstr->conenforced)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("constraint \"%s\" enforceability conflicts with constraint \"%s\" on relation \"%s\"",
						NameStr(parentConstr->conname),
						NameStr(partConstr->conname),
						RelationGetRelationName(partition))));

	if (OidIsValid(partConstr->conparentid) ||
		partConstr->condeferrable != parentConstr->condeferrable ||
		partConstr->condeferred != parentConstr->condeferred ||
		partConstr->confupdtype != parentConstr->confupdtype ||
		partConstr->confdeltype != parentConstr->confdeltype ||
		partConstr->confmatchtype != parentConstr->confmatchtype)
	{
		ReleaseSysCache(parentConstrTup);
		ReleaseSysCache(partcontup);
		return false;
	}

	ReleaseSysCache(parentConstrTup);
	ReleaseSysCache(partcontup);

	/* Looks good!  Attach this constraint. */
	/*
	 *
	 * 看起来不错！挂上这个约束。
	 */
	AttachPartitionForeignKey(wqueue, partition, fk->conoid,
							  parentConstrOid, parentInsTrigger,
							  parentUpdTrigger, trigrel);

	return true;
}

/*
 * AttachPartitionForeignKey
 *
 * 函数 AttachPartitionForeignKey
 *
 * The subroutine for tryAttachPartitionForeignKey performs the final tasks of
 * attaching the constraint, removing redundant triggers and entries from
 * pg_constraint, and setting the constraint's parent.
 *
 * tryAttachPartitionForeignKey 的子程序，完成挂接约束的最后工作：去掉多余的触发器和
 * pg_constraint 项，并设置约束的父约束。
 */
static void
AttachPartitionForeignKey(List **wqueue,
						  Relation partition,
						  Oid partConstrOid,
						  Oid parentConstrOid,
						  Oid parentInsTrigger,
						  Oid parentUpdTrigger,
						  Relation trigrel)
{
	HeapTuple	parentConstrTup;
	Form_pg_constraint parentConstr;
	HeapTuple	partcontup;
	Form_pg_constraint partConstr;
	bool		queueValidation;
	Oid			partConstrFrelid;
	Oid			partConstrRelid;
	bool		parentConstrIsEnforced;

	/* Fetch the parent constraint tuple */
	/*
	 *
	 * 取出父约束元组
	 */
	parentConstrTup = SearchSysCache1(CONSTROID,
									  ObjectIdGetDatum(parentConstrOid));
	if (!HeapTupleIsValid(parentConstrTup))
		elog(ERROR, "cache lookup failed for constraint %u", parentConstrOid);
	parentConstr = (Form_pg_constraint) GETSTRUCT(parentConstrTup);
	parentConstrIsEnforced = parentConstr->conenforced;

	/* Fetch the child constraint tuple */
	/*
	 *
	 * 取出子约束元组
	 */
	partcontup = SearchSysCache1(CONSTROID,
								 ObjectIdGetDatum(partConstrOid));
	if (!HeapTupleIsValid(partcontup))
		elog(ERROR, "cache lookup failed for constraint %u", partConstrOid);
	partConstr = (Form_pg_constraint) GETSTRUCT(partcontup);
	partConstrFrelid = partConstr->confrelid;
	partConstrRelid = partConstr->conrelid;

	/*
	 * If the referenced table is partitioned, then the partition we're
	 * attaching now has extra pg_constraint rows and action triggers that are
	 * no longer needed.  Remove those.
	 *
	 * 若被引用表是分区的，现在挂上的这个分区会有多余的 pg_constraint 行和动作触发器，不再需要。把它们去掉。
	 */
	if (get_rel_relkind(partConstrFrelid) == RELKIND_PARTITIONED_TABLE)
	{
		Relation	pg_constraint = table_open(ConstraintRelationId, RowShareLock);

		RemoveInheritedConstraint(pg_constraint, trigrel, partConstrOid,
								  partConstrRelid);

		table_close(pg_constraint, RowShareLock);
	}

	/*
	 * Will we need to validate this constraint?   A valid parent constraint
	 * implies that all child constraints have been validated, so if this one
	 * isn't, we must trigger phase 3 validation.
	 *
	 * 需要验证这个约束吗？有效的父约束意味着所有子约束都已验证，所以若这个还没验证，必须触发阶段 3 验证。
	 */
	queueValidation = parentConstr->convalidated && !partConstr->convalidated;

	ReleaseSysCache(partcontup);
	ReleaseSysCache(parentConstrTup);

	/*
	 * The action triggers in the new partition become redundant -- the parent
	 * table already has equivalent ones, and those will be able to reach the
	 * partition.  Remove the ones in the partition.  We identify them because
	 * they have our constraint OID, as well as being on the referenced rel.
	 *
	 * 新分区里的动作触发器变得多余：父表已经有等价的，而且那些能触及这个分区。去掉分区里的。我们靠它们带着我们的约束 OID、
	 * 并且位于被引用关系上来识别它们。
	 */
	DropForeignKeyConstraintTriggers(trigrel, partConstrOid, partConstrFrelid,
									 partConstrRelid);

	ConstraintSetParentConstraint(partConstrOid, parentConstrOid,
								  RelationGetRelid(partition));

	/*
	 * Like the constraint, attach partition's "check" triggers to the
	 * corresponding parent triggers if the constraint is ENFORCED. NOT
	 * ENFORCED constraints do not have these triggers.
	 *
	 * 和约束一样，若约束是 ENFORCED，把分区的检查触发器挂到对应的父触发器上。NOT ENFORCED 约束没有这些触发器。
	 */
	if (parentConstrIsEnforced)
	{
		Oid			insertTriggerOid,
					updateTriggerOid;

		GetForeignKeyCheckTriggers(trigrel,
								   partConstrOid, partConstrFrelid, partConstrRelid,
								   &insertTriggerOid, &updateTriggerOid);
		Assert(OidIsValid(insertTriggerOid) && OidIsValid(parentInsTrigger));
		TriggerSetParentTrigger(trigrel, insertTriggerOid, parentInsTrigger,
								RelationGetRelid(partition));
		Assert(OidIsValid(updateTriggerOid) && OidIsValid(parentUpdTrigger));
		TriggerSetParentTrigger(trigrel, updateTriggerOid, parentUpdTrigger,
								RelationGetRelid(partition));
	}

	/*
	 * We updated this pg_constraint row above to set its parent; validating
	 * it will cause its convalidated flag to change, so we need CCI here.  In
	 * addition, we need it unconditionally for the rare case where the parent
	 * table has *two* identical constraints; when reaching this function for
	 * the second one, we must have made our changes visible, otherwise we
	 * would try to attach both to this one.
	 *
	 * 上面我们已经更新了这行 pg_constraint 以设置其父约束；验证它会改变 convalidated 标志，所以这里需要
	 * CCI。另外，父表有两条完全相同的约束这种罕见情况也无条件需要它：处理第二条时，必须让我们的修改可见，
	 * 否则会试图把两条都挂到这一条上。
	 */
	CommandCounterIncrement();

	/* If validation is needed, put it in the queue now. */
	/*
	 *
	 * 若需要验证，现在就放进队列。
	 */
	if (queueValidation)
	{
		Relation	conrel;
		Oid			confrelid;

		conrel = table_open(ConstraintRelationId, RowExclusiveLock);

		partcontup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(partConstrOid));
		if (!HeapTupleIsValid(partcontup))
			elog(ERROR, "cache lookup failed for constraint %u", partConstrOid);

		confrelid = ((Form_pg_constraint) GETSTRUCT(partcontup))->confrelid;

		/* Use the same lock as for AT_ValidateConstraint */
		/*
		 *
		 * 使用与 AT_ValidateConstraint 相同的锁
		 */
		QueueFKConstraintValidation(wqueue, conrel, partition, confrelid,
									partcontup, ShareUpdateExclusiveLock);
		ReleaseSysCache(partcontup);
		table_close(conrel, RowExclusiveLock);
	}
}

/*
 * RemoveInheritedConstraint
 *
 * 函数 RemoveInheritedConstraint
 *
 * Removes the constraint and its associated trigger from the specified
 * relation, which inherited the given constraint.
 *
 * 从指定关系去掉该约束及其关联触发器；这个关系继承了给定约束。
 */
static void
RemoveInheritedConstraint(Relation conrel, Relation trigrel, Oid conoid,
						  Oid conrelid)
{
	ObjectAddresses *objs;
	HeapTuple	consttup;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	ScanKeyInit(&key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conrelid));

	scan = systable_beginscan(conrel,
							  ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, &key);
	objs = new_object_addresses();
	while ((consttup = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(consttup);

		if (conform->conparentid != conoid)
			continue;
		else
		{
			ObjectAddress addr;
			SysScanDesc scan2;
			ScanKeyData key2;
			int			n PG_USED_FOR_ASSERTS_ONLY;

			ObjectAddressSet(addr, ConstraintRelationId, conform->oid);
			add_exact_object_address(&addr, objs);

			/*
			 * First we must delete the dependency record that binds the
			 * constraint records together.
			 *
			 * 首先必须删除把这些约束记录绑在一起的依赖记录。
			 */
			n = deleteDependencyRecordsForSpecific(ConstraintRelationId,
												   conform->oid,
												   DEPENDENCY_INTERNAL,
												   ConstraintRelationId,
												   conoid);
			Assert(n == 1);		/* actually only one is expected */
						/*
						 *
						 * 实际上只指望有一个
						 */

			/*
			 * Now search for the triggers for this constraint and set them up
			 * for deletion too
			 *
			 * 现在搜索该约束的触发器，也把它们安排删除
			 */
			ScanKeyInit(&key2,
						Anum_pg_trigger_tgconstraint,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(conform->oid));
			scan2 = systable_beginscan(trigrel, TriggerConstraintIndexId,
									   true, NULL, 1, &key2);
			while ((trigtup = systable_getnext(scan2)) != NULL)
			{
				ObjectAddressSet(addr, TriggerRelationId,
								 ((Form_pg_trigger) GETSTRUCT(trigtup))->oid);
				add_exact_object_address(&addr, objs);
			}
			systable_endscan(scan2);
		}
	}
	/* make the dependency deletions visible */
	/*
	 *
	 * 让依赖删除可见
	 */
	CommandCounterIncrement();
	performMultipleDeletions(objs, DROP_RESTRICT,
							 PERFORM_DELETION_INTERNAL);
	systable_endscan(scan);
}

/*
 * DropForeignKeyConstraintTriggers
 *
 * 函数 DropForeignKeyConstraintTriggers
 *
 * The subroutine for tryAttachPartitionForeignKey handles the deletion of
 * action triggers for the foreign key constraint.
 *
 * tryAttachPartitionForeignKey 的子程序，负责删除外键约束的动作触发器。
 *
 * If valid confrelid and conrelid values are not provided, the respective
 * trigger check will be skipped, and the trigger will be considered for
 * removal.
 *
 * 若没有提供有效的 confrelid 和 conrelid，就跳过相应的触发器检查，并考虑去掉该触发器。
 */
static void
DropForeignKeyConstraintTriggers(Relation trigrel, Oid conoid, Oid confrelid,
								 Oid conrelid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));
	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trigger;

		/* Invalid if trigger is not for a referential integrity constraint */
		/*
		 *
		 * 若触发器不是为引用完整性约束服务的，则无效
		 */
		if (!OidIsValid(trgform->tgconstrrelid))
			continue;
		if (OidIsValid(conrelid) && trgform->tgconstrrelid != conrelid)
			continue;
		if (OidIsValid(confrelid) && trgform->tgrelid != confrelid)
			continue;

		/* We should be dropping trigger related to foreign key constraint */
		/*
		 *
		 * 我们应当删除与外键约束相关的触发器
		 */
		Assert(trgform->tgfoid == F_RI_FKEY_CHECK_INS ||
			   trgform->tgfoid == F_RI_FKEY_CHECK_UPD ||
			   trgform->tgfoid == F_RI_FKEY_CASCADE_DEL ||
			   trgform->tgfoid == F_RI_FKEY_CASCADE_UPD ||
			   trgform->tgfoid == F_RI_FKEY_RESTRICT_DEL ||
			   trgform->tgfoid == F_RI_FKEY_RESTRICT_UPD ||
			   trgform->tgfoid == F_RI_FKEY_SETNULL_DEL ||
			   trgform->tgfoid == F_RI_FKEY_SETNULL_UPD ||
			   trgform->tgfoid == F_RI_FKEY_SETDEFAULT_DEL ||
			   trgform->tgfoid == F_RI_FKEY_SETDEFAULT_UPD ||
			   trgform->tgfoid == F_RI_FKEY_NOACTION_DEL ||
			   trgform->tgfoid == F_RI_FKEY_NOACTION_UPD);

		/*
		 * The constraint is originally set up to contain this trigger as an
		 * implementation object, so there's a dependency record that links
		 * the two; however, since the trigger is no longer needed, we remove
		 * the dependency link in order to be able to drop the trigger while
		 * keeping the constraint intact.
		 *
		 * 约束最初把这个触发器设成实现对象，所以有一条把两者连起来的依赖记录；但触发器不再需要，我们去掉依赖链接，
		 * 以便在保留约束的同时删掉触发器。
		 */
		deleteDependencyRecordsFor(TriggerRelationId,
								   trgform->oid,
								   false);
		/* make dependency deletion visible to performDeletion */
		/*
		 *
		 * 让依赖删除对 performDeletion 可见
		 */
		CommandCounterIncrement();
		ObjectAddressSet(trigger, TriggerRelationId,
						 trgform->oid);
		performDeletion(&trigger, DROP_RESTRICT, 0);
		/* make trigger drop visible, in case the loop iterates */
		/*
		 *
		 * 让触发器删除可见，以防循环再转一圈
		 */
		CommandCounterIncrement();
	}

	systable_endscan(scan);
}

/*
 * GetForeignKeyActionTriggers
 * 		Returns delete and update "action" triggers of the given relation
 * 		belonging to the given constraint
 *
 * GetForeignKeyActionTriggers：返回给定关系上属于给定约束的删除和更新动作触发器
 */
static void
GetForeignKeyActionTriggers(Relation trigrel,
							Oid conoid, Oid confrelid, Oid conrelid,
							Oid *deleteTriggerOid,
							Oid *updateTriggerOid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	*deleteTriggerOid = *updateTriggerOid = InvalidOid;
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);

		if (trgform->tgconstrrelid != conrelid)
			continue;
		if (trgform->tgrelid != confrelid)
			continue;
		/* Only ever look at "action" triggers on the PK side. */
		/*
		 *
		 * 只看主键侧的动作触发器。
		 */
		if (RI_FKey_trigger_type(trgform->tgfoid) != RI_TRIGGER_PK)
			continue;
		if (TRIGGER_FOR_DELETE(trgform->tgtype))
		{
			Assert(*deleteTriggerOid == InvalidOid);
			*deleteTriggerOid = trgform->oid;
		}
		else if (TRIGGER_FOR_UPDATE(trgform->tgtype))
		{
			Assert(*updateTriggerOid == InvalidOid);
			*updateTriggerOid = trgform->oid;
		}
#ifndef USE_ASSERT_CHECKING
		/* In an assert-enabled build, continue looking to find duplicates */
		/*
		 *
		 * 在打开断言的构建里，继续找以发现重复
		 */
		if (OidIsValid(*deleteTriggerOid) && OidIsValid(*updateTriggerOid))
			break;
#endif
	}

	if (!OidIsValid(*deleteTriggerOid))
		elog(ERROR, "could not find ON DELETE action trigger of foreign key constraint %u",
			 conoid);
	if (!OidIsValid(*updateTriggerOid))
		elog(ERROR, "could not find ON UPDATE action trigger of foreign key constraint %u",
			 conoid);

	systable_endscan(scan);
}

/*
 * GetForeignKeyCheckTriggers
 * 		Returns insert and update "check" triggers of the given relation
 * 		belonging to the given constraint
 *
 * GetForeignKeyCheckTriggers：返回给定关系上属于给定约束的插入和更新检查触发器
 */
static void
GetForeignKeyCheckTriggers(Relation trigrel,
						   Oid conoid, Oid confrelid, Oid conrelid,
						   Oid *insertTriggerOid,
						   Oid *updateTriggerOid)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	trigtup;

	*insertTriggerOid = *updateTriggerOid = InvalidOid;
	ScanKeyInit(&key,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	scan = systable_beginscan(trigrel, TriggerConstraintIndexId, true,
							  NULL, 1, &key);
	while ((trigtup = systable_getnext(scan)) != NULL)
	{
		Form_pg_trigger trgform = (Form_pg_trigger) GETSTRUCT(trigtup);

		if (trgform->tgconstrrelid != confrelid)
			continue;
		if (trgform->tgrelid != conrelid)
			continue;
		/* Only ever look at "check" triggers on the FK side. */
		/*
		 *
		 * 只看外键侧的检查触发器。
		 */
		if (RI_FKey_trigger_type(trgform->tgfoid) != RI_TRIGGER_FK)
			continue;
		if (TRIGGER_FOR_INSERT(trgform->tgtype))
		{
			Assert(*insertTriggerOid == InvalidOid);
			*insertTriggerOid = trgform->oid;
		}
		else if (TRIGGER_FOR_UPDATE(trgform->tgtype))
		{
			Assert(*updateTriggerOid == InvalidOid);
			*updateTriggerOid = trgform->oid;
		}
#ifndef USE_ASSERT_CHECKING
		/* In an assert-enabled build, continue looking to find duplicates. */
		/*
		 *
		 * 在打开断言的构建里，继续找以发现重复。
		 */
		if (OidIsValid(*insertTriggerOid) && OidIsValid(*updateTriggerOid))
			break;
#endif
	}

	if (!OidIsValid(*insertTriggerOid))
		elog(ERROR, "could not find ON INSERT check triggers of foreign key constraint %u",
			 conoid);
	if (!OidIsValid(*updateTriggerOid))
		elog(ERROR, "could not find ON UPDATE check triggers of foreign key constraint %u",
			 conoid);

	systable_endscan(scan);
}

/*
 * ALTER TABLE ALTER CONSTRAINT
 *
 * ALTER TABLE ALTER CONSTRAINT（修改约束）
 *
 * Update the attributes of a constraint.
 *
 * 更新约束的属性。
 *
 * Currently only works for Foreign Key and not null constraints.
 *
 * 目前只对外键和 NOT NULL 约束有效。
 *
 * If the constraint is modified, returns its address; otherwise, return
 * InvalidObjectAddress.
 *
 * 若约束被修改，返回其地址；否则返回 InvalidObjectAddress。
 */
static ObjectAddress
ATExecAlterConstraint(List **wqueue, Relation rel, ATAlterConstraint *cmdcon,
					  bool recurse, LOCKMODE lockmode)
{
	Relation	conrel;
	Relation	tgrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	contuple;
	Form_pg_constraint currcon;
	ObjectAddress address;

	/*
	 * Disallow altering ONLY a partitioned table, as it would make no sense.
	 * This is okay for legacy inheritance.
	 *
	 * 不允许对分区表只改 ONLY，那样没意义。对传统继承则可以。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE && !recurse)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				errmsg("constraint must be altered in child tables too"),
				errhint("Do not specify the ONLY keyword."));


	conrel = table_open(ConstraintRelationId, RowExclusiveLock);
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find and check the target constraint
	 *
	 * 找到并检查目标约束
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(cmdcon->conname));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
	/*
	 *
	 * 最多只能有一行匹配
	 */
	if (!HeapTupleIsValid(contuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						cmdcon->conname, RelationGetRelationName(rel))));

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	if (cmdcon->alterDeferrability && currcon->contype != CONSTRAINT_FOREIGN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of relation \"%s\" is not a foreign key constraint",
						cmdcon->conname, RelationGetRelationName(rel))));
	if (cmdcon->alterEnforceability && currcon->contype != CONSTRAINT_FOREIGN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter enforceability of constraint \"%s\" of relation \"%s\"",
						cmdcon->conname, RelationGetRelationName(rel))));
	if (cmdcon->alterInheritability &&
		currcon->contype != CONSTRAINT_NOTNULL)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("constraint \"%s\" of relation \"%s\" is not a not-null constraint",
					   cmdcon->conname, RelationGetRelationName(rel)));

	/* Refuse to modify inheritability of inherited constraints */
	/*
	 *
	 * 拒绝修改继承来的约束的可继承性
	 */
	if (cmdcon->alterInheritability &&
		cmdcon->noinherit && currcon->coninhcount > 0)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot alter inherited constraint \"%s\" on relation \"%s\"",
					   NameStr(currcon->conname),
					   RelationGetRelationName(rel)));

	/*
	 * If it's not the topmost constraint, raise an error.
	 *
	 * 若它不是最顶层的约束，报错。
	 *
	 * Altering a non-topmost constraint leaves some triggers untouched, since
	 * they are not directly connected to this constraint; also, pg_dump would
	 * ignore the deferrability status of the individual constraint, since it
	 * only dumps topmost constraints.  Avoid these problems by refusing this
	 * operation and telling the user to alter the parent constraint instead.
	 *
	 * 修改非最顶层的约束会留下一些没碰到的触发器，因为它们并不直接连到这个约束；而且 pg_dump 会忽略单个约束的可推迟状态，
	 * 因为它只转储最顶层约束。为避免这些问题，拒绝这个操作，并告诉用户去改父约束。
	 */
	if (OidIsValid(currcon->conparentid))
	{
		HeapTuple	tp;
		Oid			parent = currcon->conparentid;
		char	   *ancestorname = NULL;
		char	   *ancestortable = NULL;

		/* Loop to find the topmost constraint */
		/*
		 *
		 * 循环找出最顶层的约束
		 */
		while (HeapTupleIsValid(tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(parent))))
		{
			Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);

			/* If no parent, this is the constraint we want */
			/*
			 *
			 * 若没有父约束，这就是我们要的约束
			 */
			if (!OidIsValid(contup->conparentid))
			{
				ancestorname = pstrdup(NameStr(contup->conname));
				ancestortable = get_rel_name(contup->conrelid);
				ReleaseSysCache(tp);
				break;
			}

			parent = contup->conparentid;
			ReleaseSysCache(tp);
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot alter constraint \"%s\" on relation \"%s\"",
						cmdcon->conname, RelationGetRelationName(rel)),
				 ancestorname && ancestortable ?
				 errdetail("Constraint \"%s\" is derived from constraint \"%s\" of relation \"%s\".",
						   cmdcon->conname, ancestorname, ancestortable) : 0,
				 errhint("You may alter the constraint it derives from instead.")));
	}

	address = InvalidObjectAddress;

	/*
	 * Do the actual catalog work, and recurse if necessary.
	 *
	 * 做实际的目录工作，必要时递归。
	 */
	if (ATExecAlterConstraintInternal(wqueue, cmdcon, conrel, tgrel, rel,
									  contuple, recurse, lockmode))
		ObjectAddressSet(address, ConstraintRelationId, currcon->oid);

	systable_endscan(scan);

	table_close(tgrel, RowExclusiveLock);
	table_close(conrel, RowExclusiveLock);

	return address;
}

/*
 * A subroutine of ATExecAlterConstraint that calls the respective routines for
 * altering constraint's enforceability, deferrability or inheritability.
 *
 * ATExecAlterConstraint 的子程序，分别调用修改约束可强制性、可推迟性或可继承性的例程。
 */
static bool
ATExecAlterConstraintInternal(List **wqueue, ATAlterConstraint *cmdcon,
							  Relation conrel, Relation tgrel, Relation rel,
							  HeapTuple contuple, bool recurse,
							  LOCKMODE lockmode)
{
	Form_pg_constraint currcon;
	bool		changed = false;
	List	   *otherrelids = NIL;

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);

	/*
	 * Do the catalog work for the enforceability or deferrability change,
	 * recurse if necessary.
	 *
	 * 为可强制性或可推迟性的变更做目录工作，必要时递归。
	 *
	 * Note that even if deferrability is requested to be altered along with
	 * enforceability, we don't need to explicitly update multiple entries in
	 * pg_trigger related to deferrability.
	 *
	 * 注意即使要求在改可强制性的同时改可推迟性，也不必显式更新 pg_trigger 里与可推迟性相关的多项。
	 *
	 * Modifying enforceability involves either creating or dropping the
	 * trigger, during which the deferrability setting will be adjusted
	 * automatically.
	 *
	 * 修改可强制性涉及创建或删除触发器，其间可推迟性设置会自动调整。
	 */
	if (cmdcon->alterEnforceability &&
		ATExecAlterConstrEnforceability(wqueue, cmdcon, conrel, tgrel,
										currcon->conrelid, currcon->confrelid,
										contuple, lockmode, InvalidOid,
										InvalidOid, InvalidOid, InvalidOid))
		changed = true;

	else if (cmdcon->alterDeferrability &&
			 ATExecAlterConstrDeferrability(wqueue, cmdcon, conrel, tgrel, rel,
											contuple, recurse, &otherrelids,
											lockmode))
	{
		/*
		 * AlterConstrUpdateConstraintEntry already invalidated relcache for
		 * the relations having the constraint itself; here we also invalidate
		 * for relations that have any triggers that are part of the
		 * constraint.
		 *
		 * AlterConstrUpdateConstraintEntry 已经使拥有该约束本身的关系的 relcache 失效；
		 * 这里也让拥有属于该约束的任何触发器的关系失效。
		 */
		foreach_oid(relid, otherrelids)
			CacheInvalidateRelcacheByRelid(relid);

		changed = true;
	}

	/*
	 * Do the catalog work for the inheritability change.
	 *
	 * 为可继承性的变更做目录工作。
	 */
	if (cmdcon->alterInheritability &&
		ATExecAlterConstrInheritability(wqueue, cmdcon, conrel, rel, contuple,
										lockmode))
		changed = true;

	return changed;
}

/*
 * Returns true if the constraint's enforceability is altered.
 *
 * 若约束的可强制性被改了，返回真。
 *
 * Depending on whether the constraint is being set to ENFORCED or NOT
 * ENFORCED, it creates or drops the trigger accordingly.
 *
 * 根据约束被设成 ENFORCED 还是 NOT ENFORCED，相应地创建或删除触发器。
 *
 * Note that we must recurse even when trying to change a constraint to not
 * enforced if it is already not enforced, in case descendant constraints
 * might be enforced and need to be changed to not enforced. Conversely, we
 * should do nothing if a constraint is being set to enforced and is already
 * enforced, as descendant constraints cannot be different in that case.
 *
 * 注意即使要把一个已经是 NOT ENFORCED 的约束改成 NOT ENFORCED，也必须递归，以防后代约束是
 * ENFORCED、需要改成 NOT ENFORCED。反过来，若要把约束设成 ENFORCED 而它已经是，就什么也不做，
 * 因为那种情况下后代约束不可能不同。
 */
static bool
ATExecAlterConstrEnforceability(List **wqueue, ATAlterConstraint *cmdcon,
								Relation conrel, Relation tgrel,
								Oid fkrelid, Oid pkrelid,
								HeapTuple contuple, LOCKMODE lockmode,
								Oid ReferencedParentDelTrigger,
								Oid ReferencedParentUpdTrigger,
								Oid ReferencingParentInsTrigger,
								Oid ReferencingParentUpdTrigger)
{
	Form_pg_constraint currcon;
	Oid			conoid;
	Relation	rel;
	bool		changed = false;

	/* Since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 本函数会递归，有可能被逼到栈溢出
	 */
	check_stack_depth();

	Assert(cmdcon->alterEnforceability);

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	conoid = currcon->oid;

	/* Should be foreign key constraint */
	/*
	 *
	 * 应当是外键约束
	 */
	Assert(currcon->contype == CONSTRAINT_FOREIGN);

	rel = table_open(currcon->conrelid, lockmode);

	if (currcon->conenforced != cmdcon->is_enforced)
	{
		AlterConstrUpdateConstraintEntry(cmdcon, conrel, contuple);
		changed = true;
	}

	/* Drop triggers */
	/*
	 *
	 * 删除触发器
	 */
	if (!cmdcon->is_enforced)
	{
		/*
		 * When setting a constraint to NOT ENFORCED, the constraint triggers
		 * need to be dropped. Therefore, we must process the child relations
		 * first, followed by the parent, to account for dependencies.
		 *
		 * 把约束设成 NOT ENFORCED 时需要删除约束触发器。因此必须先处理子关系，再处理父关系，以顾及依赖。
		 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
			get_rel_relkind(currcon->confrelid) == RELKIND_PARTITIONED_TABLE)
			AlterConstrEnforceabilityRecurse(wqueue, cmdcon, conrel, tgrel,
											 fkrelid, pkrelid, contuple,
											 lockmode, InvalidOid, InvalidOid,
											 InvalidOid, InvalidOid);

		/* Drop all the triggers */
		/*
		 *
		 * 删掉所有触发器
		 */
		DropForeignKeyConstraintTriggers(tgrel, conoid, InvalidOid, InvalidOid);
	}
	else if (changed)			/* Create triggers */
						/*
						 *
						 * 创建触发器
						 */
	{
		Oid			ReferencedDelTriggerOid = InvalidOid,
					ReferencedUpdTriggerOid = InvalidOid,
					ReferencingInsTriggerOid = InvalidOid,
					ReferencingUpdTriggerOid = InvalidOid;

		/* Prepare the minimal information required for trigger creation. */
		/*
		 *
		 * 准备创建触发器所需的最少信息。
		 */
		Constraint *fkconstraint = makeNode(Constraint);

		fkconstraint->conname = pstrdup(NameStr(currcon->conname));
		fkconstraint->fk_matchtype = currcon->confmatchtype;
		fkconstraint->fk_upd_action = currcon->confupdtype;
		fkconstraint->fk_del_action = currcon->confdeltype;

		/* Create referenced triggers */
		/*
		 *
		 * 创建被引用侧的触发器
		 */
		if (currcon->conrelid == fkrelid)
			createForeignKeyActionTriggers(currcon->conrelid,
										   currcon->confrelid,
										   fkconstraint,
										   conoid,
										   currcon->conindid,
										   ReferencedParentDelTrigger,
										   ReferencedParentUpdTrigger,
										   &ReferencedDelTriggerOid,
										   &ReferencedUpdTriggerOid);

		/* Create referencing triggers */
		/*
		 *
		 * 创建引用侧的触发器
		 */
		if (currcon->confrelid == pkrelid)
			createForeignKeyCheckTriggers(currcon->conrelid,
										  pkrelid,
										  fkconstraint,
										  conoid,
										  currcon->conindid,
										  ReferencingParentInsTrigger,
										  ReferencingParentUpdTrigger,
										  &ReferencingInsTriggerOid,
										  &ReferencingUpdTriggerOid);

		/*
		 * Tell Phase 3 to check that the constraint is satisfied by existing
		 * rows.  Only applies to leaf partitions, and (for constraints that
		 * reference a partitioned table) only if this is not one of the
		 * pg_constraint rows that exist solely to support action triggers.
		 *
		 * 告诉 Phase 3 检查现有行是否满足该约束。只适用于叶子分区，并且（对引用分区表的约束）
		 * 只在这不是那种仅为支持动作触发器而存在的 pg_constraint 行时才适用。
		 */
		if (rel->rd_rel->relkind == RELKIND_RELATION &&
			currcon->confrelid == pkrelid)
		{
			AlteredTableInfo *tab;
			NewConstraint *newcon;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = fkconstraint->conname;
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = currcon->confrelid;
			newcon->refindid = currcon->conindid;
			newcon->conid = currcon->oid;
			newcon->qual = (Node *) fkconstraint;

			/* Find or create work queue entry for this table */
			/*
			 *
			 * 查找或创建这张表的工作队列项
			 */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);
		}

		/*
		 * If the table at either end of the constraint is partitioned, we
		 * need to recurse and create triggers for each constraint that is a
		 * child of this one.
		 *
		 * 若约束任一端的表是分区的，需要递归并为这个约束的每个子约束创建触发器。
		 */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
			get_rel_relkind(currcon->confrelid) == RELKIND_PARTITIONED_TABLE)
			AlterConstrEnforceabilityRecurse(wqueue, cmdcon, conrel, tgrel,
											 fkrelid, pkrelid, contuple,
											 lockmode, ReferencedDelTriggerOid,
											 ReferencedUpdTriggerOid,
											 ReferencingInsTriggerOid,
											 ReferencingUpdTriggerOid);
	}

	table_close(rel, NoLock);

	return changed;
}

/*
 * Returns true if the constraint's deferrability is altered.
 *
 * 若约束的可推迟性被改了，返回真。
 *
 * *otherrelids is appended OIDs of relations containing affected triggers.
 *
 * 把含有受影响触发器的关系 OID 追加到 otherrelids。
 *
 * Note that we must recurse even when the values are correct, in case
 * indirect descendants have had their constraints altered locally.
 * (This could be avoided if we forbade altering constraints in partitions
 * but existing releases don't do that.)
 *
 * 注意即使值已经正确也必须递归，以防间接后代在本地改过自己的约束。（若禁止在分区上改约束就可以避免，但现有版本并不禁止。）
 */
static bool
ATExecAlterConstrDeferrability(List **wqueue, ATAlterConstraint *cmdcon,
							   Relation conrel, Relation tgrel, Relation rel,
							   HeapTuple contuple, bool recurse,
							   List **otherrelids, LOCKMODE lockmode)
{
	Form_pg_constraint currcon;
	Oid			refrelid;
	bool		changed = false;

	/* since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 本函数会递归，有可能被逼到栈溢出
	 */
	check_stack_depth();

	Assert(cmdcon->alterDeferrability);

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	refrelid = currcon->confrelid;

	/* Should be foreign key constraint */
	/*
	 *
	 * 应当是外键约束
	 */
	Assert(currcon->contype == CONSTRAINT_FOREIGN);

	/*
	 * If called to modify a constraint that's already in the desired state,
	 * silently do nothing.
	 *
	 * 若被调用来修改一个已经处于期望状态的约束，悄悄什么也不做。
	 */
	if (currcon->condeferrable != cmdcon->deferrable ||
		currcon->condeferred != cmdcon->initdeferred)
	{
		AlterConstrUpdateConstraintEntry(cmdcon, conrel, contuple);
		changed = true;

		/*
		 * Now we need to update the multiple entries in pg_trigger that
		 * implement the constraint.
		 *
		 * 现在需要更新实现该约束的多条 pg_trigger 项。
		 */
		AlterConstrTriggerDeferrability(currcon->oid, tgrel, rel,
										cmdcon->deferrable,
										cmdcon->initdeferred, otherrelids);
	}

	/*
	 * If the table at either end of the constraint is partitioned, we need to
	 * handle every constraint that is a child of this one.
	 *
	 * 若约束任一端的表是分区的，需要处理这个约束的每个子约束。
	 */
	if (recurse && changed &&
		(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
		 get_rel_relkind(refrelid) == RELKIND_PARTITIONED_TABLE))
		AlterConstrDeferrabilityRecurse(wqueue, cmdcon, conrel, tgrel, rel,
										contuple, recurse, otherrelids,
										lockmode);

	return changed;
}

/*
 * Returns true if the constraint's inheritability is altered.
 *
 * 若约束的可继承性被改了，返回真。
 */
static bool
ATExecAlterConstrInheritability(List **wqueue, ATAlterConstraint *cmdcon,
								Relation conrel, Relation rel,
								HeapTuple contuple, LOCKMODE lockmode)
{
	Form_pg_constraint currcon;
	AttrNumber	colNum;
	char	   *colName;
	List	   *children;

	Assert(cmdcon->alterInheritability);

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);

	/* The current implementation only works for NOT NULL constraints */
	/*
	 *
	 * 当前实现只对 NOT NULL 约束有效
	 */
	Assert(currcon->contype == CONSTRAINT_NOTNULL);

	/*
	 * If called to modify a constraint that's already in the desired state,
	 * silently do nothing.
	 *
	 * 若被调用来修改一个已经处于期望状态的约束，悄悄什么也不做。
	 */
	if (cmdcon->noinherit == currcon->connoinherit)
		return false;

	AlterConstrUpdateConstraintEntry(cmdcon, conrel, contuple);
	CommandCounterIncrement();

	/* Fetch the column number and name */
	/*
	 *
	 * 取出列号和列名
	 */
	colNum = extractNotNullColumn(contuple);
	colName = get_attname(currcon->conrelid, colNum, false);

	/*
	 * Propagate the change to children.  For this subcommand type we don't
	 * recursively affect children, just the immediate level.
	 *
	 * 把变更传播到子表。对这种子命令类型，我们不递归影响所有后代，只影响直接下一层。
	 */
	children = find_inheritance_children(RelationGetRelid(rel),
										 lockmode);
	foreach_oid(childoid, children)
	{
		ObjectAddress addr;

		if (cmdcon->noinherit)
		{
			HeapTuple	childtup;
			Form_pg_constraint childcon;

			childtup = findNotNullConstraint(childoid, colName);
			if (!childtup)
				elog(ERROR, "cache lookup failed for not-null constraint on column \"%s\" of relation %u",
					 colName, childoid);
			childcon = (Form_pg_constraint) GETSTRUCT(childtup);
			Assert(childcon->coninhcount > 0);
			childcon->coninhcount--;
			childcon->conislocal = true;
			CatalogTupleUpdate(conrel, &childtup->t_self, childtup);
			heap_freetuple(childtup);
		}
		else
		{
			Relation	childrel = table_open(childoid, NoLock);

			addr = ATExecSetNotNull(wqueue, childrel, NameStr(currcon->conname),
									colName, true, true, lockmode);
			if (OidIsValid(addr.objectId))
				CommandCounterIncrement();
			table_close(childrel, NoLock);
		}
	}

	return true;
}

/*
 * A subroutine of ATExecAlterConstrDeferrability that updated constraint
 * trigger's deferrability.
 *
 * ATExecAlterConstrDeferrability 的子程序，更新约束触发器的可推迟性。
 *
 * The arguments to this function have the same meaning as the arguments to
 * ATExecAlterConstrDeferrability.
 *
 * 本函数的参数含义与 ATExecAlterConstrDeferrability 的参数相同。
 */
static void
AlterConstrTriggerDeferrability(Oid conoid, Relation tgrel, Relation rel,
								bool deferrable, bool initdeferred,
								List **otherrelids)
{
	HeapTuple	tgtuple;
	ScanKeyData tgkey;
	SysScanDesc tgscan;

	ScanKeyInit(&tgkey,
				Anum_pg_trigger_tgconstraint,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));
	tgscan = systable_beginscan(tgrel, TriggerConstraintIndexId, true,
								NULL, 1, &tgkey);
	while (HeapTupleIsValid(tgtuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger tgform = (Form_pg_trigger) GETSTRUCT(tgtuple);
		Form_pg_trigger copy_tg;
		HeapTuple	tgCopyTuple;

		/*
		 * Remember OIDs of other relation(s) involved in FK constraint.
		 * (Note: it's likely that we could skip forcing a relcache inval for
		 * other rels that don't have a trigger whose properties change, but
		 * let's be conservative.)
		 *
		 * 记住外键约束涉及的其他关系的 OID。（注意：对那些没有属性发生变化的触发器的其他关系，也许可以跳过强制 relcache 失效，
		 * 但还是保守一点。）
		 */
		if (tgform->tgrelid != RelationGetRelid(rel))
			*otherrelids = list_append_unique_oid(*otherrelids,
												  tgform->tgrelid);

		/*
		 * Update enable status and deferrability of RI_FKey_noaction_del,
		 * RI_FKey_noaction_upd, RI_FKey_check_ins and RI_FKey_check_upd
		 * triggers, but not others; see createForeignKeyActionTriggers and
		 * CreateFKCheckTrigger.
		 *
		 * 更新 RI_FKey_noaction_del、RI_FKey_noaction_upd、RI_FKey_check_ins 和
		 * RI_FKey_check_upd 触发器的启用状态和可推迟性，但不更新其他触发器；见
		 * createForeignKeyActionTriggers 和 CreateFKCheckTrigger。
		 */
		if (tgform->tgfoid != F_RI_FKEY_NOACTION_DEL &&
			tgform->tgfoid != F_RI_FKEY_NOACTION_UPD &&
			tgform->tgfoid != F_RI_FKEY_CHECK_INS &&
			tgform->tgfoid != F_RI_FKEY_CHECK_UPD)
			continue;

		tgCopyTuple = heap_copytuple(tgtuple);
		copy_tg = (Form_pg_trigger) GETSTRUCT(tgCopyTuple);

		copy_tg->tgdeferrable = deferrable;
		copy_tg->tginitdeferred = initdeferred;
		CatalogTupleUpdate(tgrel, &tgCopyTuple->t_self, tgCopyTuple);

		InvokeObjectPostAlterHook(TriggerRelationId, tgform->oid, 0);

		heap_freetuple(tgCopyTuple);
	}

	systable_endscan(tgscan);
}

/*
 * Invokes ATExecAlterConstrEnforceability for each constraint that is a child of
 * the specified constraint.
 *
 * 对指定约束的每个子约束调用 ATExecAlterConstrEnforceability。
 *
 * Note that this doesn't handle recursion the normal way, viz. by scanning the
 * list of child relations and recursing; instead it uses the conparentid
 * relationships.  This may need to be reconsidered.
 *
 * 注意这里不按常规方式处理递归，也就是不扫描子关系列表再递归；而是用 conparentid 关系。这一点也许需要重新考虑。
 *
 * The arguments to this function have the same meaning as the arguments to
 * ATExecAlterConstrEnforceability.
 *
 * 本函数的参数含义与 ATExecAlterConstrEnforceability 的参数相同。
 */
static void
AlterConstrEnforceabilityRecurse(List **wqueue, ATAlterConstraint *cmdcon,
								 Relation conrel, Relation tgrel,
								 Oid fkrelid, Oid pkrelid,
								 HeapTuple contuple, LOCKMODE lockmode,
								 Oid ReferencedParentDelTrigger,
								 Oid ReferencedParentUpdTrigger,
								 Oid ReferencingParentInsTrigger,
								 Oid ReferencingParentUpdTrigger)
{
	Form_pg_constraint currcon;
	Oid			conoid;
	ScanKeyData pkey;
	SysScanDesc pscan;
	HeapTuple	childtup;

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	conoid = currcon->oid;

	ScanKeyInit(&pkey,
				Anum_pg_constraint_conparentid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	pscan = systable_beginscan(conrel, ConstraintParentIndexId,
							   true, NULL, 1, &pkey);

	while (HeapTupleIsValid(childtup = systable_getnext(pscan)))
		ATExecAlterConstrEnforceability(wqueue, cmdcon, conrel, tgrel, fkrelid,
										pkrelid, childtup, lockmode,
										ReferencedParentDelTrigger,
										ReferencedParentUpdTrigger,
										ReferencingParentInsTrigger,
										ReferencingParentUpdTrigger);

	systable_endscan(pscan);
}

/*
 * Invokes ATExecAlterConstrDeferrability for each constraint that is a child of
 * the specified constraint.
 *
 * 对指定约束的每个子约束调用 ATExecAlterConstrDeferrability。
 *
 * Note that this doesn't handle recursion the normal way, viz. by scanning the
 * list of child relations and recursing; instead it uses the conparentid
 * relationships.  This may need to be reconsidered.
 *
 * 注意这里不按常规方式处理递归，也就是不扫描子关系列表再递归；而是用 conparentid 关系。这一点也许需要重新考虑。
 *
 * The arguments to this function have the same meaning as the arguments to
 * ATExecAlterConstrDeferrability.
 *
 * 本函数的参数含义与 ATExecAlterConstrDeferrability 的参数相同。
 */
static void
AlterConstrDeferrabilityRecurse(List **wqueue, ATAlterConstraint *cmdcon,
								Relation conrel, Relation tgrel, Relation rel,
								HeapTuple contuple, bool recurse,
								List **otherrelids, LOCKMODE lockmode)
{
	Form_pg_constraint currcon;
	Oid			conoid;
	ScanKeyData pkey;
	SysScanDesc pscan;
	HeapTuple	childtup;

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	conoid = currcon->oid;

	ScanKeyInit(&pkey,
				Anum_pg_constraint_conparentid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(conoid));

	pscan = systable_beginscan(conrel, ConstraintParentIndexId,
							   true, NULL, 1, &pkey);

	while (HeapTupleIsValid(childtup = systable_getnext(pscan)))
	{
		Form_pg_constraint childcon = (Form_pg_constraint) GETSTRUCT(childtup);
		Relation	childrel;

		childrel = table_open(childcon->conrelid, lockmode);

		ATExecAlterConstrDeferrability(wqueue, cmdcon, conrel, tgrel, childrel,
									   childtup, recurse, otherrelids, lockmode);
		table_close(childrel, NoLock);
	}

	systable_endscan(pscan);
}

/*
 * Update the constraint entry for the given ATAlterConstraint command, and
 * invoke the appropriate hooks.
 *
 * 按给定的 ATAlterConstraint 命令更新约束项，并调用相应的钩子。
 */
static void
AlterConstrUpdateConstraintEntry(ATAlterConstraint *cmdcon, Relation conrel,
								 HeapTuple contuple)
{
	HeapTuple	copyTuple;
	Form_pg_constraint copy_con;

	Assert(cmdcon->alterEnforceability || cmdcon->alterDeferrability ||
		   cmdcon->alterInheritability);

	copyTuple = heap_copytuple(contuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);

	if (cmdcon->alterEnforceability)
	{
		copy_con->conenforced = cmdcon->is_enforced;

		/*
		 * NB: The convalidated status is irrelevant when the constraint is
		 * set to NOT ENFORCED, but for consistency, it should still be set
		 * appropriately. Similarly, if the constraint is later changed to
		 * ENFORCED, validation will be performed during phase 3, so it makes
		 * sense to mark it as valid in that case.
		 *
		 * 注意：约束设成 NOT ENFORCED 时 convalidated 状态无关紧要，但为了一致仍应适当设置。类似地，若以后改成
		 * ENFORCED，验证会在阶段 3 进行，所以那种情况下把它标成有效是合理的。
		 */
		copy_con->convalidated = cmdcon->is_enforced;
	}
	if (cmdcon->alterDeferrability)
	{
		copy_con->condeferrable = cmdcon->deferrable;
		copy_con->condeferred = cmdcon->initdeferred;
	}
	if (cmdcon->alterInheritability)
		copy_con->connoinherit = cmdcon->noinherit;

	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);
	InvokeObjectPostAlterHook(ConstraintRelationId, copy_con->oid, 0);

	/* Make new constraint flags visible to others */
	/*
	 *
	 * 让新的约束标志对别人可见
	 */
	CacheInvalidateRelcacheByRelid(copy_con->conrelid);

	heap_freetuple(copyTuple);
}

/*
 * ALTER TABLE VALIDATE CONSTRAINT
 *
 * ALTER TABLE VALIDATE CONSTRAINT（验证约束）
 *
 * XXX The reason we handle recursion here rather than at Phase 1 is because
 * there's no good way to skip recursing when handling foreign keys: there is
 * no need to lock children in that case, yet we wouldn't be able to avoid
 * doing so at that level.
 *
 * XXX 我们在这里而不是在 Phase 1 处理递归，是因为处理外键时没有好办法跳过递归：那种情况不必锁子表，
 * 但在那一层我们无法避免去锁。
 *
 * Return value is the address of the validated constraint.  If the constraint
 * was already validated, InvalidObjectAddress is returned.
 *
 * 返回值是已验证约束的地址。若约束早已验证，返回 InvalidObjectAddress。
 */
static ObjectAddress
ATExecValidateConstraint(List **wqueue, Relation rel, char *constrName,
						 bool recurse, bool recursing, LOCKMODE lockmode)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	tuple;
	Form_pg_constraint con;
	ObjectAddress address;

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and check the target constraint
	 *
	 * 找到并检查目标约束
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
	/*
	 *
	 * 最多只能有一行匹配
	 */
	if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						constrName, RelationGetRelationName(rel))));

	con = (Form_pg_constraint) GETSTRUCT(tuple);
	if (con->contype != CONSTRAINT_FOREIGN &&
		con->contype != CONSTRAINT_CHECK &&
		con->contype != CONSTRAINT_NOTNULL)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot validate constraint \"%s\" of relation \"%s\"",
					   constrName, RelationGetRelationName(rel)),
				errdetail("This operation is not supported for this type of constraint."));

	if (!con->conenforced)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot validate NOT ENFORCED constraint")));

	if (!con->convalidated)
	{
		if (con->contype == CONSTRAINT_FOREIGN)
		{
			QueueFKConstraintValidation(wqueue, conrel, rel, con->confrelid,
										tuple, lockmode);
		}
		else if (con->contype == CONSTRAINT_CHECK)
		{
			QueueCheckConstraintValidation(wqueue, conrel, rel, constrName,
										   tuple, recurse, recursing, lockmode);
		}
		else if (con->contype == CONSTRAINT_NOTNULL)
		{
			QueueNNConstraintValidation(wqueue, conrel, rel,
										tuple, recurse, recursing, lockmode);
		}

		ObjectAddressSet(address, ConstraintRelationId, con->oid);
	}
	else
		address = InvalidObjectAddress; /* already validated */
						/*
						 *
						 * 已经验证过
						 */

	systable_endscan(scan);

	table_close(conrel, RowExclusiveLock);

	return address;
}

/*
 * QueueFKConstraintValidation
 *
 * 函数 QueueFKConstraintValidation
 *
 * Add an entry to the wqueue to validate the given foreign key constraint in
 * Phase 3 and update the convalidated field in the pg_constraint catalog
 * for the specified relation and all its children.
 *
 * 往 wqueue 加一项，以便在 Phase 3 验证给定外键约束，并更新指定关系及其所有子关系在 pg_constraint
 * 目录中的 convalidated 字段。
 */
static void
QueueFKConstraintValidation(List **wqueue, Relation conrel, Relation fkrel,
							Oid pkrelid, HeapTuple contuple, LOCKMODE lockmode)
{
	Form_pg_constraint con;
	AlteredTableInfo *tab;
	HeapTuple	copyTuple;
	Form_pg_constraint copy_con;

	con = (Form_pg_constraint) GETSTRUCT(contuple);
	Assert(con->contype == CONSTRAINT_FOREIGN);
	Assert(!con->convalidated);

	/*
	 * Add the validation to phase 3's queue; not needed for partitioned
	 * tables themselves, only for their partitions.
	 *
	 * 把验证加入阶段 3 的队列；分区表本身不需要，只需要它们的分区。
	 *
	 * When the referenced table (pkrelid) is partitioned, the referencing
	 * table (fkrel) has one pg_constraint row pointing to each partition
	 * thereof.  These rows are there only to support action triggers and no
	 * table scan is needed, therefore skip this for them as well.
	 *
	 * 当被引用表（pkrelid）是分区的时，引用表（fkrel）对它的每个分区都有一行 pg_constraint。
	 * 这些行只为支持动作触发器而存在，不需要扫表，因此也跳过它们。
	 */
	if (fkrel->rd_rel->relkind == RELKIND_RELATION &&
		con->confrelid == pkrelid)
	{
		NewConstraint *newcon;
		Constraint *fkconstraint;

		/* Queue validation for phase 3 */
		/*
		 *
		 * 为阶段 3 排队验证
		 */
		fkconstraint = makeNode(Constraint);
		/* for now this is all we need */
		/*
		 *
		 * 目前我们只需要这些
		 */
		fkconstraint->conname = pstrdup(NameStr(con->conname));

		newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
		newcon->name = fkconstraint->conname;
		newcon->contype = CONSTR_FOREIGN;
		newcon->refrelid = con->confrelid;
		newcon->refindid = con->conindid;
		newcon->conid = con->oid;
		newcon->qual = (Node *) fkconstraint;

		/* Find or create work queue entry for this table */
		/*
		 *
		 * 查找或创建这张表的工作队列项
		 */
		tab = ATGetQueueEntry(wqueue, fkrel);
		tab->constraints = lappend(tab->constraints, newcon);
	}

	/*
	 * If the table at either end of the constraint is partitioned, we need to
	 * recurse and handle every unvalidate constraint that is a child of this
	 * constraint.
	 *
	 * 若约束任一端的表是分区的，需要递归并处理这个约束的每个尚未验证的子约束。
	 */
	if (fkrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
		get_rel_relkind(con->confrelid) == RELKIND_PARTITIONED_TABLE)
	{
		ScanKeyData pkey;
		SysScanDesc pscan;
		HeapTuple	childtup;

		ScanKeyInit(&pkey,
					Anum_pg_constraint_conparentid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(con->oid));

		pscan = systable_beginscan(conrel, ConstraintParentIndexId,
								   true, NULL, 1, &pkey);

		while (HeapTupleIsValid(childtup = systable_getnext(pscan)))
		{
			Form_pg_constraint childcon;
			Relation	childrel;

			childcon = (Form_pg_constraint) GETSTRUCT(childtup);

			/*
			 * If the child constraint has already been validated, no further
			 * action is required for it or its descendants, as they are all
			 * valid.
			 *
			 * 若子约束已经验证，它和它的后代都不需要进一步动作，因为它们全都有效。
			 */
			if (childcon->convalidated)
				continue;

			childrel = table_open(childcon->conrelid, lockmode);

			/*
			 * NB: Note that pkrelid should be passed as-is during recursion,
			 * as it is required to identify the root referenced table.
			 *
			 * 注意：递归时 pkrelid 应原样传递，因为需要它来识别根被引用表。
			 */
			QueueFKConstraintValidation(wqueue, conrel, childrel, pkrelid,
										childtup, lockmode);
			table_close(childrel, NoLock);
		}

		systable_endscan(pscan);
	}

	/*
	 * Now mark the pg_constraint row as validated (even if we didn't check,
	 * notably the ones for partitions on the referenced side).
	 *
	 * 现在把 pg_constraint 行标成已验证（即使我们没检查，尤其是被引用侧分区上的那些）。
	 *
	 * We rely on transaction abort to roll back this change if phase 3
	 * ultimately finds violating rows.  This is a bit ugly.
	 *
	 * 若阶段 3 最终发现违规行，我们靠事务中止来回滚这次修改。有点难看。
	 */
	copyTuple = heap_copytuple(contuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
	copy_con->convalidated = true;
	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

	heap_freetuple(copyTuple);
}

/*
 * QueueCheckConstraintValidation
 *
 * 函数 QueueCheckConstraintValidation
 *
 * Add an entry to the wqueue to validate the given check constraint in Phase 3
 * and update the convalidated field in the pg_constraint catalog for the
 * specified relation and all its inheriting children.
 *
 * 往 wqueue 加一项，以便在 Phase 3 验证给定 CHECK 约束，并更新指定关系及其所有继承子关系在
 * pg_constraint 目录中的 convalidated 字段。
 */
static void
QueueCheckConstraintValidation(List **wqueue, Relation conrel, Relation rel,
							   char *constrName, HeapTuple contuple,
							   bool recurse, bool recursing, LOCKMODE lockmode)
{
	Form_pg_constraint con;
	AlteredTableInfo *tab;
	HeapTuple	copyTuple;
	Form_pg_constraint copy_con;

	List	   *children = NIL;
	ListCell   *child;
	NewConstraint *newcon;
	Datum		val;
	char	   *conbin;

	con = (Form_pg_constraint) GETSTRUCT(contuple);
	Assert(con->contype == CONSTRAINT_CHECK);

	/*
	 * If we're recursing, the parent has already done this, so skip it. Also,
	 * if the constraint is a NO INHERIT constraint, we shouldn't try to look
	 * for it in the children.
	 *
	 * 若正在递归，父表已经做过，就跳过。另外，若约束是 NO INHERIT，不应到子表里去找它。
	 */
	if (!recursing && !con->connoinherit)
		children = find_all_inheritors(RelationGetRelid(rel),
									   lockmode, NULL);

	/*
	 * For CHECK constraints, we must ensure that we only mark the constraint
	 * as validated on the parent if it's already validated on the children.
	 *
	 * 对 CHECK 约束，必须确保只有在子表上已经验证之后，才把父表上的约束标成已验证。
	 *
	 * We recurse before validating on the parent, to reduce risk of
	 * deadlocks.
	 *
	 * 我们在父表上验证之前先递归，以降低死锁风险。
	 */
	foreach(child, children)
	{
		Oid			childoid = lfirst_oid(child);
		Relation	childrel;

		if (childoid == RelationGetRelid(rel))
			continue;

		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables, because we can't mark the constraint on the parent valid
		 * unless it is valid for all child tables.
		 *
		 * 若被告知不要递归，最好没有子表，因为除非约束对所有子表都有效，否则不能把父表上的约束标成有效。
		 */
		if (!recurse)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("constraint must be validated on child tables too")));

		/* find_all_inheritors already got lock */
		/*
		 *
		 * find_all_inheritors 已经拿到锁
		 */
		childrel = table_open(childoid, NoLock);

		ATExecValidateConstraint(wqueue, childrel, constrName, false,
								 true, lockmode);
		table_close(childrel, NoLock);
	}

	/* Queue validation for phase 3 */
	/*
	 *
	 * 为阶段 3 排队验证
	 */
	newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
	newcon->name = constrName;
	newcon->contype = CONSTR_CHECK;
	newcon->refrelid = InvalidOid;
	newcon->refindid = InvalidOid;
	newcon->conid = con->oid;

	val = SysCacheGetAttrNotNull(CONSTROID, contuple,
								 Anum_pg_constraint_conbin);
	conbin = TextDatumGetCString(val);
	newcon->qual = expand_generated_columns_in_expr(stringToNode(conbin), rel, 1);

	/* Find or create work queue entry for this table */
	/*
	 *
	 * 查找或创建这张表的工作队列项
	 */
	tab = ATGetQueueEntry(wqueue, rel);
	tab->constraints = lappend(tab->constraints, newcon);

	/*
	 * Invalidate relcache so that others see the new validated constraint.
	 *
	 * 使 relcache 失效，以便别人看见新的已验证约束。
	 */
	CacheInvalidateRelcache(rel);

	/*
	 * Now update the catalog, while we have the door open.
	 *
	 * 趁门开着，现在更新目录。
	 */
	copyTuple = heap_copytuple(contuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
	copy_con->convalidated = true;
	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

	heap_freetuple(copyTuple);
}

/*
 * QueueNNConstraintValidation
 *
 * 函数 QueueNNConstraintValidation
 *
 * Add an entry to the wqueue to validate the given not-null constraint in
 * Phase 3 and update the convalidated field in the pg_constraint catalog for
 * the specified relation and all its inheriting children.
 *
 * 往 wqueue 加一项，以便在 Phase 3 验证给定 NOT NULL 约束，并更新指定关系及其所有继承子关系在
 * pg_constraint 目录中的 convalidated 字段。
 */
static void
QueueNNConstraintValidation(List **wqueue, Relation conrel, Relation rel,
							HeapTuple contuple, bool recurse, bool recursing,
							LOCKMODE lockmode)
{
	Form_pg_constraint con;
	AlteredTableInfo *tab;
	HeapTuple	copyTuple;
	Form_pg_constraint copy_con;
	List	   *children = NIL;
	AttrNumber	attnum;
	char	   *colname;

	con = (Form_pg_constraint) GETSTRUCT(contuple);
	Assert(con->contype == CONSTRAINT_NOTNULL);

	attnum = extractNotNullColumn(contuple);

	/*
	 * If we're recursing, we've already done this for parent, so skip it.
	 * Also, if the constraint is a NO INHERIT constraint, we shouldn't try to
	 * look for it in the children.
	 *
	 * 若正在递归，父表已经做过，就跳过。另外，若约束是 NO INHERIT，不应到子表里去找它。
	 *
	 * We recurse before validating on the parent, to reduce risk of
	 * deadlocks.
	 *
	 * 我们在父表上验证之前先递归，以降低死锁风险。
	 */
	if (!recursing && !con->connoinherit)
		children = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);

	colname = get_attname(RelationGetRelid(rel), attnum, false);
	foreach_oid(childoid, children)
	{
		Relation	childrel;
		HeapTuple	contup;
		Form_pg_constraint childcon;
		char	   *conname;

		if (childoid == RelationGetRelid(rel))
			continue;

		/*
		 * If we are told not to recurse, there had better not be any child
		 * tables, because we can't mark the constraint on the parent valid
		 * unless it is valid for all child tables.
		 *
		 * 若被告知不要递归，最好没有子表，因为除非约束对所有子表都有效，否则不能把父表上的约束标成有效。
		 */
		if (!recurse)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					errmsg("constraint must be validated on child tables too"));

		/*
		 * The column on child might have a different attnum, so search by
		 * column name.
		 *
		 * 子表上该列的 attnum 可能不同，所以按列名搜索。
		 */
		contup = findNotNullConstraint(childoid, colname);
		if (!contup)
			elog(ERROR, "cache lookup failed for not-null constraint on column \"%s\" of relation \"%s\"",
				 colname, get_rel_name(childoid));
		childcon = (Form_pg_constraint) GETSTRUCT(contup);
		if (childcon->convalidated)
			continue;

		/* find_all_inheritors already got lock */
		/*
		 *
		 * find_all_inheritors 已经拿到锁
		 */
		childrel = table_open(childoid, NoLock);
		conname = pstrdup(NameStr(childcon->conname));

		/* XXX improve ATExecValidateConstraint API to avoid double search */
		/*
		 *
		 * XXX 改进 ATExecValidateConstraint 的 API，以免搜索两次
		 */
		ATExecValidateConstraint(wqueue, childrel, conname,
								 false, true, lockmode);
		table_close(childrel, NoLock);
	}

	/* Set attnotnull appropriately without queueing another validation */
	/*
	 *
	 * 适当设置 attnotnull，不再排队另一次验证
	 */
	set_attnotnull(NULL, rel, attnum, true, false);

	tab = ATGetQueueEntry(wqueue, rel);
	tab->verify_new_notnull = true;

	/*
	 * Invalidate relcache so that others see the new validated constraint.
	 *
	 * 使 relcache 失效，以便别人看见新的已验证约束。
	 */
	CacheInvalidateRelcache(rel);

	/*
	 * Now update the catalogs, while we have the door open.
	 *
	 * 趁门开着，现在更新目录。
	 */
	copyTuple = heap_copytuple(contuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
	copy_con->convalidated = true;
	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

	heap_freetuple(copyTuple);
}

/*
 * transformColumnNameList - transform list of column names
 *
 * transformColumnNameList：变换列名列表
 *
 * Lookup each name and return its attnum and, optionally, type and collation
 * OIDs
 *
 * 查找每个名字并返回其 attnum，以及可选的类型和排序规则 OID
 *
 * Note: the name of this function suggests that it's general-purpose,
 * but actually it's only used to look up names appearing in foreign-key
 * clauses.  The error messages would need work to use it in other cases,
 * and perhaps the validity checks as well.
 *
 * 注意：函数名让人以为它是通用的，但实际上只用来查找外键子句里出现的名字。要用到其他场合，错误信息需要加工，有效性检查也许也要。
 */
static int
transformColumnNameList(Oid relId, List *colList,
						int16 *attnums, Oid *atttypids, Oid *attcollids)
{
	ListCell   *l;
	int			attnum;

	attnum = 0;
	foreach(l, colList)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;
		Form_pg_attribute attform;

		atttuple = SearchSysCacheAttName(relId, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" referenced in foreign key constraint does not exist",
							attname)));
		attform = (Form_pg_attribute) GETSTRUCT(atttuple);
		if (attform->attnum < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("system columns cannot be used in foreign keys")));
		if (attnum >= INDEX_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d keys in a foreign key",
							INDEX_MAX_KEYS)));
		attnums[attnum] = attform->attnum;
		if (atttypids != NULL)
			atttypids[attnum] = attform->atttypid;
		if (attcollids != NULL)
			attcollids[attnum] = attform->attcollation;
		ReleaseSysCache(atttuple);
		attnum++;
	}

	return attnum;
}

/*
 * transformFkeyGetPrimaryKey -
 *
 * 函数 transformFkeyGetPrimaryKey
 *
 *	Look up the names, attnums, types, and collations of the primary key attributes
 *	for the pkrel.  Also return the index OID and index opclasses of the
 *	index supporting the primary key.  Also return whether the index has
 *	WITHOUT OVERLAPS.
 *
 * 查找 pkrel 主键属性的名字、attnum、类型和排序规则。同时返回支撑主键的索引 OID 和索引操作符类。还返回该索引是否有
 * WITHOUT OVERLAPS。
 *
 *	All parameters except pkrel are output parameters.  Also, the function
 *	return value is the number of attributes in the primary key.
 *
 * 除 pkrel 外的参数都是输出参数。函数返回值是主键中的属性个数。
 *
 *	Used when the column list in the REFERENCES specification is omitted.
 *
 * 在省略 REFERENCES 说明里的列清单时使用。
 */
static int
transformFkeyGetPrimaryKey(Relation pkrel, Oid *indexOid,
						   List **attnamelist,
						   int16 *attnums, Oid *atttypids, Oid *attcollids,
						   Oid *opclasses, bool *pk_has_without_overlaps)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	HeapTuple	indexTuple = NULL;
	Form_pg_index indexStruct = NULL;
	Datum		indclassDatum;
	oidvector  *indclass;
	int			i;

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache until we find one marked primary key
	 * (hopefully there isn't more than one such).  Insist it's valid, too.
	 *
	 * 从 relcache 取得该表的索引 OID 列表，在 pg_index 系统缓存里逐个查找，
	 * 直到找到标成主键的那个（希望不会有多于一个）。还要坚持它是有效的。
	 */
	*indexOid = InvalidOid;

	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);

		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
		if (indexStruct->indisprimary && indexStruct->indisvalid)
		{
			/*
			 * Refuse to use a deferrable primary key.  This is per SQL spec,
			 * and there would be a lot of interesting semantic problems if we
			 * tried to allow it.
			 *
			 * 拒绝使用可推迟的主键。这符合 SQL 标准，若试图允许会有许多有趣的语义问题。
			 */
			if (!indexStruct->indimmediate)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot use a deferrable primary key for referenced table \"%s\"",
								RelationGetRelationName(pkrel))));

			*indexOid = indexoid;
			break;
		}
		ReleaseSysCache(indexTuple);
	}

	list_free(indexoidlist);

	/*
	 * Check that we found it
	 *
	 * 检查我们是否找到了它
	 */
	if (!OidIsValid(*indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no primary key for referenced table \"%s\"",
						RelationGetRelationName(pkrel))));

	/* Must get indclass the hard way */
	/*
	 *
	 * 必须用较麻烦的办法取得 indclass
	 */
	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
										   Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * Now build the list of PK attributes from the indkey definition (we
	 * assume a primary key cannot have expressional elements)
	 *
	 * 现在根据 indkey 定义建立主键属性列表（假定主键不能有表达式元素）
	 */
	*attnamelist = NIL;
	for (i = 0; i < indexStruct->indnkeyatts; i++)
	{
		int			pkattno = indexStruct->indkey.values[i];

		attnums[i] = pkattno;
		atttypids[i] = attnumTypeId(pkrel, pkattno);
		attcollids[i] = attnumCollationId(pkrel, pkattno);
		opclasses[i] = indclass->values[i];
		*attnamelist = lappend(*attnamelist,
							   makeString(pstrdup(NameStr(*attnumAttName(pkrel, pkattno)))));
	}

	*pk_has_without_overlaps = indexStruct->indisexclusion;

	ReleaseSysCache(indexTuple);

	return i;
}

/*
 * transformFkeyCheckAttrs -
 *
 * 函数 transformFkeyCheckAttrs
 *
 *	Validate that the 'attnums' columns in the 'pkrel' relation are valid to
 *	reference as part of a foreign key constraint.
 *
 * 验证 pkrel 关系中 attnums 这些列作为外键约束的被引用部分是否合法。
 *
 *	Returns the OID of the unique index supporting the constraint and
 *	populates the caller-provided 'opclasses' array with the opclasses
 *	associated with the index columns.  Also sets whether the index
 *	uses WITHOUT OVERLAPS.
 *
 * 返回支撑该约束的唯一索引 OID，并把与索引列关联的操作符类填进调用方提供的 opclasses 数组。同时设置该索引是否使用
 * WITHOUT OVERLAPS。
 *
 *	Raises an ERROR on validation failure.
 *
 * 验证失败时抛出 ERROR。
 */
static Oid
transformFkeyCheckAttrs(Relation pkrel,
						int numattrs, int16 *attnums,
						bool with_period, Oid *opclasses,
						bool *pk_has_without_overlaps)
{
	Oid			indexoid = InvalidOid;
	bool		found = false;
	bool		found_deferrable = false;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i,
				j;

	/*
	 * Reject duplicate appearances of columns in the referenced-columns list.
	 * Such a case is forbidden by the SQL standard, and even if we thought it
	 * useful to allow it, there would be ambiguity about how to match the
	 * list to unique indexes (in particular, it'd be unclear which index
	 * opclass goes with which FK column).
	 *
	 * 拒绝被引用列清单里重复出现列。SQL 标准禁止这种情况，即使我们认为允许它有用，
	 * 如何把清单匹配到唯一索引也会有歧义（尤其不清楚哪个索引操作符类对应哪一列外键）。
	 */
	for (i = 0; i < numattrs; i++)
	{
		for (j = i + 1; j < numattrs; j++)
		{
			if (attnums[i] == attnums[j])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FOREIGN_KEY),
						 errmsg("foreign key referenced-columns list must not contain duplicates")));
		}
	}

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache, and match unique indexes to the list
	 * of attnums we are given.
	 *
	 * 从 relcache 取得该表的索引 OID 列表，在 pg_index 系统缓存里逐个查找，并把唯一索引与我们给出的
	 * attnum 列表匹配。
	 */
	indexoidlist = RelationGetIndexList(pkrel);

	foreach(indexoidscan, indexoidlist)
	{
		HeapTuple	indexTuple;
		Form_pg_index indexStruct;

		indexoid = lfirst_oid(indexoidscan);
		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexoid);
		indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Must have the right number of columns; must be unique (or if
		 * temporal then exclusion instead) and not a partial index; forget it
		 * if there are any expressions, too. Invalid indexes are out as well.
		 *
		 * 列数必须正确；必须唯一（若是时态的则改为排他）且不是部分索引；若有任何表达式也排除。无效索引同样不行。
		 */
		if (indexStruct->indnkeyatts == numattrs &&
			(with_period ? indexStruct->indisexclusion : indexStruct->indisunique) &&
			indexStruct->indisvalid &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred, NULL) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs, NULL))
		{
			Datum		indclassDatum;
			oidvector  *indclass;

			/* Must get indclass the hard way */
			/*
			 *
			 * 必须用较麻烦的办法取得 indclass
			 */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * The given attnum list may match the index columns in any order.
			 * Check for a match, and extract the appropriate opclasses while
			 * we're at it.
			 *
			 * 给出的 attnum 列表可以按任意顺序匹配索引列。检查是否匹配，同时取出相应的操作符类。
			 *
			 * We know that attnums[] is duplicate-free per the test at the
			 * start of this function, and we checked above that the number of
			 * index columns agrees, so if we find a match for each attnums[]
			 * entry then we must have a one-to-one match in some order.
			 *
			 * 我们知道 attnums[] 在本函数开头的测试里没有重复，上面也检查过索引列数一致，所以若为每个 attnums[]
			 * 项都找到匹配，就必定是某种顺序下的一一对应。
			 */
			for (i = 0; i < numattrs; i++)
			{
				found = false;
				for (j = 0; j < numattrs; j++)
				{
					if (attnums[i] == indexStruct->indkey.values[j])
					{
						opclasses[i] = indclass->values[j];
						found = true;
						break;
					}
				}
				if (!found)
					break;
			}
			/* The last attribute in the index must be the PERIOD FK part */
			/*
			 *
			 * 索引的最后一个属性必须是 PERIOD 外键那一部分
			 */
			if (found && with_period)
			{
				int16		periodattnum = attnums[numattrs - 1];

				found = (periodattnum == indexStruct->indkey.values[numattrs - 1]);
			}

			/*
			 * Refuse to use a deferrable unique/primary key.  This is per SQL
			 * spec, and there would be a lot of interesting semantic problems
			 * if we tried to allow it.
			 *
			 * 拒绝使用可推迟的唯一键或主键。这符合 SQL 标准，若试图允许会有许多有趣的语义问题。
			 */
			if (found && !indexStruct->indimmediate)
			{
				/*
				 * Remember that we found an otherwise matching index, so that
				 * we can generate a more appropriate error message.
				 *
				 * 记住我们找到过一个除此之外都匹配的索引，以便生成更合适的错误信息。
				 */
				found_deferrable = true;
				found = false;
			}

			/* We need to know whether the index has WITHOUT OVERLAPS */
			/*
			 *
			 * 需要知道该索引是否有 WITHOUT OVERLAPS
			 */
			if (found)
				*pk_has_without_overlaps = indexStruct->indisexclusion;
		}
		ReleaseSysCache(indexTuple);
		if (found)
			break;
	}

	if (!found)
	{
		if (found_deferrable)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot use a deferrable unique constraint for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("there is no unique constraint matching given keys for referenced table \"%s\"",
							RelationGetRelationName(pkrel))));
	}

	list_free(indexoidlist);

	return indexoid;
}

/*
 * findFkeyCast -
 *
 * 函数 findFkeyCast
 *
 *	Wrapper around find_coercion_pathway() for ATAddForeignKeyConstraint().
 *	Caller has equal regard for binary coercibility and for an exact match.
 *
 * ATAddForeignKeyConstraint() 对 find_coercion_pathway() 的包装。
 * 调用方对二进制可转换和精确匹配一视同仁。
*/
static CoercionPathType
findFkeyCast(Oid targetTypeId, Oid sourceTypeId, Oid *funcid)
{
	CoercionPathType ret;

	if (targetTypeId == sourceTypeId)
	{
		ret = COERCION_PATH_RELABELTYPE;
		*funcid = InvalidOid;
	}
	else
	{
		ret = find_coercion_pathway(targetTypeId, sourceTypeId,
									COERCION_IMPLICIT, funcid);
		if (ret == COERCION_PATH_NONE)
			/* A previously-relied-upon cast is now gone. */
			/*
			 *
			 * 先前依赖的转换现在没了。
			 */
			elog(ERROR, "could not find cast from %u to %u",
				 sourceTypeId, targetTypeId);
	}

	return ret;
}

/*
 * Permissions checks on the referenced table for ADD FOREIGN KEY
 *
 * ADD FOREIGN KEY 时对被引用表的权限检查
 *
 * Note: we have already checked that the user owns the referencing table,
 * else we'd have failed much earlier; no additional checks are needed for it.
 *
 * 注意：我们已经检查过用户拥有引用表，否则早就失败了；对它不需要额外检查。
 */
static void
checkFkeyPermissions(Relation rel, int16 *attnums, int natts)
{
	Oid			roleid = GetUserId();
	AclResult	aclresult;
	int			i;

	/* Okay if we have relation-level REFERENCES permission */
	/*
	 *
	 * 若有关系级的 REFERENCES 权限就可以
	 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), roleid,
								  ACL_REFERENCES);
	if (aclresult == ACLCHECK_OK)
		return;
	/* Else we must have REFERENCES on each column */
	/*
	 *
	 * 否则必须对每一列都有 REFERENCES 权限
	 */
	for (i = 0; i < natts; i++)
	{
		aclresult = pg_attribute_aclcheck(RelationGetRelid(rel), attnums[i],
										  roleid, ACL_REFERENCES);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));
	}
}

/*
 * Scan the existing rows in a table to verify they meet a proposed FK
 * constraint.
 *
 * 扫描表中现有行，验证它们满足拟议的外键约束。
 *
 * Caller must have opened and locked both relations appropriately.
 *
 * 调用方必须已经适当打开并锁住两边的关系。
 */
static void
validateForeignKeyConstraint(char *conname,
							 Relation rel,
							 Relation pkrel,
							 Oid pkindOid,
							 Oid constraintOid,
							 bool hasperiod)
{
	TupleTableSlot *slot;
	TableScanDesc scan;
	Trigger		trig = {0};
	Snapshot	snapshot;
	MemoryContext oldcxt;
	MemoryContext perTupCxt;

	ereport(DEBUG1,
			(errmsg_internal("validating foreign key constraint \"%s\"", conname)));

	/*
	 * Build a trigger call structure; we'll need it either way.
	 *
	 * 构造一个触发器调用结构；两种办法都需要它。
	 */
	trig.tgoid = InvalidOid;
	trig.tgname = conname;
	trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
	trig.tgisinternal = true;
	trig.tgconstrrelid = RelationGetRelid(pkrel);
	trig.tgconstrindid = pkindOid;
	trig.tgconstraint = constraintOid;
	trig.tgdeferrable = false;
	trig.tginitdeferred = false;
	/* we needn't fill in remaining fields */
	/*
	 *
	 * 其余字段不必填
	 */

	/*
	 * See if we can do it with a single LEFT JOIN query.  A false result
	 * indicates we must proceed with the fire-the-trigger method. We can't do
	 * a LEFT JOIN for temporal FKs yet, but we can once we support temporal
	 * left joins.
	 *
	 * 看看能否用一条 LEFT JOIN 查询完成。返回假表示必须改用触发触发器的办法。时态外键还不能做 LEFT JOIN，
	 * 但一旦支持时态左连接就可以。
	 */
	if (!hasperiod && RI_Initial_Check(&trig, rel, pkrel))
		return;

	/*
	 * Scan through each tuple, calling RI_FKey_check_ins (insert trigger) as
	 * if that tuple had just been inserted.  If any of those fail, it should
	 * ereport(ERROR) and that's that.
	 *
	 * 逐个扫描元组，调用 RI_FKey_check_ins（插入触发器），就好像该元组刚刚被插入。若其中任何一个失败，它应
	 * ereport(ERROR)，事情到此为止。
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "validateForeignKeyConstraint",
									  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(perTupCxt);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		LOCAL_FCINFO(fcinfo, 0);
		TriggerData trigdata = {0};

		CHECK_FOR_INTERRUPTS();

		/*
		 * Make a call to the trigger function
		 *
		 * 调用触发器函数
		 *
		 * No parameters are passed, but we do set a context
		 *
		 * 不传参数，但我们会设置一个上下文
		 */
		MemSet(fcinfo, 0, SizeForFunctionCallInfo(0));

		/*
		 * We assume RI_FKey_check_ins won't look at flinfo...
		 *
		 * 假定 RI_FKey_check_ins 不会去看 flinfo……
		 */
		trigdata.type = T_TriggerData;
		trigdata.tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
		trigdata.tg_relation = rel;
		trigdata.tg_trigtuple = ExecFetchSlotHeapTuple(slot, false, NULL);
		trigdata.tg_trigslot = slot;
		trigdata.tg_trigger = &trig;

		fcinfo->context = (Node *) &trigdata;

		RI_FKey_check_ins(fcinfo);

		MemoryContextReset(perTupCxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perTupCxt);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
}

/*
 * CreateFKCheckTrigger
 *		Creates the insert (on_insert=true) or update "check" trigger that
 *		implements a given foreign key
 *
 * CreateFKCheckTrigger：创建实现给定外键的插入（on_insert 为真）或更新检查触发器
 *
 * Returns the OID of the so created trigger.
 *
 * 返回如此创建的触发器的 OID。
 */
static Oid
CreateFKCheckTrigger(Oid myRelOid, Oid refRelOid, Constraint *fkconstraint,
					 Oid constraintOid, Oid indexOid, Oid parentTrigOid,
					 bool on_insert)
{
	ObjectAddress trigAddress;
	CreateTrigStmt *fk_trigger;

	/*
	 * Note: for a self-referential FK (referencing and referenced tables are
	 * the same), it is important that the ON UPDATE action fires before the
	 * CHECK action, since both triggers will fire on the same row during an
	 * UPDATE event; otherwise the CHECK trigger will be checking a non-final
	 * state of the row.  Triggers fire in name order, so we ensure this by
	 * using names like "RI_ConstraintTrigger_a_NNNN" for the action triggers
	 * and "RI_ConstraintTrigger_c_NNNN" for the check triggers.
	 *
	 * 注意：对自引用外键（引用表和被引用表是同一张），ON UPDATE 动作必须在 CHECK 动作之前触发，因为 UPDATE
	 * 事件中两个触发器都会在同一行上触发；否则 CHECK 触发器会检查行的非最终状态。触发器按名字顺序触发，所以动作触发器用
	 * RI_ConstraintTrigger_a_NNNN 这样的名字，检查触发器用
	 * RI_ConstraintTrigger_c_NNNN，以保证这个顺序。
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_c";
	fk_trigger->relation = NULL;

	/* Either ON INSERT or ON UPDATE */
	/*
	 *
	 * 要么 ON INSERT，要么 ON UPDATE
	 */
	if (on_insert)
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_ins");
		fk_trigger->events = TRIGGER_TYPE_INSERT;
	}
	else
	{
		fk_trigger->funcname = SystemFuncName("RI_FKey_check_upd");
		fk_trigger->events = TRIGGER_TYPE_UPDATE;
	}

	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->deferrable = fkconstraint->deferrable;
	fk_trigger->initdeferred = fkconstraint->initdeferred;
	fk_trigger->constrrel = NULL;

	trigAddress = CreateTrigger(fk_trigger, NULL, myRelOid, refRelOid,
								constraintOid, indexOid, InvalidOid,
								parentTrigOid, NULL, true, false);

	/* Make changes-so-far visible */
	/*
	 *
	 * 让目前为止的修改可见
	 */
	CommandCounterIncrement();

	return trigAddress.objectId;
}

/*
 * createForeignKeyActionTriggers
 *		Create the referenced-side "action" triggers that implement a foreign
 *		key.
 *
 * createForeignKeyActionTriggers：创建实现外键的被引用侧动作触发器。
 *
 * Returns the OIDs of the so created triggers in *deleteTrigOid and
 * *updateTrigOid.
 *
 * 把如此创建的触发器 OID 通过 deleteTrigOid 和 updateTrigOid 返回。
 */
static void
createForeignKeyActionTriggers(Oid myRelOid, Oid refRelOid, Constraint *fkconstraint,
							   Oid constraintOid, Oid indexOid,
							   Oid parentDelTrigger, Oid parentUpdTrigger,
							   Oid *deleteTrigOid, Oid *updateTrigOid)
{
	CreateTrigStmt *fk_trigger;
	ObjectAddress trigAddress;

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * DELETE action on the referenced table.
	 *
	 * 为被引用表上的 ON DELETE 动作构造并执行一条 CREATE CONSTRAINT TRIGGER 语句。
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_DELETE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->constrrel = NULL;

	switch (fkconstraint->fk_del_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_del");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_del");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_del");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_del");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_del");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_del_action);
			break;
	}

	trigAddress = CreateTrigger(fk_trigger, NULL, refRelOid, myRelOid,
								constraintOid, indexOid, InvalidOid,
								parentDelTrigger, NULL, true, false);
	if (deleteTrigOid)
		*deleteTrigOid = trigAddress.objectId;

	/* Make changes-so-far visible */
	/*
	 *
	 * 让目前为止的修改可见
	 */
	CommandCounterIncrement();

	/*
	 * Build and execute a CREATE CONSTRAINT TRIGGER statement for the ON
	 * UPDATE action on the referenced table.
	 *
	 * 为被引用表上的 ON UPDATE 动作构造并执行一条 CREATE CONSTRAINT TRIGGER 语句。
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_a";
	fk_trigger->relation = NULL;
	fk_trigger->args = NIL;
	fk_trigger->row = true;
	fk_trigger->timing = TRIGGER_TYPE_AFTER;
	fk_trigger->events = TRIGGER_TYPE_UPDATE;
	fk_trigger->columns = NIL;
	fk_trigger->whenClause = NULL;
	fk_trigger->transitionRels = NIL;
	fk_trigger->constrrel = NULL;

	switch (fkconstraint->fk_upd_action)
	{
		case FKCONSTR_ACTION_NOACTION:
			fk_trigger->deferrable = fkconstraint->deferrable;
			fk_trigger->initdeferred = fkconstraint->initdeferred;
			fk_trigger->funcname = SystemFuncName("RI_FKey_noaction_upd");
			break;
		case FKCONSTR_ACTION_RESTRICT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_restrict_upd");
			break;
		case FKCONSTR_ACTION_CASCADE:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_cascade_upd");
			break;
		case FKCONSTR_ACTION_SETNULL:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setnull_upd");
			break;
		case FKCONSTR_ACTION_SETDEFAULT:
			fk_trigger->deferrable = false;
			fk_trigger->initdeferred = false;
			fk_trigger->funcname = SystemFuncName("RI_FKey_setdefault_upd");
			break;
		default:
			elog(ERROR, "unrecognized FK action type: %d",
				 (int) fkconstraint->fk_upd_action);
			break;
	}

	trigAddress = CreateTrigger(fk_trigger, NULL, refRelOid, myRelOid,
								constraintOid, indexOid, InvalidOid,
								parentUpdTrigger, NULL, true, false);
	if (updateTrigOid)
		*updateTrigOid = trigAddress.objectId;
}

/*
 * createForeignKeyCheckTriggers
 *		Create the referencing-side "check" triggers that implement a foreign
 *		key.
 *
 * createForeignKeyCheckTriggers：创建实现外键的引用侧检查触发器。
 *
 * Returns the OIDs of the so created triggers in *insertTrigOid and
 * *updateTrigOid.
 *
 * 把如此创建的触发器 OID 通过 insertTrigOid 和 updateTrigOid 返回。
 */
static void
createForeignKeyCheckTriggers(Oid myRelOid, Oid refRelOid,
							  Constraint *fkconstraint, Oid constraintOid,
							  Oid indexOid,
							  Oid parentInsTrigger, Oid parentUpdTrigger,
							  Oid *insertTrigOid, Oid *updateTrigOid)
{
	*insertTrigOid = CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint,
										  constraintOid, indexOid,
										  parentInsTrigger, true);
	*updateTrigOid = CreateFKCheckTrigger(myRelOid, refRelOid, fkconstraint,
										  constraintOid, indexOid,
										  parentUpdTrigger, false);
}

/*
 * ALTER TABLE DROP CONSTRAINT
 *
 * ALTER TABLE DROP CONSTRAINT（删约束）
 *
 * Like DROP COLUMN, we can't use the normal ALTER TABLE recursion mechanism.
 *
 * 和 DROP COLUMN 一样，不能用普通的 ALTER TABLE 递归机制。
 */
static void
ATExecDropConstraint(Relation rel, const char *constrName,
					 DropBehavior behavior, bool recurse,
					 bool missing_ok, LOCKMODE lockmode)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData skey[3];
	HeapTuple	tuple;
	bool		found = false;

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * Find and drop the target constraint
	 *
	 * 找到并删除目标约束
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 3, skey);

	/* There can be at most one matching row */
	/*
	 *
	 * 最多只能有一行匹配
	 */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		dropconstraint_internal(rel, tuple, behavior, recurse, false,
								missing_ok, lockmode);
		found = true;
	}

	systable_endscan(scan);

	if (!found)
	{
		if (!missing_ok)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("constraint \"%s\" of relation \"%s\" does not exist",
						   constrName, RelationGetRelationName(rel)));
		else
			ereport(NOTICE,
					errmsg("constraint \"%s\" of relation \"%s\" does not exist, skipping",
						   constrName, RelationGetRelationName(rel)));
	}

	table_close(conrel, RowExclusiveLock);
}

/*
 * Remove a constraint, using its pg_constraint tuple
 *
 * 用它的 pg_constraint 元组去掉一个约束
 *
 * Implementation for ALTER TABLE DROP CONSTRAINT and ALTER TABLE ALTER COLUMN
 * DROP NOT NULL.
 *
 * ALTER TABLE DROP CONSTRAINT 和 ALTER TABLE ALTER COLUMN DROP NOT
 * NULL 的实现。
 *
 * Returns the address of the constraint being removed.
 *
 * 返回正在被去掉的约束的地址。
 */
static ObjectAddress
dropconstraint_internal(Relation rel, HeapTuple constraintTup, DropBehavior behavior,
						bool recurse, bool recursing, bool missing_ok,
						LOCKMODE lockmode)
{
	Relation	conrel;
	Form_pg_constraint con;
	ObjectAddress conobj;
	List	   *children;
	bool		is_no_inherit_constraint = false;
	char	   *constrName;
	char	   *colname = NULL;

	/* Guard against stack overflow due to overly deep inheritance tree. */
	/*
	 *
	 * 防止继承树过深导致栈溢出。
	 */
	check_stack_depth();

	/* At top level, permission check was done in ATPrepCmd, else do it */
	/*
	 *
	 * 顶层的权限检查已在 ATPrepCmd 做过，否则在这里做
	 */
	if (recursing)
		ATSimplePermissions(AT_DropConstraint, rel,
							ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	con = (Form_pg_constraint) GETSTRUCT(constraintTup);
	constrName = NameStr(con->conname);

	/* Don't allow drop of inherited constraints */
	/*
	 *
	 * 不允许删除继承来的约束
	 */
	if (con->coninhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited constraint \"%s\" of relation \"%s\"",
						constrName, RelationGetRelationName(rel))));

	/*
	 * Reset pg_constraint.attnotnull, if this is a not-null constraint.
	 *
	 * 若这是 NOT NULL 约束，重置 pg_constraint.attnotnull。
	 *
	 * While doing that, we're in a good position to disallow dropping a not-
	 * null constraint underneath a primary key, a replica identity index, or
	 * a generated identity column.
	 *
	 * 做这件事时，我们正好可以禁止在主键、复制标识索引或生成的标识列底下删掉 NOT NULL 约束。
	 */
	if (con->contype == CONSTRAINT_NOTNULL)
	{
		Relation	attrel = table_open(AttributeRelationId, RowExclusiveLock);
		AttrNumber	attnum = extractNotNullColumn(constraintTup);
		Bitmapset  *pkattrs;
		Bitmapset  *irattrs;
		HeapTuple	atttup;
		Form_pg_attribute attForm;

		/* save column name for recursion step */
		/*
		 *
		 * 为递归步骤保存列名
		 */
		colname = get_attname(RelationGetRelid(rel), attnum, false);

		/*
		 * Disallow if it's in the primary key.  For partitioned tables we
		 * cannot rely solely on RelationGetIndexAttrBitmap, because it'll
		 * return NULL if the primary key is invalid; but we still need to
		 * protect not-null constraints under such a constraint, so check the
		 * slow way.
		 *
		 * 若它属于主键则禁止。对分区表不能只靠 RelationGetIndexAttrBitmap，因为主键无效时它会返回 NULL；
		 * 但我们仍要保护这种约束底下的 NOT NULL 约束，所以走慢路径检查。
		 */
		pkattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_PRIMARY_KEY);

		if (pkattrs == NULL &&
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			Oid			pkindex = RelationGetPrimaryKeyIndex(rel, true);

			if (OidIsValid(pkindex))
			{
				Relation	pk = relation_open(pkindex, AccessShareLock);

				pkattrs = NULL;
				for (int i = 0; i < pk->rd_index->indnkeyatts; i++)
					pkattrs = bms_add_member(pkattrs, pk->rd_index->indkey.values[i]);

				relation_close(pk, AccessShareLock);
			}
		}

		if (pkattrs &&
			bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber, pkattrs))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					errmsg("column \"%s\" is in a primary key",
						   get_attname(RelationGetRelid(rel), attnum, false)));

		/* Disallow if it's in the replica identity */
		/*
		 *
		 * 若它属于复制标识则禁止
		 */
		irattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY);
		if (bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber, irattrs))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					errmsg("column \"%s\" is in index used as replica identity",
						   get_attname(RelationGetRelid(rel), attnum, false)));

		/* Disallow if it's a GENERATED AS IDENTITY column */
		/*
		 *
		 * 若它是 GENERATED AS IDENTITY 列则禁止
		 */
		atttup = SearchSysCacheCopyAttNum(RelationGetRelid(rel), attnum);
		if (!HeapTupleIsValid(atttup))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, RelationGetRelid(rel));
		attForm = (Form_pg_attribute) GETSTRUCT(atttup);
		if (attForm->attidentity != '\0')
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("column \"%s\" of relation \"%s\" is an identity column",
						   get_attname(RelationGetRelid(rel), attnum,
									   false),
						   RelationGetRelationName(rel)));

		/* All good -- reset attnotnull if needed */
		/*
		 *
		 * 一切正常，需要时重置 attnotnull
		 */
		if (attForm->attnotnull)
		{
			attForm->attnotnull = false;
			CatalogTupleUpdate(attrel, &atttup->t_self, atttup);
		}

		table_close(attrel, RowExclusiveLock);
	}

	is_no_inherit_constraint = con->connoinherit;

	/*
	 * If it's a foreign-key constraint, we'd better lock the referenced table
	 * and check that that's not in use, just as we've already done for the
	 * constrained table (else we might, eg, be dropping a trigger that has
	 * unfired events).  But we can/must skip that in the self-referential
	 * case.
	 *
	 * 若是外键约束，最好锁住被引用表并检查它没有在使用，就像我们已经对受约束的表做过的那样（否则可能例如删掉一个还有未触发事件的触发器）。
	 * 但自引用的情况下可以也必须跳过。
	 */
	if (con->contype == CONSTRAINT_FOREIGN &&
		con->confrelid != RelationGetRelid(rel))
	{
		Relation	frel;

		/* Must match lock taken by RemoveTriggerById: */
		/*
		 *
		 * 必须与 RemoveTriggerById 取得的锁一致：
		 */
		frel = table_open(con->confrelid, AccessExclusiveLock);
		CheckAlterTableIsSafe(frel);
		table_close(frel, NoLock);
	}

	/*
	 * Perform the actual constraint deletion
	 *
	 * 执行实际的约束删除
	 */
	ObjectAddressSet(conobj, ConstraintRelationId, con->oid);
	performDeletion(&conobj, behavior, 0);

	/*
	 * For partitioned tables, non-CHECK, non-NOT-NULL inherited constraints
	 * are dropped via the dependency mechanism, so we're done here.
	 *
	 * 对分区表，非 CHECK、非 NOT NULL 的继承约束通过依赖机制删除，所以到这里就结束了。
	 */
	if (con->contype != CONSTRAINT_CHECK &&
		con->contype != CONSTRAINT_NOTNULL &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		table_close(conrel, RowExclusiveLock);
		return conobj;
	}

	/*
	 * Propagate to children as appropriate.  Unlike most other ALTER
	 * routines, we have to do this one level of recursion at a time; we can't
	 * use find_all_inheritors to do it in one pass.
	 *
	 * 按需要传播到子表。和大多数其他 ALTER 例程不同，这里必须一层一层递归；不能用 find_all_inheritors
	 * 一次做完。
	 */
	if (!is_no_inherit_constraint)
		children = find_inheritance_children(RelationGetRelid(rel), lockmode);
	else
		children = NIL;

	foreach_oid(childrelid, children)
	{
		Relation	childrel;
		HeapTuple	tuple;
		Form_pg_constraint childcon;

		/* find_inheritance_children already got lock */
		/*
		 *
		 * find_inheritance_children 已经拿到锁
		 */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/*
		 * We search for not-null constraints by column name, and others by
		 * constraint name.
		 *
		 * NOT NULL 约束按列名搜索，其他约束按约束名搜索。
		 */
		if (con->contype == CONSTRAINT_NOTNULL)
		{
			tuple = findNotNullConstraint(childrelid, colname);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for not-null constraint on column \"%s\" of relation %u",
					 colname, RelationGetRelid(childrel));
		}
		else
		{
			SysScanDesc scan;
			ScanKeyData skey[3];

			ScanKeyInit(&skey[0],
						Anum_pg_constraint_conrelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(childrelid));
			ScanKeyInit(&skey[1],
						Anum_pg_constraint_contypid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(InvalidOid));
			ScanKeyInit(&skey[2],
						Anum_pg_constraint_conname,
						BTEqualStrategyNumber, F_NAMEEQ,
						CStringGetDatum(constrName));
			scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
									  true, NULL, 3, skey);
			/* There can only be one, so no need to loop */
			/*
			 *
			 * 只能有一个，所以不必循环
			 */
			tuple = systable_getnext(scan);
			if (!HeapTupleIsValid(tuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
								constrName,
								RelationGetRelationName(childrel))));
			tuple = heap_copytuple(tuple);
			systable_endscan(scan);
		}

		childcon = (Form_pg_constraint) GETSTRUCT(tuple);

		/* Right now only CHECK and not-null constraints can be inherited */
		/*
		 *
		 * 目前只有 CHECK 和 NOT NULL 约束可以继承
		 */
		if (childcon->contype != CONSTRAINT_CHECK &&
			childcon->contype != CONSTRAINT_NOTNULL)
			elog(ERROR, "inherited constraint is not a CHECK or not-null constraint");

		if (childcon->coninhcount <= 0) /* shouldn't happen */
						/*
						 *
						 * 不该发生
						 */
			elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
				 childrelid, NameStr(childcon->conname));

		if (recurse)
		{
			/*
			 * If the child constraint has other definition sources, just
			 * decrement its inheritance count; if not, recurse to delete it.
			 *
			 * 若子约束还有其他定义来源，只把它的继承计数减一；否则递归删除它。
			 */
			if (childcon->coninhcount == 1 && !childcon->conislocal)
			{
				/* Time to delete this child constraint, too */
				/*
				 *
				 * 也该删除这个子约束了
				 */
				dropconstraint_internal(childrel, tuple, behavior,
										recurse, true, missing_ok,
										lockmode);
			}
			else
			{
				/* Child constraint must survive my deletion */
				/*
				 *
				 * 子约束必须在我这次删除后仍然存在
				 */
				childcon->coninhcount--;
				CatalogTupleUpdate(conrel, &tuple->t_self, tuple);

				/* Make update visible */
				/*
				 *
				 * 让更新可见
				 */
				CommandCounterIncrement();
			}
		}
		else
		{
			/*
			 * If we were told to drop ONLY in this table (no recursion) and
			 * there are no further parents for this constraint, we need to
			 * mark the inheritors' constraints as locally defined rather than
			 * inherited.
			 *
			 * 若被告知只在这张表上删除（不递归），并且这个约束没有更多父约束，需要把继承者的约束标成局部定义，而不是继承来的。
			 */
			childcon->coninhcount--;
			if (childcon->coninhcount == 0)
				childcon->conislocal = true;

			CatalogTupleUpdate(conrel, &tuple->t_self, tuple);

			/* Make update visible */
			/*
			 *
			 * 让更新可见
			 */
			CommandCounterIncrement();
		}

		heap_freetuple(tuple);

		table_close(childrel, NoLock);
	}

	table_close(conrel, RowExclusiveLock);

	return conobj;
}

/*
 * ALTER COLUMN TYPE
 *
 * ALTER COLUMN TYPE（修改列类型）
 *
 * Unlike other subcommand types, we do parse transformation for ALTER COLUMN
 * TYPE during phase 1 --- the AlterTableCmd passed in here is already
 * transformed (and must be, because we rely on some transformed fields).
 *
 * 和其他子命令类型不同，我们在阶段 1 就对 ALTER COLUMN TYPE 做解析变换。这里传入的 AlterTableCmd
 * 已经变换过（而且必须如此，因为我们依赖一些变换后的字段）。
 *
 * The point of this is that the execution of all ALTER COLUMN TYPEs for a
 * table will be done "in parallel" during phase 3, so all the USING
 * expressions should be parsed assuming the original column types.  Also,
 * this allows a USING expression to refer to a field that will be dropped.
 *
 * 这样做的意义是，一张表上所有 ALTER COLUMN TYPE 会在阶段 3 并行执行，所以所有 USING
 * 表达式都应按原始列类型来解析。另外，这允许 USING 表达式引用即将被删除的字段。
 *
 * To make this work safely, AT_PASS_DROP then AT_PASS_ALTER_TYPE must be
 * the first two execution steps in phase 2; they must not see the effects
 * of any other subcommand types, since the USING expressions are parsed
 * against the unmodified table's state.
 *
 * 为了安全地做到这一点，AT_PASS_DROP 然后 AT_PASS_ALTER_TYPE 必须是阶段 2 的前两步；
 * 它们绝不能看见任何其他子命令类型的效果，因为 USING 表达式是对着未修改的表状态解析的。
 */
static void
ATPrepAlterColumnType(List **wqueue,
					  AlteredTableInfo *tab, Relation rel,
					  bool recurse, bool recursing,
					  AlterTableCmd *cmd, LOCKMODE lockmode,
					  AlterTableUtilityContext *context)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	Node	   *transform = def->cooked_default;
	HeapTuple	tuple;
	Form_pg_attribute attTup;
	AttrNumber	attnum;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	NewColumnValue *newval;
	ParseState *pstate = make_parsestate(NULL);
	AclResult	aclresult;
	bool		is_expr;

	pstate->p_sourcetext = context->queryString;

	if (rel->rd_rel->reloftype && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter column type of typed table"),
				 parser_errposition(pstate, def->location)));

	/* lookup the attribute so we can check inheritance status */
	/*
	 *
	 * 查找该属性，以便检查继承状态
	 */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel)),
				 parser_errposition(pstate, def->location)));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* Can't alter a system attribute */
	/*
	 *
	 * 不能修改系统属性
	 */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", colName),
				 parser_errposition(pstate, def->location)));

	/*
	 * Cannot specify USING when altering type of a generated column, because
	 * that would violate the generation expression.
	 *
	 * 修改生成列的类型时不能指定 USING，因为那会违反生成表达式。
	 */
	if (attTup->attgenerated && def->cooked_default)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				 errmsg("cannot specify USING when altering type of generated column"),
				 errdetail("Column \"%s\" is a generated column.", colName),
				 parser_errposition(pstate, def->location)));

	/*
	 * Don't alter inherited columns.  At outer level, there had better not be
	 * any inherited definition; when recursing, we assume this was checked at
	 * the parent level (see below).
	 *
	 * 不要修改继承来的列。在外层，最好没有任何继承定义；递归时，我们假定这已在父层检查过（见下文）。
	 */
	if (attTup->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"", colName),
				 parser_errposition(pstate, def->location)));

	/* Don't alter columns used in the partition key */
	/*
	 *
	 * 不要修改分区键用到的列
	 */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel)),
				 parser_errposition(pstate, def->location)));

	/* Look up the target type */
	/*
	 *
	 * 查找目标类型
	 */
	typenameTypeIdAndMod(pstate, typeName, &targettype, &targettypmod);

	aclresult = object_aclcheck(TypeRelationId, targettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettype);

	/* And the collation */
	/*
	 *
	 * 以及排序规则
	 */
	targetcollid = GetColumnDefCollation(pstate, def, targettype);

	/* make sure datatype is legal for a column */
	/*
	 *
	 * 确认数据类型可以作为列类型
	 */
	CheckAttributeType(colName, targettype, targetcollid,
					   list_make1_oid(rel->rd_rel->reltype),
					   (attTup->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL ? CHKATYPE_IS_VIRTUAL : 0));

	if (attTup->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
	{
		/* do nothing */
		/*
		 *
		 * 什么也不做
		 */
	}
	else if (tab->relkind == RELKIND_RELATION ||
			 tab->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Set up an expression to transform the old data value to the new
		 * type. If a USING option was given, use the expression as
		 * transformed by transformAlterTableStmt, else just take the old
		 * value and try to coerce it.  We do this first so that type
		 * incompatibility can be detected before we waste effort, and because
		 * we need the expression to be parsed against the original table row
		 * type.
		 *
		 * 准备一个把旧数据值变换成新类型的表达式。若给出了 USING 选项，就用 transformAlterTableStmt
		 * 变换后的表达式，否则就取旧值并尝试强制转换。先做这个，这样类型不兼容可以在浪费力气之前发现，也因为表达式需要按原始表行类型来解析。
		 */
		if (!transform)
		{
			transform = (Node *) makeVar(1, attnum,
										 attTup->atttypid, attTup->atttypmod,
										 attTup->attcollation,
										 0);
		}

		transform = coerce_to_target_type(pstate,
										  transform, exprType(transform),
										  targettype, targettypmod,
										  COERCION_ASSIGNMENT,
										  COERCE_IMPLICIT_CAST,
										  -1);
		if (transform == NULL)
		{
			/* error text depends on whether USING was specified or not */
			/*
			 *
			 * 错误文本取决于是否指定了 USING
			 */
			if (def->cooked_default != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("result of USING clause for column \"%s\""
								" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
						 errhint("You might need to add an explicit cast.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype)),
						 !attTup->attgenerated ?
				/* translator: USING is SQL, don't translate it */
				/*
				 *
				 * 翻译提示：USING 是 SQL，不要翻译它
				 */
						 errhint("You might need to specify \"USING %s::%s\".",
								 quote_identifier(colName),
								 format_type_with_typemod(targettype,
														  targettypmod)) : 0));
		}

		/* Fix collations after all else */
		/*
		 *
		 * 最后再修正排序规则
		 */
		assign_expr_collations(pstate, transform);

		/* Expand virtual generated columns in the expr. */
		/*
		 *
		 * 在表达式里展开虚拟生成列。
		 */
		transform = expand_generated_columns_in_expr(transform, rel, 1);

		/* Plan the expr now so we can accurately assess the need to rewrite. */
		/*
		 *
		 * 现在就规划这个表达式，以便准确评估是否需要重写。
		 */
		transform = (Node *) expression_planner((Expr *) transform);

		/*
		 * Add a work queue item to make ATRewriteTable update the column
		 * contents.
		 *
		 * 加一项工作队列，让 ATRewriteTable 更新列内容。
		 */
		newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
		newval->attnum = attnum;
		newval->expr = (Expr *) transform;
		newval->is_generated = false;

		tab->newvals = lappend(tab->newvals, newval);
		if (ATColumnChangeRequiresRewrite(transform, attnum))
			tab->rewrite |= AT_REWRITE_COLUMN_REWRITE;
	}
	else if (transform)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!RELKIND_HAS_STORAGE(tab->relkind) || attTup->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
	{
		/*
		 * For relations or columns without storage, do this check now.
		 * Regular tables will check it later when the table is being
		 * rewritten.
		 *
		 * 对没有存储的关系或列，现在就做这项检查。普通表会在重写表时再检查。
		 */
		find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
	}

	ReleaseSysCache(tuple);

	/*
	 * Recurse manually by queueing a new command for each child, if
	 * necessary. We cannot apply ATSimpleRecursion here because we need to
	 * remap attribute numbers in the USING expression, if any.
	 *
	 * 必要时为每个子表手工排队一条新命令来递归。这里不能用 ATSimpleRecursion，因为若有 USING 表达式，
	 * 需要重映射其中的属性号。
	 *
	 * If we are told not to recurse, there had better not be any child
	 * tables; else the alter would put them out of step.
	 *
	 * 若被告知不要递归，最好没有子表；否则这次修改会让它们对不上。
	 */
	if (recurse)
	{
		Oid			relid = RelationGetRelid(rel);
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		child_oids = find_all_inheritors(relid, lockmode,
										 &child_numparents);

		/*
		 * find_all_inheritors does the recursive search of the inheritance
		 * hierarchy, so all we have to do is process all of the relids in the
		 * list that it returns.
		 *
		 * find_all_inheritors 会递归搜索继承层次，我们只要处理它返回的全部 relid。
		 */
		forboth(lo, child_oids, li, child_numparents)
		{
			Oid			childrelid = lfirst_oid(lo);
			int			numparents = lfirst_int(li);
			Relation	childrel;
			HeapTuple	childtuple;
			Form_pg_attribute childattTup;

			if (childrelid == relid)
				continue;

			/* find_all_inheritors already got lock */
			/*
			 *
			 * find_all_inheritors 已经拿到锁
			 */
			childrel = relation_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);

			/*
			 * Verify that the child doesn't have any inherited definitions of
			 * this column that came from outside this inheritance hierarchy.
			 * (renameatt makes a similar test, though in a different way
			 * because of its different recursion mechanism.)
			 *
			 * 验证子表没有从这个继承层次之外继承来的该列定义。（renameatt 做类似的测试，但由于递归机制不同，做法不一样。）
			 */
			childtuple = SearchSysCacheAttName(RelationGetRelid(childrel),
											   colName);
			if (!HeapTupleIsValid(childtuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								colName, RelationGetRelationName(childrel))));
			childattTup = (Form_pg_attribute) GETSTRUCT(childtuple);

			if (childattTup->attinhcount > numparents)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot alter inherited column \"%s\" of relation \"%s\"",
								colName, RelationGetRelationName(childrel))));

			ReleaseSysCache(childtuple);

			/*
			 * Remap the attribute numbers.  If no USING expression was
			 * specified, there is no need for this step.
			 *
			 * 重映射属性号。若没有指定 USING 表达式，就不需要这一步。
			 */
			if (def->cooked_default)
			{
				AttrMap    *attmap;
				bool		found_whole_row;

				/* create a copy to scribble on */
				/*
				 *
				 * 造一份副本以便涂改
				 */
				cmd = copyObject(cmd);

				attmap = build_attrmap_by_name(RelationGetDescr(childrel),
											   RelationGetDescr(rel),
											   false);
				((ColumnDef *) cmd->def)->cooked_default =
					map_variable_attnos(def->cooked_default,
										1, 0,
										attmap,
										InvalidOid, &found_whole_row);
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("USING expression contains a whole-row table reference.")));
				pfree(attmap);
			}
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
	else if (!recursing &&
			 find_inheritance_children(RelationGetRelid(rel), NoLock) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("type of inherited column \"%s\" must be changed in child tables too",
						colName)));

	if (tab->relkind == RELKIND_COMPOSITE_TYPE)
		ATTypedTableRecursion(wqueue, rel, cmd, lockmode, context);
}

/*
 * When the data type of a column is changed, a rewrite might not be required
 * if the new type is sufficiently identical to the old one, and the USING
 * clause isn't trying to insert some other value.  It's safe to skip the
 * rewrite in these cases:
 *
 * 改变列的数据类型时，若新类型与旧类型足够相同，且 USING 子句并不是要塞进别的值，也许不必重写。下列情况可以安全地跳过重写：
 *
 * - the old type is binary coercible to the new type
 * - the new type is an unconstrained domain over the old type
 * - {NEW,OLD} or {OLD,NEW} is {timestamptz,timestamp} and the timezone is UTC
 *
 * 旧类型可以二进制转换到新类型；新类型是旧类型上没有约束的域；{NEW,OLD} 或 {OLD,NEW} 是
 * {timestamptz,timestamp} 且时区是 UTC
 *
 * In the case of a constrained domain, we could get by with scanning the
 * table and checking the constraint rather than actually rewriting it, but we
 * don't currently try to do that.
 *
 * 对有约束的域，本可以只扫描表并检查约束，而不真正重写，但目前我们不这么做。
 */
static bool
ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno)
{
	Assert(expr != NULL);

	for (;;)
	{
		/* only one varno, so no need to check that */
		/*
		 *
		 * 只有一个 varno，所以不必检查那个
		 */
		if (IsA(expr, Var) && ((Var *) expr)->varattno == varattno)
			return false;
		else if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		else if (IsA(expr, CoerceToDomain))
		{
			CoerceToDomain *d = (CoerceToDomain *) expr;

			if (DomainHasConstraints(d->resulttype))
				return true;
			expr = (Node *) d->arg;
		}
		else if (IsA(expr, FuncExpr))
		{
			FuncExpr   *f = (FuncExpr *) expr;

			switch (f->funcid)
			{
				case F_TIMESTAMPTZ_TIMESTAMP:
				case F_TIMESTAMP_TIMESTAMPTZ:
					if (TimestampTimestampTzRequiresRewrite())
						return true;
					else
						expr = linitial(f->args);
					break;
				default:
					return true;
			}
		}
		else
			return true;
	}
}

/*
 * ALTER COLUMN .. SET DATA TYPE
 *
 * ALTER COLUMN .. SET DATA TYPE（修改列数据类型）
 *
 * Return the address of the modified column.
 *
 * 返回被修改列的地址。
 */
static ObjectAddress
ATExecAlterColumnType(AlteredTableInfo *tab, Relation rel,
					  AlterTableCmd *cmd, LOCKMODE lockmode)
{
	char	   *colName = cmd->name;
	ColumnDef  *def = (ColumnDef *) cmd->def;
	TypeName   *typeName = def->typeName;
	HeapTuple	heapTup;
	Form_pg_attribute attTup,
				attOldTup;
	AttrNumber	attnum;
	HeapTuple	typeTuple;
	Form_pg_type tform;
	Oid			targettype;
	int32		targettypmod;
	Oid			targetcollid;
	Node	   *defaultexpr;
	Relation	attrelation;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;
	ObjectAddress address;

	/*
	 * Clear all the missing values if we're rewriting the table, since this
	 * renders them pointless.
	 *
	 * 若正在重写表，清掉所有缺失值，因为重写之后它们就没意义了。
	 */
	if (tab->rewrite)
	{
		Relation	newrel;

		newrel = table_open(RelationGetRelid(rel), NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
		/* make sure we don't conflict with later attribute modifications */
		/*
		 *
		 * 确保不和后面的属性修改冲突
		 */
		CommandCounterIncrement();
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	/* Look up the target column */
	/*
	 *
	 * 查找目标列
	 */
	heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(heapTup)) /* shouldn't happen */
					/*
					 *
					 * 不该发生
					 */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
	attnum = attTup->attnum;
	attOldTup = TupleDescAttr(tab->oldDesc, attnum - 1);

	/* Check for multiple ALTER TYPE on same column --- can't cope */
	/*
	 *
	 * 检查是否对同一列做了多次 ALTER TYPE，这种情况应付不了
	 */
	if (attTup->atttypid != attOldTup->atttypid ||
		attTup->atttypmod != attOldTup->atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter type of column \"%s\" twice",
						colName)));

	/* Look up the target type (should not fail, since prep found it) */
	/*
	 *
	 * 查找目标类型（不应失败，因为准备阶段已经找到它）
	 */
	typeTuple = typenameType(NULL, typeName, &targettypmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = tform->oid;
	/* And the collation */
	/*
	 *
	 * 以及排序规则
	 */
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/*
	 * If there is a default expression for the column, get it and ensure we
	 * can coerce it to the new datatype.  (We must do this before changing
	 * the column type, because build_column_default itself will try to
	 * coerce, and will not issue the error message we want if it fails.)
	 *
	 * 若该列有默认表达式，取出来并确认能强制转换到新数据类型。（必须在改列类型之前做，因为 build_column_default
	 * 自己会尝试转换，失败时发出的不是我们想要的错误信息。）
	 *
	 * We remove any implicit coercion steps at the top level of the old
	 * default expression; this has been agreed to satisfy the principle of
	 * least surprise.  (The conversion to the new column type should act like
	 * it started from what the user sees as the stored expression, and the
	 * implicit coercions aren't going to be shown.)
	 *
	 * 我们去掉旧默认表达式顶层的任何隐式强制转换；已经商定这样最符合最少惊讶原则。（转换到新列类型应当像是从用户看到的已存储表达式开始，
	 * 而隐式转换不会被显示出来。）
	 */
	if (attTup->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		Assert(defaultexpr);
		defaultexpr = strip_implicit_coercions(defaultexpr);
		defaultexpr = coerce_to_target_type(NULL,	/* no UNKNOWN params */
								/*
								 *
								 * 没有 UNKNOWN 参数
								 */
											defaultexpr, exprType(defaultexpr),
											targettype, targettypmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (defaultexpr == NULL)
		{
			if (attTup->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("generation expression for column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("default for column \"%s\" cannot be cast automatically to type %s",
								colName, format_type_be(targettype))));
		}
	}
	else
		defaultexpr = NULL;

	/*
	 * Find everything that depends on the column (constraints, indexes, etc),
	 * and record enough information to let us recreate the objects.
	 *
	 * 找出所有依赖该列的东西（约束、索引等），并记下足够信息以便重建这些对象。
	 *
	 * The actual recreation does not happen here, but only after we have
	 * performed all the individual ALTER TYPE operations.  We have to save
	 * the info before executing ALTER TYPE, though, else the deparser will
	 * get confused.
	 *
	 * 真正的重建不在这里发生，而要等所有单独的 ALTER TYPE 操作都做完。但必须在执行 ALTER TYPE 之前保存这些信息，
	 * 否则反解析器会搞混。
	 */
	RememberAllDependentForRebuilding(tab, AT_AlterColumnType, rel, attnum, colName);

	/*
	 * Now scan for dependencies of this column on other things.  The only
	 * things we should find are the dependency on the column datatype and
	 * possibly a collation dependency.  Those can be removed.
	 *
	 * 现在扫描这一列对其他东西的依赖。我们应当只找到对列数据类型的依赖，以及可能的排序规则依赖。那些可以去掉。
	 */
	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		foundObject.classId = foundDep->refclassid;
		foundObject.objectId = foundDep->refobjid;
		foundObject.objectSubId = foundDep->refobjsubid;

		if (foundDep->deptype != DEPENDENCY_NORMAL)
			elog(ERROR, "found unexpected dependency type '%c'",
				 foundDep->deptype);
		if (!(foundDep->refclassid == TypeRelationId &&
			  foundDep->refobjid == attTup->atttypid) &&
			!(foundDep->refclassid == CollationRelationId &&
			  foundDep->refobjid == attTup->attcollation))
			elog(ERROR, "found unexpected dependency for column: %s",
				 getObjectDescription(&foundObject, false));

		CatalogTupleDelete(depRel, &depTup->t_self);
	}

	systable_endscan(scan);

	table_close(depRel, RowExclusiveLock);

	/*
	 * Here we go --- change the recorded column type and collation.  (Note
	 * heapTup is a copy of the syscache entry, so okay to scribble on.) First
	 * fix up the missing value if any.
	 *
	 * 开始改记录的列类型和排序规则。（注意 heapTup 是系统缓存项的副本，所以可以涂改。）若有缺失值，先修正它。
	 */
	if (attTup->atthasmissing)
	{
		Datum		missingval;
		bool		missingNull;

		/* if rewrite is true the missing value should already be cleared */
		/*
		 *
		 * 若 rewrite 为真，缺失值应该已经被清掉
		 */
		Assert(tab->rewrite == 0);

		/* Get the missing value datum */
		/*
		 *
		 * 取出缺失值的 datum
		 */
		missingval = heap_getattr(heapTup,
								  Anum_pg_attribute_attmissingval,
								  attrelation->rd_att,
								  &missingNull);

		/* if it's a null array there is nothing to do */
		/*
		 *
		 * 若它是空数组，则无事可做
		 */

		if (!missingNull)
		{
			/*
			 * Get the datum out of the array and repack it in a new array
			 * built with the new type data. We assume that since the table
			 * doesn't need rewriting, the actual Datum doesn't need to be
			 * changed, only the array metadata.
			 *
			 * 从数组里取出 datum，用新类型数据重新打包进一个新数组。假定既然表不需要重写，实际的 Datum 不必改，只改数组元数据。
			 */

			int			one = 1;
			bool		isNull;
			Datum		valuesAtt[Natts_pg_attribute] = {0};
			bool		nullsAtt[Natts_pg_attribute] = {0};
			bool		replacesAtt[Natts_pg_attribute] = {0};
			HeapTuple	newTup;

			missingval = array_get_element(missingval,
										   1,
										   &one,
										   0,
										   attTup->attlen,
										   attTup->attbyval,
										   attTup->attalign,
										   &isNull);
			missingval = PointerGetDatum(construct_array(&missingval,
														 1,
														 targettype,
														 tform->typlen,
														 tform->typbyval,
														 tform->typalign));

			valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
			replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;
			nullsAtt[Anum_pg_attribute_attmissingval - 1] = false;

			newTup = heap_modify_tuple(heapTup, RelationGetDescr(attrelation),
									   valuesAtt, nullsAtt, replacesAtt);
			heap_freetuple(heapTup);
			heapTup = newTup;
			attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
		}
	}

	attTup->atttypid = targettype;
	attTup->atttypmod = targettypmod;
	attTup->attcollation = targetcollid;
	if (list_length(typeName->arrayBounds) > PG_INT16_MAX)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many array dimensions"));
	attTup->attndims = list_length(typeName->arrayBounds);
	attTup->attlen = tform->typlen;
	attTup->attbyval = tform->typbyval;
	attTup->attalign = tform->typalign;
	attTup->attstorage = tform->typstorage;
	attTup->attcompression = InvalidCompressionMethod;

	ReleaseSysCache(typeTuple);

	CatalogTupleUpdate(attrelation, &heapTup->t_self, heapTup);

	table_close(attrelation, RowExclusiveLock);

	/* Install dependencies on new datatype and collation */
	/*
	 *
	 * 建立对新数据类型和排序规则的依赖
	 */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);
	add_column_collation_dependency(RelationGetRelid(rel), attnum, targetcollid);

	/*
	 * Drop any pg_statistic entry for the column, since it's now wrong type
	 *
	 * 删掉该列的 pg_statistic 项，因为类型已经不对了
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	/*
	 * Update the default, if present, by brute force --- remove and re-add
	 * the default.  Probably unsafe to take shortcuts, since the new version
	 * may well have additional dependencies.  (It's okay to do this now,
	 * rather than after other ALTER TYPE commands, since the default won't
	 * depend on other column types.)
	 *
	 * 若有默认值，用蛮力更新：删掉再重新加上。走捷径大概不安全，因为新版本很可能有额外依赖。（现在做没问题，不必等其他 ALTER
	 * TYPE 命令之后，因为默认值不会依赖其他列的类型。）
	 */
	if (defaultexpr)
	{
		/*
		 * If it's a GENERATED default, drop its dependency records, in
		 * particular its INTERNAL dependency on the column, which would
		 * otherwise cause dependency.c to refuse to perform the deletion.
		 *
		 * 若是 GENERATED 默认值，删掉它的依赖记录，尤其是它对列的 INTERNAL 依赖，否则 dependency.c
		 * 会拒绝执行删除。
		 */
		if (attTup->attgenerated)
		{
			Oid			attrdefoid = GetAttrDefaultOid(RelationGetRelid(rel), attnum);

			if (!OidIsValid(attrdefoid))
				elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
					 RelationGetRelid(rel), attnum);
			(void) deleteDependencyRecordsFor(AttrDefaultRelationId, attrdefoid, false);
		}

		/*
		 * Make updates-so-far visible, particularly the new pg_attribute row
		 * which will be updated again.
		 *
		 * 让目前为止的更新可见，尤其是将再次被更新的新 pg_attribute 行。
		 */
		CommandCounterIncrement();

		/*
		 * We use RESTRICT here for safety, but at present we do not expect
		 * anything to depend on the default.
		 *
		 * 为安全起见这里用 RESTRICT，但目前不指望有东西依赖这个默认值。
		 */
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true,
						  true);

		(void) StoreAttrDefault(rel, attnum, defaultexpr, true);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	/* Cleanup */
	/*
	 *
	 * 清理
	 */
	heap_freetuple(heapTup);

	return address;
}

/*
 * Subroutine for ATExecAlterColumnType and ATExecSetExpression: Find everything
 * that depends on the column (constraints, indexes, etc), and record enough
 * information to let us recreate the objects.
 *
 * ATExecAlterColumnType 和 ATExecSetExpression 的子程序：找出所有依赖该列的东西（约束、
 * 索引等），并记下足够信息以便重建这些对象。
 */
static void
RememberAllDependentForRebuilding(AlteredTableInfo *tab, AlterTableType subtype,
								  Relation rel, AttrNumber attnum, const char *colName)
{
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	depTup;

	Assert(subtype == AT_AlterColumnType || subtype == AT_SetExpression);

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress foundObject;

		foundObject.classId = foundDep->classid;
		foundObject.objectId = foundDep->objid;
		foundObject.objectSubId = foundDep->objsubid;

		switch (foundObject.classId)
		{
			case RelationRelationId:
				{
					char		relKind = get_rel_relkind(foundObject.objectId);

					if (relKind == RELKIND_INDEX ||
						relKind == RELKIND_PARTITIONED_INDEX)
					{
						Assert(foundObject.objectSubId == 0);
						RememberIndexForRebuilding(foundObject.objectId, tab);
					}
					else if (relKind == RELKIND_SEQUENCE)
					{
						/*
						 * This must be a SERIAL column's sequence.  We need
						 * not do anything to it.
						 *
						 * 这必定是 SERIAL 列的序列。对它不必做任何事。
						 */
						Assert(foundObject.objectSubId == 0);
					}
					else
					{
						/* Not expecting any other direct dependencies... */
						/*
						 *
						 * 不指望还有其他直接依赖……
						 */
						elog(ERROR, "unexpected object depending on column: %s",
							 getObjectDescription(&foundObject, false));
					}
					break;
				}

			case ConstraintRelationId:
				Assert(foundObject.objectSubId == 0);
				RememberConstraintForRebuilding(foundObject.objectId, tab);
				break;

			case ProcedureRelationId:

				/*
				 * A new-style SQL function can depend on a column, if that
				 * column is referenced in the parsed function body.  Ideally
				 * we'd automatically update the function by deparsing and
				 * reparsing it, but that's risky and might well fail anyhow.
				 * FIXME someday.
				 *
				 * 新式 SQL 函数可以依赖一列，若函数体解析结果引用了该列。理想情况下我们会反解析再重新解析来自动更新函数，但那有风险，
				 * 而且很可能失败。FIXME 以后再说。
				 *
				 * This is only a problem for AT_AlterColumnType, not
				 * AT_SetExpression.
				 *
				 * 这只对 AT_AlterColumnType 是问题，对 AT_SetExpression 不是。
				 */
				if (subtype == AT_AlterColumnType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used by a function or procedure"),
							 errdetail("%s depends on column \"%s\"",
									   getObjectDescription(&foundObject, false),
									   colName)));
				break;

			case RewriteRelationId:

				/*
				 * View/rule bodies have pretty much the same issues as
				 * function bodies.  FIXME someday.
				 *
				 * 视图和规则体的问题和函数体差不多。FIXME 以后再说。
				 */
				if (subtype == AT_AlterColumnType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used by a view or rule"),
							 errdetail("%s depends on column \"%s\"",
									   getObjectDescription(&foundObject, false),
									   colName)));
				break;

			case TriggerRelationId:

				/*
				 * A trigger can depend on a column because the column is
				 * specified as an update target, or because the column is
				 * used in the trigger's WHEN condition.  The first case would
				 * not require any extra work, but the second case would
				 * require updating the WHEN expression, which has the same
				 * issues as above.  Since we can't easily tell which case
				 * applies, we punt for both.  FIXME someday.
				 *
				 * 触发器可以依赖一列，因为该列被指定为更新目标，或因为该列用在触发器的 WHEN 条件里。第一种情况不需要额外工作，第二种需要更新
				 * WHEN 表达式，问题和上面一样。既然不容易区分是哪种，两种都放弃。FIXME 以后再说。
				 */
				if (subtype == AT_AlterColumnType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used in a trigger definition"),
							 errdetail("%s depends on column \"%s\"",
									   getObjectDescription(&foundObject, false),
									   colName)));
				break;

			case PolicyRelationId:

				/*
				 * A policy can depend on a column because the column is
				 * specified in the policy's USING or WITH CHECK qual
				 * expressions.  It might be possible to rewrite and recheck
				 * the policy expression, but punt for now.  It's certainly
				 * easy enough to remove and recreate the policy; still, FIXME
				 * someday.
				 *
				 * 策略可以依赖一列，因为该列出现在策略的 USING 或 WITH CHECK 条件表达式里。也许可以重写并重新检查策略表达式，
				 * 但暂时放弃。去掉再重建策略当然很容易；不过 FIXME 以后再说。
				 */
				if (subtype == AT_AlterColumnType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used in a policy definition"),
							 errdetail("%s depends on column \"%s\"",
									   getObjectDescription(&foundObject, false),
									   colName)));
				break;

			case AttrDefaultRelationId:
				{
					ObjectAddress col = GetAttrDefaultColumnAddress(foundObject.objectId);

					if (col.objectId == RelationGetRelid(rel) &&
						col.objectSubId == attnum)
					{
						/*
						 * Ignore the column's own default expression.  The
						 * caller deals with it.
						 *
						 * 忽略该列自己的默认表达式。调用方会处理。
						 */
					}
					else
					{
						/*
						 * This must be a reference from the expression of a
						 * generated column elsewhere in the same table.
						 * Changing the type/generated expression of a column
						 * that is used by a generated column is not allowed
						 * by SQL standard, so just punt for now.  It might be
						 * doable with some thinking and effort.
						 *
						 * 这必定是同一张表里别处某个生成列的表达式对本列的引用。SQL 标准不允许改变被生成列使用的列的类型或生成表达式，所以暂时放弃。
						 * 多想想、多花点力气也许能做。
						 */
						if (subtype == AT_AlterColumnType)
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("cannot alter type of a column used by a generated column"),
									 errdetail("Column \"%s\" is used by generated column \"%s\".",
											   colName,
											   get_attname(col.objectId,
														   col.objectSubId,
														   false))));
					}
					break;
				}

			case StatisticExtRelationId:

				/*
				 * Give the extended-stats machinery a chance to fix anything
				 * that this column type change would break.
				 *
				 * 给扩展统计机制一个机会，去修补这次列类型变更会弄坏的东西。
				 */
				RememberStatisticsForRebuilding(foundObject.objectId, tab);
				break;

			case PublicationRelRelationId:

				/*
				 * Column reference in a PUBLICATION ... FOR TABLE ... WHERE
				 * clause.  Same issues as above.  FIXME someday.
				 *
				 * PUBLICATION ... FOR TABLE ... WHERE 子句里的列引用。问题和上面一样。FIXME 以后再说。
				 */
				if (subtype == AT_AlterColumnType)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter type of a column used by a publication WHERE clause"),
							 errdetail("%s depends on column \"%s\"",
									   getObjectDescription(&foundObject, false),
									   colName)));
				break;

			default:

				/*
				 * We don't expect any other sorts of objects to depend on a
				 * column.
				 *
				 * 不指望还有其他种类的对象会依赖一列。
				 */
				elog(ERROR, "unexpected object depending on column: %s",
					 getObjectDescription(&foundObject, false));
				break;
		}
	}

	systable_endscan(scan);
	table_close(depRel, NoLock);
}

/*
 * Subroutine for ATExecAlterColumnType: remember that a replica identity
 * needs to be reset.
 *
 * ATExecAlterColumnType 的子程序：记住需要重置复制标识。
 */
static void
RememberReplicaIdentityForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	if (!get_index_isreplident(indoid))
		return;

	if (tab->replicaIdentityIndex)
		elog(ERROR, "relation %u has multiple indexes marked as replica identity", tab->relid);

	tab->replicaIdentityIndex = get_rel_name(indoid);
}

/*
 * Subroutine for ATExecAlterColumnType: remember any clustered index.
 *
 * ATExecAlterColumnType 的子程序：记住任何聚簇索引。
 */
static void
RememberClusterOnForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	if (!get_index_isclustered(indoid))
		return;

	if (tab->clusterOnIndex)
		elog(ERROR, "relation %u has multiple clustered indexes", tab->relid);

	tab->clusterOnIndex = get_rel_name(indoid);
}

/*
 * Subroutine for ATExecAlterColumnType: remember that a constraint needs
 * to be rebuilt (which we might already know).
 *
 * ATExecAlterColumnType 的子程序：记住需要重建某个约束（我们也许已经知道）。
 */
static void
RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: we
	 * mustn't try to recreate the same constraint twice, and if a constraint
	 * depends on more than one column whose type is to be altered, we must
	 * capture its definition string before applying any of the column type
	 * changes.  ruleutils.c will get confused if we ask again later.
	 *
	 * 这个去重检查至关重要，原因有两个互不相关：绝不能试图重建同一个约束两次；若一个约束依赖多列且这些列的类型都要改，
	 * 必须在应用任何列类型变更之前抓住它的定义字符串。若以后再问，ruleutils.c 会搞混。
	 */
	if (!list_member_oid(tab->changedConstraintOids, conoid))
	{
		/* OK, capture the constraint's existing definition string */
		/*
		 *
		 * 好，抓住该约束现有的定义字符串
		 */
		char	   *defstring = pg_get_constraintdef_command(conoid);
		Oid			indoid;

		/*
		 * It is critical to create not-null constraints ahead of primary key
		 * indexes; otherwise, the not-null constraint would be created by the
		 * primary key, and the constraint name would be wrong.
		 *
		 * 必须在主键索引之前创建 NOT NULL 约束；否则 NOT NULL 约束会由主键创建，约束名就会不对。
		 */
		if (get_constraint_type(conoid) == CONSTRAINT_NOTNULL)
		{
			tab->changedConstraintOids = lcons_oid(conoid,
												   tab->changedConstraintOids);
			tab->changedConstraintDefs = lcons(defstring,
											   tab->changedConstraintDefs);
		}
		else
		{

			tab->changedConstraintOids = lappend_oid(tab->changedConstraintOids,
													 conoid);
			tab->changedConstraintDefs = lappend(tab->changedConstraintDefs,
												 defstring);
		}

		/*
		 * For the index of a constraint, if any, remember if it is used for
		 * the table's replica identity or if it is a clustered index, so that
		 * ATPostAlterTypeCleanup() can queue up commands necessary to restore
		 * those properties.
		 *
		 * 对约束的索引（若有），记住它是否用于表的复制标识，或是否为聚簇索引，以便 ATPostAlterTypeCleanup()
		 * 排队恢复这些属性所需的命令。
		 */
		indoid = get_constraint_index(conoid);
		if (OidIsValid(indoid))
		{
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}

/*
 * Subroutine for ATExecAlterColumnType: remember that an index needs
 * to be rebuilt (which we might already know).
 *
 * ATExecAlterColumnType 的子程序：记住需要重建某个索引（我们也许已经知道）。
 */
static void
RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: we
	 * mustn't try to recreate the same index twice, and if an index depends
	 * on more than one column whose type is to be altered, we must capture
	 * its definition string before applying any of the column type changes.
	 * ruleutils.c will get confused if we ask again later.
	 *
	 * 这个去重检查至关重要，原因有两个互不相关：绝不能试图重建同一个索引两次；若一个索引依赖多列且这些列的类型都要改，
	 * 必须在应用任何列类型变更之前抓住它的定义字符串。若以后再问，ruleutils.c 会搞混。
	 */
	if (!list_member_oid(tab->changedIndexOids, indoid))
	{
		/*
		 * Before adding it as an index-to-rebuild, we'd better see if it
		 * belongs to a constraint, and if so rebuild the constraint instead.
		 * Typically this check fails, because constraint indexes normally
		 * have only dependencies on their constraint.  But it's possible for
		 * such an index to also have direct dependencies on table columns,
		 * for example with a partial exclusion constraint.
		 *
		 * 把它加入待重建索引之前，最好看看它是否属于某个约束，若是就改为重建约束。通常这项检查会失败，因为约束索引一般只依赖它们的约束。
		 * 但这种索引也可能直接依赖表列，例如带部分排他约束时。
		 */
		Oid			conoid = get_index_constraint(indoid);

		if (OidIsValid(conoid))
		{
			RememberConstraintForRebuilding(conoid, tab);
		}
		else
		{
			/* OK, capture the index's existing definition string */
			/*
			 *
			 * 好，抓住该索引现有的定义字符串
			 */
			char	   *defstring = pg_get_indexdef_string(indoid);

			tab->changedIndexOids = lappend_oid(tab->changedIndexOids,
												indoid);
			tab->changedIndexDefs = lappend(tab->changedIndexDefs,
											defstring);

			/*
			 * Remember if this index is used for the table's replica identity
			 * or if it is a clustered index, so that ATPostAlterTypeCleanup()
			 * can queue up commands necessary to restore those properties.
			 *
			 * 记住这个索引是否用于表的复制标识，或是否为聚簇索引，以便 ATPostAlterTypeCleanup()
			 * 排队恢复这些属性所需的命令。
			 */
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}

/*
 * Subroutine for ATExecAlterColumnType: remember that a statistics object
 * needs to be rebuilt (which we might already know).
 *
 * ATExecAlterColumnType 的子程序：记住需要重建某个统计对象（我们也许已经知道）。
 */
static void
RememberStatisticsForRebuilding(Oid stxoid, AlteredTableInfo *tab)
{
	/*
	 * This de-duplication check is critical for two independent reasons: we
	 * mustn't try to recreate the same statistics object twice, and if the
	 * statistics object depends on more than one column whose type is to be
	 * altered, we must capture its definition string before applying any of
	 * the type changes. ruleutils.c will get confused if we ask again later.
	 *
	 * 这个去重检查至关重要，原因有两个互不相关：绝不能试图重建同一个统计对象两次；若统计对象依赖多列且这些列的类型都要改，
	 * 必须在应用任何类型变更之前抓住它的定义字符串。若以后再问，ruleutils.c 会搞混。
	 */
	if (!list_member_oid(tab->changedStatisticsOids, stxoid))
	{
		/* OK, capture the statistics object's existing definition string */
		/*
		 *
		 * 好，抓住该统计对象现有的定义字符串
		 */
		char	   *defstring = pg_get_statisticsobjdef_string(stxoid);

		tab->changedStatisticsOids = lappend_oid(tab->changedStatisticsOids,
												 stxoid);
		tab->changedStatisticsDefs = lappend(tab->changedStatisticsDefs,
											 defstring);
	}
}

/*
 * Cleanup after we've finished all the ALTER TYPE or SET EXPRESSION
 * operations for a particular relation.  We have to drop and recreate all the
 * indexes and constraints that depend on the altered columns.  We do the
 * actual dropping here, but re-creation is managed by adding work queue
 * entries to do those steps later.
 *
 * 某张关系上所有 ALTER TYPE 或 SET EXPRESSION 都做完后的清理。必须删掉并重建所有依赖被改列的索引和约束。
 * 真正的删除在这里做，重建则通过加入工作队列项，留到后面那些步骤去做。
 */
static void
ATPostAlterTypeCleanup(List **wqueue, AlteredTableInfo *tab, LOCKMODE lockmode)
{
	ObjectAddress obj;
	ObjectAddresses *objects;
	ListCell   *def_item;
	ListCell   *oid_item;

	/*
	 * Collect all the constraints and indexes to drop so we can process them
	 * in a single call.  That way we don't have to worry about dependencies
	 * among them.
	 *
	 * 收集所有要删除的约束和索引，以便一次调用处理完。这样不必担心它们之间的依赖。
	 */
	objects = new_object_addresses();

	/*
	 * Re-parse the index and constraint definitions, and attach them to the
	 * appropriate work queue entries.  We do this before dropping because in
	 * the case of a constraint on another table, we might not yet have
	 * exclusive lock on the table the constraint is attached to, and we need
	 * to get that before reparsing/dropping.  (That's possible at least for
	 * FOREIGN KEY, CHECK, and EXCLUSION constraints; in non-FK cases it
	 * requires a dependency on the target table's composite type in the other
	 * table's constraint expressions.)
	 *
	 * 重新解析索引和约束定义，并把它们挂到相应的工作队列项上。在删除之前做，是因为若约束在另一张表上，
	 * 我们可能还没有对约束所附的表加排他锁，而重新解析和删除之前需要拿到锁。（至少 FOREIGN KEY、CHECK 和
	 * EXCLUSION 约束可能这样；非外键的情况下，需要另一张表的约束表达式依赖目标表的复合类型。）
	 *
	 * We can't rely on the output of deparsing to tell us which relation to
	 * operate on, because concurrent activity might have made the name
	 * resolve differently.  Instead, we've got to use the OID of the
	 * constraint or index we're processing to figure out which relation to
	 * operate on.
	 *
	 * 不能靠反解析的输出来判断要操作哪张关系，因为并发活动可能让名字解析到不同对象。必须用正在处理的约束或索引的 OID
	 * 来弄清要操作哪张关系。
	 */
	forboth(oid_item, tab->changedConstraintOids,
			def_item, tab->changedConstraintDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		HeapTuple	tup;
		Form_pg_constraint con;
		Oid			relid;
		Oid			confrelid;
		bool		conislocal;

		tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
		if (!HeapTupleIsValid(tup)) /* should not happen */
					    /*
					     *
					     * 不应发生
					     */
			elog(ERROR, "cache lookup failed for constraint %u", oldId);
		con = (Form_pg_constraint) GETSTRUCT(tup);
		if (OidIsValid(con->conrelid))
			relid = con->conrelid;
		else
		{
			/* must be a domain constraint */
			/*
			 *
			 * 必须是域约束
			 */
			relid = get_typ_typrelid(getBaseType(con->contypid));
			if (!OidIsValid(relid))
				elog(ERROR, "could not identify relation associated with constraint %u", oldId);
		}
		confrelid = con->confrelid;
		conislocal = con->conislocal;
		ReleaseSysCache(tup);

		ObjectAddressSet(obj, ConstraintRelationId, oldId);
		add_exact_object_address(&obj, objects);

		/*
		 * If the constraint is inherited (only), we don't want to inject a
		 * new definition here; it'll get recreated when
		 * ATAddCheckNNConstraint recurses from adding the parent table's
		 * constraint.  But we had to carry the info this far so that we can
		 * drop the constraint below.
		 *
		 * 若约束（仅仅）是继承来的，我们不想在这里注入新定义；给父表加约束时 ATAddCheckNNConstraint 递归会重建它。
		 * 但信息必须带到这一步，下面才能删掉该约束。
		 */
		if (!conislocal)
			continue;

		/*
		 * When rebuilding another table's constraint that references the
		 * table we're modifying, we might not yet have any lock on the other
		 * table, so get one now.  We'll need AccessExclusiveLock for the DROP
		 * CONSTRAINT step, so there's no value in asking for anything weaker.
		 *
		 * 重建另一张表上引用我们正在修改的表的约束时，可能还没锁那张表，所以现在拿一把。DROP CONSTRAINT 需要
		 * AccessExclusiveLock，要更弱的锁没有意义。
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, AccessExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, confrelid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);
	}
	forboth(oid_item, tab->changedIndexOids,
			def_item, tab->changedIndexDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = IndexGetRelation(oldId, false);

		/*
		 * As above, make sure we have lock on the index's table if it's not
		 * the same table.
		 *
		 * 同上，若索引的表不是同一张表，确保锁住索引的表。
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, AccessExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, RelationRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/* add dependencies for new statistics */
	/*
	 *
	 * 为新统计对象加上依赖
	 */
	forboth(oid_item, tab->changedStatisticsOids,
			def_item, tab->changedStatisticsDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = StatisticsGetRelation(oldId, false);

		/*
		 * As above, make sure we have lock on the statistics object's table
		 * if it's not the same table.  However, we take
		 * ShareUpdateExclusiveLock here, aligning with the lock level used in
		 * CreateStatistics and RemoveStatisticsById.
		 *
		 * 同上，若统计对象的表不是同一张表，确保锁住它。不过这里取 ShareUpdateExclusiveLock，与
		 * CreateStatistics 和 RemoveStatisticsById 用的锁级别一致。
		 *
		 * CAUTION: this should be done after all cases that grab
		 * AccessExclusiveLock, else we risk causing deadlock due to needing
		 * to promote our table lock.
		 *
		 * 注意：这应在所有获取 AccessExclusiveLock 的情况之后做，否则可能因为需要提升表锁而导致死锁。
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, ShareUpdateExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, StatisticExtRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/*
	 * Queue up command to restore replica identity index marking
	 *
	 * 排队一条命令，恢复复制标识索引的标记
	 */
	if (tab->replicaIdentityIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);
		ReplicaIdentityStmt *subcmd = makeNode(ReplicaIdentityStmt);

		subcmd->identity_type = REPLICA_IDENTITY_INDEX;
		subcmd->name = tab->replicaIdentityIndex;
		cmd->subtype = AT_ReplicaIdentity;
		cmd->def = (Node *) subcmd;

		/* do it after indexes and constraints */
		/*
		 *
		 * 放在索引和约束之后做
		 */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * Queue up command to restore marking of index used for cluster.
	 *
	 * 排队一条命令，恢复用于聚簇的索引标记。
	 */
	if (tab->clusterOnIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_ClusterOn;
		cmd->name = tab->clusterOnIndex;

		/* do it after indexes and constraints */
		/*
		 *
		 * 放在索引和约束之后做
		 */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * It should be okay to use DROP_RESTRICT here, since nothing else should
	 * be depending on these objects.
	 *
	 * 这里用 DROP_RESTRICT 应该没问题，因为不该有别的东西依赖这些对象。
	 */
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	free_object_addresses(objects);

	/*
	 * The objects will get recreated during subsequent passes over the work
	 * queue.
	 *
	 * 这些对象会在随后遍历工作队列时重建。
	 */
}

/*
 * Parse the previously-saved definition string for a constraint, index or
 * statistics object against the newly-established column data type(s), and
 * queue up the resulting command parsetrees for execution.
 *
 * 按新确定的列数据类型，解析先前保存的约束、索引或统计对象定义字符串，并把得到的命令语法树排队执行。
 *
 * This might fail if, for example, you have a WHERE clause that uses an
 * operator that's not available for the new column type.
 *
 * 这可能失败，例如 WHERE 子句用了新列类型没有的操作符。
 */
static void
ATPostAlterTypeParse(Oid oldId, Oid oldRelId, Oid refRelId, char *cmd,
					 List **wqueue, LOCKMODE lockmode, bool rewrite)
{
	List	   *raw_parsetree_list;
	List	   *querytree_list;
	ListCell   *list_item;
	Relation	rel;

	/*
	 * We expect that we will get only ALTER TABLE and CREATE INDEX
	 * statements. Hence, there is no need to pass them through
	 * parse_analyze_*() or the rewriter, but instead we need to pass them
	 * through parse_utilcmd.c to make them ready for execution.
	 *
	 * 我们指望只会得到 ALTER TABLE 和 CREATE INDEX 语句。因此不必让它们走 parse_analyze_*()
	 * 或重写器，但需要让它们走 parse_utilcmd.c，以便准备好执行。
	 */
	raw_parsetree_list = raw_parser(cmd, RAW_PARSE_DEFAULT);
	querytree_list = NIL;
	foreach(list_item, raw_parsetree_list)
	{
		RawStmt    *rs = lfirst_node(RawStmt, list_item);
		Node	   *stmt = rs->stmt;

		if (IsA(stmt, IndexStmt))
			querytree_list = lappend(querytree_list,
									 transformIndexStmt(oldRelId,
														(IndexStmt *) stmt,
														cmd));
		else if (IsA(stmt, AlterTableStmt))
		{
			List	   *beforeStmts;
			List	   *afterStmts;

			stmt = (Node *) transformAlterTableStmt(oldRelId,
													(AlterTableStmt *) stmt,
													cmd,
													&beforeStmts,
													&afterStmts);
			querytree_list = list_concat(querytree_list, beforeStmts);
			querytree_list = lappend(querytree_list, stmt);
			querytree_list = list_concat(querytree_list, afterStmts);
		}
		else if (IsA(stmt, CreateStatsStmt))
			querytree_list = lappend(querytree_list,
									 transformStatsStmt(oldRelId,
														(CreateStatsStmt *) stmt,
														cmd));
		else
			querytree_list = lappend(querytree_list, stmt);
	}

	/* Caller should already have acquired whatever lock we need. */
	/*
	 *
	 * 调用方应该已经拿到我们需要的锁。
	 */
	rel = relation_open(oldRelId, NoLock);

	/*
	 * Attach each generated command to the proper place in the work queue.
	 * Note this could result in creation of entirely new work-queue entries.
	 *
	 * 把每条生成的命令挂到工作队列的正确位置。注意这可能导致创建全新的工作队列项。
	 *
	 * Also note that we have to tweak the command subtypes, because it turns
	 * out that re-creation of indexes and constraints has to act a bit
	 * differently from initial creation.
	 *
	 * 还要注意必须调整命令子类型，因为事实证明重建索引和约束与初次创建的行为必须略有不同。
	 */
	foreach(list_item, querytree_list)
	{
		Node	   *stm = (Node *) lfirst(list_item);
		AlteredTableInfo *tab;

		tab = ATGetQueueEntry(wqueue, rel);

		if (IsA(stm, IndexStmt))
		{
			IndexStmt  *stmt = (IndexStmt *) stm;
			AlterTableCmd *newcmd;

			if (!rewrite)
				TryReuseIndex(oldId, stmt);
			stmt->reset_default_tblspc = true;
			/* keep the index's comment */
			/*
			 *
			 * 保留索引的注释
			 */
			stmt->idxcomment = GetComment(oldId, RelationRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddIndex;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_OLD_INDEX] =
				lappend(tab->subcmds[AT_PASS_OLD_INDEX], newcmd);
		}
		else if (IsA(stm, AlterTableStmt))
		{
			AlterTableStmt *stmt = (AlterTableStmt *) stm;
			ListCell   *lcmd;

			foreach(lcmd, stmt->cmds)
			{
				AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lcmd);

				if (cmd->subtype == AT_AddIndex)
				{
					IndexStmt  *indstmt;
					Oid			indoid;

					indstmt = castNode(IndexStmt, cmd->def);
					indoid = get_constraint_index(oldId);

					if (!rewrite)
						TryReuseIndex(indoid, indstmt);
					/* keep any comment on the index */
					/*
					 *
					 * 保留索引上的任何注释
					 */
					indstmt->idxcomment = GetComment(indoid,
													 RelationRelationId, 0);
					indstmt->reset_default_tblspc = true;

					cmd->subtype = AT_ReAddIndex;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);

					/* recreate any comment on the constraint */
					/*
					 *
					 * 重建约束上的任何注释
					 */
					RebuildConstraintComment(tab,
											 AT_PASS_OLD_INDEX,
											 oldId,
											 rel,
											 NIL,
											 indstmt->idxname);
				}
				else if (cmd->subtype == AT_AddConstraint)
				{
					Constraint *con = castNode(Constraint, cmd->def);

					con->old_pktable_oid = refRelId;
					/* rewriting neither side of a FK */
					/*
					 *
					 * 外键的两边都没有在重写
					 */
					if (con->contype == CONSTR_FOREIGN &&
						!rewrite && tab->rewrite == 0)
						TryReuseForeignKey(oldId, con);
					con->reset_default_tblspc = true;
					cmd->subtype = AT_ReAddConstraint;
					tab->subcmds[AT_PASS_OLD_CONSTR] =
						lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

					/*
					 * Recreate any comment on the constraint.  If we have
					 * recreated a primary key, then transformTableConstraint
					 * has added an unnamed not-null constraint here; skip
					 * this in that case.
					 *
					 * 重建约束上的任何注释。若我们重建了主键，transformTableConstraint 会在这里加上一个未命名的 NOT
					 * NULL 约束；那种情况就跳过。
					 */
					if (con->conname)
						RebuildConstraintComment(tab,
												 AT_PASS_OLD_CONSTR,
												 oldId,
												 rel,
												 NIL,
												 con->conname);
					else
						Assert(con->contype == CONSTR_NOTNULL);
				}
				else
					elog(ERROR, "unexpected statement subtype: %d",
						 (int) cmd->subtype);
			}
		}
		else if (IsA(stm, AlterDomainStmt))
		{
			AlterDomainStmt *stmt = (AlterDomainStmt *) stm;

			if (stmt->subtype == 'C')	/* ADD CONSTRAINT */
							/*
							 *
							 * ADD CONSTRAINT（加约束）
							 */
			{
				Constraint *con = castNode(Constraint, stmt->def);
				AlterTableCmd *cmd = makeNode(AlterTableCmd);

				cmd->subtype = AT_ReAddDomainConstraint;
				cmd->def = (Node *) stmt;
				tab->subcmds[AT_PASS_OLD_CONSTR] =
					lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

				/* recreate any comment on the constraint */
				/*
				 *
				 * 重建约束上的任何注释
				 */
				RebuildConstraintComment(tab,
										 AT_PASS_OLD_CONSTR,
										 oldId,
										 NULL,
										 stmt->typeName,
										 con->conname);
			}
			else
				elog(ERROR, "unexpected statement subtype: %d",
					 (int) stmt->subtype);
		}
		else if (IsA(stm, CreateStatsStmt))
		{
			CreateStatsStmt *stmt = (CreateStatsStmt *) stm;
			AlterTableCmd *newcmd;

			/* keep the statistics object's comment */
			/*
			 *
			 * 保留统计对象的注释
			 */
			stmt->stxcomment = GetComment(oldId, StatisticExtRelationId, 0);

			newcmd = makeNode(AlterTableCmd);
			newcmd->subtype = AT_ReAddStatistics;
			newcmd->def = (Node *) stmt;
			tab->subcmds[AT_PASS_MISC] =
				lappend(tab->subcmds[AT_PASS_MISC], newcmd);
		}
		else
			elog(ERROR, "unexpected statement type: %d",
				 (int) nodeTag(stm));
	}

	relation_close(rel, NoLock);
}

/*
 * Subroutine for ATPostAlterTypeParse() to recreate any existing comment
 * for a table or domain constraint that is being rebuilt.
 *
 * ATPostAlterTypeParse() 的子程序，为正在重建的表约束或域约束重建任何已有注释。
 *
 * objid is the OID of the constraint.
 * Pass "rel" for a table constraint, or "domname" (domain's qualified name
 * as a string list) for a domain constraint.
 * (We could dig that info, as well as the conname, out of the pg_constraint
 * entry; but callers already have them so might as well pass them.)
 *
 * objid 是约束的 OID。表约束传 rel，域约束传 domname（域的限定名，字符串列表）。（这些信息以及 conname
 * 可以从 pg_constraint 项里挖出来；但调用方已经有了，不妨传进来。）
 */
static void
RebuildConstraintComment(AlteredTableInfo *tab, AlterTablePass pass, Oid objid,
						 Relation rel, List *domname,
						 const char *conname)
{
	CommentStmt *cmd;
	char	   *comment_str;
	AlterTableCmd *newcmd;

	/* Look for comment for object wanted, and leave if none */
	/*
	 *
	 * 查找想要的对象的注释，没有就离开
	 */
	comment_str = GetComment(objid, ConstraintRelationId, 0);
	if (comment_str == NULL)
		return;

	/* Build CommentStmt node, copying all input data for safety */
	/*
	 *
	 * 构造 CommentStmt 节点，为安全起见拷贝所有输入数据
	 */
	cmd = makeNode(CommentStmt);
	if (rel)
	{
		cmd->objtype = OBJECT_TABCONSTRAINT;
		cmd->object = (Node *)
			list_make3(makeString(get_namespace_name(RelationGetNamespace(rel))),
					   makeString(pstrdup(RelationGetRelationName(rel))),
					   makeString(pstrdup(conname)));
	}
	else
	{
		cmd->objtype = OBJECT_DOMCONSTRAINT;
		cmd->object = (Node *)
			list_make2(makeTypeNameFromNameList(copyObject(domname)),
					   makeString(pstrdup(conname)));
	}
	cmd->comment = comment_str;

	/* Append it to list of commands */
	/*
	 *
	 * 把它追加到命令列表
	 */
	newcmd = makeNode(AlterTableCmd);
	newcmd->subtype = AT_ReAddComment;
	newcmd->def = (Node *) cmd;
	tab->subcmds[pass] = lappend(tab->subcmds[pass], newcmd);
}

/*
 * Subroutine for ATPostAlterTypeParse().  Calls out to CheckIndexCompatible()
 * for the real analysis, then mutates the IndexStmt based on that verdict.
 *
 * ATPostAlterTypeParse() 的子程序。真正的分析交给 CheckIndexCompatible()，
 * 然后根据结论修改 IndexStmt。
 */
static void
TryReuseIndex(Oid oldId, IndexStmt *stmt)
{
	if (CheckIndexCompatible(oldId,
							 stmt->accessMethod,
							 stmt->indexParams,
							 stmt->excludeOpNames,
							 stmt->iswithoutoverlaps))
	{
		Relation	irel = index_open(oldId, NoLock);

		/* If it's a partitioned index, there is no storage to share. */
		/*
		 *
		 * 若是分区索引，没有可共享的存储。
		 */
		if (irel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			stmt->oldNumber = irel->rd_locator.relNumber;
			stmt->oldCreateSubid = irel->rd_createSubid;
			stmt->oldFirstRelfilelocatorSubid = irel->rd_firstRelfilelocatorSubid;
		}
		index_close(irel, NoLock);
	}
}

/*
 * Subroutine for ATPostAlterTypeParse().
 *
 * ATPostAlterTypeParse() 的子程序。
 *
 * Stash the old P-F equality operator into the Constraint node, for possible
 * use by ATAddForeignKeyConstraint() in determining whether revalidation of
 * this constraint can be skipped.
 *
 * 把旧的主键-外键相等操作符存进 Constraint 节点，供 ATAddForeignKeyConstraint()
 * 判断能否跳过重新验证这个约束。
 */
static void
TryReuseForeignKey(Oid oldId, Constraint *con)
{
	HeapTuple	tup;
	Datum		adatum;
	ArrayType  *arr;
	Oid		   *rawarr;
	int			numkeys;
	int			i;

	Assert(con->contype == CONSTR_FOREIGN);
	Assert(con->old_conpfeqop == NIL);	/* already prepared this node */
						/*
						 *
						 * 这个节点已经准备过
						 */

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tup)) /* should not happen */
				    /*
				     *
				     * 不应发生
				     */
		elog(ERROR, "cache lookup failed for constraint %u", oldId);

	adatum = SysCacheGetAttrNotNull(CONSTROID, tup,
									Anum_pg_constraint_conpfeqop);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
						/*
						 *
						 * 确保没有被 toast
						 */
	numkeys = ARR_DIMS(arr)[0];
	/* test follows the one in ri_FetchConstraintInfo() */
	/*
	 *
	 * 测试与 ri_FetchConstraintInfo() 里的一致
	 */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conpfeqop is not a 1-D Oid array");
	rawarr = (Oid *) ARR_DATA_PTR(arr);

	/* stash a List of the operator Oids in our Constraint node */
	/*
	 *
	 * 把操作符 OID 的 List 存进我们的 Constraint 节点
	 */
	for (i = 0; i < numkeys; i++)
		con->old_conpfeqop = lappend_oid(con->old_conpfeqop, rawarr[i]);

	ReleaseSysCache(tup);
}

/*
 * ALTER COLUMN .. OPTIONS ( ... )
 *
 * ALTER COLUMN .. OPTIONS ( ... )（修改列选项）
 *
 * Returns the address of the modified column
 *
 * 返回被修改列的地址
 */
static ObjectAddress
ATExecAlterColumnGenericOptions(Relation rel,
								const char *colName,
								List *options,
								LOCKMODE lockmode)
{
	Relation	ftrel;
	Relation	attrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Datum		datum;
	Form_pg_foreign_table fttableform;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	ObjectAddress address;

	if (options == NIL)
		return InvalidObjectAddress;

	/* First, determine FDW validator associated to the foreign table. */
	/*
	 *
	 * 首先，确定与该外部表关联的 FDW 校验器。
	 */
	ftrel = table_open(ForeignTableRelationId, AccessShareLock);
	tuple = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(rel->rd_id));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	fttableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(fttableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	table_close(ftrel, AccessShareLock);
	ReleaseSysCache(tuple);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/* Prevent them from altering a system attribute */
	/*
	 *
	 * 阻止他们修改系统属性
	 */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", colName)));


	/* Initialize buffers for new tuple values */
	/*
	 *
	 * 为新元组值初始化缓冲区
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the current options */
	/*
	 *
	 * 取出当前选项
	 */
	datum = SysCacheGetAttr(ATTNAME,
							tuple,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the options */
	/*
	 *
	 * 变换这些选项
	 */
	datum = transformGenericOptions(AttributeRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_attribute_attfdwoptions - 1] = datum;
	else
		repl_null[Anum_pg_attribute_attfdwoptions - 1] = true;

	repl_repl[Anum_pg_attribute_attfdwoptions - 1] = true;

	/* Everything looks good - update the tuple */
	/*
	 *
	 * 一切看起来都好，更新元组
	 */

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrel),
								 repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(attrel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  atttableform->attnum);
	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	ReleaseSysCache(tuple);

	table_close(attrel, RowExclusiveLock);

	heap_freetuple(newtuple);

	return address;
}

/*
 * ALTER TABLE OWNER
 *
 * ALTER TABLE OWNER（修改表属主）
 *
 * recursing is true if we are recursing from a table to its indexes,
 * sequences, or toast table.  We don't allow the ownership of those things to
 * be changed separately from the parent table.  Also, we can skip permission
 * checks (this is necessary not just an optimization, else we'd fail to
 * handle toast tables properly).
 *
 * recursing 为真表示我们正从表递归到它的索引、序列或 TOAST 表。不允许单独改变这些东西的属主，必须和父表一起改。
 * 同时可以跳过权限检查（这不仅是优化，否则无法正确处理 TOAST 表）。
 *
 * recursing is also true if ALTER TYPE OWNER is calling us to fix up a
 * free-standing composite type.
 *
 * 若 ALTER TYPE OWNER 调用我们来修正独立的复合类型，recursing 也为真。
 */
void
ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode)
{
	Relation	target_rel;
	Relation	class_rel;
	HeapTuple	tuple;
	Form_pg_class tuple_class;

	/*
	 * Get exclusive lock till end of transaction on the target table. Use
	 * relation_open so that we can work on indexes and sequences.
	 *
	 * 对目标表加排他锁直到事务结束。用 relation_open，这样也能处理索引和序列。
	 */
	target_rel = relation_open(relationOid, lockmode);

	/* Get its pg_class tuple, too */
	/*
	 *
	 * 也取出它的 pg_class 元组
	 */
	class_rel = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* Can we change the ownership of this tuple? */
	/*
	 *
	 * 我们能改变这个元组的属主吗？
	 */
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
			/* ok to change owner */
			/*
			 *
			 * 可以改属主
			 */
			break;
		case RELKIND_INDEX:
			if (!recursing)
			{
				/*
				 * Because ALTER INDEX OWNER used to be allowed, and in fact
				 * is generated by old versions of pg_dump, we give a warning
				 * and do nothing rather than erroring out.  Also, to avoid
				 * unnecessary chatter while restoring those old dumps, say
				 * nothing at all if the command would be a no-op anyway.
				 *
				 * 因为以前允许 ALTER INDEX OWNER，而且旧版 pg_dump 确实会生成它，我们发出警告并什么也不做，而不是报错。
				 * 另外，为避免恢复那些旧转储时不必要的唠叨，若命令本来就是空操作，就完全不说话。
				 */
				if (tuple_class->relowner != newOwnerId)
					ereport(WARNING,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot change owner of index \"%s\"",
									NameStr(tuple_class->relname)),
							 errhint("Change the ownership of the index's table instead.")));
				/* quick hack to exit via the no-op path */
				/*
				 *
				 * 快速绕道，从空操作路径退出
				 */
				newOwnerId = tuple_class->relowner;
			}
			break;
		case RELKIND_PARTITIONED_INDEX:
			if (recursing)
				break;
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change owner of index \"%s\"",
							NameStr(tuple_class->relname)),
					 errhint("Change the ownership of the index's table instead.")));
			break;
		case RELKIND_SEQUENCE:
			if (!recursing &&
				tuple_class->relowner != newOwnerId)
			{
				/* if it's an owned sequence, disallow changing it by itself */
				/*
				 *
				 * 若它是被拥有的序列，不允许单独改它
				 */
				Oid			tableId;
				int32		colId;

				if (sequenceIsOwned(relationOid, DEPENDENCY_AUTO, &tableId, &colId) ||
					sequenceIsOwned(relationOid, DEPENDENCY_INTERNAL, &tableId, &colId))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot change owner of sequence \"%s\"",
									NameStr(tuple_class->relname)),
							 errdetail("Sequence \"%s\" is linked to table \"%s\".",
									   NameStr(tuple_class->relname),
									   get_rel_name(tableId))));
			}
			break;
		case RELKIND_COMPOSITE_TYPE:
			if (recursing)
				break;
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a composite type",
							NameStr(tuple_class->relname)),
			/* translator: %s is an SQL ALTER command */
			/*
			 *
			 * 翻译提示：%s 是一条 SQL ALTER 命令
			 */
					 errhint("Use %s instead.",
							 "ALTER TYPE")));
			break;
		case RELKIND_TOASTVALUE:
			if (recursing)
				break;
			/* FALL THRU */
			/*
			 *
			 * 落入下面的处理
			 */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change owner of relation \"%s\"",
							NameStr(tuple_class->relname)),
					 errdetail_relkind_not_supported(tuple_class->relkind)));
	}

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 *
	 * 若新属主和现有属主相同，认为命令已经成功。这是为了转储恢复。
	 */
	if (tuple_class->relowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_class];
		bool		repl_null[Natts_pg_class];
		bool		repl_repl[Natts_pg_class];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* skip permission checks when recursing to index or toast table */
		/*
		 *
		 * 递归到索引或 TOAST 表时跳过权限检查
		 */
		if (!recursing)
		{
			/* Superusers can always do it */
			/*
			 *
			 * 超级用户总是可以做
			 */
			if (!superuser())
			{
				Oid			namespaceOid = tuple_class->relnamespace;
				AclResult	aclresult;

				/* Otherwise, must be owner of the existing object */
				/*
				 *
				 * 否则必须是现有对象的属主
				 */
				if (!object_ownercheck(RelationRelationId, relationOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relationOid)),
								   RelationGetRelationName(target_rel));

				/* Must be able to become new owner */
				/*
				 *
				 * 必须能够成为新属主
				 */
				check_can_set_role(GetUserId(), newOwnerId);

				/* New owner must have CREATE privilege on namespace */
				/*
				 *
				 * 新属主必须对命名空间有 CREATE 权限
				 */
				aclresult = object_aclcheck(NamespaceRelationId, namespaceOid, newOwnerId,
											ACL_CREATE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_SCHEMA,
								   get_namespace_name(namespaceOid));
			}
		}

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_class_relowner - 1] = true;
		repl_val[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 *
		 * 确定新属主对应的修改后 ACL。只有 ACL 非空时才有必要。
		 */
		aclDatum = SysCacheGetAttr(RELOID, tuple,
								   Anum_pg_class_relacl,
								   &isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 tuple_class->relowner, newOwnerId);
			repl_repl[Anum_pg_class_relacl - 1] = true;
			repl_val[Anum_pg_class_relacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(class_rel), repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(class_rel, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);

		/*
		 * We must similarly update any per-column ACLs to reflect the new
		 * owner; for neatness reasons that's split out as a subroutine.
		 *
		 * 同样必须更新所有按列的 ACL 以反映新属主；为了整洁，这拆成一个子程序。
		 */
		change_owner_fix_column_acls(relationOid,
									 tuple_class->relowner,
									 newOwnerId);

		/*
		 * Update owner dependency reference, if any.  A composite type has
		 * none, because it's tracked for the pg_type entry instead of here;
		 * indexes and TOAST tables don't have their own entries either.
		 *
		 * 若有属主依赖引用，也更新它。复合类型没有，因为它是跟着 pg_type 项而不是在这里跟踪的；索引和 TOAST 表也没有自己的项。
		 */
		if (tuple_class->relkind != RELKIND_COMPOSITE_TYPE &&
			tuple_class->relkind != RELKIND_INDEX &&
			tuple_class->relkind != RELKIND_PARTITIONED_INDEX &&
			tuple_class->relkind != RELKIND_TOASTVALUE)
			changeDependencyOnOwner(RelationRelationId, relationOid,
									newOwnerId);

		/*
		 * Also change the ownership of the table's row type, if it has one
		 *
		 * 若表有行类型，也改变该行类型的属主
		 */
		if (OidIsValid(tuple_class->reltype))
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId);

		/*
		 * If we are operating on a table or materialized view, also change
		 * the ownership of any indexes and sequences that belong to the
		 * relation, as well as its toast table (if it has one).
		 *
		 * 若操作的是表或物化视图，也改变属于该关系的所有索引和序列的属主，以及它的 TOAST 表（若有）。
		 */
		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_PARTITIONED_TABLE ||
			tuple_class->relkind == RELKIND_MATVIEW ||
			tuple_class->relkind == RELKIND_TOASTVALUE)
		{
			List	   *index_oid_list;
			ListCell   *i;

			/* Find all the indexes belonging to this relation */
			/*
			 *
			 * 找出属于这个关系的所有索引
			 */
			index_oid_list = RelationGetIndexList(target_rel);

			/* For each index, recursively change its ownership */
			/*
			 *
			 * 对每个索引，递归改变其属主
			 */
			foreach(i, index_oid_list)
				ATExecChangeOwner(lfirst_oid(i), newOwnerId, true, lockmode);

			list_free(index_oid_list);
		}

		/* If it has a toast table, recurse to change its ownership */
		/*
		 *
		 * 若有 TOAST 表，递归改变其属主
		 */
		if (tuple_class->reltoastrelid != InvalidOid)
			ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId,
							  true, lockmode);

		/* If it has dependent sequences, recurse to change them too */
		/*
		 *
		 * 若有依赖的序列，也递归改变它们
		 */
		change_owner_recurse_to_sequences(relationOid, newOwnerId, lockmode);
	}

	InvokeObjectPostAlterHook(RelationRelationId, relationOid, 0);

	ReleaseSysCache(tuple);
	table_close(class_rel, RowExclusiveLock);
	relation_close(target_rel, NoLock);
}

/*
 * change_owner_fix_column_acls
 *
 * 函数 change_owner_fix_column_acls
 *
 * Helper function for ATExecChangeOwner.  Scan the columns of the table
 * and fix any non-null column ACLs to reflect the new owner.
 *
 * ATExecChangeOwner 的辅助函数。扫描表的列，把所有非空的列 ACL 改成反映新属主。
 */
static void
change_owner_fix_column_acls(Oid relationOid, Oid oldOwnerId, Oid newOwnerId)
{
	Relation	attRelation;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	attributeTuple;

	attRelation = table_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	scan = systable_beginscan(attRelation, AttributeRelidNumIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);
		Datum		repl_val[Natts_pg_attribute];
		bool		repl_null[Natts_pg_attribute];
		bool		repl_repl[Natts_pg_attribute];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Ignore dropped columns */
		/*
		 *
		 * 忽略已删除的列
		 */
		if (att->attisdropped)
			continue;

		aclDatum = heap_getattr(attributeTuple,
								Anum_pg_attribute_attacl,
								RelationGetDescr(attRelation),
								&isNull);
		/* Null ACLs do not require changes */
		/*
		 *
		 * 空的 ACL 不需要修改
		 */
		if (isNull)
			continue;

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		newAcl = aclnewowner(DatumGetAclP(aclDatum),
							 oldOwnerId, newOwnerId);
		repl_repl[Anum_pg_attribute_attacl - 1] = true;
		repl_val[Anum_pg_attribute_attacl - 1] = PointerGetDatum(newAcl);

		newtuple = heap_modify_tuple(attributeTuple,
									 RelationGetDescr(attRelation),
									 repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(attRelation, &newtuple->t_self, newtuple);

		heap_freetuple(newtuple);
	}
	systable_endscan(scan);
	table_close(attRelation, RowExclusiveLock);
}

/*
 * change_owner_recurse_to_sequences
 *
 * 函数 change_owner_recurse_to_sequences
 *
 * Helper function for ATExecChangeOwner.  Examines pg_depend searching
 * for sequences that are dependent on serial columns, and changes their
 * ownership.
 *
 * ATExecChangeOwner 的辅助函数。检查 pg_depend，寻找依赖 serial 列的序列，并改变它们的属主。
 */
static void
change_owner_recurse_to_sequences(Oid relationOid, Oid newOwnerId, LOCKMODE lockmode)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL sequences are those having an auto dependency on one of the
	 * table's columns (we don't care *which* column, exactly).
	 *
	 * SERIAL 序列是对表的某一列有 auto 依赖的那些（我们并不在乎具体是哪一列）。
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationOid));
	/* we leave refobjsubid unspecified */
	/*
	 *
	 * 我们不指定 refobjsubid
	 */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* skip dependencies other than auto dependencies on columns */
		/*
		 *
		 * 跳过不是对列的 auto 依赖的那些依赖
		 */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			!(depForm->deptype == DEPENDENCY_AUTO || depForm->deptype == DEPENDENCY_INTERNAL))
			continue;

		/* Use relation_open just in case it's an index */
		/*
		 *
		 * 用 relation_open，以防万一它是索引
		 */
		seqRel = relation_open(depForm->objid, lockmode);

		/* skip non-sequence relations */
		/*
		 *
		 * 跳过非序列关系
		 */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			/*
			 *
			 * 不必保留这把锁
			 */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* We don't need to close the sequence while we alter it. */
		/*
		 *
		 * 修改序列时不必先关掉它。
		 */
		ATExecChangeOwner(depForm->objid, newOwnerId, true, lockmode);

		/* Now we can close it.  Keep the lock till end of transaction. */
		/*
		 *
		 * 现在可以关掉它。锁保持到事务结束。
		 */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}

/*
 * ALTER TABLE CLUSTER ON
 *
 * ALTER TABLE CLUSTER ON（指定聚簇索引）
 *
 * The only thing we have to do is to change the indisclustered bits.
 *
 * 我们唯一要做的是改 indisclustered 位。
 *
 * Return the address of the new clustering index.
 *
 * 返回新聚簇索引的地址。
 */
static ObjectAddress
ATExecClusterOn(Relation rel, const char *indexName, LOCKMODE lockmode)
{
	Oid			indexOid;
	ObjectAddress address;

	indexOid = get_relname_relid(indexName, rel->rd_rel->relnamespace);

	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						indexName, RelationGetRelationName(rel))));

	/* Check index is valid to cluster on */
	/*
	 *
	 * 检查该索引能否用来聚簇
	 */
	check_index_is_clusterable(rel, indexOid, lockmode);

	/* And do the work */
	/*
	 *
	 * 然后干活
	 */
	mark_index_clustered(rel, indexOid, false);

	ObjectAddressSet(address,
					 RelationRelationId, indexOid);

	return address;
}

/*
 * ALTER TABLE SET WITHOUT CLUSTER
 *
 * ALTER TABLE SET WITHOUT CLUSTER（取消聚簇）
 *
 * We have to find any indexes on the table that have indisclustered bit
 * set and turn it off.
 *
 * 必须找出该表上所有设置了 indisclustered 位的索引，并把它关掉。
 */
static void
ATExecDropCluster(Relation rel, LOCKMODE lockmode)
{
	mark_index_clustered(rel, InvalidOid, false);
}

/*
 * Preparation phase for SET ACCESS METHOD
 *
 * SET ACCESS METHOD 的准备阶段
 *
 * Check that the access method exists and determine whether a change is
 * actually needed.
 *
 * 检查访问方法是否存在，并确定是否真的需要变更。
 */
static void
ATPrepSetAccessMethod(AlteredTableInfo *tab, Relation rel, const char *amname)
{
	Oid			amoid;

	/*
	 * Look up the access method name and check that it differs from the
	 * table's current AM.  If DEFAULT was specified for a partitioned table
	 * (amname is NULL), set it to InvalidOid to reset the catalogued AM.
	 *
	 * 查找访问方法名，并检查它是否与表当前的访问方法不同。若对分区表指定了 DEFAULT（amname 为 NULL），把它设成
	 * InvalidOid 以重置目录里的访问方法。
	 */
	if (amname != NULL)
		amoid = get_table_am_oid(amname, false);
	else if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		amoid = InvalidOid;
	else
		amoid = get_table_am_oid(default_table_access_method, false);

	/* if it's a match, phase 3 doesn't need to do anything */
	/*
	 *
	 * 若匹配，阶段 3 不必做任何事
	 */
	if (rel->rd_rel->relam == amoid)
		return;

	/* Save info for Phase 3 to do the real work */
	/*
	 *
	 * 保存信息，让 Phase 3 做真正的工作
	 */
	tab->rewrite |= AT_REWRITE_ACCESS_METHOD;
	tab->newAccessMethod = amoid;
	tab->chgAccessMethod = true;
}

/*
 * Special handling of ALTER TABLE SET ACCESS METHOD for relations with no
 * storage that have an interest in preserving AM.
 *
 * 对没有存储、但又想保留访问方法的关系，ALTER TABLE SET ACCESS METHOD 的特殊处理。
 *
 * Since these have no storage, setting the access method is a catalog only
 * operation.
 *
 * 因为它们没有存储，设置访问方法只是目录操作。
 */
static void
ATExecSetAccessMethodNoStorage(Relation rel, Oid newAccessMethodId)
{
	Relation	pg_class;
	Oid			oldAccessMethodId;
	HeapTuple	tuple;
	Form_pg_class rd_rel;
	Oid			reloid = RelationGetRelid(rel);

	/*
	 * Shouldn't be called on relations having storage; these are processed in
	 * phase 3.
	 *
	 * 不应在有存储的关系上调用；那些在阶段 3 处理。
	 */
	Assert(!RELKIND_HAS_STORAGE(rel->rd_rel->relkind));

	/* Get a modifiable copy of the relation's pg_class row. */
	/*
	 *
	 * 取出该关系 pg_class 行的可修改副本。
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", reloid);
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* Update the pg_class row. */
	/*
	 *
	 * 更新 pg_class 行。
	 */
	oldAccessMethodId = rd_rel->relam;
	rd_rel->relam = newAccessMethodId;

	/* Leave if no update required */
	/*
	 *
	 * 若不需要更新就离开
	 */
	if (rd_rel->relam == oldAccessMethodId)
	{
		heap_freetuple(tuple);
		table_close(pg_class, RowExclusiveLock);
		return;
	}

	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	/*
	 * Update the dependency on the new access method.  No dependency is added
	 * if the new access method is InvalidOid (default case).  Be very careful
	 * that this has to compare the previous value stored in pg_class with the
	 * new one.
	 *
	 * 更新对新访问方法的依赖。若新访问方法是 InvalidOid（默认情况），则不加依赖。必须非常小心地拿 pg_class
	 * 里存的先前值和新值比较。
	 */
	if (!OidIsValid(oldAccessMethodId) && OidIsValid(rd_rel->relam))
	{
		ObjectAddress relobj,
					referenced;

		/*
		 * New access method is defined and there was no dependency
		 * previously, so record a new one.
		 *
		 * 定义了新访问方法，而先前没有依赖，于是记一条新的。
		 */
		ObjectAddressSet(relobj, RelationRelationId, reloid);
		ObjectAddressSet(referenced, AccessMethodRelationId, rd_rel->relam);
		recordDependencyOn(&relobj, &referenced, DEPENDENCY_NORMAL);
	}
	else if (OidIsValid(oldAccessMethodId) &&
			 !OidIsValid(rd_rel->relam))
	{
		/*
		 * There was an access method defined, and no new one, so just remove
		 * the existing dependency.
		 *
		 * 原先定义了访问方法，现在没有新的，于是去掉现有依赖。
		 */
		deleteDependencyRecordsForClass(RelationRelationId, reloid,
										AccessMethodRelationId,
										DEPENDENCY_NORMAL);
	}
	else
	{
		Assert(OidIsValid(oldAccessMethodId) &&
			   OidIsValid(rd_rel->relam));

		/* Both are valid, so update the dependency */
		/*
		 *
		 * 两者都有效，于是更新依赖
		 */
		changeDependencyFor(RelationRelationId, reloid,
							AccessMethodRelationId,
							oldAccessMethodId, rd_rel->relam);
	}

	/* make the relam and dependency changes visible */
	/*
	 *
	 * 让 relam 和依赖的修改可见
	 */
	CommandCounterIncrement();

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	heap_freetuple(tuple);
	table_close(pg_class, RowExclusiveLock);
}

/*
 * ALTER TABLE SET TABLESPACE
 *
 * ALTER TABLE SET TABLESPACE（设置表空间）
 */
static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, const char *tablespacename, LOCKMODE lockmode)
{
	Oid			tablespaceId;

	/* Check that the tablespace exists */
	/*
	 *
	 * 检查表空间是否存在
	 */
	tablespaceId = get_tablespace_oid(tablespacename, false);

	/* Check permissions except when moving to database's default */
	/*
	 *
	 * 除非是移到数据库默认表空间，否则检查权限
	 */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE, tablespacename);
	}

	/* Save info for Phase 3 to do the real work */
	/*
	 *
	 * 保存信息，让 Phase 3 做真正的工作
	 */
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands")));

	tab->newTableSpace = tablespaceId;
}

/*
 * Set, reset, or replace reloptions.
 *
 * 设置、重置或替换 reloptions。
 */
static void
ATExecSetRelOptions(Relation rel, List *defList, AlterTableType operation,
					LOCKMODE lockmode)
{
	Oid			relid;
	Relation	pgclass;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		datum;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_class];
	bool		repl_null[Natts_pg_class];
	bool		repl_repl[Natts_pg_class];
	const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;

	if (defList == NIL && operation != AT_ReplaceRelOptions)
		return;					/* nothing to do */
							/*
							 *
							 * 无事可做
							 */

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	/* Fetch heap tuple */
	/*
	 *
	 * 取出堆元组
	 */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCacheLocked1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	if (operation == AT_ReplaceRelOptions)
	{
		/*
		 * If we're supposed to replace the reloptions list, we just pretend
		 * there were none before.
		 *
		 * 若应当替换 reloptions 列表，就假装之前一个都没有。
		 */
		datum = (Datum) 0;
	}
	else
	{
		bool		isnull;

		/* Get the old reloptions */
		/*
		 *
		 * 取得旧的 reloptions
		 */
		datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								&isnull);
		if (isnull)
			datum = (Datum) 0;
	}

	/* Generate new proposed reloptions (text array) */
	/*
	 *
	 * 生成新提议的 reloptions（文本数组）
	 */
	newOptions = transformRelOptions(datum, defList, NULL, validnsps, false,
									 operation == AT_ResetRelOptions);

	/* Validate */
	/*
	 *
	 * 校验
	 */
	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
			(void) heap_reloptions(rel->rd_rel->relkind, newOptions, true);
			break;
		case RELKIND_PARTITIONED_TABLE:
			(void) partitioned_table_reloptions(newOptions, true);
			break;
		case RELKIND_VIEW:
			(void) view_reloptions(newOptions, true);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			(void) index_reloptions(rel->rd_indam->amoptions, newOptions, true);
			break;
		case RELKIND_TOASTVALUE:
			/* fall through to error -- shouldn't ever get here */
			/*
			 *
			 * 落到错误处理，本不该走到这里
			 */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot set options for relation \"%s\"",
							RelationGetRelationName(rel)),
					 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
			break;
	}

	/* Special-case validation of view options */
	/*
	 *
	 * 视图选项的特殊校验
	 */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		Query	   *view_query = get_view_query(rel);
		List	   *view_options = untransformRelOptions(newOptions);
		ListCell   *cell;
		bool		check_option = false;

		foreach(cell, view_options)
		{
			DefElem    *defel = (DefElem *) lfirst(cell);

			if (strcmp(defel->defname, "check_option") == 0)
				check_option = true;
		}

		/*
		 * If the check option is specified, look to see if the view is
		 * actually auto-updatable or not.
		 *
		 * 若指定了 check option，看看这个视图实际上是否可自动更新。
		 */
		if (check_option)
		{
			const char *view_updatable_error =
				view_query_is_auto_updatable(view_query, true);

			if (view_updatable_error)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
						 errhint("%s", _(view_updatable_error))));
		}
	}

	/*
	 * All we need do here is update the pg_class row; the new options will be
	 * propagated into relcaches during post-commit cache inval.
	 *
	 * 这里只要更新 pg_class 行；新选项会在提交后的缓存失效中传播进 relcache。
	 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newOptions;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;

	repl_repl[Anum_pg_class_reloptions - 1] = true;

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
								 repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);
	UnlockTuple(pgclass, &tuple->t_self, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	heap_freetuple(newtuple);

	ReleaseSysCache(tuple);

	/* repeat the whole exercise for the toast table, if there's one */
	/*
	 *
	 * 若有 TOAST 表，对它再做一遍同样的事
	 */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;
		Oid			toastid = rel->rd_rel->reltoastrelid;

		toastrel = table_open(toastid, lockmode);

		/* Fetch heap tuple */
		/*
		 *
		 * 取出堆元组
		 */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);

		if (operation == AT_ReplaceRelOptions)
		{
			/*
			 * If we're supposed to replace the reloptions list, we just
			 * pretend there were none before.
			 *
			 * 若应当替换 reloptions 列表，就假装之前一个都没有。
			 */
			datum = (Datum) 0;
		}
		else
		{
			bool		isnull;

			/* Get the old reloptions */
			/*
			 *
			 * 取得旧的 reloptions
			 */
			datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									&isnull);
			if (isnull)
				datum = (Datum) 0;
		}

		newOptions = transformRelOptions(datum, defList, "toast", validnsps,
										 false, operation == AT_ResetRelOptions);

		(void) heap_reloptions(RELKIND_TOASTVALUE, newOptions, true);

		memset(repl_val, 0, sizeof(repl_val));
		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		if (newOptions != (Datum) 0)
			repl_val[Anum_pg_class_reloptions - 1] = newOptions;
		else
			repl_null[Anum_pg_class_reloptions - 1] = true;

		repl_repl[Anum_pg_class_reloptions - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(pgclass),
									 repl_val, repl_null, repl_repl);

		CatalogTupleUpdate(pgclass, &newtuple->t_self, newtuple);

		InvokeObjectPostAlterHookArg(RelationRelationId,
									 RelationGetRelid(toastrel), 0,
									 InvalidOid, true);

		heap_freetuple(newtuple);

		ReleaseSysCache(tuple);

		table_close(toastrel, NoLock);
	}

	table_close(pgclass, RowExclusiveLock);
}

/*
 * Execute ALTER TABLE SET TABLESPACE for cases where there is no tuple
 * rewriting to be done, so we just want to copy the data as fast as possible.
 *
 * 对不必重写元组的情况执行 ALTER TABLE SET TABLESPACE，只想尽快拷贝数据。
 */
static void
ATExecSetTableSpace(Oid tableOid, Oid newTableSpace, LOCKMODE lockmode)
{
	Relation	rel;
	Oid			reltoastrelid;
	RelFileNumber newrelfilenumber;
	RelFileLocator newrlocator;
	List	   *reltoastidxids = NIL;
	ListCell   *lc;

	/*
	 * Need lock here in case we are recursing to toast table or index
	 *
	 * 若正在递归到 TOAST 表或索引，这里需要锁
	 */
	rel = relation_open(tableOid, lockmode);

	/* Check first if relation can be moved to new tablespace */
	/*
	 *
	 * 先检查关系能否移到新表空间
	 */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel), 0);
		relation_close(rel, NoLock);
		return;
	}

	reltoastrelid = rel->rd_rel->reltoastrelid;
	/* Fetch the list of indexes on toast relation if necessary */
	/*
	 *
	 * 必要时取出 TOAST 关系上的索引列表
	 */
	if (OidIsValid(reltoastrelid))
	{
		Relation	toastRel = relation_open(reltoastrelid, lockmode);

		reltoastidxids = RelationGetIndexList(toastRel);
		relation_close(toastRel, lockmode);
	}

	/*
	 * Relfilenumbers are not unique in databases across tablespaces, so we
	 * need to allocate a new one in the new tablespace.
	 *
	 * relfilenumber 在跨表空间的数据库里并不唯一，所以要在新表空间里分配一个新的。
	 */
	newrelfilenumber = GetNewRelFileNumber(newTableSpace, NULL,
										   rel->rd_rel->relpersistence);

	/* Open old and new relation */
	/*
	 *
	 * 打开新旧关系
	 */
	newrlocator = rel->rd_locator;
	newrlocator.relNumber = newrelfilenumber;
	newrlocator.spcOid = newTableSpace;

	/* hand off to AM to actually create new rel storage and copy the data */
	/*
	 *
	 * 交给访问方法去真正创建新的关系存储并拷贝数据
	 */
	if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		index_copy_data(rel, newrlocator);
	}
	else
	{
		Assert(RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind));
		table_relation_copy_data(rel, &newrlocator);
	}

	/*
	 * Update the pg_class row.
	 *
	 * 更新 pg_class 行。
	 *
	 * NB: This wouldn't work if ATExecSetTableSpace() were allowed to be
	 * executed on pg_class or its indexes (the above copy wouldn't contain
	 * the updated pg_class entry), but that's forbidden with
	 * CheckRelationTableSpaceMove().
	 *
	 * 注意：若允许在 pg_class 或其索引上执行 ATExecSetTableSpace()，这行不通（上面的拷贝不会包含更新后的
	 * pg_class 项），但 CheckRelationTableSpaceMove() 禁止这样做。
	 */
	SetRelationTableSpace(rel, newTableSpace, newrelfilenumber);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	RelationAssumeNewRelfilelocator(rel);

	relation_close(rel, NoLock);

	/* Make sure the reltablespace change is visible */
	/*
	 *
	 * 确保 reltablespace 的变更可见
	 */
	CommandCounterIncrement();

	/* Move associated toast relation and/or indexes, too */
	/*
	 *
	 * 关联的 TOAST 关系和/或索引也一起移动
	 */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
	foreach(lc, reltoastidxids)
		ATExecSetTableSpace(lfirst_oid(lc), newTableSpace, lockmode);

	/* Clean up */
	/*
	 *
	 * 清理
	 */
	list_free(reltoastidxids);
}

/*
 * Special handling of ALTER TABLE SET TABLESPACE for relations with no
 * storage that have an interest in preserving tablespace.
 *
 * 对没有存储、但又想保留表空间的关系，ALTER TABLE SET TABLESPACE 的特殊处理。
 *
 * Since these have no storage the tablespace can be updated with a simple
 * metadata only operation to update the tablespace.
 *
 * 因为它们没有存储，更新表空间可以只做元数据操作。
 */
static void
ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace)
{
	/*
	 * Shouldn't be called on relations having storage; these are processed in
	 * phase 3.
	 *
	 * 不应在有存储的关系上调用；那些在阶段 3 处理。
	 */
	Assert(!RELKIND_HAS_STORAGE(rel->rd_rel->relkind));

	/* check if relation can be moved to its new tablespace */
	/*
	 *
	 * 检查关系能否移到新表空间
	 */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  0);
		return;
	}

	/* Update can be done, so change reltablespace */
	/*
	 *
	 * 可以更新，于是修改 reltablespace
	 */
	SetRelationTableSpace(rel, newTableSpace, InvalidOid);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	/* Make sure the reltablespace change is visible */
	/*
	 *
	 * 确保 reltablespace 的变更可见
	 */
	CommandCounterIncrement();
}

/*
 * Alter Table ALL ... SET TABLESPACE
 *
 * ALTER TABLE ALL ... SET TABLESPACE（把某表空间中的对象全部移走）
 *
 * Allows a user to move all objects of some type in a given tablespace in the
 * current database to another tablespace.  Objects can be chosen based on the
 * owner of the object also, to allow users to move only their objects.
 * The user must have CREATE rights on the new tablespace, as usual.   The main
 * permissions handling is done by the lower-level table move function.
 *
 * 允许用户把当前数据库里、给定表空间中某类对象全部移到另一个表空间。也可以按对象属主筛选，让用户只移动自己的对象。
 * 用户必须对新表空间有 CREATE 权限，和平常一样。主要的权限处理由更低层的表移动函数完成。
 *
 * All to-be-moved objects are locked first. If NOWAIT is specified and the
 * lock can't be acquired then we ereport(ERROR).
 *
 * 先锁住所有将要移动的对象。若指定了 NOWAIT 且拿不到锁，就 ereport(ERROR)。
 */
Oid
AlterTableMoveAll(AlterTableMoveAllStmt *stmt)
{
	List	   *relations = NIL;
	ListCell   *l;
	ScanKeyData key[1];
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;
	Oid			orig_tablespaceoid;
	Oid			new_tablespaceoid;
	List	   *role_oids = roleSpecsToIds(stmt->roles);

	/* Ensure we were not asked to move something we can't */
	/*
	 *
	 * 确保没有要求我们移动不能移动的东西
	 */
	if (stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_INDEX &&
		stmt->objtype != OBJECT_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only tables, indexes, and materialized views exist in tablespaces")));

	/* Get the orig and new tablespace OIDs */
	/*
	 *
	 * 取得原来的和新的表空间 OID
	 */
	orig_tablespaceoid = get_tablespace_oid(stmt->orig_tablespacename, false);
	new_tablespaceoid = get_tablespace_oid(stmt->new_tablespacename, false);

	/* Can't move shared relations in to or out of pg_global */
	/*
	 *
	 * 不能把共享关系移进或移出 pg_global
	 */
	/* This is also checked by ATExecSetTableSpace, but nice to stop earlier */
	/*
	 *
	 * ATExecSetTableSpace 也会检查这一点，但早点停下来更好
	 */
	if (orig_tablespaceoid == GLOBALTABLESPACE_OID ||
		new_tablespaceoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move relations in to or out of pg_global tablespace")));

	/*
	 * Must have CREATE rights on the new tablespace, unless it is the
	 * database default tablespace (which all users implicitly have CREATE
	 * rights on).
	 *
	 * 必须对新表空间有 CREATE 权限，除非它是数据库默认表空间（所有用户隐式对它有 CREATE 权限）。
	 */
	if (OidIsValid(new_tablespaceoid) && new_tablespaceoid != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, new_tablespaceoid, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(new_tablespaceoid));
	}

	/*
	 * Now that the checks are done, check if we should set either to
	 * InvalidOid because it is our database's default tablespace.
	 *
	 * 检查做完后，看看是否应把其中某一个设成 InvalidOid，因为它是本数据库的默认表空间。
	 */
	if (orig_tablespaceoid == MyDatabaseTableSpace)
		orig_tablespaceoid = InvalidOid;

	if (new_tablespaceoid == MyDatabaseTableSpace)
		new_tablespaceoid = InvalidOid;

	/* no-op */
	/*
	 *
	 * 空操作
	 */
	if (orig_tablespaceoid == new_tablespaceoid)
		return new_tablespaceoid;

	/*
	 * Walk the list of objects in the tablespace and move them. This will
	 * only find objects in our database, of course.
	 *
	 * 遍历该表空间里的对象列表并移动它们。当然，这只会找到我们这个数据库里的对象。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_class_reltablespace,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(orig_tablespaceoid));

	rel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 1, key);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class relForm = (Form_pg_class) GETSTRUCT(tuple);
		Oid			relOid = relForm->oid;

		/*
		 * Do not move objects in pg_catalog as part of this, if an admin
		 * really wishes to do so, they can issue the individual ALTER
		 * commands directly.
		 *
		 * 不要把 pg_catalog 里的对象作为这次操作的一部分移动；若管理员真想这么做，可以直接发单独的 ALTER 命令。
		 *
		 * Also, explicitly avoid any shared tables, temp tables, or TOAST
		 * (TOAST will be moved with the main table).
		 *
		 * 同时明确避开任何共享表、临时表或 TOAST（TOAST 会随主表一起移动）。
		 */
		if (IsCatalogNamespace(relForm->relnamespace) ||
			relForm->relisshared ||
			isAnyTempNamespace(relForm->relnamespace) ||
			IsToastNamespace(relForm->relnamespace))
			continue;

		/* Only move the object type requested */
		/*
		 *
		 * 只移动所请求的对象类型
		 */
		if ((stmt->objtype == OBJECT_TABLE &&
			 relForm->relkind != RELKIND_RELATION &&
			 relForm->relkind != RELKIND_PARTITIONED_TABLE) ||
			(stmt->objtype == OBJECT_INDEX &&
			 relForm->relkind != RELKIND_INDEX &&
			 relForm->relkind != RELKIND_PARTITIONED_INDEX) ||
			(stmt->objtype == OBJECT_MATVIEW &&
			 relForm->relkind != RELKIND_MATVIEW))
			continue;

		/* Check if we are only moving objects owned by certain roles */
		/*
		 *
		 * 检查是否只移动某些角色拥有的对象
		 */
		if (role_oids != NIL && !list_member_oid(role_oids, relForm->relowner))
			continue;

		/*
		 * Handle permissions-checking here since we are locking the tables
		 * and also to avoid doing a bunch of work only to fail part-way. Note
		 * that permissions will also be checked by AlterTableInternal().
		 *
		 * 在这里做权限检查，因为我们正在锁表，也免得做了一堆工作却中途失败。注意 AlterTableInternal() 也会检查权限。
		 *
		 * Caller must be considered an owner on the table to move it.
		 *
		 * 调用方必须被视为该表的属主才能移动它。
		 */
		if (!object_ownercheck(RelationRelationId, relOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relOid)),
						   NameStr(relForm->relname));

		if (stmt->nowait &&
			!ConditionalLockRelationOid(relOid, AccessExclusiveLock))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("aborting because lock on relation \"%s.%s\" is not available",
							get_namespace_name(relForm->relnamespace),
							NameStr(relForm->relname))));
		else
			LockRelationOid(relOid, AccessExclusiveLock);

		/* Add to our list of objects to move */
		/*
		 *
		 * 加入我们要移动的对象列表
		 */
		relations = lappend_oid(relations, relOid);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	if (relations == NIL)
		ereport(NOTICE,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("no matching relations in tablespace \"%s\" found",
						orig_tablespaceoid == InvalidOid ? "(database default)" :
						get_tablespace_name(orig_tablespaceoid))));

	/* Everything is locked, loop through and move all of the relations. */
	/*
	 *
	 * 全部锁好了，循环移动所有关系。
	 */
	foreach(l, relations)
	{
		List	   *cmds = NIL;
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_SetTableSpace;
		cmd->name = stmt->new_tablespacename;

		cmds = lappend(cmds, cmd);

		EventTriggerAlterTableStart((Node *) stmt);
		/* OID is set by AlterTableInternal */
		/*
		 *
		 * OID 由 AlterTableInternal 设置
		 */
		AlterTableInternal(lfirst_oid(l), cmds, false);
		EventTriggerAlterTableEnd();
	}

	return new_tablespaceoid;
}

/*
 * 把索引各 fork 拷到新的 relfilenode。拷贝前先刷出共享缓冲区里的页，并安排删除旧文件。
 */
static void
index_copy_data(Relation rel, RelFileLocator newrlocator)
{
	SMgrRelation dstrel;

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers.  We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 *
	 * 因为我们直接拷文件、不看共享缓冲区，最好先把源关系在共享缓冲区里的页都刷出去。假定持有该关系的排他锁期间不会再有新的修改。
	 */
	FlushRelationBuffers(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * 创建并拷贝该关系的所有 fork，并安排解除旧物理文件的链接。
	 *
	 * NOTE: any conflict in relfilenumber value will be caught in
	 * RelationCreateStorage().
	 *
	 * 注意：relfilenumber 值的任何冲突都会在 RelationCreateStorage() 里被抓住。
	 */
	dstrel = RelationCreateStorage(newrlocator, rel->rd_rel->relpersistence, true);

	/* copy main fork */
	/*
	 *
	 * 拷贝主 fork
	 */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* copy those extra forks that exist */
	/*
	 *
	 * 拷贝那些存在的额外 fork
	 */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 *
			 * 若关系是永久的，或者这是 unlogged 关系的 init fork，就写 WAL 记录这次创建。
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(&newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	/* drop old relation, and close new one */
	/*
	 *
	 * 丢掉旧关系，并关闭新的
	 */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/*
 * ALTER TABLE ENABLE/DISABLE TRIGGER
 *
 * ALTER TABLE ENABLE/DISABLE TRIGGER（启用或禁用触发器）
 *
 * We just pass this off to trigger.c.
 *
 * 我们只是把它交给 trigger.c。
 */
static void
ATExecEnableDisableTrigger(Relation rel, const char *trigname,
						   char fires_when, bool skip_system, bool recurse,
						   LOCKMODE lockmode)
{
	EnableDisableTrigger(rel, trigname, InvalidOid,
						 fires_when, skip_system, recurse,
						 lockmode);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), 0);
}

/*
 * ALTER TABLE ENABLE/DISABLE RULE
 *
 * ALTER TABLE ENABLE/DISABLE RULE（启用或禁用规则）
 *
 * We just pass this off to rewriteDefine.c.
 *
 * 我们只是把它交给 rewriteDefine.c。
 */
static void
ATExecEnableDisableRule(Relation rel, const char *rulename,
						char fires_when, LOCKMODE lockmode)
{
	EnableDisableRule(rel, rulename, fires_when);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), 0);
}

/*
 * ALTER TABLE INHERIT
 *
 * ALTER TABLE INHERIT（加入继承）
 *
 * Add a parent to the child's parents. This verifies that all the columns and
 * check constraints of the parent appear in the child and that they have the
 * same data types and expressions.
 *
 * 给子表的父表列表加一个父表。这会验证父表的所有列和检查约束都出现在子表里，并且数据类型和表达式相同。
 */
static void
ATPrepAddInherit(Relation child_rel)
{
	if (child_rel->rd_rel->reloftype)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of typed table")));

	if (child_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of a partition")));

	if (child_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of partitioned table")));
}

/*
 * Return the address of the new parent relation.
 *
 * 返回新父关系的地址。
 */
static ObjectAddress
ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode)
{
	Relation	parent_rel;
	List	   *children;
	ObjectAddress address;
	const char *trigger_name;

	/*
	 * A self-exclusive lock is needed here.  See the similar case in
	 * MergeAttributes() for a full explanation.
	 *
	 * 这里需要自排他锁。完整解释见 MergeAttributes() 里的类似情形。
	 */
	parent_rel = table_openrv(parent, ShareUpdateExclusiveLock);

	/*
	 * Must be owner of both parent and child -- child was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 *
	 * 必须同时是父表和子表的属主。子表已由 ATPrepCmd 里的 ATSimplePermissions 调用检查过
	 */
	ATSimplePermissions(AT_AddInherit, parent_rel,
						ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* Permanent rels cannot inherit from temporary ones */
	/*
	 *
	 * 永久关系不能继承临时关系
	 */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		child_rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel))));

	/* If parent rel is temp, it must belong to this session */
	/*
	 *
	 * 若父关系是临时的，它必须属于本会话
	 */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!parent_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation of another session")));

	/* Ditto for the child */
	/*
	 *
	 * 子表同样如此
	 */
	if (child_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!child_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit to temporary relation of another session")));

	/* Prevent partitioned tables from becoming inheritance parents */
	/*
	 *
	 * 阻止分区表变成继承父表
	 */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from partitioned table \"%s\"",
						parent->relname)));

	/* Likewise for partitions */
	/*
	 *
	 * 分区也同样
	 */
	if (parent_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from a partition")));

	/*
	 * Prevent circularity by seeing if proposed parent inherits from child.
	 * (In particular, this disallows making a rel inherit from itself.)
	 *
	 * 看看拟议的父表是否继承自子表，以防止成环。（尤其不允许关系继承自己。）
	 *
	 * This is not completely bulletproof because of race conditions: in
	 * multi-level inheritance trees, someone else could concurrently be
	 * making another inheritance link that closes the loop but does not join
	 * either of the rels we have locked.  Preventing that seems to require
	 * exclusive locks on the entire inheritance tree, which is a cure worse
	 * than the disease.  find_all_inheritors() will cope with circularity
	 * anyway, so don't sweat it too much.
	 *
	 * 由于竞态，这并不完全无懈可击：在多层继承树里，别人可能同时在建立另一条继承链接，把环闭合起来，但并不连接我们锁住的这两个关系。
	 * 要防止这一点似乎需要对整个继承树加排他锁，这比毛病本身更糟。find_all_inheritors() 反正能应付成环，
	 * 所以不必太紧张。
	 *
	 * We use weakest lock we can on child's children, namely AccessShareLock.
	 *
	 * 对子表的子表，我们用能用的最弱锁，即 AccessShareLock。
	 */
	children = find_all_inheritors(RelationGetRelid(child_rel),
								   AccessShareLock, NULL);

	if (list_member_oid(children, RelationGetRelid(parent_rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   parent->relname,
						   RelationGetRelationName(child_rel))));

	/*
	 * If child_rel has row-level triggers with transition tables, we
	 * currently don't allow it to become an inheritance child.  See also
	 * prohibitions in ATExecAttachPartition() and CreateTrigger().
	 *
	 * 若 child_rel 有带转换表的行级触发器，目前不允许它成为继承子表。另见 ATExecAttachPartition() 和
	 * CreateTrigger() 里的禁止。
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(child_rel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming an inheritance child",
						trigger_name, RelationGetRelationName(child_rel)),
				 errdetail("ROW triggers with transition tables are not supported in inheritance hierarchies.")));

	/* OK to create inheritance */
	/*
	 *
	 * 可以建立继承了
	 */
	CreateInheritance(child_rel, parent_rel, false);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* keep our lock on the parent relation until commit */
	/*
	 *
	 * 把对父关系的锁保持到提交
	 */
	table_close(parent_rel, NoLock);

	return address;
}

/*
 * CreateInheritance
 *		Catalog manipulation portion of creating inheritance between a child
 *		table and a parent table.
 *
 * CreateInheritance：在子表和父表之间建立继承时的目录操作部分。
 *
 * Common to ATExecAddInherit() and ATExecAttachPartition().
 *
 * ATExecAddInherit() 和 ATExecAttachPartition() 共用。
 */
static void
CreateInheritance(Relation child_rel, Relation parent_rel, bool ispartition)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	int32		inhseqno;

	/* Note: get RowExclusiveLock because we will write pg_inherits below. */
	/*
	 *
	 * 注意：取 RowExclusiveLock，因为下面要写 pg_inherits。
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates in the list of parents, and determine the highest
	 * inhseqno already present; we'll use the next one for the new parent.
	 * Also, if proposed child is a partition, it cannot already be
	 * inheriting.
	 *
	 * 检查父表列表有没有重复，并确定已有的最大 inhseqno；新父表用下一个。另外，若拟议的子表是分区，它不能已经在继承。
	 *
	 * Note: we do not reject the case where the child already inherits from
	 * the parent indirectly; CREATE TABLE doesn't reject comparable cases.
	 *
	 * 注意：我们不拒绝子表已经间接继承自该父表的情况；CREATE TABLE 也不拒绝类似情况。
	 */
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	/* inhseqno sequences start at 1 */
	/*
	 *
	 * inhseqno 序列从 1 开始
	 */
	inhseqno = 0;
	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inh = (Form_pg_inherits) GETSTRUCT(inheritsTuple);

		if (inh->inhparent == RelationGetRelid(parent_rel))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" would be inherited from more than once",
							RelationGetRelationName(parent_rel))));

		if (inh->inhseqno > inhseqno)
			inhseqno = inh->inhseqno;
	}
	systable_endscan(scan);

	/* Match up the columns and bump attinhcount as needed */
	/*
	 *
	 * 把列对上，并按需要增加 attinhcount
	 */
	MergeAttributesIntoExisting(child_rel, parent_rel, ispartition);

	/* Match up the constraints and bump coninhcount as needed */
	/*
	 *
	 * 把约束对上，并按需要增加 coninhcount
	 */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * OK, it looks valid.  Make the catalog entries that show inheritance.
	 *
	 * 看起来合法。建立表示继承的目录项。
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation,
							 parent_rel->rd_rel->relkind ==
							 RELKIND_PARTITIONED_TABLE);

	/* Now we're done with pg_inherits */
	/*
	 *
	 * 现在 pg_inherits 处理完了
	 */
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 * Obtain the source-text form of the constraint expression for a check
 * constraint, given its pg_constraint tuple
 *
 * 根据 CHECK 约束的 pg_constraint 元组，取得该约束表达式的源码形式
 */
static char *
decompile_conbin(HeapTuple contup, TupleDesc tupdesc)
{
	Form_pg_constraint con;
	bool		isnull;
	Datum		attr;
	Datum		expr;

	con = (Form_pg_constraint) GETSTRUCT(contup);
	attr = heap_getattr(contup, Anum_pg_constraint_conbin, tupdesc, &isnull);
	if (isnull)
		elog(ERROR, "null conbin for constraint %u", con->oid);

	expr = DirectFunctionCall2(pg_get_expr, attr,
							   ObjectIdGetDatum(con->conrelid));
	return TextDatumGetCString(expr);
}

/*
 * Determine whether two check constraints are functionally equivalent
 *
 * 判断两个 CHECK 约束在功能上是否等价
 *
 * The test we apply is to see whether they reverse-compile to the same
 * source string.  This insulates us from issues like whether attributes
 * have the same physical column numbers in parent and child relations.
 *
 * 我们用的测试是看它们反编译后的源字符串是否相同。这样就不会被父表和子表里属性的物理列号是否相同这类问题干扰。
 *
 * Note that we ignore enforceability as there are cases where constraints
 * with differing enforceability are allowed.
 *
 * 注意我们忽略可强制性，因为有些情况下允许可强制性不同的约束。
 */
static bool
constraints_equivalent(HeapTuple a, HeapTuple b, TupleDesc tupleDesc)
{
	Form_pg_constraint acon = (Form_pg_constraint) GETSTRUCT(a);
	Form_pg_constraint bcon = (Form_pg_constraint) GETSTRUCT(b);

	if (acon->condeferrable != bcon->condeferrable ||
		acon->condeferred != bcon->condeferred ||
		strcmp(decompile_conbin(a, tupleDesc),
			   decompile_conbin(b, tupleDesc)) != 0)
		return false;
	else
		return true;
}

/*
 * Check columns in child table match up with columns in parent, and increment
 * their attinhcount.
 *
 * 检查子表的列是否与父表的列对得上，并增加它们的 attinhcount。
 *
 * Called by CreateInheritance
 *
 * 由 CreateInheritance 调用
 *
 * Currently all parent columns must be found in child. Missing columns are an
 * error.  One day we might consider creating new columns like CREATE TABLE
 * does.  However, that is widely unpopular --- in the common use case of
 * partitioned tables it's a foot-gun.
 *
 * 目前父表的所有列都必须在子表里找到。缺列是错误。将来也许可以考虑像 CREATE TABLE 那样创建新列。不过这非常不受欢迎：
 * 在分区表这个常见用法里，那是给自己挖坑。
 *
 * The data type must match exactly. If the parent column is NOT NULL then
 * the child must be as well. Defaults are not compared, however.
 *
 * 数据类型必须完全匹配。若父列是 NOT NULL，子列也必须是。不过不比较默认值。
 */
static void
MergeAttributesIntoExisting(Relation child_rel, Relation parent_rel, bool ispartition)
{
	Relation	attrrel;
	TupleDesc	parent_desc;

	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	parent_desc = RelationGetDescr(parent_rel);

	for (AttrNumber parent_attno = 1; parent_attno <= parent_desc->natts; parent_attno++)
	{
		Form_pg_attribute parent_att = TupleDescAttr(parent_desc, parent_attno - 1);
		char	   *parent_attname = NameStr(parent_att->attname);
		HeapTuple	tuple;

		/* Ignore dropped columns in the parent. */
		/*
		 *
		 * 忽略父表中已删除的列。
		 */
		if (parent_att->attisdropped)
			continue;

		/* Find same column in child (matching on column name). */
		/*
		 *
		 * 在子表里找同名列。
		 */
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel), parent_attname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute child_att = (Form_pg_attribute) GETSTRUCT(tuple);

			if (parent_att->atttypid != child_att->atttypid ||
				parent_att->atttypmod != child_att->atttypmod)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different type for column \"%s\"",
								RelationGetRelationName(child_rel), parent_attname)));

			if (parent_att->attcollation != child_att->attcollation)
				ereport(ERROR,
						(errcode(ERRCODE_COLLATION_MISMATCH),
						 errmsg("child table \"%s\" has different collation for column \"%s\"",
								RelationGetRelationName(child_rel), parent_attname)));

			/*
			 * If the parent has a not-null constraint that's not NO INHERIT,
			 * make sure the child has one too.
			 *
			 * 若父表有不是 NO INHERIT 的 NOT NULL 约束，确保子表也有。
			 *
			 * Other constraints are checked elsewhere.
			 *
			 * 其他约束在别处检查。
			 */
			if (parent_att->attnotnull && !child_att->attnotnull)
			{
				HeapTuple	contup;

				contup = findNotNullConstraintAttnum(RelationGetRelid(parent_rel),
													 parent_att->attnum);
				if (HeapTupleIsValid(contup) &&
					!((Form_pg_constraint) GETSTRUCT(contup))->connoinherit)
					ereport(ERROR,
							errcode(ERRCODE_DATATYPE_MISMATCH),
							errmsg("column \"%s\" in child table \"%s\" must be marked NOT NULL",
								   parent_attname, RelationGetRelationName(child_rel)));
			}

			/*
			 * Child column must be generated if and only if parent column is.
			 *
			 * 子列是生成列，当且仅当父列是生成列。
			 */
			if (parent_att->attgenerated && !child_att->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must be a generated column", parent_attname)));
			if (child_att->attgenerated && !parent_att->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must not be a generated column", parent_attname)));

			if (parent_att->attgenerated && child_att->attgenerated && child_att->attgenerated != parent_att->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" inherits from generated column of different kind", parent_attname),
						 errdetail("Parent column is %s, child column is %s.",
								   parent_att->attgenerated == ATTRIBUTE_GENERATED_STORED ? "STORED" : "VIRTUAL",
								   child_att->attgenerated == ATTRIBUTE_GENERATED_STORED ? "STORED" : "VIRTUAL")));

			/*
			 * Regular inheritance children are independent enough not to
			 * inherit identity columns.  But partitions are integral part of
			 * a partitioned table and inherit identity column.
			 *
			 * 普通继承的子表足够独立，不继承标识列。但分区是分区表不可分割的一部分，会继承标识列。
			 */
			if (ispartition)
				child_att->attidentity = parent_att->attidentity;

			/*
			 * OK, bump the child column's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 *
			 * 好，增加子列的继承计数。（若后面失败，这次修改会回滚。）
			 */
			if (pg_add_s16_overflow(child_att->attinhcount, 1,
									&child_att->attinhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * In case of partitions, we must enforce that value of attislocal
			 * is same in all partitions. (Note: there are only inherited
			 * attributes in partitions)
			 *
			 * 对分区，必须强制所有分区的 attislocal 值相同。（注意：分区里只有继承来的属性）
			 */
			if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			{
				Assert(child_att->attinhcount == 1);
				child_att->attislocal = false;
			}

			CatalogTupleUpdate(attrrel, &tuple->t_self, tuple);
			heap_freetuple(tuple);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing column \"%s\"", parent_attname)));
		}
	}

	table_close(attrrel, RowExclusiveLock);
}

/*
 * Check constraints in child table match up with constraints in parent,
 * and increment their coninhcount.
 *
 * 检查子表的约束是否与父表的约束对得上，并增加它们的 coninhcount。
 *
 * Constraints that are marked ONLY in the parent are ignored.
 *
 * 父表里标成 ONLY 的约束被忽略。
 *
 * Called by CreateInheritance
 *
 * 由 CreateInheritance 调用
 *
 * Currently all constraints in parent must be present in the child. One day we
 * may consider adding new constraints like CREATE TABLE does.
 *
 * 目前父表的所有约束都必须出现在子表里。将来也许会考虑像 CREATE TABLE 那样添加新约束。
 *
 * XXX This is O(N^2) which may be an issue with tables with hundreds of
 * constraints. As long as tables have more like 10 constraints it shouldn't be
 * a problem though. Even 100 constraints ought not be the end of the world.
 *
 * XXX 这是 O(N^2)，对有几百个约束的表可能成问题。只要表更像是有 10 个约束，就应该没问题。即使 100
 * 个约束也不该是世界末日。
 *
 * XXX See MergeWithExistingConstraint too if you change this code.
 *
 * XXX 若改这段代码，也看看 MergeWithExistingConstraint。
 */
static void
MergeConstraintsIntoExisting(Relation child_rel, Relation parent_rel)
{
	Relation	constraintrel;
	SysScanDesc parent_scan;
	ScanKeyData parent_key;
	HeapTuple	parent_tuple;
	Oid			parent_relid = RelationGetRelid(parent_rel);
	AttrMap    *attmap;

	constraintrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/* Outer loop scans through the parent's constraint definitions */
	/*
	 *
	 * 外层循环扫描父表的约束定义
	 */
	ScanKeyInit(&parent_key,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_relid));
	parent_scan = systable_beginscan(constraintrel, ConstraintRelidTypidNameIndexId,
									 true, NULL, 1, &parent_key);

	attmap = build_attrmap_by_name(RelationGetDescr(parent_rel),
								   RelationGetDescr(child_rel),
								   true);

	while (HeapTupleIsValid(parent_tuple = systable_getnext(parent_scan)))
	{
		Form_pg_constraint parent_con = (Form_pg_constraint) GETSTRUCT(parent_tuple);
		SysScanDesc child_scan;
		ScanKeyData child_key;
		HeapTuple	child_tuple;
		AttrNumber	parent_attno;
		bool		found = false;

		if (parent_con->contype != CONSTRAINT_CHECK &&
			parent_con->contype != CONSTRAINT_NOTNULL)
			continue;

		/* if the parent's constraint is marked NO INHERIT, it's not inherited */
		/*
		 *
		 * 若父约束标成 NO INHERIT，它不会被继承
		 */
		if (parent_con->connoinherit)
			continue;

		if (parent_con->contype == CONSTRAINT_NOTNULL)
			parent_attno = extractNotNullColumn(parent_tuple);
		else
			parent_attno = InvalidAttrNumber;

		/* Search for a child constraint matching this one */
		/*
		 *
		 * 搜索与这个匹配的子约束
		 */
		ScanKeyInit(&child_key,
					Anum_pg_constraint_conrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(RelationGetRelid(child_rel)));
		child_scan = systable_beginscan(constraintrel, ConstraintRelidTypidNameIndexId,
										true, NULL, 1, &child_key);

		while (HeapTupleIsValid(child_tuple = systable_getnext(child_scan)))
		{
			Form_pg_constraint child_con = (Form_pg_constraint) GETSTRUCT(child_tuple);
			HeapTuple	child_copy;

			if (child_con->contype != parent_con->contype)
				continue;

			/*
			 * CHECK constraint are matched by constraint name, NOT NULL ones
			 * by attribute number.
			 *
			 * CHECK 约束按约束名匹配，NOT NULL 按属性号匹配。
			 */
			if (child_con->contype == CONSTRAINT_CHECK)
			{
				if (strcmp(NameStr(parent_con->conname),
						   NameStr(child_con->conname)) != 0)
					continue;
			}
			else if (child_con->contype == CONSTRAINT_NOTNULL)
			{
				Form_pg_attribute parent_attr;
				Form_pg_attribute child_attr;
				AttrNumber	child_attno;

				parent_attr = TupleDescAttr(parent_rel->rd_att, parent_attno - 1);
				child_attno = extractNotNullColumn(child_tuple);
				if (parent_attno != attmap->attnums[child_attno - 1])
					continue;

				child_attr = TupleDescAttr(child_rel->rd_att, child_attno - 1);
				/* there shouldn't be constraints on dropped columns */
				/*
				 *
				 * 已删除的列上不该有约束
				 */
				if (parent_attr->attisdropped || child_attr->attisdropped)
					elog(ERROR, "found not-null constraint on dropped columns");
			}

			if (child_con->contype == CONSTRAINT_CHECK &&
				!constraints_equivalent(parent_tuple, child_tuple, RelationGetDescr(constraintrel)))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("child table \"%s\" has different definition for check constraint \"%s\"",
								RelationGetRelationName(child_rel), NameStr(parent_con->conname))));

			/*
			 * If the child constraint is "no inherit" then cannot merge
			 *
			 * 若子约束是 no inherit，则不能合并
			 */
			if (child_con->connoinherit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with non-inherited constraint on child table \"%s\"",
								NameStr(child_con->conname), RelationGetRelationName(child_rel))));

			/*
			 * If the child constraint is "not valid" then cannot merge with a
			 * valid parent constraint
			 *
			 * 若子约束是 not valid，则不能和有效的父约束合并
			 */
			if (parent_con->convalidated && child_con->conenforced &&
				!child_con->convalidated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on child table \"%s\"",
								NameStr(child_con->conname), RelationGetRelationName(child_rel))));

			/*
			 * A NOT ENFORCED child constraint cannot be merged with an
			 * ENFORCED parent constraint. However, the reverse is allowed,
			 * where the child constraint is ENFORCED.
			 *
			 * NOT ENFORCED 的子约束不能和 ENFORCED 的父约束合并。反过来则允许，即子约束是 ENFORCED。
			 */
			if (parent_con->conenforced && !child_con->conenforced)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with NOT ENFORCED constraint on child table \"%s\"",
								NameStr(child_con->conname), RelationGetRelationName(child_rel))));

			/*
			 * OK, bump the child constraint's inheritance count.  (If we fail
			 * later on, this change will just roll back.)
			 *
			 * 好，增加子约束的继承计数。（若后面失败，这次修改会回滚。）
			 */
			child_copy = heap_copytuple(child_tuple);
			child_con = (Form_pg_constraint) GETSTRUCT(child_copy);

			if (pg_add_s16_overflow(child_con->coninhcount, 1,
									&child_con->coninhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * In case of partitions, an inherited constraint must be
			 * inherited only once since it cannot have multiple parents and
			 * it is never considered local.
			 *
			 * 对分区，继承来的约束只能继承一次，因为它不能有多个父表，也从不被视为局部的。
			 */
			if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			{
				Assert(child_con->coninhcount == 1);
				child_con->conislocal = false;
			}

			CatalogTupleUpdate(constraintrel, &child_copy->t_self, child_copy);
			heap_freetuple(child_copy);

			found = true;
			break;
		}

		systable_endscan(child_scan);

		if (!found)
		{
			if (parent_con->contype == CONSTRAINT_NOTNULL)
				ereport(ERROR,
						errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("column \"%s\" in child table \"%s\" must be marked NOT NULL",
							   get_attname(parent_relid,
										   extractNotNullColumn(parent_tuple),
										   false),
							   RelationGetRelationName(child_rel)));

			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("child table is missing constraint \"%s\"",
							NameStr(parent_con->conname))));
		}
	}

	systable_endscan(parent_scan);
	table_close(constraintrel, RowExclusiveLock);
}

/*
 * ALTER TABLE NO INHERIT
 *
 * ALTER TABLE NO INHERIT（脱离继承）
 *
 * Return value is the address of the relation that is no longer parent.
 *
 * 返回值是不再作为父表的那个关系的地址。
 */
static ObjectAddress
ATExecDropInherit(Relation rel, RangeVar *parent, LOCKMODE lockmode)
{
	ObjectAddress address;
	Relation	parent_rel;

	if (rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot change inheritance of a partition")));

	/*
	 * AccessShareLock on the parent is probably enough, seeing that DROP
	 * TABLE doesn't lock parent tables at all.  We need some lock since we'll
	 * be inspecting the parent's schema.
	 *
	 * 对父表用 AccessShareLock 大概就够了，因为 DROP TABLE 根本不锁父表。我们需要某种锁，
	 * 因为要检查父表的模式。
	 */
	parent_rel = table_openrv(parent, AccessShareLock);

	/*
	 * We don't bother to check ownership of the parent table --- ownership of
	 * the child is presumed enough rights.
	 *
	 * 我们懒得检查父表的所有权，假定拥有子表就有足够权限。
	 */

	/* Off to RemoveInheritance() where most of the work happens */
	/*
	 *
	 * 大部分工作交给 RemoveInheritance()
	 */
	RemoveInheritance(rel, parent_rel, false);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* keep our lock on the parent relation until commit */
	/*
	 *
	 * 把对父关系的锁保持到提交
	 */
	table_close(parent_rel, NoLock);

	return address;
}

/*
 * MarkInheritDetached
 *
 * 函数 MarkInheritDetached
 *
 * Set inhdetachpending for a partition, for ATExecDetachPartition
 * in concurrent mode.  While at it, verify that no other partition is
 * already pending detach.
 *
 * 为并发模式下的 ATExecDetachPartition 给分区设置 inhdetachpending。
 * 顺便验证没有别的分区已经在等待分离。
 */
static void
MarkInheritDetached(Relation child_rel, Relation parent_rel)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	inheritsTuple;
	bool		found = false;

	Assert(parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Find pg_inherits entries by inhparent.  (We need to scan them all in
	 * order to verify that no other partition is pending detach.)
	 *
	 * 按 inhparent 查找 pg_inherits 项。（必须全部扫描，以验证没有别的分区正在等待分离。）
	 */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, InheritsParentIndexId,
							  true, NULL, 1, &key);

	while (HeapTupleIsValid(inheritsTuple = systable_getnext(scan)))
	{
		Form_pg_inherits inhForm;

		inhForm = (Form_pg_inherits) GETSTRUCT(inheritsTuple);
		if (inhForm->inhdetachpending)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("partition \"%s\" already pending detach in partitioned table \"%s.%s\"",
						   get_rel_name(inhForm->inhrelid),
						   get_namespace_name(parent_rel->rd_rel->relnamespace),
						   RelationGetRelationName(parent_rel)),
					errhint("Use ALTER TABLE ... DETACH PARTITION ... FINALIZE to complete the pending detach operation."));

		if (inhForm->inhrelid == RelationGetRelid(child_rel))
		{
			HeapTuple	newtup;

			newtup = heap_copytuple(inheritsTuple);
			((Form_pg_inherits) GETSTRUCT(newtup))->inhdetachpending = true;

			CatalogTupleUpdate(catalogRelation,
							   &inheritsTuple->t_self,
							   newtup);
			found = true;
			heap_freetuple(newtup);
			/* keep looking, to ensure we catch others pending detach */
			/*
			 *
			 * 继续找，确保能抓住其他正在等待分离的
			 */
		}
	}

	/* Done */
	/*
	 *
	 * 完成
	 */
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
						RelationGetRelationName(child_rel),
						RelationGetRelationName(parent_rel))));
}

/*
 * RemoveInheritance
 *
 * 函数 RemoveInheritance
 *
 * Drop a parent from the child's parents. This just adjusts the attinhcount
 * and attislocal of the columns and removes the pg_inherit and pg_depend
 * entries.  expect_detached is passed down to DeleteInheritsTuple, q.v..
 *
 * 从子表的父表里去掉一个父表。这只调整列的 attinhcount 和 attislocal，并删除 pg_inherit 和
 * pg_depend 项。expect_detached 会传给 DeleteInheritsTuple，见彼处。
 *
 * If attinhcount goes to 0 then attislocal gets set to true. If it goes back
 * up attislocal stays true, which means if a child is ever removed from a
 * parent then its columns will never be automatically dropped which may
 * surprise. But at least we'll never surprise by dropping columns someone
 * isn't expecting to be dropped which would actually mean data loss.
 *
 * 若 attinhcount 降到 0，就把 attislocal 设为真。若以后又升回去，attislocal 仍保持真，
 * 这意味着子表一旦从父表移除，它的列就再也不会被自动删除，这可能让人吃惊。但至少我们不会意外删掉别人没料到会被删的列，
 * 那才是真正的数据丢失。
 *
 * coninhcount and conislocal for inherited constraints are adjusted in
 * exactly the same way.
 *
 * 继承约束的 coninhcount 和 conislocal 按完全相同的方式调整。
 *
 * Common to ATExecDropInherit() and ATExecDetachPartition().
 *
 * ATExecDropInherit() 和 ATExecDetachPartition() 共用。
 */
static void
RemoveInheritance(Relation child_rel, Relation parent_rel, bool expect_detached)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	attributeTuple,
				constraintTuple;
	AttrMap    *attmap;
	List	   *connames;
	List	   *nncolumns;
	bool		found;
	bool		is_partitioning;

	is_partitioning = (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	found = DeleteInheritsTuple(RelationGetRelid(child_rel),
								RelationGetRelid(parent_rel),
								expect_detached,
								RelationGetRelationName(child_rel));
	if (!found)
	{
		if (is_partitioning)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" is not a partition of relation \"%s\"",
							RelationGetRelationName(child_rel),
							RelationGetRelationName(parent_rel))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" is not a parent of relation \"%s\"",
							RelationGetRelationName(parent_rel),
							RelationGetRelationName(child_rel))));
	}

	/*
	 * Search through child columns looking for ones matching parent rel
	 *
	 * 在子表的列里搜索与父关系匹配的那些
	 */
	catalogRelation = table_open(AttributeRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, AttributeRelidNumIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(attributeTuple = systable_getnext(scan)))
	{
		Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(attributeTuple);

		/* Ignore if dropped or not inherited */
		/*
		 *
		 * 若已删除或不是继承来的，则忽略
		 */
		if (att->attisdropped)
			continue;
		if (att->attinhcount <= 0)
			continue;

		if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel),
										NameStr(att->attname)))
		{
			/* Decrement inhcount and possibly set islocal to true */
			/*
			 *
			 * 减少 inhcount，并可能把 islocal 设为真
			 */
			HeapTuple	copyTuple = heap_copytuple(attributeTuple);
			Form_pg_attribute copy_att = (Form_pg_attribute) GETSTRUCT(copyTuple);

			copy_att->attinhcount--;
			if (copy_att->attinhcount == 0)
				copy_att->attislocal = true;

			CatalogTupleUpdate(catalogRelation, &copyTuple->t_self, copyTuple);
			heap_freetuple(copyTuple);
		}
	}
	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	/*
	 * Likewise, find inherited check and not-null constraints and disinherit
	 * them. To do this, we first need a list of the names of the parent's
	 * check constraints.  (We cheat a bit by only checking for name matches,
	 * assuming that the expressions will match.)
	 *
	 * 同样，找出继承来的 CHECK 和 NOT NULL 约束并解除继承。为此先需要父表 CHECK 约束的名字列表。
	 * （我们偷懒只按名字匹配，假定表达式会匹配。）
	 *
	 * For NOT NULL columns, we store column numbers to match, mapping them in
	 * to the child rel's attribute numbers.
	 *
	 * 对 NOT NULL 列，我们保存要匹配的列号，并映射成子关系的属性号。
	 */
	attmap = build_attrmap_by_name(RelationGetDescr(child_rel),
								   RelationGetDescr(parent_rel),
								   false);

	catalogRelation = table_open(ConstraintRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(parent_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, key);

	connames = NIL;
	nncolumns = NIL;

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);

		if (con->connoinherit)
			continue;

		if (con->contype == CONSTRAINT_CHECK)
			connames = lappend(connames, pstrdup(NameStr(con->conname)));
		if (con->contype == CONSTRAINT_NOTNULL)
		{
			AttrNumber	parent_attno = extractNotNullColumn(constraintTuple);

			nncolumns = lappend_int(nncolumns, attmap->attnums[parent_attno - 1]);
		}
	}

	systable_endscan(scan);

	/* Now scan the child's constraints to find matches */
	/*
	 *
	 * 现在扫描子表的约束以寻找匹配
	 */
	ScanKeyInit(&key[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid(constraintTuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(constraintTuple);
		bool		match = false;

		/*
		 * Match CHECK constraints by name, not-null constraints by column
		 * number, and ignore all others.
		 *
		 * CHECK 约束按名字匹配，NOT NULL 约束按列号匹配，其余全部忽略。
		 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			foreach_ptr(char, chkname, connames)
			{
				if (con->contype == CONSTRAINT_CHECK &&
					strcmp(NameStr(con->conname), chkname) == 0)
				{
					match = true;
					connames = foreach_delete_current(connames, chkname);
					break;
				}
			}
		}
		else if (con->contype == CONSTRAINT_NOTNULL)
		{
			AttrNumber	child_attno = extractNotNullColumn(constraintTuple);

			foreach_int(prevattno, nncolumns)
			{
				if (prevattno == child_attno)
				{
					match = true;
					nncolumns = foreach_delete_current(nncolumns, prevattno);
					break;
				}
			}
		}
		else
			continue;

		if (match)
		{
			/* Decrement inhcount and possibly set islocal to true */
			/*
			 *
			 * 减少 inhcount，并可能把 islocal 设为真
			 */
			HeapTuple	copyTuple = heap_copytuple(constraintTuple);
			Form_pg_constraint copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);

			if (copy_con->coninhcount <= 0) /* shouldn't happen */
							/*
							 *
							 * 不该发生
							 */
				elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
					 RelationGetRelid(child_rel), NameStr(copy_con->conname));

			copy_con->coninhcount--;
			if (copy_con->coninhcount == 0)
				copy_con->conislocal = true;

			CatalogTupleUpdate(catalogRelation, &copyTuple->t_self, copyTuple);
			heap_freetuple(copyTuple);
		}
	}

	/* We should have matched all constraints */
	/*
	 *
	 * 我们应该已经匹配了所有约束
	 */
	if (connames != NIL || nncolumns != NIL)
		elog(ERROR, "%d unmatched constraints while removing inheritance from \"%s\" to \"%s\"",
			 list_length(connames) + list_length(nncolumns),
			 RelationGetRelationName(child_rel), RelationGetRelationName(parent_rel));

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);

	drop_parent_dependency(RelationGetRelid(child_rel),
						   RelationRelationId,
						   RelationGetRelid(parent_rel),
						   child_dependency_type(is_partitioning));

	/*
	 * Post alter hook of this inherits. Since object_access_hook doesn't take
	 * multiple object identifiers, we relay oid of parent relation using
	 * auxiliary_id argument.
	 *
	 * 这次继承的修改后钩子。object_access_hook 不接受多个对象标识，所以用 auxiliary_id 参数转交父关系的
	 * OID。
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 RelationGetRelid(child_rel), 0,
								 RelationGetRelid(parent_rel), false);
}

/*
 * Drop the dependency created by StoreCatalogInheritance1 (CREATE TABLE
 * INHERITS/ALTER TABLE INHERIT -- refclassid will be RelationRelationId) or
 * heap_create_with_catalog (CREATE TABLE OF/ALTER TABLE OF -- refclassid will
 * be TypeRelationId).  There's no convenient way to do this, so go trawling
 * through pg_depend.
 *
 * 去掉由 StoreCatalogInheritance1（CREATE TABLE INHERITS/ALTER TABLE
 * INHERIT，refclassid 将是 RelationRelationId）或
 * heap_create_with_catalog（CREATE TABLE OF/ALTER TABLE OF，
 * refclassid 将是 TypeRelationId）创建的依赖。没有方便的办法，所以到 pg_depend 里去捞。
 */
static void
drop_parent_dependency(Oid relid, Oid refclassid, Oid refobjid,
					   DependencyType deptype)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	depTuple;

	catalogRelation = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(catalogRelation, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(depTuple);

		if (dep->refclassid == refclassid &&
			dep->refobjid == refobjid &&
			dep->refobjsubid == 0 &&
			dep->deptype == deptype)
			CatalogTupleDelete(catalogRelation, &depTuple->t_self);
	}

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 * ALTER TABLE OF
 *
 * ALTER TABLE OF（把表设为某类型的类型表）
 *
 * Attach a table to a composite type, as though it had been created with CREATE
 * TABLE OF.  All attname, atttypid, atttypmod and attcollation must match.  The
 * subject table must not have inheritance parents.  These restrictions ensure
 * that you cannot create a configuration impossible with CREATE TABLE OF alone.
 *
 * 把表挂到一个复合类型上，就像它是用 CREATE TABLE OF 创建的。所有 attname、atttypid、
 * atttypmod 和 attcollation 都必须匹配。目标表不能有继承父表。这些限制确保你无法造出单靠 CREATE
 * TABLE OF 无法造出的配置。
 *
 * The address of the type is returned.
 *
 * 返回该类型的地址。
 */
static ObjectAddress
ATExecAddOf(Relation rel, const TypeName *ofTypename, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Type		typetuple;
	Form_pg_type typeform;
	Oid			typeid;
	Relation	inheritsRelation,
				relationRelation;
	SysScanDesc scan;
	ScanKeyData key;
	AttrNumber	table_attno,
				type_attno;
	TupleDesc	typeTupleDesc,
				tableTupleDesc;
	ObjectAddress tableobj,
				typeobj;
	HeapTuple	classtuple;

	/* Validate the type. */
	/*
	 *
	 * 校验该类型。
	 */
	typetuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(typetuple);
	typeform = (Form_pg_type) GETSTRUCT(typetuple);
	typeid = typeform->oid;

	/* Fail if the table has any inheritance parents. */
	/*
	 *
	 * 若表有任何继承父表则失败。
	 */
	inheritsRelation = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(inheritsRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("typed tables cannot inherit")));
	systable_endscan(scan);
	table_close(inheritsRelation, AccessShareLock);

	/*
	 * Check the tuple descriptors for compatibility.  Unlike inheritance, we
	 * require that the order also match.  However, attnotnull need not match.
	 *
	 * 检查元组描述符是否兼容。和继承不同，我们要求顺序也匹配。不过 attnotnull 不必匹配。
	 */
	typeTupleDesc = lookup_rowtype_tupdesc(typeid, -1);
	tableTupleDesc = RelationGetDescr(rel);
	table_attno = 1;
	for (type_attno = 1; type_attno <= typeTupleDesc->natts; type_attno++)
	{
		Form_pg_attribute type_attr,
					table_attr;
		const char *type_attname,
				   *table_attname;

		/* Get the next non-dropped type attribute. */
		/*
		 *
		 * 取下一个未删除的类型属性。
		 */
		type_attr = TupleDescAttr(typeTupleDesc, type_attno - 1);
		if (type_attr->attisdropped)
			continue;
		type_attname = NameStr(type_attr->attname);

		/* Get the next non-dropped table attribute. */
		/*
		 *
		 * 取下一个未删除的表属性。
		 */
		do
		{
			if (table_attno > tableTupleDesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table is missing column \"%s\"",
								type_attname)));
			table_attr = TupleDescAttr(tableTupleDesc, table_attno - 1);
			table_attno++;
		} while (table_attr->attisdropped);
		table_attname = NameStr(table_attr->attname);

		/* Compare name. */
		/*
		 *
		 * 比较名字。
		 */
		if (strncmp(table_attname, type_attname, NAMEDATALEN) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has column \"%s\" where type requires \"%s\"",
							table_attname, type_attname)));

		/* Compare type. */
		/*
		 *
		 * 比较类型。
		 */
		if (table_attr->atttypid != type_attr->atttypid ||
			table_attr->atttypmod != type_attr->atttypmod ||
			table_attr->attcollation != type_attr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), type_attname)));
	}
	ReleaseTupleDesc(typeTupleDesc);

	/* Any remaining columns at the end of the table had better be dropped. */
	/*
	 *
	 * 表末尾剩下的列最好都是已删除的。
	 */
	for (; table_attno <= tableTupleDesc->natts; table_attno++)
	{
		Form_pg_attribute table_attr = TupleDescAttr(tableTupleDesc,
													 table_attno - 1);

		if (!table_attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has extra column \"%s\"",
							NameStr(table_attr->attname))));
	}

	/* If the table was already typed, drop the existing dependency. */
	/*
	 *
	 * 若表已经是类型表，去掉现有依赖。
	 */
	if (rel->rd_rel->reloftype)
		drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
							   DEPENDENCY_NORMAL);

	/* Record a dependency on the new type. */
	/*
	 *
	 * 记录对新类型的依赖。
	 */
	tableobj.classId = RelationRelationId;
	tableobj.objectId = relid;
	tableobj.objectSubId = 0;
	typeobj.classId = TypeRelationId;
	typeobj.objectId = typeid;
	typeobj.objectSubId = 0;
	recordDependencyOn(&tableobj, &typeobj, DEPENDENCY_NORMAL);

	/* Update pg_class.reloftype */
	/*
	 *
	 * 更新 pg_class.reloftype
	 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	classtuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(classtuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(classtuple))->reloftype = typeid;
	CatalogTupleUpdate(relationRelation, &classtuple->t_self, classtuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(classtuple);
	table_close(relationRelation, RowExclusiveLock);

	ReleaseSysCache(typetuple);

	return typeobj;
}

/*
 * ALTER TABLE NOT OF
 *
 * ALTER TABLE NOT OF（取消类型表）
 *
 * Detach a typed table from its originating type.  Just clear reloftype and
 * remove the dependency.
 *
 * 把类型表从它的源类型上拆下来。只需清掉 reloftype 并去掉依赖。
 */
static void
ATExecDropOf(Relation rel, LOCKMODE lockmode)
{
	Oid			relid = RelationGetRelid(rel);
	Relation	relationRelation;
	HeapTuple	tuple;

	if (!OidIsValid(rel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a typed table",
						RelationGetRelationName(rel))));

	/*
	 * We don't bother to check ownership of the type --- ownership of the
	 * table is presumed enough rights.  No lock required on the type, either.
	 *
	 * 我们懒得检查类型的所有权，假定拥有表就有足够权限。对类型也不需要锁。
	 */

	drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
						   DEPENDENCY_NORMAL);

	/* Clear pg_class.reloftype */
	/*
	 *
	 * 清掉 pg_class.reloftype
	 */
	relationRelation = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	((Form_pg_class) GETSTRUCT(tuple))->reloftype = InvalidOid;
	CatalogTupleUpdate(relationRelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}

/*
 * relation_mark_replica_identity: Update a table's replica identity
 *
 * relation_mark_replica_identity：更新表的复制标识
 *
 * Iff ri_type = REPLICA_IDENTITY_INDEX, indexOid must be the Oid of a suitable
 * index. Otherwise, it must be InvalidOid.
 *
 * 当且仅当 ri_type 为 REPLICA_IDENTITY_INDEX 时，indexOid 必须是合适索引的 OID。
 * 否则必须是 InvalidOid。
 *
 * Caller had better hold an exclusive lock on the relation, as the results
 * of running two of these concurrently wouldn't be pretty.
 *
 * 调用方最好对关系持有排他锁，两个这样的操作并发跑的结果不会好看。
 */
static void
relation_mark_replica_identity(Relation rel, char ri_type, Oid indexOid,
							   bool is_internal)
{
	Relation	pg_index;
	Relation	pg_class;
	HeapTuple	pg_class_tuple;
	HeapTuple	pg_index_tuple;
	Form_pg_class pg_class_form;
	Form_pg_index pg_index_form;
	ListCell   *index;

	/*
	 * Check whether relreplident has changed, and update it if so.
	 *
	 * 检查 relreplident 是否改变，若改变则更新。
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);
	pg_class_tuple = SearchSysCacheCopy1(RELOID,
										 ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "cache lookup failed for relation \"%s\"",
			 RelationGetRelationName(rel));
	pg_class_form = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	if (pg_class_form->relreplident != ri_type)
	{
		pg_class_form->relreplident = ri_type;
		CatalogTupleUpdate(pg_class, &pg_class_tuple->t_self, pg_class_tuple);
	}
	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(pg_class_tuple);

	/*
	 * Update the per-index indisreplident flags correctly.
	 *
	 * 正确更新每个索引的 indisreplident 标志。
	 */
	pg_index = table_open(IndexRelationId, RowExclusiveLock);
	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);
		bool		dirty = false;

		pg_index_tuple = SearchSysCacheCopy1(INDEXRELID,
											 ObjectIdGetDatum(thisIndexOid));
		if (!HeapTupleIsValid(pg_index_tuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		pg_index_form = (Form_pg_index) GETSTRUCT(pg_index_tuple);

		if (thisIndexOid == indexOid)
		{
			/* Set the bit if not already set. */
			/*
			 *
			 * 若还没置上该位，就置上。
			 */
			if (!pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = true;
			}
		}
		else
		{
			/* Unset the bit if set. */
			/*
			 *
			 * 若该位已置上，就清掉。
			 */
			if (pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = false;
			}
		}

		if (dirty)
		{
			CatalogTupleUpdate(pg_index, &pg_index_tuple->t_self, pg_index_tuple);
			InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
										 InvalidOid, is_internal);

			/*
			 * Invalidate the relcache for the table, so that after we commit
			 * all sessions will refresh the table's replica identity index
			 * before attempting any UPDATE or DELETE on the table.  (If we
			 * changed the table's pg_class row above, then a relcache inval
			 * is already queued due to that; but we might not have.)
			 *
			 * 使该表的 relcache 失效，这样提交之后所有会话都会在对该表做任何 UPDATE 或 DELETE 之前刷新表的复制标识索引。
			 * （若上面改了表的 pg_class 行，已经因此排了一次 relcache 失效；但我们可能没改。）
			 */
			CacheInvalidateRelcache(rel);
		}
		heap_freetuple(pg_index_tuple);
	}

	table_close(pg_index, RowExclusiveLock);
}

/*
 * ALTER TABLE <name> REPLICA IDENTITY ...
 *
 * ALTER TABLE name REPLICA IDENTITY ...（设置复制标识）
 */
static void
ATExecReplicaIdentity(Relation rel, ReplicaIdentityStmt *stmt, LOCKMODE lockmode)
{
	Oid			indexOid;
	Relation	indexRel;
	int			key;

	if (stmt->identity_type == REPLICA_IDENTITY_DEFAULT)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_FULL)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_NOTHING)
	{
		relation_mark_replica_identity(rel, stmt->identity_type, InvalidOid, true);
		return;
	}
	else if (stmt->identity_type == REPLICA_IDENTITY_INDEX)
	{
		 /* fallthrough */ ;
		 /*
		  *
		  * 落入下面的处理
		  */
	}
	else
		elog(ERROR, "unexpected identity type %u", stmt->identity_type);

	/* Check that the index exists */
	/*
	 *
	 * 检查索引是否存在
	 */
	indexOid = get_relname_relid(stmt->name, rel->rd_rel->relnamespace);
	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						stmt->name, RelationGetRelationName(rel))));

	indexRel = index_open(indexOid, ShareLock);

	/* Check that the index is on the relation we're altering. */
	/*
	 *
	 * 检查该索引是否在我们正在修改的关系上。
	 */
	if (indexRel->rd_index == NULL ||
		indexRel->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(indexRel),
						RelationGetRelationName(rel))));

	/*
	 * The AM must support uniqueness, and the index must in fact be unique.
	 * If we have a WITHOUT OVERLAPS constraint (identified by uniqueness +
	 * exclusion), we can use that too.
	 *
	 * 访问方法必须支持唯一性，而且索引实际上必须是唯一的。若有 WITHOUT OVERLAPS 约束（由唯一性加排他识别），也可以用它。
	 */
	if ((!indexRel->rd_indam->amcanunique ||
		 !indexRel->rd_index->indisunique) &&
		!(indexRel->rd_index->indisunique && indexRel->rd_index->indisexclusion))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot use non-unique index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Deferred indexes are not guaranteed to be always unique. */
	/*
	 *
	 * 可推迟的索引不保证始终唯一。
	 */
	if (!indexRel->rd_index->indimmediate)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use non-immediate index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Expression indexes aren't supported. */
	/*
	 *
	 * 不支持表达式索引。
	 */
	if (RelationGetIndexExpressions(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use expression index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* Predicate indexes aren't supported. */
	/*
	 *
	 * 不支持带谓词的索引。
	 */
	if (RelationGetIndexPredicate(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use partial index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));

	/* Check index for nullable columns. */
	/*
	 *
	 * 检查索引是否含可空列。
	 */
	for (key = 0; key < IndexRelationGetNumberOfKeyAttributes(indexRel); key++)
	{
		int16		attno = indexRel->rd_index->indkey.values[key];
		Form_pg_attribute attr;

		/*
		 * Reject any other system columns.  (Going forward, we'll disallow
		 * indexes containing such columns in the first place, but they might
		 * exist in older branches.)
		 *
		 * 拒绝任何其他系统列。（以后我们会从一开始就禁止包含这类列的索引，但旧分支里可能已经存在。）
		 */
		if (attno <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("index \"%s\" cannot be used as replica identity because column %d is a system column",
							RelationGetRelationName(indexRel), attno)));

		attr = TupleDescAttr(rel->rd_att, attno - 1);
		if (!attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" cannot be used as replica identity because column \"%s\" is nullable",
							RelationGetRelationName(indexRel),
							NameStr(attr->attname))));
	}

	/* This index is suitable for use as a replica identity. Mark it. */
	/*
	 *
	 * 这个索引适合用作复制标识。标上它。
	 */
	relation_mark_replica_identity(rel, stmt->identity_type, indexOid, true);

	index_close(indexRel, NoLock);
}

/*
 * ALTER TABLE ENABLE/DISABLE ROW LEVEL SECURITY
 *
 * ALTER TABLE ENABLE/DISABLE ROW LEVEL SECURITY（启用或禁用行级安全）
 */
static void
ATExecSetRowSecurity(Relation rel, bool rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	/* Pull the record for this relation and update it */
	/*
	 *
	 * 取出该关系的记录并更新
	 */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relrowsecurity = rls;
	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), 0);

	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER TABLE FORCE/NO FORCE ROW LEVEL SECURITY
 *
 * ALTER TABLE FORCE/NO FORCE ROW LEVEL SECURITY（强制或不强制行级安全）
 */
static void
ATExecForceNoForceRowSecurity(Relation rel, bool force_rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	((Form_pg_class) GETSTRUCT(tuple))->relforcerowsecurity = force_rls;
	CatalogTupleUpdate(pg_class, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), 0);

	table_close(pg_class, RowExclusiveLock);
	heap_freetuple(tuple);
}

/*
 * ALTER FOREIGN TABLE <name> OPTIONS (...)
 *
 * ALTER FOREIGN TABLE name OPTIONS (...)（设置外部表选项）
 */
static void
ATExecGenericOptions(Relation rel, List *options)
{
	Relation	ftrel;
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	HeapTuple	tuple;
	bool		isnull;
	Datum		repl_val[Natts_pg_foreign_table];
	bool		repl_null[Natts_pg_foreign_table];
	bool		repl_repl[Natts_pg_foreign_table];
	Datum		datum;
	Form_pg_foreign_table tableform;

	if (options == NIL)
		return;

	ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(FOREIGNTABLEREL,
								ObjectIdGetDatum(rel->rd_id));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign table \"%s\" does not exist",
						RelationGetRelationName(rel))));
	tableform = (Form_pg_foreign_table) GETSTRUCT(tuple);
	server = GetForeignServer(tableform->ftserver);
	fdw = GetForeignDataWrapper(server->fdwid);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* Extract the current options */
	/*
	 *
	 * 取出当前选项
	 */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tuple,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* Transform the options */
	/*
	 *
	 * 变换这些选项
	 */
	datum = transformGenericOptions(ForeignTableRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_foreign_table_ftoptions - 1] = datum;
	else
		repl_null[Anum_pg_foreign_table_ftoptions - 1] = true;

	repl_repl[Anum_pg_foreign_table_ftoptions - 1] = true;

	/* Everything looks good - update the tuple */
	/*
	 *
	 * 一切看起来都好，更新元组
	 */

	tuple = heap_modify_tuple(tuple, RelationGetDescr(ftrel),
							  repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(ftrel, &tuple->t_self, tuple);

	/*
	 * Invalidate relcache so that all sessions will refresh any cached plans
	 * that might depend on the old options.
	 *
	 * 使 relcache 失效，以便所有会话刷新任何可能依赖旧选项的缓存计划。
	 */
	CacheInvalidateRelcache(rel);

	InvokeObjectPostAlterHook(ForeignTableRelationId,
							  RelationGetRelid(rel), 0);

	table_close(ftrel, RowExclusiveLock);

	heap_freetuple(tuple);
}

/*
 * ALTER TABLE ALTER COLUMN SET COMPRESSION
 *
 * ALTER TABLE ALTER COLUMN SET COMPRESSION（设置列压缩）
 *
 * Return value is the address of the modified column
 *
 * 返回值是被修改列的地址
 */
static ObjectAddress
ATExecSetCompression(Relation rel,
					 const char *column,
					 Node *newValue,
					 LOCKMODE lockmode)
{
	Relation	attrel;
	HeapTuple	tuple;
	Form_pg_attribute atttableform;
	AttrNumber	attnum;
	char	   *compression;
	char		cmethod;
	ObjectAddress address;

	compression = strVal(newValue);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* copy the cache entry so we can scribble on it below */
	/*
	 *
	 * 拷贝缓存项，以便下面涂改
	 */
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), column);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, RelationGetRelationName(rel))));

	/* prevent them from altering a system attribute */
	/*
	 *
	 * 阻止他们修改系统属性
	 */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", column)));

	/*
	 * Check that column type is compressible, then get the attribute
	 * compression method code
	 *
	 * 检查列类型是否可压缩，然后取得属性压缩方法代码
	 */
	cmethod = GetAttributeCompression(atttableform->atttypid, compression);

	/* update pg_attribute entry */
	/*
	 *
	 * 更新 pg_attribute 项
	 */
	atttableform->attcompression = cmethod;
	CatalogTupleUpdate(attrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);

	/*
	 * Apply the change to indexes as well (only for simple index columns,
	 * matching behavior of index.c ConstructTupleDescriptor()).
	 *
	 * 把改动也应用到索引上（只针对简单索引列，与 index.c 的 ConstructTupleDescriptor() 行为一致）。
	 */
	SetIndexStorageProperties(rel, attrel, attnum,
							  false, 0,
							  true, cmethod,
							  lockmode);

	heap_freetuple(tuple);

	table_close(attrel, RowExclusiveLock);

	/* make changes visible */
	/*
	 *
	 * 让修改可见
	 */
	CommandCounterIncrement();

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * Preparation phase for SET LOGGED/UNLOGGED
 *
 * SET LOGGED/UNLOGGED 的准备阶段
 *
 * This verifies that we're not trying to change a temp table.  Also,
 * existing foreign key constraints are checked to avoid ending up with
 * permanent tables referencing unlogged tables.
 *
 * 这会验证我们不是在试图改临时表。同时检查现有外键约束，以免出现永久表引用 unlogged 表。
 */
static void
ATPrepChangePersistence(AlteredTableInfo *tab, Relation rel, bool toLogged)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/*
	 * Disallow changing status for a temp table.  Also verify whether we can
	 * get away with doing nothing; in such cases we don't need to run the
	 * checks below, either.
	 *
	 * 不允许改变临时表的状态。同时验证能否什么也不做就了事；那种情况下下面的检查也不必跑。
	 */
	switch (rel->rd_rel->relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change logged status of table \"%s\" because it is temporary",
							RelationGetRelationName(rel)),
					 errtable(rel)));
			break;
		case RELPERSISTENCE_PERMANENT:
			if (toLogged)
				/* nothing to do */
				/*
				 *
				 * 无事可做
				 */
				return;
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!toLogged)
				/* nothing to do */
				/*
				 *
				 * 无事可做
				 */
				return;
			break;
	}

	/*
	 * Check that the table is not part of any publication when changing to
	 * UNLOGGED, as UNLOGGED tables can't be published.
	 *
	 * 改成 UNLOGGED 时，检查该表不属于任何发布，因为 UNLOGGED 表不能被发布。
	 */
	if (!toLogged &&
		GetRelationPublications(RelationGetRelid(rel)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot change table \"%s\" to unlogged because it is part of a publication",
						RelationGetRelationName(rel)),
				 errdetail("Unlogged relations cannot be replicated.")));

	/*
	 * Check existing foreign key constraints to preserve the invariant that
	 * permanent tables cannot reference unlogged ones.  Self-referencing
	 * foreign keys can safely be ignored.
	 *
	 * 检查现有外键约束，以保持永久表不能引用 unlogged 表这个不变量。自引用外键可以安全地忽略。
	 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	/*
	 * Scan conrelid if changing to permanent, else confrelid.  This also
	 * determines whether a useful index exists.
	 *
	 * 若改成永久表就扫描 conrelid，否则扫描 confrelid。这也决定是否存在有用的索引。
	 */
	ScanKeyInit(&skey[0],
				toLogged ? Anum_pg_constraint_conrelid :
				Anum_pg_constraint_confrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	scan = systable_beginscan(pg_constraint,
							  toLogged ? ConstraintRelidTypidNameIndexId : InvalidOid,
							  true, NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		if (con->contype == CONSTRAINT_FOREIGN)
		{
			Oid			foreignrelid;
			Relation	foreignrel;

			/* the opposite end of what we used as scankey */
			/*
			 *
			 * 与我们用作扫描键的那一端相反的另一端
			 */
			foreignrelid = toLogged ? con->confrelid : con->conrelid;

			/* ignore if self-referencing */
			/*
			 *
			 * 自引用则忽略
			 */
			if (RelationGetRelid(rel) == foreignrelid)
				continue;

			foreignrel = relation_open(foreignrelid, AccessShareLock);

			if (toLogged)
			{
				if (!RelationIsPermanent(foreignrel))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to logged because it references unlogged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}
			else
			{
				if (RelationIsPermanent(foreignrel))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("could not change table \"%s\" to unlogged because it references logged table \"%s\"",
									RelationGetRelationName(rel),
									RelationGetRelationName(foreignrel)),
							 errtableconstraint(rel, NameStr(con->conname))));
			}

			relation_close(foreignrel, AccessShareLock);
		}
	}

	systable_endscan(scan);

	table_close(pg_constraint, AccessShareLock);

	/* force rewrite if necessary; see comment in ATRewriteTables */
	/*
	 *
	 * 必要时强制重写；见 ATRewriteTables 里的注释
	 */
	tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
	if (toLogged)
		tab->newrelpersistence = RELPERSISTENCE_PERMANENT;
	else
		tab->newrelpersistence = RELPERSISTENCE_UNLOGGED;
	tab->chgPersistence = true;
}

/*
 * Execute ALTER TABLE SET SCHEMA
 *
 * 执行 ALTER TABLE SET SCHEMA
 */
ObjectAddress
AlterTableNamespace(AlterObjectSchemaStmt *stmt, Oid *oldschema)
{
	Relation	rel;
	Oid			relid;
	Oid			oldNspOid;
	Oid			nspOid;
	RangeVar   *newrv;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	relid = RangeVarGetRelidExtended(stmt->relation, AccessExclusiveLock,
									 stmt->missing_ok ? RVR_MISSING_OK : 0,
									 RangeVarCallbackForAlterRelation,
									 stmt);

	if (!OidIsValid(relid))
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->relation->relname)));
		return InvalidObjectAddress;
	}

	rel = relation_open(relid, NoLock);

	oldNspOid = RelationGetNamespace(rel);

	/* If it's an owned sequence, disallow moving it by itself. */
	/*
	 *
	 * 若它是被拥有的序列，不允许单独移动它。
	 */
	if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
	{
		Oid			tableId;
		int32		colId;

		if (sequenceIsOwned(relid, DEPENDENCY_AUTO, &tableId, &colId) ||
			sequenceIsOwned(relid, DEPENDENCY_INTERNAL, &tableId, &colId))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot move an owned sequence into another schema"),
					 errdetail("Sequence \"%s\" is linked to table \"%s\".",
							   RelationGetRelationName(rel),
							   get_rel_name(tableId))));
	}

	/* Get and lock schema OID and check its permissions. */
	/*
	 *
	 * 取得并锁住模式 OID，并检查其权限。
	 */
	newrv = makeRangeVar(stmt->newschema, RelationGetRelationName(rel), -1);
	nspOid = RangeVarGetAndCheckCreationNamespace(newrv, NoLock, NULL);

	/* common checks on switching namespaces */
	/*
	 *
	 * 切换命名空间时的公共检查
	 */
	CheckSetNamespace(oldNspOid, nspOid);

	objsMoved = new_object_addresses();
	AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);
	free_object_addresses(objsMoved);

	ObjectAddressSet(myself, RelationRelationId, relid);

	if (oldschema)
		*oldschema = oldNspOid;

	/* close rel, but keep lock until commit */
	/*
	 *
	 * 关闭关系，但锁保持到提交
	 */
	relation_close(rel, NoLock);

	return myself;
}

/*
 * The guts of relocating a table or materialized view to another namespace:
 * besides moving the relation itself, its dependent objects are relocated to
 * the new schema.
 *
 * 把表或物化视图搬到另一个命名空间的核心：除了移动关系本身，它的依赖对象也搬到新模式。
 */
void
AlterTableNamespaceInternal(Relation rel, Oid oldNspOid, Oid nspOid,
							ObjectAddresses *objsMoved)
{
	Relation	classRel;

	Assert(objsMoved != NULL);

	/* OK, modify the pg_class row and pg_depend entry */
	/*
	 *
	 * 好，修改 pg_class 行和 pg_depend 项
	 */
	classRel = table_open(RelationRelationId, RowExclusiveLock);

	AlterRelationNamespaceInternal(classRel, RelationGetRelid(rel), oldNspOid,
								   nspOid, true, objsMoved);

	/* Fix the table's row type too, if it has one */
	/*
	 *
	 * 若表有行类型，也修正它
	 */
	if (OidIsValid(rel->rd_rel->reltype))
		AlterTypeNamespaceInternal(rel->rd_rel->reltype, nspOid,
								   false,	/* isImplicitArray */
										/*
										 *
										 * isImplicitArray 标志
										 */
								   false,	/* ignoreDependent */
										/*
										 *
										 * ignoreDependent 标志
										 */
								   false,	/* errorOnTableType */
										/*
										 *
										 * errorOnTableType 标志
										 */
								   objsMoved);

	/* Fix other dependent stuff */
	/*
	 *
	 * 修正其他依赖的东西
	 */
	AlterIndexNamespaces(classRel, rel, oldNspOid, nspOid, objsMoved);
	AlterSeqNamespaces(classRel, rel, oldNspOid, nspOid,
					   objsMoved, AccessExclusiveLock);
	AlterConstraintNamespaces(RelationGetRelid(rel), oldNspOid, nspOid,
							  false, objsMoved);

	table_close(classRel, RowExclusiveLock);
}

/*
 * The guts of relocating a relation to another namespace: fix the pg_class
 * entry, and the pg_depend entry if any.  Caller must already have
 * opened and write-locked pg_class.
 *
 * 把关系搬到另一个命名空间的核心：修正 pg_class 项，以及若有的话修正 pg_depend 项。调用方必须已经打开并写锁
 * pg_class。
 */
void
AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry,
							   ObjectAddresses *objsMoved)
{
	HeapTuple	classTup;
	Form_pg_class classForm;
	ObjectAddress thisobj;
	bool		already_done = false;

	/* no rel lock for relkind=c so use LOCKTAG_TUPLE */
	/*
	 *
	 * relkind 为 c 时没有关系锁，所以用 LOCKTAG_TUPLE
	 */
	classTup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(classTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	Assert(classForm->relnamespace == oldNspOid);

	thisobj.classId = RelationRelationId;
	thisobj.objectId = relOid;
	thisobj.objectSubId = 0;

	/*
	 * If the object has already been moved, don't move it again.  If it's
	 * already in the right place, don't move it, but still fire the object
	 * access hook.
	 *
	 * 若对象已经被移动，不要再移一次。若它已经在正确的地方，就不要移动，但仍要触发对象访问钩子。
	 */
	already_done = object_address_present(&thisobj, objsMoved);
	if (!already_done && oldNspOid != newNspOid)
	{
		ItemPointerData otid = classTup->t_self;

		/* check for duplicate name (more friendly than unique-index failure) */
		/*
		 *
		 * 检查重名（比唯一索引失败更友好）
		 */
		if (get_relname_relid(NameStr(classForm->relname),
							  newNspOid) != InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists in schema \"%s\"",
							NameStr(classForm->relname),
							get_namespace_name(newNspOid))));

		/* classTup is a copy, so OK to scribble on */
		/*
		 *
		 * classTup 是副本，所以可以涂改
		 */
		classForm->relnamespace = newNspOid;

		CatalogTupleUpdate(classRel, &otid, classTup);
		UnlockTuple(classRel, &otid, InplaceUpdateTupleLock);


		/* Update dependency on schema if caller said so */
		/*
		 *
		 * 若调用方这么说，就更新对模式的依赖
		 */
		if (hasDependEntry &&
			changeDependencyFor(RelationRelationId,
								relOid,
								NamespaceRelationId,
								oldNspOid,
								newNspOid) != 1)
			elog(ERROR, "could not change schema dependency for relation \"%s\"",
				 NameStr(classForm->relname));
	}
	else
		UnlockTuple(classRel, &classTup->t_self, InplaceUpdateTupleLock);
	if (!already_done)
	{
		add_exact_object_address(&thisobj, objsMoved);

		InvokeObjectPostAlterHook(RelationRelationId, relOid, 0);
	}

	heap_freetuple(classTup);
}

/*
 * Move all indexes for the specified relation to another namespace.
 *
 * 把指定关系的所有索引移到另一个命名空间。
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 *
 * 注意：我们假定调用方已经做了充分的权限检查，并且对所属关系持有合适的锁。
 */
static void
AlterIndexNamespaces(Relation classRel, Relation rel,
					 Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved)
{
	List	   *indexList;
	ListCell   *l;

	indexList = RelationGetIndexList(rel);

	foreach(l, indexList)
	{
		Oid			indexOid = lfirst_oid(l);
		ObjectAddress thisobj;

		thisobj.classId = RelationRelationId;
		thisobj.objectId = indexOid;
		thisobj.objectSubId = 0;

		/*
		 * Note: currently, the index will not have its own dependency on the
		 * namespace, so we don't need to do changeDependencyFor(). There's no
		 * row type in pg_type, either.
		 *
		 * 注意：目前索引不会有自己对命名空间的依赖，所以不必做 changeDependencyFor()。pg_type 里也没有行类型。
		 *
		 * XXX this objsMoved test may be pointless -- surely we have a single
		 * dependency link from a relation to each index?
		 *
		 * XXX 这个 objsMoved 测试也许没意义，一个关系到每个索引肯定只有一条依赖链接吧？
		 */
		if (!object_address_present(&thisobj, objsMoved))
		{
			AlterRelationNamespaceInternal(classRel, indexOid,
										   oldNspOid, newNspOid,
										   false, objsMoved);
			add_exact_object_address(&thisobj, objsMoved);
		}
	}

	list_free(indexList);
}

/*
 * Move all identity and SERIAL-column sequences of the specified relation to another
 * namespace.
 *
 * 把指定关系的所有标识列和 SERIAL 列序列移到另一个命名空间。
 *
 * Note: we assume adequate permission checking was done by the caller,
 * and that the caller has a suitable lock on the owning relation.
 *
 * 注意：我们假定调用方已经做了充分的权限检查，并且对所属关系持有合适的锁。
 */
static void
AlterSeqNamespaces(Relation classRel, Relation rel,
				   Oid oldNspOid, Oid newNspOid, ObjectAddresses *objsMoved,
				   LOCKMODE lockmode)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	/*
	 * SERIAL sequences are those having an auto dependency on one of the
	 * table's columns (we don't care *which* column, exactly).
	 *
	 * SERIAL 序列是对表的某一列有 auto 依赖的那些（我们并不在乎具体是哪一列）。
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	/* we leave refobjsubid unspecified */
	/*
	 *
	 * 我们不指定 refobjsubid
	 */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* skip dependencies other than auto dependencies on columns */
		/*
		 *
		 * 跳过不是对列的 auto 依赖的那些依赖
		 */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			!(depForm->deptype == DEPENDENCY_AUTO || depForm->deptype == DEPENDENCY_INTERNAL))
			continue;

		/* Use relation_open just in case it's an index */
		/*
		 *
		 * 用 relation_open，以防万一它是索引
		 */
		seqRel = relation_open(depForm->objid, lockmode);

		/* skip non-sequence relations */
		/*
		 *
		 * 跳过非序列关系
		 */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* No need to keep the lock */
			/*
			 *
			 * 不必保留这把锁
			 */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* Fix the pg_class and pg_depend entries */
		/*
		 *
		 * 修正 pg_class 和 pg_depend 项
		 */
		AlterRelationNamespaceInternal(classRel, depForm->objid,
									   oldNspOid, newNspOid,
									   true, objsMoved);

		/*
		 * Sequences used to have entries in pg_type, but no longer do.  If we
		 * ever re-instate that, we'll need to move the pg_type entry to the
		 * new namespace, too (using AlterTypeNamespaceInternal).
		 *
		 * 序列以前在 pg_type 里有项，但现在没有了。若以后恢复，也需要把 pg_type 项移到新命名空间（用
		 * AlterTypeNamespaceInternal）。
		 */
		Assert(RelationGetForm(seqRel)->reltype == InvalidOid);

		/* Now we can close it.  Keep the lock till end of transaction. */
		/*
		 *
		 * 现在可以关掉它。锁保持到事务结束。
		 */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}


/*
 * This code supports
 *	CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS | DELETE ROWS }
 *
 * 这段代码支持 CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS |
 * DELETE ROWS }
 *
 * Because we only support this for TEMP tables, it's sufficient to remember
 * the state in a backend-local data structure.
 *
 * 因为只对临时表支持这个，把状态记在后端本地的数据结构里就够了。
 */

/*
 * Register a newly-created relation's ON COMMIT action.
 *
 * 登记新建关系的 ON COMMIT 动作。
 */
void
register_on_commit_action(Oid relid, OnCommitAction action)
{
	OnCommitItem *oc;
	MemoryContext oldcxt;

	/*
	 * We needn't bother registering the relation unless there is an ON COMMIT
	 * action we need to take.
	 *
	 * 除非有需要执行的 ON COMMIT 动作，否则不必费心登记这个关系。
	 */
	if (action == ONCOMMIT_NOOP || action == ONCOMMIT_PRESERVE_ROWS)
		return;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	oc = (OnCommitItem *) palloc(sizeof(OnCommitItem));
	oc->relid = relid;
	oc->oncommit = action;
	oc->creating_subid = GetCurrentSubTransactionId();
	oc->deleting_subid = InvalidSubTransactionId;

	/*
	 * We use lcons() here so that ON COMMIT actions are processed in reverse
	 * order of registration.  That might not be essential but it seems
	 * reasonable.
	 *
	 * 这里用 lcons()，这样 ON COMMIT 动作按登记的相反顺序处理。这也许不是必需的，但看起来合理。
	 */
	on_commits = lcons(oc, on_commits);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Unregister any ON COMMIT action when a relation is deleted.
 *
 * 关系被删除时，注销任何 ON COMMIT 动作。
 *
 * Actually, we only mark the OnCommitItem entry as to be deleted after commit.
 *
 * 实际上，我们只是把 OnCommitItem 项标成提交后删除。
 */
void
remove_on_commit_action(Oid relid)
{
	ListCell   *l;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		if (oc->relid == relid)
		{
			oc->deleting_subid = GetCurrentSubTransactionId();
			break;
		}
	}
}

/*
 * Perform ON COMMIT actions.
 *
 * 执行 ON COMMIT 动作。
 *
 * This is invoked just before actually committing, since it's possible
 * to encounter errors.
 *
 * 这在真正提交之前调用，因为有可能遇到错误。
 */
void
PreCommit_on_commit_actions(void)
{
	ListCell   *l;
	List	   *oids_to_truncate = NIL;
	List	   *oids_to_drop = NIL;

	foreach(l, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(l);

		/* Ignore entry if already dropped in this xact */
		/*
		 *
		 * 若本事务里已经删除，则忽略该项
		 */
		if (oc->deleting_subid != InvalidSubTransactionId)
			continue;

		switch (oc->oncommit)
		{
			case ONCOMMIT_NOOP:
			case ONCOMMIT_PRESERVE_ROWS:
				/* Do nothing (there shouldn't be such entries, actually) */
				/*
				 *
				 * 什么也不做（其实不该有这样的项）
				 */
				break;
			case ONCOMMIT_DELETE_ROWS:

				/*
				 * If this transaction hasn't accessed any temporary
				 * relations, we can skip truncating ON COMMIT DELETE ROWS
				 * tables, as they must still be empty.
				 *
				 * 若本事务没有访问过任何临时关系，可以跳过截断 ON COMMIT DELETE ROWS 的表，因为它们必然仍是空的。
				 */
				if ((MyXactFlags & XACT_FLAGS_ACCESSEDTEMPNAMESPACE))
					oids_to_truncate = lappend_oid(oids_to_truncate, oc->relid);
				break;
			case ONCOMMIT_DROP:
				oids_to_drop = lappend_oid(oids_to_drop, oc->relid);
				break;
		}
	}

	/*
	 * Truncate relations before dropping so that all dependencies between
	 * relations are removed after they are worked on.  Doing it like this
	 * might be a waste as it is possible that a relation being truncated will
	 * be dropped anyway due to its parent being dropped, but this makes the
	 * code more robust because of not having to re-check that the relation
	 * exists at truncation time.
	 *
	 * 先截断关系再删除，这样关系之间的所有依赖会在处理完之后才去掉。这样做可能有点浪费，因为被截断的关系可能因其父表被删除而反正会被删掉，
	 * 但这样代码更稳健，不必在截断时重新检查关系是否存在。
	 */
	if (oids_to_truncate != NIL)
		heap_truncate(oids_to_truncate);

	if (oids_to_drop != NIL)
	{
		ObjectAddresses *targetObjects = new_object_addresses();

		foreach(l, oids_to_drop)
		{
			ObjectAddress object;

			object.classId = RelationRelationId;
			object.objectId = lfirst_oid(l);
			object.objectSubId = 0;

			Assert(!object_address_present(&object, targetObjects));

			add_exact_object_address(&object, targetObjects);
		}

		/*
		 * Object deletion might involve toast table access (to clean up
		 * toasted catalog entries), so ensure we have a valid snapshot.
		 *
		 * 对象删除可能访问 TOAST 表（清理被 toast 的目录项），所以确保我们有有效的快照。
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Since this is an automatic drop, rather than one directly initiated
		 * by the user, we pass the PERFORM_DELETION_INTERNAL flag.
		 *
		 * 因为这是自动删除，而不是用户直接发起的，我们传入 PERFORM_DELETION_INTERNAL 标志。
		 */
		performMultipleDeletions(targetObjects, DROP_CASCADE,
								 PERFORM_DELETION_INTERNAL | PERFORM_DELETION_QUIETLY);

		PopActiveSnapshot();

#ifdef USE_ASSERT_CHECKING

		/*
		 * Note that table deletion will call remove_on_commit_action, so the
		 * entry should get marked as deleted.
		 *
		 * 注意表删除会调用 remove_on_commit_action，所以该项应被标成已删除。
		 */
		foreach(l, on_commits)
		{
			OnCommitItem *oc = (OnCommitItem *) lfirst(l);

			if (oc->oncommit != ONCOMMIT_DROP)
				continue;

			Assert(oc->deleting_subid != InvalidSubTransactionId);
		}
#endif
	}
}

/*
 * Post-commit or post-abort cleanup for ON COMMIT management.
 *
 * ON COMMIT 管理的提交后或中止后清理。
 *
 * All we do here is remove no-longer-needed OnCommitItem entries.
 *
 * 这里只是去掉不再需要的 OnCommitItem 项。
 *
 * During commit, remove entries that were deleted during this transaction;
 * during abort, remove those created during this transaction.
 *
 * 提交时，去掉本事务期间被删除的项；中止时，去掉本事务期间创建的项。
 */
void
AtEOXact_on_commit_actions(bool isCommit)
{
	ListCell   *cur_item;

	foreach(cur_item, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (isCommit ? oc->deleting_subid != InvalidSubTransactionId :
			oc->creating_subid != InvalidSubTransactionId)
		{
			/* cur_item must be removed */
			/*
			 *
			 * 当前项必须去掉
			 */
			on_commits = foreach_delete_current(on_commits, cur_item);
			pfree(oc);
		}
		else
		{
			/* cur_item must be preserved */
			/*
			 *
			 * 当前项必须保留
			 */
			oc->creating_subid = InvalidSubTransactionId;
			oc->deleting_subid = InvalidSubTransactionId;
		}
	}
}

/*
 * Post-subcommit or post-subabort cleanup for ON COMMIT management.
 *
 * ON COMMIT 管理的子提交后或子中止后清理。
 *
 * During subabort, we can immediately remove entries created during this
 * subtransaction.  During subcommit, just relabel entries marked during
 * this subtransaction as being the parent's responsibility.
 *
 * 子中止时，可以立刻去掉本子事务期间创建的项。子提交时，只是把本子事务期间标记的项改标成由父事务负责。
 */
void
AtEOSubXact_on_commit_actions(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid)
{
	ListCell   *cur_item;

	foreach(cur_item, on_commits)
	{
		OnCommitItem *oc = (OnCommitItem *) lfirst(cur_item);

		if (!isCommit && oc->creating_subid == mySubid)
		{
			/* cur_item must be removed */
			/*
			 *
			 * 当前项必须去掉
			 */
			on_commits = foreach_delete_current(on_commits, cur_item);
			pfree(oc);
		}
		else
		{
			/* cur_item must be preserved */
			/*
			 *
			 * 当前项必须保留
			 */
			if (oc->creating_subid == mySubid)
				oc->creating_subid = parentSubid;
			if (oc->deleting_subid == mySubid)
				oc->deleting_subid = isCommit ? parentSubid : InvalidSubTransactionId;
		}
	}
}

/*
 * This is intended as a callback for RangeVarGetRelidExtended().  It allows
 * the relation to be locked only if (1) it's a plain or partitioned table,
 * materialized view, or TOAST table and (2) the current user is the owner (or
 * the superuser) or has been granted MAINTAIN.  This meets the
 * permission-checking needs of CLUSTER, REINDEX TABLE, and REFRESH
 * MATERIALIZED VIEW; we expose it here so that it can be used by all.
 *
 * 这打算作为 RangeVarGetRelidExtended() 的回调。只有当 (1) 它是普通表或分区表、物化视图或
 * TOAST 表，并且 (2) 当前用户是属主（或超级用户）或被授予了 MAINTAIN 时，才允许锁住该关系。这满足
 * CLUSTER、REINDEX TABLE 和 REFRESH MATERIALIZED VIEW 的权限检查需要；
 * 放在这里是为了大家都能用。
 */
void
RangeVarCallbackMaintainsTable(const RangeVar *relation,
							   Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;
	AclResult	aclresult;

	/* Nothing to do if the relation was not found. */
	/*
	 *
	 * 若没找到关系，则无事可做。
	 */
	if (!OidIsValid(relId))
		return;

	/*
	 * If the relation does exist, check whether it's an index.  But note that
	 * the relation might have been dropped between the time we did the name
	 * lookup and now.  In that case, there's nothing to do.
	 *
	 * 若关系确实存在，检查它是不是索引。但注意在我们做名字查找和现在之间，关系可能已经被删除。那种情况下无事可做。
	 */
	relkind = get_rel_relkind(relId);
	if (!relkind)
		return;
	if (relkind != RELKIND_RELATION && relkind != RELKIND_TOASTVALUE &&
		relkind != RELKIND_MATVIEW && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or materialized view", relation->relname)));

	/* Check permissions */
	/*
	 *
	 * 检查权限
	 */
	aclresult = pg_class_aclcheck(relId, GetUserId(), ACL_MAINTAIN);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult,
					   get_relkind_objtype(get_rel_relkind(relId)),
					   relation->relname);
}

/*
 * Callback to RangeVarGetRelidExtended() for TRUNCATE processing.
 *
 * 供 TRUNCATE 处理使用的 RangeVarGetRelidExtended() 回调。
 */
static void
RangeVarCallbackForTruncate(const RangeVar *relation,
							Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/* Nothing to do if the relation was not found. */
	/*
	 *
	 * 若没找到关系，则无事可做。
	 */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))	/* should not happen */
					/*
					 *
					 * 不应发生
					 */
		elog(ERROR, "cache lookup failed for relation %u", relId);

	truncate_check_rel(relId, (Form_pg_class) GETSTRUCT(tuple));
	truncate_check_perms(relId, (Form_pg_class) GETSTRUCT(tuple));

	ReleaseSysCache(tuple);
}

/*
 * Callback for RangeVarGetRelidExtended().  Checks that the current user is
 * the owner of the relation, or superuser.
 *
 * RangeVarGetRelidExtended() 的回调。检查当前用户是该关系的属主，或是超级用户。
 */
void
RangeVarCallbackOwnsRelation(const RangeVar *relation,
							 Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/* Nothing to do if the relation was not found. */
	/*
	 *
	 * 若没找到关系，则无事可做。
	 */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))	/* should not happen */
					/*
					 *
					 * 不应发生
					 */
		elog(ERROR, "cache lookup failed for relation %u", relId);

	if (!object_ownercheck(RelationRelationId, relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relId)),
					   relation->relname);

	if (!allowSystemTableMods &&
		IsSystemClass(relId, (Form_pg_class) GETSTRUCT(tuple)))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						relation->relname)));

	ReleaseSysCache(tuple);
}

/*
 * Common RangeVarGetRelid callback for rename, set schema, and alter table
 * processing.
 *
 * 供改名、设置模式和 ALTER TABLE 处理共用的 RangeVarGetRelid 回调。
 */
static void
RangeVarCallbackForAlterRelation(const RangeVar *rv, Oid relid, Oid oldrelid,
								 void *arg)
{
	Node	   *stmt = (Node *) arg;
	ObjectType	reltype;
	HeapTuple	tuple;
	Form_pg_class classform;
	AclResult	aclresult;
	char		relkind;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped */
							/*
							 *
							 * 已被并发删除
							 */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/* Must own relation. */
	/*
	 *
	 * 必须拥有该关系。
	 */
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);

	/* No system table modifications unless explicitly allowed. */
	/*
	 *
	 * 除非明确允许，否则不能修改系统表。
	 */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/*
	 * Extract the specified relation type from the statement parse tree.
	 *
	 * 从语句语法树里取出指定的关系类型。
	 *
	 * Also, for ALTER .. RENAME, check permissions: the user must (still)
	 * have CREATE rights on the containing namespace.
	 *
	 * 另外，对 ALTER .. RENAME，检查权限：用户必须（仍然）对所在命名空间有 CREATE 权限。
	 */
	if (IsA(stmt, RenameStmt))
	{
		aclresult = object_aclcheck(NamespaceRelationId, classform->relnamespace,
									GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(classform->relnamespace));
		reltype = ((RenameStmt *) stmt)->renameType;
	}
	else if (IsA(stmt, AlterObjectSchemaStmt))
		reltype = ((AlterObjectSchemaStmt *) stmt)->objectType;

	else if (IsA(stmt, AlterTableStmt))
		reltype = ((AlterTableStmt *) stmt)->objtype;
	else
	{
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(stmt));
		reltype = OBJECT_TABLE; /* placate compiler */
					/*
					 *
					 * 安抚编译器
					 */
	}

	/*
	 * For compatibility with prior releases, we allow ALTER TABLE to be used
	 * with most other types of relations (but not composite types). We allow
	 * similar flexibility for ALTER INDEX in the case of RENAME, but not
	 * otherwise.  Otherwise, the user must select the correct form of the
	 * command for the relation at issue.
	 *
	 * 为了与先前版本兼容，我们允许 ALTER TABLE 用于大多数其他类型的关系（但不包括复合类型）。对 ALTER INDEX，在
	 * RENAME 的情况下允许类似的灵活性，其他情况则不允许。除此之外，用户必须为所涉关系选择正确的命令形式。
	 */
	if (reltype == OBJECT_SEQUENCE && relkind != RELKIND_SEQUENCE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sequence", rv->relname)));

	if (reltype == OBJECT_VIEW && relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a view", rv->relname)));

	if (reltype == OBJECT_MATVIEW && relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a materialized view", rv->relname)));

	if (reltype == OBJECT_FOREIGN_TABLE && relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a foreign table", rv->relname)));

	if (reltype == OBJECT_TYPE && relkind != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a composite type", rv->relname)));

	if (reltype == OBJECT_INDEX && relkind != RELKIND_INDEX &&
		relkind != RELKIND_PARTITIONED_INDEX
		&& !IsA(stmt, RenameStmt))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", rv->relname)));

	/*
	 * Don't allow ALTER TABLE on composite types. We want people to use ALTER
	 * TYPE for that.
	 *
	 * 不允许对复合类型使用 ALTER TABLE。我们希望人们用 ALTER TYPE。
	 */
	if (reltype != OBJECT_TYPE && relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type", rv->relname),
		/* translator: %s is an SQL ALTER command */
		/*
		 *
		 * 翻译提示：%s 是一条 SQL ALTER 命令
		 */
				 errhint("Use %s instead.",
						 "ALTER TYPE")));

	/*
	 * Don't allow ALTER TABLE .. SET SCHEMA on relations that can't be moved
	 * to a different schema, such as indexes and TOAST tables.
	 *
	 * 不允许对不能移到不同模式的关系（例如索引和 TOAST 表）使用 ALTER TABLE .. SET SCHEMA。
	 */
	if (IsA(stmt, AlterObjectSchemaStmt))
	{
		if (relkind == RELKIND_INDEX || relkind == RELKIND_PARTITIONED_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change schema of index \"%s\"",
							rv->relname),
					 errhint("Change the schema of the table instead.")));
		else if (relkind == RELKIND_COMPOSITE_TYPE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change schema of composite type \"%s\"",
							rv->relname),
			/* translator: %s is an SQL ALTER command */
			/*
			 *
			 * 翻译提示：%s 是一条 SQL ALTER 命令
			 */
					 errhint("Use %s instead.",
							 "ALTER TYPE")));
		else if (relkind == RELKIND_TOASTVALUE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change schema of TOAST table \"%s\"",
							rv->relname),
					 errhint("Change the schema of the table instead.")));
	}

	ReleaseSysCache(tuple);
}

/*
 * Transform any expressions present in the partition key
 *
 * 变换分区键里出现的任何表达式
 *
 * Returns a transformed PartitionSpec.
 *
 * 返回变换后的 PartitionSpec。
 */
static PartitionSpec *
transformPartitionSpec(Relation rel, PartitionSpec *partspec)
{
	PartitionSpec *newspec;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	ListCell   *l;

	newspec = makeNode(PartitionSpec);

	newspec->strategy = partspec->strategy;
	newspec->partParams = NIL;
	newspec->location = partspec->location;

	/* Check valid number of columns for strategy */
	/*
	 *
	 * 检查该策略的列数是否合法
	 */
	if (partspec->strategy == PARTITION_STRATEGY_LIST &&
		list_length(partspec->partParams) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot use \"list\" partition strategy with more than one column")));

	/*
	 * Create a dummy ParseState and insert the target relation as its sole
	 * rangetable entry.  We need a ParseState for transformExpr.
	 *
	 * 造一个假的 ParseState，并把目标关系作为它唯一的 rangetable 项插进去。transformExpr 需要
	 * ParseState。
	 */
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/* take care of any partition expressions */
	/*
	 *
	 * 处理所有分区表达式
	 */
	foreach(l, partspec->partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, l);

		if (pelem->expr)
		{
			/* Copy, to avoid scribbling on the input */
			/*
			 *
			 * 拷贝一份，以免涂改输入
			 */
			pelem = copyObject(pelem);

			/* Now do parse transformation of the expression */
			/*
			 *
			 * 现在对表达式做解析变换
			 */
			pelem->expr = transformExpr(pstate, pelem->expr,
										EXPR_KIND_PARTITION_EXPRESSION);

			/* we have to fix its collations too */
			/*
			 *
			 * 也必须修正它的排序规则
			 */
			assign_expr_collations(pstate, pelem->expr);
		}

		newspec->partParams = lappend(newspec->partParams, pelem);
	}

	return newspec;
}

/*
 * Compute per-partition-column information from a list of PartitionElems.
 * Expressions in the PartitionElems must be parse-analyzed already.
 *
 * 从 PartitionElem 列表计算每个分区列的信息。PartitionElem 里的表达式必须已经解析分析过。
 */
static void
ComputePartitionAttrs(ParseState *pstate, Relation rel, List *partParams, AttrNumber *partattrs,
					  List **partexprs, Oid *partopclass, Oid *partcollation,
					  PartitionStrategy strategy)
{
	int			attn;
	ListCell   *lc;
	Oid			am_oid;

	attn = 0;
	foreach(lc, partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, lc);
		Oid			atttype;
		Oid			attcollation;

		if (pelem->name != NULL)
		{
			/* Simple attribute reference */
			/*
			 *
			 * 简单的属性引用
			 */
			HeapTuple	atttuple;
			Form_pg_attribute attform;

			atttuple = SearchSysCacheAttName(RelationGetRelid(rel),
											 pelem->name);
			if (!HeapTupleIsValid(atttuple))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" named in partition key does not exist",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));
			attform = (Form_pg_attribute) GETSTRUCT(atttuple);

			if (attform->attnum <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use system column \"%s\" in partition key",
								pelem->name),
						 parser_errposition(pstate, pelem->location)));

			/*
			 * Stored generated columns cannot work: They are computed after
			 * BEFORE triggers, but partition routing is done before all
			 * triggers.  Maybe virtual generated columns could be made to
			 * work, but then they would need to be handled as an expression
			 * below.
			 *
			 * 存储生成列不行：它们在 BEFORE 触发器之后才计算，但分区路由在所有触发器之前完成。虚拟生成列也许能弄成可用，
			 * 但那样就要在下面按表达式来处理。
			 */
			if (attform->attgenerated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot use generated column in partition key"),
						 errdetail("Column \"%s\" is a generated column.",
								   pelem->name),
						 parser_errposition(pstate, pelem->location)));

			partattrs[attn] = attform->attnum;
			atttype = attform->atttypid;
			attcollation = attform->attcollation;
			ReleaseSysCache(atttuple);
		}
		else
		{
			/* Expression */
			/*
			 *
			 * 表达式
			 */
			Node	   *expr = pelem->expr;
			char		partattname[16];
			Bitmapset  *expr_attrs = NULL;
			int			i;

			Assert(expr != NULL);
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 * The expression must be of a storable type (e.g., not RECORD).
			 * The test is the same as for whether a table column is of a safe
			 * type (which is why we needn't check for the non-expression
			 * case).
			 *
			 * 表达式必须是可存储的类型（例如不能是 RECORD）。测试与表列是否为安全类型的测试相同（所以非表达式的情况不必再查）。
			 */
			snprintf(partattname, sizeof(partattname), "%d", attn + 1);
			CheckAttributeType(partattname,
							   atttype, attcollation,
							   NIL, CHKATYPE_IS_PARTKEY);

			/*
			 * Strip any top-level COLLATE clause.  This ensures that we treat
			 * "x COLLATE y" and "(x COLLATE y)" alike.
			 *
			 * 剥掉顶层的 COLLATE 子句。这样 x COLLATE y 和 (x COLLATE y) 会被同样对待。
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			/*
			 * Examine all the columns in the partition key expression. When
			 * the whole-row reference is present, examine all the columns of
			 * the partitioned table.
			 *
			 * 检查分区键表达式里的所有列。若出现整行引用，就检查分区表的所有列。
			 */
			pull_varattnos(expr, 1, &expr_attrs);
			if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, expr_attrs))
			{
				expr_attrs = bms_add_range(expr_attrs,
										   1 - FirstLowInvalidHeapAttributeNumber,
										   RelationGetNumberOfAttributes(rel) - FirstLowInvalidHeapAttributeNumber);
				expr_attrs = bms_del_member(expr_attrs, 0 - FirstLowInvalidHeapAttributeNumber);
			}

			i = -1;
			while ((i = bms_next_member(expr_attrs, i)) >= 0)
			{
				AttrNumber	attno = i + FirstLowInvalidHeapAttributeNumber;

				Assert(attno != 0);

				/*
				 * Cannot allow system column references, since that would
				 * make partition routing impossible: their values won't be
				 * known yet when we need to do that.
				 *
				 * 不能允许系统列引用，因为那会使分区路由不可能：需要路由时它们的值还不知道。
				 */
				if (attno < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("partition key expressions cannot contain system column references")));

				/*
				 * Stored generated columns cannot work: They are computed
				 * after BEFORE triggers, but partition routing is done before
				 * all triggers.  Virtual generated columns could probably
				 * work, but it would require more work elsewhere (for example
				 * SET EXPRESSION would need to check whether the column is
				 * used in partition keys).  Seems safer to prohibit for now.
				 *
				 * 存储生成列不行：它们在 BEFORE 触发器之后才计算，但分区路由在所有触发器之前完成。虚拟生成列大概能用，
				 * 但别处还要更多工作（例如 SET EXPRESSION 需要检查该列是否用在分区键里）。目前禁止看起来更安全。
				 */
				if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use generated column in partition key"),
							 errdetail("Column \"%s\" is a generated column.",
									   get_attname(RelationGetRelid(rel), attno, false)),
							 parser_errposition(pstate, pelem->location)));
			}

			if (IsA(expr, Var) &&
				((Var *) expr)->varattno > 0)
			{

				/*
				 * User wrote "(column)" or "(column COLLATE something)".
				 * Treat it like simple attribute anyway.
				 *
				 * 用户写了 (column) 或 (column COLLATE something)。不管怎样都按简单属性对待。
				 */
				partattrs[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				partattrs[attn] = 0;	/* marks the column as expression */
							/*
							 *
							 * 把该列标成表达式
							 */
				*partexprs = lappend(*partexprs, expr);

				/*
				 * transformPartitionSpec() should have already rejected
				 * subqueries, aggregates, window functions, and SRFs, based
				 * on the EXPR_KIND_ for partition expressions.
				 *
				 * transformPartitionSpec() 本应根据分区表达式的 EXPR_KIND_ 已经拒绝了子查询、聚合、窗口函数和
				 * SRF。
				 */

				/*
				 * Preprocess the expression before checking for mutability.
				 * This is essential for the reasons described in
				 * contain_mutable_functions_after_planning.  However, we call
				 * expression_planner for ourselves rather than using that
				 * function, because if constant-folding reduces the
				 * expression to a constant, we'd like to know that so we can
				 * complain below.
				 *
				 * 检查可变性之前先预处理表达式。原因与 contain_mutable_functions_after_planning 所述相同，
				 * 这是必要的。不过我们自己调用 expression_planner，而不是用那个函数，因为若常量折叠把表达式化成常量，
				 * 我们希望知道这一点，以便在下面抱怨。
				 *
				 * Like contain_mutable_functions_after_planning, assume that
				 * expression_planner won't scribble on its input, so this
				 * won't affect the partexprs entry we saved above.
				 *
				 * 和 contain_mutable_functions_after_planning 一样，假定
				 * expression_planner 不会涂改其输入，所以不会影响上面保存的 partexprs 项。
				 */
				expr = (Node *) expression_planner((Expr *) expr);

				/*
				 * Partition expressions cannot contain mutable functions,
				 * because a given row must always map to the same partition
				 * as long as there is no change in the partition boundary
				 * structure.
				 *
				 * 分区表达式不能包含可变函数，因为只要分区边界结构不变，给定的行必须始终映射到同一个分区。
				 */
				if (contain_mutable_functions(expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in partition key expression must be marked IMMUTABLE")));

				/*
				 * While it is not exactly *wrong* for a partition expression
				 * to be a constant, it seems better to reject such keys.
				 *
				 * 分区表达式是常量虽然并不完全错误，但拒绝这种键似乎更好。
				 */
				if (IsA(expr, Const))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use constant expression as partition key")));
			}
		}

		/*
		 * Apply collation override if any
		 *
		 * 若有排序规则覆盖，就应用它
		 */
		if (pelem->collation)
			attcollation = get_collation_oid(pelem->collation, false);

		/*
		 * Check we have a collation iff it's a collatable type.  The only
		 * expected failures here are (1) COLLATE applied to a noncollatable
		 * type, or (2) partition expression had an unresolved collation. But
		 * we might as well code this to be a complete consistency check.
		 *
		 * 当且仅当类型可排序时，我们才应有排序规则。这里唯一预期的失败是 (1) 对不可排序类型使用了 COLLATE，或 (2)
		 * 分区表达式的排序规则未能解析。不过我们不妨把这段写成完整的一致性检查。
		 */
		if (type_is_collatable(atttype))
		{
			if (!OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for partition expression"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}
		else
		{
			if (OidIsValid(attcollation))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("collations are not supported by type %s",
								format_type_be(atttype))));
		}

		partcollation[attn] = attcollation;

		/*
		 * Identify the appropriate operator class.  For list and range
		 * partitioning, we use a btree operator class; hash partitioning uses
		 * a hash operator class.
		 *
		 * 确定合适的操作符类。列表和范围分区用 btree 操作符类；哈希分区用 hash 操作符类。
		 */
		if (strategy == PARTITION_STRATEGY_HASH)
			am_oid = HASH_AM_OID;
		else
			am_oid = BTREE_AM_OID;

		if (!pelem->opclass)
		{
			partopclass[attn] = GetDefaultOpClass(atttype, am_oid);

			if (!OidIsValid(partopclass[attn]))
			{
				if (strategy == PARTITION_STRATEGY_HASH)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default operator class for access method \"%s\"",
									format_type_be(atttype), "hash"),
							 errhint("You must specify a hash operator class or define a default hash operator class for the data type.")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("data type %s has no default operator class for access method \"%s\"",
									format_type_be(atttype), "btree"),
							 errhint("You must specify a btree operator class or define a default btree operator class for the data type.")));
			}
		}
		else
			partopclass[attn] = ResolveOpClass(pelem->opclass,
											   atttype,
											   am_oid == HASH_AM_OID ? "hash" : "btree",
											   am_oid);

		attn++;
	}
}

/*
 * PartConstraintImpliedByRelConstraint
 *		Do scanrel's existing constraints imply the partition constraint?
 *
 * PartConstraintImpliedByRelConstraint：scanrel 上已有的约束是否蕴含分区约束？
 *
 * "Existing constraints" include its check constraints and column-level
 * not-null constraints.  partConstraint describes the partition constraint,
 * in implicit-AND form.
 *
 * 已有约束包括它的 CHECK 约束和列级 NOT NULL 约束。partConstraint 以隐式 AND 形式描述分区约束。
 */
bool
PartConstraintImpliedByRelConstraint(Relation scanrel,
									 List *partConstraint)
{
	List	   *existConstraint = NIL;
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			i;

	if (constr && constr->has_not_null)
	{
		int			natts = scanrel->rd_att->natts;

		for (i = 1; i <= natts; i++)
		{
			CompactAttribute *att = TupleDescCompactAttr(scanrel->rd_att, i - 1);

			/* invalid not-null constraint must be ignored here */
			/*
			 *
			 * 这里必须忽略无效的 NOT NULL 约束
			 */
			if (att->attnullability == ATTNULLABLE_VALID && !att->attisdropped)
			{
				Form_pg_attribute wholeatt = TupleDescAttr(scanrel->rd_att, i - 1);
				NullTest   *ntest = makeNode(NullTest);

				ntest->arg = (Expr *) makeVar(1,
											  i,
											  wholeatt->atttypid,
											  wholeatt->atttypmod,
											  wholeatt->attcollation,
											  0);
				ntest->nulltesttype = IS_NOT_NULL;

				/*
				 * argisrow=false is correct even for a composite column,
				 * because attnotnull does not represent a SQL-spec IS NOT
				 * NULL test in such a case, just IS DISTINCT FROM NULL.
				 *
				 * 即使是复合列，argisrow 为假也是对的，因为这种情况下 attnotnull 并不表示 SQL 标准的 IS NOT
				 * NULL 测试，只是 IS DISTINCT FROM NULL。
				 */
				ntest->argisrow = false;
				ntest->location = -1;
				existConstraint = lappend(existConstraint, ntest);
			}
		}
	}

	return ConstraintImpliedByRelConstraint(scanrel, partConstraint, existConstraint);
}

/*
 * ConstraintImpliedByRelConstraint
 *		Do scanrel's existing constraints imply the given constraint?
 *
 * ConstraintImpliedByRelConstraint：scanrel 上已有的约束是否蕴含给定约束？
 *
 * testConstraint is the constraint to validate. provenConstraint is a
 * caller-provided list of conditions which this function may assume
 * to be true. Both provenConstraint and testConstraint must be in
 * implicit-AND form, must only contain immutable clauses, and must
 * contain only Vars with varno = 1.
 *
 * testConstraint 是要验证的约束。provenConstraint 是调用方提供的、本函数可以假定为真的条件列表。
 * provenConstraint 和 testConstraint 都必须是隐式 AND 形式，只能包含不可变子句，并且只能包含
 * varno 为 1 的 Var。
 */
bool
ConstraintImpliedByRelConstraint(Relation scanrel, List *testConstraint, List *provenConstraint)
{
	List	   *existConstraint = list_copy(provenConstraint);
	TupleConstr *constr = RelationGetDescr(scanrel)->constr;
	int			num_check,
				i;

	num_check = (constr != NULL) ? constr->num_check : 0;
	for (i = 0; i < num_check; i++)
	{
		Node	   *cexpr;

		/*
		 * If this constraint hasn't been fully validated yet, we must ignore
		 * it here.
		 *
		 * 若这个约束还没完全验证，这里必须忽略它。
		 */
		if (!constr->check[i].ccvalid)
			continue;

		/*
		 * NOT ENFORCED constraints are always marked as invalid, which should
		 * have been ignored.
		 *
		 * NOT ENFORCED 约束总是被标成无效，本应已经被忽略。
		 */
		Assert(constr->check[i].ccenforced);

		cexpr = stringToNode(constr->check[i].ccbin);

		/*
		 * Run each expression through const-simplification and
		 * canonicalization.  It is necessary, because we will be comparing it
		 * to similarly-processed partition constraint expressions, and may
		 * fail to detect valid matches without this.
		 *
		 * 把每个表达式过一遍常量简化和规范化。这是必要的，因为我们要拿它和同样处理过的分区约束表达式比较，不做这一步可能发现不了有效的匹配。
		 */
		cexpr = eval_const_expressions(NULL, cexpr);
		cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

		existConstraint = list_concat(existConstraint,
									  make_ands_implicit((Expr *) cexpr));
	}

	/*
	 * Try to make the proof.  Since we are comparing CHECK constraints, we
	 * need to use weak implication, i.e., we assume existConstraint is
	 * not-false and try to prove the same for testConstraint.
	 *
	 * 尝试完成证明。因为比较的是 CHECK 约束，需要用弱蕴含，即假定 existConstraint 不是假，并试图证明
	 * testConstraint 同样不是假。
	 *
	 * Note that predicate_implied_by assumes its first argument is known
	 * immutable.  That should always be true for both NOT NULL and partition
	 * constraints, so we don't test it here.
	 *
	 * 注意 predicate_implied_by 假定它的第一个参数已知不可变。对 NOT NULL 和分区约束这应当总是真的，
	 * 所以这里不测试。
	 */
	return predicate_implied_by(testConstraint, existConstraint, true);
}

/*
 * QueuePartitionConstraintValidation
 *
 * 函数 QueuePartitionConstraintValidation
 *
 * Add an entry to wqueue to have the given partition constraint validated by
 * Phase 3, for the given relation, and all its children.
 *
 * 往 wqueue 加一项，让 Phase 3 验证给定关系及其所有子关系上的给定分区约束。
 *
 * We first verify whether the given constraint is implied by pre-existing
 * relation constraints; if it is, there's no need to scan the table to
 * validate, so don't queue in that case.
 *
 * 先验证给定约束是否被关系上已有的约束所蕴含；若是，就不必扫表验证，那种情况不要入队。
 */
static void
QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
								   List *partConstraint,
								   bool validate_default)
{
	/*
	 * Based on the table's existing constraints, determine whether or not we
	 * may skip scanning the table.
	 *
	 * 根据表上已有的约束，决定是否可以跳过扫表。
	 */
	if (PartConstraintImpliedByRelConstraint(scanrel, partConstraint))
	{
		if (!validate_default)
			ereport(DEBUG1,
					(errmsg_internal("partition constraint for table \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		else
			ereport(DEBUG1,
					(errmsg_internal("updated partition constraint for default partition \"%s\" is implied by existing constraints",
									 RelationGetRelationName(scanrel))));
		return;
	}

	/*
	 * Constraints proved insufficient. For plain relations, queue a
	 * validation item now; for partitioned tables, recurse to process each
	 * partition.
	 *
	 * 约束被证明不够。对普通关系，现在就排队一项验证；对分区表，递归处理每个分区。
	 */
	if (scanrel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlteredTableInfo *tab;

		/* Grab a work queue entry. */
		/*
		 *
		 * 取一项工作队列。
		 */
		tab = ATGetQueueEntry(wqueue, scanrel);
		Assert(tab->partition_constraint == NULL);
		tab->partition_constraint = (Expr *) linitial(partConstraint);
		tab->validate_default = validate_default;
	}
	else if (scanrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		PartitionDesc partdesc = RelationGetPartitionDesc(scanrel, true);
		int			i;

		for (i = 0; i < partdesc->nparts; i++)
		{
			Relation	part_rel;
			List	   *thisPartConstraint;

			/*
			 * This is the minimum lock we need to prevent deadlocks.
			 *
			 * 这是防止死锁所需的最小锁。
			 */
			part_rel = table_open(partdesc->oids[i], AccessExclusiveLock);

			/*
			 * Adjust the constraint for scanrel so that it matches this
			 * partition's attribute numbers.
			 *
			 * 调整 scanrel 的约束，使它与这个分区的属性号匹配。
			 */
			thisPartConstraint =
				map_partition_varattnos(partConstraint, 1,
										part_rel, scanrel);

			QueuePartitionConstraintValidation(wqueue, part_rel,
											   thisPartConstraint,
											   validate_default);
			table_close(part_rel, NoLock);	/* keep lock till commit */
							/*
							 *
							 * 锁保持到提交
							 */
		}
	}
}

/*
 * ALTER TABLE <name> ATTACH PARTITION <partition-name> FOR VALUES
 *
 * ALTER TABLE name ATTACH PARTITION partition-name FOR VALUES（挂接分区）
 *
 * Return the address of the newly attached partition.
 *
 * 返回新挂上的分区的地址。
 */
static ObjectAddress
ATExecAttachPartition(List **wqueue, Relation rel, PartitionCmd *cmd,
					  AlterTableUtilityContext *context)
{
	Relation	attachrel,
				catalog;
	List	   *attachrel_children;
	List	   *partConstraint;
	SysScanDesc scan;
	ScanKeyData skey;
	AttrNumber	attno;
	int			natts;
	TupleDesc	tupleDesc;
	ObjectAddress address;
	const char *trigger_name;
	Oid			defaultPartOid;
	List	   *partBoundConstraint;
	ParseState *pstate = make_parsestate(NULL);

	pstate->p_sourcetext = context->queryString;

	/*
	 * We must lock the default partition if one exists, because attaching a
	 * new partition will change its partition constraint.
	 *
	 * 若存在默认分区，必须锁住它，因为挂上新分区会改变它的分区约束。
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
		LockRelationOid(defaultPartOid, AccessExclusiveLock);

	attachrel = table_openrv(cmd->name, AccessExclusiveLock);

	/*
	 * XXX I think it'd be a good idea to grab locks on all tables referenced
	 * by FKs at this point also.
	 *
	 * XXX 我觉得这时把外键引用的所有表也锁上是个好主意。
	 */

	/*
	 * Must be owner of both parent and source table -- parent was checked by
	 * ATSimplePermissions call in ATPrepCmd
	 *
	 * 必须同时是父表和源表的属主。父表已由 ATPrepCmd 里的 ATSimplePermissions 调用检查过
	 */
	ATSimplePermissions(AT_AttachPartition, attachrel,
						ATT_TABLE | ATT_PARTITIONED_TABLE | ATT_FOREIGN_TABLE);

	/* A partition can only have one parent */
	/*
	 *
	 * 一个分区只能有一个父表
	 */
	if (attachrel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is already a partition",
						RelationGetRelationName(attachrel))));

	if (OidIsValid(attachrel->rd_rel->reloftype))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a typed table as partition")));

	/*
	 * Table being attached should not already be part of inheritance; either
	 * as a child table...
	 *
	 * 被挂接的表不应已经是继承的一部分；无论是作为子表……
	 */
	catalog = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance child as partition")));
	systable_endscan(scan);

	/* ...or as a parent table (except the case when it is partitioned) */
	/*
	 *
	 * ……还是作为父表（除非它本身是分区的）
	 */
	ScanKeyInit(&skey,
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(attachrel)));
	scan = systable_beginscan(catalog, InheritsParentIndexId, true, NULL,
							  1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan)) &&
		attachrel->rd_rel->relkind == RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach inheritance parent as partition")));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);

	/*
	 * Prevent circularity by seeing if rel is a partition of attachrel. (In
	 * particular, this disallows making a rel a partition of itself.)
	 *
	 * 看看 rel 是否是 attachrel 的分区，以防止成环。（尤其不允许关系成为自己的分区。）
	 *
	 * We do that by checking if rel is a member of the list of attachrel's
	 * partitions provided the latter is partitioned at all.  We want to avoid
	 * having to construct this list again, so we request the strongest lock
	 * on all partitions.  We need the strongest lock, because we may decide
	 * to scan them if we find out that the table being attached (or its leaf
	 * partitions) may contain rows that violate the partition constraint. If
	 * the table has a constraint that would prevent such rows, which by
	 * definition is present in all the partitions, we need not scan the
	 * table, nor its partitions.  But we cannot risk a deadlock by taking a
	 * weaker lock now and the stronger one only when needed.
	 *
	 * 做法是：若 attachrel 根本是分区的，检查 rel 是否在 attachrel 的分区列表里。我们不想再构造一次这个列表，
	 * 所以对所有分区请求最强的锁。需要最强的锁，是因为若发现被挂接的表（或其叶子分区）可能含有违反分区约束的行，我们可能决定扫描它们。
	 * 若表有能阻止这种行的约束，按定义该约束存在于所有分区中，就不必扫描表或其分区。但不能现在拿更弱的锁、需要时再拿更强的，
	 * 那样有死锁风险。
	 */
	attachrel_children = find_all_inheritors(RelationGetRelid(attachrel),
											 AccessExclusiveLock, NULL);
	if (list_member_oid(attachrel_children, RelationGetRelid(rel)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("circular inheritance not allowed"),
				 errdetail("\"%s\" is already a child of \"%s\".",
						   RelationGetRelationName(rel),
						   RelationGetRelationName(attachrel))));

	/* If the parent is permanent, so must be all of its partitions. */
	/*
	 *
	 * 若父表是永久的，它的所有分区也必须是。
	 */
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a temporary relation as partition of permanent relation \"%s\"",
						RelationGetRelationName(rel))));

	/* Temp parent cannot have a partition that is itself not a temp */
	/*
	 *
	 * 临时父表不能有一个本身不是临时的分区
	 */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a permanent relation as partition of temporary relation \"%s\"",
						RelationGetRelationName(rel))));

	/* If the parent is temp, it must belong to this session */
	/*
	 *
	 * 若父表是临时的，它必须属于本会话
	 */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach as partition of temporary relation of another session")));

	/* Ditto for the partition */
	/*
	 *
	 * 分区同样如此
	 */
	if (attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!attachrel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach temporary relation of another session as partition")));

	/*
	 * Check if attachrel has any identity columns or any columns that aren't
	 * in the parent.
	 *
	 * 检查 attachrel 是否有任何标识列，或任何父表里没有的列。
	 */
	tupleDesc = RelationGetDescr(attachrel);
	natts = tupleDesc->natts;
	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* Ignore dropped */
		/*
		 *
		 * 忽略已删除的
		 */
		if (attribute->attisdropped)
			continue;

		if (attribute->attidentity)
			ereport(ERROR,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("table \"%s\" being attached contains an identity column \"%s\"",
						   RelationGetRelationName(attachrel), attributeName),
					errdetail("The new partition may not contain an identity column."));

		/* Try to find the column in parent (matching on column name) */
		/*
		 *
		 * 尝试在父表里按列名找到该列
		 */
		if (!SearchSysCacheExists2(ATTNAME,
								   ObjectIdGetDatum(RelationGetRelid(rel)),
								   CStringGetDatum(attributeName)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" contains column \"%s\" not found in parent \"%s\"",
							RelationGetRelationName(attachrel), attributeName,
							RelationGetRelationName(rel)),
					 errdetail("The new partition may contain only the columns present in parent.")));
	}

	/*
	 * If child_rel has row-level triggers with transition tables, we
	 * currently don't allow it to become a partition.  See also prohibitions
	 * in ATExecAddInherit() and CreateTrigger().
	 *
	 * 若 child_rel 有带转换表的行级触发器，目前不允许它成为分区。另见 ATExecAddInherit() 和
	 * CreateTrigger() 里的禁止。
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(attachrel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming a partition",
						trigger_name, RelationGetRelationName(attachrel)),
				 errdetail("ROW triggers with transition tables are not supported on partitions.")));

	/*
	 * Check that the new partition's bound is valid and does not overlap any
	 * of existing partitions of the parent - note that it does not return on
	 * error.
	 *
	 * 检查新分区的边界是否合法，且不与父表已有分区重叠。注意出错时它不会返回。
	 */
	check_new_partition_bound(RelationGetRelationName(attachrel), rel,
							  cmd->bound, pstate);

	/* OK to create inheritance.  Rest of the checks performed there */
	/*
	 *
	 * 可以建立继承了。其余检查在那里做
	 */
	CreateInheritance(attachrel, rel, true);

	/* Update the pg_class entry. */
	/*
	 *
	 * 更新 pg_class 项。
	 */
	StorePartitionBound(attachrel, rel, cmd->bound);

	/* Ensure there exists a correct set of indexes in the partition. */
	/*
	 *
	 * 确保分区上存在正确的一组索引。
	 */
	AttachPartitionEnsureIndexes(wqueue, rel, attachrel);

	/* and triggers */
	/*
	 *
	 * 以及触发器
	 */
	CloneRowTriggersToPartition(rel, attachrel);

	/*
	 * Clone foreign key constraints.  Callee is responsible for setting up
	 * for phase 3 constraint verification.
	 *
	 * 克隆外键约束。被调用方负责安排阶段 3 的约束验证。
	 */
	CloneForeignKeyConstraints(wqueue, rel, attachrel);

	/*
	 * Generate partition constraint from the partition bound specification.
	 * If the parent itself is a partition, make sure to include its
	 * constraint as well.
	 *
	 * 根据分区边界说明生成分区约束。若父表本身是分区，也要把它的约束包括进去。
	 */
	partBoundConstraint = get_qual_from_partbound(rel, cmd->bound);

	/*
	 * Use list_concat_copy() to avoid modifying partBoundConstraint in place,
	 * since it's needed later to construct the constraint expression for
	 * validating against the default partition, if any.
	 *
	 * 用 list_concat_copy()，以免就地修改 partBoundConstraint，因为若有默认分区，
	 * 后面还要用它来构造校验用的约束表达式。
	 */
	partConstraint = list_concat_copy(partBoundConstraint,
									  RelationGetPartitionQual(rel));

	/* Skip validation if there are no constraints to validate. */
	/*
	 *
	 * 若没有要验证的约束，就跳过验证。
	 */
	if (partConstraint)
	{
		/*
		 * Run the partition quals through const-simplification similar to
		 * check constraints.  We skip canonicalize_qual, though, because
		 * partition quals should be in canonical form already.
		 *
		 * 把分区条件过一遍与检查约束类似的常量简化。不过我们跳过 canonicalize_qual，因为分区条件应该已经是规范形式。
		 */
		partConstraint =
			(List *) eval_const_expressions(NULL,
											(Node *) partConstraint);

		/* XXX this sure looks wrong */
		/*
		 *
		 * XXX 这看起来肯定不对
		 */
		partConstraint = list_make1(make_ands_explicit(partConstraint));

		/*
		 * Adjust the generated constraint to match this partition's attribute
		 * numbers.
		 *
		 * 调整生成的约束，使它与这个分区的属性号匹配。
		 */
		partConstraint = map_partition_varattnos(partConstraint, 1, attachrel,
												 rel);

		/* Validate partition constraints against the table being attached. */
		/*
		 *
		 * 对照被挂接的表验证分区约束。
		 */
		QueuePartitionConstraintValidation(wqueue, attachrel, partConstraint,
										   false);
	}

	/*
	 * If we're attaching a partition other than the default partition and a
	 * default one exists, then that partition's partition constraint changes,
	 * so add an entry to the work queue to validate it, too.  (We must not do
	 * this when the partition being attached is the default one; we already
	 * did it above!)
	 *
	 * 若挂接的不是默认分区，且存在默认分区，则该分区的分区约束会变，所以也往工作队列加一项去验证它。
	 * （被挂接的分区就是默认分区时绝不能做这个；上面已经做过了！）
	 */
	if (OidIsValid(defaultPartOid))
	{
		Relation	defaultrel;
		List	   *defPartConstraint;

		Assert(!cmd->bound->is_default);

		/* we already hold a lock on the default partition */
		/*
		 *
		 * 我们已经锁着默认分区
		 */
		defaultrel = table_open(defaultPartOid, NoLock);
		defPartConstraint =
			get_proposed_default_constraint(partBoundConstraint);

		/*
		 * Map the Vars in the constraint expression from rel's attnos to
		 * defaultrel's.
		 *
		 * 把约束表达式里的 Var 从 rel 的属性号映射到 defaultrel 的。
		 */
		defPartConstraint =
			map_partition_varattnos(defPartConstraint,
									1, defaultrel, rel);
		QueuePartitionConstraintValidation(wqueue, defaultrel,
										   defPartConstraint, true);

		/* keep our lock until commit. */
		/*
		 *
		 * 把锁保持到提交。
		 */
		table_close(defaultrel, NoLock);
	}

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(attachrel));

	/*
	 * If the partition we just attached is partitioned itself, invalidate
	 * relcache for all descendent partitions too to ensure that their
	 * rd_partcheck expression trees are rebuilt; partitions already locked at
	 * the beginning of this function.
	 *
	 * 若刚挂上的分区本身是分区的，也使所有后代分区的 relcache 失效，以确保它们的 rd_partcheck 表达式树被重建；
	 * 分区在本函数开头就已经锁好。
	 */
	if (attachrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ListCell   *l;

		foreach(l, attachrel_children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(l));
		}
	}

	/* keep our lock until commit */
	/*
	 *
	 * 把锁保持到提交
	 */
	table_close(attachrel, NoLock);

	return address;
}

/*
 * AttachPartitionEnsureIndexes
 *		subroutine for ATExecAttachPartition to create/match indexes
 *
 * AttachPartitionEnsureIndexes：ATExecAttachPartition 用来创建或匹配索引的子程序
 *
 * Enforce the indexing rule for partitioned tables during ALTER TABLE / ATTACH
 * PARTITION: every partition must have an index attached to each index on the
 * partitioned table.
 *
 * 在 ALTER TABLE / ATTACH PARTITION 期间强制分区表的索引规则：
 * 每个分区都必须有一个索引挂到分区表的每个索引上。
 */
static void
AttachPartitionEnsureIndexes(List **wqueue, Relation rel, Relation attachrel)
{
	List	   *idxes;
	List	   *attachRelIdxs;
	Relation   *attachrelIdxRels;
	IndexInfo **attachInfos;
	ListCell   *cell;
	MemoryContext cxt;
	MemoryContext oldcxt;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"AttachPartitionEnsureIndexes",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	idxes = RelationGetIndexList(rel);
	attachRelIdxs = RelationGetIndexList(attachrel);
	attachrelIdxRels = palloc(sizeof(Relation) * list_length(attachRelIdxs));
	attachInfos = palloc(sizeof(IndexInfo *) * list_length(attachRelIdxs));

	/* Build arrays of all existing indexes and their IndexInfos */
	/*
	 *
	 * 建立所有现有索引及其 IndexInfo 的数组
	 */
	foreach_oid(cldIdxId, attachRelIdxs)
	{
		int			i = foreach_current_index(cldIdxId);

		attachrelIdxRels[i] = index_open(cldIdxId, AccessShareLock);
		attachInfos[i] = BuildIndexInfo(attachrelIdxRels[i]);
	}

	/*
	 * If we're attaching a foreign table, we must fail if any of the indexes
	 * is a constraint index; otherwise, there's nothing to do here.  Do this
	 * before starting work, to avoid wasting the effort of building a few
	 * non-unique indexes before coming across a unique one.
	 *
	 * 若挂接的是外部表，只要有任何一个索引是约束索引就必须失败；否则这里无事可做。在开始干活之前做这个，以免先建了几个非唯一索引，
	 * 然后才碰到一个唯一索引，白费力气。
	 */
	if (attachrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		foreach(cell, idxes)
		{
			Oid			idx = lfirst_oid(cell);
			Relation	idxRel = index_open(idx, AccessShareLock);

			if (idxRel->rd_index->indisunique ||
				idxRel->rd_index->indisprimary)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot attach foreign table \"%s\" as partition of partitioned table \"%s\"",
								RelationGetRelationName(attachrel),
								RelationGetRelationName(rel)),
						 errdetail("Partitioned table \"%s\" contains unique indexes.",
								   RelationGetRelationName(rel))));
			index_close(idxRel, AccessShareLock);
		}

		goto out;
	}

	/*
	 * For each index on the partitioned table, find a matching one in the
	 * partition-to-be; if one is not found, create one.
	 *
	 * 对分区表上的每个索引，在即将成为分区的表里找一个匹配的；找不到就创建一个。
	 */
	foreach(cell, idxes)
	{
		Oid			idx = lfirst_oid(cell);
		Relation	idxRel = index_open(idx, AccessShareLock);
		IndexInfo  *info;
		AttrMap    *attmap;
		bool		found = false;
		Oid			constraintOid;

		/*
		 * Ignore indexes in the partitioned table other than partitioned
		 * indexes.
		 *
		 * 忽略分区表里除分区索引以外的索引。
		 */
		if (idxRel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			index_close(idxRel, AccessShareLock);
			continue;
		}

		/* construct an indexinfo to compare existing indexes against */
		/*
		 *
		 * 构造一个 indexinfo，用来和现有索引比较
		 */
		info = BuildIndexInfo(idxRel);
		attmap = build_attrmap_by_name(RelationGetDescr(attachrel),
									   RelationGetDescr(rel),
									   false);
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(rel), idx);

		/*
		 * Scan the list of existing indexes in the partition-to-be, and mark
		 * the first matching, valid, unattached one we find, if any, as
		 * partition of the parent index.  If we find one, we're done.
		 *
		 * 扫描即将成为分区的表里的现有索引，把我们找到的第一个匹配、有效、尚未挂接的标成父索引的分区。若找到一个，就完成了。
		 */
		for (int i = 0; i < list_length(attachRelIdxs); i++)
		{
			Oid			cldIdxId = RelationGetRelid(attachrelIdxRels[i]);
			Oid			cldConstrOid = InvalidOid;

			/* does this index have a parent?  if so, can't use it */
			/*
			 *
			 * 这个索引有父索引吗？若有，就不能用
			 */
			if (attachrelIdxRels[i]->rd_rel->relispartition)
				continue;

			/* If this index is invalid, can't use it */
			/*
			 *
			 * 若这个索引无效，就不能用
			 */
			if (!attachrelIdxRels[i]->rd_index->indisvalid)
				continue;

			if (CompareIndexInfo(attachInfos[i], info,
								 attachrelIdxRels[i]->rd_indcollation,
								 idxRel->rd_indcollation,
								 attachrelIdxRels[i]->rd_opfamily,
								 idxRel->rd_opfamily,
								 attmap))
			{
				/*
				 * If this index is being created in the parent because of a
				 * constraint, then the child needs to have a constraint also,
				 * so look for one.  If there is no such constraint, this
				 * index is no good, so keep looking.
				 *
				 * 若父表上这个索引是因为约束而创建的，子表也需要有约束，所以去找一个。若没有这样的约束，这个索引不行，继续找。
				 */
				if (OidIsValid(constraintOid))
				{
					cldConstrOid =
						get_relation_idx_constraint_oid(RelationGetRelid(attachrel),
														cldIdxId);
					/* no dice */
					/*
					 *
					 * 没戏
					 */
					if (!OidIsValid(cldConstrOid))
						continue;

					/* Ensure they're both the same type of constraint */
					/*
					 *
					 * 确保它们是同一类型的约束
					 */
					if (get_constraint_type(constraintOid) !=
						get_constraint_type(cldConstrOid))
						continue;
				}

				/* bingo. */
				/*
				 *
				 * 找到了。
				 */
				IndexSetParentIndex(attachrelIdxRels[i], idx);
				if (OidIsValid(constraintOid))
					ConstraintSetParentConstraint(cldConstrOid, constraintOid,
												  RelationGetRelid(attachrel));
				found = true;

				CommandCounterIncrement();
				break;
			}
		}

		/*
		 * If no suitable index was found in the partition-to-be, create one
		 * now.  Note that if this is a PK, not-null constraints must already
		 * exist.
		 *
		 * 若即将成为分区的表里没有合适的索引，现在创建一个。注意若这是主键，NOT NULL 约束必须已经存在。
		 */
		if (!found)
		{
			IndexStmt  *stmt;
			Oid			conOid;

			stmt = generateClonedIndexStmt(NULL,
										   idxRel, attmap,
										   &conOid);
			DefineIndex(RelationGetRelid(attachrel), stmt, InvalidOid,
						RelationGetRelid(idxRel),
						conOid,
						-1,
						true, false, false, false, false);
		}

		index_close(idxRel, AccessShareLock);
	}

out:
	/* Clean up. */
	/*
	 *
	 * 清理。
	 */
	for (int i = 0; i < list_length(attachRelIdxs); i++)
		index_close(attachrelIdxRels[i], AccessShareLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * CloneRowTriggersToPartition
 *		subroutine for ATExecAttachPartition/DefineRelation to create row
 *		triggers on partitions
 *
 * CloneRowTriggersToPartition：ATExecAttachPartition/DefineRelation
 * 用来在分区上创建行触发器的子程序
 */
static void
CloneRowTriggersToPartition(Relation parent, Relation partition)
{
	Relation	pg_trigger;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	MemoryContext perTupCxt;

	ScanKeyInit(&key, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parent)));
	pg_trigger = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(pg_trigger, TriggerRelidNameIndexId,
							  true, NULL, 1, &key);

	perTupCxt = AllocSetContextCreate(CurrentMemoryContext,
									  "clone trig", ALLOCSET_SMALL_SIZES);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_trigger trigForm = (Form_pg_trigger) GETSTRUCT(tuple);
		CreateTrigStmt *trigStmt;
		Node	   *qual = NULL;
		Datum		value;
		bool		isnull;
		List	   *cols = NIL;
		List	   *trigargs = NIL;
		MemoryContext oldcxt;

		/*
		 * Ignore statement-level triggers; those are not cloned.
		 *
		 * 忽略语句级触发器；那些不会被克隆。
		 */
		if (!TRIGGER_FOR_ROW(trigForm->tgtype))
			continue;

		/*
		 * Don't clone internal triggers, because the constraint cloning code
		 * will.
		 *
		 * 不要克隆内部触发器，因为约束克隆代码会做。
		 */
		if (trigForm->tgisinternal)
			continue;

		/*
		 * Complain if we find an unexpected trigger type.
		 *
		 * 若发现意料之外的触发器类型，就抱怨。
		 */
		if (!TRIGGER_FOR_BEFORE(trigForm->tgtype) &&
			!TRIGGER_FOR_AFTER(trigForm->tgtype))
			elog(ERROR, "unexpected trigger \"%s\" found",
				 NameStr(trigForm->tgname));

		/* Use short-lived context for CREATE TRIGGER */
		/*
		 *
		 * CREATE TRIGGER 使用短生命周期的上下文
		 */
		oldcxt = MemoryContextSwitchTo(perTupCxt);

		/*
		 * If there is a WHEN clause, generate a 'cooked' version of it that's
		 * appropriate for the partition.
		 *
		 * 若有 WHEN 子句，生成一个适合该分区的已煮好版本。
		 */
		value = heap_getattr(tuple, Anum_pg_trigger_tgqual,
							 RelationGetDescr(pg_trigger), &isnull);
		if (!isnull)
		{
			qual = stringToNode(TextDatumGetCString(value));
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_OLD_VARNO,
													partition, parent);
			qual = (Node *) map_partition_varattnos((List *) qual, PRS2_NEW_VARNO,
													partition, parent);
		}

		/*
		 * If there is a column list, transform it to a list of column names.
		 * Note we don't need to map this list in any way ...
		 *
		 * 若有列清单，把它变换成列名列表。注意我们不需要以任何方式映射这个列表……
		 */
		if (trigForm->tgattr.dim1 > 0)
		{
			int			i;

			for (i = 0; i < trigForm->tgattr.dim1; i++)
			{
				Form_pg_attribute col;

				col = TupleDescAttr(parent->rd_att,
									trigForm->tgattr.values[i] - 1);
				cols = lappend(cols,
							   makeString(pstrdup(NameStr(col->attname))));
			}
		}

		/* Reconstruct trigger arguments list. */
		/*
		 *
		 * 重建触发器参数列表。
		 */
		if (trigForm->tgnargs > 0)
		{
			char	   *p;

			value = heap_getattr(tuple, Anum_pg_trigger_tgargs,
								 RelationGetDescr(pg_trigger), &isnull);
			if (isnull)
				elog(ERROR, "tgargs is null for trigger \"%s\" in partition \"%s\"",
					 NameStr(trigForm->tgname), RelationGetRelationName(partition));

			p = (char *) VARDATA_ANY(DatumGetByteaPP(value));

			for (int i = 0; i < trigForm->tgnargs; i++)
			{
				trigargs = lappend(trigargs, makeString(pstrdup(p)));
				p += strlen(p) + 1;
			}
		}

		trigStmt = makeNode(CreateTrigStmt);
		trigStmt->replace = false;
		trigStmt->isconstraint = OidIsValid(trigForm->tgconstraint);
		trigStmt->trigname = NameStr(trigForm->tgname);
		trigStmt->relation = NULL;
		trigStmt->funcname = NULL;	/* passed separately */
						/*
						 *
						 * 单独传递
						 */
		trigStmt->args = trigargs;
		trigStmt->row = true;
		trigStmt->timing = trigForm->tgtype & TRIGGER_TYPE_TIMING_MASK;
		trigStmt->events = trigForm->tgtype & TRIGGER_TYPE_EVENT_MASK;
		trigStmt->columns = cols;
		trigStmt->whenClause = NULL;	/* passed separately */
						/*
						 *
						 * 单独传递
						 */
		trigStmt->transitionRels = NIL; /* not supported at present */
						/*
						 *
						 * 目前不支持
						 */
		trigStmt->deferrable = trigForm->tgdeferrable;
		trigStmt->initdeferred = trigForm->tginitdeferred;
		trigStmt->constrrel = NULL; /* passed separately */
					    /*
					     *
					     * 单独传递
					     */

		CreateTriggerFiringOn(trigStmt, NULL, RelationGetRelid(partition),
							  trigForm->tgconstrrelid, InvalidOid, InvalidOid,
							  trigForm->tgfoid, trigForm->oid, qual,
							  false, true, trigForm->tgenabled);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(perTupCxt);
	}

	MemoryContextDelete(perTupCxt);

	systable_endscan(scan);
	table_close(pg_trigger, RowExclusiveLock);
}

/*
 * ALTER TABLE DETACH PARTITION
 *
 * ALTER TABLE DETACH PARTITION（分离分区）
 *
 * Return the address of the relation that is no longer a partition of rel.
 *
 * 返回不再是 rel 的分区的那个关系的地址。
 *
 * If concurrent mode is requested, we run in two transactions.  A side-
 * effect is that this command cannot run in a multi-part ALTER TABLE.
 * Currently, that's enforced by the grammar.
 *
 * 若请求并发模式，我们在两个事务里运行。副作用是这条命令不能出现在多段 ALTER TABLE 里。目前由语法强制这一点。
 *
 * The strategy for concurrency is to first modify the partition's
 * pg_inherit catalog row to make it visible to everyone that the
 * partition is detached, lock the partition against writes, and commit
 * the transaction; anyone who requests the partition descriptor from
 * that point onwards has to ignore such a partition.  In a second
 * transaction, we wait until all transactions that could have seen the
 * partition as attached are gone, then we remove the rest of partition
 * metadata (pg_inherits and pg_class.relpartbounds).
 *
 * 并发策略是：先修改分区的 pg_inherit 目录行，让所有人都能看见该分区已分离，锁住分区禁止写入，然后提交事务；从那以后，
 * 任何请求分区描述符的人都必须忽略这样的分区。在第二个事务里，我们等到所有可能把该分区看成已挂接的事务都消失，
 * 然后去掉其余的分区元数据（pg_inherits 和 pg_class.relpartbounds）。
 */
static ObjectAddress
ATExecDetachPartition(List **wqueue, AlteredTableInfo *tab, Relation rel,
					  RangeVar *name, bool concurrent)
{
	Relation	partRel;
	ObjectAddress address;
	Oid			defaultPartOid;
	PartitionDesc partdesc;

	/*
	 * We must lock the default partition, because detaching this partition
	 * will change its partition constraint.
	 *
	 * 必须锁住默认分区，因为分离这个分区会改变它的分区约束。
	 */
	partdesc = RelationGetPartitionDesc(rel, true);
	defaultPartOid = get_default_oid_from_partdesc(partdesc);
	if (OidIsValid(defaultPartOid))
	{
		/*
		 * Concurrent detaching when a default partition exists is not
		 * supported. The main problem is that the default partition
		 * constraint would change.  And there's a definitional problem: what
		 * should happen to the tuples that are being inserted that belong to
		 * the partition being detached?  Putting them on the partition being
		 * detached would be wrong, since they'd become "lost" after the
		 * detaching completes but we cannot put them in the default partition
		 * either until we alter its partition constraint.
		 *
		 * 存在默认分区时不支持并发分离。主要问题是默认分区约束会变。还有一个定义上的问题：正在插入、
		 * 且属于被分离分区的那些元组该怎么办？把它们放到被分离的分区上是错的，因为分离完成后它们会“丢失”；但在改默认分区的分区约束之前，
		 * 也不能把它们放进默认分区。
		 *
		 * I think we could solve this problem if we effected the constraint
		 * change before committing the first transaction.  But the lock would
		 * have to remain AEL and it would cause concurrent query planning to
		 * be blocked, so changing it that way would be even worse.
		 *
		 * 我觉得若在提交第一个事务之前就完成约束变更，可以解决这个问题。但锁必须保持为 AccessExclusiveLock，
		 * 而且会挡住并发的查询规划，所以那样改会更糟。
		 */
		if (concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot detach partitions concurrently when a default partition exists")));
		LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	/*
	 * In concurrent mode, the partition is locked with share-update-exclusive
	 * in the first transaction.  This allows concurrent transactions to be
	 * doing DML to the partition.
	 *
	 * 并发模式下，第一个事务用 ShareUpdateExclusiveLock 锁分区。这允许并发事务对该分区做 DML。
	 */
	partRel = table_openrv(name, concurrent ? ShareUpdateExclusiveLock :
						   AccessExclusiveLock);

	/*
	 * Check inheritance conditions and either delete the pg_inherits row (in
	 * non-concurrent mode) or just set the inhdetachpending flag.
	 *
	 * 检查继承条件，并要么删除 pg_inherits 行（非并发模式），要么只设置 inhdetachpending 标志。
	 */
	if (!concurrent)
		RemoveInheritance(partRel, rel, false);
	else
		MarkInheritDetached(partRel, rel);

	/*
	 * Ensure that foreign keys still hold after this detach.  This keeps
	 * locks on the referencing tables, which prevents concurrent transactions
	 * from adding rows that we wouldn't see.  For this to work in concurrent
	 * mode, it is critical that the partition appears as no longer attached
	 * for the RI queries as soon as the first transaction commits.
	 *
	 * 确保分离之后外键仍然成立。这会保持对引用表的锁，防止并发事务加入我们看不见的行。要让这在并发模式下工作，关键是第一个事务一提交，
	 * 对该分区的 RI 查询就必须把它看成不再挂接。
	 */
	ATDetachCheckNoForeignKeyRefs(partRel);

	/*
	 * Concurrent mode has to work harder; first we add a new constraint to
	 * the partition that matches the partition constraint.  Then we close our
	 * existing transaction, and in a new one wait for all processes to catch
	 * up on the catalog updates we've done so far; at that point we can
	 * complete the operation.
	 *
	 * 并发模式要更费劲：先给分区加一条与分区约束匹配的新约束。然后结束现有事务，在新事务里等到所有进程都赶上我们目前为止做的目录更新；
	 * 那时才能完成操作。
	 */
	if (concurrent)
	{
		Oid			partrelid,
					parentrelid;
		LOCKTAG		tag;
		char	   *parentrelname;
		char	   *partrelname;

		/*
		 * For strategies other than hash, add a constraint to the partition
		 * being detached which supplants the partition constraint. For hash
		 * we cannot do that, because the constraint would reference the
		 * partitioned table OID, possibly causing problems later.
		 *
		 * 对哈希以外的策略，给正在分离的分区加一条取代分区约束的约束。哈希做不到，因为约束会引用分区表 OID，以后可能惹麻烦。
		 */
		if (partdesc->boundinfo->strategy != PARTITION_STRATEGY_HASH)
			DetachAddConstraintIfNeeded(wqueue, partRel);

		/*
		 * We're almost done now; the only traces that remain are the
		 * pg_inherits tuple and the partition's relpartbounds.  Before we can
		 * remove those, we need to wait until all transactions that know that
		 * this is a partition are gone.
		 *
		 * 差不多做完了；剩下的痕迹只有 pg_inherits 元组和分区的 relpartbounds。去掉它们之前，
		 * 需要等到所有知道这是一个分区的事务都消失。
		 */

		/*
		 * Remember relation OIDs to re-acquire them later; and relation names
		 * too, for error messages if something is dropped in between.
		 *
		 * 记住关系 OID，以便以后重新获取；也记住关系名，以防中间有东西被删掉时用来报错。
		 */
		partrelid = RelationGetRelid(partRel);
		parentrelid = RelationGetRelid(rel);
		parentrelname = MemoryContextStrdup(PortalContext,
											RelationGetRelationName(rel));
		partrelname = MemoryContextStrdup(PortalContext,
										  RelationGetRelationName(partRel));

		/* Invalidate relcache entries for the parent -- must be before close */
		/*
		 *
		 * 使父表的 relcache 项失效，必须在关闭之前
		 */
		CacheInvalidateRelcache(rel);

		table_close(partRel, NoLock);
		table_close(rel, NoLock);
		tab->rel = NULL;

		/* Make updated catalog entry visible */
		/*
		 *
		 * 让更新后的目录项可见
		 */
		PopActiveSnapshot();
		CommitTransactionCommand();

		StartTransactionCommand();

		/*
		 * Now wait.  This ensures that all queries that were planned
		 * including the partition are finished before we remove the rest of
		 * catalog entries.  We don't need or indeed want to acquire this
		 * lock, though -- that would block later queries.
		 *
		 * 现在等待。这确保所有把该分区算进去而规划出来的查询都在我们去掉其余目录项之前结束。不过我们不需要、也不想获取这把锁，
		 * 那会挡住以后的查询。
		 *
		 * We don't need to concern ourselves with waiting for a lock on the
		 * partition itself, since we will acquire AccessExclusiveLock below.
		 *
		 * 不必操心等待分区本身上的锁，因为下面会获取 AccessExclusiveLock。
		 */
		SET_LOCKTAG_RELATION(tag, MyDatabaseId, parentrelid);
		WaitForLockersMultiple(list_make1(&tag), AccessExclusiveLock, false);

		/*
		 * Now acquire locks in both relations again.  Note they may have been
		 * removed in the meantime, so care is required.
		 *
		 * 现在再次锁住两边的关系。注意它们在此期间可能已被删除，所以要小心。
		 */
		rel = try_relation_open(parentrelid, ShareUpdateExclusiveLock);
		partRel = try_relation_open(partrelid, AccessExclusiveLock);

		/* If the relations aren't there, something bad happened; bail out */
		/*
		 *
		 * 若关系不在了，说明出了坏事；退出
		 */
		if (rel == NULL)
		{
			if (partRel != NULL)	/* shouldn't happen */
						/*
						 *
						 * 不该发生
						 */
				elog(WARNING, "dangling partition \"%s\" remains, can't fix",
					 partrelname);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partitioned table \"%s\" was removed concurrently",
							parentrelname)));
		}
		if (partRel == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partition \"%s\" was removed concurrently", partrelname)));

		tab->rel = rel;
	}

	/*
	 * Detaching the partition might involve TOAST table access, so ensure we
	 * have a valid snapshot.
	 *
	 * 分离分区可能访问 TOAST 表，所以确保我们有有效的快照。
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Do the final part of detaching */
	/*
	 *
	 * 做分离的最后一部分
	 */
	DetachPartitionFinalize(rel, partRel, concurrent, defaultPartOid);

	PopActiveSnapshot();

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	/* keep our lock until commit */
	/*
	 *
	 * 把锁保持到提交
	 */
	table_close(partRel, NoLock);

	return address;
}

/*
 * Second part of ALTER TABLE .. DETACH.
 *
 * ALTER TABLE .. DETACH 的第二部分。
 *
 * This is separate so that it can be run independently when the second
 * transaction of the concurrent algorithm fails (crash or abort).
 *
 * 单独拆出来，以便并发算法的第二个事务失败（崩溃或中止）时可以独立运行。
 */
static void
DetachPartitionFinalize(Relation rel, Relation partRel, bool concurrent,
						Oid defaultPartOid)
{
	Relation	classRel;
	List	   *fks;
	ListCell   *cell;
	List	   *indexes;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	HeapTuple	tuple,
				newtuple;
	Relation	trigrel = NULL;
	List	   *fkoids = NIL;

	if (concurrent)
	{
		/*
		 * We can remove the pg_inherits row now. (In the non-concurrent case,
		 * this was already done).
		 *
		 * 现在可以去掉 pg_inherits 行了。（非并发情况下已经做过。）
		 */
		RemoveInheritance(partRel, rel, true);
	}

	/* Drop any triggers that were cloned on creation/attach. */
	/*
	 *
	 * 删掉创建或挂接时克隆来的任何触发器。
	 */
	DropClonedTriggersFromPartition(RelationGetRelid(partRel));

	/*
	 * Detach any foreign keys that are inherited.  This includes creating
	 * additional action triggers.
	 *
	 * 分离任何继承来的外键。这包括创建额外的动作触发器。
	 */
	fks = copyObject(RelationGetFKeyList(partRel));
	if (fks != NIL)
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * It's possible that the partition being detached has a foreign key that
	 * references a partitioned table.  When that happens, there are multiple
	 * pg_constraint rows for the partition: one points to the partitioned
	 * table itself, while the others point to each of its partitions.  Only
	 * the topmost one is to be considered here; the child constraints must be
	 * left alone, because conceptually those aren't coming from our parent
	 * partitioned table, but from this partition itself.
	 *
	 * 被分离的分区可能有一个引用分区表的外键。那种情况下，该分区有多行 pg_constraint：一行指向分区表本身，
	 * 其余指向它的各个分区。这里只考虑最顶上的那一行；子约束必须留着，因为从概念上它们不是来自我们的父分区表，而是来自这个分区本身。
	 *
	 * We implement this by collecting all the constraint OIDs in a first scan
	 * of the FK array, and skipping in the loop below those constraints whose
	 * parents are listed here.
	 *
	 * 实现方法是先扫描一遍外键数组，收集所有约束 OID，然后在下面的循环里跳过那些父约束列在这里的约束。
	 */
	foreach_node(ForeignKeyCacheInfo, fk, fks)
		fkoids = lappend_oid(fkoids, fk->conoid);

	foreach(cell, fks)
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);
		HeapTuple	contup;
		Form_pg_constraint conform;

		contup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(fk->conoid));
		if (!HeapTupleIsValid(contup))
			elog(ERROR, "cache lookup failed for constraint %u", fk->conoid);
		conform = (Form_pg_constraint) GETSTRUCT(contup);

		/*
		 * Consider only inherited foreign keys, and only if their parents
		 * aren't in the list.
		 *
		 * 只考虑继承来的外键，并且只在它们的父约束不在列表里时才考虑。
		 */
		if (conform->contype != CONSTRAINT_FOREIGN ||
			!OidIsValid(conform->conparentid) ||
			list_member_oid(fkoids, conform->conparentid))
		{
			ReleaseSysCache(contup);
			continue;
		}

		/*
		 * The constraint on this table must be marked no longer a child of
		 * the parent's constraint, as do its check triggers.
		 *
		 * 这张表上的约束必须标成不再是父约束的子约束，它的检查触发器也一样。
		 */
		ConstraintSetParentConstraint(fk->conoid, InvalidOid, InvalidOid);

		/*
		 * Also, look up the partition's "check" triggers corresponding to the
		 * ENFORCED constraint being detached and detach them from the parent
		 * triggers. NOT ENFORCED constraints do not have these triggers;
		 * therefore, this step is not needed.
		 *
		 * 另外，查找与正在分离的 ENFORCED 约束对应的分区检查触发器，并把它们从父触发器上拆下来。NOT ENFORCED
		 * 约束没有这些触发器，因此不需要这一步。
		 */
		if (fk->conenforced)
		{
			Oid			insertTriggerOid,
						updateTriggerOid;

			GetForeignKeyCheckTriggers(trigrel,
									   fk->conoid, fk->confrelid, fk->conrelid,
									   &insertTriggerOid, &updateTriggerOid);
			Assert(OidIsValid(insertTriggerOid));
			TriggerSetParentTrigger(trigrel, insertTriggerOid, InvalidOid,
									RelationGetRelid(partRel));
			Assert(OidIsValid(updateTriggerOid));
			TriggerSetParentTrigger(trigrel, updateTriggerOid, InvalidOid,
									RelationGetRelid(partRel));
		}

		/*
		 * Lastly, create the action triggers on the referenced table, using
		 * addFkRecurseReferenced, which requires some elaborate setup (so put
		 * it in a separate block).  While at it, if the table is partitioned,
		 * that function will recurse to create the pg_constraint rows and
		 * action triggers for each partition.
		 *
		 * 最后，用 addFkRecurseReferenced 在被引用表上创建动作触发器，这需要一些精心准备（所以放在单独的块里）。顺便，
		 * 若表是分区的，那个函数会递归为每个分区创建 pg_constraint 行和动作触发器。
		 *
		 * Note there's no need to do addFkConstraint() here, because the
		 * pg_constraint row already exists.
		 *
		 * 注意这里不必做 addFkConstraint()，因为 pg_constraint 行已经存在。
		 */
		{
			Constraint *fkconstraint;
			int			numfks;
			AttrNumber	conkey[INDEX_MAX_KEYS];
			AttrNumber	confkey[INDEX_MAX_KEYS];
			Oid			conpfeqop[INDEX_MAX_KEYS];
			Oid			conppeqop[INDEX_MAX_KEYS];
			Oid			conffeqop[INDEX_MAX_KEYS];
			int			numfkdelsetcols;
			AttrNumber	confdelsetcols[INDEX_MAX_KEYS];
			Relation	refdRel;

			DeconstructFkConstraintRow(contup,
									   &numfks,
									   conkey,
									   confkey,
									   conpfeqop,
									   conppeqop,
									   conffeqop,
									   &numfkdelsetcols,
									   confdelsetcols);

			/* Create a synthetic node we'll use throughout */
			/*
			 *
			 * 造一个我们全程都会用的合成节点
			 */
			fkconstraint = makeNode(Constraint);
			fkconstraint->contype = CONSTRAINT_FOREIGN;
			fkconstraint->conname = pstrdup(NameStr(conform->conname));
			fkconstraint->deferrable = conform->condeferrable;
			fkconstraint->initdeferred = conform->condeferred;
			fkconstraint->is_enforced = conform->conenforced;
			fkconstraint->skip_validation = true;
			fkconstraint->initially_valid = conform->convalidated;
			/* a few irrelevant fields omitted here */
			/*
			 *
			 * 这里省略了几个无关字段
			 */
			fkconstraint->pktable = NULL;
			fkconstraint->fk_attrs = NIL;
			fkconstraint->pk_attrs = NIL;
			fkconstraint->fk_matchtype = conform->confmatchtype;
			fkconstraint->fk_upd_action = conform->confupdtype;
			fkconstraint->fk_del_action = conform->confdeltype;
			fkconstraint->fk_del_set_cols = NIL;
			fkconstraint->old_conpfeqop = NIL;
			fkconstraint->old_pktable_oid = InvalidOid;
			fkconstraint->location = -1;

			/* set up colnames, used to generate the constraint name */
			/*
			 *
			 * 准备列名，用来生成约束名
			 */
			for (int i = 0; i < numfks; i++)
			{
				Form_pg_attribute att;

				att = TupleDescAttr(RelationGetDescr(partRel),
									conkey[i] - 1);

				fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
												 makeString(NameStr(att->attname)));
			}

			refdRel = table_open(fk->confrelid, ShareRowExclusiveLock);

			addFkRecurseReferenced(fkconstraint, partRel,
								   refdRel,
								   conform->conindid,
								   fk->conoid,
								   numfks,
								   confkey,
								   conkey,
								   conpfeqop,
								   conppeqop,
								   conffeqop,
								   numfkdelsetcols,
								   confdelsetcols,
								   true,
								   InvalidOid, InvalidOid,
								   conform->conperiod);
			table_close(refdRel, NoLock);	/* keep lock till end of xact */
							/*
							 *
							 * 锁保持到事务结束
							 */
		}

		ReleaseSysCache(contup);
	}
	list_free_deep(fks);
	if (trigrel)
		table_close(trigrel, RowExclusiveLock);

	/*
	 * Any sub-constraints that are in the referenced-side of a larger
	 * constraint have to be removed.  This partition is no longer part of the
	 * key space of the constraint.
	 *
	 * 任何处于更大约束的被引用侧的子约束都必须去掉。这个分区不再属于该约束的键空间。
	 */
	foreach(cell, GetParentedForeignKeyRefs(partRel))
	{
		Oid			constrOid = lfirst_oid(cell);
		ObjectAddress constraint;

		ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);
		deleteDependencyRecordsForClass(ConstraintRelationId,
										constrOid,
										ConstraintRelationId,
										DEPENDENCY_INTERNAL);
		CommandCounterIncrement();

		ObjectAddressSet(constraint, ConstraintRelationId, constrOid);
		performDeletion(&constraint, DROP_RESTRICT, 0);
	}

	/* Now we can detach indexes */
	/*
	 *
	 * 现在可以分离索引了
	 */
	indexes = RelationGetIndexList(partRel);
	foreach(cell, indexes)
	{
		Oid			idxid = lfirst_oid(cell);
		Oid			parentidx;
		Relation	idx;
		Oid			constrOid;
		Oid			parentConstrOid;

		if (!has_superclass(idxid))
			continue;

		parentidx = get_partition_parent(idxid, false);
		Assert((IndexGetRelation(parentidx, false) == RelationGetRelid(rel)));

		idx = index_open(idxid, AccessExclusiveLock);
		IndexSetParentIndex(idx, InvalidOid);

		/*
		 * If there's a constraint associated with the index, detach it too.
		 * Careful: it is possible for a constraint index in a partition to be
		 * the child of a non-constraint index, so verify whether the parent
		 * index does actually have a constraint.
		 *
		 * 若索引关联着约束，也把它分离。小心：分区里的约束索引有可能是非约束索引的子索引，所以要核实父索引是否真的有约束。
		 */
		constrOid = get_relation_idx_constraint_oid(RelationGetRelid(partRel),
													idxid);
		parentConstrOid = get_relation_idx_constraint_oid(RelationGetRelid(rel),
														  parentidx);
		if (OidIsValid(parentConstrOid) && OidIsValid(constrOid))
			ConstraintSetParentConstraint(constrOid, InvalidOid, InvalidOid);

		index_close(idx, NoLock);
	}

	/* Update pg_class tuple */
	/*
	 *
	 * 更新 pg_class 元组
	 */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(partRel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(partRel));
	Assert(((Form_pg_class) GETSTRUCT(tuple))->relispartition);

	/* Clear relpartbound and reset relispartition */
	/*
	 *
	 * 清掉 relpartbound 并重置 relispartition
	 */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = (Datum) 0;
	new_null[Anum_pg_class_relpartbound - 1] = true;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);

	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = false;
	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	/*
	 * Drop identity property from all identity columns of partition.
	 *
	 * 去掉分区所有标识列的标识属性。
	 */
	for (int attno = 0; attno < RelationGetNumberOfAttributes(partRel); attno++)
	{
		Form_pg_attribute attr = TupleDescAttr(partRel->rd_att, attno);

		if (!attr->attisdropped && attr->attidentity)
			ATExecDropIdentity(partRel, NameStr(attr->attname), false,
							   AccessExclusiveLock, true, true);
	}

	if (OidIsValid(defaultPartOid))
	{
		/*
		 * If the relation being detached is the default partition itself,
		 * remove it from the parent's pg_partitioned_table entry.
		 *
		 * 若被分离的关系本身就是默认分区，把它从父表的 pg_partitioned_table 项里去掉。
		 *
		 * If not, we must invalidate default partition's relcache entry, as
		 * in StorePartitionBound: its partition constraint depends on every
		 * other partition's partition constraint.
		 *
		 * 否则必须使默认分区的 relcache 项失效，就像 StorePartitionBound 那样：
		 * 它的分区约束依赖其他每个分区的分区约束。
		 */
		if (RelationGetRelid(partRel) == defaultPartOid)
			update_default_partition_oid(RelationGetRelid(rel), InvalidOid);
		else
			CacheInvalidateRelcacheByRelid(defaultPartOid);
	}

	/*
	 * Invalidate the parent's relcache so that the partition is no longer
	 * included in its partition descriptor.
	 *
	 * 使父表的 relcache 失效，这样分区描述符里就不再包含这个分区。
	 */
	CacheInvalidateRelcache(rel);

	/*
	 * If the partition we just detached is partitioned itself, invalidate
	 * relcache for all descendent partitions too to ensure that their
	 * rd_partcheck expression trees are rebuilt; must lock partitions before
	 * doing so, using the same lockmode as what partRel has been locked with
	 * by the caller.
	 *
	 * 若刚分离的分区本身是分区的，也使所有后代分区的 relcache 失效，以确保它们的 rd_partcheck 表达式树被重建；
	 * 做之前必须锁住分区，锁模式与调用方锁 partRel 的相同。
	 */
	if (partRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *children;

		children = find_all_inheritors(RelationGetRelid(partRel),
									   AccessExclusiveLock, NULL);
		foreach(cell, children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(cell));
		}
	}
}

/*
 * ALTER TABLE ... DETACH PARTITION ... FINALIZE
 *
 * ALTER TABLE ... DETACH PARTITION ... FINALIZE（完成分区分离）
 *
 * To use when a DETACH PARTITION command previously did not run to
 * completion; this completes the detaching process.
 *
 * 用于先前的 DETACH PARTITION 命令没有跑完的情况；这会完成分离过程。
 */
static ObjectAddress
ATExecDetachPartitionFinalize(Relation rel, RangeVar *name)
{
	Relation	partRel;
	ObjectAddress address;
	Snapshot	snap = GetActiveSnapshot();

	partRel = table_openrv(name, AccessExclusiveLock);

	/*
	 * Wait until existing snapshots are gone.  This is important if the
	 * second transaction of DETACH PARTITION CONCURRENTLY is canceled: the
	 * user could immediately run DETACH FINALIZE without actually waiting for
	 * existing transactions.  We must not complete the detach action until
	 * all such queries are complete (otherwise we would present them with an
	 * inconsistent view of catalogs).
	 *
	 * 等到现有快照都消失。若 DETACH PARTITION CONCURRENTLY 的第二个事务被取消，这一点很重要：
	 * 用户可能不真正等待现有事务就立刻跑 DETACH FINALIZE。在所有这些查询完成之前，
	 * 绝不能完成分离动作（否则会给它们看到不一致的目录视图）。
	 */
	WaitForOlderSnapshots(snap->xmin, false);

	DetachPartitionFinalize(rel, partRel, true, InvalidOid);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	table_close(partRel, NoLock);

	return address;
}

/*
 * DetachAddConstraintIfNeeded
 *		Subroutine for ATExecDetachPartition.  Create a constraint that
 *		takes the place of the partition constraint, but avoid creating
 *		a dupe if a constraint already exists which implies the needed
 *		constraint.
 *
 * DetachAddConstraintIfNeeded：ATExecDetachPartition 的子程序。
 * 创建一条取代分区约束的约束，但若已有约束蕴含所需约束，就避免创建重复的。
 */
static void
DetachAddConstraintIfNeeded(List **wqueue, Relation partRel)
{
	List	   *constraintExpr;

	constraintExpr = RelationGetPartitionQual(partRel);
	constraintExpr = (List *) eval_const_expressions(NULL, (Node *) constraintExpr);

	/*
	 * Avoid adding a new constraint if the needed constraint is implied by an
	 * existing constraint
	 *
	 * 若所需约束已被现有约束蕴含，就不要再加新约束
	 */
	if (!PartConstraintImpliedByRelConstraint(partRel, constraintExpr))
	{
		AlteredTableInfo *tab;
		Constraint *n;

		tab = ATGetQueueEntry(wqueue, partRel);

		/* Add constraint on partition, equivalent to the partition constraint */
		/*
		 *
		 * 在分区上加一条等价于分区约束的约束
		 */
		n = makeNode(Constraint);
		n->contype = CONSTR_CHECK;
		n->conname = NULL;
		n->location = -1;
		n->is_no_inherit = false;
		n->raw_expr = NULL;
		n->cooked_expr = nodeToString(make_ands_explicit(constraintExpr));
		n->is_enforced = true;
		n->initially_valid = true;
		n->skip_validation = true;
		/* It's a re-add, since it nominally already exists */
		/*
		 *
		 * 这是重新加上，因为它名义上已经存在
		 */
		ATAddCheckNNConstraint(wqueue, tab, partRel, n,
							   true, false, true, ShareUpdateExclusiveLock);
	}
}

/*
 * DropClonedTriggersFromPartition
 *		subroutine for ATExecDetachPartition to remove any triggers that were
 *		cloned to the partition when it was created-as-partition or attached.
 *		This undoes what CloneRowTriggersToPartition did.
 *
 * DropClonedTriggersFromPartition：ATExecDetachPartition 的子程序，
 * 去掉分区作为分区创建或被挂接时克隆到它上面的任何触发器。这撤销 CloneRowTriggersToPartition 所做的事。
 */
static void
DropClonedTriggersFromPartition(Oid partitionId)
{
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	trigtup;
	Relation	tgrel;
	ObjectAddresses *objects;

	objects = new_object_addresses();

	/*
	 * Scan pg_trigger to search for all triggers on this rel.
	 *
	 * 扫描 pg_trigger，搜索这个关系上的所有触发器。
	 */
	ScanKeyInit(&skey, Anum_pg_trigger_tgrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(partitionId));
	tgrel = table_open(TriggerRelationId, RowExclusiveLock);
	scan = systable_beginscan(tgrel, TriggerRelidNameIndexId,
							  true, NULL, 1, &skey);
	while (HeapTupleIsValid(trigtup = systable_getnext(scan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(trigtup);
		ObjectAddress trig;

		/* Ignore triggers that weren't cloned */
		/*
		 *
		 * 忽略不是克隆来的触发器
		 */
		if (!OidIsValid(pg_trigger->tgparentid))
			continue;

		/*
		 * Ignore internal triggers that are implementation objects of foreign
		 * keys, because these will be detached when the foreign keys
		 * themselves are.
		 *
		 * 忽略作为外键实现对象的内部触发器，因为外键本身分离时这些会被拆掉。
		 */
		if (OidIsValid(pg_trigger->tgconstrrelid))
			continue;

		/*
		 * This is ugly, but necessary: remove the dependency markings on the
		 * trigger so that it can be removed.
		 *
		 * 这很难看，但有必要：去掉触发器上的依赖标记，这样它才能被删除。
		 */
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);

		/* remember this trigger to remove it below */
		/*
		 *
		 * 记住这个触发器，下面再删
		 */
		ObjectAddressSet(trig, TriggerRelationId, pg_trigger->oid);
		add_exact_object_address(&trig, objects);
	}

	/* make the dependency removal visible to the deletion below */
	/*
	 *
	 * 让依赖删除对下面的删除可见
	 */
	CommandCounterIncrement();
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* done */
	/*
	 *
	 * 完成
	 */
	free_object_addresses(objects);
	systable_endscan(scan);
	table_close(tgrel, RowExclusiveLock);
}

/*
 * Before acquiring lock on an index, acquire the same lock on the owning
 * table.
 *
 * 在锁索引之前，先对所属表加上相同的锁。
 */
struct AttachIndexCallbackState
{
	Oid			partitionOid;
	Oid			parentTblOid;
	bool		lockedParentTbl;
};

/*
 * 锁定待挂接的索引之前，先对所属表加上相同的锁。
 */
static void
RangeVarCallbackForAttachIndex(const RangeVar *rv, Oid relOid, Oid oldRelOid,
							   void *arg)
{
	struct AttachIndexCallbackState *state;
	Form_pg_class classform;
	HeapTuple	tuple;

	state = (struct AttachIndexCallbackState *) arg;

	if (!state->lockedParentTbl)
	{
		LockRelationOid(state->parentTblOid, AccessShareLock);
		state->lockedParentTbl = true;
	}

	/*
	 * If we previously locked some other heap, and the name we're looking up
	 * no longer refers to an index on that relation, release the now-useless
	 * lock.  XXX maybe we should do *after* we verify whether the index does
	 * not actually belong to the same relation ...
	 *
	 * 若先前锁了别的堆表，而现在查找的名字不再指向那个关系上的索引，就释放这把已经没用的锁。XXX
	 * 也许应该在核实该索引是否其实不属于同一关系之后再做……
	 */
	if (relOid != oldRelOid && OidIsValid(state->partitionOid))
	{
		UnlockRelationOid(state->partitionOid, AccessShareLock);
		state->partitionOid = InvalidOid;
	}

	/* Didn't find a relation, so no need for locking or permission checks. */
	/*
	 *
	 * 没找到关系，因此不必加锁或做权限检查。
	 */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* concurrently dropped, so nothing to do */
							/*
							 *
							 * 已被并发删除，无需处理
							 */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	if (classform->relkind != RELKIND_PARTITIONED_INDEX &&
		classform->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("\"%s\" is not an index", rv->relname)));
	ReleaseSysCache(tuple);

	/*
	 * Since we need only examine the heap's tupledesc, an access share lock
	 * on it (preventing any DDL) is sufficient.
	 *
	 * 因为我们只需查看堆的元组描述符，对它加访问共享锁（阻止任何 DDL）就够了。
	 */
	state->partitionOid = IndexGetRelation(relOid, false);
	LockRelationOid(state->partitionOid, AccessShareLock);
}

/*
 * ALTER INDEX i1 ATTACH PARTITION i2
 *
 * ALTER INDEX i1 ATTACH PARTITION i2（把索引挂成分区索引）
 */
static ObjectAddress
ATExecAttachPartitionIdx(List **wqueue, Relation parentIdx, RangeVar *name)
{
	Relation	partIdx;
	Relation	partTbl;
	Relation	parentTbl;
	ObjectAddress address;
	Oid			partIdxId;
	Oid			currParent;
	struct AttachIndexCallbackState state;

	/*
	 * We need to obtain lock on the index 'name' to modify it, but we also
	 * need to read its owning table's tuple descriptor -- so we need to lock
	 * both.  To avoid deadlocks, obtain lock on the table before doing so on
	 * the index.  Furthermore, we need to examine the parent table of the
	 * partition, so lock that one too.
	 *
	 * 我们需要锁住名为 name 的索引才能修改它，但也要读它所属表的元组描述符，所以两者都要锁。为避免死锁，先锁表再锁索引。
	 * 此外还要检查分区的父表，所以那张也要锁。
	 */
	state.partitionOid = InvalidOid;
	state.parentTblOid = parentIdx->rd_index->indrelid;
	state.lockedParentTbl = false;
	partIdxId =
		RangeVarGetRelidExtended(name, AccessExclusiveLock, 0,
								 RangeVarCallbackForAttachIndex,
								 &state);
	/* Not there? */
	/*
	 *
	 * 不存在？
	 */
	if (!OidIsValid(partIdxId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", name->relname)));

	/* no deadlock risk: RangeVarGetRelidExtended already acquired the lock */
	/*
	 *
	 * 没有死锁风险：RangeVarGetRelidExtended 已经拿到锁
	 */
	partIdx = relation_open(partIdxId, AccessExclusiveLock);

	/* we already hold locks on both tables, so this is safe: */
	/*
	 *
	 * 两张表的锁都已经持有，所以这是安全的：
	 */
	parentTbl = relation_open(parentIdx->rd_index->indrelid, AccessShareLock);
	partTbl = relation_open(partIdx->rd_index->indrelid, NoLock);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partIdx));

	/* Silently do nothing if already in the right state */
	/*
	 *
	 * 若已经处于正确状态，悄悄什么也不做
	 */
	currParent = partIdx->rd_rel->relispartition ?
		get_partition_parent(partIdxId, false) : InvalidOid;
	if (currParent != RelationGetRelid(parentIdx))
	{
		IndexInfo  *childInfo;
		IndexInfo  *parentInfo;
		AttrMap    *attmap;
		bool		found;
		int			i;
		PartitionDesc partDesc;
		Oid			constraintOid,
					cldConstrId = InvalidOid;

		/*
		 * If this partition already has an index attached, refuse the
		 * operation.
		 *
		 * 若这个分区已经挂了一个索引，拒绝该操作。
		 */
		refuseDupeIndexAttach(parentIdx, partIdx, partTbl);

		if (OidIsValid(currParent))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is already attached to another index.",
							   RelationGetRelationName(partIdx))));

		/* Make sure it indexes a partition of the other index's table */
		/*
		 *
		 * 确保它索引的是另一个索引的表的一个分区
		 */
		partDesc = RelationGetPartitionDesc(parentTbl, true);
		found = false;
		for (i = 0; i < partDesc->nparts; i++)
		{
			if (partDesc->oids[i] == state.partitionOid)
			{
				found = true;
				break;
			}
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("Index \"%s\" is not an index on any partition of table \"%s\".",
							   RelationGetRelationName(partIdx),
							   RelationGetRelationName(parentTbl))));

		/* Ensure the indexes are compatible */
		/*
		 *
		 * 确保这些索引兼容
		 */
		childInfo = BuildIndexInfo(partIdx);
		parentInfo = BuildIndexInfo(parentIdx);
		attmap = build_attrmap_by_name(RelationGetDescr(partTbl),
									   RelationGetDescr(parentTbl),
									   false);
		if (!CompareIndexInfo(childInfo, parentInfo,
							  partIdx->rd_indcollation,
							  parentIdx->rd_indcollation,
							  partIdx->rd_opfamily,
							  parentIdx->rd_opfamily,
							  attmap))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
							RelationGetRelationName(partIdx),
							RelationGetRelationName(parentIdx)),
					 errdetail("The index definitions do not match.")));

		/*
		 * If there is a constraint in the parent, make sure there is one in
		 * the child too.
		 *
		 * 若父索引有约束，确保子索引也有。
		 */
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(parentTbl),
														RelationGetRelid(parentIdx));

		if (OidIsValid(constraintOid))
		{
			cldConstrId = get_relation_idx_constraint_oid(RelationGetRelid(partTbl),
														  partIdxId);
			if (!OidIsValid(cldConstrId))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
								RelationGetRelationName(partIdx),
								RelationGetRelationName(parentIdx)),
						 errdetail("The index \"%s\" belongs to a constraint in table \"%s\" but no constraint exists for index \"%s\".",
								   RelationGetRelationName(parentIdx),
								   RelationGetRelationName(parentTbl),
								   RelationGetRelationName(partIdx))));
		}

		/*
		 * If it's a primary key, make sure the columns in the partition are
		 * NOT NULL.
		 *
		 * 若是主键，确保分区里的列是 NOT NULL。
		 */
		if (parentIdx->rd_index->indisprimary)
			verifyPartitionIndexNotNull(childInfo, partTbl);

		/* All good -- do it */
		/*
		 *
		 * 一切正常，去做
		 */
		IndexSetParentIndex(partIdx, RelationGetRelid(parentIdx));
		if (OidIsValid(constraintOid))
			ConstraintSetParentConstraint(cldConstrId, constraintOid,
										  RelationGetRelid(partTbl));

		free_attrmap(attmap);

		validatePartitionedIndex(parentIdx, parentTbl);
	}

	relation_close(parentTbl, AccessShareLock);
	/* keep these locks till commit */
	/*
	 *
	 * 这些锁保持到提交
	 */
	relation_close(partTbl, NoLock);
	relation_close(partIdx, NoLock);

	return address;
}

/*
 * Verify whether the given partition already contains an index attached
 * to the given partitioned index.  If so, raise an error.
 *
 * 验证给定分区是否已经包含一个挂到给定分区索引上的索引。若是，报错。
 */
static void
refuseDupeIndexAttach(Relation parentIdx, Relation partIdx, Relation partitionTbl)
{
	Oid			existingIdx;

	existingIdx = index_get_partition(partitionTbl,
									  RelationGetRelid(parentIdx));
	if (OidIsValid(existingIdx))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot attach index \"%s\" as a partition of index \"%s\"",
						RelationGetRelationName(partIdx),
						RelationGetRelationName(parentIdx)),
				 errdetail("Another index is already attached for partition \"%s\".",
						   RelationGetRelationName(partitionTbl))));
}

/*
 * Verify whether the set of attached partition indexes to a parent index on
 * a partitioned table is complete.  If it is, mark the parent index valid.
 *
 * 验证挂到分区表上某个父索引的分区索引集合是否完整。若完整，把父索引标成有效。
 *
 * This should be called each time a partition index is attached.
 *
 * 每次挂接分区索引时都应调用这个。
 */
static void
validatePartitionedIndex(Relation partedIdx, Relation partedTbl)
{
	Relation	inheritsRel;
	SysScanDesc scan;
	ScanKeyData key;
	int			tuples = 0;
	HeapTuple	inhTup;
	bool		updated = false;

	Assert(partedIdx->rd_rel->relkind == RELKIND_PARTITIONED_INDEX);

	/*
	 * Scan pg_inherits for this parent index.  Count each valid index we find
	 * (verifying the pg_index entry for each), and if we reach the total
	 * amount we expect, we can mark this parent index as valid.
	 *
	 * 为这个父索引扫描 pg_inherits。每找到一个有效索引就计数（并核实各自的 pg_index 项），若达到预期总数，
	 * 就可以把这个父索引标成有效。
	 */
	inheritsRel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(partedIdx)));
	scan = systable_beginscan(inheritsRel, InheritsParentIndexId, true,
							  NULL, 1, &key);
	while ((inhTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(inhTup);
		HeapTuple	indTup;
		Form_pg_index indexForm;

		indTup = SearchSysCache1(INDEXRELID,
								 ObjectIdGetDatum(inhForm->inhrelid));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u", inhForm->inhrelid);
		indexForm = (Form_pg_index) GETSTRUCT(indTup);
		if (indexForm->indisvalid)
			tuples += 1;
		ReleaseSysCache(indTup);
	}

	/* Done with pg_inherits */
	/*
	 *
	 * pg_inherits 处理完了
	 */
	systable_endscan(scan);
	table_close(inheritsRel, AccessShareLock);

	/*
	 * If we found as many inherited indexes as the partitioned table has
	 * partitions, we're good; update pg_index to set indisvalid.
	 *
	 * 若找到的继承索引数量与分区表的分区数一样多，就没问题；更新 pg_index，设置 indisvalid。
	 */
	if (tuples == RelationGetPartitionDesc(partedTbl, true)->nparts)
	{
		Relation	idxRel;
		HeapTuple	indTup;
		Form_pg_index indexForm;

		idxRel = table_open(IndexRelationId, RowExclusiveLock);
		indTup = SearchSysCacheCopy1(INDEXRELID,
									 ObjectIdGetDatum(RelationGetRelid(partedIdx)));
		if (!HeapTupleIsValid(indTup))
			elog(ERROR, "cache lookup failed for index %u",
				 RelationGetRelid(partedIdx));
		indexForm = (Form_pg_index) GETSTRUCT(indTup);

		indexForm->indisvalid = true;
		updated = true;

		CatalogTupleUpdate(idxRel, &indTup->t_self, indTup);

		table_close(idxRel, RowExclusiveLock);
		heap_freetuple(indTup);
	}

	/*
	 * If this index is in turn a partition of a larger index, validating it
	 * might cause the parent to become valid also.  Try that.
	 *
	 * 若这个索引本身又是更大索引的分区，验证它可能也会使父索引变为有效。试一下。
	 */
	if (updated && partedIdx->rd_rel->relispartition)
	{
		Oid			parentIdxId,
					parentTblId;
		Relation	parentIdx,
					parentTbl;

		/* make sure we see the validation we just did */
		/*
		 *
		 * 确保能看见我们刚做的验证
		 */
		CommandCounterIncrement();

		parentIdxId = get_partition_parent(RelationGetRelid(partedIdx), false);
		parentTblId = get_partition_parent(RelationGetRelid(partedTbl), false);
		parentIdx = relation_open(parentIdxId, AccessExclusiveLock);
		parentTbl = relation_open(parentTblId, AccessExclusiveLock);
		Assert(!parentIdx->rd_index->indisvalid);

		validatePartitionedIndex(parentIdx, parentTbl);

		relation_close(parentIdx, AccessExclusiveLock);
		relation_close(parentTbl, AccessExclusiveLock);
	}
}

/*
 * When attaching an index as a partition of a partitioned index which is a
 * primary key, verify that all the columns in the partition are marked NOT
 * NULL.
 *
 * 把索引作为主键分区索引的分区挂上时，验证分区里的所有列都标成了 NOT NULL。
 */
static void
verifyPartitionIndexNotNull(IndexInfo *iinfo, Relation partition)
{
	for (int i = 0; i < iinfo->ii_NumIndexKeyAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(partition),
											  iinfo->ii_IndexAttrNumbers[i] - 1);

		if (!att->attnotnull)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					errmsg("invalid primary key definition"),
					errdetail("Column \"%s\" of relation \"%s\" is not marked NOT NULL.",
							  NameStr(att->attname),
							  RelationGetRelationName(partition)));
	}
}

/*
 * Return an OID list of constraints that reference the given relation
 * that are marked as having a parent constraints.
 *
 * 返回引用给定关系、且标成有父约束的那些约束的 OID 列表。
 */
static List *
GetParentedForeignKeyRefs(Relation partition)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData key[2];
	List	   *constraints = NIL;

	/*
	 * If no indexes, or no columns are referenceable by FKs, we can avoid the
	 * scan.
	 *
	 * 若没有索引，或没有可被外键引用的列，就可以避免这次扫描。
	 */
	if (RelationGetIndexList(partition) == NIL ||
		bms_is_empty(RelationGetIndexAttrBitmap(partition,
												INDEX_ATTR_BITMAP_KEY)))
		return NIL;

	/* Search for constraints referencing this table */
	/*
	 *
	 * 搜索引用这张表的约束
	 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(partition)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));

	/* XXX This is a seqscan, as we don't have a usable index */
	/*
	 *
	 * XXX 这是顺序扫描，因为没有可用的索引
	 */
	scan = systable_beginscan(pg_constraint, InvalidOid, true, NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 * We only need to process constraints that are part of larger ones.
		 *
		 * 我们只需处理属于更大约束的那些约束。
		 */
		if (!OidIsValid(constrForm->conparentid))
			continue;

		constraints = lappend_oid(constraints, constrForm->oid);
	}

	systable_endscan(scan);
	table_close(pg_constraint, AccessShareLock);

	return constraints;
}

/*
 * During DETACH PARTITION, verify that any foreign keys pointing to the
 * partitioned table would not become invalid.  An error is raised if any
 * referenced values exist.
 *
 * DETACH PARTITION 期间，验证任何指向该分区表的外键不会变得无效。若存在任何被引用的值，就报错。
 */
static void
ATDetachCheckNoForeignKeyRefs(Relation partition)
{
	List	   *constraints;
	ListCell   *cell;

	constraints = GetParentedForeignKeyRefs(partition);

	foreach(cell, constraints)
	{
		Oid			constrOid = lfirst_oid(cell);
		HeapTuple	tuple;
		Form_pg_constraint constrForm;
		Relation	rel;
		Trigger		trig = {0};

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		Assert(OidIsValid(constrForm->conparentid));
		Assert(constrForm->confrelid == RelationGetRelid(partition));

		/* prevent data changes into the referencing table until commit */
		/*
		 *
		 * 直到提交之前，阻止对引用表的数据修改
		 */
		rel = table_open(constrForm->conrelid, ShareLock);

		trig.tgoid = InvalidOid;
		trig.tgname = NameStr(constrForm->conname);
		trig.tgenabled = TRIGGER_FIRES_ON_ORIGIN;
		trig.tgisinternal = true;
		trig.tgconstrrelid = RelationGetRelid(partition);
		trig.tgconstrindid = constrForm->conindid;
		trig.tgconstraint = constrForm->oid;
		trig.tgdeferrable = false;
		trig.tginitdeferred = false;
		/* we needn't fill in remaining fields */
		/*
		 *
		 * 其余字段不必填
		 */

		RI_PartitionRemove_Check(&trig, rel, partition);

		ReleaseSysCache(tuple);

		table_close(rel, NoLock);
	}
}

/*
 * resolve column compression specification to compression method.
 *
 * 把列压缩说明解析成压缩方法。
 */
static char
GetAttributeCompression(Oid atttypid, const char *compression)
{
	char		cmethod;

	if (compression == NULL || strcmp(compression, "default") == 0)
		return InvalidCompressionMethod;

	/*
	 * To specify a nondefault method, the column data type must be toastable.
	 * Note this says nothing about whether the column's attstorage setting
	 * permits compression; we intentionally allow attstorage and
	 * attcompression to be independent.  But with a non-toastable type,
	 * attstorage could not be set to a value that would permit compression.
	 *
	 * 要指定非默认方法，列的数据类型必须是可 toast 的。注意这并没有说该列的 attstorage 设置是否允许压缩；我们有意让
	 * attstorage 和 attcompression 互相独立。但对不可 toast 的类型，attstorage
	 * 无法设成允许压缩的值。
	 *
	 * We don't actually need to enforce this, since nothing bad would happen
	 * if attcompression were non-default; it would never be consulted.  But
	 * it seems more user-friendly to complain about a certainly-useless
	 * attempt to set the property.
	 *
	 * 我们其实不必强制这一点，因为 attcompression 不是默认值也不会有坏事发生；它根本不会被查阅。
	 * 但抱怨一次肯定没用的属性设置，对用户更友好。
	 */
	if (!TypeIsToastable(atttypid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s does not support compression",
						format_type_be(atttypid))));

	cmethod = CompressionNameToMethod(compression);
	if (!CompressionMethodIsValid(cmethod))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid compression method \"%s\"", compression)));

	return cmethod;
}

/*
 * resolve column storage specification
 *
 * 解析列存储说明
 */
static char
GetAttributeStorage(Oid atttypid, const char *storagemode)
{
	char		cstorage = 0;

	if (pg_strcasecmp(storagemode, "plain") == 0)
		cstorage = TYPSTORAGE_PLAIN;
	else if (pg_strcasecmp(storagemode, "external") == 0)
		cstorage = TYPSTORAGE_EXTERNAL;
	else if (pg_strcasecmp(storagemode, "extended") == 0)
		cstorage = TYPSTORAGE_EXTENDED;
	else if (pg_strcasecmp(storagemode, "main") == 0)
		cstorage = TYPSTORAGE_MAIN;
	else if (pg_strcasecmp(storagemode, "default") == 0)
		cstorage = get_typstorage(atttypid);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid storage type \"%s\"",
						storagemode)));

	/*
	 * safety check: do not allow toasted storage modes unless column datatype
	 * is TOAST-aware.
	 *
	 * 安全检查：除非列的数据类型支持 TOAST，否则不允许 toasted 存储模式。
	 */
	if (!(cstorage == TYPSTORAGE_PLAIN || TypeIsToastable(atttypid)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s can only have storage PLAIN",
						format_type_be(atttypid))));

	return cstorage;
}
