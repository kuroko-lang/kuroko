/**
 * Native module for providing access to stdio.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "../vm.h"
#include "../value.h"
#include "../object.h"

#define S(c) (krk_copyString(c,sizeof(c)-1))

static KrkClass * FileClass = NULL;
static KrkClass * BinaryFileClass = NULL;

KrkValue krk_open(int argc, KrkValue argv[]) {
	/* How are we going to store files if we need to delete them at runtime
	 * when the GC finishes... maybe we could add a new object type that
	 * native modules can use that calls free() on an arbitrary pointer,
	 * or has a callback function that can do other things like closing...
	 * For now, let's just pretend it doesn't matter and let the user figure
	 * figure it out for themselves.
	 */
	if (argc < 1) {
		krk_runtimeError(vm.exceptions.argumentError, "open() takes at least 1 argument.");
		return NONE_VAL();
	}
	if (argc > 2) {
		krk_runtimeError(vm.exceptions.argumentError, "open() takes at most 2 argument.");
		return NONE_VAL();
	}

	if (!IS_STRING(argv[0])) {
		krk_runtimeError(vm.exceptions.typeError, "open: first argument should be a filename string, not '%s'", krk_typeName(argv[0]));
		return NONE_VAL();
	}

	if (argc == 2 && !IS_STRING(argv[1])) {
		krk_runtimeError(vm.exceptions.typeError, "open: second argument should be a mode string, not '%s'", krk_typeName(argv[1]));
		return NONE_VAL();
	}

	KrkValue arg;
	int isBinary = 0;

	if (argc == 1) {
		arg = OBJECT_VAL(S("r"));
		krk_push(arg); /* Will be peeked to find arg string for fopen */
	} else {
		/* Check mode against allowable modes */
		if (AS_STRING(argv[1])->length == 0) {
			krk_runtimeError(vm.exceptions.typeError, "open: mode string must not be empty");
			return NONE_VAL();
		}
		for (size_t i = 0; i < AS_STRING(argv[1])->length-1; ++i) {
			if (AS_CSTRING(argv[1])[i] == 'b') {
				krk_runtimeError(vm.exceptions.typeError, "open: 'b' mode indicator must appear at end of mode string");
				return NONE_VAL();
			}
		}
		arg = argv[1];
		if (AS_CSTRING(argv[1])[AS_STRING(argv[1])->length-1] == 'b') {
			KrkValue tmp = OBJECT_VAL(krk_copyString(AS_CSTRING(argv[1]), AS_STRING(argv[1])->length-1));
			krk_push(tmp);
			isBinary = 1;
		} else {
			krk_push(arg);
		}
	}

	FILE * file = fopen(AS_CSTRING(argv[0]), AS_CSTRING(krk_peek(0)));
	if (!file) {
		krk_runtimeError(vm.exceptions.ioError, "open: failed to open file; system returned: %s", strerror(errno));
		return NONE_VAL();
	}

	/* Now let's build an object to hold it */
	KrkInstance * fileObject = krk_newInstance(isBinary ? BinaryFileClass : FileClass);
	krk_push(OBJECT_VAL(fileObject));

	/* Let's put the filename in there somewhere... */
	krk_attachNamedValue(&fileObject->fields, "filename", argv[0]);
	krk_attachNamedValue(&fileObject->fields, "modestr", arg);

	fileObject->_internal = file;

	krk_pop();
	krk_pop();
	return OBJECT_VAL(fileObject);
}

#define BLOCK_SIZE 1024

static KrkValue krk_file_str(int argc, KrkValue argv[]) {
	KrkInstance * fileObj = AS_INSTANCE(argv[0]);
	KrkValue filename, modestr;
	krk_tableGet(&fileObj->fields, OBJECT_VAL(S("filename")), &filename);
	krk_tableGet(&fileObj->fields, OBJECT_VAL(S("modestr")), &modestr);
	char * tmp = malloc(AS_STRING(filename)->length + AS_STRING(modestr)->length + 100); /* safety */
	sprintf(tmp, "<%s file '%s', mode '%s' at %p>", fileObj->_internal ? "open" : "closed", AS_CSTRING(filename), AS_CSTRING(modestr), (void*)fileObj);
	KrkString * out = krk_copyString(tmp, strlen(tmp));
	free(tmp);
	return OBJECT_VAL(out);
}

static KrkValue krk_file_readline(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError(vm.exceptions.baseException, "Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	size_t sizeRead = 0;
	size_t spaceAvailable = 0;
	char * buffer = NULL;

	do {
		if (spaceAvailable < sizeRead + BLOCK_SIZE) {
			spaceAvailable = (spaceAvailable ? spaceAvailable * 2 : (2 * BLOCK_SIZE));
			buffer = realloc(buffer, spaceAvailable);
		}

		char * target = &buffer[sizeRead];
		while (sizeRead < spaceAvailable) {
			int c = fgetc(file);
			if (c < 0) break;
			sizeRead++;
			*target++ = c;
			if (c == '\n') goto _finish_line;
		}
	} while (!feof(file));

_finish_line: (void)0;
	if (sizeRead == 0) {
		free(buffer);
		return NONE_VAL();
	}

	/* Make a new string to fit our output. */
	KrkString * out = krk_copyString(buffer,sizeRead);
	free(buffer);
	return OBJECT_VAL(out);
}

static KrkValue krk_file_readlines(int argc, KrkValue argv[]) {
	KrkValue myList = krk_list_of(0,NULL);
	krk_push(myList);

	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(myList)->_internal);

	for (;;) {
		KrkValue line = krk_file_readline(1, argv);
		if (IS_NONE(line)) break;

		krk_push(line);
		krk_writeValueArray(AS_LIST(_list_internal), line);
		krk_pop(); /* line */
	}

	krk_pop(); /* myList */
	return myList;
}

static KrkValue krk_file_read(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError(vm.exceptions.baseException, "Not sure how that happened.");
		return NONE_VAL();
	}

	/* Get the file ptr reference */
	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	/* We'll do our read entirely with some native buffers and manage them here. */
	size_t sizeRead = 0;
	size_t spaceAvailable = 0;
	char * buffer = NULL;

	do {
		if (spaceAvailable < sizeRead + BLOCK_SIZE) {
			spaceAvailable = (spaceAvailable ? spaceAvailable * 2 : (2 * BLOCK_SIZE));
			buffer = realloc(buffer, spaceAvailable);
		}

		char * target = &buffer[sizeRead];
		size_t newlyRead = fread(target, 1, BLOCK_SIZE, file);

		if (newlyRead < BLOCK_SIZE) {
			if (ferror(file)) {
				free(buffer);
				krk_runtimeError(vm.exceptions.ioError, "Read error.");
				return NONE_VAL();
			}
		}

		sizeRead += newlyRead;
	} while (!feof(file));

	/* Make a new string to fit our output. */
	KrkString * out = krk_copyString(buffer,sizeRead);
	free(buffer);
	return OBJECT_VAL(out);
}

static KrkValue krk_file_write(int argc, KrkValue argv[]) {
	/* Expect just a string as arg 2 */
	if (argc < 2 || !IS_INSTANCE(argv[0]) || !IS_STRING(argv[1])) {
		krk_runtimeError(vm.exceptions.typeError, "write: expected string");
		return NONE_VAL();
	}

	/* Find the file ptr reference */
	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_CSTRING(argv[1]), 1, AS_STRING(argv[1])->length, file));
}

static KrkValue krk_file_close(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError(vm.exceptions.baseException, "Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file) return NONE_VAL();

	fclose(file);

	AS_INSTANCE(argv[0])->_internal = NULL;

	return NONE_VAL();
}

static KrkValue krk_file_flush(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError(vm.exceptions.baseException, "Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file) return NONE_VAL();

	fflush(file);

	return NONE_VAL();
}

static KrkValue krk_file_reject_init(int argc, KrkValue argv[]) {
	krk_runtimeError(vm.exceptions.typeError, "File objects can not be instantiated; use fileio.open() to obtain File objects.");
	return NONE_VAL();
}

static KrkValue krk_file_enter(int argc, KrkValue argv[]) {
	/* Does nothing. */
	return NONE_VAL();
}

static KrkValue krk_file_exit(int argc, KrkValue argv[]) {
	/* Just an alias to close that triggers when a context manager is exited */
	return krk_file_close(argc,argv);
}

static void makeFileInstance(KrkInstance * module, const char name[], FILE * file) {
	KrkInstance * fileObject = krk_newInstance(FileClass);
	krk_push(OBJECT_VAL(fileObject));
	KrkValue filename = OBJECT_VAL(krk_copyString(name,strlen(name)));
	krk_push(filename);

	krk_attachNamedValue(&fileObject->fields, "filename", filename);
	fileObject->_internal = file;

	krk_attachNamedObject(&module->fields, name, (KrkObj*)fileObject);

	krk_pop(); /* filename */
	krk_pop(); /* fileObject */
}

static KrkValue krk_file_readline_b(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError(vm.exceptions.baseException, "Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	size_t sizeRead = 0;
	size_t spaceAvailable = 0;
	char * buffer = NULL;

	do {
		if (spaceAvailable < sizeRead + BLOCK_SIZE) {
			spaceAvailable = (spaceAvailable ? spaceAvailable * 2 : (2 * BLOCK_SIZE));
			buffer = realloc(buffer, spaceAvailable);
		}

		char * target = &buffer[sizeRead];
		while (sizeRead < spaceAvailable) {
			int c = fgetc(file);
			if (c < 0) break;
			sizeRead++;
			*target++ = c;
			if (c == '\n') goto _finish_line;
		}
	} while (!feof(file));

_finish_line: (void)0;
	if (sizeRead == 0) {
		free(buffer);
		return NONE_VAL();
	}

	/* Make a new string to fit our output. */
	KrkBytes * out = krk_newBytes(sizeRead, (unsigned char*)buffer);
	free(buffer);
	return OBJECT_VAL(out);
}

static KrkValue krk_file_readlines_b(int argc, KrkValue argv[]) {
	KrkValue myList = krk_list_of(0,NULL);
	krk_push(myList);

	KrkValue _list_internal = OBJECT_VAL(AS_INSTANCE(myList)->_internal);

	for (;;) {
		KrkValue line = krk_file_readline_b(1, argv);
		if (IS_NONE(line)) break;

		krk_push(line);
		krk_writeValueArray(AS_LIST(_list_internal), line);
		krk_pop(); /* line */
	}

	krk_pop(); /* myList */
	return myList;
}

static KrkValue krk_file_read_b(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError(vm.exceptions.baseException, "Not sure how that happened.");
		return NONE_VAL();
	}

	/* Get the file ptr reference */
	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	/* We'll do our read entirely with some native buffers and manage them here. */
	size_t sizeRead = 0;
	size_t spaceAvailable = 0;
	char * buffer = NULL;

	do {
		if (spaceAvailable < sizeRead + BLOCK_SIZE) {
			spaceAvailable = (spaceAvailable ? spaceAvailable * 2 : (2 * BLOCK_SIZE));
			buffer = realloc(buffer, spaceAvailable);
		}

		char * target = &buffer[sizeRead];
		size_t newlyRead = fread(target, 1, BLOCK_SIZE, file);

		if (newlyRead < BLOCK_SIZE) {
			if (ferror(file)) {
				free(buffer);
				krk_runtimeError(vm.exceptions.ioError, "Read error.");
				return NONE_VAL();
			}
		}

		sizeRead += newlyRead;
	} while (!feof(file));

	/* Make a new string to fit our output. */
	KrkBytes * out = krk_newBytes(sizeRead, (unsigned char*)buffer);
	free(buffer);
	return OBJECT_VAL(out);
}

static KrkValue krk_file_write_b(int argc, KrkValue argv[]) {
	/* Expect just a string as arg 2 */
	if (argc < 2 || !IS_INSTANCE(argv[0]) || !IS_BYTES(argv[1])) {
		krk_runtimeError(vm.exceptions.typeError, "write: expected bytes");
		return NONE_VAL();
	}

	/* Find the file ptr reference */
	FILE * file = AS_INSTANCE(argv[0])->_internal;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_BYTES(argv[1])->bytes, 1, AS_BYTES(argv[1])->length, file));
}

static void makeClass(KrkInstance * module, KrkClass ** _class, const char * name, KrkClass * base) {
	KrkString * str_Name = krk_copyString(name,strlen(name));
	krk_push(OBJECT_VAL(str_Name));
	*_class = krk_newClass(str_Name);
	krk_push(OBJECT_VAL(*_class));
	/* Bind it */
	krk_attachNamedObject(&module->fields,name,(KrkObj*)*_class);
	/* Inherit from object */
	krk_tableAddAll(&base->methods, &(*_class)->methods);
	krk_tableAddAll(&base->fields, &(*_class)->fields);
	krk_pop();
	krk_pop();
}

KrkValue krk_module_onload_fileio(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	/* Define a class to represent files. (Should this be a helper method?) */
	makeClass(module, &FileClass, "File", vm.objectClass);

	/* Add methods to it... */
	krk_defineNative(&FileClass->methods, ".read", krk_file_read);
	krk_defineNative(&FileClass->methods, ".readline", krk_file_readline);
	krk_defineNative(&FileClass->methods, ".readlines", krk_file_readlines);
	krk_defineNative(&FileClass->methods, ".write", krk_file_write);
	krk_defineNative(&FileClass->methods, ".close", krk_file_close);
	krk_defineNative(&FileClass->methods, ".flush", krk_file_flush);
	krk_defineNative(&FileClass->methods, ".__str__", krk_file_str);
	krk_defineNative(&FileClass->methods, ".__repr__", krk_file_str);
	krk_defineNative(&FileClass->methods, ".__init__", krk_file_reject_init);
	krk_defineNative(&FileClass->methods, ".__enter__", krk_file_enter);
	krk_defineNative(&FileClass->methods, ".__exit__", krk_file_exit);
	krk_finalizeClass(FileClass);

	makeClass(module, &BinaryFileClass, "BinaryFile", FileClass);
	krk_defineNative(&BinaryFileClass->methods, ".read", krk_file_read_b);
	krk_defineNative(&BinaryFileClass->methods, ".readline", krk_file_readline_b);
	krk_defineNative(&BinaryFileClass->methods, ".readlines", krk_file_readlines_b);
	krk_defineNative(&BinaryFileClass->methods, ".write", krk_file_write_b);
	krk_finalizeClass(BinaryFileClass);

	/* Make an instance for stdout, stderr, and stdin */
	makeFileInstance(module, "stdin", stdin);
	makeFileInstance(module, "stdout", stdout);
	makeFileInstance(module, "stderr", stderr);

	/* Our base will be the open method */
	krk_defineNative(&module->fields, "open", krk_open);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}
