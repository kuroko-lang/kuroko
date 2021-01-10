#pragma once

typedef enum {
	TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
	TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
	TOKEN_LEFT_SQUARE, TOKEN_RIGHT_SQUARE,
	TOKEN_COLON,
	TOKEN_COMMA,
	TOKEN_DOT,
	TOKEN_MINUS,
	TOKEN_PLUS,
	TOKEN_SEMICOLON,
	TOKEN_SOLIDUS,
	TOKEN_ASTERISK,
	TOKEN_MODULO,
	TOKEN_AT,
	TOKEN_CARET,      /* ^ (xor) */
	TOKEN_AMPERSAND,  /* & (and) */
	TOKEN_PIPE,       /* | (or) */
	TOKEN_TILDE,      /* ~ (negate) */
	TOKEN_LEFT_SHIFT, /* << */
	TOKEN_RIGHT_SHIFT,/* >> */
	TOKEN_PLUS_EQUAL, /* += */
	TOKEN_MINUS_EQUAL,/* -= */
	TOKEN_PLUS_PLUS,  /* ++ */
	TOKEN_MINUS_MINUS,/* -- */

	TOKEN_BANG, TOKEN_BANG_EQUAL,
	TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
	TOKEN_GREATER, TOKEN_GREATER_EQUAL,
	TOKEN_LESS, TOKEN_LESS_EQUAL,

	TOKEN_STRING,
	TOKEN_BIG_STRING,
	TOKEN_NUMBER,

	/*
	 * Everything after this, up to indentation,
	 * consists of alphanumerics.
	 */
	TOKEN_IDENTIFIER,
	TOKEN_AND,
	TOKEN_CLASS,
	TOKEN_DEF,
	TOKEN_ELSE,
	TOKEN_FALSE,
	TOKEN_FOR,
	TOKEN_IF,
	TOKEN_IMPORT,
	TOKEN_IN,
	TOKEN_LET,
	TOKEN_NONE,
	TOKEN_NOT,
	TOKEN_OR,
	TOKEN_ELIF,
	TOKEN_RETURN,
	TOKEN_SELF,
	TOKEN_SUPER,
	TOKEN_TRUE,
	TOKEN_WHILE,
	TOKEN_TRY,
	TOKEN_EXCEPT,
	TOKEN_RAISE,
	TOKEN_BREAK,
	TOKEN_CONTINUE,
	TOKEN_AS,
	TOKEN_FROM,
	TOKEN_LAMBDA,
	TOKEN_WITH,

	TOKEN_INDENTATION,

	TOKEN_EOL,
	TOKEN_RETRY,
	TOKEN_ERROR,
	TOKEN_EOF,
} KrkTokenType;

typedef struct {
	KrkTokenType type;
	const char * start;
	size_t length;
	size_t line;
	const char * linePtr;
	size_t col;
	size_t literalWidth;
} KrkToken;

typedef struct {
	const char * start;
	const char * cur;
	const char * linePtr;
	size_t line;
	int startOfLine;
	int hasUnget;
	KrkToken unget;
} KrkScanner;

extern void krk_initScanner(const char * src);
extern KrkToken krk_scanToken(void);
extern void krk_ungetToken(KrkToken token);
extern void krk_rewindScanner(KrkScanner to);
extern KrkScanner krk_tellScanner(void);
