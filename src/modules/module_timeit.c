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

KRK_FUNC(timeit,{
	KrkValue number;
	int times = 1000000;
	if (hasKw && krk_tableGet_fast(AS_DICT(argv[argc]), S("number"), &number)) {
		if (!IS_INTEGER(number)) return TYPE_ERROR(int,number);
		times = AS_INTEGER(number);
	}

	struct timeval tv_before, tv_after;
	gettimeofday(&tv_before,NULL);
	for (krk_integer_type t = 0; t < times; ++t) {
		krk_push(argv[0]);
		krk_callStack(0);
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return NONE_VAL();
	}
	gettimeofday(&tv_after,NULL);

	double before = (double)tv_before.tv_sec + (double)tv_before.tv_usec / 1000000.0;
	double after = (double)tv_after.tv_sec + (double)tv_after.tv_usec / 1000000.0;

	return FLOATING_VAL(after-before);
})

KrkValue krk_module_onload_timeit(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_push(OBJECT_VAL(module));

	KRK_DOC(module, "@brief Run functions very quickly without loop overhead from the interpreter.");

	BIND_FUNC(module,timeit);

	krk_pop();
	return OBJECT_VAL(module);
}
