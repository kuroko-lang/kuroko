/**
 * @file    module_random.c
 * @brief   Functions for generating pseudo-random numbers.
 * @author  K. Lange <klange@toaruos.org>
 *
 */
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

static uint32_t x = 123456789;
static uint32_t y = 362436069;
static uint32_t z = 521288629;
static uint32_t w = 88675123;

static int _rand(void) {
	uint32_t t;

	t = x ^ (x << 11);
	x = y; y = z; z = w;
	w = w ^ (w >> 19) ^ t ^ (t >> 8);

	return (w & RAND_MAX);
}

void _srand(unsigned int seed) {
	x = 123456789  ^ (seed << 16) ^ (seed >> 16);
	y = 362436069;
	z = 521288629;
	w = 88675123;
}

KRK_Function(random) {
	FUNCTION_TAKES_NONE();

	double r = (double)_rand() / (double)(RAND_MAX);

	return FLOATING_VAL(r);
}

KRK_Function(seed) {
	FUNCTION_TAKES_AT_MOST(1);
	int seed;

	if (argc > 0) {
		CHECK_ARG(0,int,krk_integer_type,_seed);
		seed = _seed;
	} else {
		struct timeval tv;
		gettimeofday(&tv,NULL);

		seed = tv.tv_sec ^ tv.tv_usec;
	}

	_srand(seed);
	return NONE_VAL();
}

KRK_Module(random) {
	KRK_DOC(module, "Functions for generating pseudo-random numbers.");

	BIND_FUNC(module, random);
	BIND_FUNC(module, seed);

	FUNC_NAME(krk,seed)(0,NULL,0);
}
