/**
 * @brief Instruction opcode values
 *
 * @warning The opcode table is not stable.
 *
 * There is no special meaning to the ordering of opcodes, and they can change
 * even within a given patch number. Opcode values are not exposed in public
 * C headers and should not be relied upon.
 *
 * Opcode numbers *are* exposed through the 'dis' module, but are only valid
 * within the same build.
 *
 * 0-operand opcodes are "simple" instructions that generally only deal with stack
 * values and require no additional arguments.
 *
 * 1- and 3- operand opcodes are paired as 'short' and 'long'. While the VM does not
 * currently depend on these instructions having the same values in the lower 6 bits,
 * it is recommended that this property remain true.
 *
 * 2-operand opcodes are generally jump instructions.
 */
typedef enum {
#define OPCODE(opc)         opc,
#define SIMPLE(opc)         OPCODE(opc)
#define CONSTANT(opc,more)  OPCODE(opc) OPCODE(opc ## _LONG)
#define OPERAND(opc,more)   OPCODE(opc) OPCODE(opc ## _LONG)
#define JUMP(opc,sign)      OPCODE(opc)
#define COMPLICATED(opc,more) OPCODE(opc)
#define CLOSURE_MORE
#define EXPAND_ARGS_MORE
#define FORMAT_VALUE_MORE
#define LOCAL_MORE
#define OVERLONG_JUMP_MORE
#include "opcodes.h"
#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef COMPLICATED
#undef OVERLONG_JUMP_MORE
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE
#undef FORMAT_VALUE_MORE
#undef OPCODE
} KrkOpCode;
