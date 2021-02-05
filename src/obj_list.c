#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define LIST_WRAP_INDEX() \
	if (index < 0) index += self->values.count; \
	if (index < 0 || index >= (krk_integer_type)self->values.count) return krk_runtimeError(vm.exceptions.indexError, "list index out of range: " PRIkrk_int, index)

#define LIST_WRAP_SOFT(val) \
	if (val < 0) val += self->values.count; \
	if (val < 0) val = 0; \
	if (val > (krk_integer_type)self->values.count) val = self->values.count

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

#define CURRENT_CTYPE KrkList *
#define CURRENT_NAME  self

KRK_METHOD(list,__get__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,index);
	LIST_WRAP_INDEX();
	return self->values.values[index];
})

KRK_METHOD(list,__set__,{
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,index);
	LIST_WRAP_INDEX();
	self->values.values[index] = argv[2];
})

KRK_METHOD(list,append,{
	METHOD_TAKES_EXACTLY(1);
	krk_writeValueArray(&self->values, argv[1]);
})

KRK_METHOD(list,insert,{
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,index);
	LIST_WRAP_INDEX();
	krk_writeValueArray(&self->values, NONE_VAL());
	memmove(
		&self->values.values[index+1],
		&self->values.values[index],
		sizeof(KrkValue) * (self->values.count - index - 1)
	);
	self->values.values[index] = argv[2];
})

KRK_METHOD(list,__repr__,{
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("[...]"));
	((KrkObj*)self)->inRepr = 1;
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb, '[');
	for (size_t i = 0; i < self->values.count; ++i) {
		/* repr(self[i]) */
		KrkClass * type = krk_getType(self->values.values[i]);
		krk_push(self->values.values[i]);
		KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);

		if (IS_STRING(result)) {
			pushStringBuilderStr(&sb, AS_STRING(result)->chars, AS_STRING(result)->length);
		}

		if (i + 1 < self->values.count) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
	}

	pushStringBuilder(&sb,']');
	((KrkObj*)self)->inRepr = 0;
	return finishStringBuilder(&sb);
})

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
KRK_METHOD(list,extend,{
	METHOD_TAKES_EXACTLY(1);
	KrkValueArray *  positionals = AS_LIST(argv[0]);
	KrkValue value = argv[1];
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
				FUNC_NAME(list,append)(2, (KrkValue[]){argv[0], krk_peek(0)}, 0);
				krk_pop();
			} while (1);
		} else {
			return krk_runtimeError(vm.exceptions.typeError, "'%s' object is not iterable", krk_typeName(value));
		}
	}
})
#undef unpackArray

KRK_METHOD(list,__init__,{
	METHOD_TAKES_AT_MOST(1);
	krk_initValueArray(AS_LIST(argv[0]));
	if (argc == 2) {
		_list_extend(2,(KrkValue[]){argv[0],argv[1]},0);
	}
	return argv[0];
})

KRK_METHOD(list,__mul__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,howMany);

	KrkValue out = krk_list_of(0, NULL);

	krk_push(out);

	for (krk_integer_type i = 0; i < howMany; i++) {
		_list_extend(2, (KrkValue[]){out,argv[0]},0);
	}

	return krk_pop();
})

KRK_METHOD(list,__len__,{
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->values.count);
})

KRK_METHOD(list,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(argv[1], self->values.values[i])) return BOOLEAN_VAL(1);
	}
	return BOOLEAN_VAL(0);
})

KRK_METHOD(list,__getslice__,{
	METHOD_TAKES_EXACTLY(2);

	if (!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) || !(IS_INTEGER(argv[2]) || IS_NONE(argv[2])))
		return krk_runtimeError(vm.exceptions.typeError, "%s() expects two integer arguments", "__getslice__");

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
})

KRK_METHOD(list,pop,{
	METHOD_TAKES_AT_MOST(1);
	krk_integer_type index = self->values.count - 1;
	if (argc == 2) {
		CHECK_ARG(1,int,krk_integer_type,ind);
		index = ind;
	}
	LIST_WRAP_INDEX();
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
})

KRK_METHOD(list,remove,{
	METHOD_TAKES_EXACTLY(1);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) {
			return FUNC_NAME(list,pop)(2,(KrkValue[]){argv[0], INTEGER_VAL(i)},0);
		}
	}
	return krk_runtimeError(vm.exceptions.valueError, "not found");
})

KRK_METHOD(list,clear,{
	METHOD_TAKES_NONE();
	krk_freeValueArray(&self->values);
})

KRK_METHOD(list,index,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(3);

	krk_integer_type min = 0;
	krk_integer_type max = self->values.count;

	if (argc > 2) {
		if (IS_INTEGER(argv[2]))
			min = AS_INTEGER(argv[2]);
		else
			return krk_runtimeError(vm.exceptions.typeError, "min must be int, not '%s'", krk_typeName(argv[2]));
	}

	if (argc > 3) {
		if (IS_INTEGER(argv[3]))
			max = AS_INTEGER(argv[3]);
		else
			return krk_runtimeError(vm.exceptions.typeError, "max must be int, not '%s'", krk_typeName(argv[3]));
	}

	LIST_WRAP_SOFT(min);
	LIST_WRAP_SOFT(max);

	for (krk_integer_type i = min; i < max; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) return INTEGER_VAL(i);
	}

	return krk_runtimeError(vm.exceptions.valueError, "not found");
})

KRK_METHOD(list,count,{
	METHOD_TAKES_EXACTLY(1);
	krk_integer_type count = 0;

	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) count++;
	}

	return INTEGER_VAL(count);
})

KRK_METHOD(list,copy,{
	METHOD_TAKES_NONE();
	return krk_list_of(self->values.count, self->values.values);
})

KRK_METHOD(list,reverse,{
	METHOD_TAKES_NONE();
	for (size_t i = 0; i < (self->values.count) / 2; i++) {
		KrkValue tmp = self->values.values[i];
		self->values.values[i] = self->values.values[self->values.count-i-1];
		self->values.values[self->values.count-i-1] = tmp;
	}
	return NONE_VAL();
})

static int _list_sorter(const void * _a, const void * _b) {
	KrkValue a = *(KrkValue*)_a;
	KrkValue b = *(KrkValue*)_b;

	KrkValue ltComp = krk_operator_lt(a,b);
	if (IS_NONE(ltComp) || (IS_BOOLEAN(ltComp) && AS_BOOLEAN(ltComp))) return -1;
	KrkValue gtComp = krk_operator_gt(a,b);
	if (IS_NONE(gtComp) || (IS_BOOLEAN(gtComp) && AS_BOOLEAN(gtComp))) return 1;
	return 0;
}

KRK_METHOD(list,sort,{
	METHOD_TAKES_NONE();

	qsort(self->values.values, self->values.count, sizeof(KrkValue), _list_sorter);
})

FUNC_SIG(listiterator,__init__);

KRK_METHOD(list,__iter__,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses.listiteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(listiterator,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();

	return OBJECT_VAL(output);
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkInstance*

KRK_METHOD(listiterator,__init__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,list,KrkList*,list);

	krk_push(argv[0]);
	krk_attachNamedValue(&self->fields, "l", OBJECT_VAL(list));
	krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(0));
	krk_pop();

	return argv[0];
})

KRK_METHOD(listiterator,__call__,{
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
})


static KrkValue _sorted(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions.argumentError,"%s() takes %s %d argument%s (%d given)","sorted","exactly",1,argc);
	KrkValue listOut = krk_list_of(0,NULL);
	krk_push(listOut);
	FUNC_NAME(list,extend)(2,(KrkValue[]){listOut,argv[0]},0);
	if (!IS_NONE(vm.currentException)) return NONE_VAL();
	FUNC_NAME(list,sort)(1,&listOut,0);
	if (!IS_NONE(vm.currentException)) return NONE_VAL();
	return krk_pop();
}

_noexport
void _createAndBind_listClass(void) {
	KrkClass * list = ADD_BASE_CLASS(vm.baseClasses.listClass, "list", vm.objectClass);
	list->allocSize = sizeof(KrkList);
	list->_ongcscan = _list_gcscan;
	list->_ongcsweep = _list_gcsweep;
	BIND_METHOD(list,__init__);
	BIND_METHOD(list,__get__);
	BIND_METHOD(list,__set__);
	BIND_METHOD(list,__len__);
	BIND_METHOD(list,__repr__);
	BIND_METHOD(list,__contains__);
	BIND_METHOD(list,__getslice__);
	BIND_METHOD(list,__iter__);
	BIND_METHOD(list,__mul__);
	BIND_METHOD(list,append);
	BIND_METHOD(list,extend);
	BIND_METHOD(list,pop);
	BIND_METHOD(list,insert);
	BIND_METHOD(list,clear);
	BIND_METHOD(list,index);
	BIND_METHOD(list,count);
	BIND_METHOD(list,copy);
	BIND_METHOD(list,remove);
	BIND_METHOD(list,reverse);
	BIND_METHOD(list,sort);
	krk_defineNative(&list->methods, ".__delitem__", FUNC_NAME(list,pop));
	krk_defineNative(&list->methods, ".__str__", FUNC_NAME(list,__repr__));
	krk_finalizeClass(list);
	list->docstring = S("Mutable sequence of arbitrary values.");

	BUILTIN_FUNCTION("listOf", krk_list_of, "Convert argument sequence to list object.");
	BUILTIN_FUNCTION("sorted", _sorted, "Return a sorted representation of an iterable.");

	KrkClass * listiterator = ADD_BASE_CLASS(vm.baseClasses.listiteratorClass, "listiterator", vm.objectClass);
	BIND_METHOD(listiterator,__init__);
	BIND_METHOD(listiterator,__call__);
	krk_finalizeClass(listiterator);

}
