#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

static KrkValue _type_init(int argc, KrkValue argv[], int hasKw) {
	if (argc != 2) return krk_runtimeError(vm.exceptions->argumentError, "type() takes 1 argument");
	return OBJECT_VAL(krk_getType(argv[1]));
}

/* Class.__base__ */
static KrkValue krk_baseOfClass(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected class");
	return AS_CLASS(argv[0])->base ? OBJECT_VAL(AS_CLASS(argv[0])->base) : NONE_VAL();
}

/* Class.__name */
static KrkValue krk_nameOfClass(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected class");
	return AS_CLASS(argv[0])->name ? OBJECT_VAL(AS_CLASS(argv[0])->name) : NONE_VAL();
}

/* Class.__file__ */
static KrkValue krk_fileOfClass(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected class");
	return AS_CLASS(argv[0])->filename ? OBJECT_VAL(AS_CLASS(argv[0])->filename) : NONE_VAL();
}

/* Class.__doc__ */
static KrkValue krk_docOfClass(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected class");
	return AS_CLASS(argv[0])->docstring ? OBJECT_VAL(AS_CLASS(argv[0])->docstring) : NONE_VAL();
}

/* Class.__str__() (and Class.__repr__) */
static KrkValue _class_to_str(int argc, KrkValue argv[], int hasKw) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected class");

	/* Determine if this class has a module */
	KrkValue module = NONE_VAL();
	krk_tableGet(&AS_CLASS(argv[0])->methods, OBJECT_VAL(S("__module__")), &module);

	KrkValue qualname = NONE_VAL();
	krk_tableGet(&AS_CLASS(argv[0])->methods, OBJECT_VAL(S("__qualname__")), &qualname);
	KrkString * name = IS_STRING(qualname) ? AS_STRING(qualname) : AS_CLASS(argv[0])->name;

	int includeModule = !(IS_NONE(module) || (IS_STRING(module) && AS_STRING(module) == S("__builtins__")));

	size_t allocSize = sizeof("<class ''>") + name->length;
	if (IS_STRING(module)) allocSize += AS_STRING(module)->length + 1;
	char * tmp = malloc(allocSize);
	size_t l = snprintf(tmp, allocSize, "<class '%s%s%s'>",
		includeModule ? AS_CSTRING(module) : "",
		includeModule ? "." : "",
		name->chars);
	KrkString * out = krk_copyString(tmp,l);
	free(tmp);
	return OBJECT_VAL(out);
}

_noexport
void _createAndBind_type(void) {
	ADD_BASE_CLASS(vm.baseClasses->typeClass, "type", vm.baseClasses->objectClass);
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__base__", krk_baseOfClass)->isDynamicProperty = 1;
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__file__", krk_fileOfClass)->isDynamicProperty = 1;
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__doc__", krk_docOfClass)->isDynamicProperty = 1;
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__name__", krk_nameOfClass)->isDynamicProperty = 1;
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__init__", _type_init);
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__str__", _class_to_str);
	krk_defineNative(&vm.baseClasses->typeClass->methods, "__repr__", _class_to_str);
	krk_finalizeClass(vm.baseClasses->typeClass);
	KRK_DOC(vm.baseClasses->typeClass, "Obtain the object representation of the class of an object.");
}
