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
static int callValue(KrkValue callee, int argCount);
static int bindMethod(KrkClass * _class, KrkString * name);
static int call(KrkClosure * closure, int argCount);

extern const char _builtins_src[];

static void resetStack() {
	vm.stackTop = vm.stack;
	vm.frameCount = 0;
	vm.openUpvalues = NULL;
	vm.flags &= ~KRK_HAS_EXCEPTION;
	vm.currentException = NONE_VAL();
}

static void dumpTraceback() {
	if (IS_STRING(vm.currentException)) {
		fprintf(stderr, "%s", AS_CSTRING(vm.currentException));
	} else {
		krk_printObject(stderr, vm.currentException);
	}
	fprintf(stderr, "\nTraceback, most recent first, %d call frames:\n", (int)vm.frameCount);

	for (size_t i = 0; i <= vm.frameCount - 1; i++) {
		CallFrame * frame = &vm.frames[i];
		KrkFunction * function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		fprintf(stderr, "  File \"%s\", line %d, in %s\n",
			(function->chunk.filename ? function->chunk.filename->chars : "?"),
			(int)function->chunk.lines[instruction],
			(function->name ? function->name->chars : "(unnamed)"));
	}
}

void krk_runtimeError(const char * fmt, ...) {
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	size_t len = vsnprintf(buf, 1024, fmt, args);
	va_end(args);
	vm.flags |= KRK_HAS_EXCEPTION;

	vm.currentException = OBJECT_VAL(krk_copyString(buf,len));
}

void krk_push(KrkValue value) {
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

void krk_swap() {
	KrkValue b = krk_pop();
	KrkValue a = krk_pop();
	krk_push(b);
	krk_push(a);
}

KrkValue krk_peek(int distance) {
	return vm.stackTop[-1 - distance];
}

void krk_defineNative(KrkTable * table, const char * name, NativeFn function) {
	KrkNative * func = krk_newNative(function);
	if (*name == '.') {
		name++;
		func->isMethod = 1;
	}
	krk_push(OBJECT_VAL(func));
	krk_push(OBJECT_VAL(krk_copyString(name, (int)strlen(name))));
	krk_tableSet(table, krk_peek(0), krk_peek(1));
	krk_pop();
	krk_pop();
}

static KrkValue krk_expose_hash_new(int argc, KrkValue argv[]) {
	/* This is absuing the existing object system so it can work without
	 * having to add any new types to the garbage collector, and yes
	 * it is absolute terrible, do not use it. */
	KrkClass * map = krk_newClass(NULL);
	return OBJECT_VAL(map);
}

static KrkValue krk_expose_hash_get(int argc, KrkValue argv[]) {
	if (argc < 2 || !IS_CLASS(argv[0])) {
		krk_runtimeError("wrong number of arguments");
		return NONE_VAL();
	}
	KrkClass * map = AS_CLASS(argv[0]);
	KrkValue out = NONE_VAL();
	if (!krk_tableGet(&map->methods, argv[1], &out)) {
		krk_runtimeError("key error");
	}
	return out;
}

static KrkValue krk_expose_hash_set(int argc, KrkValue argv[]) {
	if (argc < 3 || !IS_CLASS(argv[0])) {
		krk_runtimeError("wrong number of arguments");
		return NONE_VAL();
	}
	KrkClass * map = AS_CLASS(argv[0]);
	krk_tableSet(&map->methods, argv[1], argv[2]);
	return BOOLEAN_VAL(1);
}

static KrkValue krk_expose_hash_capacity(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_CLASS(argv[0])) {
		krk_runtimeError("wrong number of arguments");
		return NONE_VAL();
	}
	KrkClass * map = AS_CLASS(argv[0]);
	return INTEGER_VAL(map->methods.capacity);
}

static KrkValue krk_expose_hash_count(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_CLASS(argv[0])) {
		krk_runtimeError("wrong number of arguments");
		return NONE_VAL();
	}
	KrkClass * map = AS_CLASS(argv[0]);
	return INTEGER_VAL(map->methods.count);
}

static KrkValue krk_expose_hash_key_at_index(int argc, KrkValue argv[]) {
	if (argc < 2 || !IS_CLASS(argv[0])) {
		krk_runtimeError("wrong number of arguments");
		return NONE_VAL();
	}
	if (!IS_INTEGER(argv[1])) {
		krk_runtimeError("expected integer index but got %s", krk_typeName(argv[1]));
		return NONE_VAL();
	}
	int i = AS_INTEGER(argv[1]);
	KrkClass * map = AS_CLASS(argv[0]);
	if (i < 0 || i > (int)map->methods.capacity) {
		krk_runtimeError("hash table index is out of range: %d", i);
		return NONE_VAL();
	}
	KrkTableEntry entry = map->methods.entries[i];
	return entry.key;
}

static KrkValue krk_expose_list_new(int argc, KrkValue argv[]) {
	KrkFunction * list = krk_newFunction(NULL);
	return OBJECT_VAL(list);
}

static KrkValue krk_expose_list_get(int argc, KrkValue argv[]) {
	if (argc < 2 || !IS_FUNCTION(argv[0]) || !IS_INTEGER(argv[1])) {
		krk_runtimeError("wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkFunction * list = AS_FUNCTION(argv[0]);
	int index = AS_INTEGER(argv[1]);
	if (index < 0 || index >= (int)list->chunk.constants.count) {
		krk_runtimeError("index is out of range: %d", index);
		return NONE_VAL();
	}
	return list->chunk.constants.values[index];
}

static KrkValue krk_expose_list_set(int argc, KrkValue argv[]) {
	if (argc < 3 || !IS_FUNCTION(argv[0]) || !IS_INTEGER(argv[1])) {
		krk_runtimeError("wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkFunction * list = AS_FUNCTION(argv[0]);
	int index = AS_INTEGER(argv[1]);
	if (index < 0 || index >= (int)list->chunk.constants.count) {
		krk_runtimeError("index is out of range: %d", index);
		return NONE_VAL();
	}
	list->chunk.constants.values[index] = argv[2];
	return BOOLEAN_VAL(1);
}

static KrkValue krk_expose_list_append(int argc, KrkValue argv[]) {
	if (argc < 2 || !IS_FUNCTION(argv[0])) {
		krk_runtimeError("wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkFunction * list = AS_FUNCTION(argv[0]);
	krk_writeValueArray(&list->chunk.constants, argv[1]);
	return INTEGER_VAL(list->chunk.constants.count-1);
}

static KrkValue krk_expose_list_length(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_FUNCTION(argv[0])) {
		krk_runtimeError("wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkFunction * list = AS_FUNCTION(argv[0]);
	return INTEGER_VAL(list->chunk.constants.count);
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
	krk_tableSet(&outList->fields, OBJECT_VAL(S("_list")), OBJECT_VAL(listContents));
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
		krk_runtimeError("Expected even number of arguments to dictOf");
		return NONE_VAL();
	}
	KrkValue Class;
	krk_tableGet(&vm.globals,OBJECT_VAL(S("dict")), &Class);
	KrkInstance * outDict = krk_newInstance(AS_CLASS(Class));
	krk_push(OBJECT_VAL(outDict));
	KrkClass * dictContents = krk_newClass(NULL);
	krk_push(OBJECT_VAL(dictContents));
	krk_tableSet(&outDict->fields, OBJECT_VAL(S("_map")), OBJECT_VAL(dictContents));
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
		krk_runtimeError("sleep: expect at least one argument.");
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
	else if (!strcmp(AS_CSTRING(argv[0]),"tracing=1")) vm.flags |= KRK_ENABLE_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"debugging=1")) vm.flags |= KRK_ENABLE_DEBUGGING;
	else if (!strcmp(AS_CSTRING(argv[0]),"scantracing=1")) vm.flags |= KRK_ENABLE_SCAN_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"stressgc=1")) vm.flags |= KRK_ENABLE_STRESS_GC;
	else if (!strcmp(AS_CSTRING(argv[0]),"tracing=0")) vm.flags &= ~KRK_ENABLE_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"debugging=0")) vm.flags &= ~KRK_ENABLE_DEBUGGING;
	else if (!strcmp(AS_CSTRING(argv[0]),"scantracing=0")) vm.flags &= ~KRK_ENABLE_SCAN_TRACING;
	else if (!strcmp(AS_CSTRING(argv[0]),"stressgc=0")) vm.flags &= ~KRK_ENABLE_STRESS_GC;
	return BOOLEAN_VAL(1);
}

static KrkValue krk_dirObject(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError( "wrong number of arguments or bad type, got %d\n", argc);
		return NONE_VAL();
	}

	/* Obtain self-reference */
	KrkInstance * self = AS_INSTANCE(argv[0]);

	/* Create a new list instance */
	KrkValue Class;
	krk_tableGet(&vm.globals,OBJECT_VAL(S("list")), &Class);
	KrkInstance * outList = krk_newInstance(AS_CLASS(Class));
	krk_push(OBJECT_VAL(outList));
	KrkFunction * listContents = krk_newFunction(NULL);
	krk_push(OBJECT_VAL(listContents));
	krk_tableSet(&outList->fields, OBJECT_VAL(S("_list")), OBJECT_VAL(listContents));

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

	/* Prepare output value */
	KrkValue out = OBJECT_VAL(outList);
	krk_pop();
	krk_pop();
	return out;
}

static KrkValue krk_isinstance(int argc, KrkValue argv[]) {
	if (argc != 2) {
		krk_runtimeError("isinstance expects 2 arguments, got %d", argc);
		return NONE_VAL();
	}

	if (!IS_CLASS(argv[1])) {
		krk_runtimeError("isinstance() arg 2 must be class");
		return NONE_VAL();
	}

	/* Things which are not instances are not instances of anything...
	 * (for now, there should be fake classes for them later.) */
	if (!IS_INSTANCE(argv[0])) return BOOLEAN_VAL(0);

	KrkInstance * obj = AS_INSTANCE(argv[0]);
	KrkClass * obj_class = obj->_class;
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
		krk_runtimeError("%s() takes %s %d argument%s (%d given)",
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
		krk_runtimeError("Too many call frames.");
		return 0;
	}
	CallFrame * frame = &vm.frames[vm.frameCount++];
	frame->isInlined = 0;
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = (vm.stackTop - argCount - 1) - vm.stack;
	return 1;
}

static int callValue(KrkValue callee, int argCount) {
	if (IS_OBJECT(callee)) {
		switch (OBJECT_TYPE(callee)) {
			case OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount);
			case OBJ_NATIVE: {
				NativeFn native = AS_NATIVE(callee);
				int extraArgs = !!((KrkNative*)AS_OBJECT(callee))->isMethod;
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
					return callValue(initializer, argCount);
				} else if (argCount != 0) {
					krk_runtimeError("Class does not have an __init__ but arguments were passed to initializer: %d\n", argCount);
					return 0;
				}
				return 1;
			}
			case OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				vm.stackTop[-argCount - 1] = bound->receiver;
				return callValue(OBJECT_VAL(bound->method), argCount);
			}
			default:
				break;
		}
	}
	krk_runtimeError("Attempted to call non-callable type: %s", krk_typeName(callee));
	return 0;
}

static int bindMethod(KrkClass * _class, KrkString * name) {
	KrkValue method;
	if (!krk_tableGet(&_class->methods, OBJECT_VAL(name), &method)) return 0;
	KrkBoundMethod * bound = krk_newBoundMethod(krk_peek(0), AS_OBJECT(method));
	krk_pop();
	krk_push(OBJECT_VAL(bound));
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

	/* Create built-in class `object` */
	vm.object_class = krk_newClass(S("object"));
	krk_attachNamedObject(&vm.globals, "object", (KrkObj*)vm.object_class);
	krk_defineNative(&vm.object_class->methods, ".__dir__", krk_dirObject);

	/* Build classes for basic types with __init__ methods to generate them */

	vm.builtins = krk_newInstance(vm.object_class);
	krk_attachNamedObject(&vm.globals, "__builtins__", (KrkObj*)vm.builtins);

	krk_defineNative(&vm.builtins->fields, "hash_new", krk_expose_hash_new);
	krk_defineNative(&vm.builtins->fields, "hash_set", krk_expose_hash_set);
	krk_defineNative(&vm.builtins->fields, "hash_get", krk_expose_hash_get);
	krk_defineNative(&vm.builtins->fields, "hash_key_at_index", krk_expose_hash_key_at_index);
	krk_defineNative(&vm.builtins->fields, "hash_capacity", krk_expose_hash_capacity);
	krk_defineNative(&vm.builtins->fields, "hash_count", krk_expose_hash_count);

	krk_defineNative(&vm.builtins->fields, "list_new", krk_expose_list_new);
	krk_defineNative(&vm.builtins->fields, "list_get", krk_expose_list_get);
	krk_defineNative(&vm.builtins->fields, "list_set", krk_expose_list_set);
	krk_defineNative(&vm.builtins->fields, "list_append", krk_expose_list_append);
	krk_defineNative(&vm.builtins->fields, "list_length", krk_expose_list_length);

	krk_defineNative(&vm.builtins->fields, "set_tracing", krk_set_tracing);

#ifndef NO_SYSTEM_BINDS
	/* Set some other built-ins for the system module */
	krk_defineNative(&vm.builtins->fields, "sleep", krk_sleep);
	krk_defineNative(&vm.builtins->fields, "uname", krk_uname);
#endif

	krk_defineNative(&vm.globals, "listOf", krk_list_of);
	krk_defineNative(&vm.globals, "dictOf", krk_dict_of);
	krk_defineNative(&vm.globals, "isinstance", krk_isinstance);

	/* Now read the builtins module */
	KrkValue builtinsModule = krk_interpret(_builtins_src,1,"__builtins__","__builtins__");
	if (!IS_OBJECT(builtinsModule)) {
		fprintf(stderr, "VM startup failure: Failed to load __builtins__ module.\n");
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
	if (value.type == VAL_BOOLEAN) return "Boolean";
	if (value.type == VAL_NONE) return "None";
	if (value.type == VAL_INTEGER) return "Integer";
	if (value.type == VAL_FLOATING) return "Floating";
	if (value.type == VAL_OBJECT) {
		if (IS_STRING(value)) return "String";
		if (IS_FUNCTION(value)) return "Function";
		if (IS_NATIVE(value)) return "Native";
		if (IS_CLOSURE(value)) return "Closure";
		if (IS_CLASS(value)) return "Class";
		if (IS_INSTANCE(value)) return "Instance";
		if (IS_BOUND_METHOD(value)) return "BoundMethod";
		return "(Unspecified Object)";
	}
	return "???";
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
		krk_runtimeError("Incompatible types for binary operand %s: %s and %s", #operator, krk_typeName(a), krk_typeName(b)); \
		return NONE_VAL(); \
	}

MAKE_BIN_OP(add,+)
MAKE_BIN_OP(subtract,-)
MAKE_BIN_OP(multiply,*)
MAKE_BIN_OP(divide,/)

#define MAKE_BIT_OP(name,operator) \
	static KrkValue name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		krk_runtimeError("Incompatible types for binary operand %s: %s and %s", #operator, krk_typeName(a), krk_typeName(b)); \
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
		krk_runtimeError("Can not compare types %s and %s", krk_typeName(a), krk_typeName(b)); \
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
		char tmp[256] = {0};
		if (IS_INTEGER(_b)) {
			sprintf(tmp, "%ld", (long)AS_INTEGER(_b));
		} else if (IS_FLOATING(_b)) {
			sprintf(tmp, "%g", AS_FLOATING(_b));
		} else if (IS_BOOLEAN(_b)) {
			sprintf(tmp, "%s", AS_BOOLEAN(_b) ? "True" : "False");
		} else if (IS_NONE(_b)) {
			sprintf(tmp, "None");
		} else if (IS_INSTANCE(_b)) {
			KrkValue method;
			if (!krk_tableGet(&AS_INSTANCE(_b)->_class->methods, vm.specialMethodNames[METHOD_STR], &method)) {
				sprintf(tmp, "<instance>");
			} else {
				/* Push the object for self reference */
				krk_push(_b);
				KrkValue result;
				switch (callValue(method, 0)) {
					case 0: result = NONE_VAL(); break;
					case 1: result = krk_runNext(); break;
					case 2: result = krk_pop(); break;
				}
				if (!IS_STRING(result)) {
					krk_runtimeError("__str__ produced something that wasn't a string: %s", krk_typeName(result));
					return;
				}
				sprintf(tmp, "%s", AS_CSTRING(result));
			}
		} else {
			sprintf(tmp, "<Object>");
		}
		concatenate(a->chars,tmp,a->length,strlen(tmp));
	} else {
		krk_runtimeError("Can not concatenate types %s and %s", krk_typeName(_a), krk_typeName(_b)); \
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

static KrkClosure * boundNative(NativeFn method, int arity) {
	/* Build an object */
	KrkValue nativeFunction = OBJECT_VAL(krk_newNative(method));

	/* Build a function that calls it */
	KrkFunction * methodWrapper = krk_newFunction();
	methodWrapper->requiredArgs = arity; /* This is WITHOUT the self reference */
	krk_writeConstant(&methodWrapper->chunk, nativeFunction, 1);

	/* Stack silliness */
	krk_writeChunk(&methodWrapper->chunk, OP_GET_LOCAL, 1); /* Should be bind receiver */
	krk_writeChunk(&methodWrapper->chunk, 0, 1);
	for (int i = 0; i < arity; ++i) {
		krk_writeChunk(&methodWrapper->chunk, OP_GET_LOCAL, 1); /* Should be arguments */
		krk_writeChunk(&methodWrapper->chunk, i + 1, 1);
	}

	/* Call with these arguments */
	if (arity > 255) {
		int n = arity + 1;
		krk_writeChunk(&methodWrapper->chunk, OP_CALL_LONG, 1);
		krk_writeChunk(&methodWrapper->chunk, (n >> 16) & 0xFF, 1);
		krk_writeChunk(&methodWrapper->chunk, (n >> 8) & 0xFF, 1);
		krk_writeChunk(&methodWrapper->chunk, (n >> 0) & 0xFF, 1);
	} else {
		krk_writeChunk(&methodWrapper->chunk, OP_CALL, 1);
		krk_writeChunk(&methodWrapper->chunk, arity + 1, 1); /* arguments to call with */
	}

	/* Return from the wrapper with whatever result we got from the native method */
	krk_writeChunk(&methodWrapper->chunk, OP_RETURN, 1);
	return krk_newClosure(methodWrapper);
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

static KrkValue _string_get_slice(int argc, KrkValue argv[]) {
	if (argc < 3) { /* 3 because first is us */
		krk_runtimeError("slice: expected 2 arguments, got %d", argc-1);
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0]) ||
		!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) ||
		!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) {
		krk_runtimeError("slice: expected two integer arguments");
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
		krk_runtimeError("Wrong number of arguments to String.__get__");
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0])) {
		krk_runtimeError("First argument to __get__ must be String");
		return NONE_VAL();
	}
	if (!IS_INTEGER(argv[1])) {
		krk_runtimeError("Strings can not index by %s", krk_typeName(argv[1]));
		return NONE_VAL();
	}
	int asInt = AS_INTEGER(argv[1]);
	if (asInt < 0) asInt += (int)AS_STRING(argv[0])->length;
	if (asInt < 0 || asInt >= (int)AS_STRING(argv[0])->length) {
		krk_runtimeError("String index out of range: %d", asInt);
		return NONE_VAL();
	}
	return INTEGER_VAL(AS_CSTRING(argv[0])[asInt]);
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

static void bindSpecialMethod(NativeFn method, int arity) {
	KRK_PAUSE_GC();
	KrkBoundMethod * bound = krk_newBoundMethod(krk_peek(0), (KrkObj*)boundNative(method,arity));
	krk_pop(); /* The original object */
	krk_push(OBJECT_VAL(bound));
	KRK_RESUME_GC();
}

static void dumpStack(CallFrame * frame) {
	fprintf(stderr, "        | ");
	size_t i = 0;
	for (KrkValue * slot = vm.stack; slot < vm.stackTop; slot++) {
		fprintf(stderr, "[ ");
		if (i == frame->slots) fprintf(stderr, "*");
		krk_printValue(stderr, *slot);
		fprintf(stderr, " ]");
		i++;
	}
	fprintf(stderr, "\n");
}

int krk_loadModule(KrkString * name, KrkValue * moduleOut) {
	KrkValue modulePaths, modulePathsInternal;

	/* See if the module is already loaded */
	if (krk_tableGet(&vm.modules, OBJECT_VAL(name), moduleOut)) return 1;

	/* Obtain __builtins__.module_paths */
	if (!krk_tableGet(&vm.builtins->fields, OBJECT_VAL(S("module_paths")), &modulePaths) || !IS_INSTANCE(modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError("Internal error: __builtins__.module_paths not defined.");
		return 0;
	}

	/* Obtain __builtins__.module_paths._list so we can do lookups directly */
	if (!krk_tableGet(&(AS_INSTANCE(modulePaths)->fields), OBJECT_VAL(S("_list")), &modulePathsInternal) || !IS_FUNCTION(modulePathsInternal)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError("Internal error: __builtins__.module_paths is corrupted or incorrectly set.");
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
		krk_runtimeError("No module search directories are specified, so no modules may be imported.");
		return 0;
	}

	struct stat statbuf;

	/* First search for {name}.krk in the module search paths */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		krk_push(AS_FUNCTION(modulePathsInternal)->chunk.constants.values[i]);
		if (!IS_STRING(krk_peek(0))) {
			*moduleOut = NONE_VAL();
			krk_runtimeError("Module search paths must be strings; check the search path at index %d", i);
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
			krk_runtimeError("Failed to load module '%s' from '%s'", name->chars, fileName);
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
			krk_runtimeError("Failed to load native module '%s' from shared object '%s'", name->chars, fileName);
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
			krk_runtimeError("Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad();
		if (!IS_OBJECT(*moduleOut)) {
			krk_runtimeError("Failed to load module '%s' from '%s'", name->chars, fileName);
			return 0;
		}

		krk_pop(); /* filename */
		krk_push(*moduleOut);
		krk_tableSet(&vm.modules, OBJECT_VAL(name), *moduleOut);
		return 1;
	}

	/* If we still haven't found anything, fail. */
	*moduleOut = NONE_VAL();
	krk_runtimeError("No module named '%s'", name->chars);
	return 0;
}

static KrkValue run() {
	CallFrame* frame = &vm.frames[vm.frameCount - 1];

	for (;;) {
#ifdef ENABLE_DEBUGGING
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
					if (!IS_STRING(printable)) {
						krk_push(OBJECT_VAL(S("")));
						krk_push(printable);
						addObjects();
						printable = krk_peek(0);
					} else {
						krk_push(printable);
					}
					fprintf(stdout, "%s%s", AS_CSTRING(printable), (i == args - 1) ? "\n" : " ");
					krk_pop();
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
				else { krk_runtimeError("Incompatible operand type for bit negation."); goto _finishException; }
				break;
			}
			case OP_NEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(-AS_INTEGER(value)));
				else if (IS_FLOATING(value)) krk_push(FLOATING_VAL(-AS_FLOATING(value)));
				else { krk_runtimeError("Incompatible operand type for prefix negation."); goto _finishException; }
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
					krk_runtimeError("Undefined variable '%s'.", name->chars);
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
					krk_runtimeError("Undefined variable '%s'.", name->chars);
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
				if (!callValue(krk_peek(argCount), argCount)) {
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
				if (!callValue(krk_peek(argCount), argCount)) {
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
				_class->base = vm.object_class;
				krk_tableAddAll(&vm.object_class->methods, &_class->methods);
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
				switch (krk_peek(0).type) {
					case VAL_OBJECT:
						switch (OBJECT_TYPE(krk_peek(0))) {
							case OBJ_INSTANCE: {
								KrkInstance * instance = AS_INSTANCE(krk_peek(0));
								KrkValue value;
								if (krk_tableGet(&instance->fields, OBJECT_VAL(name), &value)) {
									krk_pop();
									krk_push(value);
								} else if (!bindMethod(instance->_class, name)) {
									/* Try synthentic properties */
									if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_CLASS])) {
										krk_pop();
										krk_push(OBJECT_VAL(instance->_class));
									} else {
										goto _undefined;
									}
								}
								break;
							}
							case OBJ_CLASS: {
								KrkClass * _class = AS_CLASS(krk_peek(0));
								if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_NAME])) {
									krk_pop(); /* class */
									krk_push(OBJECT_VAL(_class->name));
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_FILE])) {
									krk_pop();
									krk_push(OBJECT_VAL(_class->filename));
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_DOC])) {
									KrkValue out = _class->docstring ? OBJECT_VAL(_class->docstring) : NONE_VAL();
									krk_pop();
									krk_push(out);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_BASE])) {
									KrkValue out = _class->base ? OBJECT_VAL(_class->base) : NONE_VAL();
									krk_pop();
									krk_push(out);
								} else {
									goto _undefined;
								}
								break;
							}
							case OBJ_STRING: {
								if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_LEN])) {
									bindSpecialMethod(_string_length,0);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_GET])) {
									bindSpecialMethod(_string_get,1);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_INT])) {
									bindSpecialMethod(_string_to_int,0);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_FLOAT])) {
									bindSpecialMethod(_string_to_float,0);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_GETSLICE])) {
									bindSpecialMethod(_string_get_slice,2);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_SET])) {
									krk_runtimeError("Strings are not mutable.");
									goto _finishException;
								} else {
									goto _undefined;
								}
								break;
							}
							case OBJ_CLOSURE: {
								if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_DOC])) {
									KrkClosure * closure = AS_CLOSURE(krk_peek(0));
									KrkValue out = closure->function->docstring ? OBJECT_VAL(closure->function->docstring) : NONE_VAL();
									krk_pop();
									krk_push(out);
								} else {
									goto _undefined;
								}
								break;
							}
							case OBJ_BOUND_METHOD: {
								if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_DOC])) {
									KrkBoundMethod * boundMethod = AS_BOUND_METHOD(krk_peek(0));
									KrkObj * method = boundMethod->method;
									KrkValue out = NONE_VAL();
									switch (method->type) {
										case OBJ_CLOSURE: out = ((KrkClosure*)method)->function->docstring ?
											OBJECT_VAL(((KrkClosure*)method)->function->docstring) : NONE_VAL();
											break;
										default:
											break;
									}
									krk_pop();
									krk_push(out);
								} else {
									goto _undefined;
								}
								break;
							}
							default:
								goto _undefined;
						}
						break;
					case VAL_FLOATING: {
						if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_INT])) {
							bindSpecialMethod(_floating_to_int,0);
						} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_FLOAT])) {
							bindSpecialMethod(_noop,0);
						} else goto _undefined;
						break;
					}
					case VAL_INTEGER: {
						if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_FLOAT])) {
							bindSpecialMethod(_int_to_floating,0);
						} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_INT])) {
							bindSpecialMethod(_noop,0);
						} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_CHR])) {
							bindSpecialMethod(_int_to_char,0);
						} else goto _undefined;
						break;
					}
					default:
						krk_runtimeError("Don't know how to retreive properties for %s yet", krk_typeName(krk_peek(0)));
						goto _finishException;
				}
				break;
_undefined:
				krk_runtimeError("Field '%s' of %s is not defined.", name->chars, krk_typeName(krk_peek(0)));
				goto _finishException;
			}
			case OP_SET_PROPERTY_LONG:
			case OP_SET_PROPERTY: {
				if (!IS_INSTANCE(krk_peek(1))) {
					krk_runtimeError("Don't know how to set properties for %s yet", krk_typeName(krk_peek(1)));
					goto _finishException;
				}
				KrkInstance * instance = AS_INSTANCE(krk_peek(1));
				KrkString * name = READ_STRING(operandWidth);
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
					krk_runtimeError("Superclass must be a class.");
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
				if (!bindMethod(superclass, name)) {
					return NONE_VAL();
				}
				break;
			}
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
	callValue(OBJECT_VAL(closure), 0);

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

