#include <kuroko/vm.h>
#include <kuroko/memory.h>
#include <kuroko/object.h>
#include <kuroko/compiler.h>
#include <kuroko/table.h>
#include <kuroko/util.h>

#include "private.h"

#define FREE_OBJECT(t,p) krk_reallocate(p,sizeof(t),0)

#if defined(KRK_EXTENSIVE_MEMORY_DEBUGGING)
/**
 * Extensive Memory Debugging
 *
 * When memory is allocated for Kuroko objects, it should be tracked.
 * This tracking allows us to schedule garbage collection at reasonable
 * periods based on memory load. In order for this to work correctly,
 * allocation and deallocation, as well resizing, must correctly track
 * the sizes of objects by both using the appropriate macros and by
 * ensuring the right sizes are passed to those macros. This is a very
 * easy thing to get wrong - allocate with @c malloc but free with the
 * @c KRK_FREE_ARRAY macros, for example, and the memory tracking now has
 * a net negative, which may lead to underflowing. Use the right macros,
 * but mix up sizes between allocation and deallocation, and we may have
 * a "leak" of bytes and garbage collection may happen more often than
 * needs to. Because sizes are generally stored somewhere in an object,
 * but that 'somewhere' can be different between objects, and some objects
 * store more than one size, it can be difficult to automatically track
 * the right sizes. We could add it to the actual allocation, or try to
 * steal the information from the underlying system allocator, but both
 * of these have their respective issues.
 *
 * Enter the extensive memory debugger, which is just a glorified hash
 * table of allocated pointers to their sizes. Every time we perform
 * an allocation, we store the right size in the hash. This hash table
 * is based on the chaining hashtable from ToaruOS, simplified for our
 * use case, and it does not use the allocation macros, so it doesn't
 * have the bootstrapping problem using @c KrkTable would.
 *
 * When we resize an allocation, including freeing (which is just resizing
 * to 0), we can check that the "old" size passed to @c krk_reallocate
 * matches what is in our hash. If it doesn't, we can abort, and in so
 * doing acquire a stack trace that tells us exactly who screwed up.
 *
 * This hash does have some overhead, so enabling it is only intended
 * for debugging. It's also not very flexible - we use a fixed-size
 * chaining hash, so we only have so many slots, and those slots are
 * not keyed well for pointers.
 *
 * @warning The extensive memory debugger is not thread safe.
 */
typedef struct DHE {
	const void* ptr;
	size_t size;
	struct DHE * next;
} _dhe;

/**
 * Use a power of two so we can make our hash
 * just do quick bitmasking
 */
#define DHE_SIZE 256
static struct DHE * _debug_mem[DHE_SIZE];

static inline unsigned int _debug_mem_hash(const void * ptr) {
	/* Pointers to objects are very often 16-byte aligned, and most
	 * allocations are of objects, so it would make sense to ignore
	 * the lower bits or we'll end up with a pretty bad hash. */
	uintptr_t p = (uintptr_t)ptr;
	return (p >> 4) & (DHE_SIZE-1);
}

static void _debug_mem_set(const void* ptr, size_t size) {
	unsigned int hash = _debug_mem_hash(ptr);
	_dhe * x = _debug_mem[hash];
	if (!x) {
		x = malloc(sizeof(_dhe));
		x->ptr = ptr;
		x->size = size;
		x->next = NULL;
		_debug_mem[hash] = x;
	} else {
		_dhe * p = NULL;
		do {
			if (x->ptr == ptr) {
				x->size = size;
				return;
			} else {
				p = x;
				x = x->next;
			}
		} while (x);
		x = malloc(sizeof(_dhe));
		x->ptr = ptr;
		x->size = size;
		x->next = NULL;
		p->next = x;
	}
}

static size_t _debug_mem_get(const void * ptr) {
	unsigned int hash = _debug_mem_hash(ptr);
	_dhe * x = _debug_mem[hash];
	if (!x) return 0;
	do {
		if (x->ptr == ptr) return x->size;
		x = x->next;
	} while (x);
	return 0;
}

static void _debug_mem_remove(const void *ptr) {
	unsigned int hash = _debug_mem_hash(ptr);
	_dhe * x = _debug_mem[hash];
	if (!x) return;

	if (x->ptr == ptr) {
		_debug_mem[hash] = x->next;
		free(x);
	} else {
		_dhe * p = x;
		x = x->next;
		do {
			if (x->ptr == ptr) {
				p->next = x->next;
				free(x);
				return;
			}
			p = x;
			x = x->next;
		} while (x);
	}
}

static int _debug_mem_has(const void *ptr) {
	unsigned int hash = _debug_mem_hash(ptr);
	_dhe * x = _debug_mem[hash];
	if (!x) return 0;
	do {
		if (x->ptr == ptr) return 1;
		x = x->next;
	} while (x);
	return 0;
}
#endif

void krk_gcTakeBytes(const void * ptr, size_t size) {
#if defined(KRK_EXTENSIVE_MEMORY_DEBUGGING)
	_debug_mem_set(ptr, size);
#endif

	vm.bytesAllocated += size;
}

void * krk_reallocate(void * ptr, size_t old, size_t new) {

	vm.bytesAllocated -= old;
	vm.bytesAllocated += new;

	if (new > old && ptr != krk_currentThread.stack && &krk_currentThread == vm.threads && !(vm.globalFlags & KRK_GLOBAL_GC_PAUSED)) {
#ifndef KRK_NO_STRESS_GC
		if (vm.globalFlags & KRK_GLOBAL_ENABLE_STRESS_GC) {
			krk_collectGarbage();
		}
#endif
		if (vm.bytesAllocated > vm.nextGC) {
			krk_collectGarbage();
		}
	}

	void * out;
	if (new == 0) {
		free(ptr);
		out = NULL;
	} else {
		out = realloc(ptr, new);
	}

#if defined(KRK_EXTENSIVE_MEMORY_DEBUGGING)
	if (ptr) {
		if (!_debug_mem_has(ptr)) {
			fprintf(stderr, "Invalid reallocation of %p from %zu to %zu\n", ptr, old, new);
			abort();
		}

		size_t t = _debug_mem_get(ptr);
		if (t != old) {
			fprintf(stderr, "Invalid reallocation of %p from %zu - should be %zu - to %zu\n", ptr, old, t, new);
			abort();
		}

		_debug_mem_remove(ptr);
	}
	if (out) {
		_debug_mem_set(out, new);
	}
#endif

	return out;
}

static void freeObject(KrkObj * object) {
	switch (object->type) {
		case KRK_OBJ_STRING: {
			KrkString * string = (KrkString*)object;
			KRK_FREE_ARRAY(char, string->chars, string->length + 1);
			if (string->codes && string->codes != string->chars) free(string->codes);
			FREE_OBJECT(KrkString, object);
			break;
		}
		case KRK_OBJ_CODEOBJECT: {
			KrkCodeObject * function = (KrkCodeObject*)object;
			krk_freeChunk(&function->chunk);
			krk_freeValueArray(&function->positionalArgNames);
			krk_freeValueArray(&function->keywordArgNames);
			KRK_FREE_ARRAY(KrkLocalEntry, function->localNames, function->localNameCount);
			KRK_FREE_ARRAY(KrkExpressionsMap, function->expressions, function->expressionsCapacity);
			function->localNameCount = 0;
			FREE_OBJECT(KrkCodeObject, object);
			break;
		}
		case KRK_OBJ_NATIVE: {
			FREE_OBJECT(KrkNative, object);
			break;
		}
		case KRK_OBJ_CLOSURE: {
			KrkClosure * closure = (KrkClosure*)object;
			KRK_FREE_ARRAY(KrkUpvalue*,closure->upvalues,closure->upvalueCount);
			krk_freeTable(&closure->fields);
			FREE_OBJECT(KrkClosure, object);
			break;
		}
		case KRK_OBJ_UPVALUE: {
			FREE_OBJECT(KrkUpvalue, object);
			break;
		}
		case KRK_OBJ_CLASS: {
			KrkClass * _class = (KrkClass*)object;
			krk_freeTable(&_class->methods);
			krk_freeTable(&_class->subclasses);
			if (_class->base) {
				krk_tableDeleteExact(&_class->base->subclasses, OBJECT_VAL(object));
			}
			FREE_OBJECT(KrkClass, object);
			break;
		}
		case KRK_OBJ_INSTANCE: {
			KrkInstance * inst = (KrkInstance*)object;
			if (inst->_class->_ongcsweep) {
				inst->_class->_ongcsweep(inst);
			}
			krk_freeTable(&inst->fields);
			krk_reallocate(object,inst->_class->allocSize,0);
			break;
		}
		case KRK_OBJ_BOUND_METHOD:
			FREE_OBJECT(KrkBoundMethod, object);
			break;
		case KRK_OBJ_TUPLE: {
			KrkTuple * tuple = (KrkTuple*)object;
			krk_freeValueArray(&tuple->values);
			FREE_OBJECT(KrkTuple, object);
			break;
		}
		case KRK_OBJ_BYTES: {
			KrkBytes * bytes = (KrkBytes*)object;
			KRK_FREE_ARRAY(uint8_t, bytes->bytes, bytes->length);
			FREE_OBJECT(KrkBytes, bytes);
			break;
		}
	}
}

void krk_freeObjects(void) {
	KrkObj * object = vm.objects;
	KrkObj * other = NULL;

	while (object) {
		KrkObj * next = object->next;
		if (object->type == KRK_OBJ_INSTANCE) {
			freeObject(object);
		} else {
			object->next = other;
			other = object;
		}
		object = next;
	}

	while (other) {
		KrkObj * next = other->next;
		if (other->type == KRK_OBJ_CLASS) {
			((KrkClass*)other)->base = NULL;
		}
		freeObject(other);
		other = next;
	}

	free(vm.grayStack);
}

void krk_freeMemoryDebugger(void) {
#if defined(KRK_EXTENSIVE_MEMORY_DEBUGGING)
	for (unsigned int i = 0; i < DHE_SIZE; ++i) {
		_dhe * x = _debug_mem[i];
		_debug_mem[i] = NULL;
		while (x) {
			_dhe * n = x->next;
			free(x);
			x = n;
		}
	}
	if (vm.bytesAllocated != 0) {
		abort();
	}
#endif
}

void krk_markObject(KrkObj * object) {
	if (!object) return;
	if (object->flags & KRK_OBJ_FLAGS_IS_MARKED) return;
	object->flags |= KRK_OBJ_FLAGS_IS_MARKED;

	if (vm.grayCapacity < vm.grayCount + 1) {
		vm.grayCapacity = KRK_GROW_CAPACITY(vm.grayCapacity);
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
		case KRK_OBJ_CLOSURE: {
			KrkClosure * closure = (KrkClosure *)object;
			krk_markObject((KrkObj*)closure->function);
			for (size_t i = 0; i < closure->upvalueCount; ++i) {
				krk_markObject((KrkObj*)closure->upvalues[i]);
			}
			krk_markValue(closure->annotations);
			krk_markTable(&closure->fields);
			krk_markValue(closure->globalsOwner);
			break;
		}
		case KRK_OBJ_CODEOBJECT: {
			KrkCodeObject * function = (KrkCodeObject *)object;
			krk_markObject((KrkObj*)function->name);
			krk_markObject((KrkObj*)function->qualname);
			krk_markObject((KrkObj*)function->docstring);
			krk_markObject((KrkObj*)function->chunk.filename);
			markArray(&function->positionalArgNames);
			markArray(&function->keywordArgNames);
			markArray(&function->chunk.constants);
			for (size_t i = 0; i < function->localNameCount; ++i) {
				krk_markObject((KrkObj*)function->localNames[i].name);
			}
			break;
		}
		case KRK_OBJ_UPVALUE:
			krk_markValue(((KrkUpvalue*)object)->closed);
			break;
		case KRK_OBJ_CLASS: {
			KrkClass * _class = (KrkClass *)object;
			krk_markObject((KrkObj*)_class->name);
			krk_markObject((KrkObj*)_class->filename);
			krk_markObject((KrkObj*)_class->base);
			krk_markObject((KrkObj*)_class->_class);
			krk_markTable(&_class->methods);
			break;
		}
		case KRK_OBJ_INSTANCE: {
			krk_markObject((KrkObj*)((KrkInstance*)object)->_class);
			if (((KrkInstance*)object)->_class->_ongcscan) ((KrkInstance*)object)->_class->_ongcscan((KrkInstance*)object);
			krk_markTable(&((KrkInstance*)object)->fields);
			break;
		}
		case KRK_OBJ_BOUND_METHOD: {
			KrkBoundMethod * bound = (KrkBoundMethod *)object;
			krk_markValue(bound->receiver);
			krk_markObject((KrkObj*)bound->method);
			break;
		}
		case KRK_OBJ_TUPLE: {
			KrkTuple * tuple = (KrkTuple *)object;
			markArray(&tuple->values);
			break;
		}
		case KRK_OBJ_NATIVE:
		case KRK_OBJ_STRING:
		case KRK_OBJ_BYTES:
			break;
	}
}

static void traceReferences(void) {
	while (vm.grayCount > 0) {
		KrkObj * object = vm.grayStack[--vm.grayCount];
		blackenObject(object);
	}
}

static size_t sweep(void) {
	KrkObj * previous = NULL;
	KrkObj * object = vm.objects;
	size_t count = 0;
	while (object) {
		if (object->flags & (KRK_OBJ_FLAGS_IMMORTAL | KRK_OBJ_FLAGS_IS_MARKED)) {
			object->flags &= ~(KRK_OBJ_FLAGS_IS_MARKED | KRK_OBJ_FLAGS_SECOND_CHANCE);
			previous = object;
			object = object->next;
		} else if (object->flags & KRK_OBJ_FLAGS_SECOND_CHANCE) {
			KrkObj * unreached = object;
			object = object->next;
			if (previous != NULL) {
				previous->next = object;
			} else {
				vm.objects = object;
			}
			freeObject(unreached);
			count++;
		} else {
			object->flags |= KRK_OBJ_FLAGS_SECOND_CHANCE;
			previous = object;
			object = object->next;
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

static void tableRemoveWhite(KrkTable * table) {
	for (size_t i = 0; i < table->capacity; ++i) {
		KrkTableEntry * entry = &table->entries[i];
		if (IS_OBJECT(entry->key) && !((AS_OBJECT(entry->key))->flags & KRK_OBJ_FLAGS_IS_MARKED)) {
			krk_tableDeleteExact(table, entry->key);
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

	for (int i = 0; i < KRK_THREAD_SCRATCH_SIZE; ++i) {
		krk_markValue(thread->scratchSpace[i]);
	}
}

static void markRoots(void) {
	KrkThreadState * thread = vm.threads;
	while (thread) {
		markThreadRoots(thread);
		thread = thread->next;
	}

	krk_markObject((KrkObj*)vm.builtins);
	krk_markTable(&vm.modules);

	if (vm.specialMethodNames) {
		for (int i = 0; i < METHOD__MAX; ++i) {
			krk_markValue(vm.specialMethodNames[i]);
		}
	}
}

#ifndef KRK_NO_GC_TRACING
static int smartSize(char _out[100], size_t s) {
#if UINTPTR_MAX == 0xFFFFFFFF
	size_t count = 3;
	char * prefix = "GMK";
#else
	size_t count = 5;
	char * prefix = "PTGMK";
#endif
	for (; count > 0 && *prefix; count--, prefix++) {
		size_t base = 1UL << (count * 10);
		if (s >= base) {
			size_t t = s / base;
			return snprintf(_out, 100, "%zu.%1zu %ciB", t, (s - t * base) / (base / 10), *prefix);
		}
	}
	return snprintf(_out, 100, "%d B", (int)s);
}
#endif

size_t krk_collectGarbage(void) {
#ifndef KRK_NO_GC_TRACING
	struct timespec outTime, inTime;

	if (vm.globalFlags & KRK_GLOBAL_REPORT_GC_COLLECTS) {
		clock_gettime(CLOCK_MONOTONIC, &inTime);
	}

	size_t bytesBefore = vm.bytesAllocated;
#endif

	markRoots();
	traceReferences();
	tableRemoveWhite(&vm.strings);
	size_t out = sweep();

	/**
	 * The GC scheduling is in need of some improvement. The strategy at the moment
	 * is to schedule the next collect at double the current post-collection byte
	 * allocation size, up until that reaches 128MiB (64*2). Beyond that point,
	 * the next collection is scheduled for 64MiB after the current value.
	 *
	 * Previously, we always doubled as that was what Lox did, but this rather
	 * quickly runs into issues when memory allocation climbs into the GiB range.
	 * 64MiB seems to be a good switchover point.
	 */
	if (vm.bytesAllocated < 0x4000000) {
		vm.nextGC = vm.bytesAllocated * 2;
	} else {
		vm.nextGC = vm.bytesAllocated + 0x4000000;
	}

#ifndef KRK_NO_GC_TRACING
	if (vm.globalFlags & KRK_GLOBAL_REPORT_GC_COLLECTS) {
		clock_gettime(CLOCK_MONOTONIC, &outTime);
		struct timespec diff;
		diff.tv_sec  = outTime.tv_sec  - inTime.tv_sec;
		diff.tv_nsec = outTime.tv_nsec - inTime.tv_nsec;
		if (diff.tv_nsec < 0) { diff.tv_sec--; diff.tv_nsec += 1000000000L; }

		char smartBefore[100];
		smartSize(smartBefore, bytesBefore);
		char smartAfter[100];
		smartSize(smartAfter, vm.bytesAllocated);
		char smartFreed[100];
		smartSize(smartFreed, bytesBefore - vm.bytesAllocated);
		char smartNext[100];
		smartSize(smartNext, vm.nextGC);

		fprintf(stderr, "[gc] %lld.%.9lds %s before; %s after; freed %s in %llu objects; next collection at %s\n",
			(long long)diff.tv_sec, diff.tv_nsec,
			smartBefore,smartAfter,smartFreed,(unsigned long long)out, smartNext);
	}
#endif
	return out;
}

