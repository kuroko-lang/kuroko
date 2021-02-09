#include "vm.h"
#include "memory.h"
#include "object.h"
#include "compiler.h"
#include "table.h"
#include "util.h"

void * krk_reallocate(void * ptr, size_t old, size_t new) {
	vm.bytesAllocated += new - old;

	if (new > old && ptr != krk_currentThread.stack && &krk_currentThread == vm.threads && !(vm.globalFlags & KRK_GC_PAUSED)) {
#ifdef ENABLE_STRESS_GC
		if (vm.globalFlags & KRK_ENABLE_STRESS_GC) {
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
			if (string->codes && string->codes != string->chars) free(string->codes);
			FREE(KrkString, object);
			break;
		}
		case OBJ_FUNCTION: {
			KrkFunction * function = (KrkFunction*)object;
			krk_freeChunk(&function->chunk);
			krk_freeValueArray(&function->requiredArgNames);
			krk_freeValueArray(&function->keywordArgNames);
			FREE_ARRAY(KrkLocalEntry, function->localNames, function->localNameCount);
			function->localNameCount = 0;
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
			krk_freeTable(&_class->fields);
			FREE(KrkClass, object);
			break;
		}
		case OBJ_INSTANCE: {
			if (((KrkInstance*)object)->_class->_ongcsweep) ((KrkInstance*)object)->_class->_ongcsweep((KrkInstance*)object);
			krk_freeTable(&((KrkInstance*)object)->fields);
			FREE(KrkInstance, object);
			break;
		}
		case OBJ_BOUND_METHOD:
			FREE(KrkBoundMethod, object);
			break;
		case OBJ_TUPLE: {
			KrkTuple * tuple = (KrkTuple*)object;
			krk_freeValueArray(&tuple->values);
			FREE(KrkTuple, object);
			break;
		}
		case OBJ_BYTES: {
			KrkBytes * bytes = (KrkBytes*)object;
			FREE_ARRAY(uint8_t, bytes->bytes, bytes->length);
			FREE(KrkBytes, bytes);
			break;
		}
		case OBJ_PROPERTY: {
			FREE(KrkProperty, object);
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
			krk_markObject((KrkObj*)function->docstring);
			krk_markObject((KrkObj*)function->chunk.filename);
			krk_markObject((KrkObj*)function->globalsContext);
			markArray(&function->requiredArgNames);
			markArray(&function->keywordArgNames);
			markArray(&function->chunk.constants);
			for (size_t i = 0; i < function->localNameCount; ++i) {
				krk_markObject((KrkObj*)function->localNames[i].name);
			}
			break;
		}
		case OBJ_UPVALUE:
			krk_markValue(((KrkUpvalue*)object)->closed);
			break;
		case OBJ_CLASS: {
			KrkClass * _class = (KrkClass *)object;
			krk_markObject((KrkObj*)_class->name);
			krk_markObject((KrkObj*)_class->filename);
			krk_markObject((KrkObj*)_class->docstring);
			krk_markObject((KrkObj*)_class->base);
			krk_markTable(&_class->methods);
			krk_markTable(&_class->fields);
			break;
		}
		case OBJ_INSTANCE: {
			krk_markObject((KrkObj*)((KrkInstance*)object)->_class);
			if (((KrkInstance*)object)->_class->_ongcscan) ((KrkInstance*)object)->_class->_ongcscan((KrkInstance*)object);
			krk_markTable(&((KrkInstance*)object)->fields);
			break;
		}
		case OBJ_BOUND_METHOD: {
			KrkBoundMethod * bound = (KrkBoundMethod *)object;
			krk_markValue(bound->receiver);
			krk_markObject((KrkObj*)bound->method);
			break;
		}
		case OBJ_TUPLE: {
			KrkTuple * tuple = (KrkTuple *)object;
			markArray(&tuple->values);
			break;
		}
		case OBJ_PROPERTY: {
			KrkProperty * property = (KrkProperty *)object;
			krk_markValue(property->method);
			break;
		}
		case OBJ_NATIVE:
		case OBJ_STRING:
		case OBJ_BYTES:
			break;
	}
}

static void traceReferences() {
	while (vm.grayCount > 0) {
		KrkObj * object = vm.grayStack[--vm.grayCount];
		blackenObject(object);
	}
}

static size_t sweep() {
	KrkObj * previous = NULL;
	KrkObj * object = vm.objects;
	size_t count = 0;
	while (object) {
		if (object->isMarked || object->isProtected) {
			object->isMarked = 0;
			object->isProtected = 0;
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
			count++;
		}
	}
	return count;
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

static void markThreadRoots(KrkThreadState * thread) {
	for (KrkValue * slot = thread->stack; slot && slot < thread->stackTop; ++slot) {
		krk_markValue(*slot);
	}
	for (KrkUpvalue * upvalue = thread->openUpvalues; upvalue; upvalue = upvalue->next) {
		krk_markObject((KrkObj*)upvalue);
	}
	krk_markValue(thread->currentException);

	if (thread->module)  krk_markObject((KrkObj*)thread->module);
}

static void markRoots() {
	/* TODO all threads */
	KrkThreadState * thread = vm.threads;
	while (thread) {
		markThreadRoots(thread);
		thread = thread->next;
	}

	krk_markCompilerRoots();

	krk_markObject((KrkObj*)vm.builtins);
	krk_markTable(&vm.modules);

	if (vm.specialMethodNames) {
		for (int i = 0; i < METHOD__MAX; ++i) {
			krk_markValue(vm.specialMethodNames[i]);
		}
	}
}

size_t krk_collectGarbage(void) {
	markRoots();
	traceReferences();
	krk_tableRemoveWhite(&vm.strings);
	size_t out = sweep();
	vm.nextGC = vm.bytesAllocated * 2;
	return out;
}

static KrkValue krk_collectGarbage_wrapper(int argc, KrkValue argv[]) {
	if (&krk_currentThread != vm.threads) return krk_runtimeError(vm.exceptions->valueError, "only the main thread can do that");
	return INTEGER_VAL(krk_collectGarbage());
}

_noexport
void _createAndBind_gcMod(void) {
	/**
	 * gc = module()
	 *
	 * Namespace for methods for controlling the garbage collector.
	 */
	KrkInstance * gcModule = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "gc", (KrkObj*)gcModule);
	krk_attachNamedObject(&gcModule->fields, "__name__", (KrkObj*)S("gc"));
	krk_attachNamedValue(&gcModule->fields, "__file__", NONE_VAL());
	krk_defineNative(&gcModule->fields, "collect", krk_collectGarbage_wrapper);
	krk_attachNamedObject(&gcModule->fields, "__doc__",
		(KrkObj*)S("Namespace containing methods for controlling the garbge collector."));
}
