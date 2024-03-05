#include <string.h>
#include <limits.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

static void _slice_gcscan(KrkInstance * self) {
	krk_markValue(((struct KrkSlice*)self)->start);
	krk_markValue(((struct KrkSlice*)self)->end);
	krk_markValue(((struct KrkSlice*)self)->step);
}

KrkValue krk_slice_of(int argc, const KrkValue argv[], int hasKw) {
	KrkValue outSlice = OBJECT_VAL(krk_newInstance(vm.baseClasses->sliceClass));
	krk_push(outSlice);

	AS_slice(outSlice)->start = (argc > 0) ? argv[0] : NONE_VAL();
	AS_slice(outSlice)->end   = (argc > 1) ? argv[1] : NONE_VAL();
	AS_slice(outSlice)->step  = (argc > 2) ? argv[2] : NONE_VAL();

	return krk_pop();
}

static inline krk_integer_type _wrap(krk_integer_type count, krk_integer_type val) {
	if (val < 0) val += count;
	if (val < 0) val = 0;
	if (val > count) val = count;
	return val;
}

static inline krk_integer_type _wrapn(krk_integer_type count, krk_integer_type val) {
	if (val < 0) val += count;
	if (val < -1) val = -1;
	if (val > count) val = count;
	return val;
}

int krk_extractSlicer(const char * _method_name, KrkValue slicerVal, krk_integer_type count, krk_integer_type *start, krk_integer_type *end, krk_integer_type *step) {
	if (!(IS_slice(slicerVal))) {
		TYPE_ERROR(slice, slicerVal);
		return 1;
	}

	struct KrkSlice * slicer = AS_slice(slicerVal);

	KrkValue _start = slicer->start;
	KrkValue _end   = slicer->end;
	KrkValue _step  = slicer->step;

	if (!(IS_INTEGER(_start) || IS_NONE(_start))) {
		TYPE_ERROR(int or None, _start);
		return 1;
	}

	if (!(IS_INTEGER(_end) || IS_NONE(_end))) {
		TYPE_ERROR(int or None, _end);
		return 1;
	}

	if (!(IS_INTEGER(_step) || IS_NONE(_step))) {
		TYPE_ERROR(int or None, _step);
	}

	if (count == 0) {
		*start = 0;
		*end = 0;
		*step = 1;
		return 0;
	}

	/* First off, the step */
	*step = IS_NONE(_step) ? 1 : AS_INTEGER(_step);

	if (*step == 0) {
		krk_runtimeError(vm.exceptions->valueError, "invalid 0 step");
		return 1;
	}

	if (*step > 0) {
		/* Normal step bounds */
		*start = _wrap(count, IS_NONE(_start) ? 0 : AS_INTEGER(_start));
		*end   = _wrap(count, IS_NONE(_end) ? count : AS_INTEGER(_end));
		if (*end < *start) *end = *start;
	} else {
		*start = IS_NONE(_start) ? (count-1) : _wrap(count, AS_INTEGER(_start));
		if (*start >= count) *start = count -1;
		*end = IS_NONE(_end) ? -1 : _wrapn(count, AS_INTEGER(_end));
		if (*end > *start) *end = *start;
	}

	return 0;
}

#define CURRENT_CTYPE struct KrkSlice *
#define CURRENT_NAME  self

KRK_Method(slice,__init__) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(3);

	if (argc == 2) {
		self->start = NONE_VAL();
		self->end = argv[1];
		self->step = NONE_VAL();
	} else {
		self->start = argv[1];
		self->end = argv[2];
		if (argc > 3) {
			self->step = argv[3];
		} else {
			self->step = NONE_VAL();
		}
	}

	return NONE_VAL();
}

KRK_Method(slice,__repr__) {
	METHOD_TAKES_NONE();
	if (((KrkObj*)self)->flags & KRK_OBJ_FLAGS_IN_REPR) return OBJECT_VAL(S("slice(...)"));
	((KrkObj*)self)->flags |= KRK_OBJ_FLAGS_IN_REPR;
	struct StringBuilder sb = {0};
	pushStringBuilderStr(&sb,"slice(",6);
	/* start */
	if (!krk_pushStringBuilderFormat(&sb, "%R", self->start)) goto _error;
	pushStringBuilderStr(&sb,", ",2);
	if (!krk_pushStringBuilderFormat(&sb, "%R", self->end)) goto _error;
	pushStringBuilderStr(&sb,", ",2);
	if (!krk_pushStringBuilderFormat(&sb, "%R", self->step)) goto _error;
	pushStringBuilder(&sb,')');
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	return finishStringBuilder(&sb);

_error:
	((KrkObj*)self)->flags &= ~(KRK_OBJ_FLAGS_IN_REPR);
	krk_discardStringBuilder(&sb);
	return NONE_VAL();
}

KRK_Method(slice,start) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->start;
}

KRK_Method(slice,end) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->end;
}

KRK_Method(slice,step) {
	ATTRIBUTE_NOT_ASSIGNABLE();
	return self->step;
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkInstance *
#define IS_ellipsis(o) (krk_isInstanceOf(o,KRK_BASE_CLASS(ellipsis)))
#define AS_ellipsis(o) ((KrkInstance*)AS_INSTANCE(o))

KRK_StaticMethod(ellipsis,__new__) {
	KrkClass * _class = NULL;
	if (!krk_parseArgs("O!", (const char*[]){"cls"}, KRK_BASE_CLASS(type), &_class)) return NONE_VAL();
	if (!krk_isSubClass(_class, KRK_BASE_CLASS(ellipsis))) {
		return krk_runtimeError(vm.exceptions->typeError, "%S is not a subclass of %S", _class->name, KRK_BASE_CLASS(ellipsis)->name);
	}

	KrkValue out;
	if (!krk_tableGet_fast(&vm.builtins->fields, S("Ellipsis"), &out)) return krk_runtimeError(vm.exceptions->typeError, "Ellipsis is missing");
	return out;
}

KRK_Method(ellipsis,__repr__) {
	return OBJECT_VAL(S("Ellipsis"));
}

_noexport
void _createAndBind_sliceClass(void) {
	KrkClass * slice = ADD_BASE_CLASS(KRK_BASE_CLASS(slice), "slice", KRK_BASE_CLASS(object));
	slice->allocSize = sizeof(struct KrkSlice);
	slice->_ongcscan = _slice_gcscan;
	slice->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(slice,__init__);
	BIND_METHOD(slice,__repr__);
	BIND_PROP(slice,start);
	BIND_PROP(slice,end);
	BIND_PROP(slice,step);
	krk_attachNamedValue(&slice->methods, "__hash__", NONE_VAL());
	krk_finalizeClass(slice);

	KrkClass * ellipsis = ADD_BASE_CLASS(KRK_BASE_CLASS(ellipsis), "ellipsis", KRK_BASE_CLASS(object));
	krk_attachNamedObject(&vm.builtins->fields, "Ellipsis", (KrkObj*)krk_newInstance(ellipsis));
	ellipsis->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_STATICMETHOD(ellipsis,__new__);
	BIND_METHOD(ellipsis,__repr__);
	krk_finalizeClass(ellipsis);

}
