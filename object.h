#pragma once

#include <stdio.h>

#include "kuroko.h"
#include "value.h"

#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)
#define IS_STRING(value)   isObjType(value, OBJ_STRING)
#define AS_STRING(value)   ((KrkString *)AS_OBJECT(value))
#define AS_CSTRING(value)  (((KrkString *)AS_OBJECT(value))->chars)

typedef enum {
	OBJ_STRING,
} ObjType;

struct Obj {
	ObjType type;
	struct Obj * next;
};

struct ObjString {
	KrkObj obj;
	size_t length;
	char * chars;
};

static inline int isObjType(KrkValue value, ObjType type) {
	return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

extern KrkString * takeString(char * chars, size_t length);
extern KrkString * copyString(const char * chars, size_t length);
extern void krk_printObject(FILE * f, KrkValue value);
