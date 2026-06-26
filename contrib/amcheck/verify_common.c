/*-------------------------------------------------------------------------
 *
 * verify_common.c
 *		Utility functions common to all access methods.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_common.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "verify_common.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/tablecmds.h"
#include "utils/guc.h"
#include "utils/syscache.h"

static bool amcheck_index_mainfork_expected(Relation rel);


/*
 * Check if index relation should have a file for its main relation fork.
 * Verification uses this to skip unlogged indexes when in hot standby mode,
 * where there is simply nothing to verify.
 *
 * 检查索引关系是否应该有一个用于其主关系分支的文件。当处于热备用模式时，验证使用它来跳过未记录的索引，在热备用模式下根本不需要验证任何内容。
 *
 * NB: Caller should call index_checkable() before calling here.
 *
 * 注意：调用者应该在调用这里之前调用index_checkable()。
 */
static bool
amcheck_index_mainfork_expected(Relation rel)
{
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_UNLOGGED ||
		!RecoveryInProgress())
		return true;

	ereport(NOTICE,
			(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
			 errmsg("cannot verify unlogged index \"%s\" during recovery, skipping",
					RelationGetRelationName(rel))));

	return false;
}

/*
* Amcheck main workhorse.
* Given index relation OID, lock relation.
* Next, take a number of standard actions:
* 1) Make sure the index can be checked
* 2) change the context of the user,
* 3) keep track of GUCs modified via index functions
* 4) execute callback function to verify integrity.
*
* Amcheck 的主要主力。给定索引关系OID，锁关系。接下来，采取一些标准操作：1) 确保可以检查索引 2) 更改用户的上下文，3) 跟踪通过索引函数修改的 GUC 4) 执行回调函数以验证完整性。
*/
void
amcheck_lock_relation_and_check(Oid indrelid,
								Oid am_id,
								IndexDoCheckCallback check,
								LOCKMODE lockmode,
								void *state)
{
	Oid			heapid;
	Relation	indrel;
	Relation	heaprel;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	/*
	 * We must lock table before index to avoid deadlocks.  However, if the
	 * passed indrelid isn't an index then IndexGetRelation() will fail.
	 * Rather than emitting a not-very-helpful error message, postpone
	 * complaining, expecting that the is-it-an-index test below will fail.
	 *
	 * 我们必须在索引之前锁定表以避免死锁。  但是，如果传递的 indrelid 不是索引，则 IndexGetRelation() 将失败。不要发出没有多大帮助的错误消息，而是推迟抱怨，期望下面的 is-it-an-index 测试会失败。
	 *
	 * In hot standby mode this will raise an error when parentcheck is true.
	 *
	 * 在热备用模式下，当parentcheck为true时，这将引发错误。
	 */
	heapid = IndexGetRelation(indrelid, true);
	if (OidIsValid(heapid))
	{
		heaprel = table_open(heapid, lockmode);

		/*
		 * Switch to the table owner's userid, so that any index functions are
		 * run as that user.  Also lock down security-restricted operations
		 * and arrange to make GUC variable changes local to this command.
		 *
		 * 切换到表所有者的用户 ID，以便任何索引函数都以该用户身份运行。  还要锁定安全限制操作并安排对此命令进行本地 GUC 变量更改。
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(heaprel->rd_rel->relowner,
							   save_sec_context | SECURITY_RESTRICTED_OPERATION);
		save_nestlevel = NewGUCNestLevel();
	}
	else
	{
		heaprel = NULL;
		/* Set these just to suppress "uninitialized variable" warnings
		 *
		 * 设置这些只是为了抑制“未初始化的变量”警告
		 */
		save_userid = InvalidOid;
		save_sec_context = -1;
		save_nestlevel = -1;
	}

	/*
	 * Open the target index relations separately (like relation_openrv(), but
	 * with heap relation locked first to prevent deadlocking).  In hot
	 * standby mode this will raise an error when parentcheck is true.
	 *
	 * 单独打开目标索引关系（如relation_openrv()，但首先锁定堆关系以防止死锁）。  在热备用模式下，当parentcheck为true时，这将引发错误。
	 *
	 * There is no need for the usual indcheckxmin usability horizon test
	 * here, even in the heapallindexed case, because index undergoing
	 * verification only needs to have entries for a new transaction snapshot.
	 * (If this is a parentcheck verification, there is no question about
	 * committed or recently dead heap tuples lacking index entries due to
	 * concurrent activity.)
	 *
	 * 这里不需要通常的 indcheckxmin 可用性范围测试，即使在 heapallindexed 情况下也是如此，因为正在进行验证的索引只需要有新事务快照的条目。 （如果这是父检查验证，则不存在由于并发活动而导致缺少索引条目的已提交或最近死亡的堆元组的问题。）
	 */
	indrel = index_open(indrelid, lockmode);

	/*
	 * Since we did the IndexGetRelation call above without any lock, it's
	 * barely possible that a race against an index drop/recreation could have
	 * netted us the wrong table.
	 *
	 * 由于我们在没有任何锁的情况下进行了上面的 IndexGetRelation 调用，因此与索引删除/重新创建的竞争几乎不可能为我们带来错误的表。
	 */
	if (heaprel == NULL || heapid != IndexGetRelation(indrelid, false))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open parent table of index \"%s\"",
						RelationGetRelationName(indrel))));

	/* Check that relation suitable for checking
	 *
	 * 检查适合检查的关系
	 */
	if (index_checkable(indrel, am_id))
		check(indrel, heaprel, state, lockmode == ShareLock);

	/* Roll back any GUC changes executed by index functions
	 *
	 * 回滚索引函数执行的任何 GUC 更改
	 */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context
	 *
	 * 恢复用户 ID 和安全上下文
	 */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/*
	 * Release locks early. That's ok here because nothing in the called
	 * routines will trigger shared cache invalidations to be sent, so we can
	 * relax the usual pattern of only releasing locks after commit.
	 *
	 * 尽早释放锁。这里没关系，因为被调用的例程中没有任何内容会触发发送共享缓存失效，因此我们可以放宽仅在提交后释放锁的通常模式。
	 */
	index_close(indrel, lockmode);
	if (heaprel)
		table_close(heaprel, lockmode);
}

/*
 * Basic checks about the suitability of a relation for checking as an index.
 *
 * 关于关系是否适合作为索引进行检查的基本检查。
 *
 *
 * NB: Intentionally not checking permissions, the function is normally not
 * callable by non-superusers. If granted, it's useful to be able to check a
 * whole cluster.
 *
 * 注意：故意不检查权限，非超级用户通常无法调用该函数。如果获得许可，能够检查整个集群会很有用。
 */
bool
index_checkable(Relation rel, Oid am_id)
{
	if (rel->rd_rel->relkind != RELKIND_INDEX ||
		rel->rd_rel->relam != am_id)
	{
		HeapTuple	amtup;
		HeapTuple	amtuprel;

		amtup = SearchSysCache1(AMOID, ObjectIdGetDatum(am_id));
		amtuprel = SearchSysCache1(AMOID, ObjectIdGetDatum(rel->rd_rel->relam));
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("expected \"%s\" index as targets for verification", NameStr(((Form_pg_am) GETSTRUCT(amtup))->amname)),
				 errdetail("Relation \"%s\" is a %s index.",
						   RelationGetRelationName(rel), NameStr(((Form_pg_am) GETSTRUCT(amtuprel))->amname))));
	}

	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions"),
				 errdetail("Index \"%s\" is associated with temporary relation.",
						   RelationGetRelationName(rel))));

	if (!rel->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot check index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail("Index is not valid.")));

	return amcheck_index_mainfork_expected(rel);
}
