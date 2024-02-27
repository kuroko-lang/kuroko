/**
 * @file table.c
 * @brief Ordered hash map.
 *
 * The original table implementation was derived from CLox. CLox's
 * tables only supported string keys, but we support arbitrary values,
 * so long as they are hashable.
 *
 * This implementation maintains the same general API, but take its
 * inspiration from CPython to keep insertion order. The "entries"
 * table is still an array of key-value pairs, but no longer tracks
 * the hash lookup for the map. Instead, the entries array keeps
 * a strict insertion ordering, with deleted entries replaced with
 * sentinel values representing gaps. A separate "indexes" table
 * maps hash slots to their associated key-value pairs, or to -1
 * or -2 to represent unused and tombstone slots, respectively.
 *
 * When resizing a table, the entries array is rewritten and gaps
 * are removed. Simultaneously, the new index entries are populated.
 */
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
	table->used = 0;
	table->entries = NULL;
	table->indexes = NULL;
}

void krk_freeTable(KrkTable * table) {
	KRK_FREE_ARRAY(KrkTableEntry, table->entries, table->capacity);
	KRK_FREE_ARRAY(ssize_t, table->indexes, table->capacity);
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
		*hashOut = (uint32_t)((int)(intptr_t)AS_OBJECT(value));
		return 0;
	}
_unhashable:
	if (IS_NONE(krk_currentThread.currentException))
		krk_runtimeError(vm.exceptions->typeError, "unhashable type: '%T'", value);
	return 1;
}

static inline ssize_t krk_tableIndexKeyC(const KrkTableEntry * entries, const ssize_t * indexes, size_t capacity, KrkValue key, int (*comparator)(KrkValue,KrkValue)) {
	uint32_t index;
	if (krk_hashValue(key, &index)) return -1;
	index &= (capacity - 1);

	ssize_t tombstone = -1;
	for (;;) {
		if (indexes[index] == -1) {
			return tombstone != -1 ? tombstone : index;
		} else if (indexes[index] == -2) {
			if (tombstone == index) return tombstone;
			if (tombstone == -1) tombstone = index;
		} else if (comparator(entries[indexes[index]].key, key)) {
			return index;
		}
		index = (index + 1) & (capacity - 1);
	}
}

static ssize_t krk_tableIndexKey(const KrkTableEntry * entries, const ssize_t * indexes, size_t capacity, KrkValue key) {
	return krk_tableIndexKeyC(entries,indexes,capacity,key,krk_valuesSameOrEqual);
}

static ssize_t krk_tableIndexKeyExact(const KrkTableEntry * entries, const ssize_t * indexes, size_t capacity, KrkValue key) {
	return krk_tableIndexKeyC(entries,indexes,capacity,key,krk_valuesSame);
}

void krk_tableAdjustCapacity(KrkTable * table, size_t capacity) {
	KrkTableEntry * nentries = KRK_ALLOCATE(KrkTableEntry, capacity);
	ssize_t * nindexes = KRK_ALLOCATE(ssize_t, capacity);
	for (size_t i = 0; i < capacity; ++i) {
		nindexes[i] = -1;
		nentries[i].key = KWARGS_VAL(0);
		nentries[i].value = KWARGS_VAL(0);
	}

	/* Fill in used entries */
	const KrkTableEntry * e = table->entries;
	for (size_t i = 0; i < table->count; ++i) {
		while (IS_KWARGS(e->key)) e++;
		memcpy(&nentries[i], e, sizeof(KrkTableEntry));
		ssize_t indexkey = krk_tableIndexKeyExact(nentries,nindexes,capacity, e->key);
		nindexes[indexkey] = i;
		e++;
	}

	/* Swap before freeing */
	KrkTableEntry * oldEntries = table->entries;
	table->entries = nentries;
	KRK_FREE_ARRAY(KrkTableEntry, oldEntries, table->capacity);

	ssize_t * oldIndexes = table->indexes;
	table->indexes = nindexes;
	KRK_FREE_ARRAY(ssize_t, oldIndexes, table->capacity);

	/* Update table with new capacity and used count */
	table->capacity = capacity;
	table->used = table->count;
}

int krk_tableSet(KrkTable * table, KrkValue key, KrkValue value) {
	if (table->used + 1 > table->capacity * TABLE_MAX_LOAD) {
		size_t capacity = KRK_GROW_CAPACITY(table->capacity);
		krk_tableAdjustCapacity(table, capacity);
	}

	ssize_t index = krk_tableIndexKey(table->entries, table->indexes, table->capacity, key);
	if (index < 0) return 0;
	KrkTableEntry * entry;
	int isNew = table->indexes[index] < 0;
	if (isNew) {
		table->indexes[index] = table->used;
		entry = &table->entries[table->used];
		entry->key = key;
		table->used++;
		table->count++;
	} else {
		entry = &table->entries[table->indexes[index]];
	}
	entry->value = value;
	return isNew;
}

int krk_tableSetExact(KrkTable * table, KrkValue key, KrkValue value) {
	if (table->used + 1 > table->capacity * TABLE_MAX_LOAD) {
		size_t capacity = KRK_GROW_CAPACITY(table->capacity);
		krk_tableAdjustCapacity(table, capacity);
	}

	ssize_t index = krk_tableIndexKeyExact(table->entries, table->indexes, table->capacity, key);
	if (index < 0) return 0;
	KrkTableEntry * entry;
	int isNew = table->indexes[index] < 0;
	if (isNew) {
		table->indexes[index] = table->used;
		entry = &table->entries[table->used];
		entry->key = key;
		table->used++;
		table->count++;
	} else {
		entry = &table->entries[table->indexes[index]];
	}
	entry->value = value;
	return isNew;
}

int krk_tableSetIfExists(KrkTable * table, KrkValue key, KrkValue value) {
	if (table->count == 0) return 0;
	ssize_t index = krk_tableIndexKey(table->entries, table->indexes, table->capacity, key);
	if (index < 0 || table->indexes[index] < 0) return 0;
	table->entries[table->indexes[index]].value = value;
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
	ssize_t index = krk_tableIndexKey(table->entries, table->indexes, table->capacity, key);
	if (index < 0 || table->indexes[index] < 0) return 0;
	*value = table->entries[table->indexes[index]].value;
	return 1;
}

int krk_tableGet_fast(KrkTable * table, KrkString * str, KrkValue * value) {
	if (table->count == 0) return 0;
	uint32_t index = str->obj.hash & (table->capacity-1);

	ssize_t tombstone = -1;
	for (;;) {
		if (table->indexes[index] == -1) {
			return 0;
		} else if (table->indexes[index] == -2) {
			if (tombstone == index) return 0;
			if (tombstone == -1) tombstone = index;
		} else if (krk_valuesSame(table->entries[table->indexes[index]].key, OBJECT_VAL(str))) {
			*value = table->entries[table->indexes[index]].value;
			return 1;
		}
		index = (index + 1) & (table->capacity - 1);
	}
}

int krk_tableDelete(KrkTable * table, KrkValue key) {
	if (table->count == 0) return 0;
	ssize_t index = krk_tableIndexKey(table->entries, table->indexes, table->capacity, key);
	if (index < 0 || table->indexes[index] < 0) return 0;
	table->count--;
	table->entries[table->indexes[index]].key = KWARGS_VAL(0);
	table->entries[table->indexes[index]].value = KWARGS_VAL(0);
	table->indexes[index] = -2;
	return 1;
}

int krk_tableDeleteExact(KrkTable * table, KrkValue key) {
	if (table->count == 0) return 0;
	ssize_t index = krk_tableIndexKeyExact(table->entries, table->indexes, table->capacity, key);
	if (index < 0 || table->indexes[index] < 0) return 0;
	table->count--;
	table->entries[table->indexes[index]].key = KWARGS_VAL(0);
	table->entries[table->indexes[index]].value = KWARGS_VAL(0);
	table->indexes[index] = -2;
	return 1;
}

KrkString * krk_tableFindString(KrkTable * table, const char * chars, size_t length, uint32_t hash) {
	if (table->count == 0) return NULL;
	uint32_t index = hash & (table->capacity - 1);

	ssize_t tombstone = -1;
	for (;;) {
		if (table->indexes[index] == -1) {
			return NULL;
		} else if (table->indexes[index] == -2) {
			if (tombstone == index) return NULL;
			if (tombstone == -1) tombstone = index;
		} else if (AS_STRING(table->entries[table->indexes[index]].key)->length == length &&
		           AS_OBJECT(table->entries[table->indexes[index]].key)->hash == hash &&
		           memcmp(AS_STRING(table->entries[table->indexes[index]].key)->chars, chars, length) == 0) {
			return AS_STRING(table->entries[table->indexes[index]].key);
		}
		index = (index + 1) & (table->capacity - 1);
	}
}
