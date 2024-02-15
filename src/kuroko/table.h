#pragma once
/**
 * @file table.h
 * @brief Implementation of a generic hash table.
 *
 * I was going to just use the ToaruOS hashmap library, but to make following
 * the book easier, let's just start from their Table implementation; it has
 * an advantage of using stored entries and fixed arrays, so it has some nice
 * properties despite being chained internally...
 */

#include <stdlib.h>
#include <sys/types.h>
#include "kuroko.h"
#include "value.h"

/**
 * @brief One (key,value) pair in a table.
 */
typedef struct {
	KrkValue key;
	KrkValue value;
} KrkTableEntry;

/**
 * @brief Simple hash table of arbitrary keys to values.
 */
typedef struct {
	size_t count;            /**< Number of actual items in the dict. */
	size_t capacity;         /**< Size (in items) of each of the entries/indexes arrays */
	size_t used;             /**< Next insertion index in the entries array */
	KrkTableEntry * entries; /**< Key-value pairs, in insertion order (with KWARGS_VAL(0) gaps) */
	ssize_t * indexes;       /**< Actual hash map: indexes into the key-value pairs. */
} KrkTable;

/**
 * @brief Initialize a hash table.
 * @memberof KrkTable
 *
 * This should be called for any new hash table, especially ones
 * initialized in heap or stack space, to set up the capacity, count
 * and initial entries pointer.
 *
 * @param table Hash table to initialize.
 */
extern void krk_initTable(KrkTable * table);

/**
 * @brief Release resources associated with a hash table.
 * @memberof KrkTable
 *
 * Frees the entries array for the table and resets count and capacity.
 *
 * @param table Hash table to release.
 */
extern void krk_freeTable(KrkTable * table);

/**
 * @brief Add all key-value pairs from 'from' into 'to'.
 * @memberof KrkTable
 *
 * Copies each key-value pair from one hash table to another. If a key
 * from 'from' already exists in 'to', the existing value in 'to' will be
 * overwritten with the value from 'from'.
 *
 * @param from Source table.
 * @param to   Destination table.
 */
extern void krk_tableAddAll(KrkTable * from, KrkTable * to);

/**
 * @brief Find a character sequence in the string interning table.
 * @memberof KrkTable
 *
 * Scans through the entries in a given table - usually vm.strings - to find
 * an entry equivalent to the string specified by the 'chars' and 'length'
 * parameters, using the 'hash' parameter to speed up lookup.
 *
 * @param table   Should always be @c &vm.strings
 * @param chars   C array of chars representing the string.
 * @param length  Length of the string.
 * @param hash    Precalculated hash value for the string.
 * @return If the string was found, the string object representation, else NULL.
 */
extern struct KrkString * krk_tableFindString(KrkTable * table, const char * chars, size_t length, uint32_t hash);

/**
 * @brief Assign a value to a key in a table.
 * @memberof KrkTable
 *
 * Inserts the key-value pair specified by 'key' and 'value' into the hash
 * table 'table', replacing any value that was already preseng with the
 * same key.
 *
 * @param table Table to assign to.
 * @param key   Key to assign.
 * @param value Value to assign to the key.
 * @return 0 if the key was already present and has been overwritten, 1 if the key is new to the table.
 */
extern int krk_tableSet(KrkTable * table, KrkValue key, KrkValue value);

/**
 * @brief Obtain the value associated with a key in a table.
 * @memberof KrkTable
 *
 * Scans the table 'table' for the key 'key' and, if found, outputs
 * the associated value to *value. If the key is not found, then
 * *value will not be changed.
 *
 * @param table Table to look up.
 * @param key   Key to look for.
 * @param value Output pointer to place resulting value in.
 * @return 0 if the key was not found, 1 if it was.
 */
extern int krk_tableGet(KrkTable * table, KrkValue key, KrkValue * value);

/**
 * @brief Obtain the value associated with a string key in a table.
 * @memberof KrkTable
 *
 * Same as krk_tableGet(), but only works for string keys. This is faster
 * than using krk_tableGet() and should be used when referencing attribute
 * tables or other places where keys are guaranteed to only be strings.
 *
 * @param table Table to look up.
 * @param str   Key to look for.
 * @param value Output pointer to place resulting value in.
 * @return 0 if the key was not found, 1 if it was.
 */
extern int krk_tableGet_fast(KrkTable * table, struct KrkString * str, KrkValue * value);

/**
 * @brief Remove a key from a hash table.
 * @memberof KrkTable
 *
 * Scans the table 'table' for the key 'key' and, if found, removes
 * the entry, replacing it with a tombstone value.
 *
 * @param table Table to delete from.
 * @param key   Key to delete.
 * @return 1 if the value was found and deleted, 0 if it was not present.
 */
extern int krk_tableDelete(KrkTable * table, KrkValue key);

/**
 * @brief Remove a key from a hash table, with identity lookup.
 * @memberof KrkTable
 *
 * Scans the table 'table' for the key 'key' and, if found, removes
 * the entry, replacing it with a tombstone value.
 *
 * @param table Table to delete from.
 * @param key   Key to delete.
 * @return 1 if the value was found and deleted, 0 if it was not present.
 */
extern int krk_tableDeleteExact(KrkTable * table, KrkValue key);

/**
 * @brief Calculate the hash for a value.
 * @memberof KrkValue
 *
 * Retreives or calculates the hash value for 'value'.
 *
 * @param value Value to hash.
 * @param *hashOut An unsigned 32-bit hash value.
 * @return Status code 0 for success, 1 for unhashable type.
 */
extern int krk_hashValue(KrkValue value, uint32_t *hashOut);

/**
 * @brief Preset the size of a table.
 * @memberof KrkTable
 *
 * Reserves space for a large table.
 *
 * @param table Table to resize.
 * @param capacity Target capacity.
 */
extern void krk_tableAdjustCapacity(KrkTable * table, size_t capacity);

/**
 * @brief Update the value of a table entry only if it is found.
 * @memberof KrkTable
 *
 * Searches the table for @p key and updates its value to @p value if found.
 * If @p key is not found, it is not added to the table.
 *
 * @warning Note the return value of this function is inverted from krk_tableSet
 *
 * @param table Table to assign to.
 * @param key   Key to assign.
 * @param value Value to assign to the key.
 * @return 0 if the key was not present, 1 if it was found and updated.
 */
extern int krk_tableSetIfExists(KrkTable * table, KrkValue key, KrkValue value);
