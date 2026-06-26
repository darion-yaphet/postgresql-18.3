/*-------------------------------------------------------------------------
 *
 * trgm_regexp.c
 *	  Regular expression matching using trigrams.
 *
 * The general idea of trigram index support for a regular expression (regex)
 * search is to transform the regex into a logical expression on trigrams.
 * For example:
 *
 *	 (ab|cd)efg  =>  ((abe & bef) | (cde & def)) & efg
 *
 * If a string matches the regex, then it must match the logical expression on
 * trigrams.  The opposite is not necessarily true, however: a string that
 * matches the logical expression might not match the original regex.  Such
 * false positives are removed via recheck, by running the regular regex match
 * operator on the retrieved heap tuple.
 *
 * Since the trigram expression involves both AND and OR operators, we can't
 * expect the core index machinery to evaluate it completely.  Instead, the
 * result of regex analysis is a list of trigrams to be sought in the index,
 * plus a simplified graph that is used by trigramsMatchGraph() to determine
 * whether a particular indexed value matches the expression.
 *
 * Converting a regex to a trigram expression is based on analysis of an
 * automaton corresponding to the regex.  The algorithm consists of four
 * stages:
 *
 * 1) Compile the regexp to NFA form.  This is handled by the PostgreSQL
 *	  regexp library, which provides accessors for its opaque regex_t struct
 *	  to expose the NFA state graph and the "colors" (sets of equivalent
 *	  characters) used as state transition labels.
 *
 * 2) Transform the original NFA into an expanded graph, where arcs
 *	  are labeled with trigrams that must be present in order to move from
 *	  one state to another via the arcs.  The trigrams used in this stage
 *	  consist of colors, not characters, as in the original NFA.
 *
 * 3) Expand the color trigrams into regular trigrams consisting of
 *	  characters.  If too many distinct trigrams are produced, trigrams are
 *	  eliminated and the graph is simplified until it's simple enough.
 *
 * 4) Finally, the resulting graph is packed into a TrgmPackedGraph struct,
 *	  and returned to the caller.
 *
 * 1) Compile the regexp to NFA form
 * ---------------------------------
 * The automaton returned by the regexp compiler is a graph where vertices
 * are "states" and arcs are labeled with colors.  Each color represents
 * a set of characters, so that all characters assigned to the same color
 * are interchangeable, so far as matching the regexp is concerned.  There
 * are two special states: "initial" and "final".  A state can have multiple
 * outgoing arcs labeled with the same color, which makes the automaton
 * non-deterministic, because it can be in many states simultaneously.
 *
 * Note that this NFA is already lossy compared to the original regexp,
 * since it ignores some regex features such as lookahead constraints and
 * backref matching.  This is OK for our purposes since it's still the case
 * that only strings matching the NFA can possibly satisfy the regexp.
 *
 * 2) Transform the original NFA into an expanded graph
 * ----------------------------------------------------
 * In the 2nd stage, the automaton is transformed into a graph based on the
 * original NFA.  Each state in the expanded graph represents a state from
 * the original NFA, plus a prefix identifying the last two characters
 * (colors, to be precise) seen before entering the state.  There can be
 * multiple states in the expanded graph for each state in the original NFA,
 * depending on what characters can precede it.  A prefix position can be
 * "unknown" if it's uncertain what the preceding character was, or "blank"
 * if the character was a non-word character (we don't need to distinguish
 * which non-word character it was, so just think of all of them as blanks).
 *
 * For convenience in description, call an expanded-state identifier
 * (two prefix colors plus a state number from the original NFA) an
 * "enter key".
 *
 * Each arc of the expanded graph is labeled with a trigram that must be
 * present in the string to match.  We can construct this from an out-arc of
 * the underlying NFA state by combining the expanded state's prefix with the
 * color label of the underlying out-arc, if neither prefix position is
 * "unknown".  But note that some of the colors in the trigram might be
 * "blank".  This is OK since we want to generate word-boundary trigrams as
 * the regular trigram machinery would, if we know that some word characters
 * must be adjacent to a word boundary in all strings matching the NFA.
 *
 * The expanded graph can also have fewer states than the original NFA,
 * because we don't bother to make a separate state entry unless the state
 * is reachable by a valid arc.  When an enter key is reachable from a state
 * of the expanded graph, but we do not know a complete trigram associated
 * with that transition, we cannot make a valid arc; instead we insert the
 * enter key into the enterKeys list of the source state.  This effectively
 * means that the two expanded states are not reliably distinguishable based
 * on examining trigrams.
 *
 * So the expanded graph resembles the original NFA, but the arcs are
 * labeled with trigrams instead of individual characters, and there may be
 * more or fewer states.  It is a lossy representation of the original NFA:
 * any string that matches the original regexp must match the expanded graph,
 * but the reverse is not true.
 *
 * We build the expanded graph through a breadth-first traversal of states
 * reachable from the initial state.  At each reachable state, we identify the
 * states reachable from it without traversing a predictable trigram, and add
 * those states' enter keys to the current state.  Then we generate all
 * out-arcs leading out of this collection of states that have predictable
 * trigrams, adding their target states to the queue of states to examine.
 *
 * When building the graph, if the number of states or arcs exceed pre-defined
 * limits, we give up and simply mark any states not yet processed as final
 * states.  Roughly speaking, that means that we make use of some portion from
 * the beginning of the regexp.  Also, any colors that have too many member
 * characters are treated as "unknown", so that we can't derive trigrams
 * from them.
 *
 * 3) Expand the color trigrams into regular trigrams
 * --------------------------------------------------
 * The trigrams in the expanded graph are "color trigrams", consisting
 * of three consecutive colors that must be present in the string. But for
 * search, we need regular trigrams consisting of characters. In the 3rd
 * stage, the color trigrams are expanded into regular trigrams. Since each
 * color can represent many characters, the total number of regular trigrams
 * after expansion could be very large. Because searching the index for
 * thousands of trigrams would be slow, and would likely produce so many
 * false positives that we would have to traverse a large fraction of the
 * index, the graph is simplified further in a lossy fashion by removing
 * color trigrams. When a color trigram is removed, the states connected by
 * any arcs labeled with that trigram are merged.
 *
 * Trigrams do not all have equivalent value for searching: some of them are
 * more frequent and some of them are less frequent. Ideally, we would like
 * to know the distribution of trigrams, but we don't. But because of padding
 * we know for sure that the empty character is more frequent than others,
 * so we can penalize trigrams according to presence of whitespace. The
 * penalty assigned to each color trigram is the number of simple trigrams
 * it would produce, times the penalties[] multiplier associated with its
 * whitespace content. (The penalties[] constants were calculated by analysis
 * of some real-life text.) We eliminate color trigrams starting with the
 * highest-penalty one, until we get to a total penalty of no more than
 * WISH_TRGM_PENALTY. However, we cannot remove a color trigram if that would
 * lead to merging the initial and final states, so we may not be able to
 * reach WISH_TRGM_PENALTY. It's still okay so long as we have no more than
 * MAX_TRGM_COUNT simple trigrams in total, otherwise we fail.
 *
 * 4) Pack the graph into a compact representation
 * -----------------------------------------------
 * The 2nd and 3rd stages might have eliminated or merged many of the states
 * and trigrams created earlier, so in this final stage, the graph is
 * compacted and packed into a simpler struct that contains only the
 * information needed to evaluate it.
 *
 * ALGORITHM EXAMPLE:
 *
 * Consider the example regex "ab[cd]".  This regex is transformed into the
 * following NFA (for simplicity we show colors as their single members):
 *
 *					  4#
 *					c/
 *		 a	   b	/
 *	 1* --- 2 ---- 3
 *					\
 *					d\
 *					  5#
 *
 * We use * to mark initial state and # to mark final state. It's not depicted,
 * but states 1, 4, 5 have self-referencing arcs for all possible characters,
 * because this pattern can match to any part of a string.
 *
 * As the result of stage 2 we will have the following graph:
 *
 *		  abc	 abd
 *	 2# <---- 1* ----> 3#
 *
 * The process for generating this graph is:
 * 1) Create state 1 with enter key (UNKNOWN, UNKNOWN, 1).
 * 2) Add key (UNKNOWN, "a", 2) to state 1.
 * 3) Add key ("a", "b", 3) to state 1.
 * 4) Create new state 2 with enter key ("b", "c", 4).  Add an arc
 *	  from state 1 to state 2 with label trigram "abc".
 * 5) Mark state 2 final because state 4 of source NFA is marked as final.
 * 6) Create new state 3 with enter key ("b", "d", 5).  Add an arc
 *	  from state 1 to state 3 with label trigram "abd".
 * 7) Mark state 3 final because state 5 of source NFA is marked as final.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/pg_trgm/trgm_regexp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation_d.h"
#include "regex/regexport.h"
#include "trgm.h"
#include "tsearch/ts_locale.h"
#include "utils/formatting.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "varatt.h"

/*
 * Uncomment (or use -DTRGM_REGEXP_DEBUG) to print debug info,
 * for exploring and debugging the algorithm implementation.
 * This produces three graph files in /tmp, in Graphviz .gv format.
 * Some progress information is also printed to postmaster stderr.
 *
 * 取消注释（或使用 -DTRGM_REGEXP_DEBUG）来打印调试信息，以探索和调试算法实现。这会在 /tmp 中生成 Graphviz .gv 格式的三个图形文件。一些进度信息也会打印到 postmaster stderr。
 */
/* #define TRGM_REGEXP_DEBUG */

/*
 * These parameters are used to limit the amount of work done.
 * Otherwise regex processing could be too slow and memory-consuming.
 *
 * 这些参数用于限制完成的工作量。否则正则表达式处理可能会太慢并且消耗内存。
 *
 *	MAX_EXPANDED_STATES - How many states we allow in expanded graph
 *	MAX_EXPANDED_ARCS - How many arcs we allow in expanded graph
 *	MAX_TRGM_COUNT - How many simple trigrams we allow to be extracted
 *	WISH_TRGM_PENALTY - Maximum desired sum of color trigram penalties
 *	COLOR_COUNT_LIMIT - Maximum number of characters per color
 *
 * MAX_EXPANDED_STATES - 我们在扩展图中允许有多少个状态 MAX_EXPANDED_ARCS - 我们在扩展图中允许有多少个弧 MAX_TRGM_COUNT - 我们允许提取多少个简单三元组 WISH_TRGM_PENALTY - 颜色三元组惩罚的最大所需总和 COLOR_COUNT_LIMIT - 每种颜色的最大字符数
 */
#define MAX_EXPANDED_STATES 128
#define MAX_EXPANDED_ARCS	1024
#define MAX_TRGM_COUNT		256
#define WISH_TRGM_PENALTY	16
#define COLOR_COUNT_LIMIT	256

/*
 * Penalty multipliers for trigram counts depending on whitespace contents.
 * Numbers based on analysis of real-life texts.
 *
 * 三元组计数的惩罚乘数取决于空白内容。基于现实生活文本分析的数字。
 */
static const float4 penalties[8] = {
	1.0f,						/* "aaa" */
	3.5f,						/* "aa " */
	0.0f,						/* "a a" (impossible) */
	0.0f,						/* "a  " (impossible) */
	4.2f,						/* " aa" */
	2.1f,						/* " a " */
	25.0f,						/* "  a" */
	0.0f						/* "   " (impossible) */
};

/* Struct representing a single pg_wchar, converted back to multibyte form
 *
 * 表示单个 pg_wchar 的结构，转换回多字节形式
 */
typedef struct
{
	char		bytes[MAX_MULTIBYTE_CHAR_LEN];
} trgm_mb_char;

/*
 * Attributes of NFA colors:
 *
 * NFA颜色的属性：
 *
 *	expandable				- we know the character expansion of this color
 *	containsNonWord			- color contains non-word characters
 *							  (which will not be extracted into trigrams)
 *	wordCharsCount			- count of word characters in color
 *	wordChars				- array of this color's word characters
 *							  (which can be extracted into trigrams)
 *
 * Expandable - 我们知道该颜色的字符扩展 containsNonWord - 颜色包含非单词字符（不会被提取为三元组） wordCharsCount - 颜色中的单词字符计数 wordChars - 该颜色的单词字符数组（可以提取为三元组）
 *
 * When expandable is false, the other attributes don't matter; we just
 * assume this color represents unknown character(s).
 *
 * 当 Expandable 为 false 时，其他属性并不重要；我们只是假设该颜色代表未知字符。
 */
typedef struct
{
	bool		expandable;
	bool		containsNonWord;
	int			wordCharsCount;
	trgm_mb_char *wordChars;
} TrgmColorInfo;

/*
 * A "prefix" is information about the colors of the last two characters read
 * before reaching a specific NFA state.  These colors can have special values
 * COLOR_UNKNOWN and COLOR_BLANK.  COLOR_UNKNOWN means that we have no
 * information, for example because we read some character of an unexpandable
 * color.  COLOR_BLANK means that we read a non-word character.
 *
 * “前缀”是有关在达到特定 NFA 状态之前读取的最后两个字符的颜色的信息。  这些颜色可以具有特殊值 COLOR_UNKNOWN 和 COLOR_BLANK。  COLOR_UNKNOWN 意味着我们没有信息，例如因为我们读取了一些不可扩展颜色的字符。  COLOR_BLANK 表示我们读取的是非单词字符。
 *
 * We call a prefix ambiguous if at least one of its colors is unknown.  It's
 * fully ambiguous if both are unknown, partially ambiguous if only the first
 * is unknown.  (The case of first color known, second unknown is not valid.)
 *
 * 如果前缀的至少一种颜色未知，我们称其为不明确的。  如果两者都未知，则它是完全不明确的；如果只有第一个未知，则它是部分不明确的。  （第一种颜色已知，第二种颜色未知的情况无效。）
 *
 * Wholly- or partly-blank prefixes are mostly handled the same as regular
 * color prefixes.  This allows us to generate appropriate partly-blank
 * trigrams when the NFA requires word character(s) to appear adjacent to
 * non-word character(s).
 *
 * 完全或部分空白前缀的处理方式大多与常规颜色前缀相同。  当 NFA 要求单词字符与非单词字符相邻时，这使我们能够生成适当的部分空白三元组。
 */
typedef int TrgmColor;

/* We assume that colors returned by the regexp engine cannot be these:
 *
 * 我们假设正则表达式引擎返回的颜色不能是这些：
 */
#define COLOR_UNKNOWN	(-3)
#define COLOR_BLANK		(-4)

typedef struct
{
	TrgmColor	colors[2];
} TrgmPrefix;

/*
 * Color-trigram data type.  Note that some elements of the trigram can be
 * COLOR_BLANK, but we don't allow COLOR_UNKNOWN.
 *
 * 颜色三元组数据类型。  请注意，三元组的某些元素可以是 COLOR_BLANK，但我们不允许 COLOR_UNKNOWN。
 */
typedef struct
{
	TrgmColor	colors[3];
} ColorTrgm;

/*
 * Key identifying a state of our expanded graph: color prefix, and number
 * of the corresponding state in the underlying regex NFA.  The color prefix
 * shows how we reached the regex state (to the extent that we know it).
 *
 * 识别扩展图状态的键：颜色前缀和底层正则表达式 NFA 中相应状态的编号。  颜色前缀显示了我们如何达到正则表达式状态（就我们所知而言）。
 */
typedef struct
{
	TrgmPrefix	prefix;
	int			nstate;
} TrgmStateKey;

/*
 * One state of the expanded graph.
 *
 * 展开图的一种状态。
 *
 *	stateKey - ID of this state
 *	arcs	 - outgoing arcs of this state (List of TrgmArc)
 *	enterKeys - enter keys reachable from this state without reading any
 *			   predictable trigram (List of TrgmStateKey)
 *	flags	 - flag bits
 *	snumber  - number of this state (initially assigned as -1, -2, etc,
 *			   for debugging purposes only; then at the packaging stage,
 *			   surviving states are renumbered with positive numbers)
 *	parent	 - parent state, if this state has been merged into another
 *	tentFlags - flags this state would acquire via planned merges
 *	tentParent - planned parent state, if considering a merge
 *
 * stateKey - 此状态的 ID arcs - 此状态的传出弧（TrgmArc 列表） EnterKeys - 输入从此状态可到达的键，无需读取任何可预测的三元组（TrgmStateKey 列表） flags - 标志位 snumber - 此状态的编号（最初指定为 -1、-2 等，仅用于调试目的；然后在打包阶段，幸存状态将用正数重新编号）parent - 父状态，如果此状态已合并到另一个 tentFlags - 该状态将通过计划合并获取的标志 tentParent - 计划父状态（如果考虑合并）
 */
#define TSTATE_INIT		0x01	/* flag indicating this state is initial */
#define TSTATE_FIN		0x02	/* flag indicating this state is final */

typedef struct TrgmState
{
	TrgmStateKey stateKey;		/* hashtable key: must be first field */
	List	   *arcs;
	List	   *enterKeys;
	int			flags;
	int			snumber;
	struct TrgmState *parent;
	int			tentFlags;
	struct TrgmState *tentParent;
} TrgmState;

/*
 * One arc in the expanded graph.
 *
 * 展开图中的一条弧。
 */
typedef struct
{
	ColorTrgm	ctrgm;			/* trigram needed to traverse arc */
	TrgmState  *target;			/* next state */
} TrgmArc;

/*
 * Information about arc of specific color trigram (used in stage 3)
 *
 * 有关特定颜色卦的弧线的信息（在第3阶段使用）
 *
 * Contains pointers to the source and target states.
 *
 * 包含指向源状态和目标状态的指针。
 */
typedef struct
{
	TrgmState  *source;
	TrgmState  *target;
} TrgmArcInfo;

/*
 * Information about color trigram (used in stage 3)
 *
 * 有关颜色三元组的信息（在第 3 阶段使用）
 *
 * ctrgm	- trigram itself
 * cnumber	- number of this trigram (used in the packaging stage)
 * count	- number of simple trigrams created from this color trigram
 * expanded - indicates this color trigram is expanded into simple trigrams
 * arcs		- list of all arcs labeled with this color trigram.
 *
 * ctrgm - 三元组本身 cnumber - 此三元组的编号（在打包阶段使用） count - 从此颜色三元组创建的简单三元组的数量 扩展 - 指示此颜色三元组扩展为简单三元组 arcs - 用此颜色三元组标记的所有弧的列表。
 */
typedef struct
{
	ColorTrgm	ctrgm;
	int			cnumber;
	int			count;
	float4		penalty;
	bool		expanded;
	List	   *arcs;
} ColorTrgmInfo;

/*
 * Data structure representing all the data we need during regex processing.
 *
 * 表示正则表达式处理期间我们需要的所有数据的数据结构。
 *
 *	regex			- compiled regex
 *	colorInfo		- extracted information about regex's colors
 *	ncolors			- number of colors in colorInfo[]
 *	states			- hashtable of TrgmStates (states of expanded graph)
 *	initState		- pointer to initial state of expanded graph
 *	queue			- queue of to-be-processed TrgmStates
 *	keysQueue		- queue of to-be-processed TrgmStateKeys
 *	arcsCount		- total number of arcs of expanded graph (for resource
 *					  limiting)
 *	overflowed		- we have exceeded resource limit for transformation
 *	colorTrgms		- array of all color trigrams present in graph
 *	colorTrgmsCount - count of those color trigrams
 *	totalTrgmCount	- total count of extracted simple trigrams
 *
 * regex - 编译的正则表达式 colorInfo - 提取有关正则表达式颜色的信息 ncolors - colorInfo[] states 中的颜色数量 - TrgmStates 的哈希表（扩展图的状态） initState - 指向扩展图初始状态的指针 队列 - 待处理的 TrgmStates 队列 keysQueue - 待处理的 TrgmStateKeys 队列 arcsCount - 扩展图的弧总数（用于资源限制）溢出 - 我们有超出了转换的资源限制 colorTrgms - 图形中存在的所有颜色三元组的数组 colorTrgmsCount - 这些颜色三元组的计数 TotalTrgmCount - 提取的简单三元组的总数
 */
typedef struct
{
	/* Source regexp, and color information extracted from it (stage 1)
	 *
	 * 源正则表达式，以及从中提取的颜色信息（第 1 阶段）
	 */
	regex_t    *regex;
	TrgmColorInfo *colorInfo;
	int			ncolors;

	/* Expanded graph (stage 2)
	 *
	 * 扩展图（第 2 阶段）
	 */
	HTAB	   *states;
	TrgmState  *initState;
	int			nstates;

	/* Workspace for stage 2
	 *
	 * 第 2 阶段的工作空间
	 */
	List	   *queue;
	List	   *keysQueue;
	int			arcsCount;
	bool		overflowed;

	/* Information about distinct color trigrams in the graph (stage 3)
	 *
	 * 有关图表中不同颜色三元组的信息（第 3 阶段）
	 */
	ColorTrgmInfo *colorTrgms;
	int			colorTrgmsCount;
	int			totalTrgmCount;
} TrgmNFA;

/*
 * Final, compact representation of expanded graph.
 *
 * 扩展图的最终紧凑表示。
 */
typedef struct
{
	int			targetState;	/* index of target state (zero-based) */
	int			colorTrgm;		/* index of color trigram for transition */
} TrgmPackedArc;

typedef struct
{
	int			arcsCount;		/* number of out-arcs for this state */
	TrgmPackedArc *arcs;		/* array of arcsCount packed arcs */
} TrgmPackedState;

/* "typedef struct TrgmPackedGraph TrgmPackedGraph" appears in trgm.h
 *
 * “typedef struct TrgmPackedGraph TrgmPackedGraph”出现在 trgm.h 中
 */
struct TrgmPackedGraph
{
	/*
	 * colorTrigramsCount and colorTrigramGroups contain information about how
	 * trigrams are grouped into color trigrams.  "colorTrigramsCount" is the
	 * count of color trigrams and "colorTrigramGroups" contains number of
	 * simple trigrams for each color trigram.  The array of simple trigrams
	 * (stored separately from this struct) is ordered so that the simple
	 * trigrams for each color trigram are consecutive, and they're in order
	 * by color trigram number.
	 *
	 * colorTrigramsCount 和 colorTrigramGroups 包含有关如何将三元组分组为颜色三元组的信息。  “colorTrigramsCount”是颜色三元组的计数，“colorTrigramGroups”包含每个颜色三元组的简单三元组的数量。  简单三元组的数组（与该结构体分开存储）经过排序，以便每个颜色三元组的简单三元组是连续的，并且它们按颜色三元组编号排序。
	 */
	int			colorTrigramsCount;
	int		   *colorTrigramGroups; /* array of size colorTrigramsCount */

	/*
	 * The states of the simplified NFA.  State number 0 is always initial
	 * state and state number 1 is always final state.
	 *
	 * 简化 NFA 的状态。  状态号 0 始终是初始状态，状态号 1 始终是最终状态。
	 */
	int			statesCount;
	TrgmPackedState *states;	/* array of size statesCount */

	/* Temporary work space for trigramsMatchGraph()
	 *
	 * trigramsMatchGraph() 的临时工作空间
	 */
	bool	   *colorTrigramsActive;	/* array of size colorTrigramsCount */
	bool	   *statesActive;	/* array of size statesCount */
	int		   *statesQueue;	/* array of size statesCount */
};

/*
 * Temporary structure for representing an arc during packaging.
 *
 * 包装过程中用于表示弧线的临时结构。
 */
typedef struct
{
	int			sourceState;
	int			targetState;
	int			colorTrgm;
} TrgmPackArcInfo;


/* prototypes for private functions
 *
 * 私有函数的原型
 */
static TRGM *createTrgmNFAInternal(regex_t *regex, TrgmPackedGraph **graph,
								   MemoryContext rcontext);
static void RE_compile(regex_t *regex, text *text_re,
					   int cflags, Oid collation);
static void getColorInfo(regex_t *regex, TrgmNFA *trgmNFA);
static int	convertPgWchar(pg_wchar c, trgm_mb_char *result);
static void transformGraph(TrgmNFA *trgmNFA);
static void processState(TrgmNFA *trgmNFA, TrgmState *state);
static void addKey(TrgmNFA *trgmNFA, TrgmState *state, TrgmStateKey *key);
static void addKeyToQueue(TrgmNFA *trgmNFA, TrgmStateKey *key);
static void addArcs(TrgmNFA *trgmNFA, TrgmState *state);
static void addArc(TrgmNFA *trgmNFA, TrgmState *state, TrgmStateKey *key,
				   TrgmColor co, TrgmStateKey *destKey);
static bool validArcLabel(TrgmStateKey *key, TrgmColor co);
static TrgmState *getState(TrgmNFA *trgmNFA, TrgmStateKey *key);
static bool prefixContains(TrgmPrefix *prefix1, TrgmPrefix *prefix2);
static bool selectColorTrigrams(TrgmNFA *trgmNFA);
static TRGM *expandColorTrigrams(TrgmNFA *trgmNFA, MemoryContext rcontext);
static void fillTrgm(trgm *ptrgm, trgm_mb_char s[3]);
static void mergeStates(TrgmState *state1, TrgmState *state2);
static int	colorTrgmInfoCmp(const void *p1, const void *p2);
static int	colorTrgmInfoPenaltyCmp(const void *p1, const void *p2);
static TrgmPackedGraph *packGraph(TrgmNFA *trgmNFA, MemoryContext rcontext);
static int	packArcInfoCmp(const void *a1, const void *a2);

#ifdef TRGM_REGEXP_DEBUG
static void printSourceNFA(regex_t *regex, TrgmColorInfo *colors, int ncolors);
static void printTrgmNFA(TrgmNFA *trgmNFA);
static void printTrgmColor(StringInfo buf, TrgmColor co);
static void printTrgmPackedGraph(TrgmPackedGraph *packedGraph, TRGM *trigrams);
#endif


/*
 * Main entry point to process a regular expression.
 *
 * 处理正则表达式的主入口点。
 *
 * Returns an array of trigrams required by the regular expression, or NULL if
 * the regular expression was too complex to analyze.  In addition, a packed
 * graph representation of the regex is returned into *graph.  The results
 * must be allocated in rcontext (which might or might not be the current
 * context).
 *
 * 返回正则表达式所需的三元组数组，如果正则表达式太复杂而无法分析，则返回 NULL。  此外，正则表达式的打包图形表示形式将返回到 *graph 中。  结果必须分配在 rcontext 中（可能是也可能不是当前上下文）。
 */
TRGM *
createTrgmNFA(text *text_re, Oid collation,
			  TrgmPackedGraph **graph, MemoryContext rcontext)
{
	TRGM	   *trg;
	regex_t		regex;
	MemoryContext tmpcontext;
	MemoryContext oldcontext;

	/*
	 * This processing generates a great deal of cruft, which we'd like to
	 * clean up before returning (since this function may be called in a
	 * query-lifespan memory context).  Make a temp context we can work in so
	 * that cleanup is easy.
	 *
	 * 这个处理会产生大量的垃圾，我们希望在返回之前清理掉这些垃圾（因为这个函数可能在查询生命周期内存上下文中被调用）。  创建一个我们可以工作的临时上下文，以便轻松清理。
	 */
	tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "createTrgmNFA temporary context",
									   ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(tmpcontext);

	/*
	 * Stage 1: Compile the regexp into a NFA, using the regexp library.
	 *
	 * 第 1 阶段：使用 regexp 库将 regexp 编译为 NFA。
	 */
#ifdef IGNORECASE
	RE_compile(&regex, text_re,
			   REG_ADVANCED | REG_NOSUB | REG_ICASE, collation);
#else
	RE_compile(&regex, text_re,
			   REG_ADVANCED | REG_NOSUB, collation);
#endif

	trg = createTrgmNFAInternal(&regex, graph, rcontext);

	/* Clean up all the cruft we created (including regex)
	 *
	 * 清理我们创建的所有垃圾（包括正则表达式）
	 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(tmpcontext);

	return trg;
}

/*
 * Body of createTrgmNFA, exclusive of regex compilation/freeing.
 *
 * createTrgmNFA 的主体，不包括正则表达式编译/释放。
 */
static TRGM *
createTrgmNFAInternal(regex_t *regex, TrgmPackedGraph **graph,
					  MemoryContext rcontext)
{
	TRGM	   *trg;
	TrgmNFA		trgmNFA;

	trgmNFA.regex = regex;

	/* Collect color information from the regex
	 *
	 * 从正则表达式收集颜色信息
	 */
	getColorInfo(regex, &trgmNFA);

#ifdef TRGM_REGEXP_DEBUG
	printSourceNFA(regex, trgmNFA.colorInfo, trgmNFA.ncolors);
#endif

	/*
	 * Stage 2: Create an expanded graph from the source NFA.
	 *
	 * 第 2 阶段：从源 NFA 创建扩展图。
	 */
	transformGraph(&trgmNFA);

#ifdef TRGM_REGEXP_DEBUG
	printTrgmNFA(&trgmNFA);
#endif

	/*
	 * Fail if we were unable to make a nontrivial graph, ie it is possible to
	 * get from the initial state to the final state without reading any
	 * predictable trigram.
	 *
	 * 如果我们无法制作一个不平凡的图，即可以在不读取任何可预测的三元组的情况下从初始状态到达最终状态，那么就会失败。
	 */
	if (trgmNFA.initState->flags & TSTATE_FIN)
		return NULL;

	/*
	 * Stage 3: Select color trigrams to expand.  Fail if too many trigrams.
	 *
	 * 第 3 阶段：选择要扩展的颜色三元组。  如果卦太多就会失败。
	 */
	if (!selectColorTrigrams(&trgmNFA))
		return NULL;

	/*
	 * Stage 4: Expand color trigrams and pack graph into final
	 * representation.
	 *
	 * 第 4 阶段：扩展颜色三元组并将图表打包为最终表示。
	 */
	trg = expandColorTrigrams(&trgmNFA, rcontext);

	*graph = packGraph(&trgmNFA, rcontext);

#ifdef TRGM_REGEXP_DEBUG
	printTrgmPackedGraph(*graph, trg);
#endif

	return trg;
}

/*
 * Main entry point for evaluating a graph during index scanning.
 *
 * 在索引扫描期间评估图形的主要入口点。
 *
 * The check[] array is indexed by trigram number (in the array of simple
 * trigrams returned by createTrgmNFA), and holds true for those trigrams
 * that are present in the index entry being checked.
 *
 * check[] 数组按三元组编号（在 createTrgmNFA 返回的简单三元组数组中）进行索引，并且对于正在检查的索引条目中存在的那些三元组成立。
 */
bool
trigramsMatchGraph(TrgmPackedGraph *graph, bool *check)
{
	int			i,
				j,
				k,
				queueIn,
				queueOut;

	/*
	 * Reset temporary working areas.
	 *
	 * 重置临时工作区域。
	 */
	memset(graph->colorTrigramsActive, 0,
		   sizeof(bool) * graph->colorTrigramsCount);
	memset(graph->statesActive, 0, sizeof(bool) * graph->statesCount);

	/*
	 * Check which color trigrams were matched.  A match for any simple
	 * trigram associated with a color trigram counts as a match of the color
	 * trigram.
	 *
	 * 检查哪些颜色三元组是匹配的。  与颜色三元组关联的任何简单三元组的匹配都算作颜色三元组的匹配。
	 */
	j = 0;
	for (i = 0; i < graph->colorTrigramsCount; i++)
	{
		int			cnt = graph->colorTrigramGroups[i];

		for (k = j; k < j + cnt; k++)
		{
			if (check[k])
			{
				/*
				 * Found one matched trigram in the group. Can skip the rest
				 * of them and go to the next group.
				 *
				 * 在组中找到一个匹配的卦象。可以跳过其余的并进入下一组。
				 */
				graph->colorTrigramsActive[i] = true;
				break;
			}
		}
		j = j + cnt;
	}

	/*
	 * Initialize the statesQueue to hold just the initial state.  Note:
	 * statesQueue has room for statesCount entries, which is certainly enough
	 * since no state will be put in the queue more than once. The
	 * statesActive array marks which states have been queued.
	 *
	 * 初始化 statesQueue 以仅保存初始状态。  注意：statesQueue 有足够的空间容纳 statesCount 条目，这当然足够了，因为没有状态会被多次放入队列中。 statesActive 数组标记哪些状态已排队。
	 */
	graph->statesActive[0] = true;
	graph->statesQueue[0] = 0;
	queueIn = 0;
	queueOut = 1;

	/* Process queued states as long as there are any.
	 *
	 * 只要有排队状态就处理。
	 */
	while (queueIn < queueOut)
	{
		int			stateno = graph->statesQueue[queueIn++];
		TrgmPackedState *state = &graph->states[stateno];
		int			cnt = state->arcsCount;

		/* Loop over state's out-arcs
		 *
		 * 遍历状态的外弧
		 */
		for (i = 0; i < cnt; i++)
		{
			TrgmPackedArc *arc = &state->arcs[i];

			/*
			 * If corresponding color trigram is present then activate the
			 * corresponding state.  We're done if that's the final state,
			 * otherwise queue the state if it's not been queued already.
			 *
			 * 如果存在相应的颜色三元组，则激活相应的状态。  如果这是最终状态，我们就完成了，否则如果状态尚未排队，则对该状态进行排队。
			 */
			if (graph->colorTrigramsActive[arc->colorTrgm])
			{
				int			nextstate = arc->targetState;

				if (nextstate == 1)
					return true;	/* success: final state is reachable */

				if (!graph->statesActive[nextstate])
				{
					graph->statesActive[nextstate] = true;
					graph->statesQueue[queueOut++] = nextstate;
				}
			}
		}
	}

	/* Queue is empty, so match fails.
	 *
	 * 队列为空，因此匹配失败。
	 */
	return false;
}

/*
 * Compile regex string into struct at *regex.
 * NB: pg_regfree must be applied to regex if this completes successfully.
 *
 * 将正则表达式字符串编译到 *regex 处的结构中。注意：如果成功完成，则必须将 pg_regfree 应用于正则表达式。
 */
static void
RE_compile(regex_t *regex, text *text_re, int cflags, Oid collation)
{
	int			text_re_len = VARSIZE_ANY_EXHDR(text_re);
	char	   *text_re_val = VARDATA_ANY(text_re);
	pg_wchar   *pattern;
	int			pattern_len;
	int			regcomp_result;
	char		errMsg[100];

	/* Convert pattern string to wide characters
	 *
	 * 将模式字符串转换为宽字符
	 */
	pattern = (pg_wchar *) palloc((text_re_len + 1) * sizeof(pg_wchar));
	pattern_len = pg_mb2wchar_with_len(text_re_val,
									   pattern,
									   text_re_len);

	/* Compile regex
	 *
	 * 编译正则表达式
	 */
	regcomp_result = pg_regcomp(regex,
								pattern,
								pattern_len,
								cflags,
								collation);

	pfree(pattern);

	if (regcomp_result != REG_OKAY)
	{
		/* re didn't compile (no need for pg_regfree, if so)
		 *
		 * re 未编译（不需要 pg_regfree，如果是的话）
		 */
		pg_regerror(regcomp_result, regex, errMsg, sizeof(errMsg));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression: %s", errMsg)));
	}
}


/*---------------------
 * Subroutines for pre-processing the color map (stage 1).
 *
 * 用于预处理颜色图的子例程（第 1 阶段）。
 *---------------------
 */

/*
 * Fill TrgmColorInfo structure for each color using regex export functions.
 *
 * 使用正则表达式导出函数填充每种颜色的 TrgmColorInfo 结构。
 */
static void
getColorInfo(regex_t *regex, TrgmNFA *trgmNFA)
{
	int			colorsCount = pg_reg_getnumcolors(regex);
	int			i;

	trgmNFA->ncolors = colorsCount;
	trgmNFA->colorInfo = (TrgmColorInfo *)
		palloc0(colorsCount * sizeof(TrgmColorInfo));

	/*
	 * Loop over colors, filling TrgmColorInfo about each.  Note we include
	 * WHITE (0) even though we know it'll be reported as non-expandable.
	 *
	 * 循环颜色，填充每个颜色的 TrgmColorInfo。  请注意，我们包括 WHITE (0)，即使我们知道它将被报告为不可扩展。
	 */
	for (i = 0; i < colorsCount; i++)
	{
		TrgmColorInfo *colorInfo = &trgmNFA->colorInfo[i];
		int			charsCount = pg_reg_getnumcharacters(regex, i);
		pg_wchar   *chars;
		int			j;

		if (charsCount < 0 || charsCount > COLOR_COUNT_LIMIT)
		{
			/* Non expandable, or too large to work with
			 *
			 * 不可扩展或太大而无法使用
			 */
			colorInfo->expandable = false;
			continue;
		}

		colorInfo->expandable = true;
		colorInfo->containsNonWord = false;
		colorInfo->wordChars = (trgm_mb_char *)
			palloc(sizeof(trgm_mb_char) * charsCount);
		colorInfo->wordCharsCount = 0;

		/* Extract all the chars in this color
		 *
		 * 提取该颜色的所有字符
		 */
		chars = (pg_wchar *) palloc(sizeof(pg_wchar) * charsCount);
		pg_reg_getcharacters(regex, i, chars, charsCount);

		/*
		 * Convert characters back to multibyte form, and save only those that
		 * are word characters.  Set "containsNonWord" if any non-word
		 * character.  (Note: it'd probably be nicer to keep the chars in
		 * pg_wchar format for now, but ISWORDCHR wants to see multibyte.)
		 *
		 * 将字符转换回多字节形式，并仅保存那些单词字符。  如果有非单词字符，则设置“containsNonWord”。  （注意：目前将字符保留为 pg_wchar 格式可能会更好，但 ISWORDCHR 希望看到多字节。）
		 */
		for (j = 0; j < charsCount; j++)
		{
			trgm_mb_char c;
			int			clen = convertPgWchar(chars[j], &c);

			if (!clen)
				continue;		/* ok to ignore it altogether */
			if (ISWORDCHR(c.bytes, clen))
				colorInfo->wordChars[colorInfo->wordCharsCount++] = c;
			else
				colorInfo->containsNonWord = true;
		}

		pfree(chars);
	}
}

/*
 * Convert pg_wchar to multibyte format.
 * Returns 0 if the character should be ignored completely, else returns its
 * byte length.
 *
 * 将 pg_wchar 转换为多字节格式。如果应完全忽略该字符，则返回 0，否则返回其字节长度。
 */
static int
convertPgWchar(pg_wchar c, trgm_mb_char *result)
{
	/* "s" has enough space for a multibyte character and a trailing NUL
	 *
	 * “s”有足够的空间容纳多字节字符和尾随 NUL
	 */
	char		s[MAX_MULTIBYTE_CHAR_LEN + 1];
	int			clen;

	/*
	 * We can ignore the NUL character, since it can never appear in a PG text
	 * string.  This avoids the need for various special cases when
	 * reconstructing trigrams.
	 *
	 * 我们可以忽略 NUL 字符，因为它永远不会出现在 PG 文本字符串中。  这避免了重建卦时需要各种特殊情况。
	 */
	if (c == 0)
		return 0;

	/* Do the conversion, making sure the result is NUL-terminated
	 *
	 * 进行转换，确保结果以 NUL 结尾
	 */
	memset(s, 0, sizeof(s));
	clen = pg_wchar2mb_with_len(&c, s, 1);

	/*
	 * In IGNORECASE mode, we can ignore uppercase characters.  We assume that
	 * the regex engine generated both uppercase and lowercase equivalents
	 * within each color, since we used the REG_ICASE option; so there's no
	 * need to process the uppercase version.
	 *
	 * 在 IGNORECASE 模式下，我们可以忽略大写字符。  我们假设正则表达式引擎在每种颜色中生成了大写和小写等效项，因为我们使用了 REG_ICASE 选项；所以不需要处理大写版本。
	 *
	 * XXX this code is dependent on the assumption that str_tolower() works
	 * the same as the regex engine's internal case folding machinery.  Might
	 * be wiser to expose pg_wc_tolower and test whether c ==
	 * pg_wc_tolower(c). On the other hand, the trigrams in the index were
	 * created using str_tolower(), so we're probably screwed if there's any
	 * incompatibility anyway.
	 *
	 * XXX 此代码依赖于 str_tolower() 与正则表达式引擎的内部大小写折叠机制相同的假设。  公开 pg_wc_tolower 并测试 c == pg_wc_tolower(c) 是否更明智。另一方面，索引中的三元组是使用 str_tolower() 创建的，因此如果存在任何不兼容性，我们可能会被搞砸。
	 */
#ifdef IGNORECASE
	{
		char	   *lowerCased = str_tolower(s, clen, DEFAULT_COLLATION_OID);

		if (strcmp(lowerCased, s) != 0)
		{
			pfree(lowerCased);
			return 0;
		}
		pfree(lowerCased);
	}
#endif

	/* Fill result with exactly MAX_MULTIBYTE_CHAR_LEN bytes
	 *
	 * 用 MAX_MULTIBYTE_CHAR_LEN 字节精确填充结果
	 */
	memcpy(result->bytes, s, MAX_MULTIBYTE_CHAR_LEN);
	return clen;
}


/*---------------------
 * Subroutines for expanding original NFA graph into a trigram graph (stage 2).
 *
 * 用于将原始 NFA 图扩展为三元图的子例程（第 2 阶段）。
 *---------------------
 */

/*
 * Transform the graph, given a regex and extracted color information.
 *
 * 给定正则表达式和提取的颜色信息，转换图形。
 *
 * We create and process a queue of expanded-graph states until all the states
 * are processed.
 *
 * 我们创建并处理扩展图状态的队列，直到处理完所有状态。
 *
 * This algorithm may be stopped due to resource limitation. In this case we
 * force every unprocessed branch to immediately finish with matching (this
 * can give us false positives but no false negatives) by marking all
 * unprocessed states as final.
 *
 * 由于资源限制，该算法可能会停止。在这种情况下，我们通过将所有未处理的状态标记为最终状态，强制每个未处理的分支立即完成匹配（这可能会给我们误报，但不会误报）。
 */
static void
transformGraph(TrgmNFA *trgmNFA)
{
	HASHCTL		hashCtl;
	TrgmStateKey initkey;
	TrgmState  *initstate;
	ListCell   *lc;

	/* Initialize this stage's workspace in trgmNFA struct
	 *
	 * 在 trgmNFA 结构中初始化此阶段的工作空间
	 */
	trgmNFA->queue = NIL;
	trgmNFA->keysQueue = NIL;
	trgmNFA->arcsCount = 0;
	trgmNFA->overflowed = false;

	/* Create hashtable for states
	 *
	 * 为状态创建哈希表
	 */
	hashCtl.keysize = sizeof(TrgmStateKey);
	hashCtl.entrysize = sizeof(TrgmState);
	hashCtl.hcxt = CurrentMemoryContext;
	trgmNFA->states = hash_create("Trigram NFA",
								  1024,
								  &hashCtl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	trgmNFA->nstates = 0;

	/* Create initial state: ambiguous prefix, NFA's initial state
	 *
	 * 创建初始状态：模糊前缀，NFA的初始状态
	 */
	MemSet(&initkey, 0, sizeof(initkey));
	initkey.prefix.colors[0] = COLOR_UNKNOWN;
	initkey.prefix.colors[1] = COLOR_UNKNOWN;
	initkey.nstate = pg_reg_getinitialstate(trgmNFA->regex);

	initstate = getState(trgmNFA, &initkey);
	initstate->flags |= TSTATE_INIT;
	trgmNFA->initState = initstate;

	/*
	 * Recursively build the expanded graph by processing queue of states
	 * (breadth-first search).  getState already put initstate in the queue.
	 * Note that getState will append new states to the queue within the loop,
	 * too; this works as long as we don't do repeat fetches using the "lc"
	 * pointer.
	 *
	 * 通过处理状态队列（广度优先搜索）递归地构建扩展图。  getState 已将 initstate 放入队列中。请注意，getState 也会在循环内将新状态追加到队列中；只要我们不使用“lc”指针重复获取，这就可以工作。
	 */
	foreach(lc, trgmNFA->queue)
	{
		TrgmState  *state = (TrgmState *) lfirst(lc);

		/*
		 * If we overflowed then just mark state as final.  Otherwise do
		 * actual processing.
		 *
		 * 如果我们溢出了，那么只需将状态标记为最终状态即可。  否则进行实际处理。
		 */
		if (trgmNFA->overflowed)
			state->flags |= TSTATE_FIN;
		else
			processState(trgmNFA, state);

		/* Did we overflow?
		 *
		 * 我们溢出了吗？
		 */
		if (trgmNFA->arcsCount > MAX_EXPANDED_ARCS ||
			hash_get_num_entries(trgmNFA->states) > MAX_EXPANDED_STATES)
			trgmNFA->overflowed = true;
	}
}

/*
 * Process one state: add enter keys and then add outgoing arcs.
 *
 * 处理一种状态：添加回车键，然后添加外出弧线。
 */
static void
processState(TrgmNFA *trgmNFA, TrgmState *state)
{
	ListCell   *lc;

	/* keysQueue should be NIL already, but make sure
	 *
	 * keyQueue 应该已经为 NIL，但请确保
	 */
	trgmNFA->keysQueue = NIL;

	/*
	 * Add state's own key, and then process all keys added to keysQueue until
	 * queue is finished.  But we can quit if the state gets marked final.
	 *
	 * 添加state自己的key，然后处理添加到keysQueue中的所有key，直到队列完成。  但如果该状态被标记为最终状态，我们就可以退出。
	 */
	addKey(trgmNFA, state, &state->stateKey);
	foreach(lc, trgmNFA->keysQueue)
	{
		TrgmStateKey *key = (TrgmStateKey *) lfirst(lc);

		if (state->flags & TSTATE_FIN)
			break;
		addKey(trgmNFA, state, key);
	}

	/* Release keysQueue to clean up for next cycle
	 *
	 * 释放keysQueue以清理下一个周期
	 */
	list_free(trgmNFA->keysQueue);
	trgmNFA->keysQueue = NIL;

	/*
	 * Add outgoing arcs only if state isn't final (we have no interest in
	 * outgoing arcs if we already match)
	 *
	 * 仅当状态不是最终状态时才添加传出弧（如果我们已经匹配，我们对传出弧不感兴趣）
	 */
	if (!(state->flags & TSTATE_FIN))
		addArcs(trgmNFA, state);
}

/*
 * Add the given enter key into the state's enterKeys list, and determine
 * whether this should result in any further enter keys being added.
 * If so, add those keys to keysQueue so that processState will handle them.
 *
 * 将给定的 Enter 键添加到状态的 EnterKeys 列表中，并确定这是否会导致添加任何其他 Enter 键。如果是这样，请将这些键添加到keysQueue中，以便processState将处理它们。
 *
 * If the enter key is for the NFA's final state, mark state as TSTATE_FIN.
 * This situation means that we can reach the final state from this expanded
 * state without reading any predictable trigram, so we must consider this
 * state as an accepting one.
 *
 * 如果回车键用于 NFA 的最终状态，则将状态标记为 TSTATE_FIN。这种情况意味着我们可以从这个扩展状态到达最终状态，而无需读取任何可预测的三元组，因此我们必须将此状态视为接受状态。
 *
 * The given key could be a duplicate of one already in enterKeys, or be
 * redundant with some enterKeys.  So we check that before doing anything.
 *
 * 给定的键可能与 EnterKeys 中已有的键重复，或者与某些 EnterKeys 是冗余的。  所以我们在做任何事情之前都会检查一下。
 *
 * Note that we don't generate any actual arcs here.  addArcs will do that
 * later, after we have identified all the enter keys for this state.
 *
 * 请注意，我们在这里不生成任何实际的弧。  在我们识别出该状态的所有回车键之后，addArcs 将会在稍后执行此操作。
 */
static void
addKey(TrgmNFA *trgmNFA, TrgmState *state, TrgmStateKey *key)
{
	regex_arc_t *arcs;
	TrgmStateKey destKey;
	ListCell   *cell;
	int			i,
				arcsCount;

	/*
	 * Ensure any pad bytes in destKey are zero, since it may get used as a
	 * hashtable key by getState.
	 *
	 * 确保 destKey 中的任何填充字节均为零，因为它可能会被 getState 用作哈希表键。
	 */
	MemSet(&destKey, 0, sizeof(destKey));

	/*
	 * Compare key to each existing enter key of the state to check for
	 * redundancy.  We can drop either old key(s) or the new key if we find
	 * redundancy.
	 *
	 * 将密钥与状态的每个现有回车键进行比较以检查冗余。  如果发现冗余，我们可以删除旧密钥或新密钥。
	 */
	foreach(cell, state->enterKeys)
	{
		TrgmStateKey *existingKey = (TrgmStateKey *) lfirst(cell);

		if (existingKey->nstate == key->nstate)
		{
			if (prefixContains(&existingKey->prefix, &key->prefix))
			{
				/* This old key already covers the new key. Nothing to do
				 *
				 * 这把旧钥匙已经覆盖了新钥匙。无事可做
				 */
				return;
			}
			if (prefixContains(&key->prefix, &existingKey->prefix))
			{
				/*
				 * The new key covers this old key. Remove the old key, it's
				 * no longer needed once we add this key to the list.
				 *
				 * 新钥匙覆盖了旧钥匙。删除旧密钥，一旦我们将此密钥添加到列表中，就不再需要它了。
				 */
				state->enterKeys = foreach_delete_current(state->enterKeys,
														  cell);
			}
		}
	}

	/* No redundancy, so add this key to the state's list
	 *
	 * 没有冗余，所以将此键添加到状态列表中
	 */
	state->enterKeys = lappend(state->enterKeys, key);

	/* If state is now known final, mark it and we're done
	 *
	 * 如果现在知道状态是最终状态，则标记它，我们就完成了
	 */
	if (key->nstate == pg_reg_getfinalstate(trgmNFA->regex))
	{
		state->flags |= TSTATE_FIN;
		return;
	}

	/*
	 * Loop through all outgoing arcs of the corresponding state in the
	 * original NFA.
	 *
	 * 循环遍历原始NFA中对应状态的所有出局弧。
	 */
	arcsCount = pg_reg_getnumoutarcs(trgmNFA->regex, key->nstate);
	arcs = (regex_arc_t *) palloc(sizeof(regex_arc_t) * arcsCount);
	pg_reg_getoutarcs(trgmNFA->regex, key->nstate, arcs, arcsCount);

	for (i = 0; i < arcsCount; i++)
	{
		regex_arc_t *arc = &arcs[i];

		if (pg_reg_colorisbegin(trgmNFA->regex, arc->co))
		{
			/*
			 * Start of line/string (^).  Trigram extraction treats start of
			 * line same as start of word: double space prefix is added.
			 * Hence, make an enter key showing we can reach the arc
			 * destination with all-blank prefix.
			 *
			 * 行/字符串的开头 (^)。  三元组提取将行开头与词开头相同：添加双空格前缀。因此，按下回车键表明我们可以到达带有全空白前缀的弧线目的地。
			 */
			destKey.prefix.colors[0] = COLOR_BLANK;
			destKey.prefix.colors[1] = COLOR_BLANK;
			destKey.nstate = arc->to;

			/* Add enter key to this state
			 *
			 * 添加回车键到这个状态
			 */
			addKeyToQueue(trgmNFA, &destKey);
		}
		else if (pg_reg_colorisend(trgmNFA->regex, arc->co))
		{
			/*
			 * End of line/string ($).  We must consider this arc as a
			 * transition that doesn't read anything.  The reason for adding
			 * this enter key to the state is that if the arc leads to the
			 * NFA's final state, we must mark this expanded state as final.
			 *
			 * 行/字符串结束 ($)。  我们必须将此弧视为不读取任何内容的过渡。  将这个回车键添加到状态的原因是，如果弧通向NFA的最终状态，我们必须将这个扩展状态标记为最终状态。
			 */
			destKey.prefix.colors[0] = COLOR_UNKNOWN;
			destKey.prefix.colors[1] = COLOR_UNKNOWN;
			destKey.nstate = arc->to;

			/* Add enter key to this state
			 *
			 * 添加回车键到这个状态
			 */
			addKeyToQueue(trgmNFA, &destKey);
		}
		else if (arc->co >= 0)
		{
			/* Regular color (including WHITE)
			 *
			 * 常规颜色（包括白色）
			 */
			TrgmColorInfo *colorInfo = &trgmNFA->colorInfo[arc->co];

			if (colorInfo->expandable)
			{
				if (colorInfo->containsNonWord &&
					!validArcLabel(key, COLOR_BLANK))
				{
					/*
					 * We can reach the arc destination after reading a
					 * non-word character, but the prefix is not something
					 * that addArc will accept with COLOR_BLANK, so no trigram
					 * arc can get made for this transition.  We must make an
					 * enter key to show that the arc destination is
					 * reachable.  Set it up with an all-blank prefix, since
					 * that corresponds to what the trigram extraction code
					 * will do at a word starting boundary.
					 *
					 * 我们可以在读取非单词字符后到达弧目标，但前缀不是 addArc 接受的 COLOR_BLANK 前缀，因此无法为该转换生成三元弧。  我们必须输入回车键来表明弧线目的地是可以到达的。  将其设置为全空白前缀，因为这对应于三元组提取代码在单词起始边界处执行的操作。
					 */
					destKey.prefix.colors[0] = COLOR_BLANK;
					destKey.prefix.colors[1] = COLOR_BLANK;
					destKey.nstate = arc->to;
					addKeyToQueue(trgmNFA, &destKey);
				}

				if (colorInfo->wordCharsCount > 0 &&
					!validArcLabel(key, arc->co))
				{
					/*
					 * We can reach the arc destination after reading a word
					 * character, but the prefix is not something that addArc
					 * will accept, so no trigram arc can get made for this
					 * transition.  We must make an enter key to show that the
					 * arc destination is reachable.  The prefix for the enter
					 * key should reflect the info we have for this arc.
					 *
					 * 我们可以在读取单词字符后到达弧目的地，但前缀不是 addArc 接受的内容，因此无法为该转换生成三元弧。  我们必须输入回车键来表明弧线目的地是可以到达的。  输入键的前缀应该反映我们对此弧线的信息。
					 */
					destKey.prefix.colors[0] = key->prefix.colors[1];
					destKey.prefix.colors[1] = arc->co;
					destKey.nstate = arc->to;
					addKeyToQueue(trgmNFA, &destKey);
				}
			}
			else
			{
				/*
				 * Unexpandable color.  Add enter key with ambiguous prefix,
				 * showing we can reach the destination from this state, but
				 * the preceding colors will be uncertain.  (We do not set the
				 * first prefix color to key->prefix.colors[1], because a
				 * prefix of known followed by unknown is invalid.)
				 *
				 * 不可展开的颜色。  添加带有模糊前缀的回车键，表明从该状态可以到达目的地，但前面的颜色将不确定。  （我们不将第一个前缀颜色设置为 key->prefix.colors[1]，因为known后面跟着unknown的前缀是无效的。）
				 */
				destKey.prefix.colors[0] = COLOR_UNKNOWN;
				destKey.prefix.colors[1] = COLOR_UNKNOWN;
				destKey.nstate = arc->to;
				addKeyToQueue(trgmNFA, &destKey);
			}
		}
		else
		{
			/* RAINBOW: treat as unexpandable color
			 *
			 * RAINBOW：视为不可扩展的颜色
			 */
			destKey.prefix.colors[0] = COLOR_UNKNOWN;
			destKey.prefix.colors[1] = COLOR_UNKNOWN;
			destKey.nstate = arc->to;
			addKeyToQueue(trgmNFA, &destKey);
		}
	}

	pfree(arcs);
}

/*
 * Add copy of given key to keysQueue for later processing.
 *
 * 将给定密钥的副本添加到keysQueue以供以后处理。
 */
static void
addKeyToQueue(TrgmNFA *trgmNFA, TrgmStateKey *key)
{
	TrgmStateKey *keyCopy = (TrgmStateKey *) palloc(sizeof(TrgmStateKey));

	memcpy(keyCopy, key, sizeof(TrgmStateKey));
	trgmNFA->keysQueue = lappend(trgmNFA->keysQueue, keyCopy);
}

/*
 * Add outgoing arcs from given state, whose enter keys are all now known.
 *
 * 从给定状态添加传出弧，其回车键现在都是已知的。
 */
static void
addArcs(TrgmNFA *trgmNFA, TrgmState *state)
{
	TrgmStateKey destKey;
	ListCell   *cell;
	regex_arc_t *arcs;
	int			arcsCount,
				i;

	/*
	 * Ensure any pad bytes in destKey are zero, since it may get used as a
	 * hashtable key by getState.
	 *
	 * 确保 destKey 中的任何填充字节均为零，因为它可能会被 getState 用作哈希表键。
	 */
	MemSet(&destKey, 0, sizeof(destKey));

	/*
	 * Iterate over enter keys associated with this expanded-graph state. This
	 * includes both the state's own stateKey, and any enter keys we added to
	 * it during addKey (which represent expanded-graph states that are not
	 * distinguishable from this one by means of trigrams).  For each such
	 * enter key, examine all the out-arcs of the key's underlying NFA state,
	 * and try to make a trigram arc leading to where the out-arc leads.
	 * (addArc will deal with whether the arc is valid or not.)
	 *
	 * 迭代与此扩展图状态关联的输入键。这包括状态自己的 stateKey 以及我们在 addKey 期间添加到其中的任何输入键（它们表示无法通过三元组与此状态区分开来的扩展图状态）。  对于每个这样的输入键，检查该键的基础 NFA 状态的所有外弧，并尝试制作一个通向外弧所通向的三元弧。 （addArc将处理弧是否有效。）
	 */
	foreach(cell, state->enterKeys)
	{
		TrgmStateKey *key = (TrgmStateKey *) lfirst(cell);

		arcsCount = pg_reg_getnumoutarcs(trgmNFA->regex, key->nstate);
		arcs = (regex_arc_t *) palloc(sizeof(regex_arc_t) * arcsCount);
		pg_reg_getoutarcs(trgmNFA->regex, key->nstate, arcs, arcsCount);

		for (i = 0; i < arcsCount; i++)
		{
			regex_arc_t *arc = &arcs[i];
			TrgmColorInfo *colorInfo;

			/*
			 * Ignore non-expandable colors; addKey already handled the case.
			 *
			 * 忽略不可扩展的颜色； addKey 已经处理了这个案例。
			 *
			 * We need no special check for WHITE or begin/end pseudocolors
			 * here.  We don't need to do any processing for them, and they
			 * will be marked non-expandable since the regex engine will have
			 * reported them that way.  We do have to watch out for RAINBOW,
			 * which has a negative color number.
			 *
			 * 这里我们不需要对白色或开始/结束伪色进行特殊检查。  我们不需要对它们进行任何处理，并且它们将被标记为不可扩展，因为正则表达式引擎将以这种方式报告它们。  我们确实必须留意 RAINBOW，它的色数为负。
			 */
			if (arc->co < 0)
				continue;
			Assert(arc->co < trgmNFA->ncolors);

			colorInfo = &trgmNFA->colorInfo[arc->co];
			if (!colorInfo->expandable)
				continue;

			if (colorInfo->containsNonWord)
			{
				/*
				 * Color includes non-word character(s).
				 *
				 * 颜色包括非单词字符。
				 *
				 * Generate an arc, treating this transition as occurring on
				 * BLANK.  This allows word-ending trigrams to be manufactured
				 * if possible.
				 *
				 * 生成一个圆弧，将此转变视为发生在 BLANK 上。  如果可能的话，这允许制造词尾三元组。
				 */
				destKey.prefix.colors[0] = key->prefix.colors[1];
				destKey.prefix.colors[1] = COLOR_BLANK;
				destKey.nstate = arc->to;

				addArc(trgmNFA, state, key, COLOR_BLANK, &destKey);
			}

			if (colorInfo->wordCharsCount > 0)
			{
				/*
				 * Color includes word character(s).
				 *
				 * 颜色包括文字字符。
				 *
				 * Generate an arc.  Color is pushed into prefix of target
				 * state.
				 *
				 * 生成圆弧。  颜色被推入目标状态的前缀。
				 */
				destKey.prefix.colors[0] = key->prefix.colors[1];
				destKey.prefix.colors[1] = arc->co;
				destKey.nstate = arc->to;

				addArc(trgmNFA, state, key, arc->co, &destKey);
			}
		}

		pfree(arcs);
	}
}

/*
 * Generate an out-arc of the expanded graph, if it's valid and not redundant.
 *
 * 如果有效且不冗余，则生成展开图的外弧。
 *
 * state: expanded-graph state we want to add an out-arc to
 * key: provides prefix colors (key->nstate is not used)
 * co: transition color
 * destKey: identifier for destination state of expanded graph
 *
 * state：扩展图状态，我们要向其添加外弧 key：提供前缀颜色（不使用 key->nstate） co：过渡颜色 destKey：扩展图目标状态的标识符
 */
static void
addArc(TrgmNFA *trgmNFA, TrgmState *state, TrgmStateKey *key,
	   TrgmColor co, TrgmStateKey *destKey)
{
	TrgmArc    *arc;
	ListCell   *cell;

	/* Do nothing if this wouldn't be a valid arc label trigram
	 *
	 * 如果这不是有效的弧标签三元组，则不执行任何操作
	 */
	if (!validArcLabel(key, co))
		return;

	/*
	 * Check if we are going to reach key which is covered by a key which is
	 * already listed in this state.  If so arc is useless: the NFA can bypass
	 * it through a path that doesn't require any predictable trigram, so
	 * whether the arc's trigram is present or not doesn't really matter.
	 *
	 * 检查我们是否要到达被该状态中已列出的密钥覆盖的密钥。  如果是这样的话，弧就没用了：NFA 可以通过不需要任何可预测的三元组的路径绕过它，因此弧的三元组是否存在并不重要。
	 */
	foreach(cell, state->enterKeys)
	{
		TrgmStateKey *existingKey = (TrgmStateKey *) lfirst(cell);

		if (existingKey->nstate == destKey->nstate &&
			prefixContains(&existingKey->prefix, &destKey->prefix))
			return;
	}

	/* Checks were successful, add new arc
	 *
	 * 检查成功，添加新弧
	 */
	arc = (TrgmArc *) palloc(sizeof(TrgmArc));
	arc->target = getState(trgmNFA, destKey);
	arc->ctrgm.colors[0] = key->prefix.colors[0];
	arc->ctrgm.colors[1] = key->prefix.colors[1];
	arc->ctrgm.colors[2] = co;

	state->arcs = lappend(state->arcs, arc);
	trgmNFA->arcsCount++;
}

/*
 * Can we make a valid trigram arc label from the given prefix and arc color?
 *
 * 我们可以根据给定的前缀和弧线颜色制作有效的三元弧线标签吗？
 *
 * This is split out so that tests in addKey and addArc will stay in sync.
 *
 * 这是分开的，以便 addKey 和 addArc 中的测试保持同步。
 */
static bool
validArcLabel(TrgmStateKey *key, TrgmColor co)
{
	/*
	 * We have to know full trigram in order to add outgoing arc.  So we can't
	 * do it if prefix is ambiguous.
	 *
	 * 我们必须知道完整的卦才能添加传出弧。  所以如果前缀不明确我们就不能这样做。
	 */
	if (key->prefix.colors[0] == COLOR_UNKNOWN)
		return false;

	/* If key->prefix.colors[0] isn't unknown, its second color isn't either
	 *
	 * 如果 key->prefix.colors[0] 不是未知的，那么它的第二种颜色也不是未知的
	 */
	Assert(key->prefix.colors[1] != COLOR_UNKNOWN);
	/* And we should not be called with an unknown arc color anytime
	 *
	 * 并且我们不应该在任何时候被称为未知的弧线颜色
	 */
	Assert(co != COLOR_UNKNOWN);

	/*
	 * We don't bother with making arcs representing three non-word
	 * characters, since that's useless for trigram extraction.
	 *
	 * 我们不费心制作代表三个非单词字符的弧，因为这对于三元组提取没有用。
	 */
	if (key->prefix.colors[0] == COLOR_BLANK &&
		key->prefix.colors[1] == COLOR_BLANK &&
		co == COLOR_BLANK)
		return false;

	/*
	 * We also reject nonblank-blank-anything.  The nonblank-blank-nonblank
	 * case doesn't correspond to any trigram the trigram extraction code
	 * would make.  The nonblank-blank-blank case is also not possible with
	 * RPADDING = 1.  (Note that in many cases we'd fail to generate such a
	 * trigram even if it were valid, for example processing "foo bar" will
	 * not result in considering the trigram "o  ".  So if you want to support
	 * RPADDING = 2, there's more to do than just twiddle this test.)
	 *
	 * 我们也拒绝任何非空白的内容。  非空白-空白-非空白情况不对应于三元组提取代码将生成的任何三元组。  RPADDING = 1 也不可能出现非空白-空白-空白的情况。（请注意，在许多情况下，即使它有效，我们也无法生成这样的三元组，例如处理“foo bar”不会导致考虑三元组“o”。因此，如果您想支持 RPADDING = 2，则需要做的不仅仅是调整此测试。）
	 */
	if (key->prefix.colors[0] != COLOR_BLANK &&
		key->prefix.colors[1] == COLOR_BLANK)
		return false;

	/*
	 * Other combinations involving blank are valid, in particular we assume
	 * blank-blank-nonblank is valid, which presumes that LPADDING is 2.
	 *
	 * 其他涉及空白的组合也是有效的，特别是我们假设空白-空白-非空白是有效的，这假定 LPADDING 为 2。
	 *
	 * Note: Using again the example "foo bar", we will not consider the
	 * trigram "  b", though this trigram would be found by the trigram
	 * extraction code.  Since we will find " ba", it doesn't seem worth
	 * trying to hack the algorithm to generate the additional trigram.
	 *
	 * 注意：再次使用示例“foo bar”，我们不会考虑三元组“b”，尽管这个三元组可以通过三元组提取代码找到。  既然我们会找到“ba”，似乎不值得尝试破解算法来生成额外的三元组。
	 */

	/* arc label is valid
	 *
	 * 弧线标签有效
	 */
	return true;
}

/*
 * Get state of expanded graph for given state key,
 * and queue the state for processing if it didn't already exist.
 *
 * 获取给定状态键的扩展图的状态，如果该状态尚不存在，则对该状态进行排队以进行处理。
 */
static TrgmState *
getState(TrgmNFA *trgmNFA, TrgmStateKey *key)
{
	TrgmState  *state;
	bool		found;

	state = (TrgmState *) hash_search(trgmNFA->states, key, HASH_ENTER,
									  &found);
	if (!found)
	{
		/* New state: initialize and queue it
		 *
		 * 新状态：初始化并排队
		 */
		state->arcs = NIL;
		state->enterKeys = NIL;
		state->flags = 0;
		/* states are initially given negative numbers
		 *
		 * 状态最初被赋予负数
		 */
		state->snumber = -(++trgmNFA->nstates);
		state->parent = NULL;
		state->tentFlags = 0;
		state->tentParent = NULL;

		trgmNFA->queue = lappend(trgmNFA->queue, state);
	}
	return state;
}

/*
 * Check if prefix1 "contains" prefix2.
 *
 * 检查 prefix1 是否“包含” prefix2。
 *
 * "contains" means that any exact prefix (with no ambiguity) that satisfies
 * prefix2 also satisfies prefix1.
 *
 * “包含”意味着任何满足 prefix2 的精确前缀（没有歧义）也满足 prefix1。
 */
static bool
prefixContains(TrgmPrefix *prefix1, TrgmPrefix *prefix2)
{
	if (prefix1->colors[1] == COLOR_UNKNOWN)
	{
		/* Fully ambiguous prefix contains everything
		 *
		 * 完全不明确的前缀包含一切
		 */
		return true;
	}
	else if (prefix1->colors[0] == COLOR_UNKNOWN)
	{
		/*
		 * Prefix with only first unknown color contains every prefix with
		 * same second color.
		 *
		 * 仅具有第一个未知颜色的前缀包含具有相同第二个颜色的每个前缀。
		 */
		if (prefix1->colors[1] == prefix2->colors[1])
			return true;
		else
			return false;
	}
	else
	{
		/* Exact prefix contains only the exact same prefix
		 *
		 * 精确前缀仅包含完全相同的前缀
		 */
		if (prefix1->colors[0] == prefix2->colors[0] &&
			prefix1->colors[1] == prefix2->colors[1])
			return true;
		else
			return false;
	}
}


/*---------------------
 * Subroutines for expanding color trigrams into regular trigrams (stage 3).
 *
 * 将颜色三元组扩展为常规三元组的子例程（第 3 阶段）。
 *---------------------
 */

/*
 * Get vector of all color trigrams in graph and select which of them
 * to expand into simple trigrams.
 *
 * 获取图中所有颜色三元组的向量，并选择将其中哪些扩展为简单三元组。
 *
 * Returns true if OK, false if exhausted resource limits.
 *
 * 如果正常则返回 true，如果资源耗尽则返回 false。
 */
static bool
selectColorTrigrams(TrgmNFA *trgmNFA)
{
	HASH_SEQ_STATUS scan_status;
	int			arcsCount = trgmNFA->arcsCount,
				i;
	TrgmState  *state;
	ColorTrgmInfo *colorTrgms;
	int64		totalTrgmCount;
	float4		totalTrgmPenalty;
	int			cnumber;

	/* Collect color trigrams from all arcs
	 *
	 * 从所有弧线收集颜色卦
	 */
	colorTrgms = (ColorTrgmInfo *) palloc0(sizeof(ColorTrgmInfo) * arcsCount);
	trgmNFA->colorTrgms = colorTrgms;

	i = 0;
	hash_seq_init(&scan_status, trgmNFA->states);
	while ((state = (TrgmState *) hash_seq_search(&scan_status)) != NULL)
	{
		ListCell   *cell;

		foreach(cell, state->arcs)
		{
			TrgmArc    *arc = (TrgmArc *) lfirst(cell);
			TrgmArcInfo *arcInfo = (TrgmArcInfo *) palloc(sizeof(TrgmArcInfo));
			ColorTrgmInfo *trgmInfo = &colorTrgms[i];

			arcInfo->source = state;
			arcInfo->target = arc->target;
			trgmInfo->ctrgm = arc->ctrgm;
			trgmInfo->cnumber = -1;
			/* count and penalty will be set below
			 *
			 * 计数和处罚将在下面设置
			 */
			trgmInfo->expanded = true;
			trgmInfo->arcs = list_make1(arcInfo);
			i++;
		}
	}
	Assert(i == arcsCount);

	/* Remove duplicates, merging their arcs lists
	 *
	 * 删除重复项，合并它们的弧列表
	 */
	if (arcsCount >= 2)
	{
		ColorTrgmInfo *p1,
				   *p2;

		/* Sort trigrams to ease duplicate detection
		 *
		 * 对三元组进行排序以简化重复检测
		 */
		qsort(colorTrgms, arcsCount, sizeof(ColorTrgmInfo), colorTrgmInfoCmp);

		/* p1 is probe point, p2 is last known non-duplicate.
		 *
		 * p1 是探测点，p2 是最后已知的非重复点。
		 */
		p2 = colorTrgms;
		for (p1 = colorTrgms + 1; p1 < colorTrgms + arcsCount; p1++)
		{
			if (colorTrgmInfoCmp(p1, p2) > 0)
			{
				p2++;
				*p2 = *p1;
			}
			else
			{
				p2->arcs = list_concat(p2->arcs, p1->arcs);
			}
		}
		trgmNFA->colorTrgmsCount = (p2 - colorTrgms) + 1;
	}
	else
	{
		trgmNFA->colorTrgmsCount = arcsCount;
	}

	/*
	 * Count number of simple trigrams generated by each color trigram, and
	 * also compute a penalty value, which is the number of simple trigrams
	 * times a multiplier that depends on its whitespace content.
	 *
	 * 计算每个颜色三元组生成的简单三元组的数量，并计算惩罚值，该值是简单三元组的数量乘以取决于其空白内容的乘数。
	 *
	 * Note: per-color-trigram counts cannot overflow an int so long as
	 * COLOR_COUNT_LIMIT is not more than the cube root of INT_MAX, ie about
	 * 1290.  However, the grand total totalTrgmCount might conceivably
	 * overflow an int, so we use int64 for that within this routine.  Also,
	 * penalties are calculated in float4 arithmetic to avoid any overflow
	 * worries.
	 *
	 * 注意：只要 COLOR_COUNT_LIMIT 不大于 INT_MAX 的立方根（即约 1290），每个颜色三元组计数就不能溢出 int。但是，总的totalTrgmCount 可能会溢出 int，因此我们在此例程中使用 int64。  此外，惩罚是用 float4 算法计算的，以避免任何溢出的担忧。
	 */
	totalTrgmCount = 0;
	totalTrgmPenalty = 0.0f;
	for (i = 0; i < trgmNFA->colorTrgmsCount; i++)
	{
		ColorTrgmInfo *trgmInfo = &colorTrgms[i];
		int			j,
					count = 1,
					typeIndex = 0;

		for (j = 0; j < 3; j++)
		{
			TrgmColor	c = trgmInfo->ctrgm.colors[j];

			typeIndex *= 2;
			if (c == COLOR_BLANK)
				typeIndex++;
			else
				count *= trgmNFA->colorInfo[c].wordCharsCount;
		}
		trgmInfo->count = count;
		totalTrgmCount += count;
		trgmInfo->penalty = penalties[typeIndex] * (float4) count;
		totalTrgmPenalty += trgmInfo->penalty;
	}

	/* Sort color trigrams in descending order of their penalties
	 *
	 * 按惩罚的降序对颜色卦进行排序
	 */
	qsort(colorTrgms, trgmNFA->colorTrgmsCount, sizeof(ColorTrgmInfo),
		  colorTrgmInfoPenaltyCmp);

	/*
	 * Remove color trigrams from the graph so long as total penalty of color
	 * trigrams exceeds WISH_TRGM_PENALTY.  (If we fail to get down to
	 * WISH_TRGM_PENALTY, it's OK so long as total count is no more than
	 * MAX_TRGM_COUNT.)  We prefer to remove color trigrams with higher
	 * penalty, since those are the most promising for reducing the total
	 * penalty.  When removing a color trigram we have to merge states
	 * connected by arcs labeled with that trigram.  It's necessary to not
	 * merge initial and final states, because our graph becomes useless if
	 * that happens; so we cannot always remove the trigram we'd prefer to.
	 *
	 * 只要颜色三元组的总惩罚超过 WISH_TRGM_PENALTY，就从图表中删除颜色三元组。  （如果我们未能达到 WISH_TRGM_PENALTY，只要总计数不超过 MAX_TRGM_COUNT 就可以了。）我们更喜欢删除惩罚较高的颜色三元组，因为它们最有希望减少总惩罚。  当删除颜色三元组时，我们必须合并由用该三元组标记的弧连接的状态。  有必要不要合并初始状态和最终状态，因为如果发生这种情况，我们的图就变得毫无用处；所以我们不能总是删除我们想要的卦。
	 */
	for (i = 0; i < trgmNFA->colorTrgmsCount; i++)
	{
		ColorTrgmInfo *trgmInfo = &colorTrgms[i];
		bool		canRemove = true;
		ListCell   *cell;

		/* Done if we've reached the target
		 *
		 * 如果我们达到了目标就完成了
		 */
		if (totalTrgmPenalty <= WISH_TRGM_PENALTY)
			break;

#ifdef TRGM_REGEXP_DEBUG
		fprintf(stderr, "considering ctrgm %d %d %d, penalty %f, %d arcs\n",
				trgmInfo->ctrgm.colors[0],
				trgmInfo->ctrgm.colors[1],
				trgmInfo->ctrgm.colors[2],
				trgmInfo->penalty,
				list_length(trgmInfo->arcs));
#endif

		/*
		 * Does any arc of this color trigram connect initial and final
		 * states?	If so we can't remove it.
		 *
		 * 这个颜色三元组的任何弧线是否连接初始状态和最终状态？	如果是这样我们就无法删除它。
		 */
		foreach(cell, trgmInfo->arcs)
		{
			TrgmArcInfo *arcInfo = (TrgmArcInfo *) lfirst(cell);
			TrgmState  *source = arcInfo->source,
					   *target = arcInfo->target;
			int			source_flags,
						target_flags;

#ifdef TRGM_REGEXP_DEBUG
			fprintf(stderr, "examining arc to s%d (%x) from s%d (%x)\n",
					-target->snumber, target->flags,
					-source->snumber, source->flags);
#endif

			/* examine parent states, if any merging has already happened
			 *
			 * 检查父状态，是否已经发生任何合并
			 */
			while (source->parent)
				source = source->parent;
			while (target->parent)
				target = target->parent;

#ifdef TRGM_REGEXP_DEBUG
			fprintf(stderr, " ... after completed merges: to s%d (%x) from s%d (%x)\n",
					-target->snumber, target->flags,
					-source->snumber, source->flags);
#endif

			/* we must also consider merges we are planning right now
			 *
			 * 我们还必须考虑我们现在正在计划的合并
			 */
			source_flags = source->flags | source->tentFlags;
			while (source->tentParent)
			{
				source = source->tentParent;
				source_flags |= source->flags | source->tentFlags;
			}
			target_flags = target->flags | target->tentFlags;
			while (target->tentParent)
			{
				target = target->tentParent;
				target_flags |= target->flags | target->tentFlags;
			}

#ifdef TRGM_REGEXP_DEBUG
			fprintf(stderr, " ... after tentative merges: to s%d (%x) from s%d (%x)\n",
					-target->snumber, target_flags,
					-source->snumber, source_flags);
#endif

			/* would fully-merged state have both INIT and FIN set?
			 *
			 * 完全合并状态会同时设置 INIT 和 FIN 吗？
			 */
			if (((source_flags | target_flags) & (TSTATE_INIT | TSTATE_FIN)) ==
				(TSTATE_INIT | TSTATE_FIN))
			{
				canRemove = false;
				break;
			}

			/* ok so far, so remember planned merge
			 *
			 * 到目前为止还好，所以请记住计划的合并
			 */
			if (source != target)
			{
#ifdef TRGM_REGEXP_DEBUG
				fprintf(stderr, " ... tentatively merging s%d into s%d\n",
						-target->snumber, -source->snumber);
#endif
				target->tentParent = source;
				source->tentFlags |= target_flags;
			}
		}

		/*
		 * We must reset all the tentFlags/tentParent fields before
		 * continuing.  tentFlags could only have become set in states that
		 * are the source or parent or tentative parent of one of the current
		 * arcs; likewise tentParent could only have become set in states that
		 * are the target or parent or tentative parent of one of the current
		 * arcs.  There might be some overlap between those sets, but if we
		 * clear tentFlags in target states as well as source states, we
		 * should be okay even if we visit a state as target before visiting
		 * it as a source.
		 *
		 * 在继续之前，我们必须重置所有 tentFlags/tentParent 字段。  tentFlags 只能在作为当前弧之一的源或父或暂定父的状态中设置；同样，tentParent 只能在作为当前弧之一的目标或父级或临时父级的状态中设置。  这些集合之间可能存在一些重叠，但是如果我们清除目标状态和源状态中的 tentFlags，即使我们在将状态作为源访问之前将其作为目标访问，我们也应该没问题。
		 */
		foreach(cell, trgmInfo->arcs)
		{
			TrgmArcInfo *arcInfo = (TrgmArcInfo *) lfirst(cell);
			TrgmState  *source = arcInfo->source,
					   *target = arcInfo->target;
			TrgmState  *ttarget;

			/* no need to touch previously-merged states
			 *
			 * 无需触及之前合并的状态
			 */
			while (source->parent)
				source = source->parent;
			while (target->parent)
				target = target->parent;

			while (source)
			{
				source->tentFlags = 0;
				source = source->tentParent;
			}

			while ((ttarget = target->tentParent) != NULL)
			{
				target->tentParent = NULL;
				target->tentFlags = 0;	/* in case it was also a source */
				target = ttarget;
			}
		}

		/* Now, move on if we can't drop this trigram
		 *
		 * 现在，如果我们不能放弃这个卦，就继续吧
		 */
		if (!canRemove)
		{
#ifdef TRGM_REGEXP_DEBUG
			fprintf(stderr, " ... not ok to merge\n");
#endif
			continue;
		}

		/* OK, merge states linked by each arc labeled by the trigram
		 *
		 * 好的，合并由三元组标记的每个弧链接的状态
		 */
		foreach(cell, trgmInfo->arcs)
		{
			TrgmArcInfo *arcInfo = (TrgmArcInfo *) lfirst(cell);
			TrgmState  *source = arcInfo->source,
					   *target = arcInfo->target;

			while (source->parent)
				source = source->parent;
			while (target->parent)
				target = target->parent;
			if (source != target)
			{
#ifdef TRGM_REGEXP_DEBUG
				fprintf(stderr, "merging s%d into s%d\n",
						-target->snumber, -source->snumber);
#endif
				mergeStates(source, target);
				/* Assert we didn't merge initial and final states
				 *
				 * 断言我们没有合并初始状态和最终状态
				 */
				Assert((source->flags & (TSTATE_INIT | TSTATE_FIN)) !=
					   (TSTATE_INIT | TSTATE_FIN));
			}
		}

		/* Mark trigram unexpanded, and update totals
		 *
		 * 将三元组标记为未展开，并更新总数
		 */
		trgmInfo->expanded = false;
		totalTrgmCount -= trgmInfo->count;
		totalTrgmPenalty -= trgmInfo->penalty;
	}

	/* Did we succeed in fitting into MAX_TRGM_COUNT?
	 *
	 * 我们是否成功适应了 MAX_TRGM_COUNT？
	 */
	if (totalTrgmCount > MAX_TRGM_COUNT)
		return false;

	trgmNFA->totalTrgmCount = (int) totalTrgmCount;

	/*
	 * Sort color trigrams by colors (will be useful for bsearch in packGraph)
	 * and enumerate the color trigrams that are expanded.
	 *
	 * 按颜色对颜色三元组进行排序（对于 packGraph 中的 bsearch 很有用）并枚举展开的颜色三元组。
	 */
	cnumber = 0;
	qsort(colorTrgms, trgmNFA->colorTrgmsCount, sizeof(ColorTrgmInfo),
		  colorTrgmInfoCmp);
	for (i = 0; i < trgmNFA->colorTrgmsCount; i++)
	{
		if (colorTrgms[i].expanded)
		{
			colorTrgms[i].cnumber = cnumber;
			cnumber++;
		}
	}

	return true;
}

/*
 * Expand selected color trigrams into regular trigrams.
 *
 * 将选定的颜色三元组扩展为常规三元组。
 *
 * Returns the TRGM array to be passed to the index machinery.
 * The array must be allocated in rcontext.
 *
 * 返回要传递给索引机器的 TRGM 数组。该数组必须在 rcontext 中分配。
 */
static TRGM *
expandColorTrigrams(TrgmNFA *trgmNFA, MemoryContext rcontext)
{
	TRGM	   *trg;
	trgm	   *p;
	int			i;
	TrgmColorInfo blankColor;
	trgm_mb_char blankChar;

	/* Set up "blank" color structure containing a single zero character
	 *
	 * 设置包含单个零字符的“空白”颜色结构
	 */
	memset(blankChar.bytes, 0, sizeof(blankChar.bytes));
	blankColor.wordCharsCount = 1;
	blankColor.wordChars = &blankChar;

	/* Construct the trgm array
	 *
	 * 构造 trgm 数组
	 */
	trg = (TRGM *)
		MemoryContextAllocZero(rcontext,
							   TRGMHDRSIZE +
							   trgmNFA->totalTrgmCount * sizeof(trgm));
	trg->flag = ARRKEY;
	SET_VARSIZE(trg, CALCGTSIZE(ARRKEY, trgmNFA->totalTrgmCount));
	p = GETARR(trg);
	for (i = 0; i < trgmNFA->colorTrgmsCount; i++)
	{
		ColorTrgmInfo *colorTrgm = &trgmNFA->colorTrgms[i];
		TrgmColorInfo *c[3];
		trgm_mb_char s[3];
		int			j,
					i1,
					i2,
					i3;

		/* Ignore any unexpanded trigrams ...
		 *
		 * 忽略任何未展开的卦...
		 */
		if (!colorTrgm->expanded)
			continue;

		/* Get colors, substituting the dummy struct for COLOR_BLANK
		 *
		 * 获取颜色，用虚拟结构替换 COLOR_BLANK
		 */
		for (j = 0; j < 3; j++)
		{
			if (colorTrgm->ctrgm.colors[j] != COLOR_BLANK)
				c[j] = &trgmNFA->colorInfo[colorTrgm->ctrgm.colors[j]];
			else
				c[j] = &blankColor;
		}

		/* Iterate over all possible combinations of colors' characters
		 *
		 * 迭代颜色字符的所有可能组合
		 */
		for (i1 = 0; i1 < c[0]->wordCharsCount; i1++)
		{
			s[0] = c[0]->wordChars[i1];
			for (i2 = 0; i2 < c[1]->wordCharsCount; i2++)
			{
				s[1] = c[1]->wordChars[i2];
				for (i3 = 0; i3 < c[2]->wordCharsCount; i3++)
				{
					s[2] = c[2]->wordChars[i3];
					fillTrgm(p, s);
					p++;
				}
			}
		}
	}

	return trg;
}

/*
 * Convert trigram into trgm datatype.
 *
 * 将 trigram 转换为 trgm 数据类型。
 */
static void
fillTrgm(trgm *ptrgm, trgm_mb_char s[3])
{
	char		str[3 * MAX_MULTIBYTE_CHAR_LEN],
			   *p;
	int			i,
				j;

	/* Write multibyte string into "str" (we don't need null termination)
	 *
	 * 将多字节字符串写入“str”（我们不需要空终止）
	 */
	p = str;

	for (i = 0; i < 3; i++)
	{
		if (s[i].bytes[0] != 0)
		{
			for (j = 0; j < MAX_MULTIBYTE_CHAR_LEN && s[i].bytes[j]; j++)
				*p++ = s[i].bytes[j];
		}
		else
		{
			/* Emit a space in place of COLOR_BLANK
			 *
			 * 发出一个空格来代替 COLOR_BLANK
			 */
			*p++ = ' ';
		}
	}

	/* Convert "str" to a standard trigram (possibly hashing it)
	 *
	 * 将“str”转换为标准三元组（可能对其进行哈希处理）
	 */
	compact_trigram(ptrgm, str, p - str);
}

/*
 * Merge two states of graph.
 *
 * 合并图的两个状态。
 */
static void
mergeStates(TrgmState *state1, TrgmState *state2)
{
	Assert(state1 != state2);
	Assert(!state1->parent);
	Assert(!state2->parent);

	/* state1 absorbs state2's flags
	 *
	 * state1吸收state2的标志
	 */
	state1->flags |= state2->flags;

	/* state2, and indirectly all its children, become children of state1
	 *
	 * state2 及其所有子级间接成为 state1 的子级
	 */
	state2->parent = state1;
}

/*
 * Compare function for sorting of color trigrams by their colors.
 *
 * 比较功能，用于按颜色对颜色三元组进行排序。
 */
static int
colorTrgmInfoCmp(const void *p1, const void *p2)
{
	const ColorTrgmInfo *c1 = (const ColorTrgmInfo *) p1;
	const ColorTrgmInfo *c2 = (const ColorTrgmInfo *) p2;

	return memcmp(&c1->ctrgm, &c2->ctrgm, sizeof(ColorTrgm));
}

/*
 * Compare function for sorting color trigrams in descending order of
 * their penalty fields.
 *
 * 比较函数，用于按惩罚字段的降序对颜色三元组进行排序。
 */
static int
colorTrgmInfoPenaltyCmp(const void *p1, const void *p2)
{
	float4		penalty1 = ((const ColorTrgmInfo *) p1)->penalty;
	float4		penalty2 = ((const ColorTrgmInfo *) p2)->penalty;

	if (penalty1 < penalty2)
		return 1;
	else if (penalty1 == penalty2)
		return 0;
	else
		return -1;
}


/*---------------------
 * Subroutines for packing the graph into final representation (stage 4).
 *
 * 用于将图形打包成最终表示的子例程（第 4 阶段）。
 *---------------------
 */

/*
 * Pack expanded graph into final representation.
 *
 * 将扩展图打包成最终表示。
 *
 * The result data must be allocated in rcontext.
 *
 * 结果数据必须分配在rcontext 中。
 */
static TrgmPackedGraph *
packGraph(TrgmNFA *trgmNFA, MemoryContext rcontext)
{
	int			snumber = 2,
				arcIndex,
				arcsCount;
	HASH_SEQ_STATUS scan_status;
	TrgmState  *state;
	TrgmPackArcInfo *arcs;
	TrgmPackedArc *packedArcs;
	TrgmPackedGraph *result;
	int			i,
				j;

	/* Enumerate surviving states, giving init and fin reserved numbers
	 *
	 * 枚举幸存状态，给出 init 和 fin 保留编号
	 */
	hash_seq_init(&scan_status, trgmNFA->states);
	while ((state = (TrgmState *) hash_seq_search(&scan_status)) != NULL)
	{
		while (state->parent)
			state = state->parent;

		if (state->snumber < 0)
		{
			if (state->flags & TSTATE_INIT)
				state->snumber = 0;
			else if (state->flags & TSTATE_FIN)
				state->snumber = 1;
			else
			{
				state->snumber = snumber;
				snumber++;
			}
		}
	}

	/* Collect array of all arcs
	 *
	 * 收集所有弧的数组
	 */
	arcs = (TrgmPackArcInfo *)
		palloc(sizeof(TrgmPackArcInfo) * trgmNFA->arcsCount);
	arcIndex = 0;
	hash_seq_init(&scan_status, trgmNFA->states);
	while ((state = (TrgmState *) hash_seq_search(&scan_status)) != NULL)
	{
		TrgmState  *source = state;
		ListCell   *cell;

		while (source->parent)
			source = source->parent;

		foreach(cell, state->arcs)
		{
			TrgmArc    *arc = (TrgmArc *) lfirst(cell);
			TrgmState  *target = arc->target;

			while (target->parent)
				target = target->parent;

			if (source->snumber != target->snumber)
			{
				ColorTrgmInfo *ctrgm;

				ctrgm = (ColorTrgmInfo *) bsearch(&arc->ctrgm,
												  trgmNFA->colorTrgms,
												  trgmNFA->colorTrgmsCount,
												  sizeof(ColorTrgmInfo),
												  colorTrgmInfoCmp);
				Assert(ctrgm != NULL);
				Assert(ctrgm->expanded);

				arcs[arcIndex].sourceState = source->snumber;
				arcs[arcIndex].targetState = target->snumber;
				arcs[arcIndex].colorTrgm = ctrgm->cnumber;
				arcIndex++;
			}
		}
	}

	/* Sort arcs to ease duplicate detection
	 *
	 * 对弧进行排序以简化重复检测
	 */
	qsort(arcs, arcIndex, sizeof(TrgmPackArcInfo), packArcInfoCmp);

	/* We could have duplicates because states were merged. Remove them.
	 *
	 * 我们可能会有重复项，因为状态已合并。删除它们。
	 */
	if (arcIndex > 1)
	{
		/* p1 is probe point, p2 is last known non-duplicate.
		 *
		 * p1 是探测点，p2 是最后已知的非重复点。
		 */
		TrgmPackArcInfo *p1,
				   *p2;

		p2 = arcs;
		for (p1 = arcs + 1; p1 < arcs + arcIndex; p1++)
		{
			if (packArcInfoCmp(p1, p2) > 0)
			{
				p2++;
				*p2 = *p1;
			}
		}
		arcsCount = (p2 - arcs) + 1;
	}
	else
		arcsCount = arcIndex;

	/* Create packed representation
	 *
	 * 创建打包表示
	 */
	result = (TrgmPackedGraph *)
		MemoryContextAlloc(rcontext, sizeof(TrgmPackedGraph));

	/* Pack color trigrams information
	 *
	 * 打包颜色三元组信息
	 */
	result->colorTrigramsCount = 0;
	for (i = 0; i < trgmNFA->colorTrgmsCount; i++)
	{
		if (trgmNFA->colorTrgms[i].expanded)
			result->colorTrigramsCount++;
	}
	result->colorTrigramGroups = (int *)
		MemoryContextAlloc(rcontext, sizeof(int) * result->colorTrigramsCount);
	j = 0;
	for (i = 0; i < trgmNFA->colorTrgmsCount; i++)
	{
		if (trgmNFA->colorTrgms[i].expanded)
		{
			result->colorTrigramGroups[j] = trgmNFA->colorTrgms[i].count;
			j++;
		}
	}

	/* Pack states and arcs information
	 *
	 * 包状态和弧信息
	 */
	result->statesCount = snumber;
	result->states = (TrgmPackedState *)
		MemoryContextAlloc(rcontext, snumber * sizeof(TrgmPackedState));
	packedArcs = (TrgmPackedArc *)
		MemoryContextAlloc(rcontext, arcsCount * sizeof(TrgmPackedArc));
	j = 0;
	for (i = 0; i < snumber; i++)
	{
		int			cnt = 0;

		result->states[i].arcs = &packedArcs[j];
		while (j < arcsCount && arcs[j].sourceState == i)
		{
			packedArcs[j].targetState = arcs[j].targetState;
			packedArcs[j].colorTrgm = arcs[j].colorTrgm;
			cnt++;
			j++;
		}
		result->states[i].arcsCount = cnt;
	}

	/* Allocate working memory for trigramsMatchGraph()
	 *
	 * 为 trigramsMatchGraph() 分配工作内存
	 */
	result->colorTrigramsActive = (bool *)
		MemoryContextAlloc(rcontext, sizeof(bool) * result->colorTrigramsCount);
	result->statesActive = (bool *)
		MemoryContextAlloc(rcontext, sizeof(bool) * result->statesCount);
	result->statesQueue = (int *)
		MemoryContextAlloc(rcontext, sizeof(int) * result->statesCount);

	return result;
}

/*
 * Comparison function for sorting TrgmPackArcInfos.
 *
 * 用于对 TrgmPackArcInfos 进行排序的比较函数。
 *
 * Compares arcs in following order: sourceState, colorTrgm, targetState.
 *
 * 按以下顺序比较弧：sourceState、colorTrgm、targetState。
 */
static int
packArcInfoCmp(const void *a1, const void *a2)
{
	const TrgmPackArcInfo *p1 = (const TrgmPackArcInfo *) a1;
	const TrgmPackArcInfo *p2 = (const TrgmPackArcInfo *) a2;

	if (p1->sourceState < p2->sourceState)
		return -1;
	if (p1->sourceState > p2->sourceState)
		return 1;
	if (p1->colorTrgm < p2->colorTrgm)
		return -1;
	if (p1->colorTrgm > p2->colorTrgm)
		return 1;
	if (p1->targetState < p2->targetState)
		return -1;
	if (p1->targetState > p2->targetState)
		return 1;
	return 0;
}


/*---------------------
 * Debugging functions
 *
 * 调试功能
 *
 * These are designed to emit GraphViz files.
 *
 * 它们旨在发出 GraphViz 文件。
 *---------------------
 */

#ifdef TRGM_REGEXP_DEBUG

/*
 * Print initial NFA, in regexp library's representation
 *
 * 以正则表达式库的表示形式打印初始 NFA
 */
static void
printSourceNFA(regex_t *regex, TrgmColorInfo *colors, int ncolors)
{
	StringInfoData buf;
	int			nstates = pg_reg_getnumstates(regex);
	int			state;
	int			i;

	initStringInfo(&buf);

	appendStringInfoString(&buf, "\ndigraph sourceNFA {\n");

	for (state = 0; state < nstates; state++)
	{
		regex_arc_t *arcs;
		int			i,
					arcsCount;

		appendStringInfo(&buf, "s%d", state);
		if (pg_reg_getfinalstate(regex) == state)
			appendStringInfoString(&buf, " [shape = doublecircle]");
		appendStringInfoString(&buf, ";\n");

		arcsCount = pg_reg_getnumoutarcs(regex, state);
		arcs = (regex_arc_t *) palloc(sizeof(regex_arc_t) * arcsCount);
		pg_reg_getoutarcs(regex, state, arcs, arcsCount);

		for (i = 0; i < arcsCount; i++)
		{
			appendStringInfo(&buf, "  s%d -> s%d [label = \"%d\"];\n",
							 state, arcs[i].to, arcs[i].co);
		}

		pfree(arcs);
	}

	appendStringInfoString(&buf, " node [shape = point ]; initial;\n");
	appendStringInfo(&buf, " initial -> s%d;\n",
					 pg_reg_getinitialstate(regex));

	/* Print colors
	 *
	 * 打印颜色
	 */
	appendStringInfoString(&buf, " { rank = sink;\n");
	appendStringInfoString(&buf, "  Colors [shape = none, margin=0, label=<\n");

	for (i = 0; i < ncolors; i++)
	{
		TrgmColorInfo *color = &colors[i];
		int			j;

		appendStringInfo(&buf, "<br/>Color %d: ", i);
		if (color->expandable)
		{
			for (j = 0; j < color->wordCharsCount; j++)
			{
				char		s[MAX_MULTIBYTE_CHAR_LEN + 1];

				memcpy(s, color->wordChars[j].bytes, MAX_MULTIBYTE_CHAR_LEN);
				s[MAX_MULTIBYTE_CHAR_LEN] = '\0';
				appendStringInfoString(&buf, s);
			}
		}
		else
			appendStringInfoString(&buf, "not expandable");
		appendStringInfoChar(&buf, '\n');
	}

	appendStringInfoString(&buf, "  >];\n");
	appendStringInfoString(&buf, " }\n");
	appendStringInfoString(&buf, "}\n");

	{
		/* dot -Tpng -o /tmp/source.png < /tmp/source.gv
		 *
		 * 点-Tpng -o /tmp/source.png < /tmp/source.gv
		 */
		FILE	   *fp = fopen("/tmp/source.gv", "w");

		fprintf(fp, "%s", buf.data);
		fclose(fp);
	}

	pfree(buf.data);
}

/*
 * Print expanded graph.
 *
 * 打印展开图。
 */
static void
printTrgmNFA(TrgmNFA *trgmNFA)
{
	StringInfoData buf;
	HASH_SEQ_STATUS scan_status;
	TrgmState  *state;
	TrgmState  *initstate = NULL;

	initStringInfo(&buf);

	appendStringInfoString(&buf, "\ndigraph transformedNFA {\n");

	hash_seq_init(&scan_status, trgmNFA->states);
	while ((state = (TrgmState *) hash_seq_search(&scan_status)) != NULL)
	{
		ListCell   *cell;

		appendStringInfo(&buf, "s%d", -state->snumber);
		if (state->flags & TSTATE_FIN)
			appendStringInfoString(&buf, " [shape = doublecircle]");
		if (state->flags & TSTATE_INIT)
			initstate = state;
		appendStringInfo(&buf, " [label = \"%d\"]", state->stateKey.nstate);
		appendStringInfoString(&buf, ";\n");

		foreach(cell, state->arcs)
		{
			TrgmArc    *arc = (TrgmArc *) lfirst(cell);

			appendStringInfo(&buf, "  s%d -> s%d [label = \"",
							 -state->snumber, -arc->target->snumber);
			printTrgmColor(&buf, arc->ctrgm.colors[0]);
			appendStringInfoChar(&buf, ' ');
			printTrgmColor(&buf, arc->ctrgm.colors[1]);
			appendStringInfoChar(&buf, ' ');
			printTrgmColor(&buf, arc->ctrgm.colors[2]);
			appendStringInfoString(&buf, "\"];\n");
		}
	}

	if (initstate)
	{
		appendStringInfoString(&buf, " node [shape = point ]; initial;\n");
		appendStringInfo(&buf, " initial -> s%d;\n", -initstate->snumber);
	}

	appendStringInfoString(&buf, "}\n");

	{
		/* dot -Tpng -o /tmp/transformed.png < /tmp/transformed.gv
		 *
		 * 点-Tpng -o /tmp/transformed.png < /tmp/transformed.gv
		 */
		FILE	   *fp = fopen("/tmp/transformed.gv", "w");

		fprintf(fp, "%s", buf.data);
		fclose(fp);
	}

	pfree(buf.data);
}

/*
 * Print a TrgmColor readably.
 *
 * 打印可读的 TrgmColor。
 */
static void
printTrgmColor(StringInfo buf, TrgmColor co)
{
	if (co == COLOR_UNKNOWN)
		appendStringInfoChar(buf, 'u');
	else if (co == COLOR_BLANK)
		appendStringInfoChar(buf, 'b');
	else
		appendStringInfo(buf, "%d", (int) co);
}

/*
 * Print final packed representation of trigram-based expanded graph.
 *
 * 打印基于三元组的扩展图的最终打包表示。
 */
static void
printTrgmPackedGraph(TrgmPackedGraph *packedGraph, TRGM *trigrams)
{
	StringInfoData buf;
	trgm	   *p;
	int			i;

	initStringInfo(&buf);

	appendStringInfoString(&buf, "\ndigraph packedGraph {\n");

	for (i = 0; i < packedGraph->statesCount; i++)
	{
		TrgmPackedState *state = &packedGraph->states[i];
		int			j;

		appendStringInfo(&buf, " s%d", i);
		if (i == 1)
			appendStringInfoString(&buf, " [shape = doublecircle]");

		appendStringInfo(&buf, " [label = <s%d>];\n", i);

		for (j = 0; j < state->arcsCount; j++)
		{
			TrgmPackedArc *arc = &state->arcs[j];

			appendStringInfo(&buf, "  s%d -> s%d [label = \"trigram %d\"];\n",
							 i, arc->targetState, arc->colorTrgm);
		}
	}

	appendStringInfoString(&buf, " node [shape = point ]; initial;\n");
	appendStringInfo(&buf, " initial -> s%d;\n", 0);

	/* Print trigrams
	 *
	 * 打印卦象
	 */
	appendStringInfoString(&buf, " { rank = sink;\n");
	appendStringInfoString(&buf, "  Trigrams [shape = none, margin=0, label=<\n");

	p = GETARR(trigrams);
	for (i = 0; i < packedGraph->colorTrigramsCount; i++)
	{
		int			count = packedGraph->colorTrigramGroups[i];
		int			j;

		appendStringInfo(&buf, "<br/>Trigram %d: ", i);

		for (j = 0; j < count; j++)
		{
			if (j > 0)
				appendStringInfoString(&buf, ", ");

			/*
			 * XXX This representation is nice only for all-ASCII trigrams.
			 *
			 * XXX 此表示仅适用于全 ASCII 三元组。
			 */
			appendStringInfo(&buf, "\"%c%c%c\"", (*p)[0], (*p)[1], (*p)[2]);
			p++;
		}
	}

	appendStringInfoString(&buf, "  >];\n");
	appendStringInfoString(&buf, " }\n");
	appendStringInfoString(&buf, "}\n");

	{
		/* dot -Tpng -o /tmp/packed.png < /tmp/packed.gv
		 *
		 * 点-Tpng -o /tmp/packed.png < /tmp/packed.gv
		 */
		FILE	   *fp = fopen("/tmp/packed.gv", "w");

		fprintf(fp, "%s", buf.data);
		fclose(fp);
	}

	pfree(buf.data);
}

#endif							/* TRGM_REGEXP_DEBUG */
