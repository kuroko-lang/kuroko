#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/util.h>

/**
 * Generic alias
 *
 * Type hints must be expressions. In Python 3.x, prior to 3.9, there was
 * a module (typing) that provided special objects for collections that
 * implemented subscripting to define generic types. In 3.9, the magic
 * method __class_getitem__ was used to provide this functionality directly
 * from the built-in collection types, so you can do list[int] and it spits
 * out a special object that reprs to an equivalent string, supports various
 * methods like bitwise-or for combination to produce complex types, and so
 * on... well, we're not going to do something quite that complicated at the
 * moment, but we did spend a bunch of time adding __class_getitem__ so we'll
 * at least provide a barebones generic alias that just returns a string.
 *
 * So if you specify list[int], you'll get 'list[int]'.
 */

static KrkValue typeToString(KrkValue val) {
	if (IS_CLASS(val)) {
		return OBJECT_VAL(AS_CLASS(val)->name);
	} else if (IS_STRING(val)) {
		return val;
	} else if (IS_TUPLE(val)) {
		/* Form a string by concatenating typeToString with ',' */
		struct StringBuilder sb = {0};

		for (size_t i = 0; i < AS_TUPLE(val)->values.count; ++i) {
			krk_push(typeToString(AS_TUPLE(val)->values.values[i]));
			pushStringBuilderStr(&sb, AS_CSTRING(krk_peek(0)), AS_STRING(krk_peek(0))->length);
			krk_pop();
			if (i < AS_TUPLE(val)->values.count - 1) {
				pushStringBuilder(&sb,',');
			}
		}

		return finishStringBuilder(&sb);
	} else {
		/* Just repr it. */
		KrkClass * type = krk_getType(val);
		if (!type->_reprer) return NONE_VAL();
		krk_push(val);
		return krk_callDirect(type->_reprer, 1);
	}
}

KRK_Function(__class_getitem__) {
	FUNCTION_TAKES_EXACTLY(2);
	if (!IS_CLASS(argv[0])) return TYPE_ERROR(class,argv[0]);

	struct StringBuilder sb = {0};

	/* First lets look at the class. */
	pushStringBuilderStr(&sb, AS_CLASS(argv[0])->name->chars, AS_CLASS(argv[0])->name->length);
	pushStringBuilder(&sb,'[');

	krk_push(typeToString(argv[1]));
	if (IS_STRING(krk_peek(0))) pushStringBuilderStr(&sb, AS_CSTRING(krk_peek(0)), AS_STRING(krk_peek(0))->length);
	krk_pop();
	pushStringBuilder(&sb,']');
	return finishStringBuilder(&sb);
}

NativeFn krk_GenericAlias = FUNC_NAME(krk,__class_getitem__);
