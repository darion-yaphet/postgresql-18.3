/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 * 数据库管理命令（CREATE/DROP DATABASE）。
 *
 * Note: database creation/destruction commands use exclusive locks on
 * the database objects (as expressed by LockSharedObject()) to avoid
 * stepping on each others' toes.  Formerly we used table-level locks
 * on pg_database, but that's too coarse-grained.
 *
 * 注意：创建与删除数据库的命令通过 LockSharedObject() 对数据库对象加排他锁，避免互相干扰。以前用的是 pg_database 的表级锁，粒度太粗。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dbcommands.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/dbcommands_xlog.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "replication/slot.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_locale.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 核心流程概览：
 * createdb：解析选项，校验权限、编码、locale 与表空间，分配 OID 并写入 pg_database，
 * 再按 CREATEDB_WAL_LOG 或 CREATEDB_FILE_COPY 从模板库复制文件。
 * CreateDatabaseUsingWalLog：按块复制关系文件并逐块写 WAL。
 * CreateDatabaseUsingFileCopy：在文件系统层复制各表空间，复制前后做检查点。
 * dropdb：对目标库加排他锁，检查复制槽与订阅，原地标记无效后删除缓冲与目录。
 * RenameDatabase、movedb、AlterDatabase 及其变体：重命名、迁移默认表空间或修改 pg_database。
 * dbase_redo：重放创建与删除数据库的 WAL 记录。
 */

/*
 * Create database strategy.
 *
 * 创建数据库的策略。
 *
 * CREATEDB_WAL_LOG will copy the database at the block level and WAL log each
 * copied block.
 *
 * CREATEDB_WAL_LOG 按块复制数据库，并把每个被复制的块写入 WAL。
 *
 * CREATEDB_FILE_COPY will simply perform a file system level copy of the
 * database and log a single record for each tablespace copied. To make this
 * safe, it also triggers checkpoints before and after the operation.
 *
 * CREATEDB_FILE_COPY 在文件系统层复制数据库，并为每个被复制的表空间记录一条 WAL。
 * 为保证安全，复制前后都会触发检查点。
 */
typedef enum CreateDBStrategy
{
	CREATEDB_WAL_LOG,
	CREATEDB_FILE_COPY,
} CreateDBStrategy;

typedef struct
{
	Oid			src_dboid;		/* source (template) DB */
	/*
	 *
	 * 源（模板）数据库
	 */
	Oid			dest_dboid;		/* DB we are trying to create */
	/*
	 *
	 * 正在创建的数据库
	 */
	CreateDBStrategy strategy;	/* create db strategy */
	/*
	 *
	 * 创建数据库的策略
	 */
} createdb_failure_params;

typedef struct
{
	Oid			dest_dboid;		/* DB we are trying to move */
	/*
	 *
	 * 正在迁移的数据库
	 */
	Oid			dest_tsoid;		/* tablespace we are trying to move to */
	/*
	 *
	 * 要迁入的表空间
	 */
} movedb_failure_params;

/*
 * Information about a relation to be copied when creating a database.
 *
 * 创建数据库时需要复制的关系信息。
 */
typedef struct CreateDBRelInfo
{
	RelFileLocator rlocator;	/* physical relation identifier */
	/*
	 *
	 * 物理关系标识
	 */
	Oid			reloid;			/* relation oid */
	/*
	 *
	 * 关系 OID
	 */
	bool		permanent;		/* relation is permanent or unlogged */
	/*
	 *
	 * 关系是永久的还是 unlogged
	 */
} CreateDBRelInfo;


/* non-export function prototypes */
/*
 *
 * 本文件内部静态函数声明
 */
static void createdb_failure_callback(int code, Datum arg);
static void movedb(const char *dbname, const char *tblspcname);
static void movedb_failure_callback(int code, Datum arg);
static bool get_db_info(const char *name, LOCKMODE lockmode,
						Oid *dbIdP, Oid *ownerIdP,
						int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP, bool *dbHasLoginEvtP,
						TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
						Oid *dbTablespace, char **dbCollate, char **dbCtype, char **dbLocale,
						char **dbIcurules,
						char *dbLocProvider,
						char **dbCollversion);
static void remove_dbtablespaces(Oid db_id);
static bool check_db_file_conflict(Oid db_id);
static int	errdetail_busy_db(int notherbackends, int npreparedxacts);
static void CreateDatabaseUsingWalLog(Oid src_dboid, Oid dst_dboid, Oid src_tsid,
									  Oid dst_tsid);
static List *ScanSourceDatabasePgClass(Oid tbid, Oid dbid, char *srcpath);
static List *ScanSourceDatabasePgClassPage(Page page, Buffer buf, Oid tbid,
										   Oid dbid, char *srcpath,
										   List *rlocatorlist, Snapshot snapshot);
static CreateDBRelInfo *ScanSourceDatabasePgClassTuple(HeapTupleData *tuple,
													   Oid tbid, Oid dbid,
													   char *srcpath);
static void CreateDirAndVersionFile(char *dbpath, Oid dbid, Oid tsid,
									bool isRedo);
static void CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid,
										Oid src_tsid, Oid dst_tsid);
static void recovery_create_dbdir(char *path, bool only_tblspc);

/*
 * Create a new database using the WAL_LOG strategy.
 *
 * 使用 WAL_LOG 策略创建新数据库。
 *
 * Each copied block is separately written to the write-ahead log.
 *
 * 每个被复制的块单独写入预写日志。
 */
static void
CreateDatabaseUsingWalLog(Oid src_dboid, Oid dst_dboid,
						  Oid src_tsid, Oid dst_tsid)
{
	char	   *srcpath;
	char	   *dstpath;
	List	   *rlocatorlist = NULL;
	ListCell   *cell;
	LockRelId	srcrelid;
	LockRelId	dstrelid;
	RelFileLocator srcrlocator;
	RelFileLocator dstrlocator;
	CreateDBRelInfo *relinfo;

	/* Get source and destination database paths. */
	/*
	 *
	 * 取得源数据库与目标数据库的路径。
	 */
	srcpath = GetDatabasePath(src_dboid, src_tsid);
	dstpath = GetDatabasePath(dst_dboid, dst_tsid);

	/* Create database directory and write PG_VERSION file. */
	/*
	 *
	 * 创建数据库目录并写入 PG_VERSION 文件。
	 */
	CreateDirAndVersionFile(dstpath, dst_dboid, dst_tsid, false);

	/* Copy relmap file from source database to the destination database. */
	/*
	 *
	 * 把 relmap 文件从源数据库复制到目标数据库。
	 */
	RelationMapCopy(dst_dboid, dst_tsid, srcpath, dstpath);

	/* Get list of relfilelocators to copy from the source database. */
	/*
	 *
	 * 取得要从源数据库复制的 relfilelocator 列表。
	 */
	rlocatorlist = ScanSourceDatabasePgClass(src_tsid, src_dboid, srcpath);
	Assert(rlocatorlist != NIL);

	/*
	 * Database IDs will be the same for all relations so set them before
	 * entering the loop.
	 *
	 * 所有关系的数据库 ID 都相同，因此在进入循环前设置它们。
	 */
	srcrelid.dbId = src_dboid;
	dstrelid.dbId = dst_dboid;

	/* Loop over our list of relfilelocators and copy each one. */
	/*
	 *
	 * 遍历 relfilelocator 列表并逐个复制。
	 */
	foreach(cell, rlocatorlist)
	{
		relinfo = lfirst(cell);
		srcrlocator = relinfo->rlocator;

		/*
		 * If the relation is from the source db's default tablespace then we
		 * need to create it in the destination db's default tablespace.
		 * Otherwise, we need to create in the same tablespace as it is in the
		 * source database.
		 *
		 * 若关系位于源库的默认表空间，则在目标库的默认表空间中创建；
		 * 否则在与源库相同的表空间中创建。
		 */
		if (srcrlocator.spcOid == src_tsid)
			dstrlocator.spcOid = dst_tsid;
		else
			dstrlocator.spcOid = srcrlocator.spcOid;

		dstrlocator.dbOid = dst_dboid;
		dstrlocator.relNumber = srcrlocator.relNumber;

		/*
		 * Acquire locks on source and target relations before copying.
		 *
		 * 复制前对源关系与目标关系加锁。
		 *
		 * We typically do not read relation data into shared_buffers without
		 * holding a relation lock. It's unclear what could go wrong if we
		 * skipped it in this case, because nobody can be modifying either the
		 * source or destination database at this point, and we have locks on
		 * both databases, too, but let's take the conservative route.
		 *
		 * 通常在未持有关系锁时，不会把关系数据读入 shared_buffers。
		 * 此处即便跳过，也不清楚会出什么问题，因为此刻没人能修改源库或目标库，
		 * 而且两个数据库也都已加锁，但仍采取保守做法。
		 */
		dstrelid.relId = srcrelid.relId = relinfo->reloid;
		LockRelationId(&srcrelid, AccessShareLock);
		LockRelationId(&dstrelid, AccessShareLock);

		/* Copy relation storage from source to the destination. */
		/*
		 *
		 * 把关系存储从源复制到目标。
		 */
		CreateAndCopyRelationData(srcrlocator, dstrlocator, relinfo->permanent);

		/* Release the relation locks. */
		/*
		 *
		 * 释放关系锁。
		 */
		UnlockRelationId(&srcrelid, AccessShareLock);
		UnlockRelationId(&dstrelid, AccessShareLock);
	}

	pfree(srcpath);
	pfree(dstpath);
	list_free_deep(rlocatorlist);
}

/*
 * Scan the pg_class table in the source database to identify the relations
 * that need to be copied to the destination database.
 *
 * 扫描源数据库的 pg_class，找出需要复制到目标数据库的关系。
 *
 * This is an exception to the usual rule that cross-database access is
 * not possible. We can make it work here because we know that there are no
 * connections to the source database and (since there can't be prepared
 * transactions touching that database) no in-doubt tuples either. This
 * means that we don't need to worry about pruning removing anything from
 * under us, and we don't need to be too picky about our snapshot either.
 * As long as it sees all previously-committed XIDs as committed and all
 * aborted XIDs as aborted, we should be fine: nothing else is possible
 * here.
 *
 * 这是通常不能跨库访问这一规则的例外。这里能这样做，是因为已知源库没有连接，
 * 也不会有触及该库的预备事务，因此不存在悬疑元组。不用担心剪枝把数据从脚下删掉，
 * 对快照也不必过于挑剔。只要它把此前已提交的 XID 都视为已提交、已中止的 XID 都视为已中止，
 * 就没有问题：这里不可能出现别的状态。
 *
 * We can't rely on the relcache for anything here, because that only knows
 * about the database to which we are connected, and can't handle access to
 * other databases. That also means we can't rely on the heap scan
 * infrastructure, which would be a bad idea anyway since it might try
 * to do things like HOT pruning which we definitely can't do safely in
 * a database to which we're not even connected.
 *
 * 这里不能依赖 relcache，因为它只了解当前连接的数据库，无法访问其他数据库。
 * 因此也不能依赖堆扫描基础设施；那样做本来也不合适，因为它可能尝试 HOT 剪枝，
 * 而在我们甚至没有连接的数据库里绝不能安全地进行。
 */
static List *
ScanSourceDatabasePgClass(Oid tbid, Oid dbid, char *srcpath)
{
	RelFileLocator rlocator;
	BlockNumber nblocks;
	BlockNumber blkno;
	Buffer		buf;
	RelFileNumber relfilenumber;
	Page		page;
	List	   *rlocatorlist = NIL;
	LockRelId	relid;
	Snapshot	snapshot;
	SMgrRelation smgr;
	BufferAccessStrategy bstrategy;

	/* Get pg_class relfilenumber. */
	/*
	 *
	 * 取得 pg_class 的 relfilenumber。
	 */
	relfilenumber = RelationMapOidToFilenumberForDatabase(srcpath,
														  RelationRelationId);

	/* Don't read data into shared_buffers without holding a relation lock. */
	/*
	 *
	 * 未持有关系锁时不要把数据读入 shared_buffers。
	 */
	relid.dbId = dbid;
	relid.relId = RelationRelationId;
	LockRelationId(&relid, AccessShareLock);

	/* Prepare a RelFileLocator for the pg_class relation. */
	/*
	 *
	 * 为 pg_class 关系准备 RelFileLocator。
	 */
	rlocator.spcOid = tbid;
	rlocator.dbOid = dbid;
	rlocator.relNumber = relfilenumber;

	smgr = smgropen(rlocator, INVALID_PROC_NUMBER);
	nblocks = smgrnblocks(smgr, MAIN_FORKNUM);
	smgrclose(smgr);

	/* Use a buffer access strategy since this is a bulk read operation. */
	/*
	 *
	 * 这是批量读，使用缓冲区访问策略。
	 */
	bstrategy = GetAccessStrategy(BAS_BULKREAD);

	/*
	 * As explained in the function header comments, we need a snapshot that
	 * will see all committed transactions as committed, and our transaction
	 * snapshot - or the active snapshot - might not be new enough for that,
	 * but the return value of GetLatestSnapshot() should work fine.
	 *
	 * 如函数头注释所述，需要一个把所有已提交事务都视为已提交的快照。
	 * 事务快照或当前活跃快照可能不够新，但 GetLatestSnapshot() 的返回值应当可用。
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());

	/* Process the relation block by block. */
	/*
	 *
	 * 逐块处理该关系。
	 */
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferWithoutRelcache(rlocator, MAIN_FORKNUM, blkno,
										RBM_NORMAL, bstrategy, true);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		/* Append relevant pg_class tuples for current page to rlocatorlist. */
		/*
		 *
		 * 把当前页中相关的 pg_class 元组追加到 rlocatorlist。
		 */
		rlocatorlist = ScanSourceDatabasePgClassPage(page, buf, tbid, dbid,
													 srcpath, rlocatorlist,
													 snapshot);

		UnlockReleaseBuffer(buf);
	}
	UnregisterSnapshot(snapshot);

	/* Release relation lock. */
	/*
	 *
	 * 释放关系锁。
	 */
	UnlockRelationId(&relid, AccessShareLock);

	return rlocatorlist;
}

/*
 * Scan one page of the source database's pg_class relation and add relevant
 * entries to rlocatorlist. The return value is the updated list.
 *
 * 扫描源数据库 pg_class 的一页，并把相关项加入 rlocatorlist。返回值是更新后的列表。
 */
static List *
ScanSourceDatabasePgClassPage(Page page, Buffer buf, Oid tbid, Oid dbid,
							  char *srcpath, List *rlocatorlist,
							  Snapshot snapshot)
{
	BlockNumber blkno = BufferGetBlockNumber(buf);
	OffsetNumber offnum;
	OffsetNumber maxoff;
	HeapTupleData tuple;

	maxoff = PageGetMaxOffsetNumber(page);

	/* Loop over offsets. */
	/*
	 *
	 * 遍历偏移。
	 */
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;

		itemid = PageGetItemId(page, offnum);

		/* Nothing to do if slot is empty or already dead. */
		/*
		 *
		 * 槽位为空或已死亡时无需处理。
		 */
		if (!ItemIdIsUsed(itemid) || ItemIdIsDead(itemid) ||
			ItemIdIsRedirected(itemid))
			continue;

		Assert(ItemIdIsNormal(itemid));
		ItemPointerSet(&(tuple.t_self), blkno, offnum);

		/* Initialize a HeapTupleData structure. */
		/*
		 *
		 * 初始化 HeapTupleData 结构。
		 */
		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationRelationId;

		/* Skip tuples that are not visible to this snapshot. */
		/*
		 *
		 * 跳过对本快照不可见的元组。
		 */
		if (HeapTupleSatisfiesVisibility(&tuple, snapshot, buf))
		{
			CreateDBRelInfo *relinfo;

			/*
			 * ScanSourceDatabasePgClassTuple is in charge of constructing a
			 * CreateDBRelInfo object for this tuple, but can also decide that
			 * this tuple isn't something we need to copy. If we do need to
			 * copy the relation, add it to the list.
			 *
			 * ScanSourceDatabasePgClassTuple 负责为该元组构造 CreateDBRelInfo，
			 * 也可以判定该元组不需要复制。若需要复制该关系，则把它加入列表。
			 */
			relinfo = ScanSourceDatabasePgClassTuple(&tuple, tbid, dbid,
													 srcpath);
			if (relinfo != NULL)
				rlocatorlist = lappend(rlocatorlist, relinfo);
		}
	}

	return rlocatorlist;
}

/*
 * Decide whether a certain pg_class tuple represents something that
 * needs to be copied from the source database to the destination database,
 * and if so, construct a CreateDBRelInfo for it.
 *
 * 判断某条 pg_class 元组是否代表需要从源库复制到目标库的对象；
 * 若是，则为其构造 CreateDBRelInfo。
 *
 * Visibility checks are handled by the caller, so our job here is just
 * to assess the data stored in the tuple.
 *
 * 可见性检查由调用方处理，这里只评估元组中存放的数据。
 */
CreateDBRelInfo *
ScanSourceDatabasePgClassTuple(HeapTupleData *tuple, Oid tbid, Oid dbid,
							   char *srcpath)
{
	CreateDBRelInfo *relinfo;
	Form_pg_class classForm;
	RelFileNumber relfilenumber = InvalidRelFileNumber;

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * Return NULL if this object does not need to be copied.
	 *
	 * 若此对象不需要复制，则返回 NULL。
	 *
	 * Shared objects don't need to be copied, because they are shared.
	 * Objects without storage can't be copied, because there's nothing to
	 * copy. Temporary relations don't need to be copied either, because they
	 * are inaccessible outside of the session that created them, which must
	 * be gone already, and couldn't connect to a different database if it
	 * still existed. autovacuum will eventually remove the pg_class entries
	 * as well.
	 *
	 * 共享对象不必复制，因为它们是共享的。没有存储的对象无法复制，因为没有东西可复制。
	 * 临时关系也不必复制：它们在创建会话之外不可访问，而该会话必定已经结束；
	 * 即便会话还在，也不能连到另一个数据库。autovacuum 最终也会删掉这些 pg_class 项。
	 */
	if (classForm->reltablespace == GLOBALTABLESPACE_OID ||
		!RELKIND_HAS_STORAGE(classForm->relkind) ||
		classForm->relpersistence == RELPERSISTENCE_TEMP)
		return NULL;

	/*
	 * If relfilenumber is valid then directly use it.  Otherwise, consult the
	 * relmap.
	 *
	 * 若 relfilenumber 有效则直接使用，否则查阅 relmap。
	 */
	if (RelFileNumberIsValid(classForm->relfilenode))
		relfilenumber = classForm->relfilenode;
	else
		relfilenumber = RelationMapOidToFilenumberForDatabase(srcpath,
															  classForm->oid);

	/* We must have a valid relfilenumber. */
	/*
	 *
	 * 必须得到有效的 relfilenumber。
	 */
	if (!RelFileNumberIsValid(relfilenumber))
		elog(ERROR, "relation with OID %u does not have a valid relfilenumber",
			 classForm->oid);

	/* Prepare a rel info element and add it to the list. */
	/*
	 *
	 * 准备一个关系信息元素并加入列表。
	 */
	relinfo = (CreateDBRelInfo *) palloc(sizeof(CreateDBRelInfo));
	if (OidIsValid(classForm->reltablespace))
		relinfo->rlocator.spcOid = classForm->reltablespace;
	else
		relinfo->rlocator.spcOid = tbid;

	relinfo->rlocator.dbOid = dbid;
	relinfo->rlocator.relNumber = relfilenumber;
	relinfo->reloid = classForm->oid;

	/* Temporary relations were rejected above. */
	/*
	 *
	 * 临时关系已在上面被拒绝。
	 */
	Assert(classForm->relpersistence != RELPERSISTENCE_TEMP);
	relinfo->permanent =
		(classForm->relpersistence == RELPERSISTENCE_PERMANENT) ? true : false;

	return relinfo;
}

/*
 * Create database directory and write out the PG_VERSION file in the database
 * path.  If isRedo is true, it's okay for the database directory to exist
 * already.
 *
 * 在数据库路径下创建数据库目录并写出 PG_VERSION 文件。
 * 若 isRedo 为真，目录已存在也可以。
 */
static void
CreateDirAndVersionFile(char *dbpath, Oid dbid, Oid tsid, bool isRedo)
{
	int			fd;
	int			nbytes;
	char		versionfile[MAXPGPATH];
	char		buf[16];

	/*
	 * Note that we don't have to copy version data from the source database;
	 * there's only one legal value.
	 *
	 * 不必从源数据库复制版本数据；合法取值只有一个。
	 */
	sprintf(buf, "%s\n", PG_MAJORVERSION);
	nbytes = strlen(PG_MAJORVERSION) + 1;

	/* Create database directory. */
	/*
	 *
	 * 创建数据库目录。
	 */
	if (MakePGDirectory(dbpath) < 0)
	{
		/* Failure other than already exists or not in WAL replay? */
		/*
		 *
		 * 失败原因既不是已存在，也不是不在 WAL 重放中？
		 */
		if (errno != EEXIST || !isRedo)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", dbpath)));
	}

	/*
	 * Create PG_VERSION file in the database path.  If the file already
	 * exists and we are in WAL replay then try again to open it in write
	 * mode.
	 *
	 * 在数据库路径下创建 PG_VERSION 文件。
	 * 若文件已存在且正处于 WAL 重放，则再次以写模式打开。
	 */
	snprintf(versionfile, sizeof(versionfile), "%s/%s", dbpath, "PG_VERSION");

	fd = OpenTransientFile(versionfile, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0 && errno == EEXIST && isRedo)
		fd = OpenTransientFile(versionfile, O_WRONLY | O_TRUNC | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", versionfile)));

	/* Write PG_MAJORVERSION in the PG_VERSION file. */
	/*
	 *
	 * 把 PG_MAJORVERSION 写入 PG_VERSION 文件。
	 */
	pgstat_report_wait_start(WAIT_EVENT_VERSION_FILE_WRITE);
	errno = 0;
	if ((int) write(fd, buf, nbytes) != nbytes)
	{
		/* If write didn't set errno, assume problem is no disk space. */
		/*
		 *
		 * 若写操作没有设置 errno，则假定问题是磁盘空间不足。
		 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", versionfile)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_VERSION_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", versionfile)));
	fsync_fname(dbpath, true);
	pgstat_report_wait_end();

	/* Close the version file. */
	/*
	 *
	 * 关闭版本文件。
	 */
	CloseTransientFile(fd);

	/* If we are not in WAL replay then write the WAL. */
	/*
	 *
	 * 若不在 WAL 重放中，则写 WAL。
	 */
	if (!isRedo)
	{
		xl_dbase_create_wal_log_rec xlrec;

		START_CRIT_SECTION();

		xlrec.db_id = dbid;
		xlrec.tablespace_id = tsid;

		XLogBeginInsert();
		XLogRegisterData(&xlrec,
						 sizeof(xl_dbase_create_wal_log_rec));

		(void) XLogInsert(RM_DBASE_ID, XLOG_DBASE_CREATE_WAL_LOG);

		END_CRIT_SECTION();
	}
}

/*
 * Create a new database using the FILE_COPY strategy.
 *
 * 使用 FILE_COPY 策略创建新数据库。
 *
 * Copy each tablespace at the filesystem level, and log a single WAL record
 * for each tablespace copied.  This requires a checkpoint before and after the
 * copy, which may be expensive, but it does greatly reduce WAL generation
 * if the copied database is large.
 *
 * 在文件系统层复制每个表空间，并为每个被复制的表空间记录一条 WAL。
 * 复制前后都需要检查点，代价可能较高，但当被复制的数据库很大时，能大幅减少 WAL 生成量。
 */
static void
CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid, Oid src_tsid,
							Oid dst_tsid)
{
	TableScanDesc scan;
	Relation	rel;
	HeapTuple	tuple;

	/*
	 * Force a checkpoint before starting the copy. This will force all dirty
	 * buffers, including those of unlogged tables, out to disk, to ensure
	 * source database is up-to-date on disk for the copy.
	 * FlushDatabaseBuffers() would suffice for that, but we also want to
	 * process any pending unlink requests. Otherwise, if a checkpoint
	 * happened while we're copying files, a file might be deleted just when
	 * we're about to copy it, causing the lstat() call in copydir() to fail
	 * with ENOENT.
	 *
	 * 开始复制前强制做一次检查点。这会把所有脏缓冲区（包括 unlogged 表的）刷到磁盘，
	 * 保证源库在磁盘上是最新的，复制才正确。FlushDatabaseBuffers() 对此已经够用，
	 * 但还要处理待处理的 unlink 请求。否则若复制文件期间发生检查点，
	 * 文件可能刚好在即将复制时被删除，导致 copydir() 里的 lstat() 因 ENOENT 失败。
	 *
	 * In binary upgrade mode, we can skip this checkpoint because pg_upgrade
	 * is careful to ensure that template0 is fully written to disk prior to
	 * any CREATE DATABASE commands.
	 *
	 * 二进制升级模式下可以跳过这次检查点，因为 pg_upgrade 会确保在任何 CREATE DATABASE 之前 template0 已完整落盘。
	 */
	if (!IsBinaryUpgrade)
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE |
						  CHECKPOINT_WAIT | CHECKPOINT_FLUSH_ALL);

	/*
	 * Iterate through all tablespaces of the template database, and copy each
	 * one to the new database.
	 *
	 * 遍历模板数据库的所有表空间，并把每一个复制到新数据库。
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spaceform = (Form_pg_tablespace) GETSTRUCT(tuple);
		Oid			srctablespace = spaceform->oid;
		Oid			dsttablespace;
		char	   *srcpath;
		char	   *dstpath;
		struct stat st;

		/* No need to copy global tablespace */
		/*
		 *
		 * 不必复制全局表空间
		 */
		if (srctablespace == GLOBALTABLESPACE_OID)
			continue;

		srcpath = GetDatabasePath(src_dboid, srctablespace);

		if (stat(srcpath, &st) < 0 || !S_ISDIR(st.st_mode) ||
			directory_is_empty(srcpath))
		{
			/* Assume we can ignore it */
			/*
			 *
			 * 假定可以忽略
			 */
			pfree(srcpath);
			continue;
		}

		if (srctablespace == src_tsid)
			dsttablespace = dst_tsid;
		else
			dsttablespace = srctablespace;

		dstpath = GetDatabasePath(dst_dboid, dsttablespace);

		/*
		 * Copy this subdirectory to the new location
		 *
		 * 把该子目录复制到新位置
		 *
		 * We don't need to copy subdirectories
		 *
		 * 不需要复制子目录
		 */
		copydir(srcpath, dstpath, false);

		/* Record the filesystem change in XLOG */
		/*
		 *
		 * 把文件系统变更记入 XLOG
		 */
		{
			xl_dbase_create_file_copy_rec xlrec;

			xlrec.db_id = dst_dboid;
			xlrec.tablespace_id = dsttablespace;
			xlrec.src_db_id = src_dboid;
			xlrec.src_tablespace_id = srctablespace;

			XLogBeginInsert();
			XLogRegisterData(&xlrec,
							 sizeof(xl_dbase_create_file_copy_rec));

			(void) XLogInsert(RM_DBASE_ID,
							  XLOG_DBASE_CREATE_FILE_COPY | XLR_SPECIAL_REL_UPDATE);
		}
		pfree(srcpath);
		pfree(dstpath);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * We force a checkpoint before committing.  This effectively means that
	 * committed XLOG_DBASE_CREATE_FILE_COPY operations will never need to be
	 * replayed (at least not in ordinary crash recovery; we still have to
	 * make the XLOG entry for the benefit of PITR operations). This avoids
	 * two nasty scenarios:
	 *
	 * 提交前强制做检查点。这实际上意味着已提交的 XLOG_DBASE_CREATE_FILE_COPY 操作
	 * 在普通崩溃恢复中不需要重放（为了 PITR 仍要写这条 XLOG）。这样可以避免两种糟糕情况：
	 *
	 * #1: At wal_level=minimal, we don't XLOG the contents of newly created
	 * relfilenodes; therefore the drop-and-recreate-whole-directory behavior
	 * of DBASE_CREATE replay would lose such files created in the new
	 * database between our commit and the next checkpoint.
	 *
	 * 情形 1：wal_level=minimal 时不为新建的 relfilenode 内容写 XLOG，
	 * 因此 DBASE_CREATE 重放时删除并重建整个目录的行为，会丢失本次提交与下一次检查点之间、
	 * 在新数据库中创建的这类文件。
	 *
	 * #2: Since we have to recopy the source database during DBASE_CREATE
	 * replay, we run the risk of copying changes in it that were committed
	 * after the original CREATE DATABASE command but before the system crash
	 * that led to the replay.  This is at least unexpected and at worst could
	 * lead to inconsistencies, eg duplicate table names.
	 *
	 * 情形 2：DBASE_CREATE 重放时必须重新复制源数据库，因而可能复制到原始 CREATE DATABASE 之后、
	 * 导致本次重放的崩溃之前才提交的变更。这至少出人意料，最坏会导致不一致，例如表名重复。
	 *
	 * (Both of these were real bugs in releases 8.0 through 8.0.3.)
	 *
	 * （这两处都是 8.0 到 8.0.3 版本中的真实缺陷。）
	 *
	 * In PITR replay, the first of these isn't an issue, and the second is
	 * only a risk if the CREATE DATABASE and subsequent template database
	 * change both occur while a base backup is being taken. There doesn't
	 * seem to be much we can do about that except document it as a
	 * limitation.
	 *
	 * 在 PITR 重放中，第一种情况不是问题；第二种只有在做基础备份期间，
	 * 同时发生 CREATE DATABASE 以及随后对模板库的修改时才有风险。除了把它记为限制，似乎没有太多办法。
	 *
	 * In binary upgrade mode, we can skip this checkpoint because neither of
	 * these problems applies: we don't ever replay the WAL generated during
	 * pg_upgrade, and we don't support taking base backups during pg_upgrade
	 * (not to mention that we don't concurrently modify template0, either).
	 *
	 * 二进制升级模式下可以跳过这次检查点，因为这两个问题都不适用：
	 * 我们从不重放 pg_upgrade 期间生成的 WAL，也不支持在 pg_upgrade 期间做基础备份
	 * （更何况也不会并发修改 template0）。
	 *
	 * See CreateDatabaseUsingWalLog() for a less cheesy CREATE DATABASE
	 * strategy that avoids these problems.
	 *
	 * 更稳妥、能避开这些问题的 CREATE DATABASE 策略见 CreateDatabaseUsingWalLog()。
	 */
	if (!IsBinaryUpgrade)
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE |
						  CHECKPOINT_WAIT);
}

/*
 * CREATE DATABASE
 *
 * CREATE DATABASE 命令。
 */
Oid
createdb(ParseState *pstate, const CreatedbStmt *stmt)
{
	Oid			src_dboid;
	Oid			src_owner;
	int			src_encoding = -1;
	char	   *src_collate = NULL;
	char	   *src_ctype = NULL;
	char	   *src_locale = NULL;
	char	   *src_icurules = NULL;
	char		src_locprovider = '\0';
	char	   *src_collversion = NULL;
	bool		src_istemplate;
	bool		src_hasloginevt = false;
	bool		src_allowconn;
	TransactionId src_frozenxid = InvalidTransactionId;
	MultiXactId src_minmxid = InvalidMultiXactId;
	Oid			src_deftablespace;
	volatile Oid dst_deftablespace;
	Relation	pg_database_rel;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_database] = {0};
	bool		new_record_nulls[Natts_pg_database] = {0};
	Oid			dboid = InvalidOid;
	Oid			datdba;
	ListCell   *option;
	DefElem    *tablespacenameEl = NULL;
	DefElem    *ownerEl = NULL;
	DefElem    *templateEl = NULL;
	DefElem    *encodingEl = NULL;
	DefElem    *localeEl = NULL;
	DefElem    *builtinlocaleEl = NULL;
	DefElem    *collateEl = NULL;
	DefElem    *ctypeEl = NULL;
	DefElem    *iculocaleEl = NULL;
	DefElem    *icurulesEl = NULL;
	DefElem    *locproviderEl = NULL;
	DefElem    *istemplateEl = NULL;
	DefElem    *allowconnectionsEl = NULL;
	DefElem    *connlimitEl = NULL;
	DefElem    *collversionEl = NULL;
	DefElem    *strategyEl = NULL;
	char	   *dbname = stmt->dbname;
	char	   *dbowner = NULL;
	const char *dbtemplate = NULL;
	char	   *dbcollate = NULL;
	char	   *dbctype = NULL;
	const char *dblocale = NULL;
	char	   *dbicurules = NULL;
	char		dblocprovider = '\0';
	char	   *canonname;
	int			encoding = -1;
	bool		dbistemplate = false;
	bool		dballowconnections = true;
	int			dbconnlimit = DATCONNLIMIT_UNLIMITED;
	char	   *dbcollversion = NULL;
	int			notherbackends;
	int			npreparedxacts;
	CreateDBStrategy dbstrategy = CREATEDB_WAL_LOG;
	createdb_failure_params fparms;

	/* Extract options from the statement node tree */
	/*
	 *
	 * 从语句节点树中提取选项
	 */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "tablespace") == 0)
		{
			if (tablespacenameEl)
				errorConflictingDefElem(defel, pstate);
			tablespacenameEl = defel;
		}
		else if (strcmp(defel->defname, "owner") == 0)
		{
			if (ownerEl)
				errorConflictingDefElem(defel, pstate);
			ownerEl = defel;
		}
		else if (strcmp(defel->defname, "template") == 0)
		{
			if (templateEl)
				errorConflictingDefElem(defel, pstate);
			templateEl = defel;
		}
		else if (strcmp(defel->defname, "encoding") == 0)
		{
			if (encodingEl)
				errorConflictingDefElem(defel, pstate);
			encodingEl = defel;
		}
		else if (strcmp(defel->defname, "locale") == 0)
		{
			if (localeEl)
				errorConflictingDefElem(defel, pstate);
			localeEl = defel;
		}
		else if (strcmp(defel->defname, "builtin_locale") == 0)
		{
			if (builtinlocaleEl)
				errorConflictingDefElem(defel, pstate);
			builtinlocaleEl = defel;
		}
		else if (strcmp(defel->defname, "lc_collate") == 0)
		{
			if (collateEl)
				errorConflictingDefElem(defel, pstate);
			collateEl = defel;
		}
		else if (strcmp(defel->defname, "lc_ctype") == 0)
		{
			if (ctypeEl)
				errorConflictingDefElem(defel, pstate);
			ctypeEl = defel;
		}
		else if (strcmp(defel->defname, "icu_locale") == 0)
		{
			if (iculocaleEl)
				errorConflictingDefElem(defel, pstate);
			iculocaleEl = defel;
		}
		else if (strcmp(defel->defname, "icu_rules") == 0)
		{
			if (icurulesEl)
				errorConflictingDefElem(defel, pstate);
			icurulesEl = defel;
		}
		else if (strcmp(defel->defname, "locale_provider") == 0)
		{
			if (locproviderEl)
				errorConflictingDefElem(defel, pstate);
			locproviderEl = defel;
		}
		else if (strcmp(defel->defname, "is_template") == 0)
		{
			if (istemplateEl)
				errorConflictingDefElem(defel, pstate);
			istemplateEl = defel;
		}
		else if (strcmp(defel->defname, "allow_connections") == 0)
		{
			if (allowconnectionsEl)
				errorConflictingDefElem(defel, pstate);
			allowconnectionsEl = defel;
		}
		else if (strcmp(defel->defname, "connection_limit") == 0)
		{
			if (connlimitEl)
				errorConflictingDefElem(defel, pstate);
			connlimitEl = defel;
		}
		else if (strcmp(defel->defname, "collation_version") == 0)
		{
			if (collversionEl)
				errorConflictingDefElem(defel, pstate);
			collversionEl = defel;
		}
		else if (strcmp(defel->defname, "location") == 0)
		{
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("LOCATION is not supported anymore"),
					 errhint("Consider using tablespaces instead."),
					 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "oid") == 0)
		{
			dboid = defGetObjectId(defel);

			/*
			 * We don't normally permit new databases to be created with
			 * system-assigned OIDs. pg_upgrade tries to preserve database
			 * OIDs, so we can't allow any database to be created with an OID
			 * that might be in use in a freshly-initialized cluster created
			 * by some future version. We assume all such OIDs will be from
			 * the system-managed OID range.
			 *
			 * 通常不允许用系统分配的 OID 创建新数据库。pg_upgrade 会尽量保留数据库 OID，
			 * 因此不能允许任何数据库使用某个未来版本全新初始化的集群中可能占用的 OID。
			 * 假定这类 OID 都来自系统管理的 OID 范围。
			 *
			 * As an exception, however, we permit any OID to be assigned when
			 * allow_system_table_mods=on (so that initdb can assign system
			 * OIDs to template0 and postgres) or when performing a binary
			 * upgrade (so that pg_upgrade can preserve whatever OIDs it finds
			 * in the source cluster).
			 *
			 * 不过有例外：allow_system_table_mods=on 时允许指定任意 OID
			 * （以便 initdb 把系统 OID 分给 template0 和 postgres），
			 * 二进制升级时也允许（以便 pg_upgrade 保留源集群中的 OID）。
			 */
			if (dboid < FirstNormalObjectId &&
				!allowSystemTableMods && !IsBinaryUpgrade)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
						errmsg("OIDs less than %u are reserved for system objects", FirstNormalObjectId));
		}
		else if (strcmp(defel->defname, "strategy") == 0)
		{
			if (strategyEl)
				errorConflictingDefElem(defel, pstate);
			strategyEl = defel;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname),
					 parser_errposition(pstate, defel->location)));
	}

	if (ownerEl && ownerEl->arg)
		dbowner = defGetString(ownerEl);
	if (templateEl && templateEl->arg)
		dbtemplate = defGetString(templateEl);
	if (encodingEl && encodingEl->arg)
	{
		const char *encoding_name;

		if (IsA(encodingEl->arg, Integer))
		{
			encoding = defGetInt32(encodingEl);
			encoding_name = pg_encoding_to_char(encoding);
			if (strcmp(encoding_name, "") == 0 ||
				pg_valid_server_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%d is not a valid encoding code",
								encoding),
						 parser_errposition(pstate, encodingEl->location)));
		}
		else
		{
			encoding_name = defGetString(encodingEl);
			encoding = pg_valid_server_encoding(encoding_name);
			if (encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%s is not a valid encoding name",
								encoding_name),
						 parser_errposition(pstate, encodingEl->location)));
		}
	}
	if (localeEl && localeEl->arg)
	{
		dbcollate = defGetString(localeEl);
		dbctype = defGetString(localeEl);
		dblocale = defGetString(localeEl);
	}
	if (builtinlocaleEl && builtinlocaleEl->arg)
		dblocale = defGetString(builtinlocaleEl);
	if (collateEl && collateEl->arg)
		dbcollate = defGetString(collateEl);
	if (ctypeEl && ctypeEl->arg)
		dbctype = defGetString(ctypeEl);
	if (iculocaleEl && iculocaleEl->arg)
		dblocale = defGetString(iculocaleEl);
	if (icurulesEl && icurulesEl->arg)
		dbicurules = defGetString(icurulesEl);
	if (locproviderEl && locproviderEl->arg)
	{
		char	   *locproviderstr = defGetString(locproviderEl);

		if (pg_strcasecmp(locproviderstr, "builtin") == 0)
			dblocprovider = COLLPROVIDER_BUILTIN;
		else if (pg_strcasecmp(locproviderstr, "icu") == 0)
			dblocprovider = COLLPROVIDER_ICU;
		else if (pg_strcasecmp(locproviderstr, "libc") == 0)
			dblocprovider = COLLPROVIDER_LIBC;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("unrecognized locale provider: %s",
							locproviderstr)));
	}
	if (istemplateEl && istemplateEl->arg)
		dbistemplate = defGetBoolean(istemplateEl);
	if (allowconnectionsEl && allowconnectionsEl->arg)
		dballowconnections = defGetBoolean(allowconnectionsEl);
	if (connlimitEl && connlimitEl->arg)
	{
		dbconnlimit = defGetInt32(connlimitEl);
		if (dbconnlimit < DATCONNLIMIT_UNLIMITED)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", dbconnlimit)));
	}
	if (collversionEl)
		dbcollversion = defGetString(collversionEl);

	/* obtain OID of proposed owner */
	/*
	 *
	 * 取得拟定属主的 OID
	 */
	if (dbowner)
		datdba = get_role_oid(dbowner, false);
	else
		datdba = GetUserId();

	/*
	 * To create a database, must have createdb privilege and must be able to
	 * become the target role (this does not imply that the target role itself
	 * must have createdb privilege).  The latter provision guards against
	 * "giveaway" attacks.  Note that a superuser will always have both of
	 * these privileges a fortiori.
	 *
	 * 创建数据库必须具备 createdb 权限，并且必须能够成为目标角色
	 * （这并不意味着目标角色自身必须有 createdb 权限）。后一条用于防范把对象赠送给他人的攻击。
	 * 超级用户必然同时拥有这两项权限。
	 */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create database")));

	check_can_set_role(GetUserId(), datdba);

	/*
	 * Lookup database (template) to be cloned, and obtain share lock on it.
	 * ShareLock allows two CREATE DATABASEs to work from the same template
	 * concurrently, while ensuring no one is busy dropping it in parallel
	 * (which would be Very Bad since we'd likely get an incomplete copy
	 * without knowing it).  This also prevents any new connections from being
	 * made to the source until we finish copying it, so we can be sure it
	 * won't change underneath us.
	 *
	 * 查找要克隆的数据库（模板）并对其加共享锁。ShareLock 允许两个 CREATE DATABASE
	 * 并发使用同一模板，同时确保没有人正在并行删除它（那会非常糟糕，因为很可能在不知情时得到不完整副本）。
	 * 这也阻止在复制完成前有新连接进入源库，从而保证它不会在复制过程中被修改。
	 */
	if (!dbtemplate)
		dbtemplate = "template1";	/* Default template database name */
		/*
		 *
		 * 默认模板数据库名
		 */

	if (!get_db_info(dbtemplate, ShareLock,
					 &src_dboid, &src_owner, &src_encoding,
					 &src_istemplate, &src_allowconn, &src_hasloginevt,
					 &src_frozenxid, &src_minmxid, &src_deftablespace,
					 &src_collate, &src_ctype, &src_locale, &src_icurules, &src_locprovider,
					 &src_collversion))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("template database \"%s\" does not exist",
						dbtemplate)));

	/*
	 * If the source database was in the process of being dropped, we can't
	 * use it as a template.
	 *
	 * 若源数据库正在被删除，则不能把它当作模板。
	 */
	if (database_is_invalid_oid(src_dboid))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot use invalid database \"%s\" as template", dbtemplate),
				errhint("Use DROP DATABASE to drop invalid databases."));

	/*
	 * Permission check: to copy a DB that's not marked datistemplate, you
	 * must be superuser or the owner thereof.
	 *
	 * 权限检查：要复制未标记 datistemplate 的数据库，必须是超级用户或其属主。
	 */
	if (!src_istemplate)
	{
		if (!object_ownercheck(DatabaseRelationId, src_dboid, GetUserId()))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to copy database \"%s\"",
							dbtemplate)));
	}

	/* Validate the database creation strategy. */
	/*
	 *
	 * 校验数据库创建策略。
	 */
	if (strategyEl && strategyEl->arg)
	{
		char	   *strategy;

		strategy = defGetString(strategyEl);
		if (pg_strcasecmp(strategy, "wal_log") == 0)
			dbstrategy = CREATEDB_WAL_LOG;
		else if (pg_strcasecmp(strategy, "file_copy") == 0)
			dbstrategy = CREATEDB_FILE_COPY;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid create database strategy \"%s\"", strategy),
					 errhint("Valid strategies are \"wal_log\" and \"file_copy\".")));
	}

	/* If encoding or locales are defaulted, use source's setting */
	/*
	 *
	 * 若编码或 locale 使用默认值，则采用源库的设置
	 */
	if (encoding < 0)
		encoding = src_encoding;
	if (dbcollate == NULL)
		dbcollate = src_collate;
	if (dbctype == NULL)
		dbctype = src_ctype;
	if (dblocprovider == '\0')
		dblocprovider = src_locprovider;
	if (dblocale == NULL && dblocprovider == src_locprovider)
		dblocale = src_locale;
	if (dbicurules == NULL)
		dbicurules = src_icurules;

	/* Some encodings are client only */
	/*
	 *
	 * 有些编码仅用于客户端
	 */
	if (!PG_VALID_BE_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid server encoding %d", encoding)));

	/* Check that the chosen locales are valid, and get canonical spellings */
	/*
	 *
	 * 检查所选 locale 是否有效，并取得规范拼写
	 */
	if (!check_locale(LC_COLLATE, dbcollate, &canonname))
	{
		if (dblocprovider == COLLPROVIDER_BUILTIN)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate),
					 errhint("If the locale name is specific to the builtin provider, use BUILTIN_LOCALE.")));
		else if (dblocprovider == COLLPROVIDER_ICU)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate),
					 errhint("If the locale name is specific to the ICU provider, use ICU_LOCALE.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate)));
	}
	dbcollate = canonname;
	if (!check_locale(LC_CTYPE, dbctype, &canonname))
	{
		if (dblocprovider == COLLPROVIDER_BUILTIN)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype),
					 errhint("If the locale name is specific to the builtin provider, use BUILTIN_LOCALE.")));
		else if (dblocprovider == COLLPROVIDER_ICU)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype),
					 errhint("If the locale name is specific to the ICU provider, use ICU_LOCALE.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype)));
	}

	dbctype = canonname;

	check_encoding_locale_matches(encoding, dbcollate, dbctype);

	/* validate provider-specific parameters */
	/*
	 *
	 * 校验特定提供者的参数
	 */
	if (dblocprovider != COLLPROVIDER_BUILTIN)
	{
		if (builtinlocaleEl)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("BUILTIN_LOCALE cannot be specified unless locale provider is builtin")));
	}

	if (dblocprovider != COLLPROVIDER_ICU)
	{
		if (iculocaleEl)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("ICU locale cannot be specified unless locale provider is ICU")));

		if (dbicurules)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("ICU rules cannot be specified unless locale provider is ICU")));
	}

	/* validate and canonicalize locale for the provider */
	/*
	 *
	 * 为该提供者校验并规范化 locale
	 */
	if (dblocprovider == COLLPROVIDER_BUILTIN)
	{
		/*
		 * This would happen if template0 uses the libc provider but the new
		 * database uses builtin.
		 *
		 * 若 template0 使用 libc 提供者而新数据库使用 builtin，就会发生这种情况。
		 */
		if (!dblocale)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("LOCALE or BUILTIN_LOCALE must be specified")));

		dblocale = builtin_validate_locale(encoding, dblocale);
	}
	else if (dblocprovider == COLLPROVIDER_ICU)
	{
		if (!(is_encoding_supported_by_icu(encoding)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("encoding \"%s\" is not supported with ICU provider",
							pg_encoding_to_char(encoding))));

		/*
		 * This would happen if template0 uses the libc provider but the new
		 * database uses icu.
		 *
		 * 若 template0 使用 libc 提供者而新数据库使用 icu，就会发生这种情况。
		 */
		if (!dblocale)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("LOCALE or ICU_LOCALE must be specified")));

		/*
		 * During binary upgrade, or when the locale came from the template
		 * database, preserve locale string. Otherwise, canonicalize to a
		 * language tag.
		 *
		 * 二进制升级期间，或 locale 来自模板数据库时，保留 locale 字符串。否则规范化为语言标签。
		 */
		if (!IsBinaryUpgrade && dblocale != src_locale)
		{
			char	   *langtag = icu_language_tag(dblocale,
												   icu_validation_level);

			if (langtag && strcmp(dblocale, langtag) != 0)
			{
				ereport(NOTICE,
						(errmsg("using standard form \"%s\" for ICU locale \"%s\"",
								langtag, dblocale)));

				dblocale = langtag;
			}
		}

		icu_validate_locale(dblocale);
	}

	/* for libc, locale comes from datcollate and datctype */
	/*
	 *
	 * 对于 libc，locale 来自 datcollate 和 datctype
	 */
	if (dblocprovider == COLLPROVIDER_LIBC)
		dblocale = NULL;

	/*
	 * Check that the new encoding and locale settings match the source
	 * database.  We insist on this because we simply copy the source data ---
	 * any non-ASCII data would be wrongly encoded, and any indexes sorted
	 * according to the source locale would be wrong.
	 *
	 * 检查新的编码和 locale 设置是否与源数据库一致。必须如此，因为我们只是复制源数据：
	 * 任何非 ASCII 数据都会被错误编码，按源 locale 排序的索引也会是错的。
	 *
	 * However, we assume that template0 doesn't contain any non-ASCII data
	 * nor any indexes that depend on collation or ctype, so template0 can be
	 * used as template for creating a database with any encoding or locale.
	 *
	 * 不过假定 template0 不含非 ASCII 数据，也不含依赖 collation 或 ctype 的索引，
	 * 因此可以用 template0 为任意编码或 locale 创建数据库。
	 */
	if (strcmp(dbtemplate, "template0") != 0)
	{
		if (encoding != src_encoding)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new encoding (%s) is incompatible with the encoding of the template database (%s)",
							pg_encoding_to_char(encoding),
							pg_encoding_to_char(src_encoding)),
					 errhint("Use the same encoding as in the template database, or use template0 as template.")));

		if (strcmp(dbcollate, src_collate) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new collation (%s) is incompatible with the collation of the template database (%s)",
							dbcollate, src_collate),
					 errhint("Use the same collation as in the template database, or use template0 as template.")));

		if (strcmp(dbctype, src_ctype) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new LC_CTYPE (%s) is incompatible with the LC_CTYPE of the template database (%s)",
							dbctype, src_ctype),
					 errhint("Use the same LC_CTYPE as in the template database, or use template0 as template.")));

		if (dblocprovider != src_locprovider)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new locale provider (%s) does not match locale provider of the template database (%s)",
							collprovider_name(dblocprovider), collprovider_name(src_locprovider)),
					 errhint("Use the same locale provider as in the template database, or use template0 as template.")));

		if (dblocprovider == COLLPROVIDER_ICU)
		{
			char	   *val1;
			char	   *val2;

			Assert(dblocale);
			Assert(src_locale);
			if (strcmp(dblocale, src_locale) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("new ICU locale (%s) is incompatible with the ICU locale of the template database (%s)",
								dblocale, src_locale),
						 errhint("Use the same ICU locale as in the template database, or use template0 as template.")));

			val1 = dbicurules;
			if (!val1)
				val1 = "";
			val2 = src_icurules;
			if (!val2)
				val2 = "";
			if (strcmp(val1, val2) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("new ICU collation rules (%s) are incompatible with the ICU collation rules of the template database (%s)",
								val1, val2),
						 errhint("Use the same ICU collation rules as in the template database, or use template0 as template.")));
		}
	}

	/*
	 * If we got a collation version for the template database, check that it
	 * matches the actual OS collation version.  Otherwise error; the user
	 * needs to fix the template database first.  Don't complain if a
	 * collation version was specified explicitly as a statement option; that
	 * is used by pg_upgrade to reproduce the old state exactly.
	 *
	 * 若取得了模板数据库的 collation 版本，则检查它是否与操作系统实际的 collation 版本一致，
	 * 否则报错；用户需要先修复模板数据库。若 collation 版本是语句选项显式指定的，则不抱怨；
	 * pg_upgrade 用它来精确复现旧状态。
	 *
	 * (If the template database has no collation version, then either the
	 * platform/provider does not support collation versioning, or it's
	 * template0, for which we stipulate that it does not contain
	 * collation-using objects.)
	 *
	 * （若模板数据库没有 collation 版本，则要么平台或提供者不支持 collation 版本管理，
	 * 要么它是 template0；我们约定 template0 不含使用 collation 的对象。）
	 */
	if (src_collversion && !collversionEl)
	{
		char	   *actual_versionstr;
		const char *locale;

		if (dblocprovider == COLLPROVIDER_LIBC)
			locale = dbcollate;
		else
			locale = dblocale;

		actual_versionstr = get_collation_actual_version(dblocprovider, locale);
		if (!actual_versionstr)
			ereport(ERROR,
					(errmsg("template database \"%s\" has a collation version, but no actual collation version could be determined",
							dbtemplate)));

		if (strcmp(actual_versionstr, src_collversion) != 0)
			ereport(ERROR,
					(errmsg("template database \"%s\" has a collation version mismatch",
							dbtemplate),
					 errdetail("The template database was created using collation version %s, "
							   "but the operating system provides version %s.",
							   src_collversion, actual_versionstr),
					 errhint("Rebuild all objects in the template database that use the default collation and run "
							 "ALTER DATABASE %s REFRESH COLLATION VERSION, "
							 "or build PostgreSQL with the right library version.",
							 quote_identifier(dbtemplate))));
	}

	if (dbcollversion == NULL)
		dbcollversion = src_collversion;

	/*
	 * Normally, we copy the collation version from the template database.
	 * This last resort only applies if the template database does not have a
	 * collation version, which is normally only the case for template0.
	 *
	 * 通常从模板数据库复制 collation 版本。只有模板数据库没有 collation 版本时才走这最后的退路，
	 * 正常情况下仅 template0 如此。
	 */
	if (dbcollversion == NULL)
	{
		const char *locale;

		if (dblocprovider == COLLPROVIDER_LIBC)
			locale = dbcollate;
		else
			locale = dblocale;

		dbcollversion = get_collation_actual_version(dblocprovider, locale);
	}

	/* Resolve default tablespace for new database */
	/*
	 *
	 * 确定新数据库的默认表空间
	 */
	if (tablespacenameEl && tablespacenameEl->arg)
	{
		char	   *tablespacename;
		AclResult	aclresult;

		tablespacename = defGetString(tablespacenameEl);
		dst_deftablespace = get_tablespace_oid(tablespacename, false);
		/* check permissions */
		/*
		 *
		 * 检查权限
		 */
		aclresult = object_aclcheck(TableSpaceRelationId, dst_deftablespace, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   tablespacename);

		/* pg_global must never be the default tablespace */
		/*
		 *
		 * pg_global 绝不能作为默认表空间
		 */
		if (dst_deftablespace == GLOBALTABLESPACE_OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_global cannot be used as default tablespace")));

		/*
		 * If we are trying to change the default tablespace of the template,
		 * we require that the template not have any files in the new default
		 * tablespace.  This is necessary because otherwise the copied
		 * database would contain pg_class rows that refer to its default
		 * tablespace both explicitly (by OID) and implicitly (as zero), which
		 * would cause problems.  For example another CREATE DATABASE using
		 * the copied database as template, and trying to change its default
		 * tablespace again, would yield outright incorrect results (it would
		 * improperly move tables to the new default tablespace that should
		 * stay in the same tablespace).
		 *
		 * 若要更改模板的默认表空间，则要求模板在新的默认表空间中没有任何文件。
		 * 否则复制出的数据库里，pg_class 行会既显式（通过 OID）又隐式（为零）引用默认表空间，从而出问题。
		 * 例如再用该副本做 CREATE DATABASE 并再次更改默认表空间，会得到完全错误的结果
		 * （本应留在原表空间的表会被错误地移到新的默认表空间）。
		 */
		if (dst_deftablespace != src_deftablespace)
		{
			char	   *srcpath;
			struct stat st;

			srcpath = GetDatabasePath(src_dboid, dst_deftablespace);

			if (stat(srcpath, &st) == 0 &&
				S_ISDIR(st.st_mode) &&
				!directory_is_empty(srcpath))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot assign new default tablespace \"%s\"",
								tablespacename),
						 errdetail("There is a conflict because database \"%s\" already has some tables in this tablespace.",
								   dbtemplate)));
			pfree(srcpath);
		}
	}
	else
	{
		/* Use template database's default tablespace */
		/*
		 *
		 * 使用模板数据库的默认表空间
		 */
		dst_deftablespace = src_deftablespace;
		/* Note there is no additional permission check in this path */
		/*
		 *
		 * 注意这条路径上没有额外的权限检查
		 */
	}

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for database names are violated.  But don't complain during
	 * initdb.
	 *
	 * 若以相应开关编译，则在数据库名违反回归测试约定时发出警告。但 initdb 期间不抱怨。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (IsUnderPostmaster && strstr(dbname, "regression") == NULL)
		elog(WARNING, "databases created by regression test cases should have names including \"regression\"");
#endif

	/*
	 * Check for db name conflict.  This is just to give a more friendly error
	 * message than "unique index violation".  There's a race condition but
	 * we're willing to accept the less friendly message in that case.
	 *
	 * 检查数据库名冲突。这只是为了给出比唯一索引冲突更友好的错误信息。
	 * 存在竞态，那种情况下接受不那么友好的信息。
	 */
	if (OidIsValid(get_database_oid(dbname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", dbname)));

	/*
	 * The source DB can't have any active backends, except this one
	 * (exception is to allow CREATE DB while connected to template1).
	 * Otherwise we might copy inconsistent data.
	 *
	 * 除本后端外，源数据库不能有任何活动后端（例外是允许在连接到 template1 时执行 CREATE DATABASE）。
	 * 否则可能复制到不一致的数据。
	 *
	 * This should be last among the basic error checks, because it involves
	 * potential waiting; we may as well throw an error first if we're gonna
	 * throw one.
	 *
	 * 这项检查应放在基本错误检查的最后，因为它可能等待；既然反正要报错，不如先报别的错。
	 */
	if (CountOtherDBBackends(src_dboid, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("source database \"%s\" is being accessed by other users",
						dbtemplate),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * Select an OID for the new database, checking that it doesn't have a
	 * filename conflict with anything already existing in the tablespace
	 * directories.
	 *
	 * 为新数据库选择 OID，并检查它与表空间目录中已有文件名没有冲突。
	 */
	pg_database_rel = table_open(DatabaseRelationId, RowExclusiveLock);

	/*
	 * If database OID is configured, check if the OID is already in use or
	 * data directory already exists.
	 *
	 * 若配置了数据库 OID，则检查该 OID 是否已被占用，或数据目录是否已存在。
	 */
	if (OidIsValid(dboid))
	{
		char	   *existing_dbname = get_database_name(dboid);

		if (existing_dbname != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
					errmsg("database OID %u is already in use by database \"%s\"",
						   dboid, existing_dbname));

		if (check_db_file_conflict(dboid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
					errmsg("data directory with the specified OID %u already exists", dboid));
	}
	else
	{
		/* Select an OID for the new database if is not explicitly configured. */
		/*
		 *
		 * 若未显式配置，则为新数据库选择一个 OID。
		 */
		do
		{
			dboid = GetNewOidWithIndex(pg_database_rel, DatabaseOidIndexId,
									   Anum_pg_database_oid);
		} while (check_db_file_conflict(dboid));
	}

	/*
	 * Insert a new tuple into pg_database.  This establishes our ownership of
	 * the new database name (anyone else trying to insert the same name will
	 * block on the unique index, and fail after we commit).
	 *
	 * 向 pg_database 插入新元组。这确立我们对新数据库名的所有权
	 * （其他人再插入同名会在唯一索引上阻塞，并在我们提交后失败）。
	 */

	Assert((dblocprovider != COLLPROVIDER_LIBC && dblocale) ||
		   (dblocprovider == COLLPROVIDER_LIBC && !dblocale));

	/* Form tuple */
	/*
	 *
	 * 构造元组
	 */
	new_record[Anum_pg_database_oid - 1] = ObjectIdGetDatum(dboid);
	new_record[Anum_pg_database_datname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbname));
	new_record[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(datdba);
	new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
	new_record[Anum_pg_database_datlocprovider - 1] = CharGetDatum(dblocprovider);
	new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
	new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(dballowconnections);
	new_record[Anum_pg_database_dathasloginevt - 1] = BoolGetDatum(src_hasloginevt);
	new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
	new_record[Anum_pg_database_datfrozenxid - 1] = TransactionIdGetDatum(src_frozenxid);
	new_record[Anum_pg_database_datminmxid - 1] = TransactionIdGetDatum(src_minmxid);
	new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_deftablespace);
	new_record[Anum_pg_database_datcollate - 1] = CStringGetTextDatum(dbcollate);
	new_record[Anum_pg_database_datctype - 1] = CStringGetTextDatum(dbctype);
	if (dblocale)
		new_record[Anum_pg_database_datlocale - 1] = CStringGetTextDatum(dblocale);
	else
		new_record_nulls[Anum_pg_database_datlocale - 1] = true;
	if (dbicurules)
		new_record[Anum_pg_database_daticurules - 1] = CStringGetTextDatum(dbicurules);
	else
		new_record_nulls[Anum_pg_database_daticurules - 1] = true;
	if (dbcollversion)
		new_record[Anum_pg_database_datcollversion - 1] = CStringGetTextDatum(dbcollversion);
	else
		new_record_nulls[Anum_pg_database_datcollversion - 1] = true;

	/*
	 * We deliberately set datacl to default (NULL), rather than copying it
	 * from the template database.  Copying it would be a bad idea when the
	 * owner is not the same as the template's owner.
	 *
	 * 故意把 datacl 设为默认值（NULL），而不是从模板数据库复制。
	 * 属主与模板属主不同时，复制 ACL 是坏主意。
	 */
	new_record_nulls[Anum_pg_database_datacl - 1] = true;

	tuple = heap_form_tuple(RelationGetDescr(pg_database_rel),
							new_record, new_record_nulls);

	CatalogTupleInsert(pg_database_rel, tuple);

	/*
	 * Now generate additional catalog entries associated with the new DB
	 *
	 * 现在生成与新数据库关联的其他目录项
	 */

	/* Register owner dependency */
	/*
	 *
	 * 登记属主依赖
	 */
	recordDependencyOnOwner(DatabaseRelationId, dboid, datdba);

	/* Create pg_shdepend entries for objects within database */
	/*
	 *
	 * 为数据库内的对象创建 pg_shdepend 项
	 */
	copyTemplateDependencies(src_dboid, dboid);

	/* Post creation hook for new database */
	/*
	 *
	 * 新数据库的创建后钩子
	 */
	InvokeObjectPostCreateHook(DatabaseRelationId, dboid, 0);

	/*
	 * If we're going to be reading data for the to-be-created database into
	 * shared_buffers, take a lock on it. Nobody should know that this
	 * database exists yet, but it's good to maintain the invariant that an
	 * AccessExclusiveLock on the database is sufficient to drop all of its
	 * buffers without worrying about more being read later.
	 *
	 * 若要把即将创建的数据库的数据读入 shared_buffers，先对它加锁。
	 * 此时还不应有人知道这个数据库存在，但最好维持这一不变量：
	 * 对数据库持有 AccessExclusiveLock 就足以丢弃其全部缓冲区，而不必担心之后又有页面被读入。
	 *
	 * Note that we need to do this before entering the
	 * PG_ENSURE_ERROR_CLEANUP block below, because createdb_failure_callback
	 * expects this lock to be held already.
	 *
	 * 注意必须在进入下面的 PG_ENSURE_ERROR_CLEANUP 块之前做这件事，
	 * 因为 createdb_failure_callback 假定该锁已经持有。
	 */
	if (dbstrategy == CREATEDB_WAL_LOG)
		LockSharedObject(DatabaseRelationId, dboid, 0, AccessShareLock);

	/*
	 * Once we start copying subdirectories, we need to be able to clean 'em
	 * up if we fail.  Use an ENSURE block to make sure this happens.  (This
	 * is not a 100% solution, because of the possibility of failure during
	 * transaction commit after we leave this routine, but it should handle
	 * most scenarios.)
	 *
	 * 一旦开始复制子目录，失败时必须能把它们清掉。用 ENSURE 块保证这一点。
	 * （这不是完全覆盖的方案，因为离开本函数后在事务提交期间仍可能失败，但应能处理大多数情形。）
	 */
	fparms.src_dboid = src_dboid;
	fparms.dest_dboid = dboid;
	fparms.strategy = dbstrategy;

	PG_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
							PointerGetDatum(&fparms));
	{
		/*
		 * If the user has asked to create a database with WAL_LOG strategy
		 * then call CreateDatabaseUsingWalLog, which will copy the database
		 * at the block level and it will WAL log each copied block.
		 * Otherwise, call CreateDatabaseUsingFileCopy that will copy the
		 * database file by file.
		 *
		 * 若用户要求用 WAL_LOG 策略创建数据库，则调用 CreateDatabaseUsingWalLog，
		 * 按块复制并为每个被复制的块写 WAL。否则调用 CreateDatabaseUsingFileCopy，按文件复制数据库。
		 */
		if (dbstrategy == CREATEDB_WAL_LOG)
			CreateDatabaseUsingWalLog(src_dboid, dboid, src_deftablespace,
									  dst_deftablespace);
		else
			CreateDatabaseUsingFileCopy(src_dboid, dboid, src_deftablespace,
										dst_deftablespace);

		/*
		 * Close pg_database, but keep lock till commit.
		 *
		 * 关闭 pg_database，但把锁保持到提交。
		 */
		table_close(pg_database_rel, NoLock);

		/*
		 * Force synchronous commit, thus minimizing the window between
		 * creation of the database files and committal of the transaction. If
		 * we crash before committing, we'll have a DB that's taking up disk
		 * space but is not in pg_database, which is not good.
		 *
		 * 强制同步提交，从而尽量缩小数据库文件已创建与事务已提交之间的窗口。
		 * 若提交前崩溃，会留下占磁盘空间但不在 pg_database 中的数据库，这不好。
		 */
		ForceSyncCommit();
	}
	PG_END_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
								PointerGetDatum(&fparms));

	return dboid;
}

/*
 * Check whether chosen encoding matches chosen locale settings.  This
 * restriction is necessary because libc's locale-specific code usually
 * fails when presented with data in an encoding it's not expecting. We
 * allow mismatch in four cases:
 *
 * 检查所选编码是否与所选 locale 设置匹配。这一限制是必要的，
 * 因为 libc 中特定于 locale 的代码在遇到非预期编码的数据时通常会失败。以下四种情况允许不匹配：
 *
 * 1. locale encoding = SQL_ASCII, which means that the locale is C/POSIX
 * which works with any encoding.
 *
 * 1. locale 编码为 SQL_ASCII，表示 locale 是 C/POSIX，可与任何编码配合。
 *
 * 2. locale encoding = -1, which means that we couldn't determine the
 * locale's encoding and have to trust the user to get it right.
 *
 * 2. locale 编码为 -1，表示无法确定 locale 的编码，只能信任用户设置正确。
 *
 * 3. selected encoding is UTF8 and platform is win32. This is because
 * UTF8 is a pseudo codepage that is supported in all locales since it's
 * converted to UTF16 before being used.
 *
 * 3. 所选编码为 UTF8 且平台为 win32。因为 UTF8 是伪代码页，在所有 locale 中都支持，使用前会转换成 UTF16。
 *
 * 4. selected encoding is SQL_ASCII, but only if you're a superuser. This
 * is risky but we have historically allowed it --- notably, the
 * regression tests require it.
 *
 * 4. 所选编码为 SQL_ASCII，但仅限超级用户。这有风险，但历史上一直允许，尤其是回归测试需要它。
 *
 * Note: if you change this policy, fix initdb to match.
 *
 * 注意：若更改此策略，请同步修改 initdb。
 */
void
check_encoding_locale_matches(int encoding, const char *collate, const char *ctype)
{
	int			ctype_encoding = pg_get_encoding_from_locale(ctype, true);
	int			collate_encoding = pg_get_encoding_from_locale(collate, true);

	if (!(ctype_encoding == encoding ||
		  ctype_encoding == PG_SQL_ASCII ||
		  ctype_encoding == -1 ||
#ifdef WIN32
		  encoding == PG_UTF8 ||
#endif
		  (encoding == PG_SQL_ASCII && superuser())))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding),
						ctype),
				 errdetail("The chosen LC_CTYPE setting requires encoding \"%s\".",
						   pg_encoding_to_char(ctype_encoding))));

	if (!(collate_encoding == encoding ||
		  collate_encoding == PG_SQL_ASCII ||
		  collate_encoding == -1 ||
#ifdef WIN32
		  encoding == PG_UTF8 ||
#endif
		  (encoding == PG_SQL_ASCII && superuser())))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding),
						collate),
				 errdetail("The chosen LC_COLLATE setting requires encoding \"%s\".",
						   pg_encoding_to_char(collate_encoding))));
}

/* Error cleanup callback for createdb */
/*
 *
 * createdb 的错误清理回调
 */
static void
createdb_failure_callback(int code, Datum arg)
{
	createdb_failure_params *fparms = (createdb_failure_params *) DatumGetPointer(arg);

	/*
	 * If we were copying database at block levels then drop pages for the
	 * destination database that are in the shared buffer cache.  And tell
	 * checkpointer to forget any pending fsync and unlink requests for files
	 * in the database.  The reasoning behind doing this is same as explained
	 * in dropdb function.  But unlike dropdb we don't need to call
	 * pgstat_drop_database because this database is still not created so
	 * there should not be any stat for this.
	 *
	 * 若当时在按块复制数据库，则丢弃共享缓冲区中目标数据库的页面，
	 * 并让 checkpointer 忘记该数据库文件上待处理的 fsync 与 unlink 请求。理由与 dropdb 中的说明相同。
	 * 但与 dropdb 不同，这里不必调用 pgstat_drop_database，因为数据库尚未创建，不应有统计信息。
	 */
	if (fparms->strategy == CREATEDB_WAL_LOG)
	{
		DropDatabaseBuffers(fparms->dest_dboid);
		ForgetDatabaseSyncRequests(fparms->dest_dboid);

		/* Release lock on the target database. */
		/*
		 *
		 * 释放目标数据库上的锁。
		 */
		UnlockSharedObject(DatabaseRelationId, fparms->dest_dboid, 0,
						   AccessShareLock);
	}

	/*
	 * Release lock on source database before doing recursive remove. This is
	 * not essential but it seems desirable to release the lock as soon as
	 * possible.
	 *
	 * 在递归删除之前释放源数据库上的锁。这不是必需的，但最好尽快释放。
	 */
	UnlockSharedObject(DatabaseRelationId, fparms->src_dboid, 0, ShareLock);

	/* Throw away any successfully copied subdirectories */
	/*
	 *
	 * 丢掉已成功复制的子目录
	 */
	remove_dbtablespaces(fparms->dest_dboid);
}


/*
 * DROP DATABASE
 *
 * DROP DATABASE 命令。
 */
void
dropdb(const char *dbname, bool missing_ok, bool force)
{
	Oid			db_id;
	bool		db_istemplate;
	Relation	pgdbrel;
	HeapTuple	tup;
	ScanKeyData scankey;
	void	   *inplace_state;
	Form_pg_database datform;
	int			notherbackends;
	int			npreparedxacts;
	int			nslots,
				nslots_active;
	int			nsubscriptions;

	/*
	 * Look up the target database's OID, and get exclusive lock on it. We
	 * need this to ensure that no new backend starts up in the target
	 * database while we are deleting it (see postinit.c), and that no one is
	 * using it as a CREATE DATABASE template or trying to delete it for
	 * themselves.
	 *
	 * 查找目标数据库的 OID 并对其加排他锁。这样在删除期间不会有新后端在目标库中启动（见 postinit.c），
	 * 也没有人把它当作 CREATE DATABASE 的模板或自行删除它。
	 */
	pgdbrel = table_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL,
					 &db_istemplate, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));
		}
		else
		{
			/* Close pg_database, release the lock, since we changed nothing */
			/*
			 *
			 * 关闭 pg_database 并释放锁，因为我们没有做任何修改
			 */
			table_close(pgdbrel, RowExclusiveLock);
			ereport(NOTICE,
					(errmsg("database \"%s\" does not exist, skipping",
							dbname)));
			return;
		}
	}

	/*
	 * Permission checks
	 *
	 * 权限检查
	 */
	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   dbname);

	/* DROP hook for the database being removed */
	/*
	 *
	 * 被删除数据库的 DROP 钩子
	 */
	InvokeObjectDropHook(DatabaseRelationId, db_id, 0);

	/*
	 * Disallow dropping a DB that is marked istemplate.  This is just to
	 * prevent people from accidentally dropping template0 or template1; they
	 * can do so if they're really determined ...
	 *
	 * 禁止删除标记为 istemplate 的数据库。这只是为了防止有人误删 template0 或 template1；
	 * 若真的下定决心，他们仍然可以删除。
	 */
	if (db_istemplate)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop a template database")));

	/* Obviously can't drop my own database */
	/*
	 *
	 * 显然不能删除自己当前连接的数据库
	 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot drop the currently open database")));

	/*
	 * Check whether there are active logical slots that refer to the
	 * to-be-dropped database. The database lock we are holding prevents the
	 * creation of new slots using the database or existing slots becoming
	 * active.
	 *
	 * 检查是否有活动的逻辑复制槽引用即将删除的数据库。
	 * 我们持有的数据库锁会阻止用该库创建新槽，也阻止已有槽变为活动。
	 */
	(void) ReplicationSlotsCountDBSlots(db_id, &nslots, &nslots_active);
	if (nslots_active)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is used by an active logical replication slot",
						dbname),
				 errdetail_plural("There is %d active slot.",
								  "There are %d active slots.",
								  nslots_active, nslots_active)));
	}

	/*
	 * Check if there are subscriptions defined in the target database.
	 *
	 * 检查目标数据库中是否定义了订阅。
	 *
	 * We can't drop them automatically because they might be holding
	 * resources in other databases/instances.
	 *
	 * 不能自动删除它们，因为它们可能在其他数据库或实例中持有资源。
	 */
	if ((nsubscriptions = CountDBSubscriptions(db_id)) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being used by logical replication subscription",
						dbname),
				 errdetail_plural("There is %d subscription.",
								  "There are %d subscriptions.",
								  nsubscriptions, nsubscriptions)));


	/*
	 * Attempt to terminate all existing connections to the target database if
	 * the user has requested to do so.
	 *
	 * 若用户要求，则尝试终止目标数据库上的全部现有连接。
	 */
	if (force)
		TerminateOtherDBBackends(db_id);

	/*
	 * Check for other backends in the target database.  (Because we hold the
	 * database lock, no new ones can start after this.)
	 *
	 * 检查目标数据库中是否还有其他后端。（因为我们持有数据库锁，此后不会有新后端启动。）
	 *
	 * As in CREATE DATABASE, check this after other error conditions.
	 *
	 * 与 CREATE DATABASE 一样，把这项检查放在其他错误条件之后。
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * Delete any comments or security labels associated with the database.
	 *
	 * 删除与该数据库关联的注释或安全标签。
	 */
	DeleteSharedComments(db_id, DatabaseRelationId);
	DeleteSharedSecurityLabel(db_id, DatabaseRelationId);

	/*
	 * Remove settings associated with this database
	 *
	 * 删除与此数据库关联的设置
	 */
	DropSetting(db_id, InvalidOid);

	/*
	 * Remove shared dependency references for the database.
	 *
	 * 删除该数据库的共享依赖引用。
	 */
	dropDatabaseDependencies(db_id);

	/*
	 * Tell the cumulative stats system to forget it immediately, too.
	 *
	 * 同时让累积统计系统立刻忘掉它。
	 */
	pgstat_drop_database(db_id);

	/*
	 * Except for the deletion of the catalog row, subsequent actions are not
	 * transactional (consider DropDatabaseBuffers() discarding modified
	 * buffers). But we might crash or get interrupted below. To prevent
	 * accesses to a database with invalid contents, mark the database as
	 * invalid using an in-place update.
	 *
	 * 除删除目录行之外，后续动作都不是事务性的（例如 DropDatabaseBuffers() 会丢弃已修改的缓冲区）。
	 * 但下面可能崩溃或被中断。为防止访问内容已无效的数据库，用原地更新把数据库标为无效。
	 *
	 * We need to flush the WAL before continuing, to guarantee the
	 * modification is durable before performing irreversible filesystem
	 * operations.
	 *
	 * 继续之前需要刷出 WAL，以保证在执行不可逆的文件系统操作之前，这一修改已经持久化。
	 */
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	systable_inplace_update_begin(pgdbrel, DatabaseNameIndexId, true,
								  NULL, 1, &scankey, &tup, &inplace_state);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", db_id);
	datform = (Form_pg_database) GETSTRUCT(tup);
	datform->datconnlimit = DATCONNLIMIT_INVALID_DB;
	systable_inplace_update_finish(inplace_state, tup);
	XLogFlush(XactLastRecEnd);

	/*
	 * Also delete the tuple - transactionally. If this transaction commits,
	 * the row will be gone, but if we fail, dropdb() can be invoked again.
	 *
	 * 同时以事务方式删除该元组。若本事务提交，行就消失；若失败，可以再次调用 dropdb()。
	 */
	CatalogTupleDelete(pgdbrel, &tup->t_self);
	heap_freetuple(tup);

	/*
	 * Drop db-specific replication slots.
	 *
	 * 删除该数据库专用的复制槽。
	 */
	ReplicationSlotsDropDBSlots(db_id);

	/*
	 * Drop pages for this database that are in the shared buffer cache. This
	 * is important to ensure that no remaining backend tries to write out a
	 * dirty buffer to the dead database later...
	 *
	 * 丢弃共享缓冲区中属于此数据库的页面。这很重要，以免残留的后端稍后把脏缓冲区写到已经删除的数据库。
	 */
	DropDatabaseBuffers(db_id);

	/*
	 * Tell checkpointer to forget any pending fsync and unlink requests for
	 * files in the database; else the fsyncs will fail at next checkpoint, or
	 * worse, it will delete files that belong to a newly created database
	 * with the same OID.
	 *
	 * 让 checkpointer 忘记该数据库文件上待处理的 fsync 与 unlink 请求；
	 * 否则下次检查点时 fsync 会失败，更糟的是会删掉后来以相同 OID 新建的数据库的文件。
	 */
	ForgetDatabaseSyncRequests(db_id);

	/*
	 * Force a checkpoint to make sure the checkpointer has received the
	 * message sent by ForgetDatabaseSyncRequests.
	 *
	 * 强制做一次检查点，确保 checkpointer 已收到 ForgetDatabaseSyncRequests 发出的消息。
	 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

	/* Close all smgr fds in all backends. */
	/*
	 *
	 * 关闭所有后端中的全部 smgr 文件描述符。
	 */
	WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

	/*
	 * Remove all tablespace subdirs belonging to the database.
	 *
	 * 删除属于该数据库的全部表空间子目录。
	 */
	remove_dbtablespaces(db_id);

	/*
	 * Close pg_database, but keep lock till commit.
	 *
	 * 关闭 pg_database，但把锁保持到提交。
	 */
	table_close(pgdbrel, NoLock);

	/*
	 * Force synchronous commit, thus minimizing the window between removal of
	 * the database files and committal of the transaction. If we crash before
	 * committing, we'll have a DB that's gone on disk but still there
	 * according to pg_database, which is not good.
	 *
	 * 强制同步提交，从而尽量缩小数据库文件已删除与事务已提交之间的窗口。
	 * 若提交前崩溃，磁盘上的数据库已经没了，但 pg_database 里还在，这不好。
	 */
	ForceSyncCommit();
}


/*
 * Rename database
 *
 * 重命名数据库
 */
ObjectAddress
RenameDatabase(const char *oldname, const char *newname)
{
	Oid			db_id;
	HeapTuple	newtup;
	ItemPointerData otid;
	Relation	rel;
	int			notherbackends;
	int			npreparedxacts;
	ObjectAddress address;

	/*
	 * Look up the target database's OID, and get exclusive lock on it. We
	 * need this for the same reasons as DROP DATABASE.
	 *
	 * 查找目标数据库的 OID 并对其加排他锁。原因与 DROP DATABASE 相同。
	 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(oldname, AccessExclusiveLock, &db_id, NULL, NULL, NULL,
					 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", oldname)));

	/* must be owner */
	/*
	 *
	 * 必须是属主
	 */
	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   oldname);

	/* must have createdb rights */
	/*
	 *
	 * 必须具有 createdb 权限
	 */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to rename database")));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for database names are violated.
	 *
	 * 若以相应开关编译，则在数据库名违反回归测试约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strstr(newname, "regression") == NULL)
		elog(WARNING, "databases created by regression test cases should have names including \"regression\"");
#endif

	/*
	 * Make sure the new name doesn't exist.  See notes for same error in
	 * CREATE DATABASE.
	 *
	 * 确保新名称不存在。同类错误的说明见 CREATE DATABASE。
	 */
	if (OidIsValid(get_database_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", newname)));

	/*
	 * XXX Client applications probably store the current database somewhere,
	 * so renaming it could cause confusion.  On the other hand, there may not
	 * be an actual problem besides a little confusion, so think about this
	 * and decide.
	 *
	 * XXX：客户端应用多半会在某处保存当前数据库名，重命名可能造成混淆。
	 * 另一方面，除了一点混乱之外也许并没有实际问题，需要再考虑后决定。
	 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current database cannot be renamed")));

	/*
	 * Make sure the database does not have active sessions.  This is the same
	 * concern as above, but applied to other sessions.
	 *
	 * 确保该数据库没有活动会话。顾虑与上面相同，但针对的是其他会话。
	 *
	 * As in CREATE DATABASE, check this after other error conditions.
	 *
	 * 与 CREATE DATABASE 一样，把这项检查放在其他错误条件之后。
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						oldname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/* rename */
	/*
	 *
	 * 重命名
	 */
	newtup = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(db_id));
	if (!HeapTupleIsValid(newtup))
		elog(ERROR, "cache lookup failed for database %u", db_id);
	otid = newtup->t_self;
	namestrcpy(&(((Form_pg_database) GETSTRUCT(newtup))->datname), newname);
	CatalogTupleUpdate(rel, &otid, newtup);
	UnlockTuple(rel, &otid, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	/*
	 * Close pg_database, but keep lock till commit.
	 *
	 * 关闭 pg_database，但把锁保持到提交。
	 */
	table_close(rel, NoLock);

	return address;
}


/*
 * ALTER DATABASE SET TABLESPACE
 *
 * ALTER DATABASE SET TABLESPACE 命令。
 */
static void
movedb(const char *dbname, const char *tblspcname)
{
	Oid			db_id;
	Relation	pgdbrel;
	int			notherbackends;
	int			npreparedxacts;
	HeapTuple	oldtuple,
				newtuple;
	Oid			src_tblspcoid,
				dst_tblspcoid;
	ScanKeyData scankey;
	SysScanDesc sysscan;
	AclResult	aclresult;
	char	   *src_dbpath;
	char	   *dst_dbpath;
	DIR		   *dstdir;
	struct dirent *xlde;
	movedb_failure_params fparms;

	/*
	 * Look up the target database's OID, and get exclusive lock on it. We
	 * need this to ensure that no new backend starts up in the database while
	 * we are moving it, and that no one is using it as a CREATE DATABASE
	 * template or trying to delete it.
	 *
	 * 查找目标数据库的 OID 并对其加排他锁。这样在迁移期间不会有新后端在该库中启动，
	 * 也没有人把它当作 CREATE DATABASE 的模板或试图删除它。
	 */
	pgdbrel = table_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL, NULL,
					 NULL, NULL, NULL, NULL, &src_tblspcoid, NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	/*
	 * We actually need a session lock, so that the lock will persist across
	 * the commit/restart below.  (We could almost get away with letting the
	 * lock be released at commit, except that someone could try to move
	 * relations of the DB back into the old directory while we rmtree() it.)
	 *
	 * 实际上需要会话级锁，以便锁在下面的提交与重新开始之后仍然保持。
	 * （几乎可以让锁在提交时释放，但有人可能在我们 rmtree() 旧目录时把该库的关系移回旧目录。）
	 */
	LockSharedObjectForSession(DatabaseRelationId, db_id, 0,
							   AccessExclusiveLock);

	/*
	 * Permission checks
	 *
	 * 权限检查
	 */
	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   dbname);

	/*
	 * Obviously can't move the tables of my own database
	 *
	 * 显然不能迁移自己当前连接的数据库中的表
	 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot change the tablespace of the currently open database")));

	/*
	 * Get tablespace's oid
	 *
	 * 取得表空间的 OID
	 */
	dst_tblspcoid = get_tablespace_oid(tblspcname, false);

	/*
	 * Permission checks
	 *
	 * 权限检查
	 */
	aclresult = object_aclcheck(TableSpaceRelationId, dst_tblspcoid, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLESPACE,
					   tblspcname);

	/*
	 * pg_global must never be the default tablespace
	 *
	 * pg_global 绝不能作为默认表空间
	 */
	if (dst_tblspcoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_global cannot be used as default tablespace")));

	/*
	 * No-op if same tablespace
	 *
	 * 表空间相同则无操作
	 */
	if (src_tblspcoid == dst_tblspcoid)
	{
		table_close(pgdbrel, NoLock);
		UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
									 AccessExclusiveLock);
		return;
	}

	/*
	 * Check for other backends in the target database.  (Because we hold the
	 * database lock, no new ones can start after this.)
	 *
	 * 检查目标数据库中是否还有其他后端。（因为我们持有数据库锁，此后不会有新后端启动。）
	 *
	 * As in CREATE DATABASE, check this after other error conditions.
	 *
	 * 与 CREATE DATABASE 一样，把这项检查放在其他错误条件之后。
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * Get old and new database paths
	 *
	 * 取得旧的和新的数据库路径
	 */
	src_dbpath = GetDatabasePath(db_id, src_tblspcoid);
	dst_dbpath = GetDatabasePath(db_id, dst_tblspcoid);

	/*
	 * Force a checkpoint before proceeding. This will force all dirty
	 * buffers, including those of unlogged tables, out to disk, to ensure
	 * source database is up-to-date on disk for the copy.
	 * FlushDatabaseBuffers() would suffice for that, but we also want to
	 * process any pending unlink requests. Otherwise, the check for existing
	 * files in the target directory might fail unnecessarily, not to mention
	 * that the copy might fail due to source files getting deleted under it.
	 * On Windows, this also ensures that background procs don't hold any open
	 * files, which would cause rmdir() to fail.
	 *
	 * 继续之前强制做一次检查点。这会把所有脏缓冲区（包括 unlogged 表的）刷到磁盘，保证源库在磁盘上是最新的。
	 * FlushDatabaseBuffers() 对此已经够用，但还要处理待处理的 unlink 请求。否则对目标目录中已有文件的检查可能不必要地失败，
	 * 复制也可能因源文件被删除而失败。在 Windows 上，这还能保证后台进程不持有打开的文件，否则 rmdir() 会失败。
	 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT
					  | CHECKPOINT_FLUSH_ALL);

	/* Close all smgr fds in all backends. */
	/*
	 *
	 * 关闭所有后端中的全部 smgr 文件描述符。
	 */
	WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

	/*
	 * Now drop all buffers holding data of the target database; they should
	 * no longer be dirty so DropDatabaseBuffers is safe.
	 *
	 * 现在丢弃持有目标数据库数据的全部缓冲区；它们不应再是脏的，因此 DropDatabaseBuffers 是安全的。
	 *
	 * It might seem that we could just let these buffers age out of shared
	 * buffers naturally, since they should not get referenced anymore.  The
	 * problem with that is that if the user later moves the database back to
	 * its original tablespace, any still-surviving buffers would appear to
	 * contain valid data again --- but they'd be missing any changes made in
	 * the database while it was in the new tablespace.  In any case, freeing
	 * buffers that should never be used again seems worth the cycles.
	 *
	 * 似乎可以让这些缓冲区自己从共享缓冲区中老化掉，因为它们不应再被引用。
	 * 问题在于，若用户稍后把数据库移回原来的表空间，仍然存活的缓冲区会再次看起来含有有效数据，
	 * 但会缺少数据库位于新表空间期间所做的修改。无论如何，释放再也不会使用的缓冲区是值得的。
	 *
	 * Note: it'd be sufficient to get rid of buffers matching db_id and
	 * src_tblspcoid, but bufmgr.c presently provides no API for that.
	 *
	 * 注意：丢掉匹配 db_id 与 src_tblspcoid 的缓冲区就够了，但 bufmgr.c 目前没有这样的 API。
	 */
	DropDatabaseBuffers(db_id);

	/*
	 * Check for existence of files in the target directory, i.e., objects of
	 * this database that are already in the target tablespace.  We can't
	 * allow the move in such a case, because we would need to change those
	 * relations' pg_class.reltablespace entries to zero, and we don't have
	 * access to the DB's pg_class to do so.
	 *
	 * 检查目标目录中是否已有文件，即此数据库中已经位于目标表空间的对象。
	 * 这种情况下不能迁移，因为需要把那些关系的 pg_class.reltablespace 改为零，而我们无法访问该库的 pg_class。
	 */
	dstdir = AllocateDir(dst_dbpath);
	if (dstdir != NULL)
	{
		while ((xlde = ReadDir(dstdir, dst_dbpath)) != NULL)
		{
			if (strcmp(xlde->d_name, ".") == 0 ||
				strcmp(xlde->d_name, "..") == 0)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("some relations of database \"%s\" are already in tablespace \"%s\"",
							dbname, tblspcname),
					 errhint("You must move them back to the database's default tablespace before using this command.")));
		}

		FreeDir(dstdir);

		/*
		 * The directory exists but is empty. We must remove it before using
		 * the copydir function.
		 *
		 * 目录存在但是空的。在使用 copydir 之前必须先删掉它。
		 */
		if (rmdir(dst_dbpath) != 0)
			elog(ERROR, "could not remove directory \"%s\": %m",
				 dst_dbpath);
	}

	/*
	 * Use an ENSURE block to make sure we remove the debris if the copy fails
	 * (eg, due to out-of-disk-space).  This is not a 100% solution, because
	 * of the possibility of failure during transaction commit, but it should
	 * handle most scenarios.
	 *
	 * 用 ENSURE 块保证复制失败时（例如磁盘空间不足）清掉残留。
	 * 这不是完全覆盖的方案，因为事务提交期间仍可能失败，但应能处理大多数情形。
	 */
	fparms.dest_dboid = db_id;
	fparms.dest_tsoid = dst_tblspcoid;
	PG_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
							PointerGetDatum(&fparms));
	{
		Datum		new_record[Natts_pg_database] = {0};
		bool		new_record_nulls[Natts_pg_database] = {0};
		bool		new_record_repl[Natts_pg_database] = {0};

		/*
		 * Copy files from the old tablespace to the new one
		 *
		 * 把文件从旧表空间复制到新表空间
		 */
		copydir(src_dbpath, dst_dbpath, false);

		/*
		 * Record the filesystem change in XLOG
		 *
		 * 把文件系统变更记入 XLOG
		 */
		{
			xl_dbase_create_file_copy_rec xlrec;

			xlrec.db_id = db_id;
			xlrec.tablespace_id = dst_tblspcoid;
			xlrec.src_db_id = db_id;
			xlrec.src_tablespace_id = src_tblspcoid;

			XLogBeginInsert();
			XLogRegisterData(&xlrec,
							 sizeof(xl_dbase_create_file_copy_rec));

			(void) XLogInsert(RM_DBASE_ID,
							  XLOG_DBASE_CREATE_FILE_COPY | XLR_SPECIAL_REL_UPDATE);
		}

		/*
		 * Update the database's pg_database tuple
		 *
		 * 更新该数据库的 pg_database 元组
		 */
		ScanKeyInit(&scankey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(dbname));
		sysscan = systable_beginscan(pgdbrel, DatabaseNameIndexId, true,
									 NULL, 1, &scankey);
		oldtuple = systable_getnext(sysscan);
		if (!HeapTupleIsValid(oldtuple))	/* shouldn't happen... */
		/*
		 *
		 * 不应该发生
		 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));
		LockTuple(pgdbrel, &oldtuple->t_self, InplaceUpdateTupleLock);

		new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_tblspcoid);
		new_record_repl[Anum_pg_database_dattablespace - 1] = true;

		newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(pgdbrel),
									 new_record,
									 new_record_nulls, new_record_repl);
		CatalogTupleUpdate(pgdbrel, &oldtuple->t_self, newtuple);
		UnlockTuple(pgdbrel, &oldtuple->t_self, InplaceUpdateTupleLock);

		InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

		systable_endscan(sysscan);

		/*
		 * Force another checkpoint here.  As in CREATE DATABASE, this is to
		 * ensure that we don't have to replay a committed
		 * XLOG_DBASE_CREATE_FILE_COPY operation, which would cause us to lose
		 * any unlogged operations done in the new DB tablespace before the
		 * next checkpoint.
		 *
		 * 这里再强制做一次检查点。与 CREATE DATABASE 一样，这是为了不必重放已提交的
		 * XLOG_DBASE_CREATE_FILE_COPY，否则会丢失下一次检查点之前在新数据库表空间中做的 unlogged 操作。
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

		/*
		 * Force synchronous commit, thus minimizing the window between
		 * copying the database files and committal of the transaction. If we
		 * crash before committing, we'll leave an orphaned set of files on
		 * disk, which is not fatal but not good either.
		 *
		 * 强制同步提交，从而尽量缩小数据库文件已复制与事务已提交之间的窗口。
		 * 若提交前崩溃，会在磁盘上留下一组孤立文件，虽不致命但也不好。
		 */
		ForceSyncCommit();

		/*
		 * Close pg_database, but keep lock till commit.
		 *
		 * 关闭 pg_database，但把锁保持到提交。
		 */
		table_close(pgdbrel, NoLock);
	}
	PG_END_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
								PointerGetDatum(&fparms));

	/*
	 * Commit the transaction so that the pg_database update is committed. If
	 * we crash while removing files, the database won't be corrupt, we'll
	 * just leave some orphaned files in the old directory.
	 *
	 * 提交事务，使 pg_database 的更新被提交。若删除文件时崩溃，数据库不会损坏，只是旧目录里留下一些孤立文件。
	 *
	 * (This is OK because we know we aren't inside a transaction block.)
	 *
	 * （这是可以的，因为已知当前不在事务块内。）
	 *
	 * XXX would it be safe/better to do this inside the ensure block?	Not
	 * convinced it's a good idea; consider elog just after the transaction
	 * really commits.
	 *
	 * XXX：把这段放进 ensure 块是否更安全或更好？并不认为这是好主意；
	 * 设想事务真正提交之后立刻 elog 的情况。
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Start new transaction for the remaining work; don't need a snapshot */
	/*
	 *
	 * 为剩余工作开始新事务；不需要快照
	 */
	StartTransactionCommand();

	/*
	 * Remove files from the old tablespace
	 *
	 * 删除旧表空间中的文件
	 */
	if (!rmtree(src_dbpath, true))
		ereport(WARNING,
				(errmsg("some useless files may be left behind in old database directory \"%s\"",
						src_dbpath)));

	/*
	 * Record the filesystem change in XLOG
	 *
	 * 把文件系统变更记入 XLOG
	 */
	{
		xl_dbase_drop_rec xlrec;

		xlrec.db_id = db_id;
		xlrec.ntablespaces = 1;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xl_dbase_drop_rec));
		XLogRegisterData(&src_tblspcoid, sizeof(Oid));

		(void) XLogInsert(RM_DBASE_ID,
						  XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE);
	}

	/* Now it's safe to release the database lock */
	/*
	 *
	 * 现在可以安全地释放数据库锁
	 */
	UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
								 AccessExclusiveLock);

	pfree(src_dbpath);
	pfree(dst_dbpath);
}

/* Error cleanup callback for movedb */
/*
 *
 * movedb 的错误清理回调
 */
static void
movedb_failure_callback(int code, Datum arg)
{
	movedb_failure_params *fparms = (movedb_failure_params *) DatumGetPointer(arg);
	char	   *dstpath;

	/* Get rid of anything we managed to copy to the target directory */
	/*
	 *
	 * 清掉已经复制到目标目录的任何内容
	 */
	dstpath = GetDatabasePath(fparms->dest_dboid, fparms->dest_tsoid);

	(void) rmtree(dstpath, true);

	pfree(dstpath);
}

/*
 * Process options and call dropdb function.
 *
 * 处理选项并调用 dropdb。
 */
void
DropDatabase(ParseState *pstate, DropdbStmt *stmt)
{
	bool		force = false;
	ListCell   *lc;

	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "force") == 0)
			force = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized %s option \"%s\"",
							"DROP DATABASE", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	dropdb(stmt->dbname, stmt->missing_ok, force);
}

/*
 * ALTER DATABASE name ...
 *
 * ALTER DATABASE name ... 命令。
 */
Oid
AlterDatabase(ParseState *pstate, AlterDatabaseStmt *stmt, bool isTopLevel)
{
	Relation	rel;
	Oid			dboid;
	HeapTuple	tuple,
				newtuple;
	Form_pg_database datform;
	ScanKeyData scankey;
	SysScanDesc scan;
	ListCell   *option;
	bool		dbistemplate = false;
	bool		dballowconnections = true;
	int			dbconnlimit = DATCONNLIMIT_UNLIMITED;
	DefElem    *distemplate = NULL;
	DefElem    *dallowconnections = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *dtablespace = NULL;
	Datum		new_record[Natts_pg_database] = {0};
	bool		new_record_nulls[Natts_pg_database] = {0};
	bool		new_record_repl[Natts_pg_database] = {0};

	/* Extract options from the statement node tree */
	/*
	 *
	 * 从语句节点树中提取选项
	 */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "is_template") == 0)
		{
			if (distemplate)
				errorConflictingDefElem(defel, pstate);
			distemplate = defel;
		}
		else if (strcmp(defel->defname, "allow_connections") == 0)
		{
			if (dallowconnections)
				errorConflictingDefElem(defel, pstate);
			dallowconnections = defel;
		}
		else if (strcmp(defel->defname, "connection_limit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "tablespace") == 0)
		{
			if (dtablespace)
				errorConflictingDefElem(defel, pstate);
			dtablespace = defel;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname),
					 parser_errposition(pstate, defel->location)));
	}

	if (dtablespace)
	{
		/*
		 * While the SET TABLESPACE syntax doesn't allow any other options,
		 * somebody could write "WITH TABLESPACE ...".  Forbid any other
		 * options from being specified in that case.
		 *
		 * SET TABLESPACE 语法不允许其他选项，但有人可能写成 WITH TABLESPACE ...。
		 * 那种情况下禁止再指定任何其他选项。
		 */
		if (list_length(stmt->options) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("option \"%s\" cannot be specified with other options",
							dtablespace->defname),
					 parser_errposition(pstate, dtablespace->location)));
		/* this case isn't allowed within a transaction block */
		/*
		 *
		 * 这种情况不允许出现在事务块内
		 */
		PreventInTransactionBlock(isTopLevel, "ALTER DATABASE SET TABLESPACE");
		movedb(stmt->dbname, defGetString(dtablespace));
		return InvalidOid;
	}

	if (distemplate && distemplate->arg)
		dbistemplate = defGetBoolean(distemplate);
	if (dallowconnections && dallowconnections->arg)
		dballowconnections = defGetBoolean(dallowconnections);
	if (dconnlimit && dconnlimit->arg)
	{
		dbconnlimit = defGetInt32(dconnlimit);
		if (dbconnlimit < DATCONNLIMIT_UNLIMITED)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", dbconnlimit)));
	}

	/*
	 * Get the old tuple.  We don't need a lock on the database per se,
	 * because we're not going to do anything that would mess up incoming
	 * connections.
	 *
	 * 取得旧元组。并不需要对数据库本身加锁，因为我们不会做任何会扰乱新进连接的事。
	 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));
	LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	datform = (Form_pg_database) GETSTRUCT(tuple);
	dboid = datform->oid;

	if (database_is_invalid_form(datform))
	{
		ereport(FATAL,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot alter invalid database \"%s\"", stmt->dbname),
				errhint("Use DROP DATABASE to drop invalid databases."));
	}

	if (!object_ownercheck(DatabaseRelationId, dboid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   stmt->dbname);

	/*
	 * In order to avoid getting locked out and having to go through
	 * standalone mode, we refuse to disallow connections to the database
	 * we're currently connected to.  Lockout can still happen with concurrent
	 * sessions but the likeliness of that is not high enough to worry about.
	 *
	 * 为避免把自己锁在外面而不得不走独立模式，拒绝禁止当前所连接数据库的连接。
	 * 并发会话仍可能导致锁定，但可能性不高，不必担心。
	 */
	if (!dballowconnections && dboid == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot disallow connections for current database")));

	/*
	 * Build an updated tuple, perusing the information just obtained
	 *
	 * 根据刚刚得到的信息构造更新后的元组
	 */
	if (distemplate)
	{
		new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
		new_record_repl[Anum_pg_database_datistemplate - 1] = true;
	}
	if (dallowconnections)
	{
		new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(dballowconnections);
		new_record_repl[Anum_pg_database_datallowconn - 1] = true;
	}
	if (dconnlimit)
	{
		new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
		new_record_repl[Anum_pg_database_datconnlimit - 1] = true;
	}

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), new_record,
								 new_record_nulls, new_record_repl);
	CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
	UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(DatabaseRelationId, dboid, 0);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	/*
	 *
	 * 关闭 pg_database，但把锁保持到提交
	 */
	table_close(rel, NoLock);

	return dboid;
}


/*
 * ALTER DATABASE name REFRESH COLLATION VERSION
 *
 * ALTER DATABASE name REFRESH COLLATION VERSION 命令。
 */
ObjectAddress
AlterDatabaseRefreshColl(AlterDatabaseRefreshCollStmt *stmt)
{
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Oid			db_id;
	HeapTuple	tuple;
	Form_pg_database datForm;
	ObjectAddress address;
	Datum		datum;
	bool		isnull;
	char	   *oldversion;
	char	   *newversion;

	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));

	datForm = (Form_pg_database) GETSTRUCT(tuple);
	db_id = datForm->oid;

	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   stmt->dbname);
	LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	datum = heap_getattr(tuple, Anum_pg_database_datcollversion, RelationGetDescr(rel), &isnull);
	oldversion = isnull ? NULL : TextDatumGetCString(datum);

	if (datForm->datlocprovider == COLLPROVIDER_LIBC)
	{
		datum = heap_getattr(tuple, Anum_pg_database_datcollate, RelationGetDescr(rel), &isnull);
		if (isnull)
			elog(ERROR, "unexpected null in pg_database");
	}
	else
	{
		datum = heap_getattr(tuple, Anum_pg_database_datlocale, RelationGetDescr(rel), &isnull);
		if (isnull)
			elog(ERROR, "unexpected null in pg_database");
	}

	newversion = get_collation_actual_version(datForm->datlocprovider,
											  TextDatumGetCString(datum));

	/* cannot change from NULL to non-NULL or vice versa */
	/*
	 *
	 * 不能从 NULL 改为非 NULL，也不能反过来
	 */
	if ((!oldversion && newversion) || (oldversion && !newversion))
		elog(ERROR, "invalid collation version change");
	else if (oldversion && newversion && strcmp(newversion, oldversion) != 0)
	{
		bool		nulls[Natts_pg_database] = {0};
		bool		replaces[Natts_pg_database] = {0};
		Datum		values[Natts_pg_database] = {0};
		HeapTuple	newtuple;

		ereport(NOTICE,
				(errmsg("changing version from %s to %s",
						oldversion, newversion)));

		values[Anum_pg_database_datcollversion - 1] = CStringGetTextDatum(newversion);
		replaces[Anum_pg_database_datcollversion - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
									 values, nulls, replaces);
		CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
		heap_freetuple(newtuple);
	}
	else
		ereport(NOTICE,
				(errmsg("version has not changed")));
	UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	systable_endscan(scan);

	table_close(rel, NoLock);

	return address;
}


/*
 * ALTER DATABASE name SET ...
 *
 * ALTER DATABASE name SET ... 命令。
 */
Oid
AlterDatabaseSet(AlterDatabaseSetStmt *stmt)
{
	Oid			datid = get_database_oid(stmt->dbname, false);

	/*
	 * Obtain a lock on the database and make sure it didn't go away in the
	 * meantime.
	 *
	 * 对数据库加锁，并确认它在此期间没有消失。
	 */
	shdepLockAndCheckObject(DatabaseRelationId, datid);

	if (!object_ownercheck(DatabaseRelationId, datid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   stmt->dbname);

	AlterSetting(datid, InvalidOid, stmt->setstmt);

	UnlockSharedObject(DatabaseRelationId, datid, 0, AccessShareLock);

	return datid;
}


/*
 * ALTER DATABASE name OWNER TO newowner
 *
 * ALTER DATABASE name OWNER TO newowner 命令。
 */
ObjectAddress
AlterDatabaseOwner(const char *dbname, Oid newOwnerId)
{
	Oid			db_id;
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Form_pg_database datForm;
	ObjectAddress address;

	/*
	 * Get the old tuple.  We don't need a lock on the database per se,
	 * because we're not going to do anything that would mess up incoming
	 * connections.
	 *
	 * 取得旧元组。并不需要对数据库本身加锁，因为我们不会做任何会扰乱新进连接的事。
	 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	datForm = (Form_pg_database) GETSTRUCT(tuple);
	db_id = datForm->oid;

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is to be consistent with other
	 * objects.
	 *
	 * 若新属主与现有属主相同，则认为命令已成功。这是为了与其他对象保持一致。
	 */
	if (datForm->datdba != newOwnerId)
	{
		Datum		repl_val[Natts_pg_database];
		bool		repl_null[Natts_pg_database] = {0};
		bool		repl_repl[Natts_pg_database] = {0};
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Otherwise, must be owner of the existing object */
		/*
		 *
		 * 否则必须是现有对象的属主
		 */
		if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
						   dbname);

		/* Must be able to become new owner */
		/*
		 *
		 * 必须能够成为新属主
		 */
		check_can_set_role(GetUserId(), newOwnerId);

		/*
		 * must have createdb rights
		 *
		 * 必须具有 createdb 权限
		 *
		 * NOTE: This is different from other alter-owner checks in that the
		 * current user is checked for createdb privileges instead of the
		 * destination owner.  This is consistent with the CREATE case for
		 * databases.  Because superusers will always have this right, we need
		 * no special case for them.
		 *
		 * 注意：这与其他更改属主的检查不同，这里检查的是当前用户而不是目标属主是否具有 createdb 权限。
		 * 这与数据库 CREATE 的情形一致。超级用户始终拥有该权限，因此不必为他们单列特例。
		 */
		if (!have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of database")));

		LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

		repl_repl[Anum_pg_database_datdba - 1] = true;
		repl_val[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 *
		 * 确定新属主对应的修改后 ACL。仅当 ACL 非空时才需要。
		 */
		aclDatum = heap_getattr(tuple,
								Anum_pg_database_datacl,
								RelationGetDescr(rel),
								&isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 datForm->datdba, newOwnerId);
			repl_repl[Anum_pg_database_datacl - 1] = true;
			repl_val[Anum_pg_database_datacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
		CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);
		UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		/*
		 *
		 * 更新属主依赖引用
		 */
		changeDependencyOnOwner(DatabaseRelationId, db_id, newOwnerId);
	}

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	/*
	 *
	 * 关闭 pg_database，但把锁保持到提交
	 */
	table_close(rel, NoLock);

	return address;
}


/*
 * 读取指定数据库 collation 提供者当前的实际版本；没有版本信息时返回 NULL。
 */
Datum
pg_database_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	HeapTuple	tp;
	char		datlocprovider;
	Datum		datum;
	char	   *version;

	tp = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("database with OID %u does not exist", dbid)));

	datlocprovider = ((Form_pg_database) GETSTRUCT(tp))->datlocprovider;

	if (datlocprovider == COLLPROVIDER_LIBC)
		datum = SysCacheGetAttrNotNull(DATABASEOID, tp, Anum_pg_database_datcollate);
	else
		datum = SysCacheGetAttrNotNull(DATABASEOID, tp, Anum_pg_database_datlocale);

	version = get_collation_actual_version(datlocprovider,
										   TextDatumGetCString(datum));

	ReleaseSysCache(tp);

	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/*
 * Helper functions
 *
 * 辅助函数
 */

/*
 * Look up info about the database named "name".  If the database exists,
 * obtain the specified lock type on it, fill in any of the remaining
 * parameters that aren't NULL, and return true.  If no such database,
 * return false.
 *
 * 查找名为 name 的数据库信息。若数据库存在，则按指定锁类型加锁，
 * 填充其余非 NULL 的参数，并返回 true。若不存在，则返回 false。
 */
static bool
get_db_info(const char *name, LOCKMODE lockmode,
			Oid *dbIdP, Oid *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP, bool *dbHasLoginEvtP,
			TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
			Oid *dbTablespace, char **dbCollate, char **dbCtype, char **dbLocale,
			char **dbIcurules,
			char *dbLocProvider,
			char **dbCollversion)
{
	bool		result = false;
	Relation	relation;

	Assert(name);

	/* Caller may wish to grab a better lock on pg_database beforehand... */
	/*
	 *
	 * 调用方可能希望事先在 pg_database 上取得更合适的锁。
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);

	/*
	 * Loop covers the rare case where the database is renamed before we can
	 * lock it.  We try again just in case we can find a new one of the same
	 * name.
	 *
	 * 循环覆盖一种少见情况：在我们加锁之前数据库被重命名。再试一次，以防还能找到同名的新数据库。
	 */
	for (;;)
	{
		ScanKeyData scanKey;
		SysScanDesc scan;
		HeapTuple	tuple;
		Oid			dbOid;

		/*
		 * there's no syscache for database-indexed-by-name, so must do it the
		 * hard way
		 *
		 * 没有按名称索引数据库的系统缓存，因此只能用较笨的办法
		 */
		ScanKeyInit(&scanKey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(name));

		scan = systable_beginscan(relation, DatabaseNameIndexId, true,
								  NULL, 1, &scanKey);

		tuple = systable_getnext(scan);

		if (!HeapTupleIsValid(tuple))
		{
			/* definitely no database of that name */
			/*
			 *
			 * 肯定没有这个名字的数据库
			 */
			systable_endscan(scan);
			break;
		}

		dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

		systable_endscan(scan);

		/*
		 * Now that we have a database OID, we can try to lock the DB.
		 *
		 * 现在已有数据库 OID，可以尝试对数据库加锁。
		 */
		if (lockmode != NoLock)
			LockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);

		/*
		 * And now, re-fetch the tuple by OID.  If it's still there and still
		 * the same name, we win; else, drop the lock and loop back to try
		 * again.
		 *
		 * 然后按 OID 重新读取元组。若它仍在且名称未变，则成功；否则放下锁并回到循环再试。
		 */
		tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbOid));
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

			if (strcmp(name, NameStr(dbform->datname)) == 0)
			{
				Datum		datum;
				bool		isnull;

				/* oid of the database */
				/*
				 *
				 * 数据库的 OID
				 */
				if (dbIdP)
					*dbIdP = dbOid;
				/* oid of the owner */
				/*
				 *
				 * 属主的 OID
				 */
				if (ownerIdP)
					*ownerIdP = dbform->datdba;
				/* character encoding */
				/*
				 *
				 * 字符编码
				 */
				if (encodingP)
					*encodingP = dbform->encoding;
				/* allowed as template? */
				/*
				 *
				 * 是否允许作为模板？
				 */
				if (dbIsTemplateP)
					*dbIsTemplateP = dbform->datistemplate;
				/* Has on login event trigger? */
				/*
				 *
				 * 是否有登录事件触发器？
				 */
				if (dbHasLoginEvtP)
					*dbHasLoginEvtP = dbform->dathasloginevt;
				/* allowing connections? */
				/*
				 *
				 * 是否允许连接？
				 */
				if (dbAllowConnP)
					*dbAllowConnP = dbform->datallowconn;
				/* limit of frozen XIDs */
				/*
				 *
				 * 冻结 XID 的界限
				 */
				if (dbFrozenXidP)
					*dbFrozenXidP = dbform->datfrozenxid;
				/* minimum MultiXactId */
				/*
				 *
				 * 最小 MultiXactId
				 */
				if (dbMinMultiP)
					*dbMinMultiP = dbform->datminmxid;
				/* default tablespace for this database */
				/*
				 *
				 * 此数据库的默认表空间
				 */
				if (dbTablespace)
					*dbTablespace = dbform->dattablespace;
				/* default locale settings for this database */
				/*
				 *
				 * 此数据库的默认 locale 设置
				 */
				if (dbLocProvider)
					*dbLocProvider = dbform->datlocprovider;
				if (dbCollate)
				{
					datum = SysCacheGetAttrNotNull(DATABASEOID, tuple, Anum_pg_database_datcollate);
					*dbCollate = TextDatumGetCString(datum);
				}
				if (dbCtype)
				{
					datum = SysCacheGetAttrNotNull(DATABASEOID, tuple, Anum_pg_database_datctype);
					*dbCtype = TextDatumGetCString(datum);
				}
				if (dbLocale)
				{
					datum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_datlocale, &isnull);
					if (isnull)
						*dbLocale = NULL;
					else
						*dbLocale = TextDatumGetCString(datum);
				}
				if (dbIcurules)
				{
					datum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_daticurules, &isnull);
					if (isnull)
						*dbIcurules = NULL;
					else
						*dbIcurules = TextDatumGetCString(datum);
				}
				if (dbCollversion)
				{
					datum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_datcollversion, &isnull);
					if (isnull)
						*dbCollversion = NULL;
					else
						*dbCollversion = TextDatumGetCString(datum);
				}
				ReleaseSysCache(tuple);
				result = true;
				break;
			}
			/* can only get here if it was just renamed */
			/*
			 *
			 * 只有刚刚被重命名时才会走到这里
			 */
			ReleaseSysCache(tuple);
		}

		if (lockmode != NoLock)
			UnlockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);
	}

	table_close(relation, AccessShareLock);

	return result;
}

/* Check if current user has createdb privileges */
/*
 *
 * 检查当前用户是否具有 createdb 权限
 */
bool
have_createdb_privilege(void)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers can always do everything */
	/*
	 *
	 * 超级用户始终可以做任何事
	 */
	if (superuser())
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetUserId()));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreatedb;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * Remove tablespace directories
 *
 * 删除表空间目录
 *
 * We don't know what tablespaces db_id is using, so iterate through all
 * tablespaces removing <tablespace>/db_id
 *
 * 不知道 db_id 使用哪些表空间，因此遍历所有表空间，删除其中名为 db_id 的子目录。
 */
static void
remove_dbtablespaces(Oid db_id)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *ltblspc = NIL;
	ListCell   *cell;
	int			ntblspc;
	int			i;
	Oid		   *tablespace_ids;

	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
		Oid			dsttablespace = spcform->oid;
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		/*
		 *
		 * 不要动全局表空间
		 */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (lstat(dstpath, &st) < 0 || !S_ISDIR(st.st_mode))
		{
			/* Assume we can ignore it */
			/*
			 *
			 * 假定可以忽略
			 */
			pfree(dstpath);
			continue;
		}

		if (!rmtree(dstpath, true))
			ereport(WARNING,
					(errmsg("some useless files may be left behind in old database directory \"%s\"",
							dstpath)));

		ltblspc = lappend_oid(ltblspc, dsttablespace);
		pfree(dstpath);
	}

	ntblspc = list_length(ltblspc);
	if (ntblspc == 0)
	{
		table_endscan(scan);
		table_close(rel, AccessShareLock);
		return;
	}

	tablespace_ids = (Oid *) palloc(ntblspc * sizeof(Oid));
	i = 0;
	foreach(cell, ltblspc)
		tablespace_ids[i++] = lfirst_oid(cell);

	/* Record the filesystem change in XLOG */
	/*
	 *
	 * 把文件系统变更记入 XLOG
	 */
	{
		xl_dbase_drop_rec xlrec;

		xlrec.db_id = db_id;
		xlrec.ntablespaces = ntblspc;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, MinSizeOfDbaseDropRec);
		XLogRegisterData(tablespace_ids, ntblspc * sizeof(Oid));

		(void) XLogInsert(RM_DBASE_ID,
						  XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE);
	}

	list_free(ltblspc);
	pfree(tablespace_ids);

	table_endscan(scan);
	table_close(rel, AccessShareLock);
}

/*
 * Check for existing files that conflict with a proposed new DB OID;
 * return true if there are any
 *
 * 检查是否已有文件与拟定的新数据库 OID 冲突；若有则返回 true
 *
 * If there were a subdirectory in any tablespace matching the proposed new
 * OID, we'd get a create failure due to the duplicate name ... and then we'd
 * try to remove that already-existing subdirectory during the cleanup in
 * remove_dbtablespaces.  Nuking existing files seems like a bad idea, so
 * instead we make this extra check before settling on the OID of the new
 * database.  This exactly parallels what GetNewRelFileNumber() does for table
 * relfilenumber values.
 *
 * 若任何表空间中已有与拟定新 OID 同名的子目录，创建会因重名失败，
 * 随后 remove_dbtablespaces 的清理又会试图删掉那个已经存在的子目录。毁掉已有文件不合适，
 * 因此在确定新数据库 OID 之前多做这一检查。这与 GetNewRelFileNumber() 为表的 relfilenumber 所做的完全对应。
 */
static bool
check_db_file_conflict(Oid db_id)
{
	bool		result = false;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;

	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
		Oid			dsttablespace = spcform->oid;
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		/*
		 *
		 * 不要动全局表空间
		 */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (lstat(dstpath, &st) == 0)
		{
			/* Found a conflicting file (or directory, whatever) */
			/*
			 *
			 * 发现冲突的文件或目录
			 */
			pfree(dstpath);
			result = true;
			break;
		}

		pfree(dstpath);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * Issue a suitable errdetail message for a busy database
 *
 * 为繁忙的数据库发出合适的 errdetail 信息
 */
static int
errdetail_busy_db(int notherbackends, int npreparedxacts)
{
	if (notherbackends > 0 && npreparedxacts > 0)

		/*
		 * We don't deal with singular versus plural here, since gettext
		 * doesn't support multiple plurals in one string.
		 *
		 * 这里不处理单数与复数，因为 gettext 不支持在一个字符串里使用多种复数形式。
		 */
		errdetail("There are %d other session(s) and %d prepared transaction(s) using the database.",
				  notherbackends, npreparedxacts);
	else if (notherbackends > 0)
		errdetail_plural("There is %d other session using the database.",
						 "There are %d other sessions using the database.",
						 notherbackends,
						 notherbackends);
	else
		errdetail_plural("There is %d prepared transaction using the database.",
						 "There are %d prepared transactions using the database.",
						 npreparedxacts,
						 npreparedxacts);
	return 0;					/* just to keep ereport macro happy */
	/*
	 *
	 * 只是为了让 ereport 宏满意
	 */
}

/*
 * get_database_oid - given a database name, look up the OID
 *
 * get_database_oid：给定数据库名，查找 OID
 *
 * If missing_ok is false, throw an error if database name not found.  If
 * true, just return InvalidOid.
 *
 * 若 missing_ok 为 false，数据库名不存在时抛出错误。若为 true，则只返回 InvalidOid。
 */
Oid
get_database_oid(const char *dbname, bool missing_ok)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	dbtuple;
	Oid			oid;

	/*
	 * There's no syscache for pg_database indexed by name, so we must look
	 * the hard way.
	 *
	 * pg_database 没有按名称索引的系统缓存，因此只能用较笨的办法查找。
	 */
	pg_database = table_open(DatabaseRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(pg_database, DatabaseNameIndexId, true,
							  NULL, 1, entry);

	dbtuple = systable_getnext(scan);

	/* We assume that there can be at most one matching tuple */
	/*
	 *
	 * 假定最多只有一条匹配的元组
	 */
	if (HeapTupleIsValid(dbtuple))
		oid = ((Form_pg_database) GETSTRUCT(dbtuple))->oid;
	else
		oid = InvalidOid;

	systable_endscan(scan);
	table_close(pg_database, AccessShareLock);

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist",
						dbname)));

	return oid;
}


/*
 * get_database_name - given a database OID, look up the name
 *
 * get_database_name：给定数据库 OID，查找名称
 *
 * Returns a palloc'd string, or NULL if no such database.
 *
 * 返回 palloc 分配的字符串；若没有该数据库则返回 NULL。
 */
char *
get_database_name(Oid dbid)
{
	HeapTuple	dbtuple;
	char	   *result;

	dbtuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (HeapTupleIsValid(dbtuple))
	{
		result = pstrdup(NameStr(((Form_pg_database) GETSTRUCT(dbtuple))->datname));
		ReleaseSysCache(dbtuple);
	}
	else
		result = NULL;

	return result;
}


/*
 * While dropping a database the pg_database row is marked invalid, but the
 * catalog contents still exist. Connections to such a database are not
 * allowed.
 *
 * 删除数据库时 pg_database 行会被标为无效，但目录内容仍然存在。不允许连接到这样的数据库。
 */
bool
database_is_invalid_form(Form_pg_database datform)
{
	return datform->datconnlimit == DATCONNLIMIT_INVALID_DB;
}


/*
 * Convenience wrapper around database_is_invalid_form()
 *
 * database_is_invalid_form() 的便捷包装
 */
bool
database_is_invalid_oid(Oid dboid)
{
	HeapTuple	dbtup;
	Form_pg_database dbform;
	bool		invalid;

	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dboid));
	if (!HeapTupleIsValid(dbtup))
		elog(ERROR, "cache lookup failed for database %u", dboid);
	dbform = (Form_pg_database) GETSTRUCT(dbtup);

	invalid = database_is_invalid_form(dbform);

	ReleaseSysCache(dbtup);

	return invalid;
}


/*
 * recovery_create_dbdir()
 *
 * 函数 recovery_create_dbdir()。
 *
 * During recovery, there's a case where we validly need to recover a missing
 * tablespace directory so that recovery can continue.  This happens when
 * recovery wants to create a database but the holding tablespace has been
 * removed before the server stopped.  Since we expect that the directory will
 * be gone before reaching recovery consistency, and we have no knowledge about
 * the tablespace other than its OID here, we create a real directory under
 * pg_tblspc here instead of restoring the symlink.
 *
 * 恢复期间有一种情况确实需要补回缺失的表空间目录，恢复才能继续。
 * 这发生在恢复要创建数据库、但承载它的表空间在服务器停止前已被删除时。
 * 预期在达到恢复一致性之前该目录会消失，而且这里除 OID 外对表空间一无所知，
 * 因此在 pg_tblspc 下创建一个真实目录，而不是恢复符号链接。
 *
 * If only_tblspc is true, then the requested directory must be in pg_tblspc/
 *
 * 若 only_tblspc 为真，则请求的目录必须位于 pg_tblspc/ 之下
 */
static void
recovery_create_dbdir(char *path, bool only_tblspc)
{
	struct stat st;

	Assert(RecoveryInProgress());

	if (stat(path, &st) == 0)
		return;

	if (only_tblspc && strstr(path, PG_TBLSPC_DIR_SLASH) == NULL)
		elog(PANIC, "requested to created invalid directory: %s", path);

	if (reachedConsistency && !allow_in_place_tablespaces)
		ereport(PANIC,
				errmsg("missing directory \"%s\"", path));

	elog(reachedConsistency ? WARNING : DEBUG1,
		 "creating missing directory: %s", path);

	if (pg_mkdir_p(path, pg_dir_create_mode) != 0)
		ereport(PANIC,
				errmsg("could not create missing directory \"%s\": %m", path));
}


/*
 * DATABASE resource manager's routines
 *
 * 数据库资源管理器的例程
 */
void
dbase_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in dbase records */
	/*
	 *
	 * dbase 记录不使用备份块
	 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_DBASE_CREATE_FILE_COPY)
	{
		xl_dbase_create_file_copy_rec *xlrec =
			(xl_dbase_create_file_copy_rec *) XLogRecGetData(record);
		char	   *src_path;
		char	   *dst_path;
		char	   *parent_path;
		struct stat st;

		src_path = GetDatabasePath(xlrec->src_db_id, xlrec->src_tablespace_id);
		dst_path = GetDatabasePath(xlrec->db_id, xlrec->tablespace_id);

		/*
		 * Our theory for replaying a CREATE is to forcibly drop the target
		 * subdirectory if present, then re-copy the source data. This may be
		 * more work than needed, but it is simple to implement.
		 *
		 * 重放 CREATE 的思路是：若目标子目录存在则强制删掉，然后重新复制源数据。
		 * 这可能比必要的工作更多，但实现简单。
		 */
		if (stat(dst_path, &st) == 0 && S_ISDIR(st.st_mode))
		{
			if (!rmtree(dst_path, true))
				/* If this failed, copydir() below is going to error. */
				/*
				 *
				 * 若这一步失败，下面的 copydir() 将会报错。
				 */
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								dst_path)));
		}

		/*
		 * If the parent of the target path doesn't exist, create it now. This
		 * enables us to create the target underneath later.
		 *
		 * 若目标路径的父目录不存在，现在就创建它。这样稍后才能在其下创建目标。
		 */
		parent_path = pstrdup(dst_path);
		get_parent_directory(parent_path);
		if (stat(parent_path, &st) < 0)
		{
			if (errno != ENOENT)
				ereport(FATAL,
						errmsg("could not stat directory \"%s\": %m",
							   dst_path));

			/* create the parent directory if needed and valid */
			/*
			 *
			 * 在需要且合法时创建父目录
			 */
			recovery_create_dbdir(parent_path, true);
		}
		pfree(parent_path);

		/*
		 * There's a case where the copy source directory is missing for the
		 * same reason above.  Create the empty source directory so that
		 * copydir below doesn't fail.  The directory will be dropped soon by
		 * recovery.
		 *
		 * 出于上面同样的原因，复制源目录也可能缺失。创建空的源目录，以免下面的 copydir 失败。
		 * 该目录很快会被恢复过程删掉。
		 */
		if (stat(src_path, &st) < 0 && errno == ENOENT)
			recovery_create_dbdir(src_path, false);

		/*
		 * Force dirty buffers out to disk, to ensure source database is
		 * up-to-date for the copy.
		 *
		 * 把脏缓冲区刷到磁盘，保证源数据库在复制时是最新的。
		 */
		FlushDatabaseBuffers(xlrec->src_db_id);

		/* Close all smgr fds in all backends. */
		/*
		 *
		 * 关闭所有后端中的全部 smgr 文件描述符。
		 */
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

		/*
		 * Copy this subdirectory to the new location
		 *
		 * 把该子目录复制到新位置
		 *
		 * We don't need to copy subdirectories
		 *
		 * 不需要复制子目录
		 */
		copydir(src_path, dst_path, false);

		pfree(src_path);
		pfree(dst_path);
	}
	else if (info == XLOG_DBASE_CREATE_WAL_LOG)
	{
		xl_dbase_create_wal_log_rec *xlrec =
			(xl_dbase_create_wal_log_rec *) XLogRecGetData(record);
		char	   *dbpath;
		char	   *parent_path;

		dbpath = GetDatabasePath(xlrec->db_id, xlrec->tablespace_id);

		/* create the parent directory if needed and valid */
		/*
		 *
		 * 在需要且合法时创建父目录
		 */
		parent_path = pstrdup(dbpath);
		get_parent_directory(parent_path);
		recovery_create_dbdir(parent_path, true);
		pfree(parent_path);

		/* Create the database directory with the version file. */
		/*
		 *
		 * 创建带版本文件的数据库目录。
		 */
		CreateDirAndVersionFile(dbpath, xlrec->db_id, xlrec->tablespace_id,
								true);
		pfree(dbpath);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) XLogRecGetData(record);
		char	   *dst_path;
		int			i;

		if (InHotStandby)
		{
			/*
			 * Lock database while we resolve conflicts to ensure that
			 * InitPostgres() cannot fully re-execute concurrently. This
			 * avoids backends re-connecting automatically to same database,
			 * which can happen in some cases.
			 *
			 * 在解决冲突期间锁定数据库，确保 InitPostgres() 不能同时完整地再次执行。
			 * 这避免后端在某些情况下自动重连到同一数据库。
			 *
			 * This will lock out walsenders trying to connect to db-specific
			 * slots for logical decoding too, so it's safe for us to drop
			 * slots.
			 *
			 * 这也会挡住试图连接数据库专用逻辑解码槽的 walsender，因此可以安全地删除这些槽。
			 */
			LockSharedObjectForSession(DatabaseRelationId, xlrec->db_id, 0, AccessExclusiveLock);
			ResolveRecoveryConflictWithDatabase(xlrec->db_id);
		}

		/* Drop any database-specific replication slots */
		/*
		 *
		 * 删除所有数据库专用的复制槽
		 */
		ReplicationSlotsDropDBSlots(xlrec->db_id);

		/* Drop pages for this database that are in the shared buffer cache */
		/*
		 *
		 * 丢弃共享缓冲区中属于此数据库的页面
		 */
		DropDatabaseBuffers(xlrec->db_id);

		/* Also, clean out any fsync requests that might be pending in md.c */
		/*
		 *
		 * 同时清掉 md.c 中可能待处理的 fsync 请求
		 */
		ForgetDatabaseSyncRequests(xlrec->db_id);

		/* Clean out the xlog relcache too */
		/*
		 *
		 * 也清掉 xlog relcache
		 */
		XLogDropDatabase(xlrec->db_id);

		/* Close all smgr fds in all backends. */
		/*
		 *
		 * 关闭所有后端中的全部 smgr 文件描述符。
		 */
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

		for (i = 0; i < xlrec->ntablespaces; i++)
		{
			dst_path = GetDatabasePath(xlrec->db_id, xlrec->tablespace_ids[i]);

			/* And remove the physical files */
			/*
			 *
			 * 并删除物理文件
			 */
			if (!rmtree(dst_path, true))
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								dst_path)));
			pfree(dst_path);
		}

		if (InHotStandby)
		{
			/*
			 * Release locks prior to commit. XXX There is a race condition
			 * here that may allow backends to reconnect, but the window for
			 * this is small because the gap between here and commit is mostly
			 * fairly small and it is unlikely that people will be dropping
			 * databases that we are trying to connect to anyway.
			 *
			 * 提交前释放锁。XXX：这里有竞态，可能允许后端重连，但窗口很小，
			 * 因为此处到提交之间的间隔大多相当短，而且人们不太会删除我们正要连接的数据库。
			 */
			UnlockSharedObjectForSession(DatabaseRelationId, xlrec->db_id, 0, AccessExclusiveLock);
		}
	}
	else
		elog(PANIC, "dbase_redo: unknown op code %u", info);
}
