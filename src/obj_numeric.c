#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#undef IS_int
#undef IS_bool
#undef IS_float
#undef bool

#define IS_int(o) (IS_INTEGER(o) || krk_isInstanceOf(o,vm.baseClasses->intClass))
#define IS_bool(o) (IS_BOOLEAN(o) || krk_isInstanceOf(o,vm.baseClasses->boolClass))
#define IS_float(o) (IS_FLOATING(o) || krk_isInstanceOf(o,vm.baseClasses->floatClass))

#define IS_NoneType(o) (IS_NONE(o))
#define AS_NoneType(o) ((char)0)

#define CURRENT_CTYPE krk_integer_type
#define CURRENT_NAME  self

KRK_METHOD(int,__init__,{
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return INTEGER_VAL(0);
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_INTEGER(argv[1]));
	if (IS_INTEGER(argv[1])) return argv[1];
	if (IS_STRING(argv[1])) return krk_string_int(argc-1,&argv[1],0);
	if (IS_FLOATING(argv[1])) return INTEGER_VAL(AS_FLOATING(argv[1]));
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%s'", "int", krk_typeName(argv[1]));
})

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
	if (IS_INTEGER(argv[1])) return self == AS_INTEGER(argv[1]);
	else if (IS_FLOATING(argv[1])) return self == AS_FLOATING(argv[1]);
	return NOTIMPL_VAL();
})

KRK_METHOD(int,__hash__,{
	return INTEGER_VAL((uint32_t)AS_INTEGER(argv[0]));
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE double

KRK_METHOD(float,__init__,{
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return FLOATING_VAL(0.0);
	if (IS_STRING(argv[1])) return krk_string_float(1,&argv[1],0);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%s'", "float", krk_typeName(argv[1]));
})

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

#undef CURRENT_CTYPE
#define CURRENT_CTYPE krk_integer_type

KRK_METHOD(bool,__init__,{
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return BOOLEAN_VAL(0);
	return BOOLEAN_VAL(!krk_isFalsey(argv[1]));
})

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
	BIND_METHOD(int,__init__);
	BIND_METHOD(int,__str__);
	BIND_METHOD(int,__int__);
	BIND_METHOD(int,__chr__);
	BIND_METHOD(int,__float__);
	BIND_METHOD(int,__eq__);
	BIND_METHOD(int,__hash__);
	krk_defineNative(&_int->methods, "__repr__", FUNC_NAME(int,__str__));
	krk_finalizeClass(_int);
	KRK_DOC(_int, "Convert a number or string type to an integer representation.");

	KrkClass * _float = ADD_BASE_CLASS(vm.baseClasses->floatClass, "float", vm.baseClasses->objectClass);
	BIND_METHOD(float,__init__);
	BIND_METHOD(float,__int__);
	BIND_METHOD(float,__float__);
	BIND_METHOD(float,__str__);
	BIND_METHOD(float,__eq__);
	BIND_METHOD(float,__hash__);
	krk_defineNative(&_float->methods, "__repr__", FUNC_NAME(float,__str__));
	krk_finalizeClass(_float);
	KRK_DOC(_float, "Convert a number or string type to a float representation.");

	KrkClass * _bool = ADD_BASE_CLASS(vm.baseClasses->boolClass, "bool", vm.baseClasses->intClass);
	BIND_METHOD(bool,__init__);
	BIND_METHOD(bool,__str__);
	krk_defineNative(&_bool->methods, "__repr__", FUNC_NAME(bool,__str__));
	krk_finalizeClass(_bool);
	KRK_DOC(_bool, "Returns False if the argument is 'falsey', otherwise True.");

	KrkClass * _NoneType = ADD_BASE_CLASS(vm.baseClasses->noneTypeClass, "NoneType", vm.baseClasses->objectClass);
	BIND_METHOD(NoneType, __str__);
	BIND_METHOD(NoneType, __hash__);
	krk_defineNative(&_NoneType->methods, "__repr__", FUNC_NAME(NoneType,__str__));
	krk_finalizeClass(_NoneType);

	KrkClass * _NotImplementedType = ADD_BASE_CLASS(vm.baseClasses->notImplClass, "NotImplementedType", vm.baseClasses->objectClass);
	BIND_METHOD(NotImplementedType, __str__);
	BIND_METHOD(NotImplementedType, __hash__);
	krk_defineNative(&_NotImplementedType->methods, "__repr__", FUNC_NAME(NotImplementedType,__str__));
	krk_finalizeClass(_NotImplementedType);

	krk_attachNamedValue(&vm.builtins->fields, "NotImplemented", NOTIMPL_VAL());
}
