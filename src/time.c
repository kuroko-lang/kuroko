#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "vm.h"
#include "value.h"
#include "object.h"
#include "util.h"

KRK_FUNC(sleep,{
	FUNCTION_TAKES_EXACTLY(1);

	if (!IS_INTEGER(argv[0]) && !IS_FLOATING(argv[0])) {
		return TYPE_ERROR(int or float,argv[0]);
	}

	unsigned int usecs = (IS_INTEGER(argv[0]) ? AS_INTEGER(argv[0]) :
	                      (IS_FLOATING(argv[0]) ? AS_FLOATING(argv[0]) : 0)) *
	                      1000000;

	usleep(usecs);

	return BOOLEAN_VAL(1);
})

KRK_FUNC(time,{
	FUNCTION_TAKES_NONE();
	time_t out = time(NULL);
	return FLOATING_VAL(out);
})

_noexport
void _createAndBind_timeMod(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "time", (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)S("time"));
	krk_attachNamedValue(&module->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&module->fields, "__doc__",
		(KrkObj*)S("Provides timekeeping functions."));
	BIND_FUNC(module,sleep);
	BIND_FUNC(module,time);
}

