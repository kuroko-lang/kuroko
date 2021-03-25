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

#define TABLE_MAX_LOAD 0.75

void krk_initTable(KrkTable * table) {
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

void krk_freeTable(KrkTable * table) {
	FREE_ARRAY(KrkTableEntry, table->entries, table->capacity);
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
			*hashOut = (uint32_t)AS_FLOATING(value);
			return 0;
	}
	KrkClass * type = krk_getType(value);
	if (type && type->_hash) {
		krk_push(value);
		KrkValue result = krk_callSimple(OBJECT_VAL(type->_hash), 1, 0);
		if (!IS_INTEGER(result)) goto _unhashable;
		*hashOut = (uint32_t)AS_INTEGER(result);
		return 0;
	}
_unhashable:
	krk_runtimeError(vm.exceptions->typeError, "unhashable type: '%s'", krk_typeName(value));
	return 1;
}

KrkTableEntry * krk_findEntry(KrkTableEntry * entries, size_t capacity, KrkValue key) {
	uint32_t index;
	if (krk_hashValue(key, &index)) {
		return NULL;
	}
	index %= capacity;
	KrkTableEntry * tombstone = NULL;
	for (;;) {
		KrkTableEntry * entry = &entries[index];
		if (IS_KWARGS(entry->key)) {
			if (IS_NONE(entry->value)) {
				return tombstone != NULL ? tombstone : entry;
			} else {
				if (tombstone == NULL) tombstone = entry;
			}
		} else if (krk_valuesEqual(entry->key, key)) {
			return entry;
		}
		index = (index + 1) % capacity;
	}
}

void krk_tableAdjustCapacity(KrkTable * table, size_t capacity) {
	KrkTableEntry * entries = ALLOCATE(KrkTableEntry, capacity);
	for (size_t i = 0; i < capacity; ++i) {
		entries[i].key = KWARGS_VAL(0);
		entries[i].value = NONE_VAL();
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

	FREE_ARRAY(KrkTableEntry, table->entries, table->capacity);
	table->entries = entries;
	table->capacity = capacity;
}

int krk_tableSet(KrkTable * table, KrkValue key, KrkValue value) {
	if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
		size_t capacity = GROW_CAPACITY(table->capacity);
		krk_tableAdjustCapacity(table, capacity);
	}
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (!entry) return 0;
	int isNewKey = IS_KWARGS(entry->key);
	if (isNewKey && IS_NONE(entry->value)) table->count++;
	entry->key = key;
	entry->value = value;
	return isNewKey;
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
	uint32_t index = str->obj.hash % table->capacity;
	for (;;) {
		KrkTableEntry * entry = &table->entries[index];
		if (IS_KWARGS(entry->key)) return 0;
		if (IS_STRING(entry->key) && AS_STRING(entry->key) == str) {
			*value = entry->value;
			return 1;
		}
		index = (index + 1) % table->capacity;
	}
}

int krk_tableDelete(KrkTable * table, KrkValue key) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (!entry || IS_KWARGS(entry->key)) {
		return 0;
	}
	entry->key = KWARGS_VAL(0);
	entry->value = BOOLEAN_VAL(1);
	return 1;
}

KrkString * krk_tableFindString(KrkTable * table, const char * chars, size_t length, uint32_t hash) {
	if (table->count == 0) return NULL;

	uint32_t index = hash % table->capacity;
	for (;;) {
		KrkTableEntry * entry = &table->entries[index];
		if (IS_KWARGS(entry->key)) {
			if (IS_NONE(entry->value)) {
				return NULL;
			}
		} else if (AS_STRING(entry->key)->length == length &&
		           AS_OBJECT(entry->key)->hash == hash &&
		           memcmp(AS_STRING(entry->key)->chars, chars, length) == 0) {
			return AS_STRING(entry->key);
		}
		index = (index + 1) % table->capacity;
	}
}
