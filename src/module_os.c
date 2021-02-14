/**
 * Currently just uname().
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
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
	sprintf(tmp, "%ld", versionInfo.dwBuildNumber);

	S_KEY(version, krk_copyString(tmp,strlen(tmp)));
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
	char * tmp = malloc(AS_STRING(argv[1])->length + AS_STRING(argv[2])->length + 2);
	sprintf(tmp, "%s=%s", AS_CSTRING(argv[1]), AS_CSTRING(argv[2]));
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
	char * tmp = malloc(AS_STRING(argv[1])->length + 2);
	sprintf(tmp, "%s=", AS_CSTRING(argv[1]));
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
	return INTEGER_VAL(kill(AS_INTEGER(argv[0]), AS_INTEGER(argv[1])));
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
	FUNCTION_TAKES_AT_LEAST(1);
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
	FUNCTION_TAKES_AT_LEAST(1);
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

KrkValue krk_module_onload_os(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

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
	krk_finalizeClass(OSError);

	BIND_FUNC(module,uname);
	BIND_FUNC(module,system);
	BIND_FUNC(module,getcwd);
	BIND_FUNC(module,chdir);
	BIND_FUNC(module,getpid);
	BIND_FUNC(module,strerror);
	BIND_FUNC(module,abort);
	BIND_FUNC(module,remove);
	BIND_FUNC(module,truncate);
	BIND_FUNC(module,dup);
	BIND_FUNC(module,dup2);
	BIND_FUNC(module,isatty);
	BIND_FUNC(module,lseek);
	BIND_FUNC(module,open);
	BIND_FUNC(module,close);
	BIND_FUNC(module,read);
	BIND_FUNC(module,write);

	BIND_FUNC(module,execl);
	BIND_FUNC(module,execle);
	BIND_FUNC(module,execlp);
	BIND_FUNC(module,execv);
	BIND_FUNC(module,execvp);

	DO_INT(F_OK);
	DO_INT(R_OK);
	DO_INT(W_OK);
	DO_INT(X_OK);
	BIND_FUNC(module,access);

#ifndef _WIN32
	BIND_FUNC(module,pipe);
	BIND_FUNC(module,kill);
	BIND_FUNC(module,fork);
	BIND_FUNC(module,symlink);
	BIND_FUNC(module,sync);

	BIND_FUNC(module,tcgetpgrp);
	BIND_FUNC(module,tcsetpgrp);
	BIND_FUNC(module,ttyname);
#endif


	_loadEnviron(module);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}


