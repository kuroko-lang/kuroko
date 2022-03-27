/**
 * @file    module_random.c
 * @brief   Functions for generating pseudo-random numbers.
 * @author  K. Lange <klange@toaruos.org>
 *
 */
#include <stdlib.h>
#include <time.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

KRK_FUNC(random,{
	FUNCTION_TAKES_NONE();

	double r = (double)rand() / (double)(RAND_MAX);

	return FLOATING_VAL(r);
})

KRK_FUNC(seed,{
	FUNCTION_TAKES_AT_MOST(1);
	int seed;

	if (argc > 0) {
		CHECK_ARG(0,int,krk_integer_type,_seed);
		seed = _seed;
	} else {
		seed = time(NULL);
	}

	srand(seed);
})

KrkValue krk_module_onload_random(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_push(OBJECT_VAL(module));

	KRK_DOC(module, "Functions for generating pseudo-random numbers.");

	BIND_FUNC(module, random);
	BIND_FUNC(module, seed);

	return krk_pop();
}
