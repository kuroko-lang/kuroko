#include <stdio.h>

#include "debug.h"
#include "vm.h"

void krk_disassembleChunk(KrkChunk * chunk, const char * name) {
	fprintf(stderr, "[%s]\n", name);
	for (size_t offset = 0; offset < chunk->count;) {
		offset = krk_disassembleInstruction(chunk, offset);
	}
}

#define SIMPLE(opc) case opc: fprintf(stderr, "%s\n", #opc); return offset + 1;

size_t krk_disassembleInstruction(KrkChunk * chunk, size_t offset) {
	fprintf(stderr, "%04u ", (unsigned int)offset);
	if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
		fprintf(stderr, "   | ");
	} else {
		fprintf(stderr, "%4d ", (int)chunk->lines[offset]);
	}
	uint8_t opcode = chunk->code[offset];

	switch (opcode) {
		SIMPLE(OP_RETURN)
		SIMPLE(OP_ADD)
		SIMPLE(OP_SUBTRACT)
		SIMPLE(OP_MULTIPLY)
		SIMPLE(OP_DIVIDE)
		SIMPLE(OP_NEGATE)
		SIMPLE(OP_NONE)
		SIMPLE(OP_TRUE)
		SIMPLE(OP_FALSE)
		SIMPLE(OP_NOT)
		SIMPLE(OP_EQUAL)
		SIMPLE(OP_GREATER)
		SIMPLE(OP_LESS)
		case OP_CONSTANT:
			{
				uint8_t constant = chunk->code[offset + 1];
				fprintf(stderr, "%-16s %4d '", "OP_CONSTANT", constant);
				krk_printValue(stderr, chunk->constants.values[constant]);
				fprintf(stderr, "' (type=%s)\n", typeName(chunk->constants.values[constant]));
			}
			return offset + 2;
		case OP_CONSTANT_LONG:
			{
				size_t constant = (chunk->code[offset + 1] << 16) |
						(chunk->code[offset + 2] << 8) |
						(chunk->code[offset + 3] << 0);
				fprintf(stderr, "%-16s %10d '", "OP_CONSTANT_LONG", (int)constant);
				krk_printValue(stderr, chunk->constants.values[constant]);
				fprintf(stderr, "' (type=%s)\n", typeName(chunk->constants.values[constant]));
			}
			return offset + 4;
		default:
			fprintf(stderr, "Unknown opcode: %02x\n", opcode);
			return offset + 1;
	}
}

