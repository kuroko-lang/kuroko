#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#include <kuroko/kuroko.h>
#include <kuroko/scanner.h>

KrkScanner krk_initScanner(const char * src) {
	KrkScanner scanner;
	scanner.start = src;
	scanner.cur   = src;
	scanner.line  = 1;
	scanner.linePtr   = src;
	scanner.startOfLine = 1;
	scanner.hasUnget = 0;
	return scanner;
}

static int isAtEnd(const KrkScanner * scanner) {
	return *scanner->cur == '\0';
}

static void nextLine(KrkScanner * scanner) {
	scanner->line++;
	scanner->linePtr = scanner->cur;
}

static KrkToken makeToken(const KrkScanner * scanner, KrkTokenType type) {
	return (KrkToken){
		.type = type,
		.start = scanner->start,
		.length = (type == TOKEN_EOL) ? 0 : (size_t)(scanner->cur - scanner->start),
		.line = scanner->line,
		.linePtr = scanner->linePtr,
		.literalWidth = (type == TOKEN_EOL) ? 0 : (size_t)(scanner->cur - scanner->start),
		.col = (scanner->start - scanner->linePtr) + 1,
	};
}

static KrkToken errorToken(const KrkScanner * scanner, const char * errorStr) {
	ssize_t column = (scanner->linePtr < scanner->start) ? scanner->start - scanner->linePtr : 0;
	ssize_t width  = (scanner->start   < scanner->cur)   ? scanner->cur - scanner->start : 0;
	return (KrkToken){
		.type = TOKEN_ERROR,
		.start = errorStr,
		.length = strlen(errorStr),
		.line = scanner->line,
		.linePtr = scanner->linePtr,
		.literalWidth = (size_t)(width),
		.col = column + 1,
	};
}

static char advance(KrkScanner * scanner) {
	return (*scanner->cur == '\0') ? '\0' : *(scanner->cur++);
}

static int match(KrkScanner * scanner, char expected) {
	if (isAtEnd(scanner)) return 0;
	if (*scanner->cur != expected) return 0;
	scanner->cur++;
	return 1;
}

static char peek(const KrkScanner * scanner) {
	return *scanner->cur;
}

static char peekNext(const KrkScanner * scanner, int n) {
	if (isAtEnd(scanner)) return '\0';
	for (int i = 0; i < n; ++i) if (scanner->cur[i] == '\0') return '\0';
	return scanner->cur[n];
}

static void skipWhitespace(KrkScanner * scanner) {
	for (;;) {
		char c = peek(scanner);
		switch (c) {
			case ' ':
			case '\t':
				advance(scanner);
				break;
			default:
				return;
		}
	}
}

static KrkToken makeIndentation(KrkScanner * scanner) {
	char reject = (peek(scanner) == ' ') ? '\t' : ' ';
	while (!isAtEnd(scanner) && (peek(scanner) == ' ' || peek(scanner) == '\t')) advance(scanner);
	if (isAtEnd(scanner)) return makeToken(scanner, TOKEN_EOF);
	for (const char * start = scanner->start; start < scanner->cur; start++) {
		if (*start == reject) return errorToken(scanner, "Invalid mix of indentation.");
	}
	KrkToken out = makeToken(scanner, TOKEN_INDENTATION);
	if (reject == ' ') out.length *= 8;
	if (peek(scanner) == '#' || peek(scanner) == '\n') {
		while (!isAtEnd(scanner) && peek(scanner) != '\n') advance(scanner);
		scanner->startOfLine = 1;
		return makeToken(scanner, TOKEN_RETRY);
	}
	return out;
}

static KrkToken string(KrkScanner * scanner, char quoteMark) {
	if (peek(scanner) == quoteMark && peekNext(scanner, 1) == quoteMark) {
		advance(scanner); advance(scanner);
		/* Big string */
		while (!isAtEnd(scanner)) {
			if (peek(scanner) == quoteMark && peekNext(scanner, 1) == quoteMark && peekNext(scanner, 2) == quoteMark) {
				advance(scanner);
				advance(scanner);
				advance(scanner);
				return makeToken(scanner, TOKEN_BIG_STRING);
			}

			if (peek(scanner) == '\\') advance(scanner);
			if (peek(scanner) == '\n') {
				advance(scanner);
				nextLine(scanner);
			}
			else advance(scanner);
		}
		if (isAtEnd(scanner)) return errorToken(scanner, "Unterminated string.");
	}
	while (peek(scanner) != quoteMark && !isAtEnd(scanner)) {
		if (peek(scanner) == '\n') return errorToken(scanner, "Unterminated string.");
		if (peek(scanner) == '\\') advance(scanner);
		if (peek(scanner) == '\n') {
			advance(scanner);
			nextLine(scanner);
		}
		else advance(scanner);
	}

	if (isAtEnd(scanner)) return errorToken(scanner, "Unterminated string.");

	assert(peek(scanner) == quoteMark);
	advance(scanner);

	return makeToken(scanner, TOKEN_STRING);
}

static int isDigit(char c) {
	return c >= '0' && c <= '9';
}

static KrkToken number(KrkScanner * scanner, char c) {
	if (c == '0') {
		if (peek(scanner) == 'x' || peek(scanner) == 'X') {
			/* Hexadecimal */
			advance(scanner);
			while (isDigit(peek(scanner)) || (peek(scanner) >= 'a' && peek(scanner) <= 'f') ||
			       (peek(scanner) >= 'A' && peek(scanner) <= 'F') || (peek(scanner) == '_')) advance(scanner);
			return makeToken(scanner, TOKEN_NUMBER);
		} else if (peek(scanner) == 'b' || peek(scanner) == 'B') {
			/* Binary */
			advance(scanner);
			while (peek(scanner) == '0' || peek(scanner) == '1' || (peek(scanner) == '_')) advance(scanner);
			return makeToken(scanner, TOKEN_NUMBER);
		} if (peek(scanner) == 'o' || peek(scanner) == 'O') {
			/* Octal - must be 0o, none of those silly 0123 things */
			advance(scanner);
			while ((peek(scanner) >= '0' && peek(scanner) <= '7') || (peek(scanner) == '_')) advance(scanner);
			return makeToken(scanner, TOKEN_NUMBER);
		}
		/* Otherwise, decimal and maybe 0.123 floating */
	}

	/* Decimal */
	while (isDigit(peek(scanner)) || peek(scanner) == '_') advance(scanner);

	/* Floating point */
	if (peek(scanner) == '.' && isDigit(peekNext(scanner, 1))) {
		advance(scanner);
		while (isDigit(peek(scanner))) advance(scanner);
	}

	if (peek(scanner) == 'e' || peek(scanner) == 'E') {
		advance(scanner);
		if (peek(scanner) == '+' || peek(scanner) == '-') advance(scanner);
		while (isDigit(peek(scanner))) advance(scanner);
	}

	return makeToken(scanner, TOKEN_NUMBER);
}

static int isAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}

static int _checkKeyword(KrkScanner * scanner, size_t start, const char * rest, KrkTokenType type) {
	size_t length = strlen(rest);
	if ((size_t)(scanner->cur - scanner->start) == start + length &&
		memcmp(scanner->start + start, rest, length) == 0) return type;
	return TOKEN_IDENTIFIER;
}

#define checkKeyword(a,b,c) _checkKeyword(scanner,a,b,c)

static KrkTokenType identifierType(KrkScanner * scanner) {
#define MORE(i) (scanner->cur - scanner->start > i)
	switch (*scanner->start) {
		case 'a': if (MORE(1)) switch(scanner->start[1]) {
			case 'n': return checkKeyword(2, "d", TOKEN_AND);
			case 'w': return checkKeyword(2, "ait", TOKEN_AWAIT);
			case 's': if (MORE(2)) {
					switch (scanner->start[2]) {
						case 's': return checkKeyword(3, "ert", TOKEN_ASSERT);
						case 'y': return checkKeyword(3, "nc", TOKEN_ASYNC);
					}
					break;
				} else {
					return checkKeyword(2, "", TOKEN_AS);
				}
		} break;
		case 'b': if (MORE(1)) return checkKeyword(1, "reak", TOKEN_BREAK);
			else if (scanner->start[1] == '\'' || scanner->start[1] == '"') return TOKEN_PREFIX_B;
			break;
		case 'c': if (MORE(1)) switch(scanner->start[1]) {
			case 'l': return checkKeyword(2, "ass", TOKEN_CLASS);
			case 'o': return checkKeyword(2, "ntinue", TOKEN_CONTINUE);
		} break;
		case 'd': if (MORE(1)) switch(scanner->start[1]) {
			case 'e': if (MORE(2)) switch (scanner->start[2]) {
				case 'f': return checkKeyword(3, "", TOKEN_DEF);
				case 'l': return checkKeyword(3, "", TOKEN_DEL);
			} break;
		} break;
		case 'e': if (MORE(1)) switch(scanner->start[1]) {
			case 'l': if (MORE(2)) switch(scanner->start[2]) {
				case 's': return checkKeyword(3,"e", TOKEN_ELSE);
				case 'i': return checkKeyword(3,"f", TOKEN_ELIF);
			} break;
			case 'x': return checkKeyword(2, "cept", TOKEN_EXCEPT);
		} break;
		case 'f': if (MORE(1)) switch(scanner->start[1]) {
			case 'i': return checkKeyword(2, "nally", TOKEN_FINALLY);
			case 'o': return checkKeyword(2, "r", TOKEN_FOR);
			case 'r': return checkKeyword(2, "om", TOKEN_FROM);
		} else if (scanner->start[1] == '\'' || scanner->start[1] == '"') return TOKEN_PREFIX_F;
		break;
		case 'F': return checkKeyword(1, "alse", TOKEN_FALSE);
		case 'i': if (MORE(1)) switch (scanner->start[1]) {
			case 'f': return checkKeyword(2, "", TOKEN_IF);
			case 'n': return checkKeyword(2, "", TOKEN_IN);
			case 'm': return checkKeyword(2, "port", TOKEN_IMPORT);
			case 's': return checkKeyword(2, "", TOKEN_IS);
		} break;
		case 'l': if (MORE(1)) switch (scanner->start[1]) {
			case 'a': return checkKeyword(2, "mbda", TOKEN_LAMBDA);
			case 'e': return checkKeyword(2, "t", TOKEN_LET);
		} break;
		case 'n': return checkKeyword(1, "ot", TOKEN_NOT);
		case 'N': return checkKeyword(1, "one", TOKEN_NONE);
		case 'o': return checkKeyword(1, "r", TOKEN_OR);
		case 'p': return checkKeyword(1, "ass", TOKEN_PASS);
		case 'r': if (MORE(1)) switch (scanner->start[1]) {
			case 'e': return checkKeyword(2, "turn", TOKEN_RETURN);
			case 'a': return checkKeyword(2, "ise", TOKEN_RAISE);
		} else if (scanner->start[1] == '\'' || scanner->start[1] == '"') return TOKEN_PREFIX_R;
		break;
		case 's': return checkKeyword(1, "uper", TOKEN_SUPER);
		case 't': return checkKeyword(1, "ry", TOKEN_TRY);
		case 'T': return checkKeyword(1, "rue", TOKEN_TRUE);
		case 'w': if (MORE(1)) switch(scanner->start[1]) {
			case 'h': return checkKeyword(2, "ile", TOKEN_WHILE);
			case 'i': return checkKeyword(2, "th", TOKEN_WITH);
		} break;
		case 'y': return checkKeyword(1, "ield", TOKEN_YIELD);
	}
	return TOKEN_IDENTIFIER;
}

static KrkToken identifier(KrkScanner * scanner) {
	while (isAlpha(peek(scanner)) || isDigit(peek(scanner)) || (unsigned char)peek(scanner) > 0x7F) advance(scanner);

	return makeToken(scanner, identifierType(scanner));
}

void krk_ungetToken(KrkScanner * scanner, KrkToken token) {
	if (scanner->hasUnget) {
		abort();
	}
	scanner->hasUnget = 1;
	scanner->unget = token;
}

KrkScanner krk_tellScanner(KrkScanner * scanner) {
	return *scanner;
}

void krk_rewindScanner(KrkScanner * scanner, KrkScanner other) {
	*scanner = other;
}

KrkToken krk_scanToken(KrkScanner * scanner) {

	if (scanner->hasUnget) {
		scanner->hasUnget = 0;
		return scanner->unget;
	}

	/* If at start of line, do thing */
	if (scanner->startOfLine && (peek(scanner) == ' ' || peek(scanner) == '\t')) {
		scanner->start = scanner->cur;
		scanner->startOfLine = 0;
		return makeIndentation(scanner);
	}

	/* Eat whitespace */
	skipWhitespace(scanner);

	/* Skip comments */
	if (peek(scanner) == '#') while (peek(scanner) != '\n' && !isAtEnd(scanner)) advance(scanner);

	scanner->start = scanner->cur;
	if (isAtEnd(scanner)) return makeToken(scanner, TOKEN_EOF);

	char c = advance(scanner);

	if (c == '\n') {
		KrkToken out;
		if (scanner->startOfLine) {
			/* Ignore completely blank lines */
			out = makeToken(scanner, TOKEN_RETRY);
		} else {
			scanner->startOfLine = 1;
			out = makeToken(scanner, TOKEN_EOL);
		}
		nextLine(scanner);
		return out;
	}

	if (c == '\\' && peek(scanner) == '\n') {
		advance(scanner);
		nextLine(scanner);
		return makeToken(scanner, TOKEN_RETRY);
	}

	/* Not indentation, not a linefeed on an empty line, must be not be start of line any more */
	scanner->startOfLine = 0;

	if (isAlpha(c) || (unsigned char)c > 0x7F) return identifier(scanner);
	if (isDigit(c)) return number(scanner, c);

	switch (c) {
		case '(': return makeToken(scanner, TOKEN_LEFT_PAREN);
		case ')': return makeToken(scanner, TOKEN_RIGHT_PAREN);
		case '{': return makeToken(scanner, TOKEN_LEFT_BRACE);
		case '}': return makeToken(scanner, TOKEN_RIGHT_BRACE);
		case '[': return makeToken(scanner, TOKEN_LEFT_SQUARE);
		case ']': return makeToken(scanner, TOKEN_RIGHT_SQUARE);
		case ',': return makeToken(scanner, TOKEN_COMMA);
		case ';': return makeToken(scanner, TOKEN_SEMICOLON);
		case '~': return makeToken(scanner, TOKEN_TILDE);
		case '.': return makeToken(scanner, peek(scanner) == '.' ? (peekNext(scanner,1) == '.' ? (advance(scanner), advance(scanner), TOKEN_ELLIPSIS) : TOKEN_DOT) : TOKEN_DOT);

		case ':': return makeToken(scanner, match(scanner, '=') ? TOKEN_WALRUS        : TOKEN_COLON);
		case '!': return makeToken(scanner, match(scanner, '=') ? TOKEN_BANG_EQUAL    : TOKEN_BANG);
		case '=': return makeToken(scanner, match(scanner, '=') ? TOKEN_EQUAL_EQUAL   : TOKEN_EQUAL);
		case '<': return makeToken(scanner, match(scanner, '=') ? TOKEN_LESS_EQUAL    : (match(scanner, '<') ? (match(scanner, '=') ? TOKEN_LSHIFT_EQUAL : TOKEN_LEFT_SHIFT) :  TOKEN_LESS));
		case '>': return makeToken(scanner, match(scanner, '=') ? TOKEN_GREATER_EQUAL : (match(scanner, '>') ? (match(scanner, '=') ? TOKEN_RSHIFT_EQUAL : TOKEN_RIGHT_SHIFT) : TOKEN_GREATER));
		case '-': return makeToken(scanner, match(scanner, '=') ? TOKEN_MINUS_EQUAL   : (match(scanner, '-') ? TOKEN_MINUS_MINUS : (match(scanner, '>') ? TOKEN_ARROW : TOKEN_MINUS)));
		case '+': return makeToken(scanner, match(scanner, '=') ? TOKEN_PLUS_EQUAL    : (match(scanner, '+') ? TOKEN_PLUS_PLUS   : TOKEN_PLUS));
		case '^': return makeToken(scanner, match(scanner, '=') ? TOKEN_CARET_EQUAL   : TOKEN_CARET);
		case '|': return makeToken(scanner, match(scanner, '=') ? TOKEN_PIPE_EQUAL    : TOKEN_PIPE);
		case '&': return makeToken(scanner, match(scanner, '=') ? TOKEN_AMP_EQUAL     : TOKEN_AMPERSAND);
		case '/': return makeToken(scanner, match(scanner, '=') ? TOKEN_SOLIDUS_EQUAL : (match(scanner, '/') ? (match(scanner, '=') ? TOKEN_DSOLIDUS_EQUAL : TOKEN_DOUBLE_SOLIDUS) : TOKEN_SOLIDUS));
		case '*': return makeToken(scanner, match(scanner, '=') ? TOKEN_ASTERISK_EQUAL: (match(scanner, '*') ? (match(scanner, '=') ? TOKEN_POW_EQUAL : TOKEN_POW) : TOKEN_ASTERISK));
		case '%': return makeToken(scanner, match(scanner, '=') ? TOKEN_MODULO_EQUAL  : TOKEN_MODULO);
		case '@': return makeToken(scanner, match(scanner, '=') ? TOKEN_AT_EQUAL      : TOKEN_AT);

		case '"': return string(scanner, '"');
		case '\'': return string(scanner, '\'');
	}

	return errorToken(scanner, "Unexpected character.");
}

