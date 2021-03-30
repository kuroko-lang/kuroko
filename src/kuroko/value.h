#pragma once
/**
 * @file value.h
 * @brief Definitions for primitive stack references.
 */
#include <stdio.h>
#include <string.h>
#include "kuroko.h"

/**
 * @brief Base structure of all heap objects.
 *
 * KrkObj is the base type of all objects stored on the heap and
 * managed by the garbage collector.
 */
typedef struct KrkObj KrkObj;
typedef struct KrkString KrkString;

/**
 * @brief Tag enum for basic value types.
 *
 * These are the values stored in the upper NaN bits of
 * the boxed NaN values.
 */
typedef enum {
	KRK_VAL_BOOLEAN  = 0xFFFC,
	KRK_VAL_INTEGER  = 0xFFFD,
	KRK_VAL_HANDLER  = 0xFFFE,
	KRK_VAL_NONE     = 0xFFFF,
	KRK_VAL_KWARGS   = 0x7FFC,
	KRK_VAL_OBJECT   = 0x7FFD,
	KRK_VAL_NOTIMPL  = 0x7FFE,
} KrkValueType;

#define KRK_VAL_MASK_BOOLEAN ((uint64_t)0xFFFC000000000000) /* 1..1100 */
#define KRK_VAL_MASK_INTEGER ((uint64_t)0xFFFD000000000000) /* 1..1101 */
#define KRK_VAL_MASK_HANDLER ((uint64_t)0xFFFE000000000000) /* 1..1110 */
#define KRK_VAL_MASK_NONE    ((uint64_t)0xFFFF000000000000) /* 1..1111 */
#define KRK_VAL_MASK_KWARGS  ((uint64_t)0x7FFC000000000000) /* 0..1100 */
#define KRK_VAL_MASK_OBJECT  ((uint64_t)0x7FFD000000000000) /* 0..1101 */
#define KRK_VAL_MASK_NOTIMPL ((uint64_t)0x7FFE000000000000) /* 0..1110 */
#define KRK_VAL_MASK_NAN     ((uint64_t)0x7FFC000000000000)
#define KRK_VAL_MASK_LOW     ((uint64_t)0x0000FFFFFFFFFFFF)

/**
 * @struct KrkValue
 * @brief Stack reference or primative value.
 *
 * This type stores a stack reference to an object, or the contents of
 * a primitive type. Each VM thread's stack consists of an array of
 * these values, and they are generally passed around in the VM through
 * direct copying rather than as pointers, avoiding the need to track
 * memory used by them.
 *
 * Implemented through basic NaN-boxing where the top sixteen bits are
 * used as a tag (@ref KrkValueType) and the lower 32 or 48 bits contain
 * the various primitive types.
 */
typedef uint64_t KrkValue;

/**
 * @brief Flexible vector of stack references.
 *
 * Value Arrays provide a resizable collection of values and are the
 * backbone of lists and tuples.
 */
typedef struct {
	size_t capacity;   /**< Available allocated space. */
	size_t count;      /**< Current number of used slots. */
	KrkValue * values; /**< Pointer to heap-allocated storage. */
} KrkValueArray;

/**
 * @brief Initialize a value array.
 * @memberof KrkValueArray
 *
 * This should be called for any new value array, especially ones
 * initialized in heap or stack space, to set up the capacity, count
 * and initial value pointer.
 *
 * @param array Value array to initialize.
 */
extern void krk_initValueArray(KrkValueArray * array);

/**
 * @brief Add a value to a value array.
 * @memberof KrkValueArray
 *
 * Appends 'value' to the end of the given array, adjusting count values
 * and resizing as necessary.
 *
 * @param array Array to append to.
 * @param value Value to append to array.
 */
extern void krk_writeValueArray(KrkValueArray * array, KrkValue value);

/**
 * @brief Release relesources used by a value array.
 * @memberof KrkValueArray
 *
 * Frees the storage associated with a given value array and resets
 * its capacity and count. Does not directly free resources associated
 * with heap objects referenced by the values in this array: The GC
 * is responsible for taking care of that.
 *
 * @param array Array to release.
 */
extern void krk_freeValueArray(KrkValueArray * array);

/**
 * @brief Print a string representation of a value.
 * @memberof KrkValue
 *
 * Print a string representation of 'value' to the stream 'f'.
 * For primitives, performs appropriate formatting. For objects,
 * this will call __str__ on the object's representative type.
 * If the type does not have a __str__ method, __repr__ will be
 * tried before falling back to krk_typeName to directly print
 * the name of the class with no information on the value.
 *
 * This function provides the backend for the print() built-in.
 *
 * @param f     Stream to write to.
 * @param value Value to display.
 */
extern void krk_printValue(FILE * f, KrkValue value);

/**
 * @brief Print a value without calling the VM.
 * @memberof KrkValue
 *
 * Print a string representation of 'value' to the stream 'f',
 * avoiding calls to managed code by using simplified representations
 * where necessary. This is intended for use in debugging code, such
 * as during disassembly, or when printing values in an untrusted context.
 *
 * @note This function will truncate long strings and print them in a form
 *       closer to the 'repr()' representation, with escaped bytes, rather
 *       than directly printing them to the stream.
 *
 * @param f     Stream to write to.
 * @param value Value to display.
 */
extern void krk_printValueSafe(FILE * f, KrkValue value);

/**
 * @brief Compare two values for equality.
 * @memberof KrkValue
 *
 * Performs a relaxed equality comparison between two values,
 * check for equivalence by contents. This may call managed
 * code to run __eq__ methods.
 *
 * @return 1 if values are equivalent, 0 otherwise.
 */
extern int krk_valuesEqual(KrkValue a, KrkValue b);

/**
 * @brief Compare two values by identity.
 * @memberof KrkValue
 *
 * Performs a strict comparison between two values, comparing
 * their identities. For primitive values, this is generally
 * the same as comparing by equality. For objects, this compares
 * pointer values directly.
 *
 * @return 1 if values represent the same object or value, 0 otherwise.
 */
extern int krk_valuesSame(KrkValue a, KrkValue b);

typedef union {
	KrkValue val;
	double   dbl;
} KrkValueDbl;

#define NONE_VAL(value)     ((KrkValue)(KRK_VAL_MASK_LOW | KRK_VAL_MASK_NONE))
#define NOTIMPL_VAL(value)  ((KrkValue)(KRK_VAL_MASK_LOW | KRK_VAL_MASK_NOTIMPL))
#define BOOLEAN_VAL(value)  ((KrkValue)((uint32_t)(value) | KRK_VAL_MASK_BOOLEAN))
#define INTEGER_VAL(value)  ((KrkValue)((uint32_t)(value) | KRK_VAL_MASK_INTEGER))
#define KWARGS_VAL(value)   ((KrkValue)((uint32_t)(value) | KRK_VAL_MASK_KWARGS))
#define OBJECT_VAL(value)   ((KrkValue)(((uintptr_t)(value) & KRK_VAL_MASK_LOW) | KRK_VAL_MASK_OBJECT))
#define HANDLER_VAL(ty,ta)  ((KrkValue)((uint32_t)((((uint16_t)ty) << 16) | ((uint16_t)ta)) | KRK_VAL_MASK_HANDLER))
#define FLOATING_VAL(value) (((KrkValueDbl){.dbl = (value)}).val)

#define KRK_VAL_TYPE(value) ((value) >> 48)

#define AS_BOOLEAN(value)   ((krk_integer_type)((value) & KRK_VAL_MASK_LOW))
#define AS_INTEGER(value)   ((krk_integer_type)((value) & KRK_VAL_MASK_LOW))
#define AS_NOTIMPL(value)   ((krk_integer_type)((value) & KRK_VAL_MASK_LOW))
#define AS_HANDLER(value)   ((uint32_t)((value) & KRK_VAL_MASK_LOW))
#define AS_OBJECT(value)    ((KrkObj*)(uintptr_t)((value) & KRK_VAL_MASK_LOW))
#define AS_FLOATING(value)  (((KrkValueDbl){.val = (value)}).dbl)

#define IS_INTEGER(value)   (((value) & KRK_VAL_MASK_HANDLER) == KRK_VAL_MASK_BOOLEAN)
#define IS_BOOLEAN(value)   (((value) & KRK_VAL_MASK_NONE) == KRK_VAL_MASK_BOOLEAN)
#define IS_NONE(value)      (((value) & KRK_VAL_MASK_NONE) == KRK_VAL_MASK_NONE)
#define IS_HANDLER(value)   (((value) & KRK_VAL_MASK_NONE) == KRK_VAL_MASK_HANDLER)
#define IS_OBJECT(value)    (((value) & KRK_VAL_MASK_NONE) == KRK_VAL_MASK_OBJECT)
#define IS_KWARGS(value)    (((value) & KRK_VAL_MASK_NONE) == KRK_VAL_MASK_KWARGS)
#define IS_NOTIMPL(value)   (((value) & KRK_VAL_MASK_NONE) == KRK_VAL_MASK_NOTIMPL)
#define IS_FLOATING(value)  (((value) & KRK_VAL_MASK_NAN) != KRK_VAL_MASK_NAN)

#define AS_HANDLER_TYPE(value)    (AS_HANDLER(value) >> 16)
#define AS_HANDLER_TARGET(value)  (AS_HANDLER(value) & 0xFFFF)
#define IS_TRY_HANDLER(value)     (IS_HANDLER(value) && AS_HANDLER_TYPE(value) == OP_PUSH_TRY)
#define IS_WITH_HANDLER(value)    (IS_HANDLER(value) && AS_HANDLER_TYPE(value) == OP_PUSH_WITH)
#define IS_RAISE_HANDLER(value)   (IS_HANDLER(value) && AS_HANDLER_TYPE(value) == OP_RAISE)
#define IS_EXCEPT_HANDLER(value)  (IS_HANDLER(value) && AS_HANDLER_TYPE(value) == OP_FILTER_EXCEPT)

#define KWARGS_SINGLE (INT32_MAX)
#define KWARGS_LIST   (INT32_MAX-1)
#define KWARGS_DICT   (INT32_MAX-2)
#define KWARGS_NIL    (INT32_MAX-3)
#define KWARGS_UNSET  (0)
