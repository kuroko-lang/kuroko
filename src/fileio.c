/**
 * @file fileio.c
 * @brief Provides an interface to C FILE* streams.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include <kuroko/vm.h>
#include <kuroko/value.h>
#include <kuroko/object.h>
#include <kuroko/memory.h>
#include <kuroko/util.h>

/**
 * @brief Object for a C `FILE*` stream.
 * @extends KrkInstance
 */
struct File {
	KrkInstance inst;
	FILE * filePtr;
	int unowned;
};

#define IS_File(o) (krk_isInstanceOf(o, KRK_BASE_CLASS(File)))
#define AS_File(o) ((struct File*)AS_OBJECT(o))

#define IS_BinaryFile(o) (krk_isInstanceOf(o, KRK_BASE_CLASS(BinaryFile)))
#define AS_BinaryFile(o) ((struct File*)AS_OBJECT(o))

/**
 * @brief OBject for a C `DIR*` stream.
 * @extends KrkInstance
 */
struct Directory {
	KrkInstance inst;
	DIR * dirPtr;
};

#define IS_Directory(o) (krk_isInstanceOf(o, KRK_BASE_CLASS(Directory)))
#define AS_Directory(o) ((struct Directory*)AS_OBJECT(o))

#define CURRENT_CTYPE struct File *
#define CURRENT_NAME  self

KRK_Function(open) {
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
	KrkInstance * fileObject = krk_newInstance(isBinary ? KRK_BASE_CLASS(BinaryFile) : KRK_BASE_CLASS(File));
	krk_push(OBJECT_VAL(fileObject));

	/* Let's put the filename in there somewhere... */
	krk_attachNamedValue(&fileObject->fields, "filename", OBJECT_VAL(filename));
	krk_attachNamedValue(&fileObject->fields, "modestr", arg);

	((struct File*)fileObject)->filePtr = file;

	krk_pop();
	krk_pop();
	return OBJECT_VAL(fileObject);
}

#define BLOCK_SIZE 1024

KRK_Method(File,__str__) {
	METHOD_TAKES_NONE();
	KrkValue filename;
	KrkValue modestr;
	if (!krk_tableGet(&self->inst.fields, OBJECT_VAL(S("filename")), &filename) || !IS_STRING(filename)) return krk_runtimeError(vm.exceptions->baseException, "Corrupt File");
	if (!krk_tableGet(&self->inst.fields, OBJECT_VAL(S("modestr")), &modestr) || !IS_STRING(modestr)) return krk_runtimeError(vm.exceptions->baseException, "Corrupt File");
	size_t allocSize = AS_STRING(filename)->length + AS_STRING(modestr)->length + 100;
	char * tmp = malloc(allocSize);
	size_t len = snprintf(tmp, allocSize, "<%s file '%s', mode '%s' at %p>", self->filePtr ? "open" : "closed", AS_CSTRING(filename), AS_CSTRING(modestr), (void*)self);
	KrkString * out = krk_copyString(tmp, len);
	free(tmp);
	return OBJECT_VAL(out);
}

KRK_Method(File,readline) {
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
			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;
			if (c < 0) break;
			sizeRead++;
			*target++ = c;
			if (c == '\n') goto _finish_line;
		}

		if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;
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

KRK_Method(File,readlines) {
	METHOD_TAKES_NONE();
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	for (;;) {
		KrkValue line = FUNC_NAME(File,readline)(_thread, 1, argv, 0);
		if (IS_NONE(line)) break;
		if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;

		krk_push(line);
		krk_writeValueArray(AS_LIST(myList), line);
		krk_pop(); /* line */
	}

	krk_pop(); /* myList */
	return myList;
}

KRK_Method(File,read) {
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
			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;

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
}

KRK_Method(File,write) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_STRING(argv[1])) return TYPE_ERROR(str,argv[1]);
	/* Find the file ptr reference */
	FILE * file = self->filePtr;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_CSTRING(argv[1]), 1, AS_STRING(argv[1])->length, file));
}

KRK_Method(File,close) {
	METHOD_TAKES_NONE();
	FILE * file = self->filePtr;
	if (file) fclose(file);
	self->filePtr = NULL;
	return NONE_VAL();
}

KRK_Method(File,flush) {
	METHOD_TAKES_NONE();
	FILE * file = self->filePtr;
	if (file) fflush(file);
	return NONE_VAL();
}

KRK_Method(File,__init__) {
	return krk_runtimeError(vm.exceptions->typeError, "File objects can not be instantiated; use fileio.open() to obtain File objects.");
}

KRK_Method(File,__enter__) {
	return NONE_VAL();
}
KRK_Method(File,__exit__) {
	return FUNC_NAME(File,close)(_thread, 1,argv,0);
}

static void makeFileInstance(KrkThreadState * _thread, KrkInstance * module, const char name[], FILE * file) {
	KrkInstance * fileObject = krk_newInstance(KRK_BASE_CLASS(File));
	krk_push(OBJECT_VAL(fileObject));
	KrkValue filename = OBJECT_VAL(krk_copyString(name,strlen(name)));
	krk_push(filename);

	krk_attachNamedValue(&fileObject->fields, "filename", filename);
	((struct File*)fileObject)->filePtr = file;
	((struct File*)fileObject)->unowned = 1;

	krk_attachNamedObject(&module->fields, name, (KrkObj*)fileObject);

	krk_pop(); /* filename */
	krk_pop(); /* fileObject */
}

KRK_Method(BinaryFile,readline) {
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
			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;
			if (c < 0) break;
			sizeRead++;
			*target++ = c;
			if (c == '\n') goto _finish_line;
		}

		if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;
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

KRK_Method(BinaryFile,readlines) {
	METHOD_TAKES_NONE();
	KrkValue myList = krk_list_of(0,NULL,0);
	krk_push(myList);

	for (;;) {
		KrkValue line = FUNC_NAME(BinaryFile,readline)(_thread, 1, argv, 0);
		if (IS_NONE(line)) break;
		if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;

		krk_push(line);
		krk_writeValueArray(AS_LIST(myList), line);
		krk_pop(); /* line */
	}

	krk_pop(); /* myList */
	return myList;
}

KRK_Method(BinaryFile,read) {
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

			if (krk_currentThread.flags & KRK_THREAD_SIGNALLED) break;

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
}

KRK_Method(BinaryFile,write) {
	METHOD_TAKES_EXACTLY(1);
	if (!IS_BYTES(argv[1])) return TYPE_ERROR(bytes,argv[1]);
	/* Find the file ptr reference */
	FILE * file = self->filePtr;

	if (!file || feof(file)) {
		return NONE_VAL();
	}

	return INTEGER_VAL(fwrite(AS_BYTES(argv[1])->bytes, 1, AS_BYTES(argv[1])->length, file));
}

#undef CURRENT_CTYPE

static void _file_sweep(KrkThreadState * _thread, KrkInstance * self) {
	struct File * me = (void *)self;
	if (me->filePtr && !me->unowned) {
		fclose(me->filePtr);
		me->filePtr = NULL;
	}
}

static void _dir_sweep(KrkThreadState * _thread, KrkInstance * self) {
	struct Directory * me = (void *)self;
	if (me->dirPtr) {
		closedir(me->dirPtr);
		me->dirPtr = NULL;
	}
}

KRK_Function(opendir) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,path);

	DIR * dir = opendir(path->chars);
	if (!dir) return krk_runtimeError(vm.exceptions->ioError, "opendir: %s", strerror(errno));

	struct Directory * dirObj = (void *)krk_newInstance(KRK_BASE_CLASS(Directory));
	krk_push(OBJECT_VAL(dirObj));

	krk_attachNamedValue(&dirObj->inst.fields, "path", OBJECT_VAL(path));
	dirObj->dirPtr = dir;

	return krk_pop();
}

#define CURRENT_CTYPE struct Directory *

KRK_Method(Directory,__call__) {
	METHOD_TAKES_NONE();
	if (!self->dirPtr) return argv[0];
	struct dirent * entry = readdir(self->dirPtr);
	if (!entry) return argv[0];

	KrkValue outDict = krk_dict_of(0, NULL, 0);
	krk_push(outDict);

	krk_attachNamedValue(AS_DICT(outDict), "name", OBJECT_VAL(krk_copyString(entry->d_name,strlen(entry->d_name))));
	krk_attachNamedValue(AS_DICT(outDict), "inode", INTEGER_VAL(entry->d_ino));

	return krk_pop();
}

KRK_Method(Directory,__iter__) {
	METHOD_TAKES_NONE();
	return argv[0];
}

KRK_Method(Directory,close) {
	METHOD_TAKES_NONE();
	if (self->dirPtr) {
		closedir(self->dirPtr);
		self->dirPtr = NULL;
	}
	return NONE_VAL();
}

KRK_Method(Directory,__repr__) {
	METHOD_TAKES_NONE();
	KrkValue path;
	if (!krk_tableGet(&self->inst.fields, OBJECT_VAL(S("path")), &path) || !IS_STRING(path))
		return krk_runtimeError(vm.exceptions->valueError, "corrupt Directory");

	size_t allocSize = AS_STRING(path)->length + 100;
	char * tmp = malloc(allocSize);
	size_t len = snprintf(tmp, allocSize, "<%s directory '%s' at %p>", self->dirPtr ? "open" : "closed", AS_CSTRING(path), (void*)self);
	KrkString * out = krk_copyString(tmp, len);
	free(tmp);
	return OBJECT_VAL(out);
}

KRK_Method(Directory,__enter__) {
	return NONE_VAL();
}
KRK_Method(Directory,__exit__) {
	return FUNC_NAME(Directory,close)(_thread, 1,argv,0);
}

void krk_module_init_fileio(KrkThreadState * _thread) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "fileio", (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)S("fileio"));
	krk_attachNamedValue(&module->fields, "__file__", NONE_VAL());
	KRK_DOC(module,
		"@brief Provides access to C <stdio> buffered file I/O functions.\n\n"
		"The @c fileio module provides classes and functions for reading "
		"and writing files using the system's buffer I/O interfaces, as "
		"well as classes for listing the contents of directories."
	);

	/* Define a class to represent files. (Should this be a helper method?) */
	KrkClass * File = ADD_BASE_CLASS(KRK_BASE_CLASS(File), "File", KRK_BASE_CLASS(object));
	KRK_DOC(File,"Interface to a buffered file stream.");
	File->allocSize = sizeof(struct File);
	File->_ongcsweep = _file_sweep;

	/* Add methods to it... */
	KRK_DOC(BIND_METHOD(File,read), "@brief Read from the stream.\n"
		"@arguments bytes=-1\n\n"
		"Reads up to @p bytes bytes from the stream. If @p bytes is @c -1 then reading "
		"will continue until the system returns _end of file_.");
	KRK_DOC(BIND_METHOD(File,readline), "@brief Read one line from the stream.");
	KRK_DOC(BIND_METHOD(File,readlines), "@brief Read the entire stream and return a list of lines.");
	KRK_DOC(BIND_METHOD(File,write), "@brief Write to the stream.\n"
		"@arguments data\n\n"
		"Writes the contents of @p data to the stream.");
	KRK_DOC(BIND_METHOD(File,close), "@brief Close the stream and flush any remaining buffered writes.");
	KRK_DOC(BIND_METHOD(File,flush), "@brief Flush unbuffered writes to the stream.");
	BIND_METHOD(File,__str__);
	KRK_DOC(BIND_METHOD(File,__init__), "@bsnote{%File objects can not be initialized using this constructor. "
		"Use the <a class=\"el\" href=\"#open\">open()</a> function instead.}");
	BIND_METHOD(File,__enter__);
	BIND_METHOD(File,__exit__);
	krk_defineNative(&File->methods, "__repr__", FUNC_NAME(File,__str__));
	krk_finalizeClass(File);

	KrkClass * BinaryFile = ADD_BASE_CLASS(KRK_BASE_CLASS(BinaryFile), "BinaryFile", File);
	KRK_DOC(BinaryFile,
		"Equivalent to @ref File but using @ref bytes instead of string @ref str."
	);
	BIND_METHOD(BinaryFile,read);
	BIND_METHOD(BinaryFile,readline);
	BIND_METHOD(BinaryFile,readlines);
	BIND_METHOD(BinaryFile,write);
	krk_finalizeClass(BinaryFile);

	KrkClass * Directory = ADD_BASE_CLASS(KRK_BASE_CLASS(Directory), "Directory", KRK_BASE_CLASS(object));
	KRK_DOC(Directory,
		"Represents an opened file system directory."
	);
	Directory->allocSize = sizeof(struct Directory);
	Directory->_ongcsweep = _dir_sweep;
	BIND_METHOD(Directory,__repr__);
	KRK_DOC(BIND_METHOD(Directory,__iter__), "@brief Iterates over the contents of the directory.\n\n"
		"Each iteration returns @ref dict with two entries: <i>\"name\"</i> and <i>\"inode\"</i>.");
	KRK_DOC(BIND_METHOD(Directory,__call__), "@brief Yields one iteration through the directory.");
	BIND_METHOD(Directory,__enter__);
	KRK_DOC(BIND_METHOD(Directory,__exit__), "@brief Closes the directory upon exit from a @c with block.");
	KRK_DOC(BIND_METHOD(Directory,close), "@brief Close the directory.\n\nFurther reads can not be made after the directory has been closed.");
	krk_finalizeClass(Directory);

	/* Make an instance for stdout, stderr, and stdin */
	makeFileInstance(_thread, module, "stdin", stdin);
	makeFileInstance(_thread, module, "stdout", stdout);
	makeFileInstance(_thread, module, "stderr", stderr);

	/* Our base will be the open method */
	KRK_DOC(BIND_FUNC(module,open), "@brief Open a file.\n"
		"@arguments path,mode=\"r\"\n\n"
		"Opens @p path using the modestring @p mode. Supported modestring characters depend on the system implementation. "
		"If the last character of @p mode is @c 'b' a @ref BinaryFile will be returned. If the file could not be opened, "
		"an @ref IOError will be raised.");
	KRK_DOC(BIND_FUNC(module,opendir), "@brief Open a directory for scanning.\n"
		"@arguments path\n\n"
		"Opens the directory at @p path and returns a @ref Directory object. If @p path could not be opened or is not "
		"a directory, @ref IOError will be raised.");
}
