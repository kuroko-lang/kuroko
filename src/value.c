#include <limits.h>
#include <string.h>
#include <kuroko/memory.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

#include "opcode_enum.h"

void krk_initValueArray(KrkValueArray * array) {
	array->values = NULL;
	array->capacity = 0;
	array->count = 0;
}

void krk_writeValueArray(KrkValueArray * array, KrkValue value) {
	if (array->capacity < array->count + 1) {
		int old = array->capacity;
		array->capacity = KRK_GROW_CAPACITY(old);
		array->values = KRK_GROW_ARRAY(KrkValue, array->values, old, array->capacity);
	}

	array->values[array->count] = value;
	array->count++;
}

void krk_freeValueArray(KrkValueArray * array) {
	KRK_FREE_ARRAY(KrkValue, array->values, array->capacity);
	krk_initValueArray(array);
}

static inline int _krk_method_equivalence(KrkValue a, KrkValue b) {
	KrkClass * type = krk_getType(a);
	if (likely(type && type->_eq)) {
		krk_push(a);
		krk_push(b);
		KrkValue result = krk_callDirect(type->_eq,2);
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 0;
		if (IS_BOOLEAN(result)) return AS_BOOLEAN(result);
		if (!IS_NOTIMPL(result)) return !krk_isFalsey(result);
	}

	type = krk_getType(b);
	if (type && type->_eq) {
		krk_push(b);
		krk_push(a);
		KrkValue result = krk_callDirect(type->_eq,2);
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 0;
		if (IS_BOOLEAN(result)) return AS_BOOLEAN(result);
		if (!IS_NOTIMPL(result)) return !krk_isFalsey(result);
	}

	return 0;
}

static inline int _krk_same_type_equivalence(uint16_t valtype, KrkValue a, KrkValue b) {
	switch (valtype) {
		case KRK_VAL_BOOLEAN:
		case KRK_VAL_INTEGER:
		case KRK_VAL_NONE:
		case KRK_VAL_NOTIMPL:
		case KRK_VAL_KWARGS:
		case KRK_VAL_HANDLER:
			return krk_valuesSame(a,b);
		case KRK_VAL_OBJECT:
		default:
			return _krk_method_equivalence(a,b);
	}
}

static inline int _krk_same_type_equivalence_b(uint16_t valtype, KrkValue a, KrkValue b) {
	switch (valtype) {
		case KRK_VAL_BOOLEAN:
		case KRK_VAL_INTEGER:
		case KRK_VAL_NONE:
		case KRK_VAL_NOTIMPL:
		case KRK_VAL_KWARGS:
		case KRK_VAL_HANDLER:
			return 0;
		case KRK_VAL_OBJECT:
		default:
			return _krk_method_equivalence(a,b);
	}
}

static inline int _krk_diff_type_equivalence(uint16_t val_a, uint16_t val_b, KrkValue a, KrkValue b) {
	/* We do not want to let KWARGS leak to anything needs to, eg., examine types. */
	if (val_b == KRK_VAL_KWARGS || val_a == KRK_VAL_KWARGS) return 0;

	/* Fall back to methods */
	return _krk_method_equivalence(a,b);
}

_hot
int krk_valuesSameOrEqual(KrkValue a, KrkValue b) {
	if (krk_valuesSame(a,b)) return 1;
	uint16_t val_a = KRK_VAL_TYPE(a);
	uint16_t val_b = KRK_VAL_TYPE(b);
	return (val_a == val_b)
		? _krk_same_type_equivalence_b(val_a, a, b)
		: _krk_diff_type_equivalence(val_a, val_b, a, b);
}

_hot
int krk_valuesEqual(KrkValue a, KrkValue b) {
	uint16_t val_a = KRK_VAL_TYPE(a);
	uint16_t val_b = KRK_VAL_TYPE(b);
	return (val_a == val_b)
		? _krk_same_type_equivalence(val_a,a,b)
		: _krk_diff_type_equivalence(val_a,val_b,a,b);
}
