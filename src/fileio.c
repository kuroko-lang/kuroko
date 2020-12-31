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

KrkValue krk_open(int argc, KrkValue argv[]) {
	/* How are we going to store files if we need to delete them at runtime
	 * when the GC finishes... maybe we could add a new object type that
	 * native modules can use that calls free() on an arbitrary pointer,
	 * or has a callback function that can do other things like closing...
	 * For now, let's just pretend it doesn't matter and let the user figure
	 * figure it out for themselves.
	 */
	if (argc < 1) {
		krk_runtimeError("open() takes at least 1 argument.");
		return NONE_VAL();
	}
	if (argc > 2) {
		krk_runtimeError("open() takes at most 2 argument.");
		return NONE_VAL();
	}

	if (!IS_STRING(argv[0])) {
		krk_runtimeError("open: first argument should be a filename string.");
		return NONE_VAL();
	}

	if (argc == 2 && !IS_STRING(argv[1])) {
		krk_runtimeError("open: second argument should be a mode string.");
		return NONE_VAL();
	}

	FILE * file = fopen(AS_CSTRING(argv[0]), AS_CSTRING(argv[1]));
	if (!file) {
		krk_runtimeError("open: failed to open file; system returned: %s", strerror(errno));
		return NONE_VAL();
	}

	/* Now let's build an object to hold it */
	KrkInstance * fileObject = krk_newInstance(FileClass);
	krk_push(OBJECT_VAL(fileObject));

	/* Let's put the filename in there somewhere... */
	krk_attachNamedValue(&fileObject->fields, "filename", argv[0]);
	krk_attachNamedValue(&fileObject->fields, "_fileptr", INTEGER_VAL((long)(file))); /* Need a KrkNativePrivate or something...  */

	krk_pop();
	return OBJECT_VAL(fileObject);
}

#define BLOCK_SIZE 1024

static FILE * getFilePtr(KrkValue obj) {
	KrkValue strFilePtr = OBJECT_VAL(S("_fileptr"));
	krk_push(strFilePtr);
	KrkInstance * me = AS_INSTANCE(obj);
	KrkValue _fileptr;
	if (!krk_tableGet(&me->fields, strFilePtr, &_fileptr)) {
		krk_runtimeError("Corrupt File object?");
		return NULL;
	}
	krk_pop(); /* str object */
	return (FILE *)AS_INTEGER(_fileptr);
}

static KrkValue krk_file_readline(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError("Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = getFilePtr(argv[0]);

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
	/* Make a new string to fit our output. */
	KrkString * out = krk_copyString(buffer,sizeRead);
	free(buffer);
	return OBJECT_VAL(out);
}

static KrkValue krk_file_read(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError("Not sure how that happened.");
		return NONE_VAL();
	}

	/* Get the file ptr reference */
	FILE * file = getFilePtr(argv[0]);

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
				krk_runtimeError("Read error.");
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
		krk_runtimeError("write: expected string");
		return NONE_VAL();
	}

	/* Find the file ptr reference */
	FILE * file = getFilePtr(argv[0]);

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_CSTRING(argv[1]), 1, AS_STRING(argv[1])->length, file));
}

static KrkValue krk_file_close(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError("Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = getFilePtr(argv[0]);
	if (!file) return NONE_VAL();

	fclose(file);

	return NONE_VAL();
}

static KrkValue krk_file_flush(int argc, KrkValue argv[]) {
	if (argc < 1 || !IS_INSTANCE(argv[0])) {
		krk_runtimeError("Not sure how that happened.");
		return NONE_VAL();
	}

	FILE * file = getFilePtr(argv[0]);
	if (!file) return NONE_VAL();

	fflush(file);

	return NONE_VAL();
}

static KrkValue krk_file_reject_init(int argc, KrkValue argv[]) {
	krk_runtimeError("File objects can not be instantiated; use fileio.open() to obtain File objects.");
	return NONE_VAL();
}

static void makeFileInstance(KrkInstance * module, const char name[], FILE * file) {
	KrkInstance * fileObject = krk_newInstance(FileClass);
	krk_push(OBJECT_VAL(fileObject));

	krk_attachNamedObject(&fileObject->fields, "filename", (KrkObj*)krk_copyString(name,strlen(name)));
	krk_attachNamedValue(&fileObject->fields, "_fileptr", INTEGER_VAL((long)(file)));

	krk_attachNamedObject(&module->fields, name, (KrkObj*)fileObject);
	krk_pop();
}

KrkValue krk_module_onload_fileio(void) {
	KrkInstance * module = krk_newInstance(vm.object_class);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	/* Define a class to represent files. (Should this be a helper method?) */
	const char chr_File[] = "File";
	KrkString * str_File = S(chr_File);
	krk_push(OBJECT_VAL(str_File));
	FileClass = krk_newClass(str_File);
	krk_push(OBJECT_VAL(FileClass));
	/* Bind it */
	krk_attachNamedObject(&module->fields,chr_File,(KrkObj*)FileClass);
	krk_pop();
	krk_pop();

	/* Add methods to it... */
	krk_defineNative(&FileClass->methods, ".read", krk_file_read);
	krk_defineNative(&FileClass->methods, ".readline", krk_file_readline);
	krk_defineNative(&FileClass->methods, ".close", krk_file_close);
	krk_defineNative(&FileClass->methods, ".write", krk_file_write);
	krk_defineNative(&FileClass->methods, ".flush", krk_file_flush);
	krk_defineNative(&FileClass->methods, ".__init__", krk_file_reject_init);

	/* Make an instance for stdout, stderr, and stdin */
	makeFileInstance(module, "stdin",stdin);
	makeFileInstance(module, "stdout",stdout);
	makeFileInstance(module, "stderr",stderr);

	/* Our base will be the open method */
	krk_defineNative(&module->fields, "open", krk_open);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}
