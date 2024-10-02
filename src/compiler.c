/**
 * @file  compiler.c
 * @brief Single-pass bytecode compiler.
 *
 * Kuroko's compiler is still very reminiscent of its CLox roots and uses
 * the same parsing strategy, so if you have read the third chapter of
 * Bob's "Crafting Interpreters", this should should be fairly easy to
 * understand. One important thing that Kuroko's compiler does differently
 * is implement rewinding, which allows for conservative reparsing and
 * recompilation of subexpressions that have already been parsed. This is
 * used to compile ternaries, multiple assignments, and the expression value
 * in generator and comprehension expressions.
 *
 * Kuroko has several levels of parse precedence, including three different
 * levels indicative of assignments. Most expressions start from the TERNARY
 * or COMMA level, but top-level expression statements and assignment values
 * start at the highest level of ASSIGNMENT, which allows for multiple
 * assignment targets. Expressions parsed from the MUST_ASSIGN level are
 * assignment targets in a multiple assignment. Expression parsed from
 * the CAN_ASSIGN level are single assignment targets.
 *
 * String compilation manages escape sequence processing, so string tokens
 * received from the scanner are not directly converted to string constants.
 * F-strings are compiled as expressions generating a regular string.
 *
 * Kuroko's bytecode supports variable operand sizes using paired "short" and
 * "long" opcodes. To ease the output of these opcodes, the EMIT_OPERAND_OP
 * macro will generate the appropriate opcode given an operand.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <kuroko/kuroko.h>
#include <kuroko/compiler.h>
#include <kuroko/memory.h>
#include <kuroko/scanner.h>
#include <kuroko/object.h>
#include <kuroko/debug.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

#include "private.h"
#include "opcode_enum.h"

/**
 * @brief Token parser state.
 *
 * The parser is fairly simplistic, requiring essentially
 * no lookahead. 'previous' is generally the currently-parsed
 * token: whatever was matched by @ref match. 'current' is the
 * token to be parsed, and can be examined with @ref check.
 */
typedef struct {
	KrkToken current;              /**< @brief Token to be parsed. */
	KrkToken previous;             /**< @brief Last token matched, consumed, or advanced over. */
	char hadError;                 /**< @brief Flag indicating if the parser encountered an error. */
	unsigned int eatingWhitespace; /**< @brief Depth of whitespace-ignoring parse functions. */
} Parser;

/**
 * @brief Parse precedence ladder.
 *
 * Lower values (values listed first) bind more loosely than
 * higher values (values listed later).
 */
typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT, /**<  `=` */
	PREC_COMMA,      /**<  `,` */
	PREC_MUST_ASSIGN,/**<  Multple assignment target   */
	PREC_CAN_ASSIGN, /**<  Single assignment target, inside parens */
	PREC_DEL_TARGET, /**<  Like above, but del target list */
	PREC_TERNARY,    /**<  TrueBranch `if` Condition `else` FalseBranch */
	PREC_OR,         /**<  `or` */
	PREC_AND,        /**<  `and` */
	PREC_NOT,        /**<  `not` */
	PREC_COMPARISON, /**<  `< > <= >= in not in` */
	PREC_BITOR,      /**<  `|` */
	PREC_BITXOR,     /**<  `^` */
	PREC_BITAND,     /**<  `&` */
	PREC_SHIFT,      /**<  `<< >>` */
	PREC_SUM,        /**<  `+ -` */
	PREC_TERM,       /**<  `* / %` */
	PREC_FACTOR,     /**<  `+ - ~ !` */
	PREC_EXPONENT,   /**<  `**` */
	PREC_PRIMARY,    /**<  `. () []` */
} Precedence;

/**
 * @brief Expression type.
 *
 * Determines how an expression should be compiled.
 */
typedef enum {
	EXPR_NORMAL,        /**< This expression can not be an assignment target. */
	EXPR_CAN_ASSIGN,    /**< This expression may be an assignment target, check for assignment operators at the end. */
	EXPR_ASSIGN_TARGET, /**< This expression is definitely an assignment target or chained to one. */
	EXPR_DEL_TARGET,    /**< This expression is in the target list of a 'del' statement. */
	EXPR_METHOD_CALL,   /**< This expression is the parameter list of a method call; only used by @ref dot and @ref call */
	EXPR_CLASS_PARAMETERS,
} ExpressionType;

struct RewindState;
struct GlobalState;

/**
 * @brief Subexpression parser function.
 *
 * Used by the parse rule table for infix and prefix expression
 * parser functions. The argument passed is the @ref ExpressionType
 * to compile the expression as.
 */
typedef void (*ParseFn)(struct GlobalState *, int, struct RewindState *);

/**
 * @brief Parse rule table entry.
 *
 * Maps tokens to prefix and infix rules. Precedence values here
 * are for the infix parsing.
 */
typedef struct {
	ParseFn prefix;  /**< @brief Parse function to call when this token appears at the start of an expression. */
	ParseFn infix;   /**< @brief Parse function to call when this token appears after an expression. */
	Precedence precedence; /**< @brief Precedence ordering for Pratt parsing, @ref Precedence */
} ParseRule;

/**
 * @brief Local variable reference.
 *
 * Tracks the names and scope depth of local variables.
 * Locals are mapped to stack locations by their index
 * in the compiler's locals array.
 */
typedef struct {
	KrkToken name;   /**< @brief Token that provided the name for this variable. */
	ssize_t depth;   /**< @brief Stack depth, or -1 if uninitialized. */
	char isCaptured; /**< @brief Flag indicating if the variable is captured by a closure. */
} Local;

/**
 * @brief Closure upvalue reference.
 *
 * Tracks references to local variables from enclosing scopes.
 */
typedef struct {
	size_t index;    /**< @brief Enclosing local index or upvalue index. */
	char   isLocal;  /**< @brief Flag indicating if @ref index is a local or upvalue index. */
	KrkToken name;   /**< @brief Name for direct lookup. Mainly for non-automatically-populated upvalue cells. */
} Upvalue;

/**
 * @brief Function compilation type.
 *
 * Determines the context of the function being compiled,
 * as different kinds of functions have different semantics.
 */
typedef enum {
	TYPE_FUNCTION,          /**< Normal 'def' function. */
	TYPE_MODULE,            /**< Top level of a script. */
	TYPE_METHOD,            /**< Class method with `self` binding. */
	TYPE_INIT,              /**< Class \__init__ */
	TYPE_LAMBDA,            /**< Lambda expression body, must be a single expression. */
	TYPE_STATIC,            /**< Static class method, no `self` binding. */
	TYPE_CLASS,             /**< Class body, not a normal series of declarations. */
	TYPE_CLASSMETHOD,       /**< Class method, binds first argument to the class. */
	TYPE_COROUTINE,         /**< `await def` function. */
	TYPE_COROUTINE_METHOD,  /**< `await def` class method. */
} FunctionType;

/**
 * @brief Linked list of indices.
 *
 * Primarily used to track the indices of class properties
 * so that they can be referenced again later. @ref ind
 * will be the index of an identifier constant.
 */
struct IndexWithNext {
	size_t ind;                   /**< @brief Index of an identifier constant. */
	struct IndexWithNext * next;  /**< @brief Linked list next pointer. */
};

/**
 * @brief Tracks 'break' and 'continue' statements.
 */
struct LoopExit {
	int offset;     /**< @brief Offset of the jump expression to patch. */
	KrkToken token; /**< @brief Token for this exit statement, so its location can be printed in an error message. */
};

/**
 * @brief Subcompiler state.
 *
 * Each function is compiled in its own context, with its
 * own codeobject, locals, type, scopes, etc.
 */
typedef struct Compiler {
	struct Compiler * enclosing;       /**< @brief Enclosing function compiler, or NULL for a module. */
	KrkCodeObject * codeobject;        /**< @brief Bytecode emitter */
	FunctionType type;                 /**< @brief Type of function being compiled. */
	size_t scopeDepth;                 /**< @brief Depth of nested scope blocks. */
	size_t localCount;                 /**< @brief Total number of local variables. */
	size_t localsSpace;                /**< @brief Space in the locals array. */
	Local  * locals;                   /**< @brief Array of local variable references. */
	size_t upvaluesSpace;              /**< @brief Space in the upvalues array. */
	Upvalue * upvalues;                /**< @brief Array of upvalue references. Count is stored in the codeobject. */

	size_t loopLocalCount;             /**< @brief Tracks how many locals to pop off the stack when exiting a loop. */
	size_t breakCount;                 /**< @brief Number of break statements. */
	size_t breakSpace;                 /**< @brief Space in breaks array. */
	struct LoopExit * breaks;          /**< @brief Array of loop exit instruction indices for break statements. */
	size_t continueCount;              /**< @brief Number of continue statements. */
	size_t continueSpace;              /**< @brief Space in continues array. */
	struct LoopExit * continues;       /**< @brief Array of loop exit instruction indices for continue statements. */

	size_t localNameCapacity;          /**< @brief How much space is available in the codeobject's local names table. */

	struct IndexWithNext * properties; /**< @brief Linked list of class property constant indices. */
	struct Compiler * enclosed;        /**< @brief Subcompiler we are enclosing, need for type annotation compilation. */
	size_t annotationCount;            /**< @brief Number of type annotations found while compiling function signature. */

	int delSatisfied;                  /**< @brief Flag indicating if a 'del' target has been completed. */

	size_t optionsFlags;               /**< @brief Special __options__ imports; similar to __future__ in Python */
	int unnamedArgs;                   /**< @brief Number of positional arguments that will not be assignable through keywords */
} Compiler;

#define OPTIONS_FLAG_COMPILE_TIME_BUILTINS    (1 << 0)
#define OPTIONS_FLAG_NO_IMPLICIT_SELF         (1 << 1)

/**
 * @brief Class compilation context.
 *
 * Allows for things like @ref super to be bound correctly.
 * Also allows us to establish qualified names for functions
 * and nested class definitions.
 */
typedef struct ClassCompiler {
	struct ClassCompiler * enclosing; /**< @brief Enclosing class scope. */
	KrkToken name;                    /**< @brief Name of the current class. */
	int hasAnnotations;               /**< @brief Flag indicating if an annotation dictionary has been attached to this class. */
} ClassCompiler;

/**
 * @brief Bytecode emitter backtracking breadcrumb.
 *
 * Records the state of the bytecode emitter so it may be rewound
 * when an expression needs to be re-parsed. Allows us to implement
 * backtracking to compile the inner expression of a comprehension,
 * the left hand side of a ternary, or the targets list of a
 * complex assignment.
 *
 * We rewind the bytecode, line mapping, and constants table,
 * so that we don't keep around duplicate constants or debug info.
 */
typedef struct ChunkRecorder {
	size_t count;      /**< @brief Offset into the bytecode */
	size_t lines;      /**< @brief Offset into the line map */
	size_t constants;  /**< @brief Number of constants in the constants table */
	size_t expressions;/**< @brief Number of expression mapping entries */
} ChunkRecorder;

/**
 * @brief Compiler emit and parse state prior to this expression.
 *
 * Used to rewind the parser for ternary and comma expressions.
 */
typedef struct RewindState {
	ChunkRecorder before;     /**< @brief Bytecode and constant table output offsets. */
	KrkScanner    oldScanner; /**< @brief Scanner cursor state. */
	Parser        oldParser;  /**< @brief Previous/current tokens. */
} RewindState;

typedef struct GlobalState {
	KrkInstance inst;             /**< @protected @brief Base instance */
	Parser parser;                /**< @brief Parser state */
	KrkScanner scanner;           /**< @brief Scanner state */
	Compiler * current;           /**< @brief Current compiler (in-progress code object) state */
	ClassCompiler * currentClass; /**< @brief Current in-progress class definition (or NULL) */
} GlobalState;

static void _GlobalState_gcscan(KrkInstance * _self) {
	struct GlobalState * self = (void*)_self;
	Compiler * compiler = self->current;
	while (compiler != NULL) {
		if (compiler->enclosed && compiler->enclosed->codeobject) krk_markObject((KrkObj*)compiler->enclosed->codeobject);
		krk_markObject((KrkObj*)compiler->codeobject);
		compiler = compiler->enclosing;
	}
}

static void _GlobalState_gcsweep(KrkInstance * _self) {
	/* nothing to do? */
}

#define currentChunk() (&state->current->codeobject->chunk)

#define EMIT_OPERAND_OP(opc, arg) do { if (arg < 256) { emitBytes(opc, arg); } \
	else { emitBytes(opc ## _LONG, arg >> 16); emitBytes(arg >> 8, arg); } } while (0)

static int isMethod(int type) {
	return type == TYPE_METHOD || type == TYPE_INIT || type == TYPE_COROUTINE_METHOD;
}

static int isCoroutine(int type) {
	return type == TYPE_COROUTINE || type == TYPE_COROUTINE_METHOD;
}

static char * calculateQualName(struct GlobalState * state) {
	static char space[1024]; /* We'll just truncate if we need to */
	space[1023] = '\0';
	char * writer = &space[1023];

#define WRITE(s) do { \
	size_t len = strlen(s); \
	if (writer - len < space) goto _exit; \
	writer -= len; \
	memcpy(writer, s, len); \
} while (0)

	WRITE(state->current->codeobject->name->chars);
	/* Go up by _compiler_, ignore class compilers as we don't need them. */
	Compiler * ptr = state->current->enclosing;
	while (ptr->enclosing) { /* Ignores the top level module */
		if (ptr->type != TYPE_CLASS) {
			/* We must be the locals of a function. */
			WRITE("<locals>.");
		}
		WRITE(".");
		WRITE(ptr->codeobject->name->chars);
		ptr = ptr->enclosing;
	}

_exit:
	return writer;
}

#define codeobject_from_chunk_pointer(ptr) ((KrkCodeObject*)((char *)(ptr) - offsetof(KrkCodeObject, chunk)))
static ChunkRecorder recordChunk(KrkChunk * in) {
	return (ChunkRecorder){in->count, in->linesCount, in->constants.count, codeobject_from_chunk_pointer(in)->expressionsCount};
}

static void rewindChunk(KrkChunk * out, ChunkRecorder from) {
	out->count = from.count;
	out->linesCount = from.lines;
	out->constants.count = from.constants;
	codeobject_from_chunk_pointer(out)->expressionsCount = from.expressions;
}
#undef codeobject_from_chunk_pointer

static size_t renameLocal(struct GlobalState * state, size_t ind, KrkToken name);

static void initCompiler(struct GlobalState * state, Compiler * compiler, FunctionType type) {
	compiler->enclosing = state->current;
	state->current = compiler;
	compiler->codeobject = NULL;
	compiler->type = type;
	compiler->scopeDepth = 0;
	compiler->enclosed = NULL;
	compiler->codeobject = krk_newCodeObject();
	compiler->localCount = 0;
	compiler->localsSpace = 8;
	compiler->locals = KRK_GROW_ARRAY(Local,NULL,0,8);
	compiler->upvaluesSpace = 0;
	compiler->upvalues = NULL;
	compiler->breakCount = 0;
	compiler->breakSpace = 0;
	compiler->breaks = NULL;
	compiler->continueCount = 0;
	compiler->continueSpace = 0;
	compiler->continues = NULL;
	compiler->loopLocalCount = 0;
	compiler->localNameCapacity = 0;
	compiler->properties = NULL;
	compiler->annotationCount = 0;
	compiler->delSatisfied = 0;
	compiler->unnamedArgs = 0;
	compiler->optionsFlags = compiler->enclosing ? compiler->enclosing->optionsFlags : 0;

	if (type != TYPE_MODULE) {
		state->current->codeobject->name = krk_copyString(state->parser.previous.start, state->parser.previous.length);
		char * qualname = calculateQualName(state);
		state->current->codeobject->qualname = krk_copyString(qualname, strlen(qualname));
	}

	if (isMethod(type) && !(compiler->optionsFlags & OPTIONS_FLAG_NO_IMPLICIT_SELF)) {
		Local * local = &state->current->locals[state->current->localCount++];
		local->depth = 0;
		local->isCaptured = 0;
		local->name.start = "self";
		local->name.length = 4;
		renameLocal(state, 0, local->name);
		state->current->codeobject->requiredArgs = 1;
		state->current->codeobject->potentialPositionals = 1;
	}

	if (type == TYPE_CLASS) {
		Local * local = &state->current->locals[state->current->localCount++];
		local->depth = 0;
		local->isCaptured = 0;
		local->name.start = "";
		local->name.length = 0;
		renameLocal(state, 0, local->name);
		state->current->codeobject->requiredArgs = 1;
		state->current->codeobject->potentialPositionals = 1;
	}

	if (isCoroutine(type)) state->current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE;
}

static void rememberClassProperty(struct GlobalState * state, size_t ind) {
	struct IndexWithNext * me = malloc(sizeof(struct IndexWithNext));
	me->ind = ind;
	me->next = state->current->properties;
	state->current->properties = me;
}

static void parsePrecedence(struct GlobalState * state, Precedence precedence);
static ParseRule * getRule(KrkTokenType type);

/* These need to be forward declared or the ordering just gets really confusing... */
static void defDeclaration(struct GlobalState * state);
static void asyncDeclaration(struct GlobalState * state, int);
static void statement(struct GlobalState * state);
static void declaration(struct GlobalState * state);
static KrkToken classDeclaration(struct GlobalState * state);
static void declareVariable(struct GlobalState * state);
static void string(struct GlobalState * state, int exprType, RewindState *rewind);
static KrkToken decorator(struct GlobalState * state, size_t level, FunctionType type);
static void complexAssignment(struct GlobalState * state, ChunkRecorder before, KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized, size_t argBefore, size_t argAfter);
static void complexAssignmentTargets(struct GlobalState * state, KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized, size_t argBefore, size_t argAfter);
static int invalidTarget(struct GlobalState * state, int exprType, const char * description);
static void call(struct GlobalState * state, int exprType, RewindState *rewind);

static void finishError(struct GlobalState * state, KrkToken * token) {
	if (!token->linePtr) token->linePtr = token->start;
	size_t i = 0;
	while (token->linePtr[i] && token->linePtr[i] != '\n') i++;

	krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "line",   (KrkObj*)krk_copyString(token->linePtr, i));
	krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "file",   (KrkObj*)currentChunk()->filename);
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "lineno", INTEGER_VAL(token->line));
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "colno",  INTEGER_VAL(token->col));
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "width",  INTEGER_VAL(token->literalWidth));

	if (state->current->codeobject->name) {
		krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "func", (KrkObj*)state->current->codeobject->name);
	} else {
		KrkValue name = NONE_VAL();
		krk_tableGet(&krk_currentThread.module->fields, vm.specialMethodNames[METHOD_NAME], &name);
		krk_attachNamedValue(&AS_INSTANCE(krk_currentThread.currentException)->fields, "func", name);
	}

	state->parser.hadError = 1;
}

#ifdef KRK_NO_DOCUMENTATION
# define raiseSyntaxError(token, ...) do { if (state->parser.hadError) break; krk_runtimeError(vm.exceptions->syntaxError, "syntax error"); finishError(state,token); } while (0)
#else
# define raiseSyntaxError(token, ...) do { if (state->parser.hadError) break; krk_runtimeError(vm.exceptions->syntaxError, __VA_ARGS__); finishError(state,token); } while (0)
#endif

#define error(...) raiseSyntaxError(&state->parser.previous, __VA_ARGS__)
#define errorAtCurrent(...) raiseSyntaxError(&state->parser.current, __VA_ARGS__)

static void _advance(struct GlobalState * state) {
	state->parser.previous = state->parser.current;

	for (;;) {
		state->parser.current = krk_scanToken(&state->scanner);

		if (state->parser.eatingWhitespace &&
			(state->parser.current.type == TOKEN_INDENTATION || state->parser.current.type == TOKEN_EOL)) continue;

		if (state->parser.current.type == TOKEN_RETRY) continue;
		if (state->parser.current.type != TOKEN_ERROR) break;

		errorAtCurrent("%s", state->parser.current.start);
		break;
	}
}

#define advance() _advance(state)

static void _skipToEnd(struct GlobalState * state) {
	while (state->parser.current.type != TOKEN_EOF) advance();
}

#define skipToEnd() _skipToEnd(state)

static void _startEatingWhitespace(struct GlobalState * state) {
	state->parser.eatingWhitespace++;
	if (state->parser.current.type == TOKEN_INDENTATION || state->parser.current.type == TOKEN_EOL) advance();
}

#define startEatingWhitespace() _startEatingWhitespace(state)

static void _stopEatingWhitespace(struct GlobalState * state) {
	if (state->parser.eatingWhitespace == 0) {
		error("Internal scanner error: Invalid nesting of `startEatingWhitespace`/`stopEatingWhitespace` calls.");
	}
	state->parser.eatingWhitespace--;
}

#define stopEatingWhitespace() _stopEatingWhitespace(state)

static void _consume(struct GlobalState * state, KrkTokenType type, const char * message) {
	if (state->parser.current.type == type) {
		advance();
		return;
	}

	if (state->parser.current.type == TOKEN_EOL || state->parser.current.type == TOKEN_EOF) {
		state->parser.current = state->parser.previous;
	}
	errorAtCurrent("%s", message);
}

#define consume(...) _consume(state,__VA_ARGS__)

static int _check(struct GlobalState * state, KrkTokenType type) {
	return state->parser.current.type == type;
}

#define check(t) _check(state,t)

static int _match(struct GlobalState * state, KrkTokenType type) {
	if (!check(type)) return 0;
	advance();
	return 1;
}

#define match(t) _match(state,t)

static int identifiersEqual(KrkToken * a, KrkToken * b) {
	return (a->length == b->length && memcmp(a->start, b->start, a->length) == 0);
}

static KrkToken _syntheticToken(struct GlobalState * state, const char * text) {
	KrkToken token;
	token.start = text;
	token.length = (int)strlen(text);
	token.line = state->parser.previous.line;
	return token;
}

#define syntheticToken(t) _syntheticToken(state,t)

static void _emitByte(struct GlobalState * state, uint8_t byte) {
	krk_writeChunk(currentChunk(), byte, state->parser.previous.line);
}

#define emitByte(b) _emitByte(state,b)

static void _emitBytes(struct GlobalState * state, uint8_t byte1, uint8_t byte2) {
	emitByte(byte1);
	emitByte(byte2);
}

#define emitBytes(a,b) _emitBytes(state,a,b)

static void emitReturn(struct GlobalState * state) {
	if (state->current->type != TYPE_LAMBDA && state->current->type != TYPE_CLASS) {
		emitByte(OP_NONE);
	}
	emitByte(OP_RETURN);
}

static KrkCodeObject * endCompiler(struct GlobalState * state) {
	KrkCodeObject * function = state->current->codeobject;

	for (size_t i = 0; i < function->localNameCount; i++) {
		if (function->localNames[i].deathday == 0) {
			function->localNames[i].deathday = currentChunk()->count;
		}
	}
	function->localNames = KRK_GROW_ARRAY(KrkLocalEntry, function->localNames,
		state->current->localNameCapacity, function->localNameCount); /* Shorten this down for runtime */

	if (state->current->continueCount) { state->parser.previous = state->current->continues[0].token; error("continue without loop"); }
	if (state->current->breakCount) { state->parser.previous = state->current->breaks[0].token; error("break without loop"); }
	emitReturn(state);

	/* Reduce the size of dynamic arrays to their fixed sizes. */
	function->chunk.lines = KRK_GROW_ARRAY(KrkLineMap, function->chunk.lines,
		function->chunk.linesCapacity, function->chunk.linesCount);
	function->chunk.linesCapacity = function->chunk.linesCount;
	function->chunk.code = KRK_GROW_ARRAY(uint8_t, function->chunk.code,
		function->chunk.capacity, function->chunk.count);
	function->chunk.capacity = function->chunk.count;

	function->expressions = KRK_GROW_ARRAY(KrkExpressionsMap, function->expressions,
		function->expressionsCapacity, function->expressionsCount);
	function->expressionsCapacity = function->expressionsCount;

	/* Attach contants for arguments */
	for (int i = 0; i < function->potentialPositionals; ++i) {
		if (i < state->current->unnamedArgs) {
			krk_writeValueArray(&function->positionalArgNames, NONE_VAL());
			continue;
		}
		KrkValue value = OBJECT_VAL(krk_copyString(state->current->locals[i].name.start, state->current->locals[i].name.length));
		krk_push(value);
		krk_writeValueArray(&function->positionalArgNames, value);
		krk_pop();
	}

	size_t args = function->potentialPositionals;
	if (function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
		KrkValue value = OBJECT_VAL(krk_copyString(state->current->locals[args].name.start,
			state->current->locals[args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->positionalArgNames, value);
		krk_pop();
		args++;
	}

	for (int i = 0; i < function->keywordArgs; ++i) {
		KrkValue value = OBJECT_VAL(krk_copyString(state->current->locals[i+args].name.start,
			state->current->locals[i+args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
	}
	args += function->keywordArgs;

	if (function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
		KrkValue value = OBJECT_VAL(krk_copyString(state->current->locals[args].name.start,
			state->current->locals[args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
		args++;
	}

	function->totalArguments = function->potentialPositionals + function->keywordArgs + !!(function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) + !!(function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS);

#if !defined(KRK_NO_DISASSEMBLY) && !defined(KRK_DISABLE_DEBUG)
	if ((krk_currentThread.flags & KRK_THREAD_ENABLE_DISASSEMBLY) && !state->parser.hadError) {
		krk_disassembleCodeObject(stderr, function, function->name ? function->name->chars : "(module)");
	}
#endif

	state->current = state->current->enclosing;
	return function;
}

static void freeCompiler(Compiler * compiler) {
	KRK_FREE_ARRAY(Local,compiler->locals, compiler->localsSpace);
	KRK_FREE_ARRAY(Upvalue,compiler->upvalues, compiler->upvaluesSpace);
	KRK_FREE_ARRAY(struct LoopExit,compiler->breaks, compiler->breakSpace);
	KRK_FREE_ARRAY(struct LoopExit,compiler->continues, compiler->continueSpace);

	while (compiler->properties) {
		void * tmp = compiler->properties;
		compiler->properties = compiler->properties->next;
		free(tmp);
	}
}

static size_t _emitConstant(struct GlobalState * state, KrkValue value) {
	return krk_writeConstant(currentChunk(), value, state->parser.previous.line);
}

#define emitConstant(v) _emitConstant(state,v)

static int isMangleable(const char * name, size_t len) {
	return (len > 2 && name[0] == '_' && name[1] == '_' && name[len-1] != '_' && (len < 4 || name[len-2] != '_'));
}

static ssize_t identifierConstant(struct GlobalState * state, KrkToken * name) {
	if (state->currentClass && isMangleable(name->start, name->length)) {
		/* Mangle it */
		const char * className = state->currentClass->name.start;
		size_t classLength = state->currentClass->name.length;
		while (classLength && *className == '_') {
			classLength--;
			className++;
		}

		struct StringBuilder sb = {0};
		krk_pushStringBuilderFormat(&sb,"_%.*s%.*s",
			(int)classLength, className,
			(int)name->length, name->start);

		return krk_addConstant(currentChunk(), krk_finishStringBuilder(&sb));
	}

	return krk_addConstant(currentChunk(), OBJECT_VAL(krk_copyString(name->start, name->length)));
}

static ssize_t nonidentifierTokenConstant(struct GlobalState * state, KrkToken * name) {
	return krk_addConstant(currentChunk(), OBJECT_VAL(krk_copyString(name->start, name->length)));
}

static ssize_t resolveLocal(struct GlobalState * state, Compiler * compiler, KrkToken * name) {
	for (ssize_t i = compiler->localCount - 1; i >= 0; i--) {
		Local * local = &compiler->locals[i];
		if (identifiersEqual(name, &local->name)) {
			if (local->depth == -1) {
				error("Invalid recursive reference in declaration initializer");
			}
			if (local->depth == -2) {
				continue;
			}
			return i;
		}
	}
	return -1;
}

static size_t renameLocal(struct GlobalState * state, size_t ind, KrkToken name) {
	if (state->current->codeobject->localNameCount + 1 > state->current->localNameCapacity) {
		size_t old = state->current->localNameCapacity;
		state->current->localNameCapacity = KRK_GROW_CAPACITY(old);
		state->current->codeobject->localNames = KRK_GROW_ARRAY(KrkLocalEntry, state->current->codeobject->localNames, old, state->current->localNameCapacity);
	}
	state->current->codeobject->localNames[state->current->codeobject->localNameCount].id = ind;
	state->current->codeobject->localNames[state->current->codeobject->localNameCount].birthday = currentChunk()->count;
	state->current->codeobject->localNames[state->current->codeobject->localNameCount].deathday = 0;
	state->current->codeobject->localNames[state->current->codeobject->localNameCount].name = krk_copyString(name.start, name.length);
	return state->current->codeobject->localNameCount++;
}

static size_t addLocal(struct GlobalState * state, KrkToken name) {
	if (state->current->localCount + 1 > state->current->localsSpace) {
		size_t old = state->current->localsSpace;
		state->current->localsSpace = KRK_GROW_CAPACITY(old);
		state->current->locals = KRK_GROW_ARRAY(Local,state->current->locals,old,state->current->localsSpace);
	}
	size_t out = state->current->localCount;
	Local * local = &state->current->locals[state->current->localCount++];
	local->name = name;
	local->depth = -1;
	local->isCaptured = 0;

	if (name.length) {
		renameLocal(state, out, name);
	}

	return out;
}

static void declareVariable(struct GlobalState * state) {
	if (state->current->scopeDepth == 0) return;
	KrkToken * name = &state->parser.previous;
	/* Detect duplicate definition */
	for (ssize_t i = state->current->localCount - 1; i >= 0; i--) {
		Local * local = &state->current->locals[i];
		if (local->depth != -1 && local->depth < (ssize_t)state->current->scopeDepth) break;
		if (identifiersEqual(name, &local->name)) {
			error("Duplicate definition for local '%.*s' in this scope.", (int)name->literalWidth, name->start);
		}
	}
	addLocal(state, *name);
}

static ssize_t parseVariable(struct GlobalState * state, const char * errorMessage) {
	consume(TOKEN_IDENTIFIER, errorMessage);

	declareVariable(state);
	if (state->current->scopeDepth > 0) return 0;

	if ((state->current->optionsFlags & OPTIONS_FLAG_COMPILE_TIME_BUILTINS) && *state->parser.previous.start != '_') {
		KrkValue value;
		if (krk_tableGet_fast(&vm.builtins->fields, krk_copyString(state->parser.previous.start, state->parser.previous.length), &value)) {
			error("Conflicting declaration of global '%.*s' is invalid when 'compile_time_builtins' is enabled.",
				(int)state->parser.previous.length, state->parser.previous.start);
			return 0;
		}
	}

	return identifierConstant(state, &state->parser.previous);
}

static void markInitialized(struct GlobalState * state) {
	if (state->current->scopeDepth == 0) return;
	state->current->locals[state->current->localCount - 1].depth = state->current->scopeDepth;
}

static size_t anonymousLocal(struct GlobalState * state) {
	size_t val = addLocal(state, syntheticToken(""));
	markInitialized(state);
	return val;
}

static void defineVariable(struct GlobalState * state, size_t global) {
	if (state->current->scopeDepth > 0) {
		markInitialized(state);
		return;
	}

	EMIT_OPERAND_OP(OP_DEFINE_GLOBAL, global);
}

static void number(struct GlobalState * state, int exprType, RewindState *rewind) {
	const char * start = state->parser.previous.start;
	invalidTarget(state, exprType, "literal");

	for (size_t j = 0; j < state->parser.previous.length; ++j) {
		if (start[j] == 'x' || start[j] == 'X') break;
		if (start[j] == '.' || start[j] == 'e' || start[j] == 'E') {
#ifndef KRK_NO_FLOAT
			emitConstant(krk_parse_float(start, state->parser.previous.length));
#else
			error("no float support");
#endif
			return;
		}
	}

	/* If we got here, it's an integer of some sort. */
	KrkValue result = krk_parse_int(start, state->parser.previous.literalWidth, 0);
	if (IS_NONE(result)) {
		error("invalid numeric literal");
		return;
	}
	emitConstant(result);
}

static int _emitJump(struct GlobalState * state, uint8_t opcode) {
	emitByte(opcode);
	emitBytes(0xFF, 0xFF);
	return currentChunk()->count - 2;
}
#define emitJump(o) _emitJump(state,o)

/**
 * @brief Emit over-long jump target.
 *
 * Our jump instructions take only two bytes as operands, as that typically suffices
 * for storing the appropriate forward or backwards offset, without needing to waste
 * lots of bytes for small jumps, or recalculate everything to expand a jump to
 * fit a larger offset. If we *do* get a jump offset that is too big to fit in our
 * available operand space, we replace the whole instruction with one that fetches
 * the desired target, slowly, from a table attached to the codeobject, alongside
 * the original instruction opcode.
 */
static void _emitOverlongJump(struct GlobalState * state, int offset, int jump) {
	KrkCodeObject * co = state->current->codeobject;
	size_t i = 0;
	while (i < co->overlongJumpsCount && co->overlongJumps[i].instructionOffset != (uint32_t)offset) i++;
	if (i == co->overlongJumpsCount) {
		/* Not an existing overlong jump, need to make a new one. */
		if (co->overlongJumpsCount + 1 > co->overlongJumpsCapacity) {
			size_t old = co->overlongJumpsCapacity;
			co->overlongJumpsCapacity = KRK_GROW_CAPACITY(old);
			co->overlongJumps = KRK_GROW_ARRAY(KrkOverlongJump,co->overlongJumps,old,co->overlongJumpsCapacity);
		}
		co->overlongJumps[i].instructionOffset = offset;
		co->overlongJumps[i].originalOpcode = currentChunk()->code[offset-1];
		co->overlongJumpsCount++;
		currentChunk()->code[offset-1] = OP_OVERLONG_JUMP;
	}
	/* Update jump target */
	co->overlongJumps[i].intendedTarget = jump >> 16;
}

static void _patchJump(struct GlobalState * state, int offset) {
	int jump = currentChunk()->count - offset - 2;

	if (jump > 0xFFFF) {
		_emitOverlongJump(state, offset, jump);
	}

	currentChunk()->code[offset] = (jump >> 8) & 0xFF;
	currentChunk()->code[offset + 1] =  (jump) & 0xFF;
}

#define patchJump(o) _patchJump(state,o)

/**
 * @brief Add expression debug information.
 *
 * This allows us to print tracebacks with underlined expressions,
 * implementing Python's PEP 657. We store information on the
 * left and right sides of binary expressions and the "main" token
 * of the expression (the binary operator, usually).
 *
 * This should be called immediately after the relevant opcode is
 * emitted. The mapping applies to only a single opcode, so it should
 * be the one that is going to cause an exception to be raised - be
 * particularly careful not emit this after an @c OP_NOT if it comes
 * after a more relevant opcode.
 *
 * If the token mechanism here doesn't provide sufficient flexibility,
 * artificial tokens can be created to set up the spans.
 */
static void writeExpressionLocation(KrkToken * before, KrkToken * after, KrkToken * current, struct GlobalState * state) {
#ifndef KRK_DISABLE_DEBUG
	/* We can only support the display of underlines when the whole expression is on one line. */
	if (after->linePtr != before->linePtr) return;
	if (after->line != state->parser.previous.line) return;

	/* For the sake of runtime storage, we only store this debug information
	 * when it fits within the first 256 characters of a line, so we can
	 * use uint8_ts to store these. */
	if (before->col > 255 || current->col > 255 ||
	    (current->col + current->literalWidth) > 255 ||
	    (after->col + after->literalWidth) > 255) return;

	krk_debug_addExpression(state->current->codeobject,
		before->col, current->col,
		current->col + current->literalWidth,
		after->col + after->literalWidth);
#endif
}

static void compareChained(struct GlobalState * state, int inner, KrkToken * preceding) {
	KrkTokenType operatorType = state->parser.previous.type;
	if (operatorType == TOKEN_NOT) consume(TOKEN_IN, "'in' must follow infix 'not'");
	int invert = (operatorType == TOKEN_IS && match(TOKEN_NOT));
	KrkToken this = state->parser.previous;
	KrkToken next = state->parser.current;

	ParseRule * rule = getRule(operatorType);
	parsePrecedence(state, (Precedence)(rule->precedence + 1));

	if (getRule(state->parser.current.type)->precedence == PREC_COMPARISON) {
		emitByte(OP_SWAP);
		emitBytes(OP_DUP, 1);
	}

	switch (operatorType) {
		case TOKEN_BANG_EQUAL:    emitByte(OP_EQUAL); invert = 1; break;
		case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
		case TOKEN_GREATER:       emitByte(OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL); break;
		case TOKEN_LESS:          emitByte(OP_LESS); break;
		case TOKEN_LESS_EQUAL:    emitByte(OP_LESS_EQUAL); break;

		case TOKEN_IS: emitByte(OP_IS); break;

		case TOKEN_IN: emitByte(OP_INVOKE_CONTAINS); break;
		case TOKEN_NOT: emitByte(OP_INVOKE_CONTAINS); invert = 1; break;

		default: error("Invalid binary comparison operator?"); break;
	}

	writeExpressionLocation(preceding, &state->parser.previous, &this, state);
	if (invert) emitByte(OP_NOT);

	if (getRule(state->parser.current.type)->precedence == PREC_COMPARISON) {
		size_t exitJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);
		advance();
		compareChained(state, 1, &next);
		patchJump(exitJump);
		if (getRule(state->parser.current.type)->precedence != PREC_COMPARISON) {
			if (!inner) {
				emitBytes(OP_SWAP,OP_POP);
			}
		}
	} else if (inner) {
		emitByte(OP_JUMP);
		emitBytes(0,2);
	}
}

static void compare(struct GlobalState * state, int exprType, RewindState *rewind) {
	compareChained(state, 0, &rewind->oldParser.current);
	invalidTarget(state, exprType, "operator");
}

static void binary(struct GlobalState * state, int exprType, RewindState *rewind) {
	KrkTokenType operatorType = state->parser.previous.type;
	ParseRule * rule = getRule(operatorType);
	KrkToken this = state->parser.previous;
	parsePrecedence(state, (Precedence)(rule->precedence + (rule->precedence != PREC_EXPONENT)));
	invalidTarget(state, exprType, "operator");

	switch (operatorType) {
		case TOKEN_PIPE:        emitByte(OP_BITOR); break;
		case TOKEN_CARET:       emitByte(OP_BITXOR); break;
		case TOKEN_AMPERSAND:   emitByte(OP_BITAND); break;
		case TOKEN_LEFT_SHIFT:  emitByte(OP_SHIFTLEFT); break;
		case TOKEN_RIGHT_SHIFT: emitByte(OP_SHIFTRIGHT); break;

		case TOKEN_PLUS:     emitByte(OP_ADD); break;
		case TOKEN_MINUS:    emitByte(OP_SUBTRACT); break;
		case TOKEN_ASTERISK: emitByte(OP_MULTIPLY); break;
		case TOKEN_POW:      emitByte(OP_POW); break;
		case TOKEN_SOLIDUS:  emitByte(OP_DIVIDE); break;
		case TOKEN_DOUBLE_SOLIDUS: emitByte(OP_FLOORDIV); break;
		case TOKEN_MODULO:   emitByte(OP_MODULO); break;
		case TOKEN_IN:       emitByte(OP_EQUAL); break;
		case TOKEN_AT:       emitByte(OP_MATMUL); break;
		default: return;
	}
	writeExpressionLocation(&rewind->oldParser.current, &state->parser.previous, &this, state);
}

static int matchAssignment(struct GlobalState * state) {
	return (state->parser.current.type >= TOKEN_EQUAL && state->parser.current.type <= TOKEN_MODULO_EQUAL) ? (advance(), 1) : 0;
}

static int checkEndOfDel(struct GlobalState * state) {
	if (check(TOKEN_COMMA) || check(TOKEN_EOL) || check(TOKEN_EOF) || check(TOKEN_SEMICOLON)) {
		state->current->delSatisfied = 1;
		return 1;
	}
	return 0;
}

static int matchComplexEnd(struct GlobalState * state) {
	return match(TOKEN_COMMA) ||
			match(TOKEN_EQUAL) ||
			match(TOKEN_RIGHT_PAREN);
}

static int invalidTarget(struct GlobalState * state, int exprType, const char * description) {
	if (exprType == EXPR_CAN_ASSIGN && matchAssignment(state)) {
		error("Can not assign to %s", description);
		return 0;
	}

	if (exprType == EXPR_DEL_TARGET && checkEndOfDel(state)) {
		error("Can not delete %s", description);
		return 0;
	}

	return 1;
}

static void assignmentValue(struct GlobalState * state) {
	KrkTokenType type = state->parser.previous.type;
	KrkToken left = state->parser.previous;
	if (type == TOKEN_PLUS_PLUS || type == TOKEN_MINUS_MINUS) {
		emitConstant(INTEGER_VAL(1));
	} else {
		parsePrecedence(state, PREC_COMMA); /* But adding a tuple is maybe not defined */
	}

	switch (type) {
		case TOKEN_PIPE_EQUAL:      emitByte(OP_INPLACE_BITOR); break;
		case TOKEN_CARET_EQUAL:     emitByte(OP_INPLACE_BITXOR); break;
		case TOKEN_AMP_EQUAL:       emitByte(OP_INPLACE_BITAND); break;
		case TOKEN_LSHIFT_EQUAL:    emitByte(OP_INPLACE_SHIFTLEFT); break;
		case TOKEN_RSHIFT_EQUAL:    emitByte(OP_INPLACE_SHIFTRIGHT); break;

		case TOKEN_PLUS_EQUAL:      emitByte(OP_INPLACE_ADD); break;
		case TOKEN_PLUS_PLUS:       emitByte(OP_INPLACE_ADD); break;
		case TOKEN_MINUS_EQUAL:     emitByte(OP_INPLACE_SUBTRACT); break;
		case TOKEN_MINUS_MINUS:     emitByte(OP_INPLACE_SUBTRACT); break;
		case TOKEN_ASTERISK_EQUAL:  emitByte(OP_INPLACE_MULTIPLY); break;
		case TOKEN_POW_EQUAL:       emitByte(OP_INPLACE_POW); break;
		case TOKEN_SOLIDUS_EQUAL:   emitByte(OP_INPLACE_DIVIDE); break;
		case TOKEN_DSOLIDUS_EQUAL:  emitByte(OP_INPLACE_FLOORDIV); break;
		case TOKEN_MODULO_EQUAL:    emitByte(OP_INPLACE_MODULO); break;
		case TOKEN_AT_EQUAL:        emitByte(OP_INPLACE_MATMUL); break;

		default:
			error("Unexpected operand in assignment");
			break;
	}
	writeExpressionLocation(&left,&state->parser.previous,&left,state);
}

static void expression(struct GlobalState * state) {
	parsePrecedence(state, PREC_CAN_ASSIGN);
}

static void sliceExpression(struct GlobalState * state) {
	int isSlice = 0;
	if (match(TOKEN_COLON)) {
		emitByte(OP_NONE);
		isSlice = 1;
	} else {
		parsePrecedence(state, PREC_CAN_ASSIGN);
	}
	if (isSlice || match(TOKEN_COLON)) {
		/* We have the start value, which is either something or None */
		if (check(TOKEN_RIGHT_SQUARE) || check(TOKEN_COMMA)) {
			/* foo[x:] */
			emitByte(OP_NONE);
			EMIT_OPERAND_OP(OP_SLICE, 2);
		} else {
			if (check(TOKEN_COLON)) {
				/* foo[x::... */
				emitByte(OP_NONE);
			} else {
				/* foo[x:e... */
				parsePrecedence(state, PREC_CAN_ASSIGN);
			}
			if (match(TOKEN_COLON) && !check(TOKEN_RIGHT_SQUARE) && !check(TOKEN_COMMA)) {
				/* foo[x:e:s] */
				parsePrecedence(state, PREC_CAN_ASSIGN);
				EMIT_OPERAND_OP(OP_SLICE, 3);
			} else {
				/* foo[x:e] */
				EMIT_OPERAND_OP(OP_SLICE, 2);
			}
		}
	}
}

static void getitem(struct GlobalState * state, int exprType, RewindState *rewind) {

	KrkToken *left = &rewind->oldParser.current;
	KrkToken this  = state->parser.previous;

	sliceExpression(state);

	if (match(TOKEN_COMMA)) {
		size_t argCount = 1;
		if (!check(TOKEN_RIGHT_SQUARE)) {
			do {
				sliceExpression(state);
				argCount++;
			} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_SQUARE));
		}
		EMIT_OPERAND_OP(OP_TUPLE, argCount);
	}

	consume(TOKEN_RIGHT_SQUARE, "Expected ']' after index.");
	if (exprType == EXPR_ASSIGN_TARGET) {
		if (matchComplexEnd(state)) {
			EMIT_OPERAND_OP(OP_DUP, 2);
			emitByte(OP_INVOKE_SETTER);
			writeExpressionLocation(left,&state->parser.current,&this,state);
			emitByte(OP_POP);
			return;
		}
		exprType = EXPR_NORMAL;
	}
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		parsePrecedence(state, PREC_ASSIGNMENT);
		emitByte(OP_INVOKE_SETTER);
		writeExpressionLocation(left,&state->parser.previous,&this,state);
	} else if (exprType == EXPR_CAN_ASSIGN && matchAssignment(state)) {
		emitBytes(OP_DUP, 1); /* o e o */
		emitBytes(OP_DUP, 1); /* o e o e */
		emitByte(OP_INVOKE_GETTER); /* o e v */
		writeExpressionLocation(left,&state->parser.previous,&this,state);
		assignmentValue(state); /* o e v a */
		emitByte(OP_INVOKE_SETTER); /* r */
		writeExpressionLocation(left,&state->parser.previous,&this,state);
	} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel(state)) {
		emitByte(OP_INVOKE_DELETE);
		writeExpressionLocation(left,&state->parser.previous,&this,state);
	} else {
		emitByte(OP_INVOKE_GETTER);
		writeExpressionLocation(left,&state->parser.previous,&this,state);
	}
}

static void attributeUnpack(struct GlobalState * state, int exprType) {
	startEatingWhitespace();
	size_t argCount = 0;
	size_t argSpace = 1;
	ssize_t * args  = KRK_GROW_ARRAY(ssize_t,NULL,0,1);

	do {
		if (argSpace < argCount + 1) {
			size_t old = argSpace;
			argSpace = KRK_GROW_CAPACITY(old);
			args = KRK_GROW_ARRAY(ssize_t,args,old,argSpace);
		}
		consume(TOKEN_IDENTIFIER, "Expected attribute name");
		size_t ind = identifierConstant(state, &state->parser.previous);
		args[argCount++] = ind;
	} while (match(TOKEN_COMMA));

	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expected ')' after attribute list");

	if (exprType == EXPR_ASSIGN_TARGET) {
		error("Can not assign to '.(' in multiple target list");
		goto _dotDone;
	}

	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		size_t expressionCount = 0;
		do {
			expressionCount++;
			expression(state);
		} while (match(TOKEN_COMMA));

		if (expressionCount == 1 && argCount > 1) {
			EMIT_OPERAND_OP(OP_UNPACK, argCount);
		} else if (expressionCount > 1 && argCount == 1) {
			EMIT_OPERAND_OP(OP_TUPLE, expressionCount);
		} else if (expressionCount != argCount) {
			error("Invalid assignment to attribute pack");
			goto _dotDone;
		}

		for (size_t i = argCount; i > 0; i--) {
			if (i != 1) {
				emitBytes(OP_DUP, i);
				emitByte(OP_SWAP);
			}
			EMIT_OPERAND_OP(OP_SET_PROPERTY, args[i-1]);
			if (i != 1) {
				emitByte(OP_POP);
			}
		}
	} else {
		for (size_t i = 0; i < argCount; i++) {
			emitBytes(OP_DUP,0);
			EMIT_OPERAND_OP(OP_GET_PROPERTY,args[i]);
			emitByte(OP_SWAP);
		}
		emitByte(OP_POP);
		emitBytes(OP_TUPLE,argCount);
	}

_dotDone:
	KRK_FREE_ARRAY(ssize_t,args,argSpace);
	return;
}

static void dot(struct GlobalState * state, int exprType, RewindState *rewind) {
	if (match(TOKEN_LEFT_PAREN)) {
		attributeUnpack(state, exprType);
		return;
	}
	KrkToken name = state->parser.current;
	KrkToken this = state->parser.previous;
	consume(TOKEN_IDENTIFIER, "Expected property name");
	size_t ind = identifierConstant(state, &state->parser.previous);
	if (exprType == EXPR_ASSIGN_TARGET) {
		if (matchComplexEnd(state)) {
			EMIT_OPERAND_OP(OP_DUP, 1);
			EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
			writeExpressionLocation(&rewind->oldParser.current, &state->parser.previous, &name, state);
			emitByte(OP_POP);
			return;
		}
		exprType = EXPR_NORMAL;
	}
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		parsePrecedence(state, PREC_ASSIGNMENT);
		EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
		writeExpressionLocation(&rewind->oldParser.current, &state->parser.previous, &name, state);
	} else if (exprType == EXPR_CAN_ASSIGN && matchAssignment(state)) {
		emitBytes(OP_DUP, 0); /* Duplicate the object */
		EMIT_OPERAND_OP(OP_GET_PROPERTY, ind);
		writeExpressionLocation(&rewind->oldParser.current, &name, &this, state);
		assignmentValue(state);
		EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
		writeExpressionLocation(&rewind->oldParser.current, &state->parser.previous, &name, state);
	} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel(state)) {
		EMIT_OPERAND_OP(OP_DEL_PROPERTY, ind);
		writeExpressionLocation(&rewind->oldParser.current, &name, &this, state);
	} else if (match(TOKEN_LEFT_PAREN)) {
		EMIT_OPERAND_OP(OP_GET_METHOD, ind);
		writeExpressionLocation(&rewind->oldParser.current, &name, &this, state);
		call(state, EXPR_METHOD_CALL, rewind);
	} else {
		EMIT_OPERAND_OP(OP_GET_PROPERTY, ind);
		writeExpressionLocation(&rewind->oldParser.current, &name, &this, state);
	}
}

static void literal(struct GlobalState * state, int exprType, RewindState *rewind) {
	invalidTarget(state, exprType, "literal");
	switch (state->parser.previous.type) {
		case TOKEN_FALSE: emitByte(OP_FALSE); break;
		case TOKEN_NONE:  emitByte(OP_NONE); break;
		case TOKEN_TRUE:  emitByte(OP_TRUE); break;
		default: return;
	}
}

static void ellipsis(struct GlobalState * state, int exprType, RewindState *rewind) {
	invalidTarget(state, exprType, "literal");
	KrkValue value;
	if (!krk_tableGet_fast(&vm.builtins->fields, S("Ellipsis"), &value)) {
		error("internal compiler error");
		return;
	}
	emitConstant(value);
}

static void typeHintLocal(struct GlobalState * state) {
	state->current->enclosing->enclosed = state->current;
	state->current = state->current->enclosing;
	state->current->enclosed->annotationCount++;
	emitConstant(INTEGER_VAL(state->current->enclosed->codeobject->localNameCount-1));
	parsePrecedence(state, PREC_TERNARY);
	state->current = state->current->enclosed;
	state->current->enclosing->enclosed = NULL;
}

static void letDeclaration(struct GlobalState * state) {
	size_t argCount = 0;
	size_t argSpace = 1;
	ssize_t * args  = KRK_GROW_ARRAY(ssize_t,NULL,0,1);

	do {
		if (argSpace < argCount + 1) {
			size_t old = argSpace;
			argSpace = KRK_GROW_CAPACITY(old);
			args = KRK_GROW_ARRAY(ssize_t,args,old,argSpace);
		}
		ssize_t ind = parseVariable(state, "Expected variable name.");
		if (state->parser.hadError) goto _letDone;
		if (state->current->scopeDepth > 0) {
			/* Need locals space */
			args[argCount++] = state->current->localCount - 1;
		} else {
			args[argCount++] = ind;
		}
		if (check(TOKEN_COLON)) {
			KrkToken name = state->parser.previous;
			match(TOKEN_COLON);
			if (state->current->enclosing) {
				typeHintLocal(state);
			} else {
				KrkToken annotations = syntheticToken("__annotations__");
				size_t ind = identifierConstant(state, &annotations);
				EMIT_OPERAND_OP(OP_GET_GLOBAL, ind);
				emitConstant(OBJECT_VAL(krk_copyString(name.start, name.length)));
				parsePrecedence(state, PREC_TERNARY);
				emitBytes(OP_INVOKE_SETTER, OP_POP);
			}
		}
	} while (match(TOKEN_COMMA));

	if (match(TOKEN_EQUAL)) {
		size_t expressionCount = 0;
		do {
			expressionCount++;
			expression(state);
		} while (match(TOKEN_COMMA));
		if (expressionCount == 1 && argCount > 1) {
			EMIT_OPERAND_OP(OP_UNPACK, argCount);
		} else if (expressionCount == argCount) {
			/* Do nothing */
		} else if (expressionCount > 1 && argCount == 1) {
			EMIT_OPERAND_OP(OP_TUPLE, expressionCount);
		} else {
			error("Invalid sequence unpack in 'let' statement");
			goto _letDone;
		}
	} else {
		/* Need to nil it */
		for (size_t i = 0; i < argCount; ++i) {
			emitByte(OP_NONE);
		}
	}

	if (state->current->scopeDepth == 0) {
		for (size_t i = argCount; i > 0; i--) {
			defineVariable(state, args[i-1]);
		}
	} else {
		for (size_t i = 0; i < argCount; i++) {
			state->current->locals[state->current->localCount - 1 - i].depth = state->current->scopeDepth;
		}
	}

_letDone:
	KRK_FREE_ARRAY(ssize_t,args,argSpace);
	return;
}

static void declaration(struct GlobalState * state) {
	if (check(TOKEN_DEF)) {
		defDeclaration(state);
	} else if (check(TOKEN_CLASS)) {
		KrkToken className = classDeclaration(state);
		size_t classConst = identifierConstant(state, &className);
		state->parser.previous = className;
		declareVariable(state);
		defineVariable(state, classConst);
	} else if (check(TOKEN_AT)) {
		decorator(state, 0, TYPE_FUNCTION);
	} else if (check(TOKEN_ASYNC)) {
		asyncDeclaration(state, 1);
	} else if (match(TOKEN_EOL) || match(TOKEN_EOF)) {
		return;
	} else if (check(TOKEN_INDENTATION)) {
		return;
	} else {
		statement(state);
	}

	if (state->parser.hadError) skipToEnd();
}

static void expressionStatement(struct GlobalState * state) {
	parsePrecedence(state, PREC_ASSIGNMENT);
	emitByte(OP_POP);
}

static void beginScope(struct GlobalState * state) {
	state->current->scopeDepth++;
}

static void endScope(struct GlobalState * state) {
	state->current->scopeDepth--;

	int closeCount = 0;
	int popCount = 0;

	while (state->current->localCount > 0 &&
	       state->current->locals[state->current->localCount - 1].depth > (ssize_t)state->current->scopeDepth) {
		if (state->current->locals[state->current->localCount - 1].isCaptured) {
			if (popCount) {
				if (popCount == 1) emitByte(OP_POP);
				else { EMIT_OPERAND_OP(OP_POP_MANY, popCount); }
				popCount = 0;
			}
			closeCount++;
		} else {
			if (closeCount) {
				if (closeCount == 1) emitByte(OP_CLOSE_UPVALUE);
				else { EMIT_OPERAND_OP(OP_CLOSE_MANY, closeCount); }
				closeCount = 0;
			}
			popCount++;
		}

		for (size_t i = 0; i < state->current->codeobject->localNameCount; i++) {
			if (state->current->codeobject->localNames[i].id == state->current->localCount - 1 &&
				state->current->codeobject->localNames[i].deathday == 0) {
				state->current->codeobject->localNames[i].deathday = (size_t)currentChunk()->count;
			}
		}
		state->current->localCount--;
	}

	if (popCount) {
		if (popCount == 1) emitByte(OP_POP);
		else { EMIT_OPERAND_OP(OP_POP_MANY, popCount); }
	}
	if (closeCount) {
		if (closeCount == 1) emitByte(OP_CLOSE_UPVALUE);
		else { EMIT_OPERAND_OP(OP_CLOSE_MANY, closeCount); }
	}
}

static void block(struct GlobalState * state, size_t indentation, const char * blockName) {
	if (match(TOKEN_EOL)) {
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = state->parser.current.length;
			if (currentIndentation <= indentation) return;
			advance();
			if (!strcmp(blockName,"def") && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
				size_t before = currentChunk()->count;
				string(state, EXPR_NORMAL, NULL);
				/* That wrote to the chunk, rewind it; this should only ever go back two bytes
				 * because this should only happen as the first thing in a function definition,
				 * and thus this _should_ be the first constant and thus opcode + one-byte operand
				 * to OP_CONSTANT, but just to be safe we'll actually use the previous offset... */
				currentChunk()->count = before;
				/* Retreive the docstring from the constant table */
				state->current->codeobject->docstring = AS_STRING(currentChunk()->constants.values[currentChunk()->constants.count-1]);
				consume(TOKEN_EOL,"Garbage after docstring defintion");
				if (!check(TOKEN_INDENTATION) || state->parser.current.length != currentIndentation) {
					error("Expected at least one statement in function with docstring.");
				}
				advance();
			}
			declaration(state);
			while (check(TOKEN_INDENTATION)) {
				if (state->parser.current.length < currentIndentation) break;
				advance();
				declaration(state);
				if (check(TOKEN_EOL)) {
					advance();
				}
				if (state->parser.hadError) skipToEnd();
			};
		}
	} else {
		statement(state);
	}
}

static void doUpvalues(struct GlobalState * state, Compiler * compiler, KrkCodeObject * function) {
	assert(!!function->upvalueCount == !!compiler->upvalues);
	for (size_t i = 0; i < function->upvalueCount; ++i) {
		size_t index = compiler->upvalues[i].index;
		emitByte((compiler->upvalues[i].isLocal) | ((index > 255) ? 2 : 0));
		if (index > 255) {
			emitByte((index >> 16) & 0xFF);
			emitByte((index >> 8) & 0xFF);
		}
		emitByte(index & 0xFF);
	}
}

static void typeHint(struct GlobalState * state, KrkToken name) {
	state->current->enclosing->enclosed = state->current;
	state->current = state->current->enclosing;

	state->current->enclosed->annotationCount++;

	/* Emit name */
	emitConstant(OBJECT_VAL(krk_copyString(name.start, name.length)));
	parsePrecedence(state, PREC_TERNARY);

	state->current = state->current->enclosed;
	state->current->enclosing->enclosed = NULL;
}

static void hideLocal(struct GlobalState * state) {
	state->current->locals[state->current->localCount - 1].depth = -2;
}

static void argumentDefinition(struct GlobalState * state, int hasCollectors) {
	if (match(TOKEN_EQUAL)) {
		/*
		 * We inline default arguments by checking if they are equal
		 * to a sentinel value and replacing them with the requested
		 * argument. This allows us to send None (useful) to override
		 * defaults that are something else. This essentially ends
		 * up as the following at the top of the function:
		 * if param == KWARGS_SENTINEL:
		 *     param = EXPRESSION
		 */
		size_t myLocal = state->current->localCount - 1;
		EMIT_OPERAND_OP(OP_GET_LOCAL, myLocal);
		int jumpIndex = emitJump(OP_TEST_ARG);
		beginScope(state);
		expression(state); /* Read expression */
		EMIT_OPERAND_OP(OP_SET_LOCAL_POP, myLocal);
		endScope(state);
		patchJump(jumpIndex);
		if (hasCollectors) {
			state->current->codeobject->keywordArgs++;
		} else {
			state->current->codeobject->potentialPositionals++;
		}
	} else {
		if (hasCollectors) {
			size_t myLocal = state->current->localCount - 1;
			EMIT_OPERAND_OP(OP_GET_LOCAL, myLocal);
			int jumpIndex = emitJump(OP_TEST_ARG);
			EMIT_OPERAND_OP(OP_MISSING_KW, state->current->codeobject->keywordArgs);
			patchJump(jumpIndex);
			state->current->codeobject->keywordArgs++;
		} else {
			if (state->current->codeobject->potentialPositionals != state->current->codeobject->requiredArgs) {
				error("non-default argument follows default argument");
				return;
			}
			state->current->codeobject->requiredArgs++;
			state->current->codeobject->potentialPositionals++;
		}
	}
}

static void functionPrologue(struct GlobalState * state, Compiler * compiler) {
	KrkCodeObject * func = endCompiler(state);
	if (compiler->annotationCount) {
		EMIT_OPERAND_OP(OP_MAKE_DICT, compiler->annotationCount * 2);
	}
	size_t ind = krk_addConstant(currentChunk(), OBJECT_VAL(func));
	EMIT_OPERAND_OP(OP_CLOSURE, ind);
	doUpvalues(state, compiler, func);
	if (compiler->annotationCount) {
		emitByte(OP_ANNOTATE);
	}
	freeCompiler(compiler);
}

static int argumentList(struct GlobalState * state, FunctionType type) {
	int hasCollectors = 0;
	KrkToken self = syntheticToken("self");

	do {
		if (!(state->current->optionsFlags & OPTIONS_FLAG_NO_IMPLICIT_SELF) &&
				isMethod(type) && check(TOKEN_IDENTIFIER) &&
				identifiersEqual(&state->parser.current, &self)) {
			if (hasCollectors || state->current->codeobject->requiredArgs != 1) {
				errorAtCurrent("Argument name 'self' in a method signature is reserved for the implicit first argument.");
				return 1;
			}
			advance();
			if (type != TYPE_LAMBDA && check(TOKEN_COLON)) {
				KrkToken name = state->parser.previous;
				match(TOKEN_COLON);
				typeHint(state, name);
			}
			if (check(TOKEN_EQUAL)) {
				errorAtCurrent("'self' can not be a default argument.");
				return 1;
			}
			continue;
		}
		if (match(TOKEN_SOLIDUS)) {
			if (hasCollectors || state->current->unnamedArgs || !state->current->codeobject->potentialPositionals) {
				error("Syntax error.");
				return 1;
			}
			state->current->unnamedArgs = state->current->codeobject->potentialPositionals;
			continue;
		}
		if (match(TOKEN_ASTERISK) || check(TOKEN_POW)) {
			if (match(TOKEN_POW)) {
				if (hasCollectors == 2) {
					error("Duplicate ** in parameter list.");
					return 1;
				}
				hasCollectors = 2;
				state->current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS;
			} else {
				if (hasCollectors) {
					error("Syntax error.");
					return 1;
				}
				hasCollectors = 1;
				if (check(TOKEN_COMMA)) {
					continue;
				}
				state->current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS;
			}
			/* Collect a name, specifically "args" or "kwargs" are commont */
			ssize_t paramConstant = parseVariable(state,
				(hasCollectors == 1) ? "Expected parameter name after '*'." : "Expected parameter name after '**'.");
			if (state->parser.hadError) return 1;
			defineVariable(state, paramConstant);
			KrkToken name = state->parser.previous;
			if (!(state->current->optionsFlags & OPTIONS_FLAG_NO_IMPLICIT_SELF) && isMethod(type) && identifiersEqual(&name,&self)) {
				errorAtCurrent("Argument name 'self' in a method signature is reserved for the implicit first argument.");
				return 1;
			}
			if (type != TYPE_LAMBDA && check(TOKEN_COLON)) {
				match(TOKEN_COLON);
				typeHint(state, name);
			}
			/* Make that a valid local for this function */
			size_t myLocal = state->current->localCount - 1;
			EMIT_OPERAND_OP(OP_GET_LOCAL, myLocal);
			/* Check if it's equal to the unset-kwarg-sentinel value */
			int jumpIndex = emitJump(OP_TEST_ARG);
			/* And if it is, set it to the appropriate type */
			beginScope(state);
			if (hasCollectors == 1) EMIT_OPERAND_OP(OP_MAKE_LIST,0);
			else EMIT_OPERAND_OP(OP_MAKE_DICT,0);
			EMIT_OPERAND_OP(OP_SET_LOCAL_POP, myLocal);
			endScope(state);
			/* Otherwise pop the comparison. */
			patchJump(jumpIndex);
			continue;
		}
		if (hasCollectors == 2) {
			error("arguments follow catch-all keyword collector");
			break;
		}
		ssize_t paramConstant = parseVariable(state, "Expected parameter name.");
		if (state->parser.hadError) return 1;
		hideLocal(state);
		if (type != TYPE_LAMBDA && check(TOKEN_COLON)) {
			KrkToken name = state->parser.previous;
			match(TOKEN_COLON);
			typeHint(state, name);
		}
		argumentDefinition(state, hasCollectors);
		defineVariable(state, paramConstant);
	} while (match(TOKEN_COMMA));

	return 0;
}

static void function(struct GlobalState * state, FunctionType type, size_t blockWidth) {
	Compiler compiler;
	initCompiler(state, &compiler, type);
	compiler.codeobject->chunk.filename = compiler.enclosing->codeobject->chunk.filename;

	beginScope(state);

	consume(TOKEN_LEFT_PAREN, "Expected start of parameter list after function name.");
	startEatingWhitespace();
	if (!check(TOKEN_RIGHT_PAREN)) {
		if (argumentList(state, type)) goto _bail;
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expected end of parameter list.");

	if (match(TOKEN_ARROW)) {
		typeHint(state, syntheticToken("return"));
	}

	consume(TOKEN_COLON, "Expected colon after function signature.");
	block(state, blockWidth,"def");
_bail: (void)0;
	functionPrologue(state, &compiler);
}

static void classBody(struct GlobalState * state, size_t blockWidth) {
	if (match(TOKEN_EOL)) {
		return;
	}

	if (check(TOKEN_AT)) {
		/* '@decorator' which should be attached to a method. */
		decorator(state, 0, TYPE_METHOD);
	} else if (match(TOKEN_IDENTIFIER)) {
		/* Class field */
		size_t ind = identifierConstant(state, &state->parser.previous);

		if (check(TOKEN_COLON)) {
			/* Type annotation for field */
			KrkToken name = state->parser.previous;
			match(TOKEN_COLON);
			/* Get __annotations__ from class */
			KrkToken annotations = syntheticToken("__annotations__");
			size_t ind = identifierConstant(state, &annotations);
			if (!state->currentClass->hasAnnotations) {
				EMIT_OPERAND_OP(OP_MAKE_DICT, 0);
				EMIT_OPERAND_OP(OP_SET_NAME, ind);
				state->currentClass->hasAnnotations = 1;
			} else {
				EMIT_OPERAND_OP(OP_GET_NAME, ind);
			}
			emitConstant(OBJECT_VAL(krk_copyString(name.start, name.length)));
			parsePrecedence(state, PREC_TERNARY);
			emitBytes(OP_INVOKE_SETTER, OP_POP);

			/* A class field with a type hint can be valueless */
			if (match(TOKEN_EOL) || match(TOKEN_EOF)) return;
		}

		consume(TOKEN_EQUAL, "Class field must have value.");

		/* Value */
		parsePrecedence(state, PREC_COMMA);

		rememberClassProperty(state, ind);
		EMIT_OPERAND_OP(OP_SET_NAME, ind);
		emitByte(OP_POP);

		if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
			errorAtCurrent("Expected end of line after class attribute declaration");
		}
	} else if (match(TOKEN_PASS)) {
		/* `pass` is just a general empty statement */
		consume(TOKEN_EOL, "Expected end of line after 'pass' in class body.");
	} else {
		/* Must be a function of some sort */
		FunctionType type = TYPE_METHOD;
		if (match(TOKEN_ASYNC)) {
			type = TYPE_COROUTINE_METHOD;
			consume(TOKEN_DEF, "Expected 'def' after 'async'");
		} else if (!match(TOKEN_DEF)) {
			error("Expected method, decorator, or class variable.");
		}
		consume(TOKEN_IDENTIFIER, "Expected method name after 'def'");
		size_t ind = identifierConstant(state, &state->parser.previous);

		static struct CompilerSpecialMethod { const char * name; int type; } compilerSpecialMethods[] = {
			{"__init__", TYPE_INIT},
			{"__class_getitem__", TYPE_CLASSMETHOD},
			{"__init_subclass__", TYPE_CLASSMETHOD},
			{"__new__", TYPE_STATIC},
			{NULL,0}
		};

		for (struct CompilerSpecialMethod * method = compilerSpecialMethods; method->name; method++) {
			if (state->parser.previous.length == strlen(method->name) && memcmp(state->parser.previous.start, method->name, strlen(method->name)) == 0) {
				if (type == TYPE_COROUTINE_METHOD) {
					error("'%s' can not be a coroutine",method->name);
					return;
				}
				type = method->type;
			}
		}

		function(state, type, blockWidth);
		rememberClassProperty(state, ind);
		EMIT_OPERAND_OP(OP_SET_NAME, ind);
		emitByte(OP_POP);
	}
}

#define ATTACH_PROPERTY(propName,how,propValue) do { \
	KrkToken val_tok = syntheticToken(propValue); \
	size_t val_ind = nonidentifierTokenConstant(state, &val_tok); \
	EMIT_OPERAND_OP(how, val_ind); \
	KrkToken name_tok = syntheticToken(propName); \
	size_t name_ind = identifierConstant(state, &name_tok); \
	EMIT_OPERAND_OP(OP_SET_NAME, name_ind); \
	emitByte(OP_POP); \
} while (0)

static size_t addUpvalue(struct GlobalState * state, Compiler * compiler, ssize_t index, int isLocal, KrkToken name);
static KrkToken classDeclaration(struct GlobalState * state) {
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance(); /* Collect the `class` */

	consume(TOKEN_IDENTIFIER, "Expected class name after 'class'.");

	emitByte(OP_PUSH_BUILD_CLASS);

	Compiler subcompiler;
	initCompiler(state, &subcompiler, TYPE_CLASS);
	subcompiler.codeobject->chunk.filename = subcompiler.enclosing->codeobject->chunk.filename;

	beginScope(state);

	KrkToken classNameToken = state->parser.previous;
	assert(addUpvalue(state, state->current, 0, 4, classNameToken) == 0);

	ClassCompiler classCompiler;
	classCompiler.name = state->parser.previous;
	classCompiler.enclosing = state->currentClass;
	state->currentClass = &classCompiler;
	classCompiler.hasAnnotations = 0;

	RewindState parameters = {recordChunk(currentChunk()), krk_tellScanner(&state->scanner), state->parser};

	/* Class parameters */
	if (match(TOKEN_LEFT_PAREN)) {
		int parenDepth = 0;
		while (!check(TOKEN_EOF)) {
			if (check(TOKEN_RIGHT_PAREN) && parenDepth == 0) {
				advance();
				break;
			} else if (match(TOKEN_LEFT_BRACE)) {
				parenDepth++;
			} else if (match(TOKEN_RIGHT_BRACE)) {
				parenDepth--;
			} else {
				advance();
			}
		}
	}

	beginScope(state);

	consume(TOKEN_COLON, "Expected ':' after class.");

	/* Set Class.__module__ to the value of __name__, which is the string
	 * name of the current module. */
	ATTACH_PROPERTY("__module__", OP_GET_GLOBAL, "__name__");
	ATTACH_PROPERTY("__qualname__", OP_CONSTANT, calculateQualName(state));

	if (match(TOKEN_EOL)) {
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = state->parser.current.length;
			if (currentIndentation <= blockWidth) {
				errorAtCurrent("Unexpected indentation level for class");
			}
			advance();
			if (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)) {
				string(state, EXPR_NORMAL, NULL);
				KrkToken doc = syntheticToken("__doc__");
				size_t ind = identifierConstant(state, &doc);
				EMIT_OPERAND_OP(OP_SET_NAME, ind);
				emitByte(OP_POP);
				consume(TOKEN_EOL,"Garbage after docstring defintion");
				if (!check(TOKEN_INDENTATION) || state->parser.current.length != currentIndentation) {
					goto _pop_class;
				}
				advance();
			}
			classBody(state, currentIndentation);
			while (check(TOKEN_INDENTATION)) {
				if (state->parser.current.length < currentIndentation) break;
				advance(); /* Pass the indentation */
				classBody(state, currentIndentation);
			}
			/* Exit from block */
		}
	} /* else empty class (and at end of file?) we'll allow it for now... */
_pop_class:
	state->currentClass = state->currentClass->enclosing;
	KrkCodeObject * makeclass = endCompiler(state);
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(makeclass));

	RewindState afterFunction = {recordChunk(currentChunk()), krk_tellScanner(&state->scanner), state->parser};

	size_t nameInd = nonidentifierTokenConstant(state, &classNameToken);

	krk_rewindScanner(&state->scanner, parameters.oldScanner);
	state->parser = parameters.oldParser;

	EMIT_OPERAND_OP(OP_CLOSURE, indFunc);
	doUpvalues(state, &subcompiler, makeclass);
	freeCompiler(&subcompiler);
	EMIT_OPERAND_OP(OP_CONSTANT, nameInd);

	if (match(TOKEN_LEFT_PAREN)) {
		call(state, EXPR_CLASS_PARAMETERS, NULL);
	} else {
		emitBytes(OP_CALL, 2);
	}

	krk_rewindScanner(&state->scanner, afterFunction.oldScanner);
	state->parser = afterFunction.oldParser;

	return classCompiler.name;
}

static void lambda(struct GlobalState * state, int exprType, RewindState *rewind) {
	Compiler lambdaCompiler;
	state->parser.previous = syntheticToken("<lambda>");
	initCompiler(state, &lambdaCompiler, TYPE_LAMBDA);
	lambdaCompiler.codeobject->chunk.filename = lambdaCompiler.enclosing->codeobject->chunk.filename;
	beginScope(state);

	if (!check(TOKEN_COLON)) {
		if (argumentList(state, TYPE_LAMBDA)) goto _bail;
	}

	consume(TOKEN_COLON, "Expected ':' after lambda arguments");
	expression(state);

_bail:
	functionPrologue(state, &lambdaCompiler);

	invalidTarget(state, exprType, "lambda");
}

static void defDeclaration(struct GlobalState * state) {
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance(); /* Collect the `def` */

	ssize_t global = parseVariable(state, "Expected function name after 'def'.");
	if (state->parser.hadError) return;
	markInitialized(state);
	function(state, TYPE_FUNCTION, blockWidth);
	if (state->parser.hadError) return;
	defineVariable(state, global);
}

static void asyncDeclaration(struct GlobalState * state, int declarationLevel) {
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance(); /* 'async' */

	if (match(TOKEN_DEF)) {
		if (!declarationLevel) {
			error("'async def' not valid here");
			return;
		}
		ssize_t global = parseVariable(state, "Expected coroutine name after 'async def'");
		if (state->parser.hadError) return;
		markInitialized(state);
		function(state, TYPE_COROUTINE, blockWidth);
		if (state->parser.hadError) return;
		defineVariable(state, global);
	} else if (match(TOKEN_FOR)) {
		if (!isCoroutine(state->current->type)) {
			error("'async for' outside of async function");
			return;
		}
		error("'async for' unsupported (GH-12)");
		return;
	} else if (match(TOKEN_WITH)) {
		if (!isCoroutine(state->current->type)) {
			error("'async with' outside of async function");
			return;
		}
		error("'async with' unsupported (GH-12)");
		return;
	} else {
		errorAtCurrent("Expected 'def' after 'async'.");
		return;
	}
}

static KrkToken decorator(struct GlobalState * state, size_t level, FunctionType type) {
	int inType = type;
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance(); /* Collect the `@` */

	KrkToken funcName = {0};

	KrkToken at_staticmethod = syntheticToken("staticmethod");
	KrkToken at_classmethod  = syntheticToken("classmethod");

	if (type == TYPE_METHOD) {
		if (identifiersEqual(&at_staticmethod, &state->parser.current)) type = TYPE_STATIC;
		if (identifiersEqual(&at_classmethod, &state->parser.current)) type = TYPE_CLASSMETHOD;
	}

	if (level == 0 && inType == TYPE_FUNCTION && state->current->scopeDepth != 0) {
		emitByte(OP_NONE);
	}

	expression(state);

	consume(TOKEN_EOL, "Expected end of line after decorator.");
	if (blockWidth) {
		consume(TOKEN_INDENTATION, "Expected next line after decorator to have same indentation.");
		if (state->parser.previous.length != blockWidth) error("Expected next line after decorator to have same indentation.");
	}

	if (check(TOKEN_DEF)) {
		/* We already checked for block level */
		advance();
		consume(TOKEN_IDENTIFIER, "Expected function name after 'def'");
		funcName = state->parser.previous;
		if (type == TYPE_METHOD && funcName.length == 8 && !memcmp(funcName.start,"__init__",8)) {
			type = TYPE_INIT;
		}
		if (type == TYPE_FUNCTION && state->current->scopeDepth > 0) {
			declareVariable(state);
			markInitialized(state);
		}
		function(state, type, blockWidth);
	} else if (match(TOKEN_ASYNC)) {
		if (!match(TOKEN_DEF)) {
			errorAtCurrent("Expected 'def' after 'async' with decorator, not '%*.s'",
				(int)state->parser.current.length, state->parser.current.start);
		}
		consume(TOKEN_IDENTIFIER, "Expected coroutine name after 'def'.");
		funcName = state->parser.previous;
		if (type == TYPE_FUNCTION && state->current->scopeDepth > 0) {
			declareVariable(state);
			markInitialized(state);
		}
		function(state, type == TYPE_METHOD ? TYPE_COROUTINE_METHOD : TYPE_COROUTINE, blockWidth);
	} else if (check(TOKEN_AT)) {
		funcName = decorator(state, level+1, type);
	} else if (check(TOKEN_CLASS)) {
		if (type != TYPE_FUNCTION) {
			error("Invalid decorator applied to class");
			return funcName;
		}
		funcName = classDeclaration(state);
	} else {
		error("Expected a function declaration or another decorator.");
		return funcName;
	}

	emitBytes(OP_CALL, 1);

	if (level == 0) {
		if (inType == TYPE_FUNCTION) {
			if (state->current->scopeDepth == 0) {
				size_t ind = identifierConstant(state, &funcName);
				defineVariable(state, ind);
			} else {
				emitByte(OP_SWAP_POP);
			}
		} else {
			size_t ind = identifierConstant(state, &funcName);
			rememberClassProperty(state, ind);
			EMIT_OPERAND_OP(OP_SET_NAME, ind);
			emitByte(OP_POP);
		}
	}

	return funcName;
}

static void emitLoop(struct GlobalState * state, int loopStart, uint8_t loopType) {

	/* Patch continue statements to point to here, before the loop operation (yes that's silly) */
	while (state->current->continueCount > 0 && state->current->continues[state->current->continueCount-1].offset > loopStart) {
		patchJump(state->current->continues[state->current->continueCount-1].offset);
		state->current->continueCount--;
	}

	emitByte(loopType);

	int offset = currentChunk()->count - loopStart + ((loopType == OP_LOOP_ITER) ? -1 : 2);
	if (offset > 0xFFFF) {
		_emitOverlongJump(state, currentChunk()->count, offset);
	}
	emitBytes(offset >> 8, offset);

	/* Patch break statements */
}

static void withStatement(struct GlobalState * state) {
	/* We only need this for block() */
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	KrkToken myPrevious = state->parser.previous;

	/* Collect the with token that started this statement */
	advance();

	beginScope(state);
	expression(state);

	if (match(TOKEN_AS)) {
		consume(TOKEN_IDENTIFIER, "Expected variable name after 'as'");
		size_t ind = identifierConstant(state, &state->parser.previous);
		declareVariable(state);
		defineVariable(state, ind);
	} else {
		/* Otherwise we want an unnamed local */
		anonymousLocal(state);
	}

	/* Storage for return / exception */
	anonymousLocal(state);

	/* Handler object */
	anonymousLocal(state);
	int withJump = emitJump(OP_PUSH_WITH);

	if (check(TOKEN_COMMA)) {
		state->parser.previous = myPrevious;
		withStatement(state); /* Keep nesting */
	} else {
		consume(TOKEN_COLON, "Expected ',' or ':' after 'with' statement");

		beginScope(state);
		block(state,blockWidth,"with");
		endScope(state);
	}

	patchJump(withJump);
	emitByte(OP_CLEANUP_WITH);

	/* Scope exit pops context manager */
	endScope(state);
}

static void ifStatement(struct GlobalState * state) {
	/* Figure out what block level contains us so we can match our partner else */
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	KrkToken myPrevious = state->parser.previous;

	/* Collect the if token that started this statement */
	advance();

	/* Collect condition expression */
	expression(state);

	/* if EXPR: */
	consume(TOKEN_COLON, "Expected ':' after 'if' condition.");

	if (state->parser.hadError) return;

	int thenJump = emitJump(OP_POP_JUMP_IF_FALSE);

	/* Start a new scope and enter a block */
	beginScope(state);
	block(state,blockWidth,"if");
	endScope(state);

	if (state->parser.hadError) return;

	/* See if we have a matching else block */
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (state->parser.current.length == blockWidth))) {
		/* This is complicated */
		KrkToken previous;
		if (blockWidth) {
			previous = state->parser.previous;
			advance();
		}
		if (match(TOKEN_ELSE) || check(TOKEN_ELIF)) {
			int elseJump = emitJump(OP_JUMP);
			patchJump(thenJump);
			if (state->parser.current.type == TOKEN_ELIF || check(TOKEN_IF)) {
				state->parser.previous = myPrevious;
				ifStatement(state); /* Keep nesting */
			} else {
				consume(TOKEN_COLON, "Expected ':' after 'else'.");
				beginScope(state);
				block(state,blockWidth,"else");
				endScope(state);
			}
			patchJump(elseJump);
			return;
		} else if (!check(TOKEN_EOF) && !check(TOKEN_EOL)) {
			if (blockWidth) {
				krk_ungetToken(&state->scanner, state->parser.current);
				state->parser.current = state->parser.previous;
				state->parser.previous = previous;
			}
		} else {
			advance(); /* Ignore this blank indentation line */
		}
	}

	patchJump(thenJump);
}

static void patchBreaks(struct GlobalState * state, int loopStart) {
	/* Patch break statements to go here, after the loop operation and operand. */
	while (state->current->breakCount > 0 && state->current->breaks[state->current->breakCount-1].offset > loopStart) {
		patchJump(state->current->breaks[state->current->breakCount-1].offset);
		state->current->breakCount--;
	}
}

static void breakStatement(struct GlobalState * state) {
	if (state->current->breakSpace < state->current->breakCount + 1) {
		size_t old = state->current->breakSpace;
		state->current->breakSpace = KRK_GROW_CAPACITY(old);
		state->current->breaks = KRK_GROW_ARRAY(struct LoopExit,state->current->breaks,old,state->current->breakSpace);
	}

	if (state->current->loopLocalCount != state->current->localCount) {
		EMIT_OPERAND_OP(OP_EXIT_LOOP, state->current->loopLocalCount);
	}

	state->current->breaks[state->current->breakCount++] = (struct LoopExit){emitJump(OP_JUMP),state->parser.previous};
}

static void continueStatement(struct GlobalState * state) {
	if (state->current->continueSpace < state->current->continueCount + 1) {
		size_t old = state->current->continueSpace;
		state->current->continueSpace = KRK_GROW_CAPACITY(old);
		state->current->continues = KRK_GROW_ARRAY(struct LoopExit,state->current->continues,old,state->current->continueSpace);
	}

	if (state->current->loopLocalCount != state->current->localCount) {
		EMIT_OPERAND_OP(OP_EXIT_LOOP, state->current->loopLocalCount);
	}

	state->current->continues[state->current->continueCount++] = (struct LoopExit){emitJump(OP_JUMP),state->parser.previous};
}

static void optionalElse(struct GlobalState * state, size_t blockWidth) {
	KrkScanner scannerBefore = krk_tellScanner(&state->scanner);
	Parser  parserBefore = state->parser;
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (state->parser.current.length == blockWidth))) {
		if (blockWidth) advance();
		if (match(TOKEN_ELSE)) {
			consume(TOKEN_COLON, "Expected ':' after 'else'.");
			beginScope(state);
			block(state,blockWidth,"else");
			endScope(state);
		} else {
			krk_rewindScanner(&state->scanner, scannerBefore);
			state->parser = parserBefore;
		}
	}
}

static void whileStatement(struct GlobalState * state) {
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance();

	int loopStart = currentChunk()->count;
	int exitJump = 0;

	/* Identify two common infinite loops and optimize them (True and 1) */
	RewindState rewind = {recordChunk(currentChunk()), krk_tellScanner(&state->scanner), state->parser};
	if (!(match(TOKEN_TRUE) && match(TOKEN_COLON)) &&
	    !(match(TOKEN_NUMBER) && (state->parser.previous.length == 1 && *state->parser.previous.start == '1') && match(TOKEN_COLON))) {
		/* We did not match a common infinite loop, roll back... */
		krk_rewindScanner(&state->scanner, rewind.oldScanner);
		state->parser = rewind.oldParser;

		/* Otherwise, compile a real loop condition. */
		expression(state);
		consume(TOKEN_COLON, "Expected ':' after 'while' condition.");

		exitJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);
	}

	int oldLocalCount = state->current->loopLocalCount;
	state->current->loopLocalCount = state->current->localCount;
	beginScope(state);
	block(state,blockWidth,"while");
	endScope(state);

	state->current->loopLocalCount = oldLocalCount;
	emitLoop(state, loopStart, OP_LOOP);

	if (exitJump) {
		patchJump(exitJump);
		emitByte(OP_POP);
	}

	/* else: block must still be compiled even if we optimized
	 * out the loop condition check... */
	optionalElse(state, blockWidth);

	patchBreaks(state, loopStart);
}

static void forStatement(struct GlobalState * state) {
	/* I'm not sure if I want this to be more like Python or C/Lox/etc. */
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance();

	/* For now this is going to be kinda broken */
	beginScope(state);

	ssize_t loopInd = state->current->localCount;
	int sawComma = 0;
	ssize_t varCount = 0;
	int matchedEquals = 0;

	if (!check(TOKEN_IDENTIFIER)) {
		errorAtCurrent("Empty variable list in 'for'");
		return;
	}

	do {
		if (!check(TOKEN_IDENTIFIER)) break;
		ssize_t ind = parseVariable(state, "Expected name for loop iterator.");
		if (state->parser.hadError) return;
		if (match(TOKEN_EQUAL)) {
			matchedEquals = 1;
			expression(state);
		} else {
			emitByte(OP_NONE);
		}
		defineVariable(state, ind);
		varCount++;
		if (check(TOKEN_COMMA)) sawComma = 1;
	} while (match(TOKEN_COMMA));

	int loopStart;
	int exitJump;
	int isIter = 0;

	if (!matchedEquals && match(TOKEN_IN)) {

		beginScope(state);
		expression(state);
		endScope(state);

		anonymousLocal(state);
		emitByte(OP_INVOKE_ITER);
		loopStart = currentChunk()->count;
		exitJump = emitJump(OP_CALL_ITER);

		if (varCount > 1 || sawComma) {
			EMIT_OPERAND_OP(OP_UNPACK, varCount);
			for (ssize_t i = loopInd + varCount - 1; i >= loopInd; i--) {
				EMIT_OPERAND_OP(OP_SET_LOCAL_POP, i);
			}
		} else {
			EMIT_OPERAND_OP(OP_SET_LOCAL_POP, loopInd);
		}

		isIter = 1;

	} else {
		consume(TOKEN_SEMICOLON,"Expected ';' after C-style loop initializer.");
		loopStart = currentChunk()->count;

		beginScope(state);
		expression(state); /* condition */
		endScope(state);
		exitJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);

		if (check(TOKEN_SEMICOLON)) {
			advance();
			int bodyJump = emitJump(OP_JUMP);
			int incrementStart = currentChunk()->count;
			beginScope(state);
			do {
				expressionStatement(state);
			} while (match(TOKEN_COMMA));
			endScope(state);

			emitLoop(state, loopStart, OP_LOOP);
			loopStart = incrementStart;
			patchJump(bodyJump);
		}
	}

	consume(TOKEN_COLON,"Expected ':' after loop conditions.");

	int oldLocalCount = state->current->loopLocalCount;
	state->current->loopLocalCount = state->current->localCount;
	beginScope(state);
	block(state,blockWidth,"for");
	endScope(state);

	state->current->loopLocalCount = oldLocalCount;
	emitLoop(state, loopStart, isIter ? OP_LOOP_ITER : OP_LOOP);
	patchJump(exitJump);
	emitByte(OP_POP);
	optionalElse(state, blockWidth);
	patchBreaks(state, loopStart);
	endScope(state);
}

static void returnStatement(struct GlobalState * state) {
	if (check(TOKEN_EOL) || check(TOKEN_EOF)) {
		emitReturn(state);
	} else {
		if (state->current->type == TYPE_INIT) {
			error("__init__ may not return a value.");
		}
		parsePrecedence(state, PREC_ASSIGNMENT);
		emitByte(OP_RETURN);
	}
}

static void tryStatement(struct GlobalState * state) {
	size_t blockWidth = (state->parser.previous.type == TOKEN_INDENTATION) ? state->parser.previous.length : 0;
	advance();
	consume(TOKEN_COLON, "Expected ':' after 'try'.");

	/* Make sure we are in a local scope so this ends up on the stack */
	beginScope(state);
	int tryJump = emitJump(OP_PUSH_TRY);

	size_t exceptionObject = anonymousLocal(state);
	anonymousLocal(state); /* Try */

	beginScope(state);
	block(state,blockWidth,"try");
	endScope(state);

	if (state->parser.hadError) return;

#define EXIT_JUMP_MAX 64
	int exitJumps = 2;
	int exitJumpOffsets[EXIT_JUMP_MAX] = {0};

	/* Jump possibly to `else` */
	exitJumpOffsets[0] = emitJump(OP_JUMP);

	/* Except entry point; ENTER_EXCEPT jumps to `finally` or continues to
	 * first `except` expression test; may end up redundant if there is only an 'else'. */
	patchJump(tryJump);
	exitJumpOffsets[1] = emitJump(OP_ENTER_EXCEPT);

	int firstJump = 0;
	int nextJump = -1;

_anotherExcept:
	if (state->parser.hadError) return;
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (state->parser.current.length == blockWidth))) {
		KrkToken previous;
		if (blockWidth) {
			previous = state->parser.previous;
			advance();
		}
		if (exitJumps && !firstJump && match(TOKEN_EXCEPT)) {
			if (nextJump != -1) {
				patchJump(nextJump);
			}
			/* Match filter expression (should be class or tuple) */
			if (!check(TOKEN_COLON) && !check(TOKEN_AS)) {
				expression(state);
			} else {
				emitByte(OP_NONE);
			}
			nextJump = emitJump(OP_FILTER_EXCEPT);

			/* Match 'as' to rename exception */
			size_t nameInd = 0;
			if (match(TOKEN_AS)) {
				consume(TOKEN_IDENTIFIER, "Expected identifier after 'as'.");
				state->current->locals[exceptionObject].name = state->parser.previous;
				/* `renameLocal` only introduces names for scoped debugging */
				nameInd = renameLocal(state, exceptionObject, state->parser.previous);
			} else {
				state->current->locals[exceptionObject].name = syntheticToken("");
			}

			consume(TOKEN_COLON, "Expected ':' after 'except'.");
			beginScope(state);
			block(state,blockWidth,"except");
			endScope(state);

			/* Remove scoped name */
			if (nameInd) state->current->codeobject->localNames[nameInd].deathday = (size_t)currentChunk()->count;

			if (exitJumps < EXIT_JUMP_MAX) {
				exitJumpOffsets[exitJumps++] = emitJump(OP_JUMP);
			} else {
				error("Too many 'except' clauses.");
				return;
			}

			goto _anotherExcept;
		} else if (firstJump != 1 && match(TOKEN_ELSE)) {
			consume(TOKEN_COLON, "Expected ':' after 'else'.");
			patchJump(exitJumpOffsets[0]);
			firstJump = 1;
			emitByte(OP_TRY_ELSE);
			state->current->locals[exceptionObject].name = syntheticToken("");
			beginScope(state);
			block(state, blockWidth, "else");
			endScope(state);
			if (nextJump == -1) {
				/* If there were no except: blocks, we need to make sure that the
				 * 'try' handler goes directly to the finally, so that 'break'/'continue'
				 * within the 'try' does not run this 'else' step. */
				patchJump(tryJump);
			}
			goto _anotherExcept;
		} else if (match(TOKEN_FINALLY)) {
			consume(TOKEN_COLON, "Expected ':' after 'finally'.");
			for (int i = firstJump; i < exitJumps; ++i) {
				patchJump(exitJumpOffsets[i]);
			}
			if (nextJump != -1) {
				patchJump(nextJump);
			}
			emitByte(OP_BEGIN_FINALLY);
			exitJumps = 0;
			state->current->locals[exceptionObject].name = syntheticToken("");
			beginScope(state);
			block(state,blockWidth,"finally");
			endScope(state);
			nextJump = -2;
			emitByte(OP_END_FINALLY);
		} else if (!check(TOKEN_EOL) && !check(TOKEN_EOF)) {
			krk_ungetToken(&state->scanner, state->parser.current);
			state->parser.current = state->parser.previous;
			if (blockWidth) {
				state->parser.previous = previous;
			}
		} else {
			advance(); /* Ignore this blank indentation line */
		}
	}

	for (int i = firstJump; i < exitJumps; ++i) {
		patchJump(exitJumpOffsets[i]);
	}

	if (nextJump >= 0) {
		patchJump(nextJump);
		emitByte(OP_BEGIN_FINALLY);
		emitByte(OP_END_FINALLY);
	}

	endScope(state); /* will pop the exception handler */
}

static void raiseStatement(struct GlobalState * state) {
	parsePrecedence(state, PREC_ASSIGNMENT);

	if (match(TOKEN_FROM)) {
		parsePrecedence(state, PREC_ASSIGNMENT);
		emitByte(OP_RAISE_FROM);
	} else {
		emitByte(OP_RAISE);
	}
}

static size_t importModule(struct GlobalState * state, KrkToken * startOfName, int leadingDots) {
	size_t ind = 0;
	struct StringBuilder sb = {0};

	for (int i = 0; i < leadingDots; ++i) {
		pushStringBuilder(&sb, '.');
	}

	if (!(leadingDots && check(TOKEN_IMPORT))) {
		consume(TOKEN_IDENTIFIER, "Expected module name after 'import'.");
		if (state->parser.hadError) goto _freeImportName;
		pushStringBuilderStr(&sb, state->parser.previous.start, state->parser.previous.length);

		while (match(TOKEN_DOT)) {
			pushStringBuilderStr(&sb, state->parser.previous.start, state->parser.previous.length);
			consume(TOKEN_IDENTIFIER, "Expected module path element after '.'");
			if (state->parser.hadError) goto _freeImportName;
			pushStringBuilderStr(&sb, state->parser.previous.start, state->parser.previous.length);
		}
	}

	startOfName->start  = sb.bytes;
	startOfName->length = sb.length;

	ind = identifierConstant(state, startOfName);
	EMIT_OPERAND_OP(OP_IMPORT, ind);

_freeImportName:
	discardStringBuilder(&sb);
	return ind;
}

static void importStatement(struct GlobalState * state) {
	KrkToken left = state->parser.previous;
	do {
		KrkToken firstName = state->parser.current;
		KrkToken startOfName = {0};
		size_t ind = importModule(state, &startOfName, 0);
		writeExpressionLocation(&left,&state->parser.current,&firstName,state);
		if (match(TOKEN_AS)) {
			consume(TOKEN_IDENTIFIER, "Expected identifier after 'as'.");
			ind = identifierConstant(state, &state->parser.previous);
		} else if (startOfName.length != firstName.length) {
			/**
			 * We imported foo.bar.baz and 'baz' is now on the stack with no name.
			 * But while doing that, we built a chain so that foo and foo.bar are
			 * valid modules that already exist in the module table. We want to
			 * have 'foo.bar.baz' be this new object, so remove 'baz', reimport
			 * 'foo' directly, and put 'foo' into the appropriate namespace.
			 */
			emitByte(OP_POP);
			state->parser.previous = firstName;
			ind = identifierConstant(state, &firstName);
			EMIT_OPERAND_OP(OP_IMPORT, ind);
		}
		declareVariable(state);
		defineVariable(state, ind);
	} while (match(TOKEN_COMMA));
}

static void optionsImport(struct GlobalState * state) {
	int expectCloseParen = 0;

	KrkToken compile_time_builtins = syntheticToken("compile_time_builtins");
	KrkToken no_implicit_self = syntheticToken("no_implicit_self");

	advance();
	consume(TOKEN_IMPORT, "__options__ is not a package\n");

	if (match(TOKEN_LEFT_PAREN)) {
		expectCloseParen = 1;
		startEatingWhitespace();
	}

	do {
		consume(TOKEN_IDENTIFIER, "Expected member name");

		/* Okay, what is it? */
		if (identifiersEqual(&state->parser.previous, &compile_time_builtins)) {
			state->current->optionsFlags |= OPTIONS_FLAG_COMPILE_TIME_BUILTINS;
		} else if (identifiersEqual(&state->parser.previous, &no_implicit_self)) {
			state->current->optionsFlags |= OPTIONS_FLAG_NO_IMPLICIT_SELF;
		} else {
			error("'%.*s' is not a recognized __options__ import",
				(int)state->parser.previous.length, state->parser.previous.start);
			break;
		}

		if (check(TOKEN_AS)) {
			errorAtCurrent("__options__ imports can not be given names");
			break;
		}

	} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_PAREN));

	if (expectCloseParen) {
		stopEatingWhitespace();
		consume(TOKEN_RIGHT_PAREN, "Expected ')' after import list started with '('");
	}
}

static void fromImportStatement(struct GlobalState * state) {
	int expectCloseParen = 0;
	KrkToken startOfName = {0};
	int leadingDots = 0;

	KrkToken options = syntheticToken("__options__");
	if (check(TOKEN_IDENTIFIER) && identifiersEqual(&state->parser.current, &options)) {
		/* from __options__ import ... */
		optionsImport(state);
		return;
	}

	while (match(TOKEN_DOT) || match(TOKEN_ELLIPSIS)) {
		leadingDots += state->parser.previous.length;
	}

	importModule(state, &startOfName, leadingDots);
	consume(TOKEN_IMPORT, "Expected 'import' after module name");
	if (match(TOKEN_LEFT_PAREN)) {
		expectCloseParen = 1;
		startEatingWhitespace();
	}
	do {
		consume(TOKEN_IDENTIFIER, "Expected member name");
		size_t member = identifierConstant(state, &state->parser.previous);
		emitBytes(OP_DUP, 0); /* Duplicate the package object so we can GET_PROPERTY on it? */
		EMIT_OPERAND_OP(OP_IMPORT_FROM, member);
		if (match(TOKEN_AS)) {
			consume(TOKEN_IDENTIFIER, "Expected identifier after 'as'");
			member = identifierConstant(state, &state->parser.previous);
		}
		if (state->current->scopeDepth) {
			/* Swaps the original module and the new possible local so it can be in the right place */
			emitByte(OP_SWAP);
		}
		declareVariable(state);
		defineVariable(state, member);
	} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_PAREN));
	if (expectCloseParen) {
		stopEatingWhitespace();
		consume(TOKEN_RIGHT_PAREN, "Expected ')' after import list started with '('");
	}
	emitByte(OP_POP); /* Pop the remaining copy of the module. */
}

static void delStatement(struct GlobalState * state) {
	do {
		state->current->delSatisfied = 0;
		parsePrecedence(state, PREC_DEL_TARGET);
		if (!state->current->delSatisfied) {
			errorAtCurrent("Invalid del target");
		}
	} while (match(TOKEN_COMMA));
}

static void assertStatement(struct GlobalState * state) {
	expression(state);
	int elseJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);

	KrkToken assertionError = syntheticToken("AssertionError");
	size_t ind = identifierConstant(state, &assertionError);
	EMIT_OPERAND_OP(OP_GET_GLOBAL, ind);
	int args = 0;

	if (match(TOKEN_COMMA)) {
		expression(state);
		args = 1;
	}

	EMIT_OPERAND_OP(OP_CALL, args);
	emitByte(OP_RAISE);

	patchJump(elseJump);
	emitByte(OP_POP);
}

static void errorAfterStatement(struct GlobalState * state) {
	switch (state->parser.current.type) {
		case TOKEN_RIGHT_BRACE:
		case TOKEN_RIGHT_PAREN:
		case TOKEN_RIGHT_SQUARE:
			errorAtCurrent("Unmatched '%.*s'",
				(int)state->parser.current.length, state->parser.current.start);
			break;
		case TOKEN_IDENTIFIER:
			errorAtCurrent("Unexpected %.*s after statement.",10,"identifier");
			break;
		case TOKEN_STRING:
		case TOKEN_BIG_STRING:
			errorAtCurrent("Unexpected %.*s after statement.",6,"string");
			break;
		default:
			errorAtCurrent("Unexpected %.*s after statement.",
				(int)state->parser.current.length, state->parser.current.start);
	}
}

static void simpleStatement(struct GlobalState * state) {
_anotherSimpleStatement:
	if (match(TOKEN_RAISE)) {
		raiseStatement(state);
	} else if (match(TOKEN_RETURN)) {
		returnStatement(state);
	} else if (match(TOKEN_IMPORT)) {
		importStatement(state);
	} else if (match(TOKEN_FROM)) {
		fromImportStatement(state);
	} else if (match(TOKEN_BREAK)) {
		breakStatement(state);
	} else if (match(TOKEN_CONTINUE)) {
		continueStatement(state);
	} else if (match(TOKEN_DEL)) {
		delStatement(state);
	} else if (match(TOKEN_ASSERT)) {
		assertStatement(state);
	} else if (match(TOKEN_PASS)) {
		/* Do nothing. */
	} else if (match(TOKEN_LET)) {
		letDeclaration(state);
	} else {
		expressionStatement(state);
	}
	if (match(TOKEN_SEMICOLON)) goto _anotherSimpleStatement;
	if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
		errorAfterStatement(state);
	}
}

static void statement(struct GlobalState * state) {
	if (match(TOKEN_EOL) || match(TOKEN_EOF)) {
		return; /* Meaningless blank line */
	}

	if (check(TOKEN_IF)) {
		ifStatement(state);
	} else if (check(TOKEN_WHILE)) {
		whileStatement(state);
	} else if (check(TOKEN_FOR)) {
		forStatement(state);
	} else if (check(TOKEN_ASYNC)) {
		asyncDeclaration(state, 0);
	} else if (check(TOKEN_TRY)) {
		tryStatement(state);
	} else if (check(TOKEN_WITH)) {
		withStatement(state);
	} else {
		/* These statements don't eat line feeds, so we need expect to see another one. */
		simpleStatement(state);
	}
}

static void yield(struct GlobalState * state, int exprType, RewindState *rewind) {
	if (state->current->type == TYPE_MODULE ||
		state->current->type == TYPE_INIT ||
		state->current->type == TYPE_CLASS) {
		error("'yield' outside function");
		return;
	}
	state->current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR;
	if (match(TOKEN_FROM)) {
		parsePrecedence(state, PREC_ASSIGNMENT);
		emitByte(OP_INVOKE_ITER);
		emitByte(OP_NONE);
		size_t loopContinue = currentChunk()->count;
		size_t exitJump = emitJump(OP_YIELD_FROM);
		emitByte(OP_YIELD);
		emitLoop(state, loopContinue, OP_LOOP);
		patchJump(exitJump);
	} else if (check(TOKEN_EOL) || check(TOKEN_EOF) || check(TOKEN_RIGHT_PAREN) || check(TOKEN_RIGHT_BRACE)) {
		emitByte(OP_NONE);
		emitByte(OP_YIELD);
	} else {
		parsePrecedence(state, PREC_ASSIGNMENT);
		emitByte(OP_YIELD);
	}
	invalidTarget(state, exprType, "yield");
}

static void await(struct GlobalState * state, int exprType, RewindState *rewind) {
	if (!isCoroutine(state->current->type)) {
		error("'await' outside async function");
		return;
	}

	parsePrecedence(state, PREC_ASSIGNMENT);
	emitByte(OP_INVOKE_AWAIT);
	emitByte(OP_NONE);
	size_t loopContinue = currentChunk()->count;
	size_t exitJump = emitJump(OP_YIELD_FROM);
	emitByte(OP_YIELD);
	emitLoop(state, loopContinue, OP_LOOP);
	patchJump(exitJump);
	invalidTarget(state, exprType, "await");
}

static void unot_(struct GlobalState * state, int exprType, RewindState *rewind) {
	parsePrecedence(state, PREC_NOT);
	emitByte(OP_NOT);
	invalidTarget(state, exprType, "operator");
}

static void unary(struct GlobalState * state, int exprType, RewindState *rewind) {
	KrkTokenType operatorType = state->parser.previous.type;
	parsePrecedence(state, PREC_FACTOR);
	invalidTarget(state, exprType, "operator");
	switch (operatorType) {
		case TOKEN_PLUS:  emitByte(OP_POS); break;
		case TOKEN_MINUS: emitByte(OP_NEGATE); break;
		case TOKEN_TILDE: emitByte(OP_BITNEGATE); break;
		case TOKEN_BANG:  emitByte(OP_NOT); break;
		default: return;
	}
}

static int isHex(int c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int _pushHex(struct GlobalState * state, int isBytes, struct StringBuilder * sb, const char *c, const char *end, size_t n, char type) {
	char tmpbuf[10] = {0};
	for (size_t i = 0; i < n; ++i) {
		if (c + i + 2 == end || !isHex(c[i+2])) {
			error("truncated \\%c escape", type);
			return 1;
		}
		tmpbuf[i] = c[i+2];
	}
	unsigned long value = strtoul(tmpbuf, NULL, 16);
	if (value >= 0x110000) {
		error("invalid codepoint in \\%c escape", type);
		return 1;
	}
	if (isBytes) {
		krk_pushStringBuilder(sb, value);
	} else {
		unsigned char bytes[5] = {0};
		size_t len = krk_codepointToBytes(value, bytes);
		krk_pushStringBuilderStr(sb, (char*)bytes, len);
	}
	return 0;
}

static void string(struct GlobalState * state, int exprType, RewindState *rewind) {
	struct StringBuilder sb = {0};
#define PUSH_CHAR(c) krk_pushStringBuilder(&sb, c)
#define PUSH_HEX(n, type) _pushHex(state, isBytes, &sb, c, end, n, type)

	int isBytes = (state->parser.previous.type == TOKEN_PREFIX_B);
	int isFormat = (state->parser.previous.type == TOKEN_PREFIX_F);
	int isRaw = (state->parser.previous.type == TOKEN_PREFIX_R);

	const char * lineBefore = krk_tellScanner(&state->scanner).linePtr;
	size_t lineNo = krk_tellScanner(&state->scanner).line;

	if ((isBytes || isFormat || isRaw) && !(match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
		error("Expected string after prefix? (Internal error - scanner should not have produced this.)");
		return;
	}

	int formatElements = 0;

	/* This should capture everything but the quotes. */
	do {
		if (isRaw) {
			for (size_t i = 0; i < state->parser.previous.length - (state->parser.previous.type == TOKEN_BIG_STRING ? 6 : 2); ++i) {
				PUSH_CHAR(state->parser.previous.start[(state->parser.previous.type == TOKEN_BIG_STRING ? 3 : 1) + i]);
			}
			goto _nextStr;
		}
		int type = state->parser.previous.type == TOKEN_BIG_STRING ? 3 : 1;
		const char * c = state->parser.previous.start + type;
		const char * end = state->parser.previous.start + state->parser.previous.length - type;
		while (c < end) {
			if (*c == '\\') {
				switch (c[1]) {
					case '\\': PUSH_CHAR('\\'); break;
					case '\'': PUSH_CHAR('\''); break;
					case '\"': PUSH_CHAR('\"'); break;
					case 'a': PUSH_CHAR('\a'); break;
					case 'b': PUSH_CHAR('\b'); break;
					case 'f': PUSH_CHAR('\f'); break;
					case 'n': PUSH_CHAR('\n'); break;
					case 'r': PUSH_CHAR('\r'); break;
					case 't': PUSH_CHAR('\t'); break;
					case 'v': PUSH_CHAR('\v'); break;
					case '[': PUSH_CHAR('\033'); break;
					case 'x': {
						PUSH_HEX(2,'x');
						c += 2;
					} break;
					case 'u': {
						if (isBytes) {
							PUSH_CHAR(c[0]);
							PUSH_CHAR(c[1]);
						} else {
							PUSH_HEX(4,'u');
							c += 4;
						}
					} break;
					case 'U': {
						if (isBytes) {
							PUSH_CHAR(c[0]);
							PUSH_CHAR(c[1]);
						} else {
							PUSH_HEX(8,'U');
							c += 8;
						}
					} break;
					case '\n': break;
					default:
						if (c[1] >= '0' && c[1] <= '7') {
							int out = c[1] - '0';
							if (c + 2 != end && (c[2] >= '0' && c[2] <= '7')) {
								out <<= 3;
								out += c[2] - '0';
								c++;
								if (c + 1 != end && (c[2] >= '0' && c[2] <= '7')) {
									out <<= 3;
									out += c[2] - '0';
									c++;
								}
							}
							if (isBytes) {
								out = out % 256;
								PUSH_CHAR(out);
							} else {
								unsigned char bytes[5] = {0};
								size_t len = krk_codepointToBytes(out, bytes);
								for (size_t i = 0; i < len; i++) PUSH_CHAR(bytes[i]);
							}
						} else {
							PUSH_CHAR(c[0]);
							c++;
							continue;
						}
				}
				c += 2;
			} else if (isFormat && *c == '}') {
				if (c[1] != '}') {
					error("single '}' not allowed in f-string");
					goto _cleanupError;
				}
				PUSH_CHAR('}');
				c += 2;
				continue;
			} else if (isFormat && *c == '{') {
				if (c[1] == '{') {
					PUSH_CHAR('{');
					c += 2;
					continue;
				}
				if (sb.length) { /* Make sure there's a string for coersion reasons */
					emitConstant(krk_finishStringBuilder(&sb));
					formatElements++;
				}
				const char * start = c+1;
				KrkScanner beforeExpression = krk_tellScanner(&state->scanner);
				Parser  parserBefore = state->parser;
				KrkScanner inner = (KrkScanner){.start=c+1, .cur=c+1, .linePtr=lineBefore, .line=lineNo, .startOfLine = 0, .hasUnget = 0};
				krk_rewindScanner(&state->scanner, inner);
				advance();
				parsePrecedence(state, PREC_COMMA); /* allow unparen'd tuples, but not assignments, as expressions in f-strings */
				if (state->parser.hadError) goto _cleanupError;
				inner = krk_tellScanner(&state->scanner); /* To figure out how far to advance c */
				krk_rewindScanner(&state->scanner, beforeExpression); /* To get us back to where we were with a string token */
				state->parser = parserBefore;
				c = inner.start;

				int formatType = 0;

				while (*c == ' ') c++;
				if (*c == '=') {
					c++;
					while (*c == ' ') c++;
					emitConstant(OBJECT_VAL(krk_copyString(start,c-start)));
					formatElements++;
					formatType |= FORMAT_OP_EQ;
				}

				if (*c == '!') {
					c++;
					/* Conversion specifiers, must only be one */
					if (*c == 'r') {
						formatType |= FORMAT_OP_REPR;
					} else if (*c == 's') {
						formatType |= FORMAT_OP_STR;
					} else {
						error("Unsupported conversion flag '%c' for f-string expression.", *c);
						goto _cleanupError;
					}
					c++;
				}

				if (*c == ':') {
					/* TODO format specs */
					const char * formatStart = c+1;
					c++;
					while (c < end && *c != '}') c++;
					emitConstant(OBJECT_VAL(krk_copyString(formatStart,c-formatStart)));
					formatType |= FORMAT_OP_FORMAT;
				}

				/* Default to !r if '=' was present but neither was specified. */
				if (!(formatType & (FORMAT_OP_FORMAT | FORMAT_OP_STR)) && (formatType & FORMAT_OP_EQ)) {
					formatType |= FORMAT_OP_REPR;
				}

				EMIT_OPERAND_OP(OP_FORMAT_VALUE, formatType);

				if (*c != '}') {
					error("Expected closing '}' after expression in f-string");
					goto _cleanupError;
				}

				formatElements++;
				c++;
			} else {
				if (*(unsigned char*)c > 127 && isBytes) {
					error("bytes literal can only contain ASCII characters");
					goto _cleanupError;
				}
				PUSH_CHAR(*c);
				c++;
			}
		}

_nextStr:
		(void)0;
		isRaw = 0;
		isFormat = 0;
		if (!isBytes) {
			if (match(TOKEN_PREFIX_F)) {
				isFormat = 1;
			} else if (match(TOKEN_PREFIX_R)) {
				isRaw = 1;
			}
		}
	} while ((!isBytes || match(TOKEN_PREFIX_B)) && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)));
	if (isBytes && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
		error("Can not mix bytes and string literals");
		goto _cleanupError;
	}
	if (isBytes) {
		emitConstant(krk_finishStringBuilderBytes(&sb));
		return;
	}
	if (sb.length || !formatElements) {
		emitConstant(krk_finishStringBuilder(&sb));
		formatElements++;
	}
	if (formatElements != 1) {
		EMIT_OPERAND_OP(OP_MAKE_STRING, formatElements);
	}
_cleanupError:
	krk_discardStringBuilder(&sb);
#undef PUSH_CHAR
}

static size_t addUpvalue(struct GlobalState * state, Compiler * compiler, ssize_t index, int isLocal, KrkToken name) {
	size_t upvalueCount = compiler->codeobject->upvalueCount;
	for (size_t i = 0; i < upvalueCount; ++i) {
		Upvalue * upvalue = &compiler->upvalues[i];
		if ((ssize_t)upvalue->index == index && upvalue->isLocal == isLocal) {
			return i;
		}
	}
	if (upvalueCount + 1 > compiler->upvaluesSpace) {
		size_t old = compiler->upvaluesSpace;
		compiler->upvaluesSpace = KRK_GROW_CAPACITY(old);
		compiler->upvalues = KRK_GROW_ARRAY(Upvalue,compiler->upvalues,old,compiler->upvaluesSpace);
	}
	compiler->upvalues[upvalueCount].isLocal = isLocal;
	compiler->upvalues[upvalueCount].index = index;
	compiler->upvalues[upvalueCount].name = name;
	return compiler->codeobject->upvalueCount++;
}

static ssize_t resolveUpvalue(struct GlobalState * state, Compiler * compiler, KrkToken * name) {
	size_t upvalueCount = compiler->codeobject->upvalueCount;
	for (size_t i = 0; i < upvalueCount; ++i) {
		if (identifiersEqual(name, &compiler->upvalues[i].name)) {
			return i;
		}
	}

	if (compiler->enclosing == NULL) return -1;

	ssize_t local = resolveLocal(state, compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].isCaptured = 1;
		return addUpvalue(state, compiler, local, 1, *name);
	}
	ssize_t upvalue = resolveUpvalue(state, compiler->enclosing, name);
	if (upvalue != -1) {
		return addUpvalue(state, compiler, upvalue, 0, *name);
	}
	return -1;
}

#define OP_NONE_LONG -1
#define DO_VARIABLE(opset,opget,opdel) do { \
	if (exprType == EXPR_ASSIGN_TARGET) { \
		if (matchComplexEnd(state)) { \
			EMIT_OPERAND_OP(opset, arg); \
			break; \
		} \
		exprType = EXPR_NORMAL; \
	} \
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) { \
		parsePrecedence(state, PREC_ASSIGNMENT); \
		EMIT_OPERAND_OP(opset, arg); \
	} else if (exprType == EXPR_CAN_ASSIGN && matchAssignment(state)) { \
		EMIT_OPERAND_OP(opget, arg); \
		assignmentValue(state); \
		EMIT_OPERAND_OP(opset, arg); \
	} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel(state)) {\
		if (opdel == OP_NONE) { emitByte(OP_NONE); EMIT_OPERAND_OP(opset, arg); } \
		else { EMIT_OPERAND_OP(opdel, arg); } \
	} else { \
		EMIT_OPERAND_OP(opget, arg); \
	} } while (0)

static void namedVariable(struct GlobalState * state, KrkToken name, int exprType) {
	if (state->current->type == TYPE_CLASS) {
		/* Only at the class body level, see if this is a class property. */
		struct IndexWithNext * properties = state->current->properties;
		while (properties) {
			KrkString * constant = AS_STRING(currentChunk()->constants.values[properties->ind]);
			if (constant->length == name.length && !memcmp(constant->chars, name.start, name.length)) {
				ssize_t arg = properties->ind;
				DO_VARIABLE(OP_SET_NAME, OP_GET_NAME, OP_NONE);
				return;
			}
			properties = properties->next;
		}
	}
	ssize_t arg = resolveLocal(state, state->current, &name);
	if (arg != -1) {
		DO_VARIABLE(OP_SET_LOCAL, OP_GET_LOCAL, OP_NONE);
	} else if ((arg = resolveUpvalue(state, state->current, &name)) != -1) {
		DO_VARIABLE(OP_SET_UPVALUE, OP_GET_UPVALUE, OP_NONE);
	} else {
		if ((state->current->optionsFlags & OPTIONS_FLAG_COMPILE_TIME_BUILTINS) && *name.start != '_') {
			KrkValue value;
			if (krk_tableGet_fast(&vm.builtins->fields, krk_copyString(name.start, name.length), &value)) {
				if ((exprType == EXPR_ASSIGN_TARGET && matchComplexEnd(state)) ||
					(exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) ||
					(exprType == EXPR_CAN_ASSIGN && matchAssignment(state))) {
					error("Can not assign to '%.*s' when 'compile_time_builtins' is enabled.",
						(int)name.length, name.start);
				} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel(state)) {
					error("Can not delete '%.*s' when 'compile_time_builtins' is enabled.",
						(int)name.length, name.start);
				} else {
					emitConstant(value);
				}
				return;
			}
		}
		arg = identifierConstant(state, &name);
		DO_VARIABLE(OP_SET_GLOBAL, OP_GET_GLOBAL, OP_DEL_GLOBAL);
	}
}
#undef DO_VARIABLE

static void variable(struct GlobalState * state, int exprType, RewindState *rewind) {
	namedVariable(state, state->parser.previous, exprType);
}

static int isClassOrStaticMethod(FunctionType type) {
	return (type == TYPE_STATIC || type == TYPE_CLASSMETHOD);
}

static void super_(struct GlobalState * state, int exprType, RewindState *rewind) {
	consume(TOKEN_LEFT_PAREN, "Expected 'super' to be called.");

	/* Argument time */
	if (match(TOKEN_RIGHT_PAREN)) {
		if (!isMethod(state->current->type) && !isClassOrStaticMethod(state->current->type)) {
			error("super() outside of a method body requires arguments");
			return;
		}
		if (!state->current->codeobject->potentialPositionals) {
			error("super() is not valid in a function with no arguments");
			return;
		}
		namedVariable(state, state->currentClass->name, 0);
		EMIT_OPERAND_OP(OP_GET_LOCAL, 0);
	} else {
		expression(state);
		if (match(TOKEN_COMMA)) {
			expression(state);
		} else {
			emitByte(OP_UNSET);
		}
		consume(TOKEN_RIGHT_PAREN, "Expected ')' after argument list");
	}
	consume(TOKEN_DOT, "Expected a field of 'super()' to be referenced.");
	consume(TOKEN_IDENTIFIER, "Expected a field name.");
	size_t ind = identifierConstant(state, &state->parser.previous);
	EMIT_OPERAND_OP(OP_GET_SUPER, ind);
}

static void comprehensionInner(struct GlobalState * state, KrkScanner scannerBefore, Parser parserBefore, void (*body)(struct GlobalState*,size_t), size_t arg) {
	ssize_t loopInd = state->current->localCount;
	ssize_t varCount = 0;
	int sawComma = 0;
	if (!check(TOKEN_IDENTIFIER)) {
		errorAtCurrent("Empty variable list in comprehension");
		return;
	}
	do {
		if (!check(TOKEN_IDENTIFIER)) break;
		defineVariable(state, parseVariable(state, "Expected name for iteration variable."));
		if (state->parser.hadError) return;
		emitByte(OP_NONE);
		defineVariable(state, loopInd);
		varCount++;
		if (check(TOKEN_COMMA)) sawComma = 1;
	} while (match(TOKEN_COMMA));

	consume(TOKEN_IN, "Only iterator loops (for ... in ...) are allowed in generator expressions.");

	beginScope(state);
	parsePrecedence(state, PREC_OR); /* Otherwise we can get trapped on a ternary */
	endScope(state);

	anonymousLocal(state);
	emitByte(OP_INVOKE_ITER);
	int loopStart = currentChunk()->count;
	int exitJump = emitJump(OP_CALL_ITER);

	if (varCount > 1 || sawComma) {
		EMIT_OPERAND_OP(OP_UNPACK, varCount);
		for (ssize_t i = loopInd + varCount - 1; i >= loopInd; i--) {
			EMIT_OPERAND_OP(OP_SET_LOCAL_POP, i);
		}
	} else {
		EMIT_OPERAND_OP(OP_SET_LOCAL_POP, loopInd);
	}

	if (match(TOKEN_IF)) {
		parsePrecedence(state, PREC_OR);
		int acceptJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);
		emitLoop(state, loopStart, OP_LOOP);
		patchJump(acceptJump);
		emitByte(OP_POP); /* Pop condition */
	}

	beginScope(state);
	if (match(TOKEN_FOR)) {
		comprehensionInner(state, scannerBefore, parserBefore, body, arg);
	} else {
		KrkScanner scannerAfter = krk_tellScanner(&state->scanner);
		Parser  parserAfter = state->parser;
		krk_rewindScanner(&state->scanner, scannerBefore);
		state->parser = parserBefore;

		body(state, arg);

		krk_rewindScanner(&state->scanner, scannerAfter);
		state->parser = parserAfter;
	}
	endScope(state);

	emitLoop(state, loopStart, OP_LOOP_ITER);
	patchJump(exitJump);
	emitByte(OP_POP);
}

static void yieldInner(struct GlobalState * state, size_t arg) {
	(void)arg;
	expression(state);
	emitBytes(OP_YIELD, OP_POP);
}

/**
 * @brief Parse a generator expression.
 *
 * Builds a generator function using @ref comprehensionInner
 *
 * @param scannerBefore Scanner rewind state before the inner expression.
 * @param parserBefore  Parser rewind state before the inner expression.
 * @param body Expression body parser function.
 */
static void generatorExpression(struct GlobalState * state, KrkScanner scannerBefore, Parser parserBefore, void (*body)(struct GlobalState*,size_t)) {
	state->parser.previous = syntheticToken("<genexpr>");
	Compiler subcompiler;
	initCompiler(state, &subcompiler, TYPE_FUNCTION);
	subcompiler.codeobject->chunk.filename = subcompiler.enclosing->codeobject->chunk.filename;
	subcompiler.codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR;

	beginScope(state);
	comprehensionInner(state, scannerBefore, parserBefore, body, 0);
	endScope(state);

	KrkCodeObject *subfunction = endCompiler(state);
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(subfunction));
	EMIT_OPERAND_OP(OP_CLOSURE, indFunc);
	doUpvalues(state, &subcompiler, subfunction);
	freeCompiler(&subcompiler);
	emitBytes(OP_CALL, 0);
}

/**
 * @brief Parse a comprehension expression.
 *
 * Builds a collection comprehension using @ref comprehensionInner
 *
 * @param scannerBefore Scanner rewind state before the inner expression.
 * @param parserBefore  Parser rewind state before the inner expression.
 * @param body Expression body parser function.
 * @param type Opcode to create the initial collection object.
 */
static void comprehensionExpression(struct GlobalState * state, KrkScanner scannerBefore, Parser parserBefore, void (*body)(struct GlobalState *,size_t), int type) {
	Compiler subcompiler;
	initCompiler(state, &subcompiler, TYPE_LAMBDA);
	subcompiler.codeobject->chunk.filename = subcompiler.enclosing->codeobject->chunk.filename;

	beginScope(state);

	/* Build an empty collection to fill up. */
	emitBytes(type,0);
	size_t ind = anonymousLocal(state);

	beginScope(state);
	comprehensionInner(state, scannerBefore, parserBefore, body, ind);
	endScope(state);

	KrkCodeObject *subfunction = endCompiler(state);
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(subfunction));
	EMIT_OPERAND_OP(OP_CLOSURE, indFunc);
	doUpvalues(state, &subcompiler, subfunction);
	freeCompiler(&subcompiler);
	emitBytes(OP_CALL, 0);
}

static size_t finishStarComma(struct GlobalState * state, size_t arg, size_t * argBefore, size_t *argAfter) {
	*argBefore = arg;
	*argAfter = 1;
	EMIT_OPERAND_OP(OP_MAKE_LIST,arg);
	parsePrecedence(state, PREC_BITOR);
	emitByte(OP_LIST_EXTEND_TOP);

	if (arg == 0 && !check(TOKEN_COMMA)) {
		/* While we don't really need to, we disallow a lone @c *expr
		 * or @c (*expr) without a trailing comma because Python does.
		 * Catch that here specifically. */
		error("* expression not valid here");
		return 0;
	}

	arg++;

	while (match(TOKEN_COMMA)) {
		if (!getRule(state->parser.current.type)->prefix) break;
		if (match(TOKEN_ASTERISK)) {
			parsePrecedence(state, PREC_BITOR);
			emitByte(OP_LIST_EXTEND_TOP);
		} else {
			parsePrecedence(state, PREC_TERNARY);
			emitByte(OP_LIST_APPEND_TOP);
			(*argAfter)++;
		}
		arg++;
	}


	emitByte(OP_TUPLE_FROM_LIST);
	return arg;
}

/**
 * @brief Parse the inside of a set of parens.
 *
 * Used to parse general expression groupings as well as generator expressions.
 *
 * @param exprType Assignment target type.
 */
static void parens(struct GlobalState * state, int exprType, RewindState *rewind) {
	/* Record parser state before processing contents. */
	ChunkRecorder before = recordChunk(currentChunk());
	KrkScanner scannerBefore = krk_tellScanner(&state->scanner);
	Parser  parserBefore = state->parser;

	/*
	 * Generator expressions are not valid assignment targets, nor are
	 * an empty set of parentheses (empty tuple). A single target in
	 * parens, or a list of targets can be assigned to.
	 */
	int maybeValidAssignment = 0;

	size_t argCount = 0;
	size_t argAfter = 0;
	size_t argBefore = 0;

	/* Whitespace is ignored inside of parens */
	startEatingWhitespace();

	if (check(TOKEN_RIGHT_PAREN)) {
		/* Empty paren pair () is an empty tuple. */
		emitBytes(OP_TUPLE,0);
	} else if (match(TOKEN_ASTERISK)) {
		argCount = finishStarComma(state, 0, &argBefore, &argAfter);
		maybeValidAssignment = 1;
	} else {
		parsePrecedence(state, PREC_CAN_ASSIGN);
		maybeValidAssignment = 1;
		argCount = 1;

		if (match(TOKEN_FOR)) {
			/* Parse generator expression. */
			maybeValidAssignment = 0;
			rewindChunk(currentChunk(), before);
			generatorExpression(state, scannerBefore, parserBefore, yieldInner);
		} else if (match(TOKEN_COMMA)) {
			/* Parse as tuple literal. */
			if (!check(TOKEN_RIGHT_PAREN)) {
				/* (expr,) is a valid single-element tuple, so we need to check for that. */
				do {
					if (match(TOKEN_ASTERISK)) {
						argCount = finishStarComma(state, argCount, &argBefore, &argAfter);
						goto _done;
					}
					expression(state);
					argCount++;
				} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_PAREN));
			}
			EMIT_OPERAND_OP(OP_TUPLE, argCount);
		}
	}

_done:
	stopEatingWhitespace();

	if (!match(TOKEN_RIGHT_PAREN)) {
		switch (state->parser.current.type) {
			case TOKEN_EQUAL: error("Assignment value expression must be enclosed in parentheses."); break;
			default: error("Expected ')' at end of parenthesized expression."); break;
		}
	}

	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		if (!argCount) {
			error("Can not assign to empty target list.");
		} else if (!maybeValidAssignment) {
			error("Can not assign to generator expression.");
		} else {
			complexAssignment(state, before, scannerBefore, parserBefore, argCount, 1, argBefore, argAfter);
		}
	} else if (exprType == EXPR_ASSIGN_TARGET && (check(TOKEN_EQUAL) || check(TOKEN_COMMA) || check(TOKEN_RIGHT_PAREN))) {
		if (!argCount) {
			error("Can not assign to empty target list.");
		} else if (!maybeValidAssignment) {
			error("Can not assign to generator expression.");
		} else {
			rewindChunk(currentChunk(), before);
			complexAssignmentTargets(state, scannerBefore, parserBefore, argCount, 2, argBefore, argAfter);
			if (!matchComplexEnd(state)) {
				errorAtCurrent("Unexpected end of nested target list");
			}
		}
	}
}

/**
 * @brief Parse the expression part of a list comprehension.
 *
 * @param arg Index of the stack local to assign to.
 */
static void listInner(struct GlobalState * state, size_t arg) {
	expression(state);
	EMIT_OPERAND_OP(OP_LIST_APPEND, arg);
}

static void finishStarList(struct GlobalState * state, size_t arg) {
	EMIT_OPERAND_OP(OP_MAKE_LIST, arg);

	parsePrecedence(state, PREC_BITOR);
	emitByte(OP_LIST_EXTEND_TOP);

	while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_SQUARE)) {
		if (match(TOKEN_ASTERISK)) {
			parsePrecedence(state, PREC_BITOR);
			emitByte(OP_LIST_EXTEND_TOP);
		} else {
			expression(state);
			emitByte(OP_LIST_APPEND_TOP);
		}
	}

	stopEatingWhitespace();

	consume(TOKEN_RIGHT_SQUARE,"Expected ']' at end of list expression.");
}

/**
 * @brief Parse an expression beginning with a set of square brackets.
 *
 * Square brackets in this context are either a list literal or a list comprehension.
 */
static void list(struct GlobalState * state, int exprType, RewindState *rewind) {
	ChunkRecorder before = recordChunk(currentChunk());

	startEatingWhitespace();

	if (!check(TOKEN_RIGHT_SQUARE)) {
		KrkScanner scannerBefore = krk_tellScanner(&state->scanner);
		Parser  parserBefore = state->parser;
		if (match(TOKEN_ASTERISK)) {
			finishStarList(state, 0);
			return;
		}
		expression(state);

		if (match(TOKEN_FOR)) {
			/* Roll back the earlier compiler */
			rewindChunk(currentChunk(), before);
			/* Nested fun times */
			state->parser.previous = syntheticToken("<listcomp>");
			comprehensionExpression(state, scannerBefore, parserBefore, listInner, OP_MAKE_LIST);
		} else {
			size_t argCount = 1;
			while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_SQUARE)) {
				if (match(TOKEN_ASTERISK)) {
					finishStarList(state, argCount);
					return;
				}
				expression(state);
				argCount++;
			}
			EMIT_OPERAND_OP(OP_MAKE_LIST, argCount);
		}
	} else {
		/* Empty list expression */
		EMIT_OPERAND_OP(OP_MAKE_LIST, 0);
	}

	stopEatingWhitespace();

	consume(TOKEN_RIGHT_SQUARE,"Expected ']' at end of list expression.");
}

/**
 * @brief Parse the expression part of a dictionary comprehension.
 */
static void dictInner(struct GlobalState * state, size_t arg) {
	expression(state); /* Key */
	consume(TOKEN_COLON, "Expected ':' after dict key.");
	expression(state); /* Value */
	EMIT_OPERAND_OP(OP_DICT_SET, arg);
}

/**
 * @brief Parse the expression part of a set comprehension.
 */
static void setInner(struct GlobalState * state, size_t arg) {
	expression(state);
	EMIT_OPERAND_OP(OP_SET_ADD, arg);
}

static void finishStarSet(struct GlobalState * state, size_t args) {
	EMIT_OPERAND_OP(OP_MAKE_SET, args);

	parsePrecedence(state, PREC_BITOR);
	emitByte(OP_SET_UPDATE_TOP);

	while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE)) {
		if (match(TOKEN_ASTERISK)) {
			parsePrecedence(state, PREC_BITOR);
			emitByte(OP_SET_UPDATE_TOP);
		} else {
			expression(state);
			emitByte(OP_SET_ADD_TOP);
		}
	}

	stopEatingWhitespace();
	consume(TOKEN_RIGHT_BRACE,"Expected '}' at end of dict expression.");
}

static void finishStarDict(struct GlobalState * state, size_t args) {
	EMIT_OPERAND_OP(OP_MAKE_DICT, args);

	parsePrecedence(state, PREC_BITOR);
	emitByte(OP_DICT_UPDATE_TOP);

	while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE)) {
		if (match(TOKEN_POW)) {
			parsePrecedence(state, PREC_BITOR);
			emitByte(OP_DICT_UPDATE_TOP);
		} else {
			expression(state);
			consume(TOKEN_COLON, "Expected ':' after dict key.");
			expression(state);
			emitByte(OP_DICT_SET_TOP);
		}
	}

	stopEatingWhitespace();
	consume(TOKEN_RIGHT_BRACE,"Expected '}' at end of dict expression.");
}

/**
 * @brief Parse an expression beginning with a curly brace.
 *
 * Curly braces are either dictionaries and sets, both of
 * which can be literals or comprehensions. An empty set of
 * curly braces is an empty dictionary, not an empty set.
 *
 * We determine if we have a dictionary or a set based on
 * whether we set a colon after parsing the first inner
 * expression.
 */
static void dict(struct GlobalState * state, int exprType, RewindState *rewind) {
	ChunkRecorder before = recordChunk(currentChunk());

	startEatingWhitespace();

	if (!check(TOKEN_RIGHT_BRACE)) {
		KrkScanner scannerBefore = krk_tellScanner(&state->scanner);
		Parser  parserBefore = state->parser;

		if (match(TOKEN_ASTERISK)) {
			finishStarSet(state, 0);
			return;
		} else if (match(TOKEN_POW)) {
			finishStarDict(state, 0);
			return;
		}

		expression(state);
		if (check(TOKEN_COMMA) || check(TOKEN_RIGHT_BRACE)) {
			/* One expression, must be a set literal. */
			size_t argCount = 1;
			while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE)) {
				if (match(TOKEN_ASTERISK)) {
					finishStarSet(state, argCount);
					return;
				}
				expression(state);
				argCount++;
			}
			EMIT_OPERAND_OP(OP_MAKE_SET, argCount);
		} else if (match(TOKEN_FOR)) {
			/* One expression followed by 'for': set comprehension. */
			rewindChunk(currentChunk(), before);
			state->parser.previous = syntheticToken("<setcomp>");
			comprehensionExpression(state, scannerBefore, parserBefore, setInner, OP_MAKE_SET);
		} else {
			/* Anything else must be a colon indicating a dictionary. */
			consume(TOKEN_COLON, "Expected ':' after dict key.");
			expression(state);

			if (match(TOKEN_FOR)) {
				/* Dictionary comprehension */
				rewindChunk(currentChunk(), before);
				state->parser.previous = syntheticToken("<dictcomp>");
				comprehensionExpression(state, scannerBefore, parserBefore, dictInner, OP_MAKE_DICT);
			} else {
				/*
				 * The operand to MAKE_DICT is double the number of entries,
				 * as it is the number of stack slots to consume to produce
				 * the dict: one for each key, one for each value.
				 */
				size_t argCount = 2;
				while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE)) {
					if (match(TOKEN_POW)) {
						finishStarDict(state, argCount);
						return;
					}
					expression(state);
					consume(TOKEN_COLON, "Expected ':' after dict key.");
					expression(state);
					argCount += 2;
				}
				EMIT_OPERAND_OP(OP_MAKE_DICT, argCount);
			}
		}
	} else {
		/* Empty braces, empty dictionary. */
		EMIT_OPERAND_OP(OP_MAKE_DICT, 0);
	}

	stopEatingWhitespace();

	consume(TOKEN_RIGHT_BRACE,"Expected '}' at end of dict expression.");
}

static void ternary(struct GlobalState * state, int exprType, RewindState *rewind) {
	Parser before = state->parser;
	rewindChunk(currentChunk(), rewind->before);

	parsePrecedence(state, PREC_OR);

	int thenJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);
	consume(TOKEN_ELSE, "Expected 'else' after ternary condition");

	parsePrecedence(state, PREC_TERNARY);

	KrkScanner outScanner = krk_tellScanner(&state->scanner);
	Parser outParser = state->parser;

	int elseJump = emitJump(OP_JUMP);
	patchJump(thenJump);
	emitByte(OP_POP);

	krk_rewindScanner(&state->scanner, rewind->oldScanner);
	state->parser = rewind->oldParser;
	parsePrecedence(state, PREC_OR);
	patchJump(elseJump);

	if (!check(TOKEN_IF)) {
		state->parser = before;
		error("syntax error");
	}

	krk_rewindScanner(&state->scanner, outScanner);
	state->parser = outParser;
}

static void complexAssignmentTargets(struct GlobalState * state, KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized, size_t argBefore, size_t argAfter) {
	emitBytes(OP_DUP, 0);

	if (argAfter) {
		if (argBefore > 255  || argAfter > 256) {
			error("Too many assignment targets");
			return;
		}
		EMIT_OPERAND_OP(OP_UNPACK_EX,((argBefore << 8) | (argAfter-1)));
	} else {
		EMIT_OPERAND_OP(OP_UNPACK,targetCount);
	}
	EMIT_OPERAND_OP(OP_REVERSE,targetCount);

	/* Rewind */
	krk_rewindScanner(&state->scanner, oldScanner);
	state->parser = oldParser;

	/* Parse assignment targets */
	size_t checkTargetCount = 0;
	int seenStar = 0;
	do {
		checkTargetCount++;
		if (match(TOKEN_ASTERISK)) {
			if (seenStar) {
				errorAtCurrent("multiple *expr in assignment");
				return;
			}
			seenStar = 1;
		}
		parsePrecedence(state, PREC_MUST_ASSIGN);
		emitByte(OP_POP);

		if (checkTargetCount == targetCount && state->parser.previous.type == TOKEN_COMMA) {
			if (!match(parenthesized ? TOKEN_RIGHT_PAREN : TOKEN_EQUAL)) {
				goto _errorAtCurrent;
			}
		}

		if (checkTargetCount == targetCount && parenthesized) {
			if (state->parser.previous.type != TOKEN_RIGHT_PAREN) {
				goto _errorAtCurrent;
			}
		}

		if (checkTargetCount == targetCount && parenthesized) {
			if (parenthesized == 1 && !match(TOKEN_EQUAL)) {
				goto _errorAtCurrent;
			}
		}

		if (checkTargetCount == targetCount) return;

		if (check(TOKEN_COMMA) || check(TOKEN_EQUAL) || check(TOKEN_RIGHT_PAREN)) {
			goto _errorAtCurrent;
		}

	} while (state->parser.previous.type != TOKEN_EQUAL && !state->parser.hadError);

_errorAtCurrent:
	errorAtCurrent("Invalid complex assignment target");
}

static void complexAssignment(struct GlobalState * state, ChunkRecorder before, KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized, size_t argBefore, size_t argAfter) {

	rewindChunk(currentChunk(), before);
	parsePrecedence(state, PREC_ASSIGNMENT);

	/* Store end state */
	KrkScanner outScanner = krk_tellScanner(&state->scanner);
	Parser outParser = state->parser;

	complexAssignmentTargets(state, oldScanner,oldParser,targetCount,parenthesized, argBefore, argAfter);

	/* Restore end state */
	krk_rewindScanner(&state->scanner, outScanner);
	state->parser = outParser;
}

static void comma(struct GlobalState * state, int exprType, RewindState *rewind) {
	size_t expressionCount = 1;
	size_t argBefore = 0;
	size_t argAfter = 0;
	do {
		if (!getRule(state->parser.current.type)->prefix) break;
		if (match(TOKEN_ASTERISK)) {
			expressionCount = finishStarComma(state, expressionCount, &argBefore, &argAfter);
			goto _maybeassign;
			return;
		}
		expressionCount++;
		parsePrecedence(state, PREC_TERNARY);
	} while (match(TOKEN_COMMA));

	EMIT_OPERAND_OP(OP_TUPLE,expressionCount);

_maybeassign: (void)0;
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		complexAssignment(state, rewind->before, rewind->oldScanner, rewind->oldParser, expressionCount, 0, argBefore, argAfter);
	}
}

static void pstar(struct GlobalState * state, int exprType, RewindState *rewind) {
	size_t argBefore = 0;
	size_t argAfter = 0;
	size_t totalArgs = finishStarComma(state,0, &argBefore, &argAfter);
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		complexAssignment(state, rewind->before, rewind->oldScanner, rewind->oldParser, totalArgs, 0, argBefore, argAfter);
	}
}

static void call(struct GlobalState * state, int exprType, RewindState *rewind) {
	KrkToken left = rewind ? rewind->oldParser.current : state->parser.previous;
	KrkToken this = state->parser.previous;
	startEatingWhitespace();
	size_t argCount = 0, specialArgs = 0, keywordArgs = 0, seenKeywordUnpacking = 0;
	if (!check(TOKEN_RIGHT_PAREN)) {
		size_t chunkBefore = currentChunk()->count;
		KrkScanner scannerBefore = krk_tellScanner(&state->scanner);
		Parser  parserBefore = state->parser;
		do {
			if (check(TOKEN_RIGHT_PAREN)) break;
			if (match(TOKEN_ASTERISK) || check(TOKEN_POW)) {
				specialArgs++;
				if (match(TOKEN_POW)) {
					seenKeywordUnpacking = 1;
					emitBytes(OP_EXPAND_ARGS, 2); /* creates a KWARGS_DICT */
					expression(state); /* Expect dict */
					continue;
				} else {
					if (seenKeywordUnpacking) {
						error("Iterable expansion follows keyword argument unpacking.");
						return;
					}
					emitBytes(OP_EXPAND_ARGS, 1); /* creates a KWARGS_LIST */
					expression(state);
					continue;
				}
			}
			if (match(TOKEN_IDENTIFIER)) {
				KrkToken argName = state->parser.previous;
				if (check(TOKEN_EQUAL)) {
					/* This is a keyword argument. */
					advance();
					/* Output the name */
					size_t ind = identifierConstant(state, &argName);
					EMIT_OPERAND_OP(OP_CONSTANT, ind);
					expression(state);
					keywordArgs++;
					specialArgs++;
					continue;
				} else {
					/*
					 * This is a regular argument that happened to start with an identifier,
					 * roll it back so we can process it that way.
					 */
					krk_ungetToken(&state->scanner, state->parser.current);
					state->parser.current = argName;
				}
			} else if (seenKeywordUnpacking) {
				error("Positional argument follows keyword argument unpacking");
				return;
			} else if (keywordArgs) {
				error("Positional argument follows keyword argument");
				return;
			}
			if (specialArgs) {
				emitBytes(OP_EXPAND_ARGS, 0); /* creates a KWARGS_SINGLE */
				expression(state);
				specialArgs++;
				continue;
			}
			expression(state);
			if (argCount == 0 && match(TOKEN_FOR)) {
				currentChunk()->count = chunkBefore;
				generatorExpression(state, scannerBefore, parserBefore, yieldInner);
				argCount = 1;
				if (match(TOKEN_COMMA)) {
					error("Generator expression must be parenthesized");
					return;
				}
				break;
			}
			argCount++;
		} while (match(TOKEN_COMMA));
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
	if (specialArgs) {
		/*
		 * Creates a sentinel at the top of the stack to tell the CALL instruction
		 * how many keyword arguments are at the top of the stack. This value
		 * triggers special handling in the CALL that processes the keyword arguments,
		 * which is relatively slow, so only use keyword arguments if you have to!
		 */
		EMIT_OPERAND_OP(OP_KWARGS, specialArgs);
		/*
		 * We added two elements - name and value - for each keyword arg,
		 * plus the sentinel object that will show up at the end after the
		 * OP_KWARGS instruction complets, so make sure we have the
		 * right depth into the stack when we execute CALL
		 */
		argCount += 1 /* for the sentinel */ + 2 * specialArgs;
	}

	if (exprType == EXPR_METHOD_CALL) {
		EMIT_OPERAND_OP(OP_CALL_METHOD, argCount);
	} else if (exprType == EXPR_CLASS_PARAMETERS) {
		EMIT_OPERAND_OP(OP_CALL, (argCount + 2));
	} else {
		EMIT_OPERAND_OP(OP_CALL, argCount);
	}
	writeExpressionLocation(&left,&state->parser.previous,&this,state);

	invalidTarget(state, exprType, "function call");
}

static void and_(struct GlobalState * state, int exprType, RewindState *rewind) {
	int endJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);
	parsePrecedence(state, PREC_AND);
	patchJump(endJump);
	invalidTarget(state, exprType, "operator");
}

static void or_(struct GlobalState * state, int exprType, RewindState *rewind) {
	int endJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);
	parsePrecedence(state, PREC_OR);
	patchJump(endJump);
	invalidTarget(state, exprType, "operator");
}

static void parsePrecedence(struct GlobalState * state, Precedence precedence) {
	RewindState rewind = {recordChunk(currentChunk()), krk_tellScanner(&state->scanner), state->parser};

	advance();
	ParseFn prefixRule = getRule(state->parser.previous.type)->prefix;

	/* Only allow *expr parsing where we would allow comma expressions,
	 * otherwise pretend this prefix rule doesn't exist. */
	if (prefixRule == pstar && precedence > PREC_COMMA) prefixRule = NULL;

	if (prefixRule == NULL) {
		switch (state->parser.previous.type) {
			case TOKEN_RIGHT_BRACE:
			case TOKEN_RIGHT_PAREN:
			case TOKEN_RIGHT_SQUARE:
				error("Unmatched '%.*s'",
					(int)state->parser.previous.length, state->parser.previous.start);
				break;
			case TOKEN_EOL:
				/* TODO: This should definitely be tripping the REPL to ask for more input. */
				error("Unexpected end of line");
				break;
			case TOKEN_EOF:
				error("Unexpected end of input");
				break;
			default:
				error("'%.*s' does not start an expression",
					(int)state->parser.previous.length, state->parser.previous.start);
		}
		return;
	}
	int exprType = 0;
	if (precedence <= PREC_ASSIGNMENT || precedence == PREC_CAN_ASSIGN) exprType = EXPR_CAN_ASSIGN;
	if (precedence == PREC_MUST_ASSIGN) exprType = EXPR_ASSIGN_TARGET;
	if (precedence == PREC_DEL_TARGET) exprType = EXPR_DEL_TARGET;
	prefixRule(state, exprType, &rewind);
	while (precedence <= getRule(state->parser.current.type)->precedence) {
		if (state->parser.hadError) {
			skipToEnd();
			return;
		}

		if (exprType == EXPR_ASSIGN_TARGET && (state->parser.previous.type == TOKEN_COMMA ||
			state->parser.previous.type == TOKEN_EQUAL)) break;
		advance();
		ParseFn infixRule = getRule(state->parser.previous.type)->infix;
		infixRule(state, exprType, &rewind);
	}

	if (exprType == EXPR_CAN_ASSIGN && matchAssignment(state)) {
		error("Invalid assignment target");
	}
}


static int maybeSingleExpression(struct GlobalState * state) {
	/* We're only going to use this to reset if we found a string, and it turns out
	 * not to be a docstring. */
	RewindState rewind = {recordChunk(currentChunk()), krk_tellScanner(&state->scanner), state->parser};

	/**
	 * Docstring:
	 * If the first expression is just a single string token,
	 * and that single string token is followed by a line feed
	 * and that line feed is not the end of the input... then
	 * emit code to attach docstring.
	 */
	if (check(TOKEN_STRING) || check(TOKEN_BIG_STRING)) {
		advance();
		if (match(TOKEN_EOL)) {
			/* We found just a string on the first line, but is it the only line?
			 * We should treat that as a regular string expression, eg. in a repl. */
			int isEof = check(TOKEN_EOF);
			/* Regardless, restore the scanner/parser so we can actually parse the string. */
			krk_rewindScanner(&state->scanner, rewind.oldScanner);
			state->parser = rewind.oldParser;
			advance();
			/* Parse the string. */
			string(state, EXPR_NORMAL, NULL);
			/* If we did see end of input, it's a simple string expression. */
			if (isEof) return 1;
			/* Otherwise, it's a docstring, and there's more code following it.
			 * Emit the instructions to assign the docstring to the current globals. */
			KrkToken doc = syntheticToken("__doc__");
			size_t ind = identifierConstant(state, &doc);
			EMIT_OPERAND_OP(OP_DEFINE_GLOBAL, ind);
			return 0;
		} else {
			/* There was something other than a line feed after the string token,
			 * rewind so we can parse as an expression next. */
			krk_rewindScanner(&state->scanner, rewind.oldScanner);
			state->parser = rewind.oldParser;
		}
	}

	/* Try to parse one single expression */
	ParseRule * rule = getRule(state->parser.current.type);
	if (rule->prefix) {
		parsePrecedence(state, PREC_ASSIGNMENT);

		/* Semicolon after expression statement, finish this one and continue
		 * parsing only more simple statements, as we would normally. */
		if (match(TOKEN_SEMICOLON)) {
			emitByte(OP_POP);
			simpleStatement(state);
			return 0;
		}

		/* Expression statement that isn't the end of the input, finish it
		 * and let the declaration loop handle the rest. */
		if (match(TOKEN_EOL) && !check(TOKEN_EOF)) {
			emitByte(OP_POP);
			return 0;
		}

		/* End of input after expression, must be just the single expression;
		 * using check rather than match makes the return emit on the same
		 * line, which produces cleaner disassembly when -d tracing is enabled. */
		if (check(TOKEN_EOF))
			return 1;

		/* Must be an error. */
		errorAfterStatement(state);
		return 0;
	}

	return 0;
}

_noexport
void _createAndBind_compilerClass(void) {
	KrkClass * CompilerState = ADD_BASE_CLASS(KRK_BASE_CLASS(CompilerState), "CompilerState", KRK_BASE_CLASS(object));
	CompilerState->allocSize = sizeof(struct GlobalState);
	CompilerState->_ongcscan = _GlobalState_gcscan;
	CompilerState->_ongcsweep = _GlobalState_gcsweep;
	CompilerState->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	krk_finalizeClass(CompilerState);
}

/**
 * @brief Compile a source string to bytecode.
 *
 * Parses source code and compiles it to bytecode.
 *
 * Raises @ref SyntaxError on error and returns NULL if the
 * source failed to compile.
 *
 * @param src Code string to compile. Should be a module body or repl line.
 * @param fileName Used to describe where the source string came from in error messages.
 * @return A compiled code object, or NULL on error.
 * @exception SyntaxError if @p src could not be compiled.
 */
KrkCodeObject * krk_compile(const char * src, const char * fileName) {
	struct GlobalState * state = (void*)krk_newInstance(KRK_BASE_CLASS(CompilerState));
	krk_push(OBJECT_VAL(state));

	/* Point a new scanner at the source. */
	state->scanner = krk_initScanner(src);

	/* Reset parser state. */
	memset(&state->parser, 0, sizeof(state->parser));

	/* Start compiling a new function. */
	Compiler compiler;
	initCompiler(state, &compiler, TYPE_MODULE);
	compiler.codeobject->chunk.filename = krk_copyString(fileName, strlen(fileName));
	compiler.codeobject->name = krk_copyString("<module>",8);

	/* Start reading tokens from the scanner... */
	advance();

	/* The first line of an input may be a doc string. */
	if (maybeSingleExpression(state)) {
		state->current->type = TYPE_LAMBDA;
	} else {
		/* Parse top-level declarations... */
		while (!match(TOKEN_EOF)) {
			declaration(state);

			/* Skip over redundant whitespace */
			if (check(TOKEN_EOL) || check(TOKEN_INDENTATION) || check(TOKEN_EOF)) {
				advance();
			}
		}
	}

	KrkCodeObject * function = endCompiler(state);
	freeCompiler(&compiler);

	/*
	 * We'll always get something out of endCompiler even if it
	 * wasn't fully compiled, so be sure to check for a syntax
	 * error and return NULL
	 */
	if (state->parser.hadError) function = NULL;

	krk_pop();
	return function;
}

#define RULE(token, a, b, c) [TOKEN_ ## token] = {a, b, c}

/**
 * @brief Parse rules table.
 *
 * Each parse rule consists of a token, an prefix rule, an infix rule,
 * and a parse precedence. We also stringify the token name for use
 * in debugging and error messages. The rule table is indexed by token
 * types and is constructed with a designated initializer, so the order
 * in this file is not important; it's probably best to order either
 * visually or syntactically related elements together.
 */
ParseRule krk_parseRules[] = {
	RULE(DOT,           NULL,     dot,      PREC_PRIMARY),
	RULE(LEFT_PAREN,    parens,   call,     PREC_PRIMARY),
	RULE(LEFT_SQUARE,   list,     getitem,  PREC_PRIMARY),
	RULE(LEFT_BRACE,    dict,     NULL,     PREC_NONE),
	RULE(RIGHT_PAREN,   NULL,     NULL,     PREC_NONE),
	RULE(RIGHT_SQUARE,  NULL,     NULL,     PREC_NONE),
	RULE(RIGHT_BRACE,   NULL,     NULL,     PREC_NONE),
	RULE(COLON,         NULL,     NULL,     PREC_NONE),
	RULE(SEMICOLON,     NULL,     NULL,     PREC_NONE),
	RULE(EQUAL,         NULL,     NULL,     PREC_NONE),
	RULE(WALRUS,        NULL,     NULL,     PREC_NONE),
	RULE(PLUS_EQUAL,    NULL,     NULL,     PREC_NONE),
	RULE(MINUS_EQUAL,   NULL,     NULL,     PREC_NONE),
	RULE(PLUS_PLUS,     NULL,     NULL,     PREC_NONE),
	RULE(MINUS_MINUS,   NULL,     NULL,     PREC_NONE),
	RULE(CARET_EQUAL,   NULL,     NULL,     PREC_NONE),
	RULE(PIPE_EQUAL,    NULL,     NULL,     PREC_NONE),
	RULE(LSHIFT_EQUAL,  NULL,     NULL,     PREC_NONE),
	RULE(RSHIFT_EQUAL,  NULL,     NULL,     PREC_NONE),
	RULE(AMP_EQUAL,     NULL,     NULL,     PREC_NONE),
	RULE(SOLIDUS_EQUAL, NULL,     NULL,     PREC_NONE),
	RULE(DSOLIDUS_EQUAL,NULL,     NULL,     PREC_NONE),
	RULE(ASTERISK_EQUAL,NULL,     NULL,     PREC_NONE),
	RULE(MODULO_EQUAL,  NULL,     NULL,     PREC_NONE),
	RULE(AT_EQUAL,      NULL,     NULL,     PREC_NONE),
	RULE(POW_EQUAL,     NULL,     NULL,     PREC_NONE),
	RULE(ARROW,         NULL,     NULL,     PREC_NONE),
	RULE(MINUS,         unary,    binary,   PREC_SUM),
	RULE(PLUS,          unary,    binary,   PREC_SUM),
	RULE(TILDE,         unary,    NULL,     PREC_NONE),
	RULE(BANG,          unary,    NULL,     PREC_NONE),
	RULE(SOLIDUS,       NULL,     binary,   PREC_TERM),
	RULE(DOUBLE_SOLIDUS,NULL,     binary,   PREC_TERM),
	RULE(ASTERISK,      pstar,    binary,   PREC_TERM),
	RULE(MODULO,        NULL,     binary,   PREC_TERM),
	RULE(AT,            NULL,     binary,   PREC_TERM),
	RULE(POW,           NULL,     binary,   PREC_EXPONENT),
	RULE(PIPE,          NULL,     binary,   PREC_BITOR),
	RULE(CARET,         NULL,     binary,   PREC_BITXOR),
	RULE(AMPERSAND,     NULL,     binary,   PREC_BITAND),
	RULE(LEFT_SHIFT,    NULL,     binary,   PREC_SHIFT),
	RULE(RIGHT_SHIFT,   NULL,     binary,   PREC_SHIFT),
	RULE(BANG_EQUAL,    NULL,     compare,  PREC_COMPARISON),
	RULE(EQUAL_EQUAL,   NULL,     compare,  PREC_COMPARISON),
	RULE(GREATER,       NULL,     compare,  PREC_COMPARISON),
	RULE(GREATER_EQUAL, NULL,     compare,  PREC_COMPARISON),
	RULE(LESS,          NULL,     compare,  PREC_COMPARISON),
	RULE(LESS_EQUAL,    NULL,     compare,  PREC_COMPARISON),
	RULE(IN,            NULL,     compare,  PREC_COMPARISON),
	RULE(IS,            NULL,     compare,  PREC_COMPARISON),
	RULE(NOT,           unot_,    compare,  PREC_COMPARISON),
	RULE(IDENTIFIER,    variable, NULL,     PREC_NONE),
	RULE(STRING,        string,   NULL,     PREC_NONE),
	RULE(BIG_STRING,    string,   NULL,     PREC_NONE),
	RULE(PREFIX_B,      string,   NULL,     PREC_NONE),
	RULE(PREFIX_F,      string,   NULL,     PREC_NONE),
	RULE(PREFIX_R,      string,   NULL,     PREC_NONE),
	RULE(NUMBER,        number,   NULL,     PREC_NONE),
	RULE(AND,           NULL,     and_,     PREC_AND),
	RULE(OR,            NULL,     or_,      PREC_OR),
	RULE(FALSE,         literal,  NULL,     PREC_NONE),
	RULE(NONE,          literal,  NULL,     PREC_NONE),
	RULE(TRUE,          literal,  NULL,     PREC_NONE),
	RULE(ELLIPSIS,      ellipsis, NULL,     PREC_NONE),
	RULE(YIELD,         yield,    NULL,     PREC_NONE),
	RULE(AWAIT,         await,    NULL,     PREC_NONE),
	RULE(LAMBDA,        lambda,   NULL,     PREC_NONE),
	RULE(SUPER,         super_,   NULL,     PREC_NONE),
	RULE(CLASS,         NULL,     NULL,     PREC_NONE),
	RULE(ELSE,          NULL,     NULL,     PREC_NONE),
	RULE(FOR,           NULL,     NULL,     PREC_NONE),
	RULE(DEF,           NULL,     NULL,     PREC_NONE),
	RULE(DEL,           NULL,     NULL,     PREC_NONE),
	RULE(LET,           NULL,     NULL,     PREC_NONE),
	RULE(RETURN,        NULL,     NULL,     PREC_NONE),
	RULE(WHILE,         NULL,     NULL,     PREC_NONE),
	RULE(BREAK,         NULL,     NULL,     PREC_NONE),
	RULE(CONTINUE,      NULL,     NULL,     PREC_NONE),
	RULE(IMPORT,        NULL,     NULL,     PREC_NONE),
	RULE(RAISE,         NULL,     NULL,     PREC_NONE),
	RULE(ASYNC,         NULL,     NULL,     PREC_NONE),
	RULE(PASS,          NULL,     NULL,     PREC_NONE),
	RULE(ASSERT,        NULL,     NULL,     PREC_NONE),
	RULE(FINALLY,       NULL,     NULL,     PREC_NONE),
	RULE(ELIF,          NULL,     NULL,     PREC_NONE),
	RULE(TRY,           NULL,     NULL,     PREC_NONE),
	RULE(EXCEPT,        NULL,     NULL,     PREC_NONE),
	RULE(AS,            NULL,     NULL,     PREC_NONE),
	RULE(FROM,          NULL,     NULL,     PREC_NONE),
	RULE(WITH,          NULL,     NULL,     PREC_NONE),

	RULE(COMMA,         NULL,     comma,   PREC_COMMA),
	RULE(IF,            NULL,     ternary, PREC_TERNARY),

	RULE(INDENTATION,   NULL,     NULL,     PREC_NONE),
	RULE(ERROR,         NULL,     NULL,     PREC_NONE),
	RULE(EOL,           NULL,     NULL,     PREC_NONE),
	RULE(EOF,           NULL,     NULL,     PREC_NONE),
	RULE(RETRY,         NULL,     NULL,     PREC_NONE),
};

static ParseRule * getRule(KrkTokenType type) {
	return &krk_parseRules[type];
}

