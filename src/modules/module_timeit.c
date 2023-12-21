#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/util.h>

KRK_Function(timeit) {
	KrkValue callable;
	int times = 1000000;

	if (!krk_parseArgs("V|i", (const char *[]){"callable","number"},
		&callable, &times)) {
		return NONE_VAL();
	}

	struct timeval tv_before, tv_after;
	gettimeofday(&tv_before,NULL);
	for (krk_integer_type t = 0; t < times; ++t) {
		krk_push(callable);
		krk_callStack(0);
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return NONE_VAL();
	}
	gettimeofday(&tv_after,NULL);

	double before = (double)tv_before.tv_sec + (double)tv_before.tv_usec / 1000000.0;
	double after = (double)tv_after.tv_sec + (double)tv_after.tv_usec / 1000000.0;

	return FLOATING_VAL(after-before);
}

KRK_Module(timeit) {
	KRK_DOC(module, "@brief Run functions very quickly without loop overhead from the interpreter.");

	BIND_FUNC(module,timeit);
}
