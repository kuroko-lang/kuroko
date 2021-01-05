/**
 * Currently just dis().
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "../vm.h"
#include "../value.h"
#include "../object.h"
#include "../debug.h"

#define S(c) (krk_copyString(c,sizeof(c)-1))

/**
 * dis.dis(object)
 */
static KrkValue krk_dis(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "dis() takes ");
		return BOOLEAN_VAL(0);
	}

#ifdef ENABLE_DISASSEMBLY
	if (IS_CLOSURE(argv[0])) {
		KrkFunction * func = AS_CLOSURE(argv[0])->function;
		krk_disassembleChunk(stdout, func, func->name ? func->name->chars : "(unnamed)");
	} else if (IS_BOUND_METHOD(argv[0])) {
		if (AS_BOUND_METHOD(argv[0])->method->type == OBJ_CLOSURE) {
			KrkFunction * func = ((KrkClosure*)AS_BOUND_METHOD(argv[0])->method)->function;
			const char * methodName = func->name ? func->name->chars : "(unnamed)";
			const char * typeName = IS_CLASS(AS_BOUND_METHOD(argv[0])->receiver) ? AS_CLASS(AS_BOUND_METHOD(argv[0])->receiver)->name->chars : krk_typeName(AS_BOUND_METHOD(argv[0])->receiver);
			char * tmp = malloc(strlen(methodName) + strlen(typeName) + 2);
			sprintf(tmp, "%s.%s", typeName, methodName);
			krk_disassembleChunk(stdout, func, tmp);
			free(tmp);
		} else {
			krk_runtimeError(vm.exceptions.typeError, "Can not disassemble built-in method of '%s'", krk_typeName(AS_BOUND_METHOD(argv[0])->receiver));
		}
	} else if (IS_CLASS(argv[0])) {
		krk_runtimeError(vm.exceptions.typeError, "todo: class disassembly");
	} else {
		krk_runtimeError(vm.exceptions.typeError, "Don't know how to disassemble '%s'", krk_typeName(argv[0]));
	}
#else
	krk_runtimeError(vm.exceptions.typeError, "Kuroko was built with debug methods stripped; disassembly is not available.");
#endif

	return NONE_VAL();
}

KrkValue krk_module_onload_dis(void) {
	KrkInstance * module = krk_newInstance(vm.objectClass);
	krk_push(OBJECT_VAL(module));
	krk_defineNative(&module->fields, "dis", krk_dis);
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}

