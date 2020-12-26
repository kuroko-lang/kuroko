#pragma once

#include "chunk.h"

extern void krk_disassembleChunk(KrkChunk * chunk, const char * name);
extern size_t krk_disassembleInstruction(KrkChunk * chunk, size_t offset);
