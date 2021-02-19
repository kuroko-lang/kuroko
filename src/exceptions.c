/**
 * @file exceptions.c
 * @brief Definitions and native method bindings for error types.
 */
#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

/**
 * @def ADD_EXCEPTION_CLASS(obj,name,baseClass)
 *
 * Convenience macro for creating exception types.
 */
#define ADD_EXCEPTION_CLASS(obj,name,baseClass) do { \
	obj = krk_newClass(S(name), baseClass); \
	krk_attachNamedObject(&vm.builtins->fields, name, (KrkObj*)obj); \
	krk_finalizeClass(obj); \
} while (0)

/**
 * @brief Initialize an exception object.
 *
 * Native binding for Exception.__init__
 *
 * @param arg Optional string to attach to the exception object.
 */
static KrkValue krk_initException(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	krk_attachNamedValue(&self->fields, "arg", argc > 1 ? argv[1] : NONE_VAL());
	return argv[0];
}

/**
 * @brief Create a string representation of an Exception.
 *
 * Native binding for @c Exception.__repr__
 *
 * Generates a string representation of the form @c "Exception(arg)" .
 */
static KrkValue _exception_repr(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue arg;
	struct StringBuilder sb = {0};

	pushStringBuilderStr(&sb, self->_class->name->chars, self->_class->name->length);
	pushStringBuilder(&sb, '(');

	if (krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg)) {
		/* repr it */
		krk_push(arg);
		KrkValue repred = krk_callSimple(OBJECT_VAL(krk_getType(arg)->_reprer), 1, 0);
		pushStringBuilderStr(&sb, AS_CSTRING(repred), AS_STRING(repred)->length);
	}

	pushStringBuilder(&sb, ')');

	return finishStringBuilder(&sb);
}

/**
 * @brief Obtain a descriptive string from an exception.
 *
 * Native binding for @c Exception.__str__
 *
 * For most exceptions, this is the 'arg' value attached at initialization
 * and is printed during a traceback after the name of the exception type.
 */
static KrkValue _exception_str(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue arg;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg) || IS_NONE(arg)) {
		return NONE_VAL();
	} else if (!IS_STRING(arg)) {
		krk_push(arg);
		return krk_callSimple(OBJECT_VAL(krk_getType(arg)->_tostr), 1, 0);
	} else {
		return arg;
	}
}

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
static KrkValue _syntaxerror_str(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	/* .arg */
	KrkValue file, line, lineno, colno, arg, func;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("file")), &file) || !IS_STRING(file)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("line")), &line) || !IS_STRING(line)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("lineno")), &lineno) || !IS_INTEGER(lineno)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("colno")), &colno) || !IS_INTEGER(colno)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg) || !IS_STRING(arg)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("func")), &func)) goto _badSyntaxError;

	if (AS_INTEGER(colno) <= 0) colno = INTEGER_VAL(1);

	krk_push(OBJECT_VAL(S("  File \"{}\", line {}{}\n    {}\n    {}^\n{}: {}")));
	char * tmp = malloc(AS_INTEGER(colno));
	memset(tmp,' ',AS_INTEGER(colno));
	tmp[AS_INTEGER(colno)-1] = '\0';
	krk_push(OBJECT_VAL(krk_takeString(tmp,AS_INTEGER(colno)-1)));
	krk_push(OBJECT_VAL(self->_class->name));
	if (IS_STRING(func)) {
		krk_push(OBJECT_VAL(S(" in ")));
		krk_push(func);
		krk_addObjects();
	} else {
		krk_push(OBJECT_VAL(S("")));
	}
	KrkValue formattedString = krk_string_format(8,
		(KrkValue[]){krk_peek(3), file, lineno, krk_peek(0), line, krk_peek(2), krk_peek(1), arg}, 0);
	krk_pop(); /* instr */
	krk_pop(); /* class */
	krk_pop(); /* spaces */
	krk_pop(); /* format string */

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
	ADD_EXCEPTION_CLASS(vm.exceptions->baseException, "Exception", vm.baseClasses->objectClass);
	/* base exception class gets an init that takes an optional string */
	krk_defineNative(&vm.exceptions->baseException->methods, ".__init__", krk_initException);
	krk_defineNative(&vm.exceptions->baseException->methods, ".__repr__", _exception_repr);
	krk_defineNative(&vm.exceptions->baseException->methods, ".__str__", _exception_str);
	krk_finalizeClass(vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->typeError, "TypeError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->argumentError, "ArgumentError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->indexError, "IndexError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->keyError, "KeyError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->attributeError, "AttributeError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->nameError, "NameError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->importError, "ImportError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->ioError, "IOError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->valueError, "ValueError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->keyboardInterrupt, "KeyboardInterrupt", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->zeroDivisionError, "ZeroDivisionError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->notImplementedError, "NotImplementedError", vm.exceptions->baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions->syntaxError, "SyntaxError", vm.exceptions->baseException);
	krk_defineNative(&vm.exceptions->syntaxError->methods, ".__str__", _syntaxerror_str);
	krk_finalizeClass(vm.exceptions->syntaxError);
}

