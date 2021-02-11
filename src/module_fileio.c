/**
 * Native module for providing access to stdio.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include "vm.h"
#include "value.h"
#include "object.h"
#include "memory.h"
#include "util.h"

static KrkClass * File = NULL;
static KrkClass * BinaryFile = NULL;
struct File {
	KrkInstance inst;
	FILE * filePtr;
};

#define IS_File(o) (krk_isInstanceOf(o, File))
#define AS_File(o) ((struct File*)AS_OBJECT(o))

#define IS_BinaryFile(o) (krk_isInstanceOf(o, BinaryFile))
#define AS_BinaryFile(o) ((struct File*)AS_OBJECT(o))

static KrkClass * Directory = NULL;
struct Directory {
	KrkInstance inst;
	DIR * dirPtr;
};

#define IS_Directory(o) (krk_isInstanceOf(o,Directory))
#define AS_Directory(o) ((struct Directory*)AS_OBJECT(o))


#define CURRENT_CTYPE struct File *
#define CURRENT_NAME  self

KRK_FUNC(open,{
	FUNCTION_TAKES_AT_LEAST(1);
	FUNCTION_TAKES_AT_MOST(2);
	CHECK_ARG(0,str,KrkString*,filename);
	if (argc == 2 && !IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
	KrkValue arg;
	int isBinary = 0;
	if (argc == 1) {
		arg = OBJECT_VAL(S("r"));
		krk_push(arg); /* Will be peeked to find arg string for fopen */
	} else {
		/* Check mode against allowable modes */
		if (AS_STRING(argv[1])->length == 0) return krk_runtimeError(vm.exceptions->typeError, "open: mode string must not be empty");
		for (size_t i = 0; i < AS_STRING(argv[1])->length-1; ++i) {
			if (AS_CSTRING(argv[1])[i] == 'b') {
				return krk_runtimeError(vm.exceptions->typeError, "open: 'b' mode indicator must appear at end of mode string");
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

	FILE * file = fopen(filename->chars, AS_CSTRING(krk_peek(0)));
	if (!file) return krk_runtimeError(vm.exceptions->ioError, "open: failed to open file; system returned: %s", strerror(errno));

	/* Now let's build an object to hold it */
	KrkInstance * fileObject = krk_newInstance(isBinary ? BinaryFile : File);
	krk_push(OBJECT_VAL(fileObject));

	/* Let's put the filename in there somewhere... */
	krk_attachNamedValue(&fileObject->fields, "filename", OBJECT_VAL(filename));
	krk_attachNamedValue(&fileObject->fields, "modestr", arg);

	((struct File*)fileObject)->filePtr = file;

	krk_pop();
	krk_pop();
	return OBJECT_VAL(fileObject);
})

#define BLOCK_SIZE 1024

KRK_METHOD(File,__str__,{
	METHOD_TAKES_NONE();
	KrkValue filename;
	KrkValue modestr;
	if (!krk_tableGet(&self->inst.fields, OBJECT_VAL(S("filename")), &filename) || !IS_STRING(filename)) return krk_runtimeError(vm.exceptions->baseException, "Corrupt File");
	if (!krk_tableGet(&self->inst.fields, OBJECT_VAL(S("modestr")), &modestr) || !IS_STRING(modestr)) return krk_runtimeError(vm.exceptions->baseException, "Corrupt File");
	char * tmp = malloc(AS_STRING(filename)->length + AS_STRING(modestr)->length + 100); /* safety */
	sprintf(tmp, "<%s file '%s', mode '%s' at %p>", self->filePtr ? "open" : "closed", AS_CSTRING(filename), AS_CSTRING(modestr), (void*)self);
	KrkString * out = krk_copyString(tmp, strlen(tmp));
	free(tmp);
	return OBJECT_VAL(out);
})

KRK_METHOD(File,readline,{
	METHOD_TAKES_NONE();
	FILE * file = self->filePtr;

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
})

KRK_METHOD(File,readlines,{
	METHOD_TAKES_NONE();
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	for (;;) {
		KrkValue line = FUNC_NAME(File,readline)(1, argv, 0);
		if (IS_NONE(line)) break;

		krk_push(line);
		krk_writeValueArray(AS_LIST(myList), line);
		krk_pop(); /* line */
	}

	krk_pop(); /* myList */
	return myList;
})

KRK_METHOD(File,read,{
	METHOD_TAKES_AT_MOST(1);

	krk_integer_type sizeToRead = -1;
	if (argc > 1) {
		CHECK_ARG(1,int,krk_integer_type,sizeFromArg);
		if (sizeFromArg < -1) return krk_runtimeError(vm.exceptions->valueError, "size must be >= -1");
		sizeToRead = sizeFromArg;
	}

	/* Get the file ptr reference */
	FILE * file = self->filePtr;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	/* We'll do our read entirely with some native buffers and manage them here. */
	size_t sizeRead = 0;
	size_t spaceAvailable = 0;
	char * buffer = NULL;

	if (sizeToRead == -1) {
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
					return krk_runtimeError(vm.exceptions->ioError, "Read error.");
				}
			}

			sizeRead += newlyRead;
		} while (!feof(file));
	} else {
		spaceAvailable = sizeToRead;
		buffer = realloc(buffer, spaceAvailable);
		sizeRead = fread(buffer, 1, sizeToRead, file);
	}

	/* Make a new string to fit our output. */
	KrkString * out = krk_copyString(buffer,sizeRead);
	free(buffer);
	return OBJECT_VAL(out);
})

KRK_METHOD(File,write,{
	METHOD_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
	/* Find the file ptr reference */
	FILE * file = self->filePtr;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_CSTRING(argv[1]), 1, AS_STRING(argv[1])->length, file));
})

KRK_METHOD(File,close,{
	METHOD_TAKES_NONE();
	FILE * file = self->filePtr;
	if (file) fclose(file);
	self->filePtr = NULL;
})

KRK_METHOD(File,flush,{
	METHOD_TAKES_NONE();
	FILE * file = self->filePtr;
	if (file) fflush(file);
})

KRK_METHOD(File,__init__,{
	return krk_runtimeError(vm.exceptions->typeError, "File objects can not be instantiated; use fileio.open() to obtain File objects.");
})

KRK_METHOD(File,__enter__,{})
KRK_METHOD(File,__exit__,{
	return FUNC_NAME(File,close)(argc,argv,0);
})

static void makeFileInstance(KrkInstance * module, const char name[], FILE * file) {
	KrkInstance * fileObject = krk_newInstance(File);
	krk_push(OBJECT_VAL(fileObject));
	KrkValue filename = OBJECT_VAL(krk_copyString(name,strlen(name)));
	krk_push(filename);

	krk_attachNamedValue(&fileObject->fields, "filename", filename);
	((struct File*)fileObject)->filePtr = file;

	krk_attachNamedObject(&module->fields, name, (KrkObj*)fileObject);

	krk_pop(); /* filename */
	krk_pop(); /* fileObject */
}

KRK_METHOD(BinaryFile,readline,{
	METHOD_TAKES_NONE();
	FILE * file = self->filePtr;

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
})

KRK_METHOD(BinaryFile,readlines,{
	METHOD_TAKES_NONE();
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	for (;;) {
		KrkValue line = FUNC_NAME(BinaryFile,readline)(1, argv, 0);
		if (IS_NONE(line)) break;

		krk_push(line);
		krk_writeValueArray(AS_LIST(myList), line);
		krk_pop(); /* line */
	}

	krk_pop(); /* myList */
	return myList;
})

KRK_METHOD(BinaryFile,read,{
	METHOD_TAKES_AT_MOST(1);

	krk_integer_type sizeToRead = -1;
	if (argc > 1) {
		CHECK_ARG(1,int,krk_integer_type,sizeFromArg);
		if (sizeFromArg < -1) return krk_runtimeError(vm.exceptions->valueError, "size must be >= -1");
		sizeToRead = sizeFromArg;
	}

	/* Get the file ptr reference */
	FILE * file = self->filePtr;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	/* We'll do our read entirely with some native buffers and manage them here. */
	size_t sizeRead = 0;
	size_t spaceAvailable = 0;
	char * buffer = NULL;

	if (sizeToRead == -1) {
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
					return krk_runtimeError(vm.exceptions->ioError, "Read error.");
				}
			}

			sizeRead += newlyRead;
		} while (!feof(file));
	} else {
		spaceAvailable = sizeToRead;
		buffer = realloc(buffer, spaceAvailable);
		sizeRead = fread(buffer, 1, sizeToRead, file);
	}

	/* Make a new string to fit our output. */
	KrkBytes * out = krk_newBytes(sizeRead, (unsigned char*)buffer);
	free(buffer);
	return OBJECT_VAL(out);
})

KRK_METHOD(BinaryFile,write,{
	METHOD_TAKES_EXACTLY(1);
	if (!IS_BYTES(argv[1])) return TYPE_ERROR(bytes,argv[1]);
	/* Find the file ptr reference */
	FILE * file = self->filePtr;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_BYTES(argv[1])->bytes, 1, AS_BYTES(argv[1])->length, file));
})

#undef CURRENT_CTYPE

static void _file_sweep(KrkInstance * self) {
	struct File * me = (void *)self;
	if (me->filePtr) {
		fclose(me->filePtr);
		me->filePtr = NULL;
	}
}

static void _dir_sweep(KrkInstance * self) {
	struct Directory * me = (void *)self;
	if (me->dirPtr) {
		closedir(me->dirPtr);
		me->dirPtr = NULL;
	}
}

KRK_FUNC(opendir,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,path);

	DIR * dir = opendir(path->chars);
	if (!dir) return krk_runtimeError(vm.exceptions->ioError, "opendir: %s", strerror(errno));

	struct Directory * dirObj = (void *)krk_newInstance(Directory);
	krk_push(OBJECT_VAL(dirObj));

	krk_attachNamedValue(&dirObj->inst.fields, "path", OBJECT_VAL(path));
	dirObj->dirPtr = dir;

	return krk_pop();
})

#define CURRENT_CTYPE struct Directory *

KRK_METHOD(Directory,__call__,{
	METHOD_TAKES_NONE();
	if (!self->dirPtr) return argv[0];
	struct dirent * entry = readdir(self->dirPtr);
	if (!entry) return argv[0];

	KrkValue outDict = krk_dict_of(0, NULL, 0);
	krk_push(outDict);

	krk_attachNamedValue(AS_DICT(outDict), "name", OBJECT_VAL(krk_copyString(entry->d_name,strlen(entry->d_name))));
	krk_attachNamedValue(AS_DICT(outDict), "inode", INTEGER_VAL(entry->d_ino));

	return krk_pop();
})

KRK_METHOD(Directory,__iter__,{
	METHOD_TAKES_NONE();
	return argv[0];
})

KRK_METHOD(Directory,close,{
	METHOD_TAKES_NONE();
	if (self->dirPtr) {
		closedir(self->dirPtr);
		self->dirPtr = NULL;
	}
})

KRK_METHOD(Directory,__repr__,{
	METHOD_TAKES_NONE();
	KrkValue path;
	if (!krk_tableGet(&self->inst.fields, OBJECT_VAL(S("path")), &path) || !IS_STRING(path))
		return krk_runtimeError(vm.exceptions->valueError, "corrupt Directory");

	char * tmp = malloc(AS_STRING(path)->length + 100);
	size_t len = sprintf(tmp, "<%s directory '%s' at %p>", self->dirPtr ? "open" : "closed", AS_CSTRING(path), (void*)self);
	KrkString * out = krk_copyString(tmp, len);
	free(tmp);
	return OBJECT_VAL(out);
})

KrkValue krk_module_onload_fileio(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	/* Define a class to represent files. (Should this be a helper method?) */
	krk_makeClass(module, &File, "File", vm.baseClasses->objectClass);
	File->allocSize = sizeof(struct File);
	File->_ongcsweep = _file_sweep;

	/* Add methods to it... */
	BIND_METHOD(File,read);
	BIND_METHOD(File,readline);
	BIND_METHOD(File,readlines);
	BIND_METHOD(File,write);
	BIND_METHOD(File,close);
	BIND_METHOD(File,flush);
	BIND_METHOD(File,__str__);
	BIND_METHOD(File,__init__);
	BIND_METHOD(File,__enter__);
	BIND_METHOD(File,__exit__);
	krk_defineNative(&File->methods, ".__repr__", FUNC_NAME(File,__str__));
	krk_finalizeClass(File);

	krk_makeClass(module, &BinaryFile, "BinaryFile", File);
	BIND_METHOD(BinaryFile,read);
	BIND_METHOD(BinaryFile,readline);
	BIND_METHOD(BinaryFile,readlines);
	BIND_METHOD(BinaryFile,write);
	krk_finalizeClass(BinaryFile);

	krk_makeClass(module, &Directory, "Directory", vm.baseClasses->objectClass);
	Directory->allocSize = sizeof(struct Directory);
	Directory->_ongcsweep = _dir_sweep;
	BIND_METHOD(Directory,__repr__);
	BIND_METHOD(Directory,__iter__);
	BIND_METHOD(Directory,__call__);
	BIND_METHOD(Directory,close);
	krk_finalizeClass(Directory);

	/* Make an instance for stdout, stderr, and stdin */
	makeFileInstance(module, "stdin", stdin);
	makeFileInstance(module, "stdout", stdout);
	makeFileInstance(module, "stderr", stderr);

	/* Our base will be the open method */
	BIND_FUNC(module,open);
	BIND_FUNC(module,opendir);

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}
