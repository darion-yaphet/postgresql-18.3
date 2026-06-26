/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/uavc.c
 *
 * Implementation of userspace access vector cache; that enables to cache
 * access control decisions recently used, and reduce number of kernel
 * invocations to avoid unnecessary performance hit.
 *
 * Copyright (c) 2011-2025, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "commands/seclabel.h"
#include "common/hashfn.h"
#include "sepgsql.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/memutils.h"

/*
 * avc_cache
 *
 * It enables to cache access control decision (and behavior on execution of
 * trusted procedure, db_procedure class only) for a particular pair of
 * security labels and object class in userspace.
 *
 * 它能够缓存用户空间中一对特定的安全标签和对象类的访问控制决策（以及执行受信任过程的行为，仅 db_procedure 类）。
 */
typedef struct
{
	uint32		hash;			/* hash value of this cache entry */
	char	   *scontext;		/* security context of the subject */
	char	   *tcontext;		/* security context of the target */
	uint16		tclass;			/* object class of the target */

	uint32		allowed;		/* permissions to be allowed */
	uint32		auditallow;		/* permissions to be audited on allowed */
	uint32		auditdeny;		/* permissions to be audited on denied */

	bool		permissive;		/* true, if permissive rule */
	bool		hot_cache;		/* true, if recently referenced */
	bool		tcontext_is_valid;
	/* true, if tcontext is valid
	 *
	 * true，如果 tcontext 有效
	 */
	char	   *ncontext;		/* temporary scontext on execution of trusted
								 * procedure, or NULL elsewhere */
} avc_cache;

/*
 * Declaration of static variables
 *
 * 静态变量的声明
 */
#define AVC_NUM_SLOTS		512
#define AVC_NUM_RECLAIM		16
#define AVC_DEF_THRESHOLD	384

static MemoryContext avc_mem_cxt;
static List *avc_slots[AVC_NUM_SLOTS];	/* avc's hash buckets */
static int	avc_num_caches;		/* number of caches currently used */
static int	avc_lru_hint;		/* index of the buckets to be reclaimed next */
static int	avc_threshold;		/* threshold to launch cache-reclaiming  */
static char *avc_unlabeled;		/* system 'unlabeled' label */

/*
 * Hash function
 *
 * 哈希函数
 */
static uint32
sepgsql_avc_hash(const char *scontext, const char *tcontext, uint16 tclass)
{
	return hash_any((const unsigned char *) scontext, strlen(scontext))
		^ hash_any((const unsigned char *) tcontext, strlen(tcontext))
		^ tclass;
}

/*
 * Reset all the avc caches
 *
 * 重置所有 AVC 缓存
 */
static void
sepgsql_avc_reset(void)
{
	MemoryContextReset(avc_mem_cxt);

	memset(avc_slots, 0, sizeof(List *) * AVC_NUM_SLOTS);
	avc_num_caches = 0;
	avc_lru_hint = 0;
	avc_unlabeled = NULL;
}

/*
 * Reclaim caches recently unreferenced
 *
 * 回收最近未引用的缓存
 */
static void
sepgsql_avc_reclaim(void)
{
	ListCell   *cell;
	int			index;

	while (avc_num_caches >= avc_threshold - AVC_NUM_RECLAIM)
	{
		index = avc_lru_hint;

		foreach(cell, avc_slots[index])
		{
			avc_cache  *cache = lfirst(cell);

			if (!cache->hot_cache)
			{
				avc_slots[index]
					= foreach_delete_current(avc_slots[index], cell);

				pfree(cache->scontext);
				pfree(cache->tcontext);
				if (cache->ncontext)
					pfree(cache->ncontext);
				pfree(cache);

				avc_num_caches--;
			}
			else
			{
				cache->hot_cache = false;
			}
		}
		avc_lru_hint = (avc_lru_hint + 1) % AVC_NUM_SLOTS;
	}
}

/* -------------------------------------------------------------------------
 *
 * sepgsql_avc_check_valid
 *
 * This function checks whether the cached entries are still valid.  If
 * the security policy has been reloaded (or any other events that requires
 * resetting userspace caches has occurred) since the last reference to
 * the access vector cache, we must flush the cache.
 *
 * 该函数检查缓存的条目是否仍然有效。  If the security policy has been reloaded (or any other events that requires resetting userspace caches has occurred) since the last reference to the access vector cache, we must flush the cache.
 *
 * Access control decisions must be atomic, but multiple system calls may
 * be required to make a decision; thus, when referencing the access vector
 * cache, we must loop until we complete without an intervening cache flush
 * event.  In practice, looping even once should be very rare.  Callers should
 * do something like this:
 *
 * Access control decisions must be atomic, but multiple system calls may be required to make a decision;因此，当引用访问向量高速缓存时，我们必须循环，直到在没有中间高速缓存刷新事件的情况下完成为止。  In practice, looping even once should be very rare.  调用者应该这样做：
 *
 *	 sepgsql_avc_check_valid();
 *	 do {
 *			 :
 *		 <reference to uavc>
 *			 :
 *	 } while (!sepgsql_avc_check_valid())
 *
 * sepgsql_avc_check_valid(); do { : <uavc 参考> : } while (!sepgsql_avc_check_valid())
 *
 * -------------------------------------------------------------------------
 */
static bool
sepgsql_avc_check_valid(void)
{
	if (selinux_status_updated() > 0)
	{
		sepgsql_avc_reset();

		return false;
	}
	return true;
}

/*
 * sepgsql_avc_unlabeled
 *
 * Returns an alternative label to be applied when no label or an invalid
 * label would otherwise be assigned.
 *
 * 当没有标签或无效标签被分配时，返回要应用的替代标签。
 */
static char *
sepgsql_avc_unlabeled(void)
{
	if (!avc_unlabeled)
	{
		char	   *unlabeled;

		if (security_get_initial_context_raw("unlabeled", &unlabeled) < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SELinux: failed to get initial security label: %m")));
		PG_TRY();
		{
			avc_unlabeled = MemoryContextStrdup(avc_mem_cxt, unlabeled);
		}
		PG_FINALLY();
		{
			freecon(unlabeled);
		}
		PG_END_TRY();
	}
	return avc_unlabeled;
}

/*
 * sepgsql_avc_compute
 *
 * A fallback path, when cache mishit. It asks SELinux its access control
 * decision for the supplied pair of security context and object class.
 *
 * 缓存未命中时的后备路径。它询问 SELinux 对所提供的安全上下文和对象类对的访问控制决策。
 */
static avc_cache *
sepgsql_avc_compute(const char *scontext, const char *tcontext, uint16 tclass)
{
	char	   *ucontext = NULL;
	char	   *ncontext = NULL;
	MemoryContext oldctx;
	avc_cache  *cache;
	uint32		hash;
	int			index;
	struct av_decision avd;

	hash = sepgsql_avc_hash(scontext, tcontext, tclass);
	index = hash % AVC_NUM_SLOTS;

	/*
	 * Validation check of the supplied security context. Because it always
	 * invoke system-call, frequent check should be avoided. Unless security
	 * policy is reloaded, validation status shall be kept, so we also cache
	 * whether the supplied security context was valid, or not.
	 *
	 * 对所提供的安全上下文进行验证检查。由于它总是调用系统调用，因此应避免频繁检查。除非重新加载安全策略，否则应保留验证状态，因此我们还缓存提供的安全上下文是否有效。
	 */
	if (security_check_context_raw(tcontext) != 0)
		ucontext = sepgsql_avc_unlabeled();

	/*
	 * Ask SELinux its access control decision
	 *
	 * 询问 SELinux 的访问控制决策
	 */
	if (!ucontext)
		sepgsql_compute_avd(scontext, tcontext, tclass, &avd);
	else
		sepgsql_compute_avd(scontext, ucontext, tclass, &avd);

	/*
	 * It also caches a security label to be switched when a client labeled as
	 * 'scontext' executes a procedure labeled as 'tcontext', not only access
	 * control decision on the procedure. The security label to be switched
	 * shall be computed uniquely on a pair of 'scontext' and 'tcontext',
	 * thus, it is reasonable to cache the new label on avc, and enables to
	 * reduce unnecessary system calls. It shall be referenced at
	 * sepgsql_needs_fmgr_hook to check whether the supplied function is a
	 * trusted procedure, or not.
	 *
	 * 当标记为“scontext”的客户端执行标记为“tcontext”的过程时，它还缓存要切换的安全标签，而不仅仅是该过程的访问控制决策。待切换的安全标签应在一对“scontext”和“tcontext”上唯一计算，因此，将新标签缓存在avc上是合理的，并且能够减少不必要的系统调用。 It shall be referenced at sepgsql_needs_fmgr_hook to check whether the supplied function is a trusted procedure, or not.
	 */
	if (tclass == SEPG_CLASS_DB_PROCEDURE)
	{
		if (!ucontext)
			ncontext = sepgsql_compute_create(scontext, tcontext,
											  SEPG_CLASS_PROCESS, NULL);
		else
			ncontext = sepgsql_compute_create(scontext, ucontext,
											  SEPG_CLASS_PROCESS, NULL);
		if (strcmp(scontext, ncontext) == 0)
		{
			pfree(ncontext);
			ncontext = NULL;
		}
	}

	/*
	 * Set up an avc_cache object
	 *
	 * 设置 avc_cache 对象
	 */
	oldctx = MemoryContextSwitchTo(avc_mem_cxt);

	cache = palloc0(sizeof(avc_cache));

	cache->hash = hash;
	cache->scontext = pstrdup(scontext);
	cache->tcontext = pstrdup(tcontext);
	cache->tclass = tclass;

	cache->allowed = avd.allowed;
	cache->auditallow = avd.auditallow;
	cache->auditdeny = avd.auditdeny;
	cache->hot_cache = true;
	if (avd.flags & SELINUX_AVD_FLAGS_PERMISSIVE)
		cache->permissive = true;
	if (!ucontext)
		cache->tcontext_is_valid = true;
	if (ncontext)
		cache->ncontext = pstrdup(ncontext);

	avc_num_caches++;

	if (avc_num_caches > avc_threshold)
		sepgsql_avc_reclaim();

	avc_slots[index] = lcons(cache, avc_slots[index]);

	MemoryContextSwitchTo(oldctx);

	return cache;
}

/*
 * sepgsql_avc_lookup
 *
 * Look up a cache entry that matches the supplied security contexts and
 * object class.  If not found, create a new cache entry.
 *
 * Look up a cache entry that matches the supplied security contexts and object class.  如果没有找到，则创建一个新的缓存条目。
 */
static avc_cache *
sepgsql_avc_lookup(const char *scontext, const char *tcontext, uint16 tclass)
{
	avc_cache  *cache;
	ListCell   *cell;
	uint32		hash;
	int			index;

	hash = sepgsql_avc_hash(scontext, tcontext, tclass);
	index = hash % AVC_NUM_SLOTS;

	foreach(cell, avc_slots[index])
	{
		cache = lfirst(cell);

		if (cache->hash == hash &&
			cache->tclass == tclass &&
			strcmp(cache->tcontext, tcontext) == 0 &&
			strcmp(cache->scontext, scontext) == 0)
		{
			cache->hot_cache = true;
			return cache;
		}
	}
	/* not found, so insert a new cache
	 *
	 * 没有找到，所以插入一个新的缓存
	 */
	return sepgsql_avc_compute(scontext, tcontext, tclass);
}

/*
 * sepgsql_avc_check_perms(_label)
 *
 * It returns 'true', if the security policy suggested to allow the required
 * permissions. Otherwise, it returns 'false' or raises an error according
 * to the 'abort_on_violation' argument.
 * The 'tobject' and 'tclass' identify the target object being referenced,
 * and 'required' is a bitmask of permissions (SEPG_*__*) defined for each
 * object classes.
 * The 'audit_name' is the object name (optional). If SEPGSQL_AVC_NOAUDIT
 * was supplied, it means to skip all the audit messages.
 *
 * It returns 'true', if the security policy suggested to allow the required permissions. Otherwise, it returns 'false' or raises an error according to the 'abort_on_violation' argument. The 'tobject' and 'tclass' identify the target object being referenced, and 'required' is a bitmask of permissions (SEPG_*__*) defined for each object classes. The 'audit_name' is the object name (optional).如果提供了 SEPGSQL_AVC_NOAUDIT，则意味着跳过所有审核消息。
 */
bool
sepgsql_avc_check_perms_label(const char *tcontext,
							  uint16 tclass, uint32 required,
							  const char *audit_name,
							  bool abort_on_violation)
{
	char	   *scontext = sepgsql_get_client_label();
	avc_cache  *cache;
	uint32		denied;
	uint32		audited;
	bool		result;

	sepgsql_avc_check_valid();
	do
	{
		result = true;

		/*
		 * If the target object is unlabeled, we perform the check using the
		 * label supplied by sepgsql_avc_unlabeled().
		 *
		 * 如果目标对象未标记，我们将使用 sepgsql_avc_unlabeled() 提供的标签执行检查。
		 */
		if (tcontext)
			cache = sepgsql_avc_lookup(scontext, tcontext, tclass);
		else
			cache = sepgsql_avc_lookup(scontext,
									   sepgsql_avc_unlabeled(), tclass);

		denied = required & ~cache->allowed;

		/*
		 * Compute permissions to be audited
		 *
		 * 计算待审核权限
		 */
		if (sepgsql_get_debug_audit())
			audited = (denied ? (denied & ~0) : (required & ~0));
		else
			audited = denied ? (denied & cache->auditdeny)
				: (required & cache->auditallow);

		if (denied)
		{
			/*
			 * In permissive mode or permissive domain, violated permissions
			 * shall be audited to the log files at once, and then implicitly
			 * allowed to avoid a flood of access denied logs, because the
			 * purpose of permissive mode/domain is to collect a violation log
			 * that will make it possible to fix up the security policy.
			 *
			 * 在许可模式或许可域中，违反权限应立即审核到日志文件，然后隐式允许以避免大量访问拒绝日志，因为许可模式/域的目的是收集违规日志，从而可以修复安全策略。
			 */
			if (!sepgsql_getenforce() || cache->permissive)
				cache->allowed |= required;
			else
				result = false;
		}
	} while (!sepgsql_avc_check_valid());

	/*
	 * In the case when we have something auditable actions here,
	 * sepgsql_audit_log shall be called with text representation of security
	 * labels for both of subject and object. It records this access
	 * violation, so DBA will be able to find out unexpected security problems
	 * later.
	 *
	 * 如果我们在这里有一些可审计的操作，则应使用主题和客体安全标签的文本表示来调用 sepgsql_audit_log。 It records this access violation, so DBA will be able to find out unexpected security problems later.
	 */
	if (audited != 0 &&
		audit_name != SEPGSQL_AVC_NOAUDIT &&
		sepgsql_get_mode() != SEPGSQL_MODE_INTERNAL)
	{
		sepgsql_audit_log(denied != 0,
						  (sepgsql_getenforce() && !cache->permissive),
						  cache->scontext,
						  cache->tcontext_is_valid ?
						  cache->tcontext : sepgsql_avc_unlabeled(),
						  cache->tclass,
						  audited,
						  audit_name);
	}

	if (abort_on_violation && !result)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("SELinux: security policy violation")));

	return result;
}

bool
sepgsql_avc_check_perms(const ObjectAddress *tobject,
						uint16 tclass, uint32 required,
						const char *audit_name,
						bool abort_on_violation)
{
	char	   *tcontext = GetSecurityLabel(tobject, SEPGSQL_LABEL_TAG);
	bool		rc;

	rc = sepgsql_avc_check_perms_label(tcontext,
									   tclass, required,
									   audit_name, abort_on_violation);
	if (tcontext)
		pfree(tcontext);

	return rc;
}

/*
 * sepgsql_avc_trusted_proc
 *
 * If the supplied function OID is configured as a trusted procedure, this
 * function will return a security label to be used during the execution of
 * that function.  Otherwise, it returns NULL.
 *
 * 如果提供的函数 OID 配置为可信过程，则该函数将返回一个在该函数执行期间使用的安全标签。  否则，返回 NULL。
 */
char *
sepgsql_avc_trusted_proc(Oid functionId)
{
	char	   *scontext = sepgsql_get_client_label();
	char	   *tcontext;
	ObjectAddress tobject;
	avc_cache  *cache;

	tobject.classId = ProcedureRelationId;
	tobject.objectId = functionId;
	tobject.objectSubId = 0;
	tcontext = GetSecurityLabel(&tobject, SEPGSQL_LABEL_TAG);

	sepgsql_avc_check_valid();
	do
	{
		if (tcontext)
			cache = sepgsql_avc_lookup(scontext, tcontext,
									   SEPG_CLASS_DB_PROCEDURE);
		else
			cache = sepgsql_avc_lookup(scontext, sepgsql_avc_unlabeled(),
									   SEPG_CLASS_DB_PROCEDURE);
	} while (!sepgsql_avc_check_valid());

	return cache->ncontext;
}

/*
 * sepgsql_avc_exit
 *
 * Clean up userspace AVC on process exit.
 *
 * 在进程退出时清理用户空间 AVC。
 */
static void
sepgsql_avc_exit(int code, Datum arg)
{
	selinux_status_close();
}

/*
 * sepgsql_avc_init
 *
 * Initialize the userspace AVC.  This should be called from _PG_init.
 *
 * 初始化用户空间 AVC。  这应该从 _PG_init 调用。
 */
void
sepgsql_avc_init(void)
{
	int			rc;

	/*
	 * All the avc stuff shall be allocated in avc_mem_cxt
	 *
	 * 所有 avc 内容均应分配在 avc_mem_cxt 中
	 */
	avc_mem_cxt = AllocSetContextCreate(TopMemoryContext,
										"userspace access vector cache",
										ALLOCSET_DEFAULT_SIZES);
	memset(avc_slots, 0, sizeof(avc_slots));
	avc_num_caches = 0;
	avc_lru_hint = 0;
	avc_threshold = AVC_DEF_THRESHOLD;

	/*
	 * SELinux allows to mmap(2) its kernel status page in read-only mode to
	 * inform userspace applications its status updating (such as policy
	 * reloading) without system-call invocations. This feature is only
	 * supported in Linux-2.6.38 or later, however, libselinux provides a
	 * fallback mode to know its status using netlink sockets.
	 *
	 * SELinux 允许以只读模式 mmap(2) 其内核状态页，以通知用户空间应用程序其状态更新（例如策略重新加载），而无需系统调用调用。此功能仅在 Linux-2.6.38 或更高版本中受支持，但是，libselinux 提供了后备模式以使用 netlink 套接字了解其状态。
	 */
	rc = selinux_status_open(1);
	if (rc < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux: could not open selinux status : %m")));
	else if (rc > 0)
		ereport(LOG,
				(errmsg("SELinux: kernel status page uses fallback mode")));

	/* Arrange to close selinux status page on process exit.
	 *
	 * 安排在进程退出时关闭 selinux 状态页面。
	 */
	on_proc_exit(sepgsql_avc_exit, 0);
}
