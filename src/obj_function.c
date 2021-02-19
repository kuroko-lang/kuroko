#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"
#include "debug.h"

/* function.__doc__ */
static KrkValue _closure_get_doc(int argc, KrkValue argv[], int hasKw) {
	if (IS_NATIVE(argv[0]) && AS_NATIVE(argv[0])->doc) {
		return OBJECT_VAL(krk_copyString(AS_NATIVE(argv[0])->doc, strlen(AS_NATIVE(argv[0])->doc)));
	} else if (IS_CLOSURE(argv[0]) && AS_CLOSURE(argv[0])->function->docstring) {
		return  OBJECT_VAL(AS_CLOSURE(argv[0])->function->docstring);
	} else {
		return NONE_VAL();
	}
}

/* method.__doc__ */
static KrkValue _bound_get_doc(int argc, KrkValue argv[], int hasKw) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_doc(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)}, 0);
}

/* Check for and return the name of a native function as a string object */
static KrkValue nativeFunctionName(KrkValue func) {
	const char * string = ((KrkNative*)AS_OBJECT(func))->name;
	if (!string) return OBJECT_VAL(S("<unnamed>"));
	size_t len = strlen(string);
	return OBJECT_VAL(krk_copyString(string,len));
}

/* function.__name__ */
static KrkValue _closure_get_name(int argc, KrkValue argv[], int hasKw) {
	if (IS_NATIVE(argv[0])) return nativeFunctionName(argv[0]);
	else if (!IS_CLOSURE(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "'%s' is neither a closure nor a native", krk_typeName(argv[0]));
	return AS_CLOSURE(argv[0])->function->name ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->name) : OBJECT_VAL(S(""));
}

/* method.__name__ */
static KrkValue _bound_get_name(int argc, KrkValue argv[], int hasKw) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_name(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)}, hasKw);
}

static KrkValue _closure_ip_to_line(int argc, KrkValue argv[], int hasKw) {
	if (argc < 2) return krk_runtimeError(vm.exceptions->argumentError, "%s() expects exactly 2 arguments", "ip_to_line");
	if (!IS_CLOSURE(argv[0])) return NONE_VAL();
	if (!IS_INTEGER(argv[1])) return TYPE_ERROR(int,argv[1]);
	return INTEGER_VAL(krk_lineNumber(&AS_CLOSURE(argv[0])->function->chunk, AS_INTEGER(argv[1])));
}

static KrkValue _bound_ip_to_line(int argc, KrkValue argv[], int hasKw) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_ip_to_line(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)}, hasKw);
}

/* function.__str__ / function.__repr__ */
static KrkValue _closure_str(int argc, KrkValue argv[], int hasKw) {
	KrkValue s = _closure_get_name(argc, argv, hasKw);
	if (!IS_STRING(s)) return NONE_VAL();
	krk_push(s);

	size_t len = AS_STRING(s)->length + sizeof("<function >");
	char * tmp = malloc(len);
	snprintf(tmp, len, "<function %s>", AS_CSTRING(s));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);
	krk_pop();
	return s;
}

/* method.__str__ / method.__repr__ */
static KrkValue _bound_str(int argc, KrkValue argv[], int hasKw) {
	KrkValue s = _bound_get_name(argc, argv, hasKw);
	krk_push(s);

	const char * typeName = krk_typeName(AS_BOUND_METHOD(argv[0])->receiver);

	size_t len = AS_STRING(s)->length + sizeof("<method >") + strlen(typeName) + 1;
	char * tmp = malloc(len);
	snprintf(tmp, len, "<method %s.%s>", typeName, AS_CSTRING(s));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);
	krk_pop();
	return s;
}

/* function.__file__ */
static KrkValue _closure_get_file(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLOSURE(argv[0])) return OBJECT_VAL(S("<builtin>"));
	return AS_CLOSURE(argv[0])->function->chunk.filename ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->chunk.filename) : OBJECT_VAL(S(""));
}

/* method.__file__ */
static KrkValue _bound_get_file(int argc, KrkValue argv[], int hasKw) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_file(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)}, 0);
}

/* function.__args__ */
static KrkValue _closure_get_argnames(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLOSURE(argv[0])) return OBJECT_VAL(krk_newTuple(0));
	KrkFunction * self = AS_CLOSURE(argv[0])->function;
	KrkTuple * tuple = krk_newTuple(self->requiredArgs + self->keywordArgs);
	krk_push(OBJECT_VAL(tuple));
	for (short i = 0; i < self->requiredArgs; ++i) {
		tuple->values.values[tuple->values.count++] = self->requiredArgNames.values[i];
	}
	for (short i = 0; i < self->keywordArgs; ++i) {
		tuple->values.values[tuple->values.count++] = self->keywordArgNames.values[i];
	}
	krk_tupleUpdateHash(tuple);
	krk_pop();
	return OBJECT_VAL(tuple);
}

static KrkValue _bound_get_argnames(int argc, KrkValue argv[], int hasKw) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_argnames(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)}, 0);
}


_noexport
void _createAndBind_functionClass(void) {
	ADD_BASE_CLASS(vm.baseClasses->functionClass, "function", vm.baseClasses->objectClass);
	krk_defineNative(&vm.baseClasses->functionClass->methods, ".__str__", _closure_str);
	krk_defineNative(&vm.baseClasses->functionClass->methods, ".__repr__", _closure_str);
	krk_defineNative(&vm.baseClasses->functionClass->methods, ":__doc__", _closure_get_doc);
	krk_defineNative(&vm.baseClasses->functionClass->methods, ":__name__", _closure_get_name);
	krk_defineNative(&vm.baseClasses->functionClass->methods, ":__file__", _closure_get_file);
	krk_defineNative(&vm.baseClasses->functionClass->methods, ":__args__", _closure_get_argnames);
	krk_defineNative(&vm.baseClasses->functionClass->methods, "_ip_to_line", _closure_ip_to_line);
	krk_finalizeClass(vm.baseClasses->functionClass);

	ADD_BASE_CLASS(vm.baseClasses->methodClass, "method", vm.baseClasses->objectClass);
	krk_defineNative(&vm.baseClasses->methodClass->methods, ".__str__", _bound_str);
	krk_defineNative(&vm.baseClasses->methodClass->methods, ".__repr__", _bound_str);
	krk_defineNative(&vm.baseClasses->methodClass->methods, ".__doc__", _bound_get_doc);
	krk_defineNative(&vm.baseClasses->methodClass->methods, ":__name__", _bound_get_name);
	krk_defineNative(&vm.baseClasses->methodClass->methods, ":__file__", _bound_get_file);
	krk_defineNative(&vm.baseClasses->methodClass->methods, ":__args__", _bound_get_argnames);
	krk_defineNative(&vm.baseClasses->methodClass->methods, "_ip_to_line", _bound_ip_to_line);
	krk_finalizeClass(vm.baseClasses->methodClass);
}
