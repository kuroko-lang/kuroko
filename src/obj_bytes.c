#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#include "private.h"

struct ByteArray {
	KrkInstance inst;
	KrkValue actual;
};

#define AS_bytes(o) AS_BYTES(o)
#define CURRENT_CTYPE KrkBytes *
#define CURRENT_NAME  self

#undef IS_bytes
#define IS_bytes(o) (IS_BYTES(o) || krk_isInstanceOf(o, vm.baseClasses->bytesClass))

static int _bytes_callback(void * context, const KrkValue * values, size_t count) {
	struct StringBuilder * sb = context;
	for (size_t i = 0; i < count; ++i) {
		if (!IS_INTEGER(values[i])) {
			krk_runtimeError(vm.exceptions->typeError, "'%T' is not an integer", values[i]);
			return 1;
		}
		if (AS_INTEGER(values[i]) < 0 || AS_INTEGER(values[i]) > 255) {
			krk_runtimeError(vm.exceptions->typeError, "bytes object must be in range(0, 256)");
			return 1;
		}
		pushStringBuilder(sb, AS_INTEGER(values[i]));
	}
	return 0;
}

KRK_StaticMethod(bytes,__new__) {
	if (argc < 2) return OBJECT_VAL(krk_newBytes(0,NULL));
	METHOD_TAKES_AT_MOST(1);

	if (IS_bytearray(argv[1])) {
		return OBJECT_VAL(krk_newBytes(
			AS_BYTES(AS_bytearray(argv[1])->actual)->length,
			AS_BYTES(AS_bytearray(argv[1])->actual)->bytes));
	} else if (IS_STRING(argv[1])) {
		return OBJECT_VAL(krk_newBytes(AS_STRING(argv[1])->length, (uint8_t*)AS_CSTRING(argv[1])));
	} else if (IS_INTEGER(argv[1])) {
		if (AS_INTEGER(argv[1]) < 0) return krk_runtimeError(vm.exceptions->valueError, "negative count");
		krk_push(OBJECT_VAL(krk_newBytes(AS_INTEGER(argv[1]),NULL)));
		memset(AS_BYTES(krk_peek(0))->bytes, 0, AS_INTEGER(argv[1]));
		return krk_pop();
	} else {
		struct StringBuilder sb = {0};
		if (krk_unpackIterable(argv[1], &sb, _bytes_callback)) return NONE_VAL();
		return finishStringBuilderBytes(&sb);
	}
}

#undef IS_bytes
#define IS_bytes(o) IS_BYTES(o)

KRK_Method(bytes,__hash__) {
	METHOD_TAKES_NONE();
	uint32_t hash = 0;
	/* This is the so-called "sdbm" hash. It comes from a piece of
	 * public domain code from a clone of ndbm. */
	for (size_t i = 0; i < self->length; ++i) {
		krk_hash_advance(hash,self->bytes[i]);
	}
	return INTEGER_VAL(hash);
}

/* bytes objects are not interned; need to do this the old-fashioned way. */
KRK_Method(bytes,__eq__) {
	if (!IS_BYTES(argv[1])) return BOOLEAN_VAL(0);
	KrkBytes * them = AS_BYTES(argv[1]);
	if (self->length != them->length) return BOOLEAN_VAL(0);
	if (self->obj.hash != them->obj.hash) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->length; ++i) {
		if (self->bytes[i] != them->bytes[i]) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

#define AT_END() (self->length == 0 || i == self->length - 1)

KRK_Method(bytes,__repr__) {
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
					snprintf(hex,3,"%02x", ch);
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
}

KRK_Method(bytes,__getitem__) {
	METHOD_TAKES_EXACTLY(1);

	if (IS_INTEGER(argv[1])) {
		CHECK_ARG(1,int,krk_integer_type,asInt);

		if (asInt < 0) asInt += (long)self->length;
		if (asInt < 0 || asInt >= (long)self->length) {
			return krk_runtimeError(vm.exceptions->indexError, "bytes index out of range: %d", (int)asInt);
		}

		return INTEGER_VAL(self->bytes[asInt]);

	} else if (IS_slice(argv[1])) {
		KRK_SLICER(argv[1],self->length) {
			return NONE_VAL();
		}

		if (step == 1) {
			krk_integer_type len = end - start;
			return OBJECT_VAL(krk_newBytes(len, &self->bytes[start]));
		} else {
			struct StringBuilder sb = {0};
			krk_integer_type i = start;
			while ((step < 0) ? (i > end) : (i < end)) {
				pushStringBuilder(&sb, self->bytes[i]);
				i += step;
			}
			return finishStringBuilderBytes(&sb);
		}
	} else {
		return TYPE_ERROR(int or slice, argv[1]);
	}
}

KRK_Method(bytes,__len__) {
	return INTEGER_VAL(AS_BYTES(argv[0])->length);
}

KRK_Method(bytes,__contains__) {
	METHOD_TAKES_EXACTLY(1);

	if (IS_BYTES(argv[1])) {
		return krk_runtimeError(vm.exceptions->notImplementedError, "not implemented: bytes.__contains__(bytes)");
	}

	if (!IS_INTEGER(argv[1])) {
		return TYPE_ERROR(int,argv[1]);
	}

	krk_integer_type val = AS_INTEGER(argv[1]);
	if (val < 0 || val > 255) {
		return krk_runtimeError(vm.exceptions->valueError, "byte must be in range(0, 256)");
	}

	for (size_t i = 0; i < self->length; ++i) {
		if (self->bytes[i] == val) return BOOLEAN_VAL(1);
	}

	return BOOLEAN_VAL(0);
}

KRK_Method(bytes,decode) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(krk_copyString((char*)AS_BYTES(argv[0])->bytes, AS_BYTES(argv[0])->length));
}

struct _bytes_join_context {
	struct StringBuilder * sb;
	KrkBytes * self;
	int isFirst;
};

static int _bytes_join_callback(void * context, const KrkValue * values, size_t count) {
	struct _bytes_join_context * _context = context;

	for (size_t i = 0; i < count; ++i) {
		if (!IS_BYTES(values[i])) {
			krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%T'",
				"join", "bytes", values[i]);
			return 1;
		}

		if (_context->isFirst) {
			_context->isFirst = 0;
		} else {
			pushStringBuilderStr(_context->sb, (char*)_context->self->bytes, _context->self->length);
		}
		pushStringBuilderStr(_context->sb, (char*)AS_BYTES(values[i])->bytes, AS_BYTES(values[i])->length);
	}

	return 0;
}

KRK_Method(bytes,join) {
	METHOD_TAKES_EXACTLY(1);

	struct StringBuilder sb = {0};

	struct _bytes_join_context context = {&sb, self, 1};

	if (krk_unpackIterable(argv[1], &context, _bytes_join_callback)) {
		discardStringBuilder(&sb);
		return NONE_VAL();
	}

	return finishStringBuilderBytes(&sb);
}

KRK_Method(bytes,__add__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,bytes,KrkBytes*,them);

	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, (char*)self->bytes, self->length);
	pushStringBuilderStr(&sb, (char*)them->bytes, them->length);

	return finishStringBuilderBytes(&sb);
}

FUNC_SIG(bytesiterator,__init__);

KRK_Method(bytes,__iter__) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->bytesiteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(bytesiterator,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();

	return OBJECT_VAL(output);
}

#undef CURRENT_CTYPE

struct BytesIterator {
	KrkInstance inst;
	KrkValue l;
	size_t i;
};

#define CURRENT_CTYPE struct BytesIterator *
#define IS_bytesiterator(o) krk_isInstanceOf(o,vm.baseClasses->bytesiteratorClass)
#define AS_bytesiterator(o) (struct BytesIterator*)AS_OBJECT(o)

static void _bytesiterator_gcscan(KrkInstance * self) {
	krk_markValue(((struct BytesIterator*)self)->l);
}

KRK_Method(bytesiterator,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,bytes,KrkBytes*,bytes);
	self->l = argv[1];
	self->i = 0;
	return NONE_VAL();
}

KRK_Method(bytesiterator,__call__) {
	KrkValue _list = self->l;
	size_t _counter = self->i;
	if (!IS_BYTES(_list) || _counter >= AS_BYTES(_list)->length) {
		return argv[0];
	} else {
		self->i = _counter + 1;
		return INTEGER_VAL(AS_BYTES(_list)->bytes[_counter]);
	}
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct ByteArray *

static void _bytearray_gcscan(KrkInstance * self) {
	krk_markValue(((struct ByteArray*)self)->actual);
}

KRK_Method(bytearray,__init__) {
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) {
		self->actual = OBJECT_VAL(krk_newBytes(0,NULL));
	} else if (IS_BYTES(argv[1])) {
		self->actual = OBJECT_VAL(krk_newBytes(AS_BYTES(argv[1])->length, AS_BYTES(argv[1])->bytes));
	} else if (IS_INTEGER(argv[1])) {
		self->actual = OBJECT_VAL(krk_newBytes(AS_INTEGER(argv[1]),NULL));
		memset(AS_BYTES(self->actual)->bytes, 0, AS_BYTES(self->actual)->length);
	} else {
		return krk_runtimeError(vm.exceptions->valueError, "expected bytes");
	}
	return NONE_VAL();
}

#undef IS_bytearray
#define IS_bytearray(o) (krk_isInstanceOf(o,vm.baseClasses->bytearrayClass) && IS_BYTES(AS_bytearray(o)->actual))

/* bytes objects are not interned; need to do this the old-fashioned way. */
KRK_Method(bytearray,__eq__) {
	if (!IS_bytearray(argv[1])) return BOOLEAN_VAL(0);
	struct ByteArray * them = AS_bytearray(argv[1]);
	return BOOLEAN_VAL(krk_valuesEqual(self->actual, them->actual));
}

KRK_Method(bytearray,__repr__) {
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, "bytearray(", 10);
	if (!krk_pushStringBuilderFormat(&sb, "%R", self->actual)) {
		krk_discardStringBuilder(&sb);
		return NONE_VAL();
	}
	pushStringBuilder(&sb,')');
	return finishStringBuilder(&sb);
}

KRK_Method(bytearray,__getitem__) {
	METHOD_TAKES_EXACTLY(1);

	if (IS_INTEGER(argv[1])) {
		CHECK_ARG(1,int,krk_integer_type,asInt);

		if (asInt < 0) asInt += (long)AS_BYTES(self->actual)->length;
		if (asInt < 0 || asInt >= (long)AS_BYTES(self->actual)->length) {
			return krk_runtimeError(vm.exceptions->indexError, "bytearray index out of range: %d", (int)asInt);
		}

		return INTEGER_VAL(AS_BYTES(self->actual)->bytes[asInt]);
	} else if (IS_slice(argv[1])) {
		KRK_SLICER(argv[1],AS_BYTES(self->actual)->length) {
			return NONE_VAL();
		}

		if (step == 1) {
			krk_integer_type len = end - start;
			return OBJECT_VAL(krk_newBytes(len, &AS_BYTES(self->actual)->bytes[start]));
		} else {
			struct StringBuilder sb = {0};
			krk_integer_type i = start;
			while ((step < 0) ? (i > end) : (i < end)) {
				pushStringBuilder(&sb, AS_BYTES(self->actual)->bytes[i]);
				i += step;
			}
			return finishStringBuilderBytes(&sb);
		}

	} else {
		return TYPE_ERROR(int or slice, argv[1]);
	}
}

KRK_Method(bytearray,__setitem__) {
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,asInt);
	CHECK_ARG(2,int,krk_integer_type,val);

	if (asInt < 0) asInt += (long)AS_BYTES(self->actual)->length;
	if (asInt < 0 || asInt >= (long)AS_BYTES(self->actual)->length) {
		return krk_runtimeError(vm.exceptions->indexError, "bytearray index out of range: %d", (int)asInt);
	}
	AS_BYTES(self->actual)->bytes[asInt] = val;

	return INTEGER_VAL(AS_BYTES(self->actual)->bytes[asInt]);
}

KRK_Method(bytearray,__len__) {
	return INTEGER_VAL(AS_BYTES(self->actual)->length);
}

KRK_Method(bytearray,__contains__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,val);
	for (size_t i = 0; i < AS_BYTES(self->actual)->length; ++i) {
		if (AS_BYTES(self->actual)->bytes[i] == val) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

KRK_Method(bytearray,decode) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(krk_copyString((char*)AS_BYTES(self->actual)->bytes, AS_BYTES(self->actual)->length));
}

KRK_Method(bytearray,__iter__) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->bytesiteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(bytesiterator,__init__)(2, (KrkValue[]){krk_peek(0), self->actual},0);
	krk_pop();

	return OBJECT_VAL(output);
}


_noexport
void _createAndBind_bytesClass(void) {
	KrkClass * bytes = ADD_BASE_CLASS(vm.baseClasses->bytesClass, "bytes", vm.baseClasses->objectClass);
	bytes->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	bytes->allocSize = 0;
	KRK_DOC(BIND_STATICMETHOD(bytes,__new__),
		"@brief An array of bytes.\n"
		"@arguments iter=None\n\n"
		"Creates a new @ref bytes object. If @p iter is provided, it should be a @ref tuple or @ref list "
		"of integers within the range @c 0 and @c 255.");
	BIND_METHOD(bytes,__repr__);
	BIND_METHOD(bytes,__len__);
	BIND_METHOD(bytes,__contains__);
	BIND_METHOD(bytes,__getitem__);
	BIND_METHOD(bytes,__eq__);
	BIND_METHOD(bytes,__add__);
	BIND_METHOD(bytes,__iter__);
	BIND_METHOD(bytes,__hash__);
	BIND_METHOD(bytes,decode);
	BIND_METHOD(bytes,join);
	krk_finalizeClass(bytes);

	KrkClass * bytesiterator = ADD_BASE_CLASS(vm.baseClasses->bytesiteratorClass, "bytesiterator", vm.baseClasses->objectClass);
	bytesiterator->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	bytesiterator->allocSize = sizeof(struct BytesIterator);
	bytesiterator->_ongcscan = _bytesiterator_gcscan;
	BIND_METHOD(bytesiterator,__init__);
	BIND_METHOD(bytesiterator,__call__);
	krk_finalizeClass(bytesiterator);

	KrkClass * bytearray = ADD_BASE_CLASS(vm.baseClasses->bytearrayClass, "bytearray", vm.baseClasses->objectClass);
	bytearray->allocSize = sizeof(struct ByteArray);
	bytearray->_ongcscan = _bytearray_gcscan;
	KRK_DOC(BIND_METHOD(bytearray,__init__),
		"@brief A mutable array of bytes.\n"
		"@arguments bytes=None");
	BIND_METHOD(bytearray,__repr__);
	BIND_METHOD(bytearray,__len__);
	BIND_METHOD(bytearray,__contains__);
	BIND_METHOD(bytearray,__getitem__);
	BIND_METHOD(bytearray,__setitem__);
	BIND_METHOD(bytearray,__eq__);
	BIND_METHOD(bytearray,__iter__);
	BIND_METHOD(bytearray,decode);
	krk_finalizeClass(bytearray);
}
