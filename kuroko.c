#include <stdio.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, char * argv[]) {
	krk_initVM();

	KrkValue result = krk_runfile("test.krk",0);

	krk_freeVM();

	if (IS_INTEGER(result)) return AS_INTEGER(result);

	return 0;
}
