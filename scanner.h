#pragma once

typedef enum {
	TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
	TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
	TOKEN_LEFT_SQUARE, TOKEN_RIGHT_SQUARE,
	TOKEN_COLON,
	TOKEN_COMMA, TOKEN_DOT, TOKEN_MINUS, TOKEN_PLUS,
	TOKEN_SEMICOLON, TOKEN_SOLIDUS, TOKEN_ASTERISK,

	TOKEN_BANG, TOKEN_BANG_EQUAL,
	TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
	TOKEN_GREATER, TOKEN_GREATER_EQUAL,
	TOKEN_LESS, TOKEN_LESS_EQUAL,

	TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER, TOKEN_CODEPOINT,

	TOKEN_AND,   /* and */
	TOKEN_CLASS, /* class */
	TOKEN_DEF,   /* def */
	TOKEN_ELSE,  /* else */
	TOKEN_FALSE, /* False */
	TOKEN_FOR,   /* for */
	TOKEN_IF,    /* if */
	TOKEN_IN,    /* in */
	TOKEN_LET,   /* let */
	TOKEN_NONE,  /* None */
	TOKEN_NOT,   /* not */
	TOKEN_OR,    /* or */
	TOKEN_PRINT, /* print */
	TOKEN_RETURN,/* return */
	TOKEN_SELF,  /* self */
	TOKEN_SUPER, /* super */
	TOKEN_TRUE,  /* True */
	TOKEN_WHILE, /* while */

	TOKEN_INDENTATION,

	TOKEN_RETRY,

	TOKEN_ERROR,
	TOKEN_EOF,
} KrkTokenType;

typedef struct {
	KrkTokenType type;
	const char * start;
	size_t length;
	size_t line;
} KrkToken;

void krk_initScanner(const char * src);
KrkToken krk_scanToken(void);
