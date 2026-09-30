/*-------------------------------------------------------------------------
 *
 * spgist.h
 *	  Public header file for SP-GiST access method.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_H
#define SPGIST_H

#include "access/amapi.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"


/* SPGiST opclass support function numbers */

/* SP-GiST 操作符类的支持函数编号 */
#define SPGIST_CONFIG_PROC				1
#define SPGIST_CHOOSE_PROC				2
#define SPGIST_PICKSPLIT_PROC			3
#define SPGIST_INNER_CONSISTENT_PROC	4
#define SPGIST_LEAF_CONSISTENT_PROC		5
#define SPGIST_COMPRESS_PROC			6
#define SPGIST_OPTIONS_PROC				7
#define SPGISTNRequiredProc				5
#define SPGISTNProc						7

/*
 * Argument structs for spg_config method
 */

/*
 * spg_config 方法的参数结构体
 */
typedef struct spgConfigIn
{
	Oid			attType;		/* Data type to be indexed */

	/* 需要被索引的数据类型 */
} spgConfigIn;

typedef struct spgConfigOut
{
	Oid			prefixType;		/* Data type of inner-tuple prefixes */

	/* 内部元组前缀的数据类型 */
	Oid			labelType;		/* Data type of inner-tuple node labels */

	/* 内部元组节点标签的数据类型 */
	Oid			leafType;		/* Data type of leaf-tuple values */

	/* 叶子元组值的数据类型 */
	bool		canReturnData;	/* Opclass can reconstruct original data */

	/* 操作符类能否重建原始数据 */
	bool		longValuesOK;	/* Opclass can cope with values > 1 page */

	/* 操作符类能否处理超过一个页面大小的值 */
} spgConfigOut;

/*
 * Argument structs for spg_choose method
 */

/*
 * spg_choose 方法的参数结构体
 */
typedef struct spgChooseIn
{
	Datum		datum;			/* original datum to be indexed */

	/* 需要被索引的原始 datum */
	Datum		leafDatum;		/* current datum to be stored at leaf */

	/* 当前将要存储在叶子中的 datum */
	int			level;			/* current level (counting from zero) */

	/* 当前层级（从零开始计数） */

	/* Data from current inner tuple */

	/* 来自当前内部元组的数据 */
	bool		allTheSame;		/* tuple is marked all-the-same? */

	/* 元组是否被标记为 all-the-same？ */
	bool		hasPrefix;		/* tuple has a prefix? */

	/* 元组是否有前缀？ */
	Datum		prefixDatum;	/* if so, the prefix value */

	/* 如果有，则为前缀值 */
	int			nNodes;			/* number of nodes in the inner tuple */

	/* 内部元组中的节点数量 */
	Datum	   *nodeLabels;		/* node label values (NULL if none) */

	/* 节点标签值（如果没有则为 NULL） */
} spgChooseIn;

typedef enum spgChooseResultType
{
	spgMatchNode = 1,			/* descend into existing node */

	/* 下降进入已存在的节点 */
	spgAddNode,					/* add a node to the inner tuple */

	/* 向内部元组添加一个节点 */
	spgSplitTuple,				/* split inner tuple (change its prefix) */

	/* 分裂内部元组（改变其前缀） */
} spgChooseResultType;

typedef struct spgChooseOut
{
	spgChooseResultType resultType; /* action code, see above */

	/* 动作代码，参见上文 */
	union
	{
		struct					/* results for spgMatchNode */

		/* 用于 spgMatchNode 的结果 */
		{
			int			nodeN;	/* descend to this node (index from 0) */

			/* 下降到此节点（索引从 0 开始） */
			int			levelAdd;	/* increment level by this much */

			/* 将层级增加这么多 */
			Datum		restDatum;	/* new leaf datum */

			/* 新的叶子 datum */
		}			matchNode;
		struct					/* results for spgAddNode */

		/* 用于 spgAddNode 的结果 */
		{
			Datum		nodeLabel;	/* new node's label */

			/* 新节点的标签 */
			int			nodeN;	/* where to insert it (index from 0) */

			/* 插入它的位置（索引从 0 开始） */
		}			addNode;
		struct					/* results for spgSplitTuple */

		/* 用于 spgSplitTuple 的结果 */
		{
			/* Info to form new upper-level inner tuple with one child tuple */

			/* 用于构建带有一个子元组的新上层内部元组的信息 */
			bool		prefixHasPrefix;	/* tuple should have a prefix? */

			/* 元组是否应该有前缀？ */
			Datum		prefixPrefixDatum;	/* if so, its value */

			/* 如果有，则为其值 */
			int			prefixNNodes;	/* number of nodes */

			/* 节点数量 */
			Datum	   *prefixNodeLabels;	/* their labels (or NULL for no
											 * labels) */

			/* 它们的标签（若没有标签则为 NULL） */
			int			childNodeN; /* which node gets child tuple */

			/* 哪个节点获得子元组 */

			/* Info to form new lower-level inner tuple with all old nodes */

			/* 用于构建带有所有旧节点的新下层内部元组的信息 */
			bool		postfixHasPrefix;	/* tuple should have a prefix? */

			/* 元组是否应该有前缀？ */
			Datum		postfixPrefixDatum; /* if so, its value */

			/* 如果有，则为其值 */
		}			splitTuple;
	}			result;
} spgChooseOut;

/*
 * Argument structs for spg_picksplit method
 */

/*
 * spg_picksplit 方法的参数结构体
 */
typedef struct spgPickSplitIn
{
	int			nTuples;		/* number of leaf tuples */

	/* 叶子元组数量 */
	Datum	   *datums;			/* their datums (array of length nTuples) */

	/* 它们的 datum（长度为 nTuples 的数组） */
	int			level;			/* current level (counting from zero) */

	/* 当前层级（从零开始计数） */
} spgPickSplitIn;

typedef struct spgPickSplitOut
{
	bool		hasPrefix;		/* new inner tuple should have a prefix? */

	/* 新的内部元组是否应该有前缀？ */
	Datum		prefixDatum;	/* if so, its value */

	/* 如果有，则为其值 */

	int			nNodes;			/* number of nodes for new inner tuple */

	/* 新内部元组的节点数量 */
	Datum	   *nodeLabels;		/* their labels (or NULL for no labels) */

	/* 它们的标签（若没有标签则为 NULL） */

	int		   *mapTuplesToNodes;	/* node index for each leaf tuple */

	/* 每个叶子元组对应的节点索引 */
	Datum	   *leafTupleDatums;	/* datum to store in each new leaf tuple */

	/* 存储在每个新叶子元组中的 datum */
} spgPickSplitOut;

/*
 * Argument structs for spg_inner_consistent method
 */

/*
 * spg_inner_consistent 方法的参数结构体
 */
typedef struct spgInnerConsistentIn
{
	ScanKey		scankeys;		/* array of operators and comparison values */

	/* 操作符与比较值的数组 */
	ScanKey		orderbys;		/* array of ordering operators and comparison
								 * values */

	/* 排序操作符与比较值的数组 */
	int			nkeys;			/* length of scankeys array */

	/* scankeys 数组的长度 */
	int			norderbys;		/* length of orderbys array */

	/* orderbys 数组的长度 */

	Datum		reconstructedValue; /* value reconstructed at parent */

	/* 在父级处重建出的值 */
	void	   *traversalValue; /* opclass-specific traverse value */

	/* 操作符类特定的遍历值 */
	MemoryContext traversalMemoryContext;	/* put new traverse values here */

	/* 将新的遍历值存放在此处 */
	int			level;			/* current level (counting from zero) */

	/* 当前层级（从零开始计数） */
	bool		returnData;		/* original data must be returned? */

	/* 是否必须返回原始数据？ */

	/* Data from current inner tuple */

	/* 来自当前内部元组的数据 */
	bool		allTheSame;		/* tuple is marked all-the-same? */

	/* 元组是否被标记为 all-the-same？ */
	bool		hasPrefix;		/* tuple has a prefix? */

	/* 元组是否有前缀？ */
	Datum		prefixDatum;	/* if so, the prefix value */

	/* 如果有，则为前缀值 */
	int			nNodes;			/* number of nodes in the inner tuple */

	/* 内部元组中的节点数量 */
	Datum	   *nodeLabels;		/* node label values (NULL if none) */

	/* 节点标签值（如果没有则为 NULL） */
} spgInnerConsistentIn;

typedef struct spgInnerConsistentOut
{
	int			nNodes;			/* number of child nodes to be visited */

	/* 需要访问的子节点数量 */
	int		   *nodeNumbers;	/* their indexes in the node array */

	/* 它们在节点数组中的索引 */
	int		   *levelAdds;		/* increment level by this much for each */

	/* 每个节点各自需要增加的层级量 */
	Datum	   *reconstructedValues;	/* associated reconstructed values */

	/* 相关联的重建值 */
	void	  **traversalValues;	/* opclass-specific traverse values */

	/* 操作符类特定的遍历值 */
	double	  **distances;		/* associated distances */

	/* 相关联的距离 */
} spgInnerConsistentOut;

/*
 * Argument structs for spg_leaf_consistent method
 */

/*
 * spg_leaf_consistent 方法的参数结构体
 */
typedef struct spgLeafConsistentIn
{
	ScanKey		scankeys;		/* array of operators and comparison values */

	/* 操作符与比较值的数组 */
	ScanKey		orderbys;		/* array of ordering operators and comparison
								 * values */

	/* 排序操作符与比较值的数组 */
	int			nkeys;			/* length of scankeys array */

	/* scankeys 数组的长度 */
	int			norderbys;		/* length of orderbys array */

	/* orderbys 数组的长度 */

	Datum		reconstructedValue; /* value reconstructed at parent */

	/* 在父级处重建出的值 */
	void	   *traversalValue; /* opclass-specific traverse value */

	/* 操作符类特定的遍历值 */
	int			level;			/* current level (counting from zero) */

	/* 当前层级（从零开始计数） */
	bool		returnData;		/* original data must be returned? */

	/* 是否必须返回原始数据？ */

	Datum		leafDatum;		/* datum in leaf tuple */

	/* 叶子元组中的 datum */
} spgLeafConsistentIn;

typedef struct spgLeafConsistentOut
{
	Datum		leafValue;		/* reconstructed original data, if any */

	/* 重建出的原始数据（如果有） */
	bool		recheck;		/* set true if operator must be rechecked */

	/* 如果操作符必须重新检查，则设为 true */
	bool		recheckDistances;	/* set true if distances must be rechecked */

	/* 如果距离必须重新检查，则设为 true */
	double	   *distances;		/* associated distances */

	/* 相关联的距离 */
} spgLeafConsistentOut;


/* spgutils.c */

/* 以下函数来自 spgutils.c */

/*
 * spgoptions: parse and validate the reloptions array for an SP-GiST index,
 * returning a filled bytea options struct (used to interpret settings such as
 * fillfactor).  When validate is true, invalid options raise an error.
 */

/*
 * spgoptions：解析并校验 SP-GiST 索引的 reloptions 数组，返回一个已填充的
 * bytea 选项结构体（用于解释诸如 fillfactor 之类的设置）。当 validate 为
 * true 时，无效的选项会引发错误。
 */
extern bytea *spgoptions(Datum reloptions, bool validate);

/* spginsert.c */

/* 以下函数来自 spginsert.c */

/*
 * spgbuild: build a new SP-GiST index from scratch over the given heap.
 * Initializes metadata pages then scans the heap, inserting every indexable
 * tuple, and returns statistics about the completed build.
 */

/*
 * spgbuild：针对给定的堆从头构建一个新的 SP-GiST 索引。它初始化元数据
 * 页面，随后扫描堆表，插入每一个可索引的元组，并返回关于已完成构建的
 * 统计信息。
 */
extern IndexBuildResult *spgbuild(Relation heap, Relation index,
								  struct IndexInfo *indexInfo);

/*
 * spgbuildempty: build an empty SP-GiST index in the init fork, used to set
 * up the initial state for an unlogged index.
 */

/*
 * spgbuildempty：在 init fork 中构建一个空的 SP-GiST 索引，用于为不记录
 * 日志（unlogged）的索引建立初始状态。
 */
extern void spgbuildempty(Relation index);

/*
 * spginsert: insert one heap tuple's indexed values into an existing SP-GiST
 * index.  Wraps spgdoinsert with the per-call state setup and returns whether
 * the tuple was inserted.
 */

/*
 * spginsert：将一个堆元组的被索引值插入到已存在的 SP-GiST 索引中。它在
 * 完成每次调用的状态设置后封装 spgdoinsert，并返回该元组是否被插入。
 */
extern bool spginsert(Relation index, Datum *values, bool *isnull,
					  ItemPointer ht_ctid, Relation heapRel,
					  IndexUniqueCheck checkUnique,
					  bool indexUnchanged,
					  struct IndexInfo *indexInfo);

/* spgscan.c */

/* 以下函数来自 spgscan.c */

/*
 * spgbeginscan: allocate and initialize an index scan descriptor for an
 * SP-GiST index, preparing the scan-opaque state and workspace for the given
 * numbers of key and ordering columns.
 */

/*
 * spgbeginscan：为一个 SP-GiST 索引分配并初始化一个索引扫描描述符，
 * 为给定数量的键列和排序列准备扫描的私有状态（scan-opaque）与工作空间。
 */
extern IndexScanDesc spgbeginscan(Relation rel, int keysz, int orderbysz);

/*
 * spgendscan: release all resources associated with an SP-GiST index scan,
 * freeing memory contexts and workspace allocated by spgbeginscan.
 */

/*
 * spgendscan：释放与一个 SP-GiST 索引扫描相关联的所有资源，释放由
 * spgbeginscan 分配的内存上下文与工作空间。
 */
extern void spgendscan(IndexScanDesc scan);

/*
 * spgrescan: (re)start an SP-GiST index scan with a new set of scan keys and
 * ordering operators, resetting the traversal queue and scan state.
 */

/*
 * spgrescan：使用一组新的扫描键和排序操作符（重新）启动一个 SP-GiST
 * 索引扫描，重置遍历队列与扫描状态。
 */
extern void spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					  ScanKey orderbys, int norderbys);

/*
 * spggetbitmap: perform a bitmap index scan, collecting all matching heap
 * TIDs into the given bitmap and returning the number of TIDs found.
 */

/*
 * spggetbitmap：执行一次位图索引扫描，将所有匹配的堆 TID 收集到给定的
 * 位图中，并返回找到的 TID 数量。
 */
extern int64 spggetbitmap(IndexScanDesc scan, TIDBitmap *tbm);

/*
 * spggettuple: fetch the next matching tuple in the given scan direction,
 * returning true if one was found and setting up the current heap TID (and
 * reconstructed tuple, if requested).
 */

/*
 * spggettuple：按给定的扫描方向获取下一个匹配的元组，若找到则返回 true，
 * 并设置当前的堆 TID（若有请求，还会设置重建出的元组）。
 */
extern bool spggettuple(IndexScanDesc scan, ScanDirection dir);

/*
 * spgcanreturn: report whether the SP-GiST index can return the original
 * indexed value for the given attribute (i.e. supports index-only scans).
 */

/*
 * spgcanreturn：报告该 SP-GiST 索引能否为给定属性返回原始的被索引值
 *（即是否支持仅索引扫描，index-only scan）。
 */
extern bool spgcanreturn(Relation index, int attno);

/* spgvacuum.c */

/* 以下函数来自 spgvacuum.c */

/*
 * spgbulkdelete: scan the whole SP-GiST index during VACUUM, invoking the
 * callback to decide which heap TIDs are dead, and remove or convert the
 * corresponding leaf/redirect tuples accordingly.
 */

/*
 * spgbulkdelete：在 VACUUM 期间扫描整个 SP-GiST 索引，调用回调来判定
 * 哪些堆 TID 已死亡，并据此移除或转换相应的叶子/重定向元组。
 */
extern IndexBulkDeleteResult *spgbulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);

/*
 * spgvacuumcleanup: post-VACUUM cleanup pass for an SP-GiST index; if no prior
 * bulkdelete ran it scans to gather statistics, and updates the free space map
 * and reported index statistics.
 */

/*
 * spgvacuumcleanup：SP-GiST 索引在 VACUUM 之后的清理阶段；如果此前没有
 * 执行过 bulkdelete，它会进行扫描以收集统计信息，并更新空闲空间映射
 *（FSM）以及上报的索引统计信息。
 */
extern IndexBulkDeleteResult *spgvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);

/* spgvalidate.c */

/* 以下函数来自 spgvalidate.c */

/*
 * spgvalidate: validate the definition of an SP-GiST operator class, checking
 * that its required support functions and operators are present and have
 * sensible signatures.  Returns true if the opclass is valid.
 */

/*
 * spgvalidate：校验一个 SP-GiST 操作符类的定义，检查其所需的支持函数和
 * 操作符是否齐备且具有合理的签名。若该操作符类有效则返回 true。
 */
extern bool spgvalidate(Oid opclassoid);

/*
 * spgadjustmembers: adjust the dependency and other catalog properties of the
 * operators and support functions being added to an SP-GiST operator family,
 * setting appropriate dependency types for each member.
 */

/*
 * spgadjustmembers：调整正被加入某个 SP-GiST 操作符族的操作符与支持函数
 * 的依赖关系及其他目录属性，为每个成员设置恰当的依赖类型。
 */
extern void spgadjustmembers(Oid opfamilyoid,
							 Oid opclassoid,
							 List *operators,
							 List *functions);

#endif							/* SPGIST_H */
