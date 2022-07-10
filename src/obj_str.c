#include <string.h>
#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

#include "private.h"

static KrkValue FUNC_NAME(striterator,__init__)(int,const KrkValue[],int);

#define CURRENT_CTYPE KrkString *
#define CURRENT_NAME  self

#define AT_END() (self->length == 0 || i == self->length - 1)

#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = GROW_ARRAY(char, stringBytes, old, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)

#define KRK_STRING_FAST(string,offset)  (uint32_t)\
	((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) <= (KRK_OBJ_FLAGS_STRING_UCS1) ? ((uint8_t*)string->codes)[offset] : \
	((string->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == (KRK_OBJ_FLAGS_STRING_UCS2) ? ((uint16_t*)string->codes)[offset] : \
	((uint32_t*)string->codes)[offset]))

#define CODEPOINT_BYTES(cp) (cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4)))

KRK_Method(str,__ord__) {
	METHOD_TAKES_NONE();
	if (self->codesLength != 1)
		return krk_runtimeError(vm.exceptions->typeError, "ord() expected a character, but string of length %d found", (int)self->codesLength);
	return INTEGER_VAL(krk_unicodeCodepoint(self,0));
}

KRK_Method(str,__init__) {
	/* Ignore argument which would have been an instance */
	if (argc < 2) {
		return OBJECT_VAL(S(""));
	}
	METHOD_TAKES_AT_MOST(1);
	if (IS_STRING(argv[1])) return argv[1]; /* strings are immutable, so we can just return the arg */
	/* Find the type of arg */
	krk_push(argv[1]);
	if (!krk_getType(argv[1])->_tostr) return krk_runtimeError(vm.exceptions->typeError, "Can not convert %s to str", krk_typeName(argv[1]));
	return krk_callDirect(krk_getType(argv[1])->_tostr, 1);
}

KRK_Method(str,__add__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,them);
	const char * a;
	const char * b;
	size_t al;
	size_t bl;
	int needsPop = 0;

	a = AS_CSTRING(argv[0]);
	al = self->length;

	b = AS_CSTRING(argv[1]);
	bl = them->length;

	size_t length = al + bl;
	char * chars = ALLOCATE(char, length + 1);
	memcpy(chars, a, al);
	memcpy(chars + al, b, bl);
	chars[length] = '\0';

	size_t cpLength = self->codesLength + them->codesLength;

	int self_type = (self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK);
	int them_type = (them->obj.flags & KRK_OBJ_FLAGS_STRING_MASK);

	KrkStringType type = self_type > them_type ? self_type : them_type;

	/* Hashes can be extended, which saves us calculating the whole thing */
	uint32_t hash = self->obj.hash;
	for (size_t i = 0; i < bl; ++i) {
		hash = (int)b[i] + (hash << 6) + (hash << 16) - hash;
	}

	KrkString * result = krk_takeStringVetted(chars, length, cpLength, type, hash);
	if (needsPop) krk_pop();
	return OBJECT_VAL(result);
}

KRK_Method(str,__hash__) {
	return INTEGER_VAL(self->obj.hash);
}

KRK_Method(str,__len__) {
	return INTEGER_VAL(self->codesLength);
}

KRK_Method(str,__setitem__) {
	return krk_runtimeError(vm.exceptions->typeError, "Strings are not mutable.");
}

/* str.__int__(base=10) */
KRK_Method(str,__int__) {
	METHOD_TAKES_AT_MOST(1);
	int base = (argc < 2 || !IS_INTEGER(argv[1])) ? 0 : (int)AS_INTEGER(argv[1]);
	return krk_parse_int(AS_CSTRING(argv[0]), AS_STRING(argv[0])->length, base);
}

/* str.__float__() */
KRK_Method(str,__float__) {
	METHOD_TAKES_NONE();
	return FLOATING_VAL(strtod(AS_CSTRING(argv[0]),NULL));
}

KRK_Method(str,__getitem__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) {
		CHECK_ARG(1,int,krk_integer_type,asInt);
		if (asInt < 0) asInt += (int)AS_STRING(argv[0])->codesLength;
		if (asInt < 0 || asInt >= (int)AS_STRING(argv[0])->codesLength) {
			return krk_runtimeError(vm.exceptions->indexError, "String index out of range: " PRIkrk_int, asInt);
		}
		if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_ASCII) {
			return OBJECT_VAL(krk_copyString(self->chars + asInt, 1));
		} else {
			krk_unicodeString(self);
			unsigned char asbytes[5];
			size_t length = krk_codepointToBytes(KRK_STRING_FAST(self,asInt),(unsigned char*)&asbytes);
			return OBJECT_VAL(krk_copyString((char*)&asbytes, length));
		}
	} else if (IS_slice(argv[1])) {
		KRK_SLICER(argv[1], self->codesLength) {
			return NONE_VAL();
		}

		if (step == 1) {
			long len = end - start;
			if ((self->obj.flags & KRK_OBJ_FLAGS_STRING_MASK) == KRK_OBJ_FLAGS_STRING_ASCII) {
				return OBJECT_VAL(krk_copyString(self->chars + start, len));
			} else {
				size_t offset = 0;
				size_t length = 0;
				/* Figure out where the UTF8 for this string starts. */
				krk_unicodeString(self);
				for (long i = 0; i < start; ++i) {
					uint32_t cp = KRK_STRING_FAST(self,i);
					offset += CODEPOINT_BYTES(cp);
				}
				for (long i = start; i < end; ++i) {
					uint32_t cp = KRK_STRING_FAST(self,i);
					length += CODEPOINT_BYTES(cp);
				}
				return OBJECT_VAL(krk_copyString(self->chars + offset, length));
			}
		} else {
			struct StringBuilder sb = {0};
			krk_unicodeString(self);

			unsigned char asbytes[5];
			krk_integer_type i = start;

			while ((step < 0) ? (i > end) : (i < end)) {
				size_t length = krk_codepointToBytes(KRK_STRING_FAST(self,i),(unsigned char*)&asbytes);
				pushStringBuilderStr(&sb, (char*)asbytes, length);
				i += step;
			}

			return finishStringBuilder(&sb);
		}
	} else {
		return TYPE_ERROR(int or slice, argv[1]);
	}
}

const char * krk_parseCommonFormatSpec(struct ParsedFormatSpec *result, const char * spec, size_t length);

KRK_Method(str,__format__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,format_spec);

	struct ParsedFormatSpec opts = {0};
	const char * spec = krk_parseCommonFormatSpec(&opts, format_spec->chars, format_spec->length);
	if (!spec) return NONE_VAL();

	switch (*spec) {
		case 0:   /* unspecified */
		case 's':
			break;
		default:
			return krk_runtimeError(vm.exceptions->valueError,
				"Unknown format code '%c' for object of type '%s'",
				*spec,
				"str");
	}

	/* Note we're going to deal in codepoints exclusive here, so hold on to your hat. */
	krk_unicodeString(self);

	size_t actualLength = self->codesLength;

	/* Restrict to the precision specified */
	if (opts.hasPrecision && (size_t)opts.prec < actualLength) {
		actualLength = opts.prec;
	}

	/* How much padding do we need? */
	size_t padLeft = 0;
	size_t padRight = 0;
	if (opts.hasWidth && actualLength < (size_t)opts.width) {
		if (!opts.align || opts.align == '<') {
			padRight = opts.width - actualLength;
		} else if (opts.align == '>' || opts.align == '=') {
			padLeft = opts.width - actualLength;
		} else if (opts.align == '^') {
			padLeft = (opts.width - actualLength) / 2;
			padRight = (opts.width - actualLength) - padLeft;
		}
	}

	/* If there's no work to do, return self */
	if (padLeft == 0 && padRight == 0 && actualLength == self->codesLength) {
		return argv[0];
	}

	struct StringBuilder sb = {0};

	/* Push left padding */
	for (size_t i = 0; i < padLeft; ++i) {
		pushStringBuilderStr(&sb, opts.fill, opts.fillSize);
	}

	/* Push codes from us */
	size_t offset = 0;
	for (size_t i = 0; i < actualLength; ++i) {
		uint32_t cp = KRK_STRING_FAST(self,i);
		size_t   bytes =  CODEPOINT_BYTES(cp);
		pushStringBuilderStr(&sb, &self->chars[offset], bytes);
		offset += bytes;
	}

	/* Push right padding */
	for (size_t i = 0; i < padRight; ++i) {
		pushStringBuilderStr(&sb, opts.fill, opts.fillSize);
	}

	return finishStringBuilder(&sb);
}

/* str.format(**kwargs) */
KRK_Method(str,format) {
	KrkValue kwargs = NONE_VAL();
	if (hasKw) {
		kwargs = argv[argc];
	}

	/* Read through `self` until we find a field specifier. */
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = 0;

	int counterOffset = 0;
	char * erroneousField = NULL;
	int erroneousIndex = -1;
	const char * errorStr = "";

	char * workSpace = strdup(self->chars);
	char * c = workSpace;
	for (size_t i = 0; i < self->length; i++, c++) {
		if (*c == '{') {
			if (!AT_END() && c[1] == '{') {
				PUSH_CHAR('{');
				i++; c++; /* Skip both */
				continue;
			} else {
				/* Start field specifier */
				i++; c++; /* Skip the { */
				char * fieldStart = c;
				char * fieldStop = NULL;
				for (; i < self->length; i++, c++) {
					if (*c == '}') {
						fieldStop = c;
						break;
					}
				}
				if (!fieldStop) {
					errorStr = "Unclosed { found.";
					goto _formatError;
				}
				size_t fieldLength = fieldStop - fieldStart;
				*fieldStop = '\0';
				/* fieldStart is now a nice little C string... */
				int isDigits = 1;
				for (char * field = fieldStart; *field; ++field) {
					if (!(*field >= '0' && *field <= '9')) {
						isDigits = 0;
						break;
					}
				}
				KrkValue value;
				if (isDigits) {
					/* Must be positional */
					int positionalOffset;
					if (fieldLength == 0) {
						positionalOffset = counterOffset++;
					} else if (counterOffset) {
						goto _formatSwitchedNumbering;
					} else {
						positionalOffset = strtoul(fieldStart,NULL,10);
					}
					if (positionalOffset >= argc - 1) {
						erroneousIndex = positionalOffset;
						goto _formatOutOfRange;
					}
					value = argv[1 + positionalOffset];
				} else if (hasKw) {
					KrkValue fieldAsString = OBJECT_VAL(krk_copyString(fieldStart, fieldLength));
					krk_push(fieldAsString);
					if (!krk_tableGet(AS_DICT(kwargs), fieldAsString, &value)) {
						erroneousField = fieldStart;
						goto _formatKeyError;
					}
					krk_pop(); /* fieldAsString */
				} else {
					erroneousField = fieldStart;
					goto _formatKeyError;
				}
				KrkValue asString;
				if (IS_STRING(value)) {
					asString = value;
				} else {
					krk_push(value);
					KrkClass * type = krk_getType(value);
					if (type->_tostr) {
						asString = krk_callDirect(type->_tostr, 1);
					} else {
						if (!krk_bindMethod(type, AS_STRING(vm.specialMethodNames[METHOD_STR]))) {
							errorStr = "Failed to convert field to string.";
							goto _formatError;
						}
						asString = krk_callStack(0);
					}
					if (!IS_STRING(asString)) goto _freeAndDone;
				}
				krk_push(asString);
				for (size_t i = 0; i < AS_STRING(asString)->length; ++i) {
					PUSH_CHAR(AS_CSTRING(asString)[i]);
				}
				krk_pop();
			}
		} else if (*c == '}') {
			if (!AT_END() && c[1] == '}') {
				PUSH_CHAR('}');
				i++; c++; /* Skip both */
				continue;
			} else {
				errorStr = "Single } found.";
				goto _formatError;
			}
		} else {
			PUSH_CHAR(*c);
		}
	}

	KrkValue out = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	free(workSpace);
	FREE_ARRAY(char,stringBytes,stringCapacity);
	return out;

_formatError:
	krk_runtimeError(vm.exceptions->typeError, "Error parsing format string: %s", errorStr);
	goto _freeAndDone;

_formatSwitchedNumbering:
	krk_runtimeError(vm.exceptions->valueError, "Can not switch from automatic indexing to manual indexing");
	goto _freeAndDone;

_formatOutOfRange:
	krk_runtimeError(vm.exceptions->indexError, "Positional index out of range: %d", erroneousIndex);
	goto _freeAndDone;

_formatKeyError:
	/* which one? */
	krk_runtimeError(vm.exceptions->keyError, "'%s'", erroneousField);
	goto _freeAndDone;

_freeAndDone:
	FREE_ARRAY(char,stringBytes,stringCapacity);
	free(workSpace);
	return NONE_VAL();
}

KRK_Method(str,__mul__) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_INTEGER(argv[1])) return NOTIMPL_VAL();
	CHECK_ARG(1,int,krk_integer_type,howMany);
	if (howMany < 0) howMany = 0;

	size_t totalLength = self->length * howMany;
	char * out = malloc(totalLength + 1);
	char * c = out;

	for (krk_integer_type i = 0; i < howMany; ++i) {
		for (size_t j = 0; j < self->length; ++j) {
			*(c++) = self->chars[j];
		}
	}

	*c = '\0';
	return OBJECT_VAL(krk_takeString(out, totalLength));
}

KRK_Method(str,__rmul__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_INTEGER(argv[1])) return FUNC_NAME(str,__mul__)(argc,argv,hasKw);
	return NOTIMPL_VAL();
}

struct _str_join_context {
	struct StringBuilder * sb;
	KrkString * self;
	int isFirst;
};

static int _str_join_callback(void * context, const KrkValue * values, size_t count) {
	struct _str_join_context * _context = context;

	for (size_t i = 0; i < count; ++i) {
		if (!IS_STRING(values[i])) {
			krk_runtimeError(vm.exceptions->typeError, "%s() expects %s, not '%s'",
				"join", "str", krk_typeName(values[i]));
			return 1;
		}

		if (_context->isFirst) {
			_context->isFirst = 0;
		} else {
			pushStringBuilderStr(_context->sb, (char*)_context->self->chars, _context->self->length);
		}
		pushStringBuilderStr(_context->sb, (char*)AS_STRING(values[i])->chars, AS_STRING(values[i])->length);
	}

	return 0;
}

/* str.join(list) */
KRK_Method(str,join) {
	METHOD_TAKES_EXACTLY(1);
	struct StringBuilder sb = {0};

	struct _str_join_context context = {&sb, self, 1};

	if (krk_unpackIterable(argv[1], &context, _str_join_callback)) {
		discardStringBuilder(&sb);
		return NONE_VAL();
	}

	return finishStringBuilder(&sb);
}

static int isWhitespace(char c) {
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}\

static int substringMatch(const char * haystack, size_t haystackLen, const char * needle, size_t needleLength) {
	if (haystackLen < needleLength) return 0;
	for (size_t i = 0; i < needleLength; ++i) {
		if (haystack[i] != needle[i]) return 0;
	}
	return 1;
}

/* str.__contains__ */
KRK_Method(str,__contains__) {
	METHOD_TAKES_EXACTLY(1);
	if (IS_NONE(argv[1])) return BOOLEAN_VAL(0);
	CHECK_ARG(1,str,KrkString*,needle);
	for (size_t i = 0; i < self->length; ++i) {
		if (substringMatch(self->chars + i, self->length - i, needle->chars, needle->length)) {
			return BOOLEAN_VAL(1);
		}
	}
	return BOOLEAN_VAL(0);
}

static int charIn(uint32_t c, KrkString * str) {
	for (size_t i = 0; i < str->codesLength; ++i) {
		if (c == KRK_STRING_FAST(str,i)) return 1;
	}
	return 0;
}

/**
 * Implements all three of strip, lstrip, rstrip.
 * Set which = 0, 1, 2 respectively
 */
static KrkValue _string_strip_shared(int argc, const KrkValue argv[], int which) {
	KrkString * subset = AS_STRING(vm.specialMethodNames[METHOD_STRSTRIP]);
	if (argc > 1) {
		if (IS_STRING(argv[1])) {
			subset = AS_STRING(argv[1]);
		} else {
			return krk_runtimeError(vm.exceptions->typeError, "argument to %sstrip() should be a string",
				(which == 0 ? "" : (which == 1 ? "l" : "r")));
		}
	}

	KrkString * self = AS_STRING(argv[0]);
	krk_unicodeString(self);
	krk_unicodeString(subset);

	uint32_t c;
	size_t start = 0;
	size_t end   = self->length;
	int j = 0;
	int k = self->codesLength - 1;

	if (which < 2) while (start < end && charIn((c = KRK_STRING_FAST(self, j)), subset)) { j++; start += CODEPOINT_BYTES(c); }
	if (which != 1) while (end > start && charIn((c = KRK_STRING_FAST(self, k)), subset)) { k--; end -= CODEPOINT_BYTES(c); }

	return OBJECT_VAL(krk_copyString(&self->chars[start], end-start));
}

KRK_Method(str,strip) {
	METHOD_TAKES_AT_MOST(1); /* TODO */
	return _string_strip_shared(argc,argv,0);
}
KRK_Method(str,lstrip) {
	METHOD_TAKES_AT_MOST(1); /* TODO */
	return _string_strip_shared(argc,argv,1);
}
KRK_Method(str,rstrip) {
	METHOD_TAKES_AT_MOST(1); /* TODO */
	return _string_strip_shared(argc,argv,2);
}

#define strCompare(name,lop,iop,rop) \
	KRK_Method(str,name) { \
		METHOD_TAKES_EXACTLY(1); \
		if (!IS_STRING(argv[1])) { \
			return NOTIMPL_VAL(); \
		} \
		size_t aLen = AS_STRING(argv[0])->length; \
		size_t bLen = AS_STRING(argv[1])->length; \
		const char * a = AS_CSTRING(argv[0]); \
		const char * b = AS_CSTRING(argv[1]); \
		for (size_t i = 0; i < ((aLen < bLen) ? aLen : bLen); i++) { \
			if (a[i] lop b[i]) return BOOLEAN_VAL(1); \
			if (a[i] iop b[i]) return BOOLEAN_VAL(0); \
		} \
		return BOOLEAN_VAL((aLen rop bLen)); \
	}

strCompare(__gt__,>,<,>)
strCompare(__lt__,<,>,<)
strCompare(__ge__,>,<,>=)
strCompare(__le__,<,>,<=)

KRK_Method(str,__mod__) {
	METHOD_TAKES_EXACTLY(1);

	KrkTuple * myTuple;

	if (IS_TUPLE(argv[1])) {
		myTuple = AS_TUPLE(argv[1]);
		krk_push(argv[1]);
	} else {
		myTuple = krk_newTuple(1);
		krk_push(OBJECT_VAL(myTuple));
		myTuple->values.values[myTuple->values.count++] = argv[1];
	}

	struct StringBuilder sb = {0};
	size_t ti = 0;

	for (size_t i = 0; i < self->length; ++i) {
		if (self->chars[i] == '%') {
			int backwards = 0;
			size_t width = 0;
			i++;

			if (self->chars[i] == '%') {
				pushStringBuilder(&sb, self->chars[i]);
				continue;
			}

			if (self->chars[i] == '-') { backwards = 1; i++; }

			while (self->chars[i] >= '0' && self->chars[i] <= '9') {
				width = width * 10 + (self->chars[i] - '0');
				i++;
			}

			if (self->chars[i] == 'i') {
				if (ti >= myTuple->values.count) goto _notEnough;
				KrkValue arg = myTuple->values.values[ti++];

				if (IS_INTEGER(arg)) {
					krk_push(INTEGER_VAL(AS_INTEGER(arg)));
				} else if (IS_FLOATING(arg)) {
					krk_push(INTEGER_VAL(AS_FLOATING(arg)));
				} else {
					krk_runtimeError(vm.exceptions->typeError, "%%i format: a number is required, not %s",
						krk_typeName(arg));
					goto _exception;
				}
				krk_push(krk_callDirect(krk_getType(arg)->_tostr, 1));
				goto _doit;
			} else if (self->chars[i] == 's') {
				if (ti >= myTuple->values.count) goto _notEnough;
				KrkValue arg = myTuple->values.values[ti++];
				if (!krk_getType(arg)->_tostr) {
					krk_runtimeError(vm.exceptions->typeError, "%%s format: cannot convert %s to string",
						krk_typeName(arg));
					goto _exception;
				}

				krk_push(arg);
				krk_push(krk_callDirect(krk_getType(arg)->_tostr, 1));
				goto _doit;
			} else {
				krk_runtimeError(vm.exceptions->typeError, "%%%c format string specifier unsupported",
					self->chars[i]);
				goto _exception;
			}

_doit:
			if (!backwards && width > AS_STRING(krk_peek(0))->codesLength) {
				while (width > AS_STRING(krk_peek(0))->codesLength) {
					pushStringBuilder(&sb, ' ');
					width--;
				}
			}

			pushStringBuilderStr(&sb, AS_CSTRING(krk_peek(0)), AS_STRING(krk_peek(0))->length);
			if (backwards && width > AS_STRING(krk_peek(0))->codesLength) {
				while (width > AS_STRING(krk_peek(0))->codesLength) {
					pushStringBuilder(&sb, ' ');
					width--;
				}
			}
			krk_pop();
		} else {
			pushStringBuilder(&sb, self->chars[i]);
		}
	}

	if (ti != myTuple->values.count) {
		krk_runtimeError(vm.exceptions->typeError, "not all arguments converted durin string formatting");
		goto _exception;
	}

	krk_pop(); /* tuple */
	return finishStringBuilder(&sb);

_notEnough:
	krk_runtimeError(vm.exceptions->typeError, "not enough arguments for string format");
	goto _exception;

_exception:
	discardStringBuilder(&sb);
	return NONE_VAL();
}

/* str.split() */
KRK_Method(str,split) {
	METHOD_TAKES_AT_MOST(2);
	if (argc > 1) {
		if (!IS_STRING(argv[1])) {
			return krk_runtimeError(vm.exceptions->typeError, "Expected separator to be a string");
		} else if (AS_STRING(argv[1])->length == 0) {
			return krk_runtimeError(vm.exceptions->valueError, "Empty separator");
		}
		if (argc > 2 && !IS_INTEGER(argv[2])) {
			return krk_runtimeError(vm.exceptions->typeError, "Expected maxsplit to be an integer.");
		} else if (argc > 2 && AS_INTEGER(argv[2]) == 0) {
			return argv[0];
		}
	}

	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	size_t i = 0;
	char * c = self->chars;
	size_t count = 0;

	if (argc < 2) {
		while (i != self->length) {
			while (i != self->length && isWhitespace(*c)) {
				i++; c++;
			}
			if (i != self->length) {
				size_t stringCapacity = 0;
				size_t stringLength   = 0;
				char * stringBytes    = NULL;
				while (i != self->length && !isWhitespace(*c)) {
					PUSH_CHAR(*c);
					i++; c++;
				}
				KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
				FREE_ARRAY(char,stringBytes,stringCapacity);
				krk_push(tmp);
				krk_writeValueArray(AS_LIST(myList), tmp);
				krk_pop();
			}
		}
	} else {
		while (i != self->length) {
			size_t stringCapacity = 0;
			size_t stringLength   = 0;
			char * stringBytes    = NULL;
			while (i != self->length && !substringMatch(c, self->length - i, AS_STRING(argv[1])->chars, AS_STRING(argv[1])->length)) {
				PUSH_CHAR(*c);
				i++; c++;
			}
			KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
			if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
			krk_push(tmp);
			krk_writeValueArray(AS_LIST(myList), tmp);
			krk_pop();
			if (substringMatch(c, self->length - i, AS_STRING(argv[1])->chars, AS_STRING(argv[1])->length)) {
				i += AS_STRING(argv[1])->length;
				c += AS_STRING(argv[1])->length;
				count++;
				if (argc > 2 && count == (size_t)AS_INTEGER(argv[2])) {
					size_t stringCapacity = 0;
					size_t stringLength   = 0;
					char * stringBytes    = NULL;
					while (i != self->length) {
						PUSH_CHAR(*c);
						i++; c++;
					}
					KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
					if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
					krk_push(tmp);
					krk_writeValueArray(AS_LIST(myList), tmp);
					krk_pop();
					break;
				}
				if (i == self->length) {
					KrkValue tmp = OBJECT_VAL(S(""));
					krk_push(tmp);
					krk_writeValueArray(AS_LIST(myList), tmp);
					krk_pop();
				}
			}
		}
	}

	krk_pop();
	return myList;
}

KRK_Method(str,replace) {
	METHOD_TAKES_AT_LEAST(2);
	METHOD_TAKES_AT_MOST(3);
	CHECK_ARG(1,str,KrkString*,oldStr);
	CHECK_ARG(2,str,KrkString*,newStr);
	KrkValue count = (argc > 3 && IS_INTEGER(argv[3])) ? argv[3] : NONE_VAL();
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = NULL;

	int replacements = 0;
	size_t i = 0;
	char * c = self->chars;
	while (i < self->length) {
		if ( substringMatch(c, self->length - i, oldStr->chars, oldStr->length) && (IS_NONE(count) || replacements < AS_INTEGER(count))) {
			for (size_t j = 0; j < newStr->length; j++) {
				PUSH_CHAR(newStr->chars[j]);
			}
			if (oldStr->length == 0) {
				PUSH_CHAR(*c);
				c++;
				i++;
			}
			c += oldStr->length;
			i += oldStr->length;
			replacements++;
		} else {
			PUSH_CHAR(*c);
			c++;
			i++;
		}
	}
	KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
	return tmp;
}

#define WRAP_INDEX(index) \
	if (index < 0) index += self->codesLength; \
	if (index < 0) index = 0; \
	if (index >= (krk_integer_type)self->codesLength) index = self->codesLength

KRK_Method(str,find) {
	METHOD_TAKES_AT_LEAST(1);
	METHOD_TAKES_AT_MOST(3);
	CHECK_ARG(1,str,KrkString*,substr);

	krk_integer_type start = 0;
	krk_integer_type end = self->codesLength;

	if (argc > 2) {
		if (IS_INTEGER(argv[2])) {
			start = AS_INTEGER(argv[2]);
		} else {
			return TYPE_ERROR(int,argv[2]);
		}
	}

	if (argc > 3) {
		if (IS_INTEGER(argv[3])) {
			end = AS_INTEGER(argv[3]);
		} else {
			return TYPE_ERROR(int,argv[3]);
		}
	}

	WRAP_INDEX(start);
	WRAP_INDEX(end);

	/* Make sure both strings have code representations */
	krk_unicodeString(self);
	krk_unicodeString(substr);

	for (krk_integer_type i = start; i < end; ++i) {
		krk_integer_type j;
		for (j = 0; j < (krk_integer_type)substr->codesLength && (i + j < end); ++j) {
			if (KRK_STRING_FAST(self,i+j) != KRK_STRING_FAST(substr,j)) break;
		}
		if (j == (krk_integer_type)substr->codesLength) return INTEGER_VAL(i);
	}

	return INTEGER_VAL(-1);
}

KRK_Method(str,index) {
	KrkValue result = FUNC_NAME(str,find)(argc,argv,hasKw);
	if (IS_INTEGER(result) && AS_INTEGER(result) == -1) {
		return krk_runtimeError(vm.exceptions->valueError, "substring not found");
	}
	return result;
}

KRK_Method(str,startswith) {
	METHOD_TAKES_EXACTLY(1); /* I know the Python versions of these take optional start, end... */
	CHECK_ARG(1,str,KrkString*,prefix);
	return BOOLEAN_VAL(substringMatch(self->chars,self->length,prefix->chars,prefix->length));
}

KRK_Method(str,endswith) {
	METHOD_TAKES_EXACTLY(1); /* I know the Python versions of these take optional start, end... */
	CHECK_ARG(1,str,KrkString*,suffix);
	if (suffix->length > self->length) return BOOLEAN_VAL(0);
	return BOOLEAN_VAL(substringMatch(self->chars + (self->length - suffix->length),
		suffix->length, suffix->chars, suffix->length));
}

/**
 * str.__repr__()
 *
 * Strings are special because __str__ should do nothing but __repr__
 * should escape characters like quotes.
 */
KRK_Method(str,__repr__) {
	METHOD_TAKES_NONE();
	size_t stringCapacity = 0;
	size_t stringLength   = 0;
	char * stringBytes    = NULL;

	char * end = AS_CSTRING(argv[0]) + AS_STRING(argv[0])->length;

	/* First count quotes */
	size_t singles = 0;
	size_t doubles = 0;
	for (char * c = AS_CSTRING(argv[0]); c < end; ++c) {
		if (*c == '\'') singles++;
		if (*c == '\"') doubles++;
	}

	char quote = (singles > doubles) ? '\"' : '\'';

	PUSH_CHAR(quote);

	for (char * c = AS_CSTRING(argv[0]); c < end; ++c) {
		switch (*c) {
			/* XXX: Other non-printables should probably be escaped as well. */
			case '\\': PUSH_CHAR('\\'); PUSH_CHAR('\\'); break;
			case '\'': if (quote == *c) { PUSH_CHAR('\\'); } PUSH_CHAR('\''); break;
			case '\"': if (quote == *c) { PUSH_CHAR('\\'); } PUSH_CHAR('\"'); break;
			case '\a': PUSH_CHAR('\\'); PUSH_CHAR('a'); break;
			case '\b': PUSH_CHAR('\\'); PUSH_CHAR('b'); break;
			case '\f': PUSH_CHAR('\\'); PUSH_CHAR('f'); break;
			case '\n': PUSH_CHAR('\\'); PUSH_CHAR('n'); break;
			case '\r': PUSH_CHAR('\\'); PUSH_CHAR('r'); break;
			case '\t': PUSH_CHAR('\\'); PUSH_CHAR('t'); break;
			case '\v': PUSH_CHAR('\\'); PUSH_CHAR('v'); break;
			case 27:   PUSH_CHAR('\\'); PUSH_CHAR('['); break;
			default: {
				if ((unsigned char)*c < ' ' || (unsigned char)*c == 0x7F) {
					PUSH_CHAR('\\');
					PUSH_CHAR('x');
					char hex[3];
					snprintf(hex, 3, "%02x", (unsigned char)*c);
					PUSH_CHAR(hex[0]);
					PUSH_CHAR(hex[1]);
				} else {
					PUSH_CHAR(*c);
				}
				break;
			}
		}
	}

	PUSH_CHAR(quote);
	KrkValue tmp = OBJECT_VAL(krk_copyString(stringBytes, stringLength));
	if (stringBytes) FREE_ARRAY(char,stringBytes,stringCapacity);
	return tmp;
}

KRK_Method(str,encode) {
	METHOD_TAKES_NONE();
	return OBJECT_VAL(krk_newBytes(AS_STRING(argv[0])->length, (uint8_t*)AS_CSTRING(argv[0])));
}

KRK_Method(str,__str__) {
	METHOD_TAKES_NONE();
	return argv[0];
}

void krk_addObjects(void) {
	KrkValue tmp = FUNC_NAME(str,__add__)(2, (KrkValue[]){krk_peek(1), krk_peek(0)},0);
	krk_pop(); krk_pop();
	krk_push(tmp);
}

KRK_Method(str,__iter__) {
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->striteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(striterator,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();

	return OBJECT_VAL(output);
}

#define CHECK_ALL(test) do { \
	krk_unicodeString(self); \
	for (size_t i = 0; i < self->codesLength; ++i) { \
		uint32_t c = KRK_STRING_FAST(self,i); \
		if (!(test)) { return BOOLEAN_VAL(0); } \
	} return BOOLEAN_VAL(1); } while (0)

KRK_Method(str,isalnum) {
	CHECK_ALL( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') );
}

KRK_Method(str,isalpha) {
	CHECK_ALL( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') );
}

KRK_Method(str,isdigit) {
	CHECK_ALL( (c >= '0' && c <= '9') );
}

KRK_Method(str,isxdigit) {
	CHECK_ALL( (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') );
}

KRK_Method(str,isspace) {
	CHECK_ALL( (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v') );
}

KRK_Method(str,islower) {
	CHECK_ALL( (c >= 'a' && c <= 'z') );
}

KRK_Method(str,isupper)  {
	CHECK_ALL( (c >= 'A' && c <= 'Z') );
}

KRK_Method(str,lower) {
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};

	for (size_t i = 0; i < self->length; ++i) {
		if (self->chars[i] >= 'A' && self->chars[i] <= 'Z') {
			pushStringBuilder(&sb, self->chars[i] + ('a' - 'A'));
		} else {
			pushStringBuilder(&sb, self->chars[i]);
		}
	}

	return finishStringBuilder(&sb);
}

KRK_Method(str,upper) {
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};

	for (size_t i = 0; i < self->length; ++i) {
		if (self->chars[i] >= 'a' && self->chars[i] <= 'z') {
			pushStringBuilder(&sb, self->chars[i] - ('a' - 'A'));
		} else {
			pushStringBuilder(&sb, self->chars[i]);
		}
	}

	return finishStringBuilder(&sb);
}

KRK_Method(str,title) {
	METHOD_TAKES_NONE();
	struct StringBuilder sb = {0};

	int lastWasWhitespace = 1;

	for (size_t i = 0; i < self->length; ++i) {
		if (lastWasWhitespace && self->chars[i] >= 'a' && self->chars[i] <= 'z') {
			pushStringBuilder(&sb, self->chars[i] - ('a' - 'A'));
			lastWasWhitespace = 0;
		} else if (!lastWasWhitespace && self->chars[i] >= 'A' && self->chars[i] <= 'Z') {
			pushStringBuilder(&sb, self->chars[i] + ('a' - 'A'));
			lastWasWhitespace = 0;
		} else {
			pushStringBuilder(&sb, self->chars[i]);
			lastWasWhitespace = !((self->chars[i] >= 'A' && self->chars[i] <= 'Z') || (self->chars[i] >= 'a' && self->chars[i] <= 'z'));
		}
	}

	return finishStringBuilder(&sb);
}

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkInstance *
KRK_Method(striterator,__init__) {
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,base);
	krk_push(OBJECT_VAL(self));
	krk_attachNamedObject(&self->fields, "s", (KrkObj*)base);
	krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(0));
	return krk_pop();
}

KRK_Method(striterator,__call__) {
	METHOD_TAKES_NONE();
	KrkValue _str;
	KrkValue _counter;
	const char * errorStr = NULL;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("s")), &_str) || !IS_STRING(_str)) {
		errorStr = "no str pointer";
		goto _corrupt;
	}
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("i")), &_counter) || !IS_INTEGER(_counter)) {
		errorStr = "no index";
		goto _corrupt;
	}

	if ((size_t)AS_INTEGER(_counter) >= AS_STRING(_str)->codesLength) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return FUNC_NAME(str,__getitem__)(2,(KrkValue[]){_str,_counter},3);
	}
_corrupt:
	return krk_runtimeError(vm.exceptions->typeError, "Corrupt str iterator: %s", errorStr);
}

_noexport
void _createAndBind_strClass(void) {
	KrkClass * str = ADD_BASE_CLASS(vm.baseClasses->strClass, "str", vm.baseClasses->objectClass);
	str->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(str,__init__);
	BIND_METHOD(str,__iter__);
	BIND_METHOD(str,__ord__);
	BIND_METHOD(str,__int__);
	BIND_METHOD(str,__float__);
	BIND_METHOD(str,__getitem__);
	BIND_METHOD(str,__setitem__);
	BIND_METHOD(str,__add__);
	BIND_METHOD(str,__len__);
	BIND_METHOD(str,__mul__);
	BIND_METHOD(str,__rmul__);
	BIND_METHOD(str,__contains__);
	BIND_METHOD(str,__lt__);
	BIND_METHOD(str,__gt__);
	BIND_METHOD(str,__le__);
	BIND_METHOD(str,__ge__);
	BIND_METHOD(str,__mod__);
	BIND_METHOD(str,__repr__);
	BIND_METHOD(str,__str__);
	BIND_METHOD(str,__hash__);
	BIND_METHOD(str,__format__);
	BIND_METHOD(str,encode);
	BIND_METHOD(str,split);
	BIND_METHOD(str,strip);
	BIND_METHOD(str,lstrip);
	BIND_METHOD(str,rstrip);
	BIND_METHOD(str,join);
	BIND_METHOD(str,format);
	BIND_METHOD(str,replace);
	BIND_METHOD(str,find);
	BIND_METHOD(str,index);
	BIND_METHOD(str,startswith);
	BIND_METHOD(str,endswith);

	/* TODO these are not properly Unicode-aware */
	BIND_METHOD(str,isalnum);
	BIND_METHOD(str,isalpha);
	BIND_METHOD(str,isdigit);
	BIND_METHOD(str,isxdigit);
	BIND_METHOD(str,isspace);
	BIND_METHOD(str,islower);
	BIND_METHOD(str,isupper);

	/* Not recommended in their current forms, but here for some Python compatibility */
	BIND_METHOD(str,lower);
	BIND_METHOD(str,upper);
	BIND_METHOD(str,title);

	krk_defineNative(&str->methods,"__delitem__",FUNC_NAME(str,__setitem__));
	krk_finalizeClass(str);
	KRK_DOC(str, "Obtain a string representation of an object.");

	KrkClass * striterator = ADD_BASE_CLASS(vm.baseClasses->striteratorClass, "striterator", vm.baseClasses->objectClass);
	striterator->obj.flags |= KRK_OBJ_FLAGS_NO_INHERIT;
	BIND_METHOD(striterator,__init__);
	BIND_METHOD(striterator,__call__);
	krk_finalizeClass(striterator);
}

