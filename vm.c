#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include "vm.h"
#include "debug.h"
#include "memory.h"
#include "compiler.h"
#include "object.h"
#include "table.h"

#define S(c) (krk_copyString(c,sizeof(c)-1))

/* This is macro'd to krk_vm for namespacing reasons. */
KrkVM vm;

static KrkValue run();
static KrkValue krk_isinstance(int argc, KrkValue argv[]);

/* Embedded script for extensions to builtin-ins; see builtins.c/builtins.krk */
extern const char _builtins_src[];

/**
 * Reset the stack pointers, frame, upvalue list,
 * clear the exception flag and current exception;
 * happens on startup (twice) and after an exception.
 */
void krk_resetStack() {
	vm.stackTop = vm.stack;
	vm.frameCount = 0;
	vm.openUpvalues = NULL;
	vm.flags &= ~KRK_HAS_EXCEPTION;
	vm.currentException = NONE_VAL();
}

#ifdef ENABLE_TRACING
/**
 * When tracing is enabled, we will present the elements on the stack with
 * a safe printer; the format of values printed by krk_printValueSafe will
 * look different from those printed by printValue, but they guarantee that
 * the VM will never be called to produce a string, which would result in
 * a nasty infinite recursion if we did it while trying to trace the VM!
 */
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
	if (i == frame->slots) {
		fprintf(stderr, " * ");
	}
	fprintf(stderr, "\n");
}
#endif

/**
 * Display a traceback by working through call frames.
 * Called when no exception handler was available and
 * an exception was thrown. If there the exception value
 * is not None, it will also be printed using safe methods.
 */
static void dumpTraceback() {
	fprintf(stderr, "Traceback, most recent first, %d call frame%s:\n", (int)vm.frameCount, vm.frameCount == 1 ? "" : "s");
	for (size_t i = 0; i <= vm.frameCount - 1; i++) {
		CallFrame * frame = &vm.frames[i];
		KrkFunction * function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		fprintf(stderr, "  File \"%s\", line %d, in %s\n",
			(function->chunk.filename ? function->chunk.filename->chars : "?"),
			(int)krk_lineNumber(&function->chunk, instruction),
			(function->name ? function->name->chars : "(unnamed)"));
	}

	if (!krk_valuesEqual(vm.currentException,NONE_VAL())) {
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
}

/**
 * Raise an exception. Creates an exception object of the requested type
 * and formats a message string to attach to it. Exception classes are
 * found in vm.exceptions and are initialized on startup.
 */
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

/**
 * Push a value onto the stack, and grow the stack if necessary.
 * Note that growing the stack can involve the stack _moving_, so
 * do not rely on the memory offset of a stack value if you expect
 * the stack to grow - eg. if you are calling into managed code
 * to do anything, or if you are pushing anything.
 */
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

/**
 * Pop the top of the stack. We never reclaim space used by the stack,
 * so anything that is popped can be safely pushed back on without
 * the stack moving, and you an also generally rely on a popped item
 * still being where it was if you don't allocate anything in between;
 * the repl relies on this it expects to be able to get the last
 * pushed value and display it (if it's not None).
 */
KrkValue krk_pop() {
	vm.stackTop--;
	if (vm.stackTop < vm.stack) {
		fprintf(stderr, "Fatal error: stack underflow detected in VM, issuing breakpoint.\n");
		__asm__ ("int $3");
		return NONE_VAL();
	}
	return *vm.stackTop;
}

/* Read a value `distance` units from the top of the stack without poping it. */
inline KrkValue krk_peek(int distance) {
	return vm.stackTop[-1 - distance];
}

/* Exchange the value `distance` units down from the top of the stack with
 * the value at the top of the stack. */
void krk_swap(int distance) {
	KrkValue top = vm.stackTop[-1];
	vm.stackTop[-1] = vm.stackTop[-1 - distance];
	vm.stackTop[-1 - distance] = top;
}

/**
 * Bind a native function to the given table (eg. vm.globals, or _class->methods)
 * GC safe: pushes allocated values.
 */
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

/**
 * For a class built by native code, call this after attaching methods to
 * finalize the attachment of special methods for quicker accessn.
 *
 * For a class built by managed code, called by OP_FINALIZE
 */
void krk_finalizeClass(KrkClass * _class) {
	KrkValue tmp;

	struct TypeMap {
		KrkObj ** method;
		KrkSpecialMethods index;
	};
	struct TypeMap specials[] = {
		{&_class->_getter, METHOD_GET},
		{&_class->_setter, METHOD_SET},
		{&_class->_slicer, METHOD_GETSLICE},
		{&_class->_reprer, METHOD_REPR},
		{&_class->_tostr, METHOD_STR},
		{&_class->_call, METHOD_CALL},
		{&_class->_init, METHOD_INIT},
		{NULL, 0},
	};

	for (struct TypeMap * entry = specials; entry->method; ++entry) {
		if (krk_tableGet(&_class->methods, vm.specialMethodNames[entry->index], &tmp)) {
			*entry->method = AS_OBJECT(tmp);
		}
	}
}

/***************
 * Collections *
****************/

/**
 * dict.__init__()
 */
static KrkValue _dict_init(int argc, KrkValue argv[]) {
	KrkClass * dict = krk_newClass(NULL);
	krk_push(OBJECT_VAL(dict));
	krk_tableSet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], OBJECT_VAL(dict));
	AS_INSTANCE(argv[0])->_internal = (KrkObj*)dict;
	krk_tableSet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_INREPR], INTEGER_VAL(0));
	krk_pop();
	return argv[0];
}

/**
 * dict.__get__(key)
 */
static KrkValue _dict_get(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	KrkValue out;
	if (!krk_tableGet(AS_DICT(_dict_internal), argv[1], &out)) {
		krk_runtimeError(vm.exceptions.keyError, "key error");
	}
	return out;
}

/**
 * dict.__set__(key, value)
 */
static KrkValue _dict_set(int argc, KrkValue argv[]) {
	if (argc < 3) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	krk_tableSet(AS_DICT(_dict_internal), argv[1], argv[2]);
	return NONE_VAL();
}

/**
 * dict.__len__()
 */
static KrkValue _dict_len(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	return INTEGER_VAL(AS_DICT(_dict_internal)->count);
}

/**
 * dict.__contains__()
 */
static KrkValue _dict_contains(int argc, KrkValue argv[]) {
	KrkValue _unused;
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	return BOOLEAN_VAL(krk_tableGet(AS_DICT(_dict_internal), argv[1], &_unused));
}

/**
 * dict.capacity()
 */
static KrkValue _dict_capacity(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
		return NONE_VAL();
	}
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	return INTEGER_VAL(AS_DICT(_dict_internal)->capacity);
}

/**
 * dict._key_at_index(internalIndex)
 */
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
	if (i < 0 || i > (int)AS_DICT(_dict_internal)->capacity) {
		krk_runtimeError(vm.exceptions.indexError, "hash table index is out of range: %d", i);
		return NONE_VAL();
	}
	KrkTableEntry entry = AS_DICT(_dict_internal)->entries[i];
	return entry.key;
}

/**
 * list.__init__()
 */
static KrkValue _list_init(int argc, KrkValue argv[]) {
	KrkFunction * list = krk_newFunction(NULL);
	krk_push(OBJECT_VAL(list));
	krk_tableSet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_LIST_INT], OBJECT_VAL(list));
	AS_INSTANCE(argv[0])->_internal = (KrkObj*)list;
	krk_tableSet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_INREPR], INTEGER_VAL(0));
	krk_pop();
	return argv[0];
}

/**
 * list.__get__(index)
 */
static KrkValue _list_get(int argc, KrkValue argv[]) {
	if (argc < 2 || !IS_INTEGER(argv[1])) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments in get %d, (%s, %s)", argc, krk_typeName(argv[0]), krk_typeName(argv[1]));
		return NONE_VAL();
	}
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[0])->_internal);
	int index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(_list_internal)->count;
	if (index < 0 || index >= (int)AS_LIST(_list_internal)->count) {
		krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
		return NONE_VAL();
	}
	return AS_LIST(_list_internal)->values[index];
}

/**
 * list.__set__(index, value)
 */
static KrkValue _list_set(int argc, KrkValue argv[]) {
	if (argc < 3 || !IS_INTEGER(argv[1])) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments in set %d, (%s, %s, %s)", argc, krk_typeName(argv[0]), krk_typeName(argv[1]), krk_typeName(argv[2]));
		return NONE_VAL();
	}
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[0])->_internal);
	int index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(_list_internal)->count;
	if (index < 0 || index >= (int)AS_LIST(_list_internal)->count) {
		krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
		return NONE_VAL();
	}
	AS_LIST(_list_internal)->values[index] = argv[2];
	return NONE_VAL();
}

/**
 * list.append(value)
 */
static KrkValue _list_append(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[0])->_internal);
	krk_writeValueArray(AS_LIST(_list_internal), argv[1]);
	return NONE_VAL();
}

/**
 * list.__len__
 */
static KrkValue _list_len(int argc, KrkValue argv[]) {
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[0])->_internal);
	return INTEGER_VAL(AS_LIST(_list_internal)->count);
}

/**
 * list.__contains__
 */
static KrkValue _list_contains(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
		return NONE_VAL();
	}
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[0])->_internal);
	for (size_t i = 0; i < AS_LIST(_list_internal)->count; ++i) {
		if (krk_valuesEqual(argv[1], AS_LIST(_list_internal)->values[i])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

/**
 * Run the VM until it returns from the current call frame;
 * used by native methods to call into managed methods.
 * Returns the value returned by the RETURN instruction that
 * exited the call frame. Should be nestable so a managed method
 * can call a native method can call a managed can call a native
 * and so on (hopefully).
 */
KrkValue krk_runNext(void) {
	size_t oldExit = vm.exitOnFrame;
	vm.exitOnFrame = vm.frameCount - 1;
	KrkValue result = run();
	vm.exitOnFrame = oldExit;
	return result;
}

/**
 * Exposed method called to produce lists from [expr,...] sequences in managed code.
 * Presented in the global namespace as listOf(...)
 */
static KrkValue krk_list_of(int argc, KrkValue argv[]) {
	KrkValue Class;
	krk_tableGet(&vm.globals,OBJECT_VAL(S("list")), &Class);
	KrkInstance * outList = krk_newInstance(AS_CLASS(Class));
	krk_push(OBJECT_VAL(outList));
	KrkFunction * listContents = krk_newFunction(NULL);
	krk_push(OBJECT_VAL(listContents));
	krk_tableSet(&outList->fields, vm.specialMethodNames[METHOD_LIST_INT], OBJECT_VAL(listContents));
	outList->_internal = (KrkObj*)listContents;
	krk_tableSet(&outList->fields, vm.specialMethodNames[METHOD_INREPR], INTEGER_VAL(0));
	for (int ind = 0; ind < argc; ++ind) {
		krk_writeValueArray(&listContents->chunk.constants, argv[ind]);
	}
	KrkValue out = OBJECT_VAL(outList);
	krk_pop(); /* listContents */
	krk_pop(); /* outList */
	return out;
}

/**
 * Exposed method called to produce dictionaries from {expr: expr, ...} sequences in managed code.
 * Presented in the global namespace as dictOf(...). Expects arguments as key,value,key,value...
 */
KrkValue krk_dict_of(int argc, KrkValue argv[]) {
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
	outDict->_internal = (KrkObj*)dictContents;
	krk_tableSet(&outDict->fields, vm.specialMethodNames[METHOD_INREPR], INTEGER_VAL(0));
	for (int ind = 0; ind < argc; ind += 2) {
		krk_tableSet(&dictContents->methods, argv[ind], argv[ind+1]);
	}
	KrkValue out = OBJECT_VAL(outDict);
	krk_pop(); /* dictContents */
	krk_pop(); /* outDict */
	return out;
}

/**
 * list.__getslice__
 */
static KrkValue _list_slice(int argc, KrkValue argv[]) {
	if (argc < 3) { /* 3 because first is us */
		krk_runtimeError(vm.exceptions.argumentError, "slice: expected 2 arguments, got %d", argc-1);
		return NONE_VAL();
	}
	if (!IS_INSTANCE(argv[0]) ||
		!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) ||
		!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) {
		krk_runtimeError(vm.exceptions.typeError, "slice: expected two integer arguments");
		return NONE_VAL();
	}

	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[0])->_internal);

	int start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	int end   = IS_NONE(argv[2]) ? (int)AS_LIST(_list_internal)->count : AS_INTEGER(argv[2]);
	if (start < 0) start = (int)AS_LIST(_list_internal)->count + start;
	if (start < 0) start = 0;
	if (end < 0) end = (int)AS_LIST(_list_internal)->count + end;
	if (start > (int)AS_LIST(_list_internal)->count) start = (int)AS_LIST(_list_internal)->count;
	if (end > (int)AS_LIST(_list_internal)->count) end = (int)AS_LIST(_list_internal)->count;
	if (end < start) end = start;
	int len = end - start;

	return krk_list_of(len, &AS_LIST(_list_internal)->values[start]);
}

/**
 * __builtins__.set_tracing(mode)
 *
 * Takes either one string "mode=value" or `n` keyword args mode=value.
 */
static KrkValue krk_set_tracing(int argc, KrkValue argv[], int hasKw) {
#ifdef DEBUG
	if (argc != 1) return NONE_VAL();
	if (hasKw) {
		KrkValue _dict_internal;
		krk_tableGet(&AS_INSTANCE(argv[0])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
		KrkValue test;
		if (krk_tableGet(AS_DICT(_dict_internal), OBJECT_VAL(S("tracing")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_TRACING; else vm.flags &= ~KRK_ENABLE_TRACING; }
		if (krk_tableGet(AS_DICT(_dict_internal), OBJECT_VAL(S("disassembly")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_DISASSEMBLY; else vm.flags &= ~KRK_ENABLE_DISASSEMBLY; }
		if (krk_tableGet(AS_DICT(_dict_internal), OBJECT_VAL(S("stressgc")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_STRESS_GC; else vm.flags &= ~KRK_ENABLE_STRESS_GC; }
		if (krk_tableGet(AS_DICT(_dict_internal), OBJECT_VAL(S("scantracing")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_SCAN_TRACING; else vm.flags &= ~KRK_ENABLE_SCAN_TRACING; }
		return BOOLEAN_VAL(1);
	} else {
		if (!strcmp(AS_CSTRING(argv[0]),"tracing=1")) vm.flags |= KRK_ENABLE_TRACING;
		else if (!strcmp(AS_CSTRING(argv[0]),"disassembly=1")) vm.flags |= KRK_ENABLE_DISASSEMBLY;
		else if (!strcmp(AS_CSTRING(argv[0]),"scantracing=1")) vm.flags |= KRK_ENABLE_SCAN_TRACING;
		else if (!strcmp(AS_CSTRING(argv[0]),"stressgc=1")) vm.flags |= KRK_ENABLE_STRESS_GC;
		else if (!strcmp(AS_CSTRING(argv[0]),"tracing=0")) vm.flags &= ~KRK_ENABLE_TRACING;
		else if (!strcmp(AS_CSTRING(argv[0]),"disassembly=0")) vm.flags &= ~KRK_ENABLE_DISASSEMBLY;
		else if (!strcmp(AS_CSTRING(argv[0]),"scantracing=0")) vm.flags &= ~KRK_ENABLE_SCAN_TRACING;
		else if (!strcmp(AS_CSTRING(argv[0]),"stressgc=0")) vm.flags &= ~KRK_ENABLE_STRESS_GC;
		return BOOLEAN_VAL(1);
	}
#else
	krk_runtimeError(vm.exceptions.typeError,"Debugging is not enabled in this build.");
	return NONE_VAL();
#endif
}

/**
 * object.__dir__()
 */
static KrkValue krk_dirObject(int argc, KrkValue argv[]) {
	if (argc != 1) {
		krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments or bad type, got %d\n", argc);
		return NONE_VAL();
	}

	/* Create a new list instance */
	KrkValue myList = krk_list_of(0,NULL);
	krk_push(myList);
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(myList)->_internal);

	if (IS_INSTANCE(argv[0])) {
		/* Obtain self-reference */
		KrkInstance * self = AS_INSTANCE(argv[0]);

		/* First add each method of the class */
		for (size_t i = 0; i < self->_class->methods.capacity; ++i) {
			if (self->_class->methods.entries[i].key.type != VAL_NONE) {
				krk_writeValueArray(AS_LIST(_list_internal),
					self->_class->methods.entries[i].key);
			}
		}

		/* Then add each field of the instance */
		for (size_t i = 0; i < self->fields.capacity; ++i) {
			if (self->fields.entries[i].key.type != VAL_NONE) {
				krk_writeValueArray(AS_LIST(_list_internal),
					self->fields.entries[i].key);
			}
		}
	} else {
		KrkClass * type = AS_CLASS(krk_typeOf(1, (KrkValue[]){argv[0]}));

		for (size_t i = 0; i < type->methods.capacity; ++i) {
			if (type->methods.entries[i].key.type != VAL_NONE) {
				krk_writeValueArray(AS_LIST(_list_internal),
					type->methods.entries[i].key);
			}
		}
	}

	/* Prepare output value */
	krk_pop();
	return myList;
}

/**
 * type(obj)
 *
 * For basic types (non-instances), finds the associated pseudo-class;
 * for instances, returns the associated real class.
 *
 * Called often in native code as krk_typeOf(1,(KrkValue[]){value})
 */
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

static KrkValue _type_init(int argc, KrkValue argv[]) {
	if (argc != 2) {
		krk_runtimeError(vm.exceptions.argumentError, "type() takes 1 argument");
		return NONE_VAL();
	}
	return krk_typeOf(1,&argv[1]);
}

/* Class.__base__ */
static KrkValue krk_baseOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->base ? OBJECT_VAL(AS_CLASS(argv[0])->base) : NONE_VAL();
}

/* Class.__name */
static KrkValue krk_nameOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->name ? OBJECT_VAL(AS_CLASS(argv[0])->name) : NONE_VAL();
}

/* Class.__file__ */
static KrkValue krk_fileOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->filename ? OBJECT_VAL(AS_CLASS(argv[0])->filename) : NONE_VAL();
}

/* Class.__doc__ */
static KrkValue krk_docOfClass(int argc, KrkValue argv[]) {
	return AS_CLASS(argv[0])->docstring ? OBJECT_VAL(AS_CLASS(argv[0])->docstring) : NONE_VAL();
}

/* Class.__str__() (and Class.__repr__) */
static KrkValue _class_to_str(int argc, KrkValue argv[]) {
	char * tmp = malloc(sizeof("<type ''>") + AS_CLASS(argv[0])->name->length);
	size_t l = sprintf(tmp, "<type '%s'>", AS_CLASS(argv[0])->name->chars);
	KrkString * out = krk_copyString(tmp,l);
	free(tmp);
	return OBJECT_VAL(out);
}

/**
 * isinstance(obj,Class)
 *
 * Searches from type(obj) up the inheritence tree to see if obj
 * is an eventual descendant of Class. Unless someone made a new
 * type and didn't inherit from object(), everything is eventually
 * an object - even basic types like INTEGERs and FLOATINGs.
 */
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

/**
 * globals()
 *
 * Returns a dict of names -> values for all the globals.
 */
static KrkValue krk_globals(int argc, KrkValue argv[]) {
	/* Make a new empty dict */
	KrkValue dict = krk_dict_of(0, NULL);
	krk_push(dict);
	/* Get its internal table */
	KrkValue _dict_internal;
	krk_tableGet(&AS_INSTANCE(dict)->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
	/* Copy the globals table into it */
	krk_tableAddAll(&vm.globals, &AS_CLASS(_dict_internal)->methods);
	krk_pop();

	return dict;
}

static int checkArgumentCount(KrkClosure * closure, int argCount) {
	int minArgs = closure->function->requiredArgs;
	int maxArgs = minArgs + closure->function->keywordArgs;
	if (argCount < minArgs || argCount > maxArgs) {
		krk_runtimeError(vm.exceptions.argumentError, "%s() takes %s %d argument%s (%d given)",
		closure->function->name ? closure->function->name->chars : "<unnamed function>",
		(minArgs == maxArgs) ? "exactly" : (argCount < minArgs ? "at least" : "at most"),
		(argCount < minArgs) ? minArgs : maxArgs,
		((argCount < minArgs) ? minArgs : maxArgs) == 1 ? "" : "s",
		argCount);
		return 0;
	}
	return 1;
}

static void multipleDefs(KrkClosure * closure, int destination) {
	krk_runtimeError(vm.exceptions.typeError, "%s() got multiple values for argument '%s'",
		closure->function->name ? closure->function->name->chars : "<unnamed function>",
		(destination < closure->function->requiredArgs ? AS_CSTRING(closure->function->requiredArgNames.values[destination]) :
			AS_CSTRING(closure->function->keywordArgNames.values[destination - closure->function->requiredArgs])));
}

/**
 * Call a managed method.
 * Takes care of argument count checking, default argument filling,
 * sets up a new call frame, and then resumes the VM to run the function.
 *
 * Methods are called with their receivers on the stack as the first argument.
 * Non-methods are called with themselves on the stack before the first argument.
 * `extra` is passed by `callValue` to tell us which case we have, and thus
 * where we need to restore the stack to when we return from this call.
 */
static int call(KrkClosure * closure, int argCount, int extra) {
	KrkValue * startOfPositionals = &vm.stackTop[-argCount];
	size_t potentialPositionalArgs = closure->function->requiredArgs + closure->function->keywordArgs;
	size_t totalArguments = closure->function->requiredArgs + closure->function->keywordArgs + closure->function->collectsArguments + closure->function->collectsKeywords;
	size_t offsetOfExtraArgs = closure->function->requiredArgs + closure->function->keywordArgs;
	size_t offsetOfExtraKeys = offsetOfExtraArgs + closure->function->collectsArguments;
	size_t argCountX = argCount;

	if (argCount && IS_KWARGS(vm.stackTop[-1])) {
		/**
		 * Process keyword arguments.
		 * First, we make sure there is enough space on the stack to fit all of
		 * the potential arguments to this function. We need to call it with
		 * all of its arguments - positional and keyword - ready to go, even
		 * if they weren't specified.
		 *
		 * Then we go through all of the kwargs and figure out where they go,
		 * building a table at the top of the stack of final offsets and values.
		 *
		 * Then we clear through all of the spaces that were previously
		 * kwarg name/value pairs and replace them with a sentinel value.
		 *
		 * Then we go through our table and place values into their destination
		 * spots. If we find that something is already there (because it's not
		 * the expected sentinel value), we raise a TypeError indicating a
		 * duplicate argument.
		 *
		 * Finally, we do one last pass to see if any of the sentinel values
		 * indicating missing positional arguments is still there and raise
		 * another TypeError to indicate missing required arguments.
		 *
		 * At this point we can reset the stack head and continue to the actual
		 * call with all of the arguments, including the defaults, in the right
		 * place for the function to pull them as locals.
		 */
		long kwargsCount = AS_INTEGER(vm.stackTop[-1]);
		krk_pop(); /* Pop the arg counter */
		argCount--;
		size_t existingPositionalArgs = argCount - kwargsCount * 2;
		int found = 0;
		int extraKwargs = 0;
		intptr_t positionalsOffset = &vm.stackTop[-argCount] - vm.stack;
		intptr_t endOffset = &vm.stackTop[-kwargsCount * 2] - vm.stack;

		for (size_t availableSlots = argCount; availableSlots < (totalArguments); ++availableSlots) {
			krk_push(KWARGS_VAL(0)); /* Make sure we definitely have enough space */
		}

		/* Expand the stack a bunch to make sure we have space */
		for (int i = 0; i < argCount * 2; ++i) {
			krk_push(KWARGS_VAL(0));
		}
		for (int i = 0; i < argCount * 2; ++i) {
			krk_pop();
		}

		/* We may have moved the stack, recalculate positions. */
		startOfPositionals = vm.stack + positionalsOffset;
		KrkValue * endOfPositionals = vm.stack + endOffset;
		KrkValue * startOfExtras = vm.stackTop;
		for (long i = 0; i < kwargsCount; ++i) {
			KrkValue name = endOfPositionals[i*2];
			KrkValue value = endOfPositionals[i*2+1];
			if (IS_KWARGS(name)) {
				krk_push(name);
				krk_push(value);
				found++;
				goto _finishArg;
			}
			/* First, see if it's a positional arg. */
			for (int j = 0; j < (int)closure->function->requiredArgs; ++j) {
				if (krk_valuesEqual(name, closure->function->requiredArgNames.values[j])) {
					krk_push(INTEGER_VAL(j));
					krk_push(value);
					found++;
					goto _finishArg;
				}
			}
			/* See if it's a keyword arg. */
			for (int j = 0; j < (int)closure->function->keywordArgs; ++j) {
				if (krk_valuesEqual(name, closure->function->keywordArgNames.values[j])) {
					krk_push(INTEGER_VAL(j + closure->function->requiredArgs));
					krk_push(value);
					found++;
					goto _finishArg;
				}
			}
			/* If we got to this point, it's not a recognized argument for this function. */
			if (closure->function->collectsKeywords) {
				krk_push(name);
				krk_push(value);
				found++;
				extraKwargs++;
				continue;
			}
			krk_runtimeError(vm.exceptions.typeError, "%s() got an unexpected keyword argument '%s'",
				closure->function->name ? closure->function->name->chars : "<unnamed function>",
				AS_CSTRING(name));
			return 0;
_finishArg:
			continue;
		}

		size_t destination = existingPositionalArgs;
		for (long i = 0; i < found; ++i) {
			/* Check for specials */
			KrkValue name = startOfExtras[i*2];
			KrkValue value = startOfExtras[i*2+1];
			if (IS_KWARGS(name)) {
				if (AS_INTEGER(name) == LONG_MAX-1) {
					if (!IS_INSTANCE(value) || !AS_INSTANCE(value)->_internal || !AS_INSTANCE(value)->_internal->type == OBJ_FUNCTION) {
						krk_runtimeError(vm.exceptions.typeError, "*expresssion value is not a list.");
						return 0;
					}
					KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(value)->_internal);
					for (size_t i = 0; i < AS_LIST(_list_internal)->count; ++i) {
						startOfPositionals[destination] = AS_LIST(_list_internal)->values[i];
						destination++;
					}
					startOfExtras[i*2] = KWARGS_VAL(LONG_MAX-3);
				} else if (AS_INTEGER(name) == LONG_MAX) {
					startOfPositionals[destination] = value;
					destination++;
					startOfExtras[i*2] = KWARGS_VAL(LONG_MAX-3);
				}
			}
		}

		if (destination > potentialPositionalArgs) {
			if (!closure->function->collectsArguments) {
				checkArgumentCount(closure,destination);
				return 0;
			}
			krk_push(NONE_VAL()); krk_push(NONE_VAL()); krk_pop(); krk_pop();
			startOfPositionals[offsetOfExtraArgs] = krk_list_of(destination - potentialPositionalArgs,
				&startOfPositionals[potentialPositionalArgs]);
			destination = potentialPositionalArgs + 1;
		}

		for (long clearSlots = destination; clearSlots < startOfExtras - startOfPositionals; ++clearSlots) {
			startOfPositionals[clearSlots] = KWARGS_VAL(0);
		}

		for (int i = 0; i < found; ++i) {
			if (IS_INTEGER(startOfExtras[i*2])) {
				int destination = AS_INTEGER(startOfExtras[i*2]);
				if (!IS_KWARGS(startOfPositionals[destination])) {
					multipleDefs(closure, destination);
					return 0;
				}
				startOfPositionals[destination] = startOfExtras[i*2+1];
			} else if (IS_STRING(startOfExtras[i*2])) {
				krk_push(startOfExtras[i*2]);
				krk_push(startOfExtras[i*2+1]);
			} else if (IS_KWARGS(startOfExtras[i*2])) {
				if (AS_INTEGER(startOfExtras[i*2]) == LONG_MAX-2) {
					KrkValue _dict_internal;
					if (!IS_INSTANCE(startOfExtras[i*2+1]) || !krk_tableGet(&AS_INSTANCE(startOfExtras[i*2+1])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal)) {
						krk_runtimeError(vm.exceptions.typeError, "**expresssion value is not a dict.");
						return 0;
					}
					for (size_t j = 0; j < AS_DICT(_dict_internal)->capacity; ++j) {
						KrkTableEntry entry = AS_DICT(_dict_internal)->entries[j];
						if (entry.key.type == VAL_NONE) continue;
						KrkValue name = entry.key;
						KrkValue value = entry.value;
						for (int j = 0; j < (int)closure->function->requiredArgNames.count; ++j) {
							if (krk_valuesEqual(name, closure->function->requiredArgNames.values[j])) {
								int destination = j;
								if (!IS_KWARGS(startOfPositionals[destination])) {
									multipleDefs(closure, destination);
								}
								startOfPositionals[destination] = value;
								goto _finishDictEntry;
							}
						}
						/* See if it's a keyword arg. */
						for (int j = 0; j < (int)closure->function->keywordArgNames.count; ++j) {
							if (krk_valuesEqual(name, closure->function->keywordArgNames.values[j])) {
								int destination = j + closure->function->requiredArgs;
								if (!IS_KWARGS(startOfPositionals[destination])) {
									multipleDefs(closure, destination);
								}
								startOfPositionals[destination] = value;
								goto _finishDictEntry;
							}
						}
						krk_push(name);
						krk_push(value);
						extraKwargs++;
						_finishDictEntry: continue;
					}
				}
			} else {
				dumpStack(&vm.frames[vm.frameCount-1]);
				krk_runtimeError(vm.exceptions.typeError, "Internal error? Item at index %d from %d found is %s", i*2, found, krk_typeName(startOfExtras[i*2]));
				return 0;
			}
		}
		if (extraKwargs) {
			if (!closure->function->collectsKeywords) {
				krk_runtimeError(vm.exceptions.typeError, "%s() got an unexpected keyword argument '%s'",
					closure->function->name ? closure->function->name->chars : "<unnamed function>",
					AS_CSTRING(startOfExtras[found*2]));
			}
			krk_push(NONE_VAL()); krk_push(NONE_VAL()); krk_pop(); krk_pop();
			startOfPositionals[offsetOfExtraKeys] = krk_dict_of(extraKwargs*2,&startOfExtras[found*2]);
		}
		long clearSlots;
		for (clearSlots = destination; clearSlots < closure->function->requiredArgs; ++clearSlots) {
			if (IS_KWARGS(startOfPositionals[clearSlots])) {
				krk_runtimeError(vm.exceptions.typeError, "%s() missing required positional argument: '%s'",
					closure->function->name ? closure->function->name->chars : "<unnamed function>",
					AS_CSTRING(closure->function->requiredArgNames.values[clearSlots]));
				return 0;
			}
		}
		argCount = totalArguments;
		argCountX = argCount - (closure->function->collectsArguments + closure->function->collectsKeywords);
		while (vm.stackTop > startOfPositionals + argCount) krk_pop();
	} else {
		/* We can't have had any kwargs. */
		if ((size_t)argCount > potentialPositionalArgs && closure->function->collectsArguments) {
			krk_push(NONE_VAL()); krk_push(NONE_VAL()); krk_pop(); krk_pop();
			startOfPositionals[offsetOfExtraArgs] = krk_list_of(argCount - potentialPositionalArgs,
				&startOfPositionals[potentialPositionalArgs]);
			argCount = closure->function->requiredArgs + 1;
			argCountX = argCount - 1;
			while (vm.stackTop > startOfPositionals + argCount) krk_pop();
		}
	}
	if (!checkArgumentCount(closure, argCountX)) {
		return 0;
	}
	while (argCount < (int)totalArguments) {
		krk_push(KWARGS_VAL(0));
		argCount++;
	}
	if (vm.frameCount == FRAMES_MAX) {
		krk_runtimeError(vm.exceptions.baseException, "Too many call frames.");
		return 0;
	}
	CallFrame * frame = &vm.frames[vm.frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = (vm.stackTop - argCount) - vm.stack;
	frame->outSlots = (vm.stackTop - argCount - extra) - vm.stack;
	return 1;
}

/**
 * Call a callable.
 *
 *   For native methods, the result is available "immediately" upon return
 *   and the return value is set to 2 to indicate this - just krk_pop()
 *   to get the result. If an exception is thrown during a native method call,
 *   callValue will return 0 and the VM should be allowed to handle the exception.
 *
 *   For managed code, the VM needs to be resumed. Returns 1 to indicate this.
 *   If you want a result in a native method, call `krk_runNext()` and the
 *   result will be returned directly from that function.
 *
 *   Works for closures, classes, natives, and bound methods.
 *   If called with a non-callable, raises TypeError; this includes
 *   attempts to call a Class with no __init__ while using arguments.
 *
 *   If callValue returns 0, the VM should already be in the exception state
 *   and it is not necessary to raise another exception.
 *
 *   TODO: Instances with __call__ method.
 */
int krk_callValue(KrkValue callee, int argCount, int extra) {
	if (IS_OBJECT(callee)) {
		switch (OBJECT_TYPE(callee)) {
			case OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount, extra);
			case OBJ_NATIVE: {
				NativeFnKw native = (NativeFnKw)AS_NATIVE(callee);
				int hasKw = 0;
				if (argCount && IS_KWARGS(vm.stackTop[-1])) {
					long count = AS_INTEGER(vm.stackTop[-1]);
					for (long i = 0; i < count; ++i) {
						if (IS_KWARGS(vm.stackTop[-1 - count * 2 + i * 2])) {
							krk_runtimeError(vm.exceptions.typeError,"Unsupported use of argument expansion in native function call.");
							return 0;
						}
					}
					/* Dict it all up */
					*(vm.stackTop - count * 2 - 1) = krk_dict_of(count * 2, (vm.stackTop - count * 2 - 1));
					vm.stackTop = vm.stackTop - count * 2;
					argCount -= count * 2;
					hasKw = 1;
				}
				KrkValue * stackCopy = malloc(argCount * sizeof(KrkValue));
				memcpy(stackCopy, vm.stackTop - argCount, argCount * sizeof(KrkValue));
				KrkValue result = native(argCount, stackCopy, hasKw);
				free(stackCopy);
				if (vm.stackTop == vm.stack) {
					/* Runtime error returned from native method */
					return 0;
				}
				vm.stackTop -= argCount + extra;
				krk_push(result);
				return 2;
			}
			case OBJ_INSTANCE: {
				KrkClass * _class = AS_INSTANCE(callee)->_class;
				KrkValue callFunction;
				if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_CALL], &callFunction)) {
					return krk_callValue(callFunction, argCount + 1, 0);
				} else {
					krk_runtimeError(vm.exceptions.typeError, "Attempted to call non-callable type: %s", krk_typeName(callee));
					return 0;
				}
			}
			case OBJ_CLASS: {
				KrkClass * _class = AS_CLASS(callee);
				vm.stackTop[-argCount - 1] = OBJECT_VAL(krk_newInstance(_class));
				KrkValue initializer;
				if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_INIT], &initializer)) {
					return krk_callValue(initializer, argCount + 1, 0);
				} else if (argCount != 0) {
					krk_runtimeError(vm.exceptions.attributeError, "Class does not have an __init__ but arguments were passed to initializer: %d", argCount);
					return 0;
				}
				return 1;
			}
			case OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				vm.stackTop[-argCount - 1] = bound->receiver;
				if (!bound->method) {
					krk_runtimeError(vm.exceptions.argumentError, "Attempted to call a method binding with no attached callable (did you forget to return something from a method decorator?)");
					return 0;
				}
				return krk_callValue(OBJECT_VAL(bound->method), argCount + 1, 0);
			}
			default:
				break;
		}
	}
	krk_runtimeError(vm.exceptions.typeError, "Attempted to call non-callable type: %s", krk_typeName(callee));
	return 0;
}

/**
 * Takes care of runnext/pop
 */
KrkValue krk_callSimple(KrkValue value, int argCount, int isMethod) {
	int result = krk_callValue(value, argCount, isMethod);
	if (result == 2) {
		return krk_pop();
	} else if (result == 1) {
		return krk_runNext();
	}
	krk_runtimeError(vm.exceptions.typeError, "Invalid internal method call.");
	return NONE_VAL();
}

/**
 * Attach a method call to its callee and return a BoundMethod.
 * Works for managed and native method calls.
 */
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

/**
 * Capture upvalues and mark them as open. Called upon closure creation to
 * mark stack slots used by a function.
 */
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

/**
 * Close upvalues by moving them out of the stack and into the heap.
 * Their location attribute is set to -1 to indicate they now live on the heap.
 */
static void closeUpvalues(int last) {
	while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
		KrkUpvalue * upvalue = vm.openUpvalues;
		upvalue->closed = vm.stack[upvalue->location];
		upvalue->location = -1;
		vm.openUpvalues = upvalue->next;
	}
}

/**
 * Attach an object to a table.
 *
 * Generally used to attach classes or objects to the globals table, or to
 * a native module's export object.
 */
void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(OBJECT_VAL(obj));
	krk_tableSet(table, krk_peek(1), krk_peek(0));
	krk_pop();
	krk_pop();
}

/**
 * Same as above, but the object has already been wrapped in a value.
 */
void krk_attachNamedValue(KrkTable * table, const char name[], KrkValue obj) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(obj);
	krk_tableSet(table, krk_peek(1), krk_peek(0));
	krk_pop();
	krk_pop();
}

/**
 * Exception.__init__(arg)
 */
static KrkValue krk_initException(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	if (argc > 0) {
		krk_attachNamedValue(&self->fields, "arg", argv[1]);
	} else {
		krk_attachNamedValue(&self->fields, "arg", OBJECT_VAL(S("")));
	}

	return argv[0];
}

static KrkValue _string_init(int argc, KrkValue argv[]) {
	/* Ignore argument which would have been an instance */
	if (argc < 2) {
		return OBJECT_VAL(S(""));
	}
	if (argc > 2) {
		krk_runtimeError(vm.exceptions.argumentError, "str() takes 1 argument");
		return NONE_VAL();
	}
	if (IS_STRING(argv[1])) return argv[1]; /* strings are immutable, so we can just return the arg */
	/* Find the type of arg */
	krk_push(argv[1]);
	if (!AS_CLASS(krk_typeOf(1,&argv[1]))->_tostr) {
		krk_runtimeError(vm.exceptions.typeError, "Can not convert %s to str", krk_typeName(argv[1]));
		return NONE_VAL();
	}
	return krk_callSimple(OBJECT_VAL(AS_CLASS(krk_typeOf(1,&argv[1]))->_tostr), 1, 0);
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

/** native method that returns its first arg; useful for int(INT), etc. */
static KrkValue _noop(int argc, KrkValue argv[]) {
	return argv[0];
}

/* float.__int__() */
static KrkValue _floating_to_int(int argc, KrkValue argv[]) {
	return INTEGER_VAL((long)AS_FLOATING(argv[0]));
}

/* int.__float__() */
static KrkValue _int_to_floating(int argc, KrkValue argv[]) {
	return FLOATING_VAL((double)AS_INTEGER(argv[0]));
}

/* int.__chr__() */
static KrkValue _int_to_char(int argc, KrkValue argv[]) {
	char tmp[2] = {AS_INTEGER(argv[0]), 0};
	return OBJECT_VAL(krk_copyString(tmp,1));
}

/* str.__ord__() */
static KrkValue _char_to_int(int argc, KrkValue argv[]) {
	if (AS_STRING(argv[0])->length != 1) {
		krk_runtimeError(vm.exceptions.typeError, "ord() expected a character, but string of length %d found",
			AS_STRING(argv[0])->length);
		return NONE_VAL();
	}

	/* TODO unicode strings? Interpret as UTF-8 and return codepoint? */
	return INTEGER_VAL(AS_CSTRING(argv[0])[0]);
}

static KrkValue _print(int argc, KrkValue argv[], int hasKw) {
	KrkValue sepVal, endVal;
	char * sep = " ";
	char * end = "\n";
	if (hasKw) {
		argc--;
		KrkValue _dict_internal;
		krk_tableGet(&AS_INSTANCE(argv[argc])->fields, vm.specialMethodNames[METHOD_DICT_INT], &_dict_internal);
		if (krk_tableGet(AS_DICT(_dict_internal), OBJECT_VAL(S("sep")), &sepVal)) {
			if (!IS_STRING(sepVal)) {
				krk_runtimeError(vm.exceptions.typeError, "'sep' should be a string, not '%s'", krk_typeName(sepVal));
				return NONE_VAL();
			}
			sep = AS_CSTRING(sepVal);
		}
		if (krk_tableGet(AS_DICT(_dict_internal), OBJECT_VAL(S("end")), &endVal)) {
			if (!IS_STRING(endVal)) {
				krk_runtimeError(vm.exceptions.typeError, "'end' should be a string, not '%s'", krk_typeName(endVal));
				return NONE_VAL();
			}
			end = AS_CSTRING(endVal);
		}
	}
	for (int i = 0; i < argc; ++i) {
		KrkValue printable = argv[i];
		if (IS_STRING(printable)) { /* krk_printValue runs repr */
			fprintf(stdout, "%s", AS_CSTRING(printable));
		} else {
			krk_printValue(stdout, printable);
		}
		fprintf(stdout, "%s", (i == argc - 1) ? end : sep);
	}
	return NONE_VAL();
}

/* str.__len__() */
static KrkValue _string_length(int argc, KrkValue argv[]) {
	if (argc != 1) {
		krk_runtimeError(vm.exceptions.attributeError,"Unexpected arguments to str.__len__()");
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0])) {
		return NONE_VAL();
	}
	return INTEGER_VAL(AS_STRING(argv[0])->length);
}

/* str.__set__(ind,val) - this is invalid, throw a nicer error than 'field does not exist'. */
static KrkValue _strings_are_immutable(int argc, KrkValue argv[]) {
	krk_runtimeError(vm.exceptions.typeError, "Strings are not mutable.");
	return NONE_VAL();
}

/**
 * str.__getslice__(start,end)
 *
 * Unlike in Python, we actually handle negative values here rather than
 * somewhere else? I'm not even sure where Python does do it, but a quick
 * says not if you call __getslice__ directly...
 */
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

/* str.__int__(base=10) */
static KrkValue _string_to_int(int argc, KrkValue argv[]) {
	if (argc < 1 || argc > 2 || !IS_STRING(argv[0])) return NONE_VAL();
	int base = (argc == 1) ? 10 : (int)AS_INTEGER(argv[1]);
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

/* str.__float__() */
static KrkValue _string_to_float(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_STRING(argv[0])) return NONE_VAL();
	return FLOATING_VAL(strtod(AS_CSTRING(argv[0]),NULL));
}

static KrkValue _float_init(int argc, KrkValue argv[]) {
	if (argc < 1) return FLOATING_VAL(0.0);
	if (argc > 2) {
		krk_runtimeError(vm.exceptions.argumentError, "float() takes at most 1 argument");
		return NONE_VAL();
	}
	if (IS_STRING(argv[1])) return _string_to_float(1,&argv[1]);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));
	krk_runtimeError(vm.exceptions.typeError, "float() argument must be a string or a number, not '%s'", krk_typeName(argv[1]));
	return NONE_VAL();
}

/* str.__get__(index) */
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
	KrkString * me = AS_STRING(argv[0]);
	int asInt = AS_INTEGER(argv[1]);
	if (asInt < 0) asInt += (int)AS_STRING(argv[0])->length;
	if (asInt < 0 || asInt >= (int)AS_STRING(argv[0])->length) {
		krk_runtimeError(vm.exceptions.indexError, "String index out of range: %d", asInt);
		return NONE_VAL();
	}
	return OBJECT_VAL(krk_copyString((char[]){me->chars[asInt]},1));
}

#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = GROW_ARRAY(char, stringBytes, old, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)
#define AT_END() (self->length == 0 || i == self->length - 1)

/* str.format(**kwargs) */
static KrkValue _string_format(int argc, KrkValue argv[], int hasKw) {
	if (!IS_STRING(argv[0])) return NONE_VAL();
	KrkString * self = AS_STRING(argv[0]);
	KrkValue kwargs = NONE_VAL();
	if (hasKw) {
		argc--; /* last arg is the keyword dictionary */
		krk_tableGet(&AS_INSTANCE(argv[argc])->fields, vm.specialMethodNames[METHOD_DICT_INT], &kwargs);
	}

	/* Read through `self` until we find a field specifier. */
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = 0;

	int counterOffset = 0;
	char * erroneousField = NULL;
	int erroneousIndex = -1;
	const char * errorStr = "";

	char * workSpace = strdup(self->chars);
	char * c = workSpace;
	for (size_t i = 0; i < self->length; i++, c++) {
		if (*c == '{') {
			if (!AT_END() && c[1] == '{') {
				PUSH_CHAR('{');
				i++; c++; /* Skip both */
				continue;
			} else {
				/* Start field specifier */
				i++; c++; /* Skip the { */
				char * fieldStart = c;
				char * fieldStop = NULL;
				for (; i < self->length; i++, c++) {
					if (*c == '}') {
						fieldStop = c;
						break;
					}
				}
				if (!fieldStop) {
					errorStr = "Unclosed { found.";
					goto _formatError;
				}
				size_t fieldLength = fieldStop - fieldStart;
				*fieldStop = '\0';
				/* fieldStart is now a nice little C string... */
				int isDigits = 1;
				for (char * field = fieldStart; *field; ++field) {
					if (!(*field >= '0' && *field <= '9')) {
						isDigits = 0;
						break;
					}
				}
				KrkValue value;
				if (isDigits) {
					/* Must be positional */
					int positionalOffset;
					if (fieldLength == 0) {
						positionalOffset = counterOffset++;
					} else if (counterOffset) {
						goto _formatSwitchedNumbering;
					} else {
						positionalOffset = atoi(fieldStart);
					}
					if (positionalOffset >= argc - 1) {
						erroneousIndex = positionalOffset;
						goto _formatOutOfRange;
					}
					value = argv[1 + positionalOffset];
				} else if (hasKw) {
					KrkValue fieldAsString = OBJECT_VAL(krk_copyString(fieldStart, fieldLength));
					krk_push(fieldAsString);
					if (!krk_tableGet(AS_DICT(kwargs), fieldAsString, &value)) {
						erroneousField = fieldStart;
						goto _formatKeyError;
					}
					krk_pop(); /* fieldAsString */
				} else {
					erroneousField = fieldStart;
					goto _formatKeyError;
				}
				KrkValue asString;
				if (IS_STRING(value)) {
					asString = value;
				} else {
					krk_push(value);
					if (!krk_bindMethod(AS_CLASS(krk_typeOf(1,(KrkValue[]){value})),
						AS_STRING(vm.specialMethodNames[METHOD_STR]))) {
						errorStr = "Failed to convert field to string.";
						goto _formatError;
					}
					asString = krk_callSimple(krk_peek(0), 0, 1);
					if (!IS_STRING(asString)) goto _freeAndDone;
				}
				krk_push(asString);
				for (size_t i = 0; i < AS_STRING(asString)->length; ++i) {
					PUSH_CHAR(AS_CSTRING(asString)[i]);
				}
				krk_pop();
			}
		} else if (*c == '}') {
			if (!AT_END() && c[1] == '}') {
				PUSH_CHAR('}');
				i++; c++; /* Skip both */
				continue;
			} else {
				errorStr = "Single } found.";
				goto _formatError;
			}
		} else {
			PUSH_CHAR(*c);
		}
	}

	KrkValue out = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	free(workSpace);
	FREE_ARRAY(char,stringBytes,stringCapacity);
	return out;

_formatError:
	krk_runtimeError(vm.exceptions.typeError, "Error parsing format string: %s", errorStr);
	goto _freeAndDone;

_formatSwitchedNumbering:
	krk_runtimeError(vm.exceptions.valueError, "Can not switch from automatic indexing to manual indexing");
	goto _freeAndDone;

_formatOutOfRange:
	krk_runtimeError(vm.exceptions.indexError, "Positional index out of range: %d", erroneousIndex);
	goto _freeAndDone;

_formatKeyError:
	/* which one? */
	krk_runtimeError(vm.exceptions.keyError, "'%s'", erroneousField);
	goto _freeAndDone;

_freeAndDone:
	FREE_ARRAY(char,stringBytes,stringCapacity);
	free(workSpace);
	return NONE_VAL();
}

/* str.join(list) */
static KrkValue _string_join(int argc, KrkValue argv[], int hasKw) {
	if (!IS_STRING(argv[0])) return NONE_VAL();
	KrkString * self = AS_STRING(argv[0]);
	if (hasKw) {
		krk_runtimeError(vm.exceptions.argumentError, "str.join() does not take keyword arguments");
		return NONE_VAL();
	}

	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "str.join(): expected exactly one argument");
		return NONE_VAL();
	}

	/* TODO: Support any object with an __iter__ - kinda need an internal method to do that well. */
	if (!IS_INSTANCE(argv[1]) || !AS_INSTANCE(argv[1])->_internal || !AS_INSTANCE(argv[1])->_internal->type == OBJ_FUNCTION) {
		krk_runtimeError(vm.exceptions.typeError, "*expresssion value is not a list.");
		return NONE_VAL();
	}

	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(argv[1])->_internal);

	const char * errorStr = NULL;

	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = 0;

	for (size_t i = 0; i < AS_LIST(_list_internal)->count; ++i) {
		KrkValue value = AS_LIST(_list_internal)->values[i];
		if (!IS_STRING(AS_LIST(_list_internal)->values[i])) {
			errorStr = krk_typeName(value);
			goto _expectedString;
		}
		krk_push(value);
		if (i > 0) {
			for (size_t j = 0; j < self->length; ++j) {
				PUSH_CHAR(self->chars[j]);
			}
		}
		for (size_t j = 0; j < AS_STRING(value)->length; ++j) {
			PUSH_CHAR(AS_STRING(value)->chars[j]);
		}
		krk_pop();
	}

	KrkValue out = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	FREE_ARRAY(char,stringBytes,stringCapacity);
	return out;

_expectedString:
	krk_runtimeError(vm.exceptions.typeError, "Expected string, got %s.", errorStr);
	FREE_ARRAY(char,stringBytes,stringCapacity);
	return NONE_VAL();
}

static int isWhitespace(char c) {
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

static int substringMatch(const char * haystack, size_t haystackLen, const char * needle, size_t needleLength) {
	if (haystackLen < needleLength) return 0;
	for (size_t i = 0; i < needleLength; ++i) {
		if (haystack[i] != needle[i]) return 0;
	}
	return 1;
}

/* str.__contains__ */
static KrkValue _string_contains(int argc, KrkValue argv[]) {
	if (argc < 2) {
		krk_runtimeError(vm.exceptions.argumentError, "__contains__ expects an argument");
		return NONE_VAL();
	}
	if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < AS_STRING(argv[0])->length; ++i) {
		if (substringMatch(AS_CSTRING(argv[0]) + i, AS_STRING(argv[0])->length - i, AS_CSTRING(argv[1]), AS_STRING(argv[1])->length)) {
			return BOOLEAN_VAL(1);
		}
	}
	return BOOLEAN_VAL(0);
}

/* str.split() */
static KrkValue _string_split(int argc, KrkValue argv[], int hasKw) {
	if (!IS_STRING(argv[0])) return NONE_VAL();
	KrkString * self = AS_STRING(argv[0]);
	if (argc > 1) {
		if (!IS_STRING(argv[1])) {
			krk_runtimeError(vm.exceptions.typeError, "Expected separator to be a string");
			return NONE_VAL();
		} else if (AS_STRING(argv[1])->length == 0) {
			krk_runtimeError(vm.exceptions.valueError, "Empty separator");
			return NONE_VAL();
		}
		if (argc > 2 && !IS_INTEGER(argv[2])) {
			krk_runtimeError(vm.exceptions.typeError, "Expected maxsplit to be an integer.");
		} else if (argc > 2 && AS_INTEGER(argv[2]) == 0) {
			return argv[0];
		}
	}

	KrkValue myList = krk_list_of(0,NULL);
	krk_push(myList);
	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(myList)->_internal);

	size_t i = 0;
	char * c = self->chars;
	size_t count = 0;

	if (argc < 2) {
		while (i != self->length) {
			while (i != self->length && isWhitespace(*c)) {
				i++; c++;
			}
			if (i != self->length) {
				size_t stringCapacity = 0;
				size_t stringLength   = 0;
				char * stringBytes    = NULL;
				while (i != self->length && !isWhitespace(*c)) {
					PUSH_CHAR(*c);
					i++; c++;
				}
				KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
				krk_push(tmp);
				krk_writeValueArray(AS_LIST(_list_internal), tmp);
				krk_pop();
				FREE_ARRAY(char,stringBytes,stringCapacity);
				#if 0
				/* Need to parse kwargs to support this */
				if (argc > 2 && i != self->length && count >= (size_t)AS_INTEGER(argv[2])) {
					size_t stringCapacity = 0;
					size_t stringLength   = 0;
					char * stringBytes    = NULL;
					while (i != self->length) {
						PUSH_CHAR(*c);
						i++; c++;
					}
					krk_writeValueArray(AS_LIST(_list_internal), OBJECT_VAL(krk_copyString(stringBytes, stringLength)));
					if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
					break;
				}
				#endif
			}
		}
	} else {
		while (i != self->length) {
			size_t stringCapacity = 0;
			size_t stringLength   = 0;
			char * stringBytes    = NULL;
			while (i != self->length && !substringMatch(c, self->length - i, AS_STRING(argv[1])->chars, AS_STRING(argv[1])->length)) {
				PUSH_CHAR(*c);
				i++; c++;
			}
			KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
			krk_push(tmp);
			krk_writeValueArray(AS_LIST(_list_internal), tmp);
			krk_pop();
			if (substringMatch(c, self->length - i, AS_STRING(argv[1])->chars, AS_STRING(argv[1])->length)) {
				i += AS_STRING(argv[1])->length;
				c += AS_STRING(argv[1])->length;
				count++;
				if (argc > 2 && count == (size_t)AS_INTEGER(argv[2])) {
					size_t stringCapacity = 0;
					size_t stringLength   = 0;
					char * stringBytes    = NULL;
					while (i != self->length) {
						PUSH_CHAR(*c);
						i++; c++;
					}
					KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
					krk_push(tmp);
					krk_writeValueArray(AS_LIST(_list_internal), tmp);
					krk_pop();
					if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
					break;
				}
				if (i == self->length) {
					KrkValue tmp = OBJECT_VAL(S(""));
					krk_push(tmp);
					krk_writeValueArray(AS_LIST(_list_internal), tmp);
					krk_pop();
				}
			}
		}
	}

	krk_pop();
	return myList;
}
#undef PUSH_CHAR

static KrkValue _int_init(int argc, KrkValue argv[]) {
	if (argc < 2) return INTEGER_VAL(0);
	if (IS_INTEGER(argv[1])) return argv[1];
	if (IS_STRING(argv[1])) return _string_to_int(argc-1,&argv[1]);
	if (IS_FLOATING(argv[1])) return INTEGER_VAL(AS_FLOATING(argv[1]));
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_BOOLEAN(argv[1]));
	krk_runtimeError(vm.exceptions.typeError, "int() argument must be a string or a number, not '%s'", krk_typeName(argv[1]));
	return NONE_VAL();
}

/* function.__doc__ */
static KrkValue _closure_get_doc(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return NONE_VAL();
	return AS_CLOSURE(argv[0])->function->docstring ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->docstring) : NONE_VAL();
}

/* method.__doc__ */
static KrkValue _bound_get_doc(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_doc(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

/* Check for and return the name of a native function as a string object */
static KrkValue nativeFunctionName(KrkValue func) {
	const char * string = ((KrkNative*)AS_OBJECT(func))->name;
	size_t len = strlen(string);
	return OBJECT_VAL(krk_copyString(string,len));
}

/* function.__name__ */
static KrkValue _closure_get_name(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return nativeFunctionName(argv[0]);
	return AS_CLOSURE(argv[0])->function->name ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->name) : OBJECT_VAL(S(""));
}

/* method.__name__ */
static KrkValue _bound_get_name(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_name(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

/* function.__str__ / function.__repr__ */
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

/* method.__str__ / method.__repr__ */
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

/* function.__file__ */
static KrkValue _closure_get_file(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return OBJECT_VAL(S("<builtin>"));
	return AS_CLOSURE(argv[0])->function->chunk.filename ? OBJECT_VAL(AS_CLOSURE(argv[0])->function->chunk.filename) : OBJECT_VAL(S(""));
}

/* method.__file__ */
static KrkValue _bound_get_file(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_file(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

/**
 * object.__str__() / object.__repr__()
 *
 * Base method for all objects to implement __str__ and __repr__.
 * Generally converts to <instance of [TYPE]> and for actual object
 * types (functions, classes, instances, strings...) also adds the pointer
 * address of the object on the heap.
 *
 * Since all types have at least a pseudo-class that should eventually
 * inheret from object() and this is object.__str__ / object.__repr__,
 * all types should have a string representation available through
 * those methods.
 */
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

/**
 * str.__repr__()
 *
 * Strings are special because __str__ should do nothing but __repr__
 * should escape characters like quotes.
 */
static KrkValue _repr_str(int argc, KrkValue argv[]) {
	char * str = malloc(3 + AS_STRING(argv[0])->length * 2);
	char * tmp = str;
	*(tmp++) = '\'';
	for (char * c = AS_CSTRING(argv[0]); *c; ++c) {
		switch (*c) {
			/* XXX: Other non-printables should probably be escaped as well. */
			case '\n': *(tmp++) = '\\'; *(tmp++) = 'n'; break;
			case '\r': *(tmp++) = '\\'; *(tmp++) = 'r'; break;
			case '\t': *(tmp++) = '\\'; *(tmp++) = 't'; break;
			case '\'':  *(tmp++) = '\\'; *(tmp++) = '\''; break;
			case 27:   *(tmp++) = '\\'; *(tmp++) = '['; break;
			default:   *(tmp++) = *c; break;
		}
	}
	*(tmp++) = '\'';
	*(tmp++) = '\0';
	KrkString * out = krk_copyString(str, tmp-str-1);
	free(str);
	return OBJECT_VAL(out);
}

/**
 * int.__str__()
 *
 * Unlike Python, dot accessors are perfectly valid and work as you'd expect
 * them to in Kuroko, so we can do 123.__str__() and get the string "123".
 *
 * TODO: Implement format options here so we can get different widths,
 *       hex/octal/binary representations, etc.
 */
static KrkValue _int_to_str(int argc, KrkValue argv[]) {
	char tmp[100];
	size_t l = sprintf(tmp, "%ld", (long)AS_INTEGER(argv[0]));
	return OBJECT_VAL(krk_copyString(tmp, l));
}

/**
 * float.__str__()
 */
static KrkValue _float_to_str(int argc, KrkValue argv[]) {
	char tmp[100];
	size_t l = sprintf(tmp, "%g", AS_FLOATING(argv[0]));
	return OBJECT_VAL(krk_copyString(tmp, l));
}

/**
 * bool.__str__() -> "True" or "False"
 */
static KrkValue _bool_to_str(int argc, KrkValue argv[]) {
	return OBJECT_VAL((AS_BOOLEAN(argv[0]) ? S("True") : S("False")));
}

/**
 * Inverse of truthiness.
 *
 * None, False, and 0 are all "falsey", meaning they will trip JUMP_IF_FALSE
 * instructions / not trip JUMP_IF_TRUE instructions.
 *
 * Or in more managed code terms, `if None`, `if False`, and `if 0` are all
 * going to take the else branch.
 */
static int isFalsey(KrkValue value) {
	return IS_NONE(value) || (IS_BOOLEAN(value) && !AS_BOOLEAN(value)) ||
	       (IS_INTEGER(value) && !AS_INTEGER(value));
	/* Objects in the future: */
	/* IS_STRING && length == 0; IS_ARRAY && length == 0; IS_INSTANCE && __bool__ returns 0... */
}

static KrkValue _bool_init(int argc, KrkValue argv[]) {
	if (argc < 2) return BOOLEAN_VAL(0);
	if (argc > 2) {
		krk_runtimeError(vm.exceptions.argumentError, "bool() takes at most 1 argument");
		return NONE_VAL();
	}
	return BOOLEAN_VAL(isFalsey(argv[1]));
}

/**
 * None.__str__() -> "None"
 */
static KrkValue _none_to_str(int argc, KrkValue argv[]) {
	return OBJECT_VAL(S("None"));
}

static KrkValue _len(int argc, KrkValue argv[]) {
	if (argc != 1) {
		krk_runtimeError(vm.exceptions.argumentError, "len() takes exactly one argument");
		return NONE_VAL();
	}
	if (!IS_OBJECT(argv[0])) {
		krk_runtimeError(vm.exceptions.typeError, "object of type '%s' has no len()", krk_typeName(argv[0]));
		return NONE_VAL();
	}
	if (IS_STRING(argv[0])) return INTEGER_VAL(AS_STRING(argv[0])->length);
	krk_push(argv[0]);
	if (!krk_bindMethod(AS_CLASS(krk_typeOf(1,&argv[0])), AS_STRING(vm.specialMethodNames[METHOD_LEN]))) {
		krk_runtimeError(vm.exceptions.typeError, "object of type '%s' has no len()", krk_typeName(argv[0]));
		return NONE_VAL();
	}
	return krk_callSimple(krk_peek(0), 0, 1);
}

static KrkValue _repr(int argc, KrkValue argv[]) {
	if (argc != 1) {
		krk_runtimeError(vm.exceptions.argumentError, "repr() takes exactly one argument");
		return NONE_VAL();
	}
	krk_push(argv[0]);
	if (!krk_bindMethod(AS_CLASS(krk_typeOf(1,&argv[0])), AS_STRING(vm.specialMethodNames[METHOD_REPR]))) {
		krk_runtimeError(vm.exceptions.typeError, "internal error");
		return NONE_VAL();
	}
	return krk_callSimple(krk_peek(0), 0, 1);
}

static KrkValue _striter_init(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.striteratorClass) {
		krk_runtimeError(vm.exceptions.typeError, "Tried to call striterator.__init__() on something not a str iterator");
	}
	if (argc < 2 || !IS_STRING(argv[1])) {
		krk_runtimeError(vm.exceptions.argumentError, "Expected a str.");
	}
	KrkInstance * self = AS_INSTANCE(argv[0]);

	krk_push(argv[0]);
	krk_attachNamedValue(&self->fields, "s", argv[1]);
	krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(0));
	krk_pop();

	return argv[0];
}

static KrkValue _striter_call(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.striteratorClass) {
		krk_runtimeError(vm.exceptions.typeError, "Tried to call striterator.__call__() on something not a str iterator");
	}
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue _str;
	KrkValue _counter;
	const char * errorStr = NULL;

	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("s")), &_str)) {
		errorStr = "no str pointer";
		goto _corrupt;
	}
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("i")), &_counter)) {
		errorStr = "no index";
		goto _corrupt;
	}

	if ((size_t)AS_INTEGER(_counter) >= AS_STRING(_str)->length) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return OBJECT_VAL(krk_copyString(&AS_CSTRING(_str)[AS_INTEGER(_counter)],1));
	}

_corrupt:
	krk_runtimeError(vm.exceptions.typeError, "Corrupt str iterator: %s", errorStr);
	return NONE_VAL();
}

static KrkValue _str_iter(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.striteratorClass);

	krk_push(OBJECT_VAL(output));
	_striter_init(3, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();

	return OBJECT_VAL(output);
}

static KrkValue _listiter_init(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.listiteratorClass) {
		krk_runtimeError(vm.exceptions.typeError, "Tried to call listiterator.__init__() on something not a list iterator");
	}
	if (argc < 2 || !IS_INSTANCE(argv[1])) {
		krk_runtimeError(vm.exceptions.argumentError, "Expected a list.");
	}
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue _list = argv[1];

	krk_push(argv[0]);
	krk_attachNamedValue(&self->fields, "l", _list);
	krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(0));
	krk_pop();

	return argv[0];
}

static KrkValue _listiter_call(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.listiteratorClass) {
		krk_runtimeError(vm.exceptions.typeError, "Tried to call listiterator.__call__() on something not a list iterator");
	}
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue _list;
	KrkValue _counter;
	const char * errorStr = NULL;

	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("l")), &_list)) {
		errorStr = "no list pointer";
		goto _corrupt;
	}
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("i")), &_counter)) {
		errorStr = "no index";
		goto _corrupt;
	}

	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(_list)->_internal);

	if ((size_t)AS_INTEGER(_counter) >= AS_LIST(_list_internal)->count) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return AS_LIST(_list_internal)->values[AS_INTEGER(_counter)];
	}

_corrupt:
	krk_runtimeError(vm.exceptions.typeError, "Corrupt list iterator: %s", errorStr);
	return NONE_VAL();
}

static KrkValue _range_init(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	if (argc < 2 || argc > 3) {
		krk_runtimeError(vm.exceptions.argumentError, "range expected at least 1 and and at most 2 arguments");
		return NONE_VAL();
	}
	KrkValue min = INTEGER_VAL(0);
	KrkValue max;
	if (argc == 2) {
		max = argv[1];
	} else {
		min = argv[1];
		max = argv[2];
	}
	if (!IS_INTEGER(min)) {
		krk_runtimeError(vm.exceptions.typeError, "range: expected int, but got '%s'", krk_typeName(min));
		return NONE_VAL();
	}
	if (!IS_INTEGER(max)) {
		krk_runtimeError(vm.exceptions.typeError, "range: expected int, but got '%s'", krk_typeName(max));
		return NONE_VAL();
	}

	/* Add them to ourselves */
	krk_push(argv[0]);
	krk_attachNamedValue(&self->fields, "min", min);
	krk_attachNamedValue(&self->fields, "max", max);
	krk_pop();

	return argv[0];
}

static KrkValue _range_repr(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	KrkValue min, max;
	krk_tableGet(&self->fields, OBJECT_VAL(S("min")), &min);
	krk_tableGet(&self->fields, OBJECT_VAL(S("max")), &max);

	krk_push(OBJECT_VAL(S("range({},{})")));
	KrkValue output = _string_format(3, (KrkValue[]){krk_peek(0), min, max}, 0);
	krk_pop();
	return output;
}

static KrkValue _rangeiterator_init(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	krk_push(argv[0]);
	krk_attachNamedValue(&self->fields, "i", argv[1]);
	krk_attachNamedValue(&self->fields, "m", argv[2]);
	krk_pop();
	return argv[0];
}

static KrkValue _rangeiterator_call(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue i, m;
	krk_tableGet(&self->fields, OBJECT_VAL(S("i")), &i);
	krk_tableGet(&self->fields, OBJECT_VAL(S("m")), &m);
	if (AS_INTEGER(i) >= AS_INTEGER(m)) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(i)+1));
		return i;
	}
}

static KrkValue _range_iter(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue min, max;
	krk_tableGet(&self->fields, OBJECT_VAL(S("min")), &min);
	krk_tableGet(&self->fields, OBJECT_VAL(S("max")), &max);

	KrkInstance * output = krk_newInstance(vm.baseClasses.rangeiteratorClass);

	krk_push(OBJECT_VAL(output));
	_rangeiterator_init(3, (KrkValue[]){krk_peek(0), min, max});
	krk_pop();

	return OBJECT_VAL(output);
}


void krk_initVM(int flags) {
	vm.flags = flags;
	KRK_PAUSE_GC();

	krk_resetStack();
	vm.objects = NULL;
	vm.bytesAllocated = 0;
	vm.nextGC = 1024 * 1024;
	vm.grayCount = 0;
	vm.grayCapacity = 0;
	vm.grayStack = NULL;
	krk_initTable(&vm.globals);
	krk_initTable(&vm.strings);
	memset(vm.specialMethodNames,0,sizeof(vm.specialMethodNames));

	/* To make lookup faster, store these so we can don't have to keep boxing
	 * and unboxing, copying/hashing etc. */
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
	vm.specialMethodNames[METHOD_ORD]  = OBJECT_VAL(S("__ord__"));
	vm.specialMethodNames[METHOD_FLOAT]= OBJECT_VAL(S("__float__"));
	vm.specialMethodNames[METHOD_LEN]  = OBJECT_VAL(S("__len__"));
	vm.specialMethodNames[METHOD_DOC]  = OBJECT_VAL(S("__doc__"));
	vm.specialMethodNames[METHOD_BASE] = OBJECT_VAL(S("__base__"));
	vm.specialMethodNames[METHOD_CALL] = OBJECT_VAL(S("__call__"));
	vm.specialMethodNames[METHOD_GETSLICE] = OBJECT_VAL(S("__getslice__"));
	vm.specialMethodNames[METHOD_LIST_INT] = OBJECT_VAL(S("__list"));
	vm.specialMethodNames[METHOD_DICT_INT] = OBJECT_VAL(S("__dict"));
	vm.specialMethodNames[METHOD_INREPR] = OBJECT_VAL(S("__inrepr"));

	/* Create built-in class `object` */
	vm.objectClass = krk_newClass(S("object"));
	krk_attachNamedObject(&vm.globals, "object", (KrkObj*)vm.objectClass);
	krk_defineNative(&vm.objectClass->methods, ":__class__", krk_typeOf);
	krk_defineNative(&vm.objectClass->methods, ".__dir__", krk_dirObject);
	krk_defineNative(&vm.objectClass->methods, ".__str__", _strBase);
	krk_defineNative(&vm.objectClass->methods, ".__repr__", _strBase); /* Override if necesary */
	krk_finalizeClass(vm.objectClass);

	/* Build a __builtins__ namespace for some extra functions. */
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
	ADD_EXCEPTION_CLASS(vm.exceptions.valueError, "ValueError", vm.exceptions.baseException);

	/* Build classes for basic types */
	ADD_BASE_CLASS(vm.baseClasses.typeClass, "type", vm.objectClass);
	krk_attachNamedObject(&vm.globals, "type", (KrkObj*)vm.baseClasses.typeClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__base__", krk_baseOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__file__", krk_fileOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__doc__", krk_docOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__name__", krk_nameOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__init__", _type_init);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__str__", _class_to_str);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__repr__", _class_to_str);
	krk_finalizeClass(vm.baseClasses.typeClass);
	ADD_BASE_CLASS(vm.baseClasses.intClass, "int", vm.objectClass);
	krk_attachNamedObject(&vm.globals, "int", (KrkObj*)vm.baseClasses.intClass);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__init__", _int_init);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__int__", _noop);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__float__", _int_to_floating);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__chr__", _int_to_char);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__str__", _int_to_str);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__repr__", _int_to_str);
	krk_finalizeClass(vm.baseClasses.intClass);
	ADD_BASE_CLASS(vm.baseClasses.floatClass, "float", vm.objectClass);
	krk_attachNamedObject(&vm.globals, "float", (KrkObj*)vm.baseClasses.floatClass);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__init__", _float_init);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__int__", _floating_to_int);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__float__", _noop);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__str__", _float_to_str);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__repr__", _float_to_str);
	krk_finalizeClass(vm.baseClasses.floatClass);
	ADD_BASE_CLASS(vm.baseClasses.boolClass, "bool", vm.objectClass);
	krk_attachNamedObject(&vm.globals, "bool", (KrkObj*)vm.baseClasses.boolClass);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__init__", _bool_init);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__str__", _bool_to_str);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__repr__", _bool_to_str);
	krk_finalizeClass(vm.baseClasses.boolClass);
	ADD_BASE_CLASS(vm.baseClasses.noneTypeClass, "NoneType", vm.objectClass);
	krk_defineNative(&vm.baseClasses.noneTypeClass->methods, ".__str__", _none_to_str);
	krk_defineNative(&vm.baseClasses.noneTypeClass->methods, ".__repr__", _none_to_str);
	krk_finalizeClass(vm.baseClasses.noneTypeClass);
	ADD_BASE_CLASS(vm.baseClasses.strClass, "str", vm.objectClass);
	krk_attachNamedObject(&vm.globals, "str", (KrkObj*)vm.baseClasses.strClass);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__init__", _string_init);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__str__", _noop);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__repr__", _repr_str);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__len__", _string_length);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__get__", _string_get);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__set__", _strings_are_immutable);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__int__", _string_to_int);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__float__", _string_to_float);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__getslice__", _string_get_slice);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__ord__", _char_to_int);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__contains__", _string_contains);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__iter__", _str_iter);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".format", _string_format);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".join", _string_join);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".split", _string_split);
	krk_finalizeClass(vm.baseClasses.strClass);
	ADD_BASE_CLASS(vm.baseClasses.functionClass, "function", vm.objectClass);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__str__", _closure_str);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__repr__", _closure_str);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__doc__", _closure_get_doc);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__name__", _closure_get_name);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__file__", _closure_get_file);
	krk_finalizeClass(vm.baseClasses.functionClass);
	ADD_BASE_CLASS(vm.baseClasses.methodClass, "method", vm.objectClass);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__str__", _bound_str);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__repr__", _bound_str);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__doc__", _bound_get_doc);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__name__", _bound_get_name);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__file__", _bound_get_file);
	krk_finalizeClass(vm.baseClasses.methodClass);

	/* Build global builtin functions. */
	krk_defineNative(&vm.globals, "listOf", krk_list_of); /* Equivalent to list() */
	krk_defineNative(&vm.globals, "dictOf", krk_dict_of); /* Equivalent to dict() */
	krk_defineNative(&vm.globals, "isinstance", krk_isinstance);
	krk_defineNative(&vm.globals, "globals", krk_globals);
	krk_defineNative(&vm.globals, "dir", krk_dirObject);
	krk_defineNative(&vm.globals, "len", _len);
	krk_defineNative(&vm.globals, "repr", _repr);
	krk_defineNative(&vm.globals, "print", _print);

	/* __builtins__.set_tracing is namespaced */
	krk_defineNative(&vm.builtins->fields, "set_tracing", krk_set_tracing);

	ADD_BASE_CLASS(vm.baseClasses.listiteratorClass, "listiterator", vm.objectClass);
	krk_defineNative(&vm.baseClasses.listiteratorClass->methods, ".__init__", _listiter_init);
	krk_defineNative(&vm.baseClasses.listiteratorClass->methods, ".__call__", _listiter_call);
	krk_finalizeClass(vm.baseClasses.listiteratorClass);

	ADD_BASE_CLASS(vm.baseClasses.striteratorClass, "striterator", vm.objectClass);
	krk_defineNative(&vm.baseClasses.striteratorClass->methods, ".__init__", _striter_init);
	krk_defineNative(&vm.baseClasses.striteratorClass->methods, ".__call__", _striter_call);
	krk_finalizeClass(vm.baseClasses.striteratorClass);

	ADD_BASE_CLASS(vm.baseClasses.rangeClass, "range", vm.objectClass);
	krk_attachNamedObject(&vm.globals, "range", (KrkObj*)vm.baseClasses.rangeClass);
	krk_defineNative(&vm.baseClasses.rangeClass->methods, ".__init__", _range_init);
	krk_defineNative(&vm.baseClasses.rangeClass->methods, ".__iter__", _range_iter);
	krk_defineNative(&vm.baseClasses.rangeClass->methods, ".__repr__", _range_repr);
	krk_finalizeClass(vm.baseClasses.rangeClass);

	ADD_BASE_CLASS(vm.baseClasses.rangeiteratorClass, "rangeiterator", vm.objectClass);
	krk_defineNative(&vm.baseClasses.rangeiteratorClass->methods, ".__init__", _rangeiterator_init);
	krk_defineNative(&vm.baseClasses.rangeiteratorClass->methods, ".__call__", _rangeiterator_call);
	krk_finalizeClass(vm.baseClasses.rangeiteratorClass);

	/**
	 * Read the managed code builtins module, which contains the base
	 * definitions for collections so we can pull them into the global
	 * namespace and attach their __init__/__get__/__set__, etc. methods.
	 *
	 * A significant subset of the VM's functionality is lost without
	 * these classes being available, but it should still work to some degree.
	 */
	KrkValue builtinsModule = krk_interpret(_builtins_src,1,"__builtins__","__builtins__");
	if (!IS_OBJECT(builtinsModule)) {
		/* ... hence, this is a warning and not a complete failure. */
		fprintf(stderr, "VM startup failure: Failed to load __builtins__ module.\n");
	} else {
		KrkValue val;
		/* Now we can attach the native initializers and getters/setters to
		 * the list and dict types by pulling them out of the global namespace,
		 * as they were exported by builtins.krk */
		krk_tableGet(&vm.globals,OBJECT_VAL(S("list")),&val);
		KrkClass * _class = AS_CLASS(val);
		krk_defineNative(&_class->methods, ".__init__", _list_init);
		krk_defineNative(&_class->methods, ".__get__", _list_get);
		krk_defineNative(&_class->methods, ".__set__", _list_set);
		krk_defineNative(&_class->methods, ".__len__", _list_len);
		krk_defineNative(&_class->methods, ".__contains__", _list_contains);
		krk_defineNative(&_class->methods, ".__getslice__", _list_slice);
		krk_defineNative(&_class->methods, ".append", _list_append);
		krk_finalizeClass(_class);

		krk_tableGet(&vm.globals,OBJECT_VAL(S("dict")),&val);
		_class = AS_CLASS(val);
		krk_defineNative(&_class->methods, ".__init__", _dict_init);
		krk_defineNative(&_class->methods, ".__get__", _dict_get);
		krk_defineNative(&_class->methods, ".__set__", _dict_set);
		krk_defineNative(&_class->methods, ".__len__", _dict_len);
		krk_defineNative(&_class->methods, ".__contains__", _dict_contains);
		krk_finalizeClass(_class);

		/* These are used to for dict.keys() to create the iterators. */
		krk_defineNative(&_class->methods, ".capacity", _dict_capacity);
		krk_defineNative(&_class->methods, "._key_at_index", _dict_key_at_index);
	}

	/* The VM is now ready to start executing code. */
	krk_resetStack();
	KRK_RESUME_GC();
}

/**
 * Reclaim resources used by the VM.
 */
void krk_freeVM() {
	krk_freeTable(&vm.globals);
	krk_freeTable(&vm.strings);
	krk_freeTable(&vm.modules);
	memset(vm.specialMethodNames,0,sizeof(vm.specialMethodNames));
	krk_freeObjects();
	FREE_ARRAY(size_t, vm.stack, vm.stackSize);
}

/**
 * Internal type(value).__name__ call for use in debugging methods and
 * creating exception strings.
 */
const char * krk_typeName(KrkValue value) {
	return AS_CLASS(krk_typeOf(1, (KrkValue[]){value}))->name->chars;
}

/**
 * Basic arithmetic and string functions follow.
 *
 * BIG TODO: All of these need corresponding __methods__ so that classes
 *           can override / implement them.
 * __add__, __sub__, __mult__, __div__,
 * __or__, __and__, __xor__, __lshift__, __rshift__, __remainder__?
 */

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

/* Bit ops are invalid on doubles in C, so we can't use the same set of macros for them;
 * they should be invalid in Kuroko as well. */
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
			int t = krk_callValue(krk_peek(0), 0, 1);
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

/**
 * At the end of each instruction cycle, we check the exception flag to see
 * if an error was raised during execution. If there is an exception, this
 * function is called to scan up the stack to see if there is an exception
 * handler value. Handlers live on the stack at the point where it should be
 * reset to and keep an offset to the except branch of a try/except statement
 * pair (or the exit point of the try, if there is no except branch). These
 * objects can't be built by (text) user code, but erroneous bytecode / module
 * stack manipulation could result in a handler being in the wrong place,
 * at which point there's no guarantees about what happens.
 */
static int handleException() {
	int stackOffset, frameOffset;
	int exitSlot = (vm.exitOnFrame >= 0) ? vm.frames[vm.exitOnFrame].outSlots : 0;
	for (stackOffset = (int)(vm.stackTop - vm.stack - 1); stackOffset >= exitSlot && !IS_HANDLER(vm.stack[stackOffset]); stackOffset--);
	if (stackOffset < exitSlot) {
		if (exitSlot == 0) {
			/*
			 * No exception was found and we have reached the top of the call stack.
			 * Call dumpTraceback to present the exception to the user and reset the
			 * VM stack state. It should still be safe to execute more code after
			 * this reset, so the repl can throw errors and keep accepting new lines.
			 */
			dumpTraceback();
			krk_resetStack();
			vm.frameCount = 0;
		}
		/* If exitSlot was not 0, there was an exception during a call to runNext();
		 * this is likely to be raised higher up the stack as an exception in the outer
		 * call, but we don't want to print the traceback here. */
		return 1;
	}

	/* Find the call frame that owns this stack slot */
	for (frameOffset = vm.frameCount - 1; frameOffset >= 0 && (int)vm.frames[frameOffset].slots > stackOffset; frameOffset--);
	if (frameOffset == -1) {
		fprintf(stderr, "Internal error: Call stack is corrupted - unable to find\n");
		fprintf(stderr, "                call frame that owns exception handler.\n");
		exit(1);
	}

	/* We found an exception handler and can reset the VM to its call frame. */
	closeUpvalues(stackOffset);
	vm.stackTop = vm.stack + stackOffset + 1;
	vm.frameCount = frameOffset + 1;

	/* Clear the exception flag so we can continue executing from the handler. */
	vm.flags &= ~KRK_HAS_EXCEPTION;
	return 0;
}

/**
 * Load a module.
 *
 * The module search path is stored in __builtins__.module_paths and should
 * be a list of directories (with trailing forward-slash) to look at, in order,
 * to resolve module names. krk source files will always take priority, so if
 * a later search path has a krk source and an earlier search path has a shared
 * object module, the later search path will still win.
 */
int krk_loadModule(KrkString * name, KrkValue * moduleOut) {
	KrkValue modulePaths, modulePathsInternal;

	/* See if the module is already loaded */
	if (krk_tableGet(&vm.modules, OBJECT_VAL(name), moduleOut)) {
		krk_push(*moduleOut);
		return 1;
	}

	/* Obtain __builtins__.module_paths */
	if (!krk_tableGet(&vm.builtins->fields, OBJECT_VAL(S("module_paths")), &modulePaths) || !IS_INSTANCE(modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.baseException,
			"Internal error: __builtins__.module_paths not defined.");
		return 0;
	}

	/* Obtain __builtins__.module_paths.__list so we can do lookups directly */
	if (!krk_tableGet(&(AS_INSTANCE(modulePaths)->fields), vm.specialMethodNames[METHOD_LIST_INT], &modulePathsInternal) || !IS_FUNCTION(modulePathsInternal)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.baseException,
			"Internal error: __builtins__.module_paths is corrupted or incorrectly set.");
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
		krk_runtimeError(vm.exceptions.importError,
			"No module search directories are specified, so no modules may be imported.");
		return 0;
	}

	struct stat statbuf;

	/* First search for {name}.krk in the module search paths */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		krk_push(AS_FUNCTION(modulePathsInternal)->chunk.constants.values[i]);
		if (!IS_STRING(krk_peek(0))) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.typeError,
				"Module search paths must be strings; check the search path at index %d", i);
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
			krk_runtimeError(vm.exceptions.importError,
				"Failed to load module '%s' from '%s'", name->chars, fileName);
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
			krk_runtimeError(vm.exceptions.importError,
				"Failed to load native module '%s' from shared object '%s'", name->chars, fileName);
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
			krk_runtimeError(vm.exceptions.importError,
				"Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad();
		if (!IS_OBJECT(*moduleOut)) {
			krk_runtimeError(vm.exceptions.importError,
				"Failed to load module '%s' from '%s'", name->chars, fileName);
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

/**
 * Try to resolve and push [stack top].name.
 * If [stack top] is an instance, scan fields first.
 * Otherwise, scan for methods from [stack top].__class__.
 * Returns 0 if nothing was found, 1 if something was - and that
 * "something" will replace [stack top].
 */
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

#define READ_BYTE() (*frame->ip++)
#define BINARY_OP(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); krk_push(op(a,b)); break; }
#define READ_CONSTANT(s) (frame->closure->function->chunk.constants.values[readBytes(frame,s)])
#define READ_STRING(s) AS_STRING(READ_CONSTANT(s))

/**
 * Read bytes after an opcode. Most instructions take 1, 2, or 3 bytes as an
 * operand referring to a local slot, constant slot, or offset.
 */
static inline size_t readBytes(CallFrame * frame, int num) {
	size_t out = READ_BYTE();
	while (--num) {
		out <<= 8;
		out |= (READ_BYTE() & 0xFF);
	}
	return out;
}

/**
 * VM main loop.
 */
static KrkValue run() {
	CallFrame* frame = &vm.frames[vm.frameCount - 1];

	while (1) {
#ifdef ENABLE_TRACING
		if (vm.flags & KRK_ENABLE_TRACING) {
			dumpStack(frame);
			krk_disassembleInstruction(&frame->closure->function->chunk,
				(size_t)(frame->ip - frame->closure->function->chunk.code));
		}
#endif

		uint8_t opcode = READ_BYTE();

		/* We split the instruction opcode table in half and use the top bit
		 * to mark instructions as "long" as we can quickly determine operand
		 * widths. The standard opereand width is 1 byte. If operands need
		 * to use more than 256 possible values, such as when the stack
		 * is very large or there are a lot of constants in a single chunk of
		 * bytecode, the long opcodes provide 24 bits of operand space. */
		int operandWidth = (opcode & (1 << 7)) ? 3 : 1;

		switch (opcode) {
			case OP_RETURN: {
				KrkValue result = krk_pop();
				closeUpvalues(frame->slots);
				vm.frameCount--;
				if (vm.frameCount == 0) {
					krk_pop();
					return result;
				}
				vm.stackTop = &vm.stack[frame->outSlots];
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
				if (!krk_callValue(krk_peek(argCount), argCount, 1)) {
					if (vm.flags & KRK_HAS_EXCEPTION) goto _finishException;
					return NONE_VAL();
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			/* This version of the call instruction takes its arity from the
			 * top of the stack, so we don't have to calculate arity at compile time. */
			case OP_CALL_STACK: {
				int argCount = AS_INTEGER(krk_pop());
				if (!krk_callValue(krk_peek(argCount), argCount, 1)) {
					if (vm.flags & KRK_HAS_EXCEPTION) goto _finishException;
					return NONE_VAL();
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			case OP_EXPAND_ARGS: {
				int type = READ_BYTE();
				krk_push(KWARGS_VAL(LONG_MAX-type));
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
				KrkClass * type = AS_CLASS(krk_typeOf(1,(KrkValue[]){krk_peek(1)}));
				if (type->_getter) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_getter), 2, 0));
				} else {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
				}
				break;
			}
			case OP_INVOKE_SETTER: {
				KrkClass * type = AS_CLASS(krk_typeOf(1,(KrkValue[]){krk_peek(2)}));
				if (type->_setter) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_setter), 3, 0));
				} else {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(2)));
				}
				break;
			}
			case OP_INVOKE_GETSLICE: {
				KrkClass * type = AS_CLASS(krk_typeOf(1,(KrkValue[]){krk_peek(2)}));
				if (type->_slicer) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_slicer), 3, 0));
				} else {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not sliceable", krk_typeName(krk_peek(2)));
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
				KrkValue method = krk_peek(0);
				KrkClass * _class = AS_CLASS(krk_peek(1));
				KrkValue name = OBJECT_VAL(READ_STRING(operandWidth));
				krk_tableSet(&_class->methods, name, method);
				krk_pop();
				break;
			}
			case OP_FINALIZE: {
				KrkClass * _class = AS_CLASS(krk_peek(0));
				/* Store special methods for quick access */
				krk_finalizeClass(_class);
				krk_pop(); /* Pop the class as we're done attaching methods */
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
				krk_swap(1);
				break;
			case OP_KWARGS_LONG:
			case OP_KWARGS: {
				krk_push(KWARGS_VAL(readBytes(frame,operandWidth)));
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
	krk_callValue(OBJECT_VAL(closure), 0, 1);

	return run();
}


KrkValue krk_runfile(const char * fileName, int newScope, char * fromName, char * fromFile) {
	FILE * f = fopen(fileName,"r");
	if (!f) {
		if (!newScope) {
			fprintf(stderr, "kuroko: could not read file '%s': %s\n", fileName, strerror(errno));
		}
		return INTEGER_VAL(errno);
	}

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

