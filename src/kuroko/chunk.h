#pragma once
/**
 * @file chunk.h
 * @brief Structures and enums for bytecode chunks.
 */
#include "kuroko.h"
#include "value.h"

/**
 * @brief Map entry of instruction offsets to line numbers.
 *
 * Each code object contains an array of line mappings, indicating
 * the start offset of each line. Since a line typically maps to
 * multiple opcodes, and spans of many lines may map to no opcodes
 * in the case of blank lines or docstrings, this array is stored
 * as a sequence of <starOffset, line> pairs rather than a simple
 * array of one or the other.
 */
typedef struct {
	size_t startOffset;
	size_t line;
} KrkLineMap;

/**
 * @brief Opcode chunk of a code object.
 *
 * Opcode chunks are internal to code objects and I'm not really
 * sure why we're still separating them from the KrkCodeObjects.
 *
 * Stores four flexible arrays using three different formats:
 * - Code, representing opcodes and operands.
 * - Lines, representing offset-to-line mappings.
 * - Filename, the string name of the source file.
 * - Constants, an array of values referenced by the code object.
 */
typedef struct {
	size_t  count;
	size_t  capacity;
	uint8_t * code;

	size_t linesCount;
	size_t linesCapacity;
	KrkLineMap * lines;

	struct KrkString * filename;
	KrkValueArray constants;
} KrkChunk;

/**
 * @brief Initialize an opcode chunk.
 * @memberof KrkChunk
 */
extern void krk_initChunk(KrkChunk * chunk);

/**
 * @memberof KrkChunk
 * @brief Append a byte to an opcode chunk.
 */
extern void krk_writeChunk(KrkChunk * chunk, uint8_t byte, size_t line);

/**
 * @brief Release the resources allocated to an opcode chunk.
 * @memberof KrkChunk
 */
extern void krk_freeChunk(KrkChunk * chunk);

/**
 * @brief Add a new constant value to an opcode chunk.
 * @memberof KrkChunk
 */
extern size_t krk_addConstant(KrkChunk * chunk, KrkValue value);

/**
 * @brief Write an OP_CONSTANT(_LONG) instruction.
 * @memberof KrkChunk
 */
extern void krk_emitConstant(KrkChunk * chunk, size_t ind, size_t line);

/**
 * @brief Add a new constant and write an instruction for it.
 * @memberof KrkChunk
 */
extern size_t krk_writeConstant(KrkChunk * chunk, KrkValue value, size_t line);

/**
 * @brief Obtain the line number for a byte offset into a bytecode chunk.
 * @memberof KrkChunk
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
