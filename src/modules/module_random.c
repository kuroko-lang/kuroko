/**
 * @file    module_random.c
 * @brief   Functions for generating pseudo-random numbers.
 * @author  K. Lange <klange@toaruos.org>
 *
 */
#include <stdlib.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

KRK_FUNC(random,{
	FUNCTION_TAKES_NONE();

	double r = (double)rand() / (double)(RAND_MAX);

	return FLOATING_VAL(r);
})

KrkValue krk_module_onload_random(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_push(OBJECT_VAL(module));

	KRK_DOC(module, "Functions for generating pseudo-random numbers.");

	BIND_FUNC(module, random);

	return krk_pop();
}
