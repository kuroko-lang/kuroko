#include <kuroko/vm.h>
#include <kuroko/util.h>

KRK_Function(collect) {
	FUNCTION_TAKES_NONE();
	if (&krk_currentThread != vm.threads) return krk_runtimeError(vm.exceptions->valueError, "only the main thread can do that");
	return INTEGER_VAL(krk_collectGarbage());
}

KRK_Function(pause) {
	FUNCTION_TAKES_NONE();
	vm.globalFlags |= (KRK_GLOBAL_GC_PAUSED);
	return NONE_VAL();
}

KRK_Function(resume) {
	FUNCTION_TAKES_NONE();
	vm.globalFlags &= ~(KRK_GLOBAL_GC_PAUSED);
	return NONE_VAL();
}

KRK_Module(gc) {
	KRK_DOC(module, "@brief Namespace containing methods for controlling the garbage collector.");

	KRK_DOC(BIND_FUNC(module,collect),
		"@brief Triggers one cycle of garbage collection.");
	KRK_DOC(BIND_FUNC(module,pause),
		"@brief Disables automatic garbage collection until @ref resume is called.");
	KRK_DOC(BIND_FUNC(module,resume),
		"@brief Re-enable automatic garbage collection after it was stopped by @ref pause ");
}
