#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <kuroko/memory.h>
#include <kuroko/object.h>
#include <kuroko/value.h>
#include <kuroko/vm.h>
#include <kuroko/table.h>

#define ALLOCATE_OBJECT(type, objectType) \
	(type*)allocateObject(sizeof(type), objectType)

#ifdef ENABLE_THREADING
static volatile int _stringLock = 0;
static volatile int _objectLock = 0;
#endif

static KrkObj * allocateObject(size_t size, KrkObjType type) {
	KrkObj * object = (KrkObj*)krk_reallocate(NULL, 0, size);
	memset(object,0,size);
	object->type = type;

	_obtain_lock(_objectLock);
	object->next = vm.objects;
	krk_currentThread.scratchSpace[2] = OBJECT_VAL(object);
	vm.objects = object;
	_release_lock(_objectLock);

	object->hash = (uint32_t)((intptr_t)(object) >> 4 | ((intptr_t)object & 0xf) << 28);

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
			_release_lock(_stringLock);
			krk_runtimeError(vm.exceptions->valueError, "Invalid UTF-8 sequence in string.");
			*codepointCount = 0;
			return -1;
		}
	}
	if (maxCodepoint > 0xFFFF) {
		return KRK_OBJ_FLAGS_STRING_UCS4;
	} else if (maxCodepoint > 0xFF) {
		return KRK_OBJ_FLAGS_STRING_UCS2;
	} else if (maxCodepoint > 0x7F) {
		return KRK_OBJ_FLAGS_STRING_UCS1;
	} else {
		return KRK_OBJ_FLAGS_STRING_ASCII;
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
	else if ((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS1) _readyUCS1(string);
	else if ((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS2) _readyUCS2(string);
	else if ((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS4) _readyUCS4(string);
	else krk_runtimeError(vm.exceptions->valueError, "Internal string error.");
	return string->codes;
}

uint32_t krk_unicodeCodepoint(KrkString * string, size_t index) {
	krk_unicodeString(string);
	switch (string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) {
		case KRK_OBJ_FLAGS_STRING_ASCII:
		case KRK_OBJ_FLAGS_STRING_UCS1: return ((uint8_t*)string->codes)[index];
		case KRK_OBJ_FLAGS_STRING_UCS2: return ((uint16_t*)string->codes)[index];
		case KRK_OBJ_FLAGS_STRING_UCS4: return ((uint32_t*)string->codes)[index];
		default:
			krk_runtimeError(vm.exceptions->valueError, "Internal string error.");
			return 0;
	}
}

static KrkString * allocateString(char * chars, size_t length, uint32_t hash) {
	size_t codesLength = 0;
	int type = checkString(chars,length,&codesLength);
	if (type == -1) {
		return krk_copyString("",0);
	}
	KrkString * string = ALLOCATE_OBJECT(KrkString, KRK_OBJ_STRING);
	string->length = length;
	string->chars = chars;
	string->obj.hash = hash;
	string->obj.flags |= KRK_OBJ_FLAGS_VALID_HASH | type;
	string->codesLength = codesLength;
	string->codes = NULL;
	if (type == KRK_OBJ_FLAGS_STRING_ASCII) string->codes = string->chars;
	krk_push(OBJECT_VAL(string));
	krk_tableSet(&vm.strings, OBJECT_VAL(string), NONE_VAL());
	krk_pop();
	_release_lock(_stringLock);
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
	_obtain_lock(_stringLock);
	KrkString * interned = krk_tableFindString(&vm.strings, chars, length, hash);
	if (interned != NULL) {
		free(chars); /* This string isn't owned by us yet, so free, not FREE_ARRAY */
		_release_lock(_stringLock);
		return interned;
	}

	/* Part of taking ownership of this string is that we track its memory usage */
	krk_gcTakeBytes(chars, length + 1);
	KrkString * result = allocateString(chars, length, hash);
	return result;
}

KrkString * krk_copyString(const char * chars, size_t length) {
	uint32_t hash = hashString(chars, length);
	_obtain_lock(_stringLock);
	KrkString * interned = krk_tableFindString(&vm.strings, chars ? chars : "", length, hash);
	if (interned) {
		_release_lock(_stringLock);
		return interned;
	}
	char * heapChars = ALLOCATE(char, length + 1);
	memcpy(heapChars, chars ? chars : "", length);
	heapChars[length] = '\0';
	KrkString * result = allocateString(heapChars, length, hash);
	if (result->chars != heapChars) free(heapChars);
	_release_lock(_stringLock);
	return result;
}

KrkString * krk_takeStringVetted(char * chars, size_t length, size_t codesLength, KrkStringType type, uint32_t hash) {
	_obtain_lock(_stringLock);
	KrkString * interned = krk_tableFindString(&vm.strings, chars, length, hash);
	if (interned != NULL) {
		FREE_ARRAY(char, chars, length + 1);
		_release_lock(_stringLock);
		return interned;
	}
	KrkString * string = ALLOCATE_OBJECT(KrkString, KRK_OBJ_STRING);
	string->length = length;
	string->chars = chars;
	string->obj.hash = hash;
	string->obj.flags |= KRK_OBJ_FLAGS_VALID_HASH | type;
	string->codesLength = codesLength;
	string->codes = NULL;
	if (type == KRK_OBJ_FLAGS_STRING_ASCII) string->codes = string->chars;
	krk_push(OBJECT_VAL(string));
	krk_tableSet(&vm.strings, OBJECT_VAL(string), NONE_VAL());
	krk_pop();
	_release_lock(_stringLock);
	return string;
}

KrkCodeObject * krk_newCodeObject(void) {
	KrkCodeObject * codeobject = ALLOCATE_OBJECT(KrkCodeObject, KRK_OBJ_CODEOBJECT);
	codeobject->requiredArgs = 0;
	codeobject->keywordArgs = 0;
	codeobject->upvalueCount = 0;
	codeobject->name = NULL;
	codeobject->docstring = NULL;
	codeobject->localNameCount = 0;
	codeobject->localNames = NULL;
	codeobject->globalsContext = NULL;
	krk_initValueArray(&codeobject->requiredArgNames);
	krk_initValueArray(&codeobject->keywordArgNames);
	krk_initChunk(&codeobject->chunk);
	return codeobject;
}

KrkNative * krk_newNative(NativeFn function, const char * name, int type) {
	KrkNative * native = ALLOCATE_OBJECT(KrkNative, KRK_OBJ_NATIVE);
	native->function = function;
	native->obj.flags = type;
	native->name = name;
	native->doc = NULL;
	return native;
}

KrkClosure * krk_newClosure(KrkCodeObject * function) {
	KrkUpvalue ** upvalues = ALLOCATE(KrkUpvalue*, function->upvalueCount);
	for (size_t i = 0; i < function->upvalueCount; ++i) {
		upvalues[i] = NULL;
	}
	KrkClosure * closure = ALLOCATE_OBJECT(KrkClosure, KRK_OBJ_CLOSURE);
	closure->function = function;
	closure->upvalues = upvalues;
	closure->upvalueCount = function->upvalueCount;
	closure->annotations = krk_dict_of(0,NULL,0);
	krk_initTable(&closure->fields);
	return closure;
}

KrkUpvalue * krk_newUpvalue(int slot) {
	KrkUpvalue * upvalue = ALLOCATE_OBJECT(KrkUpvalue, KRK_OBJ_UPVALUE);
	upvalue->location = slot;
	upvalue->next = NULL;
	upvalue->closed = NONE_VAL();
	upvalue->owner = &krk_currentThread;
	return upvalue;
}

KrkClass * krk_newClass(KrkString * name, KrkClass * baseClass) {
	KrkClass * _class = ALLOCATE_OBJECT(KrkClass, KRK_OBJ_CLASS);
	_class->name = name;
	_class->allocSize = sizeof(KrkInstance);
	krk_initTable(&_class->methods);
	krk_initTable(&_class->subclasses);

	if (baseClass) {
		_class->base = baseClass;
		_class->allocSize = baseClass->allocSize;
		_class->_ongcscan = baseClass->_ongcscan;
		_class->_ongcsweep = baseClass->_ongcsweep;

		krk_tableSet(&baseClass->subclasses, OBJECT_VAL(_class), NONE_VAL());
	}

	return _class;
}

KrkInstance * krk_newInstance(KrkClass * _class) {
	size_t allocSize = _class->allocSize ? _class->allocSize : sizeof(KrkInstance);
	KrkInstance * instance = (KrkInstance*)allocateObject(allocSize, KRK_OBJ_INSTANCE);
	instance->_class = _class;
	krk_initTable(&instance->fields);
	return instance;
}

KrkBoundMethod * krk_newBoundMethod(KrkValue receiver, KrkObj * method) {
	KrkBoundMethod * bound = ALLOCATE_OBJECT(KrkBoundMethod, KRK_OBJ_BOUND_METHOD);
	bound->receiver = receiver;
	bound->method = method;
	return bound;
}

KrkTuple * krk_newTuple(size_t length) {
	KrkTuple * tuple = ALLOCATE_OBJECT(KrkTuple, KRK_OBJ_TUPLE);
	krk_initValueArray(&tuple->values);
	krk_push(OBJECT_VAL(tuple));
	tuple->values.capacity = length;
	tuple->values.values = GROW_ARRAY(KrkValue,NULL,0,length);
	krk_pop();
	return tuple;
}

KrkBytes * krk_newBytes(size_t length, uint8_t * source) {
	KrkBytes * bytes = ALLOCATE_OBJECT(KrkBytes, KRK_OBJ_BYTES);
	bytes->length = length;
	bytes->bytes  = NULL;
	krk_push(OBJECT_VAL(bytes));
	bytes->bytes  = ALLOCATE(uint8_t, length);
	bytes->obj.hash = -1;
	if (source) {
		memcpy(bytes->bytes, source, length);
	}
	krk_pop();
	return bytes;
}

