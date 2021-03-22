#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/threads.h>

#define LIST_WRAP_INDEX() \
	if (index < 0) index += self->values.count; \
	if (index < 0 || index >= (krk_integer_type)self->values.count) return krk_runtimeError(vm.exceptions->indexError, "list index out of range: " PRIkrk_int, index)

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
KrkValue krk_list_of(int argc, KrkValue argv[], int hasKw) {
	KrkValue outList = OBJECT_VAL(krk_newInstance(vm.baseClasses->listClass));
	krk_push(outList);
	krk_initValueArray(AS_LIST(outList));

	if (argc) {
		AS_LIST(outList)->capacity = argc;
		AS_LIST(outList)->values = GROW_ARRAY(KrkValue, AS_LIST(outList)->values, 0, argc);
		memcpy(AS_LIST(outList)->values, argv, sizeof(KrkValue) * argc);
		AS_LIST(outList)->count = argc;
	}

	pthread_rwlock_init(&((KrkList*)AS_OBJECT(outList))->rwlock, NULL);
	return krk_pop();
}

#define CURRENT_CTYPE KrkList *
#define CURRENT_NAME  self

KRK_METHOD(list,__getitem__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,index);
	pthread_rwlock_rdlock(&self->rwlock);
	LIST_WRAP_INDEX();
	KrkValue result = self->values.values[index];
	pthread_rwlock_unlock(&self->rwlock);
	return result;
})

KRK_METHOD(list,__setitem__,{
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,index);
	pthread_rwlock_rdlock(&self->rwlock);
	LIST_WRAP_INDEX();
	self->values.values[index] = argv[2];
	pthread_rwlock_unlock(&self->rwlock);
})

KRK_METHOD(list,append,{
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_wrlock(&self->rwlock);
	krk_writeValueArray(&self->values, argv[1]);
	pthread_rwlock_unlock(&self->rwlock);
})

KRK_METHOD(list,insert,{
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,index);
	pthread_rwlock_wrlock(&self->rwlock);
	LIST_WRAP_INDEX();
	krk_writeValueArray(&self->values, NONE_VAL());
	memmove(
		&self->values.values[index+1],
		&self->values.values[index],
		sizeof(KrkValue) * (self->values.count - index - 1)
	);
	self->values.values[index] = argv[2];
	pthread_rwlock_unlock(&self->rwlock);
})

KRK_METHOD(list,__repr__,{
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("[...]"));
	((KrkObj*)self)->inRepr = 1;
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb, '[');
	pthread_rwlock_rdlock(&self->rwlock);
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
	pthread_rwlock_unlock(&self->rwlock);

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
				if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) goto _break_loop; \
			} \
		} while (0)
KRK_METHOD(list,extend,{
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_wrlock(&self->rwlock);
	KrkValueArray *  positionals = AS_LIST(argv[0]);
	if (krk_valuesSame(argv[0],argv[1])) {
		argv[1] = krk_list_of(self->values.count, self->values.values, 0);
	}
	unpackIterableFast(argv[1]);
_break_loop:
	pthread_rwlock_unlock(&self->rwlock);
})
#undef unpackArray

KRK_METHOD(list,__init__,{
	METHOD_TAKES_AT_MOST(1);
	krk_initValueArray(AS_LIST(argv[0]));
	pthread_rwlock_init(&self->rwlock, NULL);
	if (argc == 2) {
		_list_extend(2,(KrkValue[]){argv[0],argv[1]},0);
	}
	return argv[0];
})

KRK_METHOD(list,__mul__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,howMany);

	KrkValue out = krk_list_of(0, NULL, 0);

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
	pthread_rwlock_rdlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(argv[1], self->values.values[i])) {
			pthread_rwlock_unlock(&self->rwlock);
			return BOOLEAN_VAL(1);
		}
	}
	pthread_rwlock_unlock(&self->rwlock);
	return BOOLEAN_VAL(0);
})

KRK_METHOD(list,__getslice__,{
	METHOD_TAKES_EXACTLY(2);
	if (!(IS_INTEGER(argv[1]) || IS_NONE(argv[1]))) return TYPE_ERROR(int or None, argv[1]);
	if (!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) return TYPE_ERROR(int or None, argv[2]);
	pthread_rwlock_rdlock(&self->rwlock);
	krk_integer_type start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	krk_integer_type end   = IS_NONE(argv[2]) ? (krk_integer_type)self->values.count : AS_INTEGER(argv[2]);
	LIST_WRAP_SOFT(start);
	LIST_WRAP_SOFT(end);
	if (end < start) end = start;
	krk_integer_type len = end - start;

	KrkValue result = krk_list_of(len, &AS_LIST(argv[0])->values[start], 0);
	pthread_rwlock_unlock(&self->rwlock);
	return result;
})

FUNC_SIG(list,pop);
KRK_METHOD(list,__delslice__,{
	METHOD_TAKES_EXACTLY(2);
	if (!(IS_INTEGER(argv[1]) || IS_NONE(argv[1]))) return TYPE_ERROR(int or None, argv[1]);
	if (!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) return TYPE_ERROR(int or None, argv[2]);
	pthread_rwlock_wrlock(&self->rwlock);
	krk_integer_type start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	krk_integer_type end   = IS_NONE(argv[2]) ? (krk_integer_type)self->values.count : AS_INTEGER(argv[2]);
	LIST_WRAP_SOFT(start);
	LIST_WRAP_SOFT(end);
	if (end < start) end = start;
	krk_integer_type len = end - start;

	while (len > 0) {
		FUNC_NAME(list,pop)(2,(KrkValue[]){argv[0],INTEGER_VAL(start)},0);
		len--;
	}
	pthread_rwlock_unlock(&self->rwlock);
})

KRK_METHOD(list,__setslice__,{
	METHOD_TAKES_EXACTLY(3);
	if (!(IS_INTEGER(argv[1]) || IS_NONE(argv[1]))) return TYPE_ERROR(int or None, argv[1]);
	if (!(IS_INTEGER(argv[2]) || IS_NONE(argv[2]))) return TYPE_ERROR(int or None, argv[2]);
	if (!IS_list(argv[3])) return TYPE_ERROR(list,argv[3]); /* TODO other sequence types */
	pthread_rwlock_wrlock(&self->rwlock);
	krk_integer_type start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	krk_integer_type end   = IS_NONE(argv[2]) ? (krk_integer_type)self->values.count : AS_INTEGER(argv[2]);
	LIST_WRAP_SOFT(start);
	LIST_WRAP_SOFT(end);
	if (end < start) end = start;
	krk_integer_type len = end - start;

	krk_integer_type newLen = (krk_integer_type)AS_LIST(argv[3])->count;

	for (krk_integer_type i = 0; (i < len && i < newLen); ++i) {
		AS_LIST(argv[0])->values[start+i] = AS_LIST(argv[3])->values[i];
	}

	while (len < newLen) {
		FUNC_NAME(list,insert)(3, (KrkValue[]){argv[0], INTEGER_VAL(start + len), AS_LIST(argv[3])->values[len]}, 0);
		len++;
	}

	while (newLen < len) {
		FUNC_NAME(list,pop)(2, (KrkValue[]){argv[0], INTEGER_VAL(start + len - 1)}, 0);
		len--;
	}

	pthread_rwlock_unlock(&self->rwlock);
})

KRK_METHOD(list,pop,{
	METHOD_TAKES_AT_MOST(1);
	pthread_rwlock_wrlock(&self->rwlock);
	krk_integer_type index = self->values.count - 1;
	if (argc == 2) {
		CHECK_ARG(1,int,krk_integer_type,ind);
		index = ind;
	}
	LIST_WRAP_INDEX();
	KrkValue outItem = AS_LIST(argv[0])->values[index];
	if (index == (long)AS_LIST(argv[0])->count-1) {
		AS_LIST(argv[0])->count--;
		pthread_rwlock_unlock(&self->rwlock);
		return outItem;
	} else {
		/* Need to move up */
		size_t remaining = AS_LIST(argv[0])->count - index - 1;
		memmove(&AS_LIST(argv[0])->values[index], &AS_LIST(argv[0])->values[index+1],
			sizeof(KrkValue) * remaining);
		AS_LIST(argv[0])->count--;
		pthread_rwlock_unlock(&self->rwlock);
		return outItem;
	}
})

KRK_METHOD(list,remove,{
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_wrlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) {
			pthread_rwlock_unlock(&self->rwlock);
			return FUNC_NAME(list,pop)(2,(KrkValue[]){argv[0], INTEGER_VAL(i)},0);
		}
	}
	pthread_rwlock_unlock(&self->rwlock);
	return krk_runtimeError(vm.exceptions->valueError, "not found");
})

KRK_METHOD(list,clear,{
	METHOD_TAKES_NONE();
	pthread_rwlock_wrlock(&self->rwlock);
	krk_freeValueArray(&self->values);
	pthread_rwlock_unlock(&self->rwlock);
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
			return krk_runtimeError(vm.exceptions->typeError, "min must be int, not '%s'", krk_typeName(argv[2]));
	}

	if (argc > 3) {
		if (IS_INTEGER(argv[3]))
			max = AS_INTEGER(argv[3]);
		else
			return krk_runtimeError(vm.exceptions->typeError, "max must be int, not '%s'", krk_typeName(argv[3]));
	}

	pthread_rwlock_rdlock(&self->rwlock);
	LIST_WRAP_SOFT(min);
	LIST_WRAP_SOFT(max);

	for (krk_integer_type i = min; i < max; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) {
			pthread_rwlock_unlock(&self->rwlock);
			return INTEGER_VAL(i);
		}
	}

	pthread_rwlock_unlock(&self->rwlock);
	return krk_runtimeError(vm.exceptions->valueError, "not found");
})

KRK_METHOD(list,count,{
	METHOD_TAKES_EXACTLY(1);
	krk_integer_type count = 0;

	pthread_rwlock_rdlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesEqual(self->values.values[i], argv[1])) count++;
	}
	pthread_rwlock_unlock(&self->rwlock);

	return INTEGER_VAL(count);
})

KRK_METHOD(list,copy,{
	METHOD_TAKES_NONE();
	pthread_rwlock_rdlock(&self->rwlock);
	KrkValue result = krk_list_of(self->values.count, self->values.values, 0);
	pthread_rwlock_unlock(&self->rwlock);
	return result;
})

KRK_METHOD(list,reverse,{
	METHOD_TAKES_NONE();
	pthread_rwlock_wrlock(&self->rwlock);
	for (size_t i = 0; i < (self->values.count) / 2; i++) {
		KrkValue tmp = self->values.values[i];
		self->values.values[i] = self->values.values[self->values.count-i-1];
		self->values.values[self->values.count-i-1] = tmp;
	}
	pthread_rwlock_unlock(&self->rwlock);
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

	pthread_rwlock_wrlock(&self->rwlock);
	qsort(self->values.values, self->values.count, sizeof(KrkValue), _list_sorter);
	pthread_rwlock_unlock(&self->rwlock);
})

KRK_METHOD(list,__add__,{
	METHOD_TAKES_EXACTLY(1);
	if (!IS_list(argv[1])) return TYPE_ERROR(list,argv[1]);

	pthread_rwlock_rdlock(&self->rwlock);
	KrkValue outList = krk_list_of(self->values.count, self->values.values, 0); /* copy */
	pthread_rwlock_unlock(&self->rwlock);
	FUNC_NAME(list,extend)(2,(KrkValue[]){outList,argv[1]},0); /* extend */
	return outList;
})

FUNC_SIG(listiterator,__init__);

KRK_METHOD(list,__iter__,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->listiteratorClass);

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
	return krk_runtimeError(vm.exceptions->typeError, "Corrupt list iterator: %s", errorStr);
})


static KrkValue _sorted(int argc, KrkValue argv[], int hasKw) {
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError,"%s() takes %s %d argument%s (%d given)","sorted","exactly",1,"",argc);
	KrkValue listOut = krk_list_of(0,NULL,0);
	krk_push(listOut);
	FUNC_NAME(list,extend)(2,(KrkValue[]){listOut,argv[0]},0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	FUNC_NAME(list,sort)(1,&listOut,0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	return krk_pop();
}

static KrkValue _reversed(int argc, KrkValue argv[], int hasKw) {
	/* FIXME The Python reversed() function produces an iterator and only works for things with indexing or a __reversed__ method;
	 *       Building a list and reversing it like we do here is not correct! */
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError,"%s() takes %s %d argument%s (%d given)","reversed","exactly",1,"",argc);
	KrkValue listOut = krk_list_of(0,NULL,0);
	krk_push(listOut);
	FUNC_NAME(list,extend)(2,(KrkValue[]){listOut,argv[0]},0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	FUNC_NAME(list,reverse)(1,&listOut,0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	return krk_pop();
}

_noexport
void _createAndBind_listClass(void) {
	KrkClass * list = ADD_BASE_CLASS(vm.baseClasses->listClass, "list", vm.baseClasses->objectClass);
	list->allocSize = sizeof(KrkList);
	list->_ongcscan = _list_gcscan;
	list->_ongcsweep = _list_gcsweep;
	BIND_METHOD(list,__init__);
	BIND_METHOD(list,__getitem__);
	BIND_METHOD(list,__setitem__);
	BIND_METHOD(list,__len__);
	BIND_METHOD(list,__repr__);
	BIND_METHOD(list,__contains__);
	BIND_METHOD(list,__getslice__);
	BIND_METHOD(list,__delslice__);
	BIND_METHOD(list,__setslice__);
	BIND_METHOD(list,__iter__);
	BIND_METHOD(list,__mul__);
	BIND_METHOD(list,__add__);
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
	krk_defineNative(&list->methods, ".__class_getitem__", KrkGenericAlias);
	krk_finalizeClass(list);
	KRK_DOC(list, "Mutable sequence of arbitrary values.");

	BUILTIN_FUNCTION("listOf", krk_list_of, "Convert argument sequence to list object.");
	BUILTIN_FUNCTION("sorted", _sorted, "Return a sorted representation of an iterable.");
	BUILTIN_FUNCTION("reversed", _reversed, "Return a reversed representation of an iterable.");

	KrkClass * listiterator = ADD_BASE_CLASS(vm.baseClasses->listiteratorClass, "listiterator", vm.baseClasses->objectClass);
	BIND_METHOD(listiterator,__init__);
	BIND_METHOD(listiterator,__call__);
	krk_finalizeClass(listiterator);

}
