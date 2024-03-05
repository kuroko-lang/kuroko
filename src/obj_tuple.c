#include <string.h>
#include <limits.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#define TUPLE_WRAP_INDEX() \
	if (index < 0) index += self->values.count; \
	if (index < 0 || index >= (krk_integer_type)self->values.count) return krk_runtimeError(vm.exceptions->indexError, "tuple index out of range: %zd", (ssize_t)index)

static int _tuple_init_callback(void * context, const KrkValue * values, size_t count) {
	KrkValueArray * positionals = context;
	if (positionals->count + count > positionals->capacity) {
		size_t old = positionals->capacity;
		positionals->capacity = (count == 1) ? KRK_GROW_CAPACITY(old) : (positionals->count + count);
		positionals->values = KRK_GROW_ARRAY(KrkValue, positionals->values, old, positionals->capacity);
	}

	for (size_t i = 0; i < count; ++i) {
		positionals->values[positionals->count++] = values[i];
	}

	return 0;
}

KRK_StaticMethod(tuple,__new__) {
	METHOD_TAKES_AT_MOST(1);
	if (argc == 1) {
		return OBJECT_VAL(krk_newTuple(0));
	}
	krk_push(OBJECT_VAL(krk_newTuple(0)));
	KrkValueArray * positionals = &AS_TUPLE(krk_peek(0))->values;
	KrkValue other = argv[1];
	krk_unpackIterable(other, positionals, _tuple_init_callback);
	return krk_pop();
}

/* tuple creator */
KrkValue krk_tuple_of(int argc, const KrkValue argv[], int hasKw) {
	KrkTuple * self = krk_newTuple(argc);
	krk_push(OBJECT_VAL(self));
	for (size_t i = 0; i < (size_t)argc; ++i) {
		self->values.values[self->values.count++] = argv[i];
	}
	krk_pop();

	return OBJECT_VAL(self);
}

#define IS_tuple(o) IS_TUPLE(o)
#define AS_tuple(o) AS_TUPLE(o)

#define CURRENT_CTYPE KrkTuple *
#define CURRENT_NAME  self

KRK_Method(tuple,__contains__) {
	METHOD_TAKES_EXACTLY(1);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesSameOrEqual(self->values.values[i], argv[1])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

KRK_Method(tuple,__len__) {
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->values.count);
}

KRK_Method(tuple,__getitem__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) {
		CHECK_ARG(1,int,krk_integer_type,index);
		TUPLE_WRAP_INDEX();
		return self->values.values[index];
	} else if (IS_slice(argv[1])) {
		KRK_SLICER(argv[1],self->values.count) {
			return NONE_VAL();
		}

		if (step == 1) {
			krk_integer_type len = end - start;
			KrkValue result = krk_tuple_of(len, &self->values.values[start], 0);
			return result;
		} else {
			/* iterate and push */
			krk_push(NONE_VAL());
			krk_integer_type len = 0;
			krk_integer_type i = start;
			while ((step < 0) ? (i > end) : (i < end)) {
				krk_push(self->values.values[i]);
				len++;
				i += step;
			}

			/* make into a list */
			KrkValue result = krk_callNativeOnStack(len, &krk_currentThread.stackTop[-len], 0, krk_tuple_of);
			krk_currentThread.stackTop[-len-1] = result;
			while (len) {
				krk_pop();
				len--;
			}

			return krk_pop();
		}
	} else {
		return TYPE_ERROR(int or slice, argv[1]);
	}
}

KRK_Method(tuple,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_tuple(argv[1])) return NOTIMPL_VAL();
	KrkTuple * them = AS_tuple(argv[1]);
	if (self->values.count != them->values.count) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (!krk_valuesSameOrEqual(self->values.values[i], them->values.values[i])) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

#define MAKE_TUPLE_COMPARE(name,op) \
	KRK_Method(tuple,__ ## name ## __) { \
		METHOD_TAKES_EXACTLY(1); \
		if (!IS_tuple(argv[1])) return NOTIMPL_VAL(); \
		KrkTuple * them = AS_tuple(argv[1]); \
		size_t lesser = self->values.count < them->values.count ? self->values.count : them->values.count; \
		for (size_t i = 0; i < lesser; ++i) { \
			KrkValue a = self->values.values[i]; \
			KrkValue b = them->values.values[i]; \
			if (krk_valuesSameOrEqual(a,b)) continue; \
			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return NONE_VAL(); \
			return krk_operator_ ## name(a,b); \
		} \
		return BOOLEAN_VAL((self->values.count op them->values.count)); \
	}

MAKE_TUPLE_COMPARE(gt,>)
MAKE_TUPLE_COMPARE(lt,<)
MAKE_TUPLE_COMPARE(ge,>=)
MAKE_TUPLE_COMPARE(le,<=)

KRK_Method(tuple,__repr__) {
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("(...)"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	/* String building time. */
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb, '(');

	for (size_t i = 0; i < self->values.count; ++i) {
		if (i) pushStringBuilderStr(&sb, ", ", 2);
		if (!krk_pushStringBuilderFormat(&sb, "%R", self->values.values[i])) goto _error;
	}

	if (self->values.count == 1) {
		pushStringBuilder(&sb, ',');
	}

	pushStringBuilder(&sb, ')');
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

KRK_Method(tuple,__add__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_tuple(argv[1]))
		return krk_runtimeError(vm.exceptions->typeError,
			"can only concatenate tuple (not '%T') to tuple", argv[1]);

	KrkTuple * other = AS_tuple(argv[1]);
	KrkTuple * out = krk_newTuple(self->values.count + other->values.count);
	krk_push(OBJECT_VAL(out));
	for (size_t i = 0; i < self->values.count; ++i) {
		out->values.values[out->values.count++] = self->values.values[i];
	}
	for (size_t i = 0; i < other->values.count; ++i) {
		out->values.values[out->values.count++] = other->values.values[i];
	}
	return krk_pop();
}

/**
 * @brief Iterator over the values in a tuple.
 * @extends KrkInstance
 */
struct TupleIter {
	KrkInstance inst;
	KrkValue myTuple;
	int i;
};

static KrkValue _tuple_iter_init(int argc, const KrkValue argv[], int hasKw) {
	struct TupleIter * self = (struct TupleIter *)AS_OBJECT(argv[0]);
	self->myTuple = argv[1];
	self->i = 0;
	return argv[0];
}

static void _tuple_iter_gcscan(KrkInstance * self) {
	krk_markValue(((struct TupleIter*)self)->myTuple);
}

static KrkValue _tuple_iter_call(int argc, const KrkValue argv[], int hasKw) {
	struct TupleIter * self = (struct TupleIter *)AS_OBJECT(argv[0]);
	KrkValue t = self->myTuple; /* Tuple to iterate */
	int i = self->i;
	if (i >= (krk_integer_type)AS_TUPLE(t)->values.count) {
		return argv[0];
	} else {
		self->i = i+1;
		return AS_TUPLE(t)->values.values[i];
	}
}

KRK_Method(tuple,__iter__) {
	KrkInstance * output = krk_newInstance(vm.baseClasses->tupleiteratorClass);
	krk_push(OBJECT_VAL(output));
	_tuple_iter_init(2, (KrkValue[]){krk_peek(0), argv[0]}, 0);
	krk_pop();
	return OBJECT_VAL(output);
}

KRK_Method(tuple,__hash__) {
	if (self->obj.flags & KRK_OBJ_FLAGS_VALID_HASH) {
		return INTEGER_VAL(self->obj.hash);
	}
	uint32_t t = self->values.count;
	uint32_t m = 0x3456;
	for (size_t i = 0; i < (size_t)self->values.count; ++i) {
		uint32_t step = 0;
		if (krk_hashValue(self->values.values[i], &step)) goto _unhashable;
		t = (t ^ step) * m;
		m += 2 * (self->values.count - i) + 82520;
	}
	self->obj.hash = t;
	self->obj.flags |= KRK_OBJ_FLAGS_VALID_HASH;
	return INTEGER_VAL(self->obj.hash);
_unhashable:
	return NONE_VAL();
}

KRK_Method(tuple,__mul__) {
	METHOD_TAKES_EXACTLY(1);

	if (!IS_INTEGER(argv[1])) return NOTIMPL_VAL();

	ssize_t count = AS_INTEGER(argv[1]);
	if (count < 0) count = 0;
	KrkTuple * out = krk_newTuple(count * self->values.count);
	krk_push(OBJECT_VAL(out));
	for (ssize_t i = 0; i < count; ++i) {
		for (size_t j = 0; j < self->values.count; ++j) {
			out->values.values[out->values.count++] = self->values.values[j];
		}
	}

	return krk_pop();
}

_noexport
void _createAndBind_tupleClass(void) {
	KrkClass * tuple = ADD_BASE_CLASS(vm.baseClasses->tupleClass, "tuple", vm.baseClasses->objectClass);
	tuple->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	tuple->allocSize = 0;
	BIND_STATICMETHOD(tuple,__new__);
	BIND_METHOD(tuple,__repr__);
	BIND_METHOD(tuple,__getitem__);
	BIND_METHOD(tuple,__len__);
	BIND_METHOD(tuple,__contains__);
	BIND_METHOD(tuple,__iter__);
	BIND_METHOD(tuple,__eq__);
	BIND_METHOD(tuple,__lt__);
	BIND_METHOD(tuple,__gt__);
	BIND_METHOD(tuple,__le__);
	BIND_METHOD(tuple,__ge__);
	BIND_METHOD(tuple,__hash__);
	BIND_METHOD(tuple,__add__);
	BIND_METHOD(tuple,__mul__);
	krk_finalizeClass(tuple);

	ADD_BASE_CLASS(vm.baseClasses->tupleiteratorClass, "tupleiterator", vm.baseClasses->objectClass);
	vm.baseClasses->tupleiteratorClass->allocSize = sizeof(struct TupleIter);
	vm.baseClasses->tupleiteratorClass->_ongcscan = _tuple_iter_gcscan;
	krk_defineNative(&vm.baseClasses->tupleiteratorClass->methods, "__init__", _tuple_iter_init);
	krk_defineNative(&vm.baseClasses->tupleiteratorClass->methods, "__call__", _tuple_iter_call);
	krk_finalizeClass(vm.baseClasses->tupleiteratorClass);

}
