# Functions

Declaring functions, passing them arguments, giving defaults and names to those arguments, and returning one value or several.

## Declaring a function

A function is written as its return type, its name, its parameters in parentheses, and a block. `void` means it gives nothing back:

```iris run
i64 frameSize(i64 payload, i64 headers) {
    return payload + headers
}

void announce(str addr) {
    print("listening on {addr}")
}

void main() {
    announce("0.0.0.0:8080")
    print("{frameSize(1460, 54)} bytes")
}
```

```output
listening on 0.0.0.0:8080
1514 bytes
```

Parameters next to each other with the same type can share it: `i64 frameSize(i64 payload, headers)` is the same as `i64 frameSize(i64 payload, i64 headers)`.

A function that gives a value must give it on every way out of its block. One that can reach its closing brace without a `return` doesn't compile.

## One expression

A function whose body is one expression can be written with `=>`, leaving out the braces and the `return`:

```iris run
// A 16-bit number in network byte order: its two bytes swapped.
u16 swap16(u16 x) => (x << 8) | (x >> 8)
bool isDigit(u8 c) => c >= '0' && c <= '9'
str kind(i64 status) => if status >= 500 { "a server error" } else if status >= 400 { "a client error" } else { "fine" }
void show(i64 status) => print("{status} is {kind(status)}")

void main() {
    show(503)
    show(200)
    print("{swap16(0x1F90)} {isDigit('7')} {isDigit('x')}")
}
```

```output
503 is a server error
200 is fine
36895 true false
```

For a `void` function, the expression after `=>` is a call.

## Order

Functions can be declared in any order: a function can call one declared further down the file, or in another file of the same module. A function can call itself:

```iris run
import str

// Prints the labels of a DNS name from the top down, the way a resolver walks them.
void labels(str name) {
    head, rest, found := str.cut(name, ".")
    if found {
        labels(rest)
    }
    print(head)
}

void main() {
    labels("api.eu.example.com")
}
```

```output
com
example
eu
api
```

Each name means one function: two functions can't share a name, even with different parameters. A function's name can't be a built-in one (`print`, `format`, `free`...) or a type's.

## Arguments are copies

Each argument is copied into its parameter. A function that changes a parameter changes its copy, not the caller's variable:

```iris run
void record(i64 sent) {
    sent += 1500
    print("inside: {sent}")
}

void main() {
    sent := 3000
    record(sent)
    print("outside: {sent}")
}
```

```output
inside: 4500
outside: 3000
```

To let a function change the caller's value, pass a pointer to it, with `&`. The function reads and writes through it with `*` (the [Pointers](17-pointers.html) chapter has more):

```iris run
void record(i64 *sent) {
    *sent += 1500
}

void main() {
    sent := 3000
    record(&sent)
    print("{sent}")
}
```

```output
4500
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

## Any number of arguments

A last parameter written `T... name` takes the arguments left over, any number of them, and the function sees them as a slice, `T[]`. A slice the caller already has is given by the parameter's name:

```iris run
i64 total(i64... sizes) {
    sum := 0
    for s in sizes {
        sum += s
    }
    return sum
}

void log(str level, str... parts) {
    for p in parts {
        print("{level}: {p}")
    }
}

void main() {
    print("{total()} {total(512)} {total(1460, 1460, 80)}")
    reads := [4096, 1024]
    print("{total(sizes: reads[:])}")
    port := 8080
    log("info", "starting", "port {port}")
}
```

```output
0 512 3000
5120
info: starting
info: port 8080
```

The values are put in an array for the call, so nothing is allocated. A `...` parameter comes last and has no default; with no arguments left over it is an empty slice. As a function value, `total` is an `i64(i64[])`: a call through it gives the slice.

## Several values

A function can give several values. Its return type lists their types in parentheses, and `return` gives them separated by commas, also in parentheses:

```iris run
// Full segments, and the bytes left for a last one.
(i64, i64) segments(i64 size, i64 mss) => (size / mss, size % mss)

// The first word of a request line, and whether more follows.
(str, bool) method(str line) {
    for i, c in line {
        if c == ' ' {
            return (line[0:i], true)
        }
    }
    return (line, false)
}

void main() {
    full, rest := segments(4000, 1460)
    print("{full} full segments and {rest} bytes")
    m, more := method("GET /index.html HTTP/1.1")
    print("{m} {more}")
    _, last := segments(9000, 1460)
    print("{last}")
}
```

```output
2 full segments and 1080 bytes
GET true
240
```

The caller unpacks the values into names, one for each; `_` leaves one out. A function's several values can't be kept together in one variable: they are unpacked where the call is.

## Functions that can fail

A function whose return type starts with `!` can fail: it gives its value or an error. The caller must deal with the error, with `try` or `catch`:

```iris run
enum PortError { empty, notNumber, tooBig }

!u16 parsePort(str s) {
    if s.len == 0 {
        return error(PortError.empty, "no port given")
    }
    i64 n = 0
    for c in s {
        if c < '0' || c > '9' {
            return error(PortError.notNumber, "'{s}' isn't a number")
        }
        n = n * 10 + i64(c - '0')
        if n > 65535 {
            return error(PortError.tooBig, "{s} is above 65535")
        }
    }
    return u16(n)
}

void main() {
    print("{parsePort("8080") catch 0}")
    print("{parsePort("http") catch 0}")
    print("{parsePort("70000") catch 0}")
}
```

```output
8080
0
0
```

The [Errors](18-errors.html) chapter covers them in full. The result of any other function can be left unused, as with a `print` of nothing:

```iris run
// Sends a line and gives how many bytes it took.
i64 send(str line) {
    print(line)
    return line.len
}

void main() {
    send("the byte count isn't needed here")
}
```

```output
the byte count isn't needed here
```
