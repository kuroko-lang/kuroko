#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "vm.h"
#include "debug.h"
#include "memory.h"
#include "compiler.h"
#include "object.h"
#include "table.h"

#define KRK_VERSION_MAJOR  "1"
#define KRK_VERSION_MINOR  "0"
#define KRK_VERSION_PATCH  "0"

#define KRK_VERSION_EXTRA_BASE  "-rc0"

#ifndef STATIC_ONLY
#define KRK_VERSION_EXTRA KRK_VERSION_EXTRA_BASE
#else
#define KRK_VERSION_EXTRA KRK_VERSION_EXTRA_BASE "-static"
#endif

#define KRK_BUILD_DATE     __DATE__ " at " __TIME__

#if (defined(__GNUC__) || defined(__GNUG__)) && !(defined(__clang__) || defined(__INTEL_COMPILER))
# define KRK_BUILD_COMPILER "GCC " __VERSION__
#elif (defined(__clang__))
# define KRK_BUILD_COMPILER "clang " __clang_version__
#else
# define KRK_BUILD_COMPILER ""
#endif

#ifndef _WIN32
# ifndef STATIC_ONLY
#  include <dlfcn.h>
# endif
# define PATH_SEP "/"
# define dlRefType void *
# define dlSymType void *
# define dlOpen(fileName) dlopen(fileName, RTLD_NOW)
# define dlSym(dlRef, handlerName) dlsym(dlRef,handlerName)
#else
# include <windows.h>
# define PATH_SEP "\\"
# define dlRefType HINSTANCE
# define dlSymType FARPROC
# define dlOpen(fileName) LoadLibraryA(fileName)
# define dlSym(dlRef, handlerName) GetProcAddress(dlRef, handlerName)
#endif

#define S(c) (krk_copyString(c,sizeof(c)-1))

#define likely(cond)   __builtin_expect(!!(cond), 1)
#define unlikely(cond) __builtin_expect(!!(cond), 0)

/* This is macro'd to krk_vm for namespacing reasons. */
KrkVM vm = {0};

/* Some quick forward declarations of string methods we like to call directly... */
static void addObjects();
static KrkValue _string_get(int argc, KrkValue argv[]);
static KrkValue _string_format(int argc, KrkValue argv[], int hasKw);

/* Embedded script for extensions to builtin-ins; see builtins.c/builtins.krk */
extern const char krk_builtinsSrc[];

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

		for (size_t x = vm.frameCount; x > 0; x--) {
			if (vm.frames[x-1].slots > i) continue;
			CallFrame * f = &vm.frames[x-1];
			size_t relative = i - f->slots;
			//fprintf(stderr, "(%s[%d])", f->closure->function->name->chars, (int)relative);
			/* Should resolve here? */
			if (relative < (size_t)f->closure->function->requiredArgs) {
				fprintf(stderr, "%s=", AS_CSTRING(f->closure->function->requiredArgNames.values[relative]));
				break;
			} else if (relative < (size_t)f->closure->function->requiredArgs + (size_t)f->closure->function->keywordArgs) {
				fprintf(stderr, "%s=", AS_CSTRING(f->closure->function->keywordArgNames.values[relative - f->closure->function->requiredArgs]));
				break;
			} else {
				int found = 0;
				for (size_t j = 0; j < f->closure->function->localNameCount; ++j) {
					if (relative == f->closure->function->localNames[j].id
						/* Only display this name if it's currently valid */
						&&  f->closure->function->localNames[j].birthday <= (size_t)(f->ip - f->closure->function->chunk.code)
						) {
						fprintf(stderr,"%s=", f->closure->function->localNames[j].name->chars);
						found = 1;
						break;
					}
				}
				if (found) break;
			}
		}

		
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
 * Display a traceback by scanning up the stack / call frames.
 * The format of the output here is modeled after the output
 * given by CPython, so we display the outermost call first
 * and then move inwards; on each call frame we try to open
 * the source file and print the corresponding line.
 */
void krk_dumpTraceback() {
	if (vm.frameCount) {
		fprintf(stderr, "Traceback (most recent call last):\n");
		for (size_t i = 0; i <= vm.frameCount - 1; i++) {
			CallFrame * frame = &vm.frames[i];
			KrkFunction * function = frame->closure->function;
			size_t instruction = frame->ip - function->chunk.code - 1;
			int lineNo = (int)krk_lineNumber(&function->chunk, instruction);
			fprintf(stderr, "  File \"%s\", line %d, in %s\n",
				(function->chunk.filename ? function->chunk.filename->chars : "?"),
				lineNo,
				(function->name ? function->name->chars : "(unnamed)"));
			if (function->chunk.filename) {
				FILE * f = fopen(function->chunk.filename->chars, "r");
				if (f) {
					int line = 1;
					do {
						char c = fgetc(f);
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
		}
	}

	if (!krk_valuesEqual(vm.currentException,NONE_VAL())) {
		krk_push(vm.currentException);
		KrkValue result = krk_callSimple(OBJECT_VAL(krk_getType(vm.currentException)->_reprer), 1, 0);
		fprintf(stderr, "%s\n", AS_CSTRING(result));
	}
}

/**
 * Raise an exception. Creates an exception object of the requested type
 * and formats a message string to attach to it. Exception classes are
 * found in vm.exceptions and are initialized on startup.
 */
KrkValue krk_runtimeError(KrkClass * type, const char * fmt, ...) {
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	size_t len = vsnprintf(buf, 1024, fmt, args);
	va_end(args);
	vm.flags |= KRK_HAS_EXCEPTION;

	/* Allocate an exception object of the requested type. */
	KrkInstance * exceptionObject = krk_newInstance(type);
	krk_push(OBJECT_VAL(exceptionObject));
	krk_push(OBJECT_VAL(S("arg")));
	krk_push(OBJECT_VAL(krk_copyString(buf, len)));
	/* Attach its argument */
	krk_tableSet(&exceptionObject->fields, krk_peek(1), krk_peek(0));
	krk_pop();
	krk_pop();
	krk_pop();

	/* Set the current exception to be picked up by handleException */
	vm.currentException = OBJECT_VAL(exceptionObject);
	return NONE_VAL();
}

/**
 * Since the stack can potentially move when something is pushed to it
 * if it this triggers a grow condition, it may be necessary to ensure
 * that this has already happened before actually dealing with the stack.
 */
void krk_reserve_stack(size_t space) {
	while ((size_t)(vm.stackTop - vm.stack) + space > vm.stackSize) {
		size_t old = vm.stackSize;
		size_t old_offset = vm.stackTop - vm.stack;
		vm.stackSize = GROW_CAPACITY(old);
		vm.stack = GROW_ARRAY(KrkValue, vm.stack, old, vm.stackSize);
		vm.stackTop = vm.stack + old_offset;
	}
}

/**
 * Push a value onto the stack, and grow the stack if necessary.
 * Note that growing the stack can involve the stack _moving_, so
 * do not rely on the memory offset of a stack value if you expect
 * the stack to grow - eg. if you are calling into managed code
 * to do anything, or if you are pushing anything.
 */
inline void krk_push(KrkValue value) {
	if (unlikely((size_t)(vm.stackTop - vm.stack) + 1 > vm.stackSize)) {
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
inline KrkValue krk_pop() {
	vm.stackTop--;
	if (unlikely(vm.stackTop < vm.stack)) {
		fprintf(stderr, "Fatal error: stack underflow detected in VM, issuing breakpoint.\n");
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
KrkNative * krk_defineNative(KrkTable * table, const char * name, NativeFn function) {
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
	return func;
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
		{&_class->_eq, METHOD_EQ},
		{&_class->_len, METHOD_LEN},
		{&_class->_enter, METHOD_ENTER},
		{&_class->_exit, METHOD_EXIT},
		{&_class->_delitem, METHOD_DELITEM},
		{&_class->_iter, METHOD_ITER},
		{&_class->_getattr, METHOD_GETATTR},
		{&_class->_dir, METHOD_DIR},
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

#define CHECK_DICT_FAST() if (unlikely(argc < 0 || !IS_INSTANCE(argv[0]) || \
	(AS_INSTANCE(argv[0])->_class != vm.baseClasses.dictClass && !krk_isInstanceOf(argv[0], vm.baseClasses.dictClass)))) \
		return krk_runtimeError(vm.exceptions.typeError, "expected dict")

#define CHECK_LIST_FAST() if (unlikely(argc < 0 || !IS_INSTANCE(argv[0]) || \
	(AS_INSTANCE(argv[0])->_class != vm.baseClasses.listClass && !krk_isInstanceOf(argv[0], vm.baseClasses.listClass)))) \
		return krk_runtimeError(vm.exceptions.typeError, "expected list")

/**
 * dict.__init__()
 */
static KrkValue _dict_init(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	krk_initTable(&((KrkDict *)AS_OBJECT(argv[0]))->entries);
	return argv[0];
}

static void _dict_gcscan(KrkInstance * self) {
	krk_markTable(&((KrkDict*)self)->entries);
}

static void _dict_gcsweep(KrkInstance * self) {
	krk_freeTable(&((KrkDict*)self)->entries);
}

/**
 * dict.__get__(key)
 */
static KrkValue _dict_get(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	KrkValue out;
	if (!krk_tableGet(AS_DICT(argv[0]), argv[1], &out)) return krk_runtimeError(vm.exceptions.keyError, "key error");
	return out;
}

/**
 * dict.__set__(key, value)
 */
static KrkValue _dict_set(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 3) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	krk_tableSet(AS_DICT(argv[0]), argv[1], argv[2]);
	return NONE_VAL();
}

static KrkValue _dict_or(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	if (!krk_isInstanceOf(argv[0],vm.baseClasses.dictClass) ||
	    !krk_isInstanceOf(argv[1],vm.baseClasses.dictClass))
		return krk_runtimeError(vm.exceptions.typeError, "Can not merge '%s' and '%s'.",
			krk_typeName(argv[0]),
			krk_typeName(argv[1]));

	KrkValue outDict = krk_dict_of(0,NULL);
	krk_push(outDict);

	/* Why is this src->dest... Should change that... */
	krk_tableAddAll(AS_DICT(argv[0]), AS_DICT(outDict));
	krk_tableAddAll(AS_DICT(argv[1]), AS_DICT(outDict));

	return krk_pop();
}

/**
 * dict.__delitem__
 */
static KrkValue _dict_delitem(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	if (!krk_tableDelete(AS_DICT(argv[0]), argv[1])) {
		KrkClass * type = krk_getType(argv[1]);
		if (type->_reprer) {
			krk_push(argv[1]);
			KrkValue asString = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(asString)) return krk_runtimeError(vm.exceptions.keyError, "%s", AS_CSTRING(asString));
		}
		return krk_runtimeError(vm.exceptions.keyError, "(Unrepresentable value)");
	}
	return NONE_VAL();
}

/**
 * dict.__len__()
 */
static KrkValue _dict_len(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	return INTEGER_VAL(AS_DICT(argv[0])->count);
}

/**
 * dict.__contains__()
 */
static KrkValue _dict_contains(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	KrkValue _unused;
	return BOOLEAN_VAL(krk_tableGet(AS_DICT(argv[0]), argv[1], &_unused));
}

/**
 * dict.capacity()
 */
static KrkValue _dict_capacity(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	return INTEGER_VAL(AS_DICT(argv[0])->capacity);
}

static KrkValue _dict_repr(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	KrkValue self = argv[0];
	if (AS_OBJECT(self)->inRepr) return OBJECT_VAL(S("{...}"));
	krk_push(OBJECT_VAL(S("{")));

	AS_OBJECT(self)->inRepr = 1;

	size_t c = 0;
	size_t len = AS_DICT(argv[0])->capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(argv[0])->entries[i];

		if (IS_KWARGS(entry->key)) continue;

		if (c > 0) {
			krk_push(OBJECT_VAL(S(", ")));
			addObjects();
		}

		c++;

		KrkClass * type = krk_getType(entry->key);
		krk_push(entry->key);
		krk_push(krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0));
		addObjects();

		krk_push(OBJECT_VAL(S(": ")));
		addObjects();

		type = krk_getType(entry->value);
		krk_push(entry->value);
		krk_push(krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0));
		addObjects();
	}

	AS_OBJECT(self)->inRepr = 0;

	krk_push(OBJECT_VAL(S("}")));
	addObjects();
	return krk_pop();

}

static KrkValue _dict_nth_key_fast(size_t capacity, KrkTableEntry * entries, size_t index) {
	size_t found = 0;
	for (size_t i = 0; i < capacity; ++i) {
		if (IS_KWARGS(entries[i].key)) continue;
		if (found == index) return entries[i].key;
		found++;
	}
	return NONE_VAL();
}

/**
 * list.__init__()
 */
static KrkValue _list_init(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc > 1) return krk_runtimeError(vm.exceptions.argumentError, "Can not initialize list from iterable (unsupported, try again later)");
	krk_initValueArray(AS_LIST(argv[0]));
	return argv[0];
}

static void _list_gcscan(KrkInstance * self) {
	for (size_t i = 0; i < ((KrkList*)self)->values.count; ++i) {
		krk_markValue(((KrkList*)self)->values.values[i]);
	}
}

static void _list_gcsweep(KrkInstance * self) {
	krk_freeValueArray(&((KrkList*)self)->values);
}

/**
 * list.__get__(index)
 */
static KrkValue _list_get(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 2 || !IS_INTEGER(argv[1]))) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments in get %d, (%s, %s)", argc, krk_typeName(argv[0]), krk_typeName(argv[1]));
	int index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (unlikely(index < 0 || index >= (int)AS_LIST(argv[0])->count)) return krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
	return AS_LIST(argv[0])->values[index];
}

/**
 * list.__set__(index, value)
 */
static KrkValue _list_set(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 3 || !IS_INTEGER(argv[1]))) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments in set %d, (%s, %s, %s)", argc, krk_typeName(argv[0]), krk_typeName(argv[1]), krk_typeName(argv[2]));
	int index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (unlikely(index < 0 || index >= (int)AS_LIST(argv[0])->count)) krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
	AS_LIST(argv[0])->values[index] = argv[2];
	return NONE_VAL();
}

/**
 * list.append(value)
 */
static KrkValue _list_append(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 2)) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
	krk_writeValueArray(AS_LIST(argv[0]), argv[1]);
	return NONE_VAL();
}

static KrkValue _list_insert(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 3)) return krk_runtimeError(vm.exceptions.argumentError, "list.insert() expects two arguments");
	if (unlikely(!IS_INTEGER(argv[1]))) return krk_runtimeError(vm.exceptions.typeError, "index must be integer");
	krk_integer_type index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (index < 0 || index > (long)AS_LIST(argv[0])->count) return krk_runtimeError(vm.exceptions.indexError, "list index out of range: %d", (int)index);

	krk_writeValueArray(AS_LIST(argv[0]), NONE_VAL());

	/* Move everything at and after this index one forward. */
	memcpy(&AS_LIST(argv[0])->values[index+1],
	       &AS_LIST(argv[0])->values[index],
	       sizeof(KrkValue) * (AS_LIST(argv[0])->count - index - 1));
	/* Stick argv[2] where it belongs */
	AS_LIST(argv[0])->values[index] = argv[2];
	return NONE_VAL();
}

/**
 * list.__repr__
 */
static KrkValue _list_repr(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	KrkValue self = argv[0];
	if (AS_OBJECT(self)->inRepr) return OBJECT_VAL(S("[...]"));
	krk_push(OBJECT_VAL(S("[")));

	AS_OBJECT(self)->inRepr = 1;

	size_t len = AS_LIST(self)->count;
	for (size_t i = 0; i < len; ++i) {
		KrkClass * type = krk_getType(AS_LIST(self)->values[i]);
		krk_push(AS_LIST(self)->values[i]);
		krk_push(krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0));
		addObjects();
		if (i + 1 < len) {
			krk_push(OBJECT_VAL(S(", ")));
			addObjects();
		}
	}

	AS_OBJECT(self)->inRepr = 0;

	krk_push(OBJECT_VAL(S("]")));
	addObjects();
	return krk_pop();
}

static KrkValue _list_extend(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	KrkValueArray *  positionals = AS_LIST(argv[0]);
#define unpackArray(counter, indexer) do { \
			if (positionals->count + counter > positionals->capacity) { \
				size_t old = positionals->capacity; \
				positionals->capacity = positionals->count + counter; \
				positionals->values = GROW_ARRAY(KrkValue,positionals->values,old,positionals->capacity); \
			} \
			for (size_t i = 0; i < counter; ++i) { \
				positionals->values[positionals->count] = indexer; \
				positionals->count++; \
			} \
		} while (0)
	KrkValue value = argv[1];
	//UNPACK_ARRAY();  /* This should be a macro that does all of these things. */
	if (IS_TUPLE(value)) {
		unpackArray(AS_TUPLE(value)->values.count, AS_TUPLE(value)->values.values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.listClass) {
		unpackArray(AS_LIST(value)->count, AS_LIST(value)->values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.dictClass) {
		unpackArray(AS_DICT(value)->count, _dict_nth_key_fast(AS_DICT(value)->capacity, AS_DICT(value)->entries, i));
	} else if (IS_STRING(value)) {
		unpackArray(AS_STRING(value)->codesLength, _string_get(2,(KrkValue[]){value,INTEGER_VAL(i)}));
	} else {
		KrkClass * type = krk_getType(argv[1]);
		if (type->_iter) {
			/* Create the iterator */
			size_t stackOffset = vm.stackTop - vm.stack;
			krk_push(argv[1]);
			krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

			do {
				/* Call it until it gives us itself */
				krk_push(vm.stack[stackOffset]);
				krk_push(krk_callSimple(krk_peek(0), 0, 1));
				if (krk_valuesSame(vm.stack[stackOffset], krk_peek(0))) {
					/* We're done. */
					krk_pop(); /* The result of iteration */
					krk_pop(); /* The iterator */
					break;
				}
				_list_append(2, (KrkValue[]){argv[0], krk_peek(0)});
				krk_pop();
			} while (1);
		} else {
			return krk_runtimeError(vm.exceptions.typeError, "'%s' object is not iterable", krk_typeName(value));
		}
	}
#undef unpackArray
	return NONE_VAL();
}

static KrkValue _list_mul(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (!IS_INTEGER(argv[1]))
		return krk_runtimeError(vm.exceptions.typeError, "unsupported operand types for *: '%s' and '%s'",
			"list", krk_typeName(argv[1]));

	krk_integer_type howMany = AS_INTEGER(argv[1]);

	KrkValue out = krk_list_of(0, NULL);

	krk_push(out);

	for (krk_integer_type i = 0; i < howMany; i++) {
		_list_extend(2, (KrkValue[]){out,argv[0]});
	}

	return krk_pop();
}

/**
 * list.__len__
 */
static KrkValue _list_len(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc < 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
	return INTEGER_VAL(AS_LIST(argv[0])->count);
}

/**
 * list.__contains__
 */
static KrkValue _list_contains(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
	for (size_t i = 0; i < AS_LIST(argv[0])->count; ++i) {
		if (krk_valuesEqual(argv[1], AS_LIST(argv[0])->values[i])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

/**
 * Exposed method called to produce lists from [expr,...] sequences in managed code.
 * Presented in the global namespace as listOf(...)
 */
KrkValue krk_list_of(int argc, KrkValue argv[]) {
	KrkValue outList = OBJECT_VAL(krk_newInstance(vm.baseClasses.listClass));
	krk_push(outList);
	krk_initValueArray(AS_LIST(outList));

	if (argc) {
		AS_LIST(outList)->capacity = argc;
		AS_LIST(outList)->values = GROW_ARRAY(KrkValue, AS_LIST(outList)->values, 0, argc);
		memcpy(AS_LIST(outList)->values, argv, sizeof(KrkValue) * argc);
		AS_LIST(outList)->count = argc;
	}

	return krk_pop();
}

/**
 * Exposed method called to produce dictionaries from {expr: expr, ...} sequences in managed code.
 * Presented in the global namespace as dictOf(...). Expects arguments as key,value,key,value...
 */
KrkValue krk_dict_of(int argc, KrkValue argv[]) {
	if (argc % 2 != 0) return krk_runtimeError(vm.exceptions.argumentError, "Expected even number of arguments to dictOf");
	KrkInstance * outDict = krk_newInstance(vm.baseClasses.dictClass);
	krk_push(OBJECT_VAL(outDict));
	krk_initTable(&((KrkDict*)outDict)->entries);
	for (int ind = 0; ind < argc; ind += 2) {
		krk_tableSet(&((KrkDict*)outDict)->entries, argv[ind], argv[ind+1]);
	}
	return krk_pop();
}

/**
 * list.__getslice__
 */
static KrkValue _list_slice(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc < 3) return krk_runtimeError(vm.exceptions.argumentError, "slice: expected 2 arguments, got %d", argc-1);
	if (!IS_INSTANCE(argv[0]) ||
		!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) ||
		!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) {
		return krk_runtimeError(vm.exceptions.typeError, "slice: expected two integer arguments");
	}

	int start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	int end   = IS_NONE(argv[2]) ? (int)AS_LIST(argv[0])->count : AS_INTEGER(argv[2]);
	if (start < 0) start = (int)AS_LIST(argv[0])->count + start;
	if (start < 0) start = 0;
	if (end < 0) end = (int)AS_LIST(argv[0])->count + end;
	if (start > (int)AS_LIST(argv[0])->count) start = (int)AS_LIST(argv[0])->count;
	if (end > (int)AS_LIST(argv[0])->count) end = (int)AS_LIST(argv[0])->count;
	if (end < start) end = start;
	int len = end - start;

	return krk_list_of(len, &AS_LIST(argv[0])->values[start]);
}

/**
 * list.pop()
 */
static KrkValue _list_pop(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (!AS_LIST(argv[0])->count) return krk_runtimeError(vm.exceptions.indexError, "pop from empty list");
	long index = AS_LIST(argv[0])->count - 1;
	if (argc > 1) {
		index = AS_INTEGER(argv[1]);
	}
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (index < 0 || index >= (long)AS_LIST(argv[0])->count) return krk_runtimeError(vm.exceptions.indexError, "list index out of range: %d", (int)index);
	KrkValue outItem = AS_LIST(argv[0])->values[index];
	if (index == (long)AS_LIST(argv[0])->count-1) {
		AS_LIST(argv[0])->count--;
		return outItem;
	} else {
		/* Need to move up */
		size_t remaining = AS_LIST(argv[0])->count - index - 1;
		memmove(&AS_LIST(argv[0])->values[index], &AS_LIST(argv[0])->values[index+1],
			sizeof(KrkValue) * remaining);
		AS_LIST(argv[0])->count--;
		return outItem;
	}
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
		KrkValue test;
		if (krk_tableGet(AS_DICT(argv[0]), OBJECT_VAL(S("tracing")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_TRACING; else vm.flags &= ~KRK_ENABLE_TRACING; }
		if (krk_tableGet(AS_DICT(argv[0]), OBJECT_VAL(S("disassembly")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_DISASSEMBLY; else vm.flags &= ~KRK_ENABLE_DISASSEMBLY; }
		if (krk_tableGet(AS_DICT(argv[0]), OBJECT_VAL(S("stressgc")), &test)) {
			if (AS_INTEGER(test) == 1) vm.flags |= KRK_ENABLE_STRESS_GC; else vm.flags &= ~KRK_ENABLE_STRESS_GC; }
		if (krk_tableGet(AS_DICT(argv[0]), OBJECT_VAL(S("scantracing")), &test)) {
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
	return krk_runtimeError(vm.exceptions.typeError,"Debugging is not enabled in this build.");
#endif
}

/**
 * object.__dir__()
 */
KrkValue krk_dirObject(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments or bad type, got %d\n", argc);

	/* Create a new list instance */
	KrkValue myList = krk_list_of(0,NULL);
	krk_push(myList);

	if (IS_INSTANCE(argv[0])) {
		/* Obtain self-reference */
		KrkInstance * self = AS_INSTANCE(argv[0]);

		/* First add each method of the class */
		for (size_t i = 0; i < self->_class->methods.capacity; ++i) {
			if (self->_class->methods.entries[i].key.type != VAL_KWARGS) {
				krk_writeValueArray(AS_LIST(myList),
					self->_class->methods.entries[i].key);
			}
		}

		/* Then add each field of the instance */
		for (size_t i = 0; i < self->fields.capacity; ++i) {
			if (self->fields.entries[i].key.type != VAL_KWARGS) {
				krk_writeValueArray(AS_LIST(myList),
					self->fields.entries[i].key);
			}
		}
	} else {
		if (IS_CLASS(argv[0])) {
			KrkClass * _class = AS_CLASS(argv[0]);
			for (size_t i = 0; i < _class->methods.capacity; ++i) {
				if (_class->methods.entries[i].key.type != VAL_KWARGS) {
					krk_writeValueArray(AS_LIST(myList),
						_class->methods.entries[i].key);
				}
			}
			for (size_t i = 0; i < _class->fields.capacity; ++i) {
				if (_class->fields.entries[i].key.type != VAL_KWARGS) {
					krk_writeValueArray(AS_LIST(myList),
						_class->fields.entries[i].key);
				}
			}
		}
		KrkClass * type = krk_getType(argv[0]);

		for (size_t i = 0; i < type->methods.capacity; ++i) {
			if (type->methods.entries[i].key.type != VAL_KWARGS) {
				krk_writeValueArray(AS_LIST(myList),
					type->methods.entries[i].key);
			}
		}
	}

	/* Prepare output value */
	krk_pop();
	return myList;
}

/**
 * Maps values to their base classes.
 * Internal version of type().
 */
inline KrkClass * krk_getType(KrkValue of) {
	switch (of.type) {
		case VAL_INTEGER:
			return vm.baseClasses.intClass;
		case VAL_FLOATING:
			return vm.baseClasses.floatClass;
		case VAL_BOOLEAN:
			return vm.baseClasses.boolClass;
		case VAL_NONE:
			return vm.baseClasses.noneTypeClass;
		case VAL_OBJECT:
			switch (AS_OBJECT(of)->type) {
				case OBJ_CLASS:
					return vm.baseClasses.typeClass;
				case OBJ_NATIVE:
				case OBJ_FUNCTION:
				case OBJ_CLOSURE:
					return vm.baseClasses.functionClass;
				case OBJ_BOUND_METHOD:
					return vm.baseClasses.methodClass;
				case OBJ_STRING:
					return vm.baseClasses.strClass;
				case OBJ_TUPLE:
					return vm.baseClasses.tupleClass;
				case OBJ_BYTES:
					return vm.baseClasses.bytesClass;
				case OBJ_INSTANCE:
					return AS_INSTANCE(of)->_class;
				default:
					return vm.objectClass;
			} break;
		default:
			return vm.objectClass;
	}
}

/**
 * type()
 */
KrkValue _type(int argc, KrkValue argv[]) {
	return OBJECT_VAL(krk_getType(argv[0]));
}

static KrkValue _type_init(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions.argumentError, "type() takes 1 argument");
	return OBJECT_VAL(krk_getType(argv[1]));
}

/* Class.__base__ */
static KrkValue krk_baseOfClass(int argc, KrkValue argv[]) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "expected class");
	return AS_CLASS(argv[0])->base ? OBJECT_VAL(AS_CLASS(argv[0])->base) : NONE_VAL();
}

/* Class.__name */
static KrkValue krk_nameOfClass(int argc, KrkValue argv[]) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "expected class");
	return AS_CLASS(argv[0])->name ? OBJECT_VAL(AS_CLASS(argv[0])->name) : NONE_VAL();
}

/* Class.__file__ */
static KrkValue krk_fileOfClass(int argc, KrkValue argv[]) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "expected class");
	return AS_CLASS(argv[0])->filename ? OBJECT_VAL(AS_CLASS(argv[0])->filename) : NONE_VAL();
}

/* Class.__doc__ */
static KrkValue krk_docOfClass(int argc, KrkValue argv[]) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "expected class");
	return AS_CLASS(argv[0])->docstring ? OBJECT_VAL(AS_CLASS(argv[0])->docstring) : NONE_VAL();
}

/* Class.__str__() (and Class.__repr__) */
static KrkValue _class_to_str(int argc, KrkValue argv[]) {
	if (!IS_CLASS(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "expected class");
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
int krk_isInstanceOf(KrkValue obj, KrkClass * type) {
	KrkClass * mine = krk_getType(obj);
	while (mine) {
		if (mine == type) return 1;
		mine = mine->base;
	}
	return 0;
}
static KrkValue _isinstance(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions.argumentError, "isinstance expects 2 arguments, got %d", argc);
	if (!IS_CLASS(argv[1])) return krk_runtimeError(vm.exceptions.typeError, "isinstance() arg 2 must be class");

	return BOOLEAN_VAL(krk_isInstanceOf(argv[0], AS_CLASS(argv[1])));
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
	/* Copy the globals table into it */
	krk_tableAddAll(vm.frames[vm.frameCount-1].globals, AS_DICT(dict));
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
			(destination - closure->function->requiredArgs < closure->function->keywordArgs ? AS_CSTRING(closure->function->keywordArgNames.values[destination - closure->function->requiredArgs]) :
				"(unnamed arg)")));
}

int krk_processComplexArguments(int argCount, KrkValueArray * positionals, KrkTable * keywords) {
	size_t kwargsCount = AS_INTEGER(vm.stackTop[-1]);
	krk_pop(); /* Pop the arg counter */
	argCount--;

	krk_initValueArray(positionals);
	krk_initTable(keywords);

	/* First, process all the positionals, including any from extractions. */
	size_t existingPositionalArgs = argCount - kwargsCount * 2;
	for (size_t i = 0; i < existingPositionalArgs; ++i) {
		krk_writeValueArray(positionals, vm.stackTop[-argCount + i]);
	}

	KrkValue * startOfExtras = &vm.stackTop[-kwargsCount * 2];
	/* Now unpack everything else. */
	for (size_t i = 0; i < kwargsCount; ++i) {
		KrkValue key = startOfExtras[i*2];
		KrkValue value = startOfExtras[i*2 + 1];
		if (IS_KWARGS(key)) {
			if (AS_INTEGER(key) == LONG_MAX-1) { /* unpack list */
#define unpackArray(counter, indexer) do { \
					if (positionals->count + counter > positionals->capacity) { \
						size_t old = positionals->capacity; \
						positionals->capacity = positionals->count + counter; \
						positionals->values = GROW_ARRAY(KrkValue,positionals->values,old,positionals->capacity); \
					} \
					for (size_t i = 0; i < counter; ++i) { \
						positionals->values[positionals->count] = indexer; \
						positionals->count++; \
					} \
				} while (0)
				
				if (IS_TUPLE(value)) {
					unpackArray(AS_TUPLE(value)->values.count, AS_TUPLE(value)->values.values[i]);
				} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.listClass) {
					unpackArray(AS_LIST(value)->count, AS_LIST(value)->values[i]);
				} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.dictClass) {
					unpackArray(AS_DICT(value)->count, _dict_nth_key_fast(AS_DICT(value)->capacity, AS_DICT(value)->entries, i));
				} else if (IS_STRING(value)) {
					unpackArray(AS_STRING(value)->codesLength, _string_get(2,(KrkValue[]){value,INTEGER_VAL(i)}));
				} else {
					KrkClass * type = krk_getType(value);
					if (type->_iter) {
						/* Create the iterator */
						size_t stackOffset = vm.stackTop - vm.stack;
						krk_push(value);
						krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

						do {
							/* Call it until it gives us itself */
							krk_push(vm.stack[stackOffset]);
							krk_push(krk_callSimple(krk_peek(0), 0, 1));
							if (krk_valuesSame(vm.stack[stackOffset], krk_peek(0))) {
								/* We're done. */
								krk_pop(); /* The result of iteration */
								krk_pop(); /* The iterator */
								break;
							}
							krk_writeValueArray(positionals, krk_peek(0));
							krk_pop();
						} while (1);
					} else {
						krk_runtimeError(vm.exceptions.typeError, "Can not unpack *expression: '%s' object is not iterable", krk_typeName(value));
						return 0;
					}
				}
#undef unpackArray
			} else if (AS_INTEGER(key) == LONG_MAX-2) { /* unpack dict */
				if (!IS_INSTANCE(value)) {
					krk_runtimeError(vm.exceptions.typeError, "**expression value is not a dict.");
					return 0;
				}
				for (size_t i = 0; i < AS_DICT(value)->capacity; ++i) {
					KrkTableEntry * entry = &AS_DICT(value)->entries[i];
					if (entry->key.type != VAL_KWARGS) {
						if (!IS_STRING(entry->key)) {
							krk_runtimeError(vm.exceptions.typeError, "**expression contains non-string key");
							return 0;
						}
						if (!krk_tableSet(keywords, entry->key, entry->value)) {
							krk_runtimeError(vm.exceptions.typeError, "got multiple values for argument '%s'", AS_CSTRING(entry->key));
							return 0;
						}
					}
				}
			} else if (AS_INTEGER(key) == LONG_MAX) { /* single value */
				krk_writeValueArray(positionals, value);
			}
		} else if (IS_STRING(key)) {
			if (!krk_tableSet(keywords, key, value)) {
				krk_runtimeError(vm.exceptions.typeError, "got multiple values for argument '%s'", AS_CSTRING(key));
				return 0;
			}
		}
	}

	return 1;
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
	size_t argCountX = argCount;
	KrkValueArray positionals;
	KrkTable keywords;

	if (argCount && IS_KWARGS(vm.stackTop[-1])) {
		KRK_PAUSE_GC();

		/* This processes the existing argument list into a ValueArray and a Table with the args and keywords */
		if (!krk_processComplexArguments(argCount, &positionals, &keywords)) goto _errorDuringPositionals;
		argCount--; /* It popped the KWARGS value from the top, so we have one less argument */

		/* Do we already know we have too many arguments? Let's bail before doing a bunch of work. */
		if ((positionals.count > potentialPositionalArgs) && (!closure->function->collectsArguments)) {
			checkArgumentCount(closure,positionals.count);
			goto _errorDuringPositionals;
		}

		/* Prepare stack space for all potential positionals, mark them unset */
		for (size_t i = 0; i < (size_t)argCount; ++i) {
			vm.stackTop[-argCount + i] = KWARGS_VAL(0);
		}

		/* Do we have a bunch of unused keyword argument slots? Fill them in. */
		while ((size_t)argCount < potentialPositionalArgs) {
			krk_push(KWARGS_VAL(0));
			argCount++;
		}

		/* Did we have way more arguments than we needed? Put the stack where it should be. */
		while ((size_t)argCount > potentialPositionalArgs) {
			krk_pop();
			argCount--;
		}

		/* Place positional arguments */
		for (size_t i = 0; i < potentialPositionalArgs && i < positionals.count; ++i) {
			vm.stackTop[-argCount + i] = positionals.values[i];
		}

		if (closure->function->collectsArguments) {
			size_t count  = (positionals.count > potentialPositionalArgs) ? (positionals.count - potentialPositionalArgs) : 0;
			KrkValue * offset = (count == 0) ? NULL : &positionals.values[potentialPositionalArgs];
			krk_push(krk_list_of(count, offset));
			argCount++;
		}

		krk_freeValueArray(&positionals);

		/* Now place keyword arguments */
		for (size_t i = 0; i < keywords.capacity; ++i) {
			KrkTableEntry * entry = &keywords.entries[i];
			if (entry->key.type != VAL_KWARGS) {
				KrkValue name = entry->key;
				KrkValue value = entry->value;
				/* See if we can place it */
				for (int j = 0; j < (int)closure->function->requiredArgs; ++j) {
					if (krk_valuesEqual(name, closure->function->requiredArgNames.values[j])) {
						if (!IS_KWARGS(vm.stackTop[-argCount + j])) {
							multipleDefs(closure,j);
							goto _errorAfterPositionals;
						}
						vm.stackTop[-argCount + j] = value;
						goto _finishKwarg;
					}
				}
				/* See if it's a keyword arg. */
				for (int j = 0; j < (int)closure->function->keywordArgs; ++j) {
					if (krk_valuesEqual(name, closure->function->keywordArgNames.values[j])) {
						if (!IS_KWARGS(vm.stackTop[-argCount + j + closure->function->requiredArgs])) {
							multipleDefs(closure, j + closure->function->requiredArgs);
							goto _errorAfterPositionals;
						}
						vm.stackTop[-argCount + j + closure->function->requiredArgs] = value;
						goto _finishKwarg;
					}
				}
				if (!closure->function->collectsKeywords) {
					krk_runtimeError(vm.exceptions.typeError, "%s() got an unexpected keyword argument '%s'",
						closure->function->name ? closure->function->name->chars : "<unnamed function>",
						AS_CSTRING(name));
					goto _errorAfterPositionals;
				}
				continue;
_finishKwarg:
				entry->key = KWARGS_VAL(0);
				entry->value = BOOLEAN_VAL(1);
				continue;
			}
		}

		/* If this function takes a **kwargs, we need to provide it as a dict */
		if (closure->function->collectsKeywords) {
			krk_push(krk_dict_of(0,NULL));
			argCount++;
			krk_tableAddAll(&keywords, AS_DICT(krk_peek(0)));
		}

		krk_freeTable(&keywords);

		for (size_t i = 0; i < (size_t)closure->function->requiredArgs; ++i) {
			if (IS_KWARGS(vm.stackTop[-argCount + i])) {
				krk_runtimeError(vm.exceptions.typeError, "%s() missing required positional argument: '%s'",
					closure->function->name ? closure->function->name->chars : "<unnamed function>",
					AS_CSTRING(closure->function->requiredArgNames.values[i]));
				goto _errorAfterKeywords;
			}
		}

		KRK_RESUME_GC();
		argCountX = argCount - (closure->function->collectsArguments + closure->function->collectsKeywords);
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
	frame->globals = &closure->function->globalsContext->fields;
	return 1;

_errorDuringPositionals:
	krk_freeValueArray(&positionals);
_errorAfterPositionals:
	krk_freeTable(&keywords);
_errorAfterKeywords:
	KRK_RESUME_GC();
	return 0;
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
 */
int krk_callValue(KrkValue callee, int argCount, int extra) {
	if (IS_OBJECT(callee)) {
		switch (OBJECT_TYPE(callee)) {
			case OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount, extra);
			case OBJ_NATIVE: {
				NativeFnKw native = (NativeFnKw)AS_NATIVE(callee)->function;
				if (argCount && IS_KWARGS(vm.stackTop[-1])) {
					KRK_PAUSE_GC();
					KrkValue myList = krk_list_of(0,NULL);
					KrkValue myDict = krk_dict_of(0,NULL);
					if (!krk_processComplexArguments(argCount, AS_LIST(myList), AS_DICT(myDict))) {
						KRK_RESUME_GC();
						return 0;
					}
					KRK_RESUME_GC();
					argCount--; /* Because that popped the kwargs value */
					vm.stackTop -= argCount + extra; /* We can just put the stack back to normal */
					krk_push(myList);
					krk_push(myDict);
					krk_writeValueArray(AS_LIST(myList), myDict);
					KrkValue result = native(AS_LIST(myList)->count, AS_LIST(myList)->values, 1);
					if (vm.stackTop == vm.stack) return 0;
					krk_pop();
					krk_pop();
					krk_push(result);
				} else {
					KrkValue * stackCopy = malloc(argCount * sizeof(KrkValue));
					memcpy(stackCopy, vm.stackTop - argCount, argCount * sizeof(KrkValue));
					KrkValue result = native(argCount, stackCopy, 0);
					free(stackCopy);
					if (vm.stackTop == vm.stack) return 0;
					vm.stackTop -= argCount + extra;
					krk_push(result);
				}
				return 2;
			}
			case OBJ_INSTANCE: {
				KrkClass * _class = AS_INSTANCE(callee)->_class;
				KrkValue callFunction;
				if (_class->_call) {
					return krk_callValue(OBJECT_VAL(_class->_call), argCount + 1, 0);
				} else if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_CALL], &callFunction)) {
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
				if (_class->_init) {
					return krk_callValue(OBJECT_VAL(_class->_init), argCount + 1, 0);
				} else if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_INIT], &initializer)) {
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
	if (!IS_NONE(vm.currentException)) return NONE_VAL();
	return krk_runtimeError(vm.exceptions.typeError, "Invalid internal method call: %d ('%s')", result, krk_typeName(value));
}

/**
 * Attach a method call to its callee and return a BoundMethod.
 * Works for managed and native method calls.
 */
int krk_bindMethod(KrkClass * _class, KrkString * name) {
	KrkValue method, out;
	if (!krk_tableGet(&_class->methods, OBJECT_VAL(name), &method)) return 0;
	if (IS_NATIVE(method) && ((KrkNative*)AS_OBJECT(method))->isMethod == 2) {
		out = AS_NATIVE(method)->function(1, (KrkValue[]){krk_peek(0)});
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
		addObjects();
		krk_push(arg);
		addObjects();
		return krk_pop();
	}
}

static KrkValue _syntaxerror_repr(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	/* .arg */
	KrkValue file, line, lineno, colno, width, arg, func;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("file")), &file) || !IS_STRING(file)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("line")), &line) || !IS_STRING(line)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("lineno")), &lineno) || !IS_INTEGER(lineno)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("colno")), &colno) || !IS_INTEGER(colno)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("width")), &width) || !IS_INTEGER(width)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("arg")), &arg) || !IS_STRING(arg)) goto _badSyntaxError;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("func")), &func)) goto _badSyntaxError;

	krk_push(OBJECT_VAL(S("  File \"{}\", line {}{}\n    {}\n    {}^\n{}: {}")));
	char * tmp = malloc(AS_INTEGER(colno));
	memset(tmp,' ',AS_INTEGER(colno));
	tmp[AS_INTEGER(colno)-1] = '\0';
	krk_push(OBJECT_VAL(krk_takeString(tmp,AS_INTEGER(colno)-1)));
	krk_push(OBJECT_VAL(self->_class->name));
	if (IS_STRING(func)) {
		krk_push(OBJECT_VAL(S(" in ")));
		krk_push(func);
		addObjects();
	} else {
		krk_push(OBJECT_VAL(S("")));
	}
	KrkValue formattedString = _string_format(8,
		(KrkValue[]){krk_peek(3), file, lineno, krk_peek(0), line, krk_peek(2), krk_peek(1), arg}, 0);
	krk_pop(); /* instr */
	krk_pop(); /* class */
	krk_pop(); /* spaces */
	krk_pop(); /* format string */

	return formattedString;

_badSyntaxError:
	return OBJECT_VAL(S("SyntaxError: invalid syntax"));
}

#define CHECK_STRING() if (unlikely(argc < 1 || !IS_STRING(argv[0]))) \
	return krk_runtimeError(vm.exceptions.typeError, "expected str")

static KrkValue _string_init(int argc, KrkValue argv[]) {
	/* Ignore argument which would have been an instance */
	if (argc < 2) {
		return OBJECT_VAL(S(""));
	}
	if (argc > 2) return krk_runtimeError(vm.exceptions.argumentError, "str() takes 1 argument");
	if (IS_STRING(argv[1])) return argv[1]; /* strings are immutable, so we can just return the arg */
	/* Find the type of arg */
	krk_push(argv[1]);
	if (!krk_getType(argv[1])->_tostr) return krk_runtimeError(vm.exceptions.typeError, "Can not convert %s to str", krk_typeName(argv[1]));
	return krk_callSimple(OBJECT_VAL(krk_getType(argv[1])->_tostr), 1, 0);
}

static KrkValue _string_add(int argc, KrkValue argv[]) {
	CHECK_STRING();
	const char * a, * b;
	size_t al, bl;
	int needsPop = 0;

	a = AS_CSTRING(argv[0]);
	al = AS_STRING(argv[0])->length;

	if (!IS_STRING(argv[1])) {
		KrkClass * type = krk_getType(argv[1]);
		if (type->_tostr) {
			krk_push(argv[1]);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_tostr), 1, 0);
			krk_push(result);
			needsPop = 1;
			if (!IS_STRING(result)) return krk_runtimeError(vm.exceptions.typeError, "__str__ produced something that was not a string: '%s'", krk_typeName(result));
			b = AS_CSTRING(result);
			bl = AS_STRING(result)->length;
		} else {
			b = krk_typeName(argv[1]);
			bl = strlen(b);
		}
	} else {
		b = AS_CSTRING(argv[1]);
		bl = AS_STRING(argv[1])->length;
	}

	size_t length = al + bl;
	char * chars = ALLOCATE(char, length + 1);
	memcpy(chars, a, al);
	memcpy(chars + al, b, bl);
	chars[length] = '\0';

	KrkString * result = krk_takeString(chars, length);
	if (needsPop) krk_pop();
	return OBJECT_VAL(result);
}

#define ADD_BASE_CLASS(obj, name, baseClass) do { \
	obj = krk_newClass(S(name), baseClass); \
	krk_attachNamedObject(&vm.builtins->fields, name, (KrkObj*)obj); \
} while (0)

#define ADD_EXCEPTION_CLASS(obj, name, baseClass) do { \
	obj = krk_newClass(S(name), baseClass); \
	krk_attachNamedObject(&vm.builtins->fields, name, (KrkObj*)obj); \
	krk_finalizeClass(obj); \
} while (0)

#define BUILTIN_FUNCTION(name, func, docStr) do { \
	krk_defineNative(&vm.builtins->fields, name, func)->doc = docStr; \
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
	krk_integer_type value = AS_INTEGER(argv[0]);
	unsigned char bytes[5] = {0};
	size_t len = krk_codepointToBytes(value, bytes);
	return OBJECT_VAL(krk_copyString((char*)bytes,len));
}

/* str.__ord__() */
static KrkValue _string_ord(int argc, KrkValue argv[]) {
	CHECK_STRING();
	if (AS_STRING(argv[0])->codesLength != 1)
		return krk_runtimeError(vm.exceptions.typeError, "ord() expected a character, but string of length %d found",
			AS_STRING(argv[0])->codesLength);

	return INTEGER_VAL(krk_unicodeCodepoint(AS_STRING(argv[0]),0));
}

static KrkValue _print(int argc, KrkValue argv[], int hasKw) {
	KrkValue sepVal, endVal;
	char * sep = " "; size_t sepLen = 1;
	char * end = "\n"; size_t endLen = 1;
	if (hasKw) {
		argc--;
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("sep")), &sepVal)) {
			if (!IS_STRING(sepVal)) return krk_runtimeError(vm.exceptions.typeError, "'sep' should be a string, not '%s'", krk_typeName(sepVal));
			sep = AS_CSTRING(sepVal);
			sepLen = AS_STRING(sepVal)->length;
		}
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("end")), &endVal)) {
			if (!IS_STRING(endVal)) return krk_runtimeError(vm.exceptions.typeError, "'end' should be a string, not '%s'", krk_typeName(endVal));
			end = AS_CSTRING(endVal);
			endLen = AS_STRING(endVal)->length;
		}
	}
	for (int i = 0; i < argc; ++i) {
		KrkValue printable = argv[i];
		if (IS_STRING(printable)) { /* krk_printValue runs repr */
			/* Make sure we handle nil bits correctly. */
			for (size_t j = 0; j < AS_STRING(printable)->length; ++j) {
				fputc(AS_CSTRING(printable)[j], stdout);
			}
		} else {
			krk_printValue(stdout, printable);
		}
		char * thingToPrint = (i == argc - 1) ? end : sep;
		for (size_t j = 0; j < ((i == argc - 1) ? endLen : sepLen); ++j) {
			fputc(thingToPrint[j], stdout);
		}
	}
	return NONE_VAL();
}

/* str.__len__() */
static KrkValue _string_len(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_STRING(argv[0])) return krk_runtimeError(vm.exceptions.attributeError,"Unexpected arguments to str.__len__()");
	return INTEGER_VAL(AS_STRING(argv[0])->codesLength);
}

/* str.__set__(ind,val) - this is invalid, throw a nicer error than 'field does not exist'. */
static KrkValue _strings_are_immutable(int argc, KrkValue argv[]) {
	return krk_runtimeError(vm.exceptions.typeError, "Strings are not mutable.");
}

/**
 * str.__getslice__(start,end)
 *
 * Unlike in Python, we actually handle negative values here rather than
 * somewhere else? I'm not even sure where Python does do it, but a quick
 * says not if you call __getslice__ directly...
 */
static KrkValue _string_getslice(int argc, KrkValue argv[]) {
	if (argc < 3) return krk_runtimeError(vm.exceptions.argumentError, "slice: expected 2 arguments, got %d", argc-1);
	if (!IS_STRING(argv[0]) ||
		!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) ||
		!(IS_INTEGER(argv[2]) || IS_NONE(argv[2])))
		return krk_runtimeError(vm.exceptions.typeError, "slice: expected two integer arguments");
	/* bounds check */
	KrkString * me = AS_STRING(argv[0]);
	long start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	long end   = IS_NONE(argv[2]) ? (long)me->codesLength : AS_INTEGER(argv[2]);
	if (start < 0) start = me->codesLength + start;
	if (start < 0) start = 0;
	if (end < 0) end = me->codesLength + end;
	if (start > (long)me->codesLength) start = me->codesLength;
	if (end > (long)me->codesLength) end = me->codesLength;
	if (end < start) end = start;
	long len = end - start;
	if (me->type == KRK_STRING_ASCII) {
		return OBJECT_VAL(krk_copyString(me->chars + start, len));
	} else {
		size_t offset = 0;
		size_t length = 0;
		/* Figure out where the UTF8 for this string starts. */
		krk_unicodeString(me);
		for (long i = 0; i < start; ++i) {
			uint32_t cp = KRK_STRING_FAST(me,i);
			offset += CODEPOINT_BYTES(cp);
		}
		for (long i = start; i < end; ++i) {
			uint32_t cp = KRK_STRING_FAST(me,i);
			length += CODEPOINT_BYTES(cp);
		}
		return OBJECT_VAL(krk_copyString(me->chars + offset, length));
	}
}

/* str.__int__(base=10) */
static KrkValue _string_int(int argc, KrkValue argv[]) {
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
	krk_integer_type value = parseStrInt(start, NULL, base);
	return INTEGER_VAL(value);
}

/* str.__float__() */
static KrkValue _string_float(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_STRING(argv[0])) return NONE_VAL();
	return FLOATING_VAL(strtod(AS_CSTRING(argv[0]),NULL));
}

static KrkValue _float_init(int argc, KrkValue argv[]) {
	if (argc < 1) return FLOATING_VAL(0.0);
	if (argc > 2) return krk_runtimeError(vm.exceptions.argumentError, "float() takes at most 1 argument");
	if (IS_STRING(argv[1])) return _string_float(1,&argv[1]);
	if (IS_FLOATING(argv[1])) return argv[1];
	if (IS_INTEGER(argv[1])) return FLOATING_VAL(AS_INTEGER(argv[1]));
	if (IS_BOOLEAN(argv[1])) return FLOATING_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions.typeError, "float() argument must be a string or a number, not '%s'", krk_typeName(argv[1]));
}

/* str.__get__(index) */
static KrkValue _string_get(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions.argumentError, "Wrong number of arguments to String.__get__");
	if (!IS_STRING(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "First argument to __get__ must be String");
	if (!IS_INTEGER(argv[1])) return krk_runtimeError(vm.exceptions.typeError, "String can not indexed by %s", krk_typeName(argv[1]));
	KrkString * me = AS_STRING(argv[0]);
	int asInt = AS_INTEGER(argv[1]);
	if (asInt < 0) asInt += (int)AS_STRING(argv[0])->codesLength;
	if (asInt < 0 || asInt >= (int)AS_STRING(argv[0])->codesLength) {
		return krk_runtimeError(vm.exceptions.indexError, "String index out of range: %d", asInt);
	}
	if (me->type == KRK_STRING_ASCII) {
		return OBJECT_VAL(krk_copyString(me->chars + asInt, 1));
	} else {
		size_t offset = 0;
		size_t length = 0;
		/* Figure out where the UTF8 for this string starts. */
		krk_unicodeString(me);
		for (long i = 0; i < asInt; ++i) {
			uint32_t cp = KRK_STRING_FAST(me,i);
			offset += CODEPOINT_BYTES(cp);
		}
		uint32_t cp = KRK_STRING_FAST(me,asInt);
		length = CODEPOINT_BYTES(cp);
		return OBJECT_VAL(krk_copyString(me->chars + offset, length));
	}
}

#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = GROW_ARRAY(char, stringBytes, old, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)
#define AT_END() (self->length == 0 || i == self->length - 1)

/* str.format(**kwargs) */
static KrkValue _string_format(int argc, KrkValue argv[], int hasKw) {
	CHECK_STRING();
	if (AS_STRING(argv[0])->type != KRK_STRING_ASCII) {
		return krk_runtimeError(vm.exceptions.notImplementedError, "Unable to call .format() on non-ASCII string.");
	}
	KrkString * self = AS_STRING(argv[0]);
	KrkValue kwargs = NONE_VAL();
	if (hasKw) {
		argc--; /* last arg is the keyword dictionary */
		kwargs = argv[argc];
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
					KrkClass * type = krk_getType(value);
					if (type->_tostr) {
						asString = krk_callSimple(OBJECT_VAL(type->_tostr), 1, 0);
					} else {
						if (!krk_bindMethod(type, AS_STRING(vm.specialMethodNames[METHOD_STR]))) {
							errorStr = "Failed to convert field to string.";
							goto _formatError;
						}
						asString = krk_callSimple(krk_peek(0), 0, 1);
					}
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

static KrkValue _string_mul(int argc, KrkValue argv[]) {
	CHECK_STRING();
	if (!IS_INTEGER(argv[1])) {
		return krk_runtimeError(vm.exceptions.typeError, "unsupported operand types for *: '%s' and '%s'",
			"list", krk_typeName(argv[1]));
	}

	krk_integer_type howMany = AS_INTEGER(argv[1]);

	krk_push(OBJECT_VAL(S("")));

	for (krk_integer_type i = 0; i < howMany; ++i) {
		krk_push(argv[0]);
		addObjects();
	}

	return krk_pop();
}

/* str.join(list) */
static KrkValue _string_join(int argc, KrkValue argv[], int hasKw) {
	CHECK_STRING();
	KrkString * self = AS_STRING(argv[0]);
	if (hasKw) return krk_runtimeError(vm.exceptions.argumentError, "str.join() does not take keyword arguments");
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "str.join(): expected exactly one argument");

	/* TODO fix this to use unpackArray and support other things than lists */
	if (!IS_INSTANCE(argv[1])) return krk_runtimeError(vm.exceptions.typeError, "str.join(): expected a list");

	const char * errorStr = NULL;

	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = 0;

	for (size_t i = 0; i < AS_LIST(argv[1])->count; ++i) {
		KrkValue value = AS_LIST(argv[1])->values[i];
		if (!IS_STRING(AS_LIST(argv[1])->values[i])) {
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
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "__contains__ expects an argument");
	if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < AS_STRING(argv[0])->length; ++i) {
		if (substringMatch(AS_CSTRING(argv[0]) + i, AS_STRING(argv[0])->length - i, AS_CSTRING(argv[1]), AS_STRING(argv[1])->length)) {
			return BOOLEAN_VAL(1);
		}
	}
	return BOOLEAN_VAL(0);
}

static int charIn(char c, const char * str) {
	for (const char * s = str; *s; s++) {
		if (c == *s) return 1;
	}
	return 0;
}

/**
 * Implements all three of strip, lstrip, rstrip.
 * Set which = 0, 1, 2 respectively
 */
static KrkValue _string_strip_shared(int argc, KrkValue argv[], int which) {
	CHECK_STRING();
	if (argc > 1 && IS_STRING(argv[1]) && AS_STRING(argv[1])->type != KRK_STRING_ASCII) {
		return krk_runtimeError(vm.exceptions.notImplementedError, "str.strip() not implemented for Unicode strip lists");
	}
	size_t start = 0;
	size_t end   = AS_STRING(argv[0])->length;
	const char * subset = " \t\n\r";
	if (argc > 1) {
		if (IS_STRING(argv[1])) {
			subset = AS_CSTRING(argv[1]);
		} else {
			return krk_runtimeError(vm.exceptions.typeError, "argument to %sstrip() should be a string",
				(which == 0 ? "" : (which == 1 ? "l" : "r")));
		}
	} else if (argc > 2) {
		return krk_runtimeError(vm.exceptions.typeError, "%sstrip() takes at most one argument",
			(which == 0 ? "" : (which == 1 ? "l" : "r")));
	}
	if (which < 2) while (start < end && charIn(AS_CSTRING(argv[0])[start], subset)) start++;
	if (which != 1) while (end > start && charIn(AS_CSTRING(argv[0])[end-1], subset)) end--;
	return OBJECT_VAL(krk_copyString(&AS_CSTRING(argv[0])[start], end-start));
}

static KrkValue _string_strip(int argc, KrkValue argv[]) {
	return _string_strip_shared(argc,argv,0);
}

static KrkValue _string_lstrip(int argc, KrkValue argv[]) {
	return _string_strip_shared(argc,argv,1);
}

static KrkValue _string_rstrip(int argc, KrkValue argv[]) {
	return _string_strip_shared(argc,argv,2);
}

static KrkValue _string_lt(int argc, KrkValue argv[]) {
	CHECK_STRING();
	if (argc < 2 || !IS_STRING(argv[1])) {
		return KWARGS_VAL(0); /* represents 'not implemented' */
	}

	size_t aLen = AS_STRING(argv[0])->length;
	size_t bLen = AS_STRING(argv[1])->length;
	const char * a = AS_CSTRING(argv[0]);
	const char * b = AS_CSTRING(argv[1]);

	for (size_t i = 0; i < (aLen < bLen) ? aLen : bLen; i++) {
		if (a[i] < b[i]) return BOOLEAN_VAL(1);
		if (a[i] > b[i]) return BOOLEAN_VAL(0);
	}

	return BOOLEAN_VAL((aLen < bLen));
}

static KrkValue _string_gt(int argc, KrkValue argv[]) {
	CHECK_STRING();
	if (argc < 2 || !IS_STRING(argv[1])) {
		return KWARGS_VAL(0); /* represents 'not implemented' */
	}

	size_t aLen = AS_STRING(argv[0])->length;
	size_t bLen = AS_STRING(argv[1])->length;
	const char * a = AS_CSTRING(argv[0]);
	const char * b = AS_CSTRING(argv[1]);

	for (size_t i = 0; i < (aLen < bLen) ? aLen : bLen; i++) {
		if (a[i] < b[i]) return BOOLEAN_VAL(0);
		if (a[i] > b[i]) return BOOLEAN_VAL(1);
	}

	return BOOLEAN_VAL((aLen > bLen));
}

/** TODO but throw a more descriptive error for now */
static KrkValue _string_mod(int argc, KrkValue argv[]) {
	return krk_runtimeError(vm.exceptions.notImplementedError, "%%-formatting for strings is not yet available");
}

/* str.split() */
static KrkValue _string_split(int argc, KrkValue argv[], int hasKw) {
	CHECK_STRING();
	KrkString * self = AS_STRING(argv[0]);
	if (argc > 1) {
		if (!IS_STRING(argv[1])) {
			return krk_runtimeError(vm.exceptions.typeError, "Expected separator to be a string");
		} else if (AS_STRING(argv[1])->length == 0) {
			return krk_runtimeError(vm.exceptions.valueError, "Empty separator");
		}
		if (argc > 2 && !IS_INTEGER(argv[2])) {
			return krk_runtimeError(vm.exceptions.typeError, "Expected maxsplit to be an integer.");
		} else if (argc > 2 && AS_INTEGER(argv[2]) == 0) {
			return argv[0];
		}
	}

	KrkValue myList = krk_list_of(0,NULL);
	krk_push(myList);

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
				FREE_ARRAY(char,stringBytes,stringCapacity);
				krk_push(tmp);
				krk_writeValueArray(AS_LIST(myList), tmp);
				krk_pop();
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
					krk_writeValueArray(AS_LIST(myList), OBJECT_VAL(krk_copyString(stringBytes, stringLength)));
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
			if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
			krk_push(tmp);
			krk_writeValueArray(AS_LIST(myList), tmp);
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
					if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
					krk_push(tmp);
					krk_writeValueArray(AS_LIST(myList), tmp);
					krk_pop();
					break;
				}
				if (i == self->length) {
					KrkValue tmp = OBJECT_VAL(S(""));
					krk_push(tmp);
					krk_writeValueArray(AS_LIST(myList), tmp);
					krk_pop();
				}
			}
		}
	}

	krk_pop();
	return myList;
}

/**
 * str.__repr__()
 *
 * Strings are special because __str__ should do nothing but __repr__
 * should escape characters like quotes.
 */
static KrkValue _string_repr(int argc, KrkValue argv[]) {
	CHECK_STRING();
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = NULL;

	char * end = AS_CSTRING(argv[0]) + AS_STRING(argv[0])->length;

	/* First count quotes */
	size_t singles = 0;
	size_t doubles = 0;
	for (char * c = AS_CSTRING(argv[0]); c < end; ++c) {
		if (*c == '\'') singles++;
		if (*c == '\"') doubles++;
	}

	char quote = (singles > doubles) ? '\"' : '\'';

	PUSH_CHAR(quote);

	for (char * c = AS_CSTRING(argv[0]); c < end; ++c) {
		switch (*c) {
			/* XXX: Other non-printables should probably be escaped as well. */
			case '\\': PUSH_CHAR('\\'); PUSH_CHAR('\\'); break;
			case '\'': if (quote == *c) { PUSH_CHAR('\\'); } PUSH_CHAR('\''); break;
			case '\"': if (quote == *c) { PUSH_CHAR('\\'); } PUSH_CHAR('\"'); break;
			case '\a': PUSH_CHAR('\\'); PUSH_CHAR('a'); break;
			case '\b': PUSH_CHAR('\\'); PUSH_CHAR('b'); break;
			case '\f': PUSH_CHAR('\\'); PUSH_CHAR('f'); break;
			case '\n': PUSH_CHAR('\\'); PUSH_CHAR('n'); break;
			case '\r': PUSH_CHAR('\\'); PUSH_CHAR('r'); break;
			case '\t': PUSH_CHAR('\\'); PUSH_CHAR('t'); break;
			case '\v': PUSH_CHAR('\\'); PUSH_CHAR('v'); break;
			case 27:   PUSH_CHAR('\\'); PUSH_CHAR('['); break;
			default: {
				if ((unsigned char)*c < ' ' || (unsigned char)*c == 0x7F) {
					PUSH_CHAR('\\');
					PUSH_CHAR('x');
					char hex[3];
					sprintf(hex,"%02x", (unsigned char)*c);
					PUSH_CHAR(hex[0]);
					PUSH_CHAR(hex[1]);
				} else {
					PUSH_CHAR(*c);
				}
				break;
			}
		}
	}

	PUSH_CHAR(quote);
	KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
	return tmp;
}

static KrkValue _string_encode(int argc, KrkValue argv[]) {
	CHECK_STRING();
	return OBJECT_VAL(krk_newBytes(AS_STRING(argv[0])->length, (uint8_t*)AS_CSTRING(argv[0])));
}

static KrkValue _bytes_init(int argc, KrkValue argv[]) {
	if (argc == 1) {
		return OBJECT_VAL(krk_newBytes(0,NULL));
	}

	if (IS_TUPLE(argv[1])) {
		KrkBytes * out = krk_newBytes(AS_TUPLE(argv[1])->values.count, NULL);
		krk_push(OBJECT_VAL(out));
		for (size_t i = 0; i < AS_TUPLE(argv[1])->values.count; ++i) {
			if (!IS_INTEGER(AS_TUPLE(argv[1])->values.values[i])) {
				return krk_runtimeError(vm.exceptions.typeError, "bytes(): expected tuple of ints, not of '%s'", krk_typeName(AS_TUPLE(argv[1])->values.values[i]));
			}
			out->bytes[i] = AS_INTEGER(AS_TUPLE(argv[1])->values.values[i]);
		}
		krk_bytesUpdateHash(out);
		return krk_pop();
	}

	return krk_runtimeError(vm.exceptions.typeError, "Can not convert '%s' to bytes", krk_typeName(argv[1]));
}

/* bytes objects are not interned; need to do this the old-fashioned way. */
static KrkValue _bytes_eq(int argc, KrkValue argv[]) {
	if (!IS_BYTES(argv[1])) return BOOLEAN_VAL(0);
	KrkBytes * self = AS_BYTES(argv[0]);
	KrkBytes * them = AS_BYTES(argv[1]);
	if (self->length != them->length) return BOOLEAN_VAL(0);
	if (self->hash != them->hash) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->length; ++i) {
		if (self->bytes[i] != them->bytes[i]) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

static KrkValue _bytes_repr(int argc, KrkValue argv[]) {
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = NULL;

	PUSH_CHAR('b');
	PUSH_CHAR('\'');

	for (size_t i = 0; i < AS_BYTES(argv[0])->length; ++i) {
		uint8_t ch = AS_BYTES(argv[0])->bytes[i];
		switch (ch) {
			case '\\': PUSH_CHAR('\\'); PUSH_CHAR('\\'); break;
			case '\'': PUSH_CHAR('\\'); PUSH_CHAR('\''); break;
			case '\a': PUSH_CHAR('\\'); PUSH_CHAR('a'); break;
			case '\b': PUSH_CHAR('\\'); PUSH_CHAR('b'); break;
			case '\f': PUSH_CHAR('\\'); PUSH_CHAR('f'); break;
			case '\n': PUSH_CHAR('\\'); PUSH_CHAR('n'); break;
			case '\r': PUSH_CHAR('\\'); PUSH_CHAR('r'); break;
			case '\t': PUSH_CHAR('\\'); PUSH_CHAR('t'); break;
			case '\v': PUSH_CHAR('\\'); PUSH_CHAR('v'); break;
			default: {
				if (ch < ' ' || ch >= 0x7F) {
					PUSH_CHAR('\\');
					PUSH_CHAR('x');
					char hex[3];
					sprintf(hex,"%02x", ch);
					PUSH_CHAR(hex[0]);
					PUSH_CHAR(hex[1]);
				} else {
					PUSH_CHAR(ch);
				}
				break;
			}
		}
	}

	PUSH_CHAR('\'');

	KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
	return tmp;
}

static KrkValue _bytes_get(int argc, KrkValue argv[]) {
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "bytes.__get__(): expected one argument");
	KrkBytes * self = AS_BYTES(argv[0]);
	long asInt = AS_INTEGER(argv[1]);

	if (asInt < 0) asInt += (long)self->length;
	if (asInt < 0 || asInt >= (long)self->length) {
		return krk_runtimeError(vm.exceptions.indexError, "bytes index out of range: %ld", asInt);
	}

	return INTEGER_VAL(self->bytes[asInt]);
}

static KrkValue _bytes_len(int argc, KrkValue argv[]) {
	return INTEGER_VAL(AS_BYTES(argv[0])->length);
}

static KrkValue _bytes_contains(int argc, KrkValue argv[]) {
	if (argc < 2) krk_runtimeError(vm.exceptions.argumentError, "bytes.__contains__(): expected one argument");
	return krk_runtimeError(vm.exceptions.notImplementedError, "not implemented");
}

static KrkValue _bytes_decode(int argc, KrkValue argv[]) {
	/* TODO: Actually bother checking if this explodes, or support other encodings... */
	return OBJECT_VAL(krk_copyString((char*)AS_BYTES(argv[0])->bytes, AS_BYTES(argv[0])->length));
}

#undef PUSH_CHAR

static KrkValue _int_init(int argc, KrkValue argv[]) {
	if (argc < 2) return INTEGER_VAL(0);
	if (IS_INTEGER(argv[1])) return argv[1];
	if (IS_STRING(argv[1])) return _string_int(argc-1,&argv[1]);
	if (IS_FLOATING(argv[1])) return INTEGER_VAL(AS_FLOATING(argv[1]));
	if (IS_BOOLEAN(argv[1])) return INTEGER_VAL(AS_BOOLEAN(argv[1]));
	return krk_runtimeError(vm.exceptions.typeError, "int() argument must be a string or a number, not '%s'", krk_typeName(argv[1]));
}

/* function.__doc__ */
static KrkValue _closure_get_doc(int argc, KrkValue argv[]) {
	if (IS_NATIVE(argv[0]) && AS_NATIVE(argv[0])->doc) {
		return OBJECT_VAL(krk_copyString(AS_NATIVE(argv[0])->doc, strlen(AS_NATIVE(argv[0])->doc)));
	} else if (IS_CLOSURE(argv[0]) && AS_CLOSURE(argv[0])->function->docstring) {
		return  OBJECT_VAL(AS_CLOSURE(argv[0])->function->docstring);
	} else {
		return NONE_VAL();
	}
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

	const char * typeName = krk_typeName(AS_BOUND_METHOD(argv[0])->receiver);

	size_t len = AS_STRING(s)->length + sizeof("<method >") + strlen(typeName) + 1;
	char * tmp = malloc(len);
	sprintf(tmp, "<method %s.%s>", typeName, AS_CSTRING(s));
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

/* function.__args__ */
static KrkValue _closure_get_argnames(int argc, KrkValue argv[]) {
	if (!IS_CLOSURE(argv[0])) return OBJECT_VAL(krk_newTuple(0));
	KrkFunction * self = AS_CLOSURE(argv[0])->function;
	KrkTuple * tuple = krk_newTuple(self->requiredArgs + self->keywordArgs);
	krk_push(OBJECT_VAL(tuple));
	for (short i = 0; i < self->requiredArgs; ++i) {
		tuple->values.values[tuple->values.count++] = self->requiredArgNames.values[i];
	}
	for (short i = 0; i < self->keywordArgs; ++i) {
		tuple->values.values[tuple->values.count++] = self->keywordArgNames.values[i];
	}
	krk_pop();
	return OBJECT_VAL(tuple);
}

static KrkValue _bound_get_argnames(int argc, KrkValue argv[]) {
	KrkBoundMethod * boundMethod = AS_BOUND_METHOD(argv[0]);
	return _closure_get_argnames(1, (KrkValue[]){OBJECT_VAL(boundMethod->method)});
}

static KrkValue _tuple_init(int argc, KrkValue argv[]) {
	return krk_runtimeError(vm.exceptions.typeError,"tuple() initializier unsupported");
}

/* tuple creator */
static KrkValue _tuple_of(int argc, KrkValue argv[]) {
	KrkTuple * self = krk_newTuple(argc);
	krk_push(OBJECT_VAL(self));
	for (size_t i = 0; i < (size_t)argc; ++i) {
		self->values.values[self->values.count++] = argv[i];
	}
	krk_pop();
	return OBJECT_VAL(self);
}

static KrkValue _tuple_contains(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions.argumentError, "tuple.__contains__ expects one argument");
	KrkTuple * self = AS_TUPLE(argv[0]);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

/* tuple.__len__ */
static KrkValue _tuple_len(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "tuple.__len__ does not expect arguments");
	KrkTuple * self = AS_TUPLE(argv[0]);
	return INTEGER_VAL(self->values.count);
}

/* tuple.__get__ */
static KrkValue _tuple_get(int argc, KrkValue argv[]) {
	if (argc != 2) return krk_runtimeError(vm.exceptions.argumentError, "tuple.__get__ expects one argument");
	else if (!IS_INTEGER(argv[1])) return krk_runtimeError(vm.exceptions.typeError, "can not index by '%s', expected integer", krk_typeName(argv[1]));
	KrkTuple * tuple = AS_TUPLE(argv[0]);
	long index = AS_INTEGER(argv[1]);
	if (index < 0) index += tuple->values.count;
	if (index < 0 || index >= (long)tuple->values.count) {
		return krk_runtimeError(vm.exceptions.indexError, "tuple index out of range");
	}
	return tuple->values.values[index];
}

static KrkValue _tuple_eq(int argc, KrkValue argv[]) {
	if (!IS_TUPLE(argv[1])) return BOOLEAN_VAL(0);
	KrkTuple * self = AS_TUPLE(argv[0]);
	KrkTuple * them = AS_TUPLE(argv[1]);
	if (self->values.count != them->values.count) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (!krk_valuesEqual(self->values.values[i], them->values.values[i])) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

static KrkValue _tuple_repr(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "tuple.__repr__ does not expect arguments");
	KrkTuple * tuple = AS_TUPLE(argv[0]);
	if (((KrkObj*)tuple)->inRepr) return OBJECT_VAL(S("(...)"));
	((KrkObj*)tuple)->inRepr = 1;
	/* String building time. */
	krk_push(OBJECT_VAL(S("(")));

	for (size_t i = 0; i < tuple->values.count; ++i) {
		krk_push(tuple->values.values[i]);
		krk_push(krk_callSimple(OBJECT_VAL(krk_getType(tuple->values.values[i])->_reprer), 1, 0));
		addObjects(); /* pops both, pushes result */
		if (i != tuple->values.count - 1) {
			krk_push(OBJECT_VAL(S(", ")));
			addObjects();
		}
	}

	if (tuple->values.count == 1) {
		krk_push(OBJECT_VAL(S(",")));
		addObjects();
	}

	krk_push(OBJECT_VAL(S(")")));
	addObjects();
	((KrkObj*)tuple)->inRepr = 0;
	return krk_pop();
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
	KrkClass * type = krk_getType(argv[0]);
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

static KrkValue _module_repr(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	KrkValue name = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_NAME], &name);

	if (!IS_STRING(name)) {
		return OBJECT_VAL(S("<module>"));
	}

	KrkValue file = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_FILE], &file);

	char * tmp = malloc(50 + AS_STRING(name)->length + (IS_STRING(file) ? AS_STRING(file)->length : 20));
	if (IS_STRING(file)) {
		sprintf(tmp, "<module '%s' from '%s'>", AS_CSTRING(name), AS_CSTRING(file));
	} else {
		sprintf(tmp, "<module '%s' (built-in)>", AS_CSTRING(name));
	}

	KrkValue out = OBJECT_VAL(krk_copyString(tmp, strlen(tmp)));
	free(tmp);
	return out;
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
	size_t l = sprintf(tmp, PRIkrk_int, (krk_integer_type)AS_INTEGER(argv[0]));
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
	switch (value.type) {
		case VAL_NONE: return 1;
		case VAL_BOOLEAN: return !AS_BOOLEAN(value);
		case VAL_INTEGER: return !AS_INTEGER(value);
		case VAL_FLOATING: return !AS_FLOATING(value);
		case VAL_OBJECT: {
			switch (AS_OBJECT(value)->type) {
				case OBJ_STRING: return !AS_STRING(value)->codesLength;
				case OBJ_TUPLE: return !AS_TUPLE(value)->values.count;
				default: break;
			}
		}
		default: break;
	}
	KrkClass * type = krk_getType(value);

	/* If it has a length, and that length is 0, it's Falsey */
	if (type->_len) {
		krk_push(value);
		return !AS_INTEGER(krk_callSimple(OBJECT_VAL(type->_len),1,0));
	}
	return 0; /* Assume anything else is truthy */
}

static KrkValue _bool_init(int argc, KrkValue argv[]) {
	if (argc < 2) return BOOLEAN_VAL(0);
	if (argc > 2) return krk_runtimeError(vm.exceptions.argumentError, "bool() takes at most 1 argument");
	return BOOLEAN_VAL(!isFalsey(argv[1]));
}

/**
 * None.__str__() -> "None"
 */
static KrkValue _none_to_str(int argc, KrkValue argv[]) {
	return OBJECT_VAL(S("None"));
}

static KrkValue _len(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "len() takes exactly one argument");
	/* Shortcuts */
	if (IS_STRING(argv[0])) return INTEGER_VAL(AS_STRING(argv[0])->codesLength);
	if (IS_TUPLE(argv[0])) return INTEGER_VAL(AS_TUPLE(argv[0])->values.count);

	KrkClass * type = krk_getType(argv[0]);
	if (!type->_len) return krk_runtimeError(vm.exceptions.typeError, "object of type '%s' has no len()", krk_typeName(argv[0]));
	krk_push(argv[0]);

	return krk_callSimple(OBJECT_VAL(type->_len), 1, 0);
}

static KrkValue _dir(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "dir() takes exactly one argument");
	KrkClass * type = krk_getType(argv[0]);
	if (!type->_dir) {
		return krk_dirObject(argc,argv); /* Fallback */
	}
	krk_push(argv[0]);
	return krk_callSimple(OBJECT_VAL(type->_dir), 1, 0);
}

static KrkValue _repr(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "repr() takes exactly one argument");

	/* Everything should have a __repr__ */
	KrkClass * type = krk_getType(argv[0]);
	krk_push(argv[0]);
	return krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
}

static KrkValue _ord(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "ord() takes exactly one argument");

	KrkClass * type = krk_getType(argv[0]);
	KrkValue method;
	if (krk_tableGet(&type->methods, vm.specialMethodNames[METHOD_ORD], &method)) {
		krk_push(argv[0]);
		return krk_callSimple(method, 1, 0);
	}
	return krk_runtimeError(vm.exceptions.argumentError, "ord() expected string of length 1, but got %s", krk_typeName(argv[0]));
}

static KrkValue _chr(int argc, KrkValue argv[]) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError, "chr() takes exactly one argument");

	KrkClass * type = krk_getType(argv[0]);
	KrkValue method;
	if (krk_tableGet(&type->methods, vm.specialMethodNames[METHOD_CHR], &method)) {
		krk_push(argv[0]);
		return krk_callSimple(method, 1, 0);
	}
	return krk_runtimeError(vm.exceptions.argumentError, "chr() expected an integer, but got %s", krk_typeName(argv[0]));
}

static KrkValue _hex(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_INTEGER(argv[0])) return krk_runtimeError(vm.exceptions.argumentError, "hex() expects one int argument");
	char tmp[20];
	krk_integer_type x = AS_INTEGER(argv[0]);
	size_t len = sprintf(tmp, "%s0x" PRIkrk_hex, x < 0 ? "-" : "", x < 0 ? -x : x);
	return OBJECT_VAL(krk_copyString(tmp,len));
}

static KrkValue _any(int argc, KrkValue argv[]) {
#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (!isFalsey(indexer)) return BOOLEAN_VAL(1); \
	} \
} while (0)
	KrkValue value = argv[0];
	if (IS_TUPLE(value)) {
		unpackArray(AS_TUPLE(value)->values.count, AS_TUPLE(value)->values.values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.listClass) {
		unpackArray(AS_LIST(value)->count, AS_LIST(value)->values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.dictClass) {
		unpackArray(AS_DICT(value)->count, _dict_nth_key_fast(AS_DICT(value)->capacity, AS_DICT(value)->entries, i));
	} else if (IS_STRING(value)) {
		unpackArray(AS_STRING(value)->codesLength, _string_get(2,(KrkValue[]){value,INTEGER_VAL(i)}));
	} else {
		KrkClass * type = krk_getType(argv[0]);
		if (type->_iter) {
			/* Create the iterator */
			size_t stackOffset = vm.stackTop - vm.stack;
			krk_push(argv[1]);
			krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

			do {
				/* Call it until it gives us itself */
				krk_push(vm.stack[stackOffset]);
				krk_push(krk_callSimple(krk_peek(0), 0, 1));
				if (krk_valuesSame(vm.stack[stackOffset], krk_peek(0))) {
					/* We're done. */
					krk_pop(); /* The result of iteration */
					krk_pop(); /* The iterator */
					break;
				}
				if (!isFalsey(krk_peek(0))) {
					krk_pop();
					krk_pop();
					return BOOLEAN_VAL(1);
				}
				krk_pop();
			} while (1);
		} else {
			return krk_runtimeError(vm.exceptions.typeError, "'%s' object is not iterable", krk_typeName(value));
		}
	}
#undef unpackArray
	return BOOLEAN_VAL(0);
}

static KrkValue _all(int argc, KrkValue argv[]) {
#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (isFalsey(indexer)) return BOOLEAN_VAL(0); \
	} \
} while (0)
	KrkValue value = argv[0];
	if (IS_TUPLE(value)) {
		unpackArray(AS_TUPLE(value)->values.count, AS_TUPLE(value)->values.values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.listClass) {
		unpackArray(AS_LIST(value)->count, AS_LIST(value)->values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.dictClass) {
		unpackArray(AS_DICT(value)->count, _dict_nth_key_fast(AS_DICT(value)->capacity, AS_DICT(value)->entries, i));
	} else if (IS_STRING(value)) {
		unpackArray(AS_STRING(value)->codesLength, _string_get(2,(KrkValue[]){value,INTEGER_VAL(i)}));
	} else {
		KrkClass * type = krk_getType(argv[0]);
		if (type->_iter) {
			/* Create the iterator */
			size_t stackOffset = vm.stackTop - vm.stack;
			krk_push(argv[1]);
			krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

			do {
				/* Call it until it gives us itself */
				krk_push(vm.stack[stackOffset]);
				krk_push(krk_callSimple(krk_peek(0), 0, 1));
				if (krk_valuesSame(vm.stack[stackOffset], krk_peek(0))) {
					/* We're done. */
					krk_pop(); /* The result of iteration */
					krk_pop(); /* The iterator */
					break;
				}
				if (isFalsey(krk_peek(0))) {
					krk_pop();
					krk_pop();
					return BOOLEAN_VAL(0);
				}
				krk_pop();
			} while (1);
		} else {
			return krk_runtimeError(vm.exceptions.typeError, "'%s' object is not iterable", krk_typeName(value));
		}
	}
#undef unpackArray
	return BOOLEAN_VAL(1);
}

struct DictItems {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

static void _dictitems_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictItems*)self)->dict);
}

static KrkValue _dictitems_init(int argc, KrkValue argv[]) {
	struct DictItems * self = (struct DictItems*)AS_OBJECT(argv[0]);
	self->dict = argv[1];
	self->i = 0;
	return argv[0];
}

static KrkValue _dictitems_iter(int argc, KrkValue argv[]) {
	/* Reset index and return self as iteration object */
	struct DictItems * self = (struct DictItems*)AS_OBJECT(argv[0]);
	self->i = 0;
	return argv[0];
}

static KrkValue _dictitems_call(int argc, KrkValue argv[]) {
	struct DictItems * self = (struct DictItems*)AS_OBJECT(argv[0]);
	do {
		if (self->i >= AS_DICT(self->dict)->capacity) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			KrkTuple * outValue = krk_newTuple(2);
			krk_push(OBJECT_VAL(outValue));
			outValue->values.values[0] = AS_DICT(self->dict)->entries[self->i].key;
			outValue->values.values[1] = AS_DICT(self->dict)->entries[self->i].value;
			outValue->values.count = 2;
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

/* TODO: dictitems could really use a nice repr */
static KrkValue _dict_items(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.dictitemsClass);
	krk_push(OBJECT_VAL(output));
	_dictitems_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();
	return OBJECT_VAL(output);
}

struct DictKeys {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

static void _dictkeys_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictKeys*)self)->dict);
}

static KrkValue _dictkeys_init(int argc, KrkValue argv[]) {
	struct DictKeys * self = (struct DictKeys*)AS_OBJECT(argv[0]);
	self->dict = argv[1];
	self->i = 0;
	return argv[0];
}

static KrkValue _dictkeys_iter(int argc, KrkValue argv[]) {
	/* reset indext and return self as iteration object */
	struct DictKeys * self = (struct DictKeys*)AS_OBJECT(argv[0]);
	self->i = 0;
	return argv[0];
}

static KrkValue _dictkeys_call(int argc, KrkValue argv[]) {
	struct DictKeys * self = (struct DictKeys*)AS_OBJECT(argv[0]);
	do {
		if (self->i >= AS_DICT(self->dict)->capacity) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			krk_push(AS_DICT(self->dict)->entries[self->i].key);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

/* TODO: dictkeys could really use a nice repr */
static KrkValue _dict_keys(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.dictkeysClass);
	krk_push(OBJECT_VAL(output));
	_dictkeys_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();
	return OBJECT_VAL(output);
}

struct TupleIter {
	KrkInstance inst;
	KrkValue myTuple;
	int i;
};

static KrkValue _tuple_iter_init(int argc, KrkValue argv[]) {
	struct TupleIter * self = (struct TupleIter *)AS_OBJECT(argv[0]);
	self->myTuple = argv[0];
	self->i = 0;
	return argv[0];
}

static void _tuple_iter_gcscan(KrkInstance * self) {
	krk_markValue(((struct TupleIter*)self)->myTuple);
}

static KrkValue _tuple_iter_call(int argc, KrkValue argv[]) {
	struct TupleIter * self = (struct TupleIter *)AS_OBJECT(argv[0]);
	KrkValue t = self->myTuple; /* Tuple to iterate */
	int i = self->i;
	if (i >= (krk_integer_type)AS_TUPLE(t)->values.count) {
		return argv[0];
	} else {
		self->i = i+1;
		return AS_TUPLE(t)->values.values[i];
	}
}

static KrkValue _tuple_iter(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.tupleiteratorClass);
	krk_push(OBJECT_VAL(output));
	_tuple_iter_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();
	return OBJECT_VAL(output);
}

static KrkValue _striter_init(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.striteratorClass) {
		return krk_runtimeError(vm.exceptions.typeError, "Tried to call striterator.__init__() on something not a str iterator");
	}
	if (argc < 2 || !IS_STRING(argv[1])) {
		return krk_runtimeError(vm.exceptions.argumentError, "Expected a str.");
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
		return krk_runtimeError(vm.exceptions.typeError, "Tried to call striterator.__call__() on something not a str iterator");
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

	if ((size_t)AS_INTEGER(_counter) >= AS_STRING(_str)->codesLength) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return _string_get(2,(KrkValue[]){_str,_counter});
	}

_corrupt:
	return krk_runtimeError(vm.exceptions.typeError, "Corrupt str iterator: %s", errorStr);
}

static KrkValue _string_iter(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.striteratorClass);

	krk_push(OBJECT_VAL(output));
	_striter_init(3, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();

	return OBJECT_VAL(output);
}

static KrkValue _listiter_init(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.listiteratorClass) {
		return krk_runtimeError(vm.exceptions.typeError, "Tried to call listiterator.__init__() on something not a list iterator");
	}
	if (argc < 2 || !IS_INSTANCE(argv[1])) {
		return krk_runtimeError(vm.exceptions.argumentError, "Expected a list.");
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
		return krk_runtimeError(vm.exceptions.typeError, "Tried to call listiterator.__call__() on something not a list iterator");
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

	if ((size_t)AS_INTEGER(_counter) >= AS_LIST(_list)->count) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return AS_LIST(_list)->values[AS_INTEGER(_counter)];
	}

_corrupt:
	return krk_runtimeError(vm.exceptions.typeError, "Corrupt list iterator: %s", errorStr);
}

static KrkValue _list_iter(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.listiteratorClass);

	krk_push(OBJECT_VAL(output));
	_listiter_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();

	return OBJECT_VAL(output);
}

struct Range {
	KrkInstance inst;
	krk_integer_type min;
	krk_integer_type max;
};

static KrkValue _range_init(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);
	if (argc < 2 || argc > 3) {
		return krk_runtimeError(vm.exceptions.argumentError, "range expected at least 1 and and at most 2 arguments");
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
		return krk_runtimeError(vm.exceptions.typeError, "range: expected int, but got '%s'", krk_typeName(min));
	}
	if (!IS_INTEGER(max)) {
		return krk_runtimeError(vm.exceptions.typeError, "range: expected int, but got '%s'", krk_typeName(max));
	}

	((struct Range*)self)->min = AS_INTEGER(min);
	((struct Range*)self)->max = AS_INTEGER(max);

	return argv[0];
}

static KrkValue _range_repr(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	krk_integer_type min = ((struct Range*)self)->min;
	krk_integer_type max = ((struct Range*)self)->max;

	krk_push(OBJECT_VAL(S("range({},{})")));
	KrkValue output = _string_format(3, (KrkValue[]){krk_peek(0), INTEGER_VAL(min), INTEGER_VAL(max)}, 0);
	krk_pop();
	return output;
}

struct RangeIterator {
	KrkInstance inst;
	krk_integer_type i;
	krk_integer_type max;
};

static KrkValue _rangeiterator_init(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	((struct RangeIterator*)self)->i = AS_INTEGER(argv[1]);
	((struct RangeIterator*)self)->max = AS_INTEGER(argv[2]);

	return argv[0];
}

static KrkValue _rangeiterator_call(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	krk_integer_type i, max;
	i = ((struct RangeIterator*)self)->i;
	max = ((struct RangeIterator*)self)->max;

	if (i >= max) {
		return argv[0];
	} else {
		((struct RangeIterator*)self)->i = i + 1;
		return INTEGER_VAL(i);
	}
}

static KrkValue _range_iter(int argc, KrkValue argv[]) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	KrkInstance * output = krk_newInstance(vm.baseClasses.rangeiteratorClass);
	krk_integer_type min = ((struct Range*)self)->min;
	krk_integer_type max = ((struct Range*)self)->max;

	krk_push(OBJECT_VAL(output));
	_rangeiterator_init(3, (KrkValue[]){krk_peek(0), INTEGER_VAL(min), INTEGER_VAL(max)});
	krk_pop();

	return OBJECT_VAL(output);
}

static KrkValue krk_collectGarbage_wrapper(int argc, KrkValue argv[]) {
	return INTEGER_VAL(krk_collectGarbage());
}

static KrkValue krk_getsize(int argc, KrkValue argv[]) {
	if (argc < 1) return INTEGER_VAL(0);
	if (!IS_OBJECT(argv[0])) return INTEGER_VAL(sizeof(KrkValue));
	size_t mySize = sizeof(KrkValue);
	switch (AS_OBJECT(argv[0])->type) {
		case OBJ_STRING: {
			KrkString * self = AS_STRING(argv[0]);
			mySize += sizeof(KrkString) + self->length /* For the UTF8 */
			+ ((self->codes && (self->chars != self->codes)) ? (self->type * self->codesLength) : 0);
			break;
		}
		case OBJ_BYTES: {
			KrkBytes * self = AS_BYTES(argv[0]);
			mySize += sizeof(KrkBytes) + self->length;
			break;
		}
		case OBJ_INSTANCE: {
			KrkInstance * self = AS_INSTANCE(argv[0]);
			mySize += sizeof(KrkInstance) + sizeof(KrkTableEntry) * self->fields.capacity;
			break;
		}
		case OBJ_CLASS: {
			KrkClass * self = AS_CLASS(argv[0]);
			mySize += sizeof(KrkClass) + sizeof(KrkTableEntry) * self->fields.capacity
			+ sizeof(KrkTableEntry) * self->methods.capacity;
			break;
		}
		case OBJ_NATIVE: {
			KrkNative * self = (KrkNative*)AS_OBJECT(argv[0]);
			mySize += sizeof(KrkNative) + strlen(self->name) + 1;
			break;
		}
		case OBJ_TUPLE: {
			KrkTuple * self = AS_TUPLE(argv[0]);
			mySize += sizeof(KrkTuple) + sizeof(KrkValue) * self->values.capacity;
			break;
		}
		case OBJ_BOUND_METHOD: {
			mySize += sizeof(KrkBoundMethod);
			break;
		}
		case OBJ_CLOSURE: {
			KrkClosure * self = AS_CLOSURE(argv[0]);
			mySize += sizeof(KrkClosure) + sizeof(KrkUpvalue*) * self->function->upvalueCount;
			break;
		}
		default: break;
	}
	return INTEGER_VAL(mySize);
}

static KrkValue krk_setclean(int argc, KrkValue argv[]) {
	if (!argc || (IS_BOOLEAN(argv[0]) && AS_BOOLEAN(argv[0]))) {
		vm.flags |= KRK_NO_ESCAPE;
	} else {
		vm.flags &= ~KRK_NO_ESCAPE;
	}
	return NONE_VAL();
}

void krk_initVM(int flags) {
	vm.flags = flags;
	KRK_PAUSE_GC();

	/* No active module or globals table */
	vm.module = NULL;

	krk_resetStack();
	vm.objects = NULL;
	vm.bytesAllocated = 0;
	vm.nextGC = 1024 * 1024;
	vm.grayCount = 0;
	vm.grayCapacity = 0;
	vm.grayStack = NULL;
	vm.objectClass = NULL;
	vm.moduleClass = NULL;
	krk_initTable(&vm.strings);
	memset(vm.specialMethodNames,0,sizeof(vm.specialMethodNames));
	vm.watchdog = 0;

	/*
	 * To make lookup faster, store these so we can don't have to keep boxing
	 * and unboxing, copying/hashing etc.
	 */
	struct { const char * s; size_t len; } _methods[] = {
	#define _(m,s) [m] = {s,sizeof(s)-1}
		_(METHOD_INIT, "__init__"),
		_(METHOD_STR, "__str__"),
		_(METHOD_REPR, "__repr__"),
		_(METHOD_GET, "__get__"),
		_(METHOD_SET, "__set__"),
		_(METHOD_CLASS, "__class__"),
		_(METHOD_NAME, "__name__"),
		_(METHOD_FILE, "__file__"),
		_(METHOD_INT, "__int__"),
		_(METHOD_CHR, "__chr__"),
		_(METHOD_ORD, "__ord__"),
		_(METHOD_FLOAT, "__float__"),
		_(METHOD_LEN, "__len__"),
		_(METHOD_DOC, "__doc__"),
		_(METHOD_BASE, "__base__"),
		_(METHOD_CALL, "__call__"),
		_(METHOD_GETSLICE, "__getslice__"),
		_(METHOD_LIST_INT, "__list"),
		_(METHOD_DICT_INT, "__dict"),
		_(METHOD_INREPR, "__inrepr"),
		_(METHOD_EQ, "__eq__"),
		_(METHOD_ENTER, "__enter__"),
		_(METHOD_EXIT, "__exit__"),
		_(METHOD_DELITEM, "__delitem__"),
		_(METHOD_ITER, "__iter__"),
		_(METHOD_GETATTR, "__getattr__"),
		_(METHOD_DIR, "__dir__"),
	#undef _
	};
	for (size_t i = 0; i < METHOD__MAX; ++i) {
		vm.specialMethodNames[i] = OBJECT_VAL(krk_copyString(_methods[i].s, _methods[i].len));
	}

	/**
	 * class object()
	 *
	 * The base class for all types.
	 * Defines the last-resort implementation of __str__, __repr__, and __dir__.
	 */
	vm.objectClass = krk_newClass(S("object"), NULL);

	krk_defineNative(&vm.objectClass->methods, ":__class__", _type);
	krk_defineNative(&vm.objectClass->methods, ".__dir__", krk_dirObject);
	krk_defineNative(&vm.objectClass->methods, ".__str__", _strBase);
	krk_defineNative(&vm.objectClass->methods, ".__repr__", _strBase); /* Override if necesary */
	krk_finalizeClass(vm.objectClass);
	vm.objectClass->docstring = S("Base class for all types.");

	/**
	 * class module(object)
	 *
	 * When files are imported as modules, their global namespace is the fields
	 * table of an instance of this class. All modules also end up with their
	 * names and file paths as __name__ and __file__.
	 */
	vm.moduleClass = krk_newClass(S("module"), vm.objectClass);
	krk_defineNative(&vm.moduleClass->methods, ".__repr__", _module_repr);
	krk_defineNative(&vm.moduleClass->methods, ".__str__", _module_repr);
	krk_finalizeClass(vm.moduleClass);
	vm.moduleClass->docstring = S("");

	/**
	 * __builtins__ = module()
	 *
	 * The builtins namespace is always available underneath the current
	 * globals namespace, and is also added to all modules as __builtins__
	 * for direct references (eg., in case one of the names is shadowed
	 * by a global).
	 */
	vm.builtins = krk_newInstance(vm.moduleClass);
	krk_attachNamedObject(&vm.modules, "__builtins__", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&vm.builtins->fields, "object", (KrkObj*)vm.objectClass);
	krk_attachNamedObject(&vm.builtins->fields, "__name__", (KrkObj*)S("__builtins__"));
	krk_attachNamedValue(&vm.builtins->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&vm.builtins->fields, "__doc__",
		(KrkObj*)S("Internal module containing built-in functions and classes."));

	/**
	 * kuroko = module()
	 *
	 * This is equivalent to Python's "sys" module, but we do not use that name
	 * in consideration of future compatibility, where a "sys" module may be
	 * added to emulate Python version numbers, etc.
	 */
	vm.system = krk_newInstance(vm.moduleClass);
	krk_attachNamedObject(&vm.modules, "kuroko", (KrkObj*)vm.system);
	krk_attachNamedObject(&vm.system->fields, "__name__", (KrkObj*)S("kuroko"));
	krk_attachNamedValue(&vm.system->fields, "__file__", NONE_VAL()); /* (built-in) */
	krk_attachNamedObject(&vm.system->fields, "version",
		(KrkObj*)S(KRK_VERSION_MAJOR "." KRK_VERSION_MINOR "." KRK_VERSION_PATCH KRK_VERSION_EXTRA));
	krk_attachNamedObject(&vm.system->fields, "buildenv", (KrkObj*)S(KRK_BUILD_COMPILER));
	krk_attachNamedObject(&vm.system->fields, "builddate", (KrkObj*)S(KRK_BUILD_DATE));
	krk_defineNative(&vm.system->fields, "getsizeof", krk_getsize);
	krk_defineNative(&vm.system->fields, "set_clean_output", krk_setclean);
	krk_attachNamedObject(&vm.system->fields, "path_sep", (KrkObj*)S(PATH_SEP));
	if (vm.binpath) {
		krk_attachNamedObject(&vm.system->fields, "executable_path", (KrkObj*)krk_takeString(vm.binpath, strlen(vm.binpath)));
	}

	/**
	 * gc = module()
	 *
	 * Namespace for methods for controlling the garbage collector.
	 */
	KrkInstance * gcModule = krk_newInstance(vm.moduleClass);
	krk_attachNamedObject(&vm.modules, "gc", (KrkObj*)gcModule);
	krk_attachNamedObject(&gcModule->fields, "__name__", (KrkObj*)S("gc"));
	krk_attachNamedValue(&gcModule->fields, "__file__", NONE_VAL());
	krk_defineNative(&gcModule->fields, "collect", krk_collectGarbage_wrapper);
	krk_attachNamedObject(&gcModule->fields, "__doc__",
		(KrkObj*)S("Namespace containing methods for controlling the garbge collector."));

	/* Add exception classes */
	ADD_EXCEPTION_CLASS(vm.exceptions.baseException, "Exception", vm.objectClass);
	/* base exception class gets an init that takes an optional string */
	krk_defineNative(&vm.exceptions.baseException->methods, ".__init__", krk_initException);
	krk_defineNative(&vm.exceptions.baseException->methods, ".__repr__", _exception_repr);
	krk_finalizeClass(vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.typeError, "TypeError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.argumentError, "ArgumentError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.indexError, "IndexError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.keyError, "KeyError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.attributeError, "AttributeError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.nameError, "NameError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.importError, "ImportError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.ioError, "IOError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.valueError, "ValueError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.keyboardInterrupt, "KeyboardInterrupt", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.zeroDivisionError, "ZeroDivisionError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.notImplementedError, "NotImplementedError", vm.exceptions.baseException);
	ADD_EXCEPTION_CLASS(vm.exceptions.syntaxError, "SyntaxError", vm.exceptions.baseException);
	krk_defineNative(&vm.exceptions.syntaxError->methods, ".__repr__", _syntaxerror_repr);
	krk_finalizeClass(vm.exceptions.syntaxError);

	/* Build classes for basic types */
	ADD_BASE_CLASS(vm.baseClasses.typeClass, "type", vm.objectClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__base__", krk_baseOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__file__", krk_fileOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__doc__", krk_docOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ":__name__", krk_nameOfClass);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__init__", _type_init);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__str__", _class_to_str);
	krk_defineNative(&vm.baseClasses.typeClass->methods, ".__repr__", _class_to_str);
	krk_finalizeClass(vm.baseClasses.typeClass);
	vm.baseClasses.typeClass->docstring = S("Obtain the object representation of the class of an object.");
	ADD_BASE_CLASS(vm.baseClasses.intClass, "int", vm.objectClass);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__init__", _int_init);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__int__", _noop);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__float__", _int_to_floating);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__chr__", _int_to_char);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__str__", _int_to_str);
	krk_defineNative(&vm.baseClasses.intClass->methods, ".__repr__", _int_to_str);
	krk_finalizeClass(vm.baseClasses.intClass);
	vm.baseClasses.intClass->docstring = S("Convert a number or string type to an integer representation.");
	ADD_BASE_CLASS(vm.baseClasses.floatClass, "float", vm.objectClass);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__init__", _float_init);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__int__", _floating_to_int);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__float__", _noop);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__str__", _float_to_str);
	krk_defineNative(&vm.baseClasses.floatClass->methods, ".__repr__", _float_to_str);
	krk_finalizeClass(vm.baseClasses.floatClass);
	vm.baseClasses.floatClass->docstring = S("Convert a number or string type to a float representation.");
	ADD_BASE_CLASS(vm.baseClasses.boolClass, "bool", vm.objectClass);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__init__", _bool_init);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__str__", _bool_to_str);
	krk_defineNative(&vm.baseClasses.boolClass->methods, ".__repr__", _bool_to_str);
	krk_finalizeClass(vm.baseClasses.boolClass);
	vm.baseClasses.floatClass->docstring = S("Returns False if the argument is 'falsey', otherwise True.");
	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.noneTypeClass, "NoneType", vm.objectClass);
	krk_defineNative(&vm.baseClasses.noneTypeClass->methods, ".__str__", _none_to_str);
	krk_defineNative(&vm.baseClasses.noneTypeClass->methods, ".__repr__", _none_to_str);
	krk_finalizeClass(vm.baseClasses.noneTypeClass);
	ADD_BASE_CLASS(vm.baseClasses.strClass, "str", vm.objectClass);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__init__", _string_init);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__str__", _noop);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__repr__", _string_repr);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__len__", _string_len);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__get__", _string_get);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__set__", _strings_are_immutable);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__int__", _string_int);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__float__", _string_float);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__getslice__", _string_getslice);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__ord__", _string_ord);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__contains__", _string_contains);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__iter__", _string_iter);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".format", _string_format);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".join", _string_join);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".split", _string_split);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".strip", _string_strip);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".lstrip", _string_lstrip);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".rstrip", _string_rstrip);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__lt__", _string_lt);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__gt__", _string_gt);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__mod__", _string_mod);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__add__", _string_add);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".__mul__", _string_mul);
	krk_defineNative(&vm.baseClasses.strClass->methods, ".encode", _string_encode);
	krk_finalizeClass(vm.baseClasses.strClass);
	vm.baseClasses.strClass->docstring = S("Obtain a string representation of an object.");
	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.functionClass, "function", vm.objectClass);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__str__", _closure_str);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ".__repr__", _closure_str);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__doc__", _closure_get_doc);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__name__", _closure_get_name);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__file__", _closure_get_file);
	krk_defineNative(&vm.baseClasses.functionClass->methods, ":__args__", _closure_get_argnames);
	krk_finalizeClass(vm.baseClasses.functionClass);
	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.methodClass, "method", vm.objectClass);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__str__", _bound_str);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__repr__", _bound_str);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ".__doc__", _bound_get_doc);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__name__", _bound_get_name);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__file__", _bound_get_file);
	krk_defineNative(&vm.baseClasses.methodClass->methods, ":__args__", _bound_get_argnames);
	krk_finalizeClass(vm.baseClasses.methodClass);
	ADD_BASE_CLASS(vm.baseClasses.tupleClass, "tuple", vm.objectClass);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__init__", _tuple_init);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__str__", _tuple_repr);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__repr__", _tuple_repr);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__get__", _tuple_get);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__len__", _tuple_len);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__contains__", _tuple_contains);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__iter__", _tuple_iter);
	krk_defineNative(&vm.baseClasses.tupleClass->methods, ".__eq__", _tuple_eq);
	krk_finalizeClass(vm.baseClasses.tupleClass);
	ADD_BASE_CLASS(vm.baseClasses.bytesClass, "bytes", vm.objectClass);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__init__",  _bytes_init);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__str__",  _bytes_repr);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__repr__", _bytes_repr);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".decode", _bytes_decode);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__len__", _bytes_len);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__contains__", _bytes_contains);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__get__", _bytes_get);
	krk_defineNative(&vm.baseClasses.bytesClass->methods, ".__eq__", _bytes_eq);
	krk_finalizeClass(vm.baseClasses.bytesClass);
	ADD_BASE_CLASS(vm.baseClasses.listClass, "list", vm.objectClass);
	vm.baseClasses.listClass->allocSize = sizeof(KrkList);
	vm.baseClasses.listClass->_ongcscan = _list_gcscan;
	vm.baseClasses.listClass->_ongcsweep = _list_gcsweep;
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__init__", _list_init);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__get__", _list_get);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__set__", _list_set);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__delitem__", _list_pop);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__len__", _list_len);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__str__", _list_repr);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__repr__", _list_repr);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__contains__", _list_contains);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__getslice__", _list_slice);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__iter__", _list_iter);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__mul__", _list_mul);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".append", _list_append);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".extend", _list_extend);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".pop", _list_pop);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".insert", _list_insert);
	krk_finalizeClass(vm.baseClasses.listClass);
	vm.baseClasses.listClass->docstring = S("Mutable sequence of arbitrary values.");
	ADD_BASE_CLASS(vm.baseClasses.dictClass, "dict", vm.objectClass);
	vm.baseClasses.dictClass->allocSize = sizeof(KrkDict);
	vm.baseClasses.dictClass->_ongcscan = _dict_gcscan;
	vm.baseClasses.dictClass->_ongcsweep = _dict_gcsweep;
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__init__", _dict_init);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__str__", _dict_repr);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__repr__", _dict_repr);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__get__", _dict_get);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__set__", _dict_set);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__or__", _dict_or);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__delitem__", _dict_delitem);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__len__", _dict_len);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__contains__", _dict_contains);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".keys", _dict_keys);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".items", _dict_items);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".capacity", _dict_capacity);
	krk_finalizeClass(vm.baseClasses.dictClass);
	vm.baseClasses.dictClass->docstring = S("Mapping of arbitrary keys to values.");

	/* Build global builtin functions. */
	BUILTIN_FUNCTION("listOf", krk_list_of, "Convert argument sequence to list object.");
	BUILTIN_FUNCTION("dictOf", krk_dict_of, "Convert argument sequence to dict object.");
	BUILTIN_FUNCTION("tupleOf", _tuple_of,  "Convert argument sequence to tuple object.");
	BUILTIN_FUNCTION("isinstance", _isinstance, "Determine if an object is an instance of the given class or one if its subclasses.");
	BUILTIN_FUNCTION("globals", krk_globals, "Return a mapping of names in the current global namespace.");
	BUILTIN_FUNCTION("dir", _dir, "Return a list of known property names for a given object.");
	BUILTIN_FUNCTION("len", _len, "Return the length of a given sequence object.");
	BUILTIN_FUNCTION("repr", _repr, "Produce a string representation of the given object.");
	BUILTIN_FUNCTION("print", _print, "Print values to the standard output descriptor.");
	BUILTIN_FUNCTION("ord", _ord, "Obtain the ordinal integer value of a codepoint or byte.");
	BUILTIN_FUNCTION("chr", _chr, "Convert an integer codepoint to its string representation.");
	BUILTIN_FUNCTION("hex", _hex, "Convert an integer value to a hexadecimal string.");
	BUILTIN_FUNCTION("any", _any, "Returns True if at least one element in the given iterable is truthy, False otherwise.");
	BUILTIN_FUNCTION("all", _all, "Returns True if every element in the given iterable is truthy, False otherwise.");

	/* __builtins__.set_tracing is namespaced */
	krk_defineNative(&vm.builtins->fields, "set_tracing", krk_set_tracing)->doc = "Toggle debugging modes.";

	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.listiteratorClass, "listiterator", vm.objectClass);
	krk_defineNative(&vm.baseClasses.listiteratorClass->methods, ".__init__", _listiter_init);
	krk_defineNative(&vm.baseClasses.listiteratorClass->methods, ".__call__", _listiter_call);
	krk_finalizeClass(vm.baseClasses.listiteratorClass);

	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.striteratorClass, "striterator", vm.objectClass);
	krk_defineNative(&vm.baseClasses.striteratorClass->methods, ".__init__", _striter_init);
	krk_defineNative(&vm.baseClasses.striteratorClass->methods, ".__call__", _striter_call);
	krk_finalizeClass(vm.baseClasses.striteratorClass);

	ADD_BASE_CLASS(vm.baseClasses.rangeClass, "range", vm.objectClass);
	vm.baseClasses.rangeClass->allocSize = sizeof(struct Range);
	krk_defineNative(&vm.baseClasses.rangeClass->methods, ".__init__", _range_init);
	krk_defineNative(&vm.baseClasses.rangeClass->methods, ".__iter__", _range_iter);
	krk_defineNative(&vm.baseClasses.rangeClass->methods, ".__repr__", _range_repr);
	krk_finalizeClass(vm.baseClasses.rangeClass);
	vm.baseClasses.rangeClass->docstring = S("range(max), range(min, max[, step]): "
		"An iterable object that produces numeric values. "
		"'min' is inclusive, 'max' is exclusive.");

	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.rangeiteratorClass, "rangeiterator", vm.objectClass);
	vm.baseClasses.rangeiteratorClass->allocSize = sizeof(struct RangeIterator);
	krk_defineNative(&vm.baseClasses.rangeiteratorClass->methods, ".__init__", _rangeiterator_init);
	krk_defineNative(&vm.baseClasses.rangeiteratorClass->methods, ".__call__", _rangeiterator_call);
	krk_finalizeClass(vm.baseClasses.rangeiteratorClass);

	/* TODO: Don't attach */
	ADD_BASE_CLASS(vm.baseClasses.tupleiteratorClass, "tupleiterator", vm.objectClass);
	vm.baseClasses.tupleiteratorClass->allocSize = sizeof(struct TupleIter);
	vm.baseClasses.tupleiteratorClass->_ongcscan = _tuple_iter_gcscan;
	krk_defineNative(&vm.baseClasses.tupleiteratorClass->methods, ".__init__", _tuple_iter_init);
	krk_defineNative(&vm.baseClasses.tupleiteratorClass->methods, ".__call__", _tuple_iter_call);
	krk_finalizeClass(vm.baseClasses.tupleiteratorClass);

	ADD_BASE_CLASS(vm.baseClasses.dictitemsClass, "dictitems", vm.objectClass);
	vm.baseClasses.dictitemsClass->allocSize = sizeof(struct DictItems);
	vm.baseClasses.dictitemsClass->_ongcscan = _dictitems_gcscan;
	krk_defineNative(&vm.baseClasses.dictitemsClass->methods, ".__init__", _dictitems_init);
	krk_defineNative(&vm.baseClasses.dictitemsClass->methods, ".__iter__", _dictitems_iter);
	krk_defineNative(&vm.baseClasses.dictitemsClass->methods, ".__call__", _dictitems_call);
	krk_finalizeClass(vm.baseClasses.dictitemsClass);

	ADD_BASE_CLASS(vm.baseClasses.dictkeysClass, "dictkeys", vm.objectClass);
	vm.baseClasses.dictkeysClass->allocSize = sizeof(struct DictKeys);
	vm.baseClasses.dictkeysClass->_ongcscan = _dictkeys_gcscan;
	krk_defineNative(&vm.baseClasses.dictkeysClass->methods, ".__init__", _dictkeys_init);
	krk_defineNative(&vm.baseClasses.dictkeysClass->methods, ".__iter__", _dictkeys_iter);
	krk_defineNative(&vm.baseClasses.dictkeysClass->methods, ".__call__", _dictkeys_call);
	krk_finalizeClass(vm.baseClasses.dictkeysClass);

	/* This module is slowly being deprecated. */
	KrkValue builtinsModule = krk_interpret(krk_builtinsSrc,1,"__builtins__","__builtins__");
	if (!IS_OBJECT(builtinsModule)) {
		/* ... hence, this is a warning and not a complete failure. */
		fprintf(stderr, "VM startup failure: Failed to load __builtins__ module.\n");
	}

	/* The VM is now ready to start executing code. */
	krk_resetStack();
	KRK_RESUME_GC();
}

/**
 * Reclaim resources used by the VM.
 */
void krk_freeVM() {
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
	return krk_getType(value)->name->chars;
}

static KrkValue tryBind(const char * name, KrkValue a, KrkValue b, const char * msg) {
	krk_push(b);
	krk_push(a);
	KrkClass * type = krk_getType(a);
	KrkString * methodName = krk_copyString(name, strlen(name));
	krk_push(OBJECT_VAL(methodName));
	KrkValue value = KWARGS_VAL(0);
	krk_swap(1);
	if (krk_bindMethod(type, methodName)) {
		krk_swap(1);
		krk_pop();
		krk_swap(1);
		value = krk_callSimple(krk_peek(1), 1, 1);
	}
	if (IS_KWARGS(value)) {
		return krk_runtimeError(vm.exceptions.typeError, msg, krk_typeName(a), krk_typeName(b));
	} else {
		return value;
	}
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
	static KrkValue operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return FLOATING_VAL(AS_FLOATING(a) operator (double)AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return FLOATING_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return FLOATING_VAL((double)AS_INTEGER(a) operator AS_FLOATING(b)); \
		} \
		return tryBind("__" #name "__", a, b, "unsupported operand types for " #operator ": '%s' and '%s'"); \
	}

MAKE_BIN_OP(add,+)
MAKE_BIN_OP(sub,-)
MAKE_BIN_OP(mul,*)
MAKE_BIN_OP(div,/)

#define MAKE_UNOPTIMIZED_BIN_OP(name,operator) \
	static KrkValue operator_ ## name (KrkValue a, KrkValue b) { \
		return tryBind("__" #name "__", a, b, "unsupported operand types for " #operator ": '%s' and '%s'"); \
	}

MAKE_UNOPTIMIZED_BIN_OP(pow,**)

/* Bit ops are invalid on doubles in C, so we can't use the same set of macros for them;
 * they should be invalid in Kuroko as well. */
#define MAKE_BIT_OP(name,operator) \
	static KrkValue operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		return tryBind("__" #name "__", a, b, "unsupported operand types for " #operator ": '%s' and '%s'"); \
	}

MAKE_BIT_OP(or,|)
MAKE_BIT_OP(xor,^)
MAKE_BIT_OP(and,&)
MAKE_BIT_OP(lshift,<<)
MAKE_BIT_OP(rshift,>>)
MAKE_BIT_OP(mod,%) /* not a bit op, but doesn't work on floating point */

#define MAKE_COMPARATOR(name, operator) \
	static KrkValue operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		} \
		return tryBind("__" #name "__", a, b, "'" #operator "' not supported between instances of '%s' and '%s'"); \
	}

MAKE_COMPARATOR(lt, <)
MAKE_COMPARATOR(gt, >)

static void addObjects() {
	KrkValue tmp = _string_add(2, (KrkValue[]){krk_peek(1), krk_peek(0)});
	krk_pop(); krk_pop();
	krk_push(tmp);
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
	for (stackOffset = (int)(vm.stackTop - vm.stack - 1); stackOffset >= exitSlot && !IS_TRY_HANDLER(vm.stack[stackOffset]); stackOffset--);
	if (stackOffset < exitSlot) {
		if (exitSlot == 0) {
			/*
			 * No exception was found and we have reached the top of the call stack.
			 * Call dumpTraceback to present the exception to the user and reset the
			 * VM stack state. It should still be safe to execute more code after
			 * this reset, so the repl can throw errors and keep accepting new lines.
			 */
			krk_dumpTraceback();
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
int krk_loadModule(KrkString * path, KrkValue * moduleOut, KrkString * runAs) {
	KrkValue modulePaths;

	/* See if the module is already loaded */
	if (krk_tableGet(&vm.modules, OBJECT_VAL(runAs), moduleOut)) {
		krk_push(*moduleOut);
		return 1;
	}

	/* Obtain __builtins__.module_paths */
	if (!krk_tableGet(&vm.system->fields, OBJECT_VAL(S("module_paths")), &modulePaths) || !IS_INSTANCE(modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.baseException,
			"Internal error: kuroko.module_paths not defined.");
		return 0;
	}

	/* Obtain __builtins__.module_paths.__list so we can do lookups directly */
	int moduleCount = AS_LIST(modulePaths)->count;
	if (!moduleCount) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions.importError,
			"No module search directories are specified, so no modules may be imported.");
		return 0;
	}

	struct stat statbuf;

	/* First search for {path}.krk in the module search paths */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		krk_push(AS_LIST(modulePaths)->values[i]);
		if (!IS_STRING(krk_peek(0))) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.typeError,
				"Module search paths must be strings; check the search path at index %d", i);
			return 0;
		}
		krk_push(OBJECT_VAL(path));
		addObjects(); /* Concatenate path... */
		krk_push(OBJECT_VAL(S(".krk")));
		addObjects(); /* and file extension */
		int isPackage = 0;

		char * fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) < 0) {
			krk_pop();
			/* try /__init__.krk */
			krk_push(AS_LIST(modulePaths)->values[i]);
			krk_push(OBJECT_VAL(path));
			addObjects();
			krk_push(OBJECT_VAL(S(PATH_SEP "__init__.krk")));
			addObjects();
			fileName = AS_CSTRING(krk_peek(0));
			if (stat(fileName,&statbuf) < 0) {
				continue;
			}
			isPackage = 1;
		}

		/* Compile and run the module in a new context and exit the VM when it
		 * returns to the current call frame; modules should return objects. */
		int previousExitFrame = vm.exitOnFrame;
		vm.exitOnFrame = vm.frameCount;
		*moduleOut = krk_runfile(fileName,1,runAs->chars,fileName);
		vm.exitOnFrame = previousExitFrame;
		if (!IS_OBJECT(*moduleOut)) {
			if (!(vm.flags & KRK_HAS_EXCEPTION)) {
				krk_runtimeError(vm.exceptions.importError,
					"Failed to load module '%s' from '%s'", runAs->chars, fileName);
			}
			return 0;
		}

		krk_pop(); /* concatenated filename on stack */
		krk_push(*moduleOut);
		krk_tableSet(&vm.modules, OBJECT_VAL(runAs), *moduleOut);
		/* Was this a package? */
		if (isPackage) {
			krk_attachNamedValue(&AS_INSTANCE(*moduleOut)->fields,"__ispackage__",BOOLEAN_VAL(1));
		}
		return 1;
	}

#ifndef STATIC_ONLY
	/* If we didn't find {path}.krk, try {path}.so in the same order */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		/* Assume things haven't changed and all of these are strings. */
		krk_push(AS_LIST(modulePaths)->values[i]);
		krk_push(OBJECT_VAL(path));
		addObjects(); /* this should just be basic concatenation */
		krk_push(OBJECT_VAL(S(".so")));
		addObjects();

		char * fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) < 0) continue;

		dlRefType dlRef = dlOpen(fileName);
		if (!dlRef) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.importError,
				"Failed to load native module '%s' from shared object '%s'", runAs->chars, fileName);
			return 0;
		}

		const char * start = path->chars;
		for (const char * c = start; *c; c++) {
			if (*c == '.') start = c + 1;
		}

		krk_push(OBJECT_VAL(S("krk_module_onload_")));
		krk_push(OBJECT_VAL(krk_copyString(start,strlen(start))));
		addObjects();

		char * handlerName = AS_CSTRING(krk_peek(0));

		KrkValue (*moduleOnLoad)(KrkString * name);
		dlSymType out = dlSym(dlRef, handlerName);
		memcpy(&moduleOnLoad,&out,sizeof(out));

		if (!moduleOnLoad) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions.importError,
				"Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad(runAs);
		if (!IS_INSTANCE(*moduleOut)) {
			krk_runtimeError(vm.exceptions.importError,
				"Failed to load module '%s' from '%s'", runAs->chars, fileName);
			return 0;
		}

		krk_push(*moduleOut);
		krk_swap(1);

		krk_attachNamedObject(&AS_INSTANCE(*moduleOut)->fields, "__name__", (KrkObj*)runAs);
		krk_attachNamedValue(&AS_INSTANCE(*moduleOut)->fields, "__file__", krk_peek(0));

		krk_pop(); /* filename */
		krk_tableSet(&vm.modules, OBJECT_VAL(runAs), *moduleOut);
		return 1;
	}
#endif

	/* If we still haven't found anything, fail. */
	*moduleOut = NONE_VAL();
	krk_runtimeError(vm.exceptions.importError, "No module named '%s'", runAs->chars);
	return 0;
}

int krk_doRecursiveModuleLoad(KrkString * name) {
	/* See if 'name' is clear to directly import */
	int isClear = 1;
	for (size_t i = 0; i < name->length; ++i) {
		if (name->chars[i] == '.') {
			isClear = 0;
			break;
		}
	}

	if (isClear) {
		KrkValue base;
		return krk_loadModule(name,&base,name);
	}

	/**
	 * To import foo.bar.baz
	 * - import foo as foo
	 * - import foo/bar as foo.bar
	 * - import foo/bar/baz as foo.bar.baz
	 */

	/* Let's split up name */
	krk_push(NONE_VAL());         // -1: last
	int argBase = vm.stackTop - vm.stack;
	krk_push(NONE_VAL());         // 0: Name of current node being processed.
	krk_push(OBJECT_VAL(S("")));  // 1: slash/separated/path
	krk_push(OBJECT_VAL(S("")));  // 2: dot.separated.path
	krk_push(OBJECT_VAL(name));   // 3: remaining path to process
	krk_push(OBJECT_VAL(S("."))); // 4: string "." to search for
	do {
		KrkValue listOut = _string_split(3,(KrkValue[]){vm.stack[argBase+3], vm.stack[argBase+4], INTEGER_VAL(1)}, 0);
		if (!IS_INSTANCE(listOut)) return 0;

		/* Set node */
		vm.stack[argBase+0] = AS_LIST(listOut)->values[0];

		/* Set remainder */
		if (AS_LIST(listOut)->count > 1) {
			vm.stack[argBase+3] = AS_LIST(listOut)->values[1];
		} else {
			vm.stack[argBase+3] = NONE_VAL();
		}

		/* First is /-path */
		krk_push(vm.stack[argBase+1]);
		krk_push(vm.stack[argBase+0]);
		addObjects();
		vm.stack[argBase+1] = krk_pop();
		/* Second is .-path */
		krk_push(vm.stack[argBase+2]);
		krk_push(vm.stack[argBase+0]);
		addObjects();
		vm.stack[argBase+2] = krk_pop();

		if (IS_NONE(vm.stack[argBase+3])) {
			krk_pop(); /* dot */
			krk_pop(); /* remainder */
			KrkValue current;
			if (!krk_loadModule(AS_STRING(vm.stack[argBase+1]), &current, AS_STRING(vm.stack[argBase+2]))) return 0;
			krk_pop(); /* dot-sepaerated */
			krk_pop(); /* slash-separated */
			krk_push(current);
			/* last must be something if we got here, because single-level import happens elsewhere */
			krk_tableSet(&AS_INSTANCE(vm.stack[argBase-1])->fields, vm.stack[argBase+0], krk_peek(0));
			vm.stackTop = vm.stack + argBase;
			vm.stackTop[-1] = current;
			return 1;
		} else {
			KrkValue current;
			if (!krk_loadModule(AS_STRING(vm.stack[argBase+1]), &current, AS_STRING(vm.stack[argBase+2]))) return 0;
			krk_push(current);
			if (!IS_NONE(vm.stack[argBase-1])) {
				krk_tableSet(&AS_INSTANCE(vm.stack[argBase-1])->fields, vm.stack[argBase+0], krk_peek(0));
			}
			/* Is this a package? */
			KrkValue tmp;
			if (!krk_tableGet(&AS_INSTANCE(current)->fields, OBJECT_VAL(S("__ispackage__")), &tmp) || !IS_BOOLEAN(tmp) || AS_BOOLEAN(tmp) != 1) {
				krk_runtimeError(vm.exceptions.importError, "'%s' is not a package", AS_CSTRING(vm.stack[argBase+2]));
				return 0;
			}
			vm.stack[argBase-1] = krk_pop();
			/* Now concatenate forward slash... */
			krk_push(vm.stack[argBase+1]); /* Slash path */
			krk_push(OBJECT_VAL(S(PATH_SEP)));
			addObjects();
			vm.stack[argBase+1] = krk_pop();
			/* And now for the dot... */
			krk_push(vm.stack[argBase+2]);
			krk_push(vm.stack[argBase+4]);
			addObjects();
			vm.stack[argBase+2] = krk_pop();
		}
	} while (1);
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
	KrkValue value;
	if (IS_INSTANCE(krk_peek(0))) {
		KrkInstance * instance = AS_INSTANCE(krk_peek(0));
		if (krk_tableGet(&instance->fields, OBJECT_VAL(name), &value)) {
			if (IS_PROPERTY(value)) {
				krk_push(krk_callSimple(AS_PROPERTY(value)->method, 1, 0));
				return 1;
			}
			krk_pop();
			krk_push(value);
			return 1;
		}
		objectClass = instance->_class;
	} else if (IS_CLASS(krk_peek(0))) {
		KrkClass * _class = AS_CLASS(krk_peek(0));
		if (krk_tableGet(&_class->fields, OBJECT_VAL(name), &value) ||
			krk_tableGet(&_class->methods, OBJECT_VAL(name), &value)) {
			if (IS_PROPERTY(value)) {
				krk_push(krk_callSimple(AS_PROPERTY(value)->method, 1, 0));
				return 1;
			}
			krk_pop();
			krk_push(value);
			return 1;
		}
		objectClass = krk_getType(krk_peek(0));
	} else {
		objectClass = krk_getType(krk_peek(0));
	}

	/* See if the base class for this non-instance type has a method available */
	if (krk_bindMethod(objectClass, name)) {
		return 1;
	}

	if (objectClass->_getattr) {
		krk_push(OBJECT_VAL(name));
		krk_push(krk_callSimple(OBJECT_VAL(objectClass->_getattr), 2, 0));
		return 1;
	}

	return 0;
}

static int valueDelProperty(KrkString * name) {
	if (IS_INSTANCE(krk_peek(0))) {
		KrkInstance* instance = AS_INSTANCE(krk_peek(0));
		if (!krk_tableDelete(&instance->fields, OBJECT_VAL(name))) {
			return 0;
		}
		krk_pop(); /* the original value */
		return 1;
	} else if (IS_CLASS(krk_peek(0))) {
		KrkClass * _class = AS_CLASS(krk_peek(0));
		if (!krk_tableDelete(&_class->fields, OBJECT_VAL(name))) {
			return 0;
		}
		krk_pop(); /* the original value */
		return 1;
	}
	/* TODO del on values? */
	return 0;
}

#define READ_BYTE() (*frame->ip++)
#define BINARY_OP(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); krk_push(operator_ ## op (a,b)); break; }
#define BINARY_OP_CHECK_ZERO(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); \
	if ((IS_INTEGER(b) && AS_INTEGER(b) == 0)) { krk_runtimeError(vm.exceptions.zeroDivisionError, "integer division or modulo by zero"); goto _finishException; } \
	else if ((IS_FLOATING(b) && AS_FLOATING(b) == 0.0)) { krk_runtimeError(vm.exceptions.zeroDivisionError, "float division by zero"); goto _finishException; } \
	krk_push(operator_ ## op (a,b)); break; }
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
			krk_disassembleInstruction(stderr, frame->closure->function,
				(size_t)(frame->ip - frame->closure->function->chunk.code));
		}
#endif

#ifdef ENABLE_WATCHDOG
		if (vm.watchdog - 1 == 0) {
			fprintf(stderr, "Watchdog timer tripped.\n\n");
			krk_dumpTraceback();
			krk_resetStack();
			fprintf(stderr, "\n\n");
			vm.watchdog = 0;
			exit(0);
			return NONE_VAL();
		} else if (vm.watchdog > 0) {
			vm.watchdog--;
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
			case OP_CLEANUP_WITH: {
				/* Top of stack is a HANDLER that should have had something loaded into it if it was still valid */
				KrkValue handler = krk_peek(0);
				KrkValue contextManager = krk_peek(1);
				KrkClass * type = krk_getType(contextManager);
				krk_push(contextManager);
				krk_callSimple(OBJECT_VAL(type->_exit), 1, 0);
				/* Top of stack is now either someone else's problem or a return value */
				if (AS_HANDLER(handler).type != OP_RETURN) break;
				krk_pop(); /* handler */
				krk_pop(); /* context manager */
			} /* fallthrough */
			case OP_RETURN: {
				KrkValue result = krk_pop();
				closeUpvalues(frame->slots);
				/* See if this frame had a thing */
				int stackOffset;
				for (stackOffset = (int)(vm.stackTop - vm.stack - 1); stackOffset >= (int)frame->slots && !IS_WITH_HANDLER(vm.stack[stackOffset]); stackOffset--);
				if (stackOffset >= (int)frame->slots) {
					vm.stackTop = &vm.stack[stackOffset + 1];
					krk_push(result);
					krk_swap(2);
					krk_swap(1);
					frame->ip = frame->closure->function->chunk.code + AS_HANDLER(krk_peek(0)).target;
					AS_HANDLER(vm.stackTop[-1]).type = OP_RETURN;
					break;
				}
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
			case OP_IS: {
				KrkValue b = krk_pop();
				KrkValue a = krk_pop();
				krk_push(BOOLEAN_VAL(krk_valuesSame(a,b)));
				break;
			}
			case OP_LESS: BINARY_OP(lt);
			case OP_GREATER: BINARY_OP(gt)
			case OP_ADD:
				if (IS_STRING(krk_peek(1))) addObjects(); /* Shortcut for strings */
				else BINARY_OP(add)
				break;
			case OP_SUBTRACT: BINARY_OP(sub)
			case OP_MULTIPLY: BINARY_OP(mul)
			case OP_DIVIDE: BINARY_OP_CHECK_ZERO(div)
			case OP_MODULO: BINARY_OP_CHECK_ZERO(mod)
			case OP_BITOR: BINARY_OP(or)
			case OP_BITXOR: BINARY_OP(xor)
			case OP_BITAND: BINARY_OP(and)
			case OP_SHIFTLEFT: BINARY_OP(lshift)
			case OP_SHIFTRIGHT: BINARY_OP(rshift)
			case OP_POW: BINARY_OP(pow)
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
				krk_tableSet(frame->globals, OBJECT_VAL(name), krk_peek(0));
				krk_pop();
				break;
			}
			case OP_GET_GLOBAL_LONG:
			case OP_GET_GLOBAL: {
				KrkString * name = READ_STRING(operandWidth);
				KrkValue value;
				if (!krk_tableGet(frame->globals, OBJECT_VAL(name), &value)) {
					if (!krk_tableGet(&vm.builtins->fields, OBJECT_VAL(name), &value)) {
						krk_runtimeError(vm.exceptions.nameError, "Undefined variable '%s'.", name->chars);
						goto _finishException;
					}
				}
				krk_push(value);
				break;
			}
			case OP_SET_GLOBAL_LONG:
			case OP_SET_GLOBAL: {
				KrkString * name = READ_STRING(operandWidth);
				if (krk_tableSet(frame->globals, OBJECT_VAL(name), krk_peek(0))) {
					krk_tableDelete(frame->globals, OBJECT_VAL(name));
					krk_runtimeError(vm.exceptions.nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DEL_GLOBAL_LONG:
			case OP_DEL_GLOBAL: {
				KrkString * name = READ_STRING(operandWidth);
				if (!krk_tableDelete(frame->globals, OBJECT_VAL(name))) {
					krk_runtimeError(vm.exceptions.nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_IMPORT_LONG:
			case OP_IMPORT: {
				KrkString * name = READ_STRING(operandWidth);
				if (!krk_doRecursiveModuleLoad(name)) {
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
				KrkValue handler = HANDLER_VAL(OP_PUSH_TRY, tryTarget);
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
				if (unlikely(!krk_callValue(krk_peek(argCount), argCount, 1))) goto _finishException;
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			/* This version of the call instruction takes its arity from the
			 * top of the stack, so we don't have to calculate arity at compile time. */
			case OP_CALL_STACK: {
				int argCount = AS_INTEGER(krk_pop());
				if (unlikely(!krk_callValue(krk_peek(argCount), argCount, 1))) goto _finishException;
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
				KrkClass * _class = krk_newClass(name, vm.objectClass);
				krk_push(OBJECT_VAL(_class));
				_class->filename = frame->closure->function->chunk.filename;
				break;
			}
			case OP_IMPORT_FROM_LONG:
			case OP_IMPORT_FROM: {
				KrkString * name = READ_STRING(operandWidth);
				if (unlikely(!valueGetProperty(name))) {
					/* Try to import... */
					KrkValue moduleName;
					if (!krk_tableGet(&AS_INSTANCE(krk_peek(0))->fields, vm.specialMethodNames[METHOD_NAME], &moduleName)) {
						krk_runtimeError(vm.exceptions.importError, "Can not import '%s' from non-module '%s' object", name->chars, krk_typeName(krk_peek(0)));
						goto _finishException;
					}
					krk_push(moduleName);
					krk_push(OBJECT_VAL(S(".")));
					addObjects();
					krk_push(OBJECT_VAL(name));
					addObjects();
					if (!krk_doRecursiveModuleLoad(AS_STRING(krk_peek(0)))) {
						krk_runtimeError(vm.exceptions.importError, "Can not import '%s' from '%s'", name->chars, AS_CSTRING(moduleName));
						goto _finishException;
					}
					vm.stackTop[-3] = vm.stackTop[-1];
					vm.stackTop -= 2;
				}
			} break;
			case OP_GET_PROPERTY_LONG:
			case OP_GET_PROPERTY: {
				KrkString * name = READ_STRING(operandWidth);
				if (unlikely(!valueGetProperty(name))) {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DEL_PROPERTY_LONG:
			case OP_DEL_PROPERTY: {
				KrkString * name = READ_STRING(operandWidth);
				if (unlikely(!valueDelProperty(name))) {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_INVOKE_GETTER: {
				KrkClass * type = krk_getType(krk_peek(1));
				if (likely(type->_getter)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_getter), 2, 0));
				} else {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
				}
				break;
			}
			case OP_INVOKE_SETTER: {
				KrkClass * type = krk_getType(krk_peek(2));
				if (likely(type->_setter)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_setter), 3, 0));
				} else {
					if (type->_getter) {
						krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not mutable", krk_typeName(krk_peek(2)));
					} else {
						krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(2)));
					}
				}
				break;
			}
			case OP_INVOKE_GETSLICE: {
				KrkClass * type = krk_getType(krk_peek(2));
				if (likely(type->_slicer)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_slicer), 3, 0));
				} else {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not sliceable", krk_typeName(krk_peek(2)));
				}
				break;
			}
			case OP_INVOKE_DELETE: {
				KrkClass * type = krk_getType(krk_peek(1));
				if (likely(type->_delitem)) {
					krk_callSimple(OBJECT_VAL(type->_delitem), 2, 0);
				} else {
					if (type->_getter) {
						krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not mutable", krk_typeName(krk_peek(1)));
					} else {
						krk_runtimeError(vm.exceptions.attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
					}
				}
				break;
			}
			case OP_SET_PROPERTY_LONG:
			case OP_SET_PROPERTY: {
				KrkString * name = READ_STRING(operandWidth);
				KrkTable * table = NULL;
				if (IS_INSTANCE(krk_peek(1))) table = &AS_INSTANCE(krk_peek(1))->fields;
				else if (IS_CLASS(krk_peek(1))) table = &AS_CLASS(krk_peek(1))->fields;
				if (table) {
					KrkValue previous;
					if (krk_tableGet(table, OBJECT_VAL(name), &previous) && IS_PROPERTY(previous)) {
						krk_push(krk_callSimple(AS_PROPERTY(previous)->method, 2, 0));
						break;
					} else {
						krk_tableSet(table, OBJECT_VAL(name), krk_peek(0));
					}
				} else {
					krk_runtimeError(vm.exceptions.attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				krk_swap(1);
				krk_pop();
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
				if (unlikely(!IS_CLASS(superclass))) {
					krk_runtimeError(vm.exceptions.typeError, "Superclass must be a class, not '%s'",
						krk_typeName(superclass));
					goto _finishException;
				}
				KrkClass * subclass = AS_CLASS(krk_peek(0));
				subclass->base = AS_CLASS(superclass);
				krk_tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
				krk_tableAddAll(&AS_CLASS(superclass)->fields, &subclass->fields);
				subclass->allocSize = AS_CLASS(superclass)->allocSize;
				subclass->_ongcsweep = AS_CLASS(superclass)->_ongcsweep;
				subclass->_ongcscan = AS_CLASS(superclass)->_ongcscan;
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
					krk_runtimeError(vm.exceptions.attributeError, "super(%s) has no attribute '%s'",
						superclass->name->chars, name->chars);
					goto _finishException;
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
			case OP_TUPLE_LONG:
			case OP_TUPLE: {
				size_t count = readBytes(frame, operandWidth);
				krk_reserve_stack(4);
				KrkValue tuple = _tuple_of(count,&vm.stackTop[-count]);
				if (count) {
					vm.stackTop[-count] = tuple;
					while (count > 1) {
						krk_pop();
						count--;
					}
				} else {
					krk_push(tuple);
				}
				break;
			}
			case OP_UNPACK_LONG:
			case OP_UNPACK: {
				size_t count = readBytes(frame, operandWidth);
				KrkValue sequence = krk_peek(0);
				/* First figure out what it is and if we can unpack it. */
#define unpackArray(counter, indexer) do { \
					if (counter != count) { \
						krk_runtimeError(vm.exceptions.valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter); \
					} \
					for (size_t i = 1; i < counter; ++i) { \
						krk_push(indexer); \
					} \
					size_t i = 0; \
					vm.stackTop[-count] = indexer; \
				} while (0)
				if (IS_TUPLE(sequence)) {
					unpackArray(AS_TUPLE(sequence)->values.count, AS_TUPLE(sequence)->values.values[i]);
				} else if (IS_INSTANCE(sequence) && AS_INSTANCE(sequence)->_class == vm.baseClasses.listClass) {
					unpackArray(AS_LIST(sequence)->count, AS_LIST(sequence)->values[i]);
				} else if (IS_INSTANCE(sequence) && AS_INSTANCE(sequence)->_class == vm.baseClasses.dictClass) {
					unpackArray(AS_DICT(sequence)->count, _dict_nth_key_fast(AS_DICT(sequence)->capacity, AS_DICT(sequence)->entries, i));
				} else if (IS_STRING(sequence)) {
					unpackArray(AS_STRING(sequence)->codesLength, _string_get(2,(KrkValue[]){sequence,INTEGER_VAL(i)}));
				} else {
					KrkClass * type = krk_getType(sequence);
					if (!type->_iter) {
						krk_runtimeError(vm.exceptions.typeError, "Can not unpack non-iterable '%s'", krk_typeName(sequence));
						goto _finishException;
					} else {
						size_t stackStart = vm.stackTop - vm.stack - 1;
						size_t counter = 0;
						for (size_t i = 0; i < count-1; i++) {
							krk_push(NONE_VAL());
						}
						/* Create the iterator */
						krk_push(vm.stack[stackStart]);
						krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

						do {
							/* Call it until it gives us itself */
							krk_push(vm.stackTop[-1]);
							krk_push(krk_callSimple(krk_peek(0), 0, 1));
							if (krk_valuesSame(vm.stackTop[-2], vm.stackTop[-1])) {
								/* We're done. */
								krk_pop(); /* The result of iteration */
								krk_pop(); /* The iterator */
								if (counter != count) {
									krk_runtimeError(vm.exceptions.valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter);
									goto _finishException;
								}
								break;
							}
							if (counter == count) {
								krk_runtimeError(vm.exceptions.valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter);
								goto _finishException;
							}
							/* Rotate */
							vm.stack[stackStart+counter] = krk_pop();
							counter++;
						} while (1);
					}
				}
#undef unpackArray
				break;
			}
			case OP_PUSH_WITH: {
				uint16_t cleanupTarget = readBytes(frame, 2) + (frame->ip - frame->closure->function->chunk.code);
				KrkValue contextManager = krk_peek(0);
				KrkClass * type = krk_getType(contextManager);
				if (unlikely(!type->_enter || !type->_exit)) {
					krk_runtimeError(vm.exceptions.attributeError, "Can not use '%s' as context manager", krk_typeName(contextManager));
					goto _finishException;
				}
				krk_push(contextManager);
				krk_callSimple(OBJECT_VAL(type->_enter), 1, 0);
				/* Ignore result; don't need to pop */
				KrkValue handler = HANDLER_VAL(OP_PUSH_WITH, cleanupTarget);
				krk_push(handler);
				break;
			}
			case OP_CREATE_PROPERTY: {
				KrkProperty * newProperty = krk_newProperty(krk_peek(0));
				krk_pop();
				krk_push(OBJECT_VAL(newProperty));
				break;
			}
		}
		if (likely(!(vm.flags & KRK_HAS_EXCEPTION))) continue;
_finishException:
		if (!handleException()) {
			frame = &vm.frames[vm.frameCount - 1];
			frame->ip = frame->closure->function->chunk.code + AS_HANDLER(krk_peek(0)).target;
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

KrkInstance * krk_startModule(const char * name) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	vm.module = module;
	krk_attachNamedObject(&module->fields, "__builtins__", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)krk_copyString(name,strlen(name)));
	return module;
}

KrkValue krk_interpret(const char * src, int newScope, char * fromName, char * fromFile) {
	KrkInstance * enclosing = vm.module;
	if (newScope) krk_startModule(fromName);

	KrkFunction * function = krk_compile(src, 0, fromFile);
	if (!function) {
		if (!vm.frameCount) handleException();
		return NONE_VAL();
	}

	krk_push(OBJECT_VAL(function));
	krk_attachNamedObject(&vm.module->fields, "__file__", (KrkObj*)function->chunk.filename);

	function->name = krk_copyString(fromName, strlen(fromName));

	KrkClosure * closure = krk_newClosure(function);
	krk_pop();

	krk_push(OBJECT_VAL(closure));
	krk_callValue(OBJECT_VAL(closure), 0, 1);

	KrkValue result = run();

	if (newScope) {
		KrkValue out = OBJECT_VAL(vm.module);
		vm.module = enclosing;
		return out;
	} else {
		return result;
	}
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

