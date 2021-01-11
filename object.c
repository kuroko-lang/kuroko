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
	object->isMarked = 0;
	object->next = vm.objects;
	vm.objects = object;
	return object;
}

static KrkString * allocateString(char * chars, size_t length, uint32_t hash) {
	KrkString * string = ALLOCATE_OBJECT(KrkString, OBJ_STRING);
	string->length = length;
	string->chars = chars;
	string->hash = hash;
	krk_push(OBJECT_VAL(string));
	krk_tableSet(&vm.strings, OBJECT_VAL(string), NONE_VAL());
	krk_pop();
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

KrkString * krk_takeString(char * chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	KrkString * interned = krk_tableFindString(&vm.strings, chars, length, hash);
	if (interned != NULL) {
		FREE_ARRAY(char, chars, length + 1);
		return interned;
	}
	return allocateString(chars, length, hash);
}

KrkString * krk_copyString(const char * chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	KrkString * interned = krk_tableFindString(&vm.strings, chars, length, hash);
	if (interned) return interned;
	char * heapChars = ALLOCATE(char, length + 1);
	memcpy(heapChars, chars, length);
	heapChars[length] = '\0';
	return allocateString(heapChars, length, hash);
}

KrkFunction * krk_newFunction() {
	KrkFunction * function = ALLOCATE_OBJECT(KrkFunction, OBJ_FUNCTION);
	function->requiredArgs = 0;
	function->keywordArgs = 0;
	function->upvalueCount = 0;
	function->name = NULL;
	function->docstring = NULL;
	function->collectsArguments = 0;
	function->collectsKeywords = 0;
	function->localNameCount = 0;
	function->localNames = NULL;
	function->globalsContext = NULL;
	krk_initValueArray(&function->requiredArgNames);
	krk_initValueArray(&function->keywordArgNames);
	krk_initChunk(&function->chunk);
	return function;
}

KrkNative * krk_newNative(NativeFn function, const char * name, int type) {
	KrkNative * native = ALLOCATE_OBJECT(KrkNative, OBJ_NATIVE);
	native->function = function;
	native->isMethod = type;
	native->name = name;
	return native;
}

KrkClosure * krk_newClosure(KrkFunction * function) {
	KrkUpvalue ** upvalues = ALLOCATE(KrkUpvalue*, function->upvalueCount);
	for (size_t i = 0; i < function->upvalueCount; ++i) {
		upvalues[i] = NULL;
	}
	KrkClosure * closure = ALLOCATE_OBJECT(KrkClosure, OBJ_CLOSURE);
	closure->function = function;
	closure->upvalues = upvalues;
	closure->upvalueCount = function->upvalueCount;
	return closure;
}

KrkUpvalue * krk_newUpvalue(int slot) {
	KrkUpvalue * upvalue = ALLOCATE_OBJECT(KrkUpvalue, OBJ_UPVALUE);
	upvalue->location = slot;
	upvalue->next = NULL;
	upvalue->closed = NONE_VAL();
	return upvalue;
}

KrkClass * krk_newClass(KrkString * name) {
	KrkClass * _class = ALLOCATE_OBJECT(KrkClass, OBJ_CLASS);
	_class->name = name;
	_class->filename = NULL;
	_class->docstring = NULL;
	_class->base = NULL;
	krk_initTable(&_class->methods);
	krk_initTable(&_class->fields);

	_class->_getter = NULL;
	_class->_setter = NULL;
	_class->_slicer = NULL;
	_class->_reprer = NULL;
	_class->_tostr = NULL;
	_class->_call = NULL;
	_class->_init = NULL;
	_class->_eq = NULL;

	return _class;
}

KrkInstance * krk_newInstance(KrkClass * _class) {
	KrkInstance * instance = ALLOCATE_OBJECT(KrkInstance, OBJ_INSTANCE);
	instance->_class = _class;
	krk_initTable(&instance->fields);
	krk_push(OBJECT_VAL(instance));
	krk_tableAddAll(&_class->fields, &instance->fields);
	krk_pop();
	instance->_internal = NULL; /* To be used by C-defined types to track internal objects. */
	return instance;
}

KrkBoundMethod * krk_newBoundMethod(KrkValue receiver, KrkObj * method) {
	KrkBoundMethod * bound = ALLOCATE_OBJECT(KrkBoundMethod, OBJ_BOUND_METHOD);
	bound->receiver = receiver;
	bound->method = method;
	return bound;
}

KrkTuple * krk_newTuple(size_t length) {
	KrkTuple * tuple = ALLOCATE_OBJECT(KrkTuple, OBJ_TUPLE);
	tuple->inrepr = 0;
	krk_initValueArray(&tuple->values);
	krk_push(OBJECT_VAL(tuple));
	tuple->values.capacity = length;
	tuple->values.values = GROW_ARRAY(KrkValue,NULL,0,length);
	krk_pop();
	return tuple;
}
