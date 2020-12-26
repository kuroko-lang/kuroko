#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"
#include "table.h"

#define ALLOCATE_OBJECT(type, objectType) \
	(type*)allocateObject(sizeof(type), objectType)

static KrkObj * allocateObject(size_t size, ObjType type) {
	KrkObj * object = (KrkObj*)krk_reallocate(NULL, 0, size);
	object->type = type;
	object->next = vm.objects;
	vm.objects = object;
	return object;
}

static KrkString * allocateString(char * chars, size_t length, uint32_t hash) {
	KrkString * string = ALLOCATE_OBJECT(KrkString, OBJ_STRING);
	string->length = length;
	string->chars = chars;
	string->hash = hash;
	krk_tableSet(&vm.strings, OBJECT_VAL(string), NONE_VAL());
	return string;
}

static uint32_t hashString(const char * key, size_t length) {
	uint32_t hash = 0;
	/* This is the so-called "sdbm" hash. It comes from a piece of
	 * public domain code from a clone of ndbm. */
	for (size_t i = 0; i < length; ++i) {
		hash = (int)key[i] + (hash << 6) + (hash << 16) - hash;
	}
	return hash;
}

KrkString * takeString(char * chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	KrkString * interned = tableFindString(&vm.strings, chars, length, hash);
	if (interned != NULL) {
		FREE_ARRAY(char, chars, length + 1);
		return interned;
	}
	return allocateString(chars, length, hash);
}

KrkString * copyString(const char * chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	KrkString * interned = tableFindString(&vm.strings, chars, length, hash);
	if (interned != NULL) return interned;
	char * heapChars = ALLOCATE(char, length + 1);
	memcpy(heapChars, chars, length);
	heapChars[length] = '\0';
	return allocateString(heapChars, length, hash);
}

void krk_printObject(FILE * f, KrkValue value) {
	switch (OBJECT_TYPE(value)) {
		case OBJ_STRING:
			fprintf(f, "%s", AS_CSTRING(value));
			break;
	}
}
