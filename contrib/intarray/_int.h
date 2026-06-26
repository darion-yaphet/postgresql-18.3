/*
 * contrib/intarray/_int.h
 */
#ifndef ___INT_H__
#define ___INT_H__

#include "utils/array.h"
#include "utils/memutils.h"

/* number ranges for compression
 *
 * 压缩的数字范围
 */
#define G_INT_NUMRANGES_DEFAULT		100
#define G_INT_NUMRANGES_MAX			((GISTMaxIndexKeySize - VARHDRSZ) / \
									 (2 * sizeof(int32)))
#define G_INT_GET_NUMRANGES()		(PG_HAS_OPCLASS_OPTIONS() ? \
									 ((GISTIntArrayOptions *) PG_GET_OPCLASS_OPTIONS())->num_ranges : \
									 G_INT_NUMRANGES_DEFAULT)

/* gist__int_ops opclass options
 *
 * gist__int_ops opclass 选项
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			num_ranges;		/* number of ranges */
} GISTIntArrayOptions;

/* useful macros for accessing int4 arrays
 *
 * 用于访问 int4 数组的有用宏
 */
#define ARRPTR(x)  ( (int32 *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems(ARR_NDIM(x), ARR_DIMS(x))

/* reject arrays we can't handle; to wit, those containing nulls
 *
 * 拒绝我们无法处理的数组；也就是说，那些包含空值的
 */
#define CHECKARRVALID(x) \
	do { \
		if (ARR_HASNULL(x) && array_contains_nulls(x)) \
			ereport(ERROR, \
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), \
					 errmsg("array must not contain nulls"))); \
	} while(0)

#define ARRISEMPTY(x)  (ARRNELEMS(x) == 0)

/* sort the elements of the array
 *
 * 对数组的元素进行排序
 */
#define SORT(x) \
	do { \
		int		_nelems_ = ARRNELEMS(x); \
		bool _ascending = true; \
		isort(ARRPTR(x), _nelems_, &_ascending); \
	} while(0)

/* sort the elements of the array and remove duplicates
 *
 * 对数组元素进行排序并删除重复项
 */
#define PREPAREARR(x) \
	do { \
		int		_nelems_ = ARRNELEMS(x); \
		bool _ascending = true; \
		isort(ARRPTR(x), _nelems_, &_ascending); \
		(x) = _int_unique(x); \
	} while(0)

/* "wish" function
 *
 * “愿望”功能
 */
#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


/* bigint defines
 *
 * bigint 定义
 */
#define SIGLEN_DEFAULT		(63 * 4)
#define SIGLEN_MAX			GISTMaxIndexKeySize
#define SIGLENBIT(siglen)	((siglen) * BITS_PER_BYTE)
#define GET_SIGLEN()		(PG_HAS_OPCLASS_OPTIONS() ? \
							 ((GISTIntArrayBigOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
							 SIGLEN_DEFAULT)

typedef char *BITVECP;

#define LOOPBYTE(siglen) \
			for (i = 0; i < siglen; i++)

/* beware of multiple evaluation of arguments to these macros!
 *
 * 当心这些宏参数的多次求值！
 */
#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITS_PER_BYTE ) ) )
#define GETBITBYTE(x,i) ( (*((char*)(x)) >> (i)) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITS_PER_BYTE )) & 0x01 )
#define HASHVAL(val, siglen) (((unsigned int)(val)) % SIGLENBIT(siglen))
#define HASH(sign, val, siglen) SETBIT((sign), HASHVAL(val, siglen))

/* gist__intbig_ops opclass options
 *
 * gist__intbig_ops opclass 选项
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			siglen;			/* signature length in bytes */
} GISTIntArrayBigOptions;

/*
 * type of index key
 *
 * 索引键的类型
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} GISTTYPE;

#define ALLISTRUE		0x04

#define ISALLTRUE(x)	( ((GISTTYPE*)x)->flag & ALLISTRUE )

#define GTHDRSIZE		(VARHDRSZ + sizeof(int32))
#define CALCGTSIZE(flag, siglen) ( GTHDRSIZE+(((flag) & ALLISTRUE) ? 0 : (siglen)) )

#define GETSIGN(x)		( (BITVECP)( (char*)x+GTHDRSIZE ) )

/*
 * useful functions
 *
 * 有用的功能
 */
void		isort(int32 *a, size_t len, void *arg);
ArrayType  *new_intArrayType(int num);
ArrayType  *copy_intArrayType(ArrayType *a);
ArrayType  *resize_intArrayType(ArrayType *a, int num);
int			internal_size(int *a, int len);
ArrayType  *_int_unique(ArrayType *r);
int32		intarray_match_first(ArrayType *a, int32 elem);
ArrayType  *intarray_add_elem(ArrayType *a, int32 elem);
ArrayType  *intarray_concat_arrays(ArrayType *a, ArrayType *b);
ArrayType  *int_to_intset(int32 elem);
bool		inner_int_overlap(ArrayType *a, ArrayType *b);
bool		inner_int_contains(ArrayType *a, ArrayType *b);
ArrayType  *inner_int_union(ArrayType *a, ArrayType *b);
ArrayType  *inner_int_inter(ArrayType *a, ArrayType *b);
void		rt__int_size(ArrayType *a, float *size);
void		gensign(BITVECP sign, int *a, int len, int siglen);


/*****************************************************************************
 *			Boolean Search
 *
 * 布尔搜索
 *****************************************************************************/

#define BooleanSearchStrategy	20

/*
 * item in polish notation with back link
 * to left operand
 *
 * 采用波兰表示法的项目，带有指向左操作数的反向链接
 */
typedef struct ITEM
{
	int16		type;
	int16		left;
	int32		val;
} ITEM;

typedef struct QUERYTYPE
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		size;			/* number of ITEMs */
	ITEM		items[FLEXIBLE_ARRAY_MEMBER];
} QUERYTYPE;

#define HDRSIZEQT	offsetof(QUERYTYPE, items)
#define COMPUTESIZE(size)	( HDRSIZEQT + (size) * sizeof(ITEM) )
#define QUERYTYPEMAXITEMS	((MaxAllocSize - HDRSIZEQT) / sizeof(ITEM))
#define GETQUERY(x)  ( (x)->items )

/* "type" codes for ITEM
 *
 * ITEM 的“类型”代码
 */
#define END		0
#define ERR		1
#define VAL		2
#define OPR		3
#define OPEN	4
#define CLOSE	5

/* fmgr macros for QUERYTYPE objects
 *
 * QUERYTYPE 对象的 fmgr 宏
 */
#define DatumGetQueryTypeP(X)		  ((QUERYTYPE *) PG_DETOAST_DATUM(X))
#define DatumGetQueryTypePCopy(X)	  ((QUERYTYPE *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_QUERYTYPE_P(n)	  DatumGetQueryTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_QUERYTYPE_P_COPY(n) DatumGetQueryTypePCopy(PG_GETARG_DATUM(n))

bool		signconsistent(QUERYTYPE *query, BITVECP sign, int siglen, bool calcnot);
bool		execconsistent(QUERYTYPE *query, ArrayType *array, bool calcnot);

bool		gin_bool_consistent(QUERYTYPE *query, bool *check);
bool		query_has_required_values(QUERYTYPE *query);

/* sort, either ascending or descending
 *
 * 升序或降序排序
 */
#define QSORT(a, direction) \
	do { \
		int		_nelems_ = ARRNELEMS(a); \
		bool _ascending = (direction) ? true : false; \
		isort(ARRPTR(a), _nelems_, &_ascending); \
	} while(0)

#endif							/* ___INT_H__ */
