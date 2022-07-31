#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/debug.h>

/* Check for and return the name of a native function as a string object */
static KrkValue nativeFunctionName(KrkThreadState * _thread, KrkValue func) {
	const char * string = ((KrkNative*)AS_OBJECT(func))->name;
	if (!string) return OBJECT_VAL(S("<unnamed>"));
	size_t len = strlen(string);
	return OBJECT_VAL(krk_copyString(string,len));
}

static KrkTuple * functionArgs(KrkThreadState * _thread, KrkCodeObject * _self) {
	KrkTuple * tuple = krk_newTuple(_self->requiredArgs + _self->keywordArgs + !!(_self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) + !!(_self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS));
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

	if (_self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
		struct StringBuilder sb = {0};
		pushStringBuilder(&sb, '*');
		pushStringBuilderStr(&sb, AS_CSTRING(_self->requiredArgNames.values[_self->requiredArgs]), AS_STRING(_self->requiredArgNames.values[_self->requiredArgs])->length);
		tuple->values.values[tuple->values.count++] = finishStringBuilder(&sb);
	}

	if (_self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
		struct StringBuilder sb = {0};
		pushStringBuilder(&sb, '*');
		pushStringBuilder(&sb, '*');
		pushStringBuilderStr(&sb, AS_CSTRING(_self->keywordArgNames.values[_self->keywordArgs]), AS_STRING(_self->keywordArgNames.values[_self->keywordArgs])->length);
		tuple->values.values[tuple->values.count++] = finishStringBuilder(&sb);
	}

	krk_pop();
	return tuple;
}

#define IS_method(o)     IS_BOUND_METHOD(o)
#define IS_function(o)   (IS_CLOSURE(o)|IS_NATIVE(o))

#define AS_method(o)     AS_BOUND_METHOD(o)
#define AS_function(o)   (o)

#define CURRENT_NAME  self
#define CURRENT_CTYPE KrkValue

FUNC_SIG(function,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_EXACTLY(3);
	CHECK_ARG(1,codeobject,KrkCodeObject*,code);

	if (!IS_INSTANCE(argv[3]))
		return TYPE_ERROR(dict or instance object,argv[3]);

	if (IS_CLOSURE(argv[2]) && AS_CLOSURE(argv[2])->upvalueCount == code->upvalueCount) {
		/* Option 1: A function with the same upvalue count. Copy the upvalues exactly.
		 *           As an example, this can be a lambda with a bunch of unused upvalue
		 *           references - like "lambda: a, b, c". These variables will be captured
		 *           using the relevant scope - and we don't have to care about whether
		 *           they were properly marked, because the compiler took care of it
		 *           when the lambda was compiled.
		 */
		krk_push(OBJECT_VAL(krk_newClosure(code, argv[3])));
		memcpy(AS_CLOSURE(krk_peek(0))->upvalues, AS_CLOSURE(argv[2])->upvalues,
			sizeof(KrkUpvalue*) * code->upvalueCount);
		return krk_pop();
	} else if (IS_TUPLE(argv[2]) && AS_TUPLE(argv[2])->values.count == code->upvalueCount) {
		/* Option 2: A tuple of values. New upvalue containers are built for each value,
		 *           but they are immediately closed with the value in the tuple. They
		 *           exist independently for this closure instance, and are not shared with
		 *           any other closures.
		 */
		krk_push(OBJECT_VAL(krk_newClosure(code, argv[3])));
		for (size_t i = 0; i < code->upvalueCount; ++i) {
			AS_CLOSURE(krk_peek(0))->upvalues[i] = krk_newUpvalue(-1);
			AS_CLOSURE(krk_peek(0))->upvalues[i]->closed = AS_TUPLE(argv[2])->values.values[i];
		}
		return krk_pop();
	}

	return TYPE_ERROR(managed function with equal upvalue count or tuple,argv[2]);
}

KRK_Method(function,__doc__) {
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_NATIVE(self) && AS_NATIVE(self)->doc) {
		return OBJECT_VAL(krk_copyString(AS_NATIVE(self)->doc, strlen(AS_NATIVE(self)->doc)));
	} else if (IS_CLOSURE(self) && AS_CLOSURE(self)->function->docstring) {
		return OBJECT_VAL(AS_CLOSURE(self)->function->docstring);
	}

	return NONE_VAL();
}

KRK_Method(function,__name__) {
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_NATIVE(self)) {
		return nativeFunctionName(_thread, self);
	} else if (IS_CLOSURE(self) && AS_CLOSURE(self)->function->name) {
		return OBJECT_VAL(AS_CLOSURE(self)->function->name);
	}

	return OBJECT_VAL(S(""));
}

KRK_Method(function,__qualname__) {
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_CLOSURE(self) && AS_CLOSURE(self)->function->qualname) {
		return OBJECT_VAL(AS_CLOSURE(self)->function->qualname);
	}

	return NONE_VAL();
}

KRK_Method(function,__globals__) {
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_CLOSURE(self)) {
		return AS_CLOSURE(self)->globalsOwner;
	}

	return NONE_VAL();
}

KRK_Method(function,_ip_to_line) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,ip);

	if (!IS_CLOSURE(self)) return NONE_VAL();

	size_t line = krk_lineNumber(&AS_CLOSURE(self)->function->chunk, ip);

	return INTEGER_VAL(line);
}

KRK_Method(function,__str__) {
	METHOD_TAKES_NONE();

	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb, "<function ", 10);

	/* Do we have a qualified name? */
	KrkValue name = FUNC_NAME(function,__qualname__)(_thread, 1,&self,0);
	if (IS_NONE(name)) {
		name = FUNC_NAME(function,__name__)(_thread, 1,&self,0);
	}

	if (!IS_STRING(name)) name = OBJECT_VAL(S("<unnamed>"));

	pushStringBuilderStr(&sb, AS_CSTRING(name), AS_STRING(name)->length);

	pushStringBuilderStr(&sb," at ", 4);

	char address[100];
	size_t len = snprintf(address, 100, "%p", (void*)AS_OBJECT(self));
	pushStringBuilderStr(&sb, address, len);

	pushStringBuilder(&sb,'>');

	return finishStringBuilder(&sb);
}

KRK_Method(function,__file__) {
	ATTRIBUTE_NOT_ASSIGNABLE();

	if (IS_NATIVE(self)) return OBJECT_VAL(S("<builtin>"));

	return AS_CLOSURE(self)->function->chunk.filename ?
		OBJECT_VAL(AS_CLOSURE(self)->function->chunk.filename) :
			OBJECT_VAL(S(""));
}

KRK_Method(function,__args__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) return OBJECT_VAL(krk_newTuple(0));
	KrkTuple * tuple = functionArgs(_thread, AS_CLOSURE(self)->function);
	return OBJECT_VAL(tuple);
}

KRK_Method(function,__annotations__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) return NONE_VAL();
	return AS_CLOSURE(self)->annotations;
}

KRK_Method(function,__code__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) return NONE_VAL();
	return OBJECT_VAL(AS_CLOSURE(self)->function);
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkCodeObject*

FUNC_SIG(codeobject,__init__) {
	return krk_runtimeError(vm.exceptions->typeError, "codeobject object is not instantiable");
}

KRK_Method(codeobject,__name__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->name ? OBJECT_VAL(self->name) : OBJECT_VAL(S(""));
}

KRK_Method(codeobject,__str__) {
	METHOD_TAKES_NONE();
	KrkValue s = FUNC_NAME(codeobject,__name__)(_thread, 1,argv,0);
	if (!IS_STRING(s)) return NONE_VAL();
	krk_push(s);

	size_t len = AS_STRING(s)->length + sizeof("<codeobject >");
	char * tmp = malloc(len);
	snprintf(tmp, len, "<codeobject %s>", AS_CSTRING(s));
	s = OBJECT_VAL(krk_copyString(tmp,len-1));
	free(tmp);

	krk_pop();
	return s;
}

KRK_Method(codeobject,_ip_to_line) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,ip);
	size_t line = krk_lineNumber(&self->chunk, ip);
	return INTEGER_VAL(line);
}

KRK_Method(codeobject,__constants__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	krk_push(OBJECT_VAL(krk_newTuple(self->chunk.constants.count)));
	memcpy(AS_TUPLE(krk_peek(0))->values.values,
		self->chunk.constants.values,
		sizeof(KrkValue) * self->chunk.constants.count);
	AS_TUPLE(krk_peek(0))->values.count = self->chunk.constants.count;
	return krk_pop();
}

KRK_Method(codeobject,co_code) {
	return OBJECT_VAL(krk_newBytes(self->chunk.count, self->chunk.code));
}

KRK_Method(codeobject,co_argcount) {
	return INTEGER_VAL(self->potentialPositionals);
}

KRK_Method(codeobject,__locals__) {
	krk_push(OBJECT_VAL(krk_newTuple(self->localNameCount)));
	for (size_t i = 0; i < self->localNameCount; ++i) {
		krk_push(OBJECT_VAL(krk_newTuple(4)));
		AS_TUPLE(krk_peek(0))->values.values[AS_TUPLE(krk_peek(0))->values.count++] = INTEGER_VAL(self->localNames[i].id);
		AS_TUPLE(krk_peek(0))->values.values[AS_TUPLE(krk_peek(0))->values.count++] = INTEGER_VAL(self->localNames[i].birthday);
		AS_TUPLE(krk_peek(0))->values.values[AS_TUPLE(krk_peek(0))->values.count++] = INTEGER_VAL(self->localNames[i].deathday);
		AS_TUPLE(krk_peek(0))->values.values[AS_TUPLE(krk_peek(0))->values.count++] = OBJECT_VAL(self->localNames[i].name);
		AS_TUPLE(krk_peek(1))->values.values[AS_TUPLE(krk_peek(1))->values.count++] = krk_peek(0);
		krk_pop();
	}
	return krk_pop();
}

/* Python-compatibility */
KRK_Method(codeobject,co_flags) {
	ATTRIBUTE_NOT_ASSIGNABLE();

	int out = 0;

	/* For compatibility with Python, mostly because these are specified
	 * in at least one doc page with their raw values, we convert
	 * our flags to the useful CPython flag values... */
	if (self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) out |= 0x04;
	if (self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS)  out |= 0x08;
	if (self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_IS_GENERATOR)  out |= 0x20;
	if (self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_IS_COROUTINE)  out |= 0x80;

	return INTEGER_VAL(out);
}

KRK_Method(codeobject,__args__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	KrkTuple * tuple = functionArgs(_thread, self);
	return OBJECT_VAL(tuple);
}


#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkBoundMethod*

/* __init__ here will be called with a dummy instance as argv[0]; avoid
 * complications with method argument checking by not using KRK_METHOD. */
FUNC_SIG(method,__init__) {
	static __attribute__ ((unused)) const char* _method_name = "__init__";
	METHOD_TAKES_EXACTLY(2);
	if (!IS_OBJECT(argv[1])) return krk_runtimeError(vm.exceptions->typeError, "first argument must be a heap object");
	return OBJECT_VAL(krk_newBoundMethod(argv[2],AS_OBJECT(argv[1])));
}

KRK_Method(method,__name__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__name__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__qualname__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__qualname__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,_ip_to_line) {
	METHOD_TAKES_EXACTLY(1);
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,_ip_to_line)(_thread, 2,(KrkValue[]){OBJECT_VAL(self->method),argv[1]},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__str__) {
	METHOD_TAKES_NONE();
	KrkValue s = FUNC_NAME(method,__qualname__)(_thread, 1,argv,0);
	if (!IS_STRING(s)) s = FUNC_NAME(method,__name__)(_thread, 1,argv,0);
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
}

KRK_Method(method,__file__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__file__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__args__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__args__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__doc__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__doc__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__annotations__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__annotations__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__code__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__code__)(_thread, 1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__func__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return OBJECT_VAL(self->method);
}

KRK_Method(method,__self__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return OBJECT_VAL(self->receiver);
}

KRK_Function(staticmethod) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,CLOSURE,KrkClosure*,method);
	method->obj.flags &= ~(KRK_OBJ_FLAGS_FUNCTION_MASK);
	method->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD;
	return argv[0];
}

KRK_Function(classmethod) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,CLOSURE,KrkClosure*,method);
	method->obj.flags &= ~(KRK_OBJ_FLAGS_FUNCTION_MASK);
	method->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	return argv[0];
}

_noexport
void _createAndBind_functionClass(KrkThreadState * _thread) {
	KrkClass * codeobject = ADD_BASE_CLASS(vm.baseClasses->codeobjectClass, "codeobject", vm.baseClasses->objectClass);
	codeobject->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(codeobject,__init__);
	BIND_METHOD(codeobject,__str__);
	BIND_METHOD(codeobject,_ip_to_line);
	BIND_PROP(codeobject,__constants__);
	BIND_PROP(codeobject,__name__);
	BIND_PROP(codeobject,co_flags);
	BIND_PROP(codeobject,co_code);
	BIND_PROP(codeobject,co_argcount);
	BIND_PROP(codeobject,__locals__);
	BIND_PROP(codeobject,__args__);
	krk_defineNative(&codeobject->methods, "__repr__", FUNC_NAME(codeobject,__str__));
	krk_finalizeClass(codeobject);

	KrkClass * function = ADD_BASE_CLASS(vm.baseClasses->functionClass, "function", vm.baseClasses->objectClass);
	function->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(function,__init__);
	BIND_METHOD(function,__str__);
	BIND_METHOD(function,_ip_to_line);
	BIND_PROP(function,__doc__);
	BIND_PROP(function,__name__);
	BIND_PROP(function,__qualname__);
	BIND_PROP(function,__file__);
	BIND_PROP(function,__args__);
	BIND_PROP(function,__annotations__);
	BIND_PROP(function,__code__);
	BIND_PROP(function,__globals__);
	krk_defineNative(&function->methods, "__repr__", FUNC_NAME(function,__str__));
	krk_defineNative(&function->methods, "__class_getitem__", krk_GenericAlias)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	krk_finalizeClass(function);

	KrkClass * method = ADD_BASE_CLASS(vm.baseClasses->methodClass, "method", vm.baseClasses->objectClass);
	method->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(method,__str__);
	BIND_METHOD(method,_ip_to_line);
	BIND_METHOD(method,__init__);
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
