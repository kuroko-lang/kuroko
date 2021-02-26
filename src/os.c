#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/utsname.h>
#else
#include <windows.h>
#endif

#include "vm.h"
#include "value.h"
#include "object.h"
#include "util.h"

/* Did you know this is actually specified to not exist in a header? */
extern char ** environ;

KrkClass * OSError = NULL;
KrkClass * stat_result = NULL;

#define DO_KEY(key) krk_attachNamedObject(AS_DICT(result), #key, (KrkObj*)krk_copyString(buf. key, strlen(buf .key)))
#define S_KEY(key,val) krk_attachNamedObject(AS_DICT(result), #key, (KrkObj*)val);

#ifndef _WIN32
KRK_FUNC(uname,{
	struct utsname buf;
	if (uname(&buf) < 0) return NONE_VAL();

	KrkValue result = krk_dict_of(0, NULL, 0);
	krk_push(result);

	DO_KEY(sysname);
	DO_KEY(nodename);
	DO_KEY(release);
	DO_KEY(version);
	DO_KEY(machine);

	return krk_pop();;
})
#else
KRK_FUNC(uname,{
	KrkValue result = krk_dict_of(0, NULL, 0);
	krk_push(result);

	TCHAR buffer[256] = TEXT("");
	DWORD dwSize = sizeof(buffer);
	GetComputerName(buffer, &dwSize);

	OSVERSIONINFOA versionInfo = {0};
	versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionExA(&versionInfo);

	if (versionInfo.dwMajorVersion == 10) {
		S_KEY(release,S("10"));
	} else if (versionInfo.dwMajorVersion == 6) {
		if (versionInfo.dwMinorVersion == 3) {
			S_KEY(release,S("8.1"));
		} else if (versionInfo.dwMinorVersion == 2) {
			S_KEY(release,S("8.0"));
		} else if (versionInfo.dwMinorVersion == 1) {
			S_KEY(release,S("7"));
		} else if (versionInfo.dwMinorVersion == 0) {
			S_KEY(release,S("Vista"));
		}
	} else {
		S_KEY(release,S("XP or earlier"));
	}

	char tmp[256];
	size_t len = snprintf(tmp, 256, "%ld", versionInfo.dwBuildNumber);

	S_KEY(version, krk_copyString(tmp,len));
	if (sizeof(void *) == 8) {
		S_KEY(machine,S("x64"));
	} else {
		S_KEY(machine,S("x86"));
	}

	S_KEY(sysname,S("Windows"));
	S_KEY(nodename,krk_copyString(buffer,dwSize));

	return krk_pop();
})
#endif

KrkClass * environClass;

KrkValue krk_os_setenviron(int argc, KrkValue argv[], int hasKw) {
	if (argc < 3 || !krk_isInstanceOf(argv[0], environClass) ||
		!IS_STRING(argv[1]) || !IS_STRING(argv[2])) {
		return krk_runtimeError(vm.exceptions->argumentError, "Invalid arguments to environ.__set__");
	}
	/* Set environment variable */
	size_t len = AS_STRING(argv[1])->length + AS_STRING(argv[2])->length + 2;
	char * tmp = malloc(len);
	snprintf(tmp, len, "%s=%s", AS_CSTRING(argv[1]), AS_CSTRING(argv[2]));
	int r = putenv(tmp);
	if (r == 0) {
		/* Make super call */
		krk_push(argv[0]);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callSimple(OBJECT_VAL(vm.baseClasses->dictClass->_setter), 3, 0);
	} else {
		return krk_runtimeError(OSError, strerror(errno));
	}
}

KrkValue krk_os_unsetenviron(int argc, KrkValue argv[], int hasKw) {
	if (argc < 2 || !krk_isInstanceOf(argv[0], environClass) ||
		!IS_STRING(argv[1])) {
		return krk_runtimeError(vm.exceptions->argumentError, "Invalid arguments to environ.__delitem__");
	}
#ifndef _WIN32
	unsetenv(AS_CSTRING(argv[1]));
#else
	size_t len = AS_STRING(argv[1])->length + 2;
	char * tmp = malloc(len);
	snprintf(tmp, len, "%s=", AS_CSTRING(argv[1]));
	putenv(tmp);
	free(tmp);
#endif
	krk_push(argv[0]);
	krk_push(argv[1]);
	return krk_callSimple(OBJECT_VAL(vm.baseClasses->dictClass->_delitem), 2, 0);
}

static void _loadEnviron(KrkInstance * module) {
	/* Create a new class to subclass `dict` */
	KrkString * className = S("_Environ");
	krk_push(OBJECT_VAL(className));
	environClass = krk_newClass(className, vm.baseClasses->dictClass);
	krk_attachNamedObject(&module->fields, "_Environ", (KrkObj*)environClass);
	krk_pop(); /* className */

	/* Add our set method that should also call dict's set method */
	krk_defineNative(&environClass->methods, ".__set__", krk_os_setenviron);
	krk_defineNative(&environClass->methods, ".__delitem__", krk_os_unsetenviron);
	krk_finalizeClass(environClass);

	/* Start with an empty dictionary */
	KrkInstance * environObj = AS_INSTANCE(krk_dict_of(0,NULL,0));
	krk_push(OBJECT_VAL(environObj));

	/* Transform it into an _Environ */
	environObj->_class = environClass;

	/* And attach it to the module */
	krk_attachNamedObject(&module->fields, "environ", (KrkObj*)environObj);
	krk_pop();

	/* Now load the environment into it */
	if (!environ) return; /* Empty environment */

	char ** env = environ;
	for (; *env; env++) {
		const char * equals = strchr(*env, '=');
		if (!equals) continue;

		size_t len = strlen(*env);
		size_t keyLen = equals - *env;
		size_t valLen = len - keyLen - 1;

		KrkValue key = OBJECT_VAL(krk_copyString(*env, keyLen));
		krk_push(key);
		KrkValue val = OBJECT_VAL(krk_copyString(equals+1, valLen));
		krk_push(val);

		krk_tableSet(AS_DICT(OBJECT_VAL(environObj)), key, val);
		krk_pop(); /* val */
		krk_pop(); /* key */
	}

}

KRK_FUNC(system,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,cmd);
	return INTEGER_VAL(system(cmd->chars));
})

KRK_FUNC(getcwd,{
	FUNCTION_TAKES_NONE();
	char buf[4096]; /* TODO PATH_MAX? */
	if (!getcwd(buf, 4096)) return krk_runtimeError(OSError, strerror(errno));
	return OBJECT_VAL(krk_copyString(buf, strlen(buf)));
})

KRK_FUNC(chdir,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,newDir);
	if (chdir(newDir->chars)) return krk_runtimeError(OSError, strerror(errno));
})

KRK_FUNC(getpid,{
	FUNCTION_TAKES_NONE();
	return INTEGER_VAL(getpid());
})

KRK_FUNC(strerror,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,errorNo);
	char *s = strerror(errorNo);
	if (!s) return NONE_VAL();
	return OBJECT_VAL(krk_copyString(s,strlen(s)));
})

KRK_FUNC(access,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,str,KrkString*,path);
	CHECK_ARG(1,int,krk_integer_type,mask);
	if (access(path->chars, mask) == 0) return BOOLEAN_VAL(1);
	return BOOLEAN_VAL(0);
})

KRK_FUNC(abort,{
	abort();
})

KRK_FUNC(remove,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,path);
	if (remove(path->chars) != 0) {
		return krk_runtimeError(OSError, strerror(errno));
	}
})

KRK_FUNC(truncate,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,str,KrkString*,path);
	CHECK_ARG(1,int,krk_integer_type,length);
	if (truncate(path->chars, length) != 0) {
		return krk_runtimeError(OSError, strerror(errno));
	}
})

KRK_FUNC(dup,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,fd);
	int result = dup(fd);
	if (result < 0) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

KRK_FUNC(dup2,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,int,krk_integer_type,fd);
	CHECK_ARG(1,int,krk_integer_type,fd2);
	int result = dup2(fd,fd2);
	if (result < 0) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

KRK_FUNC(isatty,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,fd);
	return BOOLEAN_VAL(isatty(fd));
})

KRK_FUNC(lseek,{
	FUNCTION_TAKES_EXACTLY(3);
	CHECK_ARG(0,int,krk_integer_type,fd);
	CHECK_ARG(1,int,krk_integer_type,pos);
	CHECK_ARG(2,int,krk_integer_type,how);
	off_t result = lseek(fd,pos,how);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

KRK_FUNC(open,{
	FUNCTION_TAKES_AT_LEAST(2);
	FUNCTION_TAKES_AT_MOST(3);
	CHECK_ARG(0,str,KrkString*,path);
	CHECK_ARG(1,int,krk_integer_type,flags);
	int mode = 0777;
	if (argc == 3) {
		CHECK_ARG(2,int,krk_integer_type,_mode);
		mode = _mode;
	}
	int result = open(path->chars, flags, mode);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

KRK_FUNC(close,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,fd);
	if (close(fd) == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
})

#ifdef _WIN32
#define mkdir(p,m) mkdir(p); (void)m
#endif
KRK_FUNC(mkdir,{
	FUNCTION_TAKES_AT_LEAST(1);
	FUNCTION_TAKES_AT_MOST(2);
	CHECK_ARG(0,str,KrkString*,path);
	int mode = 0777;
	if (argc > 1) {
		CHECK_ARG(1,int,krk_integer_type,_mode);
		mode = _mode;
	}
	int result = mkdir(path->chars, mode);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
})

KRK_FUNC(read,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,int,krk_integer_type,fd);
	CHECK_ARG(1,int,krk_integer_type,n);

	uint8_t * tmp = malloc(n);
	ssize_t result = read(fd,tmp,n);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	} else {
		krk_push(OBJECT_VAL(krk_newBytes(result,tmp)));
		free(tmp);
		return krk_pop();
	}
})

#ifndef IS_bytes
#define IS_bytes(o) IS_BYTES(o)
#define AS_bytes(o) AS_BYTES(o)
#endif

KRK_FUNC(write,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,int,krk_integer_type,fd);
	CHECK_ARG(1,bytes,KrkBytes*,data);
	ssize_t result = write(fd,data->bytes,data->length);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

#ifndef _WIN32
KRK_FUNC(pipe,{
	FUNCTION_TAKES_NONE();
	int fds[2];
	if (pipe(fds) == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	krk_push(OBJECT_VAL(krk_newTuple(2)));
	AS_TUPLE(krk_peek(0))->values.values[0] = INTEGER_VAL(fds[0]);
	AS_TUPLE(krk_peek(0))->values.values[1] = INTEGER_VAL(fds[1]);
	AS_TUPLE(krk_peek(0))->values.count = 2;
	krk_tupleUpdateHash(AS_TUPLE(krk_peek(0)));
	return krk_pop();
})

KRK_FUNC(sync,{
	FUNCTION_TAKES_NONE();
	sync();
})

KRK_FUNC(kill,{
	FUNCTION_TAKES_EXACTLY(2);
	int result = kill(AS_INTEGER(argv[0]), AS_INTEGER(argv[1]));
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

KRK_FUNC(fork,{
	FUNCTION_TAKES_NONE();
	return INTEGER_VAL(fork());
})

KRK_FUNC(symlink,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,str,KrkString*,src);
	CHECK_ARG(1,str,KrkString*,dst);
	if (symlink(src->chars, dst->chars) != 0) {
		return krk_runtimeError(OSError, strerror(errno));
	}
})

KRK_FUNC(tcgetpgrp,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,fd);
	int result = tcgetpgrp(fd);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return INTEGER_VAL(result);
})

KRK_FUNC(tcsetpgrp,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,int,krk_integer_type,fd);
	CHECK_ARG(1,int,krk_integer_type,pgrp);
	int result = tcsetpgrp(fd,pgrp);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
})

KRK_FUNC(ttyname,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,fd);
	char * result = ttyname(fd);
	if (!result) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return OBJECT_VAL(krk_copyString(result,strlen(result)));
})
#endif

static int makeArgs(int count, KrkValue * values, char *** argsOut) {
	char ** out = malloc(sizeof(char*)*count);
	for (int i = 0; i < count; ++i) {
		if (!IS_STRING(values[i])) {
			free(out);
			TYPE_ERROR(str,values[i]);
			return 1;
		}
		out[i] = AS_CSTRING(values[i]);
	}
	*argsOut = out;
	return 0;
}

KRK_FUNC(execl,{
	FUNCTION_TAKES_AT_LEAST(1);
	CHECK_ARG(0,str,KrkString*,path);
	char ** args;
	if (makeArgs(argc-1,&argv[1],&args)) return NONE_VAL();
	if (execv(path->chars, args) == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return krk_runtimeError(OSError, "Expected to not return from exec, but did.");
})

KRK_FUNC(execlp,{
	FUNCTION_TAKES_AT_LEAST(1);
	CHECK_ARG(0,str,KrkString*,filename);
	char ** args;
	if (makeArgs(argc-1,&argv[1],&args)) return NONE_VAL();
	if (execvp(filename->chars, args) == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return krk_runtimeError(OSError, "Expected to not return from exec, but did.");
})

KRK_FUNC(execle,{
	FUNCTION_TAKES_AT_LEAST(1);
	CHECK_ARG(0,str,KrkString*,path);
	CHECK_ARG((argc-1),list,KrkList*,envp);
	char ** args;
	char ** env;
	if (makeArgs(argc-2,&argv[1],&args)) return NONE_VAL();
	if (makeArgs(envp->values.count, envp->values.values,&env)) return NONE_VAL();
	if (execve(path->chars, args, env) == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	return krk_runtimeError(OSError, "Expected to not return from exec, but did.");
})

KRK_FUNC(execv,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,str,KrkString*,filename);
	CHECK_ARG(1,list,KrkList*,args);
	char ** argp;
	if (makeArgs(args->values.count, args->values.values, &argp)) return NONE_VAL();
	if (execv(filename->chars, argp) == -1) {
		free(argp);
		return krk_runtimeError(OSError, strerror(errno));
	}
	return krk_runtimeError(OSError, "Expected to not return from exec, but did.");
})

KRK_FUNC(execvp,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,str,KrkString*,path);
	CHECK_ARG(1,list,KrkList*,args);
	char ** argp;
	if (makeArgs(args->values.count, args->values.values, &argp)) return NONE_VAL();
	if (execvp(path->chars, argp) == -1) {
		free(argp);
		return krk_runtimeError(OSError, strerror(errno));
	}
	return krk_runtimeError(OSError, "Expected to not return from exec, but did.");
})

#define SET(thing) krk_attachNamedValue(&out->fields, #thing, INTEGER_VAL(buf. thing))
#ifdef _WIN32
#define STAT_STRUCT struct __stat64
#define stat _stat64
#else
#define STAT_STRUCT struct stat
#endif
KRK_FUNC(stat,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,path);
	STAT_STRUCT buf;
	int result = stat(path->chars, &buf);
	if (result == -1) {
		return krk_runtimeError(OSError, strerror(errno));
	}
	KrkInstance * out = krk_newInstance(stat_result);
	krk_push(OBJECT_VAL(out));

	SET(st_dev);
	SET(st_ino);
	SET(st_mode);
	SET(st_nlink);
	SET(st_uid);
	SET(st_gid);
	SET(st_size);

	/* TODO times */
	/* TODO block sizes */

	return krk_pop();
})
#undef SET

#define IS_stat_result(o) (krk_isInstanceOf(o,stat_result))
#define AS_stat_result(o) AS_INSTANCE(o)
#define CURRENT_NAME  self
#define CURRENT_CTYPE KrkInstance *

#define getProp(name) \
	KrkValue name = NONE_VAL(); \
	krk_tableGet(&self->fields, OBJECT_VAL(S(#name)), &name); \
	if (!IS_INTEGER(name)) return krk_runtimeError(vm.exceptions->valueError, "stat_result is invalid")

KRK_METHOD(stat_result,__repr__,{
	METHOD_TAKES_NONE();
	getProp(st_dev);
	getProp(st_ino);
	getProp(st_mode);
	getProp(st_nlink);
	getProp(st_uid);
	getProp(st_gid);
	getProp(st_size);

	char * buf = malloc(1024);
	size_t len = snprintf(buf,1024,
		"os.stat_result("
			"st_dev=%d,"
			"st_ino=%d,"
			"st_mode=%d,"
			"st_nlink=%d,"
			"st_uid=%d,"
			"st_gid=%d,"
			"st_size=%d)",
		(int)AS_INTEGER(st_dev),
		(int)AS_INTEGER(st_ino),
		(int)AS_INTEGER(st_mode),
		(int)AS_INTEGER(st_nlink),
		(int)AS_INTEGER(st_uid),
		(int)AS_INTEGER(st_gid),
		(int)AS_INTEGER(st_size));

	if (len > 1023) len = 1023;
	return OBJECT_VAL(krk_copyString(buf,len));
})

KRK_FUNC(S_ISBLK,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISBLK(mode));
})
KRK_FUNC(S_ISCHR,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISCHR(mode));
})
KRK_FUNC(S_ISDIR,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISDIR(mode));
})
KRK_FUNC(S_ISFIFO,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISFIFO(mode));
})
KRK_FUNC(S_ISREG,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISREG(mode));
})
#ifndef _WIN32
KRK_FUNC(S_ISLNK,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISLNK(mode));
})
KRK_FUNC(S_ISSOCK,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,mode);
	return INTEGER_VAL(S_ISSOCK(mode));
})
#endif

_noexport
void _createAndBind_osMod(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "os", (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)S("os"));
	krk_attachNamedValue(&module->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&module->fields, "__doc__",
		(KrkObj*)S("@brief Provides access to low-level system operations."));

#ifdef _WIN32
	krk_attachNamedObject(&module->fields, "name", (KrkObj*)S("nt"));
	krk_attachNamedObject(&module->fields, "sep", (KrkObj*)S("\\"));
	krk_attachNamedObject(&module->fields, "altsep", (KrkObj*)S("/"));
	krk_attachNamedObject(&module->fields, "pathsep", (KrkObj*)S(";"));
	krk_attachNamedObject(&module->fields, "linesep", (KrkObj*)S("\r\n"));
	krk_attachNamedObject(&module->fields, "devnull", (KrkObj*)S("nul"));
#else
	krk_attachNamedObject(&module->fields, "name", (KrkObj*)S("posix"));
	krk_attachNamedObject(&module->fields, "sep", (KrkObj*)S("/"));
	krk_attachNamedValue(&module->fields, "altsep", NONE_VAL());
	krk_attachNamedObject(&module->fields, "pathsep", (KrkObj*)S(":"));
	krk_attachNamedObject(&module->fields, "linesep", (KrkObj*)S("\n"));
	krk_attachNamedObject(&module->fields, "devnull", (KrkObj*)S("/dev/null"));
#endif

	krk_attachNamedObject(&module->fields, "curdir", (KrkObj*)S("."));
	krk_attachNamedObject(&module->fields, "pardir", (KrkObj*)S(".."));
	krk_attachNamedObject(&module->fields, "extsep", (KrkObj*)S("."));

#define DO_INT(name) krk_attachNamedValue(&module->fields, #name, INTEGER_VAL(name))

	DO_INT(O_RDONLY);
	DO_INT(O_WRONLY);
	DO_INT(O_RDWR);
	DO_INT(O_APPEND);
	DO_INT(O_CREAT);
	DO_INT(O_EXCL);
	DO_INT(O_TRUNC);

#ifdef O_CLOEXEC
	DO_INT(O_CLOEXEC);
#endif
#ifdef O_DIRECTORY
	DO_INT(O_DIRECTORY);
#endif
#ifdef O_PATH
	DO_INT(O_PATH);
#endif
#ifdef O_NOFOLLOW
	DO_INT(O_NOFOLLOW);
#endif
#ifdef O_NONBLOCK
	DO_INT(O_NONBLOCK);
#endif

	DO_INT(SEEK_SET);
	DO_INT(SEEK_CUR);
	DO_INT(SEEK_END);

#ifdef SEEK_HOLE
	DO_INT(SEEK_HOLE);
#endif
#ifdef SEEK_DATA
	DO_INT(SEEK_DATA);
#endif

	krk_makeClass(module, &OSError, "OSError", vm.exceptions->baseException);
	OSError->docstring = S(
		"Raised when system functions return a failure code. @p Exception.arg will provide a textual description of the error."
	);
	krk_finalizeClass(OSError);

	BIND_FUNC(module,uname)->doc = "@brief Returns a @ref dict of attributes describing the current platform.\n\n"
		"On POSIX platforms, the result should match the contents and layout of a standard @c uname() call. "
		"On Windows, values are synthesized from available information.";
	BIND_FUNC(module,system)->doc = "@brief Call the system shell.\n"
		"@arguments cmd\n\n"
		"Runs @p cmd using the system shell and returns the platform-dependent return value.";
	BIND_FUNC(module,getcwd)->doc = "@brief Get the name of the current working directory.";
	BIND_FUNC(module,chdir)->doc = "@brief Change the current working directory.\n"
		"@arguments newcwd\n\n"
		"Attempts to change the working directory to @p newcwd. Raises @ref OSError on failure.";
	BIND_FUNC(module,getpid)->doc = "@brief Obtain the system process identifier.";
	BIND_FUNC(module,strerror)->doc = "@brief Convert an integer error code to a string.\n"
		"@arguments errorno\n\n"
		"Provides the string description for the error code specified by @p errorno.";
	BIND_FUNC(module,abort)->doc = "@brief Abort the current process.\n\n"
		"@bsnote{This will exit the interpreter without calling cleanup routines.}";
	BIND_FUNC(module,remove)->doc = "@brief Delete a file.\n"
		"@arguments path\n\n"
		"Attempts to delete the file at @p path.";
	BIND_FUNC(module,truncate)->doc = "@brief Resize a file.\n"
		"@arguments path,length\n\n"
		"Attempts to resize the file at @p path to @p length bytes.";
	BIND_FUNC(module,dup)->doc = "@brief Duplicate a file descriptor.\n"
		"@arguments fd\n\n"
		"Returns a new file descriptor pointing to the same file as @p fd.";
	BIND_FUNC(module,dup2)->doc = "@brief Duplicate a file descriptor.\n"
		"@arguments oldfd,newfd\n\n"
		"Like @ref dup but the new file descriptor is placed at @p newfd.\n";
	BIND_FUNC(module,isatty)->doc = "@brief Determine if a file descriptor is a terminal.\n"
		"@arguments fd\n\n"
		"Returns a @ref bool indicating whether the open file descriptor @p fd refers to a terminal.";
	BIND_FUNC(module,lseek)->doc = "@brief Seek an open file descriptor.\n"
		"@arguments fd,pos,how\n\n"
		"Seeks the open file descriptor @p fd by @p pos bytes as specified in @p how. "
		"Use the values @c SEEK_SET, @c SEEK_CUR, and @c SEEK_END for @p how.";
	BIND_FUNC(module,open)->doc = "@brief Open a file.\n"
		"@arguments path,flags,mode=0o777\n\n"
		"Opens the file at @p path with the specified @p flags and @p mode. Returns a file descriptor.\n\n"
		"@bsnote{Not to be confused with <a class=\"el\" href=\"mod_fileio.html#open\">fileio.open</a>}";
	BIND_FUNC(module,close)->doc = "@brief Close an open file descriptor.\n"
		"@arguments fd";
	BIND_FUNC(module,read)->doc = "@brief Read from an open file descriptor.\n"
		"@arguments fd,n\n\n"
		"Reads at most @p n bytes from the open file descriptor @p fd.";
	BIND_FUNC(module,write)->doc = "@brief Write to an open file descriptor.\n"
		"@arguments fd,data\n\n"
		"Writes the @ref bytes object @p data to the open file descriptor @p fd.";
	BIND_FUNC(module,mkdir)->doc = "@brief Create a directory.\n"
		"@arguments path,mode=0o777\n\n"
		"Creates a directory at @p path.";

	BIND_FUNC(module,execl)->doc = "@brief Replace the current process.\n"
		"@arguments path,[args...]\n\n"
		"The @c exec* family of functions replaces the calling process's image with a new one. "
		"@c execl takes a @p path to a binary and an arbitrary number of @ref str arguments to "
		"pass to the new executable.";
	BIND_FUNC(module,execle)->doc = "@brief Replace the current process.\n"
		"@arguments path,[args...],env\n\n"
		"The @c exec* family of functions replaces the calling process's image with a new one. "
		"@c execle takes a @p path to a binary, an arbitrary number of @ref str arguments to "
		"pass to the new executable, and @ref list of @c 'KEY=VALUE' pairs to set as the new "
		"environment.";
	BIND_FUNC(module,execlp)->doc = "@brief Replace the current process.\n"
		"@arguments filename,[args...]\n\n"
		"The @c exec* family of functions replaces the calling process's image with a new one. "
		"@c execlp takes a @p filename of a binary and an arbitrary number of @ref str arguments to "
		"pass to the new executable. @p filename will be searched for in @c $PATH.";
	BIND_FUNC(module,execv)->doc = "@brief Replace the current process.\n"
		"@arguments path,args\n\n"
		"The @c exec* family of functions replaces the calling process's image with a new one. "
		"@c execv takes a @p path to a binary and a @ref list @p args of @ref str arguments to "
		"pass to the new executable.";
	BIND_FUNC(module,execvp)->doc = "@brief Replace the current process.\n"
		"@arguments filename,args\n\n"
		"The @c exec* family of functions replaces the calling process's image with a new one. "
		"@c execvp takes a @p filename of a binary and a @ref list @p args of @ref str arguments to "
		"pass to the new executable. @p filename will be searched for in @c $PATH.";

	DO_INT(F_OK);
	DO_INT(R_OK);
	DO_INT(W_OK);
	DO_INT(X_OK);
	BIND_FUNC(module,access)->doc = "@brief Determine if a file can be accessed.\n"
		"@arguments path,mask\n\n"
		"Use the values @c F_OK, @c R_OK, @c W_OK, and @c X_OK to construct @p mask and check if the current "
		"process has sufficient access rights to perform the requested operations on the file "
		"at @p path.";

#ifndef _WIN32
	BIND_FUNC(module,pipe)->doc = "@brief Create a pipe.\n\n"
		"Creates a _pipe_, returning a two-tuple of file descriptors for the read and write ends respectively.";
	BIND_FUNC(module,kill)->doc = "@brief Send a signal to a process.\n"
		"@arguments pid,signum\n\n"
		"Send the signal @p signum to the process at @p pid.\n";
	BIND_FUNC(module,fork)->doc = "@brief Fork the current process.\n\n"
		"Returns the PID of the new child process in the original process and @c 0 in the child.";
	BIND_FUNC(module,symlink)->doc = "@brief Create a symbolic link.\n"
		"@arguments src,dst\n\n"
		"Creates a symbolic link at @p src pointing to @p dst.";
	BIND_FUNC(module,sync)->doc = "@brief Commit filesystem caches to disk.";

	BIND_FUNC(module,tcgetpgrp)->doc = "@brief Get the terminal foreground process group.\n"
		"@arguments fd\n\n"
		"Return the PID representing the foreground process group of the terminal specified by the file descriptor @p fd.";
	BIND_FUNC(module,tcsetpgrp)->doc = "@brief %Set the terminal foreground process group.\n"
		"@arguments fd,pgrp\n\n"
		"%Set the PID representing the foreground process group of the terminal specified by the file descriptor @p fd to @p pgrp.";
	BIND_FUNC(module,ttyname)->doc = "@brief Get the path to a terminal device.\n"
		"@arguments fd\n\n"
		"Returns a @ref str representing the path to the terminal device provided by the file descriptor @p fd.";
#endif

	_loadEnviron(module);

	/* Nothing special */
	krk_makeClass(module, &stat_result, "stat_result", vm.baseClasses->objectClass);
	BIND_METHOD(stat_result,__repr__);
	krk_finalizeClass(stat_result);

	BIND_FUNC(module,stat)->doc = "@brief Get the status of a file\n"
		"@arguments path\n\n"
		"Runs the @c stat system call on @p path. Returns a @ref stat_result.\n";

	module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "stat", (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)S("stat"));
	krk_attachNamedValue(&module->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&module->fields, "__doc__",
		(KrkObj*)S("@brief Functions to check results from @ref stat calls."));

	BIND_FUNC(module,S_ISBLK);
	BIND_FUNC(module,S_ISCHR);
	BIND_FUNC(module,S_ISDIR);
	BIND_FUNC(module,S_ISFIFO);
	BIND_FUNC(module,S_ISREG);
#ifndef _WIN32
	BIND_FUNC(module,S_ISLNK);
	BIND_FUNC(module,S_ISSOCK);
#endif
}


