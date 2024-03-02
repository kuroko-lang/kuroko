#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/debug.h>

#include "private.h"

FUNC_SIG(list,__init__);
FUNC_SIG(list,sort);

KrkValue krk_dirObject(int argc, const KrkValue argv[], int hasKw) {
	if (argc != 1)
		return krk_runtimeError(vm.exceptions->argumentError, "%s() takes %s %d argument%s (%d given)",
			"dir", "exactly", 1, "", argc);

	/* Create a new list instance */
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	if (IS_INSTANCE(argv[0])) {
		/* Obtain self-reference */
		KrkInstance * self = AS_INSTANCE(argv[0]);
		for (size_t i = 0; i < self->fields.capacity; ++i) {
			if (!IS_KWARGS(self->fields.entries[i].key)) {
				krk_writeValueArray(AS_LIST(myList),
					self->fields.entries[i].key);
			}
		}
	} else if (IS_CLOSURE(argv[0])) {
		/* Why not just make closures instances... */
		KrkClosure * self = AS_CLOSURE(argv[0]);
		for (size_t i = 0; i < self->fields.capacity; ++i) {
			if (!IS_KWARGS(self->fields.entries[i].key)) {
				krk_writeValueArray(AS_LIST(myList),
					self->fields.entries[i].key);
			}
		}
	} else if (IS_CLASS(argv[0])) {
		KrkClass * _class = AS_CLASS(argv[0]);
		while (_class) {
			for (size_t i = 0; i < _class->methods.capacity; ++i) {
				if (!IS_KWARGS(_class->methods.entries[i].key)) {
					krk_writeValueArray(AS_LIST(myList),
						_class->methods.entries[i].key);
				}
			}
			_class = _class->base;
		}
	}

	KrkClass * type = krk_getType(argv[0]);

	while (type) {
		for (size_t i = 0; i < type->methods.capacity; ++i) {
			if (!IS_KWARGS(type->methods.entries[i].key)) {
				krk_writeValueArray(AS_LIST(myList),
					type->methods.entries[i].key);
			}
		}
		type = type->base;
	}

	/* Throw it at a set to get unique, unordered results */
	krk_push(krk_set_of(AS_LIST(myList)->count, AS_LIST(myList)->values, 0));
	krk_swap(1);
	krk_pop();

	/* Now build a fresh list */
	myList = krk_list_of(0,NULL,0);
	krk_push(myList);
	krk_swap(1);
	FUNC_NAME(list,__init__)(2,(KrkValue[]){krk_peek(1), krk_peek(0)},0);
	FUNC_NAME(list,sort)(1,(KrkValue[]){krk_peek(1)},0);
	krk_pop();

	return krk_pop();
}

#define IS_object(o) (1)
#define AS_object(o) (o)
#define CURRENT_CTYPE KrkValue
#define CURRENT_NAME  self

KRK_Method(object,__dir__) {
	return krk_dirObject(argc,argv,hasKw);
}

KRK_Method(object,__class__) {
	KrkClass * current = krk_getType(self);
	if (argc > 1) {
		if (!IS_CLASS(argv[1])) return krk_runtimeError(vm.exceptions->typeError, "'%T' object is not a class", argv[1]);
		if (!IS_INSTANCE(argv[0]) || current->allocSize != sizeof(KrkInstance)) return krk_runtimeError(vm.exceptions->typeError, "'%T' object does not have modifiable type", argv[0]); /* TODO class? */
		if (AS_CLASS(argv[1])->allocSize != sizeof(KrkInstance)) return krk_runtimeError(vm.exceptions->typeError, "'%S' type is not assignable", AS_CLASS(argv[1])->name);
		AS_INSTANCE(argv[0])->_class = AS_CLASS(argv[1]);
		current = AS_CLASS(argv[1]);
	}
	return OBJECT_VAL(current);
}

KRK_Method(object,__hash__) {
	if (!IS_OBJECT(self)) {
		uint32_t hashed;
		if (krk_hashValue(self, &hashed)) return NONE_VAL();
		return INTEGER_VAL(hashed);
	}
	KrkObj * obj = AS_OBJECT(self);
	if (!(obj->flags & KRK_OBJ_FLAGS_VALID_HASH)) {
		obj->hash = (uint32_t)((intptr_t)(obj) >> 3);
		obj->flags |= KRK_OBJ_FLAGS_VALID_HASH;
	}
	return INTEGER_VAL(obj->hash);
}

KRK_Method(object,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (krk_valuesSame(argv[0],argv[1])) return BOOLEAN_VAL(1);
	return NOTIMPL_VAL();
}

KRK_Function(getattr) {
	FUNCTION_TAKES_AT_LEAST(2);
	CHECK_ARG(1,str,KrkString*,property);

	krk_push(argv[0]);
	if (!krk_getAttribute(AS_STRING(argv[1]))) {
		krk_pop();
		if (argc == 3) return argv[2];
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", argv[0], AS_STRING(argv[1]));
	}
	return krk_pop();
}

KRK_Function(setattr) {
	FUNCTION_TAKES_EXACTLY(3);
	CHECK_ARG(1,str,KrkString*,property);

	krk_push(argv[0]);
	krk_push(argv[2]);
	if (!krk_setAttribute(AS_STRING(argv[1]))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", argv[0], AS_STRING(argv[1]));
	}
	return krk_pop();
}

KRK_Function(hasattr) {
	FUNCTION_TAKES_AT_LEAST(2);
	CHECK_ARG(1,str,KrkString*,property);

	krk_push(argv[0]);
	if (!krk_getAttribute(AS_STRING(argv[1]))) {
		krk_pop();
		return BOOLEAN_VAL(0);
	}
	krk_pop();
	return BOOLEAN_VAL(1);
}

KRK_Function(delattr) {
	FUNCTION_TAKES_AT_LEAST(2);
	CHECK_ARG(1,str,KrkString*,property);

	krk_push(argv[0]);
	if (!krk_delAttribute(AS_STRING(argv[1]))) {
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%S'", argv[0], AS_STRING(argv[1]));
	}
	return NONE_VAL();
}

/**
 * This must be marked as static so it doesn't get bound, or every object will
 * have a functioning __setattr__ and the VM will get a lot slower... I think...
 */
KRK_Method(object,__setattr__) {
	METHOD_TAKES_EXACTLY(2);
	if (!IS_STRING(argv[1])) return krk_runtimeError(vm.exceptions->typeError, "expected str");

	if (!IS_INSTANCE(argv[0])) {
		return FUNC_NAME(krk,setattr)(argc,argv,hasKw);
	}

	/* It's an instance, that presumably does not have a `__setattr__`? */
	return krk_instanceSetAttribute_wrapper(argv[0], AS_STRING(argv[1]), argv[2]);
}

KRK_StaticMethod(object,__new__) {
	KrkClass * _class = NULL;

	/* We don't actually care, but we want to accept them anyway */
	int _argc = 0;
	const KrkValue * _args = NULL;

	if (!krk_parseArgs("O!*~", (const char*[]){"cls"}, vm.baseClasses->typeClass, &_class, &_argc, &_args)) {
		return NONE_VAL();
	}

	KrkClass * _cls = _class;
	while (_cls) {
		if (_cls->_new && IS_NATIVE(OBJECT_VAL(_cls->_new)) && _cls->_new != KRK_BASE_CLASS(object)->_new) {
			return krk_runtimeError(vm.exceptions->typeError, "object.__new__(%S) is not safe, use %S.__new__()", _class->name, _cls->name);
		}
		_cls = _cls->base;
	}

	if (_class->_init == vm.baseClasses->objectClass->_init && (_argc || (hasKw && AS_DICT(argv[argc])->count))) {
		return krk_runtimeError(vm.exceptions->typeError, "%S() takes no arguments", _class->name);
	}

	return OBJECT_VAL(krk_newInstance(_class));
}

KRK_Method(object,__init__) {
	return NONE_VAL();
}

KRK_StaticMethod(object,__init_subclass__) {
	if (!krk_parseArgs(".", (const char*[]){NULL}, NULL)) return NONE_VAL();
	return NONE_VAL();
}


/**
 * object.__str__() / object.__repr__()
 *
 * Base method for all objects to implement __str__ and __repr__.
 * Generally converts to <instance of [TYPE]> and for actual object
 * types (functions, classes, instances, strings...) also adds the pointer
 * address of the object on the heap.
 *
 * Since all types have at least a pseudo-class that should eventually
 * inheret from object() and this is object.__str__ / object.__repr__,
 * all types should have a string representation available through
 * those methods.
 */
KRK_Method(object,__repr__) {
	KrkClass * type = krk_getType(self);

	KrkValue module = NONE_VAL();
	krk_tableGet(&type->methods, OBJECT_VAL(S("__module__")), &module);
	KrkValue qualname = NONE_VAL();
	krk_tableGet(&type->methods, OBJECT_VAL(S("__qualname__")), &qualname);
	KrkString * name = IS_STRING(qualname) ? AS_STRING(qualname) : type->name;
	int includeModule = !(IS_NONE(module) || (IS_STRING(module) && AS_STRING(module) == S("builtins")));

	struct StringBuilder sb = {0};

	if (!krk_pushStringBuilderFormat(&sb, "<%s%s%s object",
		includeModule ? AS_CSTRING(module) : "",
		includeModule ? "." : "",
		name->chars)) goto _error;

	if (IS_OBJECT(self) && !krk_pushStringBuilderFormat(&sb, " at %p", (void*)AS_OBJECT(self))) {
		goto _error;
	}

	krk_pushStringBuilder(&sb, '>');
	return krk_finishStringBuilder(&sb);

_error:
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

KRK_Method(object,__str__) {
	KrkClass * type = krk_getType(self);
	if (unlikely(!type->_reprer)) return krk_runtimeError(vm.exceptions->typeError, "object is not representable");
	krk_push(self);
	return krk_callDirect(type->_reprer, 1);
}

KRK_Method(object,__format__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
	if (AS_STRING(argv[1])->length != 0) return krk_runtimeError(vm.exceptions->typeError, "Unsupported format string");
	KrkClass * type = krk_getType(argv[0]);
	if (!type->_tostr) return krk_runtimeError(vm.exceptions->typeError, "'%T' can not be converted to str", argv[0]);
	krk_push(argv[0]);
	return krk_callDirect(type->_tostr, 1);
}

#undef CURRENT_CTYPE
#undef CURRENT_NAME

KRK_Function(len) {
	FUNCTION_TAKES_EXACTLY(1);
	/* Shortcuts */
	if (IS_STRING(argv[0])) return INTEGER_VAL(AS_STRING(argv[0])->codesLength);
	if (IS_TUPLE(argv[0])) return INTEGER_VAL(AS_TUPLE(argv[0])->values.count);

	KrkClass * type = krk_getType(argv[0]);
	if (!type->_len) return krk_runtimeError(vm.exceptions->typeError, "object of type '%T' has no len()", argv[0]);
	krk_push(argv[0]);

	return krk_callDirect(type->_len, 1);
}

KRK_Function(dir) {
	FUNCTION_TAKES_AT_MOST(1);
	if (argc) {
		KrkClass * type = krk_getType(argv[0]);
		if (!type->_dir) {
			return krk_dirObject(argc,argv,hasKw); /* Fallback */
		}
		krk_push(argv[0]);
		return krk_callDirect(type->_dir, 1);
	} else {
		/* Current globals */
		if (!krk_currentThread.frameCount) {
			return krk_runtimeError(vm.exceptions->nameError, "No active globals context");
		}

		/* Set up a new list for our output */
		KrkValue myList = krk_list_of(0,NULL,0);
		krk_push(myList);

		/* Put all the keys from the globals table in it */
		KrkTable * globals = krk_currentThread.frames[krk_currentThread.frameCount-1].globals;
		for (size_t i = 0; i < globals->used; ++i) {
			KrkTableEntry * entry = &globals->entries[i];
			if (IS_KWARGS(entry->key)) continue;
			krk_writeValueArray(AS_LIST(myList), entry->key);
		}

		/* Now sort it */
		FUNC_NAME(list,sort)(1,(KrkValue[]){krk_peek(0)},0);
		return krk_pop(); /* Return the list */
	}
}

KRK_Function(repr) {
	FUNCTION_TAKES_EXACTLY(1);
	/* Everything should have a __repr__ */
	KrkClass * type = krk_getType(argv[0]);
	krk_push(argv[0]);
	return krk_callDirect(type->_reprer, 1);
}

#define trySlowMethod(name) do { \
	KrkClass * type = krk_getType(argv[0]); \
	KrkValue method; \
	while (type) { \
		if (krk_tableGet(&type->methods, name, &method)) { \
			krk_push(method); \
			krk_push(argv[0]); \
			return krk_callStack(1); \
		} \
		type = type->base; \
	} \
} while (0)

KRK_Function(ord) {
	FUNCTION_TAKES_EXACTLY(1);
	trySlowMethod(vm.specialMethodNames[METHOD_ORD]);
	return TYPE_ERROR(string of length 1,argv[0]);
}

KRK_Function(chr) {
	FUNCTION_TAKES_EXACTLY(1);
	trySlowMethod(vm.specialMethodNames[METHOD_CHR]);
	return TYPE_ERROR(int,argv[0]);
}

KRK_Function(hex) {
	FUNCTION_TAKES_EXACTLY(1);
	trySlowMethod(vm.specialMethodNames[METHOD_HEX]);
	return TYPE_ERROR(int,argv[0]);
}

KRK_Function(oct) {
	FUNCTION_TAKES_EXACTLY(1);
	trySlowMethod(vm.specialMethodNames[METHOD_OCT]);
	return TYPE_ERROR(int,argv[0]);
}

KRK_Function(bin) {
	FUNCTION_TAKES_EXACTLY(1);
	trySlowMethod(vm.specialMethodNames[METHOD_BIN]);
	return TYPE_ERROR(int,argv[0]);
}

#define KRK_STRING_FAST(string,offset)  (uint32_t)\
	((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) <= (KRK_OBJ_FLAGS_STRING_UCS1) ? ((uint8_t*)string->codes)[offset] : \
	((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == (KRK_OBJ_FLAGS_STRING_UCS2) ? ((uint16_t*)string->codes)[offset] : \
	((uint32_t*)string->codes)[offset]))

int krk_unpackIterable(KrkValue iterable, void * context, int callback(void *, const KrkValue *, size_t)) {
	if (IS_TUPLE(iterable)) {
		if (callback(context, AS_TUPLE(iterable)->values.values, AS_TUPLE(iterable)->values.count)) return 1;
	} else if (IS_list(iterable)) {
		if (callback(context, AS_LIST(iterable)->values, AS_LIST(iterable)->count)) return 1;
	} else if (IS_dict(iterable)) {
		for (size_t i = 0; i < AS_DICT(iterable)->used; ++i) {
			if (!IS_KWARGS(AS_DICT(iterable)->entries[i].key)) {
				if (callback(context, &AS_DICT(iterable)->entries[i].key, 1)) return 1;
			}
		}
	} else if (IS_STRING(iterable)) {
		krk_unicodeString(AS_STRING(iterable));
		for (size_t i = 0; i < AS_STRING(iterable)->codesLength; ++i) {
			KrkValue s = krk_string_get(2, (KrkValue[]){iterable,INTEGER_VAL(i)}, i);
			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
			krk_push(s);
			if (callback(context, &s, 1)) {
				krk_pop();
				return 1;
			}
			krk_pop();
		}
	} else {
		KrkClass * type = krk_getType(iterable);
		if (unlikely(!type->_iter)) {
			krk_runtimeError(vm.exceptions->typeError, "'%T' object is not iterable", iterable);
			return 1;
		}

		/* Build the iterable */
		krk_push(iterable);
		KrkValue iterator = krk_callDirect(type->_iter, 1);

		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;

		krk_push(iterator);

		do {
			/* Call the iterator */
			krk_push(iterator);
			KrkValue item = krk_callStack(0);

			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
				krk_pop(); /* __iter__() result */
				return 1;
			}

			if (krk_valuesSame(iterator, item)) {
				break;
			}

			krk_push(item);
			if (callback(context, &item, 1)) {
				krk_pop(); /* item */
				krk_pop(); /* __iter__ */
				return 1;
			}
			krk_pop(); /* item */
		} while (1);

		krk_pop(); /* __iter__() result */
	}
	return 0;
}

struct SimpleContext {
	KrkValue base;
};

static int _any_callback(void * context, const KrkValue * values, size_t count) {
	struct SimpleContext * _context = context;
	for (size_t i = 0; i < count; ++i) {
		if (!krk_isFalsey(values[i])) {
			_context->base = BOOLEAN_VAL(1);
			return 1;
		}
	}
	return 0;
}

KRK_Function(any) {
	FUNCTION_TAKES_EXACTLY(1);
	struct SimpleContext context = { BOOLEAN_VAL(0) };
	krk_unpackIterable(argv[0], &context, _any_callback);
	return context.base;
}

static int _all_callback(void * context, const KrkValue * values, size_t count) {
	struct SimpleContext * _context = context;
	for (size_t i = 0; i < count; ++i) {
		if (krk_isFalsey(values[i])) {
			_context->base = BOOLEAN_VAL(0);
			return 1;
		}
	}
	return 0;
}

KRK_Function(all) {
	FUNCTION_TAKES_EXACTLY(1);
	struct SimpleContext context = { BOOLEAN_VAL(1) };
	krk_unpackIterable(argv[0], &context, _all_callback);
	return context.base;
}

#define CURRENT_CTYPE KrkInstance *
#define CURRENT_NAME  self

#define IS_map(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(map)))
#define AS_map(o) (AS_INSTANCE(o))
KRK_Method(map,__init__) {
	METHOD_TAKES_AT_LEAST(2);

	/* Attach the function to it */
	krk_attachNamedValue(&self->fields, "_function", argv[1]);

	/* Make the iter objects */
	KrkTuple * iters = krk_newTuple(argc - 2);
	krk_push(OBJECT_VAL(iters));

	/* Attach the tuple to the object */
	krk_attachNamedValue(&self->fields, "_iterables", krk_peek(0));
	krk_pop();

	for (int i = 2; i < argc; ++i) {
		KrkClass * type = krk_getType(argv[i]);
		if (!type->_iter) {
			return krk_runtimeError(vm.exceptions->typeError, "'%T' object is not iterable", argv[i]);
		}
		krk_push(argv[i]);
		KrkValue asIter = krk_callDirect(type->_iter, 1);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		/* Attach it to the tuple */
		iters->values.values[iters->values.count++] = asIter;
	}

	return NONE_VAL();
}

KRK_Method(map,__iter__) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
}

KRK_Method(map,__call__) {
	METHOD_TAKES_NONE();

	size_t stackOffset = krk_currentThread.stackTop - krk_currentThread.stack;

	/* Get members */
	KrkValue function = NONE_VAL();
	KrkValue iterators = NONE_VAL();

	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_function")), &function)) return krk_runtimeError(vm.exceptions->valueError, "corrupt map object");
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_iterables")), &iterators) || !IS_TUPLE(iterators)) return krk_runtimeError(vm.exceptions->valueError, "corrupt map object");

	krk_push(function);

	/* Go through each iterator */
	for (size_t i = 0; i < AS_TUPLE(iterators)->values.count; ++i) {
		/* Obtain the next value and push it */
		krk_push(AS_TUPLE(iterators)->values.values[i]);
		krk_push(krk_callStack(0));
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		/* End iteration whenever one runs out */
		if (krk_valuesEqual(krk_peek(0), AS_TUPLE(iterators)->values.values[i])) {
			for (size_t j = 0; j < i + 1; ++j) krk_pop();
			krk_pop(); /* the function */
			return OBJECT_VAL(self);
		}
	}

	/* Call the function */
	KrkValue val = krk_callStack(AS_TUPLE(iterators)->values.count);
	krk_currentThread.stackTop = krk_currentThread.stack + stackOffset;
	return val;
}

#define IS_zip(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(zip)))
#define AS_zip(o) (AS_INSTANCE(o))
KRK_Method(zip,__init__) {
	if (hasKw && AS_DICT(argv[argc])->count) return krk_runtimeError(vm.exceptions->typeError, "%s() takes no keyword arguments", "zip");

	KrkTuple * iters = krk_newTuple(argc - 1);
	krk_push(OBJECT_VAL(iters));
	krk_attachNamedValue(&self->fields, "_iterables", krk_peek(0));
	krk_pop();

	for (int i = 1; i < argc; ++i) {
		KrkClass * type = krk_getType(argv[i]);
		if (!type->_iter) {
			return krk_runtimeError(vm.exceptions->typeError, "'%T' object is not iterable", argv[i]);
		}
		krk_push(argv[i]);
		KrkValue asIter = krk_callDirect(type->_iter, 1);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		/* Attach it to the tuple */
		iters->values.values[iters->values.count++] = asIter;
	}

	return NONE_VAL();
}

KRK_Method(zip,__iter__) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
}

KRK_Method(zip,__call__) {
	METHOD_TAKES_NONE();

	/* Get members */
	KrkValue iterators = NONE_VAL();
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_iterables")), &iterators) || !IS_TUPLE(iterators)) return krk_runtimeError(vm.exceptions->valueError, "corrupt zip object");

	/* Set up a tuple */
	KrkTuple * out = krk_newTuple(AS_TUPLE(iterators)->values.count);
	krk_push(OBJECT_VAL(out));

	/* Go through each iterator */
	for (size_t i = 0; i < AS_TUPLE(iterators)->values.count; ++i) {
		krk_push(AS_TUPLE(iterators)->values.values[i]);
		krk_push(krk_callStack(0));
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		if (krk_valuesEqual(krk_peek(0), AS_TUPLE(iterators)->values.values[i])) {
			return OBJECT_VAL(self);
		}
		out->values.values[out->values.count++] = krk_pop();
	}

	/* Return tuple */
	return krk_pop();
}

#define IS_filter(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(filter)))
#define AS_filter(o) (AS_INSTANCE(o))
KRK_Method(filter,__init__) {
	METHOD_TAKES_EXACTLY(2);
	krk_attachNamedValue(&self->fields, "_function", argv[1]);
	KrkClass * type = krk_getType(argv[2]);
	if (!type->_iter) {
		return krk_runtimeError(vm.exceptions->typeError, "'%T' object is not iterable", argv[2]);
	}
	krk_push(argv[2]);
	KrkValue asIter = krk_callDirect(type->_iter, 1);
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
	krk_attachNamedValue(&self->fields, "_iterator", asIter);

	return NONE_VAL();
}

KRK_Method(filter,__iter__) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
}

KRK_Method(filter,__call__) {
	METHOD_TAKES_NONE();
	size_t stackOffset = krk_currentThread.stackTop - krk_currentThread.stack;
	KrkValue function = NONE_VAL();
	KrkValue iterator = NONE_VAL();

	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_function")), &function)) return krk_runtimeError(vm.exceptions->valueError, "corrupt filter object");
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_iterator")), &iterator)) return krk_runtimeError(vm.exceptions->valueError, "corrupt filter object");

	while (1) {
		krk_push(iterator);
		krk_push(krk_callStack(0));

		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		if (krk_valuesEqual(iterator, krk_peek(0))) {
			krk_currentThread.stackTop = krk_currentThread.stack + stackOffset;
			return OBJECT_VAL(self);
		}

		if (IS_NONE(function)) {
			if (krk_isFalsey(krk_peek(0))) {
				krk_pop(); /* iterator result */
				continue;
			}
		} else {
			krk_push(function);
			krk_push(krk_peek(1));
			KrkValue result = krk_callStack(1);
			if (krk_isFalsey(result)) {
				krk_pop(); /* iterator result */
				continue;
			}
		}

		KrkValue out = krk_pop();
		krk_currentThread.stackTop = krk_currentThread.stack + stackOffset;
		return out;
	}
}

#define IS_enumerate(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(enumerate)))
#define AS_enumerate(o) (AS_INSTANCE(o))
KRK_Method(enumerate,__init__) {
	KrkValue iterator;
	KrkValue start = INTEGER_VAL(0);
	if (!krk_parseArgs(".V|V", (const char*[]){"iterable","start"}, &iterator, &start)) return NONE_VAL();

	krk_attachNamedValue(&self->fields, "_counter", start);

	/* Attach iterator */
	KrkClass * type = krk_getType(iterator);
	if (!type->_iter) {
		return krk_runtimeError(vm.exceptions->typeError, "'%T' object is not iterable", iterator);
	}
	krk_push(iterator);
	KrkValue asIter = krk_callDirect(type->_iter, 1);
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
	krk_attachNamedValue(&self->fields, "_iterator", asIter);

	return NONE_VAL();
}

KRK_Method(enumerate,__iter__) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
}

extern KrkValue krk_operator_add (KrkValue a, KrkValue b);
KRK_Method(enumerate,__call__) {
	METHOD_TAKES_NONE();
	size_t stackOffset = krk_currentThread.stackTop - krk_currentThread.stack;
	KrkValue counter = NONE_VAL();
	KrkValue iterator = NONE_VAL();

	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_counter")), &counter)) return krk_runtimeError(vm.exceptions->valueError, "corrupt enumerate object");
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("_iterator")), &iterator)) return krk_runtimeError(vm.exceptions->valueError, "corrupt enumerate object");

	KrkTuple * tupleOut = krk_newTuple(2);
	krk_push(OBJECT_VAL(tupleOut));

	krk_push(iterator);
	krk_push(krk_callStack(0));

	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
		krk_currentThread.stackTop = krk_currentThread.stack + stackOffset;
		return NONE_VAL();
	}
	if (krk_valuesEqual(iterator, krk_peek(0))) {
		krk_pop();
		krk_pop();
		krk_currentThread.stackTop = krk_currentThread.stack + stackOffset;
		return OBJECT_VAL(self);
	}

	/* Make a tuple */
	tupleOut->values.values[tupleOut->values.count++] = counter;
	tupleOut->values.values[tupleOut->values.count++] = krk_pop();

	krk_push(krk_operator_add(counter, INTEGER_VAL(1)));
	krk_attachNamedValue(&self->fields, "_counter", krk_pop());

	KrkValue out = krk_pop();
	krk_currentThread.stackTop = krk_currentThread.stack + stackOffset;
	return out;
}

static int _sum_callback(void * context, const KrkValue * values, size_t count) {
	struct SimpleContext * _context = context;
	for (size_t i = 0; i < count; ++i) {
		_context->base = krk_operator_add(_context->base, values[i]);
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
	}
	return 0;
}

KRK_Function(sum) {
	KrkValue iterable;
	KrkValue base = INTEGER_VAL(0);
	if (!krk_parseArgs("V|$V", (const char*[]){"iterable","start"}, &iterable, &base)) return NONE_VAL();
	struct SimpleContext context = { base };
	if (krk_unpackIterable(iterable, &context, _sum_callback)) return NONE_VAL();
	return context.base;
}

static int _min_callback(void * context, const KrkValue * values, size_t count) {
	struct SimpleContext * _context = context;
	for (size_t i = 0; i < count; ++i) {
		if (IS_KWARGS(_context->base)) _context->base = values[i];
		else {
			KrkValue check = krk_operator_lt(values[i], _context->base);
			if (!IS_BOOLEAN(check)) return 1;
			else if (AS_BOOLEAN(check) == 1) _context->base = values[i];
		}
	}
	return 0;
}

KRK_Function(min) {
	FUNCTION_TAKES_AT_LEAST(1);
	struct SimpleContext context = { KWARGS_VAL(0) };
	if (argc > 1) {
		if (_min_callback(&context, argv, argc)) return NONE_VAL();
	} else {
		if (krk_unpackIterable(argv[0], &context, _min_callback)) return NONE_VAL();
	}
	if (IS_KWARGS(context.base)) return krk_runtimeError(vm.exceptions->valueError, "empty argument to %s()", "min");
	return context.base;
}

static int _max_callback(void * context, const KrkValue * values, size_t count) {
	struct SimpleContext * _context = context;
	for (size_t i = 0; i < count; ++i) {
		if (IS_KWARGS(_context->base)) _context->base = values[i];
		else {
			KrkValue check = krk_operator_gt(values[i], _context->base);
			if (!IS_BOOLEAN(check)) return 1;
			else if (AS_BOOLEAN(check) == 1) _context->base = values[i];
		}
	}
	return 0;
}

KRK_Function(max) {
	FUNCTION_TAKES_AT_LEAST(1);
	struct SimpleContext context = { KWARGS_VAL(0) };
	if (argc > 1) {
		if (_max_callback(&context, argv, argc)) return NONE_VAL();
	} else {
		if (krk_unpackIterable(argv[0], &context, _max_callback)) return NONE_VAL();
	}
	if (IS_KWARGS(context.base)) return krk_runtimeError(vm.exceptions->valueError, "empty argument to %s()", "max");
	return context.base;
}

KRK_Function(print) {
	char * sep = NULL;
	char * end = NULL;
	size_t sepLen = 0;
	size_t endLen = 0;
	int remArgc;
	const KrkValue * remArgv;
	KrkValue file = NONE_VAL();
	int flush = 0;

	if (!krk_parseArgs("*z#z#Vp", (const char*[]){"sep","end","file","flush"},
		&remArgc, &remArgv,
		&sep, &sepLen, &end, &endLen,
		&file, &flush)) {
		return NONE_VAL();
	}

	/* Set default separator and end if not provided or set to None. */
	if (!sep) { sep = " "; sepLen = 1; }
	if (!end) { end = "\n"; endLen = 1; }

	for (int i = 0; i < remArgc; ++i) {

		/* If printing through a file object, get its @c write method */
		if (!IS_NONE(file)) {
			krk_push(krk_valueGetAttribute(file, "write"));
			if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		}

		/* Convert the argument to a printable form, first by trying __str__, then __repr__ */
		KrkValue printable = remArgv[i];
		krk_push(printable);
		if (!IS_STRING(printable)) {
			KrkClass * type = krk_getType(printable);
			if (type->_tostr) {
				krk_push((printable = krk_callDirect(type->_tostr, 1)));
			} else if (type->_reprer) {
				krk_push((printable = krk_callDirect(type->_reprer, 1)));
			}
			if (!IS_STRING(printable)) return krk_runtimeError(vm.exceptions->typeError, "__str__ returned non-string (type %T)", printable);
		}

		if (!IS_NONE(file)) {
			/* Call @c write */
			krk_callStack(1);
			if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();

			/* Print separator */
			if (i + 1 < remArgc) {
				krk_push(krk_valueGetAttribute(file, "write"));
				krk_push(OBJECT_VAL(krk_copyString(sep,sepLen)));
				krk_callStack(1);
				if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
			}
		} else {
			fwrite(AS_CSTRING(printable), AS_STRING(printable)->length, 1, stdout);
			krk_pop();
			if (i + 1 < remArgc) fwrite(sep, sepLen, 1, stdout);
		}
	}

	if (!IS_NONE(file)) {
		/* Print end */
		krk_push(krk_valueGetAttribute(file, "write"));
		krk_push(OBJECT_VAL(krk_copyString(end,endLen)));
		krk_callStack(1);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();

		/* Maybe flush */
		if (flush) {
			krk_push(krk_valueGetAttribute(file, "flush"));
			krk_callStack(0);
		}
	}else {
		fwrite(end, endLen, 1, stdout);
		if (flush) fflush(stdout);
	}

	return NONE_VAL();
}

/**
 * globals()
 *
 * Returns a dict of names -> values for all the globals.
 */
KRK_Function(globals) {
	FUNCTION_TAKES_NONE();
	/* Make a new empty dict */
	KrkValue dict = krk_dict_of(0, NULL, 0);
	krk_push(dict);
	/* Copy the globals table into it */
	krk_tableAddAll(krk_currentThread.frames[krk_currentThread.frameCount-1].globals, AS_DICT(dict));
	krk_pop();

	return dict;
}

/**
 * locals()
 *
 * This is a bit trickier. Local names are... complicated. But we can do this!
 */
KRK_Function(locals) {
	FUNCTION_TAKES_AT_MOST(1);
	KrkValue dict = krk_dict_of(0, NULL, 0);
	krk_push(dict);

	int index = 1;
	if (argc > 0 && IS_INTEGER(argv[0])) {
		if (AS_INTEGER(argv[0]) < 1) {
			return krk_runtimeError(vm.exceptions->indexError, "Frame index must be >= 1");
		}
		if (krk_currentThread.frameCount < (size_t)AS_INTEGER(argv[0])) {
			return krk_runtimeError(vm.exceptions->indexError, "Frame index out of range");
		}
		index = AS_INTEGER(argv[0]);
	}

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount-index];
	KrkCodeObject * func = frame->closure->function;
	size_t offset = frame->ip - func->chunk.code;

	/* First, we'll populate with arguments */
	size_t slot = 0;
	for (short int i = 0; i < func->potentialPositionals; ++i) {
		krk_tableSet(AS_DICT(dict),
			func->positionalArgNames.values[i],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	if (func->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
		krk_tableSet(AS_DICT(dict),
			func->positionalArgNames.values[func->potentialPositionals],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	for (short int i = 0; i < func->keywordArgs; ++i) {
		krk_tableSet(AS_DICT(dict),
			func->keywordArgNames.values[i],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	if (func->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
		krk_tableSet(AS_DICT(dict),
			func->keywordArgNames.values[func->keywordArgs],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	/* Now we need to find out what non-argument locals are valid... */
	for (size_t i = 0; i < func->localNameCount; ++i) {
		if (func->localNames[i].birthday <= offset &&
			func->localNames[i].deathday >= offset) {
			krk_tableSet(AS_DICT(dict),
				OBJECT_VAL(func->localNames[i].name),
				krk_currentThread.stack[frame->slots + func->localNames[i].id]);
		}
	}

	return krk_pop();
}

KRK_Function(isinstance) {
	FUNCTION_TAKES_EXACTLY(2);
	if (IS_CLASS(argv[1])) {
		return BOOLEAN_VAL(krk_isInstanceOf(argv[0], AS_CLASS(argv[1])));
	} else if (IS_TUPLE(argv[1])) {
		for (size_t i = 0; i < AS_TUPLE(argv[1])->values.count; ++i) {
			if (IS_CLASS(AS_TUPLE(argv[1])->values.values[i]) && krk_isInstanceOf(argv[0], AS_CLASS(AS_TUPLE(argv[1])->values.values[i]))) {
				return BOOLEAN_VAL(1);
			}
		}
		return BOOLEAN_VAL(0);
	} else {
		return TYPE_ERROR(class or tuple,argv[1]);
	}
}

KRK_Function(issubclass) {
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,class,KrkClass*,cls);
	if (IS_CLASS(argv[1])) {
		return BOOLEAN_VAL(krk_isSubClass(cls, AS_CLASS(argv[1])));
	} else if (IS_TUPLE(argv[1])) {
		for (size_t i = 0; i < AS_TUPLE(argv[1])->values.count; ++i) {
			if (IS_CLASS(AS_TUPLE(argv[1])->values.values[i]) && krk_isSubClass(cls, AS_CLASS(AS_TUPLE(argv[1])->values.values[i]))) {
				return BOOLEAN_VAL(1);
			}
		}
		return BOOLEAN_VAL(0);
	} else {
		return TYPE_ERROR(class or tuple,argv[1]);
	}
}

#define IS_module(o) (IS_INSTANCE(o))
#define AS_module(o) (AS_INSTANCE(o))

KRK_Method(module,__repr__) {
	KrkValue name = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_NAME], &name);

	if (!IS_STRING(name)) {
		return OBJECT_VAL(S("<module>"));
	}

	KrkValue file = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_FILE], &file);

	struct StringBuilder sb = {0};

	if (!krk_pushStringBuilderFormat(&sb,"<module '%S' ", AS_STRING(name))) goto _error;

	if (IS_STRING(file)) {
		if (!krk_pushStringBuilderFormat(&sb, "from %R>", file)) goto _error;
	} else {
		if (!krk_pushStringBuilderFormat(&sb, "(built-in)>")) goto _error;
	}

	return krk_finishStringBuilder(&sb);
_error:
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

#define IS_Helper(o)  (krk_isInstanceOf(o, KRK_BASE_CLASS(Helper)))
#define AS_Helper(o)  (AS_INSTANCE(o))
#define IS_LicenseReader(o) (krk_isInstanceOf(o, KRK_BASE_CLASS(LicenseReader)))
#define AS_LicenseReader(o) (AS_INSTANCE(o))

KRK_Method(Helper,__repr__) {
	return OBJECT_VAL(S("Type help() for more help, or help(obj) to describe an object."));
}

KRK_Method(Helper,__call__) {
	METHOD_TAKES_AT_MOST(1);
	if (!krk_doRecursiveModuleLoad(S("help"))) return NONE_VAL();
	KrkValue helpModule = krk_pop();
	KrkValue callable = NONE_VAL();

	if (argc == 2) {
		krk_tableGet(&AS_INSTANCE(helpModule)->fields, OBJECT_VAL(S("simple")), &callable);
	} else {
		krk_tableGet(&AS_INSTANCE(helpModule)->fields, OBJECT_VAL(S("interactive")), &callable);
	}

	if (!IS_NONE(callable)) {
		krk_push(callable);
		if (argc == 2) krk_push(argv[1]);
		return krk_callStack(argc == 2);
	}

	return krk_runtimeError(vm.exceptions->typeError, "unexpected error");
}

KRK_Method(LicenseReader,__repr__) {
	return OBJECT_VAL(S("Copyright 2020-2022 K. Lange <klange@toaruos.org>. Type `license()` for more information."));
}

KRK_Method(LicenseReader,__call__) {
	METHOD_TAKES_NONE();
	if (!krk_doRecursiveModuleLoad(S("help"))) return NONE_VAL();
	KrkValue helpModule = krk_pop();

	KrkValue text = NONE_VAL();
	krk_tableGet(&AS_INSTANCE(helpModule)->fields, OBJECT_VAL(S("__licenseText")), &text);

	if (IS_STRING(text)) {
		printf("%s\n", AS_CSTRING(text));
		return NONE_VAL();
	}

	return krk_runtimeError(vm.exceptions->typeError, "unexpected error");
}

#define IS_property(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(property)))
#define AS_property(o) (AS_INSTANCE(o))

struct Property {
	KrkInstance inst;
	KrkObj* fget;
	KrkObj* fset;
};

static void _property_gcscan(KrkInstance *_self) {
	struct Property * self = (struct Property*)_self;
	if (self->fget) krk_markObject(self->fget);
	if (self->fset) krk_markObject(self->fset);
}

KRK_Method(property,__init__) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2); /* TODO fdel */
	krk_attachNamedValue(&self->fields, "fget", argv[1]);

	((struct Property*)self)->fget = IS_OBJECT(argv[1]) ? AS_OBJECT(argv[1]) : NULL;

	/* Try to attach doc */
	if (IS_NATIVE(argv[1])) {
		krk_attachNamedValue(&self->fields, "__doc__",
			AS_NATIVE(argv[1])->doc ? OBJECT_VAL(krk_copyString(AS_NATIVE(argv[1])->doc, strlen(AS_NATIVE(argv[1])->doc))) : NONE_VAL());
	} else if (IS_CLOSURE(argv[1])) {
		krk_attachNamedValue(&self->fields, "__doc__",
			AS_CLOSURE(argv[1])->function->docstring ? OBJECT_VAL(AS_CLOSURE(argv[1])->function->docstring) : NONE_VAL());
	}

	/* Try to attach name */
	if (IS_NATIVE(argv[1])) {
		krk_attachNamedValue(&self->fields, "__name__",
			 AS_NATIVE(argv[1])->name ? OBJECT_VAL(krk_copyString(AS_NATIVE(argv[1])->name, strlen(AS_NATIVE(argv[1])->name))) : NONE_VAL());
	} else if (IS_CLOSURE(argv[1])) {
		krk_attachNamedValue(&self->fields, "__name__",
			 AS_CLOSURE(argv[1])->function->name ? OBJECT_VAL(AS_CLOSURE(argv[1])->function->name) : NONE_VAL());
	}

	if (argc > 2) {
		krk_attachNamedValue(&self->fields, "fset", argv[2]);
		((struct Property*)self)->fset = IS_OBJECT(argv[2]) ? AS_OBJECT(argv[2]) : NULL;
	}

	return NONE_VAL();
}

KRK_Method(property,setter) {
	METHOD_TAKES_EXACTLY(1);
	krk_attachNamedValue(&self->fields, "fset", argv[1]);
	return argv[0]; /* Return the original property */
}

KRK_Method(property,__get__) {
	METHOD_TAKES_AT_LEAST(1); /* the owner */

	if (IS_NONE(argv[1])) return argv[0];

	struct Property * asProp = (struct Property *)self;

	if (asProp->fget) {
		krk_push(argv[1]);
		return krk_callDirect(asProp->fget, 1);
	}

	KrkValue fget;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("fget")), &fget))
		return krk_runtimeError(vm.exceptions->attributeError, "'%T' object has no attribute '%s'", argv[0], "fget");

	krk_push(fget);
	krk_push(argv[1]);
	return krk_callStack(1);
}

KRK_Method(property,__set__) {
	METHOD_TAKES_EXACTLY(2); /* the owner and the value */

	struct Property * asProp = (struct Property *)self;

	if (asProp->fset) {
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callDirect(asProp->fset, 2);
	}

	KrkValue fset;
	if (krk_tableGet(&self->fields, OBJECT_VAL(S("fset")), &fset)) {
		krk_push(fset);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callStack(2);
	}

	if (asProp->fget) {
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callDirect(asProp->fget, 2);
	}

	KrkValue fget;
	if (krk_tableGet(&self->fields, OBJECT_VAL(S("fget")), &fget)) {
		krk_push(fget);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callStack(2);
	}

	return krk_runtimeError(vm.exceptions->attributeError, "attribute can not be set");
}

KRK_Method(property,__setattr__) {
	METHOD_TAKES_EXACTLY(2);
	if (!IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
	krk_instanceSetAttribute_wrapper(argv[0], AS_STRING(argv[1]), argv[2]);
	struct Property * asProp = (struct Property *)self;

	KrkValue fget = NONE_VAL();
	krk_tableGet(&self->fields, OBJECT_VAL(S("fget")), &fget);
	asProp->fget = (IS_CLOSURE(fget) || IS_NATIVE(fget)) ? AS_OBJECT(fget) : NULL;

	KrkValue fset = NONE_VAL();
	krk_tableGet(&self->fields, OBJECT_VAL(S("fset")), &fset);
	asProp->fset = (IS_CLOSURE(fset) || IS_NATIVE(fset)) ? AS_OBJECT(fset) : NULL;

	return argv[2];
}

/**
 * Create a new property object that calls a C function; same semantics as defineNative, but
 * instead of applying the function directly it is applied as a property value, so it should
 * be used with the "fields" table rather than the methods table. This will eventually replace
 * the ":field" option for defineNative().
 */
KrkNative * krk_defineNativeProperty(KrkTable * table, const char * name, NativeFn function) {
	KrkNative * func = krk_newNative(function, name, 0);
	krk_push(OBJECT_VAL(func));
	KrkInstance * property = krk_newInstance(vm.baseClasses->propertyClass);
	krk_attachNamedObject(table, name, (KrkObj*)property);
	krk_attachNamedObject(&property->fields, "fget", (KrkObj*)func);
	krk_attachNamedObject(&property->fields, "fset", (KrkObj*)func);
	((struct Property*)property)->fget = (KrkObj*)func;
	((struct Property*)property)->fset = (KrkObj*)func;
	krk_pop();
	return func;
}

KRK_Function(id) {
	FUNCTION_TAKES_EXACTLY(1);
	if (!IS_OBJECT(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "'%T' has no identity", argv[0]);
	return INTEGER_VAL((size_t)AS_OBJECT(argv[0]));
}

KRK_Function(hash) {
	FUNCTION_TAKES_EXACTLY(1);
	uint32_t hashed;
	if (krk_hashValue(argv[0], &hashed)) return NONE_VAL();
	return INTEGER_VAL(hashed);
}

KRK_Function(next) {
	FUNCTION_TAKES_EXACTLY(1);
	krk_push(argv[0]);
	return krk_callStack(0);
}

KRK_Function(abs) {
	FUNCTION_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[0])) {
		krk_integer_type i = AS_INTEGER(argv[0]);
		return INTEGER_VAL(i >= 0 ? i : -i);
#ifndef KRK_NO_FLOAT
	} else if (IS_FLOATING(argv[0])) {
		double i = AS_FLOATING(argv[0]);
		return FLOATING_VAL(i >= 0 ? i : -i);
#endif
	} else {
		trySlowMethod(vm.specialMethodNames[METHOD_ABS]);
		return krk_runtimeError(vm.exceptions->typeError, "bad operand type for 'abs()': '%T'", argv[0]);
	}
}

KRK_Function(format) {
	FUNCTION_TAKES_AT_LEAST(1);
	FUNCTION_TAKES_AT_MOST(2);

	KrkClass * type = krk_getType(argv[0]);

	if (!type->_format) {
		return krk_runtimeError(vm.exceptions->typeError, "'%T' has no __format__ method", argv[0]);
	}

	krk_push(argv[0]);
	if (argc < 2) krk_push(OBJECT_VAL(S("")));
	else krk_push(argv[1]);

	KrkValue result = krk_callDirect(type->_format, 2);
	if (!IS_STRING(result)) {
		return krk_runtimeError(vm.exceptions->typeError, "__format__ result was not a string");
	}
	return result;
}

static void module_sweep(KrkInstance * inst) {
#ifndef KRK_STATIC_ONLY
	struct KrkModule * module = (struct KrkModule*)inst;
	if (module->libHandle) {
		krk_dlClose(module->libHandle);
	}
#endif
}

KRK_Function(__build_class__) {
	KrkValue func = NONE_VAL();
	KrkString * name = NULL;
	KrkClass * base = vm.baseClasses->objectClass;
	KrkValue metaclass = OBJECT_VAL(vm.baseClasses->typeClass);

	if (!krk_parseArgs("VO!|O!$V~",
		(const char*[]){"func","name","base","metaclass"},
		&func,
		vm.baseClasses->strClass, &name,
		vm.baseClasses->typeClass, &base,
		&metaclass)) {
		return NONE_VAL();
	}

	if (IS_CLASS(metaclass)) {
		KrkClass * basemeta = base->_class ? base->_class : vm.baseClasses->typeClass;
		if (krk_isSubClass(AS_CLASS(metaclass), basemeta)) {
			/* good to go */
		} else if (krk_isSubClass(basemeta, AS_CLASS(metaclass))) {
			/* take the more derived one */
			metaclass = OBJECT_VAL(basemeta);
		} else {
			return krk_runtimeError(vm.exceptions->typeError,
				"metaclass conflict: %S is not a subclass of %S", AS_CLASS(metaclass)->name, basemeta->name);
		}
	}

	/* Push function */
	krk_push(func);

	/* Call __prepare__ from metaclass */
	krk_push(krk_valueGetAttribute_default(metaclass, "__prepare__", KWARGS_VAL(0)));

	/* Bail early on exception */
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();

	if (IS_KWARGS(krk_peek(0))) {
		krk_pop();
		krk_push(krk_dict_of(0,NULL,0));
	} else {
		krk_push(OBJECT_VAL(name));
		krk_push(OBJECT_VAL(base));
		/* Do we have keywords? */
		int args = 2;
		if (hasKw) {
			args += 3;
			krk_push(KWARGS_VAL(KWARGS_DICT));
			krk_push(argv[argc]);
			krk_push(KWARGS_VAL(1));
		}
		krk_push(krk_callStack(args));
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
	}

	/* Run the class function on it */
	krk_push(krk_callStack(1));
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();

	/* Now call the metaclass with the name, base, namespace, and kwds */
	int args = 3;
	krk_push(OBJECT_VAL(name));
	krk_push(OBJECT_VAL(base));
	krk_push(metaclass);
	krk_swap(3);

	if (hasKw) {
		args += 3;
		krk_push(KWARGS_VAL(KWARGS_DICT));
		krk_push(argv[argc]);
		krk_push(KWARGS_VAL(1));
	}

	krk_push(krk_callStack(args));
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();

	/* Now assign the upvalue for the original function if it's a closure. */
	if (IS_CLOSURE(func)) {
		if (AS_CLOSURE(func)->upvalueCount && AS_CLOSURE(func)->upvalues[0]->location == -1 && IS_NONE(AS_CLOSURE(func)->upvalues[0]->closed)) {
			AS_CLOSURE(func)->upvalues[0]->closed = krk_peek(0);
		}
	}

	/* We're done, return the resulting class object. */
	return krk_pop();
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkUpvalue *
#define IS_Cell(o) (krk_isObjType((o), KRK_OBJ_UPVALUE))
#define AS_Cell(o) ((KrkUpvalue*)AS_OBJECT(o))

KRK_StaticMethod(Cell,__new__) {
	KrkClass * _class = NULL;
	KrkValue contents = NONE_VAL();
	if (!krk_parseArgs("O!|V:Cell", (const char*[]){"cls","contents"}, KRK_BASE_CLASS(type), &_class, &contents)) {
		return NONE_VAL();
	}
	if (_class != KRK_BASE_CLASS(Cell)) {
		return krk_runtimeError(vm.exceptions->typeError, "can not assemble new Cell from %R", OBJECT_VAL(_class));
	}

	KrkUpvalue * out = krk_newUpvalue(-1);
	out->closed = contents;
	return OBJECT_VAL(out);
}

#define UPVALUE_LOCATION(upvalue) (upvalue->location == -1 ? &upvalue->closed : &upvalue->owner->stack[upvalue->location])
KRK_Method(Cell,__repr__) {
	struct StringBuilder sb = {0};

	KrkValue contents = *UPVALUE_LOCATION(self);

	if (!krk_pushStringBuilderFormat(&sb,"<cell at %p: %T object", (void*)self, contents)) goto _error;
	if (IS_OBJECT(contents)) {
		if (!krk_pushStringBuilderFormat(&sb, " at %p>", (void*)AS_OBJECT(contents))) goto _error;
	} else {
		krk_pushStringBuilder(&sb,'>');
	}

	return krk_finishStringBuilder(&sb);

_error:
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

KRK_Method(Cell,cell_contents) {
	if (argc > 1) {
		*UPVALUE_LOCATION(self) = argv[1];
	}
	return *UPVALUE_LOCATION(self);
}

_noexport
void _createAndBind_builtins(void) {
	vm.baseClasses->objectClass = krk_newClass(S("object"), NULL);
	krk_push(OBJECT_VAL(vm.baseClasses->objectClass));

	KrkClass * object = vm.baseClasses->objectClass;
	BIND_METHOD(object,__dir__);
	BIND_METHOD(object,__str__);
	BIND_METHOD(object,__repr__);
	BIND_METHOD(object,__hash__);
	BIND_METHOD(object,__eq__);
	BIND_METHOD(object,__format__);
	BIND_STATICMETHOD(object,__setattr__);
	BIND_STATICMETHOD(object,__new__);
	BIND_METHOD(object,__init__);
	BIND_CLASSMETHOD(object,__init_subclass__);
	krk_finalizeClass(object);
	KRK_DOC(object,
		"@brief Base class for all types.\n\n"
		"The @c object base class provides the fallback implementations of methods like "
		"@ref object___dir__ \"__dir__\". All object and primitive types eventually inherit from @c object."
	);

	vm.baseClasses->moduleClass = krk_newClass(S("module"), vm.baseClasses->objectClass);

	KrkClass * module = vm.baseClasses->moduleClass;
	module->allocSize = sizeof(struct KrkModule);
	module->_ongcsweep = module_sweep;

	krk_push(OBJECT_VAL(module));

	BIND_METHOD(module,__repr__);
	krk_finalizeClass(module);
	KRK_DOC(module, "Type of imported modules and packages.");

	vm.builtins = krk_newInstance(module);
	krk_attachNamedObject(&vm.modules, "builtins", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&vm.builtins->fields, "object", (KrkObj*)vm.baseClasses->objectClass);
	krk_pop();
	krk_pop();

	krk_attachNamedObject(&vm.builtins->fields, "__name__", (KrkObj*)S("builtins"));
	krk_attachNamedValue(&vm.builtins->fields, "__file__", NONE_VAL());
	KRK_DOC(vm.builtins,
		"@brief Internal module containing built-in functions and classes.\n\n"
		"Classes and functions from the @c \\__builtins__ module are generally available from "
		"all global namespaces. Built-in names can still be shadowed by module-level globals "
		"and function-level locals, so none the names in this module are reserved. When "
		"a built-in name has been shadowed, the original can be referenced directly as "
		" @c \\__builtins__.name instead.\n\n"
		"Built-in names may be bound from several sources. Most come from the core interpreter "
		"directly, but some may come from loaded C extension modules or the interpreter binary. "
		"Kuroko source modules are also free to append new names to the built-in name space by "
		"attaching new properties to the @c \\__builtins__ instance."
	);

	KrkClass * property = ADD_BASE_CLASS(KRK_BASE_CLASS(property), "property", object);
	property->allocSize = sizeof(struct Property);
	property->_ongcscan = _property_gcscan;
	KRK_DOC(BIND_METHOD(property,__init__),
		"@brief Create a property object.\n"
		"@arguments fget,[fset]\n\n"
		"When a property object is obtained from an instance of the class in which it is defined, "
		"the function or method assigned to @p fget is called with the instance as an argument. "
		"If @p fset is provided, it will be called with the instance and a value when the property "
		"object is assigned to through an instance. For legacy compatibility reasons, a property "
		"object's @p fget method may also accept an additional argument to act as a setter if "
		"@p fset is not provided, but this functionality may be removed in the future.\n\n"
		"The typical use for @c property is as a decorator on methods in a class. See also "
		"@ref property_setter \"property.setter\" for the newer Python-style approach to decorating a companion "
		"setter method.");
	BIND_METHOD(property,__get__);
	BIND_METHOD(property,__set__);
	KRK_DOC(BIND_METHOD(property,setter),
		"@brief Assign the setter method of a property object.\n"
		"@arguments fset\n\n"
		"This should be used as a decorator from an existing property object as follows:\n\n"
		"```\n"
		"class Foo():\n"
		"    @property\n"
		"    def bar(self):\n"
		"        return 42\n"
		"    @bar.setter\n"
		"    def bar(self, val):\n"
		"        print('setting bar to',val)\n"
		"```\n"
		"Be sure to apply the decorator to a function or method with the same name, as this "
		"name will be used to assign the property to the class's attribute table; using a "
		"different name will create a duplicate alias.");
	krk_finalizeClass(property);

	/* Need to do this after creating 'property' */
	BIND_PROP(object,__class__);

	KrkClass * Helper = ADD_BASE_CLASS(KRK_BASE_CLASS(Helper), "Helper", object);
	KRK_DOC(Helper,
		"@brief Special object that prints a helpeful message.\n\n"
		"Object that prints help summary when passed to @ref repr.");
	KRK_DOC(BIND_METHOD(Helper,__call__),
		"@brief Prints help text.\n"
		"@arguments obj=None\n\n"
		"Prints the help documentation attached to @p obj or starts the interactive help system by "
		"importing the @ref mod_help module.");
	BIND_METHOD(Helper,__repr__);
	krk_finalizeClass(Helper);
	krk_attachNamedObject(&vm.builtins->fields, "help", (KrkObj*)krk_newInstance(Helper));

	KrkClass * LicenseReader = ADD_BASE_CLASS(KRK_BASE_CLASS(LicenseReader), "LicenseReader", object);
	KRK_DOC(LicenseReader, "Special object that prints Kuroko's copyright information when passed to @ref repr");
	KRK_DOC(BIND_METHOD(LicenseReader,__call__), "Print the full license statement.");
	BIND_METHOD(LicenseReader,__repr__);
	krk_finalizeClass(LicenseReader);
	krk_attachNamedObject(&vm.builtins->fields, "license", (KrkObj*)krk_newInstance(LicenseReader));

	KrkClass * map = ADD_BASE_CLASS(KRK_BASE_CLASS(map), "map", object);
	KRK_DOC(map, "Return an iterator that applies a function to a series of iterables");
	BIND_METHOD(map,__init__);
	BIND_METHOD(map,__iter__);
	BIND_METHOD(map,__call__);
	krk_finalizeClass(map);

	KrkClass * zip = ADD_BASE_CLASS(KRK_BASE_CLASS(zip), "zip", object);
	KRK_DOC(zip,
		"@brief Returns an iterator that produces tuples of the nth element of each passed iterable.\n"
		"@arguments *iterables\n\n"
		"Creates an iterator that returns a tuple of elements from each of @p iterables, until one "
		"of @p iterables is exhuasted.");
	BIND_METHOD(zip,__init__);
	BIND_METHOD(zip,__iter__);
	BIND_METHOD(zip,__call__);
	krk_finalizeClass(zip);

	KrkClass * filter = ADD_BASE_CLASS(KRK_BASE_CLASS(filter), "filter", object);
	KRK_DOC(filter, "Return an iterator that returns only the items from an iterable for which the given function returns true.");
	BIND_METHOD(filter,__init__);
	BIND_METHOD(filter,__iter__);
	BIND_METHOD(filter,__call__);
	krk_finalizeClass(filter);

	KrkClass * enumerate = ADD_BASE_CLASS(KRK_BASE_CLASS(enumerate), "enumerate", object);
	KRK_DOC(enumerate, "Return an iterator that produces a tuple with a count the iterated values of the passed iteratable.");
	BIND_METHOD(enumerate,__init__);
	BIND_METHOD(enumerate,__iter__);
	BIND_METHOD(enumerate,__call__);
	krk_finalizeClass(enumerate);

	KrkClass * Cell = ADD_BASE_CLASS(KRK_BASE_CLASS(Cell), "Cell", object);
	Cell->allocSize = 0;
	Cell->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_STATICMETHOD(Cell,__new__);
	BIND_METHOD(Cell,__repr__);
	BIND_PROP(Cell,cell_contents);
	krk_finalizeClass(Cell);

	BUILTIN_FUNCTION("isinstance", FUNC_NAME(krk,isinstance),
		"@brief Check if an object is an instance of a type.\n"
		"@arguments inst, cls\n\n"
		"Determine if an object @p inst is an instance of the given class @p cls or one if its subclasses. "
		"@p cls may be a single class or a tuple of classes.");
	BUILTIN_FUNCTION("issubclass", FUNC_NAME(krk,issubclass),
		"@brief Check if a class is a subclass of a type.\n"
		"@arguments cls, clsinfo\n\n"
		"Determine if the class @p cls is a subclass of the class @p clsinfo. @p clsinfo may be a single "
		"class or a tuple of classes.");
	BUILTIN_FUNCTION("globals", FUNC_NAME(krk,globals),
		"@brief Update and a return a mapping of names in the global namespace.\n\n"
		"Produces a dict mapping all of the names of the current globals namespace to their values. "
		"Updating this dict has no meaning, but modifying mutable values within it can affect the global namespace.");
	BUILTIN_FUNCTION("locals", FUNC_NAME(krk,locals),
		"@brief Update and return a mapping of names in the current local scope.\n"
		"@arguments callDepth=1\n\n"
		"Produces a dict mapping the names of the requested locals scope to their current stack values. "
		"If @p callDepth is provided, the locals of an outer call frame will be returned. If the requested "
		"call depth is out of range, an exception will be raised.");
	BUILTIN_FUNCTION("dir", FUNC_NAME(krk,dir),
		"@brief Return a list of known property names for a given object.\n"
		"@arguments [obj]\n\n"
		"Uses various internal methods to collect a list of property names of @p obj, returning "
		"that list sorted lexicographically. If no argument is given, the returned list will "
		"be the valid global names in the calling scope.");
	BUILTIN_FUNCTION("len", FUNC_NAME(krk,len),
		"@brief Return the length of a given sequence object.\n"
		"@arguments seq\n\n"
		"Returns the length of the sequence object @p seq, which must implement @c __len__.");
	BUILTIN_FUNCTION("repr", FUNC_NAME(krk,repr),
		"@brief Produce a string representation of the given object.\n"
		"@arguments val\n\n"
		"Return a string representation of the given object through its @c __repr__ method. "
		"@c repr strings should convey all information needed to recreate the object, if this is possible.");
	BUILTIN_FUNCTION("print", FUNC_NAME(krk,print),
		"@brief Print text to the standard output.\n"
		"@arguments *args,sep=' ',end='\\n',file=None,flush=False\n\n"
		"Prints the string representation of each argument to the standard output. "
		"The keyword argument @p sep specifies the string to print between values. "
		"The keyword argument @p end specifies the string to print after all of the values have been printed.");
	BUILTIN_FUNCTION("ord", FUNC_NAME(krk,ord),
		"@brief Obtain the ordinal integer value of a codepoint or byte.\n"
		"@arguments char\n\n"
		"Returns the integer codepoint value of a single-character string @p char.");
	BUILTIN_FUNCTION("chr", FUNC_NAME(krk,chr),
		"@brief Convert an integer codepoint to its string representation.\n"
		"@arguments codepoint\n\n"
		"Creates a single-codepoint string with the character represented by the integer codepoint @p codepoint.");
	BUILTIN_FUNCTION("hex", FUNC_NAME(krk,hex),
		"@brief Convert an integer value to a hexadecimal string.\n"
		"@arguments i\n\n"
		"Returns a string representation of @p i in hexadecimal, with a leading @c 0x.");
	BUILTIN_FUNCTION("oct", FUNC_NAME(krk,oct),
		"@brief Convert an integer value to an octal string.\n"
		"@arguments i\n\n"
		"Returns a string representation of @p i in octal, with a leading @c 0o.");
	BUILTIN_FUNCTION("any", FUNC_NAME(krk,any),
		"@brief Returns True if at least one element in the given iterable is truthy, False otherwise.\n"
		"@arguments iterable");
	BUILTIN_FUNCTION("all", FUNC_NAME(krk,all),
		"@brief Returns True if every element in the given iterable is truthy, False otherwise.\n"
		"@arguments iterable");
	BUILTIN_FUNCTION("getattr", FUNC_NAME(krk,getattr),
		"@brief Perform attribute lookup on an object using a string.\n"
		"@arguments obj,attribute,[default]\n\n"
		"Obtains the attributed named @p attribute from the object @p obj, if such an "
		"attribute exists. Attribute lookup ordering is complex and includes direct "
		"attribute tables of instances, dynamic attributes from classes, and so on. "
		"The use of @c getattr is equivalent to a dotted access. If @p attribute refers "
		"to a method of @p obj's class, a bound method will be obtained. If @p default "
		"is provided then the value supplied will be returned in the case where @p obj "
		"does not have an attribute named @p attribute, otherwise an @ref AttributeError "
		"will be raised.");
	BUILTIN_FUNCTION("setattr", FUNC_NAME(krk,setattr),
		"@brief Set an attribute of an object using a string name.\n"
		"@arguments obj,attribute,value\n\n"
		"Sets the attribute named by @p attribute of the object @p obj to @p value. "
		"If @p attribute refers to a @ref property object or other descriptor, the "
		"descriptor's @c \\__set__ method will be called. If @p obj is a class, instance, "
		"or other type with its own attribute table, then the field will be updated. If "
		"@p obj is a type without an attribute table and no class property provides an "
		"overriding setter for @p attribute, an @ref AttributeError will be raised.");
	BUILTIN_FUNCTION("hasattr", FUNC_NAME(krk,hasattr),
		"@brief Determines if an object has an attribute.\n"
		"@arguments obj,attribute\n\n"
		"Uses @ref getattr to determine if @p obj has an attribute named @p attribute.");
	BUILTIN_FUNCTION("delattr", FUNC_NAME(krk,delattr),
		"@brief Delete an attribute by name.\n"
		"@arguments obj,attribute\n\n"
		"Deletes the attribute @p attribute from @p obj.");
	BUILTIN_FUNCTION("sum", FUNC_NAME(krk,sum),
		"@brief add the elements of an iterable.\n"
		"@arguments iterable,start=0\n\n"
		"Continuously adds all of the elements from @p iterable to @p start and returns the result "
		"when @p iterable has been exhausted.");
	BUILTIN_FUNCTION("min", FUNC_NAME(krk,min),
		"@brief Return the lowest value in an iterable or the passed arguments.\n"
		"@arguments iterable");
	BUILTIN_FUNCTION("max", FUNC_NAME(krk,max),
		"@brief Return the highest value in an iterable or the passed arguments.\n"
		"@arguments iterable");
	BUILTIN_FUNCTION("id", FUNC_NAME(krk,id),
		"@brief Returns the identity of an object.\n"
		"@arguments val\n\n"
		"Returns the internal identity for @p val. Note that not all objects have "
		"identities; primitive values such as @c int or @c float do not have identities. "
		"Internally, this is the pointer value for a heap object, but this is an implementation detail.");
	BUILTIN_FUNCTION("hash", FUNC_NAME(krk,hash),
		"@brief Returns the hash of a value, used for table indexing.\n"
		"@arguments val\n\n"
		"If @p val is hashable, its hash value will be calculated if necessary and returned. "
		"If @p val is not hashable, @ref TypeError will be raised.");
	BUILTIN_FUNCTION("bin", FUNC_NAME(krk,bin),
		"@brief Convert an integer value to a binary string.\n"
		"@arguments i\n\n"
		"Produces a string representation of @p i in binary, with a leading @p 0b.");
	BUILTIN_FUNCTION("next", FUNC_NAME(krk,next),
		"@brief Compatibility function. Calls an iterable.\n"
		"@arguments iterable");
	BUILTIN_FUNCTION("abs", FUNC_NAME(krk,abs),
		"@brief Obtain the absolute value of a numeric.\n"
		"@arguments iterable");
	BUILTIN_FUNCTION("format", FUNC_NAME(krk,format),
		"@brief Format a value for string printing.\n"
		"@arguments value[,format_spec]");
	BUILTIN_FUNCTION("__build_class__", FUNC_NAME(krk,__build_class__),
		"@brief Internal function to build a type object.\n"
		"@arguments func, name, base=object, metaclass=type");
}

