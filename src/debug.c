#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "vm.h"
#include "util.h"
#include "compiler.h"

/**
 * When tracing is enabled, we will present the elements on the stack with
 * a safe printer; the format of values printed by krk_printValueSafe will
 * look different from those printed by printValue, but they guarantee that
 * the VM will never be called to produce a string, which would result in
 * a nasty infinite recursion if we did it while trying to trace the VM!
 */
void krk_debug_dumpStack(FILE * file, KrkCallFrame * frame) {
	size_t i = 0;
	if (!frame) frame = &krk_currentThread.frames[krk_currentThread.frameCount-1];
	for (KrkValue * slot = krk_currentThread.stack; slot < krk_currentThread.stackTop; slot++) {
		fprintf(file, "[%c", frame->slots == i ? '*' : ' ');

		for (size_t x = krk_currentThread.frameCount; x > 0; x--) {
			if (krk_currentThread.frames[x-1].slots > i) continue;
			KrkCallFrame * f = &krk_currentThread.frames[x-1];
			size_t relative = i - f->slots;

			/* Figure out the name of this value */
			if (relative < (size_t)f->closure->function->requiredArgs) {
				fprintf(file, "%s=", AS_CSTRING(f->closure->function->requiredArgNames.values[relative]));
				break;
			} else if (relative < (size_t)f->closure->function->requiredArgs + (size_t)f->closure->function->keywordArgs) {
				fprintf(file, "%s=", AS_CSTRING(f->closure->function->keywordArgNames.values[relative - f->closure->function->requiredArgs]));
				break;
			} else {
				int found = 0;
				for (size_t j = 0; j < f->closure->function->localNameCount; ++j) {
					if (relative == f->closure->function->localNames[j].id
						/* Only display this name if it's currently valid */
						&&  f->closure->function->localNames[j].birthday <= (size_t)(f->ip - f->closure->function->chunk.code)
						) {
						fprintf(file, "%s=", f->closure->function->localNames[j].name->chars);
						found = 1;
						break;
					}
				}
				if (found) break;
			}
		}

		krk_printValueSafe(file, *slot);
		fprintf(file, " ]");
		i++;
	}
	if (i == frame->slots) {
		fprintf(file, " * ");
	}
	fprintf(file, "\n");
}


void krk_disassembleCodeObject(FILE * f, KrkCodeObject * func, const char * name) {
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

static int isJumpTarget(KrkCodeObject * func, size_t startPoint) {
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
#define CLOSURE_MORE size += AS_codeobject(chunk->constants.values[constant])->upvalueCount * 2
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
	KrkCodeObject * function = AS_codeobject(chunk->constants.values[constant]); \
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

size_t krk_disassembleInstruction(FILE * f, KrkCodeObject * func, size_t offset) {
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

struct BreakpointEntry {
	KrkCodeObject * inFunction;
	size_t offset;
	int flags;
	uint8_t originalOpcode;
};

#define MAX_BREAKPOINTS 32
static struct BreakpointEntry breakpoints[MAX_BREAKPOINTS] = {0};
static int breakpointsCount = 0;

static KrkDebugCallback _debugger_hook = NULL;

/* Internal state tracks re-enabling repeat breakpoints */
static threadLocal int _repeatStack_top    = -1;
static threadLocal int _repeatStack_bottom = -1;
static threadLocal int _thisWasForced = 0;

int krk_debug_addBreakpointCodeOffset(KrkCodeObject * target, size_t offset, int flags) {
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
			return -1;
		}
	} else {
		index = breakpointsCount++;
	}

	breakpoints[index].inFunction = target;
	breakpoints[index].offset = offset;
	breakpoints[index].originalOpcode = target->chunk.code[offset];
	breakpoints[index].flags = flags;
	target->chunk.code[offset] = OP_BREAKPOINT;

	return index;
}

int krk_debug_addBreakpointFileLine(KrkString * filename, size_t line, int flags) {

	KrkCodeObject * target = NULL;

	/* Examine all code objects to find one that matches the requested
	 * filename and line number... */
	KrkObj * object = vm.objects;
	while (object) {
		if (object->type == KRK_OBJ_CODEOBJECT) {
			KrkChunk * chunk = &((KrkCodeObject*)object)->chunk;
			if (filename == chunk->filename) {
				/* We have a candidate. */
				if (krk_lineNumber(chunk, 0) <= line &&
				    krk_lineNumber(chunk,chunk->count) >= line) {
					target = (KrkCodeObject*)object;
					break;
				}
			}
		}
		object = object->next;
	}

	/* No matching function was found... */
	if (!target) return -1;

	/* Find the right offset in this function */

	size_t offset = 0;
	for (size_t i = 0; i < target->chunk.linesCount; ++i) {
		if (target->chunk.lines[i].line > line) break;
		if (target->chunk.lines[i].line == line) {
			offset = target->chunk.lines[i].startOffset;
			break;
		}
		offset = target->chunk.lines[i].startOffset;
	}

	return krk_debug_addBreakpointCodeOffset(target, offset, flags);
}

int krk_debug_enableBreakpoint(int breakIndex) {
	if (breakIndex < 0 || breakIndex >= breakpointsCount || breakpoints[breakIndex].inFunction == NULL)
		return 1;
	breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] = OP_BREAKPOINT;
	return 0;
}
KRK_FUNC(enablebreakpoint,{
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (krk_debug_enableBreakpoint(breakIndex))
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
})

int krk_debug_disableBreakpoint(int breakIndex) {
	if (breakIndex < 0 || breakIndex >= breakpointsCount || breakpoints[breakIndex].inFunction == NULL)
		return 1;
	breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] =
		breakpoints[breakIndex].originalOpcode;
	if (breakIndex == _repeatStack_top) {
		_repeatStack_top = -1;
	}
	return 0;
}
KRK_FUNC(disablebreakpoint,{
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (krk_debug_disableBreakpoint(breakIndex))
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
})

int krk_debug_removeBreakpoint(int breakIndex) {
	if (breakIndex < 0 || breakIndex >= breakpointsCount || breakpoints[breakIndex].inFunction == NULL)
		return 1;
	krk_debug_disableBreakpoint(breakIndex);
	breakpoints[breakIndex].inFunction = NULL;
	while (breakpointsCount && breakpoints[breakpointsCount-1].inFunction == NULL) {
		breakpointsCount--;
	}
	return 0;
}
KRK_FUNC(delbreakpoint,{
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (krk_debug_removeBreakpoint(breakIndex))
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
})

KRK_FUNC(addbreakpoint,{
	FUNCTION_TAKES_EXACTLY(2);
	CHECK_ARG(1,int,krk_integer_type,lineNo);

	int flags = KRK_BREAKPOINT_NORMAL;

	if (hasKw) {
		KrkValue flagsValue = NONE_VAL();
		if (krk_tableGet(AS_DICT(argv[argc]), OBJECT_VAL(S("flags")), &flagsValue)) {
			if (!IS_INTEGER(flagsValue))
				return TYPE_ERROR(int,flagsValue);
			flags = AS_INTEGER(flagsValue);
		}
	}

	int result;
	if (IS_STRING(argv[0])) {
		result = krk_debug_addBreakpointFileLine(AS_STRING(argv[0]), lineNo, flags);
	} else {
		KrkCodeObject * target = NULL;
		if (IS_CLOSURE(argv[0])) {
			target = AS_CLOSURE(argv[0])->function;
		} else if (IS_BOUND_METHOD(argv[0]) && IS_CLOSURE(OBJECT_VAL(AS_BOUND_METHOD(argv[0])->method))) {
			target = AS_CLOSURE(OBJECT_VAL(AS_BOUND_METHOD(argv[0])->method))->function;
		} else if (IS_codeobject(argv[0])) {
			target = AS_codeobject(argv[0]);
		} else {
			return TYPE_ERROR(function or method or filename,argv[0]);
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
		result = krk_debug_addBreakpointCodeOffset(target,last,flags);
	}

	if (result < 0)
		return krk_runtimeError(vm.exceptions->baseException, "Could not add breakpoint.");

	return INTEGER_VAL(result);
})

/*
 * Begin debugger utility functions.
 *
 * These functions are exported for use in debuggers that
 * are registered with krk_registerDebugger(...);
 */

/**
 * @brief Safely print traceback data.
 *
 * Since traceback printing may involve calling into user code,
 * we clear the debugging bits. Then we make a new exception
 * to attach a traceback to.
 */
void krk_debug_dumpTraceback(void) {
	int flagsBefore = krk_currentThread.flags;
	krk_debug_disableSingleStep();
	krk_push(krk_currentThread.currentException);

	krk_runtimeError(vm.exceptions->baseException, "(breakpoint)");
	krk_dumpTraceback();

	krk_currentThread.currentException = krk_pop();
	krk_currentThread.flags = flagsBefore;
}

void krk_debug_enableSingleStep(void) {
	krk_currentThread.flags |= KRK_THREAD_SINGLE_STEP;
}

void krk_debug_disableSingleStep(void) {
	krk_currentThread.flags &= ~(KRK_THREAD_SINGLE_STEP);
}

int krk_debuggerHook(KrkCallFrame * frame) {
	if (!_debugger_hook)
		abort();

	if (_repeatStack_top != -1) {
		/* Re-enable stored repeat breakpoint */
		krk_debug_enableBreakpoint(_repeatStack_top);
	}

	_repeatStack_top = _repeatStack_bottom;
	_repeatStack_bottom = -1;

	if (!_thisWasForced) {
		int result = _debugger_hook(frame);
		switch (result) {
			case KRK_DEBUGGER_CONTINUE:
				krk_debug_disableSingleStep();
				break;
			case KRK_DEBUGGER_ABORT:
				abort();
				break;
			case KRK_DEBUGGER_STEP:
				krk_debug_enableSingleStep();
				break;
			case KRK_DEBUGGER_QUIT:
				exit(0);
				break;
			case KRK_DEBUGGER_RAISE:
				krk_runtimeError(vm.exceptions->baseException, "raise from debugger");
				break;
		}
	} else {
		/* If we weren't asked to step to the next breakpoint, we need to disable single stepping. */
		krk_debug_disableSingleStep();
		_thisWasForced = 0;
	}

	/* If the top of the repeat stack is an index, we need to ensure we re-enable that breakpoint */
	if (_repeatStack_top != -1 && !(krk_currentThread.flags & KRK_THREAD_SINGLE_STEP)) {
		_thisWasForced = 1;
		krk_debug_enableSingleStep();
	}

	return 0;
}

int krk_debug_registerCallback(KrkDebugCallback hook) {
	if (_debugger_hook) return 1;
	_debugger_hook = hook;
	return 0;
}

int krk_debug_examineBreakpoint(int breakIndex, KrkCodeObject ** funcOut, size_t * offsetOut, int * flagsOut, int * enabled) {
	if (breakIndex < 0 || breakIndex >= breakpointsCount)
		return -1;
	if (breakpoints[breakIndex].inFunction == NULL)
		return -2;

	if (funcOut) *funcOut = breakpoints[breakIndex].inFunction;
	if (offsetOut) *offsetOut = breakpoints[breakIndex].offset;
	if (flagsOut) *flagsOut = breakpoints[breakIndex].flags;
	if (enabled) *enabled = (breakpoints[breakIndex].inFunction->chunk.code[breakpoints[breakIndex].offset] == OP_BREAKPOINT) || breakIndex == _repeatStack_top;

	return 0;
}

int krk_debugBreakpointHandler(void) {
	int index = -1;

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount-1];
	KrkCodeObject * callee = frame->closure->function;
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

	/* If this was a single-shot, we can remove it. */
	if (breakpoints[index].flags == KRK_BREAKPOINT_ONCE) {
		krk_debug_removeBreakpoint(index);
	} else if (breakpoints[index].flags == KRK_BREAKPOINT_REPEAT) {
		_repeatStack_bottom = index;
	}

	/* Rewind to rerun this instruction. */
	frame->ip--;

	return krk_debuggerHook(frame);
}

/**
 * dis.dis(object)
 */
KRK_FUNC(dis,{
	FUNCTION_TAKES_EXACTLY(1);

	if (IS_CLOSURE(argv[0])) {
		KrkCodeObject * func = AS_CLOSURE(argv[0])->function;
		krk_disassembleCodeObject(stdout, func, func->name ? func->name->chars : "(unnamed)");
	} else if (IS_BOUND_METHOD(argv[0])) {
		if (AS_BOUND_METHOD(argv[0])->method->type == OBJ_CLOSURE) {
			KrkCodeObject * func = ((KrkClosure*)AS_BOUND_METHOD(argv[0])->method)->function;
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
		if (krk_tableGet(&AS_CLASS(argv[0])->methods, OBJECT_VAL(S("__func__")), &code) && IS_CLOSURE(code)) {
			KrkCodeObject * func = AS_CLOSURE(code)->function;
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
	KrkCodeObject * c = krk_compile(code->chars,"<source>");
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
#define CLOSURE_MORE size += AS_codeobject(chunk->constants.values[constant])->upvalueCount * 2
#define EXPAND_ARGS_MORE
#define LOCAL_MORE local = operand;
FUNC_SIG(krk,examine) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,codeobject,KrkCodeObject*,func);

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
	KRK_DOC(module,
		"@brief Provides tools for disassembling bytecode.\n\n"
		"### Code Disassembly in Kuroko\n\n"
		"The @c dis module contains functions for dealing with _code objects_ which "
		"represent the compiled bytecode of a Kuroko function. The bytecode compilation "
		"process is entirely static and bytecode analysis can be performed without calling "
		"into the VM to run dynamic code.\n\n"
		"### Debugger Breakpoints\n\n"
		"Kuroko interpreters can provide a debugger hook through the C API's "
		"@ref krk_debug_registerCallback() function. Breakpoints can be managed both "
		"from the C API and from this module's @ref addbreakpoint, @ref delbreakpoint, "
		"@ref enablebreakpoint, and @ref disablebreakpoint methods."
	);

	KRK_DOC(BIND_FUNC(module, dis),
		"@brief Disassemble an object.\n"
		"@arguments obj\n\n"
		"Dumps a disassembly of the bytecode in the code object associated with @p obj. "
		"If @p obj can not be disassembled, a @ref TypeError is raised.");

	KRK_DOC(BIND_FUNC(module, build),
		"@brief Compile a string to a code object.\n"
		"@arguments code\n\n"
		"Compiles the string @p code and returns a code object. If a syntax "
		"error is encountered, it will be raised.");

	KRK_DOC(BIND_FUNC(module, examine),
		"@brief Convert a code object to a list of instructions.\n"
		"@arguments func\n\n"
		"Examines the code object @p func and returns a list representation of its instructions. "
		"Each instruction entry is a tuple of the opcode, total instruction size in bytes, and "
		"the operand of the argument, either as an integer for jump offsets, the actual value for "
		"constant operands, or the name of a local or global variable if available.");

	KRK_DOC(BIND_FUNC(module, addbreakpoint),
		"@brief Attach a breakpoint to a code object.\n"
		"@arguments func, line\n\n"
		"@p func may be a filename string, or a function, method, or code object. Returns "
		"the new breakpoint index, or raises @ref Exception if a breakpoint code not be added.");

	KRK_DOC(BIND_FUNC(module, delbreakpoint),
		"@brief Delete a breakpoint.\n"
		"@arguments handle\n\n"
		"Delete the breakpoint specified by @p handle, disabling it if it was enabled. "
		"May raise @ref IndexError if @p handle is not a valid breakpoint handle.");

	KRK_DOC(BIND_FUNC(module, enablebreakpoint),
		"@brief Enable a breakpoint.\n"
		"@arguments handle\n\n"
		"Enable the breakpoint specified by @p handle. May raise @ref IndexError if "
		"@p handle is not a valid breakpoint handle.");

	KRK_DOC(BIND_FUNC(module, disablebreakpoint),
		"@brief Disable a breakpoint.\n"
		"@arguments handle\n\n"
		"Disable the breakpoint specified by @p handle. May raise @ref IndexError if "
		"@p handle is not a valid breakpoint handle.");

	krk_attachNamedValue(&module->fields, "BREAKPOINT_ONCE", INTEGER_VAL(KRK_BREAKPOINT_ONCE));
	krk_attachNamedValue(&module->fields, "BREAKPOINT_REPEAT", INTEGER_VAL(KRK_BREAKPOINT_REPEAT));

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
