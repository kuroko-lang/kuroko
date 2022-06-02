#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include <kuroko/vm.h>
#include <kuroko/debug.h>
#include <kuroko/memory.h>
#include <kuroko/compiler.h>
#include <kuroko/object.h>
#include <kuroko/table.h>
#include <kuroko/util.h>

#include "private.h"

#define KRK_VERSION_MAJOR  "1"
#define KRK_VERSION_MINOR  "3"
#define KRK_VERSION_PATCH  "0"

#define KRK_VERSION_EXTRA_BASE  "-alpha"

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

#if defined(ENABLE_THREADING) && defined(__APPLE__) && defined(__aarch64__)
/**
 * I have not checked how this works on x86-64, so we only do this
 * on M1 Macs at the moment, but TLS is disastrously poorly implemented
 * with a function pointer thunk, provided by dyld. This is very slow.
 * We can emulate the behavior inline, and achieve better performance -
 * much closer to the direct access case, though not as good as a dedicated
 * register - but in order for that to work we have to do at least one
 * traditional access to trigger the "bootstrap" and ensure our thread
 * state is actually allocated. We do that here, called by @c krk_initVM
 * as well as by @c _startthread, and check that our approach yields
 * the same address.
 */
void krk_forceThreadData(void) {
	krk_currentThread.next = NULL;
	assert(&krk_currentThread == _macos_currentThread());
}
/**
 * And then we macro away @c krk_currentThread behind the inlinable emulation
 * which is defined in src/kuroko/vm.h so the rest of the interpreter can do
 * the same - not strictly necessary, just doing it locally here is enough
 * for major performance gains, but it's nice to be consistent.
 */
#define krk_currentThread (*_macos_currentThread())
#endif

#if !defined(KRK_NO_TRACING) && !defined(__EMSCRIPTEN__)
# define FRAME_IN(frame) if (vm.globalFlags & KRK_GLOBAL_CALLGRIND) { clock_gettime(CLOCK_MONOTONIC, &frame->in_time); }
# define FRAME_OUT(frame) \
	if (vm.globalFlags & KRK_GLOBAL_CALLGRIND && !(frame->closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR)) { \
		KrkCallFrame * caller = krk_currentThread.frameCount > 1 ? &krk_currentThread.frames[krk_currentThread.frameCount-2] : NULL; \
		struct timespec outTime; \
		clock_gettime(CLOCK_MONOTONIC, &outTime); \
		struct timespec diff; \
		diff.tv_sec  = outTime.tv_sec  - frame->in_time.tv_sec; \
		diff.tv_nsec = outTime.tv_nsec - frame->in_time.tv_nsec; \
		if (diff.tv_nsec < 0) { diff.tv_sec--; diff.tv_nsec += 1000000000L; } \
		fprintf(vm.callgrindFile, "%s %s@%p %d %s %s@%p %d %lld.%.9ld\n", \
			caller ? (caller->closure->function->chunk.filename->chars) : "stdin", \
			caller ? (caller->closure->function->qualname ? caller->closure->function->qualname->chars : caller->closure->function->name->chars) : "(root)", \
			caller ? ((void*)caller->closure->function) : NULL, \
			caller ? ((int)krk_lineNumber(&caller->closure->function->chunk, caller->ip - caller->closure->function->chunk.code)) : 1, \
			frame->closure->function->chunk.filename->chars, \
			frame->closure->function->qualname ? frame->closure->function->qualname->chars : frame->closure->function->name->chars, \
			(void*)frame->closure->function, \
			(int)krk_lineNumber(&frame->closure->function->chunk, 0), \
			(long long)diff.tv_sec, diff.tv_nsec); \
	}
#else
# define FRAME_IN(frame)
# define FRAME_OUT(frame)
#endif

/*
 * In some threading configurations, particular on Windows,
 * we can't have executables reference our thread-local thread
 * state object directly; in order to provide a consistent API
 * we make @ref krk_currentThread a macro outside of the core
 * sources that will call this function.
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
void krk_resetStack(void) {
	krk_currentThread.stackTop = krk_currentThread.stack;
	krk_currentThread.stackMax = krk_currentThread.stack + krk_currentThread.stackSize;
	krk_currentThread.frameCount = 0;
	krk_currentThread.openUpvalues = NULL;
	krk_currentThread.flags &= ~KRK_THREAD_HAS_EXCEPTION;
	krk_currentThread.currentException = NONE_VAL();
}

/**
 * Display a traceback by scanning up the stack / call frames.
 * The format of the output here is modeled after the output
 * given by CPython, so we display the outermost call first
 * and then move inwards; on each call frame we try to open
 * the source file and print the corresponding line.
 */
void krk_dumpTraceback(void) {
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
				KrkCodeObject * function = closure->function;
				size_t instruction = AS_INTEGER(entry->values.values[1]);

				/* Calculate the line number */
				int lineNo = (int)krk_lineNumber(&function->chunk, instruction);

				/* Print the simple stuff that we already know */
				fprintf(stderr, "  File \"%s\", line %d, in %s\n",
					(function->chunk.filename ? function->chunk.filename->chars : "?"),
					lineNo,
					(function->name ? function->name->chars : "(unnamed)"));

#ifndef NO_SOURCE_IN_TRACEBACK
				/* Try to open the file */
				if (function->chunk.filename) {
					FILE * f = fopen(function->chunk.filename->chars, "r");
					if (f) {
						int line = 1;
						do {
							int c = fgetc(f);
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
#endif
			}
		}

		/* Is this a SyntaxError? Handle those specially. */
		if (krk_isInstanceOf(krk_currentThread.currentException, vm.exceptions->syntaxError)) {
			KrkValue result = krk_callDirect(krk_getType(krk_currentThread.currentException)->_tostr, 1);
			fprintf(stderr, "%s\n", AS_CSTRING(result));
			return;
		}
		/* Clear the exception state while printing the exception. */
		krk_currentThread.flags &= ~(KRK_THREAD_HAS_EXCEPTION);
		fprintf(stderr, "%s", krk_typeName(krk_currentThread.currentException));
		KrkValue result = krk_callDirect(krk_getType(krk_currentThread.currentException)->_tostr, 1);
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

void krk_growStack(void) {
	size_t old = krk_currentThread.stackSize;
	size_t old_offset = krk_currentThread.stackTop - krk_currentThread.stack;
	size_t newsize = GROW_CAPACITY(old);
	if (krk_currentThread.flags & KRK_THREAD_DEFER_STACK_FREE) {
		KrkValue * newStack = GROW_ARRAY(KrkValue, NULL, 0, newsize);
		memcpy(newStack, krk_currentThread.stack, sizeof(KrkValue) * old);
		krk_currentThread.stack = newStack;
		krk_currentThread.flags &= ~(KRK_THREAD_DEFER_STACK_FREE);
	} else {
		krk_currentThread.stack = GROW_ARRAY(KrkValue, krk_currentThread.stack, old, newsize);
	}
	krk_currentThread.stackSize = newsize;
	krk_currentThread.stackTop = krk_currentThread.stack + old_offset;
	krk_currentThread.stackMax = krk_currentThread.stack + krk_currentThread.stackSize;
}

/**
 * Push a value onto the stack, and grow the stack if necessary.
 * Note that growing the stack can involve the stack _moving_, so
 * do not rely on the memory offset of a stack value if you expect
 * the stack to grow - eg. if you are calling into managed code
 * to do anything, or if you are pushing anything.
 */
inline void krk_push(KrkValue value) {
	if (unlikely(krk_currentThread.stackTop == krk_currentThread.stackMax)) krk_growStack();
	*krk_currentThread.stackTop++ = value;
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
	if (unlikely(krk_currentThread.stackTop == krk_currentThread.stack)) {
		abort();
	}
	return *--krk_currentThread.stackTop;
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
	KrkNative * func = krk_newNative(function, name, 0);
	krk_attachNamedObject(table, name, (KrkObj*)func);
	return func;
}

/**
 * Create a new property object that calls a C function; same semantics as defineNative, but
 * instead of applying the function directly it is applied as a property value, so it should
 * be used with the "fields" table rather than the methods table. This will eventually replace
 * the ":field" option for defineNative().
 */
KrkNative * krk_defineNativeProperty(KrkTable * table, const char * name, NativeFn function) {
	KrkNative * func = krk_newNative(function, name, 0);
	krk_push(OBJECT_VAL(func));
	KrkInstance * property = krk_newInstance(vm.baseClasses->propertyClass);
	krk_attachNamedObject(table, name, (KrkObj*)property);
	krk_attachNamedObject(&property->fields, "fget", (KrkObj*)func);
	krk_pop();
	return func;
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
		/* Now give it a __module__ */
		KrkValue moduleName = NONE_VAL();
		krk_tableGet(&module->fields, OBJECT_VAL(S("__name__")), &moduleName);
		krk_attachNamedValue(&(*_class)->methods,"__module__",moduleName);
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
__attribute__((nonnull))
void krk_finalizeClass(KrkClass * _class) {
	KrkValue tmp;

	struct TypeMap {
		KrkObj ** method;
		KrkSpecialMethods index;
	};
	struct TypeMap specials[] = {
	#define CACHED_METHOD(a,b,c) {&_class-> c, METHOD_ ## a},
	#define SPECIAL_ATTRS(a,b)
	#include "methods.h"
	#undef CACHED_METHOD
	#undef SPECIAL_ATTRS
		{NULL, 0},
	};

	for (struct TypeMap * entry = specials; entry->method; ++entry) {
		*entry->method = NULL;
		KrkClass * _base = _class;
		while (_base) {
			if (krk_tableGet(&_base->methods, vm.specialMethodNames[entry->index], &tmp)) break;
			_base = _base->base;
		}
		if (_base && (IS_CLOSURE(tmp) || IS_NATIVE(tmp))) {
			*entry->method = AS_OBJECT(tmp);
		}
	}

	if (_class->base && _class->_eq != _class->base->_eq) {
		if (_class->_hash == _class->base->_hash) {
			_class->_hash = NULL;
		}
	}

	for (size_t i = 0; i < _class->subclasses.capacity; ++i) {
		KrkTableEntry * entry = &_class->subclasses.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		krk_finalizeClass(AS_CLASS(entry->key));
	}
}

/**
 * Maps values to their base classes.
 * Internal version of type().
 */
inline KrkClass * krk_getType(KrkValue of) {

	static size_t objClasses[] = {
		[KRK_OBJ_CODEOBJECT]   = offsetof(struct BaseClasses, codeobjectClass),
		[KRK_OBJ_NATIVE]       = offsetof(struct BaseClasses, functionClass),
		[KRK_OBJ_CLOSURE]      = offsetof(struct BaseClasses, functionClass),
		[KRK_OBJ_BOUND_METHOD] = offsetof(struct BaseClasses, methodClass),
		[KRK_OBJ_STRING]       = offsetof(struct BaseClasses, strClass),
		[KRK_OBJ_UPVALUE]      = offsetof(struct BaseClasses, objectClass),
		[KRK_OBJ_CLASS]        = offsetof(struct BaseClasses, typeClass),
		[KRK_OBJ_TUPLE]        = offsetof(struct BaseClasses, tupleClass),
		[KRK_OBJ_BYTES]        = offsetof(struct BaseClasses, bytesClass),
		[KRK_OBJ_INSTANCE]     = 0,
	};

	switch (KRK_VAL_TYPE(of)) {
		case KRK_VAL_INTEGER:
			return vm.baseClasses->intClass;
		case KRK_VAL_BOOLEAN:
			return vm.baseClasses->boolClass;
		case KRK_VAL_NONE:
			return vm.baseClasses->noneTypeClass;
		case KRK_VAL_NOTIMPL:
			return vm.baseClasses->notImplClass;
		case KRK_VAL_OBJECT:
			if (IS_INSTANCE(of)) return AS_INSTANCE(of)->_class;
			return *(KrkClass **)((char*)vm.baseClasses + objClasses[AS_OBJECT(of)->type]);
		default:
			if (IS_FLOATING(of)) return vm.baseClasses->floatClass;
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
int krk_isInstanceOf(KrkValue obj, const KrkClass * type) {
	KrkClass * mine = krk_getType(obj);
	while (mine) {
		if (mine == type) return 1;
		mine = mine->base;
	}
	return 0;
}

static inline int checkArgumentCount(const KrkClosure * closure, int argCount) {
	int minArgs = closure->function->requiredArgs;
	int maxArgs = minArgs + closure->function->keywordArgs;
	if (unlikely(argCount < minArgs || argCount > maxArgs)) {
		krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)",
		closure->function->name ? closure->function->name->chars : "<unnamed>",
		(minArgs == maxArgs) ? "exactly" : (argCount < minArgs ? "at least" : "at most"),
		(argCount < minArgs) ? minArgs : maxArgs,
		((argCount < minArgs) ? minArgs : maxArgs) == 1 ? "" : "s",
		argCount);
		return 0;
	}
	return 1;
}

static void multipleDefs(const KrkClosure * closure, int destination) {
	krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%s'",
		closure->function->name ? closure->function->name->chars : "<unnamed>",
		(destination < closure->function->requiredArgs ? AS_CSTRING(closure->function->requiredArgNames.values[destination]) :
			(destination - closure->function->requiredArgs < closure->function->keywordArgs ? AS_CSTRING(closure->function->keywordArgNames.values[destination - closure->function->requiredArgs]) :
				"<unnamed>")));
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
int krk_processComplexArguments(int argCount, KrkValueArray * positionals, KrkTable * keywords, const char * name) {
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
			if (AS_INTEGER(key) == KWARGS_LIST) { /* unpack list */
				unpackIterableFast(value);
			} else if (AS_INTEGER(key) == KWARGS_DICT) { /* unpack dict */
				if (!IS_dict(value)) {
					krk_runtimeError(vm.exceptions->typeError, "%s(): **expression value is not a dict.", name);
					return 0;
				}
				for (size_t i = 0; i < AS_DICT(value)->capacity; ++i) {
					KrkTableEntry * entry = &AS_DICT(value)->entries[i];
					if (!IS_KWARGS(entry->key)) {
						if (!IS_STRING(entry->key)) {
							krk_runtimeError(vm.exceptions->typeError, "%s(): **expression contains non-string key", name);
							return 0;
						}
						if (!krk_tableSet(keywords, entry->key, entry->value)) {
							krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%s'", name, AS_CSTRING(entry->key));
							return 0;
						}
					}
				}
			} else if (AS_INTEGER(key) == KWARGS_SINGLE) { /* single value */
				krk_writeValueArray(positionals, value);
			}
		} else if (IS_STRING(key)) {
			if (!krk_tableSet(keywords, key, value)) {
				krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%s'", name, AS_CSTRING(key));
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
static int call(KrkClosure * closure, int argCount, int returnDepth) {
	size_t potentialPositionalArgs = closure->function->requiredArgs + closure->function->keywordArgs;
	size_t totalArguments = closure->function->requiredArgs + closure->function->keywordArgs + !!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) + !!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS);
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
		if (!krk_processComplexArguments(argCount, positionals, keywords, closure->function->name ? closure->function->name->chars : "<unnamed>")) goto _errorDuringPositionals;
		argCount--; /* It popped the KWARGS value from the top, so we have one less argument */

		/* Do we already know we have too many arguments? Let's bail before doing a bunch of work. */
		if ((positionals->count > potentialPositionalArgs) && !(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS)) {
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

		if (closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
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
			if (!IS_KWARGS(entry->key)) {
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
				if (!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS)) {
					krk_runtimeError(vm.exceptions->typeError, "%s() got an unexpected keyword argument '%s'",
						closure->function->name ? closure->function->name->chars : "<unnamed>",
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
		if (closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
			krk_push(krk_dict_of(0,NULL,0));
			argCount++;
			krk_tableAddAll(keywords, AS_DICT(krk_peek(0)));
		}

		krk_freeTable(keywords);
		krk_currentThread.scratchSpace[1] = NONE_VAL();

		for (size_t i = 0; i < (size_t)closure->function->requiredArgs; ++i) {
			if (IS_KWARGS(krk_currentThread.stackTop[-argCount + i])) {
				krk_runtimeError(vm.exceptions->typeError, "%s() missing required positional argument: '%s'",
					closure->function->name ? closure->function->name->chars : "<unnamed>",
					AS_CSTRING(closure->function->requiredArgNames.values[i]));
				goto _errorAfterKeywords;
			}
		}

		argCountX = argCount - (!!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) + !!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS));
	} else if ((size_t)argCount > potentialPositionalArgs && (closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS)) {
		krk_push(NONE_VAL()); krk_push(NONE_VAL()); krk_pop(); krk_pop();
		KrkValue * startOfPositionals = &krk_currentThread.stackTop[-argCount];
		KrkValue tmp = krk_list_of(argCount - potentialPositionalArgs,
			&startOfPositionals[potentialPositionalArgs], 0);
		startOfPositionals = &krk_currentThread.stackTop[-argCount];
		startOfPositionals[offsetOfExtraArgs] = tmp;
		argCount = closure->function->requiredArgs + 1;
		argCountX = argCount - 1;
		while (krk_currentThread.stackTop > startOfPositionals + argCount) krk_pop();
	}

	if (unlikely(!checkArgumentCount(closure, argCountX))) goto _errorAfterKeywords;

	while (argCount < (int)totalArguments) {
		krk_push(KWARGS_VAL(0));
		argCount++;
	}

	if (unlikely(closure->function->obj.flags & (KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR | KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE))) {
		KrkInstance * gen = krk_buildGenerator(closure, krk_currentThread.stackTop - argCount, argCount);
		krk_currentThread.stackTop = krk_currentThread.stackTop - argCount - returnDepth;
		krk_push(OBJECT_VAL(gen));
		return 2;
	}

	if (unlikely(krk_currentThread.frameCount == vm.maximumCallDepth)) {
		krk_runtimeError(vm.exceptions->baseException, "maximum recursion depth exceeded");
		goto _errorAfterKeywords;
	}

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = (krk_currentThread.stackTop - argCount) - krk_currentThread.stack;
	frame->outSlots = (krk_currentThread.stackTop - argCount - returnDepth) - krk_currentThread.stack;
	frame->globals = &closure->function->globalsContext->fields;
	FRAME_IN(frame);
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
 * Make a call to a native function using values on the stack without moving them.
 * If the stack is reallocated within this call, the old stack will not be freed until
 * all such nested calls through krk_callNativeOnStack have returned.
 */
inline KrkValue krk_callNativeOnStack(size_t argCount, const KrkValue *stackArgs, int hasKw, NativeFn native) {

	/**
	 * If someone is already preserving this stack, we can just call directly.
	 */
	if (unlikely(krk_currentThread.flags & KRK_THREAD_DEFER_STACK_FREE)) {
		return native(argCount, stackArgs, hasKw);
	}

	/* Mark the thread's stack as preserved. */
	krk_currentThread.flags |= KRK_THREAD_DEFER_STACK_FREE;
	void * stackBefore = krk_currentThread.stack;
	size_t sizeBefore  = krk_currentThread.stackSize;
	KrkValue result = native(argCount, stackArgs, hasKw);

	if (unlikely(krk_currentThread.stack != stackBefore)) {
		FREE_ARRAY(KrkValue, stackBefore, sizeBefore);
	}

	krk_currentThread.flags &= ~(KRK_THREAD_DEFER_STACK_FREE);
	return result;
}

/**
 * Sometimes we might call something with a bound receiver, which in most circumstances
 * can replace the called object on the stack, but sometimes we don't _have_ the called
 * object on the stack - only its arguments. If that is the case, we unfortunately need
 * to rotate the stack so we can inject the implicit bound argument at the front.
 */
static void _rotate(size_t argCount) {
	krk_push(NONE_VAL());
	memmove(&krk_currentThread.stackTop[-argCount],&krk_currentThread.stackTop[-argCount-1],sizeof(KrkValue) * argCount);
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
int krk_callValue(KrkValue callee, int argCount, int returnDepth) {
	if (likely(IS_OBJECT(callee))) {
		_innerObject:
		switch (OBJECT_TYPE(callee)) {
			case KRK_OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount, returnDepth);
			case KRK_OBJ_NATIVE: {
				NativeFn native = (NativeFn)AS_NATIVE(callee)->function;
				if (unlikely(argCount && IS_KWARGS(krk_currentThread.stackTop[-1]))) {
					KrkValue myList = krk_list_of(0,NULL,0);
					krk_currentThread.scratchSpace[0] = myList;
					KrkValue myDict = krk_dict_of(0,NULL,0);
					krk_currentThread.scratchSpace[1] = myDict;
					if (unlikely(!krk_processComplexArguments(argCount, AS_LIST(myList), AS_DICT(myDict), AS_NATIVE(callee)->name))) {
						return 0;
					}
					argCount--; /* Because that popped the kwargs value */
					krk_currentThread.stackTop -= argCount + returnDepth; /* We can just put the stack back to normal */
					krk_push(myList);
					krk_push(myDict);
					krk_currentThread.scratchSpace[0] = NONE_VAL();
					krk_currentThread.scratchSpace[1] = NONE_VAL();
					krk_writeValueArray(AS_LIST(myList), myDict);
					KrkValue result = native(AS_LIST(myList)->count-1, AS_LIST(myList)->values, 1);
					if (unlikely(krk_currentThread.stackTop == krk_currentThread.stack)) return 0;
					krk_pop();
					krk_pop();
					krk_push(result);
				} else {
					KrkValue result = krk_callNativeOnStack(argCount, krk_currentThread.stackTop - argCount, 0, native);
					if (unlikely(krk_currentThread.stackTop == krk_currentThread.stack)) return 0;
					krk_currentThread.stackTop -= argCount + returnDepth;
					krk_push(result);
				}
				return 2;
			}
			case KRK_OBJ_INSTANCE: {
				KrkClass * _class = AS_INSTANCE(callee)->_class;
				if (likely(_class->_call != NULL)) {
					if (unlikely(returnDepth == 0)) _rotate(argCount);
					krk_currentThread.stackTop[-argCount - 1] = callee;
					callee = OBJECT_VAL(_class->_call);
					argCount++;
					returnDepth = returnDepth ? (returnDepth - 1) : 0;
					goto _innerObject;
				} else {
					krk_runtimeError(vm.exceptions->typeError, "'%s' object is not callable", krk_typeName(callee));
					return 0;
				}
			}
			case KRK_OBJ_CLASS: {
				KrkClass * _class = AS_CLASS(callee);
				KrkInstance * newInstance = krk_newInstance(_class);
				if (likely(_class->_init != NULL)) {
					if (unlikely(returnDepth == 0)) _rotate(argCount);
					krk_currentThread.stackTop[-argCount - 1] = OBJECT_VAL(newInstance);
					callee = OBJECT_VAL(_class->_init);
					argCount++;
					returnDepth = returnDepth ? (returnDepth - 1) : 0;
					goto _innerObject;
				} else if (unlikely(argCount != 0)) {
					krk_runtimeError(vm.exceptions->typeError, "%s() takes no arguments (%d given)",
						_class->name->chars, argCount);
					return 0;
				}
				krk_currentThread.stackTop -= argCount + returnDepth;
				krk_push(OBJECT_VAL(newInstance));
				return 2;
			}
			case KRK_OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				if (unlikely(!bound->method)) {
					krk_runtimeError(vm.exceptions->argumentError, "???");
					return 0;
				}
				if (unlikely(returnDepth == 0)) _rotate(argCount);
				krk_currentThread.stackTop[-argCount - 1] = bound->receiver;
				callee = OBJECT_VAL(bound->method);
				argCount++;
				returnDepth = returnDepth ? (returnDepth - 1) : 0;
				goto _innerObject;
			}
			default:
				break;
		}
	}
	krk_runtimeError(vm.exceptions->typeError, "'%s' object is not callable", krk_typeName(callee));
	return 0;
}

/**
 * Takes care of runnext/pop
 */
KrkValue krk_callStack(int argCount) {
	switch (krk_callValue(krk_peek(argCount), argCount, 1)) {
		case 2: return krk_pop();
		case 1: return krk_runNext();
		default: return NONE_VAL();
	}
}

KrkValue krk_callDirect(KrkObj * callable, int argCount) {
	if (unlikely(callable->type != KRK_OBJ_CLOSURE && callable->type != KRK_OBJ_NATIVE)) {
		return krk_runtimeError(vm.exceptions->typeError, "'%s' is not a function.", krk_typeName(OBJECT_VAL(callable)));
	}
	switch (krk_callValue(OBJECT_VAL(callable), argCount, 0)) {
		case 2: return krk_pop();
		case 1: return krk_runNext();
		default: return NONE_VAL();
	}
}

/**
 * Attach a method call to its callee and return a BoundMethod.
 * Works for managed and native method calls.
 */
int krk_bindMethod(KrkClass * _class, KrkString * name) {
	KrkClass * originalClass = _class;
	KrkValue method, out;
	while (_class) {
		if (krk_tableGet_fast(&_class->methods, name, &method)) break;
		_class = _class->base;
	}
	if (!_class) return 0;
	if (IS_NATIVE(method)||IS_CLOSURE(method)) {
		if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD) {
			out = OBJECT_VAL(krk_newBoundMethod(OBJECT_VAL(originalClass), AS_OBJECT(method)));
		} else if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD) {
			out = method;
		} else if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_DYNAMIC_PROPERTY) {
			out = AS_NATIVE(method)->function(1, (KrkValue[]){krk_peek(0)}, 0);
		} else {
			out = OBJECT_VAL(krk_newBoundMethod(krk_peek(0), AS_OBJECT(method)));
		}
	} else {
		/* Does it have a descriptor __get__? */
		KrkClass * type = krk_getType(method);
		if (type->_descget) {
			krk_push(method);
			krk_swap(1);
			krk_push(krk_callDirect(type->_descget, 2));
			return 1;
		}
		out = method;
	}
	krk_pop();
	krk_push(out);
	return 1;
}

/**
 * Similar to @ref krk_bindMethod but does not create bound method objects.
 * @returns 1 if the result was an (unbound) method, 2 if it was something else, 0 if it was not found.
 */
int krk_unbindMethod(KrkClass * _class, KrkString * name) {
	KrkClass * originalClass = _class;
	KrkValue method, out;
	while (_class) {
		if (krk_tableGet_fast(&_class->methods, name, &method)) break;
		_class = _class->base;
	}
	if (!_class) return 0;
	if (IS_NATIVE(method) || IS_CLOSURE(method)) {
		if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_DYNAMIC_PROPERTY) {
			out = AS_NATIVE(method)->function(1, (KrkValue[]){krk_peek(0)}, 0);
		} else if (AS_CLOSURE(method)->obj.flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD) {
			krk_pop();
			krk_push(OBJECT_VAL(originalClass));
			krk_push(method);
			return 1;
		} else if (AS_CLOSURE(method)->obj.flags & KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD) {
			out = method;
		} else {
			krk_push(method);
			return 1;
		}
	} else {
		/* Does it have a descriptor __get__? */
		KrkClass * type = krk_getType(method);
		if (type->_descget) {
			krk_push(krk_peek(0));
			krk_push(method);
			krk_swap(1);
			krk_push(krk_callDirect(type->_descget, 2));
			return 2;
		}
		out = method;
	}
	krk_push(out);
	return 2;
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
 * Attach an object to a table.
 *
 * Generally used to attach classes or objects to the globals table, or to
 * a native module's export object.
 */
void krk_attachNamedObject(KrkTable * table, const char name[], KrkObj * obj) {
	krk_attachNamedValue(table,name,OBJECT_VAL(obj));
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
	switch (KRK_VAL_TYPE(value)) {
		case KRK_VAL_NONE: return 1;
		case KRK_VAL_BOOLEAN: return !AS_BOOLEAN(value);
		case KRK_VAL_INTEGER: return !AS_INTEGER(value);
		case KRK_VAL_NOTIMPL: return 1;
		case KRK_VAL_OBJECT: {
			switch (AS_OBJECT(value)->type) {
				case KRK_OBJ_STRING: return !AS_STRING(value)->codesLength;
				case KRK_OBJ_TUPLE: return !AS_TUPLE(value)->values.count;
				default: break;
			}
			break;
		}
		default:
			if (IS_FLOATING(value)) return !AS_FLOATING(value);
			break;
	}
	KrkClass * type = krk_getType(value);

	/* If it has a length, and that length is 0, it's Falsey */
	if (type->_len) {
		krk_push(value);
		KrkValue result = krk_callDirect(type->_len,1);
		return !AS_INTEGER(result);
	}
	return 0; /* Assume anything else is truthy */
}

#ifndef KRK_DISABLE_DEBUG
KRK_FUNC(set_tracing,{
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
})
#else
KRK_FUNC(set_tracing,{
	return krk_runtimeError(vm.exceptions->typeError,"Debugging is not enabled in this build.");
})
#endif

KRK_FUNC(getsizeof,{
	if (argc < 1 || !IS_OBJECT(argv[0])) return INTEGER_VAL(0);
	size_t mySize = 0;
	switch (AS_OBJECT(argv[0])->type) {
		case KRK_OBJ_STRING: {
			KrkString * self = AS_STRING(argv[0]);
			mySize += sizeof(KrkString) + self->length + 1; /* For the UTF8 */
			if (self->codes && self->chars != self->codes) {
				if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) <= KRK_OBJ_FLAGS_STRING_UCS1) mySize += self->codesLength;
				else if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS2) mySize += 2 * self->codesLength;
				else if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS4) mySize += 4 * self->codesLength;
			}
			break;
		}
		case KRK_OBJ_CODEOBJECT: {
			KrkCodeObject * self = (KrkCodeObject*)AS_OBJECT(argv[0]);
			mySize += sizeof(KrkCodeObject);
			/* Chunk size */
			mySize += sizeof(uint8_t) * self->chunk.capacity;
			mySize += sizeof(KrkLineMap) * self->chunk.linesCapacity;
			mySize += sizeof(KrkValue) * self->chunk.constants.capacity;
			/* requiredArgNames */
			mySize += sizeof(KrkValue) * self->requiredArgNames.capacity;
			/* keywordArgNames */
			mySize += sizeof(KrkValue) * self->keywordArgNames.capacity;
			/* Locals array */
			mySize += sizeof(KrkLocalEntry) * self->localNameCount;
			break;
		}
		case KRK_OBJ_NATIVE: {
			KrkNative * self = (KrkNative*)AS_OBJECT(argv[0]);
			mySize += sizeof(KrkNative) + strlen(self->name) + 1;
			break;
		}
		case KRK_OBJ_CLOSURE: {
			KrkClosure * self = AS_CLOSURE(argv[0]);
			mySize += sizeof(KrkClosure) + sizeof(KrkUpvalue*) * self->function->upvalueCount;
			break;
		}
		case KRK_OBJ_UPVALUE: {
			/* It should not be possible for an upvalue to be an argument to getsizeof,
			 * but for the sake of completeness, we'll include it here... */
			mySize += sizeof(KrkUpvalue);
			break;
		}
		case KRK_OBJ_CLASS: {
			KrkClass * self = AS_CLASS(argv[0]);
			mySize += sizeof(KrkClass);
			mySize += sizeof(KrkTableEntry) * self->methods.capacity;
			mySize += sizeof(KrkTableEntry) * self->subclasses.capacity;
			break;
		}
		case KRK_OBJ_INSTANCE: {
			KrkInstance * self = AS_INSTANCE(argv[0]);
			mySize += sizeof(KrkTableEntry) * self->fields.capacity;
			KrkClass * type = krk_getType(argv[0]);
			mySize += type->allocSize; /* All instance types have an allocSize set */

			/* TODO __sizeof__ */
			if (krk_isInstanceOf(argv[0], vm.baseClasses->listClass)) {
				mySize += sizeof(KrkValue) * AS_LIST(argv[0])->capacity;
			} else if (krk_isInstanceOf(argv[0], vm.baseClasses->dictClass)) {
				mySize += sizeof(KrkTableEntry) * AS_DICT(argv[0])->capacity;
			}
			break;
		}
		case KRK_OBJ_BOUND_METHOD: {
			mySize += sizeof(KrkBoundMethod);
			break;
		}
		case KRK_OBJ_TUPLE: {
			KrkTuple * self = AS_TUPLE(argv[0]);
			mySize += sizeof(KrkTuple) + sizeof(KrkValue) * self->values.capacity;
			break;
		}
		case KRK_OBJ_BYTES: {
			KrkBytes * self = AS_BYTES(argv[0]);
			mySize += sizeof(KrkBytes) + self->length;
			break;
		}
		default: break;
	}
	return INTEGER_VAL(mySize);
})

KRK_FUNC(set_clean_output,{
	if (!argc || (IS_BOOLEAN(argv[0]) && AS_BOOLEAN(argv[0]))) {
		vm.globalFlags |= KRK_GLOBAL_CLEAN_OUTPUT;
	} else {
		vm.globalFlags &= ~KRK_GLOBAL_CLEAN_OUTPUT;
	}
	return NONE_VAL();
})

KRK_FUNC(importmodule,{
	FUNCTION_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[0])) return TYPE_ERROR(str,argv[0]);
	if (!krk_doRecursiveModuleLoad(AS_STRING(argv[0]))) return NONE_VAL(); /* ImportError already raised */
	return krk_pop();
})

KRK_FUNC(modules,{
	FUNCTION_TAKES_NONE();
	KrkValue moduleList = krk_list_of(0,NULL,0);
	krk_push(moduleList);
	for (size_t i = 0; i < vm.modules.capacity; ++i) {
		KrkTableEntry * entry = &vm.modules.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		krk_writeValueArray(AS_LIST(moduleList), entry->key);
	}
	return krk_pop();
})

KRK_FUNC(unload,{
	FUNCTION_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[0])) return TYPE_ERROR(str,argv[0]);
	if (!krk_tableDelete(&vm.modules, argv[0])) {
		return krk_runtimeError(vm.exceptions->keyError, "Module is not loaded.");
	}
})

KRK_FUNC(inspect_value,{
	FUNCTION_TAKES_EXACTLY(1);
	return OBJECT_VAL(krk_newBytes(sizeof(KrkValue),(uint8_t*)&argv[0]));
})

void krk_setMaximumRecursionDepth(size_t maxDepth) {
	vm.maximumCallDepth = maxDepth;

	KrkThreadState * thread = vm.threads;
	while (thread) {
		thread->frames = realloc(thread->frames, maxDepth * sizeof(KrkCallFrame));
		thread = thread->next;
	}
}

void krk_initVM(int flags) {
#if defined(ENABLE_THREADING) && defined(__APPLE__) && defined(__aarch64__)
	krk_forceThreadData();
#endif

	vm.globalFlags = flags & 0xFF00;
	vm.maximumCallDepth = KRK_CALL_FRAMES_MAX;

	/* Reset current thread */
	krk_resetStack();
	krk_currentThread.frames   = calloc(vm.maximumCallDepth,sizeof(KrkCallFrame));
	krk_currentThread.flags    = flags & 0x00FF;
	krk_currentThread.module   = NULL;
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
	#define CACHED_METHOD(a,b,c) [METHOD_ ## a] = {b,sizeof(b)-1},
	#define SPECIAL_ATTRS(a,b)   [METHOD_ ## a] = {b,sizeof(b)-1},
	#include "methods.h"
	#undef CACHED_METHOD
	#undef SPECIAL_ATTRS
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
	_createAndBind_sliceClass();
	_createAndBind_exceptions();
	_createAndBind_generatorClass();
	_createAndBind_gcMod();
	_createAndBind_timeMod();
	_createAndBind_osMod();
	_createAndBind_fileioMod();
#ifndef KRK_DISABLE_DEBUG
	_createAndBind_disMod();
#endif
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
	KRK_DOC(vm.system, "@brief System module.");
	krk_attachNamedObject(&vm.system->fields, "version",
		(KrkObj*)S(KRK_VERSION_MAJOR "." KRK_VERSION_MINOR "." KRK_VERSION_PATCH KRK_VERSION_EXTRA));
	krk_attachNamedObject(&vm.system->fields, "buildenv", (KrkObj*)S(KRK_BUILD_COMPILER));
	krk_attachNamedObject(&vm.system->fields, "builddate", (KrkObj*)S(KRK_BUILD_DATE));
	KRK_DOC(BIND_FUNC(vm.system,getsizeof),
		"@brief Calculate the approximate size of an object in bytes.\n"
		"@arguments value\n\n"
		"@param value Value to examine.");
	KRK_DOC(BIND_FUNC(vm.system,set_clean_output),
		"@brief Disables terminal escapes in some output from the VM.\n"
		"@arguments clean=True\n\n"
		"@param clean Whether to remove escapes.");
	KRK_DOC(BIND_FUNC(vm.system,set_tracing),
		"@brief Toggle debugging modes.\n"
		"@arguments tracing=None,disassembly=None,scantracing=None,stressgc=None\n\n"
		"Enables or disables tracing options for the current thread.\n\n"
		"@param tracing Enables instruction tracing.\n"
		"@param disassembly Prints bytecode disassembly after compilation.\n"
		"@param scantracing Prints debug output from the token scanner during compilation.\n"
		"@param stressgc Forces a garbage collection cycle on each heap allocation.");
	KRK_DOC(BIND_FUNC(vm.system,importmodule),
		"@brief Import a module by string name\n"
		"@arguments module\n\n"
		"Imports the dot-separated module @p module as if it were imported by the @c import statement and returns the resulting module object.\n\n"
		"@param module A string with a dot-separated package or module name");
	KRK_DOC(BIND_FUNC(vm.system,modules),
		"Get the list of valid names from the module table");
	KRK_DOC(BIND_FUNC(vm.system,unload),
		"Removes a module from the module table. It is not necessarily garbage collected if other references to it exist.");
	KRK_DOC(BIND_FUNC(vm.system,inspect_value),
		"Obtain the memory representation of a stack value.");
	krk_attachNamedObject(&vm.system->fields, "module", (KrkObj*)vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.system->fields, "path_sep", (KrkObj*)S(PATH_SEP));
	KrkValue module_paths = krk_list_of(0,NULL,0);
	krk_attachNamedValue(&vm.system->fields, "module_paths", module_paths);
	krk_writeValueArray(AS_LIST(module_paths), OBJECT_VAL(S("./")));
#ifndef NO_FILESYSTEM
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
#endif

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
	memset(&_exceptions,0,sizeof(_exceptions));
	memset(&_baseClasses,0,sizeof(_baseClasses));
	krk_freeObjects();

	if (vm.binpath) free(vm.binpath);

	while (krk_currentThread.next) {
		KrkThreadState * thread = krk_currentThread.next;
		krk_currentThread.next = thread->next;
		FREE_ARRAY(size_t, thread->stack, thread->stackSize);
		free(thread->frames);
	}

	FREE_ARRAY(size_t, krk_currentThread.stack, krk_currentThread.stackSize);
	memset(&krk_vm,0,sizeof(krk_vm));
	free(krk_currentThread.frames);
	memset(&krk_currentThread,0,sizeof(KrkThreadState));

	extern void krk_freeMemoryDebugger(void);
	krk_freeMemoryDebugger();
}

/**
 * Internal type(value).__name__ call for use in debugging methods and
 * creating exception strings.
 */
const char * krk_typeName(KrkValue value) {
	return krk_getType(value)->name->chars;
}

static KrkValue tryBind(const char * name, KrkValue a, KrkValue b, const char * operator, const char * inverse) {
	krk_currentThread.scratchSpace[0] = a;
	krk_currentThread.scratchSpace[1] = b;

	/* Potential return value */
	KrkValue value = NONE_VAL();
	KrkString * methodName = krk_copyString(name, strlen(name));
	krk_push(OBJECT_VAL(methodName));

	/* Bind from a */
	int res;
	KrkClass * type = krk_getType(a);
	krk_push(a);
	if ((res = krk_unbindMethod(type, methodName))) {
		krk_swap(1);
		if (res == 2) {
			krk_pop();
		}
		krk_push(b);
		value = krk_callStack(res == 2 ? 1 : 2);
		if (!IS_NOTIMPL(value)) goto _success;
		krk_pop(); /* name */
	} else {
		krk_pop(); /* a */
		krk_pop(); /* name */
	}

	/* Bind from b */
	methodName = krk_copyString(inverse, strlen(inverse));
	krk_push(OBJECT_VAL(methodName));
	type = krk_getType(b);
	krk_push(b);
	if ((res = krk_unbindMethod(type, methodName))) {
		krk_swap(1);
		if (res == 2) {
			krk_pop();
		}
		krk_push(a);
		value = krk_callStack(res == 2 ? 1 : 2);
		if (!IS_NOTIMPL(value)) goto _success;
		krk_pop(); /* name */
	} else {
		krk_pop(); /* b */
		krk_pop(); /* name */
	}

	return krk_runtimeError(vm.exceptions->typeError,
		"unsupported operand types for %s: '%s' and '%s'",
		operator, krk_typeName(a), krk_typeName(b));

_success:
	krk_pop(); /* name */
	/* Return result */
	return value;
}

/**
 * Basic arithmetic and string functions follow.
 */

#define MAKE_BIN_OP(name,operator,inv) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return FLOATING_VAL(AS_FLOATING(a) operator (double)AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return FLOATING_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return FLOATING_VAL((double)AS_INTEGER(a) operator AS_FLOATING(b)); \
		} \
		return tryBind("__" #name "__", a, b, #operator, "__" #inv "__"); \
	}

MAKE_BIN_OP(add,+,radd)
MAKE_BIN_OP(sub,-,ssub)
MAKE_BIN_OP(mul,*,rmul)

/**
 * Division operators.
 */
KrkValue krk_operator_truediv(KrkValue a, KrkValue b) {
	if (IS_INTEGER(a)) {
		if (IS_INTEGER(b)) return FLOATING_VAL((double)AS_INTEGER(a) / (double)AS_INTEGER(b));
		else if (IS_FLOATING(b)) return FLOATING_VAL((double)AS_INTEGER(a) / AS_FLOATING(b));
	} else if (IS_FLOATING(a)) {
		if (IS_FLOATING(b)) return FLOATING_VAL(AS_FLOATING(a) / AS_FLOATING(b));
		else if (IS_INTEGER(b)) return FLOATING_VAL(AS_FLOATING(a) / (double)AS_INTEGER(b));
	}
	return tryBind("__truediv__", a, b, "/", "__rtruediv__");
}

#ifdef __TINYC__
#include <math.h>
#define __builtin_floor floor
#endif

KrkValue krk_operator_floordiv(KrkValue numerator, KrkValue divisor) {
	if (IS_INTEGER(divisor) && IS_INTEGER(numerator)) return INTEGER_VAL(AS_INTEGER(numerator) / AS_INTEGER(divisor));
	else if (IS_INTEGER(divisor) && IS_FLOATING(numerator)) return FLOATING_VAL(__builtin_floor(AS_FLOATING(numerator) / (double)AS_INTEGER(divisor)));
	else if (IS_FLOATING(divisor)) {
		if (IS_FLOATING(numerator)) return FLOATING_VAL(__builtin_floor(AS_FLOATING(numerator) / AS_FLOATING(divisor)));
		else if (IS_INTEGER(numerator)) return FLOATING_VAL(__builtin_floor((double)AS_INTEGER(numerator) / AS_FLOATING(divisor)));
	}
	return tryBind("__floordiv__", numerator, divisor, "//", "__rfloordiv__");
}

#define MAKE_UNOPTIMIZED_BIN_OP(name,operator,inv) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		return tryBind("__" #name "__", a, b, #operator, "__" #inv "__"); \
	}

MAKE_UNOPTIMIZED_BIN_OP(pow,**,rpow)

/* Bit ops are invalid on doubles in C, so we can't use the same set of macros for them;
 * they should be invalid in Kuroko as well. */
#define MAKE_BIT_OP_BOOL(name,operator,inv) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_BOOLEAN(a) && IS_BOOLEAN(b)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		return tryBind("__" #name "__", a, b, #operator, "__" #inv "__"); \
	}
#define MAKE_BIT_OP(name,operator,inv) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		return tryBind("__" #name "__", a, b, #operator, "__" #inv "__"); \
	}

MAKE_BIT_OP_BOOL(or,|,ror)
MAKE_BIT_OP_BOOL(xor,^,rxor)
MAKE_BIT_OP_BOOL(and,&,rand)
MAKE_BIT_OP(lshift,<<,rlshift)
MAKE_BIT_OP(rshift,>>,rrshift)
MAKE_BIT_OP(mod,%,rmod) /* not a bit op, but doesn't work on floating point */

#define MAKE_COMPARATOR(name, operator,inv) \
	KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator (double)AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return BOOLEAN_VAL((double)AS_INTEGER(a) operator AS_FLOATING(b)); \
		} \
		return tryBind("__" #name "__", a, b, #operator, "__" #inv "__"); \
	}

MAKE_COMPARATOR(lt, <, gt)
MAKE_COMPARATOR(gt, >, lt)
MAKE_COMPARATOR(le, <=, ge)
MAKE_COMPARATOR(ge, >=, le)

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
	for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1);
		stackOffset >= exitSlot &&
		!IS_TRY_HANDLER(krk_currentThread.stack[stackOffset]) &&
		!IS_WITH_HANDLER(krk_currentThread.stack[stackOffset]) &&
		!IS_EXCEPT_HANDLER(krk_currentThread.stack[stackOffset])
		; stackOffset--);
	if (stackOffset < exitSlot) {
		if (exitSlot == 0) {
			/*
			 * No exception was found and we have reached the top of the call stack.
			 * Call dumpTraceback to present the exception to the user and reset the
			 * VM stack state. It should still be safe to execute more code after
			 * this reset, so the repl can throw errors and keep accepting new lines.
			 */
			if (!(vm.globalFlags & KRK_GLOBAL_CLEAN_OUTPUT)) krk_dumpTraceback();
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
		abort();
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
int krk_loadModule(KrkString * path, KrkValue * moduleOut, KrkString * runAs, KrkValue parent) {
	KrkValue modulePaths;

	/* See if the module is already loaded */
	if (krk_tableGet_fast(&vm.modules, runAs, moduleOut)) {
		krk_push(*moduleOut);
		return 1;
	}

#ifndef NO_FILESYSTEM

	/* Obtain __builtins__.module_paths */
	if (!krk_tableGet_fast(&vm.system->fields, S("module_paths"), &modulePaths) || !IS_INSTANCE(modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions->importError,
			"kuroko.module_paths not defined.");
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
		int isPackage = 0;
		char * fileName;

		krk_push(AS_LIST(modulePaths)->values[i]);
		if (!IS_STRING(krk_peek(0))) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->typeError,
				"Module search paths must be strings; check the search path at index %d", i);
			return 0;
		}

		/* Try .../path/__init__.krk */
		krk_push(OBJECT_VAL(path));
		krk_addObjects();
		krk_push(OBJECT_VAL(S(PATH_SEP "__init__.krk")));
		krk_addObjects();
		fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) == 0) {
			isPackage = 1;
			if (runAs == S("__main__")) {
				krk_pop(); /* concatenated name */

				/* Convert back to .-formatted */
				krk_push(krk_valueGetAttribute(OBJECT_VAL(path), "replace"));
				krk_push(OBJECT_VAL(S(PATH_SEP)));
				krk_push(OBJECT_VAL(S(".")));
				krk_push(krk_callStack(2));
				KrkValue packageName = krk_peek(0);
				krk_push(packageName);
				krk_push(OBJECT_VAL(S(".")));
				krk_addObjects();
				krk_push(OBJECT_VAL(runAs));
				krk_addObjects();

				/* Try to import that. */
				KrkValue dotted_main = krk_peek(0);
				if (!krk_importModule(AS_STRING(dotted_main),runAs)) {
					krk_runtimeError(vm.exceptions->importError, "No module named %s; '%s' is a package and cannot be executed directly",
						AS_CSTRING(dotted_main), AS_CSTRING(packageName));
					return 0;
				}

				krk_swap(2);
				krk_pop(); /* package name */
				krk_pop(); /* dotted_main */
				*moduleOut = krk_peek(0);
				return 1;
			}
			goto _normalFile;
		}

#ifndef STATIC_ONLY
		/* Try .../path.so */
		krk_pop();
		krk_push(AS_LIST(modulePaths)->values[i]);
		krk_push(OBJECT_VAL(path));
		krk_addObjects(); /* Concatenate path... */
		krk_push(OBJECT_VAL(S(".so")));
		krk_addObjects(); /* and file extension */
		fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) == 0) {
			goto _sharedObject;
		}
#endif

		/* Try .../path.krk */
		krk_pop();
		krk_push(AS_LIST(modulePaths)->values[i]);
		krk_push(OBJECT_VAL(path));
		krk_addObjects(); /* Concatenate path... */
		krk_push(OBJECT_VAL(S(".krk")));
		krk_addObjects(); /* and file extension */
		fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) == 0) {
			goto _normalFile;
		}

		/* Try next search path */
		continue;

	_normalFile: (void)0;
		/* Compile and run the module in a new context and exit the VM when it
		 * returns to the current call frame; modules should return objects. */
		KrkInstance * enclosing = krk_currentThread.module;
		krk_startModule(runAs->chars);
		if (isPackage) {
			krk_attachNamedValue(&krk_currentThread.module->fields,"__ispackage__",BOOLEAN_VAL(1));
			/* For a module that is a package, __package__ is its own name */
			krk_attachNamedValue(&krk_currentThread.module->fields,"__package__",OBJECT_VAL(runAs));
		} else {
			KrkValue parentName;
			if (IS_INSTANCE(parent) && krk_tableGet_fast(&AS_INSTANCE(parent)->fields, S("__name__"), &parentName) && IS_STRING(parentName)) {
				krk_attachNamedValue(&krk_currentThread.module->fields, "__package__", parentName);
			} else {
				/* If there is no parent, or the parent doesn't have a string __name__ attribute,
				 * set the __package__ to None, so it at least exists. */
				krk_attachNamedValue(&krk_currentThread.module->fields, "__package__", NONE_VAL());
			}
		}
		krk_callfile(fileName,fileName);
		*moduleOut = OBJECT_VAL(krk_currentThread.module);
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

#ifndef STATIC_ONLY
	_sharedObject: (void)0;

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
			dlClose(dlRef);
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->importError,
				"Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad(runAs);
		if (!krk_isInstanceOf(*moduleOut, vm.baseClasses->moduleClass)) {
			dlClose(dlRef);
			krk_runtimeError(vm.exceptions->importError,
				"Failed to load module '%s' from '%s'", runAs->chars, fileName);
			return 0;
		}

		krk_push(*moduleOut);
		krk_swap(1);

		struct KrkModule * moduleAsStruct = (struct KrkModule*)AS_INSTANCE(*moduleOut);
		moduleAsStruct->libHandle = dlRef;

		krk_attachNamedObject(&AS_INSTANCE(*moduleOut)->fields, "__name__", (KrkObj*)runAs);
		krk_attachNamedValue(&AS_INSTANCE(*moduleOut)->fields, "__file__", krk_peek(0));

		krk_pop(); /* filename */
		krk_tableSet(&vm.modules, OBJECT_VAL(runAs), *moduleOut);
		return 1;
#endif
	}

#endif

	/* If we still haven't found anything, fail. */
	*moduleOut = NONE_VAL();

	/* Was this a __main__? */
	if (runAs == S("__main__")) {
		/* Then let's use 'path' instead, and replace all the /'s with .'s... */
		krk_push(krk_valueGetAttribute(OBJECT_VAL(path), "replace"));
		krk_push(OBJECT_VAL(S(PATH_SEP)));
		krk_push(OBJECT_VAL(S(".")));
		krk_push(krk_callStack(2));
	} else {
		krk_push(OBJECT_VAL(runAs));
	}

	krk_runtimeError(vm.exceptions->importError, "No module named '%s'", AS_CSTRING(krk_peek(0)));

	return 0;
}

int krk_importModule(KrkString * name, KrkString * runAs) {
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
		return krk_loadModule(name,&base,runAs,NONE_VAL());
	}

	if (name->chars[0] == '.') {
		/**
		 * For relative imports, we canonicalize the import name based on the current package,
		 * and then trying importModule again with the fully qualified name.
		 */

		KrkValue packageName;
		if (!krk_tableGet_fast(&krk_currentThread.module->fields, S("__package__"), &packageName) || !IS_STRING(packageName)) {
			/* We must have __package__ set to a string for this to make any sense. */
			krk_runtimeError(vm.exceptions->importError, "attempted relative import without a package context");
			return 0;
		}

		if (name->length == 1) {
			/* from . import ... */
			return krk_importModule(AS_STRING(packageName), AS_STRING(packageName));
		}

		if (name->chars[1] != '.') {
			/* from .something import ... */
			krk_push(packageName);
			krk_push(OBJECT_VAL(name));
			krk_addObjects();

			if (krk_importModule(AS_STRING(krk_peek(0)), AS_STRING(krk_peek(0)))) {
				krk_swap(1); /* Imported module */
				krk_pop(); /* Name */
				return 1;
			}

			return 0;
		}

		/**
		 * from .. import ...
		 *   or
		 * from ..something import ...
		 *
		 * If there n dots, there are n-1 components to pop from the end of
		 * the package name, as '..' is "go up one" and '...' is "go up two".
		 */
		size_t dots = 0;
		while (name->chars[dots+1] == '.') dots++;

		/* We'll split the package name is str.split(__package__,'.') */
		krk_push(packageName);
		krk_push(OBJECT_VAL(S(".")));
		KrkValue components = krk_string_split(2,(KrkValue[]){krk_peek(1),krk_peek(0)}, 0);
		if (!IS_list(components)) {
			krk_runtimeError(vm.exceptions->importError, "internal error while calculating package path");
			return 0;
		}
		krk_push(components);
		krk_swap(2);
		krk_pop();
		krk_pop();

		/* If there are not enough components to "go up" through, that's an error. */
		if (AS_LIST(components)->count <= dots) {
			krk_runtimeError(vm.exceptions->importError, "attempted relative import beyond top-level package");
			return 0;
		}

		size_t count = AS_LIST(components)->count - dots;
		struct StringBuilder sb = {0};

		/* Now rebuild the dotted form from the remaining components... */
		for (size_t i = 0; i < count; i++) {
			KrkValue node = AS_LIST(components)->values[i];
			if (!IS_STRING(node)) {
				discardStringBuilder(&sb);
				krk_runtimeError(vm.exceptions->importError, "internal error while calculating package path");
				return 0;
			}
			pushStringBuilderStr(&sb, AS_CSTRING(node), AS_STRING(node)->length);
			if (i + 1 != count) {
				pushStringBuilder(&sb, '.');
			}
		}

		krk_pop(); /* components */

		if (name->chars[dots+1]) {
			/* from ..something import ... - append '.something' */
			pushStringBuilderStr(&sb, &name->chars[dots], name->length - dots);
		}

		krk_push(OBJECT_VAL(finishStringBuilder(&sb)));

		/* Now to try to import the fully qualified module path */
		if (krk_importModule(AS_STRING(krk_peek(0)), AS_STRING(krk_peek(0)))) {
			krk_swap(1); /* Imported module */
			krk_pop(); /* Name */
			return 1;
		}
		return 0;
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
			if (!krk_loadModule(AS_STRING(krk_currentThread.stack[argBase+1]), &current, runAs, krk_currentThread.stack[argBase-1])) return 0;
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
			if (!krk_loadModule(AS_STRING(krk_currentThread.stack[argBase+1]), &current, AS_STRING(krk_currentThread.stack[argBase+2]),NONE_VAL())) return 0;
			krk_push(current);
			if (!IS_NONE(krk_currentThread.stack[argBase-1])) {
				krk_tableSet(&AS_INSTANCE(krk_currentThread.stack[argBase-1])->fields, krk_currentThread.stack[argBase+0], krk_peek(0));
			}
			/* Is this a package? */
			KrkValue tmp;
			if (!krk_tableGet_fast(&AS_INSTANCE(current)->fields, S("__ispackage__"), &tmp) || !IS_BOOLEAN(tmp) || AS_BOOLEAN(tmp) != 1) {
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

int krk_doRecursiveModuleLoad(KrkString * name) {
	return krk_importModule(name,name);
}

/**
 * Try to resolve and push [stack top].name.
 * If [stack top] is an instance, scan fields first.
 * Otherwise, scan for methods from [stack top].__class__.
 * Returns 0 if nothing was found, 1 if something was - and that
 * "something" will replace [stack top].
 */
static int valueGetProperty(KrkString * name) {
	KrkValue this = krk_peek(0);
	KrkClass * objectClass;
	KrkValue value;
	if (IS_INSTANCE(this)) {
		KrkInstance * instance = AS_INSTANCE(this);
		if (krk_tableGet_fast(&instance->fields, name, &value)) {
			krk_currentThread.stackTop[-1] = value;
			return 1;
		}
		objectClass = instance->_class;
	} else if (IS_CLASS(this)) {
		KrkClass * _class = AS_CLASS(this);
		do {
			if (krk_tableGet_fast(&_class->methods, name, &value)) {
				if ((IS_NATIVE(value) || IS_CLOSURE(value)) && (AS_OBJECT(value)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD)) {
					value = OBJECT_VAL(krk_newBoundMethod(this, AS_OBJECT(value)));
				}
				krk_currentThread.stackTop[-1] = value;
				return 1;
			}
			_class = _class->base;
		} while (_class);
		objectClass = vm.baseClasses->typeClass;
	} else if (IS_CLOSURE(krk_peek(0))) {
		KrkClosure * closure = AS_CLOSURE(this);
		if (krk_tableGet_fast(&closure->fields, name, &value)) {
			krk_currentThread.stackTop[-1] = value;
			return 1;
		}
		objectClass = vm.baseClasses->functionClass;
	} else {
		objectClass = krk_getType(this);
	}

	/* See if the base class for this non-instance type has a method available */
	if (krk_bindMethod(objectClass, name)) {
		return 1;
	}

	if (objectClass->_getattr) {
		krk_push(OBJECT_VAL(name));
		krk_push(krk_callDirect(objectClass->_getattr, 2));
		return 1;
	}

	return 0;
}

/**
 * Similar to the above, but specifically for getting properties that will be
 * immediately called, eg. for GET_METHOD that is followed by argument pushing
 * and then CALL_METHOD.
 */
static int valueGetMethod(KrkString * name) {
	KrkValue this = krk_peek(0);
	KrkClass * objectClass;
	KrkValue value;
	if (IS_INSTANCE(this)) {
		KrkInstance * instance = AS_INSTANCE(this);
		if (krk_tableGet_fast(&instance->fields, name, &value)) {
			krk_push(value);
			return 2;
		}
		objectClass = instance->_class;
	} else if (IS_CLASS(this)) {
		KrkClass * _class = AS_CLASS(this);
		do {
			if (krk_tableGet_fast(&_class->methods, name, &value)) {
				if ((IS_CLOSURE(value)||IS_NATIVE(value)) && (AS_OBJECT(value)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD)) {
					krk_push(value);
					return 1;
				}
				krk_push(value);
				return 2;
			}
			_class = _class->base;
		} while (_class);
		objectClass = vm.baseClasses->typeClass;
	} else if (IS_CLOSURE(krk_peek(0))) {
		KrkClosure * closure = AS_CLOSURE(this);
		if (krk_tableGet_fast(&closure->fields, name, &value)) {
			krk_push(value);
			return 2;
		}
		objectClass = vm.baseClasses->functionClass;
	} else {
		objectClass = krk_getType(this);
	}

	/* See if the base class for this non-instance type has a method available */
	int maybe = krk_unbindMethod(objectClass, name);
	if (maybe) return maybe;

	if (objectClass->_getattr) {
		krk_push(krk_peek(0));
		krk_push(OBJECT_VAL(name));
		krk_push(krk_callDirect(objectClass->_getattr, 2));
		return 2;
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

KrkValue krk_valueGetAttribute_default(KrkValue value, char * name, KrkValue defaultVal) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(value);
	if (!valueGetProperty(AS_STRING(krk_peek(1)))) {
		krk_pop();
		krk_pop();
		return defaultVal;
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
		if (!krk_tableDelete(&_class->methods, OBJECT_VAL(name))) {
			return 0;
		}
		if (name->length > 1 && name->chars[0] == '_' && name->chars[1] == '_') {
			krk_finalizeClass(_class);
		}
		krk_pop(); /* the original value */
		return 1;
	}
	/* TODO del on values? */
	return 0;
}

KrkValue krk_valueDelAttribute(KrkValue owner, char * name) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(owner);
	if (!valueDelProperty(AS_STRING(krk_peek(1)))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name);
	}
	krk_pop(); /* String */
	return NONE_VAL();
}

static int trySetDescriptor(KrkValue owner, KrkString * name, KrkValue value) {
	KrkClass * _class = krk_getType(owner);
	KrkValue property;
	while (_class) {
		if (krk_tableGet_fast(&_class->methods, name, &property)) break;
		_class = _class->base;
	}
	if (_class) {
		KrkClass * type = krk_getType(property);
		if (type->_descset) {
			/* Need to rearrange arguments */
			krk_push(property); /* owner value property */
			krk_swap(2);        /* property value owner */
			krk_swap(1);        /* property owner value */
			krk_push(krk_callDirect(type->_descset, 3));
			return 1;
		}
	}
	return 0;
}

static int valueSetProperty(KrkString * name) {
	KrkValue owner = krk_peek(1);
	KrkValue value = krk_peek(0);
	if (IS_INSTANCE(owner)) {
		if (krk_tableSet(&AS_INSTANCE(owner)->fields, OBJECT_VAL(name), value)) {
			if (trySetDescriptor(owner, name, value)) {
				krk_tableDelete(&AS_INSTANCE(owner)->fields, OBJECT_VAL(name));
				return 1;
			}
		}
	} else if (IS_CLASS(owner)) {
		krk_tableSet(&AS_CLASS(owner)->methods, OBJECT_VAL(name), value);
		if (name->length > 1 && name->chars[0] == '_' && name->chars[1] == '_') {
			/* Quietly call finalizeClass to update special method table if this looks like it might be one */
			krk_finalizeClass(AS_CLASS(owner));
		}
	} else if (IS_CLOSURE(owner)) {
		/* Closures shouldn't have descriptors, but let's let this happen anyway... */
		if (krk_tableSet(&AS_CLOSURE(owner)->fields, OBJECT_VAL(name), value)) {
			if (trySetDescriptor(owner, name, value)) {
				krk_tableDelete(&AS_CLOSURE(owner)->fields, OBJECT_VAL(name));
				return 1;
			}
		}
	} else {
		return (trySetDescriptor(owner,name,value));
	}
	krk_swap(1);
	krk_pop();
	return 1;
}

KrkValue krk_valueSetAttribute(KrkValue owner, char * name, KrkValue to) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(owner);
	krk_push(to);
	if (!valueSetProperty(AS_STRING(krk_peek(2)))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(1)), name);
	}
	krk_swap(1);
	krk_pop(); /* String */
	return krk_pop();
}

#define READ_BYTE() (*frame->ip++)
#define BINARY_OP(op) { KrkValue b = krk_peek(0); KrkValue a = krk_peek(1); \
	a = krk_operator_ ## op (a,b); \
	krk_currentThread.stackTop[-2] = a; krk_pop(); break; }
#define BINARY_OP_CHECK_ZERO(op) { KrkValue b = krk_peek(0); KrkValue a = krk_peek(1); \
	if ((IS_INTEGER(b) && AS_INTEGER(b) == 0)) { krk_runtimeError(vm.exceptions->zeroDivisionError, "integer division or modulo by zero"); goto _finishException; } \
	else if ((IS_FLOATING(b) && AS_FLOATING(b) == 0.0)) { krk_runtimeError(vm.exceptions->zeroDivisionError, "float division by zero"); goto _finishException; } \
	a = krk_operator_ ## op (a,b); \
	krk_currentThread.stackTop[-2] = a; krk_pop(); break; }
#define READ_CONSTANT(s) (frame->closure->function->chunk.constants.values[OPERAND])
#define READ_STRING(s) AS_STRING(READ_CONSTANT(s))

extern FUNC_SIG(list,append);
extern FUNC_SIG(dict,__setitem__);
extern FUNC_SIG(set,add);

/**
 * VM main loop.
 */
static KrkValue run() {
	KrkCallFrame* frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];

	while (1) {
#ifndef KRK_NO_TRACING
		if (unlikely(krk_currentThread.flags & (KRK_THREAD_ENABLE_TRACING | KRK_THREAD_SINGLE_STEP | KRK_THREAD_SIGNALLED))) {
			if (krk_currentThread.flags & KRK_THREAD_ENABLE_TRACING) {
				krk_debug_dumpStack(stderr, frame);
				krk_disassembleInstruction(stderr, frame->closure->function,
					(size_t)(frame->ip - frame->closure->function->chunk.code));
			}

			if (krk_currentThread.flags & KRK_THREAD_SINGLE_STEP) {
				krk_debuggerHook(frame);
			}

			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) {
				krk_currentThread.flags &= ~(KRK_THREAD_SIGNALLED); /* Clear signal flag */
				krk_runtimeError(vm.exceptions->keyboardInterrupt, "Keyboard interrupt.");
				goto _finishException;
			}
		}
_resumeHook: (void)0;
#endif

		/* Each instruction begins with one opcode byte */
		KrkOpCode opcode = READ_BYTE();
		int OPERAND = 0;

/* Only GCC lets us put these on empty statements; just hope clang doesn't start complaining */
#ifndef __clang__
# define FALLTHROUGH __attribute__((fallthrough));
#else
# define FALLTHROUGH
#endif

#define TWO_BYTE_OPERAND { OPERAND = (frame->ip[0] << 8) | frame->ip[1]; frame->ip += 2; }
#define THREE_BYTE_OPERAND { OPERAND = (frame->ip[0] << 16) | (frame->ip[1] << 8); frame->ip += 2; } FALLTHROUGH
#define ONE_BYTE_OPERAND { OPERAND |= READ_BYTE(); }

		switch (opcode) {
			case OP_CLEANUP_WITH: {
				/* Top of stack is a HANDLER that should have had something loaded into it if it was still valid */
				KrkValue handler = krk_peek(0);
				KrkValue exceptionObject = krk_peek(1);
				KrkValue contextManager = krk_peek(2);
				KrkClass * type = krk_getType(contextManager);
				krk_push(contextManager);
				if (AS_HANDLER_TYPE(handler) == OP_RAISE) {
					krk_push(OBJECT_VAL(krk_getType(exceptionObject)));
					krk_push(exceptionObject);
					KrkValue tracebackEntries = NONE_VAL();
					if (IS_INSTANCE(exceptionObject))
						krk_tableGet_fast(&AS_INSTANCE(exceptionObject)->fields, S("traceback"), &tracebackEntries);
					krk_push(tracebackEntries);
					krk_callDirect(type->_exit, 4);
					/* Top of stack is now either someone else's problem or a return value */
					if (!(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
						krk_pop(); /* Handler object */
						krk_currentThread.currentException = krk_pop(); /* Original exception */
						krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
					}
					goto _finishException;
				} else {
					krk_push(NONE_VAL());
					krk_push(NONE_VAL());
					krk_push(NONE_VAL());
					krk_callDirect(type->_exit, 4);
					if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) goto _finishException;
				}
				if (AS_HANDLER_TYPE(handler) != OP_RETURN) break;
				krk_pop(); /* handler */
			} /* fallthrough */
			case OP_RETURN: {
_finishReturn: (void)0;
				KrkValue result = krk_pop();
				closeUpvalues(frame->slots);
				/* See if this frame had a thing */
				int stackOffset;
				for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1);
					stackOffset >= (int)frame->slots && 
					!IS_WITH_HANDLER(krk_currentThread.stack[stackOffset]) &&
					!IS_TRY_HANDLER(krk_currentThread.stack[stackOffset]) &&
					!IS_EXCEPT_HANDLER(krk_currentThread.stack[stackOffset])
					; stackOffset--);
				if (stackOffset >= (int)frame->slots) {
					krk_currentThread.stackTop = &krk_currentThread.stack[stackOffset + 1];
					frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(krk_peek(0));
					krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_RETURN,AS_HANDLER_TARGET(krk_peek(0)));
					krk_currentThread.stackTop[-2] = result;
					break;
				}
				FRAME_OUT(frame);
				krk_currentThread.frameCount--;
				if (krk_currentThread.frameCount == 0) {
					krk_pop();
					return result;
				}
				krk_currentThread.stackTop = &krk_currentThread.stack[frame->outSlots];
				if (krk_currentThread.frameCount == (size_t)krk_currentThread.exitOnFrame) {
					if (frame->closure->function->obj.flags & (KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR | KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE)) {
						krk_push(result);
						return KWARGS_VAL(0);
					}
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
			case OP_LESS_EQUAL: BINARY_OP(le);
			case OP_GREATER_EQUAL: BINARY_OP(ge);
			case OP_ADD: BINARY_OP(add);
			case OP_SUBTRACT: BINARY_OP(sub)
			case OP_MULTIPLY: BINARY_OP(mul)
			case OP_DIVIDE: BINARY_OP_CHECK_ZERO(truediv)
			case OP_FLOORDIV: BINARY_OP_CHECK_ZERO(floordiv)
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
				else { krk_runtimeError(vm.exceptions->typeError, "Incompatible operand type for %s negation.", "bit"); goto _finishException; }
				break;
			}
			case OP_NEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(-AS_INTEGER(value)));
				else if (IS_FLOATING(value)) krk_push(FLOATING_VAL(-AS_FLOATING(value)));
				else { krk_runtimeError(vm.exceptions->typeError, "Incompatible operand type for %s negation.", "prefix"); goto _finishException; }
				break;
			}
			case OP_NONE:  krk_push(NONE_VAL()); break;
			case OP_TRUE:  krk_push(BOOLEAN_VAL(1)); break;
			case OP_FALSE: krk_push(BOOLEAN_VAL(0)); break;
			case OP_UNSET: krk_push(KWARGS_VAL(0)); break;
			case OP_NOT:   krk_push(BOOLEAN_VAL(krk_isFalsey(krk_pop()))); break;
			case OP_POP:   krk_pop(); break;
			case OP_RAISE: {
				if (IS_CLASS(krk_peek(0))) {
					krk_currentThread.currentException = krk_callStack(0);
				} else {
					krk_currentThread.currentException = krk_pop();
				}
				attachTraceback();
				krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
				goto _finishException;
			}
			case OP_CLOSE_UPVALUE:
				closeUpvalues((krk_currentThread.stackTop - krk_currentThread.stack)-1);
				krk_pop();
				break;
			case OP_INVOKE_GETTER: {
				KrkClass * type = krk_getType(krk_peek(1));
				if (likely(type->_getter != NULL)) {
					krk_push(krk_callDirect(type->_getter, 2));
				} else if (IS_CLASS(krk_peek(1)) && AS_CLASS(krk_peek(1))->_classgetitem) {
					krk_push(krk_callDirect(AS_CLASS(krk_peek(1))->_classgetitem, 2));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
				}
				break;
			}
			case OP_INVOKE_SETTER: {
				KrkClass * type = krk_getType(krk_peek(2));
				if (likely(type->_setter != NULL)) {
					krk_push(krk_callDirect(type->_setter, 3));
				} else {
					if (type->_getter) {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not mutable", krk_typeName(krk_peek(2)));
					} else {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(2)));
					}
				}
				break;
			}
			case OP_INVOKE_DELETE: {
				KrkClass * type = krk_getType(krk_peek(1));
				if (likely(type->_delitem != NULL)) {
					krk_callDirect(type->_delitem, 2);
				} else {
					if (type->_getter) {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not mutable", krk_typeName(krk_peek(1)));
					} else {
						krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", krk_typeName(krk_peek(1)));
					}
				}
				break;
			}
			case OP_INVOKE_ITER: {
				KrkClass * type = krk_getType(krk_peek(0));
				if (likely(type->_iter != NULL)) {
					krk_push(krk_callDirect(type->_iter, 1));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not iterable", krk_typeName(krk_peek(0)));
				}
				break;
			}
			case OP_INVOKE_CONTAINS: {
				KrkClass * type = krk_getType(krk_peek(0));
				if (likely(type->_contains != NULL)) {
					krk_swap(1);
					krk_push(krk_callDirect(type->_contains, 2));
				} else {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object can not be tested for membership", krk_typeName(krk_peek(0)));
				}
				break;
			}
			case OP_INVOKE_AWAIT: {
				if (!krk_getAwaitable()) goto _finishException;
				break;
			}
			case OP_FINALIZE: {
				KrkClass * _class = AS_CLASS(krk_peek(0));
				/* Store special methods for quick access */
				krk_finalizeClass(_class);
				break;
			}
			case OP_INHERIT: {
				KrkValue superclass = krk_peek(0);
				if (unlikely(!IS_CLASS(superclass))) {
					krk_runtimeError(vm.exceptions->typeError, "Superclass must be a class, not '%s'",
						krk_typeName(superclass));
					goto _finishException;
				}
				if (AS_OBJECT(superclass)->flags & KRK_OBJ_FLAGS_NO_INHERIT) {
					krk_runtimeError(vm.exceptions->typeError, "'%s' can not be subclassed",
						AS_CLASS(superclass)->name->chars);
					goto _finishException;
				}
				KrkClass * subclass = AS_CLASS(krk_peek(1));
				if (subclass->base) {
					krk_tableDelete(&subclass->base->subclasses, krk_peek(1));
				}
				subclass->base = AS_CLASS(superclass);
				subclass->allocSize = AS_CLASS(superclass)->allocSize;
				subclass->_ongcsweep = AS_CLASS(superclass)->_ongcsweep;
				subclass->_ongcscan = AS_CLASS(superclass)->_ongcscan;
				krk_tableSet(&AS_CLASS(superclass)->subclasses, krk_peek(1), NONE_VAL());
				krk_pop(); /* Super class */
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
			case OP_FILTER_EXCEPT: {
				int isMatch = 0;
				if (AS_HANDLER_TYPE(krk_peek(1)) == OP_RETURN) {
					isMatch = 0;
				} else if (AS_HANDLER_TYPE(krk_peek(1)) == OP_END_FINALLY) {
					isMatch = 0;
				} else if (IS_CLASS(krk_peek(0)) && krk_isInstanceOf(krk_peek(2), AS_CLASS(krk_peek(0)))) {
					isMatch = 1;
				} else if (IS_TUPLE(krk_peek(0))) {
					for (size_t i = 0; i < AS_TUPLE(krk_peek(0))->values.count; ++i) {
						if (IS_CLASS(AS_TUPLE(krk_peek(0))->values.values[i]) && krk_isInstanceOf(krk_peek(2), AS_CLASS(AS_TUPLE(krk_peek(0))->values.values[i]))) {
							isMatch = 1;
							break;
						}
					}
				} else if (IS_NONE(krk_peek(0))) {
					isMatch = !IS_NONE(krk_peek(2));
				}
				if (isMatch) {
					krk_currentThread.stackTop[-2] = HANDLER_VAL(OP_FILTER_EXCEPT,AS_HANDLER_TARGET(krk_peek(1)));
				}
				krk_pop();
				krk_push(BOOLEAN_VAL(isMatch));
				break;
			}
			case OP_BEGIN_FINALLY: {
				if (IS_HANDLER(krk_peek(0))) {
					if (AS_HANDLER_TYPE(krk_peek(0)) == OP_PUSH_TRY) {
						krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_BEGIN_FINALLY,AS_HANDLER_TARGET(krk_peek(0)));
					} else if (AS_HANDLER_TYPE(krk_peek(0)) == OP_FILTER_EXCEPT) {
						krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_BEGIN_FINALLY,AS_HANDLER_TARGET(krk_peek(0)));
					}
				}
				break;
			}
			case OP_END_FINALLY: {
				KrkValue handler = krk_peek(0);
				if (IS_HANDLER(handler)) {
					if (AS_HANDLER_TYPE(handler) == OP_RAISE || AS_HANDLER_TYPE(handler) == OP_END_FINALLY) {
						krk_pop(); /* handler */
						krk_currentThread.currentException = krk_pop();
						krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
						goto _finishException;
					} else if (AS_HANDLER_TYPE(handler) == OP_RETURN) {
						krk_push(krk_peek(1));
						goto _finishReturn;
					}
				}
				break;
			}
			case OP_BREAKPOINT: {
#ifndef KRK_DISABLE_DEBUG
				/* First off, halt execution. */
				krk_debugBreakpointHandler();
				if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) goto _finishException;
				goto _resumeHook;
#else
				krk_runtimeError(vm.exceptions->baseException, "Breakpoint.");
				goto _finishException;
#endif
			}
			case OP_YIELD: {
				KrkValue result = krk_peek(0);
				krk_currentThread.frameCount--;
				assert(krk_currentThread.frameCount == (size_t)krk_currentThread.exitOnFrame);
				/* Do NOT restore the stack */
				return result;
			}
			case OP_ANNOTATE: {
				if (IS_CLOSURE(krk_peek(0))) {
					krk_swap(1);
					AS_CLOSURE(krk_peek(1))->annotations = krk_peek(0);
					krk_pop();
				} else if (IS_NONE(krk_peek(0))) {
					krk_swap(1);
					krk_pop();
				} else {
					krk_runtimeError(vm.exceptions->typeError, "Can not annotate '%s'.", krk_typeName(krk_peek(0)));
					goto _finishException;
				}
				break;
			}

			/*
			 * Two-byte operands
			 */
			case OP_JUMP_IF_FALSE_OR_POP: {
				TWO_BYTE_OPERAND;
				uint16_t offset = OPERAND;
				if (krk_peek(0) == BOOLEAN_VAL(0) || krk_isFalsey(krk_peek(0))) frame->ip += offset;
				else krk_pop();
				break;
			}
			case OP_POP_JUMP_IF_FALSE: {
				TWO_BYTE_OPERAND;
				uint16_t offset = OPERAND;
				if (krk_peek(0) == BOOLEAN_VAL(0) || krk_isFalsey(krk_peek(0))) frame->ip += offset;
				krk_pop();
				break;
			}
			case OP_JUMP_IF_TRUE_OR_POP: {
				TWO_BYTE_OPERAND;
				uint16_t offset = OPERAND;
				if (!krk_isFalsey(krk_peek(0))) frame->ip += offset;
				else krk_pop();
				break;
			}
			case OP_JUMP: {
				TWO_BYTE_OPERAND;
				frame->ip += OPERAND;
				break;
			}
			case OP_LOOP: {
				TWO_BYTE_OPERAND;
				uint16_t offset = OPERAND;
				frame->ip -= offset;
				break;
			}
			case OP_PUSH_TRY: {
				TWO_BYTE_OPERAND;
				uint16_t tryTarget = OPERAND + (frame->ip - frame->closure->function->chunk.code);
				krk_push(NONE_VAL());
				KrkValue handler = HANDLER_VAL(OP_PUSH_TRY, tryTarget);
				krk_push(handler);
				break;
			}
			case OP_PUSH_WITH: {
				TWO_BYTE_OPERAND;
				uint16_t cleanupTarget = OPERAND + (frame->ip - frame->closure->function->chunk.code);
				KrkValue contextManager = krk_peek(0);
				KrkClass * type = krk_getType(contextManager);
				if (unlikely(!type->_enter || !type->_exit)) {
					if (!type->_enter) krk_runtimeError(vm.exceptions->attributeError, "__enter__");
					else if (!type->_exit) krk_runtimeError(vm.exceptions->attributeError, "__exit__");
					goto _finishException;
				}
				krk_push(contextManager);
				krk_callDirect(type->_enter, 1);
				/* Ignore result; don't need to pop */
				krk_push(NONE_VAL());
				KrkValue handler = HANDLER_VAL(OP_PUSH_WITH, cleanupTarget);
				krk_push(handler);
				break;
			}
			case OP_YIELD_FROM: {
				TWO_BYTE_OPERAND;
				uint8_t * exitIp = frame->ip + OPERAND;
				/* Stack has [iterator] [sent value] */
				/* Is this a generator or something with a 'send' method? */
				KrkValue method = krk_valueGetAttribute_default(krk_peek(1), "send", NONE_VAL());
				if (!IS_NONE(method)) {
					krk_push(method);
					krk_swap(1);
					krk_push(krk_callStack(1));
				} else {
					krk_pop();
					krk_push(krk_peek(0));
					krk_push(krk_callStack(0));
				}
				if (!krk_valuesSame(krk_peek(0), krk_peek(1))) {
					/* Value to yield */
					break;
				}

				krk_pop();

				/* Does it have a final value? */
				method = krk_valueGetAttribute_default(krk_peek(0), "__finish__", NONE_VAL());
				if (!IS_NONE(method)) {
					krk_push(method);
					krk_swap(1);
					krk_pop();
					krk_push(krk_callStack(0));
				} else {
					krk_pop();
					krk_push(NONE_VAL());
				}
				frame->ip = exitIp;
				break;
			}
			case OP_CALL_ITER: {
				TWO_BYTE_OPERAND;
				uint16_t offset = OPERAND;
				KrkValue iter = krk_peek(0);
				krk_push(iter);
				krk_push(krk_callStack(0));
				/* krk_valuesSame() */
				if (iter == krk_peek(0)) frame->ip += offset;
				break;
			}
			case OP_LOOP_ITER: {
				TWO_BYTE_OPERAND;
				uint16_t offset = OPERAND;
				KrkValue iter = krk_peek(0);
				krk_push(iter);
				krk_push(krk_callStack(0));
				if (iter != krk_peek(0)) frame->ip -= offset;
				break;
			}

			case OP_CONSTANT_LONG:
				THREE_BYTE_OPERAND;
			case OP_CONSTANT: {
				ONE_BYTE_OPERAND;
				size_t index = OPERAND;
				KrkValue constant = frame->closure->function->chunk.constants.values[index];
				krk_push(constant);
				break;
			}
			case OP_DEFINE_GLOBAL_LONG:
				THREE_BYTE_OPERAND;
			case OP_DEFINE_GLOBAL: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				krk_tableSet(frame->globals, OBJECT_VAL(name), krk_peek(0));
				krk_pop();
				break;
			}
			case OP_GET_GLOBAL_LONG:
				THREE_BYTE_OPERAND;
			case OP_GET_GLOBAL: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				KrkValue value;
				if (!krk_tableGet_fast(frame->globals, name, &value)) {
					if (!krk_tableGet_fast(&vm.builtins->fields, name, &value)) {
						krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%s'.", name->chars);
						goto _finishException;
					}
				}
				krk_push(value);
				break;
			}
			case OP_SET_GLOBAL_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_GLOBAL: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (krk_tableSet(frame->globals, OBJECT_VAL(name), krk_peek(0))) {
					krk_tableDelete(frame->globals, OBJECT_VAL(name));
					krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DEL_GLOBAL_LONG:
				THREE_BYTE_OPERAND;
			case OP_DEL_GLOBAL: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (!krk_tableDelete(frame->globals, OBJECT_VAL(name))) {
					krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%s'.", name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_IMPORT_LONG:
				THREE_BYTE_OPERAND;
			case OP_IMPORT: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (!krk_doRecursiveModuleLoad(name)) {
					goto _finishException;
				}
				break;
			}
			case OP_GET_LOCAL_LONG:
				THREE_BYTE_OPERAND;
			case OP_GET_LOCAL: {
				ONE_BYTE_OPERAND;
				krk_push(krk_currentThread.stack[frame->slots + OPERAND]);
				break;
			}
			case OP_SET_LOCAL_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_LOCAL: {
				ONE_BYTE_OPERAND;
				krk_currentThread.stack[frame->slots + OPERAND] = krk_peek(0);
				break;
			}
			case OP_SET_LOCAL_POP_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_LOCAL_POP: {
				ONE_BYTE_OPERAND;
				krk_currentThread.stack[frame->slots + OPERAND] = krk_pop();
				break;
			}
			case OP_CALL_LONG:
				THREE_BYTE_OPERAND;
			case OP_CALL: {
				ONE_BYTE_OPERAND;
				if (unlikely(!krk_callValue(krk_peek(OPERAND), OPERAND, 1))) goto _finishException;
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
				break;
			}
			case OP_CALL_METHOD_LONG:
				THREE_BYTE_OPERAND;
			case OP_CALL_METHOD: {
				ONE_BYTE_OPERAND;
				if (IS_NONE(krk_peek(OPERAND+1))) {
					if (unlikely(!krk_callValue(krk_peek(OPERAND), OPERAND, 2))) goto _finishException;
				} else {
					if (unlikely(!krk_callValue(krk_peek(OPERAND+1), OPERAND+1, 1))) goto _finishException;
				}
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
				break;
			}
			case OP_EXPAND_ARGS_LONG:
				THREE_BYTE_OPERAND;
			case OP_EXPAND_ARGS: {
				ONE_BYTE_OPERAND;
				krk_push(KWARGS_VAL(KWARGS_SINGLE-OPERAND));
				break;
			}
			case OP_CLOSURE_LONG:
				THREE_BYTE_OPERAND;
			case OP_CLOSURE: {
				ONE_BYTE_OPERAND;
				KrkCodeObject * function = AS_codeobject(READ_CONSTANT(OPERAND));
				KrkClosure * closure = krk_newClosure(function);
				krk_push(OBJECT_VAL(closure));
				for (size_t i = 0; i < closure->upvalueCount; ++i) {
					int isLocal = READ_BYTE();
					int index = READ_BYTE();
					if (isLocal & 2) {
						index = (index << 16) | (frame->ip[0] << 8) | (frame->ip[1]);
						frame->ip += 2;
					}
					if (isLocal & 1) {
						closure->upvalues[i] = captureUpvalue(frame->slots + index);
					} else {
						closure->upvalues[i] = frame->closure->upvalues[index];
					}
				}
				break;
			}
			case OP_GET_UPVALUE_LONG:
				THREE_BYTE_OPERAND;
			case OP_GET_UPVALUE: {
				ONE_BYTE_OPERAND;
				krk_push(*UPVALUE_LOCATION(frame->closure->upvalues[OPERAND]));
				break;
			}
			case OP_SET_UPVALUE_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_UPVALUE: {
				ONE_BYTE_OPERAND;
				*UPVALUE_LOCATION(frame->closure->upvalues[OPERAND]) = krk_peek(0);
				break;
			}
			case OP_CLASS_LONG:
				THREE_BYTE_OPERAND;
			case OP_CLASS: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				KrkClass * _class = krk_newClass(name, vm.baseClasses->objectClass);
				krk_push(OBJECT_VAL(_class));
				_class->filename = frame->closure->function->chunk.filename;
				krk_attachNamedObject(&_class->methods, "__func__", (KrkObj*)frame->closure);
				break;
			}
			case OP_IMPORT_FROM_LONG:
				THREE_BYTE_OPERAND;
			case OP_IMPORT_FROM: {
				ONE_BYTE_OPERAND;
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
				THREE_BYTE_OPERAND;
			case OP_GET_PROPERTY: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueGetProperty(name))) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_DEL_PROPERTY_LONG:
				THREE_BYTE_OPERAND;
			case OP_DEL_PROPERTY: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueDelProperty(name))) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_SET_PROPERTY_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_PROPERTY: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueSetProperty(name))) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(1)), name->chars);
					goto _finishException;
				}
				break;
			}
			case OP_CLASS_PROPERTY_LONG:
				THREE_BYTE_OPERAND;
			case OP_CLASS_PROPERTY: {
				ONE_BYTE_OPERAND;
				KrkValue method = krk_peek(0);
				KrkClass * _class = AS_CLASS(krk_peek(1));
				KrkValue name = OBJECT_VAL(READ_STRING(OPERAND));
				krk_tableSet(&_class->methods, name, method);
				if (AS_STRING(name) == S("__class_getitem__") && IS_CLOSURE(method)) {
					AS_CLOSURE(method)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
				}
				krk_pop();
				break;
			}
			case OP_GET_SUPER_LONG:
				THREE_BYTE_OPERAND;
			case OP_GET_SUPER: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				KrkValue baseClass = krk_peek(1);
				if (!IS_CLASS(baseClass)) {
					krk_runtimeError(vm.exceptions->typeError,
						"super() argument 1 must be class, not %s", krk_typeName(baseClass));
					goto _finishException;
				}
				if (IS_KWARGS(krk_peek(0))) {
					krk_runtimeError(vm.exceptions->notImplementedError,
						"Unbound super() reference not supported");
					goto _finishException;
				}
				if (!krk_isInstanceOf(krk_peek(0), AS_CLASS(baseClass))) {
					krk_runtimeError(vm.exceptions->typeError,
						"'%s' object is not an instance of '%s'",
						krk_typeName(krk_peek(0)), AS_CLASS(baseClass)->name->chars);
					goto _finishException;
				}
				KrkClass * superclass = AS_CLASS(baseClass)->base ? AS_CLASS(baseClass)->base : vm.baseClasses->objectClass;
				if (!krk_bindMethod(superclass, name)) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'",
						superclass->name->chars, name->chars);
					goto _finishException;
				}
				/* Swap bind and superclass */
				krk_swap(1);
				/* Pop super class */
				krk_pop();
				break;
			}
			case OP_GET_METHOD_LONG:
				THREE_BYTE_OPERAND;
			case OP_GET_METHOD: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				int result = valueGetMethod(name);
				if (result == 2) {
					krk_push(NONE_VAL());
					krk_swap(2);
					krk_pop();
				} else if (unlikely(!result)) {
					krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", krk_typeName(krk_peek(0)), name->chars);
					goto _finishException;
				} else {
					krk_swap(1); /* unbound-method object */
				}
				break;
			}
			case OP_DUP_LONG:
				THREE_BYTE_OPERAND;
			case OP_DUP:
				ONE_BYTE_OPERAND;
				krk_push(krk_peek(OPERAND));
				break;
			case OP_KWARGS_LONG:
				THREE_BYTE_OPERAND;
			case OP_KWARGS: {
				ONE_BYTE_OPERAND;
				krk_push(KWARGS_VAL(OPERAND));
				break;
			}
			case OP_CLOSE_MANY_LONG:
				THREE_BYTE_OPERAND;
			case OP_CLOSE_MANY: {
				ONE_BYTE_OPERAND;
				for (int i = 0; i < OPERAND; ++i) {
					closeUpvalues((krk_currentThread.stackTop - krk_currentThread.stack)-1);
					krk_pop();
				}
				break;
			}
			case OP_POP_MANY_LONG:
				THREE_BYTE_OPERAND;
			case OP_POP_MANY: {
				ONE_BYTE_OPERAND;
				for (int i = 0; i < OPERAND; ++i) {
					krk_pop();
				}
				break;
			}
#define doMake(func) { \
	size_t count = OPERAND; \
	KrkValue collection = krk_callNativeOnStack(count, &krk_currentThread.stackTop[-count], 0, func); \
	if (count) { \
		krk_currentThread.stackTop[-count] = collection; \
		while (count > 1) { \
			krk_pop(); count--; \
		} \
	} else { \
		krk_push(collection); \
	} \
}
			case OP_TUPLE_LONG:
				THREE_BYTE_OPERAND;
			case OP_TUPLE: {
				ONE_BYTE_OPERAND;
				doMake(krk_tuple_of);
				break;
			}
			case OP_MAKE_LIST_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_LIST: {
				ONE_BYTE_OPERAND;
				doMake(krk_list_of);
				break;
			}
			case OP_MAKE_DICT_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_DICT: {
				ONE_BYTE_OPERAND;
				doMake(krk_dict_of);
				break;
			}
			case OP_MAKE_SET_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_SET: {
				ONE_BYTE_OPERAND;
				doMake(krk_set_of);
				break;
			}
			case OP_SLICE_LONG:
				THREE_BYTE_OPERAND;
			case OP_SLICE: {
				ONE_BYTE_OPERAND;
				doMake(krk_slice_of);
				break;
			}
			case OP_LIST_APPEND_LONG:
				THREE_BYTE_OPERAND;
			case OP_LIST_APPEND: {
				ONE_BYTE_OPERAND;
				uint32_t slot = OPERAND;
				KrkValue list = krk_currentThread.stack[frame->slots + slot];
				FUNC_NAME(list,append)(2,(KrkValue[]){list,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_DICT_SET_LONG:
				THREE_BYTE_OPERAND;
			case OP_DICT_SET: {
				ONE_BYTE_OPERAND;
				uint32_t slot = OPERAND;
				KrkValue dict = krk_currentThread.stack[frame->slots + slot];
				FUNC_NAME(dict,__setitem__)(3,(KrkValue[]){dict,krk_peek(1),krk_peek(0)},0);
				krk_pop();
				krk_pop();
				break;
			}
			case OP_SET_ADD_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_ADD: {
				ONE_BYTE_OPERAND;
				uint32_t slot = OPERAND;
				KrkValue set = krk_currentThread.stack[frame->slots + slot];
				FUNC_NAME(set,add)(2,(KrkValue[]){set,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_REVERSE_LONG:
				THREE_BYTE_OPERAND;
			case OP_REVERSE: {
				ONE_BYTE_OPERAND;
				krk_push(NONE_VAL()); /* Storage space */
				for (int i = 0; i < OPERAND / 2; ++i) {
					krk_currentThread.stackTop[-1] = krk_currentThread.stackTop[-i-2];
					krk_currentThread.stackTop[-i-2] = krk_currentThread.stackTop[-(OPERAND-i)-1];
					krk_currentThread.stackTop[-(OPERAND-i)-1] = krk_currentThread.stackTop[-1];
				}
				krk_pop();
				break;
			}
			case OP_UNPACK_LONG:
				THREE_BYTE_OPERAND;
			case OP_UNPACK: {
				ONE_BYTE_OPERAND;
				size_t count = OPERAND;
				KrkValue sequence = krk_peek(0);
				/* First figure out what it is and if we can unpack it. */
#define unpackArray(counter, indexer) do { \
					if (counter != count) { \
						krk_runtimeError(vm.exceptions->valueError, "Wrong number of values to unpack (wanted %d, got %d)", (int)count, (int)counter); \
						break; \
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
						krk_runtimeError(vm.exceptions->typeError, "'%s' object is not iterable", krk_typeName(sequence));
						goto _finishException;
					} else {
						size_t stackStart = krk_currentThread.stackTop - krk_currentThread.stack - 1;
						size_t counter = 0;
						for (size_t i = 0; i < count-1; i++) {
							krk_push(NONE_VAL());
						}
						/* Create the iterator */
						krk_push(krk_currentThread.stack[stackStart]);
						krk_push(krk_callDirect(type->_iter, 1));

						do {
							/* Call it until it gives us itself */
							krk_push(krk_currentThread.stackTop[-1]);
							krk_push(krk_callStack(0));
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
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
_finishException:
			if (!handleException()) {
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
				frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(krk_peek(0));
				/* Stick the exception into the exception slot */
				if (AS_HANDLER_TYPE(krk_currentThread.stackTop[-1])== OP_FILTER_EXCEPT) {
					krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_END_FINALLY,AS_HANDLER_TARGET(krk_peek(0)));
				} else {
					krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_RAISE,AS_HANDLER_TARGET(krk_peek(0)));
				}
				krk_currentThread.stackTop[-2] = krk_currentThread.currentException;
				krk_currentThread.currentException = NONE_VAL();
			} else {
				return NONE_VAL();
			}
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
	krk_attachNamedObject(&vm.modules, name, (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__builtins__", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)krk_copyString(name,strlen(name)));
	krk_attachNamedValue(&module->fields, "__annotations__", krk_dict_of(0,NULL,0));
	return module;
}

KrkValue krk_interpret(const char * src, char * fromFile) {
	KrkCodeObject * function = krk_compile(src, fromFile);
	if (!function) {
		if (!krk_currentThread.frameCount) handleException();
		return NONE_VAL();
	}

	krk_push(OBJECT_VAL(function));
	krk_attachNamedObject(&krk_currentThread.module->fields, "__file__", (KrkObj*)function->chunk.filename);

	KrkClosure * closure = krk_newClosure(function);
	krk_pop();

	krk_push(OBJECT_VAL(closure));

	/* Quick little kludge so that empty statements return None from REPLs */
	krk_push(NONE_VAL());
	krk_pop();

	krk_callValue(OBJECT_VAL(closure), 0, 1);

	return run();
}

#ifndef NO_FILESYSTEM
KrkValue krk_runfile(const char * fileName, char * fromFile) {
	FILE * f = fopen(fileName,"r");
	if (!f) {
		fprintf(stderr, "%s: could not open file '%s': %s\n", "kuroko", fileName, strerror(errno));
		return INTEGER_VAL(errno);
	}

	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char * buf = malloc(size+1);
	if (fread(buf, 1, size, f) == 0 && size != 0) {
		fprintf(stderr, "%s: could not read file '%s': %s\n", "kuroko", fileName, strerror(errno));
		return INTEGER_VAL(errno);
	}
	fclose(f);
	buf[size] = '\0';

	KrkValue result = krk_interpret(buf, fromFile);
	free(buf);

	return result;
}

KrkValue krk_callfile(const char * fileName, char * fromFile) {
	int previousExitFrame = krk_currentThread.exitOnFrame;
	krk_currentThread.exitOnFrame = krk_currentThread.frameCount;
	KrkValue out = krk_runfile(fileName, fromFile);
	krk_currentThread.exitOnFrame = previousExitFrame;
	return out;
}
#endif
