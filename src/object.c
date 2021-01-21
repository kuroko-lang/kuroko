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
	object->inRepr   = 0;
	object->next = vm.objects;
	vm.objects = object;
	return object;
}

size_t krk_codepointToBytes(krk_integer_type value, unsigned char * out) {
	if (value > 0xFFFF) {
		out[0] = (0xF0 | (value >> 18));
		out[1] = (0x80 | ((value >> 12) & 0x3F));
		out[2] = (0x80 | ((value >> 6) & 0x3F));
		out[3] = (0x80 | ((value) & 0x3F));
		return 4;
	} else if (value > 0x7FF) {
		out[0] = (0xE0 | (value >> 12));
		out[1] = (0x80 | ((value >> 6) & 0x3F));
		out[2] = (0x80 | (value & 0x3F));
		return 3;
	} else if (value > 0x7F) {
		out[0] = (0xC0 | (value >> 6));
		out[1] = (0x80 | (value & 0x3F));
		return 2;
	} else {
		out[0] = (unsigned char)value;
		return 1;
	}
}

#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static inline uint32_t decode(uint32_t* state, uint32_t* codep, uint32_t byte) {
	static int state_table[32] = {
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xxxxxxx */
		1,1,1,1,1,1,1,1,                 /* 10xxxxxx */
		2,2,2,2,                         /* 110xxxxx */
		3,3,                             /* 1110xxxx */
		4,                               /* 11110xxx */
		1                                /* 11111xxx */
	};

	static int mask_bytes[32] = {
		0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,
		0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x1F,0x1F,0x1F,0x1F,
		0x0F,0x0F,
		0x07,
		0x00
	};

	static int next[5] = {
		0,
		1,
		0,
		2,
		3
	};

	if (*state == UTF8_ACCEPT) {
		if (byte >= 0x80 && byte <= 0xC1) goto _reject;
		*codep = byte & mask_bytes[byte >> 3];
		*state = state_table[byte >> 3];
	} else if (*state > 0) {
		if (byte < 0x80 || byte >= 0xC0) goto _reject;
		*codep = (byte & 0x3F) | (*codep << 6);
		*state = next[*state];
	}
	return *state;
_reject:
	*state = UTF8_REJECT;
	return *state;
}

static int checkString(const char * chars, size_t length, size_t *codepointCount) {
	uint32_t state = 0;
	uint32_t codepoint = 0;
	unsigned char * end = (unsigned char *)chars + length;
	uint32_t maxCodepoint = 0;
	for (unsigned char * c = (unsigned char *)chars; c < end; ++c) {
		if (!decode(&state, &codepoint, *c)) {
			if (codepoint > maxCodepoint) maxCodepoint = codepoint;
			(*codepointCount)++;
		} else if (state == UTF8_REJECT) {
			krk_runtimeError(vm.exceptions.valueError, "Invalid UTF-8 sequence in string.");
			fprintf(stderr, "Invalid sequence detected.\n");
			*codepointCount = 0;
			return KRK_STRING_ASCII;
		}
	}
	if (maxCodepoint > 0xFFFF) {
		return KRK_STRING_UCS4;
	} else if (maxCodepoint > 0xFF) {
		return KRK_STRING_UCS2;
	} else if (maxCodepoint > 0x7F) {
		return KRK_STRING_UCS1;
	} else {
		return KRK_STRING_ASCII;
	}
}

#define GENREADY(size,type) \
	static void _readyUCS ## size (KrkString * string) { \
		uint32_t state = 0; \
		uint32_t codepoint = 0; \
		unsigned char * end = (unsigned char *)string->chars + string->length; \
		string->codes = malloc(sizeof(type) * string->codesLength); \
		type *outPtr = (type *)string->codes; \
		for (unsigned char * c = (unsigned char *)string->chars; c < end; ++c) { \
			if (!decode(&state, &codepoint, *c)) { \
				*(outPtr++) = (type)codepoint; \
			} else if (state == UTF8_REJECT) { \
				state = 0; \
			} \
		} \
	}
GENREADY(1,uint8_t)
GENREADY(2,uint16_t)
GENREADY(4,uint32_t)
#undef GENREADY

void * krk_unicodeString(KrkString * string) {
	if (string->codes) return string->codes;
	if (string->type == KRK_STRING_UCS1) _readyUCS1(string);
	else if (string->type == KRK_STRING_UCS2) _readyUCS2(string);
	else if (string->type == KRK_STRING_UCS4) _readyUCS4(string);
	else krk_runtimeError(vm.exceptions.valueError, "Internal string error.");
	return string->codes;
}

uint32_t krk_unicodeCodepoint(KrkString * string, size_t index) {
	krk_unicodeString(string);
	switch (string->type) {
		case KRK_STRING_ASCII: return string->chars[index];
		case KRK_STRING_UCS1: return ((uint8_t*)string->codes)[index];
		case KRK_STRING_UCS2: return ((uint16_t*)string->codes)[index];
		case KRK_STRING_UCS4: return ((uint32_t*)string->codes)[index];
	}
	krk_runtimeError(vm.exceptions.valueError, "Invalid string.");
	return 0;
}

static KrkString * allocateString(char * chars, size_t length, uint32_t hash) {
	KrkString * string = ALLOCATE_OBJECT(KrkString, OBJ_STRING);
	string->length = length;
	string->chars = chars;
	string->hash = hash;
	string->codesLength = 0;
	string->type = checkString(chars,length,&string->codesLength);
	string->codes = NULL;
	if (string->type == KRK_STRING_ASCII) string->codes = string->chars;
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

KrkFunction * krk_newFunction(void) {
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
	native->doc = NULL;
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
	_class->_len = NULL;
	_class->_enter = NULL;
	_class->_exit = NULL;
	_class->_delitem = NULL;
	_class->_iter = NULL;
	_class->_getattr = NULL;
	_class->_dir = NULL;

	return _class;
}

KrkInstance * krk_newInstance(KrkClass * _class) {
	KrkInstance * instance = ALLOCATE_OBJECT(KrkInstance, OBJ_INSTANCE);
	instance->_class = _class;
	krk_initTable(&instance->fields);
	if (_class) {
		krk_push(OBJECT_VAL(instance));
		krk_tableAddAll(&_class->fields, &instance->fields);
		krk_pop();
	}
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

void krk_bytesUpdateHash(KrkBytes * bytes) {
	bytes->hash = hashString((char*)bytes->bytes, bytes->length);
}

KrkBytes * krk_newBytes(size_t length, uint8_t * source) {
	KrkBytes * bytes = ALLOCATE_OBJECT(KrkBytes, OBJ_BYTES);
	bytes->length = length;
	bytes->bytes  = NULL;
	krk_push(OBJECT_VAL(bytes));
	bytes->bytes  = ALLOCATE(uint8_t, length);
	bytes->hash = -1;
	if (source) {
		memcpy(bytes->bytes, source, length);
		krk_bytesUpdateHash(bytes);
	}
	krk_pop();
	return bytes;
}
