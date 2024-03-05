#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/object.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

/**
 * @brief Mutable unordered set of values.
 * @extends KrkInstance
 */
struct Set {
	KrkInstance inst;
	KrkTable entries;
};

#define IS_set(o) krk_isInstanceOf(o,KRK_BASE_CLASS(set))
#define AS_set(o) ((struct Set*)AS_OBJECT(o))

static void _set_gcscan(KrkInstance * self) {
	krk_markTable(&((struct Set*)self)->entries);
}

static void _set_gcsweep(KrkInstance * self) {
	krk_freeTable(&((struct Set*)self)->entries);
}

/**
 * @brief Iterator over the values in a set.
 * @extends KrkInstance
 */
struct SetIterator {
	KrkInstance inst;
	KrkValue set;
	size_t i;
};
#define IS_setiterator(o) krk_isInstanceOf(o,KRK_BASE_CLASS(setiterator))
#define AS_setiterator(o) ((struct SetIterator*)AS_OBJECT(o))

static void _setiterator_gcscan(KrkInstance * self) {
	krk_markValue(((struct SetIterator*)self)->set);
}

#define CURRENT_CTYPE struct Set *
#define CURRENT_NAME  self

static int _set_init_callback(void * context, const KrkValue * values, size_t count) {
	struct Set * self = context;
	for (size_t i = 0; i < count; ++i) {
		krk_tableSet(&self->entries, values[i], BOOLEAN_VAL(1));
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return 1;
	}
	return 0;
}

KRK_Method(set,__init__) {
	METHOD_TAKES_AT_MOST(1);
	krk_initTable(&self->entries);
	if (argc == 2) {
		if (krk_unpackIterable(argv[1], self, _set_init_callback)) return NONE_VAL();
	}
	return NONE_VAL();
}

KRK_Method(set,__contains__) {
	METHOD_TAKES_EXACTLY(1);
	KrkValue v;
	return BOOLEAN_VAL(krk_tableGet(&self->entries, argv[1], &v));
}

KRK_Method(set,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("{...}"));
	if (!self->entries.capacity) return OBJECT_VAL(S("set()"));
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
	}

	pushStringBuilder(&sb,'}');
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

KRK_Method(set,__and__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,struct Set*,them);

	KrkValue outSet = OBJECT_VAL(krk_newInstance(KRK_BASE_CLASS(set)));
	krk_push(outSet);
	FUNC_NAME(set,__init__)(1,&outSet,0);

	KrkClass * type = krk_getType(argv[1]);
	if (!type->_contains)
		return krk_runtimeError(vm.exceptions->typeError, "unsupported operand types for %s: '%T' and '%T'", "&", argv[0], argv[1]);

	for (size_t i = 0; i < self->entries.capacity; ++i) {
		KrkTableEntry * entry = &self->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;

		krk_push(argv[1]);
		krk_push(entry->key);
		KrkValue result = krk_callDirect(type->_contains, 2);

		if (IS_BOOLEAN(result) && AS_BOOLEAN(result)) {
			krk_tableSet(&AS_set(outSet)->entries, entry->key, BOOLEAN_VAL(1));
		}
	}

	return krk_pop();
}

KRK_Method(set,__xor__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,struct Set*,them);

	KrkValue outSet = OBJECT_VAL(krk_newInstance(KRK_BASE_CLASS(set)));
	krk_push(outSet);
	FUNC_NAME(set,__init__)(1,&outSet,0);

	KrkClass * type = krk_getType(argv[1]);
	if (!type->_contains)
		return krk_runtimeError(vm.exceptions->typeError, "unsupported operand types for %s: '%T' and '%T'", "&", argv[0], argv[1]);

	for (size_t i = 0; i < self->entries.capacity; ++i) {
		KrkTableEntry * entry = &self->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;

		krk_push(argv[1]);
		krk_push(entry->key);
		KrkValue result = krk_callDirect(type->_contains, 2);

		if (IS_BOOLEAN(result) && !AS_BOOLEAN(result)) {
			krk_tableSet(&AS_set(outSet)->entries, entry->key, BOOLEAN_VAL(1));
		}
	}

	/* Ours better have something... */
	type = krk_getType(argv[0]);

	for (size_t i = 0; i < them->entries.capacity; ++i) {
		KrkTableEntry * entry = &them->entries.entries[i];
		if (IS_KWARGS(entry->key)) continue;

		krk_push(argv[0]);
		krk_push(entry->key);
		KrkValue result = krk_callDirect(type->_contains, 2);

		if (IS_BOOLEAN(result) && !AS_BOOLEAN(result)) {
			krk_tableSet(&AS_set(outSet)->entries, entry->key, BOOLEAN_VAL(1));
		}
	}

	return krk_pop();
}


KRK_Method(set,__or__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,struct Set*,them);

	KrkValue outSet = OBJECT_VAL(krk_newInstance(KRK_BASE_CLASS(set)));
	krk_push(outSet);
	FUNC_NAME(set,__init__)(1,&outSet,0);

	krk_tableAddAll(&self->entries, &AS_set(outSet)->entries);
	krk_tableAddAll(&them->entries, &AS_set(outSet)->entries);

	return krk_pop();
}

KRK_Method(set,__len__) {
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->entries.count);
}

KRK_Method(set,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_set(argv[1]))
		return NOTIMPL_VAL();
	CHECK_ARG(1,set,struct Set*,them);
	if (self->entries.count != them->entries.count)
		return BOOLEAN_VAL(0);

	KrkValue v;

	for (unsigned int i = 0; i < self->entries.capacity; ++i) {
		if (IS_KWARGS(self->entries.entries[i].key)) continue;
		if (!krk_tableGet(&them->entries, self->entries.entries[i].key, &v)) return BOOLEAN_VAL(0);
	}

	return BOOLEAN_VAL(1);
}

/* Strict subset */
KRK_Method(set,__lt__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_set(argv[1]))
		return NOTIMPL_VAL();
	struct Set * them = AS_set(argv[1]);
	if (self->entries.count == them->entries.count)
		return BOOLEAN_VAL(0);
	KrkValue v;
	for (unsigned int i = 0; i < self->entries.capacity; ++i) {
		if (IS_KWARGS(self->entries.entries[i].key)) continue;
		if (!krk_tableGet(&them->entries, self->entries.entries[i].key, &v)) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

/* Subset or equal to */
KRK_Method(set,__le__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_set(argv[1]))
		return NOTIMPL_VAL();
	struct Set * them = AS_set(argv[1]);
	KrkValue v;
	for (unsigned int i = 0; i < self->entries.capacity; ++i) {
		if (IS_KWARGS(self->entries.entries[i].key)) continue;
		if (!krk_tableGet(&them->entries, self->entries.entries[i].key, &v)) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

/* Strict superset */
KRK_Method(set,__gt__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_set(argv[1]))
		return NOTIMPL_VAL();
	struct Set * them = AS_set(argv[1]);
	if (self->entries.count == them->entries.count)
		return BOOLEAN_VAL(0);
	KrkValue v;
	for (unsigned int i = 0; i < them->entries.capacity; ++i) {
		if (IS_KWARGS(them->entries.entries[i].key)) continue;
		if (!krk_tableGet(&self->entries, them->entries.entries[i].key, &v)) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

KRK_Method(set,__ge__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_set(argv[1]))
		return NOTIMPL_VAL();
	struct Set * them = AS_set(argv[1]);
	KrkValue v;
	for (unsigned int i = 0; i < them->entries.capacity; ++i) {
		if (IS_KWARGS(them->entries.entries[i].key)) continue;
		if (!krk_tableGet(&self->entries, them->entries.entries[i].key, &v)) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}


KRK_Method(set,add) {
	METHOD_TAKES_EXACTLY(1);
	krk_tableSet(&self->entries, argv[1], BOOLEAN_VAL(1));
	return NONE_VAL();
}

KRK_Method(set,remove) {
	METHOD_TAKES_EXACTLY(1);
	if (!krk_tableDelete(&self->entries, argv[1]))
		return krk_runtimeError(vm.exceptions->keyError, "key error");
	return NONE_VAL();
}

KRK_Method(set,discard) {
	METHOD_TAKES_EXACTLY(1);
	krk_tableDelete(&self->entries, argv[1]);
	return NONE_VAL();
}

KRK_Method(set,clear) {
	METHOD_TAKES_NONE();
	krk_freeTable(&self->entries);
	krk_initTable(&self->entries);
	return NONE_VAL();
}

KRK_Method(set,update) {
	METHOD_TAKES_AT_MOST(1);
	if (argc > 1) {
		if (IS_set(argv[1])) {
			krk_tableAddAll(&AS_set(argv[1])->entries, &self->entries);
		} else {
			if (krk_unpackIterable(argv[1], self, _set_init_callback)) return NONE_VAL();
		}
	}
	return NONE_VAL();
}

FUNC_SIG(setiterator,__init__);

KRK_Method(set,__iter__) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(KRK_BASE_CLASS(setiterator));
	krk_push(OBJECT_VAL(output));
	FUNC_NAME(setiterator,__init__)(2,(KrkValue[]){krk_peek(0), argv[0]}, 0);
	return krk_pop();
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE struct SetIterator *

KRK_Method(setiterator,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,set,void*,source);
	self->set = argv[1];
	self->i = 0;
	return NONE_VAL();
}

KRK_Method(setiterator,__call__) {
	METHOD_TAKES_NONE();

	if (unlikely(!IS_set(self->set))) return argv[0];

	do {
		if (self->i >= AS_set(self->set)->entries.capacity) return argv[0];
		if (!IS_KWARGS(AS_set(self->set)->entries.entries[self->i].key)) {
			krk_push(AS_set(self->set)->entries.entries[self->i].key);
			self->i++;
			return krk_pop();
		}
		self->i++;
	} while (1);
}

KrkValue krk_set_of(int argc, const KrkValue argv[], int hasKw) {
	KrkValue outSet = OBJECT_VAL(krk_newInstance(KRK_BASE_CLASS(set)));
	krk_push(outSet);
	krk_initTable(&AS_set(outSet)->entries);

	while (argc) {
		krk_tableSet(&AS_set(outSet)->entries, argv[argc-1], BOOLEAN_VAL(1));
		argc--;
	}

	return krk_pop();
}

_noexport
void _createAndBind_setClass(void) {
	KrkClass * set = krk_makeClass(vm.builtins, &KRK_BASE_CLASS(set), "set", vm.baseClasses->objectClass);
	set->allocSize = sizeof(struct Set);
	set->_ongcscan = _set_gcscan;
	set->_ongcsweep = _set_gcsweep;
	BIND_METHOD(set,__init__);
	BIND_METHOD(set,__repr__);
	BIND_METHOD(set,__len__);
	BIND_METHOD(set,__eq__);
	BIND_METHOD(set,__lt__);
	BIND_METHOD(set,__gt__);
	BIND_METHOD(set,__le__);
	BIND_METHOD(set,__ge__);
	BIND_METHOD(set,__and__);
	BIND_METHOD(set,__or__);
	BIND_METHOD(set,__xor__);
	BIND_METHOD(set,__contains__);
	BIND_METHOD(set,__iter__);
	KRK_DOC(BIND_METHOD(set,add),
		"@brief Add an element to the set.\n"
		"@arguments value\n\n"
		"Adds the given @p value to the set. @p value must be hashable.");
	KRK_DOC(BIND_METHOD(set,remove),
		"@brief Remove an element from the set.\n"
		"@arguments value\n\n"
		"Removes @p value from the set, raising @ref KeyError if it is not a member of the set.");
	KRK_DOC(BIND_METHOD(set,discard),
		"@brief Remove an element from the set, quietly.\n"
		"@arguments value\n\n"
		"Removes @p value from the set, without raising an exception if it is not a member.");
	KRK_DOC(BIND_METHOD(set,clear),
		"@brief Empty the set.\n\n"
		"Removes all elements from the set, in-place.");
	BIND_METHOD(set,update);
	krk_attachNamedValue(&set->methods, "__hash__", NONE_VAL());
	krk_finalizeClass(set);

	KrkClass * setiterator = krk_makeClass(vm.builtins, &KRK_BASE_CLASS(setiterator), "setiterator", vm.baseClasses->objectClass);
	setiterator->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	setiterator->allocSize = sizeof(struct SetIterator);
	setiterator->_ongcscan = _setiterator_gcscan;
	BIND_METHOD(setiterator,__init__);
	BIND_METHOD(setiterator,__call__);
	krk_finalizeClass(setiterator);
}
