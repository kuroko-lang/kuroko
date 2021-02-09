#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

static KrkValue _tuple_init(int argc, KrkValue argv[]) {
	return krk_runtimeError(vm.exceptions->typeError,"tuple() initializier unsupported");
}

/* tuple creator */
static KrkValue _tuple_of(int argc, KrkValue argv[]) {
	KrkTuple * self = krk_newTuple(argc);
	krk_push(OBJECT_VAL(self));
	for (size_t i = 0; i < (size_t)argc; ++i) {
		self->values.values[self->values.count++] = argv[i];
	}
	krk_pop();
	return OBJECT_VAL(self);
}

KrkValue krk_tuple_of(int argc, KrkValue argv[]) __attribute__((alias("_tuple_of")));


static KrkValue _tuple_contains(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions->argumentError, "tuple.__contains__ expects one argument");
	KrkTuple * self = AS_TUPLE(argv[0]);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

/* tuple.__len__ */
static KrkValue _tuple_len(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "tuple.__len__ does not expect arguments");
	KrkTuple * self = AS_TUPLE(argv[0]);
	return INTEGER_VAL(self->values.count);
}

/* tuple.__get__ */
static KrkValue _tuple_get(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions->argumentError, "tuple.__get__ expects one argument");
	else if (!IS_INTEGER(argv[1])) return krk_runtimeError(vm.exceptions->typeError, "can not index by '%s', expected integer", krk_typeName(argv[1]));
	KrkTuple * tuple = AS_TUPLE(argv[0]);
	long index = AS_INTEGER(argv[1]);
	if (index < 0) index += tuple->values.count;
	if (index < 0 || index >= (long)tuple->values.count) {
		return krk_runtimeError(vm.exceptions->indexError, "tuple index out of range");
	}
	return tuple->values.values[index];
}

static KrkValue _tuple_eq(int argc, KrkValue argv[]) {
	if (!IS_TUPLE(argv[1])) return BOOLEAN_VAL(0);
	KrkTuple * self = AS_TUPLE(argv[0]);
	KrkTuple * them = AS_TUPLE(argv[1]);
	if (self->values.count != them->values.count) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (!krk_valuesEqual(self->values.values[i], them->values.values[i])) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

static KrkValue _tuple_repr(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "tuple.__repr__ does not expect arguments");
	KrkTuple * tuple = AS_TUPLE(argv[0]);
	if (((KrkObj*)tuple)->inRepr) return OBJECT_VAL(S("(...)"));
	((KrkObj*)tuple)->inRepr = 1;
	/* String building time. */
	krk_push(OBJECT_VAL(S("(")));

	for (size_t i = 0; i < tuple->values.count; ++i) {
		krk_push(tuple->values.values[i]);
		krk_push(krk_callSimple(OBJECT_VAL(krk_getType(tuple->values.values[i])->_reprer), 1, 0));
		krk_addObjects(); /* pops both, pushes result */
		if (i != tuple->values.count - 1) {
			krk_push(OBJECT_VAL(S(", ")));
			krk_addObjects();
		}
	}

	if (tuple->values.count == 1) {
		krk_push(OBJECT_VAL(S(",")));
		krk_addObjects();
	}

	krk_push(OBJECT_VAL(S(")")));
	krk_addObjects();
	((KrkObj*)tuple)->inRepr = 0;
	return krk_pop();
}

struct TupleIter {
	KrkInstance inst;
	KrkValue myTuple;
	int i;
};

static KrkValue _tuple_iter_init(int argc, KrkValue argv[]) {
	struct TupleIter * self = (struct TupleIter *)AS_OBJECT(argv[0]);
	self->myTuple = argv[1];
	self->i = 0;
	return argv[0];
}

static void _tuple_iter_gcscan(KrkInstance * self) {
	krk_markValue(((struct TupleIter*)self)->myTuple);
}

static KrkValue _tuple_iter_call(int argc, KrkValue argv[]) {
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

static KrkValue _tuple_iter(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses->tupleiteratorClass);
	krk_push(OBJECT_VAL(output));
	_tuple_iter_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();
	return OBJECT_VAL(output);
}

_noexport
void _createAndBind_tupleClass(void) {
	ADD_BASE_CLASS(vm.baseClasses->tupleClass, "tuple", vm.baseClasses->objectClass);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__init__", _tuple_init);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__str__", _tuple_repr);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__repr__", _tuple_repr);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__get__", _tuple_get);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__len__", _tuple_len);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__contains__", _tuple_contains);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__iter__", _tuple_iter);
	krk_defineNative(&vm.baseClasses->tupleClass->methods, ".__eq__", _tuple_eq);
	krk_finalizeClass(vm.baseClasses->tupleClass);

	BUILTIN_FUNCTION("tupleOf",krk_tuple_of,"Convert argument sequence to tuple object.");

	ADD_BASE_CLASS(vm.baseClasses->tupleiteratorClass, "tupleiterator", vm.baseClasses->objectClass);
	vm.baseClasses->tupleiteratorClass->allocSize = sizeof(struct TupleIter);
	vm.baseClasses->tupleiteratorClass->_ongcscan = _tuple_iter_gcscan;
	krk_defineNative(&vm.baseClasses->tupleiteratorClass->methods, ".__init__", _tuple_iter_init);
	krk_defineNative(&vm.baseClasses->tupleiteratorClass->methods, ".__call__", _tuple_iter_call);
	krk_finalizeClass(vm.baseClasses->tupleiteratorClass);

}
