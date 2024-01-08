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

static KrkTuple * functionArgs(KrkCodeObject * _self) {
	KrkTuple * tuple = krk_newTuple(_self->totalArguments);
	krk_push(OBJECT_VAL(tuple));

	for (short i = 0; i < _self->potentialPositionals; ++i) {
		tuple->values.values[tuple->values.count++] = _self->positionalArgNames.values[i];
	}

	if (_self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
		tuple->values.values[tuple->values.count++] = krk_stringFromFormat("*%S", AS_STRING(_self->positionalArgNames.values[_self->potentialPositionals]));
	}

	for (short i = 0; i < _self->keywordArgs; ++i) {
		tuple->values.values[tuple->values.count++] = krk_stringFromFormat("%S=", AS_STRING(_self->keywordArgNames.values[i]));
	}

	if (_self->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
		tuple->values.values[tuple->values.count++] = krk_stringFromFormat("**%S", AS_STRING(_self->keywordArgNames.values[_self->keywordArgs]));
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

KRK_StaticMethod(function,__new__) {
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
		return nativeFunctionName(self);
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

KRK_Method(function,__repr__) {
	METHOD_TAKES_NONE();

	/* Do we have a qualified name? */
	KrkValue name = FUNC_NAME(function,__qualname__)(1,&self,0);
	if (IS_NONE(name)) {
		name = FUNC_NAME(function,__name__)(1,&self,0);
	}

	if (!IS_STRING(name)) name = OBJECT_VAL(S("<unnamed>"));

	krk_push(name);

	struct StringBuilder sb = {0};
	krk_pushStringBuilderFormat(&sb, "<function %S at %p>", AS_STRING(name), (void*)AS_OBJECT(self));

	krk_pop();

	return krk_finishStringBuilder(&sb);
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
	KrkTuple * tuple = functionArgs(AS_CLOSURE(self)->function);
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

KRK_Method(function,__closure__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	if (!IS_CLOSURE(self)) {
		return OBJECT_VAL(krk_newTuple(0));
	}

	size_t cnt = AS_CLOSURE(self)->upvalueCount;
	KrkTuple * out = krk_newTuple(cnt);
	krk_push(OBJECT_VAL(out));
	for (size_t i = 0; i < cnt; ++i) {
		out->values.values[out->values.count++] = OBJECT_VAL(AS_CLOSURE(self)->upvalues[i]);
	}

	return krk_pop();
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkCodeObject*

KRK_StaticMethod(codeobject,__new__) {
	return krk_runtimeError(vm.exceptions->typeError, "codeobject object is not instantiable");
}

KRK_Method(codeobject,__name__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->name ? OBJECT_VAL(self->name) : OBJECT_VAL(S(""));
}

KRK_Method(codeobject,__repr__) {
	METHOD_TAKES_NONE();
	KrkValue s = FUNC_NAME(codeobject,__name__)(1,argv,0);
	if (!IS_STRING(s)) return NONE_VAL();
	krk_push(s);

	struct StringBuilder sb = {0};
	krk_pushStringBuilderFormat(&sb, "<codeobject %S at %p>", AS_STRING(s), (void*)self);

	krk_pop();

	return krk_finishStringBuilder(&sb);
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

KRK_Method(codeobject,co_kwonlyargcount) {
	return INTEGER_VAL(self->keywordArgs);
}

KRK_Method(codeobject,co_posonlyargcount) {
	/* This is tricky because we don't store it anywhere */
	for (size_t i = 0; i < self->potentialPositionals; ++i) {
		if (!IS_NONE(self->positionalArgNames.values[i])) return INTEGER_VAL(i);
	}
	return INTEGER_VAL(0);
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
	KrkTuple * tuple = functionArgs(self);
	return OBJECT_VAL(tuple);
}

KRK_Method(codeobject,__file__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->chunk.filename ? OBJECT_VAL(self->chunk.filename) : OBJECT_VAL(S(""));
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkBoundMethod*

/* __init__ here will be called with a dummy instance as argv[0]; avoid
 * complications with method argument checking by not using KRK_METHOD. */
KRK_StaticMethod(method,__new__) {
	FUNCTION_TAKES_EXACTLY(3);
	if (!IS_OBJECT(argv[1])) return krk_runtimeError(vm.exceptions->typeError, "first argument must be a heap object");
	return OBJECT_VAL(krk_newBoundMethod(argv[2],AS_OBJECT(argv[1])));
}

KRK_Method(method,__name__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__name__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__qualname__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__qualname__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,_ip_to_line) {
	METHOD_TAKES_EXACTLY(1);
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,_ip_to_line)(2,(KrkValue[]){OBJECT_VAL(self->method),argv[1]},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__repr__) {
	METHOD_TAKES_NONE();
	KrkValue s = FUNC_NAME(method,__qualname__)(1,argv,0);
	if (!IS_STRING(s)) s = FUNC_NAME(method,__name__)(1,argv,0);
	if (!IS_STRING(s)) return NONE_VAL();
	krk_push(s);

	struct StringBuilder sb = {0};
	krk_pushStringBuilderFormat(&sb, "<bound method '%S' of %T object", AS_STRING(s), self->receiver);
	if (IS_OBJECT(self->receiver)) krk_pushStringBuilderFormat(&sb, " at %p", (void*)AS_OBJECT(self->receiver));
	krk_pushStringBuilder(&sb, '>');

	krk_pop();

	return krk_finishStringBuilder(&sb);
}

KRK_Method(method,__file__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__file__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__args__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__args__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__doc__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__doc__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__annotations__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__annotations__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__code__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return IS_function(OBJECT_VAL(self->method)) ? FUNC_NAME(function,__code__)(1,(KrkValue[]){OBJECT_VAL(self->method)},0) : OBJECT_VAL(S("?"));
}

KRK_Method(method,__func__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return OBJECT_VAL(self->method);
}

KRK_Method(method,__self__) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->receiver;
}

KRK_Function(staticmethod) {
	KrkObj* method;
	if (!krk_parseArgs("O!", (const char*[]){"method"}, KRK_BASE_CLASS(function), &method)) return NONE_VAL();
	method->flags &= ~(KRK_OBJ_FLAGS_FUNCTION_MASK);
	method->flags |= KRK_OBJ_FLAGS_FUNCTION_IS_STATIC_METHOD;
	return OBJECT_VAL(method);
}

KRK_Function(classmethod) {
	KrkObj* method;
	if (!krk_parseArgs("O!", (const char*[]){"method"}, KRK_BASE_CLASS(function), &method)) return NONE_VAL();
	method->flags &= ~(KRK_OBJ_FLAGS_FUNCTION_MASK);
	method->flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	return OBJECT_VAL(method);
}

_noexport
void _createAndBind_functionClass(void) {
	KrkClass * codeobject = ADD_BASE_CLASS(vm.baseClasses->codeobjectClass, "codeobject", vm.baseClasses->objectClass);
	codeobject->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	codeobject->allocSize =  0;
	BIND_STATICMETHOD(codeobject,__new__);
	BIND_METHOD(codeobject,__repr__);
	BIND_METHOD(codeobject,_ip_to_line);
	BIND_PROP(codeobject,__constants__);
	BIND_PROP(codeobject,__name__);
	BIND_PROP(codeobject,co_flags);
	BIND_PROP(codeobject,co_code);
	BIND_PROP(codeobject,co_argcount);
	BIND_PROP(codeobject,co_kwonlyargcount);
	BIND_PROP(codeobject,co_posonlyargcount);
	BIND_PROP(codeobject,__locals__);
	BIND_PROP(codeobject,__args__);
	BIND_PROP(codeobject,__file__);
	krk_finalizeClass(codeobject);

	KrkClass * function = ADD_BASE_CLASS(vm.baseClasses->functionClass, "function", vm.baseClasses->objectClass);
	function->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	function->allocSize =  0;
	BIND_STATICMETHOD(function,__new__);
	BIND_METHOD(function,__repr__);
	BIND_METHOD(function,_ip_to_line);
	BIND_PROP(function,__doc__);
	BIND_PROP(function,__name__);
	BIND_PROP(function,__qualname__);
	BIND_PROP(function,__file__);
	BIND_PROP(function,__args__);
	BIND_PROP(function,__annotations__);
	BIND_PROP(function,__code__);
	BIND_PROP(function,__globals__);
	BIND_PROP(function,__closure__);
	krk_defineNative(&function->methods, "__class_getitem__", krk_GenericAlias)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	krk_finalizeClass(function);

	KrkClass * method = ADD_BASE_CLASS(vm.baseClasses->methodClass, "method", vm.baseClasses->objectClass);
	method->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	method->allocSize =  0;
	BIND_STATICMETHOD(method,__new__);
	BIND_METHOD(method,__repr__);
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
	krk_finalizeClass(method);

	BUILTIN_FUNCTION("staticmethod", FUNC_NAME(krk,staticmethod), "A static method does not take an implicit self or cls argument.");
	BUILTIN_FUNCTION("classmethod", FUNC_NAME(krk,classmethod), "A class method takes an implicit cls argument, instead of self.");
}
