#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include "vm.h"
#include "debug.h"
#include "memory.h"
#include "compiler.h"
#include "object.h"
#include "table.h"

#define S(c) (krk_copyString(c,sizeof(c)-1))

/* Why is this static... why do we do this to ourselves... */
KrkVM vm;

static KrkValue run();
static int call(KrkClosure * closure, int argCount);
static KrkValue krk_isinstance(int argc, KrkValue argv[]);

extern const char _builtins_src[];

static void resetStack() {
	vm.stackTop = vm.stack;
	vm.frameCount = 0;
	vm.openUpvalues = NULL;
	vm.flags &= ~KRK_HAS_EXCEPTION;
	vm.currentException = NONE_VAL();
}

static void dumpTraceback() {
	fprintf(stderr, "Traceback, most recent first, %d call frames:\n", (int)vm.frameCount);
	for (size_t i = 0; i <= vm.frameCount - 1; i++) {
		CallFrame * frame = &vm.frames[i];
		KrkFunction * function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		fprintf(stderr, "  File \"%s\", line %d, in %s\n",
			(function->chunk.filename ? function->chunk.filename->chars : "?"),
			(int)function->chunk.lines[instruction],
			(function->name ? function->name->chars : "(unnamed)"));
	}

	if (IS_STRING(vm.currentException)) {
		/* Make sure strings are printed without quotes */
		fprintf(stderr, "%s", AS_CSTRING(vm.currentException));
	} else if (AS_BOOLEAN(krk_isinstance(2, (KrkValue[]){vm.currentException, OBJECT_VAL(vm.exceptions.baseException)}))) {
		/* ErrorClass: arg... */
		fprintf(stderr, "%s: ", AS_INSTANCE(vm.currentException)->_class->name->chars);
		KrkValue exceptionArg;
		krk_tableGet(&AS_INSTANCE(vm.currentException)->fields, OBJECT_VAL(S("arg")), &exceptionArg);
		if (IS_STRING(exceptionArg)) {
			/* Make sure strings are printed without quotes */
			fprintf(stderr, "%s", AS_CSTRING(exceptionArg));
		} else {
			krk_printValueSafe(stderr, exceptionArg);
		}
	} else {
		/* Whatever, just print it. */
		krk_printValueSafe(stderr, vm.currentException);
	}

	fprintf(stderr, "\n");
}

void krk_runtimeError(KrkClass * type, const char * fmt, ...) {
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	size_t len = vsnprintf(buf, 1024, fmt, args);
	va_end(args);
	vm.flags |= KRK_HAS_EXCEPTION;

	/* Try to allocate an instance of __builtins__. */
	KrkInstance * exceptionObject = krk_newInstance(type);
	krk_push(OBJECT_VAL(exceptionObject));
	KrkString * strArg = S("arg");
	krk_push(OBJECT_VAL(strArg));
	KrkString * strVal = krk_copyString(buf, len);
	krk_push(OBJECT_VAL(strVal));
	krk_tableSet(&exceptionObject->fields, OBJECT_VAL(strArg), OBJECT_VAL(strVal));
	krk_pop();
	krk_pop();
	krk_pop();

	vm.currentException = OBJECT_VAL(exceptionObject);
}

inline void krk_push(KrkValue value) {
	if ((size_t)(vm.stackTop - vm.stack) + 1 > vm.stackSize) {
		size_t old = vm.stackSize;
		size_t old_offset = vm.stackTop - vm.stack;
		vm.stackSize = GROW_CAPACITY(old);
		vm.stack = GROW_ARRAY(KrkValue, vm.stack, old, vm.stackSize);
		vm.stackTop = vm.stack + old_offset;
	}
	*vm.stackTop = value;
	vm.stackTop++;
}

KrkValue krk_pop() {
	vm.stackTop--;
	if (vm.stackTop < vm.stack) {
		fprintf(stderr, "Fatal error: stack underflow detected in VM, issuing breakpoint.\n");
		__asm__ ("int $3");
		return NONE_VAL();
	}
	return *vm.stackTop;
}

KrkValue krk_peek(int distance) {
	return vm.stackTop[-1 - distance];
}

void krk_swap(int distance) {
	KrkValue top = vm.stackTop[-1];
	vm.stackTop[-1] = vm.stackTop[-1 - distance];
	vm.stackTop[-1 - distance] = top;
}

void krk_defineNative(KrkTable * table, const char * name, NativeFn function) {
	int functionType = 0;
	if (*name == '.') {
		name++;
		functionType = 1;
	}
	if (*name == ':') {
		name++;
		functionType = 2;
	}
	KrkNative * func = krk_newNative(function, name, functionType);
	krk_push(OBJECT_VAL(func));
	krk_push(OBJECT_VAL(krk_copyString(name, (int)strlen(name))));
	krk_tableSet(table, krk_peek(0), krk_peek(1));
	krk_pop();
	krk_pop();
}

static KrkValue _dict_init(int argc, KrkValue argv[]) {
	KrkClass * dict = krk_newClass(NULL);
	krk_push(OBJECT_VAL(dict));
	krk_tableSet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], OBJECT_VAL(dict));
	krk_pop();
	return argv[0];
}

static KrkValue _dict_get(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	KrkValue out;
	if (!krk_tableGet(&AS_CLASS(_dict_internal)->methods, argv[1], &out)) {
		krk_runtimeError(vm.exceptions.keyError, "key error");
	}
	return out;
}

static KrkValue _dict_set(int argc, KrkValue argv[]) {
	if (argc < 3) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	krk_tableSet(&AS_CLASS(_dict_internal)->methods, argv[1], argv[2]);
	return NONE_VAL();
}

static KrkValue _dict_len(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	return INTEGER_VAL(AS_CLASS(_dict_internal)->methods.count);
}

static KrkValue _dict_capacity(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	return INTEGER_VAL(AS_CLASS(_dict_internal)->methods.capacity);
}


static KrkValue _dict_key_at_index(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	if (!IS_INTEGER(argv[1])) {
		krk_runtimeError(vm.exceptions.typeError, "expected integer index but got %s", krk_typeName(argv[1]));
		return NONE_VAL();
	}
	int i = AS_INTEGER(argv[1]);
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	if (i < 0 || i > (int)AS_CLASS(_dict_internal)->methods.capacity) {
		krk_runtimeError(vm.exceptions.indexError, "hash table index is out of range: %d", i);
		return NONE_VAL();
	}
	KrkTableEntry entry = AS_CLASS(_dict_internal)->methods.entries[i];
	return entry.key;
}

static KrkValue _list_init(int argc, KrkValue argv[]) {
	KrkFunction * list = krk_newFunction(NULL);
	krk_push(OBJECT_VAL(list));
	krk_tableSet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_LIST_INT], OBJECT_VAL(list));
	krk_pop();
	return argv[0];
}

static KrkValue _list_get(int argc, KrkValue argv[]) {
	if (argc < 2 || !IS_INTEGER(argv[1])) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_LIST_INT], &_list_internal);
	int index = AS_INTEGER(argv[1]);
	if (index < 0 || index >= (int)AS_FUNCTION(_list_internal)->chunk.constants.count) {
		krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
		return NONE_VAL();
	}
	return AS_FUNCTION(_list_internal)->chunk.constants.values[index];
}

static KrkValue _list_set(int argc, KrkValue argv[]) {
	if (argc < 3 || !IS_INTEGER(argv[1])) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_LIST_INT], &_list_internal);
	int index = AS_INTEGER(argv[1]);
	if (index < 0 || index >= (int)AS_FUNCTION(_list_internal)->chunk.constants.count) {
		krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
		return NONE_VAL();
	}
	AS_FUNCTION(_list_internal)->chunk.constants.values[index] = argv[2];
	return NONE_VAL();
}

static KrkValue _list_append(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_LIST_INT], &_list_internal);
	krk_writeValueArray(&AS_FUNCTION(_list_internal)->chunk.constants, argv[1]);
	return NONE_VAL();
}

static KrkValue _list_len(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_LIST_INT], &_list_internal);
	return INTEGER_VAL(AS_FUNCTION(_list_internal)->chunk.constants.count);
}

KrkValue krk_runNext(void) {
	size_t oldExit = vm.exitOnFrame;
	vm.exitOnFrame = vm.frameCount - 1;
	KrkValue result = run();
	vm.exitOnFrame = oldExit;
	return result;
}

KrkInstance * krk_dictCreate(KrkValue * outClass) {
	krk_tableGet(&vm.globals,OBJECT_VAL(S("dict")), outClass);
	KrkInstance * outDict = krk_newInstance(AS_CLASS(*outClass));
	krk_push(OBJECT_VAL(outDict));
	KrkValue tmp;
	if (krk_tableGet(&AS_CLASS(*outClass)->methods, vm.specialMethodNames[METHOD_INIT], &tmp)) {
		call(AS_CLOSURE(tmp), 0);
		krk_runNext();
	}
	return outDict;
}

void krk_dictSet(KrkValue dictClass, KrkInstance * dict, KrkValue key, KrkValue value) {
	krk_push(OBJECT_VAL(dict));
	krk_push(key);
	krk_push(value);
	KrkValue tmp;
	if (krk_tableGet(&AS_CLASS(dictClass)->methods, vm.specialMethodNames[METHOD_SET], &tmp)) {
		call(AS_CLOSURE(tmp), 2);
		krk_runNext();
	}
}

KrkValue krk_dictGet(KrkValue dictClass, KrkInstance * dict, KrkValue key) {
	krk_push(OBJECT_VAL(dict));
	krk_push(key);
	KrkValue tmp;
	if (krk_tableGet(&AS_CLASS(dictClass)->methods, vm.specialMethodNames[METHOD_GET], &tmp)) {
		call(AS_CLOSURE(tmp), 2);
		krk_runNext();
	}
	return krk_pop();
}

static KrkValue krk_list_of(int argc, KrkValue argv[]) {
	KrkValue Class;
	krk_tableGet(&vm.globals,OBJECT_VAL(S("list")), &Class);
	KrkInstance * outList = krk_newInstance(AS_CLASS(Class));
	krk_push(OBJECT_VAL(outList));
	KrkFunction * listContents = krk_newFunction(NULL);
	krk_push(OBJECT_VAL(listContents));
	krk_tableSet(&outList->fields, vm.specialMethodNames[METHOD_LIST_INT], OBJECT_VAL(listContents));
	for (int ind = 0; ind < argc; ++ind) {
		krk_writeValueArray(&listContents->chunk.constants, argv[ind]);
	}
	KrkValue out = OBJECT_VAL(outList);
	krk_pop();
	krk_pop();
	return out;
}

static KrkValue krk_dict_of(int argc, KrkValue argv[]) {
	if (argc % 2 != 0) {
		krk_runtimeError(vm.exceptions.argumentError, "Expected even number of arguments to dictOf");
		return NONE_VAL();
	}
	KrkValue Class;
	krk_tableGet(&vm.globals,OBJECT_VAL(S("dict")), &Class);
	KrkInstance * outDict = krk_newInstance(AS_CLASS(Class));
	krk_push(OBJECT_VAL(outDict));
	KrkClass * dictContents = krk_newClass(NULL);
	krk_push(OBJECT_VAL(dictContents));
	krk_tableSet(&outDict->fields, vm.specialMethodNames[METHOD_DICT_INT], OBJECT_VAL(dictContents));
	for (int ind = 0; ind < argc; ind += 2) {
		krk_tableSet(&dictContents->methods, argv[ind], argv[ind+1]);
	}
	KrkValue out = OBJECT_VAL(outDict);
	krk_pop();
	krk_pop();
	return out;
}

#ifndef NO_SYSTEM_BINDS
static KrkValue krk_uname(int argc, KrkValue argv[]) {
	struct utsname buf;
	if (uname(&buf) < 0) return NONE_VAL();

	KRK_PAUSE_GC();

	KrkValue dictClass;
	KrkInstance * dict = krk_dictCreate(&dictClass);
	krk_dictSet(dictClass, dict, OBJECT_VAL(S("sysname")), OBJECT_VAL(krk_copyString(buf.sysname,strlen(buf.sysname))));
	krk_dictSet(dictClass, dict, OBJECT_VAL(S("nodename")), OBJECT_VAL(krk_copyString(buf.nodename,strlen(buf.nodename))));
	krk_dictSet(dictClass, dict, OBJECT_VAL(S("release")), OBJECT_VAL(krk_copyString(buf.release,strlen(buf.release))));
	krk_dictSet(dictClass, dict, OBJECT_VAL(S("version")), OBJECT_VAL(krk_copyString(buf.version,strlen(buf.version))));
	krk_dictSet(dictClass, dict, OBJECT_VAL(S("machine")), OBJECT_VAL(krk_copyString(buf.machine,strlen(buf.machine))));

	KrkValue result = OBJECT_VAL(dict);

	KRK_RESUME_GC();

	return result;
}

static KrkValue krk_sleep(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "sleep: expect at least one argument.");
		return BOOLEAN_VAL(0);
	}

	/* Accept an integer or a floating point. Anything else, just ignore. */
	unsigned int usecs = (IS_INTEGER(argv[0]) ? AS_INTEGER(argv[0]) :
	                      (IS_FLOATING(argv[0]) ? AS_FLOATING(argv[0]) : 0)) *
	                      1000000;

	usleep(usecs);

	return BOOLEAN_VAL(1);
}
#endif

static KrkValue krk_set_tracing(int argc, KrkValue argv[]) {
	if (argc < 1) return NONE_VAL();
#ifdef DEBUG
	else if (!strcmp(AS_CSTRING(argv[0]),"tracing=1")) vm.flags |= KRK_ENABLE_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"debugging=1")) vm.flags |= KRK_ENABLE_DEBUGGING;
	else if (!strcmp(AS_CSTRING(argv[0]),"scantracing=1")) vm.flags |= KRK_ENABLE_SCAN_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"stressgc=1")) vm.flags |= KRK_ENABLE_STRESS_GC;
	else if (!strcmp(AS_CSTRING(argv[0]),"tracing=0")) vm.flags &= ~KRK_ENABLE_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"debugging=0")) vm.flags &= ~KRK_ENABLE_DEBUGGING;
	else if (!strcmp(AS_CSTRING(argv[0]),"scantracing=0")) vm.flags &= ~KRK_ENABLE_SCAN_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"stressgc=0")) vm.flags &= ~KRK_ENABLE_STRESS_GC;
	return BOOLEAN_VAL(1);
#else
	krk_runtimeError(vm.exceptions.typeError,"Debugging is not enabled in this build.");
	return NONE_VAL();
#endif
}

static KrkValue krk_dirObject(int argc, KrkValue argv[]) {
	if (argc != 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments or bad type, got %d\n", argc);
		return NONE_VAL();
	}

	/* Create a new list instance */
	KrkValue Class;
	krk_tableGet(&vm.globals,OBJECT_VAL(S("list")), &Class);
	KrkInstance * outList = krk_newInstance(AS_CLASS(Class));
	krk_push(OBJECT_VAL(outList));
	KrkFunction * listContents = krk_newFunction(NULL);
	krk_push(OBJECT_VAL(listContents));
	krk_tableSet(&outList->fields, OBJECT_VAL(S("_list")), OBJECT_VAL(listContents));


	if (IS_INSTANCE(argv[0])) {
		/* Obtain self-reference */
		KrkInstance * self = AS_INSTANCE(argv[0]);

		/* First add each method of the class */
		for (size_t i = 0; i < self->_class->methods.capacity; ++i) {
			if (self->_class->methods.entries[i].key.type != VAL_NONE) {
				krk_writeValueArray(&listContents->chunk.constants,
					self->_class->methods.entries[i].key);
			}
		}

		/* Then add each field of the instance */
		for (size_t i = 0; i < self->fields.capacity; ++i) {
			if (self->fields.entries[i].key.type != VAL_NONE) {
				krk_writeValueArray(&listContents->chunk.constants,
					self->fields.entries[i].key);
			}
		}
	} else {
		KrkClass * type = AS_CLASS(krk_typeOf(1, (KrkValue[]){argv[0]}));

		for (size_t i = 0; i < type->methods.capacity; ++i) {
			if (type->methods.entries[i].key.type != VAL_NONE) {
				krk_writeValueArray(&listContents->chunk.constants,
					type->methods.entries[i].key);
			}
		}
	}

	/* Prepare output value */
	KrkValue out = OBJECT_VAL(outList);
	krk_pop();
	krk_pop();
	return out;
}

KrkValue krk_typeOf(int argc, KrkValue argv[]) {
	switch (argv[0].type) {
		case VAL_INTEGER:
			return OBJECT_VAL(vm.baseClasses.intClass);
		case VAL_FLOATING:
			return OBJECT_VAL(vm.baseClasses.floatClass);
		case VAL_BOOLEAN:
			return OBJECT_VAL(vm.baseClasses.boolClass);
		case VAL_NONE:
			return OBJECT_VAL(vm.baseClasses.noneTypeClass);
		case VAL_OBJECT:
			switch (AS_OBJECT(argv[0])->type) {
				case OBJ_CLASS:
					return OBJECT_VAL(vm.baseClasses.typeClass);
				case OBJ_NATIVE:
				case OBJ_FUNCTION:
				case OBJ_CLOSURE:
					return OBJECT_VAL(vm.baseClasses.functionClass);
				case OBJ_BOUND_METHOD:
					return OBJECT_VAL(vm.baseClasses.methodClass);
				case OBJ_STRING:
					return OBJECT_VAL(vm.baseClasses.strClass);
				case OBJ_INSTANCE:
					return OBJECT_VAL(AS_INSTANCE(argv[0])->_class);
				default:
					return OBJECT_VAL(vm.objectClass);
			} break;
		default:
			return OBJECT_VAL(vm.objectClass);
	}
}

static KrkValue krk_baseOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->base ? OBJECT_VAL(AS_CLASS(argv[0])->base) : NONE_VAL();
}

static KrkValue krk_nameOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->name ? OBJECT_VAL(AS_CLASS(argv[0])->name) : NONE_VAL();
}

static KrkValue krk_fileOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->filename ? OBJECT_VAL(AS_CLASS(argv[0])->filename) : NONE_VAL();
}

static KrkValue krk_docOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->docstring ? OBJECT_VAL(AS_CLASS(argv[0])->docstring) : NONE_VAL();
}

static KrkValue _class_to_str(int argc, KrkValue argv[]) {
	char * tmp = malloc(sizeof("<type ''>") + AS_CLASS(argv[0])->name->length);
	size_t l = sprintf(tmp, "<type '%s'>", AS_CLASS(argv[0])->name->chars);
	KrkString * out = krk_copyString(tmp,l);
	free(tmp);
	return OBJECT_VAL(out);
}

static KrkValue krk_isinstance(int argc, KrkValue argv[]) {
	if (argc != 2) {
		krk_runtimeError(vm.exceptions.argumentError, "isinstance expects 2 arguments, got %d", argc);
		return NONE_VAL();
	}

	if (!IS_CLASS(argv[1])) {
		krk_runtimeError(vm.exceptions.typeError, "isinstance() arg 2 must be class");
		return NONE_VAL();
	}

	KrkValue obj_type = krk_typeOf(1, (KrkValue[]){argv[0]});
	KrkClass * obj_class = AS_CLASS(obj_type);

	KrkClass * _class = AS_CLASS(argv[1]);

	while (obj_class) {
		if (obj_class == _class) return BOOLEAN_VAL(1);
		obj_class = obj_class->base;
	}

	return BOOLEAN_VAL(0);
}

static int call(KrkClosure * closure, int argCount) {
	int minArgs = closure->function->requiredArgs;
	int maxArgs = minArgs + closure->function->defaultArgs;
	if (argCount < minArgs || argCount > maxArgs) {
		krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)",
		closure->function->name ? closure->function->name->chars : "<unnamed function>",
		(minArgs == maxArgs) ? "exactly" : (argCount < minArgs ? "at least" : "at most"),
		(argCount < minArgs) ? minArgs : maxArgs,
		((argCount < minArgs) ? minArgs : maxArgs) == 1 ? "" : "s",
		argCount);
		return 0;
	}
	while (argCount < (closure->function->requiredArgs + closure->function->defaultArgs)) {
		krk_push(NONE_VAL());
		argCount++;
	}
	if (vm.frameCount == FRAMES_MAX) {
		krk_runtimeError(vm.exceptions.baseException, "Too many call frames.");
		return 0;
	}
	CallFrame * frame = &vm.frames[vm.frameCount++];
	frame->isInlined = 0;
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = (vm.stackTop - argCount - 1) - vm.stack;
	return 1;
}

int krk_callValue(KrkValue callee, int argCount) {
	if (IS_OBJECT(callee)) {
		switch (OBJECT_TYPE(callee)) {
			case OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount);
			case OBJ_NATIVE: {
				NativeFn native = AS_NATIVE(callee);
				int extraArgs = (((KrkNative*)AS_OBJECT(callee))->isMethod == 1);
				KrkValue * stackCopy = malloc((argCount + extraArgs) * sizeof(KrkValue));
				memcpy(stackCopy, vm.stackTop - argCount - extraArgs, (argCount + extraArgs) * sizeof(KrkValue));
				KrkValue result = native(argCount + extraArgs, stackCopy);
				free(stackCopy);
				if (vm.stackTop == vm.stack) {
					/* Runtime error returned from native method */
					return 0;
				}
				vm.stackTop -= argCount + 1;
				krk_push(result);
				return 2;
			}
			case OBJ_CLASS: {
				KrkClass * _class = AS_CLASS(callee);
				vm.stackTop[-argCount - 1] = OBJECT_VAL(krk_newInstance(_class));
				KrkValue initializer;
				if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_INIT], &initializer)) {
					return krk_callValue(initializer, argCount);
				} else if (argCount != 0) {
					krk_runtimeError(vm.exceptions.attributeError, "Class does not have an __init__ but arguments were passed to initializer: %d\n", argCount);
					return 0;
				}
				return 1;
			}
			case OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				vm.stackTop[-argCount - 1] = bound->receiver;
				return krk_callValue(OBJECT_VAL(bound->method), argCount);
			}
			default:
				break;
		}
	}
	krk_runtimeError(vm.exceptions.typeError, "Attempted to call non-callable type: %s", krk_typeName(callee));
	return 0;
}

int krk_bindMethod(KrkClass * _class, KrkString * name) {
	KrkValue method, out;
	if (!krk_tableGet(&_class->methods, OBJECT_VAL(name), &method)) return 0;
	if (IS_NATIVE(method) && ((KrkNative*)AS_OBJECT(method))->isMethod == 2) {
		out = AS_NATIVE(method)(1, (KrkValue[]){krk_peek(0)});
	} else {
		out = OBJECT_VAL(krk_newBoundMethod(krk_peek(0), AS_OBJECT(method)));
	}
	krk_pop();
	krk_push(out);
	return 1;
}

static KrkUpvalue * captureUpvalue(int index) {
	KrkUpvalue * prevUpvalue = NULL;
	KrkUpvalue * upvalue = vm.openUpvalues;
	while (upvalue != NULL && upvalue->location > index) {
		prevUpvalue = upvalue;
		upvalue = upvalue->next;
	}
	if (upvalue != NULL && upvalue->location == index) {
		return upvalue;
	}
	KrkUpvalue * createdUpvalue = krk_newUpvalue(index);
	createdUpvalue->next = upvalue;
	if (prevUpvalue == NULL) {
		vm.openUpvalues = createdUpvalue;
	} else {
		prevUpvalue->next = createdUpvalue;
	}
	return createdUpvalue;
}

#define UPVALUE_LOCATION(upvalue) (upvalue->location == -1 ? &upvalue->closed : &vm.stack[upvalue->location])

static void closeUpvalues(int last) {
	while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
		KrkUpvalue * upvalue = vm.openUpvalues;
		upvalue->closed = vm.stack[upvalue->location];
		upvalue->location = -1;
		vm.openUpvalues = upvalue->next;
	}
}

static void defineMethod(KrkString * name) {
	KrkValue method = krk_peek(0);
	KrkClass * _class = AS_CLASS(krk_peek(1));
	krk_tableSet(&_class->methods, OBJECT_VAL(name), method);
	krk_pop();
}

void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(OBJECT_VAL(obj));
	krk_tableSet(table, krk_peek(1), krk_peek(0));
	krk_pop();
	krk_pop();
}

void krk_attachNamedValue(KrkTable * table, const char name[], KrkValue obj) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(obj);
	krk_tableSet(table, krk_peek(1), krk_peek(0));
	krk_pop();
	krk_pop();
}

static KrkValue krk_initException(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	if (argc > 0) {
		krk_attachNamedValue(&self->fields, "arg", argv[1]);
	} else {
		krk_attachNamedValue(&self->fields, "arg", OBJECT_VAL(S("")));
	}

	return argv[0];
}

#define ADD_BASE_CLASS(obj, name, baseClass) do { \
	obj = krk_newClass(S(name)); \
	krk_attachNamedObject(&vm.builtins->fields, name, (KrkObj*)obj); \
	obj->base = baseClass; \
	krk_tableAddAll(&baseClass->methods, &obj->methods); \
} while (0)

#define ADD_EXCEPTION_CLASS(obj, name, baseClass) do { \
	obj = krk_newClass(S(name)); \
	krk_attachNamedObject(&vm.globals, name, (KrkObj*)obj); \
	obj->base = baseClass; \
	krk_tableAddAll(&baseClass->methods, &obj->methods); \
} while (0)

static KrkValue _noop(int argc, KrkValue argv[]) {
	return argv[0];
}

static KrkValue _floating_to_int(int argc, KrkValue argv[]) {
	return INTEGER_VAL((long)AS_FLOATING(argv[0]));
}

static KrkValue _int_to_floating(int argc, KrkValue argv[]) {
	return FLOATING_VAL((double)AS_INTEGER(argv[0]));
}

static KrkValue _int_to_char(int argc, KrkValue argv[]) {
	char tmp[2] = {AS_INTEGER(argv[0]), 0};
	return OBJECT_VAL(krk_copyString(tmp,1));
}

static KrkValue _string_length(int argc, KrkValue argv[]) {
	if (argc != 1) {
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0])) {
		return NONE_VAL();
	}
	return INTEGER_VAL(AS_STRING(argv[0])->length);
}

static KrkValue _strings_are_immutable(int argc, KrkValue argv[]) {
	krk_runtimeError(vm.exceptions.typeError, "Strings are not mutable.");
	return NONE_VAL();
}

static KrkValue _string_get_slice(int argc, KrkValue argv[]) {
	if (argc < 3) { /* 3 because first is us */
		krk_runtimeError(vm.exceptions.argumentError, "slice: expected 2 arguments, got %d", argc-1);
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0]) ||
		!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) ||
		!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) {
		krk_runtimeError(vm.exceptions.typeError, "slice: expected two integer arguments");
		return NONE_VAL();
	}
	/* bounds check */
	KrkString * me = AS_STRING(argv[0]);
	int start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	int end   = IS_NONE(argv[2]) ? (int)me->length : AS_INTEGER(argv[2]);
	if (start < 0) start = me->length + start;
	if (start < 0) start = 0;
	if (end < 0) end = me->length + end;
	if (start > (int)me->length) start = me->length;
	if (end > (int)me->length) end = me->length;
	if (end < start) end = start;
	int len = end - start;
	return OBJECT_VAL(krk_copyString(me->chars + start, len));
}

static KrkValue _string_to_int(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_STRING(argv[0])) return NONE_VAL();
	int base = 10;
	char * start = AS_CSTRING(argv[0]);

	/*  These special cases for hexadecimal, binary, octal values. */
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		base = 16;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
		base = 2;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'o' || start[1] == 'O')) {
		base = 8;
		start += 2;
	}
	long value = strtol(start, NULL, base);
	return INTEGER_VAL(value);
}

static KrkValue _string_to_float(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_STRING(argv[0])) return NONE_VAL();
	return FLOATING_VAL(strtod(AS_CSTRING(argv[0]),NULL));
}

static KrkValue _string_get(int argc, KrkValue argv[]) {
	if (argc != 2) {
		krk_runtimeError(vm.exceptions.argumentError, "Wrong number of arguments to String.__get__");
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0])) {
		krk_runtimeError(vm.exceptions.typeError, "First argument to __get__ must be String");
		return NONE_VAL();
	}
	if (!IS_INTEGER(argv[1])) {
		krk_runtimeError(vm.exceptions.typeError, "String can not indexed by %s", krk_typeName(argv[1]));
		return NONE_VAL();
	}
	int asInt = AS_INTEGER(argv[1]);
	if (asInt < 0) asInt += (int)AS_STRING(argv[0])->length;
	if (asInt < 0 || asInt >= (int)AS_STRING(argv[0])->length) {
		krk_runtimeError(vm.exceptions.indexError, "String index out of range: %d", asInt);
		return NONE_VAL();
	}
	return INTEGER_VAL(AS_CSTRING(argv[0])[asInt]);
}

static KrkValue _closure_get_doc(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return NONE_VAL();
	return AS_CLOSURE(argv[0])->function->docstring ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->docstring) : NONE_VAL();
}

static KrkValue _bound_get_doc(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_doc(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

static KrkValue nativeFunctionName(KrkValue func) {
	const char * string = ((KrkNative*)AS_OBJECT(func))->name;
	size_t len = strlen(string);
	return OBJECT_VAL(krk_copyString(string,len));
}

static KrkValue _closure_get_name(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return nativeFunctionName(argv[0]);
	return AS_CLOSURE(argv[0])->function->name ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->name) : OBJECT_VAL(S(""));
}

static KrkValue _bound_get_name(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_name(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

static KrkValue _closure_str(int argc, KrkValue argv[]) {
	KrkValue s = _closure_get_name(argc, argv);
	krk_push(s);

	size_t len = AS_STRING(s)->length + sizeof("<function >");
	char * tmp = malloc(len);
	sprintf(tmp, "<function %s>", AS_CSTRING(s));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);
	krk_pop();
	return s;
}

static KrkValue _bound_str(int argc, KrkValue argv[]) {
	KrkValue s = _bound_get_name(argc, argv);
	krk_push(s);

	size_t len = AS_STRING(s)->length + sizeof("<method >");
	char * tmp = malloc(len);
	sprintf(tmp, "<method %s>", AS_CSTRING(s));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);
	krk_pop();
	return s;
}

static KrkValue _closure_get_file(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return OBJECT_VAL(S("<builtin>"));
	return AS_CLOSURE(argv[0])->function->chunk.filename ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->chunk.filename) : OBJECT_VAL(S(""));
}

static KrkValue _bound_get_file(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_file(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

static KrkValue _strBase(int argc, KrkValue argv[]) {
	KrkClass * type = AS_CLASS(krk_typeOf(1,(KrkValue[]){argv[0]}));
	size_t len = sizeof("<instance of . at 0x1234567812345678>") + type->name->length;
	char * tmp = malloc(len);
	if (IS_OBJECT(argv[0])) {
		sprintf(tmp, "<instance of %s at %p>", type->name->chars, (void*)AS_OBJECT(argv[0]));
	} else {
		sprintf(tmp, "<instance of %s>", type->name->chars);
	}
	KrkValue out = OBJECT_VAL(krk_copyString(tmp, strlen(tmp)));
	free(tmp);
	return out;
}

static KrkValue _repr_str(int argc, KrkValue argv[]) {
	char * str = malloc(3 + AS_STRING(argv[0])->length * 2);
	char * tmp = str;
	*(tmp++) = '"';
	for (char * c = AS_CSTRING(argv[0]); *c; ++c) {
		switch (*c) {
			/* XXX: Other non-printables should probably be escaped as well. */
			case '\n': *(tmp++) = '\\'; *(tmp++) = 'n'; break;
			case '\r': *(tmp++) = '\\'; *(tmp++) = 'r'; break;
			case '\t': *(tmp++) = '\\'; *(tmp++) = 't'; break;
			case '"':  *(tmp++) = '\\'; *(tmp++) = '"'; break;
			case 27:   *(tmp++) = '\\'; *(tmp++) = '['; break;
			default:   *(tmp++) = *c; break;
		}
	}
	*(tmp++) = '"';
	*(tmp++) = '\0';
	KrkString * out = krk_copyString(str, tmp-str-1);
	free(str);
	return OBJECT_VAL(out);
}

static KrkValue _int_to_str(int argc, KrkValue argv[]) {
	char tmp[100];
	size_t l = sprintf(tmp, "%ld", (long)AS_INTEGER(argv[0]));
	return OBJECT_VAL(krk_copyString(tmp, l));
}

static KrkValue _float_to_str(int argc, KrkValue argv[]) {
	char tmp[100];
	size_t l = sprintf(tmp, "%g", AS_FLOATING(argv[0]));
	return OBJECT_VAL(krk_copyString(tmp, l));
}

static KrkValue _bool_to_str(int argc, KrkValue argv[]) {
	return OBJECT_VAL((AS_BOOLEAN(argv[0]) ? S("True") : S("False")));
}

static KrkValue _none_to_str(int argc, KrkValue argv[]) {
	return OBJECT_VAL(S("None"));
}

void krk_initVM(int flags) {
	vm.flags = flags;
	KRK_PAUSE_GC();

	resetStack();
	vm.objects = NULL;
	vm.bytesAllocated = 0;
	vm.nextGC = 1024 * 1024;
	vm.grayCount = 0;
	vm.grayCapacity = 0;
	vm.grayStack = NULL;
	krk_initTable(&vm.globals);
	krk_initTable(&vm.strings);
	memset(vm.specialMethodNames,0,sizeof(vm.specialMethodNames));

	vm.specialMethodNames[METHOD_INIT] = OBJECT_VAL(S("__init__"));
	vm.specialMethodNames[METHOD_STR]  = OBJECT_VAL(S("__str__"));
	vm.specialMethodNames[METHOD_REPR] = OBJECT_VAL(S("__repr__"));
	vm.specialMethodNames[METHOD_GET]  = OBJECT_VAL(S("__get__"));
	vm.specialMethodNames[METHOD_SET]  = OBJECT_VAL(S("__set__"));
	vm.specialMethodNames[METHOD_CLASS]= OBJECT_VAL(S("__class__"));
	vm.specialMethodNames[METHOD_NAME] = OBJECT_VAL(S("__name__"));
	vm.specialMethodNames[METHOD_FILE] = OBJECT_VAL(S("__file__"));
	vm.specialMethodNames[METHOD_INT]  = OBJECT_VAL(S("__int__"));
	vm.specialMethodNames[METHOD_CHR]  = OBJECT_VAL(S("__chr__"));
	vm.specialMethodNames[METHOD_FLOAT]= OBJECT_VAL(S("__float__"));
	vm.specialMethodNames[METHOD_LEN]  = OBJECT_VAL(S("__len__"));
	vm.specialMethodNames[METHOD_DOC]  = OBJECT_VAL(S("__doc__"));
	vm.specialMethodNames[METHOD_BASE] = OBJECT_VAL(S("__base__"));
	vm.specialMethodNames[METHOD_GETSLICE] = OBJECT_VAL(S("__getslice__"));
	vm.specialMethodNames[METHOD_LIST_INT] = OBJECT_VAL(S("__list"));
	vm.specialMethodNames[METHOD_DICT_INT] = OBJECT_VAL(S("__dict"));

	/* Create built-in class `object` */
	vm.objectClass = krk_newClass(S("object"));
	krk_attachNamedObject(&vm.globals, "object", (KrkObj*)vm.objectClass);
	krk_defineNative(&vm.objectClass->methods, ":__class__", krk_typeOf);
	krk_defineNative(&vm.objectClass->methods, ".__dir__", krk_dirObject);
	krk_defineNative(&vm.objectClass->methods, ".__str__", _strBase);
	krk_defineNative(&vm.objectClass->methods, ".__repr__", _strBase); /* Override if necesary */

	vm.builtins = krk_newInstance(vm.objectClass);
	krk_attachNamedObject(&vm.globals, "__builtins__", (KrkObj*)vm.builtins);

	/* Add exception classes */
	ADD_EXCEPTION_CLASS(vm.exceptions.baseException, "Exception", vm.objectClass);
	/* base exception class gets an init that takes an optional string */
	krk_defineNative(&vm.exceptions.baseException->methods, ".__init__", krk_initException);
	ADD_EXCEPTION_CLASS(vm.exceptions.typeError, "TypeError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.argumentError, "ArgumentError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.indexError, "IndexError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.keyError, "KeyError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.attributeError, "AttributeError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.nameError, "NameError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.importError, "ImportError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.ioError, "IOError", vm.exceptions.baseException);

	/* Build classes for basic types */
	ADD_BASE_CLASS(vm.baseClasses.typeClass, "type", vm.objectClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__base__", krk_baseOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__file__", krk_fileOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__doc__", krk_docOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__name__", krk_nameOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__str__", _class_to_str);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__repr__", _class_to_str);
	ADD_BASE_CLASS(vm.baseClasses.intClass, "int", vm.objectClass);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__int__", _noop);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__float__", _int_to_floating);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__chr__", _int_to_char);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__str__", _int_to_str);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__repr__", _int_to_str);
	ADD_BASE_CLASS(vm.baseClasses.floatClass, "float", vm.objectClass);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__int__", _floating_to_int);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__float__", _noop);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__str__", _float_to_str);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__repr__", _float_to_str);
	ADD_BASE_CLASS(vm.baseClasses.boolClass, "bool", vm.objectClass);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__str__", _bool_to_str);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__repr__", _bool_to_str);
	ADD_BASE_CLASS(vm.baseClasses.noneTypeClass, "NoneType", vm.objectClass);
	krk_defineNative(&vm.baseClasses.noneTypeClass->methods, ".__str__", _none_to_str);
	krk_defineNative(&vm.baseClasses.noneTypeClass->methods, ".__repr__", _none_to_str);
	ADD_BASE_CLASS(vm.baseClasses.strClass, "str", vm.objectClass);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__str__", _noop);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__repr__", _repr_str);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__len__", _string_length);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__get__", _string_get);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__set__", _strings_are_immutable);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__int__", _string_to_int);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__float__", _string_to_float);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__getslice__", _string_get_slice);
	ADD_BASE_CLASS(vm.baseClasses.functionClass, "function", vm.objectClass);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__str__", _closure_str);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__repr__", _closure_str);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__doc__", _closure_get_doc);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__name__", _closure_get_name);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__file__", _closure_get_file);
	ADD_BASE_CLASS(vm.baseClasses.methodClass, "method", vm.objectClass);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__str__", _bound_str);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__repr__", _bound_str);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__doc__", _bound_get_doc);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__name__", _bound_get_name);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__file__", _bound_get_file);

	krk_defineNative(&vm.globals, "listOf", krk_list_of);
	krk_defineNative(&vm.globals, "dictOf", krk_dict_of);
	krk_defineNative(&vm.globals, "isinstance", krk_isinstance);
	krk_defineNative(&vm.globals, "type", krk_typeOf);

	krk_defineNative(&vm.builtins->fields, "set_tracing", krk_set_tracing);

#ifndef NO_SYSTEM_BINDS
	/* Set some other built-ins for the system module */
	krk_defineNative(&vm.builtins->fields, "sleep", krk_sleep);
	krk_defineNative(&vm.builtins->fields, "uname", krk_uname);
#endif

	/* Now read the builtins module */
	KrkValue builtinsModule = krk_interpret(_builtins_src,1,"__builtins__","__builtins__");
	if (!IS_OBJECT(builtinsModule)) {
		fprintf(stderr, "VM startup failure: Failed to load __builtins__ module.\n");
	} else {
		/* Get list class */
		KrkValue val;
		krk_tableGet(&vm.globals,OBJECT_VAL(S("list")),&val);
		KrkClass * _class = AS_CLASS(val);
		krk_defineNative(&_class->methods, ".__init__", _list_init);
		krk_defineNative(&_class->methods, ".__get__", _list_get);
		krk_defineNative(&_class->methods, ".__set__", _list_set);
		krk_defineNative(&_class->methods, ".__len__", _list_len);
		krk_defineNative(&_class->methods, ".append", _list_append);

		krk_tableGet(&vm.globals,OBJECT_VAL(S("dict")),&val);
		_class = AS_CLASS(val);
		krk_defineNative(&_class->methods, ".__init__", _dict_init);
		krk_defineNative(&_class->methods, ".__get__", _dict_get);
		krk_defineNative(&_class->methods, ".__set__", _dict_set);
		krk_defineNative(&_class->methods, ".__len__", _dict_len);
		krk_defineNative(&_class->methods, ".capacity", _dict_capacity);
		krk_defineNative(&_class->methods, "._key_at_index", _dict_key_at_index);
	}

	resetStack();
	KRK_RESUME_GC();
}

void krk_freeVM() {
	krk_freeTable(&vm.globals);
	krk_freeTable(&vm.strings);
	krk_freeTable(&vm.modules);
	memset(vm.specialMethodNames,0,sizeof(vm.specialMethodNames));
	krk_freeObjects();
	FREE_ARRAY(size_t, vm.stack, vm.stackSize);
}

static int isFalsey(KrkValue value) {
	return IS_NONE(value) || (IS_BOOLEAN(value) && !AS_BOOLEAN(value)) ||
	       (IS_INTEGER(value) && !AS_INTEGER(value));
	/* Objects in the future: */
	/* IS_STRING && length == 0; IS_ARRAY && length == 0; IS_INSTANCE && __bool__ returns 0... */
}

const char * krk_typeName(KrkValue value) {
	return AS_CLASS(krk_typeOf(1, (KrkValue[]){value}))->name->chars;
}

#define MAKE_BIN_OP(name,operator) \
	static KrkValue name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return FLOATING_VAL(AS_FLOATING(a) operator (double)AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return FLOATING_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return FLOATING_VAL((double)AS_INTEGER(a) operator AS_FLOATING(b)); \
		} \
		krk_runtimeError(vm.exceptions.typeError, "Incompatible types for binary operand %s: %s and %s", #operator, krk_typeName(a), krk_typeName(b)); \
		return NONE_VAL(); \
	}

MAKE_BIN_OP(add,+)
MAKE_BIN_OP(subtract,-)
MAKE_BIN_OP(multiply,*)
MAKE_BIN_OP(divide,/)

#define MAKE_BIT_OP(name,operator) \
	static KrkValue name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		krk_runtimeError(vm.exceptions.typeError, "Incompatible types for binary operand %s: %s and %s", #operator, krk_typeName(a), krk_typeName(b)); \
		return NONE_VAL(); \
	}

MAKE_BIT_OP(bitor,|)
MAKE_BIT_OP(bitxor,^)
MAKE_BIT_OP(bitand,&)
MAKE_BIT_OP(shiftleft,<<)
MAKE_BIT_OP(shiftright,>>)
MAKE_BIT_OP(modulo,%) /* not a bit op, but doesn't work on floating point */

#define MAKE_COMPARATOR(name, operator) \
	static KrkValue name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		} \
		krk_runtimeError(vm.exceptions.typeError, "Can not compare types %s and %s", krk_typeName(a), krk_typeName(b)); \
		return NONE_VAL(); \
	}

MAKE_COMPARATOR(less, <)
MAKE_COMPARATOR(greater, >)

static void concatenate(const char * a, const char * b, size_t al, size_t bl) {
	size_t length = al + bl;
	char * chars = ALLOCATE(char, length + 1);
	memcpy(chars, a, al);
	memcpy(chars + al, b, bl);
	chars[length] = '\0';

	KrkString * result = krk_takeString(chars, length);
	krk_pop();
	krk_pop();
	krk_push(OBJECT_VAL(result));
}

static void addObjects() {
	KrkValue _b = krk_peek(0);
	KrkValue _a = krk_peek(1);

	if (IS_STRING(_a)) {
		KrkString * a = AS_STRING(_a);
		if (IS_STRING(_b)) {
			KrkString * b = AS_STRING(_b);
			concatenate(a->chars,b->chars,a->length,b->length);
			return;
		}
		if (krk_bindMethod(AS_CLASS(krk_typeOf(1,(KrkValue[]){_b})), AS_STRING(vm.specialMethodNames[METHOD_STR]))) {
			KrkValue result;
			int t = krk_callValue(krk_peek(0), 0);
			if (t == 2) {
				result = krk_pop();
			} else if (t == 1) {
				result = krk_runNext();
			} else {
				krk_runtimeError(vm.exceptions.typeError, "__str__ failed to call str on %s", krk_typeName(_b));
				return;
			}
			if (!IS_STRING(result)) {
				krk_runtimeError(vm.exceptions.typeError, "__str__ produced something that wasn't a string: %s", krk_typeName(result));
				return;
			}
			krk_push(result);
			concatenate(a->chars,AS_STRING(result)->chars,a->length,AS_STRING(result)->length);
			return;
		} else {
			char tmp[256] = {0};
			sprintf(tmp, "<%s>", krk_typeName(_b));
			concatenate(a->chars,tmp,a->length,strlen(tmp));
		}
	} else {
		krk_runtimeError(vm.exceptions.typeError, "Can not concatenate types %s and %s", krk_typeName(_a), krk_typeName(_b)); \
	}
}

#define READ_BYTE() (*frame->ip++)
#define BINARY_OP(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); krk_push(op(a,b)); break; }
#define READ_CONSTANT(s) (frame->closure->function->chunk.constants.values[readBytes(frame,s)])
#define READ_STRING(s) AS_STRING(READ_CONSTANT(s))

static inline size_t readBytes(CallFrame * frame, int num) {
	size_t out = READ_BYTE();
	while (--num) {
		out <<= 8;
		out |= (READ_BYTE() & 0xFF);
	}
	return out;
}

static int handleException() {
	int stackOffset, frameOffset;
	int exitSlot = (vm.exitOnFrame >= 0) ? vm.frames[vm.exitOnFrame].slots : 0;
	for (stackOffset = (int)(vm.stackTop - vm.stack - 1); stackOffset >= exitSlot && !IS_HANDLER(vm.stack[stackOffset]); stackOffset--);
	if (stackOffset < exitSlot) {
		if (exitSlot == 0) {
			/* Don't show the internal exception */
			dumpTraceback();
			resetStack();
			vm.frameCount = 0;
		}
		return 1;
	}
	for (frameOffset = vm.frameCount - 1; frameOffset >= 0 && (int)vm.frames[frameOffset].slots > stackOffset; frameOffset--);
	if (frameOffset == -1) {
		fprintf(stderr, "Internal error.\n");
		exit(1);
	}
	closeUpvalues(stackOffset);
	vm.stackTop = vm.stack + stackOffset + 1;
	vm.frameCount = frameOffset + 1;
	vm.flags &= ~KRK_HAS_EXCEPTION;
	return 0;
}

#ifdef ENABLE_DEBUGGING
static void dumpStack(CallFrame * frame) {
	fprintf(stderr, "        | ");
	size_t i = 0;
	for (KrkValue * slot = vm.stack; slot < vm.stackTop; slot++) {
		fprintf(stderr, "[ ");
		if (i == frame->slots) fprintf(stderr, "*");
		krk_printValueSafe(stderr, *slot);
		fprintf(stderr, " ]");
		i++;
	}
	fprintf(stderr, "\n");
}
#endif

int krk_loadModule(KrkString * name, KrkValue * moduleOut) {
	KrkValue modulePaths, modulePathsInternal;

	/* See if the module is already loaded */
	if (krk_tableGet(&vm.modules, OBJECT_VAL(name), moduleOut)) return 1;

	/* Obtain __builtins__.module_paths */
	if (!krk_tableGet(&vm.builtins->fields, OBJECT_VAL(S("module_paths")), &modulePaths) || !IS_INSTANCE(modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.baseException, "Internal error: __builtins__.module_paths not defined.");
		return 0;
	}

	/* Obtain __builtins__.module_paths._list so we can do lookups directly */
	if (!krk_tableGet(&(AS_INSTANCE(modulePaths)->fields), vm.specialMethodNames[METHOD_LIST_INT], &modulePathsInternal) || !IS_FUNCTION(modulePathsInternal)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.baseException, "Internal error: __builtins__.module_paths is corrupted or incorrectly set.");
		return 0;
	}

	/*
	 * So maybe storing lists magically as functions to reuse their constants
	 * tables isn't the _best_ approach, but it works, and until I do something
	 * else it's what we have, so let's do the most efficient thing and look
	 * at the function object directly instead of calling _list_length/_get
	 */
	int moduleCount = AS_FUNCTION(modulePathsInternal)->chunk.constants.count;
	if (!moduleCount) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.importError, "No module search directories are specified, so no modules may be imported.");
		return 0;
	}

	struct stat statbuf;

	/* First search for {name}.krk in the module search paths */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		krk_push(AS_FUNCTION(modulePathsInternal)->chunk.constants.values[i]);
		if (!IS_STRING(krk_peek(0))) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.typeError, "Module search paths must be strings; check the search path at index %d", i);
			return 0;
		}
		krk_push(OBJECT_VAL(name));
		addObjects(); /* Concatenate path... */
		krk_push(OBJECT_VAL(S(".krk")));
		addObjects(); /* and file extension */

		char * fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) < 0) continue;

		/* Compile and run the module in a new context and exit the VM when it
		 * returns to the current call frame; modules should return objects. */
		int previousExitFrame = vm.exitOnFrame;
		vm.exitOnFrame = vm.frameCount;
		*moduleOut = krk_runfile(fileName,1,name->chars,fileName);
		vm.exitOnFrame = previousExitFrame;
		if (!IS_OBJECT(*moduleOut)) {
			krk_runtimeError(vm.exceptions.importError,"Failed to load module '%s' from '%s'", name->chars, fileName);
			return 0;
		}

		krk_pop(); /* concatenated filename on stack */
		krk_push(*moduleOut);
		krk_tableSet(&vm.modules, OBJECT_VAL(name), *moduleOut);
		return 1;
	}

	/* If we didn't find {name}.krk, try {name}.so in the same order */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		/* Assume things haven't changed and all of these are strings. */
		krk_push(AS_FUNCTION(modulePathsInternal)->chunk.constants.values[i]);
		krk_push(OBJECT_VAL(name));
		addObjects(); /* this should just be basic concatenation */
		krk_push(OBJECT_VAL(S(".so")));
		addObjects();

		char * fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) < 0) continue;

		void * dlRef = dlopen(fileName, RTLD_NOW);
		if (!dlRef) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.importError, "Failed to load native module '%s' from shared object '%s'", name->chars, fileName);
			return 0;
		}

		krk_push(OBJECT_VAL(S("krk_module_onload_")));
		krk_push(OBJECT_VAL(name));
		addObjects();

		char * handlerName = AS_CSTRING(krk_peek(0));

		KrkValue (*moduleOnLoad)();
		void * out = dlsym(dlRef, handlerName);
		memcpy(&moduleOnLoad,&out,sizeof(out));

		if (!moduleOnLoad) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.importError, "Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad();
		if (!IS_OBJECT(*moduleOut)) {
			krk_runtimeError(vm.exceptions.importError, "Failed to load module '%s' from '%s'", name->chars, fileName);
			return 0;
		}

		krk_pop(); /* filename */
		krk_push(*moduleOut);
		krk_tableSet(&vm.modules, OBJECT_VAL(name), *moduleOut);
		return 1;
	}

	/* If we still haven't found anything, fail. */
	*moduleOut = NONE_VAL();
	krk_runtimeError(vm.exceptions.importError, "No module named '%s'", name->chars);
	return 0;
}

static int valueGetProperty(KrkString * name) {
	KrkClass * objectClass;
	if (IS_INSTANCE(krk_peek(0))) {
		KrkInstance * instance = AS_INSTANCE(krk_peek(0));
		KrkValue value;
		if (krk_tableGet(&instance->fields, OBJECT_VAL(name), &value)) {
			krk_pop();
			krk_push(value);
			return 1;
		}
		objectClass = instance->_class;
	} else {
		objectClass = AS_CLASS(krk_typeOf(1, (KrkValue[]){krk_peek(0)}));
	}

	/* See if the base class for this non-instance type has a method available */
	if (krk_bindMethod(objectClass, name)) {
		return 1;
	}

	return 0;
}

static KrkValue run() {
	CallFrame* frame = &vm.frames[vm.frameCount - 1];

	for (;;) {
#ifdef ENABLE_TRACING
		if (vm.flags & KRK_ENABLE_TRACING) {
			dumpStack(frame);
			krk_disassembleInstruction(&frame->closure->function->chunk,
				(size_t)(frame->ip - frame->closure->function->chunk.code));
		}
#endif
		uint8_t opcode = READ_BYTE();
		int operandWidth = (opcode & (1 << 7)) ? 3 : 1;

		switch (opcode) {
			case OP_PRINT_LONG:
			case OP_PRINT: {
				uint32_t args = readBytes(frame, operandWidth);
				for (uint32_t i = 0; i < args; ++i) {
					KrkValue printable = krk_peek(args-i-1);
					if (IS_STRING(printable)) { /* krk_printValue runs repr */
						fprintf(stdout, "%s", AS_CSTRING(printable));
					} else {
						krk_printValue(stdout, printable);
					}
					fputc((i == args - 1) ? '\n' : ' ', stdout);
				}
				for (uint32_t i = 0; i < args; ++i) {
					krk_pop();
				}
				break;
			}
			case OP_RETURN: {
				KrkValue result = krk_pop();
				closeUpvalues(frame->slots);
				vm.frameCount--;
				if (frame->isInlined) {
					vm.frames[vm.frameCount - 1].ip = frame->ip;
				}
				if (vm.frameCount == 0) {
					krk_pop();
					return result;
				}
				vm.stackTop = &vm.stack[frame->slots];
				if (vm.frameCount == (size_t)vm.exitOnFrame) {
					return result;
				}
				krk_push(result);
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			case OP_EQUAL: {
				KrkValue b = krk_pop();
				KrkValue a = krk_pop();
				krk_push(BOOLEAN_VAL(krk_valuesEqual(a,b)));
				break;
			}
			case OP_LESS: BINARY_OP(less);
			case OP_GREATER: BINARY_OP(greater)
			case OP_ADD:
				if (IS_OBJECT(krk_peek(0)) || IS_OBJECT(krk_peek(1))) addObjects();
				else BINARY_OP(add)
				break;
			case OP_SUBTRACT: BINARY_OP(subtract)
			case OP_MULTIPLY: BINARY_OP(multiply)
			case OP_DIVIDE: BINARY_OP(divide)
			case OP_MODULO: BINARY_OP(modulo)
			case OP_BITOR: BINARY_OP(bitor)
			case OP_BITXOR: BINARY_OP(bitxor)
			case OP_BITAND: BINARY_OP(bitand)
			case OP_SHIFTLEFT: BINARY_OP(shiftleft)
			case OP_SHIFTRIGHT: BINARY_OP(shiftright)
			case OP_BITNEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(~AS_INTEGER(value)));
				else { krk_runtimeError(vm.exceptions.typeError, "Incompatible operand type for bit negation."); goto _finishException; }
				break;
			}
			case OP_NEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(-AS_INTEGER(value)));
				else if (IS_FLOATING(value)) krk_push(FLOATING_VAL(-AS_FLOATING(value)));
				else { krk_runtimeError(vm.exceptions.typeError, "Incompatible operand type for prefix negation."); goto _finishException; }
				break;
			}
			case OP_CONSTANT_LONG:
			case OP_CONSTANT: {
				size_t index = readBytes(frame, operandWidth);
				KrkValue constant = frame->closure->function->chunk.constants.values[index];
				krk_push(constant);
				break;
			}
			case OP_NONE:  krk_push(NONE_VAL()); break;
			case OP_TRUE:  krk_push(BOOLEAN_VAL(1)); break;
			case OP_FALSE: krk_push(BOOLEAN_VAL(0)); break;
			case OP_NOT:   krk_push(BOOLEAN_VAL(isFalsey(krk_pop()))); break;
			case OP_POP:   krk_pop(); break;
			case OP_DEFINE_GLOBAL_LONG:
			case OP_DEFINE_GLOBAL: {
				KrkString * name = READ_STRING(operandWidth);
				krk_tableSet(&vm.globals, OBJECT_VAL(name), krk_peek(0));
				krk_pop();
				break;
			}
			case OP_GET_GLOBAL_LONG:
			case OP_GET_GLOBAL: {
				KrkString * name = READ_STRING(operandWidth);
				KrkValue value;
				if (!krk_tableGet(&vm.globals, OBJECT_VAL(name), &value)) {
					krk_runtimeError(vm.exceptions.nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				krk_push(value);
				break;
			}
			case OP_SET_GLOBAL_LONG:
			case OP_SET_GLOBAL: {
				KrkString * name = READ_STRING(operandWidth);
				if (krk_tableSet(&vm.globals, OBJECT_VAL(name), krk_peek(0))) {
					krk_tableDelete(&vm.globals, OBJECT_VAL(name));
					/* TODO: This should probably just work as an assignment? */
					krk_runtimeError(vm.exceptions.nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_IMPORT_LONG:
			case OP_IMPORT: {
				KrkString * name = READ_STRING(operandWidth);
				KrkValue module;
				if (!krk_loadModule(name, &module)) {
					goto _finishException;
				}
				break;
			}
			case OP_GET_LOCAL_LONG:
			case OP_GET_LOCAL: {
				uint32_t slot = readBytes(frame, operandWidth);
				krk_push(vm.stack[frame->slots + slot]);
				break;
			}
			case OP_SET_LOCAL_LONG:
			case OP_SET_LOCAL: {
				uint32_t slot = readBytes(frame, operandWidth);
				vm.stack[frame->slots + slot] = krk_peek(0);
				break;
			}
			case OP_JUMP_IF_FALSE: {
				uint16_t offset = readBytes(frame, 2);
				if (isFalsey(krk_peek(0))) frame->ip += offset;
				break;
			}
			case OP_JUMP_IF_TRUE: {
				uint16_t offset = readBytes(frame, 2);
				if (!isFalsey(krk_peek(0))) frame->ip += offset;
				break;
			}
			case OP_JUMP: {
				frame->ip += readBytes(frame, 2);
				break;
			}
			case OP_LOOP: {
				uint16_t offset = readBytes(frame, 2);
				frame->ip -= offset;
				break;
			}
			case OP_PUSH_TRY: {
				uint16_t tryTarget = readBytes(frame, 2) + (frame->ip - frame->closure->function->chunk.code);
				KrkValue handler = HANDLER_VAL(tryTarget);
				krk_push(handler);
				break;
			}
			case OP_RAISE: {
				vm.currentException = krk_pop();
				vm.flags |= KRK_HAS_EXCEPTION;
				goto _finishException;
			}
			/* Sometimes you just want to increment a stack-local integer quickly. */
			case OP_INC_LONG:
			case OP_INC: {
				uint32_t slot = readBytes(frame, operandWidth);
				vm.stack[frame->slots + slot] = INTEGER_VAL(AS_INTEGER(vm.stack[frame->slots+slot])+1);
				break;
			}
			case OP_CALL_LONG:
			case OP_CALL: {
				int argCount = readBytes(frame, operandWidth);
				if (!krk_callValue(krk_peek(argCount), argCount)) {
					if (vm.flags & KRK_HAS_EXCEPTION) goto _finishException;
					return NONE_VAL();
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			/* This version of the call instruction takes its arity from the
			 * top of the stack, so we don't have calculate arity at compile time. */
			case OP_CALL_STACK: {
				int argCount = AS_INTEGER(krk_pop());
				if (!krk_callValue(krk_peek(argCount), argCount)) {
					if (vm.flags & KRK_HAS_EXCEPTION) goto _finishException;
					return NONE_VAL();
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			case OP_CLOSURE_LONG:
			case OP_CLOSURE: {
				KrkFunction * function = AS_FUNCTION(READ_CONSTANT(operandWidth));
				KrkClosure * closure = krk_newClosure(function);
				krk_push(OBJECT_VAL(closure));
				for (size_t i = 0; i < closure->upvalueCount; ++i) {
					int isLocal = READ_BYTE();
					int index = readBytes(frame,(i > 255) ? 3 : 1);
					if (isLocal) {
						closure->upvalues[i] = captureUpvalue(frame->slots + index);
					} else {
						closure->upvalues[i] = frame->closure->upvalues[index];
					}
				}
				break;
			}
			case OP_GET_UPVALUE_LONG:
			case OP_GET_UPVALUE: {
				int slot = readBytes(frame, operandWidth);
				krk_push(*UPVALUE_LOCATION(frame->closure->upvalues[slot]));
				break;
			}
			case OP_SET_UPVALUE_LONG:
			case OP_SET_UPVALUE: {
				int slot = readBytes(frame, operandWidth);
				*UPVALUE_LOCATION(frame->closure->upvalues[slot]) = krk_peek(0);
				break;
			}
			case OP_CLOSE_UPVALUE:
				closeUpvalues((vm.stackTop - vm.stack)-1);
				krk_pop();
				break;
			case OP_CLASS_LONG:
			case OP_CLASS: {
				KrkString * name = READ_STRING(operandWidth);
				KrkClass * _class = krk_newClass(name);
				krk_push(OBJECT_VAL(_class));
				_class->filename = frame->closure->function->chunk.filename;
				_class->base = vm.objectClass;
				krk_tableAddAll(&vm.objectClass->methods, &_class->methods);
				break;
			}
			case OP_INLINE_FUNCTION: {
				CallFrame * newFrame = &vm.frames[vm.frameCount++];
				newFrame->isInlined = 1;
				newFrame->closure = frame->closure;
				newFrame->ip = frame->ip;
				newFrame->slots = vm.stackTop - vm.stack;
				frame = newFrame;
				break;
			}
			case OP_GET_PROPERTY_LONG:
			case OP_GET_PROPERTY: {
				KrkString * name = READ_STRING(operandWidth);
				if (!valueGetProperty(name)) {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_INVOKE_GETTER: {
				krk_swap(1);
				valueGetProperty(AS_STRING(vm.specialMethodNames[METHOD_GET]));
				krk_swap(1);
				switch (krk_callValue(krk_peek(1),1)) {
					case 2: break;
					case 1: krk_push(krk_runNext()); break;
					default: krk_runtimeError(vm.exceptions.typeError, "Invalid method call."); goto _finishException;
				}
				break;
			}
			case OP_INVOKE_SETTER: {
				krk_push(krk_peek(2)); /* object to top */
				valueGetProperty(AS_STRING(vm.specialMethodNames[METHOD_SET]));
				krk_swap(3);
				krk_pop();
				switch (krk_callValue(krk_peek(2),2)) {
					case 2: break;
					case 1: krk_push(krk_runNext()); break;
					default: krk_runtimeError(vm.exceptions.typeError, "Invalid method call."); goto _finishException;
				}
				break;
			}
			case OP_INVOKE_GETSLICE: {
				krk_push(krk_peek(2)); /* object to top */
				valueGetProperty(AS_STRING(vm.specialMethodNames[METHOD_GETSLICE]));
				krk_swap(3);
				krk_pop();
				switch (krk_callValue(krk_peek(2),2)) {
					case 2: break;
					case 1: krk_push(krk_runNext()); break;
					default: krk_runtimeError(vm.exceptions.typeError, "Invalid method call."); goto _finishException;
				}
				break;
			}
			case OP_SET_PROPERTY_LONG:
			case OP_SET_PROPERTY: {
				KrkString * name = READ_STRING(operandWidth);
				if (!IS_INSTANCE(krk_peek(1))) {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				KrkInstance * instance = AS_INSTANCE(krk_peek(1));
				krk_tableSet(&instance->fields, OBJECT_VAL(name), krk_peek(0));
				KrkValue value = krk_pop();
				krk_pop(); /* instance */
				krk_push(value); /* Moves value in */
				break;
			}
			case OP_METHOD_LONG:
			case OP_METHOD: {
				defineMethod(READ_STRING(operandWidth));
				break;
			}
			case OP_INHERIT: {
				KrkValue superclass = krk_peek(1);
				if (!IS_CLASS(superclass)) {
					krk_runtimeError(vm.exceptions.typeError, "Superclass must be a class.");
					return NONE_VAL();
				}
				KrkClass * subclass = AS_CLASS(krk_peek(0));
				subclass->base = AS_CLASS(superclass);
				krk_tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
				krk_pop();
				break;
			}
			case OP_DOCSTRING: {
				KrkClass * me = AS_CLASS(krk_peek(1));
				me->docstring = AS_STRING(krk_pop());
				break;
			}
			case OP_GET_SUPER_LONG:
			case OP_GET_SUPER: {
				KrkString * name = READ_STRING(operandWidth);
				KrkClass * superclass = AS_CLASS(krk_pop());
				if (!krk_bindMethod(superclass, name)) {
					return NONE_VAL();
				}
				break;
			}
			case OP_DUP:
				krk_push(krk_peek(READ_BYTE()));
				break;
			case OP_SWAP:
				krk_swap(READ_BYTE());
				break;
		}
		if (!(vm.flags & KRK_HAS_EXCEPTION)) continue;
_finishException:
		if (!handleException()) {
			frame = &vm.frames[vm.frameCount - 1];
			frame->ip = frame->closure->function->chunk.code + AS_HANDLER(krk_peek(0));
			/* Replace the exception handler with the exception */
			krk_pop();
			krk_push(vm.currentException);
			vm.currentException = NONE_VAL();
		} else {
			return NONE_VAL();
		}
	}


#undef BINARY_OP
#undef READ_BYTE
}

KrkValue krk_interpret(const char * src, int newScope, char * fromName, char * fromFile) {
	KrkFunction * function = krk_compile(src, newScope, fromFile);

	if (!function) return NONE_VAL();

	krk_push(OBJECT_VAL(function));

	function->name = krk_copyString(fromName, strlen(fromName));

	KrkClosure * closure = krk_newClosure(function);
	krk_pop();

	krk_push(OBJECT_VAL(closure));
	krk_callValue(OBJECT_VAL(closure), 0);

	return run();
}


KrkValue krk_runfile(const char * fileName, int newScope, char * fromName, char * fromFile) {
	FILE * f = fopen(fileName,"r");
	if (!f) return NONE_VAL();

	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char * buf = malloc(size+1);
	if (fread(buf, 1, size, f) != size) {
		fprintf(stderr, "Warning: Failed to read file.\n");
	}
	fclose(f);
	buf[size] = '\0';

	KrkValue result = krk_interpret(buf, newScope, fromName, fromFile);
	free(buf);

	return result;
}

