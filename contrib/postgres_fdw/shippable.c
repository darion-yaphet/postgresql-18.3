/*-------------------------------------------------------------------------
 *
 * shippable.c
 *	  Determine which database objects are shippable to a remote server.
 *
 * We need to determine whether particular functions, operators, and indeed
 * data types are shippable to a remote server for execution --- that is,
 * do they exist and have the same behavior remotely as they do locally?
 * Built-in objects are generally considered shippable.  Other objects can
 * be shipped if they are declared as such by the user.
 *
 * Note: there are additional filter rules that prevent shipping mutable
 * functions or functions using nonportable collations.  Those considerations
 * need not be accounted for here.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/postgres_fdw/shippable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "catalog/dependency.h"
#include "postgres_fdw.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"

/* Hash table for caching the results of shippability lookups
 *
 * 用于缓存可发货性查找结果的哈希表
 */
static HTAB *ShippableCacheHash = NULL;

/*
 * Hash key for shippability lookups.  We include the FDW server OID because
 * decisions may differ per-server.  Otherwise, objects are identified by
 * their (local!) OID and catalog OID.
 *
 * 用于可发货性查找的哈希键。  我们包含 FDW 服务器 OID，因为每个服务器的决策可能有所不同。  否则，对象由它们的（本地！）OID 和目录 OID 来标识。
 */
typedef struct
{
	/* XXX we assume this struct contains no padding bytes
	 *
	 * XXX 我们假设该结构不包含填充字节
	 */
	Oid			objid;			/* function/operator/type OID */
	Oid			classid;		/* OID of its catalog (pg_proc, etc) */
	Oid			serverid;		/* FDW server we are concerned with */
} ShippableCacheKey;

typedef struct
{
	ShippableCacheKey key;		/* hash key - must be first */
	bool		shippable;
} ShippableCacheEntry;


/*
 * Flush cache entries when pg_foreign_server is updated.
 *
 * 当 pg_foreign_server 更新时刷新缓存条目。
 *
 * We do this because of the possibility of ALTER SERVER being used to change
 * a server's extensions option.  We do not currently bother to check whether
 * objects' extension membership changes once a shippability decision has been
 * made for them, however.
 *
 * 我们这样做是因为 ALTER SERVER 可能被用来更改服务器的扩展选项。  然而，一旦为对象做出了可交付性决定，我们目前不会检查对象的扩展成员身份是否发生变化。
 */
static void
InvalidateShippableCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	ShippableCacheEntry *entry;

	/*
	 * In principle we could flush only cache entries relating to the
	 * pg_foreign_server entry being outdated; but that would be more
	 * complicated, and it's probably not worth the trouble.  So for now, just
	 * flush all entries.
	 *
	 * 原则上我们只能刷新与过时的 pg_foreign_server 条目相关的缓存条目；但这会更复杂，而且可能不值得这么麻烦。  所以现在，只需刷新所有条目即可。
	 */
	hash_seq_init(&status, ShippableCacheHash);
	while ((entry = (ShippableCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(ShippableCacheHash,
						&entry->key,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

/*
 * Initialize the backend-lifespan cache of shippability decisions.
 *
 * 初始化可发货决策的后端生命周期缓存。
 */
static void
InitializeShippableCache(void)
{
	HASHCTL		ctl;

	/* Create the hash table.
	 *
	 * 创建哈希表。
	 */
	ctl.keysize = sizeof(ShippableCacheKey);
	ctl.entrysize = sizeof(ShippableCacheEntry);
	ShippableCacheHash =
		hash_create("Shippability cache", 256, &ctl, HASH_ELEM | HASH_BLOBS);

	/* Set up invalidation callback on pg_foreign_server.
	 *
	 * 在 pg_foreign_server 上设置失效回调。
	 */
	CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
								  InvalidateShippableCacheCallback,
								  (Datum) 0);
}

/*
 * Returns true if given object (operator/function/type) is shippable
 * according to the server options.
 *
 * 如果给定对象（运算符/函数/类型）可根据服务器选项传送，则返回 true。
 *
 * Right now "shippability" is exclusively a function of whether the object
 * belongs to an extension declared by the user.  In the future we could
 * additionally have a list of functions/operators declared one at a time.
 *
 * 现在，“可交付性”完全取决于对象是否属于用户声明的扩展。  将来我们还可以额外声明一次一个函数/运算符列表。
 */
static bool
lookup_shippable(Oid objectId, Oid classId, PgFdwRelationInfo *fpinfo)
{
	Oid			extensionOid;

	/*
	 * Is object a member of some extension?  (Note: this is a fairly
	 * expensive lookup, which is why we try to cache the results.)
	 *
	 * 对象是某个扩展的成员吗？  （注意：这是一个相当昂贵的查找，这就是我们尝试缓存结果的原因。）
	 */
	extensionOid = getExtensionOfObject(classId, objectId);

	/* If so, is that extension in fpinfo->shippable_extensions?
	 *
	 * 如果是这样，该扩展名是否在 fpinfo->shippable_extensions 中？
	 */
	if (OidIsValid(extensionOid) &&
		list_member_oid(fpinfo->shippable_extensions, extensionOid))
		return true;

	return false;
}

/*
 * Return true if given object is one of PostgreSQL's built-in objects.
 *
 * 如果给定对象是 PostgreSQL 的内置对象之一，则返回 true。
 *
 * We use FirstGenbkiObjectId as the cutoff, so that we only consider
 * objects with hand-assigned OIDs to be "built in", not for instance any
 * function or type defined in the information_schema.
 *
 * 我们使用 FirstGenbkiObjectId 作为截止点，以便我们仅将具有手动分配的 OID 的对象视为“内置”，而不是 information_schema 中定义的任何函数或类型。
 *
 * Our constraints for dealing with types are tighter than they are for
 * functions or operators: we want to accept only types that are in pg_catalog,
 * else deparse_type_name might incorrectly fail to schema-qualify their names.
 * Thus we must exclude information_schema types.
 *
 * 我们处理类型的约束比函数或运算符更严格：我们只想接受 pg_catalog 中的类型，否则 deparse_type_name 可能会错误地无法对它们的名称进行模式限定。因此我们必须排除 information_schema 类型。
 *
 * XXX there is a problem with this, which is that the set of built-in
 * objects expands over time.  Something that is built-in to us might not
 * be known to the remote server, if it's of an older version.  But keeping
 * track of that would be a huge exercise.
 *
 * XXX 这样做有一个问题，即内置对象集会随着时间的推移而扩展。  如果我们内置的东西是旧版本，远程服务器可能不知道它。  但跟踪这一点将是一项巨大的工作。
 */
bool
is_builtin(Oid objectId)
{
	return (objectId < FirstGenbkiObjectId);
}

/*
 * is_shippable
 *	   Is this object (function/operator/type) shippable to foreign server?
 *
 * is_shippable 该对象（函数/运算符/类型）是否可以运送到外部服务器？
 */
bool
is_shippable(Oid objectId, Oid classId, PgFdwRelationInfo *fpinfo)
{
	ShippableCacheKey key;
	ShippableCacheEntry *entry;

	/* Built-in objects are presumed shippable.
	 *
	 * 内置对象被假定为可交付的。
	 */
	if (is_builtin(objectId))
		return true;

	/* Otherwise, give up if user hasn't specified any shippable extensions.
	 *
	 * 否则，如果用户未指定任何可交付的扩展，则放弃。
	 */
	if (fpinfo->shippable_extensions == NIL)
		return false;

	/* Initialize cache if first time through.
	 *
	 * 如果第一次通过则初始化缓存。
	 */
	if (!ShippableCacheHash)
		InitializeShippableCache();

	/* Set up cache hash key
	 *
	 * 设置缓存哈希键
	 */
	key.objid = objectId;
	key.classid = classId;
	key.serverid = fpinfo->server->serverid;

	/* See if we already cached the result.
	 *
	 * 看看我们是否已经缓存了结果。
	 */
	entry = (ShippableCacheEntry *)
		hash_search(ShippableCacheHash, &key, HASH_FIND, NULL);

	if (!entry)
	{
		/* Not found in cache, so perform shippability lookup.
		 *
		 * 在缓存中找不到，因此请执行可发货性查找。
		 */
		bool		shippable = lookup_shippable(objectId, classId, fpinfo);

		/*
		 * Don't create a new hash entry until *after* we have the shippable
		 * result in hand, as the underlying catalog lookups might trigger a
		 * cache invalidation.
		 *
		 * 在我们获得可交付结果之前，不要创建新的哈希条目，因为底层目录查找可能会触发缓存失效。
		 */
		entry = (ShippableCacheEntry *)
			hash_search(ShippableCacheHash, &key, HASH_ENTER, NULL);

		entry->shippable = shippable;
	}

	return entry->shippable;
}
