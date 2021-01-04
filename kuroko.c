/**
 * Kuroko interpreter main executable.
 *
 * Reads lines from stdin with the `rline` library and executes them,
 * or executes scripts from the argument list.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifdef __toaru__
#include <toaru/rline.h>
#include <kuroko.h>
#else
#include "rline.h"
#include "kuroko.h"
#endif

#include "chunk.h"
#include "debug.h"
#include "vm.h"
#include "memory.h"


int main(int argc, char * argv[]) {
	int flags = 0;
	int opt;
	while ((opt = getopt(argc, argv, "tdgs")) != -1) {
		switch (opt) {
			case 't':
				/* Disassemble instructions as they are executed. */
				flags |= KRK_ENABLE_TRACING;
				break;
			case 'd':
				/* Disassemble code blocks after compilation. */
				flags |= KRK_ENABLE_DISASSEMBLY;
				break;
			case 's':
				/* Print debug information during compilation. */
				flags |= KRK_ENABLE_SCAN_TRACING;
				break;
			case 'g':
				/* Always garbage collect during an allocation. */
				flags |= KRK_ENABLE_STRESS_GC;
				break;
		}
	}

	krk_initVM(flags);

	KrkValue result = INTEGER_VAL(0);

	if (optind == argc) {
		/* Run the repl */
		int exit = 0;

		/* Set ^D to send EOF */
		rline_exit_string="";
		/* Enable syntax highlight for Kuroko */
		rline_exp_set_syntax("krk");
		/* TODO: Add tab completion for globals, known fields/methods... */
		//rline_exp_set_tab_complete_func(tab_complete_func);

		while (!exit) {
			size_t lineCapacity = 8;
			size_t lineCount = 0;
			char ** lines = ALLOCATE(char *, lineCapacity);
			size_t totalData = 0;
			int valid = 1;
			char * allData = NULL;
			int inBlock = 0;
			int blockWidth = 0;

			/* Main prompt is >>> like in Python */
			rline_exp_set_prompts(">>> ", "", 4, 0);

			while (1) {
				/* This would be a nice place for line editing */
				char buf[4096] = {0};

				if (inBlock) {
					/* When entering multiple lines, the additional lines
					 * will show a single > (and keep the left side aligned) */
					rline_exp_set_prompts("  > ", "", 4, 0);
					/* Also add indentation as necessary */
					rline_preload = malloc(blockWidth + 1);
					for (int i = 0; i < blockWidth; ++i) {
						rline_preload[i] = ' ';
					}
					rline_preload[blockWidth] = '\0';
				}

				rline_scroll = 0;
				if (rline(buf, 4096) == 0) {
					valid = 0;
					exit = 1;
					break;
				}
				if (buf[strlen(buf)-1] != '\n') {
					/* rline shouldn't allow this as it doesn't accept ^D to submit input
					 * unless the line is empty, but just in case... */
					fprintf(stderr, "Expected end of line in repl input. Did you ^D early?\n");
					valid = 0;
					break;
				}

				if (lineCapacity < lineCount + 1) {
					/* If we need more space, grow as needed... */
					size_t old = lineCapacity;
					lineCapacity = GROW_CAPACITY(old);
					lines = GROW_ARRAY(char *,lines,old,lineCapacity);
				}

				int i = lineCount++;
				lines[i] = strdup(buf);

				size_t lineLength = strlen(lines[i]);
				totalData += lineLength;

				/* Figure out indentation */
				int isSpaces = 1;
				int countSpaces = 0;
				for (size_t j = 0; j < lineLength; ++j) {
					if (lines[i][j] != ' ' && lines[i][j] != '\n') {
						isSpaces = 0;
						break;
					}
					countSpaces += 1;
				}

				/* Naively detect the start of a new block so we can
				 * continue to accept input. Our compiler isn't really
				 * set up to let us compile "on the fly" so we can't just
				 * run lines through it and see if it wants more... */
				if (lineLength > 2 && lines[i][lineLength-2] == ':') {
					inBlock = 1;
					blockWidth = countSpaces + 4;
					continue;
				} else if (inBlock && lineLength != 1) {
					if (isSpaces) {
						free(lines[i]);
						totalData -= lineLength;
						lineCount--;
						break;
					}
					blockWidth = countSpaces;
					continue;
				} else if (lineLength > 1 && lines[i][countSpaces] == '@') {
					inBlock = 1;
					blockWidth = countSpaces;
					continue;
				}

				/* Ignore blank lines. */
				if (isSpaces) valid = 0;

				/* If we're not in a block, or have entered a blank line,
				 * we can stop reading new lines and jump to execution. */
				break;
			}

			if (valid) {
				allData = malloc(totalData + 1);
				allData[0] = '\0';
			}

			for (size_t i = 0; i < lineCount; ++i) {
				if (valid) strcat(allData, lines[i]);
				rline_history_insert(strdup(lines[i]));
				free(lines[i]);
			}
			FREE_ARRAY(char *, lines, lineCapacity);

			if (valid) {
				KrkValue result = krk_interpret(allData, 0, "<module>","<stdin>");
				if (!IS_NONE(result)) {
					fprintf(stdout, " \033[1;30m=> ");
					krk_printValue(stdout, result);
					fprintf(stdout, "\033[0m\n");
					krk_resetStack();
				}
			}

		}
	} else {
		/* Expect the rest of the arguments to be scripts to run;
		 * collect the result of the last one and use it as the
		 * exit code if it's an integer. */
		for (int i = optind; i < argc; ++i) {
			KrkValue out = krk_runfile(argv[i],0,"<module>",argv[i]);
			if (i + 1 == argc) result = out;
		}
	}

	krk_freeVM();

	if (IS_INTEGER(result)) return AS_INTEGER(result);

	return 0;
}
