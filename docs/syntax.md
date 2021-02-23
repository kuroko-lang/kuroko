# Syntax Reference {#syntax}

@bsnote{This is a work-in-progress based on the Lua reference manual.}

### Introduction

Kuroko is a dynamic, bytecode-compiled, garbage-collected, embeddable, extensible, modular programming language in the same family as Python. Kuroko combines a familiar indentation-driven syntax with more traditional block-based variable declaration and scoping rules and adds to that a GIL-free concurrent multhreading model, all without sacrificing the high-level constructs and flexibility of its siblings.

As Kuroko was initially designed for use as an extension language, setting up the interpreter and running code in a hosted environment is quick and simple, and restricting the operations available to managed code is straightforward. Kuroko is also intended to be a featureful standalone language, and includes an ever-growing (but altogether optional) standard library.

### Concepts

#### Values, Objects, and Classes

All operations in Kuroko are structured around _objects_. Every representable thing, from integers to strings to functions and classes, is an _object_, and all objects have an associated _type_. While Kuroko is _dynamic_, this type is an inherent property of each object that can be examined at runtime.

Kuroko has several kinds of _built-in types_. Simple objects, including integers, floating-point numbers, boolean `True` and `False`, are known as _values_. Values are lightweight and ephemeral, passed around the interpreter through copying. Values are also immutable, never changing on their own but always through reassignment.

One step up from values are first-level objects. First-level objects live in the heap and are managed by the garbage collector. The garbage collector will eventually dispose of objects if no accessible _references_ to them remain. References are another kind of value, and can be thought of like pointers. The first-level objects represent the core types the interpreter uses to implement necessary functionality and include functions, strings, classes, bound methods, and a few others.

Finally, there are _instances_. Instances represent objects of user-defined types.

