# <img src="https://kuroko-lang.github.io/logo.png" align="bottom" height="24"> Kuroko

**Kuroko** is a dynamic, bytecode-compiled programming language and a [dialect](#python-compatibility) of Python. The syntax features indentation-driven blocks, familiar keywords, and explicit variable declaration with block scoping. The runtime interpreter includes a tracing garbage collector, multithreading support without a global lock, and support for single-step debugging and bytecode disassembly. The full interpreter and compiler can be built on Unix-like platforms to a shared library of around 500K and is easy to embed and extend with a [clean C API](https://kuroko-lang.github.io/docs/embedding.html) and limited libc footprint. Kuroko has been successfully built for a wide range of targets, including Linux, [ToaruOS](https://github.com/klange/toaruos), WebAssembly, macOS (including M1 ARM devices), and Windows (with mingw64).

## Build Kuroko

On most platforms, `make` is sufficient to build in the standard configuration which will produce both REPL binary (`kuroko`) with the compiler and interpreter included, as well as both a static (`libkuroko.a`) and shared library version (`libkuroko.so`) that can be used for embedding.

Additional build configurations are available with the following options:

- `KRK_DISABLE_RLINE=1`: Do not build with support for the rich syntax-highlighted line editor.
- `KRK_DISABLE_DEBUG=1`: Do not build support for disassembly. Not recommended, as it does not offer any visible improvement in performance.
- `KRK_DISABLE_DOCS=1`: Do not include documentation strings for builtins. Can reduce the library size by around 100KB depending on other configuration options.

### Windows

To build for Windows, it is recommended that a Unix-like host environment be used with the MingW64 toolchain:

```sh
CC=x86_64-w64-mingw32-gcc make
```

### WASM

WASM builds can be built from [kuroko-lang/kuroko-wasm-repl](https://github.com/kuroko-lang/kuroko-wasm-repl).

Kuroko can be built with the Asyncify option and as a worker.

### Android

For Android ARM64 targets with MTE (memory tagging extension), specify `KRK_HEAP_TAG_BYTE=0xb4` when building. Kuroko has not been tested with hardware-enforced memory tagging.

### Fully Static

Normally, the main interpreter binary statically links with the VM library, but is otherwise built as a dynamic executable and links to shared libraries for libc, pthreads, and so on. To build a fully static binary, adding `-static` to `CFLAGS` and building only the `kuroko` target should suffice.

Whether a static build supports importing C extension modules depends on the specifics of your target platform.

## Extend and Embed Kuroko

Kuroko is easy to embed in a host application or extend with C modules. Please see [the documentation on our website](https://kuroko-lang.github.io/docs/embedding.html) for further information.

## Learn Kuroko

If you already know Python, adapting to Kuroko is a breeze.

If you want to get started, [try the interactive tutorial](https://kuroko-lang.github.io/?r=y&c=tutorial()).

## Supported Functionality

Kuroko supports a wide range of functionality and syntax expected from a Python implementation, and this list is by no means exhaustive:

- Iteration loops with `for`
- `list`, `dict` and `set` comprehensions with chained `for` expressions and `if` conditions.
- Generator expressions and generator functions with `yield` and `yield from`.
- Context managers and `with`.
- Exceptions and `try`/`except`/`finally`.
- Complex assignment targets.
- Classes with inheritance, general attributes, methods (including static and class methods)
- Decorators, both for classes and functions.
- Type hints and other uses of function annotations.
- Unicode strings.
- Debugger hooks and instruction stepping.
- Multithreading.
- C extension modules.

## Python Compatibility

Kuroko aims for wide compatibility with Python 3.x and supports most syntax features and a growing collection of standard library functions. The most notable difference between Kuroko and standard Python is explicit variable declaration and the use of the `let` keyword. Many Python snippets can be ported to Kuroko with only the addition of declaration statements. Some syntax features remain unimplemented, however:

### `async for`, `async with`, `asyncio` module

Kuroko does not support the `async for`/`async with` constructs, and does not have a `asyncio` module. They are planned for a future release.

### Iterables

Kuroko iterables do not use the `__next__()` method, but rather are called normally. This allows iteration objects to be implemented as simple functions. If you are porting code which has a different use of `__call__()` than `__next__()` it will likely be necessary to change the implementation. Kuroko also doesn't have a `StopIteration` exception; iterators return themselves to signal they are exhausted (if you need an iterator to return itself, consider boxing it in a tuple).

### Inheritance

Kuroko provides only _single inheritance_. When porting from Python, mix-in classes and other uses of multiple inheritance may need to be redesigned.

### Global/Nonlocal

The scoping system in Kuroko makes the `global` and `nonlocal` keywords unnecessary and they are not supported.

### Walrus Operator

Kuroko has generalized assignment expressions, so skip the walrus and assign whatever you like (some additional parenthesis wrapping may be necessary to disambiguate assignments).
