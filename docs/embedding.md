## Kuroko C API Documentation {#embedding}

This documentation covers using Kuroko 1.1 as a scripting language in a host C application and assumes a relatively recent Linux or similarly POSIX-y build environment.

We'll also assume you've already installed Kuroko from a package that includes source headers, such as [the PPA](https://launchpad.net/~k-lange/+archive/ubuntu/kuroko/).

### Getting Started {#getting-started}

```c
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>

int main(int argc, char *argv[]) {
    krk_initVM(0);
    krk_startModule("__main__");
    krk_interpret("print('hello, world')", "<stdin>");
    krk_freeVM();
    return 0;
}
```

Let's start by looking at the example code above.

#### Headers

Kuroko's public API is exposed primarily through the `<kuroko/kuroko.h>` header.

`<kuroko/vm.h>` provides internal functions for finer control over how the VM operates.

@bsnote{warning,Headers are in the process of being reorganized and the above statements are not necessarily true _yet_.}

Other headers provide convenience functions and macros for building C extensions.

#### Initializing the VM

Our first task in integrating Kuroko into an embedded application is to set up the VM, which we do with `krk_initVM()`. This function takes one paramater representing the initial _global and thread flags_, which control debugging features for tracing. In our example code, we do not need to use to use any debug features so we pass `0`.

Valid flags to pass to `krk_initVM()` include:

- `KRK_THREAD_ENABLE_TRACING`  
Prints instruction traces during execution.
- `KRK_THREAD_ENABLE_DISASSEMBLY`  
Prints function bytecode disassembly whenever the compiler is called.
- `KRK_THREAD_ENABLE_SCAN_TRACING`  
Prints token stream data whenever the compiler is called. (Not recommended.)
- `KRK_THREAD_SINGLE_STEP`  
Halts execution and calls the debugger before every instruction.
- `KRK_GLOBAL_REPORT_GC_COLLECTS`  
Prints a message each time the garbage collector is run.
- `KRK_GLOBAL_ENABLE_STRESS_GC`  
Causes the garbage collector to be called on every allocation (from the main thread).
- `KRK_GLOBAL_CALLGRIND`  
Generate tracing data. `vm.callgrindFile` must be set to a writable stream to store the intermediate data collected by the VM.
- `KRK_GLOBAL_CLEAN_OUTPUT`  
Disables automatic printing of uncaught exception tracebacks. Use `krk_dumpTraceback()` to print a traceback from the exception in the current thread to `stderr`.

@bsnote{Be careful when using `KRK_GLOBAL_CLEAN_OUTPUT` when threading is available; uncaught exceptions from threads will not be automatically printed and are trickier to catch from C code.}

#### Starting a Module

All Kuroko code runs in the context of a _module_. When we run code directly using `krk_interpret()`, such as when providing a REPL or calling snippets, or by using `krk_runfile()` to execute the contents of a file, we need to establish our own module first. `krk_startModule()` creates a module context and gives it a name. We use the name `__main__` as a convention for directly executed code, distinguishing it from imported code.

#### Calling the Interpreter

We pass C strings containg Kuroko code to `krk_interpret()` to be run by the interpreter. The second argument to `krk_interpret()` provides a filename for the source of the code, or representative string to show in tracebacks for code that did not come from a file.

Next up, we'll look at @ref c_functions.
