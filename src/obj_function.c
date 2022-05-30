#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/debug.h>

/* Check for and return the name of a native function as a string object */
static KrkValue nativeFunctionName(KrkValue func) {
	const char * string = ((KrkNative*)AS_OBJECT(func))->name;
	if (!string) return OBJECT_VAL(S("<unnamed>"));
	size_t len = strlen(string);
	return OBJECT_VAL(krk_copyString(string,len));
}

#define IS_method(o)     IS_BOUND_METHOD(o)
#define IS_function(o)   (IS_CLOSURE(o)|IS_NATIVE(o))

#define AS_method(o)     AS_BOUND_METHOD(o)
#define AS_function(o)   (o)

#define CURRENT_NAME  self
#define CURRENT_CTYPE KrkValue

KRK_METHOD(function,__doc__,{
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_NATIVE(self) && AS_NATIVE(self)->doc) {
		return OBJECT_VAL(krk_copyString(AS_NATIVE(self)->doc, strlen(AS_NATIVE(self)->doc)));
	} else if (IS_CLOSURE(self) && AS_CLOSURE(self)->function->docstring) {
		return OBJECT_VAL(AS_CLOSURE(self)->function->docstring);
	}
})

KRK_METHOD(function,__name__,{
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_NATIVE(self)) {
		return nativeFunctionName(self);
	} else if (IS_CLOSURE(self) && AS_CLOSURE(self)->function->name) {
		return OBJECT_VAL(AS_CLOSURE(self)->function->name);
	}

	return OBJECT_VAL(S(""));
})

KRK_METHOD(function,__qualname__,{
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_CLOSURE(self) && AS_CLOSURE(self)->function->qualname) {
		return OBJECT_VAL(AS_CLOSURE(self)->function->qualname);
	}
})

KRK_METHOD(function,_ip_to_line,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,ip);

	if (!IS_CLOSURE(self)) return NONE_VAL();

	size_t line = krk_lineNumber(&AS_CLOSURE(self)->function->chunk, ip);

	return INTEGER_VAL(line);
})

KRK_METHOD(function,__str__,{
	METHOD_TAKES_NONE();

	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, "<function ", 10);

	/* Do we have a qualified name? */
	KrkValue name = FUNC_NAME(function,__qualname__)(1,&self,0);
	if (IS_NONE(name)) {
		name = FUNC_NAME(function,__name__)(1,&self,0);
	}

	if (!IS_STRING(name)) name = OBJECT_VAL(S("<unnamed>"));

	pushStringBuilderStr(&sb, AS_CSTRING(name), AS_STRING(name)->length);

	pushStringBuilderStr(&sb," at ", 4);

	char address[100];
	size_t len = snprintf(address, 100, "%p", (void*)AS_OBJECT(self));
	pushStringBuilderStr(&sb, address, len);

	pushStringBuilder(&sb,'>');

	return finishStringBuilder(&sb);
})

KRK_METHOD(function,__file__,{
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_NATIVE(self)) return OBJECT_VAL(S("<builtin>"));

	return AS_CLOSURE(self)->function->chunk.filename ?
		OBJECT_VAL(AS_CLOSURE(self)->function->chunk.filename) :
			OBJECT_VAL(S(""));
})

KRK_METHOD(function,__args__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) return OBJECT_VAL(krk_newTuple(0));
	KrkCodeObject * _self = AS_CLOSURE(self)->function;
	KrkTuple * tuple = krk_newTuple(_self->requiredArgs + _self->keywordArgs + !!(_self->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_ARGS) + !!(_self->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_KWS));
	krk_push(OBJECT_VAL(tuple));

	for (short i = 0; i < _self->requiredArgs; ++i) {
		tuple->values.values[tuple->values.count++] = _self->requiredArgNames.values[i];
	}

	for (short i = 0; i < _self->keywordArgs; ++i) {
		struct StringBuilder sb = {0};
		pushStringBuilderStr(&sb, AS_CSTRING(_self->keywordArgNames.values[i]), AS_STRING(_self->keywordArgNames.values[i])->length);
		pushStringBuilder(&sb,'=');
		tuple->values.values[tuple->values.count++] = finishStringBuilder(&sb);
	}

	if (_self->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_ARGS) {
		struct StringBuilder sb = {0};
		pushStringBuilder(&sb, '*');
		pushStringBuilderStr(&sb, AS_CSTRING(_self->requiredArgNames.values[_self->requiredArgs]), AS_STRING(_self->requiredArgNames.values[_self->requiredArgs])->length);
		tuple->values.values[tuple->values.count++] = finishStringBuilder(&sb);
	}

	if (_self->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_KWS) {
		struct StringBuilder sb = {0};
		pushStringBuilder(&sb, '*');
		pushStringBuilder(&sb, '*');
		pushStringBuilderStr(&sb, AS_CSTRING(_self->keywordArgNames.values[_self->keywordArgs]), AS_STRING(_self->keywordArgNames.values[_self->keywordArgs])->length);
		tuple->values.values[tuple->values.count++] = finishStringBuilder(&sb);
	}

	krk_pop();
	return OBJECT_VAL(tuple);
})

KRK_METHOD(function,__annotations__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) return NONE_VAL();
	return AS_CLOSURE(self)->annotations;
})

KRK_METHOD(function,__code__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) return NONE_VAL();
	return OBJECT_VAL(AS_CLOSURE(self)->function);
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkCodeObject*

KRK_METHOD(codeobject,__name__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->name ? OBJECT_VAL(self->name) : OBJECT_VAL(S(""));
})

KRK_METHOD(codeobject,__str__,{
	METHOD_TAKES_NONE();
	KrkValue s = FUNC_NAME(codeobject,__name__)(1,argv,0);
	if (!IS_STRING(s)) return NONE_VAL();
	krk_push(s);

	size_t len = AS_STRING(s)->length + sizeof("<codeobject >");
	char * tmp = malloc(len);
	snprintf(tmp, len, "<codeobject %s>", AS_CSTRING(s));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);

	krk_pop();
	return s;
})

KRK_METHOD(codeobject,_ip_to_line,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,ip);
	size_t line = krk_lineNumber(&self->chunk, ip);
	return INTEGER_VAL(line);
})

KRK_METHOD(codeobject,__constants__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	krk_push(OBJECT_VAL(krk_newTuple(self->chunk.constants.count)));
	memcpy(AS_TUPLE(krk_peek(0))->values.values,
		self->chunk.constants.values,
		sizeof(KrkValue) * self->chunk.constants.count);
	AS_TUPLE(krk_peek(0))->values.count = self->chunk.constants.count;
	return krk_pop();
})

/* Python-compatibility */
KRK_METHOD(codeobject,co_flags,{
	ATTRIBUTE_NOT_ASSIGNABLE();

	int out = 0;

	/* For compatibility with Python, mostly because these are specified
	 * in at least one doc page with their raw values, we convert
	 * our flags to the useful CPython flag values... */
	if (self->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_ARGS) out |= 0x04;
	if (self->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_KWS)  out |= 0x08;
	if (self->flags & KRK_CODEOBJECT_FLAGS_IS_GENERATOR)  out |= 0x20;
	if (self->flags & KRK_CODEOBJECT_FLAGS_IS_COROUTINE)  out |= 0x80;

	return INTEGER_VAL(out);
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkBoundMethod*

KRK_METHOD(method,__name__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__name__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,__qualname__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__qualname__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,_ip_to_line,{
	METHOD_TAKES_EXACTLY(1);
	return FUNC_NAME(function,_ip_to_line)(2,(KrkValue[]){OBJECT_VAL(self->method),argv[1]},0);
})

KRK_METHOD(method,__str__,{
	METHOD_TAKES_NONE();
	KrkValue s = FUNC_NAME(method,__qualname__)(1,argv,0);
	if (!IS_STRING(s)) s = FUNC_NAME(method,__name__)(1,argv,0);
	if (!IS_STRING(s)) return NONE_VAL();
	krk_push(s);

	KrkClass * type = krk_getType(self->receiver);
	krk_push(self->receiver);
	KrkValue reprVal = krk_callDirect(type->_reprer, 1);

	size_t len = AS_STRING(s)->length + AS_STRING(reprVal)->length + sizeof("<bound method of >") + 1;
	char * tmp = malloc(len);
	snprintf(tmp, len, "<bound method %s of %s>", AS_CSTRING(s), AS_CSTRING(reprVal));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);
	krk_pop();
	return s;
})

KRK_METHOD(method,__file__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__file__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,__args__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__args__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,__doc__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__doc__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,__annotations__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__annotations__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,__code__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return FUNC_NAME(function,__code__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0);
})

KRK_METHOD(method,__func__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return OBJECT_VAL(self->method);
})

KRK_METHOD(method,__self__,{
	ATTRIBUTE_NOT_ASSIGNABLE();
	return OBJECT_VAL(self->receiver);
})

KRK_FUNC(staticmethod,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,CLOSURE,KrkClosure*,method);
	/* Make a copy */
	krk_push(OBJECT_VAL(krk_newClosure(method->function)));
	/* Copy upvalues */
	for (size_t i = 0; i < method->upvalueCount; ++i) {
		AS_CLOSURE(krk_peek(0))->upvalues[i] = method->upvalues[i];
	}
	AS_CLOSURE(krk_peek(0))->annotations = method->annotations;
	AS_CLOSURE(krk_peek(0))->flags |= KRK_FUNCTION_FLAGS_IS_STATIC_METHOD;
	return krk_pop();
})

KRK_FUNC(classmethod,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,CLOSURE,KrkClosure*,method);
	/* Make a copy */
	krk_push(OBJECT_VAL(krk_newClosure(method->function)));
	/* Copy upvalues */
	for (size_t i = 0; i < method->upvalueCount; ++i) {
		AS_CLOSURE(krk_peek(0))->upvalues[i] = method->upvalues[i];
	}
	AS_CLOSURE(krk_peek(0))->annotations = method->annotations;
	AS_CLOSURE(krk_peek(0))->flags |= KRK_FUNCTION_FLAGS_IS_CLASS_METHOD;
	return krk_pop();
})

_noexport
void _createAndBind_functionClass(void) {
	KrkClass * codeobject = ADD_BASE_CLASS(vm.baseClasses->codeobjectClass, "codeobject", vm.baseClasses->objectClass);
	codeobject->allocSize = 0; /* cannot subclass */
	BIND_METHOD(codeobject,__str__);
	BIND_METHOD(codeobject,_ip_to_line);
	BIND_PROP(codeobject,__constants__);
	BIND_PROP(codeobject,__name__);
	BIND_PROP(codeobject,co_flags);
	krk_defineNative(&codeobject->methods, "__repr__", FUNC_NAME(codeobject,__str__));
	krk_finalizeClass(codeobject);

	KrkClass * function = ADD_BASE_CLASS(vm.baseClasses->functionClass, "function", vm.baseClasses->objectClass);
	function->allocSize = 0; /* cannot subclass */
	BIND_METHOD(function,__str__);
	BIND_METHOD(function,_ip_to_line);
	BIND_PROP(function,__doc__);
	BIND_PROP(function,__name__);
	BIND_PROP(function,__qualname__);
	BIND_PROP(function,__file__);
	BIND_PROP(function,__args__);
	BIND_PROP(function,__annotations__);
	BIND_PROP(function,__code__);
	krk_defineNative(&function->methods, "__repr__", FUNC_NAME(function,__str__));
	krk_defineNative(&function->methods, "__class_getitem__", KrkGenericAlias)->flags |= KRK_NATIVE_FLAGS_IS_CLASS_METHOD;
	krk_finalizeClass(function);

	KrkClass * method = ADD_BASE_CLASS(vm.baseClasses->methodClass, "method", vm.baseClasses->objectClass);
	method->allocSize = 0; /* cannot subclass */
	BIND_METHOD(method,__str__);
	BIND_METHOD(method,_ip_to_line);
	BIND_PROP(method,__doc__);
	BIND_PROP(method,__name__);
	BIND_PROP(method,__qualname__);
	BIND_PROP(method,__file__);
	BIND_PROP(method,__args__);
	BIND_PROP(method,__annotations__);
	BIND_PROP(method,__self__);
	BIND_PROP(method,__func__);
	BIND_PROP(method,__code__);
	krk_defineNative(&method->methods, "__repr__", FUNC_NAME(method,__str__));
	krk_finalizeClass(method);

	BUILTIN_FUNCTION("staticmethod", FUNC_NAME(krk,staticmethod), "A static method does not take an implicit self or cls argument.");
	BUILTIN_FUNCTION("classmethod", FUNC_NAME(krk,classmethod), "A class method takes an implicit cls argument, instead of self.");
}
