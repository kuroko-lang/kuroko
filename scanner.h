#pragma once

typedef enum {
	TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
	TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
	TOKEN_LEFT_SQUARE, TOKEN_RIGHT_SQUARE,
	/* 6 */
	TOKEN_COLON,
	TOKEN_COMMA, TOKEN_DOT, TOKEN_MINUS, TOKEN_PLUS,
	TOKEN_SEMICOLON, TOKEN_SOLIDUS, TOKEN_ASTERISK,

	/* 14 */
	TOKEN_BANG, TOKEN_BANG_EQUAL,
	TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
	TOKEN_GREATER, TOKEN_GREATER_EQUAL,
	TOKEN_LESS, TOKEN_LESS_EQUAL,

	/* 22 */
	TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER, TOKEN_BIG_STRING,

	TOKEN_AND,   /* 26 and */
	TOKEN_CLASS, /* 27 class */
	TOKEN_DEF,   /* 28 def */
	TOKEN_ELSE,  /* 29 else */
	TOKEN_FALSE, /* 30 False */
	TOKEN_FOR,   /* 31 for */
	TOKEN_IF,    /* 32 if */
	TOKEN_IMPORT,/* 33 import */
	TOKEN_IN,    /* 34 in */
	TOKEN_LET,   /* 35 let */
	TOKEN_NONE,  /* 36 None */
	TOKEN_NOT,   /* 37 not */
	TOKEN_OR,    /* 38 or */
	TOKEN_PRINT, /* 39 print */
	TOKEN_RETURN,/* 40 return */
	TOKEN_SELF,  /* 41 self */
	TOKEN_SUPER, /* 42 super */
	TOKEN_TRUE,  /* 43 True */
	TOKEN_WHILE, /* 44 while */

	TOKEN_INDENTATION, /* 45 */

	TOKEN_RETRY, /* 46 */
	TOKEN_EOL, /* 47 */

	TOKEN_EXPORT, /* 48 */
	TOKEN_MODULO,
	TOKEN_TRY, TOKEN_EXCEPT, TOKEN_RAISE,
	TOKEN_BREAK, TOKEN_CONTINUE,

	/* Decorators */
	TOKEN_AT,         /* @ */
	/* Bitwise operators */
	TOKEN_CARET,      /* ^ (xor) */
	TOKEN_AMPERSAND,  /* & (and) */
	TOKEN_PIPE,       /* | (or) */
	TOKEN_TILDE,      /* ~ (negate) */
	/* Shifts */
	TOKEN_LEFT_SHIFT, /* << */
	TOKEN_RIGHT_SHIFT,/* >> */
	/* Assignment shortcuts */
	TOKEN_PLUS_EQUAL, /* += */
	TOKEN_MINUS_EQUAL,/* -= */
	TOKEN_PLUS_PLUS,  /* ++ */
	TOKEN_MINUS_MINUS,/* -- */

	TOKEN_AS,
	TOKEN_FROM,
	TOKEN_ELIF, /* Provided for compatibility, recommend `else if` instead. */

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
