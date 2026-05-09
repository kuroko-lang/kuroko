# %Compiler Overview {#lang_compiler}

The Kuroko compiler transforms source text into bytecode executable by the virtual machine. The compilation pipeline is designed around a single-pass parser with explicit scope tracking and direct bytecode emission.

## Compilation Stages

1. **Scanning** converts source text into a stream of tokens.
2. **Parsing** consumes tokens according to expression and statement grammar rules.
3. **Code generation** emits bytecode instructions and constant table entries.
4. **Function finalization** packages code into callable code objects with metadata.

## Parser and Precedence

Expressions are parsed with precedence-directed parsing, allowing compact implementation of infix, prefix, and postfix operators while preserving expected Python-like operator behavior.

The parser distinguishes between contexts where assignment is legal and where only value expressions are valid, enabling generalized assignment targets while preserving clear syntax errors for invalid targets.

## Scope and Name Resolution

Kuroko uses lexical scoping with explicit declaration. During compilation, names are classified as:

- local variables in the current scope
- captured upvalues from parent scopes
- module globals

This classification determines which bytecode instructions are emitted for loads, stores, and deletes.

## Blocks and Control Flow

Control-flow statements emit jumps with deferred patching. The compiler records jump sites and backpatches offsets when block boundaries become known.

Loops and exception blocks also track cleanup actions for local values and captured references, ensuring that scope exit behavior is correct for normal control flow as well as `break`, `continue`, `return`, and exceptions.

## Functions and Classes

Function definitions produce nested code objects with parameter metadata (including defaults and collectors). Class definitions compile to runtime class-construction sequences and then attach methods and attributes.

Docstrings, annotations, and decorators are compiled as ordinary runtime objects and operations, preserving dynamic behavior while keeping syntax concise.
