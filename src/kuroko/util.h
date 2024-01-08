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

#if !defined(__has_attribute)
# define __has_attribute(attr) 0
#endif

#if __has_attribute(protected)
# define _protected __attribute__((protected))
#else
# define _protected
#endif

#if !defined(_WIN32) && !defined(EFI_PLATFORM)
#define _noexport __attribute__((visibility("hidden")))
#else
#define _noexport
#endif

#if __has_attribute(unused)
# define _unused __attribute__((unused))
#else
# define _unused
#endif

#if __has_attribute(hot)
# define _hot __attribute__((hot))
#else
# define _hot
#endif

#if __has_attribute(cold)
# define _cold __attribute__((cold))
#else
# define _cold
#endif

#if __has_attribute(nonnull)
# define _nonnull __attribute__((nonnull))
#else
# define _nonnull
#endif

#define ADD_BASE_CLASS(obj, name, baseClass) krk_makeClass(vm.builtins, &obj, name, baseClass)

#define ATTRIBUTE_NOT_ASSIGNABLE() do { if (unlikely(argc != 1)) return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%s'", \
	argv[0], _method_name); } while (0)

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

#define TYPE_ERROR(expected,value) krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%T'", \
		/* Function name */ _method_name, /* expected type */ #expected, value)

#define NOT_ENOUGH_ARGS(name) krk_runtimeError(vm.exceptions->argumentError, "Expected more args.")

#define CHECK_ARG(i, type, ctype, name) \
	if (unlikely(argc < (i+1))) return NOT_ENOUGH_ARGS(name); \
	if (unlikely(!IS_ ## type (argv[i]))) return TYPE_ERROR(type,argv[i]); \
	ctype name _unused = AS_ ## type (argv[i])

#define FUNC_NAME(klass, name) _ ## klass ## _ ## name
#define FUNC_SIG(klass, name) _noexport KrkValue FUNC_NAME(klass,name) (int argc, const KrkValue argv[], int hasKw)

/* This assumes you have a KrkInstance called `module` in the current scope. */
#define MAKE_CLASS(klass) do { krk_makeClass(module,&klass,#klass,vm.baseClasses->objectClass); klass ->allocSize = sizeof(struct klass); } while (0)
#define BIND_METHOD(klass,method) krk_defineNative(&klass->methods, #method, _ ## klass ## _ ## method)
#define BIND_PROP(klass,method) krk_defineNativeProperty(&klass->methods, #method, _ ## klass ## _ ## method)
#define BIND_FUNC(module,func) krk_defineNative(&module->fields, #func, _krk_ ## func)

static inline KrkNative * krk_defineNativeStaticMethod(KrkTable * table, const char * name, NativeFn function) {
	KrkNative * out = krk_defineNative(table,name,function);
	out->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD;
	return out;
}
#define BIND_STATICMETHOD(klass,method) krk_defineNativeStaticMethod(&klass->methods, #method, _ ## klass ## _ ## method)

static inline KrkNative * krk_defineNativeClassMethod(KrkTable * table, const char * name, NativeFn function) {
	KrkNative * out = krk_defineNative(table,name,function);
	out->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	return out;
}
#define BIND_CLASSMETHOD(klass,method) krk_defineNativeClassMethod(&klass->methods, #method, _ ## klass ## _ ## method)

#define KRK_Method_internal_name(klass, name) \
	_krk_method_ ## klass ## _ ## name
#define KRK_Method_internal_sig(klass, name) \
	static inline KrkValue KRK_Method_internal_name(klass,name) (const char * _method_name, CURRENT_CTYPE CURRENT_NAME, int argc, const KrkValue argv[], int hasKw)

#define KRK_Method(klass, name) \
	KRK_Method_internal_sig(klass, name); \
	FUNC_SIG(klass, name) { \
		static const char * _method_name = # name; \
		CHECK_ARG(0,klass,CURRENT_CTYPE,CURRENT_NAME); \
		return KRK_Method_internal_name(klass,name)(_method_name, CURRENT_NAME, argc, argv, hasKw); \
	} \
	KRK_Method_internal_sig(klass,name)

#define KRK_Function_internal_name(name) \
	_krk_function_ ## name
#define KRK_Function_internal_sig(name) \
	static inline KrkValue KRK_Function_internal_name(name) (const char * _method_name, int argc, const KrkValue argv[], int hasKw)

#define KRK_Function(name) \
	KRK_Function_internal_sig(name); \
	static KrkValue _krk_ ## name (int argc, const KrkValue argv[], int hasKw) { \
		static const char* _method_name = # name; \
		return KRK_Function_internal_name(name)(_method_name,argc,argv,hasKw); \
	} \
	KRK_Function_internal_sig(name)

#define KRK_StaticMethod_internal_sig(klass, name) \
	static inline KrkValue KRK_Method_internal_name(klass, name) (const char * _method_name, int argc, const KrkValue argv[], int hasKw)
#define KRK_StaticMethod(klass, name) \
	KRK_StaticMethod_internal_sig(klass, name); \
	FUNC_SIG(klass, name) { \
		static const char * _method_name = # name; \
		return KRK_Method_internal_name(klass,name)(_method_name,argc,argv,hasKw); \
	} \
	KRK_StaticMethod_internal_sig(klass,name)

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
void krk_pushStringBuilder(struct StringBuilder * sb, char c);
#define pushStringBuilder krk_pushStringBuilder

/**
 * @brief Append a string to the end of a string builder.
 *
 * @param sb String builder to append to.
 * @param str C string to add.
 * @param len Length of the C string.
 */
void krk_pushStringBuilderStr(struct StringBuilder * sb, const char *str, size_t len);
#define pushStringBuilderStr krk_pushStringBuilderStr

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
KrkValue krk_finishStringBuilder(struct StringBuilder * sb);
#define finishStringBuilder krk_finishStringBuilder

/**
 * @brief Finalize a string builder in a bytes object.
 *
 * Converts the contents of a string builder into a bytes object and
 * frees the space allocated for the builder.
 *
 * @param sb String builder to finalize.
 * @return A value representing a bytes object.
 */
KrkValue krk_finishStringBuilderBytes(struct StringBuilder * sb);
#define finishStringBuilderBytes krk_finishStringBuilderBytes

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
KrkValue krk_discardStringBuilder(struct StringBuilder * sb);
#define discardStringBuilder krk_discardStringBuilder

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

#define IS_dictvalues(o) krk_isInstanceOf(o,vm.baseClasses->dictvaluesClass)
#define AS_dictvalues(o) ((struct DictValues*)AS_OBJECT(o))

#define IS_bytearray(o) (krk_isInstanceOf(o,vm.baseClasses->bytearrayClass))
#define AS_bytearray(o) ((struct ByteArray*)AS_INSTANCE(o))

#define IS_slice(o) krk_isInstanceOf(o,vm.baseClasses->sliceClass)
#define AS_slice(o) ((struct KrkSlice*)AS_INSTANCE(o))

extern KrkValue krk_dict_nth_key_fast(size_t capacity, KrkTableEntry * entries, size_t index);
extern KrkValue FUNC_NAME(str,__getitem__)(int,const KrkValue*,int);
extern KrkValue FUNC_NAME(str,split)(int,const KrkValue*,int);
extern KrkValue FUNC_NAME(str,format)(int,const KrkValue*,int);
#define krk_string_get FUNC_NAME(str,__getitem__)
#define krk_string_split FUNC_NAME(str,split)
#define krk_string_format FUNC_NAME(str,format)

static inline void _setDoc_class(KrkClass * thing, const char * text, size_t size) {
	krk_attachNamedObject(&thing->methods, "__doc__", (KrkObj*)krk_copyString(text, size));
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

extern int krk_extractSlicer(const char * _method_name, KrkValue slicerVal, krk_integer_type count, krk_integer_type *start, krk_integer_type *end, krk_integer_type *step);
#define KRK_SLICER(arg,count) \
	krk_integer_type start; \
	krk_integer_type end; \
	krk_integer_type step; \
	if (krk_extractSlicer(_method_name, arg, count, &start, &end, &step)) 

/**
 * @brief Unpack an iterable.
 *
 * Unpacks an iterable value, passing a series of arrays of values to a callback, @p callback.
 *
 * If @p iterable is a list or tuple, @p callback will be called once with the total size of the container.
 * Otherwise, @p callback will be called many times with a count of 1, until the iterable is exhausted.
 *
 * If @p iterable is not iterable, an exception is set and 1 is returned.
 * If @p callback returns non-zero, unpacking stops and 1 is returned, with no additional exception.
 */
extern int krk_unpackIterable(KrkValue iterable, void * context, int callback(void *, const KrkValue *, size_t));


#define KRK_BASE_CLASS(cls) (vm.baseClasses->cls ## Class)
#define KRK_EXC(exc) (vm.exceptions->exc)


extern int krk_parseVArgs(
		const char * _method_name,
		int argc, const KrkValue argv[], int hasKw,
		const char * fmt, const char ** names, va_list args);

extern int krk_parseArgs_impl(
		const char * _method_name,
		int argc, const KrkValue argv[], int hasKw,
		const char * format, const char ** names, ...);

/**
 * @def krk_parseArgs(f,n,...)
 * @brief Parse arguments to a function while accepting keyword arguments.
 *
 * Convenience macro for @c krk_parseArgs_impl to avoid needing to pass all of
 * the implicit arguments normally provided to a KRK_Function or KRK_Method.
 *
 * @param f Format string.
 * @param n Names array.
 * @returns 1 on success, 0 on failure with an exception set.
 */
#define krk_parseArgs(f,n,...) krk_parseArgs_impl(_method_name,argc,argv,hasKw,f,n,__VA_ARGS__)


extern int krk_pushStringBuilderFormatV(struct StringBuilder * sb, const char * fmt, va_list args);
extern int krk_pushStringBuilderFormat(struct StringBuilder * sb, const char * fmt, ...);
extern KrkValue krk_stringFromFormat(const char * fmt, ...);
extern int krk_long_to_int(KrkValue val, char size, void * out);
extern int krk_isSubClass(const KrkClass * cls, const KrkClass * base);

#define KRK_Module_internal_name(name) \
	_krk_module_onload_ ## name
#define KRK_Module_internal_sig(name) \
	static inline void KRK_Module_internal_name(name) (KrkInstance * module, KrkString * runAs)
#define KRK_Module(name) \
	KRK_Module_internal_sig(name); \
	KrkValue krk_module_onload_ ## name (KrkString * runAs) { \
		KrkInstance * module = krk_newInstance(KRK_BASE_CLASS(module)); \
		krk_push(OBJECT_VAL(module)); \
		krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)runAs); \
		KRK_Module_internal_name(name)(module, runAs); \
		return krk_pop(); \
	} \
	KRK_Module_internal_sig(name)
