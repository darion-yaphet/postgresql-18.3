/*-------------------------------------------------------------------------
 *
 * cmdtag.c
 *	  Data and routines for commandtag names and enumeration.
 *
 *	  命令标签名称和枚举的数据与例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/cmdtag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "tcop/cmdtag.h"
#include "utils/builtins.h"


typedef struct CommandTagBehavior
{
	const char *name;			/* tag name, e.g. "SELECT"
								 *
								 * 标记名称，例如 "SELECT" */
	const uint8 namelen;		/* set to strlen(name)
								 *
								 * 设置为 strlen(name) */
	const bool	event_trigger_ok;
	const bool	table_rewrite_ok;
	const bool	display_rowcount;	/* should the number of rows affected be
									 * shown in the command completion string
									 *
									 * 是否应在命令完成字符串中显示受影响的行数 */
} CommandTagBehavior;

#define PG_CMDTAG(tag, name, evtrgok, rwrok, rowcnt) \
	{ name, (uint8) (sizeof(name) - 1), evtrgok, rwrok, rowcnt },

static const CommandTagBehavior tag_behavior[] = {
#include "tcop/cmdtaglist.h"
};

#undef PG_CMDTAG

/*
 * InitializeQueryCompletion
 *		Initialize a QueryCompletion with an unknown command tag and zero
 *		processed rows.
 *
 *		用未知命令标签和零已处理行数初始化 QueryCompletion。
 */
void
InitializeQueryCompletion(QueryCompletion *qc)
{
	qc->commandTag = CMDTAG_UNKNOWN;
	qc->nprocessed = 0;
}

/*
 * GetCommandTagName
 *		Return the textual command tag name for a CommandTag value.
 *
 *		返回 CommandTag 值对应的文本命令标签名称。
 */
const char *
GetCommandTagName(CommandTag commandTag)
{
	return tag_behavior[commandTag].name;
}

/*
 * GetCommandTagNameAndLen
 *		Return the textual command tag name and store its cached length.
 *
 *		返回文本命令标签名称，并保存其缓存长度。
 */
const char *
GetCommandTagNameAndLen(CommandTag commandTag, Size *len)
{
	*len = (Size) tag_behavior[commandTag].namelen;
	return tag_behavior[commandTag].name;
}

/*
 * command_tag_display_rowcount
 *		Return whether this command tag reports a processed row count.
 *
 *		返回该命令标签是否报告已处理行数。
 */
bool
command_tag_display_rowcount(CommandTag commandTag)
{
	return tag_behavior[commandTag].display_rowcount;
}

/*
 * command_tag_event_trigger_ok
 *		Return whether this command tag is allowed for event triggers.
 *
 *		返回该命令标签是否允许用于事件触发器。
 */
bool
command_tag_event_trigger_ok(CommandTag commandTag)
{
	return tag_behavior[commandTag].event_trigger_ok;
}

/*
 * command_tag_table_rewrite_ok
 *		Return whether this command tag can perform a table rewrite.
 *
 *		返回该命令标签是否可以执行表重写。
 */
bool
command_tag_table_rewrite_ok(CommandTag commandTag)
{
	return tag_behavior[commandTag].table_rewrite_ok;
}

/*
 * Search CommandTag by name
 *
 * 按名称搜索 CommandTag
 *
 * Returns CommandTag, or CMDTAG_UNKNOWN if not recognized
 *
 * 返回 CommandTag；如果无法识别则返回 CMDTAG_UNKNOWN
 *
 * The lookup uses binary search over the command tag behavior table.
 *
 * 查找过程会在命令标签行为表上执行二分搜索。
 */
CommandTag
GetCommandTagEnum(const char *commandname)
{
	const CommandTagBehavior *base,
			   *last,
			   *position;
	int			result;

	if (commandname == NULL || *commandname == '\0')
		return CMDTAG_UNKNOWN;

	base = tag_behavior;
	last = tag_behavior + lengthof(tag_behavior) - 1;
	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = pg_strcasecmp(commandname, position->name);
		if (result == 0)
			return (CommandTag) (position - tag_behavior);
		else if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return CMDTAG_UNKNOWN;
}

/*
 * BuildQueryCompletionString
 *		Build a string containing the command tag name with the
 *		QueryCompletion's nprocessed for command tags with display_rowcount
 *		set.  Returns the strlen of the constructed string.
 *
 *		构建一个包含命令标签名称的字符串；对于设置了 display_rowcount 的命令标签，
 *		还会附加 QueryCompletion 的 nprocessed。返回所构建字符串的 strlen。
 *
 * The caller must ensure that buff is at least COMPLETION_TAG_BUFSIZE bytes.
 *
 * 调用者必须确保 buff 至少有 COMPLETION_TAG_BUFSIZE 字节。
 *
 * If nameonly is true, then the constructed string will contain only the tag
 * name.
 *
 * 如果 nameonly 为 true，则构建出的字符串将只包含标签名称。
 */
Size
BuildQueryCompletionString(char *buff, const QueryCompletion *qc,
						   bool nameonly)
{
	CommandTag	tag = qc->commandTag;
	Size		taglen;
	const char *tagname = GetCommandTagNameAndLen(tag, &taglen);
	char	   *bufp;

	/*
	 * We assume the tagname is plain ASCII and therefore requires no encoding
	 * conversion.
	 *
	 * 我们假定标签名称是纯 ASCII，因此不需要进行编码转换。
	 */
	memcpy(buff, tagname, taglen);
	bufp = buff + taglen;

	/*
	 * ensure that the tagname isn't long enough to overrun the buffer
	 *
	 * 确保标签名称不会长到越过缓冲区边界。
	 */
	Assert(taglen <= COMPLETION_TAG_BUFSIZE - MAXINT8LEN - 4);

	/*
	 * In PostgreSQL versions 11 and earlier, it was possible to create a
	 * table WITH OIDS.  When inserting into such a table, INSERT used to
	 * include the Oid of the inserted record in the completion tag.  To
	 * maintain compatibility in the wire protocol, we now write a "0" (for
	 * InvalidOid) in the location where we once wrote the new record's Oid.
	 *
	 * 在 PostgreSQL 11 及更早版本中，可以创建 WITH OIDS 表。向这类表插入时，
	 * INSERT 过去会在完成标签中包含所插入记录的 Oid。为了保持线路协议兼容性，
	 * 现在会在曾经写入新记录 Oid 的位置写入 "0"（表示 InvalidOid）。
	 */
	if (command_tag_display_rowcount(tag) && !nameonly)
	{
		if (tag == CMDTAG_INSERT)
		{
			*bufp++ = ' ';
			*bufp++ = '0';
		}
		*bufp++ = ' ';
		bufp += pg_ulltoa_n(qc->nprocessed, bufp);
	}

	/*
	 * and finally, NUL terminate the string
	 *
	 * 最后，用 NUL 结束该字符串。
	 */
	*bufp = '\0';

	Assert((bufp - buff) == strlen(buff));

	return bufp - buff;
}
