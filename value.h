#pragma once

#include <stdio.h>
#include "kuroko.h"

typedef struct Obj KrkObj;
typedef struct ObjString KrkString;

typedef enum {
	VAL_NONE,
	VAL_BOOLEAN,
	VAL_INTEGER,
	VAL_FLOATING,
	VAL_HANDLER,
	VAL_OBJECT,
	/* More here later */
} KrkValueType;

typedef struct {
	KrkValueType type;
	union {
		char boolean;
		long integer;
		double  floating;
		long handler;
		KrkObj *   object;
	} as;
} KrkValue;

#define BOOLEAN_VAL(value)  ((KrkValue){VAL_BOOLEAN, {.boolean = value}})
#define NONE_VAL(value)     ((KrkValue){VAL_NONE,    {.integer = 0}})
#define INTEGER_VAL(value)  ((KrkValue){VAL_INTEGER, {.integer = value}})
#define FLOATING_VAL(value) ((KrkValue){VAL_FLOATING,{.floating = value}})
#define HANDLER_VAL(value)  ((KrkValue){VAL_HANDLER, {.handler = value}})
#define OBJECT_VAL(value)   ((KrkValue){VAL_OBJECT,  {.object = (KrkObj*)value}})

#define AS_BOOLEAN(value)   ((value).as.boolean)
#define AS_INTEGER(value)   ((value).as.integer)
#define AS_FLOATING(value)  ((value).as.floating)
#define AS_HANDLER(value)   ((value).as.handler)
#define AS_OBJECT(value)    ((value).as.object)

#define IS_BOOLEAN(value)   ((value).type == VAL_BOOLEAN)
#define IS_NONE(value)      ((value).type == VAL_NONE)
#define IS_INTEGER(value)   ((value).type == VAL_INTEGER)
#define IS_FLOATING(value)  ((value).type == VAL_FLOATING)
#define IS_HANDLER(value)   ((value).type == VAL_HANDLER)
#define IS_OBJECT(value)    ((value).type == VAL_OBJECT)

typedef struct {
	size_t capacity;
	size_t count;
	KrkValue * values;
} KrkValueArray;

extern void krk_initValueArray(KrkValueArray * array);
extern void krk_writeValueArray(KrkValueArray * array, KrkValue value);
extern void krk_freeValueArray(KrkValueArray * array);
extern void krk_printValue(FILE * f, KrkValue value);
extern int krk_valuesEqual(KrkValue a, KrkValue b);

