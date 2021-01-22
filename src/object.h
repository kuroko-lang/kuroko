#pragma once

#include <stdio.h>

#include "kuroko.h"
#include "value.h"
#include "chunk.h"
#include "table.h"

#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)
#define IS_STRING(value)   isObjType(value, OBJ_STRING)
#define AS_STRING(value)   ((KrkString *)AS_OBJECT(value))
#define AS_CSTRING(value)  (((KrkString *)AS_OBJECT(value))->chars)
#define IS_BYTES(value)    isObjType(value, OBJ_BYTES)
#define AS_BYTES(value)    ((KrkBytes*)AS_OBJECT(value))
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define AS_FUNCTION(value) ((KrkFunction *)AS_OBJECT(value))
#define IS_NATIVE(value)   isObjType(value, OBJ_NATIVE)
#define AS_NATIVE(value)   ((KrkNative *)AS_OBJECT(value))
#define IS_CLOSURE(value)  isObjType(value, OBJ_CLOSURE)
#define AS_CLOSURE(value)  ((KrkClosure *)AS_OBJECT(value))
#define IS_CLASS(value)    isObjType(value, OBJ_CLASS)
#define AS_CLASS(value)    ((KrkClass *)AS_OBJECT(value))
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define AS_INSTANCE(value) ((KrkInstance *)AS_OBJECT(value))
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define AS_BOUND_METHOD(value) ((KrkBoundMethod*)AS_OBJECT(value))

#define IS_TUPLE(value)    isObjType(value, OBJ_TUPLE)
#define AS_TUPLE(value)    ((KrkTuple *)AS_OBJECT(value))

typedef enum {
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_CLOSURE,
	OBJ_STRING,
	OBJ_UPVALUE,
	OBJ_CLASS,
	OBJ_INSTANCE,
	OBJ_BOUND_METHOD,
	OBJ_TUPLE,
	OBJ_BYTES,
} ObjType;

struct Obj {
	ObjType type;
	unsigned char isMarked:1;
	unsigned char inRepr:1;
	struct Obj * next;
};

typedef enum {
	KRK_STRING_ASCII = 0,
	KRK_STRING_UCS1  = 1,
	KRK_STRING_UCS2  = 2,
	KRK_STRING_UCS4  = 4,
} KrkStringType;

struct ObjString {
	KrkObj obj;
	KrkStringType type;
	uint32_t hash;
	size_t length;
	size_t codesLength;
	char * chars;
	void * codes;
};

typedef struct {
	KrkObj obj;
	uint32_t hash;
	size_t length;
	uint8_t * bytes;
} KrkBytes;

typedef struct KrkUpvalue {
	KrkObj obj;
	int location;
	KrkValue   closed;
	struct KrkUpvalue * next;
} KrkUpvalue;

typedef struct {
	size_t id;
	size_t birthday;
	size_t deathday;
	KrkString * name;
} KrkLocalEntry;

struct KrkInstance;

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
	struct KrkInstance * globalsContext;
} KrkFunction;

typedef struct {
	KrkObj obj;
	KrkFunction * function;
	KrkUpvalue ** upvalues;
	size_t upvalueCount;
} KrkClosure;

typedef void (*KrkCleanupCallback)(struct KrkInstance *);

typedef struct KrkClass {
	KrkObj obj;
	KrkString * name;
	KrkString * filename;
	KrkString * docstring;
	struct KrkClass * base;
	KrkTable methods;
	KrkTable fields;
	size_t allocSize;
	KrkCleanupCallback _ongcscan;
	KrkCleanupCallback _ongcsweep;

	/* Quick access for common stuff */
	KrkObj * _getter;
	KrkObj * _setter;
	KrkObj * _slicer;
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
} KrkClass;

typedef struct KrkInstance {
	KrkObj obj;
	KrkClass * _class;
	KrkTable fields;
} KrkInstance;

typedef struct {
	KrkObj obj;
	KrkValue receiver;
	KrkObj * method;
} KrkBoundMethod;

typedef KrkValue (*NativeFn)();
typedef KrkValue (*NativeFnKw)(int argCount, KrkValue* args, int hasKwargs);
typedef struct {
	KrkObj obj;
	NativeFn function;
	const char * name;
	const char * doc;
	int isMethod;
} KrkNative;

typedef struct {
	KrkObj obj;
	KrkValueArray values;
} KrkTuple;

typedef struct {
	KrkInstance inst;
	KrkValueArray values;
} KrkList;

typedef struct {
	KrkInstance inst;
	KrkTable entries;
} KrkDict;

#define AS_LIST(value) (&((KrkList *)AS_OBJECT(value))->values)
#define AS_DICT(value) (&((KrkDict *)AS_OBJECT(value))->entries)

static inline int isObjType(KrkValue value, ObjType type) {
	return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

extern KrkString * krk_takeString(char * chars, size_t length);
extern KrkString * krk_copyString(const char * chars, size_t length);
extern KrkFunction *    krk_newFunction(void);
extern KrkNative * krk_newNative(NativeFn function, const char * name, int type);
extern KrkClosure *     krk_newClosure(KrkFunction * function);
extern KrkUpvalue *     krk_newUpvalue(int slot);
extern KrkClass *       krk_newClass(KrkString * name, KrkClass * base);
extern KrkInstance *    krk_newInstance(KrkClass * _class);
extern KrkBoundMethod * krk_newBoundMethod(KrkValue receiver, KrkObj * method);
extern KrkTuple *       krk_newTuple(size_t length);

extern void * krk_unicodeString(KrkString * string);
extern uint32_t krk_unicodeCodepoint(KrkString * string, size_t index);

#define KRK_STRING_FAST(string,offset)  (uint32_t)\
	(string->type == 1 ? ((uint8_t*)string->codes)[offset] : \
	(string->type == 2 ? ((uint16_t*)string->codes)[offset] : \
	((uint32_t*)string->codes)[offset]))

#define CODEPOINT_BYTES(cp) (cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4)))

extern KrkBytes * krk_newBytes(size_t length, uint8_t * source);
extern void krk_bytesUpdateHash(KrkBytes * bytes);
extern size_t krk_codepointToBytes(krk_integer_type value, unsigned char * out);
