/*-------------------------------------------------------------------------
 *
 * UUID generation functions using the BSD, E2FS or OSSP UUID library
 *
 * Copyright (c) 2007-2025, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2009 Andrew Gierth
 *
 * contrib/uuid-ossp/uuid-ossp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/cryptohash.h"
#include "common/sha1.h"
#include "fmgr.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/uuid.h"
#include "varatt.h"

/*
 * It's possible that there's more than one uuid.h header file present.
 * We expect configure to set the HAVE_ symbol for only the one we want.
 *
 * 可能存在多个 uuid.h 头文件。我们希望configure 只为我们想要的符号设置HAVE_ 符号。
 *
 * BSD includes a uuid_hash() function that conflicts with the one in
 * builtins.h; we #define it out of the way.
 *
 * BSD 包含一个 uuid_hash() 函数，该函数与builtins.h 中的函数冲突；我们#define它了。
 */
#define uuid_hash bsd_uuid_hash

#if defined(HAVE_UUID_H)
#include <uuid.h>
#elif defined(HAVE_OSSP_UUID_H)
#include <ossp/uuid.h>
#elif defined(HAVE_UUID_UUID_H)
#include <uuid/uuid.h>
#else
#error "please use configure's --with-uuid switch to select a UUID library"
#endif

#undef uuid_hash

/* Check our UUID length against OSSP's; better both be 16
 *
 * 对照 OSSP 检查我们的 UUID 长度；最好都是16岁
 */
#if defined(HAVE_UUID_OSSP) && (UUID_LEN != UUID_LEN_BIN)
#error UUID length mismatch
#endif

/* Define some constants like OSSP's, to make the code more readable
 *
 * 定义一些常量，如 OSSP 的常量，使代码更具可读性
 */
#ifndef HAVE_UUID_OSSP
#define UUID_MAKE_MC 0
#define UUID_MAKE_V1 1
#define UUID_MAKE_V2 2
#define UUID_MAKE_V3 3
#define UUID_MAKE_V4 4
#define UUID_MAKE_V5 5
#endif

/*
 * A DCE 1.1 compatible source representation of UUIDs, derived from
 * the BSD implementation.  BSD already has this; OSSP doesn't need it.
 *
 * UUID 的 DCE 1.1 兼容源表示，源自 BSD 实现。  BSD 已经有了这个； OSSP 不需要它。
 */
#ifdef HAVE_UUID_E2FS
typedef struct
{
	uint32_t	time_low;
	uint16_t	time_mid;
	uint16_t	time_hi_and_version;
	uint8_t		clock_seq_hi_and_reserved;
	uint8_t		clock_seq_low;
	uint8_t		node[6];
} dce_uuid_t;
#else
#define dce_uuid_t uuid_t
#endif

/* If not OSSP, we need some endianness-manipulation macros
 *
 * 如果不是 OSSP，我们需要一些字节顺序操作宏
 */
#ifndef HAVE_UUID_OSSP

#define UUID_TO_NETWORK(uu) \
do { \
	uu.time_low = pg_hton32(uu.time_low); \
	uu.time_mid = pg_hton16(uu.time_mid); \
	uu.time_hi_and_version = pg_hton16(uu.time_hi_and_version); \
} while (0)

#define UUID_TO_LOCAL(uu) \
do { \
	uu.time_low = pg_ntoh32(uu.time_low); \
	uu.time_mid = pg_ntoh16(uu.time_mid); \
	uu.time_hi_and_version = pg_ntoh16(uu.time_hi_and_version); \
} while (0)

#define UUID_V3_OR_V5(uu, v) \
do { \
	uu.time_hi_and_version &= 0x0FFF; \
	uu.time_hi_and_version |= (v << 12); \
	uu.clock_seq_hi_and_reserved &= 0x3F; \
	uu.clock_seq_hi_and_reserved |= 0x80; \
} while(0)

#endif							/* !HAVE_UUID_OSSP */

PG_MODULE_MAGIC_EXT(
					.name = "uuid-ossp",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(uuid_nil);
PG_FUNCTION_INFO_V1(uuid_ns_dns);
PG_FUNCTION_INFO_V1(uuid_ns_url);
PG_FUNCTION_INFO_V1(uuid_ns_oid);
PG_FUNCTION_INFO_V1(uuid_ns_x500);

PG_FUNCTION_INFO_V1(uuid_generate_v1);
PG_FUNCTION_INFO_V1(uuid_generate_v1mc);
PG_FUNCTION_INFO_V1(uuid_generate_v3);
PG_FUNCTION_INFO_V1(uuid_generate_v4);
PG_FUNCTION_INFO_V1(uuid_generate_v5);

#ifdef HAVE_UUID_OSSP

static void
pguuid_complain(uuid_rc_t rc)
{
	char	   *err = uuid_error(rc);

	if (err != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("OSSP uuid library failure: %s", err)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("OSSP uuid library failure: error code %d", rc)));
}

/*
 * We create a uuid_t object just once per session and re-use it for all
 * operations in this module.  OSSP UUID caches the system MAC address and
 * other state in this object.  Reusing the object has a number of benefits:
 * saving the cycles needed to fetch the system MAC address over and over,
 * reducing the amount of entropy we draw from /dev/urandom, and providing a
 * positive guarantee that successive generated V1-style UUIDs don't collide.
 * (On a machine fast enough to generate multiple UUIDs per microsecond,
 * or whatever the system's wall-clock resolution is, we'd otherwise risk
 * collisions whenever random initialization of the uuid_t's clock sequence
 * value chanced to produce duplicates.)
 *
 * 我们每个会话只创建一次 uuid_t 对象，并将其重新用于该模块中的所有操作。  OSSP UUID 在此对象中缓存系统 MAC 地址和其他状态。  重用该对象有很多好处：节省反复获取系统 MAC 地址所需的周期，减少我们从 /dev/urandom 中获取的熵量，并提供连续生成的 V1 样式 UUID 不会冲突的积极保证。 （在一台足够快每微秒生成多个 UUID 的机器上，或者无论系统的挂钟分辨率如何，否则只要 uuid_t 时钟序列值的随机初始化有可能产生重复，我们就会面临冲突的风险。）
 *
 * However: when we're doing V3 or V5 UUID creation, uuid_make needs two
 * uuid_t objects, one holding the namespace UUID and one for the result.
 * It's unspecified whether it's safe to use the same uuid_t for both cases,
 * so let's cache a second uuid_t for use as the namespace holder object.
 *
 * 然而：当我们进行 V3 或 V5 UUID 创建时，uuid_make 需要两个 uuid_t 对象，一个保存命名空间 UUID，另一个保存结果。未指定在这两种情况下使用相同的 uuid_t 是否安全，因此让我们缓存第二个 uuid_t 用作命名空间持有者对象。
 */
static uuid_t *
get_cached_uuid_t(int which)
{
	static uuid_t *cached_uuid[2] = {NULL, NULL};

	if (cached_uuid[which] == NULL)
	{
		uuid_rc_t	rc;

		rc = uuid_create(&cached_uuid[which]);
		if (rc != UUID_RC_OK)
		{
			cached_uuid[which] = NULL;
			pguuid_complain(rc);
		}
	}
	return cached_uuid[which];
}

static char *
uuid_to_string(const uuid_t *uuid)
{
	char	   *buf = palloc(UUID_LEN_STR + 1);
	void	   *ptr = buf;
	size_t		len = UUID_LEN_STR + 1;
	uuid_rc_t	rc;

	rc = uuid_export(uuid, UUID_FMT_STR, &ptr, &len);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);

	return buf;
}


static void
string_to_uuid(const char *str, uuid_t *uuid)
{
	uuid_rc_t	rc;

	rc = uuid_import(uuid, UUID_FMT_STR, str, UUID_LEN_STR + 1);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
}


static Datum
special_uuid_value(const char *name)
{
	uuid_t	   *uuid = get_cached_uuid_t(0);
	char	   *str;
	uuid_rc_t	rc;

	rc = uuid_load(uuid, name);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	str = uuid_to_string(uuid);

	return DirectFunctionCall1(uuid_in, CStringGetDatum(str));
}

/* len is unused with OSSP, but we want to have the same number of args
 *
 * OSSP 未使用 len，但我们希望拥有相同数量的参数
 */
static Datum
uuid_generate_internal(int mode, const uuid_t *ns, const char *name, int len)
{
	uuid_t	   *uuid = get_cached_uuid_t(0);
	char	   *str;
	uuid_rc_t	rc;

	rc = uuid_make(uuid, mode, ns, name);
	if (rc != UUID_RC_OK)
		pguuid_complain(rc);
	str = uuid_to_string(uuid);

	return DirectFunctionCall1(uuid_in, CStringGetDatum(str));
}


static Datum
uuid_generate_v35_internal(int mode, pg_uuid_t *ns, text *name)
{
	uuid_t	   *ns_uuid = get_cached_uuid_t(1);

	string_to_uuid(DatumGetCString(DirectFunctionCall1(uuid_out,
													   UUIDPGetDatum(ns))),
				   ns_uuid);

	return uuid_generate_internal(mode,
								  ns_uuid,
								  text_to_cstring(name),
								  0);
}

#else							/* !HAVE_UUID_OSSP */

static Datum
uuid_generate_internal(int v, unsigned char *ns, const char *ptr, int len)
{
	char		strbuf[40];

	switch (v)
	{
		case 0:					/* constant-value uuids */
			strlcpy(strbuf, ptr, 37);
			break;

		case 1:					/* time/node-based uuids */
			{
#ifdef HAVE_UUID_E2FS
				uuid_t		uu;

				uuid_generate_time(uu);
				uuid_unparse(uu, strbuf);

				/*
				 * PTR, if set, replaces the trailing characters of the uuid;
				 * this is to support v1mc, where a random multicast MAC is
				 * used instead of the physical one
				 *
				 * PTR，如果设置，将替换 uuid 的尾随字符；这是为了支持 v1mc，其中使用随机多播 MAC 而不是物理 MAC
				 */
				if (ptr && len <= 36)
					strcpy(strbuf + (36 - len), ptr);
#else							/* BSD */
				uuid_t		uu;
				uint32_t	status = uuid_s_ok;
				char	   *str = NULL;

				uuid_create(&uu, &status);

				if (status == uuid_s_ok)
				{
					uuid_to_string(&uu, &str, &status);
					if (status == uuid_s_ok)
					{
						strlcpy(strbuf, str, 37);

						/*
						 * In recent NetBSD, uuid_create() has started
						 * producing v4 instead of v1 UUIDs.  Check the
						 * version field and complain if it's not v1.
						 *
						 * 在最近的 NetBSD 中，uuid_create() 已开始生成 v4 而不是 v1 UUID。  检查版本字段，如果不是 v1 则抱怨。
						 */
						if (strbuf[14] != '1')
							ereport(ERROR,
									(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
							/* translator: %c will be a hex digit
							 *
							 * 译者：%c 将是一个十六进制数字
							 */
									 errmsg("uuid_create() produced a version %c UUID instead of the expected version 1",
											strbuf[14])));

						/*
						 * PTR, if set, replaces the trailing characters of
						 * the uuid; this is to support v1mc, where a random
						 * multicast MAC is used instead of the physical one
						 *
						 * PTR，如果设置，将替换 uuid 的尾随字符；这是为了支持 v1mc，其中使用随机多播 MAC 而不是物理 MAC
						 */
						if (ptr && len <= 36)
							strcpy(strbuf + (36 - len), ptr);
					}
					free(str);
				}

				if (status != uuid_s_ok)
					ereport(ERROR,
							(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
							 errmsg("uuid library failure: %d",
									(int) status)));
#endif
				break;
			}

		case 3:					/* namespace-based MD5 uuids */
		case 5:					/* namespace-based SHA1 uuids */
			{
				dce_uuid_t	uu;
#ifdef HAVE_UUID_BSD
				uint32_t	status = uuid_s_ok;
				char	   *str = NULL;
#endif

				if (v == 3)
				{
					pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_MD5);

					if (pg_cryptohash_init(ctx) < 0)
						elog(ERROR, "could not initialize %s context: %s", "MD5",
							 pg_cryptohash_error(ctx));
					if (pg_cryptohash_update(ctx, ns, sizeof(uu)) < 0 ||
						pg_cryptohash_update(ctx, (unsigned char *) ptr, len) < 0)
						elog(ERROR, "could not update %s context: %s", "MD5",
							 pg_cryptohash_error(ctx));
					/* we assume sizeof MD5 result is 16, same as UUID size
					 *
					 * 我们假设 MD5 结果的 size 是 16，与 UUID 大小相同
					 */
					if (pg_cryptohash_final(ctx, (unsigned char *) &uu,
											sizeof(uu)) < 0)
						elog(ERROR, "could not finalize %s context: %s", "MD5",
							 pg_cryptohash_error(ctx));
					pg_cryptohash_free(ctx);
				}
				else
				{
					pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA1);
					unsigned char sha1result[SHA1_DIGEST_LENGTH];

					if (pg_cryptohash_init(ctx) < 0)
						elog(ERROR, "could not initialize %s context: %s", "SHA1",
							 pg_cryptohash_error(ctx));
					if (pg_cryptohash_update(ctx, ns, sizeof(uu)) < 0 ||
						pg_cryptohash_update(ctx, (unsigned char *) ptr, len) < 0)
						elog(ERROR, "could not update %s context: %s", "SHA1",
							 pg_cryptohash_error(ctx));
					if (pg_cryptohash_final(ctx, sha1result, sizeof(sha1result)) < 0)
						elog(ERROR, "could not finalize %s context: %s", "SHA1",
							 pg_cryptohash_error(ctx));
					pg_cryptohash_free(ctx);

					memcpy(&uu, sha1result, sizeof(uu));
				}

				/* the calculated hash is using local order
				 *
				 * 计算的哈希值使用本地顺序
				 */
				UUID_TO_NETWORK(uu);
				UUID_V3_OR_V5(uu, v);

#ifdef HAVE_UUID_E2FS
				/* uuid_unparse expects local order
				 *
				 * uuid_unparse 需要本地顺序
				 */
				UUID_TO_LOCAL(uu);
				uuid_unparse((unsigned char *) &uu, strbuf);
#else							/* BSD */
				uuid_to_string(&uu, &str, &status);

				if (status == uuid_s_ok)
					strlcpy(strbuf, str, 37);

				free(str);

				if (status != uuid_s_ok)
					ereport(ERROR,
							(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
							 errmsg("uuid library failure: %d",
									(int) status)));
#endif
				break;
			}

		case 4:					/* random uuid */
		default:
			{
#ifdef HAVE_UUID_E2FS
				uuid_t		uu;

				uuid_generate_random(uu);
				uuid_unparse(uu, strbuf);
#else							/* BSD */
				snprintf(strbuf, sizeof(strbuf),
						 "%08lx-%04x-%04x-%04x-%04x%08lx",
						 (unsigned long) arc4random(),
						 (unsigned) (arc4random() & 0xffff),
						 (unsigned) ((arc4random() & 0xfff) | 0x4000),
						 (unsigned) ((arc4random() & 0x3fff) | 0x8000),
						 (unsigned) (arc4random() & 0xffff),
						 (unsigned long) arc4random());
#endif
				break;
			}
	}

	return DirectFunctionCall1(uuid_in, CStringGetDatum(strbuf));
}

#endif							/* HAVE_UUID_OSSP */


Datum
uuid_nil(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("nil");
#else
	return uuid_generate_internal(0, NULL,
								  "00000000-0000-0000-0000-000000000000", 36);
#endif
}


Datum
uuid_ns_dns(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:DNS");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b810-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_ns_url(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:URL");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b811-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_ns_oid(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:OID");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b812-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_ns_x500(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	return special_uuid_value("ns:X500");
#else
	return uuid_generate_internal(0, NULL,
								  "6ba7b814-9dad-11d1-80b4-00c04fd430c8", 36);
#endif
}


Datum
uuid_generate_v1(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V1, NULL, NULL, 0);
}


Datum
uuid_generate_v1mc(PG_FUNCTION_ARGS)
{
#ifdef HAVE_UUID_OSSP
	char	   *buf = NULL;
#elif defined(HAVE_UUID_E2FS)
	char		strbuf[40];
	char	   *buf;
	uuid_t		uu;

	uuid_generate_random(uu);

	/* set IEEE802 multicast and local-admin bits
	 *
	 * 设置 IEEE802 多播和本地管理位
	 */
	((dce_uuid_t *) &uu)->node[0] |= 0x03;

	uuid_unparse(uu, strbuf);
	buf = strbuf + 24;
#else							/* BSD */
	char		buf[16];

	/* set IEEE802 multicast and local-admin bits
	 *
	 * 设置 IEEE802 多播和本地管理位
	 */
	snprintf(buf, sizeof(buf), "-%04x%08lx",
			 (unsigned) ((arc4random() & 0xffff) | 0x0300),
			 (unsigned long) arc4random());
#endif

	return uuid_generate_internal(UUID_MAKE_V1 | UUID_MAKE_MC, NULL,
								  buf, 13);
}


Datum
uuid_generate_v3(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *ns = PG_GETARG_UUID_P(0);
	text	   *name = PG_GETARG_TEXT_PP(1);

#ifdef HAVE_UUID_OSSP
	return uuid_generate_v35_internal(UUID_MAKE_V3, ns, name);
#else
	return uuid_generate_internal(UUID_MAKE_V3, (unsigned char *) ns,
								  VARDATA_ANY(name), VARSIZE_ANY_EXHDR(name));
#endif
}


Datum
uuid_generate_v4(PG_FUNCTION_ARGS)
{
	return uuid_generate_internal(UUID_MAKE_V4, NULL, NULL, 0);
}


Datum
uuid_generate_v5(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *ns = PG_GETARG_UUID_P(0);
	text	   *name = PG_GETARG_TEXT_PP(1);

#ifdef HAVE_UUID_OSSP
	return uuid_generate_v35_internal(UUID_MAKE_V5, ns, name);
#else
	return uuid_generate_internal(UUID_MAKE_V5, (unsigned char *) ns,
								  VARDATA_ANY(name), VARSIZE_ANY_EXHDR(name));
#endif
}
