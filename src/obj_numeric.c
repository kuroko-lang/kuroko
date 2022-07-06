#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#include "private.h"

#undef bool

#define IS_NoneType(o) (IS_NONE(o))
#define AS_NoneType(o) ((char)0)

#define CURRENT_CTYPE krk_integer_type
#define CURRENT_NAME  self

FUNC_SIG(int,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_AT_MOST(2);
	if (argc < 2) return INTEGER_VAL(0);
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_INTEGER(argv[1]));
	if (IS_INTEGER(argv[1])) return argv[1];
	if (IS_STRING(argv[1])) {
		krk_integer_type _base = 10;
		if (argc > 2) {
			CHECK_ARG(2,int,krk_integer_type,base);
			_base = base;
		}
		KrkValue result = krk_parse_int(AS_CSTRING(argv[1]), AS_STRING(argv[1])->length, _base);
		if (IS_NONE(result)) {
			krk_push(argv[1]);
			KrkValue repred = krk_callDirect(vm.baseClasses->strClass->_reprer, 1);
			return krk_runtimeError(vm.exceptions->valueError, "invalid literal for int() with base " PRIkrk_int "%s%s",
				_base, IS_STRING(repred) ? ": " : "", IS_STRING(repred) ? AS_CSTRING(repred) : "");
		}
		return result;
	}
	if (IS_FLOATING(argv[1])) return INTEGER_VAL(AS_FLOATING(argv[1]));
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%s'", "int", krk_typeName(argv[1]));
}

KRK_Method(int,__str__) {
	char tmp[100];
	size_t l = snprintf(tmp, 100, PRIkrk_int, self);
	return OBJECT_VAL(krk_copyString(tmp, l));
}

KRK_Method(int,__int__) { return argv[0]; }
KRK_Method(int,__float__) { return FLOATING_VAL(self); }

KRK_Method(int,__chr__) {
	unsigned char bytes[5] = {0};
	size_t len = krk_codepointToBytes(self, bytes);
	return OBJECT_VAL(krk_copyString((char*)bytes, len));
}

KRK_Method(int,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) return self == AS_INTEGER(argv[1]);
	else if (IS_FLOATING(argv[1])) return self == AS_FLOATING(argv[1]);
	return NOTIMPL_VAL();
}

KRK_Method(int,__hash__) {
	return INTEGER_VAL((uint32_t)AS_INTEGER(argv[0]));
}

/**
 * We _could_ use the __builtin_XXX_overflow(_p) functions gcc+clang provide,
 * but let's just do this ourselves, I guess?
 *
 * We cheat: We only bother calculating + checking if both values would fit
 * in int32_t's. This ensures multiplication works fine.
 *
 * For any case where an int32_t would overflow, we do the 'long' operation
 * and then reduce if that still yields something that would fit in our 'int48'.
 */
#define OVERFLOW_CHECKED_INT_OPERATION(name,operator) \
	extern KrkValue krk_long_coerced_ ## name (krk_integer_type a, krk_integer_type b); \
	_noexport \
	KrkValue krk_int_op_ ## name (krk_integer_type a, krk_integer_type b) { \
		if (likely((int32_t)a == a && (int32_t)b == b)) { \
			int32_t result_one = a operator b; \
			int64_t result_two = a operator b; \
			if (likely(result_one == result_two)) return INTEGER_VAL(result_two); \
		} \
		return krk_long_coerced_ ## name (a, b); \
	}

OVERFLOW_CHECKED_INT_OPERATION(add,+)
OVERFLOW_CHECKED_INT_OPERATION(sub,-)
OVERFLOW_CHECKED_INT_OPERATION(mul,*)

#define BASIC_BIN_OP(name,operator) \
	KRK_Method(int,__ ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return krk_int_op_ ## name(self, AS_INTEGER(argv[1])); \
		else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL((double)self operator AS_FLOATING(argv[1])); \
		return NOTIMPL_VAL(); \
	} \
	KRK_Method(int,__r ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return krk_int_op_ ## name(AS_INTEGER(argv[1]), self); \
		else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) operator (double)self); \
		return NOTIMPL_VAL(); \
	}

#define INT_ONLY_BIN_OP(name,operator) \
	KRK_Method(int,__ ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(self operator AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	} \
	KRK_Method(int,__r ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(AS_INTEGER(argv[1]) operator self); \
		return NOTIMPL_VAL(); \
	}

#define COMPARE_OP(name,operator) \
	KRK_Method(int,__ ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return BOOLEAN_VAL(self operator AS_INTEGER(argv[1])); \
		else if (likely(IS_FLOATING(argv[1]))) return BOOLEAN_VAL((double)self operator AS_FLOATING(argv[1])); \
		return NOTIMPL_VAL(); \
	}

BASIC_BIN_OP(add,+)
BASIC_BIN_OP(sub,-)
BASIC_BIN_OP(mul,*)
INT_ONLY_BIN_OP(or,|)
INT_ONLY_BIN_OP(xor,^)
INT_ONLY_BIN_OP(and,&)

#define DEFER_TO_LONG(name) \
	extern KrkValue krk_long_coerced_ ## name (krk_integer_type a, krk_integer_type b); \
	KRK_Method(int,__ ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return krk_long_coerced_ ## name (self, AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	} \
	KRK_Method(int,__r ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return krk_long_coerced_ ## name (AS_INTEGER(argv[1]), self); \
		return NOTIMPL_VAL(); \
	}

DEFER_TO_LONG(lshift)
DEFER_TO_LONG(rshift)
DEFER_TO_LONG(pow)

COMPARE_OP(lt, <)
COMPARE_OP(gt, >)
COMPARE_OP(le, <=)
COMPARE_OP(ge, >=)

#undef BASIC_BIN_OP
#undef INT_ONLY_BIN_OP
#undef COMPARE_OP

KRK_Method(int,__truediv__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) {
		krk_integer_type b = AS_INTEGER(argv[1]);
		if (unlikely(b == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
		return FLOATING_VAL((double)self / (double)b);
	} else if (likely(IS_FLOATING(argv[1]))) {
		double b = AS_FLOATING(argv[1]);
		if (unlikely(b == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
		return FLOATING_VAL((double)self / b);
	}
	return NOTIMPL_VAL();
}

KRK_Method(int,__rtruediv__) {
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) / (double)self);
	else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) / (double)self);
	return NOTIMPL_VAL();
}

#ifdef __TINYC__
#include <math.h>
#define __builtin_floor floor
#endif

/**
 * These have been corrected to match the behavior with negatives
 * that Python produces, for compatibility, and also because that's
 * what our 'long' type does...
 */
static KrkValue _krk_int_div(krk_integer_type a, krk_integer_type b) {
	if (unlikely(b == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division or modulo by zero");
	if (a == 0) return INTEGER_VAL(0);
	int64_t abs_a = a < 0 ? -a : a;
	int64_t abs_b = b < 0 ? -b : b;
	if ((a < 0) != (b < 0)) {
		/* If signs don't match, the result is negative, and rounding down means away from 0... */
		int64_t res = -1 - (abs_a - 1) / abs_b;
		return INTEGER_VAL(res);
	}
	return INTEGER_VAL((abs_a / abs_b));
}

static KrkValue _krk_int_mod(krk_integer_type a, krk_integer_type b) {
	if (unlikely(b == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division or modulo by zero");
	if (a == 0) return INTEGER_VAL(0);
	int64_t abs_a = a < 0 ? -a : a;
	int64_t abs_b = b < 0 ? -b : b;
	int64_t res;
	if ((a < 0) != (b < 0)) {
		/* If quotient would be negative, then remainder is inverted against the divisor. */
		res = (abs_b - 1 - (abs_a - 1) % abs_b);
	} else {
		res = abs_a % abs_b;
	}
	/* Negative divisor always yields negative remainder, except when it's 0... */
	return INTEGER_VAL((b < 0) ? -res : res);

}

KRK_Method(int,__mod__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) return _krk_int_mod(self, AS_INTEGER(argv[1]));
	return NOTIMPL_VAL();
}

KRK_Method(int,__rmod__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) return _krk_int_mod(AS_INTEGER(argv[1]), self);
	return NOTIMPL_VAL();
}


KRK_Method(int,__floordiv__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) {
		return _krk_int_div(self,AS_INTEGER(argv[1]));
	} else if (likely(IS_FLOATING(argv[1]))) {
		double b = AS_FLOATING(argv[1]);
		if (unlikely(b == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
		return FLOATING_VAL(__builtin_floor((double)self / b));
	}
	return NOTIMPL_VAL();
}

KRK_Method(int,__rfloordiv__) {
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return _krk_int_div(AS_INTEGER(argv[1]), self);
	else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(__builtin_floor(AS_FLOATING(argv[1]) / (double)self));
	return NOTIMPL_VAL();
}

KRK_Method(int,__hex__) {
	METHOD_TAKES_NONE();
	char tmp[20];
	unsigned long long val = self < 0 ? -self : self;
	size_t len = snprintf(tmp, 20, "%s0x%llx", self < 0 ? "-" : "", val);
	return OBJECT_VAL(krk_copyString(tmp,len));
}

KRK_Method(int,__oct__) {
	METHOD_TAKES_NONE();
	char tmp[20];
	unsigned long long val = self < 0 ? -self : self;
	size_t len = snprintf(tmp, 20, "%s0o%llo", self < 0 ? "-" : "", val);
	return OBJECT_VAL(krk_copyString(tmp,len));
}

KRK_Method(int,__bin__) {
	METHOD_TAKES_NONE();
	unsigned long long val = self;
	if (self < 0) val = -val;

	struct StringBuilder sb = {0};

	if (!val) pushStringBuilder(&sb, '0');
	while (val) {
		pushStringBuilder(&sb, (val & 1) ? '1' : '0');
		val = val >> 1;
	}

	pushStringBuilder(&sb, 'b');
	pushStringBuilder(&sb, '0');
	if (self< 0) pushStringBuilder(&sb,'-');

	/* Flip it */
	for (size_t i = 0; i < sb.length / 2; ++i) {
		char t = sb.bytes[i];
		sb.bytes[i] = sb.bytes[sb.length - i - 1];
		sb.bytes[sb.length - i - 1] = t;
	}

	return finishStringBuilder(&sb);
}

KRK_Method(int,__invert__) {
	return INTEGER_VAL(~self);
}

KRK_Method(int,__neg__) {
	return INTEGER_VAL(-self);
}

KRK_Method(int,__abs__) {
	return self < 0 ? INTEGER_VAL(-self) : INTEGER_VAL(self);
}

KRK_Method(int,__pos__) {
	return argv[0];
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE double

#define trySlowMethod(name) do { \
	KrkClass * type = krk_getType(argv[1]); \
	KrkValue method; \
	while (type) { \
		if (krk_tableGet(&type->methods, name, &method)) { \
			krk_push(method); \
			krk_push(argv[1]); \
			return krk_callStack(1); \
		} \
		type = type->base; \
	} \
} while (0)

FUNC_SIG(float,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return FLOATING_VAL(0.0);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));

	trySlowMethod(vm.specialMethodNames[METHOD_FLOAT]);

	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%s'", "float", krk_typeName(argv[1]));
}

KRK_Method(float,__int__) { return INTEGER_VAL(self); }
KRK_Method(float,__float__) { return argv[0]; }

static int isDigits(const char * c) {
	while (*c) {
		if (*c < '0' || *c > '9') return 0;
		c++;
	}
	return 1;
}

KRK_Method(float,__str__) {
	char tmp[100];
	size_t l = snprintf(tmp, 97, "%.16g", self);
	if (!strstr(tmp,".") && isDigits(tmp)) {
		l = snprintf(tmp,100,"%.16g.0",self);
	}
	return OBJECT_VAL(krk_copyString(tmp, l));
}

KRK_Method(float,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) return self == (double)AS_INTEGER(argv[1]);
	else if (IS_FLOATING(argv[1])) return self == AS_FLOATING(argv[1]);
	return NOTIMPL_VAL();
}

KRK_Method(float,__hash__) {
	return INTEGER_VAL((uint32_t)self);
}

KRK_Method(float,__neg__) {
	return FLOATING_VAL(-self);
}

KRK_Method(float,__abs__) {
	return self < 0.0 ? FLOATING_VAL(-self) : INTEGER_VAL(self);
}

#define BASIC_BIN_OP(name,operator) \
	KRK_Method(float,__ ## name ## __) { \
		METHOD_TAKES_EXACTLY(1); \
		if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(self operator AS_FLOATING(argv[1])); \
		else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL(self operator (double)AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	} \
	KRK_Method(float,__r ## name ## __) { \
		METHOD_TAKES_EXACTLY(1); \
		if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) operator self); \
		else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) operator self); \
		return NOTIMPL_VAL(); \
	}

#define COMPARE_OP(name,operator) \
	KRK_Method(float,__ ## name ## __) { \
		METHOD_TAKES_EXACTLY(1); \
		if (likely(IS_FLOATING(argv[1]))) return BOOLEAN_VAL(self operator AS_FLOATING(argv[1])); \
		else if (likely(IS_INTEGER(argv[1]))) return BOOLEAN_VAL(self operator (double)AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	}

BASIC_BIN_OP(add,+)
BASIC_BIN_OP(sub,-)
BASIC_BIN_OP(mul,*)
COMPARE_OP(lt, <)
COMPARE_OP(gt, >)
COMPARE_OP(le, <=)
COMPARE_OP(ge, >=)

#undef BASIC_BIN_OP
#undef COMPARE_OP

KRK_Method(float,__truediv__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_FLOATING(argv[1]))) {
		double b = AS_FLOATING(argv[1]);
		if (unlikely(b == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
		return FLOATING_VAL(self / b);
	} else if (likely(IS_INTEGER(argv[1]))) {
		krk_integer_type b = AS_INTEGER(argv[1]);
		if (unlikely(b == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
		return FLOATING_VAL(self / (double)b);
	}
	return NOTIMPL_VAL();
}

KRK_Method(float,__rtruediv__) {
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
	else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) / self);
	else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) / self);
	return NOTIMPL_VAL();
}

KRK_Method(float,__floordiv__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) {
		krk_integer_type b = AS_INTEGER(argv[1]);
		if (unlikely(b == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
		return FLOATING_VAL(__builtin_floor(self / (double)b));
	} else if (IS_FLOATING(argv[1])) {
		double b = AS_FLOATING(argv[1]);
		if (unlikely(b == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
		return FLOATING_VAL(__builtin_floor(self / b));
	}
	return NOTIMPL_VAL();
}

KRK_Method(float,__rfloordiv__) {
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) / self);
	else if (IS_FLOATING(argv[1])) return FLOATING_VAL(__builtin_floor(AS_FLOATING(argv[1]) / self));
	return NOTIMPL_VAL();
}

KRK_Method(float,__pos__) {
	return argv[0];
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE krk_integer_type

FUNC_SIG(bool,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return BOOLEAN_VAL(0);
	return BOOLEAN_VAL(!krk_isFalsey(argv[1]));
}

KRK_Method(bool,__str__) {
	return OBJECT_VAL((self ? S("True") : S("False")));
}

FUNC_SIG(NoneType,__init__) {
	if (argc > 1) return krk_runtimeError(vm.exceptions->argumentError, "%s takes no arguments", "NoneType");
	return NONE_VAL();
}

KRK_Method(NoneType,__str__) {
	return OBJECT_VAL(S("None"));
}

KRK_Method(NoneType,__hash__) {
	return INTEGER_VAL((uint32_t)AS_INTEGER(argv[0]));
}

KRK_Method(NoneType,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_NONE(argv[1])) return BOOLEAN_VAL(1);
	return NOTIMPL_VAL();
}

#define IS_NotImplementedType(o) IS_NOTIMPL(o)
#define AS_NotImplementedType(o) (1)

FUNC_SIG(NotImplementedType,__init__) {
	if (argc > 1) return krk_runtimeError(vm.exceptions->argumentError, "%s takes no arguments", "NotImplementedType");
	return NOTIMPL_VAL();
}

KRK_Method(NotImplementedType,__str__) {
	return OBJECT_VAL(S("NotImplemented"));
}

KRK_Method(NotImplementedType,__hash__) {
	return INTEGER_VAL(0);
}

KRK_Method(NotImplementedType,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_NOTIMPL(argv[1])) return BOOLEAN_VAL(1);
	return NOTIMPL_VAL();
}

#undef BIND_METHOD
/* These class names conflict with C types, so we need to cheat a bit */
#define BIND_METHOD(klass,method) do { krk_defineNative(& _ ## klass->methods, #method, _ ## klass ## _ ## method); } while (0)
#define BIND_TRIPLET(klass,name) \
	BIND_METHOD(klass,__ ## name ## __); \
	BIND_METHOD(klass,__r ## name ## __); \
	krk_defineNative(&_ ## klass->methods,"__i" #name "__",_ ## klass ## ___ ## name ## __);
_noexport
void _createAndBind_numericClasses(void) {
	KrkClass * _int = ADD_BASE_CLASS(vm.baseClasses->intClass, "int", vm.baseClasses->objectClass);
	_int->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(int,__init__);
	BIND_METHOD(int,__str__);
	BIND_METHOD(int,__int__);
	BIND_METHOD(int,__chr__);
	BIND_METHOD(int,__float__);
	BIND_METHOD(int,__eq__);
	BIND_METHOD(int,__hash__);

	BIND_TRIPLET(int,add);
	BIND_TRIPLET(int,sub);
	BIND_TRIPLET(int,mul);
	BIND_TRIPLET(int,or);
	BIND_TRIPLET(int,xor);
	BIND_TRIPLET(int,and);
	BIND_TRIPLET(int,lshift);
	BIND_TRIPLET(int,rshift);
	BIND_TRIPLET(int,mod);
	BIND_TRIPLET(int,truediv);
	BIND_TRIPLET(int,floordiv);
	BIND_TRIPLET(int,pow);

	BIND_METHOD(int,__lt__);
	BIND_METHOD(int,__gt__);
	BIND_METHOD(int,__le__);
	BIND_METHOD(int,__ge__);

	BIND_METHOD(int,__hex__);
	BIND_METHOD(int,__oct__);
	BIND_METHOD(int,__bin__);
	BIND_METHOD(int,__invert__);
	BIND_METHOD(int,__neg__);
	BIND_METHOD(int,__abs__);
	BIND_METHOD(int,__pos__);

	krk_defineNative(&_int->methods, "__repr__", FUNC_NAME(int,__str__));
	krk_finalizeClass(_int);
	KRK_DOC(_int, "Convert a number or string type to an integer representation.");

	KrkClass * _float = ADD_BASE_CLASS(vm.baseClasses->floatClass, "float", vm.baseClasses->objectClass);
	_float->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(float,__init__);
	BIND_METHOD(float,__int__);
	BIND_METHOD(float,__float__);
	BIND_METHOD(float,__str__);
	BIND_METHOD(float,__eq__);
	BIND_METHOD(float,__hash__);
	BIND_TRIPLET(float,add);
	BIND_TRIPLET(float,sub);
	BIND_TRIPLET(float,mul);
	BIND_TRIPLET(float,truediv);
	BIND_TRIPLET(float,floordiv);
	BIND_METHOD(float,__lt__);
	BIND_METHOD(float,__gt__);
	BIND_METHOD(float,__le__);
	BIND_METHOD(float,__ge__);
	BIND_METHOD(float,__neg__);
	BIND_METHOD(float,__abs__);
	BIND_METHOD(float,__pos__);
	krk_defineNative(&_float->methods, "__repr__", FUNC_NAME(float,__str__));
	krk_finalizeClass(_float);
	KRK_DOC(_float, "Convert a number or string type to a float representation.");

	KrkClass * _bool = ADD_BASE_CLASS(vm.baseClasses->boolClass, "bool", vm.baseClasses->intClass);
	_bool->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(bool,__init__);
	BIND_METHOD(bool,__str__);
	krk_defineNative(&_bool->methods, "__repr__", FUNC_NAME(bool,__str__));
	krk_finalizeClass(_bool);
	KRK_DOC(_bool, "Returns False if the argument is 'falsey', otherwise True.");

	KrkClass * _NoneType = ADD_BASE_CLASS(vm.baseClasses->noneTypeClass, "NoneType", vm.baseClasses->objectClass);
	_NoneType->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(NoneType, __init__);
	BIND_METHOD(NoneType, __str__);
	BIND_METHOD(NoneType, __hash__);
	BIND_METHOD(NoneType, __eq__);
	krk_defineNative(&_NoneType->methods, "__repr__", FUNC_NAME(NoneType,__str__));
	krk_finalizeClass(_NoneType);

	KrkClass * _NotImplementedType = ADD_BASE_CLASS(vm.baseClasses->notImplClass, "NotImplementedType", vm.baseClasses->objectClass);
	_NotImplementedType->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(NotImplementedType, __init__);
	BIND_METHOD(NotImplementedType, __str__);
	BIND_METHOD(NotImplementedType, __hash__);
	BIND_METHOD(NotImplementedType, __eq__);
	krk_defineNative(&_NotImplementedType->methods, "__repr__", FUNC_NAME(NotImplementedType,__str__));
	krk_finalizeClass(_NotImplementedType);

	krk_attachNamedValue(&vm.builtins->fields, "NotImplemented", NOTIMPL_VAL());
}
