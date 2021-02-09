#include <string.h>

#include "kuroko.h"

#ifdef ENABLE_THREADING
#include "util.h"

#include <unistd.h>
#include <pthread.h>

#if defined(__linux__)
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#elif defined(__toaru__)
##include <pthread.h>
#else
#define gettid() -1
#endif

KrkClass * ThreadError;
KrkClass * Thread;
struct Thread {
	KrkInstance inst;
	KrkThreadState * threadState;
	pthread_t nativeRef;
	pid_t  tid;
	unsigned int    started:1;
	unsigned int    alive:1;
};

KRK_FUNC(current_thread,{
	if (&krk_currentThread == vm.threads) return NONE_VAL();
	return krk_currentThread.stack[0];
})

#define IS_Thread(o)  (krk_isInstanceOf(o, Thread))
#define AS_Thread(o)  ((struct Thread *)AS_OBJECT(o))
#define CURRENT_CTYPE struct Thread *
#define CURRENT_NAME  self

static void * _startthread(void * _threadObj) {
	memset(&krk_currentThread, 0, sizeof(KrkThreadState));
	/* TODO: Wrap this in a mutex */
	if (vm.threads->next) {
		krk_currentThread.next = vm.threads->next;
	}
	vm.threads->next = &krk_currentThread;

	/* Get our run function */
	struct Thread * self = _threadObj;
	self->threadState = &krk_currentThread;
	self->tid = gettid();

	KrkValue runMethod = NONE_VAL();
	KrkClass * ourType = self->inst._class;
	if (!krk_tableGet(&ourType->methods, OBJECT_VAL(S("run")), &runMethod)) {
		krk_runtimeError(ThreadError, "Thread object has no run() method");
	} else {
		krk_push(OBJECT_VAL(self));
		krk_callValue(runMethod, 1, 0);
		krk_runNext();
	}

	self->alive = 0;

	return NULL;
}

KRK_METHOD(Thread,tid,{
	METHOD_TAKES_NONE(); /* Property, but can not be assigned. */
	return INTEGER_VAL(self->tid);
})

KRK_METHOD(Thread,join,{
	if (self->threadState == &krk_currentThread)
		return krk_runtimeError(ThreadError, "Thread can not join itself.");
	if (!self->started)
		return krk_runtimeError(ThreadError, "Thread has not been started.");

	pthread_join(self->nativeRef, NULL);
})

KRK_METHOD(Thread,start,{
	METHOD_TAKES_NONE();

	if (self->started)
		return krk_runtimeError(ThreadError, "Thread has already been started.");

	self->started = 1;
	self->alive   = 1;
	pthread_create(&self->nativeRef, NULL, _startthread, (void*)self);

	return argv[0];
})

KRK_METHOD(Thread,is_alive,{
	METHOD_TAKES_NONE();
	return BOOLEAN_VAL(self->alive);
})

KrkInstance * threadsModule;

_noexport
void _createAndBind_threadsMod(void) {
	/**
	 * threads = module()
	 *
	 * Methods for dealing with threads.
	 */
	threadsModule = krk_newInstance(vm.baseClasses->moduleClass);

	krk_attachNamedObject(&vm.modules, "threading", (KrkObj*)threadsModule);
	krk_attachNamedObject(&threadsModule->fields, "__name__", (KrkObj*)S("threading"));
	krk_attachNamedValue(&threadsModule->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&threadsModule->fields, "__doc__",
		(KrkObj*)S("Methods for dealing with threads."));

	BIND_FUNC(threadsModule, current_thread);

	krk_makeClass(threadsModule, &ThreadError, "ThreadError", vm.exceptions->baseException);
	krk_finalizeClass(ThreadError);

	krk_makeClass(threadsModule, &Thread, "Thread", vm.baseClasses->objectClass);
	Thread->allocSize = sizeof(struct Thread);
	BIND_METHOD(Thread,start);
	BIND_METHOD(Thread,join);
	BIND_METHOD(Thread,is_alive);
	BIND_PROP(Thread,tid);
	krk_finalizeClass(Thread);
}


#endif
