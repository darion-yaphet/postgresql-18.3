/*
 * File imported from FreeBSD, original by Poul-Henning Kamp.
 *
 * 从 FreeBSD 导入的文件，由 Poul-Henning Kamp 原创。
 *
 * $FreeBSD: src/lib/libcrypt/crypt-md5.c,v 1.5 1999/12/17 20:21:45 peter Exp $
 *
 * $FreeBSD: src/lib/libcrypt/crypt-md5.c,v 1.5 1999/12/17 20:21:45 彼得 Exp $
 *
 * contrib/pgcrypto/crypt-md5.c
 */

#include "postgres.h"

#include "px-crypt.h"
#include "px.h"

#define MD5_SIZE 16

static const char _crypt_a64[] =
"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void
_crypt_to64(char *s, unsigned long v, int n)
{
	while (--n >= 0)
	{
		*s++ = _crypt_a64[v & 0x3f];
		v >>= 6;
	}
}

/*
 * UNIX password
 *
 * UNIX密码
 */

char *
px_crypt_md5(const char *pw, const char *salt, char *passwd, unsigned dstlen)
{
	static const char *magic = "$1$";	/* This string is magic for this
										 * algorithm. Having it this way, we
										 *
										 * 算法。如此一来，我们
										 * can get better later on */
	char	   *p;
	const char *sp,
			   *ep;
	unsigned char final[MD5_SIZE];
	int			sl,
				pl,
				i;
	PX_MD	   *ctx,
			   *ctx1;
	int			err;
	unsigned long l;

	if (!passwd || dstlen < 120)
		return NULL;

	/* Refine the Salt first
	 *
	 * 先把盐精炼一下
	 */
	sp = salt;

	/* If it starts with the magic string, then skip that
	 *
	 * 如果它以魔术字符串开头，则跳过它
	 */
	if (strncmp(sp, magic, strlen(magic)) == 0)
		sp += strlen(magic);

	/* It stops at the first '$', max 8 chars
	 *
	 * 它停在第一个“$”处，最多 8 个字符
	 */
	for (ep = sp; *ep && *ep != '$' && ep < (sp + 8); ep++)
		continue;

	/* get the length of the true salt
	 *
	 * 得到真盐的长度
	 */
	sl = ep - sp;

	/* we need two PX_MD objects
	 *
	 * 我们需要两个 PX_MD 对象
	 */
	err = px_find_digest("md5", &ctx);
	if (err)
		return NULL;
	err = px_find_digest("md5", &ctx1);
	if (err)
	{
		/* this path is possible under low-memory circumstances
		 *
		 * 此路径在内存不足的情况下是可能的
		 */
		px_md_free(ctx);
		return NULL;
	}

	/* The password first, since that is what is most unknown
	 *
	 * 首先是密码，因为这是最不为人所知的
	 */
	px_md_update(ctx, (const uint8 *) pw, strlen(pw));

	/* Then our magic string
	 *
	 * 然后我们的魔法弦
	 */
	px_md_update(ctx, (const uint8 *) magic, strlen(magic));

	/* Then the raw salt
	 *
	 * 然后是原盐
	 */
	px_md_update(ctx, (const uint8 *) sp, sl);

	/* Then just as many characters of the MD5(pw,salt,pw)
	 *
	 * 然后就和 MD5(pw,salt,pw) 的字符一样多
	 */
	px_md_update(ctx1, (const uint8 *) pw, strlen(pw));
	px_md_update(ctx1, (const uint8 *) sp, sl);
	px_md_update(ctx1, (const uint8 *) pw, strlen(pw));
	px_md_finish(ctx1, final);
	for (pl = strlen(pw); pl > 0; pl -= MD5_SIZE)
		px_md_update(ctx, final, pl > MD5_SIZE ? MD5_SIZE : pl);

	/* Don't leave anything around in vm they could use.
	 *
	 * 不要在他们可以使用的虚拟机中留下任何东西。
	 */
	px_memset(final, 0, sizeof final);

	/* Then something really weird...
	 *
	 * 然后就发生了一件非常奇怪的事情...
	 */
	for (i = strlen(pw); i; i >>= 1)
		if (i & 1)
			px_md_update(ctx, final, 1);
		else
			px_md_update(ctx, (const uint8 *) pw, 1);

	/* Now make the output string
	 *
	 * 现在制作输出字符串
	 */
	strcpy(passwd, magic);
	strncat(passwd, sp, sl);
	strcat(passwd, "$");

	px_md_finish(ctx, final);

	/*
	 * and now, just to make sure things don't run too fast On a 60 Mhz
	 * Pentium this takes 34 msec, so you would need 30 seconds to build a
	 * 1000 entry dictionary...
	 *
	 * 现在，为了确保事情不会运行得太快，在 60 Mhz Pentium 上，这需要 34 毫秒，因此您需要 30 秒来构建一个 1000 个条目的字典......
	 */
	for (i = 0; i < 1000; i++)
	{
		px_md_reset(ctx1);
		if (i & 1)
			px_md_update(ctx1, (const uint8 *) pw, strlen(pw));
		else
			px_md_update(ctx1, final, MD5_SIZE);

		if (i % 3)
			px_md_update(ctx1, (const uint8 *) sp, sl);

		if (i % 7)
			px_md_update(ctx1, (const uint8 *) pw, strlen(pw));

		if (i & 1)
			px_md_update(ctx1, final, MD5_SIZE);
		else
			px_md_update(ctx1, (const uint8 *) pw, strlen(pw));
		px_md_finish(ctx1, final);
	}

	p = passwd + strlen(passwd);

	l = (final[0] << 16) | (final[6] << 8) | final[12];
	_crypt_to64(p, l, 4);
	p += 4;
	l = (final[1] << 16) | (final[7] << 8) | final[13];
	_crypt_to64(p, l, 4);
	p += 4;
	l = (final[2] << 16) | (final[8] << 8) | final[14];
	_crypt_to64(p, l, 4);
	p += 4;
	l = (final[3] << 16) | (final[9] << 8) | final[15];
	_crypt_to64(p, l, 4);
	p += 4;
	l = (final[4] << 16) | (final[10] << 8) | final[5];
	_crypt_to64(p, l, 4);
	p += 4;
	l = final[11];
	_crypt_to64(p, l, 2);
	p += 2;
	*p = '\0';

	/* Don't leave anything around in vm they could use.
	 *
	 * 不要在他们可以使用的虚拟机中留下任何东西。
	 */
	px_memset(final, 0, sizeof final);

	px_md_free(ctx1);
	px_md_free(ctx);

	return passwd;
}
