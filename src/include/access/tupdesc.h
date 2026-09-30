/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * tupdesc.h POSTGRES 元组描述符定义。 src/in
 * clude/access/tupdesc.h
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"


typedef struct AttrDefault
{
	AttrNumber	adnum;
	char	   *adbin;			/* nodeToString representation of expr */

	/* 中文翻译：expr 的 nodeToString 表示 */
} AttrDefault;

typedef struct ConstrCheck
{
	char	   *ccname;
	char	   *ccbin;			/* nodeToString representation of expr */

	/* 中文翻译：expr 的 nodeToString 表示 */
	bool		ccenforced;
	bool		ccvalid;
	bool		ccnoinherit;	/* this is a non-inheritable constraint */

	/* 中文翻译：这是一个不可继承的约束 */
} ConstrCheck;

/* This structure contains constraints of a tuple */

/* 中文翻译：该结构包含元组的约束 */
typedef struct TupleConstr
{
	AttrDefault *defval;		/* array */

	/* 中文翻译：大批 */
	ConstrCheck *check;			/* array */

	/* 中文翻译：大批 */
	struct AttrMissing *missing;	/* missing attributes values, NULL if none */

	/* 中文翻译：缺少属性值，如果没有则为 NULL */
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;	/* any not-null, including not valid ones */

	/* 中文翻译：任何非空的，包括无效的 */
	bool		has_generated_stored;
	bool		has_generated_virtual;
} TupleConstr;

/*
 * CompactAttribute
 *		Cut-down version of FormData_pg_attribute for faster access for tasks
 *		such as tuple deformation.  The fields of this struct are populated
 *		using the populate_compact_attribute() function, which must be called
 *		directly after the FormData_pg_attribute struct is populated or
 *		altered in any way.
 *
 * Currently, this struct is 16 bytes.  Any code changes which enlarge this
 * struct should be considered very carefully.
 *
 * Code which must access a TupleDesc's attribute data should always make use
 * the fields of this struct when required fields are available here.  It's
 * more efficient to access the memory in CompactAttribute due to it being a
 * more compact representation of FormData_pg_attribute and also because
 * accessing the FormData_pg_attribute requires an additional calculations to
 * obtain the base address of the array within the TupleDesc.
 *
 * 中文翻译：
 * CompactAttribute FormData_pg_attri
 * bute 的精简版本，用于更快地访问元组变形等任务。该结构的字段使用
 *  populate_compact_attribute() 函数填充
 * ，该函数必须在以任何方式填充或更改 FormData_pg_attr
 * ibute 结构后直接调用。目前，该结构体为 16 字节。任何扩大此
 * 结构的代码更改都应该非常仔细地考虑。当此处提供所需字段时，必须访问
 * TupleDesc 属性数据的代码应始终使用此结构的字段。访问 Co
 * mpactAttribute 中的内存效率更高，因为它是 FormD
 * ata_pg_attribute 的更紧凑表示形式，而且访问 For
 * mData_pg_attribute 需要进行额外的计算才能获取 T
 * upleDesc 中数组的基地址。
 */
typedef struct CompactAttribute
{
	int32		attcacheoff;	/* fixed offset into tuple, if known, or -1 */

	/* 中文翻译：元组中的固定偏移量（如果已知）或 -1 */
	int16		attlen;			/* attr len in bytes or -1 = varlen, -2 =
								 * cstring */

	/* 中文翻译：
	 * attr len（以字节为单位）或 -1 = varlen，-2 =
	 * cstring
	 */
	bool		attbyval;		/* as FormData_pg_attribute.attbyval */

	/* 中文翻译：作为 FormData_pg_attribute.attbyval */
	bool		attispackable;	/* FormData_pg_attribute.attstorage !=
								 * TYPSTORAGE_PLAIN */

	/* 中文翻译：
	 * FormData_pg_attribute.attstorage !
	 * = TYPSTORAGE_PLAIN
	 */
	bool		atthasmissing;	/* as FormData_pg_attribute.atthasmissing */

	/* 中文翻译：作为 FormData_pg_attribute.atthasmissing */
	bool		attisdropped;	/* as FormData_pg_attribute.attisdropped */

	/* 中文翻译：作为 FormData_pg_attribute.attisdropped */
	bool		attgenerated;	/* FormData_pg_attribute.attgenerated != '\0' */

	/* 中文翻译：FormData_pg_attribute.att generated != '\0' */
	char		attnullability; /* status of not-null constraint, see below */

	/* 中文翻译：非空约束的状态，见下文 */
	uint8		attalignby;		/* alignment requirement in bytes */

	/* 中文翻译：对齐要求（以字节为单位） */
} CompactAttribute;

/* Valid values for CompactAttribute->attnullability */

/* 中文翻译：CompactAttribute->attnullability 的有效值 */
#define	ATTNULLABLE_UNRESTRICTED 'f'	/* No constraint exists */

/* 中文翻译：不存在任何约束 */
#define	ATTNULLABLE_UNKNOWN		'u' /* constraint exists, validity unknown */

/* 中文翻译：存在约束，有效性未知 */
#define	ATTNULLABLE_VALID		'v' /* valid constraint exists */

/* 中文翻译：存在有效约束 */
#define	ATTNULLABLE_INVALID		'i' /* constraint exists, marked invalid */

/* 中文翻译：约束存在，标记为无效 */

/*
 * This struct is passed around within the backend to describe the structure
 * of tuples.  For tuples coming from on-disk relations, the information is
 * collected from the pg_attribute, pg_attrdef, and pg_constraint catalogs.
 * Transient row types (such as the result of a join query) have anonymous
 * TupleDesc structs that generally omit any constraint info; therefore the
 * structure is designed to let the constraints be omitted efficiently.
 *
 * Note that only user attributes, not system attributes, are mentioned in
 * TupleDesc.
 *
 * If the tupdesc is known to correspond to a named rowtype (such as a table's
 * rowtype) then tdtypeid identifies that type and tdtypmod is -1.  Otherwise
 * tdtypeid is RECORDOID, and tdtypmod can be either -1 for a fully anonymous
 * row type, or a value >= 0 to allow the rowtype to be looked up in the
 * typcache.c type cache.
 *
 * Note that tdtypeid is never the OID of a domain over composite, even if
 * we are dealing with values that are known (at some higher level) to be of
 * a domain-over-composite type.  This is because tdtypeid/tdtypmod need to
 * match up with the type labeling of composite Datums, and those are never
 * explicitly marked as being of a domain type, either.
 *
 * Tuple descriptors that live in caches (relcache or typcache, at present)
 * are reference-counted: they can be deleted when their reference count goes
 * to zero.  Tuple descriptors created by the executor need no reference
 * counting, however: they are simply created in the appropriate memory
 * context and go away when the context is freed.  We set the tdrefcount
 * field of such a descriptor to -1, while reference-counted descriptors
 * always have tdrefcount >= 0.
 *
 * Beyond the compact_attrs variable length array, the TupleDesc stores an
 * array of FormData_pg_attribute.  The TupleDescAttr() function, as defined
 * below, takes care of calculating the address of the elements of the
 * FormData_pg_attribute array.
 *
 * The array of CompactAttribute is effectively an abbreviated version of the
 * array of FormData_pg_attribute.  Because CompactAttribute is significantly
 * smaller than FormData_pg_attribute, code, especially performance-critical
 * code, should prioritize using the fields from the CompactAttribute over the
 * equivalent fields in FormData_pg_attribute.
 *
 * Any code making changes manually to and fields in the FormData_pg_attribute
 * array must subsequently call populate_compact_attribute() to flush the
 * changes out to the corresponding 'compact_attrs' element.
 *
 * 中文翻译：
 * 该结构在后端内传递以描述元组的结构。对于来自磁盘上关系的元组，信息是
 * 从 pg_attribute、pg_attrdef 和 pg_con
 * straint 目录中收集的。瞬态行类型（例如连接查询的结果）具有匿
 * 名 TupleDesc 结构，通常会省略任何约束信息；因此，该结构的
 * 设计是为了有效地忽略约束。请注意，TupleDesc 中仅提到了用户
 * 属性，而不是系统属性。如果已知 tupdesc 对应于命名行类型（例
 * 如表的行类型），则 tdtypeid 标识该类型，并且 tdtypm
 * od 为 -1。否则，tdtypeid 为 RECORDOID，并且
 *  tdtypmod 可以为 -1（表示完全匿名行类型），也可以为 >
 * = 0 的值以允许在typcache.c 类型缓存中查找行类型。请注
 * 意，tdtypeid 永远不是复合域的 OID，即使我们正在处理已知
 * （在更高级别）属于复合域类型的值。这是因为 tdtypeid/tdt
 * ypmod 需要与复合基准的类型标签相匹配，并且这些基准也从未明确标
 * 记为域类型。存在于缓存（目前是 relcache 或typcache
 * ）中的元组描述符是引用计数的：当它们的引用计数变为零时，它们可以被删
 * 除。然而，执行器创建的元组描述符不需要引用计数：它们只是在适当的内存
 * 上下文中创建，并在释放上下文时消失。我们将此类描述符的 tdrefc
 * ount 字段设置为 -1，而引用计数描述符始终具有 tdrefco
 * unt >= 0。 除了compact_attrs 可变长度数组之外
 * ，TupleDesc 还存储 FormData_pg_attribu
 * te 数组。 TupleDescAttr() 函数（如下定义）负责计
 * 算 FormData_pg_attribute 数组元素的地址。 C
 * ompactAttribute 数组实际上是 FormData_pg
 * _attribute 数组的缩写版本。由于 CompactAttri
 * bute 明显小于 FormData_pg_attribute，因此
 * 代码（尤其是性能关键型代码）应优先使用 CompactAttribu
 * te 中的字段，而不是 FormData_pg_attribute
 * 中的等效字段。任何手动更改 FormData_pg_attribut
 * e 数组中的字段的代码都必须随后调用 populate_compac
 * t_attribute() 将更改刷新到相应的“compact_at
 * trs”元素。
 */
typedef struct TupleDescData
{
	int			natts;			/* number of attributes in the tuple */

	/* 中文翻译：元组中的属性数量 */
	Oid			tdtypeid;		/* composite type ID for tuple type */

	/* 中文翻译：元组类型的复合类型 ID */
	int32		tdtypmod;		/* typmod for tuple type */

	/* 中文翻译：元组类型的typmod */
	int			tdrefcount;		/* reference count, or -1 if not counting */

	/* 中文翻译：引用计数，如果不计数则为 -1 */
	TupleConstr *constr;		/* constraints, or NULL if none */

	/* 中文翻译：约束，如果没有则为 NULL */
	/* compact_attrs[N] is the compact metadata of Attribute Number N+1 */

	/* 中文翻译：Compact_attrs[N] 是属性号 N+1 的紧凑元数据 */
	CompactAttribute compact_attrs[FLEXIBLE_ARRAY_MEMBER];
}			TupleDescData;
typedef struct TupleDescData *TupleDesc;

/*
 * Function populate_compact_attribute carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 populate_compact_attribute通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void populate_compact_attribute(TupleDesc tupdesc, int attnum);

/*
 * Calculates the base address of the Form_pg_attribute at the end of the
 * TupleDescData struct.
 *
 * 中文翻译：
 * 计算 TupleDescData 结构末尾的 Form_pg_att
 * ribute 的基地址。
 */
#define TupleDescAttrAddress(desc) \
	(Form_pg_attribute) ((char *) (desc) + \
	 (offsetof(struct TupleDescData, compact_attrs) + \
	 (desc)->natts * sizeof(CompactAttribute)))

/* Accessor for the i'th FormData_pg_attribute element of tupdesc. */

/* 中文翻译：tupdesc 的第 i 个 FormData_pg_attribute 元素的访问器。 */
/*
 * Function TupleDescAttr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TupleDescAttr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline FormData_pg_attribute *
TupleDescAttr(TupleDesc tupdesc, int i)
{
	FormData_pg_attribute *attrs = TupleDescAttrAddress(tupdesc);

	return &attrs[i];
}

#undef TupleDescAttrAddress

/*
 * Function verify_compact_attribute carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 verify_compact_attribute通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void verify_compact_attribute(TupleDesc, int attnum);

/*
 * Accessor for the i'th CompactAttribute element of tupdesc.
 *
 * 中文翻译：
 * tupdesc 的第 i 个 CompactAttribute 元素
 * 的访问器。
 */
/*
 * Function TupleDescCompactAttr retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TupleDescCompactAttr通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline CompactAttribute *
TupleDescCompactAttr(TupleDesc tupdesc, int i)
{
	CompactAttribute *cattr = &tupdesc->compact_attrs[i];

#ifdef USE_ASSERT_CHECKING

	/* Check that the CompactAttribute is correctly populated */

	/* 中文翻译：检查 CompactAttribute 是否已正确填充 */
	verify_compact_attribute(tupdesc, i);
#endif

	return cattr;
}

/*
 * Function CreateTemplateTupleDesc initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 CreateTemplateTupleDesc通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TupleDesc CreateTemplateTupleDesc(int natts);

/*
 * Function CreateTupleDesc initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 CreateTupleDesc通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TupleDesc CreateTupleDesc(int natts, Form_pg_attribute *attrs);

/*
 * Function CreateTupleDescCopy initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 CreateTupleDescCopy通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

/*
 * Function CreateTupleDescTruncatedCopy initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 CreateTupleDescTruncatedCopy通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TupleDesc CreateTupleDescTruncatedCopy(TupleDesc tupdesc, int natts);

/*
 * Function CreateTupleDescCopyConstr initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 CreateTupleDescCopyConstr通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

#define TupleDescSize(src) \
	(offsetof(struct TupleDescData, compact_attrs) + \
	 (src)->natts * sizeof(CompactAttribute) + \
	 (src)->natts * sizeof(FormData_pg_attribute))

/*
 * Function TupleDescCopy retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TupleDescCopy通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void TupleDescCopy(TupleDesc dst, TupleDesc src);

/*
 * Function TupleDescCopyEntry retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TupleDescCopyEntry通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
							   TupleDesc src, AttrNumber srcAttno);

/*
 * Function FreeTupleDesc completes the requested access-layer operation by releasing or finalizing the supplied state after the required cleanup steps.
 *
 * 函数 FreeTupleDesc在完成必要的清理步骤后，通过释放或终结传入状态来结束请求的访问层操作。
 */
extern void FreeTupleDesc(TupleDesc tupdesc);

/*
 * Function IncrTupleDescRefCount retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 IncrTupleDescRefCount通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void IncrTupleDescRefCount(TupleDesc tupdesc);
/*
 * Function DecrTupleDescRefCount retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 DecrTupleDescRefCount通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern void DecrTupleDescRefCount(TupleDesc tupdesc);

#define PinTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			IncrTupleDescRefCount(tupdesc); \
	} while (0)

#define ReleaseTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			DecrTupleDescRefCount(tupdesc); \
	} while (0)

/*
 * Function equalTupleDescs retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 equalTupleDescs通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
/*
 * Function equalRowTypes evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 equalRowTypes通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern bool equalRowTypes(TupleDesc tupdesc1, TupleDesc tupdesc2);
/*
 * Function hashRowType evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 hashRowType通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
extern uint32 hashRowType(TupleDesc desc);

/*
 * Function TupleDescInitEntry initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 TupleDescInitEntry通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void TupleDescInitEntry(TupleDesc desc,
							   AttrNumber attributeNumber,
							   const char *attributeName,
							   Oid oidtypeid,
							   int32 typmod,
							   int attdim);

/*
 * Function TupleDescInitBuiltinEntry initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 TupleDescInitBuiltinEntry通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void TupleDescInitBuiltinEntry(TupleDesc desc,
									  AttrNumber attributeNumber,
									  const char *attributeName,
									  Oid oidtypeid,
									  int32 typmod,
									  int attdim);

/*
 * Function TupleDescInitEntryCollation initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 TupleDescInitEntryCollation通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void TupleDescInitEntryCollation(TupleDesc desc,
										AttrNumber attributeNumber,
										Oid collationid);

/*
 * Function BuildDescFromLists retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 BuildDescFromLists通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern TupleDesc BuildDescFromLists(const List *names, const List *types, const List *typmods, const List *collations);

/*
 * Function TupleDescGetDefault retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 TupleDescGetDefault通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern Node *TupleDescGetDefault(TupleDesc tupdesc, AttrNumber attnum);

#endif							/* TUPDESC_H */

/* 中文翻译：TUPDESC_H */
