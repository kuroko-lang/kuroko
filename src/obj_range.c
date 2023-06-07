#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

/**
 * @brief `range` object.
 * @extends KrkInstance
 *
 * Generators iterator values that count from @p min to @p max.
 */
struct Range {
	KrkInstance inst;
	krk_integer_type min;
	krk_integer_type max;
	krk_integer_type step;
};
#define IS_range(o)   (krk_isInstanceOf(o,KRK_BASE_CLASS(range)))
#define AS_range(o)   ((struct Range*)AS_OBJECT(o))

struct RangeIterator {
	KrkInstance inst;
	krk_integer_type i;
	krk_integer_type max;
	krk_integer_type step;
};
#define IS_rangeiterator(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(rangeiterator)))
#define AS_rangeiterator(o) ((struct RangeIterator*)AS_OBJECT(o))

FUNC_SIG(rangeiterator,__init__);

#define CURRENT_NAME  self
#define CURRENT_CTYPE struct Range *

KRK_Method(range,__init__) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(3);
	self->min = 0;
	self->step = 1;
	if (argc == 2) {
		CHECK_ARG(1,int,krk_integer_type,_max);
		self->max = _max;
	} else {
		CHECK_ARG(1,int,krk_integer_type,_min);
		CHECK_ARG(2,int,krk_integer_type,_max);
		self->min = _min;
		self->max = _max;
		if (argc == 4) {
			CHECK_ARG(3,int,krk_integer_type,_step);
			if (_step == 0) {
				return krk_runtimeError(vm.exceptions->valueError, "range() arg 3 must not be zero");
			}
			self->step = _step;
		}
	}
	return NONE_VAL();
}

KRK_Method(range,__repr__) {
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};
	krk_pushStringBuilderFormat(&sb, "range(%zd,%zd", (ssize_t)self->min, (ssize_t)self->max);
	if (self->step != 1) krk_pushStringBuilderFormat(&sb, ",%zd", (ssize_t)self->step);
	krk_pushStringBuilder(&sb,')');
	return krk_finishStringBuilder(&sb);
}

KRK_Method(range,__iter__) {
	KrkInstance * output = krk_newInstance(KRK_BASE_CLASS(rangeiterator));
	krk_integer_type min = self->min;
	krk_integer_type max = self->max;
	krk_integer_type step = self->step;

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(rangeiterator,__init__)(4, (KrkValue[]){krk_peek(0), INTEGER_VAL(min), INTEGER_VAL(max), INTEGER_VAL(step)},0);
	krk_pop();

	return OBJECT_VAL(output);
}

KRK_Method(range,__contains__) {
	int i;
	if (!krk_parseArgs(".i", (const char*[]){"i"}, &i)) return NONE_VAL();
	if (self->step == 1) return BOOLEAN_VAL(i >= self->min && i < self->max);
	if (self->step == -1) return BOOLEAN_VAL(i <= self->min && i > self->max);
	if (self->step > 0) {
		if (i >= self->max || i < self->min) return BOOLEAN_VAL(0);
		if ((i - self->min) % self->step) return BOOLEAN_VAL(0);
		return BOOLEAN_VAL(1);
	} else {
		if (i <= self->max || i > self->min) return BOOLEAN_VAL(0);
		if ((i - self->min) % -self->step) return BOOLEAN_VAL(0);
		return BOOLEAN_VAL(1);
	}
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct RangeIterator *

KRK_Method(rangeiterator,__init__) {
	METHOD_TAKES_EXACTLY(3);
	CHECK_ARG(1,int,krk_integer_type,i);
	CHECK_ARG(2,int,krk_integer_type,max);
	CHECK_ARG(3,int,krk_integer_type,step);
	self->i = i;
	self->max = max;
	self->step = step;
	return NONE_VAL();
}

KRK_Method(rangeiterator,__call__) {
	METHOD_TAKES_NONE();
	krk_integer_type i = self->i;
	if (self->step > 0 ? (i >= self->max) : (i <= self->max)) {
		return argv[0];
	} else {
		self->i = i + self->step;
		return INTEGER_VAL(i);
	}
}

_noexport
void _createAndBind_rangeClass(void) {
	KrkClass * range = ADD_BASE_CLASS(vm.baseClasses->rangeClass, "range", vm.baseClasses->objectClass);
	range->allocSize = sizeof(struct Range);
	range->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	KRK_DOC(BIND_METHOD(range,__init__),
		"@brief Create an iterable that produces sequential numeric values.\n"
		"@arguments [min,] max, [step]\n\n"
		"With one argument, iteration will start at @c 0 and continue to @p max, exclusive. "
		"With two arguments, iteration starts at @p min and continues to @p max, exclusive. "
		"With three arguments, a @p step may also be included.");
	BIND_METHOD(range,__iter__);
	BIND_METHOD(range,__repr__);
	BIND_METHOD(range,__contains__);
	KRK_DOC(range, "@brief Iterable object that produces sequential numeric values.");
	krk_finalizeClass(range);

	KrkClass * rangeiterator = ADD_BASE_CLASS(vm.baseClasses->rangeiteratorClass, "rangeiterator", vm.baseClasses->objectClass);
	rangeiterator->allocSize = sizeof(struct RangeIterator);
	rangeiterator->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(rangeiterator,__init__);
	BIND_METHOD(rangeiterator,__call__);
	krk_finalizeClass(rangeiterator);
}
