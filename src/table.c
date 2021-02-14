#include <stdio.h>
#include <string.h>
#include "kuroko.h"
#include "object.h"
#include "value.h"
#include "memory.h"
#include "table.h"
#include "vm.h"
#include "threads.h"

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

inline uint32_t krk_hashValue(KrkValue value) {
	switch (value.type) {
		case VAL_INTEGER:  return (uint32_t)(AS_INTEGER(value));
		case VAL_FLOATING: return (uint32_t)(AS_FLOATING(value) * 1000); /* arbitrary; what's a good way to hash floats? */
		case VAL_BOOLEAN:  return (uint32_t)(AS_BOOLEAN(value));
		case VAL_NONE:     return 0;
		case VAL_OBJECT:   return (uint32_t)(AS_OBJECT(value))->hash;
		default: return 0;
	}
}

KrkTableEntry * krk_findEntry(KrkTableEntry * entries, size_t capacity, KrkValue key) {
	uint32_t index = krk_hashValue(key) % capacity;
	KrkTableEntry * tombstone = NULL;
	for (;;) {
		KrkTableEntry * entry = &entries[index];
		if (entry->key.type == VAL_KWARGS) {
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

static void adjustCapacity(KrkTable * table, size_t capacity) {
	KrkTableEntry * entries = ALLOCATE(KrkTableEntry, capacity);
	for (size_t i = 0; i < capacity; ++i) {
		entries[i].key = KWARGS_VAL(0);
		entries[i].value = NONE_VAL();
	}

	table->count = 0;
	for (size_t i = 0; i < table->capacity; ++i) {
		KrkTableEntry * entry = &table->entries[i];
		if (entry->key.type == VAL_KWARGS) continue;
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
		adjustCapacity(table, capacity);
	}
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	int isNewKey = entry->key.type == VAL_KWARGS;
	if (isNewKey && IS_NONE(entry->value)) table->count++;
	entry->key = key;
	entry->value = value;
	return isNewKey;
}

void krk_tableAddAll(KrkTable * from, KrkTable * to) {
	for (size_t i = 0; i < from->capacity; ++i) {
		KrkTableEntry * entry = &from->entries[i];
		if (entry->key.type != VAL_KWARGS) {
			krk_tableSet(to, entry->key, entry->value);
		}
	}
}

int krk_tableGet(KrkTable * table, KrkValue key, KrkValue * value) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (entry->key.type == VAL_KWARGS) {
		return 0;
	} else {
		*value = entry->value;
		return 1;
	}
}

int krk_tableDelete(KrkTable * table, KrkValue key) {
	if (table->count == 0) return 0;
	KrkTableEntry * entry = krk_findEntry(table->entries, table->capacity, key);
	if (entry->key.type == VAL_KWARGS) {
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
		if (entry->key.type == VAL_KWARGS) {
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
