# Kuroko - A bytecode-compiled scripting language

Kuroko is a bytecode-interpreted, dynamic, strongly-typed language with syntax similar to Python.

The bytecode VM / compiler is substantially based on Robert Nystrom's [_Crafting Interpreters_](https://craftinginterpreters.com/).

At the moment, the intent for this project is to add a proper scripting language to [Bim](https://github.com/klange/bim), to which both configuration scripts and syntax highlighting will be ported.

Kuroko, as its name should imply, will also be made available in [ToaruOS](https://github.com/klange/toaruos) as a general-purpose user language, and some utilities may end up being written in it.

## Features

Kuroko inherits some core features by virtue of following _Crafting Interpreters_, including its basic type system, classes/methods/functions, and the design of its compiler and bytecode VM.

On top of this, Kuroko has:

- Python-style indentation-based block syntax.
- A syntax-highlighted repl, using ToaruOS's `rline` line editing library.
- Collection types including `list`s and `dict`s, with `[]` indexing syntax and inline declarations.
- List comprehensions (`[foo(x) for x in [1,2,3,4]]` and similar expressions).
- Iterator for loops (`for i in l: ...`).
- Exception handling with `try`/`except`/`raise`.
- A module `import` system for including additional code at runtime.

## Examples

_**NOTE**: Due to limitations with Github's markdown renderer, these snippets will be highlighted as Python code._

### Hello World

Kuroko inherits a print statement from its Lox roots, which is similar to the one in Python 2:

```py
print "Hello, world!"
# → Hello, world!
```

Multiple expressions can be supplied to `print` and will be concatenated with spaces:

```py
print "Hello", 42, "!"
# → Hello 42 !
```

### Basic Types

Kuroko's basic types are integers (which use the platform `long` type), double-precision floats, booleans (`True` and `False`), and `None`.

```py
print 1 + 2 + 3
# → 6
```

When integer values are used in arithmetic operations, such as division, the result will be an integer as well:

```py
print 1 / 2
# → 0
```

To get floating-point results, one of the arguments should be explicitly typed or converted:

```py
print 1 / 2.0
# → 0.5
```

Implicit type conversion occurs late in evaluation, so be careful of integer overflows:

```py
# Probably not what you want:
print 1000000000 * 1000000000 * 1000000000 * 3.0
# → -2.07927e+19
# Try something like this instead:
print 1000000000.0 * 1000000000 * 1000000000 * 3.0
# → 3e+27
```

### Objects

Objects are values which live on the heap. Basic objects include strings, functions, classes, and instances.

Objects are passed by reference, though strings are immutable so this property is only relevant for other object types.

### Strings

Strings can be concatenated, and other values can be appended to them.

```py
print "Hello, " + 42 + "!"
# → Hello, 42!
```

### Functions

Function syntax is essentially the same as in Python:

```py
def greet(name):
    print "Hello, " + name + "!"
greet("user")
# → Hello, user!
```

Optional arguments are supported, though as of writing they must default to `None`:

```py
def greet(name=None):
    if not name:
        print "Hello, world!"
    else:
        print "Hello, " + name + "!"
greet()
gree("user")
# → Hello, world!
#   Hello, user!
```

Blocks, including function `def` blocks and control flow structures like `if` and `for`, must be indented with spaces to a level greater than the enclosing block.

You may indent blocks to whatever level you desire, so long as ordering remains consistent, though the recommendtation indentation size is 4 spaces.

Tabs are not valid as indentation and will be ignored. It is recommended that you use an editor which provides a clear visual distinction between tabs and spaces, such as [Bim](https://github.com/klange/bim).

```py
if False:
	print "Oh no, that was a tab."
# → Oh no, that was a tab.
```

Blocks can also accept a single inline statement:

```py
if True: print "The first rule of Tautology Club is the first rule of Tautology Club."
# → The first rule of Tautology Club is the first rule of Tautology Club.
```

### Variables

In a departure from Python, Kuroko has explicit variable declaration and traditional scoping rules. Variables are declared with the `let` keyword and take the value `None` if not defined at declaration time:

```py
let foo
print foo
# → None
foo = 1
print foo
# → 1
```

### Closures

Functions are first-class values and may be returned from functions and stored in variables, producing _closures_.

When a function references local values from an outter scope, such as in the example below, the referenced variables will be captured.

```py
def foo():
    let i = 1 # Local to this call to foo()
    def bar():
        print i # Reference to outer variable
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

### Basic Objects and Classes

Objects and classes in Kuroko work a lot like Python or similar languages in that they have an arbitrary and mutable set of fields, which may be methods or other values.

To create a basic object without methods, the `object` class is provided:

```py
let o = object()
o.foo = "bar"
print o.foo
# → bar
```

To supply methods, define a class:

```py
class Foo():
    def printFoo():
        print self.foo
let o = Foo()
o.foo = "bar"
o.printFoo()
# → bar
```

The `self` keyword is implicit in all methods and does not need to be supplied in the argument list. You may optionally include it in the method declaration anyway:

```py
class Foo():
    def printFoo(self):
        print self.foo
let o = Foo()
o.foo = "bar"
o.printFoo()
# → bar
```

When a class is instantiated, if it has an `__init__` method it will be called automatically. `__init__` may take arguments as well.

```py
class Foo():
    def __init__(bar):
        self.foo = bar
    def printFoo(self):
        print self.foo
let o = Foo("bar")
o.printFoo()
# → bar
```

Some other special method names include `__get__`, `__set__`, and `__str__`, which will be explained later.

_**Note**: Unlike in Python, most types are not actually instances of classes, though many of the same operations still apply to them._

### Inheritence

Classes may inherit from a single super class:

```py
class Foo():
    def __init__():
        self.type = "foo"
    def printType():
        print self.type

class Bar(Foo):
    def __init__():
        self.type = "bar"

let bar = Bar()
bar.printType()
# → bar
```

Methods can refer to the super class with the `super` keyword:

```py
class Foo():
    def __init__():
        self.type = "foo"
    def printType():
        print self.type

class Bar(Foo):
    def __init__():
        self.type = "bar"
    def printType():
        super().printType()
        print "Also, I enjoy long walks on the beach."

let bar = Bar()
bar.printType()
# → bar
#   Also, I enjoy long walks on the beach.
```

You can determine at runtime if an object is an instance of a class, either directly or through its inheretince chain, with the `isinstance` builtin function:

```py
class Foo:
class Bar:
class Baz(Bar):
let b = Baz()
print isinstance(b,Baz), isinstance(b,Bar), isinstance(b,Foo), isinstance(b,object)
# → True, True, False, True
```

All classes eventually inherit from the base class `object`, which provides default implementations of some special instance methods.

### Collections

Kuroko has built-in classes for flexible arrays (`list`) and hashmaps (`dict`):

```py
let l = list()
l.append(1)
l.append(2)
l.append("three")
l.append(False)
print l
# → [1, 2, three, False]
l[1] = 5
print l
# → [1, 5, three, False]
let d = dict()
d["foo"] = "bar"
d[1] = 2
print d
# → {1: 2, foo: bar}
```

These built-in collections can also be initialized as expressions, which act as syntactic sugar for the `listOf` and `dictOf` built-in functions:

```py
let l = [1,2,"three",False] # or listOf(1,2,"three",False)
print l
# → [1, 2, three, False]
let d = {"foo": "bar", 1: 2} # or dictOf("foo","bar",1,2)
print d
# → {1: 2, foo: bar}
```

Lists can also be generated dynamically:

```py
let fives = [x * 5 for x in [1,2,3,4,5]]
print fives
# → [5, 10, 15, 20, 25]
```

### Exceptions

Kuroko provides a mechanism for handling errors at runtime. If an error is not caught, the interpreter will end and print a traceback.

```py
def foo(bar):
    print "I expect an argument! " + bar
foo() # I didn't provide one!
# → Wrong number of arguments (1 expected, got 0)
#   Traceback, most recent first, 1 call frames:
#     File "<stdin>", line 3, in <module>
```

To catch exceptions, use `try`/`except`:

```py
def foo(bar):
    print "I expect an argument! " + bar
try:
    foo() # I didn't provide one!
except:
    print "oh no!"
# → oh no!
```

Runtime exceptions are passed to the `except` block as a special variable `exception`. As of this writing, runtime exceptions from the VM are strings.

```py
def foo(bar):
    print "I expect an argument! " + bar
try:
    foo() # I didn't provide one!
except:
    print "oh no, there was an exception: " + exception
# → oh no, there was an exception: Wrong number of arguments (1 expected, got 0)
```

Exceptions can also be generated from code:

```py
def login(password):
    if password != "supersecret":
        raise "Wrong password, try again!"
    print "[Hacker voice] I'm in."
login("foo")
# → Wrong password, try again!
#   Traceback, most recent first, 2 call frames:
#     File "<stdin>", line 5, in <module>
#     File "<stdin>", line 3, in login
```

The `except` block is optional, and an exception may be caught and ignored.

```py
def login(password):
    if password != "supersecret":
        raise "Wrong password, try again!"
    print "[Hacker voice] I'm in."
try:
    login("foo")
# → Wrong password, try again!
#   Traceback, most recent first, 2 call frames:
#     File "<stdin>", line 6, in <module>
#     File "<stdin>", line 3, in login
```

### Modules

Modules allow scripts to call other scripts.

```py
# modules/demomodule.krk
let module = object()
module.foo = "bar"
return module
```

```py
# demo.krk
import demomodule
print demomodule.foo
# → bar
```

When modules are imported, they run in a _function local_ context and variables they declare do not live in the global namespace.

To put variables into the global namespace, use the `export` keyword:

```py
# modules/demomodule.krk
let module = object()
foo = "bar"
export foo
return module
```

```py
# demo.krk
import demomodule
print foo
# → bar
```


### Loops

Kuroku supports C-style for loops, while loops, and Python-style iterator for loops.

```py
for i = 1, i < 5, i = i + 1:
    print i
# → 1
#   2
#   3
#   4
```

```py
let i = 36
while i > 1:
    i = i / 2
    print i
# → 18
#   9
#   4
#   2
#   1
```

```py
let l = list()
l.append(1)
l.append(2)
l.append(3)
for i in l:
    print i
# → 1
#   2
#   3
```

### Iterators

The special method `__iter__` should return an iterator. An iterator should be a function which increments an internal state and returns the next value. If there are no values remaining, return the iterator itself.

An example of an iterator is the `range` built-in class, which is defined like this:

```py
class range:
    def __init__(self, min, max):
        self.min = min
        self.max = max
    def __iter__(self):
        let me = self
        def makeIter(ind):
            let l = me
            let i = ind
            def iter():
                if i >= l.max:
                    return iter
                let out = i
                i = i + 1
                return out
            return iter
        return makeIter(self.min)
```

Objects which have an `__iter__` method can then be used with the `for ... in ...` syntax:

```py
for i in range(1,5):
    print i
# → 1
#   2
#   3
#   4
```

### Indexing

Objects with the methods `__get__` and `__set__` can be used with square brackets `[]`:

```py
class Foo:
    def __get__(ind):
        print "You asked for ind=" + ind
        return ind * 5
    def __set__(ind, val):
        print "You asked to set ind=" + ind + " to " + val
let f = Foo()
print f[4]
f[7] = "bar"
# → You asked for ind=4
#   20
#   You asked to set ind=7 to bar
```

### String Conversion

If an object implements the `__str__` method, it will be called to produce string values when concatenating or printing.

```py
class Foo:
    def __str__():
        return "(I am a Foo!)"
let f = Foo()
print f
# → (I am a Foo!)
```


## About the REPL

Kuroko's repl provides an interactive environment for executing code and seeing results.

When entering code at the repl, lines ending with colons (`:`) are treated specially - the repl will continue to accept input and automatically insert indentation on a new line. Please note that the repl's understanding of colons is naive: Whitespace or comments after a colon which would normally be accepted by Kuroko's parser will not be understood by the repl - if you want to place a comment after the start of a block statement, be sure that it ends in a colon so you can continue to enter statements.

Pressing backspace when the cursor is preceded by whitespace will delete up to the last column divisible by 4, which should generally delete one level of indentation automatically.

The tab key will also produce spaces when the cursor is at the beginning of the line or preceded entirely with white space.

The repl will display indentation level indicators in preceeding whitespace as a helpful guide.

When a blank line or a line consisting entirely of whitespace is entered, the repl will process the full input.

Code executed in the repl runs in a global scope and reused variable names will overwrite previous definitions, allowing function and class names to be reused.

The repl will display the last value popped from the stack before returning. Note that unlike with the `print` statement, objects printed in this way from the repl will not be converted to strings, so they may display differently.
