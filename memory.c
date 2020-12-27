#include "vm.h"
#include "memory.h"
#include "object.h"

void * krk_reallocate(void * ptr, size_t old, size_t new) {
	if (new == 0) {
		free(ptr);
		return NULL;
	}

	return realloc(ptr, new);
}

static void freeObject(KrkObj * object) {
	switch (object->type) {
		case OBJ_STRING: {
			KrkString * string = (KrkString*)object;
			FREE_ARRAY(char, string->chars, string->length + 1);
			FREE(KrkString, object);
			break;
		}
		case OBJ_FUNCTION: {
			KrkFunction * function = (KrkFunction*)object;
			krk_freeChunk(&function->chunk);
			FREE(KrkFunction, object);
			break;
		}
		case OBJ_NATIVE: {
			FREE(KrkNative, object);
			break;
		}
	}
}

void krk_freeObjects() {
	KrkObj * object = vm.objects;
	while (object) {
		KrkObj * next = object->next;
		freeObject(object);
		object = next;
	}
}
