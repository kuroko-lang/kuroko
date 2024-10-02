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

extern KrkValue krk_int_from_float(double val);

KRK_StaticMethod(int,__new__) {
	KrkObj *cls;
	int has_x = 0;
	KrkValue x = NONE_VAL();
	int has_base = 0;
	int base = 10;

	/* Most common case */
	if (!hasKw && argc == 2) {
		x = argv[1];
		goto _just_x;
	}

	if (!krk_parseArgs("O|V?i?:int", (const char*[]){"","","base"},
		&cls, &has_x, &x, &has_base, &base)) return NONE_VAL();

	if (has_base && (base < 2 || base > 36) && base != 0) {
		return krk_runtimeError(vm.exceptions->valueError, "base must be 0 or between 2 and 36");
	}

	if (!has_x && has_base) {
		return krk_runtimeError(vm.exceptions->typeError, "missing str argument");
	}

	if (!has_x) {
		return INTEGER_VAL(0);
	}

	if (has_base && !IS_STRING(x)) {
		return krk_runtimeError(vm.exceptions->typeError, "can not convert non-str with explicit base");
	}

_just_x:

	if (IS_INTEGER(x)) return INTEGER_VAL(AS_INTEGER(x));
#ifndef KRK_NO_FLOAT
	if (IS_FLOATING(x)) return krk_int_from_float(AS_FLOATING(x));
#endif

	if (IS_STRING(x)) {
		KrkValue result = krk_parse_int(AS_CSTRING(x), AS_STRING(x)->length, base);
		if (IS_NONE(result)) {
			return krk_runtimeError(vm.exceptions->valueError,
				"invalid literal for int() with base %zd: %R", (ssize_t)base, x);
		}
		return result;
	}

	if (krk_isInstanceOf(x, KRK_BASE_CLASS(long))) return x;
	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%T'", "int", x);
}

KRK_Method(int,__repr__) {
	char tmp[100];
	size_t l = snprintf(tmp, 100, PRIkrk_int, self);
	return OBJECT_VAL(krk_copyString(tmp, l));
}

KRK_Method(int,__int__) { return argv[0]; }

#ifndef KRK_NO_FLOAT
KRK_Method(int,__float__) { return FLOATING_VAL(self); }
#endif

KRK_Method(int,__chr__) {
	unsigned char bytes[5] = {0};
	size_t len = krk_codepointToBytes(self, bytes);
	return OBJECT_VAL(krk_copyString((char*)bytes, len));
}

KRK_Method(int,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (likely(IS_INTEGER(argv[1]))) return BOOLEAN_VAL(self == AS_INTEGER(argv[1]));
#ifndef KRK_NO_FLOAT
	else if (IS_FLOATING(argv[1])) return BOOLEAN_VAL(self == AS_FLOATING(argv[1]));
#endif
	return NOTIMPL_VAL();
}

KRK_Method(int,__hash__) {
	return INTEGER_VAL((uint32_t)AS_INTEGER(argv[0]));
}

static inline int matches(char c, const char * options) {
	for (const char * o = options; *o; ++o) {
		if (*o == c) return 1;
	}
	return 0;
}

const char * krk_parseCommonFormatSpec(struct ParsedFormatSpec *result, const char * spec, size_t length) {
	result->fill = " ";
	result->fillSize = 1;

	if (length > 1) {
		/* How wide is the first character? */
		int i = 1;
		if ((spec[0] & 0xC0) == 0xC0) { /* wider than one byte */
			while ((spec[i] & 0xc0) == 0x80) i++; /* count continuation bytes */
		}
		/* Is the character after it an alignment? */
		if (matches(spec[i],"<>=^")) {
			result->fill = spec;
			result->fillSize = i;
			spec += i;
		}
	}

	if (matches(*spec,"<>=^")) {
		result->align = *spec;
		spec++;
	}

	if (matches(*spec,"+- ")) {
		result->sign = *spec;
		spec++;
	}

	if (*spec == '#') {
		result->alt = 1;
		spec++;
	}

	if (!result->align && *spec == '0') {
		result->align = '=';
		result->fill = "0";
		result->fillSize = 1;
		spec++;
	}

	if (matches(*spec,"0123456789")) {
		result->hasWidth = 1;
		do {
			result->width *= 10;
			result->width += (*spec - '0');
			spec++;
		} while (matches(*spec,"0123456789"));
	}

	if (matches(*spec, "_,")) {
		result->sep = *spec;
		spec++;
	}

	if (*spec == '.') {
		spec++;
		if (!matches(*spec,"0123456789")) {
			krk_runtimeError(vm.exceptions->valueError, "Format specifier missing precision");
			return NULL;
		}
		result->hasPrecision = 1;
		while (matches(*spec,"0123456789")) {
			result->prec *= 10;
			result->prec += (*spec - '0');
			spec++;
		}
	}

	if (*spec && spec[1] != 0) {
		krk_runtimeError(vm.exceptions->valueError, "Invalid format specifier");
		return NULL;
	}

	return spec;
}

typedef int (*fmtCallback)(void *, int, int *);
KrkValue krk_doFormatString(const char * typeName, KrkString * format_spec, int positive, void * abs, fmtCallback callback, fmtCallback (*prepCallback)(void*,int)) {

	struct ParsedFormatSpec opts = {0};
	const char * spec = krk_parseCommonFormatSpec(&opts, format_spec->chars, format_spec->length);
	if (!spec) return NONE_VAL();

	const char * altPrefix = NULL;
	const char * conversions = "0123456789abcdef";
	int base = 0;

	switch (*spec) {
		case 0:   /* unspecified */
		case 'd': /* decimal integer */
			base = 10;
			break;
		case 'b': /* binary */
			base = 2;
			altPrefix = "0b";
			break;
		case 'o': /* octal */
			base = 8;
			altPrefix = "0o";
			break;
		case 'x': /* hex */
			base = 16;
			altPrefix = "0x";
			break;
		case 'X': /* HEX */
			base = 16;
			conversions = "0123456789ABCDEF";
			altPrefix = "0X";
			break;

		case 'c': /* unicode codepoint */
			return krk_runtimeError(vm.exceptions->notImplementedError,
				"TODO: 'c' format specifier");

		case 'n': /* use local-specific separators (TODO) */
			return krk_runtimeError(vm.exceptions->notImplementedError,
				"TODO: 'n' format specifier");

		default:
			return krk_runtimeError(vm.exceptions->valueError,
				"Unknown format code '%c' for object of type '%s'",
				*spec,
				typeName);
	}

	if (!opts.sign)  opts.sign = '-';
	if (!opts.align) opts.align = '>';

	struct StringBuilder sb = {0};

	int width = opts.width;
	int l = 0;

	if (opts.alt && altPrefix && width > 2) width -= 2;
	if ((!positive || opts.sign == '+') && width > 1) width--;

	int digits = 0;
	int sepcount = (opts.sep == ',' || base == 10) ? 3 : 4;
	int more = 0;

	if (prepCallback) callback = prepCallback(abs, base);

	do {
		int digit = callback(abs, base, &more);

		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
			discardStringBuilder(&sb);
			return NONE_VAL();
		}

		if (digits && !more && digit == 0) {
			/* Add backwards */
			for (int i = 0; i < opts.fillSize; ++i) {
				pushStringBuilder(&sb, opts.fill[opts.fillSize-1-i]);
			}
		} else {
			pushStringBuilder(&sb, conversions[digit]);
		}
		l++;
		digits++;

		if (opts.sep && !(digits % sepcount) && (more || (opts.align == '=' && l < width))) {
			pushStringBuilder(&sb, opts.sep);
			l++;
			if (opts.align == '=' && l == width) {
				/* Add backwards */
				for (int i = 0; i < opts.fillSize; ++i) {
					pushStringBuilder(&sb, opts.fill[opts.fillSize-1-i]);
				}
			}
		}
	} while (more || (opts.align == '=' && l < width));

	if (opts.alt && altPrefix) {
		pushStringBuilder(&sb, altPrefix[1]);
		pushStringBuilder(&sb, altPrefix[0]);
	}

	if (!positive || opts.sign == '+') {
		pushStringBuilder(&sb, positive ? '+' : '-');
	}

	if (opts.align == '>') {
		while (l < width) {
			for (int i = 0; i < opts.fillSize; ++i) {
				pushStringBuilder(&sb, opts.fill[opts.fillSize-1-i]);
			}
			l++;
		}
	} else if (opts.align == '^') {
		int remaining = (width - l) / 2;
		for (int i = 0; i < remaining; ++i) {
			for (int i = 0; i < opts.fillSize; ++i) {
				pushStringBuilder(&sb, opts.fill[opts.fillSize-1-i]);
			}
			l++;
		}
	}

	for (size_t i = 0; i < sb.length / 2; ++i) {
		char t = sb.bytes[i];
		sb.bytes[i] = sb.bytes[sb.length - i - 1];
		sb.bytes[sb.length - i - 1] = t;
	}

	if (opts.align == '<' || opts.align == '^') {
		while (l < width) {
			pushStringBuilderStr(&sb, opts.fill, opts.fillSize);
			l++;
		}
	}

	return finishStringBuilder(&sb);
}

static int formatIntCallback(void * a, int base, int *more) {
	krk_integer_type v = *(krk_integer_type*)a;
	int digit = v % base;
	v /= base;
	*(krk_integer_type*)a = v;
	*more = v > 0;
	return digit;
}


KRK_Method(int,__format__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,format_spec);

	krk_integer_type abs = self < 0 ? -self : self;

	return krk_doFormatString(krk_typeName(argv[0]), format_spec,
		self >= 0,
		&abs,
		formatIntCallback, NULL);
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

#ifndef KRK_NO_FLOAT
# define MAYBE_FLOAT(x) x
#else
# define MAYBE_FLOAT(x) krk_runtimeError(vm.exceptions->valueError, "no float support")
#endif

#define BASIC_BIN_OP(name,operator) \
	KRK_Method(int,__ ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return krk_int_op_ ## name(self, AS_INTEGER(argv[1])); \
		else if (likely(IS_FLOATING(argv[1]))) return MAYBE_FLOAT(FLOATING_VAL((double)self operator AS_FLOATING(argv[1]))); \
		return NOTIMPL_VAL(); \
	} \
	KRK_Method(int,__r ## name ## __) { \
		if (likely(IS_INTEGER(argv[1]))) return krk_int_op_ ## name(AS_INTEGER(argv[1]), self); \
		else if (likely(IS_FLOATING(argv[1]))) return MAYBE_FLOAT(FLOATING_VAL(AS_FLOATING(argv[1]) operator (double)self)); \
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
		else if (likely(IS_FLOATING(argv[1]))) return MAYBE_FLOAT(BOOLEAN_VAL((double)self operator AS_FLOATING(argv[1]))); \
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

#ifndef KRK_NO_FLOAT
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
#endif

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
#ifndef KRK_NO_FLOAT
		double b = AS_FLOATING(argv[1]);
		if (unlikely(b == 0.0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero");
		return FLOATING_VAL(__builtin_floor((double)self / b));
#else
		return krk_runtimeError(vm.exceptions->valueError, "no float support");
#endif
	}
	return NOTIMPL_VAL();
}

KRK_Method(int,__rfloordiv__) {
	METHOD_TAKES_EXACTLY(1);
	if (unlikely(self == 0)) return krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division by zero");
	else if (likely(IS_INTEGER(argv[1]))) return _krk_int_div(AS_INTEGER(argv[1]), self);
	else if (likely(IS_FLOATING(argv[1]))) return MAYBE_FLOAT(FLOATING_VAL(__builtin_floor(AS_FLOATING(argv[1]) / (double)self)));
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

#ifndef KRK_NO_FLOAT
KRK_StaticMethod(float,__new__) {
	FUNCTION_TAKES_AT_MOST(2);
	if (argc < 2) return FLOATING_VAL(0.0);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));

	trySlowMethod(vm.specialMethodNames[METHOD_FLOAT]);

	return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%T'", "float", argv[1]);
}

KRK_Method(float,__int__) { return krk_int_from_float(self); }
KRK_Method(float,__float__) { return argv[0]; }

extern KrkValue krk_double_to_string(double,unsigned int,char,int,int);
KRK_Method(float,__repr__) {
	return krk_double_to_string(self,16,' ',0,0);
}

KRK_Method(float,__format__) {
	char * format_spec;
	size_t format_spec_length;
	if (!krk_parseArgs(".s#", (const char*[]){"format_spec"}, &format_spec, &format_spec_length)) return NONE_VAL();

	struct ParsedFormatSpec opts = {0};
	const char * spec = krk_parseCommonFormatSpec(&opts, format_spec, format_spec_length);
	if (!spec) return NONE_VAL();

	char formatter = 'g';
	int digits = 16;
	int forcedigits = opts.alt;

	switch (*spec) {
		case 0:
		case 'g':
			/* defaults */
			break;

		case 'G':
			formatter = 'G';
			break;

		case 'f':
		case 'F':
		case 'e':
		case 'E':
			digits = 6;
			formatter = *spec;
			forcedigits = (opts.hasPrecision && opts.prec == 0) ? 0 : 1;
			break;

		default:
			return krk_runtimeError(vm.exceptions->valueError,
				"Unknown format code '%c' for object of type '%s'",
				*spec,
				"float");
	}

	if (opts.sep) return krk_runtimeError(vm.exceptions->valueError, "unsupported option for float");
	if (opts.hasPrecision) digits = opts.prec;
	if (!opts.align) opts.align = '>';

	KrkValue result = krk_double_to_string(self, digits, formatter, opts.sign == '+', forcedigits);
	if (!IS_STRING(result) || !opts.width) return result;

	krk_push(result);

	/* Calculate how much padding we need to add. */
	size_t avail = (size_t)opts.width > AS_STRING(result)->length ? (size_t)opts.width - AS_STRING(result)->length : 0;

	/* If there's no available space for padding, just return the string we already have. */
	if (!avail) return krk_pop();

	struct StringBuilder sb = {0};
	size_t before = 0;
	size_t after = 0;
	int hassign = 0;
	if (opts.align == '<') {
		after = avail;
	} else if (opts.align == '>') {
		before = avail;
	} else if (opts.align == '^') {
		after = avail / 2;
		before = avail - after;
	} else if (opts.align == '=') {
		before = avail;
		if (avail && AS_STRING(result)->length && (AS_CSTRING(result)[0] == '-' || AS_CSTRING(result)[0] == '+')) {
			krk_pushStringBuilder(&sb, AS_CSTRING(result)[0]);
			hassign = 1;
		}
	}

	/* Fill in padding with a new string builder. */
	for (size_t i = 0; i < before; ++i) krk_pushStringBuilderStr(&sb, opts.fill, opts.fillSize);
	krk_pushStringBuilderStr(&sb, AS_CSTRING(result) + hassign, AS_STRING(result)->length - hassign);
	for (size_t i = 0; i < after; ++i) krk_pushStringBuilderStr(&sb, opts.fill, opts.fillSize);

	krk_pop();
	return krk_finishStringBuilder(&sb);
}

KRK_Method(float,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) return BOOLEAN_VAL(self == (double)AS_INTEGER(argv[1]));
	else if (IS_FLOATING(argv[1])) return BOOLEAN_VAL(self == AS_FLOATING(argv[1]));
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

extern KrkValue krk_float_to_fraction(double d);
KRK_Method(float,as_integer_ratio) {
	return krk_float_to_fraction(self);
}
#endif

#undef CURRENT_CTYPE
#define CURRENT_CTYPE krk_integer_type

KRK_StaticMethod(bool,__new__) {
	FUNCTION_TAKES_AT_MOST(2);
	if (argc < 2) return BOOLEAN_VAL(0);
	return BOOLEAN_VAL(!krk_isFalsey(argv[1]));
}

KRK_Method(bool,__repr__) {
	return OBJECT_VAL((self ? S("True") : S("False")));
}

KRK_Method(bool,__format__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,format_spec);

	if (!format_spec->length) {
		return FUNC_NAME(bool,__repr__)(argc,argv,hasKw);
	} else {
		return FUNC_NAME(int,__format__)(argc,argv,hasKw);
	}
}

KRK_StaticMethod(NoneType,__new__) {
	if (argc > 1) return krk_runtimeError(vm.exceptions->argumentError, "%s takes no arguments", "NoneType");
	return NONE_VAL();
}

KRK_Method(NoneType,__repr__) {
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

KRK_StaticMethod(NotImplementedType,__new__) {
	if (argc > 1) return krk_runtimeError(vm.exceptions->argumentError, "%s takes no arguments", "NotImplementedType");
	return NOTIMPL_VAL();
}

KRK_Method(NotImplementedType,__repr__) {
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
#undef BIND_STATICMETHOD
/* These class names conflict with C types, so we need to cheat a bit */
#define BIND_METHOD(klass,method) do { krk_defineNative(& _ ## klass->methods, #method, _ ## klass ## _ ## method); } while (0)
#define BIND_STATICMETHOD(klass,method) do { krk_defineNativeStaticMethod(& _ ## klass->methods, #method, _ ## klass ## _ ## method); } while (0)
#define BIND_TRIPLET(klass,name) \
	BIND_METHOD(klass,__ ## name ## __); \
	BIND_METHOD(klass,__r ## name ## __); \
	krk_defineNative(&_ ## klass->methods,"__i" #name "__",_ ## klass ## ___ ## name ## __);
_noexport
void _createAndBind_numericClasses(void) {
	KrkClass * _int = ADD_BASE_CLASS(vm.baseClasses->intClass, "int", vm.baseClasses->objectClass);
	_int->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	_int->allocSize = 0;
	BIND_STATICMETHOD(int,__new__);
	BIND_METHOD(int,__repr__);
	BIND_METHOD(int,__int__);
	BIND_METHOD(int,__chr__);
	BIND_METHOD(int,__eq__);
	BIND_METHOD(int,__hash__);
	BIND_METHOD(int,__format__);

	BIND_TRIPLET(int,add);
	BIND_TRIPLET(int,sub);
	BIND_TRIPLET(int,mul);
	BIND_TRIPLET(int,or);
	BIND_TRIPLET(int,xor);
	BIND_TRIPLET(int,and);
	BIND_TRIPLET(int,lshift);
	BIND_TRIPLET(int,rshift);
	BIND_TRIPLET(int,mod);
	BIND_TRIPLET(int,floordiv);
	BIND_TRIPLET(int,pow);

#ifndef KRK_NO_FLOAT
	BIND_METHOD(int,__float__);
	BIND_TRIPLET(int,truediv);
#endif

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

	krk_finalizeClass(_int);
	KRK_DOC(_int, "Convert a number or string type to an integer representation.");

	KrkClass * _float = ADD_BASE_CLASS(vm.baseClasses->floatClass, "float", vm.baseClasses->objectClass);
	_float->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	_float->allocSize = 0;
#ifndef KRK_NO_FLOAT
	BIND_STATICMETHOD(float,__new__);
	BIND_METHOD(float,__int__);
	BIND_METHOD(float,__float__);
	BIND_METHOD(float,__repr__);
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
	BIND_METHOD(float,__format__);
	BIND_METHOD(float,as_integer_ratio);
#endif
	krk_finalizeClass(_float);
	KRK_DOC(_float, "Convert a number or string type to a float representation.");

	KrkClass * _bool = ADD_BASE_CLASS(vm.baseClasses->boolClass, "bool", vm.baseClasses->intClass);
	_bool->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_STATICMETHOD(bool,__new__);
	BIND_METHOD(bool,__repr__);
	BIND_METHOD(bool,__format__);
	krk_finalizeClass(_bool);
	KRK_DOC(_bool, "Returns False if the argument is 'falsey', otherwise True.");

	KrkClass * _NoneType = ADD_BASE_CLASS(vm.baseClasses->noneTypeClass, "NoneType", vm.baseClasses->objectClass);
	_NoneType->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	_NoneType->allocSize = 0;
	BIND_STATICMETHOD(NoneType, __new__);
	BIND_METHOD(NoneType, __repr__);
	BIND_METHOD(NoneType, __hash__);
	BIND_METHOD(NoneType, __eq__);
	krk_finalizeClass(_NoneType);

	KrkClass * _NotImplementedType = ADD_BASE_CLASS(vm.baseClasses->notImplClass, "NotImplementedType", vm.baseClasses->objectClass);
	_NotImplementedType->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	_NotImplementedType->allocSize = 0;
	BIND_STATICMETHOD(NotImplementedType, __new__);
	BIND_METHOD(NotImplementedType, __repr__);
	BIND_METHOD(NotImplementedType, __hash__);
	BIND_METHOD(NotImplementedType, __eq__);
	krk_finalizeClass(_NotImplementedType);

	krk_attachNamedValue(&vm.builtins->fields, "NotImplemented", NOTIMPL_VAL());
}
