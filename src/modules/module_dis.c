#include <stdio.h>
#include <string.h>

#include <kuroko/debug.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>
#include <kuroko/compiler.h>

#include "../private.h"
#include "../opcode_enum.h"

#ifndef KRK_DISABLE_DEBUG

KRK_Function(enablebreakpoint) {
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (krk_debug_enableBreakpoint(breakIndex))
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
	return NONE_VAL();
}

KRK_Function(disablebreakpoint) {
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (krk_debug_disableBreakpoint(breakIndex))
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
	return NONE_VAL();
}

KRK_Function(delbreakpoint) {
	CHECK_ARG(0,int,krk_integer_type,breakIndex);
	if (krk_debug_removeBreakpoint(breakIndex))
		return krk_runtimeError(vm.exceptions->indexError, "invalid breakpoint id");
	return NONE_VAL();
}

KRK_Function(addbreakpoint) {
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
}


/**
 * dis.dis(object)
 */
KRK_Function(dis) {
	FUNCTION_TAKES_EXACTLY(1);

	if (IS_CLOSURE(argv[0])) {
		KrkCodeObject * func = AS_CLOSURE(argv[0])->function;
		krk_disassembleCodeObject(stdout, func, func->name ? func->name->chars : "<unnamed>");
	} else if (IS_codeobject(argv[0])) {
		krk_disassembleCodeObject(stdout, AS_codeobject(argv[0]), AS_codeobject(argv[0])->name ? AS_codeobject(argv[0])->name->chars : "<unnamed>");
	} else if (IS_BOUND_METHOD(argv[0])) {
		if (AS_BOUND_METHOD(argv[0])->method->type == KRK_OBJ_CLOSURE) {
			KrkCodeObject * func = ((KrkClosure*)AS_BOUND_METHOD(argv[0])->method)->function;
			const char * methodName = func->name ? func->name->chars : "<unnamed>";
			const char * typeName = IS_CLASS(AS_BOUND_METHOD(argv[0])->receiver) ? AS_CLASS(AS_BOUND_METHOD(argv[0])->receiver)->name->chars : krk_typeName(AS_BOUND_METHOD(argv[0])->receiver);
			size_t allocSize = strlen(methodName) + strlen(typeName) + 2;
			char * tmp = malloc(allocSize);
			snprintf(tmp, allocSize, "%s.%s", typeName, methodName);
			krk_disassembleCodeObject(stdout, func, tmp);
			free(tmp);
		} else {
			krk_runtimeError(vm.exceptions->typeError, "Can not disassemble built-in method of '%T'", AS_BOUND_METHOD(argv[0])->receiver);
		}
	} else if (IS_CLASS(argv[0])) {
		KrkValue code;
		if (krk_tableGet(&AS_CLASS(argv[0])->methods, OBJECT_VAL(S("__func__")), &code) && IS_CLOSURE(code)) {
			KrkCodeObject * func = AS_CLOSURE(code)->function;
			krk_disassembleCodeObject(stdout, func, AS_CLASS(argv[0])->name->chars);
		}
		/* TODO Methods! */
	} else {
		krk_runtimeError(vm.exceptions->typeError, "Don't know how to disassemble '%T'", argv[0]);
	}

	return NONE_VAL();
}

KRK_Function(build) {
	FUNCTION_TAKES_AT_LEAST(1);
	FUNCTION_TAKES_AT_MOST(2);
	CHECK_ARG(0,str,KrkString*,code);
	char * fileName = "<source>";
	if (argc > 1) {
		CHECK_ARG(1,str,KrkString*,filename);
		fileName = filename->chars;
	}

	/* Unset module */
	krk_push(OBJECT_VAL(krk_currentThread.module));
	KrkInstance * module = krk_currentThread.module;
	krk_currentThread.module = NULL;
	KrkCodeObject * c = krk_compile(code->chars,fileName);
	krk_currentThread.module = module;
	krk_pop();
	if (c) return OBJECT_VAL(c);
	else return NONE_VAL();
}

#define NOOP (void)0
#define SIMPLE(opc) case opc: size = 1; break;
#define CONSTANT(opc,more) case opc: { constant = chunk->code[offset + 1]; size = 2; more; break; } \
	case opc ## _LONG: { constant = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); size = 4; more; break; }
#define OPERAND(opc,more) case opc: { operand = chunk->code[offset + 1]; size = 2; more; break; } \
	case opc ## _LONG: { operand = (chunk->code[offset + 1] << 16) | \
	(chunk->code[offset + 2] << 8) | (chunk->code[offset + 3]); size = 4; more; break; }
#define JUMP(opc,sign) case opc: { jump = 0 sign ((chunk->code[offset + 1] << 8) | (chunk->code[offset + 2])); \
	size = 3; break; }
#define CLOSURE_MORE \
	KrkCodeObject * function = AS_codeobject(chunk->constants.values[constant]); \
	size_t baseOffset = offset; \
	for (size_t j = 0; j < function->upvalueCount; ++j) { \
		int isLocal = chunk->code[baseOffset++ + size]; \
		baseOffset++; \
		if (isLocal & 2) { \
			baseOffset += 2; \
		} \
	} \
	size += baseOffset - offset;
#define EXPAND_ARGS_MORE
#define FORMAT_VALUE_MORE
#define LOCAL_MORE local = operand;
static KrkValue _examineInternal(KrkCodeObject* func) {
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
			newTuple->values.values[newTuple->values.count++] = INTEGER_VAL(operand); /* Just in case */
			for (size_t i = 0; i < func->localNameCount; ++i) {
				if (func->localNames[i].id == (size_t)local && func->localNames[i].birthday <= offset && func->localNames[i].deathday >= offset) {
					newTuple->values.values[newTuple->values.count-1] = OBJECT_VAL(func->localNames[i].name);
					break;
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
KRK_Function(examine) {
	FUNCTION_TAKES_EXACTLY(1);
	CHECK_ARG(0,codeobject,KrkCodeObject*,func);
	return _examineInternal(func);
}

#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP
#undef CLOSURE_MORE
#undef LOCAL_MORE
#undef EXPAND_ARGS_MORE
#undef FORMAT_VALUE_MORE

#endif

KrkValue krk_module_onload_dis(KrkString * runAs) {
#ifndef KRK_DISABLE_DEBUG
	KrkInstance * module = krk_newInstance(vm.baseClasses->moduleClass);
	krk_push(OBJECT_VAL(module));

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
#include "opcodes.h"
#undef SIMPLE
#undef OPERANDB
#undef OPERAND
#undef CONSTANT
#undef JUMP

	if (runAs && !strcmp(runAs->chars,"__main__")) {
		/* Force `dis` into the module table early */
		krk_attachNamedObject(&vm.modules, "dis", (KrkObj*)module);
		/* Start executing additional code */
		krk_startModule("_dis");
		krk_interpret(
			"import dis\n"
			"print(dir(dis))\n"
			"def disrec(code, seen):\n"
			"    let next = [code]\n"
			"    while next:\n"
			"        let co = next[0]\n"
			"        next = next[1:]\n"
			"        dis.dis(co)\n"
			"        for inst,size,operand in dis.examine(co):\n"
			"            if isinstance(operand,codeobject) and operand not in seen and operand not in next:\n"
			"                next.append(operand)\n"
			"        if next:\n"
			"            print()\n"
			"import kuroko\n"
			"if (len(kuroko.argv) < 2):\n"
			"    print(\"Usage: kuroko -m dis FILE\")\n"
			"    return 1\n"
			"import fileio\n"
			"for file in kuroko.argv[1:]:\n"
			"    with fileio.open(file,'r') as f:\n"
			"        let result = dis.build(f.read(), file)\n"
			"        disrec(result,set())\n",
			"_dis"
		);
	}

	return krk_pop();
#else
	return krk_runtimeError(vm.exceptions->notImplementedError, "debugger support is disabled");
#endif
}
