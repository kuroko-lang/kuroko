#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#define CURRENT_NAME  self

#define IS_type(o) (IS_CLASS(o))
#define AS_type(o) (AS_CLASS(o))

#define CURRENT_CTYPE KrkClass *

static void _callSetName(KrkClass * _class) {
	KrkValue setnames = krk_list_of(0,NULL,0);
	krk_push(setnames);
	extern FUNC_SIG(list,append);

	/* The semantics of this require that we first collect all of the relevant items... */
	for (size_t i = 0; i < _class->methods.capacity; ++i) {
		KrkTableEntry * entry = &_class->methods.entries[i];
		if (!IS_KWARGS(entry->key)) {
			KrkClass * type = krk_getType(entry->value);
			if (type->_set_name) {
				FUNC_NAME(list,append)(2,(KrkValue[]){setnames,entry->key},0);
				FUNC_NAME(list,append)(2,(KrkValue[]){setnames,entry->value},0);
			}
		}
	}

	/* Then call __set_name__ on them */
	for (size_t i = 0; i < AS_LIST(setnames)->count; i += 2) {
		KrkValue name = AS_LIST(setnames)->values[i];
		KrkValue value = AS_LIST(setnames)->values[i+1];
		KrkClass * type = krk_getType(value);
		if (type->_set_name) {
			krk_push(value);
			krk_push(OBJECT_VAL(_class));
			krk_push(name);
			krk_callDirect(type->_set_name, 3);

			/* If any of these raises an exception, bail; CPython raises
			 * an outer exception, setting the cause, but I'm being lazy
			 * at the moment... */
			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
				break;
			}
		}
	}

	/* List used to store name+value pairs */
	krk_pop();
}

KRK_StaticMethod(type,__new__) {
	KrkClass * metaclass;
	KrkString * name;
	KrkClass * base;
	KrkDict * nspace;

	if (!krk_parseArgs("O!O!O!O!~:type",
		(const char*[]){"cls","name","base","namespace"},
		vm.baseClasses->typeClass, &metaclass,
		vm.baseClasses->strClass, &name,
		vm.baseClasses->typeClass, &base,
		vm.baseClasses->dictClass, &nspace)) {
		return NONE_VAL();
	}

	if (base->obj.flags & KRK_OBJ_FLAGS_NO_INHERIT) {
		return krk_runtimeError(vm.exceptions->typeError, "'%S' can not be subclassed", base->name);
	}

	/* Now make a class */
	KrkClass * _class = krk_newClass(name, base);
	krk_push(OBJECT_VAL(_class));
	_class->_class = metaclass;

	/* Now copy the values over */
	krk_tableAddAll(&nspace->entries, &_class->methods);

	KrkValue tmp;

	if (krk_tableGet_fast(&_class->methods, S("__class_getitem__"), &tmp) && IS_CLOSURE(tmp)) {
		AS_CLOSURE(tmp)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	}

	if (krk_tableGet_fast(&_class->methods, S("__init_subclass__"), &tmp) && IS_CLOSURE(tmp)) {
		AS_CLOSURE(tmp)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	}

	if (krk_tableGet_fast(&_class->methods, S("__new__"), &tmp) && IS_CLOSURE(tmp)) {
		AS_CLOSURE(tmp)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD;
	}

	krk_finalizeClass(_class);
	_callSetName(_class);

	/* Call super().__init_subclass__ */
	krk_push(NONE_VAL());
	if (!krk_bindMethodSuper(base,S("__init_subclass__"),_class)) {
		krk_pop(); /* none */
	} else {
		if (hasKw) {
			krk_push(KWARGS_VAL(KWARGS_DICT));
			krk_push(argv[argc]);
			krk_push(KWARGS_VAL(1));
			krk_callStack(3);
		} else {
			krk_callStack(0);
		}
	}

	return krk_pop();
}

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

KRK_Method(type,__repr__) {
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
		krk_push(krk_peek(0));
		for (int i = 0; i < argc - 1; ++i) {
			krk_push(argv[i+1]);
		}

		if (hasKw) {
			krk_push(KWARGS_VAL(KWARGS_DICT));
			krk_push(argv[argc]);
			krk_push(KWARGS_VAL(1));
		}

		KrkValue result = krk_callDirect(self->_init, argCount);
		if (!IS_NONE(result)) {
			fprintf(stderr, "Warning: Non-None result returned from %s.__init__\n",
				self->name->chars);
		}
	}

	return krk_pop();
}

_noexport
void _createAndBind_type(void) {
	KrkClass * type = ADD_BASE_CLASS(vm.baseClasses->typeClass, "type", vm.baseClasses->objectClass);
	type->allocSize = sizeof(KrkClass);

	BIND_PROP(type,__base__);
	BIND_PROP(type,__file__);
	BIND_PROP(type,__name__);

	BIND_METHOD(type,__repr__);
	BIND_METHOD(type,__subclasses__);
	BIND_METHOD(type,__getitem__);
	BIND_METHOD(type,__call__);
	BIND_STATICMETHOD(type,__new__);

	krk_finalizeClass(type);
	KRK_DOC(type, "Obtain the object representation of the class of an object.");
}
