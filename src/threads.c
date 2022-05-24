#include <string.h>

#include <kuroko/kuroko.h>

#ifdef ENABLE_THREADING
#include <kuroko/util.h>

#include <unistd.h>
#include <pthread.h>

#if defined(__linux__)
# include <sys/syscall.h>
# define gettid() syscall(SYS_gettid)
#elif defined(__toaru__)
# include <pthread.h>
#elif defined(_WIN32)
# define gettid() GetCurrentThreadId()
#else
# define gettid() -1
#endif

static KrkClass * ThreadError;
static KrkClass * Thread;

/**
 * @brief Object representation of a system thread.
 * @extends KrkInstance
 *
 * Except for the main thread, each new thread spawned by the virtual machine
 * is represented by a thread object. As long as the thread is active, its
 * own thread object will live on its stac and avoid being garbage collected.
 * When a thread terminates, its thread object can be released if not referenced
 * from another thread.
 */
struct Thread {
	KrkInstance inst;
	KrkThreadState * threadState;
	pthread_t nativeRef;
	pid_t  tid;
	unsigned int    started:1;
	unsigned int    alive:1;
};

static KrkClass * Lock;

/**
 * @brief Simple atomic structure for waiting.
 * @extends KrkInstance
 *
 * Provides direct access to a mutex for managed code.
 */
struct Lock {
	KrkInstance inst;
	pthread_mutex_t mutex;
};

KRK_FUNC(current_thread,{
	if (&krk_currentThread == vm.threads) return NONE_VAL();
	return krk_currentThread.stack[0];
})

#define IS_Thread(o)  (krk_isInstanceOf(o, Thread))
#define AS_Thread(o)  ((struct Thread *)AS_OBJECT(o))
#define CURRENT_CTYPE struct Thread *
#define CURRENT_NAME  self

static volatile int _threadLock = 0;
static void * _startthread(void * _threadObj) {
#if defined(ENABLE_THREADING) && defined(__APPLE__) && defined(__aarch64__)
	krk_forceThreadData();
#endif
	memset(&krk_currentThread, 0, sizeof(KrkThreadState));
	krk_currentThread.frames = calloc(vm.maximumCallDepth,sizeof(KrkCallFrame));
	vm.globalFlags |= KRK_GLOBAL_THREADS;
	_obtain_lock(_threadLock);
	if (vm.threads->next) {
		krk_currentThread.next = vm.threads->next;
	}
	vm.threads->next = &krk_currentThread;
	_release_lock(_threadLock);

	/* Get our run function */
	struct Thread * self = _threadObj;
	self->threadState = &krk_currentThread;
	self->tid = gettid();

	KrkValue runMethod = NONE_VAL();
	KrkClass * ourType = self->inst._class;
	if (!krk_tableGet(&ourType->methods, OBJECT_VAL(S("run")), &runMethod)) {
		krk_runtimeError(ThreadError, "Thread object has no run() method");
	} else {
		krk_push(runMethod);
		krk_push(OBJECT_VAL(self));
		krk_callStack(1);
	}

	self->alive = 0;

	/* Remove this thread from the thread pool, its stack is garbage anyway */
	_obtain_lock(_threadLock);
	krk_resetStack();
	KrkThreadState * previous = vm.threads;
	while (previous) {
		if (previous->next == &krk_currentThread) {
			previous->next = krk_currentThread.next;
			break;
		}
		previous = previous->next;
	}
	_release_lock(_threadLock);

	FREE_ARRAY(size_t, krk_currentThread.stack, krk_currentThread.stackSize);
	free(krk_currentThread.frames);

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

#undef CURRENT_CTYPE

#define IS_Lock(o)  (krk_isInstanceOf(o, Lock))
#define AS_Lock(o)  ((struct Lock *)AS_OBJECT(o))
#define CURRENT_CTYPE struct Lock *

KRK_METHOD(Lock,__init__,{
	METHOD_TAKES_NONE(); /* TODO lock options, like recursive or error-checked? */
	pthread_mutex_init(&self->mutex, NULL);
	return argv[0];
})

static inline void _pushLockStatus(struct Lock * self, struct StringBuilder * sb) {
#ifdef __GLIBC__
	{
		if (self->mutex.__data.__owner) {
			pushStringBuilderStr(sb, " (locked)", 9);
		} else {
			pushStringBuilderStr(sb, " (unlocked)", 11);
		}
	}
#else
	(void)self;
	(void)sb;
#endif
}

KRK_METHOD(Lock,__repr__,{
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, "<Lock ", 6);

	/* Address of lock object */
	{
		char tmp[100];
		size_t len = snprintf(tmp, 100, "%p", (void*)self);
		pushStringBuilderStr(&sb, tmp, len);
	}

	_pushLockStatus(self,&sb);

	pushStringBuilder(&sb,'>');
	return finishStringBuilder(&sb);
})

KRK_METHOD(Lock,__enter__,{
	METHOD_TAKES_NONE();
	pthread_mutex_lock(&self->mutex);
})

KRK_METHOD(Lock,__exit__,{
	pthread_mutex_unlock(&self->mutex);
})

_noexport
void _createAndBind_threadsMod(void) {
	/**
	 * threads = module()
	 *
	 * Methods for dealing with threads.
	 */
	KrkInstance * threadsModule = krk_newInstance(vm.baseClasses->moduleClass);

	krk_attachNamedObject(&vm.modules, "threading", (KrkObj*)threadsModule);
	krk_attachNamedObject(&threadsModule->fields, "__name__", (KrkObj*)S("threading"));
	krk_attachNamedValue(&threadsModule->fields, "__file__", NONE_VAL());
	KRK_DOC(threadsModule,
		"@brief Methods and classes for creating platform threads.");

	KRK_DOC(BIND_FUNC(threadsModule, current_thread),
		"@brief Obtain a reference to the current thread.\n"
		"@arguments \n\n"
		"Returns the @ref Thread object associated with the calling thread, if one exists.");

	krk_makeClass(threadsModule, &ThreadError, "ThreadError", vm.exceptions->baseException);
	KRK_DOC(ThreadError,
		"Raised in various situations when an action on a thread is invalid."
	);
	krk_finalizeClass(ThreadError);

	krk_makeClass(threadsModule, &Thread, "Thread", vm.baseClasses->objectClass);
	KRK_DOC(Thread,
		"Base class for building threaded execution contexts.\n\n"
		"The @ref Thread class should be subclassed and the subclass should implement a @c run method."
	);
	Thread->allocSize = sizeof(struct Thread);
	KRK_DOC(BIND_METHOD(Thread,start), "Start the thread. A thread may only be started once.");
	KRK_DOC(BIND_METHOD(Thread,join), "Join the thread. Does not return until the thread finishes.");
	KRK_DOC(BIND_METHOD(Thread,is_alive), "Query the status of the thread.");
	KRK_DOC(BIND_PROP(Thread,tid), "The platform-specific thread identifier, if available. Usually an integer.");
	krk_finalizeClass(Thread);

	krk_makeClass(threadsModule, &Lock, "Lock", vm.baseClasses->objectClass);
	KRK_DOC(Lock,
		"Represents an atomic mutex.\n\n"
		"@ref Lock objects allow for exclusive access to a resource and can be used in a @c with block."
	);
	Lock->allocSize = sizeof(struct Lock);
	KRK_DOC(BIND_METHOD(Lock,__init__), "Initialize a system mutex.");
	KRK_DOC(BIND_METHOD(Lock,__enter__),"Acquire the lock.");
	KRK_DOC(BIND_METHOD(Lock,__exit__), "Release the lock.");
	BIND_METHOD(Lock,__repr__);
	krk_finalizeClass(Lock);
}


#endif
