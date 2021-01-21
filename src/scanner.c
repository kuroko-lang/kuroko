#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kuroko.h"
#include "scanner.h"

static KrkScanner scanner;

void krk_initScanner(const char * src) {
	scanner.start = src;
	scanner.cur   = src;
	scanner.line  = 1;
	scanner.linePtr   = src;
	scanner.startOfLine = 1;
	scanner.hasUnget = 0;
	/* file, etc. ? */
}

static int isAtEnd() {
	return *scanner.cur == '\0';
}

static void nextLine() {
	scanner.line++;
	scanner.linePtr = scanner.cur;
}

static KrkToken makeToken(KrkTokenType type) {
	return (KrkToken){
		.type = type,
		.start = scanner.start,
		.length = (type == TOKEN_EOL) ? 0 : (size_t)(scanner.cur - scanner.start),
		.line = scanner.line,
		.linePtr = scanner.linePtr,
		.literalWidth = (type == TOKEN_EOL) ? 0 : (size_t)(scanner.cur - scanner.start),
		.col = (scanner.start - scanner.linePtr) + 1,
	};
}

static KrkToken errorToken(const char * errorStr) {
	return (KrkToken){
		.type = TOKEN_ERROR,
		.start = errorStr,
		.length = strlen(errorStr),
		.line = scanner.line,
		.linePtr = scanner.linePtr,
		.literalWidth = (size_t)(scanner.cur - scanner.start),
		.col = (scanner.start - scanner.linePtr) + 1,
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

static char peekNext(int n) {
	if (isAtEnd()) return '\0';
	for (int i = 0; i < n; ++i) if (scanner.cur[i] == '\0') return '\0';
	return scanner.cur[n];
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
	char reject = (peek() == ' ') ? '\t' : ' ';
	while (!isAtEnd() && (peek() == ' ' || peek() == '\t')) advance();
	if (isAtEnd()) return makeToken(TOKEN_EOF);
	for (const char * start = scanner.start; start < scanner.cur; start++) {
		if (*start == reject) return errorToken("Invalid mix of indentation.");
	}
	KrkToken out = makeToken(TOKEN_INDENTATION);
	if (reject == ' ') out.length *= 8;
	if (peek() == '#') {
		/* Skip the entirety of the comment but not the line feed */
		while (!isAtEnd() && peek() != '\n') advance();
	}
	return out;
}

static KrkToken string(char quoteMark) {
	if (peek() == quoteMark && peekNext(1) == quoteMark) {
		advance(); advance();
		/* Big string */
		while (!isAtEnd()) {
			if (peek() == quoteMark && peekNext(1) == quoteMark && peekNext(2) == quoteMark) {
				advance();
				advance();
				advance();
				return makeToken(TOKEN_BIG_STRING);
			}

			if (peek() == '\\') advance();
			if (peek() == '\n') {
				advance();
				nextLine();
			}
			else advance();
		}
		if (isAtEnd()) return errorToken("Unterminated string?");
	}
	while (peek() != quoteMark && !isAtEnd()) {
		if (peek() == '\n') return errorToken("Unterminated string.");
		if (peek() == '\\') advance();
		if (peek() == '\n') {
			advance();
			nextLine();
		}
		else advance();
	}

	if (isAtEnd()) return errorToken("Unterminated string.");

	assert(peek() == quoteMark);
	advance();

	return makeToken(TOKEN_STRING);
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
	if (peek() == '.' && isDigit(peekNext(1))) {
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
		case 'a': if (MORE(1)) switch(scanner.start[1]) {
			case 'n': return checkKeyword(2, "d", TOKEN_AND);
			case 's': return checkKeyword(2, "", TOKEN_AS);
		} break;
		case 'b': if (MORE(1)) return checkKeyword(1, "reak", TOKEN_BREAK);
			else if (scanner.start[1] == '\'' || scanner.start[1] == '"') return TOKEN_PREFIX_B;
			break;
		case 'c': if (MORE(1)) switch(scanner.start[1]) {
			case 'l': return checkKeyword(2, "ass", TOKEN_CLASS);
			case 'o': return checkKeyword(2, "ntinue", TOKEN_CONTINUE);
		} break;
		case 'd': if (MORE(1)) switch(scanner.start[1]) {
			case 'e': if (MORE(2)) switch (scanner.start[2]) {
				case 'f': return checkKeyword(3, "", TOKEN_DEF);
				case 'l': return checkKeyword(3, "", TOKEN_DEL);
			} break;
		} break;
		case 'e': if (MORE(1)) switch(scanner.start[1]) {
			case 'l': if (MORE(2)) switch(scanner.start[2]) {
				case 's': return checkKeyword(3,"e", TOKEN_ELSE);
				case 'i': return checkKeyword(3,"f", TOKEN_ELIF);
			} break;
			case 'x': return checkKeyword(2, "cept", TOKEN_EXCEPT);
		} break;
		case 'f': if (MORE(1)) switch(scanner.start[1]) {
			case 'o': return checkKeyword(2, "r", TOKEN_FOR);
			case 'r': return checkKeyword(2, "om", TOKEN_FROM);
		} break;
		case 'F': return checkKeyword(1, "alse", TOKEN_FALSE);
		case 'i': if (MORE(1)) switch (scanner.start[1]) {
			case 'f': return checkKeyword(2, "", TOKEN_IF);
			case 'n': return checkKeyword(2, "", TOKEN_IN);
			case 'm': return checkKeyword(2, "port", TOKEN_IMPORT);
			case 's': return checkKeyword(2, "", TOKEN_IS);
		} break;
		case 'l': if (MORE(1)) switch (scanner.start[1]) {
			case 'a': return checkKeyword(2, "mbda", TOKEN_LAMBDA);
			case 'e': return checkKeyword(2, "t", TOKEN_LET);
		} break;
		case 'n': return checkKeyword(1, "ot", TOKEN_NOT);
		case 'N': return checkKeyword(1, "one", TOKEN_NONE);
		case 'o': return checkKeyword(1, "r", TOKEN_OR);
		case 'p': return checkKeyword(1, "ass", TOKEN_PASS);
		case 'r': if (MORE(1)) switch (scanner.start[1]) {
			case 'e': return checkKeyword(2, "turn", TOKEN_RETURN);
			case 'a': return checkKeyword(2, "ise", TOKEN_RAISE);
		} break;
		case 's': if (MORE(1)) switch(scanner.start[1]) {
			case 'e': return checkKeyword(2, "lf", TOKEN_SELF);
			case 'u': return checkKeyword(2, "per", TOKEN_SUPER);
		} break;
		case 't': return checkKeyword(1, "ry", TOKEN_TRY);
		case 'T': return checkKeyword(1, "rue", TOKEN_TRUE);
		case 'w': if (MORE(1)) switch(scanner.start[1]) {
			case 'h': return checkKeyword(2, "ile", TOKEN_WHILE);
			case 'i': return checkKeyword(2, "th", TOKEN_WITH);
		} break;
	}
	return TOKEN_IDENTIFIER;
}

static KrkToken identifier() {
	while (isAlpha(peek()) || isDigit(peek()) || (unsigned char)peek() > 0x7F) advance();

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

void krk_rewindScanner(KrkScanner to) {
	scanner = to;
}

KrkScanner krk_tellScanner(void) {
	return scanner;
}

KrkToken krk_scanToken() {

	if (scanner.hasUnget) {
		scanner.hasUnget = 0;
		return scanner.unget;
	}

	/* If at start of line, do thing */
	if (scanner.startOfLine && (peek() == ' ' || peek() == '\t')) {
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

	if (c == '\n') {
		KrkToken out;
		if (scanner.startOfLine) {
			/* Ignore completely blank lines */
			out = makeToken(TOKEN_RETRY);
		} else {
			scanner.startOfLine = 1;
			out = makeToken(TOKEN_EOL);
		}
		nextLine();
		return out;
	}

	if (c == '\\' && peek() == '\n') {
		advance();
		nextLine();
		return makeToken(TOKEN_RETRY);
	}

	/* Not indentation, not a linefeed on an empty line, must be not be start of line any more */
	scanner.startOfLine = 0;

	if (isAlpha(c) || (unsigned char)c > 0x7F) return identifier();
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
		case ';': return makeToken(TOKEN_SEMICOLON);
		case '@': return makeToken(TOKEN_AT);
		case '~': return makeToken(TOKEN_TILDE);

		case '!': return makeToken(match('=') ? TOKEN_BANG_EQUAL    : TOKEN_BANG);
		case '=': return makeToken(match('=') ? TOKEN_EQUAL_EQUAL   : TOKEN_EQUAL);
		case '<': return makeToken(match('=') ? TOKEN_LESS_EQUAL    : (match('<') ? (match('=') ? TOKEN_LSHIFT_EQUAL : TOKEN_LEFT_SHIFT) :  TOKEN_LESS));
		case '>': return makeToken(match('=') ? TOKEN_GREATER_EQUAL : (match('>') ? (match('=') ? TOKEN_RSHIFT_EQUAL : TOKEN_RIGHT_SHIFT) : TOKEN_GREATER));
		case '-': return makeToken(match('=') ? TOKEN_MINUS_EQUAL   : (match('-') ? TOKEN_MINUS_MINUS : TOKEN_MINUS));
		case '+': return makeToken(match('=') ? TOKEN_PLUS_EQUAL    : (match('+') ? TOKEN_PLUS_PLUS   : TOKEN_PLUS));
		case '^': return makeToken(match('=') ? TOKEN_CARET_EQUAL   : TOKEN_CARET);
		case '|': return makeToken(match('=') ? TOKEN_PIPE_EQUAL    : TOKEN_PIPE);
		case '&': return makeToken(match('=') ? TOKEN_AMP_EQUAL     : TOKEN_AMPERSAND);
		case '/': return makeToken(match('=') ? TOKEN_SOLIDUS_EQUAL : TOKEN_SOLIDUS);
		case '*': return makeToken(match('=') ? TOKEN_ASTERISK_EQUAL: (match('*') ? (match('=') ? TOKEN_POW_EQUAL : TOKEN_POW) : TOKEN_ASTERISK));
		case '%': return makeToken(match('=') ? TOKEN_MODULO_EQUAL  : TOKEN_MODULO);

		case '"': return string('"');
		case '\'': return string('\'');
	}

	return errorToken("Unexpected character.");
}
