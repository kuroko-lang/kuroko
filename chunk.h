#pragma once

#include "kuroko.h"
#include "value.h"

/**
 * Opcodes
 *
 * These are pretty much entirely based on the clox opcodes from the book.
 * There's not really much else to add here, since the VM is sufficient for
 * our needs. Most of the interesting changes happen in the compiler.
 */
typedef enum {
	OP_CONSTANT = 1,
	OP_NEGATE,
	OP_RETURN,
	OP_ADD,
	OP_SUBTRACT,
	OP_MULTIPLY,
	OP_DIVIDE,
	OP_MODULO,
	OP_NONE,
	OP_TRUE,
	OP_FALSE,
	OP_NOT,
	OP_POP,
	OP_EQUAL,
	OP_GREATER,
	OP_LESS,
	OP_DEFINE_GLOBAL,
	OP_GET_GLOBAL,
	OP_SET_GLOBAL,
	OP_SET_LOCAL,
	OP_GET_LOCAL,
	OP_JUMP_IF_FALSE,
	OP_JUMP_IF_TRUE,
	OP_JUMP,
	OP_LOOP,
	OP_CALL,
	OP_CLOSURE,
	OP_GET_UPVALUE,
	OP_SET_UPVALUE,
	OP_CLOSE_UPVALUE,
	OP_CLASS,
	OP_SET_PROPERTY,
	OP_GET_PROPERTY,
	OP_METHOD,
	OP_IMPORT,
	OP_INHERIT,
	OP_GET_SUPER,
	OP_PUSH_TRY,
	OP_RAISE,
	OP_DOCSTRING,
	OP_CALL_STACK,
	OP_INC,
	OP_DUP,
	OP_SWAP,
	OP_KWARGS,

	OP_BITOR,
	OP_BITXOR,
	OP_BITAND,
	OP_SHIFTLEFT,
	OP_SHIFTRIGHT,
	OP_BITNEGATE,

	OP_INVOKE_GETTER,
	OP_INVOKE_SETTER,
	OP_INVOKE_GETSLICE,

	OP_EXPAND_ARGS,
	OP_FINALIZE,
	OP_TUPLE,
	OP_UNPACK_TUPLE,
	OP_PUSH_WITH,
	OP_CLEANUP_WITH,

	OP_IS,

	OP_CONSTANT_LONG = 128,
	OP_DEFINE_GLOBAL_LONG,
	OP_GET_GLOBAL_LONG,
	OP_SET_GLOBAL_LONG,
	OP_SET_LOCAL_LONG,
	OP_GET_LOCAL_LONG,
	OP_CALL_LONG,
	OP_CLOSURE_LONG,
	OP_GET_UPVALUE_LONG,
	OP_SET_UPVALUE_LONG,
	OP_CLASS_LONG,
	OP_SET_PROPERTY_LONG,
	OP_GET_PROPERTY_LONG,
	OP_METHOD_LONG,
	OP_IMPORT_LONG,
	OP_GET_SUPER_LONG,
	OP_INC_LONG,
	OP_KWARGS_LONG,
	OP_TUPLE_LONG,
	OP_UNPACK_TUPLE_LONG,
} KrkOpCode;

typedef struct {
	size_t startOffset;
	size_t line;
} KrkLineMap;

/**
 * Bytecode chunks
 */
typedef struct {
	size_t  count;
	size_t  capacity;
	uint8_t * code;

	size_t linesCount;
	size_t linesCapacity;
	KrkLineMap * lines;

	KrkString * filename;
	KrkValueArray constants;
} KrkChunk;

extern void krk_initChunk(KrkChunk * chunk);
extern void krk_writeChunk(KrkChunk * chunk, uint8_t byte, size_t line);
extern void krk_freeChunk(KrkChunk * chunk);
extern size_t krk_addConstant(KrkChunk * chunk, KrkValue value);
extern void krk_emitConstant(KrkChunk * chunk, size_t ind, size_t line);
extern size_t krk_writeConstant(KrkChunk * chunk, KrkValue value, size_t line);
