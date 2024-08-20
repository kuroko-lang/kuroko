#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
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
#include "opcode_enum.h"

/* Ensure we don't have a macro for this so we can reference a local version. */
#undef krk_currentThread

/* This is macro'd to krk_vm for namespacing reasons. */
KrkVM vm = {0};

#ifndef KRK_DISABLE_THREADS
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

#if !defined(KRK_DISABLE_THREADS) && defined(__APPLE__) && defined(__aarch64__)
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

void krk_growStack(void) {
	size_t old = krk_currentThread.stackSize;
	size_t old_offset = krk_currentThread.stackTop - krk_currentThread.stack;
	size_t newsize = KRK_GROW_CAPACITY(old);
	if (krk_currentThread.flags & KRK_THREAD_DEFER_STACK_FREE) {
		KrkValue * newStack = KRK_GROW_ARRAY(KrkValue, NULL, 0, newsize);
		memcpy(newStack, krk_currentThread.stack, sizeof(KrkValue) * old);
		krk_currentThread.stack = newStack;
		krk_currentThread.flags &= ~(KRK_THREAD_DEFER_STACK_FREE);
	} else {
		krk_currentThread.stack = KRK_GROW_ARRAY(KrkValue, krk_currentThread.stack, old, newsize);
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
inline KrkValue krk_pop(void) {
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
 * For a class built by managed code, called by type.__new__
 */
_nonnull
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

	_class->cacheIndex = 0;

	for (struct TypeMap * entry = specials; entry->method; ++entry) {
		*entry->method = NULL;
		KrkClass * _base = _class;
		while (_base) {
			if (krk_tableGet_fast(&_base->methods, AS_STRING(vm.specialMethodNames[entry->index]), &tmp)) break;
			_base = _base->base;
		}
		if (_base && (IS_CLOSURE(tmp) || IS_NATIVE(tmp)) && (!(AS_OBJECT(tmp)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD) || entry->index == METHOD_NEW)) {
			*entry->method = AS_OBJECT(tmp);
		}
	}

	if (_class->base && _class->_eq != _class->base->_eq) {
		if (_class->_hash == _class->base->_hash) {
			_class->_hash = NULL;
			KrkValue v;
			if (!krk_tableGet_fast(&_class->methods, AS_STRING(vm.specialMethodNames[METHOD_HASH]), &v)) {
				krk_tableSet(&_class->methods, vm.specialMethodNames[METHOD_HASH], NONE_VAL());
			}
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
		[KRK_OBJ_UPVALUE]      = offsetof(struct BaseClasses, CellClass),
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
			if (IS_CLASS(of) && AS_CLASS(of)->_class) return AS_CLASS(of)->_class;
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
	int maxArgs = closure->function->potentialPositionals;
	if (unlikely(argCount < minArgs || argCount > maxArgs)) {
		krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d %sargument%s (%d given)",
		closure->function->name ? closure->function->name->chars : "<unnamed>",
		(minArgs == maxArgs) ? "exactly" : (argCount < minArgs ? "at least" : "at most"),
		(argCount < minArgs) ? minArgs : maxArgs,
		closure->function->keywordArgs ? "positional " : "",
		((argCount < minArgs) ? minArgs : maxArgs) == 1 ? "" : "s",
		argCount);
		return 0;
	}
	return 1;
}

static void multipleDefs(const KrkClosure * closure, int destination) {
	krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%S'",
		closure->function->name ? closure->function->name->chars : "<unnamed>",
		(destination < closure->function->potentialPositionals ? AS_STRING(closure->function->positionalArgNames.values[destination]) :
			(destination - closure->function->potentialPositionals < closure->function->keywordArgs ? AS_STRING(closure->function->keywordArgNames.values[destination - closure->function->potentialPositionals]) :
				S("<unnamed>"))));
}

static int _unpack_op(void * context, const KrkValue * values, size_t count) {
	KrkTuple * output = context;
	if (unlikely(output->values.count + count > output->values.capacity)) {
		krk_runtimeError(vm.exceptions->valueError, "too many values to unpack (expected %zu)",
			output->values.capacity);
		return 1;
	}
	for (size_t i = 0; i < count; ++i) {
		output->values.values[output->values.count++] = values[i];
	}
	return 0;
}

static int _unpack_args(void * context, const KrkValue * values, size_t count) {
	KrkValueArray * positionals = context;
	if (positionals->count + count > positionals->capacity) {
		size_t old = positionals->capacity;
		positionals->capacity = (count == 1) ? KRK_GROW_CAPACITY(old) : (positionals->count + count);
		positionals->values = KRK_GROW_ARRAY(KrkValue, positionals->values, old, positionals->capacity);
	}

	for (size_t i = 0; i < count; ++i) {
		positionals->values[positionals->count++] = values[i];
	}

	return 0;
}

int krk_processComplexArguments(int argCount, KrkValueArray * positionals, KrkTable * keywords, const char * name) {
#define TOP_ARGS 3
	size_t kwargsCount = AS_INTEGER(krk_currentThread.stackTop[-TOP_ARGS]);
	argCount--;

	/* First, process all the positionals, including any from extractions. */
	size_t existingPositionalArgs = argCount - kwargsCount * 2;
	for (size_t i = 0; i < existingPositionalArgs; ++i) {
		krk_writeValueArray(positionals, krk_currentThread.stackTop[-argCount + i - TOP_ARGS]);
	}

	size_t startOfExtras = &krk_currentThread.stackTop[-kwargsCount * 2 - TOP_ARGS] - krk_currentThread.stack;
	/* Now unpack everything else. */
	for (size_t i = 0; i < kwargsCount; ++i) {
		KrkValue key = krk_currentThread.stack[startOfExtras + i*2];
		KrkValue value = krk_currentThread.stack[startOfExtras + i*2 + 1];
		if (IS_KWARGS(key)) {
			if (AS_INTEGER(key) == KWARGS_LIST) { /* unpack list */
				if (krk_unpackIterable(value,positionals,_unpack_args)) return 0;
			} else if (AS_INTEGER(key) == KWARGS_DICT) { /* unpack dict */
				if (!IS_dict(value)) {
					krk_runtimeError(vm.exceptions->typeError, "%s(): **expression value is not a dict.", name);
					return 0;
				}
				for (size_t i = 0; i < AS_DICT(value)->used; ++i) {
					KrkTableEntry * entry = &AS_DICT(value)->entries[i];
					if (!IS_KWARGS(entry->key)) {
						if (!IS_STRING(entry->key)) {
							krk_runtimeError(vm.exceptions->typeError, "%s(): **expression contains non-string key", name);
							return 0;
						}
						if (!krk_tableSet(keywords, entry->key, entry->value)) {
							krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%S'", name, AS_STRING(entry->key));
							return 0;
						}
					}
				}
			} else if (AS_INTEGER(key) == KWARGS_SINGLE) { /* single value */
				krk_writeValueArray(positionals, value);
			}
		} else if (IS_STRING(key)) {
			if (!krk_tableSet(keywords, key, value)) {
				krk_runtimeError(vm.exceptions->typeError, "%s() got multiple values for argument '%S'", name, AS_STRING(key));
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
static inline int _callManaged(KrkClosure * closure, int argCount, int returnDepth) {
	size_t potentialPositionalArgs = closure->function->potentialPositionals;
	size_t totalArguments = closure->function->totalArguments;
	size_t offsetOfExtraArgs = potentialPositionalArgs;
	size_t argCountX = argCount;

	if (argCount && unlikely(IS_KWARGS(krk_currentThread.stackTop[-1]))) {

		KrkValue myList = krk_list_of(0,NULL,0);
		krk_push(myList);
		KrkValueArray * positionals;
		positionals = AS_LIST(myList);

		KrkValue myDict = krk_dict_of(0,NULL,0);
		krk_push(myDict);
		KrkTable * keywords;
		keywords = AS_DICT(myDict);

		/* This processes the existing argument list into a ValueArray and a Table with the args and keywords */
		if (!krk_processComplexArguments(argCount, positionals, keywords, closure->function->name ? closure->function->name->chars : "<unnamed>")) return 0;

		/* Store scratch while we adjust; we can not make calls while using these scratch
		 * registers as they may be clobbered by a nested call to _callManaged. */
		krk_currentThread.scratchSpace[0] = myList;
		krk_currentThread.scratchSpace[1] = myDict;

		/* Pop three things, including the kwargs count */
		krk_pop(); /* dict */
		krk_pop(); /* list */
		krk_pop(); /* kwargs */

		/* We popped the kwargs sentinel, which counted for one argCount */
		argCount--;

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

		for (size_t i = 0; i < closure->function->keywordArgs; ++i) {
			krk_push(KWARGS_VAL(0));
			argCount++;
		}

		/* We're done with the positionals */
		krk_currentThread.scratchSpace[0] = NONE_VAL();

		/* Now place keyword arguments */
		for (size_t i = 0; i < keywords->used; ++i) {
			KrkTableEntry * entry = &keywords->entries[i];
			if (!IS_KWARGS(entry->key)) {
				KrkValue name = entry->key;
				KrkValue value = entry->value;
				/* See if we can place it */
				for (int j = 0; j < (int)closure->function->potentialPositionals; ++j) {
					if (krk_valuesSame(name, closure->function->positionalArgNames.values[j])) {
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
					if (krk_valuesSame(name, closure->function->keywordArgNames.values[j])) {
						if (!IS_KWARGS(krk_currentThread.stackTop[-argCount + j + closure->function->potentialPositionals + !!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS)])) {
							multipleDefs(closure, j + closure->function->potentialPositionals);
							goto _errorAfterPositionals;
						}
						krk_currentThread.stackTop[-argCount + j + closure->function->potentialPositionals + !!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS)] = value;
						goto _finishKwarg;
					}
				}
				if (!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS)) {
					krk_runtimeError(vm.exceptions->typeError, "%s() got an unexpected keyword argument '%S'",
						closure->function->name ? closure->function->name->chars : "<unnamed>",
						AS_STRING(name));
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

		/* We're done with the keywords */
		krk_currentThread.scratchSpace[1] = NONE_VAL();

		for (size_t i = 0; i < (size_t)closure->function->requiredArgs; ++i) {
			if (IS_KWARGS(krk_currentThread.stackTop[-argCount + i])) {
				if (i < closure->function->localNameCount) {
					krk_runtimeError(vm.exceptions->typeError, "%s() %s: '%S'",
						closure->function->name ? closure->function->name->chars : "<unnamed>",
						"missing required positional argument",
						closure->function->localNames[i].name);
				} else {
					krk_runtimeError(vm.exceptions->typeError, "%s() %s",
						closure->function->name ? closure->function->name->chars : "<unnamed>",
						"missing required positional argument");
				}
				goto _errorAfterKeywords;
			}
		}

		argCountX = argCount - closure->function->keywordArgs - (!!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) + !!(closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS));
	} else if ((size_t)argCount > potentialPositionalArgs && (closure->function->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS)) {
		KrkValue * startOfPositionals = &krk_currentThread.stackTop[-argCount];
		KrkValue tmp = krk_callNativeOnStack(argCount - potentialPositionalArgs,
			&startOfPositionals[potentialPositionalArgs], 0, krk_list_of);
		startOfPositionals = &krk_currentThread.stackTop[-argCount];
		startOfPositionals[offsetOfExtraArgs] = tmp;
		argCount = potentialPositionalArgs + 1;
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

	if (unlikely(krk_currentThread.frameCount == (size_t)krk_currentThread.maximumCallDepth)) {
		krk_runtimeError(vm.exceptions->baseException, "maximum recursion depth exceeded");
		goto _errorAfterKeywords;
	}

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = (krk_currentThread.stackTop - argCount) - krk_currentThread.stack;
	frame->outSlots = frame->slots - returnDepth;
	frame->globalsOwner = closure->globalsOwner;
	frame->globals = closure->globalsTable;
	return 1;

_errorDuringPositionals:
	krk_currentThread.scratchSpace[0] = NONE_VAL();
_errorAfterPositionals:
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
	size_t sizeBefore  = krk_currentThread.stackSize;
	void * stackBefore = krk_currentThread.stack;
	KrkValue result = native(argCount, stackArgs, hasKw);

	if (unlikely(krk_currentThread.stack != stackBefore)) {
		KRK_FREE_ARRAY(KrkValue, stackBefore, sizeBefore);
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

static inline int _callNative(KrkNative* callee, int argCount, int returnDepth) {
	NativeFn native = (NativeFn)callee->function;
	size_t stackOffsetAfterCall = (krk_currentThread.stackTop - krk_currentThread.stack) - argCount - returnDepth;
	KrkValue result;
	if (unlikely(argCount && IS_KWARGS(krk_currentThread.stackTop[-1]))) {
		/* Prep space for our list + dictionary */
		KrkValue myList = krk_list_of(0,NULL,0);
		krk_push(myList);
		KrkValue myDict = krk_dict_of(0,NULL,0);
		krk_push(myDict);

		/* Parse kwargs stuff into the list+dict; note, this no longer pops anything, and expects
		 * our list + dict to be at the top: [kwargs] [list] [dict] */
		if (unlikely(!krk_processComplexArguments(argCount, AS_LIST(myList), AS_DICT(myDict), callee->name))) return 0;

		/* Write the dict into the list */
		krk_writeValueArray(AS_LIST(myList), myDict);

		/* Also add a list for storing references that get removed from the kwargs dict during mutation. */
		KrkValue refList = krk_list_of(0,NULL,0);
		krk_push(refList);
		krk_writeValueArray(AS_LIST(myList), refList);

		/* Reduce the stack to just the list */
		krk_currentThread.stack[stackOffsetAfterCall] = myList;
		krk_currentThread.stackTop = &krk_currentThread.stack[stackOffsetAfterCall+1];

		/* Call with list as arguments */
		result = native(AS_LIST(myList)->count-2, AS_LIST(myList)->values, 1);
	} else {
		result = krk_callNativeOnStack(argCount, krk_currentThread.stackTop - argCount, 0, native);
	}
	krk_currentThread.stackTop = &krk_currentThread.stack[stackOffsetAfterCall];
	krk_push(result);
	return 2;
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
			case KRK_OBJ_CLOSURE: return _callManaged(AS_CLOSURE(callee), argCount, returnDepth);
			case KRK_OBJ_NATIVE: return _callNative(AS_NATIVE(callee), argCount, returnDepth);
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
			default: {
				KrkClass * _class = krk_getType(callee);
				if (likely(_class->_call != NULL)) {
					if (unlikely(returnDepth == 0)) _rotate(argCount);
					krk_currentThread.stackTop[-argCount - 1] = callee;
					argCount++;
					returnDepth = returnDepth ? (returnDepth - 1) : 0;
					return (_class->_call->type == KRK_OBJ_CLOSURE) ? _callManaged((KrkClosure*)_class->_call, argCount, returnDepth) : _callNative((KrkNative*)_class->_call, argCount, returnDepth);
				} else {
					krk_runtimeError(vm.exceptions->typeError, "'%T' object is not callable", callee);
					return 0;
				}
			}
		}
	}
	krk_runtimeError(vm.exceptions->typeError, "'%T' object is not callable", callee);
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
	int result = 0;
	switch (callable->type) {
		case KRK_OBJ_CLOSURE: result = _callManaged((KrkClosure*)callable, argCount, 0); break;
		case KRK_OBJ_NATIVE: result = _callNative((KrkNative*)callable, argCount, 0); break;
		default: __builtin_unreachable();
	}
	if (likely(result == 2)) return krk_pop();
	else if (result == 1) return krk_runNext();
	return NONE_VAL();
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
#ifndef KRK_NO_FLOAT
			if (IS_FLOATING(value)) return !AS_FLOATING(value);
#endif
			break;
	}
	KrkClass * type = krk_getType(value);

	if (type->_bool) {
		krk_push(value);
		KrkValue result = krk_callDirect(type->_bool,1);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return 1;
		if (!IS_BOOLEAN(result)) {
			krk_runtimeError(vm.exceptions->typeError, "__bool__ should return bool, not %T", result);
			return 1;
		}
		return !AS_INTEGER(result);
	}

	/* If it has a length, and that length is 0, it's Falsey */
	if (type->_len) {
		krk_push(value);
		KrkValue result = krk_callDirect(type->_len,1);
		return !AS_INTEGER(result);
	}
	return 0; /* Assume anything else is truthy */
}

void krk_setMaximumRecursionDepth(size_t maxDepth) {
	krk_currentThread.maximumCallDepth = maxDepth;
	krk_currentThread.frames = realloc(krk_currentThread.frames, maxDepth * sizeof(KrkCallFrame));
}

void krk_initVM(int flags) {
#if !defined(KRK_DISABLE_THREADS) && defined(__APPLE__) && defined(__aarch64__)
	krk_forceThreadData();
#endif

	vm.globalFlags = flags & 0xFF00;

	/* Reset current thread */
	krk_resetStack();
	krk_currentThread.maximumCallDepth = KRK_CALL_FRAMES_MAX;
	krk_currentThread.frames   = calloc(krk_currentThread.maximumCallDepth,sizeof(KrkCallFrame));
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
	vm.exceptions = calloc(1,sizeof(struct Exceptions));
	vm.baseClasses = calloc(1,sizeof(struct BaseClasses));
	vm.specialMethodNames = calloc(METHOD__MAX,sizeof(KrkValue));
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
	_createAndBind_longClass();
	_createAndBind_compilerClass();

	if (!(vm.globalFlags & KRK_GLOBAL_NO_DEFAULT_MODULES)) {
#ifndef KRK_NO_SYSTEM_MODULES
		krk_module_init_kuroko();
#endif
#ifndef KRK_DISABLE_THREADS
		krk_module_init_threading();
#endif
	}

#ifndef KRK_DISABLE_DEBUG
	krk_debug_init();
#endif


	/* The VM is now ready to start executing code. */
	krk_resetStack();
}

/**
 * Reclaim resources used by the VM.
 */
void krk_freeVM(void) {
	krk_freeTable(&vm.strings);
	krk_freeTable(&vm.modules);
	if (vm.specialMethodNames) free(vm.specialMethodNames);
	if (vm.exceptions) free(vm.exceptions);
	if (vm.baseClasses) free(vm.baseClasses);
	krk_freeObjects();

	if (vm.binpath) free(vm.binpath);
	if (vm.dbgState) free(vm.dbgState);

	while (krk_currentThread.next) {
		KrkThreadState * thread = krk_currentThread.next;
		krk_currentThread.next = thread->next;
		KRK_FREE_ARRAY(size_t, thread->stack, thread->stackSize);
		free(thread->frames);
	}

	KRK_FREE_ARRAY(size_t, krk_currentThread.stack, krk_currentThread.stackSize);
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

static int _try_op(size_t methodOffset, KrkValue a, KrkValue b, KrkValue *out) {
	KrkClass * type = krk_getType(a);
	KrkObj * method = *(KrkObj**)((char*)type + methodOffset);
	if (likely(method != NULL)) {
		krk_push(a);
		krk_push(b);
		KrkValue result = krk_callDirect(method, 2);
		if (likely(!IS_NOTIMPL(result))) {
			*out = result;
			return 1;
		}
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
			*out = NONE_VAL();
			return 1;
		}
	}
	return 0;
}

static KrkValue _bin_op(size_t methodOffset, size_t invOffset, const char * operator, KrkValue a, KrkValue b) {
	KrkValue result;
	if (_try_op(methodOffset, a, b, &result)) return result;
	if (_try_op(invOffset, b, a, &result)) return result;
	return krk_runtimeError(vm.exceptions->typeError,
		"unsupported operand types for %s: '%T' and '%T'",
		operator, a, b);
}

#define MAKE_COMPARE_OP(name,operator,inv) \
	_protected KrkValue krk_operator_ ## name (KrkValue a, KrkValue b) { \
		return _bin_op(offsetof(KrkClass,_ ## name),offsetof(KrkClass,_ ## inv), operator, a, b); \
	}
#define MAKE_BIN_OP(name,operator,inv) \
	MAKE_COMPARE_OP(name,operator,inv) \
	_protected KrkValue krk_operator_i ## name (KrkValue a, KrkValue b) { \
		KrkValue result; \
		if (_try_op(offsetof(KrkClass,_i ## name), a, b, &result)) return result; \
		return krk_operator_ ## name(a,b); \
	}

MAKE_BIN_OP(add,"+",radd)
MAKE_BIN_OP(sub,"-",rsub)
MAKE_BIN_OP(mul,"*",rmul)
MAKE_BIN_OP(pow,"**",rpow)
MAKE_BIN_OP(or,"|",ror)
MAKE_BIN_OP(xor,"^",rxor)
MAKE_BIN_OP(and,"&",rand)
MAKE_BIN_OP(lshift,"<<",rlshift)
MAKE_BIN_OP(rshift,">>",rrshift)
MAKE_BIN_OP(mod,"%",rmod)
MAKE_BIN_OP(truediv,"/",rtruediv)
MAKE_BIN_OP(floordiv,"//",rfloordiv)
MAKE_BIN_OP(matmul,"@",rmatmul)

MAKE_COMPARE_OP(lt, "<", gt)
MAKE_COMPARE_OP(gt, ">", lt)
MAKE_COMPARE_OP(le, "<=", ge)
MAKE_COMPARE_OP(ge, ">=", le)

_protected
KrkValue krk_operator_eq(KrkValue a, KrkValue b) {
	return BOOLEAN_VAL(krk_valuesEqual(a,b));
}

_protected
KrkValue krk_operator_is(KrkValue a, KrkValue b) {
	return BOOLEAN_VAL(krk_valuesSame(a,b));
}

static KrkValue _unary_op(size_t methodOffset, const char * operator, KrkValue value) {
	KrkClass * type = krk_getType(value);
	KrkObj * method = *(KrkObj**)((char*)type + methodOffset);
	if (likely(method != NULL)) {
		krk_push(value);
		return krk_callDirect(method, 1);
	}
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
	return krk_runtimeError(vm.exceptions->typeError, "bad operand type for unary %s: '%T'", operator, value);
}

#define MAKE_UNARY_OP(sname,operator,op) \
	_protected KrkValue krk_operator_ ## operator (KrkValue value) { \
		return _unary_op(offsetof(KrkClass,sname),#op,value); \
	}

MAKE_UNARY_OP(_invert,invert,~)
MAKE_UNARY_OP(_negate,neg,-)
MAKE_UNARY_OP(_pos,pos,+)

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
static int handleException(void) {
	int stackOffset, frameOffset;
	int exitSlot = (krk_currentThread.exitOnFrame >= 0) ? krk_currentThread.frames[krk_currentThread.exitOnFrame].outSlots : 0;
	for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1);
		stackOffset >= exitSlot &&
		!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset], OP_PUSH_TRY) &&
		!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset], OP_PUSH_WITH) &&
		!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset], OP_FILTER_EXCEPT) &&
		!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset], OP_RAISE) &&
		!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset], OP_END_FINALLY)
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
		}
		krk_currentThread.frameCount = krk_currentThread.exitOnFrame;

		/* Ensure stack is in the expected place, as if we returned None. */
		closeUpvalues(exitSlot);
		krk_currentThread.stackTop = &krk_currentThread.stack[exitSlot];

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
	/* See if the module is already loaded */
	if (krk_tableGet_fast(&vm.modules, runAs, moduleOut)) {
		krk_push(*moduleOut);
		return 1;
	}

#ifndef KRK_NO_FILESYSTEM
	KrkValue modulePaths;

	/* Obtain __builtins__.module_paths */
	if (!vm.system || !krk_tableGet_fast(&vm.system->fields, S("module_paths"), &modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions->importError, "kuroko.module_paths not defined.");
		return 0;
	}

	if (!IS_list(modulePaths)) {
		*moduleOut = NONE_VAL();
		krk_runtimeError(vm.exceptions->importError, "kuroko.module_paths must be a list, not '%T'", modulePaths);
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
				"Module search path must be str, not '%T'", krk_peek(0));
			return 0;
		}

		/* Try .../path/__init__.krk */
		krk_push(OBJECT_VAL(path));
		krk_addObjects();
		krk_push(OBJECT_VAL(S(KRK_PATH_SEP "__init__.krk")));
		krk_addObjects();
		fileName = AS_CSTRING(krk_peek(0));
		if (stat(fileName,&statbuf) == 0) {
			isPackage = 1;
			if (runAs == S("__main__")) {
				krk_pop(); /* concatenated name */

				/* Convert back to .-formatted */
				krk_push(krk_valueGetAttribute(OBJECT_VAL(path), "replace"));
				krk_push(OBJECT_VAL(S(KRK_PATH_SEP)));
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
					krk_runtimeError(vm.exceptions->importError, "No module named '%S'; '%S' is a package and cannot be executed directly",
						AS_STRING(dotted_main), AS_STRING(packageName));
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

#ifndef KRK_STATIC_ONLY
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
		krk_runfile(fileName,fileName);
		*moduleOut = OBJECT_VAL(krk_currentThread.module);
		krk_currentThread.module = enclosing;
		if (!IS_OBJECT(*moduleOut) || (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
			if (!(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
				krk_runtimeError(vm.exceptions->importError,
					"Failed to load module '%S' from '%s'", runAs, fileName);
			}
			krk_tableDelete(&vm.modules, OBJECT_VAL(runAs));
			return 0;
		}

		krk_pop(); /* concatenated filename on stack */
		krk_push(*moduleOut);
		return 1;

#ifndef KRK_STATIC_ONLY
	_sharedObject: (void)0;

		krk_dlRefType dlRef = krk_dlOpen(fileName);
		if (!dlRef) {
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->importError,
				"Failed to load native module '%S' from shared object '%s'", runAs, fileName);
			return 0;
		}

		const char * start = path->chars;
		for (const char * c = start; *c; c++) {
			if (*c == '/') start = c + 1;
		}

		krk_push(OBJECT_VAL(S("krk_module_onload_")));
		krk_push(OBJECT_VAL(krk_copyString(start,strlen(start))));
		krk_addObjects();

		char * handlerName = AS_CSTRING(krk_peek(0));

		KrkValue (*moduleOnLoad)(KrkString * name);
		krk_dlSymType out = krk_dlSym(dlRef, handlerName);
		memcpy(&moduleOnLoad,&out,sizeof(out));

		if (!moduleOnLoad) {
			krk_dlClose(dlRef);
			*moduleOut = NONE_VAL();
			krk_runtimeError(vm.exceptions->importError,
				"Failed to run module initialization method '%s' from shared object '%s'",
				handlerName, fileName);
			return 0;
		}

		krk_pop(); /* onload function */

		*moduleOut = moduleOnLoad(runAs);
		if (!krk_isInstanceOf(*moduleOut, vm.baseClasses->moduleClass)) {
			krk_dlClose(dlRef);
			krk_runtimeError(vm.exceptions->importError,
				"Failed to load module '%S' from '%s'", runAs, fileName);
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
		krk_push(OBJECT_VAL(S(KRK_PATH_SEP)));
		krk_push(OBJECT_VAL(S(".")));
		krk_push(krk_callStack(2));
	} else {
		krk_push(OBJECT_VAL(runAs));
	}

	krk_runtimeError(vm.exceptions->importError, "No module named '%S'", AS_STRING(krk_peek(0)));

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

		krk_push(finishStringBuilder(&sb));

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
				krk_runtimeError(vm.exceptions->importError, "'%S' is not a package", AS_STRING(krk_currentThread.stack[argBase+2]));
				return 0;
			}
			krk_currentThread.stack[argBase-1] = krk_pop();
			/* Now concatenate forward slash... */
			krk_push(krk_currentThread.stack[argBase+1]); /* Slash path */
			krk_push(OBJECT_VAL(S(KRK_PATH_SEP)));
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

#define CACHE_SIZE 4096
typedef struct {
	KrkString * name;
	struct KrkClass  * owner;
	KrkValue    value;
	size_t index;
} KrkClassCacheEntry;
static KrkClassCacheEntry cache[CACHE_SIZE] = {0};
static size_t nextCount = 1;

static KrkClass * checkCache(KrkClass * type, KrkString * name, KrkValue * method) {
	size_t index = (name->obj.hash ^ (type->obj.hash << 4)) & (CACHE_SIZE-1);
	KrkClassCacheEntry * entry = &cache[index];
	if (entry->name == name && entry->index == type->cacheIndex) {
		*method = entry->value;
		return entry->owner;
	}

	KrkClass * _class = NULL;
	if (krk_tableGet_fast(&type->methods, name, method)) {
		_class = type;
	} else if (type->base) {
		_class = checkCache(type->base, name, method);
	}

	if (!type->cacheIndex) {
		type->cacheIndex = nextCount++;
	}
	entry->name = name;
	entry->owner = _class;
	entry->value = *method;
	entry->index = type->cacheIndex;
	return _class;
}

static void clearCache(KrkClass * type) {
	if (type->cacheIndex) {
		type->cacheIndex = 0;
		for (size_t i = 0; i < type->subclasses.capacity; ++i) {
			KrkTableEntry * entry = &type->subclasses.entries[i];
			if (krk_valuesSame(entry->key, KWARGS_VAL(0))) continue;
			clearCache(AS_CLASS(entry->key));
		}
	}
}

/**
 * Attach a method call to its callee and return a BoundMethod.
 * Works for managed and native method calls.
 */
int krk_bindMethodSuper(KrkClass * originalClass, KrkString * name, KrkClass * realClass) {
	KrkValue method, out;
	KrkClass * _class = checkCache(originalClass, name, &method);
	if (!_class) return 0;
	if (IS_NATIVE(method)||IS_CLOSURE(method)) {
		if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD) {
			out = OBJECT_VAL(krk_newBoundMethod(OBJECT_VAL(realClass), AS_OBJECT(method)));
		} else if (IS_NONE(krk_peek(0)) || (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD)) {
			out = method;
		} else {
			out = OBJECT_VAL(krk_newBoundMethod(krk_peek(0), AS_OBJECT(method)));
		}
	} else {
		/* Does it have a descriptor __get__? */
		KrkClass * type = krk_getType(method);
		if (type->_descget) {
			krk_push(method);
			krk_swap(1);
			krk_push(OBJECT_VAL(realClass));
			krk_push(krk_callDirect(type->_descget, 3));
			return 1;
		}
		out = method;
	}
	krk_pop();
	krk_push(out);
	return 1;
}

int krk_bindMethod(KrkClass * originalClass, KrkString * name) {
	return krk_bindMethodSuper(originalClass,name,originalClass);
}

static int valueGetMethod(KrkString * name) {
	KrkValue this = krk_peek(0);
	KrkClass * myClass = krk_getType(this);
	KrkValue value, method;
	KrkClass * _class = checkCache(myClass, name, &method);

	/* Class descriptors */
	if (_class) {
		KrkClass * valtype = krk_getType(method);
		if (valtype->_descget && valtype->_descset) {
			krk_push(method);
			krk_push(this);
			krk_push(OBJECT_VAL(myClass));
			value = krk_callDirect(valtype->_descget, 3);
			goto found;
		}
	}

	/* Fields */
	if (IS_INSTANCE(this)) {
		if (krk_tableGet_fast(&AS_INSTANCE(this)->fields, name, &value)) goto found;
	} else if (IS_CLASS(this)) {
		KrkClass * type = AS_CLASS(this);
		do {
			if (krk_tableGet_fast(&type->methods, name, &value)) {
				if ((IS_NATIVE(value) || IS_CLOSURE(value)) && (AS_OBJECT(value)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD)) {
					goto found_method;
				}
				KrkClass * valtype = krk_getType(value);
				if (valtype->_descget) {
					krk_push(value);
					krk_push(NONE_VAL());
					krk_push(this);
					value = krk_callDirect(valtype->_descget, 3);
				}
				goto found;
			}
			type = type->base;
		} while (type);
	} else if (IS_CLOSURE(this)) {
		if (krk_tableGet_fast(&AS_CLOSURE(this)->fields, name, &value)) goto found;
	}

	/* Method from type */
	if (_class) {
		if (IS_NATIVE(method)||IS_CLOSURE(method)) {
			if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD) {
				krk_currentThread.stackTop[-1] = OBJECT_VAL(myClass);
				value = method;
				goto found_method;
			} else if (AS_OBJECT(method)->flags & KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD) {
				value = method;
			} else {
				value = method;
				goto found_method;
			}
		} else {
			KrkClass * valtype = krk_getType(method);
			if (valtype->_descget) {
				krk_push(method);
				krk_push(this);
				krk_push(OBJECT_VAL(myClass));
				value = krk_callDirect(valtype->_descget, 3);
				goto found;
			}
			value = method;
		}
		goto found;
	}

	/* __getattr__ */
	if (myClass->_getattr) {
		krk_push(this);
		krk_push(OBJECT_VAL(name));
		value = krk_callDirect(myClass->_getattr, 2);
		goto found;
	}

	return 0;

found:
	krk_push(value);
	return 2;

found_method:
	krk_push(value);
	return 1;
}

static int valueGetProperty(KrkString * name) {
	switch (valueGetMethod(name)) {
		case 2:
			krk_currentThread.stackTop[-2] = krk_currentThread.stackTop[-1];
			krk_currentThread.stackTop--;
			return 1;
		case 1: {
			KrkValue o = OBJECT_VAL(krk_newBoundMethod(krk_currentThread.stackTop[-2], AS_OBJECT(krk_currentThread.stackTop[-1])));
			krk_currentThread.stackTop[-2] = o;
			krk_currentThread.stackTop--;
			return 1;
		}
		default:
			return 0;
	}
}

int krk_getAttribute(KrkString * name) {
	return valueGetProperty(name);
}

KrkValue krk_valueGetAttribute(KrkValue value, char * name) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(value);
	if (!valueGetProperty(AS_STRING(krk_peek(1)))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%s'", krk_peek(0), name);
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
		} else {
			clearCache(_class);
		}
		krk_pop(); /* the original value */
		return 1;
	} else if (IS_CLOSURE(krk_peek(0))) {
		KrkClosure * closure = AS_CLOSURE(krk_peek(0));
		if (!krk_tableDelete(&closure->fields, OBJECT_VAL(name))) {
			return 0;
		}
		krk_pop();
		return 1;
	}
	/* TODO __delattr__? Descriptor __delete__ methods? */
	return 0;
}

int krk_delAttribute(KrkString * name) {
	return valueDelProperty(name);
}

KrkValue krk_valueDelAttribute(KrkValue owner, char * name) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(owner);
	if (!valueDelProperty(AS_STRING(krk_peek(1)))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%s'", krk_peek(0), name);
	}
	krk_pop(); /* String */
	return NONE_VAL();
}

static int _setDescriptor(KrkValue owner, KrkClass * _class, KrkString * name, KrkValue to) {
	KrkValue property;
	_class = checkCache(_class, name, &property);
	if (_class) {
		KrkClass * type = krk_getType(property);
		if (type->_descset) {
			krk_push(property);
			krk_push(owner);
			krk_push(to);
			krk_push(krk_callDirect(type->_descset, 3));
			return 1;
		}
	}
	return 0;
}

static KrkValue setAttr_wrapper(KrkValue owner, KrkClass * _class, KrkTable * fields, KrkString * name, KrkValue to) {
	if (_setDescriptor(owner,_class,name,to)) return krk_pop();
	krk_tableSet(fields, OBJECT_VAL(name), to);
	return to;
}

_noexport
KrkValue krk_instanceSetAttribute_wrapper(KrkValue owner, KrkString * name, KrkValue to) {
	return setAttr_wrapper(owner, AS_INSTANCE(owner)->_class, &AS_INSTANCE(owner)->fields, name, to);
}

static int valueSetProperty(KrkString * name) {
	KrkValue owner = krk_peek(1);
	KrkValue value = krk_peek(0);
	KrkClass * type = krk_getType(owner);
	if (unlikely(type->_setattr != NULL)) {
		krk_push(OBJECT_VAL(name));
		krk_swap(1);
		krk_push(krk_callDirect(type->_setattr, 3));
		return 1;
	}
	if (IS_INSTANCE(owner)) {
		KrkValue o = setAttr_wrapper(owner,type,&AS_INSTANCE(owner)->fields, name, value);
		krk_currentThread.stackTop[-1] = o;
	} else if (IS_CLASS(owner)) {
		KrkValue o = setAttr_wrapper(owner,type,&AS_CLASS(owner)->methods, name, value);
		krk_currentThread.stackTop[-1] = o;
		if (name->length > 1 && name->chars[0] == '_' && name->chars[1] == '_') {
			krk_finalizeClass(AS_CLASS(owner));
		} else {
			clearCache(AS_CLASS(owner));
		}
	} else if (IS_CLOSURE(owner)) {
		KrkValue o = setAttr_wrapper(owner,type,&AS_CLOSURE(owner)->fields, name, value);
		krk_currentThread.stackTop[-1] = o;
	} else {
		if (_setDescriptor(owner,type,name,value)) {
			krk_swap(1);
			krk_pop();
		} else {
			return 0;
		}
	}
	krk_swap(1);
	krk_pop();
	return 1;
}

int krk_setAttribute(KrkString * name) {
	return valueSetProperty(name);
}

KrkValue krk_valueSetAttribute(KrkValue owner, char * name, KrkValue to) {
	krk_push(OBJECT_VAL(krk_copyString(name,strlen(name))));
	krk_push(owner);
	krk_push(to);
	if (!valueSetProperty(AS_STRING(krk_peek(2)))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%s'", krk_peek(1), name);
	}
	krk_swap(1);
	krk_pop(); /* String */
	return krk_pop();
}

#define BINARY_OP(op) { KrkValue b = krk_peek(0); KrkValue a = krk_peek(1); \
	a = krk_operator_ ## op (a,b); \
	krk_currentThread.stackTop[-2] = a; krk_pop(); break; }
#define INPLACE_BINARY_OP(op) { KrkValue b = krk_peek(0); KrkValue a = krk_peek(1); \
	a = krk_operator_i ## op (a,b); \
	krk_currentThread.stackTop[-2] = a; krk_pop(); break; }

extern KrkValue krk_int_op_add(krk_integer_type a, krk_integer_type b);
extern KrkValue krk_int_op_sub(krk_integer_type a, krk_integer_type b);

/* These operations are most likely to occur on integers, so we special case them */
#define LIKELY_INT_BINARY_OP(op) { KrkValue b = krk_peek(0); KrkValue a = krk_peek(1); \
	if (likely(IS_INTEGER(a) && IS_INTEGER(b))) a = krk_int_op_ ## op (AS_INTEGER(a), AS_INTEGER(b)); \
	else a = krk_operator_ ## op (a,b); \
	krk_currentThread.stackTop[-2] = a; krk_pop(); break; }

/* Comparators like these are almost definitely going to happen on integers. */
#define LIKELY_INT_COMPARE_OP(op,operator) { KrkValue b = krk_peek(0); KrkValue a = krk_peek(1); \
	if (likely(IS_INTEGER(a) && IS_INTEGER(b))) a = BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
	else a = krk_operator_ ## op (a,b); \
	krk_currentThread.stackTop[-2] = a; krk_pop(); break; }

#define LIKELY_INT_UNARY_OP(op,operator) { KrkValue a = krk_peek(0); \
	if (likely(IS_INTEGER(a))) a = INTEGER_VAL(operator AS_INTEGER(a)); \
	else a = krk_operator_ ## op (a); \
	krk_currentThread.stackTop[-1] = a; break; }

#define READ_BYTE() (*frame->ip++)
#define READ_CONSTANT(s) (frame->closure->function->chunk.constants.values[OPERAND])
#define READ_STRING(s) AS_STRING(READ_CONSTANT(s))

extern FUNC_SIG(list,append);
extern FUNC_SIG(dict,__setitem__);
extern FUNC_SIG(set,add);
extern FUNC_SIG(list,extend);
extern FUNC_SIG(dict,update);
extern FUNC_SIG(set,update);

struct ex_unpack {
	KrkTuple * output;
	unsigned char before;
	unsigned char after;
	KrkValue list;
	size_t total;
};

static int _unpack_ex(void * context, const KrkValue * values, size_t count) {
	struct ex_unpack * ctx = context;

	KrkTuple * output = ctx->output;

	for (size_t i = 0; i < count; ++i) {
		if (ctx->total < ctx->before) {
			output->values.values[output->values.count++] = values[i];
		} else if (ctx->total >= ctx->before) {
			if (ctx->total == ctx->before) {
				output->values.values[output->values.count++] = ctx->list;
			}
			FUNC_NAME(list,append)(2,(KrkValue[]){ctx->list,values[i]},0);
		}
		ctx->total++;
	}

	return 0;
}


static inline void makeCollection(NativeFn func, size_t count) {
	KrkValue collection = krk_callNativeOnStack(count, &krk_currentThread.stackTop[-count], 0, func);
	if (count) {
		krk_currentThread.stackTop[-count] = collection;
		while (count > 1) {
			krk_pop();
			count--;
		}
	} else {
		krk_push(collection);
	}
}

static inline int doFormatString(int options) {
	if (options & FORMAT_OP_FORMAT) {
		krk_swap(1);
		if (options & FORMAT_OP_EQ) {
			krk_swap(2);
		}
	} else if (options & FORMAT_OP_EQ) {
		krk_swap(1);
	}

	/* Was this a repr or str call? (it can't be both) */
	if (options & FORMAT_OP_STR) {
		KrkClass * type = krk_getType(krk_peek(0));
		if (likely(type->_tostr != NULL)) {
			krk_push(krk_callDirect(type->_tostr, 1));
			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
		} else {
			krk_runtimeError(vm.exceptions->typeError,
				"Can not convert '%T' to str", krk_peek(0));
			return 1;
		}
	} else if (options & FORMAT_OP_REPR) {
		KrkClass * type = krk_getType(krk_peek(0));
		if (likely(type->_reprer != NULL)) {
			krk_push(krk_callDirect(type->_reprer, 1));
			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
		} else {
			krk_runtimeError(vm.exceptions->typeError,
				"Can not repr '%T'", krk_peek(0));
			return 1;
		}
	}

	if (!(options & FORMAT_OP_FORMAT)) {
		/* Push empty string */
		krk_push(OBJECT_VAL(S("")));
	} else {
		/* Swap args so value is first */
		krk_swap(1);
	}

	/* Get the type of the value */
	KrkClass * type = krk_getType(krk_peek(1));

	if (likely(type->_format != NULL)) {
		krk_push(krk_callDirect(type->_format, 2));
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
	} else {
		krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%s'", krk_peek(1), "__format__");
		return 1;
	}

	if (!IS_STRING(krk_peek(0))) {
		krk_runtimeError(vm.exceptions->typeError, "format result is not str");
		return 1;
	}

	return 0;
}

static inline void commonMethodInvoke(size_t methodOffset, int args, const char * msgFormat) {
	KrkClass * type = krk_getType(krk_peek(args-1));
	KrkObj * method = *(KrkObj**)((char*)type + methodOffset);
	if (likely(method != NULL)) {
		krk_push(krk_callDirect(method, args));
	} else {
		krk_runtimeError(vm.exceptions->attributeError, msgFormat, krk_peek(args-1));
	}
}

int krk_isSubClass(const KrkClass * cls, const KrkClass * base) {
	while (cls) {
		if (cls == base) return 1;
		cls = cls->base;
	}
	return 0;
}

/**
 * VM main loop.
 */
static KrkValue run(void) {
	KrkCallFrame* frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];

	while (1) {
		if (unlikely(krk_currentThread.flags & (KRK_THREAD_ENABLE_TRACING | KRK_THREAD_SINGLE_STEP | KRK_THREAD_SIGNALLED))) {
#if !defined(KRK_NO_TRACING) && !defined(KRK_DISABLE_DEBUG)
			if (krk_currentThread.flags & KRK_THREAD_ENABLE_TRACING) {
				krk_debug_dumpStack(stderr, frame);
				krk_disassembleInstruction(stderr, frame->closure->function,
					(size_t)(frame->ip - frame->closure->function->chunk.code));
			}
#endif

#ifndef KRK_DISABLE_DEBUG
			if (krk_currentThread.flags & KRK_THREAD_SINGLE_STEP) {
				krk_debuggerHook(frame);
			}
#endif

			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) {
				krk_currentThread.flags &= ~(KRK_THREAD_SIGNALLED); /* Clear signal flag */
				krk_runtimeError(vm.exceptions->keyboardInterrupt, "Keyboard interrupt.");
				goto _finishException;
			}
		}
#ifndef KRK_DISABLE_DEBUG
_resumeHook: (void)0;
#endif

		/* Each instruction begins with one opcode byte */
		KrkOpCode opcode = READ_BYTE();
		unsigned int OPERAND = 0;

/* Only GCC lets us put these on empty statements; just hope clang doesn't start complaining */
#if defined(__GNUC__) && !defined(__clang__)
# define FALLTHROUGH __attribute__((fallthrough));
#else
# define FALLTHROUGH
#endif

#define TWO_BYTE_OPERAND { OPERAND = OPERAND | (frame->ip[0] << 8) | frame->ip[1]; frame->ip += 2; }
#define THREE_BYTE_OPERAND { OPERAND = (frame->ip[0] << 16) | (frame->ip[1] << 8); frame->ip += 2; } FALLTHROUGH
#define ONE_BYTE_OPERAND { OPERAND = (OPERAND & ~0xFF) | READ_BYTE(); }

_switchEntry: (void)0;
		switch (opcode) {
			case OP_CLEANUP_WITH: {
				/* Top of stack is a HANDLER that should have had something loaded into it if it was still valid */
				KrkValue handler = krk_peek(0);
				KrkValue exceptionObject = krk_peek(1);
				KrkValue contextManager = krk_peek(2);
				KrkClass * type = krk_getType(contextManager);
				krk_push(contextManager);
				if (AS_HANDLER_TYPE(handler) == OP_RAISE) {
					krk_currentThread.stackTop[-2] = HANDLER_VAL(OP_CLEANUP_WITH,AS_HANDLER_TARGET(krk_peek(1)));
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
				if (AS_HANDLER_TYPE(handler) == OP_EXIT_LOOP) {
					frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(handler);
					OPERAND = AS_INTEGER(krk_peek(1));
					goto _finishPopBlock;
				}
				if (AS_HANDLER_TYPE(handler) != OP_RETURN) break;
				krk_pop(); /* handler */
			} /* fallthrough */
			case OP_RETURN: {
_finishReturn: (void)0;
				KrkValue result = krk_pop();
				/* See if this frame had a thing */
				int stackOffset;
				for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1);
					stackOffset >= (int)frame->slots && 
					!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset],OP_PUSH_TRY) &&
					!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset],OP_PUSH_WITH) &&
					!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset],OP_FILTER_EXCEPT)
					; stackOffset--);
				if (stackOffset >= (int)frame->slots) {
					closeUpvalues(stackOffset);
					krk_currentThread.stackTop = &krk_currentThread.stack[stackOffset + 1];
					frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(krk_peek(0));
					krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_RETURN,AS_HANDLER_TARGET(krk_peek(0)));
					krk_currentThread.stackTop[-2] = result;
					break;
				}
				closeUpvalues(frame->slots);
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
			case OP_LESS:          LIKELY_INT_COMPARE_OP(lt,<)
			case OP_GREATER:       LIKELY_INT_COMPARE_OP(gt,>)
			case OP_LESS_EQUAL:    LIKELY_INT_COMPARE_OP(le,<=)
			case OP_GREATER_EQUAL: LIKELY_INT_COMPARE_OP(ge,>=)
			case OP_ADD:           LIKELY_INT_BINARY_OP(add)
			case OP_SUBTRACT:      LIKELY_INT_BINARY_OP(sub)
			case OP_MULTIPLY:      BINARY_OP(mul)
			case OP_DIVIDE:        BINARY_OP(truediv)
			case OP_FLOORDIV:      BINARY_OP(floordiv)
			case OP_MODULO:        BINARY_OP(mod)
			case OP_BITOR:         BINARY_OP(or)
			case OP_BITXOR:        BINARY_OP(xor)
			case OP_BITAND:        BINARY_OP(and)
			case OP_SHIFTLEFT:     BINARY_OP(lshift)
			case OP_SHIFTRIGHT:    BINARY_OP(rshift)
			case OP_POW:           BINARY_OP(pow)
			case OP_MATMUL:        BINARY_OP(matmul)
			case OP_EQUAL:         BINARY_OP(eq);
			case OP_IS:            BINARY_OP(is);
			case OP_BITNEGATE:     LIKELY_INT_UNARY_OP(invert,~)
			case OP_NEGATE:        LIKELY_INT_UNARY_OP(neg,-)
			case OP_POS:           LIKELY_INT_UNARY_OP(pos,+)
			case OP_NONE:  krk_push(NONE_VAL()); break;
			case OP_TRUE:  krk_push(BOOLEAN_VAL(1)); break;
			case OP_FALSE: krk_push(BOOLEAN_VAL(0)); break;
			case OP_UNSET: krk_push(KWARGS_VAL(0)); break;
			case OP_NOT:   krk_push(BOOLEAN_VAL(krk_isFalsey(krk_peek(0)))); /* fallthrough */
			case OP_SWAP_POP: krk_swap(1); /* fallthrough */
			case OP_POP:   krk_pop(); break;

			case OP_INPLACE_ADD:        INPLACE_BINARY_OP(add)
			case OP_INPLACE_SUBTRACT:   INPLACE_BINARY_OP(sub)
			case OP_INPLACE_MULTIPLY:   INPLACE_BINARY_OP(mul)
			case OP_INPLACE_DIVIDE:     INPLACE_BINARY_OP(truediv)
			case OP_INPLACE_FLOORDIV:   INPLACE_BINARY_OP(floordiv)
			case OP_INPLACE_MODULO:     INPLACE_BINARY_OP(mod)
			case OP_INPLACE_BITOR:      INPLACE_BINARY_OP(or)
			case OP_INPLACE_BITXOR:     INPLACE_BINARY_OP(xor)
			case OP_INPLACE_BITAND:     INPLACE_BINARY_OP(and)
			case OP_INPLACE_SHIFTLEFT:  INPLACE_BINARY_OP(lshift)
			case OP_INPLACE_SHIFTRIGHT: INPLACE_BINARY_OP(rshift)
			case OP_INPLACE_POW:        INPLACE_BINARY_OP(pow)
			case OP_INPLACE_MATMUL:     INPLACE_BINARY_OP(matmul)

			case OP_RAISE: {
				krk_raiseException(krk_peek(0), NONE_VAL());
				goto _finishException;
			}
			case OP_RAISE_FROM: {
				krk_raiseException(krk_peek(1), krk_peek(0));
				goto _finishException;
			}
			case OP_CLOSE_UPVALUE:
				closeUpvalues((krk_currentThread.stackTop - krk_currentThread.stack)-1);
				krk_pop();
				break;
			case OP_INVOKE_GETTER: {
				commonMethodInvoke(offsetof(KrkClass,_getter), 2, "'%T' object is not subscriptable");
				break;
			}
			case OP_INVOKE_SETTER: {
				commonMethodInvoke(offsetof(KrkClass,_setter), 3, "'%T' object doesn't support item assignment");
				break;
			}
			case OP_INVOKE_DELETE: {
				commonMethodInvoke(offsetof(KrkClass,_delitem), 2, "'%T' object doesn't support item deletion");
				krk_pop(); /* unused result */
				break;
			}
			case OP_INVOKE_ITER: {
				commonMethodInvoke(offsetof(KrkClass,_iter), 1, "'%T' object is not iterable");
				break;
			}
			case OP_INVOKE_CONTAINS: {
				krk_swap(1); /* operands are backwards */
				commonMethodInvoke(offsetof(KrkClass,_contains), 2, "'%T' object can not be tested for membership");
				break;
			}
			case OP_INVOKE_AWAIT: {
				if (!krk_getAwaitable()) goto _finishException;
				break;
			}
			case OP_SWAP:
				krk_swap(1);
				break;
			case OP_TRY_ELSE: {
				if (IS_HANDLER(krk_peek(0))) {
					krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_FILTER_EXCEPT,AS_HANDLER_TARGET(krk_peek(0)));
				}
				break;
			}
			case OP_BEGIN_FINALLY: {
				if (IS_HANDLER(krk_peek(0))) {
					switch (AS_HANDLER_TYPE(krk_peek(0))) {
						/* We either entered the @c finally without an exception, or the exception was handled by an @c except */
						case OP_PUSH_TRY:
						case OP_FILTER_EXCEPT:
							krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_BEGIN_FINALLY,AS_HANDLER_TARGET(krk_peek(0)));
							break;
						/* We entered the @c finally without handling an exception. */
						case OP_RAISE:
							krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_END_FINALLY,AS_HANDLER_TARGET(krk_peek(0)));
							break;
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
					} else if (AS_HANDLER_TYPE(handler) == OP_EXIT_LOOP) {
						frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(handler);
						OPERAND = AS_INTEGER(krk_peek(1));
						goto _finishPopBlock;
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
					krk_runtimeError(vm.exceptions->typeError, "Can not annotate '%T'.", krk_peek(0));
					goto _finishException;
				}
				break;
			}

			case OP_LIST_APPEND_TOP: {
				KrkValue list = krk_peek(1);
				FUNC_NAME(list,append)(2,(KrkValue[]){list,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_DICT_SET_TOP: {
				KrkValue dict = krk_peek(2);
				FUNC_NAME(dict,__setitem__)(3,(KrkValue[]){dict,krk_peek(1),krk_peek(0)},0);
				krk_pop();
				krk_pop();
				break;
			}
			case OP_SET_ADD_TOP: {
				KrkValue set = krk_peek(1);
				FUNC_NAME(set,add)(2,(KrkValue[]){set,krk_peek(0)},0);
				krk_pop();
				break;
			}

			case OP_LIST_EXTEND_TOP: {
				KrkValue list = krk_peek(1);
				FUNC_NAME(list,extend)(2,(KrkValue[]){list,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_DICT_UPDATE_TOP: {
				KrkValue dict = krk_peek(1);
				FUNC_NAME(dict,update)(2,(KrkValue[]){dict,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_SET_UPDATE_TOP: {
				KrkValue set = krk_peek(1);
				FUNC_NAME(set,update)(2,(KrkValue[]){set,krk_peek(0)},0);
				krk_pop();
				break;
			}

			case OP_TUPLE_FROM_LIST: {
				KrkValue list = krk_peek(0);
				size_t count = AS_LIST(list)->count;
				KrkValue tuple = OBJECT_VAL(krk_newTuple(count));
				krk_push(tuple);
				for (size_t i = 0; i < count; ++i) {
					AS_TUPLE(tuple)->values.values[AS_TUPLE(tuple)->values.count++] = AS_LIST(list)->values[i];
				}
				krk_swap(1);
				krk_pop();
				break;
			}

			case OP_OVERLONG_JUMP: {
				/* Overlong jumps replace 2-byte operand jump instructions with a zero-operand instruction that
				 * slowly scans through a dumb table to find the intended jump target and opcode. */
				for (size_t i = 0; i < frame->closure->function->overlongJumpsCount; ++i) {
					if (frame->closure->function->overlongJumps[i].instructionOffset ==
						(size_t)((char*)frame->ip - (char*)frame->closure->function->chunk.code)) {
						OPERAND = (int)frame->closure->function->overlongJumps[i].intendedTarget << 16;
						opcode  = frame->closure->function->overlongJumps[i].originalOpcode;
						goto _switchEntry;
					}
				}
				krk_runtimeError(vm.exceptions->valueError, "bad jump");
				goto _finishException;
			}

			case OP_PUSH_BUILD_CLASS: {
				KrkValue build_class = NONE_VAL();
				krk_tableGet_fast(&vm.builtins->fields, AS_STRING(vm.specialMethodNames[METHOD_BLDCLS]), &build_class);
				krk_push(build_class);
				break;
			}

			/*
			 * Two-byte operands
			 */
			case OP_JUMP_IF_FALSE_OR_POP: {
				TWO_BYTE_OPERAND;
				if (krk_valuesSame(krk_peek(0), BOOLEAN_VAL(0)) || krk_isFalsey(krk_peek(0))) frame->ip += OPERAND;
				else krk_pop();
				break;
			}
			case OP_POP_JUMP_IF_FALSE: {
				TWO_BYTE_OPERAND;
				if (krk_valuesSame(krk_peek(0), BOOLEAN_VAL(0)) || krk_isFalsey(krk_peek(0))) frame->ip += OPERAND;
				krk_pop();
				break;
			}
			case OP_JUMP_IF_TRUE_OR_POP: {
				TWO_BYTE_OPERAND;
				if (!krk_isFalsey(krk_peek(0))) frame->ip += OPERAND;
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
				frame->ip -= OPERAND;
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
				KrkValue iter = krk_peek(0);
				krk_push(iter);
				krk_push(krk_callStack(0));
				/* krk_valuesSame() */
				if (krk_valuesSame(iter, krk_peek(0))) frame->ip += OPERAND;
				break;
			}
			case OP_LOOP_ITER: {
				TWO_BYTE_OPERAND;
				KrkValue iter = krk_peek(0);
				krk_push(iter);
				krk_push(krk_callStack(0));
				if (!krk_valuesSame(iter, krk_peek(0))) frame->ip -= OPERAND;
				break;
			}
			case OP_TEST_ARG: {
				TWO_BYTE_OPERAND;
				if (!krk_valuesSame(krk_pop(), KWARGS_VAL(0))) frame->ip += OPERAND;
				break;
			}
			case OP_FILTER_EXCEPT: {
				TWO_BYTE_OPERAND;
				/* "Pop exception to match with and jump if not a match" */
				int isMatch = 0;
				if (IS_CLASS(krk_peek(0)) && krk_isInstanceOf(krk_peek(2), AS_CLASS(krk_peek(0)))) {
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
					/* If exception matched, set handler state. */
					krk_currentThread.stackTop[-2] = HANDLER_VAL(OP_FILTER_EXCEPT,AS_HANDLER_TARGET(krk_peek(1)));
				} else {
					/* If exception did not match, jump to next 'except' or 'finally' */
					frame->ip += OPERAND;
				}
				krk_pop();
				break;
			}
			case OP_ENTER_EXCEPT: {
				TWO_BYTE_OPERAND;
				switch (AS_HANDLER_TYPE(krk_peek(0))) {
					case OP_RETURN:
					case OP_END_FINALLY:
					case OP_EXIT_LOOP:
						frame->ip += OPERAND;
						break;
					case OP_RAISE_FROM:
						/* Exception happened while in @c finally */
						krk_pop(); /* handler */
						krk_currentThread.currentException = krk_pop();
						krk_currentThread.flags |= KRK_THREAD_HAS_EXCEPTION;
						goto _finishException;
				}
				break;
			}

			case OP_CONSTANT_LONG:
				THREE_BYTE_OPERAND;
			case OP_CONSTANT: {
				ONE_BYTE_OPERAND;
				KrkValue constant = frame->closure->function->chunk.constants.values[OPERAND];
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
						krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%S'.", name);
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
				if (!krk_tableSetIfExists(frame->globals, OBJECT_VAL(name), krk_peek(0))) {
					krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%S'.", name);
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
					krk_runtimeError(vm.exceptions->nameError, "Undefined variable '%S'.", name);
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
				KrkClosure * closure = krk_newClosure(function, frame->globalsOwner);
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
					} else if (isLocal & 4) {
						closure->upvalues[i] = krk_newUpvalue(0);
						closure->upvalues[i]->closed = NONE_VAL();
						closure->upvalues[i]->location = -1;
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
			case OP_IMPORT_FROM_LONG:
				THREE_BYTE_OPERAND;
			case OP_IMPORT_FROM: {
				ONE_BYTE_OPERAND;
				KrkString * name = READ_STRING(OPERAND);
				if (unlikely(!valueGetProperty(name))) {
					/* Try to import... */
					KrkValue moduleName;
					if (!krk_tableGet(&AS_INSTANCE(krk_peek(0))->fields, vm.specialMethodNames[METHOD_NAME], &moduleName)) {
						krk_runtimeError(vm.exceptions->importError, "Can not import '%S' from non-module '%T' object", name, krk_peek(0));
						goto _finishException;
					}
					krk_push(moduleName);
					krk_push(OBJECT_VAL(S(".")));
					krk_addObjects();
					krk_push(OBJECT_VAL(name));
					krk_addObjects();
					if (!krk_doRecursiveModuleLoad(AS_STRING(krk_peek(0)))) {
						krk_currentThread.flags &= ~KRK_THREAD_HAS_EXCEPTION;
						krk_runtimeError(vm.exceptions->importError, "Can not import '%S' from '%S'", name, AS_STRING(moduleName));
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
					krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", krk_peek(0), name);
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
					krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", krk_peek(0), name);
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
					krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", krk_peek(1), name);
					goto _finishException;
				}
				break;
			}
			case OP_SET_NAME_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_NAME: {
				ONE_BYTE_OPERAND;
				krk_push(krk_currentThread.stack[frame->slots]);
				krk_swap(1);
				krk_push(OBJECT_VAL(READ_STRING(OPERAND)));
				krk_swap(1);
				commonMethodInvoke(offsetof(KrkClass,_setter), 3, "'%T' object doesn't support item assignment");
				break;
			}
			case OP_GET_NAME_LONG:
				THREE_BYTE_OPERAND;
			case OP_GET_NAME: {
				ONE_BYTE_OPERAND;
				krk_push(krk_currentThread.stack[frame->slots]);
				krk_push(OBJECT_VAL(READ_STRING(OPERAND)));
				commonMethodInvoke(offsetof(KrkClass,_getter), 2, "'%T' object doesn't support item assignment");
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
						"super() argument 1 must be class, not %T", baseClass);
					goto _finishException;
				}
				if (IS_KWARGS(krk_peek(0))) {
					krk_runtimeError(vm.exceptions->notImplementedError,
						"Unbound super() reference not supported");
					goto _finishException;
				}

				KrkClass * obj_type;
				KrkValue obj = krk_peek(0);

				if (IS_CLASS(obj) && krk_isSubClass(AS_CLASS(obj),AS_CLASS(baseClass))) {
					/* Class method call */
					obj_type = AS_CLASS(obj);
					krk_pop();
					krk_push(NONE_VAL());
				} else {
					obj_type = krk_getType(obj);
					if (!krk_isInstanceOf(krk_peek(0), AS_CLASS(baseClass))) {
						krk_runtimeError(vm.exceptions->typeError,
							"'%T' object is not an instance of '%S'",
							krk_peek(0), AS_CLASS(baseClass)->name);
						goto _finishException;
					}
				}
				KrkClass * superclass = AS_CLASS(baseClass)->base ? AS_CLASS(baseClass)->base : vm.baseClasses->objectClass;
				if (!krk_bindMethodSuper(superclass, name, obj_type)) {
					krk_runtimeError(vm.exceptions->attributeError, "'%S' object has no attribute '%S'",
						superclass->name, name);
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
					krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", krk_peek(0), name);
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
				closeUpvalues((krk_currentThread.stackTop - krk_currentThread.stack) - OPERAND);
				for (unsigned int i = 0; i < OPERAND; ++i) {
					krk_pop();
				}
				break;
			}

			case OP_EXIT_LOOP_LONG:
				THREE_BYTE_OPERAND;
			case OP_EXIT_LOOP: {
				ONE_BYTE_OPERAND;
_finishPopBlock: (void)0;
				int stackOffset;
				for (stackOffset = (int)(krk_currentThread.stackTop - krk_currentThread.stack - 1);
					stackOffset >= (int)(frame->slots + OPERAND) && 
					!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset],OP_PUSH_TRY) &&
					!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset],OP_PUSH_WITH) &&
					!IS_HANDLER_TYPE(krk_currentThread.stack[stackOffset],OP_FILTER_EXCEPT)
					; stackOffset--) krk_pop();

				/* Do the handler. */
				if (stackOffset >= (int)(frame->slots + OPERAND)) {
					closeUpvalues(stackOffset);
					uint16_t popTarget = (frame->ip - frame->closure->function->chunk.code);
					krk_currentThread.stackTop = &krk_currentThread.stack[stackOffset + 1];
					frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(krk_peek(0));
					krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_EXIT_LOOP, popTarget);
					krk_currentThread.stackTop[-2] = INTEGER_VAL(OPERAND);
				} else {
					closeUpvalues(frame->slots + OPERAND);
				}

				/* Continue normally */
				break;
			}

			case OP_POP_MANY_LONG:
				THREE_BYTE_OPERAND;
			case OP_POP_MANY: {
				ONE_BYTE_OPERAND;
				for (unsigned int i = 0; i < OPERAND; ++i) {
					krk_pop();
				}
				break;
			}
			case OP_TUPLE_LONG:
				THREE_BYTE_OPERAND;
			case OP_TUPLE: {
				ONE_BYTE_OPERAND;
				makeCollection(krk_tuple_of, OPERAND);
				break;
			}
			case OP_MAKE_LIST_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_LIST: {
				ONE_BYTE_OPERAND;
				makeCollection(krk_list_of, OPERAND);
				break;
			}
			case OP_MAKE_DICT_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_DICT: {
				ONE_BYTE_OPERAND;
				makeCollection(krk_dict_of, OPERAND);
				break;
			}
			case OP_MAKE_SET_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_SET: {
				ONE_BYTE_OPERAND;
				makeCollection(krk_set_of, OPERAND);
				break;
			}
			case OP_SLICE_LONG:
				THREE_BYTE_OPERAND;
			case OP_SLICE: {
				ONE_BYTE_OPERAND;
				makeCollection(krk_slice_of, OPERAND);
				break;
			}
			case OP_LIST_APPEND_LONG:
				THREE_BYTE_OPERAND;
			case OP_LIST_APPEND: {
				ONE_BYTE_OPERAND;
				KrkValue list = krk_currentThread.stack[frame->slots + OPERAND];
				FUNC_NAME(list,append)(2,(KrkValue[]){list,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_DICT_SET_LONG:
				THREE_BYTE_OPERAND;
			case OP_DICT_SET: {
				ONE_BYTE_OPERAND;
				KrkValue dict = krk_currentThread.stack[frame->slots + OPERAND];
				FUNC_NAME(dict,__setitem__)(3,(KrkValue[]){dict,krk_peek(1),krk_peek(0)},0);
				krk_pop();
				krk_pop();
				break;
			}
			case OP_SET_ADD_LONG:
				THREE_BYTE_OPERAND;
			case OP_SET_ADD: {
				ONE_BYTE_OPERAND;
				KrkValue set = krk_currentThread.stack[frame->slots + OPERAND];
				FUNC_NAME(set,add)(2,(KrkValue[]){set,krk_peek(0)},0);
				krk_pop();
				break;
			}
			case OP_REVERSE_LONG:
				THREE_BYTE_OPERAND;
			case OP_REVERSE: {
				ONE_BYTE_OPERAND;
				krk_push(NONE_VAL()); /* Storage space */
				for (ssize_t i = 0; i < (ssize_t)OPERAND / 2; ++i) {
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
				KrkValue sequence = krk_peek(0);
				KrkTuple * values = krk_newTuple(OPERAND);
				krk_push(OBJECT_VAL(values));
				if (unlikely(krk_unpackIterable(sequence, values, _unpack_op))) {
					goto _finishException;
				}
				if (unlikely(values->values.count != OPERAND)) {
					krk_runtimeError(vm.exceptions->valueError, "not enough values to unpack (expected %u, got %zu)", OPERAND, values->values.count);
					goto _finishException;
				}
				if (unlikely(OPERAND == 0)) {
					krk_pop();
					krk_pop();
					break;
				}
				/* We no longer need the sequence */
				krk_swap(1);
				krk_pop();
				for (size_t i = 1; i < values->values.count; ++i) {
					krk_push(values->values.values[i]);
				}
				krk_currentThread.stackTop[-(ssize_t)OPERAND] = values->values.values[0];
				break;
			}

			case OP_FORMAT_VALUE_LONG:
				THREE_BYTE_OPERAND;
			case OP_FORMAT_VALUE: {
				ONE_BYTE_OPERAND;
				if (doFormatString(OPERAND)) goto _finishException;
				break;
			}

			case OP_MAKE_STRING_LONG:
				THREE_BYTE_OPERAND;
			case OP_MAKE_STRING: {
				ONE_BYTE_OPERAND;

				struct StringBuilder sb = {0};

				for (ssize_t i = 0; i < (ssize_t)OPERAND; ++i) {
					KrkValue s = krk_currentThread.stackTop[-(ssize_t)OPERAND+i];
					if (unlikely(!IS_STRING(s))) {
						discardStringBuilder(&sb);
						krk_runtimeError(vm.exceptions->valueError, "'%T' is not a string", s);
						goto _finishException;
					}
					pushStringBuilderStr(&sb, (char*)AS_STRING(s)->chars, AS_STRING(s)->length);
				}

				for (ssize_t i = 0; i < (ssize_t)OPERAND; ++i) {
					krk_pop();
				}

				krk_push(finishStringBuilder(&sb));
				break;
			}

			case OP_MISSING_KW_LONG:
				THREE_BYTE_OPERAND;
			case OP_MISSING_KW: {
				ONE_BYTE_OPERAND;
				krk_runtimeError(vm.exceptions->typeError, "%s() missing required keyword-only argument: %R",
					frame->closure->function->name ? frame->closure->function->name->chars : "<unnamed>",
					frame->closure->function->keywordArgNames.values[OPERAND]);
				break;
			}

			case OP_UNPACK_EX_LONG:
				THREE_BYTE_OPERAND;
			case OP_UNPACK_EX: {
				ONE_BYTE_OPERAND;
				unsigned char before = OPERAND >> 8;
				unsigned char after = OPERAND;

				KrkValue sequence = krk_peek(0);
				KrkTuple * values = krk_newTuple(before + after + 1);
				krk_push(OBJECT_VAL(values));
				KrkValue list = krk_list_of(0,NULL,0);
				krk_push(list);


				struct ex_unpack _context = { values, before, after, list, 0 };
				if (unlikely(krk_unpackIterable(sequence, &_context, _unpack_ex))) {
					goto _finishException;
				}

				if (values->values.count < before) {
					krk_runtimeError(vm.exceptions->typeError, "not enough values to unpack (expected at least %u, got %zu)",
						before+after, values->values.count);
					goto _finishException;
				}

				if (values->values.count == before) {
					values->values.values[values->values.count++] = list;
				}

				if (AS_LIST(list)->count < after) {
					krk_runtimeError(vm.exceptions->typeError, "not enough values to unpack (expected at least %u, got %zu)",
						before+after, values->values.count - 1 + AS_LIST(list)->count);
					goto _finishException;
				}

				if (after) {
					size_t more = after;
					while (more) {
						values->values.values[before+more] = AS_LIST(list)->values[--AS_LIST(list)->count];
						more--;
					}
					values->values.count += after;
				}

				/* We no longer need the sequence */
				krk_pop(); /* list */
				krk_swap(1);
				krk_pop();
				for (size_t i = 1; i < values->values.count; ++i) {
					krk_push(values->values.values[i]);
				}
				krk_currentThread.stackTop[-(ssize_t)(before + after + 1)] = values->values.values[0];
				break;
			}


			default:
				__builtin_unreachable();
		}
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
_finishException:
			if (!handleException()) {
				if (!IS_NONE(krk_currentThread.stackTop[-2])) {
					krk_attachInnerException(krk_currentThread.stackTop[-2]);
				}
				frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
				frame->ip = frame->closure->function->chunk.code + AS_HANDLER_TARGET(krk_peek(0));
				/* Stick the exception into the exception slot */
				switch (AS_HANDLER_TYPE(krk_currentThread.stackTop[-1])) {
					/* An exception happened while handling an exception */
					case OP_RAISE:
					case OP_FILTER_EXCEPT:
						krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_END_FINALLY,AS_HANDLER_TARGET(krk_peek(0)));
						break;
					/* An exception happened while already in the @c finally block from handling
					 * another exception. Bail. */
					case OP_END_FINALLY:
						krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_RAISE_FROM,AS_HANDLER_TARGET(krk_peek(0)));
						break;
					/* First exception in this chain. */
					default:
						krk_currentThread.stackTop[-1] = HANDLER_VAL(OP_RAISE,AS_HANDLER_TARGET(krk_peek(0)));
						break;
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

KrkValue krk_interpret(const char * src, const char * fromFile) {
	KrkCodeObject * function = krk_compile(src, fromFile);
	if (!function) {
		if (!krk_currentThread.frameCount) handleException();
		return NONE_VAL();
	}

	krk_push(OBJECT_VAL(function));
	krk_attachNamedObject(&krk_currentThread.module->fields, "__file__", (KrkObj*)function->chunk.filename);
	KrkClosure * closure = krk_newClosure(function, OBJECT_VAL(krk_currentThread.module));
	krk_pop();

	krk_push(OBJECT_VAL(closure));
	return krk_callStack(0);
}

#ifndef KRK_NO_FILESYSTEM
KrkValue krk_runfile(const char * fileName, const char * fromFile) {
	FILE * f = fopen(fileName,"r");
	if (!f) {
		fprintf(stderr, "%s: could not open file '%s': %s\n", "kuroko", fileName, strerror(errno));
		return INTEGER_VAL(errno);
	}

	char * buf;

	if (fseek(f, 0, SEEK_END) < 0) {
		struct StringBuilder sb = {0};
		char tmp[1024];
		while (!feof(f)) {
			size_t r = fread(tmp, 1, 1024, f);
			if (!r) {
				krk_discardStringBuilder(&sb);
				goto _on_fread_error;
			}
			krk_pushStringBuilderStr(&sb, tmp, r);
		}
		buf = sb.bytes;
	} else {
		size_t size = ftell(f);
		fseek(f, 0, SEEK_SET);
		buf = malloc(size+1);
		if (fread(buf, 1, size, f) == 0 && size != 0) {
			free(buf);
			_on_fread_error:
			fprintf(stderr, "%s: could not read file '%s': %s\n", "kuroko", fileName, strerror(errno));
			return INTEGER_VAL(errno);
		}
		buf[size] = '\0';
	}

	fclose(f);

	KrkValue result = krk_interpret(buf, fromFile);
	free(buf);

	return result;
}

#endif
