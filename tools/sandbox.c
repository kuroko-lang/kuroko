#include <stdio.h>
#include <string.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

#include "simple-repl.h"

int main(int argc, char * argv[]) {
	krk_initVM(0);

	krk_tableDelete(&vm.system->fields, OBJECT_VAL(S("module_paths")));
	krk_tableDelete(&vm.modules, OBJECT_VAL(S("kuroko")));
	krk_tableDelete(&vm.builtins->fields, OBJECT_VAL(S("print")));

	/* Don't automatically dump tracebacks. */
	krk_resetStack();
	krk_push(NONE_VAL());
	krk_currentThread.frames[0].outSlots = 1;

	krk_startModule("<module>");
	krk_attachNamedValue(&krk_currentThread.module->fields,"__doc__", NONE_VAL());

	int retval = 0;

	if (argc > 1) {
		KrkValue result = krk_interpret(argv[1], 0, "<stdin>","<stdin>");
		if (!IS_NONE(result)) {
			if (IS_INTEGER(result)) {
				retval = AS_INTEGER(result);
			}
			KrkClass * type = krk_getType(result);
			if (type->_reprer) {
				krk_push(result);
				result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			}
			if (IS_STRING(result)) {
				fprintf(stdout, "%s\n", AS_CSTRING(result));
			}
		} else if (krk_currentThread.flags & KRK_HAS_EXCEPTION) {
			krk_dumpTraceback();
			retval = 1;
		}
	} else {
		runSimpleRepl();
	}

	krk_freeVM();
	return retval;
}

