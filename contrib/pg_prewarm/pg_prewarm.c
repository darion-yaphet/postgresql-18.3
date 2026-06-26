/*-------------------------------------------------------------------------
 *
 * pg_prewarm.c
 *		  prewarming utilities
 *
 * Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/pg_prewarm/pg_prewarm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/relation.h"
#include "catalog/index.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_prewarm",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(pg_prewarm);

typedef enum
{
	PREWARM_PREFETCH,
	PREWARM_READ,
	PREWARM_BUFFER,
} PrewarmType;

static PGIOAlignedBlock blockbuffer;

/*
 * pg_prewarm(regclass, mode text, fork text,
 *			  first_block int8, last_block int8)
 *
 * pg_prewarm（regclass，模式文本，fork 文本，first_block int8，last_block int8）
 *
 * The first argument is the relation to be prewarmed; the second controls
 * how prewarming is done; legal options are 'prefetch', 'read', and 'buffer'.
 * The third is the name of the relation fork to be prewarmed.  The fourth
 * and fifth arguments specify the first and last block to be prewarmed.
 * If the fourth argument is NULL, it will be taken as 0; if the fifth argument
 * is NULL, it will be taken as the number of blocks in the relation.  The
 * return value is the number of blocks successfully prewarmed.
 *
 * 第一个参数是要预热的关系；第二个控制如何进行预热；合法的选项是“预取”、“读取”和“缓冲区”。第三个是要预热的关系叉的名称。  第四个和第五个参数指定要预热的第一个和最后一个块。如果第四个参数为NULL，则视为0；如果第五个参数为 NULL，则将其视为关系中的块数。  返回值是成功预热的块数。
 */
Datum
pg_prewarm(PG_FUNCTION_ARGS)
{
	Oid			relOid;
	text	   *forkName;
	text	   *type;
	int64		first_block;
	int64		last_block;
	int64		nblocks;
	int64		blocks_done = 0;
	int64		block;
	Relation	rel;
	ForkNumber	forkNumber;
	char	   *forkString;
	char	   *ttype;
	PrewarmType ptype;
	AclResult	aclresult;
	char		relkind;
	Oid			privOid;

	/* Basic sanity checking.
	 *
	 * 基本的健全性检查。
	 */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be null")));
	relOid = PG_GETARG_OID(0);
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("prewarm type cannot be null")));
	type = PG_GETARG_TEXT_PP(1);
	ttype = text_to_cstring(type);
	if (strcmp(ttype, "prefetch") == 0)
		ptype = PREWARM_PREFETCH;
	else if (strcmp(ttype, "read") == 0)
		ptype = PREWARM_READ;
	else if (strcmp(ttype, "buffer") == 0)
		ptype = PREWARM_BUFFER;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid prewarm type"),
				 errhint("Valid prewarm types are \"prefetch\", \"read\", and \"buffer\".")));
		PG_RETURN_INT64(0);		/* Placate compiler. */
	}
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation fork cannot be null")));
	forkName = PG_GETARG_TEXT_PP(2);
	forkString = text_to_cstring(forkName);
	forkNumber = forkname_to_number(forkString);

	/*
	 * Open relation and check privileges.  If the relation is an index, we
	 * must check the privileges on its parent table instead.
	 *
	 * 打开关系并检查权限。  如果关系是索引，我们必须检查其父表的权限。
	 */
	relkind = get_rel_relkind(relOid);
	if (relkind == RELKIND_INDEX ||
		relkind == RELKIND_PARTITIONED_INDEX)
	{
		privOid = IndexGetRelation(relOid, true);

		/* Lock table before index to avoid deadlock.
		 *
		 * 在索引之前锁定表以避免死锁。
		 */
		if (OidIsValid(privOid))
			LockRelationOid(privOid, AccessShareLock);
	}
	else
		privOid = relOid;

	rel = relation_open(relOid, AccessShareLock);

	/*
	 * It's possible that the relation with OID "privOid" was dropped and the
	 * OID was reused before we locked it.  If that happens, we could be left
	 * with the wrong parent table OID, in which case we must ERROR.  It's
	 * possible that such a race would change the outcome of
	 * get_rel_relkind(), too, but the worst case scenario there is that we'll
	 * check privileges on the index instead of its parent table, which isn't
	 * too terrible.
	 *
	 * 有可能在我们锁定它之前，与 OID“privOid”的关系已被删除并且 OID 被重新使用。  如果发生这种情况，我们可能会留下错误的父表 OID，在这种情况下我们必须出错。  这样的竞争也可能会改变 get_rel_relkind() 的结果，但最坏的情况是我们将检查索引而不是其父表的权限，这并不算太糟糕。
	 */
	if (!OidIsValid(privOid) ||
		(privOid != relOid &&
		 privOid != IndexGetRelation(relOid, true)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not find parent table of index \"%s\"",
						RelationGetRelationName(rel))));

	aclresult = pg_class_aclcheck(privOid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind), get_rel_name(relOid));

	/* Check that the relation has storage.
	 *
	 * 检查关系是否有存储。
	 */
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have storage",
						RelationGetRelationName(rel)),
				 errdetail_relkind_not_supported(rel->rd_rel->relkind)));

	/* Check that the fork exists.
	 *
	 * 检查分叉是否存在。
	 */
	if (!smgrexists(RelationGetSmgr(rel), forkNumber))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fork \"%s\" does not exist for this relation",
						forkString)));

	/* Validate block numbers, or handle nulls.
	 *
	 * 验证块号，或处理空值。
	 */
	nblocks = RelationGetNumberOfBlocksInFork(rel, forkNumber);
	if (PG_ARGISNULL(3))
		first_block = 0;
	else
	{
		first_block = PG_GETARG_INT64(3);
		if (first_block < 0 || first_block >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("starting block number must be between 0 and %" PRId64,
							(nblocks - 1))));
	}
	if (PG_ARGISNULL(4))
		last_block = nblocks - 1;
	else
	{
		last_block = PG_GETARG_INT64(4);
		if (last_block < 0 || last_block >= nblocks)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ending block number must be between 0 and %" PRId64,
							(nblocks - 1))));
	}

	/* Now we're ready to do the real work.
	 *
	 * 现在我们准备好做真正的工作了。
	 */
	if (ptype == PREWARM_PREFETCH)
	{
#ifdef USE_PREFETCH

		/*
		 * In prefetch mode, we just hint the OS to read the blocks, but we
		 * don't know whether it really does it, and we don't wait for it to
		 * finish.
		 *
		 * 在预取模式下，我们只是提示操作系统读取块，但我们不知道它是否真的这样做，并且我们不等待它完成。
		 *
		 * It would probably be better to pass our prefetch requests in chunks
		 * of a megabyte or maybe even a whole segment at a time, but there's
		 * no practical way to do that at present without a gross modularity
		 * violation, so we just do this.
		 *
		 * 最好一次以兆字节或什至整个段的形式传递预取请求，但目前没有实际的方法可以在不严重违反模块化的情况下做到这一点，所以我们就这样做。
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			CHECK_FOR_INTERRUPTS();
			PrefetchBuffer(rel, forkNumber, block);
			++blocks_done;
		}
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("prefetch is not supported by this build")));
#endif
	}
	else if (ptype == PREWARM_READ)
	{
		/*
		 * In read mode, we actually read the blocks, but not into shared
		 * buffers.  This is more portable than prefetch mode (it works
		 * everywhere) and is synchronous.
		 *
		 * 在读取模式下，我们实际上读取块，但不读取到共享缓冲区中。  这比预取模式更便携（它在任何地方都适用）并且是同步的。
		 */
		for (block = first_block; block <= last_block; ++block)
		{
			CHECK_FOR_INTERRUPTS();
			smgrread(RelationGetSmgr(rel), forkNumber, block, blockbuffer.data);
			++blocks_done;
		}
	}
	else if (ptype == PREWARM_BUFFER)
	{
		BlockRangeReadStreamPrivate p;
		ReadStream *stream;

		/*
		 * In buffer mode, we actually pull the data into shared_buffers.
		 *
		 * 在缓冲模式下，我们实际上将数据拉入shared_buffers。
		 */

		/* Set up the private state for our streaming buffer read callback.
		 *
		 * 为我们的流缓冲区读取回调设置私有状态。
		 */
		p.current_blocknum = first_block;
		p.last_exclusive = last_block + 1;

		/*
		 * It is safe to use batchmode as block_range_read_stream_cb takes no
		 * locks.
		 *
		 * 使用批处理模式是安全的，因为 block_range_read_stream_cb 不加锁。
		 */
		stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
											READ_STREAM_FULL |
											READ_STREAM_USE_BATCHING,
											NULL,
											rel,
											forkNumber,
											block_range_read_stream_cb,
											&p,
											0);

		for (block = first_block; block <= last_block; ++block)
		{
			Buffer		buf;

			CHECK_FOR_INTERRUPTS();
			buf = read_stream_next_buffer(stream, NULL);
			ReleaseBuffer(buf);
			++blocks_done;
		}
		Assert(read_stream_next_buffer(stream, NULL) == InvalidBuffer);
		read_stream_end(stream);
	}

	/* Close relation, release locks.
	 *
	 * 密切关系，释放锁定。
	 */
	relation_close(rel, AccessShareLock);

	if (privOid != relOid)
		UnlockRelationOid(privOid, AccessShareLock);

	PG_RETURN_INT64(blocks_done);
}
