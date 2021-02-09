#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define ADD_EXCEPTION_CLASS(obj, name, baseClass) do { \
	obj = krk_newClass(S(name), baseClass); \
	krk_attachNamedObject(&vm.builtins->fields, name, (KrkObj*)obj); \
	krk_finalizeClass(obj); \
} while (0)

/**
 * Exception.__init__(arg)
 */
static KrkValue krk_initException(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	if (argc > 0) {
		krk_attachNamedValue(&self->fields, "arg", argv[1]);
	} else {
		krk_attachNamedValue(&self->fields, "arg", NONE_VAL());
	}

	return argv[0];
}

static KrkValue _exception_repr(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	/* .arg */
	KrkValue arg;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg) || IS_NONE(arg)) {
		return OBJECT_VAL(self->_class->name);
	} else {
		krk_push(OBJECT_VAL(self->_class->name));
		krk_push(OBJECT_VAL(S(": ")));
		krk_addObjects();
		krk_push(arg);
		krk_addObjects();
		return krk_pop();
	}
}

static KrkValue _syntaxerror_repr(int argc, KrkValue argv[]) {
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

_noexport
void _createAndBind_exceptions(void) {
	/* Add exception classes */
	ADD_EXCEPTION_CLASS(vm.exceptions->baseException, "Exception", vm.baseClasses->objectClass);
	/* base exception class gets an init that takes an optional string */
	krk_defineNative(&vm.exceptions->baseException->methods, ".__init__", krk_initException);
	krk_defineNative(&vm.exceptions->baseException->methods, ".__repr__", _exception_repr);
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
	krk_defineNative(&vm.exceptions->syntaxError->methods, ".__repr__", _syntaxerror_repr);
	krk_finalizeClass(vm.exceptions->syntaxError);
}

