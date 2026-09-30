/*-------------------------------------------------------------------------
 *
 * tupmacs.h
 *	  Tuple macros used by both index tuples and heap tuples.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupmacs.h
 *
 *-------------------------------------------------------------------------
 *
 * 中文翻译：
 * tupmacs.h 索引元组和堆元组使用的元组宏。 src/incl
 * ude/access/tupmacs.h
 */
#ifndef TUPMACS_H
#define TUPMACS_H

#include "catalog/pg_type_d.h"	/* for TYPALIGN macros */

/* 中文翻译：对于 TYPALIGN 宏 */


/*
 * Check a tuple's null bitmap to determine whether the attribute is null.
 * Note that a 0 in the null bitmap indicates a null, while 1 indicates
 * non-null.
 *
 * 中文翻译：
 * 检查元组的空位图以确定该属性是否为空。请注意，空位图中的 0 表示空
 * ，而 1 表示非空。
 */
/*
 * Function att_isnull evaluates the requested access-layer condition by examining the supplied state and returning whether the condition holds.
 *
 * 函数 att_isnull通过检查传入状态来评估请求的访问层条件，并返回该条件是否成立。
 */
static inline bool
att_isnull(int ATT, const bits8 *BITS)
{
	return !(BITS[ATT >> 3] & (1 << (ATT & 0x07)));
}

#ifndef FRONTEND
/*
 * Given an attbyval and an attlen from either a Form_pg_attribute or
 * CompactAttribute and a pointer into a tuple's data area, return the
 * correct value or pointer.
 *
 * We return a Datum value in all cases.  If attbyval is false,  we return the
 * same pointer into the tuple data area that we're passed.  Otherwise, we
 * return the correct number of bytes fetched from the data area and extended
 * to Datum form.
 *
 * On machines where Datum is 8 bytes, we support fetching 8-byte byval
 * attributes; otherwise, only 1, 2, and 4-byte values are supported.
 *
 * Note that T must already be properly aligned for this to work correctly.
 *
 * 中文翻译：
 * 给定来自 Form_pg_attribute 或 CompactAt
 * tribute 的 attbyval 和 attlen 以及指向元组
 * 数据区域的指针，返回正确的值或指针。在所有情况下我们都会返回一个 D
 * atum 值。如果 attbyval 为 false，我们将相同的指
 * 针返回到我们传递的元组数据区域中。否则，我们返回从数据区域获取并扩展
 * 为数据形式的正确字节数。在 Datum 为 8 字节的机器上，我们支
 * 持获取 8 字节 byval 属性；否则，仅支持 1、2 和 4 字
 * 节值。请注意，T 必须已正确对齐才能正常工作。
 */
#define fetchatt(A,T) fetch_att(T, (A)->attbyval, (A)->attlen)

/*
 * Same, but work from byval/len parameters rather than Form_pg_attribute.
 *
 * 中文翻译：
 * 相同，但使用 byval/len 参数而不是 Form_pg_att
 * ribute。
 */
/*
 * Function fetch_att retrieves or inspects access-layer state by interpreting the supplied identifiers or buffers and returning the matching result.
 *
 * 函数 fetch_att通过解析传入的标识符或缓冲区检索或检查访问层状态，并返回匹配结果。
 */
static inline Datum
fetch_att(const void *T, bool attbyval, int attlen)
{
	if (attbyval)
	{
		switch (attlen)
		{
			case sizeof(char):
				return CharGetDatum(*((const char *) T));
			case sizeof(int16):
				return Int16GetDatum(*((const int16 *) T));
			case sizeof(int32):
				return Int32GetDatum(*((const int32 *) T));
#if SIZEOF_DATUM == 8
			case sizeof(Datum):
				return *((const Datum *) T);
#endif
			default:
				elog(ERROR, "unsupported byval length: %d", attlen);
				return 0;
		}
	}
	else
		return PointerGetDatum(T);
}
#endif							/* FRONTEND */

/* 中文翻译：前端 */

/*
 * att_align_datum aligns the given offset as needed for a datum of alignment
 * requirement attalign and typlen attlen.  attdatum is the Datum variable
 * we intend to pack into a tuple (it's only accessed if we are dealing with
 * a varlena type).  Note that this assumes the Datum will be stored as-is;
 * callers that are intending to convert non-short varlena datums to short
 * format have to account for that themselves.
 *
 * 中文翻译：
 * att_align_datum 根据对齐要求 attalign 和
 * typelen attlen 的基准对齐给定的偏移量。 attdat
 * um 是我们打算打包到元组中的 Datum 变量（只有在我们处理 v
 * arlena 类型时才可以访问它）。请注意，这假设数据将按原样存储；
 * 想要将非短 varlena 数据转换为短格式的调用者必须自己考虑这一
 * 点。
 */
#define att_align_datum(cur_offset, attalign, attlen, attdatum) \
( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * Similar to att_align_datum, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the Datum by.
 *
 * 中文翻译：
 * 与 att_align_datum 类似，但接受多个字节，通常来自
 * CompactAttribute.attalignby 来对齐基准。
 */
#define att_datum_alignby(cur_offset, attalignby, attlen, attdatum) \
	( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_pointer performs the same calculation as att_align_datum,
 * but is used when walking a tuple.  attptr is the current actual data
 * pointer; when accessing a varlena field we have to "peek" to see if we
 * are looking at a pad byte or the first byte of a 1-byte-header datum.
 * (A zero byte must be either a pad byte, or the first byte of a correctly
 * aligned 4-byte length word; in either case we can align safely.  A non-zero
 * byte must be either a 1-byte length word, or the first byte of a correctly
 * aligned 4-byte length word; in either case we need not align.)
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * a bit of a hack but should work all right as long as uintptr_t is the
 * correct width.
 *
 * 中文翻译：
 * att_align_pointer 执行与 att_align_da
 * tum 相同的计算，但在遍历元组时使用。 attptr是当前实际数据
 * 指针；当访问 varlena 字段时，我们必须“查看”以查看我们是否
 * 正在查看填充字节或 1 字节标头数据的第一个字节。 （零字节必须是填
 * 充字节，或者正确对齐的 4 字节长度字的第一个字节；在任何一种情况下
 * ，我们都可以安全地对齐。非零字节必须是 1 字节长度字，或者正确对齐
 * 的 4 字节长度字的第一个字节；在任何一种情况下，我们都不需要对齐。
 * ） 注意：一些调用者为 cur_offset 传递“char *”指
 * 针。这有点麻烦，但只要 uintptr_t 的宽度正确，就应该可以正
 * 常工作。
 */
#define att_align_pointer(cur_offset, attalign, attlen, attptr) \
( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * Similar to att_align_pointer, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the pointer by.
 *
 * 中文翻译：
 * 与 att_align_pointer 类似，但接受多个字节，通常来
 * 自 CompactAttribute.attalignby 来对齐指
 * 针。
 */
#define att_pointer_alignby(cur_offset, attalignby, attlen, attptr) \
	( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_nominal aligns the given offset as needed for a datum of alignment
 * requirement attalign, ignoring any consideration of packed varlena datums.
 * There are three main use cases for using this macro directly:
 *	* we know that the att in question is not varlena (attlen != -1);
 *	  in this case it is cheaper than the above macros and just as good.
 *	* we need to estimate alignment padding cost abstractly, ie without
 *	  reference to a real tuple.  We must assume the worst case that
 *	  all varlenas are aligned.
 *	* within arrays and multiranges, we unconditionally align varlenas (XXX this
 *	  should be revisited, probably).
 *
 * The attalign cases are tested in what is hopefully something like their
 * frequency of occurrence.
 *
 * 中文翻译：
 * att_align_nominal 根据对齐要求 attalign
 * 的基准对齐给定的偏移量，忽略对打包 varlena 基准的任何考虑。
 * 直接使用这个宏有三个主要用例： * 我们知道有问题的 att 不是
 * varlena (attlen != -1);在这种情况下，它比上面
 * 的宏更便宜并且同样好。 * 我们需要抽象地估计对齐填充成本，即不参考
 * 真实的元组。我们必须假设最坏的情况，即所有 varlenas 都对齐
 * 。 * 在数组和多范围内，我们无条件地对齐 varlenas （XX
 * X 这可能应该被重新审视）。 attalign 案例的测试希望与它们
 * 的出现频率类似。
 */
#define att_align_nominal(cur_offset, attalign) \
( \
	((attalign) == TYPALIGN_INT) ? INTALIGN(cur_offset) : \
	 (((attalign) == TYPALIGN_CHAR) ? (uintptr_t) (cur_offset) : \
	  (((attalign) == TYPALIGN_DOUBLE) ? DOUBLEALIGN(cur_offset) : \
	   ( \
			AssertMacro((attalign) == TYPALIGN_SHORT), \
			SHORTALIGN(cur_offset) \
	   ))) \
)

/*
 * Similar to att_align_nominal, but accepts a number of bytes, typically from
 * CompactAttribute.attalignby to align the offset by.
 *
 * 中文翻译：
 * 与 att_align_nominal 类似，但接受多个字节，通常来
 * 自 CompactAttribute.attalignby 来对齐偏
 * 移量。
 */
#define att_nominal_alignby(cur_offset, attalignby) \
	TYPEALIGN(attalignby, cur_offset)

/*
 * att_addlength_datum increments the given offset by the space needed for
 * the given Datum variable.  attdatum is only accessed if we are dealing
 * with a variable-length attribute.
 *
 * 中文翻译：
 * att_addlength_datum 将给定偏移量增加给定 Dat
 * um 变量所需的空间。仅当我们处理可变长度属性时才访问 attdat
 * um。
 */
#define att_addlength_datum(cur_offset, attlen, attdatum) \
	att_addlength_pointer(cur_offset, attlen, DatumGetPointer(attdatum))

/*
 * att_addlength_pointer performs the same calculation as att_addlength_datum,
 * but is used when walking a tuple --- attptr is the pointer to the field
 * within the tuple.
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * actually perfectly OK, but probably should be cleaned up along with
 * the same practice for att_align_pointer.
 *
 * 中文翻译：
 * att_addlength_pointer 执行与 att_addl
 * ength_datum 相同的计算，但在遍历元组时使用 --- at
 * tptr 是指向元组内字段的指针。注意：一些调用者为 cur_off
 * set 传递“char *”指针。这实际上完全没问题，但可能应该与
 * att_align_pointer 的相同做法一起清理。
 */
#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(attptr) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen((char *) (attptr)) + 1) \
	)) \
)

#ifndef FRONTEND
/*
 * store_att_byval is a partial inverse of fetch_att: store a given Datum
 * value into a tuple data area at the specified address.  However, it only
 * handles the byval case, because in typical usage the caller needs to
 * distinguish by-val and by-ref cases anyway, and so a do-it-all function
 * wouldn't be convenient.
 *
 * 中文翻译：
 * store_att_byval 是 fetch_att 的部分逆：将
 * 给定 Datum 值存储到指定地址的元组数据区域中。但是，它只处理
 * byval 情况，因为在典型用法中，调用者无论如何都需要区分 by-
 * val 和 by-ref 情况，因此全能函数并不方便。
 */
/*
 * Function store_att_byval carries out its declared access-layer task by interpreting supplied inputs, applying module-specific checks or state changes, and producing the required result.
 *
 * 函数 store_att_byval通过解析传入输入、执行模块特定检查或状态变更并产生所需结果，完成其声明的访问层任务。
 */
static inline void
store_att_byval(void *T, Datum newdatum, int attlen)
{
	switch (attlen)
	{
		case sizeof(char):
			*(char *) T = DatumGetChar(newdatum);
			break;
		case sizeof(int16):
			*(int16 *) T = DatumGetInt16(newdatum);
			break;
		case sizeof(int32):
			*(int32 *) T = DatumGetInt32(newdatum);
			break;
#if SIZEOF_DATUM == 8
		case sizeof(Datum):
			*(Datum *) T = newdatum;
			break;
#endif
		default:
			elog(ERROR, "unsupported byval length: %d", attlen);
	}
}
#endif							/* FRONTEND */

/* 中文翻译：前端 */

#endif							/* TUPMACS_H */

/* 中文翻译：TUPMACS_H */
