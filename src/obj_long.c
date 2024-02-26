/**
 * @file    obj_long.c
 * @brief   Big ints, implemented through my own makeshift thing
 * @author  K. Lange <klange@toaruos.org>
 *
 * Simple, slightly incomplete implementation of a 'long' type.
 * Conceptually, several things were learned from Python: we store
 * our longs as a sequence of 31-bit unsigned digits, combined with
 * a signed count of digits - negative for a negative number, positive
 * for a positive number, and 0 for 0.
 *
 *
 * TODO:
 * - Expose better functions for extracting and converting native integers,
 *   which would be useful in modules that want to take 64-bit values,
 *   extracted unsigned values, etc.
 * - Faster division for large divisors?
 * - Shifts without multiply/divide...
 */
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/util.h>
#include "private.h"

#define DIGIT_SHIFT 31
#define DIGIT_MAX   0x7FFFFFFF

struct KrkLong_Internal {
	ssize_t    width;
	uint32_t *digits;
};

typedef struct KrkLong_Internal KrkLong;

/**
 * @brief Initialize an untouched KrkLong with a signed 64-bit value.
 *
 * This should always be called at least once for every KrkLong.
 */
static int krk_long_init_si(KrkLong * num, int64_t val) {

	/* Special-case for 0: width = 0, no digits. */
	if (val == 0) {
		num->width = 0;
		num->digits = NULL;
		return 0;
	}

	/* Digits store unsigned values, so flip things over. */
	int sign = (val < 0) ? -1 : 1;
	uint64_t abs = (val < 0) ? -val : val;

	/* Quick case for things that fit in our digits... */
	if (abs <= DIGIT_MAX) {
		num->width = sign;
		num->digits = malloc(sizeof(uint32_t));
		num->digits[0] = abs;
		return 0;
	}

	/* Figure out how many digits we need. */
	uint64_t tmp = abs;
	int64_t cnt = 1;

	while (tmp > DIGIT_MAX) {
		cnt++;
		tmp >>= DIGIT_SHIFT;
	}

	/* Allocate space */
	num->width = cnt * sign;
	num->digits = malloc(sizeof(uint32_t) * cnt);

	/* Extract digits. */
	for (int64_t i = 0; i < cnt; ++i) {
		num->digits[i] = (abs & DIGIT_MAX);
		abs >>= DIGIT_SHIFT;
	}

	return 0;
}

/**
 * @brief Initialize an untouched KrkLong with an unsigned 64-bit value.
 */
static int krk_long_init_ui(KrkLong * num, uint64_t val) {

	/* Special-case for 0: width = 0, no digits. */
	if (val == 0) {
		num->width = 0;
		num->digits = NULL;
		return 0;
	}

	if (val <= DIGIT_MAX) {
		num->width = 1;
		num->digits = malloc(sizeof(uint32_t));
		num->digits[0] = val;
		return 0;
	}

	/* Figure out how many digits we need. */
	uint64_t tmp = val;
	uint64_t cnt = 1;

	while (tmp > DIGIT_MAX) {
		cnt++;
		tmp >>= DIGIT_SHIFT;
	}

	/* Allocate space */
	num->width = cnt;
	num->digits = malloc(sizeof(uint32_t) * cnt);

	/* Extract digits. */
	for (uint64_t i = 0; i < cnt; ++i) {
		num->digits[i] = (val & DIGIT_MAX);
		val >>= DIGIT_SHIFT;
	}

	return 0;
}

/**
 * @brief Variadic initializer. All values become 0.
 *
 * Be sure to pass a NULL at the end...
 */
static int krk_long_init_many(KrkLong *a, ...) {
	va_list argp;
	va_start(argp, a);

	KrkLong * next = a;
	while (next) {
		krk_long_init_si(next, 0);
		next = va_arg(argp, KrkLong *);
	}

	va_end(argp);
	return 0;
}

/**
 * @brief Initialize a new long by copying the digits from an
 *        existing, initialized long.
 *
 * The input value @p in may be a 0 value, but must be initialized.
 */
static int krk_long_init_copy(KrkLong * out, const KrkLong * in) {
	size_t abs_width = in->width < 0 ? -in->width : in->width;
	out->width = in->width;
	out->digits = out->width ? malloc(sizeof(uint32_t) * abs_width) : NULL;
	for (size_t i = 0; i < abs_width; ++i) {
		out->digits[i] = in->digits[i];
	}
	return 0;
}


/**
 * @brief Free the space used for digits.
 *
 * @p num will have a value of 0 and is not itself freed.
 */
static int krk_long_clear(KrkLong * num) {
	if (num->digits) free(num->digits);
	num->width = 0;
	num->digits = NULL;
	return 0;
}

/**
 * @brief Variadic form of @c krk_long_clear
 *
 * Be sure to pass a NULL at the end...
 */
static int krk_long_clear_many(KrkLong *a, ...) {
	va_list argp;
	va_start(argp, a);

	KrkLong * next = a;
	while (next) {
		krk_long_clear(next);
		next = va_arg(argp, KrkLong *);
	}

	va_end(argp);
	return 0;
}

/**
 * @brief Resize an initialized long to fit more digits.
 *
 * If @p newdigits is fewer digits than @p num already has,
 * it will not be resized. @p num may be a 0 value.
 */
static int krk_long_resize(KrkLong * num, ssize_t newdigits) {
	if (newdigits == 0) {
		krk_long_clear(num);
		return 0;
	}

	size_t abs = newdigits < 0 ? -newdigits : newdigits;
	size_t eabs = num->width < 0 ? -num->width : num->width;
	if (num->width == 0) {
		num->digits = calloc(sizeof(uint32_t), newdigits);
	} else if (eabs < abs) {
		num->digits = realloc(num->digits, sizeof(uint32_t) * newdigits);
		memset(&num->digits[eabs], 0, sizeof(uint32_t)*(abs-eabs));
	}

	num->width = newdigits;
	return 0;
}

/**
 * @brief Update the sign of a long.
 *
 * Inverts the width of @p num as necessary.
 */
static int krk_long_set_sign(KrkLong * num, int sign) {
	num->width = num->width < 0 ? (-num->width) * sign : num->width * sign;
	return 0;
}

/**
 * @brief Remove trailing zeros from a long.
 *
 * I don't think I made this work right if the value _is_ zero, so,
 * uh, make sure you deal with that...
 */
static int krk_long_trim(KrkLong * num) {
	int invert = num->width < 0;
	size_t owidth = invert ? -num->width : num->width;
	size_t redundant = 0;
	for (size_t i = 0; i < owidth; i++) {
		if (num->digits[owidth-i-1] == 0) {
			redundant++;
		} else {
			break;
		}
	}

	if (redundant) {
		krk_long_resize(num, owidth - redundant);
		if (invert) krk_long_set_sign(num, -1);
	}

	return 0;
}

/**
 * @brief Compare two long values.
 *
 * Suitable for a traditional sort comparison.
 *
 * @return 0 if @p a and @p b are the same, -1 if @p a is less than @p b and
 *         1 if @p a is greater than @p b
 */
static int krk_long_compare(const KrkLong * a, const KrkLong * b) {
	if (a->width > b->width) return 1;
	if (b->width > a->width) return -1;
	int sign = a->width < 0 ? -1 : 1;
	size_t abs_width = a->width < 0 ? -a->width : a->width;
	for (size_t i = 0; i < abs_width; ++i) {
		if (a->digits[abs_width-i-1] > b->digits[abs_width-i-1]) return sign; /* left is bigger */
		if (a->digits[abs_width-i-1] < b->digits[abs_width-i-1]) return -sign; /* right is bigger */
	}
	return 0; /* they are the same */
}

/**
 * @brief Compare two long values, ignoring their signs.
 *
 * Compares the absolute values of two longs. Basically the same
 * as @c krk_long_compare but without using the signs in the comparison...
 */
static int krk_long_compare_abs(const KrkLong * a, const KrkLong * b) {
	size_t a_width = a->width < 0 ? -a->width : a->width;
	size_t b_width = b->width < 0 ? -b->width : b->width;
	if (a_width > b_width) return 1;
	if (b_width > a_width) return -1;
	size_t abs_width = a_width;
	for (size_t i = 0; i < abs_width; ++i) {
		if (a->digits[abs_width-i-1] > b->digits[abs_width-i-1]) return 1; /* left is bigger */
		if (a->digits[abs_width-i-1] < b->digits[abs_width-i-1]) return -1; /* right is bigger */
	}
	return 0; /* they are the same */
}

/**
 * @brief Add the absolute value of two longs to make a third.
 *
 * @p res must be initialized to 0 beforehand and will have a positive value on return.
 * It may not be the same value as @p a or @p b.
 */
static int krk_long_add_ignore_sign(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	size_t awidth = a->width < 0 ? -a->width : a->width;
	size_t bwidth = b->width < 0 ? -b->width : b->width;
	size_t owidth = awidth < bwidth ? bwidth + 1 : awidth + 1;
	size_t carry  = 0;
	krk_long_resize(res, owidth);
	for (size_t i = 0; i < owidth - 1; ++i) {
		uint32_t out = (i < awidth ? a->digits[i] : 0) + (i < bwidth ? b->digits[i] : 0) + carry;
		res->digits[i] = out & DIGIT_MAX;
		carry = out > DIGIT_MAX;
	}
	if (carry) {
		res->digits[owidth-1] = 1;
	} else {
		krk_long_resize(res, owidth - 1);
	}
	return 0;
}

/**
 * @brief Subtract a smaller number from a bigger number.
 *
 * Performs res = |a|-|b|, assuming |a|>|b|.
 */
static int _sub_big_small(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	/* Subtract b from a, where a is bigger */
	size_t awidth = a->width < 0 ? -a->width : a->width;
	size_t bwidth = b->width < 0 ? -b->width : b->width;
	size_t owidth = awidth;

	krk_long_resize(res, owidth);

	int carry = 0;

	for (size_t i = 0; i < owidth; ++i) {
		/* We'll do long subtraction? */
		int64_t a_digit = (int64_t)(i < awidth ? a->digits[i] : 0) - carry;
		int64_t b_digit = i < bwidth ? b->digits[i] : 0;
		if (a_digit < b_digit) {
			a_digit += (int64_t)1 << DIGIT_SHIFT;
			carry = 1;
		} else {
			carry = 0;
		}

		res->digits[i] = (a_digit - b_digit) & DIGIT_MAX;
	}

	krk_long_trim(res);

	return 0;
}

/**
 * @brief Quickly swap the contents of two initialized longs.
 */
static int _swap(KrkLong * a, KrkLong * b) {
	ssize_t width = a->width;
	uint32_t * digits = a->digits;
	a->width = b->width;
	a->digits = b->digits;
	b->width = width;
	b->digits = digits;
	return 0;
}

/* Macros for handling cases where res = a or res = b */
#define PREP_OUTPUT(res,a,b) KrkLong _tmp_out_ ## res, *_swap_out_ ## res = NULL; do { if (res == a || res == b) { krk_long_init_si(&_tmp_out_ ## res, 0); _swap_out_ ## res = res; res = &_tmp_out_ ## res; } } while (0)
#define PREP_OUTPUT1(res,a) KrkLong _tmp_out_ ## res, *_swap_out_ ## res = NULL; do { if (res == a) { krk_long_init_si(&_tmp_out_ ## res, 0); _swap_out_ ## res = res;  res = &_tmp_out_ ## res; } } while (0)
#define FINISH_OUTPUT(res) do { if (_swap_out_ ## res) { _swap(_swap_out_ ## res, res); krk_long_clear(&_tmp_out_ ## res); } } while (0)

/**
 * @brief Public interface to perform addition.
 *
 * Adds @p a and @p b together into @p res.
 */
static int krk_long_add(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(res,a,b);

	if (a->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,b);
		FINISH_OUTPUT(res);
		return 0;
	} else if (b->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,a);
		FINISH_OUTPUT(res);
		return 0;
	}

	if (a->width < 0 && b->width > 0) {
		switch (krk_long_compare_abs(a,b)) {
			case -1:
				_sub_big_small(res,b,a);
				krk_long_set_sign(res,1);
				FINISH_OUTPUT(res);
				return 0;
			case 1:
				_sub_big_small(res,a,b);
				krk_long_set_sign(res,-1);
				FINISH_OUTPUT(res);
				return 0;
		}
		krk_long_clear(res);
		FINISH_OUTPUT(res);
		return 0;
	} else if (a->width > 0 && b->width < 0) {
		switch (krk_long_compare_abs(a,b)) {
			case -1:
				_sub_big_small(res,b,a);
				krk_long_set_sign(res,-1);
				FINISH_OUTPUT(res);
				return 0;
			case 1:
				_sub_big_small(res,a,b);
				krk_long_set_sign(res,1);
				FINISH_OUTPUT(res);
				return 0;
		}
		krk_long_clear(res);
		FINISH_OUTPUT(res);
		return 0;
	}

	/* sign must match for this, so take it from whichever */
	int sign = a->width < 0 ? -1 : 1;
	if (krk_long_add_ignore_sign(res,a,b)) {
		FINISH_OUTPUT(res);
		return 1;
	}
	krk_long_set_sign(res,sign);
	FINISH_OUTPUT(res);
	return 0;
}

/**
 * @brief Public interface to perform subtraction.
 *
 * Subtracts @p b from @p b into @p res.
 */
static int krk_long_sub(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(res,a,b);
	if (a->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,b);
		krk_long_set_sign(res, b->width < 0 ? 1 : -1);
		FINISH_OUTPUT(res);
		return 0;
	} else if (b->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,a);
		FINISH_OUTPUT(res);
		return 0;
	}

	if ((a->width < 0) != (b->width < 0)) {
		if (krk_long_add_ignore_sign(res,a,b)) { FINISH_OUTPUT(res); return 1; }
		krk_long_set_sign(res,a->width < 0 ? -1 : 1);
		FINISH_OUTPUT(res);
		return 0;
	}

	/* Which is bigger? */
	switch (krk_long_compare_abs(a,b)) {
		case 0:
			krk_long_clear(res);
			FINISH_OUTPUT(res);
			return 0;
		case 1:
			_sub_big_small(res,a,b);
			if (a->width < 0) krk_long_set_sign(res, -1);
			FINISH_OUTPUT(res);
			return 0;
		case -1:
			_sub_big_small(res,b,a);
			if (b->width > 0) krk_long_set_sign(res, -1);
			FINISH_OUTPUT(res);
			return 0;
	}

	__builtin_unreachable();
}

/**
 * @brief Zero all of the digits in @p num
 *
 * @p num remains untrimmed and will maintain these extra zeros.
 */
static int krk_long_zero(KrkLong * num) {
	size_t abs_width = num->width < 0 ? -num->width : num->width;
	for (size_t i = 0; i < abs_width; ++i) {
		num->digits[i] = 0;
	}
	return 0;
}

/**
 * @brief Multiply the absolute values of two longs.
 *
 * Performs a simple chalkboard long multiplication, using the
 * result as the accumulator.
 *
 * @p res must be initialized, but will be resized and zeroed on entry; it
 * must not be equal to either of @p a or @p b.
 */
static int _mul_abs(KrkLong * res, const KrkLong * a, const KrkLong * b) {

	size_t awidth = a->width < 0 ? -a->width : a->width;
	size_t bwidth = b->width < 0 ? -b->width : b->width;

	krk_long_resize(res, awidth+bwidth);
	krk_long_zero(res);

	for (size_t i = 0; i < bwidth; ++i) {
		uint64_t b_digit = b->digits[i];
		uint64_t carry = 0;
		for (size_t j = 0; j < awidth; ++j) {
			uint64_t a_digit = a->digits[j];
			uint64_t tmp = carry + a_digit * b_digit + res->digits[i+j];
			carry = tmp >> DIGIT_SHIFT;
			res->digits[i+j] = tmp & DIGIT_MAX;
		}
		res->digits[i + awidth] = carry;
	}

	krk_long_trim(res);

	return 0;
}

/**
 * @brief Public interface for multiplication.
 *
 * Multiplies @p a by @p b and places the result in @p res.
 */
static int krk_long_mul(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(res,a,b);

	if (a->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,a);
		FINISH_OUTPUT(res);
		return 0;
	}

	if (b->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,b);
		FINISH_OUTPUT(res);
		return 0;
	}

	if (_mul_abs(res,a,b)) {
		FINISH_OUTPUT(res);
		return 1;
	}

	if ((a->width < 0) == (b->width < 0)) {
		krk_long_set_sign(res,1);
	} else {
		krk_long_set_sign(res,-1);
	}

	FINISH_OUTPUT(res);
	return 0;
}

/**
 * @brief Shift a long one bit left.
 *
 * Shifts @p in left one bit in-place, resizing as needed.
 */
static int _lshift_one(KrkLong * in) {
	if (in->width == 0) {
		return 0;
	}

	size_t abs_width = in->width < 0 ? -in->width : in->width;
	size_t out_width = abs_width;

	if (in->digits[abs_width-1] >> (DIGIT_SHIFT - 1)) {
		out_width += 1;
	}

	krk_long_resize(in, out_width);

	int carry = 0;

	for (size_t i = 0; i < abs_width; ++i) {
		uint32_t digit = in->digits[i];
		in->digits[i] = ((digit << 1) + carry) & DIGIT_MAX;
		carry = (digit >> (DIGIT_SHIFT -1));
	}

	if (carry) {
		in->digits[out_width-1] = 1;
	}

	return 0;
}

/**
 * @brief Calculate the highest set bit of a long.
 *
 * TODO obviously since this returns a 'size_t', we only support
 *      numbers with that many bits, which is probably 1/8th
 *      what we could theoretically support, but whatever...
 */
static size_t _bits_in(const KrkLong * num) {
	if (num->width == 0) return 0;

	size_t abs_width = num->width < 0 ? -num->width : num->width;

	/* Top bit in digits[abs_width-1] */
	size_t c = 0;
	uint32_t digit = num->digits[abs_width-1];
	while (digit) {
		c++;
		digit >>= 1;
	}

	return c + (abs_width-1) * DIGIT_SHIFT;
}

/**
 * @brief Check if a given bit is a set in a long.
 */
static size_t _bit_is_set(const KrkLong * num, size_t bit) {
	size_t digit_offset = bit / DIGIT_SHIFT;
	size_t digit_bit    = bit % DIGIT_SHIFT;
	return !!(num->digits[digit_offset] & (1 << digit_bit));
}

/**
 * @brief Set or unset the least significant bit of a long.
 */
static int _bit_set_zero(KrkLong * num, int val) {
	if (num->width == 0) {
		krk_long_clear(num);
		krk_long_init_si(num, !!val);
		return 0;
	}

	num->digits[0] = (num->digits[0] & ~1) | (!!val);
	return 0;
}

/**
 * @brief Set a given bit in a long.
 *
 * Because of how we use it, this only supports setting the bit,
 * eg. it performs: num |= (1 << bit)
 *
 * @p num will be resized as needed.
 */
static int krk_long_bit_set(KrkLong * num, size_t bit) {
	size_t abs_width = num->width < 0 ? -num->width : num->width;
	size_t digit_offset = bit / DIGIT_SHIFT;
	size_t digit_bit    = bit % DIGIT_SHIFT;

	if (digit_offset >= abs_width) {
		krk_long_resize(num, digit_offset+1);
		for (size_t i = abs_width; i < digit_offset + 1; ++i) {
			num->digits[i] = 0;
		}
	}

	num->digits[digit_offset] |= (1 << digit_bit);
	return 0;
}

/**
 * @brief Internal division implementation.
 *
 * Divides @p |a| by @p |b| placing the remainder in @p rem and the quotient in @p quot.
 *
 * Performs one of two division operations. For single-digit divisor, we perform
 * a quicker division. For anything else, performs a binary long division, which
 * is terribly slow, but is at least correct...
 *
 * @return 1 if divisor is 0, otherwise 0.
 */
static int _div_abs(KrkLong * quot, KrkLong * rem, const KrkLong * a, const KrkLong * b) {
	/* quot = a / b; rem = a % b */

	/* Zero quotiant and remainder */
	krk_long_clear(quot);
	krk_long_clear(rem);

	if (b->width == 0) return 1; /* div by zero */
	if (a->width == 0) return 0; /* div of zero */

	size_t awidth = a->width < 0 ? -a->width : a->width;
	size_t bwidth = b->width < 0 ? -b->width : b->width;

	if (bwidth == 1 && b->digits[0] == 1) {
		krk_long_init_copy(quot, a);
		krk_long_set_sign(quot, 1);
		return 0;
	}

	if (awidth < bwidth) {
		krk_long_init_copy(rem, a);
		krk_long_set_sign(rem, 1);
		return 0;
	}

	KrkLong absa, absb;
	krk_long_init_copy(&absa, a);
	krk_long_set_sign(&absa, 1);
	krk_long_init_copy(&absb, b);
	krk_long_set_sign(&absb, 1);

	if (bwidth == 1) {
		uint64_t remainder = 0;
		for (size_t i = 0; i < awidth; ++i) {
			size_t _i = awidth - i - 1;
			remainder = (remainder << DIGIT_SHIFT) | absa.digits[_i];
			absa.digits[_i] = (uint32_t)(remainder / absb.digits[0]) & DIGIT_MAX;
			remainder -= (uint64_t)(absa.digits[_i]) * absb.digits[0];
		}

		krk_long_init_si(rem, remainder);
		_swap(quot, &absa);
		krk_long_trim(quot);

		krk_long_clear_many(&absa, &absb, NULL);
		return 0;
	}

	size_t bits = _bits_in(a);
	for (size_t i = 0; i < bits; ++i) {
		size_t _i = bits - i - 1;

		/* Shift remainder by one */
		_lshift_one(rem);

		int is_set = _bit_is_set(&absa, _i);
		_bit_set_zero(rem, is_set);
		if (krk_long_compare(rem,&absb) >= 0) {
			_sub_big_small(rem,rem,&absb);
			krk_long_bit_set(quot, _i);
		}
	}

	krk_long_trim(quot);
	krk_long_clear_many(&absa,&absb,NULL);

	return 0;
}

/**
 * @brief Public interface to division/modulo.
 *
 * Also does the Python-defined flooring / remainder inversion.
 *
 * @p quot or @p rem may be equal to @p a or @p b and I sure hope I got all
 * the temporary swap value juggling done correctly...
 *
 * @return 1 if @p b is 0, 0 otherwise.
 */
static int krk_long_div_rem(KrkLong * quot, KrkLong * rem, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(quot,a,b);
	PREP_OUTPUT(rem,a,b);
	if (_div_abs(quot,rem,a,b)) {
		FINISH_OUTPUT(rem);
		FINISH_OUTPUT(quot);
		return 1;
	}

	if ((a->width < 0) != (b->width < 0)) {
		/* Round down if remainder */
		if (rem->width) {
			KrkLong one;
			krk_long_init_si(&one, 1);
			krk_long_add(quot, quot, &one);
			_sub_big_small(rem, b, rem);
			krk_long_clear(&one);
		}

		/* Signs are different, negate and round down if necessary */
		krk_long_set_sign(quot, -1);
	}

	if (b->width < 0) {
		krk_long_set_sign(rem, -1);
	}

	FINISH_OUTPUT(rem);
	FINISH_OUTPUT(quot);
	return 0;
}

/**
 * @brief Get the absolute value of a long.
 *
 * Basically just copies @p in to @p out and sets @p out 's sign to 1.
 */
static int krk_long_abs(KrkLong * out, const KrkLong * in) {
	PREP_OUTPUT1(out,in);
	krk_long_clear(out);
	krk_long_init_copy(out, in);
	krk_long_set_sign(out, 1);
	FINISH_OUTPUT(out);
	return 0;
}

/**
 * @brief Get the "sign" of a long.
 *
 * @return 0 if @p num == 0, -1 if @p num < 0, 1 if @p num > 0
 */
static int krk_long_sign(const KrkLong * num) {
	if (num->width == 0) return 0;
	return num->width < 0 ? -1 : 1;
}

/**
 * @brief Estimate how many digits are needed to convert a long to a base.
 *
 * Dumb. Should be exact for a handful of powers of two, overestimates for
 * everything else, so deal with that on your own...
 */
size_t krk_long_digits_in_base(KrkLong * num, int base) {
	if (num->width == 0) return 1;

	size_t bits = _bits_in(num);

	if (base <  4)  return bits;
	if (base <  8)  return (bits+1)/2;
	if (base < 16)  return (bits+2)/3;
	if (base == 16) return (bits+3)/4;
	return 0;
}

/**
 * @brief Convert a long with up to 2 digits to a 64-bit value.
 */
static int64_t krk_long_medium(KrkLong * num) {
	if (num->width == 0) return 0;

	if (num->width < 0) {
		uint64_t val = num->digits[0];
		if (num->width < 1) {
			val |= (num->digits[1]) << 31;
		}
		return -val;
	} else {
		uint64_t val = num->digits[0];
		if (num->width > 1) {
			val |= (num->digits[1]) << 31;
		}
		return val;
	}
}

/**
 * @brief Implementation for multiple bitwise operators.
 *
 * Supports or, xor, (and) and. Does in-place twos-complement for
 * negatives - I thought that was pretty neat, but it's probably
 * slower than just doing it afterwards...
 */
static int do_bin_op(KrkLong * res, const KrkLong * a, const KrkLong * b, char op) {
	size_t awidth = a->width < 0 ? -a->width : a->width;
	size_t bwidth = b->width < 0 ? -b->width : b->width;
	size_t owidth = ((awidth > bwidth) ? awidth : bwidth) + 1;

	int aneg = (a->width < 0);
	int bneg = (b->width < 0);
	int rneg = 0;

	switch (op) {
		case '|': rneg = aneg | bneg; break;
		case '^': rneg = aneg ^ bneg; break;
		case '&': rneg = aneg & bneg; break;
	}

	krk_long_resize(res, owidth);

	int acarry = aneg ? 1 : 0;
	int bcarry = bneg ? 1 : 0;
	int rcarry = rneg ? 1 : 0;

	for (size_t i = 0; i < owidth; ++i) {
		uint32_t a_digit = (i < awidth ? a->digits[i] : 0);
		a_digit = aneg ? ((a_digit ^ DIGIT_MAX) + acarry) : a_digit;
		acarry = a_digit >> DIGIT_SHIFT;

		uint32_t b_digit = (i < bwidth ? b->digits[i] : 0);
		b_digit = bneg ? ((b_digit ^ DIGIT_MAX) + bcarry) : b_digit;
		bcarry = b_digit >> DIGIT_SHIFT;

		uint32_t r;
		switch (op) {
			case '|': r = a_digit | b_digit; break;
			case '^': r = a_digit ^ b_digit; break;
			case '&': r = a_digit & b_digit; break;
			default: __builtin_unreachable();
		}

		r = rneg ? (((r & DIGIT_MAX) ^ DIGIT_MAX) + rcarry) : r;
		res->digits[i] = r & DIGIT_MAX;
		rcarry = r >> DIGIT_SHIFT;
	}

	krk_long_trim(res);

	if (rneg) {
		krk_long_set_sign(res,-1);
	}

	return 0;
}

/**
 * @brief Public interface for bitwise or.
 */
static int krk_long_or(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(res,a,b);
	if (a->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,b);
		FINISH_OUTPUT(res);
		return 0;
	} else if (b->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,a);
		FINISH_OUTPUT(res);
		return 0;
	}

	int out = do_bin_op(res,a,b,'|');
	FINISH_OUTPUT(res);
	return out;
}

/**
 * @brief Public interface for bitwise xor.
 */
static int krk_long_xor(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(res,a,b);
	int out = do_bin_op(res,a,b,'^');
	FINISH_OUTPUT(res);
	return out;
}

/**
 * @brief Public interface for bitwise and.
 */
static int krk_long_and(KrkLong * res, const KrkLong * a, const KrkLong * b) {
	PREP_OUTPUT(res,a,b);
	if (a->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,a);
		FINISH_OUTPUT(res);
		return 0;
	} else if (b->width == 0) {
		krk_long_clear(res);
		krk_long_init_copy(res,b);
		FINISH_OUTPUT(res);
		return 0;
	}

	int out = do_bin_op(res,a,b,'&');
	FINISH_OUTPUT(res);
	return out;
}

/**
 * Small divisor in-place division specifically for printers.
 */
static uint32_t _div_inplace(KrkLong * a, uint32_t base) {
	if (a->width == 0) {
		return 0;
	}
	size_t awidth = a->width;
	uint64_t remainder = 0;
	for (size_t i = 0; i < awidth; ++i) {
		size_t _i = awidth - i - 1;
		remainder = (remainder << DIGIT_SHIFT) | a->digits[_i];
		a->digits[_i] = (uint32_t)(remainder / base) & DIGIT_MAX;
		remainder -= (uint64_t)(a->digits[_i]) * base;
	}

	krk_long_trim(a);
	return remainder;
}

static const char _vals[] = "0123456789abcdef";
static char * _fast_conversion(const KrkLong * abs, unsigned int bits, char * writer) {
	uint64_t buf  = abs->digits[0];
	uint32_t cnt  = DIGIT_SHIFT;
	ssize_t  ind  = 1;
	uint32_t out  = 0;

	while (ind < abs->width || buf) {
		if (ind < abs->width && cnt < bits) {
			buf |= (uint64_t)abs->digits[ind] << (uint64_t)cnt;
			ind++;
			cnt += DIGIT_SHIFT;
		}

		out = buf & ((1 << bits) - 1);
		cnt -= bits;
		buf >>= bits;

		*writer++ = _vals[out];
	}

	return writer;
}

/**
 * @brief Convert a long to a string in a given base.
 */
static char * krk_long_to_str(const KrkLong * n, int _base, const char * prefix, size_t *size, uint32_t *_hash) {
	KrkLong abs;

	krk_long_init_si(&abs, 0);
	krk_long_abs(&abs, n);

	int sign = krk_long_sign(n);   /* -? +? 0? */

	size_t len = (sign == -1 ? 1 : 0) + krk_long_digits_in_base(&abs,_base) + strlen(prefix) + 1;
	char * tmp = malloc(len);
	char * writer = tmp;

	if (sign == 0) {
		*writer++ = '0';
	} else {
		switch (_base) {
			case 2:  writer = _fast_conversion(&abs,1,writer); break;
			case 4:  writer = _fast_conversion(&abs,2,writer); break;
			case 8:  writer = _fast_conversion(&abs,3,writer); break;
			case 16: writer = _fast_conversion(&abs,4,writer); break;
			default:
				while (krk_long_sign(&abs) > 0) {
					uint32_t rem = _div_inplace(&abs,_base);
					*writer++ = _vals[rem];
				}
		}
	}

	while (*prefix) { *writer++ = *prefix++; }
	if (sign < 0) *writer++ = '-';

	char * rev = malloc(len);
	char * out = rev;
	uint32_t hash = 0;
	while (writer != tmp) {
		*out = *--writer;
		krk_hash_advance(hash,*out);
		out++;
	}
	*out = '\0';
	*_hash = hash;

	free(tmp);

	krk_long_clear(&abs);
	*size = strlen(rev);

	return rev;
}

static const unsigned char _convert_table[256] = {
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,255,255,255,255,255,255,
255, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,255,255,255,255,255,
255, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};

static int is_valid(int base, uint8_t c) {
	return _convert_table[c] < base;
}

static int convert_digit(uint8_t c) {
	return _convert_table[c];
}

static int is_whitespace(char c) {
	/* Same set we use in str.strip() by default */
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

/**
 * @brief Parse a number into a long.
 *
 * This is now the main number parse for the compiler as well as being
 * used when you pass a string to @c int or @c long.
 *
 * @return 0 on success, something else on failure.
 */
static int krk_long_parse_string(const char * str, KrkLong * num, unsigned int base, size_t len) {
	const char * end = str + len;
	const char * c = str;
	int sign = 1;

	/* Skip any leading whitespace */
	while (c < end && is_whitespace(*c)) c++;

	/* Trim any trailing whitespace */
	while (end > c && is_whitespace(end[-1])) end--;

	/* If there's nothing here, that's invalid. */
	if (c >= end) {
		return 1;
	}

	if (*c == '-') {
		sign = -1;
		c++;
	} else if (*c == '+') {
		c++;
	}

	/* Just a sign is not valid... */
	if (c >= end) {
		return 1;
	}

	/* If base was not specified, accept a prefix. */
	if (base == 0) {
		base = 10;
		if (*c == '0') {
			c++;

			if (c == end) {
				/* If we saw just '0', that's fine... */
				krk_long_init_si(num, 0);
				return 0;
			}

			if (*c == 'x' || *c == 'X') {
				base = 16;
				c++;
			} else if (*c == 'o' || *c == 'O') {
				base = 8;
				c++;
			} else if (*c == 'b' || *c == 'B') {
				base = 2;
				c++;
			} else {
				return 2;
			}
		}
	}

	if (c >= end) {
		return 1;
	}

	if (base == 1 || base > 36) {
		return 2;
	}

	krk_long_init_si(num, 0);

	if (base == 2 || base == 4 || base == 8 || base == 16 || base == 32) {
		size_t bits = 0;
		switch (base) {
			case 2: bits = 1; break;
			case 4: bits = 2; break;
			case 8: bits = 3; break;
			case 16:bits = 4; break;
			case 32:bits = 5; break;
		}
		size_t digits = 0;
		for (const char * x = c; x < end; ++x) {
			if (*x == '_') continue;
			if (unlikely(!is_valid(base, *x))) {
				krk_long_clear(num);
				return 1;
			}
			digits++;
		}

		if (!digits) {
			krk_long_clear(num);
			return 1;
		}

		size_t digit_offset = (digits * bits - 1) / DIGIT_SHIFT;
		krk_long_resize(num, digit_offset + 1);

		uint32_t cnt = 0;
		uint64_t buf = 0;
		const char * x = end;
		size_t i = 0;

		while (x != c && x[-1] == '_') x--;

		while (x != c || buf) {
			while (cnt < DIGIT_SHIFT && x > c) {
				buf |=  (uint64_t)convert_digit(x[-1]) << cnt;
				cnt += bits;
				x--;
				while (x != c && x[-1] == '_') x--;
			}

			num->digits[i++] = buf & DIGIT_MAX;
			cnt -= DIGIT_SHIFT;
			buf >>= DIGIT_SHIFT;
		}

		krk_long_trim(num);
	} else {
		KrkLong _base, scratch;
		krk_long_init_si(&_base, 0);
		krk_long_init_si(&scratch, 0);

		while (c < end && *c) {
			uint64_t accum = 0;
			uint64_t basediv = 1;
			while (c < end && *c && (basediv * base < 0x10000000000000UL)) {
				if (*c == '_') { c++; continue; }
				if (!is_valid(base, *c)) {
					krk_long_clear_many(&_base, &scratch, num, NULL);
					return 1;
				}

				basediv *= base;
				accum *= base;
				accum += convert_digit(*c);
				c++;
			}
			krk_long_init_ui(&_base, basediv);
			krk_long_mul(num, num, &_base);
			krk_long_clear_many(&scratch, &_base, NULL);
			krk_long_init_ui(&scratch, accum);
			krk_long_add(num, num, &scratch);
		}

		krk_long_clear_many(&_base, &scratch, NULL);
	}

	if (sign == -1) {
		krk_long_set_sign(num, -1);
	}

	return 0;
}

typedef KrkLong krk_long[1];

struct BigInt {
	KrkInstance inst;
	krk_long value;
};

#define AS_long(o) ((struct BigInt *)AS_OBJECT(o))
#define IS_long(o) (krk_isInstanceOf(o, KRK_BASE_CLASS(long)))

#define CURRENT_CTYPE struct BigInt *
#define CURRENT_NAME  self

static KrkValue make_long(krk_integer_type t) {
	struct BigInt * self = (struct BigInt*)krk_newInstance(KRK_BASE_CLASS(long));
	krk_push(OBJECT_VAL(self));
	krk_long_init_si(self->value, t);
	return krk_pop();
}

static void _long_gcsweep(KrkInstance * self) {
	krk_long_clear(((struct BigInt*)self)->value);
}

#ifndef KRK_NO_FLOAT
KrkValue krk_int_from_float(double val);
#endif

KRK_StaticMethod(long,__new__) {
	FUNCTION_TAKES_AT_MOST(2);
	/* Some less likely scenarios */
	if (argc < 2) {
		return make_long(0);
	} else if (IS_INTEGER(argv[1])) {
		return make_long(AS_INTEGER(argv[1]));
	} else if (IS_BOOLEAN(argv[1])) {
		return make_long(AS_BOOLEAN(argv[1]));
#ifndef KRK_NO_FLOAT
	} else if (IS_FLOATING(argv[1])) {
		return krk_int_from_float(AS_FLOATING(argv[1]));
#endif
	} else if (IS_STRING(argv[1])) {
		/* XXX This should probably work like int(...) does and default to base 10... and take a base at all... */
		struct BigInt * self = (struct BigInt*)krk_newInstance(KRK_BASE_CLASS(long));
		krk_push(OBJECT_VAL(self));
		if (krk_long_parse_string(AS_CSTRING(argv[1]),self->value,0,AS_STRING(argv[1])->length)) {
			return krk_runtimeError(vm.exceptions->valueError, "invalid literal for long() with base 0: %R", argv[1]);
		}
		return krk_pop();
	} else if (IS_long(argv[1])) {
		struct BigInt * self = (struct BigInt*)krk_newInstance(KRK_BASE_CLASS(long));
		krk_push(OBJECT_VAL(self));
		krk_long_init_copy(self->value,AS_long(argv[1])->value);
		return krk_pop();
	} else {
		return krk_runtimeError(vm.exceptions->typeError, "%s() argument must be a string or a number, not '%T'", "int", argv[1]);
	}
}

#ifndef KRK_NO_FLOAT
/**
 * Float conversions.
 *
 * CPython does a thing with ldexp, but I don't think I have an implementation
 * of that in ToaruOS's libc, and I consider that a blocker to doing the same here.
 *
 * @warning These are probably inaccurate.
 */
static double krk_long_get_double(const KrkLong * value) {
	size_t awidth = value->width < 0 ? -value->width : value->width;
	if (awidth == 0) return 0.0;

	uint64_t sign = value->width < 0 ? 1 : 0;

	/* We only need the top two digits if this value is normalized */
	uint64_t mantissa = 0;
	uint64_t high = value->digits[awidth-1];
	uint64_t med  = awidth > 1 ? value->digits[awidth-2] : 0;
	uint64_t low  = awidth > 2 ? value->digits[awidth-3] : 0;

	int s = 0;
	for (s = DIGIT_SHIFT; s >= 0; s--) {
		if (high & (1 << s)) break;
	}

	int high_shift = 52 - s;
	int med_shift  = 21 - s;
	int low_shift  = 10 + s;

	mantissa |= high << high_shift;
	mantissa |= med_shift >= 0 ? (med << med_shift) : (med >> -med_shift);
	mantissa |= low >> low_shift;

	mantissa &= 0xfffffffffffffUL;

	uint64_t exp = (s + (awidth - 1) * DIGIT_SHIFT) + 0x3FF;

	if (exp > 0x7Fe) {
		krk_runtimeError(vm.exceptions->valueError, "overflow, too large for float conversion");
		return 0.0;
	}

	uint64_t val = (sign << 63) | (exp << 52) | mantissa;

	union { uint64_t asInt; double asDbl; } u = {val};
	return u.asDbl;
}

KRK_Method(long,__float__) {
	return FLOATING_VAL(krk_long_get_double(self->value));
}

static KrkValue _krk_long_truediv(KrkLong * _top, KrkLong * _bottom) {
	if (_bottom->width == 0) return krk_runtimeError(vm.exceptions->valueError, "float division by zero");
	if (_top->width == 0) return FLOATING_VAL(0);

	KrkLong rem, top, bottom;
	krk_long_init_si(&rem, 0);
	krk_long_init_copy(&top, _top);
	krk_long_init_copy(&bottom, _bottom);

	/* Take sign from original inputs */
	int negative = (krk_long_sign(&top) < 0) != (krk_long_sign(&bottom) < 0);

	/* And then make top and bottom absolute */
	krk_long_set_sign(&top, 1);
	krk_long_set_sign(&bottom, 1);

	/* Final outputs that go into the floats */
	uint64_t quot = 0;
	long long exp = 0;

#define NEEDED_BITS 53
	int bits_wanted = NEEDED_BITS;
	size_t bits = _bits_in(&top);
	for (ssize_t i = 0; bits_wanted >= 0; ++i) {
		ssize_t _i = bits - i - 1;
		_lshift_one(&rem);
		_bit_set_zero(&rem, (_i >= 0 ? _bit_is_set(&top, _i) : 0));
		if (krk_long_compare(&rem,&bottom) >= 0) {
			if (bits_wanted == NEEDED_BITS) {
				exp = 1023 + (bits - i - 1);
			}
			_sub_big_small(&rem,&rem,&bottom);
			quot |= (1ULL << bits_wanted);
			bits_wanted--;
		} else if (bits_wanted != NEEDED_BITS) {
			bits_wanted -= 1;
		}
	}
#undef NEEDED_BITS

	krk_long_clear_many(&rem, &top, &bottom, NULL);

	/* Handle rounding? This is probably wrong. */
	quot = (quot + 1) >> 1;
	if (exp > 2046) {
		/* Saturated maximum, but not infinity */
		quot = 0x1fffffffffffffULL;
		exp = 2046;
	} else if (exp < 1 && exp >= -52) {
		/* Subnormals */
		quot >>= -exp+1;
		quot |= 0x10000000000000ULL;
		exp = 0;
	} else if (exp < -52) {
		/* Beyond subnormal, truncate to zero */
		quot = 0x10000000000000ULL;
		exp = 0;
	}

	/* Apply sign */
	if (negative) exp |= 2048;

	/* Mash bits together to form double */
	quot ^= 1ULL << 52;
	quot |= exp << 52;
	union { double d; uint64_t u; } val = {.u = quot};

	return FLOATING_VAL(val.d);
}

static KrkValue checked_float_div(double top, double bottom) {
	if (unlikely(bottom == 0.0)) return krk_runtimeError(vm.exceptions->valueError, "float division by zero");
	return FLOATING_VAL(top/bottom);
}

KRK_Method(long,__truediv__) {
	krk_long tmp;
	if (IS_long(argv[1])) krk_long_init_copy(tmp, AS_long(argv[1])->value);
	else if (IS_INTEGER(argv[1])) krk_long_init_si(tmp, AS_INTEGER(argv[1]));
	else if (IS_FLOATING(argv[1])) return checked_float_div(krk_long_get_double(self->value), AS_FLOATING(argv[1]));
	else return NOTIMPL_VAL();
	return _krk_long_truediv(self->value,tmp);
}

KRK_Method(long,__rtruediv__) {
	krk_long tmp;
	if (IS_long(argv[1])) krk_long_init_copy(tmp, AS_long(argv[1])->value);
	else if (IS_INTEGER(argv[1])) krk_long_init_si(tmp, AS_INTEGER(argv[1]));
	else if (IS_FLOATING(argv[1])) return checked_float_div(AS_FLOATING(argv[1]), krk_long_get_double(self->value));
	else return NOTIMPL_VAL();
	return _krk_long_truediv(tmp,self->value);
}
#endif

#define PRINTER(name,base,prefix) \
	KRK_Method(long,__ ## name ## __) { \
		size_t size; \
		uint32_t hash; \
		char * rev = krk_long_to_str(self->value, base, prefix, &size, &hash); \
		return OBJECT_VAL(krk_takeStringVetted(rev,size,size,KRK_OBJ_FLAGS_STRING_ASCII,hash)); \
	}

PRINTER(hex,16,"x0")
PRINTER(oct,8,"o0")
PRINTER(bin,2,"b0")

KRK_Method(long,__hash__) {
	return INTEGER_VAL((uint32_t)(krk_long_medium(self->value)));
}

static KrkValue make_long_obj(KrkLong * val) {
	krk_integer_type maybe = 0;
	if (val->width == 0) {
		maybe = 0;
	} else if (val->width == 1) {
		maybe = val->digits[0];
	} else if (val->width == -1) {
		maybe = -(int64_t)val->digits[0];
	} else if (val->width == 2 && (val->digits[1] & 0xFFFF0000) == 0) {
		maybe = ((uint64_t)val->digits[1] << 31) | val->digits[0];
	} else if (val->width == -2 && (val->digits[1] & 0xFFFF0000) == 0) {
		maybe = -(((uint64_t)val->digits[1] << 31) | val->digits[0]);
	} else {
		krk_push(OBJECT_VAL(krk_newInstance(KRK_BASE_CLASS(long))));
		*AS_long(krk_peek(0))->value = *val;
		return krk_pop();
	}

	krk_long_clear(val);
	return INTEGER_VAL(maybe);
}

KrkValue krk_parse_int(const char * start, size_t width, unsigned int base) {
	KrkLong _value;
	if (krk_long_parse_string(start, &_value, base, width)) {
		return NONE_VAL();
	}

	return make_long_obj(&_value);
}

KRK_Method(long,__int__) {
	return INTEGER_VAL(krk_long_medium(self->value));
}

#define BASIC_BIN_OP_FLOATS(name, long_func, MAYBE_FLOAT, MAYBE_FLOAT_INV) \
	KRK_Method(long,__ ## name ## __) { \
		krk_long tmp; \
		if (IS_long(argv[1])) krk_long_init_copy(tmp, AS_long(argv[1])->value); \
		else if (IS_INTEGER(argv[1])) krk_long_init_si(tmp, AS_INTEGER(argv[1])); \
		MAYBE_FLOAT \
		else return NOTIMPL_VAL(); \
		long_func(tmp,self->value,tmp); \
		return make_long_obj(tmp); \
	} \
	KRK_Method(long,__r ## name ## __) { \
		krk_long tmp; \
		if (IS_long(argv[1])) krk_long_init_copy(tmp, AS_long(argv[1])->value); \
		else if (IS_INTEGER(argv[1])) krk_long_init_si(tmp, AS_INTEGER(argv[1])); \
		MAYBE_FLOAT_INV \
		else return NOTIMPL_VAL(); \
		long_func(tmp,tmp,self->value); \
		return make_long_obj(tmp); \
	} \
	_noexport \
	KrkValue krk_long_coerced_ ## name (krk_integer_type a, krk_integer_type b) { \
		krk_long tmp_res, tmp_a, tmp_b; \
		krk_long_init_si(tmp_res, 0); \
		krk_long_init_si(tmp_a, a); \
		krk_long_init_si(tmp_b, b); \
		long_func(tmp_res, tmp_a, tmp_b); \
		krk_long_clear_many(tmp_a, tmp_b, NULL); \
		return make_long_obj(tmp_res); \
	}

#define BASIC_BIN_OP(a,b) BASIC_BIN_OP_FLOATS(a,b,,)
#ifndef KRK_NO_FLOAT
#define FLOAT_A(op) else if (IS_FLOATING(argv[1])) return FLOATING_VAL(krk_long_get_double(self->value) op AS_FLOATING(argv[1]));
#define FLOAT_B(op) else if (IS_FLOATING(argv[1])) return FLOATING_VAL(AS_FLOATING(argv[1]) op krk_long_get_double(self->value));
#else
#define FLOAT_A(op) else if (IS_FLOATING(argv[1])) return krk_runtimeError(vm.exceptions->valueError, "no float support");
#define FLOAT_B(op) else if (IS_FLOATING(argv[1])) return krk_runtimeError(vm.exceptions->valueError, "no float support");
#endif
#define BASIC_BIN_OP_FLOAT(a,b,op) BASIC_BIN_OP_FLOATS(a,b,FLOAT_A(op),FLOAT_B(op))

BASIC_BIN_OP_FLOAT(add,krk_long_add,+)
BASIC_BIN_OP_FLOAT(sub,krk_long_sub,-)
BASIC_BIN_OP_FLOAT(mul,krk_long_mul,*)
BASIC_BIN_OP(or, krk_long_or)
BASIC_BIN_OP(xor,krk_long_xor)
BASIC_BIN_OP(and,krk_long_and)

static void _krk_long_lshift_z(krk_long out, krk_long val, size_t amount) {
	if (amount == 0) {
		krk_long_clear(out);
		krk_long_init_copy(out,val);
		return;
	}

	int64_t count = _bits_in(val);
	krk_long_clear(out);
	if (count == 0) return;

	for (size_t i = count; i > 0; i--) {
		if (_bit_is_set(val,i - 1)) krk_long_bit_set(out,i - 1 + amount);
	}

	if (krk_long_sign(val) < 0) krk_long_set_sign(out,-1);
}

static void _krk_long_lshift(krk_long out, krk_long val, krk_long shift) {
	if (krk_long_sign(shift) < 0) { krk_runtimeError(vm.exceptions->valueError, "negative shift count"); return; }
	int64_t amount = krk_long_medium(shift);
	_krk_long_lshift_z(out,val,amount);
}

static void _krk_long_rshift_z(krk_long out, krk_long val, size_t amount) {
	if (amount == 0) {
		krk_long_clear(out);
		krk_long_init_copy(out,val);
		return;
	}

	int64_t count = _bits_in(val);
	krk_long_clear(out);
	if (count == 0) return;

	for (size_t i = count - 1; i >= amount; i--) {
		if (_bit_is_set(val,i)) krk_long_bit_set(out,i - amount);
	}

	if (krk_long_sign(val) < 0) {
		KrkLong one;
		krk_long_init_si(&one, 1);
		krk_long_add(out,out,&one);
		krk_long_set_sign(out,-1);
		krk_long_clear(&one);
	}
}

static void _krk_long_rshift(krk_long out, krk_long val, krk_long shift) {
	if (krk_long_sign(shift) < 0) { krk_runtimeError(vm.exceptions->valueError, "negative shift count"); return; }
	int64_t amount = krk_long_medium(shift);
	_krk_long_rshift_z(out, val, amount);
}

static void _krk_long_mod(krk_long out, krk_long a, krk_long b) {
	if (krk_long_sign(b) == 0) { krk_runtimeError(vm.exceptions->valueError, "integer division or modulo by zero"); return; }
	krk_long garbage;
	krk_long_init_si(garbage,0);
	krk_long_div_rem(garbage,out,a,b);
	krk_long_clear(garbage);
}

static void _krk_long_div(krk_long out, krk_long a, krk_long b) {
	if (krk_long_sign(b) == 0) { krk_runtimeError(vm.exceptions->valueError, "integer division or modulo by zero"); return; }
	krk_long garbage;
	krk_long_init_si(garbage,0);
	krk_long_div_rem(out,garbage,a,b);
	krk_long_clear(garbage);
}

static void _krk_long_pow(krk_long out, krk_long a, krk_long b) {
	if (krk_long_sign(b) == 0) {
		krk_long_clear(out);
		krk_long_init_si(out, 1);
		return;
	}

	if (krk_long_sign(b) < 0) {
		krk_runtimeError(vm.exceptions->notImplementedError, "TODO: negative exponent");
		return;
	}

	/**
	 * CPython sources link here as a reference:
	 * Handbook of Applied Cryptography
	 * @see https://cacr.uwaterloo.ca/hac/about/chap14.pdf
	 *
	 * Section 14.6 covers exponentiation, and this is "left-to-right binary
	 * exponentiation" as described in 14.79:
	 *    A ← 1
	 *    For i from t down to 0 do the following:
	 *       A ← A * A
	 *       If e_i = 1, then A ← A * g
	 */

	PREP_OUTPUT(out,a,b);

	krk_long_clear(out);
	krk_long_init_si(out, 1);

	krk_long scratch;
	krk_long_init_si(scratch, 0);

	for (ssize_t i = b[0].width-1; i >= 0; --i) {
		uint32_t b_i = b[0].digits[i];

		for (size_t j = (uint32_t)1 << (DIGIT_SHIFT-1); j != 0; j >>= 1) {
			krk_long_mul(scratch, out, out);
			_swap(out, scratch);

			if (b_i & j) {
				krk_long_mul(out, out, a);
			}

			/* This can take a long time, especially for values that are likely to
			 * run out of memory for storage, so best to bail on signal early. */
			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) {
				krk_long_clear_many(scratch, out, NULL);
				/* There's no need to set exception here, the VM will do it on its
				 * own eventually... */
				return;
			}
		}
	}

	krk_long_clear(scratch);
	FINISH_OUTPUT(out);
}

BASIC_BIN_OP(lshift,_krk_long_lshift)
BASIC_BIN_OP(rshift,_krk_long_rshift)
BASIC_BIN_OP(mod,_krk_long_mod)
BASIC_BIN_OP(floordiv,_krk_long_div)
BASIC_BIN_OP(pow,_krk_long_pow)

#ifndef KRK_NO_FLOAT
#define KRK_FLOAT_COMPARE(comp) else if (IS_FLOATING(argv[1])) return BOOLEAN_VAL(krk_long_get_double(self->value) comp AS_FLOATING(argv[1]));
#else
#define KRK_FLOAT_COMPARE(comp)
#endif

#define COMPARE_OP(name, comp) \
	KRK_Method(long,__ ## name ## __) { \
		krk_long tmp; \
		if (IS_long(argv[1])) krk_long_init_copy(tmp, AS_long(argv[1])->value); \
		else if (IS_INTEGER(argv[1])) krk_long_init_si(tmp, AS_INTEGER(argv[1])); \
		KRK_FLOAT_COMPARE(comp) \
		else return NOTIMPL_VAL(); \
		int cmp = krk_long_compare(self->value,tmp); \
		krk_long_clear(tmp); \
		return BOOLEAN_VAL(cmp comp 0); \
	}

COMPARE_OP(lt, <)
COMPARE_OP(gt, >)
COMPARE_OP(le, <=)
COMPARE_OP(ge, >=)
COMPARE_OP(eq, ==)

#undef BASIC_BIN_OP
#undef COMPARE_OP

KRK_Method(long,__len__) {
	return INTEGER_VAL(krk_long_sign(self->value));
}

KRK_Method(long,__invert__) {
	KrkLong tmp, one;
	krk_long_init_copy(&tmp, self->value);
	krk_long_init_si(&one, 1);
	krk_long_add(&tmp, &tmp, &one);
	krk_long_set_sign(&tmp, tmp.width > 0 ? -1 : 1);
	krk_long_clear(&one);
	return make_long_obj(&tmp);
}

KRK_Method(long,__neg__) {
	KrkLong tmp;
	krk_long_init_copy(&tmp, self->value);
	krk_long_set_sign(&tmp, tmp.width > 0 ? -1 : 1);
	return make_long_obj(&tmp);
}

KRK_Method(long,__abs__) {
	KrkLong tmp;
	krk_long_init_copy(&tmp, self->value);
	krk_long_set_sign(&tmp, 1);
	return make_long_obj(&tmp);
}

KRK_Method(long,__pos__) {
	return argv[0];
}

typedef int (*fmtCallback)(void *, int, int *);
KrkValue krk_doFormatString(const char * typeName, KrkString * format_spec, int positive, void * abs, fmtCallback callback, fmtCallback (*prepCallback)(void*,int));

struct _private {
	KrkLong * val;
	char * asStr;
	char * next;
	size_t len;
};

static int formatLongCallback(void * a, int base, int *more) {
	struct _private * val = a;

	if (val->next) {
		char c = *val->next;
		int out = 0;
		if (c >= '0' && c <= '9') out = c - '0';
		else if (c >= 'a' && c <= 'f') out = c - 'a' + 10;
		if (val->next == val->asStr || *(--val->next) == '-') {
			val->next = NULL;
			*more = 0;
		} else {
			*more = 1;
		}
		return out;
	}

	*more = 0;
	return 0;
}

static char * krk_long_to_decimal_str(const KrkLong * value, size_t * len);
static fmtCallback prepLongCallback(void * a, int base) {
	struct _private * val = a;
	if (base != 10 || (val->val->width > -10 && val->val->width < 10)) {
		uint32_t hash = 0;
		val->asStr = krk_long_to_str(val->val, base, "", &val->len, &hash);
	} else {
		val->asStr = krk_long_to_decimal_str(val->val, &val->len);
	}
	val->next = &val->asStr[val->len-1];
	return formatLongCallback;
}

KRK_Method(long,__format__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,format_spec);

	struct _private tmp = { self->value, NULL, NULL, 0 };

	KrkValue result = krk_doFormatString("long",format_spec,
		krk_long_sign(self->value) >= 0,
		&tmp,
		NULL,
		prepLongCallback);

	if (tmp.asStr) {
		free(tmp.asStr);
	}

	return result;
}

static KrkValue long_bit_count(KrkLong * val) {
	size_t count = 0;
	size_t bits = _bits_in(val);

	for (size_t i = 0; i < bits; ++i) {
		count += _bit_is_set(val, i);
	}

	KrkLong tmp;
	krk_long_init_ui(&tmp, count);
	return make_long_obj(&tmp);
}

KRK_Method(long,bit_count) {
	return long_bit_count(self->value);
}

static KrkValue long_bit_length(KrkLong * val) {
	size_t bits = _bits_in(val);
	KrkLong tmp;
	krk_long_init_ui(&tmp, bits);
	return make_long_obj(&tmp);
}

KRK_Method(long,bit_length) {
	return long_bit_length(self->value);
}

static KrkValue long_to_bytes(KrkLong * val, size_t argc, const KrkValue argv[], int hasKw) {
	static const char _method_name[] = "to_bytes";
	/**
	 * @fn to_bytes(length: int, byteorder: str, *, signed: bool = False) -> bytes
	 *
	 * @param length    size of the bytes object to produce; restricted to an int; anything bigger
	 *                  is probably going to cause trouble for repring a result anyway, so, whatever...
	 * @param byteorder must be either 'little' or 'big'.
	 * @param signed    needs to be a keyword arg because apparently that's how it is in Python...
	 *                  If a negative value is passed without @c signed=True an error will be raised.
	 */

	CHECK_ARG(1,int,krk_integer_type,length);
	CHECK_ARG(2,str,KrkString*,byteorder);
	int _signed = 0;
	if (hasKw) {
		KrkValue tmp;
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("signed")), &tmp)) {
			_signed = !krk_isFalsey(tmp);
		}
	}

	if (length < 0) {
		return krk_runtimeError(vm.exceptions->valueError, "length must be non-negative");
	}

	int order = 0;
	if (!strcmp(byteorder->chars,"little")) {
		order = 1;
	} else if (!strcmp(byteorder->chars,"big")) {
		order = -1;
	} else {
		return krk_runtimeError(vm.exceptions->valueError, "byteorder must be either 'little' or 'big'");
	}

	if (krk_long_sign(val) < 0 && !_signed) {
		return krk_runtimeError(vm.exceptions->notImplementedError, "can not convert negative value to unsigned");
	}

	/* We could avoid the copy for a positive value, but whatever... */
	KrkLong tmp;
	krk_long_init_ui(&tmp, 0);
	krk_long_abs(&tmp, val);

	/* Invert negative values; already checked for signed... */
	if (krk_long_sign(val) < 0) {
		KrkLong one;
		krk_long_init_ui(&one, 1);
		krk_long_sub(&tmp, &tmp, &one);
		krk_long_clear(&one);
	}

	/* Use bits from inverted value */
	size_t bitCount = _bits_in(&tmp);

	/* If it is signed, we need to reserve the top bit for the sign;
	 * eg., (127).to_bytes(1,...,signed=True) is fine, but (128) is not,
	 * and also (-128) should work, which is taken care of by using the
	 * inverted value... Also, as a weird special case, 0 still has no
	 * bits even if 'signed', which allows (0).to_bytes(0,...,signed=True)
	 * even though that makes no sense to me... */
	if (_signed && val->width != 0) bitCount++;

	if ((size_t)length * 8 < bitCount) {
		krk_long_clear(&tmp);
		/* Should be OverflowError, but we don't have that and I don't care to add it right now */
		return krk_runtimeError(vm.exceptions->valueError, "int too big to convert");
	}

	/* Allocate bytes for final representation */
	krk_push(OBJECT_VAL(krk_newBytes(length, NULL)));
	memset(AS_BYTES(krk_peek(0))->bytes, 0, length);

	/* We'll use a 'bit reader':
	 * - We want 8 bits for each byte.
	 * - We can collect 31 bits from each digit.
	 * - If we run out of digits, we're done.
	 */
	ssize_t i = 0;
	ssize_t j = 0;

	uint64_t accum = 0;
	int32_t remaining = 0;
	int break_here = 0;

	while (i < length && !break_here) {
		if (remaining < 8) {
			if (j < tmp.width) {
				accum |= ((uint64_t)tmp.digits[j]) << remaining;
				j++;
			} else {
				break_here = 1;
			}
			remaining += 31;
		}

		uint8_t byte = accum & 0xFF;
		accum >>= 8;
		remaining -= 8;

		AS_BYTES(krk_peek(0))->bytes[order == 1 ? i : (length - i - 1)] = byte;
		i++;
	}

	/* If input was negative, at this point we're producing an inverted value;
	 * we already encoded (|n|-1), so now we just need to bit invert. */
	if (krk_long_sign(val) < 0) {
		for (size_t i = 0; i < (size_t)length; ++i) {
			AS_BYTES(krk_peek(0))->bytes[i] ^= 0xFF;
		}
	}

	/* We produced a bytes object, so we no longer need the long. */
	krk_long_clear(&tmp);
	return krk_pop();
}

KRK_Method(long,to_bytes) {
	METHOD_TAKES_AT_LEAST(2);
	return long_to_bytes(self->value, argc, argv, hasKw);
}

/**
 * @fn long._digit_count()
 *
 * Internal. Obtain the @c width of a long.
 *
 * @return The number of digits in the internal representation of the long.
 *         The result will be negative if the long is negative.
 */
KRK_Method(long,_digit_count) {
	krk_long result; /* since it's a ssize_t */
	krk_long_init_si(result, self->value[0].width);
	return make_long_obj(result);
}

/**
 * @fn long._get_digit(index)
 *
 * Internal. Obtain an int value representative of a digit of a long.
 *
 * Basically (|n| >> (31 * index)) & 0x7FFFFFFF
 *
 * @param index Digit to get. May be an @c int or a @c long >= 0 and <= 2.
 * @return An int representation of the unsigned digit @p index of the long.
 */
KRK_Method(long,_get_digit) {
	METHOD_TAKES_EXACTLY(1);

	KrkLong * _self = self->value;

	size_t abs_width = _self->width < 0 ? -_self->width : _self->width;
	size_t index;

	if (IS_INTEGER(argv[1])) {
		index = AS_INTEGER(argv[1]);
	} else if (IS_long(argv[1])) {
		KrkLong * value = AS_long(argv[1])->value;
		if (value->width < 0 || value->width > 2) {
			return krk_runtimeError(vm.exceptions->indexError, "digit index is invalid");
		}
		index = krk_long_medium(value);
	} else {
		return TYPE_ERROR(int,argv[1]);
	}

	if (index >= abs_width) {
		return krk_runtimeError(vm.exceptions->indexError, "digit index out of range");
	}

	return INTEGER_VAL(_self->digits[index]);
}

/**
 * Huge decimals for fast conversion.
 *
 * This is a lightweight implementation of decimal-based bigints that only supports
 * addition, inplace subtraction, and multiplication (via Karatsuba). With this, we
 * can much more quickly produce decimal conversions of (binary) longs.
 */
typedef uint32_t digit_t;
#define DEC_DIGIT_SIZE sizeof(digit_t)
#define DEC_DIGIT_CNT  9
#define DEC_DIGIT_MAX 1000000000

/**
 * Adds @c a and @c b to create a new results.
 */
static digit_t * dec_add(const digit_t * a, size_t awidth, const digit_t * b, size_t bwidth, size_t * outwidth) {
	*outwidth = (awidth > bwidth ? awidth : bwidth) + 1;
	digit_t * out = calloc(*outwidth, DEC_DIGIT_SIZE);
	int64_t carry = 0;
	for (size_t i  = 0; i < *outwidth - 1; ++i) {
		digit_t n = ((i < awidth) ? a[i] : 0) + ((i < bwidth) ? b[i] : 0) + carry;
		out[i] = n % DEC_DIGIT_MAX;
		carry = (n >= DEC_DIGIT_MAX);
	}
	if (carry) {
		out[*outwidth-1] = 1;
	} else {
		*outwidth -= 1;
	}

	if (*outwidth == 0) {
		*outwidth = 1;
		out[0] = 0;
	}

	return out;
}

/**
 * Subtracts a smaller @c b from a larger @c a in-place.
 */
static void dec_isub(digit_t * a, size_t awidth, const digit_t * b, size_t bwidth) {
	int64_t carry = 0;
	for (size_t i = 0; i < awidth; ++i) {
		int64_t a_digit = (int64_t)((i < awidth) ? a[i] : 0) - carry;
		int64_t b_digit = (int64_t)((i < bwidth) ? b[i] : 0);
		if (a_digit < b_digit) {
			a_digit += DEC_DIGIT_MAX;
			carry = 1;
		} else {
			carry = 0;
		}
		a[i] = (a_digit - b_digit) % DEC_DIGIT_MAX;
	}
}

/**
 * Decimal left shift. Multiply a by 1000000000^amount and return a new result value.
 */
static digit_t * dec_shift(const digit_t * a, size_t awidth, size_t amount, size_t * outwidth) {
	if (awidth == 1 && a[0] == 0) {
		*outwidth = 1;
		return calloc(1,DEC_DIGIT_SIZE);
	}
	*outwidth = awidth + amount;
	digit_t * out = calloc(*outwidth,DEC_DIGIT_SIZE);

	for (size_t i = 0; i < awidth; ++i) {
		out[i+amount] = a[i];
	}

	return out;
}

/**
 * Multiply a by b and return a new result value.
 *
 * Uses the Karatsuba algorithm for larger values; degrades to brute-force
 * chalkboard multiplication for smaller values.
 */
static digit_t * dec_mul(const digit_t * a, size_t a_width, const digit_t * b, size_t b_width, size_t * outwidth) {
	/* We want a to be bigger than b */
	if (a_width < b_width) {
		const digit_t * t = a;
		a = b;
		b = t;
		size_t tmp = a_width;
		a_width = b_width;
		b_width = tmp;
	}

	*outwidth = a_width + b_width;

	/* Degenerate case where a or b is 0: return 0 */
	if ((a_width == 1 && a[0] == 0) || (b_width == 1 && b[0] == 0)) {
		*outwidth = 1;
		return calloc(1,DEC_DIGIT_SIZE);
	}

	/* Degenerate case where a is 1, return b */
	if (a_width == 1 && a[0] == 1) {
		*outwidth = b_width;
		digit_t * out = malloc(*outwidth * DEC_DIGIT_SIZE);
		memcpy(out, b, *outwidth * DEC_DIGIT_SIZE);
		return out;
	}

	/* Degenerate case where b is 1, return a */
	if (b_width == 1 && b[0] == 1) {
		*outwidth = a_width;
		digit_t * out = malloc(*outwidth * DEC_DIGIT_SIZE);
		memcpy(out, a, *outwidth * DEC_DIGIT_SIZE);
		return out;
	}

	if (b_width < 50) {
		/* Fallback brute-force multiplication */
		digit_t * out = calloc(*outwidth,DEC_DIGIT_SIZE);
		for (size_t i = 0; i < b_width; ++i) {
			digit_t bdigit = (i < b_width) ? b[i] : 0;
			int64_t carry = 0;
			for (size_t j = 0; j < a_width; ++j) {
				digit_t adigit = (j < a_width) ? a[j] : 0;
				uint64_t t = carry + (int64_t)adigit * (int64_t)bdigit + out[i+j];
				carry = t / DEC_DIGIT_MAX;
				out[i+j] = t % DEC_DIGIT_MAX;
			}
			out[i+a_width] = carry;
		}
		while (*outwidth > 1 && out[(*outwidth)-1] == 0) (*outwidth)--;
		return out;
	} else {
		size_t m2  = a_width / 2;

		/* Split a into its high and low halves */
		const digit_t * low1  = a;
		size_t    low1_width = (m2 <= a_width) ? m2 : a_width;
		while (low1_width > 1 && low1[low1_width-1] == 0) low1_width--;
		digit_t   a_zero = 0;
		const digit_t * high1 = (m2 <= a_width) ? (a + m2) : &a_zero;
		size_t    high1_width = (m2 <= a_width) ? (a_width - m2) : 1;

		/* Split b into its high and low halves */
		const digit_t * low2  = b;
		size_t    low2_width = (m2 <= b_width) ? m2 : b_width;
		while (low2_width > 1 && low2[low2_width-1] == 0) low2_width--;
		digit_t   b_zero = 0;
		const digit_t * high2 = (m2 <= b_width) ? (b + m2) : &b_zero;
		size_t    high2_width = (m2 <= b_width) ? (b_width - m2) : 1;

		size_t z0_width, z1_width, z2_width;

		/* z0 = low1 * low2; z2 = high1 * high2 */
		digit_t * z0 = dec_mul(low1, low1_width, low2, low2_width, &z0_width);
		digit_t * z2 = dec_mul(high1, high1_width, high2, high2_width, &z2_width);

		/* z1 = (low1 + high1) * (low2 + high2) */
		size_t sleft_width, sright_width;
		digit_t * sleft  = dec_add(low1, low1_width, high1, high1_width, &sleft_width);
		digit_t * sright = dec_add(low2, low2_width, high2, high2_width, &sright_width);
		digit_t * z1 = dec_mul(sleft, sleft_width, sright, sright_width, &z1_width);
		free(sleft);
		free(sright);

		/* Store (z1 - z2 - z0) into z1 */
		dec_isub(z1, z1_width, z2, z2_width);
		dec_isub(z1, z1_width, z0, z0_width);

		/* Calculate (z1 - z2 - z0) * 10 ^ m2 */
		size_t m2_shift_width;
		digit_t * m2_shift = dec_shift(z1, z1_width, m2, &m2_shift_width);
		free(z1);

		/* Add z0 to that */
		size_t add_width;
		digit_t * add = dec_add(m2_shift, m2_shift_width, z0, z0_width, &add_width);
		free(m2_shift);
		free(z0);

		/* Then calculate z2 * 10 ^ (m2 * 2) */
		size_t m2_2_width;
		digit_t * m2_2 = dec_shift(z2, z2_width, m2 * 2, &m2_2_width);
		free(z2);

		/* And add everything up */
		size_t result_width;
		digit_t * result = dec_add(m2_2, m2_2_width, add, add_width, &result_width);
		free(m2_2);
		free(add);

		*outwidth = result_width;
		return result;
	}
}

/**
 * @brief Raise 2 to the wth power, as a huge decimal.
 *
 * Creates a decimal representation of 2 raised to the requested power.
 *
 * If @p w is very small (eg. 2**w would fit in one decimal digit), then
 * we can create this value directly. Otherwise, we recursively break
 * down @p w into smaller values and build huge decimals out of them
 * through repeated multiplication.
 *
 * An older prototype of this used a cache, which did save some time,
 * but not a whole lot in the long run, and this implementation is
 * considerably simpler without the cache.
 *
 * @param w Power to raise 2 to.
 * @param sizeOut Resulting size of the huge decimal.
 * @returns A huge decimal representing 2 ** w.
 */
static digit_t * dec_two_raised(size_t w, size_t * sizeOut) {
	if (w <= 29) {
		*sizeOut = 1;
		digit_t * out = malloc(DEC_DIGIT_SIZE);
		out[0] = 1 << w;
		return out;
	} else {
		/* w2 = w >> 1 */
		size_t w2 = w >> 1;

		/* t = Decimal(1 << w2) */
		size_t tSize;
		digit_t * t = dec_two_raised(w2, &tSize);

		if ((w & 1) == 0) {
			/* Result = t * t */
			digit_t * result = dec_mul(t, tSize, t, tSize, sizeOut);
			free(t);
			return result;
		} else {
			/* wmw2 = w - w2 */
			size_t wmw2 = w - w2;

			/* right = 1 << wmw2 */
			size_t rightSize;
			digit_t * right = dec_two_raised(wmw2, &rightSize);

			/* result = t * right */
			digit_t * result = dec_mul(t, tSize, right, rightSize, sizeOut);
			free(t);
			free(right);
			return result;
		}
	}
}

/**
 * @brief Convert a KrkLong to a series of huge decimal digits.
 *
 * Takes a KrkLong @p n of bitwidth @p w and converts it into an array of
 * @c digit_t huge decimal digits, storing the size in @p sizeOut.
 *
 * @param n KrkLong to convert.
 * @param w Bitwidth of @p n.
 * @param sizeOut Resulting size of the huge decimal.
 * @returns Huge decimal of equivalent value.
 */
static digit_t * long_to_dec_inner(KrkLong * n, size_t w, size_t * sizeOut) {
	if (n->width == 0) {
		*sizeOut = 1;
		return calloc(1,DEC_DIGIT_SIZE);
	}
	if (w <= 29) {
		*sizeOut = 1;
		digit_t * out = malloc(DEC_DIGIT_SIZE);
		out[0] = n->digits[0];
		return out;
	}

	size_t aSize, bSize, cSize;
	digit_t * a, * b, * c;
	KrkLong hi, lo, tmp;
	krk_long_init_many(&hi, &lo, &tmp, NULL);
	/* w2 = w >> 1 */
	size_t w2 = w >> 1;
	/* hi = n >> w2 */
	_krk_long_rshift_z(&hi, n, w2);
	/* tmp = hi << w2 */
	_krk_long_lshift_z(&tmp, &hi, w2);
	/* lo = n - (hi << w2) */
	krk_long_sub(&lo, n, &tmp);
	krk_long_clear_many(&tmp, NULL);
	/* a = Dec(hi) */
	a = long_to_dec_inner(&hi, w - w2, &aSize);
	krk_long_clear_many(&hi, NULL);
	/* b = Dec(1 << w2) */
	b = dec_two_raised(w2, &bSize);
	/* c = a * b */
	c = dec_mul(a, aSize, b, bSize, &cSize);
	free(a);
	free(b);
	/* a = Dec(lo) */
	a = long_to_dec_inner(&lo, w2, &aSize);
	krk_long_clear_many(&lo,NULL);
	/* result = a + c */
	digit_t * result = dec_add(a, aSize, c, cSize, sizeOut);
	free(a);
	free(c);
	return result;
}

static char * krk_long_to_decimal_str(const KrkLong * value, size_t * len) {
	/* We can only do this on positive values, but we can re-use the digits
	 * of the current number while processing, since longs are generally
	 * not mutable by any other operations. */
	KrkLong abs = *value;
	int inv = (krk_long_sign(&abs) == -1);
	krk_long_set_sign(&abs, 1);

	/* Calculate bit width for halving */
	size_t w = _bits_in(&abs);

	/* Convert to big decimal digits */
	size_t size;
	digit_t * digits = long_to_dec_inner(&abs, w, &size);

	/* Count number of leading zeros */
	int leading = 0;
	for (size_t j = 0, div = DEC_DIGIT_MAX/10; j < DEC_DIGIT_CNT; j++, div/=10) {
		if (((digits[size-1] / div) % 10)) break;
		leading += 1;
	}

	/* Allocate space for output */
	char * out = malloc(size * DEC_DIGIT_CNT + 1 - leading + inv);
	char * writer = out;

	/* Write negative sign if original value was negative. */
	if (inv) *(writer++) = '-';

	/* Collect digits */
	for (size_t i = 0; i < size; ++i) {
		for (size_t j = 0, div = DEC_DIGIT_MAX/10; j < DEC_DIGIT_CNT; j++, div/=10) {
			if (leading) { leading--; continue; }
			*(writer++) = ((digits[size-i-1] / div) % 10) + '0';
		}
	}
	*writer = '\0';

	free(digits);
	*len = writer - out;

	return out;
}

KRK_Method(long,__repr__) {
	/* For rather small values (10 was chosen arbitrarily), use the older approach */
	size_t len;

	if (self->value->width > -10 && self->value->width < 10) {
		uint32_t hash;
		char * rev = krk_long_to_str(self->value, 10, "", &len, &hash);
		return OBJECT_VAL(krk_takeStringVetted(rev,len,len,KRK_OBJ_FLAGS_STRING_ASCII,hash));
	}

	char * out = krk_long_to_decimal_str(self->value, &len);
	return OBJECT_VAL(krk_takeString(out, len));
}

#ifndef KRK_NO_FLOAT
KrkValue krk_int_from_float(double val) {
	union { double asDbl; uint64_t asInt; } u = {val};

	int sign     = (u.asInt >> 63);
	int exponent = ((u.asInt >> 52) & 0x7FF) - 0x3FF;
	uint64_t man = (u.asInt & 0xfffffffffffffUL);

	if (exponent < 0) return INTEGER_VAL(0);
	if (exponent == 1024) return krk_runtimeError(vm.exceptions->valueError, "can not convert float %s to int", man ? "Nan" : "infinity");
	if (exponent < 47) return INTEGER_VAL(val);

	KrkLong _value;
	krk_long_init_si(&_value, 0x10000000000000 | man);

	KrkLong exp = {0};
	if (exponent > 52) {
		krk_long_init_si(&exp, exponent - 52);
		_krk_long_lshift(&_value, &_value, &exp);
	} else if (exponent < 52) {
		krk_long_init_si(&exp, -(exponent - 52));
		_krk_long_rshift(&_value, &_value, &exp);
	}
	krk_long_clear(&exp);

	krk_long_set_sign(&_value, sign == 1 ? -1 : 1);
	return make_long_obj(&_value);
}
#endif

/**
 * @brief Convert an int or long to a C integer.
 *
 * No overflow checking is performed in any case.
 *
 * @param val  int or long to convert.
 * @param size Size in bytes of desired C integer type.
 * @param out  Pointer to resulting int.
 */
_protected
int krk_long_to_int(KrkValue val, char size, void * out) {
	uint64_t accum = 0;
	if (IS_INTEGER(val)) {
		/* For integers, there's nothing to do until we want to start
		 * doing overflow checking, so just extend to 64-bit. */
		accum = AS_INTEGER(val);
	} else if (IS_long(val)) {
		/* For longs, we have some additional work. */
		struct BigInt * self = (void*)AS_OBJECT(val);
		KrkLong * this = self->value;
		size_t swidth = this->width < 0 ? -this->width : this->width;

		if (swidth > 0) {
			/* Collect up to three digits worth of bits, to the maximum
			 * we support of 64. 31, 31, and 2. */
			accum |= (uint64_t)this->digits[0];
			if (swidth > 1) {
				accum |= (uint64_t)this->digits[1] << DIGIT_SHIFT;
				if (swidth > 2) {
					accum |= (uint64_t)(this->digits[2] & 0x3) << DIGIT_SHIFT * 2;
				}
			}
			/* If this is a negative value, convert the result to twos-complement. */
			if (this->width < 0) {
				accum -= 1;
				accum ^= 0xFFFFffffFFFFffff;
			}
		}
#ifndef KRK_NO_FLOAT
	} else if (IS_FLOATING(val)) {
		krk_push(krk_int_from_float(AS_FLOATING(val)));
		int res = krk_long_to_int(krk_peek(0), size, out);
		krk_pop();
		return res;
#endif
	} else {
		krk_runtimeError(vm.exceptions->typeError, "expected %s, not '%T'", "int", val);
		return 0;
	}

	/* Now copy over the output. */
	switch (size) {
		case sizeof(uint8_t):   *(uint8_t*)out  = accum; break;
		case sizeof(uint16_t):  *(uint16_t*)out = accum; break;
		case sizeof(uint32_t):  *(uint32_t*)out = accum; break;
		case sizeof(uint64_t):  *(uint64_t*)out = accum; break;
		default:
			krk_runtimeError(vm.exceptions->SystemError, "invalid size");
			return 0;
	}

	return 1;
}


#undef CURRENT_CTYPE
#define CURRENT_CTYPE krk_integer_type

/**
 * @c int wrapper implementations of the byte conversions.
 *
 * Convert to a @c long and just use those versions...
 */

KRK_Method(int,bit_count) {
	krk_long value;
	krk_long_init_si(value, self);
	KrkValue out = long_bit_count(value);
	krk_long_clear(value);
	return out;
}

KRK_Method(int,bit_length) {
	krk_long value;
	krk_long_init_si(value, self);
	KrkValue out = long_bit_length(value);
	krk_long_clear(value);
	return out;
}

KRK_Method(int,to_bytes) {
	krk_long value;
	krk_long_init_si(value, self);
	KrkValue out = long_to_bytes(value, argc, argv, hasKw);
	krk_long_clear(value);
	return out;
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
void _createAndBind_longClass(void) {
	KrkClass * _long = ADD_BASE_CLASS(vm.baseClasses->longClass, "long", vm.baseClasses->intClass);
	_long->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	_long->allocSize = sizeof(struct BigInt);
	_long->_ongcsweep = _long_gcsweep;

	BIND_STATICMETHOD(long,__new__);
	BIND_METHOD(long,__repr__);
	BIND_METHOD(long,__eq__);
	BIND_METHOD(long,__hash__);
	BIND_METHOD(long,__hex__);
	BIND_METHOD(long,__oct__);
	BIND_METHOD(long,__bin__);
	BIND_METHOD(long,__int__);
	BIND_METHOD(long,__len__);
	BIND_METHOD(long,__pos__);

	BIND_TRIPLET(long,add);
	BIND_TRIPLET(long,sub);
	BIND_TRIPLET(long,mul);
	BIND_TRIPLET(long,or);
	BIND_TRIPLET(long,xor);
	BIND_TRIPLET(long,and);
	BIND_TRIPLET(long,lshift);
	BIND_TRIPLET(long,rshift);
	BIND_TRIPLET(long,mod);
	BIND_TRIPLET(long,floordiv);
	BIND_TRIPLET(long,pow);

#ifndef KRK_NO_FLOAT
	BIND_METHOD(long,__float__);
	BIND_TRIPLET(long,truediv);
#endif

	BIND_METHOD(long,__lt__);
	BIND_METHOD(long,__gt__);
	BIND_METHOD(long,__le__);
	BIND_METHOD(long,__ge__);
	BIND_METHOD(long,__invert__);
	BIND_METHOD(long,__neg__);
	BIND_METHOD(long,__abs__);
	BIND_METHOD(long,__format__);

	BIND_METHOD(long,bit_count);
	BIND_METHOD(long,bit_length);
	BIND_METHOD(long,to_bytes);

	/* Internal methods for inspecting longs. Since these are internal,
	 * we don't bother binding them for ints. */
	BIND_METHOD(long,_digit_count);
	BIND_METHOD(long,_get_digit);

	krk_finalizeClass(_long);

	/* Patch in small int versions */
	KrkClass * _int = vm.baseClasses->intClass;
	BIND_METHOD(int,bit_count);
	BIND_METHOD(int,bit_length);
	BIND_METHOD(int,to_bytes);

}


