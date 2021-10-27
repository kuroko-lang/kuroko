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

KRK_FUNC(wcwidth,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,codepoint);
	return INTEGER_VAL(wcwidth(codepoint));
})

KrkValue krk_module_onload_wcwidth(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_push(OBJECT_VAL(module));
	KRK_DOC(module, "Character widths.");
	BIND_FUNC(module, wcwidth);

#ifndef _WIN32
	setlocale(LC_ALL, "");
#endif

	return krk_pop();
}

