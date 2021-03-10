#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define KEY_ERROR(value) {\
	KrkClass * type = krk_getType(value); \
	krk_push(value); \
	KrkValue asString = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0); \
	if (IS_STRING(asString)) return krk_runtimeError(vm.exceptions->keyError, "%s", AS_CSTRING(asString)); \
	return krk_runtimeError(vm.exceptions->keyError, "key error"); }

/**
 * Exposed method called to produce dictionaries from `{expr: expr, ...}` sequences in managed code.
 * Presented in the global namespace as `dictOf(...)`. Expects arguments as `key,value,key,value`...
 */
KrkValue krk_dict_of(int argc, KrkValue argv[], int hasKw) {
	if (argc % 2 != 0) return krk_runtimeError(vm.exceptions->argumentError, "Expected even number of arguments to dictOf");
	KrkInstance * outDict = krk_newInstance(vm.baseClasses->dictClass);
	krk_push(OBJECT_VAL(outDict));
	krk_initTable(&((KrkDict*)outDict)->entries);
	for (int ind = 0; ind < argc; ind += 2) {
		krk_tableSet(&((KrkDict*)outDict)->entries, argv[ind], argv[ind+1]);
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

KRK_METHOD(dict,__init__,{
	METHOD_TAKES_NONE();
	krk_initTable(&self->entries);
	return argv[0];
})

KRK_METHOD(dict,__getitem__,{
	METHOD_TAKES_EXACTLY(1);
	KrkValue out;
	if (!krk_tableGet(&self->entries, argv[1], &out))
		KEY_ERROR(argv[1]);
	return out;
})

KRK_METHOD(dict,__setitem__,{
	METHOD_TAKES_EXACTLY(2);
	krk_tableSet(&self->entries, argv[1], argv[2]);
})

KRK_METHOD(dict,__or__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,them);
	KrkValue outDict = krk_dict_of(0,NULL,0);
	krk_push(outDict);
	krk_tableAddAll(&self->entries, AS_DICT(outDict));
	krk_tableAddAll(&them->entries, AS_DICT(outDict));
	return krk_pop();
})

KRK_METHOD(dict,__delitem__,{
	METHOD_TAKES_EXACTLY(1);
	if (!krk_tableDelete(&self->entries, argv[1])) {
		KEY_ERROR(argv[1]);
	}
})

KRK_METHOD(dict,__len__,{
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->entries.count);
})

KRK_METHOD(dict,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	KrkValue _unused;
	return BOOLEAN_VAL(krk_tableGet(&self->entries, argv[1], &_unused));
})

KRK_METHOD(dict,capacity,{
	METHOD_TAKES_EXACTLY(1);
	return INTEGER_VAL(self->entries.capacity);
})

KRK_METHOD(dict,__repr__,{
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("{...}"));
	((KrkObj*)self)->inRepr = 1;
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb,'{');

	size_t c = 0;
	size_t len = self->entries.capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &self->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c > 0) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
		c++;

		{
			KrkClass * type = krk_getType(entry->key);
			krk_push(entry->key);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(result)) {
				pushStringBuilderStr(&sb, AS_CSTRING(result), AS_STRING(result)->length);
			}
		}

		pushStringBuilderStr(&sb, ": ", 2);

		{
			KrkClass * type = krk_getType(entry->value);
			krk_push(entry->value);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(result)) {
				pushStringBuilderStr(&sb, AS_CSTRING(result), AS_STRING(result)->length);
			}
		}
	}

	pushStringBuilder(&sb,'}');
	((KrkObj*)self)->inRepr = 0;
	return finishStringBuilder(&sb);
})

KRK_METHOD(dict,copy,{
	METHOD_TAKES_NONE();
	KrkValue dictOut = krk_dict_of(0,NULL,0);
	krk_push(dictOut);
	krk_tableAddAll(&self->entries, AS_DICT(dictOut));
	return krk_pop();
})

KRK_METHOD(dict,clear,{
	METHOD_TAKES_NONE();
	krk_freeTable(&self->entries);
})

KRK_METHOD(dict,get,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	KrkValue out = NONE_VAL();
	if (argc > 2) out = argv[2];
	krk_tableGet(&self->entries, argv[1], &out);
	return out;
})

KRK_METHOD(dict,setdefault,{
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(2);
	KrkValue out = NONE_VAL();
	if (argc > 2) out = argv[2];

	/* TODO this is slow; use findEntry instead! */
	if (!krk_tableGet(&self->entries, argv[1], &out)) {
		krk_tableSet(&self->entries, argv[1], out);
	}

	return out;
})

KRK_METHOD(dict,update,{
	METHOD_TAKES_AT_MOST(1);
	if (argc > 1) {
		/* TODO sequence */
		CHECK_ARG(1,dict,KrkDict*,other);
		krk_tableAddAll(&other->entries, &self->entries);
	}
	if (hasKw) {
		krk_tableAddAll(AS_DICT(argv[argc]), &self->entries);
	}
})

FUNC_SIG(dictkeys,__init__);

KRK_METHOD(dict,keys,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->dictkeysClass);
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(dictkeys,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();
	return OBJECT_VAL(output);
})

FUNC_SIG(dictitems,__init__);

KRK_METHOD(dict,items,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->dictitemsClass);
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(dictitems,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();
	return OBJECT_VAL(output);
})

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

KRK_METHOD(dictitems,__init__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,source);
	self->dict = argv[1];
	self->i = 0;
	return argv[0];
})

KRK_METHOD(dictitems,__iter__,{
	METHOD_TAKES_NONE();
	self->i = 0;
	return argv[0];
})

KRK_METHOD(dictitems,__call__,{
	do {
		if (self->i >= AS_DICT(self->dict)->capacity) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			KrkTuple * outValue = krk_newTuple(2);
			krk_push(OBJECT_VAL(outValue));
			outValue->values.values[0] = AS_DICT(self->dict)->entries[self->i].key;
			outValue->values.values[1] = AS_DICT(self->dict)->entries[self->i].value;
			outValue->values.count = 2;
			krk_tupleUpdateHash(outValue);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
})

KRK_METHOD(dictitems,__repr__,{
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("dictitems([...])"));
	((KrkObj*)self)->inRepr = 1;
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb,"dictitems([",11);

	size_t c = 0;
	size_t len = AS_DICT(self->dict)->capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(self->dict)->entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c > 0) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
		c++;

		pushStringBuilder(&sb,'(');

		{
			KrkClass * type = krk_getType(entry->key);
			krk_push(entry->key);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(result)) {
				pushStringBuilderStr(&sb, AS_CSTRING(result), AS_STRING(result)->length);
			}
		}

		pushStringBuilderStr(&sb, ", ", 2);

		{
			KrkClass * type = krk_getType(entry->value);
			krk_push(entry->value);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(result)) {
				pushStringBuilderStr(&sb, AS_CSTRING(result), AS_STRING(result)->length);
			}
		}

		pushStringBuilder(&sb,')');
	}

	pushStringBuilderStr(&sb,"])",2);
	((KrkObj*)self)->inRepr = 0;
	return finishStringBuilder(&sb);
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct DictKeys *

static void _dictkeys_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictKeys*)self)->dict);
}

KRK_METHOD(dictkeys,__init__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,dict,KrkDict*,source);
	self->dict = argv[1];
	self->i = 0;
	return argv[0];
})

KRK_METHOD(dictkeys,__iter__,{
	METHOD_TAKES_NONE();
	self->i = 0;
	return argv[0];
})

KRK_METHOD(dictkeys,__call__,{
	METHOD_TAKES_NONE();
	do {
		if (self->i >= AS_DICT(self->dict)->capacity) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			krk_push(AS_DICT(self->dict)->entries[self->i].key);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
})

KRK_METHOD(dictkeys,__repr__,{
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->inRepr) return OBJECT_VAL(S("dictkeys([...])"));
	((KrkObj*)self)->inRepr = 1;
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb,"dictkeys([",10);

	size_t c = 0;
	size_t len = AS_DICT(self->dict)->capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(self->dict)->entries[i];
		if (IS_KWARGS(entry->key)) continue;
		if (c > 0) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
		c++;

		{
			KrkClass * type = krk_getType(entry->key);
			krk_push(entry->key);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(result)) {
				pushStringBuilderStr(&sb, AS_CSTRING(result), AS_STRING(result)->length);
			}
		}
	}

	pushStringBuilderStr(&sb,"])",2);
	((KrkObj*)self)->inRepr = 0;
	return finishStringBuilder(&sb);
})

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
	BIND_METHOD(dict,keys);
	BIND_METHOD(dict,items);
	BIND_METHOD(dict,capacity);
	BIND_METHOD(dict,copy);
	BIND_METHOD(dict,clear);
	BIND_METHOD(dict,get);
	BIND_METHOD(dict,setdefault);
	BIND_METHOD(dict,update);
	krk_defineNative(&dict->methods, ".__str__", FUNC_NAME(dict,__repr__));
	krk_finalizeClass(dict);
	KRK_DOC(dict, "Mapping of arbitrary keys to values.");

	BUILTIN_FUNCTION("dictOf", krk_dict_of, "Convert argument sequence to dict object.");

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
}
