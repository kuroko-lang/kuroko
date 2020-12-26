#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kuroko.h"
#include "compiler.h"
#include "scanner.h"
#include "object.h"

typedef struct {
	KrkToken current;
	KrkToken previous;
	int hadError;
	int panicMode;
} Parser;

typedef enum {
	PREC_NONE,
	PREC_ASSIGNMENT, /* = */
	PREC_OR,         /* or */
	PREC_AND,        /* and */
	PREC_EQUALITY,   /* == != in */
	PREC_COMPARISON, /* < > <= >= */
	PREC_TERM,       /* + - */
	PREC_FACTOR,     /* * */
	PREC_UNARY,      /* ! - not */
	PREC_CALL,       /* . () */
	PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(int);

typedef struct {
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
} ParseRule;

typedef struct {
	KrkToken name;
	ssize_t depth;
} Local;

typedef struct {
	Local  locals[256];
	size_t localCount;
	size_t scopeDepth;
} Compiler;

Parser parser;
Compiler * current = NULL;

KrkChunk * compilingChunk;
static KrkChunk * currentChunk() {
	return compilingChunk;
}

static void initCompiler(Compiler * compiler) {
	compiler->localCount = 0;
	compiler->scopeDepth = 0;
	current = compiler;
}

static void parsePrecedence(Precedence precedence);
static ssize_t parseVariable(const char * errorMessage);
static void defineVariable(size_t global);
static ssize_t identifierConstant(KrkToken * name);
static ssize_t resolveLocal(Compiler * compiler, KrkToken * name);
static ParseRule * getRule(KrkTokenType type);
static void expression();
static void statement();
static void declaration();

static void errorAt(KrkToken * token, const char * message) {
	if (parser.panicMode) return;
	parser.panicMode = 1;

	fprintf(stderr, "[line %d] Error", (int)token->line);
	if (token->type == TOKEN_EOF) {
		fprintf(stderr, " at end");
	} else if (token->type != TOKEN_ERROR) {
		fprintf(stderr, " at '%.*s'", (int)token->length, token->start);
	}

	fprintf(stderr, ": %s\n", message);
	parser.hadError = 1;
}

static void error(const char * message) {
	errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char * message) {
	errorAt(&parser.current, message);
}

static void advance() {
	parser.previous = parser.current;

	for (;;) {
		parser.current = krk_scanToken();
		if (parser.current.type == TOKEN_RETRY) continue;
		if (parser.current.type != TOKEN_ERROR) break;

		errorAtCurrent(parser.current.start);
	}
}

static void consume(KrkTokenType type, const char * message) {
	if (parser.current.type == type) {
		advance();
		return;
	}

	errorAtCurrent(message);
}

static int check(KrkTokenType type) {
	return parser.current.type == type;
}

static int match(KrkTokenType type) {
	if (!check(type)) return 0;
	advance();
	return 1;
}

static void emitByte(uint8_t byte) {
	krk_writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
	emitByte(byte1);
	emitByte(byte2);
}

static void emitReturn() {
	emitByte(OP_RETURN);
}

static void endCompiler() {
	emitReturn();
}

static void endOfLine() {
	if (!(match(TOKEN_EOL) || match(TOKEN_EOF))) {
		errorAtCurrent("Expected end of line.");
	}
}

static void emitConstant(KrkValue value) {
	krk_writeConstant(currentChunk(), value, parser.previous.line);
}

static void number(int canAssign) {
	const char * start= parser.previous.start;
	int base = 10;
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		base = 10;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
		base = 2;
		start += 2;
	} else if (start[0] == '0') {
		base = 8;
		start += 1;
	}
	if (base == 10) {
		/* See if it's a float */
		for (size_t j = 0; j < parser.previous.length; ++j) {
			if (parser.previous.start[j] == '.') {
				double value = strtod(start, NULL);
				emitConstant(FLOATING_VAL(value));
				return;
			}
		}
	}
	int value = strtol(start, NULL, base);
	emitConstant(INTEGER_VAL(value));
}

static void binary(int canAssign) {
	KrkTokenType operatorType = parser.previous.type;
	ParseRule * rule = getRule(operatorType);
	parsePrecedence((Precedence)(rule->precedence + 1));

	switch (operatorType) {
		case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
		case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
		case TOKEN_GREATER:       emitByte(OP_GREATER); break;
		case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
		case TOKEN_LESS:          emitByte(OP_LESS); break;
		case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;

		case TOKEN_PLUS:     emitByte(OP_ADD); break;
		case TOKEN_MINUS:    emitByte(OP_SUBTRACT); break;
		case TOKEN_ASTERISK: emitByte(OP_MULTIPLY); break;
		case TOKEN_SOLIDUS:  emitByte(OP_DIVIDE); break;
		default: return;
	}
}

static void literal(int canAssign) {
	switch (parser.previous.type) {
		case TOKEN_FALSE: emitByte(OP_FALSE); break;
		case TOKEN_NONE:  emitByte(OP_NONE); break;
		case TOKEN_TRUE:  emitByte(OP_TRUE); break;
		default: return;
	}
}

static void expression() {
	parsePrecedence(PREC_ASSIGNMENT);
}

static void varDeclaration() {
	ssize_t global = parseVariable("Expected variable name.");

	if (match(TOKEN_EQUAL)) {
		expression();
	} else {
		emitByte(OP_NONE);
	}

	endOfLine();
	defineVariable(global);
}

static void printStatement() {
	expression();
	endOfLine();
	emitByte(OP_PRINT);
}

static void synchronize() {
	parser.panicMode = 0;
	while (parser.current.type != TOKEN_EOF) {
		if (parser.previous.type == TOKEN_EOL) return;

		switch (parser.current.type) {
			case TOKEN_CLASS:
			case TOKEN_DEF:
			case TOKEN_LET:
			case TOKEN_FOR:
			case TOKEN_IF:
			case TOKEN_WHILE:
			case TOKEN_PRINT:
			case TOKEN_RETURN:
				return;
			default: break;
		}

		advance();
	}
}

static void declaration() {
	if (match(TOKEN_LET)) {
		varDeclaration();
	} else {
		statement();
	}

	if (parser.panicMode) synchronize();
}

static void expressionStatement() {
	expression();
	endOfLine();
	emitByte(OP_POP);
}

static void block() {
	consume(TOKEN_COLON, "Blocks must start with a colon; where's your colon?");

	if (match(TOKEN_EOL)) {
		/* Begin actual blocks */
		if (check(TOKEN_INDENTATION)) {
			/* TODO: Check if this is correctly indented more than the current block */
			size_t currentIndentation = parser.current.length;
			do {
				if (parser.current.length != currentIndentation) break;
				advance();
				declaration();
			} while (check(TOKEN_INDENTATION));
		} else {
			errorAtCurrent("Expected indentation for block");
		}
	} else {
		errorAtCurrent("Unsupported single-line block");
	}
}

static void beginScope() {
	current->scopeDepth++;
}

static void endScope() {
	current->scopeDepth--;
	while (current->localCount > 0 &&
	       current->locals[current->localCount - 1].depth > current->scopeDepth) {
		emitByte(OP_POP);
		current->localCount--;
	}
}

static void statement() {
	if (match(TOKEN_EOL)) return; /* Meaningless blank line */

	if (match(TOKEN_PRINT)) {
		printStatement();
	} else if (match(TOKEN_BLOCK)) {
		beginScope();
		block();
		endScope();
	} else {
		expressionStatement();
	}
}

static void grouping(int canAssign) {
	expression();
	consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void unary(int canAssign) {
	KrkTokenType operatorType = parser.previous.type;

	parsePrecedence(PREC_UNARY);

	switch (operatorType) {
		case TOKEN_MINUS: emitByte(OP_NEGATE); break;

		/* These are equivalent */
		case TOKEN_BANG:
		case TOKEN_NOT:
			emitByte(OP_NOT);
			break;

		default: return;
	}
}

static void string(int canAssign) {
	emitConstant(OBJECT_VAL(copyString(parser.previous.start + 1, parser.previous.length - 2)));
}

#define EMIT_CONSTANT_OP(opc, arg) do { if (arg < 256) { emitBytes(opc, arg); } \
	else { emitBytes(opc ## _LONG, arg >> 16); emitBytes(arg >> 8, arg); } } while (0)

static void namedVariable(KrkToken name, int canAssign) {
	ssize_t arg = resolveLocal(current, &name);
	if (arg != -1) {
		if (canAssign && match(TOKEN_EQUAL)) {
			expression();
			EMIT_CONSTANT_OP(OP_SET_LOCAL, arg);
		} else {
			EMIT_CONSTANT_OP(OP_GET_LOCAL, arg);
		}
	} else {
		arg = identifierConstant(&name);
		if (canAssign && match(TOKEN_EQUAL)) {
			expression();
			EMIT_CONSTANT_OP(OP_SET_GLOBAL, arg);
		} else {
			EMIT_CONSTANT_OP(OP_GET_GLOBAL, arg);
		}
	}
}

static void variable(int canAssign) {
	namedVariable(parser.previous, canAssign);
}

ParseRule rules[] = {
	[TOKEN_LEFT_PAREN]    = {grouping, NULL,   PREC_NONE},
	[TOKEN_RIGHT_PAREN]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LEFT_BRACE]    = {NULL,     NULL,   PREC_NONE},
	[TOKEN_RIGHT_BRACE]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LEFT_SQUARE]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_RIGHT_SQUARE]  = {NULL,     NULL,   PREC_NONE},
	[TOKEN_COLON]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_COMMA]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_DOT]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_MINUS]         = {unary,    binary, PREC_TERM},
	[TOKEN_PLUS]          = {NULL,     binary, PREC_TERM},
	[TOKEN_SEMICOLON]     = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SOLIDUS]       = {NULL,     binary, PREC_FACTOR},
	[TOKEN_ASTERISK]      = {NULL,     binary, PREC_FACTOR},
	[TOKEN_BANG]          = {unary,    NULL,   PREC_NONE},
	[TOKEN_BANG_EQUAL]    = {NULL,     binary, PREC_EQUALITY},
	[TOKEN_EQUAL]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_EQUAL_EQUAL]   = {NULL,     binary, PREC_EQUALITY},
	[TOKEN_GREATER]       = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_GREATER_EQUAL] = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_LESS]          = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_LESS_EQUAL]    = {NULL,     binary, PREC_COMPARISON},
	[TOKEN_IDENTIFIER]    = {variable, NULL,   PREC_NONE},
	[TOKEN_STRING]        = {string,   NULL,   PREC_NONE},
	[TOKEN_NUMBER]        = {number,   NULL,   PREC_NONE},
	[TOKEN_CODEPOINT]     = {NULL,     NULL,   PREC_NONE}, /* should be equivalent to number */
	[TOKEN_AND]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_CLASS]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_ELSE]          = {NULL,     NULL,   PREC_NONE},
	[TOKEN_FALSE]         = {literal,  NULL,   PREC_NONE},
	[TOKEN_FOR]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_DEF]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_IF]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_IN]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_LET]           = {NULL,     NULL,   PREC_NONE},
	[TOKEN_NONE]          = {literal,  NULL,   PREC_NONE},
	[TOKEN_NOT]           = {unary,    NULL,   PREC_NONE},
	[TOKEN_OR]            = {NULL,     NULL,   PREC_NONE},
	[TOKEN_PRINT]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_RETURN]        = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SELF]          = {NULL,     NULL,   PREC_NONE},
	[TOKEN_SUPER]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_TRUE]          = {literal,  NULL,   PREC_NONE},
	[TOKEN_WHILE]         = {NULL,     NULL,   PREC_NONE},

	/* This is going to get interesting */
	[TOKEN_INDENTATION]   = {NULL,     NULL,   PREC_NONE},
	[TOKEN_ERROR]         = {NULL,     NULL,   PREC_NONE},
	[TOKEN_EOF]           = {NULL,     NULL,   PREC_NONE},
};

static void parsePrecedence(Precedence precedence) {
	advance();
	ParseFn prefixRule = getRule(parser.previous.type)->prefix;
	if (prefixRule == NULL) {
		error("expect expression");
		return;
	}
	int canAssign = precedence <= PREC_ASSIGNMENT;
	prefixRule(canAssign);
	while (precedence <= getRule(parser.current.type)->precedence) {
		advance();
		ParseFn infixRule = getRule(parser.previous.type)->infix;
		infixRule(canAssign);
	}

	if (canAssign && match(TOKEN_EQUAL)) {
		error("invalid assignment target");
	}
}

static ssize_t identifierConstant(KrkToken * name) {
	return krk_addConstant(currentChunk(), OBJECT_VAL(copyString(name->start, name->length)));
}

static int identifiersEqual(KrkToken * a, KrkToken * b) {
	return (a->length == b->length && memcmp(a->start, b->start, a->length) == 0);
}

static ssize_t resolveLocal(Compiler * compiler, KrkToken * name) {
	for (ssize_t i = compiler->localCount - 1; i >= 0; i--) {
		Local * local = &compiler->locals[i];
		if (identifiersEqual(name, &local->name)) {
			if (local->depth == -1) {
				error("can not initialize value recursively (are you shadowing something?)");
			}
			return i;
		}
	}
	return -1;
}

static void addLocal(KrkToken name) {
	if (current->localCount == 256) {
		error("too many locals");
		return;
	}
	Local * local = &current->locals[current->localCount++];
	local->name = name;
	local->depth = -1;
}

static void declareVariable() {
	if (current->scopeDepth == 0) return;
	KrkToken * name = &parser.previous;
	/* Detect duplicate definition */
	for (ssize_t i = current->localCount - 1; i >= 0; i--) {
		Local * local = &current->locals[i];
		if (local->depth != -1 && local->depth < current->scopeDepth) break;
		if (identifiersEqual(name, &local->name)) error("Duplicate definition");
	}
	addLocal(*name);
}

static ssize_t parseVariable(const char * errorMessage) {
	consume(TOKEN_IDENTIFIER, errorMessage);

	declareVariable();
	if (current->scopeDepth > 0) return 0;

	return identifierConstant(&parser.previous);
}

static void markInitialized() {
	current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static void defineVariable(size_t global) {
	if (current->scopeDepth > 0) {
		markInitialized();
		return;
	}

	EMIT_CONSTANT_OP(OP_DEFINE_GLOBAL, global);
}

static ParseRule * getRule(KrkTokenType type) {
	return &rules[type];
}

int krk_compile(const char * src, KrkChunk * chunk) {
	krk_initScanner(src);
	Compiler compiler;
	initCompiler(&compiler);
	compilingChunk = chunk;

	parser.hadError = 0;
	parser.panicMode = 0;

	advance();

	while (!match(TOKEN_EOF)) {
		declaration();
	}

	endCompiler();
	return !parser.hadError;
}
