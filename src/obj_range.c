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
};
static KrkClass * range = NULL;
#define IS_range(o)   (krk_isInstanceOf(o,range))
#define AS_range(o)   ((struct Range*)AS_OBJECT(o))

struct RangeIterator {
	KrkInstance inst;
	krk_integer_type i;
	krk_integer_type max;
};
static KrkClass * rangeiterator = NULL;
#define IS_rangeiterator(o) (krk_isInstanceOf(o,rangeiterator))
#define AS_rangeiterator(o) ((struct RangeIterator*)AS_OBJECT(o))

FUNC_SIG(rangeiterator,__init__);

#define CURRENT_NAME  self
#define CURRENT_CTYPE struct Range *

KRK_METHOD(range,__init__,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	self->min = 0;
	if (argc == 2) {
		CHECK_ARG(1,int,krk_integer_type,_max);
		self->max = _max;
	} else {
		CHECK_ARG(1,int,krk_integer_type,_min);
		CHECK_ARG(2,int,krk_integer_type,_max);
		self->min = _min;
		self->max = _max;
	}
	return argv[0];
})

KRK_METHOD(range,__repr__,{
	METHOD_TAKES_NONE();
	krk_integer_type min = self->min;
	krk_integer_type max = self->max;
	char tmp[1024];
	size_t len = snprintf(tmp,1024,"range(" PRIkrk_int "," PRIkrk_int ")", min, max);
	return OBJECT_VAL(krk_copyString(tmp,len));
})

KRK_METHOD(range,__iter__,{
	KrkInstance * output = krk_newInstance(rangeiterator);
	krk_integer_type min = self->min;
	krk_integer_type max = self->max;

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(rangeiterator,__init__)(3, (KrkValue[]){krk_peek(0), INTEGER_VAL(min), INTEGER_VAL(max)},0);
	krk_pop();

	return OBJECT_VAL(output);
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct RangeIterator *

KRK_METHOD(rangeiterator,__init__,{
	METHOD_TAKES_EXACTLY(2);
	self->i = AS_INTEGER(argv[1]);
	self->max = AS_INTEGER(argv[2]);
	return argv[0];
})

KRK_METHOD(rangeiterator,__call__,{
	METHOD_TAKES_NONE();
	krk_integer_type i = self->i;
	if (i >= self->max) {
		return argv[0];
	} else {
		self->i = i + 1;
		return INTEGER_VAL(i);
	}
})

_noexport
void _createAndBind_rangeClass(void) {
	range = ADD_BASE_CLASS(vm.baseClasses->rangeClass, "range", vm.baseClasses->objectClass);
	range->allocSize = sizeof(struct Range);
	BIND_METHOD(range,__init__);
	BIND_METHOD(range,__iter__);
	BIND_METHOD(range,__repr__);
	KRK_DOC(range, "@brief range(max), range(min, max[, step]): "
		"An iterable object that produces numeric values. "
		"'min' is inclusive, 'max' is exclusive.");
	krk_finalizeClass(range);

	rangeiterator = ADD_BASE_CLASS(vm.baseClasses->rangeiteratorClass, "rangeiterator", vm.baseClasses->objectClass);
	rangeiterator->allocSize = sizeof(struct RangeIterator);
	BIND_METHOD(rangeiterator,__init__);
	BIND_METHOD(rangeiterator,__call__);
	krk_finalizeClass(rangeiterator);
}
