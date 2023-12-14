/**
 * @file tools/args.c
 * @brief Embedding demo showing how to use @c krk_parseArgs
 *
 * This is a small demo of embedding Kuroko in a simple application
 * and providing native function bindings that use @c krk_parseArgs
 * to evaluate arguments.
 */
#include "../src/vendor/rline.c"
#include <kuroko/vm.h>
#include <kuroko/util.h>

#define SIMPLE_REPL_ENABLE_RLINE
#include "simple-repl.h"

KRK_Function(demofunc1) {
	int a;
	const char * b = NULL;

	if (!krk_parseArgs(
		"iz",
		(const char *[]){"a","b"},
		&a,
		&b
	)) {
		return NONE_VAL();
	}

	fprintf(stderr, "a=%d\n", a);
	fprintf(stderr, "b=%s\n", b);

	return NONE_VAL();
}

KRK_Function(demofunc2) {
	int a, b, c;
	int d = 1;
	int e = 2;
	int has_f = 0;
	int f = 3;

	if (!krk_parseArgs(
		"iii|iii?", (const char *[]){"a","b","c","d","e","f"},
		&a, &b, &c,
		&d, &e, &has_f, &f)) {
		return NONE_VAL();
	}

	fprintf(stderr, "a=%d\n", a);
	fprintf(stderr, "b=%d\n", b);
	fprintf(stderr, "c=%d\n", c);
	fprintf(stderr, "d=%d\n", d);
	fprintf(stderr, "e=%d\n", e);
	fprintf(stderr, "has_f=%d\n", has_f);
	fprintf(stderr, "f=%d\n", f);

	return NONE_VAL();
}

KRK_Function(demofunc3) {
	KrkValue list, dict, set;

	if (!krk_parseArgs("V!V!V!",
		(const char*[]){"a","b","c"},
		KRK_BASE_CLASS(list), &list,
		KRK_BASE_CLASS(dict), &dict,
		KRK_BASE_CLASS(set), &set
	)) {
		return NONE_VAL();
	}

	return krk_runtimeError(vm.exceptions->valueError,
		"Correctly passed values: %R %R %R",
			list, dict, set);
}

KRK_Function(demofunc4) {
	int argcount;
	const KrkValue * args;
	int a, b = 0, c = 1, d = 2;

	if (!krk_parseArgs("i|i*$ii",
		(const char *[]){"a","b","c","d"},
		&a, &b,
		&argcount, &args,
		&c, &d
		)) {
		return NONE_VAL();
	}

	fprintf(stderr, "a=%d b=%d c=%d d=%d\n", a, b, c, d);
	fprintf(stderr, "%d extra args\n", argcount);


	return NONE_VAL();
}

KRK_Function(takeschars) {
	int a = 0, b = 0, c = 0;

	if (!krk_parseArgs(
		"C|CC", (const char *[]){"a","b","c"},
		&a, &b, &c)) {
		return NONE_VAL();
	}

	fprintf(stderr, "a=%d b=%d c=%d\n", a, b, c);

	return NONE_VAL();
}

KRK_Function(parseints) {
	unsigned char a;
	unsigned int b;
	ssize_t c;
	if (!krk_parseArgs("bIn",(const char*[]){"a","b","c"}, &a, &b, &c)) return NONE_VAL();

	fprintf(stderr, "%d %u %zd\n", a, b, c);
	return NONE_VAL();
}

KRK_Function(parsefloats) {
	float f;
	double d;

	if (!krk_parseArgs("fd",(const char*[]){"f","d"}, &f, &d)) return NONE_VAL();

	fprintf(stderr, "%f %f\n", f, d);
	return NONE_VAL();
}

int main(int argc, char * argv[]) {
	krk_initVM(0);
	krk_startModule("__main__");
	BIND_FUNC(krk_currentThread.module, demofunc1);
	BIND_FUNC(krk_currentThread.module, demofunc2);
	BIND_FUNC(krk_currentThread.module, demofunc3);
	BIND_FUNC(krk_currentThread.module, demofunc4);
	BIND_FUNC(krk_currentThread.module, takeschars);
	BIND_FUNC(krk_currentThread.module, parseints);
	BIND_FUNC(krk_currentThread.module, parsefloats);
	runSimpleRepl();
	krk_freeVM();
	return 0;
}
