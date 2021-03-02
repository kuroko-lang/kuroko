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

struct BreakPointTable {
	KrkFunction * inFunction;
	size_t offset;
	uint8_t originalOpcode;
};

#define MAX_BREAKPOINTS 16
static struct BreakPointTable breakpoints[MAX_BREAKPOINTS] = {0};
static int breakpointsCount = 0;

static char * lastDebuggerCommand = NULL;
static int debuggerShowHello = 1; /* Always make sure we show the message on the first call */

static void debug_enableSingleStep(void) {
	krk_currentThread.flags |= KRK_THREAD_ENABLE_TRACING;
	krk_currentThread.flags |= KRK_THREAD_SINGLE_STEP;
}

static void debug_disableSingleStep(void) {
	krk_currentThread.flags &= ~(KRK_THREAD_ENABLE_TRACING);
	krk_currentThread.flags &= ~(KRK_THREAD_SINGLE_STEP);
}

KRK_FUNC(enablebreakpoint,{
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (breakIndex < 0 || breakIndex >= breakpointsCount || breakpoints[breakIndex].inFunction == NULL) {
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
	}
	breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] = OP_BREAKPOINT;
})

KRK_FUNC(disablebreakpoint,{
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (breakIndex < 0 || breakIndex >= breakpointsCount || breakpoints[breakIndex].inFunction == NULL) {
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
	}
	breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] =
		breakpoints[breakIndex].originalOpcode;
})

KRK_FUNC(delbreakpoint,{
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (breakIndex < 0 || breakIndex >= breakpointsCount || breakpoints[breakIndex].inFunction == NULL) {
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
	}
	breakpoints[breakIndex].inFunction = NULL;
	while (breakpointsCount && breakpoints[breakpointsCount-1].inFunction == NULL) {
		breakpointsCount--;
	}
})

KRK_FUNC(addbreakpoint,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,lineNo);

	KrkFunction * target = NULL;
	if (IS_CLOSURE(argv[0])) {
		target = AS_CLOSURE(argv[0])->function;
	} else if (IS_BOUND_METHOD(argv[0]) && IS_CLOSURE(OBJECT_VAL(AS_BOUND_METHOD(argv[0])->method))) {
		target = AS_CLOSURE(OBJECT_VAL(AS_BOUND_METHOD(argv[0])->method))->function;
	} else if (IS_FUNCTION(argv[0])) {
		target = AS_FUNCTION(argv[0]);
	} else if (IS_STRING(argv[0])) {
		/* Look at _ALL_ objects... */
		KrkObj * object = vm.objects;
		while (object) {
			if (object->type == OBJ_FUNCTION) {
				KrkChunk * chunk = &((KrkFunction*)object)->chunk;
				if (AS_STRING(argv[0]) == chunk->filename) {
					/* We have a candidate. */
					if (krk_lineNumber(chunk, 0) <= (size_t)lineNo &&
					    krk_lineNumber(chunk,chunk->count) >= (size_t)lineNo) {
						target = (KrkFunction*)object;
						break;
					}
				}
			}
			object = object->next;
		}
		if (!target) {
			return krk_runtimeError(vm.exceptions->valueError, "Could not locate a matching bytecode object.");
		}
	} else {
		return TYPE_ERROR(function or method or filename,argv[0]);
	}

	int index = breakpointsCount;
	if (breakpointsCount == MAX_BREAKPOINTS) {
		/* See if any are available */
		for (int i = 0; i < MAX_BREAKPOINTS; ++i) {
			if (breakpoints[i].inFunction == NULL) {
				index = i;
				break;
			}
		}
		if (index == breakpointsCount) {
			return krk_runtimeError(vm.exceptions->indexError, "Too many active breakpoints, max is %d", MAX_BREAKPOINTS);
		}
	} else {
		index = breakpointsCount++;
	}

	/* Figure out what instruction this should be on */
	size_t last = 0;
	for (size_t i = 0; i < target->chunk.linesCount; ++i) {
		if (target->chunk.lines[i].line > (size_t)lineNo) break;
		if (target->chunk.lines[i].line == (size_t)lineNo) {
			last = target->chunk.lines[i].startOffset;
			break;
		}
		last = target->chunk.lines[i].startOffset;
	}

	breakpoints[index].inFunction = target;
	breakpoints[index].offset = last;
	breakpoints[index].originalOpcode = target->chunk.code[last];
	target->chunk.code[last] = OP_BREAKPOINT;

	return INTEGER_VAL(index);
})

static void debug_dumpTraceback(void) {
	int flagsBefore = krk_currentThread.flags;
	debug_disableSingleStep();
	krk_push(krk_currentThread.currentException);

	krk_runtimeError(vm.exceptions->baseException, "(breakpoint)");
	krk_dumpTraceback();

	krk_currentThread.currentException = krk_pop();
	krk_currentThread.flags = flagsBefore;
}

static void debug_help(void) {
	fprintf(stderr, " c = continue    s = step\n");
	fprintf(stderr, " t = traceback   q = quit\n");
	fprintf(stderr, " b FILE LINE = add breakpoint\n");
	fprintf(stderr, " l = list breakpoints\n");
	fprintf(stderr, " e X = enable breakpoint X\n");
	fprintf(stderr, " d X = disable breakpoint X\n");
}

int (*krk_externalDebuggerHook)(void) = NULL;
int krk_debuggerHook(void) {
	if (krk_externalDebuggerHook) {
		int mode = krk_externalDebuggerHook();
		if (mode == 1) {
			/* Continue */
			debug_disableSingleStep();
		} else if (mode == 2) {
			debug_dumpTraceback();
		}
		return 0;
	}

	if (debuggerShowHello) {
		debuggerShowHello = 0;
		fprintf(stderr, "Entering debugger.\n");
		debug_help();
	}

	KrkCallFrame* frame = &krk_currentThread.frames[krk_currentThread.frameCount - 1];
	fprintf(stderr, "At offset 0x%04lx of function '%s' from '%s' on line %lu:\n",
		(unsigned long)(frame->ip - frame->closure->function->chunk.code),
		frame->closure->function->name->chars,
		frame->closure->function->chunk.filename->chars,
		(unsigned long)krk_lineNumber(&frame->closure->function->chunk,
			(unsigned long)(frame->ip - frame->closure->function->chunk.code)));
	krk_disassembleInstruction(stderr, frame->closure->function,
		(size_t)(frame->ip - frame->closure->function->chunk.code));

	while (1) {
		fprintf(stderr, " dbg> ");
		fflush(stderr);

		char buf[1024];
		if (!fgets(buf,1024,stdin)) {
			fprintf(stderr, "^D\n");
			exit(1);
		}

		char * nl = strstr(buf,"\n");
		if (nl) *nl = '\0';

		if (nl && nl == buf && lastDebuggerCommand) {
			sprintf(buf, "%s", lastDebuggerCommand);
		} else {
			if (lastDebuggerCommand != NULL) free(lastDebuggerCommand);
			lastDebuggerCommand = strdup(buf);
		}

		if (!strcmp(buf,"c")) {
			debug_disableSingleStep();
			return 0;
		} else if (!strcmp(buf,"s")) {
			debug_enableSingleStep();
			return 0;
		} else if (!strcmp(buf,"q")) {
			exit(1);
		} else if (!strcmp(buf,"t")) {
			debug_dumpTraceback();
		} else if (!strcmp(buf,"l")) {
			for (int i = 0; i < MAX_BREAKPOINTS; ++i) {
				if (!breakpoints[i].inFunction) continue;
				fprintf(stderr, "%-2d: %s+0x%04lx (line %d in %s) %s\n",
					i,
					breakpoints[i].inFunction->name->chars,
					(unsigned long)breakpoints[i].offset,
					(int)krk_lineNumber(&breakpoints[i].inFunction->chunk, breakpoints[i].offset),
					breakpoints[i].inFunction->chunk.filename->chars,
					breakpoints[i].inFunction->chunk.code[breakpoints[i].offset] == OP_BREAKPOINT ? "enabled" : "disabled"
				);
			}
		} else if (!strcmp(buf,"h")) {
			debug_help();
		} else if (strstr(buf,"e ") == buf) {
			int breakIndex = atoi(buf+2);
			if (breakIndex <= breakpointsCount || breakpoints[breakIndex].inFunction == NULL) {
				fprintf(stderr, "not a valid breakpoint index\n");
			} else {
				breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] =
					OP_BREAKPOINT;
			}
		} else if (strstr(buf,"d ") == buf) {
			int breakIndex = atoi(buf+2);
			if (breakIndex <= breakpointsCount || breakpoints[breakIndex].inFunction == NULL) {
				fprintf(stderr, "not a valid breakpoint index\n");
			} else {
				breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] =
					breakpoints[breakIndex].originalOpcode;
			}
		} else if (strstr(buf,"b ") == buf) {
			char * filename = buf+2;
			char * afterFilename = strstr(filename, " ");
			if (!afterFilename) {
				fprintf(stderr, "expected a line number\n");
			} else {
				*afterFilename = '\0';
				int lineNumber = atoi(afterFilename+1);

				fprintf(stderr, "Trying to add breakpoint to '%s' at line %d\n", filename, lineNumber);
				krk_push(OBJECT_VAL(krk_copyString(filename,strlen(filename))));
				KrkValue result = FUNC_NAME(krk,addbreakpoint)(2,(KrkValue[]){krk_peek(0),INTEGER_VAL(lineNumber)},0);
				krk_pop();
				if (!IS_INTEGER(result)) {
					fprintf(stderr, "That probably didn't work.\n");
				} else {
					fprintf(stderr, "add breakpoint %d\n", (int)AS_INTEGER(result));
				}
			}
		} else if (strstr(buf,"p ") == buf) {
			int flagsBefore = krk_currentThread.flags;
			debug_disableSingleStep();
			krk_push(krk_currentThread.currentException);
			int previousExitFrame = krk_currentThread.exitOnFrame;
			krk_currentThread.exitOnFrame = krk_currentThread.frameCount;

			/* Compile expression */
			KrkValue value = krk_interpret(buf+2,"<debugger>");
			if (krk_currentThread.flags & KRK_THREAD_HAS_EXCEPTION) {
				fprintf(stderr," [raised exception %s]\n", krk_typeName(krk_currentThread.currentException));
				krk_currentThread.frameCount = krk_currentThread.exitOnFrame;
			} else {
				fprintf(stderr," => ");
				krk_printValue(stderr, value);
				fprintf(stderr,"\n");
			}

			krk_currentThread.exitOnFrame = previousExitFrame;
			krk_currentThread.currentException = krk_pop();
			krk_currentThread.flags = flagsBefore;
		} else {
			fprintf(stderr, "unknown command\n");
		}
	}

	return 0;
}

int krk_debugBreakpointHandler(void) {
	int index = -1;

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount-1];
	KrkFunction * callee = frame->closure->function;
	size_t offset        = (frame->ip - 1) - callee->chunk.code;

	for (int i = 0; i < breakpointsCount; ++i) {
		if (breakpoints[i].inFunction == callee && breakpoints[i].offset == offset) {
			index = i;
		}
	}

	/* A breakpoint instruction without an associated breakpoint entry?
	 * This must be an invalid block of bytecode - just bail! */
	if (index == -1) {
		abort();
	}

	/* Restore the instruction to its original state. If the debugger
	 * wants to break here again it can set the breakpoint again */
	callee->chunk.code[offset] = breakpoints[index].originalOpcode;
	frame->ip--;

	debug_enableSingleStep();
	debuggerShowHello = 1;

	return 0;
}

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
#define OPERAND(opc,more) case opc: { operand = chunk->code[offset + 1]; size = 2; more; break; } \
	case opc ## _LONG: { operand = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); size = 4; more; break; }
#define JUMP(opc,sign) case opc: { jump = 0 sign ((chunk->code[offset + 1] << 8) | (chunk->code[offset + 2])); \
	size = 3; break; }
#define CLOSURE_MORE size += AS_FUNCTION(chunk->constants.values[constant])->upvalueCount * 2
#define EXPAND_ARGS_MORE
#define LOCAL_MORE local = operand;
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
		ssize_t operand = -1;
		ssize_t local = -1;
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
		} else if (local != -1) {
			if ((short int)local < func->requiredArgs) {
				newTuple->values.values[newTuple->values.count++] = func->requiredArgNames.values[local];
			} else if ((short int)local < func->requiredArgs + func->keywordArgs) {
				newTuple->values.values[newTuple->values.count++] = func->keywordArgNames.values[local - func->requiredArgs];
			} else {
				newTuple->values.values[newTuple->values.count++] = INTEGER_VAL(operand); /* Just in case */
				for (size_t i = 0; i < func->localNameCount; ++i) {
					if (func->localNames[i].id == (size_t)local && func->localNames[i].birthday <= offset && func->localNames[i].deathday >= offset) {
						newTuple->values.values[newTuple->values.count-1] = OBJECT_VAL(func->localNames[i].name);
						break;
					}
				}
			}
		} else if (operand != -1) {
			newTuple->values.values[newTuple->values.count++] = INTEGER_VAL(operand);
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

	BIND_FUNC(module, addbreakpoint);
	BIND_FUNC(module, delbreakpoint);
	BIND_FUNC(module, enablebreakpoint);
	BIND_FUNC(module, disablebreakpoint);

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
