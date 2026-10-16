# Functions

Declaring functions, passing arguments and returning values.

## Declaring a function

A function gives its return type, its name and its parameters. `void` means it returns nothing.

```iris
i64 add(i64 a, i64 b) {
    return a + b
}

void greet(str name) {
    print("hello {name}")
}
```

Parameters next to each other with the same type can share it:

```iris
i64 add3(i64 a, b, c) {
    return a + b + c
}
```

A function that is a single expression can use `=>` and leave out the braces and `return`:

```iris
i64 square(i64 x) => x * x
bool isDigit(u8 c) => c >= '0' && c <= '9'
```

Functions can be declared in any order. A function can call one declared further down the file.

## Arguments are copies

Arguments are copied into the function. Changing a parameter does not change the caller's value. To let a function change a value, pass a pointer to it:

```iris
void reset(i64 *n) {
    *n = 0
}

i64 count = 5
reset(&count)      // count is now 0
```

## Default values

A parameter can have a default value. A call can leave it out.

```iris
void connect(str host, u16 port = 80, Duration timeout = 30s) {
}

connect("example.com")
connect("example.com", 8080)
```

Parameters with a default come after the ones without. A default must be a value known when compiling.

## Named arguments

Any argument can be passed by name. Named arguments come after the others, in any order. This makes calls with several defaults easy to read:

```iris
connect("example.com", timeout: 5s)
connect("example.com", timeout: 5s, port: 443)
```

A complete program:

```iris run
void greet(str name, str greeting = "hello") {
    print("{greeting}, {name}")
}

void main() {
    greet("iris")
    greet("iris", greeting: "hi")
}
```

```output
hello, iris
hi, iris
```

## Several return values

A function can return several values in parentheses. The caller can unpack them into new variables:

```iris
(i64, i64) divmod(i64 a, b) => (a / b, a % b)

q, r := divmod(17, 5)      // q is 3, r is 2
```

`_` skips a value:

```iris
_, r := divmod(17, 5)
```

Several values can be declared or assigned at once. The values on the right are worked out before any is stored, so this swaps two variables:

```iris run
void main() {
    x, y := 1, 2
    x, y = y, x
    print("{x} {y}")
}
```

```output
2 1
```

A function's several values must be unpacked into names; they can't be kept in one variable.

## Functions as values

A function's name can be passed to another function. The parameter's type is written as the return type followed by the parameter types:

```iris
i64 apply(i64(i64) f, i64 x) => f(x)

n := apply(square, 4)      // 16
```

Iris has no lambdas, so the function passed must have a name. A function value can be kept in a variable, a field or an array, compared with `==`, and is `null` until it is set. Calling a `null` one stops a debug build with an error. Arguments are passed by position; defaults and names don't apply through a function value.

```iris run
type Op = i64(i64, i64)

i64 add(i64 a, b) => a + b
i64 mul(i64 a, b) => a * b

struct Button {
    str label
    void(str) onClick
}

void shout(str s) => print("{s} clicked")

void main() {
    Op[2] ops = [add, mul]
    for f in ops {
        print("{f(6, 7)}")
    }
    b := Button{label: "ok", onClick: shout}
    b.onClick(b.label)
}
```

```output
13
42
ok clicked
```
