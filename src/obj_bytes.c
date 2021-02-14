#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define AS_bytes(o) AS_BYTES(o)
#define CURRENT_CTYPE KrkBytes *
#define CURRENT_NAME  self

#define IS_bytes(o) (IS_BYTES(o) || krk_isInstanceOf(o, vm.baseClasses->bytesClass))
KRK_METHOD(bytes,__init__,{
	if (argc < 2) {
		return OBJECT_VAL(krk_newBytes(0,NULL));
	}
	METHOD_TAKES_AT_MOST(1);

	/* TODO: Use generic unpacker */
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
	} else if (IS_list(argv[1])) {
		KrkBytes * out = krk_newBytes(AS_LIST(argv[1])->count, NULL);
		krk_push(OBJECT_VAL(out));
		for (size_t i = 0; i < AS_LIST(argv[1])->count; ++i) {
			if (!IS_INTEGER(AS_LIST(argv[1])->values[i])) {
				return krk_runtimeError(vm.exceptions->typeError, "bytes(): expected list of ints, not of '%s'", krk_typeName(AS_LIST(argv[1])->values[i]));
			}
			out->bytes[i] = AS_INTEGER(AS_LIST(argv[1])->values[i]);
		}
		krk_bytesUpdateHash(out);
		return krk_pop();
	}

	return krk_runtimeError(vm.exceptions->typeError, "Can not convert '%s' to bytes", krk_typeName(argv[1]));
})

#undef IS_bytes
#define IS_bytes(o) IS_BYTES(o)

/* bytes objects are not interned; need to do this the old-fashioned way. */
KRK_METHOD(bytes,__eq__,{
	if (!IS_BYTES(argv[1])) return BOOLEAN_VAL(0);
	KrkBytes * self = AS_BYTES(argv[0]);
	KrkBytes * them = AS_BYTES(argv[1]);
	if (self->length != them->length) return BOOLEAN_VAL(0);
	if (self->hash != them->hash) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->length; ++i) {
		if (self->bytes[i] != them->bytes[i]) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
})

#define AT_END() (self->length == 0 || i == self->length - 1)

KRK_METHOD(bytes,__repr__,{
	struct StringBuilder sb = {0};

	pushStringBuilder(&sb, 'b');
	pushStringBuilder(&sb, '\'');

	for (size_t i = 0; i < AS_BYTES(argv[0])->length; ++i) {
		uint8_t ch = AS_BYTES(argv[0])->bytes[i];
		switch (ch) {
			case '\\': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, '\\'); break;
			case '\'': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, '\''); break;
			case '\a': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 'a'); break;
			case '\b': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 'b'); break;
			case '\f': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 'f'); break;
			case '\n': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 'n'); break;
			case '\r': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 'r'); break;
			case '\t': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 't'); break;
			case '\v': pushStringBuilder(&sb, '\\'); pushStringBuilder(&sb, 'v'); break;
			default: {
				if (ch < ' ' || ch >= 0x7F) {
					pushStringBuilder(&sb, '\\');
					pushStringBuilder(&sb, 'x');
					char hex[3];
					sprintf(hex,"%02x", ch);
					pushStringBuilder(&sb, hex[0]);
					pushStringBuilder(&sb, hex[1]);
				} else {
					pushStringBuilder(&sb, ch);
				}
				break;
			}
		}
	}

	pushStringBuilder(&sb, '\'');

	return finishStringBuilder(&sb);
})

KRK_METHOD(bytes,__get__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,asInt);

	if (asInt < 0) asInt += (long)self->length;
	if (asInt < 0 || asInt >= (long)self->length) {
		return krk_runtimeError(vm.exceptions->indexError, "bytes index out of range: %ld", asInt);
	}

	return INTEGER_VAL(self->bytes[asInt]);
})

KRK_METHOD(bytes,__len__,{
	return INTEGER_VAL(AS_BYTES(argv[0])->length);
})

KRK_METHOD(bytes,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	return krk_runtimeError(vm.exceptions->notImplementedError, "not implemented");
})

KRK_METHOD(bytes,decode,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(krk_copyString((char*)AS_BYTES(argv[0])->bytes, AS_BYTES(argv[0])->length));
})

KRK_METHOD(bytes,join,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,list,KrkList*,iterable);

	const char * errorStr = NULL;
	struct StringBuilder sb = {0};

	for (size_t i = 0; i < iterable->values.count; ++i) {
		KrkValue value = iterable->values.values[i];
		if (!IS_BYTES(iterable->values.values[i])) {
			errorStr = krk_typeName(value);
			goto _expectedBytes;
		}
		krk_push(value);
		if (i > 0) {
			for (size_t j = 0; j < self->length; ++j) {
				pushStringBuilder(&sb, self->bytes[j]);
			}
		}
		for (size_t j = 0; j < AS_STRING(value)->length; ++j) {
			pushStringBuilder(&sb, AS_BYTES(value)->bytes[j]);
		}
		krk_pop();
	}

	return finishStringBuilderBytes(&sb);

_expectedBytes:
	krk_runtimeError(vm.exceptions->typeError, "Expected bytes, got %s.", errorStr);
	discardStringBuilder(&sb);
})

KRK_METHOD(bytes,__add__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,bytes,KrkBytes*,them);

	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, (char*)self->bytes, self->length);
	pushStringBuilderStr(&sb, (char*)them->bytes, them->length);

	return finishStringBuilderBytes(&sb);
})

_noexport
void _createAndBind_bytesClass(void) {
	KrkClass * bytes = ADD_BASE_CLASS(vm.baseClasses->bytesClass, "bytes", vm.baseClasses->objectClass);
	BIND_METHOD(bytes,__init__);
	BIND_METHOD(bytes,__repr__);
	BIND_METHOD(bytes,__len__);
	BIND_METHOD(bytes,__contains__);
	BIND_METHOD(bytes,__get__);
	BIND_METHOD(bytes,__eq__);
	BIND_METHOD(bytes,__add__);
	BIND_METHOD(bytes,decode);
	BIND_METHOD(bytes,join);
	krk_defineNative(&bytes->methods,".__str__",FUNC_NAME(bytes,__repr__)); /* alias */
	krk_finalizeClass(bytes);
}
