/**
 * @brief Pairing heap.
 * @file module__pheap.c
 * @author K. Lange <klange@toaruos.org>
 *
 * A very simple pairing heap.
 *
 * Provides a min-heap with insert, peek, and pop.
 *
 * While heap entries may be mutable, care should be taken not to modify
 * any values used for comparison, as the heap can not update ordering.
 *
 * This could likely be improved by the implementation of parent pointers,
 * which can allow for elements of the heap other than the root to be
 * removed or updated (removal + reinsertion), but that requires the
 * ability to quickly reference specific elements - which requires the
 * heap "nodes" to also be accessible or addressible in some way, such
 * as by making them Kuroko objects. Such a change would likely result
 * in performance impacts, so a parent-pointer pairing heap should be
 * a separate class.
 *
 * The implementation here is based strongly on the pseudocode found in
 * the Wikipedia article "Paring heap".
 */
#include <assert.h>
#include <stdlib.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

static KrkClass * PHeapClass;

/**
 * @brief Heap node.
 *
 * Represents one element in the heap. Each element potentially has
 * a pointer to more elements (a "right" or "next" pointer) and a
 * pointer to a subheap (a "left" pointer).
 */
typedef struct PHeap PHeap;
struct PHeap {
	struct PHeap_Obj * owner;
	KrkValue value;
	PHeap * subheaps; /* Left pointer to first child, if any. */
	PHeap * next;  /* Right pointer to next sibling, if any. */
};

/**
 * @brief Heap comparator function.
 *
 * The heap is ostensibly a min-heap, but the comparison behavior is left
 * entirely to the user. A comparator function should return true if the
 * left (first) argument has priority (is less than) the right (second)
 * argument, and 0 otherwise. This comparison must be consistent or the
 * heap will not work correctly.
 *
 * Generally, this module only uses one comparison function: a wrapper that
 * calls a Kuroko callable. As Kuroko callables may hold their own state,
 * no facility was necessary to pass user data to the comparator - only
 * the left and right heap nodes to compare are given.
 */
typedef int (*pheap_comparator_func)(PHeap *, PHeap *);

/**
 * @brief meld - Combine two heaps.
 *
 * Combines two heaps and returns the result. The heaps are "destroyed" in the process.
 *
 * @param left       One heap. Can be @c NULL for an empty heap.
 * @param right      The other heap. Can be @c NULL for an empty heap.
 * @param comparator Function that should return true if @p left has priority (is less than) @p right.
 * @returns A pointer to the new root, which will be either of @p left or @p right.
 */
static PHeap * pheap_meld(PHeap * left, PHeap * right, pheap_comparator_func comparator) {
	/*
	 * If either of the heaps is "empty" (represented by NULL),
	 * then simply return the other one.
	 */
	if (!left) {
		return right;
	}
	if (!right) {
		return left;
	}

	/*
	 * Otherwise, pull the 'smaller' of the two up and add the 'larger'
	 * to the front of the subheap list of the smaller one. We use
	 * intrusive lists within our Heap struct, so each Heap is also
	 * a List node (with a `next` pointer).
	 */
	if (comparator(left, right)) {
		/* Turns `left` into Heap(left→value, right :: left→subheaps) */
		if (left->subheaps) {
			right->next = left->subheaps;
		}
		left->subheaps = right;
		return left;
	} else {
		/* Turns `right` into Heap(right→value, left :: right→subheaps) */
		if (right->subheaps) {
			left->next = right->subheaps;
		}
		right->subheaps = left;
		return right;
	}
}

/**
 * @brief merge_pairs - Perform left-to-right/right-to-left merge on lists of subheaps.
 *
 * The core of the heap.
 *
 * @param list List of pairs to merge.
 * @param comparator Comparator function as described in @c pheap_meld.
 * @returns the resulting heap.
 */
static PHeap * pheap_merge_pairs(PHeap * list, pheap_comparator_func comparator) {
	if (!list) {
		/* An empty list is represented by NULL, and yields an empty Heap,
		 * which is also represented by NULL... */
		return NULL;
	} else if (list->next == NULL) {
		/* If a list entry doesn't have a next, it has a size of one,
		 * and we can just return this heap directly. */
		return list;
	} else {
		/* Otherwise we meld the first two, then meld them with the result of
		 * recursively melding the rest, which performs our left-right /
		 * right-left two-stage merge. */
		PHeap * next  = list->next;
		list->next = NULL;
		PHeap * rest = next->next;
		next->next = NULL;
		return pheap_meld(pheap_meld(list, next, comparator), pheap_merge_pairs(rest, comparator), comparator);
	}
}

/**
 * @brief delete_min - Remove the 'smallest' value from the heap.
 *
 * Removes the root node of the heap, rebalancing the remainder
 * of the heap. Should only be used when the heap is not empty.
 *
 * @param heap Heap to remove the root of.
 * @param comparator Comparator function as described in @c pheap_meld.
 * @returns the resulting heap.
 */
static PHeap * pheap_delete_min(PHeap * heap, pheap_comparator_func comparator) {
	PHeap * subs = heap->subheaps;
	return pheap_merge_pairs(subs, comparator);
}

/**
 * @brief visit_heap - Call a user function for every node in the heap.
 *
 * The function is called before recursing.
 *
 * @param heap Heap to walk.
 * @param func Function to call.
 * @param extra User data to pass to the function.
 */
static void pheap_visit_heap(PHeap * heap, void (*func)(PHeap *, void*), void* extra) {
	if (!heap) return;
	func(heap, extra);
	pheap_visit_heap(heap->subheaps, func, extra);
	pheap_visit_heap(heap->next, func, extra);
}

/**
 * @brief visit_heap_after - Call a user function for every node in the heap.
 *
 * The function is called after recursing, so this is suitable for freeing the
 * entirety of a heap.
 *
 * @param heap Heap to walk.
 * @param func Function to call.
 * @param extra User data to pass to the function.
 */
static void pheap_visit_heap_after(PHeap * heap, void (*func)(PHeap *, void*), void* extra) {
	if (!heap) return;
	pheap_visit_heap_after(heap->subheaps, func, extra);
	pheap_visit_heap_after(heap->next, func, extra);
	func(heap, extra);
}

struct PHeap_Obj {
	KrkInstance inst;
	KrkValue comparator;
	PHeap * heap;
	size_t count;
};

#define IS_PHeap(o) (krk_isInstanceOf(o,PHeapClass))
#define AS_PHeap(o) ((struct PHeap_Obj*)AS_OBJECT(o))
#define CURRENT_CTYPE struct PHeap_Obj *
#define CURRENT_NAME self

KRK_Method(PHeap,__init__) {
	KrkValue comparator;
	if (!krk_parseArgs(".V:PHeap", (const char*[]){"comp"}, &comparator)) return NONE_VAL();
	self->comparator = comparator;
	return NONE_VAL();
}

static int run_comparator(PHeap * left, PHeap * right) {
	assert(left->owner == right->owner);
	krk_push(left->owner->comparator);
	krk_push(left->value);
	krk_push(right->value);
	KrkValue result = krk_callStack(2);
	if (!IS_BOOLEAN(result)) return 0;
	return AS_BOOLEAN(result);
}

KRK_Method(PHeap,insert) {
	KrkValue value;
	if (!krk_parseArgs(".V",(const char*[]){"value"}, &value)) return NONE_VAL();
	struct PHeap * node = calloc(sizeof(struct PHeap), 1);
	node->owner = self;
	node->value = value;
	self->heap = pheap_meld(self->heap, node, run_comparator);
	self->count += 1;
	return NONE_VAL();
}

KRK_Method(PHeap,peek) {
	if (self->heap) return self->heap->value;
	return NONE_VAL();
}

KRK_Method(PHeap,pop) {
	PHeap * old = self->heap;
	if (!old) return krk_runtimeError(vm.exceptions->indexError, "pop from empty heap");
	self->heap = pheap_delete_min(self->heap, run_comparator);
	self->count -= 1;
	KrkValue out = old->value;
	free(old);
	return out;
}

KRK_Method(PHeap,__bool__) {
	return BOOLEAN_VAL(self->heap != NULL);
}

KRK_Method(PHeap,__len__) {
	return INTEGER_VAL(self->count);
}

static void run_visitor(PHeap * heap, void * visitor) {
	krk_push(*(KrkValue*)visitor);
	krk_push(heap->value);
	krk_callStack(1);
}

KRK_Method(PHeap,visit) {
	KrkValue func;
	int after = 0;
	if (!krk_parseArgs(".V|p",(const char*[]){"func","after"},
		&func, &after)) return NONE_VAL();

	(after ? pheap_visit_heap_after : pheap_visit_heap)(self->heap, run_visitor, &func);

	return NONE_VAL();
}

static void _scan_one(PHeap * heap, void * unused) {
	krk_markValue(heap->value);
}

static void _pheap_scan(KrkInstance * _self) {
	struct PHeap_Obj * self = (void*)_self;
	krk_markValue(self->comparator);
	pheap_visit_heap(self->heap, _scan_one, NULL);
}

static void _free_one(PHeap * heap, void * unused) {
	free(heap);
}

static void _pheap_sweep(KrkInstance * _self) {
	struct PHeap_Obj * self = (void*)_self;
	pheap_visit_heap_after(self->heap,_free_one, NULL);
}

KRK_Method(PHeap,comp) {
	return self->comparator;
}

KRK_Module(_pheap) {
	KRK_DOC(module, "Pairing heap with simple insert and pop-min operations.");

	KrkClass * PHeap = krk_makeClass(module, &PHeapClass, "PHeap", vm.baseClasses->objectClass);
	KRK_DOC(PHeap,"Pairing heap with simple insert and pop-min operations.");
	PHeap->allocSize = sizeof(struct PHeap_Obj);
	PHeap->_ongcscan = _pheap_scan;
	PHeap->_ongcsweep = _pheap_sweep;

	KRK_DOC(BIND_METHOD(PHeap,__init__),
		"@arguments comp\n\n"
		"Create a new pairing heap governed by the given comparator function.");
	KRK_DOC(BIND_METHOD(PHeap,insert),
		"@arguments value\n\n"
		"Insert a new element into the heap.");
	KRK_DOC(BIND_METHOD(PHeap,peek),
		"Retrieve the root (smallest) element of the heap, or None if it is empty.");
	KRK_DOC(BIND_METHOD(PHeap,pop),
		"Remove and return the root (smallest) element of the heap. If the heap is empty, IndexError is raised.");
	BIND_METHOD(PHeap,__bool__);
	BIND_METHOD(PHeap,__len__);
	KRK_DOC(BIND_METHOD(PHeap,visit),
		"@arguments func,after=False\n\n"
		"Call a function for each element of the heap.");
	BIND_PROP(PHeap,comp);
	krk_finalizeClass(PHeapClass);
}
