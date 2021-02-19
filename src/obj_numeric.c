#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#undef IS_int
#undef IS_bool
#undef IS_float

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
	if (IS_INTEGER(argv[1])) return argv[1];
	if (IS_STRING(argv[1])) return krk_string_int(argc-1,&argv[1],0);
	if (IS_FLOATING(argv[1])) return INTEGER_VAL(AS_FLOATING(argv[1]));
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "int() argument must be a string or a number, not '%s'", krk_typeName(argv[1]));
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

#undef CURRENT_CTYPE
#define CURRENT_CTYPE double

KRK_METHOD(float,__init__,{
	METHOD_TAKES_AT_MOST(1);
	if (argc < 2) return FLOATING_VAL(0.0);
	if (argc > 2) return krk_runtimeError(vm.exceptions->argumentError, "float() takes at most 1 argument");
	if (IS_STRING(argv[1])) return krk_string_float(1,&argv[1],0);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions->typeError, "float() argument must be a string or a number, not '%s'", krk_typeName(argv[1]));
})

KRK_METHOD(float,__int__,{ return INTEGER_VAL(self); })
KRK_METHOD(float,__float__,{ return argv[0]; })

KRK_METHOD(float,__str__,{
	char tmp[100];
	size_t l = snprintf(tmp, 100, "%g", self);
	return OBJECT_VAL(krk_copyString(tmp, l));
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE char

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

#undef BIND_METHOD
#define BIND_METHOD(klass,method) do { krk_defineNative(& _ ## klass->methods, "." #method, _ ## klass ## _ ## method); } while (0)
_noexport
void _createAndBind_numericClasses(void) {
	KrkClass * _int = ADD_BASE_CLASS(vm.baseClasses->intClass, "int", vm.baseClasses->objectClass);
	BIND_METHOD(int,__init__);
	BIND_METHOD(int,__str__);
	BIND_METHOD(int,__int__);
	BIND_METHOD(int,__chr__);
	BIND_METHOD(int,__float__);
	krk_defineNative(&_int->methods, ".__repr__", FUNC_NAME(int,__str__));
	krk_finalizeClass(_int);
	_int->docstring = S("Convert a number or string type to an integer representation.");

	KrkClass * _float = ADD_BASE_CLASS(vm.baseClasses->floatClass, "float", vm.baseClasses->objectClass);
	BIND_METHOD(float,__init__);
	BIND_METHOD(float,__int__);
	BIND_METHOD(float,__float__);
	BIND_METHOD(float,__str__);
	krk_defineNative(&_float->methods, ".__repr__", FUNC_NAME(float,__str__));
	krk_finalizeClass(_float);
	_float->docstring = S("Convert a number or string type to a float representation.");

	KrkClass * _bool = ADD_BASE_CLASS(vm.baseClasses->boolClass, "bool", vm.baseClasses->objectClass);
	BIND_METHOD(bool,__init__);
	BIND_METHOD(bool,__str__);
	krk_defineNative(&_bool->methods, ".__repr__", FUNC_NAME(bool,__str__));
	krk_finalizeClass(_bool);
	_bool->docstring = S("Returns False if the argument is 'falsey', otherwise True.");

	KrkClass * _NoneType = ADD_BASE_CLASS(vm.baseClasses->noneTypeClass, "NoneType", vm.baseClasses->objectClass);
	BIND_METHOD(NoneType, __str__);
	krk_defineNative(&_NoneType->methods, ".__repr__", FUNC_NAME(NoneType,__str__));
	krk_finalizeClass(_NoneType);
}
