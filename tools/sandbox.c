#include <stdio.h>
#include <string.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

#include "simple-repl.h"

int main(int argc, char * argv[]) {
	/* Disable automatic traceback printing, default modules */
	KrkThreadState * _thread = krk_initVM(KRK_GLOBAL_CLEAN_OUTPUT|KRK_GLOBAL_NO_DEFAULT_MODULES);

	/* Set up our module context. */
	krk_startModule("__main__");

	int retval = 0;
	if (argc > 1) {
		KrkValue result = krk_interpret(_thread, argv[1], "<stdin>");
		if (!IS_NONE(result)) {
			if (IS_INTEGER(result)) {
				retval = AS_INTEGER(result);
			}
			KrkClass * type = krk_getType(result);
			if (type->_reprer) {
				krk_push(result);
				result = krk_callDirect(type->_reprer, 1);
			}
			if (IS_STRING(result)) {
				fprintf(stdout, " => %s\n", AS_CSTRING(result));
			}
		} else if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
			krk_dumpTraceback();
			retval = 1;
		}
	} else {
		runSimpleRepl(_thread);
	}

	krk_freeVM(_thread);
	return retval;
}

