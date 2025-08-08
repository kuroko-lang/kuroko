#include <errno.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

#ifdef _WIN32
KRK_Module(poll) {
	krk_runtimeError(vm.exceptions->OSError, "poll is not available on Windows");
}
#else
#include <poll.h>

static KrkClass * PollObject;

struct PollObject {
	KrkInstance inst;
	size_t nfds;
	size_t ncap;
	struct pollfd * fds;
};

#define IS_PollObject(o) (krk_isInstanceOf(o,PollObject))
#define AS_PollObject(o) ((struct PollObject*)AS_OBJECT(o))
#define CURRENT_CTYPE struct PollObject *
#define CURRENT_NAME  self

KRK_Method(PollObject,poll) {
	int timeout = -1;

	// Handle None for timeout?
	if (!krk_parseArgs(".|i",(const char *[]){"timeout"}, &timeout)) return NONE_VAL();

	int res = poll(self->fds, self->nfds, timeout);
	if (res < 0) return krk_runtimeError(KRK_EXC(OSError), "%s", strerror(errno));

	KrkValue outlist = krk_list_of(0,NULL,0);
	krk_push(outlist);

	if (res > 0) {
		/* Go through each one and do the thing */
		for (size_t i = 0; i < self->nfds; ++i) {
			if (self->fds[i].revents) {
				krk_push(krk_tuple_of(2, (KrkValue[]){INTEGER_VAL(self->fds[i].fd),INTEGER_VAL(self->fds[i].revents)}, 0));
				krk_writeValueArray(AS_LIST(outlist), krk_peek(0));
				krk_pop();
				self->fds[i].revents = 0;
			}
		}
	}

	return krk_pop();
}

KRK_Method(PollObject,register) {
	int fd;
	int flags = POLLIN|POLLPRI|POLLOUT;

	// TODO: It would be nice to take any object that has a fileno method?
	if (!krk_parseArgs(".i|i",(const char *[]){"fd","eventmask"},
		&fd, &flags)) return NONE_VAL();

	// TODO: Should scan entire list to see if this fd was already there.
	if (self->nfds + 1 > self->ncap) {
		size_t old = self->ncap;
		self->ncap = KRK_GROW_CAPACITY(old);
		self->fds  = KRK_GROW_ARRAY(struct pollfd, self->fds, old, self->ncap);
	}
	self->fds[self->nfds].fd = fd;
	self->fds[self->nfds].events = flags;
	self->fds[self->nfds].revents = 0;
	self->nfds++;
	return NONE_VAL();
}

KRK_Method(PollObject,unregister) {
	int fd;
	if (!krk_parseArgs(".i",(const char*[]){"fd"}, &fd)) return NONE_VAL();

	for (size_t i = 0; i < self->nfds; ++i) {
		if (self->fds[i].fd == fd) {
			memcpy(&self->fds[i], &self->fds[i+1], (self->nfds - i - 1) * sizeof(struct pollfd));
			self->nfds -= 1;
			return NONE_VAL();
		}
	}

	return krk_runtimeError(vm.exceptions->keyError, "%d", fd);
}

KRK_Method(PollObject,modify) {
	int fd;
	int flags;
	if (!krk_parseArgs(".ii",(const char *[]){"fd","eventmask"},
		&fd, &flags)) return NONE_VAL();

	for (size_t i = 0; i < self->nfds; ++i) {
		if (self->fds[i].fd == fd) {
			self->fds[i].events = flags;
			return NONE_VAL();
		}
	}

	// This is OSError ENOENT in Python...
	return krk_runtimeError(vm.exceptions->keyError, "%d", fd);
}

KRK_Module(poll) {
	KRK_DOC(module, "@brief Bindings to Unix poll.");

	PollObject = krk_makeClass(module, &PollObject, "PollObject", vm.baseClasses->objectClass);
	PollObject->allocSize = sizeof(struct PollObject);

	BIND_METHOD(PollObject,poll);
	BIND_METHOD(PollObject,register);
	BIND_METHOD(PollObject,unregister);
	BIND_METHOD(PollObject,modify);

	krk_finalizeClass(PollObject);

#define POLL_CONST(o) krk_attachNamedValue(&module->fields, #o, INTEGER_VAL(o));

	POLL_CONST(POLLIN);
	POLL_CONST(POLLPRI);
	POLL_CONST(POLLOUT);
	POLL_CONST(POLLERR);
	POLL_CONST(POLLHUP);
	POLL_CONST(POLLNVAL);
}

#endif
