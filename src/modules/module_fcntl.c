#include <errno.h>
#include <fcntl.h>

#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/util.h>

KRK_Function(fcntl) {
	KrkValue fd_value;
	int cmd;
	KrkValue arg_value = NONE_VAL();
	if (!krk_parseArgs("Vi|V",
		(const char*[]){"fd","cmd","arg"},
		&fd_value, &cmd, &arg_value)) {
		return NONE_VAL();
	}
	int fd;
	if (IS_INTEGER(fd_value)) {
		fd = AS_INTEGER(fd_value);
	} else if (IS_INSTANCE(fd_value)){
		krk_push(fd_value);
		if (!krk_bindMethod(krk_getType(fd_value), S("fileno"))) {
			return krk_runtimeError(KRK_EXC(typeError),
				"no fileno() method on '%T'", fd_value);
		}
		KrkValue fileno = krk_callStack(0);
		if (!IS_INTEGER(fileno)) {
			return krk_runtimeError(KRK_EXC(typeError),
				"fileno() returned non-integer '%T'", fileno);
		}
		fd = AS_INTEGER(fileno);
	} else {
		return krk_runtimeError(KRK_EXC(typeError),
			"expected integer or object with fileno(), not '%T'", fileno);
	}
	intptr_t arg;
	KrkValue arg_copy = NONE_VAL();
	if (IS_NONE(arg_value)) {
		arg = 0;
	} else if (IS_INTEGER(arg_value)) {
		arg = AS_INTEGER(arg_value);
	} else if (IS_BYTES(arg_value)) {
		KrkBytes *bytes = AS_BYTES(arg_value);
		KrkBytes *copy = krk_newBytes(bytes->length, bytes->bytes);
		arg = (intptr_t)copy->bytes;
		arg_copy = OBJECT_VAL(copy);
	} else {
		return krk_runtimeError(KRK_EXC(typeError),
			"expected integer or bytes arg, not '%T'", arg_value);
	}
	int result;
	do {
		result = fcntl(fd, cmd, arg);
	} while (result == EINTR);
	if (result < 0) {
		return krk_runtimeError(KRK_EXC(OSError), "%s", strerror(errno));
	}
	if (IS_NONE(arg_value) || IS_INTEGER(arg_value)) {
		return INTEGER_VAL(result);
	} else {
		return arg_copy;
	}
}

KrkValue krk_module_onload_fcntl(void) {
	KrkInstance *module = krk_newInstance(KRK_BASE_CLASS(module));
	krk_push(OBJECT_VAL(module));

	KRK_DOC(module, "@brief Provides access to file descriptor duplicate, query, modify, lock, and unlock operations.");

	KRK_DOC(BIND_FUNC(module,fcntl),
		"@brief Duplicate, query, modify, lock or unlock descriptor @c fd depending on the value of @c cmd.\n"
		"@arguments fd,cmd,arg=None\n\n"
		"@p fd must be a file descriptor or an object with a @c fileno method. "
		"@p cmd should be an integer value defined by the @c F options. "
		"@p arg must be an integer value or bytes if present. "
		"@returns @ref int or @ref bytes");

#define FCNTL_CONST(c) krk_attachNamedValue(&module->fields, #c, INTEGER_VAL(c))

	FCNTL_CONST(F_DUPFD);
	FCNTL_CONST(F_DUPFD_CLOEXEC);
	FCNTL_CONST(F_GETFD);
	FCNTL_CONST(F_SETFD);
	FCNTL_CONST(F_GETFL);
	FCNTL_CONST(F_SETFL);
	FCNTL_CONST(F_GETOWN);
	FCNTL_CONST(F_SETOWN);
	FCNTL_CONST(F_GETLK);
	FCNTL_CONST(F_SETLK);
	FCNTL_CONST(F_SETLKW);
	
	FCNTL_CONST(FD_CLOEXEC);
	FCNTL_CONST(F_RDLCK);
	FCNTL_CONST(F_UNLCK);
	FCNTL_CONST(F_WRLCK);

	return krk_pop();
}
