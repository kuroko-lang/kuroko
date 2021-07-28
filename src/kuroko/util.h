#pragma once
/**
 * @file util.h
 * @brief Utilities for creating native bindings.
 *
 * This is intended for use in C extensions to provide a uniform interface
 * for defining extension methods and ensuring they have consistent argument
 * and keyword argument usage.
 */
#include "object.h"
#include "vm.h"
#include "memory.h"

/* Quick macro for turning string constants into KrkString*s */
#define S(c) (krk_copyString(c,sizeof(c)-1))

#define likely(cond)   __builtin_expect((cond), 1)
#define unlikely(cond) __builtin_expect((cond), 0)

#if !defined(_WIN32) && !defined(EFI_PLATFORM)
#define _noexport __attribute__((visibility("hidden")))
#else
#define _noexport
#endif

#define ADD_BASE_CLASS(obj, name, baseClass) krk_makeClass(vm.builtins, &obj, name, baseClass)

#define ATTRIBUTE_NOT_ASSIGNABLE() do { if (unlikely(argc != 1)) return krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", \
	krk_typeName(argv[0]), _method_name); } while (0)

#define METHOD_TAKES_NONE() do { if (unlikely(argc != 1)) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes no arguments (%d given)", \
	_method_name, (argc-1)); } while (0)

#define METHOD_TAKES_EXACTLY(n) do { if (unlikely(argc != (n+1))) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name, "exactly", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define METHOD_TAKES_AT_LEAST(n) do { if (unlikely(argc < (n+1))) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name, "at least", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define METHOD_TAKES_AT_MOST(n) do { if (unlikely(argc > (n+1))) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name, "at most", n, (n != 1) ? "s" : "", (argc-1)); } while (0)

#define FUNCTION_TAKES_NONE() do { if (unlikely(argc != 0)) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes no arguments (%d given)", \
	_method_name, (argc)); } while (0)

#define FUNCTION_TAKES_EXACTLY(n) do { if (unlikely(argc != n)) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name, "exactly", n, (n != 1) ? "s" : "", (argc)); } while (0)

#define FUNCTION_TAKES_AT_LEAST(n) do { if (unlikely(argc < n)) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name, "at least", n, (n != 1) ? "s" : "", (argc)); } while (0)

#define FUNCTION_TAKES_AT_MOST(n) do { if (unlikely(argc > n)) return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)", \
	_method_name, "at most", n, (n != 1) ? "s" : "", (argc)); } while (0)

#define TYPE_ERROR(expected,value) krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%s'", \
		/* Function name */ _method_name, /* expected type */ #expected, krk_typeName(value))

#define NOT_ENOUGH_ARGS(name) krk_runtimeError(vm.exceptions->argumentError, "Expected more args.")

#define CHECK_ARG(i, type, ctype, name) \
	if (unlikely(argc < (i+1))) return NOT_ENOUGH_ARGS(name); \
	if (unlikely(!IS_ ## type (argv[i]))) return TYPE_ERROR(type,argv[i]); \
	ctype name __attribute__((unused)) = AS_ ## type (argv[i])

#define FUNC_NAME(klass, name) _ ## klass ## _ ## name
#define FUNC_SIG(klass, name) _noexport KrkValue FUNC_NAME(klass,name) (int argc, KrkValue argv[], int hasKw)
#define KRK_METHOD(klass, name, ...) FUNC_SIG(klass, name) { \
	static __attribute__ ((unused)) const char* _method_name = # name; \
	CHECK_ARG(0,klass,CURRENT_CTYPE,CURRENT_NAME); \
	__VA_ARGS__ \
	return NONE_VAL(); }

#define KRK_FUNC(name,...) static KrkValue _krk_ ## name (int argc, KrkValue argv[], int hasKw) { \
	static __attribute__ ((unused)) const char* _method_name = # name; \
	__VA_ARGS__ \
	return NONE_VAL(); }

/* This assumes you have a KrkInstance called `module` in the current scope. */
#define MAKE_CLASS(klass) do { krk_makeClass(module,&klass,#klass,vm.baseClasses->objectClass); klass ->allocSize = sizeof(struct klass); } while (0)
#define BIND_METHOD(klass,method) krk_defineNative(&klass->methods, #method, _ ## klass ## _ ## method)
#define BIND_PROP(klass,method) krk_defineNativeProperty(&klass->methods, #method, _ ## klass ## _ ## method)
#define BIND_FUNC(module,func) krk_defineNative(&module->fields, #func, _krk_ ## func)

/**
 * @brief Inline flexible string array.
 */
struct StringBuilder {
	size_t capacity;
	size_t length;
	char * bytes;
};

/**
 * @brief Add a character to the end of a string builder.
 *
 * @param sb String builder to append to.
 * @param c  Character to append.
 */
static inline void pushStringBuilder(struct StringBuilder * sb, char c) {
	if (sb->capacity < sb->length + 1) {
		size_t old = sb->capacity;
		sb->capacity = GROW_CAPACITY(old);
		sb->bytes = GROW_ARRAY(char, sb->bytes, old, sb->capacity);
	}
	sb->bytes[sb->length++] = c;
}

/**
 * @brief Append a string to the end of a string builder.
 *
 * @param sb String builder to append to.
 * @param str C string to add.
 * @param len Length of the C string.
 */
static inline void pushStringBuilderStr(struct StringBuilder * sb, char *str, size_t len) {
	if (sb->capacity < sb->length + len) {
		while (sb->capacity < sb->length + len) {
			size_t old = sb->capacity;
			sb->capacity = GROW_CAPACITY(old);
		}
		sb->bytes = realloc(sb->bytes, sb->capacity);
	}
	for (size_t i = 0; i < len; ++i) {
		sb->bytes[sb->length++] = *(str++);
	}
}

/**
 * @brief Finalize a string builder into a string object.
 *
 * Creates a string object from the contents of the string builder and
 * frees the space allocated for the builder, returning a value representing
 * the newly created string object.
 *
 * @param sb String builder to finalize.
 * @return A value representing a string object.
 */
static inline KrkValue finishStringBuilder(struct StringBuilder * sb) {
	KrkValue out = OBJECT_VAL(krk_copyString(sb->bytes, sb->length));
	FREE_ARRAY(char,sb->bytes, sb->capacity);
	return out;
}

/**
 * @brief Finalize a string builder in a bytes object.
 *
 * Converts the contents of a string builder into a bytes object and
 * frees the space allocated for the builder.
 *
 * @param sb String builder to finalize.
 * @return A value representing a bytes object.
 */
static inline KrkValue finishStringBuilderBytes(struct StringBuilder * sb) {
	KrkValue out = OBJECT_VAL(krk_newBytes(sb->length, (uint8_t*)sb->bytes));
	FREE_ARRAY(char,sb->bytes, sb->capacity);
	return out;
}

/**
 * @brief Discard the contents of a string builder.
 *
 * Frees the resources allocated for the string builder without converting
 * it to a string or bytes object. Call this when an error has been encountered
 * and the contents of a string builder are no longer needed.
 *
 * @param  sb String builder to discard.
 * @return None, as a convenience.
 */
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

#define IS_list(o)    ((IS_INSTANCE(o) && AS_INSTANCE(o)->_class == vm.baseClasses->listClass) || krk_isInstanceOf(o,vm.baseClasses->listClass))
#define AS_list(o)    (KrkList*)AS_OBJECT(o)

#define IS_tuple(o)    IS_TUPLE(o)
#define AS_tuple(o)    AS_TUPLE(o)

#define IS_bytes(o)    IS_BYTES(o)
#define AS_bytes(o)    AS_BYTES(o)

#define IS_class(o)    IS_CLASS(o)
#define AS_class(o)    AS_CLASS(o)

#define IS_str(o)     (IS_STRING(o)||krk_isInstanceOf(o,vm.baseClasses->strClass))
#define AS_str(o)     (KrkString*)AS_OBJECT(o)

#define IS_striterator(o) (krk_isInstanceOf(o,vm.baseClasses->striteratorClass))
#define AS_striterator(o) (AS_INSTANCE(o))

#define IS_dict(o)    ((IS_INSTANCE(o) && AS_INSTANCE(o)->_class == vm.baseClasses->dictClass) || krk_isInstanceOf(o,vm.baseClasses->dictClass))
#define AS_dict(o)    (KrkDict*)AS_OBJECT(o)

#define IS_dictitems(o) krk_isInstanceOf(o,vm.baseClasses->dictitemsClass)
#define AS_dictitems(o) ((struct DictItems*)AS_OBJECT(o))

#define IS_dictkeys(o) krk_isInstanceOf(o,vm.baseClasses->dictkeysClass)
#define AS_dictkeys(o) ((struct DictKeys*)AS_OBJECT(o))

#define IS_bytesiterator(o) (krk_isInstanceOf(o,vm.baseClasses->bytesiteratorClass))
#define AS_bytesiterator(o) (AS_INSTANCE(o))

#ifndef unpackError
#define unpackError(fromInput) return krk_runtimeError(vm.exceptions->typeError, "'%s' object is not iterable", krk_typeName(fromInput));
#endif

extern KrkValue krk_dict_nth_key_fast(size_t capacity, KrkTableEntry * entries, size_t index);
extern KrkValue FUNC_NAME(str,__getitem__)(int,KrkValue*,int);
extern KrkValue FUNC_NAME(str,__int__)(int,KrkValue*,int);
extern KrkValue FUNC_NAME(str,__float__)(int,KrkValue*,int);
extern KrkValue FUNC_NAME(str,split)(int,KrkValue*,int);
extern KrkValue FUNC_NAME(str,format)(int,KrkValue*,int);
#define krk_string_get FUNC_NAME(str,__getitem__)
#define krk_string_int FUNC_NAME(str,__int__)
#define krk_string_float FUNC_NAME(str,__float__)
#define krk_string_split FUNC_NAME(str,split)
#define krk_string_format FUNC_NAME(str,format)

#define unpackIterable(fromInput) do { \
	KrkClass * type = krk_getType(fromInput); \
	if (type->_iter) { \
		size_t stackOffset = krk_currentThread.stackTop - krk_currentThread.stack; \
		krk_push(fromInput); \
		krk_push(krk_callDirect(type->_iter,1)); \
		do { \
			krk_push(krk_currentThread.stack[stackOffset]); \
			krk_push(krk_callStack(0)); \
			if (krk_valuesSame(krk_currentThread.stack[stackOffset], krk_peek(0))) { \
				krk_pop(); \
				krk_pop(); \
				break; \
			} \
			unpackArray(1,krk_peek(0)); \
			krk_pop(); \
		} while (1); \
	} else { \
		unpackError(fromInput); \
	} \
} while (0)

#define unpackIterableFast(fromInput) do { \
	__attribute__((unused)) int unpackingIterable = 0; \
	KrkValue iterableValue = (fromInput); \
	if (IS_TUPLE(iterableValue)) { \
		unpackArray(AS_TUPLE(iterableValue)->values.count, AS_TUPLE(iterableValue)->values.values[i]); \
	} else if (IS_INSTANCE(iterableValue) && AS_INSTANCE(iterableValue)->_class == vm.baseClasses->listClass) { \
		unpackArray(AS_LIST(iterableValue)->count, AS_LIST(iterableValue)->values[i]); \
	} else if (IS_INSTANCE(iterableValue) && AS_INSTANCE(iterableValue)->_class == vm.baseClasses->dictClass) { \
		unpackArray(AS_DICT(iterableValue)->count, krk_dict_nth_key_fast(AS_DICT(iterableValue)->capacity, AS_DICT(iterableValue)->entries, i)); \
	} else if (IS_STRING(iterableValue)) { \
		unpackArray(AS_STRING(iterableValue)->codesLength, krk_string_get(2,(KrkValue[]){iterableValue,INTEGER_VAL(i)},0)); \
	} else { \
		unpackingIterable = 1; \
		unpackIterable(iterableValue); \
	} \
} while (0)

static inline void _setDoc_class(KrkClass * thing, const char * text, size_t size) {
	thing->docstring = krk_copyString(text, size);
}
static inline void _setDoc_instance(KrkInstance * thing, const char * text, size_t size) {
	krk_attachNamedObject(&thing->fields, "__doc__", (KrkObj*)krk_copyString(text, size));
}
static inline void _setDoc_native(KrkNative * thing, const char * text, size_t size) {
	(void)size;
	thing->doc = text;
}

/**
 * @def KRK_DOC(thing,text)
 * @brief Attach documentation to a thing of various types.
 *
 * Classes store their docstrings directly, rather than in their attribute tables.
 * Instances use the attribute table and store strings with the name @c \__doc__.
 * Native functions store direct C string pointers for documentation.
 *
 * This macro provides a generic interface for applying documentation strings to
 * any of the above types, and handles not attaching documentation when built
 * with KRK_NO_DOCUMENTATION.
 */
#ifdef KRK_NO_DOCUMENTATION
# define KRK_DOC(thing, text) (thing);
#else
# define KRK_DOC(thing, text) \
	_Generic(&((thing)[0]), \
		KrkClass*: _setDoc_class, \
		KrkInstance*: _setDoc_instance, \
		KrkNative*: _setDoc_native \
	)(thing,text,sizeof(text)-1)
#endif

#define BUILTIN_FUNCTION(name, func, docStr) KRK_DOC(krk_defineNative(&vm.builtins->fields, name, func), docStr)

