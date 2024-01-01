/**
 * @file obj_gen.c
 * @brief Generator objects.
 *
 * Generator objects track runtime state so they can be resumed and yielded from.
 * Any function with a `yield` statement in its body is implicitly transformed
 * into a generator object when called.
 */
#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/debug.h>

/**
 * @brief Generator object implementation.
 * @extends KrkInstance
 */
struct generator {
	KrkInstance inst;
	KrkClosure * closure;
	KrkValue * args;
	size_t argCount;
	uint8_t * ip;
	int running;
	int started;
	KrkValue result;
	int type;
	KrkThreadState fakethread;
	KrkUpvalue * capturedUpvalues;
};

#define AS_generator(o) ((struct generator *)AS_OBJECT(o))
#define IS_generator(o) (krk_isInstanceOf(o, KRK_BASE_CLASS(generator)))

#define CURRENT_CTYPE struct generator *
#define CURRENT_NAME  self

static void _generator_close_upvalues(struct generator * self) {
	while (self->capturedUpvalues) {
		KrkUpvalue * upvalue = self->capturedUpvalues;
		upvalue->closed = self->args[upvalue->location];
		upvalue->location = -1;
		self->capturedUpvalues = upvalue->next;
	}
}

static void _generator_gcscan(KrkInstance * _self) {
	struct generator * self = (struct generator*)_self;
	krk_markObject((KrkObj*)self->closure);
	for (size_t i = 0; i < self->argCount; ++i) {
		krk_markValue(self->args[i]);
	}
	for (KrkUpvalue * upvalue = self->capturedUpvalues; upvalue; upvalue = upvalue->next) {
		krk_markObject((KrkObj*)upvalue);
	}
	krk_markValue(self->result);
}

static void _generator_gcsweep(KrkInstance * self) {
	_generator_close_upvalues((struct generator*)self);
	free(((struct generator*)self)->args);
}

static void _set_generator_done(struct generator * self) {
	self->ip = NULL;
	_generator_close_upvalues(self);
}

/**
 * @brief Create a generator object from a closure and set of arguments.
 *
 * Initializes the generator object, attaches the argument list, and sets up
 * the execution state to point to the start of the function's code object.
 *
 * @param closure  Function object to transform.
 * @param argsIn   Array of arguments passed to the call.
 * @param argCount Number of arguments in @p argsIn
 * @return A @ref generator object.
 */
KrkInstance * krk_buildGenerator(KrkClosure * closure, KrkValue * argsIn, size_t argCount) {
	/* Copy the args */
	KrkValue * args = malloc(sizeof(KrkValue) * (argCount));
	memcpy(args, argsIn, sizeof(KrkValue) * argCount);

	/* Create a generator object */
	struct generator * self = (struct generator *)krk_newInstance(KRK_BASE_CLASS(generator));
	self->args = args;
	self->argCount = argCount;
	self->closure = closure;
	self->ip = self->closure->function->chunk.code;
	self->result = NONE_VAL();
	self->type = closure->function->obj.flags & (KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR | KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE);
	return (KrkInstance *)self;
}

FUNC_SIG(generator,__init__) {
	return krk_runtimeError(vm.exceptions->typeError, "cannot create '%s' instances", "generator");
}

KRK_Method(generator,__repr__) {
	METHOD_TAKES_NONE();

	char * typeStr = "generator";
	if (self->type == KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE) {
		/* Regular coroutine */
		typeStr = "coroutine";
	} else if (self->type == (KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE | KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR)) {
		typeStr = "async_generator";
	}

	return krk_stringFromFormat("<%s object %S at %p>",
		typeStr, self->closure->function->name, (void*)self);
}

KRK_Method(generator,__iter__) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
}

KRK_Method(generator,__call__) {
	METHOD_TAKES_AT_MOST(1);
	if (!self->ip) return OBJECT_VAL(self);
	if (self->running) {
		return krk_runtimeError(vm.exceptions->valueError, "generator already executing");
	}
	/* Prepare frame */
	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount++];
	frame->closure = self->closure;
	frame->ip      = self->ip;
	frame->slots   = krk_currentThread.stackTop - krk_currentThread.stack;
	frame->outSlots = frame->slots;
	frame->globals = self->closure->globalsTable;
	frame->globalsOwner = self->closure->globalsOwner;

	/* Stick our stack on their stack */
	for (size_t i = 0; i < self->argCount; ++i) {
		krk_push(self->args[i]);
	}

	/* Point any of our captured upvalues back to their actual stack locations */
	while (self->capturedUpvalues) {
		KrkUpvalue * upvalue = self->capturedUpvalues;
		upvalue->owner = &krk_currentThread;
		upvalue->location = upvalue->location + frame->slots;
		self->capturedUpvalues = upvalue->next;
		upvalue->next = krk_currentThread.openUpvalues;
		krk_currentThread.openUpvalues = upvalue;
	}

	if (self->started) {
		krk_pop();
		if (argc > 1) {
			krk_push(argv[1]);
		} else {
			krk_push(NONE_VAL());
		}
	}

	/* Jump into the iterator */
	self->running = 1;
	size_t stackBefore = krk_currentThread.stackTop - krk_currentThread.stack;
	KrkValue result = krk_runNext();
	size_t stackAfter = krk_currentThread.stackTop - krk_currentThread.stack;
	self->running = 0;

	self->started = 1;

	if (IS_KWARGS(result) && AS_INTEGER(result) == 0) {
		self->result = krk_pop();
		_set_generator_done(self);
		return OBJECT_VAL(self);
	}

	/* Was there an exception? */
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
		_set_generator_done(self);
		krk_currentThread.stackTop = krk_currentThread.stack + frame->slots;
		return NONE_VAL();
	}

	/* Redirect any remaining upvalues captured from us, and release them from the VM */
	while (krk_currentThread.openUpvalues != NULL && krk_currentThread.openUpvalues->location >= (int)frame->slots) {
		KrkUpvalue * upvalue = krk_currentThread.openUpvalues;
		upvalue->location = upvalue->location - frame->slots;
		upvalue->owner = &self->fakethread;
		krk_currentThread.openUpvalues = upvalue->next;
		upvalue->next = self->capturedUpvalues;
		self->capturedUpvalues = upvalue;
	}

	/* Determine the stack state */
	if (stackAfter > stackBefore) {
		size_t newArgs = stackAfter - stackBefore;
		self->args = realloc(self->args, sizeof(KrkValue) * (self->argCount + newArgs));
		self->argCount += newArgs;
	} else if (stackAfter < stackBefore) {
		size_t deadArgs = stackBefore - stackAfter;
		self->args = realloc(self->args, sizeof(KrkValue) * (self->argCount - deadArgs));
		self->argCount -= deadArgs;
	}

	/* Save stack entries */
	memcpy(self->args, krk_currentThread.stackTop - self->argCount, sizeof(KrkValue) * self->argCount);
	self->ip      = frame->ip;
	self->fakethread.stack = self->args;

	krk_currentThread.stackTop = krk_currentThread.stack + frame->slots;

	return result;
}

KRK_Method(generator,send) {
	METHOD_TAKES_EXACTLY(1);
	if (!self->started && !IS_NONE(argv[1])) {
		return krk_runtimeError(vm.exceptions->typeError, "Can not send non-None value to just-started generator");
	}
	return FUNC_NAME(generator,__call__)(argc,argv,0);
}

KRK_Method(generator,__finish__) {
	METHOD_TAKES_NONE();
	return self->result;
}

/*
 * For compatibility with Python...
 */
KRK_Method(generator,gi_running) {
	METHOD_TAKES_NONE();
	return BOOLEAN_VAL(self->running);
}

int krk_getAwaitable(void) {
	if (IS_generator(krk_peek(0)) && AS_generator(krk_peek(0))->type == KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE) {
		/* Good to go */
		return 1;
	}

	/* Need to try for __await__ */
	KrkValue method = krk_valueGetAttribute_default(krk_peek(0), "__await__", NONE_VAL());
	if (!IS_NONE(method)) {
		krk_push(method);
		krk_swap(1);
		krk_pop();
		krk_push(krk_callStack(0));
		KrkClass * _type = krk_getType(krk_peek(0));
		if (!_type || !_type->_iter) {
			krk_runtimeError(vm.exceptions->attributeError, "__await__ returned non-iterator of type '%T'", krk_peek(0));
			return 0;
		}
	} else {
		krk_runtimeError(vm.exceptions->attributeError, "'%T' object is not awaitable", krk_peek(0));
		return 0;
	}

	return 1;
}

_noexport
void _createAndBind_generatorClass(void) {
	KrkClass * generator = ADD_BASE_CLASS(vm.baseClasses->generatorClass, "generator", vm.baseClasses->objectClass);
	generator->allocSize = sizeof(struct generator);
	generator->_ongcscan = _generator_gcscan;
	generator->_ongcsweep = _generator_gcsweep;
	generator->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(generator,__init__);
	BIND_METHOD(generator,__iter__);
	BIND_METHOD(generator,__call__);
	BIND_METHOD(generator,__repr__);
	BIND_METHOD(generator,__finish__);
	BIND_METHOD(generator,send);
	BIND_PROP(generator,gi_running);
	krk_finalizeClass(generator);
}
