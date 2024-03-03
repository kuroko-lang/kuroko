#include <stdio.h>
#include <string.h>

#include <kuroko/debug.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>
#include <kuroko/compiler.h>

#include "private.h"
#include "opcode_enum.h"

#ifndef KRK_DISABLE_DEBUG
#define NOOP (void)0

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
#pragma GCC optimize ("Os")
#endif

#define STRING_DEBUG_TRUNCATE 50

void krk_printValueSafe(FILE * f, KrkValue printable) {
	if (!IS_OBJECT(printable)) {
		switch (KRK_VAL_TYPE(printable)) {
			case KRK_VAL_INTEGER:  fprintf(f, PRIkrk_int, AS_INTEGER(printable)); break;
			case KRK_VAL_BOOLEAN:  fprintf(f, "%s", AS_BOOLEAN(printable) ? "True" : "False"); break;
			case KRK_VAL_NONE:     fprintf(f, "None"); break;
			case KRK_VAL_HANDLER:
				switch (AS_HANDLER_TYPE(printable)) {
					case OP_PUSH_TRY:      fprintf(f, "{try->%d}",     (int)AS_HANDLER_TARGET(printable)); break;
					case OP_PUSH_WITH:     fprintf(f, "{with->%d}",    (int)AS_HANDLER_TARGET(printable)); break;
					case OP_RAISE:         fprintf(f, "{raise<-%d}",   (int)AS_HANDLER_TARGET(printable)); break;
					case OP_FILTER_EXCEPT: fprintf(f, "{except<-%d}",  (int)AS_HANDLER_TARGET(printable)); break;
					case OP_BEGIN_FINALLY: fprintf(f, "{finally<-%d}", (int)AS_HANDLER_TARGET(printable)); break;
					case OP_RETURN:        fprintf(f, "{return<-%d}",  (int)AS_HANDLER_TARGET(printable)); break;
					case OP_END_FINALLY:   fprintf(f, "{end<-%d}",     (int)AS_HANDLER_TARGET(printable)); break;
					case OP_EXIT_LOOP:     fprintf(f, "{exit<-%d}",    (int)AS_HANDLER_TARGET(printable)); break;
					case OP_RAISE_FROM:    fprintf(f, "{reraise<-%d}", (int)AS_HANDLER_TARGET(printable)); break;
				}
				break;
			case KRK_VAL_KWARGS: {
				if (AS_INTEGER(printable) == KWARGS_SINGLE) {
					fprintf(f, "{unpack single}");
				} else if (AS_INTEGER(printable) == KWARGS_LIST) {
					fprintf(f, "{unpack list}");
				} else if (AS_INTEGER(printable) == KWARGS_DICT) {
					fprintf(f, "{unpack dict}");
				} else if (AS_INTEGER(printable) == KWARGS_NIL) {
					fprintf(f, "{unpack nil}");
				} else if (AS_INTEGER(printable) == KWARGS_UNSET) {
					fprintf(f, "{unset default}");
				} else {
					fprintf(f, "{sentinel=" PRIkrk_int "}",AS_INTEGER(printable));
				}
				break;
			}
			default:
#ifndef KRK_NO_FLOAT
				if (IS_FLOATING(printable)) fprintf(f, "%.16g", AS_FLOATING(printable));
#endif
				break;
		}
	} else if (IS_STRING(printable)) {
		fprintf(f, "'");
		/*
		 * Print at most STRING_DEBUG_TRUNCATE characters, as bytes, escaping anything not ASCII.
		 * See also str.__repr__ which does something similar with escape sequences, but this
		 * is a dumber, safer, and slightly faster approach.
		 */
		for (size_t c = 0; c < AS_STRING(printable)->length && c < STRING_DEBUG_TRUNCATE; ++c) {
			unsigned char byte = (unsigned char)AS_CSTRING(printable)[c];
			switch (byte) {
				case '\\': fprintf(f, "\\\\"); break;
				case '\n': fprintf(f, "\\n"); break;
				case '\r': fprintf(f, "\\r"); break;
				case '\'': fprintf(f, "\\'"); break;
				default: {
					if (byte < ' ' || byte > '~') {
						fprintf(f, "\\x%02x", byte);
					} else {
						fprintf(f, "%c", byte);
					}
					break;
				}
			}
		}
		if (AS_STRING(printable)->length > STRING_DEBUG_TRUNCATE) {
			fprintf(f,"...");
		}
		fprintf(f,"'");
	} else {
		switch (AS_OBJECT(printable)->type) {
			case KRK_OBJ_CODEOBJECT: fprintf(f, "<codeobject %s>", AS_codeobject(printable)->name ? AS_codeobject(printable)->name->chars : "?"); break;
			case KRK_OBJ_CLASS: fprintf(f, "<class %s>", AS_CLASS(printable)->name ? AS_CLASS(printable)->name->chars : "?"); break;
			case KRK_OBJ_INSTANCE: fprintf(f, "<instance of %s>", AS_INSTANCE(printable)->_class->name->chars); break;
			case KRK_OBJ_NATIVE: fprintf(f, "<nativefn %s>", ((KrkNative*)AS_OBJECT(printable))->name); break;
			case KRK_OBJ_CLOSURE: fprintf(f, "<function %s>", AS_CLOSURE(printable)->function->name->chars); break;
			case KRK_OBJ_BYTES: fprintf(f, "<bytes of len %ld>", (long)AS_BYTES(printable)->length); break;
			case KRK_OBJ_TUPLE: {
				fprintf(f, "(");
				for (size_t i = 0; i < AS_TUPLE(printable)->values.count; ++i) {
					krk_printValueSafe(f, AS_TUPLE(printable)->values.values[i]);
					if (i + 1 != AS_TUPLE(printable)->values.count) {
						fprintf(f, ",");
					}
				}
				fprintf(f, ")");
			} break;
			case KRK_OBJ_BOUND_METHOD: fprintf(f, "<method %s>",
				AS_BOUND_METHOD(printable)->method ? (
				AS_BOUND_METHOD(printable)->method->type == KRK_OBJ_CLOSURE ? ((KrkClosure*)AS_BOUND_METHOD(printable)->method)->function->name->chars :
					(AS_BOUND_METHOD(printable)->method->type == KRK_OBJ_NATIVE ? ((KrkNative*)AS_BOUND_METHOD(printable)->method)->name : "(unknown)")) : "(corrupt bound method)"); break;
			default: fprintf(f, "<%s>", krk_typeName(printable)); break;
		}
	}
}


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
			int found = 0;
			for (size_t j = 0; j < f->closure->function->localNameCount; ++j) {
				if (relative == f->closure->function->localNames[j].id
					/* Only display this name if it's currently valid */
					&&  f->closure->function->localNames[j].birthday <= (size_t)(f->ip - f->closure->function->chunk.code)
					&&  f->closure->function->localNames[j].deathday >= (size_t)(f->ip - f->closure->function->chunk.code)
					) {
					fprintf(file, "%s=", f->closure->function->localNames[j].name->chars);
					found = 1;
					break;
				}
			}
			if (found) break;
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
	int j = 0;
	for (int i = 0; i < func->potentialPositionals; ++i) {
		fprintf(f,"%s",func->localNames[j].name->chars);
		if (j + 1 < func->totalArguments) fprintf(f,",");
		j++;
	}
	if (func->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS) {
		fprintf(f,"*%s",func->localNames[j].name->chars);
		if (j + 1 < func->totalArguments) fprintf(f,",");
		j++;
	}
	for (int i = 0; i < func->keywordArgs; ++i) {
		fprintf(f,"%s=",func->localNames[j].name->chars);
		if (j + 1 < func->totalArguments) fprintf(f,",");
		j++;
	}
	if (func->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_KWS) {
		fprintf(f,"**%s",func->localNames[j].name->chars);
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

	if (IS_NONE(func->jumpTargets)) {
		func->jumpTargets = krk_dict_of(0,NULL,0);
#define SIMPLE(opc) case opc: size = 1; break;
#define CONSTANT(opc,more) case opc: { size_t constant _unused = chunk->code[offset + 1]; size = 2; more; break; } \
	case opc ## _LONG: { size_t constant _unused = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); size = 4; more; break; }
#define OPERANDB(opc,more) case opc: { size = 2; more; break; }
#define OPERAND(opc,more) OPERANDB(opc,more) \
	case opc ## _LONG: { size = 4; more; break; }
#define JUMP(opc,sign) case opc: { uint16_t jump = (chunk->code[offset + 1] << 8) | (chunk->code[offset + 2]); \
	krk_tableSet(AS_DICT(func->jumpTargets), INTEGER_VAL((size_t)(offset + 3 sign jump)), BOOLEAN_VAL(1)); \
	size = 3; break; }
#define COMPLICATED(opc,more) case opc: size = 1; more; break;
#define OVERLONG_JUMP_MORE size += 2
#define CLOSURE_MORE \
	KrkCodeObject * function = AS_codeobject(chunk->constants.values[constant]); \
	for (size_t j = 0; j < function->upvalueCount; ++j) { \
		int isLocal = chunk->code[offset++ + size]; \
		offset++; \
		if (isLocal & 2) { \
			offset += 2; \
		} \
	}
#define EXPAND_ARGS_MORE
#define LOCAL_MORE
#define FORMAT_VALUE_MORE

	while (offset < chunk->count) {
		uint8_t opcode = chunk->code[offset];
		size_t size = 0;
		switch (opcode) {
#include "opcodes.h"
		}
		offset += size;
	}
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
	}

	if (!IS_dict(func->jumpTargets)) return 0;
	KrkValue garbage;
	if (krk_tableGet(AS_DICT(func->jumpTargets), INTEGER_VAL(startPoint), &garbage)) return 1;
	return 0;
}

#define OPARGS FILE * f, const char * fullName, size_t * size, size_t * offset, KrkCodeObject * func, KrkChunk * chunk
#define OPARG_VALS f,fullName,size,offset,func,chunk

static void _print_opcode(OPARGS) {
	fprintf(f, "%-16s ", opcodeClean(fullName));
}

static void _simple(OPARGS) {
	_print_opcode(OPARG_VALS);
	fprintf(f, "     ");
	*size = 1;
}

static void _constant(OPARGS, int isLong, void (*more)(OPARGS, size_t constant)) {
	_print_opcode(OPARG_VALS);
	size_t constant = isLong ? (chunk->code[*offset + 1] << 16) | (chunk->code[*offset + 2] << 8) | (chunk->code[*offset + 3]) : chunk->code[*offset + 1];
	fprintf(f, "%4d ", (int)constant);
	krk_printValueSafe(f, chunk->constants.values[constant]);
	*size = isLong ? 4 : 2;
	if (more) more(OPARG_VALS, constant);
}

static void _operand(OPARGS, int isLong, void (*more)(OPARGS, size_t constant)) {
	_print_opcode(OPARG_VALS);
	uint32_t operand = isLong ? (chunk->code[*offset + 1] << 16) | (chunk->code[*offset + 2] << 8) | (chunk->code[*offset + 3]) : chunk->code[*offset + 1];
	fprintf(f, "%4d", (int)operand);
	*size = isLong ? 4 : 2;
	if (more) more(OPARG_VALS, operand);
}

static void _jump(OPARGS, int sign) {
	_print_opcode(OPARG_VALS);
	uint16_t jump = (chunk->code[*offset + 1] << 8) | (chunk->code[*offset + 2]);
	fprintf(f, "%4d (to %d)", (int)jump, (int)(*offset + 3 + sign * jump));
	*size = 3;
}

static void _complicated(OPARGS, void (*more)(OPARGS)) {
	_print_opcode(OPARG_VALS);
	if (more) more(OPARG_VALS);
	else *size = 1;
}

#define SIMPLE(opc)
#define JUMP(opc,sign) case opc: fprintf(f, "(%s, to %zu)", opcodeClean(#opc), *offset + 3 sign current_jump); return;
#define OPERAND(opc,more)
#define CONSTANT(opc,more)
#define COMPLICATED(opc,more)
static void _overlong_jump_more(OPARGS) {
	size_t current_jump = (chunk->code[*offset + 1] << 8) | (chunk->code[*offset + 2]);
	*size = 3;

	/* Now look it up */
	for (size_t i = 0; i < func->overlongJumpsCount; ++i) {
		if (*offset + 1 == (size_t)func->overlongJumps[i].instructionOffset) {
			current_jump |= ((size_t)func->overlongJumps[i].intendedTarget << 16);
			switch (func->overlongJumps[i].originalOpcode) {
#include "opcodes.h"
				default: break;
			}
		}
	}

	fprintf(f,"(invalid destination)");
}
#undef SIMPLE
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef COMPLICATED


#undef NOOP
#define NOOP (NULL)
#define SIMPLE(opc) case opc: _simple(f,#opc,&size,&offset,func,chunk); break;
#define CONSTANT(opc,more) case opc: _constant(f,#opc,&size,&offset,func,chunk,0,more); break; \
	case opc ## _LONG: _constant(f,#opc "_LONG",&size,&offset,func,chunk,1,more); break;
#define OPERAND(opc,more) case opc: _operand(f,#opc,&size,&offset,func,chunk,0,more); break; \
	case opc ## _LONG: _operand(f,#opc "_LONG",&size,&offset,func,chunk,1,more); break;
#define JUMP(opc,sign) case opc: _jump(f,#opc,&size,&offset,func,chunk,sign 1); break;
#define COMPLICATED(opc,more) case opc: _complicated(f,#opc,&size,&offset,func,chunk,more); break;

#define OVERLONG_JUMP_MORE _overlong_jump_more

#define CLOSURE_MORE _closure_more

static void _closure_more(OPARGS, size_t constant) {
	KrkCodeObject * function = AS_codeobject(chunk->constants.values[constant]);
	fprintf(f, " ");
	for (size_t j = 0; j < function->upvalueCount; ++j) {
		int isLocal = chunk->code[(*offset)++ + *size];
		int index = chunk->code[(*offset)++ + *size];
		if (isLocal & 2) {
			index = (index << 16) | (chunk->code[*offset + *size] << 8) | chunk->code[*offset + 1 + *size];
			offset += 2;
		}
		if (isLocal & 1) {
			for (size_t i = 0; i < func->localNameCount; ++i) {
				if (func->localNames[i].id == (size_t)index && func->localNames[i].birthday <= *offset && func->localNames[i].deathday >= *offset) {
					fprintf(f, "%s", func->localNames[i].name->chars);
					break;
				}
			}
		} else if (isLocal & 4) {
			fprintf(f, "classcell");
		} else { fprintf(f, "upvalue<%d>", index); }
		if (j + 1 != function->upvalueCount) fprintf(f, ", ");
	}
}

#define EXPAND_ARGS_MORE _expand_args_more

static void _expand_args_more(OPARGS, size_t operand) {
	fprintf(f, " (%s)", operand == 0 ? "singleton" : (operand == 1 ? "list" : "dict"));
}

#define FORMAT_VALUE_MORE _format_value_more

static void _format_value_more(OPARGS, size_t operand) {
	if (operand != 0) {
		int hasThing = 0;
		fprintf(f, " (");
		if (operand & FORMAT_OP_EQ)     { fprintf(f, "eq"); hasThing = 1; }
		if (operand & FORMAT_OP_STR)    { fprintf(f, "%sstr", hasThing ? ", " : ""); hasThing = 1; }
		if (operand & FORMAT_OP_REPR)   { fprintf(f, "%srepr", hasThing ? ", " : ""); hasThing = 1; }
		if (operand & FORMAT_OP_FORMAT) { fprintf(f, "%swith format", hasThing ? ", " : ""); }
		fprintf(f, ")");
	}
}

#define LOCAL_MORE _local_more
static void _local_more(OPARGS, size_t operand) {
	for (size_t i = 0; i < func->localNameCount; ++i) {
		if (func->localNames[i].id == operand && func->localNames[i].birthday <= *offset && func->localNames[i].deathday >= *offset) {
			fprintf(f, " (%s", func->localNames[i].name->chars);
			if ((short int) operand < func->potentialPositionals) {
				fprintf(f, ", arg");
			} else if ((short int)operand < func->potentialPositionals + func->keywordArgs + !!(func->obj.flags & KRK_OBJ_FLAGS_CODEOBJECT_COLLECTS_ARGS)) {
				fprintf(f, ", kwarg");
			}
			fprintf(f, ")");
			break;
		}
	}
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
#undef COMPLICATED
#undef OVERLONG_JUMP_MORE
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE
#undef FORMAT_VALUE_MORE
#undef NOOP

int krk_debug_addBreakpointCodeOffset(KrkCodeObject * target, size_t offset, int flags) {
	int index = vm.dbgState->breakpointsCount;
	if (vm.dbgState->breakpointsCount == MAX_BREAKPOINTS) {
		/* See if any are available */
		for (int i = 0; i < MAX_BREAKPOINTS; ++i) {
			if (vm.dbgState->breakpoints[i].inFunction == NULL) {
				index = i;
				break;
			}
		}
		if (index == vm.dbgState->breakpointsCount) {
			return -1;
		}
	} else {
		index = vm.dbgState->breakpointsCount++;
	}

	vm.dbgState->breakpoints[index].inFunction = target;
	vm.dbgState->breakpoints[index].offset = offset;
	vm.dbgState->breakpoints[index].originalOpcode = target->chunk.code[offset];
	vm.dbgState->breakpoints[index].flags = flags;
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
	if (breakIndex < 0 || breakIndex >= vm.dbgState->breakpointsCount || vm.dbgState->breakpoints[breakIndex].inFunction == NULL)
		return 1;
	vm.dbgState->breakpoints[breakIndex].inFunction->chunk.code[vm.dbgState->breakpoints[breakIndex].offset] = OP_BREAKPOINT;
	return 0;
}

int krk_debug_disableBreakpoint(int breakIndex) {
	if (breakIndex < 0 || breakIndex >= vm.dbgState->breakpointsCount || vm.dbgState->breakpoints[breakIndex].inFunction == NULL)
		return 1;
	vm.dbgState->breakpoints[breakIndex].inFunction->chunk.code[vm.dbgState->breakpoints[breakIndex].offset] =
		vm.dbgState->breakpoints[breakIndex].originalOpcode;
	if (breakIndex == vm.dbgState->repeatStack_top) {
		vm.dbgState->repeatStack_top = -1;
	}
	return 0;
}

int krk_debug_removeBreakpoint(int breakIndex) {
	if (breakIndex < 0 || breakIndex >= vm.dbgState->breakpointsCount || vm.dbgState->breakpoints[breakIndex].inFunction == NULL)
		return 1;
	krk_debug_disableBreakpoint(breakIndex);
	vm.dbgState->breakpoints[breakIndex].inFunction = NULL;
	while (vm.dbgState->breakpointsCount && vm.dbgState->breakpoints[vm.dbgState->breakpointsCount-1].inFunction == NULL) {
		vm.dbgState->breakpointsCount--;
	}
	return 0;
}
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
	if (!vm.dbgState->debuggerHook)
		abort();

	if (vm.dbgState->repeatStack_top != -1) {
		/* Re-enable stored repeat breakpoint */
		krk_debug_enableBreakpoint(vm.dbgState->repeatStack_top);
	}

	vm.dbgState->repeatStack_top = vm.dbgState->repeatStack_bottom;
	vm.dbgState->repeatStack_bottom = -1;

	if (!vm.dbgState->thisWasForced) {
		int result = vm.dbgState->debuggerHook(frame);
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
		vm.dbgState->thisWasForced = 0;
	}

	/* If the top of the repeat stack is an index, we need to ensure we re-enable that breakpoint */
	if (vm.dbgState->repeatStack_top != -1 && !(krk_currentThread.flags & KRK_THREAD_SINGLE_STEP)) {
		vm.dbgState->thisWasForced = 1;
		krk_debug_enableSingleStep();
	}

	return 0;
}

int krk_debug_registerCallback(KrkDebugCallback hook) {
	if (vm.dbgState->debuggerHook) return 1;
	vm.dbgState->debuggerHook = hook;
	return 0;
}

int krk_debug_examineBreakpoint(int breakIndex, KrkCodeObject ** funcOut, size_t * offsetOut, int * flagsOut, int * enabled) {
	if (breakIndex < 0 || breakIndex >= vm.dbgState->breakpointsCount)
		return -1;
	if (vm.dbgState->breakpoints[breakIndex].inFunction == NULL)
		return -2;

	if (funcOut) *funcOut = vm.dbgState->breakpoints[breakIndex].inFunction;
	if (offsetOut) *offsetOut = vm.dbgState->breakpoints[breakIndex].offset;
	if (flagsOut) *flagsOut = vm.dbgState->breakpoints[breakIndex].flags;
	if (enabled) *enabled = (vm.dbgState->breakpoints[breakIndex].inFunction->chunk.code[vm.dbgState->breakpoints[breakIndex].offset] == OP_BREAKPOINT) || breakIndex == vm.dbgState->repeatStack_top;

	return 0;
}

int krk_debugBreakpointHandler(void) {
	int index = -1;

	KrkCallFrame * frame = &krk_currentThread.frames[krk_currentThread.frameCount-1];
	KrkCodeObject * callee = frame->closure->function;
	size_t offset        = (frame->ip - 1) - callee->chunk.code;

	for (int i = 0; i < vm.dbgState->breakpointsCount; ++i) {
		if (vm.dbgState->breakpoints[i].inFunction == callee && vm.dbgState->breakpoints[i].offset == offset) {
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
	callee->chunk.code[offset] = vm.dbgState->breakpoints[index].originalOpcode;

	/* If this was a single-shot, we can remove it. */
	if (vm.dbgState->breakpoints[index].flags == KRK_BREAKPOINT_ONCE) {
		krk_debug_removeBreakpoint(index);
	} else if (vm.dbgState->breakpoints[index].flags == KRK_BREAKPOINT_REPEAT) {
		vm.dbgState->repeatStack_bottom = index;
	}

	/* Rewind to rerun this instruction. */
	frame->ip--;

	return krk_debuggerHook(frame);
}

void krk_debug_init(void) {
	vm.dbgState = calloc(1, sizeof(struct DebuggerState));
	vm.dbgState->repeatStack_top = -1;
	vm.dbgState->repeatStack_bottom = -1;
}

void krk_debug_addExpression(KrkCodeObject * codeobject, uint8_t start, uint8_t midStart, uint8_t midEnd, uint8_t end) {
	/* Traceback entries point to the last byte of an opcode, due to the way instruction fetch
	 * advances the instruction pointer past all of the constituent operands of an opcode; as
	 * such, our map is based on these last bytes and we need to look at the byte preceding
	 * the current count when adding new entries. */
	size_t offset = codeobject->chunk.count - 1;

	/* We can feasibly support offsets larger than UINT32_MAX on 64-bit platforms, though this
	 * should never really happen. Just in case, avoid messing up our table with bad values. */
	if (offset > UINT32_MAX) return;

	if (codeobject->expressionsCapacity < codeobject->expressionsCount + 1) {
		size_t old = codeobject->expressionsCapacity;
		codeobject->expressionsCapacity = KRK_GROW_CAPACITY(old);
		codeobject->expressions = KRK_GROW_ARRAY(KrkExpressionsMap, codeobject->expressions, old, codeobject->expressionsCapacity);
	}

	codeobject->expressions[codeobject->expressionsCount] = (KrkExpressionsMap){offset,start,midStart,midEnd,end};
	codeobject->expressionsCount++;
}

int krk_debug_expressionUnderline(const KrkCodeObject* codeobject, uint8_t* start, uint8_t* midStart, uint8_t* midEnd, uint8_t* end, size_t instruction) {
	/* We could do binary search here, but as we only print these when an exception 'escapes',
	 * it's not really worth the optimization over a linear search per line in the traceback. */
	for (size_t i = 0; i < codeobject->expressionsCount; ++i) {
		if (codeobject->expressions[i].bytecodeOffset == instruction) {
			*start = codeobject->expressions[i].start;
			*midStart = codeobject->expressions[i].midStart;
			*midEnd = codeobject->expressions[i].midEnd;
			*end = codeobject->expressions[i].end;
			return 1;
		}
	}
	return 0;
}

#endif
