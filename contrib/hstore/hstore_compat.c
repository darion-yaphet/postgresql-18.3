/*
 * contrib/hstore/hstore_compat.c
 *
 * Notes on old/new hstore format disambiguation.
 *
 * 有关旧/新 hstore 格式消歧的注释。
 *
 * There are three formats to consider:
 * 1) old contrib/hstore (referred to as hstore-old)
 * 2) prerelease pgfoundry hstore
 * 3) new contrib/hstore
 *
 * 需要考虑三种格式：1）旧的 contrib/hstore（称为 hstore-old）2）预发布 pgfoundry hstore 3）新的 contrib/hstore
 *
 * (2) and (3) are identical except for the HS_FLAG_NEWVERSION
 * bit, which is set in (3) but not (2).
 *
 * (2) 和 (3) 相同，但 HS_FLAG_NEWVERSION 位除外，该位在 (3) 中设置，但在 (2) 中未设置。
 *
 * Values that are already in format (3), or which are
 * unambiguously in format (2), are handled by the first
 * "return immediately" test in hstoreUpgrade().
 *
 * 已经采用格式 (3) 或明确采用格式 (2) 的值由 hstoreUpgrade() 中的第一个“立即返回”测试处理。
 *
 * To stress a point: we ONLY get here with possibly-ambiguous
 * values if we're doing some sort of in-place migration from an
 * old prerelease pgfoundry hstore-new; and we explicitly don't
 * support that without fixing up any potentially padded values
 * first. Most of the code here is serious overkill, but the
 * performance penalty isn't serious (especially compared to the
 * palloc() that we have to do anyway) and the belt-and-braces
 * validity checks provide some reassurance. (If for some reason
 * we get a value that would have worked on the old code, but
 * which would be botched by the conversion code, the validity
 * checks will fail it first so we get an error rather than bad
 * data.)
 *
 * 强调一点：如果我们从旧的预发布 pgfoundry hstore-new 进行某种就地迁移，我们只会得到可能不明确的值；如果不首先修复任何可能的填充值，我们明确不支持这一点。这里的大部分代码都是严重的矫枉过正，但性能损失并不严重（特别是与我们无论如何都必须执行的 palloc() 相比），并且带和括号的有效性检查提供了一些保证。 （如果由于某种原因，我们得到了一个可以在旧代码上使用的值，但该值会被转换代码破坏，那么有效性检查将首先失败，因此我们会得到一个错误而不是错误的数据。）
 *
 * Note also that empty hstores are the same in (2) and (3), so
 * there are some special-case paths for them.
 *
 * 另请注意，(2) 和 (3) 中的空 hstore 是相同的，因此它们有一些特殊情况的路径。
 *
 * We tell the difference between formats (2) and (3) as follows (but
 * note that there are some edge cases where we can't tell; see
 * comments in hstoreUpgrade):
 *
 * 我们如下区分格式 (2) 和 (3) 之间的差异（但请注意，有一些我们无法区分的边缘情况；请参阅 hstoreUpgrade 中的注释）：
 *
 * First, since there must be at least one entry, we look at
 * how the bits line up. The new format looks like:
 *
 * 首先，由于必须至少有一个条目，我们看看这些位是如何排列的。新格式如下所示：
 *
 * 10kkkkkkkkkkkkkkkkkkkkkkkkkkkkkk  (k..k = keylen)
 * 0nvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv  (v..v = keylen+vallen)
 *
 * 10kkkkkkkkkkkkkkkkkkkkkkkkkkkkkk (k..k = keylen) 0nvvvvvvvvvvvvvvvvvvvvvvvvvvvv (v..v = keylen+vallen)
 *
 * The old format looks like one of these, depending on endianness
 * and bitfield layout: (k..k = keylen, v..v = vallen, p..p = pos,
 * n = isnull)
 *
 * 旧格式看起来像其中之一，具体取决于字节顺序和位域布局： (k..k = keylen, v..v = vallen, p..p = pos, n = isnull)
 *
 * kkkkkkkkkkkkkkkkvvvvvvvvvvvvvvvv
 * nppppppppppppppppppppppppppppppp
 *
 * kkkkkkkkkkkkkkkkvvvvvvvvvvvvvvv npppppppppppppppppppppppppppppp
 *
 * kkkkkkkkkkkkkkkkvvvvvvvvvvvvvvvv
 * pppppppppppppppppppppppppppppppn
 *
 * kkkkkkkkkkkkkkkkvvvvvvvvvvvvvvv ppppppppppppppppppppppppppppppn
 *
 * vvvvvvvvvvvvvvvvkkkkkkkkkkkkkkkk
 * nppppppppppppppppppppppppppppppp
 *
 * vvvvvvvvvvvvvvvkkkkkkkkkkkkkkkk npppppppppppppppppppppppppppppppp
 *
 * vvvvvvvvvvvvvvvvkkkkkkkkkkkkkkkk
 * pppppppppppppppppppppppppppppppn   (usual i386 format)
 *
 * vvvvvvvvvvvvvvvkkkkkkkkkkkkkkkk pppppppppppppppppppppppppppppn (一般i386格式)
 *
 * If the entry is in old format, for the first entry "pos" must be 0.
 * We can obviously see that either keylen or vallen must be >32768
 * for there to be any ambiguity (which is why lengths less than that
 * are fasttracked in hstore.h) Since "pos"==0, the "v" field in the
 * new-format interpretation can only be 0 or 1, which constrains all
 * but three bits of the old-format's k and v fields. But in addition
 * to all of this, the data length implied by the keylen and vallen
 * must fit in the varlena size. So the only ambiguous edge case for
 * hstores with only one entry occurs between a new-format entry with
 * an excess (~32k) of padding, and an old-format entry. But we know
 * which format to use in that case based on how we were compiled, so
 * no actual data corruption can occur.
 *
 * 如果条目采用旧格式，则第一个条目“pos”必须为 0。我们可以明显看到，keylen 或 vallen 必须 >32768，否则会出现任何歧义（这就是为什么长度小于该值的长度会在 hstore.h 中快速跟踪）。由于“pos”==0，新格式解释中的“v”字段只能为 0 或 1，这限制了旧格式的 k 和 v 字段中除三位之外的所有位。但除此之外，keylen 和 vallen 隐含的数据长度必须适合 varlena 大小。因此，只有一个条目的 hstore 唯一不明确的边缘情况发生在具有过量 (~32k) 填充的新格式条目和旧格式条目之间。但我们根据编译方式知道在这种情况下使用哪种格式，因此不会发生实际的数据损坏。
 *
 * If there is more than one entry, the requirement that keys do not
 * decrease in length, and that positions increase contiguously, and
 * that the end of the data not be beyond the end of the varlena
 * itself, disambiguates in almost all other cases. There is a small
 * set of ambiguous cases which could occur if the old-format value
 * has a large excess of padding and just the right pattern of key
 * sizes, but these are also handled based on how we were compiled.
 *
 * 如果有多个条目，则密钥长度不减少、位置连续增加以及数据末尾不超出 varlena 本身末尾的要求在几乎所有其他情况下都消除了歧义。如果旧格式值有大量多余的填充和正确的密钥大小模式，则可能会出现一小部分不明确的情况，但这些情况也是根据我们的编译方式进行处理的。
 *
 * The otherwise undocumented function hstore_version_diag is provided
 * for testing purposes.
 *
 * 提供其他未记录的函数 hstore_version_diag 用于测试目的。
 */
#include "postgres.h"


#include "hstore.h"

/*
 * This is the structure used for entries in the old contrib/hstore
 * implementation. Notice that this is the same size as the new entry
 * (two 32-bit words per key/value pair) and that the header is the
 * same, so the old and new versions of ARRPTR, STRPTR, CALCDATASIZE
 * etc. are compatible.
 *
 * 这是旧 contrib/hstore 实现中用于条目的结构。请注意，这与新条目的大小相同（每个键/值对两个 32 位字），并且标头相同，因此新旧版本的 ARRPTR、STRPTR、CALCDATASIZE 等是兼容的。
 *
 * If the above statement isn't true on some bizarre platform, we're
 * a bit hosed (see StaticAssertStmt in hstoreValidOldFormat).
 *
 * 如果上述声明在某些奇怪的平台上不成立，我们就有点不知所措了（请参阅 hstoreValidOldFormat 中的 StaticAssertStmt ）。
 */
typedef struct
{
	uint16		keylen;
	uint16		vallen;
	uint32
				valisnull:1,
				pos:31;
} HOldEntry;

static int	hstoreValidNewFormat(HStore *hs);
static int	hstoreValidOldFormat(HStore *hs);


/*
 * Validity test for a new-format hstore.
 *	0 = not valid
 *	1 = valid but with "slop" in the length
 *	2 = exactly valid
 *
 * 新格式 hstore 的有效性测试。 0 = 无效 1 = 有效，但长度带有“斜率” 2 = 完全有效
 */
static int
hstoreValidNewFormat(HStore *hs)
{
	int			count = HS_COUNT(hs);
	HEntry	   *entries = ARRPTR(hs);
	int			buflen = (count) ? HSE_ENDPOS(entries[2 * (count) - 1]) : 0;
	int			vsize = CALCDATASIZE(count, buflen);
	int			i;

	if (hs->size_ & HS_FLAG_NEWVERSION)
		return 2;

	if (count == 0)
		return 2;

	if (!HSE_ISFIRST(entries[0]))
		return 0;

	if (vsize > VARSIZE(hs))
		return 0;

	/* entry position must be nondecreasing
	 *
	 * 入场位置必须是非递减的
	 */

	for (i = 1; i < 2 * count; ++i)
	{
		if (HSE_ISFIRST(entries[i])
			|| (HSE_ENDPOS(entries[i]) < HSE_ENDPOS(entries[i - 1])))
			return 0;
	}

	/* key length must be nondecreasing and keys must not be null
	 *
	 * 密钥长度必须是非递减的并且密钥不能为空
	 */

	for (i = 1; i < count; ++i)
	{
		if (HSTORE_KEYLEN(entries, i) < HSTORE_KEYLEN(entries, i - 1))
			return 0;
		if (HSE_ISNULL(entries[2 * i]))
			return 0;
	}

	if (vsize != VARSIZE(hs))
		return 1;

	return 2;
}

/*
 * Validity test for an old-format hstore.
 *	0 = not valid
 *	1 = valid but with "slop" in the length
 *	2 = exactly valid
 *
 * 旧格式 hstore 的有效性测试。 0 = 无效 1 = 有效，但长度带有“斜率” 2 = 完全有效
 */
static int
hstoreValidOldFormat(HStore *hs)
{
	int			count = hs->size_;
	HOldEntry  *entries = (HOldEntry *) ARRPTR(hs);
	int			vsize;
	int			lastpos = 0;
	int			i;

	if (hs->size_ & HS_FLAG_NEWVERSION)
		return 0;

	/* New format uses an HEntry for key and another for value
	 *
	 * 新格式使用一个 HEntry 作为键，另一个作为值
	 */
	StaticAssertStmt(sizeof(HOldEntry) == 2 * sizeof(HEntry),
					 "old hstore format is not upward-compatible");

	if (count == 0)
		return 2;

	if (count > 0xFFFFFFF)
		return 0;

	if (CALCDATASIZE(count, 0) > VARSIZE(hs))
		return 0;

	if (entries[0].pos != 0)
		return 0;

	/* key length must be nondecreasing
	 *
	 * 密钥长度必须是非递减的
	 */

	for (i = 1; i < count; ++i)
	{
		if (entries[i].keylen < entries[i - 1].keylen)
			return 0;
	}

	/*
	 * entry position must be strictly increasing, except for the first entry
	 * (which can be ""=>"" and thus zero-length); and all entries must be
	 * properly contiguous
	 *
	 * 条目位置必须严格递增，第一个条目除外（可以是“”=>””，因此长度为零）；并且所有条目必须正确连续
	 */

	for (i = 0; i < count; ++i)
	{
		if (entries[i].pos != lastpos)
			return 0;
		lastpos += (entries[i].keylen
					+ ((entries[i].valisnull) ? 0 : entries[i].vallen));
	}

	vsize = CALCDATASIZE(count, lastpos);

	if (vsize > VARSIZE(hs))
		return 0;

	if (vsize != VARSIZE(hs))
		return 1;

	return 2;
}


/*
 * hstoreUpgrade: PG_DETOAST_DATUM plus support for conversion of old hstores
 *
 * hstoreUpgrade：PG_DETOAST_DATUM 加上对旧 hstore 转换的支持
 */
HStore *
hstoreUpgrade(Datum orig)
{
	HStore	   *hs = (HStore *) PG_DETOAST_DATUM(orig);
	int			valid_new;
	int			valid_old;

	/* Return immediately if no conversion needed
	 *
	 * 如果不需要转换则立即返回
	 */
	if (hs->size_ & HS_FLAG_NEWVERSION)
		return hs;

	/* Do we have a writable copy? If not, make one.
	 *
	 * 我们有可写的副本吗？如果没有，就制作一个。
	 */
	if ((void *) hs == (void *) DatumGetPointer(orig))
		hs = (HStore *) PG_DETOAST_DATUM_COPY(orig);

	if (hs->size_ == 0 ||
		(VARSIZE(hs) < 32768 && HSE_ISFIRST((ARRPTR(hs)[0]))))
	{
		HS_SETCOUNT(hs, HS_COUNT(hs));
		HS_FIXSIZE(hs, HS_COUNT(hs));
		return hs;
	}

	valid_new = hstoreValidNewFormat(hs);
	valid_old = hstoreValidOldFormat(hs);

	if (!valid_old || hs->size_ == 0)
	{
		if (valid_new)
		{
			/*
			 * force the "new version" flag and the correct varlena length.
			 *
			 * 强制使用“新版本”标志和正确的 varlena 长度。
			 */
			HS_SETCOUNT(hs, HS_COUNT(hs));
			HS_FIXSIZE(hs, HS_COUNT(hs));
			return hs;
		}
		else
		{
			elog(ERROR, "invalid hstore value found");
		}
	}

	/*
	 * this is the tricky edge case. It is only possible in some quite extreme
	 * cases (the hstore must have had a lot of wasted padding space at the
	 * end). But the only way a "new" hstore value could get here is if we're
	 * upgrading in place from a pre-release version of hstore-new (NOT
	 * contrib/hstore), so we work off the following assumptions: 1. If you're
	 * moving from old contrib/hstore to hstore-new, you're required to fix up
	 * any potential conflicts first, e.g. by running ALTER TABLE ... USING
	 * col::text::hstore; on all hstore columns before upgrading. 2. If you're
	 * moving from old contrib/hstore to new contrib/hstore, then "new" values
	 * are impossible here 3. If you're moving from pre-release hstore-new to
	 * hstore-new, then "old" values are impossible here 4. If you're moving
	 * from pre-release hstore-new to new contrib/hstore, you're not doing so
	 * as an in-place upgrade, so there is no issue So the upshot of all this
	 * is that we can treat all the edge cases as "new" if we're being built
	 * as hstore-new, and "old" if we're being built as contrib/hstore.
	 *
	 * 这是棘手的边缘情况。只有在一些非常极端的情况下才有可能（hstore 最后一定有很多浪费的填充空间）。但是，获得“新”hstore 值的唯一方法是，如果我们从 hstore-new 的预发布版本（而不是 contrib/hstore）进行升级，因此我们会做出以下假设： 1. 如果您要从旧的 contrib/hstore 迁移到 hstore-new，则需要首先修复任何潜在的冲突，例如通过运行 ALTER TABLE ... USING col::text::hstore;升级之前在所有 hstore 列上。 2. 如果您要从旧的 contrib/hstore 迁移到新的 contrib/hstore，那么这里不可能出现“新”值 3. 如果您要从预发布的 hstore-new 迁移到 hstore-new，那么这里不可能出现“旧”值 4. 如果您要从预发布的 hstore-new 迁移到新的 contrib/hstore，那么您并不是就地升级，因此不会有问题 所以所有这一切的结果是我们可以处理所有如果我们构建为 hstore-new，则边缘情况为“新”；如果我们构建为 contrib/hstore，则边缘情况为“旧”。
	 *
	 * XXX the WARNING can probably be downgraded to DEBUG1 once this has been
	 * beta-tested. But for now, it would be very useful to know if anyone can
	 * actually reach this case in a non-contrived setting.
	 *
	 * XXX 一旦完成 Beta 测试，警告可能会降级为 DEBUG1。但就目前而言，了解是否有人能够在非人为的环境中真正实现此案例将非常有用。
	 */

	if (valid_new)
	{
#ifdef HSTORE_IS_HSTORE_NEW
		elog(WARNING, "ambiguous hstore value resolved as hstore-new");

		/*
		 * force the "new version" flag and the correct varlena length.
		 *
		 * 强制使用“新版本”标志和正确的 varlena 长度。
		 */
		HS_SETCOUNT(hs, HS_COUNT(hs));
		HS_FIXSIZE(hs, HS_COUNT(hs));
		return hs;
#else
		elog(WARNING, "ambiguous hstore value resolved as hstore-old");
#endif
	}

	/*
	 * must have an old-style value. Overwrite it in place as a new-style one.
	 *
	 * 必须具有旧式值。将其覆盖为新样式。
	 */
	{
		int			count = hs->size_;
		HEntry	   *new_entries = ARRPTR(hs);
		HOldEntry  *old_entries = (HOldEntry *) ARRPTR(hs);
		int			i;

		for (i = 0; i < count; ++i)
		{
			uint32		pos = old_entries[i].pos;
			uint32		keylen = old_entries[i].keylen;
			uint32		vallen = old_entries[i].vallen;
			bool		isnull = old_entries[i].valisnull;

			if (isnull)
				vallen = 0;

			new_entries[2 * i].entry = (pos + keylen) & HENTRY_POSMASK;
			new_entries[2 * i + 1].entry = (((pos + keylen + vallen) & HENTRY_POSMASK)
											| ((isnull) ? HENTRY_ISNULL : 0));
		}

		if (count)
			new_entries[0].entry |= HENTRY_ISFIRST;
		HS_SETCOUNT(hs, count);
		HS_FIXSIZE(hs, count);
	}

	return hs;
}


PG_FUNCTION_INFO_V1(hstore_version_diag);
Datum
hstore_version_diag(PG_FUNCTION_ARGS)
{
	HStore	   *hs = (HStore *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	int			valid_new = hstoreValidNewFormat(hs);
	int			valid_old = hstoreValidOldFormat(hs);

	PG_RETURN_INT32(valid_old * 10 + valid_new);
}
