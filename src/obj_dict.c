#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#if defined(__TINYC__) || (defined(_MSC_VER) && !defined(__clang__))
static int __builtin_clz(unsigned int x) {
	int i = 31;
	while (!(x & (1 << i)) && i >= 0) i--;
	return 31-i;
}
#endif

/**
 * Exposed method called to produce dictionaries from `{expr: expr, ...}` sequences in managed code.
 * Expects arguments as `key,value,key,value`...
 */
KrkValue krk_dict_of(int argc, const KrkValue argv[], int hasKw) {
	if (argc % 2 != 0) return krk_runtimeError(vm.exceptions->argumentError, "Expected even number of arguments to krk_dict_of");
	KrkInstance * outDict = krk_newInstance(vm.baseClasses->dictClass);
	krk_push(OBJECT_VAL(outDict));
	krk_initTable(&((KrkDict*)outDict)->entries);
	if (argc) {
		size_t capacity = argc;
		size_t powerOfTwoCapacity = __builtin_clz(1) - __builtin_clz(capacity);
		if ((1UL << powerOfTwoCapacity) != capacity) powerOfTwoCapacity++;
		capacity = (1UL << powerOfTwoCapacity);
		krk_tableAdjustCapacity(&((KrkDict*)outDict)->entries, capacity);
		for (int ind = 0; ind < argc; ind += 2) {
			krk_tableSet(&((KrkDict*)outDict)->entries, argv[ind], argv[ind+1]);
		}
	}
	return krk_pop();
}

static void _dict_gcscan(KrkInstance * self) {
	krk_markTable(&((KrkDict*)self)->entries);
}

static void _dict_gcsweep(KrkInstance * self) {
	krk_freeTable(&((KrkDict*)self)->entries);
}

#define CURRENT_CTYPE KrkDict *
#define CURRENT_NAME  self

struct _keyvalue_pair_context {
	KrkDict * self;
	KrkValue key;
	int counter;
};

static int _keyvalue_pair_callback(void * context, const KrkValue * entries, size_t count) {
	struct _keyvalue_pair_context * _context = context;

	if (count > 2) {
		_context->counter = count;
		return 1;
	}

	for (size_t i = 0; i < count; ++i) {
		if (_context->counter == 0) {
			_context->counter = 1;
			_context->key = entries[i];
		} else if (_context->counter == 1) {
			_context->counter = 2;
			krk_tableSet(&_context->self->entries, _context->key, entries[i]);
		} else {
			_context->counter = -1;
			return 1;
		}
	}

	if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
	return 0;
}

static int unpackKeyValuePair(void * self, const KrkValue * pairs, size_t count) {
	struct _keyvalue_pair_context context = { (KrkDict*)self, NONE_VAL(), 0 };

	for (size_t i = 0; i < count; ++i) {
		if (krk_unpackIterable(pairs[i], &context, _keyvalue_pair_callback)) return 1;
	}

	if (context.counter != 2) {
		krk_runtimeError(vm.exceptions->valueError, "dictionary update sequence element has invalid length");
		return 1;
	}
	return 0;
}

KRK_Method(dict,__init__) {
	METHOD_TAKES_AT_MOST(1);
	krk_initTable(&self->entries);

	if (argc > 1) {
		if (krk_unpackIterable(argv[1], self, unpackKeyValuePair)) return NONE_VAL();
	}

	if (hasKw) {
		krk_tableAddAll(AS_DICT(argv[argc]), &self->entries);
	}

	return NONE_VAL();
}

KRK_Method(dict,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_dict(argv[1]))
		return NOTIMPL_VAL();
	CHECK_ARG(1,dict,KrkDict*,them);
	if (self->entries.count != them->entries.count)
		return BOOLEAN_VAL(0);


	for (unsigned int i = 0; i < self->entries.capacity; ++i) {
		if (IS_KWARGS(self->entries.entries[i].key)) continue;
		KrkValue val;
		if (!krk_tableGet(&them->entries, self->entries.entries[i].key, &val)) return BOOLEAN_VAL(0);
		if (!krk_valuesSameOrEqual(self->entries.entries[i].value, val)) return BOOLEAN_VAL(0);
	}

	return BOOLEAN_VAL(1);
}


KRK_Method(dict,__getitem__) {
	METHOD_TAKES_EXACTLY(1);
	KrkValue out;
	if (!krk_tableGet(&self->entries, argv[1], &out)) {
		if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
		return krk_runtimeError(vm.exceptions->keyError, "%V", argv[1]);
	}
	return out;
}

KRK_Method(dict,__setitem__) {
	METHOD_TAKES_EXACTLY(2);
	krk_tableSet(&self->entries, argv[1], argv[2]);
	return argv[2];
}

KRK_Method(dict,__or__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,them);
	KrkValue outDict = krk_dict_of(0,NULL,0);
	krk_push(outDict);
	krk_tableAddAll(&self->entries, AS_DICT(outDict));
	krk_tableAddAll(&them->entries, AS_DICT(outDict));
	return krk_pop();
}

KRK_Method(dict,__delitem__) {
	METHOD_TAKES_EXACTLY(1);
	if (!krk_tableDelete(&self->entries, argv[1])) {
		if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
		return krk_runtimeError(vm.exceptions->keyError, "%V", argv[1]);
	}
	return NONE_VAL();
}

KRK_Method(dict,__len__) {
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->entries.count);
}

KRK_Method(dict,__contains__) {
	METHOD_TAKES_EXACTLY(1);
	KrkValue v;
	return BOOLEAN_VAL(krk_tableGet(&self->entries, argv[1], &v));
}

KRK_Method(dict,capacity) {
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->entries.capacity);
}

KRK_Method(dict,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("{...}"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb,'{');

	size_t c = 0;
	size_t len = self->entries.capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &self->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c) pushStringBuilderStr(&sb, ", ", 2);
		c++;
		if (!krk_pushStringBuilderFormat(&sb, "%R", entry->key)) goto _error;
		pushStringBuilderStr(&sb, ": ", 2);
		if (!krk_pushStringBuilderFormat(&sb, "%R", entry->value)) goto _error;
	}

	pushStringBuilder(&sb,'}');
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

KRK_Method(dict,copy) {
	METHOD_TAKES_NONE();
	KrkValue dictOut = krk_dict_of(0,NULL,0);
	krk_push(dictOut);
	krk_tableAddAll(&self->entries, AS_DICT(dictOut));
	return krk_pop();
}

KRK_Method(dict,clear) {
	METHOD_TAKES_NONE();
	krk_freeTable(&self->entries);
	return NONE_VAL();
}

KRK_Method(dict,get) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	KrkValue out = NONE_VAL();
	if (argc > 2) out = argv[2];
	krk_tableGet(&self->entries, argv[1], &out);
	return out;
}

KRK_Method(dict,setdefault) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	KrkValue out = NONE_VAL();
	if (argc > 2) out = argv[2];

	/* TODO this is slow; use findEntry instead! */
	if (!krk_tableGet(&self->entries, argv[1], &out)) {
		krk_tableSet(&self->entries, argv[1], out);
	}

	return out;
}

KRK_Method(dict,update) {
	METHOD_TAKES_AT_MOST(1);
	if (argc > 1) {
		/* TODO sequence */
		CHECK_ARG(1,dict,KrkDict*,other);
		krk_tableAddAll(&other->entries, &self->entries);
	}
	if (hasKw) {
		krk_tableAddAll(AS_DICT(argv[argc]), &self->entries);
	}
	return NONE_VAL();
}

KRK_Method(dict,__ior__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,other);
	krk_tableAddAll(&other->entries, &self->entries);
	return argv[0];
}

FUNC_SIG(dictkeys,__init__);

KRK_Method(dict,keys) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->dictkeysClass);
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(dictkeys,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();
	return OBJECT_VAL(output);
}

FUNC_SIG(dictitems,__init__);

KRK_Method(dict,items) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->dictitemsClass);
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(dictitems,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();
	return OBJECT_VAL(output);
}

FUNC_SIG(dictvalues,__init__);

KRK_Method(dict,values) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->dictvaluesClass);
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(dictvalues,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();
	return OBJECT_VAL(output);
}

KrkValue krk_dict_nth_key_fast(size_t capacity, KrkTableEntry * entries, size_t index) {
	size_t found = 0;
	for (size_t i = 0; i < capacity; ++i) {
		if (IS_KWARGS(entries[i].key)) continue;
		if (found == index) return entries[i].key;
		found++;
	}
	return NONE_VAL();
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct DictItems *

static void _dictitems_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictItems*)self)->dict);
}

KRK_Method(dictitems,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,source);
	self->dict = argv[1];
	self->i = 0;
	return NONE_VAL();
}

KRK_Method(dictitems,__iter__) {
	METHOD_TAKES_NONE();
	self->i = 0;
	return argv[0];
}

KRK_Method(dictitems,__call__) {
	do {
		if (self->i >= AS_DICT(self->dict)->used) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			KrkTuple * outValue = krk_newTuple(2);
			krk_push(OBJECT_VAL(outValue));
			outValue->values.values[0] = AS_DICT(self->dict)->entries[self->i].key;
			outValue->values.values[1] = AS_DICT(self->dict)->entries[self->i].value;
			outValue->values.count = 2;
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

KRK_Method(dictitems,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("dictitems([...])"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb,"dictitems([",11);

	size_t c = 0;
	size_t len = AS_DICT(self->dict)->used;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(self->dict)->entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c) pushStringBuilderStr(&sb, ", ", 2);
		c++;
		pushStringBuilder(&sb,'(');
		if (!krk_pushStringBuilderFormat(&sb, "%R", entry->key)) goto _error;
		pushStringBuilderStr(&sb, ", ", 2);
		if (!krk_pushStringBuilderFormat(&sb, "%R", entry->value)) goto _error;
		pushStringBuilder(&sb,')');
	}

	pushStringBuilderStr(&sb,"])",2);
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct DictKeys *

static void _dictkeys_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictKeys*)self)->dict);
}

KRK_Method(dictkeys,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,source);
	self->dict = argv[1];
	self->i = 0;
	return NONE_VAL();
}

KRK_Method(dictkeys,__iter__) {
	METHOD_TAKES_NONE();
	self->i = 0;
	return argv[0];
}

KRK_Method(dictkeys,__call__) {
	METHOD_TAKES_NONE();
	do {
		if (self->i >= AS_DICT(self->dict)->used) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			krk_push(AS_DICT(self->dict)->entries[self->i].key);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

KRK_Method(dictkeys,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("dictkeys([...])"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb,"dictkeys([",10);

	size_t c = 0;
	size_t len = AS_DICT(self->dict)->used;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(self->dict)->entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c) pushStringBuilderStr(&sb, ", ", 2);
		c++;
		if (!krk_pushStringBuilderFormat(&sb, "%R", entry->key)) goto _error;
	}

	pushStringBuilderStr(&sb,"])",2);
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct DictValues *

static void _dictvalues_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictValues*)self)->dict);
}

KRK_Method(dictvalues,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,source);
	self->dict = argv[1];
	self->i = 0;
	return NONE_VAL();
}

KRK_Method(dictvalues,__iter__) {
	METHOD_TAKES_NONE();
	self->i = 0;
	return argv[0];
}

KRK_Method(dictvalues,__call__) {
	METHOD_TAKES_NONE();
	do {
		if (self->i >= AS_DICT(self->dict)->used) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			krk_push(AS_DICT(self->dict)->entries[self->i].value);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

KRK_Method(dictvalues,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("dictvalues([...])"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb,"dictvalues([",12);

	size_t c = 0;
	size_t len = AS_DICT(self->dict)->used;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(self->dict)->entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c) pushStringBuilderStr(&sb, ", ", 2);
		c++;
		if (!krk_pushStringBuilderFormat(&sb, "%R", entry->value)) goto _error;
	}

	pushStringBuilderStr(&sb,"])",2);
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

_noexport
void _createAndBind_dictClass(void) {
	KrkClass * dict = ADD_BASE_CLASS(vm.baseClasses->dictClass, "dict", vm.baseClasses->objectClass);
	dict->allocSize = sizeof(KrkDict);
	dict->_ongcscan = _dict_gcscan;
	dict->_ongcsweep = _dict_gcsweep;
	BIND_METHOD(dict,__init__);
	BIND_METHOD(dict,__repr__);
	BIND_METHOD(dict,__getitem__);
	BIND_METHOD(dict,__setitem__);
	BIND_METHOD(dict,__or__);
	BIND_METHOD(dict,__delitem__);
	BIND_METHOD(dict,__len__);
	BIND_METHOD(dict,__contains__);
	BIND_METHOD(dict,__ior__);
	BIND_METHOD(dict,__eq__);
	BIND_METHOD(dict,keys);
	BIND_METHOD(dict,items);
	BIND_METHOD(dict,values);
	BIND_METHOD(dict,capacity);
	BIND_METHOD(dict,copy);
	BIND_METHOD(dict,clear);
	BIND_METHOD(dict,get);
	BIND_METHOD(dict,setdefault);
	BIND_METHOD(dict,update);
	krk_defineNative(&dict->methods, "__iter__", FUNC_NAME(dict,keys));
	krk_defineNative(&dict->methods, "__class_getitem__", krk_GenericAlias)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	krk_attachNamedValue(&dict->methods, "__hash__", NONE_VAL());
	krk_finalizeClass(dict);
	KRK_DOC(dict, "Mapping of arbitrary keys to values.");

	KrkClass * dictitems = ADD_BASE_CLASS(vm.baseClasses->dictitemsClass, "dictitems", vm.baseClasses->objectClass);
	dictitems->allocSize = sizeof(struct DictItems);
	dictitems->_ongcscan = _dictitems_gcscan;
	BIND_METHOD(dictitems,__init__);
	BIND_METHOD(dictitems,__iter__);
	BIND_METHOD(dictitems,__call__);
	BIND_METHOD(dictitems,__repr__);
	krk_finalizeClass(dictitems);

	KrkClass * dictkeys = ADD_BASE_CLASS(vm.baseClasses->dictkeysClass, "dictkeys", vm.baseClasses->objectClass);
	dictkeys->allocSize = sizeof(struct DictKeys);
	dictkeys->_ongcscan = _dictkeys_gcscan;
	BIND_METHOD(dictkeys,__init__);
	BIND_METHOD(dictkeys,__iter__);
	BIND_METHOD(dictkeys,__call__);
	BIND_METHOD(dictkeys,__repr__);
	krk_finalizeClass(dictkeys);

	KrkClass * dictvalues = ADD_BASE_CLASS(vm.baseClasses->dictvaluesClass, "dictvalues", vm.baseClasses->objectClass);
	dictvalues->allocSize = sizeof(struct DictValues);
	dictvalues->_ongcscan = _dictvalues_gcscan;
	BIND_METHOD(dictvalues,__init__);
	BIND_METHOD(dictvalues,__iter__);
	BIND_METHOD(dictvalues,__call__);
	BIND_METHOD(dictvalues,__repr__);
	krk_finalizeClass(dictvalues);
}
