#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

#define ALLOCATE_OBJECT(type, objectType) \
	(type*)allocateObject(sizeof(type), objectType)

static KrkObj * allocateObject(size_t size, ObjType type) {
	KrkObj * object = (KrkObj*)krk_reallocate(NULL, 0, size);
	object->type = type;
	object->next = vm.objects;
	vm.objects = object;
	return object;
}

static KrkString * allocateString(char * chars, size_t length) {
	KrkString * string = ALLOCATE_OBJECT(KrkString, OBJ_STRING);
	string->length = length;
	string->chars = chars;
	return string;
}

KrkString * takeString(char * chars, size_t length) {
	return allocateString(chars, length);
}

KrkString * copyString(const char * chars, size_t length) {
	char * heapChars = ALLOCATE(char, length + 1);
	memcpy(heapChars, chars, length);
	heapChars[length] = '\0';
	return allocateString(heapChars, length);
}

void krk_printObject(FILE * f, KrkValue value) {
	switch (OBJECT_TYPE(value)) {
		case OBJ_STRING:
			fprintf(stderr, "%s", AS_CSTRING(value));
			break;
	}
}
