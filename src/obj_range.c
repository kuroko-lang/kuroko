#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

struct Range {
	KrkInstance inst;
	krk_integer_type min;
	krk_integer_type max;
};

static KrkValue _range_init(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	if (argc < 2 || argc > 3) {
		return krk_runtimeError(vm.exceptions->argumentError, "range expected at least 1 and and at most 2 arguments");
	}
	KrkValue min = INTEGER_VAL(0);
	KrkValue max;
	if (argc == 2) {
		max = argv[1];
	} else {
		min = argv[1];
		max = argv[2];
	}
	if (!IS_INTEGER(min)) {
		return krk_runtimeError(vm.exceptions->typeError, "range: expected int, but got '%s'", krk_typeName(min));
	}
	if (!IS_INTEGER(max)) {
		return krk_runtimeError(vm.exceptions->typeError, "range: expected int, but got '%s'", krk_typeName(max));
	}

	((struct Range*)self)->min = AS_INTEGER(min);
	((struct Range*)self)->max = AS_INTEGER(max);

	return argv[0];
}

static KrkValue _range_repr(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	krk_integer_type min = ((struct Range*)self)->min;
	krk_integer_type max = ((struct Range*)self)->max;

	krk_push(OBJECT_VAL(S("range({},{})")));
	KrkValue output = krk_string_format(3, (KrkValue[]){krk_peek(0), INTEGER_VAL(min), INTEGER_VAL(max)}, 0);
	krk_pop();
	return output;
}

struct RangeIterator {
	KrkInstance inst;
	krk_integer_type i;
	krk_integer_type max;
};

static KrkValue _rangeiterator_init(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	((struct RangeIterator*)self)->i = AS_INTEGER(argv[1]);
	((struct RangeIterator*)self)->max = AS_INTEGER(argv[2]);

	return argv[0];
}

static KrkValue _rangeiterator_call(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	krk_integer_type i, max;
	i = ((struct RangeIterator*)self)->i;
	max = ((struct RangeIterator*)self)->max;

	if (i >= max) {
		return argv[0];
	} else {
		((struct RangeIterator*)self)->i = i + 1;
		return INTEGER_VAL(i);
	}
}

static KrkValue _range_iter(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	KrkInstance * output = krk_newInstance(vm.baseClasses->rangeiteratorClass);
	krk_integer_type min = ((struct Range*)self)->min;
	krk_integer_type max = ((struct Range*)self)->max;

	krk_push(OBJECT_VAL(output));
	_rangeiterator_init(3, (KrkValue[]){krk_peek(0), INTEGER_VAL(min), INTEGER_VAL(max)},0);
	krk_pop();

	return OBJECT_VAL(output);
}


_noexport
void _createAndBind_rangeClass(void) {
	ADD_BASE_CLASS(vm.baseClasses->rangeClass, "range", vm.baseClasses->objectClass);
	vm.baseClasses->rangeClass->allocSize = sizeof(struct Range);
	krk_defineNative(&vm.baseClasses->rangeClass->methods, ".__init__", _range_init);
	krk_defineNative(&vm.baseClasses->rangeClass->methods, ".__iter__", _range_iter);
	krk_defineNative(&vm.baseClasses->rangeClass->methods, ".__repr__", _range_repr);
	krk_finalizeClass(vm.baseClasses->rangeClass);
	vm.baseClasses->rangeClass->docstring = S("range(max), range(min, max[, step]): "
		"An iterable object that produces numeric values. "
		"'min' is inclusive, 'max' is exclusive.");

	ADD_BASE_CLASS(vm.baseClasses->rangeiteratorClass, "rangeiterator", vm.baseClasses->objectClass);
	vm.baseClasses->rangeiteratorClass->allocSize = sizeof(struct RangeIterator);
	krk_defineNative(&vm.baseClasses->rangeiteratorClass->methods, ".__init__", _rangeiterator_init);
	krk_defineNative(&vm.baseClasses->rangeiteratorClass->methods, ".__call__", _rangeiterator_call);
	krk_finalizeClass(vm.baseClasses->rangeiteratorClass);
}
