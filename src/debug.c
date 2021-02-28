#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "vm.h"
#include "util.h"
#include "compiler.h"

void krk_disassembleCodeObject(FILE * f, KrkFunction * func, const char * name) {
	KrkChunk * chunk = &func->chunk;
	/* Function header */
	fprintf(f, "<%s(", name);
	for (int i = 0; i < func->requiredArgs; ++i) {
		fprintf(f,"%s",AS_CSTRING(func->requiredArgNames.values[i]));
		if (i + 1 < func->requiredArgs || func->keywordArgs || func->collectsArguments || func->collectsKeywords) fprintf(f,",");
	}
	for (int i = 0; i < func->keywordArgs; ++i) {
		fprintf(f,"%s=...",AS_CSTRING(func->keywordArgNames.values[i]));
		if (i + 1 < func->keywordArgs || func->collectsArguments || func->collectsKeywords) fprintf(f,",");
	}
	if (func->collectsArguments) {
		fprintf(f,"*%s", AS_CSTRING(func->requiredArgNames.values[func->requiredArgs]));
		if (func->collectsKeywords) fprintf(f,",");
	}
	if (func->collectsKeywords) {
		fprintf(f,"**%s", AS_CSTRING(func->keywordArgNames.values[func->keywordArgs]));
	}
	fprintf(f, ") from %s>\n", chunk->filename->chars);
	for (size_t offset = 0; offset < chunk->count;) {
		offset = krk_disassembleInstruction(f, func, offset);
	}
}

static inline const char * opcodeClean(const char * opc) {
	return &opc[3];
}

static int isJumpTarget(KrkFunction * func, size_t startPoint) {
	KrkChunk * chunk = &func->chunk;
	size_t offset = 0;

#define SIMPLE(opc) case opc: size = 1; break;
#define CONSTANT(opc,more) case opc: { size_t constant __attribute__((unused)) = chunk->code[offset + 1]; size = 2; more; break; } \
	case opc ## _LONG: { size_t constant __attribute__((unused)) = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); size = 4; more; break; }
#define OPERANDB(opc,more) case opc: { size = 2; more; break; }
#define OPERAND(opc,more) OPERANDB(opc,more) \
	case opc ## _LONG: { size = 4; more; break; }
#define JUMP(opc,sign) case opc: { uint16_t jump = (chunk->code[offset + 1] << 8) | (chunk->code[offset + 2]); \
	if ((size_t)(offset + 3 sign jump) == startPoint) return 1; size = 3; break; }
#define CLOSURE_MORE size += AS_FUNCTION(chunk->constants.values[constant])->upvalueCount * 2
#define EXPAND_ARGS_MORE
#define LOCAL_MORE

	while (offset < chunk->count) {
		uint8_t opcode = chunk->code[offset];
		size_t size = 0;
		switch (opcode) {
#include "opcodes.h"
		}
		offset += size;
	}
	return 0;
#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE
}

#define SIMPLE(opc) case opc: fprintf(f, "%-16s      ", opcodeClean(#opc)); size = 1; break;
#define CONSTANT(opc,more) case opc: { size_t constant = chunk->code[offset + 1]; \
	fprintf(f, "%-16s %4d ", opcodeClean(#opc), (int)constant); \
	krk_printValueSafe(f, chunk->constants.values[constant]); \
	more; \
	size = 2; break; } \
	case opc ## _LONG: { size_t constant = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); \
	fprintf(f, "%-16s %4d ", opcodeClean(#opc "_LONG"), (int)constant); \
	krk_printValueSafe(f, chunk->constants.values[constant]); \
	more; size = 4; break; }
#define OPERANDB(opc,more) case opc: { uint32_t operand = chunk->code[offset + 1]; \
	fprintf(f, "%-16s %4d", opcodeClean(#opc), (int)operand); \
	more; size = 2; break; }
#define OPERAND(opc,more) OPERANDB(opc,more) \
	case opc ## _LONG: { uint32_t operand = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); \
	fprintf(f, "%-16s %4d", opcodeClean(#opc "_LONG"), (int)operand); \
	more; fprintf(f,"\n"); \
	size = 4; break; }
#define JUMP(opc,sign) case opc: { uint16_t jump = (chunk->code[offset + 1] << 8) | \
	(chunk->code[offset + 2]); \
	fprintf(f, "%-16s %4d (to %d)", opcodeClean(#opc), (int)jump, (int)(offset + 3 sign jump)); \
	size = 3; break; }

#define CLOSURE_MORE \
	KrkFunction * function = AS_FUNCTION(chunk->constants.values[constant]); \
	fprintf(f, " "); \
	for (size_t j = 0; j < function->upvalueCount; ++j) { \
		int isLocal = chunk->code[offset++ + 2]; \
		int index = chunk->code[offset++ + 2]; \
		if (isLocal) { \
			for (size_t i = 0; i < func->localNameCount; ++i) { \
				if (func->localNames[i].id == (size_t)index && func->localNames[i].birthday <= offset && func->localNames[i].deathday >= offset) { \
					fprintf(f, "%s", func->localNames[i].name->chars); \
					break; \
				} \
			} \
		} else { fprintf(f, "upvalue<%d>", index); } \
		if (j + 1 != function->upvalueCount) fprintf(f, ", "); \
	}

#define EXPAND_ARGS_MORE \
	fprintf(f, " (%s)", operand == 0 ? "singleton" : (operand == 1 ? "list" : "dict"));

#define LOCAL_MORE \
	if ((short int)operand < (func->requiredArgs)) { \
		fprintf(f, " (%s, arg)", AS_CSTRING(func->requiredArgNames.values[operand])); \
	} else if ((short int)operand < (func->requiredArgs + func->keywordArgs)) { \
		fprintf(f, " (%s, kwarg))", AS_CSTRING(func->keywordArgNames.values[operand-func->requiredArgs])); \
	} else { \
		for (size_t i = 0; i < func->localNameCount; ++i) { \
			if (func->localNames[i].id == operand && func->localNames[i].birthday <= offset && func->localNames[i].deathday >= offset) { \
				fprintf(f, " (%s)", func->localNames[i].name->chars); \
				break; \
			} \
		} \
	}

size_t krk_lineNumber(KrkChunk * chunk, size_t offset) {
	size_t line = 0;
	for (size_t i = 0; i < chunk->linesCount; ++i) {
		if (chunk->lines[i].startOffset > offset) break;
		line = chunk->lines[i].line;
	}
	return line;
}

size_t krk_disassembleInstruction(FILE * f, KrkFunction * func, size_t offset) {
	KrkChunk * chunk = &func->chunk;
	if (offset > 0 && krk_lineNumber(chunk, offset) == krk_lineNumber(chunk, offset - 1)) {
		fprintf(f, "     ");
	} else {
		if (offset > 0) fprintf(f,"\n");
		fprintf(f, "%4d ", (int)krk_lineNumber(chunk, offset));
	}
	if (isJumpTarget(func,offset)) {
		fprintf(f, " >> ");
	} else {
		fprintf(f, "    ");
	}
	fprintf(f, "%4u ", (unsigned int)offset);
	uint8_t opcode = chunk->code[offset];
	size_t size = 1;

	switch (opcode) {
#include "opcodes.h"
		default:
			fprintf(f, "Unknown opcode: %02x", opcode);
	}

	/* Birthdays - Local names that have become valid from this instruction */
	for (size_t i = 0; i < func->localNameCount; ++i) {
		if (func->localNames[i].birthday >= offset && func->localNames[i].birthday < offset + size) {
			fprintf(f, " +%s", func->localNames[i].name->chars);
		}
	}

	/* Deathdays - Local names that are no longer valid as of this instruction */
	for (size_t i = 0; i < func->localNameCount; ++i) {
		if (func->localNames[i].deathday >= offset && func->localNames[i].deathday < offset + size) {
			fprintf(f, " -%s", func->localNames[i].name->chars);
		}
	}

	fprintf(f,"\n");

	return offset + size;
}
#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE

/**
 * dis.dis(object)
 */
KRK_FUNC(dis,{
	FUNCTION_TAKES_EXACTLY(1);

	if (IS_CLOSURE(argv[0])) {
		KrkFunction * func = AS_CLOSURE(argv[0])->function;
		krk_disassembleCodeObject(stdout, func, func->name ? func->name->chars : "(unnamed)");
	} else if (IS_BOUND_METHOD(argv[0])) {
		if (AS_BOUND_METHOD(argv[0])->method->type == OBJ_CLOSURE) {
			KrkFunction * func = ((KrkClosure*)AS_BOUND_METHOD(argv[0])->method)->function;
			const char * methodName = func->name ? func->name->chars : "(unnamed)";
			const char * typeName = IS_CLASS(AS_BOUND_METHOD(argv[0])->receiver) ? AS_CLASS(AS_BOUND_METHOD(argv[0])->receiver)->name->chars : krk_typeName(AS_BOUND_METHOD(argv[0])->receiver);
			size_t allocSize = strlen(methodName) + strlen(typeName) + 2;
			char * tmp = malloc(allocSize);
			snprintf(tmp, allocSize, "%s.%s", typeName, methodName);
			krk_disassembleCodeObject(stdout, func, tmp);
			free(tmp);
		} else {
			krk_runtimeError(vm.exceptions->typeError, "Can not disassemble built-in method of '%s'", krk_typeName(AS_BOUND_METHOD(argv[0])->receiver));
		}
	} else if (IS_CLASS(argv[0])) {
		KrkValue code;
		if (krk_tableGet(&AS_CLASS(argv[0])->fields, OBJECT_VAL(S("__func__")), &code) && IS_CLOSURE(code)) {
			KrkFunction * func = AS_CLOSURE(code)->function;
			krk_disassembleCodeObject(stdout, func, AS_CLASS(argv[0])->name->chars);
		}
		/* TODO Methods! */
	} else {
		krk_runtimeError(vm.exceptions->typeError, "Don't know how to disassemble '%s'", krk_typeName(argv[0]));
	}

	return NONE_VAL();
})

KRK_FUNC(build,{
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,str,KrkString*,code);

	/* Unset module */
	krk_push(OBJECT_VAL(krk_currentThread.module));
	KrkInstance * module = krk_currentThread.module;
	krk_currentThread.module = NULL;
	KrkFunction * c = krk_compile(code->chars,"<source>");
	krk_currentThread.module = module;
	krk_pop();
	if (c) return OBJECT_VAL(c);
	else return NONE_VAL();
})

#define SIMPLE(opc) case opc: size = 1; break;
#define CONSTANT(opc,more) case opc: { constant = chunk->code[offset + 1]; size = 2; more; break; } \
	case opc ## _LONG: { constant = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); size = 4; more; break; }
#define OPERANDB(opc,more) case opc: { size = 2; more; break; }
#define OPERAND(opc,more) OPERANDB(opc,more) \
	case opc ## _LONG: { size = 4; more; break; }
#define JUMP(opc,sign) case opc: { jump = 0 sign ((chunk->code[offset + 1] << 8) | (chunk->code[offset + 2])); \
	size = 3; break; }
#define CLOSURE_MORE size += AS_FUNCTION(chunk->constants.values[constant])->upvalueCount * 2
#define EXPAND_ARGS_MORE
#define LOCAL_MORE
FUNC_SIG(krk,examine) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,FUNCTION,KrkFunction*,func);

	KrkValue output = krk_list_of(0,NULL,0);
	krk_push(output);

	KrkChunk * chunk = &func->chunk;
	size_t offset = 0;
	while (offset < chunk->count) {
		uint8_t opcode = chunk->code[offset];
		size_t size = 0;
		ssize_t constant = -1;
		ssize_t jump = 0;
		switch (opcode) {
#include "opcodes.h"
		}

		KrkTuple * newTuple = krk_newTuple(3);
		krk_push(OBJECT_VAL(newTuple));
		newTuple->values.values[newTuple->values.count++] = INTEGER_VAL(opcode);
		newTuple->values.values[newTuple->values.count++] = INTEGER_VAL(size);
		if (constant != -1) {
			newTuple->values.values[newTuple->values.count++] = chunk->constants.values[constant];
		} else if (jump != 0) {
			newTuple->values.values[newTuple->values.count++] = INTEGER_VAL(jump);
		} else {
			newTuple->values.values[newTuple->values.count++] = NONE_VAL();
		}
		krk_writeValueArray(AS_LIST(output), krk_peek(0));
		krk_pop();

		if (size == 0) {
			fprintf(stderr, "offset = %ld, chunk->count = %ld, found size = 0?\n", offset, chunk->count);
			abort();
		}

		offset += size;
	}

	return krk_pop();
}
#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE

_noexport
void _createAndBind_disMod(void) {
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_attachNamedObject(&vm.modules, "dis", (KrkObj*)module);
	krk_attachNamedObject(&module->fields, "__name__", (KrkObj*)S("dis"));
	krk_attachNamedValue(&module->fields, "__file__", NONE_VAL());
	krk_attachNamedObject(&module->fields, "__doc__",
		(KrkObj*)S("@brief Provides tools for disassembling bytecode."));
	BIND_FUNC(module, dis)->doc = "@brief Disassemble an object.\n"
		"@arguments obj\n\n"
		"Dumps a disassembly of the bytecode in the code object associated with @p obj. "
		"If @p obj can not be disassembled, a @ref TypeError is raised.";

	BIND_FUNC(module, build);
	BIND_FUNC(module, examine);

#define OPCODE(opc) krk_attachNamedValue(&module->fields, #opc, INTEGER_VAL(opc));
#define SIMPLE(opc) OPCODE(opc)
#define CONSTANT(opc,more) OPCODE(opc) OPCODE(opc ## _LONG)
#define OPERAND(opc,more) OPCODE(opc) OPCODE(opc ## _LONG)
#define JUMP(opc,sign) OPCODE(opc)
#define CLOSURE_MORE
#define EXPAND_ARGS_MORE
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
}
