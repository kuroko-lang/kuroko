#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

struct ByteArray {
	KrkInstance inst;
	KrkValue actual;
};

#define AS_bytes(o) AS_BYTES(o)
#define CURRENT_CTYPE KrkBytes *
#define CURRENT_NAME  self

#undef IS_bytes
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
				return krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%s'",
					"bytes", "tuple of ints", krk_typeName(AS_TUPLE(argv[1])->values.values[i]));
			}
			out->bytes[i] = AS_INTEGER(AS_TUPLE(argv[1])->values.values[i]);
		}
		return krk_pop();
	} else if (IS_list(argv[1])) {
		KrkBytes * out = krk_newBytes(AS_LIST(argv[1])->count, NULL);
		krk_push(OBJECT_VAL(out));
		for (size_t i = 0; i < AS_LIST(argv[1])->count; ++i) {
			if (!IS_INTEGER(AS_LIST(argv[1])->values[i])) {
				return krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%s'",
					"bytes", "list of ints", krk_typeName(AS_LIST(argv[1])->values[i]));
			}
			out->bytes[i] = AS_INTEGER(AS_LIST(argv[1])->values[i]);
		}
		return krk_pop();
	} else if (IS_bytearray(argv[1])) {
		return OBJECT_VAL(krk_newBytes(
			AS_BYTES(AS_bytearray(argv[1])->actual)->length,
			AS_BYTES(AS_bytearray(argv[1])->actual)->bytes));
	}

	return krk_runtimeError(vm.exceptions->typeError, "Can not convert '%s' to bytes", krk_typeName(argv[1]));
})

#undef IS_bytes
#define IS_bytes(o) IS_BYTES(o)

KRK_METHOD(bytes,__hash__,{
	METHOD_TAKES_NONE();
	uint32_t hash = 0;
	/* This is the so-called "sdbm" hash. It comes from a piece of
	 * public domain code from a clone of ndbm. */
	for (size_t i = 0; i < self->length; ++i) {
		hash = (int)self->bytes[i] + (hash << 6) + (hash << 16) - hash;
	}
	return INTEGER_VAL(hash);
})

/* bytes objects are not interned; need to do this the old-fashioned way. */
KRK_METHOD(bytes,__eq__,{
	if (!IS_BYTES(argv[1])) return BOOLEAN_VAL(0);
	KrkBytes * self = AS_BYTES(argv[0]);
	KrkBytes * them = AS_BYTES(argv[1]);
	if (self->length != them->length) return BOOLEAN_VAL(0);
	if (self->obj.hash != them->obj.hash) return BOOLEAN_VAL(0);
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
})

KRK_METHOD(bytes,__getitem__,{
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

struct _bytes_join_context {
	struct StringBuilder * sb;
	KrkBytes * self;
	int isFirst;
};

static int _bytes_join_callback(void * context, const KrkValue * values, size_t count) {
	struct _bytes_join_context * _context = context;

	for (size_t i = 0; i < count; ++i) {
		if (!IS_BYTES(values[i])) {
			krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%s'",
				"join", "bytes", krk_typeName(values[i]));
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

KRK_METHOD(bytes,join,{
	METHOD_TAKES_EXACTLY(1);

	struct StringBuilder sb = {0};

	struct _bytes_join_context context = {&sb, self, 1};

	if (krk_unpackIterable(argv[1], &context, _bytes_join_callback)) {
		discardStringBuilder(&sb);
		return NONE_VAL();
	}

	return finishStringBuilderBytes(&sb);
})

KRK_METHOD(bytes,__add__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,bytes,KrkBytes*,them);

	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, (char*)self->bytes, self->length);
	pushStringBuilderStr(&sb, (char*)them->bytes, them->length);

	return finishStringBuilderBytes(&sb);
})

FUNC_SIG(bytesiterator,__init__);

KRK_METHOD(bytes,__iter__,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->bytesiteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(bytesiterator,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();

	return OBJECT_VAL(output);
})

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

KRK_METHOD(bytesiterator,__init__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,bytes,KrkBytes*,bytes);
	self->l = argv[1];
	self->i = 0;
	return argv[0];
})

KRK_METHOD(bytesiterator,__call__,{
	KrkValue _list = self->l;
	size_t _counter = self->i;
	if (!IS_BYTES(_list) || _counter >= AS_BYTES(_list)->length) {
		return argv[0];
	} else {
		self->i = _counter + 1;
		return INTEGER_VAL(AS_BYTES(_list)->bytes[_counter]);
	}
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct ByteArray *

static void _bytearray_gcscan(KrkInstance * self) {
	krk_markValue(((struct ByteArray*)self)->actual);
}

KRK_METHOD(bytearray,__init__,{
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) {
		self->actual = OBJECT_VAL(krk_newBytes(0,NULL));
	} else if (IS_BYTES(argv[1])) {
		self->actual = OBJECT_VAL(krk_newBytes(AS_BYTES(argv[1])->length, AS_BYTES(argv[1])->bytes));
	} else if (IS_INTEGER(argv[1])) {
		self->actual = OBJECT_VAL(krk_newBytes(AS_INTEGER(argv[1]),NULL));
	} else {
		return krk_runtimeError(vm.exceptions->valueError, "expected bytes");
	}
	return argv[0];
})

#undef IS_bytearray
#define IS_bytearray(o) (krk_isInstanceOf(o,vm.baseClasses->bytearrayClass) && IS_BYTES(AS_bytearray(o)->actual))

/* bytes objects are not interned; need to do this the old-fashioned way. */
KRK_METHOD(bytearray,__eq__,{
	if (!IS_bytearray(argv[1])) return BOOLEAN_VAL(0);
	struct ByteArray * them = AS_bytearray(argv[1]);
	return BOOLEAN_VAL(krk_valuesEqual(self->actual, them->actual));
})

KRK_METHOD(bytearray,__repr__,{
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, "bytearray(", 10);

	krk_push(self->actual);
	KrkValue repred_bytes = krk_callDirect(vm.baseClasses->bytesClass->_reprer, 1);
	if (!IS_STRING(repred_bytes)) {
		/* Invalid repr of bytes? */
		discardStringBuilder(&sb);
		return NONE_VAL();
	}
	pushStringBuilderStr(&sb, AS_STRING(repred_bytes)->chars, AS_STRING(repred_bytes)->length);
	pushStringBuilder(&sb,')');
	return finishStringBuilder(&sb);
})

KRK_METHOD(bytearray,__getitem__,{
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
})

KRK_METHOD(bytearray,__setitem__,{
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,asInt);
	CHECK_ARG(2,int,krk_integer_type,val);

	if (asInt < 0) asInt += (long)AS_BYTES(self->actual)->length;
	if (asInt < 0 || asInt >= (long)AS_BYTES(self->actual)->length) {
		return krk_runtimeError(vm.exceptions->indexError, "bytearray index out of range: %d", (int)asInt);
	}
	AS_BYTES(self->actual)->bytes[asInt] = val;

	return INTEGER_VAL(AS_BYTES(self->actual)->bytes[asInt]);
})

KRK_METHOD(bytearray,__len__,{
	return INTEGER_VAL(AS_BYTES(self->actual)->length);
})

KRK_METHOD(bytearray,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,val);
	for (size_t i = 0; i < AS_BYTES(self->actual)->length; ++i) {
		if (AS_BYTES(self->actual)->bytes[i] == val) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
})

KRK_METHOD(bytearray,decode,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(krk_copyString((char*)AS_BYTES(self->actual)->bytes, AS_BYTES(self->actual)->length));
})

KRK_METHOD(bytearray,__iter__,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->bytesiteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(bytesiterator,__init__)(2, (KrkValue[]){krk_peek(0), self->actual},0);
	krk_pop();

	return OBJECT_VAL(output);
})


_noexport
void _createAndBind_bytesClass(void) {
	KrkClass * bytes = ADD_BASE_CLASS(vm.baseClasses->bytesClass, "bytes", vm.baseClasses->objectClass);
	bytes->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	KRK_DOC(BIND_METHOD(bytes,__init__),
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
	krk_defineNative(&bytes->methods,"__str__",FUNC_NAME(bytes,__repr__)); /* alias */
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
	krk_defineNative(&bytearray->methods,"__str__",FUNC_NAME(bytearray,__repr__)); /* alias */
	krk_finalizeClass(bytearray);
}
