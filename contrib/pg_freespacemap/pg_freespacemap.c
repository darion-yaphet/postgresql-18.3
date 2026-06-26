/*-------------------------------------------------------------------------
 *
 * pg_freespacemap.c
 *	  display contents of a free space map
 *
 * pg_freespacemap.c 显示自由空间映射的内容
 *
 *	  contrib/pg_freespacemap/pg_freespacemap.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "storage/freespace.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_freespacemap",
					.version = PG_VERSION
);

/*
 * Returns the amount of free space on a given page, according to the
 * free space map.
 *
 * 根据可用空间映射，返回给定页面上的可用空间量。
 */
PG_FUNCTION_INFO_V1(pg_freespace);

Datum
pg_freespace(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		blkno = PG_GETARG_INT64(1);
	int16		freespace;
	Relation	rel;

	rel = relation_open(relid, AccessShareLock);

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have storage",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	if (blkno < 0 || blkno > MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid block number")));

	freespace = GetRecordedFreeSpace(rel, blkno);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT16(freespace);
}
