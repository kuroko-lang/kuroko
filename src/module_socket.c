/**
 * @file    module_socket.c
 * @brief   Lightweight, low-level wrapper around the standard Berkley sockets API.
 * @author  K. Lange <klange@toaruos.org>
 *
 */
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif
#include <errno.h>
#include "vm.h"
#include "util.h"

static KrkClass * SocketError = NULL;
static KrkClass * SocketClass = NULL;

struct socket {
	KrkInstance inst;

	int sockfd;
	int family;
	int type;
	int proto;
};

#ifndef AF_INET6
#define AF_actual_INET6 -1
#else
#define AF_actual_INET6 AF_INET6
#endif

#ifndef AF_UNIX
#define AF_actual_UNIX -1
#else
#define AF_actual_UNIX AF_UNIX
#endif

#define IS_socket(o) (krk_isInstanceOf(o,SocketClass))
#define AS_socket(o) ((struct socket*)AS_OBJECT(o))
#define CURRENT_CTYPE struct socket *
#define CURRENT_NAME  self

#define NAMED_ARG(name,type,ctype,def,ind) \
	ctype name = def; \
	if (argc > ind) { \
		CHECK_ARG(ind,type,ctype,_tmp); \
		name = _tmp; \
	} \
	if (hasKw) { \
		KrkValue tmp; \
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S(#name)), &tmp)) { \
			if (!IS_ ## type (tmp)) return TYPE_ERROR(type,tmp); \
			name = AS_ ## type (tmp); \
		} \
	}

KRK_METHOD(socket,__init__,{
	METHOD_TAKES_AT_MOST(3);

	/* Complex argument processing time... */
	NAMED_ARG(family,int,krk_integer_type,AF_INET,1);
	NAMED_ARG(type,int,krk_integer_type,SOCK_STREAM,2);
	NAMED_ARG(proto,int,krk_integer_type,0,3);

	int result = socket(family,type,proto);

	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}

	self->sockfd = result;
	self->family = family;
	self->type   = type;
	self->proto  = proto;

	return argv[0];
})

static char * _af_name(int afval) {
	static char tmp[30];
	switch (afval) {
		case AF_INET: return "AF_INET";
#ifdef AF_INET6
		case AF_INET6: return "AF_INET6";
#endif
#ifdef AF_UNIX
		case AF_UNIX: return "AF_UNIX";
#endif
		default:
			snprintf(tmp,30,"%d",afval);
			return tmp;
	}
}

static char * _sock_type(int type) {
	static char tmp[30];
	switch (type) {
		case SOCK_STREAM: return "SOCK_STREAM";
		case SOCK_DGRAM: return "SOCK_DGRAM";
#ifdef SOCK_RAW
		case SOCK_RAW: return "SOCK_RAW";
#endif
		default:
			snprintf(tmp,30,"%d",type);
			return tmp;
	}
}

KRK_METHOD(socket,__repr__,{
	char tmp[4096];
	size_t len = snprintf(tmp, 4096, "<socket.socket fd=%d, family=%s, type=%s, proto=%d>",
		self->sockfd, _af_name(self->family), _sock_type(self->type), self->proto);
	return OBJECT_VAL(krk_copyString(tmp,len));
})

static int socket_parse_address(struct socket * self, KrkValue address, struct sockaddr_storage *sock_addr, socklen_t *sock_size) {
	if (self->family == AF_INET) {
		/* Should be 2-tuple */
		if (!IS_tuple(address)) {
			krk_runtimeError(vm.exceptions->typeError, "Expected 2-tuple, not '%s'", krk_typeName(address));
			return 1;
		}
		KrkTuple * addr = AS_TUPLE(address);
		if (addr->values.count != 2) {
			krk_runtimeError(vm.exceptions->typeError, "Expected 2-tuple, not '%s'", krk_typeName(address));
			return 1;
		}
		if (!IS_str(addr->values.values[0])) {
			krk_runtimeError(vm.exceptions->typeError, "Address should be int, not '%s'", krk_typeName(addr->values.values[0]));
			return 1;
		}
		if (!IS_int(addr->values.values[1])) {
			krk_runtimeError(vm.exceptions->typeError, "Port should be int, not '%s'", krk_typeName(addr->values.values[1]));
			return 1;
		}

		if (!AS_STRING(addr->values.values[0])->length) {
			struct sockaddr_in * sin = (struct sockaddr_in*)sock_addr;
			*sock_size = sizeof(struct sockaddr_in);
			sin->sin_family = AF_INET;
			sin->sin_port = htons(AS_int(addr->values.values[1]));
			sin->sin_addr.s_addr = INADDR_ANY;
			return 0;
		} else {
			struct addrinfo *result;
			struct addrinfo *res;
			int error = getaddrinfo(AS_CSTRING(addr->values.values[0]), NULL, NULL, &result);
			if (error != 0) {
				krk_runtimeError(SocketError, "getaddrinfo() returned error: %d", error);
				return 1;
			}

			int found = 0;
			res = result;
			while (res) {
				if (res->ai_family == AF_INET) {
					found = 1;
					*sock_size = res->ai_addrlen;
					memcpy(sock_addr, res->ai_addr, *sock_size);
					break;
				}
				res = res->ai_next;
			}

			freeaddrinfo(result);

			if (!found) {
				krk_runtimeError(SocketError, "no suitable address");
				return 1;
			}

			struct sockaddr_in * sin = (struct sockaddr_in*)sock_addr;
			sin->sin_family = AF_INET;
			sin->sin_port = htons(AS_int(addr->values.values[1]));

			return 0;
		}
	} else {
		krk_runtimeError(vm.exceptions->notImplementedError, "Not implemented.");
		return 1;
	}

	return 1;
}

KRK_METHOD(socket,connect,{
	METHOD_TAKES_EXACTLY(1);

	struct sockaddr_storage sock_addr;
	socklen_t sock_size = 0;

	/* What do we take? I guess a tuple for AF_INET */
	int parseResult = socket_parse_address(self, argv[1], &sock_addr, &sock_size);
	if (parseResult) {
		if (!(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION))
			return krk_runtimeError(SocketError, "Unspecified error.");
		return NONE_VAL();
	}

	int result = connect(self->sockfd, (struct sockaddr*)&sock_addr, sock_size);

	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}
})

KRK_METHOD(socket,bind,{
	METHOD_TAKES_EXACTLY(1);

	struct sockaddr_storage sock_addr;
	socklen_t sock_size = 0;

	/* What do we take? I guess a tuple for AF_INET */
	int parseResult = socket_parse_address(self, argv[1], &sock_addr, &sock_size);
	if (parseResult) {
		if (!(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION))
			return krk_runtimeError(SocketError, "Unspecified error.");
		return NONE_VAL();
	}

	int result = bind(self->sockfd, (struct sockaddr*)&sock_addr, sock_size);

	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}
})

KRK_METHOD(socket,listen,{
	METHOD_TAKES_AT_MOST(1);
	int backlog = 0;
	if (argc > 1) {
		CHECK_ARG(1,int,krk_integer_type,val);
		backlog = val >= 0 ? val : 0;
	}

	int result = listen(self->sockfd, backlog);
	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}
})

KRK_METHOD(socket,accept,{
	struct sockaddr_storage addr;
	socklen_t addrlen;

	int result = accept(self->sockfd, (struct sockaddr*)&addr, &addrlen);

	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}

	KrkTuple * outTuple = krk_newTuple(2);
	krk_push(OBJECT_VAL(outTuple));

	struct socket * out = (struct socket*)krk_newInstance(SocketClass);
	krk_push(OBJECT_VAL(out));

	out->sockfd = result;
	out->family = self->family;
	out->type   = self->type;
	out->proto  = self->proto;

	outTuple->values.values[0] = krk_peek(0);
	outTuple->values.count = 1;
	krk_pop();

	KrkTuple * addrTuple = krk_newTuple(2); /* TODO: Other formats */
	krk_push(OBJECT_VAL(addrTuple));

	if (self->family == AF_INET) {
		char hostname[NI_MAXHOST] = "";
		getnameinfo((struct sockaddr*)&addr, addrlen, hostname, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

		addrTuple->values.values[0] = OBJECT_VAL(krk_copyString(hostname,strlen(hostname)));
		addrTuple->values.count = 1;
		addrTuple->values.values[1] = INTEGER_VAL(htons(((struct sockaddr_in*)&addr)->sin_port));
		addrTuple->values.count = 2;
	} else {
		krk_push(NONE_VAL());
	}

	outTuple->values.values[1] = krk_peek(0);
	outTuple->values.count = 2;
	krk_pop();

	return krk_pop();
})

KRK_METHOD(socket,shutdown,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,how);

	int result = shutdown(self->sockfd, how);

	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}
})

KRK_METHOD(socket,recv,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	CHECK_ARG(1,int,krk_integer_type,bufsize);
	int flags = 0;
	if (argc > 2) {
		CHECK_ARG(2,int,krk_integer_type,_flags);
		flags = _flags;
	}

	void * buf = malloc(bufsize);
	ssize_t result = recv(self->sockfd, buf, bufsize, flags);
	if (result < 0) {
		free(buf);
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}

	KrkBytes * out = krk_newBytes(result,buf);
	free(buf);
	return OBJECT_VAL(out);
})

KRK_METHOD(socket,send,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	CHECK_ARG(1,bytes,KrkBytes*,buf);
	int flags = 0;
	if (argc > 2) {
		CHECK_ARG(2,int,krk_integer_type,_flags);
		flags = _flags;
	}

	ssize_t result = send(self->sockfd, (void*)buf->bytes, buf->length, flags);
	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}

	return INTEGER_VAL(result);
})

KRK_METHOD(socket,fileno,{
	return INTEGER_VAL(self->sockfd);
})

KRK_METHOD(socket,setsockopt,{
	METHOD_TAKES_EXACTLY(3);
	CHECK_ARG(1,int,krk_integer_type,level);
	CHECK_ARG(2,int,krk_integer_type,optname);

	int result;

	if (IS_INTEGER(argv[3])) {
		int val = AS_INTEGER(argv[3]);
		result = setsockopt(self->sockfd, level, optname, (void*)&val, sizeof(int));
	} else if (IS_BYTES(argv[3])) {
		result = setsockopt(self->sockfd, level, optname, (void*)AS_BYTES(argv[3])->bytes, AS_BYTES(argv[3])->length);
	} else {
		return TYPE_ERROR(int or bytes,argv[3]);
	}

	if (result < 0) {
		return krk_runtimeError(SocketError, "Socket error: %s", strerror(errno));
	}

})

KRK_FUNC(htons,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,value);
	return INTEGER_VAL(htons(value));
})

KrkValue krk_module_onload_socket(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_push(OBJECT_VAL(module));

	KRK_DOC(module, "Lightweight wrapper around the standard Berkeley sockets interface.");

	KrkClass * socket = krk_makeClass(module, &SocketClass, "socket", vm.baseClasses->objectClass);
	SocketClass->allocSize = sizeof(struct socket);
	BIND_METHOD(socket,__init__);
	BIND_METHOD(socket,__repr__);
	BIND_METHOD(socket,bind);
	BIND_METHOD(socket,listen);
	BIND_METHOD(socket,accept);
	BIND_METHOD(socket,connect);
	BIND_METHOD(socket,shutdown);
	BIND_METHOD(socket,recv);
	BIND_METHOD(socket,send);
	BIND_METHOD(socket,fileno);
	BIND_METHOD(socket,setsockopt);
	krk_defineNative(&socket->methods,".__str__", FUNC_NAME(socket,__repr__));
	krk_finalizeClass(SocketClass);

	BIND_FUNC(module, htons);

	/* Constants */
#define SOCK_CONST(o) krk_attachNamedValue(&module->fields, #o, INTEGER_VAL(o));

	/**
	 * AF_ constants
	 * Taken from the manpages for Linux, and all shoved behind ifdefs, so this
	 * should build fine on most anything even if most of these aren't widely
	 * supported by other platforms.
	 */
	SOCK_CONST(AF_INET);
#ifdef AF_INET6
	SOCK_CONST(AF_INET6);
#endif
#ifdef AF_UNIX
	SOCK_CONST(AF_UNIX);
#endif

	/* SOCK_ constants, similarly */
	SOCK_CONST(SOCK_STREAM);
	SOCK_CONST(SOCK_DGRAM);
#ifdef SOCK_RAW
	SOCK_CONST(SOCK_RAW);
#endif

	/* These are OR'd together with the above on Linux */
#ifdef SOCK_NONBLOCK
	SOCK_CONST(SOCK_NONBLOCK);
#endif
#ifdef SOCK_CLOEXEC
	SOCK_CONST(SOCK_CLOEXEC);
#endif

#ifdef SHUT_RD
	SOCK_CONST(SHUT_RD);
	SOCK_CONST(SHUT_WR);
	SOCK_CONST(SHUT_RDWR);
#endif

	SOCK_CONST(SOL_SOCKET);

	SOCK_CONST(SO_REUSEADDR);

	krk_makeClass(module, &SocketError, "SocketError", vm.exceptions->baseException);
	KRK_DOC(SocketError, "Raised on faults from socket functions.");
	krk_finalizeClass(SocketError);

	return krk_pop();
}
