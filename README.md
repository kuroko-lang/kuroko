![logo](.github/kuroko.png)
# Kuroko - A bytecode-compiled scripting language

Kuroko is a bytecode-interpreted, dynamic, strongly-typed language with syntax similar to Python.

The bytecode VM / compiler is substantially based on Robert Nystrom's [_Crafting Interpreters_](https://craftinginterpreters.com/).

At the moment, the intent for this project is to add a proper scripting language to [Bim](https://github.com/klange/bim), to which both configuration scripts and syntax highlighting will be ported.

Kuroko, [as its name should imply](https://toarumajutsunoindex.fandom.com/wiki/Shirai_Kuroko), will also be made available in [ToaruOS](https://github.com/klange/toaruos) as a general-purpose user language, and some utilities may end up being written in it.

## Features

Kuroko inherits some core features by virtue of following _Crafting Interpreters_, including its basic type system, classes/methods/functions, and the design of its compiler and bytecode VM.

On top of this, Kuroko adds a number of features inspired by Python, such as:

- Indentation-based block syntax.
- Collection types: `list`, `dict`, `tuple`, with compiler literal syntax (`[]`,`{}`,`(,)`).
- Iterable types, with `for ... in ...` syntax.
- List comprehensions (`[foo(x) for x in [1,2,3,4]]` and similar expressions).
- Pseudo-classes for basic values (eg. strings are pseudo-instances of a `str` class providing methods like `.format()`)
- Exception handling, with `try`/`except`/`raise`.
- Modules, both for native C code and managed Kuroko code.

## Building Kuroko

### Building as a Shared Library

Kuroko has no external dependencies beyond the system C library and support for `dlopen` for loading C modules.

Generally, `make` should suffice to build from the repository.

The compiler/VM is built as a shared object from these source files:

    builtins.c  chunk.c  compiler.c  debug.c  memory.c  object.c  scanner.c  table.c  value.c  vm.c

The interpreter binary is a thin wrapper and lives in `kuroko.c`; `rline.c` provides the syntax-highlighted line editor for the REPL.

C module sources are found in `src/` and provide optional added functionality. Each module source file corresponds to a resulting shared object of the same name that will be built to the `modules/` directory, which itself also contains modules written in Kuroko.

The core builtins, `builtins.krk` are embedded in `builtins.c` so they are always available to the interpreter; `builtins.c` is provided in the repository, but can be updated by the Makefile when changes to `builtins.krk` are made.

### Building as a Single Static Binary

Configuration options are available in the Makefile to build Kuroko as a static binary.

    make clean; make KRK_ENABLE_STATIC=1

This will produce a static binary without `dlopen` support, so it will not be able to load additional C modules at runtime.

The standard set of C modules can be bundled into the interpreter, whether building statically or normally:

    make clean; make KRK_ENABLE_BUNDLE=1

Additional options include `KRK_DISABLE_RLINE=1` to not link with the included rich line editing library (will lose tab completion and syntax highlighting in the repl) and `KRK_DISABLE_DEBUG=1` to disable debugging features (which has not been demonstrated to provide any meaningful performance improvement when the VM is built with optimizations enabled).

## Code Examples

_**NOTE**: Due to limitations with Github's markdown renderer, these snippets will be highlighted as Python code._

### Hello World

```py
print("Hello, world!")
# → Hello, world!
```

Multiple expressions can be supplied to `print` and will be concatenated with spaces:

```py
print("Hello", 42, "!")
# → Hello 42 !
```

The string used to combine arguments can be changed with `sep=`.

```py
print("Hello", 42, "!", sep="...")
# → Hello...42...!
```

The string printed after all arguments have been printed can be changed with `end=`:

```py
print("Hello",end=" ")
print("World")
# → Hello World
```

_**Note:** When using the REPL with the rich line editor enabled, the line editor will automatically add a line feed when displaying the prompt if the previous output did not include one, and will display a gray left-facing triangle to indicate this._

### Basic Types

Kuroko's basic types are integers (which use the platform `long` type), double-precision floats, booleans (`True` and `False`), and `None`.

```py
print(1 + 2 + 3)
# → 6
```

When integer values are used in arithmetic operations, such as division, the result will be an integer as well:

```py
print(1 / 2)
# → 0
```

To get floating-point results, one of the arguments should be explicitly typed or converted:

```py
print(1 / 2.0)
# → 0.5
```

Implicit type conversion occurs late in evaluation, so be careful of integer overflows:

```py
# Probably not what you want:
print(1000000000 * 1000000000 * 1000000000 * 3.0)
# → -2.07927e+19
# Try something like this instead:
print(1000000000.0 * 1000000000 * 1000000000 * 3.0)
# → 3e+27
```

### Objects

Objects are values which live on the heap. Basic objects include strings, functions, classes, and instances.

Objects are passed by reference, though strings are immutable so this property is only relevant for other object types.

### Strings

Strings can be defined with single, double, or Python-style _triple quotes_, the latter of which allows for unescaped line feeds to appear in strings.

The following escape sequences can be embedded in string literals:

- `\n`: linefeed
- `\r`: carriage return
- `\t`: horizontal tab
- `\[`: ANSI escape value (decimal value 27)

A backslash followed by another character, such as the quoting character used to define the string or another backslash character, will be taking literally.

Strings in Kuroko are immutable; they can not be modified in-place.

Strings can be concatenated, and other values can be appended to them:

```py
print("Hello, " + 42 + "!")
# → Hello, 42!
```

_**Note:** Strings in Kuroko are byte strings, as they were in Python 2. On all platforms currently supported by Kuroko, this means that strings should contain UTF-8 text. It is as-yet undecided if this will change._

### Variables

In a departure from Python, Kuroko has explicit variable declaration and traditional scoping rules. Variables are declared with the `let` keyword and take the value `None` if not defined at declaration time:

```py
let foo
print(foo)
# → None
foo = 1
print(foo)
# → 1
```

You may declare and define multiple variables on a single line:

```py
let a = 1, b = "test", c = object()
print(a,b,c)
# → 1 test <instance of object at ...>
```

### Assignments

After a variable is declared, assignments to it are valid as both expressions and statements. Kuroko provides assignment shortcuts like `+=` and `-=` as well as C-style postfix increment (`++`) and decrement (`--`).

```py
let x = 1
print(x++)
# → 2
print(x -= 7)
# → -5
print((x = 42))
# → 42
```

_**Note:** `var=` is used for keyword arguments, so be careful if you are trying to pass an assignment as a value to a function - wrap it in parentheses first or you may not get what you wanted._

### Functions

Function syntax is essentially the same as in Python:

```py
def greet(name):
    print("Hello, " + name + "!")
greet("user")
# → Hello, user!
```

Default arguments can be specified as follows:

```py
def greet(name="world"):
    print("Hello, " + name + "!")
greet()
gree("user")
# → Hello, world!
#   Hello, user!
```

If a default argument value is not provided, the expression assigned to it will be evaluated as if it were at the top of the body of the function. Note that this behavior intentionally differs from Python, where default values are calculated once when the function is defined; assigning a mutable object, such as a list, as a default value will create a new list with each invocation of the function in Kuroko, rather than re-using the same list. If you would like to have behavior like in Python, define the value outside of the function:

```py
let l = []
def pythonishDefaultList(arg, values=l):
    l.append(arg)
    print("I've seen the following:",values)
pythonishDefaultList("hello")
pythonishDefaultList("world")
# → I've seen the following: ['hello']
#   I've seen the following: ['hello', 'world']
```

Blocks, including function `def` blocks and control flow structures like `if` and `for`, must be indented with spaces to a level greater than the enclosing block.

You may indent blocks to whatever level you desire, so long as ordering remains consistent, though the recommended indentation size is 4 spaces.

It is recommended that you use an editor which provides a clear visual distinction between tabs and spaces, such as [Bim](https://github.com/klange/bim).

Blocks can also accept a single inline statement:

```py
if True: print("The first rule of Tautology Club is the first rule of Tautology Club.")
# → The first rule of Tautology Club is the first rule of Tautology Club.
```

### Closures

Functions in Kuroko are inherently closures and _capture_ local variables from their enclosing scopes.

When a function references a local from another function in which its definition is nested (or variables declared within a block), the referenced variables will continue to "live" in the heap beyond the execution of their original scope context.

If we define a function which declares a local variable and then define an inner function which references that variable, such as in the example below, each call to the other function will create a new instance of the variable and a new instance of the inner function. When the inner function is returned, it will take with it the variable it captured from the outer function and further calls to this instance of the inner function will use that variable.

```py
def foo():
    let i = 1 # Local to this call to foo()
    def bar():
        print(i) # Reference to outer variable
        i = i + 1
    return bar # Produces a closure
let a = foo() # Each copy of `bar` gets its own `i`
let b = foo()
let c = foo()
a() # So these all print "1" as the first call,
b() # but each one also increments its own copy of i
c()
a() # So further calls will reference that copy
a()
a()
# → 1
#   1
#   1
#   2
#   3
#   4
```

### Lambda Functions

Lambda functions allow for the creation of simple functions anonymously. Note that the body of a lambda is an expression, not a list of statements.

```py
let myLambda = lambda x: (x * 5)
print(myLambda(1))
print(myLambda(2))
print(myLambda(3))
# → 5
#   10
#   15
```

Creating a lambda and assigning it immediately to a name is not all that useful, but lambdas can be used whereever an expression is expected.

### Basic Objects and Classes

Objects and classes in Kuroko work a lot like Python or similar languages in that they have an arbitrary and mutable set of fields, which may be methods or other values.

To create a basic object without methods, the `object` class is provided:

```py
let o = object()
o.foo = "bar"
print(o.foo)
# → bar
```

To supply methods, define a class:

```py
class Foo():
    def printFoo():
        print(self.foo)
let o = Foo()
o.foo = "bar"
o.printFoo()
# → bar
```

The `self` keyword is implicit in all methods and does not need to be supplied in the argument list. You may optionally include it in the method declaration anyway, for compatibility with Python:

```py
class Foo():
    def printFoo(self):
        print(self.foo)
let o = Foo()
o.foo = "bar"
o.printFoo()
# → bar
```

_**Note:** As `self` is implicit, it can not be renamed; other argument names listed in a method signature will refer to additional arguments._

Classes can also define fields, which can be accessed from the class or through an instance.

```py
class Foo():
    bar = "baz"
    def printBar(self):
        print(self.bar)
let o = Foo()
o.printBar()
# → baz
print(Foo.bar)
# → baz
```

_**Note:** Instances receive a copy of their class's fields upon creation. If a class field is mutable, the instance's copy will refer to the same underlying object. Assignments to the instance's copy of a field will refer to a new object. If a new field is added to a class after instances have been created, the existing instances will not be able to reference the new field._

When a class is instantiated, if it has an `__init__` method it will be called automatically. `__init__` may take arguments as well.

```py
class Foo():
    def __init__(bar):
        self.foo = bar
    def printFoo(self):
        print(self.foo)
let o = Foo("bar")
o.printFoo()
# → bar
```

Some other special method names include `__get__`, `__set__`, and `__str__`, which will be explained later.

_**Note**: As in Python, all values are objects, but internally within the Kuroko VM not all values are **instances**. The difference is not very relevant to user code, but if you are embedding Kuroko it is important to understand._

### Inheritance

Classes may inherit from a single super class:

```py
class Foo():
    def __init__():
        self.type = "foo"
    def printType():
        print(self.type)

class Bar(Foo):
    def __init__():
        self.type = "bar"

let bar = Bar()
bar.printType()
# → bar
```

Methods can refer to the super class with the `super` keyword, which should be called as a function with no arguments, as in new-style Python code:

```py
class Foo():
    def __init__():
        self.type = "foo"
    def printType():
        print(self.type)

class Bar(Foo):
    def __init__():
        self.type = "bar"
    def printType():
        super().printType()
        print("Also, I enjoy long walks on the beach.")

let bar = Bar()
bar.printType()
# → bar
#   Also, I enjoy long walks on the beach.
```

You can determine the type of an object at runtime with the `type` function:

```py
class Foo():
let foo = Foo()
print(type(foo))
# → <type 'Foo'>
```

You can also determine if an object is an instance of a given type, either directly or through its inheritence chain, with the `isinstance` function:

```py
class Foo:
class Bar:
class Baz(Bar):
let b = Baz()
print(isinstance(b,Baz), isinstance(b,Bar), isinstance(b,Foo), isinstance(b,object))
# → True, True, False, True
```

All classes eventually inherit from the base class `object`, which provides default implementations of some special instance methods.

### Collections

Kuroko has built-in classes for flexible arrays (`list`) and hashmaps (`dict`), as well as immutable lists of items (`tuple`).

```py
let l = list()
l.append(1)
l.append(2)
l.append("three")
l.append(False)
print(l)
# → [1, 2, three, False]
l[1] = 5
print(l)
# → [1, 5, three, False]
let d = dict()
d["foo"] = "bar"
d[1] = 2
print(d)
# → {1: 2, foo: bar}
```

These built-in collections can also be initialized as expressions, which act as syntactic sugar for the `listOf` and `dictOf` built-in functions:

```py
let l = [1,2,"three",False] # or listOf(1,2,"three",False)
print(l)
# → [1, 2, three, False]
let d = {"foo": "bar", 1: 2} # or dictOf("foo","bar",1,2)
print(d)
# → {1: 2, foo: bar}
```

Tuples provide similar functionality to lists, but are intended to be immutable:

```py
let t = (1,2,3)
print(t)
# → (1, 2, 3)
```

A `set` type is also provided:

```py
let s = set([1,2,3])
print(s)
# → {1, 2, 3}
s.add(2)
print(s) # No change
# → {1, 2, 3}
s.add(4)
print(s)
# → {1, 2, 3, 4}
```

Lists can also be generated dynamically through _comprehensions_, just as in Python:

```py
let fives = [x * 5 for x in [1,2,3,4,5]]
print(fives)
# → [5, 10, 15, 20, 25]
```

_**Note:** Dictionary, tuple, and set comprehensions are not currently available, but are planned._

### Exceptions

Kuroko provides a mechanism for handling errors at runtime. If an error is not caught, the interpreter will end and print a traceback.

```py
def foo(bar):
    print("I expect an argument! " + bar)
foo() # I didn't provide one!
# → Traceback, most recent first, 1 call frame:
#     File "<stdin>", line 1, in <module>
#   ArgumentError: foo() takes exactly 1 argument (0 given)
```

When using the repl, global state will remain after an exception and the prompt will be displayed again.

To catch exceptions, use `try`/`except`:

```py
def foo(bar):
    print("I expect an argument! " + bar)
try:
    foo() # I didn't provide one!
except:
    print("oh no!")
# → oh no!
```

Runtime exceptions are passed to the `except` block as a special variable `exception`. Exceptions from the VM are instances of built-in error classes with an attribute `arg`:

```py
def foo(bar):
    print("I expect an argument! " + bar)
try:
    foo() # I didn't provide one!
except:
    print("oh no, there was an exception:", exception.arg)
# → oh no, there was an exception: foo() takes exactly 1 argument (0 given)
```

Exceptions can be generated with the `raise` statement. When raising an exception, the value can be anything, but subclasses of `__builtins__.Exception` are recommended.

```py
def login(password):
    if password != "supersecret":
        raise ValueError("Wrong password, try again!")
    print("[Hacker voice] I'm in.")
login("foo")
# → Traceback, most recent first, 2 call frames:
#     File "<stdin>", line 5, in <module>
#     File "<stdin>", line 3, in login
#   ValueError: Wrong password, try again!
```

The `except` block is optional, and an exception may be caught and ignored.

```py
def login(password):
    if password != "supersecret":
        raise ValueError("Wrong password, try again!")
    print("[Hacker voice] I'm in.")
try:
    login("foo")
# (no output)
```

`try`/`except` blocks can also be nested within each other. The deepest `try` block will be used to handle an exception. If its `except` block calls `raise`, the exception will filter up to the next `try` block. Either the original exception or a new exception can be raised.

```py
try:
    print("Level one")
    try:
        print("Level two")
        raise ValueError("Thrown exception")
    except:
        print("Caught in level two")
except:
    print("Not caught in level one!")
# → Level one
#   Level two
#   Caught in level two
try:
    print("Level one")
    try:
        print("Level two")
        raise ValueError("Thrown exception")
    except:
        print("Caught in level two")
        raise exception
except:
    print("Caught in level one!")
# → Level one
#   Level two
#   Caught in level two
#   Caught in level one!
```

### Modules

Modules in Kuroko work much like in Python, allowing scripts to call other scripts and use functions defined in other files.

```py
# modules/demomodule.krk
foo = "bar"
```

```py
# demo.krk
import demomodule
print(demomodule.foo)
# → bar
```

Modules are run once and then cached, so if they preform actions like printing or complex computation this will happen once when first imported. The globals table from the module is the fields table of an object. Further imports of the same module will return the same object.

When importing a module, the names of members which should be imported can be specified and can be renamed:

```py
from demomodule import foo
print(foo)
# → bar
```

```py
from demomodule import foo as imported
print(imported)
# → bar
```

_**Note:** When individual names are imported from a module, they refer to the same object, but if new assignments are made to the name it will not affect the original module. If you need to replace values defined in a module, always be sure to refer to it by its full name._

### Loops

Kuroku supports C-style for loops, while loops, and Python-style iterator for loops.

```py
for i = 1; i < 5; i++:
    print(i)
# → 1
#   2
#   3
#   4
```

```py
let i = 36
while i > 1:
    i = i / 2
    print(i)
# → 18
#   9
#   4
#   2
#   1
```

```py
let l = [1,2,3]
for i in l:
    print(i)
# → 1
#   2
#   3
```

If an iterator returns tuples, the values can be unpacked into multiple variables:

```py
let l = [(1,2),(3,4),(5,6)]
for left, right in l:
    print(left, right)
# → 1 2
#   3 4
#   5 6
```

An exception will be raised if a tuple returned by the iterator has the wrong size for unpacking (`ValueError`), or if a value returned is not a tuple (`TypeError`).

### Iterators

The special method `__iter__` should return an iterator. An iterator should be a function which increments an internal state and returns the next value. If there are no values remaining, return the iterator object itself.

_**Note:** The implementation of iterators in Kuroko differs from Python. In Python, iterator objects have a `__next__` method while in Kuroko they are called as functions or using the `__call__` method on instances, which allows iterators to be implemented as simple closures. In Python, completed iterators raise an exception called `StopIteration` while Kuroko iterators are expected to return themselves when they are done._

An example of an iterator is the `range` built-in class, which was previously defined like this:

```py
class range:
    "Helpful iterable."
    def __init__(self, min, max):
        self.min = min
        self.max = max
    def __iter__(self):
        let i=self.min
        let l=self.max
        def _():
            if i>=l:
                return _
            let o=i
            i++
            return o
        return _
```

Objects which have an `__iter__` method can then be used with the `for ... in ...` syntax:

```py
for i in range(1,5):
    print(i)
# → 1
#   2
#   3
#   4
```

As in the _Loops_ section above, an iterator may return a series of tuples which may be unpacked in a loop. Tuple unpacking is optional; if the loop uses a single variable, the tuple will be assigned to it with each iteration.

```py
class TupleGenerator:
    def __iter__():
        let up = 0, down = 0, limit = 5
        def _():
            if limit-- == 0: return _
            return (up++,down--)
        return _
for i in TupleGenerator():
    print(i)
# → (1, -1)
#   (2, -2)
#   (3, -3)
#   (4, -4)
for up, down in TupleGenerator():
    print(up,down)
# → 1 -1
#   2 -2
#   3 -3
#   4 -4
```

### Indexing

Objects with `__get__` and `__set__` methods can be used with square brackets `[]`:

```py
class Foo:
    def __get__(ind):
        print("You asked for ind=" + ind)
        return ind * 5
    def __set__(ind, val):
        print("You asked to set ind=" + ind + " to " + val)
let f = Foo()
print(f[4])
f[7] = "bar"
# → You asked for ind=4
#   20
#   You asked to set ind=7 to bar
```

### Slicing

Substrings can be extracted from strings via slicing:

```py
print("Hello world!"[3:8])
# → lo wo
print("Hello world!"[:-1])
# → Hello world
print("Hello world!"[-1:])
# → !
```

_**NOTE**: Step values are not yet supported._

Lists can also be sliced:

```py
print([1,2,3,4,5,6][3:])
# → [4, 5, 6]
```

### String Conversion

If an object implements the `__str__` method, it will be called to produce string values through casting or printing.

```py
class Foo:
    def __str__():
        return "(I am a Foo!)"
let f = Foo()
print(f)
# → (I am a Foo!)
```

The `__repr__` method serves a similar purpose and is used when the REPL displays values or when they are used in string representations of collections. The implementations of `__str__` and `__repr__` can be different:

```py
class Foo:
    def __str__():
        return "What is a Foo but a miserable pile of methods?"
    def __repr__():
        return "[Foo instance]"
let f = Foo()
print(f)
print([f,f,f])
# → What is a Foo but a miserable pile of methods?
#   [[Foo instance], [Foo instance], [Foo instance]]
```

As in Python, `__repr__` is intended to provide a canonical string representation which, if possible, should be usable to recreate the object.

_**Note:** As all classes eventually inherit from `object` and `object` implements both `__str__` and `__repr__`, these methods should always be available._

### File I/O

The module `fileio` provides an interface for opening, reading, and writing files, including `stdin`/`stdout`/`stderr`.

To open and read the contents of a file:

```py
import fileio
let f = fileio.open("README.md","r")
print(f.read())
f.close()
```

To write to `stdout` (notably, without automatic line feeds):

```py
import fileio
fileio.stdout.write("hello, world")
```

To read lines from `stdin`:

```py
import fileio

while True:
    fileio.stdout.write("Say something: ")
    fileio.stdout.flush()

    let data = fileio.stdin.readline()
    if data[-1] == '\n':
        data = data[:-1]
    if data == "exit":
        break
    print("You said '" + data + "'!")
```

### Decorators

Decorators allow functions and methods to be wrapped.

```py
def decorator(func):
    print("I take the function to be decorated as an argument:", func)
    def wrapper():
        print("And I am the wrapper.")
        func()
        print("Returned from wrapped function.")
    return wrapper

@decorator
def wrappedFunction():
    print("Hello, world")

wrappedFunction()
# → I take a function to be decorated as an argument: <function wrappedFunction>
#   And I am the wrapper.
#   Hello, world
#   Returned from wrapped function
```

The inner wrapper function is not necessary if all the work of the decorator can be done when the function is defined:

```py
def registerCallback(func):
    print("Registering callback function",func)
    return func

@registerCallback
def aFunction():
    print("Hello, world!")

aFunction()
# → Registering callbacuk function <function aFunction>
#   Hello, world!
```

Method wrappers work similarly, though be sure to explicitly provide a name (other than `self`) for the object instance:

```py
def methodDecorator(method):
    def methodWrapper(instance, anExtraArgument):
        method(instance)
        print("I also required this extra argument:", anExtraArgument)
    return methodWrapper

class Foo():
    @methodDecorator
    def theMethod():
        print("I am a method, so I can obviously access", self)
        print("And I also didn't take any arguments, but my wrapper did:")

let f = Foo()
f.theMethod("the newly required argument")
# → I am a method, so I can obviously access <instance of Foo at ...>
#   And I also didn't take any arguments, but my wrapper did:
#   I also required this extra argument: the newly required argument
```

Decorators are _expressions_, just like in Python, so to make a decorator with arguments create a function that takes those arguments and returns a decorator:

```py
def requirePassword(password):
    print("I am creating a decorator.")
    def decorator(func):
        print("I am wrapping", func, "and attaching",password)
        def wrapper(secretPassword):
            if secretPassword != password:
                print("You didn't say the magic word.")
                return
            func()
        return wrapper
    return decorator

@requirePassword("hunter2")
def superSecretFunction():
    print("Welcome!")

superSecretFunction("a wrong password")
print("Let's try again.")
superSecretFunction("hunter2")
# → I am wrapping <function superSecretFunction> and attaching hunter2
#   You didn't say the magic word.
#   Let's try again.
#   Welcome!
```



### Keyword Arguments

Arguments may be passed to a function by specifying their name instead of using their positional location.

```py
def aFunction(a,b,c):
    print(a,b,c)

aFunction(1,2,3)
aFunction(1,c=3,b=2)
aFunction(b=2,c=3,a=1)
# → 1 2 3
#   1 2 3
#   1 2 3
```

This will be slower in execution than a normal function call, as the interpreter will need to figure out where to place arguments in the requested function by examining it at runtime, but it allows for functions to take many default arguments without forcing the caller to specify the values for everything leading up to one they want to specifically set.

```py
def aFunction(with=None,lots=None,of=None,default=None,args=None):
    print(with,lots,of,default,args)

aFunction(of="hello!")
# → None None hello! None None
```

### `*args` and `**kwargs`

When used as parameters in a function signature, `*` and `**` before an identifier indicate that the function will accept arbitrary additional positional arguments and keyword arguments respectively. These options are typically applied to variables named `args` and `kwargs`, and they must appear last (and in this order) in the function signature if used.

The variable marked with `*` will be provided as an ordered `list`, and `**` will be an unordered `dict` of keyword names to values.

```py
def takesArgs(*args):
    print(args)
takesArgs(1,2,3)
# → [1, 2, 3]
def takesKwargs(**kwargs):
    print(kwargs)
takesKwargs(a=1,b=2,c=3)
# → {'a': 1, 'b': 2, 'c': 3}
def takesEither(*args,**kwargs):
    print(args, kwargs)
takesEither(1,2,a=3,b=4)
# → [1, 2] {'a': 3, 'b': 4}
def takesARequiredAndMore(a,*args):
    print(a, args)
takesARequiredAndMore(1,2,3,4)
# → 1 [2, 3, 4]
```

_**Note:** `*args` and `**kwargs` are especially useful in combination with Argument Expension (described below) to create decorators which do not need to know about the signatures of functions they wrap._

### Argument Expansion

When used in a function argument list, `*` and `**` before a list and dict expression respectively, will expand those values into the argument list.

```py
let l = [1,2,3]
def foo(a,b,c):
    print(a,b,c)
foo(*l)
# → 1 2 3
let d = {"foo": "a", "bar": 1}
def func(foo,bar):
    print(foo, bar)
func(**d)
# → a 1
```

If an expanded list provides too many, or too few, arguments, an ArgumentError will be raised.

If an expanded dict provides parameters which are not requested, an ArgumentError will be raised.

If an expanded dict provides an argument which has already been defined, either as a positional argument or through a named parameter, an error will be raised.

### Ternary Expressions

Ternary expressions allow for branching conditions to be used in expression contexts:

```py
print("true branch" if True else "false branch")
print("true branch" if False else "false branch")
# → true branch
# → false branch
```

Ternary expressions perform short-circuit and will not evaluate the branch they do not take:

```py
(print if True else explode)("What does this do?")
# → What does this do?
```

### Docstrings

If the first expression in a module, function, class, or method is a string, it will be attached to the corresponding object in the field `__doc__`:

```py
def foo():
    '''This is a function that does things.'''
    return 42

print(foo.__doc__)
# → This is a function that does things.
```

### `with` Blocks

A `with` statement introduces a context manager:

```py
with expr:
    ...
```

The value of `expr` must be an object with an `__enter__` and `__exit__` method, such as a `fileio.File`. The `__enter__` method will be called upon entry and the `__exit__` method will be called upon exit from the block.

The result of `expr` can also be assigned a name for use within the block. Note that as with other control flow structures in Kuroko, this name is only valid within the block and can not be referenced outside of it, and the same is true of any names defined within the block. If you need to output values from within the block, such as in the typical case of opening a file and loading its contents, be sure to declare any necessary variables before the `with` statement:

```py
from fileio import open
let lines
with open('README.md') as f:
    lines = [l.strip() for l in f.readlines()]
print(lines)
# → ["![logo]...
```

Note that you can declare a variable for the object used with `__enter__` and `__exit__` before the `with` statement:

```py
from fileio import open
let f = open('README.md')
print(f)
# → <open file 'README.md' ...>
let lines
with f:
    lines = [l.strip() for l in f.readlines()]
print(f)
# → <closed file 'README.md' ...>
```

If an early return is encountered inside of a `with` block, the `__exit__` method for the context manager will be called before the function returns.

```py
class ContextManager:
    def __init__(title):
        self.title = title
    def __enter__():
        print("Enter context manager", self.title)
    def __exit__():
        print("Exit context manager", self.title)
def doesANestedThing():
    with ContextManager('outer'):
        with ContextManager('inner'):
            with ContextManager('triple'):
                return 42
            print('Should not print')
        print('Should not print')
    print('Should not print')
print(doesANestedThing())
# → Enter context manager outer
#   Enter context manager inner
#   Enter context manager triple
#   Exit context manager triple
#   Exit context manager inner
#   Exit context manager outer
#   42
```

_**Note:** The implementation of `with` blocks is incomplete; exceptions raised from within a `with` that are not caught within the block will cause `__exit__` to not be called._

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

There are two ways to connect Kuroko with C code: embedding and modules.

Embedding involves including the interpreter library and initializing and managing the VM state yourself.

C modules allow C code to provide functions through imported modules.

If you want to provide C functionality for Kuroko, build a module. If you want to provide Kuroko as a scripting language in a C project, embed the interpreter.

With either approach, the API provided by Kuroko is the same beyond initialization.

### Embedding Kuroko

Kuroko is built as a shared libary, `libkuroko.so`, which can be linked against. `libkuroko.so` generally depends on the system dynamic linker, which may involve an additional library (eg. `-ldl`).

The simplest example of embedding Kuroko is to initialize the VM and interpret an embedded line of code:

```c
#include <stdio.h>
#include <kuroko.h>

int main(int argc, char *argv[]) {
    krk_initVM();
    krk_interpret("import kuroko\nprint('Kuroko',kuroko.version)\n", 1, "<stdin>","<stdin>");
    krk_freeVM();
    return 0;
}
```

There is a single, shared VM state. `krk_initVM()` will initialize the compiler and create built-in objects.

`krk_interpret` compiles and executes a block of code and takes the following arguments:

```c
    KrkValue krk_interpret(const char *sourceText, int newModuleScope, char *fromName, char *fromFile);
```

If `newModuleScope` is non-zero, the interpreter will parse code in the context of a new _module_ and the `KrkValue` returned will be a `module` object.

If `newModuleScope` is zero, the return value will be the last value popped from the stack during execution of `sourceText`. This can be used, as in the REPL, when provided interactive sessions.

The arguments `fromName` provide the name of the module created by when `newModuleScope` is non-zero, and `fromFile` will be used when displaying tracebacks.

### Building Modules

Modules are shared objects with at least one exported symbol: `krk_module_onload_{NAME}` (where `{NAME}` is the name of your module, which should also be the name of your shared object file excluding the `.so` suffix).

Your module's `krk_module_onload_...` function should return a `KrkValue` representing a `KrkInstance` of the `vm.moduleClass` class.

```c
KrkValue krk_module_onload_fileio(void) {
	KrkInstance * module = krk_newInstance(vm.moduleClass);
	/* Store it on the stack for now so we can do stuff that may trip GC
	 * and not lose it to garbage colletion... */
	krk_push(OBJECT_VAL(module));

	/* ... */

	/* Pop the module object before returning; it'll get pushed again
	 * by the VM before the GC has a chance to run, so it's safe. */
	assert(AS_INSTANCE(krk_pop()) == module);
	return OBJECT_VAL(module);
}
```

### Defining Native Functions

Simple functions may be added to the interpreter by binding to them to `vm.builtins` or your own module instance.

Native functions should have a call signature as follows:

```c
KrkNative my_native_function(int argc, KrkValue argv[], int hasKw);
```

If `hasKw` is non-zero, then the value in `argv[argc-1]` will represent a dictionary of keyword and value pairs. Positional arguments will be provided in order in the other indexes of `argv`.

Functions must return a value. If you do not need to return data to callers, return `NONE_VAL()`.

To bind the function, use `krk_defineNative`:

```c
krk_defineNative(&vm.builtins->fields, "my_native_function", my_native_function);
```

Binding to `vm.builtins->fields` will make your function accessible from any scope (if its name is not shadowed by a module global or function local) and is discouraged for modules but recommended for embedded applications.

### Kuroko's Object Model

For both embedding and C modules, you will likely want to create and attach functions, classes, objects, and so on.

It is recommended you read [_Crafting Interpreters_](https://craftinginterpreters.com/contents.html), particularly the third section describing the implementation of `clox`, as a primer on the basic mechanisms of the _value_ system that Kuroko is built upon.

Essentially, everything accessible to the VM is represented as a `KrkValue`, which this documentation will refer to simply as a _value_ from here on out.

Values are small, fixed-sized items and are generally considered immutable. Simple types, such as integers, booleans, and `None`, are directly represented as values and do not exist in any other form.

More complex types are represented by subtypes of `KrkObj` known as _objects_, and values that represent them contain pointers to these `KrkObj`s. The `KrkObj`s themselves live on the heap and are managed by the garbage collector.

Strings, functions, closures, classes, instances, and tuples are all basic objects and carry additional data in their heap representations.

_Strings_ (`KrkString`) are immutable and deduplicated - any two strings with the same text have the same _object_. (See _Crafting Interpreters_, chapter 19) Strings play a heavy role in the object model, providing the basic type for indexing into attribute tables in classes and instances.

_Functions_ (`KrkFunction`) represent bytecode, argument lists, default values, local names, and constants - the underlying elements of execution for a function. Generally, functions are not relevant to either embedding or C modules and are an internal implementation detail of the VM.

_Closures_ (`KrkClosure`) represent the callable objects for functions defined in user code. When embedding or building a C module, you may need to deal with closures for Kuroko code passed to your C code.

_Bound methods_ (`KrkBoundMethod`) connect methods with the "self" object they belong to, allowing a single value to be passed on the stack and through fields.

_Classes_ (`KrkClass`) represent collections of functions. In Kuroko, all object and value types have a corresponding `KrkClass`.

_Instances_ (`KrkInstance`) represent _user objects_ and store _fields_ in a hashmap and also point to the class they are an instance _of_. Instances can represent many things, including collections like lists and dictionaries, modules, and so on.

_Tuples_ (`KrkTuple`) represent simple fixed-sized lists and are intended to be immutable.

Finally, _native functions_ (`KrkNative`) represent callable references to C code.

Most extensions to Kuroko, both in the form of embedded applications and C modules, will primarily deal with classes, instances, strings, and native functions.

Two of the high-level collection types, lists and dictionaries, are instances of classes provided by the `__builtins__` module. While they are not their own type of `KrkObj`, some macros are provided to deal with them.

### Creating Objects

Now that we've gotten the introduction out of the way, we can get to actually creating and using these things.

The C module example above demonstrates the process of creating an object in the form of an instance of the `vm.moduleClass` class. All C modules should create one of these instances to expose other data to user code that imports them.

Most extensions will also want to provide their own types through classes, as well as create instances of those classes.

_**NOTE:** When creating and attaching objects, pay careful attention to the order in which you allocate new objects, including strings. If two allocations happen in sequence without the first allocated object being stored in a location reachable from the interpreter roots, the second allocation may trigger the garbage collector which will immediately free the first object. If you need to deal with complex allocation patterns, place values temporarily on the stack to prevent them from being collected._

```c
    /* Create a class 'className_' and attach it to our module. */
    KrkClass * myNewClass = krk_newClass(krk_copyString("MyNewClass", 10));
    krk_attachNamedObject(&module->fields, "MyNewClass", (KrkObj*)myNewClass);
```

Here we have created a new class named `MyNameClass` and exposed it through the `fields` table of our module object under the same name. We're not done preparing our class, though:


```c
    myNewClass->base = vm.objectClass;
    krk_tableAddAll(&vm.objectClass->methods, &myNewClass->methods);
```

We also want to make sure that our new class fits into the general inheritence hierarchy, which typically means inheriting from `vm.objectClass` - we do this by setting our new class's `base` pointer to `vm.objectClass` and copying `vm.objectClass`'s method table. Now we can start customizing our class with its own methods.

Native functions are attached to class method tables in a similar manner to normal functions:

```c
krk_defineNative(&myNewClass->methods, ".my_native_method", my_native_method);
```

When attaching methods, notice the `.` at the start of the name. This indicates to `krk_defineNative` that this method will take a "self" value as its first argument. This affects how the VM modifies the stack when calling native code and allows native functions to integrate with user code functions and methods.

In addition to methods, native functions may also provide classes with _dynamic fields_. A dynamic field works much like a method, but it is called implictly when the field is accessed. Dynamic fields are used by the native classes to provide non-instance values with field values.

```c
krk_defineNative(&myNewClass->methods, ":my_dynamic_field", my_dynamic_field);
```

If your new instances of your class will be created by user code, you can provide an `__init__` method, or any of the other special methods described in the Examples above.

When you've finished attaching all of the relevant methods to your class, be sure to call `krk_finalizeClass`, which creates shortcuts within your class's struct representation that allow the VM to find special functions quickly:

```c
krk_finalizeClass(myNewClass)
```

Specifically, this will search through the class's method table to find implementtations for functions like `__repr__` and `__init__`. This step is required for these functions to work as expected as the VM will not look them up by name.

