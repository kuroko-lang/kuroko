#include <stdio.h>

#include "kuroko.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, char * argv[]) {
	krk_initVM();

	krk_interpret("\"hello\" + \"hellf\" + 1.4");

#if 0
	KrkChunk chunk;
	krk_initChunk(&chunk);

	size_t constant = krk_addConstant(&chunk, 42);
	krk_writeConstant(&chunk, 42, 1);
	krk_writeConstant(&chunk, 86, 1);
	krk_writeChunk(&chunk, OP_ADD, 1);
	krk_writeConstant(&chunk, 3, 1);
	krk_writeChunk(&chunk, OP_DIVIDE, 1);
	krk_writeChunk(&chunk, OP_NEGATE, 1);
	krk_writeChunk(&chunk, OP_RETURN, 1);

	krk_interpret(&chunk);

	krk_freeChunk(&chunk);
#endif

	krk_freeVM();
	return 0;
}
