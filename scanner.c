#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kuroko.h"
#include "scanner.h"

typedef struct {
	const char * start;
	const char * cur;
	size_t line;
	int startOfLine;
	int hasUnget;
	KrkToken unget;
} KrkScanner;

KrkScanner scanner;

void krk_initScanner(const char * src) {
	scanner.start = src;
	scanner.cur   = src;
	scanner.line  = 1;
	scanner.startOfLine = 1;
	scanner.hasUnget = 0;
	/* file, etc. ? */
}

static int isAtEnd() {
	return *scanner.cur == '\0';
}

static KrkToken makeToken(KrkTokenType type) {
	return (KrkToken){
		.type = type,
		.start = scanner.start,
		.length = (size_t)(scanner.cur - scanner.start),
		.line = scanner.line
	};
}

static KrkToken errorToken(const char * errorStr) {
	return (KrkToken){
		.type = TOKEN_ERROR,
		.start = errorStr,
		.length = strlen(errorStr),
		.line = scanner.line
	};
}

static char advance() {
	return (*scanner.cur == '\0') ? '\0' : *(scanner.cur++);
}

static int match(char expected) {
	if (isAtEnd()) return 0;
	if (*scanner.cur != expected) return 0;
	scanner.cur++;
	return 1;
}

static char peek() {
	return *scanner.cur;
}

static char peekNext() {
	if (isAtEnd()) return '\0';
	return scanner.cur[1];
}

static void skipWhitespace() {
	for (;;) {
		char c = peek();
		switch (c) {
			case ' ':
			case '\t':
				advance();
				break;
			default:
				return;
		}
	}
}

static KrkToken makeIndentation() {
	while (!isAtEnd() && peek() == ' ') advance();
	if (peek() == '\n') {
		/* Pretend we didn't see this line */
		return makeToken(TOKEN_INDENTATION);
	}
	if (peek() == '#') {
		KrkToken out = makeToken(TOKEN_INDENTATION);
		while (!isAtEnd() && peek() != '\n') advance();
		return out;
	}
	return makeToken(TOKEN_INDENTATION);
}

static KrkToken string() {
	while (peek() != '"' && !isAtEnd()) {
		if (peek() == '\\') advance(); /* Advance twice */
		if (peek() == '\n') scanner.line++; /* Not start of line because string */
		advance();
	}

	if (isAtEnd()) return errorToken("Unterminated string.");

	assert(peek() == '"');
	advance();

	return makeToken(TOKEN_STRING);
}

static KrkToken codepoint() {
	while (peek() != '\'' && !isAtEnd()) {
		if (peek() == '\\') advance();
		if (peek() == '\n') return makeToken(TOKEN_RETRY);
		advance();
	}

	if (isAtEnd()) return errorToken("Unterminated codepoint literal.");

	assert(peek() == '\'');
	advance();

	return makeToken(TOKEN_CODEPOINT);
}

static int isDigit(char c) {
	return c >= '0' && c <= '9';
}

static KrkToken number(char c) {
	if (c == '0') {
		if (peek() == 'x' || peek() == 'X') {
			/* Hexadecimal */
			advance();
			while (isDigit(peek()) || (peek() >= 'a' && peek() <= 'f') ||
			       (peek() >= 'A' && peek() <= 'F')) advance();
			return makeToken(TOKEN_NUMBER);
		} else if (peek() == 'b' || peek() == 'B') {
			/* Binary */
			advance();
			while (peek() == '0' || peek() == '1') advance();
			return makeToken(TOKEN_NUMBER);
		} if (peek() == 'o' || peek() == 'O') {
			/* Octal - must be 0o, none of those silly 0123 things */
			advance();
			while (peek() >= '0' && peek() <= '7') advance();
			return makeToken(TOKEN_NUMBER);
		}
		/* Otherwise, decimal and maybe 0.123 floating */
	}

	/* Decimal */
	while (isDigit(peek())) advance();

	/* Floating point */
	if (peek() == '.' && isDigit(peekNext())) {
		advance();
		while (isDigit(peek())) advance();
	}

	return makeToken(TOKEN_NUMBER);
}

static int isAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}

static int checkKeyword(size_t start, const char * rest, KrkTokenType type) {
	size_t length = strlen(rest);
	if ((size_t)(scanner.cur - scanner.start) == start + length &&
		memcmp(scanner.start + start, rest, length) == 0) return type;
	return TOKEN_IDENTIFIER;
}

static KrkTokenType identifierType() {
#define MORE(i) (scanner.cur - scanner.start > i)
	switch (*scanner.start) {
		case 'a': return checkKeyword(1, "nd", TOKEN_AND);
		case 'c': return checkKeyword(1, "lass", TOKEN_CLASS);
		case 'd': return checkKeyword(1, "ef", TOKEN_DEF);
		case 'e': if (MORE(1)) switch(scanner.start[1]) {
			case 'l': return checkKeyword(2, "se", TOKEN_ELSE);
			case 'x': return checkKeyword(2, "port", TOKEN_EXPORT);
		} break;
		case 'f': return checkKeyword(1, "or", TOKEN_FOR);
		case 'F': return checkKeyword(1, "alse", TOKEN_FALSE);
		case 'i': if (MORE(1)) switch (scanner.start[1]) {
			case 'f': return checkKeyword(2, "", TOKEN_IF);
			case 'n': return checkKeyword(2, "", TOKEN_IN);
			case 'm': return checkKeyword(2, "port", TOKEN_IMPORT);
		} break;
		case 'l': return checkKeyword(1, "et", TOKEN_LET);
		case 'n': return checkKeyword(1, "ot", TOKEN_NOT);
		case 'N': return checkKeyword(1, "one", TOKEN_NONE);
		case 'o': return checkKeyword(1, "r", TOKEN_OR);
		case 'p': return checkKeyword(1, "rint", TOKEN_PRINT);
		case 'r': return checkKeyword(1, "eturn", TOKEN_RETURN);
		case 's': if (MORE(1)) switch(scanner.start[1]) {
			case 'e': return checkKeyword(2, "lf", TOKEN_SELF);
			case 'u': return checkKeyword(2, "per", TOKEN_SUPER);
		} break;
		case 'T': return checkKeyword(1, "rue", TOKEN_TRUE);
		case 'w': return checkKeyword(1, "hile", TOKEN_WHILE);
	}
	return TOKEN_IDENTIFIER;
}

static KrkToken identifier() {
	while (isAlpha(peek()) || isDigit(peek())) advance();

	return makeToken(identifierType());
}

void krk_ungetToken(KrkToken token) {
	if (scanner.hasUnget) {
		fprintf(stderr, "(internal error) Tried to unget multiple times, this is not valid.\n");
		exit(1);
	}
	scanner.hasUnget = 1;
	scanner.unget = token;
}


KrkToken krk_scanToken() {

	if (scanner.hasUnget) {
		scanner.hasUnget = 0;
		return scanner.unget;
	}

	/* If at start of line, do thing */
	if (scanner.startOfLine && peek() == ' ') {
		scanner.start = scanner.cur;
		scanner.startOfLine = 0;
		return makeIndentation();
	}

	/* Eat whitespace */
	skipWhitespace();

	/* Skip comments */
	if (peek() == '#') while (peek() != '\n' && !isAtEnd()) advance();

	scanner.start = scanner.cur;
	if (isAtEnd()) return makeToken(TOKEN_EOF);

	char c = advance();

	if (isAtEnd()) return makeToken(TOKEN_EOF);

	if (c == '\n') {
		scanner.line++;
		if (scanner.startOfLine) {
			/* Ignore completely blank lines */
			return makeToken(TOKEN_RETRY);
		} else {
			scanner.startOfLine = 1;
			return makeToken(TOKEN_EOL);
		}
	}

	/* Not indentation, not a linefeed on an empty line, must be not be start of line any more */
	scanner.startOfLine = 0;

	if (isAlpha(c)) return identifier();
	if (isDigit(c)) return number(c);

	switch (c) {
		case '(': return makeToken(TOKEN_LEFT_PAREN);
		case ')': return makeToken(TOKEN_RIGHT_PAREN);
		case '{': return makeToken(TOKEN_LEFT_BRACE);
		case '}': return makeToken(TOKEN_RIGHT_BRACE);
		case '[': return makeToken(TOKEN_LEFT_SQUARE);
		case ']': return makeToken(TOKEN_RIGHT_SQUARE);
		case ':': return makeToken(TOKEN_COLON);
		case ',': return makeToken(TOKEN_COMMA);
		case '.': return makeToken(TOKEN_DOT);
		case '-': return makeToken(TOKEN_MINUS);
		case '+': return makeToken(TOKEN_PLUS);
		case ';': return makeToken(TOKEN_SEMICOLON);
		case '/': return makeToken(TOKEN_SOLIDUS);
		case '*': return makeToken(TOKEN_ASTERISK);

		case '!': return makeToken(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
		case '=': return makeToken(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
		case '<': return makeToken(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
		case '>': return makeToken(match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);

		case '"': return string();
		case '\'': return codepoint();
	}


	return errorToken("Unexpected character.");
}
