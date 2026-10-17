# Variables

Declaring variables, giving them values, and where each one can be used.

## Declaring with :=

`:=` declares a new variable and gives it a value. The variable's type is the value's type:

```iris run
void main() {
    count := 10       // i64
    ratio := 0.25     // f64
    ready := true     // bool
    name := "iris"    // str
    timeout := 1500ms // Duration
    print("{count} {ratio} {ready} {name} {timeout}")
}
```

```output
10 0.25 true iris 1.5s
```

A whole number with no other type to go by is an `i64`, and a number with a point or an exponent is an `f64`.

## Declaring with a type

A declaration can also name the type, followed by the variable and its value. This is the way to get a type other than the one the value would give:

```iris run
void main() {
    u8 small = 200
    i32 code = -1
    f32 scale = 1.5
    str label = "ready"
    print("{small} {code} {scale} {label}")
}
```

```output
200 -1 1.5 ready
```

A number given to a typed variable must fit in it. The compiler checks this:

```iris error
u8 b = 300 // error: 300 doesn't fit in u8
```

## Zero values

A variable declared without a value starts at zero: `0` for numbers, `false` for `bool`, `""` for `str`, `null` for pointers, and every field and item at zero for structs and arrays.

```iris run
struct Point {
    i64 x
    i64 y
}

void main() {
    i64 n
    bool done
    str s
    f64 f
    i64[3] counts
    Point p
    print("[{n}] [{done}] [{s}] [{f}] [{counts[0]} {counts[2]}] [{p.x} {p.y}]")
}
```

```output
[0] [false] [] [0] [0 0] [0 0]
```

Every local variable starts at zero, so reading one before giving it a value is never undefined.

## Assignment

`=` gives a variable a new value of the same type. The operators that change a variable in place are written with `=` after them:

```iris run
void main() {
    n := 5
    n = 7
    n += 3  // 10
    n -= 1  // 9
    n *= 4  // 36
    n /= 5  // 7
    n %= 4  // 3
    n <<= 2 // 12
    n |= 1  // 13
    print("{n}")
}
```

```output
13
```

Assignment is a statement: it gives no value, so it can't sit inside an expression or a condition.

## Several at once

Several variables can be declared or assigned in one statement, with the names on the left and the values on the right. Every value is worked out before any variable changes, so two variables can swap without a third:

```iris run
void main() {
    x, y := 1, 2
    print("{x} {y}")
    x, y = y, x
    print("{x} {y}")
    first, rest := "a", "bcd"
    print("{first} then {rest}")
}
```

```output
1 2
2 1
a then bcd
```

The same form takes the several values a function returns (see [Functions](10-functions.html)). `_` stands for a value that isn't wanted:

```iris run
import str

void main() {
    name, _, found := str.cut("ada@example.com", "@")
    print("{name} {found}")
}
```

```output
ada true
```

## Scope

A variable can be used from its declaration to the end of the block it is declared in. A block inside it can use it too:

```iris run
void main() {
    total := 0
    for i in 1..4 {
        square := i * i // only inside the loop's block
        total += square
    }
    print("{total}")
}
```

```output
14
```

A name can't be declared again while it is still visible, in the same block or in a block inside it. This keeps every name meaning one thing within a function:

```iris error
x := 1
if ready {
    x := 2 // error: x is already declared on line 1
}
```

Two blocks side by side can each have their own variable with the same name, since neither sees the other:

```iris run
void main() {
    for i in 0..2 {
        step := i * 10
        print("{step}")
    }
    for i in 0..1 {
        step := 99
        print("{step}")
    }
}
```

```output
0
10
99
```

## Globals

A variable declared outside any function is a global. Every function of the module can use it, and it lives as long as the program:

```iris run
i64 calls
str prefix = "call"

void count() {
    calls += 1
    print("{prefix} {calls}")
}

void main() {
    count()
    count()
}
```

```output
call 1
call 2
```

A global's value must be known when the program is compiled: a number, text, a constant, or an array or struct made of those. Work that has to run first goes at the start of `main`, or into a `comptime` constant (see [Compile time](26-compile-time.html)). A global with no value starts at zero.

Functions can use a global declared further down the file: declarations at the top level can come in any order.

Globals are shared by every task of the program. A global that several threads change at the same time needs an `Atomic` or a `Mutex`, and `threadlocal` gives each thread its own copy (see [Threads](25-threads.html)).

## Constants

`const` gives a name to a value that never changes. The value is worked out when compiling, from numbers, text and other constants:

```iris run
const KiB = 1024
const BUFFER = 64 * KiB
const NAME = "iris"
const RATE = 0.5

void main() {
    print("{NAME}: {BUFFER} bytes at {RATE}")
}
```

```output
iris: 65536 bytes at 0.5
```

A constant has no address and takes no memory: each use is the value itself. An integer constant takes the type the place it is used needs, as long as the value fits, so `const LIMIT = 200` can be given to a `u8` and to an `i64`.
