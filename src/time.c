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

KRK_Function(sleep) {
	FUNCTION_TAKES_EXACTLY(1);

	if (!IS_INTEGER(argv[0]) && !IS_FLOATING(argv[0])) {
		return TYPE_ERROR(int or float,argv[0]);
	}

	unsigned int usecs = (IS_INTEGER(argv[0]) ? AS_INTEGER(argv[0]) :
	                      (IS_FLOATING(argv[0]) ? AS_FLOATING(argv[0]) : 0)) *
	                      1000000;

	usleep(usecs);

	return BOOLEAN_VAL(1);
}

KRK_Function(time) {
	FUNCTION_TAKES_NONE();

	struct timeval tv;
	gettimeofday(&tv,NULL);

	double out = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;

	return FLOATING_VAL(out);
}

void krk_module_init_time(KrkThreadState * _thread) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "time", (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)S("time"));
	krk_attachNamedValue(&module->fields, "__file__", NONE_VAL());
	KRK_DOC(module, "@brief Provides timekeeping functions.");
	KRK_DOC(BIND_FUNC(module,sleep), "@brief Pause execution of the current thread.\n"
		"@arguments secs\n\n"
		"Uses the system @c usleep() function to sleep for @p secs seconds, which may be a @ref float or @ref int. "
		"The available precision is platform-dependent.");
	KRK_DOC(BIND_FUNC(module,time), "@brief Return the elapsed seconds since the system epoch.\n\n"
		"Returns a @ref float representation of the number of seconds since the platform's epoch date. "
		"On POSIX platforms, this is the number of seconds since 1 January 1970. "
		"The precision of the return value is platform-dependent.");
}

