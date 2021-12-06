#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/debug.h>

static KrkClass * Helper;
static KrkClass * LicenseReader;
static KrkClass * property;

FUNC_SIG(list,__init__);
FUNC_SIG(list,sort);

KrkValue krk_dirObject(int argc, KrkValue argv[], int hasKw) {
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


KRK_FUNC(len,{
	FUNCTION_TAKES_EXACTLY(1);
	/* Shortcuts */
	if (IS_STRING(argv[0])) return INTEGER_VAL(AS_STRING(argv[0])->codesLength);
	if (IS_TUPLE(argv[0])) return INTEGER_VAL(AS_TUPLE(argv[0])->values.count);

	KrkClass * type = krk_getType(argv[0]);
	if (!type->_len) return krk_runtimeError(vm.exceptions->typeError, "object of type '%s' has no len()", krk_typeName(argv[0]));
	krk_push(argv[0]);

	return krk_callDirect(type->_len, 1);
})

KRK_FUNC(dir,{
	FUNCTION_TAKES_EXACTLY(1);
	KrkClass * type = krk_getType(argv[0]);
	if (!type->_dir) {
		return krk_dirObject(argc,argv,hasKw); /* Fallback */
	}
	krk_push(argv[0]);
	return krk_callDirect(type->_dir, 1);
})

KRK_FUNC(repr,{
	FUNCTION_TAKES_EXACTLY(1);
	/* Everything should have a __repr__ */
	KrkClass * type = krk_getType(argv[0]);
	krk_push(argv[0]);
	return krk_callDirect(type->_reprer, 1);
})

KRK_FUNC(ord,{
	FUNCTION_TAKES_EXACTLY(1);
	KrkClass * type = krk_getType(argv[0]);
	KrkValue method;
	if (krk_tableGet(&type->methods, vm.specialMethodNames[METHOD_ORD], &method)) {
		krk_push(method);
		krk_push(argv[0]);
		return krk_callStack(1);
	}
	return TYPE_ERROR(string of length 1,argv[0]);
})

KRK_FUNC(chr,{
	FUNCTION_TAKES_EXACTLY(1);
	KrkClass * type = krk_getType(argv[0]);
	KrkValue method;
	if (krk_tableGet(&type->methods, vm.specialMethodNames[METHOD_CHR], &method)) {
		krk_push(method);
		krk_push(argv[0]);
		return krk_callStack(1);
	}
	return TYPE_ERROR(int,argv[0]);
})

KRK_FUNC(hex,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,x);
	char tmp[20];
	size_t len = snprintf(tmp, 20, "%s0x" PRIkrk_hex, x < 0 ? "-" : "", x < 0 ? -x : x);
	return OBJECT_VAL(krk_copyString(tmp,len));
})

KRK_FUNC(oct,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,x);
	char tmp[20];
	size_t len = snprintf(tmp, 20, "%s0o%llo", x < 0 ? "-" : "", x < 0 ? (long long int)-x : (long long int)x);
	return OBJECT_VAL(krk_copyString(tmp,len));
})

KRK_FUNC(bin,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,int,krk_integer_type,val);

	krk_integer_type original = val;
	if (val < 0) val = -val;

	struct StringBuilder sb = {0};

	if (!val) pushStringBuilder(&sb, '0');
	while (val) {
		pushStringBuilder(&sb, (val & 1) ? '1' : '0');
		val = val >> 1;
	}

	pushStringBuilder(&sb, 'b');
	pushStringBuilder(&sb, '0');
	if (original < 0) pushStringBuilder(&sb,'-');

	/* Flip it */
	for (size_t i = 0; i < sb.length / 2; ++i) {
		char t = sb.bytes[i];
		sb.bytes[i] = sb.bytes[sb.length - i - 1];
		sb.bytes[sb.length - i - 1] = t;
	}

	return finishStringBuilder(&sb);
})

#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (!krk_isFalsey(indexer)) { \
			if (unpackingIterable) { krk_pop(); krk_pop(); } \
			return BOOLEAN_VAL(1); \
		} \
	} \
} while (0)
KRK_FUNC(any,{
	FUNCTION_TAKES_EXACTLY(1);
	unpackIterableFast(argv[0]);
	return BOOLEAN_VAL(0);
})
#undef unpackArray

#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (krk_isFalsey(indexer)) { \
			if (unpackingIterable) { krk_pop(); krk_pop(); } \
			return BOOLEAN_VAL(0); \
		} \
	} \
} while (0)
KRK_FUNC(all,{
	FUNCTION_TAKES_EXACTLY(1);
	unpackIterableFast(argv[0]);
	return BOOLEAN_VAL(1);
})
#undef unpackArray

#define CURRENT_CTYPE KrkInstance *
#define CURRENT_NAME  self

#define IS_map(o) (krk_isInstanceOf(o,map))
#define AS_map(o) (AS_INSTANCE(o))
static KrkClass * map;
KRK_METHOD(map,__init__,{
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
			return krk_runtimeError(vm.exceptions->typeError, "'%s' object is not iterable", krk_typeName(argv[i]));
		}
		krk_push(argv[i]);
		KrkValue asIter = krk_callDirect(type->_iter, 1);
		if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
		/* Attach it to the tuple */
		iters->values.values[iters->values.count++] = asIter;
	}

	return argv[0];
})

KRK_METHOD(map,__iter__,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
})

KRK_METHOD(map,__call__,{
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
})

KRK_FUNC(zip,{
	if (!argc) return NONE_VAL(); /* uh, new empty tuple maybe? */
	KrkValue map = NONE_VAL();
	KrkValue tupleOfFunc = NONE_VAL();

	krk_tableGet(&vm.builtins->fields, OBJECT_VAL(S("map")), &map);
	krk_tableGet(&vm.builtins->fields, OBJECT_VAL(S("tupleOf")), &tupleOfFunc);

	krk_push(map);
	krk_push(tupleOfFunc);
	for (int i = 0; i < argc; ++i) {
		krk_push(argv[i]);
	}

	return krk_callStack(argc+1);
})

#define IS_filter(o) (krk_isInstanceOf(o,filter))
#define AS_filter(o) (AS_INSTANCE(o))
static KrkClass * filter;
KRK_METHOD(filter,__init__,{
	METHOD_TAKES_EXACTLY(2);
	krk_attachNamedValue(&self->fields, "_function", argv[1]);
	KrkClass * type = krk_getType(argv[2]);
	if (!type->_iter) {
		return krk_runtimeError(vm.exceptions->typeError, "'%s' object is not iterable", krk_typeName(argv[2]));
	}
	krk_push(argv[2]);
	KrkValue asIter = krk_callDirect(type->_iter, 1);
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
	krk_attachNamedValue(&self->fields, "_iterator", asIter);
	return argv[0];
})

KRK_METHOD(filter,__iter__,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
})

KRK_METHOD(filter,__call__,{
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
})

#define IS_enumerate(o) (krk_isInstanceOf(o,enumerate))
#define AS_enumerate(o) (AS_INSTANCE(o))
static KrkClass * enumerate;
KRK_METHOD(enumerate,__init__,{
	METHOD_TAKES_EXACTLY(1);
	KrkValue start = INTEGER_VAL(0);
	if (hasKw) krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("start")), &start);

	krk_attachNamedValue(&self->fields, "_counter", start);

	/* Attach iterator */
	KrkClass * type = krk_getType(argv[1]);
	if (!type->_iter) {
		return krk_runtimeError(vm.exceptions->typeError, "'%s' object is not iterable", krk_typeName(argv[1]));
	}
	krk_push(argv[1]);
	KrkValue asIter = krk_callDirect(type->_iter, 1);
	if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) return NONE_VAL();
	krk_attachNamedValue(&self->fields, "_iterator", asIter);

	return argv[0];
})

KRK_METHOD(enumerate,__iter__,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(self);
})

extern KrkValue krk_operator_add (KrkValue a, KrkValue b);
KRK_METHOD(enumerate,__call__,{
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
})

#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		base = krk_operator_add(base, indexer); \
	} \
} while (0)
KRK_FUNC(sum,{
	FUNCTION_TAKES_AT_LEAST(1);
	KrkValue base = INTEGER_VAL(0);
	if (hasKw) {
		krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("start")), &base);
	}
	unpackIterableFast(argv[0]);
	return base;
})
#undef unpackArray

extern KrkValue krk_operator_lt(KrkValue a, KrkValue b);
#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (IS_KWARGS(base)) base = indexer; \
		else { \
			KrkValue check = krk_operator_lt(indexer, base); \
			if (!IS_BOOLEAN(check)) return NONE_VAL(); \
			else if (AS_BOOLEAN(check) == 1) base = indexer; \
		} \
	} \
} while (0)
KRK_FUNC(min,{
	FUNCTION_TAKES_AT_LEAST(1);
	KrkValue base = KWARGS_VAL(0);
	if (argc > 1) {
		unpackArray((size_t)argc,argv[i]);
	} else {
		unpackIterableFast(argv[0]);
	}
	if (IS_KWARGS(base)) return krk_runtimeError(vm.exceptions->valueError, "empty argument to %s()", "min");
	return base;
})
#undef unpackArray

extern KrkValue krk_operator_gt(KrkValue a, KrkValue b);
#define unpackArray(counter, indexer) do { \
	for (size_t i = 0; i < counter; ++i) { \
		if (IS_KWARGS(base)) base = indexer; \
		else { \
			KrkValue check = krk_operator_gt(indexer, base); \
			if (!IS_BOOLEAN(check)) return NONE_VAL(); \
			else if (AS_BOOLEAN(check) == 1) base = indexer; \
		} \
	} \
} while (0)
KRK_FUNC(max,{
	FUNCTION_TAKES_AT_LEAST(1);
	KrkValue base = KWARGS_VAL(0);
	if (argc > 1) {
		unpackArray((size_t)argc,argv[i]);
	} else {
		unpackIterableFast(argv[0]);
	}
	if (IS_KWARGS(base)) return krk_runtimeError(vm.exceptions->valueError, "empty argument to %s()", "max");
	return base;
})
#undef unpackArray

KRK_FUNC(print,{
	KrkValue sepVal;
	KrkValue endVal;
	char * sep = " "; size_t sepLen = 1;
	char * end = "\n"; size_t endLen = 1;
	if (hasKw) {
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("sep")), &sepVal)) {
			if (!IS_STRING(sepVal)) return krk_runtimeError(vm.exceptions->typeError, "'%s' should be a string, not '%s'", "sep", krk_typeName(sepVal));
			sep = AS_CSTRING(sepVal);
			sepLen = AS_STRING(sepVal)->length;
		}
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("end")), &endVal)) {
			if (!IS_STRING(endVal)) return krk_runtimeError(vm.exceptions->typeError, "'%s' should be a string, not '%s'", "end", krk_typeName(endVal));
			end = AS_CSTRING(endVal);
			endLen = AS_STRING(endVal)->length;
		}
	}
	if (!argc) {
		for (size_t j = 0; j < endLen; ++j) {
			fputc(end[j], stdout);
		}
	}
	for (int i = 0; i < argc; ++i) {
		KrkValue printable = argv[i];
		if (IS_STRING(printable)) { /* krk_printValue runs repr */
			/* Make sure we handle nil bits correctly. */
			for (size_t j = 0; j < AS_STRING(printable)->length; ++j) {
				fputc(AS_CSTRING(printable)[j], stdout);
			}
		} else {
			krk_printValue(stdout, printable);
		}
		char * thingToPrint = (i == argc - 1) ? end : sep;
		for (size_t j = 0; j < ((i == argc - 1) ? endLen : sepLen); ++j) {
			fputc(thingToPrint[j], stdout);
		}
	}
})

/**
 * globals()
 *
 * Returns a dict of names -> values for all the globals.
 */
KRK_FUNC(globals,{
	FUNCTION_TAKES_NONE();
	/* Make a new empty dict */
	KrkValue dict = krk_dict_of(0, NULL, 0);
	krk_push(dict);
	/* Copy the globals table into it */
	krk_tableAddAll(krk_currentThread.frames[krk_currentThread.frameCount-1].globals, AS_DICT(dict));
	krk_pop();

	return dict;
})

/**
 * locals()
 *
 * This is a bit trickier. Local names are... complicated. But we can do this!
 */
KRK_FUNC(locals,{
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
	for (short int i = 0; i < func->requiredArgs; ++i) {
		krk_tableSet(AS_DICT(dict),
			func->requiredArgNames.values[i],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	for (short int i = 0; i < func->keywordArgs; ++i) {
		krk_tableSet(AS_DICT(dict),
			func->keywordArgNames.values[i],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	if (func->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_ARGS) {
		krk_tableSet(AS_DICT(dict),
			func->requiredArgNames.values[func->requiredArgs],
			krk_currentThread.stack[frame->slots + slot]);
		slot++;
	}
	if (func->flags & KRK_CODEOBJECT_FLAGS_COLLECTS_KWS) {
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
})

KRK_FUNC(isinstance,{
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
})

static int _isSubClass(KrkClass * cls, KrkClass * base) {
	while (cls) {
		if (cls == base) return 1;
		cls = cls->base;
	}
	return 0;
}

KRK_FUNC(issubclass,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(0,class,KrkClass*,cls);
	if (IS_CLASS(argv[1])) {
		return BOOLEAN_VAL(_isSubClass(cls, AS_CLASS(argv[1])));
	} else if (IS_TUPLE(argv[1])) {
		for (size_t i = 0; i < AS_TUPLE(argv[1])->values.count; ++i) {
			if (IS_CLASS(AS_TUPLE(argv[1])->values.values[i]) && _isSubClass(cls, AS_CLASS(AS_TUPLE(argv[1])->values.values[i]))) {
				return BOOLEAN_VAL(1);
			}
		}
		return BOOLEAN_VAL(0);
	} else {
		return TYPE_ERROR(class or tuple,argv[1]);
	}
})

static KrkValue _module_repr(int argc, KrkValue argv[], int hasKw) {
	KrkInstance * self = AS_INSTANCE(argv[0]);

	KrkValue name = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_NAME], &name);

	if (!IS_STRING(name)) {
		return OBJECT_VAL(S("<module>"));
	}

	KrkValue file = NONE_VAL();
	krk_tableGet(&self->fields, vm.specialMethodNames[METHOD_FILE], &file);

	size_t allocSize = 50 + AS_STRING(name)->length + (IS_STRING(file) ? AS_STRING(file)->length : 20);
	char * tmp = malloc(allocSize);
	size_t len;
	if (IS_STRING(file)) {
		len = snprintf(tmp, allocSize, "<module '%s' from '%s'>", AS_CSTRING(name), AS_CSTRING(file));
	} else {
		len = snprintf(tmp, allocSize, "<module '%s' (built-in)>", AS_CSTRING(name));
	}

	KrkValue out = OBJECT_VAL(krk_copyString(tmp, len));
	free(tmp);
	return out;
}

static KrkValue obj_hash(int argc, KrkValue argv[], int hasKw) {
	KrkObj * self = AS_OBJECT(argv[0]);
	if (!(self->flags & KRK_OBJ_FLAGS_VALID_HASH)) {
		self->hash = INTEGER_VAL((int)(intptr_t)self);
		self->flags |= KRK_OBJ_FLAGS_VALID_HASH;
	}
	return INTEGER_VAL(self->hash);
}

static KrkValue obj_eq(int argc, KrkValue argv[], int hasKw) {
	return BOOLEAN_VAL(argc == 2 && IS_OBJECT(argv[0]) && IS_OBJECT(argv[1]) && AS_OBJECT(argv[0]) == AS_OBJECT(argv[1]));
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
static KrkValue _strBase(int argc, KrkValue argv[], int hasKw) {
	KrkClass * type = krk_getType(argv[0]);

	KrkValue module = NONE_VAL();
	krk_tableGet(&type->methods, OBJECT_VAL(S("__module__")), &module);
	KrkValue qualname = NONE_VAL();
	krk_tableGet(&type->methods, OBJECT_VAL(S("__qualname__")), &qualname);
	KrkString * name = IS_STRING(qualname) ? AS_STRING(qualname) : type->name;
	int includeModule = !(IS_NONE(module) || (IS_STRING(module) && AS_STRING(module) == S("__builtins__")));

	size_t allocSize = sizeof("<. object at 0x1234567812345678>") + name->length;
	if (includeModule) allocSize += AS_STRING(module)->length + 1;
	char * tmp = malloc(allocSize);
	size_t len;
	if (IS_OBJECT(argv[0])) {
		len = snprintf(tmp, allocSize, "<%s%s%s object at %p>",
			includeModule ? AS_CSTRING(module) : "",
			includeModule ? "." : "",
			name->chars,
			(void*)AS_OBJECT(argv[0]));
	} else {
		len = snprintf(tmp, allocSize, "<%s object>", name->chars);
	}
	KrkValue out = OBJECT_VAL(krk_copyString(tmp, len));
	free(tmp);
	return out;
}

KRK_FUNC(type,{
	FUNCTION_TAKES_EXACTLY(1);
	return OBJECT_VAL(krk_getType(argv[0]));
})

KRK_FUNC(getattr,{
	FUNCTION_TAKES_AT_LEAST(2);
	KrkValue object = argv[0];
	CHECK_ARG(1,str,KrkString*,property);
	if (argc == 3) {
		return krk_valueGetAttribute_default(object, property->chars, argv[2]);
	} else {
		return krk_valueGetAttribute(object, property->chars);
	}
})

KRK_FUNC(setattr,{
	FUNCTION_TAKES_EXACTLY(3);
	KrkValue object = argv[0];
	CHECK_ARG(1,str,KrkString*,property);
	KrkValue value = argv[2];
	return krk_valueSetAttribute(object, property->chars, value);
})

KRK_FUNC(hasattr,{
	FUNCTION_TAKES_AT_LEAST(2);
	KrkValue object = argv[0];
	CHECK_ARG(1,str,KrkString*,property);

	return BOOLEAN_VAL(!IS_KWARGS(krk_valueGetAttribute_default(object, property->chars, KWARGS_VAL(0))));
})

KRK_FUNC(delattr,{
	FUNCTION_TAKES_AT_LEAST(2);
	KrkValue object = argv[0];
	CHECK_ARG(1,str,KrkString*,property);

	return krk_valueDelAttribute(object, property->chars);
})


#define IS_Helper(o)  (krk_isInstanceOf(o, Helper))
#define AS_Helper(o)  (AS_INSTANCE(o))
#define IS_LicenseReader(o) (krk_isInstanceOf(o, LicenseReader))
#define AS_LicenseReader(o) (AS_INSTANCE(o))

KRK_METHOD(Helper,__repr__,{
	return OBJECT_VAL(S("Type help() for more help, or help(obj) to describe an object."));
})

KRK_METHOD(Helper,__call__,{
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
})

KRK_METHOD(LicenseReader,__repr__,{
	return OBJECT_VAL(S("Copyright 2020-2021 K. Lange <klange@toaruos.org>. Type `license()` for more information."));
})

KRK_METHOD(LicenseReader,__call__,{
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
})

#define IS_property(o) (krk_isInstanceOf(o,property))
#define AS_property(o) (AS_INSTANCE(o))
KRK_METHOD(property,__init__,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2); /* TODO fdel */
	krk_attachNamedValue(&self->fields, "fget", argv[1]);

	/* Try to attach doc */
	if (IS_NATIVE(argv[1]) && AS_NATIVE(argv[1])->doc) {
		krk_attachNamedValue(&self->fields, "__doc__",
			OBJECT_VAL(krk_copyString(AS_NATIVE(argv[1])->doc, strlen(AS_NATIVE(argv[1])->doc))));
	} else if (IS_CLOSURE(argv[1])) {
		krk_attachNamedValue(&self->fields, "__doc__",
			OBJECT_VAL(AS_CLOSURE(argv[1])->function->docstring));
	}

	/* Try to attach name */
	if (IS_NATIVE(argv[1]) && AS_NATIVE(argv[1])->name) {
		krk_attachNamedValue(&self->fields, "__name__",
			OBJECT_VAL(krk_copyString(AS_NATIVE(argv[1])->name, strlen(AS_NATIVE(argv[1])->name))));
	} else if (IS_CLOSURE(argv[1])) {
		krk_attachNamedValue(&self->fields, "__name__",
			OBJECT_VAL(AS_CLOSURE(argv[1])->function->name));
	}

	if (argc > 2)
		krk_attachNamedValue(&self->fields, "fset", argv[2]);

	return argv[0];
})

KRK_METHOD(property,setter,{
	METHOD_TAKES_EXACTLY(1);
	krk_attachNamedValue(&self->fields, "fset", argv[1]);
	return argv[0]; /* Return the original property */
})

KRK_METHOD(property,__get__,{
	METHOD_TAKES_EXACTLY(1); /* the owner */

	KrkValue fget;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("fget")), &fget))
		return krk_runtimeError(vm.exceptions->attributeError, "'%s' object has no attribute '%s'", "property", "fget");

	krk_push(fget);
	krk_push(argv[1]);
	return krk_callStack(1);
})

KRK_METHOD(property,__set__,{
	METHOD_TAKES_EXACTLY(2); /* the owner and the value */

	KrkValue fset;
	if (krk_tableGet(&self->fields, OBJECT_VAL(S("fset")), &fset)) {
		krk_push(fset);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callStack(2);
	}

	KrkValue fget;
	if (krk_tableGet(&self->fields, OBJECT_VAL(S("fget")), &fget)) {
		krk_push(fget);
		krk_push(argv[1]);
		krk_push(argv[2]);
		return krk_callStack(2);
	}

	return krk_runtimeError(vm.exceptions->attributeError, "attribute can not be set");
})

KRK_FUNC(id,{
	FUNCTION_TAKES_EXACTLY(1);
	if (!IS_OBJECT(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "'%s' has no identity", krk_typeName(argv[0]));
	return INTEGER_VAL((size_t)AS_OBJECT(argv[0]));
})

KRK_FUNC(hash,{
	FUNCTION_TAKES_EXACTLY(1);
	uint32_t hashed;
	if (krk_hashValue(argv[0], &hashed)) return NONE_VAL();
	return INTEGER_VAL(hashed);
})

KRK_FUNC(next,{
	FUNCTION_TAKES_EXACTLY(1);
	krk_push(argv[0]);
	return krk_callStack(0);
})

#ifndef STATIC_ONLY
static void module_sweep(KrkInstance * inst) {
	struct KrkModule * module = (struct KrkModule*)inst;
	if (module->libHandle) {
		dlClose(module->libHandle);
	}
}
#endif

_noexport
void _createAndBind_builtins(void) {
	vm.baseClasses->objectClass = krk_newClass(S("object"), NULL);
	krk_push(OBJECT_VAL(vm.baseClasses->objectClass));

	krk_defineNative(&vm.baseClasses->objectClass->methods, "__class__", FUNC_NAME(krk,type))->flags = KRK_NATIVE_FLAGS_IS_DYNAMIC_PROPERTY;
	krk_defineNative(&vm.baseClasses->objectClass->methods, "__dir__", krk_dirObject);
	krk_defineNative(&vm.baseClasses->objectClass->methods, "__str__", _strBase);
	krk_defineNative(&vm.baseClasses->objectClass->methods, "__repr__", _strBase); /* Override if necesary */
	krk_defineNative(&vm.baseClasses->objectClass->methods, "__hash__", obj_hash);
	krk_defineNative(&vm.baseClasses->objectClass->methods, "__eq__", obj_eq);
	krk_finalizeClass(vm.baseClasses->objectClass);
	KRK_DOC(vm.baseClasses->objectClass,
		"@brief Base class for all types.\n\n"
		"The @c object base class provides the fallback implementations of methods like "
		"@ref object___dir__ \"__dir__\". All object and primitive types eventually inherit from @c object."
	);

	vm.baseClasses->moduleClass = krk_newClass(S("module"), vm.baseClasses->objectClass);
	vm.baseClasses->moduleClass->allocSize = sizeof(struct KrkModule);
#ifndef STATIC_ONLY
	vm.baseClasses->moduleClass->_ongcsweep = module_sweep;
#endif
	krk_push(OBJECT_VAL(vm.baseClasses->moduleClass));
	krk_defineNative(&vm.baseClasses->moduleClass->methods, "__repr__", _module_repr);
	krk_defineNative(&vm.baseClasses->moduleClass->methods, "__str__", _module_repr);
	krk_finalizeClass(vm.baseClasses->moduleClass);
	KRK_DOC(vm.baseClasses->moduleClass, "Type of imported modules and packages.");

	vm.builtins = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "__builtins__", (KrkObj*)vm.builtins);
	krk_attachNamedObject(&vm.builtins->fields, "object", (KrkObj*)vm.baseClasses->objectClass);
	krk_pop();
	krk_pop();

	krk_attachNamedObject(&vm.builtins->fields, "__name__", (KrkObj*)S("__builtins__"));
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

	property = krk_makeClass(vm.builtins, &vm.baseClasses->propertyClass, "property", vm.baseClasses->objectClass);
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

	krk_makeClass(vm.builtins, &Helper, "Helper", vm.baseClasses->objectClass);
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

	krk_makeClass(vm.builtins, &LicenseReader, "LicenseReader", vm.baseClasses->objectClass);
	KRK_DOC(LicenseReader, "Special object that prints Kuroko's copyright information when passed to @ref repr");
	KRK_DOC(BIND_METHOD(LicenseReader,__call__), "Print the full license statement.");
	BIND_METHOD(LicenseReader,__repr__);
	krk_finalizeClass(LicenseReader);
	krk_attachNamedObject(&vm.builtins->fields, "license", (KrkObj*)krk_newInstance(LicenseReader));

	krk_makeClass(vm.builtins, &map, "map", vm.baseClasses->objectClass);
	KRK_DOC(map, "Return an iterator that applies a function to a series of iterables");
	BIND_METHOD(map,__init__);
	BIND_METHOD(map,__iter__);
	BIND_METHOD(map,__call__);
	krk_finalizeClass(map);

	krk_makeClass(vm.builtins, &filter, "filter", vm.baseClasses->objectClass);
	KRK_DOC(filter, "Return an iterator that returns only the items from an iterable for which the given function returns true.");
	BIND_METHOD(filter,__init__);
	BIND_METHOD(filter,__iter__);
	BIND_METHOD(filter,__call__);
	krk_finalizeClass(filter);

	krk_makeClass(vm.builtins, &enumerate, "enumerate", vm.baseClasses->objectClass);
	KRK_DOC(enumerate, "Return an iterator that produces a tuple with a count the iterated values of the passed iteratable.");
	BIND_METHOD(enumerate,__init__);
	BIND_METHOD(enumerate,__iter__);
	BIND_METHOD(enumerate,__call__);
	krk_finalizeClass(enumerate);

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
		"@arguments obj\n\n"
		"Uses various internal methods to collect a list of property names of @p obj, returning "
		"that list sorted lexicographically.");
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
		"@arguments *args,sep=' ',end='\\n'\n\n"
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
	BUILTIN_FUNCTION("zip", FUNC_NAME(krk,zip),
		"@brief Returns an iterator that produces tuples of the nth element of each passed iterable.\n"
		"@arguments *iterables\n\n"
		"Creates an iterator that returns a tuple of elements from each of @p iterables, until one "
		"of @p iterables is exhuasted.");
	BUILTIN_FUNCTION("next", FUNC_NAME(krk,next),
		"@brief Compatibility function. Calls an iterable.\n"
		"@arguments iterable");
}

