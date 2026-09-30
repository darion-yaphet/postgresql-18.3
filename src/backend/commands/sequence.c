/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  PostgreSQL sequences support code.
 *
 * PostgreSQL 序列支持代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/sequence.c
 *
 * 标识
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/sequence.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type.h"
#include "catalog/storage_xlog.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * We don't want to log each fetching of a value from a sequence,
 * so we pre-log a few fetches in advance. In the event of
 * crash we can lose (skip over) as many values as we pre-logged.
 *
 * 我们不想为每次从序列取值都写日志，因此预先记录若干次取值。
 * 若发生崩溃，最多会丢失（跳过）预先记录的那么多个值。
 */
#define SEQ_LOG_VALS	32

/*
 * The "special area" of a sequence's buffer page looks like this.
 *
 * 序列缓冲页的特殊区域如下所示。
 */
#define SEQ_MAGIC	  0x1717

typedef struct sequence_magic
{
	uint32		magic;
} sequence_magic;

/*
 * We store a SeqTable item for every sequence we have touched in the current
 * session.  This is needed to hold onto nextval/currval state.  (We can't
 * rely on the relcache, since it's only, well, a cache, and may decide to
 * discard entries.)
 *
 * 对本会话中接触过的每个序列保存一个 SeqTable 项。
 * 这是为了保住 nextval/currval 的状态。不能依赖 relcache，因为它只是缓存，可能会丢弃条目。
 */
typedef struct SeqTableData
{
	Oid			relid;			/* pg_class OID of this sequence (hash key) */
	/*
	 *
	 * 该序列的 pg_class OID（哈希键）
	 */
	RelFileNumber filenumber;	/* last seen relfilenumber of this sequence */
	/*
	 *
	 * 最近一次看到的该序列 relfilenumber
	 */
	LocalTransactionId lxid;	/* xact in which we last did a seq op */
	/*
	 *
	 * 最近一次序列操作所在的事务
	 */
	bool		last_valid;		/* do we have a valid "last" value? */
	/*
	 *
	 * 是否已有有效的 last 值？
	 */
	int64		last;			/* value last returned by nextval */
	/*
	 *
	 * nextval 上次返回的值
	 */
	int64		cached;			/* last value already cached for nextval */
	/*
	 *
	 * 已为 nextval 缓存的最后一个值
	 */
	/* if last != cached, we have not used up all the cached values */
	/*
	 *
	 * 若 last != cached，说明尚未用完所有缓存值
	 */
	int64		increment;		/* copy of sequence's increment field */
	/*
	 *
	 * 序列 increment 字段的副本
	 */
	/* note that increment is zero until we first do nextval_internal() */
	/*
	 *
	 * 注意在第一次执行 nextval_internal() 之前，increment 为零
	 */
} SeqTableData;

typedef SeqTableData *SeqTable;

static HTAB *seqhashtab = NULL; /* hash table for SeqTable items */
/*
 *
 * 存放 SeqTable 项的哈希表
 */

/*
 * last_used_seq is updated by nextval() to point to the last used
 * sequence.
 *
 * last_used_seq 由 nextval() 更新，指向最近使用的序列。
 */
static SeqTableData *last_used_seq = NULL;

static void fill_seq_with_data(Relation rel, HeapTuple tuple);
static void fill_seq_fork_with_data(Relation rel, HeapTuple tuple, ForkNumber forkNum);
static Relation lock_and_open_sequence(SeqTable seq);
static void create_seq_hashtable(void);
static void init_sequence(Oid relid, SeqTable *p_elm, Relation *p_rel);
static Form_pg_sequence_data read_seq_tuple(Relation rel,
											Buffer *buf, HeapTuple seqdatatuple);
static void init_params(ParseState *pstate, List *options, bool for_identity,
						bool isInit,
						Form_pg_sequence seqform,
						Form_pg_sequence_data seqdataform,
						bool *need_seq_rewrite,
						List **owned_by);
static void do_setval(Oid relid, int64 next, bool iscalled);
static void process_owned_by(Relation seqrel, List *owned_by, bool for_identity);


/*
 * 核心流程概览：
 * DefineSequence / AlterSequence：创建或修改序列目录项与序列页。
 * nextval_internal：在序列页上推进取值，并用 SEQ_LOG_VALS 批量写 WAL。
 * currval_oid / lastval：读取本会话缓存的最近取值。
 * do_setval：直接设置 last_value 与 is_called。
 * seq_redo：崩溃恢复时重放序列页。
 */

/*
 * DefineSequence
 *				Creates a new sequence relation
 *
 * DefineSequence
 * 创建一个新的序列关系
 */
ObjectAddress
DefineSequence(ParseState *pstate, CreateSeqStmt *seq)
{
	FormData_pg_sequence seqform;
	FormData_pg_sequence_data seqdataform;
	bool		need_seq_rewrite;
	List	   *owned_by;
	CreateStmt *stmt = makeNode(CreateStmt);
	Oid			seqoid;
	ObjectAddress address;
	Relation	rel;
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	Datum		value[SEQ_COL_LASTCOL];
	bool		null[SEQ_COL_LASTCOL];
	Datum		pgs_values[Natts_pg_sequence];
	bool		pgs_nulls[Natts_pg_sequence];
	int			i;

	/*
	 * If if_not_exists was given and a relation with the same name already
	 * exists, bail out. (Note: we needn't check this when not if_not_exists,
	 * because DefineRelation will complain anyway.)
	 *
	 * 若给出了 if_not_exists 且已有同名关系，则退出。
	 * （注意：未给出 if_not_exists 时不必检查，因为 DefineRelation 反正会报错。）
	 */
	if (seq->if_not_exists)
	{
		RangeVarGetAndCheckCreationNamespace(seq->sequence, NoLock, &seqoid);
		if (OidIsValid(seqoid))
		{
			/*
			 * If we are in an extension script, insist that the pre-existing
			 * object be a member of the extension, to avoid security risks.
			 *
			 * 若处于扩展脚本中，则要求已存在的对象是该扩展的成员，以避免安全风险。
			 */
			ObjectAddressSet(address, RelationRelationId, seqoid);
			checkMembershipInCurrentExtension(&address);

			/* OK to skip */
			/*
			 *
			 * 可以跳过
			 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists, skipping",
							seq->sequence->relname)));
			return InvalidObjectAddress;
		}
	}

	/* Check and set all option values */
	/*
	 *
	 * 检查并设置全部选项值
	 */
	init_params(pstate, seq->options, seq->for_identity, true,
				&seqform, &seqdataform,
				&need_seq_rewrite, &owned_by);

	/*
	 * Create relation (and fill value[] and null[] for the tuple)
	 *
	 * 创建关系（并填充元组的 value[] 与 null[]）
	 */
	stmt->tableElts = NIL;
	for (i = SEQ_COL_FIRSTCOL; i <= SEQ_COL_LASTCOL; i++)
	{
		ColumnDef  *coldef = NULL;

		switch (i)
		{
			case SEQ_COL_LASTVAL:
				coldef = makeColumnDef("last_value", INT8OID, -1, InvalidOid);
				value[i - 1] = Int64GetDatumFast(seqdataform.last_value);
				break;
			case SEQ_COL_LOG:
				coldef = makeColumnDef("log_cnt", INT8OID, -1, InvalidOid);
				value[i - 1] = Int64GetDatum((int64) 0);
				break;
			case SEQ_COL_CALLED:
				coldef = makeColumnDef("is_called", BOOLOID, -1, InvalidOid);
				value[i - 1] = BoolGetDatum(false);
				break;
		}

		coldef->is_not_null = true;
		null[i - 1] = false;

		stmt->tableElts = lappend(stmt->tableElts, coldef);
	}

	stmt->relation = seq->sequence;
	stmt->inhRelations = NIL;
	stmt->constraints = NIL;
	stmt->options = NIL;
	stmt->oncommit = ONCOMMIT_NOOP;
	stmt->tablespacename = NULL;
	stmt->if_not_exists = seq->if_not_exists;

	address = DefineRelation(stmt, RELKIND_SEQUENCE, seq->ownerId, NULL, NULL);
	seqoid = address.objectId;
	Assert(seqoid != InvalidOid);

	rel = sequence_open(seqoid, AccessExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	/* now initialize the sequence's data */
	/*
	 *
	 * 现在初始化序列的数据
	 */
	tuple = heap_form_tuple(tupDesc, value, null);
	fill_seq_with_data(rel, tuple);

	/* process OWNED BY if given */
	/*
	 *
	 * 若给出了 OWNED BY，则处理它
	 */
	if (owned_by)
		process_owned_by(rel, owned_by, seq->for_identity);

	sequence_close(rel, NoLock);

	/* fill in pg_sequence */
	/*
	 *
	 * 填写 pg_sequence
	 */
	rel = table_open(SequenceRelationId, RowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	memset(pgs_nulls, 0, sizeof(pgs_nulls));

	pgs_values[Anum_pg_sequence_seqrelid - 1] = ObjectIdGetDatum(seqoid);
	pgs_values[Anum_pg_sequence_seqtypid - 1] = ObjectIdGetDatum(seqform.seqtypid);
	pgs_values[Anum_pg_sequence_seqstart - 1] = Int64GetDatumFast(seqform.seqstart);
	pgs_values[Anum_pg_sequence_seqincrement - 1] = Int64GetDatumFast(seqform.seqincrement);
	pgs_values[Anum_pg_sequence_seqmax - 1] = Int64GetDatumFast(seqform.seqmax);
	pgs_values[Anum_pg_sequence_seqmin - 1] = Int64GetDatumFast(seqform.seqmin);
	pgs_values[Anum_pg_sequence_seqcache - 1] = Int64GetDatumFast(seqform.seqcache);
	pgs_values[Anum_pg_sequence_seqcycle - 1] = BoolGetDatum(seqform.seqcycle);

	tuple = heap_form_tuple(tupDesc, pgs_values, pgs_nulls);
	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Reset a sequence to its initial value.
 *
 * 把序列重置为初始值。
 *
 * The change is made transactionally, so that on failure of the current
 * transaction, the sequence will be restored to its previous state.
 * We do that by creating a whole new relfilenumber for the sequence; so this
 * works much like the rewriting forms of ALTER TABLE.
 *
 * 该变更是事务性的，因此若当前事务失败，序列会恢复到先前状态。
 * 做法是为序列创建一个全新的 relfilenumber；因此这很像会重写的 ALTER TABLE 形式。
 *
 * Caller is assumed to have acquired AccessExclusiveLock on the sequence,
 * which must not be released until end of transaction.  Caller is also
 * responsible for permissions checking.
 *
 * 假定调用方已取得序列上的 AccessExclusiveLock，并且直到事务结束都不能释放。调用方还负责权限检查。
 */
void
ResetSequence(Oid seq_relid)
{
	Relation	seq_rel;
	SeqTable	elm;
	Form_pg_sequence_data seq;
	Buffer		buf;
	HeapTupleData seqdatatuple;
	HeapTuple	tuple;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	int64		startv;

	/*
	 * Read the old sequence.  This does a bit more work than really
	 * necessary, but it's simple, and we do want to double-check that it's
	 * indeed a sequence.
	 *
	 * 读取旧序列。这比严格必要的工作多一点，但比较简单，而且我们确实想再确认它是序列。
	 */
	init_sequence(seq_relid, &elm, &seq_rel);
	(void) read_seq_tuple(seq_rel, &buf, &seqdatatuple);

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(seq_relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", seq_relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	startv = pgsform->seqstart;
	ReleaseSysCache(pgstuple);

	/*
	 * Copy the existing sequence tuple.
	 *
	 * 复制现有的序列元组。
	 */
	tuple = heap_copytuple(&seqdatatuple);

	/* Now we're done with the old page */
	/*
	 *
	 * 旧页已经处理完
	 */
	UnlockReleaseBuffer(buf);

	/*
	 * Modify the copied tuple to execute the restart (compare the RESTART
	 * action in AlterSequence)
	 *
	 * 修改复制出来的元组以执行重启（对照 AlterSequence 中的 RESTART 动作）
	 */
	seq = (Form_pg_sequence_data) GETSTRUCT(tuple);
	seq->last_value = startv;
	seq->is_called = false;
	seq->log_cnt = 0;

	/*
	 * Create a new storage file for the sequence.
	 *
	 * 为序列创建新的存储文件。
	 */
	RelationSetNewRelfilenumber(seq_rel, seq_rel->rd_rel->relpersistence);

	/*
	 * Ensure sequence's relfrozenxid is at 0, since it won't contain any
	 * unfrozen XIDs.  Same with relminmxid, since a sequence will never
	 * contain multixacts.
	 *
	 * 确保序列的 relfrozenxid 为 0，因为它不会包含任何未冻结的 XID。
	 * relminmxid 同样如此，因为序列永远不会包含 multixact。
	 */
	Assert(seq_rel->rd_rel->relfrozenxid == InvalidTransactionId);
	Assert(seq_rel->rd_rel->relminmxid == InvalidMultiXactId);

	/*
	 * Insert the modified tuple into the new storage file.
	 *
	 * 把修改后的元组插入新的存储文件。
	 */
	fill_seq_with_data(seq_rel, tuple);

	/* Clear local cache so that we don't think we have cached numbers */
	/*
	 *
	 * 清除本地缓存，以免误以为还有已缓存的数值
	 */
	/* Note that we do not change the currval() state */
	/*
	 *
	 * 注意我们不改变 currval() 的状态
	 */
	elm->cached = elm->last;

	sequence_close(seq_rel, NoLock);
}

/*
 * Initialize a sequence's relation with the specified tuple as content
 *
 * 用指定元组作为内容，初始化序列关系
 *
 * This handles unlogged sequences by writing to both the main and the init
 * fork as necessary.
 *
 * 对 unlogged 序列，按需要同时写入主 fork 与 init fork。
 */
static void
fill_seq_with_data(Relation rel, HeapTuple tuple)
{
	fill_seq_fork_with_data(rel, tuple, MAIN_FORKNUM);

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED)
	{
		SMgrRelation srel;

		srel = smgropen(rel->rd_locator, INVALID_PROC_NUMBER);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(&rel->rd_locator, INIT_FORKNUM);
		fill_seq_fork_with_data(rel, tuple, INIT_FORKNUM);
		FlushRelationBuffers(rel);
		smgrclose(srel);
	}
}

/*
 * Initialize a sequence's relation fork with the specified tuple as content
 *
 * 用指定元组作为内容，初始化序列关系的某个 fork
 */
static void
fill_seq_fork_with_data(Relation rel, HeapTuple tuple, ForkNumber forkNum)
{
	Buffer		buf;
	Page		page;
	sequence_magic *sm;
	OffsetNumber offnum;

	/* Initialize first page of relation with special magic number */
	/*
	 *
	 * 用特殊魔数初始化关系的第一页
	 */

	buf = ExtendBufferedRel(BMR_REL(rel), forkNum, NULL,
							EB_LOCK_FIRST | EB_SKIP_EXTENSION_LOCK);
	Assert(BufferGetBlockNumber(buf) == 0);

	page = BufferGetPage(buf);

	PageInit(page, BufferGetPageSize(buf), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(page);
	sm->magic = SEQ_MAGIC;

	/* Now insert sequence tuple */
	/*
	 *
	 * 现在插入序列元组
	 */

	/*
	 * Since VACUUM does not process sequences, we have to force the tuple to
	 * have xmin = FrozenTransactionId now.  Otherwise it would become
	 * invisible to SELECTs after 2G transactions.  It is okay to do this
	 * because if the current transaction aborts, no other xact will ever
	 * examine the sequence tuple anyway.
	 *
	 * VACUUM 不处理序列，因此必须现在就把元组的 xmin 强制为 FrozenTransactionId。
	 * 否则经过 2G 个事务后，SELECT 将看不到它。这样做是安全的，
	 * 因为若当前事务中止，不会有其他事务去查看该序列元组。
	 */
	HeapTupleHeaderSetXmin(tuple->t_data, FrozenTransactionId);
	HeapTupleHeaderSetXminFrozen(tuple->t_data);
	HeapTupleHeaderSetCmin(tuple->t_data, FirstCommandId);
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
	tuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
	ItemPointerSet(&tuple->t_data->t_ctid, 0, FirstOffsetNumber);

	/* check the comment above nextval_internal()'s equivalent call. */
	/*
	 *
	 * 参见上面 nextval_internal() 中对应调用处的注释。
	 */
	if (RelationNeedsWAL(rel))
		GetTopTransactionId();

	START_CRIT_SECTION();

	MarkBufferDirty(buf);

	offnum = PageAddItem(page, (Item) tuple->t_data, tuple->t_len,
						 InvalidOffsetNumber, false, false);
	if (offnum != FirstOffsetNumber)
		elog(ERROR, "failed to add sequence tuple to page");

	/* XLOG stuff */
	/*
	 *
	 * XLOG 相关处理
	 */
	if (RelationNeedsWAL(rel) || forkNum == INIT_FORKNUM)
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		xlrec.locator = rel->rd_locator;

		XLogRegisterData(&xlrec, sizeof(xl_seq_rec));
		XLogRegisterData(tuple->t_data, tuple->t_len);

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);
}

/*
 * AlterSequence
 *
 * 修改序列定义
 *
 * Modify the definition of a sequence relation
 *
 * 修改序列关系的定义
 */
ObjectAddress
AlterSequence(ParseState *pstate, AlterSeqStmt *stmt)
{
	Oid			relid;
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	HeapTupleData datatuple;
	Form_pg_sequence seqform;
	Form_pg_sequence_data newdataform;
	bool		need_seq_rewrite;
	List	   *owned_by;
	ObjectAddress address;
	Relation	rel;
	HeapTuple	seqtuple;
	HeapTuple	newdatatuple;

	/* Open and lock sequence, and check for ownership along the way. */
	/*
	 *
	 * 打开并锁定序列，同时检查属主。
	 */
	relid = RangeVarGetRelidExtended(stmt->sequence,
									 ShareRowExclusiveLock,
									 stmt->missing_ok ? RVR_MISSING_OK : 0,
									 RangeVarCallbackOwnsRelation,
									 NULL);
	if (relid == InvalidOid)
	{
		ereport(NOTICE,
				(errmsg("relation \"%s\" does not exist, skipping",
						stmt->sequence->relname)));
		return InvalidObjectAddress;
	}

	init_sequence(relid, &elm, &seqrel);

	rel = table_open(SequenceRelationId, RowExclusiveLock);
	seqtuple = SearchSysCacheCopy1(SEQRELID,
								   ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(seqtuple))
		elog(ERROR, "cache lookup failed for sequence %u",
			 relid);

	seqform = (Form_pg_sequence) GETSTRUCT(seqtuple);

	/* lock page buffer and read tuple into new sequence structure */
	/*
	 *
	 * 锁定页缓冲区，并把元组读入新的序列结构
	 */
	(void) read_seq_tuple(seqrel, &buf, &datatuple);

	/* copy the existing sequence data tuple, so it can be modified locally */
	/*
	 *
	 * 复制现有的序列数据元组，以便在本地修改
	 */
	newdatatuple = heap_copytuple(&datatuple);
	newdataform = (Form_pg_sequence_data) GETSTRUCT(newdatatuple);

	UnlockReleaseBuffer(buf);

	/* Check and set new values */
	/*
	 *
	 * 检查并设置新值
	 */
	init_params(pstate, stmt->options, stmt->for_identity, false,
				seqform, newdataform,
				&need_seq_rewrite, &owned_by);

	/* If needed, rewrite the sequence relation itself */
	/*
	 *
	 * 若需要，重写序列关系本身
	 */
	if (need_seq_rewrite)
	{
		/* check the comment above nextval_internal()'s equivalent call. */
		/*
		 *
		 * 参见上面 nextval_internal() 中对应调用处的注释。
		 */
		if (RelationNeedsWAL(seqrel))
			GetTopTransactionId();

		/*
		 * Create a new storage file for the sequence, making the state
		 * changes transactional.
		 *
		 * 为序列创建新的存储文件，使状态变更具有事务性。
		 */
		RelationSetNewRelfilenumber(seqrel, seqrel->rd_rel->relpersistence);

		/*
		 * Ensure sequence's relfrozenxid is at 0, since it won't contain any
		 * unfrozen XIDs.  Same with relminmxid, since a sequence will never
		 * contain multixacts.
		 *
		 * 确保序列的 relfrozenxid 为 0，因为它不会包含任何未冻结的 XID。
		 * relminmxid 同样如此，因为序列永远不会包含 multixact。
		 */
		Assert(seqrel->rd_rel->relfrozenxid == InvalidTransactionId);
		Assert(seqrel->rd_rel->relminmxid == InvalidMultiXactId);

		/*
		 * Insert the modified tuple into the new storage file.
		 *
		 * 把修改后的元组插入新的存储文件。
		 */
		fill_seq_with_data(seqrel, newdatatuple);
	}

	/* Clear local cache so that we don't think we have cached numbers */
	/*
	 *
	 * 清除本地缓存，以免误以为还有已缓存的数值
	 */
	/* Note that we do not change the currval() state */
	/*
	 *
	 * 注意我们不改变 currval() 的状态
	 */
	elm->cached = elm->last;

	/* process OWNED BY if given */
	/*
	 *
	 * 若给出了 OWNED BY，则处理它
	 */
	if (owned_by)
		process_owned_by(seqrel, owned_by, stmt->for_identity);

	/* update the pg_sequence tuple (we could skip this in some cases...) */
	/*
	 *
	 * 更新 pg_sequence 元组（某些情况下可以跳过……）
	 */
	CatalogTupleUpdate(rel, &seqtuple->t_self, seqtuple);

	InvokeObjectPostAlterHook(RelationRelationId, relid, 0);

	ObjectAddressSet(address, RelationRelationId, relid);

	table_close(rel, RowExclusiveLock);
	sequence_close(seqrel, NoLock);

	return address;
}

/*
 * 按新的持久化类型重建序列存储文件，供 ALTER SEQUENCE SET LOGGED/UNLOGGED 使用。
 */
void
SequenceChangePersistence(Oid relid, char newrelpersistence)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	HeapTupleData seqdatatuple;

	/*
	 * ALTER SEQUENCE acquires this lock earlier.  If we're processing an
	 * owned sequence for ALTER TABLE, lock now.  Without the lock, we'd
	 * discard increments from nextval() calls (in other sessions) between
	 * this function's buffer unlock and this transaction's commit.
	 *
	 * ALTER SEQUENCE 会更早取得这把锁。若正在为 ALTER TABLE 处理被拥有的序列，则现在加锁。
	 * 没有这把锁，就会丢掉本函数解锁缓冲区到本事务提交之间，其他会话 nextval() 调用所做的递增。
	 */
	LockRelationOid(relid, AccessExclusiveLock);
	init_sequence(relid, &elm, &seqrel);

	/* check the comment above nextval_internal()'s equivalent call. */
	/*
	 *
	 * 参见上面 nextval_internal() 中对应调用处的注释。
	 */
	if (RelationNeedsWAL(seqrel))
		GetTopTransactionId();

	(void) read_seq_tuple(seqrel, &buf, &seqdatatuple);
	RelationSetNewRelfilenumber(seqrel, newrelpersistence);
	fill_seq_with_data(seqrel, &seqdatatuple);
	UnlockReleaseBuffer(buf);

	sequence_close(seqrel, NoLock);
}

/*
 * 从 pg_sequence 中删除该序列对应的目录元组。
 */
void
DeleteSequenceTuple(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;

	rel = table_open(SequenceRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);

	CatalogTupleDelete(rel, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * Note: nextval with a text argument is no longer exported as a pg_proc
 * entry, but we keep it around to ease porting of C code that may have
 * called the function directly.
 *
 * 注意：以 text 为参数的 nextval 已不再作为 pg_proc 项导出，但保留它以便移植可能直接调用该函数的 C 代码。
 */
Datum
nextval(PG_FUNCTION_ARGS)
{
	text	   *seqin = PG_GETARG_TEXT_PP(0);
	RangeVar   *sequence;
	Oid			relid;

	sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin));

	/*
	 * XXX: This is not safe in the presence of concurrent DDL, but acquiring
	 * a lock here is more expensive than letting nextval_internal do it,
	 * since the latter maintains a cache that keeps us from hitting the lock
	 * manager more than once per transaction.  It's not clear whether the
	 * performance penalty is material in practice, but for now, we do it this
	 * way.
	 *
	 * XXX：在存在并发 DDL 时这并不安全，但在这里加锁比让 nextval_internal 去做更贵，
	 * 因为后者维护缓存，使每个事务不会多次访问锁管理器。
	 * 性能损失在实践中是否明显尚不清楚，目前仍采用这种方式。
	 */
	relid = RangeVarGetRelid(sequence, NoLock, false);

	PG_RETURN_INT64(nextval_internal(relid, true));
}

/*
 * 按 OID 调用 nextval_internal，供 nextval(regclass) 使用。
 */
Datum
nextval_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	PG_RETURN_INT64(nextval_internal(relid, true));
}

/*
 * nextval 的内部实现：锁定序列页，按 increment、cache 与 SEQ_LOG_VALS 推进并写 WAL。
 */
int64
nextval_internal(Oid relid, bool check_permissions)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	Page		page;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	HeapTupleData seqdatatuple;
	Form_pg_sequence_data seq;
	int64		incby,
				maxv,
				minv,
				cache,
				log,
				fetch,
				last;
	int64		result,
				next,
				rescnt = 0;
	bool		cycle;
	bool		logit = false;

	/* open and lock sequence */
	/*
	 *
	 * 打开并锁定序列
	 */
	init_sequence(relid, &elm, &seqrel);

	if (check_permissions &&
		pg_class_aclcheck(elm->relid, GetUserId(),
						  ACL_USAGE | ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	/* read-only transactions may only modify temp sequences */
	/*
	 *
	 * 只读事务只能修改临时序列
	 */
	if (!seqrel->rd_islocaltemp)
		PreventCommandIfReadOnly("nextval()");

	/*
	 * Forbid this during parallel operation because, to make it work, the
	 * cooperating backends would need to share the backend-local cached
	 * sequence information.  Currently, we don't support that.
	 *
	 * 并行操作期间禁止这样做。要让它工作，协作的后端需要共享后端本地缓存的序列信息。目前不支持这一点。
	 */
	PreventCommandIfParallelMode("nextval()");

	if (elm->last != elm->cached)	/* some numbers were cached */
	/*
	 *
	 * 有一些数值已被缓存
	 */
	{
		Assert(elm->last_valid);
		Assert(elm->increment != 0);
		elm->last += elm->increment;
		sequence_close(seqrel, NoLock);
		last_used_seq = elm;
		return elm->last;
	}

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	incby = pgsform->seqincrement;
	maxv = pgsform->seqmax;
	minv = pgsform->seqmin;
	cache = pgsform->seqcache;
	cycle = pgsform->seqcycle;
	ReleaseSysCache(pgstuple);

	/* lock page buffer and read tuple */
	/*
	 *
	 * 锁定页缓冲区并读取元组
	 */
	seq = read_seq_tuple(seqrel, &buf, &seqdatatuple);
	page = BufferGetPage(buf);

	last = next = result = seq->last_value;
	fetch = cache;
	log = seq->log_cnt;

	if (!seq->is_called)
	{
		rescnt++;				/* return last_value if not is_called */
		/*
		 *
		 * 若 is_called 为假，则返回 last_value
		 */
		fetch--;
	}

	/*
	 * Decide whether we should emit a WAL log record.  If so, force up the
	 * fetch count to grab SEQ_LOG_VALS more values than we actually need to
	 * cache.  (These will then be usable without logging.)
	 *
	 * 决定是否应发出 WAL 记录。若需要，则把取出个数提高到比实际需要缓存的值再多 SEQ_LOG_VALS 个。
	 * 这些额外的值随后可以在不写日志的情况下使用。
	 *
	 * If this is the first nextval after a checkpoint, we must force a new
	 * WAL record to be written anyway, else replay starting from the
	 * checkpoint would fail to advance the sequence past the logged values.
	 * In this case we may as well fetch extra values.
	 *
	 * 若这是检查点之后的第一次 nextval，无论如何都必须强制写一条新的 WAL 记录，
	 * 否则从该检查点开始的重放无法把序列推进到已记录的值之后。这种情况下不妨多取一些值。
	 */
	if (log < fetch || !seq->is_called)
	{
		/* forced log to satisfy local demand for values */
		/*
		 *
		 * 为满足本地对数值的需求而强制写日志
		 */
		fetch = log = fetch + SEQ_LOG_VALS;
		logit = true;
	}
	else
	{
		XLogRecPtr	redoptr = GetRedoRecPtr();

		if (PageGetLSN(page) <= redoptr)
		{
			/* last update of seq was before checkpoint */
			/*
			 *
			 * 序列的上次更新发生在检查点之前
			 */
			fetch = log = fetch + SEQ_LOG_VALS;
			logit = true;
		}
	}

	while (fetch)				/* try to fetch cache [+ log ] numbers */
	/*
	 *
	 * 尝试取出 cache（再加上 log）个数值
	 */
	{
		/*
		 * Check MAXVALUE for ascending sequences and MINVALUE for descending
		 * sequences
		 *
		 * 递增序列检查 MAXVALUE，递减序列检查 MINVALUE
		 */
		if (incby > 0)
		{
			/* ascending sequence */
			/*
			 *
			 * 递增序列
			 */
			if ((maxv >= 0 && next > maxv - incby) ||
				(maxv < 0 && next + incby > maxv))
			{
				if (rescnt > 0)
					break;		/* stop fetching */
					/*
					 *
					 * 停止取值
					 */
				if (!cycle)
					ereport(ERROR,
							(errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
							 errmsg("nextval: reached maximum value of sequence \"%s\" (%" PRId64 ")",
									RelationGetRelationName(seqrel),
									maxv)));
				next = minv;
			}
			else
				next += incby;
		}
		else
		{
			/* descending sequence */
			/*
			 *
			 * 递减序列
			 */
			if ((minv < 0 && next < minv - incby) ||
				(minv >= 0 && next + incby < minv))
			{
				if (rescnt > 0)
					break;		/* stop fetching */
					/*
					 *
					 * 停止取值
					 */
				if (!cycle)
					ereport(ERROR,
							(errcode(ERRCODE_SEQUENCE_GENERATOR_LIMIT_EXCEEDED),
							 errmsg("nextval: reached minimum value of sequence \"%s\" (%" PRId64 ")",
									RelationGetRelationName(seqrel),
									minv)));
				next = maxv;
			}
			else
				next += incby;
		}
		fetch--;
		if (rescnt < cache)
		{
			log--;
			rescnt++;
			last = next;
			if (rescnt == 1)	/* if it's first result - */
			/*
			 *
			 * 若这是第一个结果
			 */
				result = next;	/* it's what to return */
				/*
				 *
				 * 这就是要返回的值
				 */
		}
	}

	log -= fetch;				/* adjust for any unfetched numbers */
	/*
	 *
	 * 对尚未取出的数值做调整
	 */
	Assert(log >= 0);

	/* save info in local cache */
	/*
	 *
	 * 把信息保存到本地缓存
	 */
	elm->increment = incby;
	elm->last = result;			/* last returned number */
	/*
	 *
	 * 上次返回的数值
	 */
	elm->cached = last;			/* last fetched number */
	/*
	 *
	 * 最后取出的数值
	 */
	elm->last_valid = true;

	last_used_seq = elm;

	/*
	 * If something needs to be WAL logged, acquire an xid, so this
	 * transaction's commit will trigger a WAL flush and wait for syncrep.
	 * It's sufficient to ensure the toplevel transaction has an xid, no need
	 * to assign xids subxacts, that'll already trigger an appropriate wait.
	 * (Have to do that here, so we're outside the critical section)
	 *
	 * 若有内容需要写 WAL，则获取一个 xid，以便本事务提交时触发 WAL 刷盘并等待同步复制。
	 * 只要保证顶层事务拥有 xid 即可，不必给子事务分配 xid，那本来就会触发相应的等待。
	 * 必须在这里做，这样才处于临界区之外。
	 */
	if (logit && RelationNeedsWAL(seqrel))
		GetTopTransactionId();

	/* ready to change the on-disk (or really, in-buffer) tuple */
	/*
	 *
	 * 可以修改磁盘上（实际上是缓冲区中）的元组了
	 */
	START_CRIT_SECTION();

	/*
	 * We must mark the buffer dirty before doing XLogInsert(); see notes in
	 * SyncOneBuffer().  However, we don't apply the desired changes just yet.
	 * This looks like a violation of the buffer update protocol, but it is in
	 * fact safe because we hold exclusive lock on the buffer.  Any other
	 * process, including a checkpoint, that tries to examine the buffer
	 * contents will block until we release the lock, and then will see the
	 * final state that we install below.
	 *
	 * 必须在 XLogInsert() 之前把缓冲区标为脏；参见 SyncOneBuffer() 中的说明。
	 * 不过现在还不应用期望的修改。这看起来违反缓冲区更新协议，
	 * 但因为我们持有缓冲区的排他锁，实际上是安全的。
	 * 包括检查点在内的任何其他进程若要查看缓冲区内容，都会阻塞到我们释放锁，然后看到下面安装的最终状态。
	 */
	MarkBufferDirty(buf);

	/* XLOG stuff */
	/*
	 *
	 * XLOG 相关处理
	 */
	if (logit && RelationNeedsWAL(seqrel))
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;

		/*
		 * We don't log the current state of the tuple, but rather the state
		 * as it would appear after "log" more fetches.  This lets us skip
		 * that many future WAL records, at the cost that we lose those
		 * sequence values if we crash.
		 *
		 * 我们记录的不是元组的当前状态，而是再取 log 次之后应呈现的状态。
		 * 这样可以跳过那么多未来的 WAL 记录，代价是崩溃时会丢失这些序列值。
		 */
		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		/* set values that will be saved in xlog */
		/*
		 *
		 * 设置将写入 xlog 的值
		 */
		seq->last_value = next;
		seq->is_called = true;
		seq->log_cnt = 0;

		xlrec.locator = seqrel->rd_locator;

		XLogRegisterData(&xlrec, sizeof(xl_seq_rec));
		XLogRegisterData(seqdatatuple.t_data, seqdatatuple.t_len);

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG);

		PageSetLSN(page, recptr);
	}

	/* Now update sequence tuple to the intended final state */
	/*
	 *
	 * 现在把序列元组更新到预期的最终状态
	 */
	seq->last_value = last;		/* last fetched number */
	/*
	 *
	 * 最后取出的数值
	 */
	seq->is_called = true;
	seq->log_cnt = log;			/* how much is logged */
	/*
	 *
	 * 已记录了多少个值
	 */

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);

	sequence_close(seqrel, NoLock);

	return result;
}

/*
 * 返回本会话中该序列最近一次 nextval 取到的值。
 */
Datum
currval_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	SeqTable	elm;
	Relation	seqrel;

	/* open and lock sequence */
	/*
	 *
	 * 打开并锁定序列
	 */
	init_sequence(relid, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(),
						  ACL_SELECT | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	if (!elm->last_valid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("currval of sequence \"%s\" is not yet defined in this session",
						RelationGetRelationName(seqrel))));

	result = elm->last;

	sequence_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

/*
 * 返回本会话最近一次 nextval 所使用序列的当前值。
 */
Datum
lastval(PG_FUNCTION_ARGS)
{
	Relation	seqrel;
	int64		result;

	if (last_used_seq == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lastval is not yet defined in this session")));

	/* Someone may have dropped the sequence since the last nextval() */
	/*
	 *
	 * 自上次 nextval() 以来，可能有人删除了该序列
	 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(last_used_seq->relid)))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("lastval is not yet defined in this session")));

	seqrel = lock_and_open_sequence(last_used_seq);

	/* nextval() must have already been called for this sequence */
	/*
	 *
	 * 必须已经对该序列调用过 nextval()
	 */
	Assert(last_used_seq->last_valid);

	if (pg_class_aclcheck(last_used_seq->relid, GetUserId(),
						  ACL_SELECT | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	result = last_used_seq->last;
	sequence_close(seqrel, NoLock);

	PG_RETURN_INT64(result);
}

/*
 * Main internal procedure that handles 2 & 3 arg forms of SETVAL.
 *
 * 处理 SETVAL 两参数与三参数形式的主要内部过程。
 *
 * Note that the 3 arg version (which sets the is_called flag) is
 * only for use in pg_dump, and setting the is_called flag may not
 * work if multiple users are attached to the database and referencing
 * the sequence (unlikely if pg_dump is restoring it).
 *
 * 注意三参数版本（它设置 is_called 标志）只供 pg_dump 使用。
 * 若有多个用户连接数据库并引用该序列，设置 is_called 可能不会生效（pg_dump 恢复时不太可能出现这种情况）。
 *
 * It is necessary to have the 3 arg version so that pg_dump can
 * restore the state of a sequence exactly during data-only restores -
 * it is the only way to clear the is_called flag in an existing
 * sequence.
 *
 * 必须有三参数版本，pg_dump 才能在仅数据恢复时精确恢复序列状态。
 * 这是清除已有序列上 is_called 标志的唯一办法。
 */
static void
do_setval(Oid relid, int64 next, bool iscalled)
{
	SeqTable	elm;
	Relation	seqrel;
	Buffer		buf;
	HeapTupleData seqdatatuple;
	Form_pg_sequence_data seq;
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	int64		maxv,
				minv;

	/* open and lock sequence */
	/*
	 *
	 * 打开并锁定序列
	 */
	init_sequence(relid, &elm, &seqrel);

	if (pg_class_aclcheck(elm->relid, GetUserId(), ACL_UPDATE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						RelationGetRelationName(seqrel))));

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);
	maxv = pgsform->seqmax;
	minv = pgsform->seqmin;
	ReleaseSysCache(pgstuple);

	/* read-only transactions may only modify temp sequences */
	/*
	 *
	 * 只读事务只能修改临时序列
	 */
	if (!seqrel->rd_islocaltemp)
		PreventCommandIfReadOnly("setval()");

	/*
	 * Forbid this during parallel operation because, to make it work, the
	 * cooperating backends would need to share the backend-local cached
	 * sequence information.  Currently, we don't support that.
	 *
	 * 并行操作期间禁止这样做。要让它工作，协作的后端需要共享后端本地缓存的序列信息。目前不支持这一点。
	 */
	PreventCommandIfParallelMode("setval()");

	/* lock page buffer and read tuple */
	/*
	 *
	 * 锁定页缓冲区并读取元组
	 */
	seq = read_seq_tuple(seqrel, &buf, &seqdatatuple);

	if ((next < minv) || (next > maxv))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("setval: value %" PRId64 " is out of bounds for sequence \"%s\" (%" PRId64 "..%" PRId64 ")",
						next, RelationGetRelationName(seqrel),
						minv, maxv)));

	/* Set the currval() state only if iscalled = true */
	/*
	 *
	 * 仅当 iscalled = true 时设置 currval() 状态
	 */
	if (iscalled)
	{
		elm->last = next;		/* last returned number */
		/*
		 *
		 * 上次返回的数值
		 */
		elm->last_valid = true;
	}

	/* In any case, forget any future cached numbers */
	/*
	 *
	 * 无论如何，忘掉所有尚未使用的缓存数值
	 */
	elm->cached = elm->last;

	/* check the comment above nextval_internal()'s equivalent call. */
	/*
	 *
	 * 参见上面 nextval_internal() 中对应调用处的注释。
	 */
	if (RelationNeedsWAL(seqrel))
		GetTopTransactionId();

	/* ready to change the on-disk (or really, in-buffer) tuple */
	/*
	 *
	 * 可以修改磁盘上（实际上是缓冲区中）的元组了
	 */
	START_CRIT_SECTION();

	seq->last_value = next;		/* last fetched number */
	/*
	 *
	 * 最后取出的数值
	 */
	seq->is_called = iscalled;
	seq->log_cnt = 0;

	MarkBufferDirty(buf);

	/* XLOG stuff */
	/*
	 *
	 * XLOG 相关处理
	 */
	if (RelationNeedsWAL(seqrel))
	{
		xl_seq_rec	xlrec;
		XLogRecPtr	recptr;
		Page		page = BufferGetPage(buf);

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);

		xlrec.locator = seqrel->rd_locator;
		XLogRegisterData(&xlrec, sizeof(xl_seq_rec));
		XLogRegisterData(seqdatatuple.t_data, seqdatatuple.t_len);

		recptr = XLogInsert(RM_SEQ_ID, XLOG_SEQ_LOG);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buf);

	sequence_close(seqrel, NoLock);
}

/*
 * Implement the 2 arg setval procedure.
 * See do_setval for discussion.
 *
 * 实现两参数形式的 setval。讨论见 do_setval。
 */
Datum
setval_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);

	do_setval(relid, next, true);

	PG_RETURN_INT64(next);
}

/*
 * Implement the 3 arg setval procedure.
 * See do_setval for discussion.
 *
 * 实现三参数形式的 setval。讨论见 do_setval。
 */
Datum
setval3_oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);
	bool		iscalled = PG_GETARG_BOOL(2);

	do_setval(relid, next, iscalled);

	PG_RETURN_INT64(next);
}


/*
 * Open the sequence and acquire lock if needed
 *
 * 打开序列，并在需要时获取锁
 *
 * If we haven't touched the sequence already in this transaction,
 * we need to acquire a lock.  We arrange for the lock to
 * be owned by the top transaction, so that we don't need to do it
 * more than once per xact.
 *
 * 若本事务中还没有接触过该序列，则需要获取锁。
 * 我们让锁属于顶层事务，这样每个事务不必获取一次以上。
 */
static Relation
lock_and_open_sequence(SeqTable seq)
{
	LocalTransactionId thislxid = MyProc->vxid.lxid;

	/* Get the lock if not already held in this xact */
	/*
	 *
	 * 若本事务中尚未持有锁，则获取锁
	 */
	if (seq->lxid != thislxid)
	{
		ResourceOwner currentOwner;

		currentOwner = CurrentResourceOwner;
		CurrentResourceOwner = TopTransactionResourceOwner;

		LockRelationOid(seq->relid, RowExclusiveLock);

		CurrentResourceOwner = currentOwner;

		/* Flag that we have a lock in the current xact */
		/*
		 *
		 * 标记本事务中已经持有锁
		 */
		seq->lxid = thislxid;
	}

	/* We now know we have the lock, and can safely open the rel */
	/*
	 *
	 * 现在已经持有锁，可以安全地打开关系
	 */
	return sequence_open(seq->relid, NoLock);
}

/*
 * Creates the hash table for storing sequence data
 *
 * 创建用于存放序列数据的哈希表
 */
static void
create_seq_hashtable(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(SeqTableData);

	seqhashtab = hash_create("Sequence values", 16, &ctl,
							 HASH_ELEM | HASH_BLOBS);
}

/*
 * Given a relation OID, open and lock the sequence.  p_elm and p_rel are
 * output parameters.
 *
 * 给定关系 OID，打开并锁定序列。p_elm 与 p_rel 是输出参数。
 */
static void
init_sequence(Oid relid, SeqTable *p_elm, Relation *p_rel)
{
	SeqTable	elm;
	Relation	seqrel;
	bool		found;

	/* Find or create a hash table entry for this sequence */
	/*
	 *
	 * 查找或创建该序列的哈希表项
	 */
	if (seqhashtab == NULL)
		create_seq_hashtable();

	elm = (SeqTable) hash_search(seqhashtab, &relid, HASH_ENTER, &found);

	/*
	 * Initialize the new hash table entry if it did not exist already.
	 *
	 * 若哈希表项尚不存在，则初始化它。
	 *
	 * NOTE: seqhashtab entries are stored for the life of a backend (unless
	 * explicitly discarded with DISCARD). If the sequence itself is deleted
	 * then the entry becomes wasted memory, but it's small enough that this
	 * should not matter.
	 *
	 * 注意：seqhashtab 项在后端的整个生命周期内保存（除非用 DISCARD 显式丢弃）。
	 * 若序列本身被删除，该项就成为浪费的内存，但它足够小，应当不成问题。
	 */
	if (!found)
	{
		/* relid already filled in */
		/*
		 *
		 * relid 已经填好
		 */
		elm->filenumber = InvalidRelFileNumber;
		elm->lxid = InvalidLocalTransactionId;
		elm->last_valid = false;
		elm->last = elm->cached = 0;
	}

	/*
	 * Open the sequence relation.
	 *
	 * 打开序列关系。
	 */
	seqrel = lock_and_open_sequence(elm);

	/*
	 * If the sequence has been transactionally replaced since we last saw it,
	 * discard any cached-but-unissued values.  We do not touch the currval()
	 * state, however.
	 *
	 * 若自上次看到该序列以来它已被事务性地替换，则丢弃已缓存但尚未发出的值。
	 * 不过不触碰 currval() 的状态。
	 */
	if (seqrel->rd_rel->relfilenode != elm->filenumber)
	{
		elm->filenumber = seqrel->rd_rel->relfilenode;
		elm->cached = elm->last;
	}

	/* Return results */
	/*
	 *
	 * 返回结果
	 */
	*p_elm = elm;
	*p_rel = seqrel;
}


/*
 * Given an opened sequence relation, lock the page buffer and find the tuple
 *
 * 给定已打开的序列关系，锁定页缓冲区并找到元组
 *
 * *buf receives the reference to the pinned-and-ex-locked buffer
 * *seqdatatuple receives the reference to the sequence tuple proper
 *		(this arg should point to a local variable of type HeapTupleData)
 *
 * buf 接收已 pin 且排他锁定的缓冲区引用。
 * seqdatatuple 接收序列元组本身的引用
 * （该参数应指向 HeapTupleData 类型的局部变量）
 *
 * Function's return value points to the data payload of the tuple
 *
 * 函数返回值指向元组的数据载荷
 */
static Form_pg_sequence_data
read_seq_tuple(Relation rel, Buffer *buf, HeapTuple seqdatatuple)
{
	Page		page;
	ItemId		lp;
	sequence_magic *sm;
	Form_pg_sequence_data seq;

	*buf = ReadBuffer(rel, 0);
	LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(*buf);
	sm = (sequence_magic *) PageGetSpecialPointer(page);

	if (sm->magic != SEQ_MAGIC)
		elog(ERROR, "bad magic number in sequence \"%s\": %08X",
			 RelationGetRelationName(rel), sm->magic);

	lp = PageGetItemId(page, FirstOffsetNumber);
	Assert(ItemIdIsNormal(lp));

	/* Note we currently only bother to set these two fields of *seqdatatuple */
	/*
	 *
	 * 注意目前只设置 *seqdatatuple 的这两个字段
	 */
	seqdatatuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
	seqdatatuple->t_len = ItemIdGetLength(lp);

	/*
	 * Previous releases of Postgres neglected to prevent SELECT FOR UPDATE on
	 * a sequence, which would leave a non-frozen XID in the sequence tuple's
	 * xmax, which eventually leads to clog access failures or worse. If we
	 * see this has happened, clean up after it.  We treat this like a hint
	 * bit update, ie, don't bother to WAL-log it, since we can certainly do
	 * this again if the update gets lost.
	 *
	 * 早期 Postgres 没有阻止对序列做 SELECT FOR UPDATE，这会在序列元组的 xmax 中留下未冻结的 XID，
	 * 最终导致 clog 访问失败或更糟。若发现这种情况，就清理它。
	 * 我们把它当作 hint bit 更新，即不写 WAL，因为即使更新丢失，以后也一定可以再做一次。
	 */
	Assert(!(seqdatatuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI));
	if (HeapTupleHeaderGetRawXmax(seqdatatuple->t_data) != InvalidTransactionId)
	{
		HeapTupleHeaderSetXmax(seqdatatuple->t_data, InvalidTransactionId);
		seqdatatuple->t_data->t_infomask &= ~HEAP_XMAX_COMMITTED;
		seqdatatuple->t_data->t_infomask |= HEAP_XMAX_INVALID;
		MarkBufferDirtyHint(*buf, true);
	}

	seq = (Form_pg_sequence_data) GETSTRUCT(seqdatatuple);

	return seq;
}

/*
 * init_params: process the options list of CREATE or ALTER SEQUENCE, and
 * store the values into appropriate fields of seqform, for changes that go
 * into the pg_sequence catalog, and fields of seqdataform for changes to the
 * sequence relation itself.  Set *need_seq_rewrite to true if we changed any
 * parameters that require rewriting the sequence's relation (interesting for
 * ALTER SEQUENCE).  Also set *owned_by to any OWNED BY option, or to NIL if
 * there is none.
 *
 * init_params：处理 CREATE 或 ALTER SEQUENCE 的选项列表。
 * 将写入 pg_sequence 目录的变更存入 seqform 的相应字段，
 * 将写入序列关系本身的变更存入 seqdataform。
 * 若修改了需要重写序列关系的参数，则把 need_seq_rewrite 设为 true（对 ALTER SEQUENCE 有意义）。
 * 若有 OWNED BY 选项，也把它写入 owned_by；没有则为 NIL。
 *
 * If isInit is true, fill any unspecified options with default values;
 * otherwise, do not change existing options that aren't explicitly overridden.
 *
 * 若 isInit 为 true，用默认值填充未指定的选项；否则不改变未被显式覆盖的现有选项。
 *
 * Note: we force a sequence rewrite whenever we change parameters that affect
 * generation of future sequence values, even if the seqdataform per se is not
 * changed.  This allows ALTER SEQUENCE to behave transactionally.  Currently,
 * the only option that doesn't cause that is OWNED BY.  It's *necessary* for
 * ALTER SEQUENCE OWNED BY to not rewrite the sequence, because that would
 * break pg_upgrade by causing unwanted changes in the sequence's
 * relfilenumber.
 *
 * 注意：只要修改了影响未来序列值生成的参数，就强制重写序列，即使 seqdataform 本身没有变化。
 * 这使 ALTER SEQUENCE 具有事务性。目前唯一不导致重写的选项是 OWNED BY。
 * ALTER SEQUENCE OWNED BY 必须不重写序列，否则会改变序列的 relfilenumber，从而破坏 pg_upgrade。
 */
static void
init_params(ParseState *pstate, List *options, bool for_identity,
			bool isInit,
			Form_pg_sequence seqform,
			Form_pg_sequence_data seqdataform,
			bool *need_seq_rewrite,
			List **owned_by)
{
	DefElem    *as_type = NULL;
	DefElem    *start_value = NULL;
	DefElem    *restart_value = NULL;
	DefElem    *increment_by = NULL;
	DefElem    *max_value = NULL;
	DefElem    *min_value = NULL;
	DefElem    *cache_value = NULL;
	DefElem    *is_cycled = NULL;
	ListCell   *option;
	bool		reset_max_value = false;
	bool		reset_min_value = false;

	*need_seq_rewrite = false;
	*owned_by = NIL;

	foreach(option, options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "as") == 0)
		{
			if (as_type)
				errorConflictingDefElem(defel, pstate);
			as_type = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "increment") == 0)
		{
			if (increment_by)
				errorConflictingDefElem(defel, pstate);
			increment_by = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "start") == 0)
		{
			if (start_value)
				errorConflictingDefElem(defel, pstate);
			start_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "restart") == 0)
		{
			if (restart_value)
				errorConflictingDefElem(defel, pstate);
			restart_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "maxvalue") == 0)
		{
			if (max_value)
				errorConflictingDefElem(defel, pstate);
			max_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "minvalue") == 0)
		{
			if (min_value)
				errorConflictingDefElem(defel, pstate);
			min_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "cache") == 0)
		{
			if (cache_value)
				errorConflictingDefElem(defel, pstate);
			cache_value = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "cycle") == 0)
		{
			if (is_cycled)
				errorConflictingDefElem(defel, pstate);
			is_cycled = defel;
			*need_seq_rewrite = true;
		}
		else if (strcmp(defel->defname, "owned_by") == 0)
		{
			if (*owned_by)
				errorConflictingDefElem(defel, pstate);
			*owned_by = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "sequence_name") == 0)
		{
			/*
			 * The parser allows this, but it is only for identity columns, in
			 * which case it is filtered out in parse_utilcmd.c.  We only get
			 * here if someone puts it into a CREATE SEQUENCE, where it'd be
			 * redundant.  (The same is true for the equally-nonstandard
			 * LOGGED and UNLOGGED options, but for those, the default error
			 * below seems sufficient.)
			 *
			 * 解析器允许这个选项，但它只用于标识列，并在 parse_utilcmd.c 中被滤掉。
			 * 只有有人把它写进 CREATE SEQUENCE 时才会到达这里，而那时它是多余的。
			 * 同样非标准的 LOGGED 与 UNLOGGED 选项也是如此，但对它们来说，下面的默认错误就够了。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid sequence option SEQUENCE NAME"),
					 parser_errposition(pstate, defel->location)));
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	/*
	 * We must reset log_cnt when isInit or when changing any parameters that
	 * would affect future nextval allocations.
	 *
	 * 在 isInit 时，或在修改任何会影响后续 nextval 分配的参数时，必须重置 log_cnt。
	 */
	if (isInit)
		seqdataform->log_cnt = 0;

	/* AS type */
	/*
	 *
	 * 序列类型 AS type
	 */
	if (as_type != NULL)
	{
		Oid			newtypid = typenameTypeId(pstate, defGetTypeName(as_type));

		if (newtypid != INT2OID &&
			newtypid != INT4OID &&
			newtypid != INT8OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 for_identity
					 ? errmsg("identity column type must be smallint, integer, or bigint")
					 : errmsg("sequence type must be smallint, integer, or bigint")));

		if (!isInit)
		{
			/*
			 * When changing type and the old sequence min/max values were the
			 * min/max of the old type, adjust sequence min/max values to
			 * min/max of new type.  (Otherwise, the user chose explicit
			 * min/max values, which we'll leave alone.)
			 *
			 * 更改类型时，若旧序列的最小/最大值就是旧类型的最小/最大值，
			 * 则把序列的最小/最大值调整为新类型的最小/最大值。
			 * 否则说明用户显式选择了最小/最大值，保持不动。
			 */
			if ((seqform->seqtypid == INT2OID && seqform->seqmax == PG_INT16_MAX) ||
				(seqform->seqtypid == INT4OID && seqform->seqmax == PG_INT32_MAX) ||
				(seqform->seqtypid == INT8OID && seqform->seqmax == PG_INT64_MAX))
				reset_max_value = true;
			if ((seqform->seqtypid == INT2OID && seqform->seqmin == PG_INT16_MIN) ||
				(seqform->seqtypid == INT4OID && seqform->seqmin == PG_INT32_MIN) ||
				(seqform->seqtypid == INT8OID && seqform->seqmin == PG_INT64_MIN))
				reset_min_value = true;
		}

		seqform->seqtypid = newtypid;
	}
	else if (isInit)
	{
		seqform->seqtypid = INT8OID;
	}

	/* INCREMENT BY */
	/*
	 *
	 * 步长 INCREMENT BY
	 */
	if (increment_by != NULL)
	{
		seqform->seqincrement = defGetInt64(increment_by);
		if (seqform->seqincrement == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("INCREMENT must not be zero")));
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqform->seqincrement = 1;
	}

	/* CYCLE */
	/*
	 *
	 * 循环选项 CYCLE
	 */
	if (is_cycled != NULL)
	{
		seqform->seqcycle = boolVal(is_cycled->arg);
		Assert(BoolIsValid(seqform->seqcycle));
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqform->seqcycle = false;
	}

	/* MAXVALUE (null arg means NO MAXVALUE) */
	/*
	 *
	 * MAXVALUE（空参数表示 NO MAXVALUE）
	 */
	if (max_value != NULL && max_value->arg)
	{
		seqform->seqmax = defGetInt64(max_value);
		seqdataform->log_cnt = 0;
	}
	else if (isInit || max_value != NULL || reset_max_value)
	{
		if (seqform->seqincrement > 0 || reset_max_value)
		{
			/* ascending seq */
			/*
			 *
			 * 递增序列
			 */
			if (seqform->seqtypid == INT2OID)
				seqform->seqmax = PG_INT16_MAX;
			else if (seqform->seqtypid == INT4OID)
				seqform->seqmax = PG_INT32_MAX;
			else
				seqform->seqmax = PG_INT64_MAX;
		}
		else
			seqform->seqmax = -1;	/* descending seq */
			/*
			 *
			 * 递减序列
			 */
		seqdataform->log_cnt = 0;
	}

	/* Validate maximum value.  No need to check INT8 as seqmax is an int64 */
	/*
	 *
	 * 校验最大值。seqmax 已是 int64，不必再检查 INT8
	 */
	if ((seqform->seqtypid == INT2OID && (seqform->seqmax < PG_INT16_MIN || seqform->seqmax > PG_INT16_MAX))
		|| (seqform->seqtypid == INT4OID && (seqform->seqmax < PG_INT32_MIN || seqform->seqmax > PG_INT32_MAX)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MAXVALUE (%" PRId64 ") is out of range for sequence data type %s",
						seqform->seqmax,
						format_type_be(seqform->seqtypid))));

	/* MINVALUE (null arg means NO MINVALUE) */
	/*
	 *
	 * MINVALUE（空参数表示 NO MINVALUE）
	 */
	if (min_value != NULL && min_value->arg)
	{
		seqform->seqmin = defGetInt64(min_value);
		seqdataform->log_cnt = 0;
	}
	else if (isInit || min_value != NULL || reset_min_value)
	{
		if (seqform->seqincrement < 0 || reset_min_value)
		{
			/* descending seq */
			/*
			 *
			 * 递减序列
			 */
			if (seqform->seqtypid == INT2OID)
				seqform->seqmin = PG_INT16_MIN;
			else if (seqform->seqtypid == INT4OID)
				seqform->seqmin = PG_INT32_MIN;
			else
				seqform->seqmin = PG_INT64_MIN;
		}
		else
			seqform->seqmin = 1;	/* ascending seq */
			/*
			 *
			 * 递增序列
			 */
		seqdataform->log_cnt = 0;
	}

	/* Validate minimum value.  No need to check INT8 as seqmin is an int64 */
	/*
	 *
	 * 校验最小值。seqmin 已是 int64，不必再检查 INT8
	 */
	if ((seqform->seqtypid == INT2OID && (seqform->seqmin < PG_INT16_MIN || seqform->seqmin > PG_INT16_MAX))
		|| (seqform->seqtypid == INT4OID && (seqform->seqmin < PG_INT32_MIN || seqform->seqmin > PG_INT32_MAX)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MINVALUE (%" PRId64 ") is out of range for sequence data type %s",
						seqform->seqmin,
						format_type_be(seqform->seqtypid))));

	/* crosscheck min/max */
	/*
	 *
	 * 交叉检查最小值与最大值
	 */
	if (seqform->seqmin >= seqform->seqmax)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("MINVALUE (%" PRId64 ") must be less than MAXVALUE (%" PRId64 ")",
						seqform->seqmin,
						seqform->seqmax)));

	/* START WITH */
	/*
	 *
	 * 起始值 START WITH
	 */
	if (start_value != NULL)
	{
		seqform->seqstart = defGetInt64(start_value);
	}
	else if (isInit)
	{
		if (seqform->seqincrement > 0)
			seqform->seqstart = seqform->seqmin;	/* ascending seq */
			/*
			 *
			 * 递增序列
			 */
		else
			seqform->seqstart = seqform->seqmax;	/* descending seq */
			/*
			 *
			 * 递减序列
			 */
	}

	/* crosscheck START */
	/*
	 *
	 * 交叉检查 START
	 */
	if (seqform->seqstart < seqform->seqmin)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("START value (%" PRId64 ") cannot be less than MINVALUE (%" PRId64 ")",
						seqform->seqstart,
						seqform->seqmin)));
	if (seqform->seqstart > seqform->seqmax)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("START value (%" PRId64 ") cannot be greater than MAXVALUE (%" PRId64 ")",
						seqform->seqstart,
						seqform->seqmax)));

	/* RESTART [WITH] */
	/*
	 *
	 * 重启选项 RESTART [WITH]
	 */
	if (restart_value != NULL)
	{
		if (restart_value->arg != NULL)
			seqdataform->last_value = defGetInt64(restart_value);
		else
			seqdataform->last_value = seqform->seqstart;
		seqdataform->is_called = false;
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqdataform->last_value = seqform->seqstart;
		seqdataform->is_called = false;
	}

	/* crosscheck RESTART (or current value, if changing MIN/MAX) */
	/*
	 *
	 * 交叉检查 RESTART（若正在修改 MIN/MAX，则检查当前值）
	 */
	if (seqdataform->last_value < seqform->seqmin)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("RESTART value (%" PRId64 ") cannot be less than MINVALUE (%" PRId64 ")",
						seqdataform->last_value,
						seqform->seqmin)));
	if (seqdataform->last_value > seqform->seqmax)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("RESTART value (%" PRId64 ") cannot be greater than MAXVALUE (%" PRId64 ")",
						seqdataform->last_value,
						seqform->seqmax)));

	/* CACHE */
	/*
	 *
	 * 缓存个数 CACHE
	 */
	if (cache_value != NULL)
	{
		seqform->seqcache = defGetInt64(cache_value);
		if (seqform->seqcache <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("CACHE (%" PRId64 ") must be greater than zero",
							seqform->seqcache)));
		seqdataform->log_cnt = 0;
	}
	else if (isInit)
	{
		seqform->seqcache = 1;
	}
}

/*
 * Process an OWNED BY option for CREATE/ALTER SEQUENCE
 *
 * 处理 CREATE/ALTER SEQUENCE 的 OWNED BY 选项
 *
 * Ownership permissions on the sequence are already checked,
 * but if we are establishing a new owned-by dependency, we must
 * enforce that the referenced table has the same owner and namespace
 * as the sequence.
 *
 * 序列本身的属主权限已经检查过；
 * 但若要建立新的 owned-by 依赖，必须强制被引用表与序列具有相同的属主和名字空间。
 */
static void
process_owned_by(Relation seqrel, List *owned_by, bool for_identity)
{
	DependencyType deptype;
	int			nnames;
	Relation	tablerel;
	AttrNumber	attnum;

	deptype = for_identity ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO;

	nnames = list_length(owned_by);
	Assert(nnames > 0);
	if (nnames == 1)
	{
		/* Must be OWNED BY NONE */
		/*
		 *
		 * 必须是 OWNED BY NONE
		 */
		if (strcmp(strVal(linitial(owned_by)), "none") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid OWNED BY option"),
					 errhint("Specify OWNED BY table.column or OWNED BY NONE.")));
		tablerel = NULL;
		attnum = 0;
	}
	else
	{
		List	   *relname;
		char	   *attrname;
		RangeVar   *rel;

		/* Separate relname and attr name */
		/*
		 *
		 * 分开关系名和属性名
		 */
		relname = list_copy_head(owned_by, nnames - 1);
		attrname = strVal(llast(owned_by));

		/* Open and lock rel to ensure it won't go away meanwhile */
		/*
		 *
		 * 打开并锁定关系，确保它在此期间不会消失
		 */
		rel = makeRangeVarFromNameList(relname);
		tablerel = relation_openrv(rel, AccessShareLock);

		/* Must be a regular or foreign table */
		/*
		 *
		 * 必须是普通表或外部表
		 */
		if (!(tablerel->rd_rel->relkind == RELKIND_RELATION ||
			  tablerel->rd_rel->relkind == RELKIND_FOREIGN_TABLE ||
			  tablerel->rd_rel->relkind == RELKIND_VIEW ||
			  tablerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("sequence cannot be owned by relation \"%s\"",
							RelationGetRelationName(tablerel)),
					 errdetail_relkind_not_supported(tablerel->rd_rel->relkind)));

		/* We insist on same owner and schema */
		/*
		 *
		 * 要求属主和模式相同
		 */
		if (seqrel->rd_rel->relowner != tablerel->rd_rel->relowner)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("sequence must have same owner as table it is linked to")));
		if (RelationGetNamespace(seqrel) != RelationGetNamespace(tablerel))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("sequence must be in same schema as table it is linked to")));

		/* Now, fetch the attribute number from the system cache */
		/*
		 *
		 * 现在从系统缓存中取出属性号
		 */
		attnum = get_attnum(RelationGetRelid(tablerel), attrname);
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							attrname, RelationGetRelationName(tablerel))));
	}

	/*
	 * Catch user explicitly running OWNED BY on identity sequence.
	 *
	 * 捕获用户对 identity 序列显式执行 OWNED BY 的情况。
	 */
	if (deptype == DEPENDENCY_AUTO)
	{
		Oid			tableId;
		int32		colId;

		if (sequenceIsOwned(RelationGetRelid(seqrel), DEPENDENCY_INTERNAL, &tableId, &colId))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot change ownership of identity sequence"),
					 errdetail("Sequence \"%s\" is linked to table \"%s\".",
							   RelationGetRelationName(seqrel),
							   get_rel_name(tableId))));
	}

	/*
	 * OK, we are ready to update pg_depend.  First remove any existing
	 * dependencies for the sequence, then optionally add a new one.
	 *
	 * 可以更新 pg_depend 了。先删除该序列的全部现有依赖，然后按需添加一条新依赖。
	 */
	deleteDependencyRecordsForClass(RelationRelationId, RelationGetRelid(seqrel),
									RelationRelationId, deptype);

	if (tablerel)
	{
		ObjectAddress refobject,
					depobject;

		refobject.classId = RelationRelationId;
		refobject.objectId = RelationGetRelid(tablerel);
		refobject.objectSubId = attnum;
		depobject.classId = RelationRelationId;
		depobject.objectId = RelationGetRelid(seqrel);
		depobject.objectSubId = 0;
		recordDependencyOn(&depobject, &refobject, deptype);
	}

	/* Done, but hold lock until commit */
	/*
	 *
	 * 完成，但持锁直到提交
	 */
	if (tablerel)
		relation_close(tablerel, NoLock);
}


/*
 * Return sequence parameters in a list of the form created by the parser.
 *
 * 以解析器所创建的列表形式返回序列参数。
 */
List *
sequence_options(Oid relid)
{
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;
	List	   *options = NIL;

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);

	/* Use makeFloat() for 64-bit integers, like gram.y does. */
	/*
	 *
	 * 对 64 位整数使用 makeFloat()，与 gram.y 的做法相同。
	 */
	options = lappend(options,
					  makeDefElem("cache", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqcache)), -1));
	options = lappend(options,
					  makeDefElem("cycle", (Node *) makeBoolean(pgsform->seqcycle), -1));
	options = lappend(options,
					  makeDefElem("increment", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqincrement)), -1));
	options = lappend(options,
					  makeDefElem("maxvalue", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqmax)), -1));
	options = lappend(options,
					  makeDefElem("minvalue", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqmin)), -1));
	options = lappend(options,
					  makeDefElem("start", (Node *) makeFloat(psprintf(INT64_FORMAT, pgsform->seqstart)), -1));

	ReleaseSysCache(pgstuple);

	return options;
}

/*
 * Return sequence parameters (formerly for use by information schema)
 *
 * 返回序列参数（原先供 information schema 使用）
 */
Datum
pg_sequence_parameters(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		isnull[7];
	HeapTuple	pgstuple;
	Form_pg_sequence pgsform;

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT | ACL_UPDATE | ACL_USAGE) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for sequence %s",
						get_rel_name(relid))));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(isnull, 0, sizeof(isnull));

	pgstuple = SearchSysCache1(SEQRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(pgstuple))
		elog(ERROR, "cache lookup failed for sequence %u", relid);
	pgsform = (Form_pg_sequence) GETSTRUCT(pgstuple);

	values[0] = Int64GetDatum(pgsform->seqstart);
	values[1] = Int64GetDatum(pgsform->seqmin);
	values[2] = Int64GetDatum(pgsform->seqmax);
	values[3] = Int64GetDatum(pgsform->seqincrement);
	values[4] = BoolGetDatum(pgsform->seqcycle);
	values[5] = Int64GetDatum(pgsform->seqcache);
	values[6] = ObjectIdGetDatum(pgsform->seqtypid);

	ReleaseSysCache(pgstuple);

	return HeapTupleGetDatum(heap_form_tuple(tupdesc, values, isnull));
}


/*
 * Return the sequence tuple.
 *
 * 返回序列元组。
 *
 * This is primarily intended for use by pg_dump to gather sequence data
 * without needing to individually query each sequence relation.
 *
 * 这主要供 pg_dump 使用，以便收集序列数据，而不必逐个查询每个序列关系。
 */
Datum
pg_get_sequence_data(PG_FUNCTION_ARGS)
{
#define PG_GET_SEQUENCE_DATA_COLS	2
	Oid			relid = PG_GETARG_OID(0);
	Relation	seqrel;
	Datum		values[PG_GET_SEQUENCE_DATA_COLS] = {0};
	bool		isnull[PG_GET_SEQUENCE_DATA_COLS] = {0};
	TupleDesc	resultTupleDesc;
	HeapTuple	resultHeapTuple;
	Datum		result;

	resultTupleDesc = CreateTemplateTupleDesc(PG_GET_SEQUENCE_DATA_COLS);
	TupleDescInitEntry(resultTupleDesc, (AttrNumber) 1, "last_value",
					   INT8OID, -1, 0);
	TupleDescInitEntry(resultTupleDesc, (AttrNumber) 2, "is_called",
					   BOOLOID, -1, 0);
	resultTupleDesc = BlessTupleDesc(resultTupleDesc);

	seqrel = try_relation_open(relid, AccessShareLock);

	/*
	 * Return all NULLs for missing sequences, sequences for which we lack
	 * privileges, other sessions' temporary sequences, and unlogged sequences
	 * on standbys.
	 *
	 * 对不存在的序列、无权访问的序列、其他会话的临时序列，以及备库上的 unlogged 序列，全部返回 NULL。
	 */
	if (seqrel && seqrel->rd_rel->relkind == RELKIND_SEQUENCE &&
		pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) == ACLCHECK_OK &&
		!RELATION_IS_OTHER_TEMP(seqrel) &&
		(RelationIsPermanent(seqrel) || !RecoveryInProgress()))
	{
		Buffer		buf;
		HeapTupleData seqtuple;
		Form_pg_sequence_data seq;

		seq = read_seq_tuple(seqrel, &buf, &seqtuple);

		values[0] = Int64GetDatum(seq->last_value);
		values[1] = BoolGetDatum(seq->is_called);

		UnlockReleaseBuffer(buf);
	}
	else
		memset(isnull, true, sizeof(isnull));

	if (seqrel)
		relation_close(seqrel, AccessShareLock);

	resultHeapTuple = heap_form_tuple(resultTupleDesc, values, isnull);
	result = HeapTupleGetDatum(resultHeapTuple);
	PG_RETURN_DATUM(result);
#undef PG_GET_SEQUENCE_DATA_COLS
}


/*
 * Return the last value from the sequence
 *
 * 返回序列的最后一个值
 *
 * Note: This has a completely different meaning than lastval().
 *
 * 注意：这与 lastval() 的含义完全不同。
 */
Datum
pg_sequence_last_value(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	SeqTable	elm;
	Relation	seqrel;
	bool		is_called = false;
	int64		result = 0;

	/* open and lock sequence */
	/*
	 *
	 * 打开并锁定序列
	 */
	init_sequence(relid, &elm, &seqrel);

	/*
	 * We return NULL for other sessions' temporary sequences.  The
	 * pg_sequences system view already filters those out, but this offers a
	 * defense against ERRORs in case someone invokes this function directly.
	 *
	 * 对其他会话的临时序列返回 NULL。
	 * pg_sequences 系统视图已经滤掉它们，但若有人直接调用本函数，这可以避免报错。
	 *
	 * Also, for the benefit of the pg_sequences view, we return NULL for
	 * unlogged sequences on standbys and for sequences for which the current
	 * user lacks privileges instead of throwing an error.
	 *
	 * 另外，为了方便 pg_sequences 视图，对备库上的 unlogged 序列、以及当前用户无权访问的序列返回 NULL，而不是抛错。
	 */
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT | ACL_USAGE) == ACLCHECK_OK &&
		!RELATION_IS_OTHER_TEMP(seqrel) &&
		(RelationIsPermanent(seqrel) || !RecoveryInProgress()))
	{
		Buffer		buf;
		HeapTupleData seqtuple;
		Form_pg_sequence_data seq;

		seq = read_seq_tuple(seqrel, &buf, &seqtuple);

		is_called = seq->is_called;
		result = seq->last_value;

		UnlockReleaseBuffer(buf);
	}
	sequence_close(seqrel, NoLock);

	if (is_called)
		PG_RETURN_INT64(result);
	else
		PG_RETURN_NULL();
}


/*
 * 序列 WAL redo：重放 XLOG_SEQ_LOG，重建序列页内容。
 */
void
seq_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	Buffer		buffer;
	Page		page;
	Page		localpage;
	char	   *item;
	Size		itemsz;
	xl_seq_rec *xlrec = (xl_seq_rec *) XLogRecGetData(record);
	sequence_magic *sm;

	if (info != XLOG_SEQ_LOG)
		elog(PANIC, "seq_redo: unknown op code %u", info);

	buffer = XLogInitBufferForRedo(record, 0);
	page = (Page) BufferGetPage(buffer);

	/*
	 * We always reinit the page.  However, since this WAL record type is also
	 * used for updating sequences, it's possible that a hot-standby backend
	 * is examining the page concurrently; so we mustn't transiently trash the
	 * buffer.  The solution is to build the correct new page contents in
	 * local workspace and then memcpy into the buffer.  Then only bytes that
	 * are supposed to change will change, even transiently. We must palloc
	 * the local page for alignment reasons.
	 *
	 * 我们总是重新初始化该页。但这类 WAL 记录也用于更新序列，
	 * 热备后端可能正在同时查看该页，因此不能暂时把缓冲区写乱。
	 * 做法是在本地工作区构造正确的新页内容，再 memcpy 到缓冲区。
	 * 这样即使在瞬间，也只有应当改变的字节会改变。出于对齐，本地页必须用 palloc 分配。
	 */
	localpage = (Page) palloc(BufferGetPageSize(buffer));

	PageInit(localpage, BufferGetPageSize(buffer), sizeof(sequence_magic));
	sm = (sequence_magic *) PageGetSpecialPointer(localpage);
	sm->magic = SEQ_MAGIC;

	item = (char *) xlrec + sizeof(xl_seq_rec);
	itemsz = XLogRecGetDataLen(record) - sizeof(xl_seq_rec);

	if (PageAddItem(localpage, (Item) item, itemsz,
					FirstOffsetNumber, false, false) == InvalidOffsetNumber)
		elog(PANIC, "seq_redo: failed to add item to page");

	PageSetLSN(localpage, lsn);

	memcpy(page, localpage, BufferGetPageSize(buffer));
	MarkBufferDirty(buffer);
	UnlockReleaseBuffer(buffer);

	pfree(localpage);
}

/*
 * Flush cached sequence information.
 *
 * 刷新缓存的序列信息。
 */
void
ResetSequenceCaches(void)
{
	if (seqhashtab)
	{
		hash_destroy(seqhashtab);
		seqhashtab = NULL;
	}

	last_used_seq = NULL;
}

/*
 * Mask a Sequence page before performing consistency checks on it.
 *
 * 在对序列页做一致性检查之前掩盖该页。
 */
void
seq_mask(char *page, BlockNumber blkno)
{
	mask_page_lsn_and_checksum(page);

	mask_unused_space(page);
}
