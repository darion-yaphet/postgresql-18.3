/*-------------------------------------------------------------------------
 *
 * amapi.h
 *	  API for Postgres index access methods.
 *
 * Copyright (c) 2015-2025, PostgreSQL Global Development Group
 *
 * src/include/access/amapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMAPI_H
#define AMAPI_H

#include "access/cmptype.h"
#include "access/genam.h"
#include "access/stratnum.h"

/*
 * We don't wish to include planner header files here, since most of an index
 * AM's implementation isn't concerned with those data structures.  To allow
 * declaring amcostestimate_function here, use forward struct references.
 */

/*
 * 我们不希望在这里包含规划器（planner）的头文件，因为一个索引访问方法（AM）的
 * 实现大部分都与那些数据结构无关。为了能在这里声明 amcostestimate_function，
 * 我们使用前向结构体引用（forward struct reference）。
 */
struct PlannerInfo;
struct IndexPath;

/* Likewise, this file shouldn't depend on execnodes.h. */

/* 同理，本文件不应依赖 execnodes.h。 */
struct IndexInfo;


/*
 * Properties for amproperty API.  This list covers properties known to the
 * core code, but an index AM can define its own properties, by matching the
 * string property name.
 */

/*
 * amproperty API 使用的各种属性。此列表涵盖了核心代码已知的属性，但一个索引
 * 访问方法（AM）可以通过匹配字符串属性名来定义自己的属性。
 */
typedef enum IndexAMProperty
{
	AMPROP_UNKNOWN = 0,			/* anything not known to core code */

	/* 核心代码不认识的任何属性 */
	AMPROP_ASC,					/* column properties */

	/* 列属性 */
	AMPROP_DESC,
	AMPROP_NULLS_FIRST,
	AMPROP_NULLS_LAST,
	AMPROP_ORDERABLE,
	AMPROP_DISTANCE_ORDERABLE,
	AMPROP_RETURNABLE,
	AMPROP_SEARCH_ARRAY,
	AMPROP_SEARCH_NULLS,
	AMPROP_CLUSTERABLE,			/* index properties */

	/* 索引属性 */
	AMPROP_INDEX_SCAN,
	AMPROP_BITMAP_SCAN,
	AMPROP_BACKWARD_SCAN,
	AMPROP_CAN_ORDER,			/* AM properties */

	/* 访问方法（AM）属性 */
	AMPROP_CAN_UNIQUE,
	AMPROP_CAN_MULTI_COL,
	AMPROP_CAN_EXCLUDE,
	AMPROP_CAN_INCLUDE,
} IndexAMProperty;

/*
 * We use lists of this struct type to keep track of both operators and
 * support functions while building or adding to an opclass or opfamily.
 * amadjustmembers functions receive lists of these structs, and are allowed
 * to alter their "ref" fields.
 *
 * The "ref" fields define how the pg_amop or pg_amproc entry should depend
 * on the associated objects (that is, which dependency type to use, and
 * which opclass or opfamily it should depend on).
 *
 * If ref_is_hard is true, the entry will have a NORMAL dependency on the
 * operator or support func, and an INTERNAL dependency on the opclass or
 * opfamily.  This forces the opclass or opfamily to be dropped if the
 * operator or support func is dropped, and requires the CASCADE option
 * to do so.  Nor will ALTER OPERATOR FAMILY DROP be allowed.  This is
 * the right behavior for objects that are essential to an opclass.
 *
 * If ref_is_hard is false, the entry will have an AUTO dependency on the
 * operator or support func, and also an AUTO dependency on the opclass or
 * opfamily.  This allows ALTER OPERATOR FAMILY DROP, and causes that to
 * happen automatically if the operator or support func is dropped.  This
 * is the right behavior for inessential ("loose") objects.
 *
 * We also make dependencies on lefttype/righttype, of the same strength as
 * the dependency on the operator or support func, unless these dependencies
 * are redundant with the dependency on the operator or support func.
 */

/*
 * 在构建运算符类（opclass）或运算符族（opfamily），或向其中添加成员时，我们使用
 * 由该结构体类型组成的列表来同时跟踪运算符和支持函数。amadjustmembers 函数会接收
 * 由这些结构体组成的列表，并被允许修改它们的 “ref” 字段。
 *
 * 这些 “ref” 字段定义了 pg_amop 或 pg_amproc 条目应如何依赖于相关对象
 *（即：使用哪种依赖类型，以及它应依赖于哪个运算符类或运算符族）。
 *
 * 如果 ref_is_hard 为真，则该条目会对运算符或支持函数具有 NORMAL（普通）依赖，
 * 并对运算符类或运算符族具有 INTERNAL（内部）依赖。这会导致：一旦运算符或支持函数
 * 被删除，就会强制删除该运算符类或运算符族，并且必须使用 CASCADE 选项才能删除。
 * 同时也不允许使用 ALTER OPERATOR FAMILY DROP。对于那些对运算符类至关重要的对象，
 * 这是正确的行为。
 *
 * 如果 ref_is_hard 为假，则该条目会对运算符或支持函数具有 AUTO（自动）依赖，
 * 同时也对运算符类或运算符族具有 AUTO 依赖。这样就允许使用 ALTER OPERATOR FAMILY DROP，
 * 并且在运算符或支持函数被删除时会自动完成删除。对于那些非必要的（“松散的”）对象，
 * 这是正确的行为。
 *
 * 我们还会对 lefttype/righttype 建立依赖，其强度与对运算符或支持函数的依赖相同，
 * 除非这些依赖与对运算符或支持函数的依赖是冗余的。
 */
typedef struct OpFamilyMember
{
	bool		is_func;		/* is this an operator, or support func? */

	/* 这是一个运算符，还是一个支持函数？ */
	Oid			object;			/* operator or support func's OID */

	/* 运算符或支持函数的 OID */
	int			number;			/* strategy or support func number */

	/* 策略号或支持函数号 */
	Oid			lefttype;		/* lefttype */

	/* 左操作数类型 */
	Oid			righttype;		/* righttype */

	/* 右操作数类型 */
	Oid			sortfamily;		/* ordering operator's sort opfamily, or 0 */

	/* 排序运算符的排序运算符族，若无则为 0 */
	bool		ref_is_hard;	/* hard or soft dependency? */

	/* 硬依赖还是软依赖？ */
	bool		ref_is_family;	/* is dependency on opclass or opfamily? */

	/* 依赖对象是运算符类还是运算符族？ */
	Oid			refobjid;		/* OID of opclass or opfamily */

	/* 运算符类或运算符族的 OID */
} OpFamilyMember;


/*
 * Callback function signatures --- see indexam.sgml for more info.
 */

/*
 * 回调函数签名 —— 更多信息参见 indexam.sgml。
 */

/* translate AM-specific strategies to general operator types */

/* 将访问方法（AM）特定的策略转换为通用的运算符类型 */
typedef CompareType (*amtranslate_strategy_function) (StrategyNumber strategy, Oid opfamily);

/* translate general operator types to AM-specific strategies */

/* 将通用的运算符类型转换为访问方法（AM）特定的策略 */
typedef StrategyNumber (*amtranslate_cmptype_function) (CompareType cmptype, Oid opfamily);

/* build new index */

/* 构建新的索引 */
typedef IndexBuildResult *(*ambuild_function) (Relation heapRelation,
											   Relation indexRelation,
											   struct IndexInfo *indexInfo);

/* build empty index */

/* 构建空的索引 */
typedef void (*ambuildempty_function) (Relation indexRelation);

/* insert this tuple */

/* 插入这个元组 */
typedef bool (*aminsert_function) (Relation indexRelation,
								   Datum *values,
								   bool *isnull,
								   ItemPointer heap_tid,
								   Relation heapRelation,
								   IndexUniqueCheck checkUnique,
								   bool indexUnchanged,
								   struct IndexInfo *indexInfo);

/* cleanup after insert */

/* 插入之后进行清理 */
typedef void (*aminsertcleanup_function) (Relation indexRelation,
										  struct IndexInfo *indexInfo);

/* bulk delete */

/* 批量删除 */
typedef IndexBulkDeleteResult *(*ambulkdelete_function) (IndexVacuumInfo *info,
														 IndexBulkDeleteResult *stats,
														 IndexBulkDeleteCallback callback,
														 void *callback_state);

/* post-VACUUM cleanup */

/* VACUUM 之后的清理 */
typedef IndexBulkDeleteResult *(*amvacuumcleanup_function) (IndexVacuumInfo *info,
															IndexBulkDeleteResult *stats);

/* can indexscan return IndexTuples? */

/* 索引扫描是否能够返回 IndexTuple？ */
typedef bool (*amcanreturn_function) (Relation indexRelation, int attno);

/* estimate cost of an indexscan */

/* 估算一次索引扫描的代价 */
typedef void (*amcostestimate_function) (struct PlannerInfo *root,
										 struct IndexPath *path,
										 double loop_count,
										 Cost *indexStartupCost,
										 Cost *indexTotalCost,
										 Selectivity *indexSelectivity,
										 double *indexCorrelation,
										 double *indexPages);

/* estimate height of a tree-structured index
 *
 * XXX This just computes a value that is later used by amcostestimate.  This
 * API could be expanded to support passing more values if the need arises.
 */

/* 估算树状结构索引的高度
 *
 * XXX 这里只是计算一个之后供 amcostestimate 使用的值。如果有需要，这个 API
 * 可以扩展为支持传递更多的值。
 */
typedef int (*amgettreeheight_function) (Relation rel);

/* parse index reloptions */

/* 解析索引的关系选项（reloptions） */
typedef bytea *(*amoptions_function) (Datum reloptions,
									  bool validate);

/* report AM, index, or index column property */

/* 报告访问方法（AM）、索引或索引列的属性 */
typedef bool (*amproperty_function) (Oid index_oid, int attno,
									 IndexAMProperty prop, const char *propname,
									 bool *res, bool *isnull);

/* name of phase as used in progress reporting */

/* 进度报告中使用的阶段名称 */
typedef char *(*ambuildphasename_function) (int64 phasenum);

/* validate definition of an opclass for this AM */

/* 校验该访问方法（AM）的某个运算符类的定义 */
typedef bool (*amvalidate_function) (Oid opclassoid);

/* validate operators and support functions to be added to an opclass/family */

/* 校验将要添加到某个运算符类/运算符族的运算符和支持函数 */
typedef void (*amadjustmembers_function) (Oid opfamilyoid,
										  Oid opclassoid,
										  List *operators,
										  List *functions);

/* prepare for index scan */

/* 为索引扫描做准备 */
typedef IndexScanDesc (*ambeginscan_function) (Relation indexRelation,
											   int nkeys,
											   int norderbys);

/* (re)start index scan */

/* （重新）开始索引扫描 */
typedef void (*amrescan_function) (IndexScanDesc scan,
								   ScanKey keys,
								   int nkeys,
								   ScanKey orderbys,
								   int norderbys);

/* next valid tuple */

/* 下一个有效的元组 */
typedef bool (*amgettuple_function) (IndexScanDesc scan,
									 ScanDirection direction);

/* fetch all valid tuples */

/* 获取所有有效的元组 */
typedef int64 (*amgetbitmap_function) (IndexScanDesc scan,
									   TIDBitmap *tbm);

/* end index scan */

/* 结束索引扫描 */
typedef void (*amendscan_function) (IndexScanDesc scan);

/* mark current scan position */

/* 标记当前的扫描位置 */
typedef void (*ammarkpos_function) (IndexScanDesc scan);

/* restore marked scan position */

/* 恢复到已标记的扫描位置 */
typedef void (*amrestrpos_function) (IndexScanDesc scan);

/*
 * Callback function signatures - for parallel index scans.
 */

/*
 * 回调函数签名 —— 用于并行索引扫描。
 */

/* estimate size of parallel scan descriptor */

/* 估算并行扫描描述符的大小 */
typedef Size (*amestimateparallelscan_function) (Relation indexRelation,
												 int nkeys, int norderbys);

/* prepare for parallel index scan */

/* 为并行索引扫描做准备 */
typedef void (*aminitparallelscan_function) (void *target);

/* (re)start parallel index scan */

/* （重新）开始并行索引扫描 */
typedef void (*amparallelrescan_function) (IndexScanDesc scan);

/*
 * API struct for an index AM.  Note this must be stored in a single palloc'd
 * chunk of memory.
 */

/*
 * 索引访问方法（AM）的 API 结构体。注意，它必须存放在单个 palloc 分配的内存块中。
 */
typedef struct IndexAmRoutine
{
	NodeTag		type;

	/*
	 * Total number of strategies (operators) by which we can traverse/search
	 * this AM.  Zero if AM does not have a fixed set of strategy assignments.
	 */

	/*
	 * 我们可用来遍历/搜索该访问方法的策略（运算符）的总数。如果该访问方法没有
	 * 固定的策略分配集合，则为 0。
	 */
	uint16		amstrategies;
	/* total number of support functions that this AM uses */

	/* 该访问方法所使用的支持函数的总数 */
	uint16		amsupport;
	/* opclass options support function number or 0 */

	/* 运算符类选项支持函数的编号，若无则为 0 */
	uint16		amoptsprocnum;
	/* does AM support ORDER BY indexed column's value? */

	/* 该访问方法是否支持按被索引列的值进行 ORDER BY？ */
	bool		amcanorder;
	/* does AM support ORDER BY result of an operator on indexed column? */

	/* 该访问方法是否支持按作用于被索引列的某个运算符的结果进行 ORDER BY？ */
	bool		amcanorderbyop;
	/* does AM support hashing using API consistent with the hash AM? */

	/* 该访问方法是否支持使用与 hash 访问方法一致的 API 进行哈希？ */
	bool		amcanhash;
	/* do operators within an opfamily have consistent equality semantics? */

	/* 同一运算符族内的运算符是否具有一致的相等（equality）语义？ */
	bool		amconsistentequality;
	/* do operators within an opfamily have consistent ordering semantics? */

	/* 同一运算符族内的运算符是否具有一致的排序（ordering）语义？ */
	bool		amconsistentordering;
	/* does AM support backward scanning? */

	/* 该访问方法是否支持反向扫描（backward scanning）？ */
	bool		amcanbackward;
	/* does AM support UNIQUE indexes? */

	/* 该访问方法是否支持唯一（UNIQUE）索引？ */
	bool		amcanunique;
	/* does AM support multi-column indexes? */

	/* 该访问方法是否支持多列（multi-column）索引？ */
	bool		amcanmulticol;
	/* does AM require scans to have a constraint on the first index column? */

	/* 该访问方法是否要求扫描必须对第一个索引列带有约束条件？ */
	bool		amoptionalkey;
	/* does AM handle ScalarArrayOpExpr quals? */

	/* 该访问方法是否能够处理 ScalarArrayOpExpr 类型的限定条件（qual）？ */
	bool		amsearcharray;
	/* does AM handle IS NULL/IS NOT NULL quals? */

	/* 该访问方法是否能够处理 IS NULL/IS NOT NULL 类型的限定条件（qual）？ */
	bool		amsearchnulls;
	/* can index storage data type differ from column data type? */

	/* 索引的存储数据类型是否可以与列的数据类型不同？ */
	bool		amstorage;
	/* can an index of this type be clustered on? */

	/* 该类型的索引是否可以用于聚簇（CLUSTER）？ */
	bool		amclusterable;
	/* does AM handle predicate locks? */

	/* 该访问方法是否处理谓词锁（predicate lock）？ */
	bool		ampredlocks;
	/* does AM support parallel scan? */

	/* 该访问方法是否支持并行扫描？ */
	bool		amcanparallel;
	/* does AM support parallel build? */

	/* 该访问方法是否支持并行构建？ */
	bool		amcanbuildparallel;
	/* does AM support columns included with clause INCLUDE? */

	/* 该访问方法是否支持通过 INCLUDE 子句附带的列（included column）？ */
	bool		amcaninclude;
	/* does AM use maintenance_work_mem? */

	/* 该访问方法是否使用 maintenance_work_mem？ */
	bool		amusemaintenanceworkmem;
	/* does AM store tuple information only at block granularity? */

	/* 该访问方法是否仅以块（block）为粒度存储元组信息？ */
	bool		amsummarizing;
	/* OR of parallel vacuum flags.  See vacuum.h for flags. */

	/* 并行 vacuum 标志位的按位或结果。标志位定义参见 vacuum.h。 */
	uint8		amparallelvacuumoptions;
	/* type of data stored in index, or InvalidOid if variable */

	/* 索引中所存储数据的类型；若为可变类型则为 InvalidOid */
	Oid			amkeytype;

	/*
	 * If you add new properties to either the above or the below lists, then
	 * they should also (usually) be exposed via the property API (see
	 * IndexAMProperty at the top of the file, and utils/adt/amutils.c).
	 */

	/*
	 * 如果你向上面或下面的列表中添加了新的属性，那么它们（通常）也应通过属性
	 * API 对外暴露（参见本文件顶部的 IndexAMProperty，以及 utils/adt/amutils.c）。
	 */

	/* interface functions */

	/* 接口函数 */
	ambuild_function ambuild;
	ambuildempty_function ambuildempty;
	aminsert_function aminsert;
	aminsertcleanup_function aminsertcleanup;	/* can be NULL */

	/* 可以为 NULL */
	ambulkdelete_function ambulkdelete;
	amvacuumcleanup_function amvacuumcleanup;
	amcanreturn_function amcanreturn;	/* can be NULL */

	/* 可以为 NULL */
	amcostestimate_function amcostestimate;
	amgettreeheight_function amgettreeheight;	/* can be NULL */

	/* 可以为 NULL */
	amoptions_function amoptions;
	amproperty_function amproperty; /* can be NULL */

	/* 可以为 NULL */
	ambuildphasename_function ambuildphasename; /* can be NULL */

	/* 可以为 NULL */
	amvalidate_function amvalidate;
	amadjustmembers_function amadjustmembers;	/* can be NULL */

	/* 可以为 NULL */
	ambeginscan_function ambeginscan;
	amrescan_function amrescan;
	amgettuple_function amgettuple; /* can be NULL */

	/* 可以为 NULL */
	amgetbitmap_function amgetbitmap;	/* can be NULL */

	/* 可以为 NULL */
	amendscan_function amendscan;
	ammarkpos_function ammarkpos;	/* can be NULL */

	/* 可以为 NULL */
	amrestrpos_function amrestrpos; /* can be NULL */

	/* 可以为 NULL */

	/* interface functions to support parallel index scans */

	/* 用于支持并行索引扫描的接口函数 */
	amestimateparallelscan_function amestimateparallelscan; /* can be NULL */

	/* 可以为 NULL */
	aminitparallelscan_function aminitparallelscan; /* can be NULL */

	/* 可以为 NULL */
	amparallelrescan_function amparallelrescan; /* can be NULL */

	/* 可以为 NULL */

	/* interface functions to support planning */

	/* 用于支持规划（planning）的接口函数 */
	amtranslate_strategy_function amtranslatestrategy;	/* can be NULL */

	/* 可以为 NULL */
	amtranslate_cmptype_function amtranslatecmptype;	/* can be NULL */

	/* 可以为 NULL */
} IndexAmRoutine;


/* Functions in access/index/amapi.c */

/* access/index/amapi.c 中的函数 */

/*
 * GetIndexAmRoutine
 *		Call the given index AM's handler function and return the resulting
 *		IndexAmRoutine struct, which describes the AM's capabilities and
 *		interface functions.
 */

/*
 * GetIndexAmRoutine
 *		调用给定索引访问方法（AM）的处理器（handler）函数，并返回由其生成的
 *		IndexAmRoutine 结构体，该结构体描述了该访问方法的能力和接口函数。
 */
extern IndexAmRoutine *GetIndexAmRoutine(Oid amhandler);

/*
 * GetIndexAmRoutineByAmId
 *		Look up the handler for the index AM identified by amoid and return its
 *		IndexAmRoutine.  If noerror is true, return NULL instead of raising an
 *		error when the OID is not a valid index AM.
 */

/*
 * GetIndexAmRoutineByAmId
 *		查找由 amoid 标识的索引访问方法（AM）的处理器，并返回其 IndexAmRoutine。
 *		如果 noerror 为真，则当该 OID 不是一个有效的索引访问方法时返回 NULL，
 *		而不是抛出错误。
 */
extern IndexAmRoutine *GetIndexAmRoutineByAmId(Oid amoid, bool noerror);

/*
 * IndexAmTranslateStrategy
 *		Translate an AM-specific strategy number (within the given opfamily of
 *		the AM identified by amoid) into a general CompareType, using the AM's
 *		amtranslatestrategy callback.
 */

/*
 * IndexAmTranslateStrategy
 *		借助访问方法的 amtranslatestrategy 回调，将某个访问方法特定的策略号
 *		（位于由 amoid 标识的访问方法的给定运算符族中）转换为通用的 CompareType。
 */
extern CompareType IndexAmTranslateStrategy(StrategyNumber strategy, Oid amoid, Oid opfamily, bool missing_ok);

/*
 * IndexAmTranslateCompareType
 *		Translate a general CompareType into an AM-specific strategy number
 *		(within the given opfamily of the AM identified by amoid), using the
 *		AM's amtranslatecmptype callback.
 */

/*
 * IndexAmTranslateCompareType
 *		借助访问方法的 amtranslatecmptype 回调，将通用的 CompareType 转换为某个
 *		访问方法特定的策略号（位于由 amoid 标识的访问方法的给定运算符族中）。
 */
extern StrategyNumber IndexAmTranslateCompareType(CompareType cmptype, Oid amoid, Oid opfamily, bool missing_ok);

#endif							/* AMAPI_H */
