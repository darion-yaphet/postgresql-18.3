/*-------------------------------------------------------------------------
 *
 * nodeAgg.h
 *	  prototypes for nodeAgg.c
 *    nodeAgg.c 的原型。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeAgg.h
 *
 * Core Flow:
 * 核心流程：
 * 1. Initialization: ExecInitAgg sets up metadata for each aggregate (Aggref) and grouping set,
 *    including transition functions, final functions, and hash tables if needed.
 *    初始化：ExecInitAgg 为每个聚合（Aggref）和分组集设置元数据，包括转换函数、最终函数以及所需的哈希表。
 * 2. Processing:
 *    处理：
 *    a. Hashed: Input tuples are distributed into hash table buckets based on grouping keys.
 *       哈希方式：输入元组根据分组键分布到哈希表桶中。
 *    b. Sorted/Plain: Tuples are processed in sequence, and aggregate states are updated per group.
 *       排序/普通方式：元组按顺序处理，每个组的聚合状态都会更新。
 * 3. Transitions: For each input tuple, transfn/combinefn is called to update the internal state.
 *    转换：对于每个输入元组，调用 transfn/combinefn 来更新内部状态。
 * 4. Finalization: Once a group is complete, finalfn is called to convert the internal state
 *    into the final output value.
 *    最终化：一旦一个组处理完成，调用 finalfn 将内部状态转换为最终输出值。
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAGG_H
#define NODEAGG_H

#include "access/parallel.h"
#include "nodes/execnodes.h"


/*
 * AggStatePerTransData - per aggregate state value information
 * AggStatePerTransData - 每个聚合状态值的信息
 *
 * Working state for updating the aggregate's state value, by calling the
 * transition function with an input row. This struct does not store the
 * information needed to produce the final aggregate result from the transition
 * state, that's stored in AggStatePerAggData instead. This separation allows
 * multiple aggregate results to be produced from a single state value.
 * 通过使用输入行调用转换函数来更新聚合状态值的运行状态。此结构不存储从转换状态
 * 生成最终聚合结果所需的信息，该信息存储在 AggStatePerAggData 中。
 * 这种分离允许从单个状态值产生多个聚合结果。
 */
typedef struct AggStatePerTransData
{
	/*
	 * These values are set up during ExecInitAgg() and do not change
	 * thereafter:
	 * 这些值在 ExecInitAgg() 期间设置，此后不再更改：
	 */

	/*
	 * Link to an Aggref expr this state value is for.
	 * 链接到此状态值所属的 Aggref 表达式。
	 *
	 * There can be multiple Aggref's sharing the same state value, so long as
	 * the inputs and transition functions are identical and the final
	 * functions are not read-write.  This points to the first one of them.
	 * 只要输入和转换函数相同，并且最终函数不是读写的，就可以有多个 Aggref 共享同一个状态值。
	 * 这指向其中的第一个。
	 */
	Aggref	   *aggref;

	/*
	 * Is this state value actually being shared by more than one Aggref?
	 * 此状态值实际上是否由多个 Aggref 共享？
	 */
	bool		aggshared;

	/*
	 * True for ORDER BY and DISTINCT Aggrefs that are not aggpresorted.
	 * 对于非预先排序的 ORDER BY 和 DISTINCT Aggref 为真。
	 */
	bool		aggsortrequired;

	/*
	 * Number of aggregated input columns.  This includes ORDER BY expressions
	 * in both the plain-agg and ordered-set cases.  Ordered-set direct args
	 * are not counted, though.
	 * 聚合输入列的数量。这包括常规聚合和有序集情况下的 ORDER BY 表达式。
	 * 但不计算有序集的直接参数。
	 */
	int			numInputs;

	/*
	 * Number of aggregated input columns to pass to the transfn.  This
	 * includes the ORDER BY columns for ordered-set aggs, but not for plain
	 * aggs.  (This doesn't count the transition state value!)
	 * 要传递给转换函数 (transfn) 的聚合输入列数。
	 * 这包括有序集聚合的 ORDER BY 列，但不包括常规聚合。
	 * （这不包括转换状态值！）
	 */
	int			numTransInputs;

	/* Oid of the state transition or combine function */
	Oid			transfn_oid;

	/* Oid of the serialization function or InvalidOid */
	Oid			serialfn_oid;

	/* Oid of the deserialization function or InvalidOid */
	Oid			deserialfn_oid;

	/* Oid of state value's datatype */
	Oid			aggtranstype;

	/*
	 * fmgr lookup data for transition function or combine function.  Note in
	 * particular that the fn_strict flag is kept here.
	 */
	FmgrInfo	transfn;

	/* fmgr lookup data for serialization function */
	FmgrInfo	serialfn;

	/* fmgr lookup data for deserialization function */
	FmgrInfo	deserialfn;

	/* Input collation derived for aggregate */
	Oid			aggCollation;

	/* number of sorting columns */
	int			numSortCols;

	/* number of sorting columns to consider in DISTINCT comparisons */
	/* (this is either zero or the same as numSortCols) */
	int			numDistinctCols;

	/* deconstructed sorting information (arrays of length numSortCols) */
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *sortCollations;
	bool	   *sortNullsFirst;

	/*
	 * Comparators for input columns --- only set/used when aggregate has
	 * DISTINCT flag. equalfnOne version is used for single-column
	 * comparisons, equalfnMulti for the case of multiple columns.
	 */
	FmgrInfo	equalfnOne;
	ExprState  *equalfnMulti;

	/*
	 * initial value from pg_aggregate entry
	 */
	Datum		initValue;
	bool		initValueIsNull;

	/*
	 * We need the len and byval info for the agg's input and transition data
	 * types in order to know how to copy/delete values.
	 *
	 * Note that the info for the input type is used only when handling
	 * DISTINCT aggs with just one argument, so there is only one input type.
	 */
	int16		inputtypeLen,
				transtypeLen;
	bool		inputtypeByVal,
				transtypeByVal;

	/*
	 * Slots for holding the evaluated input arguments.  These are set up
	 * during ExecInitAgg() and then used for each input row requiring either
	 * FILTER or ORDER BY/DISTINCT processing.
	 */
	TupleTableSlot *sortslot;	/* current input tuple */
	TupleTableSlot *uniqslot;	/* used for multi-column DISTINCT */
	TupleDesc	sortdesc;		/* descriptor of input tuples */
	Datum		lastdatum;		/* used for single-column DISTINCT */
	bool		lastisnull;		/* used for single-column DISTINCT */
	bool		haslast;		/* got a last value for DISTINCT check */

	/*
	 * These values are working state that is initialized at the start of an
	 * input tuple group and updated for each input tuple.
	 *
	 * For a simple (non DISTINCT/ORDER BY) aggregate, we just feed the input
	 * values straight to the transition function.  If it's DISTINCT or
	 * requires ORDER BY, we pass the input values into a Tuplesort object;
	 * then at completion of the input tuple group, we scan the sorted values,
	 * eliminate duplicates if needed, and run the transition function on the
	 * rest.
	 *
	 * We need a separate tuplesort for each grouping set.
	 */

	Tuplesortstate **sortstates;	/* sort objects, if DISTINCT or ORDER BY */

	/*
	 * This field is a pre-initialized FunctionCallInfo struct used for
	 * calling this aggregate's transfn.  We save a few cycles per row by not
	 * re-initializing the unchanging fields; which isn't much, but it seems
	 * worth the extra space consumption.
	 */
	FunctionCallInfo transfn_fcinfo;

	/* Likewise for serialization and deserialization functions */
	FunctionCallInfo serialfn_fcinfo;

	FunctionCallInfo deserialfn_fcinfo;
}			AggStatePerTransData;

/*
 * AggStatePerAggData - per-aggregate information
 * AggStatePerAggData - 每个聚合的信息
 *
 * This contains the information needed to call the final function, to produce
 * a final aggregate result from the state value. If there are multiple
 * identical Aggrefs in the query, they can all share the same per-agg data.
 * 这包含调用最终函数所需的信息，以便从状态值产生最终聚合结果。
 * 如果查询中有多个完全相同的 Aggref，它们可以共享相同的 per-agg 数据。
 *
 * These values are set up during ExecInitAgg() and do not change thereafter.
 * 这些值在 ExecInitAgg() 期间设置，此后不再更改。
 */
typedef struct AggStatePerAggData
{
	/*
	 * Link to an Aggref expr this state value is for.
	 *
	 * There can be multiple identical Aggref's sharing the same per-agg. This
	 * points to the first one of them.
	 */
	Aggref	   *aggref;

	/* index to the state value which this agg should use */
	int			transno;

	/* Optional Oid of final function (may be InvalidOid) */
	Oid			finalfn_oid;

	/*
	 * fmgr lookup data for final function --- only valid when finalfn_oid is
	 * not InvalidOid.
	 */
	FmgrInfo	finalfn;

	/*
	 * Number of arguments to pass to the finalfn.  This is always at least 1
	 * (the transition state value) plus any ordered-set direct args. If the
	 * finalfn wants extra args then we pass nulls corresponding to the
	 * aggregated input columns.
	 */
	int			numFinalArgs;

	/* ExprStates for any direct-argument expressions */
	List	   *aggdirectargs;

	/*
	 * We need the len and byval info for the agg's result data type in order
	 * to know how to copy/delete values.
	 */
	int16		resulttypeLen;
	bool		resulttypeByVal;

	/*
	 * "shareable" is false if this agg cannot share state values with other
	 * aggregates because the final function is read-write.
	 */
	bool		shareable;
}			AggStatePerAggData;

/*
 * AggStatePerGroupData - per-aggregate-per-group working state
 * AggStatePerGroupData - 每个聚合每个分组的处理状态
 *
 * These values are working state that is initialized at the start of
 * an input tuple group and updated for each input tuple.
 * 这些值是在输入元组组开始时初始化并为每个输入元组更新的处理状态。
 *
 * In AGG_PLAIN and AGG_SORTED modes, we have a single array of these
 * structs (pointed to by aggstate->pergroup); we re-use the array for
 * each input group, if it's AGG_SORTED mode.  In AGG_HASHED mode, the
 * hash table contains an array of these structs for each tuple group.
 * 在 AGG_PLAIN 和 AGG_SORTED 模式下，我们有一个此类结构的单数组（由 aggstate->pergroup 指向）；
 * 如果是 AGG_SORTED 模式，我们会为每个输入组重用该数组。
 * 在 AGG_HASHED 模式下，哈希表为每个元组组包含一个此类结构的数组。
 *
 * Logically, the sortstate field belongs in this struct, but we do not
 * keep it here for space reasons: we don't support DISTINCT aggregates
 * in AGG_HASHED mode, so there's no reason to use up a pointer field
 * in every entry of the hashtable.
 * 从逻辑上讲，sortstate 字段属于此结构，但出于空间原因，我们不将其保留在此处：
 * 我们不支持 AGG_HASHED 模式下的 DISTINCT 聚合，因此没有理由在哈希表的每个条目中都占用一个指针字段。
 */
typedef struct AggStatePerGroupData
{
#define FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUE 0
	Datum		transValue;		/* current transition value */
#define FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUEISNULL 1
	bool		transValueIsNull;

#define FIELDNO_AGGSTATEPERGROUPDATA_NOTRANSVALUE 2
	bool		noTransValue;	/* true if transValue not set yet */

	/*
	 * Note: noTransValue initially has the same value as transValueIsNull,
	 * and if true both are cleared to false at the same time.  They are not
	 * the same though: if transfn later returns a NULL, we want to keep that
	 * NULL and not auto-replace it with a later input value. Only the first
	 * non-NULL input will be auto-substituted.
	 * 注意：noTransValue 最初与 transValueIsNull 具有相同的值，如果为真，则两者同时清除为假。
	 * 但它们并不相同：如果转换函数后来回传一个 NULL，我们要保留该 NULL，而不是用后面的输入值自动替换。
	 * 只有第一个非 NULL 输入会被自动替换。
	 */
}			AggStatePerGroupData;

/*
 * AggStatePerPhaseData - per-grouping-set-phase state
 * AggStatePerPhaseData - 每个分组集阶段的状态
 *
 * Grouping sets are divided into "phases", where a single phase can be
 * processed in one pass over the input. If there is more than one phase, then
 * at the end of input from the current phase, state is reset and another pass
 * taken over the data which has been re-sorted in the mean time.
 * 分组集分为多个“阶段”，其中单个阶段可以对输入执行一次扫描来处理。如果存在多个阶段，
 * 那么在当前阶段的输入结束时，状态会被重置，并对在此期间已重新排序的数据进行另一次扫描。
 *
 * Accordingly, each phase specifies a list of grouping sets and group clause
 * information, plus each phase after the first also has a sort order.
 * 因此，每个阶段指定一个分组集列表和分组子句信息，而且第一个阶段之后的每个阶段还有一个排序顺序。
 */
typedef struct AggStatePerPhaseData
{
	AggStrategy aggstrategy;	/* strategy for this phase */
	int			numsets;		/* number of grouping sets (or 0) */
	int		   *gset_lengths;	/* lengths of grouping sets */
	Bitmapset **grouped_cols;	/* column groupings for rollup */
	ExprState **eqfunctions;	/* expression returning equality, indexed by
								 * nr of cols to compare */
	Agg		   *aggnode;		/* Agg node for phase data */
	Sort	   *sortnode;		/* Sort node for input ordering for phase */

	ExprState  *evaltrans;		/* evaluation of transition functions  */

	/*----------
	 * Cached variants of the compiled expression.
	 * first subscript: 0: outerops; 1: TTSOpsMinimalTuple
	 * second subscript: 0: no NULL check; 1: with NULL check
	 *----------
	 */
	ExprState  *evaltrans_cache[2][2];
}			AggStatePerPhaseData;

/*
 * AggStatePerHashData - per-hashtable state
 *
 * When doing grouping sets with hashing, we have one of these for each
 * grouping set. (When doing hashing without grouping sets, we have just one of
 * them.)
 */
typedef struct AggStatePerHashData
{
	TupleHashTable hashtable;	/* hash table with one entry per group */
	TupleHashIterator hashiter; /* for iterating through hash table */
	TupleTableSlot *hashslot;	/* slot for loading hash table */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	Oid		   *eqfuncoids;		/* per-grouping-field equality fns */
	int			numCols;		/* number of hash key columns */
	int			numhashGrpCols; /* number of columns in hash table */
	int			largestGrpColIdx;	/* largest col required for hashing */
	AttrNumber *hashGrpColIdxInput; /* hash col indices in input slot */
	AttrNumber *hashGrpColIdxHash;	/* indices in hash table tuples */
	Agg		   *aggnode;		/* original Agg node, for numGroups etc. */
}			AggStatePerHashData;


/* Initialize Agg node / 初始化 Agg 节点 */
extern AggState *ExecInitAgg(Agg *node, EState *estate, int eflags);
/* Cleanup Agg node / 清理 Agg 节点 */
extern void ExecEndAgg(AggState *node);
/* Restart Agg scan / 重新启动 Agg 扫描 */
extern void ExecReScanAgg(AggState *node);

/* Calculate hash aggregation entry size / 计算哈希聚合条目大小 */
extern Size hash_agg_entry_size(int numTrans, Size tupleWidth,
								Size transitionSpace);
/* Set limits for hash aggregation / 设置哈希聚合的限制 */
extern void hash_agg_set_limits(double hashentrysize, double input_groups,
								int used_bits, Size *mem_limit,
								uint64 *ngroups_limit, int *num_partitions);

/* parallel instrumentation support / 并行检测支持 */
/* Estimate aggregation DSM requirement / 估算聚合操作的共享内存需求 */
extern void ExecAggEstimate(AggState *node, ParallelContext *pcxt);
/* Initialize aggregation DSM state / 初始化聚合操作的共享内存状态 */
extern void ExecAggInitializeDSM(AggState *node, ParallelContext *pcxt);
/* Initialize parallel worker for aggregation / 为聚合操作初始化并行工作进程 */
extern void ExecAggInitializeWorker(AggState *node, ParallelWorkerContext *pwcxt);
/* Retrieve parallel aggregation instrumentation / 获取并行聚合的检测结果 */
extern void ExecAggRetrieveInstrumentation(AggState *node);

#endif							/* NODEAGG_H */
