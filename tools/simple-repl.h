#ifdef SIMPLE_REPL_ENABLE_RLINE
#include "vendor/rline.h"
static int enableRline = 1;
#endif

#define PROMPT_MAIN  ">>> "
#define PROMPT_BLOCK "  > "

static int runSimpleRepl(void) {
#ifdef _WIN32
	SetConsoleOutputCP(65001);
	SetConsoleCP(65001);
#endif
	int exitRepl = 0;
	while (!exitRepl) {
		size_t lineCapacity = 8;
		size_t lineCount = 0;
		char ** lines = ALLOCATE(char *, lineCapacity);
		size_t totalData = 0;
		int valid = 1;
		char * allData = NULL;
		int inBlock = 0;
		int blockWidth = 0;
#ifdef SIMPLE_REPL_ENABLE_RLINE
		rline_exp_set_prompts(PROMPT_MAIN, "", 4, 0);
		rline_exit_string="";
		rline_exp_set_syntax("krk");
		rline_exp_set_tab_complete_func(NULL);
#endif
		while (1) {
			/* This would be a nice place for line editing */
			char buf[4096] = {0};
#ifdef SIMPLE_REPL_ENABLE_RLINE
			if (inBlock) {
				/* When entering multiple lines, the additional lines
				 * will show a single > (and keep the left side aligned) */
				rline_exp_set_prompts(PROMPT_BLOCK, "", 4, 0);
				/* Also add indentation as necessary */
				rline_preload = malloc(blockWidth + 1);
				for (int i = 0; i < blockWidth; ++i) {
					rline_preload[i] = ' ';
				}
				rline_preload[blockWidth] = '\0';
			}

			if (!enableRline) {
#else
			if (1) {
#endif
				fprintf(stdout, "%s", inBlock ? PROMPT_BLOCK : PROMPT_MAIN);
				fflush(stdout);
			}

#ifdef SIMPLE_REPL_ENABLE_RLINE
			rline_scroll = 0;
			if (enableRline) {
				if (rline(buf, 4096) == 0) {
					valid = 0;
					exitRepl = 1;
					break;
				}
			} else {
#endif
				char * out = fgets(buf, 4096, stdin);
				if (!out || !strlen(buf)) {
					fprintf(stdout, "^D\n");
					valid = 0;
					exitRepl = 1;
					break;
				}
#ifdef SIMPLE_REPL_ENABLE_RLINE
			}
#endif

			if (buf[strlen(buf)-1] != '\n') {
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
			if (lineLength > 1 && lines[i][lineLength-2] == ':') {
				inBlock = 1;
				blockWidth = countSpaces + 4;
				continue;
			} else if (lineLength > 1 && lines[i][lineLength-2] == '\\') {
				inBlock = 1;
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
			if (isSpaces && !i) valid = 0;

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
#ifdef SIMPLE_REPL_ENABLE_RLINE
			if (enableRline) {
				rline_history_insert(strdup(lines[i]));
				rline_scroll = 0;
			}
#endif
			free(lines[i]);
		}
		FREE_ARRAY(char *, lines, lineCapacity);
		if (valid) {
			KrkValue result = krk_interpret(allData, "<stdin>");
			if (!IS_NONE(result)) {
				krk_attachNamedValue(&vm.builtins->fields, "_", result);
				KrkClass * type = krk_getType(result);
				const char * formatStr = " \033[1;90m=> %s\033[0m\n";
				if (type->_reprer) {
					krk_push(result);
					result = krk_callDirect(type->_reprer, 1);
				} else if (type->_tostr) {
					krk_push(result);
					result = krk_callDirect(type->_tostr, 1);
				}
				if (!IS_STRING(result)) {
					fprintf(stdout, " \033[1;91m=> Unable to produce representation for value.\033[0m\n");
				} else {
					fprintf(stdout, formatStr, AS_CSTRING(result));
				}
			}
			krk_resetStack();
			free(allData);
		}

		(void)blockWidth;
	}

	return 0;
}
#undef PROMPT_MAIN
#undef PROMPT_BLOCK
