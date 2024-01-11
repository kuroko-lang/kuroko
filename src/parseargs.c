/**
 * @brief Function argument parser.
 *
 * Provides a simple interface for parsing arguments passed to native functions.
 *
 * This is similar to CPython's PyArg_ParseTupleAndKeywords, and many of the options
 * work the same way (though with some exceptions). With the utilities provided here,
 * C bindings can parse positional and keyword arguments, with automatic type checking
 * and conversion to C types.
 */
#include <kuroko/vm.h>
#include <kuroko/util.h>

/**
 * @brief Format a TypeError exception for an argument.
 *
 * @param method_name  Method name from parseVArgs, after possibly modification by `:`
 * @param expected     Description of expected type; generally a type name, but maybe something like "str of length 1".
 * @param arg          The value passed that failed the type check.
 * @param argName      Name of the argument. If NULL or zero-length, argument name is not included in the description.
 */
_cold
static void raise_TypeError(const char * method_name, const char * expected, KrkValue arg, const char * argName) {
	krk_runtimeError(vm.exceptions->typeError,
		"%s()%s%s expects %s, not '%T'",
		method_name, (argName && *argName) ? " argument " : "", (argName && *argName) ? argName : "",
		expected, arg);
}

/**
 * @brief Get the method name to use for an error message.
 *
 * If the format string has a ':' it is taken as the start of an alternative method name
 * to include in error messages. This may be useful when calling the macro version of
 * @c krk_parseArgs in a @c __new__ or @c __init__ method.
 *
 * @param method_name Original method name passed to krk_parseArgs.
 * @param fmt         Pointer to somewhere in the format string up to the colon.
 */
_cold
static const char * methodName(const char * method_name, const char * fmt) {
	const char * maybeColon = strchr(fmt, ':');
	return maybeColon ? maybeColon + 1 : method_name;
}

/* Just to avoid repeating ourselves... */
#define _method_name (methodName(orig_method_name, fmt))

/**
 * @brief Extract arguments from kwargs dict, but keep references to them.
 *
 * Searches for @p argName in the @p kwargs dict. If found, extracts the value
 * into @p out and stores a reference to it in @p refList and then deletes
 * the original entry from @p kwargs.
 *
 * @param kwargs  Original keyword args dictionary, which will be mutated.
 * @param argName Argument name to search for.
 * @param out     Slot to place argument value in.
 * @param refList List to store references for garbage collection.
 * @returns Non-zero if the argument was not found.
 */
static int extractKwArg(KrkTable * kwargs, KrkString * argName, KrkValue * out, KrkValueArray * refList) {
	if (!krk_tableGet_fast(kwargs, argName, out)) return 1;
	krk_writeValueArray(refList, *out);
	krk_tableDeleteExact(kwargs, OBJECT_VAL(argName));
	return 0;
}

/**
 * @brief Validate and parse arguments to a function similar to how managed
 *        function arguments are handled.
 *
 * This works like a fancy scanf. We accept the original argument specification
 * (argc,argv,hasKw), a format string, an array of argument names, and then var
 * args that are generally pointers to where to stick results.
 *
 * @param argc Original positional argument count.
 * @param argv Original argument list, @c argv[argc] should be a dict if hasKw is set.
 * @param hasKw Whether @c argv[argc] has a dict of keyword arguments.
 * @param fmt String describing formats of expected arguments.
 * @param names Array of strings of parameter names.
 * @param args  var args
 * @returns 1 on success, 0 on error.
 */
int krk_parseVArgs(
		const char * orig_method_name,
		int argc, const KrkValue argv[], int hasKw,
		const char * fmt, const char ** names, va_list args) {
	int iarg = 0;           /**< Index into positional input arguments */
	int oarg = 0;           /**< Index into names array */
	int required = 1;       /**< Parser state, whether required arguments are being collected */
	int acceptextrakws = 0; /**< Whether extra keyword args should produce an error (0) or not (1) */

	if (*fmt == '.') {
		/**
		 * If the format string starts with `.` then argument processing skips the first argument
		 * on the assumption that this is a method and the first argument has already been
		 * handled by the method wrapper macros or directly by the function. This makes error
		 * messages a bit nicer, as argument counts will exclude the implicit self.
		 */
		argv++;
		argc--;
		fmt++;
	}

	/* Required args */
	while (*fmt) {
		if (*fmt == ':') break;
		if (*fmt == '|') {
			/**
			 * @c | begins optional arguments - eg. default args. Every format option after
			 * this point should be preset to usable default value, as it will not be touched
			 * if the argument is not found.
			 */
			if (!required) {
				krk_runtimeError(vm.exceptions->typeError, "format string has multiple |s");
				return 1;
			}
			required = 0;
			fmt++;
			continue;
		}
		if (*fmt == '*') {
			/**
			 * @c * works like @c *args would in a Kuroko function signature, collecting
			 * all remaining positional arguments into a list. It does this be returning
			 * the count of remaining arguments (int) and the pointer to their start in the
			 * original argument list (KrkValue*).
			 *
			 * This also implicitly signals the end of required arguments and all later
			 * arguments are automatically optional, without needing to use @c |.
			 */
			int * out_c = va_arg(args, int *);
			const KrkValue ** out_v = va_arg(args, const KrkValue **);
			*out_c = argc - iarg;
			*out_v = &argv[iarg];
			iarg = argc;
			required = 0;
			fmt++;
			continue;
		}
		if (*fmt == '$') {
			/**
			 * @c $ indicates the end of positional arguments. Everything after this point is
			 * only accepted as a keyword argument. @c $ must appear after one of @c | or @c *.
			 *
			 * If any positional arguments remain when @c $ is encountred, a too-many arguments
			 * exception will be raised.
			 */
			if (required) {
				krk_runtimeError(vm.exceptions->typeError, "$ must be after | or * in format string");
				return 1;
			}
			if (iarg < argc) break;
			fmt++;
			continue;
		}
		if (*fmt == '~') {
			/**
			 * If @c ~ is encountered anywhere in the format string, then extraneous keyword arguments
			 * are left as-is and no exception is raised when they are found. As keyword arguments are
			 * deleted from the kwargs dict while processing other arguments, this means if @c hasKw
			 * is set then @c argv[argc] will be left with only the unhandled keyword arguments, same
			 * as for a @c **kwargs argument in a Kuroko function signature.
			 */
			acceptextrakws = 1;
			fmt++;
			continue;
		}

		KrkValue arg = KWARGS_VAL(0);

		if (iarg < argc) {
			/* Positional arguments are pretty straightforward. */
			arg = argv[iarg];
			iarg++;
		} else if ((required && !hasKw) || (hasKw && extractKwArg(AS_DICT(argv[argc]), krk_copyString(names[oarg],strlen(names[oarg])), &arg, AS_LIST(argv[argc+1])) && required)) {
			/* If keyword argument lookup failed and this is not an optional argument, raise an exception. */
			krk_runtimeError(vm.exceptions->typeError, "%s() missing required positional argument: '%s'",
				_method_name, names[oarg]);
			goto _error;
		}

		char argtype = *fmt++;

		if (*fmt == '?') {
			/* "is present", useful for things where relying on a default isn't useful but you
			 * still want to have all the type checking and automatic parsing. */
			fmt++;
			int * out = va_arg(args, int*);
			*out = !krk_valuesSame(arg, KWARGS_VAL(0));
		}

		if (*fmt == '!') {
			/* "of type", thrown an exception if the argument was present but was not
			 * an instance of a given class. Originally just for @c O and @c V but
			 * now available anywhere, though likely not useful for other types.
			 * Maybe if you want @c p to only be a bool this could be useful? */
			fmt++;
			KrkClass * type = va_arg(args, KrkClass*);
			if (!krk_valuesSame(arg, KWARGS_VAL(0)) && !krk_isInstanceOf(arg, type)) {
				raise_TypeError(_method_name, type ? type->name->chars : "unknown type", arg, names[oarg]);
				goto _error;
			}
		}

		switch (argtype) {
			/**
			 * @c O   Collect an object (with @c ! - of a given type) and place it in
			 *        in the @c KrkObj** var arg. The object must be a heap object,
			 *        so this can not be used to collect boxed value types like @c int
			 *        or @c float - use @c V for those instead. As an exception to the
			 *        heap object requirements, @c None is accepted and will result
			 *        in @c NULL (but if a type is requested, the type check will fail
			 *        before @c None can be evaluated).
			 */
			case 'O': {
				KrkObj ** out = va_arg(args, KrkObj**);
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					if (IS_NONE(arg)) {
						*out = NULL;
					} else if (!IS_OBJECT(arg)) {
						raise_TypeError(_method_name, "heap object", arg, names[oarg]);
						goto _error;
					} else {
						*out = AS_OBJECT(arg);
					}
				}
				break;
			}

			/**
			 * @c V   Accept any value (with @c ! - of a given type) and place a value
			 *        reference in the @c KrkValue* var arg. This works with boxed value
			 *        types as well, so it is safe for use with @c int and @c float and
			 *        so on. The type check is equivalent to @c instanceof. As a special
			 *        case - as with @c O - the type may be @c NULL in which case type
			 *        checking is guaranteed to fail but parsing will not. The resulting
			 *        error message is less informative in this case.
			 */
			case 'V': {
				KrkValue * out = va_arg(args, KrkValue*);
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					*out = arg;
				}
				break;
			}

			/**
			 * @c z   Collect one string or None and place a pointer to it in
			 *        a `const char **`. If @c # is specified,  the size of the
			 *        string is also placed in a following @c size_t* var arg.
			 *        If the argument is @c None the result is @c NULL and
			 *        the size is set to 0.
			 */
			case 'z': {
				char ** out = va_arg(args, char **);
				size_t * size = NULL;
				if (*fmt == '#') {
					fmt++;
					size = va_arg(args, size_t*);
				}
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					if (IS_NONE(arg)) {
						*out = NULL;
						if (size) *size = 0;
					} else if (IS_STRING(arg)) {
						*out = AS_CSTRING(arg);
						if (size) *size = AS_STRING(arg)->length;
					} else {
						raise_TypeError(_method_name, "str or None", arg, names[oarg]);
						goto _error;
					}
				}
				break;
			}

			/**
			 * @c s   Same as @c z but does not accept None.
			 */
			case 's': {
				char ** out = va_arg(args, char **);
				size_t * size = NULL;
				if (*fmt == '#') {
					fmt++;
					size = va_arg(args, size_t*);
				}
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					if (IS_STRING(arg)) {
						*out = AS_CSTRING(arg);
						if (size) *size = AS_STRING(arg)->length;
					} else {
						raise_TypeError(_method_name, "str", arg, names[oarg]);
						goto _error;
					}
				}
				break;
			}

			/**
			 * Integer conversions.
			 *
			 * TODO Currently no overflow checking is done for any case, but we should do it
			 *      for at least the signed values to align with the CPython API this is
			 *      all based on... The distinct signed vs. unsigned variants are intended
			 *      both for future compatibility and to make intent clear, but have no
			 *      functional difference at this point.
			 */
#define NUMERIC(c,type) case c: { type * out = va_arg(args, type*); if (!krk_valuesSame(arg, KWARGS_VAL(0))) { if (!krk_long_to_int(arg, sizeof(type), out)) goto _error; } break; }
			NUMERIC('b',unsigned char)
			NUMERIC('h',short)
			NUMERIC('H',unsigned short)
			NUMERIC('i',int)
			NUMERIC('I',unsigned int)
			NUMERIC('l',long)
			NUMERIC('k',unsigned long)
			NUMERIC('L',long long)
			NUMERIC('K',unsigned long long)
			NUMERIC('n',ssize_t)
			NUMERIC('N',size_t)

			/**
			 * @c C   Accept a string of length one and convert it to
			 *        a C int in a similar manner to @c ord.
			 */
			case 'C': {
				int * out = va_arg(args, int*);
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					if (!IS_STRING(arg) || AS_STRING(arg)->codesLength != 1) {
						raise_TypeError(_method_name, "str of length 1", arg, names[oarg]);
						goto _error;
					}
					*out = krk_unicodeCodepoint(AS_STRING(arg),0);
				}
				break;
			}

#ifndef KRK_NO_FLOAT
			/**
			 * @c f   Accept a Kuroko float as C float.
			 */
			case 'f': {
				float * out = va_arg(args, float*);
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					if (!IS_FLOATING(arg)) {
						KrkClass * type = krk_getType(arg);
						krk_push(arg);
						if (!krk_bindMethod(type, S("__float__"))) {
							krk_pop();
							raise_TypeError(_method_name, "float", arg, names[oarg]);
							goto _error;
						}
						arg = krk_callStack(0);
					}
					*out = AS_FLOATING(arg);
				}
				break;
			}

			/**
			 * @c d   Accept a Kuroko float as C double.
			 */
			case 'd': {
				double * out = va_arg(args, double*);
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					if (!IS_FLOATING(arg)) {
						KrkClass * type = krk_getType(arg);
						krk_push(arg);
						if (!krk_bindMethod(type, S("__float__"))) {
							krk_pop();
							raise_TypeError(_method_name, "float", arg, names[oarg]);
							goto _error;
						}
						arg = krk_callStack(0);
					}
					*out = AS_FLOATING(arg);
				}
				break;
			}
#else
			case 'f':
			case 'd':
				krk_runtimeError(vm.exceptions->typeError, "no float support");
				goto _error;
#endif

			/**
			 * @c p   Accept any value and examine its truthiness, returning an @c int.
			 *        Python's docs call this "predicate", if you were wondering where
			 *        the @c p came from. If bool conversion raises an exception, arg
			 *        parsing ends with failure and that exception remains set.
			 */
			case 'p': {
				int * out = va_arg(args, int*);
				if (!krk_valuesSame(arg, KWARGS_VAL(0))) {
					*out = !krk_isFalsey(arg);
					if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) goto _error;
				}
				break;
			}

			default: {
				krk_runtimeError(vm.exceptions->typeError, "unrecognized directive '%c' in format string", argtype);
				goto _error;
			}
		}

		oarg++;
	}

	if (iarg < argc) {
		/**
		 * If we got through the format string and there are still positional arguments,
		 * we got more than we expected and should raise an exception.
		 */
		krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)",
			_method_name, required ? "exactly" : "at most", oarg, oarg == 1 ? "" : "s", argc);
		return 0;
	}

	if (!acceptextrakws && hasKw && AS_DICT(argv[argc])->count) {
		/**
		 * If we don't accept extra keyword arguments and there's still anything left
		 * in the dict, raise an exception about unexpected keyword arguments. The
		 * remaining key (or keys) should be a string, so we should find at least one
		 * thing to complain about by name...
		 */
		for (size_t i = 0; i < AS_DICT(argv[argc])->capacity; ++i) {
			KrkTableEntry * entry = &AS_DICT(argv[argc])->entries[i];
			if (IS_STRING(entry->key)) {
				/* See if this was the name of an argument, which means it was already provided as a positional argument. */
				for (int j = 0; j < oarg; ++j) {
					if (*names[j] && strlen(names[j]) == AS_STRING(entry->key)->length && !strcmp(names[j], AS_CSTRING(entry->key))) {
						krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%s'",
							_method_name, names[j]);
						return 0;
					}
				}
				/* Otherwise just say it was unexpected. */
				krk_runtimeError(vm.exceptions->typeError, "%s() got an unexpected keyword argument '%S'",
					_method_name, AS_STRING(entry->key));
				return 0;
			}
		}
	}

	return 1;

_error:
	return 0;
}

/**
 * @brief Variable argument version of @c krk_parseVArgs.
 */
int krk_parseArgs_impl(
		const char * method_name,
		int argc, const KrkValue argv[], int hasKw,
		const char * format, const char ** names, ...) {
	va_list args;
	va_start(args, names);
	int result = krk_parseVArgs(method_name,argc,argv,hasKw,format,names,args);
	va_end(args);
	return result;
}

