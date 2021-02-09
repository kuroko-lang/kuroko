![logo](.github/kuroko.png)
# Kuroko - A bytecode-compiled scripting language

Kuroko is a bytecode-compiled, dynamic, interpreted programming language with familiar Python-like syntax and a small, embeddable core

The bytecode VM / compiler is substantially based on Robert Nystrom's [_Crafting Interpreters_](https://craftinginterpreters.com/).

This project was originally started to add a proper scripting language to [Bim](https://github.com/klange/bim) to support syntax highlighting and plugins, as well as to give [ToaruOS](https://github.com/klange/toaruos) a general-purpose user language for applications and utilities.

## Features

Kuroko inherits some core features by virtue of following _Crafting Interpreters_, including its basic type system, classes/methods/functions, and the design of its compiler and bytecode VM.

On top of this, Kuroko adds a number of features inspired by Python, such as:

- Indentation-based block syntax.
- Collection types: `list`, `dict`, `tuple`, `set`, with compiler literal syntax (`[]`,`{}`,`(,)`).
- List, dict, tuple, and set comprehensions (`[foo(x) for x in [1,2,3,4]]` and similar expressions).
- Iterable types, with `for ... in ...` syntax.
- Class methods for basic types (eg. strings are instances of a `str` class providing methods like `.format()`)
- Exception handling, with `try`/`except`/`raise`.
- Modules, both for native C code and managed Kuroko code.
- Unicode strings and identifiers.

## Building Kuroko

### Building as a Shared Library

Kuroko has a minimal set of C standard library dependencies as well as optional dependencies on `ldl` (for runtime C extension loading) and `lpthread` (for threads).

When built against `rline` (the rich line editing library that provides tab completion and syntax highlighting for the REPL), additional termios-related functions are required; `rline` will only be built into the interpreter binary and not the shared library.

Generally, `make` should suffice to build from the repository and `sudo make install` should work Debian-style multiarch environments, but check whether the default `libdir` and so on are appropriate for your platform.

Source files are found in `src`; those beginning with `module_` are normally built as a separate C extension modules and may have different requirements from the rest of the VM.

### Building as a Single Static Binary

Configuration options are available in the Makefile to build Kuroko as a static binary.

    make clean; make KRK_ENABLE_STATIC=1

This will produce a static binary without `dlopen` support, so it will not be able to load additional C modules at runtime.

The standard set of C modules can be bundled into the interpreter, whether building statically or normally:

    make clean; make KRK_ENABLE_BUNDLE=1

Additional options include `KRK_DISABLE_RLINE=1` to not link with the included rich line editing library (will lose tab completion and syntax highlighting in the repl) and `KRK_DISABLE_DEBUG=1` to disable debugging features (which has not been demonstrated to provide any meaningful performance improvement when the VM is built with optimizations enabled).

### Building for WASM

See [klange/kuroko-wasm-repl](https://github.com/klange/kuroko-wasm-repl) for information on building Kuroko with Emscripten for use in a web browser.

### Building for Windows

Experimental support is available for building Kuroko to run on Windows using MingW:

    CC=x86_64-w64-mingw32-gcc make

A capable terminal, such as Windows Terminal, is required to run the interpreter's REPL correctly; CMD.exe has also been tested successfully.

## Code Samples

Please see [the wiki](https://github.com/kuroko-lang/kuroko/wiki/Samples) for examples of Kuroko code, or follow [the interactive tutorial](https://kuroko-lang.github.io/?r=y&c=tutorial%28%29#).

## About the REPL

Kuroko's repl provides an interactive environment for executing code and seeing results.

When entering code at the repl, lines ending with colons (`:`) are treated specially - the repl will continue to accept input and automatically insert indentation on a new line. Please note that the repl's understanding of colons is naive: Whitespace or comments after a colon which would normally be accepted by Kuroko's parser will not be understood by the repl - if you want to place a comment after the start of a block statement, be sure that it ends in a colon so you can continue to enter statements.

Pressing backspace when the cursor is preceded by whitespace will delete up to the last column divisible by 4, which should generally delete one level of indentation automatically.

The tab key will also produce spaces when the cursor is at the beginning of the line or preceded entirely with white space.

The repl will display indentation level indicators in preceding whitespace as a helpful guide.

When a blank line or a line consisting entirely of whitespace is entered, the repl will process the full input.

Code executed in the repl runs in a global scope and reused variable names will overwrite previous definitions, allowing function and class names to be reused.

The repl will display the last value popped from the stack before returning.

Tab completion will provide the names of globals, as well as the fields and methods of objects.

## What's different from Python?

You may be looking at the code examples and thinking Kuroko looks a _lot_ more like Python than "syntax similar to Python" suggests. Still, there are some differences, and they come in two forms: Intentional differences and unintentional differences.

Unintentional differences likely represent incomplete features. Intentional differences are design decisions specifically meant to differentiate Kuroko from Python and usually are an attempt to improve upon or "fix" perceived mistakes.

Two notable intentional differences thus far are:

- Kuroko's variable scoping requires explicit declarations. This was done because Python's function-level scoping, and particularly how it interacts with globals, is often a thorn in the side of beginner and seasoned programmers alike. It's not so much seen as a mistake as it is something we don't wish to replicate.
- Default arguments to functions are evaluated at call time, not at definition time. How many times have you accidentally assigned an empty list as a default argument, only to be burned by its mutated descendent appearing in further calls? Kuroko doesn't do that - it works more like Ruby.

## Interfacing C with Kuroko

Please see [the wiki](https://github.com/kuroko-lang/kuroko/wiki/Embedding) for detailed instructions on embedded Kuroko or building new C extensions.

