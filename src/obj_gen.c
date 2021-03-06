#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

static KrkClass * generator;
struct generator {
	KrkInstance inst;
	KrkClosure * closure;
	KrkValue * args;
	size_t argCount;
	uint8_t * ip;
	int running;
};

#define AS_generator(o) ((struct generator *)AS_OBJECT(o))
#define IS_generator(o) (krk_isInstanceOf(o, generator))

#define CURRENT_CTYPE struct generator *
#define CURRENT_NAME  self

static void _generator_gcscan(KrkInstance * _self) {
	struct generator * self = (struct generator*)_self;
	krk_markObject((KrkObj*)self->closure);
	for (size_t i = 0; i < self->argCount; ++i) {
		krk_markValue(self->args[i]);
	}
}

static void _generator_gcsweep(KrkInstance * self) {
	free(((struct generator*)self)->args);
}

static void _set_generator_done(struct generator * self) {
	self->ip = NULL;
}

KrkInstance * krk_buildGenerator(KrkClosure * closure, KrkValue * argsIn, size_t argCount) {
	/* Copy the args */
	KrkValue * args = malloc(sizeof(KrkValue) * (argCount));
	memcpy(args, argsIn, sizeof(KrkValue) * argCount);

	/* Create a generator object */
	struct generator * self = (struct generator *)krk_newInstance(generator);
	self->args = args;
	self->argCount = argCount;
	self->closure = closure;
	self->ip = self->closure->function->chunk.code;
	return (KrkInstance *)self;
}

KRK_METHOD(generator,__repr__,{
	METHOD_TAKES_NONE();

	size_t estimatedLength = sizeof("<generator object  at 0x1234567812345678>") + 1 + self->closure->function->name->length;
	char * tmp = malloc(estimatedLength);
	size_t lenActual = snprintf(tmp, estimatedLength, "<generator object %s at %p>",
		self->closure->function->name->chars,
		(void*)self);

	return OBJECT_VAL(krk_takeString(tmp,lenActual));
})

KRK_METHOD(generator,__iter__,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
})

KRK_METHOD(generator,__call__,{
	METHOD_TAKES_NONE();
	if (!self->ip) return OBJECT_VAL(self);
	/* Prepare frame */
	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount++];
	frame->closure = self->closure;
	frame->ip      = self->ip;
	frame->slots   = krk_currentThread.stackTop - krk_currentThread.stack;
	frame->outSlots = frame->slots;
	frame->globals = &self->closure->function->globalsContext->fields;

	/* Stick our stack on their stack */
	for (size_t i = 0; i < self->argCount; ++i) {
		krk_push(self->args[i]);
	}

	/* Jump into the iterator */
	self->running = 1;
	size_t stackBefore = krk_currentThread.stackTop - krk_currentThread.stack;
	KrkValue result = krk_runNext();
	size_t stackAfter = krk_currentThread.stackTop - krk_currentThread.stack;
	self->running = 0;

	/* Was there an exception? */
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
		_set_generator_done(self);
		return NONE_VAL();
	}

	/* Determine the stack state */
	if (stackAfter > stackBefore) {
		size_t newArgs = stackAfter - stackBefore;
		self->args = realloc(self->args, sizeof(KrkValue) * (self->argCount + newArgs));
		self->argCount += newArgs;
	} else if (stackAfter < stackBefore) {
		_set_generator_done(self);
		return OBJECT_VAL(self);
	}

	/* Save stack entries */
	memcpy(self->args, krk_currentThread.stackTop - self->argCount, sizeof(KrkValue) * self->argCount);
	self->ip      = frame->ip;

	krk_currentThread.stackTop = krk_currentThread.stack + frame->slots;

	return result;
})

/*
 * For compatibility with Python...
 */
KRK_METHOD(generator,gi_running,{
	METHOD_TAKES_NONE();
	return BOOLEAN_VAL(self->running);
})

_noexport
void _createAndBind_generatorClass(void) {
	generator = ADD_BASE_CLASS(vm.baseClasses->generatorClass, "generator", vm.baseClasses->objectClass);
	generator->allocSize = sizeof(struct generator);
	generator->_ongcscan = _generator_gcscan;
	generator->_ongcsweep = _generator_gcsweep;
	BIND_METHOD(generator,__iter__);
	BIND_METHOD(generator,__call__);
	BIND_METHOD(generator,__repr__);
	BIND_PROP(generator,gi_running);
	krk_defineNative(&generator->methods, "__str__", FUNC_NAME(generator,__repr__));
	krk_finalizeClass(generator);
}
