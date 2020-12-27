#include <stdio.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, char * argv[]) {
	if (argc < 2) return 1;

	krk_initVM();

	KrkValue result = krk_runfile(argv[1],0);

	krk_freeVM();

	if (IS_INTEGER(result)) return AS_INTEGER(result);

	return 0;
}
