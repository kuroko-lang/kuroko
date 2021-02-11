#include <string.h>
#include "vm.h"
#include "value.h"
#include "memory.h"
#include "util.h"

static KrkValue FUNC_NAME(striterator,__init__)(int,KrkValue[],int);

#define CURRENT_CTYPE KrkString *
#define CURRENT_NAME  self

#define AT_END() (self->length == 0 || i == self->length - 1)

#define PUSH_CHAR(c) do { if (stringCapacity < stringLength + 1) { \
		size_t old = stringCapacity; stringCapacity = GROW_CAPACITY(old); \
		stringBytes = realloc(stringBytes, stringCapacity); \
	} stringBytes[stringLength++] = c; } while (0)

KRK_METHOD(str,__ord__,{
	METHOD_TAKES_NONE();
	if (self->codesLength != 1)
		return krk_runtimeError(vm.exceptions->typeError, "ord() expected a character, but string of length %d found", self->codesLength);
	return INTEGER_VAL(krk_unicodeCodepoint(self,0));
})

KRK_METHOD(str,__init__,{
	/* Ignore argument which would have been an instance */
	if (argc < 2) {
		return OBJECT_VAL(S(""));
	}
	METHOD_TAKES_AT_MOST(1);
	if (IS_STRING(argv[1])) return argv[1]; /* strings are immutable, so we can just return the arg */
	/* Find the type of arg */
	krk_push(argv[1]);
	if (!krk_getType(argv[1])->_tostr) return krk_runtimeError(vm.exceptions->typeError, "Can not convert %s to str", krk_typeName(argv[1]));
	return krk_callSimple(OBJECT_VAL(krk_getType(argv[1])->_tostr), 1, 0);
})

KRK_METHOD(str,__add__,{
	METHOD_TAKES_EXACTLY(1);
	const char * a;
	const char * b;
	size_t al;
	size_t bl;
	int needsPop = 0;

	a = AS_CSTRING(argv[0]);
	al = self->length;

	if (!IS_STRING(argv[1])) {
		KrkClass * type = krk_getType(argv[1]);
		if (type->_tostr) {
			krk_push(argv[1]);
			KrkValue result = krk_callSimple(OBJECT_VAL(type->_tostr), 1, 0);
			krk_push(result);
			needsPop = 1;
			if (!IS_STRING(result)) return krk_runtimeError(vm.exceptions->typeError, "__str__ produced something that was not a string: '%s'", krk_typeName(result));
			b = AS_CSTRING(result);
			bl = AS_STRING(result)->length;
		} else {
			b = krk_typeName(argv[1]);
			bl = strlen(b);
		}
	} else {
		b = AS_CSTRING(argv[1]);
		bl = AS_STRING(argv[1])->length;
	}

	size_t length = al + bl;
	char * chars = ALLOCATE(char, length + 1);
	memcpy(chars, a, al);
	memcpy(chars + al, b, bl);
	chars[length] = '\0';

	KrkString * result = krk_takeString(chars, length);
	if (needsPop) krk_pop();
	return OBJECT_VAL(result);
})

KRK_METHOD(str,__len__,{
	return INTEGER_VAL(self->codesLength);
})

KRK_METHOD(str,__set__,{
	return krk_runtimeError(vm.exceptions->typeError, "Strings are not mutable.");
})

/**
 * Unlike in Python, we actually handle negative values here rather than
 * somewhere else? I'm not even sure where Python does do it, but a quick
 * says not if you call __getslice__ directly...
 */
KRK_METHOD(str,__getslice__,{
	METHOD_TAKES_EXACTLY(2);
	if (!(IS_INTEGER(argv[1]) || IS_NONE(argv[1])) || !(IS_INTEGER(argv[2]) || IS_NONE(argv[2])))
		return krk_runtimeError(vm.exceptions->typeError, "slice: expected two integer arguments");
	/* bounds check */
	long start = IS_NONE(argv[1]) ? 0 : AS_INTEGER(argv[1]);
	long end   = IS_NONE(argv[2]) ? (long)self->codesLength : AS_INTEGER(argv[2]);
	if (start < 0) start = self->codesLength + start;
	if (start < 0) start = 0;
	if (end < 0) end = self->codesLength + end;
	if (start > (long)self->codesLength) start = self->codesLength;
	if (end > (long)self->codesLength) end = self->codesLength;
	if (end < start) end = start;
	long len = end - start;
	if (self->type == KRK_STRING_ASCII) {
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
})

/* str.__int__(base=10) */
KRK_METHOD(str,__int__,{
	METHOD_TAKES_AT_MOST(1);
	int base = (argc < 2 || !IS_INTEGER(argv[1])) ? 10 : (int)AS_INTEGER(argv[1]);
	char * start = AS_CSTRING(argv[0]);

	/*  These special cases for hexadecimal, binary, octal values. */
	if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
		base = 16;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'b' || start[1] == 'B')) {
		base = 2;
		start += 2;
	} else if (start[0] == '0' && (start[1] == 'o' || start[1] == 'O')) {
		base = 8;
		start += 2;
	}
	krk_integer_type value = parseStrInt(start, NULL, base);
	return INTEGER_VAL(value);
})

/* str.__float__() */
KRK_METHOD(str,__float__,{
	METHOD_TAKES_NONE();
	return FLOATING_VAL(strtod(AS_CSTRING(argv[0]),NULL));
})

KRK_METHOD(str,__get__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,int,krk_integer_type,asInt);
	if (asInt < 0) asInt += (int)AS_STRING(argv[0])->codesLength;
	if (asInt < 0 || asInt >= (int)AS_STRING(argv[0])->codesLength) {
		return krk_runtimeError(vm.exceptions->indexError, "String index out of range: %d", asInt);
	}
	if (self->type == KRK_STRING_ASCII) {
		return OBJECT_VAL(krk_copyString(self->chars + asInt, 1));
	} else {
		size_t offset = 0;
		size_t length = 0;
		/* Figure out where the UTF8 for this string starts. */
		krk_unicodeString(self);
		for (long i = 0; i < asInt; ++i) {
			uint32_t cp = KRK_STRING_FAST(self,i);
			offset += CODEPOINT_BYTES(cp);
		}
		uint32_t cp = KRK_STRING_FAST(self,asInt);
		length = CODEPOINT_BYTES(cp);
		return OBJECT_VAL(krk_copyString(self->chars + offset, length));
	}
})

/* str.format(**kwargs) */
KRK_METHOD(str,format,{
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
						positionalOffset = atoi(fieldStart);
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
						asString = krk_callSimple(OBJECT_VAL(type->_tostr), 1, 0);
					} else {
						if (!krk_bindMethod(type, AS_STRING(vm.specialMethodNames[METHOD_STR]))) {
							errorStr = "Failed to convert field to string.";
							goto _formatError;
						}
						asString = krk_callSimple(krk_peek(0), 0, 1);
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
})

KRK_METHOD(str,__mul__,{
	METHOD_TAKES_EXACTLY(1);
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
	return OBJECT_VAL(krk_copyString(out, totalLength));
})

/* str.join(list) */
KRK_METHOD(str,join,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,list,KrkList*,iterable);

	const char * errorStr = NULL;
	struct StringBuilder sb = {0};

	for (size_t i = 0; i < iterable->values.count; ++i) {
		KrkValue value = iterable->values.values[i];
		if (!IS_STRING(iterable->values.values[i])) {
			errorStr = krk_typeName(value);
			goto _expectedString;
		}
		krk_push(value);
		if (i > 0) {
			for (size_t j = 0; j < self->length; ++j) {
				pushStringBuilder(&sb, self->chars[j]);
			}
		}
		for (size_t j = 0; j < AS_STRING(value)->length; ++j) {
			pushStringBuilder(&sb, AS_STRING(value)->chars[j]);
		}
		krk_pop();
	}

	return finishStringBuilder(&sb);

_expectedString:
	krk_runtimeError(vm.exceptions->typeError, "Expected string, got %s.", errorStr);
	discardStringBuilder(&sb);
})

static int isWhitespace(char c) {
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

static int substringMatch(const char * haystack, size_t haystackLen, const char * needle, size_t needleLength) {
	if (haystackLen < needleLength) return 0;
	for (size_t i = 0; i < needleLength; ++i) {
		if (haystack[i] != needle[i]) return 0;
	}
	return 1;
}

/* str.__contains__ */
KRK_METHOD(str,__contains__,{
	METHOD_TAKES_EXACTLY(1);
	if (IS_NONE(argv[1])) return BOOLEAN_VAL(0);
	CHECK_ARG(1,str,KrkString*,needle);
	for (size_t i = 0; i < self->length; ++i) {
		if (substringMatch(self->chars + i, self->length - i, needle->chars, needle->length)) {
			return BOOLEAN_VAL(1);
		}
	}
	return BOOLEAN_VAL(0);
})

static int charIn(char c, const char * str) {
	for (const char * s = str; *s; s++) {
		if (c == *s) return 1;
	}
	return 0;
}

/**
 * Implements all three of strip, lstrip, rstrip.
 * Set which = 0, 1, 2 respectively
 */
static KrkValue _string_strip_shared(int argc, KrkValue argv[], int which) {
	if (argc > 1 && IS_STRING(argv[1]) && AS_STRING(argv[1])->type != KRK_STRING_ASCII) {
		return krk_runtimeError(vm.exceptions->notImplementedError, "str.strip() not implemented for Unicode strip lists");
	}
	size_t start = 0;
	size_t end   = AS_STRING(argv[0])->length;
	const char * subset = " \t\n\r";
	if (argc > 1) {
		if (IS_STRING(argv[1])) {
			subset = AS_CSTRING(argv[1]);
		} else {
			return krk_runtimeError(vm.exceptions->typeError, "argument to %sstrip() should be a string",
				(which == 0 ? "" : (which == 1 ? "l" : "r")));
		}
	} else if (argc > 2) {
		return krk_runtimeError(vm.exceptions->typeError, "%sstrip() takes at most one argument",
			(which == 0 ? "" : (which == 1 ? "l" : "r")));
	}
	if (which < 2) while (start < end && charIn(AS_CSTRING(argv[0])[start], subset)) start++;
	if (which != 1) while (end > start && charIn(AS_CSTRING(argv[0])[end-1], subset)) end--;
	return OBJECT_VAL(krk_copyString(&AS_CSTRING(argv[0])[start], end-start));
}

KRK_METHOD(str,strip,{
	METHOD_TAKES_AT_MOST(1); /* TODO */
	return _string_strip_shared(argc,argv,0);
})
KRK_METHOD(str,lstrip,{
	METHOD_TAKES_AT_MOST(1); /* TODO */
	return _string_strip_shared(argc,argv,1);
})
KRK_METHOD(str,rstrip,{
	METHOD_TAKES_AT_MOST(1); /* TODO */
	return _string_strip_shared(argc,argv,2);
})

KRK_METHOD(str,__lt__,{
	METHOD_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[1])) {
		return KWARGS_VAL(0); /* represents 'not implemented' */
	}
	if (AS_STRING(argv[0]) == AS_STRING(argv[1])) return BOOLEAN_VAL(0);

	size_t aLen = AS_STRING(argv[0])->length;
	size_t bLen = AS_STRING(argv[1])->length;
	const char * a = AS_CSTRING(argv[0]);
	const char * b = AS_CSTRING(argv[1]);

	for (size_t i = 0; i < (aLen < bLen) ? aLen : bLen; i++) {
		if (a[i] < b[i]) return BOOLEAN_VAL(1);
		if (a[i] > b[i]) return BOOLEAN_VAL(0);
	}

	return BOOLEAN_VAL((aLen < bLen));
})

KRK_METHOD(str,__gt__,{
	METHOD_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[1])) {
		return KWARGS_VAL(0); /* represents 'not implemented' */
	}
	if (AS_STRING(argv[0]) == AS_STRING(argv[1])) return BOOLEAN_VAL(0);

	size_t aLen = AS_STRING(argv[0])->length;
	size_t bLen = AS_STRING(argv[1])->length;
	const char * a = AS_CSTRING(argv[0]);
	const char * b = AS_CSTRING(argv[1]);

	for (size_t i = 0; i < (aLen < bLen) ? aLen : bLen; i++) {
		if (a[i] < b[i]) return BOOLEAN_VAL(0);
		if (a[i] > b[i]) return BOOLEAN_VAL(1);
	}

	return BOOLEAN_VAL((aLen > bLen));
})

/** TODO but throw a more descriptive error for now */
KRK_METHOD(str,__mod__,{
	return krk_runtimeError(vm.exceptions->notImplementedError, "%%-formatting for strings is not yet available");
})

/* str.split() */
KRK_METHOD(str,split,{
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
})

KRK_METHOD(str,replace,{
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
			for (char * o = newStr->chars; *o; o++ ){
				PUSH_CHAR(*o);
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
})

#define WRAP_INDEX(index) \
	if (index < 0) index += self->codesLength; \
	if (index < 0) index = 0; \
	if (index >= (krk_integer_type)self->codesLength) index = self->codesLength

KRK_METHOD(str,find,{
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
})

KRK_METHOD(str,index,{
	KrkValue result = FUNC_NAME(str,find)(argc,argv,hasKw);
	if (IS_INTEGER(result) && AS_INTEGER(result) == -1) {
		return krk_runtimeError(vm.exceptions->valueError, "substring not found");
	}
	return result;
})

KRK_METHOD(str,startswith,{
	METHOD_TAKES_EXACTLY(1); /* I know the Python versions of these take optional start, end... */
	CHECK_ARG(1,str,KrkString*,prefix);
	return BOOLEAN_VAL(substringMatch(self->chars,self->length,prefix->chars,prefix->length));
})

KRK_METHOD(str,endswith,{
	METHOD_TAKES_EXACTLY(1); /* I know the Python versions of these take optional start, end... */
	CHECK_ARG(1,str,KrkString*,suffix);
	if (suffix->length > self->length) return BOOLEAN_VAL(0);
	return BOOLEAN_VAL(substringMatch(self->chars + (self->length - suffix->length),
		suffix->length, suffix->chars, suffix->length));
})

/**
 * str.__repr__()
 *
 * Strings are special because __str__ should do nothing but __repr__
 * should escape characters like quotes.
 */
KRK_METHOD(str,__repr__,{
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
					sprintf(hex,"%02x", (unsigned char)*c);
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
})

KRK_METHOD(str,encode,{
	METHOD_TAKES_NONE();
	return OBJECT_VAL(krk_newBytes(AS_STRING(argv[0])->length, (uint8_t*)AS_CSTRING(argv[0])));
})

KRK_METHOD(str,__str__,{
	METHOD_TAKES_NONE();
	return argv[0];
})

void krk_addObjects(void) {
	KrkValue tmp = FUNC_NAME(str,__add__)(2, (KrkValue[]){krk_peek(1), krk_peek(0)},0);
	krk_pop(); krk_pop();
	krk_push(tmp);
}

KRK_METHOD(str,__iter__,{
	METHOD_TAKES_NONE();
	KrkInstance * output = krk_newInstance(vm.baseClasses->striteratorClass);

	krk_push(OBJECT_VAL(output));
	FUNC_NAME(striterator,__init__)(2, (KrkValue[]){krk_peek(0), argv[0]},0);
	krk_pop();

	return OBJECT_VAL(output);
})

#define CHECK_ALL(test) do { \
	krk_unicodeString(self); \
	for (size_t i = 0; i < self->codesLength; ++i) { \
		uint32_t c = KRK_STRING_FAST(self,i); \
		if (!(test)) { return BOOLEAN_VAL(0); } \
	} return BOOLEAN_VAL(1); } while (0)

KRK_METHOD(str,isalnum,{
	CHECK_ALL( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') );
})

KRK_METHOD(str,isalpha,{
	CHECK_ALL( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') );
})

KRK_METHOD(str,isdigit,{
	CHECK_ALL( (c >= '0' && c <= '9') );
})

KRK_METHOD(str,isxdigit,{
	CHECK_ALL( (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') );
})

KRK_METHOD(str,isspace, {
	CHECK_ALL( (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v') );
})

KRK_METHOD(str,islower, {
	CHECK_ALL( (c >= 'a' && c <= 'z') );
})

KRK_METHOD(str,isupper, {
	CHECK_ALL( (c >= 'A' && c <= 'Z') );
})

#undef CURRENT_CTYPE
#define CURRENT_CTYPE KrkInstance *
KRK_METHOD(striterator,__init__,{
	METHOD_TAKES_EXACTLY(1);
	CHECK_ARG(1,str,KrkString*,base);
	krk_push(OBJECT_VAL(self));
	krk_attachNamedObject(&self->fields, "s", (KrkObj*)base);
	krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(0));
	return krk_pop();
})

KRK_METHOD(striterator,__call__,{
	METHOD_TAKES_NONE();
	KrkValue _str;
	KrkValue _counter;
	const char * errorStr = NULL;
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("s")), &_str)) {
		errorStr = "no str pointer";
		goto _corrupt;
	}
	if (!krk_tableGet(&self->fields, OBJECT_VAL(S("i")), &_counter)) {
		errorStr = "no index";
		goto _corrupt;
	}

	if ((size_t)AS_INTEGER(_counter) >= AS_STRING(_str)->codesLength) {
		return argv[0];
	} else {
		krk_attachNamedValue(&self->fields, "i", INTEGER_VAL(AS_INTEGER(_counter)+1));
		return FUNC_NAME(str,__get__)(2,(KrkValue[]){_str,_counter},3);
	}
_corrupt:
	return krk_runtimeError(vm.exceptions->typeError, "Corrupt str iterator: %s", errorStr);
})

_noexport
void _createAndBind_strClass(void) {
	KrkClass * str = ADD_BASE_CLASS(vm.baseClasses->strClass, "str", vm.baseClasses->objectClass);
	BIND_METHOD(str,__init__);
	BIND_METHOD(str,__iter__);
	BIND_METHOD(str,__ord__);
	BIND_METHOD(str,__int__);
	BIND_METHOD(str,__float__);
	BIND_METHOD(str,__getslice__);
	BIND_METHOD(str,__get__);
	BIND_METHOD(str,__set__);
	BIND_METHOD(str,__add__);
	BIND_METHOD(str,__len__);
	BIND_METHOD(str,__mul__);
	BIND_METHOD(str,__contains__);
	BIND_METHOD(str,__lt__);
	BIND_METHOD(str,__gt__);
	BIND_METHOD(str,__mod__);
	BIND_METHOD(str,__repr__);
	BIND_METHOD(str,__str__);
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

	krk_defineNative(&str->methods,".__setslice__",FUNC_NAME(str,__set__));
	krk_defineNative(&str->methods,".__delslice__",FUNC_NAME(str,__set__));
	krk_defineNative(&str->methods,".__delitem__",FUNC_NAME(str,__set__));
	krk_finalizeClass(str);
	str->docstring = S("Obtain a string representation of an object.");

	KrkClass * striterator = ADD_BASE_CLASS(vm.baseClasses->striteratorClass, "striterator", vm.baseClasses->objectClass);
	BIND_METHOD(striterator,__init__);
	BIND_METHOD(striterator,__call__);
	krk_finalizeClass(striterator);
}

