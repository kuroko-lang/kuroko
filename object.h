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
#define IS_FUNCTION(value) isObjType(value, OBJ_FUNCTION)
#define AS_FUNCTION(value) ((KrkFunction *)AS_OBJECT(value))
#define IS_NATIVE(value)   isObjType(value, OBJ_NATIVE)
#define AS_NATIVE(value)   (((KrkNative *)AS_OBJECT(value))->function)
#define IS_CLOSURE(value)  isObjType(value, OBJ_CLOSURE)
#define AS_CLOSURE(value)  ((KrkClosure *)AS_OBJECT(value))
#define IS_CLASS(value)    isObjType(value, OBJ_CLASS)
#define AS_CLASS(value)    ((KrkClass *)AS_OBJECT(value))
#define IS_INSTANCE(value) isObjType(value, OBJ_INSTANCE)
#define AS_INSTANCE(value) ((KrkInstance *)AS_OBJECT(value))
#define IS_BOUND_METHOD(value) isObjType(value, OBJ_BOUND_METHOD)
#define AS_BOUND_METHOD(value) ((KrkBoundMethod*)AS_OBJECT(value))

typedef enum {
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_CLOSURE,
	OBJ_STRING,
	OBJ_UPVALUE,
	OBJ_CLASS,
	OBJ_INSTANCE,
	OBJ_BOUND_METHOD,
} ObjType;

struct Obj {
	ObjType type;
	char isMarked;
	struct Obj * next;
};

struct ObjString {
	KrkObj obj;
	size_t length;
	char * chars;
	uint32_t hash;
};

typedef struct KrkUpvalue {
	KrkObj obj;
	int location;
	KrkValue   closed;
	struct KrkUpvalue * next;
} KrkUpvalue;

typedef struct {
	KrkObj obj;
	short requiredArgs;
	short defaultArgs;
	size_t upvalueCount;
	KrkChunk chunk;
	KrkString * name;
	KrkString * docstring;
} KrkFunction;

typedef struct {
	KrkObj obj;
	KrkFunction * function;
	KrkUpvalue ** upvalues;
	size_t upvalueCount;
} KrkClosure;

typedef struct KrkClass {
	KrkObj obj;
	KrkString * name;
	KrkString * filename;
	KrkString * docstring;
	struct KrkClass * base;
	KrkTable methods;
} KrkClass;

typedef struct {
	KrkObj obj;
	KrkClass * _class;
	KrkTable fields;
} KrkInstance;

typedef struct {
	KrkObj obj;
	KrkValue receiver;
	KrkObj * method;
} KrkBoundMethod;

typedef KrkValue (*NativeFn)(int argCount, KrkValue* args);
typedef struct {
	KrkObj obj;
	NativeFn function;
	const char * name;
	int isMethod;
} KrkNative;

#define AS_LIST(value) (&AS_FUNCTION(value)->chunk.constants)
#define AS_DICT(value) (&AS_CLASS(value)->methods)
typedef KrkFunction KrkList;
typedef KrkClass KrkDict;
#define krk_newList() AS_LIST(krk_list_of(0,(KrkValue[]){}))
#define krk_newDict() AS_DICT(krk_dict_of(0,(KrkValue[]){}))

static inline int isObjType(KrkValue value, ObjType type) {
	return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

extern KrkString * krk_takeString(char * chars, size_t length);
extern KrkString * krk_copyString(const char * chars, size_t length);
extern KrkFunction *    krk_newFunction();
extern KrkNative * krk_newNative(NativeFn function, const char * name, int type);
extern KrkClosure *     krk_newClosure(KrkFunction * function);
extern KrkUpvalue *     krk_newUpvalue(int slot);
extern KrkClass *       krk_newClass(KrkString * name);
extern KrkInstance *    krk_newInstance(KrkClass * _class);
extern KrkBoundMethod * krk_newBoundMethod(KrkValue receiver, KrkObj * method);
