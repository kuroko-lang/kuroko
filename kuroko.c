#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"
#include "memory.h"

int main(int argc, char * argv[]) {
	krk_initVM();

	int opt;
	while ((opt = getopt(argc, argv, "tds")) != -1) {
		switch (opt) {
			case 't':
				vm.enableTracing = 1;
				break;
			case 'd':
				vm.enableDebugging = 1;
				break;
			case 's':
				vm.enableScanTracing = 1;
				break;
		}
	}

	KrkValue result = INTEGER_VAL(0);

	if (optind == argc) {
		/* Run the repl */
		int exit = 0;

		while (!exit) {
			size_t lineCapacity = 8;
			size_t lineCount = 0;
			char ** lines = ALLOCATE(char *, lineCapacity);
			size_t totalData = 0;
			int valid = 1;
			char * allData = NULL;
			int inBlock = 0;

			fprintf(stdout, ">>> ");
			fflush(stdout);

			while (1) {
				if (inBlock) {
					fprintf(stdout, "  > ");
					fflush(stdout);
				}

				/* This would be a nice place for line editing */
				char buf[4096];
				if (!fgets(buf, 4096, stdin)) {
					exit = 1;
					break;
				}
				if (buf[strlen(buf)-1] != '\n') {
					fprintf(stderr, "Expected end of line in repl input. Did you ^D early?\n");
					valid = 0;
					break;
				}
				if (lineCapacity < lineCount + 1) {
					size_t old = lineCapacity;
					lineCapacity = GROW_CAPACITY(old);
					lines = GROW_ARRAY(char *,lines,old,lineCapacity);
				}

				int i = lineCount++;
				lines[i] = strdup(buf);

				size_t lineLength = strlen(lines[i]);
				totalData += lineLength;

				if (inBlock && lineLength != 1) {
					continue;
				} else if (lineLength > 2 && lines[i][lineLength-2] == ':') {
					inBlock = 1;
					continue;
				}

				break;
			}

			if (valid) {
				allData = malloc(totalData);
				allData[0] = '\0';
			}
			for (int i = 0; i < lineCount; ++i) {
				if (valid) strcat(allData, lines[i]);
				free(lines[i]);
			}
			FREE_ARRAY(char *, lines, lineCapacity);

			if (valid) {
				KrkValue result = krk_interpret(allData, 0, "<module>","<stdin>");
				if (!IS_NONE(result)) {
					fprintf(stdout, " \033[1;30m=> ");
					krk_printValue(stdout, result);
					fprintf(stdout, "\033[0m\n");
				}
			}

		}
	} else {

		for (int i = optind; i < argc; ++i) {
			KrkValue out = krk_runfile(argv[i],0,"<module>",argv[i]);
			if (i + 1 == argc) result = out;
		}
	}

	krk_freeVM();

	if (IS_INTEGER(result)) return AS_INTEGER(result);

	return 0;
}
