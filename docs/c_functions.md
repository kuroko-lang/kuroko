## Binding C Functions {#c_functions}

Whether you're embedding Kuroko in an application or writing an extension module, binding a C function is something you'll want to do.

### The Easy Way

```c
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>
#include <kuroko/util.h>

KRK_Function(myfunction) {
    FUNCTION_TAKES_EXACTLY(1);
    CHECK_ARG(0,int,krk_integer_type,myarg);
    return INTEGER_VAL(myarg*myarg);
}

int main(int argc, char *argv[]) {
    krk_initVM(0);
    BIND_FUNC(vm.builtins, myfunction);
    krk_startModule("__main__");
    krk_interpret("print(myfunction(42))","<stdin>");
    krk_freeVM();
    return 0;
}
```

This demo uses the utility macros provided in `<kuroko/util.h>` to easily create a function with argument checking and bind it to the builtin namespace.

`KRK_Function()` takes care of the function signature and function naming for exception messages.

`FUNCTION_TAKES_EXACTLY()` provides simple argument count validation. `FUNCTION_TAKES_AT_LEAST()` and `FUNCTION_TAKES_AT_MOST()` are also available.

`CHECK_ARG()` validates the type of arguments passed to the function and unboxes them to C types.

`INTEGER_VAL()` converts a C integer to a Kuroko `int` value.

`BIND_FUNC()` binds the function to a namespace table.

### The Hard Way

While the macros above provide a convenient way to bind functions, they are just wrappers around lower-level functionality of the API.

```c
#include <kuroko/kuroko.h>
#include <kuroko/vm.h>

static KrkValue myfunction(int argc, KrkValue argv[], int hasKw) {
    int myarg;

    if (argc != 1) return krk_runtimeError(vm.exceptions->argumentError, "myfunction() expects exactly 1 argument, %d given", argc);
    if (!IS_INTEGER(argv[0])) return krk_runtimeError(vm.exceptions->typeError, "expected int, not '%T'", argv[0]);

    myarg = AS_INTEGER(argv[0]);

    return INTEGER_VAL(myarg*myarg);
}

int main(int argc, char *argv[]) {
    krk_initVM(0);
    krk_defineNative(&vm.builtins->fields, "myfunction", myfunction);
    krk_startModule("__main__");
    krk_interpret("print(myfunction(42))", "<stdin>");
    krk_freeVM();
    return 0;
}
```
