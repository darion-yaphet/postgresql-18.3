/*-------------------------------------------------------------------------
 *
 * blvalidate.c
 *	  Opclass validator for bloom.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/bloom/blvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "bloom.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

/*
 * Validator for a bloom opclass.
 *
 * Bloom opclass 的验证器。
 */
bool
blvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	Oid			opckeytype;
	char	   *opclassname;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	int			i;
	ListCell   *lc;

	/* Fetch opclass information
	 *
	 * 获取opclass信息
	 */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opckeytype = classform->opckeytype;
	if (!OidIsValid(opckeytype))
		opckeytype = opcintype;
	opclassname = NameStr(classform->opcname);

	/* Fetch opfamily information
	 *
	 * 获取opfamily信息
	 */
	opfamilyname = get_opfamily_name(opfamilyoid, false);

	/* Fetch all operators and support functions of the opfamily
	 *
	 * 获取opfamily的所有操作符和支持函数
	 */
	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/* Check individual support functions
	 *
	 * 检查个别支持功能
	 */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/*
		 * All bloom support functions should be registered with matching
		 * left/right types
		 *
		 * 所有Bloom支持函数都应使用匹配的左/右类型进行注册
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("bloom opfamily %s contains support procedure %s with cross-type registration",
							opfamilyname,
							format_procedure(procform->amproc))));
			result = false;
		}

		/*
		 * We can't check signatures except within the specific opclass, since
		 * we need to know the associated opckeytype in many cases.
		 *
		 * 我们无法检查除特定 opclass 之外的签名，因为在许多情况下我们需要知道关联的 opckeytype。
		 */
		if (procform->amproclefttype != opcintype)
			continue;

		/* Check procedure numbers and function signatures
		 *
		 * 检查过程号和函数签名
		 */
		switch (procform->amprocnum)
		{
			case BLOOM_HASH_PROC:
				ok = check_amproc_signature(procform->amproc, INT4OID, false,
											1, 1, opckeytype);
				break;
			case BLOOM_OPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("bloom opfamily %s contains function %s with invalid support number %d",
								opfamilyname,
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* don't want additional message */
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("bloom opfamily %s contains function %s with wrong signature for support number %d",
							opfamilyname,
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}
	}

	/* Check individual operators
	 *
	 * 检查个别运营商
	 */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* Check it's allowed strategy for bloom
		 *
		 * 检查允许的绽放策略
		 */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > BLOOM_NSTRATEGIES)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("bloom opfamily %s contains operator %s with invalid strategy number %d",
							opfamilyname,
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* bloom doesn't support ORDER BY operators
		 *
		 * Bloom 不支持 ORDER BY 运算符
		 */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("bloom opfamily %s contains invalid ORDER BY specification for operator %s",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* Check operator signature --- same for all bloom strategies
		 *
		 * 检查操作员签名——所有Bloom策略都相同
		 */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("bloom opfamily %s contains operator %s with wrong signature",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Now check for inconsistent groups of operators/functions
	 *
	 * 现在检查不一致的运算符/函数组
	 */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/* Remember the group exactly matching the test opclass
		 *
		 * 记住与测试 opclass 完全匹配的组
		 */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * There is not a lot we can do to check the operator sets, since each
		 * bloom opclass is more or less a law unto itself, and some contain
		 * only operators that are binary-compatible with the opclass datatype
		 * (meaning that empty operator sets can be OK).  That case also means
		 * that we shouldn't insist on nonempty function sets except for the
		 * opclass's own group.
		 *
		 * 我们可以做的不多来检查操作符集，因为每个Bloom opclass或多或少都是它自己的法则，并且有些只包含与opclass数据类型二进制兼容的操作符（意味着空操作符集可以是好的）。  这种情况也意味着我们不应该坚持使用非空函数集，除了 opclass 自己的组之外。
		 */
	}

	/* Check that the originally-named opclass is complete
	 *
	 * 检查原来命名的opclass是否完整
	 */
	for (i = 1; i <= BLOOM_NPROC; i++)
	{
		if (opclassgroup &&
			(opclassgroup->functionset & (((uint64) 1) << i)) != 0)
			continue;			/* got it */
		if (i == BLOOM_OPTIONS_PROC)
			continue;			/* optional method */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("bloom opclass %s is missing support function %d",
						opclassname, i)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(classtup);

	return result;
}
