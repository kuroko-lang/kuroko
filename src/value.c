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

void krk_printValue(FILE * f, KrkValue printable) {
	KrkClass * type = krk_getType(printable);
	if (type->_tostr) {
		krk_push(printable);
		printable = krk_callDirect(type->_tostr, 1);
		if (!IS_STRING(printable)) return;
		fprintf(f, "%s", AS_CSTRING(printable));
	} else if (type->_reprer) {
		krk_push(printable);
		printable = krk_callDirect(type->_reprer, 1);
		if (!IS_STRING(printable)) return;
		fprintf(f, "%s", AS_CSTRING(printable));
	} else {
		fprintf(f, "%s", krk_typeName(printable));
	}
}

#define STRING_DEBUG_TRUNCATE 50

void krk_printValueSafe(FILE * f, KrkValue printable) {
	if (!IS_OBJECT(printable)) {
		switch (KRK_VAL_TYPE(printable)) {
			case KRK_VAL_INTEGER:  fprintf(f, PRIkrk_int, AS_INTEGER(printable)); break;
			case KRK_VAL_BOOLEAN:  fprintf(f, "%s", AS_BOOLEAN(printable) ? "True" : "False"); break;
			case KRK_VAL_NONE:     fprintf(f, "None"); break;
			case KRK_VAL_HANDLER:
				switch (AS_HANDLER_TYPE(printable)) {
					case OP_PUSH_TRY:      fprintf(f, "{try->%d}",     (int)AS_HANDLER_TARGET(printable)); break;
					case OP_PUSH_WITH:     fprintf(f, "{with->%d}",    (int)AS_HANDLER_TARGET(printable)); break;
					case OP_RAISE:         fprintf(f, "{raise<-%d}",   (int)AS_HANDLER_TARGET(printable)); break;
					case OP_FILTER_EXCEPT: fprintf(f, "{except<-%d}",  (int)AS_HANDLER_TARGET(printable)); break;
					case OP_BEGIN_FINALLY: fprintf(f, "{finally<-%d}", (int)AS_HANDLER_TARGET(printable)); break;
					case OP_RETURN:        fprintf(f, "{return<-%d}",  (int)AS_HANDLER_TARGET(printable)); break;
					case OP_END_FINALLY:   fprintf(f, "{end<-%d}",     (int)AS_HANDLER_TARGET(printable)); break;
					case OP_EXIT_LOOP:     fprintf(f, "{exit<-%d}",    (int)AS_HANDLER_TARGET(printable)); break;
				}
				break;
			case KRK_VAL_KWARGS: {
				if (AS_INTEGER(printable) == KWARGS_SINGLE) {
					fprintf(f, "{unpack single}");
				} else if (AS_INTEGER(printable) == KWARGS_LIST) {
					fprintf(f, "{unpack list}");
				} else if (AS_INTEGER(printable) == KWARGS_DICT) {
					fprintf(f, "{unpack dict}");
				} else if (AS_INTEGER(printable) == KWARGS_NIL) {
					fprintf(f, "{unpack nil}");
				} else if (AS_INTEGER(printable) == KWARGS_UNSET) {
					fprintf(f, "{unset default}");
				} else {
					fprintf(f, "{sentinel=" PRIkrk_int "}",AS_INTEGER(printable));
				}
				break;
			}
			default:
#ifndef KRK_NO_FLOAT
				if (IS_FLOATING(printable)) fprintf(f, "%.16g", AS_FLOATING(printable));
#endif
				break;
		}
	} else if (IS_STRING(printable)) {
		fprintf(f, "'");
		/*
		 * Print at most STRING_DEBUG_TRUNCATE characters, as bytes, escaping anything not ASCII.
		 * See also str.__repr__ which does something similar with escape sequences, but this
		 * is a dumber, safer, and slightly faster approach.
		 */
		for (size_t c = 0; c < AS_STRING(printable)->length && c < STRING_DEBUG_TRUNCATE; ++c) {
			unsigned char byte = (unsigned char)AS_CSTRING(printable)[c];
			switch (byte) {
				case '\\': fprintf(f, "\\\\"); break;
				case '\n': fprintf(f, "\\n"); break;
				case '\r': fprintf(f, "\\r"); break;
				case '\'': fprintf(f, "\\'"); break;
				default: {
					if (byte < ' ' || byte > '~') {
						fprintf(f, "\\x%02x", byte);
					} else {
						fprintf(f, "%c", byte);
					}
					break;
				}
			}
		}
		if (AS_STRING(printable)->length > STRING_DEBUG_TRUNCATE) {
			fprintf(f,"...");
		}
		fprintf(f,"'");
	} else {
		switch (AS_OBJECT(printable)->type) {
			case KRK_OBJ_CODEOBJECT: fprintf(f, "<codeobject %s>", AS_codeobject(printable)->name ? AS_codeobject(printable)->name->chars : "?"); break;
			case KRK_OBJ_CLASS: fprintf(f, "<class %s>", AS_CLASS(printable)->name ? AS_CLASS(printable)->name->chars : "?"); break;
			case KRK_OBJ_INSTANCE: fprintf(f, "<instance of %s>", AS_INSTANCE(printable)->_class->name->chars); break;
			case KRK_OBJ_NATIVE: fprintf(f, "<nativefn %s>", ((KrkNative*)AS_OBJECT(printable))->name); break;
			case KRK_OBJ_CLOSURE: fprintf(f, "<function %s>", AS_CLOSURE(printable)->function->name->chars); break;
			case KRK_OBJ_BYTES: fprintf(f, "<bytes of len %ld>", (long)AS_BYTES(printable)->length); break;
			case KRK_OBJ_TUPLE: {
				fprintf(f, "(");
				for (size_t i = 0; i < AS_TUPLE(printable)->values.count; ++i) {
					krk_printValueSafe(f, AS_TUPLE(printable)->values.values[i]);
					if (i + 1 != AS_TUPLE(printable)->values.count) {
						fprintf(f, ",");
					}
				}
				fprintf(f, ")");
			} break;
			case KRK_OBJ_BOUND_METHOD: fprintf(f, "<method %s>",
				AS_BOUND_METHOD(printable)->method ? (
				AS_BOUND_METHOD(printable)->method->type == KRK_OBJ_CLOSURE ? ((KrkClosure*)AS_BOUND_METHOD(printable)->method)->function->name->chars :
					(AS_BOUND_METHOD(printable)->method->type == KRK_OBJ_NATIVE ? ((KrkNative*)AS_BOUND_METHOD(printable)->method)->name : "(unknown)")) : "(corrupt bound method)"); break;
			default: fprintf(f, "<%s>", krk_typeName(printable)); break;
		}
	}
}

/**
 * Identity really should be the simple...
 */
int krk_valuesSame(KrkValue a, KrkValue b) {
	return a == b;
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
			return a == b;
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

__attribute__((hot))
int krk_valuesSameOrEqual(KrkValue a, KrkValue b) {
	if (a == b) return 1;
	uint16_t val_a = KRK_VAL_TYPE(a);
	uint16_t val_b = KRK_VAL_TYPE(b);
	return (val_a == val_b)
		? _krk_same_type_equivalence_b(val_a, a, b)
		: _krk_diff_type_equivalence(val_a, val_b, a, b);
}

__attribute__((hot))
int krk_valuesEqual(KrkValue a, KrkValue b) {
	uint16_t val_a = KRK_VAL_TYPE(a);
	uint16_t val_b = KRK_VAL_TYPE(b);
	return (val_a == val_b)
		? _krk_same_type_equivalence(val_a,a,b)
		: _krk_diff_type_equivalence(val_a,val_b,a,b);
}
