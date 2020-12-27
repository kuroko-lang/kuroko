#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "vm.h"
#include "debug.h"
#include "memory.h"
#include "compiler.h"
#include "object.h"
#include "table.h"

/* Why is this static... why do we do this to ourselves... */
KrkVM vm;

static void resetStack() {
	vm.stackTop = vm.stack;
	vm.frameCount = 0;
	vm.openUpvalues = NULL;
}

static void runtimeError(const char * fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");

	for (int i = vm.frameCount - 1; i >= 0; i--) {
		CallFrame * frame = &vm.frames[i];
		KrkFunction * function = frame->closure->function;
		size_t instruction = frame->ip - function->chunk.code - 1;
		fprintf(stderr, "[line %d] in ", (int)function->chunk.lines[instruction]);
		if (function->name == NULL) {
			fprintf(stderr, "module\n");
		} else {
			fprintf(stderr, "%s()\n", function->name->chars);
		}
	}

	resetStack();
}

void krk_push(KrkValue value) {
	if ((size_t)(vm.stackTop - vm.stack) + 1 > vm.stackSize) {
		size_t old = vm.stackSize;
		size_t old_offset = vm.stackTop - vm.stack;
		vm.stackSize = GROW_CAPACITY(old);
		vm.stack = GROW_ARRAY(KrkValue, vm.stack, old, vm.stackSize);
		vm.stackTop = vm.stack + old_offset;
	}
	*vm.stackTop = value;
	vm.stackTop++;
}

KrkValue krk_pop() {
	vm.stackTop--;
	if (vm.stackTop < vm.stack) {
		fprintf(stderr, "stack overflow - too many pops\n");
		return NONE_VAL();
	}
	return *vm.stackTop;
}

KrkValue krk_peek(int distance) {
	return vm.stackTop[-1 - distance];
}

static void defineNative(const char * name, NativeFn function) {
	krk_push(OBJECT_VAL(copyString(name, (int)strlen(name))));
	krk_push(OBJECT_VAL(newNative(function)));
	krk_tableSet(&vm.globals, vm.stack[0], vm.stack[1]);
	krk_pop();
	krk_pop();
}

static KrkValue krk_sleep(int argc, KrkValue argv[]) {
	if (argc < 1) {
		runtimeError("sleep: expect at least one argument.");
		return BOOLEAN_VAL(0);
	}

	/* Accept an integer or a floating point. Anything else, just ignore. */
	unsigned int usecs = (IS_INTEGER(argv[0]) ? AS_INTEGER(argv[0]) :
	                      (IS_FLOATING(argv[0]) ? AS_FLOATING(argv[0]) : 0)) *
	                      1000000;

	usleep(usecs);

	return BOOLEAN_VAL(1);
}

static int call(KrkClosure * closure, int argCount) {
	if (argCount != closure->function->arity) {
		runtimeError("Wrong number of arguments (%d expected, got %d)", closure->function->arity, argCount);
		return 0;
	}
	if (vm.frameCount == FRAMES_MAX) {
		runtimeError("Too many call frames.");
		return 0;
	}
	CallFrame * frame = &vm.frames[vm.frameCount++];
	frame->closure = closure;
	frame->ip = closure->function->chunk.code;
	frame->slots = vm.stackTop - argCount - 1;
	return 1;
}

static int callValue(KrkValue callee, int argCount) {
	if (IS_OBJECT(callee)) {
		switch (OBJECT_TYPE(callee)) {
			case OBJ_CLOSURE:
				return call(AS_CLOSURE(callee), argCount);
			case OBJ_NATIVE: {
				NativeFn native = AS_NATIVE(callee);
				KrkValue result = native(argCount, vm.stackTop - argCount);
				vm.stackTop -= argCount + 1;
				krk_push(result);
				return 1;
			}
			case OBJ_CLASS: {
				KrkClass * _class = AS_CLASS(callee);
				vm.stackTop[-argCount - 1] = OBJECT_VAL(newInstance(_class));
				KrkValue initializer;
				if (krk_tableGet(&_class->methods, OBJECT_VAL(vm.__init__), &initializer)) {
					return call(AS_CLOSURE(initializer), argCount);
				} else if (argCount != 0) {
					runtimeError("Class does not have an __init__ but arguments were passed to initializer: %d\n", argCount);
					return 0;
				}
				return 1;
			}
			case OBJ_BOUND_METHOD: {
				KrkBoundMethod * bound = AS_BOUND_METHOD(callee);
				vm.stackTop[-argCount - 1] = bound->receiver;
				return call(bound->method, argCount);
			}
			default:
				break;
		}
	}
	runtimeError("Attempted to call non-callable type.");
	return 0;
}

static int bindMethod(KrkClass * _class, KrkString * name) {
	KrkValue method;
	if (!krk_tableGet(&_class->methods, OBJECT_VAL(name), &method)) return 0;
	KrkBoundMethod * bound = newBoundMethod(krk_peek(0), AS_CLOSURE(method));
	krk_pop();
	krk_push(OBJECT_VAL(bound));
	return 1;
}

static KrkUpvalue * captureUpvalue(KrkValue * local) {
	KrkUpvalue * prevUpvalue = NULL;
	KrkUpvalue * upvalue = vm.openUpvalues;
	while (upvalue != NULL && upvalue->location > local) {
		prevUpvalue = upvalue;
		upvalue = upvalue->next;
	}
	if (upvalue && upvalue->location == local) {
		return upvalue;
	}
	KrkUpvalue * createdUpvalue = newUpvalue(local);
	createdUpvalue->next = upvalue;
	if (prevUpvalue == NULL) {
		vm.openUpvalues = createdUpvalue;
	} else {
		prevUpvalue->next = createdUpvalue;
	}
	return createdUpvalue;
}

static void closeUpvalues(KrkValue * last) {
	while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
		KrkUpvalue * upvalue = vm.openUpvalues;
		upvalue->closed = *upvalue->location;
		upvalue->location = &upvalue->closed;
		vm.openUpvalues = upvalue->next;
	}
}

static void defineMethod(KrkString * name) {
	KrkValue method = krk_peek(0);
	KrkClass * _class = AS_CLASS(krk_peek(1));
	krk_tableSet(&_class->methods, OBJECT_VAL(name), method);
	krk_pop();
}

void krk_initVM() {
	resetStack();
	vm.objects = NULL;
	vm.bytesAllocated = 0;
	vm.nextGC = 1024 * 1024;
	vm.grayCount = 0;
	vm.grayCapacity = 0;
	vm.grayStack = NULL;
	krk_initTable(&vm.globals);
	krk_initTable(&vm.strings);
	vm.__init__ = NULL;
	vm.__init__ = copyString("__init__", 8);
	defineNative("__krk_builtin_sleep", krk_sleep);
}

void krk_freeVM() {
	krk_freeTable(&vm.globals);
	krk_freeTable(&vm.strings);
	vm.__init__ = NULL;
	krk_freeObjects();
	FREE_ARRAY(size_t, vm.stack, vm.stackSize);
}

static int isFalsey(KrkValue value) {
	return IS_NONE(value) || (IS_BOOLEAN(value) && !AS_BOOLEAN(value)) ||
	       (IS_INTEGER(value) && !AS_INTEGER(value));
	/* Objects in the future: */
	/* IS_STRING && length == 0; IS_ARRAY && length == 0; IS_INSTANCE && __bool__ returns 0... */
}

const char * typeName(KrkValue value) {
	if (value.type == VAL_BOOLEAN) return "Boolean";
	if (value.type == VAL_NONE) return "None";
	if (value.type == VAL_INTEGER) return "Integer";
	if (value.type == VAL_FLOATING) return "Floating";
	if (value.type == VAL_OBJECT) {
		if (IS_STRING(value)) return "String";
		if (IS_FUNCTION(value)) return "Function";
		if (IS_NATIVE(value)) return "Native";
		if (IS_CLOSURE(value)) return "Closure";
		return "(Unspecified Object)";
	}
	return "???";
}

#define MAKE_BIN_OP(name,operator) \
	static KrkValue name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return INTEGER_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return FLOATING_VAL(AS_FLOATING(a) operator (double)AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return FLOATING_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return FLOATING_VAL((double)AS_INTEGER(a) operator AS_FLOATING(b)); \
		} \
		runtimeError("Incompatible types for binary operand %s: %s and %s", #operator, typeName(a), typeName(b)); \
		return NONE_VAL(); \
	}

MAKE_BIN_OP(add,+)
MAKE_BIN_OP(subtract,-)
MAKE_BIN_OP(multiply,*)
MAKE_BIN_OP(divide,/)

#define MAKE_COMPARATOR(name, operator) \
	static KrkValue name (KrkValue a, KrkValue b) { \
		if (IS_INTEGER(a) && IS_INTEGER(b)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		if (IS_FLOATING(a)) { \
			if (IS_INTEGER(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_INTEGER(b)); \
			else if (IS_FLOATING(b)) return BOOLEAN_VAL(AS_FLOATING(a) operator AS_FLOATING(b)); \
		} else if (IS_FLOATING(b)) { \
			if (IS_INTEGER(a)) return BOOLEAN_VAL(AS_INTEGER(a) operator AS_INTEGER(b)); \
		} \
		runtimeError("Can not compare types %s and %s", typeName(a), typeName(b)); \
		return NONE_VAL(); \
	}

MAKE_COMPARATOR(less, <)
MAKE_COMPARATOR(greater, >)

static void concatenate(const char * a, const char * b, size_t al, size_t bl) {
	size_t length = al + bl;
	char * chars = ALLOCATE(char, length + 1);
	memcpy(chars, a, al);
	memcpy(chars + al, b, bl);
	chars[length] = '\0';

	KrkString * result = takeString(chars, length);
	krk_pop();
	krk_pop();
	krk_push(OBJECT_VAL(result));
}

static void addObjects() {
	KrkValue _b = krk_peek(0);
	KrkValue _a = krk_peek(1);

	if (IS_STRING(_a)) {
		KrkString * a = AS_STRING(_a);
		if (IS_STRING(_b)) {
			KrkString * b = AS_STRING(_b);
			concatenate(a->chars,b->chars,a->length,b->length);
			return;
		}
		char tmp[256];
		if (IS_INTEGER(_b)) {
			sprintf(tmp, "%d", AS_INTEGER(_b));
		} else if (IS_FLOATING(_b)) {
			sprintf(tmp, "%g", AS_FLOATING(_b));
		} else if (IS_BOOLEAN(_b)) {
			sprintf(tmp, "%s", AS_BOOLEAN(_b) ? "True" : "False");
		} else if (IS_NONE(_b)) {
			sprintf(tmp, "None");
		} else {
			sprintf(tmp, "<Object>");
		}
		concatenate(a->chars,tmp,a->length,strlen(tmp));
	} else {
		runtimeError("Can not concatenate types %s and %s", typeName(_a), typeName(_b)); \
		krk_pop();
		krk_pop();
		krk_push(NONE_VAL());
	}
}

#define READ_BYTE() (*frame->ip++)
#define BINARY_OP(op) { KrkValue b = krk_pop(); KrkValue a = krk_pop(); krk_push(op(a,b)); break; }
#define READ_CONSTANT(s) (frame->closure->function->chunk.constants.values[readBytes(frame,s)])
#define READ_STRING(s) AS_STRING(READ_CONSTANT(s))

static size_t readBytes(CallFrame * frame, int num) {
	if (num == 1) return READ_BYTE();
	else if (num == 2) {
		unsigned int top = READ_BYTE();
		unsigned int bot = READ_BYTE();
		return (top << 8) | (bot);
	} else if (num == 3) {
		unsigned int top = READ_BYTE();
		unsigned int mid = READ_BYTE();
		unsigned int bot = READ_BYTE();
		return (top << 16) | (mid << 8) | (bot);
	}

	runtimeError("Invalid byte read?");
	return (size_t)-1;
}

static KrkValue run() {
	CallFrame* frame = &vm.frames[vm.frameCount - 1];

	for (;;) {
#undef DEBUG
#ifdef DEBUG
		fprintf(stderr, "        | ");
		for (KrkValue * slot = vm.stack; slot < vm.stackTop; slot++) {
			fprintf(stderr, "[ ");
			krk_printValue(stderr, *slot);
			fprintf(stderr, " ]");
		}
		fprintf(stderr, "\n");
		krk_disassembleInstruction(&frame->closure->function->chunk,
			(size_t)(frame->ip - frame->closure->function->chunk.code));
#endif
		uint8_t opcode;
		switch ((opcode = READ_BYTE())) {
			case OP_PRINT: {
				krk_printValue(stdout, krk_pop());
				fprintf(stdout, "\n");
				break;
			}
			case OP_RETURN: {
				KrkValue result = krk_pop();
				closeUpvalues(frame->slots);
				vm.frameCount--;
				if (vm.frameCount == 0) {
					return result;
				}
				vm.stackTop = frame->slots;
				if (vm.frameCount == vm.exitOnFrame) {
					return result;
				}
				krk_push(result);
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			case OP_EQUAL: {
				KrkValue b = krk_pop();
				KrkValue a = krk_pop();
				krk_push(BOOLEAN_VAL(krk_valuesEqual(a,b)));
				break;
			}
			case OP_LESS: BINARY_OP(less);
			case OP_GREATER: BINARY_OP(greater)
			case OP_ADD:
				if (IS_OBJECT(krk_peek(0)) || IS_OBJECT(krk_peek(1))) addObjects();
				else BINARY_OP(add)
				break;
			case OP_SUBTRACT: BINARY_OP(subtract)
			case OP_MULTIPLY: BINARY_OP(multiply)
			case OP_DIVIDE: BINARY_OP(divide)
			case OP_NEGATE: {
				KrkValue value = krk_pop();
				if (IS_INTEGER(value)) krk_push(INTEGER_VAL(-AS_INTEGER(value)));
				else if (IS_FLOATING(value)) krk_push(FLOATING_VAL(-AS_FLOATING(value)));
				else { runtimeError("Incompatible operand type for prefix negation."); return NONE_VAL(); }
				break;
			}
			case OP_CONSTANT_LONG:
			case OP_CONSTANT: {
				size_t index = readBytes(frame, opcode == OP_CONSTANT ? 1 : 3);
				KrkValue constant = frame->closure->function->chunk.constants.values[index];
				krk_push(constant);
				break;
			}
			case OP_NONE:  krk_push(NONE_VAL()); break;
			case OP_TRUE:  krk_push(BOOLEAN_VAL(1)); break;
			case OP_FALSE: krk_push(BOOLEAN_VAL(0)); break;
			case OP_NOT:   krk_push(BOOLEAN_VAL(isFalsey(krk_pop()))); break;
			case OP_POP:   krk_pop(); break;
			case OP_DEFINE_GLOBAL_LONG:
			case OP_DEFINE_GLOBAL: {
				KrkString * name = READ_STRING((opcode == OP_DEFINE_GLOBAL ? 1 : 3));
				krk_tableSet(&vm.globals, OBJECT_VAL(name), krk_peek(0));
				krk_pop();
				break;
			}
			case OP_GET_GLOBAL_LONG:
			case OP_GET_GLOBAL: {
				KrkString * name = READ_STRING((opcode == OP_GET_GLOBAL ? 1 : 3));
				KrkValue value;
				if (!krk_tableGet(&vm.globals, OBJECT_VAL(name), &value)) {
					runtimeError("Undefined variable '%s'.", name->chars);
					return NONE_VAL();
				}
				krk_push(value);
				break;
			}
			case OP_SET_GLOBAL_LONG:
			case OP_SET_GLOBAL: {
				KrkString * name = READ_STRING((opcode == OP_SET_GLOBAL ? 1 : 3));
				if (krk_tableSet(&vm.globals, OBJECT_VAL(name), krk_peek(0))) {
					krk_tableDelete(&vm.globals, OBJECT_VAL(name));
					/* TODO: This should probably just work as an assignment? */
					runtimeError("Undefined variable '%s'.", name->chars);
					return NONE_VAL();
				}
				break;
			}
			case OP_IMPORT_LONG:
			case OP_IMPORT: {
				KrkString * name = READ_STRING((opcode == OP_IMPORT ? 1 : 3));
				KrkValue module;
				if (!krk_tableGet(&vm.modules, OBJECT_VAL(name), &module)) {
					/* Try to open it */
					char tmp[256];
					sprintf(tmp, "%s.krk", name->chars);
					vm.exitOnFrame = vm.frameCount;
					module = krk_runfile(tmp,1);
					vm.exitOnFrame = -1;
					if (!IS_OBJECT(module)) {
						runtimeError("Failed to import module - expected to receive an object, but got a %s instead.", typeName(module));
					}
					krk_tableSet(&vm.modules, OBJECT_VAL(name), module);
				}
				krk_push(module);
				break;
			}
			case OP_GET_LOCAL_LONG:
			case OP_GET_LOCAL: {
				uint32_t slot = readBytes(frame, (opcode == OP_GET_LOCAL ? 1 : 3));
				krk_push(frame->slots[slot]);
				break;
			}
			case OP_SET_LOCAL_LONG:
			case OP_SET_LOCAL: {
				uint32_t slot = readBytes(frame, (opcode == OP_SET_LOCAL ? 1 : 3));
				frame->slots[slot] = krk_peek(0);
				break;
			}
			case OP_JUMP_IF_FALSE: {
				uint16_t offset = readBytes(frame, 2);
				if (isFalsey(krk_peek(0))) frame->ip += offset;
				break;
			}
			case OP_JUMP_IF_TRUE: {
				uint16_t offset = readBytes(frame, 2);
				if (!isFalsey(krk_peek(0))) frame->ip += offset;
				break;
			}
			case OP_JUMP: {
				frame->ip += readBytes(frame, 2);
				break;
			}
			case OP_LOOP: {
				uint16_t offset = readBytes(frame, 2);
				frame->ip -= offset;
				break;
			}
			case OP_CALL: {
				int argCount = READ_BYTE();
				if (!callValue(krk_peek(argCount), argCount)) {
					return NONE_VAL();
				}
				frame = &vm.frames[vm.frameCount - 1];
				break;
			}
			case OP_CLOSURE_LONG:
			case OP_CLOSURE: {
				KrkFunction * function = AS_FUNCTION(READ_CONSTANT((opcode == OP_CLOSURE ? 1 : 3)));
				KrkClosure * closure = newClosure(function);
				krk_push(OBJECT_VAL(closure));
				for (size_t i = 0; i < closure->upvalueCount; ++i) {
					int isLocal = READ_BYTE();
					int index = READ_BYTE();
					if (isLocal) {
						closure->upvalues[i] = captureUpvalue(frame->slots + index);
					} else {
						closure->upvalues[i] = frame->closure->upvalues[index];
					}
				}
				break;
			}
			case OP_GET_UPVALUE_LONG:
			case OP_GET_UPVALUE: {
				int slot = readBytes(frame, (opcode == OP_GET_UPVALUE) ? 1 : 3);
				krk_push(*frame->closure->upvalues[slot]->location);
				break;
			}
			case OP_SET_UPVALUE_LONG:
			case OP_SET_UPVALUE: {
				int slot = readBytes(frame, (opcode == OP_SET_UPVALUE) ? 1 : 3);
				*frame->closure->upvalues[slot]->location = krk_peek(0);
				break;
			}
			case OP_CLOSE_UPVALUE:
				closeUpvalues(vm.stackTop - 1);
				krk_pop();
				break;
			case OP_CLASS_LONG:
			case OP_CLASS: {
				KrkString * name = READ_STRING((opcode == OP_CLASS ? 1 : 3));
				krk_push(OBJECT_VAL(newClass(name)));
				break;
			}
			case OP_GET_PROPERTY_LONG:
			case OP_GET_PROPERTY: {
				if (!IS_INSTANCE(krk_peek(0))) {
					runtimeError("Don't know how to retreive properties for %s yet", typeName(krk_peek(0)));
					return NONE_VAL();
				}
				KrkInstance * instance = AS_INSTANCE(krk_peek(0));
				KrkString * name = READ_STRING((opcode == OP_GET_PROPERTY ? 1 : 3));
				KrkValue value;
				if (krk_tableGet(&instance->fields, OBJECT_VAL(name), &value)) {
					krk_pop();
					krk_push(value);
					break;
				}
				if (!bindMethod(instance->_class, name)) {
					runtimeError("Undefined property/method '%s'.", name->chars);
					return NONE_VAL();
				}
				break;
			}
			case OP_SET_PROPERTY_LONG:
			case OP_SET_PROPERTY: {
				if (!IS_INSTANCE(krk_peek(1))) {
					runtimeError("Don't know how to set properties for %s yet", typeName(krk_peek(1)));
					return NONE_VAL();
				}
				KrkInstance * instance = AS_INSTANCE(krk_peek(1));
				KrkString * name = READ_STRING((opcode == OP_SET_PROPERTY ? 1 : 3));
				krk_tableSet(&instance->fields, OBJECT_VAL(name), krk_peek(0));
				KrkValue value = krk_pop();
				krk_pop(); /* instance */
				krk_push(value); /* Moves value in */
				break;
			}
			case OP_METHOD_LONG:
			case OP_METHOD: {
				defineMethod(READ_STRING((opcode == OP_METHOD ? 1 : 3)));
				break;
			}
		}
	}

#undef BINARY_OP
#undef READ_BYTE
}

KrkValue krk_interpret(const char * src, int newScope) {
	KrkFunction * function = krk_compile(src, newScope);

	krk_push(OBJECT_VAL(function));
	KrkClosure * closure = newClosure(function);
	krk_pop();

	krk_push(OBJECT_VAL(closure));
	callValue(OBJECT_VAL(closure), 0);

	return run();
}


KrkValue krk_runfile(const char * fileName, int newScope) {
	FILE * f = fopen(fileName,"r");
	if (!f) return NONE_VAL();

	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);

	char * buf = malloc(size+1);
	fread(buf, 1, size, f);
	fclose(f);
	buf[size] = '\0';

	KrkValue result = krk_interpret(buf, newScope);
	free(buf);

	return result;
}

