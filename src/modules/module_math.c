/**
 * math module; thin wrapper around libc math functions.
 */
#include <math.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/util.h>

#define ONE_ARGUMENT(name) if (argc != 1) { \
	krk_runtimeError(vm.exceptions->argumentError, "%s() expects one argument", #name); \
	return NONE_VAL(); \
}

#define TWO_ARGUMENTS(name) if (argc != 2) { \
	krk_runtimeError(vm.exceptions->argumentError, "%s() expects two arguments", #name); \
	return NONE_VAL(); \
}

#define FORCE_FLOAT(src,arg) \
	KrkValue arg = src; \
	if (!IS_FLOATING(arg)) { switch (KRK_VAL_TYPE(arg)) { \
		case KRK_VAL_INTEGER: arg = FLOATING_VAL(AS_INTEGER(arg)); break; \
		case KRK_VAL_BOOLEAN: arg = FLOATING_VAL(AS_BOOLEAN(arg)); break; \
		default: { \
			KrkClass * type = krk_getType(arg); \
			krk_push(arg); \
			if (!krk_bindMethod(type, S("__float__"))) { \
				krk_pop(); \
			} else { \
				arg = krk_callStack(0); \
			} \
		} break; \
	} }

#define REAL_NUMBER_NOT(name, garbage) { \
	krk_runtimeError(vm.exceptions->typeError, "%s() argument must be real number, not '%T'", #name, garbage); \
	return NONE_VAL(); \
}

extern KrkValue krk_int_from_float(double val);

#define MATH_DELEGATE(func) \
static KrkValue _math_ ## func(int argc, const KrkValue argv[], int hasKw) { \
	ONE_ARGUMENT(func) \
	if (IS_FLOATING(argv[0])) { \
		return krk_int_from_float(func(AS_FLOATING(argv[0]))); \
	} else if (IS_INTEGER(argv[0])) { \
		return argv[0]; /* no op */ \
	} else { \
		KrkClass * type = krk_getType(argv[0]); \
		krk_push(argv[0]); \
		if (!krk_bindMethod(type, S("__" #func "__"))) REAL_NUMBER_NOT(func,argv[0]) \
		return krk_callStack(0); \
	} \
}

MATH_DELEGATE(ceil)
MATH_DELEGATE(floor)
MATH_DELEGATE(trunc)

#define MATH_ONE_NAME(func,name) \
static KrkValue _math_ ## name(int argc, const KrkValue argv[], int hasKw) { \
	ONE_ARGUMENT(name) \
	FORCE_FLOAT(argv[0],arg_0) \
	if (IS_FLOATING(arg_0)) { \
		return FLOATING_VAL(func(AS_FLOATING(arg_0))); \
	} else REAL_NUMBER_NOT(name,arg_0) \
}
#define MATH_ONE(func) MATH_ONE_NAME(func,func)

MATH_ONE(exp)
MATH_ONE(expm1)
MATH_ONE(log2)
MATH_ONE(log10)
MATH_ONE(sqrt)
MATH_ONE(acos)
MATH_ONE(asin)
MATH_ONE(atan)
MATH_ONE(cos)
MATH_ONE(sin)
MATH_ONE(tan)
MATH_ONE(acosh)
MATH_ONE(asinh)
MATH_ONE(atanh)
MATH_ONE(cosh)
MATH_ONE(sinh)
MATH_ONE(tanh)
MATH_ONE(erf)
MATH_ONE(erfc)
MATH_ONE(log1p)
MATH_ONE(tgamma)
MATH_ONE(lgamma)

#define MATH_TWO(func) \
static KrkValue _math_ ## func(int argc, const KrkValue argv[], int hasKw) { \
	TWO_ARGUMENTS(func) \
	FORCE_FLOAT(argv[0],arg_0) \
	FORCE_FLOAT(argv[1],arg_1) \
	if (!IS_FLOATING(arg_0)) REAL_NUMBER_NOT(func,arg_0) \
	if (!IS_FLOATING(arg_1)) REAL_NUMBER_NOT(func,arg_1) \
	return FLOATING_VAL(func(AS_FLOATING(arg_0),AS_FLOATING(arg_1))); \
}

MATH_TWO(copysign)
MATH_TWO(fmod)
MATH_TWO(remainder)
MATH_TWO(pow)
MATH_TWO(atan2)

static KrkValue _float_pow(int argc, const KrkValue argv[], int hasKw) {
	TWO_ARGUMENTS(__pow__);
	if (unlikely(!IS_FLOATING(argv[0]))) return krk_runtimeError(vm.exceptions->typeError, "expected float");
	if (likely(IS_FLOATING(argv[1]))) {
		return FLOATING_VAL(pow(AS_FLOATING(argv[0]),AS_FLOATING(argv[1])));
	} else if (likely(IS_INTEGER(argv[1]))) {
		return FLOATING_VAL(pow(AS_FLOATING(argv[0]),(double)AS_INTEGER(argv[1])));
	}
	return NOTIMPL_VAL();
}


static KrkValue _math_frexp(int argc, const KrkValue argv[], int hasKw) {
	ONE_ARGUMENT(frexp)
	FORCE_FLOAT(argv[0],arg_0)
	if (!IS_FLOATING(arg_0)) {
		REAL_NUMBER_NOT(frexp,arg_0)
	}
	int exp = 0;
	double result = frexp(AS_FLOATING(arg_0), &exp);
	KrkTuple * outValue = krk_newTuple(2);
	outValue->values.values[0] = FLOATING_VAL(result);
	outValue->values.values[1] = INTEGER_VAL(exp);
	outValue->values.count = 2;
	return OBJECT_VAL(outValue);
}

#define MATH_IS(func) \
static KrkValue _math_ ## func(int argc, const KrkValue argv[], int hasKw) { \
	ONE_ARGUMENT(func) \
	if (!IS_FLOATING(argv[0])) REAL_NUMBER_NOT(func,argv[0]) \
	return BOOLEAN_VAL(func(AS_FLOATING(argv[0]))); \
}

MATH_IS(isfinite)
MATH_IS(isinf)
MATH_IS(isnan)

#define bind(name) krk_defineNative(&module->fields, #name, _math_ ## name)

KRK_Module(math) {
	KRK_DOC(module, "@brief Provides access to floating-point mathematical functions from the system `libm`.");
	KRK_DOC(bind(ceil),
		"@brief Returns the smallest integer value not less than the input.\n"
		"@arguments x");
	KRK_DOC(bind(floor),
		"@brief Returns the largest integer value not greater than the input.\n"
		"@arguments x");
	KRK_DOC(bind(trunc),
		"@brief Rounds the input towards zero to an integer.\n"
		"@arguments x");
	KRK_DOC(bind(exp),
		"@brief Returns the base-e exponentiation of the input.\n"
		"@arguments x");
	KRK_DOC(bind(expm1),
		"@brief Equivalent to `exp(x) - 1`.\n"
		"@arguments x");
	KRK_DOC(bind(log2),
		"@brief Calculates the base-2 logarithm of the input.\n"
		"@arguments x");
	KRK_DOC(bind(log10),
		"@brief Calculates the base-10 logarithm of the input.\n"
		"@arguments x");
	KRK_DOC(bind(sqrt),
		"@brief Calculates the square root of the input.\n"
		"@arguments x");
	KRK_DOC(bind(acos),
		"@brief Calculates the arc-cosine of the radian input.\n"
		"@arguments x");
	KRK_DOC(bind(asin),
		"@brief Calculates the arc-sine of the radian input.\n"
		"@arguments x");
	KRK_DOC(bind(atan),
		"@brief Calculates the arc-tangent of the radian input.\n"
		"@arguments x");
	KRK_DOC(bind(cos),
		"@brief Calculates the cosine of the radian input.\n"
		"@arguments x");
	KRK_DOC(bind(sin),
		"@brief Calculates the sine of the radian input.\n"
		"@arguments x");
	KRK_DOC(bind(tan),
		"@brief Calculates the tangent of the radian input.\n"
		"@arguments x");
	KRK_DOC(bind(acosh),
		"@brief Calculates the inverse hyperbolic cosine of the input.\n"
		"@arguments x");
	KRK_DOC(bind(asinh),
		"@brief Calculates the inverse hyperbolic sine of the input.\n"
		"@arguments x");
	KRK_DOC(bind(atanh),
		"@brief Calculates the inverse hyperbolic tangent of the input.\n"
		"@arguments x");
	KRK_DOC(bind(cosh),
		"@brief Calculates the hyperbolic cosine of the input.\n"
		"@arguments x");
	KRK_DOC(bind(sinh),
		"@brief Calculates the hyperbolic sine of the input.\n"
		"@arguments x");
	KRK_DOC(bind(tanh),
		"@brief Calculates the hyperbolic tangent of the input.\n"
		"@arguments x");
	KRK_DOC(bind(erf),
		"@brief Calculates the error function of the input.\n"
		"@arguments x");
	KRK_DOC(bind(erfc),
		"@brief Calculates the complementary error function of the input.\n"
		"@arguments x");
	KRK_DOC(bind(tgamma),
		"@brief Calculates the gamma of the input.\n"
		"@arguments x");
	KRK_DOC(bind(lgamma),
		"@brief Calculates the log gamma of the input.\n"
		"@arguments x");
	KRK_DOC(bind(copysign),
		"@brief Copies the sign from @p x to @p y\n"
		"@arguments x,y");
	KRK_DOC(bind(fmod),
		"@brief Returns the floating point remainder of @p x over @p y\n"
		"@arguments x,y");
	KRK_DOC(bind(remainder),
		"@brief Somehow different from `fmod`.");
	KRK_DOC(bind(log1p),
		"@brief Equivalent to `log(x + 1)`\n"
		"@arguments x");
	KRK_DOC(bind(expm1),
		"@brief Equivalent to `exp(x) - 1`\n"
		"@arguments x");
	KRK_DOC(bind(pow),
		"@brief Calculates `x^p`\n"
		"@arguments x,p");
	KRK_DOC(bind(atan2),
		"@brief Calculates the arctangent of `x` and `y`\n"
		"@arguments x,y");
	KRK_DOC(bind(frexp),
		"@brief Converts a floating point input to a fractional and integer component pair, returned as a tuple.\n"
		"@arguments x\n"
		"@returns @ref tuple of two @ref int");
	KRK_DOC(bind(isfinite),
		"@brief Determines if the input is finite.\n"
		"@arguments x\n");
	KRK_DOC(bind(isinf),
		"@brief Determines if the input is infinite.\n"
		"@arguments x\n");
	KRK_DOC(bind(isnan),
		"@brief Determines if the input is the floating point `NaN`.\n"
		"@arguments x\n");

	/**
	 * Maybe the math library should be a core one, but I'm not sure if I want
	 * to have to depend on -lm in the main interpreter, so instead if we have
	 * imported math, we'll just quietly give floats a __pow__ method...
	 */
	krk_defineNative(&vm.baseClasses->floatClass->methods, "__pow__", _float_pow);
	krk_finalizeClass(vm.baseClasses->floatClass);

	krk_attachNamedValue(&module->fields, "pi",  FLOATING_VAL(M_PI));
	krk_attachNamedValue(&module->fields, "e",   FLOATING_VAL(M_E));
	krk_attachNamedValue(&module->fields, "inf", FLOATING_VAL(INFINITY));
	krk_attachNamedValue(&module->fields, "nan", FLOATING_VAL(NAN));
}
