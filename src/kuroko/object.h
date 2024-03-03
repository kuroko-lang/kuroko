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

#ifndef KRK_DISABLE_THREADS
#include <pthread.h>
#endif

/**
 * @brief Union tag for heap objects.
 *
 * Allows for quick identification of special types.
 */
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
	uint16_t type;        /**< @brief Tag indicating core type */
	uint16_t flags;       /**< @brief General object flags, mostly related to garbage collection. */
	uint32_t hash;        /**< @brief Cached hash value for table keys */
	struct KrkObj * next; /**< @brief Invasive linked list of all objects in the VM */
} KrkObj;

#define KRK_OBJ_FLAGS_STRING_MASK   0x0003
#define KRK_OBJ_FLAGS_STRING_ASCII  0x0000
#define KRK_OBJ_FLAGS_STRING_UCS1   0x0001
#define KRK_OBJ_FLAGS_STRING_UCS2   0x0002
#define KRK_OBJ_FLAGS_STRING_UCS4   0x0003

#define KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS 0x0001
#define KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS  0x0002
#define KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR  0x0004
#define KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE  0x0008

#define KRK_OBJ_FLAGS_FUNCTION_MASK                0x0003
#define KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD     0x0001
#define KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD    0x0002

#define KRK_OBJ_FLAGS_NO_INHERIT    0x0200
#define KRK_OBJ_FLAGS_SECOND_CHANCE 0x0100
#define KRK_OBJ_FLAGS_IS_MARKED     0x0010
#define KRK_OBJ_FLAGS_IN_REPR       0x0020
#define KRK_OBJ_FLAGS_IMMORTAL      0x0040
#define KRK_OBJ_FLAGS_VALID_HASH    0x0080


/**
 * @brief String compact storage type.
 *
 * Indicates how raw codepoint values are stored in an a string object.
 * We use the smallest form, of the four options, that can provide
 * direct lookup by codepoint index for all codepoints. ASCII and
 * UCS1 are separated because ASCII can re-use the existing UTF8
 * encoded data instead of allocating a separate buffer for codepoints,
 * but this is not possible for UCS1.
 */
typedef enum {
	/* For compatibility */
	KRK_STRING_ASCII   = KRK_OBJ_FLAGS_STRING_ASCII,  /**< Codepoints can be extracted directly from UTF8 data. */
	KRK_STRING_UCS1    = KRK_OBJ_FLAGS_STRING_UCS1,   /**< Codepoints are one byte. */
	KRK_STRING_UCS2    = KRK_OBJ_FLAGS_STRING_UCS2,   /**< Codepoints are two bytes. */
	KRK_STRING_UCS4    = KRK_OBJ_FLAGS_STRING_UCS4,   /**< Codepoints are four bytes. */
} KrkStringType;

/**
 * @brief Immutable sequence of Unicode codepoints.
 * @extends KrkObj
 */
typedef struct KrkString {
	KrkObj obj;          /**< @protected @brief Base */
	size_t length;       /**< @brief String length in bytes */
	size_t codesLength;  /**< @brief String length in Unicode codepoints */
	char * chars;        /**< @brief UTF8 canonical data */
	void * codes;        /**< @brief Codepoint data */
} KrkString;

/**
 * @brief Immutable sequence of bytes.
 * @extends KrkObj
 */
typedef struct {
	KrkObj obj;       /**< @protected @brief Base */
	size_t length;    /**< @brief Length of data in bytes */
	uint8_t * bytes;  /**< @brief Pointer to separately-stored bytes data */
} KrkBytes;

/**
 * @brief Storage for values referenced from nested functions.
 * @extends KrkObj
 */
typedef struct KrkUpvalue {
	KrkObj obj;                    /**< @protected @brief Base */
	int location;                  /**< @brief Stack offset or -1 if closed */
	KrkValue   closed;             /**< @brief Heap storage for closed value */
	struct KrkUpvalue * next;      /**< @brief Invasive linked list pointer to next upvalue */
	struct KrkThreadState * owner; /**< @brief The thread that owns the stack this upvalue belongs in */
} KrkUpvalue;

/**
 * @brief Metadata on a local variable name in a function.
 *
 * This is used by the disassembler to print the names of
 * locals when they are referenced by instructions.
 */
typedef struct {
	size_t id;        /**< @brief Local ID as used by opcodes; offset from the frame's stack base */
	size_t birthday;  /**< @brief Instruction offset that this local name became valid on */
	size_t deathday;  /**< @brief Instruction offset that this local name becomes invalid on */
	KrkString * name; /**< @brief Name of the local */
} KrkLocalEntry;

/**
 * @brief Map entry of opcode offsets to expressions spans.
 *
 * Used for printing tracebacks with underlined expressions.
 */
typedef struct {
	uint32_t bytecodeOffset;
	uint8_t start;
	uint8_t midStart;
	uint8_t midEnd;
	uint8_t end;
} KrkExpressionsMap;

struct KrkInstance;

typedef struct {
	uint32_t instructionOffset;   /**< @brief Instruction (operand offset) this jump target applies to */
	uint16_t intendedTarget;      /**< @brief High bytes of the intended target. */
	uint8_t  originalOpcode;      /**< @brief Original jump opcode to execute. */
} KrkOverlongJump;

/**
 * @brief Code object.
 * @extends KrkObj
 *
 * Contains the static data associated with a chunk of bytecode.
 */
typedef struct {
	KrkObj obj;                            /**< @protected @brief Base */
	unsigned short requiredArgs;           /**< @brief Arity of required (non-default) arguments */
	unsigned short keywordArgs;            /**< @brief Arity of keyword (default) arguments */
	unsigned short potentialPositionals;   /**< @brief Precalculated positional arguments for complex argument processing */
	unsigned short totalArguments;         /**< @brief Total argument cells we can fill in complex argument processing */
	size_t upvalueCount;                   /**< @brief Number of upvalues this function collects as a closure */
	KrkChunk chunk;                        /**< @brief Bytecode data */
	KrkString * name;                      /**< @brief Name of the function */
	KrkString * docstring;                 /**< @brief Docstring attached to the function */
	KrkValueArray positionalArgNames;      /**< @brief Array of names for positional arguments (and *args) */
	KrkValueArray keywordArgNames;         /**< @brief Array of names for keyword-only arguments (and **kwargs) */
	size_t localNameCapacity;              /**< @brief Capacity of @ref localNames */
	size_t localNameCount;                 /**< @brief Number of entries in @ref localNames */
	KrkLocalEntry * localNames;            /**< @brief Stores the names of local variables used in the function, for debugging */
	KrkString * qualname;                  /**< @brief The dotted name of the function */
	size_t expressionsCapacity;            /**< @brief Capacity of @ref expressions */
	size_t expressionsCount;               /**< @brief Number of entries in @ref expressions */
	KrkExpressionsMap * expressions;       /**< @brief Mapping of bytecode offsets to expression spans for debugging */
	KrkValue jumpTargets;                  /**< @brief Possibly a set of jump targets... */
	KrkOverlongJump * overlongJumps;       /**< @brief Pessimal overlong jump container */
	size_t overlongJumpsCapacity;          /**< @brief Number of possible entries in pessimal jump table */
	size_t overlongJumpsCount;             /**< @brief Number of entries in pessimal jump table */
} KrkCodeObject;


/**
 * @brief Function object.
 * @extends KrkObj
 *
 * Not to be confused with code objects, a closure is a single instance of a function.
 */
typedef struct {
	KrkObj obj;                /**< @protected @brief Base */
	KrkCodeObject * function;  /**< @brief The codeobject containing the bytecode run when this function is called */
	KrkUpvalue ** upvalues;    /**< @brief Array of upvalues collected from the surrounding context when the closure was created */
	size_t upvalueCount;       /**< @brief Number of entries in @ref upvalues */
	KrkValue annotations;      /**< @brief Dictionary of type hints */
	KrkTable fields;           /**< @brief Object attributes table */
	KrkValue globalsOwner;     /**< @brief Owner of the globals table for this function */
	KrkTable * globalsTable;   /**< @brief Pointer to globals table with owner object */
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
	KrkObj obj;               /**< @protected @brief Base */
	struct KrkClass * _class; /**< @brief Metaclass */
	KrkTable methods;         /**< @brief General attributes table */
	KrkString * name;         /**< @brief Name of the class */
	KrkString * filename;     /**< @brief Filename of the original source that defined the codeobject for the class */
	struct KrkClass * base;   /**< @brief Pointer to base class implementation */
	size_t allocSize;         /**< @brief Size to allocate when creating instances of this class */
	KrkCleanupCallback _ongcscan;   /**< @brief C function to call when the garbage collector visits an instance of this class in the scan phase */
	KrkCleanupCallback _ongcsweep;  /**< @brief C function to call when the garbage collector is discarding an instance of this class */
	KrkTable subclasses;      /**< @brief Set of classes that subclass this class */

	KrkObj * _getter;         /**< @brief @c %__getitem__  Called when an instance is subscripted */
	KrkObj * _setter;         /**< @brief @c %__setitem__  Called when a subscripted instance is assigned to */
	KrkObj * _reprer;         /**< @brief @c %__repr__     Called to create a reproducible string representation of an instance */
	KrkObj * _tostr;          /**< @brief @c %__str__      Called to produce a string from an instance */
	KrkObj * _call;           /**< @brief @c %__call__     Called when an instance is called like a function */
	KrkObj * _init;           /**< @brief @c %__init__     Implicitly called when an instance is created */
	KrkObj * _eq;             /**< @brief @c %__eq__       Implementation for equality check (==) */
	KrkObj * _len;            /**< @brief @c %__len__      Generally called by len() but may be used to determine truthiness */
	KrkObj * _enter;          /**< @brief @c %__enter__    Called upon entry into a `with` block */
	KrkObj * _exit;           /**< @brief @c %__exit__     Called upon exit from a `with` block */
	KrkObj * _delitem;        /**< @brief @c %__delitem__  Called when `del` is used with a subscript */
	KrkObj * _iter;           /**< @brief @c %__iter__     Called by `for ... in ...`, etc. */
	KrkObj * _getattr;        /**< @brief @c %__getattr__  Overrides normal behavior for attribute access */
	KrkObj * _dir;            /**< @brief @c %__dir__      Overrides normal behavior for @c dir() */
	KrkObj * _contains;       /**< @brief @c %__contains__ Called to resolve `in` (as a binary operator) */
	KrkObj * _descget;        /**< @brief @c %__get__      Called when a descriptor object is bound as a property */
	KrkObj * _descset;        /**< @brief @c %__set__      Called when a descriptor object is assigned to as a property */
	KrkObj * _classgetitem;   /**< @brief @c %__class_getitem__ Class method called when a type object is subscripted; used for type hints */
	KrkObj * _hash;           /**< @brief @c %__hash__     Called when an instance is a key in a dict or an entry in a set */

	KrkObj * _add, * _radd, * _iadd;
	KrkObj * _sub, * _rsub, * _isub;
	KrkObj * _mul, * _rmul, * _imul;
	KrkObj * _or,  * _ror,  * _ior;
	KrkObj * _xor, * _rxor, * _ixor;
	KrkObj * _and, * _rand, * _iand;
	KrkObj * _mod, * _rmod, * _imod;
	KrkObj * _pow, * _rpow, * _ipow;
	KrkObj * _lshift, * _rlshift, * _ilshift;
	KrkObj * _rshift, * _rrshift, * _irshift;
	KrkObj * _truediv, * _rtruediv, * _itruediv;
	KrkObj * _floordiv, * _rfloordiv, * _ifloordiv;

	KrkObj * _lt, * _gt, * _le, * _ge;
	KrkObj * _invert, * _negate;
	KrkObj * _set_name;
	KrkObj * _matmul, * _rmatmul, * _imatmul;
	KrkObj * _pos;
	KrkObj * _setattr;
	KrkObj * _format;
	KrkObj * _new;
	KrkObj * _bool;

	size_t cacheIndex;
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
	KrkObj obj;         /**< @protected @brief Base */
	KrkClass * _class;  /**< @brief Type */
	KrkTable fields;    /**< @brief Attributes table */
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
	KrkObj obj;         /**< @protected @brief Base */
	KrkValue receiver;  /**< @brief Object to pass as implicit first argument */
	KrkObj * method;    /**< @brief Function to call */
} KrkBoundMethod;

typedef KrkValue (*NativeFn)(int argCount, const KrkValue* args, int hasKwargs);

/**
 * @brief Managed binding to a C function.
 * @extends KrkObj
 *
 * Represents a C function that has been exposed to managed code.
 */
typedef struct {
	KrkObj obj;           /**< @protected @brief Base */
	NativeFn function;    /**< @brief C function pointer */
	const char * name;    /**< @brief Name to use when repring */
	const char * doc;     /**< @brief Docstring to supply from @c %__doc__ */
} KrkNative;

/**
 * @brief Immutable sequence of arbitrary values.
 * @extends KrkObj
 *
 * Tuples are fixed-length non-mutable collections of values intended
 * for use in situations where the flexibility of a list is not needed.
 */
typedef struct {
	KrkObj obj;            /**< @protected @brief Base */
	KrkValueArray values;  /**< @brief Stores the length, capacity, and actual values of the tuple */
} KrkTuple;

/**
 * @brief Mutable array of values.
 * @extends KrkInstance
 *
 * A list is a flexible array of values that can be extended, cleared,
 * sorted, rearranged, iterated over, etc.
 */
typedef struct {
	KrkInstance inst;     /**< @protected @brief Base */
	KrkValueArray values; /**< @brief Stores the length, capacity, and actual values of the list */
#ifndef KRK_DISABLE_THREADS
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
	KrkInstance inst;  /**< @protected @brief Base */
	KrkTable entries;  /**< @brief The actual table of values in the dict */
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
 * @extends KrkInstance
 */
struct DictValues {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

/**
 * @extends KrkInstance
 * @brief Representation of a loaded module.
 */
struct KrkModule {
	KrkInstance inst;
#ifndef KRK_STATIC_ONLY
	krk_dlRefType libHandle;
#endif
};

/**
 * @extends KrkInstance
 */
struct KrkSlice {
	KrkInstance inst;
	KrkValue    start;
	KrkValue    end;
	KrkValue    step;
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
 * @brief Like @ref krk_takeString but for when the caller has already calculated
 *        code lengths, hash, and string type.
 * @memberof KrkString
 *
 * Creates a new string object in cases where the caller has already calculated
 * codepoint length, expanded string type, and hash. Useful for functions that
 * create strings from other KrkStrings, where it's easier to know these things
 * without having to start from scratch.
 *
 * @param chars C string to take ownership of.
 * @param length Length of the C string.
 * @param codesLength Length of the expected resulting KrkString in codepoints.
 * @param type Compact type of the string, eg. UCS1, UCS2, UCS4... @see KrkStringType
 * @param hash Precalculated string hash.
 */
extern KrkString * krk_takeStringVetted(char * chars, size_t length, size_t codesLength, KrkStringType type, uint32_t hash);

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
 * @brief Calls __await__
 */
extern int krk_getAwaitable(void);

/**
 * @brief Special value for type hint expressions.
 *
 * Returns a generic alias object. Bind this to a class's \__class_getitem__
 * to allow for generic collection types to be used in type hints.
 */
extern NativeFn krk_GenericAlias;

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
extern KrkClosure *     krk_newClosure(KrkCodeObject * function, KrkValue globals);

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
 * its __init__ function (eg. with @ref krk_callStack) as needed.
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
