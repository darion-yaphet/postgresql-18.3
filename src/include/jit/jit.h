/*-------------------------------------------------------------------------
 * jit.h
 *	  Provider independent JIT infrastructure.
 *	  独立于提供者的 JIT 基础设施。
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * src/include/jit/jit.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * Core Flow Explanation:
 * PostgreSQL's JIT infrastructure is designed to be provider-independent, allowing 
 * different JIT backends (like LLVM) to be used. The core process involves:
 * 1. Initializing the JIT provider through _PG_jit_provider_init.
 * 2. Deciding whether to JIT based on the estimated cost of the query (controlled by GUCs).
 * 3. Compiling expressions via jit_compile_expr when appropriate.
 * 4. Cleaning up resources after execution or errors via jit_release_context and jit_reset_after_error.
 * 5. Collecting performance metrics using JitInstrumentation.
 *
 * 核心流程说明：
 * PostgreSQL 的 JIT 基础设施设计为独立于提供者，允许使用不同的 JIT 后端（如 LLVM）。
 * 核心流程包括：
 * 1. 通过 _PG_jit_provider_init 初始化 JIT 提供者。
 * 2. 根据查询的估计代价决定是否进行 JIT（由 GUC 参数控制）。
 * 3. 在适当时通过 jit_compile_expr 编译表达式。
 * 4. 在执行完成或发生错误后，通过 jit_release_context 和 jit_reset_after_error 清理资源。
 * 5. 使用 JitInstrumentation 收集性能指标。
 */

#ifndef JIT_H
#define JIT_H

#include "executor/instrument.h"
#include "utils/resowner.h"


/* Flags determining what kind of JIT operations to perform */
/* 决定要执行哪种 JIT 操作的标志 */
#define PGJIT_NONE     0
#define PGJIT_PERFORM  (1 << 0)
#define PGJIT_OPT3     (1 << 1)
#define PGJIT_INLINE   (1 << 2)
#define PGJIT_EXPR	   (1 << 3)
#define PGJIT_DEFORM   (1 << 4)


typedef struct JitInstrumentation
{
	/* number of emitted functions 生成的函数数量 */
	size_t		created_functions;

	/* accumulated time to generate code 生成代码的累计时间 */
	instr_time	generation_counter;

	/* accumulated time to deform tuples, included into generation_counter */
	/* 元组变形（deform）的累计时间，已包含在 generation_counter 中 */
	instr_time	deform_counter;

	/* accumulated time for inlining 内联（inlining）的累计时间 */
	instr_time	inlining_counter;

	/* accumulated time for optimization 优化的累计时间 */
	instr_time	optimization_counter;

	/* accumulated time for code emission 代码生成的累计时间 */
	instr_time	emission_counter;
} JitInstrumentation;

/*
 * DSM structure for accumulating jit instrumentation of all workers.
 * 用于累加所有 worker 的 JIT 测量信息的 DSM 结构。
 */
typedef struct SharedJitInstrumentation
{
	int			num_workers;
	JitInstrumentation jit_instr[FLEXIBLE_ARRAY_MEMBER];
} SharedJitInstrumentation;

typedef struct JitContext
{
	/* see PGJIT_* above 参见上面的 PGJIT_* */
	int			flags;

	JitInstrumentation instr;
} JitContext;

typedef struct JitProviderCallbacks JitProviderCallbacks;

/*
 * _PG_jit_provider_init: Initialize a JIT provider.
 * 初始化一个 JIT 提供者。
 */
extern PGDLLEXPORT void _PG_jit_provider_init(JitProviderCallbacks *cb);
typedef void (*JitProviderInit) (JitProviderCallbacks *cb);
typedef void (*JitProviderResetAfterErrorCB) (void);
typedef void (*JitProviderReleaseContextCB) (JitContext *context);
struct ExprState;
typedef bool (*JitProviderCompileExprCB) (struct ExprState *state);

/*
 * JitProviderCallbacks: Table of function pointers for the current JIT provider's operations.
 * 当前 JIT 提供者操作的函数指针表。
 */
struct JitProviderCallbacks
{
	JitProviderResetAfterErrorCB reset_after_error;	/* error recovery 错误恢复 */
	JitProviderReleaseContextCB release_context;	/* resource cleanup 资源清理 */
	JitProviderCompileExprCB compile_expr;			/* expression compilation 表达式编译 */
};


/* GUCs */
/* 控制 JIT 行为的配置参数 (GUCs) */
extern PGDLLIMPORT bool jit_enabled;				/* master switch JIT 总开关 */
extern PGDLLIMPORT char *jit_provider;				/* JIT provider library JIT 提供者库 */
extern PGDLLIMPORT bool jit_debugging_support;		/* Enable debugging support 启用调试支持 */
extern PGDLLIMPORT bool jit_dump_bitcode;			/* Dump IR bitcode to files 将 IR 位码转储到文件 */
extern PGDLLIMPORT bool jit_expressions;			/* JIT compile expressions JIT 编译表达式 */
extern PGDLLIMPORT bool jit_profiling_support;		/* Enable profiling support 启用分析支持 */
extern PGDLLIMPORT bool jit_tuple_deforming;		/* JIT compile tuple deforming JIT 编译元组变形过程 */
extern PGDLLIMPORT double jit_above_cost;			/* Cost threshold to enable JIT 启用 JIT 的代价阈值 */
extern PGDLLIMPORT double jit_inline_above_cost;	/* Cost threshold to enable inlining 启用内联的代价阈值 */
extern PGDLLIMPORT double jit_optimize_above_cost;	/* Cost threshold to enable optimizations 启用优化的代价阈值 */


/*
 * jit_reset_after_error: Reset JIT state following an error.
 * 发生错误后重置 JIT 状态。
 */
extern void jit_reset_after_error(void);

/*
 * jit_release_context: Release a JIT context and its related products.
 * 释放 JIT 上下文及其相关产物。
 */
extern void jit_release_context(JitContext *context);

/*
 * Functions for attempting to JIT code. Callers must accept that these might
 * not be able to perform JIT (i.e. return false).
 * 尝试 JIT 编译代码的函数。调用者必须接受这些函数可能无法执行 JIT（即返回 false）。
 */

/*
 * jit_compile_expr: Attempt to JIT compile an expression.
 * 尝试 JIT 编译一个表达式。
 */
extern bool jit_compile_expr(struct ExprState *state);

/*
 * InstrJitAgg: Accumulate JIT instrumentation results.
 * 累加 JIT 测量结果。
 */
extern void InstrJitAgg(JitInstrumentation *dst, JitInstrumentation *add);


#endif							/* JIT_H */
