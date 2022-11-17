#include <kuroko/vm.h>
#include <kuroko/util.h>

/**
 * For use with @c ! formats, collects a @c KrkClass* and compares if the arg
 * is set. As a special case, the type may be @c NULL in which case failure is
 * guaranteed; this allows the standard library to reference potentially
 * uninitialized types (like fileio.File which may be uninitialized if the
 * module is not loaded, but may still need to be referenced as a potential
 * type in a function like @c print ).
 */
static int matchType(const char * _method_name, va_list * args, KrkValue arg) {
	KrkClass * type = va_arg(*args, KrkClass*);
	if (arg != KWARGS_VAL(0) && !krk_isInstanceOf(arg, type)) {
		krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%T'",
			_method_name, type ? type->name->chars : "unknown type", arg);
		return 0;
	}
	return 1;
}

/**
 * @brief Validate and parse arguments to a function similar to how managed
 *        function arguments are handled.
 *
 * This attempts to emulate CPythons' PyArg_ParseTupleAndKeywords.
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
		const char * _method_name,
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
	for (; *fmt; fmt++) {
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
			continue;
		}

		int wasPositional = 0;
		KrkValue arg = KWARGS_VAL(0);
		krk_push(OBJECT_VAL(krk_copyString(names[oarg],strlen(names[oarg]))));

		if (iarg < argc) {
			/* Positional arguments are pretty straightforward. */
			arg = argv[iarg];
			iarg++;
			wasPositional = 1;
		} else if ((required && !hasKw) || (hasKw && !krk_tableGet_fast(AS_DICT(argv[argc]), AS_STRING(krk_peek(0)), &arg) && required)) {
			/* If keyword argument lookup failed and this is not an optional argument, raise an exception. */
			krk_runtimeError(vm.exceptions->typeError, "%s() missing required positional argument: '%S'",
				_method_name, AS_STRING(krk_peek(0)));
			goto _error;
		}

		if (hasKw && krk_tableDelete(AS_DICT(argv[argc]), krk_peek(0)) && wasPositional) {
			/* We remove all arguments from kwargs. If we got this argument from a positional argument,
			 * and it was found during deletion, we raise a multiple-defs exception. */
			krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%S'",
				_method_name, AS_STRING(krk_peek(0)));
			goto _error;
		}

		switch (*fmt) {
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
				if (fmt[1] == '!') {
					fmt++;
					if (!matchType(_method_name, &args, arg)) goto _error;
				}
				KrkObj ** out = va_arg(args, KrkObj**);
				if (arg != KWARGS_VAL(0)) {
					if (IS_NONE(arg)) {
						*out = NULL;
					} else if (!IS_OBJECT(arg)) {
						TYPE_ERROR(heap object,arg);
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
				if (fmt[1] == '!') {
					fmt++;
					if (!matchType(_method_name, &args, arg)) goto _error;
				}
				KrkValue * out = va_arg(args, KrkValue*);
				if (arg != KWARGS_VAL(0)) {
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
				if (fmt[1] == '#') {
					fmt++;
					size = va_arg(args, size_t*);
				}
				if (arg != KWARGS_VAL(0)) {
					if (arg == NONE_VAL()) {
						*out = NULL;
						if (size) *size = 0;
					} else if (IS_STRING(arg)) {
						*out = AS_CSTRING(arg);
						if (size) *size = AS_STRING(arg)->length;
					} else {
						TYPE_ERROR(str or None,arg);
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
				if (fmt[1] == '#') {
					fmt++;
					size = va_arg(args, size_t*);
				}
				if (arg != KWARGS_VAL(0)) {
					if (IS_STRING(arg)) {
						*out = AS_CSTRING(arg);
						if (size) *size = AS_STRING(arg)->length;
					} else {
						TYPE_ERROR(str,arg);
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
#define NUMERIC(c,type) case c: { type * out = va_arg(args, type*); if (arg != KWARGS_VAL(0)) { if (!krk_long_to_int(arg, sizeof(type), out)) goto _error; } break; }
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
				if (arg != KWARGS_VAL(0)) {
					if (!IS_STRING(arg) || AS_STRING(arg)->codesLength != 1) {
						TYPE_ERROR(str of length 1,arg);
						goto _error;
					}
					*out = krk_unicodeCodepoint(AS_STRING(arg),0);
				}
				break;
			}

			/**
			 * @c f   Accept a Kuroko float as C float.
			 */
			case 'f': {
				float * out = va_arg(args, float*);
				if (arg != KWARGS_VAL(0)) {
					if (!IS_FLOATING(arg)) {
						TYPE_ERROR(float,arg);
						goto _error;
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
				if (arg != KWARGS_VAL(0)) {
					if (!IS_FLOATING(arg)) {
						TYPE_ERROR(float,arg);
						goto _error;
					}
					*out = AS_FLOATING(arg);
				}
				break;
			}

			/**
			 * @c p   Accept any value and examine its truthiness, returning an @c int.
			 *        Python's docs call this "predicate", if you were wondering where
			 *        the @c p came from. If bool conversion raises an exception, arg
			 *        parsing ends with failure and that exception remains set.
			 */
			case 'p': {
				int * out = va_arg(args, int*);
				if (arg != KWARGS_VAL(0)) {
					*out = !krk_isFalsey(arg);
					if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) goto _error;
				}
				break;
			}

			default: {
				krk_runtimeError(vm.exceptions->typeError, "unrecognized directive '%c' in format string", *fmt);
				goto _error;
			}
		}

		krk_pop();
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
				krk_runtimeError(vm.exceptions->typeError, "%s() got an unexpected keyword argument '%S'",
					_method_name, AS_STRING(entry->key));
				return 0;
			}
		}
	}

	return 1;

_error:
	krk_pop(); /* name of argument with error */
	return 0;
}

/**
 * @brief Variable argument version of @c krk_parseVArgs.
 */
int krk_parseArgs_impl(
		const char * _method_name,
		int argc, const KrkValue argv[], int hasKw,
		const char * format, const char ** names, ...) {
	va_list args;
	va_start(args, names);
	int result = krk_parseVArgs(_method_name,argc,argv,hasKw,format,names,args);
	va_end(args);
	return result;
}

