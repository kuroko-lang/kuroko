#pragma once

/*
 * I was going to just use the ToaruOS hashmap library, but to make following
 * the book easier, let's just start from their Table implementation; it has
 * an advantage of using stored entries and fixed arrays, so it has some nice
 * properties despite being chained internally...
 */

#include <stdlib.h>
#include "kuroko.h"
#include "value.h"

typedef struct {
	KrkValue key;
	KrkValue value;
} KrkTableEntry;

typedef struct {
	size_t count;
	size_t capacity;
	KrkTableEntry * entries;
} KrkTable;

extern void krk_initTable(KrkTable * table);
extern void krk_freeTable(KrkTable * table);
extern void krk_tableAddAll(KrkTable * from, KrkTable * to);
extern KrkString * krk_tableFindString(KrkTable * table, const char * chars, size_t length, uint32_t hash);
extern int krk_tableSet(KrkTable * table, KrkValue key, KrkValue value);
extern int krk_tableGet(KrkTable * table, KrkValue key, KrkValue * value);
extern int krk_tableDelete(KrkTable * table, KrkValue key);
extern KrkTableEntry * krk_findEntry(KrkTableEntry * entries, size_t capacity, KrkValue key);
