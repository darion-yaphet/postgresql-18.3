/*-------------------------------------------------------------------------
 *
 * reloptions.h
 *	  Core support for relation and tablespace options (pg_class.reloptions
 *	  and pg_tablespace.spcoptions)
 *
 * Note: the functions dealing with text-array reloptions values declare
 * them as Datum, not ArrayType *, to avoid needing to include array.h
 * into a lot of low-level code.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/reloptions.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * reloptions.h 对关系和表空间选项的核心支持（pg_cla
 * ss.reloptions 和 pg_tablespace.spco
 * ptions） 注意：处理文本数组 reloptions 值的函数将
 * 它们声明为 Datum，而不是 ArrayType *，以避免需要将
 *  array.h 包含到大量低级代码中。 src/include/a
 * ccess/reloptions.h
 */
#ifndef RELOPTIONS_H
#define RELOPTIONS_H

#include "access/amapi.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "nodes/pg_list.h"
#include "storage/lock.h"

/* types supported by reloptions */

/* 中文翻译：reloptions 支持的类型 */
typedef enum relopt_type
{
	RELOPT_TYPE_BOOL,
	RELOPT_TYPE_INT,
	RELOPT_TYPE_REAL,
	RELOPT_TYPE_ENUM,
	RELOPT_TYPE_STRING,
} relopt_type;

/* kinds supported by reloptions */

/* 中文翻译：reloptions 支持的种类 */
typedef enum relopt_kind
{
	RELOPT_KIND_LOCAL = 0,
	RELOPT_KIND_HEAP = (1 << 0),
	RELOPT_KIND_TOAST = (1 << 1),
	RELOPT_KIND_BTREE = (1 << 2),
	RELOPT_KIND_HASH = (1 << 3),
	RELOPT_KIND_GIN = (1 << 4),
	RELOPT_KIND_GIST = (1 << 5),
	RELOPT_KIND_ATTRIBUTE = (1 << 6),
	RELOPT_KIND_TABLESPACE = (1 << 7),
	RELOPT_KIND_SPGIST = (1 << 8),
	RELOPT_KIND_VIEW = (1 << 9),
	RELOPT_KIND_BRIN = (1 << 10),
	RELOPT_KIND_PARTITIONED = (1 << 11),
	/* if you add a new kind, make sure you update "last_default" too */

	/* 中文翻译：如果您添加新种类，请确保也更新“last_default” */
	RELOPT_KIND_LAST_DEFAULT = RELOPT_KIND_PARTITIONED,
	/* some compilers treat enums as signed ints, so we can't use 1 << 31 */

	/* 中文翻译：一些编译器将枚举视为有符号整数，因此我们不能使用 1 << 31 */
	RELOPT_KIND_MAX = (1 << 30)
} relopt_kind;

/* reloption namespaces allowed for heaps -- currently only TOAST */

/* 中文翻译：堆允许使用 reloption 命名空间——目前仅 TOAST */
#define HEAP_RELOPT_NAMESPACES { "toast", NULL }

/* generic struct to hold shared data */

/* 中文翻译：保存共享数据的通用结构 */
typedef struct relopt_gen
{
	const char *name;			/* must be first (used as list termination
								 * marker) */

	/* 中文翻译：
	 * 必须是第一个（用作列表终止标记）
	 */
	const char *desc;
	bits32		kinds;
	LOCKMODE	lockmode;
	int			namelen;
	relopt_type type;
} relopt_gen;

/* holds a parsed value */

/* 中文翻译：保存一个解析值 */
typedef struct relopt_value
{
	relopt_gen *gen;
	bool		isset;
	union
	{
		bool		bool_val;
		int			int_val;
		double		real_val;
		int			enum_val;
		char	   *string_val; /* allocated separately */

		/* 中文翻译：单独分配 */
	}			values;
} relopt_value;

/* reloptions records for specific variable types */

/* 中文翻译：特定变量类型的reloptions记录 */
typedef struct relopt_bool
{
	relopt_gen	gen;
	bool		default_val;
} relopt_bool;

typedef struct relopt_int
{
	relopt_gen	gen;
	int			default_val;
	int			min;
	int			max;
} relopt_int;

typedef struct relopt_real
{
	relopt_gen	gen;
	double		default_val;
	double		min;
	double		max;
} relopt_real;

/*
 * relopt_enum_elt_def -- One member of the array of acceptable values
 * of an enum reloption.
 *
 * 中文翻译：
 * relopt_enum_elt_def——枚举 reloption
 * 的可接受值数组之一。
 */
typedef struct relopt_enum_elt_def
{
	const char *string_val;
	int			symbol_val;
} relopt_enum_elt_def;

typedef struct relopt_enum
{
	relopt_gen	gen;
	relopt_enum_elt_def *members;
	int			default_val;
	const char *detailmsg;
	/* null-terminated array of members */

	/* 中文翻译：空终止成员数组 */
} relopt_enum;

/* validation routines for strings */

/* 中文翻译：字符串的验证例程 */
typedef void (*validate_string_relopt) (const char *value);
typedef Size (*fill_string_relopt) (const char *value, void *ptr);

/* validation routine for the whole option set */

/* 中文翻译：整个选项集的验证例程 */
typedef void (*relopts_validator) (void *parsed_options, relopt_value *vals, int nvals);

typedef struct relopt_string
{
	relopt_gen	gen;
	int			default_len;
	bool		default_isnull;
	validate_string_relopt validate_cb;
	fill_string_relopt fill_cb;
	char	   *default_val;
} relopt_string;

/* This is the table datatype for build_reloptions() */

/* 中文翻译：这是 build_reloptions() 的表数据类型 */
typedef struct
{
	const char *optname;		/* option's name */

	/* 中文翻译：选项名称 */
	relopt_type opttype;		/* option's datatype */

	/* 中文翻译：选项的数据类型 */
	int			offset;			/* offset of field in result struct */

	/* 中文翻译：结果结构中字段的偏移量 */

	/*
	 * isset_offset is an optional offset of a field in the result struct that
	 * stores whether the option is explicitly set for the relation or if it
	 * just picked up the default value.  In most cases, this can be
	 * accomplished by giving the reloption a special out-of-range default
	 * value (e.g., some integer reloptions use -2), but this isn't always
	 * possible.  For example, a Boolean reloption cannot be given an
	 * out-of-range default, so we need another way to discover the source of
	 * its value.  This offset is only used if given a value greater than
	 * zero.
	 *
	 * 中文翻译：
	 * isset_offset 是结果结构中字段的可选偏移量，用于存储是为
	 * 关系显式设置该选项还是仅获取默认值。在大多数情况下，这可以通过给re
	 * loption一个特殊的超出范围的默认值来完成（例如，一些整数rel
	 * options使用-2），但这并不总是可行的。例如，布尔型relop
	 * tion不能被赋予超出范围的默认值，因此我们需要另一种方法来发现其值
	 * 的来源。仅当给定的值大于零时才使用此偏移量。
	 */
	int			isset_offset;
} relopt_parse_elt;

/* Local reloption definition */

/* 中文翻译：局部重新选择定义 */
typedef struct local_relopt
{
	relopt_gen *option;			/* option definition */

	/* 中文翻译：选项定义 */
	int			offset;			/* offset of parsed value in bytea structure */

	/* 中文翻译：解析值在bytea结构中的偏移量 */
} local_relopt;

/* Structure to hold local reloption data for build_local_reloptions() */

/* 中文翻译：用于保存 build_local_reloptions() 的本地 reloption 数据的结构 */
typedef struct local_relopts
{
	List	   *options;		/* list of local_relopt definitions */

	/* 中文翻译：local_relopt 定义列表 */
	List	   *validators;		/* list of relopts_validator callbacks */

	/* 中文翻译：relopts_validator 回调列表 */
	Size		relopt_struct_size; /* size of parsed bytea structure */

	/* 中文翻译：解析的bytea结构的大小 */
} local_relopts;

/*
 * Utility macro to get a value for a string reloption once the options
 * are parsed.  This gets a pointer to the string value itself.  "optstruct"
 * is the StdRdOptions struct or equivalent, "member" is the struct member
 * corresponding to the string option.
 *
 * 中文翻译：
 * 实用程序宏，用于在解析选项后获取字符串重新选项的值。这将获取指向字符
 * 串值本身的指针。 “optstruct”是 StdRdOptions
 *  结构或等效结构，“member”是与字符串选项对应的结构成员。
 */
#define GET_STRING_RELOPTION(optstruct, member) \
	((optstruct)->member == 0 ? NULL : \
	 (char *)(optstruct) + (optstruct)->member)

/*
 * Function add_reloption_kind carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_reloption_kind通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern relopt_kind add_reloption_kind(void);
/*
 * Function add_bool_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_bool_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_bool_reloption(bits32 kinds, const char *name, const char *desc,
							   bool default_val, LOCKMODE lockmode);
/*
 * Function add_int_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_int_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_int_reloption(bits32 kinds, const char *name, const char *desc,
							  int default_val, int min_val, int max_val,
							  LOCKMODE lockmode);
/*
 * Function add_real_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_real_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_real_reloption(bits32 kinds, const char *name, const char *desc,
							   double default_val, double min_val, double max_val,
							   LOCKMODE lockmode);
/*
 * Function add_enum_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_enum_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_enum_reloption(bits32 kinds, const char *name, const char *desc,
							   relopt_enum_elt_def *members, int default_val,
							   const char *detailmsg, LOCKMODE lockmode);
/*
 * Function add_string_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_string_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_string_reloption(bits32 kinds, const char *name, const char *desc,
								 const char *default_val, validate_string_relopt validator,
								 LOCKMODE lockmode);

/*
 * Function init_local_reloptions initializes the requested access-layer operation by preparing input state, validating required context, and returning the initialized handle or result.
 *
 * 函数 init_local_reloptions通过准备输入状态、校验所需上下文并返回已初始化的句柄或结果，初始化请求的访问层操作。
 */
extern void init_local_reloptions(local_relopts *relopts, Size relopt_struct_size);
/*
 * Function register_reloptions_validator updates access-layer state by applying the requested flags, values, or synchronization checks to the supplied object.
 *
 * 函数 register_reloptions_validator通过将请求的标志、值或同步检查应用于传入对象来更新访问层状态。
 */
extern void register_reloptions_validator(local_relopts *relopts,
										  relopts_validator validator);
/*
 * Function add_local_bool_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_local_bool_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_local_bool_reloption(local_relopts *relopts, const char *name,
									 const char *desc, bool default_val,
									 int offset);
/*
 * Function add_local_int_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_local_int_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_local_int_reloption(local_relopts *relopts, const char *name,
									const char *desc, int default_val,
									int min_val, int max_val, int offset);
/*
 * Function add_local_real_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_local_real_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_local_real_reloption(local_relopts *relopts, const char *name,
									 const char *desc, double default_val,
									 double min_val, double max_val,
									 int offset);
/*
 * Function add_local_enum_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_local_enum_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_local_enum_reloption(local_relopts *relopts,
									 const char *name, const char *desc,
									 relopt_enum_elt_def *members,
									 int default_val, const char *detailmsg,
									 int offset);
/*
 * Function add_local_string_reloption carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 add_local_string_reloption通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern void add_local_string_reloption(local_relopts *relopts, const char *name,
									   const char *desc,
									   const char *default_val,
									   validate_string_relopt validator,
									   fill_string_relopt filler, int offset);

/*
 * Function transformRelOptions constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 transformRelOptions通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern Datum transformRelOptions(Datum oldOptions, List *defList,
								 const char *namspace, const char *const validnsps[],
								 bool acceptOidsOff, bool isReset);
/*
 * Function untransformRelOptions constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 untransformRelOptions通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern List *untransformRelOptions(Datum options);
/*
 * Function extractRelOptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 extractRelOptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
								amoptions_function amoptions);
/*
 * Function build_reloptions constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 build_reloptions通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void *build_reloptions(Datum reloptions, bool validate,
							  relopt_kind kind,
							  Size relopt_struct_size,
							  const relopt_parse_elt *relopt_elems,
							  int num_relopt_elems);
/*
 * Function build_local_reloptions constructs or changes access-layer data by processing the supplied values, applying required storage rules, and producing the updated result.
 *
 * 函数 build_local_reloptions通过处理传入值、应用所需的存储规则并产生更新结果来构造或变更访问层数据。
 */
extern void *build_local_reloptions(local_relopts *relopts, Datum options,
									bool validate);

/*
 * Function default_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 default_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *default_reloptions(Datum reloptions, bool validate,
								 relopt_kind kind);
/*
 * Function heap_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 heap_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);
/*
 * Function view_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 view_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *view_reloptions(Datum reloptions, bool validate);
/*
 * Function partitioned_table_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 partitioned_table_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *partitioned_table_reloptions(Datum reloptions, bool validate);
/*
 * Function index_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 index_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *index_reloptions(amoptions_function amoptions, Datum reloptions,
							   bool validate);
/*
 * Function attribute_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 attribute_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *attribute_reloptions(Datum reloptions, bool validate);
/*
 * Function tablespace_reloptions carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 tablespace_reloptions通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
extern bytea *tablespace_reloptions(Datum reloptions, bool validate);
/*
 * Function AlterTableGetRelOptionsLockLevel retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 AlterTableGetRelOptionsLockLevel通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
extern LOCKMODE AlterTableGetRelOptionsLockLevel(List *defList);

#endif							/* RELOPTIONS_H */

/* 中文翻译：RELOPTIONS_H */
