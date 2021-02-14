#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define TUPLE_WRAP_INDEX() \
	if (index < 0) index += self->values.count; \
	if (index < 0 || index >= (krk_integer_type)self->values.count) return krk_runtimeError(vm.exceptions->indexError, "tuple index out of range: " PRIkrk_int, index)

static KrkValue _tuple_init(int argc, KrkValue argv[], int hasKw) {
	return krk_runtimeError(vm.exceptions->typeError,"tuple() initializier unsupported");
}

inline void krk_tupleUpdateHash(KrkTuple * self) {
	self->obj.hash = self->values.count;
	for (size_t i = 0; i < (size_t)self->values.count; ++i) {
		self->obj.hash <<= 8;
		self->obj.hash ^= krk_hashValue(self->values.values[i]);
	}
}

/* tuple creator */
KrkValue krk_tuple_of(int argc, KrkValue argv[], int hasKw) {
	KrkTuple * self = krk_newTuple(argc);
	krk_push(OBJECT_VAL(self));
	for (size_t i = 0; i < (size_t)argc; ++i) {
		self->values.values[self->values.count++] = argv[i];
	}
	krk_tupleUpdateHash(self);
	krk_pop();

	return OBJECT_VAL(self);
}

#define IS_tuple(o) IS_TUPLE(o)
#define AS_tuple(o) AS_TUPLE(o)

#define CURRENT_CTYPE KrkTuple *
#define CURRENT_NAME  self

KRK_METHOD(tuple,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
})

KRK_METHOD(tuple,__len__,{
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->values.count);
})

KRK_METHOD(tuple,__get__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,index);
	TUPLE_WRAP_INDEX();
	return self->values.values[index];
})

KRK_METHOD(tuple,__eq__,{
	METHOD_TAKES_EXACTLY(1);
	if (!IS_tuple(argv[1])) return BOOLEAN_VAL(0);
	KrkTuple * them = AS_tuple(argv[1]);
	if (self->values.count != them->values.count) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (!krk_valuesEqual(self->values.values[i], them->values.values[i])) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
})

KRK_METHOD(tuple,__repr__,{
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("(...)"));
	((KrkObj*)self)->inRepr = 1;
	/* String building time. */
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb, '(');

	for (size_t i = 0; i < self->values.count; ++i) {
		KrkClass * type = krk_getType(self->values.values[i]);
		krk_push(self->values.values[i]);
		KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
		if (IS_STRING(result)) {
			pushStringBuilderStr(&sb, AS_STRING(result)->chars, AS_STRING(result)->length);
		}
		if (i != self->values.count - 1) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
	}

	if (self->values.count == 1) {
		pushStringBuilder(&sb, ',');
	}

	pushStringBuilder(&sb, ')');
	((KrkObj*)self)->inRepr = 0;
	return finishStringBuilder(&sb);
})

struct TupleIter {
	KrkInstance inst;
	KrkValue myTuple;
	int i;
};

static KrkValue _tuple_iter_init(int argc, KrkValue argv[], int hasKw) {
	struct TupleIter * self = (struct TupleIter *)AS_OBJECT(argv[0]);
	self->myTuple = argv[1];
	self->i = 0;
	return argv[0];
}

static void _tuple_iter_gcscan(KrkInstance * self) {
	krk_markValue(((struct TupleIter*)self)->myTuple);
}

static KrkValue _tuple_iter_call(int argc, KrkValue argv[], int hasKw) {
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

KRK_METHOD(tuple,__iter__,{
	KrkInstance * output = krk_newInstance(vm.baseClasses->tupleiteratorClass);
	krk_push(OBJECT_VAL(output));
	_tuple_iter_init(2, (KrkValue[]){krk_peek(0), argv[0]}, 0);
	krk_pop();
	return OBJECT_VAL(output);
})

_noexport
void _createAndBind_tupleClass(void) {
	KrkClass * tuple = ADD_BASE_CLASS(vm.baseClasses->tupleClass, "tuple", vm.baseClasses->objectClass);
	BIND_METHOD(tuple,__repr__);
	BIND_METHOD(tuple,__get__);
	BIND_METHOD(tuple,__len__);
	BIND_METHOD(tuple,__contains__);
	BIND_METHOD(tuple,__iter__);
	BIND_METHOD(tuple,__eq__);
	krk_defineNative(&tuple->methods, ".__init__", _tuple_init);
	krk_defineNative(&tuple->methods, ".__str__", FUNC_NAME(tuple,__repr__));
	krk_finalizeClass(tuple);

	BUILTIN_FUNCTION("tupleOf",krk_tuple_of,"Convert argument sequence to tuple object.");

	ADD_BASE_CLASS(vm.baseClasses->tupleiteratorClass, "tupleiterator", vm.baseClasses->objectClass);
	vm.baseClasses->tupleiteratorClass->allocSize = sizeof(struct TupleIter);
	vm.baseClasses->tupleiteratorClass->_ongcscan = _tuple_iter_gcscan;
	krk_defineNative(&vm.baseClasses->tupleiteratorClass->methods, ".__init__", _tuple_iter_init);
	krk_defineNative(&vm.baseClasses->tupleiteratorClass->methods, ".__call__", _tuple_iter_call);
	krk_finalizeClass(vm.baseClasses->tupleiteratorClass);

}
