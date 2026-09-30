/*-------------------------------------------------------------------------
 *
 * typecmds.c
 *	  Routines for SQL commands that manipulate types (and domains).
 *
 * typecmds.c：处理操纵类型（及 domain）的 SQL 命令的例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/typecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooCreate" routines (in src/backend/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * DESCRIPTION："DefineFoo" 例程从解析树中取出相应参数与标志，交给 src/backend/catalog 中对应的 "FooCreate" 例程去实际修改系统目录。
 * 这些例程也会校验用户是否有权执行该命令。
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send functions
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 * NOTES：这些对象必须按下列顺序定义并提交：
 * "create function"：input/output、recv/send 函数
 * "create type"：类型本身
 * "create operator"：operators
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* result structure for get_rels_with_domain() */
/*
 *
 * get_rels_with_domain() 的结果结构。
 */
typedef struct
{
	Relation	rel;			/* opened and locked relation */
						/*
						 *
						 * 已打开并加锁的关系。
						 */
	int			natts;			/* number of attributes of interest */
							/*
							 *
							 * 相关属性的个数。
							 */
	int		   *atts;			/* attribute numbers */
							/*
							 *
							 * 属性号。
							 */
	/* atts[] is of allocated length RelationGetNumberOfAttributes(rel) */
	/*
	 *
	 * atts[] 的分配长度是 RelationGetNumberOfAttributes(rel)。
	 */
} RelToCheck;

/* parameter structure for AlterTypeRecurse() */
/*
 *
 * AlterTypeRecurse() 的参数结构。
 */
typedef struct
{
	/* Flags indicating which type attributes to update */
	/*
	 *
	 * 标志：指示要更新哪些类型属性。
	 */
	bool		updateStorage;
	bool		updateReceive;
	bool		updateSend;
	bool		updateTypmodin;
	bool		updateTypmodout;
	bool		updateAnalyze;
	bool		updateSubscript;
	/* New values for relevant attributes */
	/*
	 *
	 * 相关属性的新值。
	 */
	char		storage;
	Oid			receiveOid;
	Oid			sendOid;
	Oid			typmodinOid;
	Oid			typmodoutOid;
	Oid			analyzeOid;
	Oid			subscriptOid;
} AlterTypeRecurseParams;

/* Potentially set by pg_upgrade_support functions */
/*
 *
 * 可能由 pg_upgrade_support 函数设置。
 */
Oid			binary_upgrade_next_array_pg_type_oid = InvalidOid;
Oid			binary_upgrade_next_mrng_pg_type_oid = InvalidOid;
Oid			binary_upgrade_next_mrng_array_pg_type_oid = InvalidOid;

static void makeRangeConstructors(const char *name, Oid namespace,
								  Oid rangeOid, Oid subtype);
static void makeMultirangeConstructors(const char *name, Oid namespace,
									   Oid multirangeOid, Oid rangeOid,
									   Oid rangeArrayOid, Oid *castFuncOid);
static Oid	findTypeInputFunction(List *procname, Oid typeOid);
static Oid	findTypeOutputFunction(List *procname, Oid typeOid);
static Oid	findTypeReceiveFunction(List *procname, Oid typeOid);
static Oid	findTypeSendFunction(List *procname, Oid typeOid);
static Oid	findTypeTypmodinFunction(List *procname);
static Oid	findTypeTypmodoutFunction(List *procname);
static Oid	findTypeAnalyzeFunction(List *procname, Oid typeOid);
static Oid	findTypeSubscriptingFunction(List *procname, Oid typeOid);
static Oid	findRangeSubOpclass(List *opcname, Oid subtype);
static Oid	findRangeCanonicalFunction(List *procname, Oid typeOid);
static Oid	findRangeSubtypeDiffFunction(List *procname, Oid subtype);
static void validateDomainCheckConstraint(Oid domainoid, const char *ccbin);
static void validateDomainNotNullConstraint(Oid domainoid);
static List *get_rels_with_domain(Oid domainOid, LOCKMODE lockmode);
static void checkEnumOwner(HeapTuple tup);
static char *domainAddCheckConstraint(Oid domainOid, Oid domainNamespace,
									  Oid baseTypeOid,
									  int typMod, Constraint *constr,
									  const char *domainName, ObjectAddress *constrAddr);
static Node *replace_domain_constraint_value(ParseState *pstate,
											 ColumnRef *cref);
static void domainAddNotNullConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
									   int typMod, Constraint *constr,
									   const char *domainName, ObjectAddress *constrAddr);
static void AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
							 HeapTuple tup, Relation catalog,
							 AlterTypeRecurseParams *atparams);


/*
 * 核心流程：
 * DefineType 处理 CREATE TYPE 基类型：须为超级用户；无参数时只创建 shell type，
 * 否则要求 shell 已存在，解析 I/O、typmod、analyze、subscript 等选项后调用 TypeCreate，
 * 并创建配套数组类型。
 * DefineDomain、DefineEnum、DefineRange 分别登记 domain、enum、range（含 multirange 与构造函数）。
 * AlterDomain* 修改 domain 的默认值、NOT NULL 与约束。
 * RenameType、AlterTypeOwner*、AlterTypeNamespace* 负责重命名、更换属主与切换模式。
 * AlterType 处理 ALTER TYPE SET，AlterTypeRecurse 把可继承属性递归应用到数组类型与 domain。
 */

/*
 * DefineType
 *		Registers a new base type.
 *
 * DefineType：登记一个新的基类型。
 */
ObjectAddress
DefineType(ParseState *pstate, List *names, List *parameters)
{
	char	   *typeName;
	Oid			typeNamespace;
	int16		internalLength = -1;	/* default: variable-length */
						/*
						 *
						 * 默认：变长。
						 */
	List	   *inputName = NIL;
	List	   *outputName = NIL;
	List	   *receiveName = NIL;
	List	   *sendName = NIL;
	List	   *typmodinName = NIL;
	List	   *typmodoutName = NIL;
	List	   *analyzeName = NIL;
	List	   *subscriptName = NIL;
	char		category = TYPCATEGORY_USER;
	bool		preferred = false;
	char		delimiter = DEFAULT_TYPDELIM;
	Oid			elemType = InvalidOid;
	char	   *defaultValue = NULL;
	bool		byValue = false;
	char		alignment = TYPALIGN_INT;	/* default alignment */
							/*
							 *
							 * 默认对齐方式。
							 */
	char		storage = TYPSTORAGE_PLAIN; /* default TOAST storage method */
						    /*
						     *
						     * 默认 TOAST 存储方式。
						     */
	Oid			collation = InvalidOid;
	DefElem    *likeTypeEl = NULL;
	DefElem    *internalLengthEl = NULL;
	DefElem    *inputNameEl = NULL;
	DefElem    *outputNameEl = NULL;
	DefElem    *receiveNameEl = NULL;
	DefElem    *sendNameEl = NULL;
	DefElem    *typmodinNameEl = NULL;
	DefElem    *typmodoutNameEl = NULL;
	DefElem    *analyzeNameEl = NULL;
	DefElem    *subscriptNameEl = NULL;
	DefElem    *categoryEl = NULL;
	DefElem    *preferredEl = NULL;
	DefElem    *delimiterEl = NULL;
	DefElem    *elemTypeEl = NULL;
	DefElem    *defaultValueEl = NULL;
	DefElem    *byValueEl = NULL;
	DefElem    *alignmentEl = NULL;
	DefElem    *storageEl = NULL;
	DefElem    *collatableEl = NULL;
	Oid			inputOid;
	Oid			outputOid;
	Oid			receiveOid = InvalidOid;
	Oid			sendOid = InvalidOid;
	Oid			typmodinOid = InvalidOid;
	Oid			typmodoutOid = InvalidOid;
	Oid			analyzeOid = InvalidOid;
	Oid			subscriptOid = InvalidOid;
	char	   *array_type;
	Oid			array_oid;
	Oid			typoid;
	ListCell   *pl;
	ObjectAddress address;

	/*
	 * As of Postgres 8.4, we require superuser privilege to create a base
	 * type.  This is simple paranoia: there are too many ways to mess up the
	 * system with an incorrect type definition (for instance, representation
	 * parameters that don't match what the C code expects).  In practice it
	 * takes superuser privilege to create the I/O functions, and so the
	 * former requirement that you own the I/O functions pretty much forced
	 * superuserness anyway.  We're just making doubly sure here.
	 *
	 * 自 Postgres 8.4 起，创建基类型需要超级用户权限。这是出于谨慎：错误的类型定义有太多方式破坏系统（例如表示参数与 C 代码预期不符）。
	 * 实践中创建 I/O 函数本身就需要超级用户，因此原先“必须拥有这些 I/O 函数”的要求实质上已经强制了超级用户身份。这里只是再确认一次。
	 *
	 * XXX re-enable NOT_USED code sections below if you remove this test.
	 *
	 * XXX：若移除此检查，请重新启用下面的 NOT_USED 代码段。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create a base type")));

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表转换成名字与命名空间。
	 */
	typeNamespace = QualifiedNameGetCreationNamespace(names, &typeName);

#ifdef NOT_USED
	/* XXX this is unnecessary given the superuser check above */
	/*
	 *
	 * XXX：鉴于上面的超级用户检查，这是多余的。
	 */
	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查我们在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));
#endif

	/*
	 * Look to see if type already exists.
	 *
	 * 查看类型是否已存在。
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 *
	 * 若它不是 shell，则查看它是否为自动生成的数组类型；若是，将其改名挪走。
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/*
	 * If this command is a parameterless CREATE TYPE, then we're just here to
	 * make a shell type, so do that (or fail if there already is a shell).
	 *
	 * 若该命令是不带参数的 CREATE TYPE，则这里只是创建 shell type；若 shell 已存在则失败。
	 */
	if (parameters == NIL)
	{
		if (OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		address = TypeShellMake(typeName, typeNamespace, GetUserId());
		return address;
	}

	/*
	 * Otherwise, we must already have a shell type, since there is no other
	 * way that the I/O functions could have been created.
	 *
	 * 否则，我们必须已经有 shell type，因为 I/O 函数没有别的办法能被创建出来。
	 */
	if (!OidIsValid(typoid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" does not exist", typeName),
				 errhint("Create the type as a shell type, then create its I/O functions, then do a full CREATE TYPE.")));

	/* Extract the parameters from the parameter list */
	/*
	 *
	 * 从参数列表中提取参数。
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "like") == 0)
			defelp = &likeTypeEl;
		else if (strcmp(defel->defname, "internallength") == 0)
			defelp = &internalLengthEl;
		else if (strcmp(defel->defname, "input") == 0)
			defelp = &inputNameEl;
		else if (strcmp(defel->defname, "output") == 0)
			defelp = &outputNameEl;
		else if (strcmp(defel->defname, "receive") == 0)
			defelp = &receiveNameEl;
		else if (strcmp(defel->defname, "send") == 0)
			defelp = &sendNameEl;
		else if (strcmp(defel->defname, "typmod_in") == 0)
			defelp = &typmodinNameEl;
		else if (strcmp(defel->defname, "typmod_out") == 0)
			defelp = &typmodoutNameEl;
		else if (strcmp(defel->defname, "analyze") == 0 ||
				 strcmp(defel->defname, "analyse") == 0)
			defelp = &analyzeNameEl;
		else if (strcmp(defel->defname, "subscript") == 0)
			defelp = &subscriptNameEl;
		else if (strcmp(defel->defname, "category") == 0)
			defelp = &categoryEl;
		else if (strcmp(defel->defname, "preferred") == 0)
			defelp = &preferredEl;
		else if (strcmp(defel->defname, "delimiter") == 0)
			defelp = &delimiterEl;
		else if (strcmp(defel->defname, "element") == 0)
			defelp = &elemTypeEl;
		else if (strcmp(defel->defname, "default") == 0)
			defelp = &defaultValueEl;
		else if (strcmp(defel->defname, "passedbyvalue") == 0)
			defelp = &byValueEl;
		else if (strcmp(defel->defname, "alignment") == 0)
			defelp = &alignmentEl;
		else if (strcmp(defel->defname, "storage") == 0)
			defelp = &storageEl;
		else if (strcmp(defel->defname, "collatable") == 0)
			defelp = &collatableEl;
		else
		{
			/* WARNING, not ERROR, for historical backwards-compatibility */
			/*
			 *
			 * 出于历史上的向后兼容，这里用 WARNING 而不是 ERROR。
			 */
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			continue;
		}
		if (*defelp != NULL)
			errorConflictingDefElem(defel, pstate);
		*defelp = defel;
	}

	/*
	 * Now interpret the options; we do this separately so that LIKE can be
	 * overridden by other options regardless of the ordering in the parameter
	 * list.
	 *
	 * 现在解释这些选项；单独做这一步，是为了让 LIKE 能被其他选项覆盖，而与参数列表中的顺序无关。
	 */
	if (likeTypeEl)
	{
		Type		likeType;
		Form_pg_type likeForm;

		likeType = typenameType(pstate, defGetTypeName(likeTypeEl), NULL);
		likeForm = (Form_pg_type) GETSTRUCT(likeType);
		internalLength = likeForm->typlen;
		byValue = likeForm->typbyval;
		alignment = likeForm->typalign;
		storage = likeForm->typstorage;
		ReleaseSysCache(likeType);
	}
	if (internalLengthEl)
		internalLength = defGetTypeLength(internalLengthEl);
	if (inputNameEl)
		inputName = defGetQualifiedName(inputNameEl);
	if (outputNameEl)
		outputName = defGetQualifiedName(outputNameEl);
	if (receiveNameEl)
		receiveName = defGetQualifiedName(receiveNameEl);
	if (sendNameEl)
		sendName = defGetQualifiedName(sendNameEl);
	if (typmodinNameEl)
		typmodinName = defGetQualifiedName(typmodinNameEl);
	if (typmodoutNameEl)
		typmodoutName = defGetQualifiedName(typmodoutNameEl);
	if (analyzeNameEl)
		analyzeName = defGetQualifiedName(analyzeNameEl);
	if (subscriptNameEl)
		subscriptName = defGetQualifiedName(subscriptNameEl);
	if (categoryEl)
	{
		char	   *p = defGetString(categoryEl);

		category = p[0];
		/* restrict to non-control ASCII */
		/*
		 *
		 * 限制为非控制字符的 ASCII。
		 */
		if (category < 32 || category > 126)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid type category \"%s\": must be simple ASCII",
							p)));
	}
	if (preferredEl)
		preferred = defGetBoolean(preferredEl);
	if (delimiterEl)
	{
		char	   *p = defGetString(delimiterEl);

		delimiter = p[0];
		/* XXX shouldn't we restrict the delimiter? */
		/*
		 *
		 * XXX：难道不该限制分隔符吗？
		 */
	}
	if (elemTypeEl)
	{
		elemType = typenameTypeId(NULL, defGetTypeName(elemTypeEl));
		/* disallow arrays of pseudotypes */
		/*
		 *
		 * 不允许伪类型的数组。
		 */
		if (get_typtype(elemType) == TYPTYPE_PSEUDO)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("array element type cannot be %s",
							format_type_be(elemType))));
	}
	if (defaultValueEl)
		defaultValue = defGetString(defaultValueEl);
	if (byValueEl)
		byValue = defGetBoolean(byValueEl);
	if (alignmentEl)
	{
		char	   *a = defGetString(alignmentEl);

		/*
		 * Note: if argument was an unquoted identifier, parser will have
		 * applied translations to it, so be prepared to recognize translated
		 * type names as well as the nominal form.
		 *
		 * 注意：若参数是未加引号的标识符，解析器会对其做翻译，因此除了名义形式外，还要能识别翻译后的类型名。
		 */
		if (pg_strcasecmp(a, "double") == 0 ||
			pg_strcasecmp(a, "float8") == 0 ||
			pg_strcasecmp(a, "pg_catalog.float8") == 0)
			alignment = TYPALIGN_DOUBLE;
		else if (pg_strcasecmp(a, "int4") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.int4") == 0)
			alignment = TYPALIGN_INT;
		else if (pg_strcasecmp(a, "int2") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.int2") == 0)
			alignment = TYPALIGN_SHORT;
		else if (pg_strcasecmp(a, "char") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.bpchar") == 0)
			alignment = TYPALIGN_CHAR;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("alignment \"%s\" not recognized", a)));
	}
	if (storageEl)
	{
		char	   *a = defGetString(storageEl);

		if (pg_strcasecmp(a, "plain") == 0)
			storage = TYPSTORAGE_PLAIN;
		else if (pg_strcasecmp(a, "external") == 0)
			storage = TYPSTORAGE_EXTERNAL;
		else if (pg_strcasecmp(a, "extended") == 0)
			storage = TYPSTORAGE_EXTENDED;
		else if (pg_strcasecmp(a, "main") == 0)
			storage = TYPSTORAGE_MAIN;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("storage \"%s\" not recognized", a)));
	}
	if (collatableEl)
		collation = defGetBoolean(collatableEl) ? DEFAULT_COLLATION_OID : InvalidOid;

	/*
	 * make sure we have our required definitions
	 *
	 * 确保已具备必需的定义。
	 */
	if (inputName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function must be specified")));
	if (outputName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function must be specified")));

	if (typmodinName == NIL && typmodoutName != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function is useless without a type modifier input function")));

	/*
	 * Convert I/O proc names to OIDs
	 *
	 * 把 I/O 过程名转换成 OID。
	 */
	inputOid = findTypeInputFunction(inputName, typoid);
	outputOid = findTypeOutputFunction(outputName, typoid);
	if (receiveName)
		receiveOid = findTypeReceiveFunction(receiveName, typoid);
	if (sendName)
		sendOid = findTypeSendFunction(sendName, typoid);

	/*
	 * Convert typmodin/out function proc names to OIDs.
	 *
	 * 把 typmodin/typmodout 函数的过程名转换成 OID。
	 */
	if (typmodinName)
		typmodinOid = findTypeTypmodinFunction(typmodinName);
	if (typmodoutName)
		typmodoutOid = findTypeTypmodoutFunction(typmodoutName);

	/*
	 * Convert analysis function proc name to an OID. If no analysis function
	 * is specified, we'll use zero to select the built-in default algorithm.
	 *
	 * 把分析函数的过程名转换成 OID。若未指定分析函数，则用 0 选择内置默认算法。
	 */
	if (analyzeName)
		analyzeOid = findTypeAnalyzeFunction(analyzeName, typoid);

	/*
	 * Likewise look up the subscripting function if any.  If it is not
	 * specified, but a typelem is specified, allow that if
	 * raw_array_subscript_handler can be used.  (This is for backwards
	 * compatibility; maybe someday we should throw an error instead.)
	 *
	 * 同样查找 subscript 函数（如果有）。若未指定，但指定了 typelem，则在可以使用 raw_array_subscript_handler 时允许这种情况。
	 * （这是为了向后兼容；也许将来应改为报错。）
	 */
	if (subscriptName)
		subscriptOid = findTypeSubscriptingFunction(subscriptName, typoid);
	else if (OidIsValid(elemType))
	{
		if (internalLength > 0 && !byValue && get_typlen(elemType) > 0)
			subscriptOid = F_RAW_ARRAY_SUBSCRIPT_HANDLER;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("element type cannot be specified without a subscripting function")));
	}

	/*
	 * Check permissions on functions.  We choose to require the creator/owner
	 * of a type to also own the underlying functions.  Since creating a type
	 * is tantamount to granting public execute access on the functions, the
	 * minimum sane check would be for execute-with-grant-option.  But we
	 * don't have a way to make the type go away if the grant option is
	 * revoked, so ownership seems better.
	 *
	 * 检查函数权限。我们要求类型的创建者/属主同时拥有底层函数。
	 * 创建类型相当于向 public 授予这些函数的执行权，最低限度的合理检查应是带 grant option 的 EXECUTE。
	 * 但我们无法在 grant option 被收回时让类型消失，因此改为要求拥有这些函数。
	 *
	 * XXX For now, this is all unnecessary given the superuser check above.
	 * If we ever relax that, these calls likely should be moved into
	 * findTypeInputFunction et al, where they could be shared by AlterType.
	 *
	 * XXX：鉴于上面的超级用户检查，这些目前都是多余的。
	 * 若将来放宽该检查，这些调用很可能应移入 findTypeInputFunction 等函数，以便 AlterType 共用。
	 */
#ifdef NOT_USED
	if (inputOid && !object_ownercheck(ProcedureRelationId, inputOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(inputName));
	if (outputOid && !object_ownercheck(ProcedureRelationId, outputOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(outputName));
	if (receiveOid && !object_ownercheck(ProcedureRelationId, receiveOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(receiveName));
	if (sendOid && !object_ownercheck(ProcedureRelationId, sendOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(sendName));
	if (typmodinOid && !object_ownercheck(ProcedureRelationId, typmodinOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(typmodinName));
	if (typmodoutOid && !object_ownercheck(ProcedureRelationId, typmodoutOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(typmodoutName));
	if (analyzeOid && !object_ownercheck(ProcedureRelationId, analyzeOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(analyzeName));
	if (subscriptOid && !object_ownercheck(ProcedureRelationId, subscriptOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(subscriptName));
#endif

	/*
	 * OK, we're done checking, time to make the type.  We must assign the
	 * array type OID ahead of calling TypeCreate, since the base type and
	 * array type each refer to the other.
	 *
	 * 检查已完成，可以创建类型了。必须在调用 TypeCreate 之前分配数组类型的 OID，因为基类型与数组类型互相引用。
	 */
	array_oid = AssignTypeArrayOid();

	/*
	 * now have TypeCreate do all the real work.
	 *
	 * 现在让 TypeCreate 完成全部实际工作。
	 *
	 * Note: the pg_type.oid is stored in user tables as array elements (base
	 * types) in ArrayType and in composite types in DatumTupleFields.  This
	 * oid must be preserved by binary upgrades.
	 *
	 * 注意：pg_type.oid 会作为数组元素（基类型）存放在用户表的 ArrayType 中，也会出现在复合类型的 DatumTupleFields 中。
	 * 二进制升级必须保留该 oid。
	 */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
					/*
					 *
					 * 没有预定的类型 OID。
					 */
				   typeName,	/* type name */
						/*
						 *
						 * 类型名。
						 */
				   typeNamespace,	/* namespace */
							/*
							 *
							 * 命名空间。
							 */
				   InvalidOid,	/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
				   0,			/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
				   GetUserId(), /* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
				   internalLength,	/* internal size */
							/*
							 *
							 * 内部长度。
							 */
				   TYPTYPE_BASE,	/* type-type (base type) */
							/*
							 *
							 * 类型种类（基类型）。
							 */
				   category,	/* type-category */
						/*
						 *
						 * 类型类别。
						 */
				   preferred,	/* is it a preferred type? */
						/*
						 *
						 * 是否为首选类型？
						 */
				   delimiter,	/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
				   inputOid,	/* input procedure */
						/*
						 *
						 * input 过程。
						 */
				   outputOid,	/* output procedure */
						/*
						 *
						 * output 过程。
						 */
				   receiveOid,	/* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
				   sendOid,		/* send procedure */
							/*
							 *
							 * send 过程。
							 */
				   typmodinOid, /* typmodin procedure */
						/*
						 *
						 * typmodin 过程。
						 */
				   typmodoutOid,	/* typmodout procedure */
							/*
							 *
							 * typmodout 过程。
							 */
				   analyzeOid,	/* analyze procedure */
						/*
						 *
						 * analyze 过程。
						 */
				   subscriptOid,	/* subscript procedure */
							/*
							 *
							 * subscript 过程。
							 */
				   elemType,	/* element type ID */
						/*
						 *
						 * 元素类型 ID。
						 */
				   false,		/* this is not an implicit array type */
							/*
							 *
							 * 这不是隐式数组类型。
							 */
				   array_oid,	/* array type we are about to create */
						/*
						 *
						 * 即将创建的数组类型。
						 */
				   InvalidOid,	/* base type ID (only for domains) */
						/*
						 *
						 * 基类型 ID（仅用于 domain）。
						 */
				   defaultValue,	/* default type value */
							/*
							 *
							 * 默认类型值。
							 */
				   NULL,		/* no binary form available */
							/*
							 *
							 * 没有可用的二进制形式。
							 */
				   byValue,		/* passed by value */
							/*
							 *
							 * 按值传递。
							 */
				   alignment,	/* required alignment */
						/*
						 *
						 * 所需对齐方式。
						 */
				   storage,		/* TOAST strategy */
							/*
							 *
							 * TOAST 策略。
							 */
				   -1,			/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
				   0,			/* Array Dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
				   false,		/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
				   collation);	/* type's collation */
						/*
						 *
						 * 类型的 collation。
						 */
	Assert(typoid == address.objectId);

	/*
	 * Create the array type that goes with it.
	 *
	 * 创建与之配套的数组类型。
	 */
	array_type = makeArrayTypeName(typeName, typeNamespace);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for arrays */
	/*
	 *
	 * 数组的对齐方式必须是 TYPALIGN_INT 或 TYPALIGN_DOUBLE。
	 */
	alignment = (alignment == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	TypeCreate(array_oid,		/* force assignment of this type OID */
					/*
					 *
					 * 强制分配此类型 OID。
					 */
			   array_type,		/* type name */
						/*
						 *
						 * 类型名。
						 */
			   typeNamespace,	/* namespace */
						/*
						 *
						 * 命名空间。
						 */
			   InvalidOid,		/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
			   0,				/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
			   GetUserId(),		/* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
			   -1,				/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
			   TYPTYPE_BASE,	/* type-type (base type) */
						/*
						 *
						 * 类型种类（基类型）。
						 */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
						/*
						 *
						 * 类型类别（数组）。
						 */
			   false,			/* array types are never preferred */
							/*
							 *
							 * 数组类型从不是首选类型。
							 */
			   delimiter,		/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
			   F_ARRAY_IN,		/* input procedure */
						/*
						 *
						 * input 过程。
						 */
			   F_ARRAY_OUT,		/* output procedure */
						/*
						 *
						 * output 过程。
						 */
			   F_ARRAY_RECV,	/* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
			   F_ARRAY_SEND,	/* send procedure */
						/*
						 *
						 * send 过程。
						 */
			   typmodinOid,		/* typmodin procedure */
						/*
						 *
						 * typmodin 过程。
						 */
			   typmodoutOid,	/* typmodout procedure */
						/*
						 *
						 * typmodout 过程。
						 */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
						/*
						 *
						 * analyze 过程。
						 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
							/*
							 *
							 * 数组 subscript 过程。
							 */
			   typoid,			/* element type ID */
							/*
							 *
							 * 元素类型 ID。
							 */
			   true,			/* yes this is an array type */
							/*
							 *
							 * 是的，这是数组类型。
							 */
			   InvalidOid,		/* no further array type */
						/*
						 *
						 * 没有更外层的数组类型。
						 */
			   InvalidOid,		/* base type ID */
						/*
						 *
						 * 基类型 ID。
						 */
			   NULL,			/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
			   NULL,			/* binary default isn't sent either */
							/*
							 *
							 * 同样不发送二进制默认值。
							 */
			   false,			/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
			   alignment,		/* see above */
						/*
						 *
						 * 见上文。
						 */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
						/*
						 *
						 * ARRAY 总是可以被 toast。
						 */
			   -1,				/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
			   0,				/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
			   false,			/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
			   collation);		/* type's collation */
						/*
						 *
						 * 类型的 collation。
						 */

	pfree(array_type);

	return address;
}

/*
 * Guts of type deletion.
 *
 * 类型删除的核心实现。
 */
void
RemoveTypeById(Oid typeOid)
{
	Relation	relation;
	HeapTuple	tup;

	relation = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	CatalogTupleDelete(relation, &tup->t_self);

	/*
	 * If it is an enum, delete the pg_enum entries too; we don't bother with
	 * making dependency entries for those, so it has to be done "by hand"
	 * here.
	 *
	 * 若它是 enum，也删除 pg_enum 项；我们没有为那些项建立依赖，因此必须在这里手工删除。
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_ENUM)
		EnumValuesDelete(typeOid);

	/*
	 * If it is a range type, delete the pg_range entry too; we don't bother
	 * with making a dependency entry for that, so it has to be done "by hand"
	 * here.
	 *
	 * 若它是 range 类型，也删除 pg_range 项；我们没有为该项建立依赖，因此必须在这里手工删除。
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_RANGE)
		RangeDelete(typeOid);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}


/*
 * DefineDomain
 *		Registers a new domain.
 *
 * DefineDomain：登记一个新的 domain。
 */
ObjectAddress
DefineDomain(ParseState *pstate, CreateDomainStmt *stmt)
{
	char	   *domainName;
	char	   *domainArrayName;
	Oid			domainNamespace;
	AclResult	aclresult;
	int16		internalLength;
	Oid			inputProcedure;
	Oid			outputProcedure;
	Oid			receiveProcedure;
	Oid			sendProcedure;
	Oid			analyzeProcedure;
	bool		byValue;
	char		category;
	char		delimiter;
	char		alignment;
	char		storage;
	char		typtype;
	Datum		datum;
	bool		isnull;
	char	   *defaultValue = NULL;
	char	   *defaultValueBin = NULL;
	bool		saw_default = false;
	bool		typNotNull = false;
	bool		nullDefined = false;
	int32		typNDims = list_length(stmt->typeName->arrayBounds);
	HeapTuple	typeTup;
	List	   *schema = stmt->constraints;
	ListCell   *listptr;
	Oid			basetypeoid;
	Oid			old_type_oid;
	Oid			domaincoll;
	Oid			domainArrayOid;
	Form_pg_type baseType;
	int32		basetypeMod;
	Oid			baseColl;
	ObjectAddress address;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表转换成名字与命名空间。
	 */
	domainNamespace = QualifiedNameGetCreationNamespace(stmt->domainname,
														&domainName);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查我们在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, domainNamespace, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(domainNamespace));

	/*
	 * Check for collision with an existing type name.  If there is one and
	 * it's an autogenerated array, we can rename it out of the way.
	 *
	 * 检查是否与已有类型名冲突。若冲突对象是自动生成的数组类型，可以把它改名挪走。
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(domainName),
								   ObjectIdGetDatum(domainNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, domainName, domainNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", domainName)));
	}

	/*
	 * Look up the base type.
	 *
	 * 查找基类型。
	 */
	typeTup = typenameType(pstate, stmt->typeName, &basetypeMod);
	baseType = (Form_pg_type) GETSTRUCT(typeTup);
	basetypeoid = baseType->oid;

	/*
	 * Base type must be a plain base type, a composite type, another domain,
	 * an enum or a range type.  Domains over pseudotypes would create a
	 * security hole.  (It would be shorter to code this to just check for
	 * pseudotypes; but it seems safer to call out the specific typtypes that
	 * are supported, rather than assume that all future typtypes would be
	 * automatically supported.)
	 *
	 * 基类型必须是普通基类型、复合类型、另一个 domain、enum 或 range。
	 * 在伪类型上建立 domain 会形成安全漏洞。（只检查伪类型会更短，但显式列出支持的 typtype 更安全，以免假定未来所有 typtype 都会自动支持。）
	 */
	typtype = baseType->typtype;
	if (typtype != TYPTYPE_BASE &&
		typtype != TYPTYPE_COMPOSITE &&
		typtype != TYPTYPE_DOMAIN &&
		typtype != TYPTYPE_ENUM &&
		typtype != TYPTYPE_RANGE &&
		typtype != TYPTYPE_MULTIRANGE)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("\"%s\" is not a valid base type for a domain",
						TypeNameToString(stmt->typeName)),
				 parser_errposition(pstate, stmt->typeName->location)));

	aclresult = object_aclcheck(TypeRelationId, basetypeoid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, basetypeoid);

	/*
	 * Collect the properties of the new domain.  Some are inherited from the
	 * base type, some are not.  If you change any of this inheritance
	 * behavior, be sure to update AlterTypeRecurse() to match!
	 *
	 * 收集新 domain 的属性。有些从基类型继承，有些不继承。
	 * 若更改任何继承行为，务必同步修改 AlterTypeRecurse()。
	 */

	/*
	 * Identify the collation if any
	 *
	 * 确定 collation（如果有）。
	 */
	baseColl = baseType->typcollation;
	if (stmt->collClause)
		domaincoll = get_collation_oid(stmt->collClause->collname, false);
	else
		domaincoll = baseColl;

	/* Complain if COLLATE is applied to an uncollatable type */
	/*
	 *
	 * 若对不支持 collation 的类型使用 COLLATE，则报错。
	 */
	if (OidIsValid(domaincoll) && !OidIsValid(baseColl))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(basetypeoid)),
				 parser_errposition(pstate, stmt->typeName->location)));

	/* passed by value */
	/*
	 *
	 * 按值传递。
	 */
	byValue = baseType->typbyval;

	/* Required Alignment */
	/*
	 *
	 * 所需对齐方式。
	 */
	alignment = baseType->typalign;

	/* TOAST Strategy */
	/*
	 *
	 * TOAST 策略。
	 */
	storage = baseType->typstorage;

	/* Storage Length */
	/*
	 *
	 * 存储长度。
	 */
	internalLength = baseType->typlen;

	/* Type Category */
	/*
	 *
	 * 类型类别。
	 */
	category = baseType->typcategory;

	/* Array element Delimiter */
	/*
	 *
	 * 数组元素分隔符。
	 */
	delimiter = baseType->typdelim;

	/* I/O Functions */
	/*
	 *
	 * I/O 函数。
	 */
	inputProcedure = F_DOMAIN_IN;
	outputProcedure = baseType->typoutput;
	receiveProcedure = F_DOMAIN_RECV;
	sendProcedure = baseType->typsend;

	/* Domains never accept typmods, so no typmodin/typmodout needed */
	/*
	 *
	 * domain 从不接受 typmod，因此不需要 typmodin/typmodout。
	 */

	/* Analysis function */
	/*
	 *
	 * 分析函数。
	 */
	analyzeProcedure = baseType->typanalyze;

	/*
	 * Domains don't need a subscript function, since they are not
	 * subscriptable on their own.  If the base type is subscriptable, the
	 * parser will reduce the type to the base type before subscripting.
	 *
	 * domain 不需要 subscript 函数，因为它们自身不能被下标访问。
	 * 若基类型可下标访问，解析器会先把类型归约为基类型再做下标运算。
	 */

	/* Inherited default value */
	/*
	 *
	 * 继承的默认值。
	 */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefault, &isnull);
	if (!isnull)
		defaultValue = TextDatumGetCString(datum);

	/* Inherited default binary value */
	/*
	 *
	 * 继承的二进制默认值。
	 */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefaultbin, &isnull);
	if (!isnull)
		defaultValueBin = TextDatumGetCString(datum);

	/*
	 * Run through constraints manually to avoid the additional processing
	 * conducted by DefineRelation() and friends.
	 *
	 * 手工遍历约束，以避免 DefineRelation() 及其相关函数所做的额外处理。
	 */
	foreach(listptr, schema)
	{
		Constraint *constr = lfirst(listptr);

		if (!IsA(constr, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(constr));
		switch (constr->contype)
		{
			case CONSTR_DEFAULT:

				/*
				 * The inherited default value may be overridden by the user
				 * with the DEFAULT <expr> clause ... but only once.
				 *
				 * 用户可以用 DEFAULT 表达式子句覆盖继承的默认值，但只能覆盖一次。
				 */
				if (saw_default)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("multiple default expressions"),
							parser_errposition(pstate, constr->location));
				saw_default = true;

				if (constr->raw_expr)
				{
					Node	   *defaultExpr;

					/*
					 * Cook the constr->raw_expr into an expression. Note:
					 * name is strictly for error message
					 *
					 * 把 constr->raw_expr 编译成表达式。注意：name 仅用于错误信息。
					 */
					defaultExpr = cookDefault(pstate, constr->raw_expr,
											  basetypeoid,
											  basetypeMod,
											  domainName,
											  0);

					/*
					 * If the expression is just a NULL constant, we treat it
					 * like not having a default.
					 *
					 * 若表达式只是 NULL 常量，则当作没有默认值。
					 *
					 * Note that if the basetype is another domain, we'll see
					 * a CoerceToDomain expr here and not discard the default.
					 * This is critical because the domain default needs to be
					 * retained to override any default that the base domain
					 * might have.
					 *
					 * 注意：若基类型是另一个 domain，这里会看到 CoerceToDomain 表达式，此时不能丢弃默认值。
					 * 这一点很关键，因为必须保留该 domain 的默认值，以覆盖基 domain 可能具有的默认值。
					 */
					if (defaultExpr == NULL ||
						(IsA(defaultExpr, Const) &&
						 ((Const *) defaultExpr)->constisnull))
					{
						defaultValue = NULL;
						defaultValueBin = NULL;
					}
					else
					{
						/*
						 * Expression must be stored as a nodeToString result,
						 * but we also require a valid textual representation
						 * (mainly to make life easier for pg_dump).
						 *
						 * 表达式必须以 nodeToString 的结果存储，同时还需要一份有效的文本表示（主要是为了方便 pg_dump）。
						 */
						defaultValue =
							deparse_expression(defaultExpr,
											   NIL, false, false);
						defaultValueBin = nodeToString(defaultExpr);
					}
				}
				else
				{
					/* No default (can this still happen?) */
					/*
					 *
					 * 没有默认值（这种情况还会发生吗？）
					 */
					defaultValue = NULL;
					defaultValueBin = NULL;
				}
				break;

			case CONSTR_NOTNULL:
				if (nullDefined)
				{
					if (!typNotNull)
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("conflicting NULL/NOT NULL constraints"),
								parser_errposition(pstate, constr->location));

					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("redundant NOT NULL constraint definition"),
							parser_errposition(pstate, constr->location));
				}
				if (constr->is_no_inherit)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("not-null constraints for domains cannot be marked NO INHERIT"),
							parser_errposition(pstate, constr->location));
				typNotNull = true;
				nullDefined = true;
				break;

			case CONSTR_NULL:
				if (nullDefined && typNotNull)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("conflicting NULL/NOT NULL constraints"),
							parser_errposition(pstate, constr->location));
				typNotNull = false;
				nullDefined = true;
				break;

			case CONSTR_CHECK:

				/*
				 * Check constraints are handled after domain creation, as
				 * they require the Oid of the domain; at this point we can
				 * only check that they're not marked NO INHERIT, because that
				 * would be bogus.
				 *
				 * CHECK 约束在 domain 创建之后处理，因为它们需要 domain 的 OID；
				 * 此时只能检查它们没有被标成 NO INHERIT，否则该标记没有意义。
				 */
				if (constr->is_no_inherit)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("check constraints for domains cannot be marked NO INHERIT"),
							parser_errposition(pstate, constr->location));

				break;

				/*
				 * All else are error cases
				 *
				 * 其余情况一律报错。
				 */
			case CONSTR_UNIQUE:
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("unique constraints not possible for domains"),
						parser_errposition(pstate, constr->location));
				break;

			case CONSTR_PRIMARY:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("primary key constraints not possible for domains"),
						 parser_errposition(pstate, constr->location)));
				break;

			case CONSTR_EXCLUSION:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("exclusion constraints not possible for domains"),
						 parser_errposition(pstate, constr->location)));
				break;

			case CONSTR_FOREIGN:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("foreign key constraints not possible for domains"),
						 parser_errposition(pstate, constr->location)));
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("specifying constraint deferrability not supported for domains"),
						 parser_errposition(pstate, constr->location)));
				break;

			case CONSTR_GENERATED:
			case CONSTR_IDENTITY:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("specifying GENERATED not supported for domains"),
						 parser_errposition(pstate, constr->location)));
				break;

			case CONSTR_ATTR_ENFORCED:
			case CONSTR_ATTR_NOT_ENFORCED:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("specifying constraint enforceability not supported for domains"),
						 parser_errposition(pstate, constr->location)));
				break;

				/* no default, to let compiler warn about missing case */
				/*
				 *
				 * 不设 default，以便编译器对遗漏的 case 发出警告。
				 */
		}
	}

	/* Allocate OID for array type */
	/*
	 *
	 * 为数组类型分配 OID。
	 */
	domainArrayOid = AssignTypeArrayOid();

	/*
	 * Have TypeCreate do all the real work.
	 *
	 * 让 TypeCreate 完成全部实际工作。
	 */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
					/*
					 *
					 * 没有预定的类型 OID。
					 */
				   domainName,	/* type name */
						/*
						 *
						 * 类型名。
						 */
				   domainNamespace, /* namespace */
						    /*
						     *
						     * 命名空间。
						     */
				   InvalidOid,	/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
				   0,			/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
				   GetUserId(), /* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
				   internalLength,	/* internal size */
							/*
							 *
							 * 内部长度。
							 */
				   TYPTYPE_DOMAIN,	/* type-type (domain type) */
							/*
							 *
							 * 类型种类（domain 类型）。
							 */
				   category,	/* type-category */
						/*
						 *
						 * 类型类别。
						 */
				   false,		/* domain types are never preferred */
							/*
							 *
							 * domain 类型从不是首选类型。
							 */
				   delimiter,	/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
				   inputProcedure,	/* input procedure */
							/*
							 *
							 * input 过程。
							 */
				   outputProcedure, /* output procedure */
						    /*
						     *
						     * output 过程。
						     */
				   receiveProcedure,	/* receive procedure */
							/*
							 *
							 * receive 过程。
							 */
				   sendProcedure,	/* send procedure */
							/*
							 *
							 * send 过程。
							 */
				   InvalidOid,	/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
				   InvalidOid,	/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
				   analyzeProcedure,	/* analyze procedure */
							/*
							 *
							 * analyze 过程。
							 */
				   InvalidOid,	/* subscript procedure - none */
						/*
						 *
						 * 没有 subscript 过程。
						 */
				   InvalidOid,	/* no array element type */
						/*
						 *
						 * 没有数组元素类型。
						 */
				   false,		/* this isn't an array */
							/*
							 *
							 * 这不是数组。
							 */
				   domainArrayOid,	/* array type we are about to create */
							/*
							 *
							 * 即将创建的数组类型。
							 */
				   basetypeoid, /* base type ID */
						/*
						 *
						 * 基类型 ID。
						 */
				   defaultValue,	/* default type value (text) */
							/*
							 *
							 * 默认类型值（文本）。
							 */
				   defaultValueBin, /* default type value (binary) */
						    /*
						     *
						     * 默认类型值（二进制）。
						     */
				   byValue,		/* passed by value */
							/*
							 *
							 * 按值传递。
							 */
				   alignment,	/* required alignment */
						/*
						 *
						 * 所需对齐方式。
						 */
				   storage,		/* TOAST strategy */
							/*
							 *
							 * TOAST 策略。
							 */
				   basetypeMod, /* typeMod value */
						/*
						 *
						 * typeMod 值。
						 */
				   typNDims,	/* Array dimensions for base type */
						/*
						 *
						 * 基类型的数组维数。
						 */
				   typNotNull,	/* Type NOT NULL */
						/*
						 *
						 * 类型为 NOT NULL。
						 */
				   domaincoll); /* type's collation */
						/*
						 *
						 * 类型的 collation。
						 */

	/*
	 * Create the array type that goes with it.
	 *
	 * 创建与之配套的数组类型。
	 */
	domainArrayName = makeArrayTypeName(domainName, domainNamespace);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for arrays */
	/*
	 *
	 * 数组的对齐方式必须是 TYPALIGN_INT 或 TYPALIGN_DOUBLE。
	 */
	alignment = (alignment == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	TypeCreate(domainArrayOid,	/* force assignment of this type OID */
					/*
					 *
					 * 强制分配此类型 OID。
					 */
			   domainArrayName, /* type name */
					    /*
					     *
					     * 类型名。
					     */
			   domainNamespace, /* namespace */
					    /*
					     *
					     * 命名空间。
					     */
			   InvalidOid,		/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
			   0,				/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
			   GetUserId(),		/* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
			   -1,				/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
			   TYPTYPE_BASE,	/* type-type (base type) */
						/*
						 *
						 * 类型种类（基类型）。
						 */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
						/*
						 *
						 * 类型类别（数组）。
						 */
			   false,			/* array types are never preferred */
							/*
							 *
							 * 数组类型从不是首选类型。
							 */
			   delimiter,		/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
			   F_ARRAY_IN,		/* input procedure */
						/*
						 *
						 * input 过程。
						 */
			   F_ARRAY_OUT,		/* output procedure */
						/*
						 *
						 * output 过程。
						 */
			   F_ARRAY_RECV,	/* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
			   F_ARRAY_SEND,	/* send procedure */
						/*
						 *
						 * send 过程。
						 */
			   InvalidOid,		/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
			   InvalidOid,		/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
						/*
						 *
						 * analyze 过程。
						 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
							/*
							 *
							 * 数组 subscript 过程。
							 */
			   address.objectId,	/* element type ID */
						/*
						 *
						 * 元素类型 ID。
						 */
			   true,			/* yes this is an array type */
							/*
							 *
							 * 是的，这是数组类型。
							 */
			   InvalidOid,		/* no further array type */
						/*
						 *
						 * 没有更外层的数组类型。
						 */
			   InvalidOid,		/* base type ID */
						/*
						 *
						 * 基类型 ID。
						 */
			   NULL,			/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
			   NULL,			/* binary default isn't sent either */
							/*
							 *
							 * 同样不发送二进制默认值。
							 */
			   false,			/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
			   alignment,		/* see above */
						/*
						 *
						 * 见上文。
						 */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
						/*
						 *
						 * ARRAY 总是可以被 toast。
						 */
			   -1,				/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
			   0,				/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
			   false,			/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
			   domaincoll);		/* type's collation */
						/*
						 *
						 * 类型的 collation。
						 */

	pfree(domainArrayName);

	/*
	 * Process constraints which refer to the domain ID returned by TypeCreate
	 *
	 * 处理引用 TypeCreate 返回的 domain ID 的约束。
	 */
	foreach(listptr, schema)
	{
		Constraint *constr = lfirst(listptr);

		/* it must be a Constraint, per check above */
		/*
		 *
		 * 根据上面的检查，它必须是 Constraint。
		 */

		switch (constr->contype)
		{
			case CONSTR_CHECK:
				domainAddCheckConstraint(address.objectId, domainNamespace,
										 basetypeoid, basetypeMod,
										 constr, domainName, NULL);
				break;

			case CONSTR_NOTNULL:
				domainAddNotNullConstraint(address.objectId, domainNamespace,
										   basetypeoid, basetypeMod,
										   constr, domainName, NULL);
				break;

				/* Other constraint types were fully processed above */
				/*
				 *
				 * 其他约束类型已在上面完整处理。
				 */

			default:
				break;
		}

		/* CCI so we can detect duplicate constraint names */
		/*
		 *
		 * 执行 CommandCounterIncrement（CCI），以便检测重复的约束名。
		 */
		CommandCounterIncrement();
	}

	/*
	 * Now we can clean up.
	 *
	 * 现在可以清理了。
	 */
	ReleaseSysCache(typeTup);

	return address;
}


/*
 * DefineEnum
 *		Registers a new enum.
 *
 * DefineEnum：登记一个新的 enum。
 */
ObjectAddress
DefineEnum(CreateEnumStmt *stmt)
{
	char	   *enumName;
	char	   *enumArrayName;
	Oid			enumNamespace;
	AclResult	aclresult;
	Oid			old_type_oid;
	Oid			enumArrayOid;
	ObjectAddress enumTypeAddr;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表转换成名字与命名空间。
	 */
	enumNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &enumName);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查我们在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, enumNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(enumNamespace));

	/*
	 * Check for collision with an existing type name.  If there is one and
	 * it's an autogenerated array, we can rename it out of the way.
	 *
	 * 检查是否与已有类型名冲突。若冲突对象是自动生成的数组类型，可以把它改名挪走。
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(enumName),
								   ObjectIdGetDatum(enumNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, enumName, enumNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", enumName)));
	}

	/* Allocate OID for array type */
	/*
	 *
	 * 为数组类型分配 OID。
	 */
	enumArrayOid = AssignTypeArrayOid();

	/* Create the pg_type entry */
	/*
	 *
	 * 创建 pg_type 项。
	 */
	enumTypeAddr =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
					/*
					 *
					 * 没有预定的类型 OID。
					 */
				   enumName,	/* type name */
						/*
						 *
						 * 类型名。
						 */
				   enumNamespace,	/* namespace */
							/*
							 *
							 * 命名空间。
							 */
				   InvalidOid,	/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
				   0,			/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
				   GetUserId(), /* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
				   sizeof(Oid), /* internal size */
						/*
						 *
						 * 内部长度。
						 */
				   TYPTYPE_ENUM,	/* type-type (enum type) */
							/*
							 *
							 * 类型种类（enum 类型）。
							 */
				   TYPCATEGORY_ENUM,	/* type-category (enum type) */
							/*
							 *
							 * 类型类别（enum 类型）。
							 */
				   false,		/* enum types are never preferred */
							/*
							 *
							 * enum 类型从不是首选类型。
							 */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
							/*
							 *
							 * 数组元素分隔符。
							 */
				   F_ENUM_IN,	/* input procedure */
						/*
						 *
						 * input 过程。
						 */
				   F_ENUM_OUT,	/* output procedure */
						/*
						 *
						 * output 过程。
						 */
				   F_ENUM_RECV, /* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
				   F_ENUM_SEND, /* send procedure */
						/*
						 *
						 * send 过程。
						 */
				   InvalidOid,	/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
				   InvalidOid,	/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
				   InvalidOid,	/* analyze procedure - default */
						/*
						 *
						 * analyze 过程，使用默认。
						 */
				   InvalidOid,	/* subscript procedure - none */
						/*
						 *
						 * 没有 subscript 过程。
						 */
				   InvalidOid,	/* element type ID */
						/*
						 *
						 * 元素类型 ID。
						 */
				   false,		/* this is not an array type */
							/*
							 *
							 * 这不是数组类型。
							 */
				   enumArrayOid,	/* array type we are about to create */
							/*
							 *
							 * 即将创建的数组类型。
							 */
				   InvalidOid,	/* base type ID (only for domains) */
						/*
						 *
						 * 基类型 ID（仅用于 domain）。
						 */
				   NULL,		/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
				   NULL,		/* binary default isn't sent either */
							/*
							 *
							 * 同样不发送二进制默认值。
							 */
				   true,		/* always passed by value */
							/*
							 *
							 * 始终按值传递。
							 */
				   TYPALIGN_INT,	/* int alignment */
							/*
							 *
							 * int 对齐。
							 */
				   TYPSTORAGE_PLAIN,	/* TOAST strategy always plain */
							/*
							 *
							 * TOAST 策略始终为 plain。
							 */
				   -1,			/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
				   0,			/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
				   false,		/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
				   InvalidOid); /* type's collation */
						/*
						 *
						 * 类型的 collation。
						 */

	/* Enter the enum's values into pg_enum */
	/*
	 *
	 * 把 enum 的取值写入 pg_enum。
	 */
	EnumValuesCreate(enumTypeAddr.objectId, stmt->vals);

	/*
	 * Create the array type that goes with it.
	 *
	 * 创建与之配套的数组类型。
	 */
	enumArrayName = makeArrayTypeName(enumName, enumNamespace);

	TypeCreate(enumArrayOid,	/* force assignment of this type OID */
					/*
					 *
					 * 强制分配此类型 OID。
					 */
			   enumArrayName,	/* type name */
						/*
						 *
						 * 类型名。
						 */
			   enumNamespace,	/* namespace */
						/*
						 *
						 * 命名空间。
						 */
			   InvalidOid,		/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
			   0,				/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
			   GetUserId(),		/* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
			   -1,				/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
			   TYPTYPE_BASE,	/* type-type (base type) */
						/*
						 *
						 * 类型种类（基类型）。
						 */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
						/*
						 *
						 * 类型类别（数组）。
						 */
			   false,			/* array types are never preferred */
							/*
							 *
							 * 数组类型从不是首选类型。
							 */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
			   F_ARRAY_IN,		/* input procedure */
						/*
						 *
						 * input 过程。
						 */
			   F_ARRAY_OUT,		/* output procedure */
						/*
						 *
						 * output 过程。
						 */
			   F_ARRAY_RECV,	/* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
			   F_ARRAY_SEND,	/* send procedure */
						/*
						 *
						 * send 过程。
						 */
			   InvalidOid,		/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
			   InvalidOid,		/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
						/*
						 *
						 * analyze 过程。
						 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
							/*
							 *
							 * 数组 subscript 过程。
							 */
			   enumTypeAddr.objectId,	/* element type ID */
							/*
							 *
							 * 元素类型 ID。
							 */
			   true,			/* yes this is an array type */
							/*
							 *
							 * 是的，这是数组类型。
							 */
			   InvalidOid,		/* no further array type */
						/*
						 *
						 * 没有更外层的数组类型。
						 */
			   InvalidOid,		/* base type ID */
						/*
						 *
						 * 基类型 ID。
						 */
			   NULL,			/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
			   NULL,			/* binary default isn't sent either */
							/*
							 *
							 * 同样不发送二进制默认值。
							 */
			   false,			/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
			   TYPALIGN_INT,	/* enums have int align, so do their arrays */
						/*
						 *
						 * enum 按 int 对齐，其数组也是。
						 */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
						/*
						 *
						 * ARRAY 总是可以被 toast。
						 */
			   -1,				/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
			   0,				/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
			   false,			/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
			   InvalidOid);		/* type's collation */
						/*
						 *
						 * 类型的 collation。
						 */

	pfree(enumArrayName);

	return enumTypeAddr;
}

/*
 * AlterEnum
 *		Adds a new label to an existing enum.
 *
 * AlterEnum：向已有 enum 添加新标签。
 */
ObjectAddress
AlterEnum(AlterEnumStmt *stmt)
{
	Oid			enum_type_oid;
	TypeName   *typename;
	HeapTuple	tup;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(stmt->typeName);
	enum_type_oid = typenameTypeId(NULL, typename);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(enum_type_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", enum_type_oid);

	/* Check it's an enum and check user has permission to ALTER the enum */
	/*
	 *
	 * 确认它是 enum，并检查用户是否有权 ALTER 该 enum。
	 */
	checkEnumOwner(tup);

	ReleaseSysCache(tup);

	if (stmt->oldVal)
	{
		/* Rename an existing label */
		/*
		 *
		 * 重命名已有标签。
		 */
		RenameEnumLabel(enum_type_oid, stmt->oldVal, stmt->newVal);
	}
	else
	{
		/* Add a new label */
		/*
		 *
		 * 添加一个新的标签。
		 */
		AddEnumLabel(enum_type_oid, stmt->newVal,
					 stmt->newValNeighbor, stmt->newValIsAfter,
					 stmt->skipIfNewValExists);
	}

	InvokeObjectPostAlterHook(TypeRelationId, enum_type_oid, 0);

	ObjectAddressSet(address, TypeRelationId, enum_type_oid);

	return address;
}


/*
 * checkEnumOwner
 *
 * 函数 checkEnumOwner。
 *
 * Check that the type is actually an enum and that the current user
 * has permission to do ALTER TYPE on it.  Throw an error if not.
 *
 * 确认该类型确实是 enum，且当前用户有权对其执行 ALTER TYPE。否则报错。
 */
static void
checkEnumOwner(HeapTuple tup)
{
	Form_pg_type typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check that this is actually an enum */
	/*
	 *
	 * 确认这确实是 enum。
	 */
	if (typTup->typtype != TYPTYPE_ENUM)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not an enum",
						format_type_be(typTup->oid))));

	/* Permission check: must own type */
	/*
	 *
	 * 权限检查：必须拥有该类型。
	 */
	if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);
}


/*
 * DefineRange
 *		Registers a new range type.
 *
 * DefineRange：登记一个新的 range 类型。
 *
 * Perhaps it might be worthwhile to set pg_type.typelem to the base type,
 * and likewise on multiranges to set it to the range type. But having a
 * non-zero typelem is treated elsewhere as a synonym for being an array,
 * and users might have queries with that same assumption.
 *
 * 也许值得把 pg_type.typelem 设为基类型，并同样把 multirange 的 typelem 设为 range 类型。
 * 但非零的 typelem 在别处被当作“这是数组”的同义词，用户查询也可能有同样假设。
 */
ObjectAddress
DefineRange(ParseState *pstate, CreateRangeStmt *stmt)
{
	char	   *typeName;
	Oid			typeNamespace;
	Oid			typoid;
	char	   *rangeArrayName;
	char	   *multirangeTypeName = NULL;
	char	   *multirangeArrayName;
	Oid			multirangeNamespace = InvalidOid;
	Oid			rangeArrayOid;
	Oid			multirangeOid;
	Oid			multirangeArrayOid;
	Oid			rangeSubtype = InvalidOid;
	List	   *rangeSubOpclassName = NIL;
	List	   *rangeCollationName = NIL;
	List	   *rangeCanonicalName = NIL;
	List	   *rangeSubtypeDiffName = NIL;
	Oid			rangeSubOpclass;
	Oid			rangeCollation;
	regproc		rangeCanonical;
	regproc		rangeSubtypeDiff;
	int16		subtyplen;
	bool		subtypbyval;
	char		subtypalign;
	char		alignment;
	AclResult	aclresult;
	ListCell   *lc;
	ObjectAddress address;
	ObjectAddress mltrngaddress PG_USED_FOR_ASSERTS_ONLY;
	Oid			castFuncOid;

	/* Convert list of names to a name and namespace */
	/*
	 *
	 * 把名字列表转换成名字与命名空间。
	 */
	typeNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &typeName);

	/* Check we have creation rights in target namespace */
	/*
	 *
	 * 检查我们在目标命名空间中是否有创建权限。
	 */
	aclresult = object_aclcheck(NamespaceRelationId, typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));

	/*
	 * Look to see if type already exists.
	 *
	 * 查看类型是否已存在。
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/*
	 * If it's not a shell, see if it's an autogenerated array type, and if so
	 * rename it out of the way.
	 *
	 * 若它不是 shell，则查看它是否为自动生成的数组类型；若是，将其改名挪走。
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/*
	 * Unlike DefineType(), we don't insist on a shell type existing first, as
	 * it's only needed if the user wants to specify a canonical function.
	 *
	 * 与 DefineType() 不同，这里不坚持必须先有 shell type，因为只有用户要指定 canonical 函数时才需要它。
	 */

	/* Extract the parameters from the parameter list */
	/*
	 *
	 * 从参数列表中提取参数。
	 */
	foreach(lc, stmt->params)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "subtype") == 0)
		{
			if (OidIsValid(rangeSubtype))
				errorConflictingDefElem(defel, pstate);
			/* we can look up the subtype name immediately */
			/*
			 *
			 * 可以立即查找子类型名。
			 */
			rangeSubtype = typenameTypeId(NULL, defGetTypeName(defel));
		}
		else if (strcmp(defel->defname, "subtype_opclass") == 0)
		{
			if (rangeSubOpclassName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeSubOpclassName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "collation") == 0)
		{
			if (rangeCollationName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeCollationName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "canonical") == 0)
		{
			if (rangeCanonicalName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeCanonicalName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "subtype_diff") == 0)
		{
			if (rangeSubtypeDiffName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeSubtypeDiffName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "multirange_type_name") == 0)
		{
			if (multirangeTypeName != NULL)
				errorConflictingDefElem(defel, pstate);
			/* we can look up the subtype name immediately */
			/*
			 *
			 * 可以立即查找子类型名。
			 */
			multirangeNamespace = QualifiedNameGetCreationNamespace(defGetQualifiedName(defel),
																	&multirangeTypeName);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* Must have a subtype */
	/*
	 *
	 * 必须指定子类型。
	 */
	if (!OidIsValid(rangeSubtype))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type attribute \"subtype\" is required")));
	/* disallow ranges of pseudotypes */
	/*
	 *
	 * 不允许伪类型的 range。
	 */
	if (get_typtype(rangeSubtype) == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("range subtype cannot be %s",
						format_type_be(rangeSubtype))));

	/* Identify subopclass */
	/*
	 *
	 * 确定子类型的 opclass。
	 */
	rangeSubOpclass = findRangeSubOpclass(rangeSubOpclassName, rangeSubtype);

	/* Identify collation to use, if any */
	/*
	 *
	 * 确定要使用的 collation（如果有）。
	 */
	if (type_is_collatable(rangeSubtype))
	{
		if (rangeCollationName != NIL)
			rangeCollation = get_collation_oid(rangeCollationName, false);
		else
			rangeCollation = get_typcollation(rangeSubtype);
	}
	else
	{
		if (rangeCollationName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("range collation specified but subtype does not support collation")));
		rangeCollation = InvalidOid;
	}

	/* Identify support functions, if provided */
	/*
	 *
	 * 确定支持函数（如果提供了）。
	 */
	if (rangeCanonicalName != NIL)
	{
		if (!OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot specify a canonical function without a pre-created shell type"),
					 errhint("Create the type as a shell type, then create its canonicalization function, then do a full CREATE TYPE.")));
		rangeCanonical = findRangeCanonicalFunction(rangeCanonicalName,
													typoid);
	}
	else
		rangeCanonical = InvalidOid;

	if (rangeSubtypeDiffName != NIL)
		rangeSubtypeDiff = findRangeSubtypeDiffFunction(rangeSubtypeDiffName,
														rangeSubtype);
	else
		rangeSubtypeDiff = InvalidOid;

	get_typlenbyvalalign(rangeSubtype,
						 &subtyplen, &subtypbyval, &subtypalign);

	/* alignment must be TYPALIGN_INT or TYPALIGN_DOUBLE for ranges */
	/*
	 *
	 * range 的对齐方式必须是 TYPALIGN_INT 或 TYPALIGN_DOUBLE。
	 */
	alignment = (subtypalign == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	/* Allocate OID for array type, its multirange, and its multirange array */
	/*
	 *
	 * 为数组类型、其 multirange 以及 multirange 的数组类型分配 OID。
	 */
	rangeArrayOid = AssignTypeArrayOid();
	multirangeOid = AssignTypeMultirangeOid();
	multirangeArrayOid = AssignTypeMultirangeArrayOid();

	/* Create the pg_type entry */
	/*
	 *
	 * 创建 pg_type 项。
	 */
	address =
		TypeCreate(InvalidOid,	/* no predetermined type OID */
					/*
					 *
					 * 没有预定的类型 OID。
					 */
				   typeName,	/* type name */
						/*
						 *
						 * 类型名。
						 */
				   typeNamespace,	/* namespace */
							/*
							 *
							 * 命名空间。
							 */
				   InvalidOid,	/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
				   0,			/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
				   GetUserId(), /* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
				   -1,			/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
				   TYPTYPE_RANGE,	/* type-type (range type) */
							/*
							 *
							 * 类型种类（range 类型）。
							 */
				   TYPCATEGORY_RANGE,	/* type-category (range type) */
							/*
							 *
							 * 类型类别（range 类型）。
							 */
				   false,		/* range types are never preferred */
							/*
							 *
							 * range 类型从不是首选类型。
							 */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
							/*
							 *
							 * 数组元素分隔符。
							 */
				   F_RANGE_IN,	/* input procedure */
						/*
						 *
						 * input 过程。
						 */
				   F_RANGE_OUT, /* output procedure */
						/*
						 *
						 * output 过程。
						 */
				   F_RANGE_RECV,	/* receive procedure */
							/*
							 *
							 * receive 过程。
							 */
				   F_RANGE_SEND,	/* send procedure */
							/*
							 *
							 * send 过程。
							 */
				   InvalidOid,	/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
				   InvalidOid,	/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
				   F_RANGE_TYPANALYZE,	/* analyze procedure */
							/*
							 *
							 * analyze 过程。
							 */
				   InvalidOid,	/* subscript procedure - none */
						/*
						 *
						 * 没有 subscript 过程。
						 */
				   InvalidOid,	/* element type ID - none */
						/*
						 *
						 * 元素类型 ID，无。
						 */
				   false,		/* this is not an array type */
							/*
							 *
							 * 这不是数组类型。
							 */
				   rangeArrayOid,	/* array type we are about to create */
							/*
							 *
							 * 即将创建的数组类型。
							 */
				   InvalidOid,	/* base type ID (only for domains) */
						/*
						 *
						 * 基类型 ID（仅用于 domain）。
						 */
				   NULL,		/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
				   NULL,		/* no binary form available either */
							/*
							 *
							 * 同样没有可用的二进制形式。
							 */
				   false,		/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
				   alignment,	/* alignment */
						/*
						 *
						 * 对齐方式。
						 */
				   TYPSTORAGE_EXTENDED, /* TOAST strategy (always extended) */
							/*
							 *
							 * TOAST 策略（始终为 extended）。
							 */
				   -1,			/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
				   0,			/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
				   false,		/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
				   InvalidOid); /* type's collation (ranges never have one) */
						/*
						 *
						 * 类型的 collation（range 从没有）。
						 */
	Assert(typoid == InvalidOid || typoid == address.objectId);
	typoid = address.objectId;

	/* Create the multirange that goes with it */
	/*
	 *
	 * 创建与之配套的 multirange。
	 */
	if (multirangeTypeName)
	{
		Oid			old_typoid;

		/*
		 * Look to see if multirange type already exists.
		 *
		 * 查看 multirange 类型是否已存在。
		 */
		old_typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
									 CStringGetDatum(multirangeTypeName),
									 ObjectIdGetDatum(multirangeNamespace));

		/*
		 * If it's not a shell, see if it's an autogenerated array type, and
		 * if so rename it out of the way.
		 *
		 * 若它不是 shell，则查看它是否为自动生成的数组类型；若是，将其改名挪走。
		 */
		if (OidIsValid(old_typoid) && get_typisdefined(old_typoid))
		{
			if (!moveArrayTypeName(old_typoid, multirangeTypeName, multirangeNamespace))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("type \"%s\" already exists", multirangeTypeName)));
		}
	}
	else
	{
		/* Generate multirange name automatically */
		/*
		 *
		 * 自动生成 multirange 的名字。
		 */
		multirangeNamespace = typeNamespace;
		multirangeTypeName = makeMultirangeTypeName(typeName, multirangeNamespace);
	}

	mltrngaddress =
		TypeCreate(multirangeOid,	/* force assignment of this type OID */
						/*
						 *
						 * 强制分配此类型 OID。
						 */
				   multirangeTypeName,	/* type name */
							/*
							 *
							 * 类型名。
							 */
				   multirangeNamespace, /* namespace */
							/*
							 *
							 * 命名空间。
							 */
				   InvalidOid,	/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
				   0,			/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
				   GetUserId(), /* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
				   -1,			/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
				   TYPTYPE_MULTIRANGE,	/* type-type (multirange type) */
							/*
							 *
							 * 类型种类（multirange 类型）。
							 */
				   TYPCATEGORY_RANGE,	/* type-category (range type) */
							/*
							 *
							 * 类型类别（range 类型）。
							 */
				   false,		/* multirange types are never preferred */
							/*
							 *
							 * multirange 类型从不是首选类型。
							 */
				   DEFAULT_TYPDELIM,	/* array element delimiter */
							/*
							 *
							 * 数组元素分隔符。
							 */
				   F_MULTIRANGE_IN, /* input procedure */
						    /*
						     *
						     * input 过程。
						     */
				   F_MULTIRANGE_OUT,	/* output procedure */
							/*
							 *
							 * output 过程。
							 */
				   F_MULTIRANGE_RECV,	/* receive procedure */
							/*
							 *
							 * receive 过程。
							 */
				   F_MULTIRANGE_SEND,	/* send procedure */
							/*
							 *
							 * send 过程。
							 */
				   InvalidOid,	/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
				   InvalidOid,	/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
				   F_MULTIRANGE_TYPANALYZE, /* analyze procedure */
							    /*
							     *
							     * analyze 过程。
							     */
				   InvalidOid,	/* subscript procedure - none */
						/*
						 *
						 * 没有 subscript 过程。
						 */
				   InvalidOid,	/* element type ID - none */
						/*
						 *
						 * 元素类型 ID，无。
						 */
				   false,		/* this is not an array type */
							/*
							 *
							 * 这不是数组类型。
							 */
				   multirangeArrayOid,	/* array type we are about to create */
							/*
							 *
							 * 即将创建的数组类型。
							 */
				   InvalidOid,	/* base type ID (only for domains) */
						/*
						 *
						 * 基类型 ID（仅用于 domain）。
						 */
				   NULL,		/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
				   NULL,		/* no binary form available either */
							/*
							 *
							 * 同样没有可用的二进制形式。
							 */
				   false,		/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
				   alignment,	/* alignment */
						/*
						 *
						 * 对齐方式。
						 */
				   'x',			/* TOAST strategy (always extended) */
							/*
							 *
							 * TOAST 策略（始终为 extended）。
							 */
				   -1,			/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
				   0,			/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
				   false,		/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
				   InvalidOid); /* type's collation (ranges never have one) */
						/*
						 *
						 * 类型的 collation（range 从没有）。
						 */
	Assert(multirangeOid == mltrngaddress.objectId);

	/* Create the entry in pg_range */
	/*
	 *
	 * 在 pg_range 中创建对应项。
	 */
	RangeCreate(typoid, rangeSubtype, rangeCollation, rangeSubOpclass,
				rangeCanonical, rangeSubtypeDiff, multirangeOid);

	/*
	 * Create the array type that goes with it.
	 *
	 * 创建与之配套的数组类型。
	 */
	rangeArrayName = makeArrayTypeName(typeName, typeNamespace);

	TypeCreate(rangeArrayOid,	/* force assignment of this type OID */
					/*
					 *
					 * 强制分配此类型 OID。
					 */
			   rangeArrayName,	/* type name */
						/*
						 *
						 * 类型名。
						 */
			   typeNamespace,	/* namespace */
						/*
						 *
						 * 命名空间。
						 */
			   InvalidOid,		/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
			   0,				/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
			   GetUserId(),		/* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
			   -1,				/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
			   TYPTYPE_BASE,	/* type-type (base type) */
						/*
						 *
						 * 类型种类（基类型）。
						 */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
						/*
						 *
						 * 类型类别（数组）。
						 */
			   false,			/* array types are never preferred */
							/*
							 *
							 * 数组类型从不是首选类型。
							 */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
			   F_ARRAY_IN,		/* input procedure */
						/*
						 *
						 * input 过程。
						 */
			   F_ARRAY_OUT,		/* output procedure */
						/*
						 *
						 * output 过程。
						 */
			   F_ARRAY_RECV,	/* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
			   F_ARRAY_SEND,	/* send procedure */
						/*
						 *
						 * send 过程。
						 */
			   InvalidOid,		/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
			   InvalidOid,		/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
						/*
						 *
						 * analyze 过程。
						 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
							/*
							 *
							 * 数组 subscript 过程。
							 */
			   typoid,			/* element type ID */
							/*
							 *
							 * 元素类型 ID。
							 */
			   true,			/* yes this is an array type */
							/*
							 *
							 * 是的，这是数组类型。
							 */
			   InvalidOid,		/* no further array type */
						/*
						 *
						 * 没有更外层的数组类型。
						 */
			   InvalidOid,		/* base type ID */
						/*
						 *
						 * 基类型 ID。
						 */
			   NULL,			/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
			   NULL,			/* binary default isn't sent either */
							/*
							 *
							 * 同样不发送二进制默认值。
							 */
			   false,			/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
			   alignment,		/* alignment - same as range's */
						/*
						 *
						 * 对齐方式，与 range 相同。
						 */
			   TYPSTORAGE_EXTENDED, /* ARRAY is always toastable */
						/*
						 *
						 * ARRAY 总是可以被 toast。
						 */
			   -1,				/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
			   0,				/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
			   false,			/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
			   InvalidOid);		/* typcollation */
						/*
						 *
						 * typcollation。
						 */

	pfree(rangeArrayName);

	/* Create the multirange's array type */
	/*
	 *
	 * 创建该 multirange 的数组类型。
	 */

	multirangeArrayName = makeArrayTypeName(multirangeTypeName, typeNamespace);

	TypeCreate(multirangeArrayOid,	/* force assignment of this type OID */
					/*
					 *
					 * 强制分配此类型 OID。
					 */
			   multirangeArrayName, /* type name */
						/*
						 *
						 * 类型名。
						 */
			   multirangeNamespace, /* namespace */
						/*
						 *
						 * 命名空间。
						 */
			   InvalidOid,		/* relation oid (n/a here) */
						/*
						 *
						 * 关系 OID（此处不适用）。
						 */
			   0,				/* relation kind (ditto) */
							/*
							 *
							 * 关系种类（同上）。
							 */
			   GetUserId(),		/* owner's ID */
						/*
						 *
						 * 属主 ID。
						 */
			   -1,				/* internal size (always varlena) */
							/*
							 *
							 * 内部长度（始终为 varlena）。
							 */
			   TYPTYPE_BASE,	/* type-type (base type) */
						/*
						 *
						 * 类型种类（基类型）。
						 */
			   TYPCATEGORY_ARRAY,	/* type-category (array) */
						/*
						 *
						 * 类型类别（数组）。
						 */
			   false,			/* array types are never preferred */
							/*
							 *
							 * 数组类型从不是首选类型。
							 */
			   DEFAULT_TYPDELIM,	/* array element delimiter */
						/*
						 *
						 * 数组元素分隔符。
						 */
			   F_ARRAY_IN,		/* input procedure */
						/*
						 *
						 * input 过程。
						 */
			   F_ARRAY_OUT,		/* output procedure */
						/*
						 *
						 * output 过程。
						 */
			   F_ARRAY_RECV,	/* receive procedure */
						/*
						 *
						 * receive 过程。
						 */
			   F_ARRAY_SEND,	/* send procedure */
						/*
						 *
						 * send 过程。
						 */
			   InvalidOid,		/* typmodin procedure - none */
						/*
						 *
						 * 没有 typmodin 过程。
						 */
			   InvalidOid,		/* typmodout procedure - none */
						/*
						 *
						 * 没有 typmodout 过程。
						 */
			   F_ARRAY_TYPANALYZE,	/* analyze procedure */
						/*
						 *
						 * analyze 过程。
						 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
							/*
							 *
							 * 数组 subscript 过程。
							 */
			   multirangeOid,	/* element type ID */
						/*
						 *
						 * 元素类型 ID。
						 */
			   true,			/* yes this is an array type */
							/*
							 *
							 * 是的，这是数组类型。
							 */
			   InvalidOid,		/* no further array type */
						/*
						 *
						 * 没有更外层的数组类型。
						 */
			   InvalidOid,		/* base type ID */
						/*
						 *
						 * 基类型 ID。
						 */
			   NULL,			/* never a default type value */
							/*
							 *
							 * 从无默认类型值。
							 */
			   NULL,			/* binary default isn't sent either */
							/*
							 *
							 * 同样不发送二进制默认值。
							 */
			   false,			/* never passed by value */
							/*
							 *
							 * 从不以值传递。
							 */
			   alignment,		/* alignment - same as range's */
						/*
						 *
						 * 对齐方式，与 range 相同。
						 */
			   'x',				/* ARRAY is always toastable */
							/*
							 *
							 * ARRAY 总是可以被 toast。
							 */
			   -1,				/* typMod (Domains only) */
							/*
							 *
							 * typMod（仅用于 domain）。
							 */
			   0,				/* Array dimensions of typbasetype */
							/*
							 *
							 * typbasetype 的数组维数。
							 */
			   false,			/* Type NOT NULL */
							/*
							 *
							 * 类型为 NOT NULL。
							 */
			   InvalidOid);		/* typcollation */
						/*
						 *
						 * typcollation。
						 */

	/* And create the constructor functions for this range type */
	/*
	 *
	 * 并为该 range 类型创建构造函数。
	 */
	makeRangeConstructors(typeName, typeNamespace, typoid, rangeSubtype);
	makeMultirangeConstructors(multirangeTypeName, typeNamespace,
							   multirangeOid, typoid, rangeArrayOid,
							   &castFuncOid);

	/* Create cast from the range type to its multirange type */
	/*
	 *
	 * 创建从 range 类型到其 multirange 类型的 cast。
	 */
	CastCreate(typoid, multirangeOid, castFuncOid, InvalidOid, InvalidOid,
			   COERCION_CODE_EXPLICIT, COERCION_METHOD_FUNCTION,
			   DEPENDENCY_INTERNAL);

	pfree(multirangeArrayName);

	return address;
}

/*
 * Because there may exist several range types over the same subtype, the
 * range type can't be uniquely determined from the subtype.  So it's
 * impossible to define a polymorphic constructor; we have to generate new
 * constructor functions explicitly for each range type.
 *
 * 同一子类型上可能存在多个 range 类型，因此无法仅凭子类型唯一确定 range 类型。
 * 也就不可能定义多态构造函数，必须为每个 range 类型显式生成构造函数。
 *
 * We actually define 4 functions, with 0 through 3 arguments.  This is just
 * to offer more convenience for the user.
 *
 * 实际上定义 4 个函数，参数个数从 0 到 3。这只是为了给用户更多便利。
 */
static void
makeRangeConstructors(const char *name, Oid namespace,
					  Oid rangeOid, Oid subtype)
{
	static const char *const prosrc[2] = {"range_constructor2",
	"range_constructor3"};
	static const int pronargs[2] = {2, 3};

	Oid			constructorArgTypes[3];
	ObjectAddress myself,
				referenced;
	int			i;

	constructorArgTypes[0] = subtype;
	constructorArgTypes[1] = subtype;
	constructorArgTypes[2] = TEXTOID;

	referenced.classId = TypeRelationId;
	referenced.objectId = rangeOid;
	referenced.objectSubId = 0;

	for (i = 0; i < lengthof(prosrc); i++)
	{
		oidvector  *constructorArgTypesVector;

		constructorArgTypesVector = buildoidvector(constructorArgTypes,
												   pronargs[i]);

		myself = ProcedureCreate(name,	/* name: same as range type */
						/*
						 *
						 * 名字：与 range 类型相同。
						 */
								 namespace, /* namespace */
									    /*
									     *
									     * 命名空间。
									     */
								 false, /* replace */
									/*
									 *
									 * replace。
									 */
								 false, /* returns set */
									/*
									 *
									 * 是否返回集合。
									 */
								 rangeOid,	/* return type */
										/*
										 *
										 * 返回类型。
										 */
								 BOOTSTRAP_SUPERUSERID, /* proowner */
											/*
											 *
											 * proowner。
											 */
								 INTERNALlanguageId,	/* language */
											/*
											 *
											 * 语言。
											 */
								 F_FMGR_INTERNAL_VALIDATOR, /* language validator */
											    /*
											     *
											     * 语言校验函数。
											     */
								 prosrc[i], /* prosrc */
									    /*
									     *
									     * prosrc。
									     */
								 NULL,	/* probin */
									/*
									 *
									 * probin。
									 */
								 NULL,	/* prosqlbody */
									/*
									 *
									 * prosqlbody。
									 */
								 PROKIND_FUNCTION,
								 false, /* security_definer */
									/*
									 *
									 * security_definer。
									 */
								 false, /* leakproof */
									/*
									 *
									 * leakproof。
									 */
								 false, /* isStrict */
									/*
									 *
									 * isStrict。
									 */
								 PROVOLATILE_IMMUTABLE, /* volatility */
											/*
											 *
											 * 易变性。
											 */
								 PROPARALLEL_SAFE,	/* parallel safety */
											/*
											 *
											 * 并行安全性。
											 */
								 constructorArgTypesVector, /* parameterTypes */
											    /*
											     *
											     * parameterTypes。
											     */
								 PointerGetDatum(NULL), /* allParameterTypes */
											/*
											 *
											 * allParameterTypes。
											 */
								 PointerGetDatum(NULL), /* parameterModes */
											/*
											 *
											 * parameterModes。
											 */
								 PointerGetDatum(NULL), /* parameterNames */
											/*
											 *
											 * parameterNames。
											 */
								 NIL,	/* parameterDefaults */
									/*
									 *
									 * parameterDefaults。
									 */
								 PointerGetDatum(NULL), /* trftypes */
											/*
											 *
											 * trftypes。
											 */
								 NIL,	/* trfoids */
									/*
									 *
									 * trfoids。
									 */
								 PointerGetDatum(NULL), /* proconfig */
											/*
											 *
											 * proconfig。
											 */
								 InvalidOid,	/* prosupport */
										/*
										 *
										 * prosupport。
										 */
								 1.0,	/* procost */
									/*
									 *
									 * procost。
									 */
								 0.0);	/* prorows */
									/*
									 *
									 * prorows。
									 */

		/*
		 * Make the constructors internally-dependent on the range type so
		 * that they go away silently when the type is dropped.  Note that
		 * pg_dump depends on this choice to avoid dumping the constructors.
		 *
		 * 让这些构造函数内部依赖于 range 类型，这样类型被删除时构造函数会一并静默消失。
		 * 注意 pg_dump 依赖这一选择，从而不转储这些构造函数。
		 */
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
}

/*
 * We make a separate multirange constructor for each range type
 * so its name can include the base type, like range constructors do.
 * If we had an anyrangearray polymorphic type we could use it here,
 * but since each type has its own constructor name there's no need.
 *
 * 为每个 range 类型单独做一个 multirange 构造函数，这样名字可以像 range 构造函数那样包含基类型。
 * 若有 anyrangearray 多态类型就可以在这里使用，但既然每个类型都有自己的构造函数名，也就不需要了。
 *
 * Sets castFuncOid to the oid of the new constructor that can be used
 * to cast from a range to a multirange.
 *
 * 把 castFuncOid 设为可用于从 range 转换到 multirange 的新构造函数的 oid。
 */
static void
makeMultirangeConstructors(const char *name, Oid namespace,
						   Oid multirangeOid, Oid rangeOid, Oid rangeArrayOid,
						   Oid *castFuncOid)
{
	ObjectAddress myself,
				referenced;
	oidvector  *argtypes;
	Datum		allParamTypes;
	ArrayType  *allParameterTypes;
	Datum		paramModes;
	ArrayType  *parameterModes;

	referenced.classId = TypeRelationId;
	referenced.objectId = multirangeOid;
	referenced.objectSubId = 0;

	/* 0-arg constructor - for empty multiranges */
	/*
	 *
	 * 0 参数构造函数，用于空的 multirange。
	 */
	argtypes = buildoidvector(NULL, 0);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
					/*
					 *
					 * 名字：与 multirange 类型相同。
					 */
							 namespace,
							 false, /* replace */
								/*
								 *
								 * replace。
								 */
							 false, /* returns set */
								/*
								 *
								 * 是否返回集合。
								 */
							 multirangeOid, /* return type */
									/*
									 *
									 * 返回类型。
									 */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
										/*
										 *
										 * proowner。
										 */
							 INTERNALlanguageId,	/* language */
										/*
										 *
										 * 语言。
										 */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor0", /* prosrc */
										    /*
										     *
										     * prosrc。
										     */
							 NULL,	/* probin */
								/*
								 *
								 * probin。
								 */
							 NULL,	/* prosqlbody */
								/*
								 *
								 * prosqlbody。
								 */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
								/*
								 *
								 * security_definer。
								 */
							 false, /* leakproof */
								/*
								 *
								 * leakproof。
								 */
							 true,	/* isStrict */
								/*
								 *
								 * isStrict。
								 */
							 PROVOLATILE_IMMUTABLE, /* volatility */
										/*
										 *
										 * 易变性。
										 */
							 PROPARALLEL_SAFE,	/* parallel safety */
										/*
										 *
										 * 并行安全性。
										 */
							 argtypes,	/* parameterTypes */
									/*
									 *
									 * parameterTypes。
									 */
							 PointerGetDatum(NULL), /* allParameterTypes */
										/*
										 *
										 * allParameterTypes。
										 */
							 PointerGetDatum(NULL), /* parameterModes */
										/*
										 *
										 * parameterModes。
										 */
							 PointerGetDatum(NULL), /* parameterNames */
										/*
										 *
										 * parameterNames。
										 */
							 NIL,	/* parameterDefaults */
								/*
								 *
								 * parameterDefaults。
								 */
							 PointerGetDatum(NULL), /* trftypes */
										/*
										 *
										 * trftypes。
										 */
							 NIL,	/* trfoids */
								/*
								 *
								 * trfoids。
								 */
							 PointerGetDatum(NULL), /* proconfig */
										/*
										 *
										 * proconfig。
										 */
							 InvalidOid,	/* prosupport */
									/*
									 *
									 * prosupport。
									 */
							 1.0,	/* procost */
								/*
								 *
								 * procost。
								 */
							 0.0);	/* prorows */
								/*
								 *
								 * prorows。
								 */

	/*
	 * Make the constructor internally-dependent on the multirange type so
	 * that they go away silently when the type is dropped.  Note that pg_dump
	 * depends on this choice to avoid dumping the constructors.
	 *
	 * 让该构造函数内部依赖于 multirange 类型，这样类型被删除时构造函数会一并静默消失。
	 * 注意 pg_dump 依赖这一选择，从而不转储这些构造函数。
	 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);

	/*
	 * 1-arg constructor - for casts
	 *
	 * 1 参数构造函数，用于类型转换。
	 *
	 * In theory we shouldn't need both this and the vararg (n-arg)
	 * constructor, but having a separate 1-arg function lets us define casts
	 * against it.
	 *
	 * 理论上不必同时要这个函数和可变参数（n 参数）构造函数，但单独的 1 参数函数让我们可以基于它定义 cast。
	 */
	argtypes = buildoidvector(&rangeOid, 1);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
					/*
					 *
					 * 名字：与 multirange 类型相同。
					 */
							 namespace,
							 false, /* replace */
								/*
								 *
								 * replace。
								 */
							 false, /* returns set */
								/*
								 *
								 * 是否返回集合。
								 */
							 multirangeOid, /* return type */
									/*
									 *
									 * 返回类型。
									 */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
										/*
										 *
										 * proowner。
										 */
							 INTERNALlanguageId,	/* language */
										/*
										 *
										 * 语言。
										 */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor1", /* prosrc */
										    /*
										     *
										     * prosrc。
										     */
							 NULL,	/* probin */
								/*
								 *
								 * probin。
								 */
							 NULL,	/* prosqlbody */
								/*
								 *
								 * prosqlbody。
								 */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
								/*
								 *
								 * security_definer。
								 */
							 false, /* leakproof */
								/*
								 *
								 * leakproof。
								 */
							 true,	/* isStrict */
								/*
								 *
								 * isStrict。
								 */
							 PROVOLATILE_IMMUTABLE, /* volatility */
										/*
										 *
										 * 易变性。
										 */
							 PROPARALLEL_SAFE,	/* parallel safety */
										/*
										 *
										 * 并行安全性。
										 */
							 argtypes,	/* parameterTypes */
									/*
									 *
									 * parameterTypes。
									 */
							 PointerGetDatum(NULL), /* allParameterTypes */
										/*
										 *
										 * allParameterTypes。
										 */
							 PointerGetDatum(NULL), /* parameterModes */
										/*
										 *
										 * parameterModes。
										 */
							 PointerGetDatum(NULL), /* parameterNames */
										/*
										 *
										 * parameterNames。
										 */
							 NIL,	/* parameterDefaults */
								/*
								 *
								 * parameterDefaults。
								 */
							 PointerGetDatum(NULL), /* trftypes */
										/*
										 *
										 * trftypes。
										 */
							 NIL,	/* trfoids */
								/*
								 *
								 * trfoids。
								 */
							 PointerGetDatum(NULL), /* proconfig */
										/*
										 *
										 * proconfig。
										 */
							 InvalidOid,	/* prosupport */
									/*
									 *
									 * prosupport。
									 */
							 1.0,	/* procost */
								/*
								 *
								 * procost。
								 */
							 0.0);	/* prorows */
								/*
								 *
								 * prorows。
								 */
	/* ditto */
	/*
	 *
	 * 同上。
	 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	*castFuncOid = myself.objectId;

	/* n-arg constructor - vararg */
	/*
	 *
	 * n 参数构造函数，即可变参数。
	 */
	argtypes = buildoidvector(&rangeArrayOid, 1);
	allParamTypes = ObjectIdGetDatum(rangeArrayOid);
	allParameterTypes = construct_array_builtin(&allParamTypes, 1, OIDOID);
	paramModes = CharGetDatum(FUNC_PARAM_VARIADIC);
	parameterModes = construct_array_builtin(&paramModes, 1, CHAROID);
	myself = ProcedureCreate(name,	/* name: same as multirange type */
					/*
					 *
					 * 名字：与 multirange 类型相同。
					 */
							 namespace,
							 false, /* replace */
								/*
								 *
								 * replace。
								 */
							 false, /* returns set */
								/*
								 *
								 * 是否返回集合。
								 */
							 multirangeOid, /* return type */
									/*
									 *
									 * 返回类型。
									 */
							 BOOTSTRAP_SUPERUSERID, /* proowner */
										/*
										 *
										 * proowner。
										 */
							 INTERNALlanguageId,	/* language */
										/*
										 *
										 * 语言。
										 */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor2", /* prosrc */
										    /*
										     *
										     * prosrc。
										     */
							 NULL,	/* probin */
								/*
								 *
								 * probin。
								 */
							 NULL,	/* prosqlbody */
								/*
								 *
								 * prosqlbody。
								 */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
								/*
								 *
								 * security_definer。
								 */
							 false, /* leakproof */
								/*
								 *
								 * leakproof。
								 */
							 true,	/* isStrict */
								/*
								 *
								 * isStrict。
								 */
							 PROVOLATILE_IMMUTABLE, /* volatility */
										/*
										 *
										 * 易变性。
										 */
							 PROPARALLEL_SAFE,	/* parallel safety */
										/*
										 *
										 * 并行安全性。
										 */
							 argtypes,	/* parameterTypes */
									/*
									 *
									 * parameterTypes。
									 */
							 PointerGetDatum(allParameterTypes),	/* allParameterTypes */
												/*
												 *
												 * allParameterTypes。
												 */
							 PointerGetDatum(parameterModes),	/* parameterModes */
												/*
												 *
												 * parameterModes。
												 */
							 PointerGetDatum(NULL), /* parameterNames */
										/*
										 *
										 * parameterNames。
										 */
							 NIL,	/* parameterDefaults */
								/*
								 *
								 * parameterDefaults。
								 */
							 PointerGetDatum(NULL), /* trftypes */
										/*
										 *
										 * trftypes。
										 */
							 NIL,	/* trfoids */
								/*
								 *
								 * trfoids。
								 */
							 PointerGetDatum(NULL), /* proconfig */
										/*
										 *
										 * proconfig。
										 */
							 InvalidOid,	/* prosupport */
									/*
									 *
									 * prosupport。
									 */
							 1.0,	/* procost */
								/*
								 *
								 * procost。
								 */
							 0.0);	/* prorows */
								/*
								 *
								 * prorows。
								 */
	/* ditto */
	/*
	 *
	 * 同上。
	 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	pfree(allParameterTypes);
	pfree(parameterModes);
}

/*
 * Find suitable I/O and other support functions for a type.
 *
 * 为类型查找合适的 I/O 及其他支持函数。
 *
 * typeOid is the type's OID (which will already exist, if only as a shell
 * type).
 *
 * typeOid 是该类型的 OID（即便只是 shell type，也已经存在）。
 */

static Oid
findTypeInputFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/*
	 * Input functions can take a single argument of type CSTRING, or three
	 * arguments (string, typioparam OID, typmod).  Whine about ambiguity if
	 * both forms exist.
	 *
	 * input 函数可以接受一个 CSTRING 参数，或三个参数（string、typioparam OID、typmod）。
	 * 若两种形式都存在，则对这种歧义报错。
	 */
	argList[0] = CSTRINGOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type input function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* If not found, reference the 1-argument signature in error msg */
		/*
		 *
		 * 若未找到，则在错误信息中引用单参数签名。
		 */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* Input functions must return the target type. */
	/*
	 *
	 * input 函数必须返回目标类型。
	 */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/*
	 * Print warnings if any of the type's I/O functions are marked volatile.
	 * There is a general assumption that I/O functions are stable or
	 * immutable; this allows us for example to mark record_in/record_out
	 * stable rather than volatile.  Ideally we would throw errors not just
	 * warnings here; but since this check is new as of 9.5, and since the
	 * volatility marking might be just an error-of-omission and not a true
	 * indication of how the function behaves, we'll let it pass as a warning
	 * for now.
	 *
	 * 若类型的任何 I/O 函数被标为 volatile，则打印警告。
	 * 通常假定 I/O 函数是 stable 或 immutable；例如因此才能把 record_in/record_out 标成 stable 而不是 volatile。
	 * 理想情况下这里应报错而不仅是警告；但该检查自 9.5 才有，且 volatility 标记可能只是遗漏而非函数真实行为，因此目前只给警告。
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

/*
 * 查找类型的 output 函数：必须接受该类型的单个参数并返回 cstring。
 */
static Oid
findTypeOutputFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Output functions always take a single argument of the type and return
	 * cstring.
	 *
	 * output 函数总是接受该类型的单个参数并返回 cstring。
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	/*
	 *
	 * 目前只发警告，原因见 findTypeInputFunction 中的注释。
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

/*
 * 查找类型的 receive 函数。
 * 可为 INTERNAL 单参，或 (internal, typioparam OID, typmod) 三参，且必须返回目标类型。
 */
static Oid
findTypeReceiveFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/*
	 * Receive functions can take a single argument of type INTERNAL, or three
	 * arguments (internal, typioparam OID, typmod).  Whine about ambiguity if
	 * both forms exist.
	 *
	 * receive 函数可以接受一个 INTERNAL 参数，或三个参数（internal、typioparam OID、typmod）。
	 * 若两种形式都存在，则对这种歧义报错。
	 */
	argList[0] = INTERNALOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type receive function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* If not found, reference the 1-argument signature in error msg */
		/*
		 *
		 * 若未找到，则在错误信息中引用单参数签名。
		 */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* Receive functions must return the target type. */
	/*
	 *
	 * receive 函数必须返回目标类型。
	 */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/* Just a warning for now, per comments in findTypeInputFunction */
	/*
	 *
	 * 目前只发警告，原因见 findTypeInputFunction 中的注释。
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

/*
 * 查找类型的 send 函数：必须接受该类型的单个参数并返回 bytea。
 */
static Oid
findTypeSendFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Send functions always take a single argument of the type and return
	 * bytea.
	 *
	 * send 函数总是接受该类型的单个参数并返回 bytea。
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s must return type %s",
						NameListToString(procname), "bytea")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	/*
	 *
	 * 目前只发警告，原因见 findTypeInputFunction 中的注释。
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

/*
 * 查找 typmod_in 函数：必须接受一个 cstring[] 参数并返回 int4。
 */
static Oid
findTypeTypmodinFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * typmodin functions always take one cstring[] argument and return int4.
	 *
	 * typmodin 函数总是接受一个 cstring[] 参数并返回 int4。
	 */
	argList[0] = CSTRINGARRAYOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_in function %s must return type %s",
						NameListToString(procname), "integer")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	/*
	 *
	 * 目前只发警告，原因见 findTypeInputFunction 中的注释。
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

/*
 * 查找 typmod_out 函数：必须接受一个 int4 参数并返回 cstring。
 */
static Oid
findTypeTypmodoutFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * typmodout functions always take one int4 argument and return cstring.
	 *
	 * typmodout 函数总是接受一个 int4 参数并返回 cstring。
	 */
	argList[0] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_out function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* Just a warning for now, per comments in findTypeInputFunction */
	/*
	 *
	 * 目前只发警告，原因见 findTypeInputFunction 中的注释。
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

/*
 * 查找类型的 analyze 函数：必须接受一个 INTERNAL 参数并返回 bool。
 */
static Oid
findTypeAnalyzeFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Analyze functions always take one INTERNAL argument and return bool.
	 *
	 * analyze 函数总是接受一个 INTERNAL 参数并返回 bool。
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type analyze function %s must return type %s",
						NameListToString(procname), "boolean")));

	return procOid;
}

/*
 * 查找类型的 subscript 函数：必须接受并返回 INTERNAL，且不允许显式选用 array_subscript_handler。
 */
static Oid
findTypeSubscriptingFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * Subscripting support functions always take one INTERNAL argument and
	 * return INTERNAL.  (The argument is not used, but we must have it to
	 * maintain type safety.)
	 *
	 * subscript 支持函数总是接受一个 INTERNAL 参数并返回 INTERNAL。（该参数并不使用，但为了类型安全必须有它。）
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type subscripting function %s must return type %s",
						NameListToString(procname), "internal")));

	/*
	 * We disallow array_subscript_handler() from being selected explicitly,
	 * since that must only be applied to autogenerated array types.
	 *
	 * 不允许显式选用 array_subscript_handler()，它只能用于自动生成的数组类型。
	 */
	if (procOid == F_ARRAY_SUBSCRIPT_HANDLER)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("user-defined types cannot use subscripting function %s",
						NameListToString(procname))));

	return procOid;
}

/*
 * Find suitable support functions and opclasses for a range type.
 *
 * 为 range 类型查找合适的支持函数与 opclass。
 */

/*
 * Find named btree opclass for subtype, or default btree opclass if
 * opcname is NIL.
 *
 * 查找子类型指定的 btree opclass；若 opcname 为 NIL，则使用默认 btree opclass。
 */
static Oid
findRangeSubOpclass(List *opcname, Oid subtype)
{
	Oid			opcid;
	Oid			opInputType;

	if (opcname != NIL)
	{
		opcid = get_opclass_oid(BTREE_AM_OID, opcname, false);

		/*
		 * Verify that the operator class accepts this datatype. Note we will
		 * accept binary compatibility.
		 *
		 * 验证该操作符类接受此数据类型。注意我们接受二进制兼容。
		 */
		opInputType = get_opclass_input_type(opcid);
		if (!IsBinaryCoercible(subtype, opInputType))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("operator class \"%s\" does not accept data type %s",
							NameListToString(opcname),
							format_type_be(subtype))));
	}
	else
	{
		opcid = GetDefaultOpClass(subtype, BTREE_AM_OID);
		if (!OidIsValid(opcid))
		{
			/* We spell the error message identically to ResolveOpClass */
			/*
			 *
			 * 错误信息的措辞与 ResolveOpClass 保持一致。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(subtype), "btree"),
					 errhint("You must specify an operator class for the range type or define a default operator class for the subtype.")));
		}
	}

	return opcid;
}

/*
 * 查找 range 的 canonical 函数：必须接受并返回该 range 类型、为 immutable，且创建者有执行权限。
 */
static Oid
findRangeCanonicalFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;
	AclResult	aclresult;

	/*
	 * Range canonical functions must take and return the range type, and must
	 * be immutable.
	 *
	 * range 的 canonical 函数必须接受并返回该 range 类型，且必须是 immutable。
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must return range type",
						func_signature_string(procname, 1, NIL, argList))));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must be immutable",
						func_signature_string(procname, 1, NIL, argList))));

	/* Also, range type's creator must have permission to call function */
	/*
	 *
	 * 此外，range 类型的创建者必须有权调用该函数。
	 */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(procOid));

	return procOid;
}

/*
 * 查找 range 子类型的 diff 函数：必须接受两个子类型参数、返回 float8、为 immutable，且创建者有执行权限。
 */
static Oid
findRangeSubtypeDiffFunction(List *procname, Oid subtype)
{
	Oid			argList[2];
	Oid			procOid;
	AclResult	aclresult;

	/*
	 * Range subtype diff functions must take two arguments of the subtype,
	 * must return float8, and must be immutable.
	 *
	 * range 子类型的 diff 函数必须接受两个子类型参数，返回 float8，且必须是 immutable。
	 */
	argList[0] = subtype;
	argList[1] = subtype;

	procOid = LookupFuncName(procname, 2, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 2, NIL, argList))));

	if (get_func_rettype(procOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must return type %s",
						func_signature_string(procname, 2, NIL, argList),
						"double precision")));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must be immutable",
						func_signature_string(procname, 2, NIL, argList))));

	/* Also, range type's creator must have permission to call function */
	/*
	 *
	 * 此外，range 类型的创建者必须有权调用该函数。
	 */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(procOid));

	return procOid;
}

/*
 *	AssignTypeArrayOid
 *
 * 函数 AssignTypeArrayOid。
 *
 *	Pre-assign the type's array OID for use in pg_type.typarray
 *
 * 预先分配类型的数组 OID，供写入 pg_type.typarray。
 */
Oid
AssignTypeArrayOid(void)
{
	Oid			type_array_oid;

	/* Use binary-upgrade override for pg_type.typarray? */
	/*
	 *
	 * 是否使用二进制升级对 pg_type.typarray 的覆盖值？
	 */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_array_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type array OID value not set when in binary upgrade mode")));

		type_array_oid = binary_upgrade_next_array_pg_type_oid;
		binary_upgrade_next_array_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
											Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_array_oid;
}

/*
 *	AssignTypeMultirangeOid
 *
 * 函数 AssignTypeMultirangeOid。
 *
 *	Pre-assign the range type's multirange OID for use in pg_type.oid
 *
 * 预先分配 range 类型的 multirange OID，供写入 pg_type.oid。
 */
Oid
AssignTypeMultirangeOid(void)
{
	Oid			type_multirange_oid;

	/* Use binary-upgrade override for pg_type.oid? */
	/*
	 *
	 * 是否使用二进制升级对 pg_type.oid 的覆盖值？
	 */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_mrng_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type multirange OID value not set when in binary upgrade mode")));

		type_multirange_oid = binary_upgrade_next_mrng_pg_type_oid;
		binary_upgrade_next_mrng_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
												 Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_oid;
}

/*
 *	AssignTypeMultirangeArrayOid
 *
 * 函数 AssignTypeMultirangeArrayOid。
 *
 *	Pre-assign the range type's multirange array OID for use in pg_type.typarray
 *
 * 预先分配 range 类型的 multirange 数组 OID，供写入 pg_type.typarray。
 */
Oid
AssignTypeMultirangeArrayOid(void)
{
	Oid			type_multirange_array_oid;

	/* Use binary-upgrade override for pg_type.oid? */
	/*
	 *
	 * 是否使用二进制升级对 pg_type.oid 的覆盖值？
	 */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_mrng_array_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type multirange array OID value not set when in binary upgrade mode")));

		type_multirange_array_oid = binary_upgrade_next_mrng_array_pg_type_oid;
		binary_upgrade_next_mrng_array_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
													   Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_array_oid;
}


/*-------------------------------------------------------------------
 * DefineCompositeType
 *
 * 函数 DefineCompositeType。
 *
 * Create a Composite Type relation.
 * `DefineRelation' does all the work, we just provide the correct
 * arguments!
 *
 * 创建复合类型关系。实际工作由 DefineRelation 完成，这里只提供正确的参数。
 *
 * If the relation already exists, then 'DefineRelation' will abort
 * the xact...
 *
 * 若关系已存在，DefineRelation 会中止事务。
 *
 * Return type is the new type's object address.
 *
 * 返回值是新类型的对象地址。
 *-------------------------------------------------------------------
 */
ObjectAddress
DefineCompositeType(RangeVar *typevar, List *coldeflist)
{
	CreateStmt *createStmt = makeNode(CreateStmt);
	Oid			old_type_oid;
	Oid			typeNamespace;
	ObjectAddress address;

	/*
	 * now set the parameters for keys/inheritance etc. All of these are
	 * uninteresting for composite types...
	 *
	 * 现在设置键、继承等参数。对复合类型来说这些都不重要。
	 */
	createStmt->relation = typevar;
	createStmt->tableElts = coldeflist;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->options = NIL;
	createStmt->oncommit = ONCOMMIT_NOOP;
	createStmt->tablespacename = NULL;
	createStmt->if_not_exists = false;

	/*
	 * Check for collision with an existing type name. If there is one and
	 * it's an autogenerated array, we can rename it out of the way.  This
	 * check is here mainly to get a better error message about a "type"
	 * instead of below about a "relation".
	 *
	 * 检查是否与已有类型名冲突。若冲突对象是自动生成的数组类型，可以把它改名挪走。
	 * 放在这里主要是为了给出关于“类型”的更明确错误，而不是下面关于“关系”的错误。
	 */
	typeNamespace = RangeVarGetAndCheckCreationNamespace(createStmt->relation,
														 NoLock, NULL);
	RangeVarAdjustRelationPersistence(createStmt->relation, typeNamespace);
	old_type_oid =
		GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						CStringGetDatum(createStmt->relation->relname),
						ObjectIdGetDatum(typeNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, createStmt->relation->relname, typeNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", createStmt->relation->relname)));
	}

	/*
	 * Finally create the relation.  This also creates the type.
	 *
	 * 最后创建关系。这同时也会创建类型。
	 */
	DefineRelation(createStmt, RELKIND_COMPOSITE_TYPE, InvalidOid, &address,
				   NULL);

	return address;
}

/*
 * AlterDomainDefault
 *
 * 函数 AlterDomainDefault。
 *
 * Routine implementing ALTER DOMAIN SET/DROP DEFAULT statements.
 *
 * 实现 ALTER DOMAIN SET/DROP DEFAULT 语句的例程。
 *
 * Returns ObjectAddress of the modified domain.
 *
 * 返回被修改 domain 的 ObjectAddress。
 */
ObjectAddress
AlterDomainDefault(List *names, Node *defaultRaw)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	ParseState *pstate;
	Relation	rel;
	char	   *defaultValue;
	Node	   *defaultExpr = NULL; /* NULL if no default specified */
					/*
					 *
					 * 未指定默认值时为 NULL。
					 */
	Datum		new_record[Natts_pg_type] = {0};
	bool		new_record_nulls[Natts_pg_type] = {0};
	bool		new_record_repl[Natts_pg_type] = {0};
	HeapTuple	newtuple;
	Form_pg_type typTup;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	/*
	 *
	 * 在类型表中查找该 domain。
	 */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	/*
	 *
	 * 确认它是 domain，并检查用户是否有权执行 ALTER DOMAIN。
	 */
	checkDomainOwner(tup);

	/* Setup new tuple */
	/*
	 *
	 * 准备新元组。
	 */

	/* Store the new default into the tuple */
	/*
	 *
	 * 把新的默认值存入元组。
	 */
	if (defaultRaw)
	{
		/* Create a dummy ParseState for transformExpr */
		/*
		 *
		 * 为 transformExpr 创建一个临时 ParseState。
		 */
		pstate = make_parsestate(NULL);

		/*
		 * Cook the colDef->raw_expr into an expression. Note: Name is
		 * strictly for error message
		 *
		 * 把 colDef->raw_expr 编译成表达式。注意：Name 仅用于错误信息。
		 */
		defaultExpr = cookDefault(pstate, defaultRaw,
								  typTup->typbasetype,
								  typTup->typtypmod,
								  NameStr(typTup->typname),
								  0);

		/*
		 * If the expression is just a NULL constant, we treat the command
		 * like ALTER ... DROP DEFAULT.  (But see note for same test in
		 * DefineDomain.)
		 *
		 * 若表达式只是 NULL 常量，则把该命令当作 ALTER ... DROP DEFAULT。（但请参见 DefineDomain 中同一检查的说明。）
		 */
		if (defaultExpr == NULL ||
			(IsA(defaultExpr, Const) && ((Const *) defaultExpr)->constisnull))
		{
			/* Default is NULL, drop it */
			/*
			 *
			 * 默认值为 NULL，将其删除。
			 */
			defaultExpr = NULL;
			new_record_nulls[Anum_pg_type_typdefaultbin - 1] = true;
			new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
			new_record_nulls[Anum_pg_type_typdefault - 1] = true;
			new_record_repl[Anum_pg_type_typdefault - 1] = true;
		}
		else
		{
			/*
			 * Expression must be stored as a nodeToString result, but we also
			 * require a valid textual representation (mainly to make life
			 * easier for pg_dump).
			 *
			 * 表达式必须以 nodeToString 的结果存储，同时还需要一份有效的文本表示（主要是为了方便 pg_dump）。
			 */
			defaultValue = deparse_expression(defaultExpr,
											  NIL, false, false);

			/*
			 * Form an updated tuple with the new default and write it back.
			 *
			 * 用新的默认值组成更新后的元组并写回。
			 */
			new_record[Anum_pg_type_typdefaultbin - 1] = CStringGetTextDatum(nodeToString(defaultExpr));

			new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
			new_record[Anum_pg_type_typdefault - 1] = CStringGetTextDatum(defaultValue);
			new_record_repl[Anum_pg_type_typdefault - 1] = true;
		}
	}
	else
	{
		/* ALTER ... DROP DEFAULT */
		/*
		 *
		 * 处理 ALTER ... DROP DEFAULT。
		 */
		new_record_nulls[Anum_pg_type_typdefaultbin - 1] = true;
		new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
		new_record_nulls[Anum_pg_type_typdefault - 1] = true;
		new_record_repl[Anum_pg_type_typdefault - 1] = true;
	}

	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel),
								 new_record, new_record_nulls,
								 new_record_repl);

	CatalogTupleUpdate(rel, &tup->t_self, newtuple);

	/* Rebuild dependencies */
	/*
	 *
	 * 重建依赖。
	 */
	GenerateTypeDependencies(newtuple,
							 rel,
							 defaultExpr,
							 NULL,	/* don't have typacl handy */
								/*
								 *
								 * 手头没有 typacl。
								 */
							 0, /* relation kind is n/a */
							    /*
							     *
							     * 关系种类不适用。
							     */
							 false, /* a domain isn't an implicit array */
								/*
								 *
								 * domain 不是隐式数组。
								 */
							 false, /* nor is it any kind of dependent type */
								/*
								 *
								 * 它也不是任何依赖类型。
								 */
							 false, /* don't touch extension membership */
								/*
								 *
								 * 不改动扩展成员关系。
								 */
							 true); /* We do need to rebuild dependencies */
								/*
								 *
								 * 确实需要重建依赖。
								 */

	InvokeObjectPostAlterHook(TypeRelationId, domainoid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(rel, RowExclusiveLock);
	heap_freetuple(newtuple);

	return address;
}

/*
 * AlterDomainNotNull
 *
 * 函数 AlterDomainNotNull。
 *
 * Routine implementing ALTER DOMAIN SET/DROP NOT NULL statements.
 *
 * 实现 ALTER DOMAIN SET/DROP NOT NULL 语句的例程。
 *
 * Returns ObjectAddress of the modified domain.
 *
 * 返回被修改 domain 的 ObjectAddress。
 */
ObjectAddress
AlterDomainNotNull(List *names, bool notNull)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	HeapTuple	tup;
	Form_pg_type typTup;
	ObjectAddress address = InvalidObjectAddress;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	/*
	 *
	 * 在类型表中查找该 domain。
	 */
	typrel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	/*
	 *
	 * 确认它是 domain，并检查用户是否有权执行 ALTER DOMAIN。
	 */
	checkDomainOwner(tup);

	/* Is the domain already set to the desired constraint? */
	/*
	 *
	 * 该 domain 是否已经处于所期望的约束状态？
	 */
	if (typTup->typnotnull == notNull)
	{
		table_close(typrel, RowExclusiveLock);
		return address;
	}

	if (notNull)
	{
		Constraint *constr;

		constr = makeNode(Constraint);
		constr->contype = CONSTR_NOTNULL;
		constr->initially_valid = true;
		constr->location = -1;

		domainAddNotNullConstraint(domainoid, typTup->typnamespace,
								   typTup->typbasetype, typTup->typtypmod,
								   constr, NameStr(typTup->typname), NULL);

		validateDomainNotNullConstraint(domainoid);
	}
	else
	{
		HeapTuple	conTup;
		ObjectAddress conobj;

		conTup = findDomainNotNullConstraint(domainoid);
		if (conTup == NULL)
			elog(ERROR, "could not find not-null constraint on domain \"%s\"", NameStr(typTup->typname));

		ObjectAddressSet(conobj, ConstraintRelationId, ((Form_pg_constraint) GETSTRUCT(conTup))->oid);
		performDeletion(&conobj, DROP_RESTRICT, 0);
	}

	/*
	 * Okay to update pg_type row.  We can scribble on typTup because it's a
	 * copy.
	 *
	 * 可以更新 pg_type 行。typTup 是副本，可以直接改写。
	 */
	typTup->typnotnull = notNull;

	CatalogTupleUpdate(typrel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(TypeRelationId, domainoid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	heap_freetuple(tup);
	table_close(typrel, RowExclusiveLock);

	return address;
}

/*
 * AlterDomainDropConstraint
 *
 * 函数 AlterDomainDropConstraint。
 *
 * Implements the ALTER DOMAIN DROP CONSTRAINT statement
 *
 * 实现 ALTER DOMAIN DROP CONSTRAINT 语句。
 *
 * Returns ObjectAddress of the modified domain.
 *
 * 返回被修改 domain 的 ObjectAddress。
 */
ObjectAddress
AlterDomainDropConstraint(List *names, const char *constrName,
						  DropBehavior behavior, bool missing_ok)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	Relation	rel;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey[3];
	HeapTuple	contup;
	bool		found = false;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	/*
	 *
	 * 在类型表中查找该 domain。
	 */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	/*
	 *
	 * 确认它是 domain，并检查用户是否有权执行 ALTER DOMAIN。
	 */
	checkDomainOwner(tup);

	/* Grab an appropriate lock on the pg_constraint relation */
	/*
	 *
	 * 以适当的锁模式锁定 pg_constraint 关系。
	 */
	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/* Find and remove the target constraint */
	/*
	 *
	 * 查找并删除目标约束。
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainoid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));

	conscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 3, skey);

	/* There can be at most one matching row */
	/*
	 *
	 * 最多只能有一行匹配。
	 */
	if ((contup = systable_getnext(conscan)) != NULL)
	{
		Form_pg_constraint construct = (Form_pg_constraint) GETSTRUCT(contup);
		ObjectAddress conobj;

		if (construct->contype == CONSTRAINT_NOTNULL)
		{
			((Form_pg_type) GETSTRUCT(tup))->typnotnull = false;
			CatalogTupleUpdate(rel, &tup->t_self, tup);
		}

		conobj.classId = ConstraintRelationId;
		conobj.objectId = construct->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);
		found = true;
	}

	/* Clean up after the scan */
	/*
	 *
	 * 扫描结束后清理。
	 */
	systable_endscan(conscan);
	table_close(conrel, RowExclusiveLock);

	if (!found)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of domain \"%s\" does not exist",
							constrName, TypeNameToString(typename))));
		else
			ereport(NOTICE,
					(errmsg("constraint \"%s\" of domain \"%s\" does not exist, skipping",
							constrName, TypeNameToString(typename))));
	}

	/*
	 * We must send out an sinval message for the domain, to ensure that any
	 * dependent plans get rebuilt.  Since this command doesn't change the
	 * domain's pg_type row, that won't happen automatically; do it manually.
	 *
	 * 必须为该 domain 发出 sinval 消息，以确保任何依赖计划被重建。
	 * 由于此命令不修改 domain 的 pg_type 行，这件事不会自动发生，需要手工做。
	 */
	CacheInvalidateHeapTuple(rel, tup, NULL);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * AlterDomainAddConstraint
 *
 * 函数 AlterDomainAddConstraint。
 *
 * Implements the ALTER DOMAIN .. ADD CONSTRAINT statement.
 *
 * 实现 ALTER DOMAIN .. ADD CONSTRAINT 语句。
 */
ObjectAddress
AlterDomainAddConstraint(List *names, Node *newConstraint,
						 ObjectAddress *constrAddr)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	HeapTuple	tup;
	Form_pg_type typTup;
	Constraint *constr;
	char	   *ccbin;
	ObjectAddress address = InvalidObjectAddress;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	/*
	 *
	 * 在类型表中查找该 domain。
	 */
	typrel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	/*
	 *
	 * 确认它是 domain，并检查用户是否有权执行 ALTER DOMAIN。
	 */
	checkDomainOwner(tup);

	if (!IsA(newConstraint, Constraint))
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(newConstraint));

	constr = (Constraint *) newConstraint;

	/* enforced by parser */
	/*
	 *
	 * 由解析器保证。
	 */
	Assert(constr->contype == CONSTR_CHECK || constr->contype == CONSTR_NOTNULL);

	if (constr->contype == CONSTR_CHECK)
	{
		/*
		 * First, process the constraint expression and add an entry to
		 * pg_constraint.
		 *
		 * 首先处理约束表达式，并向 pg_constraint 添加一项。
		 */

		ccbin = domainAddCheckConstraint(domainoid, typTup->typnamespace,
										 typTup->typbasetype, typTup->typtypmod,
										 constr, NameStr(typTup->typname), constrAddr);


		/*
		 * If requested to validate the constraint, test all values stored in
		 * the attributes based on the domain the constraint is being added
		 * to.
		 *
		 * 若要求校验该约束，则测试所有基于此 domain 的属性中已存储的值。
		 */
		if (!constr->skip_validation)
			validateDomainCheckConstraint(domainoid, ccbin);

		/*
		 * We must send out an sinval message for the domain, to ensure that
		 * any dependent plans get rebuilt.  Since this command doesn't change
		 * the domain's pg_type row, that won't happen automatically; do it
		 * manually.
		 *
		 * 必须为该 domain 发出 sinval 消息，以确保任何依赖计划被重建。
		 * 由于此命令不修改 domain 的 pg_type 行，这件事不会自动发生，需要手工做。
		 */
		CacheInvalidateHeapTuple(typrel, tup, NULL);
	}
	else if (constr->contype == CONSTR_NOTNULL)
	{
		/* Is the domain already set NOT NULL? */
		/*
		 *
		 * 该 domain 是否已经是 NOT NULL？
		 */
		if (typTup->typnotnull)
		{
			table_close(typrel, RowExclusiveLock);
			return address;
		}
		domainAddNotNullConstraint(domainoid, typTup->typnamespace,
								   typTup->typbasetype, typTup->typtypmod,
								   constr, NameStr(typTup->typname), constrAddr);

		if (!constr->skip_validation)
			validateDomainNotNullConstraint(domainoid);

		typTup->typnotnull = true;
		CatalogTupleUpdate(typrel, &tup->t_self, tup);
	}

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(typrel, RowExclusiveLock);

	return address;
}

/*
 * AlterDomainValidateConstraint
 *
 * 函数 AlterDomainValidateConstraint。
 *
 * Implements the ALTER DOMAIN .. VALIDATE CONSTRAINT statement.
 *
 * 实现 ALTER DOMAIN .. VALIDATE CONSTRAINT 语句。
 */
ObjectAddress
AlterDomainValidateConstraint(List *names, const char *constrName)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	Relation	conrel;
	HeapTuple	tup;
	Form_pg_constraint con;
	Form_pg_constraint copy_con;
	char	   *conbin;
	SysScanDesc scan;
	Datum		val;
	HeapTuple	tuple;
	HeapTuple	copyTuple;
	ScanKeyData skey[3];
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* Look up the domain in the type table */
	/*
	 *
	 * 在类型表中查找该 domain。
	 */
	typrel = table_open(TypeRelationId, AccessShareLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);

	/* Check it's a domain and check user has permission for ALTER DOMAIN */
	/*
	 *
	 * 确认它是 domain，并检查用户是否有权执行 ALTER DOMAIN。
	 */
	checkDomainOwner(tup);

	/*
	 * Find and check the target constraint
	 *
	 * 查找并检查目标约束。
	 */
	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainoid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));

	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* There can be at most one matching row */
	/*
	 *
	 * 最多只能有一行匹配。
	 */
	if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of domain \"%s\" does not exist",
						constrName, TypeNameToString(typename))));

	con = (Form_pg_constraint) GETSTRUCT(tuple);
	if (con->contype != CONSTRAINT_CHECK)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of domain \"%s\" is not a check constraint",
						constrName, TypeNameToString(typename))));

	val = SysCacheGetAttrNotNull(CONSTROID, tuple, Anum_pg_constraint_conbin);
	conbin = TextDatumGetCString(val);

	validateDomainCheckConstraint(domainoid, conbin);

	/*
	 * Now update the catalog, while we have the door open.
	 *
	 * 趁目录关系仍处于打开状态，现在更新它。
	 */
	copyTuple = heap_copytuple(tuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
	copy_con->convalidated = true;
	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	heap_freetuple(copyTuple);

	systable_endscan(scan);

	table_close(typrel, AccessShareLock);
	table_close(conrel, RowExclusiveLock);

	ReleaseSysCache(tup);

	return address;
}

/*
 * Verify that all columns currently using the domain are not null.
 *
 * 验证当前使用该 domain 的所有列都不是 null。
 */
static void
validateDomainNotNullConstraint(Oid domainoid)
{
	List	   *rels;
	ListCell   *rt;

	/* Fetch relation list with attributes based on this domain */
	/*
	 *
	 * 取出属性基于该 domain 的关系列表。
	 */
	/* ShareLock is sufficient to prevent concurrent data changes */
	/*
	 *
	 * ShareLock 足以防止并发的数据修改。
	 */

	rels = get_rels_with_domain(domainoid, ShareLock);

	foreach(rt, rels)
	{
		RelToCheck *rtc = (RelToCheck *) lfirst(rt);
		Relation	testrel = rtc->rel;
		TupleDesc	tupdesc = RelationGetDescr(testrel);
		TupleTableSlot *slot;
		TableScanDesc scan;
		Snapshot	snapshot;

		/* Scan all tuples in this relation */
		/*
		 *
		 * 扫描该关系中的全部元组。
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(testrel, snapshot, 0, NULL);
		slot = table_slot_create(testrel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			int			i;

			/* Test attributes that are of the domain */
			/*
			 *
			 * 检查属于该 domain 的属性。
			 */
			for (i = 0; i < rtc->natts; i++)
			{
				int			attnum = rtc->atts[i];
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

				if (slot_attisnull(slot, attnum))
				{
					/*
					 * In principle the auxiliary information for this error
					 * should be errdatatype(), but errtablecol() seems
					 * considerably more useful in practice.  Since this code
					 * only executes in an ALTER DOMAIN command, the client
					 * should already know which domain is in question.
					 *
					 * 原则上此错误的辅助信息应为 errdatatype()，但实践中 errtablecol() 有用得多。
					 * 这段代码只在 ALTER DOMAIN 命令中执行，客户端应当已经知道涉及哪个 domain。
					 */
					ereport(ERROR,
							(errcode(ERRCODE_NOT_NULL_VIOLATION),
							 errmsg("column \"%s\" of table \"%s\" contains null values",
									NameStr(attr->attname),
									RelationGetRelationName(testrel)),
							 errtablecol(testrel, attnum)));
				}
			}
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		/* Close each rel after processing, but keep lock */
		/*
		 *
		 * 处理完每个关系后关闭它，但保留锁。
		 */
		table_close(testrel, NoLock);
	}
}

/*
 * Verify that all columns currently using the domain satisfy the given check
 * constraint expression.
 *
 * 验证当前使用该 domain 的所有列都满足给定的 CHECK 约束表达式。
 */
static void
validateDomainCheckConstraint(Oid domainoid, const char *ccbin)
{
	Expr	   *expr = (Expr *) stringToNode(ccbin);
	List	   *rels;
	ListCell   *rt;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *exprstate;

	/* Need an EState to run ExecEvalExpr */
	/*
	 *
	 * 运行 ExecEvalExpr 需要一个 EState。
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);

	/* build execution state for expr */
	/*
	 *
	 * 为 expr 构建执行状态。
	 */
	exprstate = ExecPrepareExpr(expr, estate);

	/* Fetch relation list with attributes based on this domain */
	/*
	 *
	 * 取出属性基于该 domain 的关系列表。
	 */
	/* ShareLock is sufficient to prevent concurrent data changes */
	/*
	 *
	 * ShareLock 足以防止并发的数据修改。
	 */

	rels = get_rels_with_domain(domainoid, ShareLock);

	foreach(rt, rels)
	{
		RelToCheck *rtc = (RelToCheck *) lfirst(rt);
		Relation	testrel = rtc->rel;
		TupleDesc	tupdesc = RelationGetDescr(testrel);
		TupleTableSlot *slot;
		TableScanDesc scan;
		Snapshot	snapshot;

		/* Scan all tuples in this relation */
		/*
		 *
		 * 扫描该关系中的全部元组。
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(testrel, snapshot, 0, NULL);
		slot = table_slot_create(testrel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			int			i;

			/* Test attributes that are of the domain */
			/*
			 *
			 * 检查属于该 domain 的属性。
			 */
			for (i = 0; i < rtc->natts; i++)
			{
				int			attnum = rtc->atts[i];
				Datum		d;
				bool		isNull;
				Datum		conResult;
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

				d = slot_getattr(slot, attnum, &isNull);

				econtext->domainValue_datum = d;
				econtext->domainValue_isNull = isNull;

				conResult = ExecEvalExprSwitchContext(exprstate,
													  econtext,
													  &isNull);

				if (!isNull && !DatumGetBool(conResult))
				{
					/*
					 * In principle the auxiliary information for this error
					 * should be errdomainconstraint(), but errtablecol()
					 * seems considerably more useful in practice.  Since this
					 * code only executes in an ALTER DOMAIN command, the
					 * client should already know which domain is in question,
					 * and which constraint too.
					 *
					 * 原则上此错误的辅助信息应为 errdomainconstraint()，但实践中 errtablecol() 有用得多。
					 * 这段代码只在 ALTER DOMAIN 命令中执行，客户端应当已经知道涉及哪个 domain 以及哪个约束。
					 */
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("column \"%s\" of table \"%s\" contains values that violate the new constraint",
									NameStr(attr->attname),
									RelationGetRelationName(testrel)),
							 errtablecol(testrel, attnum)));
				}
			}

			ResetExprContext(econtext);
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		/* Hold relation lock till commit (XXX bad for concurrency) */
		/*
		 *
		 * 把关系锁一直持有到提交（XXX：对并发不友好）。
		 */
		table_close(testrel, NoLock);
	}

	FreeExecutorState(estate);
}

/*
 * get_rels_with_domain
 *
 * 函数 get_rels_with_domain。
 *
 * Fetch all relations / attributes which are using the domain
 *
 * 取出所有使用该 domain 的关系及其属性。
 *
 * The result is a list of RelToCheck structs, one for each distinct
 * relation, each containing one or more attribute numbers that are of
 * the domain type.  We have opened each rel and acquired the specified lock
 * type on it.
 *
 * 结果是 RelToCheck 结构的列表，每个不同的关系一项，每项包含一个或多个属于该 domain 类型的属性号。
 * 我们已经打开每个关系，并按其指定的锁模式加锁。
 *
 * We support nested domains by including attributes that are of derived
 * domain types.  Current callers do not need to distinguish between attributes
 * that are of exactly the given domain and those that are of derived domains.
 *
 * 通过纳入派生 domain 类型的属性来支持嵌套 domain。
 * 当前调用方不需要区分“恰好是给定 domain”的属性与“派生 domain”的属性。
 *
 * XXX this is completely broken because there is no way to lock the domain
 * to prevent columns from being added or dropped while our command runs.
 * We can partially protect against column drops by locking relations as we
 * come across them, but there is still a race condition (the window between
 * seeing a pg_depend entry and acquiring lock on the relation it references).
 * Also, holding locks on all these relations simultaneously creates a non-
 * trivial risk of deadlock.  We can minimize but not eliminate the deadlock
 * risk by using the weakest suitable lock (ShareLock for most callers).
 *
 * XXX：这完全是坏的，因为无法锁定 domain，以防止命令执行期间列被添加或删除。
 * 我们可以在遇到关系时加锁，从而部分防范列删除，但竞态仍在（看到 pg_depend 项与锁住它所引用的关系之间的窗口）。
 * 同时锁住所有这些关系还会带来不小的死锁风险。使用最弱的合适锁（对多数调用方是 ShareLock）可以降低但不能消除该风险。
 *
 * XXX the API for this is not sufficient to support checking domain values
 * that are inside container types, such as composite types, arrays, or
 * ranges.  Currently we just error out if a container type containing the
 * target domain is stored anywhere.
 *
 * XXX：此 API 不足以支持检查位于容器类型（如复合类型、数组或 range）内部的 domain 值。
 * 目前只要存有包含目标 domain 的容器类型，就直接报错。
 *
 * Generally used for retrieving a list of tests when adding
 * new constraints to a domain.
 *
 * 通常用于在向 domain 添加新约束时取得需要检查的对象列表。
 */
static List *
get_rels_with_domain(Oid domainOid, LOCKMODE lockmode)
{
	List	   *result = NIL;
	char	   *domainTypeName = format_type_be(domainOid);
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	Assert(lockmode != NoLock);

	/* since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 由于此函数会递归，可能被推到栈溢出。
	 */
	check_stack_depth();

	/*
	 * We scan pg_depend to find those things that depend on the domain. (We
	 * assume we can ignore refobjsubid for a domain.)
	 *
	 * 扫描 pg_depend，找出依赖该 domain 的对象。（假定对 domain 可以忽略 refobjsubid。）
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		RelToCheck *rtc = NULL;
		ListCell   *rellist;
		Form_pg_attribute pg_att;
		int			ptr;

		/* Check for directly dependent types */
		/*
		 *
		 * 检查直接依赖的类型。
		 */
		if (pg_depend->classid == TypeRelationId)
		{
			if (get_typtype(pg_depend->objid) == TYPTYPE_DOMAIN)
			{
				/*
				 * This is a sub-domain, so recursively add dependent columns
				 * to the output list.  This is a bit inefficient since we may
				 * fail to combine RelToCheck entries when attributes of the
				 * same rel have different derived domain types, but it's
				 * probably not worth improving.
				 *
				 * 这是一个子 domain，因此递归把依赖列加入输出列表。
				 * 这有点低效，因为同一关系上属性属于不同派生 domain 时，可能无法合并 RelToCheck 项，但大概不值得优化。
				 */
				result = list_concat(result,
									 get_rels_with_domain(pg_depend->objid,
														  lockmode));
			}
			else
			{
				/*
				 * Otherwise, it is some container type using the domain, so
				 * fail if there are any columns of this type.
				 *
				 * 否则，它是使用该 domain 的某种容器类型；若存在该类型的列，则失败。
				 */
				find_composite_type_dependencies(pg_depend->objid,
												 NULL,
												 domainTypeName);
			}
			continue;
		}

		/* Else, ignore dependees that aren't user columns of relations */
		/*
		 *
		 * 否则，忽略那些不是关系中用户列的依赖对象。
		 */
		/* (we assume system columns are never of domain types) */
		/*
		 *
		 * （假定系统列的类型绝不会是 domain）
		 */
		if (pg_depend->classid != RelationRelationId ||
			pg_depend->objsubid <= 0)
			continue;

		/* See if we already have an entry for this relation */
		/*
		 *
		 * 查看是否已有该关系的项。
		 */
		foreach(rellist, result)
		{
			RelToCheck *rt = (RelToCheck *) lfirst(rellist);

			if (RelationGetRelid(rt->rel) == pg_depend->objid)
			{
				rtc = rt;
				break;
			}
		}

		if (rtc == NULL)
		{
			/* First attribute found for this relation */
			/*
			 *
			 * 该关系上找到的第一个相关属性。
			 */
			Relation	rel;

			/* Acquire requested lock on relation */
			/*
			 *
			 * 按请求的锁模式锁定该关系。
			 */
			rel = relation_open(pg_depend->objid, lockmode);

			/*
			 * Check to see if rowtype is stored anyplace as a composite-type
			 * column; if so we have to fail, for now anyway.
			 *
			 * 检查该行类型是否作为复合类型列存放在某处；若是，目前只能失败。
			 */
			if (OidIsValid(rel->rd_rel->reltype))
				find_composite_type_dependencies(rel->rd_rel->reltype,
												 NULL,
												 domainTypeName);

			/*
			 * Otherwise, we can ignore relations except those with both
			 * storage and user-chosen column types.
			 *
			 * 否则，可以忽略那些不同时具备存储和用户选定列类型的关系。
			 *
			 * XXX If an index-only scan could satisfy "col::some_domain" from
			 * a suitable expression index, this should also check expression
			 * index columns.
			 *
			 * XXX：若仅索引扫描能借助合适的表达式索引满足 "col::some_domain"，这里还应检查表达式索引列。
			 */
			if (rel->rd_rel->relkind != RELKIND_RELATION &&
				rel->rd_rel->relkind != RELKIND_MATVIEW)
			{
				relation_close(rel, lockmode);
				continue;
			}

			/* Build the RelToCheck entry with enough space for all atts */
			/*
			 *
			 * 建立 RelToCheck 项，并为全部属性预留空间。
			 */
			rtc = (RelToCheck *) palloc(sizeof(RelToCheck));
			rtc->rel = rel;
			rtc->natts = 0;
			rtc->atts = (int *) palloc(sizeof(int) * RelationGetNumberOfAttributes(rel));
			result = lappend(result, rtc);
		}

		/*
		 * Confirm column has not been dropped, and is of the expected type.
		 * This defends against an ALTER DROP COLUMN occurring just before we
		 * acquired lock ... but if the whole table were dropped, we'd still
		 * have a problem.
		 *
		 * 确认列尚未被删除，且类型符合预期。
		 * 这是为了防范在我们加锁之前刚好发生 ALTER DROP COLUMN；若整表被删除，问题仍然存在。
		 */
		if (pg_depend->objsubid > RelationGetNumberOfAttributes(rtc->rel))
			continue;
		pg_att = TupleDescAttr(rtc->rel->rd_att, pg_depend->objsubid - 1);
		if (pg_att->attisdropped || pg_att->atttypid != domainOid)
			continue;

		/*
		 * Okay, add column to result.  We store the columns in column-number
		 * order; this is just a hack to improve predictability of regression
		 * test output ...
		 *
		 * 可以把列加入结果了。列按列号顺序存放；这只是为了让回归测试输出更可预测。
		 */
		Assert(rtc->natts < RelationGetNumberOfAttributes(rtc->rel));

		ptr = rtc->natts++;
		while (ptr > 0 && rtc->atts[ptr - 1] > pg_depend->objsubid)
		{
			rtc->atts[ptr] = rtc->atts[ptr - 1];
			ptr--;
		}
		rtc->atts[ptr] = pg_depend->objsubid;
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	return result;
}

/*
 * checkDomainOwner
 *
 * 函数 checkDomainOwner。
 *
 * Check that the type is actually a domain and that the current user
 * has permission to do ALTER DOMAIN on it.  Throw an error if not.
 *
 * 确认该类型确实是 domain，且当前用户有权对其执行 ALTER DOMAIN。否则报错。
 */
void
checkDomainOwner(HeapTuple tup)
{
	Form_pg_type typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Check that this is actually a domain */
	/*
	 *
	 * 确认这确实是 domain。
	 */
	if (typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typTup->oid))));

	/* Permission check: must own type */
	/*
	 *
	 * 权限检查：必须拥有该类型。
	 */
	if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);
}

/*
 * domainAddCheckConstraint - code shared between CREATE and ALTER DOMAIN
 *
 * domainAddCheckConstraint：CREATE DOMAIN 与 ALTER DOMAIN 共用的代码。
 */
static char *
domainAddCheckConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
						 int typMod, Constraint *constr,
						 const char *domainName, ObjectAddress *constrAddr)
{
	Node	   *expr;
	char	   *ccbin;
	ParseState *pstate;
	CoerceToDomainValue *domVal;
	Oid			ccoid;

	Assert(constr->contype == CONSTR_CHECK);

	/*
	 * Assign or validate constraint name
	 *
	 * 指定或校验约束名。
	 */
	if (constr->conname)
	{
		if (ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
								 domainOid,
								 constr->conname))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for domain \"%s\" already exists",
							constr->conname, domainName)));
	}
	else
		constr->conname = ChooseConstraintName(domainName,
											   NULL,
											   "check",
											   domainNamespace,
											   NIL);

	/*
	 * Convert the A_EXPR in raw_expr into an EXPR
	 *
	 * 把 raw_expr 中的 A_EXPR 转换成 EXPR。
	 */
	pstate = make_parsestate(NULL);

	/*
	 * Set up a CoerceToDomainValue to represent the occurrence of VALUE in
	 * the expression.  Note that it will appear to have the type of the base
	 * type, not the domain.  This seems correct since within the check
	 * expression, we should not assume the input value can be considered a
	 * member of the domain.
	 *
	 * 设置 CoerceToDomainValue 来表示表达式中出现的 VALUE。
	 * 注意它看起来会具有基类型而不是 domain 的类型。这是合理的，因为在检查表达式内部，不应假定输入值已经属于该 domain。
	 */
	domVal = makeNode(CoerceToDomainValue);
	domVal->typeId = baseTypeOid;
	domVal->typeMod = typMod;
	domVal->collation = get_typcollation(baseTypeOid);
	domVal->location = -1;		/* will be set when/if used */
					/*
					 *
					 * 在使用时再设置（如果会用到）。
					 */

	pstate->p_pre_columnref_hook = replace_domain_constraint_value;
	pstate->p_ref_hook_state = domVal;

	expr = transformExpr(pstate, constr->raw_expr, EXPR_KIND_DOMAIN_CHECK);

	/*
	 * Make sure it yields a boolean result.
	 *
	 * 确保结果为 boolean。
	 */
	expr = coerce_to_boolean(pstate, expr, "CHECK");

	/*
	 * Fix up collation information.
	 *
	 * 整理 collation 信息。
	 */
	assign_expr_collations(pstate, expr);

	/*
	 * Domains don't allow variables (this is probably dead code now that
	 * add_missing_from is history, but let's be sure).
	 *
	 * domain 不允许变量（add_missing_from 已成为历史后，这段很可能是死代码，但仍加以确认）。
	 */
	if (pstate->p_rtable != NIL ||
		contain_var_clause(expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use table references in domain check constraint")));

	/*
	 * Convert to string form for storage.
	 *
	 * 转换成字符串形式以便存储。
	 */
	ccbin = nodeToString(expr);

	/*
	 * Store the constraint in pg_constraint
	 *
	 * 把约束存入 pg_constraint。
	 */
	ccoid =
		CreateConstraintEntry(constr->conname,	/* Constraint Name */
							/*
							 *
							 * 约束名。
							 */
							  domainNamespace,	/* namespace */
										/*
										 *
										 * 命名空间。
										 */
							  CONSTRAINT_CHECK, /* Constraint Type */
									    /*
									     *
									     * 约束类型。
									     */
							  false,	/* Is Deferrable */
									/*
									 *
									 * 是否可延迟。
									 */
							  false,	/* Is Deferred */
									/*
									 *
									 * 是否已延迟。
									 */
							  true, /* Is Enforced */
								/*
								 *
								 * 是否强制执行。
								 */
							  !constr->skip_validation, /* Is Validated */
										    /*
										     *
										     * 是否已验证。
										     */
							  InvalidOid,	/* no parent constraint */
									/*
									 *
									 * 没有父约束。
									 */
							  InvalidOid,	/* not a relation constraint */
									/*
									 *
									 * 不是关系约束。
									 */
							  NULL,
							  0,
							  0,
							  domainOid,	/* domain constraint */
									/*
									 *
									 * domain 约束。
									 */
							  InvalidOid,	/* no associated index */
									/*
									 *
									 * 没有关联索引。
									 */
							  InvalidOid,	/* Foreign key fields */
									/*
									 *
									 * 外键字段。
									 */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* not an exclusion constraint */
								/*
								 *
								 * 不是排除约束。
								 */
							  expr, /* Tree form of check constraint */
								/*
								 *
								 * CHECK 约束的树形式。
								 */
							  ccbin,	/* Binary form of check constraint */
									/*
									 *
									 * CHECK 约束的二进制形式。
									 */
							  true, /* is local */
								/*
								 *
								 * 是否为本地约束。
								 */
							  0,	/* inhcount */
								/*
								 *
								 * inhcount。
								 */
							  false,	/* connoinherit */
									/*
									 *
									 * connoinherit。
									 */
							  false,	/* conperiod */
									/*
									 *
									 * conperiod。
									 */
							  false);	/* is_internal */
									/*
									 *
									 * is_internal。
									 */
	if (constrAddr)
		ObjectAddressSet(*constrAddr, ConstraintRelationId, ccoid);

	/*
	 * Return the compiled constraint expression so the calling routine can
	 * perform any additional required tests.
	 *
	 * 返回已编译的约束表达式，供调用方做任何额外的必要检查。
	 */
	return ccbin;
}

/* Parser pre_columnref_hook for domain CHECK constraint parsing */
/*
 *
 * 供解析 domain 的 CHECK 约束使用的解析器 pre_columnref_hook。
 */
static Node *
replace_domain_constraint_value(ParseState *pstate, ColumnRef *cref)
{
	/*
	 * Check for a reference to "value", and if that's what it is, replace
	 * with a CoerceToDomainValue as prepared for us by
	 * domainAddCheckConstraint. (We handle VALUE as a name, not a keyword, to
	 * avoid breaking a lot of applications that have used VALUE as a column
	 * name in the past.)
	 *
	 * 检查是否引用了 "value"；若是，则替换为 domainAddCheckConstraint 准备好的 CoerceToDomainValue。
	 * 把 VALUE 当作名字而不是关键字，以免破坏过去把 VALUE 用作列名的大量应用。
	 */
	if (list_length(cref->fields) == 1)
	{
		Node	   *field1 = (Node *) linitial(cref->fields);
		char	   *colname;

		colname = strVal(field1);
		if (strcmp(colname, "value") == 0)
		{
			CoerceToDomainValue *domVal = copyObject(pstate->p_ref_hook_state);

			/* Propagate location knowledge, if any */
			/*
			 *
			 * 传递位置信息（如果有）。
			 */
			domVal->location = cref->location;
			return (Node *) domVal;
		}
	}
	return NULL;
}

/*
 * domainAddNotNullConstraint - code shared between CREATE and ALTER DOMAIN
 *
 * domainAddNotNullConstraint：CREATE DOMAIN 与 ALTER DOMAIN 共用的代码。
 */
static void
domainAddNotNullConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
						   int typMod, Constraint *constr,
						   const char *domainName, ObjectAddress *constrAddr)
{
	Oid			ccoid;

	Assert(constr->contype == CONSTR_NOTNULL);

	/*
	 * Assign or validate constraint name
	 *
	 * 指定或校验约束名。
	 */
	if (constr->conname)
	{
		if (ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
								 domainOid,
								 constr->conname))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for domain \"%s\" already exists",
							constr->conname, domainName)));
	}
	else
		constr->conname = ChooseConstraintName(domainName,
											   NULL,
											   "not_null",
											   domainNamespace,
											   NIL);

	/*
	 * Store the constraint in pg_constraint
	 *
	 * 把约束存入 pg_constraint。
	 */
	ccoid =
		CreateConstraintEntry(constr->conname,	/* Constraint Name */
							/*
							 *
							 * 约束名。
							 */
							  domainNamespace,	/* namespace */
										/*
										 *
										 * 命名空间。
										 */
							  CONSTRAINT_NOTNULL,	/* Constraint Type */
										/*
										 *
										 * 约束类型。
										 */
							  false,	/* Is Deferrable */
									/*
									 *
									 * 是否可延迟。
									 */
							  false,	/* Is Deferred */
									/*
									 *
									 * 是否已延迟。
									 */
							  true, /* Is Enforced */
								/*
								 *
								 * 是否强制执行。
								 */
							  !constr->skip_validation, /* Is Validated */
										    /*
										     *
										     * 是否已验证。
										     */
							  InvalidOid,	/* no parent constraint */
									/*
									 *
									 * 没有父约束。
									 */
							  InvalidOid,	/* not a relation constraint */
									/*
									 *
									 * 不是关系约束。
									 */
							  NULL,
							  0,
							  0,
							  domainOid,	/* domain constraint */
									/*
									 *
									 * domain 约束。
									 */
							  InvalidOid,	/* no associated index */
									/*
									 *
									 * 没有关联索引。
									 */
							  InvalidOid,	/* Foreign key fields */
									/*
									 *
									 * 外键字段。
									 */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* not an exclusion constraint */
								/*
								 *
								 * 不是排除约束。
								 */
							  NULL,
							  NULL,
							  true, /* is local */
								/*
								 *
								 * 是否为本地约束。
								 */
							  0,	/* inhcount */
								/*
								 *
								 * inhcount。
								 */
							  false,	/* connoinherit */
									/*
									 *
									 * connoinherit。
									 */
							  false,	/* conperiod */
									/*
									 *
									 * conperiod。
									 */
							  false);	/* is_internal */
									/*
									 *
									 * is_internal。
									 */

	if (constrAddr)
		ObjectAddressSet(*constrAddr, ConstraintRelationId, ccoid);
}


/*
 * Execute ALTER TYPE RENAME
 *
 * 执行 ALTER TYPE RENAME。
 */
ObjectAddress
RenameType(RenameStmt *stmt)
{
	List	   *names = castNode(List, stmt->object);
	const char *newTypeName = stmt->newname;
	TypeName   *typename;
	Oid			typeOid;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;
	ObjectAddress address;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* Look up the type in the type table */
	/*
	 *
	 * 在类型表中查找该类型。
	 */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* check permissions on type */
	/*
	 *
	 * 检查对该类型的权限。
	 */
	if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/* ALTER DOMAIN used on a non-domain? */
	/*
	 *
	 * 对非 domain 使用了 ALTER DOMAIN？
	 */
	if (stmt->renameType == OBJECT_DOMAIN && typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/*
	 * If it's a composite type, we need to check that it really is a
	 * free-standing composite type, and not a table's rowtype. We want people
	 * to use ALTER TABLE not ALTER TYPE for that case.
	 *
	 * 若它是复合类型，需要确认它确实是独立的复合类型，而不是表的行类型。
	 * 那种情况应使用 ALTER TABLE，而不是 ALTER TYPE。
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE &&
		get_rel_relkind(typTup->typrelid) != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
		/*
		 *
		 * 翻译者注：%s 是一条 SQL ALTER 命令。
		 */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	/* don't allow direct alteration of array types, either */
	/*
	 *
	 * 同样不允许直接修改数组类型。
	 */
	if (IsTrueArrayType(typTup))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(typTup->typelem))));

	/* we do allow separate renaming of multirange types, though */
	/*
	 *
	 * 不过我们允许单独重命名 multirange 类型。
	 */

	/*
	 * If type is composite we need to rename associated pg_class entry too.
	 * RenameRelationInternal will call RenameTypeInternal automatically.
	 *
	 * 若类型是复合类型，还需要重命名关联的 pg_class 项。RenameRelationInternal 会自动调用 RenameTypeInternal。
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE)
		RenameRelationInternal(typTup->typrelid, newTypeName, false, false);
	else
		RenameTypeInternal(typeOid, newTypeName,
						   typTup->typnamespace);

	ObjectAddressSet(address, TypeRelationId, typeOid);
	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change the owner of a type.
 *
 * 更改类型的属主。
 */
ObjectAddress
AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype)
{
	TypeName   *typename;
	Oid			typeOid;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	newtup;
	Form_pg_type typTup;
	AclResult	aclresult;
	ObjectAddress address;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);

	/* Use LookupTypeName here so that shell types can be processed */
	/*
	 *
	 * 这里使用 LookupTypeName，以便能处理 shell type。
	 */
	tup = LookupTypeName(NULL, typename, NULL, false);
	if (tup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typename))));
	typeOid = typeTypeId(tup);

	/* Copy the syscache entry so we can scribble on it below */
	/*
	 *
	 * 复制 syscache 项，以便下面就地修改。
	 */
	newtup = heap_copytuple(tup);
	ReleaseSysCache(tup);
	tup = newtup;
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* Don't allow ALTER DOMAIN on a type */
	/*
	 *
	 * 不允许对普通类型执行 ALTER DOMAIN。
	 */
	if (objecttype == OBJECT_DOMAIN && typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/*
	 * If it's a composite type, we need to check that it really is a
	 * free-standing composite type, and not a table's rowtype. We want people
	 * to use ALTER TABLE not ALTER TYPE for that case.
	 *
	 * 若它是复合类型，需要确认它确实是独立的复合类型，而不是表的行类型。
	 * 那种情况应使用 ALTER TABLE，而不是 ALTER TYPE。
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE &&
		get_rel_relkind(typTup->typrelid) != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
		/*
		 *
		 * 翻译者注：%s 是一条 SQL ALTER 命令。
		 */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	/* don't allow direct alteration of array types, either */
	/*
	 *
	 * 同样不允许直接修改数组类型。
	 */
	if (IsTrueArrayType(typTup))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(typTup->typelem))));

	/* don't allow direct alteration of multirange types, either */
	/*
	 *
	 * 同样不允许直接修改 multirange 类型。
	 */
	if (typTup->typtype == TYPTYPE_MULTIRANGE)
	{
		Oid			rangetype = get_multirange_range(typeOid);

		/* We don't expect get_multirange_range to fail, but cope if so */
		/*
		 *
		 * 不指望 get_multirange_range 失败，但若失败也能应付。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter multirange type %s",
						format_type_be(typeOid)),
				 OidIsValid(rangetype) ?
				 errhint("You can alter type %s, which will alter the multirange type as well.",
						 format_type_be(rangetype)) : 0));
	}

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 *
	 * 若新属主与现属主相同，则视为命令已成功。这是为了转储恢复。
	 */
	if (typTup->typowner != newOwnerId)
	{
		/* Superusers can always do it */
		/*
		 *
		 * 超级用户始终可以执行。
		 */
		if (!superuser())
		{
			/* Otherwise, must be owner of the existing object */
			/*
			 *
			 * 否则，必须是现有对象的属主。
			 */
			if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
				aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);

			/* Must be able to become new owner */
			/*
			 *
			 * 必须能够成为新属主。
			 */
			check_can_set_role(GetUserId(), newOwnerId);

			/* New owner must have CREATE privilege on namespace */
			/*
			 *
			 * 新属主必须对命名空间拥有 CREATE 权限。
			 */
			aclresult = object_aclcheck(NamespaceRelationId, typTup->typnamespace,
										newOwnerId,
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_SCHEMA,
							   get_namespace_name(typTup->typnamespace));
		}

		AlterTypeOwner_oid(typeOid, newOwnerId, true);
	}

	ObjectAddressSet(address, TypeRelationId, typeOid);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * AlterTypeOwner_oid - change type owner unconditionally
 *
 * AlterTypeOwner_oid：无条件更改类型属主。
 *
 * This function recurses to handle dependent types (arrays and multiranges).
 * It invokes any necessary access object hooks.  If hasDependEntry is true,
 * this function modifies the pg_shdepend entry appropriately (this should be
 * passed as false only for table rowtypes and dependent types).
 *
 * 此函数递归处理依赖类型（数组与 multirange）。它会调用必要的对象访问钩子。
 * 若 hasDependEntry 为真，则相应修改 pg_shdepend 项（仅对表行类型和依赖类型应传入 false）。
 *
 * This is used by ALTER TABLE/TYPE OWNER commands, as well as by REASSIGN
 * OWNED BY.  It assumes the caller has done all needed checks.
 *
 * 供 ALTER TABLE/TYPE OWNER 以及 REASSIGN OWNED BY 使用。假定调用方已完成所有必要检查。
 */
void
AlterTypeOwner_oid(Oid typeOid, Oid newOwnerId, bool hasDependEntry)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/*
	 * If it's a composite type, invoke ATExecChangeOwner so that we fix up
	 * the pg_class entry properly.  That will call back to
	 * AlterTypeOwnerInternal to take care of the pg_type entry(s).
	 *
	 * 若它是复合类型，则调用 ATExecChangeOwner，以便正确修正 pg_class 项。
	 * 该函数会回调 AlterTypeOwnerInternal 来处理 pg_type 项。
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE)
		ATExecChangeOwner(typTup->typrelid, newOwnerId, true, AccessExclusiveLock);
	else
		AlterTypeOwnerInternal(typeOid, newOwnerId);

	/* Update owner dependency reference */
	/*
	 *
	 * 更新属主依赖引用。
	 */
	if (hasDependEntry)
		changeDependencyOnOwner(TypeRelationId, typeOid, newOwnerId);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	ReleaseSysCache(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * AlterTypeOwnerInternal - bare-bones type owner change.
 *
 * AlterTypeOwnerInternal：只修改类型属主的最简实现。
 *
 * This routine simply modifies the owner of a pg_type entry, and recurses
 * to handle any dependent types.
 *
 * 此例程只修改 pg_type 项的属主，并递归处理任何依赖类型。
 */
void
AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;
	Datum		repl_val[Natts_pg_type];
	bool		repl_null[Natts_pg_type];
	bool		repl_repl[Natts_pg_type];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	repl_repl[Anum_pg_type_typowner - 1] = true;
	repl_val[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(newOwnerId);

	aclDatum = heap_getattr(tup,
							Anum_pg_type_typacl,
							RelationGetDescr(rel),
							&isNull);
	/* Null ACLs do not require changes */
	/*
	 *
	 * 空的 ACL 不需要修改。
	 */
	if (!isNull)
	{
		newAcl = aclnewowner(DatumGetAclP(aclDatum),
							 typTup->typowner, newOwnerId);
		repl_repl[Anum_pg_type_typacl - 1] = true;
		repl_val[Anum_pg_type_typacl - 1] = PointerGetDatum(newAcl);
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
							repl_repl);

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* If it has an array type, update that too */
	/*
	 *
	 * 若它有数组类型，也一并更新。
	 */
	if (OidIsValid(typTup->typarray))
		AlterTypeOwnerInternal(typTup->typarray, newOwnerId);

	/* If it is a range type, update the associated multirange too */
	/*
	 *
	 * 若它是 range 类型，也更新关联的 multirange。
	 */
	if (typTup->typtype == TYPTYPE_RANGE)
	{
		Oid			multirange_typeid = get_range_multirange(typeOid);

		if (!OidIsValid(multirange_typeid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("could not find multirange type for data type %s",
							format_type_be(typeOid))));
		AlterTypeOwnerInternal(multirange_typeid, newOwnerId);
	}

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	table_close(rel, RowExclusiveLock);
}

/*
 * Execute ALTER TYPE SET SCHEMA
 *
 * 执行 ALTER TYPE SET SCHEMA。
 */
ObjectAddress
AlterTypeNamespace(List *names, const char *newschema, ObjectType objecttype,
				   Oid *oldschema)
{
	TypeName   *typename;
	Oid			typeOid;
	Oid			nspOid;
	Oid			oldNspOid;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* Don't allow ALTER DOMAIN on a non-domain type */
	/*
	 *
	 * 不允许对非 domain 类型执行 ALTER DOMAIN。
	 */
	if (objecttype == OBJECT_DOMAIN && get_typtype(typeOid) != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/* get schema OID and check its permissions */
	/*
	 *
	 * 取得模式 OID 并检查其权限。
	 */
	nspOid = LookupCreationNamespace(newschema);

	objsMoved = new_object_addresses();
	oldNspOid = AlterTypeNamespace_oid(typeOid, nspOid, false, objsMoved);
	free_object_addresses(objsMoved);

	if (oldschema)
		*oldschema = oldNspOid;

	ObjectAddressSet(myself, TypeRelationId, typeOid);

	return myself;
}

/*
 * ALTER TYPE SET SCHEMA, where the caller has already looked up the OIDs
 * of the type and the target schema and checked the schema's privileges.
 *
 * ALTER TYPE SET SCHEMA 的入口：调用方已查出类型与目标模式的 OID，并已检查模式权限。
 *
 * If ignoreDependent is true, we silently ignore dependent types
 * (array types and table rowtypes) rather than raising errors.
 *
 * 若 ignoreDependent 为真，则静默忽略依赖类型（数组类型与表行类型），而不是报错。
 *
 * This entry point is exported for use by AlterObjectNamespace_oid,
 * which doesn't want errors when it passes OIDs of dependent types.
 *
 * 此入口导出给 AlterObjectNamespace_oid 使用，它在传入依赖类型的 OID 时不希望报错。
 *
 * Returns the type's old namespace OID, or InvalidOid if we did nothing.
 *
 * 返回类型原先的命名空间 OID；若什么都没做则返回 InvalidOid。
 */
Oid
AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, bool ignoreDependent,
					   ObjectAddresses *objsMoved)
{
	Oid			elemOid;

	/* check permissions on type */
	/*
	 *
	 * 检查对该类型的权限。
	 */
	if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/* don't allow direct alteration of array types */
	/*
	 *
	 * 不允许直接修改数组类型。
	 */
	elemOid = get_element_type(typeOid);
	if (OidIsValid(elemOid) && get_array_type(elemOid) == typeOid)
	{
		if (ignoreDependent)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(elemOid))));
	}

	/* and do the work */
	/*
	 *
	 * 然后完成实际工作。
	 */
	return AlterTypeNamespaceInternal(typeOid, nspOid,
									  false,	/* isImplicitArray */
											/*
											 *
											 * isImplicitArray。
											 */
									  ignoreDependent,	/* ignoreDependent */
												/*
												 *
												 * ignoreDependent。
												 */
									  true, /* errorOnTableType */
										/*
										 *
										 * errorOnTableType。
										 */
									  objsMoved);
}

/*
 * Move specified type to new namespace.
 *
 * 把指定类型移动到新的命名空间。
 *
 * Caller must have already checked privileges.
 *
 * 调用方必须已经检查过权限。
 *
 * The function automatically recurses to process the type's array type,
 * if any.  isImplicitArray should be true only when doing this internal
 * recursion (outside callers must never try to move an array type directly).
 *
 * 函数会自动递归处理该类型的数组类型（如果有）。
 * isImplicitArray 只应在这次内部递归时为真（外部调用者绝不能直接移动数组类型）。
 *
 * If ignoreDependent is true, we silently don't process table types.
 *
 * 若 ignoreDependent 为真，则静默跳过表类型。
 *
 * If errorOnTableType is true, the function errors out if the type is
 * a table type.  ALTER TABLE has to be used to move a table to a new
 * namespace.  (This flag is ignored if ignoreDependent is true.)
 *
 * 若 errorOnTableType 为真，且该类型是表类型，则报错。
 * 把表移到新的命名空间必须使用 ALTER TABLE。（若 ignoreDependent 为真，则忽略此标志。）
 *
 * We also do nothing if the type is already listed in *objsMoved.
 * After a successful move, we add the type to *objsMoved.
 *
 * 若类型已列在 *objsMoved 中，同样什么都不做。成功移动后，把该类型加入 *objsMoved。
 *
 * Returns the type's old namespace OID, or InvalidOid if we did nothing.
 *
 * 返回类型原先的命名空间 OID；若什么都没做则返回 InvalidOid。
 */
Oid
AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool ignoreDependent,
						   bool errorOnTableType,
						   ObjectAddresses *objsMoved)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typform;
	Oid			oldNspOid;
	Oid			arrayOid;
	bool		isCompositeType;
	ObjectAddress thisobj;

	/*
	 * Make sure we haven't moved this object previously.
	 *
	 * 确保此前没有移动过该对象。
	 */
	thisobj.classId = TypeRelationId;
	thisobj.objectId = typeOid;
	thisobj.objectSubId = 0;

	if (object_address_present(&thisobj, objsMoved))
		return InvalidOid;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typform = (Form_pg_type) GETSTRUCT(tup);

	oldNspOid = typform->typnamespace;
	arrayOid = typform->typarray;

	/* If the type is already there, we scan skip these next few checks. */
	/*
	 *
	 * 若类型已经位于目标模式，则跳过接下来的几项检查。
	 */
	if (oldNspOid != nspOid)
	{
		/* common checks on switching namespaces */
		/*
		 *
		 * 切换命名空间时的通用检查。
		 */
		CheckSetNamespace(oldNspOid, nspOid);

		/* check for duplicate name (more friendly than unique-index failure) */
		/*
		 *
		 * 检查重名（比唯一索引失败更友好）。
		 */
		if (SearchSysCacheExists2(TYPENAMENSP,
								  NameGetDatum(&typform->typname),
								  ObjectIdGetDatum(nspOid)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists in schema \"%s\"",
							NameStr(typform->typname),
							get_namespace_name(nspOid))));
	}

	/* Detect whether type is a composite type (but not a table rowtype) */
	/*
	 *
	 * 判断该类型是否为复合类型（但不是表的行类型）。
	 */
	isCompositeType =
		(typform->typtype == TYPTYPE_COMPOSITE &&
		 get_rel_relkind(typform->typrelid) == RELKIND_COMPOSITE_TYPE);

	/* Enforce not-table-type if requested */
	/*
	 *
	 * 若调用方要求，则禁止表类型。
	 */
	if (typform->typtype == TYPTYPE_COMPOSITE && !isCompositeType)
	{
		if (ignoreDependent)
		{
			table_close(rel, RowExclusiveLock);
			return InvalidOid;
		}
		if (errorOnTableType)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("%s is a table's row type",
							format_type_be(typeOid)),
			/* translator: %s is an SQL ALTER command */
			/*
			 *
			 * 翻译者注：%s 是一条 SQL ALTER 命令。
			 */
					 errhint("Use %s instead.", "ALTER TABLE")));
	}

	if (oldNspOid != nspOid)
	{
		/* OK, modify the pg_type row */
		/*
		 *
		 * 可以修改 pg_type 行了。
		 */

		/* tup is a copy, so we can scribble directly on it */
		/*
		 *
		 * tup 是副本，可以直接改写。
		 */
		typform->typnamespace = nspOid;

		CatalogTupleUpdate(rel, &tup->t_self, tup);
	}

	/*
	 * Composite types have pg_class entries.
	 *
	 * 复合类型在 pg_class 中有对应项。
	 *
	 * We need to modify the pg_class tuple as well to reflect the change of
	 * schema.
	 *
	 * 还需要修改 pg_class 元组，以反映模式变更。
	 */
	if (isCompositeType)
	{
		Relation	classRel;

		classRel = table_open(RelationRelationId, RowExclusiveLock);

		AlterRelationNamespaceInternal(classRel, typform->typrelid,
									   oldNspOid, nspOid,
									   false, objsMoved);

		table_close(classRel, RowExclusiveLock);

		/*
		 * Check for constraints associated with the composite type (we don't
		 * currently support this, but probably will someday).
		 *
		 * 检查与该复合类型关联的约束（目前不支持，但将来可能会）。
		 */
		AlterConstraintNamespaces(typform->typrelid, oldNspOid,
								  nspOid, false, objsMoved);
	}
	else
	{
		/* If it's a domain, it might have constraints */
		/*
		 *
		 * 若它是 domain，则可能带有约束。
		 */
		if (typform->typtype == TYPTYPE_DOMAIN)
			AlterConstraintNamespaces(typeOid, oldNspOid, nspOid, true,
									  objsMoved);
	}

	/*
	 * Update dependency on schema, if any --- a table rowtype has not got
	 * one, and neither does an implicit array.
	 *
	 * 更新对模式的依赖（如果有）。表行类型没有这种依赖，隐式数组也没有。
	 */
	if (oldNspOid != nspOid &&
		(isCompositeType || typform->typtype != TYPTYPE_COMPOSITE) &&
		!isImplicitArray)
		if (changeDependencyFor(TypeRelationId, typeOid,
								NamespaceRelationId, oldNspOid, nspOid) != 1)
			elog(ERROR, "could not change schema dependency for type \"%s\"",
				 format_type_be(typeOid));

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	add_exact_object_address(&thisobj, objsMoved);

	/* Recursively alter the associated array type, if any */
	/*
	 *
	 * 若有关联的数组类型，则递归修改它。
	 */
	if (OidIsValid(arrayOid))
		AlterTypeNamespaceInternal(arrayOid, nspOid,
								   true,	/* isImplicitArray */
										/*
										 *
										 * isImplicitArray。
										 */
								   false,	/* ignoreDependent */
										/*
										 *
										 * ignoreDependent。
										 */
								   true,	/* errorOnTableType */
										/*
										 *
										 * errorOnTableType。
										 */
								   objsMoved);

	return oldNspOid;
}

/*
 * AlterType
 *		ALTER TYPE <type> SET (option = ...)
 *
 * AlterType：执行 ALTER TYPE name SET (option = ...)。
 *
 * NOTE: the set of changes that can be allowed here is constrained by many
 * non-obvious implementation restrictions.  Tread carefully when considering
 * adding new flexibility.
 *
 * 注意：这里允许的变更集合受到许多不明显的实现限制。考虑增加灵活性时务必谨慎。
 */
ObjectAddress
AlterType(AlterTypeStmt *stmt)
{
	ObjectAddress address;
	Relation	catalog;
	TypeName   *typename;
	HeapTuple	tup;
	Oid			typeOid;
	Form_pg_type typForm;
	bool		requireSuper = false;
	AlterTypeRecurseParams atparams;
	ListCell   *pl;

	catalog = table_open(TypeRelationId, RowExclusiveLock);

	/* Make a TypeName so we can use standard type lookup machinery */
	/*
	 *
	 * 构造 TypeName，以便使用标准的类型查找机制。
	 */
	typename = makeTypeNameFromNameList(stmt->typeName);
	tup = typenameType(NULL, typename, NULL);

	typeOid = typeTypeId(tup);
	typForm = (Form_pg_type) GETSTRUCT(tup);

	/* Process options */
	/*
	 *
	 * 处理选项。
	 */
	memset(&atparams, 0, sizeof(atparams));
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "storage") == 0)
		{
			char	   *a = defGetString(defel);

			if (pg_strcasecmp(a, "plain") == 0)
				atparams.storage = TYPSTORAGE_PLAIN;
			else if (pg_strcasecmp(a, "external") == 0)
				atparams.storage = TYPSTORAGE_EXTERNAL;
			else if (pg_strcasecmp(a, "extended") == 0)
				atparams.storage = TYPSTORAGE_EXTENDED;
			else if (pg_strcasecmp(a, "main") == 0)
				atparams.storage = TYPSTORAGE_MAIN;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("storage \"%s\" not recognized", a)));

			/*
			 * Validate the storage request.  If the type isn't varlena, it
			 * certainly doesn't support non-PLAIN storage.
			 *
			 * 校验存储请求。若类型不是 varlena，则肯定不支持非 PLAIN 存储。
			 */
			if (atparams.storage != TYPSTORAGE_PLAIN && typForm->typlen != -1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("fixed-size types must have storage PLAIN")));

			/*
			 * Switching from PLAIN to non-PLAIN is allowed, but it requires
			 * superuser, since we can't validate that the type's C functions
			 * will support it.  Switching from non-PLAIN to PLAIN is
			 * disallowed outright, because it's not practical to ensure that
			 * no tables have toasted values of the type.  Switching among
			 * different non-PLAIN settings is OK, since it just constitutes a
			 * change in the strategy requested for columns created in the
			 * future.
			 *
			 * 从 PLAIN 切换到非 PLAIN 是允许的，但需要超级用户，因为我们无法验证该类型的 C 函数是否支持。
			 * 从非 PLAIN 切换到 PLAIN 则完全禁止，因为无法切实保证没有表存有该类型的 toasted 值。
			 * 在不同的非 PLAIN 设置之间切换是可以的，因为那只是改变今后新建列所请求的策略。
			 */
			if (atparams.storage != TYPSTORAGE_PLAIN &&
				typForm->typstorage == TYPSTORAGE_PLAIN)
				requireSuper = true;
			else if (atparams.storage == TYPSTORAGE_PLAIN &&
					 typForm->typstorage != TYPSTORAGE_PLAIN)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot change type's storage to PLAIN")));

			atparams.updateStorage = true;
		}
		else if (strcmp(defel->defname, "receive") == 0)
		{
			if (defel->arg != NULL)
				atparams.receiveOid =
					findTypeReceiveFunction(defGetQualifiedName(defel),
											typeOid);
			else
				atparams.receiveOid = InvalidOid;	/* NONE, remove function */
									/*
									 *
									 * NONE，表示移除该函数。
									 */
			atparams.updateReceive = true;
			/* Replacing an I/O function requires superuser. */
			/*
			 *
			 * 替换 I/O 函数需要超级用户。
			 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "send") == 0)
		{
			if (defel->arg != NULL)
				atparams.sendOid =
					findTypeSendFunction(defGetQualifiedName(defel),
										 typeOid);
			else
				atparams.sendOid = InvalidOid;	/* NONE, remove function */
								/*
								 *
								 * NONE，表示移除该函数。
								 */
			atparams.updateSend = true;
			/* Replacing an I/O function requires superuser. */
			/*
			 *
			 * 替换 I/O 函数需要超级用户。
			 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "typmod_in") == 0)
		{
			if (defel->arg != NULL)
				atparams.typmodinOid =
					findTypeTypmodinFunction(defGetQualifiedName(defel));
			else
				atparams.typmodinOid = InvalidOid;	/* NONE, remove function */
									/*
									 *
									 * NONE，表示移除该函数。
									 */
			atparams.updateTypmodin = true;
			/* Replacing an I/O function requires superuser. */
			/*
			 *
			 * 替换 I/O 函数需要超级用户。
			 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "typmod_out") == 0)
		{
			if (defel->arg != NULL)
				atparams.typmodoutOid =
					findTypeTypmodoutFunction(defGetQualifiedName(defel));
			else
				atparams.typmodoutOid = InvalidOid; /* NONE, remove function */
								    /*
								     *
								     * NONE，表示移除该函数。
								     */
			atparams.updateTypmodout = true;
			/* Replacing an I/O function requires superuser. */
			/*
			 *
			 * 替换 I/O 函数需要超级用户。
			 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "analyze") == 0)
		{
			if (defel->arg != NULL)
				atparams.analyzeOid =
					findTypeAnalyzeFunction(defGetQualifiedName(defel),
											typeOid);
			else
				atparams.analyzeOid = InvalidOid;	/* NONE, remove function */
									/*
									 *
									 * NONE，表示移除该函数。
									 */
			atparams.updateAnalyze = true;
			/* Replacing an analyze function requires superuser. */
			/*
			 *
			 * 替换 analyze 函数需要超级用户。
			 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "subscript") == 0)
		{
			if (defel->arg != NULL)
				atparams.subscriptOid =
					findTypeSubscriptingFunction(defGetQualifiedName(defel),
												 typeOid);
			else
				atparams.subscriptOid = InvalidOid; /* NONE, remove function */
								    /*
								     *
								     * NONE，表示移除该函数。
								     */
			atparams.updateSubscript = true;
			/* Replacing a subscript function requires superuser. */
			/*
			 *
			 * 替换 subscript 函数需要超级用户。
			 */
			requireSuper = true;
		}

		/*
		 * The rest of the options that CREATE accepts cannot be changed.
		 * Check for them so that we can give a meaningful error message.
		 *
		 * CREATE 所接受的其余选项不能修改。这里检查它们，以便给出有意义的错误信息。
		 */
		else if (strcmp(defel->defname, "input") == 0 ||
				 strcmp(defel->defname, "output") == 0 ||
				 strcmp(defel->defname, "internallength") == 0 ||
				 strcmp(defel->defname, "passedbyvalue") == 0 ||
				 strcmp(defel->defname, "alignment") == 0 ||
				 strcmp(defel->defname, "like") == 0 ||
				 strcmp(defel->defname, "category") == 0 ||
				 strcmp(defel->defname, "preferred") == 0 ||
				 strcmp(defel->defname, "default") == 0 ||
				 strcmp(defel->defname, "element") == 0 ||
				 strcmp(defel->defname, "delimiter") == 0 ||
				 strcmp(defel->defname, "collatable") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" cannot be changed",
							defel->defname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * Permissions check.  Require superuser if we decided the command
	 * requires that, else must own the type.
	 *
	 * 权限检查。若判定该命令需要超级用户则要求超级用户，否则必须拥有该类型。
	 */
	if (requireSuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter a type")));
	}
	else
	{
		if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
			aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);
	}

	/*
	 * We disallow all forms of ALTER TYPE SET on types that aren't plain base
	 * types.  It would for example be highly unsafe, not to mention
	 * pointless, to change the send/receive functions for a composite type.
	 * Moreover, pg_dump has no support for changing these properties on
	 * non-base types.  We might weaken this someday, but not now.
	 *
	 * 我们不允许对非普通基类型使用任何形式的 ALTER TYPE SET。
	 * 例如修改复合类型的 send/receive 函数既非常不安全，也没有意义。
	 * 而且 pg_dump 不支持修改非基类型的这些属性。也许将来会放宽，但现在不行。
	 *
	 * Note: if you weaken this enough to allow composite types, be sure to
	 * adjust the GenerateTypeDependencies call in AlterTypeRecurse.
	 *
	 * 注意：若把限制放宽到允许复合类型，务必调整 AlterTypeRecurse 中对 GenerateTypeDependencies 的调用。
	 */
	if (typForm->typtype != TYPTYPE_BASE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a base type",
						format_type_be(typeOid))));

	/*
	 * For the same reasons, don't allow direct alteration of array types.
	 *
	 * 出于同样的原因，不允许直接修改数组类型。
	 */
	if (IsTrueArrayType(typForm))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a base type",
						format_type_be(typeOid))));

	/* OK, recursively update this type and any arrays/domains over it */
	/*
	 *
	 * 可以递归更新该类型以及基于它的数组与 domain 了。
	 */
	AlterTypeRecurse(typeOid, false, tup, catalog, &atparams);

	/* Clean up */
	/*
	 *
	 * 清理。
	 */
	ReleaseSysCache(tup);

	table_close(catalog, RowExclusiveLock);

	ObjectAddressSet(address, TypeRelationId, typeOid);

	return address;
}

/*
 * AlterTypeRecurse: one recursion step for AlterType()
 *
 * AlterTypeRecurse：AlterType() 的一层递归。
 *
 * Apply the changes specified by "atparams" to the type identified by
 * "typeOid", whose existing pg_type tuple is "tup".  If necessary,
 * recursively update its array type as well.  Then search for any domains
 * over this type, and recursively apply (most of) the same changes to those
 * domains.
 *
 * 把 atparams 指定的变更应用到 typeOid 所标识的类型，其现有 pg_type 元组为 tup。
 * 必要时递归更新其数组类型。然后查找基于该类型的 domain，并递归套用（大部分）相同变更。
 *
 * We need this because the system generally assumes that a domain inherits
 * many properties from its base type.  See DefineDomain() above for details
 * of what is inherited.  Arrays inherit a smaller number of properties,
 * but not none.
 *
 * 需要这样做，是因为系统通常假定 domain 从基类型继承许多属性。继承了哪些见上面的 DefineDomain()。
 * 数组继承的属性更少，但并非完全不继承。
 *
 * There's a race condition here, in that some other transaction could
 * concurrently add another domain atop this base type; we'd miss updating
 * that one.  Hence, be wary of allowing ALTER TYPE to change properties for
 * which it'd be really fatal for a domain to be out of sync with its base
 * type (typlen, for example).  In practice, races seem unlikely to be an
 * issue for plausible use-cases for ALTER TYPE.  If one does happen, it could
 * be fixed by re-doing the same ALTER TYPE once all prior transactions have
 * committed.
 *
 * 这里存在竞态：其他事务可能同时在该基类型上再加一个 domain，我们会漏掉对它的更新。
 * 因此，对那些 domain 与基类型不同步就会真正致命的属性（例如 typlen），要谨慎允许 ALTER TYPE 去改。
 * 实践中，对合理的 ALTER TYPE 用法，竞态不太可能成为问题。若真的发生，可在先前事务都提交后再执行一次同样的 ALTER TYPE 来修复。
 */
static void
AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
				 HeapTuple tup, Relation catalog,
				 AlterTypeRecurseParams *atparams)
{
	Datum		values[Natts_pg_type];
	bool		nulls[Natts_pg_type];
	bool		replaces[Natts_pg_type];
	HeapTuple	newtup;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	domainTup;

	/* Since this function recurses, it could be driven to stack overflow */
	/*
	 *
	 * 由于此函数会递归，可能被推到栈溢出。
	 */
	check_stack_depth();

	/* Update the current type's tuple */
	/*
	 *
	 * 更新当前类型的元组。
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(replaces, 0, sizeof(replaces));

	if (atparams->updateStorage)
	{
		replaces[Anum_pg_type_typstorage - 1] = true;
		values[Anum_pg_type_typstorage - 1] = CharGetDatum(atparams->storage);
	}
	if (atparams->updateReceive)
	{
		replaces[Anum_pg_type_typreceive - 1] = true;
		values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(atparams->receiveOid);
	}
	if (atparams->updateSend)
	{
		replaces[Anum_pg_type_typsend - 1] = true;
		values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(atparams->sendOid);
	}
	if (atparams->updateTypmodin)
	{
		replaces[Anum_pg_type_typmodin - 1] = true;
		values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(atparams->typmodinOid);
	}
	if (atparams->updateTypmodout)
	{
		replaces[Anum_pg_type_typmodout - 1] = true;
		values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(atparams->typmodoutOid);
	}
	if (atparams->updateAnalyze)
	{
		replaces[Anum_pg_type_typanalyze - 1] = true;
		values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(atparams->analyzeOid);
	}
	if (atparams->updateSubscript)
	{
		replaces[Anum_pg_type_typsubscript - 1] = true;
		values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(atparams->subscriptOid);
	}

	newtup = heap_modify_tuple(tup, RelationGetDescr(catalog),
							   values, nulls, replaces);

	CatalogTupleUpdate(catalog, &newtup->t_self, newtup);

	/* Rebuild dependencies for this type */
	/*
	 *
	 * 重建该类型的依赖。
	 */
	GenerateTypeDependencies(newtup,
							 catalog,
							 NULL,	/* don't have defaultExpr handy */
								/*
								 *
								 * 手头没有 defaultExpr。
								 */
							 NULL,	/* don't have typacl handy */
								/*
								 *
								 * 手头没有 typacl。
								 */
							 0, /* we rejected composite types above */
							    /*
							     *
							     * 上面已拒绝复合类型。
							     */
							 isImplicitArray,	/* it might be an array */
										/*
										 *
										 * 它可能是数组。
										 */
							 isImplicitArray,	/* dependent iff it's array */
										/*
										 *
										 * 当且仅当它是数组时才是依赖类型。
										 */
							 false, /* don't touch extension membership */
								/*
								 *
								 * 不改动扩展成员关系。
								 */
							 true);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	/*
	 * Arrays inherit their base type's typmodin and typmodout, but none of
	 * the other properties we're concerned with here.  Recurse to the array
	 * type if needed.
	 *
	 * 数组类型继承基类型的 typmodin 与 typmodout，但不继承此处关心的其他属性。
	 * 如有需要，递归到数组类型。
	 */
	if (!isImplicitArray &&
		(atparams->updateTypmodin || atparams->updateTypmodout))
	{
		Oid			arrtypoid = ((Form_pg_type) GETSTRUCT(newtup))->typarray;

		if (OidIsValid(arrtypoid))
		{
			HeapTuple	arrtup;
			AlterTypeRecurseParams arrparams;

			arrtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(arrtypoid));
			if (!HeapTupleIsValid(arrtup))
				elog(ERROR, "cache lookup failed for type %u", arrtypoid);

			memset(&arrparams, 0, sizeof(arrparams));
			arrparams.updateTypmodin = atparams->updateTypmodin;
			arrparams.updateTypmodout = atparams->updateTypmodout;
			arrparams.typmodinOid = atparams->typmodinOid;
			arrparams.typmodoutOid = atparams->typmodoutOid;

			AlterTypeRecurse(arrtypoid, true, arrtup, catalog, &arrparams);

			ReleaseSysCache(arrtup);
		}
	}

	/*
	 * Now we need to recurse to domains.  However, some properties are not
	 * inherited by domains, so clear the update flags for those.
	 *
	 * 现在需要递归到 domain。不过有些属性不会被 domain 继承，因此清除那些属性的更新标志。
	 */
	atparams->updateReceive = false;	/* domains use F_DOMAIN_RECV */
						/*
						 *
						 * domain 使用 F_DOMAIN_RECV。
						 */
	atparams->updateTypmodin = false;	/* domains don't have typmods */
						/*
						 *
						 * domain 没有 typmod。
						 */
	atparams->updateTypmodout = false;
	atparams->updateSubscript = false;	/* domains don't have subscriptors */
						/*
						 *
						 * domain 没有 subscript 函数。
						 */

	/* Skip the scan if nothing remains to be done */
	/*
	 *
	 * 若已无事可做，则跳过扫描。
	 */
	if (!(atparams->updateStorage ||
		  atparams->updateSend ||
		  atparams->updateAnalyze))
		return;

	/* Search pg_type for possible domains over this type */
	/*
	 *
	 * 在 pg_type 中搜索可能基于该类型的 domain。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_type_typbasetype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = systable_beginscan(catalog, InvalidOid, false,
							  NULL, 1, key);

	while ((domainTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_type domainForm = (Form_pg_type) GETSTRUCT(domainTup);

		/*
		 * Shouldn't have a nonzero typbasetype in a non-domain, but let's
		 * check
		 *
		 * 非 domain 不应有非零的 typbasetype，但仍检查一下。
		 */
		if (domainForm->typtype != TYPTYPE_DOMAIN)
			continue;

		AlterTypeRecurse(domainForm->oid, false, domainTup, catalog, atparams);
	}

	systable_endscan(scan);
}
