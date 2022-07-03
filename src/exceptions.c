/**
 * @file exceptions.c
 * @brief Definitions and native method bindings for error types.
 */
#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

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

#define IS_Exception(o)    (likely(krk_isInstanceOf(o,vm.exceptions->baseException)))
#define AS_Exception(o)    (AS_INSTANCE(o))
#define IS_KeyError(o)     (likely(krk_isInstanceOf(o,vm.exceptions->keyError)))
#define AS_KeyError(o)     (AS_INSTANCE(o))
#define IS_SyntaxError(o)  (likely(krk_isInstanceOf(o,vm.exceptions->syntaxError)))
#define AS_SyntaxError(o)  (AS_INSTANCE(o))
#define CURRENT_CTYPE KrkInstance*
#define CURRENT_NAME  self

/**
 * @brief Initialize an exception object.
 *
 * Native binding for Exception.__init__
 *
 * @param arg Optional string to attach to the exception object.
 */
KRK_METHOD(Exception,__init__,{
	if (argc > 1) {
		krk_attachNamedValue(&self->fields, "arg", argv[1]);
	}
	krk_attachNamedValue(&self->fields, "__cause__", NONE_VAL());
	krk_attachNamedValue(&self->fields, "__context__", NONE_VAL());
	return argv[0];
})

/**
 * @brief Create a string representation of an Exception.
 *
 * Native binding for @c Exception.__repr__
 *
 * Generates a string representation of the form @c "Exception(arg)" .
 */
KRK_METHOD(Exception,__repr__,{
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
})

/**
 * @brief Obtain a descriptive string from an exception.
 *
 * Native binding for @c Exception.__str__
 *
 * For most exceptions, this is the 'arg' value attached at initialization
 * and is printed during a traceback after the name of the exception type.
 */
KRK_METHOD(Exception,__str__,{
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
})

KRK_METHOD(KeyError,__str__,{
	if (!IS_INSTANCE(argv[0])) return NONE_VAL(); /* uh oh */
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue arg;
	if (krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg)) {
		KrkClass * type = krk_getType(arg);
		if (type->_reprer) {
			krk_push(arg);
			return krk_callDirect(krk_getType(arg)->_reprer, 1);
		}
	}
	return FUNC_NAME(Exception,__str__)(argc,argv,hasKw);
})

/**
 * @brief Generate printable text for a syntax error.
 *
 * Native binding for @c SyntaxError.__str__
 *
 * Syntax errors are handled specially by the traceback generator so that they
 * can print the original source line containing the erroneous input, so instead
 * of printing {Exception.__class__.__name__}: {str(Exception)} we just print
 * {str(Exception)} for syntax errors and they handle the rest. This is a bit
 * of a kludge, but it works for now.
 */
KRK_METHOD(SyntaxError,__str__,{
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
})


/**
 * @brief Bind native methods and classes for exceptions.
 *
 * Called on VM initialization to create the base classes for exception types
 * and bind the native methods for exception objects.
 */
_noexport
void _createAndBind_exceptions(void) {
	/* Add exception classes */
	ADD_EXCEPTION_CLASS(vm.exceptions->baseException, Exception, vm.baseClasses->objectClass);
	BIND_METHOD(Exception,__init__);
	BIND_METHOD(Exception,__repr__);
	BIND_METHOD(Exception,__str__);
	krk_finalizeClass(Exception);

	ADD_EXCEPTION_CLASS(vm.exceptions->typeError, TypeError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->argumentError, ArgumentError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->indexError, IndexError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->keyError, KeyError, Exception);
	BIND_METHOD(KeyError,__str__);
	krk_finalizeClass(KeyError);

	ADD_EXCEPTION_CLASS(vm.exceptions->attributeError, AttributeError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->nameError, NameError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->importError, ImportError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->ioError, IOError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->valueError, ValueError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->keyboardInterrupt, KeyboardInterrupt, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->zeroDivisionError, ZeroDivisionError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->notImplementedError, NotImplementedError, Exception);
	ADD_EXCEPTION_CLASS(vm.exceptions->assertionError, AssertionError, vm.exceptions->baseException);

	ADD_EXCEPTION_CLASS(vm.exceptions->syntaxError, SyntaxError, vm.exceptions->baseException);
	BIND_METHOD(SyntaxError,__str__);
	krk_finalizeClass(SyntaxError);
}

