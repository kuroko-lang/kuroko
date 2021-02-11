#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

static KrkValue _bytes_init(int argc, KrkValue argv[], int hasKw) {
	if (argc == 1) {
		return OBJECT_VAL(krk_newBytes(0,NULL));
	}

	if (IS_TUPLE(argv[1])) {
		KrkBytes * out = krk_newBytes(AS_TUPLE(argv[1])->values.count, NULL);
		krk_push(OBJECT_VAL(out));
		for (size_t i = 0; i < AS_TUPLE(argv[1])->values.count; ++i) {
			if (!IS_INTEGER(AS_TUPLE(argv[1])->values.values[i])) {
				return krk_runtimeError(vm.exceptions->typeError, "bytes(): expected tuple of ints, not of '%s'", krk_typeName(AS_TUPLE(argv[1])->values.values[i]));
			}
			out->bytes[i] = AS_INTEGER(AS_TUPLE(argv[1])->values.values[i]);
		}
		krk_bytesUpdateHash(out);
		return krk_pop();
	}

	return krk_runtimeError(vm.exceptions->typeError, "Can not convert '%s' to bytes", krk_typeName(argv[1]));
}

/* bytes objects are not interned; need to do this the old-fashioned way. */
static KrkValue _bytes_eq(int argc, KrkValue argv[], int hasKw) {
	if (!IS_BYTES(argv[1])) return BOOLEAN_VAL(0);
	KrkBytes * self = AS_BYTES(argv[0]);
	KrkBytes * them = AS_BYTES(argv[1]);
	if (self->length != them->length) return BOOLEAN_VAL(0);
	if (self->hash != them->hash) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->length; ++i) {
		if (self->bytes[i] != them->bytes[i]) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = GROW_ARRAY(char, stringBytes, old, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)
#define AT_END() (self->length == 0 || i == self->length - 1)

static KrkValue _bytes_repr(int argc, KrkValue argv[], int hasKw) {
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = NULL;

	PUSH_CHAR('b');
	PUSH_CHAR('\'');

	for (size_t i = 0; i < AS_BYTES(argv[0])->length; ++i) {
		uint8_t ch = AS_BYTES(argv[0])->bytes[i];
		switch (ch) {
			case '\\': PUSH_CHAR('\\'); PUSH_CHAR('\\'); break;
			case '\'': PUSH_CHAR('\\'); PUSH_CHAR('\''); break;
			case '\a': PUSH_CHAR('\\'); PUSH_CHAR('a'); break;
			case '\b': PUSH_CHAR('\\'); PUSH_CHAR('b'); break;
			case '\f': PUSH_CHAR('\\'); PUSH_CHAR('f'); break;
			case '\n': PUSH_CHAR('\\'); PUSH_CHAR('n'); break;
			case '\r': PUSH_CHAR('\\'); PUSH_CHAR('r'); break;
			case '\t': PUSH_CHAR('\\'); PUSH_CHAR('t'); break;
			case '\v': PUSH_CHAR('\\'); PUSH_CHAR('v'); break;
			default: {
				if (ch < ' ' || ch >= 0x7F) {
					PUSH_CHAR('\\');
					PUSH_CHAR('x');
					char hex[3];
					sprintf(hex,"%02x", ch);
					PUSH_CHAR(hex[0]);
					PUSH_CHAR(hex[1]);
				} else {
					PUSH_CHAR(ch);
				}
				break;
			}
		}
	}

	PUSH_CHAR('\'');

	KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
	return tmp;
}

static KrkValue _bytes_get(int argc, KrkValue argv[], int hasKw) {
	if (argc < 2) return krk_runtimeError(vm.exceptions->argumentError, "bytes.__get__(): expected one argument");
	KrkBytes * self = AS_BYTES(argv[0]);
	long asInt = AS_INTEGER(argv[1]);

	if (asInt < 0) asInt += (long)self->length;
	if (asInt < 0 || asInt >= (long)self->length) {
		return krk_runtimeError(vm.exceptions->indexError, "bytes index out of range: %ld", asInt);
	}

	return INTEGER_VAL(self->bytes[asInt]);
}

static KrkValue _bytes_len(int argc, KrkValue argv[], int hasKw) {
	return INTEGER_VAL(AS_BYTES(argv[0])->length);
}

static KrkValue _bytes_contains(int argc, KrkValue argv[], int hasKw) {
	if (argc < 2) krk_runtimeError(vm.exceptions->argumentError, "bytes.__contains__(): expected one argument");
	return krk_runtimeError(vm.exceptions->notImplementedError, "not implemented");
}

static KrkValue _bytes_decode(int argc, KrkValue argv[], int hasKw) {
	/* TODO: Actually bother checking if this explodes, or support other encodings... */
	return OBJECT_VAL(krk_copyString((char*)AS_BYTES(argv[0])->bytes, AS_BYTES(argv[0])->length));
}

#undef PUSH_CHAR

_noexport
void _createAndBind_bytesClass(void) {
	ADD_BASE_CLASS(vm.baseClasses->bytesClass, "bytes", vm.baseClasses->objectClass);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__init__",  _bytes_init);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__str__",  _bytes_repr);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__repr__", _bytes_repr);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".decode", _bytes_decode);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__len__", _bytes_len);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__contains__", _bytes_contains);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__get__", _bytes_get);
	krk_defineNative(&vm.baseClasses->bytesClass->methods, ".__eq__", _bytes_eq);
	krk_finalizeClass(vm.baseClasses->bytesClass);
}
