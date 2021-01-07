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

KrkValue krk_module_onload_os(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	krk_defineNative(&module->fields, "uname", krk_uname);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}


