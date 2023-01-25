#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#define CURRENT_NAME  self

#define IS_type(o) (1)
#define AS_type(o) (o)
#define CURRENT_CTYPE KrkValue
KRK_Method(type,__init__) {
	METHOD_TAKES_EXACTLY(1);
	return OBJECT_VAL(krk_getType(argv[1]));
}
#undef IS_type
#undef AS_type
#undef CURRENT_CTYPE

#define IS_type(o) (IS_CLASS(o))
#define AS_type(o) (AS_CLASS(o))

#define CURRENT_CTYPE KrkClass *

KRK_Method(type,__base__) {
	if (argc > 1) return krk_runtimeError(vm.exceptions->typeError, "__base__ can not be reassigned");
	return self->base ? OBJECT_VAL(self->base) : NONE_VAL();
}

KRK_Method(type,__name__) {
	if (argc > 1) {
		if (!IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
		self->name = AS_STRING(argv[1]);
	}
	return self->name ? OBJECT_VAL(self->name) : NONE_VAL();
}

KRK_Method(type,__file__) {
	if (argc > 1) {
		if (!IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
		self->filename = AS_STRING(argv[1]);
	}
	return self->filename ? OBJECT_VAL(self->filename) : NONE_VAL();
}

KRK_Method(type,__str__) {
	/* Determine if this class has a module */
	KrkValue module = NONE_VAL();
	krk_tableGet(&self->methods, OBJECT_VAL(S("__module__")), &module);

	KrkValue qualname = NONE_VAL();
	krk_tableGet(&self->methods, OBJECT_VAL(S("__qualname__")), &qualname);
	KrkString * name = IS_STRING(qualname) ? AS_STRING(qualname) : self->name;

	int includeModule = !(IS_NONE(module) || (IS_STRING(module) && AS_STRING(module) == S("builtins")));

	return krk_stringFromFormat("<class '%s%s%S'>",
		includeModule ? AS_CSTRING(module) : "",
		includeModule ? "." : "",
		name);
}

KRK_Method(type,__subclasses__) {
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	for (size_t i = 0; i < self->subclasses.capacity; ++i) {
		KrkTableEntry * entry = &self->subclasses.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		krk_writeValueArray(AS_LIST(myList), entry->key);
	}

	return krk_pop();
}

KRK_Method(type,__getitem__) {
	if (self->_classgetitem && argc == 2) {
		krk_push(argv[0]);
		krk_push(argv[1]);
		return krk_callDirect(self->_classgetitem, 2);
	}
	return krk_runtimeError(vm.exceptions->attributeError, "'%s' object is not subscriptable", "type");
}

KRK_Method(type,__call__) {
	if (self == vm.baseClasses->typeClass) {
		if (argc == 2) {
			return OBJECT_VAL(krk_getType(argv[1]));
		}
		return krk_runtimeError(vm.exceptions->attributeError, "TODO type(...)");
	}

	if (!self->_new) {
		return krk_runtimeError(vm.exceptions->typeError, "%S() can not be built", self->name);
	}

	/* Push args */
	int argCount = argc;
	for (int i = 0; i < argc; ++i) {
		krk_push(argv[i]);
	}

	if (hasKw) {
		argCount += 3;
		krk_push(KWARGS_VAL(KWARGS_DICT));
		krk_push(argv[argc]);
		krk_push(KWARGS_VAL(1));
	}

	krk_push(krk_callDirect(self->_new, argCount));

	/* Exception here */
	if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return NONE_VAL();

	if (krk_isInstanceOf(krk_peek(0), self) && likely(self->_init != NULL)) {
		for (int i = 0; i < argc - 1; ++i) {
			krk_push(argv[i+1]);
		}

		if (hasKw) {
			krk_push(KWARGS_VAL(KWARGS_DICT));
			krk_push(argv[argc]);
			krk_push(KWARGS_VAL(1));
		}

		return krk_callDirect(self->_init, argCount);
	}

	return krk_pop();
}

_noexport
void _createAndBind_type(void) {
	KrkClass * type = ADD_BASE_CLASS(vm.baseClasses->typeClass, "type", vm.baseClasses->objectClass);
	//type->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	type->allocSize = sizeof(KrkClass);

	BIND_PROP(type,__base__);
	BIND_PROP(type,__file__);
	BIND_PROP(type,__name__);

	BIND_METHOD(type,__init__);
	BIND_METHOD(type,__str__);
	BIND_METHOD(type,__subclasses__);
	BIND_METHOD(type,__getitem__);
	BIND_METHOD(type,__call__);
	krk_defineNative(&type->methods,"__repr__",FUNC_NAME(type,__str__));

	krk_finalizeClass(type);
	KRK_DOC(type, "Obtain the object representation of the class of an object.");
}
