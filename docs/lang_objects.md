# Object Model {#lang_objects}

Kuroko represents runtime values with a compact tagged value type and a managed heap for complex objects. This section summarizes the object categories exposed to user code and used by the VM.

## Values and Heap Objects

Primitive values such as integers, booleans, and `None` are represented directly as lightweight value slots.

Heap-allocated objects represent richer runtime entities, including:

- strings
- functions and closures
- classes and instances
- tuples, lists, dicts, and sets
- generators and iterators
- modules and native wrappers

Heap objects are reference-tracked by the runtime and reclaimed by the garbage collector when no reachable references remain.

## Classes and Instances

Every object has a class that defines behavior and method lookup. Instance attribute access follows the class hierarchy and descriptor rules, with method binding performed at access time.

Kuroko supports single inheritance. Class construction is dynamic: class bodies execute as code and produce the class namespace before final class objects are created.

## Callables

Several object kinds are callable:

- functions compiled from Kuroko source
- bound methods produced from method lookup
- classes (through instance construction)
- native C functions exposed to the runtime

Call semantics are uniform at the bytecode level, with argument unpacking and keyword handling performed through callable metadata.

## Identity, Equality, and Mutability

Identity compares object references. Equality is type-directed and may be overridden by user classes.

Mutable container types expose in-place updates, while immutable types (such as strings and tuples) produce new values when transformed.

## Iteration Model

Iteration operates through callable iterators and exhaustion signaling conventions in the VM. This differs from CPython's `__next__` exception protocol and enables lightweight iterator implementations for both native and user objects.
