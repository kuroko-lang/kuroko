#include <kuroko/vm.h>
#include <kuroko/util.h>

#include <wchar.h>
#include <locale.h>

#ifndef _WIN32
extern int wcwidth(wchar_t c);
#else
static
#include "../wcwidth._h"
#endif

KRK_Function(wcwidth) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,codepoint);
	return INTEGER_VAL(wcwidth(codepoint));
}

KRK_Module(wcwidth) {
	KRK_DOC(module, "Character widths.");
	BIND_FUNC(module, wcwidth);

#ifndef _WIN32
	setlocale(LC_ALL, "");
#endif
}

