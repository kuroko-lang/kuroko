/**
 * Currently just uname().
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/utsname.h>
#else
#include <windows.h>
#endif

#include "vm.h"
#include "value.h"
#include "object.h"

/* Did you know this is actually specified to not exist in a header? */
extern char ** environ;

#define S(c) (krk_copyString(c,sizeof(c)-1))

/**
 * system.uname()
 */
static KrkValue _os_uname(int argc, KrkValue argv[]) {
#ifndef _WIN32
	struct utsname buf;
	if (uname(&buf) < 0) return NONE_VAL();

	KRK_PAUSE_GC();

	KrkValue result = krk_dict_of(5 * 2, (KrkValue[]) {
		OBJECT_VAL(S("sysname")), OBJECT_VAL(krk_copyString(buf.sysname,strlen(buf.sysname))),
		OBJECT_VAL(S("nodename")), OBJECT_VAL(krk_copyString(buf.nodename,strlen(buf.nodename))),
		OBJECT_VAL(S("release")), OBJECT_VAL(krk_copyString(buf.release,strlen(buf.release))),
		OBJECT_VAL(S("version")), OBJECT_VAL(krk_copyString(buf.version,strlen(buf.version))),
		OBJECT_VAL(S("machine")), OBJECT_VAL(krk_copyString(buf.machine,strlen(buf.machine)))
	});

	KRK_RESUME_GC();
	return result;
#else
	KRK_PAUSE_GC();

	TCHAR buffer[256] = TEXT("");
	DWORD dwSize = sizeof(buffer);
	GetComputerName(buffer, &dwSize);

	OSVERSIONINFOA versionInfo = {0};
	versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionExA(&versionInfo);
	KrkValue release;
	if (versionInfo.dwMajorVersion == 10) {
		release = OBJECT_VAL(S("10"));
	} else if (versionInfo.dwMajorVersion == 6) {
		if (versionInfo.dwMinorVersion == 3) {
			release = OBJECT_VAL(S("8.1"));
		} else if (versionInfo.dwMinorVersion == 2) {
			release = OBJECT_VAL(S("8.0"));
		} else if (versionInfo.dwMinorVersion == 1) {
			release = OBJECT_VAL(S("7"));
		} else if (versionInfo.dwMinorVersion == 0) {
			release = OBJECT_VAL(S("Vista"));
		}
	} else {
		release = OBJECT_VAL(S("XP or earlier"));
	}

	char tmp[256];
	sprintf(tmp, "%ld", versionInfo.dwBuildNumber);

	KrkValue version = OBJECT_VAL(krk_copyString(tmp,strlen(tmp)));
	KrkValue machine;
	if (sizeof(void *) == 8) {
		machine = OBJECT_VAL(S("x64"));
	} else {
		machine = OBJECT_VAL(S("x86"));
	}

	KrkValue result = krk_dict_of(5 * 2, (KrkValue[]) {
		OBJECT_VAL(S("sysname")), OBJECT_VAL(S("Windows")),
		OBJECT_VAL(S("nodename")), OBJECT_VAL(krk_copyString(buffer,dwSize)),
		OBJECT_VAL(S("release")), release,
		OBJECT_VAL(S("version")), version,
		OBJECT_VAL(S("machine")), machine
	});

	KRK_RESUME_GC();
	return result;
#endif
}

KrkClass * environClass;

KrkValue krk_os_setenviron(int argc, KrkValue argv[]) {
	if (argc < 3 || !krk_isInstanceOf(argv[0], environClass) ||
		!IS_STRING(argv[1]) || !IS_STRING(argv[2])) {
		return krk_runtimeError(vm.exceptions.argumentError, "Invalid arguments to environ.__set__");
	}
	/* Set environment variable */
#ifdef setenv
	if (setenv(AS_CSTRING(argv[1]), AS_CSTRING(argv[2]), 1) == 0) {
#else
	char * tmp = malloc(AS_STRING(argv[1])->length + AS_STRING(argv[2])->length + 2);
	sprintf(tmp, "%s=%s", AS_CSTRING(argv[1]), AS_CSTRING(argv[2]));
	int r = putenv(tmp);
	free(tmp);
	if (r == 0) {
#endif
		/* Make super call */
		krk_push(argv[0]);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callSimple(OBJECT_VAL(vm.baseClasses.dictClass->_setter), 3, 0);
	} else {
		/* OSError? */
		return krk_runtimeError(vm.exceptions.baseException, strerror(errno));
	}
}

static void _loadEnviron(KrkInstance * module) {
	/* Create a new class to subclass `dict` */
	KrkString * className = S("_Environ");
	krk_push(OBJECT_VAL(className));
	environClass = krk_newClass(className, vm.baseClasses.dictClass);
	krk_attachNamedObject(&module->fields, "_Environ", (KrkObj*)environClass);
	krk_pop(); /* className */

	/* Add our set method that should also call dict's set method */
	krk_defineNative(&environClass->methods, ".__set__", krk_os_setenviron);
	krk_finalizeClass(environClass);

	/* Start with an empty dictionary */
	KrkInstance * environObj = AS_INSTANCE(krk_dict_of(0,NULL));
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

static KrkValue _os_system(int argc, KrkValue argv[]) {
	if (argc != 1 || !IS_STRING(argv[0])) return krk_runtimeError(vm.exceptions.typeError, "system() expects one string argument");

	return INTEGER_VAL(system(AS_CSTRING(argv[0])));
}

KrkValue krk_module_onload_os(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	krk_defineNative(&module->fields, "uname", _os_uname);
	krk_defineNative(&module->fields, "system", _os_system);

	_loadEnviron(module);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}


