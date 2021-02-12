/**
 * Bytecode Compiler for Kuroko
 *
 * Prototype bytecode marshaling tool to write binary forms of Kuroko source files.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/compiler.h>
#include <kuroko/util.h>

#include "simple-repl.h"

#ifdef ISDEBUG
#define DEBUGOUT(...) fprintf(stderr, __VAR_ARGS__)
#else
#define DEBUGOUT(...)
#endif

struct MarshalHeader {
	uint8_t  magic[4];   /* K R K B */
	uint8_t  version[4]; /* 1 0 1 0 */
} __attribute__((packed));

struct FunctionHeader {
	uint32_t nameInd;
	uint32_t docInd;
	uint16_t reqArgs;
	uint16_t kwArgs;
	uint16_t upvalues;
	uint32_t locals;
	uint32_t bcSize;
	uint32_t lmSize;
	uint32_t ctSize;
	uint8_t  flags;
	uint8_t  data[];
} __attribute__((packed));

struct LineMapEntry {
	uint16_t startOffset;
	uint16_t line;
} __attribute__((packed));

NativeFn ListPop;
NativeFn ListAppend;
NativeFn ListContains;
NativeFn ListIndex;
KrkValue SeenFunctions;
KrkValue UnseenFunctions;
KrkValue StringTable;

static void _initListFunctions(void) {
	KrkValue _list_pop;
	KrkValue _list_append;
	KrkValue _list_contains;
	KrkValue _list_index;
	krk_tableGet(&vm.baseClasses->listClass->methods, OBJECT_VAL(S("pop")), &_list_pop);
	krk_tableGet(&vm.baseClasses->listClass->methods, OBJECT_VAL(S("append")), &_list_append);
	krk_tableGet(&vm.baseClasses->listClass->methods, OBJECT_VAL(S("__contains__")), &_list_contains);
	krk_tableGet(&vm.baseClasses->listClass->methods, OBJECT_VAL(S("index")), &_list_index);
	ListPop = AS_NATIVE(_list_pop)->function;
	ListAppend = AS_NATIVE(_list_append)->function;
	ListContains = AS_NATIVE(_list_contains)->function;
	ListIndex = AS_NATIVE(_list_index)->function;
}

static void findInterpreter(char * argv[]) {
	/* Try asking /proc */
	char * binpath = realpath("/proc/self/exe", NULL);
	if (!binpath || (access(binpath, X_OK) != 0)) {
		if (strchr(argv[0], '/')) {
			binpath = realpath(argv[0], NULL);
		} else {
			/* Search PATH for argv[0] */
			char * _path = strdup(getenv("PATH"));
			char * path = _path;
			while (path) {
				char * next = strchr(path,':');
				if (next) *next++ = '\0';

				char tmp[4096];
				sprintf(tmp, "%s/%s", path, argv[0]);
				if (access(tmp, X_OK) == 0) {
					binpath = strdup(tmp);
					break;
				}
				path = next;
			}
			free(_path);
		}
	}
	if (binpath) {
		vm.binpath = binpath;
	} /* Else, give up at this point and just don't attach it at all. */
}

static KrkString ** myStrings = NULL;
static size_t available = 0;
static size_t count = 0;
static size_t internString(KrkString * str) {
	for (size_t i = 0; i < count; ++i) {
		if (myStrings[i] == str) return i;
	}

	if (count + 1 > available) {
		available = (available == 0) ? 8 : (available * 2);
		myStrings = realloc(myStrings,available * sizeof(KrkString*));
	}

	myStrings[count] = str;
	return count++;
}

static int doStringTable(FILE * out) {
	uint32_t stringCount = count;
	fwrite(&stringCount, 1, sizeof(uint32_t), out);

	for (size_t i = 0; i < count; ++i) {
		uint32_t strLen = myStrings[i]->length;
		fwrite(&strLen, 1, sizeof(uint32_t), out);
		fwrite(myStrings[i]->chars, 1, strLen, out);
	}

	return 0;
}

#define WRITE_INTEGER(i) _writeInteger(out, i)
static void _writeInteger(FILE* out, krk_integer_type i) {
	if (i >= 0 && i < 256) { \
		fwrite((uint8_t[]){'i',i}, 1, 2, out);
	} else {
		uint8_t data[9];
		data[0] = 'I';
		int64_t value = i;
		memcpy(&data[1], &value, sizeof(int64_t));
		fwrite(data, 1, 9, out);
	}
}

#define WRITE_FLOATING(f) _writeFloating(out, f)
static void _writeFloating(FILE * out, double f) {
	uint64_t doubleOut;
	memcpy(&doubleOut, &f, sizeof(double));
	fwrite("d", 1, 1, out);
	fwrite(&doubleOut, 1, sizeof(uint64_t), out);
}

#define WRITE_KWARGS(k) fwrite("k",1,1,out);

#define WRITE_STRING(s) _writeString(out, s)
static void _writeString(FILE * out, KrkString * s) {
	uint32_t ind = internString(s);
	if (ind < 256) {
		fwrite((uint8_t[]){'s',(uint8_t)ind}, 1, 2, out);
	} else {
		fwrite("S",1,1,out);
		fwrite(&ind,1,sizeof(uint32_t),out);
	}
}

#define WRITE_BYTES(b) _writeBytes(out,b)
static void _writeBytes(FILE * out, KrkBytes * b) {
	if (b->length < 256) {
		fwrite((uint8_t[]){'b', (uint8_t)b->length}, 1, 2, out);
		fwrite(b->bytes, 1, b->length, out);
	} else {
		fwrite("B",1,1,out);
		uint32_t len = b->length;
		fwrite(&len, 1, sizeof(uint32_t), out);
		fwrite(b->bytes, 1, b->length, out);
	}
}

#define WRITE_FUNCTION(f) _writeFunction(out,f)
static void _writeFunction(FILE * out, KrkFunction * f) {
	/* Find this function in the function table. */
	KrkValue this = OBJECT_VAL(f);
	KrkValue index = ListIndex(2,(KrkValue[]){SeenFunctions,this},0);
	if (!IS_INTEGER(index)) {
		fprintf(stderr, "Internal error: Expected int from list.index, got '%s'\n", krk_typeName(index));
		exit(1);
	}
	krk_integer_type i = AS_INTEGER(index);
	if (i < 0) {
		fprintf(stderr, "Internal error: expected an index, not %ld\n", (unsigned long)i);
		exit(1);
	}
	if (i < 256) {
		fwrite((uint8_t[]){'f',(uint8_t)i},1,2,out);
	} else {
		uint32_t val = i;
		fwrite("F",1,1,out);
		fwrite(&val,1,sizeof(uint32_t),out);
	}
}

static int doFirstPass(FILE * out) {
	/* Go through all functions and build string tables and function index */

	while (AS_LIST(UnseenFunctions)->count) {
		KrkValue nextFunc = ListPop(2,(KrkValue[]){UnseenFunctions,INTEGER_VAL(0)},0);
		krk_push(nextFunc);
		ListAppend(2,(KrkValue[]){SeenFunctions,nextFunc},0);

		/* Examine */
		KrkFunction * func = AS_FUNCTION(nextFunc);

		if (func->name) internString(func->name);
		if (func->docstring) internString(func->docstring);

		for (size_t i = 0; i < func->requiredArgNames.count; ++i) {
			internString(AS_STRING(func->requiredArgNames.values[i]));
		}

		for (size_t i = 0; i < func->keywordArgNames.count; ++i) {
			internString(AS_STRING(func->requiredArgNames.values[i]));
		}

		for (size_t i = 0; i < func->localNameCount; ++i) {
			internString(func->localNames[i].name);
		}

		for (size_t i = 0; i < func->chunk.constants.count; ++i) {
			KrkValue value = func->chunk.constants.values[i];
			if (IS_OBJECT(value)) {
				if (IS_STRING(value)) {
					internString(AS_STRING(value));
				} else if (IS_FUNCTION(value)) {
					/* If we haven't seen this function yet, append it to the list */
					krk_push(value);
					KrkValue boolResult = ListContains(2,(KrkValue[]){SeenFunctions,value},0);
					if (IS_BOOLEAN(boolResult) && AS_BOOLEAN(boolResult) == 0) {
						ListAppend(2,(KrkValue[]){UnseenFunctions,value},0);
					}
					krk_pop();
				}
			}
		}

		krk_pop();
	}

	return 0;
}

static int doSecondPass(FILE * out) {

	/* Write the function count */
	uint32_t functionCount = AS_LIST(SeenFunctions)->count;
	fwrite(&functionCount, 1, sizeof(uint32_t), out);

	for (size_t funcIndex = 0; funcIndex < AS_LIST(SeenFunctions)->count; ++funcIndex) {
		KrkFunction * func = AS_FUNCTION(AS_LIST(SeenFunctions)->values[funcIndex]);

		uint8_t flags = 0;
		if (func->collectsArguments) flags |= (1 << 0);
		if (func->collectsKeywords)  flags |= (1 << 1);

		struct FunctionHeader header = {
			func->name ? internString(func->name) : UINT32_MAX,
			func->docstring ? internString(func->docstring) : UINT32_MAX,
			func->requiredArgs,
			func->keywordArgs,
			func->upvalueCount,
			func->localNameCount,
			func->chunk.count,
			func->chunk.linesCount,
			func->chunk.constants.count,
			flags
		};

		fwrite(&header, 1, sizeof(struct FunctionHeader), out);

		/* Argument names first */
		for (size_t i = 0; i < (size_t)func->requiredArgs + !!(func->collectsArguments); ++i) {
			WRITE_STRING(AS_STRING(func->requiredArgNames.values[i]));
		}
		for (size_t i = 0; i < (size_t)func->keywordArgs + !!(func->collectsKeywords); ++i) {
			WRITE_STRING(AS_STRING(func->requiredArgNames.values[i]));
		}

		/* Bytecode operations next */
		fwrite(func->chunk.code, 1, func->chunk.count, out);

		/* Now let's do line references */
		for (size_t i = 0; i < func->chunk.linesCount; ++i) {
			struct LineMapEntry entry = {
				func->chunk.lines[i].startOffset,
				func->chunk.lines[i].line
			};
			fwrite(&entry, 1, sizeof(struct LineMapEntry), out);
		}

		for (size_t i = 0; i < func->chunk.constants.count; ++i) {
			KrkValue * val = &func->chunk.constants.values[i];
			switch (val->type) {
				case VAL_OBJECT:
					switch (AS_OBJECT(*val)->type) {
						case OBJ_STRING:
							WRITE_STRING(AS_STRING(*val));
							break;
						case OBJ_BYTES:
							WRITE_BYTES(AS_BYTES(*val));
							break;
						case OBJ_FUNCTION:
							WRITE_FUNCTION(AS_FUNCTION(*val));
							break;
						default:
							fprintf(stderr,
								"Invalid object found in constants table,"
								"this marashal format can not store '%s'\n",
								krk_typeName(*val));
							return 1;
					}
					break;
				case VAL_KWARGS:
					WRITE_KWARGS(AS_INTEGER(*val));
					fwrite("k", 1, 1, out);
					break;
				case VAL_INTEGER:
					WRITE_INTEGER(AS_INTEGER(*val));
					break;
				case VAL_FLOATING:
					WRITE_FLOATING(AS_FLOATING(*val));
					break;
				default:
					fprintf(stderr,
						"Invalid value found in constants table,"
						"this marashal format can not store '%s'\n",
						krk_typeName(*val));
					return 1;
			}
		}
	}

	return 0;
}

static int compileFile(char * fileName) {
	/* Compile source file */
	FILE * f = fopen(fileName, "r");
	if (!f) {
		fprintf(stderr, "%s: %s\n", fileName, strerror(errno));
		return 1;
	}

	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char * buf = malloc(size + 1);
	if (fread(buf, 1, size, f) != size) {
		fprintf(stderr, "%s: %s\n", fileName, strerror(errno));
		return 2;
	}
	fclose(f);
	buf[size] = '\0';

	FILE * out = fopen("out.kbc", "w");


	krk_startModule("__main__");
	KrkFunction * func = krk_compile(buf, 0, fileName);

	if (krk_currentThread.flags & KRK_HAS_EXCEPTION) {
		fprintf(stderr, "%s: exception during compilation:\n", fileName);
		krk_dumpTraceback();
		return 3;
	}

	/* Start with the primary header */
	struct MarshalHeader header = {
		{'K','R','K','B'},
		{'1','0','1','0'},
	};

	fwrite(&header, 1, sizeof(header), out);

	SeenFunctions = krk_list_of(0,NULL,0);
	krk_push(SeenFunctions);

	UnseenFunctions = krk_list_of(1,(KrkValue[]){OBJECT_VAL(func)},0);
	krk_push(UnseenFunctions);

	if (doFirstPass(out)) return 1;
	if (doStringTable(out)) return 1;
	if (doSecondPass(out)) return 1;

	krk_pop(); /* UnseenFunctions */
	krk_pop(); /* SeenFunctions */

	return 0;
}

static KrkValue valueFromConstant(int i, FILE * inFile) {
	uint8_t c = fgetc(inFile);
	DEBUGOUT("  %4lu: ", (unsigned long)i);
	switch (c) {
		case 'i':
		case 'I': {
			int64_t inVal = (c == 'i') ? fgetc(inFile) : 0;
			if (c == 'I') assert(fread(&inVal, 1, sizeof(int64_t), inFile) == sizeof(int64_t));
			DEBUGOUT("int %lld\n", (long long)inVal);
			return INTEGER_VAL(inVal);
		}
		case 's':
		case 'S': {
			uint32_t ind = (c == 's') ? fgetc(inFile) : 0;
			if (c == 'S') assert(fread(&ind, 1, sizeof(uint32_t), inFile) == sizeof(uint32_t));
			KrkValue valOut = AS_LIST(StringTable)->values[ind];
#ifdef ISDEBUG
			fprintf(stderr, "str #%lu ", (unsigned long)ind);
			krk_printValueSafe(stderr, valOut);
			fprintf(stderr, "\n");
#endif
			return valOut;
		}
		case 'd': {
			double val;
			assert(fread(&val, 1, sizeof(double), inFile) == sizeof(double));
			DEBUGOUT("float %g\n", val);
			return FLOATING_VAL(val);
		}
		case 'f':
		case 'F': {
			uint32_t ind = (c == 'f') ? fgetc(inFile) : 0;
			if (c == 'F') assert(fread(&ind, 1, sizeof(uint32_t), inFile) == sizeof(uint32_t));
			DEBUGOUT("function #%lu\n", (unsigned long)ind);
			return AS_LIST(SeenFunctions)->values[ind];
		}
		default: {
			fprintf(stderr, "Unknown type '%c'.\n", c);
			return NONE_VAL();
		}
	}
}

static int readFile(char * fileName) {

	FILE * inFile = fopen(fileName, "r");
	if (!inFile) {
		fprintf(stderr, "%s: %s\n", fileName, strerror(errno));
		return 1;
	}

	krk_startModule("__main__");

	StringTable = krk_list_of(0,NULL,0);
	krk_push(StringTable);

	SeenFunctions = krk_list_of(0,NULL,0);
	krk_push(SeenFunctions);

	struct MarshalHeader header;
	assert(fread(&header, 1, sizeof(header), inFile) == sizeof(header));

	if (memcmp(header.magic,(uint8_t[]){'K','R','K','B'},4) != 0)
		return fprintf(stderr, "Invalid header.\n"), 1;

	if (memcmp(header.version,(uint8_t[]){'1','0','1','0'},4) != 0)
		return fprintf(stderr, "Bytecode is for a different version.\n"), 2;

	/* Read string table */
	uint32_t stringCount;
	assert(fread(&stringCount, 1, sizeof(uint32_t), inFile) == sizeof(uint32_t));

	DEBUGOUT("[String Table (count=%lu)]\n", (unsigned long)stringCount);
	for (size_t i = 0; i < (size_t)stringCount; ++i) {
		uint32_t strLen;
		assert(fread(&strLen, 1, sizeof(uint32_t), inFile) == sizeof(uint32_t));

		char * strVal = malloc(strLen+1);
		assert(fread(strVal, 1, strLen, inFile) == strLen);
		strVal[strLen] = '\0';

		/* Create a string */
		krk_push(OBJECT_VAL(krk_takeString(strVal,strLen)));
		ListAppend(2,(KrkValue[]){StringTable, krk_peek(0)},0);
#ifdef ISDEBUG
		fprintf(stderr, "%04lu: ", (unsigned long)i);
		krk_printValueSafe(stderr, krk_peek(0));
		fprintf(stderr, " (len=%lu)\n", (unsigned long)strLen);
#endif
		krk_pop();
	}

	uint32_t functionCount;
	assert(fread(&functionCount, 1, sizeof(uint32_t), inFile) == sizeof(uint32_t));

	DEBUGOUT("[Code Objects (count=%lu)]\n", (unsigned long)functionCount);

	for (size_t i = 0; i < (size_t)functionCount; ++i) {
		krk_push(OBJECT_VAL(krk_newFunction()));
		ListAppend(2,(KrkValue[]){SeenFunctions, krk_peek(0)}, 0);
		krk_pop();
	}

	for (size_t i = 0; i < (size_t)functionCount; ++i) {

		KrkFunction * self = AS_FUNCTION(AS_LIST(SeenFunctions)->values[i]);

		struct FunctionHeader function;
		assert(fread(&function, 1, sizeof(function), inFile) == sizeof(function));

		if (function.nameInd != UINT32_MAX) {
			self->name = AS_STRING(AS_LIST(StringTable)->values[function.nameInd]);
		} else {
			self->name = S("__main__");
		}

#ifdef ISDEBUG
		fprintf(stderr, "<");
		krk_printValueSafe(stderr,OBJECT_VAL(self->name));
		fprintf(stderr, ">\n");
#endif

		if (function.docInd != UINT32_MAX) {
			self->docstring = AS_STRING(AS_LIST(StringTable)->values[function.docInd]);
		}

#ifdef ISDEBUG
		fprintf(stderr, "   Required arguments: %lu\n", (unsigned long)function.reqArgs);
		fprintf(stderr, "   Keyword arguments:  %lu\n", (unsigned long)function.kwArgs);
		fprintf(stderr, "   Named locals:       %lu\n", (unsigned long)function.locals);
		fprintf(stderr, "   Bytes of bytecode:  %lu\n", (unsigned long)function.bcSize);
		fprintf(stderr, "   Line mappings:      %lu\n", (unsigned long)function.lmSize);
		fprintf(stderr, "   Constants:          %lu\n", (unsigned long)function.ctSize);
#endif

		self->requiredArgs = function.reqArgs;
		self->keywordArgs  = function.kwArgs;
		self->collectsArguments = (function.flags & (1 << 0)) ? 1 : 0;
		self->collectsKeywords  = (function.flags & (1 << 1)) ? 1 : 0;
		self->globalsContext = krk_currentThread.module;
		self->upvalueCount = function.upvalues;

		/* Read argument names */
		DEBUGOUT("  [Required Arguments]\n");
		for (size_t i = 0; i < (size_t)function.reqArgs + self->collectsArguments; i++) {
			krk_writeValueArray(&self->requiredArgNames, valueFromConstant(i,inFile));
		}

		DEBUGOUT("  [Keyword Arguments]\n");
		for (size_t i = 0; i < (size_t)function.kwArgs + self->collectsKeywords; i++) {
			krk_writeValueArray(&self->keywordArgNames, valueFromConstant(i,inFile));
		}

		/* Skip bytecode for now, we'll look at it later */
		self->chunk.capacity = function.bcSize;
		self->chunk.code = malloc(self->chunk.capacity);
		assert(fread(self->chunk.code, 1, self->chunk.capacity, inFile) == self->chunk.capacity);
		self->chunk.count = self->chunk.capacity;

		self->chunk.linesCapacity = function.lmSize;
		self->chunk.lines = malloc(sizeof(KrkLineMap) * function.lmSize);
		/* Examine line mappings */
		DEBUGOUT("  [Line Mapping]\n");
		for (size_t i = 0; i < function.lmSize; ++i) {
			struct LineMapEntry entry;
			assert(fread(&entry,1,sizeof(struct LineMapEntry),inFile) == sizeof(struct LineMapEntry));


			DEBUGOUT("  %4lu = 0x%04lx\n", (unsigned long)entry.line, (unsigned long)entry.startOffset);

			self->chunk.lines[i].startOffset = entry.startOffset;
			self->chunk.lines[i].line = entry.line;
		}
		self->chunk.linesCount = self->chunk.linesCapacity;

		/* Read constants */
		DEBUGOUT("  [Constants Table]\n");
		for (size_t i = 0; i < function.ctSize; i++) {
			krk_writeValueArray(&self->chunk.constants, valueFromConstant(i, inFile));
		}
	}

	/* Now we can move the first function up and call it to initialize a module */
	krk_pop();
	krk_pop();
	krk_push(AS_LIST(SeenFunctions)->values[0]);

	KrkClosure * closure = krk_newClosure(AS_FUNCTION(krk_peek(0)));
	krk_pop();
	krk_push(OBJECT_VAL(closure));

	krk_callValue(OBJECT_VAL(closure), 0, 1);

	/* TODO: Load module into module table */
	KrkValue result = krk_runNext();
	if (IS_INTEGER(result)) return AS_INTEGER(result);
	else {
		return runSimpleRepl();
	}
}

int main(int argc, char * argv[]) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s path-to-file.krk\n"
		                "       %s -r path-to-file.kbc\n",
		                argv[0], argv[0]);
		return 1;
	}

	/* Initialize a VM */
	findInterpreter(argv);
	krk_initVM(0);
	_initListFunctions();

	if (argc < 3) {
		return compileFile(argv[1]);
	} else if (argc == 3 && !strcmp(argv[1],"-r")) {
		return readFile(argv[2]);
	}

	return 1;
}
