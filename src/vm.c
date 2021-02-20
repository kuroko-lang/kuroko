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
#include "util.h"

#define KRK_VERSION_MAJOR  "1"
#define KRK_VERSION_MINOR  "1"
#define KRK_VERSION_PATCH  "0"

#define KRK_VERSION_EXTRA_BASE  "-preview"

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

/* Ensure we don't have a macro for this so we can reference a local version. */
#undef krk_currentThread

/* This is macro'd to krk_vm for namespacing reasons. */
KrkVM vm = {0};

#ifdef ENABLE_THREADING
/*
 * Marking our little VM thread state as 'initial-exec' is
 * the fastest way to allocate TLS data and yields virtually
 * identical performance in the single-thread case to not
 * having a TLS pointer, but it has some drawbacks...
 *
 * Despite documentation saying otherwise, a small thread-local
 * can generally be allocated even with dlopen, but this is
 * not guaranteed.
 */
__attribute__((tls_model("initial-exec")))
__thread KrkThreadState krk_currentThread;
#else
/* There is only one thread, so don't store it as TLS... */
KrkThreadState krk_currentThread;
#endif

/*
 * When threading is enabled, `krk_currentThread` becomes
 * a macro to call this outside of `vm.c` - we reference
 * the static TLS version above locally. When threading
 * is not enabled, there is a global `krk_currentThread`.
 */
KrkThreadState * krk_getCurrentThread(void) {
	return &krk_currentThread;
}

static struct Exceptions _exceptions = {0};
static struct BaseClasses _baseClasses = {0};
static KrkValue _specialMethodNames[METHOD__MAX];

/**
 * Reset the stack pointers, frame, upvalue list,
 * clear the exception flag and current exception;
 * happens on startup (twice) and after an exception.
 */
void krk_resetStack() {
	krk_currentThread.stackTop = krk_currentThread.stack;
	krk_currentThread.frameCount = 0;
	krk_currentThread.openUpvalues = NULL;
	krk_currentThread.flags &= ~KRK_THREAD_HAS_EXCEPTION;
	krk_currentThread.currentException = NONE_VAL();
}

#ifdef ENABLE_TRACING
/**
 * When tracing is enabled, we will present the elements on the stack with
 * a safe printer; the format of values printed by krk_printValueSafe will
 * look different from those printed by printValue, but they guarantee that
 * the VM will never be called to produce a string, which would result in
 * a nasty infinite recursion if we did it while trying to trace the VM!
 */
static void dumpStack(KrkCallFrame * frame) {
	fprintf(stderr, "        | ");
	size_t i = 0;
	for (KrkValue * slot = krk_currentThread.stack; slot < krk_currentThread.stackTop; slot++) {
		fprintf(stderr, "[ ");
		if (i == frame->slots) fprintf(stderr, "*");

		for (size_t x = krk_currentThread.frameCount; x > 0; x--) {
			if (krk_currentThread.frames[x-1].slots > i) continue;
			KrkCallFrame * f = &krk_currentThread.frames[x-1];
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
	if (!krk_valuesEqual(krk_currentThread.currentException,NONE_VAL())) {
		krk_push(krk_currentThread.currentException);
		KrkValue tracebackEntries;
		if (IS_INSTANCE(krk_currentThread.currentException)
			&& krk_tableGet(&AS_INSTANCE(krk_currentThread.currentException)->fields, OBJECT_VAL(S("traceback")), &tracebackEntries)
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
				KrkFunction * function = closure->function;
				size_t instruction = AS_INTEGER(entry->values.values[1]);

				/* Calculate the line number */
				int lineNo = (int)krk_lineNumber(&function->chunk, instruction);

				/* Print the simple stuff that we already know */
				fprintf(stderr, "  File \"%s\", line %d, in %s\n",
					(function->chunk.filename ? function->chunk.filename->chars : "?"),
					lineNo,
					(function->name ? function->name->chars : "(unnamed)"));

				/* Try to open the file */
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

		/* Is this a SyntaxError? Handle those specially. */
		if (krk_isInstanceOf(krk_currentThread.currentException, vm.exceptions->syntaxError)) {
			KrkValue result = krk_callSimple(OBJECT_VAL(krk_getType(krk_currentThread.currentException)->_tostr), 1, 0);
			fprintf(stderr, "%s\n", AS_CSTRING(result));
			return;
		}
		/* Clear the exception state while printing the exception. */
		krk_currentThread.flags &= ~(KRK_THREAD_HAS_EXCEPTION);
		fprintf(stderr, "%s", krk_typeName(krk_currentThread.currentException));
		KrkValue result = krk_callSimple(OBJECT_VAL(krk_getType(krk_currentThread.currentException)->_tostr), 1, 0);
		if (!IS_STRING(result)) {
			fprintf(stderr, "\n");
		} else {
			fprintf(stderr, ": %s\n", AS_CSTRING(result));
		}
		/* Turn the exception flag back on */
		krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
	}
}

/**
 * Attach a traceback to the current exception object, if it doesn't already have one.
 */
static void attachTraceback(void) {
	if (IS_INSTANCE(krk_currentThread.currentException)) {
		KrkInstance * theException = AS_INSTANCE(krk_currentThread.currentException);

		/* If there already is a traceback, don't add a new one; this exception was re-raised. */
		KrkValue existing;
		if (krk_tableGet(&theException->fields, OBJECT_VAL(S("traceback")), &existing)) return;

		KrkValue tracebackList = krk_list_of(0,NULL,0);
		krk_push(tracebackList);
		/* Build the traceback object */
		if (krk_currentThread.frameCount) {
			for (size_t i = 0; i < krk_currentThread.frameCount; i++) {
				KrkCallFrame * frame = &krk_currentThread.frames[i];
				KrkTuple * tbEntry = krk_newTuple(2);
				krk_push(OBJECT_VAL(tbEntry));
				tbEntry->values.values[tbEntry->values.count++] = OBJECT_VAL(frame->closure);
				tbEntry->values.values[tbEntry->values.count++] = INTEGER_VAL(frame->ip - frame->closure->function->chunk.code - 1);
				krk_tupleUpdateHash(tbEntry);
				krk_writeValueArray(AS_LIST(tracebackList), OBJECT_VAL(tbEntry));
				krk_pop();
			}
		}
		krk_attachNamedValue(&theException->fields, "traceback", tracebackList);
		krk_pop();
	} /* else: probably a legacy 'raise str', just don't bother. */
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
	krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;

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
	krk_currentThread.currentException = OBJECT_VAL(exceptionObject);
	attachTraceback();
	return NONE_VAL();
}

/**
 * Since the stack can potentially move when something is pushed to it
 * if it this triggers a grow condition, it may be necessary to ensure
 * that this has already happened before actually dealing with the stack.
 */
void krk_reserve_stack(size_t space) {
	while ((size_t)(krk_currentThread.stackTop - krk_currentThread.stack) + space > krk_currentThread.stackSize) {
		size_t old = krk_currentThread.stackSize;
		size_t old_offset = krk_currentThread.stackTop - krk_currentThread.stack;
		krk_currentThread.stackSize = GROW_CAPACITY(old);
		krk_currentThread.stack = GROW_ARRAY(KrkValue, krk_currentThread.stack, old, krk_currentThread.stackSize);
		krk_currentThread.stackTop = krk_currentThread.stack + old_offset;
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
	if (unlikely((size_t)(krk_currentThread.stackTop - krk_currentThread.stack) + 1 > krk_currentThread.stackSize)) {
		size_t old = krk_currentThread.stackSize;
		size_t old_offset = krk_currentThread.stackTop - krk_currentThread.stack;
		krk_currentThread.stackSize = GROW_CAPACITY(old);
		krk_currentThread.stack = GROW_ARRAY(KrkValue, krk_currentThread.stack, old, krk_currentThread.stackSize);
		krk_currentThread.stackTop = krk_currentThread.stack + old_offset;
	}
	*krk_currentThread.stackTop = value;
	krk_currentThread.stackTop++;
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
	krk_currentThread.stackTop--;
	if (unlikely(krk_currentThread.stackTop < krk_currentThread.stack)) {
		fprintf(stderr, "Fatal error: stack underflow detected in VM, issuing breakpoint.\n");
		return NONE_VAL();
	}
	return *krk_currentThread.stackTop;
}

/* Read a value `distance` units from the top of the stack without poping it. */
inline KrkValue krk_peek(int distance) {
	return krk_currentThread.stackTop[-1 - distance];
}

/* Exchange the value `distance` units down from the top of the stack with
 * the value at the top of the stack. */
inline void krk_swap(int distance) {
	KrkValue top = krk_currentThread.stackTop[-1];
	krk_currentThread.stackTop[-1] = krk_currentThread.stackTop[-1 - distance];
	krk_currentThread.stackTop[-1 - distance] = top;
}

/**
 * Bind a native function to the given table (eg. vm.builtins->fields, or _class->methods)
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
 * Create a new property object that calls a C function; same semantics as defineNative, but
 * instead of applying the function directly it is applied as a property value, so it should
 * be used with the "fields" table rather than the methods table. This will eventually replace
 * the ":field" option for defineNative().
 */
KrkProperty * krk_defineNativeProperty(KrkTable * table, const char * name, NativeFn function) {
	KrkNative * func = krk_newNative(function, name, 1);
	krk_push(OBJECT_VAL(func));
	KrkProperty * property = krk_newProperty(krk_peek(0));
	krk_attachNamedObject(table, name, (KrkObj*)property);
	krk_pop();
	return property;
}

/**
 * Shortcut for building classes.
 */
KrkClass * krk_makeClass(KrkInstance * module, KrkClass ** _class, const char * name, KrkClass * base) {
	KrkString * str_Name = krk_copyString(name,strlen(name));
	krk_push(OBJECT_VAL(str_Name));
	*_class = krk_newClass(str_Name, base);
	if (module) {
		krk_push(OBJECT_VAL(*_class));
		/* Bind it */
		krk_attachNamedObject(&module->fields,name,(KrkObj*)*_class);
		krk_pop();
	}
	krk_pop();
	return *_class;
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
		{&_class->_getslice, METHOD_GETSLICE},
		{&_class->_setslice, METHOD_SETSLICE},
		{&_class->_delslice, METHOD_DELSLICE},
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

/**
 * __builtins__.set_tracing(mode)
 *
 * Takes either one string "mode=value" or `n` keyword args mode=value.
 */
static KrkValue krk_set_tracing(int argc, KrkValue argv[], int hasKw) {
#ifdef DEBUG
	if (hasKw) {
		KrkValue test;
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("tracing")), &test) && IS_INTEGER(test)) {
			if (AS_INTEGER(test) == 1) krk_currentThread.flags |= KRK_THREAD_ENABLE_TRACING; else krk_currentThread.flags &= ~KRK_THREAD_ENABLE_TRACING; }
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("disassembly")), &test) && IS_INTEGER(test)) {
			if (AS_INTEGER(test) == 1) krk_currentThread.flags |= KRK_THREAD_ENABLE_DISASSEMBLY; else krk_currentThread.flags &= ~KRK_THREAD_ENABLE_DISASSEMBLY; }
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("scantracing")), &test) && IS_INTEGER(test)) {
			if (AS_INTEGER(test) == 1) krk_currentThread.flags |= KRK_THREAD_ENABLE_SCAN_TRACING; else krk_currentThread.flags &= ~KRK_THREAD_ENABLE_SCAN_TRACING; }

		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("stressgc")), &test) && IS_INTEGER(test)) {
			if (AS_INTEGER(test) == 1) vm.globalFlags |= KRK_GLOBAL_ENABLE_STRESS_GC; else krk_currentThread.flags &= ~KRK_GLOBAL_ENABLE_STRESS_GC; }
	}
	return BOOLEAN_VAL(1);
#else
	return krk_runtimeError(vm.exceptions->typeError,"Debugging is not enabled in this build.");
#endif
}

/**
 * Maps values to their base classes.
 * Internal version of type().
 */
inline KrkClass * krk_getType(KrkValue of) {
	switch (of.type) {
		case VAL_INTEGER:
			return vm.baseClasses->intClass;
		case VAL_FLOATING:
			return vm.baseClasses->floatClass;
		case VAL_BOOLEAN:
			return vm.baseClasses->boolClass;
		case VAL_NONE:
			return vm.baseClasses->noneTypeClass;
		case VAL_OBJECT:
			switch (AS_OBJECT(of)->type) {
				case OBJ_CLASS:
					return vm.baseClasses->typeClass;
				case OBJ_NATIVE:
				case OBJ_FUNCTION:
				case OBJ_CLOSURE:
					return vm.baseClasses->functionClass;
				case OBJ_BOUND_METHOD:
					return vm.baseClasses->methodClass;
				case OBJ_STRING:
					return vm.baseClasses->strClass;
				case OBJ_TUPLE:
					return vm.baseClasses->tupleClass;
				case OBJ_BYTES:
					return vm.baseClasses->bytesClass;
				case OBJ_PROPERTY:
					return vm.baseClasses->propertyClass;
				case OBJ_INSTANCE:
					return AS_INSTANCE(of)->_class;
				default:
					return vm.baseClasses->objectClass;
			} break;
		default:
			return vm.baseClasses->objectClass;
	}
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

static int checkArgumentCount(KrkClosure * closure, int argCount) {
	int minArgs = closure->function->requiredArgs;
	int maxArgs = minArgs + closure->function->keywordArgs;
	if (argCount < minArgs || argCount > maxArgs) {
		krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)",
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
	krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%s'",
		closure->function->name ? closure->function->name->chars : "<unnamed function>",
		(destination < closure->function->requiredArgs ? AS_CSTRING(closure->function->requiredArgNames.values[destination]) :
			(destination - closure->function->requiredArgs < closure->function->keywordArgs ? AS_CSTRING(closure->function->keywordArgNames.values[destination - closure->function->requiredArgs]) :
				"(unnamed arg)")));
}

#undef unpackError
#define unpackError(fromInput) return krk_runtimeError(vm.exceptions->typeError, "Can not unpack *expression: '%s' object is not iterable", krk_typeName(fromInput)), 0;
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
int krk_processComplexArguments(int argCount, KrkValueArray * positionals, KrkTable * keywords) {
	size_t kwargsCount = AS_INTEGER(krk_currentThread.stackTop[-1]);
	krk_pop(); /* Pop the arg counter */
	argCount--;

	krk_initValueArray(positionals);
	krk_initTable(keywords);

	/* First, process all the positionals, including any from extractions. */
	size_t existingPositionalArgs = argCount - kwargsCount * 2;
	for (size_t i = 0; i < existingPositionalArgs; ++i) {
		krk_writeValueArray(positionals, krk_currentThread.stackTop[-argCount + i]);
	}

	KrkValue * startOfExtras = &krk_currentThread.stackTop[-kwargsCount * 2];
	/* Now unpack everything else. */
	for (size_t i = 0; i < kwargsCount; ++i) {
		KrkValue key = startOfExtras[i*2];
		KrkValue value = startOfExtras[i*2 + 1];
		if (IS_KWARGS(key)) {
			if (AS_INTEGER(key) == LONG_MAX-1) { /* unpack list */
				unpackIterableFast(value);
			} else if (AS_INTEGER(key) == LONG_MAX-2) { /* unpack dict */
				if (!IS_INSTANCE(value)) {
					krk_runtimeError(vm.exceptions->typeError, "**expression value is not a dict.");
					return 0;
				}
				for (size_t i = 0; i < AS_DICT(value)->capacity; ++i) {
					KrkTableEntry * entry = &AS_DICT(value)->entries[i];
					if (entry->key.type != VAL_KWARGS) {
						if (!IS_STRING(entry->key)) {
							krk_runtimeError(vm.exceptions->typeError, "**expression contains non-string key");
							return 0;
						}
						if (!krk_tableSet(keywords, entry->key, entry->value)) {
							krk_runtimeError(vm.exceptions->typeError, "got multiple values for argument '%s'", AS_CSTRING(entry->key));
							return 0;
						}
					}
				}
			} else if (AS_INTEGER(key) == LONG_MAX) { /* single value */
				krk_writeValueArray(positionals, value);
			}
		} else if (IS_STRING(key)) {
			if (!krk_tableSet(keywords, key, value)) {
				krk_runtimeError(vm.exceptions->typeError, "got multiple values for argument '%s'", AS_CSTRING(key));
				return 0;
			}
		}
	}
	return 1;
}
#undef unpackArray

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
	KrkValue * startOfPositionals = &krk_currentThread.stackTop[-argCount];
	size_t potentialPositionalArgs = closure->function->requiredArgs + closure->function->keywordArgs;
	size_t totalArguments = closure->function->requiredArgs + closure->function->keywordArgs + closure->function->collectsArguments + closure->function->collectsKeywords;
	size_t offsetOfExtraArgs = closure->function->requiredArgs + closure->function->keywordArgs;
	size_t argCountX = argCount;
	KrkValueArray * positionals;
	KrkTable * keywords;

	if (argCount && IS_KWARGS(krk_currentThread.stackTop[-1])) {

		KrkValue myList = krk_list_of(0,NULL,0);
		krk_currentThread.scratchSpace[0] = myList;
		KrkValue myDict = krk_dict_of(0,NULL,0);
		krk_currentThread.scratchSpace[1] = myDict;
		positionals = AS_LIST(myList);
		keywords = AS_DICT(myDict);

		/* This processes the existing argument list into a ValueArray and a Table with the args and keywords */
		if (!krk_processComplexArguments(argCount, positionals, keywords)) goto _errorDuringPositionals;
		argCount--; /* It popped the KWARGS value from the top, so we have one less argument */

		/* Do we already know we have too many arguments? Let's bail before doing a bunch of work. */
		if ((positionals->count > potentialPositionalArgs) && (!closure->function->collectsArguments)) {
			checkArgumentCount(closure,positionals->count);
			goto _errorDuringPositionals;
		}

		/* Prepare stack space for all potential positionals, mark them unset */
		for (size_t i = 0; i < (size_t)argCount; ++i) {
			krk_currentThread.stackTop[-argCount + i] = KWARGS_VAL(0);
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
		for (size_t i = 0; i < potentialPositionalArgs && i < positionals->count; ++i) {
			krk_currentThread.stackTop[-argCount + i] = positionals->values[i];
		}

		if (closure->function->collectsArguments) {
			size_t count  = (positionals->count > potentialPositionalArgs) ? (positionals->count - potentialPositionalArgs) : 0;
			KrkValue * offset = (count == 0) ? NULL : &positionals->values[potentialPositionalArgs];
			krk_push(krk_list_of(count, offset, 0));
			argCount++;
		}

		krk_freeValueArray(positionals);
		krk_currentThread.scratchSpace[0] = NONE_VAL();

		/* Now place keyword arguments */
		for (size_t i = 0; i < keywords->capacity; ++i) {
			KrkTableEntry * entry = &keywords->entries[i];
			if (entry->key.type != VAL_KWARGS) {
				KrkValue name = entry->key;
				KrkValue value = entry->value;
				/* See if we can place it */
				for (int j = 0; j < (int)closure->function->requiredArgs; ++j) {
					if (krk_valuesEqual(name, closure->function->requiredArgNames.values[j])) {
						if (!IS_KWARGS(krk_currentThread.stackTop[-argCount + j])) {
							multipleDefs(closure,j);
							goto _errorAfterPositionals;
						}
						krk_currentThread.stackTop[-argCount + j] = value;
						goto _finishKwarg;
					}
				}
				/* See if it's a keyword arg. */
				for (int j = 0; j < (int)closure->function->keywordArgs; ++j) {
					if (krk_valuesEqual(name, closure->function->keywordArgNames.values[j])) {
						if (!IS_KWARGS(krk_currentThread.stackTop[-argCount + j + closure->function->requiredArgs])) {
							multipleDefs(closure, j + closure->function->requiredArgs);
							goto _errorAfterPositionals;
						}
						krk_currentThread.stackTop[-argCount + j + closure->function->requiredArgs] = value;
						goto _finishKwarg;
					}
				}
				if (!closure->function->collectsKeywords) {
					krk_runtimeError(vm.exceptions->typeError, "%s() got an unexpected keyword argument '%s'",
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
			krk_push(krk_dict_of(0,NULL,0));
			argCount++;
			krk_tableAddAll(keywords, AS_DICT(krk_peek(0)));
		}

		krk_freeTable(keywords);
		krk_currentThread.scratchSpace[1] = NONE_VAL();

		for (size_t i = 0; i < (size_t)closure->function->requiredArgs; ++i) {
			if (IS_KWARGS(krk_currentThread.stackTop[-argCount + i])) {
				krk_runtimeError(vm.exceptions->typeError, "%s() missing required positional argument: '%s'",
					closure->function->name ? closure->function->name->chars : "<unnamed function>",
					AS_CSTRING(closure->function->requiredArgNames.values[i]));
				goto _errorAfterKeywords;
			}
		}

		argCountX = argCount - (closure->function->collectsArguments + closure->function->collectsKeywords);
	} else {
		/* We can't have had any kwargs. */
		if ((size_t)argCount > potentialPositionalArgs && closure->function->collectsArguments) {
			krk_push(NONE_VAL()); krk_push(NONE_VAL()); krk_pop(); krk_pop();
			startOfPositionals[offsetOfExtraArgs] = krk_list_of(argCount - potentialPositionalArgs,
				&startOfPositionals[potentialPositionalArgs], 0);
			argCount = closure->function->requiredArgs + 1;
			argCountX = argCount - 1;
			while (krk_currentThread.stackTop > startOfPositionals + argCount) krk_pop();
		}
	}
	if (!checkArgumentCount(closure, argCountX)) {
		return 0;
	}
	while (argCount < (int)totalArguments) {
		krk_push(KWARGS_VAL(0));
		argCount++;
	}
	if (krk_currentThread.frameCount == KRK_CALL_FRAMES_MAX) {
		krk_runtimeError(vm.exceptions->baseException, "Too many call frames.");
		return 0;
	}
	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = (krk_currentThread.stackTop - argCount) - krk_currentThread.stack;
	frame->outSlots = (krk_currentThread.stackTop - argCount - extra) - krk_currentThread.stack;
	frame->globals = &closure->function->globalsContext->fields;
	return 1;

_errorDuringPositionals:
	krk_freeValueArray(positionals);
	krk_currentThread.scratchSpace[0] = NONE_VAL();
_errorAfterPositionals:
	krk_freeTable(keywords);
	krk_currentThread.scratchSpace[1] = NONE_VAL();
_errorAfterKeywords:
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
				NativeFn native = (NativeFn)AS_NATIVE(callee)->function;
				if (argCount && IS_KWARGS(krk_currentThread.stackTop[-1])) {
					KrkValue myList = krk_list_of(0,NULL,0);
					krk_currentThread.scratchSpace[0] = myList;
					KrkValue myDict = krk_dict_of(0,NULL,0);
					krk_currentThread.scratchSpace[1] = myDict;
					if (!krk_processComplexArguments(argCount, AS_LIST(myList), AS_DICT(myDict))) {
						return 0;
					}
					argCount--; /* Because that popped the kwargs value */
					krk_currentThread.stackTop -= argCount + extra; /* We can just put the stack back to normal */
					krk_push(myList);
					krk_push(myDict);
					krk_currentThread.scratchSpace[0] = NONE_VAL();
					krk_currentThread.scratchSpace[1] = NONE_VAL();
					krk_writeValueArray(AS_LIST(myList), myDict);
					KrkValue result = native(AS_LIST(myList)->count-1, AS_LIST(myList)->values, 1);
					if (krk_currentThread.stackTop == krk_currentThread.stack) return 0;
					krk_pop();
					krk_pop();
					krk_push(result);
				} else {
					KrkValue * stackCopy = malloc(argCount * sizeof(KrkValue));
					memcpy(stackCopy, krk_currentThread.stackTop - argCount, argCount * sizeof(KrkValue));
					KrkValue result = native(argCount, stackCopy, 0);
					free(stackCopy);
					if (krk_currentThread.stackTop == krk_currentThread.stack) return 0;
					krk_currentThread.stackTop -= argCount + extra;
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
					krk_runtimeError(vm.exceptions->typeError, "Attempted to call non-callable type: %s", krk_typeName(callee));
					return 0;
				}
			}
			case OBJ_CLASS: {
				KrkClass * _class = AS_CLASS(callee);
				KrkInstance * newInstance = krk_newInstance(_class);
				krk_currentThread.stackTop[-argCount - 1] = OBJECT_VAL(newInstance);
				KrkValue initializer;
				if (_class->_init) {
					return krk_callValue(OBJECT_VAL(_class->_init), argCount + 1, 0);
				} else if (krk_tableGet(&_class->methods, vm.specialMethodNames[METHOD_INIT], &initializer)) {
					return krk_callValue(initializer, argCount + 1, 0);
				} else if (argCount != 0) {
					krk_runtimeError(vm.exceptions->attributeError, "Class does not have an __init__ but arguments were passed to initializer: %d", argCount);
					return 0;
				}
				return 1;
			}
			case OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				krk_currentThread.stackTop[-argCount - 1] = bound->receiver;
				if (!bound->method) {
					krk_runtimeError(vm.exceptions->argumentError, "Attempted to call a method binding with no attached callable (did you forget to return something from a method decorator?)");
					return 0;
				}
				return krk_callValue(OBJECT_VAL(bound->method), argCount + 1, 0);
			}
			default:
				break;
		}
	}
	krk_runtimeError(vm.exceptions->typeError, "Attempted to call non-callable type: %s", krk_typeName(callee));
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
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	return krk_runtimeError(vm.exceptions->typeError, "Invalid internal method call: %d ('%s')", result, krk_typeName(value));
}

/**
 * Attach a method call to its callee and return a BoundMethod.
 * Works for managed and native method calls.
 */
int krk_bindMethod(KrkClass * _class, KrkString * name) {
	KrkValue method, out;
	if (!krk_tableGet(&_class->methods, OBJECT_VAL(name), &method)) return 0;
	if (IS_NATIVE(method) && ((KrkNative*)AS_OBJECT(method))->isMethod == 2) {
		out = AS_NATIVE(method)->function(1, (KrkValue[]){krk_peek(0)}, 0);
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
	KrkUpvalue * upvalue = krk_currentThread.openUpvalues;
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
		krk_currentThread.openUpvalues = createdUpvalue;
	} else {
		prevUpvalue->next = createdUpvalue;
	}
	return createdUpvalue;
}

#define UPVALUE_LOCATION(upvalue) (upvalue->location == -1 ? &upvalue->closed : &upvalue->owner->stack[upvalue->location])

/**
 * Close upvalues by moving them out of the stack and into the heap.
 * Their location attribute is set to -1 to indicate they now live on the heap.
 */
static void closeUpvalues(int last) {
	while (krk_currentThread.openUpvalues != NULL && krk_currentThread.openUpvalues->location >= last) {
		KrkUpvalue * upvalue = krk_currentThread.openUpvalues;
		upvalue->closed = krk_currentThread.stack[upvalue->location];
		upvalue->location = -1;
		krk_currentThread.openUpvalues = upvalue->next;
	}
}

/**
 * Attach an object to a table.
 *
 * Generally used to attach classes or objects to the globals table, or to
 * a native module's export object.
 */
void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj) {
	krk_push(OBJECT_VAL(obj));
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_tableSet(table, krk_peek(0), krk_peek(1));
	krk_pop();
	krk_pop();
}

/**
 * Same as above, but the object has already been wrapped in a value.
 */
void krk_attachNamedValue(KrkTable * table, const char name[], KrkValue obj) {
	krk_push(obj);
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_tableSet(table, krk_peek(0), krk_peek(1));
	krk_pop();
	krk_pop();
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
int krk_isFalsey(KrkValue value) {
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

static KrkValue krk_getsize(int argc, KrkValue argv[], int hasKw) {
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
			mySize += sizeof(KrkTableEntry) * self->fields.capacity;
			KrkClass * type = krk_getType(argv[0]);
			if (type->allocSize) {
				mySize += type->allocSize;
			} else {
				mySize += sizeof(KrkInstance);
			}
			if (krk_isInstanceOf(argv[0], vm.baseClasses->listClass)) {
				mySize += sizeof(KrkValue) * AS_LIST(argv[0])->capacity;
			} else if (krk_isInstanceOf(argv[0], vm.baseClasses->dictClass)) {
				mySize += sizeof(KrkTableEntry) * AS_DICT(argv[0])->capacity;
			}
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

static KrkValue krk_setclean(int argc, KrkValue argv[], int hasKw) {
	if (!argc || (IS_BOOLEAN(argv[0]) && AS_BOOLEAN(argv[0]))) {
		vm.globalFlags |= KRK_GLOBAL_CLEAN_OUTPUT;
	} else {
		vm.globalFlags &= ~KRK_GLOBAL_CLEAN_OUTPUT;
	}
	return NONE_VAL();
}

static KrkValue krk_import_wrapper(int argc, KrkValue argv[], int hasKw) {
	if (!argc || !IS_STRING(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected string");
	if (!krk_doRecursiveModuleLoad(AS_STRING(argv[0]))) return NONE_VAL(); /* ImportError already raised */
	return krk_pop();
}

void krk_initVM(int flags) {
	vm.globalFlags = flags & 0xFF00;

	/* Reset current thread */
	krk_resetStack();
	krk_currentThread.frames   = calloc(KRK_CALL_FRAMES_MAX,sizeof(KrkCallFrame));
	krk_currentThread.flags    = flags & 0x00FF;
	krk_currentThread.module   = NULL;
	krk_currentThread.watchdog = 0;
	vm.threads = &krk_currentThread;
	vm.threads->next = NULL;

	/* GC state */
	vm.objects = NULL;
	vm.bytesAllocated = 0;
	vm.nextGC = 1024 * 1024;
	vm.grayCount = 0;
	vm.grayCapacity = 0;
	vm.grayStack = NULL;

	/* Global objects */
	vm.exceptions = &_exceptions;
	vm.baseClasses = &_baseClasses;
	vm.specialMethodNames = _specialMethodNames;
	krk_initTable(&vm.strings);
	krk_initTable(&vm.modules);

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
		_(METHOD_SETSLICE, "__setslice__"),
		_(METHOD_DELSLICE, "__delslice__"),
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

	/* Build classes for basic types */
	_createAndBind_builtins();
	_createAndBind_type();
	_createAndBind_numericClasses();
	_createAndBind_strClass();
	_createAndBind_listClass();
	_createAndBind_tupleClass();
	_createAndBind_bytesClass();
	_createAndBind_dictClass();
	_createAndBind_functionClass();
	_createAndBind_rangeClass();
	_createAndBind_setClass();
	_createAndBind_exceptions();
	_createAndBind_gcMod();
	_createAndBind_disMod();
	_createAndBind_timeMod();
	_createAndBind_osMod();
	_createAndBind_fileioMod();
#ifdef ENABLE_THREADING
	_createAndBind_threadsMod();
#endif

	/**
	 * kuroko = module()
	 *
	 * This is equivalent to Python's "sys" module, but we do not use that name
	 * in consideration of future compatibility, where a "sys" module may be
	 * added to emulate Python version numbers, etc.
	 */
	vm.system = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "kuroko", (KrkObj*)vm.system);
	krk_attachNamedObject(&vm.system->fields, "__name__", (KrkObj*)S("kuroko"));
	krk_attachNamedValue(&vm.system->fields, "__file__", NONE_VAL()); /* (built-in) */
	krk_attachNamedObject(&vm.system->fields, "__doc__", (KrkObj*)S("System module."));
	krk_attachNamedObject(&vm.system->fields, "version",
		(KrkObj*)S(KRK_VERSION_MAJOR "." KRK_VERSION_MINOR "." KRK_VERSION_PATCH KRK_VERSION_EXTRA));
	krk_attachNamedObject(&vm.system->fields, "buildenv", (KrkObj*)S(KRK_BUILD_COMPILER));
	krk_attachNamedObject(&vm.system->fields, "builddate", (KrkObj*)S(KRK_BUILD_DATE));
	krk_defineNative(&vm.system->fields, "getsizeof", krk_getsize)->doc = "Calculate the approximate size of an object in bytes.\n"
		"@arguments value\n\n"
		"@param value Value to examine.";
	krk_defineNative(&vm.system->fields, "set_clean_output", krk_setclean)->doc = "Disables terminal escapes in some output from the VM.\n"
		"@arguments clean=True\n\n"
		"@param clean Whether to remove escapes.";
	krk_defineNative(&vm.system->fields, "set_tracing", krk_set_tracing)->doc = "Toggle debugging modes.\n"
		"@arguments tracing=None,disassembly=None,scantracing=None,stressgc=None\n\n"
		"Enables or disables tracing options for the current thread.\n\n"
		"@param tracing Enables instruction tracing.\n"
		"@param disassembly Prints bytecode disassembly after compilation.\n"
		"@param scantracing Prints debug output from the token scanner during compilation.\n"
		"@param stressgc Forces a garbage collection cycle on each heap allocation.";
	krk_defineNative(&vm.system->fields, "importmodule", krk_import_wrapper)->doc = "Import a module by string name\n"
		"@arguments module\n\n"
		"Imports the dot-separated module @p module as if it were imported by the @c import statement and returns the resulting module object.\n\n"
		"@param module A string with a dot-separated package or module name";
	krk_attachNamedObject(&vm.system->fields, "module", (KrkObj*)vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.system->fields, "path_sep", (KrkObj*)S(PATH_SEP));
	KrkValue module_paths = krk_list_of(0,NULL,0);
	krk_attachNamedValue(&vm.system->fields, "module_paths", module_paths);
	krk_writeValueArray(AS_LIST(module_paths), OBJECT_VAL(S("./")));
	if (vm.binpath) {
		krk_attachNamedObject(&vm.system->fields, "executable_path", (KrkObj*)krk_copyString(vm.binpath, strlen(vm.binpath)));
		char * dir = strdup(vm.binpath);
#ifndef _WIN32
		char * slash = strrchr(dir,'/');
		if (slash) *slash = '\0';
		if (strstr(dir,"/bin") == (dir + strlen(dir) - 4)) {
			slash = strrchr(dir,'/');
			if (slash) *slash = '\0';
			size_t allocSize = sizeof("/lib/kuroko/") + strlen(dir);
			char * out = malloc(allocSize);
			size_t len = snprintf(out, allocSize, "%s/lib/kuroko/", dir);
			krk_writeValueArray(AS_LIST(module_paths), OBJECT_VAL(krk_takeString(out, len)));
		} else {
			size_t allocSize = sizeof("/modules/") + strlen(dir);
			char * out = malloc(allocSize);
			size_t len = snprintf(out, allocSize, "%s/modules/", dir);
			krk_writeValueArray(AS_LIST(module_paths), OBJECT_VAL(krk_takeString(out, len)));
		}
#else
		char * backslash = strrchr(dir,'\\');
		if (backslash) *backslash = '\0';
		size_t allocSize = sizeof("\\modules\\") + strlen(dir);
		char * out = malloc(allocSize);
		size_t len = snprintf(out, allocSize, "%s\\modules\\", dir);
		krk_writeValueArray(AS_LIST(module_paths), OBJECT_VAL(krk_takeString(out,len)));
#endif
		free(dir);
	}

	/* The VM is now ready to start executing code. */
	krk_resetStack();
}

/**
 * Reclaim resources used by the VM.
 */
void krk_freeVM() {
	krk_freeTable(&vm.strings);
	krk_freeTable(&vm.modules);
	memset(_specialMethodNames,0,sizeof(_specialMethodNames));
	krk_freeObjects();

	/* for thread in threads... */
	FREE_ARRAY(size_t, krk_currentThread.stack, krk_currentThread.stackSize);
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
		return krk_runtimeError(vm.exceptions->typeError, msg, krk_typeName(a), krk_typeName(b));
	} else {
		return value;
	}
}

/**
 * Basic arithmetic and string functions follow.
 */

#define MAKE_BIN_OP(name,operator) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
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
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		return tryBind("__" #name "__", a, b, "unsupported operand types for " #operator ": '%s' and '%s'"); \
	}

MAKE_UNOPTIMIZED_BIN_OP(pow,**)

/* Bit ops are invalid on doubles in C, so we can't use the same set of macros for them;
 * they should be invalid in Kuroko as well. */
#define MAKE_BIT_OP(name,operator) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
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
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
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
	int exitSlot = (krk_currentThread.exitOnFrame >= 0) ? krk_currentThread.frames[krk_currentThread.exitOnFrame].outSlots : 0;
	for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1); stackOffset >= exitSlot && !IS_TRY_HANDLER(krk_currentThread.stack[stackOffset]); stackOffset--);
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
			krk_currentThread.frameCount = 0;
		}
		/* If exitSlot was not 0, there was an exception during a call to runNext();
		 * this is likely to be raised higher up the stack as an exception in the outer
		 * call, but we don't want to print the traceback here. */
		return 1;
	}

	/* Find the call frame that owns this stack slot */
	for (frameOffset = krk_currentThread.frameCount - 1; frameOffset >= 0 && (int)krk_currentThread.frames[frameOffset].slots > stackOffset; frameOffset--);
	if (frameOffset == -1) {
		fprintf(stderr, "Internal error: Call stack is corrupted - unable to find\n");
		fprintf(stderr, "                call frame that owns exception handler.\n");
		exit(1);
	}

	/* We found an exception handler and can reset the VM to its call frame. */
	closeUpvalues(stackOffset);
	krk_currentThread.stackTop = krk_currentThread.stack + stackOffset + 1;
	krk_currentThread.frameCount = frameOffset + 1;

	/* Clear the exception flag so we can continue executing from the handler. */
	krk_currentThread.flags &= ~KRK_THREAD_HAS_EXCEPTION;
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
		krk_runtimeError(vm.exceptions->baseException,
			"Internal error: kuroko.module_paths not defined.");
		return 0;
	}

	/* Obtain __builtins__.module_paths.__list so we can do lookups directly */
	int moduleCount = AS_LIST(modulePaths)->count;
	if (!moduleCount) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions->importError,
			"No module search directories are specified, so no modules may be imported.");
		return 0;
	}

	struct stat statbuf;

	/* First search for {path}.krk in the module search paths */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		krk_push(AS_LIST(modulePaths)->values[i]);
		if (!IS_STRING(krk_peek(0))) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->typeError,
				"Module search paths must be strings; check the search path at index %d", i);
			return 0;
		}
		krk_push(OBJECT_VAL(path));
		krk_addObjects(); /* Concatenate path... */
		krk_push(OBJECT_VAL(S(".krk")));
		krk_addObjects(); /* and file extension */
		int isPackage = 0;

		char * fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) < 0) {
			krk_pop();
			/* try /__init__.krk */
			krk_push(AS_LIST(modulePaths)->values[i]);
			krk_push(OBJECT_VAL(path));
			krk_addObjects();
			krk_push(OBJECT_VAL(S(PATH_SEP "__init__.krk")));
			krk_addObjects();
			fileName = AS_CSTRING(krk_peek(0));
			if (stat(fileName,&statbuf) < 0) {
				continue;
			}
			isPackage = 1;
		}

		/* Compile and run the module in a new context and exit the VM when it
		 * returns to the current call frame; modules should return objects. */
		KrkInstance * enclosing = krk_currentThread.module;
		krk_startModule(runAs->chars);
		krk_tableSet(&vm.modules, OBJECT_VAL(runAs), OBJECT_VAL(krk_currentThread.module));
		if (isPackage) krk_attachNamedValue(&krk_currentThread.module->fields,"__ispackage__",BOOLEAN_VAL(1));
		*moduleOut = krk_callfile(fileName,runAs->chars,fileName);
		krk_currentThread.module = enclosing;
		if (!IS_OBJECT(*moduleOut)) {
			if (!(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
				krk_runtimeError(vm.exceptions->importError,
					"Failed to load module '%s' from '%s'", runAs->chars, fileName);
			}
			return 0;
		}

		krk_pop(); /* concatenated filename on stack */
		krk_push(*moduleOut);
		return 1;
	}

#ifndef STATIC_ONLY
	/* If we didn't find {path}.krk, try {path}.so in the same order */
	for (int i = 0; i < moduleCount; ++i, krk_pop()) {
		/* Assume things haven't changed and all of these are strings. */
		krk_push(AS_LIST(modulePaths)->values[i]);
		krk_push(OBJECT_VAL(path));
		krk_addObjects(); /* this should just be basic concatenation */
		krk_push(OBJECT_VAL(S(".so")));
		krk_addObjects();

		char * fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) < 0) continue;

		dlRefType dlRef = dlOpen(fileName);
		if (!dlRef) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->importError,
				"Failed to load native module '%s' from shared object '%s'", runAs->chars, fileName);
			return 0;
		}

		const char * start = path->chars;
		for (const char * c = start; *c; c++) {
			if (*c == '.') start = c + 1;
		}

		krk_push(OBJECT_VAL(S("krk_module_onload_")));
		krk_push(OBJECT_VAL(krk_copyString(start,strlen(start))));
		krk_addObjects();

		char * handlerName = AS_CSTRING(krk_peek(0));

		KrkValue (*moduleOnLoad)(KrkString * name);
		dlSymType out = dlSym(dlRef, handlerName);
		memcpy(&moduleOnLoad,&out,sizeof(out));

		if (!moduleOnLoad) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->importError,
				"Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad(runAs);
		if (!IS_INSTANCE(*moduleOut)) {
			krk_runtimeError(vm.exceptions->importError,
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
	krk_runtimeError(vm.exceptions->importError, "No module named '%s'", runAs->chars);
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
	int argBase = krk_currentThread.stackTop - krk_currentThread.stack;
	krk_push(NONE_VAL());         // 0: Name of current node being processed.
	krk_push(OBJECT_VAL(S("")));  // 1: slash/separated/path
	krk_push(OBJECT_VAL(S("")));  // 2: dot.separated.path
	krk_push(OBJECT_VAL(name));   // 3: remaining path to process
	krk_push(OBJECT_VAL(S("."))); // 4: string "." to search for
	do {
		KrkValue listOut = krk_string_split(3,(KrkValue[]){krk_currentThread.stack[argBase+3], krk_currentThread.stack[argBase+4], INTEGER_VAL(1)}, 0);
		if (!IS_INSTANCE(listOut)) return 0;

		/* Set node */
		krk_currentThread.stack[argBase+0] = AS_LIST(listOut)->values[0];

		/* Set remainder */
		if (AS_LIST(listOut)->count > 1) {
			krk_currentThread.stack[argBase+3] = AS_LIST(listOut)->values[1];
		} else {
			krk_currentThread.stack[argBase+3] = NONE_VAL();
		}

		/* First is /-path */
		krk_push(krk_currentThread.stack[argBase+1]);
		krk_push(krk_currentThread.stack[argBase+0]);
		krk_addObjects();
		krk_currentThread.stack[argBase+1] = krk_pop();
		/* Second is .-path */
		krk_push(krk_currentThread.stack[argBase+2]);
		krk_push(krk_currentThread.stack[argBase+0]);
		krk_addObjects();
		krk_currentThread.stack[argBase+2] = krk_pop();

		if (IS_NONE(krk_currentThread.stack[argBase+3])) {
			krk_pop(); /* dot */
			krk_pop(); /* remainder */
			KrkValue current;
			if (!krk_loadModule(AS_STRING(krk_currentThread.stack[argBase+1]), &current, AS_STRING(krk_currentThread.stack[argBase+2]))) return 0;
			krk_pop(); /* dot-sepaerated */
			krk_pop(); /* slash-separated */
			krk_push(current);
			/* last must be something if we got here, because single-level import happens elsewhere */
			krk_tableSet(&AS_INSTANCE(krk_currentThread.stack[argBase-1])->fields, krk_currentThread.stack[argBase+0], krk_peek(0));
			krk_currentThread.stackTop = krk_currentThread.stack + argBase;
			krk_currentThread.stackTop[-1] = current;
			return 1;
		} else {
			KrkValue current;
			if (!krk_loadModule(AS_STRING(krk_currentThread.stack[argBase+1]), &current, AS_STRING(krk_currentThread.stack[argBase+2]))) return 0;
			krk_push(current);
			if (!IS_NONE(krk_currentThread.stack[argBase-1])) {
				krk_tableSet(&AS_INSTANCE(krk_currentThread.stack[argBase-1])->fields, krk_currentThread.stack[argBase+0], krk_peek(0));
			}
			/* Is this a package? */
			KrkValue tmp;
			if (!krk_tableGet(&AS_INSTANCE(current)->fields, OBJECT_VAL(S("__ispackage__")), &tmp) || !IS_BOOLEAN(tmp) || AS_BOOLEAN(tmp) != 1) {
				krk_runtimeError(vm.exceptions->importError, "'%s' is not a package", AS_CSTRING(krk_currentThread.stack[argBase+2]));
				return 0;
			}
			krk_currentThread.stack[argBase-1] = krk_pop();
			/* Now concatenate forward slash... */
			krk_push(krk_currentThread.stack[argBase+1]); /* Slash path */
			krk_push(OBJECT_VAL(S(PATH_SEP)));
			krk_addObjects();
			krk_currentThread.stack[argBase+1] = krk_pop();
			/* And now for the dot... */
			krk_push(krk_currentThread.stack[argBase+2]);
			krk_push(krk_currentThread.stack[argBase+4]);
			krk_addObjects();
			krk_currentThread.stack[argBase+2] = krk_pop();
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
				/* Properties retreived from instances are magic. */
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

KrkValue krk_valueGetAttribute(KrkValue value, char * name) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(value);
	if (!valueGetProperty(AS_STRING(krk_peek(1)))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name);
	}
	krk_swap(1);
	krk_pop(); /* String */
	return krk_pop();
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
#define BINARY_OP(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); krk_push(krk_operator_ ## op (a,b)); break; }
#define BINARY_OP_CHECK_ZERO(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); \
	if ((IS_INTEGER(b) && AS_INTEGER(b) == 0)) { krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division or modulo by zero"); goto _finishException; } \
	else if ((IS_FLOATING(b) && AS_FLOATING(b) == 0.0)) { krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero"); goto _finishException; } \
	krk_push(krk_operator_ ## op (a,b)); break; }
#define READ_CONSTANT(s) (frame->closure->function->chunk.constants.values[OPERAND])
#define READ_STRING(s) AS_STRING(READ_CONSTANT(s))

/**
 * Read bytes after an opcode. Most instructions take 1, 2, or 3 bytes as an
 * operand referring to a local slot, constant slot, or offset.
 */
static inline size_t readBytes(KrkCallFrame * frame, int num) {
	size_t out = 0;
	switch (num) {
		case 3: out = READ_BYTE(); /* fallthrough*/
		case 2: out <<= 8; out |= READ_BYTE(); /* fallthrough */
		case 1: out <<= 8; out |= READ_BYTE(); /* fallthrough */
		case 0: return out;
	}
	return out;
}

/**
 * VM main loop.
 */
static KrkValue run() {
	KrkCallFrame* frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];

	while (1) {
#ifdef ENABLE_TRACING
		if (krk_currentThread.flags & KRK_THREAD_ENABLE_TRACING) {
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

		/* Each instruction begins with one opcode byte */
		uint8_t opcode = READ_BYTE();

		/* The top two bits of the opcode indicate how many bytes
		 * of operands it takes: 0, 1, 2, or 3 (naturally) */
		size_t OPERAND = readBytes(frame, opcode >> 6);

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
				for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1); stackOffset >= (int)frame->slots && !IS_WITH_HANDLER(krk_currentThread.stack[stackOffset]); stackOffset--);
				if (stackOffset >= (int)frame->slots) {
					krk_currentThread.stackTop = &krk_currentThread.stack[stackOffset + 1];
					krk_push(result);
					krk_swap(2);
					krk_swap(1);
					frame->ip = frame->closure->function->chunk.code + AS_HANDLER(krk_peek(0)).target;
					AS_HANDLER(krk_currentThread.stackTop[-1]).type = OP_RETURN;
					break;
				}
				krk_currentThread.frameCount--;
				if (krk_currentThread.frameCount == 0) {
					krk_pop();
					return result;
				}
				krk_currentThread.stackTop = &krk_currentThread.stack[frame->outSlots];
				if (krk_currentThread.frameCount == (size_t)krk_currentThread.exitOnFrame) {
					return result;
				}
				krk_push(result);
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
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
			case OP_GREATER: BINARY_OP(gt);
			case OP_ADD: BINARY_OP(add);
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
				else { krk_runtimeError(vm.exceptions->typeError, "Incompatible operand type for bit negation."); goto _finishException; }
				break;
			}
			case OP_NEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(-AS_INTEGER(value)));
				else if (IS_FLOATING(value)) krk_push(FLOATING_VAL(-AS_FLOATING(value)));
				else { krk_runtimeError(vm.exceptions->typeError, "Incompatible operand type for prefix negation."); goto _finishException; }
				break;
			}
			case OP_NONE:  krk_push(NONE_VAL()); break;
			case OP_TRUE:  krk_push(BOOLEAN_VAL(1)); break;
			case OP_FALSE: krk_push(BOOLEAN_VAL(0)); break;
			case OP_NOT:   krk_push(BOOLEAN_VAL(krk_isFalsey(krk_pop()))); break;
			case OP_POP:   krk_pop(); break;
			case OP_RAISE: {
				krk_currentThread.currentException = krk_pop();
				attachTraceback();
				krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
				goto _finishException;
			}
			/* This version of the call instruction takes its arity from the
			 * top of the stack, so we don't have to calculate arity at compile time. */
			case OP_CALL_STACK: {
				int argCount = AS_INTEGER(krk_pop());
				if (unlikely(!krk_callValue(krk_peek(argCount), argCount, 1))) goto _finishException;
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
				break;
			}
			case OP_CLOSE_UPVALUE:
				closeUpvalues((krk_currentThread.stackTop - krk_currentThread.stack)-1);
				krk_pop();
				break;
			case OP_INVOKE_GETTER: {
				KrkClass * type = krk_getType(krk_peek(1));
				if (likely(type->_getter)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_getter), 2, 0));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
				}
				break;
			}
			case OP_INVOKE_SETTER: {
				KrkClass * type = krk_getType(krk_peek(2));
				if (likely(type->_setter)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_setter), 3, 0));
				} else {
					if (type->_getter) {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not mutable", krk_typeName(krk_peek(2)));
					} else {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(2)));
					}
				}
				break;
			}
			case OP_INVOKE_GETSLICE: {
				KrkClass * type = krk_getType(krk_peek(2));
				if (likely(type->_getslice)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_getslice), 3, 0));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not sliceable", krk_typeName(krk_peek(2)));
				}
				break;
			}
			case OP_INVOKE_SETSLICE: {
				KrkClass * type = krk_getType(krk_peek(3));
				if (likely(type->_setslice)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_setslice), 4, 0));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not sliceable", krk_typeName(krk_peek(3)));
				}
				break;
			}
			case OP_INVOKE_DELSLICE: {
				KrkClass * type = krk_getType(krk_peek(2));
				if (likely(type->_delslice)) {
					krk_push(krk_callSimple(OBJECT_VAL(type->_delslice), 3, 0));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not sliceable", krk_typeName(krk_peek(2)));
				}
				break;
			}
			case OP_INVOKE_DELETE: {
				KrkClass * type = krk_getType(krk_peek(1));
				if (likely(type->_delitem)) {
					krk_callSimple(OBJECT_VAL(type->_delitem), 2, 0);
				} else {
					if (type->_getter) {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not mutable", krk_typeName(krk_peek(1)));
					} else {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
					}
				}
				break;
			}
			case OP_FINALIZE: {
				KrkClass * _class = AS_CLASS(krk_peek(0));
				/* Store special methods for quick access */
				krk_finalizeClass(_class);
				break;
			}
			case OP_INHERIT: {
				KrkValue superclass = krk_peek(1);
				if (unlikely(!IS_CLASS(superclass))) {
					krk_runtimeError(vm.exceptions->typeError, "Superclass must be a class, not '%s'",
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
			case OP_SWAP:
				krk_swap(1);
				break;
			case OP_CREATE_PROPERTY: {
				KrkProperty * newProperty = krk_newProperty(krk_peek(0));
				krk_pop();
				krk_push(OBJECT_VAL(newProperty));
				break;
			}
			case OP_FILTER_EXCEPT: {
				int isMatch = 0;
				if (IS_CLASS(krk_peek(0)) && krk_isInstanceOf(krk_peek(1), AS_CLASS(krk_peek(0)))) {
					isMatch = 1;
				} if (IS_TUPLE(krk_peek(0))) {
					for (size_t i = 0; i < AS_TUPLE(krk_peek(0))->values.count; ++i) {
						if (IS_CLASS(AS_TUPLE(krk_peek(0))->values.values[i]) && krk_isInstanceOf(krk_peek(1), AS_CLASS(AS_TUPLE(krk_peek(0))->values.values[i]))) {
							isMatch = 1;
							break;
						}
					}
				}
				if (!isMatch) {
					/* Restore and re-raise the exception if it didn't match. */
					krk_currentThread.currentException = krk_peek(1);
					krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
					goto _finishException;
				}
				/* Else pop the filter value */
				krk_pop();
				break;
			}

			/*
			 * Two-byte operands
			 */

			case OP_JUMP_IF_FALSE: {
				uint16_t offset = OPERAND;
				if (krk_isFalsey(krk_peek(0))) frame->ip += offset;
				break;
			}
			case OP_JUMP_IF_TRUE: {
				uint16_t offset = OPERAND;
				if (!krk_isFalsey(krk_peek(0))) frame->ip += offset;
				break;
			}
			case OP_JUMP: {
				frame->ip += OPERAND;
				break;
			}
			case OP_LOOP: {
				uint16_t offset = OPERAND;
				frame->ip -= offset;
				break;
			}
			case OP_PUSH_TRY: {
				uint16_t tryTarget = OPERAND + (frame->ip - frame->closure->function->chunk.code);
				KrkValue handler = HANDLER_VAL(OP_PUSH_TRY, tryTarget);
				krk_push(handler);
				break;
			}
			case OP_PUSH_WITH: {
				uint16_t cleanupTarget = OPERAND + (frame->ip - frame->closure->function->chunk.code);
				KrkValue contextManager = krk_peek(0);
				KrkClass * type = krk_getType(contextManager);
				if (unlikely(!type->_enter || !type->_exit)) {
					krk_runtimeError(vm.exceptions->attributeError, "Can not use '%s' as context manager", krk_typeName(contextManager));
					goto _finishException;
				}
				krk_push(contextManager);
				krk_callSimple(OBJECT_VAL(type->_enter), 1, 0);
				/* Ignore result; don't need to pop */
				KrkValue handler = HANDLER_VAL(OP_PUSH_WITH, cleanupTarget);
				krk_push(handler);
				break;
			}

			case OP_CONSTANT_LONG:
			case OP_CONSTANT: {
				size_t index = OPERAND;
				KrkValue constant = frame->closure->function->chunk.constants.values[index];
				krk_push(constant);
				break;
			}
			case OP_DEFINE_GLOBAL_LONG:
			case OP_DEFINE_GLOBAL: {
				KrkString * name = READ_STRING(OPERAND);
				krk_tableSet(frame->globals, OBJECT_VAL(name), krk_peek(0));
				krk_pop();
				break;
			}
			case OP_GET_GLOBAL_LONG:
			case OP_GET_GLOBAL: {
				KrkString * name = READ_STRING(OPERAND);
				KrkValue value;
				if (!krk_tableGet(frame->globals, OBJECT_VAL(name), &value)) {
					if (!krk_tableGet(&vm.builtins->fields, OBJECT_VAL(name), &value)) {
						krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%s'.", name->chars);
						goto _finishException;
					}
				}
				krk_push(value);
				break;
			}
			case OP_SET_GLOBAL_LONG:
			case OP_SET_GLOBAL: {
				KrkString * name = READ_STRING(OPERAND);
				if (krk_tableSet(frame->globals, OBJECT_VAL(name), krk_peek(0))) {
					krk_tableDelete(frame->globals, OBJECT_VAL(name));
					krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DEL_GLOBAL_LONG:
			case OP_DEL_GLOBAL: {
				KrkString * name = READ_STRING(OPERAND);
				if (!krk_tableDelete(frame->globals, OBJECT_VAL(name))) {
					krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_IMPORT_LONG:
			case OP_IMPORT: {
				KrkString * name = READ_STRING(OPERAND);
				if (!krk_doRecursiveModuleLoad(name)) {
					goto _finishException;
				}
				break;
			}
			case OP_GET_LOCAL_LONG:
			case OP_GET_LOCAL: {
				uint32_t slot = OPERAND;
				krk_push(krk_currentThread.stack[frame->slots + slot]);
				break;
			}
			case OP_SET_LOCAL_LONG:
			case OP_SET_LOCAL: {
				uint32_t slot = OPERAND;
				krk_currentThread.stack[frame->slots + slot] = krk_peek(0);
				break;
			}
			/* Sometimes you just want to increment a stack-local integer quickly. */
			case OP_INC_LONG:
			case OP_INC: {
				uint32_t slot = OPERAND;
				krk_currentThread.stack[frame->slots + slot] = INTEGER_VAL(AS_INTEGER(krk_currentThread.stack[frame->slots+slot])+1);
				break;
			}
			case OP_CALL_LONG:
			case OP_CALL: {
				int argCount = OPERAND;
				if (unlikely(!krk_callValue(krk_peek(argCount), argCount, 1))) goto _finishException;
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
				break;
			}
			/* EXPAND_ARGS_LONG? */
			case OP_EXPAND_ARGS: {
				int type = OPERAND;
				krk_push(KWARGS_VAL(LONG_MAX-type));
				break;
			}
			case OP_CLOSURE_LONG:
			case OP_CLOSURE: {
				KrkFunction * function = AS_FUNCTION(READ_CONSTANT(OPERAND));
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
				int slot = OPERAND;
				krk_push(*UPVALUE_LOCATION(frame->closure->upvalues[slot]));
				break;
			}
			case OP_SET_UPVALUE_LONG:
			case OP_SET_UPVALUE: {
				int slot = OPERAND;
				*UPVALUE_LOCATION(frame->closure->upvalues[slot]) = krk_peek(0);
				break;
			}
			case OP_CLASS_LONG:
			case OP_CLASS: {
				KrkString * name = READ_STRING(OPERAND);
				KrkClass * _class = krk_newClass(name, vm.baseClasses->objectClass);
				krk_push(OBJECT_VAL(_class));
				_class->filename = frame->closure->function->chunk.filename;
				krk_attachNamedObject(&_class->fields, "__func__", (KrkObj*)frame->closure);
				break;
			}
			case OP_IMPORT_FROM_LONG:
			case OP_IMPORT_FROM: {
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueGetProperty(name))) {
					/* Try to import... */
					KrkValue moduleName;
					if (!krk_tableGet(&AS_INSTANCE(krk_peek(0))->fields, vm.specialMethodNames[METHOD_NAME], &moduleName)) {
						krk_runtimeError(vm.exceptions->importError, "Can not import '%s' from non-module '%s' object", name->chars, krk_typeName(krk_peek(0)));
						goto _finishException;
					}
					krk_push(moduleName);
					krk_push(OBJECT_VAL(S(".")));
					krk_addObjects();
					krk_push(OBJECT_VAL(name));
					krk_addObjects();
					if (!krk_doRecursiveModuleLoad(AS_STRING(krk_peek(0)))) {
						krk_runtimeError(vm.exceptions->importError, "Can not import '%s' from '%s'", name->chars, AS_CSTRING(moduleName));
						goto _finishException;
					}
					krk_currentThread.stackTop[-3] = krk_currentThread.stackTop[-1];
					krk_currentThread.stackTop -= 2;
				}
			} break;
			case OP_GET_PROPERTY_LONG:
			case OP_GET_PROPERTY: {
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueGetProperty(name))) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DEL_PROPERTY_LONG:
			case OP_DEL_PROPERTY: {
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueDelProperty(name))) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_SET_PROPERTY_LONG:
			case OP_SET_PROPERTY: {
				KrkString * name = READ_STRING(OPERAND);
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
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
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
				KrkValue name = OBJECT_VAL(READ_STRING(OPERAND));
				krk_tableSet(&_class->methods, name, method);
				krk_pop();
				break;
			}
			case OP_GET_SUPER_LONG:
			case OP_GET_SUPER: {
				KrkString * name = READ_STRING(OPERAND);
				KrkClass * superclass = AS_CLASS(krk_pop());
				if (!krk_bindMethod(superclass, name)) {
					krk_runtimeError(vm.exceptions->attributeError, "super(%s) has no attribute '%s'",
						superclass->name->chars, name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DUP_LONG:
			case OP_DUP:
				krk_push(krk_peek(OPERAND));
				break;
			case OP_KWARGS_LONG:
			case OP_KWARGS: {
				krk_push(KWARGS_VAL(OPERAND));
				break;
			}
			case OP_TUPLE_LONG:
			case OP_TUPLE: {
				size_t count = OPERAND;
				krk_reserve_stack(4);
				KrkValue tuple = krk_tuple_of(count,&krk_currentThread.stackTop[-count],0);
				if (count) {
					krk_currentThread.stackTop[-count] = tuple;
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
				size_t count = OPERAND;
				KrkValue sequence = krk_peek(0);
				/* First figure out what it is and if we can unpack it. */
#define unpackArray(counter, indexer) do { \
					if (counter != count) { \
						krk_runtimeError(vm.exceptions->valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter); \
					} \
					for (size_t i = 1; i < counter; ++i) { \
						krk_push(indexer); \
					} \
					size_t i = 0; \
					krk_currentThread.stackTop[-count] = indexer; \
				} while (0)
				if (IS_TUPLE(sequence)) {
					unpackArray(AS_TUPLE(sequence)->values.count, AS_TUPLE(sequence)->values.values[i]);
				} else if (IS_INSTANCE(sequence) && AS_INSTANCE(sequence)->_class == vm.baseClasses->listClass) {
					unpackArray(AS_LIST(sequence)->count, AS_LIST(sequence)->values[i]);
				} else if (IS_INSTANCE(sequence) && AS_INSTANCE(sequence)->_class == vm.baseClasses->dictClass) {
					unpackArray(AS_DICT(sequence)->count, krk_dict_nth_key_fast(AS_DICT(sequence)->capacity, AS_DICT(sequence)->entries, i));
				} else if (IS_STRING(sequence)) {
					unpackArray(AS_STRING(sequence)->codesLength, krk_string_get(2,(KrkValue[]){sequence,INTEGER_VAL(i)},0));
				} else {
					KrkClass * type = krk_getType(sequence);
					if (!type->_iter) {
						krk_runtimeError(vm.exceptions->typeError, "Can not unpack non-iterable '%s'", krk_typeName(sequence));
						goto _finishException;
					} else {
						size_t stackStart = krk_currentThread.stackTop - krk_currentThread.stack - 1;
						size_t counter = 0;
						for (size_t i = 0; i < count-1; i++) {
							krk_push(NONE_VAL());
						}
						/* Create the iterator */
						krk_push(krk_currentThread.stack[stackStart]);
						krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

						do {
							/* Call it until it gives us itself */
							krk_push(krk_currentThread.stackTop[-1]);
							krk_push(krk_callSimple(krk_peek(0), 0, 1));
							if (krk_valuesSame(krk_currentThread.stackTop[-2], krk_currentThread.stackTop[-1])) {
								/* We're done. */
								krk_pop(); /* The result of iteration */
								krk_pop(); /* The iterator */
								if (counter != count) {
									krk_runtimeError(vm.exceptions->valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter);
									goto _finishException;
								}
								break;
							}
							if (counter == count) {
								krk_runtimeError(vm.exceptions->valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter);
								goto _finishException;
							}
							/* Rotate */
							krk_currentThread.stack[stackStart+counter] = krk_pop();
							counter++;
						} while (1);
					}
				}
#undef unpackArray
				break;
			}
		}
		if (likely(!(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION))) continue;
_finishException:
		if (!handleException()) {
			frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
			frame->ip = frame->closure->function->chunk.code + AS_HANDLER(krk_peek(0)).target;
			/* Replace the exception handler with the exception */
			krk_pop();
			krk_push(krk_currentThread.currentException);
			krk_currentThread.currentException = NONE_VAL();
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
	size_t oldExit = krk_currentThread.exitOnFrame;
	krk_currentThread.exitOnFrame = krk_currentThread.frameCount - 1;
	KrkValue result = run();
	krk_currentThread.exitOnFrame = oldExit;
	return result;
}

KrkInstance * krk_startModule(const char * name) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_currentThread.module = module;
	krk_attachNamedObject(&module->fields, "__builtins__", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)krk_copyString(name,strlen(name)));
	return module;
}

KrkValue krk_interpret(const char * src, int newScope, char * fromName, char * fromFile) {
	KrkFunction * function = krk_compile(src, 0, fromFile);
	if (!function) {
		if (!krk_currentThread.frameCount) handleException();
		return NONE_VAL();
	}

	krk_push(OBJECT_VAL(function));
	krk_attachNamedObject(&krk_currentThread.module->fields, "__file__", (KrkObj*)function->chunk.filename);

	function->name = krk_copyString(fromName, strlen(fromName));

	KrkClosure * closure = krk_newClosure(function);
	krk_pop();

	krk_push(OBJECT_VAL(closure));
	if (!newScope) {
		/* Quick little kludge so that empty statements return None from REPLs */
		krk_push(NONE_VAL());
		krk_pop();
	}
	krk_callValue(OBJECT_VAL(closure), 0, 1);

	KrkValue result = run();

	if (newScope) {
		return OBJECT_VAL(krk_currentThread.module);
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

KrkValue krk_callfile(const char * fileName, char * fromName, char * fromFile) {
	int previousExitFrame = krk_currentThread.exitOnFrame;
	krk_currentThread.exitOnFrame = krk_currentThread.frameCount;
	KrkValue out = krk_runfile(fileName, 1, fromName, fromFile);
	krk_currentThread.exitOnFrame = previousExitFrame;
	return out;
}

