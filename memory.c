#include "vm.h"
#include "memory.h"
#include "object.h"
#include "compiler.h"
#include "table.h"

void * krk_reallocate(void * ptr, size_t old, size_t new) {
	vm.bytesAllocated += new - old;

	if (new > old && ptr != vm.stack && !(vm.flags & KRK_GC_PAUSED)) {
#ifdef ENABLE_STRESS_GC
		if (vm.flags & KRK_ENABLE_STRESS_GC) {
			krk_collectGarbage();
		}
#endif
		if (vm.bytesAllocated > vm.nextGC) {
			krk_collectGarbage();
		}
	}

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
		case OBJ_CLOSURE: {
			KrkClosure * closure = (KrkClosure*)object;
			FREE_ARRAY(KrkUpvalue*,closure->upvalues,closure->upvalueCount);
			FREE(KrkClosure, object);
			break;
		}
		case OBJ_UPVALUE: {
			FREE(KrkUpvalue, object);
			break;
		}
		case OBJ_CLASS: {
			KrkClass * _class = (KrkClass*)object;
			krk_freeTable(&_class->methods);
			FREE(KrkClass, object);
			break;
		}
		case OBJ_INSTANCE: {
			krk_freeTable(&((KrkInstance*)object)->fields);
			FREE(KrkInstance, object);
			break;
		}
		case OBJ_BOUND_METHOD:
			FREE(KrkBoundMethod, object);
			break;
	}
}

void krk_freeObjects() {
	KrkObj * object = vm.objects;
	while (object) {
		KrkObj * next = object->next;
		freeObject(object);
		object = next;
	}
	free(vm.grayStack);
}

void krk_markObject(KrkObj * object) {
	if (!object) return;
	if (object->isMarked) return;
	object->isMarked = 1;

	if (vm.grayCapacity < vm.grayCount + 1) {
		vm.grayCapacity = GROW_CAPACITY(vm.grayCapacity);
		vm.grayStack = realloc(vm.grayStack, sizeof(KrkObj*) * vm.grayCapacity);
		if (!vm.grayStack) exit(1);
	}
	vm.grayStack[vm.grayCount++] = object;
}

void krk_markValue(KrkValue value) {
	if (!IS_OBJECT(value)) return;
	krk_markObject(AS_OBJECT(value));
}

static void markArray(KrkValueArray * array) {
	for (size_t i = 0; i < array->count; ++i) {
		krk_markValue(array->values[i]);
	}
}

static void blackenObject(KrkObj * object) {
	switch (object->type) {
		case OBJ_CLOSURE: {
			KrkClosure * closure = (KrkClosure *)object;
			krk_markObject((KrkObj*)closure->function);
			for (size_t i = 0; i < closure->upvalueCount; ++i) {
				krk_markObject((KrkObj*)closure->upvalues[i]);
			}
			break;
		}
		case OBJ_FUNCTION: {
			KrkFunction * function = (KrkFunction *)object;
			krk_markObject((KrkObj*)function->name);
			krk_markObject((KrkObj*)function->chunk.filename);
			markArray(&function->chunk.constants);
			break;
		}
		case OBJ_UPVALUE:
			krk_markValue(((KrkUpvalue*)object)->closed);
			break;
		case OBJ_CLASS: {
			KrkClass * _class = (KrkClass *)object;
			krk_markObject((KrkObj*)_class->name);
			krk_markObject((KrkObj*)_class->filename);
			krk_markTable(&_class->methods);
			break;
		}
		case OBJ_INSTANCE: {
			krk_markObject((KrkObj*)((KrkInstance*)object)->_class);
			krk_markTable(&((KrkInstance*)object)->fields);
			break;
		}
		case OBJ_BOUND_METHOD: {
			KrkBoundMethod * bound = (KrkBoundMethod *)object;
			krk_markValue(bound->receiver);
			krk_markObject((KrkObj*)bound->method);
			break;
		}
		case OBJ_NATIVE:
		case OBJ_STRING:
			break;
	}
}

static void traceReferences() {
	while (vm.grayCount > 0) {
		KrkObj * object = vm.grayStack[--vm.grayCount];
		blackenObject(object);
	}
}

static void sweep() {
	KrkObj * previous = NULL;
	KrkObj * object = vm.objects;
	while (object) {
		if (object->isMarked) {
			object->isMarked = 0;
			previous = object;
			object = object->next;
		} else {
			KrkObj * unreached = object;
			object = object->next;
			if (previous != NULL) {
				previous->next = object;
			} else {
				vm.objects = object;
			}
			freeObject(unreached);
		}
	}
}

void krk_markTable(KrkTable * table) {
	for (size_t i = 0; i < table->capacity; ++i) {
		KrkTableEntry * entry = &table->entries[i];
		krk_markValue(entry->key);
		krk_markValue(entry->value);
	}
}

void krk_tableRemoveWhite(KrkTable * table) {
	for (size_t i = 0; i < table->capacity; ++i) {
		KrkTableEntry * entry = &table->entries[i];
		if (IS_OBJECT(entry->key) && !(AS_OBJECT(entry->key))->isMarked) {
			krk_tableDelete(table, entry->key);
		}
	}
}

static void markRoots() {
	for (KrkValue * slot = vm.stack; slot < vm.stackTop; ++slot) {
		krk_markValue(*slot);
	}
	for (KrkUpvalue * upvalue = vm.openUpvalues; upvalue; upvalue = upvalue->next) {
		krk_markObject((KrkObj*)upvalue);
	}
	krk_markTable(&vm.globals);
	krk_markCompilerRoots();
	for (int i = 0; i < METHOD__MAX; ++i) {
		krk_markValue(vm.specialMethodNames[i]);
	}
	krk_markValue(vm.currentException);
}

void krk_collectGarbage(void) {
	markRoots();
	traceReferences();
	krk_tableRemoveWhite(&vm.strings);
	sweep();
	vm.nextGC = vm.bytesAllocated * 2;
}
