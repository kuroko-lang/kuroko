#pragma once

#include <stdio.h>
#include "chunk.h"
#include "object.h"

extern void krk_disassembleChunk(FILE * f, KrkFunction * func, const char * name);
extern size_t krk_disassembleInstruction(FILE * f, KrkFunction * func, size_t offset);
extern size_t krk_lineNumber(KrkChunk * chunk, size_t offset);
