# Functions

Declaring functions, passing them arguments, giving defaults and names to those arguments, and returning one value or several.

## Declaring a function

A function is written as its return type, its name, its parameters in parentheses, and a block. `void` means it gives nothing back:

```iris run
i64 add(i64 a, i64 b) {
    return a + b
}

void greet(str name) {
    print("hello, {name}")
}

void main() {
    greet("Ada")
    print("{add(2, 3)}")
}
```

```output
hello, Ada
5
```

Parameters next to each other with the same type can share it: `i64 add(i64 a, b)` is the same as `i64 add(i64 a, i64 b)`.

A function that gives a value must give it on every way out of its block. One that can reach its closing brace without a `return` doesn't compile.

## One expression

A function whose body is one expression can be written with `=>`, leaving out the braces and the `return`:

```iris run
i64 square(i64 x) => x * x
bool isDigit(u8 c) => c >= '0' && c <= '9'
str sign(i64 n) => if n < 0 { "negative" } else if n == 0 { "zero" } else { "positive" }
void show(i64 n) => print("{n} is {sign(n)}")

void main() {
    show(square(-3))
    show(0)
    print("{isDigit('7')} {isDigit('x')}")
}
```

```output
9 is positive
0 is zero
true false
```

For a `void` function, the expression after `=>` is a call.

## Order

Functions can be declared in any order: a function can call one declared further down the file, or in another file of the same module. A function can call itself:

```iris run
i64 fib(i64 n) => if n < 2 { n } else { fib(n - 1) + fib(n - 2) }

void main() {
    for i in 0..10 {
        print("{fib(i)}")
    }
}
```

```output
0
1
1
2
3
5
8
13
21
34
```

Each name means one function: two functions can't share a name, even with different parameters. A function's name can't be a built-in one (`print`, `format`, `free`...) or a type's.

## Arguments are copies

Each argument is copied into its parameter. A function that changes a parameter changes its copy, not the caller's variable:

```iris run
void bump(i64 n) {
    n += 1
    print("inside: {n}")
}

void main() {
    count := 5
    bump(count)
    print("outside: {count}")
}
```

```output
inside: 6
outside: 5
```

To let a function change the caller's value, pass a pointer to it, with `&`. The function reads and writes through it with `*` (the [Pointers](17-pointers.html) chapter has more):

```iris run
void bump(i64 *n) {
    *n += 1
}

void main() {
    count := 5
    bump(&count)
    print("{count}")
}
```

```output
6
```

A struct is copied too, field by field. A large struct, or one the function should change, is passed as a pointer: a pointer to a struct reaches the fields with a dot, as the value does.

## Defaults

A parameter can have a default value, which is used when the call leaves it out. Parameters with defaults come after the ones without:

```iris run
void connect(str host, u16 port = 80, Duration timeout = 30s) {
    print("{host}:{port}, waiting at most {timeout}")
}

void main() {
    connect("example.com")
    connect("example.com", 8080)
    connect("example.com", 443, 5s)
}
```

```output
example.com:80, waiting at most 30s
example.com:8080, waiting at most 30s
example.com:443, waiting at most 5s
```

A default is a value known when compiling: a number, text, a constant, or an empty slice `[]`.

## Named arguments

Any argument can be given by its parameter's name. This lets a call skip some defaults and give a later one, and makes calls with several values of the same type easier to read:

```iris run
void connect(str host, u16 port = 80, Duration timeout = 30s, bool tls = false) {
    print("{host}:{port} timeout {timeout} tls {tls}")
}

void main() {
    connect("example.com", tls: true)
    connect("example.com", timeout: 5s, port: 8443)
    connect(host: "localhost")
}
```

```output
example.com:80 timeout 30s tls true
example.com:8443 timeout 5s tls false
localhost:80 timeout 30s tls false
```

Named arguments come after the ones given by position, in any order among themselves. A parameter is given once, by position or by name.

## Several values

A function can give several values. Its return type lists their types in parentheses, and `return` gives them separated by commas, also in parentheses:

```iris run
(i64, i64) divmod(i64 a, i64 b) => (a / b, a % b)

(str, bool) firstWord(str text) {
    for i, c in text {
        if c == ' ' {
            return (text[0:i], true)
        }
    }
    return (text, false)
}

void main() {
    q, r := divmod(17, 5)
    print("{q} remainder {r}")
    word, more := firstWord("hello world")
    print("{word} {more}")
    _, last := divmod(20, 6)
    print("{last}")
}
```

```output
3 remainder 2
hello true
2
```

The caller unpacks the values into names, one for each; `_` leaves one out. A function's several values can't be kept together in one variable: they are unpacked where the call is.

## Functions that can fail

A function whose return type starts with `!` can fail: it gives its value or an error. The caller must deal with the error, with `try` or `catch`:

```iris run
enum ParseError { empty, notDigit }

!i64 digit(str s) {
    if s.len == 0 {
        return error(ParseError.empty, "nothing to read")
    }
    if s[0] < '0' || s[0] > '9' {
        return error(ParseError.notDigit, "'{s}' isn't a digit")
    }
    return i64(s[0] - '0')
}

void main() {
    print("{digit("7") catch -1}")
    print("{digit("x") catch -1}")
}
```

```output
7
-1
```

The [Errors](18-errors.html) chapter covers them in full. The result of any other function can be left unused, as with a `print` of nothing:

```iris run
i64 log(str s) {
    print(s)
    return s.len
}

void main() {
    log("the length isn't needed here")
}
```

```output
the length isn't needed here
```
