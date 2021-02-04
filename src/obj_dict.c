#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

#define CHECK_DICT_FAST() if (unlikely(argc < 0 || !IS_INSTANCE(argv[0]) || \
	(AS_INSTANCE(argv[0])->_class != vm.baseClasses.dictClass && !krk_isInstanceOf(argv[0], vm.baseClasses.dictClass)))) \
		return krk_runtimeError(vm.exceptions.typeError, "expected dict")

/**
 * Exposed method called to produce dictionaries from {expr: expr, ...} sequences in managed code.
 * Presented in the global namespace as dictOf(...). Expects arguments as key,value,key,value...
 */
KrkValue krk_dict_of(int argc, KrkValue argv[]) {
	if (argc % 2 != 0) return krk_runtimeError(vm.exceptions.argumentError, "Expected even number of arguments to dictOf");
	KrkInstance * outDict = krk_newInstance(vm.baseClasses.dictClass);
	krk_push(OBJECT_VAL(outDict));
	krk_initTable(&((KrkDict*)outDict)->entries);
	for (int ind = 0; ind < argc; ind += 2) {
		krk_tableSet(&((KrkDict*)outDict)->entries, argv[ind], argv[ind+1]);
	}
	return krk_pop();
}

/**
 * dict.__init__()
 */
static KrkValue _dict_init(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	krk_initTable(&((KrkDict *)AS_OBJECT(argv[0]))->entries);
	return argv[0];
}

static void _dict_gcscan(KrkInstance * self) {
	krk_markTable(&((KrkDict*)self)->entries);
}

static void _dict_gcsweep(KrkInstance * self) {
	krk_freeTable(&((KrkDict*)self)->entries);
}

/**
 * dict.__get__(key)
 */
static KrkValue _dict_get(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	KrkValue out;
	if (!krk_tableGet(AS_DICT(argv[0]), argv[1], &out)) return krk_runtimeError(vm.exceptions.keyError, "key error");
	return out;
}

/**
 * dict.__set__(key, value)
 */
static KrkValue _dict_set(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 3) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	krk_tableSet(AS_DICT(argv[0]), argv[1], argv[2]);
	return NONE_VAL();
}

static KrkValue _dict_or(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	if (!krk_isInstanceOf(argv[0],vm.baseClasses.dictClass) ||
	    !krk_isInstanceOf(argv[1],vm.baseClasses.dictClass))
		return krk_runtimeError(vm.exceptions.typeError, "Can not merge '%s' and '%s'.",
			krk_typeName(argv[0]),
			krk_typeName(argv[1]));

	KrkValue outDict = krk_dict_of(0,NULL);
	krk_push(outDict);

	/* Why is this src->dest... Should change that... */
	krk_tableAddAll(AS_DICT(argv[0]), AS_DICT(outDict));
	krk_tableAddAll(AS_DICT(argv[1]), AS_DICT(outDict));

	return krk_pop();
}

/**
 * dict.__delitem__
 */
static KrkValue _dict_delitem(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 2) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	if (!krk_tableDelete(AS_DICT(argv[0]), argv[1])) {
		KrkClass * type = krk_getType(argv[1]);
		if (type->_reprer) {
			krk_push(argv[1]);
			KrkValue asString = krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0);
			if (IS_STRING(asString)) return krk_runtimeError(vm.exceptions.keyError, "%s", AS_CSTRING(asString));
		}
		return krk_runtimeError(vm.exceptions.keyError, "(Unrepresentable value)");
	}
	return NONE_VAL();
}

/**
 * dict.__len__()
 */
static KrkValue _dict_len(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	return INTEGER_VAL(AS_DICT(argv[0])->count);
}

/**
 * dict.__contains__()
 */
static KrkValue _dict_contains(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	KrkValue _unused;
	return BOOLEAN_VAL(krk_tableGet(AS_DICT(argv[0]), argv[1], &_unused));
}

/**
 * dict.capacity()
 */
static KrkValue _dict_capacity(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	if (argc < 1) return krk_runtimeError(vm.exceptions.argumentError, "wrong number of arguments");
	return INTEGER_VAL(AS_DICT(argv[0])->capacity);
}

static KrkValue _dict_repr(int argc, KrkValue argv[]) {
	CHECK_DICT_FAST();
	KrkValue self = argv[0];
	if (AS_OBJECT(self)->inRepr) return OBJECT_VAL(S("{...}"));
	krk_push(OBJECT_VAL(S("{")));

	AS_OBJECT(self)->inRepr = 1;

	size_t c = 0;
	size_t len = AS_DICT(argv[0])->capacity;
	for (size_t i = 0; i < len; ++i) {
		KrkTableEntry * entry = &AS_DICT(argv[0])->entries[i];

		if (IS_KWARGS(entry->key)) continue;

		if (c > 0) {
			krk_push(OBJECT_VAL(S(", ")));
			krk_addObjects();
		}

		c++;

		KrkClass * type = krk_getType(entry->key);
		krk_push(entry->key);
		krk_push(krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0));
		krk_addObjects();

		krk_push(OBJECT_VAL(S(": ")));
		krk_addObjects();

		type = krk_getType(entry->value);
		krk_push(entry->value);
		krk_push(krk_callSimple(OBJECT_VAL(type->_reprer), 1, 0));
		krk_addObjects();
	}

	AS_OBJECT(self)->inRepr = 0;

	krk_push(OBJECT_VAL(S("}")));
	krk_addObjects();
	return krk_pop();

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


struct DictItems {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

static void _dictitems_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictItems*)self)->dict);
}

static KrkValue _dictitems_init(int argc, KrkValue argv[]) {
	struct DictItems * self = (struct DictItems*)AS_OBJECT(argv[0]);
	self->dict = argv[1];
	self->i = 0;
	return argv[0];
}

static KrkValue _dictitems_iter(int argc, KrkValue argv[]) {
	/* Reset index and return self as iteration object */
	struct DictItems * self = (struct DictItems*)AS_OBJECT(argv[0]);
	self->i = 0;
	return argv[0];
}

static KrkValue _dictitems_call(int argc, KrkValue argv[]) {
	struct DictItems * self = (struct DictItems*)AS_OBJECT(argv[0]);
	do {
		if (self->i >= AS_DICT(self->dict)->capacity) return argv[0];
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

/* TODO: dictitems could really use a nice repr */
static KrkValue _dict_items(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.dictitemsClass);
	krk_push(OBJECT_VAL(output));
	_dictitems_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();
	return OBJECT_VAL(output);
}

struct DictKeys {
	KrkInstance inst;
	KrkValue dict;
	size_t i;
};

static void _dictkeys_gcscan(KrkInstance * self) {
	krk_markValue(((struct DictKeys*)self)->dict);
}

static KrkValue _dictkeys_init(int argc, KrkValue argv[]) {
	struct DictKeys * self = (struct DictKeys*)AS_OBJECT(argv[0]);
	self->dict = argv[1];
	self->i = 0;
	return argv[0];
}

static KrkValue _dictkeys_iter(int argc, KrkValue argv[]) {
	/* reset indext and return self as iteration object */
	struct DictKeys * self = (struct DictKeys*)AS_OBJECT(argv[0]);
	self->i = 0;
	return argv[0];
}

static KrkValue _dictkeys_call(int argc, KrkValue argv[]) {
	struct DictKeys * self = (struct DictKeys*)AS_OBJECT(argv[0]);
	do {
		if (self->i >= AS_DICT(self->dict)->capacity) return argv[0];
		if (!IS_KWARGS(AS_DICT(self->dict)->entries[self->i].key)) {
			krk_push(AS_DICT(self->dict)->entries[self->i].key);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

/* TODO: dictkeys could really use a nice repr */
static KrkValue _dict_keys(int argc, KrkValue argv[]) {
	KrkInstance * output = krk_newInstance(vm.baseClasses.dictkeysClass);
	krk_push(OBJECT_VAL(output));
	_dictkeys_init(2, (KrkValue[]){krk_peek(0), argv[0]});
	krk_pop();
	return OBJECT_VAL(output);
}

_noexport
void _createAndBind_dictClass(void) {
	ADD_BASE_CLASS(vm.baseClasses.dictClass, "dict", vm.objectClass);
	vm.baseClasses.dictClass->allocSize = sizeof(KrkDict);
	vm.baseClasses.dictClass->_ongcscan = _dict_gcscan;
	vm.baseClasses.dictClass->_ongcsweep = _dict_gcsweep;
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__init__", _dict_init);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__str__", _dict_repr);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__repr__", _dict_repr);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__get__", _dict_get);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__set__", _dict_set);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__or__", _dict_or);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__delitem__", _dict_delitem);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__len__", _dict_len);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".__contains__", _dict_contains);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".keys", _dict_keys);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".items", _dict_items);
	krk_defineNative(&vm.baseClasses.dictClass->methods, ".capacity", _dict_capacity);
	krk_finalizeClass(vm.baseClasses.dictClass);
	vm.baseClasses.dictClass->docstring = S("Mapping of arbitrary keys to values.");

	ADD_BASE_CLASS(vm.baseClasses.dictitemsClass, "dictitems", vm.objectClass);
	vm.baseClasses.dictitemsClass->allocSize = sizeof(struct DictItems);
	vm.baseClasses.dictitemsClass->_ongcscan = _dictitems_gcscan;
	krk_defineNative(&vm.baseClasses.dictitemsClass->methods, ".__init__", _dictitems_init);
	krk_defineNative(&vm.baseClasses.dictitemsClass->methods, ".__iter__", _dictitems_iter);
	krk_defineNative(&vm.baseClasses.dictitemsClass->methods, ".__call__", _dictitems_call);
	krk_finalizeClass(vm.baseClasses.dictitemsClass);

	ADD_BASE_CLASS(vm.baseClasses.dictkeysClass, "dictkeys", vm.objectClass);
	vm.baseClasses.dictkeysClass->allocSize = sizeof(struct DictKeys);
	vm.baseClasses.dictkeysClass->_ongcscan = _dictkeys_gcscan;
	krk_defineNative(&vm.baseClasses.dictkeysClass->methods, ".__init__", _dictkeys_init);
	krk_defineNative(&vm.baseClasses.dictkeysClass->methods, ".__iter__", _dictkeys_iter);
	krk_defineNative(&vm.baseClasses.dictkeysClass->methods, ".__call__", _dictkeys_call);
	krk_finalizeClass(vm.baseClasses.dictkeysClass);
}
