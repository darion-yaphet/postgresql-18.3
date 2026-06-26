/*
 * contrib/hstore/hstore.h
 */
#ifndef __HSTORE_H__
#define __HSTORE_H__

#include "fmgr.h"
#include "utils/array.h"


/*
 * HEntry: there is one of these for each key _and_ value in an hstore
 *
 * HEntry：hstore 中的每个键和值都有一个
 *
 * the position offset points to the _end_ so that we can get the length
 * by subtraction from the previous entry.  the ISFIRST flag lets us tell
 * whether there is a previous entry.
 *
 * 位置偏移量指向_end_，这样我们就可以通过减去前一个条目来获得长度。  ISFIRST 标志让我们知道是否有先前的条目。
 */
typedef struct
{
	uint32		entry;
} HEntry;

#define HENTRY_ISFIRST 0x80000000
#define HENTRY_ISNULL  0x40000000
#define HENTRY_POSMASK 0x3FFFFFFF

/* note possible multiple evaluations, also access to prior array element
 *
 * 注意可能的多次评估，还可以访问先前的数组元素
 */
#define HSE_ISFIRST(he_) (((he_).entry & HENTRY_ISFIRST) != 0)
#define HSE_ISNULL(he_) (((he_).entry & HENTRY_ISNULL) != 0)
#define HSE_ENDPOS(he_) ((he_).entry & HENTRY_POSMASK)
#define HSE_OFF(he_) (HSE_ISFIRST(he_) ? 0 : HSE_ENDPOS((&(he_))[-1]))
#define HSE_LEN(he_) (HSE_ISFIRST(he_)	\
					  ? HSE_ENDPOS(he_) \
					  : HSE_ENDPOS(he_) - HSE_ENDPOS((&(he_))[-1]))

/*
 * determined by the size of "endpos" (ie HENTRY_POSMASK), though this is a
 * bit academic since currently varlenas (and hence both the input and the
 * whole hstore) have the same limit
 *
 * 由“endpos”（即 HENTRY_POSMASK）的大小决定，尽管这有点学术性，因为当前 varlenas（以及输入和整个 hstore）具有相同的限制
 */
#define HSTORE_MAX_KEY_LEN 0x3FFFFFFF
#define HSTORE_MAX_VALUE_LEN 0x3FFFFFFF

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint32		size_;			/* flags and number of items in hstore */
	/* array of HEntry follows
	 *
	 * HEntry 数组如下
	 */
} HStore;

/*
 * It's not possible to get more than 2^28 items into an hstore, so we reserve
 * the top few bits of the size field.  See hstore_compat.c for one reason
 * why.  Some bits are left for future use here.  MaxAllocSize makes the
 * practical count limit slightly more than 2^28 / 3, or INT_MAX / 24, the
 * limit for an hstore full of 4-byte keys and null values.  Therefore, we
 * don't explicitly check the format-imposed limit.
 *
 * 不可能将超过 2^28 个项目放入 hstore，因此我们保留 size 字段的前几位。  原因之一请参见 hstore_compat.c。  这里留下一些位以供将来使用。  MaxAllocSize 使实际计数限制略大于 2^28 / 3 或 INT_MAX / 24，即充满 4 字节键和空值的 hstore 的限制。  因此，我们不会明确检查格式施加的限制。
 */
#define HS_FLAG_NEWVERSION 0x80000000

#define HS_COUNT(hsp_) ((hsp_)->size_ & 0x0FFFFFFF)
#define HS_SETCOUNT(hsp_,c_) ((hsp_)->size_ = (c_) | HS_FLAG_NEWVERSION)


/*
 * "x" comes from an existing HS_COUNT() (as discussed, <= INT_MAX/24) or a
 * Pairs array length (due to MaxAllocSize, <= INT_MAX/40).  "lenstr" is no
 * more than INT_MAX, that extreme case arising in hstore_from_arrays().
 * Therefore, this calculation is limited to about INT_MAX / 5 + INT_MAX.
 *
 * “x”来自现有的 HS_COUNT()（如上所述，<= INT_MAX/24）或 Pairs 数组长度（由于 MaxAllocSize，<= INT_MAX/40）。  “lenstr”不超过 INT_MAX，即 hstore_from_arrays() 中出现的极端情况。因此，此计算仅限于 INT_MAX / 5 + INT_MAX 左右。
 */
#define HSHRDSIZE	(sizeof(HStore))
#define CALCDATASIZE(x, lenstr) ( (x) * 2 * sizeof(HEntry) + HSHRDSIZE + (lenstr) )

/* note multiple evaluations of x
 *
 * 注意 x 的多重评估
 */
#define ARRPTR(x)		( (HEntry*) ( (HStore*)(x) + 1 ) )
#define STRPTR(x)		( (char*)(ARRPTR(x) + HS_COUNT((HStore*)(x)) * 2) )

/* note multiple/non evaluations
 *
 * 注意多重/非评估
 */
#define HSTORE_KEY(arr_,str_,i_)	((str_) + HSE_OFF((arr_)[2*(i_)]))
#define HSTORE_VAL(arr_,str_,i_)	((str_) + HSE_OFF((arr_)[2*(i_)+1]))
#define HSTORE_KEYLEN(arr_,i_)		(HSE_LEN((arr_)[2*(i_)]))
#define HSTORE_VALLEN(arr_,i_)		(HSE_LEN((arr_)[2*(i_)+1]))
#define HSTORE_VALISNULL(arr_,i_)	(HSE_ISNULL((arr_)[2*(i_)+1]))

/*
 * currently, these following macros are the _only_ places that rely
 * on internal knowledge of HEntry. Everything else should be using
 * the above macros. Exception: the in-place upgrade in hstore_compat.c
 * messes with entries directly.
 *
 * 目前，以下宏是唯一依赖 HEntry 内部知识的地方。其他一切都应该使用上面的宏。例外：hstore_compat.c 中的就地升级直接与条目混淆。
 */

/*
 * copy one key/value pair (which must be contiguous starting at
 * sptr_) into an under-construction hstore; dent_ is an HEntry*,
 * dbuf_ is the destination's string buffer, dptr_ is the current
 * position in the destination. lots of modification and multiple
 * evaluation here.
 *
 * 将一个键/值对（必须从 sptr_ 开始连续）复制到正在构建的 hstore 中； dent_ 是 HEntry*，dbuf_ 是目标的字符串缓冲区，dptr_ 是目标中的当前位置。这里有很多修改和多次评估。
 */
#define HS_COPYITEM(dent_,dbuf_,dptr_,sptr_,klen_,vlen_,vnull_)			\
	do {																\
		memcpy((dptr_), (sptr_), (klen_)+(vlen_));						\
		(dptr_) += (klen_)+(vlen_);										\
		(dent_)++->entry = ((dptr_) - (dbuf_) - (vlen_)) & HENTRY_POSMASK; \
		(dent_)++->entry = ((((dptr_) - (dbuf_)) & HENTRY_POSMASK)		\
							 | ((vnull_) ? HENTRY_ISNULL : 0));			\
	} while(0)

/*
 * add one key/item pair, from a Pairs structure, into an
 * under-construction hstore
 *
 * 将一个键/项目对从 Pairs 结构添加到正在建设中的 hstore 中
 */
#define HS_ADDITEM(dent_,dbuf_,dptr_,pair_)								\
	do {																\
		memcpy((dptr_), (pair_).key, (pair_).keylen);					\
		(dptr_) += (pair_).keylen;										\
		(dent_)++->entry = ((dptr_) - (dbuf_)) & HENTRY_POSMASK;		\
		if ((pair_).isnull)												\
			(dent_)++->entry = ((((dptr_) - (dbuf_)) & HENTRY_POSMASK)	\
								 | HENTRY_ISNULL);						\
		else															\
		{																\
			memcpy((dptr_), (pair_).val, (pair_).vallen);				\
			(dptr_) += (pair_).vallen;									\
			(dent_)++->entry = ((dptr_) - (dbuf_)) & HENTRY_POSMASK;	\
		}																\
	} while (0)

/* finalize a newly-constructed hstore
 *
 * 完成新建的hstore
 */
#define HS_FINALIZE(hsp_,count_,buf_,ptr_)							\
	do {															\
		int _buflen = (ptr_) - (buf_);								\
		if ((count_))												\
			ARRPTR(hsp_)[0].entry |= HENTRY_ISFIRST;				\
		if ((count_) != HS_COUNT((hsp_)))							\
		{															\
			HS_SETCOUNT((hsp_),(count_));							\
			memmove(STRPTR(hsp_), (buf_), _buflen);					\
		}															\
		SET_VARSIZE((hsp_), CALCDATASIZE((count_), _buflen));		\
	} while (0)

/* ensure the varlena size of an existing hstore is correct
 *
 * 确保现有 hstore 的 varlena 大小正确
 */
#define HS_FIXSIZE(hsp_,count_)											\
	do {																\
		int bl = (count_) ? HSE_ENDPOS(ARRPTR(hsp_)[2*(count_)-1]) : 0; \
		SET_VARSIZE((hsp_), CALCDATASIZE((count_),bl));					\
	} while (0)

/* DatumGetHStoreP includes support for reading old-format hstore values
 *
 * DatumGetHStoreP 包括对读取旧格式 hstore 值的支持
 */
extern PGDLLEXPORT HStore *hstoreUpgrade(Datum orig);

#define DatumGetHStoreP(d) hstoreUpgrade(d)

#define PG_GETARG_HSTORE_P(x) DatumGetHStoreP(PG_GETARG_DATUM(x))


/*
 * Pairs is a "decompressed" representation of one key/value pair.
 * The two strings are not necessarily null-terminated.
 *
 * 对是一个键/值对的“解压缩”表示。这两个字符串不一定以空终止。
 */
typedef struct
{
	char	   *key;
	char	   *val;
	size_t		keylen;
	size_t		vallen;
	bool		isnull;			/* value is null? */
	bool		needfree;		/* need to pfree the value? */
} Pairs;

extern PGDLLEXPORT int hstoreUniquePairs(Pairs *a, int32 l, int32 *buflen);
extern PGDLLEXPORT HStore *hstorePairs(Pairs *pairs, int32 pcount, int32 buflen);

extern PGDLLEXPORT size_t hstoreCheckKeyLen(size_t len);
extern PGDLLEXPORT size_t hstoreCheckValLen(size_t len);

extern PGDLLEXPORT int hstoreFindKey(HStore *hs, int *lowbound, char *key, int keylen);
extern PGDLLEXPORT Pairs *hstoreArrayToPairs(ArrayType *a, int *npairs);

#define HStoreContainsStrategyNumber	7
#define HStoreExistsStrategyNumber		9
#define HStoreExistsAnyStrategyNumber	10
#define HStoreExistsAllStrategyNumber	11
#define HStoreOldContainsStrategyNumber 13	/* backwards compatibility */

/*
 * defining HSTORE_POLLUTE_NAMESPACE=0 will prevent use of old function names;
 * for now, we default to on for the benefit of people restoring old dumps
 *
 * 定义 HSTORE_POLLUTE_NAMESPACE=0 将阻止使用旧函数名称；目前，为了人们恢复旧转储的利益，我们默认启用
 */
#ifndef HSTORE_POLLUTE_NAMESPACE
#define HSTORE_POLLUTE_NAMESPACE 1
#endif

#if HSTORE_POLLUTE_NAMESPACE
#define HSTORE_POLLUTE(newname_,oldname_) \
	PG_FUNCTION_INFO_V1(oldname_);		  \
	extern PGDLLEXPORT Datum newname_(PG_FUNCTION_ARGS);	  \
	Datum oldname_(PG_FUNCTION_ARGS) { return newname_(fcinfo); } \
	extern int no_such_variable
#else
#define HSTORE_POLLUTE(newname_,oldname_) \
	extern int no_such_variable
#endif

#endif							/* __HSTORE_H__ */
