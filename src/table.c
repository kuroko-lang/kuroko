#include <stdio.h>
#include <string.h>
#include <kuroko/kuroko.h>
#include <kuroko/object.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/table.h>
#include <kuroko/vm.h>
#include <kuroko/threads.h>
#include <kuroko/util.h>

#define TABLE_MAX_LOAD 3 / 4

void krk_initTable(KrkTable * table) {
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

void krk_freeTable(KrkTable * table) {
	KRK_FREE_ARRAY(KrkTableEntry, table->entries, table->capacity);
	krk_initTable(table);
}

inline int krk_hashValue(KrkValue value, uint32_t *hashOut) {
	switch (KRK_VAL_TYPE(value)) {
		case KRK_VAL_BOOLEAN:
		case KRK_VAL_INTEGER:
		case KRK_VAL_NONE:
		case KRK_VAL_HANDLER:
		case KRK_VAL_KWARGS:
			*hashOut = (uint32_t)AS_INTEGER(value);
			return 0;
		case KRK_VAL_OBJECT:
			if (AS_OBJECT(value)->flags & KRK_OBJ_FLAGS_VALID_HASH) {
				*hashOut = AS_OBJECT(value)->hash;
				return 0;
			}
			break;
		default:
#ifndef KRK_NO_FLOAT
			*hashOut = (uint32_t)AS_FLOATING(value);
			return 0;
#else
			break;
#endif
	}
	KrkClass * type = krk_getType(value);
	if (type && type->_hash) {
		krk_push(value);
		KrkValue result = krk_callDirect(type->_hash, 1);
		if (!IS_INTEGER(result)) goto _unhashable;
		*hashOut = (uint32_t)AS_INTEGER(result);
		return 0;
	}
	if (IS_CLASS(value)) {
		*hashOut = INTEGER_VAL((int)(intptr_t)AS_OBJECT(value));
		return 0;
	}
_unhashable:
	if (IS_NONE(krk_currentThread.currentException))
		krk_runtimeError(vm.exceptions->typeError, "unhashable type: '%T'", value);
	return 1;
}

KrkTableEntry * krk_findEntry(KrkTableEntry * entries, size_t capacity, KrkValue key) {
	uint32_t index;
	if (krk_hashValue(key, &index)) {
		return NULL;
	}
	index &= (capacity-1);
	KrkTableEntry * tombstone = NULL;
	for (;;) {
		KrkTableEntry * entry = &entries[index];
		if (entry->key == KWARGS_VAL(0)) {
			return tombstone != NULL ? tombstone : entry;
		} else if (entry->key == KWARGS_VAL(1)) {
			if (tombstone == entry) return tombstone;
			if (tombstone == NULL) tombstone = entry;
		} else if (krk_valuesSameOrEqual(entry->key, key)) {
			return entry;
		}
		index = (index + 1) & (capacity-1);
	}
}

KrkTableEntry * krk_findEntryExact(KrkTableEntry * entries, size_t capacity, KrkValue key) {
	uint32_t index;
	if (krk_hashValue(key, &index)) {
		return NULL;
	}
	index &= (capacity-1);
	KrkTableEntry * tombstone = NULL;
	for (;;) {
		KrkTableEntry * entry = &entries[index];
		if (entry->key == KWARGS_VAL(0)) {
			return tombstone != NULL ? tombstone : entry;
		} else if (entry->key == KWARGS_VAL(1)) {
			if (tombstone == entry) return tombstone;
			if (tombstone == NULL) tombstone = entry;
		} else if (krk_valuesSame(entry->key, key)) {
			return entry;
		}
		index = (index + 1) & (capacity-1);
	}
}

#ifdef __TINYC__
int __builtin_clz(unsigned int x) {
	int i = 31;
	while (!(x & (1 << i)) && i >= 0) i--;
	return 31-i;
}
#endif

void krk_tableAdjustCapacity(KrkTable * table, size_t capacity) {
	if (capacity) {
		/* Fast power-of-two calculation */
		size_t powerOfTwoCapacity = __builtin_clz(1) - __builtin_clz(capacity);
		if ((1UL << powerOfTwoCapacity) != capacity) powerOfTwoCapacity++;
		capacity = (1UL << powerOfTwoCapacity);
	}

	KrkTableEntry * entries = KRK_ALLOCATE(KrkTableEntry, capacity);
	for (size_t i = 0; i < capacity; ++i) {
		entries[i].key = KWARGS_VAL(0);
		entries[i].value = KWARGS_VAL(0);
	}

	table->count = 0;
	for (size_t i = 0; i < table->capacity; ++i) {
		KrkTableEntry * entry = &table->entries[i];
		if (IS_KWARGS(entry->key)) continue;
		KrkTableEntry * dest = krk_findEntry(entries, capacity, entry->key);
		dest->key = entry->key;
		dest->value = entry->value;
		table->count++;
	}

	KRK_FREE_ARRAY(KrkTableEntry, table->entries, table->capacity);
	table->entries = entries;
	table->capacity = capacity;
}

int krk_tableSet(KrkTable * table, KrkValue key, KrkValue value) {
	if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
		size_t capacity = KRK_GROW_CAPACITY(table->capacity);
		krk_tableAdjustCapacity(table, capacity);
	}
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (!entry) return 0;
	int isNewKey = IS_KWARGS(entry->key);
	if (isNewKey) table->count++;
	entry->key = key;
	entry->value = value;
	return isNewKey;
}

int krk_tableSetIfExists(KrkTable * table, KrkValue key, KrkValue value) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (!entry) return 0;
	if (IS_KWARGS(entry->key)) return 0; /* Not found */
	entry->key = key;
	entry->value = value;
	return 1;
}

void krk_tableAddAll(KrkTable * from, KrkTable * to) {
	for (size_t i = 0; i < from->capacity; ++i) {
		KrkTableEntry * entry = &from->entries[i];
		if (!IS_KWARGS(entry->key)) {
			krk_tableSet(to, entry->key, entry->value);
		}
	}
}

int krk_tableGet(KrkTable * table, KrkValue key, KrkValue * value) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (!entry || IS_KWARGS(entry->key)) {
		return 0;
	} else {
		*value = entry->value;
		return 1;
	}
}

int krk_tableGet_fast(KrkTable * table, KrkString * str, KrkValue * value) {
	if (unlikely(table->count == 0)) return 0;
	uint32_t index = str->obj.hash & (table->capacity-1);
	KrkTableEntry * tombstone = NULL;
	for (;;) {
		KrkTableEntry * entry = &table->entries[index];
		if (entry->key == KWARGS_VAL(0)) {
			return 0;
		} else if (entry->key == KWARGS_VAL(1)) {
			if (tombstone == entry) return 0;
			if (tombstone == NULL) tombstone = entry;
		} else if (entry->key == OBJECT_VAL(str)) {
			*value = entry->value;
			return 1;
		}
		index = (index + 1) & (table->capacity-1);
	}
}

int krk_tableDelete(KrkTable * table, KrkValue key) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (!entry || IS_KWARGS(entry->key)) {
		return 0;
	}
	table->count--;
	entry->key = KWARGS_VAL(1);
	entry->value = KWARGS_VAL(0);
	return 1;
}

int krk_tableDeleteExact(KrkTable * table, KrkValue key) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntryExact(table->entries, table->capacity, key);
	if (!entry || IS_KWARGS(entry->key)) {
		return 0;
	}
	table->count--;
	entry->key = KWARGS_VAL(1);
	entry->value = KWARGS_VAL(0);
	return 1;
}

KrkString * krk_tableFindString(KrkTable * table, const char * chars, size_t length, uint32_t hash) {
	if (table->count == 0) return NULL;

	uint32_t index = hash & (table->capacity-1);
	KrkTableEntry * tombstone = NULL;
	for (;;) {
		KrkTableEntry * entry = &table->entries[index];
		if (entry->key == KWARGS_VAL(0)) {
			return NULL;
		} else if (entry->key == KWARGS_VAL(1)) {
			if (tombstone == entry) return NULL;
			if (tombstone == NULL) tombstone = entry;
		} else if (AS_STRING(entry->key)->length == length &&
		           AS_OBJECT(entry->key)->hash == hash &&
		           memcmp(AS_STRING(entry->key)->chars, chars, length) == 0) {
			return AS_STRING(entry->key);
		}
		index = (index + 1) & (table->capacity-1);
	}
}
