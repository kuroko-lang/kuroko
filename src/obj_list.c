#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define CHECK_LIST_FAST() if (unlikely(argc < 0 || !IS_INSTANCE(argv[0]) || \
	(AS_INSTANCE(argv[0])->_class != vm.baseClasses.listClass && !krk_isInstanceOf(argv[0], vm.baseClasses.listClass)))) \
		return krk_runtimeError(vm.exceptions.typeError, "expected list")


static void _list_gcscan(KrkInstance * self) {
	for (size_t i = 0; i < ((KrkList*)self)->values.count; ++i) {
		krk_markValue(((KrkList*)self)->values.values[i]);
	}
}

static void _list_gcsweep(KrkInstance * self) {
	krk_freeValueArray(&((KrkList*)self)->values);
}

/**
 * Exposed method called to produce lists from [expr,...] sequences in managed code.
 * Presented in the global namespace as listOf(...)
 */
KrkValue krk_list_of(int argc, KrkValue argv[]) {
	KrkValue outList = OBJECT_VAL(krk_newInstance(vm.baseClasses.listClass));
	krk_push(outList);
	krk_initValueArray(AS_LIST(outList));

	if (argc) {
		AS_LIST(outList)->capacity = argc;
		AS_LIST(outList)->values = GROW_ARRAY(KrkValue, AS_LIST(outList)->values, 0, argc);
		memcpy(AS_LIST(outList)->values, argv, sizeof(KrkValue) * argc);
		AS_LIST(outList)->count = argc;
	}

	return krk_pop();
}

/**
 * list.__get__(index)
 */
static KrkValue _list_get(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 2 || !IS_INTEGER(argv[1]))) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments in get %d, (%s, %s)", argc, krk_typeName(argv[0]), krk_typeName(argv[1]));
	int index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (unlikely(index < 0 || index >= (int)AS_LIST(argv[0])->count)) return krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
	return AS_LIST(argv[0])->values[index];
}

/**
 * list.__set__(index, value)
 */
static KrkValue _list_set(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 3 || !IS_INTEGER(argv[1]))) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments in set %d, (%s, %s, %s)", argc, krk_typeName(argv[0]), krk_typeName(argv[1]), krk_typeName(argv[2]));
	int index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (unlikely(index < 0 || index >= (int)AS_LIST(argv[0])->count)) krk_runtimeError(vm.exceptions.indexError, "index is out of range: %d", index);
	AS_LIST(argv[0])->values[index] = argv[2];
	return NONE_VAL();
}

/**
 * list.append(value)
 */
static KrkValue _list_append(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 2)) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
	krk_writeValueArray(AS_LIST(argv[0]), argv[1]);
	return NONE_VAL();
}

static KrkValue _list_insert(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (unlikely(argc < 3)) return krk_runtimeError(vm.exceptions.argumentError, "list.insert() expects two arguments");
	if (unlikely(!IS_INTEGER(argv[1]))) return krk_runtimeError(vm.exceptions.typeError, "index must be integer");
	krk_integer_type index = AS_INTEGER(argv[1]);
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (index < 0 || index > (long)AS_LIST(argv[0])->count) return krk_runtimeError(vm.exceptions.indexError, "list index out of range: %d", (int)index);

	krk_writeValueArray(AS_LIST(argv[0]), NONE_VAL());

	/* Move everything at and after this index one forward. */
	memmove(&AS_LIST(argv[0])->values[index+1],
	       &AS_LIST(argv[0])->values[index],
	       sizeof(KrkValue) * (AS_LIST(argv[0])->count - index - 1));
	/* Stick argv[2] where it belongs */
	AS_LIST(argv[0])->values[index] = argv[2];
	return NONE_VAL();
}

/**
 * list.__repr__
 */
static KrkValue _list_repr(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	KrkValue self = argv[0];
	if (AS_OBJECT(self)->inRepr) return OBJECT_VAL(S("[...]"));
	krk_push(OBJECT_VAL(S("[")));

	AS_OBJECT(self)->inRepr = 1;

	size_t len = AS_LIST(self)->count;
	for (size_t i = 0; i < len; ++i) {
		KrkClass * type = krk_getType(AS_LIST(self)->values[i]);
		krk_push(AS_LIST(self)->values[i]);
		krk_push(krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0));
		krk_addObjects();
		if (i + 1 < len) {
			krk_push(OBJECT_VAL(S(", ")));
			krk_addObjects();
		}
	}

	AS_OBJECT(self)->inRepr = 0;

	krk_push(OBJECT_VAL(S("]")));
	krk_addObjects();
	return krk_pop();
}

#define unpackArray(counter, indexer) do { \
			if (positionals->count + counter > positionals->capacity) { \
				size_t old = positionals->capacity; \
				positionals->capacity = positionals->count + counter; \
				positionals->values = GROW_ARRAY(KrkValue,positionals->values,old,positionals->capacity); \
			} \
			for (size_t i = 0; i < counter; ++i) { \
				positionals->values[positionals->count] = indexer; \
				positionals->count++; \
			} \
		} while (0)
static KrkValue _list_extend(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	KrkValueArray *  positionals = AS_LIST(argv[0]);
	KrkValue value = argv[1];
	//UNPACK_ARRAY();  /* This should be a macro that does all of these things. */
	if (IS_TUPLE(value)) {
		unpackArray(AS_TUPLE(value)->values.count, AS_TUPLE(value)->values.values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.listClass) {
		unpackArray(AS_LIST(value)->count, AS_LIST(value)->values[i]);
	} else if (IS_INSTANCE(value) && AS_INSTANCE(value)->_class == vm.baseClasses.dictClass) {
		unpackArray(AS_DICT(value)->count, krk_dict_nth_key_fast(AS_DICT(value)->capacity, AS_DICT(value)->entries, i));
	} else if (IS_STRING(value)) {
		unpackArray(AS_STRING(value)->codesLength, krk_string_get(2,(KrkValue[]){value,INTEGER_VAL(i)},0));
	} else {
		KrkClass * type = krk_getType(argv[1]);
		if (type->_iter) {
			/* Create the iterator */
			size_t stackOffset = vm.stackTop - vm.stack;
			krk_push(argv[1]);
			krk_push(krk_callSimple(OBJECT_VAL(type->_iter), 1, 0));

			do {
				/* Call it until it gives us itself */
				krk_push(vm.stack[stackOffset]);
				krk_push(krk_callSimple(krk_peek(0), 0, 1));
				if (krk_valuesSame(vm.stack[stackOffset], krk_peek(0))) {
					/* We're done. */
					krk_pop(); /* The result of iteration */
					krk_pop(); /* The iterator */
					break;
				}
				_list_append(2, (KrkValue[]){argv[0], krk_peek(0)});
				krk_pop();
			} while (1);
		} else {
			return krk_runtimeError(vm.exceptions.typeError, "'%s' object is not iterable", krk_typeName(value));
		}
	}
#undef unpackArray
	return NONE_VAL();
}

/**
 * list.__init__()
 */
static KrkValue _list_init(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	krk_initValueArray(AS_LIST(argv[0]));

	if (argc > 2) return krk_runtimeError(vm.exceptions.argumentError, "too many arguments to list.__init__");
	if (argc == 2) {
		/* TODO: Why not just initialize it this way... */
		_list_extend(2,(KrkValue[]){argv[0],argv[1]});
	}

	return argv[0];
}


static KrkValue _list_mul(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (!IS_INTEGER(argv[1]))
		return krk_runtimeError(vm.exceptions.typeError, "unsupported operand types for *: '%s' and '%s'",
			"list", krk_typeName(argv[1]));

	krk_integer_type howMany = AS_INTEGER(argv[1]);

	KrkValue out = krk_list_of(0, NULL);

	krk_push(out);

	for (krk_integer_type i = 0; i < howMany; i++) {
		_list_extend(2, (KrkValue[]){out,argv[0]});
	}

	return krk_pop();
}

/**
 * list.__len__
 */
static KrkValue _list_len(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc < 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
	return INTEGER_VAL(AS_LIST(argv[0])->count);
}

/**
 * list.__contains__
 */
static KrkValue _list_contains(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number or type of arguments");
	for (size_t i = 0; i < AS_LIST(argv[0])->count; ++i) {
		if (krk_valuesEqual(argv[1], AS_LIST(argv[0])->values[i])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
}

/**
 * list.__getslice__
 */
static KrkValue _list_slice(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (argc < 3) return krk_runtimeError(vm.exceptions.argumentError, "slice: expected 2 arguments, got %d", argc-1);
	if (!IS_INSTANCE(argv[0]) ||
		!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) ||
		!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) {
		return krk_runtimeError(vm.exceptions.typeError, "slice: expected two integer arguments");
	}

	int start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	int end   = IS_NONE(argv[2]) ? (int)AS_LIST(argv[0])->count : AS_INTEGER(argv[2]);
	if (start < 0) start = (int)AS_LIST(argv[0])->count + start;
	if (start < 0) start = 0;
	if (end < 0) end = (int)AS_LIST(argv[0])->count + end;
	if (start > (int)AS_LIST(argv[0])->count) start = (int)AS_LIST(argv[0])->count;
	if (end > (int)AS_LIST(argv[0])->count) end = (int)AS_LIST(argv[0])->count;
	if (end < start) end = start;
	int len = end - start;

	return krk_list_of(len, &AS_LIST(argv[0])->values[start]);
}

/**
 * list.pop()
 */
static KrkValue _list_pop(int argc, KrkValue argv[]) {
	CHECK_LIST_FAST();
	if (!AS_LIST(argv[0])->count) return krk_runtimeError(vm.exceptions.indexError, "pop from empty list");
	long index = AS_LIST(argv[0])->count - 1;
	if (argc > 1) {
		index = AS_INTEGER(argv[1]);
	}
	if (index < 0) index += AS_LIST(argv[0])->count;
	if (index < 0 || index >= (long)AS_LIST(argv[0])->count) return krk_runtimeError(vm.exceptions.indexError, "list index out of range: %d", (int)index);
	KrkValue outItem = AS_LIST(argv[0])->values[index];
	if (index == (long)AS_LIST(argv[0])->count-1) {
		AS_LIST(argv[0])->count--;
		return outItem;
	} else {
		/* Need to move up */
		size_t remaining = AS_LIST(argv[0])->count - index - 1;
		memmove(&AS_LIST(argv[0])->values[index], &AS_LIST(argv[0])->values[index+1],
			sizeof(KrkValue) * remaining);
		AS_LIST(argv[0])->count--;
		return outItem;
	}
}

static KrkValue _listiter_init(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.listiteratorClass) {
		return krk_runtimeError(vm.exceptions.typeError, "Tried to call listiterator.__init__() on something not a list iterator");
	}
	if (argc < 2 || !IS_INSTANCE(argv[1])) {
		return krk_runtimeError(vm.exceptions.argumentError, "Expected a list.");
	}
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue _list = argv[1];

	krk_push(argv[0]);
	krk_attachNamedValue(&self->fields, "l", _list);
	krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(0));
	krk_pop();

	return argv[0];
}

static KrkValue _listiter_call(int argc, KrkValue argv[]) {
	if (!IS_INSTANCE(argv[0]) || AS_INSTANCE(argv[0])->_class != vm.baseClasses.listiteratorClass) {
		return krk_runtimeError(vm.exceptions.typeError, "Tried to call listiterator.__call__() on something not a list iterator");
	}
	KrkInstance * self = AS_INSTANCE(argv[0]);
	KrkValue _list;
	KrkValue _counter;
	const char * errorStr = NULL;

	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("l")), &_list)) {
		errorStr = "no list pointer";
		goto _corrupt;
	}
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("i")), &_counter)) {
		errorStr = "no index";
		goto _corrupt;
	}

	if ((size_t)AS_INTEGER(_counter) >= AS_LIST(_list)->count) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return AS_LIST(_list)->values[AS_INTEGER(_counter)];
	}

_corrupt:
	return krk_runtimeError(vm.exceptions.typeError, "Corrupt list iterator: %s", errorStr);
}

static KrkValue _list_iter(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.listiteratorClass);

	krk_push(OBJECT_VAL(output));
	_listiter_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();

	return OBJECT_VAL(output);
}

_noexport
void _createAndBind_listClass(void) {
	ADD_BASE_CLASS(vm.baseClasses.listClass, "list", vm.objectClass);
	vm.baseClasses.listClass->allocSize = sizeof(KrkList);
	vm.baseClasses.listClass->_ongcscan = _list_gcscan;
	vm.baseClasses.listClass->_ongcsweep = _list_gcsweep;
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__init__", _list_init);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__get__", _list_get);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__set__", _list_set);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__delitem__", _list_pop);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__len__", _list_len);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__str__", _list_repr);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__repr__", _list_repr);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__contains__", _list_contains);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__getslice__", _list_slice);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__iter__", _list_iter);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".__mul__", _list_mul);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".append", _list_append);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".extend", _list_extend);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".pop", _list_pop);
	krk_defineNative(&vm.baseClasses.listClass->methods, ".insert", _list_insert);
	krk_finalizeClass(vm.baseClasses.listClass);
	vm.baseClasses.listClass->docstring = S("Mutable sequence of arbitrary values.");

	krk_makeClass(vm.builtins, &vm.baseClasses.listiteratorClass, "listiterator", vm.objectClass);
	krk_defineNative(&vm.baseClasses.listiteratorClass->methods, ".__init__", _listiter_init);
	krk_defineNative(&vm.baseClasses.listiteratorClass->methods, ".__call__", _listiter_call);
	krk_finalizeClass(vm.baseClasses.listiteratorClass);

}
