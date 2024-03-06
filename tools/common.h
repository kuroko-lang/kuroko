#include <stdlib.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>

/* Copied from src/kuroko.c */
static void findInterpreter(char * argv[]) {
#ifdef _WIN32
	vm.binpath = strdup(_pgmptr);
#else
	/* Try asking /proc */
	char tmp[4096];
	char * binpath = realpath("/proc/self/exe", NULL);
	if (!binpath || (access(binpath, X_OK) != 0)) {
		if (binpath) {
			free(binpath);
			binpath = NULL;
		}
		if (strchr(argv[0], '/')) {
			binpath = realpath(argv[0], NULL);
		} else {
			/* Search PATH for argv[0] */
			char * p = getenv("PATH");
			if (!p) return;
			char * _path = strdup(p);
			char * path = _path;
			while (path) {
				char * next = strchr(path,':');
				if (next) *next++ = '\0';

				snprintf(tmp, 4096, "%s/%s", path, argv[0]);
				if (access(tmp, X_OK) == 0) {
					binpath = strdup(tmp);
					break;
				}
				path = next;
			}
			free(_path);
		}
	}
	if (binpath) {
		vm.binpath = binpath;
	} /* Else, give up at this point and just don't attach it at all. */
#endif
}

/* Collect arguments for script, also copied from src/kuroko.c */
static void addArgs(int argc, char * argv[]) {
	for (int arg = optind; arg < argc; ++arg) {
		krk_push(OBJECT_VAL(krk_copyString(argv[arg],strlen(argv[arg]))));
	}
	KrkValue argList = krk_callNativeOnStack(argc - optind + (optind == argc), &krk_currentThread.stackTop[-(argc - optind + (optind == argc))], 0, krk_list_of);
	krk_push(argList);
	krk_attachNamedValue(&vm.system->fields, "argv", argList);
	krk_pop();
	for (int arg = optind; arg < argc + (optind == argc); ++arg) krk_pop();
}

