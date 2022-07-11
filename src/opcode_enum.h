/**
 * @brief Instruction opcode values
 *
 * The instruction opcode table is divided in four parts. The high two bits of each
 * opcode encodes the number of operands to pull from the codeobject and thus the
 * size (generally) of the instruction (note that OP_CLOSURE(_LONG) has additional
 * arguments depending on the function it points to).
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
#define CLOSURE_MORE
#define EXPAND_ARGS_MORE
#define FORMAT_VALUE_MORE
#define LOCAL_MORE
#include "opcodes.h"
#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE
#undef FORMAT_VALUE_MORE
#undef OPCODE
} KrkOpCode;
