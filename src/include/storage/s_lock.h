/*-------------------------------------------------------------------------
 *
 * s_lock.h
 *	   Implementation of spinlocks.
 *
 *	NOTE: none of the macros in this file are intended to be called directly.
 *	Call them through the macros in spin.h.
 *
 *	The following hardware-dependent macros must be provided for each
 *	supported platform:
 *
 *	void S_INIT_LOCK(slock_t *lock)
 *		Initialize a spinlock (to the unlocked state).
 *
 *	int S_LOCK(slock_t *lock)
 *		Acquire a spinlock, waiting if necessary.
 *		Time out and abort() if unable to acquire the lock in a
 *		"reasonable" amount of time --- typically ~ 1 minute.
 *		Should return number of "delays"; see s_lock.c
 *
 *	void S_UNLOCK(slock_t *lock)
 *		Unlock a previously acquired lock.
 *
 *	bool S_LOCK_FREE(slock_t *lock)
 *		Tests if the lock is free. Returns true if free, false if locked.
 *		This does *not* change the state of the lock.
 *
 *	void SPIN_DELAY(void)
 *		Delay operation to occur inside spinlock wait loop.
 *
 *	Note to implementors: there are default implementations for all these
 *	macros at the bottom of the file.  Check if your platform can use
 *	these or needs to override them.
 *
 *  Usually, S_LOCK() is implemented in terms of even lower-level macros
 *	TAS() and TAS_SPIN():
 *
 *	int TAS(slock_t *lock)
 *		Atomic test-and-set instruction.  Attempt to acquire the lock,
 *		but do *not* wait.	Returns 0 if successful, nonzero if unable
 *		to acquire the lock.
 *
 *	int TAS_SPIN(slock_t *lock)
 *		Like TAS(), but this version is used when waiting for a lock
 *		previously found to be contended.  By default, this is the
 *		same as TAS(), but on some architectures it's better to poll a
 *		contended lock using an unlocked instruction and retry the
 *		atomic test-and-set only when it appears free.
 *
 *	TAS() and TAS_SPIN() are NOT part of the API, and should never be called
 *	directly.
 *
 *	CAUTION: on some platforms TAS() and/or TAS_SPIN() may sometimes report
 *	failure to acquire a lock even when the lock is not locked.  For example,
 *	on Alpha TAS() will "fail" if interrupted.  Therefore a retry loop must
 *	always be used, even if you are certain the lock is free.
 *
 *	It is the responsibility of these macros to make sure that the compiler
 *	does not re-order accesses to shared memory to precede the actual lock
 *	acquisition, or follow the lock release.  Prior to PostgreSQL 9.5, this
 *	was the caller's responsibility, which meant that callers had to use
 *	volatile-qualified pointers to refer to both the spinlock itself and the
 *	shared data being accessed within the spinlocked critical section.  This
 *	was notationally awkward, easy to forget (and thus error-prone), and
 *	prevented some useful compiler optimizations.  For these reasons, we
 *	now require that the macros themselves prevent compiler re-ordering,
 *	so that the caller doesn't need to take special precautions.
 *
 *	On platforms with weak memory ordering, the TAS(), TAS_SPIN(), and
 *	S_UNLOCK() macros must further include hardware-level memory fence
 *	instructions to prevent similar re-ordering at the hardware level.
 *	TAS() and TAS_SPIN() must guarantee that loads and stores issued after
 *	the macro are not executed until the lock has been obtained.  Conversely,
 *	S_UNLOCK() must guarantee that loads and stores issued before the macro
 *	have been executed before the lock is released.
 *
 *	On most supported platforms, TAS() uses a tas() function written
 *	in assembly language to execute a hardware atomic-test-and-set
 *	instruction.  Equivalent OS-supplied mutex routines could be used too.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/include/storage/s_lock.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * int S_LOCK(slock_t *lock)
 *	获取自旋锁；必要时等待。若在“合理”的时间（通常约 1 分钟）内无法获取，则超时并
 *	调用 abort()。应返回“延迟”次数；参见 s_lock.c。
 *
 * void S_UNLOCK(slock_t *lock)
 *	释放先前获取的锁。
 *
 * bool S_LOCK_FREE(slock_t *lock)
 *	测试锁是否空闲。空闲返回真，锁定返回假；不改变锁状态。
 *
 * void SPIN_DELAY(void)
 *	在自旋锁等待循环中执行的延迟操作。
 *
 * 实现者注意：文件底部提供了这些宏的默认实现。请检查平台能否使用它们，或是否需要覆盖。
 *
 * 通常，S_LOCK() 由更低层的 TAS() 与 TAS_SPIN() 宏实现：
 *
 * int TAS(slock_t *lock)
 *	原子测试并设置指令。尝试获取锁但不等待；成功返回 0，无法获取时返回非零。
 *
 * int TAS_SPIN(slock_t *lock)
 *	与 TAS() 相似，但用于等待已发现存在争用的锁。默认等同 TAS()；但部分架构更适合
 *	先用非锁定指令轮询有争用的锁，仅在看似空闲时重试原子测试并设置。
 *
 * TAS() 和 TAS_SPIN() 不是 API 的组成部分，绝不能直接调用。
 *
 * 注意：某些平台上 TAS() 和/或 TAS_SPIN() 即使锁未锁定也可能报告获取失败。例如 Alpha
 * 上 TAS() 在中断时会“失败”。因此即使确定锁空闲，也必须始终使用重试循环。
 *
 * 这些宏必须确保编译器不会将共享内存访问重排到实际获取锁之前或释放锁之后。PostgreSQL
 * 9.5 前此责任属于调用方，调用方必须通过 volatile 限定指针引用自旋锁和临界区内共享数据。
 * 该记法笨拙、易被遗漏并妨碍优化。因此现在要求宏自身阻止编译器重排，调用方无需特别防范。
 *
 * 在弱内存序平台上，TAS()、TAS_SPIN() 和 S_UNLOCK() 还必须包含硬件级内存屏障，防止
 * 硬件重排。TAS() 和 TAS_SPIN() 必须保证宏后的加载和存储直到获取锁后才执行；相反，
 * S_UNLOCK() 必须保证宏前的加载和存储在释放锁前已执行。
 *
 * 在多数受支持平台上，TAS() 使用汇编编写的 tas() 函数执行硬件原子测试并设置指令；也可
 * 使用等效的操作系统互斥例程。
 */
#ifndef S_LOCK_H
#define S_LOCK_H

#ifdef FRONTEND
#error "s_lock.h may not be included from frontend code"
#endif

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
/*************************************************************************
 * All the gcc inlines
 * Gcc consistently defines the CPU as __cpu__.
 * Other compilers use __cpu or __cpu__ so we test for both in those cases.
 */

/*
 * 所有 gcc 内联实现。
 * Gcc 始终将 CPU 定义为 __cpu__；其他编译器使用 __cpu 或 __cpu__，因此这些情形会测试两者。
 */

/*----------
 * Standard gcc asm format (assuming "volatile slock_t *lock"):

	__asm__ __volatile__(
		"	instruction	\n"
		"	instruction	\n"
		"	instruction	\n"
:		"=r"(_res), "+m"(*lock)		// return register, in/out lock value
:		"r"(lock)					// lock pointer, in input register
:		"memory", "cc");			// show clobbered registers here

 * The output-operands list (after first colon) should always include
 * "+m"(*lock), whether or not the asm code actually refers to this
 * operand directly.  This ensures that gcc believes the value in the
 * lock variable is used and set by the asm code.  Also, the clobbers
 * list (after third colon) should always include "memory"; this prevents
 * gcc from thinking it can cache the values of shared-memory fields
 * across the asm code.  Add "cc" if your asm code changes the condition
 * code register, and also list any temp registers the code uses.
 *----------
 */

/*----------
 * 标准 gcc 汇编格式（假定为“volatile slock_t *lock”）：
 *
 * 输出操作数列表（第一个冒号后）应始终包含 “+m”(*lock)，无论汇编代码是否直接引用它。
 * 这可确保 gcc 认为汇编代码使用并设置了锁变量值。破坏列表（第三个冒号后）也应始终包含
 * “memory”；它阻止 gcc 认为可跨越汇编代码缓存共享内存字段值。若汇编代码改变条件码寄存器，
 * 请添加 “cc”，并列出代码使用的临时寄存器。
 *----------
 */


#ifdef __i386__		/* 32-bit i386 */

/* 32 位 i386。 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

/*
 * Attempts to acquire an i386 spinlock with an atomic exchange.
 */

/*
 * 使用原子交换尝试获取 i386 自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res = 1;

	/*
	 * Use a non-locking test before asserting the bus lock.  Note that the
	 * extra test appears to be a small loss on some x86 platforms and a small
	 * win on others; it's by no means clear that we should keep it.
	 *
	 * When this was last tested, we didn't have separate TAS() and TAS_SPIN()
	 * macros.  Nowadays it probably would be better to do a non-locking test
	 * in TAS_SPIN() but not in TAS(), like on x86_64, but no-one's done the
	 * testing to verify that.  Without some empirical evidence, better to
	 * leave it alone.
	 */

	/*
	 * 在断言总线锁前先使用非锁定测试。额外测试在部分 x86 平台略有损失，在另一些平台略有
	 * 收益；是否应保留尚不明确。
	 *
	 * 上次测试时尚未分离 TAS() 与 TAS_SPIN()。如今可能更适合像 x86_64 一样仅在
	 * TAS_SPIN() 中使用非锁定测试，但尚无人验证。没有实证数据，最好保持不变。
	 */
	__asm__ __volatile__(
		"	cmpb	$0,%1	\n"
		"	jne		1f		\n"
		"	lock			\n"
		"	xchgb	%0,%1	\n"
		"1: \n"
:		"+q"(_res), "+m"(*lock)
:		/* no inputs */

		/* 无输入。 */
:		"memory", "cc");
	return (int) _res;
}

#define SPIN_DELAY() spin_delay()

/*
 * Delays an i386 spin loop using the PAUSE-equivalent instruction sequence.
 */

/*
 * 使用等效 PAUSE 指令序列延迟 i386 自旋循环。
 */
static __inline__ void
spin_delay(void)
{
	/*
	 * This sequence is equivalent to the PAUSE instruction ("rep" is
	 * ignored by old IA32 processors if the following instruction is
	 * not a string operation); the IA-32 Architecture Software
	 * Developer's Manual, Vol. 3, Section 7.7.2 describes why using
	 * PAUSE in the inner loop of a spin lock is necessary for good
	 * performance:
	 *
	 *     The PAUSE instruction improves the performance of IA-32
	 *     processors supporting Hyper-Threading Technology when
	 *     executing spin-wait loops and other routines where one
	 *     thread is accessing a shared lock or semaphore in a tight
	 *     polling loop. When executing a spin-wait loop, the
	 *     processor can suffer a severe performance penalty when
	 *     exiting the loop because it detects a possible memory order
	 *     violation and flushes the core processor's pipeline. The
	 *     PAUSE instruction provides a hint to the processor that the
	 *     code sequence is a spin-wait loop. The processor uses this
	 *     hint to avoid the memory order violation and prevent the
	 *     pipeline flush. In addition, the PAUSE instruction
	 *     de-pipelines the spin-wait loop to prevent it from
	 *     consuming execution resources excessively.
	 */

	/*
	 * 该序列等效于 PAUSE 指令（若后续指令不是字符串操作，旧 IA32 处理器会忽略 “rep”）。
	 * IA-32 架构软件开发手册第 3 卷第 7.7.2 节说明，为获得良好性能，必须在自旋锁内循环
	 * 中使用 PAUSE：它向处理器提示这是自旋等待循环，避免内存序违规和流水线刷新，并使循环
	 * 去流水线化，从而避免过度消耗执行资源。
	 */
	__asm__ __volatile__(
		" rep; nop			\n");
}

#endif	 /* __i386__ */


#ifdef __x86_64__		/* AMD Opteron, Intel EM64T */

/* AMD Opteron、Intel EM64T。 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

/*
 * On Intel EM64T, it's a win to use a non-locking test before the xchg proper,
 * but only when spinning.
 *
 * See also Implementing Scalable Atomic Locks for Multi-Core Intel(tm) EM64T
 * and IA32, by Michael Chynoweth and Mary R. Lee. As of this writing, it is
 * available at:
 * http://software.intel.com/en-us/articles/implementing-scalable-atomic-locks-for-multi-core-intel-em64t-and-ia32-architectures
 */
#define TAS_SPIN(lock)    (*(lock) ? 1 : TAS(lock))

/*
 * Attempts to acquire an x86-64 spinlock with an atomic exchange.
 */

/*
 * 使用原子交换尝试获取 x86-64 自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res = 1;

	__asm__ __volatile__(
		"	lock			\n"
		"	xchgb	%0,%1	\n"
:		"+q"(_res), "+m"(*lock)
:		/* no inputs */

		/* 无输入。 */
:		"memory", "cc");
	return (int) _res;
}

#define SPIN_DELAY() spin_delay()

/*
 * Delays an x86-64 spin loop with the PAUSE-equivalent instruction sequence.
 */

/*
 * 使用等效 PAUSE 指令序列延迟 x86-64 自旋循环。
 */
static __inline__ void
spin_delay(void)
{
	/*
	 * Adding a PAUSE in the spin delay loop is demonstrably a no-op on
	 * Opteron, but it may be of some use on EM64T, so we keep it.
	 */

	/*
	 * 在自旋延迟循环中添加 PAUSE 在 Opteron 上可证实没有作用，但在 EM64T 上可能有帮助，
	 * 因此保留它。
	 */
	__asm__ __volatile__(
		" rep; nop			\n");
}

#endif	 /* __x86_64__ */


/*
 * On ARM and ARM64, we use __sync_lock_test_and_set(int *, int) if available.
 *
 * We use the int-width variant of the builtin because it works on more chips
 * than other widths.
 */

/*
 * 在 ARM 和 ARM64 上，如可用则使用 __sync_lock_test_and_set(int *, int)。
 *
 * 使用该内建函数的 int 宽度变体，因为它比其他宽度适用于更多芯片。
 */
#if defined(__arm__) || defined(__arm) || defined(__aarch64__)
#ifdef HAVE_GCC__SYNC_INT32_TAS
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef int slock_t;

/*
 * Acquires an ARM spinlock through the compiler's atomic test-and-set builtin.
 */

/*
 * 通过编译器的原子测试并设置内建函数获取 ARM 自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#if defined(__aarch64__)

/*
 * On ARM64, it's a win to use a non-locking test before the TAS proper.  It
 * may be a win on 32-bit ARM, too, but nobody's tested it yet.
 */

/*
 * 在 ARM64 上，在真正执行 TAS 前使用非锁定测试有收益。在 32 位 ARM 上也可能有收益，
 * 但尚无人测试。
 */
#define TAS_SPIN(lock)	(*(lock) ? 1 : TAS(lock))

#define SPIN_DELAY() spin_delay()

/*
 * Delays an ARM64 spin loop with an instruction-synchronization barrier.
 */

/*
 * 使用指令同步屏障延迟 ARM64 自旋循环。
 */
static __inline__ void
spin_delay(void)
{
	/*
	 * Using an ISB instruction to delay in spinlock loops appears beneficial
	 * on high-core-count ARM64 processors.  It seems mostly a wash for smaller
	 * gear, and ISB doesn't exist at all on pre-v7 ARM chips.
	 */

	/*
	 * 在自旋锁循环中使用 ISB 指令似乎有利于高核心数 ARM64 处理器。对较小配置基本无差别，
	 * 且 v7 前 ARM 芯片根本没有 ISB。
	 */
	__asm__ __volatile__(
		" isb;				\n");
}

#endif	 /* __aarch64__ */
#endif	 /* HAVE_GCC__SYNC_INT32_TAS */
#endif	 /* __arm__ || __arm || __aarch64__ */


/* S/390 and S/390x Linux (32- and 64-bit zSeries) */

/* S/390 和 S/390x Linux（32 位和 64 位 zSeries）。 */
#if defined(__s390__) || defined(__s390x__)
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock)	   tas(lock)

/*
 * Attempts to acquire an S/390 spinlock using compare-and-swap.
 */

/*
 * 使用比较并交换尝试获取 S/390 自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	int			_res = 0;

	__asm__	__volatile__(
		"	cs 	%0,%3,0(%2)		\n"
:		"+d"(_res), "+m"(*lock)
:		"a"(lock), "d"(1)
:		"memory", "cc");
	return _res;
}

#endif	 /* __s390__ || __s390x__ */


#if defined(__sparc__)		/* Sparc */

/* Sparc。 */
/*
 * Solaris has always run sparc processors in TSO (total store) mode, but
 * linux didn't use to and the *BSDs still don't. So, be careful about
 * acquire/release semantics. The CPU will treat superfluous members as
 * NOPs, so it's just code space.
 */

/*
 * Solaris 始终以 TSO（全序存储）模式运行 sparc 处理器，但 Linux 过去并非如此，BSD 现在
 * 仍不是。因此需谨慎处理获取/释放语义。CPU 会将多余成员视为 NOP，因此代价只是代码空间。
 */
#define HAS_TEST_AND_SET

typedef unsigned char slock_t;

#define TAS(lock) tas(lock)

/*
 * Attempts to acquire a SPARC spinlock with the architecture's atomic primitive.
 */

/*
 * 使用该架构的原子原语尝试获取 SPARC 自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t		_res;

	/*
	 *	See comment in src/backend/port/tas/sunstudio_sparc.s for why this
	 *	uses "ldstub", and that file uses "cas".  gcc currently generates
	 *	sparcv7-targeted binaries, so "cas" use isn't possible.
	 */

	/*
	 * 参见 src/backend/port/tas/sunstudio_sparc.s 中的注释，了解此处为何使用 “ldstub”
	 * 而该文件使用 “cas”。gcc 当前生成面向 sparcv7 的二进制文件，因此不能使用 “cas”。
	 */
	__asm__ __volatile__(
		"	ldstub	[%2], %0	\n"
:		"=r"(_res), "+m"(*lock)
:		"r"(lock)
:		"memory");
#if defined(__sparcv7) || defined(__sparc_v7__)
	/*
	 * No stbar or membar available, luckily no actually produced hardware
	 * requires a barrier.
	 */

	/* 没有可用的 stbar 或 membar，幸运的是没有实际生产的硬件需要屏障。 */
#elif defined(__sparcv8) || defined(__sparc_v8__)
	/* stbar is available (and required for both PSO, RMO), membar isn't */

	/* stbar 可用（PSO 和 RMO 都需要），membar 不可用。 */
	__asm__ __volatile__ ("stbar	 \n":::"memory");
#else
	/*
	 * #LoadStore (RMO) | #LoadLoad (RMO) together are the appropriate acquire
	 * barrier for sparcv8+ upwards.
	 */

	/* #LoadStore (RMO) | #LoadLoad (RMO) 共同构成适用于 sparcv8+ 的获取屏障。 */
	__asm__ __volatile__ ("membar #LoadStore | #LoadLoad \n":::"memory");
#endif
	return (int) _res;
}

#if defined(__sparcv7) || defined(__sparc_v7__)
/*
 * No stbar or membar available, luckily no actually produced hardware
 * requires a barrier.  We fall through to the default gcc definition of
 * S_UNLOCK in this case.
 */

/*
 * 没有可用的 stbar 或 membar，幸运的是没有实际生产的硬件需要屏障。此情形会落入默认的
 * gcc S_UNLOCK 定义。
 */
#elif defined(__sparcv8) || defined(__sparc_v8__)
/* stbar is available (and required for both PSO, RMO), membar isn't */

/* stbar 可用（PSO 和 RMO 都需要），membar 不可用。 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("stbar	 \n":::"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)
#else
/*
 * #LoadStore (RMO) | #StoreStore (RMO, PSO) together are the appropriate
 * release barrier for sparcv8+ upwards.
 */

/* #LoadStore (RMO) | #StoreStore (RMO, PSO) 共同构成适用于 sparcv8+ 的释放屏障。 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("membar #LoadStore | #StoreStore \n":::"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)
#endif

#endif	 /* __sparc__ */


/* PowerPC */

/* PowerPC。 */
#if defined(__ppc__) || defined(__powerpc__) || defined(__ppc64__) || defined(__powerpc64__)
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

/* On PPC, it's a win to use a non-locking test before the lwarx */

/* 在 PPC 上，在 lwarx 前使用非锁定测试有收益。 */
#define TAS_SPIN(lock)	(*(lock) ? 1 : TAS(lock))

/*
 * The second operand of addi can hold a constant zero or a register number,
 * hence constraint "=&b" to avoid allocating r0.  "b" stands for "address
 * base register"; most operands having this register-or-zero property are
 * address bases, e.g. the second operand of lwax.
 *
 * NOTE: per the Enhanced PowerPC Architecture manual, v1.0 dated 7-May-2002,
 * an isync is a sufficient synchronization barrier after a lwarx/stwcx loop.
 * But if the spinlock is in ordinary memory, we can use lwsync instead for
 * better performance.
 */

/*
 * addi 的第二操作数可为常量零或寄存器编号，因此使用 “=&b” 约束避免分配 r0。“b”表示
 * “地址基寄存器”；具有寄存器或零属性的大多数操作数都是地址基址，例如 lwax 的第二操作数。
 *
 * 注意：根据 2002-05-07 的 Enhanced PowerPC Architecture 手册 v1.0，lwarx/stwcx
 * 循环后的 isync 是足够的同步屏障。但若自旋锁位于普通内存，可改用 lwsync 以提高性能。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	slock_t _t;
	int _res;

	__asm__ __volatile__(
"	lwarx   %0,0,%3,1	\n"
"	cmpwi   %0,0		\n"
"	bne     1f			\n"
"	addi    %0,%0,1		\n"
"	stwcx.  %0,0,%3		\n"
"	beq     2f			\n"
"1: \n"
"	li      %1,1		\n"
"	b       3f			\n"
"2: \n"
"	lwsync				\n"
"	li      %1,0		\n"
"3: \n"
:	"=&b"(_t), "=r"(_res), "+m"(*lock)
:	"r"(lock)
:	"memory", "cc");
	return _res;
}

/*
 * PowerPC S_UNLOCK is almost standard but requires a "sync" instruction.
 * But we can use lwsync instead for better performance.
 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__ ("	lwsync \n" ::: "memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* powerpc */


#if defined(__mips__) && !defined(__sgi)	/* non-SGI MIPS */
#define HAS_TEST_AND_SET

typedef unsigned int slock_t;

#define TAS(lock) tas(lock)

/*
 * Original MIPS-I processors lacked the LL/SC instructions, but if we are
 * so unfortunate as to be running on one of those, we expect that the kernel
 * will handle the illegal-instruction traps and emulate them for us.  On
 * anything newer (and really, MIPS-I is extinct) LL/SC is the only sane
 * choice because any other synchronization method must involve a kernel
 * call.  Unfortunately, many toolchains still default to MIPS-I as the
 * codegen target; if the symbol __mips shows that that's the case, we
 * have to force the assembler to accept LL/SC.
 *
 * R10000 and up processors require a separate SYNC, which has the same
 * issues as LL/SC.
 */

/*
 * 原始 MIPS-I 处理器没有 LL/SC 指令。若不幸运行于其上，预期内核会处理非法指令陷阱并
 * 模拟它们。对任何较新的处理器（实际上 MIPS-I 已淘汰），LL/SC 是唯一合理选择，因为任何
 * 其他同步方法都必须调用内核。不幸的是，许多工具链仍默认以 MIPS-I 为代码生成目标；若符号
 * __mips 表明如此，必须强制汇编器接受 LL/SC。
 *
 * R10000 及以上处理器需要单独 SYNC，它与 LL/SC 有相同问题。
 */
#if __mips < 2
#define MIPS_SET_MIPS2	"       .set mips2          \n"
#else
#define MIPS_SET_MIPS2
#endif

/*
 * Attempts to acquire a MIPS spinlock using LL/SC and a synchronization barrier.
 */

/*
 * 使用 LL/SC 和同步屏障尝试获取 MIPS 自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	volatile slock_t *_l = lock;
	int			_res;
	int			_tmp;

	__asm__ __volatile__(
		"       .set push           \n"
		MIPS_SET_MIPS2
		"       .set noreorder      \n"
		"       .set nomacro        \n"
		"       ll      %0, %2      \n"
		"       or      %1, %0, 1   \n"
		"       sc      %1, %2      \n"
		"       xori    %1, 1       \n"
		"       or      %0, %0, %1  \n"
		"       sync                \n"
		"       .set pop              "
:		"=&r" (_res), "=&r" (_tmp), "+R" (*_l)
:		/* no inputs */

		/* 无输入。 */
:		"memory");
	return _res;
}

/* MIPS S_UNLOCK is almost standard but requires a "sync" instruction */

/* MIPS S_UNLOCK 几乎为标准实现，但需要 “sync” 指令。 */
#define S_UNLOCK(lock)	\
do \
{ \
	__asm__ __volatile__( \
		"       .set push           \n" \
		MIPS_SET_MIPS2 \
		"       .set noreorder      \n" \
		"       .set nomacro        \n" \
		"       sync                \n" \
		"       .set pop              " \
:		/* no outputs */ \
:		/* no inputs */	\
:		"memory"); \
	*((volatile slock_t *) (lock)) = 0; \
} while (0)

#endif /* __mips__ && !__sgi */



/*
 * If we have no platform-specific knowledge, but we found that the compiler
 * provides __sync_lock_test_and_set(), use that.  Prefer the int-width
 * version over the char-width version if we have both, on the rather dubious
 * grounds that that's known to be more likely to work in the ARM ecosystem.
 * (But we dealt with ARM above.)
 */

/*
 * 若没有平台特定知识，但发现编译器提供 __sync_lock_test_and_set()，则使用它。若同时
 * 有 int 宽度和 char 宽度版本，基于其在 ARM 生态中更可能可用这一并不十分可靠的理由，
 * 优先选择 int 宽度版本。（ARM 已在上方处理。）
 */
#if !defined(HAS_TEST_AND_SET)

#if defined(HAVE_GCC__SYNC_INT32_TAS)
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef int slock_t;

/*
 * Acquires a generic integer-width spinlock with the compiler builtin.
 */

/*
 * 通过编译器内建函数获取通用 int 宽度自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#elif defined(HAVE_GCC__SYNC_CHAR_TAS)
#define HAS_TEST_AND_SET

#define TAS(lock) tas(lock)

typedef char slock_t;

/*
 * Acquires a generic character-width spinlock with the compiler builtin.
 */

/*
 * 通过编译器内建函数获取通用 char 宽度自旋锁。
 */
static __inline__ int
tas(volatile slock_t *lock)
{
	return __sync_lock_test_and_set(lock, 1);
}

#define S_UNLOCK(lock) __sync_lock_release(lock)

#endif	 /* HAVE_GCC__SYNC_INT32_TAS */

#endif	/* !defined(HAS_TEST_AND_SET) */


/*
 * Default implementation of S_UNLOCK() for gcc/icc.
 *
 * Note that this implementation is unsafe for any platform that can reorder
 * a memory access (either load or store) after a following store.  That
 * happens not to be possible on x86 and most legacy architectures (some are
 * single-processor!), but many modern systems have weaker memory ordering.
 * Those that do must define their own version of S_UNLOCK() rather than
 * relying on this one.
 */

/*
 * gcc/icc 的 S_UNLOCK() 默认实现。
 *
 * 对可将内存访问（加载或存储）重排到后续存储之后的平台，该实现不安全。x86 和大多数旧架构
 * 不会发生这种情况（有些甚至是单处理器），但许多现代系统内存序更弱。此类平台必须定义自己
 * 的 S_UNLOCK()，不能依赖该实现。
 */
#if !defined(S_UNLOCK)
#define S_UNLOCK(lock)	\
	do { __asm__ __volatile__("" : : : "memory");  *(lock) = 0; } while (0)
#endif

#endif	/* defined(__GNUC__) || defined(__INTEL_COMPILER) */


/*
 * ---------------------------------------------------------------------
 * Platforms that use non-gcc inline assembly:
 * ---------------------------------------------------------------------
 */

/*
 * ---------------------------------------------------------------------
 * 使用非 gcc 内联汇编的平台：
 * ---------------------------------------------------------------------
 */

#if !defined(HAS_TEST_AND_SET)	/* We didn't trigger above, let's try here */

/* These are in sunstudio_(sparc|x86).s */

/* 这些位于 sunstudio_(sparc|x86).s 中。 */

#if defined(__SUNPRO_C) && (defined(__i386) || defined(__x86_64__) || defined(__sparc__) || defined(__sparc))
#define HAS_TEST_AND_SET

#if defined(__i386) || defined(__x86_64__) || defined(__sparcv9) || defined(__sparcv8plus)
typedef unsigned int slock_t;
#else
typedef unsigned char slock_t;
#endif

extern slock_t pg_atomic_cas(volatile slock_t *lock, slock_t with,
									  slock_t cmp);

#define TAS(a) (pg_atomic_cas((a), 1, 0) != 0)
#endif


#ifdef _MSC_VER
typedef LONG slock_t;

#define HAS_TEST_AND_SET
#define TAS(lock) (InterlockedCompareExchange(lock, 1, 0))

#define SPIN_DELAY() spin_delay()

/* If using Visual C++ on Win64, inline assembly is unavailable.
 * Use a _mm_pause intrinsic instead of rep nop.
 */

/*
 * 在 Win64 上使用 Visual C++ 时，内联汇编不可用。请使用 _mm_pause 内建函数代替 rep nop。
 */
#if defined(_WIN64)
/*
 * Delays a Win64 spin loop using the _mm_pause intrinsic.
 */

/*
 * 使用 _mm_pause 内建函数延迟 Win64 自旋循环。
 */
static __forceinline void
spin_delay(void)
{
	_mm_pause();
}
#else
/*
 * Delays a 32-bit MSVC spin loop with the architecture's pause instruction.
 */

/*
 * 使用该架构的暂停指令延迟 32 位 MSVC 自旋循环。
 */
static __forceinline void
spin_delay(void)
{
	/* See comment for gcc code. Same code, MASM syntax */

	/* 参见 gcc 代码的注释。相同代码，采用 MASM 语法。 */
	__asm rep nop;
}
#endif

#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)

#define S_UNLOCK(lock)	\
	do { _ReadWriteBarrier(); (*(lock)) = 0; } while (0)

#endif


#endif	/* !defined(HAS_TEST_AND_SET) */


/* Blow up if we didn't have any way to do spinlocks */

/* 如果没有任何实现自旋锁的方法，则报错退出。 */

/* 如果没有任何实现自旋锁的方法，则报错退出。 */
#ifndef HAS_TEST_AND_SET
#error PostgreSQL does not have spinlock support on this platform.  Please report this to pgsql-bugs@lists.postgresql.org.
#endif


/*
 * Default Definitions - override these above as needed.
 */

/*
 * 默认定义——可按需要在上方覆盖。
 */

#if !defined(S_LOCK)
#define S_LOCK(lock) \
	(TAS(lock) ? s_lock((lock), __FILE__, __LINE__, __func__) : 0)
#endif	 /* S_LOCK */

#if !defined(S_LOCK_FREE)
#define S_LOCK_FREE(lock)	(*(lock) == 0)
#endif	 /* S_LOCK_FREE */

#if !defined(S_UNLOCK)
/*
 * Our default implementation of S_UNLOCK is essentially *(lock) = 0.  This
 * is unsafe if the platform can reorder a memory access (either load or
 * store) after a following store; platforms where this is possible must
 * define their own S_UNLOCK.  But CPU reordering is not the only concern:
 * if we simply defined S_UNLOCK() as an inline macro, the compiler might
 * reorder instructions from inside the critical section to occur after the
 * lock release.  Since the compiler probably can't know what the external
 * function s_unlock is doing, putting the same logic there should be adequate.
 * A sufficiently-smart globally optimizing compiler could break that
 * assumption, though, and the cost of a function call for every spinlock
 * release may hurt performance significantly, so we use this implementation
 * only for platforms where we don't know of a suitable intrinsic.  For the
 * most part, those are relatively obscure platform/compiler combinations to
 * which the PostgreSQL project does not have access.
 */

/*
 * S_UNLOCK 的默认实现本质上是 *(lock) = 0。若平台可将内存访问（加载或存储）重排到后续
 * 存储之后，它不安全；此类平台必须定义自己的 S_UNLOCK。但 CPU 重排不是唯一问题：若仅将
 * S_UNLOCK() 定义为内联宏，编译器可能将临界区内指令重排到锁释放之后。编译器很可能不知道
 * 外部函数 s_unlock 的行为，因此将相同逻辑置于该函数应当足够。不过，全局优化足够智能的
 * 编译器仍可能破坏这一假设；而每次自旋锁释放都进行函数调用可能显著损害性能，所以仅在未知
 * 合适内建函数的平台使用此实现。大体而言，这些是 PostgreSQL 项目无法访问的较冷门平台/
 * 编译器组合。
 */
#define USE_DEFAULT_S_UNLOCK
/*
 * Releases a spinlock through the out-of-line fallback implementation.
 */

/*
 * 通过非内联回退实现释放自旋锁。
 */
extern void s_unlock(volatile slock_t *lock);
#define S_UNLOCK(lock)		s_unlock(lock)
#endif	 /* S_UNLOCK */

#if !defined(S_INIT_LOCK)
#define S_INIT_LOCK(lock)	S_UNLOCK(lock)
#endif	 /* S_INIT_LOCK */

#if !defined(SPIN_DELAY)
#define SPIN_DELAY()	((void) 0)
#endif	 /* SPIN_DELAY */

#if !defined(TAS)
/*
 * Attempts a platform fallback atomic test-and-set operation.
 */

/*
 * 尝试执行平台回退的原子测试并设置操作。
 */
extern int	tas(volatile slock_t *lock);		/* in port/.../tas.s, or
												 * s_lock.c */

/* 位于 port/.../tas.s 或 s_lock.c 中。 */

#define TAS(lock)		tas(lock)
#endif	 /* TAS */

#if !defined(TAS_SPIN)
#define TAS_SPIN(lock)	TAS(lock)
#endif	 /* TAS_SPIN */


/*
 * Platform-independent out-of-line support routines
 */

/*
 * 平台无关的非内联支持例程。
 */
/*
 * Acquires a contended spinlock by retrying TAS with bounded delays.
 */

/*
 * 通过带上限延迟重试 TAS 来获取存在争用的自旋锁。
 */
extern int s_lock(volatile slock_t *lock, const char *file, int line, const char *func);

/* Support for dynamic adjustment of spins_per_delay */

/* 支持动态调整 spins_per_delay。 */
#define DEFAULT_SPINS_PER_DELAY  100

/*
 * Updates the shared spin-delay target used by spinlock contention loops.
 */

/*
 * 更新自旋锁争用循环使用的共享自旋延迟目标。
 */
extern void set_spins_per_delay(int shared_spins_per_delay);
/*
 * Recalculates and returns the spin-delay setting from shared feedback.
 */

/*
 * 根据共享反馈重新计算并返回自旋延迟设置。
 */
extern int	update_spins_per_delay(int shared_spins_per_delay);

/*
 * Support for spin delay which is useful in various places where
 * spinlock-like procedures take place.
 */

/*
 * 支持在多个执行类似自旋锁过程的位置使用的自旋延迟。
 */
typedef struct
{
	int			spins;
	int			delays;
	int			cur_delay;
	const char *file;
	int			line;
	const char *func;
} SpinDelayStatus;

/*
 * Initializes spin-delay counters and source-location context for one loop.
 */

/*
 * 为一个循环初始化自旋延迟计数器和源位置上下文。
 */
static inline void
init_spin_delay(SpinDelayStatus *status,
				const char *file, int line, const char *func)
{
	status->spins = 0;
	status->delays = 0;
	status->cur_delay = 0;
	status->file = file;
	status->line = line;
	status->func = func;
}

#define init_local_spin_delay(status) init_spin_delay(status, __FILE__, __LINE__, __func__)
/*
 * Performs one adaptive spin-delay iteration and updates its status.
 */

/*
 * 执行一次自适应自旋延迟迭代并更新其状态。
 */
extern void perform_spin_delay(SpinDelayStatus *status);
/*
 * Finishes a spin-delay sequence and records its outcome.
 */

/*
 * 完成自旋延迟序列并记录其结果。
 */
extern void finish_spin_delay(SpinDelayStatus *status);

#endif	 /* S_LOCK_H */
