#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#undef bool

#define IS_NoneType(o) (IS_NONE(o))
#define AS_NoneType(o) ((char)0)

#define CURRENT_CTYPE krk_integer_type
#define CURRENT_NAME  self

FUNC_SIG(int,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return INTEGER_VAL(0);
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_INTEGER(argv[1]));
	if (IS_INTEGER(argv[1])) return argv[1];
	if (IS_STRING(argv[1])) return krk_string_int(argc-1,&argv[1],0);
	if (IS_FLOATING(argv[1])) return INTEGER_VAL(AS_FLOATING(argv[1]));
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%s'", "int", krk_typeName(argv[1]));
}

KRK_METHOD(int,__str__,{
	char tmp[100];
	size_t l = snprintf(tmp, 100, PRIkrk_int, self);
	return OBJECT_VAL(krk_copyString(tmp, l));
})

KRK_METHOD(int,__int__,{ return argv[0]; })
KRK_METHOD(int,__float__,{ return FLOATING_VAL(self); })

KRK_METHOD(int,__chr__,{
	unsigned char bytes[5] = {0};
	size_t len = krk_codepointToBytes(self, bytes);
	return OBJECT_VAL(krk_copyString((char*)bytes, len));
})

KRK_METHOD(int,__eq__,{
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) return self == AS_INTEGER(argv[1]);
	else if (IS_FLOATING(argv[1])) return self == AS_FLOATING(argv[1]);
	return NOTIMPL_VAL();
})

KRK_METHOD(int,__hash__,{
	return INTEGER_VAL((uint32_t)AS_INTEGER(argv[0]));
})

#define BASIC_BIN_OP(name,operator) \
	KRK_METHOD(int,__ ## name ## __,{ \
		if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(self operator AS_INTEGER(argv[1])); \
		else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL((double)self operator AS_FLOATING(argv[1])); \
		return NOTIMPL_VAL(); \
	}) \
	KRK_METHOD(int,__r ## name ## __,{ \
		if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(AS_INTEGER(argv[1]) operator self); \
		else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) operator (double)self); \
		return NOTIMPL_VAL(); \
	})

#define INT_ONLY_BIN_OP(name,operator) \
	KRK_METHOD(int,__ ## name ## __,{ \
		if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(self operator AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	}) \
	KRK_METHOD(int,__r ## name ## __,{ \
		if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(AS_INTEGER(argv[1]) operator self); \
		return NOTIMPL_VAL(); \
	})

#define COMPARE_OP(name,operator) \
	KRK_METHOD(int,__ ## name ## __,{ \
		if (likely(IS_INTEGER(argv[1]))) return BOOLEAN_VAL(self operator AS_INTEGER(argv[1])); \
		else if (likely(IS_FLOATING(argv[1]))) return BOOLEAN_VAL((double)self operator AS_FLOATING(argv[1])); \
		return NOTIMPL_VAL(); \
	})

BASIC_BIN_OP(add,+)
BASIC_BIN_OP(sub,-)
BASIC_BIN_OP(mul,*)
INT_ONLY_BIN_OP(or,|)
INT_ONLY_BIN_OP(xor,^)
INT_ONLY_BIN_OP(and,&)
INT_ONLY_BIN_OP(lshift,<<)
INT_ONLY_BIN_OP(rshift,>>)

KRK_METHOD(int,__mod__,{
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) {
		if (unlikely(AS_INTEGER(argv[1]) == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer modulo by zero");
		return INTEGER_VAL(self % AS_INTEGER(argv[1]));
	}
	return NOTIMPL_VAL();
})

KRK_METHOD(int,__rmod__,{
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer modulo by zero");
	if (likely(IS_INTEGER(argv[1]))) {
		return INTEGER_VAL(AS_INTEGER(argv[1]) % self);
	}
	return NOTIMPL_VAL();
})

COMPARE_OP(lt, <)
COMPARE_OP(gt, >)
COMPARE_OP(le, <=)
COMPARE_OP(ge, >=)

#undef BASIC_BIN_OP
#undef INT_ONLY_BIN_OP
#undef COMPARE_OP

KRK_METHOD(int,__truediv__,{
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
})

KRK_METHOD(int,__rtruediv__,{
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) / (double)self);
	else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) / (double)self);
	return NOTIMPL_VAL();
})

#ifdef __TINYC__
#include <math.h>
#define __builtin_floor floor
#endif

KRK_METHOD(int,__floordiv__,{
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) {
		krk_integer_type b = AS_INTEGER(argv[1]);
		if (unlikely(b == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
		return INTEGER_VAL(self / b);
	} else if (likely(IS_FLOATING(argv[1]))) {
		double b = AS_FLOATING(argv[1]);
		if (unlikely(b == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
		return FLOATING_VAL(__builtin_floor((double)self / b));
	}
	return NOTIMPL_VAL();
})

KRK_METHOD(int,__rfloordiv__,{
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return INTEGER_VAL(AS_INTEGER(argv[1]) / self);
	else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(__builtin_floor(AS_FLOATING(argv[1]) / (double)self));
	return NOTIMPL_VAL();
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE double

FUNC_SIG(float,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return FLOATING_VAL(0.0);
	if (IS_STRING(argv[1])) return krk_string_float(1,&argv[1],0);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%s'", "float", krk_typeName(argv[1]));
}

KRK_METHOD(float,__int__,{ return INTEGER_VAL(self); })
KRK_METHOD(float,__float__,{ return argv[0]; })

static int isDigits(const char * c) {
	while (*c) {
		if (*c < '0' || *c > '9') return 0;
		c++;
	}
	return 1;
}

KRK_METHOD(float,__str__,{
	char tmp[100];
	size_t l = snprintf(tmp, 97, "%.16g", self);
	if (!strstr(tmp,".") && isDigits(tmp)) {
		l = snprintf(tmp,100,"%.16g.0",self);
	}
	return OBJECT_VAL(krk_copyString(tmp, l));
})

KRK_METHOD(float,__eq__,{
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) return self == (double)AS_INTEGER(argv[1]);
	else if (IS_FLOATING(argv[1])) return self == AS_FLOATING(argv[1]);
	return NOTIMPL_VAL();
})

KRK_METHOD(float,__hash__,{
	return INTEGER_VAL((uint32_t)self);
})

#define BASIC_BIN_OP(name,operator) \
	KRK_METHOD(float,__ ## name ## __,{ \
		METHOD_TAKES_EXACTLY(1); \
		if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(self operator AS_FLOATING(argv[1])); \
		else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL(self operator (double)AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	}) \
	KRK_METHOD(float,__r ## name ## __,{ \
		METHOD_TAKES_EXACTLY(1); \
		if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) operator self); \
		else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) operator self); \
		return NOTIMPL_VAL(); \
	})

#define COMPARE_OP(name,operator) \
	KRK_METHOD(float,__ ## name ## __,{ \
		METHOD_TAKES_EXACTLY(1); \
		if (likely(IS_FLOATING(argv[1]))) return BOOLEAN_VAL(self operator AS_FLOATING(argv[1])); \
		else if (likely(IS_INTEGER(argv[1]))) return BOOLEAN_VAL(self operator (double)AS_INTEGER(argv[1])); \
		return NOTIMPL_VAL(); \
	})

BASIC_BIN_OP(add,+)
BASIC_BIN_OP(sub,-)
BASIC_BIN_OP(mul,*)
COMPARE_OP(lt, <)
COMPARE_OP(gt, >)
COMPARE_OP(le, <=)
COMPARE_OP(ge, >=)

#undef BASIC_BIN_OP
#undef COMPARE_OP

KRK_METHOD(float,__truediv__,{
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
})

KRK_METHOD(float,__rtruediv__,{
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
	else if (likely(IS_FLOATING(argv[1]))) return FLOATING_VAL(AS_FLOATING(argv[1]) / self);
	else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) / self);
	return NOTIMPL_VAL();
})

KRK_METHOD(float,__floordiv__,{
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
})

KRK_METHOD(float,__rfloordiv__,{
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return FLOATING_VAL((double)AS_INTEGER(argv[1]) / self);
	else if (IS_FLOATING(argv[1])) return FLOATING_VAL(__builtin_floor(AS_FLOATING(argv[1]) / self));
	return NOTIMPL_VAL();
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE krk_integer_type

FUNC_SIG(bool,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return BOOLEAN_VAL(0);
	return BOOLEAN_VAL(!krk_isFalsey(argv[1]));
}

KRK_METHOD(bool,__str__,{
	return OBJECT_VAL((self ? S("True") : S("False")));
})

KRK_METHOD(NoneType,__str__,{
	return OBJECT_VAL(S("None"));
})

KRK_METHOD(NoneType,__hash__,{
	return INTEGER_VAL((uint32_t)AS_INTEGER(argv[0]));
})

#define IS_NotImplementedType(o) IS_NOTIMPL(o)
#define AS_NotImplementedType(o) (1)
KRK_METHOD(NotImplementedType,__str__,{
	return OBJECT_VAL(S("NotImplemented"));
})

KRK_METHOD(NotImplementedType,__hash__,{
	return INTEGER_VAL(0);
})

#undef BIND_METHOD
#define BIND_METHOD(klass,method) do { krk_defineNative(& _ ## klass->methods, #method, _ ## klass ## _ ## method); } while (0)
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

#define BIND_TRIPLET(name) \
	BIND_METHOD(int,__ ## name ## __); \
	BIND_METHOD(int,__r ## name ## __); \
	krk_defineNative(&_int->methods,"__i" #name "__",_int___ ## name ## __);
	BIND_TRIPLET(add);
	BIND_TRIPLET(sub);
	BIND_TRIPLET(mul);
	BIND_TRIPLET(or);
	BIND_TRIPLET(xor);
	BIND_TRIPLET(and);
	BIND_TRIPLET(lshift);
	BIND_TRIPLET(rshift);
	BIND_TRIPLET(mod);
	BIND_TRIPLET(truediv);
	BIND_TRIPLET(floordiv);
#undef BIND_TRIPLET

	BIND_METHOD(int,__lt__);
	BIND_METHOD(int,__gt__);
	BIND_METHOD(int,__le__);
	BIND_METHOD(int,__ge__);

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
#define BIND_TRIPLET(name) \
	BIND_METHOD(float,__ ## name ## __); \
	BIND_METHOD(float,__r ## name ## __); \
	krk_defineNative(&_float->methods,"__i" #name "__",_float___ ## name ## __);
	BIND_TRIPLET(add);
	BIND_TRIPLET(sub);
	BIND_TRIPLET(mul);
	BIND_TRIPLET(truediv);
	BIND_TRIPLET(floordiv);
#undef BIND_TRIPLET
	BIND_METHOD(float,__lt__);
	BIND_METHOD(float,__gt__);
	BIND_METHOD(float,__le__);
	BIND_METHOD(float,__ge__);
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
	BIND_METHOD(NoneType, __str__);
	BIND_METHOD(NoneType, __hash__);
	krk_defineNative(&_NoneType->methods, "__repr__", FUNC_NAME(NoneType,__str__));
	krk_finalizeClass(_NoneType);

	KrkClass * _NotImplementedType = ADD_BASE_CLASS(vm.baseClasses->notImplClass, "NotImplementedType", vm.baseClasses->objectClass);
	_NotImplementedType->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(NotImplementedType, __str__);
	BIND_METHOD(NotImplementedType, __hash__);
	krk_defineNative(&_NotImplementedType->methods, "__repr__", FUNC_NAME(NotImplementedType,__str__));
	krk_finalizeClass(_NotImplementedType);

	krk_attachNamedValue(&vm.builtins->fields, "NotImplemented", NOTIMPL_VAL());
}
