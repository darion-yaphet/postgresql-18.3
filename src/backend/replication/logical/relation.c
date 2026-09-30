/*-------------------------------------------------------------------------
 * relation.c
 *	   PostgreSQL logical replication relation mapping cache
 *
 * PostgreSQL 逻辑复制的关系映射缓存。
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/relation.c
 *
 * NOTES
 *	  Routines in this file mainly have to do with mapping the properties
 *	  of local replication target relations to the properties of their
 *	  remote counterpart.
 *
 * 本文件的例程主要把本地复制目标关系的属性映射到远端对应关系。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_subscription_rel.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "replication/logicalrelation.h"
#include "replication/worker_internal.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static MemoryContext LogicalRepRelMapContext = NULL;

static HTAB *LogicalRepRelMap = NULL;

/*
 * Partition map (LogicalRepPartMap)
 *
 * 分区映射 LogicalRepPartMap。
 *
 * When a partitioned table is used as replication target, replicated
 * operations are actually performed on its leaf partitions, which requires
 * the partitions to also be mapped to the remote relation.  Parent's entry
 * (LogicalRepRelMapEntry) cannot be used as-is for all partitions, because
 * individual partitions may have different attribute numbers, which means
 * attribute mappings to remote relation's attributes must be maintained
 * separately for each partition.
 *
 * 分区表作为复制目标时，复制操作实际落在叶子分区上，因此分区也必须映射到远端关系。
 * 父表项 LogicalRepRelMapEntry 不能原样用于所有分区，因为各分区的属性号可能不同，
 * 到远端属性的映射必须按分区分别维护。
 */
static MemoryContext LogicalRepPartMapContext = NULL;
static HTAB *LogicalRepPartMap = NULL;
typedef struct LogicalRepPartMapEntry
{
	Oid			partoid;		/* LogicalRepPartMap's key
								 *
								 * LogicalRepPartMap 的键。
								 */
	LogicalRepRelMapEntry relmapentry;
} LogicalRepPartMapEntry;

static Oid	FindLogicalRepLocalIndex(Relation localrel, LogicalRepRelation *remoterel,
									 AttrMap *attrMap);

/*
 * Relcache invalidation callback for our relation map cache.
 *
 * 关系映射缓存的 relcache 失效回调。
 */
static void
logicalrep_relmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepRelMapEntry *entry;

	/* Just to be sure.
	 *
	 * 再确认一次。
	 */
	if (LogicalRepRelMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		/* TODO, use inverse lookup hashtable?
		 *
		 * 待办：是否改用反向查找哈希表？
		 */
		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->localreloid == reloid)
			{
				entry->localrelvalid = false;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* invalidate all cache entries
		 *
		 * 使全部缓存项失效。
		 */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepRelMap);

		while ((entry = (LogicalRepRelMapEntry *) hash_seq_search(&status)) != NULL)
			entry->localrelvalid = false;
	}
}

/*
 * Initialize the relation map cache.
 *
 * 初始化关系映射缓存。
 */
static void
logicalrep_relmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepRelMapContext)
		LogicalRepRelMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepRelMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* Initialize the relation hash table.
	 *
	 * 初始化关系哈希表。
	 */
	ctl.keysize = sizeof(LogicalRepRelId);
	ctl.entrysize = sizeof(LogicalRepRelMapEntry);
	ctl.hcxt = LogicalRepRelMapContext;

	LogicalRepRelMap = hash_create("logicalrep relation map cache", 128, &ctl,
								   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events.
	 *
	 * 监听失效事件。
	 */
	CacheRegisterRelcacheCallback(logicalrep_relmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * Free the entry of a relation map cache.
 *
 * 释放关系映射缓存中的一项。
 */
static void
logicalrep_relmap_free_entry(LogicalRepRelMapEntry *entry)
{
	LogicalRepRelation *remoterel;

	remoterel = &entry->remoterel;

	pfree(remoterel->nspname);
	pfree(remoterel->relname);

	if (remoterel->natts > 0)
	{
		int			i;

		for (i = 0; i < remoterel->natts; i++)
			pfree(remoterel->attnames[i]);

		pfree(remoterel->attnames);
		pfree(remoterel->atttyps);
	}
	bms_free(remoterel->attkeys);

	if (entry->attrmap)
		free_attrmap(entry->attrmap);
}

/*
 * Add new entry or update existing entry in the relation map cache.
 *
 * 在关系映射缓存中新增或更新一项。
 *
 * Called when new relation mapping is sent by the publisher to update
 * our expected view of incoming data from said publisher.
 *
 * 发布端发来新的关系映射时调用，用来更新我们对这份数据的预期。
 */
void
logicalrep_relmap_update(LogicalRepRelation *remoterel)
{
	MemoryContext oldctx;
	LogicalRepRelMapEntry *entry;
	bool		found;
	int			i;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/*
	 * HASH_ENTER returns the existing entry if present or creates a new one.
	 *
	 * HASH_ENTER 在已有项时返回它，否则创建新项。
	 */
	entry = hash_search(LogicalRepRelMap, &remoterel->remoteid,
						HASH_ENTER, &found);

	if (found)
		logicalrep_relmap_free_entry(entry);

	memset(entry, 0, sizeof(LogicalRepRelMapEntry));

	/* Make cached copy of the data
	 *
	 * 把数据复制进缓存。
	 */
	oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
	entry->remoterel.remoteid = remoterel->remoteid;
	entry->remoterel.nspname = pstrdup(remoterel->nspname);
	entry->remoterel.relname = pstrdup(remoterel->relname);
	entry->remoterel.natts = remoterel->natts;
	entry->remoterel.attnames = palloc(remoterel->natts * sizeof(char *));
	entry->remoterel.atttyps = palloc(remoterel->natts * sizeof(Oid));
	for (i = 0; i < remoterel->natts; i++)
	{
		entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
		entry->remoterel.atttyps[i] = remoterel->atttyps[i];
	}
	entry->remoterel.replident = remoterel->replident;
	entry->remoterel.attkeys = bms_copy(remoterel->attkeys);
	MemoryContextSwitchTo(oldctx);
}

/*
 * Find attribute index in TupleDesc struct by attribute name.
 *
 * 按属性名在 TupleDesc 中查找属性下标。
 *
 * Returns -1 if not found.
 *
 * 找不到则返回 -1。
 */
static int
logicalrep_rel_att_by_name(LogicalRepRelation *remoterel, const char *attname)
{
	int			i;

	for (i = 0; i < remoterel->natts; i++)
	{
		if (strcmp(remoterel->attnames[i], attname) == 0)
			return i;
	}

	return -1;
}

/*
 * Returns a comma-separated string of attribute names based on the provided
 * relation and bitmap indicating which attributes to include.
 *
 * 按给定关系和表示要包含哪些属性的位图，返回逗号分隔的属性名字符串。
 */
static char *
logicalrep_get_attrs_str(LogicalRepRelation *remoterel, Bitmapset *atts)
{
	StringInfoData attsbuf;
	int			attcnt = 0;
	int			i = -1;

	Assert(!bms_is_empty(atts));

	initStringInfo(&attsbuf);

	while ((i = bms_next_member(atts, i)) >= 0)
	{
		attcnt++;
		if (attcnt > 1)
			appendStringInfoString(&attsbuf, _(", "));

		appendStringInfo(&attsbuf, _("\"%s\""), remoterel->attnames[i]);
	}

	return attsbuf.data;
}

/*
 * If attempting to replicate missing or generated columns, report an error.
 * Prioritize 'missing' errors if both occur though the prioritization is
 * arbitrary.
 *
 * 若试图复制缺失列或生成列，则报错。两者同时出现时优先报缺失列，这个优先级是随意定的。
 */
static void
logicalrep_report_missing_or_gen_attrs(LogicalRepRelation *remoterel,
									   Bitmapset *missingatts,
									   Bitmapset *generatedatts)
{
	if (!bms_is_empty(missingatts))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("logical replication target relation \"%s.%s\" is missing replicated column: %s",
							  "logical replication target relation \"%s.%s\" is missing replicated columns: %s",
							  bms_num_members(missingatts),
							  remoterel->nspname,
							  remoterel->relname,
							  logicalrep_get_attrs_str(remoterel,
													   missingatts)));

	if (!bms_is_empty(generatedatts))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg_plural("logical replication target relation \"%s.%s\" has incompatible generated column: %s",
							  "logical replication target relation \"%s.%s\" has incompatible generated columns: %s",
							  bms_num_members(generatedatts),
							  remoterel->nspname,
							  remoterel->relname,
							  logicalrep_get_attrs_str(remoterel,
													   generatedatts)));
}

/*
 * Check if replica identity matches and mark the updatable flag.
 *
 * 检查副本标识是否匹配，并设置可更新标志。
 *
 * We allow for stricter replica identity (fewer columns) on subscriber as
 * that will not stop us from finding unique tuple. IE, if publisher has
 * identity (id,timestamp) and subscriber just (id) this will not be a
 * problem, but in the opposite scenario it will.
 *
 * 订阅端的副本标识可以更严、列更少，这不妨碍找到唯一元组。
 * 例如发布端标识是 id 加 timestamp，订阅端只有 id，没有问题；反过来则会有问题。
 *
 * We just mark the relation entry as not updatable here if the local
 * replica identity is found to be insufficient for applying
 * updates/deletes (inserts don't care!) and leave it to
 * check_relation_updatable() to throw the actual error if needed.
 *
 * 若本地副本标识不足以应用 UPDATE 和 DELETE，这里只把关系项标为不可更新。INSERT 不在乎这一点。
 * 真正的错误留给 check_relation_updatable() 在需要时抛出。
 */
static void
logicalrep_rel_mark_updatable(LogicalRepRelMapEntry *entry)
{
	Bitmapset  *idkey;
	LogicalRepRelation *remoterel = &entry->remoterel;
	int			i;

	entry->updatable = true;

	idkey = RelationGetIndexAttrBitmap(entry->localrel,
									   INDEX_ATTR_BITMAP_IDENTITY_KEY);
	/* fallback to PK if no replica identity
	 *
	 * 没有副本标识时退回到主键。
	 */
	if (idkey == NULL)
	{
		idkey = RelationGetIndexAttrBitmap(entry->localrel,
										   INDEX_ATTR_BITMAP_PRIMARY_KEY);

		/*
		 * If no replica identity index and no PK, the published table must
		 * have replica identity FULL.
		 *
		 * 若既没有副本标识索引也没有主键，发布表必须是 REPLICA IDENTITY FULL。
		 */
		if (idkey == NULL && remoterel->replident != REPLICA_IDENTITY_FULL)
			entry->updatable = false;
	}

	i = -1;
	while ((i = bms_next_member(idkey, i)) >= 0)
	{
		int			attnum = i + FirstLowInvalidHeapAttributeNumber;

		if (!AttrNumberIsForUserDefinedAttr(attnum))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" uses "
							"system columns in REPLICA IDENTITY index",
							remoterel->nspname, remoterel->relname)));

		attnum = AttrNumberGetAttrOffset(attnum);

		if (entry->attrmap->attnums[attnum] < 0 ||
			!bms_is_member(entry->attrmap->attnums[attnum], remoterel->attkeys))
		{
			entry->updatable = false;
			break;
		}
	}
}

/*
 * 核心流程：发布端关系经 logicalrep_relmap_update 进入映射缓存，logicalrep_rel_open 打开本地表并建立列映射；分区目标再由 logicalrep_partition_open 重写 attrmap。
 */

/*
 * Open the local relation associated with the remote one.
 *
 * 打开与远端关系对应的本地关系。
 *
 * Rebuilds the Relcache mapping if it was invalidated by local DDL.
 *
 * 若本地 DDL 使映射失效，则重建 Relcache 映射。
 */
LogicalRepRelMapEntry *
logicalrep_rel_open(LogicalRepRelId remoteid, LOCKMODE lockmode)
{
	LogicalRepRelMapEntry *entry;
	bool		found;
	LogicalRepRelation *remoterel;

	if (LogicalRepRelMap == NULL)
		logicalrep_relmap_init();

	/* Search for existing entry.
	 *
	 * 查找已有项。
	 */
	entry = hash_search(LogicalRepRelMap, &remoteid,
						HASH_FIND, &found);

	if (!found)
		elog(ERROR, "no relation map entry for remote relation ID %u",
			 remoteid);

	remoterel = &entry->remoterel;

	/* Ensure we don't leak a relcache refcount.
	 *
	 * 确保不泄漏 relcache 引用计数。
	 */
	if (entry->localrel)
		elog(ERROR, "remote relation ID %u is already open", remoteid);

	/*
	 * When opening and locking a relation, pending invalidation messages are
	 * processed which can invalidate the relation.  Hence, if the entry is
	 * currently considered valid, try to open the local relation by OID and
	 * see if invalidation ensues.
	 *
	 * 打开并锁住关系时会处理待定的失效消息，关系可能因此失效。
	 * 因此若该项当前仍视为有效，先按 OID 打开本地关系，看失效是否发生。
	 */
	if (entry->localrelvalid)
	{
		entry->localrel = try_table_open(entry->localreloid, lockmode);
		if (!entry->localrel)
		{
			/* Table was renamed or dropped.
			 *
			 * 表已被改名或删除。
			 */
			entry->localrelvalid = false;
		}
		else if (!entry->localrelvalid)
		{
			/* Note we release the no-longer-useful lock here.
			 *
			 * 这里释放已经没用的锁。
			 */
			table_close(entry->localrel, lockmode);
			entry->localrel = NULL;
		}
	}

	/*
	 * If the entry has been marked invalid since we last had lock on it,
	 * re-open the local relation by name and rebuild all derived data.
	 *
	 * 若自上次持锁以来该项已被标为失效，则按名称重新打开本地关系，并重建全部派生数据。
	 */
	if (!entry->localrelvalid)
	{
		Oid			relid;
		TupleDesc	desc;
		MemoryContext oldctx;
		int			i;
		Bitmapset  *missingatts;
		Bitmapset  *generatedattrs = NULL;

		/* Release the no-longer-useful attrmap, if any.
		 *
		 * 若还有已经没用的 attrmap，则释放它。
		 */
		if (entry->attrmap)
		{
			free_attrmap(entry->attrmap);
			entry->attrmap = NULL;
		}

		/* Try to find and lock the relation by name.
		 *
		 * 尝试按名称找到并锁住关系。
		 */
		relid = RangeVarGetRelid(makeRangeVar(remoterel->nspname,
											  remoterel->relname, -1),
								 lockmode, true);
		if (!OidIsValid(relid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication target relation \"%s.%s\" does not exist",
							remoterel->nspname, remoterel->relname)));
		entry->localrel = table_open(relid, NoLock);
		entry->localreloid = relid;

		/* Check for supported relkind.
		 *
		 * 检查 relkind 是否受支持。
		 */
		CheckSubscriptionRelkind(entry->localrel->rd_rel->relkind,
								 remoterel->nspname, remoterel->relname);

		/*
		 * Build the mapping of local attribute numbers to remote attribute
		 * numbers and validate that we don't miss any replicated columns as
		 * that would result in potentially unwanted data loss.
		 *
		 * 建立本地属性号到远端属性号的映射，并确认没有漏掉被复制的列，否则可能无意中丢数据。
		 */
		desc = RelationGetDescr(entry->localrel);
		oldctx = MemoryContextSwitchTo(LogicalRepRelMapContext);
		entry->attrmap = make_attrmap(desc->natts);
		MemoryContextSwitchTo(oldctx);

		/* check and report missing attrs, if any
		 *
		 * 若有缺失属性，检查并报告。
		 */
		missingatts = bms_add_range(NULL, 0, remoterel->natts - 1);
		for (i = 0; i < desc->natts; i++)
		{
			int			attnum;
			Form_pg_attribute attr = TupleDescAttr(desc, i);

			if (attr->attisdropped)
			{
				entry->attrmap->attnums[i] = -1;
				continue;
			}

			attnum = logicalrep_rel_att_by_name(remoterel,
												NameStr(attr->attname));

			entry->attrmap->attnums[i] = attnum;
			if (attnum >= 0)
			{
				/* Remember which subscriber columns are generated.
				 *
				 * 记住订阅端哪些列是生成列。
				 */
				if (attr->attgenerated)
					generatedattrs = bms_add_member(generatedattrs, attnum);

				missingatts = bms_del_member(missingatts, attnum);
			}
		}

		logicalrep_report_missing_or_gen_attrs(remoterel, missingatts,
											   generatedattrs);

		/* be tidy
		 *
		 * 收拾干净。
		 */
		bms_free(generatedattrs);
		bms_free(missingatts);

		/*
		 * Set if the table's replica identity is enough to apply
		 * update/delete.
		 *
		 * 设置该表的副本标识是否足以应用 UPDATE 和 DELETE。
		 */
		logicalrep_rel_mark_updatable(entry);

		/*
		 * Finding a usable index is an infrequent task. It occurs when an
		 * operation is first performed on the relation, or after invalidation
		 * of the relation cache entry (such as ANALYZE or CREATE/DROP index
		 * on the relation).
		 *
		 * 寻找可用索引并不频繁。它发生在对该关系的第一次操作时，或关系缓存项失效之后，例如 ANALYZE，或对该关系 CREATE 或 DROP 索引。
		 */
		entry->localindexoid = FindLogicalRepLocalIndex(entry->localrel, remoterel,
														entry->attrmap);

		entry->localrelvalid = true;
	}

	if (entry->state != SUBREL_STATE_READY)
		entry->state = GetSubscriptionRelState(MySubscription->oid,
											   entry->localreloid,
											   &entry->statelsn);

	return entry;
}

/*
 * Close the previously opened logical relation.
 *
 * 关闭先前打开的逻辑关系。
 */
void
logicalrep_rel_close(LogicalRepRelMapEntry *rel, LOCKMODE lockmode)
{
	table_close(rel->localrel, lockmode);
	rel->localrel = NULL;
}

/*
 * Partition cache: look up partition LogicalRepRelMapEntry's
 *
 * 分区缓存：查找分区的 LogicalRepRelMapEntry。
 *
 * Unlike relation map cache, this is keyed by partition OID, not remote
 * relation OID, because we only have to use this cache in the case where
 * partitions are not directly mapped to any remote relation, such as when
 * replication is occurring with one of their ancestors as target.
 *
 * 与关系映射缓存不同，这里以分区 OID 为键，而不是远端关系 OID。
 * 只有分区没有直接映射到任何远端关系时才用这个缓存，例如复制目标是它们的某个祖先。
 */

/*
 * Relcache invalidation callback
 *
 * relcache 失效回调。
 */
static void
logicalrep_partmap_invalidate_cb(Datum arg, Oid reloid)
{
	LogicalRepPartMapEntry *entry;

	/* Just to be sure.
	 *
	 * 再确认一次。
	 */
	if (LogicalRepPartMap == NULL)
		return;

	if (reloid != InvalidOid)
	{
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		/* TODO, use inverse lookup hashtable?
		 *
		 * 待办：是否改用反向查找哈希表？
		 */
		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->relmapentry.localreloid == reloid)
			{
				entry->relmapentry.localrelvalid = false;
				hash_seq_term(&status);
				break;
			}
		}
	}
	else
	{
		/* invalidate all cache entries
		 *
		 * 使全部缓存项失效。
		 */
		HASH_SEQ_STATUS status;

		hash_seq_init(&status, LogicalRepPartMap);

		while ((entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
			entry->relmapentry.localrelvalid = false;
	}
}

/*
 * Reset the entries in the partition map that refer to remoterel.
 *
 * 重置分区映射中引用 remoterel 的项。
 *
 * Called when new relation mapping is sent by the publisher to update our
 * expected view of incoming data from said publisher.
 *
 * 发布端发来新的关系映射时调用，用来更新我们对这份数据的预期。
 *
 * Note that we don't update the remoterel information in the entry here,
 * we will update the information in logicalrep_partition_open to avoid
 * unnecessary work.
 *
 * 这里不更新项里的 remoterel 信息，留到 logicalrep_partition_open 再更新，以免做无用功。
 */
void
logicalrep_partmap_reset_relmap(LogicalRepRelation *remoterel)
{
	HASH_SEQ_STATUS status;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelMapEntry *entry;

	if (LogicalRepPartMap == NULL)
		return;

	hash_seq_init(&status, LogicalRepPartMap);
	while ((part_entry = (LogicalRepPartMapEntry *) hash_seq_search(&status)) != NULL)
	{
		entry = &part_entry->relmapentry;

		if (entry->remoterel.remoteid != remoterel->remoteid)
			continue;

		logicalrep_relmap_free_entry(entry);

		memset(entry, 0, sizeof(LogicalRepRelMapEntry));
	}
}

/*
 * Initialize the partition map cache.
 *
 * 初始化分区映射缓存。
 */
static void
logicalrep_partmap_init(void)
{
	HASHCTL		ctl;

	if (!LogicalRepPartMapContext)
		LogicalRepPartMapContext =
			AllocSetContextCreate(CacheMemoryContext,
								  "LogicalRepPartMapContext",
								  ALLOCSET_DEFAULT_SIZES);

	/* Initialize the relation hash table.
	 *
	 * 初始化关系哈希表。
	 */
	ctl.keysize = sizeof(Oid);	/* partition OID
								 *
								 * 分区 OID。
								 */
	ctl.entrysize = sizeof(LogicalRepPartMapEntry);
	ctl.hcxt = LogicalRepPartMapContext;

	LogicalRepPartMap = hash_create("logicalrep partition map cache", 64, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Watch for invalidation events.
	 *
	 * 监听失效事件。
	 */
	CacheRegisterRelcacheCallback(logicalrep_partmap_invalidate_cb,
								  (Datum) 0);
}

/*
 * logicalrep_partition_open
 *
 * 打开逻辑复制分区的映射项。
 *
 * Returned entry reuses most of the values of the root table's entry, save
 * the attribute map, which can be different for the partition.  However,
 * we must physically copy all the data, in case the root table's entry
 * gets freed/rebuilt.
 *
 * 返回的项复用根表项的大部分值，但属性映射可能因分区而不同。
 * 尽管如此，必须把全部数据实际复制一份，以防根表项被释放或重建。
 *
 * Note there's no logicalrep_partition_close, because the caller closes the
 * component relation.
 *
 * 没有 logicalrep_partition_close，因为调用者会关闭组成关系。
 */
LogicalRepRelMapEntry *
logicalrep_partition_open(LogicalRepRelMapEntry *root,
						  Relation partrel, AttrMap *map)
{
	LogicalRepRelMapEntry *entry;
	LogicalRepPartMapEntry *part_entry;
	LogicalRepRelation *remoterel = &root->remoterel;
	Oid			partOid = RelationGetRelid(partrel);
	AttrMap    *attrmap = root->attrmap;
	bool		found;
	MemoryContext oldctx;

	if (LogicalRepPartMap == NULL)
		logicalrep_partmap_init();

	/* Search for existing entry.
	 *
	 * 查找已有项。
	 */
	part_entry = (LogicalRepPartMapEntry *) hash_search(LogicalRepPartMap,
														&partOid,
														HASH_ENTER, &found);

	entry = &part_entry->relmapentry;

	/*
	 * We must always overwrite entry->localrel with the latest partition
	 * Relation pointer, because the Relation pointed to by the old value may
	 * have been cleared after the caller would have closed the partition
	 * relation after the last use of this entry.  Note that localrelvalid is
	 * only updated by the relcache invalidation callback, so it may still be
	 * true irrespective of whether the Relation pointed to by localrel has
	 * been cleared or not.
	 *
	 * 必须总是用最新的分区 Relation 指针覆盖 entry 的 localrel。调用者上次用完并关闭分区关系后，旧指针指向的 Relation 可能已被清掉。
	 * localrelvalid 只由 relcache 失效回调更新，所以即使 localrel 指向的 Relation 已被清掉，它仍可能为真。
	 */
	if (found && entry->localrelvalid)
	{
		entry->localrel = partrel;
		return entry;
	}

	/* Switch to longer-lived context.
	 *
	 * 切换到寿命更长的内存上下文。
	 */
	oldctx = MemoryContextSwitchTo(LogicalRepPartMapContext);

	if (!found)
	{
		memset(part_entry, 0, sizeof(LogicalRepPartMapEntry));
		part_entry->partoid = partOid;
	}

	/* Release the no-longer-useful attrmap, if any.
	 *
	 * 若还有已经没用的 attrmap，则释放它。
	 */
	if (entry->attrmap)
	{
		free_attrmap(entry->attrmap);
		entry->attrmap = NULL;
	}

	if (!entry->remoterel.remoteid)
	{
		int			i;

		/* Remote relation is copied as-is from the root entry.
		 *
		 * 远端关系从根表项原样复制。
		 */
		entry->remoterel.remoteid = remoterel->remoteid;
		entry->remoterel.nspname = pstrdup(remoterel->nspname);
		entry->remoterel.relname = pstrdup(remoterel->relname);
		entry->remoterel.natts = remoterel->natts;
		entry->remoterel.attnames = palloc(remoterel->natts * sizeof(char *));
		entry->remoterel.atttyps = palloc(remoterel->natts * sizeof(Oid));
		for (i = 0; i < remoterel->natts; i++)
		{
			entry->remoterel.attnames[i] = pstrdup(remoterel->attnames[i]);
			entry->remoterel.atttyps[i] = remoterel->atttyps[i];
		}
		entry->remoterel.replident = remoterel->replident;
		entry->remoterel.attkeys = bms_copy(remoterel->attkeys);
	}

	entry->localrel = partrel;
	entry->localreloid = partOid;

	/*
	 * If the partition's attributes don't match the root relation's, we'll
	 * need to make a new attrmap which maps partition attribute numbers to
	 * remoterel's, instead of the original which maps root relation's
	 * attribute numbers to remoterel's.
	 *
	 * 若分区属性与根关系不一致，需要新建 attrmap，把分区属性号映射到 remoterel，
	 * 而不是沿用把根关系属性号映射到 remoterel 的那份。
	 *
	 * Note that 'map' which comes from the tuple routing data structure
	 * contains 1-based attribute numbers (of the parent relation).  However,
	 * the map in 'entry', a logical replication data structure, contains
	 * 0-based attribute numbers (of the remote relation).
	 *
	 * 来自元组路由结构的 map 含的是父关系的从 1 起算的属性号。
	 * entry 这份逻辑复制结构里的 map 含的是远端关系的从 0 起算的属性号。
	 */
	if (map)
	{
		AttrNumber	attno;

		entry->attrmap = make_attrmap(map->maplen);
		for (attno = 0; attno < entry->attrmap->maplen; attno++)
		{
			AttrNumber	root_attno = map->attnums[attno];

			/* 0 means it's a dropped attribute.  See comments atop AttrMap.
			 *
			 * 0 表示该属性已删除。见 AttrMap 顶部的注释。
			 */
			if (root_attno == 0)
				entry->attrmap->attnums[attno] = -1;
			else
				entry->attrmap->attnums[attno] = attrmap->attnums[root_attno - 1];
		}
	}
	else
	{
		/* Lacking copy_attmap, do this the hard way.
		 *
		 * 没有 copy_attmap，只好手工复制。
		 */
		entry->attrmap = make_attrmap(attrmap->maplen);
		memcpy(entry->attrmap->attnums, attrmap->attnums,
			   attrmap->maplen * sizeof(AttrNumber));
	}

	/* Set if the table's replica identity is enough to apply update/delete.
	 *
	 * 设置该表的副本标识是否足以应用 UPDATE 和 DELETE。
	 */
	logicalrep_rel_mark_updatable(entry);

	/* state and statelsn are left set to 0.
	 *
	 * state 和 statelsn 保持为 0。
	 */
	MemoryContextSwitchTo(oldctx);

	/*
	 * Finding a usable index is an infrequent task. It occurs when an
	 * operation is first performed on the relation, or after invalidation of
	 * the relation cache entry (such as ANALYZE or CREATE/DROP index on the
	 * relation).
	 *
	 * 寻找可用索引并不频繁。它发生在对该关系的第一次操作时，或关系缓存项失效之后，例如 ANALYZE，或对该关系 CREATE 或 DROP 索引。
	 *
	 * We also prefer to run this code on the oldctx so that we do not leak
	 * anything in the LogicalRepPartMapContext (hence CacheMemoryContext).
	 *
	 * 这段代码也放在 oldctx 里跑，以免往 LogicalRepPartMapContext，也就是 CacheMemoryContext 里泄漏。
	 */
	entry->localindexoid = FindLogicalRepLocalIndex(partrel, remoterel,
													entry->attrmap);

	entry->localrelvalid = true;

	return entry;
}

/*
 * Returns the oid of an index that can be used by the apply worker to scan
 * the relation.
 *
 * 返回应用工作进程用来扫描该关系的索引 OID。
 *
 * We expect to call this function when REPLICA IDENTITY FULL is defined for
 * the remote relation.
 *
 * 预期在远端关系定义为 REPLICA IDENTITY FULL 时调用本函数。
 *
 * If no suitable index is found, returns InvalidOid.
 *
 * 找不到合适的索引则返回 InvalidOid。
 */
static Oid
FindUsableIndexForReplicaIdentityFull(Relation localrel, AttrMap *attrmap)
{
	List	   *idxlist = RelationGetIndexList(localrel);

	foreach_oid(idxoid, idxlist)
	{
		bool		isUsableIdx;
		Relation	idxRel;

		idxRel = index_open(idxoid, AccessShareLock);
		isUsableIdx = IsIndexUsableForReplicaIdentityFull(idxRel, attrmap);
		index_close(idxRel, AccessShareLock);

		/* Return the first eligible index found
		 *
		 * 返回找到的第一个合格索引。
		 */
		if (isUsableIdx)
			return idxoid;
	}

	return InvalidOid;
}

/*
 * Returns true if the index is usable for replica identity full.
 *
 * 若该索引可用于 REPLICA IDENTITY FULL，则返回 true。
 *
 * The index must have an equal strategy for each key column, be non-partial,
 * and the leftmost field must be a column (not an expression) that references
 * the remote relation column. These limitations help to keep the index scan
 * similar to PK/RI index scans.
 *
 * 索引的每个键列都必须有等值策略，不能是部分索引，且最左字段必须是引用远端关系列的普通列，不能是表达式。
 * 这些限制是为了让索引扫描接近主键或副本标识索引扫描。
 *
 * attrmap is a map of local attributes to remote ones. We can consult this
 * map to check whether the local index attribute has a corresponding remote
 * attribute.
 *
 * attrmap 把本地属性映射到远端属性。可以用它检查本地索引属性是否有对应的远端属性。
 *
 * Note that the limitations of index scans for replica identity full only
 * adheres to a subset of the limitations of PK/RI. For example, we support
 * columns that are marked as [NULL] or we are not interested in the [NOT
 * DEFERRABLE] aspect of constraints here. It works for us because we always
 * compare the tuples for non-PK/RI index scans. See
 * RelationFindReplTupleByIndex().
 *
 * REPLICA IDENTITY FULL 的索引扫描限制只是主键或副本标识限制的一个子集。
 * 例如这里接受标成 NULL 的列，也不关心约束是否 NOT DEFERRABLE。
 * 这是因为非主键、非副本标识的索引扫描总会再比较元组。见 RelationFindReplTupleByIndex()。
 *
 * XXX: To support partial indexes, the required changes are likely to be larger.
 * If none of the tuples satisfy the expression for the index scan, we fall-back
 * to sequential execution, which might not be a good idea in some cases.
 *
 * 待办：要支持部分索引，改动可能会大得多。若没有元组满足索引扫描表达式，就会退回顺序扫描，有些情况下这并不合适。
 */
bool
IsIndexUsableForReplicaIdentityFull(Relation idxrel, AttrMap *attrmap)
{
	AttrNumber	keycol;
	oidvector  *indclass;

	/* The index must not be a partial index
	 *
	 * 索引不能是部分索引。
	 */
	if (!heap_attisnull(idxrel->rd_indextuple, Anum_pg_index_indpred, NULL))
		return false;

	Assert(idxrel->rd_index->indnatts >= 1);

	indclass = (oidvector *) DatumGetPointer(SysCacheGetAttrNotNull(INDEXRELID,
																	idxrel->rd_indextuple,
																	Anum_pg_index_indclass));

	/* Ensure that the index has a valid equal strategy for each key column
	 *
	 * 确保索引的每个键列都有有效的等值策略。
	 */
	for (int i = 0; i < idxrel->rd_index->indnkeyatts; i++)
	{
		Oid			opfamily;

		opfamily = get_opclass_family(indclass->values[i]);
		if (IndexAmTranslateCompareType(COMPARE_EQ, idxrel->rd_rel->relam, opfamily, true) == InvalidStrategy)
			return false;
	}

	/*
	 * For indexes other than PK and REPLICA IDENTITY, we need to match the
	 * local and remote tuples.  The equality routine tuples_equal() cannot
	 * accept a data type where the type cache cannot provide an equality
	 * operator.
	 *
	 * 主键和副本标识以外的索引需要匹配本地元组与远端元组。若类型缓存提供不了等值运算符，tuples_equal() 不能接受这种数据类型。
	 */
	for (int i = 0; i < idxrel->rd_att->natts; i++)
	{
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(TupleDescAttr(idxrel->rd_att, i)->atttypid, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			return false;
	}

	/* The leftmost index field must not be an expression
	 *
	 * 最左索引字段不能是表达式。
	 */
	keycol = idxrel->rd_index->indkey.values[0];
	if (!AttributeNumberIsValid(keycol))
		return false;

	/*
	 * And the leftmost index field must reference the remote relation column.
	 * This is because if it doesn't, the sequential scan is favorable over
	 * index scan in most cases.
	 *
	 * 并且最左索引字段必须引用远端关系的列。否则在多数情况下顺序扫描比索引扫描更合适。
	 */
	if (attrmap->maplen <= AttrNumberGetAttrOffset(keycol) ||
		attrmap->attnums[AttrNumberGetAttrOffset(keycol)] < 0)
		return false;

	/*
	 * The given index access method must implement "amgettuple", which will
	 * be used later to fetch the tuples.  See RelationFindReplTupleByIndex().
	 *
	 * 给定的索引访问方法必须实现 amgettuple，稍后用它取元组。见 RelationFindReplTupleByIndex()。
	 */
	if (GetIndexAmRoutineByAmId(idxrel->rd_rel->relam, false)->amgettuple == NULL)
		return false;

	return true;
}

/*
 * Return the OID of the replica identity index if one is defined;
 * the OID of the PK if one exists and is not deferrable;
 * otherwise, InvalidOid.
 *
 * 若定义了副本标识索引则返回其 OID；否则若有不可延迟的主键则返回主键 OID；都没有则返回 InvalidOid。
 */
Oid
GetRelationIdentityOrPK(Relation rel)
{
	Oid			idxoid;

	idxoid = RelationGetReplicaIndex(rel);

	if (!OidIsValid(idxoid))
		idxoid = RelationGetPrimaryKeyIndex(rel, false);

	return idxoid;
}

/*
 * Returns the index oid if we can use an index for subscriber. Otherwise,
 * returns InvalidOid.
 *
 * 若订阅端可以用某个索引，则返回该索引 OID，否则返回 InvalidOid。
 */
static Oid
FindLogicalRepLocalIndex(Relation localrel, LogicalRepRelation *remoterel,
						 AttrMap *attrMap)
{
	Oid			idxoid;

	/*
	 * We never need index oid for partitioned tables, always rely on leaf
	 * partition's index.
	 *
	 * 分区表本身不需要索引 OID，始终使用叶子分区的索引。
	 */
	if (localrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return InvalidOid;

	/*
	 * Simple case, we already have a primary key or a replica identity index.
	 *
	 * 简单情况：已经有主键或副本标识索引。
	 */
	idxoid = GetRelationIdentityOrPK(localrel);
	if (OidIsValid(idxoid))
		return idxoid;

	if (remoterel->replident == REPLICA_IDENTITY_FULL)
	{
		/*
		 * We are looking for one more opportunity for using an index. If
		 * there are any indexes defined on the local relation, try to pick a
		 * suitable index.
		 *
		 * 再找一次使用索引的机会。若本地关系上定义了索引，就试着选一个合适的。
		 *
		 * The index selection safely assumes that all the columns are going
		 * to be available for the index scan given that remote relation has
		 * replica identity full.
		 *
		 * 远端关系是 REPLICA IDENTITY FULL 时，选索引可以放心假定索引扫描用得到全部列。
		 *
		 * Note that we are not using the planner to find the cheapest method
		 * to scan the relation as that would require us to either use lower
		 * level planner functions which would be a maintenance burden in the
		 * long run or use the full-fledged planner which could cause
		 * overhead.
		 *
		 * 这里不用规划器来找扫描该关系的最便宜方法。那要么要用底层规划器函数，长期维护负担大，要么用完整规划器，开销可能很大。
		 */
		return FindUsableIndexForReplicaIdentityFull(localrel, attrMap);
	}

	return InvalidOid;
}
