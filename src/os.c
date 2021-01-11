/**
 * Currently just uname().
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "../vm.h"
#include "../value.h"
#include "../object.h"

/* Did you know this is actually specified to not exist in a header? */
extern char ** environ;

#define S(c) (krk_copyString(c,sizeof(c)-1))

/**
 * system.uname()
 */
static KrkValue krk_uname(int argc, KrkValue argv[]) {
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
}

KrkValue krk_os_setenviron(int argc, KrkValue * argv[]) {
	krk_runtimeError(vm.exceptions.typeError, "(unimplemented)");
	return NONE_VAL();
}

static void _loadEnviron(KrkInstance * module) {
	/* Create a new class to subclass `dict` */
	KrkString * className = S("_Environ");
	krk_push(OBJECT_VAL(className));
	KrkClass * environClass = krk_newClass(className);
	krk_attachNamedObject(&module->fields, "_Environ", (KrkObj*)environClass);
	krk_pop(); /* className */

	/* Inherit from base dict */
	KrkValue dictClass;
	krk_tableGet(&vm.builtins->fields,OBJECT_VAL(S("dict")), &dictClass);
	krk_tableAddAll(&AS_CLASS(dictClass)->methods, &environClass->methods);
	krk_tableAddAll(&AS_CLASS(dictClass)->fields, &environClass->fields);
	environClass->base = AS_CLASS(dictClass);

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

	KrkClass * dictContents = environObj->_internal;

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

		krk_tableSet(&dictContents->methods, key, val);
		krk_pop(); /* val */
		krk_pop(); /* key */
	}

}

KrkValue krk_module_onload_os(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	krk_defineNative(&module->fields, "uname", krk_uname);

	_loadEnviron(module);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}


