/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  Commands to manipulate table spaces
 *
 * 操纵表空间的命令。
 *
 * Tablespaces in PostgreSQL are designed to allow users to determine
 * where the data file(s) for a given database object reside on the file
 * system.
 *
 * PostgreSQL 的表空间让用户决定某个数据库对象的数据文件放在文件系统的何处。
 *
 * A tablespace represents a directory on the file system. At tablespace
 * creation time, the directory must be empty. To simplify things and
 * remove the possibility of having file name conflicts, we isolate
 * files within a tablespace into database-specific subdirectories.
 *
 * 表空间对应文件系统上的一个目录。创建时该目录必须为空。
 * 为避免文件名冲突，表空间内的文件按数据库放到各自的子目录中。
 *
 * To support file access via the information given in RelFileLocator, we
 * maintain a symbolic-link map in $PGDATA/pg_tblspc. The symlinks are
 * named by tablespace OIDs and point to the actual tablespace directories.
 * There is also a per-cluster version directory in each tablespace.
 * Thus the full path to an arbitrary file is
 *			$PGDATA/pg_tblspc/spcoid/PG_MAJORVER_CATVER/dboid/relfilenumber
 * e.g.
 *			$PGDATA/pg_tblspc/20981/PG_9.0_201002161/719849/83292814
 *
 * 为按 RelFileLocator 访问文件，在 $PGDATA/pg_tblspc 中维护符号链接映射。
 * 链接以表空间 OID 命名，指向实际目录。每个表空间还有按集群版本划分的目录。
 * 任意文件的完整路径为
 * $PGDATA/pg_tblspc/spcoid/PG_MAJORVER_CATVER/dboid/relfilenumber
 * 例如
 * $PGDATA/pg_tblspc/20981/PG_9.0_201002161/719849/83292814
 *
 * There are two tablespaces created at initdb time: pg_global (for shared
 * tables) and pg_default (for everything else).  For backwards compatibility
 * and to remain functional on platforms without symlinks, these tablespaces
 * are accessed specially: they are respectively
 *			$PGDATA/global/relfilenumber
 *			$PGDATA/base/dboid/relfilenumber
 *
 * initdb 时创建两个表空间：pg_global（共享表）和 pg_default（其余对象）。
 * 为兼容且在没有符号链接的平台上仍能工作，它们的访问路径分别是
 * $PGDATA/global/relfilenumber
 * $PGDATA/base/dboid/relfilenumber
 *
 * To allow CREATE DATABASE to give a new database a default tablespace
 * that's different from the template database's default, we make the
 * provision that a zero in pg_class.reltablespace means the database's
 * default tablespace.  Without this, CREATE DATABASE would have to go in
 * and munge the system catalogs of the new database.
 *
 * 为让 CREATE DATABASE 给新库指定与模板库不同的默认表空间，
 * 约定 pg_class.reltablespace 为 0 表示使用数据库的默认表空间。
 * 否则 CREATE DATABASE 必须去改新库的系统目录。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/standby.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"

/*
 * 核心流程概览：
 * CreateTableSpace 校验超级用户、路径与名称，写入 pg_tablespace，
 * 再由 create_tablespace_directories 建立目录并把 $PGDATA/pg_tblspc 符号链接过去。
 * DropTableSpace 确认表空间为空后删除目录项与物理目录；RenameTableSpace 与
 * AlterTableSpaceOptions 修改名称或选项。
 * GetDefaultTablespace / PrepareTempTablespaces 解析 default_tablespace 与 temp_tablespaces。
 * tblspc_redo 在 WAL 重放时重建或删除表空间目录。
 */
/* GUC variables */
/*
 *
 * GUC 变量。
 */
char	   *default_tablespace = NULL;
char	   *temp_tablespaces = NULL;
bool		allow_in_place_tablespaces = false;

Oid			binary_upgrade_next_pg_tablespace_oid = InvalidOid;

static void create_tablespace_directories(const char *location,
										  const Oid tablespaceoid);
static bool destroy_tablespace_directories(Oid tablespaceoid, bool redo);


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, fall through quietly.
 *
 * 使用某表空间的每个数据库用以其 OID 命名的子目录隔离。
 * 在该表空间中首次创建对象时建立该子目录；若已存在则静默继续。
 *
 * isRedo indicates that we are creating an object during WAL replay.
 * In this case we will cope with the possibility of the tablespace
 * directory not being there either --- this could happen if we are
 * replaying an operation on a table in a subsequently-dropped tablespace.
 * We handle this by making a directory in the place where the tablespace
 * symlink would normally be.  This isn't an exact replay of course, but
 * it's the best we can do given the available information.
 *
 * isRedo 表示正在 WAL 重放期间创建对象。此时表空间目录也可能不存在——
 * 例如重放的是随后被删除的表空间中的表操作。
 * 处理办法是在通常放置表空间符号链接的位置建一个目录。这不是精确重放，但是现有信息下能做的最好结果。
 *
 * If tablespaces are not supported, we still need it in case we have to
 * re-create a database subdirectory (of $PGDATA/base) during WAL replay.
 *
 * 即使不支持表空间，WAL 重放时仍可能需要重建 $PGDATA/base 下的数据库子目录。
 */
void
TablespaceCreateDbspace(Oid spcOid, Oid dbOid, bool isRedo)
{
	struct stat st;
	char	   *dir;

	/*
	 * The global tablespace doesn't have per-database subdirectories, so
	 * nothing to do for it.
	 *
	 * 全局表空间没有按数据库划分的子目录，因此无需处理。
	 */
	if (spcOid == GLOBALTABLESPACE_OID)
		return;

	Assert(OidIsValid(spcOid));
	Assert(OidIsValid(dbOid));

	dir = GetDatabasePath(dbOid, spcOid);

	if (stat(dir, &st) < 0)
	{
		/* Directory does not exist? */
		/*
		 *
		 * 目录不存在？
		 */
		if (errno == ENOENT)
		{
			/*
			 * Acquire TablespaceCreateLock to ensure that no DROP TABLESPACE
			 * or TablespaceCreateDbspace is running concurrently.
			 *
			 * 获取 TablespaceCreateLock，确保没有并发的 DROP TABLESPACE 或 TablespaceCreateDbspace。
			 */
			LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

			/*
			 * Recheck to see if someone created the directory while we were
			 * waiting for lock.
			 *
			 * 等锁期间可能已有人创建了目录，再检查一次。
			 */
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
			{
				/* Directory was created */
				/*
				 *
				 * 目录已创建。
				 */
			}
			else
			{
				/* Directory creation failed? */
				/*
				 *
				 * 创建目录失败？
				 */
				if (MakePGDirectory(dir) < 0)
				{
					/* Failure other than not exists or not in WAL replay? */
					/*
					 *
					 * 失败原因既不是不存在，也不是处于 WAL 重放？
					 */
					if (errno != ENOENT || !isRedo)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));

					/*
					 * During WAL replay, it's conceivable that several levels
					 * of directories are missing if tablespaces are dropped
					 * further ahead of the WAL stream than we're currently
					 * replaying.  An easy way forward is to create them as
					 * plain directories and hope they are removed by further
					 * WAL replay if necessary.  If this also fails, there is
					 * trouble we cannot get out of, so just report that and
					 * bail out.
					 *
					 * WAL 重放时，若表空间在我们尚未重放到的记录中已被删除，可能缺多层目录。
					 * 简单做法是把它们建成普通目录，指望后续重放在需要时删掉。
					 * 若这一步也失败，就无法继续，只能报错退出。
					 */
					if (pg_mkdir_p(dir, pg_dir_create_mode) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));
				}
			}

			LWLockRelease(TablespaceCreateLock);
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", dir)));
		}
	}
	else
	{
		/* Is it not a directory? */
		/*
		 *
		 * 它不是目录？
		 */
		if (!S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" exists but is not a directory",
							dir)));
	}

	pfree(dir);
}

/*
 * Create a table space
 *
 * 创建表空间。
 *
 * Only superusers can create a tablespace. This seems a reasonable restriction
 * since we're determining the system layout and, anyway, we probably have
 * root if we're doing this kind of activity
 *
 * 只有超级用户能创建表空间。这是合理限制：我们在决定系统布局，
 * 做这种操作时通常也已经有 root 权限。
 */
Oid
CreateTableSpace(CreateTableSpaceStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_tablespace];
	bool		nulls[Natts_pg_tablespace] = {0};
	HeapTuple	tuple;
	Oid			tablespaceoid;
	char	   *location;
	Oid			ownerId;
	Datum		newOptions;
	bool		in_place;

	/* Must be superuser */
	/*
	 *
	 * 必须是超级用户。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create tablespace \"%s\"",
						stmt->tablespacename),
				 errhint("Must be superuser to create a tablespace.")));

	/* However, the eventual owner of the tablespace need not be */
	/*
	 *
	 * 不过表空间的最终属主不必是超级用户。
	 */
	if (stmt->owner)
		ownerId = get_rolespec_oid(stmt->owner, false);
	else
		ownerId = GetUserId();

	/* Unix-ify the offered path, and strip any trailing slashes */
	/*
	 *
	 * 把给出的路径转成 Unix 形式，并去掉末尾斜杠。
	 */
	location = pstrdup(stmt->location);
	canonicalize_path(location);

	/* disallow quotes, else CREATE DATABASE would be at risk */
	/*
	 *
	 * 不允许引号，否则 CREATE DATABASE 会有风险。
	 */
	if (strchr(location, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("tablespace location cannot contain single quotes")));

	in_place = allow_in_place_tablespaces && strlen(location) == 0;

	/*
	 * Allowing relative paths seems risky
	 *
	 * 允许相对路径看起来有风险。
	 *
	 * This also helps us ensure that location is not empty or whitespace,
	 * unless specifying a developer-only in-place tablespace.
	 *
	 * 这也用来确保 location 不是空的或纯空白，除非指定了仅供开发使用的就地表空间。
	 */
	if (!in_place && !is_absolute_path(location))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location must be an absolute path")));

	/*
	 * Check that location isn't too long. Remember that we're going to append
	 * 'PG_XXX/<dboid>/<relid>_<fork>.<nnn>'.  FYI, we never actually
	 * reference the whole path here, but MakePGDirectory() uses the first two
	 * parts.
	 *
	 * 检查 location 不会太长。后面还会追加 PG_XXX/<dboid>/<relid>_<fork>.<nnn>。
	 * 这里并不引用完整路径，但 MakePGDirectory() 会用到前两段。
	 */
	if (strlen(location) + 1 + strlen(TABLESPACE_VERSION_DIRECTORY) + 1 +
		OIDCHARS + 1 + OIDCHARS + 1 + FORKNAMECHARS + 1 + OIDCHARS > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location \"%s\" is too long",
						location)));

	/* Warn if the tablespace is in the data directory. */
	/*
	 *
	 * 若表空间位于数据目录内则发出警告。
	 */
	if (path_is_prefix_of_path(DataDir, location))
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location should not be inside the data directory")));

	/*
	 * Disallow creation of tablespaces named "pg_xxx"; we reserve this
	 * namespace for system purposes.
	 *
	 * 禁止创建名为 pg_xxx 的表空间；该命名空间保留给系统使用。
	 */
	if (!allowSystemTableMods && IsReservedName(stmt->tablespacename))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"",
						stmt->tablespacename),
				 errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for tablespace names are violated.
	 *
	 * 若以相应开关编译，则在表空间名违反回归测试约定时告警。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->tablespacename, "regress_", 8) != 0)
		elog(WARNING, "tablespaces created by regression test cases should have names starting with \"regress_\"");
#endif

	/*
	 * Check that there is no other tablespace by this name.  (The unique
	 * index would catch this anyway, but might as well give a friendlier
	 * message.)
	 *
	 * 检查没有其他同名表空间。（唯一索引反正会拦住，但这里给出更友好的信息。）
	 */
	if (OidIsValid(get_tablespace_oid(stmt->tablespacename, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						stmt->tablespacename)));

	/*
	 * Insert tuple into pg_tablespace.  The purpose of doing this first is to
	 * lock the proposed tablename against other would-be creators. The
	 * insertion will roll back if we find problems below.
	 *
	 * 先向 pg_tablespace 插入元组，以便锁住拟用的名字，挡住其他创建者。
	 * 若下面发现问题，这次插入会回滚。
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	if (IsBinaryUpgrade)
	{
		/* Use binary-upgrade override for tablespace oid */
		/*
		 *
		 * 二进制升级时使用覆盖的表空间 OID。
		 */
		if (!OidIsValid(binary_upgrade_next_pg_tablespace_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_tablespace OID value not set when in binary upgrade mode")));

		tablespaceoid = binary_upgrade_next_pg_tablespace_oid;
		binary_upgrade_next_pg_tablespace_oid = InvalidOid;
	}
	else
		tablespaceoid = GetNewOidWithIndex(rel, TablespaceOidIndexId,
										   Anum_pg_tablespace_oid);
	values[Anum_pg_tablespace_oid - 1] = ObjectIdGetDatum(tablespaceoid);
	values[Anum_pg_tablespace_spcname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablespacename));
	values[Anum_pg_tablespace_spcowner - 1] =
		ObjectIdGetDatum(ownerId);
	nulls[Anum_pg_tablespace_spcacl - 1] = true;

	/* Generate new proposed spcoptions (text array) */
	/*
	 *
	 * 生成新的 spcoptions（text 数组）。
	 */
	newOptions = transformRelOptions((Datum) 0,
									 stmt->options,
									 NULL, NULL, false, false);
	(void) tablespace_reloptions(newOptions, true);
	if (newOptions != (Datum) 0)
		values[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		nulls[Anum_pg_tablespace_spcoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* Record dependency on owner */
	/*
	 *
	 * 记录对属主的依赖。
	 */
	recordDependencyOnOwner(TableSpaceRelationId, tablespaceoid, ownerId);

	/* Post creation hook for new tablespace */
	/*
	 *
	 * 新建表空间的创建后钩子。
	 */
	InvokeObjectPostCreateHook(TableSpaceRelationId, tablespaceoid, 0);

	create_tablespace_directories(location, tablespaceoid);

	/* Record the filesystem change in XLOG */
	/*
	 *
	 * 把文件系统变更记入 XLOG。
	 */
	{
		xl_tblspc_create_rec xlrec;

		xlrec.ts_id = tablespaceoid;

		XLogBeginInsert();
		XLogRegisterData(&xlrec,
						 offsetof(xl_tblspc_create_rec, ts_path));
		XLogRegisterData(location, strlen(location) + 1);

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_CREATE);
	}

	/*
	 * Force synchronous commit, to minimize the window between creating the
	 * symlink on-disk and marking the transaction committed.  It's not great
	 * that there is any window at all, but definitely we don't want to make
	 * it larger than necessary.
	 *
	 * 强制同步提交，以缩小“磁盘上已创建符号链接”与“事务标记为已提交”之间的窗口。
	 * 窗口本身并不理想，但绝不能比必要的更大。
	 */
	ForceSyncCommit();

	pfree(location);

	/* We keep the lock on pg_tablespace until commit */
	/*
	 *
	 * 对 pg_tablespace 的锁保持到提交。
	 */
	table_close(rel, NoLock);

	return tablespaceoid;
}

/*
 * Drop a table space
 *
 * 删除表空间。
 *
 * Be careful to check that the tablespace is empty.
 *
 * 务必检查表空间是否为空。
 */
void
DropTableSpace(DropTableSpaceStmt *stmt)
{
	char	   *tablespacename = stmt->tablespacename;
	TableScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_tablespace spcform;
	ScanKeyData entry[1];
	Oid			tablespaceoid;
	char	   *detail;
	char	   *detail_log;

	/*
	 * Find the target tuple
	 *
	 * 找到目标元组。
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		if (!stmt->missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tablespacename)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("tablespace \"%s\" does not exist, skipping",
							tablespacename)));
			table_endscan(scandesc);
			table_close(rel, NoLock);
		}
		return;
	}

	spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
	tablespaceoid = spcform->oid;

	/* Must be tablespace owner */
	/*
	 *
	 * 必须是表空间属主。
	 */
	if (!object_ownercheck(TableSpaceRelationId, tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   tablespacename);

	/* Disallow drop of the standard tablespaces, even by superuser */
	/*
	 *
	 * 即使超级用户也不能删除标准表空间。
	 */
	if (IsPinnedObject(TableSpaceRelationId, tablespaceoid))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE,
					   tablespacename);

	/* Check for pg_shdepend entries depending on this tablespace */
	/*
	 *
	 * 检查是否有 pg_shdepend 项依赖此表空间。
	 */
	if (checkSharedDependencies(TableSpaceRelationId, tablespaceoid,
								&detail, &detail_log))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("tablespace \"%s\" cannot be dropped because some objects depend on it",
						tablespacename),
				 errdetail_internal("%s", detail),
				 errdetail_log("%s", detail_log)));

	/* DROP hook for the tablespace being removed */
	/*
	 *
	 * 被删除表空间的 DROP 钩子。
	 */
	InvokeObjectDropHook(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * Remove the pg_tablespace tuple (this will roll back if we fail below)
	 *
	 * 删除 pg_tablespace 元组（若下面失败会回滚）。
	 */
	CatalogTupleDelete(rel, &tuple->t_self);

	table_endscan(scandesc);

	/*
	 * Remove any comments or security labels on this tablespace.
	 *
	 * 删除此表空间上的注释或安全标签。
	 */
	DeleteSharedComments(tablespaceoid, TableSpaceRelationId);
	DeleteSharedSecurityLabel(tablespaceoid, TableSpaceRelationId);

	/*
	 * Remove dependency on owner.
	 *
	 * 删除对属主的依赖。
	 */
	deleteSharedDependencyRecordsFor(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * Acquire TablespaceCreateLock to ensure that no TablespaceCreateDbspace
	 * is running concurrently.
	 *
	 * 获取 TablespaceCreateLock，确保没有并发的 TablespaceCreateDbspace。
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/*
	 * Try to remove the physical infrastructure.
	 *
	 * 尝试删除物理基础设施。
	 */
	if (!destroy_tablespace_directories(tablespaceoid, false))
	{
		/*
		 * Not all files deleted?  However, there can be lingering empty files
		 * in the directories, left behind by for example DROP TABLE, that
		 * have been scheduled for deletion at next checkpoint (see comments
		 * in mdunlink() for details).  We could just delete them immediately,
		 * but we can't tell them apart from important data files that we
		 * mustn't delete.  So instead, we force a checkpoint which will clean
		 * out any lingering files, and try again.
		 *
		 * 没有删光所有文件？目录里可能残留空文件，例如 DROP TABLE 留下、
		 * 计划在下次检查点删除的文件（详见 mdunlink() 的注释）。
		 * 不能立刻删，因为无法把它们和不能删的重要数据文件区分开。
		 * 因此强制一次检查点清掉残留文件，然后再试。
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

		/*
		 * On Windows, an unlinked file persists in the directory listing
		 * until no process retains an open handle for the file.  The DDL
		 * commands that schedule files for unlink send invalidation messages
		 * directing other PostgreSQL processes to close the files, but
		 * nothing guarantees they'll be processed in time.  So, we'll also
		 * use a global barrier to ask all backends to close all files, and
		 * wait until they're finished.
		 *
		 * 在 Windows 上，已 unlink 的文件在仍有进程持有打开句柄时会留在目录列表中。
		 * 安排 unlink 的 DDL 会发失效消息让其他进程关闭文件，但不保证及时处理。
		 * 因此再用全局 barrier 要求所有后端关闭全部文件，并等待完成。
		 */
		LWLockRelease(TablespaceCreateLock);
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));
		LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

		/* And now try again. */
		/*
		 *
		 * 然后再试一次。
		 */
		if (!destroy_tablespace_directories(tablespaceoid, false))
		{
			/* Still not empty, the files must be important then */
			/*
			 *
			 * 仍然非空，这些文件就应当是重要的。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace \"%s\" is not empty",
							tablespacename)));
		}
	}

	/* Record the filesystem change in XLOG */
	/*
	 *
	 * 把文件系统变更记入 XLOG。
	 */
	{
		xl_tblspc_drop_rec xlrec;

		xlrec.ts_id = tablespaceoid;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, sizeof(xl_tblspc_drop_rec));

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_DROP);
	}

	/*
	 * Note: because we checked that the tablespace was empty, there should be
	 * no need to worry about flushing shared buffers or free space map
	 * entries for relations in the tablespace.
	 *
	 * 注意：已经确认表空间为空，因此不必担心刷新其中关系的共享缓冲区或空闲空间映射项。
	 */

	/*
	 * Force synchronous commit, to minimize the window between removing the
	 * files on-disk and marking the transaction committed.  It's not great
	 * that there is any window at all, but definitely we don't want to make
	 * it larger than necessary.
	 *
	 * 强制同步提交，以缩小“磁盘上文件已删除”与“事务标记为已提交”之间的窗口。
	 * 窗口本身并不理想，但绝不能比必要的更大。
	 */
	ForceSyncCommit();

	/*
	 * Allow TablespaceCreateDbspace again.
	 *
	 * 再次允许 TablespaceCreateDbspace。
	 */
	LWLockRelease(TablespaceCreateLock);

	/* We keep the lock on pg_tablespace until commit */
	/*
	 *
	 * 对 pg_tablespace 的锁保持到提交。
	 */
	table_close(rel, NoLock);
}


/*
 * create_tablespace_directories
 *
 * create_tablespace_directories。
 *
 *	Attempt to create filesystem infrastructure linking $PGDATA/pg_tblspc/
 *	to the specified directory
 *
 * 尝试建立把 $PGDATA/pg_tblspc/ 链接到指定目录的文件系统结构。
 */
static void
create_tablespace_directories(const char *location, const Oid tablespaceoid)
{
	char	   *linkloc;
	char	   *location_with_version_dir;
	struct stat st;
	bool		in_place;

	linkloc = psprintf("%s/%u", PG_TBLSPC_DIR, tablespaceoid);

	/*
	 * If we're asked to make an 'in place' tablespace, create the directory
	 * directly where the symlink would normally go.  This is a developer-only
	 * option for now, to facilitate regression testing.
	 *
	 * 若要求创建 in place 表空间，则直接在通常放置符号链接的位置建目录。
	 * 目前这只是便于回归测试的开发选项。
	 */
	in_place = strlen(location) == 0;

	if (in_place)
	{
		if (MakePGDirectory(linkloc) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							linkloc)));
	}

	location_with_version_dir = psprintf("%s/%s", in_place ? linkloc : location,
										 TABLESPACE_VERSION_DIRECTORY);

	/*
	 * Attempt to coerce target directory to safe permissions.  If this fails,
	 * it doesn't exist or has the wrong owner.  Not needed for in-place mode,
	 * because in that case we created the directory with the desired
	 * permissions.
	 *
	 * 尝试把目标目录权限收紧到安全值。失败则说明目录不存在或属主不对。
	 * in-place 模式不需要，因为目录是按所需权限创建的。
	 */
	if (!in_place && chmod(location, pg_dir_create_mode) != 0)
	{
		if (errno == ENOENT)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FILE),
					 errmsg("directory \"%s\" does not exist", location),
					 InRecovery ? errhint("Create this directory for the tablespace before "
										  "restarting the server.") : 0));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not set permissions on directory \"%s\": %m",
							location)));
	}

	/*
	 * The creation of the version directory prevents more than one tablespace
	 * in a single location.  This imitates TablespaceCreateDbspace(), but it
	 * ignores concurrency and missing parent directories.  The chmod() would
	 * have failed in the absence of a parent.  pg_tablespace_spcname_index
	 * prevents concurrency.
	 *
	 * 创建版本目录可以防止同一位置存在多个表空间。这模仿 TablespaceCreateDbspace()，
	 * 但不处理并发和缺失的父目录。没有父目录时 chmod() 本就会失败。
	 * pg_tablespace_spcname_index 防止并发。
	 */
	if (stat(location_with_version_dir, &st) < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m",
							location_with_version_dir)));
		else if (MakePGDirectory(location_with_version_dir) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							location_with_version_dir)));
	}
	else if (!S_ISDIR(st.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" exists but is not a directory",
						location_with_version_dir)));
	else if (!InRecovery)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("directory \"%s\" already in use as a tablespace",
						location_with_version_dir)));

	/*
	 * In recovery, remove old symlink, in case it points to the wrong place.
	 *
	 * 恢复时删掉旧符号链接，以免它指向错误位置。
	 */
	if (!in_place && InRecovery)
		remove_tablespace_symlink(linkloc);

	/*
	 * Create the symlink under PGDATA
	 *
	 * 在 PGDATA 下创建符号链接。
	 */
	if (!in_place && symlink(location, linkloc) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\": %m",
						linkloc)));

	pfree(linkloc);
	pfree(location_with_version_dir);
}


/*
 * destroy_tablespace_directories
 *
 * destroy_tablespace_directories。
 *
 * Attempt to remove filesystem infrastructure for the tablespace.
 *
 * 尝试删除该表空间的文件系统结构。
 *
 * 'redo' indicates we are redoing a drop from XLOG; in that case we should
 * not throw an ERROR for problems, just LOG them.  The worst consequence of
 * not removing files here would be failure to release some disk space, which
 * does not justify throwing an error that would require manual intervention
 * to get the database running again.
 *
 * redo 表示正在按 XLOG 重做删除；此时出问题不应 ERROR，只记 LOG。
 * 这里没删掉文件最坏只是没释放一些磁盘空间，不值得抛出需要人工干预才能让数据库继续运行的错误。
 *
 * Returns true if successful, false if some subdirectory is not empty
 *
 * 成功返回 true；若某个子目录非空则返回 false。
 */
static bool
destroy_tablespace_directories(Oid tablespaceoid, bool redo)
{
	char	   *linkloc;
	char	   *linkloc_with_version_dir;
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *subfile;
	struct stat st;

	linkloc_with_version_dir = psprintf("%s/%u/%s", PG_TBLSPC_DIR, tablespaceoid,
										TABLESPACE_VERSION_DIRECTORY);

	/*
	 * Check if the tablespace still contains any files.  We try to rmdir each
	 * per-database directory we find in it.  rmdir failure implies there are
	 * still files in that subdirectory, so give up.  (We do not have to worry
	 * about undoing any already completed rmdirs, since the next attempt to
	 * use the tablespace from that database will simply recreate the
	 * subdirectory via TablespaceCreateDbspace.)
	 *
	 * 检查表空间是否仍有文件。尝试 rmdir 其中每个按数据库划分的目录。
	 * rmdir 失败说明子目录里还有文件，于是放弃。（不必撤销已成功的 rmdir，
	 * 该数据库下次使用此表空间时会经 TablespaceCreateDbspace 重建子目录。）
	 *
	 * Since we hold TablespaceCreateLock, no one else should be creating any
	 * fresh subdirectories in parallel. It is possible that new files are
	 * being created within subdirectories, though, so the rmdir call could
	 * fail.  Worst consequence is a less friendly error message.
	 *
	 * 我们持有 TablespaceCreateLock，不应有人并发创建新子目录。
	 * 但子目录内仍可能正在创建新文件，因此 rmdir 可能失败。最坏只是错误信息没那么友好。
	 *
	 * If redo is true then ENOENT is a likely outcome here, and we allow it
	 * to pass without comment.  In normal operation we still allow it, but
	 * with a warning.  This is because even though ProcessUtility disallows
	 * DROP TABLESPACE in a transaction block, it's possible that a previous
	 * DROP failed and rolled back after removing the tablespace directories
	 * and/or symlink.  We want to allow a new DROP attempt to succeed at
	 * removing the catalog entries (and symlink if still present), so we
	 * should not give a hard error here.
	 *
	 * redo 为真时这里很可能得到 ENOENT，允许它无声通过。正常操作也允许，但会警告。
	 * 虽然 ProcessUtility 禁止在事务块中 DROP TABLESPACE，先前的 DROP 仍可能在删掉目录和/或符号链接后失败回滚。
	 * 新的 DROP 应能成功删掉目录项（以及仍在的符号链接），因此这里不应给硬错误。
	 */
	dirdesc = AllocateDir(linkloc_with_version_dir);
	if (dirdesc == NULL)
	{
		if (errno == ENOENT)
		{
			if (!redo)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not open directory \"%s\": %m",
								linkloc_with_version_dir)));
			/* The symlink might still exist, so go try to remove it */
			/*
			 *
			 * 符号链接可能还在，尝试删除它。
			 */
			goto remove_symlink;
		}
		else if (redo)
		{
			/* in redo, just log other types of error */
			/*
			 *
			 * redo 时，其他类型的错误只记日志。
			 */
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							linkloc_with_version_dir)));
			pfree(linkloc_with_version_dir);
			return false;
		}
		/* else let ReadDir report the error */
		/*
		 *
		 * 否则让 ReadDir 报告错误。
		 */
	}

	while ((de = ReadDir(dirdesc, linkloc_with_version_dir)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		subfile = psprintf("%s/%s", linkloc_with_version_dir, de->d_name);

		/* This check is just to deliver a friendlier error message */
		/*
		 *
		 * 此检查只是为了给出更友好的错误信息。
		 */
		if (!redo && !directory_is_empty(subfile))
		{
			FreeDir(dirdesc);
			pfree(subfile);
			pfree(linkloc_with_version_dir);
			return false;
		}

		/* remove empty directory */
		/*
		 *
		 * 删除空目录。
		 */
		if (rmdir(subfile) < 0)
			ereport(redo ? LOG : ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							subfile)));

		pfree(subfile);
	}

	FreeDir(dirdesc);

	/* remove version directory */
	/*
	 *
	 * 删除版本目录。
	 */
	if (rmdir(linkloc_with_version_dir) < 0)
	{
		ereport(redo ? LOG : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m",
						linkloc_with_version_dir)));
		pfree(linkloc_with_version_dir);
		return false;
	}

	/*
	 * Try to remove the symlink.  We must however deal with the possibility
	 * that it's a directory instead of a symlink --- this could happen during
	 * WAL replay (see TablespaceCreateDbspace).
	 *
	 * 尝试删除符号链接。但要处理它其实是目录的情况——WAL 重放时可能如此（见 TablespaceCreateDbspace）。
	 *
	 * Note: in the redo case, we'll return true if this final step fails;
	 * there's no point in retrying it.  Also, ENOENT should provoke no more
	 * than a warning.
	 *
	 * 注意：redo 时若这最后一步失败，仍返回 true，再重试没有意义。ENOENT 最多给一个警告。
	 */
remove_symlink:
	linkloc = pstrdup(linkloc_with_version_dir);
	get_parent_directory(linkloc);
	if (lstat(linkloc, &st) < 0)
	{
		int			saved_errno = errno;

		ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						linkloc)));
	}
	else if (S_ISDIR(st.st_mode))
	{
		if (rmdir(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
		}
	}
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
		}
	}
	else
	{
		/* Refuse to remove anything that's not a directory or symlink */
		/*
		 *
		 * 拒绝删除既不是目录也不是符号链接的东西。
		 */
		ereport(redo ? LOG : ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}

	pfree(linkloc_with_version_dir);
	pfree(linkloc);

	return true;
}


/*
 * Check if a directory is empty.
 *
 * 检查目录是否为空。
 *
 * This probably belongs somewhere else, but not sure where...
 *
 * 这段逻辑也许该放在别处，但还不确定放哪。
 */
bool
directory_is_empty(const char *path)
{
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(path);

	while ((de = ReadDir(dirdesc, path)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;
		FreeDir(dirdesc);
		return false;
	}

	FreeDir(dirdesc);
	return true;
}

/*
 *	remove_tablespace_symlink
 *
 * remove_tablespace_symlink。
 *
 * This function removes symlinks in pg_tblspc.  On Windows, junction points
 * act like directories so we must be able to apply rmdir.  This function
 * works like the symlink removal code in destroy_tablespace_directories,
 * except that failure to remove is always an ERROR.  But if the file doesn't
 * exist at all, that's OK.
 *
 * 本函数删除 pg_tblspc 中的符号链接。在 Windows 上 junction point 表现得像目录，因此必须能 rmdir。
 * 行为类似 destroy_tablespace_directories 里的删除逻辑，但删除失败始终是 ERROR。文件根本不存在则可以。
 */
void
remove_tablespace_symlink(const char *linkloc)
{
	struct stat st;

	if (lstat(linkloc, &st) < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", linkloc)));
	}

	if (S_ISDIR(st.st_mode))
	{
		/*
		 * This will fail if the directory isn't empty, but not if it's a
		 * junction point.
		 *
		 * 目录非空时会失败，但 junction point 不会。
		 */
		if (rmdir(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
	}
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
	}
	else
	{
		/* Refuse to remove anything that's not a directory or symlink */
		/*
		 *
		 * 拒绝删除既不是目录也不是符号链接的东西。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}
}

/*
 * Rename a tablespace
 *
 * 重命名表空间。
 */
ObjectAddress
RenameTableSpace(const char *oldname, const char *newname)
{
	Oid			tspId;
	Relation	rel;
	ScanKeyData entry[1];
	TableScanDesc scan;
	HeapTuple	tup;
	HeapTuple	newtuple;
	Form_pg_tablespace newform;
	ObjectAddress address;

	/* Search pg_tablespace */
	/*
	 *
	 * 搜索 pg_tablespace。
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scan = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						oldname)));

	newtuple = heap_copytuple(tup);
	newform = (Form_pg_tablespace) GETSTRUCT(newtuple);
	tspId = newform->oid;

	table_endscan(scan);

	/* Must be owner */
	/*
	 *
	 * 必须是属主。
	 */
	if (!object_ownercheck(TableSpaceRelationId, tspId, GetUserId()))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE, oldname);

	/* Validate new name */
	/*
	 *
	 * 校验新名称。
	 */
	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"", newname),
				 errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for tablespace names are violated.
	 *
	 * 若以相应开关编译，则在表空间名违反回归测试约定时告警。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(newname, "regress_", 8) != 0)
		elog(WARNING, "tablespaces created by regression test cases should have names starting with \"regress_\"");
#endif

	/* Make sure the new name doesn't exist */
	/*
	 *
	 * 确认新名称尚不存在。
	 */
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(newname));
	scan = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						newname)));

	table_endscan(scan);

	/* OK, update the entry */
	/*
	 *
	 * 可以，更新该目录项。
	 */
	namestrcpy(&(newform->spcname), newname);

	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tspId, 0);

	ObjectAddressSet(address, TableSpaceRelationId, tspId);

	table_close(rel, NoLock);

	return address;
}

/*
 * Alter table space options
 *
 * 修改表空间选项。
 */
Oid
AlterTableSpaceOptions(AlterTableSpaceOptionsStmt *stmt)
{
	Relation	rel;
	ScanKeyData entry[1];
	TableScanDesc scandesc;
	HeapTuple	tup;
	Oid			tablespaceoid;
	Datum		datum;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_tablespace];
	bool		isnull;
	bool		repl_null[Natts_pg_tablespace];
	bool		repl_repl[Natts_pg_tablespace];
	HeapTuple	newtuple;

	/* Search pg_tablespace */
	/*
	 *
	 * 搜索 pg_tablespace。
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						stmt->tablespacename)));

	tablespaceoid = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

	/* Must be owner of the existing object */
	/*
	 *
	 * 必须是现有对象的属主。
	 */
	if (!object_ownercheck(TableSpaceRelationId, tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   stmt->tablespacename);

	/* Generate new proposed spcoptions (text array) */
	/*
	 *
	 * 生成新的 spcoptions（text 数组）。
	 */
	datum = heap_getattr(tup, Anum_pg_tablespace_spcoptions,
						 RelationGetDescr(rel), &isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 stmt->options, NULL, NULL, false,
									 stmt->isReset);
	(void) tablespace_reloptions(newOptions, true);

	/* Build new tuple. */
	/*
	 *
	 * 构造新元组。
	 */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_tablespace_spcoptions - 1] = true;
	repl_repl[Anum_pg_tablespace_spcoptions - 1] = true;
	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val,
								 repl_null, repl_repl);

	/* Update system catalog. */
	/*
	 *
	 * 更新系统目录。
	 */
	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tablespaceoid, 0);

	heap_freetuple(newtuple);

	/* Conclude heap scan. */
	/*
	 *
	 * 结束堆扫描。
	 */
	table_endscan(scandesc);
	table_close(rel, NoLock);

	return tablespaceoid;
}

/*
 * Routines for handling the GUC variable 'default_tablespace'.
 *
 * 处理 GUC 变量 default_tablespace 的例程。
 */

/* check_hook: validate new default_tablespace */
/*
 *
 * check_hook：校验新的 default_tablespace。
 */
bool
check_default_tablespace(char **newval, void **extra, GucSource source)
{
	/*
	 * If we aren't inside a transaction, or connected to a database, we
	 * cannot do the catalog accesses necessary to verify the name.  Must
	 * accept the value on faith.
	 *
	 * 若不在事务中或未连接到数据库，就无法访问目录来校验名称。只能先接受该值。
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		if (**newval != '\0' &&
			!OidIsValid(get_tablespace_oid(*newval, true)))
		{
			/*
			 * When source == PGC_S_TEST, don't throw a hard error for a
			 * nonexistent tablespace, only a NOTICE.  See comments in guc.h.
			 *
			 * 当 source 为 PGC_S_TEST 时，表空间不存在不抛硬错误，只发 NOTICE。见 guc.h 的注释。
			 */
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tablespace \"%s\" does not exist",
								*newval)));
			}
			else
			{
				GUC_check_errdetail("Tablespace \"%s\" does not exist.",
									*newval);
				return false;
			}
		}
	}

	return true;
}

/*
 * GetDefaultTablespace -- get the OID of the current default tablespace
 *
 * GetDefaultTablespace：取得当前默认表空间的 OID。
 *
 * Temporary objects have different default tablespaces, hence the
 * relpersistence parameter must be specified.  Also, for partitioned tables,
 * we disallow specifying the database default, so that needs to be specified
 * too.
 *
 * 临时对象使用不同的默认表空间，因此必须给出 relpersistence。
 * 分区表不允许指定数据库默认表空间，这一点也要一并指明。
 *
 * May return InvalidOid to indicate "use the database's default tablespace".
 *
 * 可以返回 InvalidOid，表示使用数据库的默认表空间。
 *
 * Note that caller is expected to check appropriate permissions for any
 * result other than InvalidOid.
 *
 * 调用方应对除 InvalidOid 以外的结果检查相应权限。
 *
 * This exists to hide (and possibly optimize the use of) the
 * default_tablespace GUC variable.
 *
 * 这样做是为了隐藏（并可能优化使用）default_tablespace 这个 GUC。
 */
Oid
GetDefaultTablespace(char relpersistence, bool partitioned)
{
	Oid			result;

	/* The temp-table case is handled elsewhere */
	/*
	 *
	 * 临时表的情况在别处处理。
	 */
	if (relpersistence == RELPERSISTENCE_TEMP)
	{
		PrepareTempTablespaces();
		return GetNextTempTableSpace();
	}

	/* Fast path for default_tablespace == "" */
	/*
	 *
	 * default_tablespace 为空字符串时的快速路径。
	 */
	if (default_tablespace == NULL || default_tablespace[0] == '\0')
		return InvalidOid;

	/*
	 * It is tempting to cache this lookup for more speed, but then we would
	 * fail to detect the case where the tablespace was dropped since the GUC
	 * variable was set.  Note also that we don't complain if the value fails
	 * to refer to an existing tablespace; we just silently return InvalidOid,
	 * causing the new object to be created in the database's tablespace.
	 *
	 * 缓存这次查找会更快，但就无法发现 GUC 设置之后表空间已被删除。
	 * 若该值不指向现有表空间，也不报错，只是静默返回 InvalidOid，
	 * 使新对象创建在数据库的表空间中。
	 */
	result = get_tablespace_oid(default_tablespace, true);

	/*
	 * Allow explicit specification of database's default tablespace in
	 * default_tablespace without triggering permissions checks.  Don't allow
	 * specifying that when creating a partitioned table, however, since the
	 * result is confusing.
	 *
	 * 允许在 default_tablespace 中显式指定数据库默认表空间，且不触发权限检查。
	 * 但创建分区表时不允许这样指定，因为结果会令人困惑。
	 */
	if (result == MyDatabaseTableSpace)
	{
		if (partitioned)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
		result = InvalidOid;
	}
	return result;
}


/*
 * Routines for handling the GUC variable 'temp_tablespaces'.
 *
 * 处理 GUC 变量 temp_tablespaces 的例程。
 */

typedef struct
{
	/* Array of OIDs to be passed to SetTempTablespaces() */
	/*
	 *
	 * 传给 SetTempTablespaces() 的 OID 数组。
	 */
	int			numSpcs;
	Oid			tblSpcs[FLEXIBLE_ARRAY_MEMBER];
} temp_tablespaces_extra;

/* check_hook: validate new temp_tablespaces */
/*
 *
 * check_hook：校验新的 temp_tablespaces。
 */
bool
check_temp_tablespaces(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	List	   *namelist;

	/* Need a modifiable copy of string */
	/*
	 *
	 * 需要一份可修改的字符串副本。
	 */
	rawname = pstrdup(*newval);

	/* Parse string into list of identifiers */
	/*
	 *
	 * 把字符串解析成标识符列表。
	 */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		/*
		 *
		 * 名称列表有语法错误。
		 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawname);
		list_free(namelist);
		return false;
	}

	/*
	 * If we aren't inside a transaction, or connected to a database, we
	 * cannot do the catalog accesses necessary to verify the name.  Must
	 * accept the value on faith. Fortunately, there's then also no need to
	 * pass the data to fd.c.
	 *
	 * 若不在事务中或未连接到数据库，就无法访问目录来校验名称，只能先接受该值。
	 * 好在此时也不必把数据传给 fd.c。
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		temp_tablespaces_extra *myextra;
		Oid		   *tblSpcs;
		int			numSpcs;
		ListCell   *l;

		/* temporary workspace until we are done verifying the list */
		/*
		 *
		 * 在校验完列表之前使用的临时工作区。
		 */
		tblSpcs = (Oid *) palloc(list_length(namelist) * sizeof(Oid));
		numSpcs = 0;
		foreach(l, namelist)
		{
			char	   *curname = (char *) lfirst(l);
			Oid			curoid;
			AclResult	aclresult;

			/* Allow an empty string (signifying database default) */
			/*
			 *
			 * 允许空字符串（表示数据库默认表空间）。
			 */
			if (curname[0] == '\0')
			{
				/* InvalidOid signifies database's default tablespace */
				/*
				 *
				 * InvalidOid 表示数据库的默认表空间。
				 */
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/*
			 * In an interactive SET command, we ereport for bad info.  When
			 * source == PGC_S_TEST, don't throw a hard error for a
			 * nonexistent tablespace, only a NOTICE.  See comments in guc.h.
			 *
			 * 交互式 SET 遇到错误信息会 ereport。当 source 为 PGC_S_TEST 时，
			 * 表空间不存在不抛硬错误，只发 NOTICE。见 guc.h 的注释。
			 */
			curoid = get_tablespace_oid(curname, source <= PGC_S_TEST);
			if (curoid == InvalidOid)
			{
				if (source == PGC_S_TEST)
					ereport(NOTICE,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace \"%s\" does not exist",
									curname)));
				continue;
			}

			/*
			 * Allow explicit specification of database's default tablespace
			 * in temp_tablespaces without triggering permissions checks.
			 *
			 * 允许在 temp_tablespaces 中显式指定数据库默认表空间，且不触发权限检查。
			 */
			if (curoid == MyDatabaseTableSpace)
			{
				/* InvalidOid signifies database's default tablespace */
				/*
				 *
				 * InvalidOid 表示数据库的默认表空间。
				 */
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/* Check permissions, similarly complaining only if interactive */
			/*
			 *
			 * 检查权限；同样只在交互式设置时抱怨。
			 */
			aclresult = object_aclcheck(TableSpaceRelationId, curoid, GetUserId(),
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
			{
				if (source >= PGC_S_INTERACTIVE)
					aclcheck_error(aclresult, OBJECT_TABLESPACE, curname);
				continue;
			}

			tblSpcs[numSpcs++] = curoid;
		}

		/* Now prepare an "extra" struct for assign_temp_tablespaces */
		/*
		 *
		 * 现在为 assign_temp_tablespaces 准备 extra 结构。
		 */
		myextra = guc_malloc(LOG, offsetof(temp_tablespaces_extra, tblSpcs) +
							 numSpcs * sizeof(Oid));
		if (!myextra)
			return false;
		myextra->numSpcs = numSpcs;
		memcpy(myextra->tblSpcs, tblSpcs, numSpcs * sizeof(Oid));
		*extra = myextra;

		pfree(tblSpcs);
	}

	pfree(rawname);
	list_free(namelist);

	return true;
}

/* assign_hook: do extra actions as needed */
/*
 *
 * assign_hook：按需做额外动作。
 */
void
assign_temp_tablespaces(const char *newval, void *extra)
{
	temp_tablespaces_extra *myextra = (temp_tablespaces_extra *) extra;

	/*
	 * If check_temp_tablespaces was executed inside a transaction, then pass
	 * the list it made to fd.c.  Otherwise, clear fd.c's list; we must be
	 * still outside a transaction, or else restoring during transaction exit,
	 * and in either case we can just let the next PrepareTempTablespaces call
	 * make things sane.
	 *
	 * 若 check_temp_tablespaces 在事务内执行，则把它生成的列表传给 fd.c。
	 * 否则清空 fd.c 的列表：此时要么仍在事务外，要么正在事务退出时恢复，
	 * 都可以等下一次 PrepareTempTablespaces 再把状态弄正确。
	 */
	if (myextra)
		SetTempTablespaces(myextra->tblSpcs, myextra->numSpcs);
	else
		SetTempTablespaces(NULL, 0);
}

/*
 * PrepareTempTablespaces -- prepare to use temp tablespaces
 *
 * PrepareTempTablespaces：准备使用临时表空间。
 *
 * If we have not already done so in the current transaction, parse the
 * temp_tablespaces GUC variable and tell fd.c which tablespace(s) to use
 * for temp files.
 *
 * 若当前事务中还没做过，则解析 temp_tablespaces，并告诉 fd.c 临时文件使用哪些表空间。
 */
void
PrepareTempTablespaces(void)
{
	char	   *rawname;
	List	   *namelist;
	Oid		   *tblSpcs;
	int			numSpcs;
	ListCell   *l;

	/* No work if already done in current transaction */
	/*
	 *
	 * 若当前事务中已经做过，则无需再做。
	 */
	if (TempTablespacesAreSet())
		return;

	/*
	 * Can't do catalog access unless within a transaction.  This is just a
	 * safety check in case this function is called by low-level code that
	 * could conceivably execute outside a transaction.  Note that in such a
	 * scenario, fd.c will fall back to using the current database's default
	 * tablespace, which should always be OK.
	 *
	 * 不在事务中就不能访问目录。这只是安全检查，以防底层代码在事务外调用本函数。
	 * 那种情况下 fd.c 会退回当前数据库的默认表空间，这应当总是可以的。
	 */
	if (!IsTransactionState())
		return;

	/* Need a modifiable copy of string */
	/*
	 *
	 * 需要一份可修改的字符串副本。
	 */
	rawname = pstrdup(temp_tablespaces);

	/* Parse string into list of identifiers */
	/*
	 *
	 * 把字符串解析成标识符列表。
	 */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		/*
		 *
		 * 名称列表有语法错误。
		 */
		SetTempTablespaces(NULL, 0);
		pfree(rawname);
		list_free(namelist);
		return;
	}

	/* Store tablespace OIDs in an array in TopTransactionContext */
	/*
	 *
	 * 把表空间 OID 存入 TopTransactionContext 中的数组。
	 */
	tblSpcs = (Oid *) MemoryContextAlloc(TopTransactionContext,
										 list_length(namelist) * sizeof(Oid));
	numSpcs = 0;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			curoid;
		AclResult	aclresult;

		/* Allow an empty string (signifying database default) */
		/*
		 *
		 * 允许空字符串（表示数据库默认表空间）。
		 */
		if (curname[0] == '\0')
		{
			/* InvalidOid signifies database's default tablespace */
			/*
			 *
			 * InvalidOid 表示数据库的默认表空间。
			 */
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* Else verify that name is a valid tablespace name */
		/*
		 *
		 * 否则确认该名称是有效的表空间名。
		 */
		curoid = get_tablespace_oid(curname, true);
		if (curoid == InvalidOid)
		{
			/* Skip any bad list elements */
			/*
			 *
			 * 跳过列表中的坏元素。
			 */
			continue;
		}

		/*
		 * Allow explicit specification of database's default tablespace in
		 * temp_tablespaces without triggering permissions checks.
		 *
		 * 允许在 temp_tablespaces 中显式指定数据库默认表空间，且不触发权限检查。
		 */
		if (curoid == MyDatabaseTableSpace)
		{
			/* InvalidOid signifies database's default tablespace */
			/*
			 *
			 * InvalidOid 表示数据库的默认表空间。
			 */
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* Check permissions similarly */
		/*
		 *
		 * 同样检查权限。
		 */
		aclresult = object_aclcheck(TableSpaceRelationId, curoid, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			continue;

		tblSpcs[numSpcs++] = curoid;
	}

	SetTempTablespaces(tblSpcs, numSpcs);

	pfree(rawname);
	list_free(namelist);
}


/*
 * get_tablespace_oid - given a tablespace name, look up the OID
 *
 * get_tablespace_oid：按表空间名查找 OID。
 *
 * If missing_ok is false, throw an error if tablespace name not found.  If
 * true, just return InvalidOid.
 *
 * missing_ok 为 false 时，找不到表空间名就报错；为 true 时返回 InvalidOid。
 */
Oid
get_tablespace_oid(const char *tablespacename, bool missing_ok)
{
	Oid			result;
	Relation	rel;
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on name, on the theory that pg_tablespace will usually have just
	 * a few entries and so an indexed lookup is a waste of effort.
	 *
	 * 搜索 pg_tablespace。尽管 name 上有索引，这里仍用堆扫描，
	 * 因为 pg_tablespace 通常只有几行，走索引并不划算。
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	/*
	 *
	 * 假定最多只有一个匹配元组。
	 */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_tablespace) GETSTRUCT(tuple))->oid;
	else
		result = InvalidOid;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						tablespacename)));

	return result;
}

/*
 * get_tablespace_name - given a tablespace OID, look up the name
 *
 * get_tablespace_name：按表空间 OID 查找名称。
 *
 * Returns a palloc'd string, or NULL if no such tablespace.
 *
 * 返回 palloc 分配的字符串；没有该表空间则返回 NULL。
 */
char *
get_tablespace_name(Oid spc_oid)
{
	char	   *result;
	Relation	rel;
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on oid, on the theory that pg_tablespace will usually have just a
	 * few entries and so an indexed lookup is a waste of effort.
	 *
	 * 搜索 pg_tablespace。尽管 oid 上有索引，这里仍用堆扫描，
	 * 因为 pg_tablespace 通常只有几行，走索引并不划算。
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	/*
	 *
	 * 假定最多只有一个匹配元组。
	 */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablespace) GETSTRUCT(tuple))->spcname));
	else
		result = NULL;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	return result;
}


/*
 * TABLESPACE resource manager's routines
 *
 * TABLESPACE 资源管理器的例程。
 */
void
tblspc_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in tblspc records */
	/*
	 *
	 * tblspc 记录不使用备份块。
	 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) XLogRecGetData(record);
		char	   *location = xlrec->ts_path;

		create_tablespace_directories(location, xlrec->ts_id);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) XLogRecGetData(record);

		/* Close all smgr fds in all backends. */
		/*
		 *
		 * 关闭所有后端中的全部 smgr 文件描述符。
		 */
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

		/*
		 * If we issued a WAL record for a drop tablespace it implies that
		 * there were no files in it at all when the DROP was done. That means
		 * that no permanent objects can exist in it at this point.
		 *
		 * 若为删除表空间写了 WAL 记录，说明 DROP 时里面已经没有任何文件。
		 * 因此此刻不可能还有永久对象。
		 *
		 * It is possible for standby users to be using this tablespace as a
		 * location for their temporary files, so if we fail to remove all
		 * files then do conflict processing and try again, if currently
		 * enabled.
		 *
		 * 备库用户可能把此表空间用作临时文件位置。若没能删光文件，
		 * 则在当前启用了冲突处理时做冲突处理并重试。
		 *
		 * Other possible reasons for failure include bollixed file
		 * permissions on a standby server when they were okay on the primary,
		 * etc etc. There's not much we can do about that, so just remove what
		 * we can and press on.
		 *
		 * 其他失败原因包括备库上文件权限乱了而主库上正常，等等。对此能做的不多，能删多少就删多少，然后继续。
		 */
		if (!destroy_tablespace_directories(xlrec->ts_id, true))
		{
			ResolveRecoveryConflictWithTablespace(xlrec->ts_id);

			/*
			 * If we did recovery processing then hopefully the backends who
			 * wrote temp files should have cleaned up and exited by now.  So
			 * retry before complaining.  If we fail again, this is just a LOG
			 * condition, because it's not worth throwing an ERROR for (as
			 * that would crash the database and require manual intervention
			 * before we could get past this WAL record on restart).
			 *
			 * 若做过恢复处理，写过临时文件的后端现在应该已经清理并退出。因此先重试再抱怨。
			 * 若再次失败，只记 LOG：不值得 ERROR，否则会让数据库崩溃，重启后还要人工干预才能越过这条 WAL 记录。
			 */
			if (!destroy_tablespace_directories(xlrec->ts_id, true))
				ereport(LOG,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("directories for tablespace %u could not be removed",
								xlrec->ts_id),
						 errhint("You can remove the directories manually if necessary.")));
		}
	}
	else
		elog(PANIC, "tblspc_redo: unknown op code %u", info);
}
