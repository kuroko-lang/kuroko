#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include "vm.h"
#include "debug.h"
#include "memory.h"
#include "compiler.h"
#include "object.h"
#include "table.h"

#define MODULE_PATH "modules"
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
		fprintf(stderr, "stack underflow!\n");
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
	krk_push(OBJECT_VAL(krk_copyString(name, (int)strlen(name))));
	krk_push(OBJECT_VAL(krk_newNative(function)));
	krk_tableSet(table, vm.stack[0], vm.stack[1]);
	krk_pop();
	krk_pop();
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

static void runNext(void) {
	size_t oldExit = vm.exitOnFrame;
	vm.exitOnFrame = vm.frameCount - 1;
	run();
	vm.exitOnFrame = oldExit;
}

static KrkInstance * _dict_create(KrkValue * outClass) {
	krk_tableGet(&vm.globals,OBJECT_VAL(S("dict")), outClass);
	KrkInstance * outDict = krk_newInstance(AS_CLASS(*outClass));
	krk_push(OBJECT_VAL(outDict));
	KrkValue tmp;
	if (krk_tableGet(&AS_CLASS(*outClass)->methods, vm.specialMethodNames[METHOD_INIT], &tmp)) {
		call(AS_CLOSURE(tmp), 0);
		runNext();
	}
	return outDict;
}

static void _dict_set(KrkValue dictClass, KrkInstance * dict, KrkValue key, KrkValue value) {
	krk_push(OBJECT_VAL(dict));
	krk_push(key);
	krk_push(value);
	KrkValue tmp;
	if (krk_tableGet(&AS_CLASS(dictClass)->methods, vm.specialMethodNames[METHOD_SET], &tmp)) {
		call(AS_CLOSURE(tmp), 2);
		runNext();
	}
}

static KrkValue krk_uname(int argc, KrkValue argv[]) {
	struct utsname buf;
	if (uname(&buf) < 0) return NONE_VAL();

	KRK_PAUSE_GC();

	KrkValue dictClass;
	KrkInstance * dict = _dict_create(&dictClass);
	_dict_set(dictClass, dict, OBJECT_VAL(S("sysname")), OBJECT_VAL(krk_copyString(buf.sysname,strlen(buf.sysname))));
	_dict_set(dictClass, dict, OBJECT_VAL(S("nodename")), OBJECT_VAL(krk_copyString(buf.nodename,strlen(buf.nodename))));
	_dict_set(dictClass, dict, OBJECT_VAL(S("release")), OBJECT_VAL(krk_copyString(buf.release,strlen(buf.release))));
	_dict_set(dictClass, dict, OBJECT_VAL(S("version")), OBJECT_VAL(krk_copyString(buf.version,strlen(buf.version))));
	_dict_set(dictClass, dict, OBJECT_VAL(S("machine")), OBJECT_VAL(krk_copyString(buf.machine,strlen(buf.machine))));

	KrkValue result = OBJECT_VAL(dict);

	KRK_RESUME_GC();

	return result;
}

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

static int call(KrkClosure * closure, int argCount) {
	if (argCount != closure->function->arity) {
		krk_runtimeError("Wrong number of arguments (%d expected, got %d)", closure->function->arity, argCount);
		return 0;
	}
	if (vm.frameCount == FRAMES_MAX) {
		krk_runtimeError("Too many call frames.");
		return 0;
	}
	CallFrame * frame = &vm.frames[vm.frameCount++];
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
				KrkValue result = native(argCount, vm.stackTop - argCount);
				if (vm.stackTop == vm.stack) {
					/* Runtime error returned from native method */
					return 0;
				}
				vm.stackTop -= argCount + 1;
				krk_push(result);
				return 1;
			}
			case OBJ_CLASS: {
				KrkClass * _class = AS_CLASS(callee);
				vm.stackTop[-argCount - 1] = OBJECT_VAL(krk_newInstance(_class));
				KrkValue initializer;
				if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_INIT], &initializer)) {
					return call(AS_CLOSURE(initializer), argCount);
				} else if (argCount != 0) {
					krk_runtimeError("Class does not have an __init__ but arguments were passed to initializer: %d\n", argCount);
					return 0;
				}
				return 1;
			}
			case OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				vm.stackTop[-argCount - 1] = bound->receiver;
				return call(bound->method, argCount);
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
	KrkBoundMethod * bound = krk_newBoundMethod(krk_peek(0), AS_CLOSURE(method));
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
	krk_tableSet(&vm.globals, vm.stack[0], vm.stack[1]);
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

	/* Create built-in class `object` */
	vm.object_class = krk_newClass(S("object"));
	krk_attachNamedObject(&vm.globals, "object", (KrkObj*)vm.object_class);

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

	/* Set some other built-ins for the system module */
	krk_defineNative(&vm.builtins->fields, "sleep", krk_sleep);
	krk_defineNative(&vm.builtins->fields, "uname", krk_uname);
	krk_defineNative(&vm.builtins->fields, "set_tracing", krk_set_tracing);

	/* Now read the builtins module */
	KrkValue builtinsModule = krk_interpret(_builtins_src,1,"__builtins__","__builtins__");
	if (!IS_OBJECT(builtinsModule)) {
		fprintf(stderr, "VM startup failure: Failed to load __builtins__ module.\n");
	} else {
		krk_attachNamedObject(&vm.builtins->fields, "__builtins__", AS_OBJECT(builtinsModule));
	}
	resetStack();

	KRK_RESUME_GC();
}

void krk_freeVM() {
	krk_freeTable(&vm.globals);
	krk_freeTable(&vm.strings);
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

static KrkValue modulo(KrkValue a, KrkValue b) {
	if (!IS_INTEGER(a) || !IS_INTEGER(b)) return NONE_VAL();
	return INTEGER_VAL(AS_INTEGER(a) % AS_INTEGER(b));
}

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
				call(AS_CLOSURE(method), 0);
				int previousExitFrame = vm.exitOnFrame;
				vm.exitOnFrame = vm.frameCount - 1;
				KrkValue result = run();
				vm.exitOnFrame = previousExitFrame;
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

static size_t readBytes(CallFrame * frame, int num) {
	if (num == 1) return READ_BYTE();
	else if (num == 2) {
		unsigned int top = READ_BYTE();
		unsigned int bot = READ_BYTE();
		return (top << 8) | (bot);
	} else if (num == 3) {
		unsigned int top = READ_BYTE();
		unsigned int mid = READ_BYTE();
		unsigned int bot = READ_BYTE();
		return (top << 16) | (mid << 8) | (bot);
	}

	krk_runtimeError("Invalid byte read?");
	return (size_t)-1;
}

static KrkClosure * boundNative(NativeFn method, int arity) {
	/* Build an object */
	KrkValue nativeFunction = OBJECT_VAL(krk_newNative(method));

	/* Build a function that calls it */
	KrkFunction * methodWrapper = krk_newFunction();
	methodWrapper->arity = arity; /* This is WITHOUT the self reference */
	krk_writeConstant(&methodWrapper->chunk, nativeFunction, 1);

	/* Stack silliness */
	krk_writeChunk(&methodWrapper->chunk, OP_GET_LOCAL, 1); /* Should be bind receiver */
	krk_writeChunk(&methodWrapper->chunk, 0, 1);
	for (int i = 0; i < arity; ++i) {
		krk_writeChunk(&methodWrapper->chunk, OP_GET_LOCAL, 1); /* Should be arguments */
		krk_writeChunk(&methodWrapper->chunk, i + 1, 1);
	}

	/* Call with these arguments */
	krk_writeChunk(&methodWrapper->chunk, OP_CALL, 1);
	krk_writeChunk(&methodWrapper->chunk, arity + 1, 1); /* arguments to call with */

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
	KrkBoundMethod * bound = krk_newBoundMethod(krk_peek(0), boundNative(method,arity));
	krk_pop(); /* The original object */
	krk_push(OBJECT_VAL(bound));
	KRK_RESUME_GC();
}

static KrkValue run() {
	CallFrame* frame = &vm.frames[vm.frameCount - 1];

	for (;;) {
#ifdef ENABLE_DEBUGGING
		if (vm.flags & KRK_ENABLE_TRACING) {
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
			krk_disassembleInstruction(&frame->closure->function->chunk,
				(size_t)(frame->ip - frame->closure->function->chunk.code));
		}
#endif
		uint8_t opcode;
		switch ((opcode = READ_BYTE())) {
			case OP_PRINT: {
				if (!IS_STRING(krk_peek(0))) {
					krk_push(OBJECT_VAL(S("")));
					krk_swap();
					addObjects();
				}
				fprintf(stdout, "%s\n", AS_CSTRING(krk_pop()));
				break;
			}
			case OP_RETURN: {
				KrkValue result = krk_pop();
				closeUpvalues(frame->slots);
				vm.frameCount--;
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
			case OP_NEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(-AS_INTEGER(value)));
				else if (IS_FLOATING(value)) krk_push(FLOATING_VAL(-AS_FLOATING(value)));
				else { krk_runtimeError("Incompatible operand type for prefix negation."); goto _finishException; }
				break;
			}
			case OP_CONSTANT_LONG:
			case OP_CONSTANT: {
				size_t index = readBytes(frame, opcode == OP_CONSTANT ? 1 : 3);
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
				KrkString * name = READ_STRING((opcode == OP_DEFINE_GLOBAL ? 1 : 3));
				krk_tableSet(&vm.globals, OBJECT_VAL(name), krk_peek(0));
				krk_pop();
				break;
			}
			case OP_GET_GLOBAL_LONG:
			case OP_GET_GLOBAL: {
				KrkString * name = READ_STRING((opcode == OP_GET_GLOBAL ? 1 : 3));
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
				KrkString * name = READ_STRING((opcode == OP_SET_GLOBAL ? 1 : 3));
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
				KrkString * name = READ_STRING((opcode == OP_IMPORT ? 1 : 3));
				KrkValue module;
				if (!krk_tableGet(&vm.modules, OBJECT_VAL(name), &module)) {
					/* Try to open it */
					char tmp[256];
					sprintf(tmp, MODULE_PATH "/%s.krk", name->chars);
					int previousExitFrame = vm.exitOnFrame;
					vm.exitOnFrame = vm.frameCount;
					module = krk_runfile(tmp,1,name->chars,tmp);
					vm.exitOnFrame = previousExitFrame;
					if (!IS_OBJECT(module)) {
						krk_runtimeError("Failed to import module - expected to receive an object, but got a %s instead.", krk_typeName(module));
						goto _finishException;
					}
					krk_push(module);
					krk_tableSet(&vm.modules, OBJECT_VAL(name), module);
					break;
				}
				krk_push(module);
				break;
			}
			case OP_GET_LOCAL_LONG:
			case OP_GET_LOCAL: {
				uint32_t slot = readBytes(frame, (opcode == OP_GET_LOCAL ? 1 : 3));
				krk_push(vm.stack[frame->slots + slot]);
				break;
			}
			case OP_SET_LOCAL_LONG:
			case OP_SET_LOCAL: {
				uint32_t slot = readBytes(frame, (opcode == OP_SET_LOCAL ? 1 : 3));
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
			case OP_CALL: {
				int argCount = READ_BYTE();
				if (!callValue(krk_peek(argCount), argCount)) {
					if (vm.flags & KRK_HAS_EXCEPTION) goto _finishException;
					return NONE_VAL();
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			case OP_CLOSURE_LONG:
			case OP_CLOSURE: {
				KrkFunction * function = AS_FUNCTION(READ_CONSTANT((opcode == OP_CLOSURE ? 1 : 3)));
				KrkClosure * closure = krk_newClosure(function);
				krk_push(OBJECT_VAL(closure));
				for (size_t i = 0; i < closure->upvalueCount; ++i) {
					int isLocal = READ_BYTE();
					int index = READ_BYTE();
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
				int slot = readBytes(frame, (opcode == OP_GET_UPVALUE) ? 1 : 3);
				krk_push(*UPVALUE_LOCATION(frame->closure->upvalues[slot]));
				break;
			}
			case OP_SET_UPVALUE_LONG:
			case OP_SET_UPVALUE: {
				int slot = readBytes(frame, (opcode == OP_SET_UPVALUE) ? 1 : 3);
				*UPVALUE_LOCATION(frame->closure->upvalues[slot]) = krk_peek(0);
				break;
			}
			case OP_CLOSE_UPVALUE:
				closeUpvalues((vm.stackTop - vm.stack)-1);
				krk_pop();
				break;
			case OP_CLASS_LONG:
			case OP_CLASS: {
				KrkString * name = READ_STRING((opcode == OP_CLASS ? 1 : 3));
				KrkClass * _class = krk_newClass(name);
				_class->filename = frame->closure->function->chunk.filename;
				krk_push(OBJECT_VAL(_class));
				break;
			}
			case OP_GET_PROPERTY_LONG:
			case OP_GET_PROPERTY: {
				KrkString * name = READ_STRING((opcode == OP_GET_PROPERTY ? 1 : 3));
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
								} else {
									goto _undefined;
								}
								break;
							}
							case OBJ_STRING: {
								/* vm.specialMethodNames[NAME_LEN] ? */
								if (!strcmp(name->chars,"length")) {
									bindSpecialMethod(_string_length,0);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_GET])) {
									bindSpecialMethod(_string_get,1);
								} else if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_SET])) {
									krk_runtimeError("Strings are not mutable.");
									goto _finishException;
								} else {
									goto _undefined;
								}
								break;
							}
							default:
								break;
						}
						break;
					case VAL_FLOATING: {
						if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_INT])) {
							bindSpecialMethod(_floating_to_int,0);
						} else goto _undefined;
						break;
					}
					case VAL_INTEGER: {
						if (krk_valuesEqual(OBJECT_VAL(name), vm.specialMethodNames[METHOD_FLOAT])) {
							bindSpecialMethod(_int_to_floating,0);
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
				KrkString * name = READ_STRING((opcode == OP_SET_PROPERTY ? 1 : 3));
				krk_tableSet(&instance->fields, OBJECT_VAL(name), krk_peek(0));
				KrkValue value = krk_pop();
				krk_pop(); /* instance */
				krk_push(value); /* Moves value in */
				break;
			}
			case OP_METHOD_LONG:
			case OP_METHOD: {
				defineMethod(READ_STRING((opcode == OP_METHOD ? 1 : 3)));
				break;
			}
			case OP_INHERIT: {
				KrkValue superclass = krk_peek(1);
				if (!IS_CLASS(superclass)) {
					krk_runtimeError("Superclass must be a class.");
					return NONE_VAL();
				}
				KrkClass * subclass = AS_CLASS(krk_peek(0));
				krk_tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
				krk_pop();
				break;
			}
			case OP_GET_SUPER_LONG:
			case OP_GET_SUPER: {
				KrkString * name = READ_STRING((opcode == OP_GET_SUPER ? 1 : 3));
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

