#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#define CURRENT_NAME  self

#define IS_type(o) (1)
#define AS_type(o) (o)
#define CURRENT_CTYPE KrkValue
KRK_METHOD(type,__init__,{
	METHOD_TAKES_EXACTLY(1);
	return OBJECT_VAL(krk_getType(argv[1]));
})
#undef IS_type
#undef AS_type
#undef CURRENT_CTYPE

#define IS_type(o) (IS_CLASS(o))
#define AS_type(o) (AS_CLASS(o))

#define CURRENT_CTYPE KrkClass *

KRK_METHOD(type,__base__,{
	return self->base ? OBJECT_VAL(self->base) : NONE_VAL();
})

KRK_METHOD(type,__name__,{
	return self->name ? OBJECT_VAL(self->name) : NONE_VAL();
})

KRK_METHOD(type,__file__,{
	return self->filename ? OBJECT_VAL(self->filename) : NONE_VAL();
})

KRK_METHOD(type,__doc__,{
	return self->docstring ? OBJECT_VAL(self->docstring) : NONE_VAL();
})

KRK_METHOD(type,__str__,{
	/* Determine if this class has a module */
	KrkValue module = NONE_VAL();
	krk_tableGet(&self->methods, OBJECT_VAL(S("__module__")), &module);

	KrkValue qualname = NONE_VAL();
	krk_tableGet(&self->methods, OBJECT_VAL(S("__qualname__")), &qualname);
	KrkString * name = IS_STRING(qualname) ? AS_STRING(qualname) : self->name;

	int includeModule = !(IS_NONE(module) || (IS_STRING(module) && AS_STRING(module) == S("__builtins__")));

	size_t allocSize = sizeof("<class ''>") + name->length;
	if (IS_STRING(module)) allocSize += AS_STRING(module)->length + 1;
	char * tmp = malloc(allocSize);
	size_t l = snprintf(tmp, allocSize, "<class '%s%s%s'>",
		includeModule ? AS_CSTRING(module) : "",
		includeModule ? "." : "",
		name->chars);
	KrkString * out = krk_copyString(tmp,l);
	free(tmp);
	return OBJECT_VAL(out);
})

KRK_METHOD(type,__subclasses__,{
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	for (size_t i = 0; i < self->subclasses.capacity; ++i) {
		KrkTableEntry * entry = &self->subclasses.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		krk_writeValueArray(AS_LIST(myList), entry->key);
	}

	return krk_pop();
})

_noexport
void _createAndBind_type(void) {
	KrkClass * type = ADD_BASE_CLASS(vm.baseClasses->typeClass, "type", vm.baseClasses->objectClass);
	type->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;

	BIND_METHOD(type,__base__)->obj.flags = KRK_OBJ_FLAGS_FUNCTION_IS_DYNAMIC_PROPERTY;
	BIND_METHOD(type,__file__)->obj.flags = KRK_OBJ_FLAGS_FUNCTION_IS_DYNAMIC_PROPERTY;
	BIND_METHOD(type,__doc__) ->obj.flags = KRK_OBJ_FLAGS_FUNCTION_IS_DYNAMIC_PROPERTY;
	BIND_METHOD(type,__name__)->obj.flags = KRK_OBJ_FLAGS_FUNCTION_IS_DYNAMIC_PROPERTY;

	BIND_METHOD(type,__init__);
	BIND_METHOD(type,__str__);
	BIND_METHOD(type,__subclasses__);
	krk_defineNative(&type->methods,"__repr__",FUNC_NAME(type,__str__));

	krk_finalizeClass(type);
	KRK_DOC(type, "Obtain the object representation of the class of an object.");
}
