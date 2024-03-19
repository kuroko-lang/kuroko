#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>
#include <kuroko/threads.h>

#define LIST_WRAP_INDEX() \
	if (index < 0) index += self->values.count; \
	if (unlikely(index < 0 || index >= (krk_integer_type)self->values.count)) return krk_runtimeError(vm.exceptions->indexError, "list index out of range: %zd", (ssize_t)index)

#define LIST_WRAP_SOFT(val) \
	if (val < 0) val += self->values.count; \
	if (val < 0) val = 0; \
	if (val > (krk_integer_type)self->values.count) val = self->values.count

static void _list_gcscan(KrkInstance * self) {
	for (size_t i = 0; i < ((KrkList*)self)->values.count; ++i) {
		krk_markValue(((KrkList*)self)->values.values[i]);
	}
}

static void _list_gcsweep(KrkInstance * self) {
	krk_freeValueArray(&((KrkList*)self)->values);
}

/**
 * Convenience constructor for the C API.
 */
KrkValue krk_list_of(int argc, const KrkValue argv[], int hasKw) {
	KrkValue outList = OBJECT_VAL(krk_newInstance(vm.baseClasses->listClass));
	krk_push(outList);
	krk_initValueArray(AS_LIST(outList));

	if (argc) {
		AS_LIST(outList)->capacity = argc;
		AS_LIST(outList)->values = KRK_GROW_ARRAY(KrkValue, AS_LIST(outList)->values, 0, argc);
		memcpy(AS_LIST(outList)->values, argv, sizeof(KrkValue) * argc);
		AS_LIST(outList)->count = argc;
	}

	pthread_rwlock_init(&((KrkList*)AS_OBJECT(outList))->rwlock, NULL);
	return krk_pop();
}

#define CURRENT_CTYPE KrkList *
#define CURRENT_NAME  self

KRK_Method(list,__getitem__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) {
		CHECK_ARG(1,int,krk_integer_type,index);
		if (vm.globalFlags & KRK_GLOBAL_THREADS) pthread_rwlock_rdlock(&self->rwlock);
		LIST_WRAP_INDEX();
		KrkValue result = self->values.values[index];
		if (vm.globalFlags & KRK_GLOBAL_THREADS) pthread_rwlock_unlock(&self->rwlock);
		return result;
	} else if (IS_slice(argv[1])) {
		pthread_rwlock_rdlock(&self->rwlock);

		KRK_SLICER(argv[1],self->values.count) {
			pthread_rwlock_unlock(&self->rwlock);
			return NONE_VAL();
		}

		if (step == 1) {
			krk_integer_type len = end - start;
			KrkValue result = krk_list_of(len, &AS_LIST(argv[0])->values[start], 0);
			pthread_rwlock_unlock(&self->rwlock);
			return result;
		} else {
			/* iterate and push */
			krk_push(NONE_VAL());
			krk_integer_type len = 0;
			krk_integer_type i = start;
			while ((step < 0) ? (i > end) : (i < end)) {
				krk_push(self->values.values[i]);
				len++;
				i += step;
			}

			/* make into a list */
			KrkValue result = krk_callNativeOnStack(len, &krk_currentThread.stackTop[-len], 0, krk_list_of);
			krk_currentThread.stackTop[-len-1] = result;
			while (len) {
				krk_pop();
				len--;
			}

			pthread_rwlock_unlock(&self->rwlock);
			return krk_pop();
		}
	} else {
		return TYPE_ERROR(int or slice,argv[1]);
	}
}

KRK_Method(list,__eq__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_list(argv[1])) return NOTIMPL_VAL();
	KrkList * them = AS_list(argv[1]);
	if (self->values.count != them->values.count) return BOOLEAN_VAL(0);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (!krk_valuesSameOrEqual(self->values.values[i], them->values.values[i])) return BOOLEAN_VAL(0);
	}
	return BOOLEAN_VAL(1);
}

KRK_Method(list,append) {
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_wrlock(&self->rwlock);
	krk_writeValueArray(&self->values, argv[1]);
	pthread_rwlock_unlock(&self->rwlock);
	return NONE_VAL();
}

KRK_Method(list,insert) {
	METHOD_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,index);
	pthread_rwlock_wrlock(&self->rwlock);
	LIST_WRAP_SOFT(index);
	krk_writeValueArray(&self->values, NONE_VAL());
	memmove(
		&self->values.values[index+1],
		&self->values.values[index],
		sizeof(KrkValue) * (self->values.count - index - 1)
	);
	self->values.values[index] = argv[2];
	pthread_rwlock_unlock(&self->rwlock);
	return NONE_VAL();
}

KRK_Method(list,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("[...]"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	struct StringBuilder sb = {0};
	pushStringBuilder(&sb, '[');
	pthread_rwlock_rdlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (!krk_pushStringBuilderFormat(&sb,"%R",self->values.values[i])) goto _error;
		if (i + 1 < self->values.count) {
			pushStringBuilderStr(&sb, ", ", 2);
		}
	}
	pthread_rwlock_unlock(&self->rwlock);

	pushStringBuilder(&sb,']');
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	krk_discardStringBuilder(&sb);
	pthread_rwlock_unlock(&self->rwlock);
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return NONE_VAL();
}

static int _list_extend_callback(void * context, const KrkValue * values, size_t count) {
	KrkValueArray * positionals = context;
	if (positionals->count + count > positionals->capacity) {
		size_t old = positionals->capacity;
		positionals->capacity = (count == 1) ? KRK_GROW_CAPACITY(old) : (positionals->count + count);
		positionals->values = KRK_GROW_ARRAY(KrkValue, positionals->values, old, positionals->capacity);
	}

	for (size_t i = 0; i < count; ++i) {
		positionals->values[positionals->count++] = values[i];
	}

	return 0;
}

KRK_Method(list,extend) {
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_wrlock(&self->rwlock);
	KrkValueArray *  positionals = AS_LIST(argv[0]);
	KrkValue other = argv[1];
	if (krk_valuesSame(argv[0],other)) {
		other = krk_list_of(self->values.count, self->values.values, 0);
	}

	krk_unpackIterable(other, positionals, _list_extend_callback);

	pthread_rwlock_unlock(&self->rwlock);
	return NONE_VAL();
}

KRK_Method(list,__init__) {
	METHOD_TAKES_AT_MOST(1);
	krk_initValueArray(AS_LIST(argv[0]));
	pthread_rwlock_init(&self->rwlock, NULL);
	if (argc == 2) {
		_list_extend(2,(KrkValue[]){argv[0],argv[1]},0);
	}
	return NONE_VAL();
}

KRK_Method(list,__mul__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,howMany);

	KrkValue out = krk_list_of(0, NULL, 0);

	krk_push(out);

	for (krk_integer_type i = 0; i < howMany; i++) {
		_list_extend(2, (KrkValue[]){out,argv[0]},0);
	}

	return krk_pop();
}

KRK_Method(list,__len__) {
	METHOD_TAKES_NONE();
	return INTEGER_VAL(self->values.count);
}

KRK_Method(list,__contains__) {
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_rdlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesSameOrEqual(argv[1], self->values.values[i])) {
			pthread_rwlock_unlock(&self->rwlock);
			return BOOLEAN_VAL(1);
		}
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) break;
	}
	pthread_rwlock_unlock(&self->rwlock);
	return BOOLEAN_VAL(0);
}

KRK_Method(list,pop) {
	METHOD_TAKES_AT_MOST(1);
	pthread_rwlock_wrlock(&self->rwlock);
	krk_integer_type index = self->values.count - 1;
	if (argc == 2) {
		CHECK_ARG(1,int,krk_integer_type,ind);
		index = ind;
	}
	LIST_WRAP_INDEX();
	KrkValue outItem = AS_LIST(argv[0])->values[index];
	if (index == (long)AS_LIST(argv[0])->count-1) {
		AS_LIST(argv[0])->count--;
		pthread_rwlock_unlock(&self->rwlock);
		return outItem;
	} else {
		/* Need to move up */
		size_t remaining = AS_LIST(argv[0])->count - index - 1;
		memmove(&AS_LIST(argv[0])->values[index], &AS_LIST(argv[0])->values[index+1],
			sizeof(KrkValue) * remaining);
		AS_LIST(argv[0])->count--;
		pthread_rwlock_unlock(&self->rwlock);
		return outItem;
	}
}

KRK_Method(list,__setitem__) {
	METHOD_TAKES_EXACTLY(2);
	if (IS_INTEGER(argv[1])) {
		CHECK_ARG(1,int,krk_integer_type,index);
		if (vm.globalFlags & KRK_GLOBAL_THREADS) pthread_rwlock_rdlock(&self->rwlock);
		LIST_WRAP_INDEX();
		self->values.values[index] = argv[2];
		if (vm.globalFlags & KRK_GLOBAL_THREADS) pthread_rwlock_unlock(&self->rwlock);
		return argv[2];
	} else if (IS_slice(argv[1])) {
		if (!IS_list(argv[2])) {
			return TYPE_ERROR(list,argv[2]); /* TODO other sequence types */
		}

		KRK_SLICER(argv[1],self->values.count) {
			return NONE_VAL();
		}

		if (step != 1) {
			return krk_runtimeError(vm.exceptions->valueError, "step value unsupported");
		}

		krk_integer_type len = end - start;
		krk_integer_type newLen = (krk_integer_type)AS_LIST(argv[2])->count;

		for (krk_integer_type i = 0; (i < len && i < newLen); ++i) {
			AS_LIST(argv[0])->values[start+i] = AS_LIST(argv[2])->values[i];
		}

		while (len < newLen) {
			FUNC_NAME(list,insert)(3, (KrkValue[]){argv[0], INTEGER_VAL(start + len), AS_LIST(argv[2])->values[len]}, 0);
			len++;
		}

		while (newLen < len) {
			FUNC_NAME(list,pop)(2, (KrkValue[]){argv[0], INTEGER_VAL(start + len - 1)}, 0);
			len--;
		}

		return OBJECT_VAL(self);
	} else {
		return TYPE_ERROR(int or slice, argv[1]);
	}
}

KRK_Method(list,__delitem__) {
	METHOD_TAKES_EXACTLY(1);

	if (IS_INTEGER(argv[1])) {
		FUNC_NAME(list,pop)(2,(KrkValue[]){argv[0],argv[1]},0);
	} else if (IS_slice(argv[1])) {
		KRK_SLICER(argv[1],self->values.count) {
			return NONE_VAL();
		}

		if (step != 1) {
			return krk_runtimeError(vm.exceptions->valueError, "step value unsupported");
		}

		krk_integer_type len = end - start;

		while (len > 0) {
			FUNC_NAME(list,pop)(2,(KrkValue[]){argv[0],INTEGER_VAL(start)},0);
			len--;
		}
	} else {
		return TYPE_ERROR(int or slice, argv[1]);
	}

	return NONE_VAL();
}

KRK_Method(list,remove) {
	METHOD_TAKES_EXACTLY(1);
	pthread_rwlock_wrlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesSameOrEqual(self->values.values[i], argv[1])) {
			pthread_rwlock_unlock(&self->rwlock);
			return FUNC_NAME(list,pop)(2,(KrkValue[]){argv[0], INTEGER_VAL(i)},0);
		}
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
			pthread_rwlock_unlock(&self->rwlock);
			return NONE_VAL();
		}
	}
	pthread_rwlock_unlock(&self->rwlock);
	return krk_runtimeError(vm.exceptions->valueError, "not found");
}

KRK_Method(list,clear) {
	METHOD_TAKES_NONE();
	pthread_rwlock_wrlock(&self->rwlock);
	krk_freeValueArray(&self->values);
	pthread_rwlock_unlock(&self->rwlock);
	return NONE_VAL();
}

KRK_Method(list,index) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(3);

	krk_integer_type min = 0;
	krk_integer_type max = self->values.count;

	if (argc > 2) {
		if (IS_INTEGER(argv[2]))
			min = AS_INTEGER(argv[2]);
		else
			return krk_runtimeError(vm.exceptions->typeError, "%s must be int, not '%T'", "min", argv[2]);
	}

	if (argc > 3) {
		if (IS_INTEGER(argv[3]))
			max = AS_INTEGER(argv[3]);
		else
			return krk_runtimeError(vm.exceptions->typeError, "%s must be int, not '%T'", "max", argv[3]);
	}

	pthread_rwlock_rdlock(&self->rwlock);
	LIST_WRAP_SOFT(min);
	LIST_WRAP_SOFT(max);

	for (krk_integer_type i = min; i < max; ++i) {
		if (krk_valuesSameOrEqual(self->values.values[i], argv[1])) {
			pthread_rwlock_unlock(&self->rwlock);
			return INTEGER_VAL(i);
		}
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) {
			pthread_rwlock_unlock(&self->rwlock);
			return NONE_VAL();
		}
	}

	pthread_rwlock_unlock(&self->rwlock);
	return krk_runtimeError(vm.exceptions->valueError, "not found");
}

KRK_Method(list,count) {
	METHOD_TAKES_EXACTLY(1);
	krk_integer_type count = 0;

	pthread_rwlock_rdlock(&self->rwlock);
	for (size_t i = 0; i < self->values.count; ++i) {
		if (krk_valuesSameOrEqual(self->values.values[i], argv[1])) count++;
		if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) break;
	}
	pthread_rwlock_unlock(&self->rwlock);

	return INTEGER_VAL(count);
}

KRK_Method(list,copy) {
	METHOD_TAKES_NONE();
	pthread_rwlock_rdlock(&self->rwlock);
	KrkValue result = krk_list_of(self->values.count, self->values.values, 0);
	pthread_rwlock_unlock(&self->rwlock);
	return result;
}

/** @brief In-place reverse a value array. */
static void reverse_values(KrkValue * values, size_t n) {
	KrkValue * end = values + n - 1;
	while (values < end) {
		krk_currentThread.scratchSpace[0] = *values;
		*values = *end;
		*end = krk_currentThread.scratchSpace[0];
		values++;
		end--;
	}
	krk_currentThread.scratchSpace[0] = NONE_VAL();
}

KRK_Method(list,reverse) {
	METHOD_TAKES_NONE();
	pthread_rwlock_wrlock(&self->rwlock);
	if (self->values.count > 1) reverse_values(self->values.values, self->values.count);
	pthread_rwlock_unlock(&self->rwlock);
	return NONE_VAL();
}

struct SortSlice {
	KrkValue * keys;
	KrkValue * values;
};

struct SliceAndPower {
	struct SortSlice begin;
	size_t power;
};

struct Run {
	struct SortSlice start;
	struct SortSlice end;
	size_t power;
};

/** @brief s++ */
static inline void slice_advance(struct SortSlice * slice) {
	slice->keys++;
	if (slice->values) slice->values++;
}

/** @brief s-- */
static inline void slice_decrement(struct SortSlice * slice) {
	slice->keys--;
	if (slice->values) slice->values--;
}


/* @brief s + 1 */
static struct SortSlice slice_next(struct SortSlice slice) {
	return (struct SortSlice){slice.keys + 1, slice.values ? slice.values + 1 : NULL};
}

/** @brief s + n */
static struct SortSlice slice_plus(struct SortSlice slice, ssize_t n) {
	return (struct SortSlice){slice.keys + n, slice.values ? slice.values + n : NULL};
}

/** @brief Copy start-end to buffer */
static void copy_slice(struct SortSlice start, struct SortSlice end, struct SortSlice buffer) {
	while (start.keys != end.keys) {
		*buffer.keys = *start.keys;
		if (buffer.values) *buffer.values = *start.values;
		slice_advance(&start);
		slice_advance(&buffer);
	}
}

/** @brief Very strictly a < b */
static int _list_sorter(KrkValue a, KrkValue b) {
	KrkValue comp = krk_operator_lt(a,b);
	return (IS_NONE(comp) || (IS_BOOLEAN(comp) && AS_BOOLEAN(comp)));
}

/** @brief While next is strictly < current, advance current */
static struct SortSlice powersort_strictlyDecreasingPrefix(struct SortSlice begin, struct SortSlice end) {
	while (begin.keys + 1 < end.keys && _list_sorter(*(begin.keys + 1), *begin.keys)) slice_advance(&begin);
	return slice_next(begin);
}

/** @brief While next is greater than or equal to current, advance current */
static struct SortSlice powersort_weaklyIncreasingPrefix(struct SortSlice begin, struct SortSlice end) {
	while (begin.keys + 1 < end.keys && !_list_sorter(*(begin.keys + 1), *begin.keys)) slice_advance(&begin);
	return slice_next(begin);
}

/**
 * @brief Extend a run to the right
 *
 * Returns a slice pointing at the end of the run after extended it to the right.
 * The resulting run consists of strictly ordered (a <= b, b > a) entries. We also
 * handle reverse runs by reversing them in-place.
 *
 * @param begin Start of run
 * @param end   End of available input to scan; always end of list.
 * @returns Slice pointing to end of run
 */
static struct SortSlice powersort_extend_and_reverse_right(struct SortSlice begin, struct SortSlice end) {
	struct SortSlice j = begin;
	if (j.keys == end.keys) return j;
	if (j.keys + 1 == end.keys) return slice_next(j);
	if (_list_sorter(*slice_next(j).keys, *j.keys)) {
		/* If next is strictly less than current, begin a reversed chain; we already know
		 * we can advance by one, so do that before continuing to save a comparison. */
		j = powersort_strictlyDecreasingPrefix(slice_next(begin), end);
		reverse_values(begin.keys, j.keys - begin.keys);
		if (begin.values) reverse_values(begin.values, j.values - begin.values);
	} else {
		/* Weakly increasing means j+1 >= j; continue with that chain*/
		j = powersort_weaklyIncreasingPrefix(slice_next(begin), end);
	}
	return j;
}

/**
 * @brief Calculate power.
 *
 * I'll be honest here, I don't really know what this does; it's from the reference impl.
 * and described in the paper.
 */
static size_t powersort_power(size_t begin, size_t end, size_t beginA, size_t beginB, size_t endB) {
	size_t n = end - begin;
	unsigned long l = beginA - begin + beginB - begin;
	unsigned long r = beginB - begin + endB -  begin;
	size_t common = 0;
	int digitA = l >= n;
	int digitB = r >= n;
	while (digitA == digitB) {
		common++;
		if (digitA) {
			l -= n;
			r -= n;
		}
		l <<= 1;
		r <<= 1;
		digitA = l >= n;
		digitB = r >= n;
	}
	return common + 1;
}

/**
 * @brief Merge neighboring runs.
 *
 * Merges the neighboring, sorted runs [left, mid) and [mid, right) using the provided
 * buffer space. Specifically, the smaller of the two runs is copied to the buffer, and
 * then merging occurs in-place.
 *
 * @param left   Start of the first run
 * @param mid    End of first run, start of second run
 * @param right  End of second run
 * @param buffer Scratch space
 */
static void powersort_merge(struct SortSlice left, struct SortSlice mid, struct SortSlice right, struct SortSlice buffer) {
	size_t n1 = mid.keys - left.keys;
	size_t n2 = right.keys - mid.keys;

	if (n1 <= n2) {
		copy_slice(left, mid, buffer);
		struct SortSlice c1 = buffer, e1 = slice_plus(buffer, n1);
		struct SortSlice c2 = mid, e2 = right, o = left;

		while (c1.keys < e1.keys && c2.keys < e2.keys) {
			if (!_list_sorter(*c2.keys, *c1.keys)) {
				*o.keys = *c1.keys;
				if (o.values) *o.values = *c1.values;
				slice_advance(&c1);
			} else {
				*o.keys = *c2.keys;
				if (o.values) *o.values = *c2.values;
				slice_advance(&c2);
			}
			slice_advance(&o);
		}

		while (c1.keys < e1.keys) {
			*o.keys = *c1.keys;
			if (o.values) *o.values = *c1.values;
			slice_advance(&c1);
			slice_advance(&o);
		}
	} else {
		copy_slice(mid, right, buffer);

		struct SortSlice c1 = slice_plus(mid, -1), s1 = left, o = slice_plus(right, -1);
		struct SortSlice c2 = slice_plus(buffer, n2 - 1), s2 = buffer;

		while (c1.keys >= s1.keys && c2.keys >= s2.keys) {
			if (!_list_sorter(*c2.keys, *c1.keys)) {
				*o.keys = *c2.keys;
				if (o.values) *o.values = *c2.values;
				slice_decrement(&c2);
			} else {
				*o.keys = *c1.keys;
				if (o.values) *o.values = *c1.values;
				slice_decrement(&c1);
			}
			slice_decrement(&o);
		}

		while (c2.keys >= s2.keys) {
			*o.keys = *c2.keys;
			if (o.values) *o.values = *c2.values;
			slice_decrement(&c2);
			slice_decrement(&o);
		}
	}
}

/**
 * @brief Powersort - merge-sort sorted runs
 *
 * This is an implementation of Munro-Wild Powersort from the paper at:
 * @ref https://www.wild-inter.net/publications/html/munro-wild-2018.pdf.html
 *
 * The reference implementation was also a helpful thing to study, and much
 * of the iteration and merging is based on its use of C++ iterators:
 * @ref https://github.com/sebawild/powersort
 *
 * There's no fancy extensions or improvements here, just the plain approach
 * set out in the paper, which is probably good enough for us? That means no
 * extending short runs to a minimum run length, no fancy node power calcs,
 * just a short bit of extending and merging.
 *
 * If the key function raises an exception, no sorting will be attempted
 * and the exception from the key function will be raised immediately.
 *
 * If the values to be sorted can not compare with __lt__, an exception
 * should be thrown eventually, but the entire list may still be scanned
 * and the resulting state is undefined.
 *
 * @param list    List to sort in-place.
 * @param key     Key function, or None to sort values directly.
 * @param reverse Sort direction, 0 for normal (a[0] <= b[0], etc.), 1 for reversed.
 */
static void powersort(KrkList * list, KrkValue key, int reverse) {
	size_t n = list->values.count;
	struct SortSlice slice = {list->values.values, NULL};

	/* If there is a key function, create a separate array to store
	 * the resulting key values; shove it in a tuple so we can keep
	 * those key values from being garbage collected. */
	if (!IS_NONE(key)) {
		KrkTuple * _keys = krk_newTuple(n);
		krk_push(OBJECT_VAL(_keys));
		for (size_t i = 0; i < n; ++i) {
			krk_push(key);
			krk_push(list->values.values[i]);
			_keys->values.values[i] = krk_callStack(1);
			_keys->values.count++;

			/* If the key function threw an exception, bail early. */
			if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) goto _end_sort;
		}

		/* values are secondary, keys are what actually gets sorted */
		slice.values = slice.keys;
		slice.keys   = _keys->values.values;
	}

	/* We handle reverse sort by reversing, sorting normally, and then reversing again */
	if (reverse) {
		reverse_values(slice.keys, n);
		if (slice.values) reverse_values(slice.values, n);
	}

	/* Supposedly the absolute maximum for this is strictly less than the number of bits
	 * we can fit in a size_t, so 64 ought to cover us until someone tries porting Kuroko
	 * to one of the 128-bit architectures, but even then I don't think we can handle
	 * holding that many values in a list to begin with.
	 *
	 * stack[0] should always be empty. */
	struct SliceAndPower stack[64] = {0};
	int top = 0;

	/* Buffer space for the merges. We shouldn't need anywhere close to this much space,
	 * but best to be safe, and we're already allocating a bunch of space for key tuples */
	KrkTuple * bufferSpace = krk_newTuple(slice.values ? (n * 2) : n);
	krk_push(OBJECT_VAL(bufferSpace));
	for (size_t i = 0; i < bufferSpace->values.capacity; ++i) bufferSpace->values.values[bufferSpace->values.count++] = NONE_VAL();
	struct SortSlice buffer = {&bufferSpace->values.values[0], slice.values ? &bufferSpace->values.values[n] : NULL};

	/* This just take the role of the C++ iterators in the reference implementaiton */
	struct SortSlice begin = {slice.keys, slice.values};
	struct SortSlice end   = {slice.keys + n, slice.values ? slice.values + n : NULL};

	/* Our first run starts from the left and extends as far as it can. */
	struct Run a = {begin, powersort_extend_and_reverse_right(begin,end), 0};

	while (a.end.keys < end.keys) {
		/* Our next run is whatever is after that, assuming the initial run isn't the whole list. */
		struct Run b = {a.end, powersort_extend_and_reverse_right(a.end, end), 0};
		/* I don't really understand the power part of powersort, but whatever. */
		a.power = powersort_power(0, n, a.start.keys - begin.keys, b.start.keys - begin.keys, b.end.keys - begin.keys);

		/* While the stack has things with higher power, merge them into a */
		while (stack[top].power > a.power) {
			struct SliceAndPower top_run = stack[top--];
			powersort_merge(top_run.begin, a.start, a.end, buffer);
			a.start = top_run.begin;
		}
		/* Put a on top of the stack, and then replace a with b */
		stack[++top] = (struct SliceAndPower){a.start, a.power};
		a = (struct Run){b.start, b.end, 0};
	}

	/* While there are things in the stack (excluding the empty 0 slot), merge them into the last a */
	while (top > 0) {
		struct SliceAndPower top_run = stack[top--];
		powersort_merge(top_run.begin, a.start, end, buffer);
		a.start = top_run.begin;
	}

	krk_pop(); /* tuple with buffer space */
_end_sort:
	if (!IS_NONE(key)) krk_pop(); /* keys tuple */

	/* If we reversed at the start, reverse again now as the list is forward-sorted */
	if (reverse) reverse_values(list->values.values, n);
}

KRK_Method(list,sort) {
	KrkValue key = NONE_VAL();
	int reverse = 0;
	if (!krk_parseArgs(".|$Vp", (const char*[]){"key","reverse"}, &key, &reverse)) return NONE_VAL();

	if (self->values.count < 2) return NONE_VAL();

	pthread_rwlock_wrlock(&self->rwlock);
	powersort(self, key, reverse);
	pthread_rwlock_unlock(&self->rwlock);

	return NONE_VAL();
}

KRK_Method(list,__add__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_list(argv[1])) return TYPE_ERROR(list,argv[1]);

	pthread_rwlock_rdlock(&self->rwlock);
	KrkValue outList = krk_list_of(self->values.count, self->values.values, 0); /* copy */
	pthread_rwlock_unlock(&self->rwlock);
	FUNC_NAME(list,extend)(2,(KrkValue[]){outList,argv[1]},0); /* extend */
	return outList;
}

FUNC_SIG(listiterator,__init__);

KRK_Method(list,__iter__) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->listiteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(listiterator,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();

	return OBJECT_VAL(output);
}

#define MAKE_LIST_COMPARE(name,op) \
	KRK_Method(list,__ ## name ## __) { \
		METHOD_TAKES_EXACTLY(1); \
		if (!IS_list(argv[1])) return NOTIMPL_VAL(); \
		KrkList * them = AS_list(argv[1]); \
		size_t lesser = self->values.count < them->values.count ? self->values.count : them->values.count; \
		for (size_t i = 0; i < lesser; ++i) { \
			KrkValue a = self->values.values[i]; \
			KrkValue b = them->values.values[i]; \
			if (krk_valuesSameOrEqual(a,b)) continue; \
			if (unlikely(krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION)) return NONE_VAL(); \
			return krk_operator_ ## name(a,b); \
		} \
		return BOOLEAN_VAL((self->values.count op them->values.count)); \
	}

MAKE_LIST_COMPARE(gt,>)
MAKE_LIST_COMPARE(lt,<)
MAKE_LIST_COMPARE(ge,>=)
MAKE_LIST_COMPARE(le,<=)

#undef CURRENT_CTYPE

struct ListIterator {
	KrkInstance inst;
	KrkValue l;
	size_t i;
};

#define CURRENT_CTYPE struct ListIterator *
#define IS_listiterator(o) (likely(IS_INSTANCE(o) && AS_INSTANCE(o)->_class == vm.baseClasses->listiteratorClass) || krk_isInstanceOf(o,vm.baseClasses->listiteratorClass))
#define AS_listiterator(o) (struct ListIterator*)AS_OBJECT(o)

static void _listiterator_gcscan(KrkInstance * self) {
	krk_markValue(((struct ListIterator*)self)->l);
}

KRK_Method(listiterator,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,list,KrkList*,list);
	self->l = argv[1];
	self->i = 0;
	return NONE_VAL();
}

FUNC_SIG(listiterator,__call__) {
	static _unused const char* _method_name = "__call__";
	if (unlikely((argc != 1))) goto _bad;
	if (unlikely(!IS_OBJECT(argv[0]))) goto _bad;
	if (unlikely(AS_INSTANCE(argv[0])->_class != vm.baseClasses->listiteratorClass)) goto _bad;

_maybeGood: (void)0;
	struct  ListIterator * self = (struct ListIterator *)AS_OBJECT(argv[0]);

	KrkValue _list = self->l;
	size_t _counter = self->i;
	if (unlikely(_counter >= AS_LIST(_list)->count)) {
		return argv[0];
	} else {
		self->i = _counter + 1;
		return AS_LIST(_list)->values[_counter];
	}

_bad:
	if (argc != 1) return NOT_ENOUGH_ARGS(name);
	if (!krk_isInstanceOf(argv[0], vm.baseClasses->listiteratorClass)) return TYPE_ERROR(listiterator, argv[0]);
	goto _maybeGood;
}

KRK_Function(sorted) {
	if (argc < 1) return krk_runtimeError(vm.exceptions->argumentError,"%s() takes %s %d argument%s (%d given)","sorted","at least",1,"",argc);
	KrkValue listOut = krk_list_of(0,NULL,0);
	krk_push(listOut);
	FUNC_NAME(list,extend)(2,(KrkValue[]){listOut,argv[0]},0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	FUNC_NAME(list,sort)(1,(KrkValue[]){listOut,hasKw ? argv[1] : NONE_VAL(), hasKw ? argv[2] : NONE_VAL()},hasKw);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	return krk_pop();
}

KRK_Function(reversed) {
	/* FIXME The Python reversed() function produces an iterator and only works for things with indexing or a __reversed__ method;
	 *       Building a list and reversing it like we do here is not correct! */
	if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError,"%s() takes %s %d argument%s (%d given)","reversed","exactly",1,"",argc);
	KrkValue listOut = krk_list_of(0,NULL,0);
	krk_push(listOut);
	FUNC_NAME(list,extend)(2,(KrkValue[]){listOut,argv[0]},0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	FUNC_NAME(list,reverse)(1,&listOut,0);
	if (!IS_NONE(krk_currentThread.currentException)) return NONE_VAL();
	return krk_pop();
}

_noexport
void _createAndBind_listClass(void) {
	KrkClass * list = ADD_BASE_CLASS(vm.baseClasses->listClass, "list", vm.baseClasses->objectClass);
	list->allocSize = sizeof(KrkList);
	list->_ongcscan = _list_gcscan;
	list->_ongcsweep = _list_gcsweep;
	BIND_METHOD(list,__init__);
	BIND_METHOD(list,__eq__);
	BIND_METHOD(list,__getitem__);
	BIND_METHOD(list,__setitem__);
	BIND_METHOD(list,__delitem__);
	BIND_METHOD(list,__len__);
	BIND_METHOD(list,__repr__);
	BIND_METHOD(list,__contains__);
	BIND_METHOD(list,__iter__);
	BIND_METHOD(list,__mul__);
	BIND_METHOD(list,__add__);
	BIND_METHOD(list,__lt__);
	BIND_METHOD(list,__gt__);
	BIND_METHOD(list,__le__);
	BIND_METHOD(list,__ge__);
	KRK_DOC(BIND_METHOD(list,append),
		"@brief Add an item to the end of the list.\n"
		"@arguments item\n\n"
		"Adds an item to the end of a list. Appending items to a list is an amortized constant-time "
		"operation, but may result in the reallocation of the list if not enough additional space is "
		"available to store to the new element in the current allocation.");
	KRK_DOC(BIND_METHOD(list,extend),
		"@brief Add the contents of an iterable to the end of a list.\n"
		"@argument iterable\n\n"
		"Adds all of the elements of @p iterable to the end of the list, as if each were added individually "
		"with @ref _list_append.");
	KRK_DOC(BIND_METHOD(list,pop),
		"@brief Remove and return an element from the list.\n"
		"@arguments [index]\n\n"
		"Removes and returns the entry at the end of the list, or at @p index if provided. "
		"Popping from the end of the list is constant-time. Popping from the head of the list "
		"is always O(n) as the contents of the list must be shifted.");
	KRK_DOC(BIND_METHOD(list,insert),
		"@brief Add an entry to the list at a given offset.\n"
		"@arguments index, val\n\n"
		"Adds @p val to the list at offset @p index, moving all following items back. Inserting "
		"near the beginning of a list can be costly.");
	KRK_DOC(BIND_METHOD(list,clear),
		"@brief Empty a list.\n\n"
		"Removes all entries from the list.");
	KRK_DOC(BIND_METHOD(list,index),
		"@brief Locate an item in the list by value.\n"
		"@arguments val,[min,[max]]\n\n"
		"Searches for @p val in the list and returns its index if found. If @p min is provided, "
		"the search will begin at index @p min. If @p max is also provided, the search will end "
		"at index @p max.\n"
		"Raises @ref ValueError if the item is not found.");
	KRK_DOC(BIND_METHOD(list,count),
		"@brief Count instances of a value in the list.\n"
		"@arguments val\n\n"
		"Scans the list for values equal to @p val and returns the count of matching entries.");
	KRK_DOC(BIND_METHOD(list,copy),
		"@brief Clone a list.\n\n"
		"Equivalent to @c list[:], creates a new list with the same items as this list.");
	KRK_DOC(BIND_METHOD(list,remove),
		"@brief Remove an item from the list.\n"
		"@arguments val\n\n"
		"Scans the list for an entry equivalent to @p val and removes it from the list.\n"
		"Raises @ref ValueError if no matching entry is found.");
	KRK_DOC(BIND_METHOD(list,reverse),
		"@brief Reverse the contents of a list.\n\n"
		"Reverses the elements of the list in-place.");
	KRK_DOC(BIND_METHOD(list,sort),
		"@brief Sort the contents of a list.\n\n"
		"Performs an in-place sort of the elements in the list, returning @c None as a gentle reminder "
		"that the sort is in-place. If a sorted copy is desired, use @ref sorted instead.");
	krk_defineNative(&list->methods, "__class_getitem__", krk_GenericAlias)->obj.flags |= KRK_OBJ_FLAGS_FUNCTION_IS_CLASS_METHOD;
	krk_attachNamedValue(&list->methods, "__hash__", NONE_VAL());
	krk_finalizeClass(list);
	KRK_DOC(list, "Mutable sequence of arbitrary values.");

	BUILTIN_FUNCTION("sorted", FUNC_NAME(krk,sorted),
		"@brief Return a sorted representation of an iterable.\n"
		"@arguments iterable\n\n"
		"Creates a new, sorted list from the elements of @p iterable.");
	BUILTIN_FUNCTION("reversed", FUNC_NAME(krk,reversed),
		"@brief Return a reversed representation of an iterable.\n"
		"@arguments iterable\n\n"
		"Creates a new, reversed list from the elements of @p iterable.");

	KrkClass * listiterator = ADD_BASE_CLASS(vm.baseClasses->listiteratorClass, "listiterator", vm.baseClasses->objectClass);
	listiterator->allocSize = sizeof(struct ListIterator);
	listiterator->_ongcscan = _listiterator_gcscan;
	listiterator->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(listiterator,__init__);
	BIND_METHOD(listiterator,__call__);
	krk_finalizeClass(listiterator);

}
