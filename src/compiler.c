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

/**
 * @brief Token parser state.
 *
 * The parser is fairly simplistic, requiring essentially
 * no lookahead. 'previous' is generally the currently-parsed
 * token: whatever was matched by @ref match. 'current' is the
 * token to be parsed, and can be examined with @ref check.
 */
typedef struct {
	KrkToken current;              /**< Token to be parsed. */
	KrkToken previous;             /**< Last token matched, consumed, or advanced over. */
	char hadError;                 /**< Flag indicating if the parser encountered an error. */
	unsigned int eatingWhitespace; /**< Depth of whitespace-ignoring parse functions. */
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
} ExpressionType;

struct RewindState;
/**
 * @brief Subexpression parser function.
 *
 * Used by the parse rule table for infix and prefix expression
 * parser functions. The argument passed is the @ref ExpressionType
 * to compile the expression as.
 */
typedef void (*ParsePrefixFn)(int);
typedef void (*ParseInfixFn)(int, struct RewindState *);

/**
 * @brief Parse rule table entry.
 *
 * Maps tokens to prefix and infix rules. Precedence values here
 * are for the infix parsing.
 */
typedef struct {
#ifndef KRK_NO_SCAN_TRACING
	const char * name;     /**< Stringified token name for error messages and debugging. */
#endif
	ParsePrefixFn prefix;  /**< Parse function to call when this token appears at the start of an expression. */
	ParseInfixFn infix;    /**< Parse function to call when this token appears after an expression. */
	Precedence precedence; /**< Precedence ordering for Pratt parsing, @ref Precedence */
} ParseRule;

/**
 * @brief Local variable reference.
 *
 * Tracks the names and scope depth of local variables.
 * Locals are mapped to stack locations by their index
 * in the compiler's locals array.
 */
typedef struct {
	KrkToken name;   /**< Token that provided the name for this variable. */
	ssize_t depth;   /**< Stack depth, or -1 if uninitialized. */
	char isCaptured; /**< Flag indicating if the variable is captured by a closure. */
} Local;

/**
 * @brief Closure upvalue reference.
 *
 * Tracks references to local variables from enclosing scopes.
 */
typedef struct {
	size_t index;    /**< Enclosing local index or upvalue index. */
	char   isLocal;  /**< Flag indicating if @ref index is a local or upvalue index. */
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
	size_t ind;                   /**< Index of an identifier constant. */
	struct IndexWithNext * next;  /**< Linked list next pointer. */
};

/**
 * @brief Tracks 'break' and 'continue' statements.
 */
struct LoopExit {
	int offset;     /**< Offset of the jump expression to patch. */
	KrkToken token; /**< Token for this exit statement, so its location can be printed in an error message. */
};

/**
 * @brief Subcompiler state.
 *
 * Each function is compiled in its own context, with its
 * own codeobject, locals, type, scopes, etc.
 */
typedef struct Compiler {
	struct Compiler * enclosing;       /**< Enclosing function compiler, or NULL for a module. */
	KrkCodeObject * codeobject;        /**< Bytecode emitter */
	FunctionType type;                 /**< Type of function being compiled. */
	size_t scopeDepth;                 /**< Depth of nested scope blocks. */
	size_t localCount;                 /**< Total number of local variables. */
	size_t localsSpace;                /**< Space in the locals array. */
	Local  * locals;                   /**< Array of local variable references. */
	size_t upvaluesSpace;              /**< Space in the upvalues array. */
	Upvalue * upvalues;                /**< Array of upvalue references. Count is stored in the codeobject. */

	size_t loopLocalCount;             /**< Tracks how many locals to pop off the stack when exiting a loop. */
	size_t breakCount;                 /**< Number of break statements. */
	size_t breakSpace;                 /**< Space in breaks array. */
	struct LoopExit * breaks;          /**< Array of loop exit instruction indices for break statements. */
	size_t continueCount;              /**< Number of continue statements. */
	size_t continueSpace;              /**< Space in continues array. */
	struct LoopExit * continues;       /**< Array of loop exit instruction indices for continue statements. */

	size_t localNameCapacity;          /**< How much space is available in the codeobject's local names table. */

	struct IndexWithNext * properties; /**< Linked list of class property constant indices. */
	struct Compiler * enclosed;        /**< Subcompiler we are enclosing, need for type annotation compilation. */
	size_t annotationCount;            /**< Number of type annotations found while compiling function signature. */

	int delSatisfied;                  /**< Flag indicating if a 'del' target has been completed. */

	size_t optionsFlags;               /**< Special __options__ imports; similar to __future__ in Python */
} Compiler;

#define OPTIONS_FLAG_COMPILE_TIME_BUILTINS    (1 << 0)

/**
 * @brief Class compilation context.
 *
 * Allows for things like @ref super to be bound correctly.
 * Also allows us to establish qualified names for functions
 * and nested class definitions.
 */
typedef struct ClassCompiler {
	struct ClassCompiler * enclosing; /**< Enclosing class scope. */
	KrkToken name;                    /**< Name of the current class. */
	int hasAnnotations;               /**< Flag indicating if an annotation dictionary has been attached to this class. */
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
	size_t count;      /**< Offset into the bytecode */
	size_t lines;      /**< Offset into the line map */
	size_t constants;  /**< Number of constants in the constants table */
} ChunkRecorder;

/**
 * @brief Compiler emit and parse state prior to this expression.
 *
 * Used to rewind the parser for ternary and comma expressions.
 */
typedef struct RewindState {
	ChunkRecorder before;     /**< Bytecode and constant table output offsets. */
	KrkScanner    oldScanner; /**< Scanner cursor state. */
	Parser        oldParser;  /**< Previous/current tokens. */
} RewindState;

static Parser parser;
static Compiler * current = NULL;
static ClassCompiler * currentClass = NULL;

#define currentChunk() (&current->codeobject->chunk)

#define EMIT_OPERAND_OP(opc, arg) do { if (arg < 256) { emitBytes(opc, arg); } \
	else { emitBytes(opc ## _LONG, arg >> 16); emitBytes(arg >> 8, arg); } } while (0)

static int isMethod(int type) {
	return type == TYPE_METHOD || type == TYPE_INIT || type == TYPE_COROUTINE_METHOD;
}

static int isCoroutine(int type) {
	return type == TYPE_COROUTINE || type == TYPE_COROUTINE_METHOD;
}

static char * calculateQualName(void) {
	static char space[1024]; /* We'll just truncate if we need to */
	space[1023] = '\0';
	char * writer = &space[1023];

#define WRITE(s) do { \
	size_t len = strlen(s); \
	if (writer - len < space) goto _exit; \
	writer -= len; \
	memcpy(writer, s, len); \
} while (0)

	WRITE(current->codeobject->name->chars);
	/* Go up by _compiler_, ignore class compilers as we don't need them. */
	Compiler * ptr = current->enclosing;
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

static ChunkRecorder recordChunk(KrkChunk * in) {
	return (ChunkRecorder){in->count, in->linesCount, in->constants.count};
}

static void rewindChunk(KrkChunk * out, ChunkRecorder from) {
	out->count = from.count;
	out->linesCount = from.lines;
	out->constants.count = from.constants;
}

static void initCompiler(Compiler * compiler, FunctionType type) {
	compiler->enclosing = current;
	current = compiler;
	compiler->codeobject = NULL;
	compiler->type = type;
	compiler->scopeDepth = 0;
	compiler->enclosed = NULL;
	compiler->codeobject = krk_newCodeObject();
	compiler->codeobject->globalsContext = (KrkInstance*)krk_currentThread.module;
	compiler->localCount = 0;
	compiler->localsSpace = 8;
	compiler->locals = GROW_ARRAY(Local,NULL,0,8);
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
	compiler->optionsFlags = compiler->enclosing ? compiler->enclosing->optionsFlags : 0;

	if (type != TYPE_MODULE) {
		current->codeobject->name = krk_copyString(parser.previous.start, parser.previous.length);
		char * qualname = calculateQualName();
		current->codeobject->qualname = krk_copyString(qualname, strlen(qualname));
	}

	if (isMethod(type)) {
		Local * local = &current->locals[current->localCount++];
		local->depth = 0;
		local->isCaptured = 0;
		local->name.start = "self";
		local->name.length = 4;
	}
}

static void rememberClassProperty(size_t ind) {
	struct IndexWithNext * me = malloc(sizeof(struct IndexWithNext));
	me->ind = ind;
	me->next = current->properties;
	current->properties = me;
}

static void parsePrecedence(Precedence precedence);
static ParseRule * getRule(KrkTokenType type);

/* These need to be forward declared or the ordering just gets really confusing... */
static void defDeclaration(void);
static void asyncDeclaration(int);
static void statement(void);
static void declaration(void);
static KrkToken classDeclaration(void);
static void declareVariable(void);
static void string(int exprType);
static KrkToken decorator(size_t level, FunctionType type);
static void complexAssignment(ChunkRecorder before, KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized);
static void complexAssignmentTargets(KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized);
static int invalidTarget(int exprType, const char * description);
static void call(int exprType, RewindState *rewind);

static void finishError(KrkToken * token) {
	size_t i = 0;
	while (token->linePtr[i] && token->linePtr[i] != '\n') i++;

	krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "line",   (KrkObj*)krk_copyString(token->linePtr, i));
	krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "file",   (KrkObj*)currentChunk()->filename);
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "lineno", INTEGER_VAL(token->line));
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "colno",  INTEGER_VAL(token->col));
	krk_attachNamedValue (&AS_INSTANCE(krk_currentThread.currentException)->fields, "width",  INTEGER_VAL(token->literalWidth));

	if (current->codeobject->name) {
		krk_attachNamedObject(&AS_INSTANCE(krk_currentThread.currentException)->fields, "func", (KrkObj*)current->codeobject->name);
	} else {
		KrkValue name = NONE_VAL();
		krk_tableGet(&krk_currentThread.module->fields, vm.specialMethodNames[METHOD_NAME], &name);
		krk_attachNamedValue(&AS_INSTANCE(krk_currentThread.currentException)->fields, "func", name);
	}

	parser.hadError = 1;
}

#ifdef KRK_NO_DOCUMENTATION
# define raiseSyntaxError(token, ...) do { if (parser.hadError) break; krk_runtimeError(vm.exceptions->syntaxError, "syntax error"); finishError(token); } while (0)
#else
# define raiseSyntaxError(token, ...) do { if (parser.hadError) break; krk_runtimeError(vm.exceptions->syntaxError, __VA_ARGS__); finishError(token); } while (0)
#endif

#define error(...) raiseSyntaxError(&parser.previous, __VA_ARGS__)
#define errorAtCurrent(...) raiseSyntaxError(&parser.current, __VA_ARGS__)

static void advance(void) {
	parser.previous = parser.current;

	for (;;) {
		parser.current = krk_scanToken();

		if (parser.eatingWhitespace &&
			(parser.current.type == TOKEN_INDENTATION || parser.current.type == TOKEN_EOL)) continue;

#ifndef KRK_NO_SCAN_TRACING
		if (krk_currentThread.flags & KRK_THREAD_ENABLE_SCAN_TRACING) {
			fprintf(stderr, "  [%s<%d> %d:%d '%.*s']\n",
				getRule(parser.current.type)->name,
				(int)parser.current.type,
				(int)parser.current.line,
				(int)parser.current.col,
				(int)parser.current.length,
				parser.current.start);
		}
#endif

		if (parser.current.type == TOKEN_RETRY) continue;
		if (parser.current.type != TOKEN_ERROR) break;

		errorAtCurrent("%s", parser.current.start);
		break;
	}
}

static void skipToEnd(void) {
	while (parser.current.type != TOKEN_EOF) advance();
}

static void startEatingWhitespace(void) {
	parser.eatingWhitespace++;
	if (parser.current.type == TOKEN_INDENTATION || parser.current.type == TOKEN_EOL) advance();
}

static void stopEatingWhitespace(void) {
	if (parser.eatingWhitespace == 0) {
		error("Internal scanner error: Invalid nesting of `startEatingWhitespace`/`stopEatingWhitespace` calls.");
	}
	parser.eatingWhitespace--;
}

static void consume(KrkTokenType type, const char * message) {
	if (parser.current.type == type) {
		advance();
		return;
	}

	if (parser.current.type == TOKEN_EOL || parser.current.type == TOKEN_EOF) {
		parser.current = parser.previous;
	}
	errorAtCurrent("%s", message);
}

static int check(KrkTokenType type) {
	return parser.current.type == type;
}

static int match(KrkTokenType type) {
	if (!check(type)) return 0;
	advance();
	return 1;
}

static int identifiersEqual(KrkToken * a, KrkToken * b) {
	return (a->length == b->length && memcmp(a->start, b->start, a->length) == 0);
}

static KrkToken syntheticToken(const char * text) {
	KrkToken token;
	token.start = text;
	token.length = (int)strlen(text);
	token.line = parser.previous.line;
	return token;
}

static void emitByte(uint8_t byte) {
	krk_writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
	emitByte(byte1);
	emitByte(byte2);
}

static void emitReturn(void) {
	if (current->type == TYPE_INIT) {
		emitBytes(OP_GET_LOCAL, 0);
	} else if (current->type == TYPE_MODULE) {
		/* Un-pop the last stack value */
		emitBytes(OP_GET_LOCAL, 0);
	} else if (current->type != TYPE_LAMBDA && current->type != TYPE_CLASS) {
		emitByte(OP_NONE);
	}
	emitByte(OP_RETURN);
}

static KrkCodeObject * endCompiler(void) {
	KrkCodeObject * function = current->codeobject;

	for (size_t i = 0; i < current->codeobject->localNameCount; i++) {
		if (current->codeobject->localNames[i].deathday == 0) {
			current->codeobject->localNames[i].deathday = currentChunk()->count;
		}
	}
	current->codeobject->localNames = GROW_ARRAY(KrkLocalEntry, current->codeobject->localNames, \
		current->localNameCapacity, current->codeobject->localNameCount); /* Shorten this down for runtime */

	if (current->continueCount) { parser.previous = current->continues[0].token; error("continue without loop"); }
	if (current->breakCount) { parser.previous = current->breaks[0].token; error("break without loop"); }
	emitReturn();

	/* Attach contants for arguments */
	for (int i = 0; i < function->requiredArgs; ++i) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[i].name.start, current->locals[i].name.length));
		krk_push(value);
		krk_writeValueArray(&function->requiredArgNames, value);
		krk_pop();
	}
	for (int i = 0; i < function->keywordArgs; ++i) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[i+function->requiredArgs].name.start,
			current->locals[i+function->requiredArgs].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
	}
	size_t args = current->codeobject->requiredArgs + current->codeobject->keywordArgs;
	if (current->codeobject->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[args].name.start,
			current->locals[args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->requiredArgNames, value);
		krk_pop();
		args++;
	}
	if (current->codeobject->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
		KrkValue value = OBJECT_VAL(krk_copyString(current->locals[args].name.start,
			current->locals[args].name.length));
		krk_push(value);
		krk_writeValueArray(&function->keywordArgNames, value);
		krk_pop();
		args++;
	}

	current->codeobject->potentialPositionals = current->codeobject->requiredArgs + current->codeobject->keywordArgs;
	current->codeobject->totalArguments = current->codeobject->potentialPositionals + !!(current->codeobject->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) + !!(current->codeobject->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS);

#ifndef KRK_NO_DISASSEMBLY
	if ((krk_currentThread.flags & KRK_THREAD_ENABLE_DISASSEMBLY) && !parser.hadError) {
		krk_disassembleCodeObject(stderr, function, function->name ? function->name->chars : "(module)");
	}
#endif

	current = current->enclosing;
	return function;
}

static void freeCompiler(Compiler * compiler) {
	FREE_ARRAY(Local,compiler->locals, compiler->localsSpace);
	FREE_ARRAY(Upvalue,compiler->upvalues, compiler->upvaluesSpace);
	FREE_ARRAY(struct LoopExit,compiler->breaks, compiler->breakSpace);
	FREE_ARRAY(struct LoopExit,compiler->continues, compiler->continueSpace);

	while (compiler->properties) {
		void * tmp = compiler->properties;
		compiler->properties = compiler->properties->next;
		free(tmp);
	}
}

static size_t emitConstant(KrkValue value) {
	return krk_writeConstant(currentChunk(), value, parser.previous.line);
}

static ssize_t identifierConstant(KrkToken * name) {
	return krk_addConstant(currentChunk(), OBJECT_VAL(krk_copyString(name->start, name->length)));
}

static ssize_t resolveLocal(Compiler * compiler, KrkToken * name) {
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

static size_t renameLocal(size_t ind, KrkToken name) {
	if (current->codeobject->localNameCount + 1 > current->localNameCapacity) {
		size_t old = current->localNameCapacity;
		current->localNameCapacity = GROW_CAPACITY(old);
		current->codeobject->localNames = GROW_ARRAY(KrkLocalEntry, current->codeobject->localNames, old, current->localNameCapacity);
	}
	current->codeobject->localNames[current->codeobject->localNameCount].id = ind;
	current->codeobject->localNames[current->codeobject->localNameCount].birthday = currentChunk()->count;
	current->codeobject->localNames[current->codeobject->localNameCount].deathday = 0;
	current->codeobject->localNames[current->codeobject->localNameCount].name = krk_copyString(name.start, name.length);
	return current->codeobject->localNameCount++;
}

static size_t addLocal(KrkToken name) {
	if (current->localCount + 1 > current->localsSpace) {
		size_t old = current->localsSpace;
		current->localsSpace = GROW_CAPACITY(old);
		current->locals = GROW_ARRAY(Local,current->locals,old,current->localsSpace);
	}
	size_t out = current->localCount;
	Local * local = &current->locals[current->localCount++];
	local->name = name;
	local->depth = -1;
	local->isCaptured = 0;

	if (name.length) {
		renameLocal(out, name);
	}

	return out;
}

static void declareVariable(void) {
	if (current->scopeDepth == 0) return;
	KrkToken * name = &parser.previous;
	/* Detect duplicate definition */
	for (ssize_t i = current->localCount - 1; i >= 0; i--) {
		Local * local = &current->locals[i];
		if (local->depth != -1 && local->depth < (ssize_t)current->scopeDepth) break;
		if (identifiersEqual(name, &local->name)) {
			error("Duplicate definition for local '%.*s' in this scope.", (int)name->literalWidth, name->start);
		}
	}
	addLocal(*name);
}

static ssize_t parseVariable(const char * errorMessage) {
	consume(TOKEN_IDENTIFIER, errorMessage);

	declareVariable();
	if (current->scopeDepth > 0) return 0;

	if ((current->optionsFlags & OPTIONS_FLAG_COMPILE_TIME_BUILTINS) && *parser.previous.start != '_') {
		KrkValue value;
		if (krk_tableGet_fast(&vm.builtins->fields, krk_copyString(parser.previous.start, parser.previous.length), &value)) {
			error("Conflicting declaration of global '%.*s' is invalid when 'compile_time_builtins' is enabled.",
				(int)parser.previous.length, parser.previous.start);
			return 0;
		}
	}

	return identifierConstant(&parser.previous);
}

static void markInitialized(void) {
	if (current->scopeDepth == 0) return;
	current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static size_t anonymousLocal(void) {
	size_t val = addLocal(syntheticToken(""));
	markInitialized();
	return val;
}

static void defineVariable(size_t global) {
	if (current->scopeDepth > 0) {
		markInitialized();
		return;
	}

	EMIT_OPERAND_OP(OP_DEFINE_GLOBAL, global);
}

static void number(int exprType) {
	const char * start = parser.previous.start;
	invalidTarget(exprType, "literal");

	for (size_t j = 0; j < parser.previous.length; ++j) {
		if (parser.previous.start[j] == '.') {
			double value = strtod(start, NULL);
			emitConstant(FLOATING_VAL(value));
			return;
		}
	}

	/* If we got here, it's an integer of some sort. */
	KrkValue result = krk_parse_int(start, parser.previous.literalWidth, 0);
	if (IS_NONE(result)) {
		error("invalid numeric literal");
		return;
	}
	emitConstant(result);
}

static int emitJump(uint8_t opcode) {
	emitByte(opcode);
	emitBytes(0xFF, 0xFF);
	return currentChunk()->count - 2;
}

static void patchJump(int offset) {
	int jump = currentChunk()->count - offset - 2;
	if (jump > 0xFFFF) error("Jump offset is too large for opcode.");

	currentChunk()->code[offset] = (jump >> 8) & 0xFF;
	currentChunk()->code[offset + 1] =  (jump) & 0xFF;
}

static void compareChained(int inner) {
	KrkTokenType operatorType = parser.previous.type;
	if (operatorType == TOKEN_NOT) consume(TOKEN_IN, "'in' must follow infix 'not'");
	int invert = (operatorType == TOKEN_IS && match(TOKEN_NOT));

	ParseRule * rule = getRule(operatorType);
	parsePrecedence((Precedence)(rule->precedence + 1));

	if (getRule(parser.current.type)->precedence == PREC_COMPARISON) {
		emitByte(OP_SWAP);
		emitBytes(OP_DUP, 1);
	}

	switch (operatorType) {
		case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
		case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
		case TOKEN_GREATER:       emitByte(OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL); break;
		case TOKEN_LESS:          emitByte(OP_LESS); break;
		case TOKEN_LESS_EQUAL:    emitByte(OP_LESS_EQUAL); break;

		case TOKEN_IS: emitByte(OP_IS); if (invert) emitByte(OP_NOT); break;

		case TOKEN_IN: emitByte(OP_INVOKE_CONTAINS); break;
		case TOKEN_NOT: emitBytes(OP_INVOKE_CONTAINS, OP_NOT); break;

		default: error("Invalid binary comparison operator?"); break;
	}

	if (getRule(parser.current.type)->precedence == PREC_COMPARISON) {
		size_t exitJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);
		advance();
		compareChained(1);
		patchJump(exitJump);
		if (getRule(parser.current.type)->precedence != PREC_COMPARISON) {
			if (!inner) {
				emitBytes(OP_SWAP,OP_POP);
			}
		}
	} else if (inner) {
		emitByte(OP_JUMP);
		emitBytes(0,2);
	}
}

static void compare(int exprType, RewindState *rewind) {
	compareChained(0);
	invalidTarget(exprType, "operator");
}

static void binary(int exprType, RewindState *rewind) {
	KrkTokenType operatorType = parser.previous.type;
	ParseRule * rule = getRule(operatorType);
	parsePrecedence((Precedence)(rule->precedence + 1));
	invalidTarget(exprType, "operator");

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
}

static int matchAssignment(void) {
	return (parser.current.type >= TOKEN_EQUAL && parser.current.type <= TOKEN_MODULO_EQUAL) ? (advance(), 1) : 0;
}

static int checkEndOfDel(void) {
	if (check(TOKEN_COMMA) || check(TOKEN_EOL) || check(TOKEN_EOF) || check(TOKEN_SEMICOLON)) {
		current->delSatisfied = 1;
		return 1;
	}
	return 0;
}

static int matchComplexEnd(void) {
	return match(TOKEN_COMMA) ||
			match(TOKEN_EQUAL) ||
			match(TOKEN_RIGHT_PAREN);
}

static int invalidTarget(int exprType, const char * description) {
	if (exprType == EXPR_CAN_ASSIGN && matchAssignment()) {
		error("Can not assign to %s", description);
		return 0;
	}

	if (exprType == EXPR_DEL_TARGET && checkEndOfDel()) {
		error("Can not delete %s", description);
		return 0;
	}

	return 1;
}

static void assignmentValue(void) {
	KrkTokenType type = parser.previous.type;
	if (type == TOKEN_PLUS_PLUS || type == TOKEN_MINUS_MINUS) {
		emitConstant(INTEGER_VAL(1));
	} else {
		parsePrecedence(PREC_COMMA); /* But adding a tuple is maybe not defined */
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
}

static void expression(void) {
	parsePrecedence(PREC_CAN_ASSIGN);
}

static void sliceExpression(void) {
	int isSlice = 0;
	if (match(TOKEN_COLON)) {
		emitByte(OP_NONE);
		isSlice = 1;
	} else {
		parsePrecedence(PREC_CAN_ASSIGN);
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
				parsePrecedence(PREC_CAN_ASSIGN);
			}
			if (match(TOKEN_COLON) && !check(TOKEN_RIGHT_SQUARE) && !check(TOKEN_COMMA)) {
				/* foo[x:e:s] */
				parsePrecedence(PREC_CAN_ASSIGN);
				EMIT_OPERAND_OP(OP_SLICE, 3);
			} else {
				/* foo[x:e] */
				EMIT_OPERAND_OP(OP_SLICE, 2);
			}
		}
	}
}

static void getitem(int exprType, RewindState *rewind) {

	sliceExpression();

	if (match(TOKEN_COMMA)) {
		size_t argCount = 1;
		if (!check(TOKEN_RIGHT_SQUARE)) {
			do {
				sliceExpression();
				argCount++;
			} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_SQUARE));
		}
		EMIT_OPERAND_OP(OP_TUPLE, argCount);
	}

	consume(TOKEN_RIGHT_SQUARE, "Expected ']' after index.");
	if (exprType == EXPR_ASSIGN_TARGET) {
		if (matchComplexEnd()) {
			EMIT_OPERAND_OP(OP_DUP, 2);
			emitByte(OP_INVOKE_SETTER);
			emitByte(OP_POP);
			return;
		}
		exprType = EXPR_NORMAL;
	}
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		parsePrecedence(PREC_ASSIGNMENT);
		emitByte(OP_INVOKE_SETTER);
	} else if (exprType == EXPR_CAN_ASSIGN && matchAssignment()) {
		emitBytes(OP_DUP, 1); /* o e o */
		emitBytes(OP_DUP, 1); /* o e o e */
		emitByte(OP_INVOKE_GETTER); /* o e v */
		assignmentValue(); /* o e v a */
		emitByte(OP_INVOKE_SETTER); /* r */
	} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel()) {
		emitByte(OP_INVOKE_DELETE);
	} else {
		emitByte(OP_INVOKE_GETTER);
	}
}

static void attributeUnpack(int exprType) {
	startEatingWhitespace();
	size_t argCount = 0;
	size_t argSpace = 1;
	ssize_t * args  = GROW_ARRAY(ssize_t,NULL,0,1);

	do {
		if (argSpace < argCount + 1) {
			size_t old = argSpace;
			argSpace = GROW_CAPACITY(old);
			args = GROW_ARRAY(ssize_t,args,old,argSpace);
		}
		consume(TOKEN_IDENTIFIER, "Expected attribute name");
		size_t ind = identifierConstant(&parser.previous);
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
			expression();
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
	FREE_ARRAY(ssize_t,args,argSpace);
	return;
}

static void dot(int exprType, RewindState *rewind) {
	if (match(TOKEN_LEFT_PAREN)) {
		attributeUnpack(exprType);
		return;
	}
	consume(TOKEN_IDENTIFIER, "Expected property name");
	size_t ind = identifierConstant(&parser.previous);
	if (exprType == EXPR_ASSIGN_TARGET) {
		if (matchComplexEnd()) {
			EMIT_OPERAND_OP(OP_DUP, 1);
			EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
			emitByte(OP_POP);
			return;
		}
		exprType = EXPR_NORMAL;
	}
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		parsePrecedence(PREC_ASSIGNMENT);
		EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
	} else if (exprType == EXPR_CAN_ASSIGN && matchAssignment()) {
		emitBytes(OP_DUP, 0); /* Duplicate the object */
		EMIT_OPERAND_OP(OP_GET_PROPERTY, ind);
		assignmentValue();
		EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
	} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel()) {
		EMIT_OPERAND_OP(OP_DEL_PROPERTY, ind);
	} else if (match(TOKEN_LEFT_PAREN)) {
		EMIT_OPERAND_OP(OP_GET_METHOD, ind);
		call(EXPR_METHOD_CALL,NULL);
	} else {
		EMIT_OPERAND_OP(OP_GET_PROPERTY, ind);
	}
}

static void literal(int exprType) {
	invalidTarget(exprType, "literal");
	switch (parser.previous.type) {
		case TOKEN_FALSE: emitByte(OP_FALSE); break;
		case TOKEN_NONE:  emitByte(OP_NONE); break;
		case TOKEN_TRUE:  emitByte(OP_TRUE); break;
		default: return;
	}
}

static void letDeclaration(void) {
	size_t argCount = 0;
	size_t argSpace = 1;
	ssize_t * args  = GROW_ARRAY(ssize_t,NULL,0,1);

	do {
		if (argSpace < argCount + 1) {
			size_t old = argSpace;
			argSpace = GROW_CAPACITY(old);
			args = GROW_ARRAY(ssize_t,args,old,argSpace);
		}
		ssize_t ind = parseVariable("Expected variable name.");
		if (parser.hadError) goto _letDone;
		if (current->scopeDepth > 0) {
			/* Need locals space */
			args[argCount++] = current->localCount - 1;
			if (match(TOKEN_COLON)) {
				error("Annotation on scoped variable declaration is meaningless.");
				goto _letDone;
			}
		} else {
			args[argCount++] = ind;
			if (check(TOKEN_COLON)) {
				KrkToken name = parser.previous;
				match(TOKEN_COLON);
				/* Get __annotations__ from globals */
				KrkToken annotations = syntheticToken("__annotations__");
				size_t ind = identifierConstant(&annotations);
				EMIT_OPERAND_OP(OP_GET_GLOBAL, ind);
				emitConstant(OBJECT_VAL(krk_copyString(name.start, name.length)));
				parsePrecedence(PREC_TERNARY);
				emitBytes(OP_INVOKE_SETTER, OP_POP);
			}
		}
	} while (match(TOKEN_COMMA));

	if (match(TOKEN_EQUAL)) {
		size_t expressionCount = 0;
		do {
			expressionCount++;
			expression();
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

	if (current->scopeDepth == 0) {
		for (size_t i = argCount; i > 0; i--) {
			defineVariable(args[i-1]);
		}
	} else {
		for (size_t i = 0; i < argCount; i++) {
			current->locals[current->localCount - 1 - i].depth = current->scopeDepth;
		}
	}

_letDone:
	if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
		errorAtCurrent("Expected end of line after 'let' statement.");
	}

	FREE_ARRAY(ssize_t,args,argSpace);
	return;
}

static void declaration(void) {
	if (check(TOKEN_DEF)) {
		defDeclaration();
	} else if (match(TOKEN_LET)) {
		letDeclaration();
	} else if (check(TOKEN_CLASS)) {
		KrkToken className = classDeclaration();
		size_t classConst = identifierConstant(&className);
		parser.previous = className;
		declareVariable();
		defineVariable(classConst);
	} else if (check(TOKEN_AT)) {
		decorator(0, TYPE_FUNCTION);
	} else if (check(TOKEN_ASYNC)) {
		asyncDeclaration(1);
	} else if (match(TOKEN_EOL) || match(TOKEN_EOF)) {
		return;
	} else if (check(TOKEN_INDENTATION)) {
		return;
	} else {
		statement();
	}

	if (parser.hadError) skipToEnd();
}

static void expressionStatement(void) {
	parsePrecedence(PREC_ASSIGNMENT);
	emitByte(OP_POP);
}

static void beginScope(void) {
	current->scopeDepth++;
}

static void endScope(void) {
	current->scopeDepth--;

	int closeCount = 0;
	int popCount = 0;

	while (current->localCount > 0 &&
	       current->locals[current->localCount - 1].depth > (ssize_t)current->scopeDepth) {
		if (current->locals[current->localCount - 1].isCaptured) {
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

		for (size_t i = 0; i < current->codeobject->localNameCount; i++) {
			if (current->codeobject->localNames[i].id == current->localCount - 1 &&
				current->codeobject->localNames[i].deathday == 0) {
				current->codeobject->localNames[i].deathday = (size_t)currentChunk()->count;
			}
		}
		current->localCount--;
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

static void block(size_t indentation, const char * blockName) {
	if (match(TOKEN_EOL)) {
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = parser.current.length;
			if (currentIndentation <= indentation) return;
			advance();
			if (!strcmp(blockName,"def") && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
				size_t before = currentChunk()->count;
				string(EXPR_NORMAL);
				/* That wrote to the chunk, rewind it; this should only ever go back two bytes
				 * because this should only happen as the first thing in a function definition,
				 * and thus this _should_ be the first constant and thus opcode + one-byte operand
				 * to OP_CONSTANT, but just to be safe we'll actually use the previous offset... */
				currentChunk()->count = before;
				/* Retreive the docstring from the constant table */
				current->codeobject->docstring = AS_STRING(currentChunk()->constants.values[currentChunk()->constants.count-1]);
				consume(TOKEN_EOL,"Garbage after docstring defintion");
				if (!check(TOKEN_INDENTATION) || parser.current.length != currentIndentation) {
					error("Expected at least one statement in function with docstring.");
				}
				advance();
			}
			declaration();
			while (check(TOKEN_INDENTATION)) {
				if (parser.current.length < currentIndentation) break;
				advance();
				declaration();
				if (check(TOKEN_EOL)) {
					advance();
				}
				if (parser.hadError) skipToEnd();
			};
#ifndef KRK_NO_SCAN_TRACING
			if (krk_currentThread.flags & KRK_THREAD_ENABLE_SCAN_TRACING) {
				fprintf(stderr, "\n\nfinished with block %s (ind=%d) on line %d, sitting on a %s (len=%d)\n\n",
					blockName, (int)indentation, (int)parser.current.line,
					getRule(parser.current.type)->name, (int)parser.current.length);
			}
#endif
		}
	} else {
		statement();
	}
}

static void doUpvalues(Compiler * compiler, KrkCodeObject * function) {
	assert(!!function->upvalueCount == !!compiler->upvalues);
	for (size_t i = 0; i < function->upvalueCount; ++i) {
		size_t index = compiler->upvalues[i].index;
		emitByte((compiler->upvalues[i].isLocal ? 1 : 0) | ((index > 255) ? 2 : 0));
		if (index > 255) {
			emitByte((index >> 16) & 0xFF);
			emitByte((index >> 8) & 0xFF);
		}
		emitByte(index & 0xFF);
	}
}

static void typeHint(KrkToken name) {
	current->enclosing->enclosed = current;
	current = current->enclosing;

	current->enclosed->annotationCount++;

	/* Emit name */
	emitConstant(OBJECT_VAL(krk_copyString(name.start, name.length)));
	parsePrecedence(PREC_TERNARY);

	current = current->enclosed;
	current->enclosing->enclosed = NULL;
}

static void hideLocal(void) {
	current->locals[current->localCount - 1].depth = -2;
}

static void argumentDefinition(void) {
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
		size_t myLocal = current->localCount - 1;
		EMIT_OPERAND_OP(OP_GET_LOCAL, myLocal);
		emitBytes(OP_UNSET, OP_IS);
		int jumpIndex = emitJump(OP_JUMP_IF_FALSE_OR_POP);
		beginScope();
		expression(); /* Read expression */
		EMIT_OPERAND_OP(OP_SET_LOCAL, myLocal);
		endScope();
		patchJump(jumpIndex);
		emitByte(OP_POP); /* comparison result or expression value */
		current->codeobject->keywordArgs++;
	} else {
		if (current->codeobject->keywordArgs) {
			error("non-keyword argument follows keyword argument");
			return;
		}
		current->codeobject->requiredArgs++;
	}
}

static void functionPrologue(Compiler * compiler) {
	KrkCodeObject * func = endCompiler();
	if (compiler->annotationCount) {
		EMIT_OPERAND_OP(OP_MAKE_DICT, compiler->annotationCount * 2);
	}
	size_t ind = krk_addConstant(currentChunk(), OBJECT_VAL(func));
	EMIT_OPERAND_OP(OP_CLOSURE, ind);
	doUpvalues(compiler, func);
	if (compiler->annotationCount) {
		emitByte(OP_ANNOTATE);
	}
	freeCompiler(compiler);
}

static int argumentList(FunctionType type) {
	int hasCollectors = 0;
	KrkToken self = syntheticToken("self");

	do {
		if (isMethod(type) && check(TOKEN_IDENTIFIER) &&
				identifiersEqual(&parser.current, &self)) {
			if (hasCollectors || current->codeobject->requiredArgs != 1) {
				errorAtCurrent("Argument name 'self' in a method signature is reserved for the implicit first argument.");
				return 1;
			}
			advance();
			if (type != TYPE_LAMBDA && check(TOKEN_COLON)) {
				KrkToken name = parser.previous;
				match(TOKEN_COLON);
				typeHint(name);
			}
			if (check(TOKEN_EQUAL)) {
				errorAtCurrent("'self' can not be a keyword argument.");
				return 1;
			}
			continue;
		}
		if (match(TOKEN_ASTERISK) || check(TOKEN_POW)) {
			if (match(TOKEN_POW)) {
				if (hasCollectors == 2) {
					error("Duplicate ** in parameter list.");
					return 1;
				}
				hasCollectors = 2;
				current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS;
			} else {
				if (hasCollectors) {
					error("Syntax error.");
					return 1;
				}
				hasCollectors = 1;
				current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS;
			}
			/* Collect a name, specifically "args" or "kwargs" are commont */
			ssize_t paramConstant = parseVariable(
				(hasCollectors == 1) ? "Expected parameter name after '*'." : "Expected parameter name after '**'.");
			if (parser.hadError) return 1;
			defineVariable(paramConstant);
			KrkToken name = parser.previous;
			if (isMethod(type) && identifiersEqual(&name,&self)) {
				errorAtCurrent("Argument name 'self' in a method signature is reserved for the implicit first argument.");
				return 1;
			}
			if (type != TYPE_LAMBDA && check(TOKEN_COLON)) {
				match(TOKEN_COLON);
				typeHint(name);
			}
			/* Make that a valid local for this function */
			size_t myLocal = current->localCount - 1;
			EMIT_OPERAND_OP(OP_GET_LOCAL, myLocal);
			/* Check if it's equal to the unset-kwarg-sentinel value */
			emitBytes(OP_UNSET, OP_IS);
			int jumpIndex = emitJump(OP_JUMP_IF_FALSE_OR_POP);
			/* And if it is, set it to the appropriate type */
			beginScope();
			if (hasCollectors == 1) EMIT_OPERAND_OP(OP_MAKE_LIST,0);
			else EMIT_OPERAND_OP(OP_MAKE_DICT,0);
			EMIT_OPERAND_OP(OP_SET_LOCAL, myLocal);
			endScope();
			/* Otherwise pop the comparison. */
			patchJump(jumpIndex);
			emitByte(OP_POP); /* comparison value or expression */
			continue;
		}
		if (hasCollectors) {
			error("arguments follow catch-all collector");
			break;
		}
		ssize_t paramConstant = parseVariable("Expected parameter name.");
		if (parser.hadError) return 1;
		hideLocal();
		if (type != TYPE_LAMBDA && check(TOKEN_COLON)) {
			KrkToken name = parser.previous;
			match(TOKEN_COLON);
			typeHint(name);
		}
		argumentDefinition();
		defineVariable(paramConstant);
	} while (match(TOKEN_COMMA));

	return 0;
}

static void function(FunctionType type, size_t blockWidth) {
	Compiler compiler;
	initCompiler(&compiler, type);
	compiler.codeobject->chunk.filename = compiler.enclosing->codeobject->chunk.filename;

	beginScope();

	if (isMethod(type)) current->codeobject->requiredArgs = 1;
	if (isCoroutine(type)) current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE;

	consume(TOKEN_LEFT_PAREN, "Expected start of parameter list after function name.");
	startEatingWhitespace();
	if (!check(TOKEN_RIGHT_PAREN)) {
		if (argumentList(type)) goto _bail;
	}
	stopEatingWhitespace();
	consume(TOKEN_RIGHT_PAREN, "Expected end of parameter list.");

	if (match(TOKEN_ARROW)) {
		typeHint(syntheticToken("return"));
	}

	consume(TOKEN_COLON, "Expected colon after function signature.");
	block(blockWidth,"def");
_bail: (void)0;
	functionPrologue(&compiler);
}

static void classBody(size_t blockWidth) {
	if (match(TOKEN_EOL)) {
		return;
	}

	if (check(TOKEN_AT)) {
		/* '@decorator' which should be attached to a method. */
		decorator(0, TYPE_METHOD);
	} else if (match(TOKEN_IDENTIFIER)) {
		/* Class field */
		size_t ind = identifierConstant(&parser.previous);

		if (check(TOKEN_COLON)) {
			/* Type annotation for field */
			KrkToken name = parser.previous;
			match(TOKEN_COLON);
			/* Get __annotations__ from class */
			emitBytes(OP_DUP, 0);
			KrkToken annotations = syntheticToken("__annotations__");
			size_t ind = identifierConstant(&annotations);
			if (!currentClass->hasAnnotations) {
				EMIT_OPERAND_OP(OP_MAKE_DICT, 0);
				EMIT_OPERAND_OP(OP_SET_PROPERTY, ind);
				currentClass->hasAnnotations = 1;
			} else {
				EMIT_OPERAND_OP(OP_GET_PROPERTY, ind);
			}
			emitConstant(OBJECT_VAL(krk_copyString(name.start, name.length)));
			parsePrecedence(PREC_TERNARY);
			emitBytes(OP_INVOKE_SETTER, OP_POP);

			/* A class field with a type hint can be valueless */
			if (match(TOKEN_EOL) || match(TOKEN_EOF)) return;
		}

		consume(TOKEN_EQUAL, "Class field must have value.");

		/* Value */
		parsePrecedence(PREC_COMMA);

		rememberClassProperty(ind);
		EMIT_OPERAND_OP(OP_CLASS_PROPERTY, ind);

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
		size_t ind = identifierConstant(&parser.previous);

		if (parser.previous.length == 8 && memcmp(parser.previous.start, "__init__", 8) == 0) {
			if (type == TYPE_COROUTINE_METHOD) {
				error("'%s' can not be a coroutine","__init__");
				return;
			}
			type = TYPE_INIT;
		} else if (parser.previous.length == 17 && memcmp(parser.previous.start, "__class_getitem__", 17) == 0) {
			if (type == TYPE_COROUTINE_METHOD) {
				error("'%s' can not be a coroutine","__class_getitem__");
				return;
			}
			/* This magic method is implicitly always a class method,
			 * so mark it as such so we don't do implicit self for it. */
			type = TYPE_CLASSMETHOD;
		}

		function(type, blockWidth);
		rememberClassProperty(ind);
		EMIT_OPERAND_OP(OP_CLASS_PROPERTY, ind);
	}
}

#define ATTACH_PROPERTY(propName,how,propValue) do { \
	KrkToken val_tok = syntheticToken(propValue); \
	size_t val_ind = identifierConstant(&val_tok); \
	EMIT_OPERAND_OP(how, val_ind); \
	KrkToken name_tok = syntheticToken(propName); \
	size_t name_ind = identifierConstant(&name_tok); \
	EMIT_OPERAND_OP(OP_CLASS_PROPERTY, name_ind); \
} while (0)

static KrkToken classDeclaration(void) {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `class` */

	consume(TOKEN_IDENTIFIER, "Expected class name after 'class'.");
	Compiler subcompiler;
	initCompiler(&subcompiler, TYPE_CLASS);
	subcompiler.codeobject->chunk.filename = subcompiler.enclosing->codeobject->chunk.filename;

	beginScope();

	size_t constInd = identifierConstant(&parser.previous);
	declareVariable();
	EMIT_OPERAND_OP(OP_CLASS, constInd);
	markInitialized();

	ClassCompiler classCompiler;
	classCompiler.name = parser.previous;
	classCompiler.enclosing = currentClass;
	currentClass = &classCompiler;
	classCompiler.hasAnnotations = 0;

	if (match(TOKEN_LEFT_PAREN)) {
		startEatingWhitespace();
		if (!check(TOKEN_RIGHT_PAREN)) {
			expression();
			emitByte(OP_INHERIT);
		}
		stopEatingWhitespace();
		consume(TOKEN_RIGHT_PAREN, "Expected ')' after superclass.");
	}

	beginScope();

	consume(TOKEN_COLON, "Expected ':' after class.");

	/* Set Class.__module__ to the value of __name__, which is the string
	 * name of the current module. */
	ATTACH_PROPERTY("__module__", OP_GET_GLOBAL, "__name__");
	ATTACH_PROPERTY("__qualname__", OP_CONSTANT, calculateQualName());

	if (match(TOKEN_EOL)) {
		if (check(TOKEN_INDENTATION)) {
			size_t currentIndentation = parser.current.length;
			if (currentIndentation <= blockWidth) {
				errorAtCurrent("Unexpected indentation level for class");
			}
			advance();
			if (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)) {
				string(EXPR_NORMAL);
				emitByte(OP_DOCSTRING);
				consume(TOKEN_EOL,"Garbage after docstring defintion");
				if (!check(TOKEN_INDENTATION) || parser.current.length != currentIndentation) {
					goto _pop_class;
				}
				advance();
			}
			classBody(currentIndentation);
			while (check(TOKEN_INDENTATION)) {
				if (parser.current.length < currentIndentation) break;
				advance(); /* Pass the indentation */
				classBody(currentIndentation);
			}
#ifndef KRK_NO_SCAN_TRACING
			if (krk_currentThread.flags & KRK_THREAD_ENABLE_SCAN_TRACING) fprintf(stderr, "Exiting from class definition on %s\n", getRule(parser.current.type)->name);
#endif
			/* Exit from block */
		}
	} /* else empty class (and at end of file?) we'll allow it for now... */
_pop_class:
	emitByte(OP_FINALIZE);
	currentClass = currentClass->enclosing;
	KrkCodeObject * makeclass = endCompiler();
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(makeclass));
	EMIT_OPERAND_OP(OP_CLOSURE, indFunc);
	doUpvalues(&subcompiler, makeclass);
	freeCompiler(&subcompiler);
	emitBytes(OP_CALL, 0);

	return classCompiler.name;
}

static void lambda(int exprType) {
	Compiler lambdaCompiler;
	parser.previous = syntheticToken("<lambda>");
	initCompiler(&lambdaCompiler, TYPE_LAMBDA);
	lambdaCompiler.codeobject->chunk.filename = lambdaCompiler.enclosing->codeobject->chunk.filename;
	beginScope();

	if (!check(TOKEN_COLON)) {
		if (argumentList(TYPE_LAMBDA)) goto _bail;
	}

	consume(TOKEN_COLON, "Expected ':' after lambda arguments");
	expression();

_bail:
	functionPrologue(&lambdaCompiler);

	invalidTarget(exprType, "lambda");
}

static void defDeclaration(void) {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `def` */

	ssize_t global = parseVariable("Expected function name after 'def'.");
	if (parser.hadError) return;
	markInitialized();
	function(TYPE_FUNCTION, blockWidth);
	if (parser.hadError) return;
	defineVariable(global);
}

static void asyncDeclaration(int declarationLevel) {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* 'async' */

	if (match(TOKEN_DEF)) {
		if (!declarationLevel) {
			error("'async def' not valid here");
			return;
		}
		ssize_t global = parseVariable("Expected coroutine name after 'async def'");
		if (parser.hadError) return;
		markInitialized();
		function(TYPE_COROUTINE, blockWidth);
		if (parser.hadError) return;
		defineVariable(global);
	} else if (match(TOKEN_FOR)) {
		if (!isCoroutine(current->type)) {
			error("'async for' outside of async function");
			return;
		}
		error("'async for' unsupported (GH-12)");
		return;
	} else if (match(TOKEN_WITH)) {
		if (!isCoroutine(current->type)) {
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

static KrkToken decorator(size_t level, FunctionType type) {
	int inType = type;
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance(); /* Collect the `@` */

	KrkToken funcName = {0};

	KrkToken at_staticmethod = syntheticToken("staticmethod");
	KrkToken at_classmethod  = syntheticToken("classmethod");

	if (type == TYPE_METHOD) {
		if (identifiersEqual(&at_staticmethod, &parser.current)) type = TYPE_STATIC;
		if (identifiersEqual(&at_classmethod, &parser.current)) type = TYPE_CLASSMETHOD;
	}

	expression();

	consume(TOKEN_EOL, "Expected end of line after decorator.");
	if (blockWidth) {
		consume(TOKEN_INDENTATION, "Expected next line after decorator to have same indentation.");
		if (parser.previous.length != blockWidth) error("Expected next line after decorator to have same indentation.");
	}

	if (check(TOKEN_DEF)) {
		/* We already checked for block level */
		advance();
		consume(TOKEN_IDENTIFIER, "Expected function name after 'def'");
		funcName = parser.previous;
		if (type == TYPE_METHOD && funcName.length == 8 && !memcmp(funcName.start,"__init__",8)) {
			type = TYPE_INIT;
		}
		function(type, blockWidth);
	} else if (match(TOKEN_ASYNC)) {
		if (!match(TOKEN_DEF)) {
			errorAtCurrent("Expected 'def' after 'async' with decorator, not '%*.s'",
				(int)parser.current.length, parser.current.start);
		}
		consume(TOKEN_IDENTIFIER, "Expected coroutine name after 'def'.");
		funcName = parser.previous;
		function(type == TYPE_METHOD ? TYPE_COROUTINE_METHOD : TYPE_COROUTINE, blockWidth);
	} else if (check(TOKEN_AT)) {
		funcName = decorator(level+1, type);
	} else if (check(TOKEN_CLASS)) {
		if (type != TYPE_FUNCTION) {
			error("Invalid decorator applied to class");
			return funcName;
		}
		funcName = classDeclaration();
	} else {
		error("Expected a function declaration or another decorator.");
		return funcName;
	}

	emitBytes(OP_CALL, 1);

	if (level == 0) {
		if (inType == TYPE_FUNCTION) {
			parser.previous = funcName;
			declareVariable();
			size_t ind = (current->scopeDepth > 0) ? 0 : identifierConstant(&funcName);
			defineVariable(ind);
		} else {
			size_t ind = identifierConstant(&funcName);
			rememberClassProperty(ind);
			EMIT_OPERAND_OP(OP_CLASS_PROPERTY, ind);
		}
	}

	return funcName;
}

static void emitLoop(int loopStart, uint8_t loopType) {

	/* Patch continue statements to point to here, before the loop operation (yes that's silly) */
	while (current->continueCount > 0 && current->continues[current->continueCount-1].offset > loopStart) {
		patchJump(current->continues[current->continueCount-1].offset);
		current->continueCount--;
	}

	emitByte(loopType);

	int offset = currentChunk()->count - loopStart + ((loopType == OP_LOOP_ITER) ? -1 : 2);
	if (offset > 0xFFFF) error("Loop jump offset is too large for opcode.");
	emitBytes(offset >> 8, offset);

	/* Patch break statements */
}

static void withStatement(void) {
	/* We only need this for block() */
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	KrkToken myPrevious = parser.previous;

	/* Collect the with token that started this statement */
	advance();

	beginScope();
	expression();

	if (match(TOKEN_AS)) {
		consume(TOKEN_IDENTIFIER, "Expected variable name after 'as'");
		size_t ind = identifierConstant(&parser.previous);
		declareVariable();
		defineVariable(ind);
	} else {
		/* Otherwise we want an unnamed local */
		anonymousLocal();
	}

	/* Storage for return / exception */
	anonymousLocal();

	/* Handler object */
	anonymousLocal();
	int withJump = emitJump(OP_PUSH_WITH);

	if (check(TOKEN_COMMA)) {
		parser.previous = myPrevious;
		withStatement(); /* Keep nesting */
	} else {
		consume(TOKEN_COLON, "Expected ',' or ':' after 'with' statement");

		beginScope();
		block(blockWidth,"with");
		endScope();
	}

	patchJump(withJump);
	emitByte(OP_CLEANUP_WITH);

	/* Scope exit pops context manager */
	endScope();
}

static void ifStatement(void) {
	/* Figure out what block level contains us so we can match our partner else */
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	KrkToken myPrevious = parser.previous;

	/* Collect the if token that started this statement */
	advance();

	/* Collect condition expression */
	expression();

	/* if EXPR: */
	consume(TOKEN_COLON, "Expected ':' after 'if' condition.");

	if (parser.hadError) return;

	int thenJump = emitJump(OP_POP_JUMP_IF_FALSE);

	/* Start a new scope and enter a block */
	beginScope();
	block(blockWidth,"if");
	endScope();

	if (parser.hadError) return;

	int elseJump = emitJump(OP_JUMP);
	patchJump(thenJump);

	/* See if we have a matching else block */
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (parser.current.length == blockWidth))) {
		/* This is complicated */
		KrkToken previous;
		if (blockWidth) {
			previous = parser.previous;
			advance();
		}
		if (match(TOKEN_ELSE) || check(TOKEN_ELIF)) {
			if (parser.current.type == TOKEN_ELIF || check(TOKEN_IF)) {
				parser.previous = myPrevious;
				ifStatement(); /* Keep nesting */
			} else {
				consume(TOKEN_COLON, "Expected ':' after 'else'.");
				beginScope();
				block(blockWidth,"else");
				endScope();
			}
		} else if (!check(TOKEN_EOF) && !check(TOKEN_EOL)) {
			krk_ungetToken(parser.current);
			parser.current = parser.previous;
			if (blockWidth) {
				parser.previous = previous;
			}
		} else {
			advance(); /* Ignore this blank indentation line */
		}
	}

	patchJump(elseJump);
}

static void patchBreaks(int loopStart) {
	/* Patch break statements to go here, after the loop operation and operand. */
	while (current->breakCount > 0 && current->breaks[current->breakCount-1].offset > loopStart) {
		patchJump(current->breaks[current->breakCount-1].offset);
		current->breakCount--;
	}
}

static void breakStatement(void) {
	if (current->breakSpace < current->breakCount + 1) {
		size_t old = current->breakSpace;
		current->breakSpace = GROW_CAPACITY(old);
		current->breaks = GROW_ARRAY(struct LoopExit,current->breaks,old,current->breakSpace);
	}

	for (size_t i = current->loopLocalCount; i < current->localCount; ++i) {
		emitByte(OP_POP);
	}
	current->breaks[current->breakCount++] = (struct LoopExit){emitJump(OP_JUMP),parser.previous};
}

static void continueStatement(void) {
	if (current->continueSpace < current->continueCount + 1) {
		size_t old = current->continueSpace;
		current->continueSpace = GROW_CAPACITY(old);
		current->continues = GROW_ARRAY(struct LoopExit,current->continues,old,current->continueSpace);
	}

	for (size_t i = current->loopLocalCount; i < current->localCount; ++i) {
		emitByte(OP_POP);
	}
	current->continues[current->continueCount++] = (struct LoopExit){emitJump(OP_JUMP),parser.previous};
}

static void optionalElse(size_t blockWidth) {
	KrkScanner scannerBefore = krk_tellScanner();
	Parser  parserBefore = parser;
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (parser.current.length == blockWidth))) {
		if (blockWidth) advance();
		if (match(TOKEN_ELSE)) {
			consume(TOKEN_COLON, "Expected ':' after 'else'.");
			beginScope();
			block(blockWidth,"else");
			endScope();
		} else {
			krk_rewindScanner(scannerBefore);
			parser = parserBefore;
		}
	}
}

static void whileStatement(void) {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();

	int loopStart = currentChunk()->count;
	int exitJump = 0;

	/* Identify two common infinite loops and optimize them (True and 1) */
	RewindState rewind = {recordChunk(currentChunk()), krk_tellScanner(), parser};
	if (!(match(TOKEN_TRUE) && match(TOKEN_COLON)) &&
	    !(match(TOKEN_NUMBER) && (parser.previous.length == 1 && *parser.previous.start == '1') && match(TOKEN_COLON))) {
		/* We did not match a common infinite loop, roll back... */
		krk_rewindScanner(rewind.oldScanner);
		parser = rewind.oldParser;

		/* Otherwise, compile a real loop condition. */
		expression();
		consume(TOKEN_COLON, "Expected ':' after 'while' condition.");

		exitJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);
	}

	int oldLocalCount = current->loopLocalCount;
	current->loopLocalCount = current->localCount;
	beginScope();
	block(blockWidth,"while");
	endScope();

	current->loopLocalCount = oldLocalCount;
	emitLoop(loopStart, OP_LOOP);

	if (exitJump) {
		patchJump(exitJump);
		emitByte(OP_POP);
	}

	/* else: block must still be compiled even if we optimized
	 * out the loop condition check... */
	optionalElse(blockWidth);

	patchBreaks(loopStart);
}

static void forStatement(void) {
	/* I'm not sure if I want this to be more like Python or C/Lox/etc. */
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();

	/* For now this is going to be kinda broken */
	beginScope();

	ssize_t loopInd = current->localCount;
	int sawComma = 0;
	ssize_t varCount = 0;
	int matchedEquals = 0;

	if (!check(TOKEN_IDENTIFIER)) {
		errorAtCurrent("Empty variable list in 'for'");
		return;
	}

	do {
		if (!check(TOKEN_IDENTIFIER)) break;
		ssize_t ind = parseVariable("Expected name for loop iterator.");
		if (parser.hadError) return;
		if (match(TOKEN_EQUAL)) {
			matchedEquals = 1;
			expression();
		} else {
			emitByte(OP_NONE);
		}
		defineVariable(ind);
		varCount++;
		if (check(TOKEN_COMMA)) sawComma = 1;
	} while (match(TOKEN_COMMA));

	int loopStart;
	int exitJump;
	int isIter = 0;

	if (!matchedEquals && match(TOKEN_IN)) {

		beginScope();
		expression();
		endScope();

		anonymousLocal();
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

		beginScope();
		expression(); /* condition */
		endScope();
		exitJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);

		if (check(TOKEN_SEMICOLON)) {
			advance();
			int bodyJump = emitJump(OP_JUMP);
			int incrementStart = currentChunk()->count;
			beginScope();
			do {
				expressionStatement();
			} while (match(TOKEN_COMMA));
			endScope();

			emitLoop(loopStart, OP_LOOP);
			loopStart = incrementStart;
			patchJump(bodyJump);
		}
	}

	consume(TOKEN_COLON,"Expected ':' after loop conditions.");

	int oldLocalCount = current->loopLocalCount;
	current->loopLocalCount = current->localCount;
	beginScope();
	block(blockWidth,"for");
	endScope();

	current->loopLocalCount = oldLocalCount;
	emitLoop(loopStart, isIter ? OP_LOOP_ITER : OP_LOOP);
	patchJump(exitJump);
	emitByte(OP_POP);
	optionalElse(blockWidth);
	patchBreaks(loopStart);
	endScope();
}

static void returnStatement(void) {
	if (check(TOKEN_EOL) || check(TOKEN_EOF)) {
		emitReturn();
	} else {
		if (current->type == TYPE_INIT) {
			error("__init__ may not return a value.");
		}
		parsePrecedence(PREC_ASSIGNMENT);
		emitByte(OP_RETURN);
	}
}

static void tryStatement(void) {
	size_t blockWidth = (parser.previous.type == TOKEN_INDENTATION) ? parser.previous.length : 0;
	advance();
	consume(TOKEN_COLON, "Expected ':' after 'try'.");

	/* Make sure we are in a local scope so this ends up on the stack */
	beginScope();
	int tryJump = emitJump(OP_PUSH_TRY);

	size_t exceptionObject = anonymousLocal();
	anonymousLocal(); /* Try */

	beginScope();
	block(blockWidth,"try");
	endScope();

	if (parser.hadError) return;

#define EXIT_JUMP_MAX 32
	int exitJumps = 1;
	int exitJumpOffsets[EXIT_JUMP_MAX] = {0};

	exitJumpOffsets[0] = emitJump(OP_JUMP);
	patchJump(tryJump);

	int nextJump = -1;

_anotherExcept:
	if (parser.hadError) return;
	if (blockWidth == 0 || (check(TOKEN_INDENTATION) && (parser.current.length == blockWidth))) {
		KrkToken previous;
		if (blockWidth) {
			previous = parser.previous;
			advance();
		}
		if (match(TOKEN_EXCEPT)) {
			if (nextJump != -1) {
				patchJump(nextJump);
				emitByte(OP_POP);
			}
			/* Match filter expression (should be class or tuple) */
			if (!check(TOKEN_COLON) && !check(TOKEN_AS)) {
				expression();
			} else {
				emitByte(OP_NONE);
			}
			emitByte(OP_FILTER_EXCEPT);
			nextJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);

			/* Match 'as' to rename exception */
			if (match(TOKEN_AS)) {
				consume(TOKEN_IDENTIFIER, "Expected identifier after 'as'.");
				current->locals[exceptionObject].name = parser.previous;
			} else {
				/* XXX Should we remove this now? */
				current->locals[exceptionObject].name = syntheticToken("exception");
			}

			size_t nameInd = renameLocal(exceptionObject, current->locals[exceptionObject].name);

			consume(TOKEN_COLON, "Expected ':' after 'except'.");
			beginScope();
			block(blockWidth,"except");
			endScope();

			current->codeobject->localNames[nameInd].deathday = (size_t)currentChunk()->count;

			if (exitJumps < EXIT_JUMP_MAX) {
				exitJumpOffsets[exitJumps++] = emitJump(OP_JUMP);
			} else {
				error("Too many 'except' clauses.");
				return;
			}

			goto _anotherExcept;
		} else if (match(TOKEN_FINALLY)) {
			consume(TOKEN_COLON, "Expected ':' after 'finally'.");
			for (int i = 0; i < exitJumps; ++i) {
				patchJump(exitJumpOffsets[i]);
			}
			size_t nameInd = renameLocal(exceptionObject, syntheticToken("__tmp"));
			emitByte(OP_BEGIN_FINALLY);
			exitJumps = 0;
			if (nextJump != -1) {
				emitByte(OP_NONE);
				patchJump(nextJump);
				emitByte(OP_POP);
			}
			beginScope();
			block(blockWidth,"finally");
			endScope();
			nextJump = -2;
			current->codeobject->localNames[nameInd].deathday = (size_t)currentChunk()->count;
			emitByte(OP_END_FINALLY);
		} else if (!check(TOKEN_EOL) && !check(TOKEN_EOF)) {
			krk_ungetToken(parser.current);
			parser.current = parser.previous;
			if (blockWidth) {
				parser.previous = previous;
			}
		} else {
			advance(); /* Ignore this blank indentation line */
		}
	}

	for (int i = 0; i < exitJumps; ++i) {
		patchJump(exitJumpOffsets[i]);
	}

	if (nextJump >= 0) {
		emitByte(OP_BEGIN_FINALLY);
		emitByte(OP_NONE);
		patchJump(nextJump);
		emitByte(OP_POP);
		emitByte(OP_END_FINALLY);
	}

	endScope(); /* will pop the exception handler */
}

static void raiseStatement(void) {
	parsePrecedence(PREC_ASSIGNMENT);

	if (match(TOKEN_FROM)) {
		parsePrecedence(PREC_ASSIGNMENT);
		emitByte(OP_RAISE_FROM);
	} else {
		emitByte(OP_RAISE);
	}
}

static size_t importModule(KrkToken * startOfName, int leadingDots) {
	size_t ind = 0;
	struct StringBuilder sb = {0};

	for (int i = 0; i < leadingDots; ++i) {
		pushStringBuilder(&sb, '.');
	}

	if (!(leadingDots && check(TOKEN_IMPORT))) {
		consume(TOKEN_IDENTIFIER, "Expected module name after 'import'.");
		if (parser.hadError) goto _freeImportName;
		pushStringBuilderStr(&sb, parser.previous.start, parser.previous.length);

		while (match(TOKEN_DOT)) {
			pushStringBuilderStr(&sb, parser.previous.start, parser.previous.length);
			consume(TOKEN_IDENTIFIER, "Expected module path element after '.'");
			if (parser.hadError) goto _freeImportName;
			pushStringBuilderStr(&sb, parser.previous.start, parser.previous.length);
		}
	}

	startOfName->start  = sb.bytes;
	startOfName->length = sb.length;

	ind = identifierConstant(startOfName);
	EMIT_OPERAND_OP(OP_IMPORT, ind);

_freeImportName:
	discardStringBuilder(&sb);
	return ind;
}

static void importStatement(void) {
	do {
		KrkToken firstName = parser.current;
		KrkToken startOfName = {0};
		size_t ind = importModule(&startOfName, 0);
		if (match(TOKEN_AS)) {
			consume(TOKEN_IDENTIFIER, "Expected identifier after 'as'.");
			ind = identifierConstant(&parser.previous);
		} else if (startOfName.length != firstName.length) {
			/**
			 * We imported foo.bar.baz and 'baz' is now on the stack with no name.
			 * But while doing that, we built a chain so that foo and foo.bar are
			 * valid modules that already exist in the module table. We want to
			 * have 'foo.bar.baz' be this new object, so remove 'baz', reimport
			 * 'foo' directly, and put 'foo' into the appropriate namespace.
			 */
			emitByte(OP_POP);
			parser.previous = firstName;
			ind = identifierConstant(&firstName);
			EMIT_OPERAND_OP(OP_IMPORT, ind);
		}
		declareVariable();
		defineVariable(ind);
	} while (match(TOKEN_COMMA));
}

static void optionsImport(void) {
	int expectCloseParen = 0;

	KrkToken compile_time_builtins = syntheticToken("compile_time_builtins");

	advance();
	consume(TOKEN_IMPORT, "__options__ is not a package\n");

	if (match(TOKEN_LEFT_PAREN)) {
		expectCloseParen = 1;
		startEatingWhitespace();
	}

	do {
		consume(TOKEN_IDENTIFIER, "Expected member name");

		/* Okay, what is it? */
		if (identifiersEqual(&parser.previous, &compile_time_builtins)) {
			current->optionsFlags |= OPTIONS_FLAG_COMPILE_TIME_BUILTINS;
		} else {
			error("'%.*s' is not a recognized __options__ import",
				(int)parser.previous.length, parser.previous.start);
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

static void fromImportStatement(void) {
	int expectCloseParen = 0;
	KrkToken startOfName = {0};
	int leadingDots = 0;

	KrkToken options = syntheticToken("__options__");
	if (check(TOKEN_IDENTIFIER) && identifiersEqual(&parser.current, &options)) {
		/* from __options__ import ... */
		optionsImport();
		return;
	}

	while (match(TOKEN_DOT)) {
		leadingDots++;
	}

	importModule(&startOfName, leadingDots);
	consume(TOKEN_IMPORT, "Expected 'import' after module name");
	if (match(TOKEN_LEFT_PAREN)) {
		expectCloseParen = 1;
		startEatingWhitespace();
	}
	do {
		consume(TOKEN_IDENTIFIER, "Expected member name");
		size_t member = identifierConstant(&parser.previous);
		emitBytes(OP_DUP, 0); /* Duplicate the package object so we can GET_PROPERTY on it? */
		EMIT_OPERAND_OP(OP_IMPORT_FROM, member);
		if (match(TOKEN_AS)) {
			consume(TOKEN_IDENTIFIER, "Expected identifier after 'as'");
			member = identifierConstant(&parser.previous);
		}
		if (current->scopeDepth) {
			/* Swaps the original module and the new possible local so it can be in the right place */
			emitByte(OP_SWAP);
		}
		declareVariable();
		defineVariable(member);
	} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_PAREN));
	if (expectCloseParen) {
		stopEatingWhitespace();
		consume(TOKEN_RIGHT_PAREN, "Expected ')' after import list started with '('");
	}
	emitByte(OP_POP); /* Pop the remaining copy of the module. */
}

static void delStatement(void) {
	do {
		current->delSatisfied = 0;
		parsePrecedence(PREC_DEL_TARGET);
		if (!current->delSatisfied) {
			errorAtCurrent("Invalid del target");
		}
	} while (match(TOKEN_COMMA));
}

static void assertStatement(void) {
	expression();
	int elseJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);

	KrkToken assertionError = syntheticToken("AssertionError");
	size_t ind = identifierConstant(&assertionError);
	EMIT_OPERAND_OP(OP_GET_GLOBAL, ind);
	int args = 0;

	if (match(TOKEN_COMMA)) {
		expression();
		args = 1;
	}

	EMIT_OPERAND_OP(OP_CALL, args);
	emitByte(OP_RAISE);

	patchJump(elseJump);
	emitByte(OP_POP);
}

static void statement(void) {
	if (match(TOKEN_EOL) || match(TOKEN_EOF)) {
		return; /* Meaningless blank line */
	}

	if (check(TOKEN_IF)) {
		ifStatement();
	} else if (check(TOKEN_WHILE)) {
		whileStatement();
	} else if (check(TOKEN_FOR)) {
		forStatement();
	} else if (check(TOKEN_ASYNC)) {
		asyncDeclaration(0);
	} else if (check(TOKEN_TRY)) {
		tryStatement();
	} else if (check(TOKEN_WITH)) {
		withStatement();
	} else {
		/* These statements don't eat line feeds, so we need expect to see another one. */
_anotherSimpleStatement:
		if (match(TOKEN_RAISE)) {
			raiseStatement();
		} else if (match(TOKEN_RETURN)) {
			returnStatement();
		} else if (match(TOKEN_IMPORT)) {
			importStatement();
		} else if (match(TOKEN_FROM)) {
			fromImportStatement();
		} else if (match(TOKEN_BREAK)) {
			breakStatement();
		} else if (match(TOKEN_CONTINUE)) {
			continueStatement();
		} else if (match(TOKEN_DEL)) {
			delStatement();
		} else if (match(TOKEN_ASSERT)) {
			assertStatement();
		} else if (match(TOKEN_PASS)) {
			/* Do nothing. */
		} else {
			expressionStatement();
		}
		if (match(TOKEN_SEMICOLON)) goto _anotherSimpleStatement;
		if (!match(TOKEN_EOL) && !match(TOKEN_EOF)) {
			switch (parser.current.type) {
				case TOKEN_RIGHT_BRACE:
				case TOKEN_RIGHT_PAREN:
				case TOKEN_RIGHT_SQUARE:
					errorAtCurrent("Unmatched '%.*s'",
						(int)parser.current.length, parser.current.start);
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
						(int)parser.current.length, parser.current.start);
			}
		}
	}
}

static void yield(int exprType) {
	if (current->type == TYPE_MODULE ||
		current->type == TYPE_INIT ||
		current->type == TYPE_CLASS) {
		error("'yield' outside function");
		return;
	}
	current->codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR;
	if (match(TOKEN_FROM)) {
		parsePrecedence(PREC_ASSIGNMENT);
		emitByte(OP_INVOKE_ITER);
		emitByte(OP_NONE);
		size_t loopContinue = currentChunk()->count;
		size_t exitJump = emitJump(OP_YIELD_FROM);
		emitByte(OP_YIELD);
		emitLoop(loopContinue, OP_LOOP);
		patchJump(exitJump);
	} else if (check(TOKEN_EOL) || check(TOKEN_EOF) || check(TOKEN_RIGHT_PAREN) || check(TOKEN_RIGHT_BRACE)) {
		emitByte(OP_NONE);
		emitByte(OP_YIELD);
	} else {
		parsePrecedence(PREC_ASSIGNMENT);
		emitByte(OP_YIELD);
	}
	invalidTarget(exprType, "yield");
}

static void await(int exprType) {
	if (!isCoroutine(current->type)) {
		error("'await' outside async function");
		return;
	}

	parsePrecedence(PREC_ASSIGNMENT);
	emitByte(OP_INVOKE_AWAIT);
	emitByte(OP_NONE);
	size_t loopContinue = currentChunk()->count;
	size_t exitJump = emitJump(OP_YIELD_FROM);
	emitByte(OP_YIELD);
	emitLoop(loopContinue, OP_LOOP);
	patchJump(exitJump);
	invalidTarget(exprType, "await");
}

static void unot_(int exprType) {
	parsePrecedence(PREC_NOT);
	emitByte(OP_NOT);
	invalidTarget(exprType, "operator");
}

static void unary(int exprType) {
	KrkTokenType operatorType = parser.previous.type;
	parsePrecedence(PREC_FACTOR);
	invalidTarget(exprType, "operator");
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

static void string(int exprType) {
	/* We'll just build with a flexible array like everything else. */
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = 0;
#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = GROW_ARRAY(char, stringBytes, old, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)

#define PUSH_HEX(n, type) do { \
	char tmpbuf[10] = {0}; \
	for (size_t i = 0; i < n; ++i) { \
		if (c + i + 2 == end || !isHex(c[i+2])) { \
			error("truncated \\%c escape", type); \
			FREE_ARRAY(char,stringBytes,stringCapacity); \
			return; \
		} \
		tmpbuf[i] = c[i+2]; \
	} \
	unsigned long value = strtoul(tmpbuf, NULL, 16); \
	if (value >= 0x110000) { \
		error("invalid codepoint in \\%c escape", type); \
	} \
	if (isBytes) { \
		PUSH_CHAR(value); \
		break; \
	} \
	unsigned char bytes[5] = {0}; \
	size_t len = krk_codepointToBytes(value, bytes); \
	for (size_t i = 0; i < len; i++) PUSH_CHAR(bytes[i]); \
} while (0)

	int isBytes = (parser.previous.type == TOKEN_PREFIX_B);
	int isFormat = (parser.previous.type == TOKEN_PREFIX_F);
	int isRaw = (parser.previous.type == TOKEN_PREFIX_R);

	int atLeastOne = 0;
	const char * lineBefore = krk_tellScanner().linePtr;
	size_t lineNo = krk_tellScanner().line;

	if ((isBytes || isFormat || isRaw) && !(match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
		error("Expected string after prefix? (Internal error - scanner should not have produced this.)");
		return;
	}

	if (isRaw) {
		emitConstant(OBJECT_VAL(krk_copyString(
			parser.previous.start + (parser.previous.type == TOKEN_BIG_STRING ? 3 : 1),
			parser.previous.length - (parser.previous.type == TOKEN_BIG_STRING ? 6 : 2))));
		return;
	}

	/* This should capture everything but the quotes. */
	do {
		int type = parser.previous.type == TOKEN_BIG_STRING ? 3 : 1;
		const char * c = parser.previous.start + type;
		const char * end = parser.previous.start + parser.previous.length - type;
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
					FREE_ARRAY(char,stringBytes,stringCapacity);
					return;
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
				if (!atLeastOne || stringLength) { /* Make sure there's a string for coersion reasons */
					emitConstant(OBJECT_VAL(krk_copyString(stringBytes,stringLength)));
					if (atLeastOne) emitByte(OP_ADD);
					atLeastOne = 1;
				}
				const char * start = c+1;
				stringLength = 0;
				KrkScanner beforeExpression = krk_tellScanner();
				Parser  parserBefore = parser;
				KrkScanner inner = (KrkScanner){.start=c+1, .cur=c+1, .linePtr=lineBefore, .line=lineNo, .startOfLine = 0, .hasUnget = 0};
				krk_rewindScanner(inner);
				advance();
				parsePrecedence(PREC_COMMA); /* allow unparen'd tuples, but not assignments, as expressions in f-strings */
				if (parser.hadError) {
					FREE_ARRAY(char,stringBytes,stringCapacity);
					return;
				}
				inner = krk_tellScanner(); /* To figure out how far to advance c */
				krk_rewindScanner(beforeExpression); /* To get us back to where we were with a string token */
				parser = parserBefore;
				c = inner.start;
				KrkToken which = syntheticToken("str");
				int hasEq = 0;
				while (*c == ' ') c++;
				if (*c == '=') {
					c++;
					while (*c == ' ') c++;
					emitConstant(OBJECT_VAL(krk_copyString(start,c-start)));
					emitByte(OP_SWAP);
					hasEq = 1;
				}
				if (*c == '!') {
					c++;
					/* Conversion specifiers, must only be one */
					if (*c == 'r') {
						which = syntheticToken("repr");
					} else if (*c == 's') {
						which = syntheticToken("str");
					} else {
						error("Unsupported conversion flag '%c' for f-string expression.", *c);
						goto _cleanupError;
					}
					c++;
				}
				size_t ind = identifierConstant(&which);
				EMIT_OPERAND_OP(OP_GET_GLOBAL, ind);
				emitByte(OP_SWAP);
				emitBytes(OP_CALL, 1);
				if (*c == ':') {
					/* TODO format specs */
					error("Format spec not supported in f-string (GH-10)");
					goto _cleanupError;
				}
				if (*c != '}') {
					error("Expected closing '}' after expression in f-string");
					goto _cleanupError;
				}
				if (hasEq) emitByte(OP_ADD);
				if (atLeastOne) emitByte(OP_ADD);
				atLeastOne = 1;
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
	} while ((!isBytes || match(TOKEN_PREFIX_B)) && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)));
	if (isBytes && (match(TOKEN_STRING) || match(TOKEN_BIG_STRING))) {
		error("Can not mix bytes and string literals");
		goto _cleanupError;
	}
	if (isBytes) {
		stringBytes = krk_reallocate(stringBytes, stringCapacity, stringLength);
		KrkBytes * bytes = krk_newBytes(0,NULL);
		bytes->bytes = (uint8_t*)stringBytes;
		bytes->length = stringLength;
		emitConstant(OBJECT_VAL(bytes));
		return;
	}
	if (!isFormat || stringLength || !atLeastOne) {
		emitConstant(OBJECT_VAL(krk_copyString(stringBytes,stringLength)));
		if (atLeastOne) emitByte(OP_ADD);
	}
	FREE_ARRAY(char,stringBytes,stringCapacity);
#undef PUSH_CHAR
	return;
_cleanupError:
	FREE_ARRAY(char,stringBytes,stringCapacity);
}

static size_t addUpvalue(Compiler * compiler, ssize_t index, int isLocal) {
	size_t upvalueCount = compiler->codeobject->upvalueCount;
	for (size_t i = 0; i < upvalueCount; ++i) {
		Upvalue * upvalue = &compiler->upvalues[i];
		if ((ssize_t)upvalue->index == index && upvalue->isLocal == isLocal) {
			return i;
		}
	}
	if (upvalueCount + 1 > compiler->upvaluesSpace) {
		size_t old = compiler->upvaluesSpace;
		compiler->upvaluesSpace = GROW_CAPACITY(old);
		compiler->upvalues = GROW_ARRAY(Upvalue,compiler->upvalues,old,compiler->upvaluesSpace);
	}
	compiler->upvalues[upvalueCount].isLocal = isLocal;
	compiler->upvalues[upvalueCount].index = index;
	return compiler->codeobject->upvalueCount++;
}

static ssize_t resolveUpvalue(Compiler * compiler, KrkToken * name) {
	if (compiler->enclosing == NULL) return -1;
	ssize_t local = resolveLocal(compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].isCaptured = 1;
		return addUpvalue(compiler, local, 1);
	}
	ssize_t upvalue = resolveUpvalue(compiler->enclosing, name);
	if (upvalue != -1) {
		return addUpvalue(compiler, upvalue, 0);
	}
	return -1;
}

#define OP_NONE_LONG -1
#define DO_VARIABLE(opset,opget,opdel) do { \
	if (exprType == EXPR_ASSIGN_TARGET) { \
		if (matchComplexEnd()) { \
			EMIT_OPERAND_OP(opset, arg); \
			break; \
		} \
		exprType = EXPR_NORMAL; \
	} \
	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) { \
		parsePrecedence(PREC_ASSIGNMENT); \
		EMIT_OPERAND_OP(opset, arg); \
	} else if (exprType == EXPR_CAN_ASSIGN && matchAssignment()) { \
		EMIT_OPERAND_OP(opget, arg); \
		assignmentValue(); \
		EMIT_OPERAND_OP(opset, arg); \
	} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel()) {\
		if (opdel == OP_NONE) { emitByte(OP_NONE); EMIT_OPERAND_OP(opset, arg); } \
		else { EMIT_OPERAND_OP(opdel, arg); } \
	} else { \
		EMIT_OPERAND_OP(opget, arg); \
	} } while (0)

static void namedVariable(KrkToken name, int exprType) {
	if (current->type == TYPE_CLASS) {
		/* Only at the class body level, see if this is a class property. */
		struct IndexWithNext * properties = current->properties;
		while (properties) {
			KrkString * constant = AS_STRING(currentChunk()->constants.values[properties->ind]);
			if (constant->length == name.length && !memcmp(constant->chars, name.start, name.length)) {
				ssize_t arg = properties->ind;
				EMIT_OPERAND_OP(OP_GET_LOCAL, 0);
				DO_VARIABLE(OP_SET_PROPERTY, OP_GET_PROPERTY, OP_NONE);
				return;
			}
			properties = properties->next;
		}
	}
	ssize_t arg = resolveLocal(current, &name);
	if (arg != -1) {
		DO_VARIABLE(OP_SET_LOCAL, OP_GET_LOCAL, OP_NONE);
	} else if ((arg = resolveUpvalue(current, &name)) != -1) {
		DO_VARIABLE(OP_SET_UPVALUE, OP_GET_UPVALUE, OP_NONE);
	} else {
		if ((current->optionsFlags & OPTIONS_FLAG_COMPILE_TIME_BUILTINS) && *name.start != '_') {
			KrkValue value;
			if (krk_tableGet_fast(&vm.builtins->fields, krk_copyString(name.start, name.length), &value)) {
				if ((exprType == EXPR_ASSIGN_TARGET && matchComplexEnd()) ||
					(exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) ||
					(exprType == EXPR_CAN_ASSIGN && matchAssignment())) {
					error("Can not assign to '%.*s' when 'compile_time_builtins' is enabled.",
						(int)name.length, name.start);
				} else if (exprType == EXPR_DEL_TARGET && checkEndOfDel()) {
					error("Can not delete '%.*s' when 'compile_time_builtins' is enabled.",
						(int)name.length, name.start);
				} else {
					emitConstant(value);
				}
				return;
			}
		}
		arg = identifierConstant(&name);
		DO_VARIABLE(OP_SET_GLOBAL, OP_GET_GLOBAL, OP_DEL_GLOBAL);
	}
}
#undef DO_VARIABLE

static void variable(int exprType) {
	namedVariable(parser.previous, exprType);
}

static void super_(int exprType) {
	consume(TOKEN_LEFT_PAREN, "Expected 'super' to be called.");

	/* Argument time */
	if (match(TOKEN_RIGHT_PAREN)) {
		if (!isMethod(current->type)) {
			error("super() outside of a method body requires arguments");
			return;
		}
		namedVariable(currentClass->name, 0);
		EMIT_OPERAND_OP(OP_GET_LOCAL, 0);
	} else {
		expression();
		if (match(TOKEN_COMMA)) {
			expression();
		} else {
			emitByte(OP_UNSET);
		}
		consume(TOKEN_RIGHT_PAREN, "Expected ')' after argument list");
	}
	consume(TOKEN_DOT, "Expected a field of 'super()' to be referenced.");
	consume(TOKEN_IDENTIFIER, "Expected a field name.");
	size_t ind = identifierConstant(&parser.previous);
	EMIT_OPERAND_OP(OP_GET_SUPER, ind);
}

static void comprehensionInner(KrkScanner scannerBefore, Parser parserBefore, void (*body)(size_t), size_t arg) {
	ssize_t loopInd = current->localCount;
	ssize_t varCount = 0;
	int sawComma = 0;
	if (!check(TOKEN_IDENTIFIER)) {
		errorAtCurrent("Empty variable list in comprehension");
		return;
	}
	do {
		if (!check(TOKEN_IDENTIFIER)) break;
		defineVariable(parseVariable("Expected name for iteration variable."));
		if (parser.hadError) return;
		emitByte(OP_NONE);
		defineVariable(loopInd);
		varCount++;
		if (check(TOKEN_COMMA)) sawComma = 1;
	} while (match(TOKEN_COMMA));

	consume(TOKEN_IN, "Only iterator loops (for ... in ...) are allowed in generator expressions.");

	beginScope();
	parsePrecedence(PREC_OR); /* Otherwise we can get trapped on a ternary */
	endScope();

	anonymousLocal();
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
		parsePrecedence(PREC_OR);
		int acceptJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);
		emitLoop(loopStart, OP_LOOP);
		patchJump(acceptJump);
		emitByte(OP_POP); /* Pop condition */
	}

	beginScope();
	if (match(TOKEN_FOR)) {
		comprehensionInner(scannerBefore, parserBefore, body, arg);
	} else {
		KrkScanner scannerAfter = krk_tellScanner();
		Parser  parserAfter = parser;
		krk_rewindScanner(scannerBefore);
		parser = parserBefore;

		body(arg);

		krk_rewindScanner(scannerAfter);
		parser = parserAfter;
	}
	endScope();

	emitLoop(loopStart, OP_LOOP_ITER);
	patchJump(exitJump);
	emitByte(OP_POP);
}

static void yieldInner(size_t arg) {
	(void)arg;
	expression();
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
static void generatorExpression(KrkScanner scannerBefore, Parser parserBefore, void (*body)(size_t)) {
	parser.previous = syntheticToken("<genexpr>");
	Compiler subcompiler;
	initCompiler(&subcompiler, TYPE_FUNCTION);
	subcompiler.codeobject->chunk.filename = subcompiler.enclosing->codeobject->chunk.filename;
	subcompiler.codeobject->obj.flags |= KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR;

	beginScope();
	comprehensionInner(scannerBefore, parserBefore, body, 0);
	endScope();

	KrkCodeObject *subfunction = endCompiler();
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(subfunction));
	EMIT_OPERAND_OP(OP_CLOSURE, indFunc);
	doUpvalues(&subcompiler, subfunction);
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
static void comprehensionExpression(KrkScanner scannerBefore, Parser parserBefore, void (*body)(size_t), int type) {
	Compiler subcompiler;
	initCompiler(&subcompiler, TYPE_FUNCTION);
	subcompiler.codeobject->chunk.filename = subcompiler.enclosing->codeobject->chunk.filename;

	beginScope();

	/* Build an empty collection to fill up. */
	emitBytes(type,0);
	size_t ind = anonymousLocal();

	beginScope();
	comprehensionInner(scannerBefore, parserBefore, body, ind);
	endScope();

	emitByte(OP_RETURN);

	KrkCodeObject *subfunction = endCompiler();
	size_t indFunc = krk_addConstant(currentChunk(), OBJECT_VAL(subfunction));
	EMIT_OPERAND_OP(OP_CLOSURE, indFunc);
	doUpvalues(&subcompiler, subfunction);
	freeCompiler(&subcompiler);
	emitBytes(OP_CALL, 0);
}

/**
 * @brief Parse the inside of a set of parens.
 *
 * Used to parse general expression groupings as well as generator expressions.
 *
 * @param exprType Assignment target type.
 */
static void parens(int exprType) {
	/* Record parser state before processing contents. */
	ChunkRecorder before = recordChunk(currentChunk());
	KrkScanner scannerBefore = krk_tellScanner();
	Parser  parserBefore = parser;

	/*
	 * Generator expressions are not valid assignment targets, nor are
	 * an empty set of parentheses (empty tuple). A single target in
	 * parens, or a list of targets can be assigned to.
	 */
	int maybeValidAssignment = 0;

	size_t argCount = 0;

	/* Whitespace is ignored inside of parens */
	startEatingWhitespace();

	if (check(TOKEN_RIGHT_PAREN)) {
		/* Empty paren pair () is an empty tuple. */
		emitBytes(OP_TUPLE,0);
	} else {
		parsePrecedence(PREC_CAN_ASSIGN);
		maybeValidAssignment = 1;
		argCount = 1;

		if (match(TOKEN_FOR)) {
			/* Parse generator expression. */
			maybeValidAssignment = 0;
			rewindChunk(currentChunk(), before);
			generatorExpression(scannerBefore, parserBefore, yieldInner);
		} else if (match(TOKEN_COMMA)) {
			/* Parse as tuple literal. */
			if (!check(TOKEN_RIGHT_PAREN)) {
				/* (expr,) is a valid single-element tuple, so we need to check for that. */
				do {
					expression();
					argCount++;
				} while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_PAREN));
			}
			EMIT_OPERAND_OP(OP_TUPLE, argCount);
		}
	}

	stopEatingWhitespace();

	if (!match(TOKEN_RIGHT_PAREN)) {
		switch (parser.current.type) {
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
			complexAssignment(before, scannerBefore, parserBefore, argCount, 1);
		}
	} else if (exprType == EXPR_ASSIGN_TARGET && (check(TOKEN_EQUAL) || check(TOKEN_COMMA) || check(TOKEN_RIGHT_PAREN))) {
		if (!argCount) {
			error("Can not assign to empty target list.");
		} else if (!maybeValidAssignment) {
			error("Can not assign to generator expression.");
		} else {
			rewindChunk(currentChunk(), before);
			complexAssignmentTargets(scannerBefore, parserBefore, argCount, 2);
			if (!matchComplexEnd()) {
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
static void listInner(size_t arg) {
	expression();
	EMIT_OPERAND_OP(OP_LIST_APPEND, arg);
}

/**
 * @brief Parse an expression beginning with a set of square brackets.
 *
 * Square brackets in this context are either a list literal or a list comprehension.
 */
static void list(int exprType) {
	ChunkRecorder before = recordChunk(currentChunk());

	startEatingWhitespace();

	if (!check(TOKEN_RIGHT_SQUARE)) {
		KrkScanner scannerBefore = krk_tellScanner();
		Parser  parserBefore = parser;
		expression();

		if (match(TOKEN_FOR)) {
			/* Roll back the earlier compiler */
			rewindChunk(currentChunk(), before);
			/* Nested fun times */
			parser.previous = syntheticToken("<listcomp>");
			comprehensionExpression(scannerBefore, parserBefore, listInner, OP_MAKE_LIST);
		} else {
			size_t argCount = 1;
			while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_SQUARE)) {
				expression();
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
static void dictInner(size_t arg) {
	expression(); /* Key */
	consume(TOKEN_COLON, "Expected ':' after dict key.");
	expression(); /* Value */
	EMIT_OPERAND_OP(OP_DICT_SET, arg);
}

/**
 * @brief Parse the expression part of a set comprehension.
 */
static void setInner(size_t arg) {
	expression();
	EMIT_OPERAND_OP(OP_SET_ADD, arg);
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
static void dict(int exprType) {
	ChunkRecorder before = recordChunk(currentChunk());

	startEatingWhitespace();

	if (!check(TOKEN_RIGHT_BRACE)) {
		KrkScanner scannerBefore = krk_tellScanner();
		Parser  parserBefore = parser;

		expression();
		if (check(TOKEN_COMMA) || check(TOKEN_RIGHT_BRACE)) {
			/* One expression, must be a set literal. */
			size_t argCount = 1;
			while (match(TOKEN_COMMA)) {
				expression();
				argCount++;
			}
			EMIT_OPERAND_OP(OP_MAKE_SET, argCount);
		} else if (match(TOKEN_FOR)) {
			/* One expression followed by 'for': set comprehension. */
			rewindChunk(currentChunk(), before);
			parser.previous = syntheticToken("<setcomp>");
			comprehensionExpression(scannerBefore, parserBefore, setInner, OP_MAKE_SET);
		} else {
			/* Anything else must be a colon indicating a dictionary. */
			consume(TOKEN_COLON, "Expected ':' after dict key.");
			expression();

			if (match(TOKEN_FOR)) {
				/* Dictionary comprehension */
				rewindChunk(currentChunk(), before);
				parser.previous = syntheticToken("<dictcomp>");
				comprehensionExpression(scannerBefore, parserBefore, dictInner, OP_MAKE_DICT);
			} else {
				/*
				 * The operand to MAKE_DICT is double the number of entries,
				 * as it is the number of stack slots to consume to produce
				 * the dict: one for each key, one for each value.
				 */
				size_t argCount = 2;
				while (match(TOKEN_COMMA) && !check(TOKEN_RIGHT_BRACE)) {
					expression();
					consume(TOKEN_COLON, "Expected ':' after dict key.");
					expression();
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

static void ternary(int exprType, RewindState *rewind) {
	rewindChunk(currentChunk(), rewind->before);

	parsePrecedence(PREC_OR);

	int thenJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);
	consume(TOKEN_ELSE, "Expected 'else' after ternary condition");

	parsePrecedence(PREC_TERNARY);

	KrkScanner outScanner = krk_tellScanner();
	Parser outParser = parser;

	int elseJump = emitJump(OP_JUMP);
	patchJump(thenJump);
	emitByte(OP_POP);

	krk_rewindScanner(rewind->oldScanner);
	parser = rewind->oldParser;
	parsePrecedence(PREC_OR);
	patchJump(elseJump);

	krk_rewindScanner(outScanner);
	parser = outParser;
}

static void complexAssignmentTargets(KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized) {
	emitBytes(OP_DUP, 0);

	if (targetCount > 0) {
		EMIT_OPERAND_OP(OP_UNPACK,targetCount);
		EMIT_OPERAND_OP(OP_REVERSE,targetCount);
	}

	/* Rewind */
	krk_rewindScanner(oldScanner);
	parser = oldParser;

	/* Parse assignment targets */
	size_t checkTargetCount = 0;
	do {
		checkTargetCount++;
		parsePrecedence(PREC_MUST_ASSIGN);
		emitByte(OP_POP);

		if (checkTargetCount == targetCount && parser.previous.type == TOKEN_COMMA) {
			if (!match(parenthesized ? TOKEN_RIGHT_PAREN : TOKEN_EQUAL)) {
				goto _errorAtCurrent;
			}
		}

		if (checkTargetCount == targetCount && parenthesized) {
			if (parser.previous.type != TOKEN_RIGHT_PAREN) {
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

	} while (parser.previous.type != TOKEN_EQUAL && !parser.hadError);

_errorAtCurrent:
	errorAtCurrent("Invalid complex assignment target");
}

static void complexAssignment(ChunkRecorder before, KrkScanner oldScanner, Parser oldParser, size_t targetCount, int parenthesized) {

	rewindChunk(currentChunk(), before);
	parsePrecedence(PREC_ASSIGNMENT);

	/* Store end state */
	KrkScanner outScanner = krk_tellScanner();
	Parser outParser = parser;

	complexAssignmentTargets(oldScanner,oldParser,targetCount,parenthesized);

	/* Restore end state */
	krk_rewindScanner(outScanner);
	parser = outParser;
}

static void comma(int exprType, RewindState *rewind) {
	size_t expressionCount = 1;
	do {
		if (!getRule(parser.current.type)->prefix) break;
		expressionCount++;
		parsePrecedence(PREC_TERNARY);
	} while (match(TOKEN_COMMA));

	EMIT_OPERAND_OP(OP_TUPLE,expressionCount);

	if (exprType == EXPR_CAN_ASSIGN && match(TOKEN_EQUAL)) {
		complexAssignment(rewind->before, rewind->oldScanner, rewind->oldParser, expressionCount, 0);
	}
}

static void call(int exprType, RewindState *rewind) {
	startEatingWhitespace();
	size_t argCount = 0, specialArgs = 0, keywordArgs = 0, seenKeywordUnpacking = 0;
	if (!check(TOKEN_RIGHT_PAREN)) {
		size_t chunkBefore = currentChunk()->count;
		KrkScanner scannerBefore = krk_tellScanner();
		Parser  parserBefore = parser;
		do {
			if (check(TOKEN_RIGHT_PAREN)) break;
			if (match(TOKEN_ASTERISK) || check(TOKEN_POW)) {
				specialArgs++;
				if (match(TOKEN_POW)) {
					seenKeywordUnpacking = 1;
					emitBytes(OP_EXPAND_ARGS, 2); /* creates a KWARGS_DICT */
					expression(); /* Expect dict */
					continue;
				} else {
					if (seenKeywordUnpacking) {
						error("Iterable expansion follows keyword argument unpacking.");
						return;
					}
					emitBytes(OP_EXPAND_ARGS, 1); /* creates a KWARGS_LIST */
					expression();
					continue;
				}
			}
			if (match(TOKEN_IDENTIFIER)) {
				KrkToken argName = parser.previous;
				if (check(TOKEN_EQUAL)) {
					/* This is a keyword argument. */
					advance();
					/* Output the name */
					size_t ind = identifierConstant(&argName);
					EMIT_OPERAND_OP(OP_CONSTANT, ind);
					expression();
					keywordArgs++;
					specialArgs++;
					continue;
				} else {
					/*
					 * This is a regular argument that happened to start with an identifier,
					 * roll it back so we can process it that way.
					 */
					krk_ungetToken(parser.current);
					parser.current = argName;
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
				expression();
				specialArgs++;
				continue;
			}
			expression();
			if (argCount == 0 && match(TOKEN_FOR)) {
				currentChunk()->count = chunkBefore;
				generatorExpression(scannerBefore, parserBefore, yieldInner);
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
	} else {
		EMIT_OPERAND_OP(OP_CALL, argCount);
	}

	invalidTarget(exprType, "function call");
}

static void and_(int exprType, RewindState *rewind) {
	int endJump = emitJump(OP_JUMP_IF_FALSE_OR_POP);
	parsePrecedence(PREC_AND);
	patchJump(endJump);
	invalidTarget(exprType, "operator");
}

static void or_(int exprType, RewindState *rewind) {
	int endJump = emitJump(OP_JUMP_IF_TRUE_OR_POP);
	parsePrecedence(PREC_OR);
	patchJump(endJump);
	invalidTarget(exprType, "operator");
}

static void parsePrecedence(Precedence precedence) {
	RewindState rewind = {recordChunk(currentChunk()), krk_tellScanner(), parser};

	advance();
	ParsePrefixFn prefixRule = getRule(parser.previous.type)->prefix;
	if (prefixRule == NULL) {
		switch (parser.previous.type) {
			case TOKEN_RIGHT_BRACE:
			case TOKEN_RIGHT_PAREN:
			case TOKEN_RIGHT_SQUARE:
				error("Unmatched '%.*s'",
					(int)parser.previous.length, parser.previous.start);
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
					(int)parser.previous.length, parser.previous.start);
		}
		return;
	}
	int exprType = 0;
	if (precedence <= PREC_ASSIGNMENT || precedence == PREC_CAN_ASSIGN) exprType = EXPR_CAN_ASSIGN;
	if (precedence == PREC_MUST_ASSIGN) exprType = EXPR_ASSIGN_TARGET;
	if (precedence == PREC_DEL_TARGET) exprType = EXPR_DEL_TARGET;
	prefixRule(exprType);
	while (precedence <= getRule(parser.current.type)->precedence) {
		if (parser.hadError) {
			skipToEnd();
			return;
		}

		if (exprType == EXPR_ASSIGN_TARGET && (parser.previous.type == TOKEN_COMMA ||
			parser.previous.type == TOKEN_EQUAL)) break;
		advance();
		ParseInfixFn infixRule = getRule(parser.previous.type)->infix;
		infixRule(exprType, &rewind);
	}

	if (exprType == EXPR_CAN_ASSIGN && matchAssignment()) {
		error("Invalid assignment target");
	}
}


#ifdef ENABLE_THREADING
static volatile int _compilerLock = 0;
#endif

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
KrkCodeObject * krk_compile(const char * src, char * fileName) {
	/* Allow only one compiler across threads at a time. */
	_obtain_lock(_compilerLock);

	/* Point a new scanner at the source. */
	krk_initScanner(src);

	/* Reset parser state. */
	memset(&parser, 0, sizeof(parser));

	/* Start compiling a new function. */
	Compiler compiler;
	initCompiler(&compiler, TYPE_MODULE);
	compiler.codeobject->chunk.filename = krk_copyString(fileName, strlen(fileName));
	compiler.codeobject->name = krk_copyString("<module>",8);

	/* Start reading tokens from the scanner... */
	advance();

	/*
	 * If we haven't already assigned a docstring to the current module,
	 * check if the first token of the file is a string and use that as
	 * the docstring.
	 */
	if (krk_currentThread.module) {
		KrkValue doc;
		if (!krk_tableGet(&krk_currentThread.module->fields, OBJECT_VAL(krk_copyString("__doc__", 7)), &doc)) {
			if (match(TOKEN_STRING) || match(TOKEN_BIG_STRING)) {
				string(EXPR_NORMAL);
				krk_attachNamedObject(&krk_currentThread.module->fields, "__doc__",
					(KrkObj*)AS_STRING(currentChunk()->constants.values[currentChunk()->constants.count-1]));
				emitByte(OP_POP); /* string() actually put an instruction for that, pop its result */
				consume(TOKEN_EOL,"Garbage after docstring");
			} else {
				krk_attachNamedValue(&krk_currentThread.module->fields, "__doc__", NONE_VAL());
			}
		}
	}

	/* Parse top-level declarations... */
	while (!match(TOKEN_EOF)) {
		declaration();

		/* Skip over redundant whitespace */
		if (check(TOKEN_EOL) || check(TOKEN_INDENTATION) || check(TOKEN_EOF)) {
			advance();
		}
	}

	KrkCodeObject * function = endCompiler();
	freeCompiler(&compiler);

	/*
	 * We'll always get something out of endCompiler even if it
	 * wasn't fully compiled, so be sure to check for a syntax
	 * error and return NULL
	 */
	if (parser.hadError) function = NULL;

	_release_lock(_compilerLock);
	return function;
}

/**
 * @brief GC scan for compiler-owned references.
 *
 * Called by the garbage collector during the scan phase
 * to mark references held by the compiler.
 */
void krk_markCompilerRoots(void) {
	Compiler * compiler = current;
	while (compiler != NULL) {
		if (compiler->enclosed && compiler->enclosed->codeobject) krk_markObject((KrkObj*)compiler->enclosed->codeobject);
		krk_markObject((KrkObj*)compiler->codeobject);
		compiler = compiler->enclosing;
	}
}

#ifdef KRK_NO_SCAN_TRACING
# define RULE(token, a, b, c) [token] = {a, b, c}
#else
# define RULE(token, a, b, c) [token] = {# token, a, b, c}
#endif

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
	RULE(TOKEN_DOT,           NULL,     dot,      PREC_PRIMARY),
	RULE(TOKEN_LEFT_PAREN,    parens,   call,     PREC_PRIMARY),
	RULE(TOKEN_LEFT_SQUARE,   list,     getitem,  PREC_PRIMARY),
	RULE(TOKEN_LEFT_BRACE,    dict,     NULL,     PREC_NONE),
	RULE(TOKEN_RIGHT_PAREN,   NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_RIGHT_SQUARE,  NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_RIGHT_BRACE,   NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_COLON,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_SEMICOLON,     NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_EQUAL,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_WALRUS,        NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_PLUS_EQUAL,    NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_MINUS_EQUAL,   NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_PLUS_PLUS,     NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_MINUS_MINUS,   NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_CARET_EQUAL,   NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_PIPE_EQUAL,    NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_LSHIFT_EQUAL,  NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_RSHIFT_EQUAL,  NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_AMP_EQUAL,     NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_SOLIDUS_EQUAL, NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_DSOLIDUS_EQUAL,NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_ASTERISK_EQUAL,NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_MODULO_EQUAL,  NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_AT_EQUAL,      NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_ARROW,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_MINUS,         unary,    binary,   PREC_SUM),
	RULE(TOKEN_PLUS,          unary,    binary,   PREC_SUM),
	RULE(TOKEN_TILDE,         unary,    NULL,     PREC_NONE),
	RULE(TOKEN_BANG,          unary,    NULL,     PREC_NONE),
	RULE(TOKEN_SOLIDUS,       NULL,     binary,   PREC_TERM),
	RULE(TOKEN_DOUBLE_SOLIDUS,NULL,     binary,   PREC_TERM),
	RULE(TOKEN_ASTERISK,      NULL,     binary,   PREC_TERM),
	RULE(TOKEN_MODULO,        NULL,     binary,   PREC_TERM),
	RULE(TOKEN_AT,            NULL,     binary,   PREC_TERM),
	RULE(TOKEN_POW,           NULL,     binary,   PREC_EXPONENT),
	RULE(TOKEN_PIPE,          NULL,     binary,   PREC_BITOR),
	RULE(TOKEN_CARET,         NULL,     binary,   PREC_BITXOR),
	RULE(TOKEN_AMPERSAND,     NULL,     binary,   PREC_BITAND),
	RULE(TOKEN_LEFT_SHIFT,    NULL,     binary,   PREC_SHIFT),
	RULE(TOKEN_RIGHT_SHIFT,   NULL,     binary,   PREC_SHIFT),
	RULE(TOKEN_BANG_EQUAL,    NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_EQUAL_EQUAL,   NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_GREATER,       NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_GREATER_EQUAL, NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_LESS,          NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_LESS_EQUAL,    NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_IN,            NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_IS,            NULL,     compare,  PREC_COMPARISON),
	RULE(TOKEN_NOT,           unot_,    compare,  PREC_COMPARISON),
	RULE(TOKEN_IDENTIFIER,    variable, NULL,     PREC_NONE),
	RULE(TOKEN_STRING,        string,   NULL,     PREC_NONE),
	RULE(TOKEN_BIG_STRING,    string,   NULL,     PREC_NONE),
	RULE(TOKEN_PREFIX_B,      string,   NULL,     PREC_NONE),
	RULE(TOKEN_PREFIX_F,      string,   NULL,     PREC_NONE),
	RULE(TOKEN_PREFIX_R,      string,   NULL,     PREC_NONE),
	RULE(TOKEN_NUMBER,        number,   NULL,     PREC_NONE),
	RULE(TOKEN_AND,           NULL,     and_,     PREC_AND),
	RULE(TOKEN_OR,            NULL,     or_,      PREC_OR),
	RULE(TOKEN_FALSE,         literal,  NULL,     PREC_NONE),
	RULE(TOKEN_NONE,          literal,  NULL,     PREC_NONE),
	RULE(TOKEN_TRUE,          literal,  NULL,     PREC_NONE),
	RULE(TOKEN_YIELD,         yield,    NULL,     PREC_NONE),
	RULE(TOKEN_AWAIT,         await,    NULL,     PREC_NONE),
	RULE(TOKEN_LAMBDA,        lambda,   NULL,     PREC_NONE),
	RULE(TOKEN_SUPER,         super_,   NULL,     PREC_NONE),
	RULE(TOKEN_CLASS,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_ELSE,          NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_FOR,           NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_DEF,           NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_DEL,           NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_LET,           NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_RETURN,        NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_WHILE,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_BREAK,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_CONTINUE,      NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_IMPORT,        NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_RAISE,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_ASYNC,         NULL,     NULL,     PREC_NONE),

	RULE(TOKEN_COMMA,         NULL,     comma,   PREC_COMMA),
	RULE(TOKEN_IF,            NULL,     ternary, PREC_TERNARY),

	RULE(TOKEN_INDENTATION,   NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_ERROR,         NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_EOL,           NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_EOF,           NULL,     NULL,     PREC_NONE),
	RULE(TOKEN_RETRY,         NULL,     NULL,     PREC_NONE),
};

static ParseRule * getRule(KrkTokenType type) {
	return &krk_parseRules[type];
}

