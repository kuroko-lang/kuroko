/**
 * @file exceptions.c
 * @brief Definitions and native method bindings for error types.
 */
#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#include "private.h"
#include "opcode_enum.h"

/**
 * @def ADD_EXCEPTION_CLASS(obj,name,baseClass)
 *
 * Convenience macro for creating exception types.
 */
#define ADD_EXCEPTION_CLASS(obj,name,baseClass) KrkClass * name; do { \
	ADD_BASE_CLASS(obj,#name,baseClass); \
	krk_finalizeClass(obj); \
	name = obj; \
	(void)name; \
} while (0)

#define IS_BaseException(o)    (likely(krk_isInstanceOf(o,vm.exceptions->baseException)))
#define AS_BaseException(o)    (AS_INSTANCE(o))
#define IS_KeyError(o)     (likely(krk_isInstanceOf(o,vm.exceptions->keyError)))
#define AS_KeyError(o)     (AS_INSTANCE(o))
#define IS_SyntaxError(o)  (likely(krk_isInstanceOf(o,vm.exceptions->syntaxError)))
#define AS_SyntaxError(o)  (AS_INSTANCE(o))
#define CURRENT_CTYPE KrkInstance*
#define CURRENT_NAME  self

/**
 * @brief Initialize an exception object.
 *
 * Native binding for BaseException.__init__
 *
 * @param arg Optional string to attach to the exception object.
 */
KRK_Method(BaseException,__init__) {
	if (argc > 1) {
		krk_attachNamedValue(&self->fields, "arg", argv[1]);
	}
	krk_attachNamedValue(&self->fields, "__cause__", NONE_VAL());
	krk_attachNamedValue(&self->fields, "__context__", NONE_VAL());
	return NONE_VAL();
}

/**
 * @brief Create a string representation of an BaseException.
 *
 * Native binding for @c BaseException.__repr__
 *
 * Generates a string representation of the form @c "BaseException(arg)" .
 */
KRK_Method(BaseException,__repr__) {
	KrkValue arg;
	struct StringBuilder sb = {0};

	pushStringBuilderStr(&sb, self->_class->name->chars, self->_class->name->length);
	pushStringBuilder(&sb, '(');

	if (krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg)) {
		/* repr it */
		krk_push(arg);
		KrkValue repred = krk_callDirect(krk_getType(arg)->_reprer, 1);
		pushStringBuilderStr(&sb, AS_CSTRING(repred), AS_STRING(repred)->length);
	}

	pushStringBuilder(&sb, ')');

	return finishStringBuilder(&sb);
}

/**
 * @brief Obtain a descriptive string from an exception.
 *
 * Native binding for @c BaseException.__str__
 *
 * For most exceptions, this is the 'arg' value attached at initialization
 * and is printed during a traceback after the name of the exception type.
 */
KRK_Method(BaseException,__str__) {
	KrkValue arg;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg) || IS_NONE(arg)) {
		return OBJECT_VAL(S(""));
	} else if (!IS_STRING(arg)) {
		KrkClass * type = krk_getType(arg);
		if (type->_tostr) {
			krk_push(arg);
			return krk_callDirect(krk_getType(arg)->_tostr, 1);
		}
		return OBJECT_VAL(S(""));
	} else {
		return arg;
	}
}

KRK_Method(KeyError,__str__) {
	if (!IS_INSTANCE(argv[0])) return NONE_VAL(); /* uh oh */
	KrkValue arg;
	if (krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg)) {
		KrkClass * type = krk_getType(arg);
		if (type->_reprer) {
			krk_push(arg);
			return krk_callDirect(krk_getType(arg)->_reprer, 1);
		}
	}
	return FUNC_NAME(BaseException,__str__)(argc,argv,hasKw);
}

/**
 * @brief Generate printable text for a syntax error.
 *
 * Native binding for @c SyntaxError.__str__
 *
 * Syntax errors are handled specially by the traceback generator so that they
 * can print the original source line containing the erroneous input, so instead
 * of printing {BaseException.__class__.__name__}: {str(BaseException)} we just print
 * {str(BaseException)} for syntax errors and they handle the rest. This is a bit
 * of a kludge, but it works for now.
 */
KRK_Method(SyntaxError,__str__) {
	/* .arg */
	KrkValue file, line, lineno, colno, arg, func, width;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("file")), &file) || !IS_STRING(file)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("line")), &line) || !IS_STRING(line)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("lineno")), &lineno) || !IS_INTEGER(lineno)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("colno")), &colno) || !IS_INTEGER(colno)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg) || !IS_STRING(arg)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("func")), &func)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("width")), &width) || !IS_INTEGER(width)) goto _badSyntaxError;

	if (AS_INTEGER(colno) <= 0) colno = INTEGER_VAL(1);

	int definitelyNotFullWidth = !(AS_STRING(line)->obj.flags & KRK_OBJ_FLAGS_STRING_MASK);

	krk_push(OBJECT_VAL(S("^")));
	if (definitelyNotFullWidth && AS_INTEGER(width) > 1) {
		for (krk_integer_type i = 1; i < AS_INTEGER(width); ++i) {
			krk_push(OBJECT_VAL(S("^")));
			krk_addObjects();
		}
	}

	krk_push(OBJECT_VAL(S("  File \"{}\", line {}{}\n    {}\n    {}{}\n{}: {}")));
	unsigned int column = AS_INTEGER(colno);
	char * tmp = malloc(column);
	memset(tmp,' ',column);
	tmp[column-1] = '\0';
	krk_push(OBJECT_VAL(krk_takeString(tmp,column-1)));
	krk_push(OBJECT_VAL(self->_class->name));
	if (IS_STRING(func)) {
		krk_push(OBJECT_VAL(S(" in ")));
		krk_push(func);
		krk_addObjects();
	} else {
		krk_push(OBJECT_VAL(S("")));
	}
	KrkValue formattedString = krk_string_format(9,
		(KrkValue[]){krk_peek(3), file, lineno, krk_peek(0), line, krk_peek(2), krk_peek(4), krk_peek(1), arg}, 0);
	krk_pop(); /* instr */
	krk_pop(); /* class */
	krk_pop(); /* spaces */
	krk_pop(); /* format string */
	krk_pop(); /* carets */

	return formattedString;

_badSyntaxError:
	return OBJECT_VAL(S("SyntaxError: invalid syntax"));
}


/**
 * @brief Bind native methods and classes for exceptions.
 *
 * Called on VM initialization to create the base classes for exception types
 * and bind the native methods for exception objects.
 */
_noexport
void _createAndBind_exceptions(void) {
	/* Add exception classes */
	ADD_EXCEPTION_CLASS(vm.exceptions->baseException, BaseException, vm.baseClasses->objectClass);
	BIND_METHOD(BaseException,__init__);
	BIND_METHOD(BaseException,__repr__);
	BIND_METHOD(BaseException,__str__);
	krk_finalizeClass(BaseException);

	/* KeyboardInterrupt is currently the only thing that directly inherits from BaseException. */
	ADD_EXCEPTION_CLASS(vm.exceptions->keyboardInterrupt, KeyboardInterrupt, BaseException);

	/* Everything else subclasses Exception */
	ADD_EXCEPTION_CLASS(vm.exceptions->Exception, Exception, BaseException);

	/* TypeError has a subclass ArgumentError, which is what we raise on arity mismatches */
	ADD_EXCEPTION_CLASS(vm.exceptions->typeError, TypeError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->argumentError, ArgumentError, TypeError);

	/* KeyError gets its own string conversion so it can repr msg */
	ADD_EXCEPTION_CLASS(vm.exceptions->keyError, KeyError, Exception);
	BIND_METHOD(KeyError,__str__);
	krk_finalizeClass(KeyError);

	/* There is nothing special about these. */
	ADD_EXCEPTION_CLASS(vm.exceptions->indexError, IndexError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->attributeError, AttributeError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->nameError, NameError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->importError, ImportError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->ioError, IOError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->valueError, ValueError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->zeroDivisionError, ZeroDivisionError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->notImplementedError, NotImplementedError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->assertionError, AssertionError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->OSError, OSError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->SystemError, SystemError, Exception);

	/* SyntaxError also gets a special __str__ method... but also the whole exception
	 * printer has special logic for it - TODO fix that */
	ADD_EXCEPTION_CLASS(vm.exceptions->syntaxError, SyntaxError, vm.exceptions->Exception);
	BIND_METHOD(SyntaxError,__str__);
	krk_finalizeClass(SyntaxError);
}

static void dumpInnerException(KrkValue exception, int depth) {
	if (depth > 10) {
		fprintf(stderr, "Too many inner exceptions encountered.\n");
		return;
	}

	krk_push(exception);
	if (IS_INSTANCE(exception)) {

		KrkValue inner;

		/* Print cause or context */
		if (krk_tableGet(&AS_INSTANCE(exception)->fields, OBJECT_VAL(S("__cause__")), &inner) && !IS_NONE(inner)) {
			dumpInnerException(inner, depth + 1);
			fprintf(stderr, "\nThe above exception was the direct cause of the following exception:\n\n");
		} else if (krk_tableGet(&AS_INSTANCE(exception)->fields, OBJECT_VAL(S("__context__")), &inner) && !IS_NONE(inner)) {
			dumpInnerException(inner, depth + 1);
			fprintf(stderr, "\nDuring handling of the above exception, another exception occurred:\n\n");
		}

		KrkValue tracebackEntries;
		if (krk_tableGet(&AS_INSTANCE(exception)->fields, OBJECT_VAL(S("traceback")), &tracebackEntries)
			&& IS_list(tracebackEntries) && AS_LIST(tracebackEntries)->count > 0) {

			/* This exception has a traceback we can print. */
			fprintf(stderr, "Traceback (most recent call last):\n");
			for (size_t i = 0; i < AS_LIST(tracebackEntries)->count; ++i) {

				/* Quietly skip invalid entries as we don't want to bother printing explanatory text for them */
				if (!IS_TUPLE(AS_LIST(tracebackEntries)->values[i])) continue;
				KrkTuple * entry = AS_TUPLE(AS_LIST(tracebackEntries)->values[i]);
				if (entry->values.count != 2) continue;
				if (!IS_CLOSURE(entry->values.values[0])) continue;
				if (!IS_INTEGER(entry->values.values[1])) continue;

				/* Get the function and instruction index from this traceback entry */
				KrkClosure * closure = AS_CLOSURE(entry->values.values[0]);
				KrkCodeObject * function = closure->function;
				size_t instruction = AS_INTEGER(entry->values.values[1]);

				/* Calculate the line number */
				int lineNo = (int)krk_lineNumber(&function->chunk, instruction);

				/* Print the simple stuff that we already know */
				fprintf(stderr, "  File \"%s\", line %d, in %s\n",
					(function->chunk.filename ? function->chunk.filename->chars : "?"),
					lineNo,
					(function->name ? function->name->chars : "(unnamed)"));

#ifndef KRK_NO_SOURCE_IN_TRACEBACK
			/* Try to open the file */
			if (function->chunk.filename) {
				FILE * f = fopen(function->chunk.filename->chars, "r");
				if (f) {
					int line = 1;
					do {
						int c = fgetc(f);
						if (c < -1) break;
						if (c == '\n') {
							line++;
							continue;
						}
						if (line == lineNo) {
							fprintf(stderr,"    ");
							while (c == ' ') c = fgetc(f);
							do {
								fputc(c, stderr);
								c = fgetc(f);
							} while (!feof(f) && c > 0 && c != '\n');
							fprintf(stderr, "\n");
							break;
						}
					} while (!feof(f));
					fclose(f);
				}
			}
#endif
			}
		}
	}

	/* Is this a SyntaxError? Handle those specially. */
	if (krk_isInstanceOf(exception, vm.exceptions->syntaxError)) {
		KrkValue result = krk_callDirect(krk_getType(exception)->_tostr, 1);
		fprintf(stderr, "%s\n", AS_CSTRING(result));
		return;
	}

	/* Clear the exception state while printing the exception. */
	int hadException = krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION;
	krk_currentThread.flags &= ~(KRK_THREAD_HAS_EXCEPTION);

	/* Prepare to print exception name with prefixed module, if it's not __builtins__. */
	KrkClass * type = krk_getType(exception);
	KrkValue module = NONE_VAL();
	krk_tableGet(&type->methods, OBJECT_VAL(S("__module__")), &module);
	if (!(IS_NONE(module) || (IS_STRING(module) && AS_STRING(module) == S("builtins")))) {
		fprintf(stderr, "%s.", AS_CSTRING(module));
	}

	/* Print type name */
	fprintf(stderr, "%s", krk_typeName(exception));

	/* Stringify it. */
	KrkValue result = krk_callDirect(krk_getType(exception)->_tostr, 1);
	if (!IS_STRING(result) || AS_STRING(result)->length == 0) {
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, ": ");
		fwrite(AS_CSTRING(result), AS_STRING(result)->length, 1, stderr);
		fprintf(stderr, "\n");
	}

	/* Turn the exception flag back on */
	krk_currentThread.flags |= hadException;
}

/**
 * Display a traceback by scanning up the stack / call frames.
 * The format of the output here is modeled after the output
 * given by CPython, so we display the outermost call first
 * and then move inwards; on each call frame we try to open
 * the source file and print the corresponding line.
 */
void krk_dumpTraceback(void) {
	if (!krk_valuesEqual(krk_currentThread.currentException,NONE_VAL())) {
		dumpInnerException(krk_currentThread.currentException, 0);
	}
}

/**
 * Attach a traceback to the current exception object, if it doesn't already have one.
 */
static void attachTraceback(void) {
	if (IS_INSTANCE(krk_currentThread.currentException)) {
		KrkInstance * theException = AS_INSTANCE(krk_currentThread.currentException);
		KrkValue tracebackList;
		if (krk_tableGet(&theException->fields, OBJECT_VAL(S("traceback")), &tracebackList)) {
			krk_push(tracebackList);
		} else {
			krk_push(NONE_VAL());
		}
		tracebackList = krk_list_of(0,NULL,0);
		krk_push(tracebackList);

		/* Build the traceback object */
		if (krk_currentThread.frameCount) {

			/* Go up until we get to the exit frame */
			size_t frameOffset = 0;
			if (krk_currentThread.stackTop > krk_currentThread.stack) {
				size_t stackOffset = krk_currentThread.stackTop - krk_currentThread.stack - 1;
				while (stackOffset > 0 && !IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset], OP_PUSH_TRY)) stackOffset--;
				frameOffset = krk_currentThread.frameCount - 1;
				while (frameOffset > 0 && krk_currentThread.frames[frameOffset].slots > stackOffset) frameOffset--;
			}

			for (size_t i = frameOffset; i < krk_currentThread.frameCount; i++) {
				KrkCallFrame * frame = &krk_currentThread.frames[i];
				KrkTuple * tbEntry = krk_newTuple(2);
				krk_push(OBJECT_VAL(tbEntry));
				tbEntry->values.values[tbEntry->values.count++] = OBJECT_VAL(frame->closure);
				tbEntry->values.values[tbEntry->values.count++] = INTEGER_VAL(frame->ip - frame->closure->function->chunk.code - 1);
				krk_writeValueArray(AS_LIST(tracebackList), OBJECT_VAL(tbEntry));
				krk_pop();
			}
		}

		if (IS_list(krk_peek(1))) {
			KrkValueArray * existingTraceback = AS_LIST(krk_peek(1));
			for (size_t i = 0; i < existingTraceback->count; ++i) {
				krk_writeValueArray(AS_LIST(tracebackList), existingTraceback->values[i]);
			}
		}

		krk_attachNamedValue(&theException->fields, "traceback", tracebackList);
		krk_pop();
		krk_pop();
	} /* else: probably a legacy 'raise str', just don't bother. */
}

void krk_attachInnerException(KrkValue innerException) {
	if (IS_INSTANCE(krk_currentThread.currentException)) {
		KrkInstance * theException = AS_INSTANCE(krk_currentThread.currentException);
		if (krk_valuesSame(krk_currentThread.currentException,innerException)) {
			/* re-raised? */
			return;
		} else {
			krk_attachNamedValue(&theException->fields, "__context__", innerException);
		}
	}
}

void krk_raiseException(KrkValue base, KrkValue cause) {
	if (IS_CLASS(base)) {
		krk_push(base);
		base = krk_callStack(0);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return;
	}
	krk_currentThread.currentException = base;
	if (IS_CLASS(cause)) {
		krk_push(cause);
		cause = krk_callStack(0);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return;
	}
	if (IS_INSTANCE(krk_currentThread.currentException) && !IS_NONE(cause)) {
		krk_attachNamedValue(&AS_INSTANCE(krk_currentThread.currentException)->fields,
			"__cause__", cause);
	}
	attachTraceback();
	krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
}

/**
 * Raise an exception. Creates an exception object of the requested type
 * and formats a message string to attach to it. Exception classes are
 * found in vm.exceptions and are initialized on startup.
 */
KrkValue krk_runtimeError(KrkClass * type, const char * fmt, ...) {
	KrkValue msg = KWARGS_VAL(0);
	struct StringBuilder sb = {0};

	va_list args;
	va_start(args, fmt);

	if (!strcmp(fmt,"%V")) {
		msg = va_arg(args, KrkValue);
	} else if (!krk_pushStringBuilderFormatV(&sb, fmt, args)) {
		return NONE_VAL();
	}

	va_end(args);
	krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;

	/* Allocate an exception object of the requested type. */
	KrkInstance * exceptionObject = krk_newInstance(type);
	krk_push(OBJECT_VAL(exceptionObject));
	krk_attachNamedValue(&exceptionObject->fields, "arg", msg == KWARGS_VAL(0) ? finishStringBuilder(&sb) : msg);
	krk_attachNamedValue(&exceptionObject->fields, "__cause__", NONE_VAL());
	krk_attachNamedValue(&exceptionObject->fields, "__context__", NONE_VAL());
	krk_pop();

	/* Set the current exception to be picked up by handleException */
	krk_currentThread.currentException = OBJECT_VAL(exceptionObject);
	attachTraceback();
	return NONE_VAL();
}
