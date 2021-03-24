#include <limits.h>
#include <string.h>
#include <kuroko/memory.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/vm.h>

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
		printable = krk_callSimple(OBJECT_VAL(type->_tostr), 1, 0);
		if (!IS_STRING(printable)) return;
		fprintf(f, "%s", AS_CSTRING(printable));
	} else if (type->_reprer) {
		krk_push(printable);
		printable = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
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
				if (IS_FLOATING(printable)) fprintf(f, "%.16g", AS_FLOATING(printable));
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
			case KRK_OBJ_CODEOBJECT: fprintf(f, "<function %s>", AS_codeobject(printable)->name ? AS_codeobject(printable)->name->chars : "?"); break;
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
					((KrkNative*)AS_BOUND_METHOD(printable)->method)->name) : "(corrupt bound method)"); break;
			default: fprintf(f, "<%s>", krk_typeName(printable)); break;
		}
	}
}

int krk_valuesSame(KrkValue a, KrkValue b) {
	if (KRK_VAL_TYPE(a) != KRK_VAL_TYPE(b)) return 0;
	if (IS_OBJECT(a)) return AS_OBJECT(a) == AS_OBJECT(b);
	return krk_valuesEqual(a,b);
}

__attribute__((hot))
inline
int krk_valuesEqual(KrkValue a, KrkValue b) {
	if (KRK_VAL_TYPE(a) == KRK_VAL_TYPE(b)) {
		switch (KRK_VAL_TYPE(a)) {
			case KRK_VAL_BOOLEAN:  return AS_BOOLEAN(a) == AS_BOOLEAN(b);
			case KRK_VAL_NONE:     return 1; /* None always equals None */
			case KRK_VAL_KWARGS:   /* Equal if same number of args; may be useful for comparing sentinels (0) to arg lists. */
			case KRK_VAL_INTEGER:  return AS_INTEGER(a) == AS_INTEGER(b);
			case KRK_VAL_HANDLER:  krk_runtimeError(vm.exceptions->valueError,"Invalid value"); return 0;
			case KRK_VAL_OBJECT: {
				if (AS_OBJECT(a) == AS_OBJECT(b)) return 1;
			} break;
			default: break;
		}
	}
	if (IS_FLOATING(a) && IS_FLOATING(b)) return AS_FLOATING(a) == AS_FLOATING(b);

	if (IS_KWARGS(a) || IS_KWARGS(b)) return 0;

	if (!IS_OBJECT(a) && !IS_OBJECT(b)) {
		switch (KRK_VAL_TYPE(a)) {
			case KRK_VAL_INTEGER: {
				if (IS_BOOLEAN(b))       return AS_INTEGER(a) == AS_BOOLEAN(b);
				else if (IS_FLOATING(b)) return (double)AS_INTEGER(a) == AS_FLOATING(b);
				return 0;
			} break;
			case KRK_VAL_BOOLEAN: {
				if (IS_INTEGER(b))       return AS_INTEGER(a) == AS_BOOLEAN(b);
				else if (IS_FLOATING(b)) return (double)AS_INTEGER(a) == AS_FLOATING(b);
				return 0;
			} break;
			default:
				if (IS_FLOATING(a)) {
					if (IS_BOOLEAN(b)) return AS_FLOATING(a) == (double)AS_BOOLEAN(b);
					else if (IS_INTEGER(b)) return AS_FLOATING(a) == (double)AS_INTEGER(b);
					return 0;
				}
				break;
		}
	}

	if (IS_TUPLE(a) && IS_TUPLE(b)) {
		KrkTuple * self = AS_TUPLE(a);
		KrkTuple * them = AS_TUPLE(b);
		if (self->values.count != them->values.count || self->obj.hash != them->obj.hash) return 0;
		for (size_t i = 0; i < self->values.count; ++i) {
			if (!krk_valuesEqual(self->values.values[i], them->values.values[i])) return 0;
		}
		return 1;
	}

	KrkClass * type = krk_getType(a);
	if (type && type->_eq) {
		krk_push(a);
		krk_push(b);
		KrkValue result = krk_callSimple(OBJECT_VAL(type->_eq),2,0);
		if (IS_BOOLEAN(result)) return AS_BOOLEAN(result);
		return 0;
	}

	return 0;
}
