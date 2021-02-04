/**
 * Utilities for creating native bindings.
 *
 * This is intended for use in C extensions to provide a uniform interface
 * for defining extension methods and ensuring they have consistent argument
 * and keyword argument usage.
 */
#pragma once

#include "object.h"
#include "vm.h"

/* Quick macro for turning string constants into KrkString*s */
#define S(c) (krk_copyString(c,sizeof(c)-1))

#define likely(cond)   __builtin_expect(!!(cond), 1)
#define unlikely(cond) __builtin_expect(!!(cond), 0)
#define _noexport __attribute__((visibility("hidden")))

/**
 * Binding macros.
 *
 * These macros are intended to be used together to define functions for a class.
 */
static inline const char * _method_name(const char * func) {
	const char * out = func;
	if (*out == '_') out++;
	while (*out && *out != '_') out++;
	if (*out == '_') out++;
	return out;
}

#define ADD_BASE_CLASS(obj, name, baseClass) krk_makeClass(vm.builtins, &obj, name, baseClass)

#define BUILTIN_FUNCTION(name, func, docStr) do { \
	krk_defineNative(&vm.builtins->fields, name, func)->doc = docStr; \
} while (0)

#define METHOD_TAKES_NONE() do { if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes no arguments (%d given)", \
	_method_name(__func__), "exactly", (argc-1)); } while (0)

#define METHOD_TAKES_EXACTLY(n) do { if (argc != (n+1)) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name(__func__), "exactly", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define METHOD_TAKES_AT_LEAST(n) do { if (argc < (n+1)) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name(__func__), "at least", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define METHOD_TAKES_AT_MOST(n) do { if (argc > (n+1)) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name(__func__), "at most", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define TYPE_ERROR(expected,value) krk_runtimeError(vm.exceptions.typeError, "%s() expects %s, not '%s'", \
		/* Function name */ _method_name(__func__), /* expected type */ #expected, krk_typeName(value))

#define NOT_ENOUGH_ARGS() krk_runtimeError(vm.exceptions.argumentError, "%s() missing required positional argument", \
		/* Function name */ _method_name(__func__))

#define CHECK_ARG(i, type, ctype, name) \
	if (argc < (i+1)) return NOT_ENOUGH_ARGS(); \
	if (!IS_ ## type (argv[i])) return TYPE_ERROR(type,argv[i]); \
	ctype name __attribute__((unused)) = AS_ ## type (argv[i])

#define FUNC_NAME(klass, name) _ ## klass ## _ ## name

#define KRK_METHOD(klass, name, body) static KrkValue _ ## klass ## _ ## name (int argc, KrkValue argv[], int hasKw) { \
	CHECK_ARG(0,klass,CURRENT_CTYPE,CURRENT_NAME); \
	body; return NONE_VAL(); }

#define KRK_FUNC(name,body) static KrkValue _krk_ ## name (int argc, KrkValue argv[], int hasKw) { \
	body; return NONE_VAL(); }

/* This assumes you have a KrkInstance called `module` in the current scope. */
#define MAKE_CLASS(klass) do { krk_makeClass(module,&klass,#klass,vm.objectClass); klass ->allocSize = sizeof(struct klass); } while (0)
#define BIND_METHOD(klass,method) do { krk_defineNative(&klass->methods, "." #method, _ ## klass ## _ ## method); } while (0)
#define BIND_FIELD(klass,method) do { krk_defineNative(&klass->methods, ":" #method, _ ## klass ## _ ## method); } while (0)
#define BIND_PROP(klass,method) do { krk_defineNativeProperty(&klass->fields, #method, _ ## klass ## _ ## method); } while (0)
