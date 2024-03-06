#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/debug.h>
#include <kuroko/util.h>

#include "common.h"

static int usage(char * argv[]) {
	fprintf(stderr, "usage: %s [-s count] [FILE] [args...]\n", argv[0]);
	return 1;
}

static size_t instrCounter = 0;     /* Total counter of executed instructions */
static size_t stopAt = 500000;

int krk_callgrind_debuggerHook(KrkCallFrame * frame) {
	instrCounter++;
	if (instrCounter < stopAt) return KRK_DEBUGGER_STEP;
	if (instrCounter == stopAt) {
		krk_runtimeError(vm.exceptions->baseException, "Watchdog counter expired.");
	}
	return KRK_DEBUGGER_CONTINUE;
}


int main(int argc, char *argv[]) {
	int opt;
	while ((opt = getopt(argc, argv, "+:s:-:")) != -1) {
		switch (opt) {
			case 's':
				stopAt = strtoul(optarg,NULL,10);
				break;
			case '?':
				if (optopt != '-') {
					fprintf(stderr, "%s: unrocognized option '%c'\n", argv[0], optopt);
					return 1;
				}
				optarg = argv[optind]+1;
				/* fall through */
			case '-':
				if (!strcmp(optarg,"help")) {
				} else {
					fprintf(stderr, "%s: unrecognized option: '--%s'\n", argv[0], optarg);
					return 1;
				}
		}
	}

	if (optind == argc) {
		return usage(argv);
	}

	findInterpreter(argv);
	krk_initVM(KRK_THREAD_SINGLE_STEP);
	krk_debug_registerCallback(krk_callgrind_debuggerHook);
	addArgs(argc,argv);

	krk_startModule("__main__");
	krk_runfile(argv[optind],argv[optind]);

	fprintf(stderr, "%zu total instructions\n", instrCounter);

	krk_freeVM();
	return 0;
}


