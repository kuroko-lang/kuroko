#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/debug.h>
#include <kuroko/util.h>

#include "common.h"

#define DEFAULT_LIMIT 500000

static int usage(char * argv[]) {
	fprintf(stderr, "usage: %s [-s COUNT] [-q] FILE [args...]\n", argv[0]);
	return 1;
}

static int help(char * argv[]) {
	usage(argv);
	fprintf(stderr,
		"Run scripts with an instruction counter and halt when a "
		"limit is exceeded. The default limit is %d. A total count of "
		"executed instructions is printed after completion.\n"
		"\n"
		"Options:\n"
		" -s COUNT    Set watchdog timeout to COUNT instructions.\n"
		"             Specify -1 to set disable limit.\n"
		" -q          Do not print total instruction count.\n"
		"\n"
		" --help      Show this help text.\n"
		"\n",
		DEFAULT_LIMIT);
	return 0;
}


static size_t instrCounter = 0;     /* Total counter of executed instructions */
static size_t stopAt = DEFAULT_LIMIT;
static int    quiet = 0;

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
	while ((opt = getopt(argc, argv, "+:s:q-:")) != -1) {
		switch (opt) {
			case 's':
				stopAt = strtoul(optarg,NULL,10);
				break;
			case 'q':
				quiet = 1;
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
					return help(argv);
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

	if (!quiet) {
		fprintf(stderr, "%zu total instructions\n", instrCounter);
	}

	krk_freeVM();
	return 0;
}


