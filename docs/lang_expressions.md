# Expressions {#lang_expressions}

Expressions produce values and may also perform side effects through calls, assignments, comprehensions, and attribute or index operations.

## Primary Expressions

Primary expression forms include:

- literals (`None`, booleans, numbers, strings)
- grouped expressions in parentheses
- list, tuple, dict, and set displays
- name references
- attribute access and indexing
- function and method calls

Primary expressions can be chained (for example, call results followed by attribute access).

## Operator Families

Kuroko supports Python-like unary and binary operators, including:

- arithmetic (`+`, `-`, `*`, `/`, `//`, `%`, `**`, `@`)
- bitwise (`|`, `^`, `&`, shifts)
- comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`, `is`, `in`)
- boolean (`not`, `and`, `or`)

Comparison operators support chaining semantics.

## Assignment Expressions and Targets

Assignment is expression-based and can appear in places where plain expressions are allowed, subject to parser disambiguation rules.

Valid assignment targets include names, attributes, indexes, unpacking patterns, and selected nested forms.

Augmented assignment operators (such as `+=` and `*=`) are available for mutable and user-defined types implementing the corresponding operations.

## Comprehensions and Generators

List, dict, and set comprehensions evaluate iteration clauses and filter clauses in order, with each comprehension creating its own local scope behavior consistent with Kuroko's closure model.

Generator expressions produce lazy iterators, and generator functions use `yield` and `yield from` for cooperative value production.

## Calls and Argument Passing

Call expressions support positional arguments, keyword arguments, iterable expansion, and mapping expansion. Runtime argument binding validates arity and names against callable metadata.
