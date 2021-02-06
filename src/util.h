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
#ifndef _WIN32
#define _noexport __attribute__((visibility("hidden")))
#else
#define _noexport
#endif

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

/* _method_name works for this, but let's skip the inlined function call where possible */
#define _function_name(f) (f+5)

#define METHOD_TAKES_NONE() do { if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes no arguments (%d given)", \
	_method_name(__func__), (argc-1)); } while (0)

#define METHOD_TAKES_EXACTLY(n) do { if (argc != (n+1)) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name(__func__), "exactly", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define METHOD_TAKES_AT_LEAST(n) do { if (argc < (n+1)) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name(__func__), "at least", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define METHOD_TAKES_AT_MOST(n) do { if (argc > (n+1)) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name(__func__), "at most", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define FUNCTION_TAKES_NONE() do { if (argc != 0) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes no arguments (%d given)", \
	_function_name(__func__), (argc)); } while (0)

#define FUNCTION_TAKES_EXACTLY(n) do { if (argc != n) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_function_name(__func__), "exactly", n, (n != 1) ? "s" : "", (argc)); } while (0)

#define FUNCTION_TAKES_AT_LEAST(n) do { if (argc < n) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_function_name(__func__), "at least", n, (n != 1) ? "s" : "", (argc)); } while (0)

#define FUNCTION_TAKES_AT_MOST(n) do { if (argc > n) return krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)", \
	_function_name(__func__), "at most", n, (n != 1) ? "s" : "", (argc)); } while (0)

#define TYPE_ERROR(expected,value) krk_runtimeError(vm.exceptions.typeError, "%s() expects %s, not '%s'", \
		/* Function name */ _method_name(__func__), /* expected type */ #expected, krk_typeName(value))

#define NOT_ENOUGH_ARGS() krk_runtimeError(vm.exceptions.argumentError, "%s() missing required positional argument", \
		/* Function name */ _method_name(__func__))

#define CHECK_ARG(i, type, ctype, name) \
	if (argc < (i+1)) return NOT_ENOUGH_ARGS(); \
	if (!IS_ ## type (argv[i])) return TYPE_ERROR(type,argv[i]); \
	ctype name __attribute__((unused)) = AS_ ## type (argv[i])

#define FUNC_NAME(klass, name) _ ## klass ## _ ## name
#define FUNC_SIG(klass, name) static KrkValue FUNC_NAME(klass,name) (int argc, KrkValue argv[], int hasKw)
#define KRK_METHOD(klass, name, body) FUNC_SIG(klass, name) { \
	CHECK_ARG(0,klass,CURRENT_CTYPE,CURRENT_NAME); \
	body; return NONE_VAL(); }

#define KRK_FUNC(name,body) static KrkValue _krk_ ## name (int argc, KrkValue argv[], int hasKw) { \
	body; return NONE_VAL(); }

/* This assumes you have a KrkInstance called `module` in the current scope. */
#define MAKE_CLASS(klass) do { krk_makeClass(module,&klass,#klass,vm.objectClass); klass ->allocSize = sizeof(struct klass); } while (0)
#define BIND_METHOD(klass,method) do { krk_defineNative(&klass->methods, "." #method, _ ## klass ## _ ## method); } while (0)
#define BIND_FIELD(klass,method) do { krk_defineNative(&klass->methods, ":" #method, _ ## klass ## _ ## method); } while (0)
#define BIND_PROP(klass,method) do { krk_defineNativeProperty(&klass->fields, #method, _ ## klass ## _ ## method); } while (0)
#define BIND_FUNC(module,func) do { krk_defineNative(&module->fields, #func, _krk_ ## func); } while (0)

struct StringBuilder {
	size_t capacity;
	size_t length;
	char * bytes;
};

static inline void pushStringBuilder(struct StringBuilder * sb, char c) {
	if (sb->capacity < sb->length + 1) {
		size_t old = sb->capacity;
		sb->capacity = GROW_CAPACITY(old);
		sb->bytes = GROW_ARRAY(char, sb->bytes, old, sb->capacity);
	}
	sb->bytes[sb->length++] = c;
}

static inline void pushStringBuilderStr(struct StringBuilder * sb, char *str, size_t len) {
	while (sb->capacity < sb->length + len) {
		size_t old = sb->capacity;
		sb->capacity = GROW_CAPACITY(old);
		sb->bytes = realloc(sb->bytes, sb->capacity);
	}
	for (size_t i = 0; i < len; ++i) {
		sb->bytes[sb->length++] = *(str++);
	}
}

static inline KrkValue finishStringBuilder(struct StringBuilder * sb) {
	KrkValue out = OBJECT_VAL(krk_copyString(sb->bytes, sb->length));
	FREE_ARRAY(char,sb->bytes, sb->capacity);
	return out;
}

static inline KrkValue discardStringBuilder(struct StringBuilder * sb) {
	FREE_ARRAY(char,sb->bytes, sb->capacity);
	return NONE_VAL();
}

#define IS_int(o)     (IS_INTEGER(o))
#define AS_int(o)     (AS_INTEGER(o))

#define IS_bool(o)    (IS_BOOLEAN(o))
#define AS_bool(o)    (AS_BOOLEAN(o))

#define IS_float(o)   (IS_FLOATING(o))
#define AS_float(o)   (AS_FLOATING(o))

#define IS_list(o)    krk_isInstanceOf(o,vm.baseClasses.listClass)
#define AS_list(o)    (KrkList*)AS_OBJECT(o)

#define IS_listiterator(o) krk_isInstanceOf(o,vm.baseClasses.listiteratorClass)
#define AS_listiterator(o) AS_INSTANCE(o)

#define IS_str(o)     (IS_STRING(o)||krk_isInstanceOf(o,vm.baseClasses.strClass))
#define AS_str(o)     (KrkString*)AS_OBJECT(o)

#define IS_striterator(o) (krk_isInstanceOf(o,vm.baseClasses.striteratorClass))
#define AS_striterator(o) (AS_INSTANCE(o))

#define IS_dict(o)    krk_isInstanceOf(o,vm.baseClasses.dictClass)
#define AS_dict(o)    (KrkDict*)AS_OBJECT(o)

#define IS_dictitems(o) krk_isInstanceOf(o,vm.baseClasses.dictitemsClass)
#define AS_dictitems(o) ((struct DictItems*)AS_OBJECT(o))

#define IS_dictkeys(o) krk_isInstanceOf(o,vm.baseClasses.dictkeysClass)
#define AS_dictkeys(o) ((struct DictKeys*)AS_OBJECT(o))

