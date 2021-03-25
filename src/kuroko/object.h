#pragma once
/**
 * @file object.h
 * @brief Struct definitions for core object types.
 */
#include <stdio.h>
#include "kuroko.h"
#include "value.h"
#include "chunk.h"
#include "table.h"

#ifdef ENABLE_THREADING
#include <pthread.h>
#endif

typedef enum {
	KRK_OBJ_CODEOBJECT,
	KRK_OBJ_NATIVE,
	KRK_OBJ_CLOSURE,
	KRK_OBJ_STRING,
	KRK_OBJ_UPVALUE,
	KRK_OBJ_CLASS,
	KRK_OBJ_INSTANCE,
	KRK_OBJ_BOUND_METHOD,
	KRK_OBJ_TUPLE,
	KRK_OBJ_BYTES,
} KrkObjType;

#undef KrkObj
/**
 * @brief The most basic object type.
 *
 * This is the base of all object types and contains
 * the core structures for garbage collection.
 */
typedef struct KrkObj {
	KrkObjType type;
	unsigned char isMarked:1;
	unsigned char inRepr:1;
	unsigned char generation:2;
	unsigned char isImmortal:1;
	uint32_t hash;
	struct KrkObj * next;
} KrkObj;

typedef enum {
	KRK_STRING_ASCII = 0,
	KRK_STRING_UCS1  = 1,
	KRK_STRING_UCS2  = 2,
	KRK_STRING_UCS4  = 4,
	KRK_STRING_INVALID = 5,
} KrkStringType;

#undef KrkString
/**
 * @brief Immutable sequence of Unicode codepoints.
 * @extends KrkObj
 */
typedef struct KrkString {
	KrkObj obj;
	KrkStringType type;
	size_t length;
	size_t codesLength;
	char * chars;
	void * codes;
} KrkString;

/**
 * @brief Immutable sequence of bytes.
 * @extends KrkObj
 */
typedef struct {
	KrkObj obj;
	size_t length;
	uint8_t * bytes;
} KrkBytes;

/**
 * @brief Storage for values referenced from nested functions.
 * @extends KrkObj
 */
typedef struct KrkUpvalue {
	KrkObj obj;
	int location;
	KrkValue   closed;
	struct KrkUpvalue * next;
	struct KrkThreadState * owner;
} KrkUpvalue;

/**
 * @brief Metadata on a local variable name in a function.
 *
 * This is used by the disassembler to print the names of
 * locals when they are referenced by instructions.
 */
typedef struct {
	size_t id;
	size_t birthday;
	size_t deathday;
	KrkString * name;
} KrkLocalEntry;

struct KrkInstance;

/**
 * @brief Code object.
 * @extends KrkObj
 *
 * Contains the static data associated with a chunk of bytecode.
 */
typedef struct {
	KrkObj obj;
	short requiredArgs;
	short keywordArgs;
	size_t upvalueCount;
	KrkChunk chunk;
	KrkString * name;
	KrkString * docstring;
	KrkValueArray requiredArgNames;
	KrkValueArray keywordArgNames;
	size_t localNameCapacity;
	size_t localNameCount;
	KrkLocalEntry * localNames;
	unsigned char collectsArguments:1;
	unsigned char collectsKeywords:1;
	unsigned char isGenerator:1;
	struct KrkInstance * globalsContext;
	KrkString * qualname;
} KrkCodeObject;

/**
 * @brief Function object.
 * @extends KrkObj
 *
 * Not to be confused with code objects, a closure is a single instance of a function.
 */
typedef struct {
	KrkObj obj;
	KrkCodeObject * function;
	KrkUpvalue ** upvalues;
	size_t upvalueCount;
	unsigned char isClassMethod:1;
	unsigned char isStaticMethod:1;
	KrkValue annotations;
	KrkTable fields;
} KrkClosure;

typedef void (*KrkCleanupCallback)(struct KrkInstance *);

/**
 * @brief Type object.
 * @extends KrkObj
 *
 * Represents classes defined in user code as well as classes defined
 * by C extensions to represent method tables for new types.
 */
typedef struct KrkClass {
	KrkObj obj;
	KrkString * name;
	KrkString * filename;
	KrkString * docstring;
	struct KrkClass * base;
	KrkTable methods;
	size_t allocSize;
	KrkCleanupCallback _ongcscan;
	KrkCleanupCallback _ongcsweep;

	/* Quick access for common stuff */
	KrkObj * _getter;
	KrkObj * _setter;
	KrkObj * _getslice;
	KrkObj * _reprer;
	KrkObj * _tostr;
	KrkObj * _call;
	KrkObj * _init;
	KrkObj * _eq;
	KrkObj * _len;
	KrkObj * _enter;
	KrkObj * _exit;
	KrkObj * _delitem;
	KrkObj * _iter;
	KrkObj * _getattr;
	KrkObj * _dir;
	KrkObj * _setslice;
	KrkObj * _delslice;
	KrkObj * _contains;
	KrkObj * _descget;
	KrkObj * _descset;
	KrkObj * _classgetitem;
} KrkClass;

/**
 * @brief An object of a class.
 * @extends KrkObj
 *
 * Created by class initializers, instances are the standard type of objects
 * built by managed code. Not all objects are instances, but all instances are
 * objects, and all instances have well-defined class.
 */
typedef struct KrkInstance {
	KrkObj obj;
	KrkClass * _class;
	KrkTable fields;
} KrkInstance;

/**
 * @brief A function that has been attached to an object to serve as a method.
 * @extends KrkObj
 *
 * When a bound method is called, its receiver is implicitly extracted as
 * the first argument. Bound methods are created whenever a method is retreived
 * from the class of a value.
 */
typedef struct {
	KrkObj obj;
	KrkValue receiver;
	KrkObj * method;
} KrkBoundMethod;

typedef KrkValue (*NativeFn)(int argCount, KrkValue* args, int hasKwargs);

/**
 * @brief Managed binding to a C function.
 * @extends KrkObj
 *
 * Represents a C function that has been exposed to managed code.
 */
typedef struct {
	KrkObj obj;
	NativeFn function;
	const char * name;
	const char * doc;
	unsigned int isDynamicProperty:1;
} KrkNative;

/**
 * @brief Immutable sequence of arbitrary values.
 * @extends KrkObj
 *
 * Tuples are fixed-length non-mutable collections of values intended
 * for use in situations where the flexibility of a list is not needed.
 */
typedef struct {
	KrkObj obj;
	KrkValueArray values;
} KrkTuple;

/**
 * @brief Mutable array of values.
 * @extends KrkInstance
 *
 * A list is a flexible array of values that can be extended, cleared,
 * sorted, rearranged, iterated over, etc.
 */
typedef struct {
	KrkInstance inst;
	KrkValueArray values;
#ifdef ENABLE_THREADING
	pthread_rwlock_t rwlock;
#endif
} KrkList;

/**
 * @brief Flexible mapping type.
 * @extends KrkInstance
 *
 * Provides key-to-value mappings as a first-class object type.
 */
typedef struct {
	KrkInstance inst;
	KrkTable entries;
} KrkDict;

/**
 * @extends KrkInstance
 */
struct DictItems {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

/**
 * @extends KrkInstance
 */
struct DictKeys {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

/**
 * @brief Yield ownership of a C string to the GC and obtain a string object.
 * @memberof KrkString
 *
 * Creates a string object represented by the characters in 'chars' and of
 * length 'length'. The source string must be nil-terminated and must
 * remain valid for the lifetime of the object, as its ownership is yielded
 * to the GC. Useful for strings which were allocated on the heap by
 * other mechanisms.
 *
 * 'chars' must be a nil-terminated C string representing a UTF-8
 * character sequence.
 *
 * @param chars  C string to take ownership of.
 * @param length Length of the C string.
 * @return A string object.
 */
extern KrkString * krk_takeString(char * chars, size_t length);

/**
 * @brief Obtain a string object representation of the given C string.
 * @memberof KrkString
 *
 * Converts the C string 'chars' into a string object by checking the
 * string table for it. If the string table does not have an equivalent
 * string, a new one will be created by copying 'chars'.
 *
 * 'chars' must be a nil-terminated C string representing a UTF-8
 * character sequence.
 *
 * @param chars  C string to convert to a string object.
 * @param length Length of the C string.
 * @return A string object.
 */
extern KrkString * krk_copyString(const char * chars, size_t length);

/**
 * @brief Ensure that a codepoint representation of a string is available.
 * @memberof KrkString
 *
 * Obtain an untyped pointer to the codepoint representation of a string.
 * If the string does not have a codepoint representation allocated, it will
 * be generated by this function and remain with the string for the duration
 * of its lifetime.
 *
 * @param string String to obtain the codepoint representation of.
 * @return A pointer to the bytes of the codepoint representation.
 */
extern void * krk_unicodeString(KrkString * string);

/**
 * @brief Obtain the codepoint at a given index in a string.
 * @memberof KrkString
 *
 * This is a convenience function which ensures that a Unicode codepoint
 * representation has been generated and returns the codepoint value at
 * the requested index. If you need to find multiple codepoints, it
 * is recommended that you use the KRK_STRING_FAST macro after calling
 * krk_unicodeString instead.
 *
 * @note This function does not perform any bounds checking.
 *
 * @param string String to index into.
 * @param index  Offset of the codepoint to obtain.
 * @return Integer representation of the codepoint at the requested index.
 */
extern uint32_t krk_unicodeCodepoint(KrkString * string, size_t index);

/**
 * @brief Convert an integer codepoint to a UTF-8 byte representation.
 * @memberof KrkString
 *
 * Converts a single codepoint to a sequence of bytes containing the
 * UTF-8 representation. 'out' must be allocated by the caller.
 *
 * @param value Codepoint to encode.
 * @param out   Array to write UTF-8 sequence into.
 * @return The length of the UTF-8 sequence, in bytes.
 */
extern size_t krk_codepointToBytes(krk_integer_type value, unsigned char * out);

/**
 * @brief Convert a function into a generator with the given arguments.
 * @memberof KrkClosure
 *
 * Converts the function @p function to a generator object and provides it @p arguments
 * (of length @p argCount) as its initial arguments. The generator object is returned.
 *
 * @param function  Function to convert.
 * @param arguments Arguments to pass to the generator.
 * @param argCount  Number of arguments in @p arguments
 * @return A new generator object.
 */
extern KrkInstance * krk_buildGenerator(KrkClosure * function, KrkValue * arguments, size_t argCount);

/**
 * @brief Special value for type hint expressions.
 *
 * Returns a generic alias object. Bind this to a class's \__class_getitem__
 * to allow for generic collection types to be used in type hints.
 */
extern NativeFn KrkGenericAlias;

/**
 * @brief Create a new, uninitialized code object.
 * @memberof KrkCodeObject
 *
 * The code object will have an empty bytecode chunk and
 * no assigned names or docstrings. This is intended only
 * to be used by a compiler directly.
 */
extern KrkCodeObject *    krk_newCodeObject(void);

/**
 * @brief Create a native function binding object.
 * @memberof KrkNative
 *
 * Converts a C function pointer into a native binding object
 * which can then be used in the same place a function object
 * (KrkClosure) would be used.
 */
extern KrkNative *      krk_newNative(NativeFn function, const char * name, int type);

/**
 * @brief Create a new function object.
 * @memberof KrkClosure
 *
 * Function objects are the callable first-class objects representing
 * functions in managed code. Each function object has an associated
 * code object, which may be sured with other function objects, such
 * as when a function is used to create a closure.
 *
 * @param function Code object to assign to the new function object.
 */
extern KrkClosure *     krk_newClosure(KrkCodeObject * function);

/**
 * @brief Create an upvalue slot.
 * @memberof KrkUpvalue
 *
 * Upvalue slots hold references to values captured in closures.
 * This function should only be used directly by the VM in the
 * process of running compiled bytecode and creating function
 * objects from code objects.
 */
extern KrkUpvalue *     krk_newUpvalue(int slot);

/**
 * @brief Create a new class object.
 * @memberof KrkClass
 *
 * Creates a new class with the give name and base class.
 * Generally, you will want to use @ref krk_makeClass instead,
 * which handles binding the class to a module.
 */
extern KrkClass *       krk_newClass(KrkString * name, KrkClass * base);

/**
 * @brief Create a new instance of the given class.
 * @memberof KrkInstance
 *
 * Handles allocation, but not __init__, of the new instance.
 * Be sure to populate any fields expected by the class or call
 * its __init__ function (eg. with @ref krk_callSimple) as needed.
 */
extern KrkInstance *    krk_newInstance(KrkClass * _class);

/**
 * @brief Create a new bound method.
 * @memberof KrkBoundMethod
 *
 * Binds the callable specified by @p method to the value @p receiver
 * and returns a @c method object. When a @c method object is called,
 * @p receiver will automatically be provided as the first argument.
 */
extern KrkBoundMethod * krk_newBoundMethod(KrkValue receiver, KrkObj * method);

/**
 * @brief Create a new tuple.
 * @memberof KrkTuple
 *
 * Creates a tuple object with the request space preallocated.
 * The actual length of the tuple must be updated after places
 * values within it by setting @c value.count.
 */
extern KrkTuple *       krk_newTuple(size_t length);

/**
 * @brief Create a new byte array.
 * @memberof KrkBytes
 *
 * Allocates a bytes object of the given size, optionally copying
 * data from @p source.
 */
extern KrkBytes *       krk_newBytes(size_t length, uint8_t * source);

/**
 * @brief Update the hash of a bytes object.
 * @memberof KrkBytes
 *
 * If the contents of a bytes object have been modified
 * after it was initialized by @ref krk_newBytes, the caller
 * must updates its hash.
 */
extern void krk_bytesUpdateHash(KrkBytes * bytes);

/**
 * @brief Update the hash of a tuple.
 * @memberof KrkTuple
 *
 * If a tuple's contents have been changed, its
 * hash must be updated.
 */
extern void krk_tupleUpdateHash(KrkTuple * self);

#define krk_isObjType(v,t) (IS_OBJECT(v) && (AS_OBJECT(v)->type == (t)))
#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)
#define IS_STRING(value)   krk_isObjType(value, KRK_OBJ_STRING)
#define AS_STRING(value)   ((KrkString *)AS_OBJECT(value))
#define AS_CSTRING(value)  (((KrkString *)AS_OBJECT(value))->chars)
#define IS_BYTES(value)    krk_isObjType(value, KRK_OBJ_BYTES)
#define AS_BYTES(value)    ((KrkBytes*)AS_OBJECT(value))
#define IS_NATIVE(value)   krk_isObjType(value, KRK_OBJ_NATIVE)
#define AS_NATIVE(value)   ((KrkNative *)AS_OBJECT(value))
#define IS_CLOSURE(value)  krk_isObjType(value, KRK_OBJ_CLOSURE)
#define AS_CLOSURE(value)  ((KrkClosure *)AS_OBJECT(value))
#define IS_CLASS(value)    krk_isObjType(value, KRK_OBJ_CLASS)
#define AS_CLASS(value)    ((KrkClass *)AS_OBJECT(value))
#define IS_INSTANCE(value) krk_isObjType(value, KRK_OBJ_INSTANCE)
#define AS_INSTANCE(value) ((KrkInstance *)AS_OBJECT(value))
#define IS_BOUND_METHOD(value) krk_isObjType(value, KRK_OBJ_BOUND_METHOD)
#define AS_BOUND_METHOD(value) ((KrkBoundMethod*)AS_OBJECT(value))
#define IS_TUPLE(value)    krk_isObjType(value, KRK_OBJ_TUPLE)
#define AS_TUPLE(value)    ((KrkTuple *)AS_OBJECT(value))
#define AS_LIST(value) (&((KrkList *)AS_OBJECT(value))->values)
#define AS_DICT(value) (&((KrkDict *)AS_OBJECT(value))->entries)

#define IS_codeobject(value) krk_isObjType(value, KRK_OBJ_CODEOBJECT)
#define AS_codeobject(value) ((KrkCodeObject *)AS_OBJECT(value))
