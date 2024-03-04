#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/util.h>

#define KRK_VERSION_MAJOR  1
#define KRK_VERSION_MINOR  5
#define KRK_VERSION_PATCH  0
#define KRK_VERSION_LEVEL  0xa
#define KRK_VERSION_SERIAL 0x1

#define KRK_VERSION_EXTRA_BASE  "a1"

#ifndef KRK_STATIC_ONLY
#define KRK_VERSION_EXTRA KRK_VERSION_EXTRA_BASE
#else
#define KRK_VERSION_EXTRA KRK_VERSION_EXTRA_BASE "-static"
#endif

#define KRK_BUILD_DATE     __DATE__ " at " __TIME__

#if (defined(__GNUC__) || defined(__GNUG__)) && !(defined(__clang__) || defined(__INTEL_COMPILER))
# define KRK_BUILD_COMPILER "GCC " __VERSION__
#elif (defined(__clang__))
# define KRK_BUILD_COMPILER "clang " __clang_version__
#elif (defined(_MSC_VER) && !defined(__clang__))
# define KRK_ARG_STR(str) #str
# define KRK_ARG_LOL(s) KRK_ARG_STR(s)
# define KRK_BUILD_COMPILER "msvc " KRK_ARG_LOL(_MSC_FULL_VER)
#else
# define KRK_BUILD_COMPILER ""
#endif


#ifndef KRK_DISABLE_DEBUG
KRK_Function(set_tracing) {
	int tracing = -1;
	int disassembly = -1;

	if (!krk_parseArgs(
		"|$pp", (const char *[]){"tracing","disassembly"},
		&tracing, &disassembly)) {
		return NONE_VAL();
	}

#define SET_THREAD(arg,flag) do { if (arg != -1) { if (arg) krk_currentThread.flags |= KRK_THREAD_ENABLE_ ## flag; else krk_currentThread.flags &= ~KRK_THREAD_ENABLE_ ## flag; } } while (0)
	SET_THREAD(tracing,TRACING);
	SET_THREAD(disassembly,DISASSEMBLY);
#undef SET_THREAD

	return BOOLEAN_VAL(1);
}
#else
KRK_Function(set_tracing) {
	return krk_runtimeError(vm.exceptions->typeError,"Debugging is not enabled in this build.");
}
#endif

KRK_Function(getsizeof) {
	if (argc < 1 || !IS_OBJECT(argv[0])) return INTEGER_VAL(0);
	size_t mySize = 0;
	switch (AS_OBJECT(argv[0])->type) {
		case KRK_OBJ_STRING: {
			KrkString * self = AS_STRING(argv[0]);
			mySize += sizeof(KrkString) + self->length + 1; /* For the UTF8 */
			if (self->codes && self->chars != self->codes) {
				if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) <= KRK_OBJ_FLAGS_STRING_UCS1) mySize += self->codesLength;
				else if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS2) mySize += 2 * self->codesLength;
				else if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_UCS4) mySize += 4 * self->codesLength;
			}
			break;
		}
		case KRK_OBJ_CODEOBJECT: {
			KrkCodeObject * self = (KrkCodeObject*)AS_OBJECT(argv[0]);
			mySize += sizeof(KrkCodeObject);
			/* Chunk size */
			mySize += sizeof(uint8_t) * self->chunk.capacity;
			mySize += sizeof(KrkLineMap) * self->chunk.linesCapacity;
			mySize += sizeof(KrkValue) * self->chunk.constants.capacity;
			mySize += sizeof(KrkExpressionsMap) * self->expressionsCapacity;
			/* requiredArgNames */
			mySize += sizeof(KrkValue) * self->positionalArgNames.capacity;
			/* keywordArgNames */
			mySize += sizeof(KrkValue) * self->keywordArgNames.capacity;
			/* Locals array */
			mySize += sizeof(KrkLocalEntry) * self->localNameCount;
			/* Overlong jumps */
			mySize += sizeof(KrkOverlongJump) * self->overlongJumpsCapacity;
			break;
		}
		case KRK_OBJ_NATIVE: {
			KrkNative * self = (KrkNative*)AS_OBJECT(argv[0]);
			mySize += sizeof(KrkNative) + strlen(self->name) + 1;
			break;
		}
		case KRK_OBJ_CLOSURE: {
			KrkClosure * self = AS_CLOSURE(argv[0]);
			mySize += sizeof(KrkClosure) + sizeof(KrkUpvalue*) * self->function->upvalueCount;
			break;
		}
		case KRK_OBJ_UPVALUE: {
			/* It should not be possible for an upvalue to be an argument to getsizeof,
			 * but for the sake of completeness, we'll include it here... */
			mySize += sizeof(KrkUpvalue);
			break;
		}
		case KRK_OBJ_CLASS: {
			KrkClass * self = AS_CLASS(argv[0]);
			mySize += sizeof(KrkClass);
			mySize += (sizeof(KrkTableEntry) + sizeof(ssize_t)) * self->methods.capacity;
			mySize += (sizeof(KrkTableEntry) + sizeof(ssize_t)) * self->subclasses.capacity;
			break;
		}
		case KRK_OBJ_INSTANCE: {
			KrkInstance * self = AS_INSTANCE(argv[0]);
			mySize += (sizeof(KrkTableEntry) + sizeof(ssize_t)) * self->fields.capacity;
			KrkClass * type = krk_getType(argv[0]);
			mySize += type->allocSize; /* All instance types have an allocSize set */

			/* TODO __sizeof__ */
			if (krk_isInstanceOf(argv[0], vm.baseClasses->listClass)) {
				mySize += sizeof(KrkValue) * AS_LIST(argv[0])->capacity;
			} else if (krk_isInstanceOf(argv[0], vm.baseClasses->dictClass)) {
				mySize += (sizeof(KrkTableEntry) + sizeof(ssize_t)) * AS_DICT(argv[0])->capacity;
			}
			break;
		}
		case KRK_OBJ_BOUND_METHOD: {
			mySize += sizeof(KrkBoundMethod);
			break;
		}
		case KRK_OBJ_TUPLE: {
			KrkTuple * self = AS_TUPLE(argv[0]);
			mySize += sizeof(KrkTuple) + sizeof(KrkValue) * self->values.capacity;
			break;
		}
		case KRK_OBJ_BYTES: {
			KrkBytes * self = AS_BYTES(argv[0]);
			mySize += sizeof(KrkBytes) + self->length;
			break;
		}
		default: break;
	}
	return INTEGER_VAL(mySize);
}

KRK_Function(set_clean_output) {
	if (!argc || (IS_BOOLEAN(argv[0]) && AS_BOOLEAN(argv[0]))) {
		vm.globalFlags |= KRK_GLOBAL_CLEAN_OUTPUT;
	} else {
		vm.globalFlags &= ~KRK_GLOBAL_CLEAN_OUTPUT;
	}
	return NONE_VAL();
}

KRK_Function(importmodule) {
	FUNCTION_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[0])) return TYPE_ERROR(str,argv[0]);
	if (!krk_doRecursiveModuleLoad(AS_STRING(argv[0]))) return NONE_VAL(); /* ImportError already raised */
	return krk_pop();
}

KRK_Function(modules) {
	FUNCTION_TAKES_NONE();
	KrkValue moduleList = krk_list_of(0,NULL,0);
	krk_push(moduleList);
	for (size_t i = 0; i < vm.modules.capacity; ++i) {
		KrkTableEntry * entry = &vm.modules.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		krk_writeValueArray(AS_LIST(moduleList), entry->key);
	}
	return krk_pop();
}

KRK_Function(unload) {
	FUNCTION_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[0])) return TYPE_ERROR(str,argv[0]);
	if (!krk_tableDelete(&vm.modules, argv[0])) {
		return krk_runtimeError(vm.exceptions->keyError, "Module is not loaded.");
	}
	return NONE_VAL();
}

KRK_Function(inspect_value) {
	FUNCTION_TAKES_EXACTLY(1);
	return OBJECT_VAL(krk_newBytes(sizeof(KrkValue),(uint8_t*)&argv[0]));
}

KRK_Function(members) {
	KrkValue val;
	if (!krk_parseArgs("V", (const char*[]){"obj"}, &val)) return NONE_VAL();

	KrkValue myDict = krk_dict_of(0,NULL,0);
	krk_push(myDict);

	KrkTable * src = NULL;

	if (IS_INSTANCE(val) || IS_CLASS(val)) {
		src = &AS_INSTANCE(val)->fields;
	} else if (IS_CLOSURE(val)) {
		src = &AS_CLOSURE(val)->fields;
	}

	if (src) {
		krk_tableAddAll(src, AS_DICT(myDict));
	}

	return krk_pop();
}

KRK_Function(set_recursion_depth) {
	unsigned int maxdepth;
	int quiet = 0;
	if (!krk_parseArgs("I|p",(const char*[]){"maxdepth","quiet"},&maxdepth,&quiet)) return NONE_VAL();
	if (krk_currentThread.exitOnFrame != 0) {
		if (quiet) return BOOLEAN_VAL(0);
		return krk_runtimeError(vm.exceptions->valueError, "Can not change recursion depth in this context.");
	}
	krk_setMaximumRecursionDepth(maxdepth);
	return BOOLEAN_VAL(1);
}

KRK_Function(get_recursion_depth) {
	return INTEGER_VAL(krk_currentThread.maximumCallDepth);
}

void krk_module_init_kuroko(void) {
	/**
	 * kuroko = module()
	 *
	 * This is equivalent to Python's "sys" module, but we do not use that name
	 * in consideration of future compatibility, where a "sys" module may be
	 * added to emulate Python version numbers, etc.
	 */
	vm.system = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "kuroko", (KrkObj*)vm.system);
	krk_attachNamedObject(&vm.system->fields, "__name__", (KrkObj*)S("kuroko"));
	krk_attachNamedValue(&vm.system->fields, "__file__", NONE_VAL()); /* (built-in) */
	KRK_DOC(vm.system, "@brief System module.");
#define STR_(x) #x
#define STR(x) STR_(x)
	krk_attachNamedObject(&vm.system->fields, "version",
		(KrkObj*)S(STR(KRK_VERSION_MAJOR) "." STR(KRK_VERSION_MINOR) "." STR(KRK_VERSION_PATCH) KRK_VERSION_EXTRA));
	krk_attachNamedObject(&vm.system->fields, "buildenv", (KrkObj*)S(KRK_BUILD_COMPILER));
	krk_attachNamedObject(&vm.system->fields, "builddate", (KrkObj*)S(KRK_BUILD_DATE));
	krk_attachNamedValue(&vm.system->fields, "hexversion",
		INTEGER_VAL((KRK_VERSION_MAJOR << 24) | (KRK_VERSION_MINOR << 16) | (KRK_VERSION_PATCH << 8) | (KRK_VERSION_LEVEL << 4) | (KRK_VERSION_SERIAL)));

	KRK_DOC(BIND_FUNC(vm.system,getsizeof),
		"@brief Calculate the approximate size of an object in bytes.\n"
		"@arguments value\n\n"
		"@param value Value to examine.");
	KRK_DOC(BIND_FUNC(vm.system,set_clean_output),
		"@brief Disables terminal escapes in some output from the VM.\n"
		"@arguments clean=True\n\n"
		"@param clean Whether to remove escapes.");
	KRK_DOC(BIND_FUNC(vm.system,set_tracing),
		"@brief Toggle debugging modes.\n"
		"@arguments tracing=None,disassembly=None\n\n"
		"Enables or disables tracing options for the current thread.\n\n"
		"@param tracing Enables instruction tracing.\n"
		"@param disassembly Prints bytecode disassembly after compilation.");
	KRK_DOC(BIND_FUNC(vm.system,importmodule),
		"@brief Import a module by string name\n"
		"@arguments module\n\n"
		"Imports the dot-separated module @p module as if it were imported by the @c import statement and returns the resulting module object.\n\n"
		"@param module A string with a dot-separated package or module name");
	KRK_DOC(BIND_FUNC(vm.system,modules),
		"Get the list of valid names from the module table");
	KRK_DOC(BIND_FUNC(vm.system,unload),
		"Removes a module from the module table. It is not necessarily garbage collected if other references to it exist.");
	KRK_DOC(BIND_FUNC(vm.system,inspect_value),
		"Obtain the memory representation of a stack value.");
	KRK_DOC(BIND_FUNC(vm.system,members),
		"Obtain a copy of a dict of the direct members of an object.");
	KRK_DOC(BIND_FUNC(vm.system,set_recursion_depth),
		"Change the maximum recursion depth of the current thread if possible.");
	KRK_DOC(BIND_FUNC(vm.system,get_recursion_depth),
		"Examine the maximum recursion depth of the current thread.");
	krk_attachNamedObject(&vm.system->fields, "module", (KrkObj*)vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.system->fields, "path_sep", (KrkObj*)S(KRK_PATH_SEP));
	KrkValue module_paths = krk_list_of(0,NULL,0);
	krk_attachNamedValue(&vm.system->fields, "module_paths", module_paths);
	krk_writeValueArray(AS_LIST(module_paths), OBJECT_VAL(S("./")));
#ifndef KRK_NO_FILESYSTEM
	if (vm.binpath) {
		krk_attachNamedObject(&vm.system->fields, "executable_path", (KrkObj*)krk_copyString(vm.binpath, strlen(vm.binpath)));
		char * dir = strdup(vm.binpath);
#ifndef _WIN32
		char * slash = strrchr(dir,'/');
		if (slash) *slash = '\0';
		if (strstr(dir,"/bin") == (dir + strlen(dir) - 4)) {
			slash = strrchr(dir,'/');
			if (slash) *slash = '\0';
			krk_writeValueArray(AS_LIST(module_paths), krk_stringFromFormat("%s/lib/kuroko/", dir));
		} else {
			krk_writeValueArray(AS_LIST(module_paths), krk_stringFromFormat("%s/modules/", dir));
		}
#else
		char * backslash = strrchr(dir,'\\');
		if (backslash) *backslash = '\0';
		krk_writeValueArray(AS_LIST(module_paths), krk_stringFromFormat("%s\\modules\\", dir));
#endif
		free(dir);
	}
#endif
}
