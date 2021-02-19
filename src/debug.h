#pragma once
/**
 * @file debug.h
 * @brief Functions for disassembling bytecode.
 */
#include <stdio.h>
#include "chunk.h"
#include "object.h"

/**
 * @brief Print a disassembly of 'func' to the stream 'f'.
 *
 * Generates and prints a bytecode disassembly of the code object 'func',
 * writing it to the requested stream.
 *
 * @param f     Stream to write to.
 * @param func  Code object to disassemble.
 * @param name  Function name to display in disassembly output.
 */
extern void krk_disassembleChunk(FILE * f, KrkFunction * func, const char * name);

/**
 * @brief Print a disassembly of a single opcode instruction.
 *
 * Generates and prints a bytecode disassembly for one instruction from
 * the code object 'func' at byte offset 'offset', printing the result to
 * the requested stream and returning the size of the instruction.
 *
 * @param f      Stream to write to.
 * @param func   Code object to disassemble.
 * @param offset Byte offset of the instruction to disassemble.
 * @return The size of the instruction in bytes.
 */
extern size_t krk_disassembleInstruction(FILE * f, KrkFunction * func, size_t offset);

/**
 * @brief Obtain the line number for a byte offset into a bytecode chunk.
 *
 * Scans the line mapping table for the given chunk to find the
 * correct line number from the original source file for the instruction
 * at byte index 'offset'.
 *
 * @param chunk  Bytecode chunk containing the instruction.
 * @param offset Byte offset of the instruction to locate.
 * @return Line number, 1-indexed.
 */
extern size_t krk_lineNumber(KrkChunk * chunk, size_t offset);

/* Internal stuff */
extern void _createAndBind_disMod(void);
