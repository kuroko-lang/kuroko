/**
 * math module; thin wrapper around libc math functions.
 */
#include <math.h>
#include "../vm.h"
#include "../value.h"
#include "../object.h"

#define S(c) (krk_copyString(c,sizeof(c)-1))

#define ONE_ARGUMENT(name) if (argc != 1) { \
	krk_runtimeError(vm.exceptions.argumentError, "%s() expects one argument", #name); \
	return NONE_VAL(); \
}

#define TWO_ARGUMENTS(name) if (argc != 2) { \
	krk_runtimeError(vm.exceptions.argumentError, "%s() expects two arguments", #name); \
	return NONE_VAL(); \
}

#define FORCE_FLOAT(arg) \
	if (!IS_FLOATING(arg)) { switch (arg.type) { \
		case VAL_INTEGER: arg = FLOATING_VAL(AS_INTEGER(arg)); break; \
		case VAL_BOOLEAN: arg = FLOATING_VAL(AS_BOOLEAN(arg)); break; \
		default: { \
			KrkClass * type = AS_CLASS(krk_typeOf(1,&arg)); \
			krk_push(arg); \
			if (!krk_bindMethod(type, S("__float__"))) { \
				krk_pop(); \
			} else { \
				arg = krk_callSimple(krk_peek(0), 0, 1); \
			} \
		} break; \
	} }

#define REAL_NUMBER_NOT(name, garbage) { \
	krk_runtimeError(vm.exceptions.typeError, "%s() argument must be real number, not %s", #name, krk_typeName(garbage)); \
	return NONE_VAL(); \
}

#define MATH_DELEGATE(func) \
static KrkValue _math_ ## func(int argc, KrkValue argv[]) { \
	ONE_ARGUMENT(func) \
	if (IS_FLOATING(argv[0])) { \
		return INTEGER_VAL(func(AS_FLOATING(argv[0]))); \
	} else if (IS_INTEGER(argv[0])) { \
		return argv[0]; /* no op */ \
	} else { \
		KrkClass * type = AS_CLASS(krk_typeOf(1,&argv[0])); \
		krk_push(argv[0]); \
		if (!krk_bindMethod(type, S("__" #func "__"))) REAL_NUMBER_NOT(func,argv[0]) \
		return krk_callSimple(krk_peek(0), 0, 1); \
	} \
}

MATH_DELEGATE(ceil)
MATH_DELEGATE(floor)
#ifdef trunc
MATH_DELEGATE(trunc)
#endif

#define MATH_ONE_NAME(func,name) \
static KrkValue _math_ ## name(int argc, KrkValue argv[]) { \
	ONE_ARGUMENT(name) \
	FORCE_FLOAT(argv[0]) \
	if (IS_FLOATING(argv[0])) { \
		return FLOATING_VAL(func(AS_FLOATING(argv[0]))); \
	} else REAL_NUMBER_NOT(name,argv[0]) \
}
#define MATH_ONE(func) MATH_ONE_NAME(func,func)

MATH_ONE(exp)
#ifdef expm1
MATH_ONE(expm1)
#endif
MATH_ONE(log2)
MATH_ONE(log10)
MATH_ONE(sqrt)
MATH_ONE(acos)
MATH_ONE(asin)
MATH_ONE(atan)
MATH_ONE(cos)
MATH_ONE(sin)
MATH_ONE(tan)
#ifdef acosh
MATH_ONE(acosh)
MATH_ONE(asinh)
MATH_ONE(atanh)
#endif
MATH_ONE(cosh)
MATH_ONE(sinh)
MATH_ONE(tanh)
#ifdef erf
MATH_ONE(erf)
MATH_ONE(erfc)
#endif
#ifdef gamma
MATH_ONE(gamma)
MATH_ONE(lgamma)
#endif
MATH_ONE_NAME(log,log1p)

#define MATH_TWO(func) \
static KrkValue _math_ ## func(int argc, KrkValue argv[]) { \
	TWO_ARGUMENTS(func) \
	FORCE_FLOAT(argv[0]) \
	FORCE_FLOAT(argv[1]) \
	if (!IS_FLOATING(argv[0])) REAL_NUMBER_NOT(func,argv[0]) \
	if (!IS_FLOATING(argv[1])) REAL_NUMBER_NOT(func,argv[1]) \
	return FLOATING_VAL(func(AS_FLOATING(argv[0]),AS_FLOATING(argv[1]))); \
}

#ifdef copysign
MATH_TWO(copysign)
#endif
MATH_TWO(fmod)
#ifdef remainder
MATH_TWO(remainder)
#endif
MATH_TWO(pow)
MATH_TWO(atan2)

static KrkValue _math_frexp(int argc, KrkValue argv[]) {
	ONE_ARGUMENT(frexp)
	FORCE_FLOAT(argv[0])
	if (!IS_FLOATING(argv[0])) {
		REAL_NUMBER_NOT(frexp,argv[0])
	}
	int exp = 0;
	double result = frexp(AS_FLOATING(argv[0]), &exp);
	KrkTuple * outValue = krk_newTuple(2);
	outValue->values.values[0] = FLOATING_VAL(result);
	outValue->values.values[1] = INTEGER_VAL(exp);
	outValue->values.count = 2;
	return OBJECT_VAL(outValue);
}

#define MATH_IS(func) \
static KrkValue _math_ ## func(int argc, KrkValue argv[]) { \
	ONE_ARGUMENT(func) \
	if (!IS_FLOATING(argv[0])) REAL_NUMBER_NOT(func,argv[0]) \
	return BOOLEAN_VAL(func(AS_FLOATING(argv[0]))); \
}

#ifdef isfinite
MATH_IS(isfinite)
MATH_IS(isinf)
MATH_IS(isnan)
#endif

#define bind(name) krk_defineNative(&module->fields, #name, _math_ ## name)

KrkValue krk_module_onload_math(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	krk_push(OBJECT_VAL(module));

	bind(ceil);
	bind(floor);
#ifdef _math_trunc
	bind(trunc);
#endif
	bind(exp);
#ifdef _math_expm1
	bind(expm1);
#endif
	bind(log2);
	bind(log10);
	bind(sqrt);
	bind(acos);
	bind(asin);
	bind(atan);
	bind(cos);
	bind(sin);
	bind(tan);
#ifdef _math_acosh
	bind(acosh);
	bind(asinh);
	bind(atanh);
#endif
	bind(cosh);
	bind(sinh);
	bind(tanh);
#ifdef _math_erf
	bind(erf);
	bind(erfc);
#endif
#ifdef _math_gamma
	bind(gamma);
	bind(lgamma);
#endif
#ifdef _math_copysign
	bind(copysign);
#endif
	bind(fmod);
#ifdef _math_remainder
	bind(remainder);
#endif
	bind(log1p);
	bind(pow);
	bind(atan2);
	bind(frexp);
#ifdef isfinite
	bind(isfinite);
	bind(isinf);
	bind(isnan);
#endif

	krk_attachNamedValue(&module->fields, "pi",  FLOATING_VAL(M_PI));
#ifndef __toaru__
	/* TODO: Add these to toaru... */
	krk_attachNamedValue(&module->fields, "e",   FLOATING_VAL(M_E));
	krk_attachNamedValue(&module->fields, "inf", FLOATING_VAL(INFINITY));
	krk_attachNamedValue(&module->fields, "nan", FLOATING_VAL(NAN));
#endif

	krk_pop();
	return OBJECT_VAL(module);
}
