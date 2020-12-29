#include <string.h>
#include "memory.h"
#include "value.h"
#include "object.h"

void krk_initValueArray(KrkValueArray * array) {
	array->values = NULL;
	array->capacity = 0;
	array->count = 0;
}

void krk_writeValueArray(KrkValueArray * array, KrkValue value) {
	if (array->capacity < array->count + 1) {
		int old = array->capacity;
		array->capacity = GROW_CAPACITY(old);
		array->values = GROW_ARRAY(KrkValue, array->values, old, array->capacity);
	}

	array->values[array->count] = value;
	array->count++;
}

void krk_freeValueArray(KrkValueArray * array) {
	FREE_ARRAY(KrkValue, array->values, array->capacity);
	krk_initValueArray(array);
}

void krk_printValue(FILE * f, KrkValue value) {
	if (IS_FLOATING(value)) {
		fprintf(f, "%g", AS_FLOATING(value));
	} else if (IS_INTEGER(value)) {
		fprintf(f, "%ld", (long)AS_INTEGER(value));
	} else if (IS_BOOLEAN(value)) {
		fprintf(f, "%s", AS_BOOLEAN(value) ? "True" : "False");
	} else if (IS_NONE(value)) {
		fprintf(f, "None");
	} else if (IS_HANDLER(value)) {
		fprintf(f, "{try->%ld}", AS_HANDLER(value));
	} else if (IS_OBJECT(value)) {
		krk_printObject(f, value);
	}
}

int krk_valuesEqual(KrkValue a, KrkValue b) {
	if (a.type != b.type) return 0; /* uh, maybe not, this is complicated */

	switch (a.type) {
		case VAL_BOOLEAN:  return AS_BOOLEAN(a) == AS_BOOLEAN(b);
		case VAL_NONE:     return 1; /* None always equals None */
		case VAL_INTEGER:  return AS_INTEGER(a) == AS_INTEGER(b);
		case VAL_FLOATING: return AS_FLOATING(a) == AS_FLOATING(b);
		case VAL_HANDLER: {
			fprintf(stderr, "Attempted to compare a value to an exception handler. VM leaked a stack value.\n");
			exit(1);
		}
		case VAL_OBJECT: {
			if (IS_STRING(a) && IS_STRING(b)) return AS_OBJECT(a) == AS_OBJECT(b);
			/* If their pointers are equal, assume they are always equivalent */
			if (AS_OBJECT(a) == AS_OBJECT(b)) return 1;
			/* TODO: __eq__ */
			return 0;
		}
		default: return 0;
	}
}
