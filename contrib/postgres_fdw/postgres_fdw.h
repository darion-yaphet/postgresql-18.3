/*-------------------------------------------------------------------------
 *
 * postgres_fdw.h
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/postgres_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_FDW_H
#define POSTGRES_FDW_H

#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "libpq-fe.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "utils/relcache.h"

/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * postgres_fdw foreign table.  For a baserel, this struct is created by
 * postgresGetForeignRelSize, although some fields are not filled till later.
 * postgresGetForeignJoinPaths creates it for a joinrel, and
 * postgresGetForeignUpperPaths creates it for an upperrel.
 *
 * FDW 特定的规划器信息保存在 postgres_fdw 外部表的 RelOptInfo.fdw_private 中。  对于 Baserel，此结构是由 postgresGetForeignRelSize 创建的，尽管某些字段直到稍后才填充。 postgresGetForeignJoinPaths 为 joinrel 创建它，postgresGetForeignUpperPaths 为 upperrel 创建它。
 */
typedef struct PgFdwRelationInfo
{
	/*
	 * True means that the relation can be pushed down. Always true for simple
	 * foreign scan.
	 *
	 * True 意味着关系可以下推。对于简单的外国扫描始终如此。
	 */
	bool		pushdown_safe;

	/*
	 * Restriction clauses, divided into safe and unsafe to pushdown subsets.
	 * All entries in these lists should have RestrictInfo wrappers; that
	 * improves efficiency of selectivity and cost estimation.
	 *
	 * 限制子句，分为安全和不安全的下推子集。这些列表中的所有条目都应该有 RestrictInfo 包装器；这提高了选择性和成本估算的效率。
	 */
	List	   *remote_conds;
	List	   *local_conds;

	/* Actual remote restriction clauses for scan (sans RestrictInfos)
	 *
	 * 扫描的实际远程限制子句（无 RestrictInfos）
	 */
	List	   *final_remote_exprs;

	/* Bitmap of attr numbers we need to fetch from the remote server.
	 *
	 * 我们需要从远程服务器获取的 attr 数字的位图。
	 */
	Bitmapset  *attrs_used;

	/* True means that the query_pathkeys is safe to push down
	 *
	 * True 意味着 query_pathkeys 可以安全地下推
	 */
	bool		qp_is_pushdown_safe;

	/* Cost and selectivity of local_conds.
	 *
	 * local_conds 的成本和选择性。
	 */
	QualCost	local_conds_cost;
	Selectivity local_conds_sel;

	/* Selectivity of join conditions
	 *
	 * 连接条件的选择性
	 */
	Selectivity joinclause_sel;

	/* Estimated size and cost for a scan, join, or grouping/aggregation.
	 *
	 * 扫描、连接或分组/聚合的估计大小和成本。
	 */
	double		rows;
	int			width;
	int			disabled_nodes;
	Cost		startup_cost;
	Cost		total_cost;

	/*
	 * Estimated number of rows fetched from the foreign server, and costs
	 * excluding costs for transferring those rows from the foreign server.
	 * These are only used by estimate_path_cost_size().
	 *
	 * 从外部服务器获取的估计行数，以及成本（不包括从外部服务器传输这些行的成本）。这些仅由estimate_path_cost_size()使用。
	 */
	double		retrieved_rows;
	Cost		rel_startup_cost;
	Cost		rel_total_cost;

	/* Options extracted from catalogs.
	 *
	 * 从目录中提取的选项。
	 */
	bool		use_remote_estimate;
	Cost		fdw_startup_cost;
	Cost		fdw_tuple_cost;
	List	   *shippable_extensions;	/* OIDs of shippable extensions */
	bool		async_capable;

	/* Cached catalog information.
	 *
	 * 缓存的目录信息。
	 */
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;			/* only set in use_remote_estimate mode */

	int			fetch_size;		/* fetch size for this remote table */

	/*
	 * Name of the relation, for use while EXPLAINing ForeignScan.  It is used
	 * for join and upper relations but is set for all relations.  For a base
	 * relation, this is really just the RT index as a string; we convert that
	 * while producing EXPLAIN output.  For join and upper relations, the name
	 * indicates which base foreign tables are included and the join type or
	 * aggregation type used.
	 *
	 * 关系的名称，在解释ForeignScan时使用。  它用于连接和上层关系，但为所有关系设置。  对于基本关系，这实际上只是字符串形式的 RT 索引；我们在生成 EXPLAIN 输出时对其进行转换。  对于联接和上层关系，名称指示包含哪些基本外部表以及使用的联接类型或聚合类型。
	 */
	char	   *relation_name;

	/* Join information
	 *
	 * 加盟信息
	 */
	RelOptInfo *outerrel;
	RelOptInfo *innerrel;
	JoinType	jointype;
	/* joinclauses contains only JOIN/ON conditions for an outer join
	 *
	 * joinclauses 仅包含外连接的 JOIN/ON 条件
	 */
	List	   *joinclauses;	/* List of RestrictInfo */

	/* Upper relation information
	 *
	 * 上层关系信息
	 */
	UpperRelationKind stage;

	/* Grouping information
	 *
	 * 分组信息
	 */
	List	   *grouped_tlist;

	/* Subquery information
	 *
	 * 子查询信息
	 */
	bool		make_outerrel_subquery; /* do we deparse outerrel as a
										 * subquery? */
	bool		make_innerrel_subquery; /* do we deparse innerrel as a
										 * subquery? */
	Relids		lower_subquery_rels;	/* all relids appearing in lower
										 * subqueries */
	Relids		hidden_subquery_rels;	/* relids, which can't be referred to
										 * from upper relations, used
										 * internally for equivalence member
										 *
										 * 来自上层关系，内部用于等效成员
										 * search */

	/*
	 * Index of the relation.  It is used to create an alias to a subquery
	 * representing the relation.
	 *
	 * 关系的索引。  它用于为表示关系的子查询创建别名。
	 */
	int			relation_index;
} PgFdwRelationInfo;

/*
 * Extra control information relating to a connection.
 *
 * 与连接相关的额外控制信息。
 */
typedef struct PgFdwConnState
{
	AsyncRequest *pendingAreq;	/* pending async request */
} PgFdwConnState;

/*
 * Method used by ANALYZE to sample remote rows.
 *
 * ANALYZE 用于对远程行进行采样的方法。
 */
typedef enum PgFdwSamplingMethod
{
	ANALYZE_SAMPLE_OFF,			/* no remote sampling */
	ANALYZE_SAMPLE_AUTO,		/* choose by server version */
	ANALYZE_SAMPLE_RANDOM,		/* remote random() */
	ANALYZE_SAMPLE_SYSTEM,		/* TABLESAMPLE system */
	ANALYZE_SAMPLE_BERNOULLI,	/* TABLESAMPLE bernoulli */
} PgFdwSamplingMethod;

/* in postgres_fdw.c
 *
 * 在 postgres_fdw.c 中
 */
extern int	set_transmission_modes(void);
extern void reset_transmission_modes(int nestlevel);
extern void process_pending_request(AsyncRequest *areq);

/* in connection.c
 *
 * 在连接.c
 */
extern PGconn *GetConnection(UserMapping *user, bool will_prep_stmt,
							 PgFdwConnState **state);
extern void ReleaseConnection(PGconn *conn);
extern unsigned int GetCursorNumber(PGconn *conn);
extern unsigned int GetPrepStmtNumber(PGconn *conn);
extern void do_sql_command(PGconn *conn, const char *sql);
extern PGresult *pgfdw_get_result(PGconn *conn);
extern PGresult *pgfdw_exec_query(PGconn *conn, const char *query,
								  PgFdwConnState *state);
extern void pgfdw_report_error(int elevel, PGresult *res, PGconn *conn,
							   bool clear, const char *sql);

/* in option.c
 *
 * 在选项.c中
 */
extern int	ExtractConnectionOptions(List *defelems,
									 const char **keywords,
									 const char **values);
extern List *ExtractExtensionList(const char *extensionsString,
								  bool warnOnMissing);
extern char *process_pgfdw_appname(const char *appname);
extern char *pgfdw_application_name;

/* in deparse.c
 *
 * 在deparse.c
 */
extern void classifyConditions(PlannerInfo *root,
							   RelOptInfo *baserel,
							   List *input_conds,
							   List **remote_conds,
							   List **local_conds);
extern bool is_foreign_expr(PlannerInfo *root,
							RelOptInfo *baserel,
							Expr *expr);
extern bool is_foreign_param(PlannerInfo *root,
							 RelOptInfo *baserel,
							 Expr *expr);
extern bool is_foreign_pathkey(PlannerInfo *root,
							   RelOptInfo *baserel,
							   PathKey *pathkey);
extern void deparseInsertSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *targetAttrs, bool doNothing,
							 List *withCheckOptionList, List *returningList,
							 List **retrieved_attrs, int *values_end_len);
extern void rebuildInsertSql(StringInfo buf, Relation rel,
							 char *orig_query, List *target_attrs,
							 int values_end_len, int num_params,
							 int num_rows);
extern void deparseUpdateSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *targetAttrs,
							 List *withCheckOptionList, List *returningList,
							 List **retrieved_attrs);
extern void deparseDirectUpdateSql(StringInfo buf, PlannerInfo *root,
								   Index rtindex, Relation rel,
								   RelOptInfo *foreignrel,
								   List *targetlist,
								   List *targetAttrs,
								   List *remote_conds,
								   List **params_list,
								   List *returningList,
								   List **retrieved_attrs);
extern void deparseDeleteSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *returningList,
							 List **retrieved_attrs);
extern void deparseDirectDeleteSql(StringInfo buf, PlannerInfo *root,
								   Index rtindex, Relation rel,
								   RelOptInfo *foreignrel,
								   List *remote_conds,
								   List **params_list,
								   List *returningList,
								   List **retrieved_attrs);
extern void deparseAnalyzeSizeSql(StringInfo buf, Relation rel);
extern void deparseAnalyzeInfoSql(StringInfo buf, Relation rel);
extern void deparseAnalyzeSql(StringInfo buf, Relation rel,
							  PgFdwSamplingMethod sample_method,
							  double sample_frac,
							  List **retrieved_attrs);
extern void deparseTruncateSql(StringInfo buf,
							   List *rels,
							   DropBehavior behavior,
							   bool restart_seqs);
extern void deparseStringLiteral(StringInfo buf, const char *val);
extern EquivalenceMember *find_em_for_rel(PlannerInfo *root,
										  EquivalenceClass *ec,
										  RelOptInfo *rel);
extern EquivalenceMember *find_em_for_rel_target(PlannerInfo *root,
												 EquivalenceClass *ec,
												 RelOptInfo *rel);
extern List *build_tlist_to_deparse(RelOptInfo *foreignrel);
extern void deparseSelectStmtForRel(StringInfo buf, PlannerInfo *root,
									RelOptInfo *rel, List *tlist,
									List *remote_conds, List *pathkeys,
									bool has_final_sort, bool has_limit,
									bool is_subquery,
									List **retrieved_attrs, List **params_list);
extern const char *get_jointype_name(JoinType jointype);

/* in shippable.c
 *
 * 在可发货.c
 */
extern bool is_builtin(Oid objectId);
extern bool is_shippable(Oid objectId, Oid classId, PgFdwRelationInfo *fpinfo);

#endif							/* POSTGRES_FDW_H */
