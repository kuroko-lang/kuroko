# Statements {#lang_statements}

Statements control declaration, control flow, module interaction, and block structure.

## Simple Statements

Simple statement forms include:

- declaration and assignment
- expression statements
- `pass`
- `del`
- `assert`
- `return`, `yield`, and `raise`
- `import` and `from ... import ...`

Multiple simple statements may be separated with semicolons on one logical line.

## Block Statements

Indented suites follow header statements ending in `:`. Core block statements include:

- `if` / `elif` / `else`
- `while`
- `for`
- `try` / `except` / `else` / `finally`
- `with`
- `def` and `class`

Decorators apply to following function or class definitions and are evaluated from top to bottom before binding the resulting object.

## Function and Class Definitions

`def` introduces a new callable with lexical scope capture and optional annotations. `class` executes a class body to build the class namespace, then creates the class object (with optional single base class inheritance).

Async function definitions are supported through `async def`, with `await` expression support in function bodies.

## Control Transfer

`break` and `continue` affect the innermost active loop, while `return` exits the current function. In all cases, pending cleanup for active exception and context-manager blocks is preserved.

Exception statements use class-based matching in `except` clauses and can bind caught exceptions with `as`.

## Imports and Module Scope

Imports resolve modules through configured search paths and package metadata. `from module import *` is restricted to global module scope so name binding remains compile-time analyzable for local blocks.
