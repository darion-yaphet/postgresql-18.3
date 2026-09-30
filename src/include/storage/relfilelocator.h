/*-------------------------------------------------------------------------
 *
 * relfilelocator.h
 *	  Physical access information for relations.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/relfilelocator.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILELOCATOR_H
#define RELFILELOCATOR_H

#include "common/relpath.h"
#include "storage/procnumber.h"

/*
 * RelFileLocator must provide all that we need to know to physically access
 * a relation, with the exception of the backend's proc number, which can be
 * provided separately.  Note, however, that a "physical" relation is
 * comprised of multiple files on the filesystem, as each fork is stored as
 * a separate file, and each fork can be divided into multiple segments. See
 * md.c.
 *
 * spcOid identifies the tablespace of the relation.  It corresponds to
 * pg_tablespace.oid.
 *
 * dbOid identifies the database of the relation.  It is zero for
 * "shared" relations (those common to all databases of a cluster).
 * Nonzero dbOid values correspond to pg_database.oid.
 *
 * relNumber identifies the specific relation.  relNumber corresponds to
 * pg_class.relfilenode (NOT pg_class.oid, because we need to be able
 * to assign new physical files to relations in some situations).
 * Notice that relNumber is only unique within a database in a particular
 * tablespace.
 *
 * Note: spcOid must be GLOBALTABLESPACE_OID if and only if dbOid is
 * zero.  We support shared relations only in the "global" tablespace.
 *
 * Note: in pg_class we allow reltablespace == 0 to denote that the
 * relation is stored in its database's "default" tablespace (as
 * identified by pg_database.dattablespace).  However this shorthand
 * is NOT allowed in RelFileLocator structs --- the real tablespace ID
 * must be supplied when setting spcOid.
 *
 * Note: in pg_class, relfilenode can be zero to denote that the relation
 * is a "mapped" relation, whose current true filenode number is available
 * from relmapper.c.  Again, this case is NOT allowed in RelFileLocators.
 *
 * Note: various places use RelFileLocator in hashtable keys.  Therefore,
 * there *must not* be any unused padding bytes in this struct.  That
 * should be safe as long as all the fields are of type Oid.
 */

/*
 * RelFileLocator 必须提供物理访问一个关系所需的全部信息，后端的进程编号除外，后者
 * 可以单独提供。请注意，一个“物理”关系由文件系统中的多个文件组成：每个分叉文件单独
 * 存储，每个分叉文件又可划分为多个段。参见 md.c。
 *
 * spcOid 标识关系所在的表空间，对应 pg_tablespace.oid。
 *
 * dbOid 标识关系所在的数据库。对“共享”关系（集群中所有数据库共用的关系）其值为零；
 * 非零 dbOid 对应 pg_database.oid。
 *
 * relNumber 标识具体关系，对应 pg_class.relfilenode（不是 pg_class.oid，因为某些
 * 情形需要为关系分配新的物理文件）。请注意，relNumber 仅在特定表空间内的数据库中
 * 唯一。
 *
 * 注意：当且仅当 dbOid 为零时，spcOid 必须为 GLOBALTABLESPACE_OID。仅“全局”表空间
 * 支持共享关系。
 *
 * 注意：pg_class 中允许 reltablespace == 0 表示关系存储在其数据库的“默认”表空间
 * （由 pg_database.dattablespace 标识）。但 RelFileLocator 结构体不允许使用这一简写
 * ——设置 spcOid 时必须提供真实表空间 ID。
 *
 * 注意：pg_class 中 relfilenode 可为零，表示该关系是“映射”关系，其当前真实文件节点号
 * 可从 relmapper.c 获得。RelFileLocator 同样不允许该情形。
 *
 * 注意：多个位置将 RelFileLocator 用作哈希表键。因此该结构体中绝不能有未使用的填充
 * 字节。只要所有字段都是 Oid 类型，这应当是安全的。
 */
typedef struct RelFileLocator
{
	Oid			spcOid;			/* tablespace */

	/* 表空间。 */
	Oid			dbOid;			/* database */

	/* 数据库。 */
	RelFileNumber relNumber;	/* relation */

	/* 关系。 */
} RelFileLocator;

/*
 * Augmenting a relfilelocator with the backend's proc number provides all the
 * information we need to locate the physical storage.  'backend' is
 * INVALID_PROC_NUMBER for regular relations (those accessible to more than
 * one backend), or the owning backend's proc number for backend-local
 * relations.  Backend-local relations are always transient and removed in
 * case of a database crash; they are never WAL-logged or fsync'd.
 */

/*
 * 为 relfilelocator 添加后端进程编号即可获得定位物理存储所需的完整信息。对于常规关系
 * （可由一个以上后端访问），backend 为 INVALID_PROC_NUMBER；对于后端本地关系，则为
 * 所有者后端的进程编号。后端本地关系始终是临时的，数据库崩溃时会被移除；它们从不写入
 * WAL，也不执行 fsync。
 */
typedef struct RelFileLocatorBackend
{
	RelFileLocator locator;
	ProcNumber	backend;
} RelFileLocatorBackend;

#define RelFileLocatorBackendIsTemp(rlocator) \
	((rlocator).backend != INVALID_PROC_NUMBER)

/*
 * Note: RelFileLocatorEquals and RelFileLocatorBackendEquals compare relNumber
 * first since that is most likely to be different in two unequal
 * RelFileLocators.  It is probably redundant to compare spcOid if the other
 * fields are found equal, but do it anyway to be sure.  Likewise for checking
 * the backend number in RelFileLocatorBackendEquals.
 */

/*
 * 注意：RelFileLocatorEquals 和 RelFileLocatorBackendEquals 首先比较 relNumber，
 * 因为它最可能在两个不相等的 RelFileLocator 中不同。如果其他字段相等，比较 spcOid
 * 可能是冗余的，但仍进行比较以确保正确性。RelFileLocatorBackendEquals 中检查后端编号
 * 也是如此。
 */
#define RelFileLocatorEquals(locator1, locator2) \
	((locator1).relNumber == (locator2).relNumber && \
	 (locator1).dbOid == (locator2).dbOid && \
	 (locator1).spcOid == (locator2).spcOid)

#define RelFileLocatorBackendEquals(locator1, locator2) \
	((locator1).locator.relNumber == (locator2).locator.relNumber && \
	 (locator1).locator.dbOid == (locator2).locator.dbOid && \
	 (locator1).backend == (locator2).backend && \
	 (locator1).locator.spcOid == (locator2).locator.spcOid)

#endif							/* RELFILELOCATOR_H */
