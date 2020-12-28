#include <stdio.h>
#include <unistd.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, char * argv[]) {
	krk_initVM();

	int opt;
	while ((opt = getopt(argc, argv, "td")) != -1) {
		switch (opt) {
			case 't':
				vm.enableTracing = 1;
				break;
			case 'd':
				vm.enableDebugging = 1;
				break;
		}
	}

	if (optind == argc) {
		fprintf(stderr, "no repl available; try running a script\n");
		return 1;
	}

	KrkValue result;

	for (int i = optind; i < argc; ++i) {
		KrkValue out = krk_runfile(argv[i],0);
		if (i + 1 == argc) result = out;
	}

	krk_freeVM();

	if (IS_INTEGER(result)) return AS_INTEGER(result);

	return 0;
}
