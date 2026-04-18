/**
 * @brief Keigo - K's embedded implementation of getopt
 * @author K. Lange <klange@toaruos.org>
 * @version 2026.4.18
 *
 * Stripped down implementation of 'getopt' with no argument permutating,
 * no native long options, no integrated error printing; still supports
 * option-arguments and optional option-arguments, and you can use '-:'
 * to emulate long options.
 *
 * Returns '?' for unknown option, ':' for missing required argument,
 * and sets ctx->opt for both of those cases. Otherwise, returns the
 * option character encountered, and sets ctx->arg for any argument;
 * missing optional arguments set ctx->arg to 0. Finally returns -1
 * when done with options.
 *
 * ctx->i is your optind equivalent; use it to know where to start looking
 * for non-options in your argv once keigo returns -1.
 *
 * Any single byte is accepted as an option character except ':' or '\0'.
 * '-' should only be used as a required option argument, as '--' will
 * be skipped and halt option processing (this also implicitly means
 * that '-- foo' does not work the same as '--foo', which imo is a bonus).
 *
 * Since keigo stores its context in a user-provided struct, it's reentrant,
 * unlike normal getopt.
 */
#ifndef KEIGO_H
#define KEIGO_H

struct Keigo {
	int i; /* optind */
	int opt; /* optopt */
	char * arg; /* optarg */
	char * nextchar; /* internal state */
};

/* Since we do this five separate times... */
#define keigo_finish_arg() do { \
	ctx->nextchar = 0; \
	ctx->i++; \
} while (0)

static int keigo(struct Keigo *ctx, int argc, char * const argv[], const char *options) {
	if (!ctx->i) keigo_finish_arg();
	if (ctx->i >= argc) return -1; /* already finished */
	if (!ctx->nextchar) {
		ctx->nextchar = argv[ctx->i];
		if (*ctx->nextchar != '-' || !ctx->nextchar[1]) return ctx->nextchar = 0, -1; /* non-option (incl. '-') */
		ctx->nextchar++; /* advance over dash */
		if (*ctx->nextchar == '-' && !ctx->nextchar[1]) return ctx->nextchar = 0, ctx->i++, -1; /* '--' needs to be skipped */
	}
	const char * opt;
	for (opt = options; *opt; opt++) if (*opt == *ctx->nextchar) break; /* strchrnul */
	if (!*opt || *opt == ':') { /* unknown option; force ':' to not be accepted */
		ctx->opt = *ctx->nextchar;
		if (!*++ctx->nextchar) keigo_finish_arg();
		return '?';
	}
	if (opt[1] == ':') {
		if (ctx->nextchar[1]) { /* arg from rest of this arg */
			ctx->arg = &ctx->nextchar[1];
		} else if (opt[2] == ':') { /* optional arg that was unset */
			ctx->arg = 0;
		} else { /* arg from next arg */
			if (ctx->i + 1 == argc) { /* required arg but at end of argv */
				keigo_finish_arg(); /* don't re-process this if we get called again */
				return ctx->opt = *opt, ':';
			}
			ctx->arg = argv[++ctx->i];
		}
		keigo_finish_arg();
	} else if (!*++ctx->nextchar) keigo_finish_arg();
	return *opt;
}

#undef keigo_finish_arg
#endif /* !KEIGO_H */

#ifdef KEIGO_TEST_APP
/*
 * Example / test code.
 * cc -DKEIGO_TEST_APP -x c -o keigo keigo.h
 */
#include <stdio.h>
#include <string.h>

int main (int argc, char *argv[]) {
	struct Keigo ctx = {0};
	int opt;
	while ((opt = keigo(&ctx, argc, argv, "ab:c::d-:")) != -1) {
		switch (opt) {
			case 'a': printf("'-a' was set\n"); break;
			case 'b': printf("'-b' had argument '%s'\n", ctx.arg); break;
			case 'c': printf(ctx.arg ? "'-c' had argument '%s'\n" : "'-c' had no argument\n", ctx.arg); break;
			case 'd': printf("'-d' was set\n"); break;
			case '-':
				if (!strcmp(ctx.arg,"help")) return printf("%s: no help available\n", argv[0]), 0;
				return fprintf(stderr, "%s: unknown option '--%s'\n", argv[0], ctx.arg), 1;
			case '?': return fprintf(stderr, "%s: unknown option '-%c'\n", argv[0], ctx.opt), 1;
			case ':': return fprintf(stderr, "%s: option '-%c' requires an argument\n", argv[0], ctx.opt), 1;
		}
	}
	if (ctx.i == argc) return fprintf(stderr, "%s: expected arguments\n", argv[0]), 2;
	for (; ctx.i < argc; ctx.i++) {
		printf("got argument '%s'\n", argv[ctx.i]);
	}
	return 0;
}
#endif /* KEIGO_TEST_APP */
