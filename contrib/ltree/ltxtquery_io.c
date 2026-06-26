/*
 * txtquery io
 * Teodor Sigaev <teodor@stack.net>
 * contrib/ltree/ltxtquery_io.c
 *
 * txtquery io Teodor Sigaev <teodor@stack.net> contrib/ltree/ltxtquery_io.c
 *
 * txtquery io Teodor Sigaev <teodor@stack.net> contrib/ltree/ltxtquery_io.c
 *
 * txtquery io Teodor Sigaev <teodor@stack.net> contrib/ltree/ltxtquery_io.c
 */
#include "postgres.h"

#include <ctype.h>

#include "crc32.h"
#include "libpq/pqformat.h"
#include "ltree.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "varatt.h"


/* parser's states
 *
 * 解析器的状态
 */
#define WAITOPERAND 1
#define INOPERAND 2
#define WAITOPERATOR	3

/*
 * node of query tree, also used
 * for storing polish notation in parser
 *
 * 查询树的节点，也用于在解析器中存储波兰表示法
 */
typedef struct NODE
{
	int32		type;
	int32		val;
	int16		distance;
	int16		length;
	uint16		flag;
	struct NODE *next;
} NODE;

typedef struct
{
	char	   *buf;
	int32		state;
	int32		count;
	struct Node *escontext;
	/* reverse polish notation in list (for temporary usage)
	 *
	 * 列表中的逆波兰表示法（供临时使用）
	 */
	NODE	   *str;
	/* number in str
	 *
	 * str 中的数字
	 */
	int32		num;

	/* user-friendly operand
	 *
	 * 用户友好的操作数
	 */
	int32		lenop;
	int32		sumlen;
	char	   *op;
	char	   *curop;
} QPRS_STATE;

/*
 * get token from query string
 *
 * 从查询字符串中获取令牌
 *
 * caller needs to check if a soft-error was set if the result is ERR.
 *
 * 如果结果是 ERR，调用者需要检查是否设置了软错误。
 */
static int32
gettoken_query(QPRS_STATE *state, int32 *val, int32 *lenval, char **strval, uint16 *flag)
{
	int			charlen;

	for (;;)
	{
		charlen = pg_mblen_cstr(state->buf);

		switch (state->state)
		{
			case WAITOPERAND:
				if (t_iseq(state->buf, '!'))
				{
					(state->buf)++;
					*val = (int32) '!';
					return OPR;
				}
				else if (t_iseq(state->buf, '('))
				{
					state->count++;
					(state->buf)++;
					return OPEN;
				}
				else if (ISLABEL(state->buf))
				{
					state->state = INOPERAND;
					*strval = state->buf;
					*lenval = charlen;
					*flag = 0;
				}
				else if (!isspace((unsigned char) *state->buf))
					ereturn(state->escontext, ERR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("operand syntax error")));
				break;
			case INOPERAND:
				if (ISLABEL(state->buf))
				{
					if (*flag)
						ereturn(state->escontext, ERR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("modifiers syntax error")));
					*lenval += charlen;
				}
				else if (t_iseq(state->buf, '%'))
					*flag |= LVAR_SUBLEXEME;
				else if (t_iseq(state->buf, '@'))
					*flag |= LVAR_INCASE;
				else if (t_iseq(state->buf, '*'))
					*flag |= LVAR_ANYEND;
				else
				{
					state->state = WAITOPERATOR;
					return VAL;
				}
				break;
			case WAITOPERATOR:
				if (t_iseq(state->buf, '&') || t_iseq(state->buf, '|'))
				{
					state->state = WAITOPERAND;
					*val = (int32) *(state->buf);
					(state->buf)++;
					return OPR;
				}
				else if (t_iseq(state->buf, ')'))
				{
					(state->buf)++;
					state->count--;
					return (state->count < 0) ? ERR : CLOSE;
				}
				else if (*(state->buf) == '\0')
				{
					return (state->count) ? ERR : END;
				}
				else if (!t_iseq(state->buf, ' '))
				{
					return ERR;
				}
				break;
			default:
				return ERR;
				break;
		}

		state->buf += charlen;
	}

	/* should not get here
	 *
	 * 不应该到达这里
	 */
}

/*
 * push new one in polish notation reverse view
 *
 * 以波兰符号反向视图推送新的
 */
static bool
pushquery(QPRS_STATE *state, int32 type, int32 val, int32 distance, int32 lenval, uint16 flag)
{
	NODE	   *tmp = (NODE *) palloc(sizeof(NODE));

	tmp->type = type;
	tmp->val = val;
	tmp->flag = flag;
	if (distance > 0xffff)
		ereturn(state->escontext, false,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("value is too big")));
	if (lenval > 0xff)
		ereturn(state->escontext, false,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("operand is too long")));
	tmp->distance = distance;
	tmp->length = lenval;
	tmp->next = state->str;
	state->str = tmp;
	state->num++;
	return true;
}

/*
 * This function is used for query text parsing
 *
 * 该函数用于查询文本解析
 */
static bool
pushval_asis(QPRS_STATE *state, int type, char *strval, int lenval, uint16 flag)
{
	if (lenval > 0xffff)
		ereturn(state->escontext, false,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("word is too long")));

	if (!pushquery(state, type, ltree_crc32_sz(strval, lenval),
				   state->curop - state->op, lenval, flag))
		return false;

	while (state->curop - state->op + lenval + 1 >= state->lenop)
	{
		int32		tmp = state->curop - state->op;

		state->lenop *= 2;
		state->op = (char *) repalloc(state->op, state->lenop);
		state->curop = state->op + tmp;
	}
	memcpy(state->curop, strval, lenval);
	state->curop += lenval;
	*(state->curop) = '\0';
	state->curop++;
	state->sumlen += lenval + 1;
	return true;
}

#define STACKDEPTH		32
/*
 * make polish notation of query
 *
 * 使用波兰语表示查询
 */
static int32
makepol(QPRS_STATE *state)
{
	int32		val = 0,
				type;
	int32		lenval = 0;
	char	   *strval = NULL;
	int32		stack[STACKDEPTH];
	int32		lenstack = 0;
	uint16		flag = 0;

	/* since this function recurses, it could be driven to stack overflow
	 *
	 * 由于该函数会递归，因此可能会导致堆栈溢出
	 */
	check_stack_depth();

	while ((type = gettoken_query(state, &val, &lenval, &strval, &flag)) != END)
	{
		switch (type)
		{
			case VAL:
				if (!pushval_asis(state, VAL, strval, lenval, flag))
					return ERR;
				while (lenstack && (stack[lenstack - 1] == (int32) '&' ||
									stack[lenstack - 1] == (int32) '!'))
				{
					lenstack--;
					if (!pushquery(state, OPR, stack[lenstack], 0, 0, 0))
						return ERR;
				}
				break;
			case OPR:
				if (lenstack && val == (int32) '|')
				{
					if (!pushquery(state, OPR, val, 0, 0, 0))
						return ERR;
				}
				else
				{
					if (lenstack == STACKDEPTH)
						/* internal error
						 *
						 * 内部错误
						 */
						elog(ERROR, "stack too short");
					stack[lenstack] = val;
					lenstack++;
				}
				break;
			case OPEN:
				if (makepol(state) == ERR)
					return ERR;
				while (lenstack && (stack[lenstack - 1] == (int32) '&' ||
									stack[lenstack - 1] == (int32) '!'))
				{
					lenstack--;
					if (!pushquery(state, OPR, stack[lenstack], 0, 0, 0))
						return ERR;
				}
				break;
			case CLOSE:
				while (lenstack)
				{
					lenstack--;
					if (!pushquery(state, OPR, stack[lenstack], 0, 0, 0))
						return ERR;
				};
				return END;
				break;
			case ERR:
				if (SOFT_ERROR_OCCURRED(state->escontext))
					return ERR;
				/* fall through
				 *
				 * 跌倒
				 */
			default:
				ereturn(state->escontext, ERR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error")));

		}
	}
	while (lenstack)
	{
		lenstack--;
		if (!pushquery(state, OPR, stack[lenstack], 0, 0, 0))
			return ERR;
	};
	return END;
}

static void
findoprnd(ITEM *ptr, int32 *pos)
{
	/* since this function recurses, it could be driven to stack overflow.
	 *
	 * 由于该函数会递归，因此可能会导致堆栈溢出。
	 */
	check_stack_depth();

	if (ptr[*pos].type == VAL || ptr[*pos].type == VALTRUE)
	{
		ptr[*pos].left = 0;
		(*pos)++;
	}
	else if (ptr[*pos].val == (int32) '!')
	{
		ptr[*pos].left = 1;
		(*pos)++;
		findoprnd(ptr, pos);
	}
	else
	{
		ITEM	   *curitem = &ptr[*pos];
		int32		tmp = *pos;

		(*pos)++;
		findoprnd(ptr, pos);
		curitem->left = *pos - tmp;
		findoprnd(ptr, pos);
	}
}


/*
 * input
 */
static ltxtquery *
queryin(char *buf, struct Node *escontext)
{
	QPRS_STATE	state;
	int32		i;
	ltxtquery  *query;
	int32		commonlen;
	ITEM	   *ptr;
	NODE	   *tmp;
	int32		pos = 0;

#ifdef BS_DEBUG
	char		pbuf[16384],
			   *cur;
#endif

	/* init state
	 *
	 * 初始化状态
	 */
	state.buf = buf;
	state.state = WAITOPERAND;
	state.count = 0;
	state.num = 0;
	state.str = NULL;
	state.escontext = escontext;

	/* init list of operand
	 *
	 * 初始化操作数列表
	 */
	state.sumlen = 0;
	state.lenop = 64;
	state.curop = state.op = (char *) palloc(state.lenop);
	*(state.curop) = '\0';

	/* parse query & make polish notation (postfix, but in reverse order)
	 *
	 * 解析查询并制作波兰符号（后缀，但顺序相反）
	 */
	if (makepol(&state) == ERR)
		return NULL;
	if (!state.num)
		ereturn(escontext, NULL,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error"),
				 errdetail("Empty query.")));

	if (LTXTQUERY_TOO_BIG(state.num, state.sumlen))
		ereturn(escontext, NULL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("ltxtquery is too large")));
	commonlen = COMPUTESIZE(state.num, state.sumlen);

	query = (ltxtquery *) palloc0(commonlen);
	SET_VARSIZE(query, commonlen);
	query->size = state.num;
	ptr = GETQUERY(query);

	/* set item in polish notation
	 *
	 * 以波兰表示法设置项目
	 */
	for (i = 0; i < state.num; i++)
	{
		ptr[i].type = state.str->type;
		ptr[i].val = state.str->val;
		ptr[i].distance = state.str->distance;
		ptr[i].length = state.str->length;
		ptr[i].flag = state.str->flag;
		tmp = state.str->next;
		pfree(state.str);
		state.str = tmp;
	}

	/* set user-friendly operand view
	 *
	 * 设置用户友好的操作数视图
	 */
	memcpy(GETOPERAND(query), state.op, state.sumlen);
	pfree(state.op);

	/* set left operand's position for every operator
	 *
	 * 为每个运算符设置左操作数的位置
	 */
	pos = 0;
	findoprnd(ptr, &pos);

	return query;
}

/*
 * in without morphology
 *
 * 没有形态学
 */
PG_FUNCTION_INFO_V1(ltxtq_in);
Datum
ltxtq_in(PG_FUNCTION_ARGS)
{
	ltxtquery  *res;

	if ((res = queryin(PG_GETARG_POINTER(0), fcinfo->context)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_POINTER(res);
}

/*
 * ltxtquery type recv function
 *
 * ltxtquery类型recv函数
 *
 * The type is sent as text in binary mode, so this is almost the same
 * as the input function, but it's prefixed with a version number so we
 * can change the binary format sent in future if necessary. For now,
 * only version 1 is supported.
 *
 * 该类型以二进制模式作为文本发送，因此这与输入函数几乎相同，但它带有版本号前缀，因此我们可以在必要时更改将来发送的二进制格式。目前仅支持版本 1。
 */
PG_FUNCTION_INFO_V1(ltxtq_recv);
Datum
ltxtq_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			version = pq_getmsgint(buf, 1);
	char	   *str;
	int			nbytes;
	ltxtquery  *res;

	if (version != 1)
		elog(ERROR, "unsupported ltxtquery version number %d", version);

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	res = queryin(str, NULL);
	pfree(str);

	PG_RETURN_POINTER(res);
}

/*
 * out function
 *
 * 输出函数
 */
typedef struct
{
	ITEM	   *curpol;
	char	   *buf;
	char	   *cur;
	char	   *op;
	int32		buflen;
} INFIX;

#define RESIZEBUF(inf,addsize) \
while( ( (inf)->cur - (inf)->buf ) + (addsize) + 1 >= (inf)->buflen ) \
{ \
	int32 len = (inf)->cur - (inf)->buf; \
	(inf)->buflen *= 2; \
	(inf)->buf = (char*) repalloc( (void*)(inf)->buf, (inf)->buflen ); \
	(inf)->cur = (inf)->buf + len; \
}

/*
 * recursive walk on tree and print it in
 * infix (human-readable) view
 *
 * 在树上递归行走并在中缀（人类可读）视图中打印它
 */
static void
infix(INFIX *in, bool first)
{
	/* since this function recurses, it could be driven to stack overflow.
	 *
	 * 由于该函数会递归，因此可能会导致堆栈溢出。
	 */
	check_stack_depth();

	if (in->curpol->type == VAL)
	{
		char	   *op = in->op + in->curpol->distance;

		RESIZEBUF(in, in->curpol->length * 2 + 5);
		while (*op)
		{
			*(in->cur) = *op;
			op++;
			in->cur++;
		}
		if (in->curpol->flag & LVAR_SUBLEXEME)
		{
			*(in->cur) = '%';
			in->cur++;
		}
		if (in->curpol->flag & LVAR_INCASE)
		{
			*(in->cur) = '@';
			in->cur++;
		}
		if (in->curpol->flag & LVAR_ANYEND)
		{
			*(in->cur) = '*';
			in->cur++;
		}
		*(in->cur) = '\0';
		in->curpol++;
	}
	else if (in->curpol->val == (int32) '!')
	{
		bool		isopr = false;

		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol++;
		if (in->curpol->type == OPR)
		{
			isopr = true;
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}
		infix(in, isopr);
		if (isopr)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
	else
	{
		int32		op = in->curpol->val;
		INFIX		nrm;

		in->curpol++;
		if (op == (int32) '|' && !first)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}

		nrm.curpol = in->curpol;
		nrm.op = in->op;
		nrm.buflen = 16;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);

		/* get right operand
		 *
		 * 得到正确的操作数
		 */
		infix(&nrm, false);

		/* get & print left operand
		 *
		 * 获取并打印左操作数
		 */
		in->curpol = nrm.curpol;
		infix(in, false);

		/* print operator & right operand
		 *
		 * 打印运算符和右操作数
		 */
		RESIZEBUF(in, 3 + (nrm.cur - nrm.buf));
		sprintf(in->cur, " %c %s", op, nrm.buf);
		in->cur = strchr(in->cur, '\0');
		pfree(nrm.buf);

		if (op == (int32) '|' && !first)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
}

PG_FUNCTION_INFO_V1(ltxtq_out);
Datum
ltxtq_out(PG_FUNCTION_ARGS)
{
	ltxtquery  *query = PG_GETARG_LTXTQUERY_P(0);
	INFIX		nrm;

	if (query->size == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error"),
				 errdetail("Empty query.")));

	nrm.curpol = GETQUERY(query);
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
	*(nrm.cur) = '\0';
	nrm.op = GETOPERAND(query);
	infix(&nrm, true);

	PG_RETURN_POINTER(nrm.buf);
}

/*
 * ltxtquery type send function
 *
 * ltxtquery类型发送函数
 *
 * The type is sent as text in binary mode, so this is almost the same
 * as the output function, but it's prefixed with a version number so we
 * can change the binary format sent in future if necessary. For now,
 * only version 1 is supported.
 *
 * 该类型以二进制模式作为文本发送，因此这与输出函数几乎相同，但它带有版本号前缀，因此我们可以在必要时更改将来发送的二进制格式。目前仅支持版本 1。
 */
PG_FUNCTION_INFO_V1(ltxtq_send);
Datum
ltxtq_send(PG_FUNCTION_ARGS)
{
	ltxtquery  *query = PG_GETARG_LTXTQUERY_P(0);
	StringInfoData buf;
	int			version = 1;
	INFIX		nrm;

	if (query->size == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error"),
				 errdetail("Empty query.")));

	nrm.curpol = GETQUERY(query);
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
	*(nrm.cur) = '\0';
	nrm.op = GETOPERAND(query);
	infix(&nrm, true);

	pq_begintypsend(&buf);
	pq_sendint8(&buf, version);
	pq_sendtext(&buf, nrm.buf, strlen(nrm.buf));
	pfree(nrm.buf);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}
