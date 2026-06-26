#include "postgres.h"

#include "fmgr.h"
#include "ltree/ltree.h"
#include "plpy_util.h"

PG_MODULE_MAGIC_EXT(
					.name = "ltree_plpython",
					.version = PG_VERSION
);

/* Linkage to functions in plpython module
 *
 * 与 plpython 模块中函数的链接
 */
typedef PyObject *(*PLyUnicode_FromStringAndSize_t) (const char *s, Py_ssize_t size);
static PLyUnicode_FromStringAndSize_t PLyUnicode_FromStringAndSize_p;


/*
 * Module initialize function: fetch function pointers for cross-module calls.
 *
 * 模块初始化函数：获取跨模块调用的函数指针。
 */
void
_PG_init(void)
{
	/* Asserts verify that typedefs above match original declarations
	 *
	 * 断言验证上面的 typedef 是否与原始声明匹配
	 */
	AssertVariableIsOfType(&PLyUnicode_FromStringAndSize, PLyUnicode_FromStringAndSize_t);
	PLyUnicode_FromStringAndSize_p = (PLyUnicode_FromStringAndSize_t)
		load_external_function("$libdir/" PLPYTHON_LIBNAME, "PLyUnicode_FromStringAndSize",
							   true, NULL);
}


/* These defines must be after the module init function
 *
 * 这些定义必须位于模块初始化函数之后
 */
#define PLyUnicode_FromStringAndSize PLyUnicode_FromStringAndSize_p


PG_FUNCTION_INFO_V1(ltree_to_plpython);

Datum
ltree_to_plpython(PG_FUNCTION_ARGS)
{
	ltree	   *in = PG_GETARG_LTREE_P(0);
	int			i;
	PyObject   *list;
	ltree_level *curlevel;

	list = PyList_New(in->numlevel);
	if (!list)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	curlevel = LTREE_FIRST(in);
	for (i = 0; i < in->numlevel; i++)
	{
		PyList_SetItem(list, i, PLyUnicode_FromStringAndSize(curlevel->name, curlevel->len));
		curlevel = LEVEL_NEXT(curlevel);
	}

	PG_FREE_IF_COPY(in, 0);

	return PointerGetDatum(list);
}
